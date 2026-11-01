/**
 * TraceCraft channel monitor — a live, schema-agnostic list of every channel.
 *
 * No EEZ project needed: flash this first to prove the HID pipeline works on your
 * board (any board in src/boards/), then design
 * your own dashboard.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/Board.h"
#ifdef TRACECRAFT_BOARD_WAVESHARE_P4_LCD5
  #include "boards/waveshare_p4_lcd5_controls.h"
#endif
#include "tc_generated.h"   // channel registry

using namespace tracecraft;

// The name the recorder lists this dashboard under (max 31 chars). Set your own
// in platformio.ini: build_flags = -D TRACECRAFT_DEVICE_NAME='"My Dash"'
#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft Monitor"
#endif

static App app;
#ifdef TRACECRAFT_BOARD_WAVESHARE_P4_LCD5
static board::P4Controls controls(app);
#endif

// --- A live, schema-agnostic list of every channel ---------------------------
// Proves HID transport + decode + render without EEZ. Takes the screen to build
// on, so a deck could host it as a page.
static lv_obj_t* s_rows[TRACECRAFT_MAX_CHANNELS];
static lv_obj_t* s_hint = nullptr;

static void buildFallbackUI(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t* title = lv_label_create(scr);
  lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_CYAN), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_label_set_text(title, "TraceCraft");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  s_hint = lv_label_create(scr);
  lv_obj_set_style_text_color(s_hint, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_label_set_text(s_hint, "channel monitor - design your dash in EEZ Studio");
  lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 48);

  for (uint8_t i = 0; i < tcStore.count(); i++) {
    lv_obj_t* row = lv_label_create(scr);
    lv_obj_set_style_text_color(row, lv_color_white(), 0);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_24, 0);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 20, 90 + i * 34);
    lv_label_set_text(row, "");
    s_rows[i] = row;
  }
}

static void tickFallbackUI() {
  // Idle state: tell the user we're connected but no telemetry is flowing yet,
  // rather than freezing on stale (or zero) numbers. Change-cached: an
  // unconditional set_text on 61 rows every loop invalidated the whole list
  // continuously and dragged the idle page to ~12 FPS.
  const bool waiting = app.waiting();
  static int8_t s_hintShown = -1;
  if (s_hint && (int8_t)waiting != s_hintShown) {
    s_hintShown = (int8_t)waiting;
    lv_label_set_text(s_hint, waiting
        ? "waiting for telemetry - start your sim"
        : "channel monitor - design your dash in EEZ Studio");
    lv_obj_set_style_text_color(s_hint,
        waiting ? lv_palette_main(LV_PALETTE_AMBER) : lv_palette_main(LV_PALETTE_GREY), 0);
  }
  static char s_rowShown[TRACECRAFT_MAX_CHANNELS][80];
  char buf[80];
  for (uint8_t i = 0; i < tcStore.count(); i++) {
    const ChannelDef& d = tcStore.defs()[i];
    // A channel the recorder says it has no value for (PROTOCOL.md §4.8) shows a
    // dash rather than a 0 the sim never published. With an older recorder that
    // cannot say, has() is true for every channel and the row shows its value as
    // before. ASCII on purpose: the built-in Montserrat fonts have no em app.dash().
    if (!tcStore.has(i) && tcStore.presenceKnown()) {
      snprintf(buf, sizeof(buf), "%s  -", d.name);
    } else switch (d.type) {
      case Ch_Int:   snprintf(buf, sizeof(buf), "%s  %ld", d.name, (long)tcStore.getInt(i)); break;
      case Ch_Float: snprintf(buf, sizeof(buf), "%s  %.2f", d.name, tcStore.getFloat(i)); break;
      case Ch_Bool:  snprintf(buf, sizeof(buf), "%s  %s", d.name, tcStore.getBool(i) ? "ON" : "off"); break;
      case Ch_Str:   snprintf(buf, sizeof(buf), "%s  %s", d.name, tcStore.getStr(i)); break;
    }
    if (strcmp(buf, s_rowShown[i]) != 0) {
      strcpy(s_rowShown[i], buf);
      lv_label_set_text(s_rows[i], buf);
    }
  }
}

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  buildFallbackUI(lv_screen_active());
  app.ready();
#ifdef TRACECRAFT_BOARD_WAVESHARE_P4_LCD5
  controls.bootCue();
#endif
}

void loop() {
#ifdef TRACECRAFT_BOARD_WAVESHARE_P4_LCD5
  controls.serviceTouch();
#endif
  app.loop();
#ifdef TRACECRAFT_BOARD_WAVESHARE_P4_LCD5
  controls.readyCue();
#endif
  tickFallbackUI();
}
