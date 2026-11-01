/**
 * TraceCraft G-meter (Lilygo T-Display S3, 320x170): an elegant lateral-first
 * G-meter. Pure firmware, no EEZ.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/tdisplay_s3.h"
#include "tc_generated.h"     // channel registry
#include "ui_gmeter/gmeter.h"

using namespace tracecraft;

#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft G-Meter"
#endif

static App app;

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  gmeter_setup(lv_screen_active());
  app.ready();
}

void loop() {
  app.loop();
  gmeter_tick(app.waiting());                                // glide the marker + fills
  gmeter_rate(app.dash().framesApplied(), app.hid().frameSeq());   // measured link rate
}
