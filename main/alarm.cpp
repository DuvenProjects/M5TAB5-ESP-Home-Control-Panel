// ============================================================================
//  alarm.cpp  -  Alarm page
//   Left  : status card (disarmed / armed / ALARM! + reason) with the detector
//           states (motion, fire, smoke, gas) and the alarm unit state,
//           two switches: motion alarm  |  fire / smoke / gas alarm
//   Right : PIN keypad: the correct PIN switches EVERY alarm off and stops a
//           ringing alarm (default PIN 1234)
//   Switching an alarm on needs no PIN; switching it off needs the PIN.
//   While an alarm rings: blinking red background + siren on the Tab5 speaker
// ============================================================================
#include <M5Unified.h>
#include "alarm.h"
#include "ui.h"
#include "menu.h"
#include "settings.h"    // motion sensor switch
#include "udp_alarm.h"   // fire / smoke / gas alarm unit

enum { DET_MOTION = 0, DET_FIRE, DET_SMOKE, DET_GAS, DET_COUNT };

static struct {
  lv_obj_t *scr, *status_card, *icon, *status, *reason;
  lv_obj_t *det_dot[DET_COUNT], *unit;
  lv_obj_t *sw_motion, *sw_safety;
  lv_obj_t *pin_lbl, *msg;
  char pin[8];
  bool blink;
} A;

static const char* KEYS[] = {
  "1", "2", "3", "\n",
  "4", "5", "6", "\n",
  "7", "8", "9", "\n",
  LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
};

static void show_pin() {
  char stars[16] = "";
  size_t n = strlen(A.pin);
  for (size_t i = 0; i < n; i++) strcat(stars, "* ");
  lv_label_set_text(A.pin_lbl, n ? stars : "- - - -");
}

static void show_msg(const char* txt, lv_color_t col) {
  lv_label_set_text(A.msg, txt);
  lv_obj_set_style_text_color(A.msg, col, 0);
}

static void set_switch(lv_obj_t* sw, bool on) {
  if (on == lv_obj_has_state(sw, LV_STATE_CHECKED)) return;
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED); else lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

static void update_status() {
  bool motionArmed = g_data.alarm_armed;
  bool safetyArmed = udp_alarm_safety_armed();
  const SafetyState& s = udp_alarm_state();

  // ---- status + reason
  lv_color_t col; const char* txt; const char* sym;
  if (g_data.alarm_active)              { col = C_RED;   txt = tr(S_ALARM_ACTIVE); sym = LV_SYMBOL_WARNING; }
  else if (motionArmed || safetyArmed)  { col = C_GREEN; txt = tr(S_ARMED);        sym = LV_SYMBOL_EYE_OPEN; }
  else                                  { col = ui_col(0x8E8E93); txt = tr(S_DISARMED); sym = LV_SYMBOL_EYE_CLOSE; }
  lv_label_set_text(A.status, txt);
  lv_obj_set_style_text_color(A.status, col, 0);
  lv_obj_set_style_bg_color(A.icon, col, 0);
  lv_label_set_text(lv_obj_get_child(A.icon, 0), sym);

  char r[160] = "";
  if (g_data.alarm_active) {
    strlcpy(r, ui_alarm_reason(), sizeof(r));
  } else {
    if (motionArmed) strlcat(r, tr(settings_motion_enabled() ? S_AL_WATCHING : S_SENSOR_OFF), sizeof(r));
    if (safetyArmed) {
      if (r[0]) strlcat(r, "\n", sizeof(r));
      strlcat(r, tr(S_AL_SAFE_WATCH), sizeof(r));
    }
    if (!r[0]) strlcpy(r, tr(S_AL_IDLE), sizeof(r));
  }
  lv_label_set_text(A.reason, r);

  // ---- detector dots: red = detected, green = watched, grey = off / unknown
  bool online = s.unit == ALARM_UNIT_ONLINE;
  const bool det[DET_COUNT]   = {g_data.motion, s.fire, s.smoke, s.gas};
  const bool watch[DET_COUNT] = {motionArmed, safetyArmed && online, safetyArmed && online, safetyArmed && online};
  for (int i = 0; i < DET_COUNT; i++) {
    lv_color_t c = (det[i] && (i == DET_MOTION ? true : online)) ? (i == DET_MOTION ? C_ORANGE : C_RED)
                 : watch[i] ? C_GREEN : ui_col(0x5A5A60);
    lv_obj_set_style_bg_color(A.det_dot[i], c, 0);
  }

  // ---- alarm unit
  StrId us = s.unit == ALARM_UNIT_ONLINE ? S_AL_UNIT_ONLINE
           : s.unit == ALARM_UNIT_OFFLINE ? S_AL_UNIT_OFFLINE : S_AL_UNIT_WAIT;
  lv_label_set_text(A.unit, tr(us));
  lv_obj_set_style_text_color(A.unit, s.unit == ALARM_UNIT_ONLINE ? C_TEXT2
                                     : s.unit == ALARM_UNIT_OFFLINE ? C_ORANGE : ui_col(0x8E8E93), 0);

  // ---- switches follow changes made elsewhere (Blynk, scenes)
  set_switch(A.sw_motion, motionArmed);
  set_switch(A.sw_safety, safetyArmed);
}

