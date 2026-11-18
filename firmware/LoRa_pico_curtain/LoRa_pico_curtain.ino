// LoRa curtain node for the Raspberry Pi Pico 2 (RP2350). Drives a NEMA 17 stepper through an
// HY-DIV258N driver (TB6600-style opto inputs) and stops on two limit switches:
// one pressed when the curtain is fully OPEN, one pressed when it is fully CLOSED.
// No relays, no fan. Speaks the same protocol as the other nodes, plus CMD_CURTAIN.
//
// Board package: "Raspberry Pi Pico/RP2040/RP2350" by Earle Philhower; board "Raspberry Pi Pico 2".
// Serial monitor: USB, any baud rate. Type "help" for test commands (open, close, goto 50 ...).
//
// Position is counted in motor steps from the CLOSED switch (0) to the OPEN switch (travel).
// "learn" runs closed -> open once and stores the travel in flash; after that the node can go to
// any percentage. A "goto" before the first learn runs the learn first, then goes there.
// Percent: 0 = fully closed, 100 = fully open.
//
// The motor is stepped by the RP2350's second core (setup1/loop1), so LoRa traffic on the
// first core never makes it stutter.

#include <SPI.h>
#include <LoRa.h>
#include <EEPROM.h>
#include <pico/unique_id.h>

// ---- LoRa wiring: identical to LoRa_pico_node ----
//   SX1278   Pico 2
//   3.3V     3V3(OUT)  pin 36      (+ 100 uF capacitor across 3.3V/GND at the module)
//   GND      GND       pin 38
//   MISO     GP16      pin 21
//   NSS      GP17      pin 22
//   SCK      GP18      pin 24
//   MOSI     GP19      pin 25
//   DIO0     GP20      pin 26
//   RST      GP21      pin 27
#define LORA_SS 17
#define LORA_RST 21
#define LORA_DIO0 20
#define LORA_SPI_HZ 1000000

#define LORA_BAND 433E6
#define LORA_SYNC_WORD 0x12   // must match the master
#define LORA_TX_POWER 17      // dBm (2-20)

// Set to a value from 0x0001 to 0xFFFE to choose this node's ID yourself; 0 = from the chip ID.
#define FIXED_NODE_ID 0

// ---- HY-DIV258N stepper driver ----
// Common-cathode wiring (default, DRIVER_COMMON_ANODE 0), EN not used (DRIVER_EN_WIRED 0):
//   PUL+  -> GP2  (pin 4)
//   DIR+  -> GP3  (pin 5)
//   PUL-, DIR-, EN+, EN-  -> GND
// HIGH = opto on. The pins are set to 12 mA drive, because only 3.3 V reaches the optos.
// With EN unused the driver is always enabled: the motor holds (and gets warm) when stopped.
// To let the node switch the motor off when idle, wire EN+ -> GP4 (pin 6), EN- -> GND and set
// DRIVER_EN_WIRED 1.
// Common-anode wiring (DRIVER_COMMON_ANODE 1) gives the optos a full 5 V if 3.3 V proves too weak:
//   PUL+, DIR+, EN+ -> 5 V;  PUL- -> GP2, DIR- -> GP3, EN- -> GP4
// The pins are then driven open-drain (LOW = opto on, released = off). Safe because the RP2350's
// GPIOs are 5 V tolerant (do NOT copy this to an ESP32).
// Driver power: VCC/GND from a 12-24 V supply (NOT the 5 V rail). Motor coils to A+/A-/B+/B-.
//
// ---- StepStick module: DRV8825 / TMC2209 / A4988 (set DRIVER_STEPSTICK 1) ----
//   STEP -> GP2 (pin 4), DIR -> GP3 (pin 5), EN -> GP4 (pin 6) + 10 k pull-up to 3V3 (motor off while booting)
//   VMOT/GND -> 12 V supply, 100-470 uF >= 25 V across VMOT/GND right at the module (spikes kill these chips)
//   Logic GND -> Pico GND (common ground with the 12 V supply).
//   DRV8825: RST and SLP -> 3V3, FLT unconnected. 1/8 microstep = M0 HIGH, M1 HIGH, M2 LOW (3V3/3V3/GND),
//            the same as the HY-DIV268N DIP setting, so the learned 40340 steps stay valid.
//            Current limit = 2 x Vref (0.1 ohm sense). Motor rated 1.2 A -> Vref max 0.60 V; 0.50 V (1.0 A) runs
//            cooler. Set it before connecting the motor; fit the heatsink (it gets hot above ~1 A).
//   TMC2209: VIO -> 3V3, MS1/MS2 -> GND = 1/8; Vref ~1.7 V = 1.2 A RMS on BTT v1.3.
//   EN is active LOW on all of them, which the common-cathode + EN_OPTO_ON_DISABLES logic below matches.
#define DRIVER_STEPSTICK 1
#if DRIVER_STEPSTICK
#define DRIVER_COMMON_ANODE 0
#define DRIVER_EN_WIRED 1
#else
#define DRIVER_COMMON_ANODE 0
#define DRIVER_EN_WIRED 0
#endif
#define PUL_PIN 2
#define DIR_PIN 3
#define EN_PIN 4
// TB6600-style drivers free the motor while the EN opto is on. Set false if yours is the other way
// round (motor only holds/turns while EN- is connected).
const bool EN_OPTO_ON_DISABLES = true;
// Swap to true if "open" turns the motor the wrong way.
const bool DIR_REVERSE = false;  // 2026-11-15: with switches mounted, "close" must move toward the CLOSED switch
// true = keep the motor energised when stopped (holds the curtain, but the motor gets warm).
const bool HOLD_WHEN_IDLE = false;

