#pragma once
#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <lvgl.h>          // lv_draw_sw_rgb565_swap — optimized flush byte-swap
#include <TraceCraftDisplay.h>

namespace tracecraft {

/**
 * Shared renderer for LilyGO AMOLED boards (T4-S3 RM690B0, T-Display S3 AMOLED
 * RM67162, ... over QSPI) via the LilyGo-AMOLED-Series library.
 *
 * begin() auto-detects the exact board, so one renderer serves the whole family;
 * width()/height() report the detected panel. These controllers corrupt
 * odd-aligned partial writes, hence alignment()==2.
 */
class AmoledRenderer : public IRenderer {
  LilyGo_AMOLED _amoled;
  uint8_t       _rotation;

public:
  explicit AmoledRenderer(uint8_t rotation = 0) : _rotation(rotation) {}

  void init() override {
    if (!_amoled.begin()) {
      Serial.println(F("[TraceCraft] LilyGo_AMOLED begin() failed (board not detected)"));
    }
    _amoled.setRotation(_rotation);
    _amoled.setBrightness(200);
  }
  uint16_t width()  override { return _amoled.width();  }
  uint16_t height() override { return _amoled.height(); }
  uint8_t  alignment() const override { return 2; }   // RM67162/RM690B0 need even windows

  /** Current touch point, or false when the panel isn't being touched.
   *  Coordinates arrive in ROTATED (UI) space: the LilyGo lib's
   *  setRotation() configures the touch controller's swap/mirror to match
   *  the display rotation, so these are the same coordinates LVGL draws
   *  in — no manual remap needed. */
  bool touchPoint(int16_t& x, int16_t& y) {
    return _amoled.getPoint(&x, &y) > 0;
  }

  /** Panel brightness 0..255 (MIPI 0x51). A single command, no framebuffer
   *  traffic — so it's the tear-free way to "flash" a screen: paint the color
   *  once, then pulse brightness instead of repainting the whole frame (a full-
   *  frame repaint races the scan-out and shears/tears on this QSPI panel). */
  void setBrightness(uint8_t b) { _amoled.setBrightness(b); }

  void flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* px) override {
    // RM67162/RM690B0 expect big-endian RGB565; LVGL renders little-endian.
    // LVGL's optimized swap beats the naive per-pixel loop — this runs on
    // EVERY flushed pixel, so it's real time on full-screen slides.
    lv_draw_sw_rgb565_swap(px, (uint32_t)w * h);
    _amoled.pushColors(x, y, w, h, px);
  }

  /** Flush for pixels ALREADY in panel byte order (big-endian RGB565).
   *  The T4 deck's slide pre-swaps its page snapshots once per transition,
   *  so the per-frame bands skip the swap entirely. */
  void flushPreSwapped(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* px) {
    _amoled.pushColors(x, y, w, h, px);
  }
};

} // namespace tracecraft
