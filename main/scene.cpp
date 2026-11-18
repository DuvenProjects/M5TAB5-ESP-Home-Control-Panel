// ============================================================================
//  scene.cpp  -  Scene definitions and the Scenes page (4 x 2 cards)
// ============================================================================
#include "scene.h"
#include "ui.h"
#include "menu.h"
#include "rgb.h"
#include "lights.h"
#include "hvac.h"

//  name          description       icon                card      rgb    colour    %   effect       blind setp   alarm
const Scene SCENES[SCENE_COUNT] = {
  {S_SC_MORNING, S_SCI_MORNING,  LV_SYMBOL_UP,       0x4A3A1A, true,  0xFFC080, 60, RGB_STATIC,  100, 22.0f, 0},
  {S_SC_DAY,     S_SCI_DAY,      LV_SYMBOL_IMAGE,    0x1E3A4A, false, 0xFFFFFF, 0,  RGB_STATIC,  100, 21.5f, -1},
  {S_SC_AWAY,    S_SCI_AWAY,     LV_SYMBOL_HOME,     0x3A1E1E, false, 0x000000, 0,  RGB_STATIC,  0,   18.0f, 1},
  {S_SC_MOVIE,   S_SCI_MOVIE,    LV_SYMBOL_VIDEO,    0x1E2246, true,  0x3050FF, 15, RGB_STATIC,  0,   0,     -1},
  {S_SC_RELAX,   S_SCI_RELAX,    LV_SYMBOL_TINT,     0x3A2A1E, true,  0xFF9040, 40, RGB_BREATH,  50,  22.5f, -1},
  {S_SC_PARTY,   S_SCI_PARTY,    LV_SYMBOL_AUDIO,    0x3A1E46, true,  0xBF5AF2, 100,RGB_RAINBOW, -1,  0,     -1},
  {S_SC_NIGHT,   S_SCI_NIGHT,    LV_SYMBOL_EYE_CLOSE,0x1A1A2E, true,  0xFF6020, 5,  RGB_CANDLE,  0,   20.0f, 1},
  {S_SC_ALL_OFF, S_SCI_ALL_OFF,  LV_SYMBOL_POWER,    0x2A2A2E, false, 0x000000, 0,  RGB_STATIC,  -1,  0,     -1},
};

const char* scene_name(uint8_t id) { return id < SCENE_COUNT ? tr(SCENES[id].name) : "--"; }

void scene_apply(uint8_t id) {
  if (id >= SCENE_COUNT) return;
  const Scene& s = SCENES[id];
  uint8_t r = (s.rgb >> 16) & 0xFF, g = (s.rgb >> 8) & 0xFF, b = s.rgb & 0xFF;
  if (s.rgb_on) rgb_set(true, r, g, b, s.rgb_bri, s.rgb_fx);
  else          rgb_set(false, g_data.rgb_r, g_data.rgb_g, g_data.rgb_b, g_data.rgb_brightness, RGB_STATIC);
  if (s.blinds >= 0)     blinds_set_target(-1, s.blinds);
  if (s.setpoint > 0)    hvac_apply_setpoint(s.setpoint);
  if (s.alarm >= 0)      g_data.alarm_armed = s.alarm == 1;
  g_data.scene_id = id;
  data_mark_dirty();
}

// ============================================================ PAGE
static lv_obj_t* s_cards[SCENE_COUNT];
static lv_obj_t* s_check[SCENE_COUNT];

static void mark_selected() {
  for (int i = 0; i < SCENE_COUNT; i++) {
    bool sel = (i == g_data.scene_id);
    lv_obj_set_style_border_width(s_cards[i], sel ? 4 : 1, 0);
    lv_obj_set_style_border_color(s_cards[i], sel ? C_TEXT : ui_col(0x3A3A40), 0);
    if (sel) lv_obj_remove_flag(s_check[i], LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(s_check[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void scene_cb(lv_event_t* e) {
  scene_apply((uint8_t)(intptr_t)lv_event_get_user_data(e));
  mark_selected();
}

lv_obj_t* scene_create() {
  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_SCENES), LV_SYMBOL_IMAGE, MENU_SUBPAGE, ui_col(0x4A2A50));

  const int W = 295, H = 286, G = 20;
  for (int i = 0; i < SCENE_COUNT; i++) {
    const Scene& s = SCENES[i];
    int x = 20 + (i % 4) * (W + G);
    int y = 108 + (i / 4) * (H + G);
    lv_obj_t* c = ui_tile_create(scr, x, y, W, H, ui_col(s.color));
    s_cards[i] = c;
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, scene_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_color_t ic_bg = lv_color_mix(ui_col(0xFFFFFF), ui_col(s.color), 50);
    lv_obj_t* ic = ui_icon_circle(c, s.symbol, 96, ic_bg, C_TEXT);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t* n = ui_label(c, tr(s.name), FONT_28, C_TEXT);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_t* d = ui_label(c, tr(s.info), FONT_16, C_TEXT2);
    lv_obj_set_width(d, W - 40);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 188);

    s_check[i] = ui_label(c, "", FONT_16, C_GREEN);
    lv_label_set_text_fmt(s_check[i], LV_SYMBOL_OK " %s", tr(S_ACTIVE));
    lv_obj_align(s_check[i], LV_ALIGN_BOTTOM_MID, 0, 0);
  }
  mark_selected();
  return scr;
}
