// ESP32 Lolin Wemos s2 Mini + AHT10 -> UDP sender
//
// Reads temperature and humidity from an AHT10 sensor and sends them as a small
// JSON packet over UDP to the M5Stack Tab5 (which uses a static IP).
//
// Required library (Library Manager): "Adafruit AHTX0" (it also installs
// "Adafruit BusIO" and "Adafruit Unified Sensor").
//
// Arduino IDE board: "(Lolin) Wemos s2 Mini"
//
// Selcuk Duven - 2027

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Adafruit_AHTX0.h>

// ---------------------------------------------------------------------------
// User settings
// ---------------------------------------------------------------------------

// Wi-Fi network
static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Tab5 receiver (must match the static IP configured in the Tab5 sketch)
static const IPAddress TAB5_IP(192, 168, 1, 50);
static const uint16_t  UDP_PORT = 4210;

// I2C pins used for the AHT10. Check the LilyGO T4-S3 pin diagram and change
// them if you connect the sensor to other free pins.
//#define SENSOR_SDA 6
//#define SENSOR_SCL 7

// Device name sent in every packet (the receiver ignores other names)
static const char* DEVICE_ID = "s2mini";

// Send interval in milliseconds
static const uint32_t SEND_INTERVAL_MS = 5000;

// Calibration offsets (use them to compensate self-heating of the board)
static const float TEMP_OFFSET_C = 0.0f;
static const float HUM_OFFSET_PCT = 0.0f;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static Adafruit_AHTX0 aht;
static WiFiUDP        udp;
static bool           sensorOk = false;
static uint32_t       lastSend = 0;
static uint32_t       seq      = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Connect to Wi-Fi (blocks up to timeoutMs). Returns true when connected.
static bool wifi_connect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("Connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("Wi-Fi connection failed");
  return false;
}

// Try to start the AHT10 sensor on the configured I2C pins
static bool sensor_begin() {
  //Wire.begin(SENSOR_SDA, SENSOR_SCL);
  //if (aht.begin(&Wire)) {
    if(aht.begin()) {
    Serial.println("AHT10 found");
    return true;
  }
  Serial.println("AHT10 not found, check wiring and I2C pins");
  return false;
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(15, OUTPUT);
  delay(500);

  WiFi.persistent(false);          // Do not wear the flash with credentials
  WiFi.setAutoReconnect(true);     // Reconnect automatically after a drop

  sensorOk = sensor_begin();
  wifi_connect(15000);
}

void loop() {
  uint32_t now = millis();
  if (now - lastSend < SEND_INTERVAL_MS) {
    delay(10);
    return;
  }
  lastSend = now;

  // Make sure the sensor is available (retry if it was not found at boot)
  if (!sensorOk) {
    sensorOk = sensor_begin();
    if (!sensorOk) return;
  }

  // Make sure Wi-Fi is up
  if (!wifi_connect(10000)) return;

  // Read the sensor
  sensors_event_t humidity, temp;
  if (!aht.getEvent(&humidity, &temp)) {
    Serial.println("Sensor read failed");
    sensorOk = false;              // Force a re-init on the next round
    return;
  }
  float t = temp.temperature + TEMP_OFFSET_C;
  float h = humidity.relative_humidity + HUM_OFFSET_PCT;
  if (h < 0) h = 0;
  if (h > 100) h = 100;

  // Build the JSON packet: {"id":"t4s3","seq":12,"t":23.45,"h":48.20}
  char payload[96];
  int len = snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"seq\":%lu,\"t\":%.2f,\"h\":%.2f}",
                     DEVICE_ID, (unsigned long)seq++, t, h);

  // Send it to the Tab5
  udp.beginPacket(TAB5_IP, UDP_PORT);
  udp.write((const uint8_t*)payload, len);
  bool ok = udp.endPacket();

  Serial.printf("%s -> %s:%u  %s\n", ok ? "sent" : "FAILED", TAB5_IP.toString().c_str(), UDP_PORT, payload);
}
