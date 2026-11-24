// ============================================================================
//  aq.h  -  Indoor air quality: ENS160 sensor (DFRobot_ENS160), Home tile and
//           Air quality page. All LVGL code is in aq.cpp.
//
//  Wiring: the ENS160 shares the I2C bus of the AHT10 (Wire, SDA G7 / SCL G6)
//  with the AHT10 of the HVAC module. hvac_control() starts Wire, so
//  aq_begin() must be called after it.
//  The AHT10 temperature / humidity is used to compensate the gas readings.
// ============================================================================
#pragma once
#include <lvgl.h>
#include <stdint.h>

#define ENS160_I2C_ADDR   0x53    // SDO high (DFRobot default); 0x52 when SDO is grounded

enum AqState : uint8_t {
  AQ_NO_SENSOR = 0,   // not found / not responding
  AQ_WARMUP,          // first 3 minutes after power-on (no valid data)
  AQ_INITIAL,         // initial start-up: first hour of use (data usable, less accurate)
  AQ_OK,              // normal operation
  AQ_INVALID          // sensor reports invalid output
};

struct AqData {
  AqState  state;
  uint8_t  aqi;           // UBA air quality index 1 (excellent) .. 5 (unhealthy), 0 = none
  uint16_t eco2;          // equivalent CO2, ppm (400 .. 65000)
  uint16_t tvoc;          // total volatile organic compounds, ppb (0 .. 65000)
  bool     compensated;   // AHT10 temperature / humidity sent to the sensor
};

void          aq_begin();              // ui.init(), after hvac_control()
const AqData& aq_get();
bool          aq_has_values();         // aqi / eco2 / tvoc are valid (OK or INITIAL)
const char*   aq_state_text();         // level name ("Good" ...) or sensor state, translated

// Home tile: creates the whole tile (tap -> PAGE_AQ); it updates itself
lv_obj_t*     aq_tile_create(lv_obj_t* parent, int x, int y, int w, int h);

lv_obj_t*     aq_create();             // Air quality page (ui.show(PAGE_AQ))
