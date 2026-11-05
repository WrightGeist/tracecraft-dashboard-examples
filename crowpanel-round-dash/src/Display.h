#pragma once
#include <cstdint>

// The round panel and the three screens. See Display.cpp.

// Bring up the panel and LVGL and build the screens. Call once.
void display_begin(void);

// Show the next screen. Returns its number (0-2), for dialpad_set_page().
uint8_t display_next_page(void);

// Draw the latest telemetry. With `waiting` (no recorder sending), the dials show "--"
// rather than zeros nobody sent.
void display_update(bool waiting);

// The boot splash: the link phase, and the recorder's verdict on this firmware.
void display_splash_tick(uint8_t phase, uint8_t verdict, const char* latestVer);
