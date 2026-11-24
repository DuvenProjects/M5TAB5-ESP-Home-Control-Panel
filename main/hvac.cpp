/*
 * hvac.cpp
 * HVAC controller for M5Stack Tab5 (ESP32-P4) using LVGL 9 and an AHT10 sensor.
 *
 * Hardware (pins in hvac.h):
 *   - AHT10 / AHT20 temperature / humidity sensor (own bounded driver), also used for control
 *   - Heating output and cooling output (digital GPIO, relays)
 *   - Heating LED and cooling LED (digital GPIO)
 *   - Fan speed PWM output and dry mode PWM output
 *   - All outputs are driven to 0 at start-up
 *
 * Features:
 *   - On/off button, modes: Auto, Heat, Cool, Dry, Fan
 *   - Heating target 10-30 C, cooling target 18-30 C (0.5 C steps),
 *     dry mode humidity target 10-100 % (5 % steps)
 *   - Fan speed: Auto and 3 fixed speeds
 *   - Hysteresis control; "Quick" button starts the active heating / cooling
 *     mode early when the hysteresis condition allows it
 *   - Safety: any sensor error disables every output; heating stops above
 *     30 C; heating, cooling and dry stop after 2 h of continuous run
 *     (fan mode is not limited by this)
 *   - Mode, fan speed and targets are stored in NVS and restored at start-up
 *   - Information window with the complete HVAC status
 *
 * Architecture:
 *   main.ino : M5Tab5 LVGL display / touch driver, ui.early(), ui.init()
 *   ui.cpp   : ui.early() -> hvac_pins_safe(), ui.init() -> hvac_control()
 *              ui.show(PAGE_HVAC) -> hvac_create() (persistent screen)
 *   hvac.cpp : ALL LVGL screen code and the control logic. Everything runs
 *              from one LVGL timer, so no loop function is needed.
 */

#include "hvac.h"

#include <M5Unified.h>
#include <Wire.h>
#include <Preferences.h>
#include <lvgl.h>
#include <time.h>
#include <stdarg.h>
#include "app_data.h"     // g_data: values shared with the Home page
#include "settings.h"     // time format (Localization)
#include "menu.h"         // common top bar (icon + title, clock, Wi-Fi, X)
#include "lang.h"         // tr(), FONT_xx
#include "theme.h"        // ui_col(): dark / light theme colours
#include "health.h"         // stage marker (watchdog diagnosis)

/* ------------------------------------------------------------------ */
/* Fonts and icons                                                     */
/* ------------------------------------------------------------------ */

extern "C" {
LV_FONT_DECLARE(font_hvac_big);     // 104 px digits (target temperature)
LV_FONT_DECLARE(font_hvac_mid);     //  72 px digits (Auto mode, two targets)
#if HVAC_USE_ICON_FONT
LV_FONT_DECLARE(font_hvac_icons);
LV_FONT_DECLARE(font_hvac_anim);   // 64 px flame / snowflake (heating / cooling animation)
#endif
}

#if HVAC_USE_ICON_FONT
#define ICON_FONT      (&font_hvac_icons)
#define ICON_HOME_TEMP "\xEE\x80\x80"   // U+E000
#define ICON_POWER     "\xEE\x80\x81"   // U+E001
#define ICON_FLAME     "\xEE\x80\x82"   // U+E002
#define ICON_SNOW      "\xEE\x80\x83"   // U+E003
#define ICON_AUTO      "\xEE\x80\x84"   // U+E004
#define ICON_DROP      "\xEE\x80\x85"   // U+E005
#define ICON_FAN       "\xEE\x80\x86"   // U+E006
#define ICON_INFO      "\xEE\x80\x87"   // U+E007
#define ICON_BOLT      "\xEE\x80\x88"   // U+E008
// Wi-Fi status icons U+E009..U+E00D = level 0 (no connection) .. 4 (full signal)
static const char *const WIFI_ICONS[5] = {"\xEE\x80\x89", "\xEE\x80\x8A", "\xEE\x80\x8B", "\xEE\x80\x8C", "\xEE\x80\x8D"};
#define ANIM_FONT      (&font_hvac_anim)
#define ANIM_FLAME     "\xEE\x80\x80"    // U+E000 in font_hvac_anim
#define ANIM_SNOW      "\xEE\x80\x81"    // U+E001 in font_hvac_anim
#define ANIM_DROP      "\xEE\x80\x82"    // U+E002
#define ANIM_FAN       "\xEE\x80\x83"    // U+E003
#else
#define ICON_FONT      (&lv_font_montserrat_28)
#define ICON_HOME_TEMP LV_SYMBOL_HOME
#define ICON_POWER     LV_SYMBOL_POWER
#define ICON_FLAME     LV_SYMBOL_CHARGE
#define ICON_SNOW      LV_SYMBOL_REFRESH
#define ICON_AUTO      LV_SYMBOL_SETTINGS
#define ICON_DROP      LV_SYMBOL_TINT
#define ICON_FAN       LV_SYMBOL_LOOP
#define ICON_INFO      LV_SYMBOL_LIST
#define ICON_BOLT      LV_SYMBOL_PLAY
static const char *const WIFI_ICONS[5] = {LV_SYMBOL_CLOSE, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI};
#define ANIM_FONT      (&lv_font_montserrat_48)
#define ANIM_FLAME     LV_SYMBOL_CHARGE
#define ANIM_SNOW      LV_SYMBOL_REFRESH
#define ANIM_DROP      LV_SYMBOL_TINT
#define ANIM_FAN       LV_SYMBOL_LOOP
#endif

#define DEG "\xC2\xB0"              // degree sign (UTF-8)

// Temperatures are kept in Celsius; these convert for display only
// (unit selected in Settings > Display, see settings.h)
static inline float dispT(float c) { return settings_temp(c); }
static inline float dispD(float dc) { return settings_temp_delta(dc); }

// "30 °C" / "86 °F"
static const char *tempText(float c) {
  static char b[24];
  snprintf(b, sizeof(b), "%d %s", (int)lroundf(dispT(c)), settings_temp_unit());
  return b;
}

// Toast with a printf style text
static void showToast(const char *msg);
static void showToastF(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  showToast(b);
}

/* ------------------------------------------------------------------ */
/* Configuration constants                                             */
/* ------------------------------------------------------------------ */

// Target ranges
static const float    HEAT_MIN        = 10.0f;
static const float    HEAT_MAX        = 30.0f;
static const float    HEAT_DEFAULT    = 21.0f;
static const float    COOL_MIN        = 18.0f;
static const float    COOL_MAX        = 30.0f;
static const float    COOL_DEFAULT    = 25.0f;
static const float    TEMP_STEP       = 0.5f;
static const int      HUM_MIN         = 10;
static const int      HUM_MAX         = 100;
static const int      HUM_STEP        = 5;
static const int      HUM_DEFAULT     = 50;

// In Auto mode the heating target must stay below the cooling target
static const float    TARGET_GAP      = 1.0f;

// Hysteresis (+/- around the target)
static const float    TEMP_HYST       = 0.5f;   // C
static const float    HUM_HYST        = 3.0f;   // % RH

// Safety limits
static const float    HEAT_OVER_TEMP  = 30.0f;                          // heating stops above this
static const uint32_t MAX_RUN_MS      = 2UL * 60UL * 60UL * 1000UL;     // 2 h continuous run
static const uint32_t LOCKOUT_MS      = 15UL * 60UL * 1000UL;           // lock after max run time

// Sensor
static const uint32_t SENSOR_PERIOD_MS  = 2000;
static const uint32_t SENSOR_RETRY_MS   = 10000;
static const uint8_t  SENSOR_FAIL_LIMIT = 3;

// Fan speed (percent of PWM duty) for the 3 fixed speeds
static const uint8_t  FAN_PCT[3]        = {33, 66, 100};

// Timing
static const uint32_t SAVE_DELAY_MS   = 1500;   // debounce before writing targets to NVS
static const uint32_t UI_REFRESH_MS   = 500;
static const uint32_t TOAST_MS        = 3000;

// Screen geometry: UI_LP(landscape 1280 x 720, portrait 720 x 1280) (theme.h)
//  landscape: header row with Quick / Power on the right, Auto targets side by side
//  portrait : Quick / Power in a row under the header, Auto targets under each
//             other, the warning banner at the bottom of the card
#define SCR_W      ui_scr_w()
#define SCR_H      ui_scr_h()
#define CARD_X     20
#define CARD_Y     108                          // same top / bottom as the other pages
#define CARD_W     UI_LP(1240, 680)
#define CARD_H     UI_LP(592, 1152)             // 108 .. 700 / 108 .. 1260
#define CONTENT_X  UI_LP(50, 30)                // content left edge inside the card
#define CONTENT_W  UI_LP(1142, 620)             // content width inside the card
#define HEAD_Y     UI_LP(40, 30)                // header row (icon, name, buttons)
#define PILL_Y     UI_LP(HEAD_Y + 14, 142)      // Quick / Power buttons
#define PILL_W     UI_LP(230, 302)
#define STATUS_Y   UI_LP(146, 252)              // status caption
#define NOW_Y      UI_LP(298, 754)              // measurement line
#define BLK_Y      UI_LP(143, 404)              // target area (captions in line with the status)
#define BLK_H      190
#define AUTO_BLK_W UI_LP(500, 620)              // width of one target block in Auto mode
#define AUTO_Y1    UI_LP(BLK_Y, 300)            // Auto mode: heating target
#define AUTO_Y2    UI_LP(BLK_Y, 510)            //            cooling target (portrait: below)
#define MODE_Y     UI_LP(356, 820)              // mode button row
#define MODE_H     UI_LP(102, 118)
#define MODE_GAP   UI_LP(14, 12)
#define FAN_Y      UI_LP(488, 960)              // fan selector row
#define FAN_H      UI_LP(73, 84)

// Colors
#define COL_BG        0x000000
#define COL_TEXT      0xFFFFFF
#define COL_MUTED     0xAEB8C4
#define COL_TILE      0x2A2D33
#define COL_CARD      0x202020      // flat card background (neutral gray in RGB565)
#define COL_ACTIVE    0xFFB300      // background of active buttons
#define COL_ON_ACTIVE 0x202020      // text / icon on an active button
#define COL_PANEL     0x1F2228
#define COL_OK        0x3DDC63
#define COL_ERR       0xFF5252

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

enum Mode   : uint8_t { MODE_AUTO = 0, MODE_HEAT, MODE_COOL, MODE_DRY, MODE_FAN, MODE_COUNT };
enum Action : uint8_t { ACT_NONE = 0, ACT_HEAT, ACT_COOL, ACT_DRY, ACT_FAN };

