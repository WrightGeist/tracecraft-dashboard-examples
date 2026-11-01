#pragma once
#include <lvgl.h>

// T4-S3 deck — TIRES + BRAKES page. A per-corner diagnostic replicating the
// local (recorder) Diagnostic dashboard's Tires + brakes panel: tread-band tire
// temps (I/M/O where the sim publishes them), pressure in psi, tire wear,
// brake temps, and the LMU-only flat / wheel-detached states.
lv_obj_t* t4tires_setup();
void t4tires_tick(bool waitingForData);
