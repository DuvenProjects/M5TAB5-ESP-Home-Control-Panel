/*
 * settings.cpp
 * Settings application for M5Stack Tab5 (ESP32-P4) using LVGL 9.
 *
 * Layout (1280 x 720, landscape):
 *   - Header: common top bar (menu.cpp): icon, title, clock, Wi-Fi, X
 *   - Left side menu: Display / WiFi / Localization / System
 *   - Right side: content cards of the selected menu
 *
 * Display       : brightness, sleep timeout (slider), rotation (0 / 180 deg),
 *                 temperature unit (Celsius / Fahrenheit), motion sensor (PIR) on / off
 * WiFi          : on/off, scan (signal level + encryption), connect with
 *                 on-screen QWERTY keyboard, manual "add network", saved credentials
 * Localization  : language (Turkish, English, German, French, Spanish,
 *                 Chinese - every text of the application follows it),
 *                 time zone, time format, date format
 * System        : speaker volume, microphone on/off, software version, restart
 *
 * All settings are stored in NVS (Preferences) and restored at start-up.
 *
 * Architecture:
 *   main.ino     : M5Tab5 LVGL display / touch driver only, then ui.init()
 *   ui.cpp       : ui.init() calls settings(); ui.show(PAGE_SETTINGS) ->
 *                  settings_create() (persistent screen). Wi-Fi, screen sleep,
 *                  brightness, rotation, time zone and the language are owned
 *                  by this file only.
 *   settings.cpp : ALL LVGL screen code. settings() builds the UI once; all
 *                  periodic work (Wi-Fi, clock, screen sleep, toast) runs from
 *                  LVGL timers, so no loop function is needed.
 */

#include "settings.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <lvgl.h>
#include "app_data.h"     // g_data (Wi-Fi credentials, brightness copy)
#include "menu.h"         // common top bar (clock + Wi-Fi icon + X)
#include "lang.h"         // tr(), lang_set(), FONT_xx
#include "theme.h"        // ui_col(): dark / light theme colours
#include "udp_th.h"       // udp_th_wifi_config(): static IP for the outdoor unit
#include "sysinfo.h"      // CPU load, monthly HVAC run time (Info window)
#include <esp_wifi.h>     // esp_wifi_sta_get_ap_info(): Wi-Fi standard
#include <esp_timer.h>    // esp_timer_get_time(): uptime
#include <esp_chip_info.h> // chip revision

/* ------------------------------------------------------------------ */
/* Icons                                                               */
/* ------------------------------------------------------------------ */

#if SETTINGS_USE_ICON_FONT
extern "C" {
LV_FONT_DECLARE(font_settings_icons);
}
#define ICON_FONT      (&font_settings_icons)
#define ICON_GEAR      "\xEE\x80\x80"   // U+E000
#define ICON_MONITOR   "\xEE\x80\x81"   // U+E001
#define ICON_TRANSLATE "\xEE\x80\x82"   // U+E002
#define ICON_CHIP      "\xEE\x80\x83"   // U+E003
#define ICON_SUN       "\xEE\x80\x84"   // U+E004
#define ICON_MOON      "\xEE\x80\x85"   // U+E005
#define ICON_ROTATE    "\xEE\x80\x86"   // U+E006
#define ICON_SPEAKER   "\xEE\x80\x87"   // U+E007
#define ICON_MIC       "\xEE\x80\x88"   // U+E008
#define ICON_GLOBE     "\xEE\x80\x89"   // U+E009
#define ICON_CLOCK     "\xEE\x80\x8A"   // U+E00A
#define ICON_CALENDAR  "\xEE\x80\x8B"   // U+E00B
// Wi-Fi glyphs: [0] = open, [1] = secured; index = signal level 1..4 minus 1
static const char *const WIFI_GLYPH[2][4] = {
  {"\xEE\x80\x90", "\xEE\x80\x91", "\xEE\x80\x92", "\xEE\x80\x93"},
  {"\xEE\x80\x94", "\xEE\x80\x95", "\xEE\x80\x96", "\xEE\x80\x97"}
};
#else
#define ICON_FONT      (&lv_font_montserrat_28)
#define ICON_GEAR      LV_SYMBOL_SETTINGS
#define ICON_MONITOR   LV_SYMBOL_IMAGE
#define ICON_TRANSLATE LV_SYMBOL_KEYBOARD
#define ICON_CHIP      LV_SYMBOL_DRIVE
#define ICON_SUN       LV_SYMBOL_EYE_OPEN
#define ICON_MOON      LV_SYMBOL_POWER
#define ICON_ROTATE    LV_SYMBOL_LOOP
#define ICON_SPEAKER   LV_SYMBOL_VOLUME_MAX
#define ICON_MIC       LV_SYMBOL_AUDIO
#define ICON_GLOBE     LV_SYMBOL_GPS
#define ICON_CLOCK     LV_SYMBOL_BELL
#define ICON_CALENDAR  LV_SYMBOL_LIST
static const char *const WIFI_GLYPH[2][4] = {
  {LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI},
  {LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI}
};
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

// Screen geometry (landscape)
static const int SCR_W      = 1280;
static const int SCR_H      = 720;
static const int NAV_X      = 16;
static const int NAV_Y      = 116;
static const int NAV_W      = 344;
static const int NAV_H      = 136;
static const int NAV_GAP    = 12;
static const int PAGE_X     = 376;
static const int PAGE_Y     = 116;
static const int PAGE_W     = 888;
static const int PAGE_H     = 588;

// Colors
#define COL_BG          0x000000
#define COL_CARD        0x1C1C1C
#define COL_CARD_PRESS  0x2A2A2A
#define COL_SEP         0x2E2E2E
#define COL_TEXT        0xFFFFFF
#define COL_MUTED       0x9E9E9E
#define COL_ICON        0xB0B0B0
#define COL_ACCENT      0xFFB300
#define COL_ACCENT_DIM  0x5E4300
#define COL_PILL        0x2B2B2B
#define COL_OK          0x3DDC63
#define COL_ERR         0xFF5252

// Setting limits and option tables
static const uint8_t  BRIGHT_MIN = 5;
static const uint32_t SLEEP_SECONDS[8] = {0, 1800, 900, 300, 60, 30, 15, 5};
static const StrId TIME_FORMATS[2] = {S_TIMEFMT_24, S_TIMEFMT_12};
static const StrId DATE_FORMATS[3] = {S_DATEFMT_DMY, S_DATEFMT_MDY, S_DATEFMT_ISO};

// Translated label of a sleep option ("Never", "30 min", "15 sec" ...)
static const char *sleepLabel(uint8_t idx) {
  static char b[24];
  uint32_t secs = SLEEP_SECONDS[idx % 8];
  if (secs == 0)        return tr(S_NEVER);
  if (secs >= 60)       snprintf(b, sizeof(b), tr(S_FMT_MIN), (int)(secs / 60));
  else                  snprintf(b, sizeof(b), tr(S_FMT_SEC), (int)secs);
  return b;
}

