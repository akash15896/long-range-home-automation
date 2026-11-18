# LoRa home automation — project status

Paths below are relative to `firmware/` unless they start with `server/` (the sketches lived in the Arduino
sketchbook root until 2026-11-18).

## Update 2026-11-16 — curtain node E26D WORKS on the real mechanism (READ FIRST for curtain)
Firmware `LoRa_pico_curtain/LoRa_pico_curtain.ino`, Pico 2 on COM6 (board `rp2040:rp2040:rpipico2`).
Uploaded with `arduino-cli` from `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\`.

Changes uploaded 2026-11-15/16:
- GOTO hunting fix (stop when remaining == 0, `stopDistance + 1` margin). No more ±1-step hunting.
- Real C/NC microswitches mounted at both ends: **C -> GND, NC -> pin, NO unused**. `SWITCH_NC = true`.
  With NC, a broken wire reads as "pressed", so the motor stops rather than running past the end.
- `DIR_REVERSE = false` (with `true`, "close" drove towards the OPEN end).
- **Switch pins swapped in code** because the mounted wires are crossed and too short to swap:
  `OPEN_SWITCH_PIN 11`, `CLOSED_SWITCH_PIN 10`. The info/watch labels were updated to match.
- Re-learned travel: **40340 steps** (~20 s end to end). Safety cutoff = 1.25 × travel + 200 steps (~25 s).

Tested over USB serial on 2026-11-16, all PASSED: goto 50 / 25 / 75 / 50 each landed on the exact step and held.
`close` and `open` stopped on their switches, and `open` ended at exactly 40340, so no steps were lost.
Left at 100 % open.

Lessons:
- Power the stepper driver first. With it off, the motor doesn't move but the firmware still counts steps.
- A belt clamp broke once and the carriage overran a switch. Keep the switches right at the mechanical ends.
- If a serial script that is driving the motor gets killed, it never sends `stop`. Always send `stop` afterwards.
- Run `watch` (20 s, press each lever) after any switch wiring change, before moving the motor.

Later on 2026-11-16 (Claude session):
- The master's scan found E26D (RSSI −54 dBm), and `status E26D` answered over LoRa: "position unknown" (RSSI −50, SNR 9).
  The position had been saved at 100 %, so either the travel data was lost or a move started before a restart.
  An `open` re-homes it on the OPEN switch. Not done yet: needs the user present with the stepper driver powered.
- **Master Wi-Fi was erased by mistake.** A serial script opened COM5 with DTR on, which holds BOOT (GPIO0) low, and after
  10 s the firmware forgot the Wi-Fi. Re-provision with the ESP BLE Provisioning app (`PROV_LoRaMaster`, PIN = `PROV_PIN` in `secrets.h`).
  Scripts must open COM5 with DTR **and** RTS off.
- Wi-Fi re-provisioned by the user; master back on the server.

**LoRa test PASSED (2026-11-16, after the user's setup changes):** from the master, `close` 100→0 in ~22 s (LoRa
answered every 2 s while moving, steps hit 0 exactly at the CLOSED switch), then `50` (14 s), `25` (11 s),
`75` (14 s), `open` (8 s) all landed on target. `open` ended at 40340/40340 on the switch, so no steps were lost. RSSI −38…−40, SNR 9–10.
One glitch: the first `open` after the setup change got no LoRa answers during the move (stop over LoRa also failed),
and the Pico's USB came back as **COM6**. The Pico did not restart (the position was kept, and it finished at 40340 exactly). It did not happen again.
If it comes back, suspect motor noise or a supply dip (grounding, driver supply, wires near the Ra-02).
The curtain is on **COM6** now. COM3 is a different device, held by another program. Left at 100 % open.

**Planned driver swap (2026-11-16):** the user currently runs an HY-DIV268N-5A (1/8 microstep) from a big SMPS, and the motor
is rated 1.2 A. Plan: **DRV8825** (user has one; M0+M1 HIGH, M2 LOW = 1/8, so 40340 steps stay valid; RST+SLP to 3V3; Vref 0.50 V = 1.0 A, max 0.60 V = 1.2 A; heatsink). Alternative TMC2209 (MS1/MS2 GND), Vref about 1.7 V on BTT v1.3
(1.2 A RMS, or about 1.4 V for 1.0 A), a 12 V 1.5-2 A supply, 100-470 µF across VM, a mini buck 12→5 V → SS14 → VSYS, and EN on GP4 with 10 k to 3V3.
Firmware: `#define DRIVER_STEPSTICK 1` (it also sets DRIVER_EN_WIRED 1). Both variants compile. **Not uploaded**: the
board still runs DRIVER_TMC2209 0 for the HY-DIV268N.

