/**
 * TraceCraft — Adafruit MacroPad RP2040 (product 5128): USB game controller +
 * a settings dashboard on the OLED.
 *
 * TWO things at once, over two HID interfaces on one USB device:
 *   1. GAME CONTROLLER (device -> PC): buttons only, no axes, so a sim sees a
 *      clean button pad. Keys 1..12 are buttons 1..12. Turning the knob taps an
 *      INCREASE / DECREASE button for the selected setting — each of the 12
 *      cells has its own pair (buttons 17..40, see kDialBtnBase) — so the driver
 *      binds "TC up" / "TC down" once and the knob drives whichever setting is
 *      highlighted. Pressing the knob reaches the PC as nothing: it pages the
 *      OLED (below), so flipping pages never fires a sim binding.
 *   2. TELEMETRY DASHBOARD (PC -> device): the TraceCraft core's vendor HID
 *      receives live telemetry from the recorder, and the 128x64 OLED shows the
 *      driver's adjustable cockpit dials as a 3-column x 4-row grid of compact
 *      abbreviations — the legend for the 3x4 keys below it, with each setting's
 *      current value. One page per supported sim (LMU, ACC, AC EVO, iRacing);
 *      pressing the knob cycles through them. LMU and iRacing are filled in;
 *      ACC and AC EVO are placeholder grids waiting for their dial lists.
 *
 * One cell is always SELECTED (drawn inverted): the first cell when a page
 * opens, then whichever cell's key was pressed last. The knob adjusts it.
 *
 * The gamepad is a second Adafruit_USBD_HID alongside the core's vendor HID
 * (CFG_TUD_HID = 2 on RP2040, so both interfaces coexist). The OLED is driven
 * directly with Adafruit_GFX (not LVGL) — the display layer isn't used here.
 *
 * Standalone sketch (own setup()/loop()); env: macropad.
 */
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <TraceCraft.h>                 // core: HID transport + Store + Dashboard
#include "tc_generated.h"     // tcStore + channel indices

using namespace tracecraft;

// ---- core telemetry client (vendor HID receive) ---------------------------
static HidTransport hid;
static Dashboard    dash;

// ---- OLED (SH1106 128x64 on SPI1) -----------------------------------------
// MacroPad OLED pins come from the board variant (OLED_DC/RST/CS); SPI1 is
// SCK=26, MOSI=27. The panel is 128x64 landscape: rotation 0/2 = landscape,
// 1/3 = portrait. 0 renders upright with the encoder at top.
#define OLED_ROTATION 0
static Adafruit_SH1106G oled(128, 64, &SPI1, OLED_DC, OLED_RST, OLED_CS);

// ---- gamepad (second HID interface, buttons only, no axes) ----------------
// 48 buttons:
//   1..12   the keys (row-major from top left, like the grid)
//   13..16  unused (13 was the knob press before it took over paging, 14/15 its
//           old CW/CCW taps; kept empty so a sim's bindings for 1..12 don't move)
//   17..40  the knob's per-cell pairs: cell i (0..11, row-major) taps 17 + 2i to
//           INCREASE and 18 + 2i to DECREASE — TC is 17/18, CUT 19/20, and so on
//   41..48  spare (pads the report to whole bytes)
// Beyond 32 buttons the Windows game-controller panel (joy.cpl) shows only the
// first 32; DirectInput and the sims read all of them.
static constexpr int kButtons     = 48;
static constexpr int kDialBtnBase = 16;   // bit of button 17
static const uint8_t kGamepadDesc[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Gamepad)
  0xA1, 0x01,        // Collection (Application)
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (Button 1)
  0x29, kButtons,    //   Usage Maximum (Button 48)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, kButtons,    //   Report Count (48)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0xC0               // End Collection
};
static Adafruit_USBD_HID gamepad(kGamepadDesc, sizeof(kGamepadDesc),
                                 HID_ITF_PROTOCOL_NONE, 4, false);
static constexpr int kReportBytes = kButtons / 8;
static uint64_t s_btn     = 0;          // current button bitmask (bit n = button n+1)
static uint64_t s_btnSent = ~0ull;      // last mask sent (force first send)

