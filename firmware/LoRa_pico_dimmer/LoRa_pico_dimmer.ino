// LoRa dimmer node for the Raspberry Pi Pico (RP2040). Drives a 2-channel AC dimmer module
// (RobotDyn-style: one Z-C output + one triac gate input per channel):
//   channel 1 (L1) = light bulb brightness  -> master "dim <id> <0-100|on|off|next>" (CMD_DIMMER)
//   channel 2 (L2) = fan speed              -> master "fan <id> <0-100>"             (CMD_SET_FAN)
// The fan uses the same command and ACK fields as the stepper fan regulator, so the master and the
// website show and set it without changes. No relays.
//
// Board package: "Raspberry Pi Pico/RP2040/RP2350" by Earle Philhower; board "Raspberry Pi Pico"
// (FQBN rp2040:rp2040:rpipico). Also builds for the Pico 2 (rp2040:rp2040:rpipico2).
// Serial monitor: USB, any baud rate. Type "help" for test commands.
// Both levels are remembered across power cuts and resets.
//
// ---- Wiring (Pico pin numbers; on the as-built board the Pico is mirrored, see the build sheet) ----
//   SX1278 / Ra-02   Pico            Dimmer module   Pico
//   3.3V             3V3 pin 36      VCC             3V3 pin 36  (3.3 V, NOT 5 V)
//   GND              GND pin 38      GND             GND pin 38
//   MISO             GP16 pin 21     Z-C             GP26 pin 31
//   NSS              GP17 pin 22     D1 / PWM1       GP27 pin 32  (light)
//   SCK              GP18 pin 24     D2 / PWM2       GP28 pin 34  (fan)
//   MOSI             GP19 pin 25
//   DIO0             GP20 pin 26     Status LED      GP15 pin 20 -> 330 ohm -> LED -> GND pin 18
//   RST              GP21 pin 27     Reset button    push switch C -> GND pin 28, NO -> RUN pin 30
//   (optional brightness button, HAS_BUTTON 1: C -> GND pin 18, NO -> GP14 pin 19)
//
// Mains: AC-L / AC-N from the MCB; L1 -> bulb -> N; L2 -> fan -> N. Take the fan's old wall
// regulator out of the circuit (or leave it at full speed): two speed controls in series misbehave.
//
// The triacs are fired from interrupts and hardware alarms, never from loop(), so LoRa traffic
// does not make the bulb flicker or the fan surge.

#include <SPI.h>
#include <LoRa.h>
#include <EEPROM.h>
#include <math.h>
#include <pico/unique_id.h>
#include "pico/time.h"
#include "hardware/gpio.h"
#include "dimmer_channel.h"

// ---- LoRa (same wiring as LoRa_pico_node) ----
#define LORA_SS 17
#define LORA_RST 21
#define LORA_DIO0 20
#define LORA_SPI_HZ 1000000
#define LORA_BAND 433E6
#define LORA_SYNC_WORD 0x12   // must match the master
#define LORA_TX_POWER 17      // dBm (2-20). Clean at 17 and 20 after fixing a loose ground (re-check with txtest)

// Set to a value from 0x0001 to 0xFFFE to choose this node's ID yourself; 0 = from the chip ID.
#define FIXED_NODE_ID 0

// ---- Dimmer pins ----
#define ZC_PIN 26
#define LIGHT_GATE_PIN 27     // module D1 / PWM1 -> L1
#define FAN_GATE_PIN 28       // module D2 / PWM2 -> L2
#define HAS_BUTTON 0          // 1 = a light push button is wired to BUTTON_PIN
#define BUTTON_PIN 14
#define BUTTON_USES_NC false  // false: C->GND, NO->GP14.  true: C->GND, NC->GP14.
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 700

// Light steps for "next" / the optional button (percent of full lamp power).
const uint8_t LEVELS[] = {0, 25, 50, 75, 100};
const uint8_t LEVEL_COUNT = sizeof(LEVELS) / sizeof(LEVELS[0]);

// ---- Fan ----
// Speed 1-100 is spread over FAN_MIN_POWER..100 % power, so the lowest speed still turns the motor.
// Raise FAN_MIN_POWER if the fan hums but does not turn at speed 1.
#define FAN_MIN_POWER 35
#define FAN_KICK_MS 1500      // start from standstill at full power for this long, then slow down
#define FAN_FADE_MS_PER_PERCENT 15

