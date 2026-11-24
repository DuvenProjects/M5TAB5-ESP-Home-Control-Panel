// ============================================================================
//  aq.cpp  -  Indoor air quality (ENS160) + Home tile + Air quality page
//
//  Sensor : DFRobot_ENS160 library, I2C on Port A (shared with the AHT10)
//           read every 2 s; temperature / humidity compensation from the
//           AHT10 every 60 s; one history sample per minute (last 2 hours)
//  Tile   : AQI badge, level text, 5 step colour bar, eCO2 and TVOC
//  Page   : AQI ring + advice, eCO2 / TVOC cards with level bars,
//           history chart, sensor state and compensation info
//  Levels follow the UBA scale of the ENS160 (1 excellent .. 5 unhealthy).
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <DFRobot_ENS160.h>
#include "aq.h"
#include "ui.h"
#include "menu.h"
#include "hvac.h"        // hvac_sensor_ok(): AHT10 values for compensation
#include "settings.h"    // temperature unit
#include "health.h"         // stage marker (watchdog diagnosis)

// ============================================================ SENSOR
static DFRobot_ENS160_I2C s_ens(&Wire, ENS160_I2C_ADDR);

#define AQ_READ_MS       2000UL     // reading interval
#define AQ_RETRY_MS      10000UL    // retry interval while the sensor is missing
#define AQ_COMP_MS       60000UL    // compensation update interval
#define AQ_HIST_MS       60000UL    // one history sample per minute
#define AQ_HIST_N        120        // 2 hours
#define AQ_FAIL_LIMIT    3          // failed reads before "sensor error"
#define AQ_NONE          0xFFFF     // empty history slot

static AqData   s_data = {AQ_NO_SENSOR, 0, 0, 0, false};
static bool     s_present   = false;
static uint8_t  s_fails     = 0;
static uint32_t s_lastRead  = 0, s_lastRetry = 0, s_lastComp = 0, s_lastHist = 0;
static float    s_compT = NAN, s_compH = NAN;      // values sent to the sensor (Celsius, %RH)
static uint32_t s_ver = 1;                         // changes when the data changes
static uint32_t s_histVer = 1;                     // changes when a history sample is added

static uint16_t s_histEco2[AQ_HIST_N];
static uint16_t s_histTvoc[AQ_HIST_N];
static uint8_t  s_histHead = 0;                    // next write position
static uint8_t  s_histCount = 0;

// Colour of each level (0 = no data)
static const uint32_t AQ_COL[6] = {0x8E8E93, 0x34C759, 0xA8E05F, 0xFFD60A, 0xFF9F0A, 0xFF453A};

