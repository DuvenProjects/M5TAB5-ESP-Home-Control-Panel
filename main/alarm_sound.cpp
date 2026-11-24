// ============================================================================
//  alarm_sound.cpp  -  Alarm sirens (see alarm_sound.h)
//
//  alarm_sound_tick() runs every 100 ms in the main thread and plays the
//  pattern step by step with M5.Speaker.tone() on ALARM_SOUND_CHANNEL. The
//  channel volume is the alarm volume; the system sounds use the other
//  channels with the system volume (settings.cpp), the master volume is 100 %.
//  When fire / smoke / gas and motion ring together, the fire tone wins.
// ============================================================================
#include <M5Unified.h>
#include <Preferences.h>
#include "alarm_sound.h"
#include "app_data.h"          // g_data.alarm_active
#include "settings.h"          // settings_restore_audio()
#include "camera_mail.h"       // internal I2C bus guard

uint8_t ui_alarm_sources();    // ui.cpp: sources of the ringing alarm (AlarmSource bits)

static Preferences s_prefs;
static bool     s_en[ADET_COUNT]   = {true, true, true, true};
static uint8_t  s_vol[ATONE_COUNT] = {80, 100};
static uint32_t s_seq = 1;
static bool     s_volDirty = false;
static uint32_t s_volDirtyMs = 0;

static int      s_playing = -1;          // tone group playing, -1 = silent
static uint16_t s_phase = 0;             // 100 ms steps since the start of the tone
static int      s_preview = -1;          // tone group of a volume test
static uint16_t s_previewLeft = 0;       // remaining 100 ms steps of the test
static bool     s_micStopped = false;    // the microphone was stopped for the siren
static volatile bool s_spkRestart = false;  // camera finished: start the speaker again

static const char* const EN_KEY[ADET_COUNT] = {"en_m", "en_f", "en_s", "en_g"};
static const char* const VOL_KEY[ATONE_COUNT] = {"vol_m", "vol_s"};

void alarm_sound_begin() {
  s_prefs.begin("alarmsnd", false);
  for (int i = 0; i < ADET_COUNT; i++) s_en[i] = s_prefs.getBool(EN_KEY[i], true);
  s_vol[ATONE_MOTION] = s_prefs.getUChar(VOL_KEY[0], 80);
  s_vol[ATONE_SAFETY] = s_prefs.getUChar(VOL_KEY[1], 100);
  for (int i = 0; i < ATONE_COUNT; i++) if (s_vol[i] > 100) s_vol[i] = 100;
}

bool alarm_sound_enabled(int det) { return det >= 0 && det < ADET_COUNT && s_en[det]; }

void alarm_sound_set_enabled(int det, bool on) {
  if (det < 0 || det >= ADET_COUNT || s_en[det] == on) return;
  s_en[det] = on;
  s_prefs.putBool(EN_KEY[det], on);
  s_seq++;
}

uint8_t alarm_sound_volume(int tone) { return (tone >= 0 && tone < ATONE_COUNT) ? s_vol[tone] : 0; }

void alarm_sound_set_volume(int tone, uint8_t pct) {
  if (tone < 0 || tone >= ATONE_COUNT) return;
  if (pct > 100) pct = 100;
  if (s_vol[tone] == pct) return;
  s_vol[tone] = pct;
  s_volDirty = true;                     // a slider sends many values: save once it stops
  s_volDirtyMs = millis();
  s_seq++;
}

void alarm_sound_preview(int tone) {
  if (tone < 0 || tone >= ATONE_COUNT) return;
  s_preview = tone;
  s_previewLeft = (tone == ATONE_MOTION) ? 18 : 25;     // one warble cycle set / three beeps
  if (s_playing == tone) s_playing = -2;                 // restart the pattern
}

uint32_t alarm_sound_change_seq() { return s_seq; }

void alarm_sound_restart_speaker() { s_spkRestart = true; }

