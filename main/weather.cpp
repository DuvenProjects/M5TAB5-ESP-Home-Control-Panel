// ============================================================================
//  weather.cpp  -  OpenWeatherMap data task + detailed Weather page
//
//  Left panel   : location, date, big temperature + icon, condition, H/L,
//                 feels like / humidity / wind / air quality cards,
//                 sunrise - sunset arc (with the position of the sun)
//  Right top    : temperature chart of the next 24 hours (3 h steps)
//  Right middle : 5 day forecast (with min-max range bars)
//  Right bottom : details (pressure, visibility, clouds, gust, rain, PM2.5)
//  All texts follow the language selected in Settings (lang.h).
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "weather.h"
#include "ui.h"
#include "menu.h"
#include "settings.h"

// ============================================================ DATA LAYER
static SemaphoreHandle_t s_mtx  = nullptr;
static TaskHandle_t      s_task = nullptr;
static WeatherData       s_wx;
static volatile uint32_t s_ver  = 0;
static volatile int      s_code = 0;       // last HTTP code (0 = no request yet)
// Status of the data task: a string id (+ number) so that the text is shown in
// the language that is active when it is displayed
static volatile StrId    s_status_id  = S_WX_WAIT_WIFI;
static volatile int      s_status_arg = 0;

static void set_status(StrId id, int arg = 0) {
  if (id != s_status_id || arg != s_status_arg)
    Serial.printf("[WX] status %d (%d)\n", (int)id, arg);
  s_status_id  = id;
  s_status_arg = arg;
}

static String base_query() {
  String q;
  if (g_data.latitude != 0.0f || g_data.longitude != 0.0f)
    q = "lat=" + String(g_data.latitude, 4) + "&lon=" + String(g_data.longitude, 4);
  else
    q = "q=" + String(g_data.city);
  q += "&appid=" + String(g_data.owm_api_key);
  return q;
}

