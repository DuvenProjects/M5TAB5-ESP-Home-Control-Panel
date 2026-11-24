// ============================================================================
//  camera_mail.cpp  -  Photo with the Tab5 camera, sent by e-mail
//
//  Main thread : settings, camera power (IO expander), requests, status text.
//  Worker task : camera start (once), photo, JPEG, SMTP. One job at a time.
//
//  Camera
//   * SC2356 (driver SC202CS), MIPI-CSI, 1280 x 720, through the ESP_Video
//     library (Arduino-ESP32 3.3.11+). The ISP turns the RAW picture into
//     RGB565; the first frames are dropped for CAM_SETTLE_MS so that exposure
//     and white balance can settle.
//   * The control bus (SCCB) of the sensor is the internal I2C bus of the
//     Tab5 that M5Unified already uses (touch, IO expanders ...). The camera
//     gets the handle of that bus (i2c_master_get_bus_handle), so both share
//     one driver and never talk at the same time.
//   * Power: camera enable = pin 6 of the IO expander PI4IOE5V6408 (0x43).
//  JPEG: hardware encoder of the ESP32-P4 (driver/jpeg_encode.h).
//  E-mail: SMTP over SSL/TLS (port 465), AUTH LOGIN, multipart/mixed with the
//  JPEG as attachment (mail_config.h).
// ============================================================================
#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <time.h>
#include "mail_config.h"
#include "camera_mail.h"
#include "settings.h"           // settings_wifi_connected(), orientation, rotation
#include "lang.h"               // tr()
#include "alarm_sound.h"        // speaker restart after a photo

#if CAM_ENABLE
#include <NetworkClientSecure.h>
#include <ESP_Video.h>
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#endif

// ============================================================ SHARED STATE
struct CamJob {
  CamRequest kind;
  char to[64];
  char subject[128];
  char body[320];
  int  rotation;                 // 0 / 90 / 180 / 270, clockwise (taken when the photo is requested)
};

static Preferences       s_prefs;
static bool              s_enabled = true;
static char              s_mailTo[64] = "";
#if CAM_ENABLE
static QueueHandle_t     s_jobs = nullptr;
static SemaphoreHandle_t s_i2cLock = nullptr;     // internal I2C bus: M5Unified <-> camera
#endif

#if CAM_ENABLE
static volatile uint32_t s_lockSince = 0;        // millis() when the camera task took the bus (0 = free)
static bool              s_lockOwned = false;    // main thread: the lock is really held
#endif

// Main thread only. If the camera task holds the bus for more than 20 s, the
// camera driver is stuck: the main thread then uses the bus anyway, so that
// touch, sound and battery keep working (a dark screen that cannot be woken
// up is worse than a rare bus collision).
bool cam_i2c_try_lock(uint32_t wait_ms) {
#if CAM_ENABLE
  if (!s_i2cLock) return true;
  if (xSemaphoreTake(s_i2cLock, pdMS_TO_TICKS(wait_ms)) == pdTRUE) { s_lockOwned = true; return true; }
  uint32_t since = s_lockSince;
  if (since && millis() - since > 20000) {
    static bool told = false;
    if (!told) { told = true; Serial.println("[CAM] camera task holds the I2C bus > 20 s: bus used anyway"); }
    s_lockOwned = false;
    return true;
  }
  return false;
#else
  (void)wait_ms;
  return true;
#endif
}

void cam_i2c_unlock() {
#if CAM_ENABLE
  if (s_i2cLock && s_lockOwned) { s_lockOwned = false; xSemaphoreGive(s_i2cLock); }
#endif
}
static volatile CamState s_state = CAM_ST_IDLE;
static volatile uint32_t s_stateTime = 0;
static volatile bool     s_busy = false;
static volatile uint32_t s_seq = 1;
static char              s_detail[96] = "";       // written by the worker, read by the UI
static uint8_t           s_rotQ[2] = {0, 0};      // picture turn in 90 deg steps: [0] landscape, [1] portrait