// key GPIOs (active-low, INPUT_PULLUP) -> buttons 1..12 (bits 0..11)
static const uint8_t kKeyPins[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static constexpr int kEncA = 18, kEncB = 17, kEncSW = 0;

// ---- encoder: interrupt-driven bounce-tolerant quadrature decoder ----------
static volatile int16_t s_encDelta = 0;
static void encISR() {
  static const int8_t kValid[16] = { 0,1,1,0, 1,0,0,1, 1,0,0,1, 0,1,1,0 };
  static uint8_t prevNext = 0;
  static uint16_t store   = 0;
  prevNext <<= 2;
  if (digitalRead(kEncB)) prevNext |= 0x02;
  if (digitalRead(kEncA)) prevNext |= 0x01;
  prevNext &= 0x0f;
  if (kValid[prevNext]) {
    store = (store << 4) | prevNext;
    if ((store & 0xff) == 0x2b)      s_encDelta--;
    else if ((store & 0xff) == 0x17) s_encDelta++;
  }
}

// ---- OLED dashboard --------------------------------------------------------
static uint32_t s_lastDraw = 0;
static constexpr uint32_t kDrawIntervalMs = 90;

// How a cell turns its channel into text. A channel the recorder says it has no
// value for (tcStore.has() false, PROTOCOL.md §4.8) is always "--". An older
// recorder cannot say -- has() is then true for everything and an absent channel
// reads 0 -- so each kind still decides what a 0 means.
enum class Fmt : uint8_t {
  None,     // placeholder: no channel yet, always "--"
  Key,      // label only — a reminder of what the key under it does (no value)
  Dial,     // integer aid dial; 0 -> "--" (car lacks the dial, or it's off)
  Raw,      // integer where 0 is a real setting (LMU's motor map 0 is a map)
  OnOff,    // integer stage where 0 means off (lights, wipers)
  Pct1,     // float percent, one decimal (brake bias 54.5); <= 1 -> "--"
  Kw,       // signed float kW, whole number (live regen flow)
  Drs,      // ch = drs_active, ch2 = drs_available: "OPN" / "RDY" / "--"
  P2p,      // ch = p2p_active, ch2 = p2p_remaining: "ON" while firing, else uses left
};

struct Cell {
  const char* label;
  uint8_t     ch;       // TC_CH_* index; ignored for Fmt::None / Fmt::Key
  Fmt         fmt;
  uint8_t     ch2;      // second channel, for the two-channel kinds only
};

struct Page {
  const char*    title;
  const uint8_t* view;          // TC_VIEW_* the host streams on this page; null = keep the last
  Cell           cells[4][3];   // [row][column]
};

// A placeholder cell: label shown, value always "--".
#define TBD { "---", 0, Fmt::None }

// One page per supported sim; the encoder cycles through them in this order. The
// 3x4 grid mirrors the MacroPad's 3x4 keys, so each cell is the legend for the key
// under it: label = what the driver bound there, value = that setting now. Each
// page's view lists exactly the channels its cells read (macropad.channels.json).
static const Page kPages[] = {
  { "LMU", TC_VIEW_LMU, {
    { { "TC",   TC_CH_TC_LEVEL,       Fmt::Dial  }, { "CUT", TC_CH_TC_CUT,         Fmt::Dial  }, { "SLIP", TC_CH_TC_SLIP,  Fmt::Dial } },
    { { "ABS",  TC_CH_ABS_LEVEL,      Fmt::Dial  }, { "BBi", TC_CH_BRAKE_BIAS_PCT, Fmt::Pct1  }, { "BMi",  TC_CH_BRAKE_MIG, Fmt::Dial } },
    // RGN is LMU's live regen flow in kW (+ harvesting, - deploying): the sim
    // publishes no regen-level dial. TCO (TC override) is a bindable action.
    { { "MAP",  TC_CH_ENGINE_MAP,     Fmt::Raw   }, { "RGN", TC_CH_REGEN_POWER_KW, Fmt::Kw    }, { "TCO",  0,               Fmt::Key } },
    // WIPT (wiper toggle) is a bindable action; WIP shows the wiper state it sets.
    { { "LIT",  TC_CH_HEADLIGHTS,     Fmt::OnOff }, { "WIP", TC_CH_WIPERS,         Fmt::OnOff }, { "WIPT", 0,               Fmt::Key } },
  } },
  { "ACC",     nullptr, { { TBD, TBD, TBD }, { TBD, TBD, TBD }, { TBD, TBD, TBD }, { TBD, TBD, TBD } } },
  { "AC EVO",  nullptr, { { TBD, TBD, TBD }, { TBD, TBD, TBD }, { TBD, TBD, TBD }, { TBD, TBD, TBD } } },
  // Columns: traction control, brakes, hybrid / powertrain. iRacing numbers its
  // TC dials; which does what is per car. The recorder reads no fourth brake dial
  // (brake-bias fine, peak bias, migration), so that key is a placeholder.
  { "iRacing", TC_VIEW_IRACING, {
    { { "TC1", TC_CH_TC_LEVEL, Fmt::Dial }, { "BB",  TC_CH_BRAKE_BIAS_PCT, Fmt::Pct1 }, { "DRS", TC_CH_DRS_ACTIVE, Fmt::Drs, TC_CH_DRS_AVAILABLE } },
    { { "TC2", TC_CH_TC2,      Fmt::Dial }, { "ABS", TC_CH_ABS_LEVEL,      Fmt::Dial }, { "P2P", TC_CH_P2P_ACTIVE, Fmt::P2p, TC_CH_P2P_REMAINING } },
    // DEP / RGN: the MGU-K deploy-mode and regen-gain dials (hybrid cars).
    { { "TC3", TC_CH_TC3,      Fmt::Dial }, { "EB",  TC_CH_ENGINE_BRAKING, Fmt::Dial }, { "DEP", TC_CH_MGUK_MODE,  Fmt::Dial } },
    { { "TC4", TC_CH_TC4,      Fmt::Dial }, TBD,                                          { "RGN", TC_CH_MGUK_REGEN, Fmt::Dial } },
  } },
};
#undef TBD
static constexpr int kPageCount = sizeof(kPages) / sizeof(kPages[0]);
static int s_page = 0;   // boots on LMU
static int s_sel  = 0;   // selected cell, row-major 0..11 (= key index); the knob adjusts it

static void fmtCell(char* out, size_t n, const Cell& c, bool waiting) {
  if (c.fmt == Fmt::Key) { out[0] = '\0'; return; }
  if (waiting || c.fmt == Fmt::None) { strcpy(out, "--"); return; }
  if (!tcStore.has(c.ch)) { strcpy(out, "--"); return; }   // the sim publishes no value
  switch (c.fmt) {
    case Fmt::Dial: {
      const int v = (int)tcStore.getInt(c.ch);
      if (v > 0) snprintf(out, n, "%d", v); else strcpy(out, "--");
      break;
    }
    case Fmt::Raw:
      snprintf(out, n, "%d", (int)tcStore.getInt(c.ch));
      break;
    case Fmt::OnOff: {
      const int v = (int)tcStore.getInt(c.ch);
      if (v > 0) snprintf(out, n, "%d", v); else strcpy(out, "OFF");
      break;
    }
    case Fmt::Pct1: {
      const float v = tcStore.getFloat(c.ch);
      const int tenths = (int)(v * 10.0f + 0.5f);
      if (v > 1.0f) snprintf(out, n, "%d.%d", tenths / 10, tenths % 10); else strcpy(out, "--");
      break;
    }
    case Fmt::Kw: {
      const float v = tcStore.getFloat(c.ch);
      snprintf(out, n, "%d", (int)(v < 0 ? v - 0.5f : v + 0.5f));
      break;
    }
    case Fmt::Drs:
      if (tcStore.getBool(c.ch))       strcpy(out, "OPN");
      else if (tcStore.getBool(c.ch2)) strcpy(out, "RDY");
      else                             strcpy(out, "--");
      break;
    case Fmt::P2p: {
      const int left = (int)tcStore.getInt(c.ch2);
      if (tcStore.getBool(c.ch)) strcpy(out, "ON");
      else if (left > 0)         snprintf(out, n, "%d", left);
      else                       strcpy(out, "--");
      break;
    }
    default:
      strcpy(out, "--");
  }
}

// Grid geometry. The default 6x8 font advances 6 px a character, and three
// label+value pairs barely fit across 128 px — the middle column holds
// "BBi 54.5" and "RGN -117". So each value starts right after the longest label
// in its column (a tabular sub-column), which puts the spare space BETWEEN cells
// rather than inside them; no divider lines needed. Both the same small size.
static constexpr int kColX[3] = { 0, 42, 90 };
static constexpr int kValueGap = 4;   // label's last pixel -> value's first: 3 px clear
static constexpr int kHeaderRuleY = 10;
static constexpr int kRowY[4] = { 14, 27, 40, 53 };

static int textWidth(const char* s) { return (int)strlen(s) * 6 - 1; }

// Where a column's values start on this page: past its longest label.
static int valueX(const Page& p, int col) {
  int w = 0;
  for (int row = 0; row < 4; row++) {
    const int lw = textWidth(p.cells[row][col].label);
    if (lw > w) w = lw;
  }
  return kColX[col] + w + kValueGap;
}

static void drawDashboard() {
  const bool waiting = dash.waitingForData();
  const Page& p = kPages[s_page];

  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);

  // Header: the sim this page is for, and a dot per page (filled = this one).
  oled.setCursor(0, 1);
  oled.print(p.title);
  for (int i = 0; i < kPageCount; i++) {
    const int x = 128 - (kPageCount - i) * 8;
    if (i == s_page) oled.fillRect(x, 2, 5, 5, SH110X_WHITE);
    else             oled.drawRect(x, 2, 5, 5, SH110X_WHITE);
  }
  oled.drawFastHLine(0, kHeaderRuleY, 128, SH110X_WHITE);

  char value[8];
  for (int col = 0; col < 3; col++) {
    const int vx = valueX(p, col);
    for (int row = 0; row < 4; row++) {
      const Cell& c = p.cells[row][col];
      fmtCell(value, sizeof(value), c, waiting);
      // The selected cell is drawn inverted: a white block from just left of its
      // column to just short of the next, black text on it. Rows are 13 px apart,
      // so the 12 px block leaves the neighbours' text untouched.
      const bool sel = (row * 3 + col) == s_sel;
      if (sel) {
        const int x0 = col == 0 ? 0 : kColX[col] - 1;
        const int x1 = col == 2 ? 127 : kColX[col + 1] - 3;
        oled.fillRect(x0, kRowY[row] - 2, x1 - x0 + 1, 12, SH110X_WHITE);
        oled.setTextColor(SH110X_BLACK);
      }
      oled.setCursor(kColX[col], kRowY[row]);
      oled.print(c.label);
      oled.setCursor(vx, kRowY[row]);
      oled.print(value);
      if (sel) oled.setTextColor(SH110X_WHITE);
    }
  }

  oled.display();
}