// Speeds in steps per second, counting microsteps. Set the driver's DIP switches first; e.g. at
// 1/8 microstepping a 200-step motor needs 1600 steps per turn, so 2000 steps/s = 1.25 turns/s.
const float START_SPEED = 300;    // speed it starts and stops at (no ramp needed below this)
const float MAX_SPEED = 2000;     // cruising speed; lower it if the motor stalls
const float ACCEL = 2000;         // steps/s per second; lower = gentler start and stop
const uint16_t PULSE_US = 10;     // pulse width; the optos need a few microseconds

// Safety stop: if a switch isn't reached within this many steps, the motor stops and reports a
// fault. Before learning, MAX_UNLEARNED_STEPS is used; after, the learned travel + 25 %.
const uint32_t MAX_UNLEARNED_STEPS = 200000;
const uint32_t MIN_TRAVEL_STEPS = 200;   // a shorter learned travel = switches wired wrong

// ---- limit switches ----
// Each switch between its GPIO and GND; the Pico's internal pull-up does the rest.
//   OPEN switch   -> GP11 (pin 15) and GND   (swapped 2026-11-16 to match the mounted wiring)
//   CLOSED switch -> GP10 (pin 14) and GND
// Wire the switches' NC (normally closed) contacts: the pin reads LOW until the switch is pressed,
// and a broken wire reads as "pressed", so the motor stops instead of running past the end.
// Set SWITCH_NC to false if you use the NO contacts instead.
#define OPEN_SWITCH_PIN 11
#define CLOSED_SWITCH_PIN 10
const bool SWITCH_NC = true;    // real C+NC microswitches: C -> GND, NC -> pin, NO unused.

// ---- protocol (keep identical in LoRa_esp32_master) ----
const uint8_t MAGIC = 0xA7;
const uint16_t MASTER_ID = 0x0000;
const uint16_t BROADCAST_ID = 0xFFFF;

enum MsgType : uint8_t { MSG_DISCOVER = 1, MSG_HELLO = 2, MSG_WELCOME = 3, MSG_CMD = 4, MSG_ACK = 5 };
// CMD_CURTAIN data: action (see CurtainAction), percent (0-100, only for CURTAIN_GOTO).
// ACK data for every command: status, relay bits (always 0), fan level (0), fan flags (0),
// curtain position (0-100, 0xFF = not known yet), curtain flags (see CurtainFlag).
enum Command : uint8_t {
  CMD_PING = 1, CMD_SET_RELAY = 3, CMD_GET_STATUS = 4, CMD_SET_FAN = 5, CMD_FAN_HOME = 6, CMD_CURTAIN = 7
};
enum CmdStatus : uint8_t {
  STATUS_OK = 0, STATUS_UNKNOWN_CMD = 1, STATUS_BAD_RELAY = 2, STATUS_NO_FAN = 3, STATUS_NO_CURTAIN = 4,
  STATUS_CURTAIN_FAULT = 5
};
enum CurtainAction : uint8_t {
  CURTAIN_STOP = 0, CURTAIN_OPEN = 1, CURTAIN_CLOSE = 2, CURTAIN_GOTO = 3, CURTAIN_LEARN = 4
};
enum CurtainFlag : uint8_t {
  CURTAIN_PRESENT = 1, CURTAIN_MOVING = 2, CURTAIN_KNOWN = 4, CURTAIN_LEARNED = 8,
  CURTAIN_AT_OPEN = 16, CURTAIN_AT_CLOSED = 32, CURTAIN_FAULT = 64, CURTAIN_LEARNING = 128
};

// On air: MAGIC, type, dst(2), src(2), seq, len, data[len]
const uint8_t HEADER_LEN = 8;
const uint8_t MAX_DATA = 32;

struct Packet {
  uint8_t type;
  uint16_t dst;
  uint16_t src;
  uint8_t seq;
  uint8_t len;
  uint8_t data[MAX_DATA];
};

void sendPacket(const Packet &p) {
  LoRa.beginPacket();
  LoRa.write(MAGIC);
  LoRa.write(p.type);
  LoRa.write(p.dst >> 8);
  LoRa.write(p.dst & 0xFF);
  LoRa.write(p.src >> 8);
  LoRa.write(p.src & 0xFF);
  LoRa.write(p.seq);
  LoRa.write(p.len);
  LoRa.write(p.data, p.len);
  LoRa.endPacket();
}

