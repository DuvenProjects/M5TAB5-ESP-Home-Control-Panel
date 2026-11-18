// ============================================================================
//  sysinfo.h  -  System statistics (no LVGL code)
//    - CPU load per core (FreeRTOS idle hooks, approximate)
//    - HVAC run time of the current month (saved in NVS, reset when a new
//      month starts) and of the previous month
//  The values are shown by the Info window of Settings > System.
// ============================================================================
#pragma once
#include <stdint.h>

struct HvacRuntime {
  uint32_t month;          // yyyymm of the counted month, 0 = not known yet
  uint32_t total_s;        // seconds the HVAC was running (any action)
  uint32_t heat_s;         // of which heating
  uint32_t cool_s;         // of which cooling
  uint32_t prev_month;     // yyyymm of the previous month, 0 = none
  uint32_t prev_total_s;   // total of the previous month
};

void               sysinfo_begin();          // ui.init(), after hvac_control()
void               sysinfo_tick();           // every second (ui.cpp slow timer)
void               sysinfo_save();           // before a restart (ui_prepare_restart)

int                sysinfo_cpu_load(int core);   // 0..100 %, -1 = no such core
int                sysinfo_cpu_cores();
const HvacRuntime& sysinfo_hvac_runtime();
