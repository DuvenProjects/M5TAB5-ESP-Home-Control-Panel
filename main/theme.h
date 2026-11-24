// ============================================================================
//  theme.h  -  Dark / light colour theme (Settings > Display > Theme)
//
//  Every page writes its colours as dark-theme hex values through ui_col().
//  In the dark theme the value is returned unchanged; in the light theme it is
//  converted to its light counterpart:
//    - backgrounds / tiles (dark)   -> light grey / white (tint kept, softened)
//    - borders, separators          -> mid grey
//    - white / grey text            -> near-black / dark grey
//    - accent colours               -> kept (pastel ones a bit darker for contrast)
//  Pages are rebuilt after a theme change, so every screen picks it up.
// ============================================================================
#pragma once
#include <lvgl.h>
#include <stdint.h>

lv_color_t ui_col(uint32_t dark_hex);   // colour for the active theme
bool       ui_light_theme();            // true = light theme selected

// Screen orientation (Settings > Display > Orientation): landscape 1280 x 720
// or portrait 720 x 1280. It is read from NVS once at start-up, before the
// LVGL display is created; changing it restarts the panel.
bool       ui_portrait();
static inline int ui_scr_w() { return ui_portrait() ? 720 : 1280; }
static inline int ui_scr_h() { return ui_portrait() ? 1280 : 720; }
// On-screen keyboard look shared by every keyboard (Wi-Fi dialog, e-mail
// editor): large letters and icons (32 px), clear key surfaces, highlighted
// control keys. Call after lv_keyboard_create().
void       ui_style_keyboard(lv_obj_t* kb);

// Picks the landscape or the portrait value
#define UI_LP(landscape, portrait) (ui_portrait() ? (portrait) : (landscape))
