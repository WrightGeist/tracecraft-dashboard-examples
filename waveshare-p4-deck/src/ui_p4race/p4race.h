#pragma once
#include <lvgl.h>

// P4 deck — RACE page (1280x720): mirrored RPM light
// banks meeting at the top center (fill inward, flash bright blue at 93% of
// max), the gear huge and centered, the class position beneath it, cyan TC
// lamps down both edges while traction control intervenes, and the current
// flag framing the ENTIRE screen as a thick colored border.
lv_obj_t* p4race_setup();
void p4race_tick(bool waitingForData);
