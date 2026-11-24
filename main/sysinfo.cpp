// ============================================================================
//  sysinfo.cpp  -  System statistics (see sysinfo.h). No LVGL code.
// ============================================================================
#include "sysinfo.h"

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <esp_freertos_hooks.h>

#include "hvac.h"           // hvac_running(), hvac_heating(), hvac_cooling()

#define RUNTIME_SAVE_S   600          // NVS write at most every 10 min of run time

// ------------------------------------------------------------------ CPU load
// The idle hook runs about once per RTOS tick while a core has nothing to do
// (it returns true, so the core sleeps until the next interrupt). Idle calls
// per second compared with the tick rate give the idle share of the core.
#define MAX_CORES 2
static volatile uint32_t s_idleCnt[MAX_CORES];
static uint32_t          s_idleMax[MAX_CORES];
static int               s_load[MAX_CORES] = {-1, -1};

static bool idleHook0() { s_idleCnt[0]++; return true; }
static bool idleHook1() { s_idleCnt[1]++; return true; }

static void cpuLoadUpdate() {
  for (int c = 0; c < sysinfo_cpu_cores(); c++) {
    uint32_t n = s_idleCnt[c];
    s_idleCnt[c] = 0;
    if (n > s_idleMax[c]) s_idleMax[c] = n;        // other interrupts can add calls
    int load = 100 - (int)((uint64_t)n * 100 / s_idleMax[c]);
    s_load[c] = load < 0 ? 0 : load > 100 ? 100 : load;
  }
}

int sysinfo_cpu_cores() { return portNUM_PROCESSORS < MAX_CORES ? portNUM_PROCESSORS : MAX_CORES; }

int sysinfo_cpu_load(int core) {
  if (core < 0 || core >= sysinfo_cpu_cores()) return -1;
  return s_load[core];
}

// ------------------------------------------------------------------ HVAC run time
static Preferences s_prefs;
static HvacRuntime s_rt;
static uint32_t    s_unsaved = 0;                  // run seconds since the last NVS write

static uint32_t currentMonth() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;                  // clock not set yet
  struct tm t;
  localtime_r(&now, &t);
  return (uint32_t)(t.tm_year + 1900) * 100 + (uint32_t)(t.tm_mon + 1);
}

void sysinfo_save() {
  s_prefs.putUInt("month", s_rt.month);
  s_prefs.putUInt("tot",   s_rt.total_s);
  s_prefs.putUInt("heat",  s_rt.heat_s);
  s_prefs.putUInt("cool",  s_rt.cool_s);
  s_prefs.putUInt("pmon",  s_rt.prev_month);
  s_prefs.putUInt("ptot",  s_rt.prev_total_s);
  s_unsaved = 0;
}

void sysinfo_begin() {
  for (int c = 0; c < sysinfo_cpu_cores(); c++) s_idleMax[c] = configTICK_RATE_HZ;
  esp_register_freertos_idle_hook_for_cpu(idleHook0, 0);
#if portNUM_PROCESSORS > 1
  esp_register_freertos_idle_hook_for_cpu(idleHook1, 1);
#endif

  s_prefs.begin("sysinfo", false);
  s_rt.month        = s_prefs.getUInt("month", 0);
  s_rt.total_s      = s_prefs.getUInt("tot", 0);
  s_rt.heat_s       = s_prefs.getUInt("heat", 0);
  s_rt.cool_s       = s_prefs.getUInt("cool", 0);
  s_rt.prev_month   = s_prefs.getUInt("pmon", 0);
  s_rt.prev_total_s = s_prefs.getUInt("ptot", 0);
}

void sysinfo_tick() {
  cpuLoadUpdate();

  // New month (once the clock is set): last month's total is kept, counters restart
  uint32_t m = currentMonth();
  if (m != 0 && m != s_rt.month) {
    if (s_rt.month != 0) {
      s_rt.prev_month   = s_rt.month;
      s_rt.prev_total_s = s_rt.total_s;
      s_rt.total_s = s_rt.heat_s = s_rt.cool_s = 0;
    }
    s_rt.month = m;
    sysinfo_save();
  }

  // Run time: every second the HVAC works (heating, cooling, dry or fan)
  if (hvac_running()) {
    s_rt.total_s++;
    if (hvac_heating()) s_rt.heat_s++;
    if (hvac_cooling()) s_rt.cool_s++;
    if (++s_unsaved >= RUNTIME_SAVE_S) sysinfo_save();
  }
}

const HvacRuntime& sysinfo_hvac_runtime() { return s_rt; }
