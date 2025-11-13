#include <M5Unified.h>

// PORT.C pins: RX=GPIO18, TX=GPIO17
#define FINGERPRINT_RX 18
#define FINGERPRINT_TX 17

// Protocol constants
#define CMD_HEAD 0xAA
#define CMD_TAIL 0x55
#define CMD_CAPTURE_IMAGE 0x05
#define CMD_GET_IMAGE 0x06
#define CMD_GET_IMAGE_SIZE 0x07

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

// Calculate checksum (XOR of all bytes except header)
uint8_t calcChecksum(uint8_t* data, uint8_t len) {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < len; i++) {
    sum ^= data[i];
  }
  return sum;
}

// Send command with protocol format: 0xAA [CMD] [PARAM_LEN] [PARAMS...] [CHECKSUM] 0x55
bool sendCmd(uint8_t cmd, uint8_t* params = nullptr, uint8_t paramLen = 0) {
  uint8_t buf[32];
  uint8_t idx = 0;
  
  buf[idx++] = CMD_HEAD;
  buf[idx++] = cmd;
  buf[idx++] = paramLen;
  
  if (params && paramLen > 0) {
    for (uint8_t i = 0; i < paramLen; i++) {
      buf[idx++] = params[i];
    }
  }
  
  // Calculate checksum (XOR of cmd, paramLen, and params)
  uint8_t chk = calcChecksum(&buf[1], idx - 1);
  buf[idx++] = chk;
  buf[idx++] = CMD_TAIL;
  
  // Clear buffer
  while (fpSerial->available()) fpSerial->read();
  
  // Send command
  fpSerial->write(buf, idx);
  fpSerial->flush();
  
  Serial.printf("TX: cmd=0x%02X len=%d\n", cmd, idx);
  return true;
}

// Read response packet
bool readResp(uint8_t* resp, uint8_t maxLen, uint16_t timeout = 2000) {
  uint8_t idx = 0;
  unsigned long start = millis();
  
  // First, read any available bytes to find header
  while (millis() - start < timeout) {
    if (fpSerial->available()) {
      uint8_t b = fpSerial->read();
      if (b == CMD_HEAD) {
        resp[idx++] = b;
        Serial.println("Found header 0xAA");
        break;
      } else {
        // Discard non-header bytes
        Serial.printf("Discarding: 0x%02X\n", b);
      }
    }
    delay(1);
  }
  
  if (idx == 0) {
    Serial.println("No header received");
    return false;
  }
  
  // Read cmd and paramLen (at least 2 more bytes)
  start = millis();
  while (millis() - start < timeout && idx < 3) {
    if (fpSerial->available()) {
      resp[idx++] = fpSerial->read();
      if (idx >= maxLen) break;
    }
    delay(1);
  }
  
  if (idx < 3) {
    Serial.println("Incomplete header");
    return false;
  }
  
  uint8_t paramLen = resp[2];
  uint8_t totalLen = 3 + paramLen + 2; // header + cmd + paramLen + params + checksum + tail
  
  Serial.printf("Expecting %d bytes total (paramLen=%d)\n", totalLen, paramLen);
  
  // Read rest of packet
  start = millis();
  while (millis() - start < timeout && idx < totalLen) {
    if (fpSerial->available()) {
      resp[idx++] = fpSerial->read();
      if (idx >= maxLen) break;
    }
    delay(1);
  }
  
  Serial.printf("RX: cmd=0x%02X len=%d bytes received\n", resp[1], idx);
  if (idx < totalLen) {
    Serial.printf("Warning: Expected %d bytes but got %d\n", totalLen, idx);
  }
  return (idx >= 5); // At least header + cmd + paramLen + checksum + tail
}

