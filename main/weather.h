// ============================================================================
//  weather.h  -  OpenWeatherMap based weather (data + page + icons)
//  Free APIs used:
//    /data/2.5/weather        current weather, sunrise / sunset
//    /data/2.5/forecast       5 day / 3 hour forecast (24 h chart + daily)
//    /data/2.5/air_pollution  air quality (AQI, PM2.5)
// ============================================================================
#pragma once
#include <lvgl.h>
#include <time.h>

#define WX_HOURS 9      // now + 8 x 3 hours = 24 hours
#define WX_DAYS  6

struct WxHour { time_t dt; float temp; float pop; char icon[4]; };
struct WxDay  { time_t dt; float tmin; float tmax; char icon[4]; };

struct WeatherData {
  bool    valid;
  time_t  updated;
  int32_t tz;                 // UTC offset of the city (s)
  time_t  observed;           // time of the current observation (UTC)
  char    city[32];
  char    desc[32];           // English description from the API (log only)
  int     cond_id;            // condition code (200..804) -> weather_cond_text()
  char    icon[4];            // "01d" ...
  float   temp, feels, tmin, tmax;
  int     humidity, pressure, clouds, visibility;
  float   wind_speed, wind_gust;
  int     wind_deg;
  time_t  sunrise, sunset;
  int     aqi;                // 1..5  (0 = none)
  float   pm25;
  float   rain_prob;          // rain probability of the next 3 hours, %
  WxHour  hours[WX_HOURS]; int nhours;
  WxDay   days[WX_DAYS];     int ndays;   // [0] = today, [1..5] = next 5 days
};

void      weather_begin();                       // starts the background task
void      weather_request_update();              // refresh now
uint32_t  weather_version();                     // incremented on every successful update
bool      weather_get(WeatherData& out);         // thread-safe copy
const char* weather_status();                    // why there is no data (translated)
const char* weather_cond_text(int owm_id);       // translated condition text
void      weather_to_display_units(WeatherData& w);  // temperatures -> unit selected in Settings

lv_obj_t* weather_create();                      // Weather page
lv_obj_t* weather_icon_create(lv_obj_t* parent, const char* code, int size);
const char* weather_wind_dir(int deg);
