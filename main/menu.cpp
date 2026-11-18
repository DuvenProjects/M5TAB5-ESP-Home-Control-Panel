// ============================================================================
//  menu.cpp  -  Common top bar (title, clock, Blynk, motion, Wi-Fi, battery, settings / close)
// ============================================================================
#include <time.h>
#include <stdint.h>
#include "menu.h"
#include "ui.h"
#include "settings.h"   // time / date format (Localization)
#include "hvac.h"       // HVAC_USE_ICON_FONT
#include "blynk.h"      // blynk_state() (Home only)

// Wi-Fi icon: classic Wi-Fi fan from hvac_icons.c (U+E009..U+E00D)
//   level 0 = not connected (dim arcs + slash), 1..4 = signal
#if HVAC_USE_ICON_FONT
extern "C" { LV_FONT_DECLARE(font_hvac_icons); }
#define WIFI_FONT (&font_hvac_icons)
static const char* const WIFI_GLYPH[5] = {"\xEE\x80\x89", "\xEE\x80\x8A", "\xEE\x80\x8B",
                                          "\xEE\x80\x8C", "\xEE\x80\x8D"};
#else
#define WIFI_FONT (&lv_font_montserrat_32)
static const char* const WIFI_GLYPH[5] = {LV_SYMBOL_WIFI, LV_SYMBOL_WIFI, LV_SYMBOL_WIFI,
                                          LV_SYMBOL_WIFI, LV_SYMBOL_WIFI};
#endif

#define BAT_W       34      // battery outline
#define BAT_H       18
#define BAT_FILL_W  (BAT_W - 6)
#define BAT_BLOCK_W 108     // symbol + outline + nub + "100%"

struct MenuCtx {
  lv_obj_t* clock;
  lv_obj_t* date;
  lv_obj_t* motion;       // nullptr on sub pages
  lv_obj_t* motion_lbl;
  lv_obj_t* blynk;        // nullptr on sub pages
  lv_obj_t* blynk_dot;
  lv_obj_t* blynk_lbl;
  int       blynk_st;
  lv_obj_t* wifi;
  int       wifi_lvl;
  lv_obj_t* bat_body;     // battery outline
  lv_obj_t* bat_fill;     // charge level bar
  lv_obj_t* bat_sym;      // bolt (charging) / USB plug (on USB)
  lv_obj_t* bat_pct;      // "85%"
  int       bat_key;
};

static int rssi_level(int rssi) {
  if (rssi == 0)    return 0;            // not connected
  if (rssi >= -55)  return 4;
  if (rssi >= -65)  return 3;
  if (rssi >= -75)  return 2;
  return 1;
}

