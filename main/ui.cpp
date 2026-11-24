// ============================================================================
//  ui.cpp  -  Application core
//   * ui.early()           : every output to 0 at the very start of setup()
//   * ui.init()            : data, settings (Wi-Fi, time, language), hardware,
//                            HVAC, weather, first page
//   * ui.show(page)        : page switching
//   * ui_alarm_trigger()   : opens the alarm page when an alarm event arrives
//   * data_load/data_save  : g_data <-> Tab5 NVS
// ============================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "ui.h"
#include "home.h"
#include "menu.h"
#include "weather.h"
#include "settings.h"    // settings(): Wi-Fi, screen sleep, brightness, time zone, language
#include "hvac.h"        // hvac_control(), hvac_create(), quick()
#include "lights.h"
#include "rgb.h"
#include "scene.h"
#include "alarm.h"
#include "aq.h"          // ENS160 air quality
#include "clock.h"        // clock styles (Home clock tile)
#include "blynk.h"       // Blynk IoT (no LVGL)
#include "splash.h"      // start-up screen
#include "udp_th.h"      // outdoor unit (UDP) + g_outdoor
#include "udp_alarm.h"   // fire / smoke / gas alarm unit (UDP)
#include "sysinfo.h"     // CPU load, monthly HVAC run time
#include "alarm_sound.h" // sirens with their own volume
#include "camera_mail.h" // photo by e-mail on a motion alarm
#include "health.h"         // stage marker (watchdog diagnosis)

UI ui;

static Preferences   prefs;
static bool          s_dirty      = false;
static uint32_t      s_dirty_ms   = 0;
static volatile bool s_alarm_req  = false;
static char          s_alarm_reason[64] = "";
static volatile uint8_t s_alarm_src = 0;            // AlarmSource bits of the ringing alarm

// ============================================================ PERSISTENT DATA
void data_defaults() {
  memset(&g_data, 0, sizeof(g_data));
  g_data.magic   = APP_DATA_MAGIC;
  g_data.version = APP_DATA_VERSION;
  strlcpy(g_data.wifi_ssid,   DEF_WIFI_SSID,   sizeof(g_data.wifi_ssid));
  strlcpy(g_data.wifi_pass,   DEF_WIFI_PASS,   sizeof(g_data.wifi_pass));
  strlcpy(g_data.owm_api_key, DEF_OWM_API_KEY, sizeof(g_data.owm_api_key));
  strlcpy(g_data.city,        DEF_CITY,        sizeof(g_data.city));
  g_data.latitude       = DEF_LATITUDE;
  g_data.longitude      = DEF_LONGITUDE;
  g_data.tz_offset      = DEF_TZ_OFFSET;
  g_data.brightness     = 204;     // owned by settings.cpp (copy)
  g_data.screen_timeout = 300;     // owned by settings.cpp (copy)
  g_data.indoor_temp    = 22.0f;
  g_data.indoor_hum     = 45.0f;
  g_data.hvac_setpoint  = 22.0f;
  g_data.hvac_mode      = 0;
  g_data.outdoor_temp   = NAN;
  strlcpy(g_data.weather_desc, "--", sizeof(g_data.weather_desc));
  strlcpy(g_data.weather_icon, "01d", sizeof(g_data.weather_icon));
  for (int i = 0; i < BLIND_COUNT; i++) { g_data.blind_pos[i] = 0; g_data.blind_target[i] = 0; }
  g_data.blind_travel_s = 20;
  g_data.rgb_on = false;
  g_data.rgb_r = 255; g_data.rgb_g = 160; g_data.rgb_b = 80;
  g_data.rgb_brightness = 70;
  g_data.rgb_effect = 0;
  g_data.scene_id   = 0;
  g_data.alarm_armed = false;
  strlcpy(g_data.alarm_pin, DEF_ALARM_PIN, sizeof(g_data.alarm_pin));
}

void data_load() {
  prefs.begin("tab5home", true);
  size_t n = prefs.getBytesLength("app");
  bool ok = false;
  if (n == sizeof(AppData)) {
    prefs.getBytes("app", &g_data, sizeof(AppData));
    ok = (g_data.magic == APP_DATA_MAGIC && g_data.version == APP_DATA_VERSION);
  }
  prefs.end();
  if (!ok) { data_defaults(); data_save(); }

  // The weather settings cannot be changed on the screen, so they are taken
  // from the DEF_* values of app_data.h on every start (overrides old NVS data)
  strlcpy(g_data.owm_api_key, DEF_OWM_API_KEY, sizeof(g_data.owm_api_key));
  strlcpy(g_data.city,        DEF_CITY,        sizeof(g_data.city));
  g_data.latitude  = DEF_LATITUDE;
  g_data.longitude = DEF_LONGITUDE;

  // reset the runtime fields
  g_data.alarm_active = false;
  g_data.motion       = false;
  g_data.wifi_rssi    = 0;
  for (int i = 0; i < BLIND_COUNT; i++) g_data.blind_target[i] = g_data.blind_pos[i];
}

