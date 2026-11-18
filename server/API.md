# Automation server API (v1)

Base URL: the public origin, e.g. `https://automation.example.com`. All bodies are JSON.
Errors look like `{"error": "human-readable message"}`.

## Authentication

| Client | How |
|---|---|
| Mobile app | `Authorization: Bearer <access_token>` from the sign-in endpoints below |
| Web page (same origin) | the `automation_session` cookie. Requests that change something must send our own `Origin` (browsers do this automatically) |

Only emails on the server's allowlist can sign in. **admin** may run every command;
**member** may not run `scan`, `led`, curtain `learn` or fan `home` (they get `403`).

### Sign in (mobile)

Use the platform's native SDK, then trade its ID token for our tokens.

```
POST /api/v1/auth/google   {"id_token": "...", "nonce": "raw-nonce", "client": "Pixel 9"}
POST /api/v1/auth/apple    {"id_token": "...", "nonce": "raw-nonce", "name": "Ada Lovelace", "client": "iPhone"}
```

- `nonce`: the **raw** nonce you generated and passed (as-is or SHA-256 hashed) to the SDK. Strongly
  recommended. If the ID token contains a nonce, this field is required.
- `name`: Apple returns the person's name to the app only on the very first sign-in; pass it on.
- `client`: a label for this device (max 64 characters).
- Google: the token's audience must be one of `GOOGLE_APP_CLIENT_IDS` or `GOOGLE_CLIENT_ID`
  (on Android, set `serverClientId` to `GOOGLE_CLIENT_ID`). Apple: audience = one of `APPLE_BUNDLE_IDS`.

Response `200`:
```json
{
  "access_token": "at_…", "token_type": "Bearer", "expires_in": 900,
  "refresh_token": "rt_…",
  "user": {"email": "you@example.com", "name": "You", "role": "admin"}
}
```
`401` bad/expired token or nonce mismatch · `403` email not on the allowlist · `404` provider not set up ·
`429` too many attempts (30/min per IP).

Store the refresh token in the Keychain / Android Keystore. Never log tokens.

### Refresh

```
POST /api/v1/auth/refresh  {"refresh_token": "rt_…"}
```
Returns a **new** access token and a **new** refresh token (same shape as sign-in). The old refresh
token stops working. If an already-used refresh token is ever presented again, the server assumes it
was copied and signs that device out completely (`401`); the app must sign in again.
Access tokens last 15 minutes, refresh tokens 60 days (renewed on every refresh).

### Sign out

```
POST /api/v1/auth/logout   (Bearer)  -> 204
```
Revokes this device's access and refresh tokens.

## Endpoints

### `GET /api/v1/me`
`{"email": "...", "name": "...", "role": "admin" | "member"}`

### `GET /api/v1/state`
```json
{
  "type": "state", "master_online": true, "led": false,
  "nodes": [
    {"id": "E26D", "reachable": true, "rssi": -40, "snr": 9.5, "relays": [false,false,false,false],
     "fan": null, "fan_homing": false,
     "curtain": {"position": 100, "moving": false, "learning": false, "learned": true, "open": true, "closed": false, "fault": false},
     "dimmer": null}
  ]
}
```

### `POST /api/v1/commands`
Runs one command and waits (up to 15 s) for the ESP32 master's LoRa result.

| action | fields | role |
|---|---|---|
| `relay` | `node`, `relay` (1-4, 0 = all), `state`: `on` / `off` / `toggle` | member |
| `fan` | `node`, `level` 0-100 — or `state: "home"` (re-home) | member / **admin** for home |
| `curtain` | `node`, `state`: `open` / `close` / `stop` / `goto` (+ `level` 0-100) / `learn` | member / **admin** for learn |
| `dim` | `node`, `state`: `set` (+ `level` 0-100) / `next` / `on` / `off` | member |
| `status` | `node` (or `"all"`) | member |
| `led` | `state`: `on` / `off` (master's test LED) | **admin** |
| `scan` | – | **admin** |

`node` is a 1-4 digit hex ID (e.g. `E26D`) or `"all"`.

Responses:
- `200` `{"id": 12, "result": {"ok": true, "node": "E26D", "relays": [...], "curtain": {...}, "rssi": -40, ...}}`
- `202` `{"id": 13, "status": "sent"}` for `scan` and node `"all"` (several results; watch the state)
- `400` invalid command · `403` needs admin · `502` the node answered with an error or did not answer
  (`result.error`) · `503` master offline · `504` no result within 15 s

Example:
```bash
curl -X POST https://automation.example.com/api/v1/commands \
  -H "Authorization: Bearer $AT" -H "Content-Type: application/json" \
  -d '{"action":"curtain","node":"E26D","state":"goto","level":50}'
```

## Live updates: `GET /ws/ui` (WebSocket)

Send `Authorization: Bearer <access_token>` on the upgrade request (mobile), or rely on the cookie
(web page, our Origin only). The server sends:
- `{"type":"me", "email", "name", "role"}` once, then
- `{"type":"state", ...}` (same as `/api/v1/state`) on every change, and
- `{"type":"log", "level":"info"|"error", "text": "..."}` messages.

You may send commands as JSON (same body as `/api/v1/commands`); errors come back as `log` messages.
The socket closes within a minute after the login is revoked or the email is removed from the allowlist.
Reconnect with a fresh access token when it closes.

## Web sign-in routes (browser only)
`GET /login`, `POST /login` (emergency password), `GET /auth/google/start` → `GET /auth/google/callback`,
`GET /auth/apple/start` → `POST /auth/apple/callback`, `POST /logout`.
