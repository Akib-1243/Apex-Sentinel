#include <Wire.h>
#include <SoftwareSerial.h>
#include <TinyGPS++.h>
#include <LiquidCrystal_I2C.h>

// --- Pin Setup ---
const int GPS_RX_PIN    = 4;  // GPS TX -> Digital Pin 4
const int GPS_TX_PIN    = 3;  // GPS RX -> Digital Pin 3
const int GSM_RX_PIN    = 8;  // SIM800L TXD -> Digital Pin 8
const int GSM_TX_PIN    = 7;  // SIM800L RXD -> Digital Pin 7
const int BUZZER_PIN    = 5;  // Buzzer (+) -> Digital Pin 5

SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
SoftwareSerial gsmSerial(GSM_RX_PIN, GSM_TX_PIN);
TinyGPSPlus gps;

LiquidCrystal_I2C lcd(0x27, 16, 2);

const int MPU_ADDR = 0x68;

// --- ACCIDENT THRESHOLDS ---
const float IMPACT_THRESHOLD = 2.10; // Increased to 3.5G so fast speed bumps don't trigger it
const float FLIP_THRESHOLD   = 60.0; // Angle threshold (degrees)
const unsigned long FLIP_DURATION = 2000; // Must STAY flipped for 2 full seconds (2000ms)

const String EMERGENCY_NUMBER = "+8801521202272"; // Emergency contact

unsigned long lastPrintTime = 0;
unsigned long flipStartTime = 0;
bool isFlipping = false;
String gsmStatus = "Checking...";

void beepStartup() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(120);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

void beepAccidentAlert() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
  digitalWrite(BUZZER_PIN, LOW);
}

void setup() {
  Serial.begin(9600);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  beepStartup();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Starting");
  lcd.setCursor(0, 1);
  lcd.print("Please Wait...");

  delay(500);

  Wire.begin();
  Wire.setWireTimeout(25000, true);
  Wire.setClock(100000);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);

  gpsSerial.begin(9600);
  gsmSerial.begin(9600);

  checkGSMNetwork();
  gpsSerial.listen();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready!");
  delay(1000);
  lcd.clear();
}

void loop() {
  // Read GPS
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Read MPU-6050
  float totalG = 1.0;
  float tiltAngle = 0.0;

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(true) == 0) {
    if (Wire.requestFrom(MPU_ADDR, 6, true) == 6) {
      int16_t rawX = (Wire.read() << 8) | Wire.read();
      int16_t rawY = (Wire.read() << 8) | Wire.read();
      int16_t rawZ = (Wire.read() << 8) | Wire.read();

      if (rawX != 0 || rawY != 0 || rawZ != 0) {
        float ax = rawX / 16384.0;
        float ay = rawY / 16384.0;
        float az = rawZ / 16384.0;

        totalG = sqrt(ax * ax + ay * ay + az * az);
        float ratio = constrain(az / totalG, -1.0, 1.0);
        tiltAngle = acos(ratio) * (180.0 / PI);
      }
    }
  }

  // Update LCD & Serial every 1 second
  if (millis() - lastPrintTime >= 1000) {
    lastPrintTime = millis();
    updateDisplays(totalG, tiltAngle);
  }

  // --- 1. HARD IMPACT ACCIDENT (Crash) ---
  if (totalG >= IMPACT_THRESHOLD) {
    triggerAccident("Hard Impact", String(totalG, 1) + "G");
  } 

  // --- 2. ROLLOVER / FLIP VERIFICATION (Must stay tilted for 2 seconds) ---
  if (tiltAngle >= FLIP_THRESHOLD) {
    if (!isFlipping) {
      isFlipping = true;
      flipStartTime = millis(); // Start timer
    } else if (millis() - flipStartTime >= FLIP_DURATION) {
      // Vehicle has stayed upside down / on side for 2 FULL SECONDS
      triggerAccident("Rollover/Flip", String(tiltAngle, 0) + "deg");
      isFlipping = false;
    }
  } else {
    // If the car recovers (normal turn/bump ends), reset the timer!
    isFlipping = false;
  }

  delay(40);
}

void updateDisplays(float gForce, float tilt) {
  // --- LCD Display ---
  lcd.setCursor(0, 0);
  lcd.print("Force: ");
  lcd.print(gForce, 2);
  lcd.print("   ");

  lcd.setCursor(0, 1);
  lcd.print("Tilt: ");
  lcd.print(tilt, 1);
  lcd.print((char)223);
  lcd.print("      ");

  // --- Serial Monitor ---
  Serial.print("[SENSOR] Force: ");
  Serial.print(gForce, 2);
  Serial.print(" | Tilt: ");
  Serial.print(tilt, 1);
  Serial.println("°");
}
void triggerAccident(String crashName, String crashValue) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("! ACCIDENT !");
  lcd.setCursor(0, 1);
  lcd.print(crashName + " " + crashValue);

  Serial.println("\n**************************************************");
  Serial.println("  >>> [!] CRITICAL ACCIDENT DETECTED! <<<");
  Serial.println("  Cause: " + crashName + " (" + crashValue + ")");
  Serial.println("**************************************************");

  beepAccidentAlert();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sending SMS...");
  lcd.setCursor(0, 1);
  lcd.print("To Hospital");

  String locationInfo;
  if (gps.location.isValid()) {
    float lat = gps.location.lat();
    float lng = gps.location.lng();
    locationInfo = "Location: https://maps.google.com/?q=" + String(lat, 6) + "," + String(lng, 6);
  } else {
    locationInfo = "Location: Satellites acquiring. Last fix pending.";
  }

  String smsMessage = "EMERGENCY: Accident Detected!\n" + crashName + " (" + crashValue + ")\n" + locationInfo;

  Serial.println("Sending SMS to " + EMERGENCY_NUMBER + "...");
  gsmSerial.listen();
  delay(100);

  gsmSerial.println("AT+CMGF=1");
  delay(300);
  gsmSerial.println("AT+CMGS=\"" + EMERGENCY_NUMBER + "\"");
  delay(300);
  gsmSerial.print(smsMessage);
  delay(200);
  gsmSerial.write(26);
  delay(2000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SMS Sent!");
  lcd.setCursor(0, 1);
  lcd.print("Alert Complete");
  Serial.println(">>> SMS Alert Dispatched! <<<");

  gpsSerial.listen();
  delay(4000);

  lcd.clear();
  beepStartup();
}

void checkGSMNetwork() {
  gsmSerial.listen();
  delay(200);
  gsmSerial.println("AT");
  delay(300);
  gsmSerial.println("AT+CREG?");
  delay(500);

  String response = "";
  while (gsmSerial.available()) {
    response += (char)gsmSerial.read();
  }

  if (response.indexOf("+CREG: 0,1") != -1 || response.indexOf("+CREG: 0,5") != -1) {
    gsmStatus = "Ready (Registered on Network)";
  } else if (response.indexOf("OK") != -1) {
    gsmStatus = "SIM OK (Searching Signal...)";
  } else {
    gsmStatus = "No Response (Check Power/Wires)";
  }
}