static void set_state(CamState st, const char* detail = nullptr) {
  if (detail) strlcpy(s_detail, detail, sizeof(s_detail));
  else        s_detail[0] = 0;
  s_state = st;
  time_t now = time(nullptr);
  s_stateTime = now > 1700000000 ? (uint32_t)now : 0;
  s_seq = s_seq + 1;
  Serial.printf("[CAM] state %d %s\n", (int)st, s_detail);
}

// ============================================================ SETTINGS
bool cam_enabled() { return s_enabled; }

void cam_set_enabled(bool on) {
  if (on == s_enabled) return;
  s_enabled = on;
  s_prefs.putBool("on", on);
  if (!on) set_state(CAM_ST_OFF);
  else if (s_state == CAM_ST_OFF) set_state(CAM_ST_IDLE);
  s_seq = s_seq + 1;
}

const char* cam_mail_to() { return s_mailTo; }

bool cam_mail_valid(const char* a) {
  if (!a) return false;
  size_t n = strlen(a);
  if (n < 6 || n >= sizeof(s_mailTo)) return false;
  const char* at = strchr(a, '@');
  if (!at || at == a || strchr(at + 1, '@')) return false;
  const char* dot = strrchr(at, '.');
  if (!dot || dot == at + 1 || dot[1] == 0) return false;
  for (size_t i = 0; i < n; i++) {                // no spaces, no line breaks (mail headers)
    char c = a[i];
    if (!(isalnum((unsigned char)c) || strchr("._%+-@", c))) return false;
  }
  return true;
}

bool cam_set_mail_to(const char* a) {
  if (!cam_mail_valid(a)) return false;
  strlcpy(s_mailTo, a, sizeof(s_mailTo));
  s_prefs.putString("mail", s_mailTo);
  if (s_state == CAM_ST_ERR_NO_ADDR) set_state(CAM_ST_IDLE);
  s_seq = s_seq + 1;
  return true;
}

// ---- picture rotation
// The camera turns with the Tab5. The turn that makes the photo upright is
// saved for landscape and for portrait (Alarm settings window); Settings >
// Display > Rotation 180 deg adds half a turn, because the Tab5 is then held
// the other way round. Read when the photo is requested, so a rotation change
// in Settings counts at once (no restart needed).
uint8_t cam_rotation_steps(bool portrait) { return s_rotQ[portrait ? 1 : 0]; }

void cam_set_rotation_steps(bool portrait, uint8_t q) {
  q &= 3;
  if (s_rotQ[portrait ? 1 : 0] == q) return;
  s_rotQ[portrait ? 1 : 0] = q;
  s_prefs.putUChar(portrait ? "rot_p" : "rot_l", q);
  s_seq = s_seq + 1;
}

int cam_photo_rotation() {
  int r = cam_rotation_steps(settings_portrait()) * 90 + (settings_rotated_180() ? 180 : 0);
  return r % 360;
}

CamState    cam_state()        { return s_state; }
uint32_t    cam_state_time()   { return s_stateTime; }
const char* cam_state_detail() { return s_detail; }
uint32_t    cam_change_seq()   { return s_seq; }

void cam_state_text(char* buf, size_t n) {
  char t[16] = "";
  if (s_stateTime) {
    time_t tt = s_stateTime;
    struct tm lt;
    localtime_r(&tt, &lt);
    strftime(t, sizeof(t), "%H:%M", &lt);
  }
  switch (s_state) {
    case CAM_ST_IDLE:        strlcpy(buf, tr(S_CAM_ST_READY), n); break;
    case CAM_ST_CAPTURING:   strlcpy(buf, tr(S_CAM_ST_CAPTURING), n); break;
    case CAM_ST_SENDING:     strlcpy(buf, tr(S_CAM_ST_SENDING), n); break;
    case CAM_ST_SENT:        snprintf(buf, n, "%s  %s", tr(S_CAM_ST_SENT), t); break;
    case CAM_ST_ERR_CAMERA:  snprintf(buf, n, "%s  %s", tr(S_CAM_ST_ERR_CAMERA), t); break;
    case CAM_ST_ERR_MAIL:    snprintf(buf, n, "%s  %s", tr(S_CAM_ST_ERR_MAIL), t); break;
    case CAM_ST_ERR_NO_ADDR: strlcpy(buf, tr(S_CAM_ST_NO_ADDR), n); break;
    case CAM_ST_ERR_NO_WIFI: strlcpy(buf, tr(S_CAM_ST_NO_WIFI), n); break;
    case CAM_ST_ERR_ACCOUNT: strlcpy(buf, tr(S_CAM_ST_NO_ACCOUNT), n); break;
    case CAM_ST_OFF:         strlcpy(buf, tr(S_CAM_ST_OFF), n); break;
    case CAM_ST_BUSY:        strlcpy(buf, tr(S_CAM_ST_BUSY), n); break;
  }
}