// Capture fingerprint image
bool captureImage() {
  Serial.println("Sending capture command...");
  showStatus("Capturing...", YELLOW);
  
  // Clear any pending data first
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    Serial.printf("Clearing before capture: 0x%02X\n", b);
  }
  delay(200);
  
  if (!sendCmd(CMD_CAPTURE_IMAGE)) {
    Serial.println("Failed to send capture command");
    return false;
  }
  
  delay(3000); // Wait longer for capture to complete
  
  // Read ALL available bytes, not just formatted response
  Serial.println("Reading response...");
  uint8_t resp[128] = {0};
  int bytesRead = 0;
  unsigned long start = millis();
  
  // Read for up to 5 seconds
  while (millis() - start < 5000) {
    if (Serial2.available()) {
      if (bytesRead < 128) {
        resp[bytesRead] = Serial2.read();
        Serial.printf("Rx[%d]: 0x%02X\n", bytesRead, resp[bytesRead]);
        bytesRead++;
      } else {
        Serial2.read(); // Discard overflow
      }
    }
    delay(10);
  }
  
  if (bytesRead == 0) {
    Serial.println("No response to capture command");
    showStatus("No response!", RED);
    delay(2000);
    return false;
  }
  
  Serial.printf("Got %d bytes total:\n", bytesRead);
  for (int i = 0; i < bytesRead && i < 32; i++) {
    Serial.printf("  [%d]: 0x%02X\n", i, resp[i]);
  }
  
  // Try to find response header
  int headerIdx = -1;
  for (int i = 0; i < bytesRead; i++) {
    if (resp[i] == CMD_HEAD) {
      headerIdx = i;
      Serial.printf("Found header 0xAA at index %d\n", i);
      break;
    }
  }
  
  if (headerIdx >= 0 && headerIdx + 2 < bytesRead) {
    // Found header, check if it's a capture response
    uint8_t cmd = resp[headerIdx + 1];
    uint8_t paramLen = resp[headerIdx + 2];
    
    Serial.printf("Response: cmd=0x%02X paramLen=%d\n", cmd, paramLen);
    
    if (cmd == CMD_CAPTURE_IMAGE) {
      if (headerIdx + 3 + paramLen < bytesRead) {
        uint8_t result = resp[headerIdx + 3];
        Serial.printf("Result code: 0x%02X\n", result);
        
        if (result == 0x00) {
          Serial.println("Capture successful!");
          return true;
        } else {
          Serial.printf("Capture failed: error code 0x%02X\n", result);
          showStatus("Capture error!", RED);
          delay(2000);
          return false;
        }
      } else {
        Serial.println("Incomplete response packet");
        showStatus("Incomplete!", RED);
        delay(2000);
        return false;
      }
    } else {
      Serial.printf("Unexpected command in response: 0x%02X (expected 0x%02X)\n", cmd, CMD_CAPTURE_IMAGE);
      // But maybe it's still a success response with different format?
      // Check if result is 0x00 anywhere
      for (int i = headerIdx + 3; i < bytesRead && i < headerIdx + 10; i++) {
        if (resp[i] == 0x00) {
          Serial.println("Found 0x00 result code, assuming success");
          return true;
        }
      }
      showStatus("Wrong cmd!", RED);
      delay(2000);
      return false;
    }
  } else {
    // No header found, but we got data - maybe different protocol?
    Serial.println("No header found, but got data. Checking for success pattern...");
    Serial.print("All received bytes: ");
    for (int i = 0; i < bytesRead; i++) {
      Serial.printf("0x%02X ", resp[i]);
    }
    Serial.println();
    
    // Look for common success patterns
    for (int i = 0; i < bytesRead - 1; i++) {
      // Maybe response is just [CMD] [RESULT]?
      if (resp[i] == CMD_CAPTURE_IMAGE && resp[i+1] == 0x00) {
        Serial.println("Found success pattern: cmd + 0x00");
        return true;
      }
      // Maybe response is [RESULT] where 0x00 = success?
      if (resp[i] == 0x00 && bytesRead == 1) {
        Serial.println("Found single 0x00 byte, assuming success");
        return true;
      }
    }
    
    // Check if any byte is 0x00 (might indicate success)
    for (int i = 0; i < bytesRead; i++) {
      if (resp[i] == 0x00) {
        Serial.printf("Found 0x00 at index %d, assuming capture success\n", i);
        return true;
      }
    }
    
    Serial.println("Unknown response format - but got data, assuming success for now");
    Serial.println("Please check Serial Monitor for actual bytes received");
    // For now, if we got ANY response, assume it might be success
    // This will let us proceed to get image size
    return true;
  }
}

