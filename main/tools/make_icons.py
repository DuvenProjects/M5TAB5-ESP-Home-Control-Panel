#!/usr/bin/env python3
"""
Generates hvac_icons.c : LVGL 9 bitmap fonts (4 bpp, uncompressed):
  font_hvac_icons : 36x36 px icon glyphs (below)
  font_hvac_big   : Inter Bold 104 px, characters 0-9 . - % degree sign and space
  font_hvac_mid   : Inter Bold 72 px, same characters

Code points (UTF-8 in hvac.cpp):
  U+E000 house+thermometer  U+E001 power   U+E002 flame     U+E003 snowflake
  U+E004 auto gauge         U+E005 droplet U+E006 fan       U+E007 info
  U+E008 bolt (quick start)

Usage: python3 make_icons.py [out.c] [preview.png]
"""
import math
import sys
from PIL import Image, ImageDraw, ImageFont

SIZE = 36
SS = 8
LATIN = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
BIGTTF = "/usr/share/fonts/opentype/inter/Inter-Bold.otf"
BIG_CHARS = " %-.0123456789\u00b0"


class Canvas:
    def __init__(self, size):
        self.s, self.n = size, size * SS
        self.im = Image.new("L", (self.n, self.n), 0)
        self.d = ImageDraw.Draw(self.im)

    def p(self, v):
        return v * self.n

    def pt(self, x, y):
        return (self.p(x), self.p(y))

    def circle(self, cx, cy, r, fill=255, outline=None, w=0.0):
        box = [self.p(cx - r), self.p(cy - r), self.p(cx + r), self.p(cy + r)]
        if outline is None:
            self.d.ellipse(box, fill=fill)
        else:
            self.d.ellipse(box, outline=outline, width=max(1, int(self.p(w))))

    def line(self, pts, w, fill=255, caps=True):
        self.d.line([self.pt(x, y) for x, y in pts], fill=fill,
                    width=max(1, int(self.p(w))), joint="curve")
        if caps:
            for x, y in pts:
                self.circle(x, y, w / 2, fill=fill)

    def arc(self, cx, cy, r, a0, a1, w, fill=255):
        ro = r + w / 2
        self.d.arc([self.p(cx - ro), self.p(cy - ro), self.p(cx + ro), self.p(cy + ro)],
                   a0, a1, fill=fill, width=max(1, int(self.p(w))))

    def poly(self, pts, fill=255):
        self.d.polygon([self.pt(x, y) for x, y in pts], fill=fill)

    def rrect(self, x0, y0, x1, y1, r, fill=255):
        self.d.rounded_rectangle([self.p(x0), self.p(y0), self.p(x1), self.p(y1)],
                                 radius=self.p(r), fill=fill)

    def rect(self, x0, y0, x1, y1, fill=255):
        self.d.rectangle([self.p(x0), self.p(y0), self.p(x1), self.p(y1)], fill=fill)

    def text(self, x, y, s, px):
        f = ImageFont.truetype(LATIN, int(self.p(px)))
        self.d.text(self.pt(x, y), s, font=f, fill=255, anchor="mm")

    def result(self):
        return self.im.resize((self.s, self.s), Image.LANCZOS)


def house(c):
    c.poly([(0.03, 0.50), (0.40, 0.10), (0.77, 0.50)])
    c.rect(0.13, 0.48, 0.67, 0.92)
    c.rect(0.31, 0.64, 0.49, 0.92, fill=0)
    c.rrect(0.74, 0.12, 0.90, 0.66, 0.08)
    c.circle(0.82, 0.76, 0.15)
    c.rrect(0.79, 0.30, 0.85, 0.72, 0.03, fill=0)
    c.circle(0.82, 0.76, 0.05, fill=0)


def power(c):
    c.arc(0.5, 0.54, 0.31, 300, 360, 0.09)
    c.arc(0.5, 0.54, 0.31, 0, 240, 0.09)
    c.line([(0.5, 0.08), (0.5, 0.50)], 0.09)


def flame(c):
    c.circle(0.5, 0.65, 0.29)
    c.poly([(0.22, 0.54), (0.52, 0.03), (0.78, 0.54)])
    c.poly([(0.22, 0.54), (0.10, 0.28), (0.36, 0.40)])
    c.circle(0.5, 0.72, 0.13, fill=0)
    c.poly([(0.38, 0.72), (0.5, 0.50), (0.62, 0.72)], fill=0)