static bool http_json(const String& url, JsonDocument& doc, JsonDocument* filter = nullptr) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  s_code = code;
  if (code != 200) {
    if      (code == 401) set_status(S_WX_BAD_KEY);
    else if (code == 404) set_status(S_WX_NOT_FOUND);
    else if (code == 429) set_status(S_WX_LIMIT);
    else if (code < 0)    set_status(S_FMT_WX_CONN, code);
    else                  set_status(S_FMT_WX_HTTP, code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  DeserializationError err = filter
      ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
      : deserializeJson(doc, body);
  if (err) { Serial.printf("[WX] JSON %s\n", err.c_str()); set_status(S_WX_DATA_ERR); }
  return !err;
}

static bool fetch_all(WeatherData& w) {
  memset(&w, 0, sizeof(w));
  const String api = "http://api.openweathermap.org/data/2.5/";
  const String q   = base_query();

  // ---------------- 1) current weather
  {
    JsonDocument d;
    if (!http_json(api + "weather?" + q + "&units=metric&lang=en", d)) return false;
    w.temp       = d["main"]["temp"]       | NAN;
    w.feels      = d["main"]["feels_like"] | NAN;
    w.tmin       = d["main"]["temp_min"]   | NAN;
    w.tmax       = d["main"]["temp_max"]   | NAN;
    w.humidity   = d["main"]["humidity"]   | 0;
    w.pressure   = d["main"]["pressure"]   | 0;
    w.visibility = d["visibility"]         | 0;
    w.wind_speed = d["wind"]["speed"]      | 0.0f;
    w.wind_deg   = d["wind"]["deg"]        | 0;
    w.wind_gust  = d["wind"]["gust"]       | 0.0f;
    w.clouds     = d["clouds"]["all"]      | 0;
    w.sunrise    = d["sys"]["sunrise"]     | 0;
    w.sunset     = d["sys"]["sunset"]      | 0;
    w.tz         = d["timezone"]           | 0;
    w.observed   = d["dt"]                 | (long)time(nullptr);
    strlcpy(w.city, d["name"] | g_data.city, sizeof(w.city));
    strlcpy(w.desc, d["weather"][0]["description"] | "", sizeof(w.desc));
    strlcpy(w.icon, d["weather"][0]["icon"] | "01d", sizeof(w.icon));
    w.cond_id    = d["weather"][0]["id"]   | 800;       // condition code -> translated text
    if (w.desc[0]) w.desc[0] = toupper(w.desc[0]);
  }

  // ---------------- 2) 5 day / 3 hour forecast
  {
    JsonDocument filter;
    filter["list"][0]["dt"] = true;
    filter["list"][0]["main"]["temp"] = true;
    filter["list"][0]["main"]["temp_min"] = true;
    filter["list"][0]["main"]["temp_max"] = true;
    filter["list"][0]["weather"][0]["icon"] = true;
    filter["list"][0]["pop"] = true;
    JsonDocument d;
    if (http_json(api + "forecast?" + q + "&units=metric&cnt=40", d, &filter)) {
      JsonArray list = d["list"].as<JsonArray>();

      // 24 hours: [0] = now, then 8 x 3 hours
      w.hours[0].dt = time(nullptr); w.hours[0].temp = w.temp; w.hours[0].pop = 0;
      strlcpy(w.hours[0].icon, w.icon, 4);
      w.nhours = 1;
      for (JsonObject it : list) {
        if (w.nhours >= WX_HOURS) break;
        WxHour& h = w.hours[w.nhours++];
        h.dt   = it["dt"] | 0;
        h.temp = it["main"]["temp"] | 0.0f;
        h.pop  = (it["pop"] | 0.0f) * 100.0f;
        strlcpy(h.icon, it["weather"][0]["icon"] | "01d", 4);
      }
      if (list.size() > 0) w.rain_prob = (list[0]["pop"] | 0.0f) * 100.0f;

      // Daily: days[d] = local date "today + d" (d = 0..5). The days are
      // anchored to the real local date of the observation, NOT to the first
      // forecast entry: late in the evening the first 3 h entry already
      // belongs to tomorrow. Icon: the entry closest to 13:00.
      const long today = (long)((w.observed + w.tz) / 86400);
      int best[WX_DAYS];
      for (int i = 0; i < WX_DAYS; i++) {
        w.days[i].dt   = (time_t)((today + i) * 86400L + 12 * 3600L - w.tz);   // local noon
        w.days[i].tmin = 999.0f;
        w.days[i].tmax = -999.0f;
        strlcpy(w.days[i].icon, "01d", 4);
        best[i] = 99;
      }
      // today starts with the current observation
      w.days[0].tmin = min(w.tmin, w.temp);
      w.days[0].tmax = max(w.tmax, w.temp);
      strlcpy(w.days[0].icon, w.icon, 4);
      best[0] = -1;                            // keep the current icon for today

      int last = 0;                            // last day that has data
      for (JsonObject it : list) {
        time_t dt = it["dt"] | 0;
        int d = (int)((long)((dt + w.tz) / 86400) - today);
        if (d < 0 || d >= WX_DAYS) continue;
        float tmn = it["main"]["temp_min"] | 0.0f;
        float tmx = it["main"]["temp_max"] | 0.0f;
        WxDay& dd = w.days[d];
        dd.tmin = min(dd.tmin, tmn);
        dd.tmax = max(dd.tmax, tmx);
        int hour = (int)(((dt + w.tz) % 86400) / 3600);
        int diff = abs(hour - 13);
        if (diff < best[d]) { best[d] = diff; strlcpy(dd.icon, it["weather"][0]["icon"] | "01d", 4); }
        if (d > last) last = d;
      }
      w.ndays = last + 1;
      for (int i = 1; i < w.ndays; i++) {      // a day without entries (should not happen)
        if (w.days[i].tmax < -900.0f) { w.days[i].tmin = w.days[i - 1].tmin; w.days[i].tmax = w.days[i - 1].tmax; }
      }
    }
  }

  // ---------------- 3) air quality (needs lat / lon)
  if (g_data.latitude != 0.0f || g_data.longitude != 0.0f) {
    JsonDocument d;
    String url = api + "air_pollution?lat=" + String(g_data.latitude, 4) +
                 "&lon=" + String(g_data.longitude, 4) + "&appid=" + g_data.owm_api_key;
    if (http_json(url, d)) {
      w.aqi  = d["list"][0]["main"]["aqi"] | 0;
      w.pm25 = d["list"][0]["components"]["pm2_5"] | 0.0f;
    }
  }

  w.updated = time(nullptr);
  w.valid   = true;
  return true;
}

static void wx_task(void*) {
  for (;;) {
    uint32_t wait_ms = 5000;
    bool key_ok = strlen(g_data.owm_api_key) >= 32 &&
                  strcmp(g_data.owm_api_key, "OPENWEATHERMAP_API_KEY") != 0;
    if (!key_ok)                         set_status(S_WX_NO_KEY);
    else if (!settings_wifi_connected()) set_status(S_WX_WAIT_WIFI);
    if (key_ok && settings_wifi_connected()) {
      set_status(S_UPDATING);
      static WeatherData tmp;                       // static: keeps it off the task stack
      if (fetch_all(tmp)) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_wx = tmp;
        xSemaphoreGive(s_mtx);
        s_ver++;
        wait_ms = 10 * 60 * 1000;                   // every 10 minutes
        set_status(S_OK_STATE);
        Serial.printf("[WX] %s %.1fC %s\n", tmp.city, tmp.temp, tmp.desc);
      } else {
        wait_ms = 60 * 1000;                        // error: retry after 1 minute
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  }
}

// Copies the summary into g_data (in the LVGL context) on new data or a language change
static void sync_gdata_cb(lv_timer_t*) {
  static uint32_t seen = 0;
  static uint8_t  seen_lang = 0xFF;
  if (seen == s_ver && seen_lang == lang_get()) return;
  seen = s_ver;
  seen_lang = lang_get();
  WeatherData w;
  if (!weather_get(w)) return;
  g_data.outdoor_temp = w.temp;
  strlcpy(g_data.weather_desc, weather_cond_text(w.cond_id), sizeof(g_data.weather_desc));
  strlcpy(g_data.weather_icon, w.icon, sizeof(g_data.weather_icon));
}

void weather_begin() {
  if (s_mtx) return;
  s_mtx = xSemaphoreCreateMutex();
  memset(&s_wx, 0, sizeof(s_wx));
  xTaskCreatePinnedToCore(wx_task, "weather", 12288, nullptr, 3, &s_task, 0);
  lv_timer_create(sync_gdata_cb, 1000, nullptr);
}

void weather_request_update() { if (s_task) xTaskNotifyGive(s_task); }
uint32_t weather_version() { return s_ver; }
// Converts every temperature of a data copy to the unit selected in Settings
// (the data task always works in Celsius)
void weather_to_display_units(WeatherData& w) {
  if (!settings_fahrenheit()) return;
  w.temp  = settings_temp(w.temp);
  w.feels = settings_temp(w.feels);
  w.tmin  = settings_temp(w.tmin);
  w.tmax  = settings_temp(w.tmax);
  for (int i = 0; i < w.nhours; i++) w.hours[i].temp = settings_temp(w.hours[i].temp);
  for (int i = 0; i < w.ndays; i++) {
    w.days[i].tmin = settings_temp(w.days[i].tmin);
    w.days[i].tmax = settings_temp(w.days[i].tmax);
  }
}

const char* weather_status() {
  static char b[64];
  StrId id = s_status_id;
  if (id == S_FMT_WX_CONN || id == S_FMT_WX_HTTP) {
    snprintf(b, sizeof(b), tr(id), s_status_arg);
    return b;
  }
  return tr(id);
}

// OpenWeatherMap condition code -> text in the active language
const char* weather_cond_text(int id) {
  StrId s;
  if      (id >= 200 && id < 300) s = S_W_THUNDER;
  else if (id >= 300 && id < 400) s = S_W_DRIZZLE;
  else if (id == 500)             s = S_W_LIGHT_RAIN;
  else if (id == 501)             s = S_W_RAIN;
  else if (id >= 502 && id <= 504) s = S_W_HEAVY_RAIN;
  else if (id == 511)             s = S_W_FREEZING_RAIN;
  else if (id >= 520 && id < 600) s = S_W_SHOWERS;
  else if (id == 600)             s = S_W_LIGHT_SNOW;
  else if (id == 601)             s = S_W_SNOW;
  else if (id == 602)             s = S_W_HEAVY_SNOW;
  else if (id >= 611 && id <= 616) s = S_W_SLEET;
  else if (id >= 620 && id < 700) s = S_W_SNOW;
  else if (id == 701)             s = S_W_MIST;
  else if (id == 711)             s = S_W_SMOKE;
  else if (id == 721)             s = S_W_HAZE;
  else if (id == 731 || id == 751 || id == 761 || id == 762) s = S_W_DUST;
  else if (id == 741)             s = S_W_FOG;
  else if (id == 771)             s = S_W_SQUALL;
  else if (id == 781)             s = S_W_TORNADO;
  else if (id == 800)             s = S_W_CLEAR;
  else if (id == 801)             s = S_W_FEW_CLOUDS;
  else if (id == 802)             s = S_W_SCATTERED;
  else if (id == 803)             s = S_W_BROKEN;
  else                            s = S_W_OVERCAST;
  return tr(s);
}

bool weather_get(WeatherData& out) {
  if (!s_mtx) return false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  out = s_wx;
  xSemaphoreGive(s_mtx);
  return out.valid;
}

const char* weather_wind_dir(int deg) {
  return tr((StrId)(S_WIND_N + ((deg % 360 + 360 + 22) % 360) / 45));
}

// ============================================================ ICON DRAWING
static lv_obj_t* blob(lv_obj_t* p, int x, int y, int w, int h, lv_color_t c, int radius = LV_RADIUS_CIRCLE) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static void draw_sun(lv_obj_t* p, int x, int y, int d) {
  lv_obj_t* s = blob(p, x, y, d, d, ui_col(0xFFC83D));
  lv_obj_set_style_shadow_width(s, d / 2, 0);
  lv_obj_set_style_shadow_color(s, ui_col(0xFFB000), 0);
  lv_obj_set_style_shadow_opa(s, LV_OPA_50, 0);
}

static void draw_moon(lv_obj_t* p, int x, int y, int d, lv_color_t bg) {
  lv_obj_t* m = blob(p, x, y, d, d, ui_col(0xE6E0FF));
  lv_obj_set_style_shadow_width(m, d / 2, 0);
  lv_obj_set_style_shadow_color(m, ui_col(0x9A8CFF), 0);
  lv_obj_set_style_shadow_opa(m, LV_OPA_40, 0);
  blob(p, x + d * 35 / 100, y - d * 12 / 100, d * 85 / 100, d * 85 / 100, bg);   // crescent
}

static void draw_cloud(lv_obj_t* p, int x, int y, int w, lv_color_t c) {
  int h = w * 45 / 100;
  blob(p, x,             y + h * 40 / 100, w,            h * 60 / 100, c);
  blob(p, x + w * 12 / 100, y + h * 15 / 100, w * 40 / 100, w * 40 / 100, c);
  blob(p, x + w * 36 / 100, y - h * 10 / 100, w * 50 / 100, w * 50 / 100, c);
}

lv_obj_t* weather_icon_create(lv_obj_t* parent, const char* code, int sz) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, sz, sz);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_clip_corner(box, false, 0);

  int  id    = atoi(code ? code : "01");
  bool night = code && strlen(code) >= 3 && code[2] == 'n';
  lv_color_t bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
  lv_color_t white = ui_col(0xF2F4F8);
  lv_color_t grey  = ui_col(0xB8BEC8);

  switch (id) {
    case 1:                                            // clear
      if (night) draw_moon(box, sz * 22 / 100, sz * 22 / 100, sz * 56 / 100, bg);
      else       draw_sun (box, sz * 20 / 100, sz * 20 / 100, sz * 60 / 100);
      break;
    case 2:                                            // few clouds
      if (night) draw_moon(box, sz * 8 / 100, sz * 8 / 100, sz * 46 / 100, bg);
      else       draw_sun (box, sz * 8 / 100, sz * 8 / 100, sz * 50 / 100);
      draw_cloud(box, sz * 24 / 100, sz * 48 / 100, sz * 70 / 100, white);
      break;
    case 3:
    case 4:                                            // cloudy
      draw_cloud(box, sz * 30 / 100, sz * 22 / 100, sz * 60 / 100, grey);
      draw_cloud(box, sz * 8 / 100,  sz * 42 / 100, sz * 76 / 100, white);
      break;
    case 9:
    case 10:                                           // rain
      draw_cloud(box, sz * 10 / 100, sz * 18 / 100, sz * 80 / 100, grey);
      for (int i = 0; i < 3; i++)
        blob(box, sz * (28 + i * 20) / 100, sz * 68 / 100, max(3, sz / 18), sz * 22 / 100,
             ui_col(0x5AB4FF), 4);
      break;
    case 11: {                                         // thunderstorm
      draw_cloud(box, sz * 10 / 100, sz * 14 / 100, sz * 80 / 100, ui_col(0x8E95A3));
      lv_obj_t* l = lv_label_create(box);
      lv_label_set_text(l, LV_SYMBOL_CHARGE);
      lv_obj_set_style_text_color(l, C_YELLOW, 0);
      lv_obj_set_style_text_font(l, sz >= 80 ? FONT_32 : FONT_20, 0);
      lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
      break;
    }
    case 13:                                           // snow
      draw_cloud(box, sz * 10 / 100, sz * 16 / 100, sz * 80 / 100, grey);
      for (int i = 0; i < 3; i++)
        blob(box, sz * (26 + i * 20) / 100, sz * (70 + (i % 2) * 10) / 100, sz / 10, sz / 10, white);
      break;
    default:                                           // mist (50)
      for (int i = 0; i < 4; i++)
        blob(box, sz * (10 + (i % 2) * 8) / 100, sz * (22 + i * 16) / 100,
             sz * 72 / 100, max(4, sz / 12), grey, 6);
      break;
  }
  return box;
}

