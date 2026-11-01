/**
 * TraceCraft EEZ dashboard — the dashboard you design in EEZ Studio, with ZERO
 * firmware edits: every EEZ "global variable" you bind a widget to is a telemetry
 * channel, wired up by the generated get_var_* glue.
 *
 * The EEZ project exports to src/ui/, sized for the T4-S3's 450x600. Run EEZ Build
 * after changing it.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/t4_s3.h"
#include "tc_generated.h"   // channel registry + EEZ get_var_* glue

#include "ui/ui.h"

using namespace tracecraft;

#include "eez_effects/eez_effects.h"

#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft Dash"
#endif

static App app;

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  ui_init();                    // build the EEZ-Studio screen
#ifndef TRACECRAFT_BOARD_TDISPLAY_S3_LONG
  tuneDashGauges();             // fuel strip == arc-track thickness (450x600 export)
#endif
#ifdef TRACECRAFT_EFFECT_RPM_ARC
  captureArcColors();           // remember the arc's design colors for the effect
#endif
#ifdef TRACECRAFT_EFFECT_SHIFT_LIGHTS
  setupShiftLights();           // draw the LED strip into the `shift_lights` box
#endif
#ifdef TRACECRAFT_EFFECT_CAUTION_FLAG
  setupCautionFlag();           // draw the flag strips into the `caution_flag` box
#endif
#ifdef TRACECRAFT_EFFECT_TELLTALES
  setupTellTales();             // lamps flanking the speed readout
#endif
  app.ready();
}

void loop() {
  app.loop();
  ui_tick();                    // EEZ pushes channel values into the bound widgets
#ifdef TRACECRAFT_EFFECT_RPM_ARC
  applyArcLimitEffect();        // redline color + pulse (presentation-only LVGL)
#endif
#ifdef TRACECRAFT_EFFECT_SHIFT_LIGHTS
  applyShiftLights();           // RPM LED strip (green->amber->red, flash at limit)
#endif
#ifdef TRACECRAFT_EFFECT_CAUTION_FLAG
  applyCautionFlag();           // waving yellow flag while `caution` is true
#endif
#ifdef TRACECRAFT_EFFECT_FUEL_BAR
  applyFuelBar();               // low-fuel color zones + critical flash
#endif
#ifdef TRACECRAFT_EFFECT_TIRE_TEMPS
  applyTireTemps();             // per-corner tire temps + temperature colors
#endif
#ifdef TRACECRAFT_EFFECT_TELLTALES
  applyTellTales();             // turn/lights/wipers + TC/ABS lamps by the speed readout
#endif
}
