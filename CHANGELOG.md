# Changelog

All notable changes to **ESP Home Control Panel** (M5Stack Tab5) are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).
The version shown on the device is `SETTINGS_VERSION` in `settings.h`.

🇹🇷 Turkish version: [README.tr.md → Sürüm geçmişi](README.tr.md#sürüm-geçmişi)

## [1.3.2] - 2026-11-24

Fix of a main loop watchdog restart ("watchdog: the loop had frozen") after a night of operation.

### Fixed
- **Main loop freeze by the AHT10 sensor:** the Adafruit AHTX0 library waits for the sensor's *busy* bit with an endless loop. A failed I2C read returns `0xFF` (busy bit set), so a single glitch on the Port A cable (interference, loose contact, sensor reset) froze the main loop until the watchdog restarted the panel. The sensor now has its own driver in `hvac.cpp` where every wait has a limit; a failed reading only counts as a sensor error (shown on the HVAC page after 3 failures, new start attempt every 10 s).

### Changed
- The AHT10 measurement runs in two steps (start, then read 100 ms later), the screen no longer waits ~80 ms every 2 s for the sensor.
- The **Adafruit AHTX0 / BusIO / Unified Sensor** libraries are no longer needed.
- Software version **1.3.2**.

### Added
- **Stage marker for the watchdog:** the main loop notes which part runs (touch, display, HVAC sensor, air quality sensor, Wi-Fi, Blynk, UDP, siren, power / RTC, ...). If it stands still for 25 s, a small monitor task prints `[SYS] main loop stopped for 25 s in: ...` and stores the stage in NVS; after the restart it is shown in Settings → System → Information (*Last start*, e.g. "Watchdog (hvac sensor)") and printed as `[SYS] the main loop had stopped in: ...`.
- **Last fault** row in Settings → System → Information: reason, stage and date / time of the last unexpected restart (kept in NVS, also printed at start-up as `[SYS] last unexpected restart: ...`). A **Clear** button sets it and the counter of unexpected restarts back to 0.
- Reset reasons *USB reset (program upload)*, *power glitch* and *CPU lock-up* are named (a USB reset after uploading a program was shown as "Other"; it is not counted as unexpected).

## [1.3.1] - 2026-11-22

Stability release after a freeze (dark screen, Blynk offline) during a night of operation.

### Added
- **Main loop watchdog** (`health.cpp`, `health.h`): if the main loop (screen, touch, pages) stands still for 30 s, the Tab5 restarts by itself instead of staying frozen.
- **Reset reason:** printed at start-up (`[SYS] start, reset reason: ...`) and shown in Settings → System → Information (*Last start*), with a counter of unexpected restarts (watchdog, crash, low voltage; NVS namespace `health`).
- **Health line** every 10 minutes on the serial monitor: uptime, free internal RAM (and its minimum), free PSRAM, Wi-Fi and Blynk state. After every photo the free memory is printed as well.
- **Wi-Fi reconnect watchdog:** when the saved network is not connected for 30 s, a new connection attempt starts every 60 s (the automatic reconnect of the driver can give up, e.g. after a long router outage or a failed connection at start-up). Blynk reconnects by itself afterwards.

### Changed
- The Wi-Fi signal strength is read from the ESP32-C6 at most every 5 s (it was read twice a second; each request can block the screen for up to 5 s when the Wi-Fi chip does not answer).
- Camera: if the camera task holds the internal I2C bus for more than 20 s (stuck camera driver), touch, sound and battery reading use the bus anyway, so the screen can still be woken up. A photo that gets no frame within 9.5 s is aborted. Larger stack for the camera / e-mail task.
- Software version **1.3.1**.

### Fixed (also in the v1.3.0 package after the release)
- Photo upside down after switching Rotation to 180° without a restart; new *Photo rotation* setting per orientation.
- Camera did not start again after the first photo (`ISP_CCM ... out of range`): the camera is started afresh for every photo.
- No sound after switching the microphone on and off (shared I2S port of speaker and microphone).

## [1.3.0] - 2026-11-20

### Added
- **Alarm sirens** (`alarm_sound.cpp`, `alarm_sound.h`):
  - Motion: two-tone warble 1400 / 2100 Hz (300 ms each). Fire / smoke / gas: 3100 Hz temporal-3 (0.5 s beep × 3, 1.5 s pause).
  - Volume per tone 0–100 % on speaker channel 7 (`ALARM_SOUND_CHANNEL`); the system sounds use channels 0–6 with the system volume, the master volume stays at 100 %.
  - Siren switch per alarm (motion, fire, smoke, gas). NVS namespace `alarmsnd`.
  - The microphone is stopped while the siren plays and restored afterwards (shared I2S bus).
- **Alarm page:** speaker buttons on the two alarm cards; gear button opens the *Alarm settings* window (siren volumes with *Test*, camera switch, e-mail address with keyboard, *Send photo now*, camera status).
- **Camera photo by e-mail** (`camera_mail.cpp`, `camera_mail.h`, `mail_config.h`):
  - SC2356 (driver SC202CS) over MIPI-CSI through `ESP_Video`, RGB565 1280 × 720, frames dropped for `CAM_SETTLE_MS` (exposure), rotation from `mail_config.h` + Settings rotation, hardware JPEG (`driver/jpeg_encode.h`).
  - SMTP over SSL/TLS (port 465), AUTH LOGIN, multipart/mixed with the JPEG attached, UTF-8 subject; one retry.
  - Worker task on core 0; requests from the motion alarm, Blynk V28 and the device. NVS namespace `camera` (`on`, `mail`).
  - Camera enable: IO expander 0x43 pin 6. The sensor uses the internal I2C bus of M5Unified (`i2c_master_get_bus_handle`).
- **Photo rotation:** taken at the moment of the photo from the screen orientation and *Rotation 180°* (a rotation change in Settings counts at once), plus a turn per orientation chosen in the Alarm settings window (NVS `rot_l` / `rot_p`, defaults in `mail_config.h`). Fixes photos sent upside down after switching to 180°.
- **Internal I2C bus guard** (`cam_i2c_try_lock()`): touch reading, battery, RTC sync and audio set-up skip their access while the camera uses the bus.
- **Blynk** V25–V29 (siren volumes, camera on / off, take photo, camera status).
- Translations (6 languages); Alarm screenshots updated, new `docs/16_alarm_settings.png` (landscape and portrait).

### Changed
- `ui_alarm_trigger_src(reason, sources)`: alarms carry their source (motion, fire, smoke, gas); `ui_alarm_trigger()` = motion.
- Settings → System volume: system sounds only.
- esp32 boards package 3.3.11+ required for the camera (`CAM_ENABLE 0` without it).
- Software version raised to **1.3.0**.

## [1.2.0] - 2026-11-19

### Added
- **Orientation setting** (`settings.cpp`): *Settings → Display → Orientation*, above *Rotation*, with **Landscape** (1280 × 720) and **Portrait** (720 × 1280).
  - Stored in NVS (namespace `settings`, key `portrait`).
  - A change shows a message, saves the data, switches every output off (`ui_prepare_restart()`) and restarts the device in the new orientation.
  - `main.ino` reads the orientation (`settings_portrait()`) before the LVGL display is created: M5GFX rotation 1 (landscape) or 0 (portrait).
  - *Rotation* 0° / 180° works in both orientations: landscape 1 / 3, portrait 0 / 2.
- **Portrait layout for every page:**
  - Home (`home.cpp`): clock + indoor tile, weather with a 5-day forecast, blinds + RGB, air quality + scenes + alarm. The clock face is scaled to the narrower tile.
  - Climate (`hvac.cpp`): Quick / Power in their own row, the two Auto targets under each other, the information button in the card header, the warning banner at the bottom of the card. The information window uses 2 columns.
  - Weather (`weather.cpp`): the 4 panels under each other; the 4 small value cards in one row; details in 2 columns.
  - Blinds (`lights.cpp`), RGB light (`rgb.cpp`), Alarm (`alarm.cpp`), Air quality (`aq.cpp`): cards under each other at full width.
  - Scenes (`scene.cpp`): 2 × 4 cards. Clock styles (`clock.cpp`): 5 × 2 cards, name next to the preview.
  - Settings (`settings.cpp`): the 4 menu cards in a row under the top bar, pages below; information window, pickers and keyboard sized for the narrow screen.
  - Top bar (`menu.cpp`): clock right-aligned next to the Wi-Fi icon; on Home the motion indicator shows its icon only and the clock is hidden (Home has its own clock tile).
  - Splash screen (`splash.cpp`): logo above the title.
- Helpers in `theme.h`: `ui_portrait()`, `ui_scr_w()`, `ui_scr_h()`, `UI_LP(landscape, portrait)`.
- Translations in all 6 languages: orientation, landscape, portrait, restart message. `lang.cpp` / `lang.h` / `fonts_ui.c` regenerated.
- Portrait screenshots in `docs/portrait/` and `docs/overview_portrait.png`.

### Changed
- Settings → Display: the segmented buttons can be as wide as their text (orientation names).
- Software version raised to **1.2.0**.

### Fixed
- Home top bar: the Blynk / motion row is laid out from the left, so its width always follows its content (it could be cut off when the motion text got shorter).

### Upgrade notes
- No saved setting is reset; the device starts in landscape until portrait is selected.
- If the portrait picture is upside down on your unit, select *Rotation → 180°*.

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

[1.3.1]: #131---2026-11-22
[1.3.0]: #130---2026-11-20
[1.2.0]: #120---2026-11-19
[1.1.0]: #110---2026-11-18
[1.0.0]: #100