// Get image size
bool getImageSize(uint16_t* width, uint16_t* height) {
  Serial.println("Getting image size...");
  showStatus("Getting size...", CYAN);
  
  // Clear buffer first
  while (Serial2.available()) Serial2.read();
  delay(200);
  
  if (!sendCmd(CMD_GET_IMAGE_SIZE)) {
    return false;
  }
  
  delay(1000);
  
  // Read ALL available bytes
  uint8_t resp[128] = {0};
  int bytesRead = 0;
  unsigned long start = millis();
  
  while (millis() - start < 3000) {
    if (Serial2.available()) {
      if (bytesRead < 128) {
        resp[bytesRead] = Serial2.read();
        Serial.printf("Size Rx[%d]: 0x%02X\n", bytesRead, resp[bytesRead]);
        bytesRead++;
      } else {
        Serial2.read();
      }
    }
    delay(10);
  }
  
  if (bytesRead == 0) {
    Serial.println("No response to get image size");
    return false;
  }
  
  Serial.printf("Got %d bytes for size:\n", bytesRead);
  for (int i = 0; i < bytesRead && i < 16; i++) {
    Serial.printf("  [%d]: 0x%02X\n", i, resp[i]);
  }
  
  // Try to find header
  int headerIdx = -1;
  for (int i = 0; i < bytesRead; i++) {
    if (resp[i] == CMD_HEAD) {
      headerIdx = i;
      Serial.printf("Found header at index %d\n", i);
      break;
    }
  }
  
  if (headerIdx >= 0 && headerIdx + 6 < bytesRead) {
    uint8_t cmd = resp[headerIdx + 1];
    uint8_t paramLen = resp[headerIdx + 2];
    
    Serial.printf("cmd=0x%02X paramLen=%d\n", cmd, paramLen);
    
    if (cmd == CMD_GET_IMAGE_SIZE && paramLen >= 4) {
      // Size: width (2 bytes) + height (2 bytes)
      *width = (resp[headerIdx + 3] << 8) | resp[headerIdx + 4];
      *height = (resp[headerIdx + 5] << 8) | resp[headerIdx + 6];
      Serial.printf("Image size: %dx%d\n", *width, *height);
      return true;
    }
  }
  
  // Try alternative: maybe size is at fixed positions?
  if (bytesRead >= 7) {
    // Try assuming format: [HEAD] [CMD] [LEN] [W_H] [W_L] [H_H] [H_L] ...
    if (resp[0] == CMD_HEAD && resp[1] == CMD_GET_IMAGE_SIZE) {
      *width = (resp[3] << 8) | resp[4];
      *height = (resp[5] << 8) | resp[6];
      Serial.printf("Image size (alt): %dx%d\n", *width, *height);
      return true;
    }
  }
  
  // Try to find size bytes anywhere in response
  // Common fingerprint sensor sizes: 256x288, 192x192, 160x160, etc.
  for (int i = 0; i < bytesRead - 3; i++) {
    // Try reading as width (2 bytes) + height (2 bytes)
    uint16_t w = (resp[i] << 8) | resp[i+1];
    uint16_t h = (resp[i+2] << 8) | resp[i+3];
    
    // Check if values are reasonable (typical fingerprint sensor dimensions)
    if (w >= 64 && w <= 512 && h >= 64 && h <= 512) {
      *width = w;
      *height = h;
      Serial.printf("Image size (found at index %d): %dx%d\n", i, *width, *height);
      return true;
    }
  }
  
  // Last resort: use default common size
  Serial.println("Failed to parse image size, using default");
  Serial.print("All bytes: ");
  for (int i = 0; i < bytesRead; i++) {
    Serial.printf("0x%02X ", resp[i]);
  }
  Serial.println();
  
  // Try common fingerprint sensor sizes
  *width = 256;
  *height = 288;
  Serial.printf("Using default size: %dx%d\n", *width, *height);
  return true; // Return true with default size to proceed
}

// Get image data
bool getImageData(uint8_t* imgBuf, uint16_t width, uint16_t height, uint16_t maxSize) {
  Serial.println("Getting image data...");
  showStatus("Getting image...", CYAN);
  
  if (!sendCmd(CMD_GET_IMAGE)) {
    return false;
  }
  
  delay(500);
  
  // Read image data (raw bytes, not in packet format)
  uint16_t bytesRead = 0;
  uint16_t expectedBytes = width * height;
  unsigned long start = millis();
  
  Serial.printf("Reading %d bytes of image data...\n", expectedBytes);
  
  while (millis() - start < 10000 && bytesRead < expectedBytes && bytesRead < maxSize) {
    if (fpSerial->available()) {
      imgBuf[bytesRead++] = fpSerial->read();
      
      // Show progress every 10%
      if (expectedBytes > 0 && bytesRead % (expectedBytes / 10 + 1) == 0) {
        Serial.printf("Progress: %d%%\n", (bytesRead * 100) / expectedBytes);
      }
    }
    delay(1);
  }
  
  Serial.printf("Read %d bytes (expected %d)\n", bytesRead, expectedBytes);
  return (bytesRead > 0);
}

