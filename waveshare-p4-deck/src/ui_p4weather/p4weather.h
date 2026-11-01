#pragma once
// TraceCraft P4 deck — weather & track-conditions page (see p4weather.cpp).
struct _lv_obj_t;
_lv_obj_t* p4weather_setup();
void p4weather_tick(bool waitingForData);