// Switching on: no PIN. Switching off: only with the PIN (keypad) -> undo.
static void switch_cb(lv_event_t* e) {
  lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  bool motion = (sw == A.sw_motion);
  if (on) {
    if (motion) { g_data.alarm_armed = true; data_mark_dirty(); }
    else        udp_alarm_set_safety_armed(true);
    show_msg(tr(S_ALARM_ARMED_MSG), C_GREEN);
  } else {
    set_switch(sw, true);
    show_msg(tr(S_AL_USE_PIN), C_ORANGE);
  }
  update_status();
}

static void key_cb(lv_event_t* e) {
  lv_obj_t* m = (lv_obj_t*)lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(m);
  if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
  const char* k = lv_buttonmatrix_get_button_text(m, id);
  size_t n = strlen(A.pin);

  if (strcmp(k, LV_SYMBOL_BACKSPACE) == 0) { if (n) A.pin[n - 1] = 0; }
  else if (strcmp(k, LV_SYMBOL_OK) == 0) {
    if (strcmp(A.pin, g_data.alarm_pin) == 0) {
      ui_alarm_disarm_all();                        // motion + fire / smoke / gas off, siren off
      show_msg(tr(S_AL_ALL_DISARMED), C_GREEN);
    } else {
      show_msg(tr(S_WRONG_PIN), C_RED);
    }
    A.pin[0] = 0;
  }
  else if (n < 6) { A.pin[n] = k[0]; A.pin[n + 1] = 0; }
  show_pin();
  update_status();
}

static void page_timer_cb(lv_timer_t* t) {
  A.blink = !A.blink;
  if (g_data.alarm_active) {
    lv_obj_set_style_bg_color(A.scr, A.blink ? ui_col(0x5A0A0A) : C_BG, 0);
    lv_obj_set_style_bg_color(A.status_card, A.blink ? ui_col(0x7A1010) : C_TILE, 0);
    M5.Speaker.tone(A.blink ? 2600 : 1800, 450);
  } else {
    lv_obj_set_style_bg_color(A.scr, C_BG, 0);
    lv_obj_set_style_bg_color(A.status_card, C_TILE, 0);
  }
  update_status();
}