// ============================================================ PAGE
static const uint32_t AQI_COL[] = {0x808080,0x34C759,0xA8E05F,0xFFD60A,0xFF9F0A,0xFF453A};

#define CH_X   20
#define CH_Y   58
#define CH_W   750
#define CH_H   118
#define CH_PL  26
#define CH_PR  26
#define CH_PT  30
#define CH_PB  8

static struct {
  lv_obj_t *city, *date, *icon_box, *temp, *desc, *hl;
  lv_obj_t *feels, *hum, *wind, *aqi, *aqi_dot;
  lv_obj_t *sun_cont, *arc, *sun_dot, *sun_info, *daylen, *sunrise, *sunset;
  lv_obj_t *updated;
  lv_obj_t *chart; lv_chart_series_t* ser;
  lv_obj_t *h_temp[WX_HOURS], *h_time[WX_HOURS];
  lv_obj_t *d_name[5], *d_icon[5], *d_hi[5], *d_lo[5], *d_bar[5];
  lv_obj_t *det[6];
  uint32_t ver;
  WeatherData wx;
} W;

static void fmt_hm(char* out, size_t n, time_t t, int32_t tz) {
  time_t lt = t + tz; struct tm tmv; gmtime_r(&lt, &tmv);
  snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

static lv_obj_t* panel(lv_obj_t* scr, int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
  lv_obj_t* p = ui_tile_create(scr, x, y, w, h, ui_col(c1));
  lv_obj_set_style_bg_grad_color(p, ui_col(c2), 0);
  lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_pad_all(p, 0, 0);
  lv_obj_set_style_transform_scale(p, 256, LV_STATE_PRESSED);  // panels do not shrink when pressed
  lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
  return p;
}

static lv_obj_t* mini_card(lv_obj_t* p, int x, int y, const char* title, lv_obj_t** val) {
  lv_obj_t* c = lv_obj_create(p);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, 188, 72);
  lv_obj_set_style_radius(c, 18, 0);
  lv_obj_set_style_bg_color(c, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_10, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_color(c, ui_col(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(c, LV_OPA_20, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t = ui_label(c, title, FONT_14, C_TEXT2);
  lv_obj_set_pos(t, 16, 10);
  *val = ui_label(c, "--", FONT_24, C_TEXT);
  lv_obj_set_pos(*val, 16, 34);
  return c;
}

static void refresh_click_cb(lv_event_t* e) {
  lv_label_set_text(W.updated, tr(S_UPDATING));
  weather_request_update();
}

// --------------------------------------------------- sun arc
static void update_sun() {
  const WeatherData& w = W.wx;
  if (!w.valid || w.sunrise == 0) return;
  time_t now = time(nullptr);
  float p; bool day = false;
  char buf[48];
  if (now < w.sunrise) {
    p = 0; long s = w.sunrise - now;
    snprintf(buf, sizeof(buf), tr(S_FMT_SUNRISE_IN), s / 3600, (s % 3600) / 60);
  } else if (now > w.sunset) {
    p = 1; long s = (w.sunrise + 86400) - now;
    snprintf(buf, sizeof(buf), tr(S_FMT_SUNRISE_IN), s / 3600, (s % 3600) / 60);
  } else {
    day = true;
    p = (float)(now - w.sunrise) / (float)(w.sunset - w.sunrise);
    long s = w.sunset - now;
    snprintf(buf, sizeof(buf), tr(S_FMT_SUNSET_IN), s / 3600, (s % 3600) / 60);
  }
  lv_label_set_text(W.sun_info, buf);
  lv_arc_set_value(W.arc, (int)(p * 1000));

  const int ARC = 240, AX = 77, AY = 18, R = ARC / 2 - 3, DOT = 24;
  float a = (180.0f + p * 180.0f) * (float)M_PI / 180.0f;
  int cx = AX + ARC / 2 + (int)(R * cosf(a));
  int cy = AY + ARC / 2 + (int)(R * sinf(a));
  lv_obj_set_pos(W.sun_dot, cx - DOT / 2, cy - DOT / 2);
  lv_obj_set_style_bg_color(W.sun_dot, day ? ui_col(0xFFC83D) : ui_col(0x8A8FB8), 0);
  lv_obj_set_style_shadow_width(W.sun_dot, day ? 24 : 0, 0);

  long dl = w.sunset - w.sunrise;
  lv_label_set_text_fmt(W.daylen, tr(S_FMT_DAYLIGHT), dl / 3600, (dl % 3600) / 60);
  char t1[8], t2[8];
  fmt_hm(t1, sizeof(t1), w.sunrise, w.tz);
  fmt_hm(t2, sizeof(t2), w.sunset, w.tz);
  lv_label_set_text_fmt(W.sunrise, LV_SYMBOL_UP "  %s  %s", tr(S_SUNRISE), t1);
  lv_label_set_text_fmt(W.sunset,  "%s  %s  " LV_SYMBOL_DOWN, tr(S_SUNSET), t2);
}

// --------------------------------------------------- fill every value
static void update_all() {
  const WeatherData& w = W.wx;
  char b[64];

  lv_label_set_text_fmt(W.city, LV_SYMBOL_GPS "  %s", w.valid ? w.city : g_data.city);
  if (!w.valid) {
    lv_label_set_text(W.desc, weather_status());
    return;
  }

  lv_obj_clean(W.icon_box);
  weather_icon_create(W.icon_box, w.icon, 110);
  lv_label_set_text_fmt(W.temp, "%d°", (int)lroundf(w.temp));
  lv_label_set_text(W.desc, weather_cond_text(w.cond_id));
  lv_label_set_text_fmt(W.hl, tr(S_FMT_HL),
                        (int)lroundf(w.ndays ? w.days[0].tmax : w.tmax),
                        (int)lroundf(w.ndays ? w.days[0].tmin : w.tmin));
  lv_label_set_text_fmt(W.feels, "%d°", (int)lroundf(w.feels));
  lv_label_set_text_fmt(W.hum, "%d %%", w.humidity);
  lv_label_set_text_fmt(W.wind, "%d km/h %s", (int)lroundf(w.wind_speed * 3.6f), weather_wind_dir(w.wind_deg));
  int aqi = constrain(w.aqi, 0, 5);
  lv_label_set_text_fmt(W.aqi, "   %s", aqi ? tr((StrId)(S_AQI_GOOD + aqi - 1)) : "--");
  lv_obj_set_style_bg_color(W.aqi_dot, ui_col(AQI_COL[aqi]), 0);

  char hm[8]; fmt_hm(hm, sizeof(hm), w.updated, w.tz);
  { char ub[96]; snprintf(ub, sizeof(ub), tr(S_FMT_UPDATED), hm);
    lv_label_set_text_fmt(W.updated, "%s  " LV_SYMBOL_REFRESH, ub); }

  // ---- 24 hour chart
  if (w.nhours > 1) {
    float mn = 999, mx = -999;
    for (int i = 0; i < w.nhours; i++) { mn = min(mn, w.hours[i].temp); mx = max(mx, w.hours[i].temp); }
    int ymin = (int)floorf(mn) - 2, ymax = (int)ceilf(mx) + 2;
    lv_chart_set_point_count(W.chart, w.nhours);
    lv_chart_set_range(W.chart, LV_CHART_AXIS_PRIMARY_Y, ymin * 10, ymax * 10);
    int cw = CH_W - CH_PL - CH_PR, chh = CH_H - CH_PT - CH_PB;
    for (int i = 0; i < WX_HOURS; i++) {
      if (i >= w.nhours) {
        lv_obj_add_flag(W.h_temp[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(W.h_time[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_remove_flag(W.h_temp[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(W.h_time[i], LV_OBJ_FLAG_HIDDEN);
      float t = w.hours[i].temp;
      lv_chart_set_value_by_id(W.chart, W.ser, i, (int32_t)lroundf(t * 10));
      int px = CH_X + CH_PL + cw * i / (w.nhours - 1);
      int py = CH_Y + CH_PT + (int)(chh * (ymax - t) / (float)(ymax - ymin));
      lv_label_set_text_fmt(W.h_temp[i], "%d°", (int)lroundf(t));
      lv_obj_set_pos(W.h_temp[i], px - 35, py - 34);
      if (i == 0) lv_label_set_text(W.h_time[i], tr(S_NOW));
      else { fmt_hm(b, sizeof(b), w.hours[i].dt, w.tz); lv_label_set_text(W.h_time[i], b); }
      lv_obj_set_pos(W.h_time[i], px - 35, CH_Y + CH_H + 8);
    }
    lv_chart_refresh(W.chart);
  }

  // ---- 5 day forecast (from tomorrow)
  float gmin = 999, gmax = -999;
  for (int i = 1; i < w.ndays; i++) { gmin = min(gmin, w.days[i].tmin); gmax = max(gmax, w.days[i].tmax); }
  for (int c = 0; c < 5; c++) {
    int i = c + 1;
    lv_obj_clean(W.d_icon[c]);
    if (i >= w.ndays) {
      lv_label_set_text(W.d_name[c], "--");
      lv_label_set_text(W.d_hi[c], ""); lv_label_set_text(W.d_lo[c], "");
      continue;
    }
    time_t lt = w.days[i].dt + w.tz; struct tm tmv; gmtime_r(&lt, &tmv);
    lv_label_set_text(W.d_name[c], lang_day_short(tmv.tm_wday));
    char ic[4]; strlcpy(ic, w.days[i].icon, 4); if (strlen(ic) == 3) ic[2] = 'd';
    weather_icon_create(W.d_icon[c], ic, 56);
    lv_label_set_text_fmt(W.d_hi[c], "%d°", (int)lroundf(w.days[i].tmax));
    lv_label_set_text_fmt(W.d_lo[c], "%d°", (int)lroundf(w.days[i].tmin));
    lv_bar_set_range(W.d_bar[c], (int)floorf(gmin), (int)ceilf(gmax));
    lv_bar_set_start_value(W.d_bar[c], (int)lroundf(w.days[i].tmin), LV_ANIM_OFF);
    lv_bar_set_value(W.d_bar[c], (int)lroundf(w.days[i].tmax), LV_ANIM_OFF);
  }

  // ---- details
  lv_label_set_text_fmt(W.det[0], "%d hPa", w.pressure);
  lv_label_set_text_fmt(W.det[1], "%.1f km", w.visibility / 1000.0f);
  lv_label_set_text_fmt(W.det[2], "%d %%", w.clouds);
  lv_label_set_text_fmt(W.det[3], "%d km/h", (int)lroundf(w.wind_gust * 3.6f));
  lv_label_set_text_fmt(W.det[4], "%d %%", (int)lroundf(w.rain_prob));
  if (w.aqi) lv_label_set_text_fmt(W.det[5], "%.1f ug/m3", w.pm25);
  else       lv_label_set_text(W.det[5], "--");

  update_sun();
}

static void page_timer_cb(lv_timer_t* t) {
  // date
  if (ui_time_valid()) {
    time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
    char db[64];
    lang_format_long_date(db, sizeof(db), &lt);
    lv_label_set_text(W.date, db);
  }
  uint32_t v = weather_version() * 2 + (settings_fahrenheit() ? 1 : 0);   // new data or unit
  if (v != W.ver) {
    W.ver = v;
    weather_get(W.wx);
    weather_to_display_units(W.wx);
    update_all();
  } else {
    if (!W.wx.valid) lv_label_set_text(W.desc, weather_status());
    update_sun();
  }
}

lv_obj_t* weather_create() {
  memset(&W, 0, sizeof(W));
  W.ver = 0xFFFFFFFF;

  lv_obj_t* scr = ui_screen_create();
  menu_create(scr, tr(S_WEATHER), LV_SYMBOL_IMAGE, MENU_SUBPAGE, ui_col(0x3A3F7A));

  // =========================================================== LEFT PANEL
  lv_obj_t* L = panel(scr, 20, 108, 430, 592, 0x2E3168, 0x16172A);

  W.city = ui_label(L, "", FONT_20, ui_col(0xC8CCF0));
  lv_obj_set_pos(W.city, 24, 20);
  W.date = ui_label(L, "", FONT_20, C_TEXT2);
  lv_obj_set_pos(W.date, 24, 52);

  W.icon_box = lv_obj_create(L);
  lv_obj_remove_style_all(W.icon_box);
  lv_obj_set_size(W.icon_box, 110, 110);
  lv_obj_set_pos(W.icon_box, 22, 92);
  lv_obj_set_style_bg_color(W.icon_box, ui_col(0x262952), 0);  // background for the crescent
  lv_obj_remove_flag(W.icon_box, LV_OBJ_FLAG_SCROLLABLE);

  W.temp = ui_label(L, "--°", FONT_48, C_TEXT);
  lv_obj_set_pos(W.temp, 150, 86);
  lv_obj_set_style_transform_scale(W.temp, 400, 0);          // ~1.56x large
  lv_obj_set_style_transform_pivot_x(W.temp, 0, 0);
  lv_obj_set_style_transform_pivot_y(W.temp, 0, 0);

  W.desc = ui_label(L, tr(S_LOADING), FONT_24, C_TEXT);
  lv_obj_set_pos(W.desc, 152, 166);
  W.hl = ui_label(L, "", FONT_16, C_TEXT2);
  lv_obj_set_pos(W.hl, 152, 198);

  mini_card(L, 18,  232, tr(S_FEELS_LIKE), &W.feels);
  mini_card(L, 222, 232, tr(S_HUMIDITY_CAP), &W.hum);
  mini_card(L, 18,  314, tr(S_WIND_CAP), &W.wind);
  lv_obj_t* aq = mini_card(L, 222, 314, tr(S_AIR_QUALITY_CAP), &W.aqi);
  W.aqi_dot = lv_obj_create(aq);
  lv_obj_remove_style_all(W.aqi_dot);
  lv_obj_set_size(W.aqi_dot, 14, 14);
  lv_obj_set_pos(W.aqi_dot, 16, 44);
  lv_obj_set_style_radius(W.aqi_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(W.aqi_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(W.aqi_dot, ui_col(0x808080), 0);
  lv_obj_set_style_text_font(W.aqi, FONT_20, 0);
  lv_obj_set_pos(W.aqi, 22, 38);

  // ---- sunrise / sunset arc
  W.sun_cont = lv_obj_create(L);
  lv_obj_remove_style_all(W.sun_cont);
  lv_obj_set_size(W.sun_cont, 394, 150);
  lv_obj_set_pos(W.sun_cont, 18, 398);
  lv_obj_remove_flag(W.sun_cont, LV_OBJ_FLAG_SCROLLABLE);

  W.arc = lv_arc_create(W.sun_cont);
  lv_obj_set_size(W.arc, 240, 240);
  lv_obj_set_pos(W.arc, 77, 18);
  lv_arc_set_rotation(W.arc, 180);
  lv_arc_set_bg_angles(W.arc, 0, 180);
  lv_arc_set_range(W.arc, 0, 1000);
  lv_arc_set_value(W.arc, 0);
  lv_obj_remove_style(W.arc, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(W.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(W.arc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(W.arc, ui_col(0x5A5F96), LV_PART_MAIN);
  lv_obj_set_style_arc_width(W.arc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(W.arc, ui_col(0xFFC83D), LV_PART_INDICATOR);

  // horizon line
  lv_obj_t* hz = lv_obj_create(W.sun_cont);
  lv_obj_remove_style_all(hz);
  lv_obj_set_size(hz, 300, 2);
  lv_obj_set_pos(hz, 47, 18 + 120);
  lv_obj_set_style_bg_color(hz, ui_col(0x5A5F96), 0);
  lv_obj_set_style_bg_opa(hz, LV_OPA_COVER, 0);

  W.sun_dot = lv_obj_create(W.sun_cont);
  lv_obj_remove_style_all(W.sun_dot);
  lv_obj_set_size(W.sun_dot, 24, 24);
  lv_obj_set_style_radius(W.sun_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(W.sun_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(W.sun_dot, ui_col(0xFFC83D), 0);
  lv_obj_set_style_shadow_color(W.sun_dot, ui_col(0xFFB000), 0);
  lv_obj_set_style_shadow_opa(W.sun_dot, LV_OPA_60, 0);
  lv_obj_set_pos(W.sun_dot, 77 - 12, 138 - 12);

  W.sun_info = ui_label(W.sun_cont, "", FONT_16, C_TEXT);
  lv_obj_align(W.sun_info, LV_ALIGN_TOP_MID, 0, 74);
  W.daylen = ui_label(W.sun_cont, "", FONT_14, C_TEXT2);
  lv_obj_align(W.daylen, LV_ALIGN_TOP_MID, 0, 100);

  W.sunrise = ui_label(L, "", FONT_16, C_TEXT);
  lv_obj_set_pos(W.sunrise, 24, 552);
  W.sunset = ui_label(L, "", FONT_16, C_TEXT);
  lv_obj_align(W.sunset, LV_ALIGN_TOP_RIGHT, -24, 552);

  // =========================================================== 24 HOURS
  lv_obj_t* H = panel(scr, 470, 108, 790, 232, 0x34306A, 0x1C1B33);
  lv_obj_t* ht = ui_label(H, tr(S_NEXT_24H), FONT_16, C_TEXT2);
  lv_obj_set_pos(ht, 24, 18);
  lv_obj_set_style_text_letter_space(ht, 2, 0);

  W.updated = ui_label(H, "", FONT_14, C_TEXT2);
  lv_obj_align(W.updated, LV_ALIGN_TOP_RIGHT, -24, 20);
  lv_obj_add_flag(W.updated, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(W.updated, 16);
  lv_obj_add_event_cb(W.updated, refresh_click_cb, LV_EVENT_CLICKED, nullptr);

  W.chart = lv_chart_create(H);
  lv_obj_set_pos(W.chart, CH_X, CH_Y);
  lv_obj_set_size(W.chart, CH_W, CH_H);
  lv_chart_set_type(W.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(W.chart, 0, 0);
  lv_chart_set_point_count(W.chart, WX_HOURS);
  lv_obj_set_style_bg_opa(W.chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(W.chart, 0, 0);
  lv_obj_set_style_pad_left(W.chart, CH_PL, 0);
  lv_obj_set_style_pad_right(W.chart, CH_PR, 0);
  lv_obj_set_style_pad_top(W.chart, CH_PT, 0);
  lv_obj_set_style_pad_bottom(W.chart, CH_PB, 0);
  lv_obj_set_style_line_width(W.chart, 4, LV_PART_ITEMS);
  lv_obj_set_style_width(W.chart, 10, LV_PART_INDICATOR);
  lv_obj_set_style_height(W.chart, 10, LV_PART_INDICATOR);
  lv_obj_remove_flag(W.chart, LV_OBJ_FLAG_CLICKABLE);
  W.ser = lv_chart_add_series(W.chart, ui_col(0x5EE6D0), LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < WX_HOURS; i++) {
    W.h_temp[i] = ui_label(H, "", FONT_16, C_TEXT);
    lv_obj_set_width(W.h_temp[i], 70);
    lv_obj_set_style_text_align(W.h_temp[i], LV_TEXT_ALIGN_CENTER, 0);
    W.h_time[i] = ui_label(H, "", FONT_14, C_TEXT2);
    lv_obj_set_width(W.h_time[i], 70);
    lv_obj_set_style_text_align(W.h_time[i], LV_TEXT_ALIGN_CENTER, 0);
  }

  // =========================================================== 5 DAYS
  lv_obj_t* D = panel(scr, 470, 354, 790, 176, 0x22243E, 0x1A1B2C);
  for (int c = 0; c < 5; c++) {
    int x = c * 158;
    if (c > 0) {
      lv_obj_t* sep = lv_obj_create(D);
      lv_obj_remove_style_all(sep);
      lv_obj_set_size(sep, 1, 130);
      lv_obj_set_pos(sep, x, 22);
      lv_obj_set_style_bg_color(sep, ui_col(0x3A3C5A), 0);
      lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    }
    W.d_name[c] = ui_label(D, "--", FONT_20, C_TEXT2);
    lv_obj_set_width(W.d_name[c], 158);
    lv_obj_set_style_text_align(W.d_name[c], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(W.d_name[c], x, 14);

    W.d_icon[c] = lv_obj_create(D);
    lv_obj_remove_style_all(W.d_icon[c]);
    lv_obj_set_size(W.d_icon[c], 56, 56);
    lv_obj_set_pos(W.d_icon[c], x + 51, 44);
    lv_obj_set_style_bg_color(W.d_icon[c], ui_col(0x1E2035), 0);
    lv_obj_remove_flag(W.d_icon[c], LV_OBJ_FLAG_SCROLLABLE);

    W.d_hi[c] = ui_label(D, "", FONT_24, C_TEXT);
    lv_obj_set_pos(W.d_hi[c], x + 30, 108);
    W.d_lo[c] = ui_label(D, "", FONT_24, C_TEXT2);
    lv_obj_set_pos(W.d_lo[c], x + 88, 108);

    W.d_bar[c] = lv_bar_create(D);
    lv_obj_set_size(W.d_bar[c], 110, 6);
    lv_obj_set_pos(W.d_bar[c], x + 24, 148);
    lv_bar_set_mode(W.d_bar[c], LV_BAR_MODE_RANGE);
    lv_obj_set_style_bg_color(W.d_bar[c], ui_col(0x3A3C5A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(W.d_bar[c], ui_col(0x5AB4FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(W.d_bar[c], ui_col(0x7CE08A), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(W.d_bar[c], LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
  }

  // =========================================================== DETAILS
  lv_obj_t* X = panel(scr, 470, 544, 790, 156, 0x1C2A30, 0x161E22);
  lv_obj_t* xt = ui_label(X, "", FONT_16, C_TEXT2);
  lv_obj_set_pos(xt, 24, 14);
  lv_obj_set_style_text_letter_space(xt, 2, 0);
  lv_label_set_text_fmt(xt, LV_SYMBOL_LIST "  %s", tr(S_DETAILS));
  static const StrId names[6] = {S_DET_PRESSURE, S_DET_VISIBILITY, S_DET_CLOUDS,
                                 S_DET_GUST, S_DET_RAIN, S_DET_PM25};
  for (int i = 0; i < 6; i++) {
    int col = i % 3, row = i / 3;
    int x = 24 + col * 252, y = 46 + row * 54;
    lv_obj_t* n = ui_label(X, tr(names[i]), FONT_16, C_TEXT2);
    lv_obj_set_pos(n, x, y + 4);
    W.det[i] = ui_label(X, "--", FONT_20, C_TEXT);
    lv_obj_set_pos(W.det[i], x + 142, y + 2);
  }

  lv_timer_t* t = ui_page_timer(scr, page_timer_cb, 1000, nullptr);
  page_timer_cb(t);
  return scr;
}
