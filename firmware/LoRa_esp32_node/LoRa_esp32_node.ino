// LoRa node for an ESP32 dev board (ESP32-WROOM-32). Speaks the same protocol as LoRa_nano_node
// and LoRa_pico_node, so the ESP32 master and the website treat it like any other node.
//
// Board: "ESP32 Dev Module" (esp32:esp32). Serial monitor: 115200 baud.
// Tools > Flash Frequency: 40MHz. This board misreads its flash at the default 80 MHz
// ("partition table error" boot loop). Tools > Upload Speed 115200 if uploads break off.
// Uploading: if it doesn't start on its own, hold BOOT until "Connecting..." finishes.
//
// The node ID is worked out from the chip's factory MAC address, so it never changes, not even
// after re-flashing. It is printed at startup. Set FIXED_NODE_ID to choose one yourself.
//
// Relays and the fan stepper are optional: with nothing connected to their pins the node still
// joins the network and answers ping / status / relay commands (the relay pins just toggle).

#include <SPI.h>
#include <LoRa.h>
#include <EEPROM.h>

// ---- LoRa wiring (SX1278 / Ra-02, all 3.3 V, direct wires) - same pins as the master ----
//   SX1278   ESP32
//   3.3V     3V3       (+ 100 uF capacitor across 3.3V/GND at the module)
//   GND      GND
//   MISO     GPIO19
//   NSS      GPIO5
//   SCK      GPIO18
//   MOSI     GPIO23
//   DIO0     GPIO2
//   RST      GPIO14
// GPIO18/19/23 are the ESP32's default SPI (VSPI) pins, so SPI needs no extra setup.
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2
// Direct wires allow much more; 1 MHz leaves margin for breadboard jumpers.
#define LORA_SPI_HZ 1000000

#define LORA_BAND 433E6
#define LORA_SYNC_WORD 0x12   // must match the master (0x12 is the library default)
#define LORA_TX_POWER 17      // dBm (2-20)

// Relay module inputs IN1..IN4.
const uint8_t RELAY_COUNT = 4;
const uint8_t RELAY_PINS[RELAY_COUNT] = { 25, 26, 27, 32 };
// true = relay switches on when the pin is HIGH; false = on when LOW.
const bool RELAY_ACTIVE_HIGH[RELAY_COUNT] = { false, false, false, false };

// Set to a value from 0x0001 to 0xFFFE to choose this node's ID yourself; 0 = from the MAC address.
#define FIXED_NODE_ID 0

// ---- fan regulator (28BYJ-48 + ULN2003), off until the motor is connected ----
#define HAS_FAN 0   // 1 = this node has the fan stepper
// ULN2003 board inputs IN1..IN4.
const uint8_t FAN_PINS[4] = { 16, 17, 21, 22 };
// How far the knob turns from OFF to full speed, in degrees. Measure your regulator.
const uint16_t FAN_TRAVEL_DEGREES = 270;
// Swap to true if homing turns the knob towards full speed instead of OFF.
const bool FAN_REVERSE = false;
// Time per half-step. Larger = slower but stronger; raise it if the motor stalls mid-turn.
const uint16_t FAN_STEP_US = 2000;
const uint16_t FAN_HOME_STEP_US = 2500;
const uint16_t HALF_STEPS_PER_TURN = 4076;   // 28BYJ-48 gearbox, half-stepping
const uint16_t FAN_RANGE_STEPS = (uint32_t)FAN_TRAVEL_DEGREES * HALF_STEPS_PER_TURN / 360;
// Homing turns a bit further than the full range, so it reaches the stop from anywhere.
const uint16_t FAN_HOME_STEPS = FAN_RANGE_STEPS + FAN_RANGE_STEPS / 8;

// ---- protocol (keep identical in LoRa_esp32_master) ----
const uint8_t MAGIC = 0xA7;
const uint16_t MASTER_ID = 0x0000;
const uint16_t BROADCAST_ID = 0xFFFF;