enum ButtonId : intptr_t {
  BTN_HEAT_MINUS = 1, BTN_HEAT_PLUS,
  BTN_COOL_MINUS,     BTN_COOL_PLUS,
  BTN_HUM_MINUS,      BTN_HUM_PLUS,
  BTN_POWER, BTN_QUICK, BTN_INFO, BTN_CLOSE, BTN_FAN_PICK
};

struct Palette { uint32_t accent, top, bottom; };

// One palette per situation (card gradient and accent color)
static const Palette PAL_OFF  = {0xB0B6BF, 0x2B2F36, 0x16181C};
static const Palette PAL_AUTO = {0xB39DDB, 0x3C3459, 0x1E1B2D};
static const Palette PAL_HEAT = {0xFF8A50, 0x5F3724, 0x2B1B14};
static const Palette PAL_COOL = {0x4FC3F7, 0x2D4F78, 0x1A2B42};
static const Palette PAL_DRY  = {0x4DD0C4, 0x1F5053, 0x132A2C};
static const Palette PAL_FAN  = {0x9FB4C7, 0x38485B, 0x1D252E};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static bool          g_uiReady = false;

// Sensor
static uint8_t       g_ahtPhase        = 0;      // 0 idle, 1 measuring
static uint32_t      g_ahtTrigMs       = 0;
static bool          g_sensorOk        = false;
static float         g_temp            = 0.0f;
static float         g_hum             = 0.0f;
static uint8_t       g_sensorFails     = 0;
static uint32_t      g_lastSensorMs    = 0;
static uint32_t      g_lastRetryMs     = 0;

// Settings (persisted in NVS)
static Preferences   g_prefs;
static Mode          g_mode            = MODE_AUTO;
static float         g_heatSp          = HEAT_DEFAULT;
static float         g_coolSp          = COOL_DEFAULT;
static int           g_humSp           = HUM_DEFAULT;
static uint8_t       g_fanSpeed        = 0;           // 0 = auto, 1..3 = fixed speeds
static bool          g_spDirty         = false;
static uint32_t      g_spChangedMs     = 0;

// Runtime (power always starts OFF)
static bool          g_power           = false;
static Action        g_action          = ACT_NONE;
static bool          g_quickRun        = false;       // current run was started with Quick
static uint32_t      g_runStartMs      = 0;           // start of the continuous heating/cooling/dry run
static bool          g_lock            = false;       // locked after the max run time
static uint32_t      g_lockUntilMs     = 0;

// Last values written to the outputs (to avoid redundant writes)
static bool          g_pwmReady        = false;
static int8_t        g_outHeat         = -1;
static int8_t        g_outCool         = -1;
static int16_t       g_outFanPct       = -1;
static int16_t       g_outDryPct       = -1;
static uint8_t       g_fanPct          = 0;           // current fan duty (percent)
static uint8_t       g_dryPct          = 0;           // current dry PWM duty (percent)

// UI state
static bool          g_uiDirty         = true;
static uint32_t      g_lastUiMs        = 0;
static uint32_t      g_lastClockMs     = 0;
static int           g_lastPalette     = -1;
static int           g_lastLayoutMode  = -1;
static void        (*g_homeCb)()       = nullptr;

// UI objects
struct Block { lv_obj_t *root, *cap, *val, *minus, *plus; };

static lv_obj_t *g_scr = nullptr, *g_homeScr = nullptr, *g_card = nullptr;
static lv_obj_t *g_headIconCircle, *g_headIcon, *g_cardIcon;
static lv_obj_t *g_banner, *g_bannerLbl, *g_clock, *g_wifi, *g_animIcon;
static int g_wifiLevel = 0;      // 0 = not connected, 1..4 = signal bars
static int g_animKind  = 0;      // 0 none, 1 flame, 2 snowflake, 3 droplet, 4 fan
static lv_obj_t *g_pwrBtn, *g_pwrIcon, *g_pwrLbl;
static lv_obj_t *g_quickBtn, *g_quickIcon, *g_quickLbl;
static lv_obj_t *g_status, *g_now;
static Block     g_blk[3];                 // 0 = heating, 1 = cooling, 2 = humidity
static Block     g_fanBlk;                 // fan mode block (no buttons)
static lv_obj_t *g_modeBtn[MODE_COUNT];
static lv_obj_t *g_modeIcon[MODE_COUNT];
static lv_obj_t *g_modeLbl[MODE_COUNT];
static lv_obj_t *g_fanRow, *g_fanRowLbl;

// Overlays
static lv_obj_t *g_toast = nullptr, *g_toastLbl = nullptr;
static uint32_t  g_toastUntil = 0;
static lv_obj_t *g_picker = nullptr;
static lv_obj_t *g_info = nullptr;

enum InfoId : uint8_t {
  I_POWER = 0, I_MODE, I_STATE, I_TEMP, I_HUM, I_SENSOR,
  I_HEAT_SP, I_COOL_SP, I_HUM_SP, I_FAN, I_RUN, I_OUT, INFO_COUNT
};
static const StrId INFO_CAPTIONS[INFO_COUNT] = {
  S_INFO_POWER, S_INFO_MODE, S_INFO_STATE, S_INFO_TEMP, S_INFO_HUM, S_INFO_SENSOR,
  S_INFO_HEAT_SP, S_INFO_COOL_SP, S_INFO_HUM_SP, S_INFO_FAN, S_INFO_RUN, S_INFO_OUTPUTS
};
static lv_obj_t *g_infoVal[INFO_COUNT];

// Forward declarations
static void showToast(const char *msg);
static void refreshUi(uint32_t now);
static void refreshInfo(uint32_t now);
static void evaluate(uint32_t now);
static void closePicker();
static void closeInfo();

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *modeName(Mode m) {
  static const StrId n[MODE_COUNT] = {S_MODE_AUTO, S_MODE_HEAT, S_MODE_COOL, S_MODE_DRY, S_MODE_FAN};
  return tr(n[m]);
}

