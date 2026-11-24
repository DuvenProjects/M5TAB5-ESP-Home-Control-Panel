// ============================================================================
//  mail_config.h  -  E-mail account that sends the camera photos (SMTP)
//
//  The RECIPIENT address is entered on the device:
//      Alarm page > [gear] Alarm settings > E-mail address
//  The SENDER account is set here. Use a separate mail account for the Tab5.
//
//  Gmail (recommended):
//   1. Google account > Security > 2-Step Verification: on
//   2. Google account > Security > App passwords: create one ("Tab5")
//   3. Enter the account and the 16-character app password below
//      (NOT your normal Gmail password).
//  Other providers: any SMTP server with SSL/TLS on port 465 works
//  (e.g. Yandex smtp.yandex.com, Yahoo smtp.mail.yahoo.com, GMX mail.gmx.net).
//  STARTTLS on port 587 is not supported.
//
//  Never publish this file with your real password (e.g. on GitHub).
// ============================================================================
#pragma once

#define MAIL_SMTP_HOST      "smtp.gmail.com"
#define MAIL_SMTP_PORT      465                       // SSL / TLS
#define MAIL_SMTP_USER      "YOUR_SENDER@gmail.com"   // sender account (login)
#define MAIL_SMTP_PASSWORD  "YOUR_APP_PASSWORD"       // Gmail: 16-character app password
#define MAIL_FROM_NAME      "Tab5 Home"               // name shown as the sender

// Root certificate of the SMTP server (PEM). Empty = the connection is
// encrypted, but the server certificate is not checked.
#define MAIL_ROOT_CA        ""

// ---- Camera (SC2356 / SC202CS, 1280 x 720) --------------------------------
// 1 = camera + e-mail compiled in (needs the "esp32" boards package 3.3.11 or
// newer: it brings the ESP_Video library). 0 = without camera (older packages).
#ifndef CAM_ENABLE
#define CAM_ENABLE              1
#endif
// Default picture turn (0, 90, 180 or 270 deg clockwise) for each orientation.
// It can be changed on the device: Alarm page > gear > Photo rotation (saved),
// check it with "Send photo now". Settings > Display > Rotation 180 deg is
// added automatically.
#define CAM_ROTATION_LANDSCAPE  0       // Settings > Display > Orientation: Landscape
#define CAM_ROTATION_PORTRAIT   270     //                                   Portrait
#define CAM_JPEG_QUALITY        80      // 1..100
#define CAM_SETTLE_MS           1500    // exposure / white balance settling time before the photo
