// ============================================================================
//  home.h  -  Home page (dashboard)
//   Top row    : clock / date | indoor (HVAC + humidity + Quick) | weather
//   Bottom row : blinds       | RGB LED | scenes                  | alarm
// ============================================================================
#pragma once
#include <lvgl.h>

lv_obj_t* home_create();