static bool conditioning(Action a) {
  return a == ACT_HEAT || a == ACT_COOL || a == ACT_DRY;
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/* Outputs                                                             */
/* ------------------------------------------------------------------ */

static void pwmInit() {
  if (!g_pwmReady) {
    ledcAttach(FAN_PWM_PIN, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcAttach(DRY_PWM_PIN, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    g_pwmReady = true;
  }
  ledcWrite(FAN_PWM_PIN, 0);
  ledcWrite(DRY_PWM_PIN, 0);
}

// All outputs to 0
void hvac_pins_safe() {
  pinMode(HEAT_PIN, OUTPUT);     digitalWrite(HEAT_PIN, OUTPUT_INACTIVE_LEVEL);
  pinMode(COOL_PIN, OUTPUT);     digitalWrite(COOL_PIN, OUTPUT_INACTIVE_LEVEL);
  pinMode(HEAT_LED_PIN, OUTPUT); digitalWrite(HEAT_LED_PIN, LED_INACTIVE_LEVEL);
  pinMode(COOL_LED_PIN, OUTPUT); digitalWrite(COOL_LED_PIN, LED_INACTIVE_LEVEL);
  if (g_pwmReady) {
    ledcWrite(FAN_PWM_PIN, 0);
    ledcWrite(DRY_PWM_PIN, 0);
  } else {
    pinMode(FAN_PWM_PIN, OUTPUT); digitalWrite(FAN_PWM_PIN, LOW);
    pinMode(DRY_PWM_PIN, OUTPUT); digitalWrite(DRY_PWM_PIN, LOW);
  }
  g_outHeat = g_outCool = -1;
  g_outFanPct = g_outDryPct = -1;
}

// Fan duty for the current action
static uint8_t computeFanPct() {
  if (g_action == ACT_NONE) return 0;
  if (g_fanSpeed >= 1 && g_fanSpeed <= 3) return FAN_PCT[g_fanSpeed - 1];

  // Auto fan speed
  switch (g_action) {
    case ACT_HEAT: {
      float err = g_heatSp - g_temp;
      return err < 1.5f ? FAN_PCT[0] : (err < 3.0f ? FAN_PCT[1] : FAN_PCT[2]);
    }
    case ACT_COOL: {
      float err = g_temp - g_coolSp;
      return err < 1.5f ? FAN_PCT[0] : (err < 3.0f ? FAN_PCT[1] : FAN_PCT[2]);
    }
    case ACT_DRY:
      return (g_hum - (float)g_humSp) < 10.0f ? FAN_PCT[0] : FAN_PCT[1];   // quiet
    default:
      return FAN_PCT[1];                                                    // fan only
  }
}

// Dry PWM duty: 50 % at the target, 100 % when 10 % RH above it
static uint8_t computeDryPct() {
  if (g_action != ACT_DRY) return 0;
  float pct = 50.0f + 5.0f * (g_hum - (float)g_humSp);
  return (uint8_t)clampf(pct, 50.0f, 100.0f);
}

// Writes the outputs that belong to the current action (only changed values)
static void applyOutputs() {
  bool heat = (g_action == ACT_HEAT);
  bool cool = (g_action == ACT_COOL);      // never both: only one action exists at a time
  g_fanPct = computeFanPct();
  g_dryPct = computeDryPct();

  // Switch off first, then on (heating and cooling are never on together)
  if (!heat && g_outHeat != 0) {
    digitalWrite(HEAT_PIN, OUTPUT_INACTIVE_LEVEL);
    digitalWrite(HEAT_LED_PIN, LED_INACTIVE_LEVEL);
    g_outHeat = 0;
  }
  if (!cool && g_outCool != 0) {
    digitalWrite(COOL_PIN, OUTPUT_INACTIVE_LEVEL);
    digitalWrite(COOL_LED_PIN, LED_INACTIVE_LEVEL);
    g_outCool = 0;
  }
  if (heat && g_outHeat != 1) {
    digitalWrite(HEAT_PIN, OUTPUT_ACTIVE_LEVEL);
    digitalWrite(HEAT_LED_PIN, LED_ACTIVE_LEVEL);
    g_outHeat = 1;
  }
  if (cool && g_outCool != 1) {
    digitalWrite(COOL_PIN, OUTPUT_ACTIVE_LEVEL);
    digitalWrite(COOL_LED_PIN, LED_ACTIVE_LEVEL);
    g_outCool = 1;
  }

  if (g_pwmReady) {
    if (g_outFanPct != g_fanPct) {
      ledcWrite(FAN_PWM_PIN, (uint32_t)g_fanPct * ((1u << PWM_RESOLUTION_BITS) - 1) / 100u);
      g_outFanPct = g_fanPct;
    }
    if (g_outDryPct != g_dryPct) {
      ledcWrite(DRY_PWM_PIN, (uint32_t)g_dryPct * ((1u << PWM_RESOLUTION_BITS) - 1) / 100u);
      g_outDryPct = g_dryPct;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Sensor                                                              */
/* ------------------------------------------------------------------ */

// AHT10 / AHT20 driver of our own. The Adafruit library waits for the
// "busy" bit with an endless loop; a failed I2C read returns 0xFF (busy
// bit set), so one glitch on the cable froze the main loop until the
// watchdog restarted the panel. Here every wait has a limit and the
// measurement runs in two steps (start, then read 100 ms later) so the
// screen never waits for the sensor.
static const uint8_t AHT_BUSY = 0x80, AHT_CAL = 0x08;

static bool ahtWrite(const uint8_t* d, size_t n) {
  Wire.beginTransmission(SENSOR_I2C_ADDR);
  Wire.write(d, n);
  return Wire.endTransmission() == 0;
}

static bool ahtRead(uint8_t* d, size_t n) {
  if (Wire.requestFrom((uint8_t)SENSOR_I2C_ADDR, (uint8_t)n) != n) { while (Wire.available()) Wire.read(); return false; }
  for (size_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

static bool sensorPresent() {
  Wire.beginTransmission(SENSOR_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

static bool sensorBegin() {
  g_ahtPhase = 0;
  const uint8_t rst = 0xBA;                              // soft reset
  if (!ahtWrite(&rst, 1)) return false;
  delay(20);
  uint8_t st = 0;
  if (!ahtRead(&st, 1) || st == 0xFF) return false;
  if (!(st & AHT_CAL)) {                                 // calibrate: AHT10 0xE1, AHT20 0xBE
    const uint8_t c10[3] = {0xE1, 0x08, 0x00}, c20[3] = {0xBE, 0x08, 0x00};
    if (!ahtWrite(c10, 3)) ahtWrite(c20, 3);
    for (int i = 0; i < 10; i++) {                       // max. 100 ms
      delay(10);
      if (ahtRead(&st, 1) && st != 0xFF && !(st & AHT_BUSY)) break;
    }
    if (!ahtRead(&st, 1) || st == 0xFF || !(st & AHT_CAL)) return false;
  }
  return true;
}

static bool ahtTrigger() {
  const uint8_t cmd[3] = {0xAC, 0x33, 0x00};
  return ahtWrite(cmd, 3);
}

// 1 = new values, 0 = still busy, -1 = error
static int ahtFetch() {
  uint8_t d[6];
  if (!ahtRead(d, 6) || d[0] == 0xFF) return -1;
  if (d[0] & AHT_BUSY) return 0;
  uint32_t rh = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rt = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  float h = rh * 100.0f / 1048576.0f;
  float t = rt * 200.0f / 1048576.0f - 50.0f;
  if (isnan(t) || isnan(h) || t <= -40.0f || t >= 85.0f || h < 0.0f || h > 100.0f) return -1;
  g_temp = t;
  g_hum  = h;
  return 1;
}

static void sensorResult(bool ok) {
  if (ok) {
    g_sensorFails = 0;
    g_sensorOk = true;
  } else {
    if (g_sensorFails < 255) g_sensorFails++;
    if (g_sensorFails >= SENSOR_FAIL_LIMIT) g_sensorOk = false;
  }
  g_uiDirty = true;
}

// Start-up: one measurement with a fixed limit (max. ~200 ms)
static void readSensor() {
  int r = -1;
  if (ahtTrigger()) {
    delay(80);
    for (int i = 0; i < 12; i++) {
      r = ahtFetch();
      if (r != 0) break;
      delay(10);
    }
  }
  sensorResult(r == 1);
}

static void sensorTask(uint32_t now) {
  if (g_ahtPhase == 1) {                                 // measurement running
    if (now - g_ahtTrigMs < 100) return;
    int r = ahtFetch();
    if (r == 0 && now - g_ahtTrigMs < 500) return;       // busy: next tick
    g_ahtPhase = 0;
    sensorResult(r == 1);
    return;
  }
  if ((now - g_lastSensorMs) < SENSOR_PERIOD_MS) return;
  g_lastSensorMs = now;
  // While faulty, try to start the sensor again from time to time
  if (!g_sensorOk && (now - g_lastRetryMs) >= SENSOR_RETRY_MS) {
    g_lastRetryMs = now;
    if (!(sensorPresent() && sensorBegin())) { sensorResult(false); return; }
  }
  if (ahtTrigger()) { g_ahtPhase = 1; g_ahtTrigMs = now; }
  else sensorResult(false);
}

/* ------------------------------------------------------------------ */
/* Control logic                                                       */
/* ------------------------------------------------------------------ */

// Change the active action (outputs are updated immediately)
static void setAction(Action next, uint32_t now) {
  if (next == g_action) return;
  if (conditioning(next) && !conditioning(g_action)) g_runStartMs = now;   // new continuous run
  g_action   = next;
  g_quickRun = false;
  g_uiDirty  = true;
  applyOutputs();
}

// "on" = the output is already running (stop threshold), otherwise start threshold
static bool wantHeat(bool on) {
  if (g_temp > HEAT_OVER_TEMP) return false;
  return on ? (g_temp < g_heatSp + TEMP_HYST) : (g_temp <= g_heatSp - TEMP_HYST);
}

static bool wantCool(bool on) {
  return on ? (g_temp > g_coolSp - TEMP_HYST) : (g_temp >= g_coolSp + TEMP_HYST);
}

static bool wantDry(bool on) {
  return on ? (g_hum > (float)g_humSp - HUM_HYST) : (g_hum >= (float)g_humSp + HUM_HYST);
}

static void evaluate(uint32_t now) {
  // Release the lockout when it expires
  if (g_lock && (int32_t)(now - g_lockUntilMs) >= 0) {
    g_lock = false;
    g_uiDirty = true;
  }

  // Hard stop: power off or sensor fault -> every output is 0 (fan included).
  // A sensor fault also switches the HVAC off: it must be turned on again by the user.
  if (!g_sensorOk) g_power = false;
  if (!g_power) {
    setAction(ACT_NONE, now);
    applyOutputs();
    return;
  }

  // Max continuous run time (heating, cooling and dry; not the fan mode)
  if (conditioning(g_action) && (now - g_runStartMs) >= MAX_RUN_MS) {
    setAction(ACT_NONE, now);
    g_lock = true;
    g_lockUntilMs = now + LOCKOUT_MS;
    g_uiDirty = true;
  }

  Action next = ACT_NONE;
  bool onHeat = (g_action == ACT_HEAT);
  bool onCool = (g_action == ACT_COOL);
  bool onDry  = (g_action == ACT_DRY);

  if (g_mode == MODE_FAN) {
    next = ACT_FAN;
  } else if (!g_lock) {
    switch (g_mode) {
      case MODE_HEAT: if (wantHeat(onHeat)) next = ACT_HEAT; break;
      case MODE_COOL: if (wantCool(onCool)) next = ACT_COOL; break;
      case MODE_DRY:  if (wantDry(onDry))   next = ACT_DRY;  break;
      case MODE_AUTO:
        if (wantHeat(onHeat))      next = ACT_HEAT;
        else if (wantCool(onCool)) next = ACT_COOL;
        break;
      default: break;
    }
  }
  setAction(next, now);
  applyOutputs();     // fan / dry duty follow the measurements
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static void loadSettings() {
  g_prefs.begin("hvac", false);

  uint8_t m = g_prefs.getUChar("mode", MODE_AUTO);
  g_mode = (m < MODE_COUNT) ? (Mode)m : MODE_AUTO;

  float h = g_prefs.getFloat("heat", HEAT_DEFAULT);
  float c = g_prefs.getFloat("cool", COOL_DEFAULT);
  if (isnan(h) || h < HEAT_MIN || h > HEAT_MAX) h = HEAT_DEFAULT;
  if (isnan(c) || c < COOL_MIN || c > COOL_MAX) c = COOL_DEFAULT;
  g_heatSp = roundf(h * 2.0f) / 2.0f;
  g_coolSp = roundf(c * 2.0f) / 2.0f;

  int hs = g_prefs.getUChar("hum", HUM_DEFAULT);
  if (hs < HUM_MIN || hs > HUM_MAX) hs = HUM_DEFAULT;
  g_humSp = (hs / HUM_STEP) * HUM_STEP;

  uint8_t f = g_prefs.getUChar("fan", 0);
  g_fanSpeed = (f <= 3) ? f : 0;

  // last on / off choice of the user (screen or Blynk). The outputs still
  // start at 0; the control logic decides after the first sensor reading,
  // and a sensor error keeps the HVAC off.
  g_power = g_prefs.getBool("power", false);
}

// Targets are saved automatically shortly after the last change
static void saveTask(uint32_t now) {
  if (g_spDirty && (now - g_spChangedMs) >= SAVE_DELAY_MS) {
    g_prefs.putFloat("heat", g_heatSp);
    g_prefs.putFloat("cool", g_coolSp);
    g_prefs.putUChar("hum", (uint8_t)g_humSp);
    g_spDirty = false;
  }
}

static void markTargetsChanged() {
  g_spDirty = true;
  g_spChangedMs = millis();
  g_uiDirty = true;
}

/* ------------------------------------------------------------------ */
/* User actions                                                        */
/* ------------------------------------------------------------------ */

// Auto mode: keep the cooling target at least TARGET_GAP above the heating target.
// heatMoved = true: push the cooling target up (or pull heating down at the limit).
static bool enforceGap(bool heatMoved) {
  if (g_coolSp - g_heatSp >= TARGET_GAP) return true;
  if (heatMoved) {
    float c = g_heatSp + TARGET_GAP;
    if (c <= COOL_MAX) { g_coolSp = c; return true; }
    g_heatSp = g_coolSp - TARGET_GAP;
  } else {
    float h = g_coolSp - TARGET_GAP;
    if (h >= HEAT_MIN) { g_heatSp = h; return true; }
    g_coolSp = g_heatSp + TARGET_GAP;
  }
  return false;
}

static void changeHeat(float delta) {
  float n = clampf(g_heatSp + delta, HEAT_MIN, HEAT_MAX);
  if (n == g_heatSp) { showToastF(tr(S_T_HEAT_RANGE), (int)lroundf(dispT(HEAT_MIN)), (int)lroundf(dispT(HEAT_MAX)), settings_temp_unit()); return; }
  g_heatSp = n;
  if (g_mode == MODE_AUTO && !enforceGap(true)) { char g[24]; snprintf(g, sizeof(g), "%.1f %s", dispD(TARGET_GAP), settings_temp_unit()); showToastF(tr(S_T_AUTO_GAP), g); }
  markTargetsChanged();
}

static void changeCool(float delta) {
  float n = clampf(g_coolSp + delta, COOL_MIN, COOL_MAX);
  if (n == g_coolSp) { showToastF(tr(S_T_COOL_RANGE), (int)lroundf(dispT(COOL_MIN)), (int)lroundf(dispT(COOL_MAX)), settings_temp_unit()); return; }
  g_coolSp = n;
  if (g_mode == MODE_AUTO && !enforceGap(false)) { char g[24]; snprintf(g, sizeof(g), "%.1f %s", dispD(TARGET_GAP), settings_temp_unit()); showToastF(tr(S_T_AUTO_GAP), g); }
  markTargetsChanged();
}

static void changeHum(int delta) {
  int n = g_humSp + delta;
  if (n < HUM_MIN) n = HUM_MIN;
  if (n > HUM_MAX) n = HUM_MAX;
  if (n == g_humSp) { showToast(tr(S_T_HUM_RANGE)); return; }
  g_humSp = n;
  markTargetsChanged();
}

static void setMode(Mode m) {
  if (m == g_mode) return;
  g_mode = m;
  g_prefs.putUChar("mode", (uint8_t)m);        // saved immediately
  if (m == MODE_AUTO && enforceGap(true)) markTargetsChanged();
  g_uiDirty = true;
  evaluate(millis());
}

static void setFan(uint8_t speed) {
  if (speed > 3) speed = 0;
  g_fanSpeed = speed;
  g_prefs.putUChar("fan", speed);
  g_uiDirty = true;
  applyOutputs();
}

// Switches the HVAC on / off and saves the choice. Returns false when it
// cannot be started (sensor error).
static bool setPower(bool on, bool showMessage) {
  if (on && !g_sensorOk) {
    if (showMessage) showToast(tr(S_T_SENSOR_NOSTART));
    return false;
  }
  if (on != g_power) {
    g_power = on;
    g_prefs.putBool("power", on);                // saved immediately
  }
  g_uiDirty = true;
  evaluate(millis());
  return true;
}

static void togglePower() {
  setPower(!g_power, true);
}

// Quick start: runs the active heating / cooling mode right away when the
// hysteresis condition allows it. Pressing it again stops a quick run.
static void handleQuick() {
  uint32_t now = millis();

  if (!g_power)    { showToast(tr(S_T_HVAC_OFF)); return; }
  if (!g_sensorOk) { showToast(tr(S_T_SENSOR_NOSTART)); return; }

  if (g_quickRun && conditioning(g_action)) {      // second press: stop
    setAction(ACT_NONE, now);
    return;
  }
  if (g_mode == MODE_DRY || g_mode == MODE_FAN) {
    showToast(tr(S_T_QUICK_MODES));
    return;
  }
  if (g_lock) { showToast(tr(S_T_LOCKED_WAIT)); return; }
  if (g_action == ACT_HEAT || g_action == ACT_COOL) {
    showToast(tr(S_T_ALREADY_RUNNING));
    return;
  }

  // Direction: fixed in Heat / Cool mode, nearest side in Auto mode
  Action dir;
  if (g_mode == MODE_HEAT)      dir = ACT_HEAT;
  else if (g_mode == MODE_COOL) dir = ACT_COOL;
  else                          dir = (g_temp < (g_heatSp + g_coolSp) / 2.0f) ? ACT_HEAT : ACT_COOL;

  if (dir == ACT_HEAT) {
    if (g_temp > HEAT_OVER_TEMP)                { showToastF(tr(S_T_ROOM_ABOVE_30), tempText(HEAT_OVER_TEMP)); return; }
    if (g_temp >= g_heatSp + TEMP_HYST)         { showToast(tr(S_T_WARM_ENOUGH)); return; }
  } else {
    if (g_temp <= g_coolSp - TEMP_HYST)         { showToast(tr(S_T_COOL_ENOUGH)); return; }
  }

  setAction(dir, now);
  g_quickRun = true;
}

/* ------------------------------------------------------------------ */
/* UI helpers                                                          */
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

static void flexRow(lv_obj_t *o, lv_flex_align_t main = LV_FLEX_ALIGN_CENTER) {
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(o, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static void flexColumn(lv_obj_t *o, lv_flex_align_t cross = LV_FLEX_ALIGN_CENTER) {
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, cross, cross);
}

static lv_obj_t *newLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, ui_col(color), 0);
  return l;
}

// Makes a box react to touch: translucent white background while pressed
static void makeClickable(lv_obj_t *o, ButtonId id, lv_event_code_t code = LV_EVENT_CLICKED);

static void onButton(lv_event_t *e);

static void makeClickable(lv_obj_t *o, ButtonId id, lv_event_code_t code) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(o, ui_col(0xFFFFFF), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(o, 60, LV_STATE_PRESSED);
  lv_obj_add_event_cb(o, onButton, code, (void *)(intptr_t)id);
}

// Round -/+ button
static lv_obj_t *newStepButton(lv_obj_t *parent, const char *symbol, ButtonId id) {
  lv_obj_t *b = newBox(parent);
  lv_obj_set_size(b, 118, 102);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(b, 14, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_border_color(b, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(b, 45, 0);
  makeClickable(b, id, LV_EVENT_SHORT_CLICKED);
  lv_obj_add_event_cb(b, onButton, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)id);   // hold to repeat
  lv_obj_t *l = newLabel(b, symbol, FONT_32, COL_TEXT);
  lv_obj_center(l);
  return b;
}

// Target block: caption, big value, minus / plus buttons
static Block newBlock(lv_obj_t *card, const char *caption, ButtonId minusId, ButtonId plusId) {
  Block b;
  b.root = newBox(card);
  lv_obj_set_size(b.root, CONTENT_W, BLK_H);
  lv_obj_set_pos(b.root, CONTENT_X, BLK_Y);

  b.cap = newLabel(b.root, caption, FONT_24, COL_MUTED);
  lv_obj_set_style_text_letter_space(b.cap, 3, 0);
  lv_obj_align(b.cap, LV_ALIGN_TOP_MID, 0, 5);

  b.val = newLabel(b.root, "", &font_hvac_big, COL_TEXT);
  lv_obj_align(b.val, LV_ALIGN_CENTER, 0, -2);

  if (minusId) {
    b.minus = newStepButton(b.root, LV_SYMBOL_MINUS, minusId);
    lv_obj_align(b.minus, LV_ALIGN_LEFT_MID, 0, 0);
    b.plus = newStepButton(b.root, LV_SYMBOL_PLUS, plusId);
    lv_obj_align(b.plus, LV_ALIGN_RIGHT_MID, 0, 0);
  } else {
    b.minus = b.plus = nullptr;
  }
  lv_obj_add_flag(b.root, LV_OBJ_FLAG_HIDDEN);
  return b;
}

// Pill button with icon and text (power / quick)
static lv_obj_t *newPill(lv_obj_t *card, int x, int y, int w, int h, const char *icon, const char *text,
                         ButtonId id, lv_obj_t **iconOut, lv_obj_t **textOut) {
  lv_obj_t *p = newBox(card);
  lv_obj_set_size(p, w, h);
  lv_obj_set_pos(p, x, y);
  lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(p, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(p, 18, 0);
  lv_obj_set_style_pad_column(p, 12, 0);
  flexRow(p);
  makeClickable(p, id);
  *iconOut = newLabel(p, icon, ICON_FONT, COL_TEXT);
  *textOut = newLabel(p, text, FONT_28, COL_TEXT);
  return p;
}

/* ------------------------------------------------------------------ */
/* Toast                                                               */
/* ------------------------------------------------------------------ */

static void createToast() {
  g_toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g_toast, ui_col(0x3A3F47), 0);
  lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_toast, 30, 0);
  lv_obj_set_style_border_width(g_toast, 2, 0);
  lv_obj_set_style_border_color(g_toast, ui_col(0xFFC107), 0);
  lv_obj_set_style_pad_hor(g_toast, 32, 0);
  lv_obj_set_style_pad_ver(g_toast, 16, 0);
  lv_obj_set_style_max_width(g_toast, 1100, 0);
  lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_CLICKABLE);
  g_toastLbl = newLabel(g_toast, "", FONT_24, COL_TEXT);
  lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
}

static void showToast(const char *msg) {
  lv_label_set_text(g_toastLbl, msg);
  lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_move_foreground(g_toast);
  g_toastUntil = millis() + TOAST_MS;
}

static void toastTask(uint32_t now) {
  if (g_toast && !lv_obj_has_flag(g_toast, LV_OBJ_FLAG_HIDDEN) && (int32_t)(now - g_toastUntil) >= 0) {
    lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  }
}

/* ------------------------------------------------------------------ */
/* Fan speed picker                                                    */
/* ------------------------------------------------------------------ */

static const char *fanText(uint8_t speed) {
  static const StrId t[4] = {S_AUTO, S_FAN_OPT1, S_FAN_OPT2, S_FAN_OPT3};
  return tr(t[speed > 3 ? 0 : speed]);
}

static void closePicker() {
  if (g_picker) {
    lv_obj_delete_async(g_picker);
    g_picker = nullptr;
  }
}

static void onPickerItem(lv_event_t *e) {
  setFan((uint8_t)(intptr_t)lv_event_get_user_data(e));
  closePicker();
}

static void onBackdrop(lv_event_t *e) {
  (void)e;
  closePicker();
}

static void openFanPicker() {
  if (g_picker) return;

  g_picker = lv_obj_create(lv_layer_top());          // dimmed backdrop, tap to close
  lv_obj_set_size(g_picker, SCR_W, SCR_H);
  lv_obj_set_pos(g_picker, 0, 0);
  lv_obj_set_style_radius(g_picker, 0, 0);
  lv_obj_set_style_border_width(g_picker, 0, 0);
  lv_obj_set_style_bg_color(g_picker, ui_col(0x000000), 0);
  lv_obj_set_style_bg_opa(g_picker, LV_OPA_70, 0);
  lv_obj_remove_flag(g_picker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(g_picker, onBackdrop, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *panel = lv_obj_create(g_picker);
  lv_obj_set_size(panel, 640, 4 * 76 + 88);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, ui_col(0x252A31), 0);
  lv_obj_set_style_radius(panel, 28, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = newLabel(panel, tr(S_FAN_SPEED_TITLE), FONT_24, COL_MUTED);
  lv_obj_set_pos(t, 32, 26);

  for (int i = 0; i < 4; i++) {
    lv_obj_t *row = newBox(panel);
    lv_obj_set_size(row, 640, 76);
    lv_obj_set_pos(row, 0, 76 + i * 76);
    lv_obj_set_style_pad_hor(row, 32, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, ui_col(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, 40, LV_STATE_PRESSED);
    flexRow(row, LV_FLEX_ALIGN_SPACE_BETWEEN);
    bool sel = (i == g_fanSpeed);
    newLabel(row, fanText((uint8_t)i), FONT_28, sel ? 0x4FC3F7 : COL_TEXT);
    if (sel) newLabel(row, LV_SYMBOL_OK, FONT_28, 0x4FC3F7);
    lv_obj_add_event_cb(row, onPickerItem, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}

/* ------------------------------------------------------------------ */
/* Information window                                                  */
/* ------------------------------------------------------------------ */

static const char *stateText() {
  if (!g_sensorOk) return tr(S_SENSOR_ERROR);
  if (!g_power)    return tr(S_OFF_CAP);
  if (g_lock)      return tr(S_CAP_LOCKED);
  switch (g_action) {
    case ACT_HEAT: return tr(S_CAP_HEATING);
    case ACT_COOL: return tr(S_CAP_COOLING);
    case ACT_DRY:  return tr(S_CAP_DRYING);
    case ACT_FAN:  return tr(S_CAP_FAN_RUNNING);
    default:       return tr(S_CAP_STANDBY);
  }
}

static void hms(char *out, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000UL;
  snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
}

static void refreshInfo(uint32_t now) {
  if (!g_info) return;
  char b[96], r[16];

  setText(g_infoVal[I_POWER], tr(g_power ? S_ON : S_OFF));
  setText(g_infoVal[I_MODE], modeName(g_mode));
  setText(g_infoVal[I_STATE], stateText());

  if (g_sensorOk) {
    snprintf(b, sizeof(b), "%.1f %s", dispT(g_temp), settings_temp_unit());   setText(g_infoVal[I_TEMP], b);
    snprintf(b, sizeof(b), "%.0f %%", g_hum);          setText(g_infoVal[I_HUM], b);
    setText(g_infoVal[I_SENSOR], tr(S_OK_STATE));
    lv_obj_set_style_text_color(g_infoVal[I_SENSOR], ui_col(COL_OK), 0);
  } else {
    setText(g_infoVal[I_TEMP], "--");
    setText(g_infoVal[I_HUM], "--");
    setText(g_infoVal[I_SENSOR], tr(S_ERROR_CAP));
    lv_obj_set_style_text_color(g_infoVal[I_SENSOR], ui_col(COL_ERR), 0);
  }

  snprintf(b, sizeof(b), tr(S_FMT_SP_HYST), dispT(g_heatSp), settings_temp_unit(), dispD(TEMP_HYST));  setText(g_infoVal[I_HEAT_SP], b);
  snprintf(b, sizeof(b), tr(S_FMT_SP_HYST), dispT(g_coolSp), settings_temp_unit(), dispD(TEMP_HYST));  setText(g_infoVal[I_COOL_SP], b);
  snprintf(b, sizeof(b), tr(S_FMT_HUM_HYST), g_humSp, HUM_HYST);            setText(g_infoVal[I_HUM_SP], b);

  if (g_fanSpeed == 0) snprintf(b, sizeof(b), tr(S_FMT_FAN_AUTO_NOW), (unsigned)g_fanPct);
  else                 snprintf(b, sizeof(b), tr(S_FMT_FAN_SPEED_NOW), (unsigned)g_fanSpeed, (unsigned)g_fanPct);
  setText(g_infoVal[I_FAN], b);

  if (conditioning(g_action)) {
    hms(b, sizeof(b), now - g_runStartMs);
    hms(r, sizeof(r), MAX_RUN_MS);
    char c[128];
    snprintf(c, sizeof(c), "%s / %s", b, r);
    setText(g_infoVal[I_RUN], c);
  } else if (g_action == ACT_FAN) {
    setText(g_infoVal[I_RUN], tr(S_FAN_NO_LIMIT));
  } else if (g_lock) {
    snprintf(b, sizeof(b), tr(S_FMT_LOCKED_MIN), (unsigned long)((g_lockUntilMs - now) / 60000UL + 1));
    setText(g_infoVal[I_RUN], b);
  } else {
    setText(g_infoVal[I_RUN], "-");
  }

  snprintf(b, sizeof(b), tr(S_FMT_OUTPUTS), tr(g_action == ACT_HEAT ? S_ON_CAP : S_OFF_CAP),
           tr(g_action == ACT_COOL ? S_ON_CAP : S_OFF_CAP), (unsigned)g_dryPct);
  setText(g_infoVal[I_OUT], b);
}

static void closeInfo() {
  if (g_info) {
    lv_obj_delete_async(g_info);
    g_info = nullptr;
  }
}

static void onInfoBackdrop(lv_event_t *e) {
  (void)e;
  closeInfo();
}

static void openInfo() {
  if (g_info) return;

  g_info = lv_obj_create(lv_layer_top());            // dimmed backdrop, tap to close
  lv_obj_set_size(g_info, SCR_W, SCR_H);
  lv_obj_set_pos(g_info, 0, 0);
  lv_obj_set_style_radius(g_info, 0, 0);
  lv_obj_set_style_border_width(g_info, 0, 0);
  lv_obj_set_style_bg_color(g_info, ui_col(0x000000), 0);
  lv_obj_set_style_bg_opa(g_info, LV_OPA_70, 0);
  lv_obj_remove_flag(g_info, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(g_info, onInfoBackdrop, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *panel = lv_obj_create(g_info);
  // landscape: 3 x 4 tiles; portrait: 2 x 6 tiles
  const int pw = UI_LP(1140, 680), ph = UI_LP(660, 1000), cols = UI_LP(3, 2);
  const int tw = UI_LP(340, 290);
  lv_obj_set_size(panel, pw, ph);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, ui_col(COL_PANEL), 0);
  lv_obj_set_style_radius(panel, 32, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ic = newLabel(panel, ICON_INFO, ICON_FONT, COL_TEXT);
  lv_obj_set_pos(ic, 40, 30);
  lv_obj_t *title = newLabel(panel, tr(S_INFO_TITLE), FONT_32, COL_TEXT);
  lv_obj_set_pos(title, 96, 26);

  lv_obj_t *close = newBox(panel);                   // close button
  lv_obj_set_size(close, 72, 60);
  lv_obj_set_pos(close, pw - 40 - 72, 18);
  lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
  makeClickable(close, BTN_INFO);
  lv_obj_center(newLabel(close, LV_SYMBOL_CLOSE, FONT_32, COL_TEXT));

  // Tiles
  int rows = 0;
  for (int i = 0; i < INFO_COUNT; i++) {
    int col = i % cols, row = i / cols;
    if (row + 1 > rows) rows = row + 1;
    lv_obj_t *tile = newBox(panel);
    lv_obj_set_size(tile, tw, 112);
    lv_obj_set_pos(tile, 40 + col * (tw + 20), 96 + row * 122);
    lv_obj_set_style_bg_color(tile, ui_col(COL_TILE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 22, 0);
    newLabel(tile, tr(INFO_CAPTIONS[i]), FONT_20, COL_MUTED);   // caption (top left)
    lv_obj_t *cap = lv_obj_get_child(tile, 0);
    lv_obj_set_pos(cap, 20, 12);
    g_infoVal[i] = newLabel(tile, "", FONT_24, COL_TEXT);
    lv_obj_set_width(g_infoVal[i], tw - 40);
    lv_obj_set_pos(g_infoVal[i], 20, 42);
  }

  char safety[640];
  snprintf(safety, sizeof(safety), tr(S_INFO_SAFETY), tempText(HEAT_OVER_TEMP));
  lv_obj_t *note = newLabel(panel, safety, FONT_20, COL_MUTED);
  lv_obj_set_width(note, pw - 80);
  lv_obj_set_pos(note, 40, 96 + rows * 122 + 2);

  refreshInfo(millis());
}

/* ------------------------------------------------------------------ */
/* Home page navigation                                                */
/* ------------------------------------------------------------------ */

void hvac_set_home_callback(void (*callback)()) {
  g_homeCb = callback;
}

void hvac_open() {
  if (!g_uiReady) return;
  lv_screen_load(g_scr);
}

static void onHomeTile(lv_event_t *e) {
  (void)e;
  hvac_open();
}

// Placeholder home page: used until the application registers its own one
static void buildHomeScreen() {
  g_homeScr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_homeScr, ui_col(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_homeScr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(g_homeScr, LV_OBJ_FLAG_SCROLLABLE);

  newLabel(g_homeScr, tr(S_HOME), FONT_32, COL_TEXT);
  lv_obj_set_pos(lv_obj_get_child(g_homeScr, 0), 28, 40);

  lv_obj_t *tile = newBox(g_homeScr);
  lv_obj_set_size(tile, 470, 150);
  lv_obj_set_pos(tile, 28, 130);
  lv_obj_set_style_radius(tile, 36, 0);
  lv_obj_set_style_bg_color(tile, ui_col(COL_TILE), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(tile, ui_col(0x3A3F47), LV_STATE_PRESSED);
  lv_obj_add_event_cb(tile, onHomeTile, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *circle = newBox(tile);
  lv_obj_set_size(circle, 80, 80);
  lv_obj_set_pos(circle, 24, 35);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(circle, ui_col(0x1B4466), 0);
  lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
  lv_obj_center(newLabel(circle, ICON_HOME_TEMP, ICON_FONT, 0x4FC3F7));

  lv_obj_t *tl = newLabel(tile, tr(S_CLIMATE), FONT_32, COL_TEXT);
  lv_obj_set_pos(tl, 128, 38);
  lv_obj_t *sl = newLabel(tile, tr(S_PH_HVAC_SUB), FONT_20, COL_MUTED);
  lv_obj_set_pos(sl, 128, 88);

  lv_obj_t *hint = newLabel(g_homeScr, tr(S_PH_HVAC_HINT), FONT_20, COL_MUTED);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

// Returns to the home page (close button)
void hvac_go_home() {
  if (!g_uiReady) return;
  closePicker();
  closeInfo();
  if (g_homeCb) {
    g_homeCb();
    return;
  }
  if (!g_homeScr) buildHomeScreen();
  lv_screen_load(g_homeScr);
}

/* ------------------------------------------------------------------ */
/* Button events                                                       */
/* ------------------------------------------------------------------ */

static void onButton(lv_event_t *e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  if (id >= 100 && id < 100 + MODE_COUNT) {          // mode buttons
    setMode((Mode)(id - 100));
    return;
  }
  switch ((ButtonId)id) {
    case BTN_HEAT_MINUS: changeHeat(-TEMP_STEP); break;
    case BTN_HEAT_PLUS:  changeHeat(+TEMP_STEP); break;
    case BTN_COOL_MINUS: changeCool(-TEMP_STEP); break;
    case BTN_COOL_PLUS:  changeCool(+TEMP_STEP); break;
    case BTN_HUM_MINUS:  changeHum(-HUM_STEP);   break;
    case BTN_HUM_PLUS:   changeHum(+HUM_STEP);   break;
    case BTN_POWER:      togglePower();          break;
    case BTN_QUICK:      handleQuick();          break;
    case BTN_INFO:       if (g_info) closeInfo(); else openInfo(); break;
    case BTN_CLOSE:      hvac_go_home();         break;
    case BTN_FAN_PICK:   openFanPicker();        break;
  }
}

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

// Builds the HVAC screen in the active language. Called once at start-up and
// again by hvac_relocalize() after the language was changed.
static void buildScreen() {
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, ui_col(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  // The screen is not loaded here: Home -> Indoor tile -> ui.show(PAGE_HVAC)

  /* ---- Header: common top bar of the project (clock + date, Wi-Fi icon, X -> Home) ---- */
  lv_obj_t *bar = menu_create(g_scr, tr(S_CLIMATE), LV_SYMBOL_HOME, MENU_SUBPAGE, ui_col(0x2B2B2B));
  // The HVAC icon (changes with the mode) is shown in place of the bar icon
  lv_obj_add_flag(lv_obj_get_child(bar, 0), LV_OBJ_FLAG_HIDDEN);
  g_headIconCircle = newBox(bar);
  lv_obj_set_size(g_headIconCircle, 68, 68);
  lv_obj_align(g_headIconCircle, LV_ALIGN_LEFT_MID, 28, 4);
  lv_obj_set_style_radius(g_headIconCircle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_headIconCircle, ui_col(0x2B2B2B), 0);
  lv_obj_set_style_bg_opa(g_headIconCircle, LV_OPA_COVER, 0);
  g_headIcon = newLabel(g_headIconCircle, ICON_AUTO, ICON_FONT, COL_TEXT);
  lv_obj_center(g_headIcon);


  // Warning banner (sensor error / lockout / over temperature)
  // landscape: over the clock of the top bar; portrait: bottom of the card
  g_banner = newBox(g_scr);
  lv_obj_set_size(g_banner, 640, UI_LP(84, 72));
  if (ui_portrait()) lv_obj_set_pos(g_banner, 40, CARD_Y + CARD_H - 72 - 18);
  else               lv_obj_set_pos(g_banner, 250, 24);
  lv_obj_set_style_radius(g_banner, 22, 0);
  lv_obj_set_style_bg_opa(g_banner, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(g_banner, ui_col(0x8E1B1B), 0);
  g_bannerLbl = newLabel(g_banner, "", FONT_24, COL_TEXT);
  lv_obj_set_width(g_bannerLbl, 600);
  lv_obj_set_style_text_align(g_bannerLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(g_bannerLbl);
  lv_obj_add_flag(g_banner, LV_OBJ_FLAG_HIDDEN);

  // Information button (left of the Wi-Fi icon of the top bar)
  lv_obj_t *infoBtn = newBox(g_scr);
  lv_obj_set_size(infoBtn, 64, 64);
  if (ui_portrait())                      // header row of the card, right side
    lv_obj_set_pos(infoBtn, CARD_X + CONTENT_X + CONTENT_W - 64, CARD_Y + HEAD_Y + 12);
  else
    lv_obj_set_pos(infoBtn, 944, 20);     // Wi-Fi icon starts at x = 1028, battery to its right
  lv_obj_set_style_radius(infoBtn, LV_RADIUS_CIRCLE, 0);
  makeClickable(infoBtn, BTN_INFO);
  lv_obj_center(newLabel(infoBtn, ICON_INFO, ICON_FONT, COL_TEXT));

  // Clock, Wi-Fi icon and X (Home) are part of the common top bar (menu.cpp).
  // The warning banner covers the clock of the top bar while it is shown.
  g_clock = nullptr;
  g_wifi  = nullptr;

  /* ---- Main card ---- */
  g_card = newBox(g_scr);
  lv_obj_set_size(g_card, CARD_W, CARD_H);
  lv_obj_set_pos(g_card, CARD_X, CARD_Y);
  lv_obj_set_style_radius(g_card, 28, 0);
  lv_obj_set_style_bg_opa(g_card, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(g_card, ui_col(COL_CARD), 0);

  lv_obj_t *circle = newBox(g_card);
  lv_obj_set_size(circle, 88, 88);
  lv_obj_set_pos(circle, CONTENT_X, HEAD_Y);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(circle, 50, 0);
  g_cardIcon = circle;                               // recolored with the palette
  lv_obj_t *homeIcon = newLabel(circle, ICON_HOME_TEMP, ICON_FONT, COL_TEXT);
  lv_obj_center(homeIcon);
  lv_obj_set_user_data(circle, homeIcon);

  lv_obj_t *name = newLabel(g_card, "HVAC", FONT_32, COL_TEXT);
  lv_obj_set_pos(name, 163, HEAD_Y + 22);

  // Heating / cooling animation (flame / snowflake) next to the title
  g_animIcon = newLabel(g_card, ANIM_FLAME, ANIM_FONT, COL_ACTIVE);
  lv_obj_set_size(g_animIcon, 64, 64);
  lv_obj_set_style_text_align(g_animIcon, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_transform_pivot_x(g_animIcon, 32, 0);
  lv_obj_set_style_transform_pivot_y(g_animIcon, 32, 0);
  lv_obj_set_pos(g_animIcon, 290, HEAD_Y + 12);
  lv_obj_add_flag(g_animIcon, LV_OBJ_FLAG_HIDDEN);

  // Power and Quick buttons (top right of the card)
  g_pwrBtn = newPill(g_card, CONTENT_X + CONTENT_W - PILL_W, PILL_Y, PILL_W, UI_LP(60, 70), ICON_POWER, tr(S_OFF), BTN_POWER, &g_pwrIcon, &g_pwrLbl);
  g_quickBtn = newPill(g_card, CONTENT_X + CONTENT_W - PILL_W - 16 - PILL_W, PILL_Y, PILL_W, UI_LP(60, 70), ICON_BOLT, tr(S_QUICK), BTN_QUICK, &g_quickIcon, &g_quickLbl);

  // Status caption (COOLING / HEATING / ...) and measurement line
  g_status = newLabel(g_card, tr(S_OFF_CAP), FONT_28, COL_MUTED);
  lv_obj_set_style_text_letter_space(g_status, 4, 0);
  lv_obj_align(g_status, LV_ALIGN_TOP_MID, 0, STATUS_Y);

  g_now = newLabel(g_card, "", FONT_28, COL_MUTED);
  lv_obj_align(g_now, LV_ALIGN_TOP_MID, 0, NOW_Y);

  // Target blocks
  g_blk[0] = newBlock(g_card, tr(S_CAP_HEAT), BTN_HEAT_MINUS, BTN_HEAT_PLUS);
  g_blk[1] = newBlock(g_card, tr(S_CAP_COOL), BTN_COOL_MINUS, BTN_COOL_PLUS);
  g_blk[2] = newBlock(g_card, tr(S_CAP_HUMIDITY), BTN_HUM_MINUS, BTN_HUM_PLUS);
  g_fanBlk = newBlock(g_card, tr(S_CAP_FAN), (ButtonId)0, (ButtonId)0);

  // Mode buttons
  const char *icons[MODE_COUNT] = {ICON_AUTO, ICON_FLAME, ICON_SNOW, ICON_DROP, ICON_FAN};
  const int gap = MODE_GAP, bw = (CONTENT_W - 4 * gap) / MODE_COUNT;   // 217 / 114
  for (int i = 0; i < MODE_COUNT; i++) {
    lv_obj_t *b = newBox(g_card);
    lv_obj_set_size(b, bw, MODE_H);
    lv_obj_set_pos(b, CONTENT_X + i * (bw + gap), MODE_Y);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, ui_col(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(b, 14, 0);
    lv_obj_set_style_pad_row(b, 2, 0);
    flexColumn(b);
    makeClickable(b, (ButtonId)(100 + i));
    g_modeIcon[i] = newLabel(b, icons[i], ICON_FONT, COL_MUTED);
    g_modeLbl[i] = newLabel(b, modeName((Mode)i), UI_LP(FONT_28, FONT_20), COL_TEXT);
    g_modeBtn[i] = b;
  }

  // Fan speed selector (opens a picker)
  g_fanRow = newBox(g_card);
  lv_obj_set_size(g_fanRow, CONTENT_W, FAN_H);
  lv_obj_set_pos(g_fanRow, CONTENT_X, FAN_Y);
  lv_obj_set_style_radius(g_fanRow, 24, 0);
  lv_obj_set_style_bg_color(g_fanRow, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(g_fanRow, 12, 0);
  lv_obj_set_style_border_width(g_fanRow, 2, 0);
  lv_obj_set_style_border_color(g_fanRow, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(g_fanRow, 55, 0);
  makeClickable(g_fanRow, BTN_FAN_PICK);
  g_fanRowLbl = newLabel(g_fanRow, tr(S_FAN_AUTO), FONT_32, COL_TEXT);
  lv_obj_align(g_fanRowLbl, LV_ALIGN_LEFT_MID, 36, 0);
  lv_obj_t *chev = newLabel(g_fanRow, LV_SYMBOL_DOWN, FONT_28, COL_TEXT);
  lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -30, 0);

  // Portrait: the information button and the warning banner lie on the card
  if (ui_portrait()) {
    lv_obj_move_foreground(infoBtn);
    lv_obj_move_foreground(g_banner);
  }
}

static void buildUi() {
  // Dark theme; applies to every widget created afterwards
  lv_display_t *disp = lv_display_get_default();
  lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                         lv_palette_main(LV_PALETTE_CYAN), !ui_light_theme(), LV_FONT_DEFAULT);
  lv_display_set_theme(disp, th);

  buildScreen();
  createToast();
}

/* ------------------------------------------------------------------ */
/* UI update                                                           */
/* ------------------------------------------------------------------ */

static const Palette *currentPalette(int *index) {
  const Palette *p = &PAL_OFF;
  int i = 0;
  if (!g_sensorOk || !g_power) { p = &PAL_OFF; i = 0; }
  else if (g_action == ACT_HEAT) { p = &PAL_HEAT; i = 3; }
  else if (g_action == ACT_COOL) { p = &PAL_COOL; i = 4; }
  else if (g_action == ACT_DRY)  { p = &PAL_DRY;  i = 5; }
  else {
    switch (g_mode) {
      case MODE_HEAT: p = &PAL_HEAT; i = 3; break;
      case MODE_COOL: p = &PAL_COOL; i = 4; break;
      case MODE_DRY:  p = &PAL_DRY;  i = 5; break;
      case MODE_FAN:  p = &PAL_FAN;  i = 6; break;
      default:        p = &PAL_AUTO; i = 1; break;
    }
  }
  *index = i;
  return p;
}

// Shows the blocks that belong to the selected mode
static void applyLayout() {
  if (g_lastLayoutMode == (int)g_mode) return;
  g_lastLayoutMode = (int)g_mode;

  for (int i = 0; i < 3; i++) lv_obj_add_flag(g_blk[i].root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_fanBlk.root, LV_OBJ_FLAG_HIDDEN);

  auto place = [](Block &b, int x, int w, bool autoMode, int y = BLK_Y) {
    lv_obj_set_pos(b.root, x, y);
    lv_obj_set_width(b.root, w);
    lv_obj_set_style_text_font(b.val, autoMode ? &font_hvac_mid : &font_hvac_big, 0);
    if (b.minus) {
      lv_obj_set_size(b.minus, autoMode ? 96 : 118, autoMode ? 84 : 102);
      lv_obj_set_size(b.plus,  autoMode ? 96 : 118, autoMode ? 84 : 102);
    }
    if (autoMode) lv_obj_remove_flag(b.cap, LV_OBJ_FLAG_HIDDEN);    // "HEAT" / "COOL" captions
    else          lv_obj_add_flag(b.cap, LV_OBJ_FLAG_HIDDEN);       // the status caption is enough
    lv_obj_remove_flag(b.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(b.root);
  };

  switch (g_mode) {
    case MODE_AUTO:
      place(g_blk[0], CONTENT_X, AUTO_BLK_W, true, AUTO_Y1);                          // heating target (left / top)
      place(g_blk[1], CONTENT_X + CONTENT_W - AUTO_BLK_W, AUTO_BLK_W, true, AUTO_Y2);  // cooling target (right / below)
      break;
    case MODE_HEAT: place(g_blk[0], CONTENT_X, CONTENT_W, false); break;
    case MODE_COOL: place(g_blk[1], CONTENT_X, CONTENT_W, false); break;
    case MODE_DRY:  place(g_blk[2], CONTENT_X, CONTENT_W, false); break;
    case MODE_FAN:  place(g_fanBlk, CONTENT_X, CONTENT_W, false); break;
    default: break;
  }
}

/* ---- Heating / cooling animation ---- */
static void animFlameCb(void *o, int32_t v) {      // flicker: scale + vertical wobble
  lv_obj_t *l = (lv_obj_t *)o;
  lv_obj_set_style_transform_scale(l, 256 + v / 4, 0);
  lv_obj_set_style_translate_y(l, -v / 40, 0);
  lv_obj_set_style_text_opa(l, (lv_opa_t)(255 - (v > 500 ? 40 : v / 14)), 0);
}
static void animDropCb(void *o, int32_t v) {       // droplet: gentle fall + fade, then repeat
  lv_obj_set_style_translate_y((lv_obj_t *)o, -8 + v * 16 / 1000, 0);
  lv_obj_set_style_text_opa((lv_obj_t *)o, (lv_opa_t)(v < 150 ? 255 * v / 150 : (v > 750 ? 255 * (1000 - v) / 250 : 255)), 0);
}
static void animSnowCb(void *o, int32_t v) {       // slow rotation (0.1 deg units)
  lv_obj_set_style_transform_rotation((lv_obj_t *)o, v, 0);
}

// Starts the animation 'kind' on a 64x64 icon label (HVAC card or Home tile)
static void applyAnim(lv_obj_t *icon, int kind) {
  lv_anim_delete(icon, nullptr);
  lv_obj_set_style_transform_scale(icon, 256, 0);
  lv_obj_set_style_transform_rotation(icon, 0, 0);
  lv_obj_set_style_translate_y(icon, 0, 0);
  lv_obj_set_style_text_opa(icon, LV_OPA_COVER, 0);
  if (kind == 0) { lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN); return; }
  static const char *const ic[5] = {"", ANIM_FLAME, ANIM_SNOW, ANIM_DROP, ANIM_FAN};
  static const uint32_t col[5] = {0, 0xFF8A00, 0x4FC3F7, 0x80DEEA, 0xB0BEC5};
  setText(icon, ic[kind]);
  lv_obj_set_style_text_color(icon, ui_col(col[kind]), 0);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, icon);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  if (kind == 1) {
    lv_anim_set_exec_cb(&a, animFlameCb);
    lv_anim_set_values(&a, 0, 640);                // scale 100%..163%, wobble
    lv_anim_set_duration(&a, 650);
    lv_anim_set_playback_duration(&a, 650);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  } else if (kind == 3) {
    lv_anim_set_exec_cb(&a, animDropCb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
  } else {
    lv_anim_set_exec_cb(&a, animSnowCb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_duration(&a, kind == 4 ? 1100 : 6000);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
  }
  lv_anim_start(&a);
}

// Animation that matches the real output state:
// 0 none, 1 flame (heating), 2 snowflake (cooling), 3 droplet (dry), 4 fan
static int currentAnimKind() {
  return !(g_power && g_sensorOk) ? 0 :
         g_action == ACT_FAN ? 4 :
         g_lock ? 0 :
         g_action == ACT_HEAT ? 1 : g_action == ACT_COOL ? 2 : g_action == ACT_DRY ? 3 : 0;
}

static void setAnim(int kind) {
  if (kind == g_animKind) return;
  g_animKind = kind;
  applyAnim(g_animIcon, kind);
}

void hvac_set_wifi_status(int level) {
  if (level < 0) level = 0;
  if (level > 4) level = 4;
  g_wifiLevel = level;
  if (g_uiReady) setText(g_wifi, WIFI_ICONS[level]);
}

static void refreshUi(uint32_t now) {
  g_uiDirty = false;
  g_lastUiMs = now;
  char b[96];

  // Palette (card gradient + accent color)
  int pi;
  const Palette *pal = currentPalette(&pi);
  if (pi != g_lastPalette) {
    g_lastPalette = pi;
    lv_obj_set_style_bg_color(g_cardIcon, ui_col(pal->accent), 0);
    lv_obj_set_style_text_color((lv_obj_t *)lv_obj_get_user_data(g_cardIcon), ui_col(pal->accent), 0);
  }
  uint32_t accent = pal->accent;

  applyLayout();

  // Status caption
  setText(g_status, stateText());
  bool active = g_sensorOk && g_power && g_action != ACT_NONE;
  lv_obj_set_style_text_color(g_status, ui_col(!g_sensorOk ? COL_ERR : (active ? accent : COL_MUTED)), 0);

  // Target values
  snprintf(b, sizeof(b), "%.1f" DEG, dispT(g_heatSp));  setText(g_blk[0].val, b);
  snprintf(b, sizeof(b), "%.1f" DEG, dispT(g_coolSp));  setText(g_blk[1].val, b);
  snprintf(b, sizeof(b), "%d%%", g_humSp);       setText(g_blk[2].val, b);

  if (g_fanSpeed == 0) {
    lv_obj_set_style_text_font(g_fanBlk.val, FONT_48, 0);
    setText(g_fanBlk.val, tr(S_AUTO));
  } else {
    lv_obj_set_style_text_font(g_fanBlk.val, &font_hvac_big, 0);
    snprintf(b, sizeof(b), "%u%%", (unsigned)FAN_PCT[g_fanSpeed - 1]);
    setText(g_fanBlk.val, b);
  }

  // Dim the targets while the HVAC is off
  lv_opa_t opa = g_power && g_sensorOk ? LV_OPA_COVER : LV_OPA_60;
  for (int i = 0; i < 3; i++) lv_obj_set_style_opa(g_blk[i].root, opa, 0);
  lv_obj_set_style_opa(g_fanBlk.root, opa, 0);

  // Measurement line
  if (!g_sensorOk) {
    snprintf(b, sizeof(b), tr(S_FMT_NOW_NA));          // "%%" -> "%"
  } else if (g_mode == MODE_DRY) {
    snprintf(b, sizeof(b), tr(S_FMT_NOW_RH_T), g_hum, dispT(g_temp));
  } else if (g_mode == MODE_FAN) {
    if (g_fanSpeed == 0) snprintf(b, sizeof(b), tr(S_FMT_FANAUTO_T_RH), dispT(g_temp), g_hum);
    else                 snprintf(b, sizeof(b), tr(S_FMT_FANSPD_T_RH), (unsigned)g_fanSpeed, dispT(g_temp), g_hum);
  } else {
    snprintf(b, sizeof(b), tr(S_FMT_NOW_T_RH), dispT(g_temp), g_hum);
  }
  setText(g_now, b);

  // Power button
  bool on = g_power && g_sensorOk;
  setText(g_pwrLbl, tr(on ? S_ON : S_OFF));
  lv_obj_set_style_bg_color(g_pwrBtn, ui_col(on ? COL_ACTIVE : 0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(g_pwrBtn, on ? LV_OPA_COVER : 18, 0);
  lv_obj_set_style_text_color(g_pwrIcon, ui_col(on ? COL_ON_ACTIVE : COL_TEXT), 0);
  lv_obj_set_style_text_color(g_pwrLbl, ui_col(on ? COL_ON_ACTIVE : COL_TEXT), 0);

  // Quick button: available in Heat / Cool / Auto mode while running
  bool quickOk = on && !g_lock && (g_mode == MODE_HEAT || g_mode == MODE_COOL || g_mode == MODE_AUTO);
  bool quickRun = g_quickRun && conditioning(g_action);
  setText(g_quickLbl, tr(quickRun ? S_STOP : S_QUICK));
  lv_obj_set_style_bg_color(g_quickBtn, ui_col(quickRun ? COL_ACTIVE : 0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(g_quickBtn, quickRun ? LV_OPA_COVER : 18, 0);
  lv_obj_set_style_text_color(g_quickLbl, ui_col(quickRun ? COL_ON_ACTIVE : COL_TEXT), 0);
  lv_obj_set_style_opa(g_quickBtn, quickOk ? LV_OPA_COVER : LV_OPA_50, 0);
  lv_obj_set_style_text_color(g_quickIcon, ui_col(quickRun ? COL_ON_ACTIVE : (quickOk ? accent : COL_MUTED)), 0);

  // Mode buttons
  for (int i = 0; i < MODE_COUNT; i++) {
    bool sel = (i == (int)g_mode);
    lv_obj_set_style_bg_color(g_modeBtn[i], ui_col(sel ? COL_ACTIVE : 0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_modeBtn[i], sel ? LV_OPA_COVER : 14, 0);
    lv_obj_set_style_text_color(g_modeIcon[i], ui_col(sel ? COL_ON_ACTIVE : COL_MUTED), 0);
    lv_obj_set_style_text_color(g_modeLbl[i], ui_col(sel ? COL_ON_ACTIVE : COL_TEXT), 0);
  }

  // Fan selector text
  if (g_fanSpeed == 0) setText(g_fanRowLbl, tr(S_FAN_AUTO));
  else { snprintf(b, sizeof(b), tr(S_FMT_FAN_SPEED), (unsigned)g_fanSpeed); setText(g_fanRowLbl, b); }

  // Warning banner
  const char *msg = nullptr;
  uint32_t bcol = 0x8E1B1B;
  char bm[160];
  if (!g_sensorOk) {
    msg = tr(S_B_SENSOR);
  } else if (g_lock) {
    snprintf(bm, sizeof(bm), tr(S_B_FMT_LOCK), (unsigned long)((g_lockUntilMs - now) / 60000UL + 1));
    msg = bm;
    bcol = 0x7A5A0C;
  } else if (g_power && g_temp > HEAT_OVER_TEMP && (g_mode == MODE_HEAT || g_mode == MODE_AUTO)) {
    snprintf(bm, sizeof(bm), tr(S_B_OVER_TEMP), tempText(HEAT_OVER_TEMP)); msg = bm;
    bcol = 0x7A5A0C;
  }
  if (msg) {
    setText(g_bannerLbl, msg);
    lv_obj_set_style_bg_color(g_banner, ui_col(bcol), 0);
    lv_obj_remove_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_banner);
  } else {
    lv_obj_add_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
  }

  // Heating / cooling animation follows the real output state
  setAnim(currentAnimKind());

  refreshInfo(now);
}

// Clock: shown by the common top bar (menu.cpp)
static void clockTask(uint32_t now) {
  // The clock is shown by the common top bar (menu.cpp)
  (void)now;
}

// Values shared with the Home page (g_data)
static void shareTask() {
  g_data.indoor_temp = g_sensorOk ? g_temp : NAN;
  g_data.indoor_hum  = g_sensorOk ? g_hum  : NAN;
  g_data.hvac_mode   = (uint8_t)g_mode;
  bool coolSide = (g_mode == MODE_COOL) || (g_mode == MODE_AUTO && g_action == ACT_COOL);
  g_data.hvac_setpoint = coolSide ? g_coolSp : g_heatSp;
}

// Periodic work: sensor, control logic, storage, clock, UI (LVGL timer, 100 ms)
static void hvacTimerCb(lv_timer_t *t) {
  (void)t;
  uint32_t now = millis();
  { HSTAGE("hvac sensor"); sensorTask(now); }
  HSTAGE("hvac");
  evaluate(now);
  saveTask(now);
  clockTask(now);
  toastTask(now);
  shareTask();
  if (g_uiDirty || (now - g_lastUiMs) >= UI_REFRESH_MS) refreshUi(now);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

void hvac_control() {
  if (g_uiReady) return;

  // 1) Every output to 0, then attach the PWM channels (still 0)
  hvac_pins_safe();
  pwmInit();
  g_action = ACT_NONE;
  g_power  = false;

  // 2) Stored settings (mode, targets, fan speed, on / off)
  loadSettings();

  // 3) Sensor on Port A. Release M5Unified's external I2C first so that
  //    Wire can own the bus.
  M5.Ex_I2C.release();
  Wire.begin(SENSOR_SDA_PIN, SENSOR_SCL_PIN, 100000);
  Wire.setTimeOut(50);
  if (sensorBegin()) readSensor();
  else               g_sensorFails = SENSOR_FAIL_LIMIT;
  if (!g_sensorOk)   g_sensorFails = SENSOR_FAIL_LIMIT;
  g_lastSensorMs = g_lastRetryMs = millis();

  // 4) Screen
  buildUi();
  g_uiReady = true;
  refreshUi(millis());
  lv_timer_create(hvacTimerCb, 100, nullptr);
}

/* ------------------------------------------------------------------ */
/* Tab5 Home project integration                                       */
/* ------------------------------------------------------------------ */

lv_obj_t *hvac_create() {
  if (!g_uiReady) hvac_control();
  g_uiDirty = true;
  return g_scr;
}

lv_obj_t *hvac_screen() { return g_scr; }

void hvac_hide_popups() {
  closePicker();
  closeInfo();
}

// Home "Quick" button: same as the Quick button on the HVAC screen
void quick() {
  if (!g_uiReady) return;
  handleQuick();
  g_uiDirty = true;
}

bool hvac_sensor_ok() { return g_sensorOk; }

const char *hvac_summary() {
  static char b[48];
  if (!g_sensorOk) return tr(S_SENSOR_ERROR);
  if (!g_power)    return tr(S_OFF);
  static const StrId st[] = {S_ST_STANDBY, S_ST_HEATING, S_ST_COOLING, S_ST_DRYING, S_ST_FAN};
  const char *s = tr(g_lock ? S_ST_LOCKED : st[g_action]);
  snprintf(b, sizeof(b), "%s  -  %s", modeName(g_mode), s);
  return b;
}

const char *hvac_target_text() {
  static char b[48];
  switch (g_mode) {
    case MODE_AUTO: snprintf(b, sizeof(b), tr(S_FMT_HEAT_COOL), dispT(g_heatSp), dispT(g_coolSp)); break;
    case MODE_HEAT: snprintf(b, sizeof(b), tr(S_FMT_TARGET_T), dispT(g_heatSp), settings_temp_unit()); break;
    case MODE_COOL: snprintf(b, sizeof(b), tr(S_FMT_TARGET_T), dispT(g_coolSp), settings_temp_unit()); break;
    case MODE_DRY:  snprintf(b, sizeof(b), tr(S_FMT_TARGET_RH), g_humSp); break;
    default:
      if (g_fanSpeed == 0) snprintf(b, sizeof(b), "%s", tr(S_FAN_AUTO));
      else                 snprintf(b, sizeof(b), tr(S_FMT_FAN_SPEED), (unsigned)g_fanSpeed);
      break;
  }
  return b;
}

void hvac_apply_setpoint(float sp) {
  if (!g_uiReady || sp <= 0.0f) return;
  sp = roundf(sp * 2.0f) / 2.0f;
  if (g_mode == MODE_COOL) {
    g_coolSp = clampf(sp, COOL_MIN, COOL_MAX);
  } else {
    g_heatSp = clampf(sp, HEAT_MIN, HEAT_MAX);
    if (g_mode == MODE_AUTO) enforceGap(true);
  }
  markTargetsChanged();
  evaluate(millis());
}

// Rebuilds the HVAC screen in the new language (called by ui_relocalize() after
// the language was changed in Settings). The control logic keeps running.
void hvac_relocalize() {
  if (!g_uiReady) return;
  closePicker();
  closeInfo();
  lv_obj_t *old = g_scr;
  bool active = (lv_screen_active() == old);
  g_animKind = 0;                        // forces the caches to be re-applied
  g_lastPalette = -1;
  g_lastLayoutMode = -1;
  if (g_toast) { lv_obj_delete(g_toast); g_toast = nullptr; }
  createToast();                         // top layer: rebuilt for the theme colours
  buildScreen();
  hvac_set_wifi_status(g_wifiLevel);
  if (active) lv_screen_load(g_scr);
  if (old) lv_obj_delete_async(old);     // its animations are deleted with it
  refreshUi(millis());
}

// ---- HVAC animation for other pages (Home tile) ----
// The current kind is kept in the user data of the label (+1, 0 = not set)
lv_obj_t *hvac_anim_create(lv_obj_t *parent) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, ANIM_FONT, 0);
  lv_obj_set_size(l, 64, 64);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_transform_pivot_x(l, 32, 0);
  lv_obj_set_style_transform_pivot_y(l, 32, 0);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_user_data(l, (void *)(intptr_t)0);
  return l;
}

void hvac_anim_update(lv_obj_t *icon) {
  if (!icon) return;
  int kind = currentAnimKind();
  if ((intptr_t)lv_obj_get_user_data(icon) == kind + 1) return;   // already running
  lv_obj_set_user_data(icon, (void *)(intptr_t)(kind + 1));
  applyAnim(icon, kind);
}

// ---- Remote control (blynk.cpp). Called from the LVGL / main thread only.

bool hvac_power() { return g_power; }

bool hvac_set_power(bool on) {
  if (!g_uiReady) return false;
  return setPower(on, false);
}

// Changes the temperature target of the active mode by delta (Celsius):
// Heat -> heating target, Cool -> cooling target, Auto -> both (the gap is
// kept). Dry / Fan have no temperature target. Saved like a screen change.
bool hvac_change_target(float delta) {
  if (!g_uiReady || delta == 0.0f) return false;
  switch (g_mode) {
    case MODE_HEAT: {
      float n = clampf(g_heatSp + delta, HEAT_MIN, HEAT_MAX);
      if (n == g_heatSp) return false;
      g_heatSp = n;
      break;
    }
    case MODE_COOL: {
      float n = clampf(g_coolSp + delta, COOL_MIN, COOL_MAX);
      if (n == g_coolSp) return false;
      g_coolSp = n;
      break;
    }
    case MODE_AUTO: {
      float h = g_heatSp + delta, c = g_coolSp + delta;
      if (h < HEAT_MIN || h > HEAT_MAX || c < COOL_MIN || c > COOL_MAX) return false;
      g_heatSp = h;
      g_coolSp = c;
      break;
    }
    default:
      return false;
  }
  markTargetsChanged();
  evaluate(millis());
  return true;
}

bool hvac_heating() { return g_power && g_action == ACT_HEAT; }
bool hvac_cooling() { return g_power && g_action == ACT_COOL; }
bool hvac_running() { return g_power && g_action != ACT_NONE; }   // any action (heat, cool, dry, fan)
