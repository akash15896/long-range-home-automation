//! Who is calling, and the sign-in / sign-out routes.
//!
//! Two ways to prove who you are on every request:
//!   - web page: the `automation_session` cookie (HttpOnly, Secure, SameSite=Strict). Requests
//!     that change something must also come from our own Origin (CSRF protection).
//!   - mobile app: `Authorization: Bearer <access token>` from /api/v1/auth/{google,apple,refresh}.
//!
//! The role is looked up from the allowlist on every request, so removing an email from
//! AUTOMATION_ADMINS/AUTOMATION_MEMBERS (and restarting) locks that person out at once.

use std::{
    collections::HashMap,
    sync::Arc,
    time::{Duration, Instant},
};

use axum::{
    async_trait,
    extract::{FromRequestParts, Query, State},
    http::{header, request::Parts, HeaderMap, Method, StatusCode},
    response::{Html, IntoResponse, Redirect, Response},
    Form, Json,
};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    config::{Role, PASSWORD_ACCOUNT},
    db::{self, new_token, Refresh, TokenKind, User},
    oidc::{self, NonceCheck, Provider},
    AppState,
};

pub const SESSION_COOKIE: &str = "automation_session";
const FLOW_COOKIE: &str = "automation_signin";
const FLOW_LIFETIME: Duration = Duration::from_secs(10 * 60);
const MAX_PENDING_FLOWS: usize = 500;
const LOGIN_PAGE: &str = include_str!("../static/login.html");

// ---------- errors ----------

/// A JSON error: `{"error": "..."}` with an HTTP status.
#[derive(Debug)]
pub struct ApiError(pub StatusCode, pub String);

impl ApiError {
    pub fn new(status: StatusCode, msg: impl Into<String>) -> Self {
        ApiError(status, msg.into())
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.0, Json(json!({ "error": self.1 }))).into_response()
    }
}

fn internal(e: impl std::fmt::Display) -> ApiError {
    eprintln!("internal error: {e}");
    ApiError::new(StatusCode::INTERNAL_SERVER_ERROR, "Something went wrong on the server.")
}

// ---------- rate limiting ----------

/// Fixed-window counter per key (client IP). Small and in memory: this is a home server.
pub struct RateLimiter {
    window: Duration,
    max: u32,
    hits: std::sync::Mutex<HashMap<String, (Instant, u32)>>,
}

impl RateLimiter {
    pub fn new(max: u32, window: Duration) -> Self {
        RateLimiter { window, max, hits: std::sync::Mutex::new(HashMap::new()) }
    }

    /// Counts one attempt; false when the key has used up its attempts for this window.
    pub fn allow(&self, key: &str) -> bool {
        let mut hits = self.hits.lock().unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        if hits.len() > 10_000 {
            hits.retain(|_, (start, _)| now.duration_since(*start) < self.window);
        }
        let entry = hits.entry(key.to_string()).or_insert((now, 0));
        if now.duration_since(entry.0) >= self.window {
            *entry = (now, 0);
        }
        entry.1 += 1;
        entry.1 <= self.max
    }
}

/// The caller's IP. The server only listens on localhost behind the Cloudflare tunnel, so
/// Cloudflare's header can be trusted.
pub fn client_ip(headers: &HeaderMap) -> String {
    headers
        .get("cf-connecting-ip")
        .or_else(|| headers.get("x-forwarded-for"))
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.split(',').next())
        .map(|v| v.trim().to_string())
        .unwrap_or_else(|| "local".into())
}

// ---------- who is calling ----------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Via {
    Cookie,
    Bearer,
}

/// A signed-in caller who is (still) on the allowlist.
#[derive(Clone, Debug)]
pub struct AuthUser {
    pub user: User,
    pub role: Role,
    /// The login this request belongs to (one web session or one app sign-in).
    pub family: String,
    pub via: Via,
}

fn cookie(headers: &HeaderMap, name: &str) -> Option<String> {
    headers
        .get_all(header::COOKIE)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .flat_map(|v| v.split(';'))
        .find_map(|c| c.trim().strip_prefix(&format!("{name}=")).map(String::from))
}

fn bearer(headers: &HeaderMap) -> Option<&str> {
    headers.get(header::AUTHORIZATION)?.to_str().ok()?.strip_prefix("Bearer ").map(str::trim)
}