void data_save() {
  prefs.begin("tab5home", false);
  prefs.putBytes("app", &g_data, sizeof(AppData));
  prefs.end();
  s_dirty = false;
}

void data_mark_dirty() { s_dirty = true; s_dirty_ms = millis(); }

// ============================================================ HELPERS
lv_obj_t* ui_screen_create() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

lv_obj_t* ui_tile_create(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t bg) {
  lv_obj_t* t = lv_obj_create(parent);
  lv_obj_remove_style_all(t);
  lv_obj_set_pos(t, x, y);
  lv_obj_set_size(t, w, h);
  lv_obj_set_style_bg_color(t, bg, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t, 28, 0);
  lv_obj_set_style_border_width(t, 1, 0);
  lv_obj_set_style_border_color(t, ui_col(0x3A3A40), 0);
  lv_obj_set_style_pad_all(t, 18, 0);
  // press effect
  lv_obj_set_style_transform_scale(t, 245, LV_STATE_PRESSED);
  lv_obj_set_style_transform_pivot_x(t, w / 2, 0);
  lv_obj_set_style_transform_pivot_y(t, h / 2, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  return t;
}

lv_obj_t* ui_label(lv_obj_t* parent, const char* txt, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

lv_obj_t* ui_icon_circle(lv_obj_t* parent, const char* symbol, int size, lv_color_t bg, lv_color_t fg) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, size, size);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(c, bg, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = lv_label_create(c);
  lv_label_set_text(l, symbol);
  lv_obj_set_style_text_color(l, fg, 0);
  lv_obj_set_style_text_font(l, size >= 64 ? FONT_32 : FONT_24, 0);
  lv_obj_center(l);
  return c;
}

static void timer_del_cb(lv_event_t* e) {
  lv_timer_t* t = (lv_timer_t*)lv_event_get_user_data(e);
  if (t) lv_timer_delete(t);
}

lv_timer_t* ui_page_timer(lv_obj_t* scr, lv_timer_cb_t cb, uint32_t period, void* user) {
  lv_timer_t* t = lv_timer_create(cb, period, user);
  lv_obj_add_event_cb(scr, timer_del_cb, LV_EVENT_DELETE, t);
  return t;
}

void ui_back_home_cb(lv_event_t* e) { ui.show(PAGE_HOME); }

// Touch activity: the screen sleep is handled by settings.cpp
void ui_user_activity() { settings_wake(false); }
void ui_touch_click()    { settings_touch_click(); }

bool ui_time_valid() { return time(nullptr) > 1700000000; }

// ============================================================ ALARM
static uint32_t s_alarm_seq = 0;

void ui_alarm_trigger_src(const char* reason, uint8_t sources) {
  strlcpy(s_alarm_reason, reason ? reason : "", sizeof(s_alarm_reason));
  if (!g_data.alarm_active) s_alarm_src = 0;
  s_alarm_src |= sources;      // the siren tone follows the sources (alarm_sound.cpp)
  g_data.alarm_active = true;
  s_alarm_seq++;
  s_alarm_req = true;          // the page switch itself runs in the LVGL timer
}
void ui_alarm_trigger(const char* reason) { ui_alarm_trigger_src(reason, ASRC_MOTION); }
uint32_t ui_alarm_seq() { return s_alarm_seq; }
uint8_t  ui_alarm_sources() { return g_data.alarm_active ? s_alarm_src : 0; }

void ui_alarm_disarm_all() {
  g_data.alarm_active = false;
  s_alarm_src = 0;
  M5.Speaker.stop();
  if (g_data.alarm_armed) { g_data.alarm_armed = false; data_mark_dirty(); }
  udp_alarm_set_safety_armed(false);
}
const char* ui_alarm_reason() { return s_alarm_reason[0] ? s_alarm_reason : tr(S_ALARM); }

// Motion alarm only (Blynk V11). A ringing alarm is stopped by ui_alarm_disarm_all().
void ui_alarm_set_armed(bool armed) {
  if (g_data.alarm_armed != armed) {
    g_data.alarm_armed = armed;
    data_mark_dirty();                              // saved to NVS like a keypad change
  }
}
void UI::alarm(const char* reason) { ui_alarm_trigger(reason); }

// ============================================================ LANGUAGE
// Settings calls this after the language was changed. Settings rebuilds its own
// screen; pages that are created on every visit need nothing.
void ui_relocalize() {
  hvac_relocalize();                               // persistent HVAC screen
}

// ============================================================ PAGE SWITCHING
void UI::show(PageId page) {
  // The HVAC and Settings screens are persistent: they are not deleted when
  // they are left, only their open overlays (pickers, keyboard, info) close
  lv_obj_t* old = lv_screen_active();
  bool leaving_hvac = (old != nullptr && old == hvac_screen());
  bool leaving_set  = (old != nullptr && old == settings_screen());
  if (leaving_hvac) hvac_hide_popups();
  if (leaving_set)  settings_hide_popups();

  lv_obj_t* scr = nullptr;
  switch (page) {
    case PAGE_HOME:     scr = home_create();     break;
    case PAGE_WEATHER:  scr = weather_create();  break;
    case PAGE_SETTINGS: scr = settings_create(); break;
    case PAGE_HVAC:     scr = hvac_create();     break;
    case PAGE_LIGHTS:   scr = lights_create();   break;
    case PAGE_RGB:      scr = rgb_create();      break;
    case PAGE_SCENE:    scr = scene_create();    break;
    case PAGE_ALARM:    scr = alarm_create();    break;
    case PAGE_AQ:       scr = aq_create();       break;
    case PAGE_CLOCK:    scr = clock_create();    break;
  }
  if (!scr) return;
  if (scr == old) return;
  _page = page;
  // Instant switch (no transition effect: a full-screen fade is slow with
  // software rendering). The old screen is deleted, except the persistent ones.
  lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, !(leaving_hvac || leaving_set));
}

