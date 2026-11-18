#ifndef SETTINGS_H
#define SETTINGS_H

/* ------------------------------------------------------------------ */
/* Application configuration                                           */
/* ------------------------------------------------------------------ */

// Software version shown on the System page
#define SETTINGS_VERSION        "1.1.0"

// 1 = use the generated icon font (settings_icons.c)
// 0 = use LVGL built-in symbols instead (delete nothing, just set to 0)
#define SETTINGS_USE_ICON_FONT  1

// M5Stack Tab5: Wi-Fi is provided by the ESP32-C6 co-processor over SDIO.
// These pins are passed to WiFi.setPins() before Wi-Fi is first used.
// Verify them against the M5Tab5 Wi-Fi example of your board package version.
#define WIFI_SDIO_CLK           12
#define WIFI_SDIO_CMD           13
#define WIFI_SDIO_D0            11
#define WIFI_SDIO_D1            10
#define WIFI_SDIO_D2            9
#define WIFI_SDIO_D3            8
#define WIFI_SDIO_RST           15

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

// Builds the settings screen (all LVGL code is in settings.cpp).
// Called once from ui.init() after M5.begin() and the LVGL display/touch driver
// (main.ino) are ready. It loads the stored settings, applies them and connects
// to the saved Wi-Fi network. Periodic work runs from an LVGL timer.
void settings();

// Returns to the home page. Called by the close (X) button of the settings screen.
// Uses the callback from settingsSetHomeCallback() if one is registered,
// otherwise a built-in placeholder home page is shown.
void settingsGoHome();

// Shows the settings screen again (the placeholder home page uses it).
void settingsOpen();

// Registers the real home page of the application (optional).
void settingsSetHomeCallback(void (*callback)());

// Registers a function that is called after the language was changed in
// Settings > Localization (the settings screen rebuilds itself first).
void settingsSetLanguageCallback(void (*callback)());

/* ---- Tab5 Home project integration (C++ only: settings_icons.c includes
        this header as C) ---- */
#ifdef __cplusplus
#include <lvgl.h>
#include <stddef.h>

// Returns the persistent Settings screen (called by ui.show(PAGE_SETTINGS)).
// The screen is NOT deleted when it is left.
lv_obj_t* settings_create();
lv_obj_t* settings_screen();
void      settings_hide_popups();        // closes the picker / Wi-Fi password dialog

// Wakes the screen from sleep (PIR motion, alarm).
// release_shield = true: the next touch is not swallowed.
void      settings_wake(bool release_shield);

// Wi-Fi is owned by this module (single owner)
bool      settings_wifi_connected();

// Settings > Display > Motion sensor (PIR) switch (stored in NVS)
bool      settings_motion_enabled();

// Settings > System > Blynk switch (stored in NVS, default on)
bool      settings_blynk_enabled();

// Settings > System > Touch sound: short click on every new touch
// (does nothing when switched off, at volume 0 or while the microphone is on)
void      settings_touch_click();

// Settings > Display > Temperature unit. Every value of the application is
// kept in Celsius; pages convert only for display.
bool        settings_fahrenheit();
bool        settings_light_theme();     // Display > Theme: true = light
float       settings_temp(float c);          // Celsius -> selected unit
float       settings_temp_delta(float dc);   // temperature difference (hysteresis, gap)
const char* settings_temp_unit();            // "°C" or "°F"

// Time / date text following the Localization settings (used by other pages)
bool      settings_time24();
bool      settings_format_time(char* out, size_t n);   // "21:50" or "09:50 PM" (AM / PM translated)
bool      settings_format_date(char* out, size_t n);   // DD/MM/YYYY, MM/DD/YYYY, ISO
#endif

#endif  // SETTINGS_H
