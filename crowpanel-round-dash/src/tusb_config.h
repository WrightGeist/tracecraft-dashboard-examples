// TinyUSB configuration, read by the raw espressif/tinyusb component (CMakeLists.txt).
// Two HID interfaces: the TraceCraft dashboard and the gamepad.
#pragma once

#include "sdkconfig.h"

#define CFG_TUSB_MCU           OPT_MCU_ESP32S3
#define CFG_TUSB_OS            OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENABLED        1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_HID            2   // dashboard (instance 0) + gamepad (instance 1)
#define CFG_TUD_CDC            1   // a debug console: the library logs its link state here
#define CFG_TUD_MSC            0
#define CFG_TUD_MIDI           0
#define CFG_TUD_VENDOR         0

#define CFG_TUD_HID_EP_BUFSIZE 64
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