/// True when the request comes from our own page. Browsers always send Origin on WebSocket
/// upgrades and on cross-site POSTs, so a missing Origin is refused too.
pub fn origin_allowed(st: &AppState, headers: &HeaderMap) -> bool {
    let Some(origin) = headers.get(header::ORIGIN).and_then(|v| v.to_str().ok()) else {
        return false;
    };
    let host = headers.get(header::HOST).and_then(|v| v.to_str().ok()).unwrap_or("");
    origin == st.config.origin || origin.split("://").nth(1) == Some(host)
}

/// Authenticates a request without the CSRF check (callers decide when it is needed).
pub fn authenticate(st: &AppState, headers: &HeaderMap) -> Result<AuthUser, ApiError> {
    let (owner, via) = if let Some(token) = bearer(headers) {
        let owner = st.db.lookup(TokenKind::Access, token);
        (owner.ok_or_else(|| ApiError::new(StatusCode::UNAUTHORIZED, "The access token is invalid or expired."))?, Via::Bearer)
    } else if let Some(token) = cookie(headers, SESSION_COOKIE) {
        let owner = st.db.lookup(TokenKind::Session, &token);
        (owner.ok_or_else(|| ApiError::new(StatusCode::UNAUTHORIZED, "Your session has expired. Sign in again."))?, Via::Cookie)
    } else {
        return Err(ApiError::new(StatusCode::UNAUTHORIZED, "Sign in first."));
    };
    let role = st
        .config
        .role_for(&owner.user.email)
        .ok_or_else(|| ApiError::new(StatusCode::FORBIDDEN, "This account is no longer allowed."))?;
    Ok(AuthUser { user: owner.user, role, family: owner.family, via })
}

#[async_trait]
impl FromRequestParts<Arc<AppState>> for AuthUser {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, st: &Arc<AppState>) -> Result<Self, Self::Rejection> {
        let user = authenticate(st, &parts.headers)?;
        let safe = matches!(parts.method, Method::GET | Method::HEAD | Method::OPTIONS);
        if user.via == Via::Cookie && !safe && !origin_allowed(st, &parts.headers) {
            return Err(ApiError::new(StatusCode::FORBIDDEN, "Request blocked: it did not come from this site."));
        }
        Ok(user)
    }
}

// ---------- login page ----------

#[derive(Deserialize)]
pub struct LoginQuery {
    e: Option<String>,
}

fn login_error(code: &str) -> &'static str {
    match code {
        "password" => "Wrong password. Try again.",
        "not_allowed" => "That account is not on the allowed list. Ask the admin to add your email.",
        "expired" => "The sign-in took too long or was started in another tab. Try again.",
        "rate" => "Too many attempts. Wait a few minutes and try again.",
        "cancelled" => "Sign-in was cancelled.",
        _ => "Sign-in failed. Try again.",
    }
}

fn escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

pub async fn login_page(State(st): State<Arc<AppState>>, Query(q): Query<LoginQuery>) -> Html<String> {
    let mut buttons = String::new();
    if st.config.google.is_some() {
        buttons += r#"<a class="provider google" href="/auth/google/start">Sign in with Google</a>"#;
    }
    if st.config.apple.as_ref().is_some_and(|a| a.services_id.is_some()) {
        buttons += r#"<a class="provider apple" href="/auth/apple/start">Sign in with Apple</a>"#;
    }
    let password = if st.config.password.is_some() {
        let open = if buttons.is_empty() || q.e.as_deref() == Some("password") { " open" } else { "" };
        format!(
            r#"<details class="password"{open}><summary>Emergency password login</summary>
<form method="post" action="/login">
<label for="password">Password</label>
<input id="password" name="password" type="password" autocomplete="current-password" required>
<button type="submit">Sign in</button>
</form></details>"#
        )
    } else {
        String::new()
    };
    let error = q.e.as_deref().map(login_error).unwrap_or("");
    Html(LOGIN_PAGE.replace("{{PROVIDERS}}", &buttons).replace("{{PASSWORD}}", &password).replace("{{ERROR}}", &escape(error)))
}

fn session_cookie(token: &str) -> String {
    format!("{SESSION_COOKIE}={token}; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age={}", db::SESSION_LIFETIME.as_secs())
}

