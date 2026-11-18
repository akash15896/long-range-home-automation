#include <SPI.h>
#include <LoRa.h>

#define LORA_SS 10
#define LORA_RST 9
#define LORA_DIO0 2

#define LORA_BAND 433E6

int counter = 0;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  while(!Serial);

  Serial.println("LoRa Sender Initializing");

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_BAND))
  {
    Serial.println("Starting LoRa failed! Check Wiring");
    while(1);
  }

  Serial.println("LoRa Receiver Ready.");
}

void loop() {
  // put your main code here, to run repeatedly:

  int packetSize = LoRa.parsePacket();


  if (packetSize)
  {
    Serial.println("----------------------------------------------------");
    Serial.print("Received packet: [");

    while(LoRa.available())
    {
      Serial.print((char)LoRa.read());
    }

    Serial.println("]");
    Serial.print("with RSS: ");
    Serial.println(LoRa.packetRssi());
    Serial.print("dBm|SNR: ");
    Serial.print(LoRa.packetSnr());
    Serial.println(" dB");
    Serial.println("----------------------------------------------------");
  }
}
