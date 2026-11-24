// ============================================================================
//  blynk.h  -  Blynk IoT connection (control functions only, no LVGL code)
//
//  From the Blynk app:   HVAC on / off, target temperature - / +, alarm arm / disarm,
//                        siren volumes, camera on / off, photo by e-mail
//  To the Blynk app:     indoor / outdoor temperature and humidity, HVAC target,
//                        power / heating / cooling LEDs, alarm armed / alarm LEDs,
//                        alarm notification (event), air quality (ENS160)
//  Changes made in the app are saved to the Tab5 NVS by the HVAC module,
//  exactly like changes made on the screen.
//
//  Library: "Blynk" by Volodymyr Shymanskyy (Arduino Library Manager, 1.3+)
//
//  ---- Datastreams to create in the Blynk template (Blynk.Console) ----------
//   Pin  Name              Type     Min  Max   Unit  Widget (app)
//   V0   HVAC power        Integer  0    1           Switch
//   V1   Target down       Integer  0    1           Button (push)
//   V2   Target up         Integer  0    1           Button (push)
//   V3   HVAC target       Double   10   30    °C    Value display / Gauge
//   V4   Indoor temp.      Double   -10  50    °C    Value display / Gauge
//   V5   Indoor humidity   Double   0    100   %     Value display / Gauge
//   V6   Outdoor temp.     Double   -40  60    °C    Value display / Gauge
//   V7   Outdoor humidity  Double   0    100   %     Value display / Gauge
//   V8   HVAC on LED       Integer  0    255         LED
//   V9   Heating LED       Integer  0    255         LED
//   V10  Cooling LED       Integer  0    255         LED
//   V11  Alarm armed       Integer  0    1           Switch (arm / disarm, no PIN)
//   V12  Alarm armed LED   Integer  0    255         LED
//   V13  Alarm LED         Integer  0    255         LED (alarm ringing)
//   V14  Air quality index Integer  1    5           Value display / Gauge (1 = excellent)
//   V15  eCO2              Integer  400  5000  ppm   Value display / Gauge / Chart
//   V16  TVOC              Integer  0    3000  ppb   Value display / Gauge / Chart
//   V17  Air quality text  String                    Label ("Good", "Warming up" ...)
//   V18  Fire/smoke/gas on Integer  0    1           Switch (alarm unit group on / off)
//   V19  Disarm all        Integer  0    1           Button (push): every alarm off, siren off
//   V20  Fire LED          Integer  0    255         LED
//   V21  Smoke LED         Integer  0    255         LED
//   V22  Gas LED           Integer  0    255         LED
//   V23  Fire/smoke/gas armed LED  Integer  0  255   LED
//   V24  Alarm text        String                    Label (reason / detections / "No alarm")
//   V25  Siren motion      Integer  0    100   %     Slider (motion alarm tone volume, 0 = silent)
//   V26  Siren fire/smoke/gas Integer 0  100   %     Slider (fire / smoke / gas tone volume)
//   V27  Camera on         Integer  0    1           Switch (photos allowed / not allowed)
//   V28  Take photo        Integer  0    1           Button (push): photo of the home by e-mail
//   V29  Camera status     String                    Label ("Photo sent 14:03", errors ...)
//  V11 switches only the motion alarm; it does not silence a ringing alarm
//  (use V19 "Disarm all").
//
//  ---- Event (Blynk.Console > Template > Events & Notifications) -----------
//   Event code "alarm" (BLYNK_ALARM_EVENT), type Critical / Warning, with
//   "Notifications" (push) enabled. Every alarm sends it with the reason,
//   e.g. "ALARM: Motion detected".
//  Temperatures are always sent in Celsius (one decimal).
// ============================================================================
#pragma once
#include <stdint.h>

// ---- Credentials: copy them from Blynk.Console > Device > Device info -----
#define BLYNK_TEMPLATE_ID     "TMPLxxxxxxxxx"
#define BLYNK_TEMPLATE_NAME   "Tab5 Home"
#define BLYNK_AUTH_TOKEN      "YOUR_BLYNK_AUTH_TOKEN"

// ---- Virtual pins ----------------------------------------------------------
#define VPIN_HVAC_POWER       0     // app <-> device
#define VPIN_TARGET_DOWN      1     // app  -> device
#define VPIN_TARGET_UP        2     // app  -> device
#define VPIN_HVAC_TARGET      3     // device -> app
#define VPIN_INDOOR_TEMP      4
#define VPIN_INDOOR_HUM       5
#define VPIN_OUTDOOR_TEMP     6
#define VPIN_OUTDOOR_HUM      7
#define VPIN_LED_POWER        8
#define VPIN_LED_HEATING      9
#define VPIN_LED_COOLING      10
#define VPIN_ALARM_ARM        11    // app <-> device
#define VPIN_LED_ALARM_ARMED  12    // device -> app
#define VPIN_LED_ALARM        13    // device -> app
#define VPIN_AQ_INDEX         14    // device -> app (ENS160)
#define VPIN_AQ_ECO2          15
#define VPIN_AQ_TVOC          16
#define VPIN_AQ_TEXT          17
#define VPIN_SAFETY_ARM       18    // app <-> device: fire / smoke / gas alarm on / off
#define VPIN_DISARM_ALL       19    // app  -> device: every alarm off
#define VPIN_LED_FIRE         20    // device -> app
#define VPIN_LED_SMOKE        21
#define VPIN_LED_GAS          22
#define VPIN_LED_SAFETY_ARMED 23
#define VPIN_ALARM_TEXT       24
#define VPIN_SIREN_MOTION     25    // app <-> device: 0..100 %
#define VPIN_SIREN_SAFETY     26    // app <-> device: 0..100 %
#define VPIN_CAMERA_ON        27    // app <-> device
#define VPIN_TAKE_PHOTO       28    // app  -> device (push button)
#define VPIN_CAMERA_TEXT      29    // device -> app

#define BLYNK_ALARM_EVENT     "alarm"   // event code in the Blynk template

#define BLYNK_TARGET_STEP     0.5f  // °C per - / + press

// Starts the Blynk background task (ui.init(), after hvac_control() and
// settings()). Does nothing while BLYNK_AUTH_TOKEN is the placeholder.
void blynk_begin();

// Main loop side (called every 100 ms by ui.cpp, LVGL thread): applies the
// commands received from the app and refreshes the values to be sent.
void blynk_process();

bool blynk_connected();

// Status for the Home top bar
enum BlynkLinkState {
  BLYNK_LINK_OFF,        // switched off in Settings > System, or no auth token
  BLYNK_LINK_OFFLINE,    // enabled, not connected (no Wi-Fi / server unreachable)
  BLYNK_LINK_ONLINE      // connected to the Blynk cloud
};
BlynkLinkState blynk_state();
