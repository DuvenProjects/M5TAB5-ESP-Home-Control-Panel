// ============================================================================
//  alarm_unit.ino  -  Fire / smoke / gas alarm unit for the Tab5 Home panel
//
//  Board   : LOLIN (WEMOS) ESP32-S2 mini   (Arduino: "LOLIN S2 Mini")
//  Sensors : KY-026 flame sensor, MQ-2 smoke sensor, MQ-5 gas sensor
//  Output  : UDP packet to the Tab5 (udp_alarm.cpp on the Tab5)
//              {"id":"s2mini_alarm","seq":7,"fire":0,"smoke":1,"gas":0,
//               "mq2":812,"mq5":640,"warm":0}
//            sent at once on every change and every HEARTBEAT_MS as a sign of life.
//            (the Tab5 only reads id / fire / smoke / gas, the other fields are
//             for diagnostics)
//
//  ---------------------------------------------------------------- WIRING
//   KY-026 flame  : VCC -> 3V3   GND -> GND   DO -> GPIO 7   (AO -> GPIO 9, optional)
//   MQ-2  smoke   : VCC -> 5V (VBUS)          GND -> GND
//                   AO  -> voltage divider -> GPIO 3
//   MQ-5  gas     : VCC -> 5V (VBUS)          GND -> GND
//                   AO  -> voltage divider -> GPIO 5
//   Voltage divider (MQ modules output up to 5 V, the ESP32-S2 takes max. 3.3 V):
//        AO ──[ 10 kΩ ]──┬── GPIO
//                        └──[ 20 kΩ ]── GND        (pin voltage = AO x 2/3)
//   Do NOT connect the DO pins of the MQ modules directly: they are pulled up to 5 V.
//   Optional buzzer (active, 3.3 V) : + -> GPIO 11, - -> GND
//
//  ---------------------------------------------------------------- NOTES
//   - MQ sensors need a heating time: alarms are ignored for WARMUP_MS after
//     start-up ("warm":1). A brand-new MQ sensor needs 24-48 h of burn-in
//     before its values are stable.
//   - Calibrate: open the Serial Monitor (115200), note the values in clean
//     air and set MQ2_ALARM_MV / MQ5_ALARM_MV well above them. Test with a lighter
//     (gas without flame) for MQ-5 and with smoke for MQ-2.
//   - KY-026: adjust the blue potentiometer so that the DO LED switches on
//     with a flame at the wanted distance. If "fire" is reported the wrong way
//     round, change FLAME_ACTIVE_LEVEL.
//   - These hobby sensors do NOT replace certified smoke / gas detectors.
// ============================================================================
#include <WiFi.h>
#include <WiFiUdp.h>

// ---------------------------------------------------------------- user settings
static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Tab5 address and port (udp_th.h on the Tab5: UDP_LOCAL_IP, UDP_PORT)
static const IPAddress TAB5_IP(192, 168, 1, 50);
static const uint16_t  TAB5_PORT = 4210;

static const char* DEVICE_ID = "s2mini_alarm";   // = UDP_ALARM_ID on the Tab5

// Pins (ESP32-S2: use ADC1 pins GPIO 1..10, ADC2 does not work with Wi-Fi)
#define PIN_FLAME_DO        7
#define PIN_FLAME_AO        9      // only printed (diagnostics), -1 = not connected
#define PIN_MQ2_AO          3      // smoke
#define PIN_MQ5_AO          5      // gas
#define PIN_BUZZER          11     // local buzzer, -1 = none
#define PIN_LED             15     // on-board LED of the S2 mini

#define FLAME_ACTIVE_LEVEL  HIGH   // DO level when a flame is seen (swap if inverted)

// Voltage divider factor: sensor voltage = pin voltage x DIVIDER
#define DIVIDER             1.5f   // 10 kΩ / 20 kΩ

// Alarm thresholds in sensor millivolts (AO of the module). Alarm above
// *_ALARM_MV, clear below *_CLEAR_MV (hysteresis). Calibrate them!
#define MQ2_ALARM_MV        2000
#define MQ2_CLEAR_MV        1700
#define MQ5_ALARM_MV        2000
#define MQ5_CLEAR_MV        1700

#define WARMUP_MS           60000UL   // MQ heating time after power-on
#define SAMPLE_MS           200UL     // sensor reading interval
#define ON_SAMPLES          5         // samples in a row to raise an alarm (1 s)
#define OFF_SAMPLES         15        // samples in a row to clear it (3 s)
#define HEARTBEAT_MS        10000UL   // packet also without changes (Tab5 offline limit: 90 s)
#define WIFI_RETRY_MS       15000UL