// Display grayscale image
void displayImage(uint8_t* img, uint16_t width, uint16_t height) {
  Serial.println("Displaying image...");
  showStatus("Displaying...", GREEN);
  
  M5.Display.fillScreen(BLACK);
  
  // Calculate scaling to fit screen
  int dw = M5.Display.width();
  int dh = M5.Display.height() - 30; // Leave space for status
  float sx = (float)dw / width;
  float sy = (float)dh / height;
  float scale = (sx < sy) ? sx : sy;
  
  int sw = (int)(width * scale);
  int sh = (int)(height * scale);
  int ox = (dw - sw) / 2;
  int oy = (dh - sh) / 2;
  
  Serial.printf("Scaling %dx%d to %dx%d (scale=%.2f)\n", width, height, sw, sh, scale);
  
  // Draw image pixel by pixel
  M5.Display.startWrite();
  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      int sx = (int)(x / scale);
      int sy = (int)(y / scale);
      int idx = sy * width + sx;
      
      if (idx < width * height) {
        uint8_t p = img[idx];
        uint16_t color = M5.Display.color565(p, p, p);
        M5.Display.writePixel(ox + x, oy + y, color);
      }
    }
  }
  M5.Display.endWrite();
  
  // Show info
  M5.Display.setCursor(10, M5.Display.height() - 25);
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(1);
  char sizeInfo[32];
  snprintf(sizeInfo, sizeof(sizeInfo), "Size: %dx%d", width, height);
  M5.Display.println(sizeInfo);
  
  M5.Display.setCursor(10, M5.Display.height() - 15);
  M5.Display.println("Displaying 10 seconds...");
}

