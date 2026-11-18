#!/usr/bin/env python3
"""
make_lang.py - generates lang.h and lang.cpp from tools/strings.py

Edit the translation table in tools/strings.py, then run (from the sketch folder):
    python3 tools/make_lang.py
    python3 tools/make_fonts.py      (adds any new character to the fonts)
The script checks that every language uses the same printf conversions.
"""
import os
import re
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from strings import S, BIG, LANG_NAMES
OUT = os.path.join(os.path.dirname(HERE), '')
ORDER = [1, 0, 2, 3, 4, 5]          # data columns EN,TR,DE,FR,ES,ZH -> enum order TR,EN,DE,FR,ES,ZH

# ---- validate: unique ids, same printf conversions in every language
ids = [r[0] for r in S]
assert len(ids) == len(set(ids)), [i for i in ids if ids.count(i) > 1]
conv = re.compile(r'%(?:%|[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h)?[diuxXfsc])')
bad = 0
for r in S:
    ref = [c for c in conv.findall(r[1]) if c != '%%']
    for k, txt in enumerate(r[1:]):
        got = [c for c in conv.findall(txt) if c != '%%']
        if got != ref:
            print('FORMAT MISMATCH', r[0], k, ref, got); bad += 1
        # stray single % that is not a conversion
        if re.search(r'%(?!%|[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h)?[diuxXfsc])', txt.replace('%%', '')):
            if r[0].startswith('FMT') or r[0].startswith('B_FMT'):
                print('STRAY %', r[0], k, txt); bad += 1
assert not bad

def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

h = ['// ============================================================================',
     '//  lang.h  -  User interface languages, translated strings and UI fonts',
     '//  GENERATED from tools/strings.py by tools/make_lang.py - edit the table',
     '//  there (or lang.cpp directly) and re-run tools/make_fonts.py so that every',
     '//  character used by the translations exists in the fonts.',
     '// ============================================================================',
     '#pragma once',
     '#include <stdint.h>',
     '#include <stddef.h>',
     '#include <time.h>',
     '#include <lvgl.h>',
     '',
     '// Order matches the language list of Settings > Localization',
     'enum Language : uint8_t { LANG_TR = 0, LANG_EN, LANG_DE, LANG_FR, LANG_ES, LANG_ZH, LANG_COUNT };',
     '#define LANG_DEFAULT LANG_EN',
     '',
     'enum StrId : uint16_t {']
for i in ids:
    h.append('  S_%s,' % i)
h += ['  S_COUNT',
      '};',
      '',
      'const char* tr(StrId id);                 // text of the active language',
      'void        lang_set(uint8_t lang);        // switches the active language',
      'uint8_t     lang_get();',
      'const char* lang_name(uint8_t lang);       // native language name ("Türkçe" ...)',
      'const char* lang_day_short(int wday);      // 0 = Sunday',
      'const char* lang_day_long(int wday);',
      'const char* lang_month(int mon);           // 0 = January',
      'void        lang_format_long_date(char* out, size_t n, const struct tm* t);  // "Friday, November 6"',
      '',
      '// ---- UI fonts (fonts_ui.c): Latin incl. Turkish/German/French/Spanish',
      '//      letters + Chinese glyphs. LV_SYMBOL_* icons come from the built-in',
      '//      Montserrat font of the same size through the font fallback chain.',
      '#ifdef __cplusplus',
      'extern "C" {',
      '#endif']
SIZES = [14, 16, 20, 24, 28, 32, 40, 48]
for s in SIZES:
    h.append('LV_FONT_DECLARE(font_ui_%d)' % s)
h.append('LV_FONT_DECLARE(font_ui_clock)          // digits + colon, 100 px (Home clock)')
h += ['#ifdef __cplusplus', '}', '#endif', '']
for s in SIZES:
    h.append('#define FONT_%d (&font_ui_%d)' % (s, s))
h.append('#define FONT_CLOCK (&font_ui_clock)')
h.append('')
open(OUT + 'lang.h', 'w').write('\n'.join(h))

c = ['// ============================================================================',
     '//  lang.cpp  -  Translation table (TR, EN, DE, FR, ES, ZH)',
     '//  Every row: /* S_ID */ {Turkish, English, German, French, Spanish, Chinese}',
     '//  Printf formats must use the same conversions in the same order in every',
     '//  language. After adding characters run tools/make_fonts.py.',
     '// ============================================================================',
     '#include <stdio.h>',
     '#include "lang.h"',
     '',
     'static uint8_t s_lang = LANG_DEFAULT;',
     '',
     'static const char* const LANG_NAMES[LANG_COUNT] = {' + ', '.join(cstr(n) for n in LANG_NAMES) + '};',
     '',
     'static const char* const STR[S_COUNT][LANG_COUNT] = {']
for r in S:
    vals = [r[1 + k] for k in ORDER]
    c.append('  /* S_%s */ {%s},' % (r[0], ', '.join(cstr(v) for v in vals)))
c += ['};',
      '',
      '// Strings that are also drawn with the 40 / 48 px fonts:',
      '// BIG: ' + ' '.join('S_' + b for b in BIG),
      '',
      'const char* tr(StrId id) {',
      '  if (id >= S_COUNT) return "";',
      '  const char* s = STR[id][s_lang];',
      '  return (s && s[0]) ? s : STR[id][LANG_EN];',
      '}',
      '',
      'void lang_set(uint8_t lang) { s_lang = (lang < LANG_COUNT) ? lang : LANG_DEFAULT; }',
      'uint8_t lang_get() { return s_lang; }',
      'const char* lang_name(uint8_t lang) { return lang < LANG_COUNT ? LANG_NAMES[lang] : ""; }',
      '',
      'const char* lang_day_short(int wday) { return tr((StrId)(S_DAY_SUN + ((wday % 7 + 7) % 7))); }',
      'const char* lang_day_long(int wday)  { return tr((StrId)(S_DAYL_SUN + ((wday % 7 + 7) % 7))); }',
      'const char* lang_month(int mon)      { return tr((StrId)(S_MON_JAN + ((mon % 12 + 12) % 12))); }',
      '',
      '// Long date in the word order of the active language',
      'void lang_format_long_date(char* out, size_t n, const struct tm* t) {',
      '  const char* d = lang_day_long(t->tm_wday);',
      '  const char* m = lang_month(t->tm_mon);',
      '  switch (s_lang) {',
      '    case LANG_TR: snprintf(out, n, "%s, %d %s", d, t->tm_mday, m); break;',
      '    case LANG_DE: snprintf(out, n, "%s, %d. %s", d, t->tm_mday, m); break;',
      '    case LANG_FR: snprintf(out, n, "%s %d %s", d, t->tm_mday, m); break;',
      '    case LANG_ES: snprintf(out, n, "%s, %d de %s", d, t->tm_mday, m); break;',
      '    case LANG_ZH: snprintf(out, n, "%s%d\u65e5 %s", m, t->tm_mday, d); break;',
      '    default:      snprintf(out, n, "%s, %s %d", d, m, t->tm_mday); break;',
      '  }',
      '}',
      '']
open(OUT + 'lang.cpp', 'w').write('\n'.join(c))
print(len(ids), 'strings')
