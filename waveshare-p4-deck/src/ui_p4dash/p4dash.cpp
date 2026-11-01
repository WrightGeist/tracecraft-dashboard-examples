// TraceCraft P4 race dash — 1280x720 landscape on the Waveshare ESP32-P4 5" DSI panel.
//
// STYLE GUIDE: the recorder's Diagnostic dashboard. This dash borrows its vocabulary
// wholesale — same palette tokens, Geist Mono for every glyph (tabular digits),
// dark chrome panels, and its three signature idioms:
//
//   setting tiles   TC / TC CUT / ABS render as outlined pills: transparent fill,
//                   2px solid accent border, label AND value in the accent color
//                   (TC family = blue, ABS = red — the DDU brake-cluster color).
//                   When the driver clicks a dial the tile FLASHES: fills solid
//                   with the accent, text goes near-black, holds ~1s, fades.
//   activation      TC / ABS intervention lights are label-in-circle warning
//   lights          lights: neutral outlined ring + dim label when idle, filled
//                   accent + dark label while the aid fires. Color = status,
//                   never decoration — nothing is tinted at rest.
//   input feedback  the gear box invert-flashes on every gear change; the lap
//                   number flashes an amber pill on every S/F crossing; the
//                   sector disc (web-app sector palette) flashes white on entry.
//
// The RPM display is a converging shift-light row (F1-wheel style): 20 circular
// LEDs, two banks of 10 that fill from the outer edges toward the center and MEET
// IN THE MIDDLE exactly at max RPM, then all flash at the limiter. Unlit LEDs are
// dark chrome wells (same idle vocabulary as the activation lights); lit LEDs run
// green (outer 6) -> amber (2) -> red (innermost 2) per bank.
//
// Fonts are real compiled Geist Mono Bold subsets (fonts/ — see its NOTICE.md);
// big panels compile fonts instead of scaling 48px.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include "p4dash.h"
#include "tc_generated.h"

// P4 (1280x720) fonts — real compiled Geist Mono subsets.
LV_FONT_DECLARE(geist_mono_240);   // gear digit      (0-9 - N R)
LV_FONT_DECLARE(geist_mono_150);   // speed           (0-9)
LV_FONT_DECLARE(geist_mono_96);    // lap + sector    (0-9 -)
LV_FONT_DECLARE(geist_mono_56);    // TC / ABS tiles  (0-9 - F O)
LV_FONT_DECLARE(geist_mono_22);    // captions        (ASCII)

