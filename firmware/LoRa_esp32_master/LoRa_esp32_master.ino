// LoRa master (ESP32).
// 1. "scan" broadcasts DISCOVER; every node answers HELLO with its ID, and the master
//    registers each one (WELCOME + ACK). Nodes reply in random time slots so they don't collide.
// 2. Afterwards the master talks to each node separately: it sends a command, the node
//    acknowledges, and the master resends up to 3 times if no ACK arrives.
//
// Type these in the Serial Monitor (115200 baud, "Newline" line ending):
//   scan            find nodes
//   list            show registered nodes
//   ping <id|all>   check a node is reachable, e.g. "ping 3F2A"
//   status <id|all> show the node's 4 relay states
//   relay <id|all> <1-4|all> <on|off|toggle>
//                   e.g. "relay 3F2A 2 on", "relay all all off"
//   fan <id> <0-100|home>
//                   set the fan regulator level in %, e.g. "fan 3F2A 60",
//                   or "fan 3F2A home" to find OFF again if the knob slipped
//   curtain <id|all> <open|close|stop|learn|0-100>
//                   curtain node: run to a limit switch, stop, learn the travel
//                   (closed -> open), or go to a position in % open, e.g. "curtain 3F2A 50"
//   dim <id|all> <0-100|on|off|next>
//                   dimmer node: set the bulb brightness in %, or step to the next level
//                   like its push button does, e.g. "dim 3F2A 50"
//   wifi            show Wi-Fi status
//   wifi reset      forget the saved Wi-Fi and start Bluetooth setup again
//   led <on|off>    switch the LED on STATUS_LED_PIN
//   server          show the automation server connection
//   radio           print the LoRa module's registers (to check its settings)
//   txpower <2-20>  set LoRa transmit power in dBm until restart (default 17);
//                   use a low value when testing with the node on the same desk
//
// Automation server: the master keeps an encrypted WebSocket open to SERVER_HOST (secrets.h)
// and runs the commands the web page sends: led, relay, fan, curtain, dim, status, scan. Results go back to
// the server, including results of commands typed here, so the page always matches.
// Wi-Fi setup over Bluetooth: on first start (or after "wifi reset", or holding BOOT for
// 10 seconds) the master waits for the "ESP BLE Provisioning" phone app from Espressif.
// Scan the QR code printed in the Serial Monitor, or pick PROV_DEVICE_NAME and enter
// PROV_PIN, then choose your Wi-Fi network and type its password. It is saved in flash.
//
// Needs Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"; with the default
// scheme the sketch is too big.

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include "WiFiProv.h"
#include <WebSocketsClient.h>   // "WebSockets" library by Markus Sattler
#include <ArduinoJson.h>
#include "secrets.h"            // server address, device token and PROV_PIN (copy secrets.example.h)
#include "root_ca.h"            // certificates used to check the server is genuine

#ifndef PROV_PIN
#error "Define PROV_PIN in secrets.h (see secrets.example.h)"
#endif

#define PROV_DEVICE_NAME "PROV_LoRaMaster"   // the app lists devices whose name starts with "PROV_"
#define WIFI_RESET_BUTTON 0                  // BOOT button
#define STATUS_LED_PIN 4                     // LED + 220 ohm resistor to GND

#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

#define LORA_BAND 433E6
#define LORA_SYNC_WORD 0x12   // must match the nodes (0x12 is the library default)

// ---- protocol (keep identical in LoRa_nano_node) ----
const uint8_t MAGIC = 0xA7;
const uint16_t MASTER_ID = 0x0000;
const uint16_t BROADCAST_ID = 0xFFFF;

