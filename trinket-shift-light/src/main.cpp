/**
 * Headless TraceCraftCore example — a real SHIFT LIGHT on an Adafruit Trinket M0.
 *
 * The smallest part this SDK runs on: a SAMD21 with 32 KB of RAM, 256 KB of flash,
 * five GPIO and one onboard RGB DotStar. No screen, no LVGL, no rendering framework —
 * just the USB telemetry CLIENT driving that single pixel from
 * live RPM, using the same GT3 idiom as the big dashboards: green through amber to
 * red as the revs climb, then a BLUE strobe at the shift point.
 *
 * Why this example exists: it is the proof that "any USB-capable MCU" is a real claim
 * and not marketing. Everything above the `usb::` seam is the same code the 1280x720
 * P4 deck compiles — the same client, the same channel registry, the same handshake.
 * If it fits here, the constraint is your imagination, not the framework. A servo, a
 * haptic puck, a fan controller, a relay board, a strip of WS2812s behind your wheel:
 * same shape, swap what you do with the values.
 *
 * It declares only three channels (shift_light.channels.json, `custom_channels` in
 * platformio.ini), so a frame is one USB report and the core's buffers are sized to
 * three channels. Do the same on a small part.
 *
 * What you should see on boot: the DotStar walks red -> green -> blue once (400 ms
 * each), then pulses BLUE about once a second — alive, waiting for telemetry — while
 * the small red pin-13 LED blinks ~4x/second as a loop heartbeat. NOTHING here is
 * ever steady: a steady light means setup() never finished.
 *
 * Build + flash:  pio run -e trinket_m0 -t uf2
 *   then double-tap RST and copy firmware.uf2 onto the TRINKETBOOT drive.
 */
#include <Arduino.h>
#include <TraceCraft.h>                // CORE only — no <TraceCraftDisplay.h>, no LVGL
#include <Adafruit_DotStar.h>
#include "tc_generated.h"    // tcStore + this build's channel registry

using namespace tracecraft;

static HidTransport hid;
static Dashboard    dash;

// Onboard RGB DotStar (1 px): the Trinket M0 wires it to data pin 7, clock pin 8,
// byte order BGR (verified on hardware — BRG swaps red and green, which shows the
// low-rev green as red and makes the whole ramp read backwards).
static Adafruit_DotStar led(1, 7, 8, DOTSTAR_BGR);

// Channels are looked up BY NAME at boot; anything this build didn't declare resolves
// to -1 and is treated as absent (never hardcode TC_CH_* — that's what keeps a
// peripheral sim-agnostic when the registry is regenerated).
static int s_iRpm = -1, s_iShift = -1, s_iMax = -1;

// Used only when the sim publishes neither a shift point nor a rev limit. A recorder
// that sends presence (PROTOCOL.md §4.8) says so outright -- has() is false -- but an
// older one cannot, and "not published" then arrives as 0. Either way, never treat
// it as a redline of zero, or every car would sit permanently on the strobe.
static constexpr int kFallbackRedline = 7500;

static int idxOf(const char* name) {
  for (uint8_t i = 0; i < tcStore.count(); i++)
    if (strcmp(tcStore.defs()[i].name, name) == 0) return i;
  return -1;
}

// A channel this build declared AND the host has a value for. has() is the host's
// word with a recorder that sends presence, and true for every channel with an
// older one, so the `> 0` checks in shiftPoint() stay: they are what still works
// with every recorder.
static bool present(int idx) { return idx >= 0 && tcStore.has((uint8_t)idx); }
static int readInt(int idx) { return present(idx) ? tcStore.getInt(idx) : 0; }

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, r, g, b);
  led.show();
}

// The rev ceiling this car shifts at: the sim's own shift point when it publishes one,
// else its rev limit, else a sane constant. Read every loop rather than latched at
// boot — it changes when the driver swaps cars without reconnecting the device.
static int shiftPoint() {
  int rpm = readInt(s_iShift);
  if (rpm <= 0) rpm = readInt(s_iMax);
  return rpm > 0 ? rpm : kFallbackRedline;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);        // onboard red LED (pin 13) = loop heartbeat
  led.begin();
  led.setBrightness(120);

  // Boot self-test FIRST, while USB is still down: red -> green -> blue proves the
  // pixel and the sketch are alive before anything can go wrong over the wire.
  //
  // THE ORDER MATTERS, AND THE INTUITIVE ONE IS WRONG. usb::poll() on the Adafruit
  // cores is TinyUSBDevice.task() — a COOPERATIVE pump, driven from dash.loop(). So
  // anything that blocks after usb::start() stops servicing control transfers, and
  // 1.2 s of delay() lands squarely in the enumeration window: the host gives up and
  // the device never appears AT ALL (not even as a failed descriptor). Before the
  // attach there is no enumeration to starve, so the same delays are free here.
  setLed(60, 0, 0);  delay(400);
  setLed(0, 60, 0);  delay(400);
  setLed(0, 0, 60);  delay(400);

  usb::setIdentity(proto::VENDOR_ID, proto::PRODUCT_ID, "TraceCraft", "TraceCraft Shift Light");
  dash.begin(hid, tcStore, "TraceCraft Shift Light");   // registers the HID interface
  usb::start();                        // enumerate (CDC + HID)

  s_iRpm   = idxOf("rpm");
  s_iShift = idxOf("rpm_shift");
  s_iMax   = idxOf("rpm_max");

  usb::log().printf("\n[TraceCraft Shift Light] schema 0x%04X, %u channels\n",
                    tcStore.schemaId(), (unsigned)tcStore.count());

}

void loop() {
  digitalWrite(LED_BUILTIN, (millis() / 250) % 2);   // heartbeat: proves loop() is alive
  dash.loop();                         // pump USB + answer discovery + apply frames

  // Idle (no telemetry yet): a clearly-visible blue BLINK = alive and waiting. If you
  // see this blinking but nothing changes when you drive, the device isn't receiving
  // telemetry — check that it shows as streaming in the recorder's dashboard list.
  if (dash.waitingForData()) {
    const bool on = (millis() % 1000) < 500;
    setLed(0, 0, on ? 150 : 0);
    return;
  }

  const float f = readInt(s_iRpm) / (float)shiftPoint();   // fraction of the shift point

  // AT the shift point: blue strobe. Blue because it has to beat a red LED already at
  // full brightness — the same reason real GT3 wheels change hue instead of just
  // blinking harder, and the same cue the on-screen dashboards use.
  if (f >= 1.0f) {
    const bool on = (millis() % 100) < 50;
    setLed(0, 0, on ? 255 : 0);
    return;
  }

  // Below it: green -> amber -> red, solid until 80%, then a quickening blink so the
  // last stretch to the shift point is felt rather than read.
  uint8_t r, g, b;
  if      (f < 0.55f) { r = 0;   g = 255; b = 0; }   // green
  else if (f < 0.80f) { r = 255; g = 120; b = 0; }   // amber
  else                { r = 255; g = 0;   b = 0; }   // red

  if (f >= 0.80f) {
    const int  period = (int)(400.0f - (f - 0.80f) / 0.20f * 320.0f);   // 400 ms -> 80 ms
    const bool on     = (millis() % period) < period / 2;
    setLed(on ? r : 0, on ? g : 0, on ? b : 0);
  } else {
    setLed(r, g, b);                   // solid
  }
}
