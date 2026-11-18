# iOS app: notes for building it

Nothing is built yet. This folder will hold the Xcode project. Everything the app needs from the server
already exists and is deployed (server v0.2.0). The server contract is in [`../server/API.md`](../server/API.md):
read it first.

## What the app does

- Sign in with Apple or Google, then talk only to our server (never to the ESP32 or LoRa directly).
- Show every node live: relays, fan level, curtain position/moving, dimmer level, master online/offline.
- Control: relays on/off/toggle, fan level, curtain open/close/stop/go-to %, dimmer level / next / on / off.
- Admin-only extras (hide for members, the server refuses them anyway): scan, curtain learn, fan re-home, master LED.
- Nice later: Home Screen widgets, Siri / Shortcuts via App Intents, a Live Activity while a curtain moves.

## Identifiers (already registered)

| What | Value | Where |
|---|---|---|
| Bundle ID (App ID with *Sign in with Apple*) | `me.akashmahor.automation` | developer.apple.com → Identifiers. Server: `APPLE_BUNDLE_IDS` ✅ |
| Services ID (web sign-in only, not used by the app) | `me.akashmahor.automation.web` | Server: `APPLE_SERVICES_ID` ✅ |
| Google **iOS** OAuth client | **not created yet** | console.cloud.google.com → Credentials → OAuth client ID → *iOS*, bundle ID above. Add its client ID to `GOOGLE_APP_CLIENT_IDS` in the Pi's env file and restart the server |
| Server base URL | `https://automation.akashmahor.me` | |

## Suggested stack

- SwiftUI, iOS 17+ (Observation framework, `async`/`await`). No third-party networking library needed.
- `AuthenticationServices` for Sign in with Apple (Xcode: add the *Sign in with Apple* capability).
- [GoogleSignIn-iOS](https://github.com/google/GoogleSignIn-iOS) via Swift Package Manager. Info.plist:
  `GIDClientID` = the iOS client ID, plus a URL type whose scheme is the *reversed* client ID
  (`com.googleusercontent.apps.…`, shown by Google when you create the iOS client).
- `URLSession` for REST, `URLSessionWebSocketTask` for live updates, Keychain for tokens.

## Sign-in flow

1. Generate a random **raw nonce** (32 bytes, base64url) for every attempt.
2. **Apple:** `ASAuthorizationAppleIDProvider().createRequest()`, `requestedScopes = [.fullName, .email]`,
   `request.nonce = sha256Hex(rawNonce)`. From the credential: `identityToken` (UTF-8 string) and, on the
   **first sign-in only**, `fullName`.
   `POST /api/v1/auth/apple {"id_token", "nonce": rawNonce, "name": "First Last", "client": UIDevice.current.name}`
3. **Google:** `GIDSignIn.sharedInstance.signIn(withPresenting:hint:additionalScopes:nonce:)` with the raw nonce,
   then `result.user.idToken?.tokenString`.
   `POST /api/v1/auth/google {"id_token", "nonce": rawNonce, "client": ...}`
4. Response: `access_token` (15 min), `refresh_token` (60 days), `user {email, name, role}`. Store both tokens in
   the Keychain (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`), keep `role` to show/hide admin controls.
5. Errors: `403` = email not on the allowlist (show "Ask the admin to add <email>"). `401` = token/nonce problem
   (retry sign-in). `429` = wait a minute.
6. Tell users to pick **Share My Email** on the Apple sheet: "Hide My Email" gives an
   `@privaterelay.appleid.com` address that is not on the allowlist unless the admin adds it.

## Calling the API

- Every request: `Authorization: Bearer <access_token>`.
- On `401`: refresh once (`POST /api/v1/auth/refresh`), retry the request once, else go to the sign-in screen.
- **Refresh must be single-flight.** Refresh tokens rotate, and presenting an already-used one signs the device
  out for safety. Put refreshing in one `actor` so concurrent requests wait for the same refresh instead of
  each sending the old token. Save the new refresh token to the Keychain *before* using the new access token.
- Refresh proactively when the access token has < 1 minute left (`expires_in` is in seconds).
- Commands: `POST /api/v1/commands` waits up to 15 s for the LoRa result. Use a 20 s request timeout and show a
  spinner on the control. `200` = done (use `result` to update the UI right away); `502` = the node did not answer
  or refused (`result.error`); `503` = master offline; `504` = no answer in time; `202` for `scan` / node `"all"`.
- Sign out: `POST /api/v1/auth/logout`, then delete the Keychain items and call `GIDSignIn.sharedInstance.signOut()`.

## Live updates

- `URLRequest(url: wss://automation.akashmahor.me/ws/ui)` with the `Authorization` header →
  `URLSession.shared.webSocketTask(with: request)`.
- Messages: `{"type":"me"}` once, then `{"type":"state", ...}` on every change (same shape as
  `GET /api/v1/state`), and `{"type":"log", level, text}`. Render purely from the latest `state`.
- The server closes the socket within a minute if the login is revoked. Reconnect with backoff (1, 2, 5, 10 s),
  refreshing the access token first. Disconnect when the app goes to the background; on foreground, call
  `GET /api/v1/state` and reconnect.

## Data model (Codable)

```swift
struct AppState: Decodable { let master_online: Bool; let led: Bool?; let nodes: [Node] }
struct Node: Decodable, Identifiable {
    let id: String; let reachable: Bool; let rssi: Int?; let snr: Double?
    let relays: [Bool]; let fan: Int?; let fan_homing: Bool
    let curtain: Curtain?; let dimmer: Dimmer?
}
struct Curtain: Decodable { let position: Int?; let moving: Bool?; let learning: Bool?; let learned: Bool?
                            let open: Bool?; let closed: Bool?; let fault: Bool? }
struct Dimmer: Decodable { let level: Int?; let mains: Bool?; let fading: Bool? }
```
Use `.convertFromSnakeCase` or explicit `CodingKeys`. A node is a curtain node if `curtain != nil`, a dimmer
node if `dimmer != nil` (then its `fan` is the dimmer's channel 2), otherwise a relay node (4 relays, optional fan).
Curtain `position` is `null` while unknown: show "Position unknown, press Open to re-home".

## Distribution / App Review

- For family use, **TestFlight internal testing** (up to 100 people on your developer team) or ad-hoc
  is simplest; no public App Store listing needed.
- If it goes to the App Store: offering Google sign-in requires also offering Sign in with Apple (done). Apps
  that create accounts must let users delete them in the app (guideline 5.1.1(v)). The server has no
  delete-account endpoint yet (see "Server work").
- No `.p8` key is needed: the server checks Apple's ID tokens itself and never stores Apple refresh tokens.

## Server work the app may want (not built yet)

- `DELETE /api/v1/me`: delete the account and sign out everywhere (App Store account deletion rule).
- User approval screen / endpoints: people not on the allowlist request access, an admin approves in the app.
- Apple server-to-server notifications (`/auth/apple/notifications`): sign users out when they revoke Apple
  sign-in. The App ID's *Server-to-Server Notification Endpoint* was left empty for now.
- Push notifications (curtain fault, master offline) would need APNs: a `.p8` key and a device-token endpoint.
