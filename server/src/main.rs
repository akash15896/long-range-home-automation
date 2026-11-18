//! Automation server for the LoRa home-automation system.
//!
//! A web page and a mobile app switch the nodes' relays, set fan and dimmer levels and move
//! curtains. Every command goes to the ESP32 LoRa master, which keeps a WebSocket open here:
//!
//!   browser <-> /ws/ui        session cookie (+ Origin check), or a Bearer access token (app)
//!   app     --> /api/v1/...    REST, Bearer access token (see api.rs); the page may use its cookie
//!   ESP32   <-> /ws/device    "Authorization: Bearer <AUTOMATION_DEVICE_TOKEN>"
//!
//! People sign in with Google or Apple (allowlisted emails, admin/member roles) or with the
//! emergency password (see auth.rs). Settings: config.rs and deploy/automation.env.example.

mod api;
mod auth;
mod config;
mod db;
mod oidc;

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
        State,
    },
    http::{header, HeaderMap, StatusCode},
    response::{Html, IntoResponse, Redirect, Response},
    routing::{get, post},
    Router,
};
use serde_json::{json, Value};
use tokio::sync::{broadcast, mpsc, oneshot, Mutex};

use crate::{
    auth::{AuthUser, Via},
    config::Config,
};

const DEVICE_PING_EVERY: Duration = Duration::from_secs(20);
const DEVICE_SILENCE_LIMIT: Duration = Duration::from_secs(60);
const CURTAIN_POLL_EVERY: Duration = Duration::from_secs(3);

const INDEX_HTML: &str = include_str!("../static/index.html");

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
pub struct Status {
    master_online: bool,
    led: Option<bool>,
    nodes: BTreeMap<String, Node>,
    // Set when the master connects: its first node list is followed by a status request,
    // so the page learns which nodes are curtains without a manual refresh.
    refresh_on_nodes: bool,
}

pub struct AppState {
    config: Config,
    db: db::Db,
    http: reqwest::Client,
    google_keys: oidc::KeyCache,
    apple_keys: oidc::KeyCache,
    /// Web sign-ins started but not finished yet, by their `state` value.
    pending: Mutex<HashMap<String, auth::PendingFlow>>,
    /// Password login attempts per IP.
    login_limiter: auth::RateLimiter,
    /// App sign-in / refresh calls per IP.
    api_limiter: auth::RateLimiter,
    // Connection id + sender of the ESP32 currently connected. A new connection replaces an old one.
    device: Mutex<Option<(u64, mpsc::UnboundedSender<String>)>>,
    status: Mutex<Status>,
    ui_tx: broadcast::Sender<String>, // to every open page
    next_id: AtomicU64,
    /// REST calls waiting for the master's result, by command id.
    waiters: Mutex<HashMap<u64, oneshot::Sender<Value>>>,
}

#[tokio::main]
async fn main() {
    let config = Config::from_env();
    let db = db::Db::open(&config.db_path).unwrap_or_else(|e| panic!("cannot open database {}: {e}", config.db_path));
    let http = reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .user_agent("automation-server")
        .build()
        .expect("cannot build HTTP client");
    let bind = config.bind.clone();
    println!(
        "sign-in methods: password {}, Google {}, Apple web {}, Apple app {}",
        on_off(config.password.is_some()),
        on_off(config.google.is_some()),
        on_off(config.apple.as_ref().is_some_and(|a| a.services_id.is_some())),
        on_off(config.apple.as_ref().is_some_and(|a| !a.bundle_ids.is_empty())),
    );

    let state = Arc::new(new_state(config, db, http));
    tokio::spawn(poll_moving_curtains(state.clone()));
    tokio::spawn(prune_tokens(state.clone()));

    let app = router(state);
    let listener = tokio::net::TcpListener::bind(&bind).await.expect("cannot listen");
    println!("automation-server listening on {bind}");
    axum::serve(listener, app).await.expect("server error");
}

fn new_state(config: Config, db: db::Db, http: reqwest::Client) -> AppState {
    AppState {
        config,
        db,
        http,
        google_keys: oidc::KeyCache::new(oidc::Provider::Google),
        apple_keys: oidc::KeyCache::new(oidc::Provider::Apple),
        pending: Mutex::new(HashMap::new()),
        login_limiter: auth::RateLimiter::new(10, Duration::from_secs(15 * 60)),
        api_limiter: auth::RateLimiter::new(30, Duration::from_secs(60)),
        device: Mutex::new(None),
        status: Mutex::new(Status::default()),
        ui_tx: broadcast::channel(64).0,
        next_id: AtomicU64::new(1),
        waiters: Mutex::new(HashMap::new()),
    }
}

fn on_off(on: bool) -> &'static str {
    if on { "on" } else { "off" }
}

