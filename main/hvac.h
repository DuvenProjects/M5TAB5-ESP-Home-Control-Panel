#ifndef HVAC_H
#define HVAC_H

/* ------------------------------------------------------------------ */
/* Application configuration                                           */
/* ------------------------------------------------------------------ */

#define HVAC_VERSION          "1.0.0"

// 1 = use the generated icon font (hvac_icons.c)
// 0 = use LVGL built-in symbols instead
#define HVAC_USE_ICON_FONT    1

/* ------------------------------------------------------------------ */
/* Hardware configuration (adjust to your wiring and verify against    */
/* the M5Stack Tab5 pinout: the numbers below are placeholders)        */
/* ------------------------------------------------------------------ */

// AHT10 temperature / humidity sensor (I2C on G7 / G6, shared with the ENS160)
#define SENSOR_SDA_PIN        7
#define SENSOR_SCL_PIN        6
#define SENSOR_I2C_ADDR       0x38

// Digital outputs (heating / cooling relays)
// Moved to free pins of the Tab5 GPIO_EXT / M5-Bus connector so that they do
// not collide with the RGB / blind / PIR pins of the project.
#define HEAT_PIN              17         // (G16 is used by the PIR)
#define COOL_PIN              45
#define OUTPUT_ACTIVE_LEVEL   HIGH       // relay module energized level
#define OUTPUT_INACTIVE_LEVEL LOW

// Indicator LEDs (heating / cooling)
#define HEAT_LED_PIN          52
#define COOL_LED_PIN          5
#define LED_ACTIVE_LEVEL      HIGH
#define LED_INACTIVE_LEVEL    LOW

// PWM outputs
#define FAN_PWM_PIN           19         // fan speed control
#define DRY_PWM_PIN           18         // dry mode control
#define PWM_FREQ_HZ           20000
#define PWM_RESOLUTION_BITS   8          // duty range 0..255

/* ------------------------------------------------------------------ */
/* Public API (C++ only: hvac_icons.c includes this header as C)       */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus

// Drives every output pin (relays, LEDs, PWM) to 0. Safe to call at any time.
// ui.early() calls it as the very first thing in setup().
void hvac_pins_safe();

// Main function: sets up the sensor, loads the stored settings, builds the
// whole HVAC screen (all LVGL code is in hvac.cpp) and starts the control
// logic. Called once from ui.init() after the LVGL display / touch driver
// (main.ino) is ready. Control and UI run from an LVGL timer, so no loop
// function is needed.
void hvac_control();

// Returns to the home page. Called by the close (X) button of the HVAC screen.
// Uses the callback from hvac_set_home_callback() if one is registered,
// otherwise a built-in placeholder home page is shown.
// The HVAC control logic keeps running while the home page is shown.
void hvac_go_home();

// Shows the HVAC screen again (the placeholder home page uses it).
void hvac_open();

// Wi-Fi status icon (top right). level: 0 = not connected, 1..4 = signal bars.
// Call it whenever the connection / RSSI changes (e.g. from the Settings app).
void hvac_set_wifi_status(int level);

// Registers the real home page of the application (optional).
void hvac_set_home_callback(void (*callback)());

/* ---- Tab5 Home project integration ---- */
#include <lvgl.h>

// Returns the HVAC screen (called by ui.show(PAGE_HVAC)). The screen is
// persistent: it is NOT deleted when it is left; the control keeps running.
lv_obj_t*   hvac_create();
lv_obj_t*   hvac_screen();           // persistent screen object (nullptr before start)
void        hvac_hide_popups();      // closes the fan picker / info window

// "Quick" button of the Home page: same action as Quick on the HVAC screen
void        quick();

// For the Indoor tile of the Home page (translated texts)
bool        hvac_sensor_ok();
const char* hvac_summary();          // "Off", "Heat - Heating", "Auto - Standby" ...
const char* hvac_target_text();      // "Target 21.0 C", "Heat 21.0 / Cool 25.0" ...

// For scenes: sets the heating / cooling target of the active mode
void        hvac_apply_setpoint(float sp);

// Rebuilds the HVAC screen in the language selected in Settings
void        hvac_relocalize();

// Same running animation as on the HVAC screen (flame = heating, snowflake =
// cooling, droplet = dry, fan = fan) for other pages, e.g. the Home tile.
// Create once, then call hvac_anim_update() periodically (hidden when idle).
lv_obj_t*   hvac_anim_create(lv_obj_t* parent);
void        hvac_anim_update(lv_obj_t* icon);

// ---- Remote control (Blynk). Main / LVGL thread only. Changes are saved
//      to NVS exactly like changes made on the screen.
bool        hvac_power();                     // HVAC switched on
bool        hvac_set_power(bool on);          // false: cannot start (sensor error)
bool        hvac_change_target(float delta);  // +/- Celsius on the active mode's target;
                                              // false at the limits or in Dry / Fan mode
bool        hvac_heating();                   // heating relay on
bool        hvac_cooling();                   // cooling relay on
bool        hvac_running();                   // working now: heating, cooling, dry or fan


#endif  // __cplusplus

#endif  // HVAC_H