static bool account_set() {
  return strstr(MAIL_SMTP_USER, "YOUR_") == nullptr && strstr(MAIL_SMTP_PASSWORD, "YOUR_") == nullptr &&
         strlen(MAIL_SMTP_PASSWORD) >= 4;
}

// ============================================================ REQUEST (main thread)
bool cam_request(CamRequest kind, const char* reason) {
#if !CAM_ENABLE
  (void)kind; (void)reason;
  set_state(CAM_ST_ERR_CAMERA, "CAM_ENABLE is 0 in mail_config.h");
  return false;
#else
  if (!s_enabled)             { set_state(CAM_ST_OFF); return false; }
  if (!s_mailTo[0])           { set_state(CAM_ST_ERR_NO_ADDR); return false; }
  if (!account_set())         { set_state(CAM_ST_ERR_ACCOUNT); return false; }
  if (!settings_wifi_connected()) { set_state(CAM_ST_ERR_NO_WIFI); return false; }
  if (!s_jobs)                { set_state(CAM_ST_ERR_CAMERA, "camera task not started"); return false; }
  if (s_busy)                 return false;      // a photo is on its way: its state stays shown

  CamJob j;
  memset(&j, 0, sizeof(j));
  j.kind = kind;
  strlcpy(j.to, s_mailTo, sizeof(j.to));
  j.rotation = cam_photo_rotation();               // the screen orientation of this moment

  char when[40] = "";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(when, sizeof(when), "%d.%m.%Y %H:%M:%S", &lt);
  }
  if (kind == CAM_REQ_ALARM)
    snprintf(j.subject, sizeof(j.subject), "[Tab5] %s: %s", tr(S_CAM_SUBJ_ALARM), reason ? reason : "");
  else
    snprintf(j.subject, sizeof(j.subject), "[Tab5] %s", tr(S_CAM_SUBJ_CHECK));
  snprintf(j.body, sizeof(j.body), "%s\r\n%s%s%s\r\n", tr(S_CAM_BODY),
           when, (reason && reason[0]) ? "  -  " : "", reason ? reason : "");

  s_busy = true;
  set_state(CAM_ST_CAPTURING);
  if (xQueueSend(s_jobs, &j, 0) != pdTRUE) { s_busy = false; set_state(CAM_ST_BUSY); return false; }
  return true;
#endif
}

#if CAM_ENABLE
// ============================================================ CAMERA (worker)
static i2c_master_bus_handle_t s_i2c = nullptr;   // internal I2C bus of M5Unified
static ESPVideoClass           s_video;
static ESPVideoCaptureDevClass s_dev;
static bool                    s_camReady = false;
static jpeg_encoder_handle_t   s_jpeg = nullptr;

// The camera is started for every photo and fully stopped afterwards
// (camera_stop). Kept running between photos, the colour correction of the
// ISP started the next time with values from the last photo that the hardware
// does not accept ("ISP_CCM ... out of range" -> start capture failed).
// A fresh start also keeps the I2C bus free between photos.
static void camera_stop() {
  s_dev.end();                                     // stop stream, free buffers, close
  s_video.end();                                   // ISP pipeline, sensor, CSI (the I2C bus stays)
  s_camReady = false;
}

