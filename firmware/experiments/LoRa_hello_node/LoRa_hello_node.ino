// Minimal LoRa test, Nano side: prints each packet and answers "ack" with the same number.
// Library defaults: 433 MHz, sync word 0x12, SF7, 125 kHz, 17 dBm, no CRC.
// Pair with LoRa_hello_master on the ESP32. Serial Monitor at 9600 baud.
//
// Wiring with the level converter: RST is a direct wire, so this sketch only pulls it LOW and
// then lets it float, never driving 5 V into the module. SPI runs at 125 kHz: the converter garbles signals above about 250 kHz.

#include <SPI.h>
#include <LoRa.h>

#define LORA_SS 10
#define LORA_RST 9
#define LORA_DIO0 2

uint8_t readRegister(uint8_t address) {
  digitalWrite(LORA_SS, LOW);
  SPI.beginTransaction(SPISettings(125000, MSBFIRST, SPI_MODE0));
  SPI.transfer(address & 0x7F);
  uint8_t value = SPI.transfer(0);
  SPI.endTransaction();
  digitalWrite(LORA_SS, HIGH);
  return value;
}

void setup() {
  Serial.begin(9600);
  delay(250);   // let the 3.3 V supply settle

  digitalWrite(LORA_RST, LOW);   // reset pulse without driving RST high
  pinMode(LORA_RST, OUTPUT);
  delay(1);
  pinMode(LORA_RST, INPUT);
  delay(10);

  LoRa.setPins(LORA_SS, -1, LORA_DIO0);
  LoRa.setSPIFrequency(125E3);
  if (!LoRa.begin(433E6)) {
    Serial.println("Starting LoRa failed! Check wiring.");
    while (1);
  }
  Serial.print("Hello node ready. Radio version 0x");
  Serial.print(readRegister(0x42), HEX);
  Serial.println(" (should be 0x12)");
}

void loop() {
  if (!LoRa.parsePacket()) return;

  String text;
  while (LoRa.available()) text += (char)LoRa.read();
  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  Serial.print("got   ");
  Serial.print(text);
  Serial.print("  (RSSI ");
  Serial.print(rssi);
  Serial.print(" dBm, SNR ");
  Serial.print(snr, 1);
  Serial.println(" dB)");

  // Reply with "ack #N" for "hello #N".
  int hash = text.indexOf('#');
  String reply = "ack " + (hash >= 0 ? text.substring(hash) : String("?"));
  delay(50);   // give the master time to switch to receive
  LoRa.beginPacket();
  LoRa.print(reply);
  LoRa.endPacket();
  Serial.print("sent  ");
  Serial.println(reply);
}
