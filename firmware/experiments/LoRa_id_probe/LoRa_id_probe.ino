// Temporary: reads the SX1278 version register (0x42, should be 0x12) raw at a few SPI speeds.
#include <SPI.h>
uint8_t rd(uint32_t hz, uint8_t a) {
  digitalWrite(10, LOW);
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  SPI.transfer(a & 0x7F);
  uint8_t v = SPI.transfer(0);
  SPI.endTransaction();
  digitalWrite(10, HIGH);
  return v;
}
void setup() {
  Serial.begin(9600);
  pinMode(10, OUTPUT); digitalWrite(10, HIGH);
  Serial.print("RST pin (D9) reads "); pinMode(9, INPUT); Serial.println(digitalRead(9));
  pinMode(9, OUTPUT); digitalWrite(9, LOW); delay(1); pinMode(9, INPUT); delay(20);
  SPI.begin();
  uint32_t speeds[] = { 50000, 125000, 250000 };
  for (uint8_t s = 0; s < 3; s++) {
    Serial.print(speeds[s] / 1000); Serial.print(" kHz: 0x42 ->");
    for (uint8_t i = 0; i < 8; i++) { Serial.print(' '); Serial.print(rd(speeds[s], 0x42), HEX); }
    Serial.println();
  }
}
void loop() {}