static void menu_update(lv_timer_t* t) {
  MenuCtx* c = (MenuCtx*)lv_timer_get_user_data(t);

  // --- clock
  if (ui_time_valid()) {
    time_t now = time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    char tb[24], db[24];
    settings_format_time(tb, sizeof(tb));               // 24 h or AM / PM
    settings_format_date(db, sizeof(db));               // selected date format
    lv_label_set_text(c->clock, tb);
    lv_label_set_text_fmt(c->date, "%s  %s", lang_day_short(lt.tm_wday), db);
  } else {
    lv_label_set_text(c->clock, "--:--");
    lv_label_set_text(c->date, tr(S_TIME_SYNC));
  }

  // --- Wi-Fi icon (fan drawn by signal level)
  int lvl = rssi_level(g_data.wifi_rssi);
  if (lvl != c->wifi_lvl) {
    c->wifi_lvl = lvl;
    lv_label_set_text(c->wifi, WIFI_GLYPH[lvl]);
    lv_obj_set_style_text_color(c->wifi, lvl ? C_TEXT : ui_col(0x8E8E93), 0);
  }

  // --- battery: level bar + percentage, bolt = charging, plug = on USB
  {
    const PowerInfo& p = ui_power();
    int key = p.level * 4 + (int)p.src;
    if (key != c->bat_key) {
      c->bat_key = key;
      bool has = p.level >= 0;
      int  w = has ? (p.level * BAT_FILL_W + 50) / 100 : BAT_FILL_W;
      if (w < 3) w = 3;
      lv_obj_set_width(c->bat_fill, w);
      lv_color_t fc = p.src == PWR_CHARGING ? C_GREEN
                    : !has                  ? ui_col(0x8E8E93)
                    : p.level <= 20         ? C_RED
                    : p.level <= 40         ? C_ORANGE : C_TEXT;
      lv_obj_set_style_bg_color(c->bat_fill, fc, 0);
      if (p.src == PWR_CHARGING) {
        lv_label_set_text(c->bat_sym, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(c->bat_sym, C_GREEN, 0);
      } else if (p.src == PWR_USB) {
        lv_label_set_text(c->bat_sym, LV_SYMBOL_USB);
        lv_obj_set_style_text_color(c->bat_sym, C_ACCENT, 0);
      } else {
        lv_label_set_text(c->bat_sym, "");
      }
      if (has) lv_label_set_text_fmt(c->bat_pct, "%d%%", p.level);
      else     lv_label_set_text(c->bat_pct, "USB");
    }
  }

  // --- Blynk status (Home only): grey = off, red dot = offline, green = online
  if (c->blynk) {
    int st = (int)blynk_state();
    if (st != c->blynk_st) {
      c->blynk_st = st;
      lv_color_t dot = st == BLYNK_LINK_ONLINE  ? ui_col(0x34C759)
                     : st == BLYNK_LINK_OFFLINE ? ui_col(0xFF453A)
                                           : ui_col(0x48484A);
      lv_obj_set_style_bg_color(c->blynk_dot, dot, 0);
      lv_obj_set_style_text_color(c->blynk_lbl,
          st == BLYNK_LINK_ONLINE ? C_TEXT : st == BLYNK_LINK_OFFLINE ? C_TEXT2 : ui_col(0x6E6E73), 0);
      lv_obj_set_style_border_color(c->blynk,
          st == BLYNK_LINK_ONLINE ? ui_col(0x34C759) : ui_col(0x2A2A2E), 0);
    }
  }

  // --- motion icon (Home only)
  if (c->motion) {
    if (!settings_motion_enabled()) {                  // switched off in Settings > System
      lv_obj_set_style_bg_color(c->motion, ui_col(0x2A2A2E), 0);
      lv_obj_set_style_text_color(c->motion_lbl, ui_col(0x6E6E73), 0);
      lv_label_set_text_fmt(c->motion_lbl, LV_SYMBOL_EYE_CLOSE "  %s", tr(S_SENSOR_OFF));
      return;
    }
    bool m = g_data.motion;
    lv_obj_set_style_bg_color(c->motion, m ? C_ORANGE : ui_col(0x2A2A2E), 0);
    lv_obj_set_style_text_color(c->motion_lbl, m ? ui_col(0x000000) : C_TEXT2, 0);
    lv_label_set_text_fmt(c->motion_lbl, "%s  %s", m ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE,
                          tr(m ? S_MOTION : S_IDLE));
  }
}

static void ctx_free_cb(lv_event_t* e) { lv_free(lv_event_get_user_data(e)); }
static void settings_cb(lv_event_t* e) { ui.show(PAGE_SETTINGS); }

static lv_obj_t* round_btn(lv_obj_t* parent, const char* sym, lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, 64, 64);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, ui_col(0x1C1C1E), 0);
  lv_obj_set_style_bg_color(b, ui_col(0x3A3A40), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, sym);
  lv_obj_set_style_text_font(l, FONT_32, 0);
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_ext_click_area(b, 12);
  return b;
}

