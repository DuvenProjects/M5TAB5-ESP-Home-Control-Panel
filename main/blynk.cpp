// ============================================================================
//  blynk.cpp  -  Blynk IoT link (control functions only, no LVGL code)
//
//  Threads:
//   * Blynk task (core 0): keeps the connection (Blynk.run()), receives the
//     app commands and sends the values. It never touches the HVAC module.
//   * Main / LVGL thread: blynk_process() (every 100 ms, from ui.cpp) applies
//     the queued commands to the HVAC module and copies the values to send
//     into a snapshot. All HVAC calls stay in the main thread.
//
//  Sending: a value is sent only when it changes (at most once per second),
//  plus a full refresh after every (re)connection and every 10 minutes.
// ============================================================================
#include "blynk.h"            // BLYNK_TEMPLATE_ID / NAME / AUTH_TOKEN first

#define BLYNK_PRINT Serial    // Blynk log on the serial monitor (remove to silence)

#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <math.h>

#include "app_data.h"         // g_data: indoor / outdoor values, HVAC target
#include "hvac.h"             // hvac_power(), hvac_set_power(), hvac_change_target() ...
#include "udp_th.h"           // g_outdoor: outdoor temperature / humidity
#include "settings.h"         // settings_wifi_connected()
#include "ui.h"               // alarm: ui_alarm_set_armed(), ui_alarm_reason(), tr()
#include "udp_alarm.h"        // fire / smoke / gas alarm unit
#include "aq.h"               // ENS160 air quality values

// ============================================================ SHARED STATE
enum RemoteCmd : uint8_t { RCMD_POWER_OFF = 0, RCMD_POWER_ON, RCMD_TARGET_DOWN, RCMD_TARGET_UP,
                           RCMD_ALARM_DISARM, RCMD_ALARM_ARM,
                           RCMD_SAFETY_OFF, RCMD_SAFETY_ON, RCMD_DISARM_ALL };

struct Snapshot {
  float inTemp, inHum, outTemp, outHum, target;     // NAN = not available
  bool  power, heating, cooling;
  bool  alarmArmed, alarmActive;
  bool  safetyArmed, fire, smoke, gas;               // fire / smoke / gas alarm unit
  char  alarmText[64];                              // ringing reason / detections / "No alarm"
  bool     aqValid;                                 // ENS160 values usable
  uint8_t  aqi;                                     // 1..5
  uint16_t eco2, tvoc;                              // ppm, ppb
  char     aqText[40];                              // level name / sensor state (translated)
};

static QueueHandle_t  s_cmdQueue  = nullptr;        // app -> main thread
static portMUX_TYPE   s_mux       = portMUX_INITIALIZER_UNLOCKED;
static Snapshot       s_snap      = {NAN, NAN, NAN, NAN, NAN, false, false, false, false, false,
                                     false, false, false, false, "",
                                     false, 0, 0, 0, ""};
static volatile bool  s_resendAlarm  = false;       // after an arm / disarm command
static volatile bool  s_eventPending = false;       // alarm notification waiting to be sent
static char           s_eventText[120] = "";        // protected by s_mux
static volatile bool  s_linkAllowed    = false;          // Wi-Fi + Settings switch (main thread)
static volatile bool  s_resendPower  = false;       // after a power command (sync the app switch)
static volatile bool  s_resendTarget = false;       // after a - / + command (limits)
static volatile bool  s_connected = false;
static bool           s_enabled   = false;

#define TOKEN_PLACEHOLDER   "YOUR_BLYNK_AUTH_TOKEN"
#define CONNECT_RETRY_MS    15000UL
#define CONNECT_TIMEOUT_MS  4000UL
#define SEND_INTERVAL_MS    1000UL
#define FULL_REFRESH_MS     600000UL                // 10 minutes

// ============================================================ APP -> DEVICE
// Runs in the Blynk task: only queue the command, the main thread applies it
static void postCmd(RemoteCmd c) {
  if (s_cmdQueue) xQueueSend(s_cmdQueue, &c, 0);
}

BLYNK_WRITE(VPIN_HVAC_POWER) {
  postCmd(param.asInt() ? RCMD_POWER_ON : RCMD_POWER_OFF);
}

BLYNK_WRITE(VPIN_ALARM_ARM) {
  postCmd(param.asInt() ? RCMD_ALARM_ARM : RCMD_ALARM_DISARM);
}

