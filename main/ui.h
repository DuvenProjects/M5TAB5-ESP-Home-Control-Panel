// ============================================================================
//  ui.h  -  Page management, common styles, alarm trigger, persistent storage
// ============================================================================
#pragma once
#include <lvgl.h>
#include "app_data.h"
#include "lang.h"          // tr(), FONT_xx
#include "theme.h"         // ui_col(): dark / light theme colours

enum PageId {
  PAGE_HOME = 0,
  PAGE_WEATHER,
  PAGE_SETTINGS,
  PAGE_HVAC,
  PAGE_LIGHTS,      // blinds control page -> lights.cpp
  PAGE_RGB,
  PAGE_SCENE,
  PAGE_ALARM,
  PAGE_AQ,          // indoor air quality (ENS160) -> aq.cpp
  PAGE_CLOCK        // clock style selection -> clock.cpp
};

// ---------------------------------------------------------------- COLORS
#define C_BG        ui_col(0x000000)
#define C_TILE      ui_col(0x1C1C1E)
#define C_TILE2     ui_col(0x2A2A2E)
#define C_TEXT      ui_col(0xFFFFFF)
#define C_TEXT2     ui_col(0xA0A0A8)
#define C_ACCENT    ui_col(0x5AC8FA)
#define C_ORANGE    ui_col(0xFFB070)
#define C_PURPLE    ui_col(0x9C6ADE)
#define C_GREEN     ui_col(0x34C759)
#define C_RED       ui_col(0xFF453A)
#define C_YELLOW    ui_col(0xFFD60A)

class UI {
public:
  void early();                                 // start of setup(): every output to 0
  void init();                                  // main.ino -> ui.init()
  void show(PageId page);                       // switch page
  PageId current() const { return _page; }
  void alarm(const char* reason);               // an alarm event arrived
private:
  PageId _page = PAGE_HOME;
};
extern UI ui;

// Opens the alarm page directly when an alarm event arrives.
// Safe to call from any task (not from an ISR).
void ui_alarm_trigger(const char* reason);                       // motion / other
void ui_alarm_trigger_src(const char* reason, uint8_t sources);  // AlarmSource bits (alarm_sound.h)
uint8_t ui_alarm_sources();                                      // sources of the ringing alarm
const char* ui_alarm_reason();
// Arms / disarms the motion alarm without the PIN (Blynk V11). Main thread only.
void ui_alarm_set_armed(bool armed);
// Switches every alarm off (motion + fire / smoke / gas) and stops a ringing
// alarm: PIN on the Alarm page, "disarm all" button in Blynk. Main thread only.
void ui_alarm_disarm_all();
// Incremented on every alarm trigger (Blynk sends one notification per trigger)
uint32_t ui_alarm_seq();

// Called after the language was changed in Settings: rebuilds the persistent
// screens (HVAC) so that every text uses the new language.
void ui_relocalize();

// Helpers --------------------------------------------------------------------
lv_obj_t* ui_screen_create();                               // empty black screen
lv_obj_t* ui_tile_create(lv_obj_t* parent, int x, int y, int w, int h,
                         lv_color_t bg);                    // rounded card
lv_obj_t* ui_label(lv_obj_t* parent, const char* txt,
                   const lv_font_t* font, lv_color_t color);
lv_obj_t* ui_icon_circle(lv_obj_t* parent, const char* symbol,
                         int size, lv_color_t bg, lv_color_t fg);
lv_timer_t* ui_page_timer(lv_obj_t* scr, lv_timer_cb_t cb,
                          uint32_t period, void* user);     // deleted with the screen
void ui_back_home_cb(lv_event_t* e);                        // X button
void ui_user_activity();
void ui_touch_click();          // new touch: click sound (Settings > System)
void ui_prepare_restart();                                  // before ESP.restart()
bool ui_time_valid();

// Power supply of the Tab5 (top bar battery icon), refreshed every 2 s
enum PowerSource : uint8_t { PWR_BATTERY, PWR_USB, PWR_CHARGING };
struct PowerInfo {
  int         level;          // battery %, -1 = no battery / unknown
  PowerSource src;            // on battery / on USB (not charging) / charging
};
const PowerInfo& ui_power();