void alarm_sound_log_state(const char* where) {
  Serial.printf("[SND] %s: speaker %s, mic %s, master %u, system ch %u, siren ch %u\n", where,
                M5.Speaker.isRunning() ? "on" : "OFF", M5.Mic.isRunning() ? "on" : "off",
                (unsigned)M5.Speaker.getVolume(), (unsigned)M5.Speaker.getChannelVolume(0),
                (unsigned)M5.Speaker.getChannelVolume(ALARM_SOUND_CHANNEL));
}

// One 100 ms step of a pattern
static void play_step(int tone, uint16_t ph) {
  if (tone == ATONE_MOTION) {
    // 300 ms 1400 Hz, 300 ms 2100 Hz, ...
    if (ph % 3 == 0) M5.Speaker.tone(((ph / 3) & 1) ? 2100 : 1400, 320, ALARM_SOUND_CHANNEL, true);
  } else {
    // temporal-3: beep 0.5 s, pause 0.5 s (x3), pause 1.5 s -> 4 s cycle
    uint16_t p = ph % 40;
    if (p == 0 || p == 10 || p == 20) M5.Speaker.tone(3100, 500, ALARM_SOUND_CHANNEL, true);
  }
}

void alarm_sound_tick() {
  // After a photo: codec + amplifier set up again (end + begin rewrites them)
  if (s_spkRestart && !M5.Mic.isRunning() && cam_i2c_try_lock()) {
    s_spkRestart = false;
    if (M5.Speaker.isRunning()) M5.Speaker.end();
    M5.Speaker.begin();
    cam_i2c_unlock();
    alarm_sound_log_state("after photo");
  }

  if (s_volDirty && millis() - s_volDirtyMs > 1500) {
    s_volDirty = false;
    s_prefs.putUChar(VOL_KEY[0], s_vol[0]);
    s_prefs.putUChar(VOL_KEY[1], s_vol[1]);
  }

  // Which tone should play now?
  int want = -1;
  if (g_data.alarm_active) {
    uint8_t m = ui_alarm_sources();
    bool safety = ((m & ASRC_FIRE)  && s_en[ADET_FIRE]) ||
                  ((m & ASRC_SMOKE) && s_en[ADET_SMOKE]) ||
                  ((m & ASRC_GAS)   && s_en[ADET_GAS]);
    bool motion = (m & ASRC_MOTION) && s_en[ADET_MOTION];
    if (safety && s_vol[ATONE_SAFETY] > 0)      want = ATONE_SAFETY;
    else if (motion && s_vol[ATONE_MOTION] > 0) want = ATONE_MOTION;
    s_preview = -1;                                      // a real alarm ends a volume test
  } else if (s_preview >= 0) {
    if (s_previewLeft > 0) { s_previewLeft--; want = s_preview; }
    else s_preview = -1;
  }

  if (want != s_playing) {
    M5.Speaker.stop(ALARM_SOUND_CHANNEL);
    s_playing = want;
    s_phase = 0;
    if (want < 0 && s_micStopped) {                      // siren over: microphone back on
      s_micStopped = false;
      settings_restore_audio();
    }
  }
  if (want < 0) return;

  // The speaker and the microphone share the I2S bus: the siren has priority.
  // Starting them sets up the audio codec over the internal I2C bus, which the
  // camera may be using: then try again in 100 ms.
  if (M5.Mic.isRunning() || !M5.Speaker.isRunning()) {
    if (!cam_i2c_try_lock()) return;
    if (M5.Mic.isRunning()) {
      M5.Mic.end();                                       // removes the shared I2S driver:
      if (M5.Speaker.isRunning()) M5.Speaker.end();       // the speaker must start afresh
      s_micStopped = true;
    }
    if (!M5.Speaker.isRunning()) M5.Speaker.begin();
    cam_i2c_unlock();
  }
  M5.Speaker.setChannelVolume(ALARM_SOUND_CHANNEL, (uint8_t)((s_vol[want] * 255) / 100));
  play_step(want, s_phase++);
}