**2026-11-17: TMC2209 V2.1 firmware uploaded** (`DRIVER_STEPSTICK 1`, EN on GP4) to COM6. Wiring page:
`LoRa_pico_curtain/tmc2209_wiring.html`, https://claude.ai/artifact/FDMME55nFDbVJCLrwA5cyz (MS1/MS2 GND = 1/8, Vref ~1.4 V set
off-board, A+→A1 A−→A2 B+→B1 B−→B2). After upload `info` showed "0% open, steps 0/41097" with neither switch pressed:
travel is 41097 (not 40340, re-learned since?) and the position is likely not real. Next: `watch`, then `open` to re-home.
`open` then re-homed OK (hit the OPEN switch at ~28-32k counted steps, so the start position was wrong). The next `close`
was stopped by the user: it had counted to **−4360** (≈45k steps from OPEN) and the CLOSED switch never read pressed.
Position now unknown. Find out why before moving again (CLOSED switch / lever, belt slip, or skipped steps with the TMC2209).
A second `close` was also stopped by the user (counted another ~40k steps, no switch). Third try: `close` hit the CLOSED
switch after only ~2.6 s, then `open` ran 0→100 % in ~21.5 s and stopped on the OPEN switch (41097/41097). So the carriage
moved very little during the two stopped closes: suspect stalling/skipping toward CLOSED (Vref too low? belt/pulley grub screw? a
tight spot on the rail). Left at 100 % open.

Next for the curtain:
1. Test from the website (Open / Stop / Close, slider, progress bar).
2. Decide whether to keep the `watch` / `findpins` debug commands.

## Update 2026-11-06 — curtain controls on the website
- Server + page now support curtain nodes: Open / Stop / Close, position slider (goto 0-100), Learn travel
  (with confirm), progress bar, moving/learning/fault chips. Curtain nodes show no relay buttons.
- Server polls `status` every 3 s for curtains that are moving, and asks `status all` once after the master
  connects (so a curtain node is recognised without a manual Refresh).
- Deployed on the Pi (incremental build ~1 min). Backup of the previous source + binary: `~/automation/backup-2026-11-06/`.
- Pico E26D: LoRa module was mounted upside down (read 0x00); fixed. Stepper driver connected; "open" over LoRa
  ran 0→100 % in ~20 s. Watch for dropped commands / USB resets while the motor runs (grounding / noise).

Last updated: 2026-11-18 (moved into the git repo; curtain update at the top; see also "4-channel carrier PCB" at the end).
To resume, open the repo and say "read PROJECT_STATUS.md and continue". Build/upload/safety notes: `AGENTS.md`.

## Update 2026-11-03 — Raspberry Pi Pico 2 node WORKS

New sketch `LoRa_pico_node/` (Nano sketch left unchanged): same protocol, SX1278 wired directly (3.3 V,
no converter): MISO GP16, NSS GP17, SCK GP18, MOSI GP19, DIO0 GP20, RST GP21, VCC 3V3(OUT) pin 36, GND pin 38.
SPI 1 MHz, TX 17 dBm, sync 0x12. Node ID from the chip's unique ID: this Pico is **E26D** (USB COM6).
Relays GP2–GP5 (not connected yet), fan GP6–GP9 with HAS_FAN 0. Serial commands: `info`, `radio`.

Tested 2026-11-03 with the master: scan found E26D (RSSI −14 dBm), 10/10 pings, relay on/toggle/status/off all
answered. 15-min soak test (2026-11-03): 299/299 commands answered, 0 misses, 0 Pico restarts, RSSI −19…−36 dBm,
SNR avg 9.3 dB (brief dip to 2 dB around 13:52, recovered).
DIO0 on GP20 works (it needed `pinMode(INPUT_PULLDOWN)`: RP2350 pins are isolated until set up, and the
LoRa library never does it). The SPI RxDone poll every 5 ms stays as a fallback for a loose DIO0 wire.
Uploads: board package rp2040:rp2040 6.3.2, FQBN rp2040:rp2040:rpipico2. The very first UF2 copy to the
RP2350 drive was silently dropped; `picotool load -v -x <elf>` worked. Later uploads via COM6 worked normally; if the IDE is open, build with `--build-path` in another folder.

