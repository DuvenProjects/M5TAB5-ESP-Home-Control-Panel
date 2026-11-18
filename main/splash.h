// ============================================================================
//  splash.h  -  Start-up screen: project logo + "ESP HOME CONTROL PANEL"
// ============================================================================
#pragma once
#include <lvgl.h>

#define SPLASH_TIME_MS  1000      // minimum time the start-up screen is shown

// Builds the start-up screen (not loaded). ui.init() loads it, finishes the
// start-up work and then shows the Home page.
lv_obj_t* splash_create();
