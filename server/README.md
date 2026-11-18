# Automation server

Rust (axum 0.7, tokio) server. It serves a password-protected control page and relays commands between
browsers (`/ws/ui`) and the ESP32 LoRa master (`/ws/device`). The HTML pages in `static/` are compiled into the
binary with `include_str!`, so any page change needs a rebuild.

## Run locally
```bash
export AUTOMATION_PASSWORD=dev AUTOMATION_DEVICE_TOKEN=dev-token \
       AUTOMATION_ORIGIN=http://localhost:3000 AUTOMATION_BIND=127.0.0.1:3000
cargo run --release
```
The session cookie is `Secure`, so login only sticks over HTTPS (or on browsers that treat localhost as secure).

## Deploy (Raspberry Pi, Debian aarch64)
1. Copy `Cargo.toml`, `Cargo.lock`, `src/`, `static/` to `~/automation` on the Pi (back up the old files first).
2. `cd ~/automation && ~/.cargo/bin/cargo build --release` (clean build ~17 min on 905 MB RAM; add swap if it is OOM-killed).
3. First time only: `sudo install -m 600 automation.env /etc/automation/automation.env` (from
   `deploy/automation.env.example`), `sudo cp deploy/automation.service /etc/systemd/system/`,
   `sudo systemctl enable --now automation`.
4. `sudo systemctl restart automation` and watch `sudo journalctl -u automation -f`.

A Cloudflare tunnel maps the public hostname to `http://localhost:3000`.