// ---------------------------------------------------------------- state
struct Detector {
  bool    active = false;
  uint8_t onCnt = 0, offCnt = 0;
  // Debounce: 'raw' must stay the same for several samples
  bool update(bool raw) {
    if (raw) { offCnt = 0; if (!active && ++onCnt >= ON_SAMPLES)  { active = true;  onCnt = 0; return true; } }
    else     { onCnt = 0;  if (active && ++offCnt >= OFF_SAMPLES) { active = false; offCnt = 0; return true; } }
    return false;
  }
};

static WiFiUDP  udp;
static Detector fire, smoke, gas;
static uint32_t seq = 0, lastSample = 0, lastSend = 0, lastWifiTry = 0, lastLog = 0;
static int      mq2mv = 0, mq5mv = 0, flameRaw = -1;

// ---------------------------------------------------------------- helpers
static int sensorMv(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(pin);   // calibrated pin voltage
  return (int)((sum / 8) * DIVIDER);
}

static bool warm() { return millis() >= WARMUP_MS; }

static void sendPacket() {
  if (WiFi.status() != WL_CONNECTED) return;
  char buf[160];
  int n = snprintf(buf, sizeof(buf),
                   "{\"id\":\"%s\",\"seq\":%lu,\"fire\":%d,\"smoke\":%d,\"gas\":%d,"
                   "\"mq2\":%d,\"mq5\":%d,\"warm\":%d}",
                   DEVICE_ID, (unsigned long)++seq, fire.active, smoke.active, gas.active,
                   mq2mv, mq5mv, warm() ? 0 : 1);
  udp.beginPacket(TAB5_IP, TAB5_PORT);
  udp.write((const uint8_t*)buf, n);
  udp.endPacket();
  lastSend = millis();
  Serial.printf("[UDP] %s\n", buf);
}

static void wifiKeepAlive() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < WIFI_RETRY_MS && lastWifiTry != 0) return;
  lastWifiTry = millis();
  Serial.println("[WiFi] connecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ---------------------------------------------------------------- Arduino
void setup() {
  Serial.begin(115200);
  pinMode(PIN_FLAME_DO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  if (PIN_BUZZER >= 0) { pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW); }
  analogSetAttenuation(ADC_11db);                    // ESP32-S2: 0 .. ~2.5 V on the pin = 0 .. ~3.7 V sensor

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiKeepAlive();
  Serial.printf("Alarm unit '%s' -> %s:%u  (MQ warm-up %lu s)\n", DEVICE_ID,
                TAB5_IP.toString().c_str(), TAB5_PORT, WARMUP_MS / 1000);
}

void loop() {
  uint32_t now = millis();
  wifiKeepAlive();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    mq2mv = sensorMv(PIN_MQ2_AO);
    mq5mv = sensorMv(PIN_MQ5_AO);
    if (PIN_FLAME_AO >= 0) flameRaw = analogRead(PIN_FLAME_AO);
    bool flameRawOn = digitalRead(PIN_FLAME_DO) == FLAME_ACTIVE_LEVEL;

    // Smoke / gas only after the warm-up; hysteresis between ALARM and CLEAR
    bool smokeRaw = warm() && (smoke.active ? mq2mv > MQ2_CLEAR_MV : mq2mv >= MQ2_ALARM_MV);
    bool gasRaw   = warm() && (gas.active   ? mq5mv > MQ5_CLEAR_MV : mq5mv >= MQ5_ALARM_MV);

    bool changed = fire.update(flameRawOn);
    changed |= smoke.update(smokeRaw);
    changed |= gas.update(gasRaw);
    if (changed) sendPacket();                       // at once on every change

    bool any = fire.active || smoke.active || gas.active;
    digitalWrite(PIN_LED, any ? (now / 250) % 2 : (now % 2000) < 50);   // blink fast / short flash
    if (PIN_BUZZER >= 0) digitalWrite(PIN_BUZZER, any && (now / 400) % 2);
  }

  if (now - lastSend >= HEARTBEAT_MS) sendPacket();  // sign of life

  if (now - lastLog >= 1000) {                       // values for calibration
    lastLog = now;
    Serial.printf("MQ-2 %4d mV  MQ-5 %4d mV  flame DO=%d AO=%d  %s  WiFi %s\n", mq2mv, mq5mv,
                  digitalRead(PIN_FLAME_DO), flameRaw, warm() ? "" : "(warming up)",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "--");
  }
}