struct TzEntry { const char *label; const char *posix; };
static const TzEntry TIMEZONES[] = {
  {"UTC-12:00  Baker Island",        "<-12>12"},
  {"UTC-11:00  Pago Pago",           "<-11>11"},
  {"UTC-10:00  Honolulu",            "HST10"},
  {"UTC-09:00  Anchorage",           "AKST9AKDT,M3.2.0,M11.1.0"},
  {"UTC-08:00  Los Angeles",         "PST8PDT,M3.2.0,M11.1.0"},
  {"UTC-07:00  Denver",              "MST7MDT,M3.2.0,M11.1.0"},
  {"UTC-06:00  Chicago",             "CST6CDT,M3.2.0,M11.1.0"},
  {"UTC-05:00  New York",            "EST5EDT,M3.2.0,M11.1.0"},
  {"UTC-04:00  Halifax",             "AST4ADT,M3.2.0,M11.1.0"},
  {"UTC-03:00  Sao Paulo",           "<-03>3"},
  {"UTC-02:00  South Georgia",       "<-02>2"},
  {"UTC-01:00  Azores",              "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
  {"UTC+00:00  London",              "GMT0BST,M3.5.0/1,M10.5.0"},
  {"UTC+01:00  Berlin, Paris",       "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"UTC+02:00  Athens, Cairo",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"UTC+03:00  Istanbul, Moscow",    "<+03>-3"},
  {"UTC+03:30  Tehran",              "<+0330>-3:30"},
  {"UTC+04:00  Dubai",               "<+04>-4"},
  {"UTC+05:00  Tashkent",            "<+05>-5"},
  {"UTC+05:30  New Delhi",           "IST-5:30"},
  {"UTC+06:00  Dhaka",               "<+06>-6"},
  {"UTC+07:00  Bangkok",             "<+07>-7"},
  {"UTC+08:00  Beijing, Singapore",  "CST-8"},
  {"UTC+09:00  Tokyo, Seoul",        "JST-9"},
  {"UTC+10:00  Sydney",              "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"UTC+12:00  Auckland",            "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
static const int TZ_COUNT = sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);
static const uint8_t TZ_DEFAULT = 15;    // UTC+03:00 Istanbul

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t SCAN_TIMEOUT_MS    = 15000;
static const uint32_t WIFI_POLL_MS       = 500;
static const uint32_t TOAST_MS           = 2500;
static const int      MAX_NETS           = 24;

/* ------------------------------------------------------------------ */
/* Types and state                                                     */
/* ------------------------------------------------------------------ */

enum PageId : uint8_t { PAGE_DISPLAY = 0, PAGE_WIFI, PAGE_LOCALE, PAGE_SYSTEM, PAGE_COUNT };
enum PickerId : uint8_t { PK_LANG = 0, PK_TZ, PK_TIMEFMT, PK_DATEFMT };
enum SegId : intptr_t { SEG_ROT_0 = 1, SEG_ROT_180, SEG_UNIT_C, SEG_UNIT_F, SEG_THEME_DARK, SEG_THEME_LIGHT };

struct Config {
  uint8_t brightness;   // 5..100 %
  uint8_t sleepIdx;     // index into SLEEP_SECONDS
  uint8_t rotation;     // 0 = 0 deg, 1 = 180 deg
  uint8_t volume;       // 0..100 %
  bool    micOn;
  bool    clickOn;      // click sound on every touch
  bool    pirOn;        // motion sensor (PIR) enabled
  bool    blynkOn;      // Blynk IoT connection enabled
  bool    fahrenheit;   // temperature unit: false = Celsius, true = Fahrenheit
  bool    lightTheme;   // colour theme: false = dark, true = light
  bool    wifiOn;
  uint8_t lang;         // Language (lang.h): TR, EN, DE, FR, ES, ZH
  uint8_t tz;           // index into TIMEZONES
  bool    time24;
  uint8_t dateFmt;      // index into DATE_FORMATS
};

struct NetInfo {
  char    ssid[33];
  int8_t  rssi;
  uint8_t enc;
};

static bool        g_initialized = false;
static Config      g_cfg;
static Preferences g_prefs;
static void      (*g_homeCb)() = nullptr;     // optional: real home page registered by the application
static void      (*g_langCb)() = nullptr;     // optional: called after the language was changed
static bool        g_langPending = false;    // language changed: rebuild the screen in the timer
static lv_obj_t   *g_homeScr = nullptr;        // built-in placeholder home page (created on demand)

// Display / power
static bool          g_uiReady = false;      // true once the LVGL UI exists
static bool          g_screenOff = false;    // backlight off (sleep mode)
static uint32_t      g_lastActivityMs = 0;
static lv_obj_t     *g_sleepShield = nullptr;  // full screen touch absorber while asleep

// Wi-Fi
static bool     g_wifiPinsSet   = false;
static bool     g_connecting    = false;
static bool     g_connFailed    = false;
static uint32_t g_connectStartMs = 0;
static char     g_pendingSsid[33] = "";
static char     g_pendingPass[65] = "";
static bool     g_scanning      = false;
static bool     g_hasScanned    = false;
static uint32_t g_scanStartMs   = 0;
static uint32_t g_lastWifiPollMs = 0;
static int      g_lastWifiSig   = -1;
static NetInfo  g_nets[MAX_NETS];
static int      g_netCount      = 0;

// Time
static char     g_lastClockText[48] = "";
static uint32_t g_lastClockMs   = 0;

// UI objects
static lv_obj_t *g_scr = nullptr;
static lv_obj_t *g_pages[PAGE_COUNT];
static struct { lv_obj_t *card, *circle, *icon, *title, *sub; } g_nav[PAGE_COUNT];
static int       g_curPage = PAGE_DISPLAY;

static lv_obj_t *g_slBright, *g_lblBright, *g_slSleep, *g_lblSleep;
static lv_obj_t *g_segRot0, *g_segRot180, *g_segUnitC, *g_segUnitF;
static lv_obj_t *g_segThemeDark, *g_segThemeLight;
static lv_obj_t *g_slVolume, *g_lblVolume, *g_swMic, *g_swClick;
static lv_obj_t *g_swPir, *g_swBlynk, *g_btnRestart;
static lv_timer_t *g_restartTimer = nullptr;   // reverts the confirm state of the restart button
static lv_obj_t *g_swWifi, *g_lblWifiSub, *g_lblConnIcon, *g_lblConnName, *g_lblConnSub;
static lv_obj_t *g_btnDisconnect, *g_btnScan, *g_spinner, *g_netList;
static lv_obj_t *g_lblLang, *g_lblTz, *g_lblTimeFmt, *g_lblDateFmt;

// Overlays
static lv_obj_t *g_toast = nullptr, *g_toastLabel = nullptr;
static uint32_t  g_toastUntil = 0;
static lv_obj_t *g_picker = nullptr;
static PickerId  g_pickerId = PK_LANG;
static lv_obj_t *g_dlg = nullptr, *g_dlgSsid = nullptr, *g_dlgPass = nullptr, *g_dlgKb = nullptr;

// Wi-Fi status that is safe to call before the Wi-Fi driver was started
static wl_status_t wifiStatus() {
  return g_wifiPinsSet ? WiFi.status() : WL_NO_SHIELD;
}

// Forward declarations
static void showToast(const char *msg);
static void updateNav();
static void wifiUiRefresh();
static void rebuildNetworkList();
static void applyTimezone();
static void openConnectDialog(const char *ssid, bool manual, const char *encText);
static void openPicker(PickerId id);

/* ------------------------------------------------------------------ */
/* Settings storage                                                    */
/* ------------------------------------------------------------------ */

static uint8_t clampU8(int v, int lo, int hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (uint8_t)v;
}

static void loadConfig() {
  g_prefs.begin("settings", false);
  g_cfg.brightness = clampU8(g_prefs.getUChar("bright", 80), BRIGHT_MIN, 100);
  g_cfg.sleepIdx   = clampU8(g_prefs.getUChar("sleep", 3), 0, 7);          // default 5 min
  g_cfg.rotation   = clampU8(g_prefs.getUChar("rot", 0), 0, 1);
  g_cfg.volume     = clampU8(g_prefs.getUChar("vol", 60), 0, 100);
  g_cfg.micOn      = g_prefs.getBool("mic", false);
  g_cfg.clickOn    = g_prefs.getBool("click", true);
  g_cfg.pirOn      = g_prefs.getBool("pir", true);
  g_cfg.blynkOn    = g_prefs.getBool("blynk", true);
  g_cfg.fahrenheit = g_prefs.getBool("tunitf", false);
  g_cfg.lightTheme = g_prefs.getBool("light", false);
  g_cfg.wifiOn     = g_prefs.getBool("wifi", true);
  g_cfg.lang       = clampU8(g_prefs.getUChar("lang", LANG_DEFAULT), 0, LANG_COUNT - 1);
  g_cfg.tz         = clampU8(g_prefs.getUChar("tz", TZ_DEFAULT), 0, TZ_COUNT - 1);
  g_cfg.time24     = g_prefs.getBool("t24", true);
  g_cfg.dateFmt    = clampU8(g_prefs.getUChar("datef", 0), 0, 2);
  lang_set(g_cfg.lang);                 // every screen is built in this language
}

/* ------------------------------------------------------------------ */
/* Hardware apply functions                                            */
/* ------------------------------------------------------------------ */

static void applyBrightness() {
  g_data.brightness     = (uint8_t)((g_cfg.brightness * 255) / 100);   // copy (Home etc.)
  g_data.screen_timeout = (uint16_t)SLEEP_SECONDS[g_cfg.sleepIdx];
  if (!g_screenOff) M5.Display.setBrightness(g_data.brightness);
}

static void applyRotation() {
  M5.Display.setRotation(g_cfg.rotation ? 3 : 1);   // 1 = landscape (0 deg), 3 = 180 deg
  if (g_uiReady) {                                  // LVGL is running: redraw everything
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
  }
}

static void applyVolume() {
  M5.Speaker.setVolume((uint8_t)((g_cfg.volume * 255) / 100));
}

static void applyMic() {
  if (g_cfg.micOn) {
    if (!M5.Mic.isRunning()) M5.Mic.begin();
  } else {
    if (M5.Mic.isRunning()) M5.Mic.end();
  }
}

// Called for every touch activity reported by the input device
static void onIndevActivity(lv_event_t *e) {
  (void)e;
  g_lastActivityMs = millis();
}

static void wakeScreen() {
  g_screenOff = false;
  applyBrightness();
  g_lastActivityMs = millis();
}

// The shield absorbs the touch that wakes the screen, so it cannot click a widget below.
static void onSleepShield(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    if (g_screenOff) wakeScreen();
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!g_screenOff) lv_obj_add_flag(g_sleepShield, LV_OBJ_FLAG_HIDDEN);
  }
}

static void sleepTask(uint32_t now) {
  if (g_screenOff) return;
  uint32_t secs = SLEEP_SECONDS[g_cfg.sleepIdx];
  if (secs == 0) return;                                   // "Never"

  // Touch activity: indev events plus a state poll as a fallback
  lv_indev_t *in = lv_indev_get_next(nullptr);
  if (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) g_lastActivityMs = now;

  uint32_t idle = now - g_lastActivityMs;
  uint32_t lvIdle = lv_display_get_inactive_time(lv_display_get_default());
  if (lvIdle < idle) idle = lvIdle;

  if (idle > secs * 1000UL) {
    g_screenOff = true;
    M5.Display.setBrightness(0);                           // backlight off
    lv_obj_remove_flag(g_sleepShield, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_sleepShield);
  }
}

/* ------------------------------------------------------------------ */
/* UI helper functions                                                 */
/* ------------------------------------------------------------------ */

static void setText(lv_obj_t *lbl, const char *txt) {
  if (lbl && strcmp(lv_label_get_text(lbl), txt) != 0) lv_label_set_text(lbl, txt);
}

// Transparent, non-scrolling, non-clickable container
static lv_obj_t *newBox(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static void flexRow(lv_obj_t *o, lv_flex_align_t main = LV_FLEX_ALIGN_START) {
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(o, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static void flexColumn(lv_obj_t *o) {
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

static lv_obj_t *newLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, ui_col(color), 0);
  return l;
}

static lv_obj_t *newIcon(lv_obj_t *parent, const char *glyph, uint32_t color) {
  return newLabel(parent, glyph, ICON_FONT, color);
}

// Content page card (rounded, dark)
static lv_obj_t *newCard(lv_obj_t *parent) {
  lv_obj_t *c = newBox(parent);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(c, ui_col(COL_CARD), 0);
  lv_obj_set_style_radius(c, 28, 0);
  lv_obj_set_style_pad_hor(c, 28, 0);
  lv_obj_set_style_pad_ver(c, 8, 0);
  flexColumn(c);
  return c;
}

static lv_obj_t *newSectionLabel(lv_obj_t *card, const char *text) {
  lv_obj_t *l = newLabel(card, text, FONT_20, COL_MUTED);
  lv_obj_set_style_pad_top(l, 14, 0);
  lv_obj_set_style_pad_bottom(l, 2, 0);
  return l;
}

// Remove the separator of the last row of a card
static void finishCard(lv_obj_t *card) {
  lv_obj_t *last = lv_obj_get_child(card, -1);
  if (last) lv_obj_set_style_border_width(last, 0, 0);
}

// Settings row: [icon] [title / optional subtitle (grows)] [controls added by caller]
static lv_obj_t *newRow(lv_obj_t *card, const char *icon, const char *title, const char *sub,
                        lv_obj_t **subOut = nullptr) {
  lv_obj_t *row = newBox(card);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(row, 88, 0);
  lv_obj_set_style_pad_ver(row, 8, 0);
  lv_obj_set_style_pad_column(row, 20, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, ui_col(COL_SEP), 0);
  flexRow(row);

  if (icon) newIcon(row, icon, COL_ICON);

  lv_obj_t *col = newBox(row);
  flexColumn(col);
  lv_obj_set_flex_grow(col, 1);
  newLabel(col, title, FONT_24, COL_TEXT);
  if (sub) {
    lv_obj_t *s = newLabel(col, sub, FONT_20, COL_MUTED);
    if (subOut) *subOut = s;
  }
  return row;
}

static lv_obj_t *newValueLabel(lv_obj_t *row, const char *text, int width) {
  lv_obj_t *l = newLabel(row, text, FONT_24, COL_TEXT);
  lv_obj_set_width(l, width);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
  return l;
}

// Amber bar slider; the callback gets VALUE_CHANGED and RELEASED events
static lv_obj_t *newSlider(lv_obj_t *row, int min, int max, int val, lv_event_cb_t cb) {
  lv_obj_t *s = lv_slider_create(row);
  lv_obj_set_size(s, 400, 48);
  lv_slider_set_range(s, min, max);
  lv_slider_set_value(s, val, LV_ANIM_OFF);
  lv_obj_set_style_radius(s, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, ui_col(COL_ACCENT_DIM), LV_PART_MAIN);
  lv_obj_set_style_radius(s, 24, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, ui_col(COL_ACCENT), LV_PART_INDICATOR);
  // Knob = small dark vertical tick (48x48 knob area shrunk by negative padding to 6x26)
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_bg_color(s, ui_col(0x3A2A00), LV_PART_KNOB);
  lv_obj_set_style_radius(s, 3, LV_PART_KNOB);
  lv_obj_set_style_border_width(s, 0, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
  lv_obj_set_style_pad_hor(s, -21, LV_PART_KNOB);
  lv_obj_set_style_pad_ver(s, -11, LV_PART_KNOB);
  lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, nullptr);
  return s;
}

static lv_obj_t *newSwitch(lv_obj_t *row, bool on, lv_event_cb_t cb) {
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 96, 52);
  lv_obj_set_style_bg_color(sw, ui_col(COL_PILL), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, ui_col(COL_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return sw;
}

// Pill shaped button with text
static lv_obj_t *newPill(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud,
                         uint32_t bg = COL_PILL, uint32_t fg = COL_TEXT) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, LV_SIZE_CONTENT, 52);
  lv_obj_set_style_radius(b, 26, 0);
  lv_obj_set_style_pad_hor(b, 26, 0);
  lv_obj_set_style_bg_color(b, ui_col(bg), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, FONT_24, 0);
  lv_obj_set_style_text_color(l, ui_col(fg), 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

// Segmented control (two options)
static void styleSegment(lv_obj_t *btn, bool selected) {
  if (!btn) return;                               // not created yet (page still being built)
  lv_obj_set_style_bg_opa(btn, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(btn, ui_col(0xF2F2F2), 0);
  lv_obj_t *l = lv_obj_get_child(btn, 0);
  if (l) lv_obj_set_style_text_color(l, ui_col(selected ? 0x202020 : 0xDDDDDD), 0);
}

static lv_obj_t *newSegButton(lv_obj_t *parent, const char *text, int w, SegId id, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, 44);
  lv_obj_set_style_radius(b, 22, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, FONT_24, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
  return b;
}

/* ------------------------------------------------------------------ */
/* Toast                                                               */
/* ------------------------------------------------------------------ */

static void createToast() {
  g_toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g_toast, ui_col(0x383838), 0);
  lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_toast, 30, 0);
  lv_obj_set_style_border_width(g_toast, 0, 0);
  lv_obj_set_style_pad_hor(g_toast, 30, 0);
  lv_obj_set_style_pad_ver(g_toast, 14, 0);
  lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_CLICKABLE);
  g_toastLabel = newLabel(g_toast, "", FONT_24, COL_TEXT);
  lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
}

static void showToast(const char *msg) {
  lv_label_set_text(g_toastLabel, msg);
  lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_toast);
  g_toastUntil = millis() + TOAST_MS;
}

static void toastTask(uint32_t now) {
  if (g_toast && !lv_obj_has_flag(g_toast, LV_OBJ_FLAG_HIDDEN) && (int32_t)(now - g_toastUntil) >= 0) {
    lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  }
}

/* ------------------------------------------------------------------ */
/* Time zone / clock                                                   */
/* ------------------------------------------------------------------ */

static void applyTimezone() {
  const char *tz = TIMEZONES[g_cfg.tz].posix;
  if (wifiStatus() == WL_CONNECTED) {
    configTzTime(tz, "pool.ntp.org", "time.google.com");   // sets TZ and (re)starts SNTP
  } else {
    setenv("TZ", tz, 1);
    tzset();
  }
}

// Formats the current time according to the selected time format.
// Returns false if the clock has not been synchronized yet.
static bool formatTime(char *out, size_t n, bool withSeconds) {
  time_t t = time(nullptr);
  if (t < 1700000000) {
    snprintf(out, n, "--:--");
    return false;
  }
  struct tm tmv;
  localtime_r(&t, &tmv);
  if (g_cfg.time24) {
    strftime(out, n, withSeconds ? "%H:%M:%S" : "%H:%M", &tmv);
  } else {
    char hm[16];
    strftime(hm, sizeof(hm), withSeconds ? "%I:%M:%S" : "%I:%M", &tmv);
    snprintf(out, n, "%s %s", hm, tr(tmv.tm_hour < 12 ? S_AM : S_PM));
  }
  return true;
}

static void clockTask(uint32_t now) {
  if (now - g_lastClockMs < 1000) return;
  g_lastClockMs = now;
  char buf[48];
  formatTime(buf, sizeof(buf), false);
  if (strcmp(buf, g_lastClockText) != 0) {
    strcpy(g_lastClockText, buf);
    updateNav();
  }
}

/* ------------------------------------------------------------------ */
/* Side menu                                                           */
/* ------------------------------------------------------------------ */

struct NavDef { StrId title; uint32_t circle; uint32_t tint; uint32_t iconColor; };
static const NavDef NAV_DEFS[PAGE_COUNT] = {
  {S_NAV_DISPLAY,  0x5C4108, 0x4A3A12, 0xFFA000},
  {S_NAV_WIFI,     0x1B4466, 0x1F3447, 0x42A5F5},
  {S_NAV_LOCALE,   0x4A2457, 0x3A2442, 0xAB47BC},
  {S_NAV_SYSTEM,   0x1A4A44, 0x1B3B37, 0x26A69A},
};

static void updateNav() {
  char buf[64];

  snprintf(buf, sizeof(buf), tr(S_FMT_NAV_DISPLAY), (unsigned)g_cfg.brightness,
           g_cfg.sleepIdx == 0 ? tr(S_SLEEP_OFF) : sleepLabel(g_cfg.sleepIdx));
  setText(g_nav[PAGE_DISPLAY].sub, buf);

  if (!g_cfg.wifiOn)                          setText(g_nav[PAGE_WIFI].sub, tr(S_OFF));
  else if (g_connecting)                      setText(g_nav[PAGE_WIFI].sub, tr(S_CONNECTING));
  else if (wifiStatus() == WL_CONNECTED) {
    String s = WiFi.SSID();
    setText(g_nav[PAGE_WIFI].sub, s.c_str());
  } else                                      setText(g_nav[PAGE_WIFI].sub, tr(S_NOT_CONNECTED));

  char tbuf[32];
  formatTime(tbuf, sizeof(tbuf), false);
  snprintf(buf, sizeof(buf), "%s, %s", lang_name(g_cfg.lang), tbuf);
  setText(g_nav[PAGE_LOCALE].sub, buf);

  snprintf(buf, sizeof(buf), "v%s", SETTINGS_VERSION);
  setText(g_nav[PAGE_SYSTEM].sub, buf);
}

static void selectPage(int p) {
  g_curPage = p;
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == p) lv_obj_remove_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(g_nav[i].card, ui_col(i == p ? NAV_DEFS[i].tint : COL_CARD), 0);
  }
  lv_obj_scroll_to_y(g_pages[p], 0, LV_ANIM_OFF);
}