BLYNK_WRITE(VPIN_SAFETY_ARM) {
  postCmd(param.asInt() ? RCMD_SAFETY_ON : RCMD_SAFETY_OFF);
}

BLYNK_WRITE(VPIN_DISARM_ALL) {
  if (param.asInt()) postCmd(RCMD_DISARM_ALL);      // on press only
}

BLYNK_WRITE(VPIN_TARGET_DOWN) {
  if (param.asInt()) postCmd(RCMD_TARGET_DOWN);      // on press only (push button 1 -> 0)
}

BLYNK_WRITE(VPIN_TARGET_UP) {
  if (param.asInt()) postCmd(RCMD_TARGET_UP);
}

static volatile bool s_pushAll = true;

BLYNK_CONNECTED() {
  // The device is the source of truth: send its state instead of syncing
  // the old values stored on the server
  s_pushAll = true;
}

// ============================================================ DEVICE -> APP (Blynk task)
static Snapshot s_sent;                             // last values sent
static bool     s_sentValid = false;

static void sendFloat(int pin, float v) {
  char b[16];
  snprintf(b, sizeof(b), "%.1f", v);
  Blynk.virtualWrite(pin, b);
}

static void sendLed(int pin, bool on) { Blynk.virtualWrite(pin, on ? 255 : 0); }

static bool changed(float a, float b, float tol) {
  if (isnan(a)) return false;                       // nothing valid to send
  if (isnan(b)) return true;
  return fabsf(a - b) >= tol;
}

static void pushValues(bool all) {
  Snapshot s;
  portENTER_CRITICAL(&s_mux);
  s = s_snap;
  portEXIT_CRITICAL(&s_mux);

  bool first = all || !s_sentValid;
  bool rp = s_resendPower;  s_resendPower = false;
  bool rt = s_resendTarget; s_resendTarget = false;

  if (first || changed(s.inTemp,  s_sent.inTemp,  0.05f)) { if (!isnan(s.inTemp))  sendFloat(VPIN_INDOOR_TEMP,  s.inTemp);  s_sent.inTemp  = s.inTemp; }
  if (first || changed(s.inHum,   s_sent.inHum,   0.5f))  { if (!isnan(s.inHum))   sendFloat(VPIN_INDOOR_HUM,   s.inHum);   s_sent.inHum   = s.inHum; }
  if (first || changed(s.outTemp, s_sent.outTemp, 0.05f)) { if (!isnan(s.outTemp)) sendFloat(VPIN_OUTDOOR_TEMP, s.outTemp); s_sent.outTemp = s.outTemp; }
  if (first || changed(s.outHum,  s_sent.outHum,  0.5f))  { if (!isnan(s.outHum))  sendFloat(VPIN_OUTDOOR_HUM,  s.outHum);  s_sent.outHum  = s.outHum; }
  if (first || rt || changed(s.target, s_sent.target, 0.05f)) { if (!isnan(s.target)) sendFloat(VPIN_HVAC_TARGET, s.target); s_sent.target = s.target; }

  if (first || rp || s.power != s_sent.power) {
    Blynk.virtualWrite(VPIN_HVAC_POWER, s.power ? 1 : 0);   // keeps the app switch in sync
    sendLed(VPIN_LED_POWER, s.power);
    s_sent.power = s.power;
  }
  if (first || s.heating != s_sent.heating) { sendLed(VPIN_LED_HEATING, s.heating); s_sent.heating = s.heating; }
  if (first || s.cooling != s_sent.cooling) { sendLed(VPIN_LED_COOLING, s.cooling); s_sent.cooling = s.cooling; }

  bool ra = s_resendAlarm; s_resendAlarm = false;
  if (first || ra || s.alarmArmed != s_sent.alarmArmed) {
    Blynk.virtualWrite(VPIN_ALARM_ARM, s.alarmArmed ? 1 : 0);   // keeps the app switch in sync
    sendLed(VPIN_LED_ALARM_ARMED, s.alarmArmed);
    s_sent.alarmArmed = s.alarmArmed;
  }
  if (first || s.alarmActive != s_sent.alarmActive) { sendLed(VPIN_LED_ALARM, s.alarmActive); s_sent.alarmActive = s.alarmActive; }
  if (first || ra || s.safetyArmed != s_sent.safetyArmed) {
    Blynk.virtualWrite(VPIN_SAFETY_ARM, s.safetyArmed ? 1 : 0);
    sendLed(VPIN_LED_SAFETY_ARMED, s.safetyArmed);
    s_sent.safetyArmed = s.safetyArmed;
  }
  if (first || s.fire  != s_sent.fire)  { sendLed(VPIN_LED_FIRE,  s.fire);  s_sent.fire  = s.fire; }
  if (first || s.smoke != s_sent.smoke) { sendLed(VPIN_LED_SMOKE, s.smoke); s_sent.smoke = s.smoke; }
  if (first || s.gas   != s_sent.gas)   { sendLed(VPIN_LED_GAS,   s.gas);   s_sent.gas   = s.gas; }
  if (first || strcmp(s.alarmText, s_sent.alarmText) != 0) {
    Blynk.virtualWrite(VPIN_ALARM_TEXT, s.alarmText);
    memcpy(s_sent.alarmText, s.alarmText, sizeof(s.alarmText));
  }

  // Air quality (ENS160): only valid values; small eCO2 / TVOC changes are not sent
  if (s.aqValid) {
    if (first || s.aqi != s_sent.aqi) { Blynk.virtualWrite(VPIN_AQ_INDEX, s.aqi); s_sent.aqi = s.aqi; }
    if (first || abs((int)s.eco2 - (int)s_sent.eco2) >= 10) { Blynk.virtualWrite(VPIN_AQ_ECO2, s.eco2); s_sent.eco2 = s.eco2; }
    if (first || abs((int)s.tvoc - (int)s_sent.tvoc) >= 5)  { Blynk.virtualWrite(VPIN_AQ_TVOC, s.tvoc); s_sent.tvoc = s.tvoc; }
  }
  if (first || strcmp(s.aqText, s_sent.aqText) != 0) {
    Blynk.virtualWrite(VPIN_AQ_TEXT, s.aqText);
    memcpy(s_sent.aqText, s.aqText, sizeof(s.aqText));
  }

  // Alarm notification: kept pending until it could be sent
  if (s_eventPending) {
    char txt[sizeof(s_eventText)];
    portENTER_CRITICAL(&s_mux);
    memcpy(txt, s_eventText, sizeof(txt));
    s_eventPending = false;
    portEXIT_CRITICAL(&s_mux);
    Blynk.logEvent(BLYNK_ALARM_EVENT, txt);
  }
  s_sentValid = true;
}