## ESP32 node (2026-11-03) — WORKS, and the Nano's radio module is FAULTY

New sketch `LoRa_esp32_node/` (copy of the Pico node for a second ESP32 dev board, on **COM7**, node ID **77CA**).
Same LoRa pins as the master: NSS 5, RST 14, DIO0 2, SCK 18, MISO 19, MOSI 23. Relays 25/26/27/32,
fan 16/17/21/22 (HAS_FAN 0). Node ID from the factory MAC.
- This board misreads its flash at 80 MHz ("partition table error" boot loop): build with
  FQBN `esp32:esp32:esp32:FlashFreq=40,FlashMode=dio,UploadSpeed=115200` (IDE: Tools > Flash Frequency 40MHz).
  Needs BOOT held for uploads.
- With the module taken from the Nano, the version register read 0x00 (once 0x07). With the Pico's module on
  the same wires: starts first try, 10/10 pings, relay toggle OK, DIO0 fine.
  **Conclusion: the Nano's SX1278 module is faulty** — this explains the Nano's degrading SPI on 2026-10-31,
  not (only) the BSS138 converter. Replace that module.
- Currently the only good module is on the ESP32 node; the Pico has no module (E26D offline).

## Where we stopped (2026-10-31, evening) — READ FIRST

**Problem:** the Nano can no longer talk reliably to its SX1278 over SPI, and it gets worse over time.
Probe sketch (reads version register 0x42, should be 0x12; sketch `LoRa_id_probe/`): this morning 50/125/250 kHz all clean → evening 125 kHz flaky → finally even 50 kHz gives
`10 C 0 0 0...`. Once one transfer is garbled, every following read returns 0 until the radio is reset.
`LoRa.begin()` then fails ("Starting LoRa failed! Check Wiring").
- Before it broke completely: the node heard the master's scan, answered, and then heard nothing more
  (deaf right after its own transmit). Master got 0/15 replies. Lowering node TX power to 5 dBm didn't help.
