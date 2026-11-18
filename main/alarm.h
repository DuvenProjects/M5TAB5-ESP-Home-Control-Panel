// ============================================================================
//  alarm.h  -  Alarm page (arm / disarm, PIN keypad, alarm state)
//  To trigger an alarm from anywhere:  ui_alarm_trigger("Front door");  (ui.h)
// ============================================================================
#pragma once
#include <lvgl.h>

lv_obj_t* alarm_create();
