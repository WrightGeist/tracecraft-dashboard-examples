#pragma once
#include <Arduino.h>

/**
 * Board selection for this project's dashboards. Define exactly one
 * TRACECRAFT_BOARD_* macro in build_flags. Each board header provides
 * tracecraft::board::renderer() and hasPSRAM(), which a dashboard hands to
 * app.begin(). The libraries know no boards: a board is this project's code.
 */

#if defined(TRACECRAFT_BOARD_T4_S3)
  #include "t4_s3.h"
#elif defined(TRACECRAFT_BOARD_TDISPLAY_S3)
  #include "tdisplay_s3.h"
#elif defined(TRACECRAFT_BOARD_CROWPANEL_ROUND)
  #include "crowpanel_round.h"
#elif defined(TRACECRAFT_BOARD_WAVESHARE_P4_LCD5)
  #include "waveshare_p4_lcd5.h"
#else
  #error "TraceCraft: no board selected. Add e.g. -D TRACECRAFT_BOARD_T4_S3 to build_flags."
#endif