void setup() {
  // --- inputs ---
  for (uint8_t i = 0; i < 12; i++) pinMode(kKeyPins[i], INPUT_PULLUP);
  pinMode(kEncSW, INPUT_PULLUP);
  pinMode(kEncA, INPUT_PULLUP);
  pinMode(kEncB, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(kEncA), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(kEncB), encISR, CHANGE);

  // --- USB: identity + both HID interfaces (gamepad IN + vendor telemetry) ---
  // One device, VID/PID = the TraceCraft vendor id so the recorder recognizes it.
  usb::setIdentity(proto::VENDOR_ID, proto::PRODUCT_ID, "TraceCraft", "TraceCraft MacroPad");
  gamepad.begin();                       // gamepad HID interface
  dash.begin(hid, tcStore, "TraceCraft MacroPad");   // vendor HID (telemetry) interface
  dash.setView(kPages[s_page].view);     // boot page's channels; re-announced after each descriptor
  usb::start();                          // enumerate

  // --- OLED ---
  SPI1.setSCK(26);
  SPI1.setTX(27);
  oled.begin(0, true);
  oled.setRotation(OLED_ROTATION);
  // GFX wraps by default: a glyph whose 6 px advance crosses the right edge jumps
  // to x=0 one text line down. The right column's last character ends on the
  // edge, so its "--" left a stray dash under every first-column label.
  oled.setTextWrap(false);
  oled.clearDisplay();
  oled.display();
}

