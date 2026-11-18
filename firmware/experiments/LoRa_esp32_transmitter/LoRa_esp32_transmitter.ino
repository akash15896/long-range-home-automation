#include <SPI.h>
#include <LoRa.h>

#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

#define LORA_BAND 433E6

int counter = 0;

const int ledPin = 13; 

void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH);

  Serial.begin(115200);
  while(!Serial)
  {
    digitalWrite(ledPin, LOW);    // Turn the LED off
    delay(300); 
    digitalWrite(ledPin, HIGH);   // Turn the LED on
    delay(300); 
  }

  digitalWrite(ledPin, HIGH);

  Serial.println("LoRa Sender Initializing");

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_BAND))
  {
    Serial.println("Starting LoRa failed! Check Wiring");
    digitalWrite(ledPin, LOW);
    while(1);
  }

  Serial.println("LoRa Sender Ready");

  digitalWrite(ledPin, LOW);
}

void loop() {
  // put your main code here, to run repeatedly:
  Serial.print("Sending packet: ");
  Serial.println(counter);

  LoRa.beginPacket();
  LoRa.print("Hello from ESP32, Count: ");
  LoRa.print(counter);
  LoRa.endPacket();

  counter++;

  delay(2000);
}
