// ============================================================================
//  health.cpp  -  Stability guard (see health.h)
// ============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include "health.h"
#include "lang.h"
#include "settings.h"          // settings_wifi_connected()
#include "blynk.h"             // blynk_connected()
#include <time.h>

static esp_reset_reason_t s_reason = ESP_RST_UNKNOWN;
static uint32_t           s_unexpected = 0;        // watchdog / crash / brownout restarts (NVS)
static bool               s_wdt = false;
static uint32_t           s_lastLogMs = 0;
static volatile const char* s_stage = "setup";
static volatile uint32_t  s_feedMs = 0;
static char               s_lastStuck[40] = "";    // stage stored before the last watchdog restart
// Last unexpected restart (NVS, kept until cleared): reason, time, stage
static uint8_t            s_lfReason = 0;
static uint32_t           s_lfTime = 0;              // epoch, 0 = clock not set
static char               s_lfStage[40] = "";

void health_stage(const char* s) { s_stage = s; }
const char* health_stage_get() { return (const char*)s_stage; }

// Watches the main loop: 25 s without feeding -> note the stage in NVS
// (the watchdog restarts at HEALTH_WDT_S = 30 s)
static void monitor_task(void*) {
  bool saved = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t idle = millis() - s_feedMs;
    if (idle < (HEALTH_WDT_S - 5) * 1000UL) { saved = false; continue; }
    if (saved) continue;
    saved = true;
    const char* st = (const char*)s_stage;
    Serial.printf("[SYS] main loop stopped for %lu s in: %s\n", (unsigned long)(idle / 1000), st ? st : "?");
    Preferences p;
    if (p.begin("health", false)) { p.putString("stuck", st ? st : "?"); p.end(); }
  }
}

static const char* reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "CRASH (panic)";
    case ESP_RST_INT_WDT:   return "WATCHDOG (interrupt)";
    case ESP_RST_TASK_WDT:  return "WATCHDOG (task)";
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_BROWNOUT:  return "LOW VOLTAGE (brownout)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_USB:       return "USB reset (program upload / serial monitor)";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_EFUSE:     return "efuse error";
    case ESP_RST_PWR_GLITCH:return "POWER GLITCH";
    case ESP_RST_CPU_LOCKUP:return "CRASH (CPU lock-up)";
    default:                return "other";
  }
}

static bool unexpected(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT || r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH ||
         r == ESP_RST_CPU_LOCKUP;
}

void health_begin() {
  s_reason = esp_reset_reason();
  Preferences p;
  if (p.begin("health", false)) {
    s_unexpected = p.getUInt("bad", 0);
    if (unexpected(s_reason)) p.putUInt("bad", ++s_unexpected);
    if (p.isKey("stuck")) {                          // stage noted before the restart
      if (unexpected(s_reason)) p.getString("stuck", s_lastStuck, sizeof(s_lastStuck));
      p.remove("stuck");
    }
    if (unexpected(s_reason)) {                      // remember this one as the last fault
      time_t now = time(nullptr);
      s_lfReason = (uint8_t)s_reason;
      s_lfTime = (now > 1700000000) ? (uint32_t)now : 0;   // RTC time is set before (ui.init)
      strlcpy(s_lfStage, s_lastStuck, sizeof(s_lfStage));
      p.putUChar("lf_r", s_lfReason);
      p.putUInt("lf_t", s_lfTime);
      p.putString("lf_s", s_lfStage);
    } else {
      s_lfReason = p.getUChar("lf_r", 0);
      s_lfTime = p.getUInt("lf_t", 0);
      p.getString("lf_s", s_lfStage, sizeof(s_lfStage));
    }
    p.end();
  }
  Serial.printf("[SYS] start, reset reason: %s (unexpected restarts so far: %lu)\n",
                reason_name(s_reason), (unsigned long)s_unexpected);
  if (s_lastStuck[0]) Serial.printf("[SYS] the main loop had stopped in: %s\n", s_lastStuck);
  if (s_lfReason && !unexpected(s_reason)) {
    char b[96];
    health_fault_text(b, sizeof(b));
    Serial.printf("[SYS] last unexpected restart: %s\n", b);
  }

  // Task watchdog for the main loop: HEALTH_WDT_S seconds, then panic -> restart
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = HEALTH_WDT_S * 1000;
  cfg.idle_core_mask = 0;                // idle tasks are not watched (long TLS / JPEG work is fine)
  cfg.trigger_panic = true;
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);
  if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&cfg);   // watchdog not running yet
  if (e == ESP_OK && esp_task_wdt_add(nullptr) == ESP_OK) s_wdt = true;
  s_feedMs = millis();
  s_stage = "loop";
  xTaskCreatePinnedToCore(monitor_task, "health", 3072, nullptr, 1, nullptr, 0);
  Serial.printf("[SYS] main loop watchdog %s (%d s)\n", s_wdt ? "on" : "NOT available", HEALTH_WDT_S);
  health_log("start");
}