// ---- Triac timing (microseconds) ----
#define GATE_PULSE_US 100     // light: short gate pulse
#define ZC_OFFSET_US 0        // add if the Z-C pulse arrives early (tune if low levels flicker)
#define MIN_DELAY_US 400      // never fire earlier than this after Z-C
#define END_MARGIN_US 700     // never fire later than this before the next Z-C; the fan's long gate
                              // pulse also ends here, so the triac can switch off at zero current
#define MIN_ZC_GAP_US 6000    // Z-C edges closer than this are noise
#define LIGHT_FADE_MS_PER_PERCENT 4
#define ZC_LOST_US 100000     // no Z-C for this long = no mains at the dimmer

// ---- protocol (keep identical in LoRa_esp32_master) ----
const uint8_t MAGIC = 0xA7;
const uint16_t MASTER_ID = 0x0000;
const uint16_t BROADCAST_ID = 0xFFFF;

enum MsgType : uint8_t { MSG_DISCOVER = 1, MSG_HELLO = 2, MSG_WELCOME = 3, MSG_CMD = 4, MSG_ACK = 5 };
// CMD_SET_FAN data: speed (0-100) -> channel 2. CMD_FAN_HOME: accepted, nothing to do.
// CMD_DIMMER data: level (0-100), action (see DimmerAction) -> channel 1.
// ACK data from this node (8 bytes): status, relay bits (0), fan speed, fan flags (FAN_PRESENT),
// curtain position (0), curtain flags (0), light level (0-100), dimmer flags (see DimmerFlag).
enum Command : uint8_t {
  CMD_PING = 1, CMD_SET_RELAY = 3, CMD_GET_STATUS = 4, CMD_SET_FAN = 5, CMD_FAN_HOME = 6, CMD_CURTAIN = 7,
  CMD_DIMMER = 8
};
enum CmdStatus : uint8_t {
  STATUS_OK = 0, STATUS_UNKNOWN_CMD = 1, STATUS_BAD_RELAY = 2, STATUS_NO_FAN = 3, STATUS_NO_CURTAIN = 4,
  STATUS_CURTAIN_FAULT = 5, STATUS_NO_DIMMER = 6
};
enum FanFlag : uint8_t { FAN_PRESENT = 1, FAN_HOMING = 2 };
enum DimmerAction : uint8_t { DIMMER_SET = 0, DIMMER_NEXT = 1 };
enum DimmerFlag : uint8_t { DIMMER_PRESENT = 1, DIMMER_MAINS_OK = 2, DIMMER_FADING = 4 };

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

// The Pico has no real EEPROM: the library keeps a copy in RAM and writes a flash sector on commit().
const size_t EEPROM_SIZE = 256;
// Writing flash stops all interrupts for a few tens of ms, so the triacs miss some half waves.
// When every channel is at OFF or 100 % that is invisible, so the levels are saved at once;
// otherwise only after they have stayed unchanged for SAVE_DELAY_MS (one blip instead of many).
const unsigned long SAVE_DELAY_MS = 10000;

uint16_t nodeId;
uint8_t registeredScan = 0;
uint8_t lastCmdSeq = 0;
Packet lastAck;

// =============================================================================================
// Dimmer channels

Channel light = { "Light", LIGHT_GATE_PIN, false, LIGHT_FADE_MS_PER_PERCENT, 7, 0xA7,
                  MODE_OFF, 1000, false, 0, 0, 0, 0xFF, 0, 0, 0 };
Channel fan   = { "Fan", FAN_GATE_PIN, true, FAN_FADE_MS_PER_PERCENT, 9, 0xA9,
                  MODE_OFF, 1000, false, 0, 0, 0, 0xFF, 0, 0, 0 };
Channel *const channels[2] = { &light, &fan };

volatile uint32_t halfPeriodUs = 10000;   // measured; starts at 50 Hz
volatile uint32_t lastZcUs = 0;
volatile uint32_t zcCount = 0;
volatile uint8_t zcResync = 0;            // edges to skip after a flash write

