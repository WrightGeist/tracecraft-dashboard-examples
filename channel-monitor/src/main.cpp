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
// Recycle only visible labels so the complete catalog fits the small S3 LVGL pool.
static constexpr int kVisibleRows = 40;
static constexpr int kRowHeight = 34;
static lv_obj_t* s_rows[kVisibleRows];
static lv_obj_t* s_list = nullptr;
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
  lv_label_set_text(s_hint, "channel monitor - scroll for more");
  lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 48);

  const int height = lv_display_get_vertical_resolution(nullptr);
  s_list = lv_obj_create(scr);
  lv_obj_set_size(s_list, LV_PCT(100), height - 86);
  lv_obj_set_pos(s_list, 0, 86);
  lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_list, 0, 0);
  lv_obj_set_style_pad_all(s_list, 0, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_t* spacer = lv_obj_create(s_list);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_size(spacer, 1, tcStore.count() * kRowHeight);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < kVisibleRows; i++) {
    lv_obj_t* row = lv_label_create(s_list);
    lv_obj_set_style_text_color(row, lv_color_white(), 0);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(row, 12, i * kRowHeight);
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
        : "channel monitor - scroll for more");
    lv_obj_set_style_text_color(s_hint,
        waiting ? lv_palette_main(LV_PALETTE_AMBER) : lv_palette_main(LV_PALETTE_GREY), 0);
  }
  static char s_rowShown[kVisibleRows][80];
  char buf[80];
  int first = lv_obj_get_scroll_y(s_list) / kRowHeight;
  if (first < 0) first = 0;
  for (int slot = 0; slot < kVisibleRows; slot++) {
    const int index = first + slot;
    if (index >= tcStore.count()) {
      lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const uint8_t i = (uint8_t)index;
    lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_rows[slot], 12, index * kRowHeight);
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
    if (strcmp(buf, s_rowShown[slot]) != 0) {
      strcpy(s_rowShown[slot], buf);
      lv_label_set_text(s_rows[slot], buf);
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
