// ============================================================================
//  alarm_sound.h  -  Alarm sirens, independent of the system volume
//
//  Two tones:
//   * Motion (intruder) : fast two-tone warble 1400 / 2100 Hz
//   * Fire / smoke / gas: 3100 Hz "temporal-3" pattern (3 beeps, pause),
//                         the usual pattern of smoke alarms (ISO 8201)
//  Each alarm (motion, fire, smoke, gas) has its own sound switch (Alarm page),
//  each tone has its own volume 0..100 % (Alarm settings window, Blynk V25 / V26).
//  The siren plays on its own speaker channel (ALARM_SOUND_CHANNEL), so the
//  system volume of Settings > System does not change it.
//  Settings are stored in NVS (namespace "alarmsnd").
// ============================================================================
#pragma once
#include <stdint.h>

// Alarm sources (bit mask)
enum AlarmSource : uint8_t {
  ASRC_MOTION = 0x01,
  ASRC_FIRE   = 0x02,
  ASRC_SMOKE  = 0x04,
  ASRC_GAS    = 0x08,
  ASRC_SAFETY = ASRC_FIRE | ASRC_SMOKE | ASRC_GAS
};

// Detector index 0..3 = motion, fire, smoke, gas (same order as the Alarm page)
enum { ADET_MOTION = 0, ADET_FIRE, ADET_SMOKE, ADET_GAS, ADET_COUNT };
// Tone group 0..1
enum { ATONE_MOTION = 0, ATONE_SAFETY, ATONE_COUNT };

#define ALARM_SOUND_CHANNEL  7       // M5Unified speaker channel of the siren (0..7)

void    alarm_sound_begin();                       // ui.init(): loads the settings
void    alarm_sound_tick();                        // every 100 ms (ui.cpp, main thread)

bool    alarm_sound_enabled(int det);              // sound switch of one alarm
void    alarm_sound_set_enabled(int det, bool on); // saved to NVS
uint8_t alarm_sound_volume(int tone);              // 0..100 %
void    alarm_sound_set_volume(int tone, uint8_t pct);   // saved to NVS (delayed)
void    alarm_sound_preview(int tone);             // plays the tone for ~2 s (volume test)
uint32_t alarm_sound_change_seq();                 // increases on every change (Blynk sync)

// The speaker codec (ES8388) and its amplifier are set up over the internal
// I2C bus, which the camera also uses. After every photo the camera task asks
// for a fresh speaker start (done in the main thread by alarm_sound_tick()).
void    alarm_sound_restart_speaker();             // any task
void    alarm_sound_log_state(const char* where);  // serial: speaker state (diagnosis)
