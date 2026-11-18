// Define pin connections
const int dirPin = 2;
const int stepPin = 3;

// Define steps per revolution based on your driver's DIP switch settings
// (Common defaults: 200 for Full Step, 400 for Half Step, 1600 for 1/8 Step)
const int stepsPerRevolution = 1600; 

void setup() {
  // Declare pins as Outputs
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
}

void loop() {
  // Set motor direction clockwise
  digitalWrite(dirPin, HIGH);

  // Spin the motor 1 full revolution slowly
  for (int i = 0; i < stepsPerRevolution; i++) {
    // These four lines create a single pulse
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(2000); // Changing this value changes the speed (lower = faster)
    digitalWrite(stepPin, LOW);
    delayMicroseconds(2000);
  }

  delay(1000); // Pause for 1 second

  // Set motor direction counterclockwise
  digitalWrite(dirPin, LOW);

  // Spin the motor 1 full revolution slowly
  for (int i = 0; i < stepsPerRevolution; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(2000); 
    digitalWrite(stepPin, LOW);
    delayMicroseconds(2000);
  }

  delay(1000); // Pause for 1 second
}


