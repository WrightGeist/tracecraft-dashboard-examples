/**
 * TraceCraft AMOLED dash (Lilygo T-Display S3 AMOLED, 536x240): three pages —
 * tire temperatures, pit, fuel — and a tap on the screen moves to the next.
 * Pure firmware, no EEZ.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/t_display_s3_amoled.h"
#include "tc_generated.h"             // channel registry
#include "ui_amoleddash/amoleddash.h"

using namespace tracecraft;

#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft AMOLED Dash"
#endif

static App app;

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  lv_screen_load(amoleddash_setup());
  app.ready();
}

void loop() {
  app.loop();
  amoleddash_tick(app.waiting());   // updates all three pages
  {
    // Tap the touch panel to advance to the next screen (down-edge = one tap).
    // Polled here in the app loop (NOT LVGL's read_cb) so the CST816T I2C read is safe.
    int16_t tx, ty;
    static bool s_touchWas = false;
    const bool down = board::touchPoint(tx, ty);
    if (down && !s_touchWas) amoleddash_advance();
    s_touchWas = down;
  }
  {
    // Apply the dash's desired brightness (its "flash" is a tear-free brightness
    // pulse, not a full-frame repaint). Only on change — one MIPI 0x51 command.
    static uint8_t s_bri = 200;   // AmoledRenderer::init default
    const uint8_t b = amoleddash_brightness();
    if (b != s_bri) { s_bri = b; board::amoled().setBrightness(b); }
  }
}