static void blynkTask(void*) {
  Blynk.config(BLYNK_AUTH_TOKEN);                   // default server: blynk.cloud
  uint32_t lastTry = 0, lastSend = 0, lastFull = 0;
  bool tried = false;
  for (;;) {
    uint32_t now = millis();
    if (s_linkAllowed) {
      if (Blynk.connected()) {
        Blynk.run();
        bool all = s_pushAll || (now - lastFull >= FULL_REFRESH_MS);
        if (all || now - lastSend >= SEND_INTERVAL_MS || s_resendPower || s_resendTarget ||
            s_resendAlarm || s_eventPending) {
          if (all) { s_pushAll = false; lastFull = now; }
          lastSend = now;
          pushValues(all);
        }
      } else if (!tried || now - lastTry >= CONNECT_RETRY_MS) {
        tried = true;
        lastTry = now;
        Blynk.connect(CONNECT_TIMEOUT_MS);          // blocks only this task
      }
    } else {
      if (Blynk.connected()) Blynk.disconnect();
      tried = false;                                // connect at once when allowed again
    }
    s_connected = Blynk.connected();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================ MAIN THREAD
void blynk_begin() {
  if (s_enabled) return;
  if (strcmp(BLYNK_AUTH_TOKEN, TOKEN_PLACEHOLDER) == 0 || strlen(BLYNK_AUTH_TOKEN) < 20) {
    Serial.println("[BLYNK] disabled: set BLYNK_AUTH_TOKEN in blynk.h");
    return;
  }
  s_cmdQueue = xQueueCreate(16, sizeof(RemoteCmd));
  s_enabled = true;
  blynk_process();                                  // first snapshot before the task starts
  xTaskCreatePinnedToCore(blynkTask, "blynk", 8192, nullptr, 2, nullptr, 0);
}

void blynk_process() {
  if (!s_enabled) return;
  // Link allowed only with Wi-Fi and the Settings > System > Blynk switch on
  s_linkAllowed = settings_wifi_connected() && settings_blynk_enabled();

  // ---- apply the commands of the app (saved to NVS by the HVAC module)
  RemoteCmd c;
  while (xQueueReceive(s_cmdQueue, &c, 0) == pdTRUE) {
    switch (c) {
      case RCMD_POWER_ON:    hvac_set_power(true);  s_resendPower = true; break;
      case RCMD_POWER_OFF:   hvac_set_power(false); s_resendPower = true; break;
      case RCMD_TARGET_DOWN: hvac_change_target(-BLYNK_TARGET_STEP); s_resendTarget = true; break;
      case RCMD_TARGET_UP:   hvac_change_target(+BLYNK_TARGET_STEP); s_resendTarget = true; break;
      case RCMD_ALARM_ARM:    ui_alarm_set_armed(true);  s_resendAlarm = true; break;   // no PIN
      case RCMD_ALARM_DISARM: ui_alarm_set_armed(false); s_resendAlarm = true; break;
      case RCMD_SAFETY_ON:    udp_alarm_set_safety_armed(true);  s_resendAlarm = true; break;
      case RCMD_SAFETY_OFF:   udp_alarm_set_safety_armed(false); s_resendAlarm = true; break;
      case RCMD_DISARM_ALL:   ui_alarm_disarm_all(); s_resendAlarm = true; break;      // everything off
    }
  }

  // ---- new alarm: queue the notification for the app (sent by the Blynk task)
  // (one notification per trigger: a new fire / smoke / gas detection during a
  //  ringing alarm sends a new one)
  static uint32_t lastSeq = 0;
  if (ui_alarm_seq() != lastSeq && g_data.alarm_active) {
    portENTER_CRITICAL(&s_mux);
    snprintf(s_eventText, sizeof(s_eventText), "%s: %s", tr(S_ALARM), ui_alarm_reason());
    s_eventPending = true;
    portEXIT_CRITICAL(&s_mux);
  }
  lastSeq = ui_alarm_seq();

  Snapshot s;
  bool sensorOk = hvac_sensor_ok();
  s.inTemp  = sensorOk ? g_data.indoor_temp : NAN;
  s.inHum   = sensorOk ? g_data.indoor_hum  : NAN;
  s.outTemp = g_outdoor.temp;                       // outdoor unit or OpenWeatherMap (udp_th.h)
  s.outHum  = g_outdoor.hum;
  s.target  = g_data.hvac_setpoint;
  s.power   = hvac_power();
  s.heating = hvac_heating();
  s.cooling = hvac_cooling();
  s.alarmArmed  = g_data.alarm_armed;
  s.alarmActive = g_data.alarm_active;
  const SafetyState& sf = udp_alarm_state();
  bool online = sf.unit == ALARM_UNIT_ONLINE;
  s.safetyArmed = udp_alarm_safety_armed();
  s.fire  = online && sf.fire;
  s.smoke = online && sf.smoke;
  s.gas   = online && sf.gas;
  if (g_data.alarm_active) {
    snprintf(s.alarmText, sizeof(s.alarmText), "%s", ui_alarm_reason());
  } else {
    s.alarmText[0] = 0;
    const bool d[3] = {s.fire, s.smoke, s.gas};
    const StrId n[3] = {S_AL_FIRE, S_AL_SMOKE, S_AL_GAS};
    for (int i = 0; i < 3; i++) if (d[i]) {
      if (s.alarmText[0]) strlcat(s.alarmText, ", ", sizeof(s.alarmText));
      strlcat(s.alarmText, tr(n[i]), sizeof(s.alarmText));
    }
    if (!s.alarmText[0])
      strlcpy(s.alarmText, tr(sf.unit == ALARM_UNIT_OFFLINE ? S_AL_UNIT_OFFLINE : S_AL_NO_ALARM), sizeof(s.alarmText));
  }
  const AqData& aq = aq_get();
  s.aqValid = aq_has_values();
  s.aqi     = aq.aqi;
  s.eco2    = aq.eco2;
  s.tvoc    = aq.tvoc;
  strlcpy(s.aqText, aq_state_text(), sizeof(s.aqText));

  portENTER_CRITICAL(&s_mux);
  s_snap = s;
  portEXIT_CRITICAL(&s_mux);
}

bool blynk_connected() { return s_connected; }

BlynkLinkState blynk_state() {
  if (!s_enabled || !settings_blynk_enabled()) return BLYNK_LINK_OFF;
  return s_connected ? BLYNK_LINK_ONLINE : BLYNK_LINK_OFFLINE;
}