enum MsgType : uint8_t { MSG_DISCOVER = 1, MSG_HELLO = 2, MSG_WELCOME = 3, MSG_CMD = 4, MSG_ACK = 5 };
// CMD_SET_RELAY data: relay (1-4, 0 = all), state (0 off, 1 on, 2 toggle).
// CMD_SET_FAN data: level (0-100). CMD_FAN_HOME: find OFF again, then return to the level.
// ACK data for every command: status (see CmdStatus), relay bits (bit 0 = relay 1),
// fan level (0-100), fan flags (see FanFlag).
enum Command : uint8_t { CMD_PING = 1, CMD_SET_RELAY = 3, CMD_GET_STATUS = 4, CMD_SET_FAN = 5, CMD_FAN_HOME = 6 };
enum CmdStatus : uint8_t { STATUS_OK = 0, STATUS_UNKNOWN_CMD = 1, STATUS_BAD_RELAY = 2, STATUS_NO_FAN = 3 };
enum FanFlag : uint8_t { FAN_PRESENT = 1, FAN_HOMING = 2 };

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

const uint16_t SLOT_MS = 60;               // must match the master

// The ESP32 has no real EEPROM: the library keeps a copy in RAM and writes it to flash on
// commit(). Values are only written when they changed, so flash isn't worn needlessly.
const size_t EEPROM_SIZE = 256;
const uint8_t EEPROM_RELAY_MARKER = 0xA5;  // EEPROM[3] = marker, [4] = relay bits
const int EEPROM_RELAY_ADDR = 3;
const uint8_t EEPROM_FAN_MARKER = 0xA6;    // EEPROM[5] = marker, [6] = fan level
const int EEPROM_FAN_ADDR = 5;

uint16_t nodeId;
uint8_t registeredScan = 0;   // scan ID of the master's last WELCOME
uint8_t lastCmdSeq = 0;       // to spot a resent command and not run it twice
Packet lastAck;
uint8_t relayBits = 0;        // bit 0 = relay 1 ... bit 3 = relay 4; 1 = on

uint8_t fanLevel = 0;         // level asked for, 0-100
bool fanHoming = false;
uint16_t fanPos = 0;          // knob position in half-steps from OFF
uint16_t fanTarget = 0;
uint8_t fanPhase = 0;         // index into FAN_SEQUENCE
unsigned long fanLastStep = 0;

// Folds the chip's 6-byte factory MAC address into 16 bits (FNV-1a hash), avoiding the master
// and broadcast addresses. Two nodes could in theory get the same ID; use FIXED_NODE_ID then.
uint16_t loadNodeId() {
  if (FIXED_NODE_ID != 0) return FIXED_NODE_ID;

  uint64_t mac = ESP.getEfuseMac();
  uint32_t hash = 2166136261u;
  for (uint8_t i = 0; i < 6; i++) {
    hash ^= (uint8_t)(mac >> (8 * i));
    hash *= 16777619u;
  }
  uint16_t id = (hash >> 16) ^ (hash & 0xFFFF);
  if (id == MASTER_ID || id == BROADCAST_ID) id = 0x0001;
  return id;
}

void writeRelayPin(uint8_t i) {
  bool on = relayBits & (1 << i);
  digitalWrite(RELAY_PINS[i], on == RELAY_ACTIVE_HIGH[i] ? HIGH : LOW);
}

void setupRelays() {
  if (EEPROM.read(EEPROM_RELAY_ADDR) == EEPROM_RELAY_MARKER) {
    relayBits = EEPROM.read(EEPROM_RELAY_ADDR + 1) & 0x0F;
  }
  // Set the level before switching the pin to output, so relays don't click at power-up.
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    writeRelayPin(i);
    pinMode(RELAY_PINS[i], OUTPUT);
  }
}

void saveRelays() {
  EEPROM.write(EEPROM_RELAY_ADDR, EEPROM_RELAY_MARKER);
  EEPROM.write(EEPROM_RELAY_ADDR + 1, relayBits);
  EEPROM.commit();
}

// Half-step coil pattern for IN1..IN4, bit 0 = IN1.
const uint8_t FAN_SEQUENCE[8] = { 0b0001, 0b0011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001 };

void writeFanCoils(uint8_t pattern) {
  for (uint8_t i = 0; i < 4; i++) digitalWrite(FAN_PINS[i], pattern & (1 << i) ? HIGH : LOW);
}

