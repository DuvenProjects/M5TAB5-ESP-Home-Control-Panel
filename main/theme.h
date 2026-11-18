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
