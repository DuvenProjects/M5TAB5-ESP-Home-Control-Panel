// ============================================================================
//  udp_alarm.cpp  -  Fire / smoke / gas alarm unit (see udp_alarm.h)
//  Built like udp_th.cpp: Wi-Fi is owned by settings.cpp, this module only
//  reads the packets. Runs in the main thread (called by ui.cpp). No LVGL code.
// ============================================================================
#include "udp_alarm.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <string.h>

#include "settings.h"     // settings_wifi_connected()
#include "lang.h"         // tr()

void ui_alarm_trigger(const char* reason);   // ui.cpp

static SafetyState s_st;
static bool        s_armed    = true;         // fire / smoke / gas alarm switched on
static uint32_t    s_lastRxMs = 0;
static bool        s_hasData  = false;
static bool        s_notified[3];             // alarm already raised for this detection
static Preferences s_prefs;

#if UDP_ALARM_PORT != UDP_PORT
static WiFiUDP s_udp;                          // own socket (other port than udp_th)
static bool    s_udpStarted = false;
#endif

// ---------------------------------------------------------------- packet parsing
static bool json_string(const char* json, const char* key, char* out, size_t outLen) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p += strlen(pattern);
  const char* e = strchr(p, '"');
  if (!e) return false;
  size_t n = e - p;
  if (n >= outLen) n = outLen - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// Flag following "key": -> 0 / 1 / true / false (missing key -> false)
static bool json_flag(const char* json, const char* key) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p += strlen(pattern);
  while (*p == ' ' || *p == '"') p++;
  if (*p == 't' || *p == 'T') return true;
  return strtol(p, nullptr, 10) != 0;
}

bool udp_alarm_packet(const char* json, const char* remote) {
  char id[20];
  if (!json_string(json, "id", id, sizeof(id)) || strcmp(id, UDP_ALARM_ID) != 0) return false;
  s_st.fire  = json_flag(json, "fire");
  s_st.smoke = json_flag(json, "smoke");
  s_st.gas   = json_flag(json, "gas");
  strlcpy(s_st.remote, remote ? remote : "", sizeof(s_st.remote));
  s_st.packets++;
  s_hasData  = true;
  s_lastRxMs = millis();
#if UDP_DEBUG
  Serial.printf("[ALARM UDP] fire=%d smoke=%d gas=%d\n", s_st.fire, s_st.smoke, s_st.gas);
#endif
  return true;
}

#if UDP_ALARM_PORT != UDP_PORT
static void own_socket_poll() {
  bool wifiOk = settings_wifi_connected();
  if (wifiOk && !s_udpStarted) s_udpStarted = s_udp.begin(UDP_ALARM_PORT) == 1;
  else if (!wifiOk && s_udpStarted) { s_udp.stop(); s_udpStarted = false; }
  if (!s_udpStarted) return;
  for (int i = 0; i < 4; i++) {
    if (s_udp.parsePacket() <= 0) return;
    char buf[256];
    int len = s_udp.read(buf, sizeof(buf) - 1);
    if (len <= 0) continue;
    buf[len] = 0;
    udp_alarm_packet(buf, s_udp.remoteIP().toString().c_str());
  }
}
#endif

// ---------------------------------------------------------------- public
void udp_alarm_begin() {
  memset(&s_st, 0, sizeof(s_st));
  s_prefs.begin("alarm2", false);
  s_armed = s_prefs.getBool("safe", true);
}

void udp_alarm_update() {
#if UDP_ALARM_PORT != UDP_PORT
  own_socket_poll();
#endif
  uint32_t now = millis();
  s_st.age_s = s_hasData ? (now - s_lastRxMs) / 1000 : 0;
  if (!s_hasData)                                   s_st.unit = ALARM_UNIT_WAITING;
  else if (now - s_lastRxMs >= UDP_ALARM_OFFLINE_MS) s_st.unit = ALARM_UNIT_OFFLINE;
  else                                              s_st.unit = ALARM_UNIT_ONLINE;

  // Raise the alarm once per detection while the group is switched on. A
  // detection that is still present when the group is switched on again
  // raises it again.
  const bool det[3] = {s_st.fire, s_st.smoke, s_st.gas};
  static const StrId REASON[3] = {S_AL_FIRE_ALARM, S_AL_SMOKE_ALARM, S_AL_GAS_ALARM};
  bool raise = false;
  for (int i = 0; i < 3; i++) {
    if (!det[i] || !s_armed) { s_notified[i] = false; continue; }
    if (!s_notified[i]) { s_notified[i] = true; raise = true; }
  }
  if (raise) {
    // Reason lists every detection that is present now, e.g. "Fire alarm!  Smoke alarm!"
    char reason[64] = "";
    for (int i = 0; i < 3; i++) {
      if (!det[i]) continue;
      if (reason[0]) strlcat(reason, "  ", sizeof(reason));
      strlcat(reason, tr(REASON[i]), sizeof(reason));
    }
    ui_alarm_trigger(reason);
  }
}

const SafetyState& udp_alarm_state() { return s_st; }

bool udp_alarm_safety_armed() { return s_armed; }

void udp_alarm_set_safety_armed(bool on) {
  if (on == s_armed) return;
  s_armed = on;
  s_prefs.putBool("safe", on);
}