static void onNavClicked(lv_event_t *e) {
  selectPage((int)(intptr_t)lv_event_get_user_data(e));
}

// Close (X) button: common top bar (menu.cpp) -> ui.show(PAGE_HOME)

/* ------------------------------------------------------------------ */
/* Display page                                                        */
/* ------------------------------------------------------------------ */

static void updateSegments() {
  styleSegment(g_segRot0,   g_cfg.rotation == 0);
  styleSegment(g_segRot180, g_cfg.rotation == 1);
  styleSegment(g_segUnitC,  !g_cfg.fahrenheit);
  styleSegment(g_segUnitF,  g_cfg.fahrenheit);
  styleSegment(g_segThemeDark,  !g_cfg.lightTheme);
  styleSegment(g_segThemeLight, g_cfg.lightTheme);
}

// LVGL default theme (switches, sliders ...) follows the colour theme
static void applyLvTheme() {
  lv_display_t *disp = lv_display_get_default();
  lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_AMBER),
                                         lv_palette_main(LV_PALETTE_BLUE), !g_cfg.lightTheme,
                                         LV_FONT_DEFAULT);
  lv_display_set_theme(disp, th);
}

// Dark / light theme: every screen is rebuilt with the new colours (in the
// settings timer, like a language change; the application rebuilds its own
// persistent screens in the callback, the other pages on their next opening)
static void onTheme(lv_event_t *e) {
  bool light = ((SegId)(intptr_t)lv_event_get_user_data(e) == SEG_THEME_LIGHT);
  if (light == g_cfg.lightTheme) return;
  g_cfg.lightTheme = light;
  g_prefs.putBool("light", light);
  applyLvTheme();
  g_langPending = true;
}

// Temperature unit (every page converts its Celsius values with settings_temp())
static void onTempUnit(lv_event_t *e) {
  bool f = ((SegId)(intptr_t)lv_event_get_user_data(e) == SEG_UNIT_F);
  if (f == g_cfg.fahrenheit) return;
  g_cfg.fahrenheit = f;
  g_prefs.putBool("tunitf", f);
  updateSegments();
}

static void onBrightness(lv_event_t *e) {
  lv_obj_t *s = lv_event_get_target_obj(e);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    g_cfg.brightness = (uint8_t)lv_slider_get_value(s);
    applyBrightness();
    char b[16]; snprintf(b, sizeof(b), "%u %%", (unsigned)g_cfg.brightness);
    setText(g_lblBright, b);
    updateNav();
  } else {
    g_prefs.putUChar("bright", g_cfg.brightness);   // save when the finger is released
  }
}

static void onSleep(lv_event_t *e) {
  lv_obj_t *s = lv_event_get_target_obj(e);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    g_cfg.sleepIdx = (uint8_t)lv_slider_get_value(s);
    setText(g_lblSleep, sleepLabel(g_cfg.sleepIdx));
    updateNav();
  } else {
    g_prefs.putUChar("sleep", g_cfg.sleepIdx);
  }
}

static void onRotation(lv_event_t *e) {
  SegId id = (SegId)(intptr_t)lv_event_get_user_data(e);
  uint8_t r = (id == SEG_ROT_180) ? 1 : 0;
  if (r == g_cfg.rotation) return;
  g_cfg.rotation = r;
  g_prefs.putUChar("rot", r);
  updateSegments();
  applyRotation();
}

// Motion sensor (PIR) on / off
static void onPirSwitch(lv_event_t *e) {
  g_cfg.pirOn = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
  g_prefs.putBool("pir", g_cfg.pirOn);
}

// Row with a built-in LVGL symbol as icon (the settings icon font has none)
static lv_obj_t *newSymbolRow(lv_obj_t *card, const char *symbol, const char *title, const char *sub) {
  lv_obj_t *row = newRow(card, nullptr, title, sub);
  lv_obj_t *ic = newLabel(row, symbol, FONT_28, COL_ICON);
  lv_obj_set_width(ic, 36);
  lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_move_to_index(ic, 0);
  return row;
}

static void buildDisplayPage(lv_obj_t *page) {
  char b[16];
  lv_obj_t *card = newCard(page);
  newSectionLabel(card, tr(S_SCREEN));

  lv_obj_t *row = newRow(card, ICON_SUN, tr(S_BRIGHTNESS), nullptr);
  g_slBright = newSlider(row, BRIGHT_MIN, 100, g_cfg.brightness, onBrightness);
  snprintf(b, sizeof(b), "%u %%", (unsigned)g_cfg.brightness);
  g_lblBright = newValueLabel(row, b, 110);

  row = newRow(card, ICON_MOON, tr(S_SLEEP), nullptr);
  g_slSleep = newSlider(row, 0, 7, g_cfg.sleepIdx, onSleep);
  g_lblSleep = newValueLabel(row, sleepLabel(g_cfg.sleepIdx), 120);

  row = newRow(card, ICON_ROTATE, tr(S_ROTATION), nullptr);
  lv_obj_t *seg = newBox(row);                       // pill container
  lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(seg, ui_col(COL_PILL), 0);
  lv_obj_set_style_radius(seg, 28, 0);
  lv_obj_set_style_pad_all(seg, 4, 0);
  lv_obj_set_style_pad_column(seg, 4, 0);
  flexRow(seg);
  g_segRot0   = newSegButton(seg, "0\xC2\xB0",   104, SEG_ROT_0,   onRotation);
  g_segRot180 = newSegButton(seg, "180\xC2\xB0", 104, SEG_ROT_180, onRotation);

  // Temperature unit: Celsius / Fahrenheit
  row = newSymbolRow(card, "\xC2\xB0", tr(S_TEMP_UNIT), nullptr);
  seg = newBox(row);
  lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(seg, ui_col(COL_PILL), 0);
  lv_obj_set_style_radius(seg, 28, 0);
  lv_obj_set_style_pad_all(seg, 4, 0);
  lv_obj_set_style_pad_column(seg, 4, 0);
  flexRow(seg);
  g_segUnitC = newSegButton(seg, "\xC2\xB0" "C", 104, SEG_UNIT_C, onTempUnit);
  g_segUnitF = newSegButton(seg, "\xC2\xB0" "F", 104, SEG_UNIT_F, onTempUnit);
  updateSegments();

  // Colour theme: dark / light (all pages)
  row = newSymbolRow(card, LV_SYMBOL_IMAGE, tr(S_THEME), nullptr);
  seg = newBox(row);
  lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(seg, ui_col(COL_PILL), 0);
  lv_obj_set_style_radius(seg, 28, 0);
  lv_obj_set_style_pad_all(seg, 4, 0);
  lv_obj_set_style_pad_column(seg, 4, 0);
  flexRow(seg);
  g_segThemeDark  = newSegButton(seg, tr(S_THEME_DARK),  104, SEG_THEME_DARK,  onTheme);
  g_segThemeLight = newSegButton(seg, tr(S_THEME_LIGHT), 104, SEG_THEME_LIGHT, onTheme);
  updateSegments();

  // Motion sensor (PIR): wakes the screen and triggers the alarm
  row = newSymbolRow(card, LV_SYMBOL_EYE_OPEN, tr(S_MOTION_SENSOR), tr(S_MOTION_SENSOR_SUB));
  g_swPir = newSwitch(row, g_cfg.pirOn, onPirSwitch);
  finishCard(card);
}