// Card with icon, title, subtitle and a switch
static lv_obj_t* switch_card(lv_obj_t* scr, int y, const char* sym, uint32_t icol,
                             const char* title, const char* sub) {
  lv_obj_t* c = ui_tile_create(scr, 20, y, 600, 134, C_TILE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(c, 256, LV_STATE_PRESSED);
  lv_obj_t* ic = ui_icon_circle(c, sym, 72, ui_col(icol), ui_col(0xFFFFFF));
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* t = ui_label(c, title, FONT_28, C_TEXT);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 92, -18);
  lv_obj_t* s = ui_label(c, sub, FONT_20, C_TEXT2);
  lv_obj_set_width(s, 330);
  lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
  lv_obj_align(s, LV_ALIGN_LEFT_MID, 92, 20);

  lv_obj_t* sw = lv_switch_create(c);
  lv_obj_set_size(sw, 110, 60);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_radius(sw, 30, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, 30, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sw, 26, LV_PART_KNOB);
  lv_obj_set_style_bg_color(sw, ui_col(0x3A3A40), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return sw;
}

lv_obj_t* alarm_create() {
  memset(&A, 0, sizeof(A));
  lv_obj_t* scr = ui_screen_create();
  A.scr = scr;
  menu_create(scr, tr(S_ALARM), LV_SYMBOL_BELL, MENU_SUBPAGE, ui_col(0x5A1E1E));

  // ---------------- status card (20, 108, 600 x 300)
  A.status_card = ui_tile_create(scr, 20, 108, 600, 300, C_TILE);
  lv_obj_remove_flag(A.status_card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(A.status_card, 256, LV_STATE_PRESSED);

  A.icon = ui_icon_circle(A.status_card, LV_SYMBOL_EYE_CLOSE, 104, C_TILE2, ui_col(0xFFFFFF));
  lv_obj_set_style_text_font(lv_obj_get_child(A.icon, 0), FONT_40, 0);
  lv_obj_align(A.icon, LV_ALIGN_TOP_LEFT, 0, 0);

  A.status = ui_label(A.status_card, "", FONT_40, C_TEXT);
  lv_obj_align(A.status, LV_ALIGN_TOP_LEFT, 124, 4);
  A.reason = ui_label(A.status_card, "", FONT_20, C_TEXT2);
  lv_obj_set_width(A.reason, 440);
  lv_label_set_long_mode(A.reason, LV_LABEL_LONG_WRAP);
  lv_obj_align(A.reason, LV_ALIGN_TOP_LEFT, 124, 58);

  // Detector states: Motion | Fire | Smoke | Gas
  static const StrId DET_NAME[DET_COUNT] = {S_MOTION, S_AL_FIRE, S_AL_SMOKE, S_AL_GAS};
  for (int i = 0; i < DET_COUNT; i++) {
    lv_obj_t* chip = lv_obj_create(A.status_card);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 132, 52);
    lv_obj_set_pos(chip, i * 144, 150);
    lv_obj_set_style_radius(chip, 26, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, C_TILE2, 0);
    lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    A.det_dot[i] = lv_obj_create(chip);
    lv_obj_remove_style_all(A.det_dot[i]);
    lv_obj_set_size(A.det_dot[i], 16, 16);
    lv_obj_set_style_radius(A.det_dot[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(A.det_dot[i], LV_OPA_COVER, 0);
    lv_obj_align(A.det_dot[i], LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t* l = ui_label(chip, tr(DET_NAME[i]), FONT_20, C_TEXT);
    lv_obj_set_width(l, 86);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 40, 0);
  }
  A.unit = ui_label(A.status_card, "", FONT_20, C_TEXT2);
  lv_obj_align(A.unit, LV_ALIGN_BOTTOM_LEFT, 4, 0);

  // ---------------- the two alarm groups
  A.sw_motion = switch_card(scr, 420, LV_SYMBOL_EYE_OPEN, 0x3A2A6A, tr(S_AL_MOTION_ALARM), tr(S_AL_MOTION_SUB));
  char sub[64];                                     // "Fire  •  Smoke  •  Gas"
  snprintf(sub, sizeof(sub), "%s  •  %s  •  %s", tr(S_AL_FIRE), tr(S_AL_SMOKE), tr(S_AL_GAS));
  A.sw_safety = switch_card(scr, 566, LV_SYMBOL_WARNING, 0x8A3A10, tr(S_AL_SAFETY), sub);

  // ---------------- PIN keypad: disarms every alarm
  lv_obj_t* k = ui_tile_create(scr, 640, 108, 620, 592, C_TILE);
  lv_obj_remove_flag(k, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(k, 256, LV_STATE_PRESSED);
  lv_obj_t* kt = ui_label(k, tr(S_AL_PIN_ALL), FONT_24, C_TEXT2);
  lv_obj_align(kt, LV_ALIGN_TOP_MID, 0, 0);
  A.pin_lbl = ui_label(k, "", FONT_40, C_TEXT);
  lv_obj_align(A.pin_lbl, LV_ALIGN_TOP_MID, 0, 40);
  A.msg = ui_label(k, "", FONT_20, C_TEXT2);
  lv_obj_align(A.msg, LV_ALIGN_TOP_MID, 0, 94);

  lv_obj_t* m = lv_buttonmatrix_create(k);
  lv_buttonmatrix_set_map(m, KEYS);
  lv_obj_set_size(m, 560, 420);
  lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(m, 0, 0);
  lv_obj_set_style_pad_all(m, 4, 0);
  lv_obj_set_style_pad_gap(m, 14, 0);
  lv_obj_set_style_radius(m, 24, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, C_TILE2, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, ui_col(0x48484E), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(m, C_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_text_font(m, FONT_32, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
  lv_obj_add_event_cb(m, key_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  show_pin();
  update_status();
  ui_page_timer(scr, page_timer_cb, 600, nullptr);
  return scr;
}
