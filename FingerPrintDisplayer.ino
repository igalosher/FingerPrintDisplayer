#include <M5Unified.h>

// PORT.C pins: RX=GPIO18, TX=GPIO17
#define FINGERPRINT_RX 18
#define FINGERPRINT_TX 17

HardwareSerial* fpSerial = &Serial2;
bool sensorReady = false;
unsigned long workingBaud = 0;

void showStatus(const char* msg, uint16_t color = WHITE) {
  M5.Display.fillRect(0, 200, 320, 40, BLACK);
  M5.Display.setCursor(10, 200);
  M5.Display.setTextColor(color);
  M5.Display.setTextSize(1);
  M5.Display.println(msg);
}

// Simple test: send any byte and see if we get response
bool testCommunication(unsigned long baud) {
  Serial.printf("Testing @ %lu baud...\n", baud);
  
  if (Serial2) {
    Serial2.end();
  }
  delay(200);
  
  Serial2.begin(baud, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  delay(1000);
  
  // Clear buffer
  unsigned long start = millis();
  while (millis() - start < 500) {
    if (Serial2.available()) {
      Serial2.read();
    }
    delay(10);
  }
  
  // Send a simple test byte sequence
  // Try: 0xAA 0x05 0x00 0x00 0x05 0x55 (simple capture command)
  uint8_t testCmd[] = {0xAA, 0x05, 0x00, 0x00, 0x05, 0x55};
  Serial2.write(testCmd, 6);
  Serial2.flush();
  
  delay(500);
  
  // Check for ANY response
  int bytesReceived = 0;
  uint8_t response[32] = {0};
  start = millis();
  while (millis() - start < 1000) {
    if (Serial2.available()) {
      if (bytesReceived < 32) {
        response[bytesReceived++] = Serial2.read();
      } else {
        Serial2.read(); // Discard overflow
      }
    }
    delay(10);
  }
  
  if (bytesReceived > 0) {
    Serial.printf("Got %d bytes response: ", bytesReceived);
    for (int i = 0; i < bytesReceived && i < 16; i++) {
      Serial.printf("0x%02X ", response[i]);
    }
    Serial.println();
    workingBaud = baud;
    return true;
  }
  
  Serial.println("No response");
  return false;
}

bool initSensor() {
  Serial.println("=== Initializing Fingerprint Sensor ===");
  showStatus("Testing sensor...", CYAN);
  
  // Try multiple baud rates
  unsigned long baudRates[] = {115200, 19200, 57600, 9600, 38400};
  int numBauds = sizeof(baudRates) / sizeof(baudRates[0]);
  
  for (int i = 0; i < numBauds; i++) {
    Serial.printf("\nTrying baud rate: %lu\n", baudRates[i]);
    char statusMsg[32];
    snprintf(statusMsg, sizeof(statusMsg), "Testing: %lu", baudRates[i]);
    showStatus(statusMsg, YELLOW);
    
    if (testCommunication(baudRates[i])) {
      Serial.printf("SUCCESS! Sensor responds at %lu baud\n", baudRates[i]);
      snprintf(statusMsg, sizeof(statusMsg), "Found @ %lu", baudRates[i]);
      showStatus(statusMsg, GREEN);
      delay(1000);
      return true;
    }
    delay(500);
  }
  
  Serial.println("No communication found");
  return false;
}

void setup() {
  // Initialize M5Stack
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.fillScreen(BLACK);
  
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n\n=== FingerPrint Displayer ===");
  
  // Show title
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println("FingerPrint");
  M5.Display.println("Displayer");
  
  // Initialize sensor
  if (initSensor()) {
    sensorReady = true;
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(10, 10);
    M5.Display.setTextColor(GREEN);
    M5.Display.setTextSize(2);
    M5.Display.println("Ready!");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, 50);
    char info[32];
    snprintf(info, sizeof(info), "Baud: %lu", workingBaud);
    M5.Display.println(info);
    M5.Display.setCursor(10, 70);
    snprintf(info, sizeof(info), "RX:%d TX:%d", FINGERPRINT_RX, FINGERPRINT_TX);
    M5.Display.println(info);
    showStatus("Sensor ready!", GREEN);
    Serial.println("Initialization successful!");
    delay(2000);
  } else {
    sensorReady = false;
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(10, 10);
    M5.Display.setTextColor(RED);
    M5.Display.setTextSize(2);
    M5.Display.println("Init Failed");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, 50);
    M5.Display.println("Check PORT.C");
    M5.Display.println("GPIO18, GPIO17");
    M5.Display.println("Power: 5V");
    showStatus("Initialization failed!", RED);
    Serial.println("Initialization failed - will retry in loop");
  }
}

void loop() {
  M5.update();
  
  if (!sensorReady) {
    // Retry initialization every 5 seconds
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 5000) {
      Serial.println("Retrying initialization...");
      showStatus("Retrying...", YELLOW);
      sensorReady = initSensor();
      lastRetry = millis();
      
      if (sensorReady) {
        M5.Display.fillScreen(BLACK);
        M5.Display.setCursor(10, 10);
        M5.Display.setTextColor(GREEN);
        M5.Display.setTextSize(2);
        M5.Display.println("Ready!");
        showStatus("Sensor ready!", GREEN);
        delay(2000);
      }
    }
    delay(1000);
    return;
  }
  
  // Normal operation - show waiting message
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 10);
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(2);
  M5.Display.println("Waiting...");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 50);
  M5.Display.println("Place finger");
  M5.Display.println("on sensor");
  
  showStatus("Waiting for finger...", CYAN);
  
  // For now, just show a pattern when we detect any serial activity
  // This will be replaced with actual image capture later
  delay(1000);
  
  // Check if sensor sends any data (finger detected)
  if (Serial2.available() > 0) {
    Serial.println("Sensor activity detected!");
    showStatus("Finger detected!", GREEN);
    
    // Read available data
    uint8_t data[64];
    int count = 0;
    while (Serial2.available() && count < 64) {
      data[count++] = Serial2.read();
    }
    
    Serial.printf("Received %d bytes\n", count);
    
    // Show a simple pattern
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(10, 10);
    M5.Display.setTextColor(GREEN);
    M5.Display.setTextSize(2);
    M5.Display.println("Fingerprint");
    M5.Display.println("Detected!");
    
    // Draw a simple pattern
    int centerX = M5.Display.width() / 2;
    int centerY = M5.Display.height() / 2;
    for (int r = 0; r < 50; r += 5) {
      M5.Display.drawCircle(centerX, centerY, r, WHITE);
    }
    
    showStatus("Displaying 10 seconds...", GREEN);
    delay(10000);
  }
  
  delay(200);
}
