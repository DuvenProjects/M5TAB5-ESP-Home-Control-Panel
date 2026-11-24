// ============================================================================
//  home.cpp  -  Home page (dashboard)
//
//   +-------------------+-------------------+-------------------+
//   |  Clock & date     |  Indoor (HVAC)    |  Weather          |
//   |  10 styles: tap   |  temperature,     |  big temperature, |
//   |  -> clock.cpp     |  target, humidity,|  3 day forecast   |
//   |                   |  [Quick]          |                   |
//   +---------+---------+---------+---------+---------+---------+
//   | Blinds  |  RGB    | Air quality | Scenes | Alarm |
//   +---------+---------+-------------+--------+-------+
//
//  Portrait (720 x 1280):
//   +-------------+-------------+
//   | Clock       | Indoor      |
//   +-------------+-------------+
//   | Weather (5 day forecast)  |
//   +-------------+-------------+
//   | Blinds      | RGB         |
//   +-------------+------+------+
//   | Air quality |Scenes|Alarm |
//   +-------------+------+------+
// ============================================================================
#include <time.h>
#include <math.h>
#include "home.h"
#include "ui.h"
#include "menu.h"
#include "weather.h"
#include "lights.h"
#include "rgb.h"
#include "scene.h"
#include "hvac.h"          // quick(), HVAC state
#include "settings.h"      // time / date format (Localization)
#include "aq.h"            // air quality tile (ENS160)
#include "udp_th.h"        // g_outdoor: outdoor unit (UDP) / OpenWeatherMap values
#include "udp_alarm.h"     // fire / smoke / gas alarm switch
#include "clock.h"         // clock styles (tap the clock tile)

static struct {
  // clock (style selected on the clock style page, clock.cpp)
  lv_obj_t *face;
  // indoor
  lv_obj_t *in_temp, *in_set, *in_hum, *in_mode, *in_anim;
  // weather
  lv_obj_t *wx_icon, *wx_temp, *wx_desc, *wx_city, *wx_hum, *wx_src, *wx_dec;
  int32_t  out_key;            // last shown outdoor values (change detection)
  lv_obj_t *fc_day[5], *fc_icon[5], *fc_hi[5], *fc_lo[5];
  int      fc_n;               // forecast days shown: 3 (landscape) / 5 (portrait)
  uint32_t wx_ver;
  // bottom row
  lv_obj_t *bl_state, *bl_slider;
  lv_obj_t *rgb_tile, *rgb_icon, *rgb_state, *rgb_sw;
  lv_obj_t *sc_state;
  lv_obj_t *al_tile, *al_icon, *al_state;
} H;

