// AC bulb dimmer for the Raspberry Pi Pico (RP2040, also builds for Pico 2) + a 2-channel AC dimmer module
// (RobotDyn-style: zero-cross output + one triac gate input per channel). Only channel 1 is used.
//
// One push button steps through the brightness levels round robin:
//   OFF -> 25% -> 50% -> 75% -> 100% -> OFF -> ...
// Hold the button for LONG_PRESS_MS to switch straight to OFF.
//
// Board package: "Raspberry Pi Pico/RP2040/RP2350" by Earle Philhower; board "Raspberry Pi Pico" (FQBN rp2040:rp2040:rpipico)
// (for a Pico 2, pick "Raspberry Pi Pico 2" - the code is the same).
// Serial monitor: USB, any baud rate. Type "help" for test commands.
//
// ---- Wiring (low-voltage side) ----
//   Dimmer module   Pico
//   VCC             3V3(OUT) pin 36   <- 3.3 V, NOT 5 V: the module pulls Z-C up to VCC
//   GND             GND      pin 38
//   Z-C (ZC)        GP2      pin 4
//   D1 / PWM1       GP3      pin 5
//   D2 / PWM2       (not connected - spare channel)
//
//   Push switch     Pico
//   C               GND      pin 18
//   NO              GP15     pin 20    (BUTTON_USES_NC = false)
//   NC              (not connected)    (or NC -> GP15 instead of NO, with BUTTON_USES_NC = true)
//
// ---- How it works ----
// The module's Z-C pin pulses at every mains zero crossing (100x/s at 50 Hz, 120x/s at 60 Hz).
// On each pulse we start a hardware alarm; when it fires we give the triac a short gate pulse.
// Firing late in the half wave = dim, firing early = bright (phase-angle control).
// 100% holds the gate on all the time; OFF holds it low.
// The mains frequency is measured from the zero-cross pulses, so 50 Hz and 60 Hz both work.

#include <Arduino.h>
#include <math.h>
#include "pico/time.h"
#include "hardware/gpio.h"

// ---- Pins ----
#define ZC_PIN 2
#define GATE_PIN 3
#define BUTTON_PIN 15

// ---- Button ----
#define BUTTON_USES_NC false   // false: C->GND, NO->GP15.  true: C->GND, NC->GP15.
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 700

// ---- Brightness levels (percent of full lamp power), stepped round robin ----
const uint8_t LEVELS[] = {0, 25, 50, 75, 100};
const uint8_t LEVEL_COUNT = sizeof(LEVELS) / sizeof(LEVELS[0]);

// ---- Timing (microseconds) ----
#define GATE_PULSE_US 100     // triac gate pulse length
#define ZC_OFFSET_US 0        // add if the Z-C pulse arrives early (tune if low levels flicker)
#define MIN_DELAY_US 400      // never fire earlier than this after Z-C
#define END_MARGIN_US 700     // never fire later than this before the next Z-C
#define MIN_ZC_GAP_US 6000    // Z-C edges closer than this are noise
#define FADE_MS_PER_PERCENT 4 // smooth change between levels (0 = jump instantly)

// ---- Shared with interrupts ----
enum GateMode : uint8_t { MODE_OFF, MODE_PHASE, MODE_FULL };
volatile GateMode gateMode = MODE_OFF;
volatile uint32_t delayPermille = 1000;   // firing point as a fraction of the half wave
volatile uint32_t halfPeriodUs = 10000;   // measured; starts at 50 Hz
volatile uint32_t lastZcUs = 0;
volatile uint32_t zcCount = 0;
volatile bool pulseHigh = false;
volatile alarm_id_t gateAlarm = 0;

uint8_t levelIndex = 0;
float currentPower = 0;   // percent, fades toward targetPower
float targetPower = 0;

// ---------------------------------------------------------------------------------------------
// Interrupts

int64_t gateAlarmCb(alarm_id_t, void *) {
  if (gateMode != MODE_PHASE) {   // mode changed while the alarm was pending
    gateAlarm = 0;
    return 0;
  }
  if (!pulseHigh) {
    gpio_put(GATE_PIN, 1);
    pulseHigh = true;
    return GATE_PULSE_US;         // come back to end the pulse
  }
  gpio_put(GATE_PIN, 0);
  pulseHigh = false;
  gateAlarm = 0;
  return 0;
}

void onZeroCross() {
  uint32_t now = micros();
  uint32_t dt = now - lastZcUs;
  if (dt < MIN_ZC_GAP_US) return;   // noise / bounce on the Z-C line
  lastZcUs = now;
  zcCount++;
  if (dt > 7000 && dt < 11500) halfPeriodUs = (halfPeriodUs * 7 + dt) / 8;   // 43..71 Hz

  if (gateMode != MODE_PHASE) return;

  if (gateAlarm) cancel_alarm(gateAlarm);
  gpio_put(GATE_PIN, 0);
  pulseHigh = false;

  uint32_t half = halfPeriodUs;
  uint32_t d = half * delayPermille / 1000 + ZC_OFFSET_US;
  if (d < MIN_DELAY_US) d = MIN_DELAY_US;
  if (d > half - END_MARGIN_US) d = half - END_MARGIN_US;
  gateAlarm = add_alarm_in_us(d, gateAlarmCb, nullptr, true);
}

