// ============================================================================
//  rgb.h  -  RGB LED (PWM) control and the RGB page
//   Pins in app_data.h: PIN_RGB_R / PIN_RGB_G / PIN_RGB_B
// ============================================================================
#pragma once
#include <lvgl.h>

enum RgbEffect { RGB_STATIC = 0, RGB_BREATH, RGB_RAINBOW, RGB_CANDLE, RGB_EFFECT_COUNT };
const char* rgb_effect_name(uint8_t fx);         // translated effect name

void      rgb_hw_init();                         // starts the ledc PWM
void      rgb_tick();                            // effect engine (100 ms)
void      rgb_apply();                           // applies the g_data values to the LED
void      rgb_set(bool on, uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint8_t effect);
lv_color_t rgb_current_color();                  // for the Home tile

lv_obj_t* rgb_create();                          // RGB page
