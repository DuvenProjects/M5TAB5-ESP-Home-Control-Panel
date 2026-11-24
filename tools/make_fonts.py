#!/usr/bin/env python3
"""
make_fonts.py - generates fonts_ui.c : LVGL 9 bitmap fonts (4 bpp, uncompressed)

  font_ui_14 ... font_ui_48 : every character used by the project
     - Latin (ASCII, Turkish, German, French, Spanish letters, degree, bullet)
       rendered with Inter Medium
     - Chinese characters rendered with Noto Sans CJK SC Medium
       (sizes 14..32 contain all of them; 40 and 48 only the characters of the
        strings listed on the "// BIG:" line of lang.cpp)
     - LV_SYMBOL_* icons are NOT included: each font falls back to the built-in
       lv_font_montserrat_<size> (enable those sizes in lv_conf.h)

The character set is collected from every string literal in the sketch folder
(*.cpp, *.h, *.ino), so after adding or changing translations in lang.cpp just
run this script again.

Usage:  python3 tools/make_fonts.py            (run from the sketch folder)
Needs:  Pillow, fontTools, Inter-Medium.otf, NotoSansCJK-Medium.ttc
"""
import glob
import os
import re
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

LATIN_TTF = "/usr/share/fonts/opentype/inter/Inter-Medium.otf"
CJK_TTC = "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc"
CJK_INDEX = 2                      # face index of "Noto Sans CJK SC" in the collection
SIZES = [14, 16, 20, 24, 28, 32, 40, 48]

# Big digital clock of the Home page: digits and colon only, tabular digits
# ("tnum") so that the time does not move when the digits change
CLOCK_TTF = "/usr/share/fonts/opentype/inter/Inter-Light.otf"
CLOCK_SIZE = 100
CLOCK_CHARS = "0123456789:-"         # "-" for "--:--" before the time is set
CJK_FULL_MAX = 32                  # sizes above this only get the BIG characters

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.dirname(HERE)
OUT = os.path.join(SKETCH, "fonts_ui.c")

LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def decode_literal(s):
    """Decodes a C string literal body (UTF-8 text, \\xHH, \\n, \\\\, \\")."""
    out = bytearray()
    i = 0
    raw = s.encode("utf-8")
    while i < len(raw):
        ch = raw[i]
        if ch == 0x5C and i + 1 < len(raw):            # backslash
            n = raw[i + 1]
            if n == ord("x"):
                hexd = raw[i + 2:i + 4]
                try:
                    out.append(int(hexd, 16))
                    i += 4
                    continue
                except ValueError:
                    pass
            out.append({ord("n"): 10, ord("t"): 9}.get(n, n))
            i += 2
            continue
        out.append(ch)
        i += 1
    return out.decode("utf-8", errors="ignore")


def collect():
    chars = set(chr(c) for c in range(0x20, 0x7F))
    chars |= set("°•–…")
    big = set()
    files = glob.glob(os.path.join(SKETCH, "*.cpp")) + glob.glob(os.path.join(SKETCH, "*.h")) + \
        glob.glob(os.path.join(SKETCH, "*.ino"))
    for fn in files:
        text = open(fn, encoding="utf-8").read()
        for m in LIT.finditer(text):
            chars |= set(decode_literal(m.group(1)))
    # BIG strings (lang.cpp)
    lang = open(os.path.join(SKETCH, "lang.cpp"), encoding="utf-8").read()
    m = re.search(r"// BIG:(.*)", lang)
    big_ids = m.group(1).split() if m else []
    for bid in big_ids:
        row = re.search(r"/\* %s \*/ \{(.*)\}," % re.escape(bid), lang)
        if row:
            for lm in LIT.finditer(row.group(1)):
                big |= set(decode_literal(lm.group(1)))
    keep = lambda c: ord(c) >= 0x20 and not (0xE000 <= ord(c) <= 0xF8FF) and c not in "\n\t"
    return sorted(c for c in chars if keep(c)), set(c for c in big if keep(c))


def pack4(img):
    nib = [round(v * 15 / 255) for v in img.tobytes()]
    if len(nib) % 2:
        nib.append(0)
    return bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))


def render(font, ch, features=None):
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls", features=features)
    w, h = x1 - x0, y1 - y0
    adv = font.getlength(ch, features=features)
    if w <= 0 or h <= 0:
        return (b"", round(adv * 16), 0, 0, 0, 0)
    ew = w + (w % 2)                                   # even width: rows stay byte aligned
    im = Image.new("L", (ew, h), 0)
    ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls", features=features)
    return (pack4(im), round(adv * 16), ew, h, x0, -y1)