namespace {

// ---- palette — the Diagnostic dashboard's colors, verbatim ---------------
constexpr uint32_t kPageBg  = 0x0c0d10;
constexpr uint32_t kPanelBg = 0x16181c;
constexpr uint32_t kBorder  = 0x2a2d35;
constexpr uint32_t kBarBg   = 0x0a0b0d;   // gear box / bar wells
constexpr uint32_t kInk     = 0xf1f2f5;
constexpr uint32_t kInkMute = 0xdde0e6;   // panel titles
constexpr uint32_t kInkDim  = 0xb8bbc1;   // captions
constexpr uint32_t kInkMiss = 0x80848c;   // idle/missing annotations
constexpr uint32_t kGreen   = 0x4ade80;
constexpr uint32_t kAmber   = 0xfacc15;
constexpr uint32_t kRed     = 0xf87171;
constexpr uint32_t kBlue    = 0x38bdf8;   // TC / stability family
constexpr uint32_t kDark    = 0x0a0a0b;   // ink on bright fills
// Web-app sector palette (mirrored by the diagnostic dashboard's sector disc).
constexpr uint32_t kSectorColors[4] = { 0xd97c7c, 0x7aa3c8, 0xd4a05a, 0x7fb393 };

// ---- geometry (P4 1280x720) ----------------------------------------------------
constexpr int kW = 1280, kH = 720;
constexpr int kMargin = 28;
constexpr int kBarY = 24;
constexpr int kMidY = 136, kMidH = 356;
constexpr int kBotY = 566, kBotH = 126;
// RPM shift-light row: 2 banks x 10 LEDs converging on the center. A wider gap
// between the banks marks the meeting point. kLedMax sizes the widget arrays;
// the actual count/geometry is runtime (g_led*).
constexpr int kLedMax = 20;
constexpr int kLedN = 20, kLedPerSide = 10;
constexpr int kLedD = 46, kLedGap = 14, kLedCenterGap = 24;
constexpr int kLedRowW = kLedN * kLedD + (kLedN - 1) * kLedGap + kLedCenterGap;
constexpr int kLedX0 = (kW - kLedRowW) / 2;

// Runtime LED-row parameters — set by the setup, read by the tick.
// Default to the P4 row.
int g_ledN = kLedN, g_ledPerSide = kLedPerSide;

// ---- timing ----------------------------------------------------------------------
constexpr uint32_t kTextMs      = 50;     // fast readouts update at most 20 Hz
constexpr uint32_t kTileHoldMs  = 1000;   // setting-tile flash: hold solid...
constexpr uint32_t kTileFadeMs  = 150;    // ...then fade back to outlined
constexpr uint32_t kGearFlashMs = 360;    // gear invert-flash decay
constexpr uint32_t kLapFlashMs  = 1400;   // lap amber-pill decay
constexpr uint32_t kSecFlashMs  = 550;    // sector white-flash decay
constexpr int32_t  kAidHoldMs   = 150;    // keep an activation light lit past the last active frame

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// Lerp two 0xRRGGBB colors (t: 0 -> a, 1 -> b).
lv_color_t lerpHex(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0; if (t > 1) t = 1;
  const int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
  const int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
  return lv_color_make((uint8_t)(ar + (br - ar) * t),
                       (uint8_t)(ag + (bg - ag) * t),
                       (uint8_t)(ab + (bb - ab) * t));
}

void setHidden(lv_obj_t* o, bool hide) {
  if (!o) return;
  if (hide && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  if (!hide && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* makeBox(lv_obj_t* parent, int w, int h, uint32_t fill, int radius) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, hex(fill), 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* txt) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, hex(color), 0);
  lv_label_set_text(l, txt);
  return l;
}

// Uppercase tracked-out caption — the diagnostic's label voice.
lv_obj_t* makeCaption(lv_obj_t* parent, uint32_t color, const char* txt) {
  lv_obj_t* l = makeLabel(parent, &geist_mono_22, color, txt);
  lv_obj_set_style_text_letter_space(l, 3, 0);
  return l;
}

// Dark chrome panel with an uppercase title, per the diagnostic's Panel().
lv_obj_t* makePanel(lv_obj_t* parent, int x, int y, int w, int h, const char* title) {
  lv_obj_t* p = makeBox(parent, w, h, kPanelBg, 4);
  lv_obj_set_pos(p, x, y);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_color(p, hex(kBorder), 0);
  lv_obj_t* t = makeCaption(p, kInkMute, title);
  lv_obj_set_pos(t, 16, 12);
  return p;
}

// Fixed-width center-aligned value label (fixed width keeps proportional layout
// stable; Geist Mono digits are monospaced anyway, so this is belt-and-braces).
lv_obj_t* makeValue(lv_obj_t* parent, const lv_font_t* font, uint32_t color,
                    const char* txt, int boxW) {
  lv_obj_t* l = makeLabel(parent, font, color, txt);
  lv_obj_set_width(l, boxW);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  return l;
}

// ---- DDU setting tile (TC / TC CUT / ABS) ---------------------------------------
// Outlined pill at rest, solid accent flash on value change (see file header).
struct SettingTile {
  lv_obj_t* box = nullptr;
  lv_obj_t* cap = nullptr;
  lv_obj_t* val = nullptr;
  uint32_t  accent = 0;
  int32_t   prev = INT32_MIN;   // INT32_MIN = no value observed yet (don't flash on first data)
  uint32_t  flashMs = 0;
};

SettingTile makeTile(lv_obj_t* parent, int x, int y, int w, int h,
                     const lv_font_t* valFont, const lv_font_t* capFont,
                     uint32_t accent, const char* label) {
  SettingTile t;
  t.accent = accent;
  t.box = makeBox(parent, w, h, kPageBg, 8);
  lv_obj_set_pos(t.box, x, y);
  lv_obj_set_style_bg_opa(t.box, LV_OPA_TRANSP, 0);       // transparent at rest
  lv_obj_set_style_border_width(t.box, 2, 0);
  lv_obj_set_style_border_color(t.box, hex(accent), 0);
  t.cap = makeLabel(t.box, capFont, accent, label);
  lv_obj_set_style_text_letter_space(t.cap, 3, 0);
  lv_obj_align(t.cap, LV_ALIGN_TOP_MID, 0, h > 90 ? 12 : 6);
  t.val = makeValue(t.box, valFont, accent, "-", w - 12);
  lv_obj_align(t.val, LV_ALIGN_BOTTOM_MID, 0, h > 90 ? -14 : -6);
  return t;
}

// Value formatting shared by TC/ABS ("level" semantics: 0 = OFF) vs TC CUT (>0 or dash).
void formatLevel(char* out, size_t n, int32_t v, bool zeroIsOff) {
  if (v > 0) snprintf(out, n, "%ld", (long)v);
  else if (v == 0 && zeroIsOff) snprintf(out, n, "OFF");
  else snprintf(out, n, "-");
}

void tickTile(SettingTile& t, int32_t v, bool zeroIsOff, uint32_t nowMs) {
  if (v != t.prev) {
    const bool first = (t.prev == INT32_MIN);   // first data isn't a driver click
    t.prev = v;
    char b[8];
    formatLevel(b, sizeof(b), v, zeroIsOff);
    lv_label_set_text(t.val, b);
    if (!first) t.flashMs = nowMs;
  }
  if (t.flashMs == 0) return;
  const uint32_t el = nowMs - t.flashMs;
  if (el < kTileHoldMs) {                                  // solid accent pill
    lv_obj_set_style_bg_opa(t.box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t.box, hex(t.accent), 0);
    lv_obj_set_style_text_color(t.cap, hex(kDark), 0);
    lv_obj_set_style_text_color(t.val, hex(kDark), 0);
  } else if (el < kTileHoldMs + kTileFadeMs) {             // fade back to outlined
    const float k = (float)(el - kTileHoldMs) / kTileFadeMs;
    lv_obj_set_style_bg_color(t.box, lerpHex(t.accent, kPageBg, k), 0);
    lv_obj_set_style_bg_opa(t.box, (lv_opa_t)(255 * (1.0f - k)), 0);
    lv_obj_set_style_text_color(t.cap, lerpHex(kDark, t.accent, k), 0);
    lv_obj_set_style_text_color(t.val, lerpHex(kDark, t.accent, k), 0);
  } else {                                                 // at rest
    lv_obj_set_style_bg_opa(t.box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(t.cap, hex(t.accent), 0);
    lv_obj_set_style_text_color(t.val, hex(t.accent), 0);
    t.flashMs = 0;
  }
}

// ---- activation light (TC / ABS intervening) -------------------------------------
// Label-in-circle warning light: neutral ring idle, filled accent while firing.
struct ActLight {
  lv_obj_t* ring = nullptr;
  lv_obj_t* cap  = nullptr;
  uint32_t  accent = 0;
  uint32_t  lastActiveMs = 0;
  int       litPrev = -1;
};

ActLight makeLight(lv_obj_t* parent, int x, int y, int d,
                   const lv_font_t* capFont, uint32_t accent, const char* label) {
  ActLight a;
  a.accent = accent;
  a.ring = makeBox(parent, d, d, kPageBg, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(a.ring, x, y);
  lv_obj_set_style_bg_opa(a.ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(a.ring, 2, 0);
  lv_obj_set_style_border_color(a.ring, hex(kBorder), 0);
  a.cap = makeLabel(a.ring, capFont, kInkDim, label);
  lv_obj_set_style_text_letter_space(a.cap, 3, 0);
  lv_obj_center(a.cap);
  return a;
}

void tickLight(ActLight& a, bool active, uint32_t nowMs) {
  if (active) a.lastActiveMs = nowMs;
  const int lit = (a.lastActiveMs != 0 && nowMs - a.lastActiveMs < (uint32_t)kAidHoldMs) ? 1 : 0;
  if (lit == a.litPrev) return;
  a.litPrev = lit;
  lv_obj_set_style_bg_opa(a.ring, lit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(a.ring, hex(a.accent), 0);
  lv_obj_set_style_border_color(a.ring, hex(lit ? a.accent : kBorder), 0);
  lv_obj_set_style_text_color(a.cap, hex(lit ? kDark : kInkDim), 0);
}

// ---- widgets -----------------------------------------------------------------------
lv_obj_t* s_leds[kLedMax];
lv_obj_t* s_gearBox  = nullptr;
lv_obj_t* s_gear     = nullptr;
lv_obj_t* s_speed    = nullptr;
lv_obj_t* s_lapVal   = nullptr;
lv_obj_t* s_secDisc  = nullptr;
lv_obj_t* s_secNum   = nullptr;
lv_obj_t* s_flagSlot = nullptr;
lv_obj_t* s_flagText = nullptr;
SettingTile s_tcTile, s_cutTile, s_absTile;
ActLight    s_tcLight, s_absLight;

// ---- change-detection / animation state ---------------------------------------------
uint32_t s_ledColPrev[kLedMax];
char     s_gearPrev[8] = "";
bool     s_gearSeen    = false;
uint32_t s_gearFlashAt = 0;
uint32_t s_gearColor   = kInk;
int32_t  s_speedPrev   = -1;
uint32_t s_speedTxtMs  = 0;
int32_t  s_lapPrev     = INT32_MIN;
uint32_t s_lapFlashAt  = 0;
int      s_secIdx      = -1;
uint32_t s_secFlashAt  = 0;
int      s_flagPhasePrev = -1;

} // namespace

lv_obj_t* p4dash_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);   // own screen — the caller loads/pages it
  lv_obj_set_style_bg_color(scr, hex(kPageBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // --- RPM shift lights: two banks of dark LED wells converging on the center ---
  for (int i = 0; i < kLedN; i++) {
    lv_obj_t* led = makeBox(scr, kLedD, kLedD, kBarBg, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(led, 1, 0);
    lv_obj_set_style_border_color(led, hex(kBorder), 0);
    lv_obj_set_pos(led, kLedX0 + i * (kLedD + kLedGap) + (i >= kLedPerSide ? kLedCenterGap : 0), kBarY);
    s_leds[i] = led;
    s_ledColPrev[i] = kBarBg;
  }

  // --- middle band: SPEED panel | GEAR box | RACE STATE panel -------------------
  lv_obj_t* speedPanel = makePanel(scr, kMargin, kMidY, 380, kMidH, "SPEED");
  s_speed = makeValue(speedPanel, &geist_mono_150, kInk, "0", 340);
  lv_obj_align(s_speed, LV_ALIGN_CENTER, 0, -6);
  lv_obj_t* speedCap = makeCaption(speedPanel, kInkDim, "km/h");
  lv_obj_align(speedCap, LV_ALIGN_BOTTOM_MID, 0, -16);

  // Gear: the diagnostic's GearDisplay, scaled to hero size — near-black well,
  // chrome border, tabular digit (R red / N amber / ink), invert-flash on change.
  s_gearBox = makeBox(scr, 260, kMidH - 26, kBarBg, 8);
  lv_obj_set_pos(s_gearBox, 640 - 130, kMidY);
  lv_obj_set_style_border_width(s_gearBox, 1, 0);
  lv_obj_set_style_border_color(s_gearBox, hex(kBorder), 0);
  s_gear = makeValue(s_gearBox, &geist_mono_240, kInk, "N", 240);
  lv_obj_align(s_gear, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t* gearCap = makeCaption(scr, kInkDim, "GEAR");
  lv_obj_set_width(gearCap, 260);
  lv_obj_set_style_text_align(gearCap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(gearCap, 640 - 130, kMidY + kMidH - 12);

  lv_obj_t* racePanel = makePanel(scr, 810, kMidY, kW - kMargin - 810, kMidH, "RACE STATE");
  const int raceW = kW - kMargin - 810;
  lv_obj_t* lapCap = makeCaption(racePanel, kInkDim, "LAP");
  lv_obj_align(lapCap, LV_ALIGN_TOP_LEFT, raceW / 4 - 22, 64);
  s_lapVal = makeValue(racePanel, &geist_mono_96, kInk, "-", 200);
  lv_obj_set_style_radius(s_lapVal, 8, 0);               // amber pill on lap flash
  lv_obj_set_style_bg_opa(s_lapVal, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_ver(s_lapVal, 8, 0);
  lv_obj_align(s_lapVal, LV_ALIGN_LEFT_MID, raceW / 4 - 100, 24);
  lv_obj_t* secCap = makeCaption(racePanel, kInkDim, "SECTOR");
  lv_obj_align(secCap, LV_ALIGN_TOP_LEFT, 3 * raceW / 4 - 42, 64);
  s_secDisc = makeBox(racePanel, 150, 150, 0x1c1c1e, LV_RADIUS_CIRCLE);
  lv_obj_align(s_secDisc, LV_ALIGN_LEFT_MID, 3 * raceW / 4 - 75, 34);
  s_secNum = makeValue(s_secDisc, &geist_mono_96, kInkMiss, "-", 150);
  lv_obj_align(s_secNum, LV_ALIGN_CENTER, 0, 0);

  // --- bottom band: setting tiles + activation lights + flag slot ----------------
  s_tcTile  = makeTile(scr, kMargin, kBotY, 190, kBotH, &geist_mono_56, &geist_mono_22, kBlue, "TC");
  s_cutTile = makeTile(scr, kMargin + 206, kBotY, 190, kBotH, &geist_mono_56, &geist_mono_22, kBlue, "TC CUT");
  s_absTile = makeTile(scr, kMargin + 412, kBotY, 190, kBotH, &geist_mono_56, &geist_mono_22, kRed, "ABS");
  s_tcLight  = makeLight(scr, 690, kBotY + (kBotH - 104) / 2, 104, &geist_mono_22, kBlue, "TC");
  s_absLight = makeLight(scr, 810, kBotY + (kBotH - 104) / 2, 104, &geist_mono_22, kRed, "ABS");

  // Flag slot: reserved space like the diagnostic's flag cluster — dark well with
  // a dim label when idle, solid amber "YELLOW" while the caution flag flies.
  s_flagSlot = makeBox(scr, 280, 80, kBarBg, 8);
  lv_obj_set_pos(s_flagSlot, kW - kMargin - 280, kBotY + (kBotH - 80) / 2);
  lv_obj_set_style_border_width(s_flagSlot, 1, 0);
  lv_obj_set_style_border_color(s_flagSlot, hex(kBorder), 0);
  s_flagText = makeCaption(s_flagSlot, kInkMiss, "FLAG");
  lv_obj_center(s_flagText);

  return scr;
}

void p4dash_tick(bool waitingForData) {
  const uint32_t nowMs = millis();


  // --- resolve channels (same fallbacks as the diagnostic) -------------------------
  const int32_t rpm = tcStore.getInt(TC_CH_RPM);
  int32_t rpmMax = tcStore.getInt(TC_CH_RPM_MAX);
  if (rpmMax < 1000) rpmMax = 8000;

  // --- RPM shift lights: each bank fills outer -> inner from 45% of max, both
  // banks complete (meet in the middle) exactly at max RPM, and the whole row
  // blinks SOLID RED at the limiter. Zone colors per bank while climbing,
  // outer -> inner: 6 green, 2 amber, 2 red. Per-LED color cache keeps a quiet
  // row invalidation-free. ---------------------------------------------------------
  const int32_t lo = (int32_t)(rpmMax * 0.45f);
  float frac = (float)(rpm - lo) / (float)(rpmMax - lo);
  if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
  const int  litN    = (int)(frac * g_ledPerSide + 0.5f);      // lit per bank
  const bool atLimit = rpm >= (int32_t)(rpmMax * 0.99f);
  const bool flashOn = (nowMs / 90) % 2 == 0;
  const int  greenN  = (g_ledPerSide * 6 + 5) / 10;            // ~60% green
  const int  amberN  = (g_ledPerSide * 85 + 50) / 100;        // ~next 25% amber
  for (int i = 0; i < g_ledN; i++) {
    // Distance from the row's outer edge within this LED's bank (0 = outermost).
    const int pos = i < g_ledPerSide ? i : g_ledN - 1 - i;
    const bool on = atLimit ? flashOn : (pos < litN);
    uint32_t c;
    if (!on)             c = kBarBg;
    else if (atLimit)    c = kRed;   // limiter: the whole row goes red
    else if (pos < greenN) c = kGreen;
    else if (pos < amberN) c = kAmber;
    else                 c = kRed;
    if (c == s_ledColPrev[i]) continue;
    s_ledColPrev[i] = c;
    lv_obj_set_style_bg_color(s_leds[i], hex(c), 0);
    lv_obj_set_style_border_color(s_leds[i], hex(c == kBarBg ? kBorder : c), 0);
  }

  // --- gear: R red / N amber / ink, invert-flash on change --------------------------
  const char* gear = tcStore.getStr(TC_CH_GEAR);
  if (!gear || !gear[0]) gear = "N";
  if (strncmp(gear, s_gearPrev, sizeof(s_gearPrev) - 1) != 0) {
    strncpy(s_gearPrev, gear, sizeof(s_gearPrev) - 1);
    lv_label_set_text(s_gear, gear);
    s_gearColor = gear[0] == 'R' ? kRed : gear[0] == 'N' ? kAmber : kInk;
    if (s_gearSeen) s_gearFlashAt = nowMs;    // no flash on first data
    s_gearSeen = true;
  }
  if (s_gearFlashAt != 0) {
    const uint32_t el = nowMs - s_gearFlashAt;
    if (el < kGearFlashMs) {                  // inverted -> settle (ease-out-ish lerp)
      const float k = (float)el / kGearFlashMs;
      lv_obj_set_style_bg_color(s_gearBox, lerpHex(kInk, kBarBg, k), 0);
      lv_obj_set_style_text_color(s_gear, lerpHex(kDark, s_gearColor, k), 0);
      lv_obj_set_style_border_color(s_gearBox, lerpHex(kInk, kBorder, k), 0);
    } else {
      lv_obj_set_style_bg_color(s_gearBox, hex(kBarBg), 0);
      lv_obj_set_style_text_color(s_gear, hex(s_gearColor), 0);
      lv_obj_set_style_border_color(s_gearBox, hex(kBorder), 0);
      s_gearFlashAt = 0;
    }
  } else {
    lv_obj_set_style_text_color(s_gear, hex(s_gearColor), 0);
  }

  // --- speed (throttled) --------------------------------------------------------------
  const int32_t speed = tcStore.getInt(TC_CH_SPEED_KMH);
  if (speed != s_speedPrev && nowMs - s_speedTxtMs >= kTextMs) {
    s_speedPrev = speed; s_speedTxtMs = nowMs;
    char b[8];
    snprintf(b, sizeof(b), "%ld", (long)speed);
    lv_label_set_text(s_speed, b);
  }

  // --- lap: amber pill flash on every S/F crossing (the diagnostic's lap-flash) --------
  const int32_t lap = tcStore.getInt(TC_CH_LAP_NUM);
  if (lap != s_lapPrev) {
    const bool first = (s_lapPrev == INT32_MIN);
    s_lapPrev = lap;
    char b[8];
    if (lap > 0) snprintf(b, sizeof(b), "%ld", (long)lap); else snprintf(b, sizeof(b), "-");
    lv_label_set_text(s_lapVal, b);
    if (!first && lap > 0) s_lapFlashAt = nowMs;
  }
  if (s_lapFlashAt != 0) {
    const uint32_t el = nowMs - s_lapFlashAt;
    if (el < kLapFlashMs) {
      const float k = (float)el / kLapFlashMs;
      lv_obj_set_style_bg_opa(s_lapVal, (lv_opa_t)(255 * (1.0f - k)), 0);
      lv_obj_set_style_bg_color(s_lapVal, hex(kAmber), 0);
      lv_obj_set_style_text_color(s_lapVal, lerpHex(kDark, kInk, k), 0);
    } else {
      lv_obj_set_style_bg_opa(s_lapVal, LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(s_lapVal, hex(kInk), 0);
      s_lapFlashAt = 0;
    }
  }

  // --- sector disc: published sector, lap_dist_pct thirds as fallback ------------------
  int sc = (int)tcStore.getInt(TC_CH_SECTOR_COUNT);
  if (sc < 1) sc = 3;
  const int   s   = (int)tcStore.getInt(TC_CH_SECTOR);
  const float pct = tcStore.getFloat(TC_CH_LAP_DIST_PCT);
  int idx = (s > 0) ? s : (int)(pct * sc);
  if (idx < 0) idx = 0;
  if (idx >= sc) idx = sc - 1;
  if (waitingForData) {
    if (s_secIdx != -1) {
      s_secIdx = -1;
      lv_label_set_text(s_secNum, "-");
      lv_obj_set_style_bg_color(s_secDisc, hex(0x1c1c1e), 0);
      lv_obj_set_style_text_color(s_secNum, hex(kInkMiss), 0);
    }
  } else {
    if (idx != s_secIdx) {
      if (s_secIdx >= 0) s_secFlashAt = nowMs;
      s_secIdx = idx;
      char b[8];
      snprintf(b, sizeof(b), "%d", idx + 1);
      lv_label_set_text(s_secNum, b);
      lv_obj_set_style_text_color(s_secNum, hex(kDark), 0);
    }
    const uint32_t base = kSectorColors[idx % 4];
    const uint32_t el = nowMs - s_secFlashAt;
    lv_color_t fill = (s_secFlashAt != 0 && el < kSecFlashMs)
                        ? lerpHex(0xffffff, base, (float)el / kSecFlashMs)
                        : hex(base);
    lv_obj_set_style_bg_color(s_secDisc, fill, 0);
  }

  // --- setting tiles + activation lights ------------------------------------------------
  tickTile(s_tcTile,  tcStore.getInt(TC_CH_TC_LEVEL),  true,  nowMs);
  tickTile(s_cutTile, tcStore.getInt(TC_CH_TC_CUT),    false, nowMs);
  tickTile(s_absTile, tcStore.getInt(TC_CH_ABS_LEVEL), true,  nowMs);
  tickLight(s_tcLight,  tcStore.getBool(TC_CH_TC_ACTIVE),  nowMs);
  tickLight(s_absLight, tcStore.getBool(TC_CH_ABS_ACTIVE), nowMs);

  // --- flag slot: idle chrome well; solid pulsing amber while caution flies -------------
  const bool caution = tcStore.getBool(TC_CH_CAUTION);
  const int flagPhase = caution ? (int)((nowMs / 25) % 20) : -1;
  if (flagPhase != s_flagPhasePrev) {
    s_flagPhasePrev = flagPhase;
    if (caution) {
      const float k = (flagPhase < 10 ? flagPhase : 20 - flagPhase) / 10.0f;   // 0..1..0
      lv_obj_set_style_bg_color(s_flagSlot, lerpHex(kAmber, 0xffe066, k), 0);
      lv_obj_set_style_border_color(s_flagSlot, hex(kAmber), 0);
      lv_obj_set_style_text_color(s_flagText, hex(kDark), 0);
      lv_label_set_text(s_flagText, "YELLOW");
    } else {
      lv_obj_set_style_bg_color(s_flagSlot, hex(kBarBg), 0);
      lv_obj_set_style_border_color(s_flagSlot, hex(kBorder), 0);
      lv_obj_set_style_text_color(s_flagText, hex(kInkMiss), 0);
      lv_label_set_text(s_flagText, "FLAG");
    }
  }
}
