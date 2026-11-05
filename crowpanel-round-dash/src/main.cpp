/**
 * CrowPanel round dial: a TraceCraft dashboard and a gamepad on one USB cable.
 *
 * The screen shows brake bias, TC, ABS and engine map from the sim. The encoder presses
 * gamepad buttons the sim can bind (two per screen), and the dial button changes the
 * screen. See README.md.
 */
#include <TraceCraft.h>        // the telemetry client
#include "tracecraft/usb/idf.h"
#include "tc_generated.h"      // tcStore, generated from crowpanel_round.channels.json
#include "Dialpad.h"
#include "Display.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace tracecraft;

#define DEVICE_NAME "TraceCraft Round Dial"

// A product id of its own: Windows remembers a device's interfaces by its IDs, and
// this one has a gamepad that a plain dashboard does not.
static constexpr uint16_t kProductId = 0x4D02;

// The gamepad is the second HID interface (usb_descriptors.c).
static constexpr uint8_t kGamepad = 1;

static Dashboard    dash;
static HidTransport hid;

static void sendButtons(uint8_t buttons) {
  static uint8_t sent = 0xFF;
  if (buttons == sent || !tud_mounted() || !tud_hid_n_ready(kGamepad)) return;
  if (tud_hid_n_report(kGamepad, 0, &buttons, sizeof(buttons))) sent = buttons;
}

extern "C" void app_main(void) {
  usb::setIdentity(proto::VENDOR_ID, kProductId, "TraceCraft", DEVICE_NAME);
  dash.begin(hid, tcStore, DEVICE_NAME);   // the name the recorder lists
  usb::start();
  dialpad_begin();
  display_begin();

  uint32_t lastDraw = 0;
  for (;;) {
    dash.loop();
    if (dialpad_take_press()) dialpad_set_page(display_next_page());
    sendButtons(dialpad_buttons());

    // The screen at about 20 Hz; the knob is read at about 200 Hz.
    const uint32_t now = hal::millis();
    if (now - lastDraw >= 50) {
      lastDraw = now;
      display_update(dash.waitingForData());
      display_splash_tick((uint8_t)dash.linkPhase(), dash.updateVerdict(), dash.latestVersion());
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
