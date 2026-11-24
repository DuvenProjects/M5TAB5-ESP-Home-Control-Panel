// ============================================================================
//  rgb.cpp  -  RGB LED PWM driver + effects + RGB control page
//   PWM: 5 kHz, 10 bit, gamma corrected. RGB_COMMON_ANODE = 0 for a common
//   cathode LED (or a 12 V strip driven by N-MOSFETs), 1 for common anode.
// ============================================================================
#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "rgb.h"
#include "ui.h"
#include "menu.h"

const char* rgb_effect_name(uint8_t fx) {
  static const StrId ids[RGB_EFFECT_COUNT] = {S_FX_STATIC, S_FX_BREATHE, S_FX_RAINBOW, S_FX_CANDLE};
  return tr(ids[fx % RGB_EFFECT_COUNT]);
}

#define PWM_FREQ 5000
#define PWM_BITS 10
#define PWM_MAX  ((1 << PWM_BITS) - 1)

static uint8_t s_out_r, s_out_g, s_out_b;          // colour currently sent to the LED (0..255)

static uint32_t gamma10(uint8_t v) {               // 8 bit -> 10 bit, gamma 2.2
  float f = powf(v / 255.0f, 2.2f);
  return (uint32_t)(f * PWM_MAX + 0.5f);
}

static void write_rgb(uint8_t r, uint8_t g, uint8_t b) {
  s_out_r = r; s_out_g = g; s_out_b = b;
  uint32_t R = gamma10(r), G = gamma10(g), B = gamma10(b);
#if RGB_COMMON_ANODE
  R = PWM_MAX - R; G = PWM_MAX - G; B = PWM_MAX - B;
#endif
  ledcWrite(PIN_RGB_R, R);
  ledcWrite(PIN_RGB_G, G);
  ledcWrite(PIN_RGB_B, B);
}

