// ============================================================================
//  clock.h  -  Clock styles of the Home clock tile + style selection page
//
//  10 styles: 0..4 digital (Classic, Neon, Flip cards, LCD, Stacked),
//             5..9 analog  (Classic, Minimal, Roman, Sport, Hybrid).
//  The selected style is stored in NVS (namespace "clock", key "theme").
//  Home: tap the clock tile -> clock style page -> tap a style.
// ============================================================================
#pragma once
#include <lvgl.h>
#include <stdint.h>

#define CLOCK_THEME_COUNT  10
#define CLOCK_FACE_W       364                    // content size of the Home clock tile
#define CLOCK_FACE_H       308

uint8_t     clock_theme();                        // selected style (loaded from NVS once)
void        clock_set_theme(uint8_t theme);       // select + save to NVS
const char* clock_theme_name(uint8_t theme);      // translated name

lv_obj_t*   clock_face_create(lv_obj_t* parent, uint8_t theme);   // CLOCK_FACE_W x CLOCK_FACE_H
void        clock_face_update(lv_obj_t* face);    // call every second

lv_obj_t*   clock_create();                       // style selection page (PAGE_CLOCK)
