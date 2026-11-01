#pragma once
// TraceCraft T-Display S3 AMOLED dashboard — 3 screens (tire temp / pit limiter /
// fuel), tap the touch panel to page. See amoleddash.cpp. Pure LVGL; navigation
// (touch polling) is driven from src/main.cpp via amoleddash_advance().
struct _lv_obj_t;
_lv_obj_t* amoleddash_setup();          // build all screens, load screen 1, return it
void amoleddash_tick(bool waitingForData);  // update every screen from the telemetry store
void amoleddash_advance();              // switch to the next screen (wraps) — called on a tap
// Desired panel brightness 0..255 for THIS frame. The "flash" on the tire/pit/fuel
// screens is a brightness pulse (tear-free) rather than a full-screen repaint;
// src/main.cpp applies this via board::amoled().setBrightness() when it changes.
unsigned char amoleddash_brightness();