// The camera library prints a lot of errors that are no fault here:
//  * the sensor search tries every sensor driver of the library on the bus
//    (imx500, gc2145, ov5640 ...) before it finds the SC202CS,
//  * the automatic colour correction of the SC202CS profile computes values
//    the ISP does not accept ("ISP_CCM ... out of range"), then the ISP keeps
//    its last valid values - the photo is still fine.
// Hundreds of these lines per photo slow the serial port down, so they are muted.
static void quiet_camera_logs() {
  static const char* const TAGS[] = {
    "ISP_CCM", "isp_video", "ISP", "esp_video", "csi_video", "video_common",
    "sccb", "sccb_i2c", "esp_video_init",
    "imx500", "gc2145", "mira220", "os02n10", "os04c10", "ov2710", "ov5640",
    "ov5645", "ov5647", "ov9281", "pivariety", "sc035hgs"};
  for (const char* t : TAGS) esp_log_level_set(t, ESP_LOG_NONE);
}

static bool camera_start() {
  if (s_camReady) return true;
  quiet_camera_logs();
  if (!s_i2c) { set_state(CAM_ST_ERR_CAMERA, "no I2C bus handle"); return false; }
  ESPVideoCamConfigClass cam;
  cam.begin(s_i2c, 400000);                        // reset / power-down: IO expander (cam_begin)
  ESPVideoCSIConfigClass csi;
  csi.begin(cam);
  if (!s_video.isActive() && !s_video.begin(csi)) { set_state(CAM_ST_ERR_CAMERA, "sensor not found (SCCB)"); return false; }
  if (!s_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, 2)) { camera_stop(); set_state(CAM_ST_ERR_CAMERA, "capture device"); return false; }
  if (!s_dev.setFormat(ESP_VIDEO_FORMAT_RGB565)) { camera_stop(); set_state(CAM_ST_ERR_CAMERA, "RGB565 format"); return false; }
  jpeg_encode_engine_cfg_t ecfg = {};
  ecfg.timeout_ms = 2000;
  if (!s_jpeg && jpeg_new_encoder_engine(&ecfg, &s_jpeg) != ESP_OK) { set_state(CAM_ST_ERR_CAMERA, "JPEG encoder"); return false; }
  s_camReady = true;
  return true;
}

// Copies a RGB565 picture turned by rot degrees (clockwise) into dst
static void rotate_copy(const uint16_t* src, uint32_t w, uint32_t h, uint16_t* dst, int rot) {
  switch (rot) {
    case 90:                                       // dst: h x w
      for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) dst[x * h + (h - 1 - y)] = src[y * w + x];
      break;
    case 180:
      for (uint32_t i = 0, n = w * h; i < n; i++) dst[n - 1 - i] = src[i];
      break;
    case 270:
      for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) dst[(w - 1 - x) * h + y] = src[y * w + x];
      break;
    default:
      memcpy(dst, src, (size_t)w * h * 2);
  }
}

