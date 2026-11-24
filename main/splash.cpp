// ============================================================================
//  splash.cpp  -  Start-up screen (all LVGL code of the splash is here)
//
//   [logo]   ESP HOME CONTROL PANEL
//            ──────
//            Created by Selcuk
//
//  Portrait: the logo above the texts, everything centred.
//
//  Logo: rounded orange badge with a house outline and a wireless signal
//  inside (drawn with LVGL lines / arcs, no image file needed).
// ============================================================================
#include "splash.h"
#include "ui.h"
#include "lang.h"           // FONT_xx

#define LOGO_SIZE   180

// lv_line keeps a pointer to its points: they must stay valid (static)
static const lv_point_precise_t ROOF[] = {{28, 94}, {90, 36}, {152, 94}};
static const lv_point_precise_t BODY[] = {{46, 80}, {46, 150}, {134, 150}, {134, 80}};

static void logo_line(lv_obj_t* parent, const lv_point_precise_t* pts, uint32_t n) {
  lv_obj_t* l = lv_line_create(parent);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_width(l, 12, 0);
  lv_obj_set_style_line_color(l, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_set_pos(l, 0, 0);
}

// Signal arc (top quarter) centred on (cx, cy)
static void logo_arc(lv_obj_t* parent, int cx, int cy, int r) {
  lv_obj_t* a = lv_arc_create(parent);
  lv_obj_set_size(a, 2 * r, 2 * r);
  lv_obj_set_pos(a, cx - r, cy - r);
  lv_arc_set_bg_angles(a, 225, 315);                 // 270 = straight up
  lv_obj_set_style_arc_width(a, 9, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, ui_col(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t* logo_create(lv_obj_t* parent) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, LOGO_SIZE, LOGO_SIZE);
  lv_obj_set_style_radius(b, 44, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, ui_col(0xFFB070), 0);
  lv_obj_set_style_bg_grad_color(b, ui_col(0xFF6A3D), 0);
  lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_shadow_width(b, 60, 0);
  lv_obj_set_style_shadow_color(b, ui_col(0xFF7A3D), 0);
  lv_obj_set_style_shadow_opa(b, LV_OPA_30, 0);
  lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

  logo_line(b, ROOF, 3);
  logo_line(b, BODY, 4);
  logo_arc(b, 90, 138, 40);
  logo_arc(b, 90, 138, 22);
  lv_obj_t* dot = lv_obj_create(b);                  // signal source
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 14, 14);
  lv_obj_set_pos(dot, 90 - 7, 138 - 7);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, ui_col(0xFFFFFF), 0);
  return b;
}

lv_obj_t* splash_create() {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Logo + texts side by side (portrait: under each other), centred on the screen
  const bool port = ui_portrait();
  lv_obj_t* row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, port ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 48, 0);
  lv_obj_set_style_pad_row(row, 56, 0);
  lv_obj_center(row);

  logo_create(row);

  lv_obj_t* col = lv_obj_create(row);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 14, 0);
  if (port) lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* title = ui_label(col, port ? "ESP HOME\nCONTROL PANEL" : "ESP HOME CONTROL PANEL", FONT_48, C_TEXT);
  lv_obj_set_style_text_letter_space(title, 2, 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t* bar = lv_obj_create(col);                // short orange accent line
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 96, 5);
  lv_obj_set_style_radius(bar, 3, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bar, C_ORANGE, 0);

  ui_label(col, "Created by Selcuk", FONT_28, C_TEXT2);
  return scr;
}
