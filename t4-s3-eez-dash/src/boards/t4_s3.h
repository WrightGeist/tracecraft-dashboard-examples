#pragma once
#include "AmoledRenderer.h"

/**
 * Lilygo T4-S3 — 2.41" RM690B0 AMOLED (450x600), QSPI, ESP32-S3R8.
 * Uses the shared LilyGo AMOLED renderer (begin() auto-detects the board).
 * Note: GPIO18 is the display TE pin — do not repurpose it.
 */
namespace tracecraft {
namespace board {
  inline AmoledRenderer& amoled() { static AmoledRenderer r(3); return r; }  // rotation 3 -> 450x600 portrait (USB at bottom)
  inline IRenderer& renderer() { return amoled(); }
  inline bool hasPSRAM() { return true; }   // ESP32-S3R8 (8MB PSRAM)
  /** Touch (CST226 over I2C, already brought up by begin()). UI-space
   *  coords — see AmoledRenderer::touchPoint. Polled by the T4 deck's
   *  swipe classifier in the t4-s3-deck project. */
  inline bool touchPoint(int16_t& x, int16_t& y) { return amoled().touchPoint(x, y); }
}
}
