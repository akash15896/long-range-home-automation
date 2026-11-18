// Minimal LoRa test, ESP32 side: sends "hello #N" every 2 s and prints any reply.
// Library defaults: 433 MHz, sync word 0x12, SF7, 125 kHz, 17 dBm, no CRC.
// Pair with LoRa_hello_node on the Nano. Serial Monitor at 115200 baud.

#include <SPI.h>
#include <LoRa.h>

#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

uint32_t counter = 0, replies = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("Starting LoRa failed! Check wiring.");
    while (1) delay(1000);
  }
  Serial.println("Hello master ready");
}

void loop() {
  counter++;
  LoRa.beginPacket();
  LoRa.print("hello #");
  LoRa.print(counter);
  LoRa.endPacket();
  Serial.printf("sent  hello #%lu\n", (unsigned long)counter);

  // Wait up to 1 s for the node's reply.
  unsigned long start = millis();
  while (millis() - start < 1000) {
    if (LoRa.parsePacket()) {
      String text;
      while (LoRa.available()) text += (char)LoRa.read();
      replies++;
      Serial.printf("got   %s  (RSSI %d dBm, SNR %.1f dB)  replies %lu/%lu\n", text.c_str(),
                    LoRa.packetRssi(), LoRa.packetSnr(), (unsigned long)replies, (unsigned long)counter);
      break;
    }
  }
  delay(1000);
}
