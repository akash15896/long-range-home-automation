//! Automation server for automation.akashmahor.me.
//!
//! A password-protected web page switches the ESP32 LoRa master's LED and the Nano nodes'
//! relays, sets the level of a node's fan regulator (stepper-driven knob) and moves curtain nodes. Browsers and the ESP32 each keep a WebSocket open to this server:
//!
//!   browser <-> /ws/ui      needs the login cookie
//!   ESP32   <-> /ws/device  needs "Authorization: Bearer <AUTOMATION_DEVICE_TOKEN>"
//!
//! Browser commands are checked, given an id and forwarded to the ESP32. The ESP32 answers
//! with results, which update the shared state that every open page sees.
//!
//! Settings (environment variables, normally from /etc/automation/automation.env):
//!   AUTOMATION_PASSWORD      login password for the web page
//!   AUTOMATION_DEVICE_TOKEN  secret the ESP32 sends when connecting
//!   AUTOMATION_ORIGIN        public address, e.g. https://automation.akashmahor.me
//!   AUTOMATION_BIND          listen address (default 127.0.0.1:3000; cloudflared connects here)

use std::{
    collections::{BTreeMap, HashMap},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};

use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        Form, Query, State,
    },
    http::{header, HeaderMap, StatusCode},
    response::{Html, IntoResponse, Redirect, Response},
    routing::{get, post},
    Router,
};
use rand::RngCore;
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::{broadcast, mpsc, Mutex};

const SESSION_COOKIE: &str = "automation_session";
const SESSION_LIFETIME: Duration = Duration::from_secs(30 * 24 * 3600);
const DEVICE_PING_EVERY: Duration = Duration::from_secs(20);
const DEVICE_SILENCE_LIMIT: Duration = Duration::from_secs(60);
const CURTAIN_POLL_EVERY: Duration = Duration::from_secs(3);

const INDEX_HTML: &str = include_str!("../static/index.html");
const LOGIN_HTML: &str = include_str!("../static/login.html");

#[derive(Default)]
struct Node {
    relays: [bool; 4],
    fan: Option<u8>, // fan regulator level 0-100; None = node has no fan
    fan_homing: bool,
    // Curtain state as the master reports it: position (0-100 or null), moving, learning,
    // learned, open, closed, fault. None = node has no curtain.
    curtain: Option<Value>,
    // Dimmer state as the master reports it: level (0-100), mains, fading. None = no dimmer.
    dimmer: Option<Value>,
    rssi: Option<i64>,
    snr: Option<f64>,
    reachable: bool,
}

#[derive(Default)]
struct Status {
    master_online: bool,
    led: Option<bool>,
    nodes: BTreeMap<String, Node>,
    // Set when the master connects: its first node list is followed by a status request,
    // so the page learns which nodes are curtains without a manual refresh.
    refresh_on_nodes: bool,
}

struct AppState {
    password: String,
    device_token: String,
    allowed_origin: String,
    sessions: Mutex<HashMap<String, Instant>>, // session token -> expiry
    // Connection id + sender of the ESP32 currently connected. A new connection replaces an old one.
    device: Mutex<Option<(u64, mpsc::UnboundedSender<String>)>>,
    status: Mutex<Status>,
    ui_tx: broadcast::Sender<String>, // to every open page
    next_id: AtomicU64,
}

#[tokio::main]
async fn main() {
    let env = |name: &str| std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"));
    let bind = std::env::var("AUTOMATION_BIND").unwrap_or_else(|_| "127.0.0.1:3000".into());

    let state = Arc::new(AppState {
        password: env("AUTOMATION_PASSWORD"),
        device_token: env("AUTOMATION_DEVICE_TOKEN"),
        allowed_origin: env("AUTOMATION_ORIGIN"),
        sessions: Mutex::new(HashMap::new()),
        device: Mutex::new(None),
        status: Mutex::new(Status::default()),
        ui_tx: broadcast::channel(64).0,
        next_id: AtomicU64::new(1),
    });

    tokio::spawn(poll_moving_curtains(state.clone()));

    let app = Router::new()
        .route("/", get(index))
        .route("/login", get(login_page).post(login))
        .route("/logout", post(logout))
        .route("/api/session", get(session_check))
        .route("/ws/ui", get(ui_ws))
        .route("/ws/device", get(device_ws))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(&bind).await.expect("cannot listen");
    println!("automation-server listening on {bind}");
    axum::serve(listener, app).await.expect("server error");
}