def snowflake(c):
    for k in range(3):
        a = math.radians(90 + k * 60)
        ca, sa = math.cos(a), math.sin(a)
        c.line([(0.5 - 0.46 * ca, 0.5 - 0.46 * sa), (0.5 + 0.46 * ca, 0.5 + 0.46 * sa)], 0.07)
        for sgn in (1, -1):
            # branch start point on the arm, branches point outward at +/-45 deg
            ex, ey = 0.5 + sgn * 0.27 * ca, 0.5 + sgn * 0.27 * sa
            ox, oy = sgn * ca, sgn * sa
            for off in (-45, 45):
                r = math.radians(off)
                dx = ox * math.cos(r) - oy * math.sin(r)
                dy = ox * math.sin(r) + oy * math.cos(r)
                c.line([(ex, ey), (ex + 0.15 * dx, ey + 0.15 * dy)], 0.06)


def gauge(c):
    cx, cy = 0.45, 0.54
    c.arc(cx, cy, 0.33, 135, 360, 0.08)
    c.arc(cx, cy, 0.33, 0, 45, 0.08)
    c.line([(cx, cy), (cx + 0.17, cy - 0.18)], 0.075)
    c.circle(cx, cy, 0.07)
    c.text(0.80, 0.78, "A", 0.36)


def droplet(c):
    c.circle(0.5, 0.65, 0.28)
    c.poly([(0.23, 0.57), (0.5, 0.04), (0.77, 0.57)])
    c.arc(0.5, 0.65, 0.17, 95, 165, 0.05, fill=0)


def fan(c):
    for k in range(3):
        a = math.radians(k * 120 - 90)
        cx, cy = 0.5 + 0.24 * math.cos(a), 0.5 + 0.24 * math.sin(a)
        r = a + math.radians(35)
        pts = []
        for t in range(0, 360, 15):
            tt = math.radians(t)
            ex, ey = 0.23 * math.cos(tt), 0.12 * math.sin(tt)
            pts.append((cx + ex * math.cos(r) - ey * math.sin(r),
                        cy + ex * math.sin(r) + ey * math.cos(r)))
        c.poly(pts)
    c.circle(0.5, 0.5, 0.10)
    c.circle(0.5, 0.5, 0.04, fill=0)


def info(c):
    c.circle(0.5, 0.5, 0.43, outline=255, w=0.08)
    c.circle(0.5, 0.29, 0.06)
    c.line([(0.5, 0.45), (0.5, 0.74)], 0.09)


def bolt(c):
    c.poly([(0.60, 0.03), (0.20, 0.56), (0.46, 0.56), (0.38, 0.97), (0.80, 0.40), (0.54, 0.40)])


def wifi_icon(level):
    """level 0 = no connection (dim arcs + slash), 1..4 = signal bars."""
    def draw(c):
        lit, dim = 255, 70
        c.circle(0.5, 0.80, 0.075, fill=lit if level >= 1 else dim)
        for i, r in enumerate((0.20, 0.38, 0.56)):
            c.arc(0.5, 0.80, r, 228, 312, 0.09, fill=lit if level >= i + 2 else dim)
        if level == 0:
            c.line([(0.12, 0.10), (0.88, 0.90)], 0.08)
    return draw


# U+E000..E008 icons, U+E009..E00D Wi-Fi level 0..4
GLYPHS = [house, power, flame, snowflake, gauge, droplet, fan, info, bolt] + [wifi_icon(i) for i in range(5)]


def pack4(img):
    nib = [round(v * 15 / 255) for v in img.tobytes()]
    if len(nib) % 2:
        nib.append(0)
    return bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))