int64_t gateAlarmCb(alarm_id_t, void *userData) {
  Channel *ch = (Channel *)userData;
  if (ch->mode != MODE_PHASE) {   // mode changed while the alarm was pending
    ch->alarm = 0;
    return 0;
  }
  if (!ch->pulseHigh) {
    gpio_put(ch->gatePin, 1);
    ch->pulseHigh = true;
    if (!ch->longPulse) return GATE_PULSE_US;
    // Long pulse: stay on until END_MARGIN before the next zero cross. A motor's current lags the
    // voltage, so a short pulse can end before the triac has latched.
    uint32_t endAt = halfPeriodUs - END_MARGIN_US;
    uint32_t since = micros() - lastZcUs;
    return since + GATE_PULSE_US < endAt ? endAt - since : GATE_PULSE_US;
  }
  gpio_put(ch->gatePin, 0);
  ch->pulseHigh = false;
  ch->alarm = 0;
  return 0;
}

void scheduleGate(Channel &ch, uint32_t half) {
  if (ch.alarm) cancel_alarm(ch.alarm);
  gpio_put(ch.gatePin, 0);
  ch.pulseHigh = false;
  ch.alarm = 0;
  if (ch.mode != MODE_PHASE) return;
  uint32_t d = half * ch.delayPermille / 1000 + ZC_OFFSET_US;
  if (d < MIN_DELAY_US) d = MIN_DELAY_US;
  if (d > half - END_MARGIN_US) d = half - END_MARGIN_US;
  ch.alarm = add_alarm_in_us(d, gateAlarmCb, &ch, true);
}

void onZeroCross() {
  uint32_t now = micros();
  uint32_t dt = now - lastZcUs;
  if (zcResync) {                 // straight after a flash write: re-learn the timing, don't fire
    zcResync--;
    lastZcUs = now;
    return;
  }
  if (dt < MIN_ZC_GAP_US) return; // noise / bounce on the Z-C line
  lastZcUs = now;
  zcCount++;
  if (dt > 7000 && dt < 11500) halfPeriodUs = (halfPeriodUs * 7 + dt) / 8;   // 43..71 Hz
  uint32_t half = halfPeriodUs;
  if (light.mode == MODE_PHASE) scheduleGate(light, half);
  if (fan.mode == MODE_PHASE) scheduleGate(fan, half);
}

// For a resistive load fired at fraction x of the half wave, the power is
// P(x) = 1 - x + sin(2*pi*x) / (2*pi). Solve for x so the levels are real power steps.
uint32_t powerToDelayPermille(float percent) {
  float p = percent / 100.0f;
  float lo = 0, hi = 1;
  for (int i = 0; i < 20; i++) {
    float x = (lo + hi) / 2;
    float px = 1 - x + sinf(2 * PI * x) / (2 * PI);
    if (px > p) lo = x; else hi = x;
  }
  return (uint32_t)((lo + hi) / 2 * 1000);
}

void applyPower(Channel &ch, float percent) {
  if (percent <= 0.5f) {
    ch.mode = MODE_OFF;
    gpio_put(ch.gatePin, 0);
  } else if (percent >= 99.5f) {
    ch.mode = MODE_FULL;
    gpio_put(ch.gatePin, 1);
  } else {
    ch.delayPermille = powerToDelayPermille(percent);
    if (ch.mode != MODE_PHASE) gpio_put(ch.gatePin, 0);
    ch.mode = MODE_PHASE;
  }
}

float targetPower(const Channel &ch) {
  if (&ch == &fan && ch.level > 0) return FAN_MIN_POWER + (100 - FAN_MIN_POWER) * ch.level / 100.0f;
  return ch.level;
}

void setLevel(Channel &ch, uint8_t level, const char *source) {
  level = min(level, (uint8_t)100);
  // A fan starting from standstill gets a short full-power kick so the motor surely turns.
  if (&ch == &fan && level > 0 && ch.power < 0.5f) {
    ch.kickUntil = millis() + FAN_KICK_MS;
    ch.power = 100;
    applyPower(ch, ch.power);
  }
  ch.level = level;
  ch.changedAt = millis();
  Serial.printf("%s -> %u%% (%s)\n", ch.name, level, source);
}

// Next light step above the current level; wraps to OFF after 100 %.
void nextLevel(const char *source) {
  for (uint8_t i = 0; i < LEVEL_COUNT; i++) {
    if (LEVELS[i] > light.level) {
      setLevel(light, LEVELS[i], source);
      return;
    }
  }
  setLevel(light, 0, source);
}