enum MsgType : uint8_t { MSG_DISCOVER = 1, MSG_HELLO = 2, MSG_WELCOME = 3, MSG_CMD = 4, MSG_ACK = 5 };
// CMD_SET_RELAY data: relay (1-4, 0 = all), state (0 off, 1 on, 2 toggle).
// CMD_SET_FAN data: level (0-100). CMD_FAN_HOME: find OFF again, then return to the level.
// ACK data for every command: status (see CmdStatus), relay bits (bit 0 = relay 1),
// fan level (0-100), fan flags (see FanFlag); curtain nodes add curtain position
// (0-100, 0xFF = unknown) and curtain flags (see CurtainFlag).
// CMD_CURTAIN data: action (see CurtainAction), percent (0-100, only for CURTAIN_GOTO).
// CMD_DIMMER data: level (0-100), action (see DimmerAction). Dimmer nodes send 8 ACK bytes:
// the 6 above (curtain flags 0), then dimmer level (0-100) and dimmer flags (see DimmerFlag).
enum Command : uint8_t {
  CMD_PING = 1, CMD_SET_RELAY = 3, CMD_GET_STATUS = 4, CMD_SET_FAN = 5, CMD_FAN_HOME = 6, CMD_CURTAIN = 7,
  CMD_DIMMER = 8
};
enum CmdStatus : uint8_t {
  STATUS_OK = 0, STATUS_UNKNOWN_CMD = 1, STATUS_BAD_RELAY = 2, STATUS_NO_FAN = 3, STATUS_NO_CURTAIN = 4,
  STATUS_CURTAIN_FAULT = 5, STATUS_NO_DIMMER = 6
};
enum FanFlag : uint8_t { FAN_PRESENT = 1, FAN_HOMING = 2 };
enum CurtainAction : uint8_t {
  CURTAIN_STOP = 0, CURTAIN_OPEN = 1, CURTAIN_CLOSE = 2, CURTAIN_GOTO = 3, CURTAIN_LEARN = 4
};
enum CurtainFlag : uint8_t {
  CURTAIN_PRESENT = 1, CURTAIN_MOVING = 2, CURTAIN_KNOWN = 4, CURTAIN_LEARNED = 8,
  CURTAIN_AT_OPEN = 16, CURTAIN_AT_CLOSED = 32, CURTAIN_FAULT = 64, CURTAIN_LEARNING = 128
};
const uint8_t CURTAIN_UNKNOWN = 0xFF;
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

// Discovery timing: nodes pick one of DISCOVERY_SLOTS slots, each SLOT_MS long.
const uint8_t DISCOVERY_SLOTS = 32;
const uint16_t SLOT_MS = 60;
const uint8_t MAX_DISCOVERY_ROUNDS = 8;

const uint8_t ACK_RETRIES = 3;
const uint16_t ACK_TIMEOUT_MS = 600;

int8_t txPowerDbm = 17;   // changed by the "txpower" command until restart

// Sets the radio up from scratch. Also used to recover a radio that came up deaf.
bool startRadio() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_BAND)) return false;
  // Set every modem setting explicitly (all must match the nodes): a module that was not
  // properly reset can keep stale values, e.g. a 500 kHz bandwidth, and then hears nothing.
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSpreadingFactor(7);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(txPowerDbm);
  return true;
}

const uint8_t MAX_NODES = 32;
uint16_t nodes[MAX_NODES];
int16_t nodeRssi[MAX_NODES];   // signal of each node's last reply during scan
uint8_t nodeCount = 0;

bool ledOn = false;

// ---- automation server link ----
WebSocketsClient server;

// Commands from the server wait here and run one at a time from loop(), so a command that
// arrives during a long LoRa scan doesn't interrupt it.
struct ServerCommand {
  uint32_t id;
  char action[8];   // led, relay, fan, curtain, dim, status, scan
  char node[5];     // node ID or "all"
  uint8_t relay;    // 1-4, 0 = all
  char state[7];    // on, off, toggle; fan: set, home; curtain: open, close, stop, learn, goto;
                    // dim: set, next, on, off
  uint8_t level;    // fan level, curtain position or dimmer level, 0-100
};
const uint8_t QUEUE_SIZE = 8;
ServerCommand queue[QUEUE_SIZE];
uint8_t queueHead = 0, queueCount = 0;
void runServerCommand(const ServerCommand &c);   // declared here because it uses ServerCommand

void sendToServer(JsonDocument &doc) {
  if (!server.isConnected()) return;
  String text;
  serializeJson(doc, text);
  server.sendTXT(text);
}

// Keeps the server connection alive (answers pings, receives commands) while LoRa waits.
void serviceNetwork() {
  server.loop();
}

uint8_t nextSeq = 1;
uint8_t scanId = 0;

// Sequence numbers skip 0, which nodes treat as "no command seen yet".
uint8_t newSeq() {
  if (nextSeq == 0) nextSeq = 1;
  return nextSeq++;
}

bool isRegistered(uint16_t id) {
  for (uint8_t i = 0; i < nodeCount; i++) {
    if (nodes[i] == id) return true;
  }
  return false;
}