/* ------------------------------------------------------------------ */
/* System page                                                         */
/* ------------------------------------------------------------------ */

static void onVolume(lv_event_t *e) {
  lv_obj_t *s = lv_event_get_target_obj(e);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    g_cfg.volume = (uint8_t)lv_slider_get_value(s);
    applyVolume();
    char b[16]; snprintf(b, sizeof(b), "%u %%", (unsigned)g_cfg.volume);
    setText(g_lblVolume, b);
  } else {
    g_prefs.putUChar("vol", g_cfg.volume);
    if (g_cfg.volume > 0) M5.Speaker.tone(1000, 120);   // audible feedback
  }
}

static void onMicSwitch(lv_event_t *e) {
  g_cfg.micOn = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
  g_prefs.putBool("mic", g_cfg.micOn);
  applyMic();
}

// Touch (click) sound on / off
static void onClickSwitch(lv_event_t *e) {
  g_cfg.clickOn = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
  g_prefs.putBool("click", g_cfg.clickOn);
  if (g_cfg.clickOn) settings_touch_click();        // let the user hear it
}

// Blynk on / off (blynk.cpp connects / disconnects within ~100 ms)
static void onBlynkSwitch(lv_event_t *e) {
  g_cfg.blynkOn = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
  g_prefs.putBool("blynk", g_cfg.blynkOn);
}

// Restart: the first tap asks for confirmation, a second tap within 4 s restarts
void ui_prepare_restart();                       // ui.cpp: saves data, outputs to 0

static void restartNowCb(lv_timer_t *t) {
  (void)t;
  ESP.restart();
}

static void restartRevertCb(lv_timer_t *t) {
  (void)t;
  g_restartTimer = nullptr;
  if (!g_btnRestart) return;
  lv_label_set_text(lv_obj_get_child(g_btnRestart, 0), tr(S_RESTART));
  lv_obj_set_style_bg_color(g_btnRestart, ui_col(COL_PILL), 0);
}

static void onRestartClicked(lv_event_t *e) {
  (void)e;
  if (g_restartTimer == nullptr) {               // first tap: ask for confirmation
    lv_label_set_text(lv_obj_get_child(g_btnRestart, 0), tr(S_RESTART_CONFIRM));
    lv_obj_set_style_bg_color(g_btnRestart, ui_col(0xC62828), 0);
    g_restartTimer = lv_timer_create(restartRevertCb, 4000, nullptr);
    lv_timer_set_repeat_count(g_restartTimer, 1);
    return;
  }
  lv_timer_delete(g_restartTimer);               // second tap: restart
  g_restartTimer = nullptr;
  showToast(tr(S_T_RESTARTING));
  ui_prepare_restart();
  lv_timer_t *t = lv_timer_create(restartNowCb, 800, nullptr);   // let the toast show
  lv_timer_set_repeat_count(t, 1);
}

/* ------------------------------------------------------------------ */
/* System information window (System > Device > Information)           */
/* ------------------------------------------------------------------ */

int ui_power_level();                            // ui.cpp: battery %, -1 = none
int ui_power_source();                           // ui.cpp: 0 battery, 1 USB, 2 charging

enum InfoRow : uint8_t {
  IR_UPTIME = 0, IR_PROGRAM, IR_RAM, IR_PSRAM, IR_CHIP, IR_CPU_LOAD, IR_CPU_FREQ,
  IR_WIFI_SIG, IR_WIFI_LINK, IR_IP, IR_MAC, IR_BATTERY, IR_VERSION,
  IR_HVAC, IR_HVAC_PREV, IR_COUNT
};
static const StrId INFO_KEYS[IR_COUNT] = {
  S_SI_UPTIME, S_SI_PROGRAM, S_SI_RAM, S_SI_PSRAM, S_SI_CHIP, S_SI_CPU_LOAD, S_SI_CPU_FREQ,
  S_SI_WIFI_SIGNAL, S_SI_WIFI_LINK, S_SI_IP, S_SI_MAC, S_SI_BATTERY, S_SI_VERSION,
  S_SI_HVAC_MONTH, S_SI_HVAC_PREV
};

static lv_obj_t   *g_info = nullptr;             // full screen backdrop (top layer)
static lv_obj_t   *g_infoKey[IR_COUNT];
static lv_obj_t   *g_infoVal[IR_COUNT];
static lv_timer_t *g_infoTimer = nullptr;