/// Starts a web session. The answer is a small page that moves on to "/" by itself: after a
/// redirect from Google/Apple the browser would not send a SameSite=Strict cookie yet.
fn finish_web_login(st: &AppState, user: &User, how: &str) -> Response {
    let family = new_token("fam");
    let token = match st.db.issue(TokenKind::Session, user.id, &family, "web") {
        Ok(t) => t,
        Err(e) => {
            eprintln!("could not store session: {e}");
            return Redirect::to("/login?e=failed").into_response();
        }
    };
    println!("sign-in: {} via {how} (web)", user.email);
    let page = r#"<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0;url=/"><title>Signed in</title><a href="/">Continue</a>"#;
    let clear_flow = format!("{FLOW_COOKIE}=; Path=/auth; HttpOnly; Secure; SameSite=None; Max-Age=0");
    let mut resp = Html(page).into_response();
    resp.headers_mut().append(header::SET_COOKIE, session_cookie(&token).parse().unwrap());
    resp.headers_mut().append(header::SET_COOKIE, clear_flow.parse().unwrap());
    resp
}

// ---------- emergency password login ----------

#[derive(Deserialize)]
pub struct PasswordForm {
    password: String,
}

pub fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

pub async fn password_login(State(st): State<Arc<AppState>>, headers: HeaderMap, Form(form): Form<PasswordForm>) -> Response {
    let Some(password) = st.config.password.as_deref() else {
        return Redirect::to("/login?e=failed").into_response();
    };
    let ip = client_ip(&headers);
    if !st.login_limiter.allow(&ip) {
        eprintln!("password login rate-limited for {ip}");
        return Redirect::to("/login?e=rate").into_response();
    }
    if !constant_time_eq(form.password.as_bytes(), password.as_bytes()) {
        eprintln!("wrong password from {ip}");
        tokio::time::sleep(Duration::from_secs(1)).await; // slows down guessing
        return Redirect::to("/login?e=password").into_response();
    }
    match st.db.user_for_identity("password", PASSWORD_ACCOUNT, PASSWORD_ACCOUNT, Some("Emergency login")) {
        Ok(user) => finish_web_login(&st, &user, "password"),
        Err(e) => {
            eprintln!("password login failed: {e}");
            Redirect::to("/login?e=failed").into_response()
        }
    }
}

// ---------- web sign-in with Google / Apple ----------

pub struct PendingFlow {
    provider: Provider,
    nonce: String,
    verifier: String,
    created: Instant,
}

/// Remembers a started sign-in and binds it to this browser with a short-lived cookie.
/// SameSite=None because Apple returns with a cross-site POST.
async fn start_flow(st: &AppState, provider: Provider) -> Result<(String, String, String, String), Box<Response>> {
    let state = new_token("st");
    let nonce = new_token("n");
    let verifier = new_token("v");
    let mut pending = st.pending.lock().await;
    pending.retain(|_, f| f.created.elapsed() < FLOW_LIFETIME);
    if pending.len() >= MAX_PENDING_FLOWS {
        return Err(Box::new(Redirect::to("/login?e=rate").into_response()));
    }
    pending.insert(state.clone(), PendingFlow { provider, nonce: nonce.clone(), verifier: verifier.clone(), created: Instant::now() });
    let cookie = format!("{FLOW_COOKIE}={state}; Path=/auth; HttpOnly; Secure; SameSite=None; Max-Age={}", FLOW_LIFETIME.as_secs());
    Ok((state, nonce, verifier, cookie))
}

/// Takes the pending sign-in for `state`, if this browser started it and it is still fresh.
async fn take_flow(st: &AppState, headers: &HeaderMap, state: &str, provider: Provider) -> Option<PendingFlow> {
    if cookie(headers, FLOW_COOKIE).as_deref() != Some(state) {
        return None;
    }
    let flow = st.pending.lock().await.remove(state)?;
    (flow.provider == provider && flow.created.elapsed() < FLOW_LIFETIME).then_some(flow)
}

fn redirect_with_cookie(url: &str, cookie: String) -> Response {
    let mut resp = Redirect::to(url).into_response();
    resp.headers_mut().append(header::SET_COOKIE, cookie.parse().unwrap());
    resp
}

