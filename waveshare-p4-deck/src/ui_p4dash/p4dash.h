#pragma once

// Full-screen 1280x720 landscape race dash for the Waveshare ESP32-P4 5" panel —
// one page of the P4 deck (src/main.cpp). Pure firmware, no EEZ. p4dash_setup() after
// lvglport::init() builds the dash on its OWN screen and returns it (the caller
// loads it, or pages between it and its siblings); p4dash_tick() every loop —
// cheap when the screen isn't active, so a pager can keep every page current.
struct _lv_obj_t;
_lv_obj_t* p4dash_setup();
void p4dash_tick(bool waitingForData);
