// Include shim. EEZ-Studio's generated code uses `#include <lvgl/lvgl.h>` (the
// ESP-IDF / component layout), but the PlatformIO `lvgl` library exposes the
// header at the root as `<lvgl.h>`. Because `include/` is on the include path
// (-I include), this file lets the EEZ default resolve unchanged — so you never
// have to touch the generated src/ui/ files or the EEZ "LVGL include" setting.
#pragma once
#include <lvgl.h>