// Takes one photo and encodes it. *jpg is allocated here (heap_caps_free).
static bool take_photo(int rotation, uint8_t** jpg, uint32_t* jpgLen) {
  *jpg = nullptr; *jpgLen = 0;
  // Start; on a failed stream start everything once more from scratch
  bool started = false;
  for (int attempt = 0; attempt < 2 && !started; attempt++) {
    if (!camera_start()) { camera_stop(); delay(200); continue; }
    started = s_dev.startCapture();
    if (!started) {
      Serial.println("[CAM] stream did not start, camera restarted");
      camera_stop();
      delay(200);
    }
  }
  if (!started) { set_state(CAM_ST_ERR_CAMERA, "start capture"); return false; }

  uint8_t* in = nullptr;  size_t inCap = 0;
  uint8_t* out = nullptr; size_t outCap = 0;
  uint32_t w = 0, h = 0;
  bool ok = false;
  uint32_t t0 = millis();
  int fails = 0;
  for (;;) {
    ESPVideoBufferClass b = s_dev.captureBuffer();
    if (!b.valid()) {
      if (++fails > 10) { set_state(CAM_ST_ERR_CAMERA, "no frame"); break; }
      delay(20);
      continue;
    }
    if (millis() - t0 < CAM_SETTLE_MS) {          // exposure / white balance settle
      if (millis() - t0 > CAM_SETTLE_MS + 8000) { set_state(CAM_ST_ERR_CAMERA, "capture timeout"); break; }
      continue;
    }
    w = b.getWidth(); h = b.getHeight();
    if (w == 0 || h == 0 || b.size() < (size_t)w * h * 2) { set_state(CAM_ST_ERR_CAMERA, "frame size"); break; }
    jpeg_encode_memory_alloc_cfg_t ic = {};
    ic.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    in = (uint8_t*)jpeg_alloc_encoder_mem((size_t)w * h * 2, &ic, &inCap);
    if (!in) { set_state(CAM_ST_ERR_CAMERA, "no memory"); break; }
    rotate_copy((const uint16_t*)b.data(), w, h, (uint16_t*)in, rotation);
    ok = true;
    break;                                        // b is given back to the driver here
  }
  camera_stop();                                  // stream, ISP and sensor off until the next photo
  if (!ok) { if (in) heap_caps_free(in); return false; }

  if (rotation == 90 || rotation == 270) { uint32_t t = w; w = h; h = t; }
  jpeg_encode_memory_alloc_cfg_t oc = {};
  oc.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  out = (uint8_t*)jpeg_alloc_encoder_mem((size_t)w * h, &oc, &outCap);   // 1 byte / pixel is plenty
  if (!out) { heap_caps_free(in); set_state(CAM_ST_ERR_CAMERA, "no memory"); return false; }

  jpeg_encode_cfg_t cfg = {};
  cfg.width = w;
  cfg.height = h;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  cfg.image_quality = CAM_JPEG_QUALITY;
  uint32_t len = 0;
  esp_err_t e = jpeg_encoder_process(s_jpeg, &cfg, in, (uint32_t)((size_t)w * h * 2), out, (uint32_t)outCap, &len);
  heap_caps_free(in);
  if (e != ESP_OK || len == 0) {
    heap_caps_free(out);
    char d[48]; snprintf(d, sizeof(d), "JPEG error %d", (int)e);
    set_state(CAM_ST_ERR_CAMERA, d);
    return false;
  }
  Serial.printf("[CAM] photo %lu x %lu, JPEG %lu bytes\n", (unsigned long)w, (unsigned long)h, (unsigned long)len);
  *jpg = out; *jpgLen = len;
  return true;
}

// ============================================================ SMTP (worker)
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64(const uint8_t* src, size_t n, char* dst) {   // dst: 4 * ceil(n / 3) + 1
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)src[i] << 16;
    if (i + 1 < n) v |= (uint32_t)src[i + 1] << 8;
    if (i + 2 < n) v |= src[i + 2];
    dst[o++] = B64[(v >> 18) & 63];
    dst[o++] = B64[(v >> 12) & 63];
    dst[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
    dst[o++] = (i + 2 < n) ? B64[v & 63] : '=';
  }
  dst[o] = 0;
  return o;
}

class Smtp {
public:
  NetworkClientSecure c;
  char line[256];
  int code = 0;

  // Reads a (multi-line) reply, returns its code
  int reply(uint32_t timeoutMs = 15000) {
    code = 0;
    for (;;) {
      size_t n = 0;
      uint32_t t0 = millis();
      for (;;) {
        if (c.available()) {
          char ch = (char)c.read();
          if (ch == '\n') break;
          if (ch != '\r' && n < sizeof(line) - 1) line[n++] = ch;
        } else {
          if (!c.connected() || millis() - t0 > timeoutMs) { line[n] = 0; return code = -1; }
          delay(5);
        }
      }
      line[n] = 0;
      if (n < 3) continue;
      if (n == 3 || line[3] == ' ') { code = atoi(line); return code; }   // last line
      // "250-..." continuation line: read on
    }
  }
  bool cmd(const char* s, int expect, int expect2 = 0) {
    if (s) { c.print(s); c.print("\r\n"); }
    int r = reply();
    return r == expect || (expect2 && r == expect2);
  }
  bool cmdB64(const char* s, int expect) {
    char e[160];
    b64((const uint8_t*)s, strlen(s), e);
    return cmd(e, expect);
  }
};

