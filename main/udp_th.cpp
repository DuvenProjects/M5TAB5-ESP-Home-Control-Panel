// ============================================================================
//  udp_th.cpp  -  Outdoor unit (UDP) receiver + outdoor value selection
//  Adapted from the stand-alone Tab5 climate receiver: Wi-Fi is now managed
//  by settings.cpp, only the UDP part and the packet parser are kept.
//  No LVGL code.
// ============================================================================
#include "udp_th.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>
#include <string.h>

#include "settings.h"     // settings_wifi_connected()
#include "weather.h"      // OpenWeatherMap current values
#include "udp_alarm.h"    // fire / smoke / gas unit on the same port

OutdoorData g_outdoor;

static WiFiUDP  s_udp;
static bool     s_udpStarted = false;
static uint32_t s_lastRxMs   = 0;

// ---------------------------------------------------------------- Wi-Fi config
void udp_th_wifi_config(const char* ssid) {
  (void)ssid;                                       // same address on every network
#if UDP_USE_STATIC_IP
  WiFi.config(IPAddress(UDP_LOCAL_IP), IPAddress(UDP_GATEWAY),
              IPAddress(UDP_SUBNET), IPAddress(UDP_DNS));     // before WiFi.begin()
#else
  WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));  // DHCP
#endif
}

// ---------------------------------------------------------------- packet parsing
// Numeric value following "key": in a flat JSON string
static bool json_number(const char* json, const char* key, float& out) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p += strlen(pattern);
  while (*p == ' ') p++;
  char* end = nullptr;
  out = strtof(p, &end);
  return end != p;
}

// String value following "key":" in a flat JSON string
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

// Reads every waiting packet (at most a few per call)
static void udp_poll() {
  for (int i = 0; i < 4; i++) {
    int size = s_udp.parsePacket();
    if (size <= 0) return;

    char buf[256];
    int len = s_udp.read(buf, sizeof(buf) - 1);
    if (len <= 0) continue;
    buf[len] = 0;

    g_outdoor.udp_raw_packets++;
    snprintf(g_outdoor.udp_remote, sizeof(g_outdoor.udp_remote), "%s",
             s_udp.remoteIP().toString().c_str());
#if UDP_DEBUG
    Serial.printf("[UDP] from %s: %s\n", g_outdoor.udp_remote, buf);
#endif

#if UDP_ALARM_PORT == UDP_PORT
    if (udp_alarm_packet(buf, g_outdoor.udp_remote)) continue;    // alarm unit (udp_alarm.cpp)
#endif
    char  id[20];
    float t, h, seq = 0;
    if (!json_string(buf, "id", id, sizeof(id)) || strcmp(id, UDP_EXPECTED_ID) != 0) continue; // wrong sender
    if (!json_number(buf, "t", t) || !json_number(buf, "h", h)) continue;                      // bad packet
    if (isnan(t) || isnan(h) || t < -60 || t > 80 || h < 0 || h > 100) continue;               // implausible
    json_number(buf, "seq", seq);

    g_outdoor.udp_temp     = t;
    g_outdoor.udp_hum      = h;
    g_outdoor.udp_seq      = (uint32_t)seq;
    g_outdoor.udp_has_data = true;
    g_outdoor.udp_packets++;
    s_lastRxMs = millis();
  }
}

// ---------------------------------------------------------------- public
void udp_th_begin() {
  memset(&g_outdoor, 0, sizeof(g_outdoor));
  g_outdoor.owm_temp = g_outdoor.owm_hum = NAN;
  g_outdoor.udp_temp = g_outdoor.udp_hum = NAN;
  g_outdoor.temp = g_outdoor.hum = NAN;
  g_outdoor.src = OUTDOOR_SRC_NONE;
}

void udp_th_update() {
  uint32_t now = millis();

  // ---- socket follows Wi-Fi
  bool wifiOk = settings_wifi_connected();
  if (wifiOk && !s_udpStarted) {
    s_udpStarted = s_udp.begin(UDP_PORT) == 1;
    if (s_udpStarted)
      Serial.printf("[UDP] listening on %s:%u\n", WiFi.localIP().toString().c_str(), UDP_PORT);
  } else if (!wifiOk && s_udpStarted) {
    s_udp.stop();                                   // re-opened after the reconnection
    s_udpStarted = false;
  }
  if (s_udpStarted) udp_poll();

  // ---- outdoor unit state
  g_outdoor.udp_age_s = g_outdoor.udp_has_data ? (now - s_lastRxMs) / 1000 : 0;
  g_outdoor.udp_fresh = g_outdoor.udp_has_data && (now - s_lastRxMs < UDP_STALE_MS);

  // ---- OpenWeatherMap current values (copied once per second)
  static uint32_t lastOwm = 0;
  if (now - lastOwm >= 1000 || lastOwm == 0) {
    lastOwm = now;
    WeatherData w;
    if (weather_get(w) && w.valid) {
      g_outdoor.owm_valid = true;
      g_outdoor.owm_temp  = w.temp;
      g_outdoor.owm_hum   = (float)w.humidity;
    }
  }

  // ---- values shown on the Home page / sent to Blynk
  bool useUdp = false, useOwm = false;
#if OUTDOOR_USE_UDP
  useUdp = g_outdoor.udp_fresh;
  #if OUTDOOR_FALLBACK_OWM
  // OpenWeatherMap only after UDP_STALE_MS without a packet (counted from the
  // last packet, or from start-up when none has arrived yet)
  bool udpTimedOut = g_outdoor.udp_has_data ? !g_outdoor.udp_fresh : (now >= UDP_STALE_MS);
  useOwm = !useUdp && udpTimedOut && g_outdoor.owm_valid;
  #endif
#else
  useOwm = g_outdoor.owm_valid;
#endif
  if (useUdp) {
    g_outdoor.temp = g_outdoor.udp_temp;
    g_outdoor.hum  = g_outdoor.udp_hum;
    g_outdoor.src  = OUTDOOR_SRC_UDP;
  } else if (useOwm) {
    g_outdoor.temp = g_outdoor.owm_temp;
    g_outdoor.hum  = g_outdoor.owm_hum;
    g_outdoor.src  = OUTDOOR_SRC_OWM;
  } else {
    g_outdoor.temp = g_outdoor.hum = NAN;
    g_outdoor.src  = OUTDOOR_SRC_NONE;
  }

  // ---- status on Serial every 30 s
  static uint32_t lastLog = 0;
  if (now - lastLog >= 30000) {
    lastLog = now;
    Serial.printf("[UDP] %s raw=%lu valid=%lu last=%s age=%lus\n",
                  s_udpStarted ? "listening" : "closed",
                  (unsigned long)g_outdoor.udp_raw_packets, (unsigned long)g_outdoor.udp_packets,
                  g_outdoor.udp_remote, (unsigned long)g_outdoor.udp_age_s);
  }
}
