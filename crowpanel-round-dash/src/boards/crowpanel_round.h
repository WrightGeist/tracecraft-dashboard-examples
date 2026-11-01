#pragma once
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <TraceCraftDisplay.h>

/**
 * Elecrow CrowPanel 1.28" HMI Rotary Display — 1.28" 240x240 ROUND GC9A01 IPS over SPI,
 * ESP32-S3 (8MB PSRAM / 16MB flash). Capacitive touch (CST816, I2C) + the rotary encoder
 * are INPUT-only and unused for now (the framework is receive-only telemetry display).
 *
 * Pins (Elecrow wiki): SCLK 10, MOSI 11, DC 3, CS 9, RST 14, backlight 46. The board
 * GATES POWER to the LCD + backlight LED behind GPIO 1 (LCD_3V3) and GPIO 2 (LEDA_3V3) —
 * both MUST be driven HIGH in init() or the panel stays dark.
 *
 * It's a ROUND panel: the four corners are cut off by the bezel, so center-anchored UIs
 * read best (the generic channel-list monitor is fine for bring-up, just clipped at the
 * edges). If colors look inverted/negative, flip `invert`; if swapped, flip setSwapBytes.
 */
namespace tracecraft {

class CrowPanelRoundLGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  CrowPanelRoundLGFX() {
    { // SPI bus
      auto c = _bus.config();
      c.spi_host   = SPI2_HOST;
      c.spi_mode   = 0;
      c.freq_write = 40000000;     // 40 MHz — safe; the GC9A01 can usually take 80 MHz
      c.pin_sclk = 10; c.pin_mosi = 11; c.pin_miso = -1; c.pin_dc = 3;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    { // GC9A01 round panel
      auto c = _panel.config();
      c.pin_cs = 9; c.pin_rst = 14; c.pin_busy = -1;
      c.panel_width  = 240; c.panel_height = 240;
      c.offset_x = 0; c.offset_y = 0;
      c.readable = false; c.invert = true;     // GC9A01 IPS panels are usually inverted
      c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c);
    }
    { // PWM backlight
      auto c = _light.config();
      c.pin_bl = 46; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

/** LovyanGFX-backed renderer for the CrowPanel 1.28" round GC9A01. */
class CrowPanelRoundRenderer : public IRenderer {
  CrowPanelRoundLGFX _tft;
public:
  void init() override {
    pinMode(1, OUTPUT); digitalWrite(1, HIGH);   // LCD_3V3  — MUST be HIGH or panel is dark
    pinMode(2, OUTPUT); digitalWrite(2, HIGH);   // LEDA_3V3 — backlight LED rail
    _tft.init();
    _tft.setRotation(0);
    _tft.setSwapBytes(true);       // RGB565 byte order for LovyanGFX pushImage
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
  inline IRenderer& renderer() { static CrowPanelRoundRenderer r; return r; }
  inline bool hasPSRAM() { return true; }   // ESP32-S3, 8MB PSRAM
}
}
