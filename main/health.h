// ============================================================================
//  health.h  -  Stability guard of the panel
//
//  * Task watchdog on the main loop (LVGL, touch, pages): if the loop stops
//    for HEALTH_WDT_S seconds, the ESP32-P4 restarts by itself instead of
//    staying frozen with a dark screen.
//  * Reset reason of this start (power on, restart, watchdog, crash, low
//    voltage) on the serial monitor and in Settings > System > Information;
//    unexpected restarts are counted in NVS.
//  * Stage marker: the main loop notes which part runs now (HSTAGE("hvac
//    sensor")). If the loop stops for 25 s a small monitor task stores that
//    stage in NVS before the watchdog restarts; it is shown after the start
//    on the serial monitor and in Settings > System > Information.
//  * Every 10 minutes a health line on the serial monitor:
//      [SYS] up 5 h 12 min, RAM 180 KB (min 150 KB), PSRAM 26.1 MB (block 25.9 MB), ...
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HEALTH_WDT_S   30        // main loop may not stop longer than this

void health_begin();             // end of setup(): reset reason, watchdog
void health_loop();              // every loop(): feeds the watchdog, periodic log
void health_log(const char* where);            // one health line now
void health_stage(const char* s);              // main loop: part running now (string literal)
const char* health_stage_get();

// Scope marker: sets the stage, restores the previous one at the end
struct HealthStage {
  const char* prev;
  explicit HealthStage(const char* s) : prev(health_stage_get()) { health_stage(s); }
  ~HealthStage() { health_stage(prev); }
};
#define HSTAGE_CAT2(a, b) a##b
#define HSTAGE_CAT(a, b)  HSTAGE_CAT2(a, b)
#define HSTAGE(s) HealthStage HSTAGE_CAT(_hstage_, __LINE__)(s)

void health_reset_text(char* buf, size_t n);   // reason of this start + counter (translated)
bool health_fault_text(char* buf, size_t n);   // last unexpected restart: reason (stage), date; false = none
void health_clear_faults();                    // counter and last fault to 0 (Information window)
uint32_t health_fault_count();