// "12 h 34 min"
static void fmtHM(char *b, size_t n, uint32_t s) {
  snprintf(b, n, tr(S_FMT_HM), (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
}

// yyyymm -> "November 2026"
static void fmtMonth(char *b, size_t n, uint32_t ym) {
  if (ym == 0) { snprintf(b, n, "%s", tr(S_SI_THIS_MONTH)); return; }
  snprintf(b, n, "%s %lu", lang_month((int)(ym % 100) - 1), (unsigned long)(ym / 100));
}

static void refreshInfo() {
  if (!g_info) return;
  char b[160], t1[48], t2[48], t3[48];

  // Uptime
  uint64_t s = esp_timer_get_time() / 1000000ULL;
  snprintf(b, sizeof(b), tr(S_FMT_UPTIME), (unsigned long)(s / 86400), (unsigned long)((s / 3600) % 24),
           (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  setText(g_infoVal[IR_UPTIME], b);

  // Program memory: sketch size is read once (it scans the whole image)
  static uint32_t sketch = 0, part = 0, flash = 0;
  if (sketch == 0) { sketch = ESP.getSketchSize(); part = ESP.getFreeSketchSpace(); flash = ESP.getFlashChipSize(); }
  snprintf(b, sizeof(b), "%.2f / %.2f MB  (%d %%)   ·   %s %lu MB",
           sketch / 1048576.0, part / 1048576.0, part ? (int)((uint64_t)sketch * 100 / part) : 0,
           tr(S_SI_FLASH), (unsigned long)(flash / 1048576));
  setText(g_infoVal[IR_PROGRAM], b);

  // RAM (internal heap) and PSRAM: used / total
  uint32_t hs = ESP.getHeapSize(), hf = ESP.getFreeHeap();
  snprintf(b, sizeof(b), "%lu / %lu KB  (%d %%)", (unsigned long)((hs - hf) / 1024),
           (unsigned long)(hs / 1024), hs ? (int)((uint64_t)(hs - hf) * 100 / hs) : 0);
  setText(g_infoVal[IR_RAM], b);
  uint32_t ps = ESP.getPsramSize(), pf = ESP.getFreePsram();
  if (ps) snprintf(b, sizeof(b), "%.1f / %.1f MB  (%d %%)", (ps - pf) / 1048576.0, ps / 1048576.0,
                   (int)((uint64_t)(ps - pf) * 100 / ps));
  else    snprintf(b, sizeof(b), "-");
  setText(g_infoVal[IR_PSRAM], b);

  // Chip
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  int rev = ci.revision;                         // major * 100 + minor
  snprintf(t1, sizeof(t1), tr(S_SI_CORES), (int)ESP.getChipCores());
  snprintf(b, sizeof(b), "%s  v%d.%d   ·   %s   ·   Wi-Fi: ESP32-C6", ESP.getChipModel(),
           rev / 100, rev % 100, t1);
  setText(g_infoVal[IR_CHIP], b);

  // CPU load per core (approximate, idle time based) and clock
  b[0] = 0;
  for (int c = 0; c < sysinfo_cpu_cores(); c++) {
    int l = sysinfo_cpu_load(c);
    size_t n = strlen(b);
    if (l < 0) snprintf(b + n, sizeof(b) - n, "%sCore %d: --", c ? "   ·   " : "", c);
    else       snprintf(b + n, sizeof(b) - n, "%sCore %d: %d %%", c ? "   ·   " : "", c, l);
  }
  setText(g_infoVal[IR_CPU_LOAD], b);
  snprintf(b, sizeof(b), "%lu MHz", (unsigned long)getCpuFrequencyMhz());
  setText(g_infoVal[IR_CPU_FREQ], b);

  // Wi-Fi
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int q = 2 * (rssi + 100);
    q = q < 0 ? 0 : q > 100 ? 100 : q;
    snprintf(b, sizeof(b), "%d dBm  (%d %%)   ·   %s   ·   Ch %d", rssi, q, WiFi.SSID().c_str(),
             (int)WiFi.channel());
    setText(g_infoVal[IR_WIFI_SIG], b);
    // Standard of the link and its maximum PHY rate (1 stream, 20 MHz)
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      const char *std = ap.phy_11ax ? "802.11ax (Wi-Fi 6)" : ap.phy_11n ? "802.11n (Wi-Fi 4)"
                      : ap.phy_11g ? "802.11g" : "802.11b";
      int mbps = ap.phy_11ax ? 143 : ap.phy_11n ? 72 : ap.phy_11g ? 54 : 11;
      snprintf(b, sizeof(b), "%s   ·   max %d Mbps", std, mbps);
    } else {
      snprintf(b, sizeof(b), "-");
    }
    setText(g_infoVal[IR_WIFI_LINK], b);
    setText(g_infoVal[IR_IP], WiFi.localIP().toString().c_str());
  } else {
    setText(g_infoVal[IR_WIFI_SIG], tr(S_NOT_CONNECTED));
    setText(g_infoVal[IR_WIFI_LINK], "-");
    setText(g_infoVal[IR_IP], "-");
  }
  setText(g_infoVal[IR_MAC], WiFi.macAddress().c_str());

  // Battery
  int lvl = ui_power_level(), src = ui_power_source();
  if (lvl < 0) {
    setText(g_infoVal[IR_BATTERY], tr(S_SI_NO_BATTERY));
  } else {
    int mv = M5.Power.getBatteryVoltage();
    const char *st = src == 2 ? tr(S_SI_CHARGING) : src == 1 ? tr(S_SI_USB) : tr(S_SI_ON_BATTERY);
    if (mv > 0) snprintf(b, sizeof(b), "%d %%   ·   %.2f V   ·   %s", lvl, mv / 1000.0, st);
    else        snprintf(b, sizeof(b), "%d %%   ·   %s", lvl, st);
    setText(g_infoVal[IR_BATTERY], b);
  }

  // Software version
  snprintf(b, sizeof(b), "v%s   ·   %s %s", SETTINGS_VERSION, __DATE__, __TIME__);
  setText(g_infoVal[IR_VERSION], b);

  // HVAC run time: this month (heating / cooling share) and the previous month
  const HvacRuntime &rt = sysinfo_hvac_runtime();
  fmtMonth(t1, sizeof(t1), rt.month);
  snprintf(b, sizeof(b), tr(S_SI_HVAC_MONTH), t1);
  setText(g_infoKey[IR_HVAC], b);
  fmtHM(t1, sizeof(t1), rt.total_s);
  fmtHM(t2, sizeof(t2), rt.heat_s);
  fmtHM(t3, sizeof(t3), rt.cool_s);
  snprintf(b, sizeof(b), "%s   (%s %s  ·  %s %s)", t1, tr(S_MODE_HEAT), t2, tr(S_MODE_COOL), t3);
  setText(g_infoVal[IR_HVAC], b);
  if (rt.prev_month) {
    fmtMonth(t1, sizeof(t1), rt.prev_month);
    snprintf(b, sizeof(b), tr(S_SI_HVAC_PREV), t1);
    setText(g_infoKey[IR_HVAC_PREV], b);
    fmtHM(t1, sizeof(t1), rt.prev_total_s);
    setText(g_infoVal[IR_HVAC_PREV], t1);
  } else {
    snprintf(b, sizeof(b), tr(S_SI_HVAC_PREV), "-");
    setText(g_infoKey[IR_HVAC_PREV], b);
    setText(g_infoVal[IR_HVAC_PREV], tr(S_SI_NONE));
  }
}

static void infoTimerCb(lv_timer_t *t) { (void)t; refreshInfo(); }

static void closeInfo() {
  if (g_infoTimer) { lv_timer_delete(g_infoTimer); g_infoTimer = nullptr; }
  if (g_info)      { lv_obj_delete_async(g_info); g_info = nullptr; }
}

static void onInfoClose(lv_event_t *e) { (void)e; closeInfo(); }

static void onInfoBackdrop(lv_event_t *e) {
  if (lv_event_get_target_obj(e) == g_info) closeInfo();     // tap outside the window
}

static void openInfo(lv_event_t *e) {
  (void)e;
  if (g_info) return;
  g_info = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_info);
  lv_obj_set_size(g_info, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(g_info, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_info, LV_OPA_60, 0);
  lv_obj_remove_flag(g_info, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_info, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_info, onInfoBackdrop, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *panel = lv_obj_create(g_info);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 960, 640);
  lv_obj_center(panel);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(panel, ui_col(COL_CARD), 0);
  lv_obj_set_style_radius(panel, 28, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, ui_col(COL_SEP), 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);            // taps inside do not close

  lv_obj_t *title = newLabel(panel, tr(S_SI_TITLE), FONT_28, COL_TEXT);
  lv_obj_set_pos(title, 32, 26);

  lv_obj_t *x = lv_button_create(panel);
  lv_obj_set_size(x, 56, 56);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -20, 16);
  lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(x, ui_col(COL_PILL), 0);
  lv_obj_set_style_shadow_width(x, 0, 0);
  lv_obj_t *xl = newLabel(x, LV_SYMBOL_CLOSE, FONT_24, COL_TEXT);
  lv_obj_center(xl);
  lv_obj_set_ext_click_area(x, 12);
  lv_obj_add_event_cb(x, onInfoClose, LV_EVENT_CLICKED, nullptr);

  // Scrollable list: name | value
  lv_obj_t *list = lv_obj_create(panel);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 960 - 48, 640 - 104);
  lv_obj_set_pos(list, 24, 88);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(list, 16, 0);
  lv_obj_add_flag(list, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

  for (int i = 0; i < IR_COUNT; i++) {
    lv_obj_t *row = newBox(list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    flexRow(row);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    if (i < IR_COUNT - 1) {
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, ui_col(COL_SEP), 0);
    }
    g_infoKey[i] = newLabel(row, tr(INFO_KEYS[i]), FONT_20, COL_MUTED);
    lv_obj_set_width(g_infoKey[i], 300);
    lv_label_set_long_mode(g_infoKey[i], LV_LABEL_LONG_WRAP);
    g_infoVal[i] = newLabel(row, "", FONT_20, COL_TEXT);
    lv_obj_set_flex_grow(g_infoVal[i], 1);
    lv_label_set_long_mode(g_infoVal[i], LV_LABEL_LONG_WRAP);
  }

  refreshInfo();
  g_infoTimer = lv_timer_create(infoTimerCb, 1000, nullptr);
}

static void buildSystemPage(lv_obj_t *page) {
  char b[32];

  // Application header card
  lv_obj_t *hc = newCard(page);
  lv_obj_set_style_pad_ver(hc, 22, 0);
  flexRow(hc);
  lv_obj_set_style_pad_column(hc, 24, 0);
  lv_obj_t *circle = newBox(hc);
  lv_obj_set_size(circle, 72, 72);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(circle, ui_col(NAV_DEFS[PAGE_SYSTEM].circle), 0);
  lv_obj_t *ic = newIcon(circle, ICON_CHIP, NAV_DEFS[PAGE_SYSTEM].iconColor);
  lv_obj_center(ic);
  lv_obj_t *col = newBox(hc);
  flexColumn(col);
  lv_obj_t *line = newBox(col);
  flexRow(line);
  lv_obj_set_style_pad_column(line, 12, 0);
  newLabel(line, tr(S_TAB5_SETTINGS), FONT_28, COL_TEXT);
  snprintf(b, sizeof(b), "v%s", SETTINGS_VERSION);
  newLabel(line, b, FONT_24, COL_MUTED);
  char vb[96];
  snprintf(vb, sizeof(vb), tr(S_FMT_SW_VERSION), SETTINGS_VERSION);
  newLabel(col, vb, FONT_20, COL_MUTED);

  // Audio card
  lv_obj_t *card = newCard(page);
  newSectionLabel(card, tr(S_AUDIO));

  lv_obj_t *row = newRow(card, ICON_SPEAKER, tr(S_VOLUME), nullptr);
  g_slVolume = newSlider(row, 0, 100, g_cfg.volume, onVolume);
  snprintf(b, sizeof(b), "%u %%", (unsigned)g_cfg.volume);
  g_lblVolume = newValueLabel(row, b, 110);

  row = newRow(card, ICON_MIC, tr(S_MICROPHONE), tr(S_MIC_SUB));
  g_swMic = newSwitch(row, g_cfg.micOn, onMicSwitch);

  row = newSymbolRow(card, LV_SYMBOL_AUDIO, tr(S_TOUCH_SOUND), tr(S_TOUCH_SOUND_SUB));
  g_swClick = newSwitch(row, g_cfg.clickOn, onClickSwitch);
  finishCard(card);

  // Device card: Blynk switch + information + restart
  card = newCard(page);
  newSectionLabel(card, tr(S_DEVICE));

  row = newSymbolRow(card, LV_SYMBOL_UPLOAD, tr(S_BLYNK), tr(S_BLYNK_SUB));
  g_swBlynk = newSwitch(row, g_cfg.blynkOn, onBlynkSwitch);

  row = newSymbolRow(card, LV_SYMBOL_LIST, tr(S_SI_BTN), tr(S_SI_BTN_SUB));
  lv_obj_t *infoBtn = newPill(row, tr(S_SI_SHOW), openInfo, nullptr);
  lv_obj_set_style_min_width(infoBtn, 200, 0);

  row = newSymbolRow(card, LV_SYMBOL_REFRESH, tr(S_RESTART), tr(S_RESTART_SUB));
  if (g_restartTimer) { lv_timer_delete(g_restartTimer); g_restartTimer = nullptr; }
  g_btnRestart = newPill(row, tr(S_RESTART), onRestartClicked, nullptr);
  lv_obj_set_style_min_width(g_btnRestart, 200, 0);
  finishCard(card);
}

/* ------------------------------------------------------------------ */
/* Localization page (value rows open a picker overlay)                */
/* ------------------------------------------------------------------ */

static void refreshLocaleValues() {
  setText(g_lblLang, lang_name(g_cfg.lang));
  setText(g_lblTz, TIMEZONES[g_cfg.tz].label);
  setText(g_lblTimeFmt, tr(TIME_FORMATS[g_cfg.time24 ? 0 : 1]));
  setText(g_lblDateFmt, tr(DATE_FORMATS[g_cfg.dateFmt]));
  g_lastClockText[0] = '\0';        // force a sidebar refresh
  updateNav();
}

static void closePicker() {
  if (g_picker) {
    lv_obj_delete_async(g_picker);
    g_picker = nullptr;
  }
}

static void onPickerItem(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  switch (g_pickerId) {
    case PK_LANG:
      if (idx == g_cfg.lang) break;
      g_cfg.lang = (uint8_t)idx;
      g_prefs.putUChar("lang", g_cfg.lang);
      closePicker();
      lang_set(g_cfg.lang);
      g_langPending = true;                          // screens are rebuilt by the timer
      return;
    case PK_TZ:
      g_cfg.tz = (uint8_t)idx;
      g_prefs.putUChar("tz", g_cfg.tz);
      applyTimezone();
      break;
    case PK_TIMEFMT:
      g_cfg.time24 = (idx == 0);
      g_prefs.putBool("t24", g_cfg.time24);
      break;
    case PK_DATEFMT:
      g_cfg.dateFmt = (uint8_t)idx;
      g_prefs.putUChar("datef", g_cfg.dateFmt);
      break;
  }
  refreshLocaleValues();
  closePicker();
}

static void onPickerBackdrop(lv_event_t *e) {
  (void)e;
  closePicker();
}

static void openPicker(PickerId id) {
  if (g_picker) return;
  g_pickerId = id;

  const char *title = "";
  int count = 0, selected = 0;
  switch (id) {
    case PK_LANG:    title = tr(S_LANGUAGE);    count = LANG_COUNT; selected = g_cfg.lang; break;
    case PK_TZ:      title = tr(S_TIME_ZONE);   count = TZ_COUNT;   selected = g_cfg.tz;   break;
    case PK_TIMEFMT: title = tr(S_TIME_FORMAT); count = 2;          selected = g_cfg.time24 ? 0 : 1; break;
    case PK_DATEFMT: title = tr(S_DATE_FORMAT); count = 3;          selected = g_cfg.dateFmt; break;
  }

  // Dimmed backdrop; tapping it closes the picker
  g_picker = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_picker, SCR_W, SCR_H);
  lv_obj_set_pos(g_picker, 0, 0);
  lv_obj_set_style_radius(g_picker, 0, 0);
  lv_obj_set_style_border_width(g_picker, 0, 0);
  lv_obj_set_style_bg_color(g_picker, ui_col(0x000000), 0);
  lv_obj_set_style_bg_opa(g_picker, LV_OPA_70, 0);
  lv_obj_remove_flag(g_picker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(g_picker, onPickerBackdrop, LV_EVENT_CLICKED, nullptr);

  int listH = count * 72;
  if (listH > 480) listH = 480;
  lv_obj_t *panel = lv_obj_create(g_picker);
  lv_obj_set_size(panel, 640, listH + 96);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, ui_col(0x242424), 0);
  lv_obj_set_style_radius(panel, 28, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = newLabel(panel, title, FONT_24, COL_MUTED);
  lv_obj_set_pos(t, 32, 26);

  lv_obj_t *list = lv_obj_create(panel);
  lv_obj_set_size(list, 640, listH);
  lv_obj_set_pos(list, 0, 80);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  flexColumn(list);

  lv_obj_t *selRow = nullptr;
  for (int i = 0; i < count; i++) {
    const char *text = "";
    switch (id) {
      case PK_LANG:    text = lang_name(i);            break;
      case PK_TZ:      text = TIMEZONES[i].label;      break;
      case PK_TIMEFMT: text = tr(TIME_FORMATS[i]);     break;
      case PK_DATEFMT: text = tr(DATE_FORMATS[i]);     break;
    }
    bool disabled = false;

    lv_obj_t *row = newBox(list);
    lv_obj_set_size(row, LV_PCT(100), 72);
    lv_obj_set_style_pad_hor(row, 32, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, ui_col(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    flexRow(row, LV_FLEX_ALIGN_SPACE_BETWEEN);
    uint32_t col = disabled ? 0x666666 : (i == selected ? COL_ACCENT : COL_TEXT);
    newLabel(row, text, FONT_24, col);
    if (i == selected) newLabel(row, LV_SYMBOL_OK, FONT_24, COL_ACCENT);
    lv_obj_add_event_cb(row, onPickerItem, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (i == selected) selRow = row;
  }
  if (selRow) lv_obj_scroll_to_view(selRow, LV_ANIM_OFF);
}

static void onLocaleRow(lv_event_t *e) {
  openPicker((PickerId)(intptr_t)lv_event_get_user_data(e));
}

// Row with an icon, a title and "value  v" on the right
static lv_obj_t *newPickerRow(lv_obj_t *card, const char *icon, const char *title, PickerId id,
                              lv_obj_t **valueOut) {
  lv_obj_t *row = newRow(card, icon, title, nullptr);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(row, ui_col(COL_CARD_PRESS), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
  *valueOut = newLabel(row, "", FONT_24, COL_MUTED);
  newLabel(row, LV_SYMBOL_DOWN, FONT_20, COL_MUTED);
  lv_obj_add_event_cb(row, onLocaleRow, LV_EVENT_CLICKED, (void *)(intptr_t)id);
  return row;
}

static void buildLocalePage(lv_obj_t *page) {
  lv_obj_t *card = newCard(page);
  newPickerRow(card, ICON_TRANSLATE, tr(S_LANGUAGE),    PK_LANG,    &g_lblLang);
  newPickerRow(card, ICON_GLOBE,     tr(S_TIME_ZONE),   PK_TZ,      &g_lblTz);
  newPickerRow(card, ICON_CLOCK,     tr(S_TIME_FORMAT), PK_TIMEFMT, &g_lblTimeFmt);
  newPickerRow(card, ICON_CALENDAR,  tr(S_DATE_FORMAT), PK_DATEFMT, &g_lblDateFmt);
  finishCard(card);
  refreshLocaleValues();
}

/* ------------------------------------------------------------------ */
/* Wi-Fi logic                                                         */
/* ------------------------------------------------------------------ */

static int signalLevel(int rssi) {            // 1..4 bars
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -77) return 2;
  return 1;
}

static const char *encName(uint8_t enc) {
  switch ((wifi_auth_mode_t)enc) {
    case WIFI_AUTH_OPEN:            return tr(S_ENC_OPEN);
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
    default:                        return tr(S_ENC_SECURED);
  }
}

static bool isConnected() {
  return g_cfg.wifiOn && wifiStatus() == WL_CONNECTED;
}

static void wifiEnable(bool on) {
  if (on) {
    if (!g_wifiPinsSet) {
      WiFi.setPins(WIFI_SDIO_CLK, WIFI_SDIO_CMD, WIFI_SDIO_D0, WIFI_SDIO_D1,
                   WIFI_SDIO_D2, WIFI_SDIO_D3, WIFI_SDIO_RST);
      g_wifiPinsSet = true;
    }
    WiFi.persistent(false);                 // credentials are stored by this app
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // Connect with the saved credentials (default network)
    String ssid = g_prefs.getString("ssid", "");
    if (ssid.length() > 0) {
      String pass = g_prefs.getString("pass", "");
      strncpy(g_pendingSsid, ssid.c_str(), sizeof(g_pendingSsid) - 1);
      strncpy(g_pendingPass, pass.c_str(), sizeof(g_pendingPass) - 1);
      udp_th_wifi_config(g_pendingSsid);     // static IP for the outdoor unit (udp_th.h)
      WiFi.begin(g_pendingSsid, g_pendingPass[0] ? g_pendingPass : nullptr);
      g_connecting = true;
      g_connFailed = false;
      g_connectStartMs = millis();
    }
  } else {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    g_connecting = false;
    g_connFailed = false;
    g_scanning = false;
    g_netCount = 0;
    g_hasScanned = false;
  }
}

static void startConnect(const char *ssid, const char *pass) {
  strncpy(g_pendingSsid, ssid, sizeof(g_pendingSsid) - 1);
  g_pendingSsid[sizeof(g_pendingSsid) - 1] = '\0';
  strncpy(g_pendingPass, pass, sizeof(g_pendingPass) - 1);
  g_pendingPass[sizeof(g_pendingPass) - 1] = '\0';
  WiFi.disconnect(false, false);
  udp_th_wifi_config(g_pendingSsid);       // static IP for the outdoor unit (udp_th.h)
  WiFi.begin(g_pendingSsid, g_pendingPass[0] ? g_pendingPass : nullptr);
  g_connecting = true;
  g_connFailed = false;
  g_connectStartMs = millis();
  wifiUiRefresh();
}

static void onConnected() {
  g_connecting = false;
  g_connFailed = false;
  g_prefs.putString("ssid", g_pendingSsid);      // saved: becomes the default network
  g_prefs.putString("pass", g_pendingPass);
  strlcpy(g_data.wifi_ssid, g_pendingSsid, sizeof(g_data.wifi_ssid));
  strlcpy(g_data.wifi_pass, g_pendingPass, sizeof(g_data.wifi_pass));
  data_mark_dirty();
  applyTimezone();                               // start NTP with the selected time zone
  showToast(tr(S_T_CONNECTED));
  wifiUiRefresh();
}

static void onConnectFailed() {
  WiFi.disconnect(false, false);
  g_connecting = false;
  g_connFailed = true;
  showToast(tr(S_T_CONN_FAILED_PW));
  wifiUiRefresh();
}

static void startScan() {
  if (!g_cfg.wifiOn) { showToast(tr(S_T_WIFI_ON_FIRST)); return; }
  if (g_scanning) return;
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true, false);        // asynchronous scan, hidden networks excluded
  if (r == WIFI_SCAN_FAILED) { showToast(tr(S_T_SCAN_FAILED)); return; }
  g_scanning = true;
  g_scanStartMs = millis();
  lv_obj_remove_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
  rebuildNetworkList();
}

static void collectScanResults(int n) {
  g_netCount = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    uint8_t enc = (uint8_t)WiFi.encryptionType(i);
    int found = -1;
    for (int k = 0; k < g_netCount; k++) {
      if (strcmp(g_nets[k].ssid, s.c_str()) == 0) { found = k; break; }
    }
    if (found >= 0) {                            // same SSID twice: keep the strongest
      if (rssi > g_nets[found].rssi) { g_nets[found].rssi = rssi; g_nets[found].enc = enc; }
    } else if (g_netCount < MAX_NETS) {
      strncpy(g_nets[g_netCount].ssid, s.c_str(), 32);
      g_nets[g_netCount].ssid[32] = '\0';
      g_nets[g_netCount].rssi = rssi;
      g_nets[g_netCount].enc = enc;
      g_netCount++;
    }
  }
  // Sort by signal strength (strongest first)
  for (int i = 1; i < g_netCount; i++) {
    NetInfo key = g_nets[i];
    int j = i - 1;
    while (j >= 0 && g_nets[j].rssi < key.rssi) { g_nets[j + 1] = g_nets[j]; j--; }
    g_nets[j + 1] = key;
  }
}

static void wifiTask(uint32_t now) {
  if (!g_cfg.wifiOn) return;
  if (now - g_lastWifiPollMs < WIFI_POLL_MS) return;
  g_lastWifiPollMs = now;

  // Connection progress
  if (g_connecting) {
    wl_status_t st = wifiStatus();
    if (st == WL_CONNECTED) onConnected();
    else if (st == WL_CONNECT_FAILED || (now - g_connectStartMs) > CONNECT_TIMEOUT_MS) onConnectFailed();
  }

  // Scan progress
  if (g_scanning) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      if ((now - g_scanStartMs) > SCAN_TIMEOUT_MS) {
        g_scanning = false;
        lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
        showToast(tr(S_T_SCAN_TIMEOUT));
        rebuildNetworkList();
      }
    } else {
      g_scanning = false;
      lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
      if (n < 0) {
        showToast(tr(S_T_SCAN_FAILED));
      } else {
        collectScanResults(n);
        g_hasScanned = true;
      }
      WiFi.scanDelete();
      rebuildNetworkList();
    }
  }

  // Detect connection changes (lost / restored / signal level change)
  int sig = (int)wifiStatus() * 10 + (wifiStatus() == WL_CONNECTED ? signalLevel(WiFi.RSSI()) : 0);
  if (sig != g_lastWifiSig) {
    g_lastWifiSig = sig;
    wifiUiRefresh();
  }
}