// Moves the target; fanService() then walks the motor there without blocking LoRa.
void setFanTarget(uint8_t level) {
  fanLevel = level;
  if (!fanHoming) fanTarget = (uint32_t)level * FAN_RANGE_STEPS / 100;
  EEPROM.write(EEPROM_FAN_ADDR, EEPROM_FAN_MARKER);
  EEPROM.write(EEPROM_FAN_ADDR + 1, level);
  EEPROM.commit();
}

// Pretends the knob is past full speed and drives it to 0, so it ends up pressed against OFF.
void startFanHoming() {
  fanHoming = true;
  fanPos = FAN_HOME_STEPS;
  fanTarget = 0;
}

void setupFan() {
  if (!HAS_FAN) return;
  for (uint8_t i = 0; i < 4; i++) pinMode(FAN_PINS[i], OUTPUT);
  writeFanCoils(0);
  if (EEPROM.read(EEPROM_FAN_ADDR) == EEPROM_FAN_MARKER) {
    fanLevel = min(EEPROM.read(EEPROM_FAN_ADDR + 1), (uint8_t)100);
  }
  startFanHoming();
}

// Call often: takes at most one step each time it is due.
void fanService() {
  if (!HAS_FAN) return;
  if (fanPos == fanTarget) {
    if (fanHoming) {
      fanHoming = false;
      setFanTarget(fanLevel);   // homed: now go to the level asked for
    } else {
      writeFanCoils(0);         // switch the coils off when idle; the knob's friction holds it
    }
    return;
  }
  unsigned long now = micros();
  if (now - fanLastStep < (fanHoming ? FAN_HOME_STEP_US : FAN_STEP_US)) return;
  fanLastStep = now;

  bool up = fanTarget > fanPos;
  fanPos += up ? 1 : -1;
  fanPhase = (fanPhase + ((up != FAN_REVERSE) ? 1 : 7)) & 7;
  writeFanCoils(FAN_SEQUENCE[fanPhase]);
}

uint8_t fanFlags() {
  return (HAS_FAN ? FAN_PRESENT : 0) | (fanHoming ? FAN_HOMING : 0);
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

uint8_t setRelay(uint8_t relay, uint8_t state) {
  if (relay > RELAY_COUNT || state > 2) return STATUS_BAD_RELAY;

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (relay != 0 && relay != i + 1) continue;
    uint8_t mask = 1 << i;
    if (state == 2) relayBits ^= mask;
    else if (state == 1) relayBits |= mask;
    else relayBits &= ~mask;
    writeRelayPin(i);
  }
  saveRelays();
  return STATUS_OK;
}

uint8_t handleCommand(const Packet &p) {
  if (p.len < 1) return STATUS_UNKNOWN_CMD;
  switch (p.data[0]) {
    case CMD_PING:
    case CMD_GET_STATUS:
      return STATUS_OK;
    case CMD_SET_RELAY:
      if (p.len < 3) return STATUS_BAD_RELAY;
      return setRelay(p.data[1], p.data[2]);
    case CMD_SET_FAN:
      if (!HAS_FAN) return STATUS_NO_FAN;
      if (p.len < 2 || p.data[1] > 100) return STATUS_BAD_RELAY;
      setFanTarget(p.data[1]);
      return STATUS_OK;
    case CMD_FAN_HOME:
      if (!HAS_FAN) return STATUS_NO_FAN;
      startFanHoming();
      return STATUS_OK;
    default:
      return STATUS_UNKNOWN_CMD;
  }
}

void printRelays() {
  Serial.print("Relays:");
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    Serial.print(' ');
    Serial.print(i + 1);
    Serial.print(relayBits & (1 << i) ? "=ON" : "=OFF");
  }
  if (HAS_FAN) {
    Serial.print("  Fan: ");
    Serial.print(fanLevel);
    Serial.print('%');
    if (fanHoming) Serial.print(" (finding OFF)");
  }
  Serial.println();
}

void printNodeId() {
  if (nodeId < 0x1000) Serial.print('0');
  if (nodeId < 0x100) Serial.print('0');
  if (nodeId < 0x10) Serial.print('0');
  Serial.println(nodeId, HEX);
}

// If nothing at all is heard for this long, the radio is set up again from scratch, in case
// it came up deaf. Re-initialising takes about 20 ms, so a working radio loses nothing.
const unsigned long RADIO_SILENCE_RESET_MS = 60000;
unsigned long lastHeard = 0;

