# Long-range home automation

DIY home automation over **LoRa (SX1278, 433 MHz)**. An ESP32 master talks to mains-powered
nodes (relays, a fan regulator, a motorised curtain, an AC light/fan dimmer) and keeps a WebSocket open to a
small Rust web server, so everything can be controlled from a phone browser.

```
browser ── HTTPS ──> Rust server (Raspberry Pi, Cloudflare tunnel) ── WSS ──> ESP32 master ── LoRa ──> nodes
```

| Folder | Contents |
|---|---|
| [`firmware/`](firmware/) | Arduino sketches: master, Pico/ESP32/Nano nodes, experiments, perfboard + PCB generators |
| [`server/`](server/) | Rust (axum) web server and its systemd/env deployment files |
| [`scripts/`](scripts/) | Helper scripts (compile all sketches) |
| [`PROJECT_STATUS.md`](PROJECT_STATUS.md) | Running log: hardware state, test results, next steps |
| [`AGENTS.md`](AGENTS.md) | Build, upload, protocol and safety notes (for humans and coding agents) |

## Quick start

1. Install Arduino CLI or IDE 2, the cores `esp32:esp32`, `rp2040:rp2040` (earlephilhower) and `arduino:avr`, and
   the libraries: `arduino-cli lib install "LoRa@0.8.0" "WebSockets@2.6.1" "ArduinoJson@7.2.1"`.
2. `cp firmware/LoRa_esp32_master/secrets.example.h firmware/LoRa_esp32_master/secrets.h` and fill it in.
3. `scripts/build-firmware.sh` to compile everything. Each sketch's board is set in its `sketch.yaml`.
4. Server: see [`server/README.md`](server/README.md).

The LoRa protocol has no authentication yet, so anyone with a 433 MHz radio can send commands.
