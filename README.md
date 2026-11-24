# ESP Home Control Panel

**A smart-home control panel for the M5Stack Tab5 (ESP32-P4, 5" 1280 × 720 touch screen), written in Arduino C++ with LVGL 9.**

It puts climate control, weather, blinds, RGB lighting, scenes, a two-group alarm system and indoor air quality on one touch screen. You can also control it remotely with the Blynk app. Optional wireless units add an outdoor temperature sensor and a fire / smoke / gas detector.

![Platform](https://img.shields.io/badge/platform-M5Stack%20Tab5%20(ESP32--P4)-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-00979D)
![LVGL](https://img.shields.io/badge/LVGL-9.2-orange)
![Languages](https://img.shields.io/badge/UI-6%20languages-green)
![Version](https://img.shields.io/badge/version-1.3.2-brightgreen)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

🇹🇷 [Türkçe açıklama: README.tr.md](README.tr.md)

![Screens](docs/overview.png)

*More in [Screenshots](#screenshots).*

> ### 🆕 What's new in v1.3.0
> - **Sirens with their own volume:** the alarm sirens no longer follow the system volume. Two tones: an intruder warble for **motion** and the "temporal-3" pattern of smoke alarms for **fire / smoke / gas**. Each tone has its own volume (0–100 %), on the device and in Blynk (V25 / V26).
> - **Siren on / off per alarm:** speaker buttons for motion, fire, smoke and gas on the Alarm page.
> - **Camera photo by e-mail:** on a motion alarm the Tab5 camera takes a photo and e-mails it to your address. The address and the camera switch are in the new **Alarm settings** window (gear on the Alarm page); the camera can also be switched in Blynk (V27).
> - **Home check from Blynk:** a push button (V28) takes a photo and e-mails it at once; V29 shows the result.
>
> Earlier: v1.2.0 landscape / portrait orientation, v1.1.0 clock styles. Full list: [Changelog](#changelog)

---

## Contents

- [Overview](#overview)
- [Screenshots](#screenshots)
- [Features](#features)
- [Hardware](#hardware)
- [Software requirements](#software-requirements)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Alarm system](#alarm-system)
- [Wireless units (UDP)](#wireless-units-udp)
- [Blynk setup](#blynk-setup)
- [Project structure](#project-structure)
- [Development](#development)
- [Troubleshooting](#troubleshooting)
- [Security and safety](#security-and-safety)
- [Changelog](#changelog)
- [Credits and license](#credits-and-license)

---

## Overview

```mermaid
flowchart LR
  subgraph Tab5["M5Stack Tab5"]
    UI["Touch UI (LVGL 9)"]
  end
  AHT["AHT10<br/>temp / humidity"] -- I2C --> Tab5
  ENS["ENS160<br/>air quality"] -- I2C --> Tab5
  PIR["PIR sensor"] --> Tab5
  Tab5 --> OUT["Relays · blinds · RGB strip<br/>HVAC LEDs / PWM"]
  OU["Outdoor unit<br/>(ESP + T/H sensor)"] -- "UDP 4210" --> Tab5
  AU["Alarm unit<br/>(ESP32-S2 + flame / MQ-2 / MQ-5)"] -- "UDP 4210" --> Tab5
  OWM["OpenWeatherMap"] -- HTTPS --> Tab5
  Tab5 <-- "Blynk IoT" --> APP["Blynk app"]
```

---

## Screenshots

*Rendered from the real LVGL code in a desktop simulator with demo weather data; all images are in [`docs/`](docs/).*

### Landscape


| | |
|---|---|
| **Home**<br>![Home](docs/01_home.png) | **Home (light theme)**<br>![Home (light theme)](docs/14_home_light.png) |
| **Clock styles**<br>![Clock styles](docs/02_clock_styles.png) | **Climate (HVAC)**<br>![Climate (HVAC)](docs/03_hvac.png) |
| **Weather**<br>![Weather](docs/04_weather.png) | **Blinds**<br>![Blinds](docs/05_blinds.png) |
| **RGB light**<br>![RGB light](docs/06_rgb.png) | **Scenes**<br>![Scenes](docs/07_scenes.png) |
| **Alarm**<br>![Alarm](docs/08_alarm.png) | **Air quality**<br>![Air quality](docs/09_air_quality.png) |
| **Settings – Display**<br>![Settings – Display](docs/10_settings.png) | **Settings – Wi-Fi**<br>![Settings – Wi-Fi](docs/11_settings_wifi.png) |
| **Settings – Localization**<br>![Settings – Localization](docs/12_settings_localization.png) | **Settings – System**<br>![Settings – System](docs/13_settings_system.png) |
| **Splash screen**<br>![Splash screen](docs/00_splash.png) |   |

### Portrait

*Settings → Display → Orientation → Portrait (720 × 1280). Images in [`docs/portrait/`](docs/portrait/).*

![Portrait pages](docs/overview_portrait.png)

| | | |
|---|---|---|
| **Home**<br>![Home](docs/portrait/01_home.png) | **Home (light theme)**<br>![Home (light theme)](docs/portrait/14_home_light.png) | **Clock styles**<br>![Clock styles](docs/portrait/02_clock_styles.png) |
| **Climate (HVAC)**<br>![Climate (HVAC)](docs/portrait/03_hvac.png) | **Weather**<br>![Weather](docs/portrait/04_weather.png) | **Blinds**<br>![Blinds](docs/portrait/05_blinds.png) |
| **RGB light**<br>![RGB light](docs/portrait/06_rgb.png) | **Scenes**<br>![Scenes](docs/portrait/07_scenes.png) | **Alarm**<br>![Alarm](docs/portrait/08_alarm.png) |
| **Air quality**<br>![Air quality](docs/portrait/09_air_quality.png) | **Settings – Display**<br>![Settings – Display](docs/portrait/10_settings.png) | **Settings – Wi-Fi**<br>![Settings – Wi-Fi](docs/portrait/11_settings_wifi.png) |
| **Settings – Localization**<br>![Settings – Localization](docs/portrait/12_settings_localization.png) | **Settings – System**<br>![Settings – System](docs/portrait/13_settings_system.png) | **Splash screen**<br>![Splash screen](docs/portrait/00_splash.png) |

---

## Features

| Area | Description |
|---|---|
| **Home** | Clock and date in one of 10 styles (5 digital, 5 analog): tap the clock to choose, the choice is saved. Indoor climate tile with a live HVAC animation and a *Quick* button. Outdoor temperature (one decimal) and humidity with a 3-day forecast. Tiles for blinds, RGB, air quality, scenes and alarm. |
| **HVAC** | Auto / Heat / Cool / Dry / Fan modes and a 3-speed + auto fan. Separate heating (10–30 °C) and cooling (18–30 °C) targets, plus a humidity target in Dry mode. Reads the AHT10 sensor. Stops on sensor error, over-temperature or 2 h continuous run. Monthly run time. |
| **Weather** | OpenWeatherMap: current weather, 24 h and 5-day forecast, details, air pollution, sunrise / sunset arc. |
| **Outdoor values** | From your own outdoor unit (UDP). Falls back to OpenWeatherMap after 1 min without data. |
| **Blinds** | Two motorised blinds: open / close / stop. Each blind is drawn as a window whose curtains move with the motor; drag on the window to set the position. |
| **RGB light** | PWM strip: colour wheel, brightness, presets, effects (breathe, rainbow, candle). |
| **Scenes** | Morning, Day, Away, Movie, Relax, Party, Night, All off. |
| **Alarm** | Two independent groups: **motion** (PIR) and **fire / smoke / gas** (UDP alarm unit). Each group has its own switch, and one PIN disarms everything. Two siren tones with their own volume, siren on / off per alarm, push notification through Blynk. |
| **Camera** | Photo by e-mail on a motion alarm or on request (Blynk / Alarm settings). Camera on / off and recipient address on the device. |
| **Air quality** | ENS160: AQI 1–5, eCO2, TVOC, 2-hour chart, temperature / humidity compensation from the AHT10. |
| **Blynk IoT** | HVAC power and target, both alarm groups, *disarm all*, every sensor value, status LEDs, notifications. |
| **Settings** | Display: brightness, sleep, orientation (landscape / portrait), rotation, dark / light theme, °C / °F, motion sensor. Wi-Fi: scan, connect, keyboard. Localization: language, time zone, time / date format. System: volume, microphone, touch sound, Blynk on/off, system information, restart. |
| **System information** | Uptime, program memory, RAM / PSRAM, chip, CPU load per core, clock speed, Wi-Fi signal / link / IP / MAC, battery, version, HVAC run time this month and last month. |
| **Top bar** | Clock, Wi-Fi signal, battery level with USB / charging state. On Home it also shows motion and Blynk status. |
| **Languages** | English, Türkçe, Deutsch, Français, Español, 中文. Every text on every page is translated. |

All settings and states are stored in the Tab5 flash (NVS) and restored at power-up. All outputs are switched off at boot.

---

## Hardware

### Main unit

| Part | Notes |
|---|---|
| **M5Stack Tab5** | ESP32-P4 + ESP32-C6 (Wi-Fi 6), 1280 × 720 touch screen, battery |
| **AHT10** | Indoor temperature / humidity, I2C 0x38 |
| **ENS160** | Air quality, I2C 0x53 (same bus as the AHT10) |
| **PIR sensor** | e.g. HC-SR501 / AM312 |
| **Relay board, H-bridge, MOSFETs** | Heating / cooling relays, blind motors, RGB strip |

| Function | GPIO | Defined in |
|---|---|---|
| AHT10 + ENS160 SDA / SCL | G7 / G6 | `hvac.h` |
| Heating / cooling relay | G17 / G45 | `hvac.h` |
| Heating / cooling LED | G52 / G5 | `hvac.h` |
| Fan PWM / Dry PWM | G19 / G18 | `hvac.h` |
| PIR motion sensor | G16 | `app_data.h` |
| RGB red / green / blue (PWM) | G2 / G48 / G47 | `app_data.h` |
| Blind 1 open / close | G3 / G4 | `app_data.h` |
| Blind 2 open / close | G35 / G51 | `app_data.h` |

> ⚠️ The GPIOs use 3.3 V logic. Drive motors, LED strips and mains devices only through relay boards, an H-bridge or MOSFETs.

### Optional wireless units

| Unit | Hardware | Sketch |
|---|---|---|
| Outdoor unit | Any ESP32 / ESP8266 with a temperature / humidity sensor | your own (packet format below) |
| Alarm unit | LOLIN ESP32-S2 mini + KY-026 flame, MQ-2 smoke, MQ-5 gas sensor | [`extras/alarm_unit/alarm_unit.ino`](extras/alarm_unit/alarm_unit.ino) |

---

## Software requirements

**Arduino IDE 2.x** and **esp32 by Espressif 3.3.11 or newer** (3.3.11 brings the `ESP_Video` camera library; with an older package set `CAM_ENABLE 0` in `mail_config.h`). Tested by compiling with 3.3.13, board *M5Tab5*.

| Board setting | Value |
|---|---|
| Board | M5Tab5 (or ESP32P4 Dev Module) |
| PSRAM | Enabled |
| Flash size | 16 MB |
| Partition scheme | app partition of **at least 4 MB** (the fonts for six languages need ~1 MB) |

| Library (Library Manager) | Version |
|---|---|
| M5Unified (+ M5GFX) | latest |
| **lvgl** | **9.2.x** (not 8.x) |
| ArduinoJson | 7.x |
| DFRobot_ENS160 | latest |
| Blynk (Volodymyr Shymanskyy) | 1.3+ |

> 💡 If another library offers to install its dependencies and that would replace LVGL 9 with 8.x, choose **"Install without dependencies"**.

---

## Quick start

1. Download or clone the repository. The sketch folder must have the same name as the `.ino` file (e.g. `main/main.ino`).
2. Copy **`lv_conf.h`** to `Documents/Arduino/libraries/`, next to the `lvgl` folder.
3. Enter your settings (see [Configuration](#configuration)).
4. Select the board settings above, then compile and upload.
5. On the Tab5 open **Settings → Wi-Fi** to choose a network if you did not set a default one.

---

## Configuration

| File | Settings |
|---|---|
| `app_data.h` | `DEF_OWM_API_KEY` (free key from [openweathermap.org](https://openweathermap.org/api)), `DEF_CITY`, `DEF_LATITUDE`, `DEF_LONGITUDE`, `DEF_ALARM_PIN` (default `1234`), optional `DEF_WIFI_SSID` / `DEF_WIFI_PASS`, pins |
| `hvac.h` | HVAC and sensor pins, relay / LED active level |
| `blynk.h` | `BLYNK_TEMPLATE_ID`, `BLYNK_TEMPLATE_NAME`, `BLYNK_AUTH_TOKEN` (Blynk stays off while the token is a placeholder) |
| `udp_th.h` | Static IP of the Tab5, outdoor unit id / port, outdoor value source |
| `udp_alarm.h` | Alarm unit id / port, offline time |
| `mail_config.h` | Sender e-mail account for the camera photos (SMTP server, user, app password), camera rotation, JPEG quality, `CAM_ENABLE` |

---

## Alarm system

The Alarm page has two **independent** groups:

| Group | Triggered by | Switch on the Tab5 | Blynk |
|---|---|---|---|
| **Motion alarm** | PIR sensor | Alarm page | V11 |
| **Fire, smoke and gas** | alarm unit (`fire`, `smoke`, `gas`) | Alarm page | V18 |

- You can keep fire / smoke / gas armed while you are at home and the motion alarm is off.
- Switching a group **on** needs no PIN.
- Switching alarms **off** needs the PIN on the keypad. One PIN entry disarms **all** groups and stops the siren. In Blynk, **V19 – Disarm all** does the same.
- When an alarm goes off:
  - the Tab5 opens the Alarm page, blinks red and sounds the siren of that alarm (motion: two-tone warble, fire / smoke / gas: 3 beeps and a pause);
  - the page names the cause (e.g. *Smoke alarm!*) and shows the live state of every detector;
  - Blynk receives a push notification with the cause.
- **Sirens:** the speaker buttons under each group switch the siren of motion, fire, smoke or gas on or off. The volumes of the two tones are set in the **Alarm settings** window (gear on the Alarm page, *Test* plays the tone) or with Blynk V25 / V26. They do not depend on the system volume in Settings.
- **Camera:** see [Camera and e-mail](#camera-and-e-mail).
- Other sensors can raise an alarm from your own code:

```cpp
#include "ui.h"
ui_alarm_trigger("Front door");   // opens the Alarm page, starts the siren, notifies Blynk
```

---

## Camera and e-mail

![Alarm settings](docs/16_alarm_settings.png)

1. **Sender account** in `mail_config.h` (use a separate account for the Tab5):
   - Gmail: turn on 2-step verification, create an **app password** (Google account → Security → App passwords) and enter the address and the 16-character app password as `MAIL_SMTP_USER` / `MAIL_SMTP_PASSWORD`.
   - Other providers: any SMTP server with SSL/TLS on **port 465** (STARTTLS on 587 is not supported).
2. **Recipient:** Alarm page → gear → *E-mail address* → *Change*. The address is stored on the Tab5.
3. **Camera on / off:** same window, or Blynk V27. Off = no photos at all.
4. **Test:** *Send photo now* (or Blynk V28). The status line shows *Photo sent 14:03* or the error (e.g. the reply of the mail server).

What happens:
- **Motion alarm** with the camera on: one photo, e-mailed with the subject *[Tab5] Alarm photo: Motion detected*.
- **Blynk V28 / Send photo now:** subject *[Tab5] Home check*.
- The photo is 1280 × 720 JPEG (about 100–200 KB). The camera, the JPEG encoder and the e-mail run in their own task; the screen keeps working.
- While the camera takes the photo (about 2 s) the touch screen pauses, because the camera shares the internal I2C bus with the touch controller.
- **Photo rotation:** the camera turns with the Tab5. In the Alarm settings window, *Photo rotation* (0° / 90° / 180° / 270°) is saved separately for landscape and portrait; choose the one that makes the photo upright and check it with *Send photo now*. *Rotation 180°* in Settings is added automatically at the moment of the photo, so all four screen positions come out upright. Defaults: `CAM_ROTATION_LANDSCAPE` / `CAM_ROTATION_PORTRAIT` in `mail_config.h`.
- Without a camera (older esp32 package): `CAM_ENABLE 0`.

> 🔒 Keep `mail_config.h` private (do not publish your app password). The connection is encrypted; the server certificate is checked only when `MAIL_ROOT_CA` is set.

---

## Wireless units (UDP)

Both units send small JSON packets to the Tab5 on UDP port **4210**. The Tab5 uses the static address **192.168.1.50** on every network (`UDP_USE_STATIC_IP 1` in `udp_th.h`). Adapt `UDP_LOCAL_IP`, `UDP_GATEWAY`, `UDP_SUBNET` and `UDP_DNS` to your network, or set `UDP_USE_STATIC_IP 0` for DHCP.

### Outdoor unit

```json
{"id":"s2mini","seq":12,"t":23.45,"h":48.20}
```

- Only packets with `UDP_EXPECTED_ID` are accepted.
- Home shows the outdoor unit values with a green *Outdoor unit* label.
- After **1 minute** without a packet, Home shows OpenWeatherMap values with an orange label. It switches back as soon as a packet arrives. See `UDP_STALE_MS`, `OUTDOOR_USE_UDP` and `OUTDOOR_FALLBACK_OWM`.
- Your code can read both sources at any time: `g_outdoor.udp_temp / udp_hum` and `g_outdoor.owm_temp / owm_hum`.

### Fire / smoke / gas alarm unit

```json
{"id":"s2mini_alarm","seq":7,"fire":0,"smoke":1,"gas":0}
```

- Only packets with `UDP_ALARM_ID` (`"s2mini_alarm"`) are accepted. Values can be `0 / 1` or `false / true`.
- The unit sends a packet on every change and every 10 s as a heartbeat. If no packet arrives for 90 s, the Alarm page shows *Alarm unit not responding*.
- The ready-made sketch [`extras/alarm_unit/alarm_unit.ino`](extras/alarm_unit/alarm_unit.ino) contains wiring, warm-up, debouncing and calibration notes. Open it as a separate sketch for the LOLIN S2 mini and enter your Wi-Fi details and the Tab5 IP at the top.

| Sensor | Supply | Output → ESP32-S2 mini |
|---|---|---|
| KY-026 flame | 3V3 | DO → GPIO 7 |
| MQ-2 smoke | 5 V | AO → divider (10 kΩ / 20 kΩ) → GPIO 3 |
| MQ-5 gas | 5 V | AO → divider (10 kΩ / 20 kΩ) → GPIO 5 |

> ⚠️ The MQ modules output up to 5 V. Always use the voltage divider, and never connect their DO pins directly to the ESP32-S2.

---

## Blynk setup

In [Blynk.Console](https://blynk.cloud) create a template with the datastreams below. Then copy the template id, the template name and the device auth token into `blynk.h`.

**Climate**

| Pin | Name | Type | Range | Direction | Widget |
|---|---|---|---|---|---|
| V0 | HVAC power | Integer | 0–1 | app ↔ device | Switch |
| V1 | Target down | Integer | 0–1 | app → device | Button (push) |
| V2 | Target up | Integer | 0–1 | app → device | Button (push) |
| V3 | HVAC target | Double | 10–30 °C | device → app | Value / Gauge |
| V4 / V5 | Indoor temperature / humidity | Double | °C / % | device → app | Value / Gauge |
| V6 / V7 | Outdoor temperature / humidity | Double | °C / % | device → app | Value / Gauge |
| V8 / V9 / V10 | HVAC on / heating / cooling LED | Integer | 0–255 | device → app | LED |

**Alarm**

| Pin | Name | Type | Range | Direction | Widget |
|---|---|---|---|---|---|
| V11 | Motion alarm on | Integer | 0–1 | app ↔ device | Switch |
| V12 | Motion alarm armed LED | Integer | 0–255 | device → app | LED |
| V13 | Alarm ringing LED | Integer | 0–255 | device → app | LED |
| V18 | Fire / smoke / gas alarm on | Integer | 0–1 | app ↔ device | Switch |
| V19 | Disarm all | Integer | 0–1 | app → device | Button (push) |
| V20 / V21 / V22 | Fire / smoke / gas LED | Integer | 0–255 | device → app | LED |
| V23 | Fire / smoke / gas armed LED | Integer | 0–255 | device → app | LED |
| V24 | Alarm text | String | – | device → app | Label |
| V25 | Siren volume: motion | Integer | 0–100 % | app ↔ device | Slider |
| V26 | Siren volume: fire / smoke / gas | Integer | 0–100 % | app ↔ device | Slider |

**Camera**

| Pin | Name | Type | Range | Direction | Widget |
|---|---|---|---|---|---|
| V27 | Camera on | Integer | 0–1 | app ↔ device | Switch |
| V28 | Take photo | Integer | 0–1 | app → device | Button (push) |
| V29 | Camera status | String | – | device → app | Label |

**Air quality**

| Pin | Name | Type | Range | Direction | Widget |
|---|---|---|---|---|---|
| V14 | Air quality index | Integer | 1–5 | device → app | Value / Gauge |
| V15 | eCO2 | Integer | 400–5000 ppm | device → app | Value / Chart |
| V16 | TVOC | Integer | 0–3000 ppb | device → app | Value / Chart |
| V17 | Air quality text | String | – | device → app | Label |

**Notification:** under *Events & Notifications*, add an event with the code **`alarm`** and enable push notifications. Each alarm sends one notification with its cause.

Notes:
- Temperatures are always sent in °C.
- Changes made in the app are saved on the Tab5 exactly like changes made on the screen.
- Blynk can be switched off in **Settings → System**.

---

## Project structure

| File | Purpose |
|---|---|
| `main.ino` | Display / touch driver, global `AppData g_data`, calls `ui.init()` |
| `ui.h / ui.cpp` | Page switching, data storage, time, PIR, alarm logic, battery state |
| `menu.*` · `splash.*` | Common top bar · start-up screen |
| `home.*` `clock.*` `weather.*` `hvac.*` `lights.*` `rgb.*` `scene.*` `alarm.*` `aq.*` `settings.*` | One file pair per page; all LVGL code of a page lives in its `.cpp` |
| `blynk.*` | Blynk IoT link (own FreeRTOS task, no LVGL code) |
| `udp_th.*` | Outdoor unit receiver and outdoor value selection |
| `udp_alarm.*` | Fire / smoke / gas alarm unit receiver |
| `health.*` | Main loop watchdog, reset reason, health log |
| `alarm_sound.*` | Siren tones, their volumes and the siren switches (own speaker channel) |
| `camera_mail.*` · `mail_config.h` | Camera photo (ESP_Video, hardware JPEG) and e-mail (SMTP) in an own task · sender account |
| `sysinfo.*` | CPU load, monthly HVAC run time |
| `theme.*` | Dark / light theme (`ui_col()`) |
| `lang.*` · `fonts_ui.c` · `*_icons.c` | Translations, fonts and icons (generated) |
| `app_data.h` · `lv_conf.h` | Global data structure, defaults, pins · LVGL configuration |
| `tools/` | Python generators for translations, fonts and icons |
| `extras/alarm_unit/` | Sketch for the fire / smoke / gas alarm unit (not compiled with the panel) |
| `docs/` | Screenshots of every page, landscape and portrait (`docs/portrait/`), used by this README |
| `CHANGELOG.md` | Version history |

---

## Development

**Add or change texts**

1. Edit `tools/strings.py`. Each text is one line containing all six languages.
2. Run from the sketch folder:
   ```bash
   python3 tools/make_lang.py
   python3 tools/make_fonts.py
   ```
   This needs Python 3 with **Pillow** and **fontTools**, plus the **Inter** and **Noto Sans CJK** font files (paths at the top of `make_fonts.py`).

**Rules**
- Each page keeps its LVGL code in its own `.cpp`.
- Background modules (`blynk`, `udp_*`, `sysinfo`) contain no LVGL calls.
- Everything that touches the UI or the HVAC runs in the LVGL thread.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Hundreds of errors such as `lv_button_create was not declared` | LVGL 8 is installed. Install **lvgl 9.2.x** and keep only one `lvgl` folder. |
| Sketch too big | Choose a partition scheme with ≥ 4 MB app space. |
| No weather data | Check the API key in `app_data.h` (new keys can take up to 2 h to activate) and the status line on the Weather page. |
| No internet with the static IP | Your router uses another address range. Adapt `udp_th.h` or set `UDP_USE_STATIC_IP 0`. |
| No outdoor values / alarm unit offline | Check the sender's target IP, port and `id`. Set `UDP_DEBUG 1` to print every packet on the serial monitor. |
| False smoke / gas alarms | Let the MQ sensors burn in (24–48 h), then raise `MQ2_ALARM_MV` / `MQ5_ALARM_MV` in the alarm unit sketch. |
| Tab5 froze / restarted by itself | Settings → System → Information → *Last start* shows the reason (watchdog, crash, low voltage). The serial monitor shows `[SYS]` lines every 10 min; send the lines before the freeze. |
| Blynk icon stays grey | The token in `blynk.h` is not set, or Blynk is switched off in Settings → System. |
| `ESP_Video.h: No such file or directory` | Update the esp32 package to 3.3.11 or newer, or set `CAM_ENABLE 0` in `mail_config.h`. |
| *Sender account not set* | Enter `MAIL_SMTP_USER` / `MAIL_SMTP_PASSWORD` in `mail_config.h`. |
| *E-mail could not be sent* + `password: 535 ...` | Wrong user / password. Gmail needs an app password, not the normal password. |
| *Camera error* + `sensor not found` | The serial monitor shows `[CAM] ...` lines; check the `I2C port ... bus shared` line at start-up. |
| Photo turned or upside down | Alarm page → gear → *Photo rotation* (for the current orientation). |

---

## Security and safety

- **Do not commit real credentials.** Keep placeholders in `app_data.h` (Wi-Fi, OpenWeatherMap key), `blynk.h` (auth token) and `extras/alarm_unit/alarm_unit.ino` (Wi-Fi). If a key was ever published, regenerate it.
- Change the default alarm PIN `1234`.
- This project switches relays, motors and heating / cooling devices. You build and use the hardware at your own risk. Mains-voltage work must be done by a qualified person.
- The flame / smoke / gas sensors are hobby modules. They **do not replace certified smoke and gas detectors**.

---

## Changelog

The version is shown on the Settings → System page and in the system information window (`SETTINGS_VERSION` in `settings.h`). Details: [CHANGELOG.md](CHANGELOG.md).

### v1.3.2 — 2026-11-24

**Fix**
- Main loop freeze (watchdog restart) caused by the AHT10 library: it waited endlessly for the sensor after a failed I2C read. Own sensor driver with time limits; a glitch now only counts as a sensor error.
- The screen no longer waits for the AHT10 measurement; the Adafruit AHTX0 libraries are no longer needed.
- If the main loop ever stops again, the part where it stopped is shown after the restart in Settings → System → Information (*Last start*) and on the serial monitor.
- New *Last fault* row (reason, stage, date) with a *Clear* button for the counter; a restart after a program upload is shown as *USB reset*.

### v1.3.1 — 2026-11-22

**Stability**
- Main loop watchdog: after 30 s of standstill the Tab5 restarts by itself instead of staying frozen.
- Reset reason in Settings → System → Information (*Last start*) and on the serial monitor, with a counter of unexpected restarts.
- Health line every 10 minutes on the serial monitor (`[SYS] ...`: uptime, memory, Wi-Fi, Blynk).
- Wi-Fi reconnect watchdog: a new attempt every 60 s while the saved network is not connected.
- Wi-Fi signal strength read at most every 5 s (each request can block the screen when the Wi-Fi chip does not answer).
- A stuck camera can no longer block touch and sound.
- Also included: photo rotation, camera restart and microphone / speaker fixes made after v1.3.0.


**New**
- **Alarm sirens with their own volume** (`alarm_sound.cpp`):
  - Two tones: motion = two-tone warble (1400 / 2100 Hz), fire / smoke / gas = 3100 Hz "temporal-3" (three beeps, pause).
  - Each tone has its own volume 0–100 % (Alarm settings window, Blynk V25 / V26). The siren plays on its own speaker channel, so the system volume does not change it.
  - When fire / smoke / gas and motion ring together, the fire tone plays.
- **Siren on / off per alarm:** speaker buttons for motion, fire, smoke and gas on the two alarm cards of the Alarm page.
- **Alarm settings window** (gear on the Alarm page): siren volumes with *Test*, camera on / off, e-mail address (keyboard), *Send photo now* and the camera status.
- **Camera photo by e-mail** (`camera_mail.cpp`, `mail_config.h`):
  - The photo is turned upright for landscape / portrait and 0° / 180° (*Photo rotation* in the Alarm settings window).
  - Motion alarm with the camera on: a photo is taken and e-mailed to the saved address.
  - Blynk V28 and *Send photo now*: photo of the home on request.
  - Camera SC2356 over MIPI-CSI (ESP_Video library), 1280 × 720, hardware JPEG; SMTP over SSL (port 465) in its own task.
  - The recipient is stored on the Tab5, the sender account is set in `mail_config.h`.
- **Blynk:** V25 / V26 siren volumes (slider), V27 camera on / off, V28 take photo (push), V29 camera status.
- Translations for all new texts in 6 languages.

**Changed**
- The system volume (Settings → System) now only sets the system sounds (touch click, volume test).
- The internal I2C bus is shared with the camera: touch, battery and audio set-up pause while a photo is taken (about 2 s).
- Requires the esp32 boards package **3.3.11** or newer (`CAM_ENABLE 0` in `mail_config.h` for older packages). Compiled with 3.3.13.
- Software version **1.3.0**.

**Upgrade notes**
- No setting is reset. Default volumes: motion 80 %, fire / smoke / gas 100 %; every siren is on; the camera is on but sends nothing until an e-mail address is saved.
- New files: `alarm_sound.*`, `camera_mail.*`, `mail_config.h`. Add V25–V29 to your Blynk template.

### v1.2.0 — 2026-11-19

**New**
- **Orientation setting:** *Settings → Display → Orientation* (above *Rotation*) with **Landscape** and **Portrait**.
  - Landscape is 1280 × 720 as before; portrait is 720 × 1280.
  - The choice is saved in NVS (namespace `settings`, key `portrait`). The device saves its data, switches every output off and restarts in the new orientation.
  - *Rotation* (0° / 180°) works in both orientations (landscape: M5GFX rotation 1 / 3, portrait: 0 / 2).
- **Portrait layout for every page:**
  - **Home:** clock + indoor tile, weather with a 5-day forecast, blinds + RGB, air quality + scenes + alarm.
  - **Climate:** Quick / Power in their own row, the Auto targets under each other, the warning banner at the bottom of the card.
  - **Weather, Blinds, RGB light, Alarm, Air quality:** the cards under each other at full width.
  - **Scenes:** 2 × 4 cards. **Clock styles:** 5 × 2 cards with the name next to the preview.
  - **Settings:** the 4 menu cards in a row under the top bar, the pages below them. Pop-up windows and the keyboard fit the narrow screen.
  - **Top bar:** clock right-aligned next to the Wi-Fi icon; on Home the motion indicator shows only its icon.
- Portrait screenshots in [`docs/portrait/`](docs/portrait/).

**Changed**
- `main.ino` reads the saved orientation before the LVGL display is created.
- The layouts use `ui_portrait()`, `ui_scr_w()`, `ui_scr_h()` and `UI_LP(landscape, portrait)` from `theme.h`.
- New translations (orientation, landscape, portrait, restart message) in all 6 languages; fonts regenerated.
- Software version **1.2.0**.

**Upgrade notes**
- No setting is reset. The device starts in landscape until portrait is selected.
- If the picture is upside down in portrait, select *Rotation → 180°*.

### v1.1.0 — 2026-11-18

**New**
- **Clock styles** (`clock.cpp`): tap the Home clock to open a page with 10 styles, each shown live.
  - Digital: *Classic*, *Neon*, *Flip cards*, *LCD* (7-segment) and *Stacked*.
  - Analog: *Classic*, *Minimal*, *Roman*, *Sport* (seconds ring) and *Hybrid* (dial + digital time).
  - The selected style is saved in NVS (namespace `clock`) and kept after a restart.
  - Every style follows the 12 / 24 h and date format settings.
- **Screenshots** of every page in [`docs/`](docs/) and a gallery in this README.

**Changed**
- **Blinds page:** the position slider is replaced by a window animation.
  - The curtains follow the real motor position.
  - Dragging on the window (away from the centre = more open) sets a new target, shown by two white marks.
- **RGB light page:** the three R / G / B sliders are replaced by one colour wheel.
  - Angle = colour, distance from the centre = saturation.
  - A marker, a colour sample and the hex code show the selected colour.
  - Presets, effects and Blynk work as before.
- **Climate (HVAC) page:** the main card now runs from y = 108 to y = 700 like the cards on the other pages, and every element was re-positioned with even spacing.
- Software version **1.1.0**.

**Fixed**
- **Settings start-up crash on LVGL 9.2:** the dark / light theme buttons were styled before they were created. Newer LVGL versions ignored this silently.

**Upgrade notes**
- No setting is reset. The clock style uses its own NVS namespace.
- New files: `clock.cpp` and `clock.h`. The translations and fonts were regenerated.

### v1.0.0

First public version.
- **Home dashboard:** clock, indoor climate, weather, blinds, RGB light, air quality, scenes and alarm.
- **Climate (HVAC):** Auto / Heat / Cool / Dry / Fan modes, Quick mode, sensor and over-temperature safety, monthly run time.
- **Weather:** OpenWeatherMap current values, 24-hour chart, 5-day forecast, sunrise and sunset.
- **Outdoor unit (UDP):** decimal outdoor temperature; switches to OpenWeatherMap 1 minute after the last packet.
- **Blinds and RGB:** two motorised blinds; RGB light with presets and effects (breathe, rainbow, candle).
- **Scenes and alarm:**
  - Scenes for common situations.
  - Two alarm groups (motion, fire / smoke / gas); one PIN disarms all.
  - Alarm unit sketch for an ESP32-S2 mini.
- **Blynk:** switches, alarm control, LEDs and notifications, air quality values.
- **Settings:**
  - Brightness, screen sleep, rotation, dark / light theme.
  - Wi-Fi with a static IP on every network.
  - 6 languages, time zone, time and date format.
  - Volume, microphone, touch sound, Blynk on / off, restart.
  - System information window.
- **Other:** splash screen, battery and Wi-Fi status in the top bar.

---

## Credits and license

- [LVGL](https://lvgl.io) · [M5Unified / M5GFX](https://github.com/m5stack/M5Unified) · [OpenWeatherMap](https://openweathermap.org) · [Blynk IoT](https://blynk.io)
- Fonts: [Inter](https://rsms.me/inter/) and [Noto Sans CJK](https://github.com/notofonts/noto-cjk) (SIL Open Font License 1.1), Montserrat (built into LVGL)

This project is released under the **MIT License**; see [LICENSE](LICENSE). You may use, copy, modify and distribute it, also commercially, as long as the copyright notice is kept. It is provided without warranty. The libraries and fonts keep their own licenses.

---

**Created by Selcuk**