// Sends p to one node and waits for its ACK, resending if needed.
// On success, reply holds the ACK (its data carries the node's answer).
bool sendWithAck(Packet &p, Packet &reply) {
  p.src = MASTER_ID;
  p.seq = newSeq();

  for (uint8_t attempt = 1; attempt <= ACK_RETRIES; attempt++) {
    sendPacket(p);
    unsigned long start = millis();
    while (millis() - start < ACK_TIMEOUT_MS) {
      if (receivePacket(reply) && reply.type == MSG_ACK && reply.dst == MASTER_ID &&
          reply.src == p.dst && reply.seq == p.seq) {
        return true;
      }
      serviceNetwork();
    }
    Serial.printf("  no ACK from %04X (attempt %u/%u)\n", p.dst, attempt, ACK_RETRIES);
  }
  // In case our own radio is the deaf one, set it up again before the next command.
  startRadio();
  return false;
}

void scanNodes() {
  // A new scan ID makes every node answer again, including ones registered before.
  uint8_t previous = scanId;
  do { scanId = random(1, 256); } while (scanId == previous);
  nodeCount = 0;

  Serial.println("Scanning for nodes...");
  uint8_t emptyRounds = 0;

  for (uint8_t round = 1; round <= MAX_DISCOVERY_ROUNDS && emptyRounds < 2; round++) {
    Packet p;
    p.type = MSG_DISCOVER;
    p.dst = BROADCAST_ID;
    p.src = MASTER_ID;
    p.seq = newSeq();
    p.len = 2;
    p.data[0] = scanId;
    p.data[1] = DISCOVERY_SLOTS;
    sendPacket(p);

    // Collect HELLOs during the reply window.
    uint16_t heard[MAX_NODES];
    uint8_t heardCount = 0;
    unsigned long start = millis();
    unsigned long window = (unsigned long)DISCOVERY_SLOTS * SLOT_MS + 200;
    Packet in;
    while (millis() - start < window) {
      if (receivePacket(in) && in.type == MSG_HELLO && in.dst == MASTER_ID &&
          in.len >= 1 && in.data[0] == scanId && !isRegistered(in.src)) {
        bool dup = false;
        for (uint8_t i = 0; i < heardCount; i++) dup |= heard[i] == in.src;
        if (!dup && heardCount < MAX_NODES) heard[heardCount++] = in.src;
      }
      serviceNetwork();
    }

    // Register each new node. Registered nodes stop answering this scan's DISCOVERs,
    // so later rounds only hear nodes whose HELLO collided earlier.
    uint8_t added = 0;
    for (uint8_t i = 0; i < heardCount && nodeCount < MAX_NODES; i++) {
      Packet w, ack;
      w.type = MSG_WELCOME;
      w.dst = heard[i];
      w.len = 1;
      w.data[0] = scanId;
      if (sendWithAck(w, ack)) {
        nodeRssi[nodeCount] = LoRa.packetRssi();
        nodes[nodeCount++] = heard[i];
        added++;
        Serial.printf("  found node %04X  (RSSI %d dBm)\n", heard[i], LoRa.packetRssi());
      }
    }

    Serial.printf("Round %u: %u new node(s)\n", round, added);
    emptyRounds = added == 0 ? emptyRounds + 1 : 0;
  }

  Serial.printf("Scan done: %u node(s) registered. Type \"list\" to see them.\n", nodeCount);
  reportNodes();
}

void reportNodes() {
  JsonDocument doc;
  doc["type"] = "nodes";
  JsonArray list = doc["list"].to<JsonArray>();
  char id[5];
  for (uint8_t i = 0; i < nodeCount; i++) {
    JsonObject n = list.add<JsonObject>();
    snprintf(id, sizeof(id), "%04X", nodes[i]);
    n["id"] = id;
    n["rssi"] = nodeRssi[i];
  }
  sendToServer(doc);
}

void listNodes() {
  if (nodeCount == 0) {
    Serial.println("No nodes registered. Type \"scan\" first.");
    return;
  }
  Serial.printf("%u node(s):\n", nodeCount);
  for (uint8_t i = 0; i < nodeCount; i++) Serial.printf("  %04X\n", nodes[i]);
}

const char *commandName(uint8_t cmd) {
  switch (cmd) {
    case CMD_SET_RELAY: return "relay";
    case CMD_GET_STATUS: return "status";
    case CMD_SET_FAN:
    case CMD_FAN_HOME: return "fan";
    case CMD_CURTAIN: return "curtain";
    case CMD_DIMMER: return "dim";
    default: return "ping";
  }
}