lv_obj_t* menu_create(lv_obj_t* scr, const char* title, const char* symbol,
                      MenuMode mode, lv_color_t icon_bg) {
  MenuCtx* c = (MenuCtx*)lv_malloc(sizeof(MenuCtx));
  memset(c, 0, sizeof(MenuCtx));

  lv_obj_t* bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 1280, MENU_HEIGHT);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(bar, ctx_free_cb, LV_EVENT_DELETE, c);

  // ---- left: icon + title
  lv_obj_t* ic = ui_icon_circle(bar, symbol, 68, icon_bg, C_TEXT);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 28, 4);
  lv_obj_t* t = ui_label(bar, title, FONT_32, C_TEXT);
  lv_obj_align_to(t, ic, LV_ALIGN_OUT_RIGHT_MID, 20, 0);

  // ---- centre: clock + date
  c->clock = ui_label(bar, "--:--", FONT_40, C_TEXT);
  lv_obj_align(c->clock, LV_ALIGN_TOP_MID, 0, 10);
  c->date = ui_label(bar, "", FONT_16, C_TEXT2);
  lv_obj_align(c->date, LV_ALIGN_TOP_MID, 0, 62);

  // ---- right: settings or close
  lv_obj_t* rb = round_btn(bar, mode == MENU_HOME ? LV_SYMBOL_SETTINGS : LV_SYMBOL_CLOSE,
                           mode == MENU_HOME ? settings_cb : ui_back_home_cb);
  lv_obj_align(rb, LV_ALIGN_RIGHT_MID, -28, 4);

  // ---- battery (between the Wi-Fi icon and the round button), one row on
  //      the same centre line as the round button:  [sym] [outline][nub] 85%
  lv_obj_t* bb = lv_obj_create(bar);
  lv_obj_remove_style_all(bb);
  lv_obj_set_size(bb, BAT_BLOCK_W, 64);                // same height as the round button
  lv_obj_remove_flag(bb, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_align(bb, LV_ALIGN_RIGHT_MID, -94, 4);        // same y offset as the round button
  c->bat_sym = ui_label(bb, "", FONT_16, C_TEXT);      // LV_SYMBOL_* from the Montserrat fallback
  lv_obj_set_width(c->bat_sym, 16);
  lv_obj_set_style_text_align(c->bat_sym, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(c->bat_sym, LV_ALIGN_LEFT_MID, 0, 0);
  c->bat_body = lv_obj_create(bb);
  lv_obj_remove_style_all(c->bat_body);
  lv_obj_set_size(c->bat_body, BAT_W, BAT_H);
  lv_obj_align(c->bat_body, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_set_style_radius(c->bat_body, 4, 0);
  lv_obj_set_style_border_width(c->bat_body, 2, 0);
  lv_obj_set_style_border_color(c->bat_body, ui_col(0xC7C7CC), 0);
  lv_obj_remove_flag(c->bat_body, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  c->bat_fill = lv_obj_create(c->bat_body);
  lv_obj_remove_style_all(c->bat_fill);
  lv_obj_set_size(c->bat_fill, BAT_FILL_W, BAT_H - 6);
  // child positions start inside the 2 px border: 1 px gap on every side
  lv_obj_set_pos(c->bat_fill, 1, 1);
  lv_obj_set_style_radius(c->bat_fill, 2, 0);
  lv_obj_set_style_bg_opa(c->bat_fill, LV_OPA_COVER, 0);
  lv_obj_t* nub = lv_obj_create(bb);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, 3, 8);
  lv_obj_align(nub, LV_ALIGN_LEFT_MID, 20 + BAT_W, 0);
  lv_obj_set_style_radius(nub, 1, 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nub, ui_col(0xC7C7CC), 0);
  c->bat_pct = ui_label(bb, "", FONT_16, C_TEXT2);
  lv_obj_align(c->bat_pct, LV_ALIGN_LEFT_MID, 20 + BAT_W + 3 + 6, 0);
  c->bat_key = INT32_MIN;

  // ---- Wi-Fi icon (classic fan, 36 px)
  lv_obj_t* wf = lv_obj_create(bar);
  lv_obj_remove_style_all(wf);
  lv_obj_set_size(wf, 44, 44);
  lv_obj_remove_flag(wf, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(wf, LV_ALIGN_RIGHT_MID, -198, 4);       // left of the battery
  c->wifi = lv_label_create(wf);
  lv_obj_set_style_text_font(c->wifi, WIFI_FONT, 0);
  lv_label_set_text(c->wifi, WIFI_GLYPH[0]);
  lv_obj_center(c->wifi);
  c->wifi_lvl = -1;

  // ---- Home only: [Blynk status] [motion indicator] in a right-aligned row;
  //      the row re-aligns itself when the translated motion text changes width
  if (mode == MENU_HOME) {
    lv_obj_t* grp = lv_obj_create(bar);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, 48);
    lv_obj_remove_flag(grp, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 6, 0);
    lv_obj_align(grp, LV_ALIGN_RIGHT_MID, -258, 4);   // left of the Wi-Fi icon

    // Blynk status pill: coloured dot + "Blynk"
    c->blynk = lv_obj_create(grp);
    lv_obj_remove_style_all(c->blynk);
    lv_obj_set_size(c->blynk, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(c->blynk, 10, 0);
    lv_obj_set_style_radius(c->blynk, 24, 0);
    lv_obj_set_style_bg_opa(c->blynk, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c->blynk, ui_col(0x2A2A2E), 0);
    lv_obj_set_style_border_width(c->blynk, 2, 0);
    lv_obj_set_flex_flow(c->blynk, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c->blynk, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c->blynk, 10, 0);
    lv_obj_remove_flag(c->blynk, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    c->blynk_dot = lv_obj_create(c->blynk);
    lv_obj_remove_style_all(c->blynk_dot);
    lv_obj_set_size(c->blynk_dot, 14, 14);
    lv_obj_set_style_radius(c->blynk_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c->blynk_dot, LV_OPA_COVER, 0);
    c->blynk_lbl = ui_label(c->blynk, tr(S_BLYNK), FONT_20, C_TEXT2);
    c->blynk_st = -1;

    c->motion = lv_obj_create(grp);
    lv_obj_remove_style_all(c->motion);
    lv_obj_set_size(c->motion, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_min_width(c->motion, 150, 0);
    lv_obj_set_style_pad_hor(c->motion, 10, 0);
    lv_obj_set_style_radius(c->motion, 24, 0);
    lv_obj_set_style_bg_opa(c->motion, LV_OPA_COVER, 0);
    c->motion_lbl = ui_label(c->motion, "", FONT_20, C_TEXT2);
    lv_obj_center(c->motion_lbl);
  }

  lv_timer_t* tm = ui_page_timer(scr, menu_update, 500, c);
  menu_update(tm);
  return bar;
}
