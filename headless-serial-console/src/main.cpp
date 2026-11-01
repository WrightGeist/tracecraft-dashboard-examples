/**
 * Headless TraceCraftCore example — a live SERIAL TELEMETRY CONSOLE.
 *
 * Generic ESP32-S3 DevKitC (or any board with a USB-CDC console). NO display, NO
 * LVGL, NO rendering framework of any kind — just the USB telemetry CLIENT
 * plus a printf loop over the USB-CDC log. This is the
 * honest "ingest data + speak HID, then do whatever you want with it" template:
 * bring the client up, and on every frame read the channels and DO SOMETHING.
 * Here we print a live table to a serial terminal — swap that for your own logic
 * (log to an SD card, drive a servo/LED/haptic, forward over UART/BLE/CAN, or
 * hand the values to a renderer of your own choosing). The core doesn't care what
 * you build on top; it only guarantees the data and the HID handshake.
 *
 * What you'll see: on boot it prints the channel registry this build declared
 * then — once the recorder starts streaming — a compact live
 * line of the marquee channels a few times a second. Channels the build didn't
 * declare are simply skipped; nothing is hardcoded to a specific sim.
 *
 * Build + flash:  pio run -e console_s3 -t upload
 *   (ESP32-S3 native USB — hold BOOT + tap RST first if it won't auto-reset.)
 *   Then open any serial monitor; USB-CDC ignores the baud rate.
 */
#include <Arduino.h>
#include <TraceCraft.h>                // CORE only — no <TraceCraftDisplay.h>, no LVGL
#include "tc_generated.h"    // tcStore + the shared channel registry

using namespace tracecraft;

static HidTransport hid;
static Dashboard    dash;

// A compact live line is nicer than dumping all ~100 channels every tick. These
// are looked up BY NAME at boot; any the build didn't declare resolve to -1 and
// are silently skipped, so this stays sim-agnostic (never hardcode TC_CH_*).
struct Marquee { const char* channel; const char* label; int idx; };
static Marquee s_marquee[] = {
  { "gear",         "gear" },
  { "rpm",          "rpm"  },
  { "speed_kmh",    "kph"  },
  { "throttle_pct", "thr"  },
  { "brake_pct",    "brk"  },
  { "fuel_pct",     "fuel" },
  { "lap_time_str", "lap"  },
};

static const char* typeName(ChannelType t) {
  switch (t) {
    case Ch_Int:   return "int";
    case Ch_Float: return "float";
    case Ch_Bool:  return "bool";
    case Ch_Str:   return "str";
  }
  return "?";
}

// Index of a channel by canonical name, or -1 if this build didn't declare it.
static int idxOf(const char* name) {
  for (uint8_t i = 0; i < tcStore.count(); i++)
    if (strcmp(tcStore.defs()[i].name, name) == 0) return i;
  return -1;
}

// Append "<label> <value> " for one channel to buf (type-appropriate formatting).
static void appendValue(char* buf, size_t cap, const char* label, int idx) {
  size_t n = strlen(buf);
  if (n >= cap) return;
  char v[40];
  // "-" for a channel the recorder says it has no value for (PROTOCOL.md §4.8);
  // with an older recorder has() is always true and the value prints as before.
  if (!tcStore.has((uint8_t)idx)) { snprintf(buf + n, cap - n, "%s -  ", label); return; }
  switch (tcStore.defs()[idx].type) {
    case Ch_Int:   snprintf(v, sizeof(v), "%ld",   (long)tcStore.getInt(idx));  break;
    case Ch_Float: snprintf(v, sizeof(v), "%.2f",  tcStore.getFloat(idx));      break;
    case Ch_Bool:  snprintf(v, sizeof(v), "%s",    tcStore.getBool(idx) ? "on" : "off"); break;
    case Ch_Str:   snprintf(v, sizeof(v), "%s",    tcStore.getStr(idx));        break;
  }
  snprintf(buf + n, cap - n, "%s %s  ", label, v);
}