// ---- knob taps --------------------------------------------------------------
// A sim samples its controllers once a frame or so, so a detent can't be a one-loop
// pulse: each becomes a press held kTapHoldMs then a release held kTapGapMs, which
// a sim reads as one distinct press. Detents arrive faster than that, so they queue
// (signed: + = increase), and turning back cancels queued taps before adding any.
static constexpr uint32_t kTapHoldMs = 50;
static constexpr uint32_t kTapGapMs  = 50;
static constexpr int      kTapQueueMax = 12;   // a hard spin can't run on for seconds
static int      s_tapQueue = 0;
static int      s_tapBit   = -1;               // button bit held right now, -1 = none
static uint32_t s_tapAt    = 0;                // when the current hold / gap began

// Select a cell. Queued taps belonged to the old one, so they're dropped; a tap
// already held finishes on its own button.
static void selectCell(int cell) {
  if (cell == s_sel) return;
  s_sel = cell;
  s_tapQueue = 0;
}

static void pumpTaps(uint32_t now) {
  if (s_tapBit >= 0) {
    if (now - s_tapAt >= kTapHoldMs) { s_tapBit = -1; s_tapAt = now; }
  } else if (s_tapQueue != 0 && now - s_tapAt >= kTapGapMs) {
    const bool up = s_tapQueue > 0;
    s_tapBit   = kDialBtnBase + 2 * s_sel + (up ? 0 : 1);
    s_tapQueue += up ? -1 : 1;
    s_tapAt    = now;
  }
}