bool receivePacket(Packet &p) {
  int size = LoRa.parsePacket();
  if (size == 0) return false;

  bool ok = size >= HEADER_LEN && LoRa.read() == MAGIC;
  if (ok) {
    p.type = LoRa.read();
    p.dst = LoRa.read() << 8;
    p.dst |= LoRa.read();
    p.src = LoRa.read() << 8;
    p.src |= LoRa.read();
    p.seq = LoRa.read();
    p.len = LoRa.read();
    ok = p.len <= MAX_DATA && p.len == size - HEADER_LEN;
  }
  if (ok) {
    for (uint8_t i = 0; i < p.len; i++) p.data[i] = LoRa.read();
  }
  while (LoRa.available()) LoRa.read();   // drop anything left of a bad packet
  return ok;
}
// ---- end protocol ----

const uint16_t SLOT_MS = 60;   // must match the master

// EEPROM (kept in a flash sector; commit() pauses the other core, so it only happens while the
// motor is stopped). [10] marker, [11-14] travel; [15] marker (0 = position unknown), [16-19] position.
const size_t EEPROM_SIZE = 256;
const int EEPROM_TRAVEL_ADDR = 10;
const uint8_t EEPROM_TRAVEL_MARKER = 0xC1;
const int EEPROM_POS_ADDR = 15;
const uint8_t EEPROM_POS_MARKER = 0xC2;

uint16_t nodeId;
uint8_t registeredScan = 0;
uint8_t lastCmdSeq = 0;
Packet lastAck;

// ================= motor (second core) =================

enum Mode : uint8_t { MODE_IDLE, MODE_OPEN, MODE_CLOSE, MODE_GOTO, MODE_LEARN_CLOSE, MODE_LEARN_OPEN };
enum Fault : uint8_t { FAULT_NONE, FAULT_NO_SWITCH, FAULT_BOTH_SWITCHES, FAULT_SHORT_TRAVEL };
const uint8_t NO_PERCENT = 0xFF;

// Written by core 0, read by core 1: a new request is announced by bumping reqSeq last.
volatile uint32_t reqSeq = 0;
volatile uint8_t reqAction = CURTAIN_STOP;
volatile uint8_t reqPercent = 0;
volatile bool motorReady = false;   // core 1 waits for core 0 to load the saved state

// Written by core 1, read by core 0.
volatile int32_t pos = 0;           // steps from the CLOSED switch
volatile bool posKnown = false;
volatile uint32_t travel = 0;       // steps from CLOSED to OPEN, 0 = not learned yet
volatile uint8_t mode = MODE_IDLE;
volatile bool moving = false;
volatile bool atOpen = false, atClosed = false;
volatile uint8_t fault = FAULT_NONE;
volatile uint32_t motionCount = 0;  // bumped every time the motor stops; core 0 saves then

// Core 1 only.
int32_t gotoTarget = 0;
uint8_t thenGoto = NO_PERCENT;      // percentage to go to once the CLOSED switch / learn is done
int8_t curDir = 0;
float speed = 0;
uint32_t nextStepAt = 0;
uint32_t stepsThisMove = 0;
uint32_t idleSince = 0;
bool driverOn = false;

void optoSet(uint8_t pin, bool on) {
#if DRIVER_COMMON_ANODE
  gpio_set_dir(pin, on);   // output (driving LOW) = opto on; input = released
#else
  gpio_put(pin, on);
#endif
}

void setupDriverPins() {
  const uint8_t pins[3] = { PUL_PIN, DIR_PIN, EN_PIN };
  for (uint8_t pin : pins) {
    if (pin == EN_PIN && !DRIVER_EN_WIRED) continue;   // GP4 stays free
    gpio_init(pin);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);   // optos need ~8-10 mA
    gpio_disable_pulls(pin);
    gpio_put(pin, 0);
#if DRIVER_COMMON_ANODE
    gpio_set_dir(pin, GPIO_IN);           // released = opto off
    gpio_set_input_enabled(pin, false);   // no input buffer = no leakage at 5 V
#else
    gpio_set_dir(pin, GPIO_OUT);
#endif
  }
}

void driverEnable(bool on) {
  if (on == driverOn) return;
  driverOn = on;
  if (!DRIVER_EN_WIRED) return;   // driver always enabled
  optoSet(EN_PIN, EN_OPTO_ON_DISABLES ? !on : on);
  if (on) delayMicroseconds(2000);   // let the driver wake up before the first pulse
}

// Debounced switches: a change only counts after 4 matching samples 250 us apart (1 ms).
bool switchPressed(uint8_t pin) {
  bool high = digitalRead(pin) == HIGH;
  return SWITCH_NC ? high : !high;
}

