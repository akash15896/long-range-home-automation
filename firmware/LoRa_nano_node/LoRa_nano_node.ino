// LoRa node (Arduino Nano). Upload the same sketch to every node.
// Each node creates a random 4-digit hex ID the first time it runs and keeps it in EEPROM,
// so IDs stay the same after power cycles. The ID is printed at startup (9600 baud).
//
// Answers the master's DISCOVER with HELLO, then acknowledges every command addressed to it.
// Controls a 4-relay module on RELAY_PINS. Relay states are saved in EEPROM and restored
// after a power cut. Every reply carries the current state of all 4 relays.
//
// Optional ceiling fan control (set HAS_FAN to 1 on the node that has it): a 28BYJ-48 stepper
// on a ULN2003 board turns a smooth fan regulator knob to a level of 0-100%. There is no
// position sensor, so after power-up the motor turns past OFF against the knob's end stop
// (the motor just skips steps there) and counts that as 0%, then returns to the saved level.

#include <SPI.h>
#include <LoRa.h>
#include <EEPROM.h>

// LoRa wiring on a 5 V Nano: NSS (D10), SCK (D13) and MOSI (D11) go through the 4-channel
// BSS138 level converter. MISO (D12) and DIO0 (D2) are direct wires. RST (D9) is also direct:
// the sketch only ever pulls it LOW or lets it float (the module has its own pull-up), so 5 V
// never reaches the module.
#define LORA_SS 10
#define LORA_RST 9
#define LORA_DIO0 2
// The converter garbles SPI above about 250 kHz (and a garbled read can turn into a write that
// corrupts the radio's settings), so stay well below that.
#define LORA_SPI_HZ 50000

#define LORA_BAND 433E6
#define LORA_SYNC_WORD 0x12   // must match the master (0x12 is the library default)
// Transmit power in dBm (2-20). 17 is the library default and works on the same desk too.
#define LORA_TX_POWER 5

// Relay module inputs IN1..IN4. D10-D13, D9 and D2 are taken by the LoRa module; A0-A3 by the fan.
const uint8_t RELAY_COUNT = 4;
const uint8_t RELAY_PINS[RELAY_COUNT] = { 4, 5, 6, 7 };
// true = relay switches on when the pin is HIGH; false = on when LOW.
const bool RELAY_ACTIVE_HIGH[RELAY_COUNT] = { false, false, false, false };

// Set to a value from 0x0001 to 0xFFFE to choose this node's ID yourself; 0 = automatic.
#define FIXED_NODE_ID 0

// ---- fan regulator (28BYJ-48 + ULN2003) ----
#define HAS_FAN 1   // 1 = this node has the fan stepper
// ULN2003 board inputs IN1..IN4.
const uint8_t FAN_PINS[4] = { A0, A1, A2, A3 };
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
const uint8_t EEPROM_ID_MARKER = 0x5A;     // EEPROM[0] = marker, [1..2] = node ID
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

uint16_t loadNodeId() {
  if (FIXED_NODE_ID != 0) return FIXED_NODE_ID;

  if (EEPROM.read(0) == EEPROM_ID_MARKER) {
    uint16_t id = (EEPROM.read(1) << 8) | EEPROM.read(2);
    if (id != MASTER_ID && id != BROADCAST_ID) return id;
  }

  // First run: mix floating analog pins and timing into a seed, then store a random ID.
  uint32_t seed = micros();
  for (uint8_t i = 0; i < 32; i++) seed = (seed << 1) ^ analogRead(A0 + (i % 6)) ^ micros();
  randomSeed(seed);
  uint16_t id = random(1, 0xFFFF);   // 0x0001 .. 0xFFFE
  EEPROM.update(0, EEPROM_ID_MARKER);
  EEPROM.update(1, id >> 8);
  EEPROM.update(2, id & 0xFF);
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
  // update() only writes bytes that changed, which saves EEPROM wear.
  EEPROM.update(EEPROM_RELAY_ADDR, EEPROM_RELAY_MARKER);
  EEPROM.update(EEPROM_RELAY_ADDR + 1, relayBits);
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
  EEPROM.update(EEPROM_FAN_ADDR, EEPROM_FAN_MARKER);
  EEPROM.update(EEPROM_FAN_ADDR + 1, level);
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
  memcpy(p.data, data, len);
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

// If nothing at all is heard for this long, the radio is set up again from scratch. A radio
// configured while its 3.3 V supply was still rising can come up deaf; this clears that
// without a restart. Re-initialising takes about 20 ms, so a working radio loses nothing.
const unsigned long RADIO_SILENCE_RESET_MS = 60000;
unsigned long lastHeard = 0;

// Hardware reset without ever driving RST high: pull it LOW, then release it.
void resetRadio() {
  digitalWrite(LORA_RST, LOW);
  pinMode(LORA_RST, OUTPUT);
  delay(1);
  pinMode(LORA_RST, INPUT);
  delay(10);   // the module needs 5 ms after reset before it accepts SPI
}

bool startRadio() {
  resetRadio();
  LoRa.setPins(LORA_SS, -1, LORA_DIO0);   // -1: the library must not drive RST itself
  LoRa.setSPIFrequency(LORA_SPI_HZ);
  if (!LoRa.begin(LORA_BAND)) return false;
  // begin() already sets 125 kHz, SF7, CR 4/5, preamble 8. Setting them again would mean more
  // read-modify-write SPI transfers over the level converter, each a chance to garble a register.
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.receive();   // continuous receive; DIO0 goes HIGH when a packet has arrived
  return true;
}

void setup() {
  setupRelays();   // first, so relays reach their saved state as early as possible
  Serial.begin(9600);

  nodeId = loadNodeId();
  randomSeed(nodeId ^ micros());   // different nodes pick different discovery slots
  setupFan();   // after loadNodeId, which reads A0-A3 for randomness on the first run

  Serial.println("LoRa Node Initializing");
  printRelays();
  delay(250);   // let the 3.3 V regulator and the LoRa module finish powering up
  if (!startRadio()) {
    Serial.println("Starting LoRa failed! Check Wiring");
    while (1);
  }
  lastHeard = millis();

  Serial.print("LoRa Node Ready. My ID: ");
  if (nodeId < 0x1000) Serial.print('0');
  if (nodeId < 0x100) Serial.print('0');
  if (nodeId < 0x10) Serial.print('0');
  Serial.println(nodeId, HEX);
}

void loop() {
  fanService();

  if (millis() - lastHeard > RADIO_SILENCE_RESET_MS) {
    startRadio();
    lastHeard = millis();
  }

  // Only talk to the radio when DIO0 says a packet has arrived. Polling it over SPI thousands
  // of times a second (as LoRa.parsePacket() alone would) gives the level converter that
  // many chances to garble a transfer and corrupt the radio's settings.
  if (digitalRead(LORA_DIO0) == LOW) return;
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