void setup() {
  // Bring USB up ourselves: register the HID interface + identity BEFORE
  // enumeration (CDC_ON_BOOT=0), so the descriptor and PID are in place when the
  // host first sees the device.
  usb::setIdentity(proto::VENDOR_ID, proto::PRODUCT_ID, "TraceCraft", "TraceCraft Console");
  dash.begin(hid, tcStore, "TraceCraft Console");   // registers the HID interface
  usb::start();                                      // enumerate (CDC + HID)

  // Print the registry THIS build declared so you can
  // see exactly which channels the recorder will fill by name.
  usb::log().printf("\n=== TraceCraft serial console ===\n");
  usb::log().printf("schema 0x%04X, %u channels:\n",
                    tcStore.schemaId(), (unsigned)tcStore.count());
  for (uint8_t i = 0; i < tcStore.count(); i++)
    usb::log().printf("  [%2u] %-22s %s\n", i, tcStore.defs()[i].name,
                      typeName(tcStore.defs()[i].type));

  for (auto& m : s_marquee) {
    m.idx = idxOf(m.channel);
    // Say so when a name does not resolve. A silent skip once hid a typo in this
    // very table for months: the pedals were spelled `throttle` / `brake`, the
    // manifest says `throttle_pct` / `brake_pct`, and nothing ever printed.
    if (m.idx < 0)
      usb::log().printf("[console] channel '%s' is not in this build's registry -- not shown\n",
                        m.channel);
  }
  usb::log().print("\nWaiting for TraceCraft Recorder to stream telemetry...\n");
}

void loop() {
  dash.loop();   // pump USB, answer discovery, apply incoming frames

  static bool     waiting = true;
  static uint32_t lastMs  = 0;
  const uint32_t  now     = millis();

  // Idle until the recorder streams. Before the first frame every channel reads
  // its type's zero, so waitingForData() (link liveness) is the honest "have we
  // ever had data" gate.
  // Emit a slow heartbeat while idle so a terminal opened at ANY time sees life
  // (the one-shot boot banner races USB enumeration and can be dropped).
  if (dash.waitingForData()) {
    waiting = true;
    if (now - lastMs >= 2000) {
      lastMs = now;
      usb::log().printf("[waiting] no telemetry yet - %u channels ready; start a session "
                        "with USB-dashboard streaming enabled\n", (unsigned)tcStore.count());
    }
    return;
  }
  if (waiting) { usb::log().print("[streaming]\n"); waiting = false; }


  // ---- throughput census ------------------------------------------------------
  // The rate this device ACTUALLY receives, once a second, from dash.linkStats():
  // frames delivered, frames the host wrote that never arrived (SEQ gaps: lost on
  // USB, or overwritten in the mailbox because loop() ran slower than the frame
  // rate), 64-byte USB reports per frame, and the host's own target rate (it
  // states it on the keep-alive ping, PROTOCOL.md §4.5). Reports per frame is
  // what caps the rate on a Full-Speed link: one report per 1 ms, so the ceiling
  // is 1000 / reports-per-frame regardless of the recorder's setting. When the
  // host falls short of its own target, the channel set is the reason; a board
  // that declares only what it draws fits one report and runs ~900 Hz .
  //
  // Drive it with the SDK's feed_demo.py <hz>.
  {
    static uint32_t lastRateMs = 0;
    const tracecraft::LinkStats& s = dash.linkStats();
    if (s.valid && now - lastRateMs >= 1000) {
      lastRateMs = now;
      if (s.targetHz) {
        usb::log().printf("[rate] %u frames/s of %u target (%u%%)  %u dropped/s  "
                          "%lu reports/s  %.1f reports/frame  %u ch\n",
                          (unsigned)s.framesPerSec, (unsigned)s.targetHz,
                          (unsigned)s.deliveredPct(), (unsigned)s.droppedPerSec,
                          (unsigned long)s.reportsPerSec, (double)s.reportsPerFrame,
                          (unsigned)tcStore.count());
      } else {
        usb::log().printf("[rate] %u frames/s (host did not state a target)  %u dropped/s  "
                          "%lu reports/s  %.1f reports/frame  %u ch\n",
                          (unsigned)s.framesPerSec, (unsigned)s.droppedPerSec,
                          (unsigned long)s.reportsPerSec, (double)s.reportsPerFrame,
                          (unsigned)tcStore.count());
      }
      if (s.hostBelowTarget()) {
        const unsigned ceiling = s.reportsPerFrame > 0.f ? (unsigned)(1000.f / s.reportsPerFrame) : 0u;
        usb::log().printf("[rate] below target: %.0f reports per frame cap a Full-Speed link "
                          "near %u Hz. Declare fewer channels to raise it.\n",
                          (double)s.reportsPerFrame, ceiling);
      }
      if (s.dropping()) {
        usb::log().printf("[rate] dropping: %u frames/s lost on this device (USB loss, or "
                          "loop() slower than the frame rate).\n", (unsigned)s.droppedPerSec);
      }
    }
  }

  if (now - lastMs < 250) return;   // ~4 Hz — readable, not a flood
  lastMs = now;

  char line[256] = "";
  for (auto& m : s_marquee)
    if (m.idx >= 0) appendValue(line, sizeof(line), m.label, m.idx);
  usb::log().printf("%s\n", line);
}
