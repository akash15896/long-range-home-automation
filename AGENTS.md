# AGENTS.md: guide for coding agents

This is a hobby **LoRa (433 MHz) home-automation system**:

```
browser ──HTTPS/WSS──> automation server (Rust, axum) on a Raspberry Pi, behind a Cloudflare tunnel
                              │  /ws/device (Bearer token)
                              ▼
                       ESP32 LoRa master ──LoRa 433 MHz──> nodes (relays, fan, curtain, AC dimmer)
```

**Start every session by reading `PROJECT_STATUS.md`.** It is the running log of hardware state, test results
and next steps; the newest updates are at the top of each section. Read `LOCAL_NOTES.md` too if it exists
(gitignored: Pi address, SSH, private details). Code is the source of truth when the two disagree.

## Repository layout

| Path | What |
|---|---|
| `firmware/LoRa_esp32_master/` | ESP32 master: LoRa master + Wi-Fi (BLE provisioning) + WebSocket client to the server. Serial CLI. |
| `firmware/LoRa_pico_node/` | Pico 2 (RP2350) relay node: 4 relays + optional fan-knob stepper (`HAS_FAN`). Perfboard sheet generator. |
| `firmware/LoRa_pico_curtain/` | Pico 2 curtain node: stepper + 2 limit switches, learn/goto. TMC2209 wiring page. |
| `firmware/LoRa_pico_dimmer/` | Pico (RP2040) 2-channel AC phase dimmer node (light + fan). Perfboard sheets, `pcb_4ch/` 4-channel carrier PCB (Python Gerber generator). |
| `firmware/LoRa_esp32_node/` | Relay node on a second ESP32 (copy of the Pico node). |
| `firmware/LoRa_nano_node/` | Original 5 V Nano relay node (its radio module is faulty; not in use). |
| `firmware/experiments/` | Early tests and probes (hello link test, SX1278 ID probe, stepper spin tests, standalone dimmer). |
| `server/` | Rust automation server (`src/main.rs`; `static/*.html` are embedded with `include_str!`). |
| `server/deploy/` | systemd unit and env template. |
| `scripts/build-firmware.sh` | Compile-check sketches (no upload). |

Each Arduino sketch folder **must keep the same name as its `.ino`** (Arduino rule). Helper headers live
next to the `.ino` (e.g. `dimmer_channel.h`). Each sketch folder has a `sketch.yaml` with its `default_fqbn`.

## Safety rules (real hardware, mains voltage, motors)

1. **Never upload firmware, send serial commands that move motors/switch relays, or touch the Raspberry Pi
   without the user's go-ahead in this session.** Compiling is always fine.
2. **Secrets:** `firmware/LoRa_esp32_master/secrets.h` (device token, BLE PIN) and the server's
   `/etc/automation/automation.env` are private. Never print, log or commit them. The repo is public.
   Templates: `secrets.example.h`, `server/deploy/automation.env.example`.
3. **ESP32 master serial (CP210x):** open the port with **DTR and RTS both off**. DTR on holds BOOT (GPIO0) low,
   and after 10 s the firmware erases Wi-Fi. The Pico ports need DTR **on** to reply.
4. **Curtain:** if a script that drives the motor is killed, it never sent `stop`. Always send `stop` afterwards.
   Run `watch` after any switch wiring change, before moving the motor. The stepper driver may be unpowered:
   the firmware then still counts steps although nothing moved.
5. Mains boards (dimmer): tell the user to power off AC **and** USB before wiring anything.
6. Do not copy the Pico 2 open-drain relay trick to RP2040 or ESP32 boards (they are not 5 V tolerant).

## Toolchain

- Arduino CLI 1.5.x. On the user's Windows PC it is bundled with the IDE:
  `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe` (not on PATH).
- Cores: `esp32:esp32` 3.3.12, `rp2040:rp2040` 6.3.2 (earlephilhower), `arduino:avr` 1.8.8.
- Libraries (sketchbook `libraries/`, not in this repo): **LoRa 0.8.0** (sandeepmistry), **WebSockets 2.6.1**
  (links2004 / Markus Sattler), **ArduinoJson 7.2.1**. Fresh machine:
  `arduino-cli lib install "LoRa@0.8.0" "WebSockets@2.6.1" "ArduinoJson@7.2.1"`
- Rust server: built **on the Pi** (aarch64); no Rust toolchain on the Windows PC.

### Build / verify
```bash
scripts/build-firmware.sh                       # all sketches
scripts/build-firmware.sh LoRa_pico_curtain     # one sketch
```
The master is skipped if `secrets.h` is missing; copy `secrets.example.h` first (placeholders compile fine).
If the Arduino IDE has the sketch open, building with a separate `--build-path` (the script does) avoids clashes.

### Upload (only with the user's OK)
Ports change between sessions. Check `arduino-cli board list` and confirm with the user which board is which.
```bash
arduino-cli upload -p COMx firmware/LoRa_pico_curtain      # uses sketch.yaml's FQBN; compile first with the same build path
```