// Prints a curtain node's state and adds it to the server result as "curtain": {...}.
void reportCurtain(JsonDocument &doc, uint16_t id, uint8_t position, uint8_t flags) {
  bool known = position != CURTAIN_UNKNOWN;
  bool moving = flags & CURTAIN_MOVING;
  bool learning = flags & CURTAIN_LEARNING;
  bool fault = flags & CURTAIN_FAULT;
  if (known) Serial.printf("Node %04X curtain: %u%% open", id, position);
  else Serial.printf("Node %04X curtain: position unknown", id);
  if (learning) Serial.print(", learning travel");
  else if (moving) Serial.print(", moving");
  if (!(flags & CURTAIN_LEARNED)) Serial.print(", travel not learned yet");
  if (fault) Serial.print(", FAULT (see the node's USB serial \"info\")");
  Serial.println();

  JsonObject c = doc["curtain"].to<JsonObject>();
  if (known) c["position"] = position;
  else c["position"] = nullptr;
  c["moving"] = moving;
  c["learning"] = learning;
  c["learned"] = (bool)(flags & CURTAIN_LEARNED);
  c["open"] = (bool)(flags & CURTAIN_AT_OPEN);     // OPEN limit switch pressed
  c["closed"] = (bool)(flags & CURTAIN_AT_CLOSED); // CLOSED limit switch pressed
  c["fault"] = fault;
}

// Prints a dimmer node's state and adds it to the server result as "dimmer": {...}.
void reportDimmer(JsonDocument &doc, uint16_t id, uint8_t level, uint8_t flags) {
  bool mains = flags & DIMMER_MAINS_OK;
  Serial.printf("Node %04X dimmer: %u%%%s%s\n", id, level, flags & DIMMER_FADING ? " (fading)" : "",
                mains ? "" : ", NO MAINS at the dimmer (zero-cross missing)");
  JsonObject d = doc["dimmer"].to<JsonObject>();
  d["level"] = level;
  d["mains"] = mains;
  d["fading"] = (bool)(flags & DIMMER_FADING);
}

// Parses "on", "off", "next" or a brightness 0-100 into level and DimmerAction.
bool parseDimmer(const String &what, uint8_t &level, uint8_t &action) {
  level = 0;
  action = DIMMER_SET;
  if (what == "on") level = 100;
  else if (what == "off") level = 0;
  else if (what == "next") action = DIMMER_NEXT;
  else {
    long p = what.toInt();
    if (p < 0 || p > 100 || (p == 0 && what != "0")) return false;
    level = p;
  }
  return true;
}

// Parses "open", "close", "stop", "learn" or a percentage 0-100. Returns false if it's none of them.
bool parseCurtain(const String &what, uint8_t &action, uint8_t &percent) {
  percent = 0;
  if (what == "open") action = CURTAIN_OPEN;
  else if (what == "close") action = CURTAIN_CLOSE;
  else if (what == "stop") action = CURTAIN_STOP;
  else if (what == "learn") action = CURTAIN_LEARN;
  else {
    long p = what.toInt();
    if (p < 0 || p > 100 || (p == 0 && what != "0")) return false;
    action = CURTAIN_GOTO;
    percent = p;
  }
  return true;
}

