// ============================================================================
//  camera_mail.h  -  Photo with the Tab5 camera, sent by e-mail
//
//  * Motion alarm: when the camera is switched on, a photo is taken and sent
//    to the saved e-mail address (ui.cpp).
//  * Blynk V28 / Alarm settings window "Send photo now": a photo to check the
//    home, on request.
//  * Camera on / off: Alarm settings window and Blynk V27. Off = no photos.
//  * The recipient address is entered on the device and stored in NVS
//    (namespace "camera"); the sender account is in mail_config.h.
//
//  The work (camera, JPEG, SMTP) runs in its own task, so the screen never
//  waits for it. Camera: SC2356 (SC202CS driver) over MIPI-CSI with the
//  ESP_Video library of Arduino-ESP32 3.3.11+, JPEG: hardware encoder of the
//  ESP32-P4.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

enum CamState : uint8_t {
  CAM_ST_IDLE = 0,       // nothing sent yet
  CAM_ST_CAPTURING,      // taking the photo
  CAM_ST_SENDING,        // sending the e-mail
  CAM_ST_SENT,           // last photo sent (cam_state_time())
  CAM_ST_ERR_CAMERA,     // camera did not start / no picture
  CAM_ST_ERR_MAIL,       // SMTP error (cam_state_detail())
  CAM_ST_ERR_NO_ADDR,    // no recipient address saved
  CAM_ST_ERR_NO_WIFI,    // no Wi-Fi
  CAM_ST_ERR_ACCOUNT,    // sender account not set in mail_config.h
  CAM_ST_OFF,            // camera switched off
  CAM_ST_BUSY            // a request came while a photo was in progress
};

enum CamRequest : uint8_t {
  CAM_REQ_ALARM = 0,     // motion alarm
  CAM_REQ_REMOTE,        // Blynk button
  CAM_REQ_LOCAL          // "Send photo now" on the device
};

void        cam_begin();                     // ui.init(): settings, camera power, worker task

bool        cam_enabled();                   // camera on (photos allowed)
void        cam_set_enabled(bool on);        // saved to NVS
const char* cam_mail_to();                   // recipient, "" = not set
bool        cam_set_mail_to(const char* a);  // false = not a valid address (not saved)
bool        cam_mail_valid(const char* a);   // address check (letters, digits, . _ % + - @)

// Queues a photo (main thread). reason: text for the subject, e.g. the alarm
// reason. Returns false when it cannot be done now (off, no address, busy);
// cam_state() tells why.
bool        cam_request(CamRequest kind, const char* reason);

// Picture rotation in 90 deg steps (clockwise) for landscape / portrait,
// saved in NVS. Settings > Display > Rotation 180 deg is added automatically.
uint8_t     cam_rotation_steps(bool portrait);              // 0..3
void        cam_set_rotation_steps(bool portrait, uint8_t q);
int         cam_photo_rotation();                           // turn used now: 0 / 90 / 180 / 270

CamState    cam_state();
uint32_t    cam_state_time();                // epoch of the last state change (0 = unknown)
const char* cam_state_detail();              // SMTP reply / camera error text (English)
void        cam_state_text(char* buf, size_t n);   // translated one-line status
uint32_t    cam_change_seq();                // increases on every change (Blynk sync, UI)

// ---- Internal I2C bus guard
// M5Unified reaches the internal I2C bus (touch, battery monitor, RTC, audio
// codec, IO expanders) with its own register access, the camera (SCCB) with the
// ESP-IDF driver: the two must not run at the same time. The camera task holds
// this lock while it uses the camera (about 2 s per photo); the main thread
// takes it around its own accesses and skips them while the camera has it.
bool        cam_i2c_try_lock(uint32_t wait_ms = 0);   // true = locked, call cam_i2c_unlock()
void        cam_i2c_unlock();
