//! REST API (v1) and the command checks shared with the browser WebSocket.
//!
//!   GET  /api/v1/me         who am I: email, name, role
//!   GET  /api/v1/state      master online?, LED, every node (same JSON the page gets)
//!   POST /api/v1/commands   run a command; waits for the master's LoRa result (up to 15 s)
//!
//! Commands use the same JSON as the page's WebSocket, e.g.
//!   {"action":"relay","node":"E26D","relay":1,"state":"toggle"}
//!   {"action":"curtain","node":"E26D","state":"goto","level":50}
//! Commands for node "all", and "scan", answer 202 at once: watch /api/v1/state or the WebSocket.

use std::{sync::Arc, sync::atomic::Ordering, time::Duration};

use axum::{
    extract::State,
    http::{header, StatusCode},
    response::{IntoResponse, Response},
    Json,
};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::oneshot;

use crate::{
    auth::{user_json, ApiError, AuthUser},
    config::Role,
    snapshot, AppState,
};

const RESULT_TIMEOUT: Duration = Duration::from_secs(15);

#[derive(Deserialize, Debug, Default)]
pub struct Command {
    pub action: String,
    pub node: Option<String>,
    pub relay: Option<u8>,
    pub state: Option<String>,
    pub level: Option<u8>,
}

// "all", or a node ID of 1-4 hex digits.
fn valid_node(node: Option<&str>) -> Result<String, String> {
    let node = node.ok_or("Choose a node.")?.to_uppercase();
    let is_id = (1..=4).contains(&node.len()) && node.chars().all(|c| c.is_ascii_hexdigit());
    if node == "ALL" {
        Ok("all".into())
    } else if is_id {
        Ok(node)
    } else {
        Err(format!("Unknown node \"{node}\"."))
    }
}

fn valid_state(state: Option<&str>, allowed: &[&str]) -> Result<String, String> {
    let state = state.unwrap_or_default();
    if allowed.contains(&state) { Ok(state.to_string()) } else { Err(format!("Unknown state \"{state}\".")) }
}

/// The role a command needs. Setup and diagnostics are admin-only.
pub fn required_role(cmd: &Command) -> Role {
    let state = cmd.state.as_deref();
    match cmd.action.as_str() {
        "led" | "scan" => Role::Admin,
        "curtain" if state == Some("learn") => Role::Admin,
        "fan" if state == Some("home") => Role::Admin,
        _ => Role::Member,
    }
}

/// Checks a command and turns it into the message the ESP32 master understands.
pub fn build(cmd: &Command, id: u64) -> Result<Value, String> {
    let out = match cmd.action.as_str() {
        "led" => {
            let state = valid_state(cmd.state.as_deref(), &["on", "off"])?;
            json!({ "type": "command", "id": id, "action": "led", "state": state })
        }
        "relay" => {
            let node = valid_node(cmd.node.as_deref())?;
            let relay = cmd.relay.filter(|r| *r <= 4).ok_or("Relay must be 1-4, or 0 for all.")?;
            let state = valid_state(cmd.state.as_deref(), &["on", "off", "toggle"])?;
            json!({ "type": "command", "id": id, "action": "relay", "node": node, "relay": relay, "state": state })
        }
        "fan" => {
            let node = valid_node(cmd.node.as_deref())?;
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
            let node = valid_node(cmd.node.as_deref())?;
            let state = valid_state(cmd.state.as_deref(), &["open", "close", "stop", "learn", "goto"])?;
            if state == "goto" {
                let level = cmd.level.filter(|l| *l <= 100).ok_or("Curtain position must be 0-100.")?;
                json!({ "type": "command", "id": id, "action": "curtain", "node": node, "state": state, "level": level })
            } else {
                json!({ "type": "command", "id": id, "action": "curtain", "node": node, "state": state })
            }
        }
        "dim" => {
            let node = valid_node(cmd.node.as_deref())?;
            let state = valid_state(cmd.state.as_deref(), &["set", "next", "on", "off"])?;
            if state == "set" {
                let level = cmd.level.filter(|l| *l <= 100).ok_or("Dimmer level must be 0-100.")?;
                json!({ "type": "command", "id": id, "action": "dim", "node": node, "state": state, "level": level })
            } else {
                json!({ "type": "command", "id": id, "action": "dim", "node": node, "state": state })
            }
        }
        "status" => {
            let node = valid_node(cmd.node.as_deref())?;
            json!({ "type": "command", "id": id, "action": "status", "node": node })
        }
        "scan" => json!({ "type": "command", "id": id, "action": "scan" }),
        other => return Err(format!("Unknown action \"{other}\".")),
    };
    Ok(out)
}

/// Problems with a command, as the HTTP status they map to.
pub enum CommandError {
    Forbidden(String),
    Invalid(String),
    Offline,
}

impl CommandError {
    pub fn message(&self) -> String {
        match self {
            CommandError::Forbidden(m) | CommandError::Invalid(m) => m.clone(),
            CommandError::Offline => "The ESP32 master is offline, so the command was not sent.".into(),
        }
    }
}

impl From<CommandError> for ApiError {
    fn from(e: CommandError) -> Self {
        let status = match e {
            CommandError::Forbidden(_) => StatusCode::FORBIDDEN,
            CommandError::Invalid(_) => StatusCode::BAD_REQUEST,
            CommandError::Offline => StatusCode::SERVICE_UNAVAILABLE,
        };
        ApiError(status, e.message())
    }
}