def emit(f, name, cps, glyphs, line_h, base_line, size):
    f.write("/* ---- %s : %d glyphs ---- */\n" % (name, len(cps)))
    data = b"".join(g[0] for g in glyphs)
    f.write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_bitmap[] = {\n" % name)
    for i in range(0, len(data), 24):
        f.write("    " + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
    if not data:
        f.write("    0\n")
    f.write("};\n\n")

    f.write("static const lv_font_fmt_txt_glyph_dsc_t %s_glyph_dsc[] = {\n" % name)
    f.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
    off = 0
    for cp, g in zip(cps, glyphs):
        f.write("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}, /* U+%04X */\n"
                % (off, g[1], g[2], g[3], g[4], g[5], cp))
        off += len(g[0])
    f.write("};\n\n")

    # Latin: contiguous ranges; everything >= U+2000 (bullet, CJK, fullwidth
    # punctuation) in one sparse table (binary searched by LVGL)
    cmaps = []
    sparse = [(i, cp) for i, cp in enumerate(cps) if cp >= 0x2000]
    for i, cp in enumerate(cps):
        if cp >= 0x2000:
            continue
        if cmaps and cp == cmaps[-1][0] + cmaps[-1][1]:
            cmaps[-1][1] += 1
        else:
            cmaps.append([cp, 1, i + 1])
    if sparse:
        first = sparse[0][1]
        f.write("static const uint16_t %s_unicode_list[] = {\n    " % name)
        f.write(", ".join("0x%04x" % (cp - first) for _, cp in sparse))
        f.write("\n};\n\n")
    f.write("static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {\n" % name)
    for start, length, gid in cmaps:
        f.write("    {.range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
                "     .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n"
                "     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},\n" % (start, length, gid))
    if sparse:
        first, last = sparse[0][1], sparse[-1][1]
        f.write("    {.range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
                "     .unicode_list = %s_unicode_list, .glyph_id_ofs_list = NULL, .list_length = %d,\n"
                "     .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},\n"
                % (first, last - first + 1, sparse[0][0] + 1, name, len(sparse)))
    f.write("};\n\n")
    ncm = len(cmaps) + (1 if sparse else 0)
    assert ncm < 512, "too many cmaps"
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
    .underline_position = -2,
    .underline_thickness = %(ut)d,
    .dsc = &%(n)s_dsc,
    .fallback = %(fb)s,
    .user_data = NULL,
};

""" % dict(n=name, cn=ncm, lh=line_h, bl=base_line, ut=max(1, size // 16),
           fb=("&lv_font_montserrat_%d" % size) if size in SIZES else "NULL"))
    return len(data)


HEADER = """/*
 * fonts_ui.c  -  GENERATED FILE (tools/make_fonts.py), do not edit by hand.
 * UI fonts for every language of the application. LVGL 9, 4 bpp, uncompressed.
 *   Latin: Inter Medium (SIL Open Font License)
 *   Chinese: Noto Sans CJK SC Medium (SIL Open Font License)
 *   Fallback: built-in Montserrat of the same size (LV_SYMBOL_* icons)
 *   font_ui_clock: Inter Light 100 px, digits + colon (tabular), Home clock
 */
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
    chars, big = collect()
    latin_cmap = TTFont(LATIN_TTF).getBestCmap()
    cjk_cmap = TTFont(CJK_TTC, fontNumber=CJK_INDEX).getBestCmap()
    total = 0
    missing = set()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(HEADER)
        for size in SIZES:
            lat = ImageFont.truetype(LATIN_TTF, size)
            cjk = ImageFont.truetype(CJK_TTC, size, index=CJK_INDEX)
            asc, desc = lat.getmetrics()
            cps, glyphs = [], []
            for ch in chars:
                cp = ord(ch)
                if cp in latin_cmap:
                    font = lat
                elif cp in cjk_cmap:
                    if size > CJK_FULL_MAX and ch not in big:
                        continue
                    font = cjk
                else:
                    missing.add(ch)
                    continue
                cps.append(cp)
                glyphs.append(render(font, ch))
            total += emit(f, "ui_%d" % size, cps, glyphs, asc + desc, desc, size)
        # digital clock font (no fallback)
        clk = ImageFont.truetype(CLOCK_TTF, CLOCK_SIZE)
        asc, desc = clk.getmetrics()
        cps = sorted(ord(c) for c in CLOCK_CHARS)
        glyphs = [render(clk, chr(cp), features=["tnum"]) for cp in cps]
        total += emit(f, "ui_clock", cps, glyphs, asc + desc, desc, CLOCK_SIZE)
    if missing:
        print("WARNING: no glyph for", " ".join("U+%04X" % ord(c) for c in sorted(missing)))
    print("written %s  (%d characters, %.0f KB bitmaps)" % (OUT, len(chars), total / 1024))


if __name__ == "__main__":
    sys.exit(main())