// Sends one command to a node, prints the outcome and reports it to the server.
// arg1/arg2: relay and state for CMD_SET_RELAY, level and 0 for CMD_SET_FAN.
// requestId is the server's command id, or 0 for commands typed in the Serial Monitor.
void sendCommand(uint16_t id, uint8_t cmd, uint8_t arg1, uint8_t arg2, uint32_t requestId) {
  Packet p, ack;
  p.type = MSG_CMD;
  p.dst = id;
  p.len = 3;
  p.data[0] = cmd;
  p.data[1] = arg1;
  p.data[2] = arg2;

  JsonDocument doc;
  char nodeId[5];
  snprintf(nodeId, sizeof(nodeId), "%04X", id);
  doc["type"] = "result";
  doc["id"] = requestId;
  doc["action"] = commandName(cmd);
  doc["node"] = nodeId;

  if (!sendWithAck(p, ack)) {
    Serial.printf("Node %04X did not answer.\n", id);
    doc["ok"] = false;
    doc["error"] = "did not answer";
    sendToServer(doc);
    return;
  }
  uint8_t status = ack.len >= 1 ? ack.data[0] : 0xFF;
  uint8_t bits = ack.len >= 2 ? ack.data[1] : 0;
  // Curtain nodes send 6 bytes; their status is reported even when a command is refused.
  uint8_t curtainFlags = ack.len >= 6 ? ack.data[5] : 0;
  if (curtainFlags & CURTAIN_PRESENT) reportCurtain(doc, id, ack.data[4], curtainFlags);
  // Dimmer nodes send 8 bytes.
  uint8_t dimmerFlags = ack.len >= 8 ? ack.data[7] : 0;
  if (dimmerFlags & DIMMER_PRESENT) reportDimmer(doc, id, ack.data[6], dimmerFlags);
  doc["ok"] = status == STATUS_OK;
  if (status == STATUS_OK) {
    if ((curtainFlags & CURTAIN_PRESENT) || (dimmerFlags & DIMMER_PRESENT))
      Serial.printf("  (RSSI %d dBm, SNR %.1f dB)\n", LoRa.packetRssi(), LoRa.packetSnr());
    else
      Serial.printf("Node %04X relays: 1=%s 2=%s 3=%s 4=%s  (RSSI %d dBm, SNR %.1f dB)\n", id,
                    bits & 1 ? "ON" : "OFF", bits & 2 ? "ON" : "OFF", bits & 4 ? "ON" : "OFF", bits & 8 ? "ON" : "OFF",
                    LoRa.packetRssi(), LoRa.packetSnr());
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) relays.add((bits >> i) & 1 ? true : false);
    doc["rssi"] = LoRa.packetRssi();
    doc["snr"] = LoRa.packetSnr();
    // Nodes with older firmware send only 2 bytes and have no fan.
    uint8_t fanFlags = ack.len >= 4 ? ack.data[3] : 0;
    if (fanFlags & FAN_PRESENT) {
      uint8_t level = ack.data[2];
      bool homing = fanFlags & FAN_HOMING;
      Serial.printf("Node %04X fan: %u%%%s\n", id, level, homing ? " (finding OFF first)" : "");
      JsonObject fan = doc["fan"].to<JsonObject>();
      fan["level"] = level;
      fan["homing"] = homing;
    }
  } else if (status == STATUS_BAD_RELAY) {
    Serial.printf("Node %04X: invalid relay number, state or fan level.\n", id);
    doc["error"] = "invalid relay number, state or fan level";
  } else if (status == STATUS_NO_FAN) {
    Serial.printf("Node %04X has no fan regulator (HAS_FAN is 0 in its sketch).\n", id);
    doc["error"] = "has no fan regulator";
  } else if (status == STATUS_CURTAIN_FAULT) {
    Serial.printf("Node %04X refused: both curtain limit switches read pressed - check their wiring.\n", id);
    doc["error"] = "both curtain limit switches read pressed; check their wiring";
  } else if (cmd == CMD_CURTAIN && (status == STATUS_UNKNOWN_CMD || status == STATUS_NO_CURTAIN)) {
    Serial.printf("Node %04X is not a curtain node.\n", id);
    doc["error"] = "is not a curtain node";
  } else if (cmd == CMD_DIMMER && (status == STATUS_UNKNOWN_CMD || status == STATUS_NO_DIMMER)) {
    Serial.printf("Node %04X is not a dimmer node.\n", id);
    doc["error"] = "is not a dimmer node";
  } else if (status == STATUS_UNKNOWN_CMD && (dimmerFlags & DIMMER_PRESENT)) {
    Serial.printf("Node %04X is a dimmer node: use \"dim\" (it has no relays, fan or curtain).\n", id);
    doc["error"] = "is a dimmer node; it has no relays, fan or curtain";
  } else {
    Serial.printf("Node %04X rejected the command (status %u). Is it running the latest node sketch?\n", id, status);
    doc["error"] = "rejected the command; is it running the latest node sketch?";
  }
  sendToServer(doc);
}

void setLed(bool on, uint32_t requestId) {
  ledOn = on;
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  Serial.printf("LED %s\n", on ? "ON" : "OFF");

  JsonDocument doc;
  doc["type"] = "result";
  doc["id"] = requestId;
  doc["action"] = "led";
  doc["ok"] = true;
  doc["led"] = on;
  sendToServer(doc);
}

void onServerEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.println("Connected to automation server.");
      JsonDocument hello;
      hello["type"] = "hello";
      hello["led"] = ledOn;
      sendToServer(hello);
      if (nodeCount > 0) reportNodes();
      break;
    }
    case WStype_DISCONNECTED:
      Serial.println("Automation server disconnected, retrying...");
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length) || doc["type"] != "command") break;
      if (queueCount == QUEUE_SIZE) {
        JsonDocument busy;
        busy["type"] = "result";
        busy["id"] = doc["id"];
        busy["ok"] = false;
        busy["error"] = "too many commands waiting; try again in a moment";
        sendToServer(busy);
        break;
      }
      ServerCommand &c = queue[(queueHead + queueCount) % QUEUE_SIZE];
      c.id = doc["id"].as<uint32_t>();
      strlcpy(c.action, doc["action"] | "", sizeof(c.action));
      strlcpy(c.node, doc["node"] | "", sizeof(c.node));
      c.relay = doc["relay"] | 0;
      strlcpy(c.state, doc["state"] | "", sizeof(c.state));
      c.level = doc["level"] | 0;
      queueCount++;
      break;
    }
    default:
      break;
  }
}

void runServerCommand(const ServerCommand &c) {
  if (strcmp(c.action, "led") == 0) {
    setLed(strcmp(c.state, "on") == 0, c.id);
    return;
  }
  if (strcmp(c.action, "scan") == 0) {
    scanNodes();
    for (uint8_t i = 0; i < nodeCount; i++) sendCommand(nodes[i], CMD_GET_STATUS, 0, 0, c.id);
    return;
  }

  uint8_t cmd, arg1 = 0, arg2 = 0;
  if (strcmp(c.action, "status") == 0) cmd = CMD_GET_STATUS;
  else if (strcmp(c.action, "relay") == 0) {
    cmd = CMD_SET_RELAY;
    arg1 = c.relay;
    arg2 = strcmp(c.state, "on") == 0 ? 1 : strcmp(c.state, "toggle") == 0 ? 2 : 0;
  } else if (strcmp(c.action, "fan") == 0) {
    if (strcmp(c.state, "home") == 0) cmd = CMD_FAN_HOME;
    else {
      cmd = CMD_SET_FAN;
      arg1 = min(c.level, (uint8_t)100);
    }
  } else if (strcmp(c.action, "curtain") == 0) {
    // state: open, close, stop, learn, or goto with level = 0-100
    cmd = CMD_CURTAIN;
    String what = strcmp(c.state, "goto") == 0 ? String(min(c.level, (uint8_t)100)) : String(c.state);
    if (!parseCurtain(what, arg1, arg2)) {
      JsonDocument bad;
      bad["type"] = "result";
      bad["id"] = c.id;
      bad["action"] = "curtain";
      bad["node"] = c.node;
      bad["ok"] = false;
      bad["error"] = "curtain state must be open, close, stop, learn or goto";
      sendToServer(bad);
      return;
    }
  } else if (strcmp(c.action, "dim") == 0) {
    // state: set with level = 0-100, next, on, off
    cmd = CMD_DIMMER;
    String what = strcmp(c.state, "set") == 0 ? String(min(c.level, (uint8_t)100)) : String(c.state);
    if (!parseDimmer(what, arg1, arg2)) {
      JsonDocument bad;
      bad["type"] = "result";
      bad["id"] = c.id;
      bad["action"] = "dim";
      bad["node"] = c.node;
      bad["ok"] = false;
      bad["error"] = "dim state must be set, next, on or off";
      sendToServer(bad);
      return;
    }
  } else return;

  if (strcmp(c.node, "all") == 0) {
    for (uint8_t i = 0; i < nodeCount; i++) sendCommand(nodes[i], cmd, arg1, arg2, c.id);
  } else {
    sendCommand(strtoul(c.node, nullptr, 16), cmd, arg1, arg2, c.id);
  }
}

volatile bool waitingForWifiSetup = false;