void readSwitches() {
  static uint32_t lastSample = 0;
  static uint8_t openCount = 0, closedCount = 0;
  uint32_t now = micros();
  if (now - lastSample < 250) return;
  lastSample = now;

  bool o = switchPressed(OPEN_SWITCH_PIN);
  bool c = switchPressed(CLOSED_SWITCH_PIN);
  openCount = (o != atOpen) ? openCount + 1 : 0;
  closedCount = (c != atClosed) ? closedCount + 1 : 0;
  if (openCount >= 4) { atOpen = o; openCount = 0; }
  if (closedCount >= 4) { atClosed = c; closedCount = 0; }
}

uint32_t maxStepsForMove() {
  if (mode == MODE_LEARN_OPEN || mode == MODE_LEARN_CLOSE || travel == 0) return MAX_UNLEARNED_STEPS;
  return travel + travel / 4 + 200;
}

void stopNow() {
  moving = false;
  speed = 0;
  curDir = 0;
  motionCount = motionCount + 1;
}

void setFault(uint8_t f) {
  fault = f;
  posKnown = false;
  mode = MODE_IDLE;
  thenGoto = NO_PERCENT;
}

// Start a GOTO towards a percentage; 0 and 100 run all the way to the switch so the ends are exact.
void startGoto(uint8_t percent) {
  if (percent == 0) { mode = MODE_CLOSE; return; }
  if (percent >= 100) { mode = MODE_OPEN; return; }
  if (travel == 0) { mode = MODE_LEARN_CLOSE; thenGoto = percent; return; }
  if (!posKnown) { mode = MODE_CLOSE; thenGoto = percent; return; }
  gotoTarget = (int32_t)((uint64_t)travel * percent / 100);
  mode = MODE_GOTO;
}

void takeRequest() {
  static uint32_t seenSeq = 0;
  if (reqSeq == seenSeq) return;
  seenSeq = reqSeq;
  __sync_synchronize();
  uint8_t action = reqAction, percent = reqPercent;

  thenGoto = NO_PERCENT;
  if (action != CURTAIN_STOP) fault = FAULT_NONE;
  switch (action) {
    case CURTAIN_OPEN: mode = MODE_OPEN; break;
    case CURTAIN_CLOSE: mode = MODE_CLOSE; break;
    case CURTAIN_GOTO: startGoto(percent); break;
    case CURTAIN_LEARN: mode = MODE_LEARN_CLOSE; break;
    default: mode = MODE_IDLE; break;   // stop: ramps down, then idles
  }
}

// Reached the switch at the end we were heading for.
void arrived(int8_t dir) {
  if (dir < 0) {   // CLOSED switch
    pos = 0;
    posKnown = true;
    if (mode == MODE_LEARN_CLOSE) { mode = MODE_LEARN_OPEN; return; }
    if (thenGoto != NO_PERCENT) {
      uint8_t p = thenGoto;
      thenGoto = NO_PERCENT;
      startGoto(p);
      return;
    }
  } else {         // OPEN switch
    if (mode == MODE_LEARN_OPEN) {
      if ((uint32_t)pos < MIN_TRAVEL_STEPS || !posKnown) { setFault(FAULT_SHORT_TRAVEL); return; }
      travel = pos;
      if (thenGoto != NO_PERCENT) {
        uint8_t p = thenGoto;
        thenGoto = NO_PERCENT;
        startGoto(p);
        return;
      }
    } else if (travel) {
      pos = travel;
      posKnown = true;
    }
  }
  mode = MODE_IDLE;
}

void setup1() {
  while (!motorReady) delay(1);
}