/// Turns a verified identity into a user, if the email is allowed.
fn allowed_user(st: &AppState, id: &oidc::Identity, name: Option<&str>) -> Result<(User, Role), ApiError> {
    let Some(role) = st.config.role_for(&id.email) else {
        println!("sign-in refused: {} via {} is not on the allowlist", id.email, id.provider.as_str());
        return Err(ApiError::new(StatusCode::FORBIDDEN, "This account is not on the allowed list."));
    };
    let user = st.db.user_for_identity(id.provider.as_str(), &id.subject, &id.email, name.or(id.name.as_deref())).map_err(internal)?;
    Ok((user, role))
}

fn web_failure(err: ApiError) -> Response {
    if err.0 == StatusCode::FORBIDDEN {
        Redirect::to("/login?e=not_allowed").into_response()
    } else {
        eprintln!("web sign-in failed: {}", err.1);
        Redirect::to("/login?e=failed").into_response()
    }
}

pub async fn google_start(State(st): State<Arc<AppState>>) -> Response {
    let Some(google) = &st.config.google else { return Redirect::to("/login").into_response() };
    let (state, nonce, verifier, cookie) = match start_flow(&st, Provider::Google).await {
        Ok(v) => v,
        Err(resp) => return *resp,
    };
    let mut url = reqwest::Url::parse(oidc::GOOGLE_AUTH_URL).unwrap();
    url.query_pairs_mut()
        .append_pair("client_id", &google.web_client_id)
        .append_pair("redirect_uri", &format!("{}/auth/google/callback", st.config.origin))
        .append_pair("response_type", "code")
        .append_pair("scope", "openid email profile")
        .append_pair("state", &state)
        .append_pair("nonce", &nonce)
        .append_pair("code_challenge", &oidc::pkce_challenge(&verifier))
        .append_pair("code_challenge_method", "S256")
        .append_pair("prompt", "select_account");
    redirect_with_cookie(url.as_str(), cookie)
}

#[derive(Deserialize)]
pub struct GoogleCallback {
    code: Option<String>,
    state: Option<String>,
    error: Option<String>,
}

pub async fn google_callback(State(st): State<Arc<AppState>>, headers: HeaderMap, Query(q): Query<GoogleCallback>) -> Response {
    let Some(google) = &st.config.google else { return Redirect::to("/login").into_response() };
    if q.error.is_some() {
        return Redirect::to("/login?e=cancelled").into_response();
    }
    let (Some(code), Some(state)) = (q.code, q.state) else { return Redirect::to("/login?e=failed").into_response() };
    let Some(flow) = take_flow(&st, &headers, &state, Provider::Google).await else {
        return Redirect::to("/login?e=expired").into_response();
    };
    let redirect_uri = format!("{}/auth/google/callback", st.config.origin);
    let result = async {
        let id_token = oidc::google_exchange_code(&st.http, &google.web_client_id, &google.web_client_secret, &code, &redirect_uri, &flow.verifier)
            .await
            .map_err(|e| ApiError::new(StatusCode::BAD_GATEWAY, e))?;
        let audiences = [google.web_client_id.clone()];
        let id = oidc::verify(&st.http, &st.google_keys, &id_token, &audiences, NonceCheck::Exact(&flow.nonce))
            .await
            .map_err(|e| ApiError::new(StatusCode::UNAUTHORIZED, e))?;
        allowed_user(&st, &id, None)
    }
    .await;
    match result {
        Ok((user, _)) => finish_web_login(&st, &user, "google"),
        Err(e) => web_failure(e),
    }
}

pub async fn apple_start(State(st): State<Arc<AppState>>) -> Response {
    let Some(services_id) = st.config.apple.as_ref().and_then(|a| a.services_id.clone()) else {
        return Redirect::to("/login").into_response();
    };
    let (state, nonce, _, cookie) = match start_flow(&st, Provider::Apple).await {
        Ok(v) => v,
        Err(resp) => return *resp,
    };
    let mut url = reqwest::Url::parse(oidc::APPLE_AUTH_URL).unwrap();
    url.query_pairs_mut()
        .append_pair("client_id", &services_id)
        .append_pair("redirect_uri", &format!("{}/auth/apple/callback", st.config.origin))
        .append_pair("response_type", "code id_token")
        .append_pair("response_mode", "form_post")
        .append_pair("scope", "name email")
        .append_pair("state", &state)
        .append_pair("nonce", &nonce);
    redirect_with_cookie(url.as_str(), cookie)
}