// ============================================================ BACKGROUND WORK
// 100 ms: alarm request, PIR, blinds and RGB effect engine
static void fast_timer_cb(lv_timer_t* t) {
  { HSTAGE("udp"); udp_th_update(); udp_alarm_update(); }   // outdoor unit, fire / smoke / gas unit
  HSTAGE("blynk");
  blynk_process();                                  // Blynk commands -> HVAC (main thread)
  if (s_alarm_req) {
    s_alarm_req = false;
    settings_wake(true);
    if (ui.current() != PAGE_ALARM) ui.show(PAGE_ALARM);
  }
  if (g_data.alarm_active) settings_wake(true);     // the screen stays on during an alarm

  // Motion sensor: can be switched off in Settings > System
  bool m = settings_motion_enabled() && digitalRead(PIN_PIR) == HIGH;
  if (m != g_data.motion) {
    g_data.motion = m;
    if (m) {
      settings_wake(true);                          // motion wakes the screen
      if (g_data.alarm_armed && !g_data.alarm_active) {
        ui_alarm_trigger_src(tr(S_MOTION_DETECTED), ASRC_MOTION);
        if (cam_enabled()) cam_request(CAM_REQ_ALARM, tr(S_MOTION_DETECTED));   // photo by e-mail
      }
    }
  }
  health_stage("siren");
  alarm_sound_tick();                               // siren pattern (own volume)
  health_stage("blinds / rgb");
  blinds_tick();
  rgb_tick();
}

// 1 s: Wi-Fi state, saving, RTC sync
// ============================================================ POWER
static PowerInfo s_power = {-1, PWR_USB};

static void power_read() {
  if (!cam_i2c_try_lock()) return;                  // the camera uses the I2C bus: next time
  int  lvl = M5.Power.getBatteryLevel();            // 0..100, < 0 = not available
  auto chg = M5.Power.isCharging();
  int  vbus = M5.Power.getVBUSVoltage();            // mV, < 0 = not measurable
  bool charging = (chg == m5::Power_Class::is_charging);
  if (chg == m5::Power_Class::charge_unknown) {     // fall back to the battery current
    int32_t cur = M5.Power.getBatteryCurrent();     // mA, > 0 = into the battery
    charging = cur > 20;
  }
  cam_i2c_unlock();
  bool usb = (vbus > 4000) || charging;
  if (lvl < 0 || lvl > 100) { s_power.level = -1; s_power.src = PWR_USB; return; }
  s_power.level = lvl;
  s_power.src   = charging ? PWR_CHARGING : usb ? PWR_USB : PWR_BATTERY;
}

const PowerInfo& ui_power() { return s_power; }

// Plain accessors for modules that cannot include ui.h (settings.cpp has its own PageId)
int ui_power_level()  { return s_power.level; }
int ui_power_source() { return (int)s_power.src; }    // PowerSource