// ---------- login ----------

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

fn session_token(headers: &HeaderMap) -> Option<String> {
    headers
        .get_all(header::COOKIE)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .flat_map(|v| v.split(';'))
        .find_map(|c| c.trim().strip_prefix(&format!("{SESSION_COOKIE}=")).map(String::from))
}

async fn logged_in(st: &AppState, headers: &HeaderMap) -> bool {
    let Some(token) = session_token(headers) else { return false };
    let sessions = st.sessions.lock().await;
    sessions.get(&token).is_some_and(|expiry| *expiry > Instant::now())
}

async fn index(State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if logged_in(&st, &headers).await {
        Html(INDEX_HTML).into_response()
    } else {
        Redirect::to("/login").into_response()
    }
}

async fn session_check(State(st): State<Arc<AppState>>, headers: HeaderMap) -> StatusCode {
    if logged_in(&st, &headers).await { StatusCode::NO_CONTENT } else { StatusCode::UNAUTHORIZED }
}

#[derive(Deserialize)]
struct LoginQuery {
    e: Option<String>,
}

async fn login_page(Query(q): Query<LoginQuery>) -> Html<String> {
    let error = if q.e.is_some() { "Wrong password. Try again." } else { "" };
    Html(LOGIN_HTML.replace("{{ERROR}}", error))
}

#[derive(Deserialize)]
struct LoginForm {
    password: String,
}

async fn login(State(st): State<Arc<AppState>>, Form(form): Form<LoginForm>) -> Response {
    if !constant_time_eq(form.password.as_bytes(), st.password.as_bytes()) {
        tokio::time::sleep(Duration::from_secs(1)).await; // slows down password guessing
        return Redirect::to("/login?e=1").into_response();
    }

    let mut bytes = [0u8; 32];
    rand::thread_rng().fill_bytes(&mut bytes);
    let token: String = bytes.iter().map(|b| format!("{b:02x}")).collect();

    let mut sessions = st.sessions.lock().await;
    let now = Instant::now();
    sessions.retain(|_, expiry| *expiry > now);
    sessions.insert(token.clone(), now + SESSION_LIFETIME);

    let cookie = format!(
        "{SESSION_COOKIE}={token}; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age={}",
        SESSION_LIFETIME.as_secs()
    );
    ([(header::SET_COOKIE, cookie)], Redirect::to("/")).into_response()
}

async fn logout(State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if let Some(token) = session_token(&headers) {
        st.sessions.lock().await.remove(&token);
    }
    let cookie = format!("{SESSION_COOKIE}=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0");
    ([(header::SET_COOKIE, cookie)], Redirect::to("/login")).into_response()
}

// ---------- shared state sent to pages ----------

fn snapshot(status: &Status) -> String {
    let nodes: Vec<Value> = status
        .nodes
        .iter()
        .map(|(id, n)| {
            json!({
                "id": id, "relays": n.relays, "fan": n.fan, "fan_homing": n.fan_homing,
                "curtain": n.curtain, "dimmer": n.dimmer, "rssi": n.rssi, "snr": n.snr, "reachable": n.reachable
            })
        })
        .collect();
    json!({ "type": "state", "master_online": status.master_online, "led": status.led, "nodes": nodes })
        .to_string()
}

fn log_message(text: &str, level: &str) -> String {
    json!({ "type": "log", "level": level, "text": text }).to_string()
}

async fn broadcast_state(st: &AppState, log: Option<(String, &str)>) {
    let snap = snapshot(&*st.status.lock().await);
    let _ = st.ui_tx.send(snap); // fails only when no page is open
    if let Some((text, level)) = log {
        let _ = st.ui_tx.send(log_message(&text, level));
    }
}

// ---------- browser WebSocket ----------

// Blocks other websites from opening this WebSocket with the user's cookie.
fn origin_allowed(st: &AppState, headers: &HeaderMap) -> bool {
    let Some(origin) = headers.get(header::ORIGIN).and_then(|v| v.to_str().ok()) else {
        return false;
    };
    let host = headers.get(header::HOST).and_then(|v| v.to_str().ok()).unwrap_or("");
    origin == st.allowed_origin || origin.split("://").nth(1) == Some(host)
}