def c_array(f, name, data):
    f.write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_bitmap[] = {\n" % name)
    for i in range(0, len(data), 24):
        f.write("    " + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
    if not data:
        f.write("    0\n")
    f.write("};\n\n")


def emit_font(f, name, glyphs, cps, line_h, base_line, ul_pos, ul_th):
    """glyphs: list of (bitmap, adv16, w, h, ofs_x, ofs_y) ordered like cps (sorted code points)."""
    data = b"".join(g[0] for g in glyphs)
    c_array(f, name, data)
    f.write("static const lv_font_fmt_txt_glyph_dsc_t %s_glyph_dsc[] = {\n" % name)
    f.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}, /* id 0 reserved */\n")
    off = 0
    for cp, g in zip(cps, glyphs):
        f.write("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}, /* U+%04X */\n"
                % (off, g[1], g[2], g[3], g[4], g[5], cp))
        off += len(g[0])
    f.write("};\n\n")
    cmaps = []
    for i, cp in enumerate(cps):
        if cmaps and cp == cmaps[-1][0] + cmaps[-1][1]:
            cmaps[-1][1] += 1
        else:
            cmaps.append([cp, 1, i + 1])
    f.write("static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {\n" % name)
    for start, length, gid in cmaps:
        f.write("    {.range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
                "     .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n"
                "     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},\n" % (start, length, gid))
    f.write("};\n\n")
    f.write("""static const lv_font_fmt_txt_dsc_t %(n)s_dsc = {
    .glyph_bitmap = %(n)s_bitmap,
    .glyph_dsc = %(n)s_glyph_dsc,
    .cmaps = %(n)s_cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = %(cn)d,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t font_%(n)s = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = %(lh)d,
    .base_line = %(bl)d,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = %(up)d,
    .underline_thickness = %(ut)d,
    .dsc = &%(n)s_dsc,
    .fallback = NULL,
    .user_data = NULL,
};

""" % dict(n=name, cn=len(cmaps), lh=line_h, bl=base_line, up=ul_pos, ut=ul_th))


def big_font_glyphs(px):
    font = ImageFont.truetype(BIGTTF, px)
    ascent, descent = font.getmetrics()
    cps = sorted(ord(c) for c in BIG_CHARS)
    glyphs = []
    for cp in cps:
        ch = chr(cp)
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = x1 - x0, y1 - y0
        adv = font.getlength(ch)
        if w <= 0 or h <= 0:
            glyphs.append((b"", round(adv * 16), 0, 0, 0, 0))
            continue
        ew = w + (w % 2)                       # even width (nibble stream == row aligned)
        im = Image.new("L", (ew, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
        glyphs.append((pack4(im), round(adv * 16), ew, h, x0, -y1))
    return cps, glyphs, ascent + descent, descent


HEADER = """/*
 * hvac_icons.c  -  GENERATED FILE (tools/make_icons.py)
 * Fonts for the Tab5 HVAC application. LVGL 9, 4 bpp, uncompressed.
 *   font_hvac_icons : 36x36 px icons U+E000..U+E00D (needs HVAC_USE_ICON_FONT 1)
 *   font_hvac_anim  : 64x64 px flame E000 / snowflake E001 / droplet E002 / fan E003 (running animations)
 *   font_hvac_big   : Inter Bold 104 px, characters  %-.0123456789 and degree sign
 *   font_hvac_mid   : Inter Bold 72 px, same characters
 */
#include "hvac.h"

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "hvac_icons.c"
    prev = sys.argv[2] if len(sys.argv) > 2 else None
    imgs, glyphs = [], []
    for fn in GLYPHS:
        c = Canvas(SIZE)
        fn(c)
        im = c.result()
        imgs.append(im)
        glyphs.append((pack4(im), SIZE * 16, SIZE, SIZE, 0, 0))
    cps = [0xE000 + i for i in range(len(GLYPHS))]
    with open(out, "w") as f:
        f.write(HEADER)
        f.write("#if HVAC_USE_ICON_FONT\n\n")
        # emit_font names the lv_font_t "font_<name>"
        emit_font(f, "hvac_icons", glyphs, cps, SIZE, 0, 0, 1)
        # 64 px animation icons: U+E000 flame, U+E001 snowflake, U+E002 droplet, U+E003 fan
        ag = []
        for fn in (flame, snowflake, droplet, fan):
            c = Canvas(64)
            fn(c)
            ag.append((pack4(c.result()), 64 * 16, 64, 64, 0, 0))
        emit_font(f, "hvac_anim", ag, [0xE000, 0xE001, 0xE002, 0xE003], 64, 0, 0, 1)
        f.write("#endif /* HVAC_USE_ICON_FONT */\n\n")
        for name, px in (("hvac_big", 104), ("hvac_mid", 72)):
            bcps, bglyphs, lh, bl = big_font_glyphs(px)
            emit_font(f, name, bglyphs, bcps, lh, bl, -4, max(2, px // 16))
    if prev:
        sheet = Image.new("L", (len(imgs) * 56 + 16, 64), 0)
        for i, im in enumerate(imgs):
            sheet.paste(im, (16 + i * 56, 14))
        sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(prev)
    print("written", out)


main()
