// ============================================================================
//  clock.cpp  -  Clock styles (Home clock tile) + style selection page
//
//  Every style is a "face": a transparent CLOCK_FACE_W x CLOCK_FACE_H object
//  that shows the time and the date. clock_face_update() refreshes it once a
//  second (Home page timer, style page timer).
//
//   Digital                         Analog (lv_scale + line needles)
//   0 Classic  big time + seconds   5 Classic  numbers 1..12
//   1 Neon     glowing, seconds bar 6 Minimal  dots, thin hands
//   2 Flip     four flip cards      7 Roman    cream dial, I..XII
//   3 LCD      7-segment display    8 Sport    seconds ring, 12/3/6/9
//   4 Stacked  hours over minutes   9 Hybrid   small dial + digital time
// ============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "clock.h"
#include "ui.h"
#include "menu.h"
#include "settings.h"        // 12 / 24 h, date format

// ------------------------------------------------------------------ style
static uint8_t s_theme = 0xFF;                    // 0xFF = not loaded yet

uint8_t clock_theme() {
  if (s_theme == 0xFF) {
    Preferences p;
    s_theme = 0;
    if (p.begin("clock", true)) {                 // read only (false: no saved style yet)
      s_theme = p.getUChar("theme", 0);
      p.end();
    }
    if (s_theme >= CLOCK_THEME_COUNT) s_theme = 0;
  }
  return s_theme;
}

void clock_set_theme(uint8_t theme) {
  if (theme >= CLOCK_THEME_COUNT) return;
  s_theme = theme;
  Preferences p;
  if (p.begin("clock", false)) {
    p.putUChar("theme", theme);
    p.end();
  }
}

const char* clock_theme_name(uint8_t theme) {
  static const StrId N[CLOCK_THEME_COUNT] = {
    S_CLK_CLASSIC, S_CLK_NEON, S_CLK_FLIP, S_CLK_LCD, S_CLK_STACKED,
    S_CLK_CLASSIC, S_CLK_MINIMAL, S_CLK_ROMAN, S_CLK_SPORT, S_CLK_HYBRID};
  return tr(N[theme % CLOCK_THEME_COUNT]);
}

// ------------------------------------------------------------------ time
struct Now {
  bool valid;
  int  h, m, s, wday;
  int  hd;                    // hour to display (24 h or 1..12)
  bool pm;
  char date[24];
};

static void now_get(Now& n) {
  n.valid = ui_time_valid();
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  n.h = lt.tm_hour; n.m = lt.tm_min; n.s = lt.tm_sec; n.wday = lt.tm_wday;
  n.pm = n.h >= 12;
  n.hd = settings_time24() ? n.h : (n.h % 12 == 0 ? 12 : n.h % 12);
  if (!settings_format_date(n.date, sizeof(n.date))) strcpy(n.date, "--");
}

// Small text next to the time: seconds (24 h) or AM / PM (12 h)
static const char* small_text(const Now& n, char* b, size_t sz) {
  if (!settings_time24()) return tr(n.pm ? S_PM : S_AM);
  snprintf(b, sz, ":%02d", n.s);
  return b;
}

// ------------------------------------------------------------------ face data
struct Face {
  uint8_t   theme;
  lv_obj_t *time, *small, *day, *date;            // labels (not every style uses all)
  lv_obj_t *dig[4];                               // flip: digit labels
  lv_obj_t *seg[4][7];                            // LCD: segments a..g
  lv_obj_t *colon[2];                             // LCD: colon dots
  lv_obj_t *bar;                                  // neon: seconds bar
  lv_obj_t *scale, *hh, *mh, *sh;                 // analog: dial + hands
  lv_obj_t *arc;                                  // sport: seconds ring
  int       last_s;
};

static void face_delete_cb(lv_event_t* e) {
  lv_free(lv_event_get_user_data(e));
}

// ------------------------------------------------------------------ helpers
static lv_obj_t* box(lv_obj_t* p, int x, int y, int w, int h, lv_color_t c, int radius) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

static lv_obj_t* text(lv_obj_t* p, const char* t, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = ui_label(p, t, f, c);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}

