const int dirPin = 2;
const int stepPin = 3;

const int switchPin = 10;

void setup() {
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  digitalWrite(dirPin, HIGH); // Set direction
  pinMode(switchPin, INPUT_PULLUP);
}

void loop() {
  int readSwitch = digitalRead(switchPin);
  // Direct port manipulation or optimized fast pulsing

  if (readSwitch == LOW)
  {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(200); // 30µs HIGH time (Adjust this down to 25-30 to test max speed)
    digitalWrite(stepPin, LOW);
    delayMicroseconds(200); // 30µs LOW time
  }
}