bool startRadio() {
  // The LoRa library never sets up DIO0 (it only would with onReceive()), so do it here.
  // The pull-down keeps the pin LOW if the wire comes loose.
  pinMode(LORA_DIO0, INPUT_PULLDOWN);
  // On 3.3 V the library can drive RST itself (it pulses it LOW, then HIGH).
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(LORA_SPI_HZ);
  if (!LoRa.begin(LORA_BAND)) return false;
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.receive();   // continuous receive; DIO0 goes HIGH when a packet has arrived
  return true;
}

void setup() {
  EEPROM.begin(EEPROM_SIZE);
  setupRelays();   // first, so relays reach their saved state as early as possible
  setupFan();

  Serial.begin(115200);

  nodeId = loadNodeId();
  randomSeed(esp_random() ^ nodeId);   // different nodes pick different discovery slots

  Serial.println("LoRa ESP32 Node Initializing");
  printRelays();
  delay(100);   // let the LoRa module finish powering up
  while (!startRadio()) {
    // The version register should read 0x12. 0x00 = MISO dead or no power; 0xFF = MISO stuck
    // HIGH or NSS not reaching the module; anything else = SCK/MOSI/NSS swapped or a bad contact.
    Serial.print("Starting LoRa failed! Version register reads 0x");
    Serial.print(readRadioRegister(0x42), HEX);
    Serial.println(" (should be 0x12). Check wiring. Retrying in 2 s...");
    delay(2000);
  }
  lastHeard = millis();

  Serial.print("LoRa ESP32 Node Ready. My ID: ");
  printNodeId();
}

// Reads one radio register directly (the LoRa library keeps its own reader private).
uint8_t readRadioRegister(uint8_t reg) {
  SPI.beginTransaction(SPISettings(LORA_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return value;
}

// DIO0 goes HIGH when a packet has arrived. As a fallback (loose DIO0 wire), the radio's
// RxDone flag is also checked over SPI every few milliseconds, which is cheap on direct wires.
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
    Serial.println("Warning: packet arrived but DIO0 stayed LOW - check the DIO0 -> GPIO2 wire.");
  }
  return true;
}

// Serial commands: "info" = ID and outputs, "radio" = LoRa register dump.
void serialService() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 32) line += c;
      continue;
    }
    line.trim();
    if (line == "info") {
      Serial.print("My ID: ");
      printNodeId();
      printRelays();
    } else if (line == "radio") {
      LoRa.dumpRegisters(Serial);
      LoRa.receive();
    } else if (line.length()) {
      Serial.println("Commands: info, radio");
    }
    line = "";
  }
}

void loop() {
  fanService();
  serialService();

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
  LoRa.receive();   // back to continuous receive, also after any reply we sent
}

void handlePacket(const Packet &p) {
  if (p.type == MSG_DISCOVER && p.dst == BROADCAST_ID && p.len >= 2) {
    uint8_t scan = p.data[0];
    if (scan == registeredScan) return;   // already registered in this scan
    uint8_t slots = p.data[1] ? p.data[1] : 1;
    delay((unsigned long)random(slots) * SLOT_MS);
    reply(p, MSG_HELLO, &scan, 1);
    Serial.println("Answered discovery");
    return;
  }

  if (p.dst != nodeId) return;

  if (p.type == MSG_WELCOME && p.len >= 1) {
    registeredScan = p.data[0];
    lastCmdSeq = 0;   // master may have restarted; start duplicate tracking afresh
    reply(p, MSG_ACK, nullptr, 0);
    Serial.println("Registered with master");
    return;
  }

  if (p.type == MSG_CMD) {
    if (p.seq == lastCmdSeq && lastAck.seq == p.seq) {
      // Master didn't hear our ACK and resent the command: answer again without repeating it,
      // so a "toggle" doesn't switch twice.
      delay(50);
      sendPacket(lastAck);
      return;
    }
    lastCmdSeq = p.seq;
    uint8_t status = handleCommand(p);
    uint8_t answer[4] = { status, relayBits, fanLevel, fanFlags() };
    reply(p, MSG_ACK, answer, 4);
    Serial.print("Command ");
    Serial.print(p.data[0]);
    Serial.print(answer[0] == STATUS_OK ? " done. " : " rejected. ");
    printRelays();
  }
}