static void write_scaled(uint8_t r, uint8_t g, uint8_t b, float k) {
  k = constrain(k, 0.0f, 1.0f);
  write_rgb((uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
}

void rgb_hw_init() {
  ledcAttach(PIN_RGB_R, PWM_FREQ, PWM_BITS);
  ledcAttach(PIN_RGB_G, PWM_FREQ, PWM_BITS);
  ledcAttach(PIN_RGB_B, PWM_FREQ, PWM_BITS);
  rgb_apply();
}

void rgb_apply() {
  if (!g_data.rgb_on) { write_rgb(0, 0, 0); return; }
  if (g_data.rgb_effect == RGB_STATIC)
    write_scaled(g_data.rgb_r, g_data.rgb_g, g_data.rgb_b, g_data.rgb_brightness / 100.0f);
  // the other effects run in rgb_tick()
}

void rgb_tick() {
  if (!g_data.rgb_on || g_data.rgb_effect == RGB_STATIC) return;
  float bri = g_data.rgb_brightness / 100.0f;
  uint32_t ms = millis();
  switch (g_data.rgb_effect) {
    case RGB_BREATH: {
      float k = 0.15f + 0.85f * (0.5f - 0.5f * cosf(ms / 4000.0f * 2 * (float)M_PI));
      write_scaled(g_data.rgb_r, g_data.rgb_g, g_data.rgb_b, bri * k);
      break;
    }
    case RGB_RAINBOW: {
      uint16_t hue = (ms / 50) % 360;
      lv_color_t c = lv_color_hsv_to_rgb(hue, 100, 100);
      write_scaled(c.red, c.green, c.blue, bri);
      break;
    }
    case RGB_CANDLE: {
      float k = 0.6f + (random(0, 40) / 100.0f);
      write_scaled(255, 120, 30, bri * k);
      break;
    }
  }
}

void rgb_set(bool on, uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint8_t effect) {
  g_data.rgb_on = on; g_data.rgb_r = r; g_data.rgb_g = g; g_data.rgb_b = b;
  g_data.rgb_brightness = bri; g_data.rgb_effect = effect;
  rgb_apply();
  data_mark_dirty();
}

lv_color_t rgb_current_color() {
  return lv_color_make(g_data.rgb_r, g_data.rgb_g, g_data.rgb_b);
}

// ============================================================ PAGE
static struct {
  lv_obj_t *preview, *preview_lbl, *sw, *bri, *bri_lbl;
  lv_obj_t *wheel, *knob, *swatch, *hex;
  lv_obj_t *fx[RGB_EFFECT_COUNT];
} P;

// ---------------------------------------------------------------- colour wheel
// One round palette instead of three R / G / B sliders: the angle is the hue,
// the distance from the centre the saturation (centre = white). Brightness is
// set with the brightness slider. The wheel image is drawn once into PSRAM.
#define WHEEL_D   340                               // diameter (px)
#define KNOB_D    40

static uint8_t* s_wheel_buf = nullptr;              // ARGB8888, WHEEL_D x WHEEL_D

static void hsv_to_rgb_f(float h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b) {
  h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
  float R, G, B;
  if      (h < 60)  { R = c; G = x; B = 0; }
  else if (h < 120) { R = x; G = c; B = 0; }
  else if (h < 180) { R = 0; G = c; B = x; }
  else if (h < 240) { R = 0; G = x; B = c; }
  else if (h < 300) { R = x; G = 0; B = c; }
  else              { R = c; G = 0; B = x; }
  *r = (uint8_t)lroundf((R + m) * 255); *g = (uint8_t)lroundf((G + m) * 255); *b = (uint8_t)lroundf((B + m) * 255);
}

static void wheel_draw() {
  if (s_wheel_buf) return;
  s_wheel_buf = (uint8_t*)heap_caps_malloc(WHEEL_D * WHEEL_D * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_wheel_buf) return;
  const float R = WHEEL_D / 2.0f - 1, c = (WHEEL_D - 1) / 2.0f;
  for (int y = 0; y < WHEEL_D; y++) {
    for (int x = 0; x < WHEEL_D; x++) {
      uint8_t* px = s_wheel_buf + (y * WHEEL_D + x) * 4;            // B G R A
      float dx = x - c, dy = y - c, d = sqrtf(dx * dx + dy * dy);
      float a = R + 0.5f - d;                                       // anti-aliased edge
      if (a <= 0) { px[0] = px[1] = px[2] = px[3] = 0; continue; }
      float hue = atan2f(-dy, dx) * 180.0f / (float)M_PI;           // 0 deg = right, counter-clockwise
      uint8_t r, g, b;
      hsv_to_rgb_f(hue, fminf(d / R, 1.0f), 1.0f, &r, &g, &b);
      px[0] = b; px[1] = g; px[2] = r; px[3] = a >= 1 ? 255 : (uint8_t)(a * 255);
    }
  }
}

// Knob position and swatch for the current colour
static void wheel_show(lv_color_t col) {
  if (!P.wheel) return;
  lv_color_hsv_t hsv = lv_color_to_hsv(col);
  float R = WHEEL_D / 2.0f - KNOB_D / 4.0f;
  float ang = hsv.h * (float)M_PI / 180.0f, d = R * hsv.s / 100.0f;
  int x = (int)lroundf(WHEEL_D / 2.0f + d * cosf(ang)) - KNOB_D / 2;
  int y = (int)lroundf(WHEEL_D / 2.0f - d * sinf(ang)) - KNOB_D / 2;
  lv_obj_set_pos(P.knob, lv_obj_get_x(P.wheel) + x, lv_obj_get_y(P.wheel) + y);
  lv_obj_set_style_bg_color(P.knob, col, 0);
  lv_obj_set_style_bg_color(P.swatch, col, 0);
  lv_label_set_text_fmt(P.hex, "#%02X%02X%02X", col.red, col.green, col.blue);
}

static const uint32_t PRESETS[8] = {
  0xFFA050, 0xFFFFFF, 0xFF3B30, 0x34C759, 0x0A84FF, 0xBF5AF2, 0xFF2D95, 0x5AF2E6
};

static void refresh_ui() {
  lv_color_t c = rgb_current_color();
  bool on = g_data.rgb_on;
  lv_obj_set_style_bg_color(P.preview, on ? c : ui_col(0x2A2A2E), 0);
  lv_obj_set_style_shadow_color(P.preview, c, 0);
  lv_obj_set_style_shadow_width(P.preview, on ? 20 + g_data.rgb_brightness : 0, 0);
  lv_obj_set_style_bg_opa(P.preview, on ? (lv_opa_t)(LV_OPA_40 + g_data.rgb_brightness * 3 / 2) : LV_OPA_COVER, 0);
  lv_label_set_text(P.preview_lbl, on ? rgb_effect_name(g_data.rgb_effect) : tr(S_OFF));

  if (on) lv_obj_add_state(P.sw, LV_STATE_CHECKED); else lv_obj_remove_state(P.sw, LV_STATE_CHECKED);
  lv_slider_set_value(P.bri, g_data.rgb_brightness, LV_ANIM_OFF);
  lv_label_set_text_fmt(P.bri_lbl, tr(S_FMT_BRIGHTNESS), g_data.rgb_brightness);

  wheel_show(c);
  for (int i = 0; i < RGB_EFFECT_COUNT; i++) {
    bool sel = (i == g_data.rgb_effect);
    lv_obj_set_style_bg_color(P.fx[i], sel ? C_ORANGE : ui_col(0x3A3A40), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(P.fx[i], 0), sel ? ui_col(0x000000) : C_TEXT, 0);
  }
}

static void changed() { rgb_apply(); data_mark_dirty(); refresh_ui(); }

static void sw_cb(lv_event_t* e) {
  g_data.rgb_on = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  changed();
}
static void bri_cb(lv_event_t* e) {
  g_data.rgb_brightness = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
  changed();
}
// Touch on the wheel: angle -> hue, distance -> saturation (full value)
static void wheel_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
  lv_point_t pt;
  lv_indev_get_point(lv_indev_active(), &pt);
  lv_area_t a;
  lv_obj_get_coords(P.wheel, &a);
  float R = WHEEL_D / 2.0f - KNOB_D / 4.0f;
  float dx = pt.x - (a.x1 + WHEEL_D / 2.0f), dy = pt.y - (a.y1 + WHEEL_D / 2.0f);
  float d = sqrtf(dx * dx + dy * dy);
  if (code == LV_EVENT_PRESSED && d > WHEEL_D / 2.0f + 12) return;   // corner of the image
  float hue = atan2f(-dy, dx) * 180.0f / (float)M_PI;
  uint8_t r, g, b;
  hsv_to_rgb_f(hue, fminf(d / R, 1.0f), 1.0f, &r, &g, &b);
  if (r == g_data.rgb_r && g == g_data.rgb_g && b == g_data.rgb_b) return;
  g_data.rgb_r = r; g_data.rgb_g = g; g_data.rgb_b = b;
  g_data.rgb_on = true;
  if (g_data.rgb_effect == RGB_RAINBOW || g_data.rgb_effect == RGB_CANDLE) g_data.rgb_effect = RGB_STATIC;
  changed();
}
static void preset_cb(lv_event_t* e) {
  uint32_t c = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  g_data.rgb_r = (c >> 16) & 0xFF; g_data.rgb_g = (c >> 8) & 0xFF; g_data.rgb_b = c & 0xFF;
  g_data.rgb_on = true;
  if (g_data.rgb_effect == RGB_RAINBOW || g_data.rgb_effect == RGB_CANDLE) g_data.rgb_effect = RGB_STATIC;
  changed();
}
static void fx_cb(lv_event_t* e) {
  g_data.rgb_effect = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  g_data.rgb_on = true;
  changed();
}

// Slider inside the band x .. x+w of its card. The round knob is centred on
// the bar end at the minimum / maximum value, so the bar is shortened by the
// knob radius on both sides: the knob never leaves the card.
#define KNOB_PAD  2                                 // knob is 2 px larger than the bar
static lv_obj_t* big_slider(lv_obj_t* p, int x, int y, int w, int h, int max, lv_color_t ind) {
  int r = h / 2 + KNOB_PAD;                         // knob radius
  lv_obj_t* s = lv_slider_create(p);
  lv_obj_set_size(s, w - 2 * r, h);
  lv_obj_set_pos(s, x + r, y);
  lv_obj_set_ext_click_area(s, r);                  // the knob can still be grabbed at the ends
  lv_slider_set_range(s, 0, max);
  lv_obj_set_style_radius(s, h / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(s, h / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, ui_col(0x2E2E34), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, ind, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, KNOB_PAD, LV_PART_KNOB);
  return s;
}

static lv_obj_t* card(lv_obj_t* scr, int x, int w, const char* title, int y = 108, int h = 592) {
  lv_obj_t* c = ui_tile_create(scr, x, y, w, h, C_TILE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(c, 256, LV_STATE_PRESSED);
  lv_obj_t* t = ui_label(c, title, FONT_24, C_TEXT);
  lv_obj_set_pos(t, 6, 4);
  return c;
}

lv_obj_t* rgb_create() {
  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_RGB_LIGHT), LV_SYMBOL_TINT, MENU_SUBPAGE, ui_col(0x5A3A20));

  // Landscape: 3 cards side by side (light | colour | presets).
  // Portrait : the 3 cards under each other, full width.
  const bool port = ui_portrait();

  // ---------------- card 1: preview + on / off + brightness
  lv_obj_t* c1 = port ? card(scr, 20, 680, tr(S_LIGHT), 108, 400) : card(scr, 20, 400, tr(S_LIGHT));
  P.sw = lv_switch_create(c1);
  lv_obj_set_size(P.sw, 100, 52);
  lv_obj_align(P.sw, LV_ALIGN_TOP_RIGHT, -4, 0);
  lv_obj_set_style_bg_color(P.sw, C_ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(P.sw, sw_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  P.preview = lv_obj_create(c1);
  lv_obj_remove_style_all(P.preview);
  lv_obj_set_size(P.preview, 230, 230);
  if (port) lv_obj_set_pos(P.preview, 16, 90);     // left; brightness on the right
  else      lv_obj_align(P.preview, LV_ALIGN_TOP_MID, 0, 90);
  lv_obj_set_style_radius(P.preview, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(P.preview, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_spread(P.preview, 6, 0);
  lv_obj_set_style_shadow_opa(P.preview, LV_OPA_70, 0);
  lv_obj_remove_flag(P.preview, LV_OBJ_FLAG_CLICKABLE);
  P.preview_lbl = ui_label(P.preview, "", FONT_24, lv_color_hex(0xFFFFFF));  // on the LED colour
  lv_obj_center(P.preview_lbl);

  P.bri_lbl = ui_label(c1, "", FONT_20, C_TEXT2);
  lv_obj_set_pos(P.bri_lbl, port ? 290 : 6, port ? 170 : 400);
  P.bri = port ? big_slider(c1, 290, 210, 350, 70, 100, C_ORANGE)
               : big_slider(c1, 6, 440, 350, 70, 100, C_ORANGE);
  lv_slider_set_range(P.bri, 1, 100);
  lv_obj_add_event_cb(P.bri, bri_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  // ---------------- card 2: colour wheel (replaces the R / G / B sliders)
  lv_obj_t* c2 = port ? card(scr, 20, 680, tr(S_COLOR), 522, 440) : card(scr, 440, 400, tr(S_COLOR));
  wheel_draw();
  P.wheel = lv_image_create(c2);
  if (s_wheel_buf) {
    static lv_image_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = WHEEL_D;
    dsc.header.h = WHEEL_D;
    dsc.header.stride = WHEEL_D * 4;
    dsc.data = s_wheel_buf;
    dsc.data_size = WHEEL_D * WHEEL_D * 4;
    lv_image_set_src(P.wheel, &dsc);
  }
  lv_obj_set_size(P.wheel, WHEEL_D, WHEEL_D);
  lv_obj_set_pos(P.wheel, port ? 16 : (364 - WHEEL_D) / 2, 56);
  lv_obj_add_flag(P.wheel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(P.wheel, 12);
  lv_obj_add_event_cb(P.wheel, wheel_cb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(P.wheel, wheel_cb, LV_EVENT_PRESSING, nullptr);

  P.knob = lv_obj_create(c2);                                    // selected colour
  lv_obj_remove_style_all(P.knob);
  lv_obj_set_size(P.knob, KNOB_D, KNOB_D);
  lv_obj_set_style_radius(P.knob, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(P.knob, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(P.knob, 4, 0);
  lv_obj_set_style_border_color(P.knob, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_shadow_width(P.knob, 12, 0);
  lv_obj_set_style_shadow_opa(P.knob, LV_OPA_60, 0);
  lv_obj_set_style_shadow_color(P.knob, lv_color_hex(0x000000), 0);
  lv_obj_remove_flag(P.knob, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

  P.swatch = lv_obj_create(c2);                                  // colour sample + hex code
  lv_obj_remove_style_all(P.swatch);
  lv_obj_set_size(P.swatch, 56, 56);
  lv_obj_set_pos(P.swatch, port ? 400 : 6, port ? 196 : 440);
  lv_obj_set_style_radius(P.swatch, 16, 0);
  lv_obj_set_style_bg_opa(P.swatch, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(P.swatch, 2, 0);
  lv_obj_set_style_border_color(P.swatch, ui_col(0x3A3A40), 0);
  lv_obj_remove_flag(P.swatch, LV_OBJ_FLAG_CLICKABLE);
  P.hex = ui_label(c2, "", FONT_28, C_TEXT);
  lv_obj_set_pos(P.hex, port ? 474 : 80, port ? 208 : 452);

  // ---------------- card 3: preset colours + effects
  // portrait: 8 presets in one row, the effects in one row below
  lv_obj_t* c3 = port ? card(scr, 20, 680, tr(S_PRESETS), 976, 284) : card(scr, 860, 400, tr(S_PRESETS));
  for (int i = 0; i < 8; i++) {
    lv_obj_t* b = lv_button_create(c3);
    lv_obj_set_size(b, port ? 64 : 72, port ? 64 : 72);
    if (port) lv_obj_set_pos(b, 6 + i * 80, 48);
    else      lv_obj_set_pos(b, 6 + (i % 4) * 88, 60 + (i / 4) * 92);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PRESETS[i]), 0);   // real LED colour (not themed)
    lv_obj_set_style_border_width(b, 3, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, C_TEXT, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)PRESETS[i]);
  }
  lv_obj_t* fl = ui_label(c3, tr(S_EFFECTS), FONT_24, C_TEXT);
  lv_obj_set_pos(fl, 6, port ? 128 : 270);
  for (int i = 0; i < RGB_EFFECT_COUNT; i++) {
    lv_obj_t* b = lv_button_create(c3);
    P.fx[i] = b;
    lv_obj_set_size(b, port ? 150 : 170, port ? 70 : 80);
    if (port) lv_obj_set_pos(b, 6 + i * 160, 170);
    else      lv_obj_set_pos(b, 6 + (i % 2) * 184, 320 + (i / 2) * 96);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t* l = ui_label(b, rgb_effect_name(i), FONT_20, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, fx_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }

  refresh_ui();
  return scr;
}
