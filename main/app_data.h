// ============================================================================
//  app_data.h  -  Global data structure (30 parameters) shared by all pages
//  The TYPE is defined here, the INSTANCE (g_data) lives in main.ino.
//  It is stored in NVS (Preferences): ui.cpp -> data_load() / data_save()
// ============================================================================
#pragma once
#include <Arduino.h>

#define APP_DATA_MAGIC   0x54414235UL   // "TAB5"
#define APP_DATA_VERSION 1              // increase when the layout or defaults change

// ---------------------------------------------------------------- DEFAULTS
//  Written to g_data on the first start (or when APP_DATA_VERSION changes).
//  API key, city and coordinates are re-applied on every start.
#define DEF_WIFI_SSID    "YOUR_WIFI_SSID"
#define DEF_WIFI_PASS    "YOUR_WIFI_PASSWORD"
#define DEF_OWM_API_KEY  "OPENWEATHERMAP_API_KEY"
#define DEF_CITY         "Canakkale"
#define DEF_LATITUDE     40.1553f
#define DEF_LONGITUDE    26.4142f
#define DEF_TZ_OFFSET    (3 * 3600)
#define DEF_ALARM_PIN    "1234"

#define PLACEHOLDER_WIFI_SSID "YOUR_WIFI_SSID"   // ignored as a default network

// ---------------------------------------------------------------- PINS
//  Free pins of the Tab5 GPIO_EXT / M5-Bus connector are used.
//  (G2, G3, G4, G16, G35, G47, G48, G51)
//  HVAC pins are defined in hvac.h: G17, G45, G52, G5, G18, G19
//  AHT10 + ENS160 I2C:  SDA G7 / SCL G6
#define PIN_PIR          16   // motion sensor (HC-SR501 / AM312 OUT)

#define PIN_RGB_R        2    // RGB LED red   (MOSFET gate / LED driver)
#define PIN_RGB_G        48   // RGB LED green
#define PIN_RGB_B        47   // RGB LED blue
#define RGB_COMMON_ANODE 0    // set to 1 for a common anode strip / LED

#define PIN_BLIND1_UP    3    // blind 1 motor relay / H-bridge  OPEN
#define PIN_BLIND1_DOWN  4    // blind 1 motor relay / H-bridge  CLOSE
#define PIN_BLIND2_UP    35   // blind 2 motor OPEN
#define PIN_BLIND2_DOWN  51   // blind 2 motor CLOSE
#define RELAY_ACTIVE_LOW 0    // set to 1 if the relay board switches on LOW
#define BLIND_COUNT      2

// ---------------------------------------------------------------- GLOBAL STRUCT
struct AppData {
  uint32_t magic;               // (structure check - not a parameter)
  uint16_t version;             // (structure check - not a parameter)

  // --- network / services -------------------------------------- PERSISTENT
  char     wifi_ssid[33];       //  1
  char     wifi_pass[65];       //  2
  char     owm_api_key[48];     //  3  OpenWeatherMap API key
  char     city[32];            //  4  displayed city name
  float    latitude;            //  5
  float    longitude;           //  6
  int32_t  tz_offset;           //  7  UTC offset (seconds), unused: Settings owns the time zone
  uint8_t  brightness;          //  8  screen brightness 0..255   (copy, owned by settings.cpp)
  uint16_t screen_timeout;      //  9  seconds, 0 = never          (copy, owned by settings.cpp)

  // --- climate / HVAC ----------------------------------------------------
  //  (HVAC settings are stored by hvac.cpp in its own "hvac" NVS namespace;
  //   these fields are copies written by hvac.cpp for the Home page)
  float    indoor_temp;         // 10  AHT10 temperature (NAN on error)
  float    indoor_hum;          // 11  AHT10 humidity
  float    hvac_setpoint;       // 12  active heating / cooling target
  uint8_t  hvac_mode;           // 13  0=Auto 1=Heat 2=Cool 3=Dry 4=Fan

  // --- outdoor weather (filled by weather.cpp) ----------------------------
  float    outdoor_temp;        // 14
  char     weather_desc[32];    // 15  description in the active language
  char     weather_icon[8];     // 16  OWM icon code ("01d", "10n" ...)

  // --- blinds ------------------------------------------------- PERSISTENT
  uint8_t  blind_pos[BLIND_COUNT];    // 17  0 = closed .. 100 = open (real position)
  uint8_t  blind_target[BLIND_COUNT]; // 18  target position
  uint16_t blind_travel_s;      // 19  full travel time (seconds)

  // --- RGB LED ------------------------------------------------ PERSISTENT
  bool     rgb_on;              // 20
  uint8_t  rgb_r;               // 21
  uint8_t  rgb_g;               // 22
  uint8_t  rgb_b;               // 23
  uint8_t  rgb_brightness;      // 24  0..100 %
  uint8_t  rgb_effect;          // 25  0=static 1=breathe 2=rainbow 3=candle

  // --- scene / alarm / state ---------------------------------------------
  uint8_t  scene_id;            // 26  active scene               PERSISTENT
  bool     alarm_armed;         // 27  alarm armed                PERSISTENT
  char     alarm_pin[8];        // 28  alarm PIN                  PERSISTENT
  bool     alarm_active;        // 29  alarm ringing   (runtime)
  bool     motion;              // 30  motion detected (runtime)

  int8_t   wifi_rssi;           //  (runtime - used by the Wi-Fi icon, 0 = not connected)
};

extern AppData g_data;          // defined in main.ino

// ui.cpp
void data_defaults();
void data_load();
void data_save();
void data_mark_dirty();          // call after a change: saved 3 s later
