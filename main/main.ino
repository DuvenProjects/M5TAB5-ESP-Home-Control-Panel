// ============================================================================
//  main.ino  -  M5Stack Tab5 (ESP32-P4)  +  LVGL 9.x  display / touch driver
//
//  This file contains ONLY the display driver and the instance of the global
//  data structure. The whole user interface is started by ui.init(); every
//  page lives in its own .cpp file.
//
//  Libraries : M5Unified, M5GFX, lvgl (9.2.x), ArduinoJson (7.x), Adafruit AHTX0
//  Board     : esp32 by Espressif 3.1+  ->  "M5Tab5" / ESP32P4 Dev Module
//              PSRAM: Enabled, Flash: 16MB, Partition: an app partition of
//              at least 4 MB (the multi-language fonts need ~1 MB)
//  lv_conf.h : copy the lv_conf.h of this project to the Arduino/libraries
//              folder (next to the lvgl folder).
// ============================================================================
#include <M5Unified.h>
#include <lvgl.h>
#include "app_data.h"
#include "ui.h"

// -------------------------------------------- global structure (30 parameters)
AppData g_data;

// -------------------------------------------- display settings
#define SCREEN_W   1280
#define SCREEN_H   720
#define BUF_LINES  120                      // height of the partial render buffers

static lv_display_t* disp;
static lv_indev_t*   touch;
static uint8_t*      buf1;
static uint8_t*      buf2;

static uint32_t my_tick() { return millis(); }

// LVGL -> M5GFX
static void disp_flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  int32_t w = lv_area_get_width(a);
  int32_t h = lv_area_get_height(a);
  M5.Display.pushImage(a->x1, a->y1, w, h, (lgfx::rgb565_t*)px);
  lv_display_flush_ready(d);
}

// GT911 touch -> LVGL
static void touch_read(lv_indev_t* indev, lv_indev_data_t* data) {
  static bool was_pressed = false;
  M5.update();
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail(0);
    if (t.isPressed()) {
      data->point.x = t.x;
      data->point.y = t.y;
      data->state   = LV_INDEV_STATE_PRESSED;
      if (!was_pressed) ui_touch_click();    // new touch: click sound
      was_pressed = true;
      ui_user_activity();                    // screen sleep timer
      return;
    }
  }
  was_pressed = false;
  data->state = LV_INDEV_STATE_RELEASED;
}

void setup() {
  ui.early();                                // first: every output to 0 (safe start)

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(1);                 // 1280 x 720 landscape (Settings applies 0 / 180 deg)
  M5.Display.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb(my_tick);

  size_t buf_size = SCREEN_W * BUF_LINES * sizeof(uint16_t);
  buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  buf2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  disp = lv_display_create(M5.Display.width(), M5.Display.height());
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, disp_flush);
  lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

  touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touch_read);

  ui.init();                                 // the whole application starts here
}

void loop() {
  lv_timer_handler();
  delay(5);
}