void health_log(const char* where) {
  uint32_t s = millis() / 1000;
  Serial.printf("[SYS] %s: up %lu h %lu min, RAM %u KB (min %u KB), PSRAM %u KB (block %u KB), "
                "Wi-Fi %s, Blynk %s\n", where,
                (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                settings_wifi_connected() ? "on" : "OFF", blynk_connected() ? "on" : "off");
}

void health_loop() {
  if (s_wdt) esp_task_wdt_reset();
  uint32_t now = millis();
  s_feedMs = now;
  if (now - s_lastLogMs >= 10UL * 60UL * 1000UL) {
    s_lastLogMs = now;
    health_log("health");
  }
}

static StrId reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return S_RST_POWERON;
    case ESP_RST_SW:       return S_RST_SW;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP: return S_RST_PANIC;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return S_RST_WDT;
    case ESP_RST_BROWNOUT: return S_RST_BROWNOUT;
    case ESP_RST_PWR_GLITCH: return S_RST_GLITCH;
    case ESP_RST_USB:
    case ESP_RST_JTAG:     return S_RST_USB;
    default:               return S_RST_OTHER;
  }
}

uint32_t health_fault_count() { return s_unexpected; }

bool health_fault_text(char* buf, size_t n) {
  if (!s_lfReason) { snprintf(buf, n, "%s", tr(S_RST_NO_FAULT)); return false; }
  char st[48] = "", tm[24] = "";
  if (s_lfStage[0]) snprintf(st, sizeof(st), " (%s)", s_lfStage);
  if (s_lfTime) {
    time_t t = s_lfTime; struct tm lt; localtime_r(&t, &lt);
    strftime(tm, sizeof(tm), "   ·   %d.%m.%Y %H:%M", &lt);
  }
  snprintf(buf, n, "%s%s%s", tr(reason_str((esp_reset_reason_t)s_lfReason)), st, tm);
  return true;
}

void health_clear_faults() {
  s_unexpected = 0; s_lfReason = 0; s_lfTime = 0; s_lfStage[0] = 0;
  Preferences p;
  if (p.begin("health", false)) {
    p.putUInt("bad", 0); p.remove("lf_r"); p.remove("lf_t"); p.remove("lf_s");
    p.end();
  }
  Serial.println("[SYS] fault counter cleared");
}

void health_reset_text(char* buf, size_t n) {
  StrId id = reason_str(s_reason);
  if (s_lastStuck[0])
    snprintf(buf, n, "%s (%s)   ·   %s: %lu", tr(id), s_lastStuck, tr(S_RST_UNEXPECTED), (unsigned long)s_unexpected);
  else
    snprintf(buf, n, "%s   ·   %s: %lu", tr(id), tr(S_RST_UNEXPECTED), (unsigned long)s_unexpected);
}