void fadeService(Channel &ch) {
  unsigned long now = millis();
  if (ch.kickUntil && (long)(now - ch.kickUntil) < 0) return;
  ch.kickUntil = 0;
  float target = targetPower(ch);
  if (ch.power == target) return;
  if (now - ch.lastFadeStep < ch.fadeMsPerPercent) return;
  ch.lastFadeStep = now;
  if (ch.power < target) ch.power = min(ch.power + 1, target);
  else ch.power = max(ch.power - 1, target);
  // A fan slowing to OFF switches off at its lowest running speed instead of humming below it.
  if (&ch == &fan && target == 0 && ch.power < FAN_MIN_POWER) ch.power = 0;
  applyPower(ch, ch.power);
}

bool settled(const Channel &ch) {
  return ch.kickUntil == 0 && ch.power == targetPower(ch);
}

bool mainsOk() {
  return micros() - lastZcUs < ZC_LOST_US;
}

void saveService() {
  if (light.level == light.saved && fan.level == fan.saved) return;
  if (!settled(light) || !settled(fan)) return;
  bool invisible = true;
  unsigned long newest = 0;
  for (Channel *ch : channels) {
    if (ch->level != 0 && ch->level != 100) invisible = false;
    newest = max(newest, ch->changedAt);
  }
  // The fan at 100 % speed is 100 % power, light 100 % is full: both invisible during the write.
  if (!invisible && millis() - newest < SAVE_DELAY_MS) return;
  for (Channel *ch : channels) {
    EEPROM.write(ch->eepromAddr, ch->eepromMarker);
    EEPROM.write(ch->eepromAddr + 1, ch->level);
  }
  zcResync = 2;   // the edge held back during the write arrives late; skip it and re-sync
  EEPROM.commit();
  for (Channel *ch : channels) ch->saved = ch->level;
}

void setupDimmer() {
  for (Channel *ch : channels) {
    pinMode(ch->gatePin, OUTPUT);
    digitalWrite(ch->gatePin, LOW);
    if (EEPROM.read(ch->eepromAddr) == ch->eepromMarker) {
      ch->saved = min(EEPROM.read(ch->eepromAddr + 1), (uint8_t)100);
    }
  }
  pinMode(ZC_PIN, INPUT_PULLUP);
  if (HAS_BUTTON) pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ZC_PIN), onZeroCross, RISING);
  // Come back at the saved levels (the fan gets its start-up kick).
  if (light.saved != 0xFF) setLevel(light, light.saved, "restored");
  if (fan.saved != 0xFF) setLevel(fan, fan.saved, "restored");
}

uint8_t dimmerFlags() {
  return DIMMER_PRESENT | (mainsOk() ? DIMMER_MAINS_OK : 0) | (settled(light) ? 0 : DIMMER_FADING);
}

// ---- Optional local button (light) ----
bool buttonPressedRaw() {
  bool level = digitalRead(BUTTON_PIN);   // pull-up: HIGH when the contact is open
  return BUTTON_USES_NC ? level : !level;
}

void buttonService() {
  if (!HAS_BUTTON) return;
  static bool stable = false, lastRaw = false, longDone = false;
  static uint32_t changedAt = 0, pressedAt = 0;
  bool raw = buttonPressedRaw();
  uint32_t now = millis();

  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (now - changedAt >= DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable) {
      pressedAt = now;
      longDone = false;
    } else if (!longDone) {
      nextLevel("button");
    }
  }
  if (stable && !longDone && now - pressedAt >= LONG_PRESS_MS) {
    longDone = true;
    setLevel(light, 0, "button, long press");
  }
}

void dimmerService() {
  buttonService();
  fadeService(light);
  fadeService(fan);
  saveService();
}

// =============================================================================================
// Node

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

void reply(const Packet &request, uint8_t type, const uint8_t *data, uint8_t len) {
  Packet p;
  p.type = type;
  p.dst = MASTER_ID;
  p.src = nodeId;
  p.seq = request.seq;
  p.len = len;
  if (len) memcpy(p.data, data, len);
  delay(50);   // give the master time to switch back to receive (the triacs run on interrupts)
  sendPacket(p);
  if (type == MSG_ACK) lastAck = p;
}

