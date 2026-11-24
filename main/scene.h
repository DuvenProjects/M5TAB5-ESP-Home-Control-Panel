// ============================================================================
//  scene.h  -  Scenes (combined RGB + blinds + HVAC + alarm settings)
// ============================================================================
#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "lang.h"

struct Scene {
  StrId       name;         // translated name
  StrId       info;         // translated description
  const char* symbol;
  uint32_t    color;        // card colour
  bool        rgb_on;
  uint32_t    rgb;          // 0xRRGGBB
  uint8_t     rgb_bri;      // %
  uint8_t     rgb_fx;       // RgbEffect
  int8_t      blinds;       // 0..100, -1 = unchanged
  float       setpoint;     // 0 = unchanged
  int8_t      alarm;        // 1 arm, 0 disarm, -1 unchanged
};

#define SCENE_COUNT 8
extern const Scene SCENES[SCENE_COUNT];

void        scene_apply(uint8_t id);
const char* scene_name(uint8_t id);          // translated
lv_obj_t*   scene_create();