| Sketch | Board | FQBN (sketch.yaml) | Node ID | Usual port | Notes |
|---|---|---|---|---|---|
| LoRa_esp32_master | ESP32 dev (CP210x) | `esp32:esp32:esp32:PartitionScheme=huge_app` | 0x0000 | COM5 | Needs Huge APP partition. Hold BOOT for uploads. |
| LoRa_pico_curtain | Pico 2 | `rp2040:rp2040:rpipico2` | E26D | COM6 | |
| LoRa_pico_node | Pico 2 | `rp2040:rp2040:rpipico2` | (chip UID) | | Was first tested on the Pico 2 that now runs the curtain firmware (E26D). |
| LoRa_pico_dimmer | Pico (RP2040) | `rp2040:rp2040:rpipico` | E29E | COM8 | Mounted in the mains board. |
| LoRa_esp32_node | 2nd ESP32 | `esp32:esp32:esp32:FlashFreq=40,FlashMode=dio,UploadSpeed=115200` | 77CA | COM7 | Boot-loops at 80 MHz flash. Hold BOOT. |
| LoRa_nano_node | Nano (5 V, CH340) | `arduino:avr:nano` | EEPROM | | Radio module faulty; bootloader variant unconfirmed. |

If the first UF2 upload to an RP2350 is silently dropped, `picotool load -v -x <elf>` works.

## LoRa protocol (shared by master and every node)

433 MHz, SF7 / BW 125 kHz / CR 4/5 / preamble 8, sync word **0x12**, CRC on, TX 17 dBm.
Packet: `MAGIC 0xA7, type, dst(2), src(2), seq, len, data[len]` (header 8 bytes, max data 32).
Master 0x0000, broadcast 0xFFFF. Types: DISCOVER 1, HELLO 2, WELCOME 3, CMD 4, ACK 5.
Commands: PING 1, SET_RELAY 3, GET_STATUS 4, SET_FAN 5, FAN_HOME 6, CURTAIN 7, DIMMER 8.
ACK: status, relay bits, fan level, fan flags, [curtain %, curtain flags], [dimmer level, dimmer flags].
3 retries, 600 ms timeout. **No authentication yet** (planned: shared key + MAC + replay counter; ask the user
"signing only, or signing + encryption?" before starting).

**The protocol is copy-pasted into every sketch** (the enums near the top of each `.ino`). When you change it,
update the master, every affected node, *and* `server/src/main.rs` (it whitelists actions: led, relay, fan,
curtain, dim, status, scan) plus `server/static/index.html`. Keep the enum values identical.

## Automation server (`server/`)

- axum 0.7 + tokio + SQLite (rusqlite, bundled). Module map and provider setup: `server/README.md`.
  REST/WebSocket contract for the mobile app: `server/API.md`. Env vars: `server/deploy/automation.env.example`.
- Auth: Google/Apple sign-in (web + mobile ID tokens) for allowlisted emails with roles admin/member, plus an
  emergency password (admin). Web = `automation_session` cookie (SameSite=Strict; mutating requests need our
  Origin). App = Bearer access token (15 min) + rotating refresh token (60 days, reuse = family revoked).
  Tokens are stored hashed. Admin-only actions live in `api::required_role`, enforced for REST and WebSocket.
- When adding an action: update `api::build` + `api::required_role`, the master firmware, `static/index.html`,
  and `API.md`.
- Verify without a Rust install: `docker run --rm -v "<repo>/server:/src" -w /src rust:1-bookworm cargo test`
  (and `cargo clippy --all-targets`).
- Deployed on the Pi as systemd service `automation`, binary `~/automation/bin/automation-server` (source copy in `~/automation`, not a git checkout),
  listening on 127.0.0.1:3000; Cloudflare tunnel maps the public hostname to it.
- Deploy flow (**ask first**): cross-compile on the PC with `scripts/build-server-pi.sh` (~1 min, Docker), copy the
  binary to `~/automation/bin/`, keep the old one in a dated backup folder, restart, check the journal. Full steps and
  rollback: `server/README.md`. Do not build on the Pi (30+ minutes on 905 MB RAM).
- `pkill -f <pattern>` over SSH also matches the SSH command's own shell (its command line contains the pattern);
  kill by PID or use `pgrep -x` instead.
- Keep this repo's `server/` identical to what runs on the Pi.

## Conventions

- Comments and serial messages are plain English, short sentences. Match the existing style.
- Each node has a USB serial CLI (115200 baud): `info`, `radio`, plus node-specific commands listed in the
  header comment of each `.ino`. Read that header before scripting a node.
- Generated files are committed next to their generator (perfboard HTML, Gerbers, preview PNG). Regenerate
  with the command in the relevant README/status note instead of hand-editing outputs.
- After hardware tests or plan changes, add a dated entry to `PROJECT_STATUS.md`.