// "=?UTF-8?B?...?=": Turkish / Chinese subjects and names arrive intact
static void enc_word(const char* text, char* out, size_t outLen) {
  char e[260];
  b64((const uint8_t*)text, strnlen(text, 180), e);
  snprintf(out, outLen, "=?UTF-8?B?%s?=", e);
}

static bool send_mail(const CamJob& j, const uint8_t* jpg, uint32_t len, char* err, size_t errLen) {
  Smtp s;
  if (strlen(MAIL_ROOT_CA) > 10) s.c.setCACert(MAIL_ROOT_CA);
  else                           s.c.setInsecure();       // encrypted, server not verified
  if (!s.c.connect(MAIL_SMTP_HOST, MAIL_SMTP_PORT, 15000)) {
    snprintf(err, errLen, "no connection to %s:%d", MAIL_SMTP_HOST, MAIL_SMTP_PORT);
    return false;
  }

  bool ok = false;
  const char* step = "greeting";
  do {
    if (s.reply() != 220)                              break;
    step = "EHLO";  if (!s.cmd("EHLO tab5.local", 250))          break;
    step = "AUTH";  if (!s.cmd("AUTH LOGIN", 334))               break;
    step = "user";  if (!s.cmdB64(MAIL_SMTP_USER, 334))          break;
    step = "password"; if (!s.cmdB64(MAIL_SMTP_PASSWORD, 235))   break;
    char b[160];
    snprintf(b, sizeof(b), "MAIL FROM:<%s>", MAIL_SMTP_USER);
    step = "MAIL FROM"; if (!s.cmd(b, 250))                      break;
    snprintf(b, sizeof(b), "RCPT TO:<%s>", j.to);
    step = "RCPT TO"; if (!s.cmd(b, 250, 251))                   break;
    step = "DATA";  if (!s.cmd("DATA", 354))                     break;

    // ---- headers
    char w[300];
    enc_word(MAIL_FROM_NAME, w, sizeof(w));
    s.c.printf("From: %s <%s>\r\n", w, MAIL_SMTP_USER);
    s.c.printf("To: <%s>\r\n", j.to);
    enc_word(j.subject, w, sizeof(w));
    s.c.printf("Subject: %s\r\n", w);
    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm lt;
      localtime_r(&now, &lt);
      char d[48];
      strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &lt);
      s.c.printf("Date: %s\r\n", d);
    }
    const char* BND = "----tab5-photo-boundary";
    s.c.print("MIME-Version: 1.0\r\n");
    s.c.printf("Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n", BND);

    // ---- text part
    s.c.printf("--%s\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n", BND);
    s.c.print(j.body);
    s.c.print("\r\n");

    // ---- photo: base64 in lines of 76 characters, sent in blocks of ~1.5 KB
    char fn[40];
    if (now > 1700000000) {
      struct tm lt;
      localtime_r(&now, &lt);
      strftime(fn, sizeof(fn), "tab5_%Y%m%d_%H%M%S.jpg", &lt);
    } else {
      strlcpy(fn, "tab5_photo.jpg", sizeof(fn));
    }
    s.c.printf("--%s\r\nContent-Type: image/jpeg; name=\"%s\"\r\nContent-Transfer-Encoding: base64\r\n"
               "Content-Disposition: attachment; filename=\"%s\"\r\n\r\n", BND, fn, fn);
    static char blk[20 * 78 + 1];
    size_t bn = 0;
    bool wr = true;
    for (uint32_t i = 0; i < len && wr; i += 57) {
      size_t n = (len - i) < 57 ? (len - i) : 57;
      bn += b64(jpg + i, n, blk + bn);
      blk[bn++] = '\r'; blk[bn++] = '\n';
      if (bn >= 19 * 78 || i + n >= len) {
        wr = s.c.write((const uint8_t*)blk, bn) == bn;
        bn = 0;
      }
    }
    if (!wr) { step = "upload"; break; }
    s.c.printf("--%s--\r\n", BND);
    step = "end of message"; if (!s.cmd(".", 250))               break;
    s.cmd("QUIT", 221);
    ok = true;
  } while (false);

  if (!ok) snprintf(err, errLen, "%s: %d %s", step, s.code, s.line);
  s.c.stop();
  return ok;
}