// Called from the Wi-Fi task, not from loop().
void onWifiEvent(arduino_event_t *event) {
  switch (event->event_id) {
    case ARDUINO_EVENT_PROV_START:
      waitingForWifiSetup = true;
      Serial.println("\nWi-Fi not set up. Open the \"ESP BLE Provisioning\" app and scan this QR code:");
      WiFiProv.printQR(PROV_DEVICE_NAME, PROV_PIN, "ble");
      Serial.printf("Or choose \"%s\" in the app and enter PIN %s.\n", PROV_DEVICE_NAME, PROV_PIN);
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      Serial.printf("\nReceived Wi-Fi network \"%s\" from phone, connecting...\n",
                    (const char *)event->event_info.prov_cred_recv.ssid);
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      if (event->event_info.prov_fail_reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) {
        Serial.println("\nWi-Fi password is wrong. Try again in the app.");
      } else {
        Serial.println("\nWi-Fi network not found. Check the name and that it is 2.4 GHz.");
      }
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("\nWi-Fi setup saved.");
      break;
    case ARDUINO_EVENT_PROV_END:
      waitingForWifiSetup = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("\nWi-Fi connected to \"%s\", IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (!waitingForWifiSetup) Serial.println("\nWi-Fi disconnected, reconnecting...");
      break;
    default:
      break;
  }
}

void printWifiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected to \"%s\", IP %s, signal %d dBm\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (waitingForWifiSetup) {
    Serial.printf("Waiting for Wi-Fi setup from the phone app (device \"%s\", PIN %s).\n", PROV_DEVICE_NAME, PROV_PIN);
  } else {
    Serial.println("Wi-Fi not connected yet, still trying.");
  }
}

void forgetWifi() {
  Serial.println("Forgetting saved Wi-Fi and restarting into Bluetooth setup...");
  WiFi.disconnect(true, true);   // second true erases the saved network
  delay(500);
  ESP.restart();
}