// ============================================================ helpers
static void go_cb(lv_event_t* e) { ui.show((PageId)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t* nav_tile(lv_obj_t* scr, int x, int y, int w, int h, lv_color_t bg, PageId page) {
  lv_obj_t* t = ui_tile_create(scr, x, y, w, h, bg);
  lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(t, go_cb, LV_EVENT_CLICKED, (void*)(intptr_t)page);
  return t;
}

// Centred single-line label limited to the width of a narrow tile
static void narrow_label(lv_obj_t* l, int w) {
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

static lv_obj_t* box(lv_obj_t* p, int x, int y, int w, int h, lv_color_t c, int r) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

// small thermometer icon (as in the template)
static lv_obj_t* thermo_icon(lv_obj_t* p) {
  lv_obj_t* c = box(p, 0, 0, 64, 64, ui_col(0x5A2A1E), LV_RADIUS_CIRCLE);
  box(c, 27, 12, 10, 30, ui_col(0xFF7A50), 5);
  box(c, 22, 34, 20, 20, ui_col(0xFF7A50), LV_RADIUS_CIRCLE);
  return c;
}

// ============================================================ updates
static void update_clock() {
  clock_face_update(H.face);
}

static void update_weather() {
  uint32_t v = weather_version() * 2 + (settings_fahrenheit() ? 1 : 0);   // new data or unit
  if (v == H.wx_ver) return;
  WeatherData w;
  if (!weather_get(w)) { lv_label_set_text(H.wx_desc, weather_status()); return; }
  H.wx_ver = v;
  weather_to_display_units(w);

  lv_obj_clean(H.wx_icon);
  weather_icon_create(H.wx_icon, w.icon, 96);
  lv_label_set_text(H.wx_desc, weather_cond_text(w.cond_id));
  lv_label_set_text(H.wx_city, w.city);

  for (int i = 0; i < H.fc_n; i++) {
    lv_obj_clean(H.fc_icon[i]);
    if (i >= w.ndays) continue;
    time_t lt = w.days[i].dt + w.tz; struct tm tmv; gmtime_r(&lt, &tmv);
    lv_label_set_text(H.fc_day[i], i == 0 ? tr(S_TODAY) : lang_day_short(tmv.tm_wday));
    char ic[4]; strlcpy(ic, w.days[i].icon, 4); if (strlen(ic) == 3) ic[2] = 'd';
    weather_icon_create(H.fc_icon[i], ic, 52);
    lv_label_set_text_fmt(H.fc_hi[i], "%d°", (int)lroundf(w.days[i].tmax));
    lv_label_set_text_fmt(H.fc_lo[i], "%d°", (int)lroundf(w.days[i].tmin));
  }
}

// Big outdoor temperature + humidity: from g_outdoor (outdoor unit or
// OpenWeatherMap, selected in udp_th.h). Icon, description and forecast
// stay from OpenWeatherMap (update_weather).
// Puts ".d°" right after the scaled integer part, on the same baseline
#define WX_TEMP_SCALE  486                         // ~1.9x (256 = 1x)
static void place_decimal() {
  lv_obj_update_layout(H.wx_temp);
  int32_t w   = lv_obj_get_width(H.wx_temp) * WX_TEMP_SCALE / 256;
  const lv_font_t* fb = FONT_48;
  const lv_font_t* fs = FONT_40;
  int32_t base = -10 + (fb->line_height - fb->base_line) * WX_TEMP_SCALE / 256;  // baseline of the big text
  lv_obj_set_pos(H.wx_dec, 106 + w + 4, base - (fs->line_height - fs->base_line));
}

static void update_outdoor() {
  const OutdoorData& o = g_outdoor;
  bool ok = !isnan(o.temp);
  float t = ok ? settings_temp(o.temp) : 0;
  int32_t tenths = ok ? (int32_t)lroundf(t * 10.0f) : 0;   // one decimal: 12.1
  int32_t key = ok ? (tenths * 1000 + (int32_t)lroundf(isnan(o.hum) ? -1 : o.hum)) * 4 + o.src
                   : -1;
  key = key * 2 + (settings_fahrenheit() ? 1 : 0);
  if (key == H.out_key) return;
  H.out_key = key;

  if (!ok) {
    lv_label_set_text(H.wx_temp, "--");
    lv_label_set_text(H.wx_dec, ".-°");
    place_decimal();
    lv_label_set_text(H.wx_hum, "");
    lv_label_set_text(H.wx_src, "");
    return;
  }
  // Float value with one decimal: big integer part + smaller ".d°"
  int32_t a = tenths < 0 ? -tenths : tenths;
  lv_label_set_text_fmt(H.wx_temp, "%s%ld", tenths < 0 ? "-" : "", (long)(a / 10));
  lv_label_set_text_fmt(H.wx_dec, ".%ld°", (long)(a % 10));
  place_decimal();
  if (isnan(o.hum)) lv_label_set_text(H.wx_hum, "");
  else              lv_label_set_text_fmt(H.wx_hum, LV_SYMBOL_TINT " %d %%", (int)lroundf(o.hum));

  if (o.src == OUTDOOR_SRC_UDP) {
    lv_label_set_text(H.wx_src, tr(S_OUTDOOR_UNIT));
    lv_obj_set_style_text_color(H.wx_src, C_GREEN, 0);
  } else {
    lv_label_set_text(H.wx_src, "OpenWeatherMap");
    // orange: the outdoor unit is selected but sends nothing (fallback)
    lv_obj_set_style_text_color(H.wx_src, OUTDOOR_USE_UDP ? C_ORANGE : C_TEXT2, 0);
  }
}

static void update_tiles() {
  // --- indoor
  bool ok = hvac_sensor_ok();
  if (ok) {
    lv_label_set_text_fmt(H.in_temp, "%.1f°", settings_temp(g_data.indoor_temp));
    lv_label_set_text_fmt(H.in_hum, LV_SYMBOL_TINT "  %d %%", (int)lroundf(g_data.indoor_hum));
  } else {
    lv_label_set_text(H.in_temp, "--.-°");
    lv_label_set_text_fmt(H.in_hum, LV_SYMBOL_WARNING "  %s", tr(S_AHT10_ERROR));
  }
  lv_obj_set_style_text_color(H.in_hum, ok ? ui_col(0x5AB4FF) : C_RED, 0);
  lv_label_set_text(H.in_set, hvac_target_text());
  lv_label_set_text(H.in_mode, hvac_summary());
  hvac_anim_update(H.in_anim);                       // same animation as the HVAC page
  lv_obj_set_style_text_color(H.in_mode, ok ? C_ORANGE : C_RED, 0);

  // --- blinds
  int avg = blinds_average();
  bool moving = false;
  for (int i = 0; i < BLIND_COUNT; i++) moving |= blinds_moving(i);
  lv_label_set_text_fmt(H.bl_state, "%s  •  %d %%",
                        tr(moving ? S_MOVING : (avg == 0 ? S_CLOSED : S_OPEN_STATE)), avg);
  if (!lv_obj_has_state(H.bl_slider, LV_STATE_PRESSED))
    lv_slider_set_value(H.bl_slider, avg, LV_ANIM_OFF);

  // --- RGB
  lv_color_t c = rgb_current_color();
  bool on = g_data.rgb_on;
  lv_obj_set_style_bg_color(H.rgb_icon, on ? c : ui_col(0x3A3A40), 0);
  lv_obj_set_style_shadow_color(H.rgb_icon, c, 0);
  lv_obj_set_style_shadow_width(H.rgb_icon, on ? 30 : 0, 0);
  lv_obj_set_style_bg_color(H.rgb_tile, on ? lv_color_mix(c, ui_col(0x1C1C1E), 50) : C_TILE, 0);
  if (on) lv_label_set_text_fmt(H.rgb_state, "%s  •  %d %%", rgb_effect_name(g_data.rgb_effect), g_data.rgb_brightness);
  else    lv_label_set_text(H.rgb_state, tr(S_OFF));
  if (on != lv_obj_has_state(H.rgb_sw, LV_STATE_CHECKED)) {
    if (on) lv_obj_add_state(H.rgb_sw, LV_STATE_CHECKED); else lv_obj_remove_state(H.rgb_sw, LV_STATE_CHECKED);
  }

  // --- scene
  lv_label_set_text(H.sc_state, scene_name(g_data.scene_id));

  // --- alarm
  if (g_data.alarm_active) {
    lv_label_set_text(H.al_state, tr(S_ALARM_ACTIVE));
    lv_obj_set_style_bg_color(H.al_tile, ui_col(0x6A1010), 0);
    lv_obj_set_style_bg_color(H.al_icon, C_RED, 0);
  } else {
    bool armed = g_data.alarm_armed || udp_alarm_safety_armed();   // motion or fire / smoke / gas
    lv_label_set_text(H.al_state, tr(armed ? S_ARMED : S_DISARMED));
    lv_obj_set_style_bg_color(H.al_tile, ui_col(0x2A1A1A), 0);
    lv_obj_set_style_bg_color(H.al_icon, armed ? ui_col(0x2E7D32) : ui_col(0x5A2A2A), 0);
  }
}

static void page_timer_cb(lv_timer_t* t) {
  update_clock();
  update_weather();
  update_outdoor();
  update_tiles();
}

// ============================================================ events
static void quick_cb(lv_event_t* e) {
  quick();                                    // function in hvac.cpp
  update_tiles();
}

static void blind_slider_cb(lv_event_t* e) {
  lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
  blinds_set_target(-1, lv_slider_get_value(s));
}

static void rgb_sw_cb(lv_event_t* e) {
  g_data.rgb_on = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  rgb_apply();
  data_mark_dirty();
  update_tiles();
}

// ============================================================ PAGE
lv_obj_t* home_create() {
  memset(&H, 0, sizeof(H));
  H.wx_ver = 0xFFFFFFFF;
  H.out_key = INT32_MIN;

  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_HOME), LV_SYMBOL_HOME, MENU_HOME, ui_col(0x2A2A2E));

  // Tile geometry: landscape = 3 big tiles + 5 small tiles in 2 rows,
  // portrait = 4 rows (see the drawing at the top)
  const bool port = ui_portrait();
  const int Y1 = 108, H1 = 344;                      // clock + indoor (+ weather in landscape)
  const int Y2 = port ? 1046 : 472, H2 = port ? 214 : 228;   // air quality, scenes, alarm
  const int Y3 = port ? 812 : Y2;                    // blinds + RGB
  const int XC = 20, XI = port ? 370 : 440, WT = port ? 330 : 400;
  const int XW = port ? 20 : 860, YW = port ? 472 : Y1, WW = port ? 680 : 400, HW = port ? 320 : H1;
  H.fc_n = port ? 5 : 3;

  // ------------------------------------------------ CLOCK & DATE
  // Tap: clock style page (10 digital / analog styles, saved in NVS)
  lv_obj_t* ct = nav_tile(scr, XC, Y1, WT, H1, C_TILE, PAGE_CLOCK);
  H.face = clock_face_create(ct, clock_theme());
  if (port) {                                        // 364 x 308 face in a 294 px wide tile
    const int sc = (WT - 36) * 256 / CLOCK_FACE_W;
    lv_obj_set_style_transform_pivot_x(H.face, 0, 0);
    lv_obj_set_style_transform_pivot_y(H.face, 0, 0);
    lv_obj_set_style_transform_scale(H.face, sc, 0);
    lv_obj_set_pos(H.face, 0, (H1 - 36 - CLOCK_FACE_H * sc / 256) / 2);
  }

  // ------------------------------------------------ INDOOR / HVAC
  lv_obj_t* it = nav_tile(scr, XI, Y1, WT, H1, ui_col(0x2A1C18), PAGE_HVAC);
  lv_obj_t* th = thermo_icon(it);
  lv_obj_set_pos(th, 0, 0);
  lv_obj_t* itl = ui_label(it, tr(S_INDOOR), FONT_24, C_TEXT);
  lv_obj_set_pos(itl, 80, 2);
  H.in_mode = ui_label(it, "", FONT_16, C_ORANGE);
  lv_obj_set_pos(H.in_mode, 80, 36);
  lv_obj_set_width(H.in_mode, WT - 36 - 80);
  lv_label_set_long_mode(H.in_mode, LV_LABEL_LONG_DOT);

  H.in_temp = ui_label(it, "--", FONT_48, C_TEXT);
  lv_obj_set_pos(H.in_temp, 4, 72);
  lv_obj_set_style_transform_scale(H.in_temp, 486, 0);       // ~1.9x large indoor temperature
  lv_obj_set_style_transform_pivot_x(H.in_temp, 0, 0);
  lv_obj_set_style_transform_pivot_y(H.in_temp, 0, 0);

  // running HVAC animation (flame / snowflake / droplet / fan), hidden when idle
  H.in_anim = hvac_anim_create(it);
  lv_obj_set_pos(H.in_anim, WT - 36 - 68, 96);

  H.in_set = ui_label(it, "", FONT_20, C_TEXT2);
  lv_obj_set_pos(H.in_set, 4, 196);
  H.in_hum = ui_label(it, "", FONT_24, ui_col(0x5AB4FF));
  lv_obj_set_pos(H.in_hum, 4, 236);

  lv_obj_t* qb = lv_button_create(it);
  lv_obj_set_size(qb, 150, 72);
  lv_obj_align(qb, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(qb, 24, 0);
  lv_obj_set_style_bg_color(qb, C_ORANGE, 0);
  lv_obj_set_style_bg_color(qb, ui_col(0xFF8A30), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(qb, 0, 0);
  lv_obj_t* ql = ui_label(qb, "", FONT_24, ui_col(0x2A1C18));
  lv_label_set_text_fmt(ql, LV_SYMBOL_CHARGE " %s", tr(S_QUICK));
  lv_obj_center(ql);
  lv_obj_add_event_cb(qb, quick_cb, LV_EVENT_CLICKED, nullptr);

  // ------------------------------------------------ WEATHER
  lv_obj_t* wt = nav_tile(scr, XW, YW, WW, HW, ui_col(0x2C2D3A), PAGE_WEATHER);
  H.wx_icon = box(wt, -4, -4, 96, 96, ui_col(0x2C2D3A), 0);
  lv_obj_set_style_bg_opa(H.wx_icon, LV_OPA_TRANSP, 0);
  H.wx_temp = ui_label(wt, "--", FONT_48, C_TEXT);
  lv_obj_set_pos(H.wx_temp, 106, -10);
  lv_obj_set_style_transform_scale(H.wx_temp, WX_TEMP_SCALE, 0);  // ~1.9x large outdoor temperature
  lv_obj_set_style_transform_pivot_x(H.wx_temp, 0, 0);
  lv_obj_set_style_transform_pivot_y(H.wx_temp, 0, 0);
  H.wx_dec = ui_label(wt, "", FONT_40, C_TEXT);                    // ".1°" decimal part
  H.wx_city = ui_label(wt, g_data.city, FONT_16, C_TEXT2);
  lv_obj_align(H.wx_city, LV_ALIGN_TOP_RIGHT, 0, 0);
  H.wx_desc = ui_label(wt, tr(S_LOADING), FONT_20, C_TEXT);
  lv_obj_set_pos(H.wx_desc, 4, 108);
  lv_obj_set_width(H.wx_desc, WW - 36 - 114);
  lv_label_set_long_mode(H.wx_desc, LV_LABEL_LONG_DOT);
  H.wx_hum = ui_label(wt, "", FONT_20, ui_col(0x5AB4FF));   // outdoor humidity
  lv_obj_align(H.wx_hum, LV_ALIGN_TOP_RIGHT, 0, 108);
  H.wx_src = ui_label(wt, "", FONT_16, C_TEXT2);                  // value source, under the
  lv_obj_set_pos(H.wx_src, 110, 82);                              // big temperature

  lv_obj_t* sep = box(wt, 0, 146, WW - 36, 1, ui_col(0x44465A), 0);
  (void)sep;
  const int cw = port ? (WW - 36) / H.fc_n : 122;    // forecast column width
  for (int i = 0; i < H.fc_n; i++) {
    int x = i * cw + (cw - 122) / 2;
    H.fc_day[i] = ui_label(wt, "--", FONT_16, C_TEXT2);
    lv_obj_set_width(H.fc_day[i], 120);
    lv_obj_set_style_text_align(H.fc_day[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(H.fc_day[i], x, 158);
    H.fc_icon[i] = box(wt, x + 34, 186, 52, 52, ui_col(0x2C2D3A), 0);
    H.fc_hi[i] = ui_label(wt, "", FONT_24, C_TEXT);
    lv_obj_set_pos(H.fc_hi[i], x + 16, 250);
    H.fc_lo[i] = ui_label(wt, "", FONT_24, C_TEXT2);
    lv_obj_set_pos(H.fc_lo[i], x + 66, 250);
  }

  // ------------------------------------------------ BLINDS
  // blinds | RGB | air quality | scenes | alarm  (20 + 290+20+290+20+280+20+150+20+150 = 1260)
  // portrait: blinds | RGB (330 + 330), air quality | scenes | alarm (330 + 155 + 155)
  const int WB = port ? 330 : 290, WQ = port ? 330 : 280, WS = port ? 155 : 150, G2 = 20;
  const int XB = 20, XR = XB + WB + G2;
  const int XQ = port ? 20 : XR + WB + G2, XS = XQ + WQ + G2, XA = XS + WS + G2;
  const int IW = WB - 36;                            // inner width of the blinds / RGB tiles
  lv_obj_t* bt = nav_tile(scr, XB, Y3, WB, H2, ui_col(0x231C30), PAGE_LIGHTS);
  lv_obj_t* bi = ui_icon_circle(bt, LV_SYMBOL_LIST, 64, ui_col(0x3D2F5C), ui_col(0x9C6ADE));
  lv_obj_set_pos(bi, 0, 0);
  lv_obj_t* btl = ui_label(bt, tr(S_BLINDS), FONT_24, C_TEXT);
  lv_obj_set_pos(btl, 78, 4);
  H.bl_state = ui_label(bt, "", FONT_16, C_TEXT2);
  lv_obj_set_pos(H.bl_state, 78, 36);
  lv_obj_set_width(H.bl_state, IW - 78);
  lv_label_set_long_mode(H.bl_state, LV_LABEL_LONG_DOT);

  H.bl_slider = lv_slider_create(bt);
  lv_obj_set_size(H.bl_slider, IW, 84);
  lv_obj_align(H.bl_slider, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_slider_set_range(H.bl_slider, 0, 100);
  lv_obj_set_style_radius(H.bl_slider, 26, LV_PART_MAIN);
  lv_obj_set_style_radius(H.bl_slider, 26, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(H.bl_slider, ui_col(0x3D2F5C), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(H.bl_slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(H.bl_slider, ui_col(0x9C6ADE), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(H.bl_slider, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(H.bl_slider, 0, LV_PART_KNOB);
  lv_obj_add_event_cb(H.bl_slider, blind_slider_cb, LV_EVENT_RELEASED, nullptr);

  // ------------------------------------------------ RGB LED
  H.rgb_tile = nav_tile(scr, XR, Y3, WB, H2, C_TILE, PAGE_RGB);
  H.rgb_icon = ui_icon_circle(H.rgb_tile, LV_SYMBOL_TINT, 64, ui_col(0x3A3A40), C_TEXT);
  lv_obj_set_pos(H.rgb_icon, 0, 0);
  lv_obj_set_style_shadow_opa(H.rgb_icon, LV_OPA_70, 0);
  lv_obj_t* rtl = ui_label(H.rgb_tile, tr(S_RGB_LIGHT), FONT_24, C_TEXT);
  lv_obj_set_pos(rtl, 78, 4);
  H.rgb_state = ui_label(H.rgb_tile, "", FONT_16, C_TEXT2);
  lv_obj_set_pos(H.rgb_state, 78, 36);
  lv_obj_set_width(H.rgb_state, IW - 78);
  lv_label_set_long_mode(H.rgb_state, LV_LABEL_LONG_DOT);
  H.rgb_sw = lv_switch_create(H.rgb_tile);
  lv_obj_set_size(H.rgb_sw, IW, 84);
  lv_obj_align(H.rgb_sw, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(H.rgb_sw, 26, LV_PART_MAIN);
  lv_obj_set_style_radius(H.rgb_sw, 26, LV_PART_INDICATOR);
  lv_obj_set_style_radius(H.rgb_sw, 22, LV_PART_KNOB);
  lv_obj_set_style_bg_color(H.rgb_sw, ui_col(0x3A3A40), LV_PART_MAIN);
  lv_obj_set_style_bg_color(H.rgb_sw, C_ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(H.rgb_sw, rgb_sw_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  // ------------------------------------------------ AIR QUALITY (aq.cpp)
  aq_tile_create(scr, XQ, Y2, WQ, H2);

  // ------------------------------------------------ SCENES
  lv_obj_t* st = nav_tile(scr, XS, Y2, WS, H2, ui_col(0x35203A), PAGE_SCENE);
  lv_obj_t* si = ui_icon_circle(st, LV_SYMBOL_IMAGE, 80, ui_col(0x5A2A66), ui_col(0xD070E0));
  lv_obj_align(si, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_t* stl = ui_label(st, tr(S_SCENES), FONT_24, C_TEXT);
  narrow_label(stl, WS - 36);
  lv_obj_align(stl, LV_ALIGN_TOP_MID, 0, 98);
  H.sc_state = ui_label(st, "", FONT_16, C_TEXT2);
  narrow_label(H.sc_state, WS - 36);
  lv_obj_align(H.sc_state, LV_ALIGN_TOP_MID, 0, 134);

  // ------------------------------------------------ ALARM
  H.al_tile = nav_tile(scr, XA, Y2, WS, H2, ui_col(0x2A1A1A), PAGE_ALARM);
  H.al_icon = ui_icon_circle(H.al_tile, LV_SYMBOL_BELL, 80, ui_col(0x5A2A2A), C_TEXT);
  lv_obj_align(H.al_icon, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_t* atl = ui_label(H.al_tile, tr(S_ALARM), FONT_24, C_TEXT);
  narrow_label(atl, WS - 36);
  lv_obj_align(atl, LV_ALIGN_TOP_MID, 0, 98);
  H.al_state = ui_label(H.al_tile, "", FONT_16, C_TEXT2);
  narrow_label(H.al_state, WS - 36);
  lv_obj_align(H.al_state, LV_ALIGN_TOP_MID, 0, 134);

  lv_timer_t* t = ui_page_timer(scr, page_timer_cb, 1000, nullptr);
  page_timer_cb(t);
  return scr;
}