// ============================================================ WORKER TASK
static void worker(void*) {
  CamJob j;
  for (;;) {
    if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
    uint8_t* jpg = nullptr;
    uint32_t len = 0;
    set_state(CAM_ST_CAPTURING);
    delay(300);                                   // an alarm siren starts first (speaker set-up uses the bus)
    xSemaphoreTake(s_i2cLock, portMAX_DELAY);     // the main thread pauses its I2C accesses
    s_lockSince = millis() | 1;
    bool shot = take_photo(j.rotation, &jpg, &len);
    s_lockSince = 0;
    xSemaphoreGive(s_i2cLock);
    alarm_sound_restart_speaker();                // speaker codec / amplifier set up again
    if (shot) {
      set_state(CAM_ST_SENDING);
      char err[96] = "";
      bool ok = false;
      for (int attempt = 0; attempt < 2 && !ok; attempt++) {     // one retry
        if (!settings_wifi_connected()) { strlcpy(err, "no Wi-Fi", sizeof(err)); break; }
        ok = send_mail(j, jpg, len, err, sizeof(err));
        if (!ok) { Serial.printf("[CAM] e-mail failed: %s\n", err); delay(3000); }
      }
      heap_caps_free(jpg);
      Serial.printf("[CAM] after photo: RAM %u KB, PSRAM %u KB free\n",
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
      if (ok) set_state(CAM_ST_SENT);
      else    set_state(CAM_ST_ERR_MAIL, err);
    }
    s_busy = false;
  }
}
#endif  // CAM_ENABLE

// ============================================================ START (main thread)
void cam_begin() {
  s_prefs.begin("camera", false);
  s_enabled = s_prefs.getBool("on", true);
  String m = s_prefs.getString("mail", "");
  strlcpy(s_mailTo, m.c_str(), sizeof(s_mailTo));
  if (!s_enabled) s_state = CAM_ST_OFF;

  // Picture turn per orientation (Alarm settings window), defaults: mail_config.h
  s_rotQ[0] = s_prefs.getUChar("rot_l", (CAM_ROTATION_LANDSCAPE / 90) & 3) & 3;
  s_rotQ[1] = s_prefs.getUChar("rot_p", (CAM_ROTATION_PORTRAIT / 90) & 3) & 3;

#if CAM_ENABLE
  // Camera power: IO expander PI4IOE5V6408 (0x43), pin 6 = camera enable
  if (M5.getBoard() == m5::board_t::board_M5Tab5 || M5.getBoard() == m5::board_t::board_M5Tab5X) {
    auto& io = M5.getIOExpander(0);
    io.setDirection(6, true);
    io.setHighImpedance(6, false);
    io.digitalWrite(6, true);
  }
  // Control bus of the sensor = the internal I2C bus that M5Unified opened
  i2c_port_num_t port = (i2c_port_num_t)M5.In_I2C.getPort();
  if (i2c_master_get_bus_handle(port, &s_i2c) != ESP_OK) s_i2c = nullptr;
  Serial.printf("[CAM] I2C port %d, bus %s\n", (int)port, s_i2c ? "shared" : "NOT FOUND");

  s_jobs = xQueueCreate(1, sizeof(CamJob));
  s_i2cLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(worker, "cam_mail", 16384, nullptr, 1, nullptr, 0);   // TLS handshake needs room
#endif
}