// Test communication by listening first, then sending
bool testCommunication(unsigned long baud) {
  Serial.printf("\n=== Testing @ %lu baud ===\n", baud);
  
  if (Serial2) {
    Serial2.end();
  }
  delay(300);
  
  Serial2.begin(baud, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  delay(2000); // Give sensor more time
  
  // First, listen for any spontaneous data
  Serial.println("Listening for sensor...");
  int bytesReceived = 0;
  uint8_t response[64] = {0};
  unsigned long start = millis();
  while (millis() - start < 1500) {
    if (Serial2.available()) {
      if (bytesReceived < 64) {
        response[bytesReceived] = Serial2.read();
        Serial.printf("Rx[%d]: 0x%02X\n", bytesReceived, response[bytesReceived]);
        bytesReceived++;
      } else {
        Serial2.read();
      }
    }
    delay(10);
  }
  
  if (bytesReceived > 0) {
    Serial.printf("Sensor active! Got %d bytes\n", bytesReceived);
    workingBaud = baud;
    return true;
  }
  
  // Clear buffer
  while (Serial2.available()) Serial2.read();
  delay(200);
  
  // Try sending command
  Serial.println("Sending test command...");
  uint8_t testCmd[] = {0xAA, 0x07, 0x00, 0x07, 0x55};
  Serial.print("TX: ");
  for (int i = 0; i < 5; i++) {
    Serial.printf("0x%02X ", testCmd[i]);
  }
  Serial.println();
  
  Serial2.write(testCmd, 5);
  Serial2.flush();
  delay(1000);
  
  bytesReceived = 0;
  start = millis();
  while (millis() - start < 2000) {
    if (Serial2.available()) {
      if (bytesReceived < 64) {
        response[bytesReceived] = Serial2.read();
        Serial.printf("Rx[%d]: 0x%02X\n", bytesReceived, response[bytesReceived]);
        bytesReceived++;
      } else {
        Serial2.read();
      }
    }
    delay(10);
  }
  
  if (bytesReceived > 0) {
    Serial.printf("SUCCESS! Got %d bytes\n", bytesReceived);
    workingBaud = baud;
    return true;
  }
  
  Serial.println("No response");
  return false;
}

bool initSensor() {
  Serial.println("=== Initializing Fingerprint Sensor ===");
  Serial.printf("RX=GPIO%d, TX=GPIO%d\n", FINGERPRINT_RX, FINGERPRINT_TX);
  showStatus("Testing sensor...", CYAN);
  
  // Try multiple baud rates - prioritize 115200 as it's most common
  unsigned long baudRates[] = {115200, 19200, 57600, 9600, 38400, 230400};
  int numBauds = sizeof(baudRates) / sizeof(baudRates[0]);
  
  for (int i = 0; i < numBauds; i++) {
    Serial.printf("\n=== Trying baud rate: %lu ===\n", baudRates[i]);
    char statusMsg[32];
    snprintf(statusMsg, sizeof(statusMsg), "Testing: %lu", baudRates[i]);
    showStatus(statusMsg, YELLOW);
    
    if (testCommunication(baudRates[i])) {
      Serial.printf("\n*** SUCCESS! Sensor responds at %lu baud ***\n", baudRates[i]);
      snprintf(statusMsg, sizeof(statusMsg), "Found @ %lu", baudRates[i]);
      showStatus(statusMsg, GREEN);
      delay(1000);
      
      // Reinitialize Serial2 with working baud rate for future use
      Serial2.end();
      delay(200);
      Serial2.begin(workingBaud, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
      delay(500);
      
      return true;
    }
    Serial.printf("Failed at %lu baud\n", baudRates[i]);
    delay(500);
  }
  
  Serial.println("\n*** No communication found with any baud rate ***");
  Serial.println("Check:");
  Serial.println("  1. PORT.C connection (GPIO18, GPIO17)");
  Serial.println("  2. Power supply (5V)");
  Serial.println("  3. Sensor is powered on");
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
  
  // Wait for finger to be placed - check for sensor activity
  // Check immediately first (finger might already be there)
  bool fingerDetected = false;
  
  // Quick check first
  if (Serial2.available() > 0) {
    Serial.println("Sensor activity detected immediately!");
    uint8_t data = Serial2.read();
    Serial.printf("Received: 0x%02X\n", data);
    fingerDetected = true;
    while (Serial2.available()) {
      uint8_t b = Serial2.read();
      Serial.printf("  Additional: 0x%02X\n", b);
    }
    delay(300);
  } else {
    // If no immediate activity, wait and check periodically
    unsigned long waitStart = millis();
    while (millis() - waitStart < 2000) { // Wait up to 2 seconds
      if (Serial2.available() > 0) {
        Serial.println("Sensor activity detected!");
        uint8_t data = Serial2.read();
        Serial.printf("Received: 0x%02X\n", data);
        fingerDetected = true;
        while (Serial2.available()) {
          uint8_t b = Serial2.read();
          Serial.printf("  Additional: 0x%02X\n", b);
        }
        delay(300);
        break;
      }
      delay(50); // Check every 50ms
      M5.update();
    }
  }
  
  if (!fingerDetected) {
    // No finger detected, wait a bit and try again
    delay(200);
    return;
  }
  
  // Finger detected, now try to capture
  Serial.println("Finger detected! Attempting capture...");
  showStatus("Finger detected!", GREEN);
  delay(300);
  
  // Try to capture image
  if (captureImage()) {
    // Get image size
    uint16_t width = 0, height = 0;
    if (getImageSize(&width, &height) && width > 0 && height > 0) {
      // Allocate buffer for image (max 320x240 = 76800 bytes)
      uint16_t maxPixels = width * height;
      if (maxPixels > 76800) {
        Serial.printf("Image too large: %dx%d\n", width, height);
        showStatus("Image too large!", RED);
        delay(2000);
      } else {
        // Allocate memory for image
        uint8_t* imgBuf = (uint8_t*)malloc(maxPixels);
        if (imgBuf) {
          if (getImageData(imgBuf, width, height, maxPixels)) {
            Serial.println("Image data received, displaying...");
            displayImage(imgBuf, width, height);
            delay(10000); // Display for 10 seconds
          } else {
            Serial.println("Failed to get image data");
            showStatus("No image data", RED);
            delay(2000);
          }
          free(imgBuf);
        } else {
          Serial.println("Failed to allocate image buffer");
          showStatus("Memory error", RED);
          delay(2000);
        }
      }
    } else {
      Serial.println("Failed to get image size");
      showStatus("Size error", RED);
      delay(1500); // Show error for 1.5 seconds
      // Clear any pending data before going back to waiting
      while (Serial2.available()) Serial2.read();
    }
  } else {
    // Capture failed - show error and wait before retrying
    Serial.println("Capture failed, will retry...");
    showStatus("Capture failed", RED);
    delay(1500); // Show error for 1.5 seconds
    // Clear any pending data before going back to waiting
    while (Serial2.available()) Serial2.read();
  }
  
  // No delay here - go straight back to waiting
}