// ---------------------------------------------------------------------------------------------
// Power -> firing delay
// For a resistive load (filament bulb) fired at fraction x of the half wave, the power delivered
// is P(x) = 1 - x + sin(2*pi*x) / (2*pi). Solve for x so the levels are real power steps.

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

void applyPower(float percent) {
  if (percent <= 0.5f) {
    gateMode = MODE_OFF;
    gpio_put(GATE_PIN, 0);
  } else if (percent >= 99.5f) {
    gateMode = MODE_FULL;
    gpio_put(GATE_PIN, 1);
  } else {
    delayPermille = powerToDelayPermille(percent);
    if (gateMode != MODE_PHASE) gpio_put(GATE_PIN, 0);
    gateMode = MODE_PHASE;
  }
}

void setTarget(float percent) {
  targetPower = constrain(percent, 0, 100);
  if (FADE_MS_PER_PERCENT == 0) {
    currentPower = targetPower;
    applyPower(currentPower);
  }
  Serial.printf("Brightness -> %d%%\n", (int)targetPower);
}

void updateFade() {
  static uint32_t lastStep = 0;
  if (currentPower == targetPower) return;
  if (millis() - lastStep < FADE_MS_PER_PERCENT) return;
  lastStep = millis();
  if (currentPower < targetPower) currentPower = min(currentPower + 1, targetPower);
  else currentPower = max(currentPower - 1, targetPower);
  applyPower(currentPower);
}

// ---------------------------------------------------------------------------------------------
// Button

bool buttonPressedRaw() {
  bool level = digitalRead(BUTTON_PIN);   // pull-up: HIGH when the contact is open
  return BUTTON_USES_NC ? level : !level;
}

void nextLevel() {
  levelIndex = (levelIndex + 1) % LEVEL_COUNT;
  setTarget(LEVELS[levelIndex]);
}

void pollButton() {
  static bool stable = false, lastRaw = false, longDone = false;
  static uint32_t changedAt = 0, pressedAt = 0;
  bool raw = buttonPressedRaw();
  uint32_t now = millis();

  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (now - changedAt >= DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable) {                       // pressed
      pressedAt = now;
      longDone = false;
    } else if (!longDone) {             // released after a short press
      nextLevel();
    }
  }
  if (stable && !longDone && now - pressedAt >= LONG_PRESS_MS) {   // long press -> OFF
    longDone = true;
    levelIndex = 0;
    setTarget(0);
  }
}

// ---------------------------------------------------------------------------------------------
// Serial test commands

void printInfo() {
  uint32_t half = halfPeriodUs;
  uint32_t age = micros() - lastZcUs;
  Serial.printf("Level %d/%d, target %d%%, now %d%%, mode %s\n", levelIndex, LEVEL_COUNT - 1,
                (int)targetPower, (int)currentPower,
                gateMode == MODE_OFF ? "OFF" : gateMode == MODE_FULL ? "FULL" : "PHASE");
  Serial.printf("Zero cross: %lu pulses, half period %lu us (%.1f Hz mains), last %lu ms ago\n",
                (unsigned long)zcCount, (unsigned long)half, 500000.0f / half,
                (unsigned long)(age / 1000));
  if (gateMode == MODE_PHASE)
    Serial.printf("Firing %lu us after zero cross\n", (unsigned long)(half * delayPermille / 1000));
  if (age > 100000) Serial.println("WARNING: no zero-cross pulses - is mains on AC-L/AC-N and Z-C on GP2?");
  Serial.printf("Button: %s\n", buttonPressedRaw() ? "pressed" : "released");
}

void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 32) line += c; continue; }
    line.trim();
    line.toLowerCase();
    if (line == "help") {
      Serial.println("Commands: info | next | off | on | 0..100 (set brightness %)");
    } else if (line == "info") {
      printInfo();
    } else if (line == "next") {
      nextLevel();
    } else if (line == "off") {
      levelIndex = 0; setTarget(0);
    } else if (line == "on") {
      levelIndex = LEVEL_COUNT - 1; setTarget(100);
    } else if (line.length() && isDigit(line[0])) {
      setTarget(line.toInt());
    } else if (line.length()) {
      Serial.println("Unknown command, type help");
    }
    line = "";
  }
}

// ---------------------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(GATE_PIN, OUTPUT);
  digitalWrite(GATE_PIN, LOW);
  pinMode(ZC_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ZC_PIN), onZeroCross, RISING);

  delay(1500);   // give USB serial a moment
  Serial.println("Pico AC dimmer ready. Button: short press = next level, long press = off.");
  Serial.println("Type help for serial commands.");
}

void loop() {
  pollButton();
  updateFade();
  handleSerial();

  static uint32_t lastWarn = 0;
  if (gateMode == MODE_PHASE && micros() - lastZcUs > 200000 && millis() - lastWarn > 5000) {
    lastWarn = millis();
    Serial.println("WARNING: no zero-cross pulses - dimming cannot work (check Z-C wire / mains).");
  }
}
