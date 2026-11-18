// ============================================================================
//  lights.h  -  Blind motor control and the Blinds page
//   Two outputs per blind: UP (open) and DOWN (close). The position is
//   calculated from the full travel time (g_data.blind_travel_s), 0..100 %.
// ============================================================================
#pragma once
#include <lvgl.h>
#include "app_data.h"

const char* blind_name(int idx);              // translated blind name

void      blinds_hw_init();                    // prepares the pins (called by ui.init)
void      blinds_tick();                       // every 100 ms (called by ui)
void      blinds_set_target(int idx, int pos); // 0..100   (idx = -1 -> all)
void      blinds_stop(int idx);                // idx = -1 -> all
bool      blinds_moving(int idx);
int       blinds_average();                    // average % for the Home tile

lv_obj_t* lights_create();                     // Blinds page