void loop1() {
  takeRequest();
  readSwitches();

  if (atOpen && atClosed) {   // both pressed: wiring problem, never move
    if (moving) stopNow();
    if (fault != FAULT_BOTH_SWITCHES) setFault(FAULT_BOTH_SWITCHES);
    driverEnable(HOLD_WHEN_IDLE);
    return;
  }
  if (fault == FAULT_BOTH_SWITCHES) fault = FAULT_NONE;   // cleared by itself once fixed

  // Which way the current mode wants to go, and (for GOTO) how far is left.
  int8_t want = 0;
  uint32_t remaining = UINT32_MAX;
  switch (mode) {
    case MODE_OPEN:
    case MODE_LEARN_OPEN: want = 1; break;
    case MODE_CLOSE:
    case MODE_LEARN_CLOSE: want = -1; break;
    case MODE_GOTO: {
      int32_t diff = gotoTarget - pos;
      want = diff > 0 ? 1 : diff < 0 ? -1 : 0;
      remaining = diff >= 0 ? diff : -diff;
      break;
    }
    default: break;
  }

  uint32_t now = micros();
  if (!moving) {
    if (mode == MODE_GOTO && want == 0) mode = MODE_IDLE;   // already there
    if (want != 0 && (want > 0 ? atOpen : atClosed)) { arrived(want); return; }
    if (want == 0) {
      // Standing on a switch tells us where we are, even after a restart.
      if (atClosed) { pos = 0; posKnown = true; }
      else if (atOpen && travel) { pos = travel; posKnown = true; }
      if (!HOLD_WHEN_IDLE && driverOn && millis() - idleSince > 500) driverEnable(false);
      return;
    }
    // Start moving.
    driverEnable(true);
    optoSet(DIR_PIN, (want > 0) != DIR_REVERSE);
    delayMicroseconds(50);   // direction must settle before the first pulse
    curDir = want;
    speed = START_SPEED;
    stepsThisMove = 0;
    nextStepAt = now;
    moving = true;
    return;
  }

  if ((int32_t)(now - nextStepAt) < 0) return;

  // Switch in the direction of travel: stop at once and take it as the reference.
  if (curDir > 0 ? atOpen : atClosed) {
    int8_t d = curDir;
    stopNow();
    idleSince = millis();
    if (want == d) arrived(d);
    else if (d < 0) { pos = 0; posKnown = true; }
    else if (travel) { pos = travel; posKnown = true; }
    return;
  }

  // GOTO target reached: stop here (the ramp has already brought the speed down).
  if (mode == MODE_GOTO && remaining == 0) {
    stopNow();
    idleSince = millis();
    mode = MODE_IDLE;
    return;
  }

  // Slow down when asked to stop or turn round, or when the GOTO target is near. The +1 leaves
  // room for the step about to be taken, so the motor never speeds up just before the target.
  float stopDistance = (speed * speed - START_SPEED * START_SPEED) / (2 * ACCEL);
  bool slowDown = want != curDir || stopDistance + 1 >= remaining;
  if (slowDown && want != curDir && speed <= START_SPEED + 1) {
    stopNow();   // slow enough to stop dead; a reversal starts on the next pass
    idleSince = millis();
    return;
  }

  // One step.
  optoSet(PUL_PIN, true);
  delayMicroseconds(PULSE_US);
  optoSet(PUL_PIN, false);
  pos = pos + curDir;
  stepsThisMove++;
  if (stepsThisMove > maxStepsForMove()) {
    stopNow();
    idleSince = millis();
    setFault(FAULT_NO_SWITCH);
    return;
  }

  float v2 = speed * speed + (slowDown ? -2 : 2) * ACCEL;
  speed = sqrtf(max(v2, START_SPEED * START_SPEED));
  if (speed > MAX_SPEED) speed = MAX_SPEED;
  nextStepAt += (uint32_t)(1000000.0f / speed);
  if ((int32_t)(micros() - nextStepAt) > 20000) nextStepAt = micros();   // don't try to catch up
}

// ================= core 0: LoRa, saving, status =================

void requestCurtain(uint8_t action, uint8_t percent) {
  // Saved position becomes invalid while the motor runs (a power cut mid-move would leave it wrong).
  // Only written while the motor is stopped: a flash write pauses core 1.
  if (!moving && mode == MODE_IDLE && EEPROM.read(EEPROM_POS_ADDR) == EEPROM_POS_MARKER &&
      action != CURTAIN_STOP) {
    EEPROM.write(EEPROM_POS_ADDR, 0);
    EEPROM.commit();
  }
  reqAction = action;
  reqPercent = percent;
  __sync_synchronize();
  reqSeq = reqSeq + 1;
}

// Saves travel and position once the motor has been stopped for a second.
void saveService() {
  static uint32_t savedCount = 0;
  static unsigned long stoppedAt = 0;
  if (moving || mode != MODE_IDLE) {
    stoppedAt = millis();
    return;
  }
  if (savedCount == motionCount || millis() - stoppedAt < 1000) return;
  savedCount = motionCount;

  uint32_t t = travel;
  EEPROM.write(EEPROM_TRAVEL_ADDR, t ? EEPROM_TRAVEL_MARKER : 0);
  EEPROM.put(EEPROM_TRAVEL_ADDR + 1, t);
  int32_t p = pos;
  EEPROM.write(EEPROM_POS_ADDR, posKnown ? EEPROM_POS_MARKER : 0);
  EEPROM.put(EEPROM_POS_ADDR + 1, p);
  EEPROM.commit();   // does nothing if no byte changed
}

void loadSaved() {
  if (EEPROM.read(EEPROM_TRAVEL_ADDR) == EEPROM_TRAVEL_MARKER) {
    uint32_t t;
    EEPROM.get(EEPROM_TRAVEL_ADDR + 1, t);
    if (t >= MIN_TRAVEL_STEPS && t <= MAX_UNLEARNED_STEPS) travel = t;
  }
  if (travel && EEPROM.read(EEPROM_POS_ADDR) == EEPROM_POS_MARKER) {
    int32_t p;
    EEPROM.get(EEPROM_POS_ADDR + 1, p);
    if (p >= 0 && (uint32_t)p <= travel) {
      pos = p;
      posKnown = true;
    }
  }
}

uint8_t curtainPercent() {
  if (atClosed && !moving) return 0;
  if (atOpen && !moving) return 100;
  if (!posKnown || travel == 0) return NO_PERCENT;
  int32_t p = pos;
  if (p <= 0) return 0;
  if ((uint32_t)p >= travel) return 100;
  return (uint64_t)p * 100 / travel;
}