- Ruled out: relays/stepper (never connected in any test), radios/antennas, frequency, master.
- **Cause is physical** (software can't degrade over time at the same SPI speed): loose breadboard/jumper
  contact on NSS/SCK/MOSI/GND, 3.3 V sagging (AMS1117 hot or bad contact), or a failing BSS138 converter channel.

**Next steps, in order:**
1. User checks: AMS1117 / converter warm? wires loose? measure LoRa 3.3 V pin while running; re-run the ID probe
   while wiggling NSS/SCK/MOSI wires.
2. Recommended fix: replace the BSS138 converter with 1 kΩ (series, from Nano) + 2 kΩ (to GND) dividers on NSS,
   SCK, MOSI, RST; add 100 µF across LoRa 3.3 V/GND. Alternative: move the node to a 3.3 V board (Pi Pico).
3. Then re-upload `LoRa_nano_node` and **restore test values**: `LORA_SPI_HZ` is 50000 (back to 125000, or 1–4 MHz
   with dividers) and `LORA_TX_POWER` is 5 (back to 17).
4. Link test: master serial `scan`, then `ping/status/relay 253E ...` ×15 (script: open COM5 115200 + COM4 9600,
   send lines, count "did not answer"). Then fan, persistence, website, adapter-only power.
5. Update the wiring artifact (still shows the old converter variant) and this file.

**Code changes made this evening (both sketches):**
- Sync word 0xF3 → **0x12** (library default) on master and node — both must match.
- Node: removed the once-a-second register check (`radioSettingsOk`/`readRadioRegister`) — its reads over the
  marginal converter could themselves garble registers; removed explicit BW/SF/CR/preamble calls (begin() sets
  the same defaults); reply delay 20 → 50 ms. Still: CRC on, binary protocol, DIO0-driven receive,
  re-init after 60 s silence.
- Master: still sets BW/SF/CR/preamble explicitly (harmless, ESP32 is 3.3 V, no converter).

## What exists

| Part | Where | State |
|---|---|---|
| ESP32 master sketch | `LoRa_esp32_master/` | Uploaded 2026-10-31 evening (sync word 0x12); connected to the server |
| Nano node sketch | `LoRa_nano_node/` | **Not on the Nano right now** — the Nano runs the `id_probe` test sketch. Radio link broken (see "Where we stopped") |
| Original test sketches | `LoRa_esp32_transmitter/`, `LoRa_nano_receive/` | Unchanged |
| Automation server (Rust) | `server/` in this repo; live on the Pi in `~/automation` | **Running** at https://automation.akashmahor.me |

### ESP32 master (`LoRa_esp32_master`)
- Board: ESP32 on **COM5** (CP210x). Core esp32:esp32 3.3.12.
- Tools → Partition Scheme → **Huge APP (3MB No OTA/1MB SPIFFS)** (FQBN option `PartitionScheme=huge_app`).
- Libraries: LoRa 0.8.0, WebSockets 2.6.1 (links2004), ArduinoJson 7.2.1.
- Wi‑Fi via BLE provisioning (Espressif "ESP BLE Provisioning" app): device `PROV_LoRaMaster`, PIN = `PROV_PIN` in `secrets.h`
  (change it from the old default). Hold BOOT 10 s to erase Wi‑Fi.
- Connects to `wss://automation.akashmahor.me/ws/device` with a Bearer token from `secrets.h` (keep private).
  `root_ca.h` holds ISRG X1/X2 + GTS R1/R4 roots.
- Status LED: **GPIO 4** → 220 Ω → LED → GND.
- Serial commands: `scan`, `list`, `ping`, `status <id|all>`, `relay <id|all> <1-4|all> <on|off|toggle>`,
  `fan <id> <0-100|home>`, `led <on|off>`, `wifi`, `wifi reset`, `server`.

### Nano node (`LoRa_nano_node`)
- Relays on D4–D7, **all active LOW**. Relay state saved in EEPROM and restored after power loss.
- Random node ID stored in EEPROM (or set `FIXED_NODE_ID`).
- **Wiring that works (verified 2026-10-31, node 253E):** 5 V adapter → Nano 5V pin; from that same 5 V rail:
  level converter HV, AMS1117 IN (→ 3.3 V for LoRa VCC + converter LV), ULN2003 +, relay JD‑VCC; all GNDs common.
  BSS138 converter on D10/D13/D11 → NSS/SCK/MOSI only. **MISO→D12, DIO0→D2 and RST→D9 are direct wires** (sketch
  only pulls RST low, never drives it high). **Node SPI = 125 kHz** (`LORA_SPI_HZ`): the converter garbles SPI above
  ~250 kHz and garbled reads become writes that corrupt radio settings (radio goes deaf). Never power LoRa from
  the Nano's 3V3 pin. (Evening 2026-10-31: this setup degraded — see "Where we stopped".)
  TX power 17 dBm on both (fine on one desk). Master serial: `txpower <2-20>`, `radio` (register dump).
  Link test sketches: `LoRa_hello_master` / `LoRa_hello_node` (library defaults, hello/ack every 2 s).
  For future nodes prefer a 3.3 V board (Raspberry Pi Pico) or resistor dividers instead of the BSS138 converter.
  Wiring diagram: https://claude.ai/artifact/Uftz4GkYznsMjbyzcq9xF2
- A classic 5 V Nano needs a level shifter (BSS138) on NSS/SCK/MOSI/RST, a 3.3 V regulator (AMS1117) for the
  LoRa module, and a 5 V supply for relay JD‑VCC. Consider `LoRa.setSPIFrequency(1E6)`.
- **Fan regulator (added 2026-10-31, not yet tested on hardware):** 28BYJ-48 + ULN2003 on A0–A3 (IN1–IN4),
  turning a smooth (no-click) regulator knob, level 0–100 %. Enable with `#define HAS_FAN 1` on that node only.
  Tune `FAN_TRAVEL_DEGREES` (default 270), `FAN_REVERSE`, `FAN_STEP_US`. No sensor: at power-up it drives past
  OFF against the end stop (homing), then returns to the level saved in EEPROM[5..6]. "Re-home" recalibrates.

### LoRa protocol
433 MHz, sync word 0x12 (was 0xF3 until 2026-10-31 evening), CRC on. Packet: MAGIC 0xA7, type, dst(2), src(2), seq, len, data.
Master 0x0000, broadcast 0xFFFF. Types: DISCOVER 1, HELLO 2, WELCOME 3, CMD 4, ACK 5.
Commands: PING 1, SET_RELAY 3 (relay 1–4 or 0=all; state 0 off/1 on/2 toggle), GET_STATUS 4,
SET_FAN 5 (level 0–100), FAN_HOME 6. Status: OK 0, UNKNOWN_CMD 1, BAD_RELAY 2, NO_FAN 3.
ACK data = [status, relayBits, fanLevel, fanFlags (1 present, 2 homing)]; 3 retries, 600 ms timeout. **No signing yet** (deferred).

## Raspberry Pi
Host details, SSH and sudo notes are in `LOCAL_NOTES.md` (gitignored). Deploy steps: `server/README.md`.

## Next steps
1. ~~Upload master + node, test end to end~~ — done (user confirmed 2026-10-31: website commands reach the Nano).
1a. Fan regulator: server change deployed 2026-10-31. Still to do: upload master + all nodes (new ACK format), set HAS_FAN 1 on
    the fan node, measure knob travel, test homing direction.
2. Add HTTP polling as a fallback to the WebSocket link (requested, not built).
3. Optional: replace Apache with nginx + uvicorn for all sites (plan agreed in outline, not started).
4. Later: LoRa message signing + replay counter (ask: signing only, or signing + encryption?).
5. Remove password-free sudo on the Pi when done (see `LOCAL_NOTES.md`).

## Open questions
- Exact Nano model (classic 5 V vs 3.3 V variant) was never confirmed.

## Pico node production board (2026-11-03)
- Perfboard design rev C (2026-11-04, Pico upright with USB at the top edge; rev B had USB facing the mains side): `LoRa_pico_node/perfboard.html`, regenerate with `python gen_perfboard_c.py perfboard_template_c.html perfboard.html`, published at https://claude.ai/artifact/2rBMVZDff5NSyQxokXP7DY (15 x 9 cm, HLK-10M05 + Ra-02 breakout on board, mains zone on the left).
- Relay 4 (GP5) = fan power; fan speed = 28BYJ-48 stepper on the regulator knob (GP6-9, HAS_FAN 1 on that node).
- Status LED mirrored on GP15 (uploaded, works).
- Pico relay outputs changed to open-drain for active-LOW relays (RP2350 GPIO is 5 V tolerant), so a stock 5 V relay module with JD-VCC jumper works. Compiled, NOT yet uploaded/tested (boards were unplugged). Do not do this on the ESP32 node.
- 30-min two-node soak: E26D 356/359 (3 misses while wiring), 77CA 148/148 after power-up.

## Dimmer node (2026-11-14)

- `pico_ac_dimmer/`: standalone button dimmer on the original Pico (RP2040, COM8). Tested with mains and a bulb: works at every level.
- `LoRa_pico_dimmer/`: the same dimmer as a LoRa node, no relays. (pins later moved: Z-C GP26, light gate GP27, fan gate GP28), LED GP15, LoRa on the usual GP16-21. No brightness button (HAS_BUTTON 0); the panel push switch is the reset button on J7 (NO to RUN, C to GND). CMD_DIMMER = 8; the ACK is 8 bytes (level in byte 6, flags in byte 7). Uploaded 2026-11-14.
- Master: `dim <id|all> <0-100|on|off|next>` and JSON action "dim" (state set/next/on/off, level). Uploaded 2026-11-14.
- The server and web UI support "dim" (deployed 2026-11-14).
- Perfboard rev D: `LoRa_pico_dimmer/perfboard.html`, published at https://claude.ai/artifact/XxS4mkmjZsQHNrqRXD2ysf.
- 2026-11-14 update: both dimmer channels are used now. Light on L1 (GP27, `dim`) and fan on L2 (GP28, the existing `fan` command and the website fan slider). Z-C moved to GP26. (Uploaded later the same day, see "Where we stopped" below.) The sheet now shows the as-built board: the Pico is mirrored under the board and the Ra-02 is wired and working.

### Where we stopped (2026-11-14, evening): dimmer node READ FIRST
Powered down to mount the board in the mains switchboard.

Working and tested:
- Node firmware uploaded to the Pico (RP2040, COM8). **Node ID E29E.**
- Master firmware (with `dim`) uploaded to the ESP32 (COM5). Wi-Fi and the automation server are connected.
- From the master: `scan` finds E29E (about −30 to −50 dBm, SNR ~9.8), `ping`, `dim E29E <0-100|next>` and `fan E29E <0-100>` are all ACKed.
- The board runs on AC alone (Hi-Link 5 V → D1 → VSYS). LoRa TX is at 17 dBm (`txtest` is clean at 17 and 20).
- Reset button J7 (2-leg switch, pin 28 GND to pin 30 RUN): pressing it reboots the node, and it rejoins the master.
- Saved levels: light 40 %, fan 0 %. The bulb lights at 40 % as soon as mains and zero-cross are present.

Problems found and fixed today:
- **A disconnected ground on the perfboard** caused LoRa transmit brownouts (SPI corruption at 10+ dBm) and wrong 3V3 readings (4.8 V on AC, under 3 V on USB). Fixed by the user.
- The Ra-02 booted with stale settings (SF6 / CR 4/6). The node now sets SF7 / 125 kHz / CR 4/5 / preamble 8 explicitly, like the master.
- Wiring the reset switch while powered locked up the Ra-02 (version read 0x00). A full power-off fixed it. **Always power off (AC and USB) before soldering or connecting anything.**
- The firmware sets GP23 HIGH at boot (Pico regulator in PWM mode, steadier 3V3). It's harmless, so keep it.

Still to do:
1. **Zero-cross not detected:** the node reports "NO MAINS". Connect the dimmer's AC-L/AC-N input, VCC → pin 36 (3V3), GND → pin 38 and Z-C → pin 31 (GP26). Then check `info` (should show 50 Hz pulses) and test the bulb (L1) and fan (L2) from the master.
2. ~~Website: add the "dim" action to the server~~ — done 2026-11-14 (main.rs has "dim", the page shows a Light card for dimmer nodes).
3. Optional: label the unknown parts on the as-built sheet (5-pin socket, blue part, brown round part, white JST).
4. Status LED: R1 is 330 Ω. With a blue, white or green LED, use about 100 Ω for more brightness.

Node serial commands (USB, 115200): `info`, `on`, `off`, `next`, `0-100` (light), `fan N`, `radio`, `rxlog`, `txpower N`, `txtest`, `pwm on/off`.
Upload: `arduino-cli compile/upload --fqbn rp2040:rp2040:rpipico` in `LoRa_pico_dimmer/` (port COM8).

## 4-channel carrier PCB, rev A (2026-11-15)
- Folder: `LoRa_pico_dimmer/pcb_4ch/`. Run `python gen_pcb.py` to regenerate `gerbers/`, `LoRa_pico_dimmer_4ch_rev_A_gerbers.zip` (the file to upload to JLCPCB/PCBWay), `preview.png`. Full details in `pcb_4ch/README.md`.
- Board: 2 layers, 108 x 84 mm, 1.6 mm, 1 oz, HASL. It carries the Pico, the Ra-02 (bare module soldered on), the HLK-10M05 with fuse + MOV, and J3 (3V3, GND, ZC, CH1-CH4), which goes to an **off-board 4-channel RobotDyn-style dimmer module** (loads < 2 A/channel).
- Pins: ZC GP26, CH1 GP27, CH2 GP28 (unchanged), **CH3 GP22 (pin 29), CH4 GP13 (pin 17)**.
- The script's own DRC passes (8 mm mains-to-LV) and gerbonara parses all the files. KiCad is installed now (2026-11-15) but the Gerbers have not been opened in GerbView yet.
- Not ordered yet. Before ordering: open the files in GerbView, measure the HLK pin spacing (AC pins 7.8 mm, DC pins 22.8 mm, AC end to DC end 42.5 mm), and plan an IPEX 433 MHz antenna + nylon screws on the left holes.
- **Firmware/master/website still handle only 2 channels.** CH3/CH4 need adding to `LoRa_pico_dimmer.ino`, the master and the server/web UI (not started).