void handleSerialLine(String line) {
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  int space = line.indexOf(' ');
  String word = space < 0 ? line : line.substring(0, space);
  String arg = space < 0 ? "" : line.substring(space + 1);
  arg.trim();

  if (word == "scan") { scanNodes(); return; }
  if (word == "list") { listNodes(); return; }
  if (word == "wifi") {
    if (arg == "reset") forgetWifi();
    else printWifiStatus();
    return;
  }
  if (word == "led") {
    if (arg == "on" || arg == "off") setLed(arg == "on", 0);
    else Serial.println("Use: led on, or led off");
    return;
  }
  if (word == "txpower") {
    // Lower power for bench tests with the node close by; a restart goes back to full power.
    long dbm = arg.toInt();
    if (dbm < 2 || dbm > 20) {
      Serial.println("Use: txpower <2-20> (dBm; 17 is the default)");
      return;
    }
    txPowerDbm = dbm;
    LoRa.setTxPower(dbm);
    Serial.printf("LoRa transmit power set to %ld dBm until the next restart.\n", dbm);
    return;
  }
  if (word == "radio") {
    // Raw SX1278 registers, for checking the radio's settings: 0x06-0x08 frequency,
    // 0x1D/0x1E modem config, 0x39 sync word, 0x42 version (0x12).
    LoRa.dumpRegisters(Serial);
    return;
  }
  if (word == "server") {
    Serial.printf("Automation server %s: %s\n", SERVER_HOST,
                  server.isConnected() ? "connected" : "not connected (retrying every 5 s)");
    return;
  }

  uint8_t cmd, relay = 0, state = 0;
  String target = arg;
  if (word == "ping") cmd = CMD_PING;
  else if (word == "fan") {
    // fan <id> <0-100|home>; the level travels in relay's place
    int s1 = arg.indexOf(' ');
    String what = s1 < 0 ? "" : arg.substring(s1 + 1);
    what.trim();
    target = s1 < 0 ? arg : arg.substring(0, s1);
    if (what == "home") cmd = CMD_FAN_HOME;
    else {
      long level = what.toInt();
      if (level < 0 || level > 100 || (level == 0 && what != "0")) {
        Serial.println("Use: fan <id> <0-100|home>, e.g. \"fan 3F2A 60\"");
        return;
      }
      cmd = CMD_SET_FAN;
      relay = level;
    }
  }
  else if (word == "curtain") {
    // curtain <id> <open|close|stop|learn|0-100>; action and percent travel in relay/state's place
    int s1 = arg.indexOf(' ');
    String what = s1 < 0 ? "" : arg.substring(s1 + 1);
    what.trim();
    target = s1 < 0 ? arg : arg.substring(0, s1);
    if (!parseCurtain(what, relay, state)) {
      Serial.println("Use: curtain <id|all> <open|close|stop|learn|0-100>, e.g. \"curtain 3F2A 50\"");
      return;
    }
    cmd = CMD_CURTAIN;
  }
  else if (word == "dim") {
    // dim <id> <0-100|on|off|next>; level and action travel in relay/state's place
    int s1 = arg.indexOf(' ');
    String what = s1 < 0 ? "" : arg.substring(s1 + 1);
    what.trim();
    target = s1 < 0 ? arg : arg.substring(0, s1);
    if (!parseDimmer(what, relay, state)) {
      Serial.println("Use: dim <id|all> <0-100|on|off|next>, e.g. \"dim 3F2A 50\"");
      return;
    }
    cmd = CMD_DIMMER;
  }
  else if (word == "status") cmd = CMD_GET_STATUS;
  else if (word == "relay") {
    // relay <id|all> <1-4|all> <on|off|toggle>
    int s1 = arg.indexOf(' ');
    int s2 = arg.lastIndexOf(' ');
    if (s1 < 0 || s2 == s1) {
      Serial.println("Use: relay <id|all> <1-4|all> <on|off|toggle>, e.g. \"relay 3F2A 2 on\"");
      return;
    }
    target = arg.substring(0, s1);
    String which = arg.substring(s1 + 1, s2);
    String what = arg.substring(s2 + 1);
    which.trim();

    if (which == "all") relay = 0;
    else {
      relay = which.toInt();
      if (relay < 1 || relay > 4) {
        Serial.println("Relay must be 1-4 or all.");
        return;
      }
    }
    if (what == "on") state = 1;
    else if (what == "off") state = 0;
    else if (what == "toggle") state = 2;
    else {
      Serial.println("State must be on, off or toggle.");
      return;
    }
    cmd = CMD_SET_RELAY;
  } else {
    Serial.println("Commands: scan, list, ping <id|all>, status <id|all>, "
                   "relay <id|all> <1-4|all> <on|off|toggle>, fan <id> <0-100|home>, "
                   "curtain <id|all> <open|close|stop|learn|0-100>, dim <id|all> <0-100|on|off|next>, "
                   "led <on|off>, wifi, wifi reset, server");
    return;
  }

  if (target == "all") {
    if (nodeCount == 0) Serial.println("No nodes registered. Type \"scan\" first.");
    for (uint8_t i = 0; i < nodeCount; i++) sendCommand(nodes[i], cmd, relay, state, 0);
    return;
  }
  if (target.length() == 0) {
    Serial.println("Give a node ID from \"list\", e.g. \"status 3F2A\", or \"all\".");
    return;
  }
  uint16_t id = strtoul(target.c_str(), nullptr, 16);
  if (!isRegistered(id)) Serial.printf("Note: %04X is not in the list; trying anyway.\n", id);
  sendCommand(id, cmd, relay, state, 0);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("LoRa Master Initializing");

  if (!startRadio()) {   // the delay(1000) above lets the LoRa module finish powering up
    Serial.println("Starting LoRa failed! Check Wiring");
    while (1) delay(1000);
  }
  randomSeed(esp_random());
  nextSeq = random(1, 256);   // so a restarted master doesn't reuse the numbers nodes just saw

  // Connects with the saved Wi-Fi, or starts Bluetooth setup if none is saved.
  pinMode(WIFI_RESET_BUTTON, INPUT_PULLUP);
  WiFi.onEvent(onWifiEvent);
  WiFi.begin();
  uint8_t uuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf, 0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
  // Security 1 (encrypted, needs PROV_PIN) is what the phone app expects; the library default is unencrypted.
  WiFiProv.beginProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BLE, NETWORK_PROV_SECURITY_1,
                          PROV_PIN, PROV_DEVICE_NAME, NULL, uuid, false);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Connects (and reconnects) by itself once Wi-Fi is up.
  server.beginSslWithCA(SERVER_HOST, SERVER_PORT, SERVER_PATH, ROOT_CA_PEM, "");
  server.setExtraHeaders("Authorization: Bearer " DEVICE_TOKEN);
  server.onEvent(onServerEvent);
  server.setReconnectInterval(5000);
  server.enableHeartbeat(15000, 5000, 2);   // ping every 15 s; drop after 2 missed pongs

  Serial.println("LoRa Master Ready. Type \"scan\" to find nodes.");
}

void loop() {
  // Holding BOOT for 10 seconds forgets the saved Wi-Fi (long, so holding it for an upload does not).
  static unsigned long pressedAt = 0;
  if (digitalRead(WIFI_RESET_BUTTON) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    else if (millis() - pressedAt > 10000) forgetWifi();
  } else {
    pressedAt = 0;
  }

  serviceNetwork();
  if (queueCount > 0) {
    ServerCommand c = queue[queueHead];
    queueHead = (queueHead + 1) % QUEUE_SIZE;
    queueCount--;
    runServerCommand(c);
  }

  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleSerialLine(line);
      line = "";
    } else {
      line += c;
    }
  }
}
