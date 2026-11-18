// ============================================================================
//  lights.cpp  -  Blind motors + Blinds control page
//
//  Hardware: 2 relays (or H-bridge IN1 / IN2) per blind motor
//     PIN_BLINDx_UP   = 1  -> motor runs in the OPEN direction
//     PIN_BLINDx_DOWN = 1  -> motor runs in the CLOSE direction
//  Both directions are NEVER active together; a 300 ms pause is kept when
//  the direction changes.
// ============================================================================
#include <Arduino.h>
#include "lights.h"
#include "ui.h"
#include "menu.h"

const char* blind_name(int idx) { return tr(idx == 0 ? S_BLIND_LIVING : S_BLIND_BED); }

static const uint8_t PIN_UP[BLIND_COUNT]   = {PIN_BLIND1_UP,   PIN_BLIND2_UP};
static const uint8_t PIN_DOWN[BLIND_COUNT] = {PIN_BLIND1_DOWN, PIN_BLIND2_DOWN};

static float    s_pos[BLIND_COUNT];          // precise position
static int8_t   s_dir[BLIND_COUNT];          // -1 closing, 0 stopped, +1 opening
static uint32_t s_hold[BLIND_COUNT];         // pause after a direction change
static uint32_t s_overrun[BLIND_COUNT];      // extra run time at the end positions
static uint32_t s_last_ms = 0;

#define OVERRUN_MS  1500                       // at 0 / 100: run into the mechanical stop
#define DEADTIME_MS 300

