# Changelog

All notable changes to **ESP Home Control Panel** (M5Stack Tab5) are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).
The version shown on the device is `SETTINGS_VERSION` in `settings.h`.

🇹🇷 Turkish version: [README.tr.md → Sürüm geçmişi](README.tr.md#sürüm-geçmişi)

## [1.1.0] - 2026-11-18

### Added
- **Clock styles** (`clock.cpp`, `clock.h`). A tap on the Home clock opens a page with 10 live previews:
  - Digital: Classic, Neon (glow and seconds bar), Flip cards, LCD (7-segment, blinking colon), Stacked (hours above minutes).
  - Analog (`lv_scale` with line needles): Classic (1–12), Minimal (dots), Roman (cream dial, I–XII), Sport (seconds ring), Hybrid (small dial with digital time and date).
  - A tap selects the style, saves it and returns to Home.
  - The style is stored in NVS (namespace `clock`, key `theme`).
  - Every style follows the 12 / 24 h and date format settings and the dark / light theme.
- **Screenshots** of every page (dark and light Home, all four Settings tabs) in `docs/`. The README has an overview image and a gallery.
- Translations for the clock style names in all 6 UI languages.

### Changed
- **Blinds page:** the position slider is replaced by a window animation.
  - Each blind is drawn as a window (sky, sun, hills, frame) with two folded curtains on a rod.
  - The curtains follow the real motor position and are animated every 100 ms.
  - Dragging on the window sets a new target: the farther from the centre, the more open. The target is shown by two white marks until it is reached.
  - The Open / Stop / Close buttons are unchanged.
- **RGB light page:** the three R / G / B sliders are replaced by one colour wheel.
  - Angle = hue, distance from the centre = saturation (the centre is white).
  - The wheel is drawn once into PSRAM.
  - A ring marker, a colour sample and the hex code show the selected colour.
  - Brightness, presets, effects and Blynk control work as before.
- **Climate (HVAC) page:** the main card now uses the same area as the cards on the other pages (y = 108 … 700, was 130 … 697).
  - The header, targets, measurement line, mode buttons and fan selector were re-positioned with even spacing.
  - All positions are constants at the top of `hvac.cpp` (`HEAD_Y`, `STATUS_Y`, `BLK_Y`, `NOW_Y`, `MODE_Y`, `FAN_Y`).
- Software version raised to **1.1.0**.

### Fixed
- **Crash at start-up with LVGL 9.2:** `updateSegments()` in `settings.cpp` styled the dark / light theme buttons before they were created. `styleSegment()` now ignores buttons that do not exist yet.

### Upgrade notes
- No saved setting is reset; the clock style uses its own NVS namespace.
- Copy the whole project folder: `clock.cpp` / `clock.h` are new and `lang.cpp` / `fonts_ui.c` were regenerated.

## [1.0.0]

First public version.

### Added
- **Home dashboard:** clock, indoor climate, weather, blinds, RGB light, air quality, scenes and alarm tiles. The top bar shows Blynk, motion, Wi-Fi and battery status.
- **Climate (HVAC):** Auto / Heat / Cool / Dry / Fan modes.
  - Quick mode, fan speed selection and heating / cooling animations.
  - Sensor error, lockout and over-temperature safety.
  - Information window with the HVAC run time this month and last month.
- **Weather:** OpenWeatherMap current values, 24-hour chart, 5-day forecast, sunrise / sunset and details.
- **Outdoor unit (UDP 4210):** outdoor temperature shown with one decimal. The panel falls back to OpenWeatherMap 1 minute after the last packet and switches back as soon as a packet arrives.
- **Blinds:** two motorised blinds with open / stop / close, all-blinds control and an adjustable motor travel time.
- **RGB light:** PWM strip with brightness, presets and effects (breathe, rainbow, candle).
- **Scenes** for common situations.
- **Alarm system:**
  - Two groups: motion (PIR) and fire / smoke / gas (wireless alarm unit `s2mini_alarm`).
  - One PIN disarms all groups.
  - Siren on the speaker.
  - Sketch for the alarm unit (`extras/alarm_unit/`, ESP32-S2 mini with KY-026, MQ-2, MQ-5).
- **Air quality:** ENS160 eCO2, TVOC and AQI with history; the values are sent to Blynk.
- **Blynk IoT:** switches, alarm control, alarm LEDs and notifications (V18–V24), air quality datastreams. Blynk can be turned on or off on the device.
- **Settings:**
  - Display: brightness, screen sleep, rotation, dark / light theme.
  - Wi-Fi: scan and connect; the static IP is kept on every network.
  - Localization: 6 languages, time zone, 12 / 24 h and date format.
  - System: volume, microphone, touch sound, Blynk on / off, system information, restart.
- **Other:** splash screen ("ESP HOME CONTROL PANEL", "Created by Selcuk"); README in English and Turkish; MIT license.

[1.1.0]: #110---2026-11-18
[1.0.0]: #100
