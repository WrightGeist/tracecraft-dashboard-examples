#pragma once
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <TraceCraftDisplay.h>

/**
 * Lilygo T-Display S3 — 1.9" 170x320 ST7789, 8-bit (i80) parallel bus, driven via
 * LovyanGFX. Pin map and panel offset (35) are the community-verified values;
 * double-check against your board revision if the image is shifted.
 *
 * The USB/HID telemetry path is identical to every other ESP32-S3 board — only
 * the display backend differs — so plug-n-play HID works here unchanged.
 */
namespace tracecraft {

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM     _light;

public:
  LGFX() {
    { // 8-bit parallel bus
      auto c = _bus.config();
      c.freq_write = 16000000;
      c.pin_wr = 8;  c.pin_rd = 9;  c.pin_rs = 7;   // RS == DC
      c.pin_d0 = 39; c.pin_d1 = 40; c.pin_d2 = 41; c.pin_d3 = 42;
      c.pin_d4 = 45; c.pin_d5 = 46; c.pin_d6 = 47; c.pin_d7 = 48;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    { // ST7789 panel
      auto c = _panel.config();
      c.pin_cs = 6; c.pin_rst = 5; c.pin_busy = -1;
      c.panel_width  = 170; c.panel_height = 320;
      c.offset_x = 35;  c.offset_y = 0;     // 170-wide panel centered on 240-wide controller
      c.readable  = false; c.invert = true;
      c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c);
    }
    { // PWM backlight
      auto c = _light.config();
      c.pin_bl = 38; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

/** LovyanGFX-backed renderer for the T-Display S3. */
class TDisplayS3Renderer : public IRenderer {
  LGFX _tft;
public:
  void init() override {
    pinMode(15, OUTPUT);            // PIN_POWER_ON — MUST be HIGH or the panel stays dark
    digitalWrite(15, HIGH);
    _tft.init();
    _tft.setRotation(1);           // landscape -> 320x170
    _tft.setSwapBytes(true);       // RGB565 byte order for ST7789
    _tft.setBrightness(200);
    _tft.fillScreen(0);
  }
  uint16_t width()  override { return _tft.width();  }
  uint16_t height() override { return _tft.height(); }
  void flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* px) override {
    _tft.startWrite();
    _tft.pushImage(x, y, w, h, px);
    _tft.endWrite();
  }
};

namespace board {
  inline IRenderer& renderer() { static TDisplayS3Renderer r; return r; }
  inline bool hasPSRAM() {
#if defined(BOARD_HAS_PSRAM)
    return true;
#else
    return false;
#endif
  }
} // namespace board

} // namespace tracecraft
