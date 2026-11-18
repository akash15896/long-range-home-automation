// Dimmer channel state, in a header so the Arduino builder's generated prototypes can see it.
#pragma once
#include <Arduino.h>
#include "pico/time.h"

enum GateMode : uint8_t { MODE_OFF, MODE_PHASE, MODE_FULL };

struct Channel {
  const char *name;
  uint8_t gatePin;
  bool longPulse;               // fan: hold the gate on until END_MARGIN before the next Z-C
  uint16_t fadeMsPerPercent;
  int eepromAddr;
  uint8_t eepromMarker;
  // shared with interrupts
  volatile GateMode mode;
  volatile uint32_t delayPermille;
  volatile bool pulseHigh;
  volatile alarm_id_t alarm;
  // loop only
  uint8_t level;                // what was asked for (light %, or fan speed 0-100)
  float power;                  // percent of full power now, fades toward targetPower()
  uint8_t saved;                // level in flash (0xFF = none)
  unsigned long changedAt;
  unsigned long kickUntil;      // fan start-up boost
  unsigned long lastFadeStep;
};