uint8_t handleCommand(const Packet &p) {
  if (p.len < 1) return STATUS_UNKNOWN_CMD;
  switch (p.data[0]) {
    case CMD_PING:
    case CMD_GET_STATUS:
      return STATUS_OK;
    case CMD_DIMMER:
      if (p.len < 3 || p.data[1] > 100 || p.data[2] > DIMMER_NEXT) return STATUS_BAD_RELAY;
      if (p.data[2] == DIMMER_NEXT) nextLevel("master");
      else setLevel(light, p.data[1], "master");
      return STATUS_OK;
    case CMD_SET_FAN:
      if (p.len < 2 || p.data[1] > 100) return STATUS_BAD_RELAY;
      setLevel(fan, p.data[1], "master");
      return STATUS_OK;
    case CMD_FAN_HOME:
      return STATUS_OK;            // nothing to home: the dimmer sets the speed directly
    default:
      return STATUS_UNKNOWN_CMD;   // this node has no relays or curtain
  }
}

void printNodeId() {
  Serial.printf("%04X\n", nodeId);
}

void printChannel(const Channel &ch) {
  uint32_t half = halfPeriodUs;
  Serial.printf("%s: %u%% (power now %d%%), mode %s", ch.name, ch.level, (int)ch.power,
                ch.mode == MODE_OFF ? "OFF" : ch.mode == MODE_FULL ? "FULL" : "PHASE");
  if (ch.mode == MODE_PHASE) Serial.printf(", firing %lu us after zero cross", (unsigned long)(half * ch.delayPermille / 1000));
  if (ch.kickUntil) Serial.print(", start-up kick");
  Serial.println();
}

void printInfo() {
  uint32_t half = halfPeriodUs;
  Serial.print("My ID: ");
  printNodeId();
  printChannel(light);
  printChannel(fan);
  Serial.printf("Zero cross: %lu pulses, half period %lu us (%.1f Hz), mains %s\n",
                (unsigned long)zcCount, (unsigned long)half, 500000.0f / half,
                mainsOk() ? "OK" : "NOT DETECTED (check Z-C -> GP26 and mains at AC-L/AC-N)");
  if (HAS_BUTTON) Serial.printf("Button: %s\n", buttonPressedRaw() ? "pressed" : "released");
}

// ---- Radio ----
const unsigned long RADIO_SILENCE_RESET_MS = 60000;
unsigned long lastHeard = 0;

bool startRadio() {
  pinMode(LORA_DIO0, INPUT_PULLDOWN);   // keeps DIO0 LOW if its wire comes loose
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(LORA_SPI_HZ);
  if (!LoRa.begin(LORA_BAND)) return false;
  // Set every modem setting explicitly (must match the master): a module that was not
  // properly reset keeps stale values (seen here: SF6 / CR 4/6) and then hears nothing.
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSpreadingFactor(7);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
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
bool rxLog = false;   // "rxlog" serial command prints every packet heard

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

// ---- Status LED (onboard GP25 + panel LED on GP15) ----
//   short blink every 2 s  = running, radio OK
//   double blink every 2 s = running, but no mains at the dimmer
//   fast blinking          = radio not found
//   longer flash           = a command from the master arrived
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
  unsigned long t = now % 2000;
  bool blink = t < 40 || (!mainsOk() && t >= 200 && t < 240);
  setLed(now < ledFlashUntil || blink);
}

// Sends 5 test packets (a type the master ignores) and checks the modem registers after each:
// if they change, the SPI bus or the radio supply glitched during transmit.
void txTest() {
  uint8_t before1D = readRadioRegister(0x1D), before1E = readRadioRegister(0x1E);
  Serial.printf("Before: 0x1D=%02X 0x1E=%02X 0x42=%02X\n", before1D, before1E, readRadioRegister(0x42));
  for (int i = 1; i <= 5; i++) {
    Packet p;
    p.type = 0xEE; p.dst = MASTER_ID; p.src = nodeId; p.seq = i; p.len = 8;
    memset(p.data, 0x55, 8);
    unsigned long t = micros();
    sendPacket(p);
    t = micros() - t;
    uint8_t r1D = readRadioRegister(0x1D), r1E = readRadioRegister(0x1E), r42 = readRadioRegister(0x42);
    Serial.printf("TX %d: %lu us, 0x1D=%02X 0x1E=%02X 0x42=%02X %s\n", i, t, r1D, r1E, r42,
                  (r1D == before1D && r1E == before1E && r42 == 0x12) ? "OK" : "CORRUPTED");
    delay(200);
  }
  startRadio();
  Serial.println("Radio re-initialised.");
}

