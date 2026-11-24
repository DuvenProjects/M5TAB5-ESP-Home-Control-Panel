// ============================================================================
//  alarm.cpp  -  Alarm page
//   Left  : status card (disarmed / armed / ALARM! + reason) with the detector
//           states (motion, fire, smoke, gas) and the alarm unit state,
//           two switches: motion alarm  |  fire / smoke / gas alarm
//   Right : PIN keypad: the correct PIN switches EVERY alarm off and stops a
//           ringing alarm (default PIN 1234)
//   Switching an alarm on needs no PIN; switching it off needs the PIN.
//   While an alarm rings: blinking red background + siren (alarm_sound.cpp)
//   Speaker buttons on the two alarm cards: siren of motion / fire / smoke /
//   gas on or off. Gear button (status card): Alarm settings window with the
//   two siren volumes, the camera switch, the e-mail address and "Send photo
//   now" (camera_mail.cpp).
// ============================================================================
#include <M5Unified.h>
#include "alarm.h"
#include "ui.h"
#include "menu.h"
#include "settings.h"    // motion sensor switch
#include "udp_alarm.h"   // fire / smoke / gas alarm unit
#include "alarm_sound.h" // siren switches and volumes
#include "camera_mail.h" // camera, e-mail address, photo

enum { DET_MOTION = 0, DET_FIRE, DET_SMOKE, DET_GAS, DET_COUNT };

static struct {
  lv_obj_t *scr, *status_card, *icon, *status, *reason;
  lv_obj_t *det_dot[DET_COUNT], *unit;
  lv_obj_t *sw_motion, *sw_safety;
  lv_obj_t *pin_lbl, *msg;
  lv_obj_t *snd[ADET_COUNT];                       // speaker buttons
  // Alarm settings window
  lv_obj_t *dlg, *vol_sl[ATONE_COUNT], *vol_lbl[ATONE_COUNT], *cam_sw, *mail_lbl, *cam_st;
  lv_obj_t *rot[4];                                // photo rotation 0 / 90 / 180 / 270
  lv_obj_t *ed, *ed_ta, *ed_msg;                   // e-mail editor (keyboard)
  uint32_t dlg_snd_seq, dlg_cam_seq;
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

// ============================================================ SIREN SWITCHES
static const StrId DET_NAME[DET_COUNT] = {S_MOTION, S_AL_FIRE, S_AL_SMOKE, S_AL_GAS};

static void style_sound_button(int i) {
  lv_obj_t* b = A.snd[i];
  if (!b) return;
  bool on = alarm_sound_enabled(i);
  lv_obj_set_style_bg_color(b, on ? ui_col(0x1F4A2C) : C_TILE2, 0);
  lv_obj_set_style_border_color(b, on ? C_GREEN : ui_col(0x55555C), 0);
  lv_obj_t* l = lv_obj_get_child(b, 0);
  lv_label_set_text_fmt(l, "%s  %s", on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE, tr(DET_NAME[i]));
  lv_obj_set_style_text_color(l, on ? C_TEXT : C_TEXT2, 0);
}

static void sound_btn_cb(lv_event_t* e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  alarm_sound_set_enabled(i, !alarm_sound_enabled(i));
  style_sound_button(i);
  show_msg(tr(S_AL_SIREN_HINT), C_TEXT2);
}

static lv_obj_t* sound_button(lv_obj_t* parent, int det) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, LV_SIZE_CONTENT, 40);
  lv_obj_set_style_pad_hor(b, 14, 0);
  lv_obj_set_style_radius(b, 20, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = ui_label(b, "", FONT_16, C_TEXT);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, sound_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)det);
  A.snd[det] = b;
  style_sound_button(det);
  return b;
}

// ============================================================ ALARM SETTINGS WINDOW
static void dlg_update();