static void set_text(lv_obj_t* l, const char* t) {
  if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

// Centred label with a fixed width (text changes do not move it)
static lv_obj_t* ctext(lv_obj_t* p, const lv_font_t* f, lv_color_t c, int y, int w = CLOCK_FACE_W) {
  lv_obj_t* l = text(p, "", f, c);
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(l, (CLOCK_FACE_W - w) / 2, y);
  return l;
}

// Row "time + small text" sharing one baseline, centred at y
static void time_row(lv_obj_t* p, Face* f, int y, lv_color_t tc, lv_color_t sc) {
  lv_obj_t* row = lv_obj_create(p);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  f->time  = text(row, "--:--", FONT_CLOCK, tc);
  f->small = text(row, "", FONT_28, sc);
  lv_obj_set_width(f->small, 56);                 // fixed: the time does not move
  lv_obj_set_style_pad_bottom(f->small, FONT_CLOCK->base_line - FONT_28->base_line, 0);
}

// ================================================================== DIGITAL
// 0 Classic: big time, seconds, day, date, city (the original Home clock)
static void mk_classic(lv_obj_t* p, Face* f) {
  time_row(p, f, 18, C_TEXT, C_ORANGE);
  f->day  = ctext(p, FONT_28, C_TEXT, 158);
  f->date = ctext(p, FONT_24, C_TEXT2, 202);
  lv_obj_t* tz = ctext(p, FONT_16, C_TEXT2, CLOCK_FACE_H - 22);
  lv_label_set_text_fmt(tz, LV_SYMBOL_GPS "  %s", g_data.city);
}

// 1 Neon: dark glass panel, cyan glow, seconds bar
static void mk_neon(lv_obj_t* p, Face* f) {
  lv_obj_t* pn = box(p, 4, 4, CLOCK_FACE_W - 8, CLOCK_FACE_H - 8, lv_color_hex(0x050F16), 22);
  lv_obj_set_style_border_width(pn, 2, 0);
  lv_obj_set_style_border_color(pn, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_border_opa(pn, LV_OPA_70, 0);
  lv_obj_set_style_shadow_width(pn, 26, 0);
  lv_obj_set_style_shadow_color(pn, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_shadow_opa(pn, LV_OPA_30, 0);
  time_row(p, f, 34, lv_color_hex(0x8AF7FF), lv_color_hex(0xFF4FD8));
  box(p, 32, 176, CLOCK_FACE_W - 64, 6, lv_color_hex(0x0E3440), 3);      // bar track
  f->bar = box(p, 32, 176, 4, 6, lv_color_hex(0x00E5FF), 3);
  lv_obj_set_style_shadow_width(f->bar, 12, 0);
  lv_obj_set_style_shadow_color(f->bar, lv_color_hex(0x00E5FF), 0);
  f->day  = ctext(p, FONT_28, lv_color_hex(0xFF4FD8), 204);
  f->date = ctext(p, FONT_20, lv_color_hex(0x9FEFFF), 246);
}

// 2 Flip cards: HH : MM on four split cards
#define FC_W 76
#define FC_H 118
static void mk_flip(lv_obj_t* p, Face* f) {
  const int gap = 6, cgap = 28;
  int x = (CLOCK_FACE_W - (4 * FC_W + 2 * gap + cgap)) / 2;
  for (int i = 0; i < 4; i++) {
    lv_obj_t* c = box(p, x, 20, FC_W, FC_H, lv_color_hex(0x343438), 12);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(0x1C1C1F), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(c, 10, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_shadow_offset_y(c, 4, 0);
    f->dig[i] = text(c, "-", FONT_CLOCK, lv_color_hex(0xF2F2F2));
    lv_obj_center(f->dig[i]);
    box(c, 0, FC_H / 2 - 1, FC_W, 3, lv_color_hex(0x0A0A0A), 0);       // split line
    x += FC_W + (i == 1 ? cgap : gap);
    if (i == 1) {
      lv_obj_t* col = text(p, ":", FONT_48, lv_color_hex(0xF2F2F2));
      lv_obj_set_pos(col, x - cgap + 8, 20 + FC_H / 2 - 32);
    }
  }
  f->small = ctext(p, FONT_28, C_ORANGE, 150);
  f->day   = ctext(p, FONT_24, C_TEXT, 200);
  f->date  = ctext(p, FONT_20, C_TEXT2, 238);
}

// 3 LCD: green 7-segment display
#define SG_W 52
#define SG_H 100
#define SG_T 10
static const uint8_t SEG_MAP[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
static void mk_lcd(lv_obj_t* p, Face* f) {
  lv_obj_t* pn = box(p, 8, 14, CLOCK_FACE_W - 16, 170, lv_color_hex(0x9DB58A), 16);
  lv_obj_set_style_bg_grad_color(pn, lv_color_hex(0x7F9A6C), 0);
  lv_obj_set_style_bg_grad_dir(pn, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(pn, 4, 0);
  lv_obj_set_style_border_color(pn, lv_color_hex(0x2B2F2A), 0);
  const lv_color_t on = lv_color_hex(0x1C2614);
  const int half = (SG_H - 3 * SG_T) / 2;
  const int seg[7][4] = {
    {SG_T, 0, SG_W - 2 * SG_T, SG_T},                   // a
    {SG_W - SG_T, SG_T, SG_T, half},                    // b
    {SG_W - SG_T, 2 * SG_T + half, SG_T, half},         // c
    {SG_T, SG_H - SG_T, SG_W - 2 * SG_T, SG_T},         // d
    {0, 2 * SG_T + half, SG_T, half},                   // e
    {0, SG_T, SG_T, half},                              // f
    {SG_T, SG_T + half, SG_W - 2 * SG_T, SG_T},         // g
  };
  int x = 22;
  for (int d = 0; d < 4; d++) {
    for (int k = 0; k < 7; k++)
      f->seg[d][k] = box(pn, x + seg[k][0], 30 + seg[k][1], seg[k][2], seg[k][3], on, 3);
    x += SG_W + 14;
    if (d == 1) {                                       // colon
      f->colon[0] = box(pn, x + 2, 30 + 28, 10, 10, on, 2);
      f->colon[1] = box(pn, x + 2, 30 + 62, 10, 10, on, 2);
      x += 24;
    }
  }
  f->small = text(pn, "", FONT_28, on);                 // seconds / AM-PM
  lv_obj_set_pos(f->small, x - 4, 30 + SG_H - 34);
  f->day  = ctext(p, FONT_24, C_TEXT, 204);
  f->date = ctext(p, FONT_20, C_TEXT2, 242);
}

// 4 Stacked: hours above minutes, day / date / seconds on the right
static void mk_stacked(lv_obj_t* p, Face* f) {
  f->dig[0] = text(p, "--", FONT_CLOCK, C_TEXT);        // hours
  lv_obj_set_pos(f->dig[0], 8, -10);
  f->dig[1] = text(p, "--", FONT_CLOCK, C_ORANGE);      // minutes
  lv_obj_set_pos(f->dig[1], 8, 104);
  box(p, 186, 24, 4, 200, C_ORANGE, 2);                 // accent bar
  f->day = text(p, "", FONT_28, C_TEXT);
  lv_obj_set_width(f->day, 160);
  lv_label_set_long_mode(f->day, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(f->day, 204, 40);
  f->date = text(p, "", FONT_20, C_TEXT2);
  lv_obj_set_pos(f->date, 204, 86);
  f->small = text(p, "", FONT_48, C_TEXT2);
  lv_obj_set_pos(f->small, 204, 150);
  lv_obj_t* tz = text(p, "", FONT_16, C_TEXT2);
  lv_label_set_text_fmt(tz, LV_SYMBOL_GPS "  %s", g_data.city);
  lv_obj_set_pos(tz, 8, CLOCK_FACE_H - 22);
}

// ================================================================== ANALOG
struct DialStyle {
  uint32_t face, border; int border_w;
  uint32_t major, minor; int major_len, major_w, minor_len, minor_w;
  bool     round_ticks;
  const char* const* labels;                       // nullptr = no numbers
  const lv_font_t* font; uint32_t label_col;
  uint32_t hand, sec; int hour_w, min_w, sec_w;
  bool     themed;                                 // colours follow the dark / light theme
};

static lv_color_t dc(const DialStyle& s, uint32_t c) { return s.themed ? ui_col(c) : lv_color_hex(c); }

static const char* const NUM_LABELS[]   = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", nullptr};
static const char* const ROMAN_LABELS[] = {"XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII", nullptr};
static const char* const SPORT_LABELS[] = {"12", "", "", "3", "", "", "6", "", "", "9", "", "", "12", nullptr};

static lv_obj_t* hand(lv_obj_t* scale, lv_color_t c, int w) {
  lv_obj_t* l = lv_line_create(scale);
  lv_obj_set_style_line_width(l, w, 0);
  lv_obj_set_style_line_color(l, c, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  return l;
}

// Dial of diameter d at (x, y): face, ticks, numbers, three hands, centre cap
static void dial(lv_obj_t* p, Face* f, int x, int y, int d, const DialStyle& s) {
  lv_obj_t* fc = box(p, x, y, d, d, dc(s, s.face), LV_RADIUS_CIRCLE);
  if (s.border_w) {
    lv_obj_set_style_border_width(fc, s.border_w, 0);
    lv_obj_set_style_border_color(fc, dc(s, s.border), 0);
  }
  lv_obj_t* sc = lv_scale_create(fc);
  f->scale = sc;
  int inner = d - 2 * s.border_w - 8;
  lv_obj_set_size(sc, inner, inner);
  lv_obj_center(sc);
  lv_obj_remove_flag(sc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_range(sc, 0, 720);                  // 12 h in minutes
  lv_scale_set_angle_range(sc, 360);
  lv_scale_set_rotation(sc, 270);                  // 0 at 12 o'clock
  lv_scale_set_total_tick_count(sc, 61);
  lv_scale_set_major_tick_every(sc, 5);
  lv_scale_set_label_show(sc, s.labels != nullptr);
  if (s.labels) lv_scale_set_text_src(sc, (const char**)s.labels);
  lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_arc_width(sc, 0, LV_PART_MAIN);
  lv_obj_set_style_length(sc, s.major_len, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(sc, s.major_w, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(sc, dc(s, s.major), LV_PART_INDICATOR);
  lv_obj_set_style_line_rounded(sc, s.round_ticks, LV_PART_INDICATOR);
  if (s.labels) {
    lv_obj_set_style_text_font(sc, s.font, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(sc, dc(s, s.label_col), LV_PART_INDICATOR);
  }
  lv_obj_set_style_length(sc, s.minor_len, LV_PART_ITEMS);
  lv_obj_set_style_line_width(sc, s.minor_w, LV_PART_ITEMS);
  lv_obj_set_style_line_color(sc, dc(s, s.minor), LV_PART_ITEMS);
  lv_obj_set_style_line_opa(sc, s.minor_len ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_ITEMS);

  f->hh = hand(sc, dc(s, s.hand), s.hour_w);
  f->mh = hand(sc, dc(s, s.hand), s.min_w);
  f->sh = hand(sc, dc(s, s.sec), s.sec_w);
  int cap = s.hour_w + 8;
  lv_obj_t* c = box(fc, 0, 0, cap, cap, dc(s, s.sec), LV_RADIUS_CIRCLE);
  lv_obj_center(c);
  lv_obj_set_style_border_width(c, 2, 0);
  lv_obj_set_style_border_color(c, dc(s, s.hand), 0);
}

// Date line under a dial: "Wed  18/11/2026"
static void dial_date(lv_obj_t* p, Face* f, int y, lv_color_t c) {
  f->date = ctext(p, FONT_20, c, y);
}

static void mk_a_classic(lv_obj_t* p, Face* f) {
  static const DialStyle S = {0x26262A, 0x3A3A40, 3, 0xFFFFFF, 0x8E8E93, 12, 4, 6, 2, false,
                              NUM_LABELS, FONT_20, 0xFFFFFF, 0xFFFFFF, 0xFFB070, 8, 5, 2, true};
  dial(p, f, (CLOCK_FACE_W - 250) / 2, 2, 250, S);
  dial_date(p, f, 270, C_TEXT2);
}

static void mk_a_minimal(lv_obj_t* p, Face* f) {
  static const DialStyle S = {0x1C1C1E, 0x1C1C1E, 0, 0xA0A0A8, 0x000000, 6, 8, 0, 0, true,
                              nullptr, FONT_20, 0, 0xFFFFFF, 0xFF9F0A, 6, 4, 2, true};
  dial(p, f, (CLOCK_FACE_W - 250) / 2, 2, 250, S);
  dial_date(p, f, 270, C_TEXT2);
}

static void mk_a_roman(lv_obj_t* p, Face* f) {
  static const DialStyle S = {0xF3E9D2, 0xB8935A, 6, 0x2B2118, 0x6B5A44, 12, 3, 5, 1, false,
                              ROMAN_LABELS, FONT_20, 0x2B2118, 0x2B2118, 0xC62828, 8, 5, 2, false};
  dial(p, f, (CLOCK_FACE_W - 250) / 2, 2, 250, S);
  dial_date(p, f, 270, C_TEXT2);
}

static void mk_a_sport(lv_obj_t* p, Face* f) {
  static const DialStyle S = {0x0B0B0B, 0x0B0B0B, 0, 0xC8FF8A, 0x4A5A3A, 14, 5, 6, 2, false,
                              SPORT_LABELS, FONT_28, 0xFFFFFF, 0xC8FF8A, 0xFF6D00, 9, 6, 3, false};
  const int d = 250, x = (CLOCK_FACE_W - d) / 2;
  dial(p, f, x, 2, d, S);
  f->arc = lv_arc_create(p);                       // seconds ring around the dial
  lv_obj_set_size(f->arc, d, d);
  lv_obj_set_pos(f->arc, x, 2);
  lv_arc_set_rotation(f->arc, 270);
  lv_arc_set_bg_angles(f->arc, 0, 360);
  lv_arc_set_range(f->arc, 0, 60);
  lv_obj_remove_style(f->arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(f->arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(f->arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(f->arc, lv_color_hex(0x1E2A12), LV_PART_MAIN);
  lv_obj_set_style_arc_width(f->arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(f->arc, lv_color_hex(0x7CFC00), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(f->arc, false, LV_PART_INDICATOR);
  dial_date(p, f, 270, C_TEXT2);
}

static void mk_a_hybrid(lv_obj_t* p, Face* f) {
  static const DialStyle S = {0x26262A, 0x3A3A40, 3, 0xFFFFFF, 0x8E8E93, 10, 3, 5, 1, false,
                              nullptr, FONT_20, 0, 0xFFFFFF, 0xFFB070, 7, 4, 2, true};
  dial(p, f, 0, 50, 200, S);
  f->time = text(p, "--:--", FONT_48, C_TEXT);
  lv_obj_set_pos(f->time, 214, 62);
  f->small = text(p, "", FONT_24, C_ORANGE);
  lv_obj_set_pos(f->small, 216, 124);
  f->day = text(p, "", FONT_24, C_TEXT);
  lv_obj_set_width(f->day, 150);
  lv_label_set_long_mode(f->day, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(f->day, 216, 172);
  f->date = text(p, "", FONT_20, C_TEXT2);
  lv_obj_set_pos(f->date, 216, 208);
}

// ================================================================== API
lv_obj_t* clock_face_create(lv_obj_t* parent, uint8_t theme) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, CLOCK_FACE_W, CLOCK_FACE_H);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);

  Face* f = (Face*)lv_malloc(sizeof(Face));
  memset(f, 0, sizeof(Face));
  f->theme  = theme % CLOCK_THEME_COUNT;
  f->last_s = -1;
  lv_obj_set_user_data(o, f);
  lv_obj_add_event_cb(o, face_delete_cb, LV_EVENT_DELETE, f);

  switch (f->theme) {
    case 0: mk_classic(o, f);   break;
    case 1: mk_neon(o, f);      break;
    case 2: mk_flip(o, f);      break;
    case 3: mk_lcd(o, f);       break;
    case 4: mk_stacked(o, f);   break;
    case 5: mk_a_classic(o, f); break;
    case 6: mk_a_minimal(o, f); break;
    case 7: mk_a_roman(o, f);   break;
    case 8: mk_a_sport(o, f);   break;
    case 9: mk_a_hybrid(o, f);  break;
  }
  clock_face_update(o);
  return o;
}

static void lcd_digit(Face* f, int d, int v) {          // v < 0: dash
  uint8_t bits = v < 0 ? 0x40 : SEG_MAP[v % 10];
  for (int k = 0; k < 7; k++)
    lv_obj_set_style_bg_opa(f->seg[d][k], (bits >> k) & 1 ? LV_OPA_COVER : 16, 0);   // unlit: faint
}

void clock_face_update(lv_obj_t* face) {
  Face* f = face ? (Face*)lv_obj_get_user_data(face) : nullptr;
  if (!f) return;
  Now n;
  now_get(n);
  if (n.valid && n.s == f->last_s) return;
  f->last_s = n.valid ? n.s : -1;

  char b[48], sb[8];
  const char* small = n.valid ? small_text(n, sb, sizeof(sb)) : "";
  const char* dayl  = n.valid ? lang_day_long(n.wday) : "";

  switch (f->theme) {
    case 0: case 1:                                      // Classic, Neon
      if (n.valid) snprintf(b, sizeof(b), "%02d:%02d", n.hd, n.m); else strcpy(b, "--:--");
      set_text(f->time, b);
      set_text(f->small, small);
      set_text(f->day, dayl);
      set_text(f->date, n.valid ? n.date : "");
      if (f->bar) lv_obj_set_width(f->bar, 4 + (CLOCK_FACE_W - 64 - 4) * (n.valid ? n.s : 0) / 59);
      break;

    case 2: {                                            // Flip cards
      int v[4] = {n.hd / 10, n.hd % 10, n.m / 10, n.m % 10};
      for (int i = 0; i < 4; i++) {
        if (n.valid) snprintf(b, sizeof(b), "%d", v[i]); else strcpy(b, "-");
        set_text(f->dig[i], b);
      }
      set_text(f->small, small);
      set_text(f->day, dayl);
      set_text(f->date, n.valid ? n.date : "");
      break;
    }

    case 3: {                                            // LCD
      int v[4] = {n.hd / 10, n.hd % 10, n.m / 10, n.m % 10};
      for (int i = 0; i < 4; i++) lcd_digit(f, i, n.valid ? v[i] : -1);
      lv_opa_t co = (!n.valid || n.s % 2 == 0) ? LV_OPA_COVER : 40;   // blinking colon
      lv_obj_set_style_bg_opa(f->colon[0], co, 0);
      lv_obj_set_style_bg_opa(f->colon[1], co, 0);
      set_text(f->small, n.valid ? (settings_time24() ? (snprintf(sb, sizeof(sb), "%02d", n.s), sb)
                                                     : tr(n.pm ? S_PM : S_AM)) : "");
      set_text(f->day, dayl);
      set_text(f->date, n.valid ? n.date : "");
      break;
    }

    case 4:                                              // Stacked
      if (n.valid) {
        snprintf(b, sizeof(b), "%02d", n.hd); set_text(f->dig[0], b);
        snprintf(b, sizeof(b), "%02d", n.m);  set_text(f->dig[1], b);
      } else {
        set_text(f->dig[0], "--"); set_text(f->dig[1], "--");
      }
      set_text(f->small, small);
      set_text(f->day, dayl);
      set_text(f->date, n.valid ? n.date : "");
      break;

    default: {                                           // analog styles
      int h12 = n.h % 12;
      lv_obj_update_layout(f->scale);                    // size known also before the first draw
      int r = lv_obj_get_width(f->scale) / 2;
      lv_scale_set_line_needle_value(f->scale, f->hh, r * 50 / 100, h12 * 60 + n.m);
      lv_scale_set_line_needle_value(f->scale, f->mh, r * 78 / 100, n.m * 12 + n.s / 5);
      lv_scale_set_line_needle_value(f->scale, f->sh, r * 88 / 100, n.s * 12);
      if (f->arc) lv_arc_set_value(f->arc, n.valid ? n.s : 0);
      if (f->theme == 9) {                               // Hybrid: digital part
        if (n.valid) snprintf(b, sizeof(b), "%02d:%02d", n.hd, n.m); else strcpy(b, "--:--");
        set_text(f->time, b);
        if (n.valid && settings_time24()) { snprintf(sb, sizeof(sb), ":%02d", n.s); set_text(f->small, sb); }
        else set_text(f->small, n.valid ? tr(n.pm ? S_PM : S_AM) : "");
        set_text(f->day, dayl);
        set_text(f->date, n.valid ? n.date : "");
      } else {
        if (n.valid) snprintf(b, sizeof(b), "%s   %s", lang_day_short(n.wday), n.date); else b[0] = 0;
        set_text(f->date, b);
      }
      break;
    }
  }
}

// ================================================================== PAGE
// 2 rows x 5 cards: digital styles on top, analog below. Each card shows the
// real style (scaled to 60 %); a tap selects it, saves it and returns Home.
// Portrait: 5 rows x 2 cards (digital left, analog right), the style scaled
// to 48 % on the left of the card, its name on the right.
#define CARD_W   UI_LP(235, 332)
#define CARD_H   UI_LP(288, 218)
#define PREV_SC  UI_LP(154, 123)                         // 256 = 100 %  -> 60 % / 48 %

static struct {
  lv_obj_t* card[CLOCK_THEME_COUNT];
  lv_obj_t* face[CLOCK_THEME_COUNT];
  lv_obj_t* check[CLOCK_THEME_COUNT];
} C;

static void mark_selected() {
  for (int i = 0; i < CLOCK_THEME_COUNT; i++) {
    bool sel = (i == clock_theme());
    lv_obj_set_style_border_color(C.card[i], sel ? C_ORANGE : ui_col(0x3A3A40), 0);
    lv_obj_set_style_border_width(C.card[i], sel ? 4 : 1, 0);
    if (sel) lv_obj_remove_flag(C.check[i], LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(C.check[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void go_home_cb(lv_timer_t* t) { ui.show(PAGE_HOME); }

static void card_cb(lv_event_t* e) {
  clock_set_theme((uint8_t)(intptr_t)lv_event_get_user_data(e));
  mark_selected();
  lv_timer_t* t = lv_timer_create(go_home_cb, 350, nullptr);   // let the selection show
  lv_timer_set_repeat_count(t, 1);
}

static void page_timer_cb(lv_timer_t* t) {
  for (int i = 0; i < CLOCK_THEME_COUNT; i++) clock_face_update(C.face[i]);
}

lv_obj_t* clock_create() {
  memset(&C, 0, sizeof(C));
  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_CLOCK_TITLE), LV_SYMBOL_BELL, MENU_SUBPAGE, ui_col(0x3A2A1A));

  const bool port = ui_portrait();
  for (int i = 0; i < CLOCK_THEME_COUNT; i++) {
    int col = port ? i / 5 : i % 5, row = port ? i % 5 : i / 5;
    lv_obj_t* c = ui_tile_create(scr, 20 + col * (CARD_W + 16), 108 + row * (CARD_H + 16), CARD_W, CARD_H, C_TILE);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_radius(c, 24, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    C.card[i] = c;

    lv_obj_t* fc = clock_face_create(c, i);              // the real style, scaled down
    if (port) lv_obj_set_pos(fc, 10, (CARD_H - CLOCK_FACE_H * PREV_SC / 256) / 2);
    else      lv_obj_set_pos(fc, (CARD_W - CLOCK_FACE_W * PREV_SC / 256) / 2, 12);
    lv_obj_set_style_transform_pivot_x(fc, 0, 0);
    lv_obj_set_style_transform_pivot_y(fc, 0, 0);
    lv_obj_set_style_transform_scale(fc, PREV_SC, 0);
    C.face[i] = fc;

    // name + kind: under the preview (landscape) or right of it (portrait)
    const int tx = port ? 194 : 16, ty = port ? 70 : 214;
    lv_obj_t* nm = ui_label(c, clock_theme_name(i), FONT_20, C_TEXT);
    lv_obj_set_width(nm, port ? CARD_W - tx - 12 : CARD_W - 60);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(nm, tx, ty);
    lv_obj_t* kind = ui_label(c, tr(i < 5 ? S_CLK_DIGITAL : S_CLK_ANALOG), FONT_16, C_TEXT2);
    lv_obj_set_pos(kind, tx, ty + 32);

    C.check[i] = ui_icon_circle(c, LV_SYMBOL_OK, 36, C_ORANGE, ui_col(0x000000));
    if (port) lv_obj_set_pos(C.check[i], tx, ty + 70);
    else      lv_obj_set_pos(C.check[i], CARD_W - 52, 222);
  }
  mark_selected();
  ui_page_timer(scr, page_timer_cb, 1000, nullptr);
  return scr;
}
