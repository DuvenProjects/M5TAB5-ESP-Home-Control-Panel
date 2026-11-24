// ============================================================================
//  udp_alarm.h  -  Fire / smoke / gas alarm unit (UDP)  -  no LVGL code
//
//  A separate alarm unit sends its detector states as a UDP packet:
//      {"id":"s2mini_alarm","seq":7,"fire":0,"smoke":1,"gas":0}
//  (values 0 / 1 or false / true). It should send a packet on every change
//  and at least every 30 s as a heartbeat, so the Tab5 can see that it is alive.
//
//  Two independent alarm groups (Alarm page + Blynk):
//    - Motion alarm (PIR, intrusion)  : g_data.alarm_armed
//    - Fire / smoke / gas alarm       : udp_alarm_safety_armed()  (NVS, default on)
//  So the fire / smoke / gas alarm can stay on while somebody is at home and
//  the motion alarm is off. One "disarm all" (PIN on the Tab5, button in Blynk)
//  switches both groups off and stops a ringing alarm.
// ============================================================================
#pragma once
#include <stdint.h>
#include "udp_th.h"          // UDP_PORT

#define UDP_ALARM_ID          "s2mini_alarm"   // only packets with this "id" are accepted
#define UDP_ALARM_PORT        UDP_PORT         // 4210: shares the socket of udp_th.cpp;
                                               // another number opens an own socket
#define UDP_ALARM_OFFLINE_MS  90000UL          // no packet for this long -> unit offline

enum AlarmUnitState : uint8_t {
  ALARM_UNIT_WAITING = 0,      // no packet received yet
  ALARM_UNIT_ONLINE,
  ALARM_UNIT_OFFLINE           // no packet for UDP_ALARM_OFFLINE_MS
};

struct SafetyState {
  bool           fire, smoke, gas;   // detector states of the last packet
  AlarmUnitState unit;
  uint32_t       age_s;              // seconds since the last packet
  uint32_t       packets;            // valid packets
  char           remote[16];         // IP of the alarm unit
};

void               udp_alarm_begin();             // ui.init(), loads the switch from NVS
void               udp_alarm_update();            // every 100 ms (ui.cpp, main thread)

// Called by udp_th.cpp for packets on the shared port. true = it was an alarm packet.
bool               udp_alarm_packet(const char* json, const char* remote);

const SafetyState& udp_alarm_state();
bool               udp_alarm_safety_armed();      // fire / smoke / gas alarm switched on
void               udp_alarm_set_safety_armed(bool on);   // saved to NVS