static void dlg_close_cb(lv_event_t* e) {
  (void)e;
  if (A.dlg) { lv_obj_delete(A.dlg); A.dlg = nullptr; }
  for (int i = 0; i < ATONE_COUNT; i++) A.vol_sl[i] = A.vol_lbl[i] = nullptr;
  for (int i = 0; i < 4; i++) A.rot[i] = nullptr;
  A.cam_sw = A.mail_lbl = A.cam_st = nullptr;
}

static void vol_cb(lv_event_t* e) {
  int t = (int)(intptr_t)lv_event_get_user_data(e);
  lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    alarm_sound_set_volume(t, (uint8_t)lv_slider_get_value(s));
    lv_label_set_text_fmt(A.vol_lbl[t], "%d %%", (int)alarm_sound_volume(t));
  } else if (!g_data.alarm_active) {                // released: let the user hear it
    alarm_sound_preview(t);
  }
  A.dlg_snd_seq = alarm_sound_change_seq();
}

static void test_cb(lv_event_t* e) {
  alarm_sound_log_state("test");
  if (!g_data.alarm_active) alarm_sound_preview((int)(intptr_t)lv_event_get_user_data(e));
}

static void cam_sw_cb(lv_event_t* e) {
  cam_set_enabled(lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED));
  dlg_update();
}

// Photo rotation of the current orientation (landscape / portrait)
static void rot_cb(lv_event_t* e) {
  cam_set_rotation_steps(ui_portrait(), (uint8_t)(intptr_t)lv_event_get_user_data(e));
  dlg_update();
}

static void send_now_cb(lv_event_t* e) {
  (void)e;
  cam_request(CAM_REQ_LOCAL, nullptr);
  dlg_update();
}

static lv_obj_t* pill_button(lv_obj_t* parent, const char* txt, lv_color_t bg, lv_event_cb_t cb, void* ud) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, LV_SIZE_CONTENT, 56);
  lv_obj_set_style_pad_hor(b, 24, 0);
  lv_obj_set_style_radius(b, 28, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = ui_label(b, txt, FONT_20, C_TEXT);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

// ---- e-mail editor: text field + keyboard, covers the screen
static void ed_close() {
  if (A.ed) { lv_obj_delete(A.ed); A.ed = A.ed_ta = A.ed_msg = nullptr; }
}

static void ed_save() {
  if (!A.ed_ta) return;
  if (!cam_set_mail_to(lv_textarea_get_text(A.ed_ta))) {
    lv_label_set_text(A.ed_msg, tr(S_CAM_MAIL_INVALID));
    return;
  }
  ed_close();
  dlg_update();
}

static void ed_kb_cb(lv_event_t* e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_READY) ed_save();
  else if (c == LV_EVENT_CANCEL) ed_close();
}
static void ed_save_cb(lv_event_t* e)   { (void)e; ed_save(); }
static void ed_cancel_cb(lv_event_t* e) { (void)e; ed_close(); }

