/**
 * Headless TraceCraftCore example — a shift light on a Raspberry Pi Pico (RP2040),
 * using nothing but the single green LED already soldered to the board.
 *
 * The Trinket M0 example (trinket-shift-light) is the same idea on
 * an RGB pixel, where rising revs are a color ramp. A Pico has ONE color, so this
 * is the more interesting version of the problem: you have a single channel and you
 * still have to say "not yet / soon / NOW" without ambiguity. The answer is to use
 * the two dimensions a mono LED does have —
 *
 *   BRIGHTNESS  = how close you are          (dim -> full, PWM)
 *   RATE        = how urgent it is           (solid -> blink -> strobe)
 *
 * which is legible in peripheral vision precisely because it never asks you to
 * distinguish two colors mid-corner. The same technique drives a servo's angle, a
 * fan's duty, or a haptic motor's amplitude — swap analogWrite() for your actuator
 * and the logic above it is unchanged.
 *
 * What you should see on boot: three quick pulses (self-test), then a slow breathing
 * fade — alive, waiting for telemetry. Under live telemetry the LED tracks the revs:
 * dark below 55%, brightening to 80%, then blinking faster and faster, then a hard
 * strobe at the shift point. NOTHING here is ever steady-on: a steady LED means
 * setup() never finished.
 *
 * Build + flash:  pio run -e pico -t uf2
 *   then hold BOOTSEL while plugging in (the RPI-RP2 drive appears) and copy the
 *   .uf2 onto it. The board reboots into the sketch by itself.
 */
#include <Arduino.h>
#include <TraceCraft.h>                // CORE only — no display library, no LVGL
#include "tc_generated.h"    // tcStore + this build's registry (shift_light.channels.json)

using namespace tracecraft;

static HidTransport hid;
static Dashboard    dash;

// The Pico's onboard LED is GP25, PWM-capable, so it can express brightness as well
// as on/off. (Pico W wires its LED to the WiFi chip instead of a GPIO — that board
// needs the wireless driver just to blink, which is why this example targets the
// plain Pico.)
static const int kLed = LED_BUILTIN;

// Channels are resolved BY NAME at boot; anything this build didn't declare comes
// back -1 and is treated as absent. Never hardcode TC_CH_* in a peripheral — the
// registry is regenerated as dashboards come and go.
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

// The rev ceiling this car shifts at: the sim's own shift point when it publishes
// one, else its rev limit, else a sane constant. Read every loop rather than latched
// at boot — it changes when the driver swaps cars without replugging the device.
static int shiftPoint() {
  int rpm = readInt(s_iShift);
  if (rpm <= 0) rpm = readInt(s_iMax);
  return rpm > 0 ? rpm : kFallbackRedline;
}

void setup() {
  pinMode(kLed, OUTPUT);

  // Self-test BEFORE the USB bring-up. usb::poll() is a COOPERATIVE pump driven from
  // dash.loop(), so anything that blocks after usb::start() starves control transfers
  // during enumeration and the host walks away. Before the
  // attach there is no enumeration to starve, so these delays are free here.
  for (int i = 0; i < 3; i++) {
    analogWrite(kLed, 255); delay(120);
    analogWrite(kLed, 0);   delay(120);
  }

  usb::setIdentity(proto::VENDOR_ID, proto::PRODUCT_ID, "TraceCraft", "TraceCraft Shift Light");
  dash.begin(hid, tcStore, "TraceCraft Shift Light");
  usb::start();

  s_iRpm   = idxOf("rpm");
  s_iShift = idxOf("rpm_shift");
  s_iMax   = idxOf("rpm_max");

  usb::log().printf("\n[TraceCraft Pico Shift Light] schema 0x%04X, %u channels\n",
                    tcStore.schemaId(), (unsigned)tcStore.count());
}

void loop() {
  dash.loop();                         // pump USB + answer discovery + apply frames

  // Idle: a slow breathing fade, which is unmistakably different from any of the
  // driving states below (none of which fade). If you see this while driving, the
  // device isn't receiving telemetry — check the recorder's dashboard list.
  if (dash.waitingForData()) {
    const uint32_t t = millis() % 2000;
    const uint32_t up = t < 1000 ? t : 2000 - t;      // 0..1000..0
    analogWrite(kLed, (int)(up * 255 / 1000));
    return;
  }

  const float f = readInt(s_iRpm) / (float)shiftPoint();

  if (f >= 1.0f) {                     // AT the shift point: hard strobe, full power
    analogWrite(kLed, (millis() % 100) < 50 ? 255 : 0);
  } else if (f >= 0.80f) {             // closing in: blink, quickening 400ms -> 80ms
    const int period = (int)(400.0f - (f - 0.80f) / 0.20f * 320.0f);
    analogWrite(kLed, (millis() % period) < period / 2 ? 255 : 0);
  } else if (f >= 0.55f) {             // on the way: solid, brightness tracks revs
    analogWrite(kLed, (int)(40 + (f - 0.55f) / 0.25f * 215.0f));
  } else {
    analogWrite(kLed, 0);              // nothing to say yet
  }
}