/// Checks permission and the command, then sends it to the master. Returns the command id and
/// the receiver for its result when `wait` is set.
pub async fn dispatch(
    st: &AppState,
    who: &str,
    role: Role,
    cmd: &Command,
    wait: bool,
) -> Result<(u64, Option<oneshot::Receiver<Value>>), CommandError> {
    if required_role(cmd) == Role::Admin && role != Role::Admin {
        return Err(CommandError::Forbidden(format!("Only an admin can run \"{}\" commands like this one.", cmd.action)));
    }
    let id = st.next_id.fetch_add(1, Ordering::Relaxed);
    let out = build(cmd, id).map_err(CommandError::Invalid)?;

    let rx = if wait {
        let (tx, rx) = oneshot::channel();
        st.waiters.lock().await.insert(id, tx);
        Some(rx)
    } else {
        None
    };
    let sent = {
        let device = st.device.lock().await;
        device.as_ref().is_some_and(|(_, tx)| tx.send(out.to_string()).is_ok())
    };
    if !sent {
        st.waiters.lock().await.remove(&id);
        return Err(CommandError::Offline);
    }
    println!("command {id} from {who}: {out}");
    Ok((id, rx))
}

// ---------- handlers ----------

pub async fn me(user: AuthUser) -> Json<Value> {
    Json(user_json(&user.user, user.role))
}

pub async fn state(State(st): State<Arc<AppState>>, _user: AuthUser) -> Response {
    let snap = snapshot(&*st.status.lock().await);
    ([(header::CONTENT_TYPE, "application/json")], snap).into_response()
}

pub async fn command(State(st): State<Arc<AppState>>, user: AuthUser, Json(cmd): Json<Command>) -> Result<Response, ApiError> {
    let fan_out = cmd.action == "scan" || cmd.node.as_deref().is_some_and(|n| n.eq_ignore_ascii_case("all"));
    let (id, rx) = dispatch(&st, &user.user.email, user.role, &cmd, !fan_out).await?;
    let Some(rx) = rx else {
        return Ok((StatusCode::ACCEPTED, Json(json!({ "id": id, "status": "sent" }))).into_response());
    };
    match tokio::time::timeout(RESULT_TIMEOUT, rx).await {
        Ok(Ok(result)) => {
            let status = if result["ok"] == true { StatusCode::OK } else { StatusCode::BAD_GATEWAY };
            Ok((status, Json(json!({ "id": id, "result": result }))).into_response())
        }
        _ => {
            st.waiters.lock().await.remove(&id);
            Ok((StatusCode::GATEWAY_TIMEOUT, Json(json!({ "id": id, "error": "No answer from the master in time." }))).into_response())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cmd(action: &str, node: Option<&str>, state: Option<&str>, level: Option<u8>) -> Command {
        Command { action: action.into(), node: node.map(Into::into), state: state.map(Into::into), level, relay: None }
    }

    #[test]
    fn setup_commands_are_admin_only() {
        assert_eq!(required_role(&cmd("scan", None, None, None)), Role::Admin);
        assert_eq!(required_role(&cmd("led", None, Some("on"), None)), Role::Admin);
        assert_eq!(required_role(&cmd("curtain", Some("E26D"), Some("learn"), None)), Role::Admin);
        assert_eq!(required_role(&cmd("fan", Some("E26D"), Some("home"), None)), Role::Admin);
        assert_eq!(required_role(&cmd("curtain", Some("E26D"), Some("goto"), Some(50))), Role::Member);
        assert_eq!(required_role(&cmd("dim", Some("E29E"), Some("next"), None)), Role::Member);
    }

    fn test_state() -> AppState {
        crate::new_state(crate::config::test_config(), crate::db::Db::open(":memory:").unwrap(), reqwest::Client::new())
    }

    #[tokio::test]
    async fn members_cannot_run_setup_commands() {
        let st = test_state();
        let scan = cmd("scan", None, None, None);
        assert!(matches!(dispatch(&st, "kid", Role::Member, &scan, false).await, Err(CommandError::Forbidden(_))));
        let learn = cmd("curtain", Some("E26D"), Some("learn"), None);
        assert!(matches!(dispatch(&st, "kid", Role::Member, &learn, false).await, Err(CommandError::Forbidden(_))));
        // Allowed for a member, but no master is connected in this test.
        let open = cmd("curtain", Some("E26D"), Some("open"), None);
        assert!(matches!(dispatch(&st, "kid", Role::Member, &open, true).await, Err(CommandError::Offline)));
        assert!(st.waiters.lock().await.is_empty(), "a refused command must not leave a waiter behind");
        assert!(matches!(dispatch(&st, "boss", Role::Admin, &scan, false).await, Err(CommandError::Offline)));
    }

    #[test]
    fn commands_are_validated() {
        let ok = build(&cmd("curtain", Some("e26d"), Some("goto"), Some(40)), 7).unwrap();
        assert_eq!(ok["node"], "E26D");
        assert_eq!(ok["level"], 40);
        assert_eq!(ok["id"], 7);
        assert!(build(&cmd("curtain", Some("E26D"), Some("goto"), Some(101)), 1).is_err());
        assert!(build(&cmd("curtain", Some("E26D"), Some("explode"), None), 1).is_err());
        assert!(build(&cmd("relay", Some("XYZ!"), Some("on"), None), 1).is_err());
        assert!(build(&cmd("fan", Some("all"), None, Some(10)), 1).is_err());
        assert!(build(&cmd("reboot", None, None, None), 1).is_err());
    }
}
