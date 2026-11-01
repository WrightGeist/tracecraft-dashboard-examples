#pragma once
#include <lvgl.h>

// P4 deck — G-FORCE page (1280x720). The friction circle (lateral vs
// longitudinal) on the left, and a SEPARATE vertical-G column on the right.
//
// Two indicators rather than one on purpose: the circle is 2D because grip is
// 2D, and it answers "am I using the tires". Vertical answers a different
// question — "what is the track doing to the car" (curbs, compressions, going
// light over a crest) — so folding it into the circle would mix two readings
// and make both harder to parse.
//
// Reads accel_lat / accel_lon / accel_vert.
lv_obj_t* p4gforce_setup();
void p4gforce_tick(bool waitingForData);