fn router(state: Arc<AppState>) -> Router {
    Router::new()
        .route("/", get(index))
        .route("/login", get(auth::login_page).post(auth::password_login))
        .route("/logout", post(auth::logout))
        .route("/auth/google/start", get(auth::google_start))
        .route("/auth/google/callback", get(auth::google_callback))
        .route("/auth/apple/start", get(auth::apple_start))
        .route("/auth/apple/callback", post(auth::apple_callback))
        .route("/api/session", get(session_check))
        .route("/api/v1/auth/google", post(auth::app_google))
        .route("/api/v1/auth/apple", post(auth::app_apple))
        .route("/api/v1/auth/refresh", post(auth::app_refresh))
        .route("/api/v1/auth/logout", post(auth::app_logout))
        .route("/api/v1/me", get(api::me))
        .route("/api/v1/state", get(api::state))
        .route("/api/v1/commands", post(api::command))
        .route("/ws/ui", get(ui_ws))
        .route("/ws/device", get(device_ws))
        .with_state(state)
}

/// Deletes expired tokens once an hour.
async fn prune_tokens(st: Arc<AppState>) {
    let mut tick = tokio::time::interval(Duration::from_secs(3600));
    loop {
        tick.tick().await;
        if let Err(e) = st.db.prune() {
            eprintln!("could not prune tokens: {e}");
        }
    }
}

async fn index(State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    match auth::authenticate(&st, &headers) {
        Ok(u) if u.via == Via::Cookie => Html(INDEX_HTML).into_response(),
        _ => Redirect::to("/login").into_response(),
    }
}

async fn session_check(State(st): State<Arc<AppState>>, headers: HeaderMap) -> StatusCode {
    if auth::authenticate(&st, &headers).is_ok() { StatusCode::NO_CONTENT } else { StatusCode::UNAUTHORIZED }
}

// ---------- shared state sent to pages ----------

pub fn snapshot(status: &Status) -> String {
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

// ---------- browser / app WebSocket ----------

async fn ui_ws(ws: WebSocketUpgrade, State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    let user = match auth::authenticate(&st, &headers) {
        Ok(u) => u,
        Err(e) => return e.into_response(),
    };
    // A cookie is sent by the browser automatically, so check that our own page opened the socket.
    if user.via == Via::Cookie && !auth::origin_allowed(&st, &headers) {
        return StatusCode::FORBIDDEN.into_response();
    }
    ws.on_upgrade(move |socket| run_ui(socket, st, user))
}

async fn run_ui(mut socket: WebSocket, st: Arc<AppState>, user: AuthUser) {
    let mut updates = st.ui_tx.subscribe();
    let snap = snapshot(&*st.status.lock().await);
    let hello = json!({ "type": "me", "email": user.user.email, "name": user.user.name, "role": user.role.as_str() });
    if socket.send(Message::Text(hello.to_string())).await.is_err() || socket.send(Message::Text(snap)).await.is_err() {
        return;
    }
    // Re-check the login now and then, so signing out or being removed from the allowlist
    // also closes sockets that are already open.
    let mut recheck = tokio::time::interval(Duration::from_secs(60));
    recheck.tick().await;

    loop {
        tokio::select! {
            msg = socket.recv() => match msg {
                Some(Ok(Message::Text(text))) => {
                    if let Err(e) = handle_ui_command(&st, &user, &text).await {
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
            _ = recheck.tick() => {
                if !still_signed_in(&st, &user) {
                    let _ = socket.send(Message::Text(log_message("Signed out.", "error"))).await;
                    break;
                }
            }
        }
    }
}

/// True while the login behind an open socket is still valid and allowed. App sockets stay
/// open past the 15-minute access token as long as the app's sign-in (its family) is active.
fn still_signed_in(st: &AppState, user: &AuthUser) -> bool {
    st.config.role_for(&user.user.email).is_some() && st.db.family_active(&user.family)
}

async fn handle_ui_command(st: &AppState, user: &AuthUser, text: &str) -> Result<(), String> {
    let cmd: api::Command = serde_json::from_str(text).map_err(|_| "The page sent an unreadable command.".to_string())?;
    // Use the current allowlist role, in case it changed while the socket was open.
    let role = st.config.role_for(&user.user.email).ok_or("This account is no longer allowed.")?;
    api::dispatch(st, &user.user.email, role, &cmd, false).await.map(|_| ()).map_err(|e| e.message())
}

// ---------- ESP32 WebSocket ----------

async fn device_ws(ws: WebSocketUpgrade, State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    let authorized = headers
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .is_some_and(|token| auth::constant_time_eq(token.as_bytes(), st.config.device_token.as_bytes()));
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
                // A REST call may be waiting for exactly this result.
                if let Some(id) = v["id"].as_u64() {
                    if let Some(waiter) = st.waiters.lock().await.remove(&id) {
                        let _ = waiter.send(v.clone());
                    }
                }
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