uint8_t curtainFlags() {
  uint8_t f = CURTAIN_PRESENT;
  if (moving || mode != MODE_IDLE) f |= CURTAIN_MOVING;
  if (curtainPercent() != NO_PERCENT) f |= CURTAIN_KNOWN;
  if (travel) f |= CURTAIN_LEARNED;
  if (atOpen) f |= CURTAIN_AT_OPEN;
  if (atClosed) f |= CURTAIN_AT_CLOSED;
  if (fault != FAULT_NONE) f |= CURTAIN_FAULT;
  if (mode == MODE_LEARN_CLOSE || mode == MODE_LEARN_OPEN) f |= CURTAIN_LEARNING;
  return f;
}

const char *faultText() {
  switch (fault) {
    case FAULT_NO_SWITCH: return "switch never reached - stopped for safety (jammed, or switch not working)";
    case FAULT_BOTH_SWITCHES: return "both switches read pressed - check switch wiring";
    case FAULT_SHORT_TRAVEL: return "learned travel too short - are the switches swapped?";
    default: return "";
  }
}

void printCurtain() {
  Serial.print("Curtain: ");
  uint8_t p = curtainPercent();
  if (p == NO_PERCENT) Serial.print("position unknown");
  else { Serial.print(p); Serial.print("% open"); }
  if (moving || mode != MODE_IDLE) Serial.print(mode == MODE_LEARN_CLOSE || mode == MODE_LEARN_OPEN ? ", learning" : ", moving");
  Serial.print(atOpen ? "  [OPEN switch pressed]" : "");
  Serial.print(atClosed ? "  [CLOSED switch pressed]" : "");
  Serial.print("  steps ");
  Serial.print((int32_t)pos);
  Serial.print('/');
  if (travel) Serial.print((uint32_t)travel); else Serial.print("not learned");
  Serial.println();
  Serial.print("  Raw pins: OPEN GP11=");
  Serial.print(digitalRead(OPEN_SWITCH_PIN) ? "HIGH" : "LOW");
  Serial.print("  CLOSED GP10=");
  Serial.print(digitalRead(CLOSED_SWITCH_PIN) ? "HIGH" : "LOW");
  Serial.println(SWITCH_NC ? "  (NC mode: LOW = free, HIGH = pressed)" : "  (NO mode: HIGH = free, LOW = pressed)");
  if (fault != FAULT_NONE) {
    Serial.print("  FAULT: ");
    Serial.println(faultText());
  }
}

uint16_t loadNodeId() {
  if (FIXED_NODE_ID != 0) return FIXED_NODE_ID;

  pico_unique_board_id_t board;
  pico_get_unique_board_id(&board);
  uint32_t hash = 2166136261u;
  for (uint8_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
    hash ^= board.id[i];
    hash *= 16777619u;
  }
  uint16_t id = (hash >> 16) ^ (hash & 0xFFFF);
  if (id == MASTER_ID || id == BROADCAST_ID) id = 0x0001;
  return id;
}

void printNodeId() {
  if (nodeId < 0x1000) Serial.print('0');
  if (nodeId < 0x100) Serial.print('0');
  if (nodeId < 0x10) Serial.print('0');
  Serial.println(nodeId, HEX);
}

void reply(const Packet &request, uint8_t type, const uint8_t *data, uint8_t len) {
  Packet p;
  p.type = type;
  p.dst = MASTER_ID;
  p.src = nodeId;
  p.seq = request.seq;
  p.len = len;
  if (len) memcpy(p.data, data, len);
  delay(50);   // give the master time to switch back to receive
  sendPacket(p);
  if (type == MSG_ACK) lastAck = p;
}

uint8_t handleCurtain(uint8_t action, uint8_t percent) {
  if (action > CURTAIN_LEARN || (action == CURTAIN_GOTO && percent > 100)) return STATUS_BAD_RELAY;
  if (action != CURTAIN_STOP && atOpen && atClosed) return STATUS_CURTAIN_FAULT;
  requestCurtain(action, percent);
  return STATUS_OK;
}

uint8_t handleCommand(const Packet &p) {
  if (p.len < 1) return STATUS_UNKNOWN_CMD;
  switch (p.data[0]) {
    case CMD_PING:
    case CMD_GET_STATUS:
      return STATUS_OK;
    case CMD_SET_RELAY:
      return STATUS_BAD_RELAY;   // this node has no relays
    case CMD_SET_FAN:
    case CMD_FAN_HOME:
      return STATUS_NO_FAN;
    case CMD_CURTAIN:
      if (p.len < 2) return STATUS_BAD_RELAY;
      return handleCurtain(p.data[1], p.len >= 3 ? p.data[2] : 0);
    default:
      return STATUS_UNKNOWN_CMD;
  }
}

// ---- radio ----
const unsigned long RADIO_SILENCE_RESET_MS = 60000;
unsigned long lastHeard = 0;

