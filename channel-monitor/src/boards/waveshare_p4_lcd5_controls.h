#pragma once
// What every UI on the Waveshare P4 does beyond tracecraft::App: tap for brightness,
// and the boot and ready blips. A dashboard on this board keeps one beside its App:
//
//   static App app;
//   static board::P4Controls controls(app);
//   void setup() { app.begin(...); /* build your UI */ app.ready(); controls.bootCue(); }
//   void loop()  { controls.serviceTouch(); app.loop(); controls.readyCue(); /* tick */ }
#include <TraceCraftApp.h>
#include "waveshare_p4_lcd5.h"

namespace tracecraft {
namespace board {

class P4Controls {
public:
  explicit P4Controls(App& app) : _app(app) {}

  // Boot cue: one terse piezo-style blip (race-ECU voice).
  void bootCue() { beep(2093, 60); }

  // Ready cue: the race-ECU arming voice — two terse, identical, flat blips the
  // moment the splash finishes fading into the dash. Deliberately unmusical:
  // real motorsport dashes (MoTeC/AiM class) chirp a piezo, they don't play
  // jingles. Same pitch as the boot blip so the whole boot speaks one voice.
  // Audio is async (queued): nothing blocks here. Once a lap, after app.loop().
  void readyCue() {
    if (_splashWasActive && !_app.splash().active()) {
      TC_LOOPMARK('l');
      beep(2093, 60);
      tone(0, 0, 80);
      beep(2093, 60);
    }
    _splashWasActive = _app.splash().active();
  }

  // Tap the LEFT half of the screen to lower the backlight, the RIGHT half to
  // raise it, in 10% steps floored at 10% (a pitch-black panel with no physical
  // brightness button would look bricked). A small top-layer chip shows the new
  // level for a beat. Gestures classify on release, so a swipe never also fires
  // the tap.

  // Your UI's own gestures (a swipe to page, say). The hook is called for each
  // queued gesture before the brightness tap sees it — return true to keep it —
  // and then once with TouchGesture::None when the queue is empty.
  using GestureHook = bool (*)(TouchGesture g, int16_t x, int16_t y);
  void onGesture(GestureHook hook) { _gestureHook = hook; }

  // The brightness chip's font. LVGL's built-in Montserrat 24 by default.
  void setChipFont(const lv_font_t* font) { _chipFont = font; }

  bool brightnessChipShown() const {
    return _briChip != nullptr && !lv_obj_has_flag(_briChip, LV_OBJ_FLAG_HIDDEN);
  }

  // Drain the touch queue. Once a lap, before app.loop(); call it again late in
  // your own lap to halve the average touch-to-action latency.
  void serviceTouch() {
    TC_LOOPMARK('b');
    int16_t x, y;
    TouchGesture g;
    // Touch is inert until the boot splash clears: brightness and paging belong
    // to the dashboard, not the loading screen. Whatever the touch task queued
    // while booting is DISCARDED — the GT911 reports a spurious tap on power-up,
    // and without this it would surface the brightness chip over the splash (or
    // fire the instant the dash appears).
    if (_app.splash().active()) {
      while (touchGesture(x, y) != TouchGesture::None) { /* discard */ }
      return;
    }
    // Drain EVERY queued event — the render loop laps slower than the touch task
    // samples, and a stroke emits Down then Tap/Swipe; consuming one per lap
    // would add a full lap of latency to the second event.
    while ((g = touchGesture(x, y)) != TouchGesture::None) {
      if (_gestureHook && _gestureHook(g, x, y)) continue;
      if (g == TouchGesture::Tap) brightnessTap(x);
    }
    if (_gestureHook) _gestureHook(TouchGesture::None, 0, 0);
    if (_briChip && _briShownMs != 0 && millis() - _briShownMs > 900) {
      lv_obj_add_flag(_briChip, LV_OBJ_FLAG_HIDDEN);
      _briShownMs = 0;
    }
  }

private:
  void brightnessTap(int16_t x) {
    const lv_coord_t half = lv_display_get_horizontal_resolution(NULL) / 2;
    int b = (int)brightness() + (x >= half ? 10 : -10);
    if (b < 10) b = 10;
    if (b > 100) b = 100;
    setBrightness((uint8_t)b);

    if (!_briChip) {
      _briChip = lv_obj_create(lv_layer_top());
      lv_obj_remove_style_all(_briChip);
      lv_obj_set_size(_briChip, 300, 64);
      lv_obj_set_style_radius(_briChip, 12, 0);
      lv_obj_set_style_bg_opa(_briChip, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(_briChip, lv_color_hex(0x16181c), 0);
      lv_obj_set_style_border_width(_briChip, 1, 0);
      lv_obj_set_style_border_color(_briChip, lv_color_hex(0x2a2d35), 0);
      lv_obj_align(_briChip, LV_ALIGN_CENTER, 0, 0);
      _briLabel = lv_label_create(_briChip);
      lv_obj_set_style_text_font(_briLabel, _chipFont, 0);
      lv_obj_set_style_text_color(_briLabel, lv_color_hex(0xf1f2f5), 0);
      lv_obj_center(_briLabel);
    }
    char t[24];
    snprintf(t, sizeof(t), "BRIGHTNESS %d%%", b);
    lv_label_set_text(_briLabel, t);
    lv_obj_remove_flag(_briChip, LV_OBJ_FLAG_HIDDEN);
    _briShownMs = millis();
  }

  App&              _app;
  GestureHook       _gestureHook = nullptr;
  const lv_font_t*  _chipFont = &lv_font_montserrat_24;
  lv_obj_t*         _briChip = nullptr;
  lv_obj_t*         _briLabel = nullptr;
  uint32_t          _briShownMs = 0;
  bool              _splashWasActive = true;
};

} // namespace board
} // namespace tracecraft