// ---- Serial ----
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
    if (line == "info") printInfo();
    else if (line == "radio") { LoRa.dumpRegisters(Serial); LoRa.receive(); }
    else if (line == "rxlog") { rxLog = !rxLog; Serial.println(rxLog ? "RX log on" : "RX log off"); }
    else if (line.startsWith("txpower ")) {
      int dbm = constrain(line.substring(8).toInt(), 2, 20);
      LoRa.setTxPower(dbm); LoRa.receive();
      Serial.printf("TX power %d dBm (until restart)\n", dbm);
    }
    else if (line == "txtest") txTest();
    else if (line == "pwm on" || line == "pwm off") {   // Pico 3V3 regulator mode (GP23 = PS pin)
      pinMode(23, OUTPUT); digitalWrite(23, line == "pwm on");
      Serial.println(line == "pwm on" ? "3V3 regulator: PWM mode (low ripple)" : "3V3 regulator: power-save mode");
    }
    else if (line == "next") nextLevel("serial");
    else if (line == "off") setLevel(light, 0, "serial");
    else if (line == "on") setLevel(light, 100, "serial");
    else if (line.startsWith("fan ")) setLevel(fan, constrain(line.substring(4).toInt(), 0, 100), "serial");
    else if (line.length() && isDigit(line[0])) setLevel(light, constrain(line.toInt(), 0, 100), "serial");
    else if (line.length()) Serial.println("Commands: info, next, on, off, 0-100 (light), fan 0-100, radio");
    line = "";
  }
}

// =============================================================================================

void setup() {
  // Pico 3V3 regulator into PWM mode (GP23 = its PS pin): power-save mode let 3V3 dip on LoRa transmit
  // and corrupted the SPI bus at 10 dBm.
  pinMode(23, OUTPUT);
  digitalWrite(23, HIGH);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(EXT_LED, OUTPUT);
  setLed(true);
  EEPROM.begin(EEPROM_SIZE);
  Serial.begin(115200);
  setupDimmer();
  while (!Serial && millis() < 3000) { dimmerService(); delay(1); }

  nodeId = loadNodeId();
  randomSeed(rp2040.hwrand32() ^ nodeId);

  Serial.println("LoRa Pico Dimmer Node Initializing (channel 1 = light, channel 2 = fan)");
  delay(100);
  while (!startRadio()) {
    Serial.print("Starting LoRa failed! Version register reads 0x");
    Serial.print(readRadioRegister(0x42), HEX);
    Serial.println(" (should be 0x12). Check wiring. Retrying in 2 s...");
    // The dimmer keeps working while the radio is missing.
    for (uint8_t i = 0; i < 20; i++) {
      setLed(!ledState);
      unsigned long until = millis() + 100;
      while (millis() < until) dimmerService();
    }
  }
  lastHeard = millis();

  Serial.print("LoRa Pico Dimmer Node Ready. My ID: ");
  printNodeId();
  Serial.println("Type help for commands.");
}

void loop() {
  dimmerService();
  serialService();
  ledService();

  if (millis() - lastHeard > RADIO_SILENCE_RESET_MS) {
    startRadio();
    lastHeard = millis();
  }

  if (!radioHasPacket()) return;
  Packet p;
  bool good = receivePacket(p);
  if (rxLog) {
    if (good) Serial.printf("RX type %u dst %04X src %04X seq %u len %u, RSSI %d SNR %.1f\n",
                            p.type, p.dst, p.src, p.seq, p.len, LoRa.packetRssi(), LoRa.packetSnr());
    else Serial.printf("RX bad/foreign packet, RSSI %d\n", LoRa.packetRssi());
  }
  if (good) {
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
      delay(50);   // resent command: answer again without repeating it (so "next" steps once)
      sendPacket(lastAck);
      return;
    }
    lastCmdSeq = p.seq;
    ledFlashUntil = millis() + 300;
    uint8_t status = handleCommand(p);
    uint8_t answer[8] = { status, 0, fan.level, FAN_PRESENT, 0, 0, light.level, dimmerFlags() };
    reply(p, MSG_ACK, answer, 8);
    Serial.printf("Command %u %s. Light %u%%, fan %u%%\n", p.data[0], status == STATUS_OK ? "done" : "rejected",
                  light.level, fan.level);
  }
}
