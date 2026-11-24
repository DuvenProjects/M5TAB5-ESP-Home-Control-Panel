/* ============================================================================
 *  lv_conf.h  -  minimal configuration for LVGL 9.2.x (M5Stack Tab5)
 *  COPY THIS FILE TO:  Arduino/libraries/lv_conf.h
 *  (NEXT TO the lvgl folder, not inside it). Every option that is not
 *  defined here gets its default value from lv_conf_internal.h.
 * ==========================================================================*/
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH            16

/* malloc / sprintf: C library -> PSRAM can be used, %f works */
#define LV_USE_STDLIB_MALLOC      LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING      LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF     LV_STDLIB_CLIB
#define LV_SPRINTF_USE_FLOAT      1

#define LV_USE_OS                 LV_OS_NONE
#define LV_DEF_REFR_PERIOD        16
#define LV_DPI_DEF                160
#define LV_USE_LOG                0
#define LV_USE_ASSERT_NULL        1
#define LV_USE_ASSERT_MALLOC      1

/* Built-in Montserrat fonts: used as the fallback of the UI fonts (fonts_ui.c)
 * for the LV_SYMBOL_* icons - every size below must stay enabled */
#define LV_FONT_MONTSERRAT_14     1
#define LV_FONT_MONTSERRAT_16     1
#define LV_FONT_MONTSERRAT_20     1
#define LV_FONT_MONTSERRAT_24     1
#define LV_FONT_MONTSERRAT_28     1
#define LV_FONT_MONTSERRAT_32     1
#define LV_FONT_MONTSERRAT_40     1
#define LV_FONT_MONTSERRAT_48     1
#define LV_FONT_DEFAULT           &lv_font_montserrat_16

/* Widgets used by the project (enabled by default, listed to be sure) */
#define LV_USE_ARC                1
#define LV_USE_BAR                1
#define LV_USE_BUTTON             1
#define LV_USE_BUTTONMATRIX       1
#define LV_USE_CHART              1
#define LV_USE_LABEL              1
#define LV_USE_LINE               1
#define LV_USE_SCALE              1
#define LV_USE_SLIDER             1
#define LV_USE_SWITCH             1

#define LV_USE_THEME_DEFAULT      1
#define LV_THEME_DEFAULT_DARK     1

#endif /* LV_CONF_H */
#endif
