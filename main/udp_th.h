// ============================================================================
//  udp_th.h  -  Outdoor unit (UDP) receiver + outdoor value selection
//               (control code only, no LVGL code)
//
//  The outdoor unit sends its temperature / humidity as a UDP packet:
//      {"id":"s2mini","seq":12,"t":23.45,"h":48.20}
//
//  Wi-Fi itself is owned by settings.cpp (single owner). This module only
//  opens the UDP socket while Wi-Fi is connected. When UDP_USE_STATIC_IP is 1,
//  settings.cpp gives the Tab5 the fixed address below on EVERY network (the
//  sender sends to that address). Change it here if your network uses another
//  address range; 0 = DHCP.
//
//  g_outdoor keeps BOTH sources all the time:
//     owm_*  : current values of OpenWeatherMap
//     udp_*  : values of the outdoor unit
//     temp / hum / src : the values shown on the Home page and sent to Blynk,
//                        chosen by OUTDOOR_USE_UDP / OUTDOOR_FALLBACK_OWM.
//  To use other values anywhere in the code just read g_outdoor.
// ============================================================================
#pragma once
#include <Arduino.h>

// ---- Selection of the outdoor values shown on the Home page ----------------
#define OUTDOOR_USE_UDP        1      // 1: outdoor unit (UDP)   0: OpenWeatherMap
#define OUTDOOR_FALLBACK_OWM   1      // 1: show OpenWeatherMap while the outdoor
                                      //    unit sends nothing (stale)

// ---- UDP -------------------------------------------------------------------
#define UDP_PORT               4210        // must match the sender
#define UDP_EXPECTED_ID        "s2mini"    // only packets with this "id" are accepted
#define UDP_STALE_MS           60000UL     // no packet for this long -> not fresh: the Home page
                                           // switches to OpenWeatherMap (also 1 min after start-up
                                           // when no packet has arrived yet); a new packet switches
                                           // back to the outdoor unit at once
#define UDP_DEBUG              0           // 1: print every packet on Serial

// ---- Static IP of the Tab5 (the sender sends to this address) --------------
// Choose an address outside the router's DHCP pool (or reserve it in the router).
#define UDP_USE_STATIC_IP      1           // 1: fixed address below on every network, 0: DHCP
#define UDP_LOCAL_IP           192, 168, 1, 50
#define UDP_GATEWAY            192, 168, 1, 1
#define UDP_SUBNET             255, 255, 255, 0
#define UDP_DNS                192, 168, 1, 1

enum OutdoorSource : uint8_t {
  OUTDOOR_SRC_NONE = 0,       // no value yet
  OUTDOOR_SRC_UDP,            // outdoor unit
  OUTDOOR_SRC_OWM             // OpenWeatherMap
};

struct OutdoorData {
  // OpenWeatherMap current values (kept up to date by udp_th_update())
  bool     owm_valid;
  float    owm_temp;          // Celsius
  float    owm_hum;           // %

  // Outdoor unit (UDP)
  bool     udp_has_data;      // at least one valid packet received
  bool     udp_fresh;         // last packet younger than UDP_STALE_MS
  float    udp_temp;          // Celsius
  float    udp_hum;           // %
  uint32_t udp_seq;           // sequence number of the last packet
  uint32_t udp_age_s;         // seconds since the last packet
  uint32_t udp_packets;       // valid packets
  uint32_t udp_raw_packets;   // every packet (also rejected ones)
  char     udp_remote[16];    // IP of the last sender

  // Values shown on the Home page / sent to Blynk (NAN = not available)
  float         temp;         // Celsius
  float         hum;          // %
  OutdoorSource src;
};

extern OutdoorData g_outdoor;     // main / LVGL thread only

// Called by ui.init() once
void udp_th_begin();

// Called every 100 ms by ui.cpp (main thread): opens / closes the socket with
// Wi-Fi, reads the packets and refreshes g_outdoor
void udp_th_update();

// Called by settings.cpp right before every WiFi.begin(): static IP
// (UDP_USE_STATIC_IP 1) or DHCP (0)
void udp_th_wifi_config(const char* ssid);
