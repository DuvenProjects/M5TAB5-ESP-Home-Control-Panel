// ============================================================================
//  menu.h  -  Top bar used by every page
//   Left   : round icon + page title
//   Centre : clock + date (format from Settings > Localization)
//   Right  : [Blynk + motion - Home only] [Wi-Fi] [battery] [Settings / X]
// ============================================================================
#pragma once
#include <lvgl.h>

enum MenuMode {
  MENU_HOME,      // motion + Wi-Fi + settings
  MENU_SUBPAGE    // Wi-Fi + close (X) -> Home
};

#define MENU_HEIGHT 96

lv_obj_t* menu_create(lv_obj_t* scr, const char* title, const char* symbol,
                      MenuMode mode, lv_color_t icon_bg);
