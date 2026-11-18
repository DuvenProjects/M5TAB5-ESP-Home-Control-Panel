// ============================================================================
//  theme.cpp  -  Dark -> light colour conversion (see theme.h)
// ============================================================================
#include "theme.h"
#include <math.h>
#include "settings.h"     // settings_light_theme()

bool ui_light_theme() { return settings_light_theme(); }

static float hue2rgb(float p, float q, float t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.0f / 6) return p + (q - p) * 6 * t;
  if (t < 1.0f / 2) return q;
  if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
  return p;
}

static uint32_t to_light(uint32_t hex) {
  float r = ((hex >> 16) & 0xFF) / 255.0f, g = ((hex >> 8) & 0xFF) / 255.0f, b = (hex & 0xFF) / 255.0f;
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
  float l = (mx + mn) / 2, h = 0, s = 0, d = mx - mn;
  if (d > 0.0001f) {
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r)      h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else              h = (r - g) / d + 4;
    h /= 6;
  }

  float nl, ns = s;
  if (l < 0.11f) {                    // screen backgrounds -> light grey .. white
    nl = 0.93f + l / 0.11f * 0.07f;
    ns = s * 0.5f;
  } else if (l < 0.30f) {             // tiles, cards, boxes -> white .. light grey
    nl = 1.0f - (l - 0.11f) / 0.19f * 0.20f;
    ns = s * 0.5f;
  } else if (s < 0.25f) {             // neutral greys / white text -> dark greys / near black
    nl = 0.80f - (l - 0.30f) / 0.70f * 0.70f;
  } else {                            // accent colours: keep, darken pastels
    nl = l > 0.55f ? 0.55f - (l - 0.55f) * 0.3f : l;
  }

  float q = nl < 0.5f ? nl * (1 + ns) : nl + ns - nl * ns, p = 2 * nl - q;
  float rr = ns < 0.0001f ? nl : hue2rgb(p, q, h + 1.0f / 3);
  float gg = ns < 0.0001f ? nl : hue2rgb(p, q, h);
  float bb = ns < 0.0001f ? nl : hue2rgb(p, q, h - 1.0f / 3);
  auto c8 = [](float v) { int i = (int)lroundf(v * 255); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
  return (c8(rr) << 16) | (c8(gg) << 8) | c8(bb);
}

lv_color_t ui_col(uint32_t dark_hex) {
  return lv_color_hex(ui_light_theme() ? to_light(dark_hex) : dark_hex);
}