bool startRadio() {
  pinMode(LORA_DIO0, INPUT_PULLDOWN);   // RP2350: pin input must be set up explicitly
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(LORA_SPI_HZ);
  if (!LoRa.begin(LORA_BAND)) return false;
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.receive();
  return true;
}

uint8_t readRadioRegister(uint8_t reg) {
  SPI.beginTransaction(SPISettings(LORA_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return value;
}

const uint8_t REG_IRQ_FLAGS = 0x12;
const uint8_t IRQ_RX_DONE = 0x40;
const unsigned long IRQ_POLL_MS = 5;
bool dio0Warned = false;

bool radioHasPacket() {
  if (digitalRead(LORA_DIO0) == HIGH) return true;
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < IRQ_POLL_MS) return false;
  lastPoll = millis();
  if (!(readRadioRegister(REG_IRQ_FLAGS) & IRQ_RX_DONE)) return false;
  if (digitalRead(LORA_DIO0) == LOW && !dio0Warned) {
    dio0Warned = true;
    Serial.println("Warning: packet arrived but DIO0 stayed LOW - check the DIO0 -> GP20 wire.");
  }
  return true;
}

// ---- status LED (onboard + GP15 -> 330 ohm -> LED -> GND) ----
//   short blink every 2 s = running, radio OK
//   fast blinking         = radio not found
//   longer flash          = a command from the master arrived
//   slow blink (1 Hz)     = curtain fault (see "info" on USB serial)
#define STATUS_LED LED_BUILTIN
#define EXT_LED 15
unsigned long ledFlashUntil = 0;
bool ledState = false;

void setLed(bool on) {
  ledState = on;
  digitalWrite(STATUS_LED, on ? HIGH : LOW);
  digitalWrite(EXT_LED, on ? HIGH : LOW);
}

void ledService() {
  unsigned long now = millis();
  if (fault != FAULT_NONE) setLed(now % 1000 < 500);
  else setLed(now < ledFlashUntil || now % 2000 < 40);
}

// ---- USB serial test commands ----
void serialService() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 32) line += c;
      continue;
    }
    line.trim();
    line.toLowerCase();
    if (line == "watch") {
      // Prints the raw switch pins every 200 ms for 20 s, to check the wiring by hand.
      Serial.println("Watching switches for 20 s - press them now.");
      for (uint8_t i = 0; i < 100; i++) {
        Serial.print(digitalRead(OPEN_SWITCH_PIN) ? "OPEN GP11=HIGH " : "OPEN GP11=LOW  ");
        Serial.print(digitalRead(CLOSED_SWITCH_PIN) ? "CLOSED GP10=HIGH" : "CLOSED GP10=LOW ");
        Serial.print("   seen as: ");
        Serial.print(atOpen ? "OPEN pressed " : "");
        Serial.println(atClosed ? "CLOSED pressed" : "");
        delay(200);
      }
    } else if (line == "findpins") {
      // Wiring check: pulls up every unused GPIO and reports any that is pulled LOW (button pressed).
      const uint8_t pins[] = { 0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 22, 26, 27, 28 };
      for (uint8_t pin : pins) pinMode(pin, INPUT_PULLUP);
      delay(5);
      Serial.println("Finding pins for 20 s - press each button now.");
      uint32_t lowMask = 0;
      for (uint8_t pin : pins) if (digitalRead(pin) == LOW) lowMask |= 1u << pin;
      if (lowMask) {
        Serial.print("Already LOW before pressing (wired to GND?):");
        for (uint8_t pin : pins) if (lowMask & (1u << pin)) { Serial.print(" GP"); Serial.print(pin); }
        Serial.println();
      }
      unsigned long start = millis();
      while (millis() - start < 20000) {
        for (uint8_t pin : pins) {
          bool low = digitalRead(pin) == LOW;
          if (low != (bool)(lowMask & (1u << pin))) {
            lowMask ^= 1u << pin;
            Serial.print("GP");
            Serial.print(pin);
            Serial.print(" (physical pin ");
            Serial.print(pin < 2 ? pin + 1 : pin < 6 ? pin + 2 : pin < 10 ? pin + 3 : pin < 14 ? pin + 4 : pin < 16 ? pin + 5 : pin == 22 ? 29 : pin == 28 ? 34 : pin + 5);
            Serial.println(low ? ") went LOW - pressed" : ") back HIGH - released");
          }
        }
        delay(20);
      }
      Serial.println("Done.");
    } else if (line == "info") {
      Serial.print("My ID: ");
      printNodeId();
      printCurtain();
    } else if (line == "radio") {
      LoRa.dumpRegisters(Serial);
      LoRa.receive();
    } else if (line == "open" || line == "close" || line == "stop" || line == "learn") {
      uint8_t a = line == "open" ? CURTAIN_OPEN : line == "close" ? CURTAIN_CLOSE
                : line == "learn" ? CURTAIN_LEARN : CURTAIN_STOP;
      if (handleCurtain(a, 0) == STATUS_OK) Serial.println("OK: " + line);
      else Serial.println("Refused: both switches read pressed.");
    } else if (line.startsWith("goto ")) {
      long p = line.substring(5).toInt();
      if (p < 0 || p > 100) Serial.println("Use: goto <0-100>");
      else if (handleCurtain(CURTAIN_GOTO, p) == STATUS_OK) { Serial.print("OK: going to "); Serial.print(p); Serial.println('%'); }
      else Serial.println("Refused: both switches read pressed.");
    } else if (line == "forget") {
      if (moving || mode != MODE_IDLE) Serial.println("Stop the motor first.");
      else {
        travel = 0;
        posKnown = false;
        motionCount = motionCount + 1;   // saveService() writes it out
        Serial.println("Travel forgotten; run \"learn\" again.");
      }
    } else if (line.length()) {
      Serial.println("Commands: info, watch, findpins, open, close, stop, goto <0-100>, learn, forget, radio");
    }
    line = "";
  }
}