async fn ui_ws(ws: WebSocketUpgrade, State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if !logged_in(&st, &headers).await || !origin_allowed(&st, &headers) {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    ws.on_upgrade(move |socket| run_ui(socket, st))
}

async fn run_ui(mut socket: WebSocket, st: Arc<AppState>) {
    let mut updates = st.ui_tx.subscribe();
    let snap = snapshot(&*st.status.lock().await);
    if socket.send(Message::Text(snap)).await.is_err() {
        return;
    }

    loop {
        tokio::select! {
            msg = socket.recv() => match msg {
                Some(Ok(Message::Text(text))) => {
                    if let Err(e) = forward_ui_command(&st, &text).await {
                        if socket.send(Message::Text(log_message(&e, "error"))).await.is_err() {
                            break;
                        }
                    }
                }
                Some(Ok(_)) => {}
                _ => break,
            },
            update = updates.recv() => match update {
                Ok(text) => {
                    if socket.send(Message::Text(text)).await.is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Lagged(_)) => {
                    let snap = snapshot(&*st.status.lock().await);
                    if socket.send(Message::Text(snap)).await.is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Closed) => break,
            },
        }
    }
}

#[derive(Deserialize)]
struct UiCommand {
    action: String,
    node: Option<String>,
    relay: Option<u8>,
    state: Option<String>,
    level: Option<u8>,
}

// "all", or a node ID of 1-4 hex digits.
fn valid_node(node: Option<String>) -> Result<String, String> {
    let node = node.ok_or("Choose a node.")?.to_uppercase();
    let is_id = (1..=4).contains(&node.len()) && node.chars().all(|c| c.is_ascii_hexdigit());
    if node == "ALL" { Ok("all".into()) } else if is_id { Ok(node) } else { Err(format!("Unknown node \"{node}\".")) }
}

fn valid_state(state: Option<String>, allowed: &[&str]) -> Result<String, String> {
    let state = state.unwrap_or_default();
    if allowed.contains(&state.as_str()) { Ok(state) } else { Err(format!("Unknown state \"{state}\".")) }
}

// Checks a browser command, gives it an id and sends it to the ESP32.
async fn forward_ui_command(st: &AppState, text: &str) -> Result<(), String> {
    let cmd: UiCommand = serde_json::from_str(text).map_err(|_| "The page sent an unreadable command.")?;
    let id = st.next_id.fetch_add(1, Ordering::Relaxed);

    let out = match cmd.action.as_str() {
        "led" => {
            let state = valid_state(cmd.state, &["on", "off"])?;
            json!({ "type": "command", "id": id, "action": "led", "state": state })
        }
        "relay" => {
            let node = valid_node(cmd.node)?;
            let relay = cmd.relay.filter(|r| *r <= 4).ok_or("Relay must be 1-4, or 0 for all.")?;
            let state = valid_state(cmd.state, &["on", "off", "toggle"])?;
            json!({ "type": "command", "id": id, "action": "relay", "node": node, "relay": relay, "state": state })
        }
        "fan" => {
            let node = valid_node(cmd.node)?;
            if node == "all" {
                return Err("Choose one node for the fan.".into());
            }
            if cmd.state.as_deref() == Some("home") {
                json!({ "type": "command", "id": id, "action": "fan", "node": node, "state": "home" })
            } else {
                let level = cmd.level.filter(|l| *l <= 100).ok_or("Fan level must be 0-100.")?;
                json!({ "type": "command", "id": id, "action": "fan", "node": node, "state": "set", "level": level })
            }
        }
        "curtain" => {
            let node = valid_node(cmd.node)?;
            let state = valid_state(cmd.state, &["open", "close", "stop", "learn", "goto"])?;
            if state == "goto" {
                let level = cmd.level.filter(|l| *l <= 100).ok_or("Curtain position must be 0-100.")?;
                json!({ "type": "command", "id": id, "action": "curtain", "node": node, "state": state, "level": level })
            } else {
                json!({ "type": "command", "id": id, "action": "curtain", "node": node, "state": state })
            }
        }
        "dim" => {
            let node = valid_node(cmd.node)?;
            let state = valid_state(cmd.state, &["set", "next", "on", "off"])?;
            if state == "set" {
                let level = cmd.level.filter(|l| *l <= 100).ok_or("Dimmer level must be 0-100.")?;
                json!({ "type": "command", "id": id, "action": "dim", "node": node, "state": state, "level": level })
            } else {
                json!({ "type": "command", "id": id, "action": "dim", "node": node, "state": state })
            }
        }
        "status" => {
            let node = valid_node(cmd.node)?;
            json!({ "type": "command", "id": id, "action": "status", "node": node })
        }
        "scan" => json!({ "type": "command", "id": id, "action": "scan" }),
        other => return Err(format!("Unknown action \"{other}\".")),
    };

    let device = st.device.lock().await;
    let sent = device.as_ref().is_some_and(|(_, tx)| tx.send(out.to_string()).is_ok());
    if sent { Ok(()) } else { Err("The ESP32 master is offline, so the command was not sent.".into()) }
}

// ---------- ESP32 WebSocket ----------

async fn device_ws(ws: WebSocketUpgrade, State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    let authorized = headers
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .is_some_and(|token| constant_time_eq(token.as_bytes(), st.device_token.as_bytes()));
    if !authorized {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    ws.on_upgrade(move |socket| run_device(socket, st))
}

async fn run_device(mut socket: WebSocket, st: Arc<AppState>) {
    let (tx, mut commands) = mpsc::unbounded_channel::<String>();
    let conn_id = st.next_id.fetch_add(1, Ordering::Relaxed);
    *st.device.lock().await = Some((conn_id, tx));
    {
        let mut status = st.status.lock().await;
        status.master_online = true;
        status.refresh_on_nodes = true;
    }
    broadcast_state(&st, Some(("ESP32 master connected.".into(), "info"))).await;

    let mut ping = tokio::time::interval(DEVICE_PING_EVERY);
    let mut last_heard = Instant::now();

    loop {
        tokio::select! {
            msg = socket.recv() => match msg {
                Some(Ok(Message::Text(text))) => {
                    last_heard = Instant::now();
                    handle_device_message(&st, &text).await;
                }
                Some(Ok(_)) => last_heard = Instant::now(), // pong, ping or binary
                _ => break,
            },
            // None means a newer ESP32 connection replaced this one.
            command = commands.recv() => match command {
                Some(text) => {
                    if socket.send(Message::Text(text)).await.is_err() {
                        break;
                    }
                }
                None => break,
            },
            _ = ping.tick() => {
                if last_heard.elapsed() > DEVICE_SILENCE_LIMIT {
                    break;
                }
                if socket.send(Message::Ping(Vec::new())).await.is_err() {
                    break;
                }
            }
        }
    }

    // Mark offline only if no newer connection has taken over.
    let mut device = st.device.lock().await;
    if device.as_ref().map(|(id, _)| *id) == Some(conn_id) {
        *device = None;
        drop(device);
        st.status.lock().await.master_online = false;
        broadcast_state(&st, Some(("ESP32 master disconnected.".into(), "error"))).await;
    }
}

fn relay_summary(relays: &[bool; 4]) -> String {
    relays
        .iter()
        .enumerate()
        .map(|(i, on)| format!("{}={}", i + 1, if *on { "ON" } else { "OFF" }))
        .collect::<Vec<_>>()
        .join(" ")
}

fn curtain_summary(c: &Value) -> String {
    let mut text = match c["position"].as_u64() {
        Some(p) => format!("{p}% open"),
        None => "position unknown".into(),
    };
    if c["learning"] == true {
        text += ", learning travel";
    } else if c["moving"] == true {
        text += ", moving";
    }
    if c["learned"] == false {
        text += ", travel not learned yet";
    }
    if c["fault"] == true {
        text += ", FAULT";
    }
    text
}

fn dimmer_summary(d: &Value) -> String {
    let mut text = format!("{}%", d["level"].as_u64().unwrap_or(0));
    if d["fading"] == true {
        text += " (fading)";
    }
    if d["mains"] == false {
        text += ", no mains at the dimmer";
    }
    text
}

// While a curtain moves or a dimmer fades, asks the master for its status every few seconds
// so open pages see the change and see it stop.
async fn poll_moving_curtains(st: Arc<AppState>) {
    let mut tick = tokio::time::interval(CURTAIN_POLL_EVERY);
    loop {
        tick.tick().await;
        let moving: Vec<String> = {
            let status = st.status.lock().await;
            if !status.master_online {
                continue;
            }
            status
                .nodes
                .iter()
                .filter(|(_, n)| {
                    n.reachable
                        && (n.curtain.as_ref().is_some_and(|c| c["moving"] == true || c["learning"] == true)
                            || n.dimmer.as_ref().is_some_and(|d| d["fading"] == true))
                })
                .map(|(id, _)| id.clone())
                .collect()
        };
        let device = st.device.lock().await;
        let Some((_, tx)) = device.as_ref() else { continue };
        for node in moving {
            let id = st.next_id.fetch_add(1, Ordering::Relaxed);
            let _ = tx.send(json!({ "type": "command", "id": id, "action": "status", "node": node }).to_string());
        }
    }
}

async fn handle_device_message(st: &AppState, text: &str) {
    let Ok(v) = serde_json::from_str::<Value>(text) else { return };
    let mut log: Option<(String, &str)> = None;
    let mut refresh = false;
    {
        let mut status = st.status.lock().await;
        match v["type"].as_str() {
            Some("hello") => {
                status.led = v["led"].as_bool();
            }
            Some("nodes") => {
                let mut nodes = BTreeMap::new();
                for n in v["list"].as_array().into_iter().flatten() {
                    let Some(id) = n["id"].as_str() else { continue };
                    let old = status.nodes.remove(id).unwrap_or_default();
                    nodes.insert(id.to_string(), Node { rssi: n["rssi"].as_i64(), reachable: true, ..old });
                }
                status.nodes = nodes;
                refresh = std::mem::take(&mut status.refresh_on_nodes) && !status.nodes.is_empty();
                log =Some((format!("Scan finished: {} node(s) found.", status.nodes.len()), "info"));
            }
            Some("result") => {
                let ok = v["ok"].as_bool().unwrap_or(false);
                if v["action"] == "led" {
                    if ok {
                        status.led = v["led"].as_bool();
                    }
                    let state = if status.led == Some(true) { "ON" } else { "OFF" };
                    log = Some((format!("ESP32 LED is {state}."), "info"));
                } else if let Some(id) = v["node"].as_str() {
                    let node = status.nodes.entry(id.to_string()).or_default();
                    let reason = v["error"].as_str().unwrap_or("did not answer");
                    // A node that answered with an error (e.g. "has no fan") is still reachable.
                    node.reachable = ok || reason != "did not answer";
                    // Curtain nodes report their state even when they refuse a command.
                    if v["curtain"].is_object() {
                        node.curtain = Some(v["curtain"].clone());
                    } else if ok {
                        node.curtain = None;
                    }
                    if v["dimmer"].is_object() {
                        node.dimmer = Some(v["dimmer"].clone());
                    } else if ok {
                        node.dimmer = None;
                    }
                    if ok && node.curtain.is_some() {
                        node.rssi = v["rssi"].as_i64();
                        node.snr = v["snr"].as_f64();
                        log = Some((format!("Node {id}: curtain {}", curtain_summary(&v["curtain"])), "info"));
                    } else if ok {
                        for (i, b) in v["relays"].as_array().into_iter().flatten().take(4).enumerate() {
                            node.relays[i] = b.as_bool().unwrap_or(false);
                        }
                        node.rssi = v["rssi"].as_i64();
                        node.snr = v["snr"].as_f64();
                        node.fan = v["fan"]["level"].as_u64().map(|l| l.min(100) as u8);
                        node.fan_homing = v["fan"]["homing"].as_bool().unwrap_or(false);
                        let mut text = match &node.dimmer {
                            Some(d) => format!("Node {id}: light {}", dimmer_summary(d)),
                            None => format!("Node {id}: {}", relay_summary(&node.relays)),
                        };
                        if let Some(level) = node.fan {
                            text += &format!(", fan {level}%");
                            if node.fan_homing {
                                text += " (finding OFF first)";
                            }
                        }
                        log = Some((text, "info"));
                    } else {
                        log = Some((format!("Node {id}: {reason}."), "error"));
                    }
                } else if let Some(err) = v["error"].as_str() {
                    log = Some((format!("Master: {err}"), "error"));
                }
            }
            Some("log") => {
                log = v["text"].as_str().map(|t| (t.to_string(), "info"));
            }
            _ => {}
        }
    }
    broadcast_state(st, log).await;
    if refresh {
        let id = st.next_id.fetch_add(1, Ordering::Relaxed);
        if let Some((_, tx)) = st.device.lock().await.as_ref() {
            let _ = tx.send(json!({ "type": "command", "id": id, "action": "status", "node": "all" }).to_string());
        }
    }
}
