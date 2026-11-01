#pragma once

// Timing-focused 1280x720 dash for the Waveshare ESP32-P4 5" panel — one page
// of the P4 deck (src/main.cpp). Sibling of
// the ui_p4dash race-dash page: same RPM shift-light row + gear box, but the flanks
// carry lap/sector timing — current / last / best lap, per-sector times and
// deltas (clocked on-device from the streamed lap clock + sector index; see
// p4timing.cpp). Builds on its OWN screen and returns it; tick every loop even
// when the screen isn't active so the sector clock never goes stale.
struct _lv_obj_t;
_lv_obj_t* p4timing_setup();
void p4timing_tick(bool waitingForData);