// ---- knob press: debounced, pages on the press edge -----------------------------
static constexpr uint32_t kSwDebounceMs = 20;
static bool     s_swRaw = false, s_swDown = false;
static uint32_t s_swChangedAt = 0;
static uint16_t s_keysPrev = 0;

void loop() {
  dash.loop();                           // core: pump vendor USB + apply telemetry
  const uint32_t now = millis();
  bool redraw = false;

  // --- keys: buttons 1..12, and a press selects the cell above the key ---
  uint16_t keys = 0;
  for (uint8_t i = 0; i < 12; i++)
    if (digitalRead(kKeyPins[i]) == LOW) keys |= (uint16_t)(1u << i);
  const uint16_t pressed = keys & ~s_keysPrev;
  s_keysPrev = keys;
  for (int i = 0; i < 12; i++)
    if (pressed & (1u << i)) { selectCell(i); redraw = true; }

  // --- knob press flips to the next page (wrapping), on the first cell ---
  const bool swRaw = digitalRead(kEncSW) == LOW;
  if (swRaw != s_swRaw) { s_swRaw = swRaw; s_swChangedAt = now; }
  else if (swRaw != s_swDown && now - s_swChangedAt >= kSwDebounceMs) {
    s_swDown = swRaw;
    if (s_swDown) {
      s_page = (s_page + 1) % kPageCount;
      s_sel = 0;
      s_tapQueue = 0;
      redraw = true;
      // Ask the host for this page's channels only. Values the view didn't carry
      // show their last-seen state for a frame; these are settings, so that's fine.
      if (kPages[s_page].view) dash.setView(kPages[s_page].view);
    }
  }

  // --- knob rotation queues increase / decrease taps for the selected cell ---
  noInterrupts();
  const int d = s_encDelta;
  s_encDelta = 0;
  interrupts();
  if (d != 0) {
    s_tapQueue += d;
    if (s_tapQueue >  kTapQueueMax) s_tapQueue =  kTapQueueMax;
    if (s_tapQueue < -kTapQueueMax) s_tapQueue = -kTapQueueMax;
  }
  pumpTaps(now);

  // --- send the gamepad report when anything changed ---
  s_btn = (uint64_t)keys;
  if (s_tapBit >= 0) s_btn |= 1ull << s_tapBit;
  if (s_btn != s_btnSent && gamepad.ready()) {
    gamepad.sendReport(0, &s_btn, kReportBytes);   // little-endian: bits 0..47
    s_btnSent = s_btn;
  }

  // --- refresh the OLED at a modest rate, and at once on a page or selection change ---
  if (redraw || now - s_lastDraw >= kDrawIntervalMs) {
    s_lastDraw = now;
    drawDashboard();
  }
}