static inline void out(uint8_t pin, bool on) {
  digitalWrite(pin, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

static void drive(int i, int dir) {
  out(PIN_UP[i],   dir > 0);
  out(PIN_DOWN[i], dir < 0);
  s_dir[i] = dir;
}

void blinds_hw_init() {
  for (int i = 0; i < BLIND_COUNT; i++) {
    pinMode(PIN_UP[i], OUTPUT);
    pinMode(PIN_DOWN[i], OUTPUT);
    drive(i, 0);
    s_pos[i] = g_data.blind_pos[i];
    s_hold[i] = 0; s_overrun[i] = 0;
  }
  if (g_data.blind_travel_s < 3) g_data.blind_travel_s = 20;
  s_last_ms = millis();
}

void blinds_tick() {
  uint32_t now = millis();
  uint32_t dt  = now - s_last_ms;
  s_last_ms = now;
  float step = 100.0f * dt / (g_data.blind_travel_s * 1000.0f);

  for (int i = 0; i < BLIND_COUNT; i++) {
    int target = g_data.blind_target[i];
    int want = 0;
    if      (target > s_pos[i] + 0.5f) want = +1;
    else if (target < s_pos[i] - 0.5f) want = -1;

    // short extra run at the end positions
    if (want == 0 && s_dir[i] != 0 && (target == 0 || target == 100)) {
      if (s_overrun[i] == 0) s_overrun[i] = now + OVERRUN_MS;
      if ((int32_t)(now - s_overrun[i]) < 0) want = s_dir[i];
    }
    if (want == 0) s_overrun[i] = 0;

    if (want != s_dir[i]) {
      if (s_dir[i] != 0) {                       // stop first
        drive(i, 0);
        s_hold[i] = now + DEADTIME_MS;
        if (want == 0) {                         // target reached
          s_pos[i] = target;
          g_data.blind_pos[i] = target;
          data_mark_dirty();
        }
        continue;
      }
      if ((int32_t)(now - s_hold[i]) < 0) continue;
      drive(i, want);
    }

    if (s_dir[i] != 0) {
      s_pos[i] += s_dir[i] * step;
      s_pos[i] = constrain(s_pos[i], 0.0f, 100.0f);
      g_data.blind_pos[i] = (uint8_t)lroundf(s_pos[i]);
    }
  }
}

void blinds_set_target(int idx, int pos) {
  pos = constrain(pos, 0, 100);
  for (int i = 0; i < BLIND_COUNT; i++)
    if (idx < 0 || idx == i) g_data.blind_target[i] = pos;
  data_mark_dirty();
}

void blinds_stop(int idx) {
  for (int i = 0; i < BLIND_COUNT; i++)
    if (idx < 0 || idx == i) {
      g_data.blind_target[i] = (uint8_t)lroundf(s_pos[i]);
      s_pos[i] = g_data.blind_target[i];
    }
  data_mark_dirty();
}

bool blinds_moving(int idx) { return s_dir[idx] != 0; }

int blinds_average() {
  int s = 0;
  for (int i = 0; i < BLIND_COUNT; i++) s += g_data.blind_pos[i];
  return s / BLIND_COUNT;
}

// ============================================================ PAGE
// Each blind is shown as a window with two curtains. The curtains follow the
// real motor position; dragging on the window (finger away from the centre =
// more open) sets a new target, shown by two white marks until it is reached.
#define WIN_W      560
#define WIN_H      180
#define ROD_H      14                        // curtain rod at the top
#define CUR_MIN    36                        // width of one curtain when fully open
#define FOLDS      5                         // fabric folds per curtain

static struct {
  lv_obj_t* pct[BLIND_COUNT];
  lv_obj_t* state[BLIND_COUNT];
  lv_obj_t* win[BLIND_COUNT];
  lv_obj_t* cur_l[BLIND_COUNT];
  lv_obj_t* cur_r[BLIND_COUNT];
  lv_obj_t* mark_l[BLIND_COUNT];
  lv_obj_t* mark_r[BLIND_COUNT];
  int       drag[BLIND_COUNT];               // -1 = not dragging, else 0..100
  int       shown_w[BLIND_COUNT];            // current curtain width (px)
  lv_obj_t* travel;
} P;

#define BL_PURPLE     ui_col(0x9C6ADE)
#define BL_PURPLE_BG  ui_col(0x2E2440)

// Curtain width for an opening of pos % (0 = closed, 100 = open)
static int curtain_w(float pos) {
  float half = WIN_W / 2.0f + 2;             // closed: the two curtains overlap 4 px
  return (int)lroundf(half - (half - CUR_MIN) * constrain(pos, 0.0f, 100.0f) / 100.0f);
}

static void anim_w_cb(void* o, int32_t v) { lv_obj_set_width((lv_obj_t*)o, v); }

// Moves both curtains of blind i to width w (short animation between updates)
static void curtains_to(int i, int w, uint32_t ms) {
  if (w == P.shown_w[i]) return;
  lv_obj_t* cs[2] = {P.cur_l[i], P.cur_r[i]};
  for (lv_obj_t* c : cs) {
    lv_anim_delete(c, anim_w_cb);
    if (ms == 0) { lv_obj_set_width(c, w); continue; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c);
    lv_anim_set_exec_cb(&a, anim_w_cb);
    lv_anim_set_values(&a, lv_obj_get_width(c), w);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
  }
  P.shown_w[i] = w;
}

// Target marks (white lines where the curtain edges will stop)
static void show_marks(int i, int target) {
  bool show = target >= 0;
  lv_obj_t* ms[2] = {P.mark_l[i], P.mark_r[i]};
  for (lv_obj_t* m : ms) {
    if (show) lv_obj_remove_flag(m, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
  }
  if (!show) return;
  int w = curtain_w(target);
  lv_obj_set_x(P.mark_l[i], w - 2);
  lv_obj_set_x(P.mark_r[i], WIN_W - w - 1);
}

// Finger position on the window -> opening (0..100 %)
static int drag_pos(int i) {
  lv_point_t pt;
  lv_indev_get_point(lv_indev_active(), &pt);
  lv_area_t a;
  lv_obj_get_coords(P.win[i], &a);
  int cx = (a.x1 + a.x2) / 2;
  int dx = abs(pt.x - cx);
  int span = WIN_W / 2 - CUR_MIN - 10;
  return constrain((dx - 10) * 100 / span, 0, 100);
}

static void win_cb(lv_event_t* e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSING) {
    P.drag[i] = drag_pos(i);
    lv_label_set_text_fmt(P.pct[i], "%d%%", P.drag[i]);
    show_marks(i, P.drag[i]);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (P.drag[i] >= 0) blinds_set_target(i, P.drag[i]);
    P.drag[i] = -1;
  }
}

// One curtain: a row of folds (horizontal gradients) + a darker hem
static lv_obj_t* curtain(lv_obj_t* win, bool left) {
  lv_obj_t* c = lv_obj_create(win);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, CUR_MIN, WIN_H - ROD_H + 4);
  lv_obj_align(c, left ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, 0, ROD_H - 4);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_shadow_width(c, 18, 0);
  lv_obj_set_style_shadow_color(c, lv_color_hex(0x000000), 0);
  lv_obj_set_style_shadow_opa(c, LV_OPA_50, 0);
  lv_obj_set_style_shadow_offset_x(c, left ? 4 : -4, 0);
  lv_obj_remove_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  for (int k = 0; k < FOLDS; k++) {
    lv_obj_t* f = lv_obj_create(c);
    lv_obj_remove_style_all(f);
    lv_obj_set_height(f, LV_PCT(100));
    lv_obj_set_flex_grow(f, 1);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(0x5B3E8E), 0);       // fold shadow
    lv_obj_set_style_bg_grad_color(f, lv_color_hex(0x9C7BD6), 0);  // fold highlight
    lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_main_stop(f, 0, 0);
    lv_obj_set_style_bg_grad_stop(f, 200, 0);
    lv_obj_remove_flag(f, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  }
  return c;
}

// Window: sky, sun, hills, frame cross, rod, two curtains, target marks
static lv_obj_t* window_create(lv_obj_t* p, int i) {
  lv_obj_t* w = lv_obj_create(p);
  lv_obj_remove_style_all(w);
  lv_obj_set_size(w, WIN_W, WIN_H);
  lv_obj_set_style_radius(w, 16, 0);
  lv_obj_set_style_clip_corner(w, true, 0);
  lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(w, lv_color_hex(0x3D7FD0), 0);         // sky (real colours,
  lv_obj_set_style_bg_grad_color(w, lv_color_hex(0xBFE3FF), 0);    //  not themed)
  lv_obj_set_style_bg_grad_dir(w, LV_GRAD_DIR_VER, 0);
  lv_obj_remove_flag(w, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(w, 256, LV_STATE_PRESSED);

  auto blob = [&](int x, int y, int bw, int bh, uint32_t col) {
    lv_obj_t* o = lv_obj_create(w);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, bw, bh);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
  };
  lv_obj_t* sun = blob(400, 34, 40, 40, 0xFFD54A);
  lv_obj_set_style_shadow_width(sun, 30, 0);
  lv_obj_set_style_shadow_color(sun, lv_color_hex(0xFFE082), 0);
  blob(120, 46, 70, 22, 0xFFFFFF);                                  // clouds
  blob(150, 36, 50, 26, 0xFFFFFF);
  blob(-80, 128, 420, 120, 0x4C9A5A);                               // hills
  blob(220, 138, 460, 120, 0x3E8450);

  lv_obj_t* vbar = blob(WIN_W / 2 - 3, 0, 6, WIN_H, 0xE6E0D6);      // window frame cross
  lv_obj_set_style_radius(vbar, 0, 0);
  lv_obj_t* hbar = blob(0, WIN_H / 2 + 4, WIN_W, 6, 0xE6E0D6);
  lv_obj_set_style_radius(hbar, 0, 0);

  P.cur_l[i] = curtain(w, true);
  P.cur_r[i] = curtain(w, false);

  for (int k = 0; k < 2; k++) {                                     // target marks
    lv_obj_t* m = blob(0, ROD_H, 3, WIN_H - ROD_H, 0xFFFFFF);
    lv_obj_set_style_radius(m, 2, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_80, 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    if (k == 0) P.mark_l[i] = m; else P.mark_r[i] = m;
  }

  lv_obj_t* rod = blob(6, 3, WIN_W - 12, 10, 0x8A6E4B);             // curtain rod
  lv_obj_set_style_radius(rod, 5, 0);

  P.drag[i] = -1;
  P.shown_w[i] = -1;
  lv_obj_add_event_cb(w, win_cb, LV_EVENT_PRESSING, (void*)(intptr_t)i);
  lv_obj_add_event_cb(w, win_cb, LV_EVENT_RELEASED, (void*)(intptr_t)i);
  lv_obj_add_event_cb(w, win_cb, LV_EVENT_PRESS_LOST, (void*)(intptr_t)i);
  return w;
}

static void btn_cb(lv_event_t* e) {
  intptr_t code = (intptr_t)lv_event_get_user_data(e);
  int i   = (int)(code / 10) - 1;          // -1 = all
  int act = code % 10;                     // 0 open, 1 stop, 2 close, 3 half
  if (act == 0) blinds_set_target(i, 100);
  if (act == 1) blinds_stop(i);
  if (act == 2) blinds_set_target(i, 0);
  if (act == 3) blinds_set_target(i, 50);
}

static void travel_cb(lv_event_t* e) {
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  g_data.blind_travel_s = constrain((int)g_data.blind_travel_s + d, 3, 120);
  lv_label_set_text_fmt(P.travel, "%d s", g_data.blind_travel_s);
  data_mark_dirty();
}

// Button with an icon / short text and an optional translated text (S_COUNT = none)
static lv_obj_t* action_btn(lv_obj_t* p, const char* txt, StrId text_id, int w, intptr_t code,
                            lv_event_cb_t cb, lv_color_t bg) {
  lv_obj_t* b = lv_button_create(p);
  lv_obj_set_size(b, w, 66);
  lv_obj_set_style_radius(b, 22, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = ui_label(b, txt, FONT_20, C_TEXT);
  if (text_id < S_COUNT) lv_label_set_text_fmt(l, "%s  %s", txt, tr(text_id));
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)code);
  return b;
}

#define PAGE_MS 100                          // refresh period = curtain animation step

static void page_timer_cb(lv_timer_t* t) {
  bool first = (t == nullptr);
  for (int i = 0; i < BLIND_COUNT; i++) {
    int pos = g_data.blind_pos[i];
    curtains_to(i, curtain_w(s_pos[i]), first ? 0 : PAGE_MS);   // follows the motor
    if (P.drag[i] < 0) {
      lv_label_set_text_fmt(P.pct[i], "%d%%", pos);
      bool moving = blinds_moving(i) || abs((int)g_data.blind_target[i] - pos) > 0;
      show_marks(i, moving ? g_data.blind_target[i] : -1);
    }
    if (blinds_moving(i)) {
      bool up = g_data.blind_target[i] > pos;
      lv_label_set_text_fmt(P.state[i], "%s  %s", up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                            tr(up ? S_OPENING : S_CLOSING));
    } else {
      lv_label_set_text(P.state[i], tr(pos == 0 ? S_CLOSED : (pos == 100 ? S_OPEN_STATE : S_PARTLY_OPEN)));
    }
  }
}

lv_obj_t* lights_create() {
  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_BLINDS), LV_SYMBOL_LIST, MENU_SUBPAGE, BL_PURPLE_BG);

  for (int i = 0; i < BLIND_COUNT; i++) {
    lv_obj_t* c = ui_tile_create(scr, 20 + i * 630, 108, 610, 430, C_TILE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_scale(c, 256, LV_STATE_PRESSED);

    lv_obj_t* ic = ui_icon_circle(c, LV_SYMBOL_LIST, 64, BL_PURPLE_BG, BL_PURPLE);
    lv_obj_set_pos(ic, 6, 6);
    lv_obj_t* n = ui_label(c, blind_name(i), FONT_28, C_TEXT);
    lv_obj_set_pos(n, 90, 8);
    P.state[i] = ui_label(c, "", FONT_20, C_TEXT2);
    lv_obj_set_pos(P.state[i], 90, 44);

    P.pct[i] = ui_label(c, "0%", FONT_48, C_TEXT);
    lv_obj_align(P.pct[i], LV_ALIGN_TOP_RIGHT, -10, 10);

    P.win[i] = window_create(c, i);                 // curtains instead of a slider
    lv_obj_set_pos(P.win[i], 7, 104);

    intptr_t base = (i + 1) * 10;
    lv_obj_t* b;
    b = action_btn(c, LV_SYMBOL_UP, S_BTN_OPEN, 170, base + 0, btn_cb, ui_col(0x3A2E55));
    lv_obj_set_pos(b, 7, 300);
    b = action_btn(c, LV_SYMBOL_STOP, S_STOP, 170, base + 1, btn_cb, ui_col(0x3A3A40));
    lv_obj_set_pos(b, 202, 300);
    b = action_btn(c, LV_SYMBOL_DOWN, S_BTN_CLOSE, 170, base + 2, btn_cb, ui_col(0x3A2E55));
    lv_obj_set_pos(b, 397, 300);
  }

  // ---- bottom bar: all blinds + motor travel time
  lv_obj_t* a = ui_tile_create(scr, 20, 556, 1240, 144, C_TILE);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(a, 256, LV_STATE_PRESSED);
  lv_obj_t* at = ui_label(a, tr(S_ALL_BLINDS), FONT_20, C_TEXT);
  lv_obj_set_width(at, 150);
  lv_label_set_long_mode(at, LV_LABEL_LONG_WRAP);
  lv_obj_align(at, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_t* b;
  b = action_btn(a, LV_SYMBOL_UP, S_OPEN_ALL, 180, 0, btn_cb, BL_PURPLE);
  lv_obj_align(b, LV_ALIGN_LEFT_MID, 170, 0);
  b = action_btn(a, "50%", (StrId)S_COUNT, 110, 3, btn_cb, ui_col(0x3A2E55));
  lv_obj_align(b, LV_ALIGN_LEFT_MID, 365, 0);
  b = action_btn(a, LV_SYMBOL_STOP, S_STOP, 140, 1, btn_cb, ui_col(0x3A3A40));
  lv_obj_align(b, LV_ALIGN_LEFT_MID, 490, 0);
  b = action_btn(a, LV_SYMBOL_DOWN, S_CLOSE_ALL, 190, 2, btn_cb, ui_col(0x3A2E55));
  lv_obj_align(b, LV_ALIGN_LEFT_MID, 645, 0);

  lv_obj_t* tl = ui_label(a, tr(S_MOTOR_TRAVEL), FONT_16, C_TEXT2);
  lv_obj_align(tl, LV_ALIGN_RIGHT_MID, -240, 0);
  b = action_btn(a, LV_SYMBOL_MINUS, (StrId)S_COUNT, 66, -1, travel_cb, ui_col(0x3A3A40));
  lv_obj_align(b, LV_ALIGN_RIGHT_MID, -160, 0);
  P.travel = ui_label(a, "", FONT_20, C_TEXT);
  lv_label_set_text_fmt(P.travel, "%d s", g_data.blind_travel_s);
  lv_obj_align(P.travel, LV_ALIGN_RIGHT_MID, -96, 0);
  b = action_btn(a, LV_SYMBOL_PLUS, (StrId)S_COUNT, 66, 1, travel_cb, ui_col(0x3A3A40));
  lv_obj_align(b, LV_ALIGN_RIGHT_MID, -10, 0);

  ui_page_timer(scr, page_timer_cb, PAGE_MS, nullptr);
  page_timer_cb(nullptr);                          // first update without animation
  return scr;
}