static bool i2cPresent() {
  Wire.beginTransmission(ENS160_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

static void setData(const AqData& d) {
  if (memcmp(&d, &s_data, sizeof(AqData)) != 0) {
    s_data = d;
    s_ver++;
  }
}

// Sends the AHT10 temperature / humidity to the ENS160 (improves accuracy)
static void compensate() {
  AqData d = s_data;
  if (hvac_sensor_ok() && !isnan(g_data.indoor_temp) && !isnan(g_data.indoor_hum)) {
    s_compT = g_data.indoor_temp;
    s_compH = g_data.indoor_hum;
    s_ens.setTempAndHum(s_compT, s_compH);
    d.compensated = true;
  } else {
    d.compensated = false;
  }
  setData(d);
}

static bool tryBegin() {
  if (!i2cPresent()) return false;
  if (s_ens.begin() != NO_ERR) return false;
  s_ens.setPWRMode(ENS160_STANDARD_MODE);
  s_present = true;
  s_fails = 0;
  compensate();
  s_lastComp = millis();
  Serial.println("[AQ] ENS160 found");
  return true;
}

static void readSensor() {
  AqData d = s_data;
  if (!i2cPresent()) {
    if (++s_fails >= AQ_FAIL_LIMIT) {
      s_present = false;
      d.state = AQ_NO_SENSOR;
      d.aqi = 0; d.eco2 = 0; d.tvoc = 0; d.compensated = false;
      setData(d);
      Serial.println("[AQ] ENS160 lost");
    }
    return;
  }
  s_fails = 0;

  uint8_t st = s_ens.getENS160Status();     // 0 normal, 1 warm-up, 2 initial start-up, 3 invalid
  switch (st) {
    case 0:  d.state = AQ_OK;      break;
    case 1:  d.state = AQ_WARMUP;  break;
    case 2:  d.state = AQ_INITIAL; break;
    default: d.state = AQ_INVALID; break;
  }
  if (d.state == AQ_OK || d.state == AQ_INITIAL) {
    uint8_t  aqi  = s_ens.getAQI();
    uint16_t eco2 = s_ens.getECO2();
    uint16_t tvoc = s_ens.getTVOC();
    if (aqi >= 1 && aqi <= 5 && eco2 >= 400) {
      d.aqi = aqi; d.eco2 = eco2; d.tvoc = tvoc;
    } else {
      d.state = AQ_INVALID;
    }
  }
  if (d.state == AQ_WARMUP || d.state == AQ_INVALID) { d.aqi = 0; d.eco2 = 0; d.tvoc = 0; }
  setData(d);
}

static void addHistory() {
  bool ok = aq_has_values();
  s_histEco2[s_histHead] = ok ? s_data.eco2 : AQ_NONE;
  s_histTvoc[s_histHead] = ok ? s_data.tvoc : AQ_NONE;
  s_histHead = (s_histHead + 1) % AQ_HIST_N;
  if (s_histCount < AQ_HIST_N) s_histCount++;
  s_histVer++;
}

static void aqTimerCb(lv_timer_t* t) {
  (void)t;
  HSTAGE("air quality sensor");
  uint32_t now = millis();
  if (!s_present) {
    if (now - s_lastRetry >= AQ_RETRY_MS) { s_lastRetry = now; tryBegin(); }
  } else {
    if (now - s_lastRead >= AQ_READ_MS) { s_lastRead = now; readSensor(); }
    if (now - s_lastComp >= AQ_COMP_MS) { s_lastComp = now; compensate(); }
  }
  if (now - s_lastHist >= AQ_HIST_MS) { s_lastHist = now; addHistory(); }
}

void aq_begin() {
  for (int i = 0; i < AQ_HIST_N; i++) { s_histEco2[i] = AQ_NONE; s_histTvoc[i] = AQ_NONE; }
  uint32_t now = millis();
  s_lastRetry = now;
  s_lastHist  = now;
  if (tryBegin()) readSensor();
  s_lastRead = now;
  lv_timer_create(aqTimerCb, 500, nullptr);
}

const AqData& aq_get() { return s_data; }

bool aq_has_values() { return s_data.state == AQ_OK || s_data.state == AQ_INITIAL; }

// ============================================================ HELPERS
static int eco2Level(uint16_t v) { return v < 600 ? 1 : v < 800 ? 2 : v < 1000 ? 3 : v < 1500 ? 4 : 5; }
static int tvocLevel(uint16_t v) { return v < 65 ? 1 : v < 220 ? 2 : v < 660 ? 3 : v < 2200 ? 4 : 5; }

static const char* levelText(int lvl) { return tr((StrId)(S_AQ_L1 + (lvl - 1))); }

// Short state text: level name, or why there is no value
static const char* stateText() {
  switch (s_data.state) {
    case AQ_NO_SENSOR: return tr(S_AQ_SENSOR_ERR);
    case AQ_WARMUP:    return tr(S_WARMING_UP);
    case AQ_INVALID:   return tr(S_AQ_ST_INVALID);
    default:           return levelText(s_data.aqi);
  }
}

const char* aq_state_text() { return stateText(); }

// Long sensor state for the page
static const char* sensorStateText() {
  switch (s_data.state) {
    case AQ_NO_SENSOR: return tr(S_AQ_SENSOR_ERR);
    case AQ_WARMUP:    return tr(S_WARMING_UP);
    case AQ_INITIAL:   return tr(S_AQ_ST_INITIAL);
    case AQ_INVALID:   return tr(S_AQ_ST_INVALID);
    default:           return tr(S_OK_STATE);
  }
}

static lv_obj_t* box(lv_obj_t* p, int x, int y, int w, int h, uint32_t col, int r) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_bg_color(o, ui_col(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// 5 step colour bar; the active step is fully opaque and taller
struct LevelBar { lv_obj_t* seg[5]; int y; };

static void levelBarCreate(LevelBar& b, lv_obj_t* p, int x, int y, int w) {
  const int gap = 6, sw = (w - 4 * gap) / 5;
  b.y = y;
  for (int i = 0; i < 5; i++) b.seg[i] = box(p, x + i * (sw + gap), y, sw, 10, AQ_COL[i + 1], 5);
}

static void levelBarSet(LevelBar& b, int lvl) {
  for (int i = 0; i < 5; i++) {
    bool on = (i + 1 == lvl);
    lv_obj_set_style_bg_opa(b.seg[i], on ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_height(b.seg[i], on ? 14 : 10);
    lv_obj_set_y(b.seg[i], on ? b.y - 2 : b.y);
  }
}

static void goPageCb(lv_event_t* e) { ui.show(PAGE_AQ); }

// ============================================================ HOME TILE
#define TILE_BG 0x16241F

struct TileCtx {
  lv_obj_t *badge, *num, *level, *eco2, *eco2Unit, *tvoc, *tvocUnit;
  LevelBar bar;
  uint32_t ver;
};

static void tileUpdate(TileCtx* c) {
  if (c->ver == s_ver) return;
  c->ver = s_ver;
  bool ok = aq_has_values();
  int lvl = ok ? s_data.aqi : 0;
  lv_color_t col = ui_col(AQ_COL[lvl]);

  lv_obj_set_style_bg_color(c->badge, lv_color_mix(col, ui_col(TILE_BG), 80), 0);
  lv_obj_set_style_text_color(c->num, col, 0);
  if (ok) lv_label_set_text_fmt(c->num, "%d", lvl);
  else    lv_label_set_text(c->num, s_data.state == AQ_NO_SENSOR ? "!" : "--");

  lv_label_set_text(c->level, stateText());
  lv_obj_set_style_text_color(c->level, ok ? col : (s_data.state == AQ_NO_SENSOR ? C_RED : C_TEXT2), 0);
  levelBarSet(c->bar, lvl);

  if (ok) {
    lv_label_set_text_fmt(c->eco2, "%u", (unsigned)s_data.eco2);
    lv_label_set_text_fmt(c->tvoc, "%u", (unsigned)s_data.tvoc);
  } else {
    lv_label_set_text(c->eco2, "--");
    lv_label_set_text(c->tvoc, "--");
  }
  lv_obj_set_style_text_color(c->eco2, ok ? ui_col(AQ_COL[eco2Level(s_data.eco2)]) : C_TEXT2, 0);
  lv_obj_set_style_text_color(c->tvoc, ok ? ui_col(AQ_COL[tvocLevel(s_data.tvoc)]) : C_TEXT2, 0);
  lv_obj_align_to(c->eco2Unit, c->eco2, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
  lv_obj_align_to(c->tvocUnit, c->tvoc, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
}

static void tileTimerCb(lv_timer_t* t) { tileUpdate((TileCtx*)lv_timer_get_user_data(t)); }
static void tileFreeCb(lv_event_t* e) { lv_free(lv_event_get_user_data(e)); }

lv_obj_t* aq_tile_create(lv_obj_t* parent, int x, int y, int w, int h) {
  TileCtx* c = (TileCtx*)lv_malloc(sizeof(TileCtx));
  memset(c, 0, sizeof(TileCtx));

  lv_obj_t* t = ui_tile_create(parent, x, y, w, h, ui_col(TILE_BG));
  lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(t, goPageCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(t, tileFreeCb, LV_EVENT_DELETE, c);
  const int iw = w - 36;                            // inner width (padding 18)

  // badge with the AQI number
  c->badge = box(t, 0, 0, 64, 64, 0x2A2A2E, LV_RADIUS_CIRCLE);
  c->num = ui_label(c->badge, "--", FONT_32, C_TEXT);
  lv_obj_center(c->num);

  lv_obj_t* title = ui_label(t, tr(S_AIR_QUALITY), FONT_20, C_TEXT);
  lv_obj_set_pos(title, 76, 4);
  lv_obj_set_width(title, iw - 76);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  c->level = ui_label(t, "", FONT_16, C_TEXT2);
  lv_obj_set_pos(c->level, 76, 36);
  lv_obj_set_width(c->level, iw - 76);
  lv_label_set_long_mode(c->level, LV_LABEL_LONG_DOT);

  levelBarCreate(c->bar, t, 0, 84, iw);

  // eCO2 / TVOC columns
  const int cx2 = iw / 2 + 6;
  lv_obj_t* l = ui_label(t, "eCO2", FONT_14, C_TEXT2);
  lv_obj_set_pos(l, 0, 110);
  l = ui_label(t, "TVOC", FONT_14, C_TEXT2);
  lv_obj_set_pos(l, cx2, 110);
  c->eco2 = ui_label(t, "--", FONT_28, C_TEXT);
  lv_obj_set_pos(c->eco2, 0, 130);
  c->tvoc = ui_label(t, "--", FONT_28, C_TEXT);
  lv_obj_set_pos(c->tvoc, cx2, 130);
  c->eco2Unit = ui_label(t, "ppm", FONT_14, C_TEXT2);
  c->tvocUnit = ui_label(t, "ppb", FONT_14, C_TEXT2);

  c->ver = 0;
  lv_timer_t* tm = ui_page_timer(lv_obj_get_screen(parent), tileTimerCb, 1000, c);
  tileTimerCb(tm);
  return t;
}

// ============================================================ PAGE
#define CH_POINTS AQ_HIST_N
#define COL_ECO2  0x5EE6D0
#define COL_TVOC  0xFFB070

static struct {
  lv_obj_t *arc, *num, *level, *advice, *sensor, *comp;
  lv_obj_t *eco2, *eco2Lvl, *eco2Unit, *tvoc, *tvocLvl, *tvocUnit;
  LevelBar eco2Bar, tvocBar;
  lv_obj_t *chart, *noData, *eco2Max, *tvocMax;
  lv_chart_series_t *serEco2, *serTvoc;
  uint32_t ver, histVer;
} P;

static void pageValues() {
  bool ok = aq_has_values();
  int lvl = ok ? s_data.aqi : 0;
  lv_color_t col = ui_col(AQ_COL[lvl]);

  // ---- AQI ring + level + advice
  lv_arc_set_value(P.arc, lvl);
  lv_obj_set_style_arc_color(P.arc, col, LV_PART_INDICATOR);
  if (ok) lv_label_set_text_fmt(P.num, "%d", lvl);
  else    lv_label_set_text(P.num, s_data.state == AQ_NO_SENSOR ? "!" : "--");
  lv_obj_set_style_text_color(P.num, ok ? col : C_TEXT2, 0);
  lv_label_set_text(P.level, stateText());
  lv_obj_set_style_text_color(P.level, ok ? col : (s_data.state == AQ_NO_SENSOR ? C_RED : C_TEXT2), 0);
  lv_label_set_text(P.advice, ok ? tr((StrId)(S_AQ_ADV1 + (lvl - 1))) : "");

  // ---- sensor state + compensation
  lv_label_set_text_fmt(P.sensor, "%s:  %s", tr(S_AQ_SENSOR), sensorStateText());
  if (s_data.compensated && !isnan(s_compT)) {
    char t[24], b[128];
    snprintf(t, sizeof(t), "%.1f %s", settings_temp(s_compT), settings_temp_unit());
    snprintf(b, sizeof(b), tr(S_FMT_AQ_COMP), t, (int)lroundf(s_compH));
    lv_label_set_text(P.comp, b);
  } else {
    lv_label_set_text(P.comp, s_data.state == AQ_NO_SENSOR ? "" : tr(S_AQ_NO_COMP));
  }

  // ---- eCO2 / TVOC cards
  if (ok) {
    int le = eco2Level(s_data.eco2), lt = tvocLevel(s_data.tvoc);
    lv_label_set_text_fmt(P.eco2, "%u", (unsigned)s_data.eco2);
    lv_label_set_text_fmt(P.tvoc, "%u", (unsigned)s_data.tvoc);
    lv_label_set_text(P.eco2Lvl, levelText(le));
    lv_label_set_text(P.tvocLvl, levelText(lt));
    lv_obj_set_style_text_color(P.eco2Lvl, ui_col(AQ_COL[le]), 0);
    lv_obj_set_style_text_color(P.tvocLvl, ui_col(AQ_COL[lt]), 0);
    levelBarSet(P.eco2Bar, le);
    levelBarSet(P.tvocBar, lt);
  } else {
    lv_label_set_text(P.eco2, "--");
    lv_label_set_text(P.tvoc, "--");
    lv_label_set_text(P.eco2Lvl, "");
    lv_label_set_text(P.tvocLvl, "");
    levelBarSet(P.eco2Bar, 0);
    levelBarSet(P.tvocBar, 0);
  }
  lv_obj_align_to(P.eco2Unit, P.eco2, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);
  lv_obj_align_to(P.tvocUnit, P.tvoc, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);
}

static void pageChart() {
  uint16_t maxE = 0, maxT = 0;
  int valid = 0;
  // oldest -> newest; empty slots at the start of the chart
  for (int i = 0; i < CH_POINTS; i++) {
    int age = CH_POINTS - 1 - i;                    // 0 = newest
    uint16_t e = AQ_NONE, t = AQ_NONE;
    if (age < s_histCount) {
      int idx = (s_histHead - 1 - age + 2 * AQ_HIST_N) % AQ_HIST_N;
      e = s_histEco2[idx];
      t = s_histTvoc[idx];
    }
    if (e != AQ_NONE) { valid++; if (e > maxE) maxE = e; if (t > maxT) maxT = t; }
    lv_chart_set_value_by_id(P.chart, P.serEco2, i, e == AQ_NONE ? LV_CHART_POINT_NONE : (int32_t)e);
    lv_chart_set_value_by_id(P.chart, P.serTvoc, i, t == AQ_NONE ? LV_CHART_POINT_NONE : (int32_t)t);
  }
  int topE = maxE < 1000 ? 1000 : ((maxE * 11 / 10 + 99) / 100) * 100;
  int topT = maxT < 300 ? 300 : ((maxT * 11 / 10 + 49) / 50) * 50;
  lv_chart_set_range(P.chart, LV_CHART_AXIS_PRIMARY_Y, 400, topE);
  lv_chart_set_range(P.chart, LV_CHART_AXIS_SECONDARY_Y, 0, topT);
  lv_label_set_text_fmt(P.eco2Max, "%d ppm", topE);
  lv_label_set_text_fmt(P.tvocMax, "%d ppb", topT);
  if (valid) lv_obj_add_flag(P.noData, LV_OBJ_FLAG_HIDDEN);
  else       lv_obj_remove_flag(P.noData, LV_OBJ_FLAG_HIDDEN);
  lv_chart_refresh(P.chart);
}

static void pageTimerCb(lv_timer_t* t) {
  if (P.ver != s_ver)        { P.ver = s_ver; pageValues(); }
  if (P.histVer != s_histVer) { P.histVer = s_histVer; pageChart(); }
}

static lv_obj_t* card(lv_obj_t* scr, int x, int y, int w, int h) {
  lv_obj_t* c = ui_tile_create(scr, x, y, w, h, C_TILE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_transform_scale(c, 256, LV_STATE_PRESSED);
  return c;
}

static void valueCard(lv_obj_t* scr, int x, const char* caption, const char* unit,
                      lv_obj_t** val, lv_obj_t** unitLbl, lv_obj_t** lvlLbl, LevelBar& bar) {
  const int w = UI_LP(390, 330);
  lv_obj_t* c = card(scr, x, UI_LP(108, 542), w, 196);
  lv_obj_t* l = ui_label(c, caption, FONT_20, C_TEXT2);
  lv_obj_set_pos(l, 0, 0);
  *lvlLbl = ui_label(c, "", FONT_20, C_TEXT2);
  lv_obj_align(*lvlLbl, LV_ALIGN_TOP_RIGHT, 0, 0);
  *val = ui_label(c, "--", FONT_48, C_TEXT);
  lv_obj_set_pos(*val, 0, 46);
  *unitLbl = ui_label(c, unit, FONT_20, C_TEXT2);
  levelBarCreate(bar, c, 0, 136, w - 36);
}

lv_obj_t* aq_create() {
  memset(&P, 0, sizeof(P));
  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_AIR_QUALITY), LV_SYMBOL_LOOP, MENU_SUBPAGE, ui_col(0x1F4A3A));

  // Landscape: AQI card on the left, values + history on the right.
  // Portrait : AQI card (arc left, level + advice right), the two value
  //            cards side by side, history at the bottom.
  const bool port = ui_portrait();
  const int tx = port ? 270 : 0, tw = port ? 374 : 384;     // level / advice texts

  // =========================================================== LEFT: AQI
  lv_obj_t* L = port ? card(scr, 20, 108, 680, 420) : card(scr, 20, 108, 420, 592);
  lv_obj_t* cap = ui_label(L, tr(S_AQ_INDEX), FONT_20, C_TEXT2);
  if (port) lv_obj_set_pos(cap, 0, 0);
  else      lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);

  P.arc = lv_arc_create(L);
  lv_obj_set_size(P.arc, 230, 230);
  if (port) lv_obj_set_pos(P.arc, 10, 40);
  else      lv_obj_align(P.arc, LV_ALIGN_TOP_MID, 0, 40);
  lv_arc_set_bg_angles(P.arc, 135, 45);
  lv_arc_set_range(P.arc, 0, 5);
  lv_arc_set_value(P.arc, 0);
  lv_obj_remove_style(P.arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(P.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(P.arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(P.arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(P.arc, ui_col(0x3A3A40), LV_PART_MAIN);
  P.num = ui_label(P.arc, "--", FONT_48, C_TEXT);
  lv_obj_align(P.num, LV_ALIGN_CENTER, 0, -10);
  lv_obj_t* of5 = ui_label(P.arc, "/ 5", FONT_20, C_TEXT2);
  lv_obj_align(of5, LV_ALIGN_CENTER, 0, 36);

  P.level = ui_label(L, "", FONT_32, C_TEXT);
  lv_obj_set_width(P.level, tw);
  lv_obj_set_style_text_align(P.level, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(P.level, tx, port ? 70 : 286);
  P.advice = ui_label(L, "", FONT_20, C_TEXT);
  lv_obj_set_width(P.advice, tw);
  lv_label_set_long_mode(P.advice, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(P.advice, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(P.advice, tx, port ? 124 : 340);

  const int sy = port ? 286 : 444, sw = port ? 644 : 384;  // sensor / compensation lines
  box(L, 0, sy, sw, 1, 0x3A3A40, 0);
  P.sensor = ui_label(L, "", FONT_16, C_TEXT2);
  lv_obj_set_width(P.sensor, sw);
  lv_obj_set_pos(P.sensor, 0, sy + 18);
  P.comp = ui_label(L, "", FONT_16, C_TEXT2);
  lv_obj_set_width(P.comp, sw);
  lv_label_set_long_mode(P.comp, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(P.comp, 0, sy + 48);

  // =========================================================== RIGHT TOP: values
  valueCard(scr, port ? 20 : 460, "eCO2", "ppm", &P.eco2, &P.eco2Unit, &P.eco2Lvl, P.eco2Bar);
  valueCard(scr, port ? 370 : 870, "TVOC", "ppb", &P.tvoc, &P.tvocUnit, &P.tvocLvl, P.tvocBar);

  // =========================================================== RIGHT BOTTOM: history
  const int chh = port ? 340 : 236;                  // chart height
  lv_obj_t* H = port ? card(scr, 20, 752, 680, 508) : card(scr, 460, 324, 800, 376);
  lv_obj_t* ht = ui_label(H, tr(S_AQ_HISTORY), FONT_20, C_TEXT2);
  lv_obj_set_pos(ht, 0, 0);
  // legend
  lv_obj_t* lg = ui_label(H, "TVOC (ppb)", FONT_16, C_TEXT);
  lv_obj_align(lg, LV_ALIGN_TOP_RIGHT, 0, 2);
  lv_obj_t* dot = box(H, 0, 0, 12, 12, COL_TVOC, LV_RADIUS_CIRCLE);
  lv_obj_align_to(dot, lg, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_obj_t* lg2 = ui_label(H, "eCO2 (ppm)", FONT_16, C_TEXT);
  lv_obj_align_to(lg2, dot, LV_ALIGN_OUT_LEFT_MID, -24, 0);
  lv_obj_t* dot2 = box(H, 0, 0, 12, 12, COL_ECO2, LV_RADIUS_CIRCLE);
  lv_obj_align_to(dot2, lg2, LV_ALIGN_OUT_LEFT_MID, -8, 0);

  P.chart = lv_chart_create(H);
  lv_obj_set_pos(P.chart, 0, 64);
  lv_obj_set_size(P.chart, port ? 644 : 764, chh);
  lv_chart_set_type(P.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(P.chart, CH_POINTS);
  lv_chart_set_div_line_count(P.chart, 4, 0);
  lv_obj_set_style_bg_opa(P.chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(P.chart, 0, 0);
  lv_obj_set_style_pad_all(P.chart, 4, 0);
  lv_obj_set_style_line_color(P.chart, ui_col(0x2E2E34), LV_PART_MAIN);
  lv_obj_set_style_line_width(P.chart, 3, LV_PART_ITEMS);
  lv_obj_set_style_width(P.chart, 0, LV_PART_INDICATOR);     // no point markers
  lv_obj_set_style_height(P.chart, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(P.chart, LV_OBJ_FLAG_CLICKABLE);
  P.serEco2 = lv_chart_add_series(P.chart, ui_col(COL_ECO2), LV_CHART_AXIS_PRIMARY_Y);
  P.serTvoc = lv_chart_add_series(P.chart, ui_col(COL_TVOC), LV_CHART_AXIS_SECONDARY_Y);

  P.eco2Max = ui_label(H, "", FONT_14, ui_col(COL_ECO2));
  lv_obj_set_pos(P.eco2Max, 0, 40);
  P.tvocMax = ui_label(H, "", FONT_14, ui_col(COL_TVOC));
  lv_obj_align(P.tvocMax, LV_ALIGN_TOP_RIGHT, 0, 40);
  lv_obj_t* l = ui_label(H, "-2 h", FONT_14, C_TEXT2);
  lv_obj_set_pos(l, 0, 76 + chh);
  l = ui_label(H, tr(S_NOW), FONT_14, C_TEXT2);
  lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, 76 + chh);
  P.noData = ui_label(H, tr(S_AQ_NO_DATA), FONT_20, C_TEXT2);
  lv_obj_align(P.noData, LV_ALIGN_TOP_MID, 0, 64 + chh / 2 - 12);

  P.ver = 0;
  P.histVer = 0;
  lv_timer_t* t = ui_page_timer(scr, pageTimerCb, 1000, nullptr);
  pageTimerCb(t);
  return scr;
}