/* ------------------------------------------------------------------ */
/* Wi-Fi UI                                                            */
/* ------------------------------------------------------------------ */

static void onNetworkClicked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= g_netCount) return;
  const NetInfo &n = g_nets[idx];

  if (g_connecting) { showToast(tr(S_T_CONNECTING_WAIT)); return; }
  if (isConnected()) {
    String cur = WiFi.SSID();
    if (cur == n.ssid) showToast(tr(S_T_ALREADY_CONNECTED));
    else               showToast(tr(S_T_DISCONNECT_FIRST));
    return;
  }
  if ((wifi_auth_mode_t)n.enc == WIFI_AUTH_OPEN) {
    startConnect(n.ssid, "");                    // open network: no password needed
  } else {
    openConnectDialog(n.ssid, false, encName(n.enc));   // opens the keyboard
  }
}

static bool g_listDirty = false;

// Requests a rebuild; the real work is done by listTask() from the main loop,
// because the list must not be deleted from inside one of its own event callbacks.
static void rebuildNetworkList() {
  g_listDirty = true;
}

static void doRebuildNetworkList() {
  if (!g_netList) return;
  lv_obj_clean(g_netList);

  if (g_netCount == 0) {
    const char *msg = g_scanning ? tr(S_SCANNING)
                    : g_hasScanned ? tr(S_NO_NETWORKS)
                                   : tr(S_TAP_SCAN);
    lv_obj_t *l = newLabel(g_netList, msg, FONT_24, COL_MUTED);
    lv_obj_set_style_pad_ver(l, 28, 0);
    return;
  }

  String cur = isConnected() ? WiFi.SSID() : String("");
  char buf[64];
  for (int i = 0; i < g_netCount; i++) {
    const NetInfo &n = g_nets[i];
    bool secured = (wifi_auth_mode_t)n.enc != WIFI_AUTH_OPEN;
    bool connected = (cur.length() > 0 && cur == n.ssid);

    lv_obj_t *row = newBox(g_netList);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 88, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_pad_column(row, 20, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, i == g_netCount - 1 ? 0 : 1, 0);
    lv_obj_set_style_border_color(row, ui_col(COL_SEP), 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, ui_col(COL_CARD_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    flexRow(row);

    // Signal level icon (with a lock for secured networks)
    newIcon(row, WIFI_GLYPH[secured ? 1 : 0][signalLevel(n.rssi) - 1], connected ? COL_ACCENT : COL_ICON);

    lv_obj_t *col = newBox(row);
    flexColumn(col);
    lv_obj_set_flex_grow(col, 1);
    newLabel(col, n.ssid, FONT_24, COL_TEXT);
    snprintf(buf, sizeof(buf), "%s  |  %d dBm", encName(n.enc), (int)n.rssi);   // encryption + signal
    newLabel(col, buf, FONT_20, COL_MUTED);

    if (connected) newLabel(row, LV_SYMBOL_OK, FONT_24, COL_OK);
    else           newLabel(row, LV_SYMBOL_RIGHT, FONT_24, COL_MUTED);

    lv_obj_add_event_cb(row, onNetworkClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}

static void listTask() {
  if (g_listDirty) {
    g_listDirty = false;
    doRebuildNetworkList();
  }
}

static void wifiUiRefresh() {
  if (!g_lblConnName) return;
  char buf[96];
  bool connected = isConnected();

  if (!g_cfg.wifiOn) {
    setText(g_lblConnName, tr(S_WIFI_OFF));
    setText(g_lblConnSub, "");
  } else if (g_connecting) {
    setText(g_lblConnName, g_pendingSsid);
    setText(g_lblConnSub, tr(S_CONNECTING));
    lv_obj_set_style_text_color(g_lblConnSub, ui_col(COL_ACCENT), 0);
  } else if (connected) {
    String ssid = WiFi.SSID();
    String ip = WiFi.localIP().toString();
    setText(g_lblConnName, ssid.c_str());
    snprintf(buf, sizeof(buf), tr(S_FMT_CONNECTED_INFO), ip.c_str(), (int)WiFi.RSSI());
    setText(g_lblConnSub, buf);
    lv_obj_set_style_text_color(g_lblConnSub, ui_col(COL_OK), 0);
  } else {
    setText(g_lblConnName, tr(S_NOT_CONNECTED));
    setText(g_lblConnSub, tr(g_connFailed ? S_CONN_FAILED : S_SELECT_NETWORK));
    lv_obj_set_style_text_color(g_lblConnSub, ui_col(g_connFailed ? COL_ERR : COL_MUTED), 0);
  }

  // Connection icon follows the signal level
  int lvl = connected ? signalLevel(WiFi.RSSI()) : 1;
  lv_label_set_text(g_lblConnIcon, WIFI_GLYPH[0][lvl - 1]);
  lv_obj_set_style_text_color(g_lblConnIcon, ui_col(connected ? COL_ACCENT : COL_ICON), 0);

  if (connected || g_connecting) lv_obj_remove_flag(g_btnDisconnect, LV_OBJ_FLAG_HIDDEN);
  else                           lv_obj_add_flag(g_btnDisconnect, LV_OBJ_FLAG_HIDDEN);

  setText(g_lblWifiSub, tr(g_cfg.wifiOn ? S_ON : S_OFF));
  if (g_cfg.wifiOn != lv_obj_has_state(g_swWifi, LV_STATE_CHECKED)) {
    if (g_cfg.wifiOn) lv_obj_add_state(g_swWifi, LV_STATE_CHECKED);
    else              lv_obj_remove_state(g_swWifi, LV_STATE_CHECKED);
  }

  rebuildNetworkList();
  updateNav();
}

static void onWifiSwitch(lv_event_t *e) {
  g_cfg.wifiOn = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
  g_prefs.putBool("wifi", g_cfg.wifiOn);
  wifiEnable(g_cfg.wifiOn);
  g_lastWifiSig = -1;
  wifiUiRefresh();
}

static void onScanClicked(lv_event_t *e) {
  (void)e;
  startScan();
}

static void onDisconnectClicked(lv_event_t *e) {
  (void)e;
  WiFi.disconnect(false, false);
  g_connecting = false;
  g_connFailed = false;
  g_prefs.remove("ssid");                        // forget: do not auto-connect at next start
  g_prefs.remove("pass");
  g_data.wifi_ssid[0] = '\0';                    // varsayilan ag da unutulur
  g_data.wifi_pass[0] = '\0';
  data_mark_dirty();
  showToast(tr(S_T_DISCONNECTED));
  g_lastWifiSig = -1;
  wifiUiRefresh();
}

static void onAddNetworkClicked(lv_event_t *e) {
  (void)e;
  if (!g_cfg.wifiOn) { showToast(tr(S_T_WIFI_ON_FIRST)); return; }
  if (g_connecting)  { showToast(tr(S_T_CONNECTING_WAIT)); return; }
  if (isConnected()) { showToast(tr(S_T_DISCONNECT_FIRST)); return; }
  openConnectDialog("", true, "");
}

static void buildWifiPage(lv_obj_t *page) {
  // Card 1: Wi-Fi switch and connection status
  lv_obj_t *card = newCard(page);
  lv_obj_t *row = newRow(card, WIFI_GLYPH[0][3], tr(S_WIFI), tr(S_OFF), &g_lblWifiSub);
  g_swWifi = newSwitch(row, g_cfg.wifiOn, onWifiSwitch);

  row = newRow(card, WIFI_GLYPH[0][0], tr(S_NOT_CONNECTED), "", &g_lblConnSub);
  g_lblConnIcon = lv_obj_get_child(row, 0);
  g_lblConnName = lv_obj_get_child(lv_obj_get_child(row, 1), 0);
  g_btnDisconnect = newPill(row, tr(S_DISCONNECT), onDisconnectClicked, nullptr);
  lv_obj_add_flag(g_btnDisconnect, LV_OBJ_FLAG_HIDDEN);
  finishCard(card);

  // Card 2: networks
  card = newCard(page);
  lv_obj_t *hdr = newBox(card);
  lv_obj_set_width(hdr, LV_PCT(100));
  lv_obj_set_height(hdr, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(hdr, 12, 0);
  flexRow(hdr, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(hdr, 12, 0);
  lv_obj_t *t = newLabel(hdr, tr(S_NETWORKS), FONT_20, COL_MUTED);
  lv_obj_set_flex_grow(t, 1);
  g_spinner = lv_spinner_create(hdr);
  lv_obj_set_size(g_spinner, 40, 40);
  lv_spinner_set_anim_params(g_spinner, 1000, 200);
  lv_obj_set_style_arc_width(g_spinner, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_spinner, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_spinner, ui_col(COL_PILL), LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_spinner, ui_col(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
  char pb[48];
  snprintf(pb, sizeof(pb), LV_SYMBOL_PLUS "  %s", tr(S_ADD));
  newPill(hdr, pb, onAddNetworkClicked, nullptr);
  snprintf(pb, sizeof(pb), LV_SYMBOL_REFRESH "  %s", tr(S_SCAN));
  g_btnScan = newPill(hdr, pb, onScanClicked, nullptr);

  g_netList = newBox(card);
  lv_obj_set_width(g_netList, LV_PCT(100));
  lv_obj_set_height(g_netList, LV_SIZE_CONTENT);
  flexColumn(g_netList);
  rebuildNetworkList();
}

/* ------------------------------------------------------------------ */
/* Connect dialog (on-screen QWERTY keyboard)                          */
/* ------------------------------------------------------------------ */

static void closeDialog() {
  if (g_dlg) {
    lv_obj_delete_async(g_dlg);
    g_dlg = g_dlgSsid = g_dlgPass = g_dlgKb = nullptr;
  }
}

static void dlgFocus(lv_obj_t *ta) {
  if (!g_dlgKb || !ta) return;
  lv_keyboard_set_textarea(g_dlgKb, ta);
  if (g_dlgSsid) {
    if (g_dlgSsid == ta) lv_obj_add_state(g_dlgSsid, LV_STATE_FOCUSED);
    else                 lv_obj_remove_state(g_dlgSsid, LV_STATE_FOCUSED);
  }
  if (g_dlgPass) {
    if (g_dlgPass == ta) lv_obj_add_state(g_dlgPass, LV_STATE_FOCUSED);
    else                 lv_obj_remove_state(g_dlgPass, LV_STATE_FOCUSED);
  }
}

static void dlgSubmit() {
  if (!g_dlg) return;
  char ssid[33], pass[65];
  strncpy(ssid, lv_textarea_get_text(g_dlgSsid), sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  strncpy(pass, lv_textarea_get_text(g_dlgPass), sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = '\0';
  if (ssid[0] == '\0') { showToast(tr(S_T_ENTER_SSID)); return; }
  closeDialog();
  startConnect(ssid, pass);
}

static void onDlgConnect(lv_event_t *e) { (void)e; dlgSubmit(); }
static void onDlgCancel(lv_event_t *e)  { (void)e; closeDialog(); }

static void onDlgKeyboard(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) dlgSubmit();         // keyboard "OK" key
  else if (code == LV_EVENT_CANCEL) closeDialog();
}

static void onDlgTextClicked(lv_event_t *e) {
  dlgFocus(lv_event_get_target_obj(e));
}

static void onDlgEye(lv_event_t *e) {
  (void)e;
  if (!g_dlgPass) return;
  bool hidden = lv_textarea_get_password_mode(g_dlgPass);
  lv_textarea_set_password_mode(g_dlgPass, !hidden);
}

static lv_obj_t *newDlgTextarea(lv_obj_t *parent, int x, int y, int w, const char *placeholder) {
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_pos(ta, x, y);
  lv_obj_set_size(ta, w, 64);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_style_bg_color(ta, ui_col(COL_CARD), 0);
  lv_obj_set_style_border_width(ta, 0, 0);
  lv_obj_set_style_radius(ta, 20, 0);
  lv_obj_set_style_pad_hor(ta, 22, 0);
  lv_obj_set_style_text_font(ta, FONT_24, 0);
  lv_obj_set_style_text_font(ta, FONT_24, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_text_color(ta, ui_col(COL_TEXT), 0);
  lv_obj_add_event_cb(ta, onDlgTextClicked, LV_EVENT_CLICKED, nullptr);
  return ta;
}

// manual = true: SSID can be typed; false: SSID is fixed (network chosen from the list)
static void openConnectDialog(const char *ssid, bool manual, const char *encText) {
  if (g_dlg) return;

  g_dlg = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_dlg, SCR_W, SCR_H);
  lv_obj_set_pos(g_dlg, 0, 0);
  lv_obj_set_style_bg_color(g_dlg, ui_col(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_dlg, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_dlg, 0, 0);
  lv_obj_set_style_border_width(g_dlg, 0, 0);
  lv_obj_set_style_pad_all(g_dlg, 0, 0);
  lv_obj_remove_flag(g_dlg, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = newLabel(g_dlg, manual ? tr(S_ADD_NETWORK) : ssid, FONT_28, COL_TEXT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
  char sub[96];
  if (manual) snprintf(sub, sizeof(sub), "%s", tr(S_ENTER_SSID_PW));
  else        snprintf(sub, sizeof(sub), tr(S_FMT_SECURITY), encText);
  lv_obj_t *subl = newLabel(g_dlg, sub, FONT_20, COL_MUTED);
  lv_obj_align(subl, LV_ALIGN_TOP_MID, 0, 62);

  lv_obj_t *cancel = newPill(g_dlg, tr(S_CANCEL), onDlgCancel, nullptr);
  lv_obj_set_pos(cancel, 24, 14);
  lv_obj_t *conn = newPill(g_dlg, tr(S_CONNECT), onDlgConnect, nullptr, COL_ACCENT, 0x202020);
  lv_obj_align(conn, LV_ALIGN_TOP_RIGHT, -24, 14);

  g_dlgSsid = newDlgTextarea(g_dlg, 40, 110, SCR_W - 80, tr(S_NETWORK_NAME));
  lv_textarea_set_max_length(g_dlgSsid, 32);
  if (!manual) {
    lv_textarea_set_text(g_dlgSsid, ssid);
    lv_obj_add_state(g_dlgSsid, LV_STATE_DISABLED);       // fixed SSID
  }

  g_dlgPass = newDlgTextarea(g_dlg, 40, 194, SCR_W - 80 - 110, tr(S_PASSWORD));
  lv_textarea_set_max_length(g_dlgPass, 64);
  lv_textarea_set_password_mode(g_dlgPass, true);

  lv_obj_t *eye = newPill(g_dlg, LV_SYMBOL_EYE_OPEN, onDlgEye, nullptr);
  lv_obj_set_size(eye, 96, 64);
  lv_obj_set_pos(eye, SCR_W - 40 - 96, 194);

  // Standard English QWERTY keyboard
  g_dlgKb = lv_keyboard_create(g_dlg);
  lv_obj_set_size(g_dlgKb, SCR_W, 340);
  lv_obj_align(g_dlgKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_mode(g_dlgKb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(g_dlgKb, onDlgKeyboard, LV_EVENT_ALL, nullptr);

  dlgFocus(manual ? g_dlgSsid : g_dlgPass);
}

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

static lv_obj_t *newPage(lv_obj_t *parent) {
  lv_obj_t *p = newBox(parent);
  lv_obj_set_pos(p, PAGE_X, PAGE_Y);
  lv_obj_set_size(p, PAGE_W, PAGE_H);
  // newBox() makes the page non-clickable: a non-clickable object never gets
  // the drag, so the page could not be scrolled. Pages taller than the screen
  // (System) must scroll vertically.
  lv_obj_add_flag(p, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                     LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC));
  lv_obj_set_scroll_dir(p, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);    // thin bar only while scrolling
  lv_obj_set_style_bg_opa(p, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(p, 6, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_row(p, 16, 0);
  lv_obj_set_style_pad_bottom(p, 32, 0);                     // last card clear of the edge
  flexColumn(p);
  return p;
}

// Periodic work, driven by an LVGL timer (runs inside lv_timer_handler)
static void rebuildForLanguage();

static void settingsTimerCb(lv_timer_t *t) {
  (void)t;
  uint32_t now = millis();
  if (g_langPending) rebuildForLanguage();
  wifiTask(now);
  listTask();
  clockTask(now);
  sleepTask(now);
  toastTask(now);
}

// Builds the settings screen (header, side menu, pages) in the active language.
// Called once at start-up and again after the language was changed.
static void buildScreen() {
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, ui_col(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  // The screen is not loaded here: Home -> settings icon -> ui.show(PAGE_SETTINGS)

  // ---- Header: common top bar of the project (icon + title, clock, Wi-Fi icon, X -> Home)
  menu_create(g_scr, tr(S_SETTINGS), LV_SYMBOL_SETTINGS, MENU_SUBPAGE, ui_col(COL_CARD));

  // ---- Side menu ----
  const char *navIcons[PAGE_COUNT] = {ICON_MONITOR, WIFI_GLYPH[0][3], ICON_TRANSLATE, ICON_CHIP};
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_t *c = newBox(g_scr);
    lv_obj_set_size(c, NAV_W, NAV_H);
    lv_obj_set_pos(c, NAV_X, NAV_Y + i * (NAV_H + NAV_GAP));
    lv_obj_set_style_radius(c, 36, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, ui_col(COL_CARD), 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, onNavClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *circle = newBox(c);
    lv_obj_set_size(circle, 72, 72);
    lv_obj_set_pos(circle, 20, (NAV_H - 72) / 2);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(circle, ui_col(NAV_DEFS[i].circle), 0);
    lv_obj_t *ic = newIcon(circle, navIcons[i], NAV_DEFS[i].iconColor);
    lv_obj_center(ic);

    lv_obj_t *tl = newLabel(c, tr(NAV_DEFS[i].title), FONT_28, COL_TEXT);
    lv_obj_set_pos(tl, 108, 34);
    lv_obj_t *sl = newLabel(c, "", FONT_20, COL_MUTED);
    lv_obj_set_pos(sl, 108, 78);

    g_nav[i].card = c; g_nav[i].circle = circle; g_nav[i].icon = ic;
    g_nav[i].title = tl; g_nav[i].sub = sl;
  }

  // ---- Pages ----
  for (int i = 0; i < PAGE_COUNT; i++) g_pages[i] = newPage(g_scr);
  buildDisplayPage(g_pages[PAGE_DISPLAY]);
  buildWifiPage(g_pages[PAGE_WIFI]);
  buildLocalePage(g_pages[PAGE_LOCALE]);
  buildSystemPage(g_pages[PAGE_SYSTEM]);

  selectPage(g_curPage);
  updateNav();
  wifiUiRefresh();
}

// Rebuilds the screen after a language change (runs in the settings timer,
// never inside an event of the objects that are deleted)
static void rebuildForLanguage() {
  g_langPending = false;
  closePicker();
  closeDialog();
  lv_obj_t *old = g_scr;
  bool active = (lv_screen_active() == old);
  g_lastClockText[0] = '\0';
  g_lastWifiSig = -1;
  closeInfo();                                     // top layer window of the old language / theme
  if (g_toast) { lv_obj_delete(g_toast); g_toast = nullptr; }
  createToast();                                   // top layer: rebuilt for the theme colours
  buildScreen();
  if (active) lv_screen_load(g_scr);
  if (old) lv_obj_delete_async(old);
  if (g_langCb) g_langCb();                        // the application rebuilds its screens
}

static void buildUi() {
  // Dark / light theme with amber accent; applies to every widget created afterwards
  applyLvTheme();

  buildScreen();
  createToast();

  // Sleep shield: invisible full screen object on the system layer, shown only while asleep
  g_sleepShield = lv_obj_create(lv_layer_sys());
  lv_obj_set_size(g_sleepShield, SCR_W, SCR_H);
  lv_obj_set_pos(g_sleepShield, 0, 0);
  lv_obj_set_style_bg_opa(g_sleepShield, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_sleepShield, 0, 0);
  lv_obj_set_style_radius(g_sleepShield, 0, 0);
  lv_obj_remove_flag(g_sleepShield, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_sleepShield, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_sleepShield, onSleepShield, LV_EVENT_ALL, nullptr);

  // Touch activity tracking for the sleep timer
  lv_indev_t *in = lv_indev_get_next(nullptr);
  if (in) lv_indev_add_event_cb(in, onIndevActivity, LV_EVENT_ALL, nullptr);

  lv_timer_create(settingsTimerCb, 100, nullptr);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

// Registers the real home page of the application. If a callback is set,
// settingsGoHome() calls it instead of showing the built-in placeholder page.
void settingsSetLanguageCallback(void (*callback)()) {
  g_langCb = callback;
}

void settingsSetHomeCallback(void (*callback)()) {
  g_homeCb = callback;
}

/* ------------------------------------------------------------------ */
/* Home page navigation                                                */
/* ------------------------------------------------------------------ */

// Shows the settings screen again (from the home page)
void settingsOpen() {
  if (!g_uiReady) return;
  lv_screen_load(g_scr);
}

static void onHomeSettingsClicked(lv_event_t *e) {
  (void)e;
  settingsOpen();
}

// Minimal placeholder home page: title and a tile that opens the settings.
// It is only used until the application registers its own home page.
static void buildHomeScreen() {
  g_homeScr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_homeScr, ui_col(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_homeScr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(g_homeScr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = newLabel(g_homeScr, tr(S_HOME), FONT_28, COL_TEXT);
  lv_obj_set_pos(title, 28, 40);

  // Tile styled like the side menu cards
  lv_obj_t *tile = newBox(g_homeScr);
  lv_obj_set_size(tile, NAV_W, NAV_H);
  lv_obj_set_pos(tile, NAV_X, NAV_Y);
  lv_obj_set_style_radius(tile, 36, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tile, ui_col(COL_CARD), 0);
  lv_obj_set_style_bg_color(tile, ui_col(COL_CARD_PRESS), LV_STATE_PRESSED);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tile, onHomeSettingsClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *circle = newBox(tile);
  lv_obj_set_size(circle, 72, 72);
  lv_obj_set_pos(circle, 20, (NAV_H - 72) / 2);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(circle, ui_col(COL_PILL), 0);
  lv_obj_center(newIcon(circle, ICON_GEAR, COL_TEXT));

  lv_obj_t *tl = newLabel(tile, tr(S_SETTINGS), FONT_28, COL_TEXT);
  lv_obj_set_pos(tl, 108, 34);
  lv_obj_t *sl = newLabel(tile, tr(S_PH_SETTINGS_SUB), FONT_20, COL_MUTED);
  lv_obj_set_pos(sl, 108, 78);

  lv_obj_t *hint = newLabel(g_homeScr, tr(S_PH_HINT), FONT_20, COL_MUTED);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

// Returns to the home page. Used by the close (X) button of the settings screen.
void settingsGoHome() {
  if (!g_uiReady) return;
  closePicker();                       // close any open overlay first
  closeDialog();
  if (g_homeCb) {                      // application provided home page
    g_homeCb();
    return;
  }
  if (!g_homeScr) buildHomeScreen();
  lv_screen_load(g_homeScr);
}

// Builds the settings screen. Requires: M5.begin() done and LVGL display + touch
// driver initialized (done in main.ino before ui.init()).
void settings() {
  if (g_uiReady) return;

  // Restore every setting from non-volatile memory and apply it
  loadConfig();
  applyRotation();
  applyBrightness();
  applyVolume();
  applyMic();
  setenv("TZ", TIMEZONES[g_cfg.tz].posix, 1);
  tzset();
  g_lastActivityMs = millis();

  buildUi();
  g_uiReady = true;

  // Default network: if no network is stored by Settings, DEF_WIFI_SSID /
  // DEF_WIFI_PASS of app_data.h (g_data) are used
  if (g_prefs.getString("ssid", "").length() == 0 && g_data.wifi_ssid[0] &&
      strcmp(g_data.wifi_ssid, PLACEHOLDER_WIFI_SSID) != 0) {
    g_prefs.putString("ssid", g_data.wifi_ssid);
    g_prefs.putString("pass", g_data.wifi_pass);
  }

  // Wi-Fi: connect to the saved network if Wi-Fi is enabled
  if (g_cfg.wifiOn) wifiEnable(true);
  wifiUiRefresh();
}

/* ------------------------------------------------------------------ */
/* Tab5 Home project integration                                       */
/* ------------------------------------------------------------------ */

lv_obj_t *settings_create() {
  if (!g_uiReady) settings();
  updateNav();
  wifiUiRefresh();
  return g_scr;
}

lv_obj_t *settings_screen() { return g_scr; }

void settings_hide_popups() {
  closePicker();
  closeDialog();
  closeInfo();
}

void settings_wake(bool release_shield) {
  if (!g_uiReady) return;
  lv_display_trigger_activity(nullptr);
  g_lastActivityMs = millis();
  if (g_screenOff) wakeScreen();
  if (release_shield && g_sleepShield) lv_obj_add_flag(g_sleepShield, LV_OBJ_FLAG_HIDDEN);
}

bool settings_wifi_connected() { return isConnected(); }

bool settings_motion_enabled() { return g_cfg.pirOn; }
bool settings_blynk_enabled() { return g_cfg.blynkOn; }

// Short click on every new touch (called by the touch driver, main.ino)
void settings_touch_click() {
  if (!g_cfg.clickOn || g_cfg.volume == 0) return;
  if (M5.Mic.isRunning()) return;                  // speaker and microphone share the I2S bus
  if (!M5.Speaker.isRunning()) M5.Speaker.begin();
  M5.Speaker.tone(3000, 20);                       // non-blocking, 20 ms
}

bool  settings_fahrenheit() { return g_cfg.fahrenheit; }
bool  settings_light_theme() { return g_cfg.lightTheme; }
float settings_temp(float c) { return g_cfg.fahrenheit ? c * 9.0f / 5.0f + 32.0f : c; }
float settings_temp_delta(float dc) { return g_cfg.fahrenheit ? dc * 9.0f / 5.0f : dc; }
const char *settings_temp_unit() { return g_cfg.fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C"; }

bool settings_time24() { return g_cfg.time24; }

bool settings_format_time(char *out, size_t n) { return formatTime(out, n, false); }

bool settings_format_date(char *out, size_t n) {
  time_t t = time(nullptr);
  if (t < 1700000000) { snprintf(out, n, "--"); return false; }
  struct tm tmv;
  localtime_r(&t, &tmv);
  static const char *const fmt[3] = {"%d/%m/%Y", "%m/%d/%Y", "%Y-%m-%d"};
  strftime(out, n, fmt[g_cfg.dateFmt % 3], &tmv);
  return true;
}
