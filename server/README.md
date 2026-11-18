# Automation server

Rust (axum 0.7, tokio) server. It serves the control page, a REST API for the mobile app
([API.md](API.md)), and relays commands to the ESP32 LoRa master (`/ws/device`).
The HTML pages in `static/` are compiled into the binary with `include_str!`, so page changes need a rebuild.

| File | Role |
|---|---|
| `src/main.rs` | state, routes, browser/app WebSocket, ESP32 WebSocket |
| `src/auth.rs` | who is calling (cookie / Bearer), web sign-in flows, app tokens, rate limits |
| `src/oidc.rs` | Google/Apple ID-token verification (JWKS cache), Google code exchange, PKCE |
| `src/db.rs` | SQLite: users, linked identities, hashed tokens (session / access / refresh) |
| `src/api.rs` | REST handlers, command validation and roles (shared with the WebSocket) |
| `src/config.rs` | environment variables, allowlist, roles |

## Sign-in

- **Google** and **Apple** on the web page and in the mobile app. Only emails in `AUTOMATION_ADMINS` /
  `AUTOMATION_MEMBERS` get in. One person signing in with both providers (same email) gets one account.
- **Emergency password** (`AUTOMATION_PASSWORD`): admin login for when Google/Apple are unavailable.
  Rate-limited to 10 tries per 15 minutes per IP. Leave empty to turn it off.
- Tokens are random and stored only as SHA-256 hashes in SQLite, so restarts keep everyone signed in.

### Adding or removing people
There is no approval screen yet (planned, see `ios/README.md` → "Server work"). The allowlist is two lines in
`/etc/automation/automation.env` on the Pi, read at startup:
```
AUTOMATION_ADMINS=you@gmail.com
AUTOMATION_MEMBERS=family1@gmail.com,family2@icloud.com
```
1. Edit the file (`sudo nano /etc/automation/automation.env`), comma-separated, no spaces needed.
2. `sudo systemctl restart automation`. Existing logins stay valid; removed people are refused on their next
   request and their open pages/app sockets close within a minute.
3. **Google:** while the OAuth consent screen is in *Testing*, Google itself blocks anyone not listed under
   *Test users* (Google Auth Platform → Audience). Add them there too, or publish the app (no review needed for
   `openid email profile`). Apple has no such list.
4. Apple "Hide My Email" users sign in as `…@privaterelay.appleid.com`; add that address, or have them share
   their real email.

### Google setup (console.cloud.google.com)
1. Create (or pick) a project. **APIs & Services → OAuth consent screen**: External, app name, your email;
   scopes `openid`, `email`, `profile`. While the app is in "Testing", add every allowed person as a test user
   (or publish it; these scopes need no Google review).
2. **Credentials → Create credentials → OAuth client ID → Web application**.
   Authorized redirect URI: `https://<your host>/auth/google/callback`.
   Put the client ID and secret in `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET`.
3. For the mobile app: create an **iOS** client (bundle ID) and/or an **Android** client (package name + SHA-1),
   and list their client IDs in `GOOGLE_APP_CLIENT_IDS`.

### Apple setup (developer.apple.com → Certificates, Identifiers & Profiles)
1. **Identifiers → App IDs**: your iOS app's bundle ID with the *Sign in with Apple* capability.
   List it in `APPLE_BUNDLE_IDS` (that is the audience of the app's tokens).
2. **Identifiers → Services IDs** (for the web): e.g. `com.yourname.automation.web`. Enable *Sign in with Apple*,
   configure it with the App ID above as primary, domain `<your host>` and return URL
   `https://<your host>/auth/apple/callback`. Put the Services ID in `APPLE_SERVICES_ID`.
3. No `.p8` key is needed: the server checks the ID token Apple posts back (signature, audience, nonce) and
   never calls Apple's token endpoint.
4. "Hide My Email" users arrive as `…@privaterelay.appleid.com`: add that address to the allowlist, or have them
   share their real email.

## Run locally (Docker, no Rust install needed)
```bash
docker run --rm -it -p 3000:3000 -v "$PWD:/src" -w /src -e CARGO_TARGET_DIR=/tmp/t \
  -e AUTOMATION_BIND=0.0.0.0:3000 -e AUTOMATION_ORIGIN=http://localhost:3000 \
  -e AUTOMATION_DEVICE_TOKEN=dev-token -e AUTOMATION_PASSWORD=dev -e AUTOMATION_DB=/tmp/dev.db \
  rust:1-bookworm cargo run
```
`cargo test` runs the unit tests (tokens, refresh rotation and reuse detection, nonce/claim checks, PKCE,
roles, command validation). The session cookie is `Secure`, so browsers keep it only over HTTPS (or on localhost).

## Deploy (Raspberry Pi, Debian aarch64)

The service runs `/home/<user>/automation/bin/automation-server`. Build it on the PC, not on the Pi
(cross-compiling in Docker takes about a minute; building on the Pi's 905 MB RAM takes 30+ minutes).

1. `scripts/build-server-pi.sh` (repo root) → `build/server-aarch64/automation-server`.
2. Copy it to the Pi as `~/automation/bin/automation-server.new`, `chmod 755`.
3. Optional dry run with the real settings on a spare port, while the live server keeps running:
   export each line of `/etc/automation/automation.env`, set `AUTOMATION_BIND=127.0.0.1:3999` and
   `AUTOMATION_DB=/tmp/dryrun.db`, start it and `curl http://127.0.0.1:3999/login`.
4. Keep the old binary: `cp -a ~/automation/bin/automation-server ~/automation/backup-$(date +%F)/`, then
   `mv automation-server.new automation-server`.
5. If `deploy/automation.service` changed: `sudo install -m 644 automation.service /etc/systemd/system/ && sudo systemctl daemon-reload`.
   New settings go in `/etc/automation/automation.env` (see `deploy/automation.env.example`).
6. `sudo systemctl restart automation` and check `sudo journalctl -u automation -n 20`. The first lines list the
   sign-in methods that are on.

**Rollback:** copy the backed-up binary back to `~/automation/bin/automation-server` and restart.
The database in `/var/lib/automation/` is only read by the new version; the old one ignores it.

The source in `~/automation` on the Pi (and `cargo build --release` there) still works as a fallback.
A Cloudflare tunnel maps the public hostname to `http://localhost:3000`.