static void slow_timer_cb(lv_timer_t* t) {
  static bool rtc_synced = false;
  static uint8_t pwr_div = 0;
  HSTAGE("power / rtc");
  if (++pwr_div >= 2) { pwr_div = 0; power_read(); } // battery every 2 s
  sysinfo_tick();                                   // CPU load, HVAC run time

  // Wi-Fi is owned by settings.cpp (connect / reconnect); it is only read here
  bool wifi_ok = settings_wifi_connected();
  g_data.wifi_rssi = wifi_ok ? settings_wifi_rssi() : 0;   // cached (SDIO request)
  {                                                  // Wi-Fi level for the HVAC module
    int r = g_data.wifi_rssi;
    int lvl = (r == 0) ? 0 : (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
    static int last_lvl = -1;
    if (lvl != last_lvl) { last_lvl = lvl; hvac_set_wifi_status(lvl); }
  }

  if (!rtc_synced && ui_time_valid() && wifi_ok && cam_i2c_try_lock()) {
    time_t now = time(nullptr);
    struct tm utc; gmtime_r(&now, &utc);
    M5.Rtc.setDateTime(&utc);                       // NTP -> hardware RTC
    cam_i2c_unlock();
    rtc_synced = true;
  }

  if (s_dirty && millis() - s_dirty_ms > 3000) data_save();
}

// ============================================================ START
// Called first in setup(): every output to 0 for a safe start
void UI::early() {
  hvac_pins_safe();                                 // heating / cooling / LEDs / PWM
  const uint8_t outs[] = {PIN_BLIND1_UP, PIN_BLIND1_DOWN, PIN_BLIND2_UP, PIN_BLIND2_DOWN};
  for (uint8_t p : outs) { pinMode(p, OUTPUT); digitalWrite(p, RELAY_ACTIVE_LOW ? HIGH : LOW); }
  const uint8_t rgb[] = {PIN_RGB_R, PIN_RGB_G, PIN_RGB_B};
  for (uint8_t p : rgb)  { pinMode(p, OUTPUT); digitalWrite(p, RGB_COMMON_ANODE ? HIGH : LOW); }
}

// Called by Settings > System > Restart just before ESP.restart():
// saves pending data and drives every output to 0
void ui_prepare_restart() {
  data_save();
  sysinfo_save();
  blinds_stop(-1);
  ui.early();
}

static void go_home_cb() { ui.show(PAGE_HOME); }

// ============================================================ START-UP SCREEN
static lv_obj_t* s_splash    = nullptr;
static uint32_t  s_splash_t0 = 0;

static void splash_done_cb(lv_timer_t* t) {
  // An alarm may already have replaced the splash (show() deleted it then)
  if (lv_screen_active() == s_splash) ui.show(PAGE_HOME);
  s_splash = nullptr;
}

void UI::init() {
  data_load();

  // Time: first from the hardware RTC; time zone and NTP are set by settings()
  M5.Rtc.setSystemTimeFromRtc();

  // Settings: loads and applies the stored settings (brightness, sleep,
  // rotation, volume, microphone, language, time zone) and connects to the
  // saved Wi-Fi network. It runs first so that every page uses the language.
  settings();
  settingsSetHomeCallback(go_home_cb);
  settingsSetLanguageCallback(ui_relocalize);

  // Start-up screen: shown now (after Settings applied the rotation and the
  // brightness) while the rest of the start-up work runs
  s_splash = splash_create();
  lv_screen_load_anim(s_splash, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);  // deletes the default screen
  lv_refr_now(nullptr);
  s_splash_t0 = millis();

  pinMode(PIN_PIR, INPUT);
  rgb_hw_init();
  blinds_hw_init();

  weather_begin();                                  // OpenWeatherMap background task
  udp_th_begin();                                   // outdoor unit values (g_outdoor)
  udp_alarm_begin();                                // fire / smoke / gas alarm unit
  alarm_sound_begin();                              // siren switches and volumes

  hvac_control();                                   // AHT10 + HVAC control + HVAC screen
  hvac_set_home_callback(go_home_cb);               // X of the HVAC screen -> Home

  aq_begin();                                       // ENS160 (needs Wire, started by hvac_control)
  sysinfo_begin();                                  // CPU load + HVAC run time (NVS)
  cam_begin();                                      // camera + e-mail task (after the I2C buses)
  blynk_begin();                                    // Blynk task (after HVAC + settings)

  power_read();                                     // battery icon has a value at once
  lv_timer_create(fast_timer_cb, 100, nullptr);
  lv_timer_create(slow_timer_cb, 1000, nullptr);

  // Home after SPLASH_TIME_MS in total (the start-up work above counts too)
  uint32_t spent = millis() - s_splash_t0;
  uint32_t wait  = spent >= SPLASH_TIME_MS ? 1 : SPLASH_TIME_MS - spent;
  lv_timer_t* t = lv_timer_create(splash_done_cb, wait, nullptr);
  lv_timer_set_repeat_count(t, 1);
}