static void mail_edit_cb(lv_event_t* e) {
  (void)e;
  if (A.ed) return;
  A.ed = lv_obj_create(A.scr);
  lv_obj_remove_style_all(A.ed);
  lv_obj_set_size(A.ed, ui_scr_w(), ui_scr_h());
  lv_obj_set_style_bg_opa(A.ed, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(A.ed, C_BG, 0);
  lv_obj_add_flag(A.ed, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(A.ed, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* t = ui_label(A.ed, tr(S_CAM_MAIL), FONT_28, C_TEXT);
  lv_obj_set_pos(t, 40, 30);
  A.ed_ta = lv_textarea_create(A.ed);
  lv_textarea_set_one_line(A.ed_ta, true);
  lv_textarea_set_max_length(A.ed_ta, 63);
  lv_textarea_set_placeholder_text(A.ed_ta, "name@example.com");
  lv_textarea_set_text(A.ed_ta, cam_mail_to());
  lv_obj_set_style_text_font(A.ed_ta, FONT_28, 0);
  lv_obj_set_style_bg_color(A.ed_ta, C_TILE2, 0);
  lv_obj_set_style_text_color(A.ed_ta, C_TEXT, 0);
  lv_obj_set_style_border_color(A.ed_ta, C_ORANGE, 0);
  lv_obj_set_style_radius(A.ed_ta, 16, 0);
  lv_obj_set_width(A.ed_ta, ui_scr_w() - 80);
  lv_obj_set_pos(A.ed_ta, 40, 84);
  lv_obj_add_state(A.ed_ta, LV_STATE_FOCUSED);       // shows the cursor

  A.ed_msg = ui_label(A.ed, "", FONT_20, C_RED);
  lv_obj_set_pos(A.ed_msg, 40, 176);
  lv_obj_t* bs = pill_button(A.ed, tr(S_SAVE), ui_col(0x248A3D), ed_save_cb, nullptr);
  lv_obj_align(bs, LV_ALIGN_TOP_RIGHT, -40, 164);
  lv_obj_t* bc = pill_button(A.ed, tr(S_CANCEL), C_TILE2, ed_cancel_cb, nullptr);
  lv_obj_align_to(bc, bs, LV_ALIGN_OUT_LEFT_MID, -16, 0);

  lv_obj_t* kb = lv_keyboard_create(A.ed);
  lv_obj_set_size(kb, ui_scr_w(), UI_LP(440, 520));
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  ui_style_keyboard(kb);                            // large letters and icons
  lv_keyboard_set_textarea(kb, A.ed_ta);
  lv_obj_add_event_cb(kb, ed_kb_cb, LV_EVENT_ALL, nullptr);
}

// Values of the window follow changes made elsewhere (Blynk, e-mail task)
static void dlg_update() {
  if (!A.dlg) return;
  for (int t = 0; t < ATONE_COUNT; t++) {
    if (!A.vol_sl[t]) continue;
    if (!lv_obj_has_state(A.vol_sl[t], LV_STATE_PRESSED))
      lv_slider_set_value(A.vol_sl[t], alarm_sound_volume(t), LV_ANIM_OFF);
    lv_label_set_text_fmt(A.vol_lbl[t], "%d %%", (int)alarm_sound_volume(t));
  }
  if (A.cam_sw) {
    if (cam_enabled()) lv_obj_add_state(A.cam_sw, LV_STATE_CHECKED);
    else               lv_obj_remove_state(A.cam_sw, LV_STATE_CHECKED);
  }
  for (int i = 0; i < 4; i++) {
    if (!A.rot[i]) continue;
    bool sel = cam_rotation_steps(ui_portrait()) == i;
    lv_obj_set_style_bg_color(A.rot[i], sel ? C_ORANGE : C_TILE2, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(A.rot[i], 0), sel ? ui_col(0x000000) : C_TEXT, 0);
  }
  if (A.mail_lbl) {
    const char* m = cam_mail_to();
    lv_label_set_text(A.mail_lbl, m[0] ? m : tr(S_CAM_MAIL_NONE));
    lv_obj_set_style_text_color(A.mail_lbl, m[0] ? C_TEXT : C_ORANGE, 0);
  }
  if (A.cam_st) {
    char b[200];
    cam_state_text(b, sizeof(b));
    CamState s = cam_state();
    if ((s == CAM_ST_ERR_MAIL || s == CAM_ST_ERR_CAMERA) && cam_state_detail()[0]) {
      strlcat(b, "\n", sizeof(b));                   // technical detail (English)
      strlcat(b, cam_state_detail(), sizeof(b));
    }
    lv_label_set_text(A.cam_st, b);
    lv_color_t c = (s == CAM_ST_SENT) ? C_GREEN
                 : (s == CAM_ST_CAPTURING || s == CAM_ST_SENDING || s == CAM_ST_IDLE) ? C_TEXT2 : C_ORANGE;
    lv_obj_set_style_text_color(A.cam_st, c, 0);
  }
  A.dlg_snd_seq = alarm_sound_change_seq();
  A.dlg_cam_seq = cam_change_seq();
}

// Row of the window: flex row, full width, items centred vertically
static lv_obj_t* dlg_row(lv_obj_t* parent, int h) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, LV_PCT(100), h);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 16, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

static void dlg_open_cb(lv_event_t* e) {
  (void)e;
  if (A.dlg) return;
  const bool port = ui_portrait();
  A.dlg = lv_obj_create(A.scr);                    // dimmed background, catches the touches
  lv_obj_remove_style_all(A.dlg);
  lv_obj_set_size(A.dlg, ui_scr_w(), ui_scr_h());
  lv_obj_set_style_bg_opa(A.dlg, LV_OPA_70, 0);
  lv_obj_set_style_bg_color(A.dlg, ui_col(0x000000), 0);
  lv_obj_add_flag(A.dlg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(A.dlg, LV_OBJ_FLAG_SCROLLABLE);

  const int pw = port ? 680 : 960, ph = port ? 790 : 700;
  lv_obj_t* p = lv_obj_create(A.dlg);
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, pw, ph);
  lv_obj_center(p);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p, C_TILE, 0);
  lv_obj_set_style_radius(p, 28, 0);
  lv_obj_set_style_pad_all(p, 28, 0);
  lv_obj_set_style_pad_row(p, 8, 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

  // ---- title + close
  lv_obj_t* r = dlg_row(p, 64);
  lv_obj_t* t = ui_label(r, tr(S_AL_SETTINGS), FONT_28, C_TEXT);
  lv_obj_set_flex_grow(t, 1);
  lv_obj_t* x = ui_icon_circle(r, LV_SYMBOL_CLOSE, 60, C_TILE2, C_TEXT);
  lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(x, 12);
  lv_obj_add_event_cb(x, dlg_close_cb, LV_EVENT_CLICKED, nullptr);

  // ---- siren volumes: name | slider | % | Test  (portrait: name above)
  ui_label(p, tr(S_AL_SIREN_VOL), FONT_20, C_TEXT2);
  const StrId VN[ATONE_COUNT] = {S_AL_MOTION_ALARM, S_AL_SAFETY};
  for (int i = 0; i < ATONE_COUNT; i++) {
    if (port) {
      lv_obj_t* n = ui_label(p, tr(VN[i]), FONT_24, C_TEXT);
      (void)n;
    }
    r = dlg_row(p, 64);
    lv_obj_set_style_pad_column(r, 28, 0);           // room for the slider knob
    if (!port) {
      lv_obj_t* n = ui_label(r, tr(VN[i]), FONT_24, C_TEXT);
      lv_obj_set_width(n, 290);
      lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    }
    lv_obj_t* s = lv_slider_create(r);
    lv_obj_set_size(s, port ? 320 : 320, 36);
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, alarm_sound_volume(i), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, C_TILE2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, i == 0 ? ui_col(0x7A5AF8) : C_RED, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, ui_col(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, vol_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
    lv_obj_add_event_cb(s, vol_cb, LV_EVENT_RELEASED, (void*)(intptr_t)i);
    A.vol_sl[i] = s;
    A.vol_lbl[i] = ui_label(r, "", FONT_24, C_TEXT);
    lv_obj_set_width(A.vol_lbl[i], 90);
    lv_obj_t* tb = pill_button(r, tr(S_AL_TEST), C_TILE2, test_cb, (void*)(intptr_t)i);
    lv_obj_set_height(tb, 48);
  }

  lv_obj_t* sep = lv_obj_create(p);
  lv_obj_remove_style_all(sep);
  lv_obj_set_size(sep, LV_PCT(100), 1);
  lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(sep, ui_col(0x3A3A40), 0);

  // ---- camera on / off
  r = dlg_row(p, 80);
  ui_icon_circle(r, LV_SYMBOL_IMAGE, 60, ui_col(0x1E3A5A), ui_col(0xFFFFFF));
  lv_obj_t* col = lv_obj_create(r);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  ui_label(col, tr(S_CAMERA), FONT_24, C_TEXT);
  lv_obj_t* sub = ui_label(col, tr(S_CAM_SUB), FONT_16, C_TEXT2);
  lv_obj_set_width(sub, LV_PCT(100));
  lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
  A.cam_sw = lv_switch_create(r);
  lv_obj_set_size(A.cam_sw, 100, 54);
  lv_obj_set_style_bg_color(A.cam_sw, ui_col(0x3A3A40), LV_PART_MAIN);
  lv_obj_set_style_bg_color(A.cam_sw, C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(A.cam_sw, cam_sw_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  // ---- e-mail address
  r = dlg_row(p, 70);
  lv_obj_t* ml = ui_label(r, tr(S_CAM_MAIL), FONT_20, C_TEXT2);
  lv_obj_set_width(ml, port ? 150 : 200);
  lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
  A.mail_lbl = ui_label(r, "", FONT_24, C_TEXT);
  lv_obj_set_flex_grow(A.mail_lbl, 1);
  lv_label_set_long_mode(A.mail_lbl, LV_LABEL_LONG_DOT);
  pill_button(r, tr(S_CAM_CHANGE), C_TILE2, mail_edit_cb, nullptr);

  // ---- photo rotation of this orientation (Rotation 180 deg is added by itself)
  r = dlg_row(p, 60);
  lv_obj_set_style_pad_column(r, 10, 0);
  char rt[64];
  snprintf(rt, sizeof(rt), "%s (%s)", tr(S_CAM_ROT), tr(port ? S_PORTRAIT : S_LANDSCAPE));
  lv_obj_t* rl = ui_label(r, rt, FONT_20, C_TEXT2);
  lv_obj_set_flex_grow(rl, 1);
  lv_label_set_long_mode(rl, LV_LABEL_LONG_WRAP);
  static const char* const ROT_TXT[4] = {"0°", "90°", "180°", "270°"};
  for (int i = 0; i < 4; i++) {
    A.rot[i] = pill_button(r, ROT_TXT[i], C_TILE2, rot_cb, (void*)(intptr_t)i);
    lv_obj_set_size(A.rot[i], port ? 88 : 96, 48);
    lv_obj_set_style_pad_hor(A.rot[i], 0, 0);
  }

  // ---- photo now + status
  r = dlg_row(p, port ? 190 : 110);
  if (port) {
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(r, 12, 0);
  }
  char sn[64];
  snprintf(sn, sizeof(sn), "%s  %s", LV_SYMBOL_IMAGE, tr(S_CAM_SEND_NOW));
  pill_button(r, sn, ui_col(0x1E5AA0), send_now_cb, nullptr);
  A.cam_st = ui_label(r, "", FONT_20, C_TEXT2);
  if (port) lv_obj_set_width(A.cam_st, LV_PCT(100));
  else      lv_obj_set_flex_grow(A.cam_st, 1);
  lv_label_set_long_mode(A.cam_st, LV_LABEL_LONG_WRAP);

  dlg_update();
}

static void page_timer_cb(lv_timer_t* t) {
  A.blink = !A.blink;
  if (A.dlg && (A.dlg_snd_seq != alarm_sound_change_seq() || A.dlg_cam_seq != cam_change_seq())) dlg_update();
  for (int i = 0; i < DET_COUNT; i++) style_sound_button(i);   // changes from Blynk
  if (g_data.alarm_active) {
    lv_obj_set_style_bg_color(A.scr, A.blink ? ui_col(0x5A0A0A) : C_BG, 0);
    lv_obj_set_style_bg_color(A.status_card, A.blink ? ui_col(0x7A1010) : C_TILE, 0);
  } else {
    lv_obj_set_style_bg_color(A.scr, C_BG, 0);
    lv_obj_set_style_bg_color(A.status_card, C_TILE, 0);
  }
  update_status();
}

// Card with icon, title, subtitle and a switch
static lv_obj_t* switch_card(lv_obj_t* scr, int y, const char* sym, uint32_t icol,
                             const char* title, const char* sub, lv_obj_t** card = nullptr) {
  lv_obj_t* c = ui_tile_create(scr, 20, y, UI_LP(600, 680), 134, C_TILE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(c, 256, LV_STATE_PRESSED);
  lv_obj_t* ic = ui_icon_circle(c, sym, 72, ui_col(icol), ui_col(0xFFFFFF));
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* t = ui_label(c, title, FONT_28, C_TEXT);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 92, -18);
  if (sub) {
    lv_obj_t* s = ui_label(c, sub, FONT_20, C_TEXT2);
    lv_obj_set_width(s, UI_LP(330, 410));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 92, 20);
  }
  if (card) *card = c;

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

  // Landscape: status + the two switches on the left, PIN keypad on the right.
  // Portrait : the same cards under each other (keypad at the bottom).
  const bool port = ui_portrait();

  // ---------------- status card (20, 108, 600 x 300)
  A.status_card = ui_tile_create(scr, 20, 108, port ? 680 : 600, 300, C_TILE);
  lv_obj_remove_flag(A.status_card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(A.status_card, 256, LV_STATE_PRESSED);

  A.icon = ui_icon_circle(A.status_card, LV_SYMBOL_EYE_CLOSE, 104, C_TILE2, ui_col(0xFFFFFF));
  lv_obj_set_style_text_font(lv_obj_get_child(A.icon, 0), FONT_40, 0);
  lv_obj_align(A.icon, LV_ALIGN_TOP_LEFT, 0, 0);

  A.status = ui_label(A.status_card, "", FONT_40, C_TEXT);
  lv_obj_align(A.status, LV_ALIGN_TOP_LEFT, 124, 4);
  A.reason = ui_label(A.status_card, "", FONT_20, C_TEXT2);
  lv_obj_set_width(A.reason, port ? 440 : 360);
  lv_label_set_long_mode(A.reason, LV_LABEL_LONG_WRAP);
  lv_obj_align(A.reason, LV_ALIGN_TOP_LEFT, 124, 58);

  // Gear: Alarm settings window (siren volumes, camera, e-mail)
  lv_obj_t* gear = ui_icon_circle(A.status_card, LV_SYMBOL_SETTINGS, 64, C_TILE2, C_TEXT);
  lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gear, 12);
  lv_obj_add_event_cb(gear, dlg_open_cb, LV_EVENT_CLICKED, nullptr);

  // Detector states: Motion | Fire | Smoke | Gas
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
  //     under the title: speaker buttons (siren on / off per alarm)
  lv_obj_t* card;
  A.sw_motion = switch_card(scr, 420, LV_SYMBOL_EYE_OPEN, 0x3A2A6A, tr(S_AL_MOTION_ALARM), nullptr, &card);
  lv_obj_t* b = sound_button(card, DET_MOTION);
  lv_obj_align(b, LV_ALIGN_LEFT_MID, 92, 22);
  lv_obj_t* ms = ui_label(card, tr(S_AL_MOTION_SUB), FONT_16, C_TEXT2);
  lv_obj_set_width(ms, UI_LP(190, 270));
  lv_label_set_long_mode(ms, LV_LABEL_LONG_DOT);
  lv_obj_align_to(ms, b, LV_ALIGN_OUT_RIGHT_MID, 14, 0);
  A.sw_safety = switch_card(scr, 566, LV_SYMBOL_WARNING, 0x8A3A10, tr(S_AL_SAFETY), nullptr, &card);
  lv_obj_t* prev = nullptr;
  for (int i = DET_FIRE; i <= DET_GAS; i++) {
    b = sound_button(card, i);
    if (!prev) lv_obj_align(b, LV_ALIGN_LEFT_MID, 92, 22);
    else       lv_obj_align_to(b, prev, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    prev = b;
  }

  // ---------------- PIN keypad: disarms every alarm
  lv_obj_t* k = port ? ui_tile_create(scr, 20, 714, 680, 546, C_TILE)
                     : ui_tile_create(scr, 640, 108, 620, 592, C_TILE);
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
  lv_obj_set_size(m, 560, port ? 380 : 420);
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
