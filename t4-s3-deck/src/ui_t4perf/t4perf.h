// T4-S3 deck — PERFORMANCE page. See t4perf.cpp for the layout notes.
#pragma once
#include <lvgl.h>

// Builds the page on its own screen and returns it (the deck pager loads it).
lv_obj_t* t4perf_setup();

// Sample + redraw. `waiting` = no recorder streaming yet; blanks every row
// rather than plotting the zeros an unsent channel decodes to.
void t4perf_tick(bool waiting);