void setup() {
  pinMode(STATUS_LED, OUTPUT);
  pinMode(EXT_LED, OUTPUT);
  setLed(true);

  setupDriverPins();
  driverEnable(HOLD_WHEN_IDLE);
  if (DRIVER_EN_WIRED && !HOLD_WHEN_IDLE) optoSet(EN_PIN, EN_OPTO_ON_DISABLES);   // driverOn starts false: force the pin
  pinMode(OPEN_SWITCH_PIN, INPUT_PULLUP);
  pinMode(CLOSED_SWITCH_PIN, INPUT_PULLUP);
  delay(2);
  atOpen = switchPressed(OPEN_SWITCH_PIN);
  atClosed = switchPressed(CLOSED_SWITCH_PIN);

  EEPROM.begin(EEPROM_SIZE);
  loadSaved();
  motorReady = true;   // core 1 may start

  Serial.begin(115200);
  while (!Serial && millis() < 3000) delay(10);

  nodeId = loadNodeId();
  randomSeed(rp2040.hwrand32() ^ nodeId);

  Serial.println("LoRa Pico Curtain Node Initializing");
  printCurtain();
  delay(100);
  while (!startRadio()) {
    Serial.print("Starting LoRa failed! Version register reads 0x");
    Serial.print(readRadioRegister(0x42), HEX);
    Serial.println(" (should be 0x12). Check wiring. Retrying in 2 s...");
    for (uint8_t i = 0; i < 10; i++) {
      setLed(!ledState);
      delay(100);
    }
    setLed(false);
    delay(1000);
  }
  lastHeard = millis();

  Serial.print("LoRa Pico Curtain Node Ready. My ID: ");
  printNodeId();
}

void loop() {
  serialService();
  ledService();
  saveService();

  static uint8_t lastFault = FAULT_NONE;
  if (fault != lastFault) {
    lastFault = fault;
    if (fault != FAULT_NONE) printCurtain();
  }
  static bool wasMoving = false;
  bool busy = moving || mode != MODE_IDLE;
  if (wasMoving && !busy) printCurtain();   // report where it stopped
  wasMoving = busy;

  if (millis() - lastHeard > RADIO_SILENCE_RESET_MS) {
    startRadio();
    lastHeard = millis();
  }

  if (!radioHasPacket()) return;
  Packet p;
  if (receivePacket(p)) {
    lastHeard = millis();
    if (p.src == MASTER_ID) handlePacket(p);
  }
  LoRa.receive();
}

void handlePacket(const Packet &p) {
  if (p.type == MSG_DISCOVER && p.dst == BROADCAST_ID && p.len >= 2) {
    uint8_t scan = p.data[0];
    if (scan == registeredScan) return;
    uint8_t slots = p.data[1] ? p.data[1] : 1;
    delay((unsigned long)random(slots) * SLOT_MS);
    reply(p, MSG_HELLO, &scan, 1);
    Serial.println("Answered discovery");
    return;
  }

  if (p.dst != nodeId) return;

  if (p.type == MSG_WELCOME && p.len >= 1) {
    registeredScan = p.data[0];
    lastCmdSeq = 0;
    reply(p, MSG_ACK, nullptr, 0);
    Serial.println("Registered with master");
    return;
  }

  if (p.type == MSG_CMD) {
    if (p.seq == lastCmdSeq && lastAck.seq == p.seq) {
      delay(50);
      sendPacket(lastAck);   // resent command: answer again without repeating it
      return;
    }
    lastCmdSeq = p.seq;
    ledFlashUntil = millis() + 300;
    uint8_t status = handleCommand(p);
    if (status == STATUS_OK && p.data[0] == CMD_CURTAIN) delay(5);   // let core 1 pick it up
    uint8_t answer[6] = { status, 0, 0, 0, curtainPercent(), curtainFlags() };
    reply(p, MSG_ACK, answer, 6);
    Serial.print("Command ");
    Serial.print(p.data[0]);
    Serial.print(status == STATUS_OK ? " done. " : " rejected. ");
    printCurtain();
  }
}