#[derive(Deserialize)]
pub struct AppleCallback {
    state: Option<String>,
    id_token: Option<String>,
    /// JSON with the person's name; Apple sends it only on the very first sign-in.
    user: Option<String>,
    error: Option<String>,
}

fn apple_name(user_json: Option<&str>) -> Option<String> {
    let v: serde_json::Value = serde_json::from_str(user_json?).ok()?;
    let first = v["name"]["firstName"].as_str().unwrap_or("");
    let last = v["name"]["lastName"].as_str().unwrap_or("");
    let full = format!("{first} {last}").trim().to_string();
    (!full.is_empty()).then_some(full)
}

/// Apple posts the ID token straight back (response_mode=form_post). Its signature, audience
/// and our nonce prove who signed in, so the code exchange (and Apple's .p8 key) is not needed.
pub async fn apple_callback(State(st): State<Arc<AppState>>, headers: HeaderMap, Form(f): Form<AppleCallback>) -> Response {
    let Some(services_id) = st.config.apple.as_ref().and_then(|a| a.services_id.clone()) else {
        return Redirect::to("/login").into_response();
    };
    if f.error.is_some() {
        return Redirect::to("/login?e=cancelled").into_response();
    }
    let (Some(state), Some(id_token)) = (f.state, f.id_token) else { return Redirect::to("/login?e=failed").into_response() };
    let Some(flow) = take_flow(&st, &headers, &state, Provider::Apple).await else {
        return Redirect::to("/login?e=expired").into_response();
    };
    let name = apple_name(f.user.as_deref());
    let result = async {
        let id = oidc::verify(&st.http, &st.apple_keys, &id_token, &[services_id], NonceCheck::Exact(&flow.nonce))
            .await
            .map_err(|e| ApiError::new(StatusCode::UNAUTHORIZED, e))?;
        allowed_user(&st, &id, name.as_deref())
    }
    .await;
    match result {
        Ok((user, _)) => finish_web_login(&st, &user, "apple"),
        Err(e) => web_failure(e),
    }
}

pub async fn logout(State(st): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if let Some(token) = cookie(&headers, SESSION_COOKIE) {
        if let Some(owner) = st.db.lookup(TokenKind::Session, &token) {
            let _ = st.db.revoke_family(&owner.family);
        }
    }
    let cookie = format!("{SESSION_COOKIE}=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0");
    let mut resp = Redirect::to("/login").into_response();
    resp.headers_mut().append(header::SET_COOKIE, cookie.parse().unwrap());
    resp
}

// ---------- mobile app tokens ----------

#[derive(Deserialize)]
pub struct AppSignIn {
    id_token: String,
    /// The raw nonce the app passed to the native sign-in SDK (recommended).
    nonce: Option<String>,
    /// Apple gives the name to the app only on the first sign-in; the app may pass it on.
    name: Option<String>,
    /// A label for this device, e.g. "Akash's iPhone" (max 64 characters).
    client: Option<String>,
}

#[derive(Serialize)]
pub struct TokenResponse {
    access_token: String,
    token_type: &'static str,
    expires_in: u64,
    refresh_token: String,
    user: serde_json::Value,
}

pub fn user_json(user: &User, role: Role) -> serde_json::Value {
    json!({ "email": user.email, "name": user.name, "role": role.as_str() })
}

fn issue_app_tokens(st: &AppState, user: &User, role: Role, family: &str, client: &str) -> Result<TokenResponse, ApiError> {
    let access_token = st.db.issue(TokenKind::Access, user.id, family, client).map_err(internal)?;
    let refresh_token = st.db.issue(TokenKind::Refresh, user.id, family, client).map_err(internal)?;
    Ok(TokenResponse {
        access_token,
        token_type: "Bearer",
        expires_in: db::ACCESS_LIFETIME.as_secs(),
        refresh_token,
        user: user_json(user, role),
    })
}

