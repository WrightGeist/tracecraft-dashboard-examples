#pragma once
#include "AmoledRenderer.h"

/**
 * Lilygo T-Display S3 AMOLED — 1.91" RM67162 AMOLED (536x240), QSPI, ESP32-S3.
 *
 * Same RM67162/RM690B0 AMOLED family as the T4-S3, so it reuses the shared
 * AmoledRenderer: LilyGo_AMOLED::begin() auto-detects the exact board and width()/
 * height() report the detected 536x240 panel. Only the default rotation differs
 * (landscape here vs the T4-S3's portrait). The USB/telemetry path is the standard
 * ESP32-S3 one — unchanged.
 *
 * Rotation 1 = 536x240 landscape. If the image is upside-down/sideways for how you've
 * mounted it, change the AmoledRenderer arg (0..3).
 */
namespace tracecraft {
namespace board {
  // RM67162 native is 240x536 (portrait). LilyGo setRotation case 2 swaps to
  // _width=536/_height=240 -> 536x240 LANDSCAPE. (Rotation 1 is the native 240x536
  // portrait; if you want portrait, use r(1). Flip 180° with r(0) if it's upside-down.)
  inline AmoledRenderer& amoled() { static AmoledRenderer r(2); return r; }  // rotation 2 -> 536x240 landscape
  inline IRenderer& renderer() { return amoled(); }
  inline bool hasPSRAM() { return true; }   // ESP32-S3R8 (8MB PSRAM)
  /** Touch (CST816T over I2C, already brought up by begin()). UI-space coords —
   *  see AmoledRenderer::touchPoint. Polled by the amoled dashboard's tap-to-page
   *  handler in src/main.cpp / ui_amoleddash. */
  inline bool touchPoint(int16_t& x, int16_t& y) { return amoled().touchPoint(x, y); }
}
}
