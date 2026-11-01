#pragma once
#include <lvgl.h>

// P4 deck — FPS page (1280x720). The game's frame rate as a hero readout and
// a big ~60 s rolling trace. Reads game_fps (ETW-measured on the host, works
// on every sim) — live-only, never recorded.
lv_obj_t* p4fps_setup();
void p4fps_tick(bool waitingForData);