fn rate_check(st: &AppState, headers: &HeaderMap) -> Result<(), ApiError> {
    if st.api_limiter.allow(&client_ip(headers)) {
        Ok(())
    } else {
        Err(ApiError::new(StatusCode::TOO_MANY_REQUESTS, "Too many sign-in attempts. Wait a minute."))
    }
}

async fn app_sign_in(st: &AppState, headers: &HeaderMap, provider: Provider, body: AppSignIn) -> Result<Json<TokenResponse>, ApiError> {
    rate_check(st, headers)?;
    let (keys, audiences) = match provider {
        Provider::Google => {
            let g = st.config.google.as_ref().ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "Google sign-in is not set up."))?;
            let mut auds = g.app_client_ids.clone();
            auds.push(g.web_client_id.clone());
            (&st.google_keys, auds)
        }
        Provider::Apple => {
            let a = st.config.apple.as_ref().ok_or_else(|| ApiError::new(StatusCode::NOT_FOUND, "Apple sign-in is not set up."))?;
            (&st.apple_keys, a.bundle_ids.clone())
        }
    };
    let id = oidc::verify(&st.http, keys, &body.id_token, &audiences, NonceCheck::FromApp(body.nonce.as_deref()))
        .await
        .map_err(|e| ApiError::new(StatusCode::UNAUTHORIZED, e))?;
    let (user, role) = allowed_user(st, &id, body.name.as_deref())?;
    let client: String = body.client.unwrap_or_else(|| "app".into()).chars().take(64).collect();
    let tokens = issue_app_tokens(st, &user, role, &new_token("fam"), &client)?;
    println!("sign-in: {} via {} (app \"{client}\")", user.email, provider.as_str());
    Ok(Json(tokens))
}

pub async fn app_google(State(st): State<Arc<AppState>>, headers: HeaderMap, Json(body): Json<AppSignIn>) -> Result<Json<TokenResponse>, ApiError> {
    app_sign_in(&st, &headers, Provider::Google, body).await
}

pub async fn app_apple(State(st): State<Arc<AppState>>, headers: HeaderMap, Json(body): Json<AppSignIn>) -> Result<Json<TokenResponse>, ApiError> {
    app_sign_in(&st, &headers, Provider::Apple, body).await
}

#[derive(Deserialize)]
pub struct RefreshBody {
    refresh_token: String,
}

pub async fn app_refresh(State(st): State<Arc<AppState>>, headers: HeaderMap, Json(body): Json<RefreshBody>) -> Result<Json<TokenResponse>, ApiError> {
    rate_check(&st, &headers)?;
    match st.db.refresh(&body.refresh_token).map_err(internal)? {
        Refresh::Ok { user, family, client } => {
            let Some(role) = st.config.role_for(&user.email) else {
                let _ = st.db.revoke_family(&family);
                return Err(ApiError::new(StatusCode::FORBIDDEN, "This account is no longer allowed."));
            };
            Ok(Json(issue_app_tokens(&st, &user, role, &family, &client)?))
        }
        Refresh::Reused => {
            eprintln!("refresh token reused from {}: that sign-in was revoked", client_ip(&headers));
            Err(ApiError::new(StatusCode::UNAUTHORIZED, "This sign-in was revoked for safety. Sign in again."))
        }
        Refresh::Invalid => Err(ApiError::new(StatusCode::UNAUTHORIZED, "The refresh token is invalid or expired. Sign in again.")),
    }
}

/// Signs out the login the request belongs to (the app's tokens, or the web session).
pub async fn app_logout(State(st): State<Arc<AppState>>, user: AuthUser) -> Result<StatusCode, ApiError> {
    st.db.revoke_family(&user.family).map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rate_limiter_blocks_after_max() {
        let rl = RateLimiter::new(2, Duration::from_secs(60));
        assert!(rl.allow("a"));
        assert!(rl.allow("a"));
        assert!(!rl.allow("a"));
        assert!(rl.allow("b"));
    }

    #[test]
    fn apple_name_is_read_from_the_first_sign_in() {
        let json = r#"{"name":{"firstName":"Ada","lastName":"Lovelace"},"email":"a@b.c"}"#;
        assert_eq!(apple_name(Some(json)).as_deref(), Some("Ada Lovelace"));
        assert_eq!(apple_name(None), None);
        assert_eq!(apple_name(Some("not json")), None);
    }
}
