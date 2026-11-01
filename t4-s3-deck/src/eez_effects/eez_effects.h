#pragma once
// Presentation effects for the EEZ-Studio dashboards (apps/eez_dash, apps/t4_deck).
//
// EEZ no-flow can bind a widget's value and flags to a channel, but not its style
// colors or a time-based animation. These hooks add that with a little LVGL, each
// behind its own -D TRACECRAFT_EFFECT_* flag, against the widget identifiers the
// EEZ projects share (rpm_arc, fuel_bar, shift_lights, ...).
//
// Include once, from the dashboard's main.cpp, AFTER the EEZ export's ui.h and
// tc_generated.h: the effects read `objects.*` and the TC_CH_* channel indices.
#include <math.h>
#include <TraceCraft.h>

#if defined(TRACECRAFT_EFFECT_RPM_ARC)
// --- Rev-limit arc effect ---------------------------------------------------
// EEZ no-flow can bind a widget's value/flags to a variable, but NOT its style
// colors or a time-based animation — those need a little LVGL. This hook reddens
// the RPM arc near the redline: the indicator turns red and the track pulses
// between its design color and bright red. The trigger is data-driven (the live
// `rpm` channel vs the arc's own max), so there's no extra channel or EEZ edit.
static lv_color_t s_arcInd, s_arcTrack;     // arc's design colors, captured once
static int        s_arcState = -1;          // -1 unknown, 0 normal, 1 at-limit

static void captureArcColors() {
  s_arcInd   = lv_obj_get_style_arc_color(objects.rpm_arc, LV_PART_INDICATOR);
  s_arcTrack = lv_obj_get_style_arc_color(objects.rpm_arc, LV_PART_MAIN);
}

static void applyArcLimitEffect() {
  lv_obj_t*     arc     = objects.rpm_arc;
  const int32_t maxr    = lv_arc_get_max_value(arc);
  const int32_t redline = (int32_t)(maxr * 0.95f);
  const bool    atLimit = tcStore.getInt(TC_CH_RPM) >= redline;

  if (atLimit) {
    if (s_arcState != 1) {                  // indicator -> red (once)
      lv_obj_set_style_arc_color(arc, lv_color_hex(0xFF0000), LV_PART_INDICATOR);
      s_arcState = 1;
    }
    uint32_t ph = millis() % 320;           // ~3 Hz triangle
    float k = ph / 320.0f;
    k = k < 0.5f ? k * 2.0f : 2.0f - k * 2.0f;            // 0 -> 1 -> 0
    lv_color_t track = lv_color_mix(lv_color_hex(0xFF0000), s_arcTrack,
                                    (uint8_t)(k * 255.0f));
    lv_obj_set_style_arc_color(arc, track, LV_PART_MAIN); // pulse design<->red
  } else if (s_arcState != 0) {             // restore design colors (once)
    lv_obj_set_style_arc_color(arc, s_arcInd,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, s_arcTrack, LV_PART_MAIN);
    s_arcState = 0;
  }
}
#endif

#if defined(TRACECRAFT_EFFECT_RPM_DOTS)
// --- RPM gauge rendered as a dotted arc -------------------------------------
// Replaces the solid rpm_arc with a ring of dots tracing the same sweep. The
// EEZ widget stays (still ticked, still the source of the min/max range) but
// its arc/knob are hidden; the firmware lays N dots along the track centerline
// and lights them up to the live RPM fraction, cyan filling to a red redline
// flash — same color language as the solid arc, just beaded.
#ifndef TC_RPM_DOTS
#define TC_RPM_DOTS 36
#endif
static lv_obj_t* s_rpmDots[TC_RPM_DOTS];
static int  s_rpmDotN = 0;
static int  s_rpmLitPrev = -2;
static bool s_rpmLimitPrev = false, s_rpmFlashPrev = false;
static constexpr uint32_t kRpmDotOff = 0x202020;
static constexpr uint32_t kRpmDotOn  = 0x00BCD4;   // matches the arc's design cyan

static void setupRpmDots() {
  lv_obj_t* arc = objects.rpm_arc;
  if (!arc) return;
  lv_obj_update_layout(arc);
  const float aw = lv_obj_get_width(arc);
  const float ah = lv_obj_get_height(arc);
  const float trackW = lv_obj_get_style_arc_width(arc, LV_PART_MAIN);
  const float cx = lv_obj_get_x(arc) + aw / 2.0f;
  const float cy = lv_obj_get_y(arc) + ah / 2.0f;
  const float r  = aw / 2.0f - trackW / 2.0f;        // track centerline
  // Hide the solid arc — the dots stand in for it. Keep the widget itself so
  // EEZ's tick still writes the value and lv_arc_get_max_value stays valid.
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

  // LVGL arc default sweep: 135deg (bottom-left) clockwise 270deg to 45deg
  // (bottom-right), gap centered at the bottom. Angle math is y-down clockwise,
  // so plain cos/sin trace the same path.
  const float start = 135.0f, sweep = 270.0f;
  const int   n = TC_RPM_DOTS;
  const int   d = 16;                                // dot diameter
  s_rpmDotN = n;
  lv_obj_t* parent = lv_obj_get_parent(arc);
  for (int i = 0; i < n; i++) {
    const float t = (n > 1) ? (float)i / (n - 1) : 0.0f;
    const float a = (start + sweep * t) * 3.14159265f / 180.0f;
    const int x = (int)(cx + r * cosf(a) - d / 2.0f + 0.5f);
    const int y = (int)(cy + r * sinf(a) - d / 2.0f + 0.5f);
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, d, d);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_radius(dot, d / 2, 0);          // square + radius => circle
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kRpmDotOff), 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    s_rpmDots[i] = dot;
  }
}

static void applyRpmDots() {
  if (!s_rpmDotN) return;
  const int32_t maxr = lv_arc_get_max_value(objects.rpm_arc);
  const int32_t rpm  = tcStore.getInt(TC_CH_RPM);
  const int32_t redl = (int32_t)(maxr * 0.97f);
  float frac = maxr > 0 ? (float)rpm / (float)maxr : 0.0f;
  if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
  const int  lit     = (int)(frac * s_rpmDotN + 0.5f);
  const bool atLimit = rpm >= redl;
  const bool flashOn = (millis() / 90) % 2 == 0;

  if (lit == s_rpmLitPrev && atLimit == s_rpmLimitPrev &&
      (!atLimit || flashOn == s_rpmFlashPrev)) return;   // no visible change
  s_rpmLitPrev = lit; s_rpmLimitPrev = atLimit; s_rpmFlashPrev = flashOn;

  for (int i = 0; i < s_rpmDotN; i++) {
    const bool on = atLimit ? flashOn : (i < lit);
    lv_color_t c;
    if (!on)                              c = lv_color_hex(kRpmDotOff);
    else if (atLimit)                     c = lv_color_hex(0xFF0000);
    else if (i >= s_rpmDotN * 90 / 100)   c = lv_color_hex(0xFF3030);   // top dots warn
    else                                  c = lv_color_hex(kRpmDotOn);
    lv_obj_set_style_bg_color(s_rpmDots[i], c, 0);
  }
}
#endif // TRACECRAFT_EFFECT_RPM_DOTS

#if defined(TRACECRAFT_EFFECT_SHIFT_LIGHTS)
// --- Shift-light bar --------------------------------------------------------
// A row of LEDs that fills green -> amber -> red as RPM climbs toward the
// redline, then flashes them all at the limiter. Per-segment color over time
// isn't expressible via EEZ no-flow binding, so the firmware renders it: you
// place an empty container named `shift_lights` in EEZ (position + size it),
// and this draws the LEDs into it and drives them from the live `rpm` channel.
#define TC_SHIFT_LEDS 32   // MAX dot capacity; the actual count is computed to pack the strip
#ifndef TC_RPM_MAX
#define TC_RPM_MAX 8000   // redline for the shift-light strip; override per dashboard build.
                          // Decoupled from any widget so the strip needs no rpm_arc present.
#endif
static lv_obj_t* s_leds[TC_SHIFT_LEDS];
static int  s_ledCount = 0;   // dots actually created (depends on strip width)
static int  s_litPrev = -2;
static bool s_shiftLimitPrev = false, s_shiftFlashPrev = false;

static void setupShiftLights() {
  lv_obj_t* bar = objects.shift_lights;
  if (!bar) return;
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_update_layout(bar);                       // resolve width/height first
  int W = lv_obj_get_width(bar);
  int H = lv_obj_get_height(bar);
  int d = H;                                   // dot diameter = strip height
  if (d < 2) d = 2;
  // Pack as many dots as fit across the strip with tight spacing (race-dash style),
  // then center the row. Count is dynamic (capped at TC_SHIFT_LEDS).
  int gap = d / 3; if (gap < 2) gap = 2;
  int n = (W + gap) / (d + gap);
  if (n > TC_SHIFT_LEDS) n = TC_SHIFT_LEDS;
  if (n < 1) n = 1;
  s_ledCount = n;
  int rowW = n * d + (n - 1) * gap;
  int x0 = (W - rowW) / 2;
  if (x0 < 0) x0 = 0;
  for (int i = 0; i < n; i++) {
    lv_obj_t* led = lv_obj_create(bar);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, d, d);                // square box + radius d/2 => circle
    lv_obj_set_pos(led, x0 + i * (d + gap), 0);
    lv_obj_set_style_radius(led, d / 2, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(led, lv_color_hex(0x202020), 0);
    lv_obj_remove_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(led, LV_OBJ_FLAG_CLICKABLE);
    s_leds[i] = led;
  }
}

static void applyShiftLights() {
  if (!objects.shift_lights) return;
  const int32_t maxr = TC_RPM_MAX;
  const int32_t rpm  = tcStore.getInt(TC_CH_RPM);
  const int32_t lo   = (int32_t)(maxr * 0.55f);    // strip starts filling here
  const int32_t redl = (int32_t)(maxr * 0.97f);    // limiter -> all flash
  float frac = (maxr > lo) ? (float)(rpm - lo) / (float)(maxr - lo) : 0.0f;
  if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
  const int  lit     = (int)(frac * s_ledCount + 0.5f);
  const bool atLimit = rpm >= redl;
  const bool flashOn = (millis() / 90) % 2 == 0;

  if (lit == s_litPrev && atLimit == s_shiftLimitPrev &&
      (!atLimit || flashOn == s_shiftFlashPrev)) return;   // no visible change
  s_litPrev = lit; s_shiftLimitPrev = atLimit; s_shiftFlashPrev = flashOn;

  for (int i = 0; i < s_ledCount; i++) {
    bool on = atLimit ? flashOn : (i < lit);
    lv_color_t c;
    if (!on)                              c = lv_color_hex(0x202020);          // off
    else if (i < s_ledCount * 6 / 10)     c = lv_palette_main(LV_PALETTE_GREEN);
    else if (i < s_ledCount * 85 / 100)   c = lv_palette_main(LV_PALETTE_AMBER);
    else                                  c = lv_palette_main(LV_PALETTE_RED);
    lv_obj_set_style_bg_color(s_leds[i], c, 0);
  }
}
#endif

#if defined(TRACECRAFT_EFFECT_CAUTION_FLAG)
// --- Caution (yellow) flag --------------------------------------------------
// A waving yellow flag shown while the `caution` channel is true. It's built
// from vertical strips whose brightness and vertical offset follow a traveling
// sine, giving a rippling-cloth look with no image assets. You place an empty
// container named `caution_flag` in EEZ; the firmware fills + animates it and
// hides it when caution clears.
#define TC_FLAG_STRIPS 16
static lv_obj_t* s_flag = nullptr;
static lv_obj_t* s_flagStrip[TC_FLAG_STRIPS];
static int       s_flagH = 0, s_flagStripH = 0;

static void setupCautionFlag() {
  lv_obj_t* box = objects.caution_flag;
  if (!box) return;
  s_flag = box;
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);   // only the strips are visible
  lv_obj_update_layout(box);
  const int W = lv_obj_get_width(box);
  s_flagH      = lv_obj_get_height(box);
  s_flagStripH = (int)(s_flagH * 0.78f);
  for (int i = 0; i < TC_FLAG_STRIPS; i++) {
    lv_obj_t* s = lv_obj_create(box);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, W / TC_FLAG_STRIPS + 1, s_flagStripH);
    lv_obj_set_pos(s, i * (W / TC_FLAG_STRIPS), (s_flagH - s_flagStripH) / 2);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    s_flagStrip[i] = s;
  }
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);          // hidden until caution
}

static void applyCautionFlag() {
  if (!s_flag) return;
  const bool caution = tcStore.getBool(TC_CH_CAUTION);
  if (!caution) {
    if (!lv_obj_has_flag(s_flag, LV_OBJ_FLAG_HIDDEN))
      lv_obj_add_flag(s_flag, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (lv_obj_has_flag(s_flag, LV_OBJ_FLAG_HIDDEN))
    lv_obj_remove_flag(s_flag, LV_OBJ_FLAG_HIDDEN);

  const float t     = millis() / 1000.0f;
  const int   baseY = (s_flagH - s_flagStripH) / 2;
  const float amp   = s_flagH * 0.12f;
  for (int i = 0; i < TC_FLAG_STRIPS; i++) {
    const float ph   = t * 5.0f - i * 0.5f;                            // traveling wave
    const float edge = 0.3f + 0.7f * (float)i / (TC_FLAG_STRIPS - 1);  // free edge waves more
    lv_obj_set_y(s_flagStrip[i], baseY + (int)(amp * edge * sinf(ph)));
    const float   b = 0.5f + 0.5f * sinf(ph + 0.5f);                   // 0..1 fold shading
    const uint8_t w = (uint8_t)(120 + 135 * b);                        // 120..255 yellow weight
    lv_obj_set_style_bg_color(s_flagStrip[i],
        lv_color_mix(lv_palette_main(LV_PALETTE_YELLOW), lv_color_black(), w), 0);
  }
}
#endif

#if defined(TRACECRAFT_EFFECT_FUEL_BAR)
// --- Low-fuel warning -------------------------------------------------------
// Colors the fuel bar's fill by level — green (design) -> amber -> red -> a
// flashing red when critically low. Driven by the live `fuel_pct` channel; the
// design fill color is captured lazily so "normal" restores whatever you chose
// in EEZ. Presentation-only, no extra channel.
static lv_color_t s_fuelColor;
static bool       s_fuelCaptured = false;
static int        s_fuelZone = -1;
static bool       s_fuelFlashPrev = false;

static void applyFuelBar() {
  lv_obj_t* bar = objects.fuel_bar;
  if (!bar) return;
  if (!s_fuelCaptured) {
    s_fuelColor = lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR);
    s_fuelCaptured = true;
  }
  const int32_t pct = tcStore.getInt(TC_CH_FUEL_PCT);
  int zone;                                   // 0 ok, 1 getting low, 2 low, 3 critical
  if      (pct > 30) zone = 0;
  else if (pct > 15) zone = 1;
  else if (pct > 8)  zone = 2;
  else               zone = 3;
  const bool flashOn = (millis() / 150) % 2 == 0;        // ~3 Hz

  if (zone == s_fuelZone && (zone != 3 || flashOn == s_fuelFlashPrev)) return;
  s_fuelZone = zone; s_fuelFlashPrev = flashOn;

  lv_color_t c;
  switch (zone) {
    case 0:  c = s_fuelColor;                       break;   // design (green)
    case 1:  c = lv_palette_main(LV_PALETTE_AMBER); break;   // getting low
    case 2:  c = lv_palette_main(LV_PALETTE_RED);   break;   // low
    default: c = flashOn ? lv_color_hex(0xFF0000) : lv_color_hex(0x400000);  // critical
  }
  lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
}
#endif

#if !defined(TRACECRAFT_BOARD_TDISPLAY_S3_LONG)
// Post-EEZ gauge tuning for the 450x600 dash: the export's fuel strip (27px)
// reads thinner than the RPM arc's 48px track. Match them — slim the arc to a
// 40px track (shrinking it to 368px to keep its outer margin) and grow the
// fuel bar to the same 40px, re-pinned to the bottom edge. Runtime restyle
// after ui_init keeps the generated EEZ export pristine (screens.c is never
// hand-edited).
static void tuneDashGauges() {
  lv_obj_t* arc = objects.rpm_arc;
  lv_obj_t* bar = objects.fuel_bar;
  if (!arc || !bar) return;
  lv_obj_set_size(arc, 368, 368);
  lv_obj_set_pos(arc, 41, 190);
  lv_obj_set_style_arc_width(arc, 40, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_arc_width(arc, 16, LV_PART_INDICATOR | LV_STATE_DEFAULT);  // keep the 18/48 design ratio
  const int barTop = 550, barH = 40;
  lv_obj_set_size(bar, 352, barH);
  lv_obj_set_pos(bar, 49, barTop);
  // E / F end labels: match their vertical center to the bar's. They're
  // ALIGN_CENTER (offset from screen center = 300 for the 600px export), so
  // offset_y = barCenterY - 300. X positions stay as the export placed them.
  const int labelOffY = (barTop + barH / 2) - 300;
  // Tone the E/F end labels down: the export ships them full-white, which
  // glares next to the muted readouts. Dim gray reads as a quiet axis label.
  const lv_color_t labelGray = lv_color_hex(0x707070);
  if (objects.obj1) {  // "E"
    lv_obj_set_pos(objects.obj1, -194, labelOffY);
    lv_obj_set_style_text_color(objects.obj1, labelGray, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (objects.obj2) {  // "F"
    lv_obj_set_pos(objects.obj2, 195, labelOffY);
    lv_obj_set_style_text_color(objects.obj2, labelGray, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}
#endif

#if defined(TRACECRAFT_EFFECT_TELLTALES)
#include "icons/telltale_icons.h"   // A8 tell-tale icons
// --- Tell-tale lights -------------------------------------------------------
// Six status lamps flanking the speed readout, three per side: turn signals
// (the channels carry the sim's flash cycle, so hazards blink both), headlight
// stage, wipers, and TC/ABS intervention. Pure firmware on top of the EEZ tree,
// same pattern as the other effects. All six are recolorable A8 icons baked
// from assets/icons/ SVGs, always present and dim when idle — a dark cockpit
// lamp reads as "equipped, off"; absence reads as broken.
static lv_obj_t* s_ttTurnL  = nullptr;
static lv_obj_t* s_ttTurnR  = nullptr;
static lv_obj_t* s_ttLights = nullptr;
static lv_obj_t* s_ttWipers = nullptr;
static lv_obj_t* s_ttTc     = nullptr;
static lv_obj_t* s_ttAbs    = nullptr;

// Idle (unlit) glyph color — deliberately very dark so an inactive lamp reads
// as "present but off" without competing with lit lamps or the live readouts.
static constexpr uint32_t kTtDim = 0x161616;

// Recolorable A8 icon lamp: alpha comes from the baked bitmap, color from the
// style image-recolor — one asset serves the dim and lit states.
static lv_obj_t* ttIcon(const lv_image_dsc_t* src, int x, int y) {
  lv_obj_t* o = lv_image_create(objects.main);
  lv_image_set_src(o, src);
  lv_obj_set_style_align(o, LV_ALIGN_CENTER, 0);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(o, lv_color_hex(kTtDim), 0);
  return o;
}

static void setupTellTales() {
  // Three glyphs per side, evenly stacked with comfortable breathing room.
  s_ttTurnL  = ttIcon(&tc_icon_blinker_left,  -150, -222);
  s_ttLights = ttIcon(&tc_icon_headlights,    -150, -168);
  s_ttWipers = ttIcon(&tc_icon_wipers,        -150, -114);
  s_ttTurnR  = ttIcon(&tc_icon_blinker_right,  150, -222);
  s_ttTc     = ttIcon(&tc_icon_traction,       150, -168);
  s_ttAbs    = ttIcon(&tc_icon_abs,            150, -112);
}

// Icon lamp: recolor the A8 bitmap dim <-> lit.
static void ttIconLamp(lv_obj_t* icon, uint8_t& shown, uint8_t state, uint32_t litColor) {
  if (state == shown) return;
  shown = state;
  lv_obj_set_style_image_recolor(icon, lv_color_hex(state ? litColor : kTtDim), 0);
}

// Turn-signal lamp with a FIRMWARE-driven blink. ACC reports the indicator as
// a steady "engaged" boolean (not the flash cycle the recorder comment once
// assumed), so we generate the blink locally: latch "engaged" for a hold
// window (which also bridges the off-phase if a future sim DOES pulse the
// signal), then toggle on a fixed cadence. Both sides read the same millis()
// phase, so hazards blink in sync. ~1.5 Hz, like a real relay.
static void ttBlinker(lv_obj_t* icon, bool raw, uint32_t& lastTrue,
                      uint8_t& shown, uint32_t litColor) {
  const uint32_t now = millis();
  if (raw) lastTrue = now;
  const bool engaged = lastTrue != 0 && (now - lastTrue) < 700;   // hold bridges any off-phase
  const uint8_t want = (engaged && (now / 330) % 2 == 0) ? 1 : 0; // ~1.5 Hz
  ttIconLamp(icon, shown, want, litColor);
}

static void applyTellTales() {
  static uint8_t sL = 0xFF, sR = 0xFF, sHl = 0xFF, sWp = 0xFF, sTc = 0xFF, sAb = 0xFF;
  static uint32_t tL = 0, tR = 0;

  ttBlinker(s_ttTurnL, tcStore.getBool(TC_CH_TURN_LEFT),  tL, sL, 0x35D07F);
  ttBlinker(s_ttTurnR, tcStore.getBool(TC_CH_TURN_RIGHT), tR, sR, 0x35D07F);

  // Headlight stage is tri-state: 0 off (dim) / 1 low (green) / 2+ high (blue).
  int hs = (int)tcStore.getInt(TC_CH_HEADLIGHTS);
  hs = hs < 0 ? 0 : hs > 2 ? 2 : hs;
  if ((uint8_t)hs != sHl) {
    sHl = (uint8_t)hs;
    const uint32_t c = hs == 0 ? kTtDim : hs == 1 ? 0x35D07F : 0x4C9AFF;
    lv_obj_set_style_image_recolor(s_ttLights, lv_color_hex(c), 0);
  }

  ttIconLamp(s_ttWipers, sWp, tcStore.getInt(TC_CH_WIPERS) > 0 ? 1 : 0, 0x22D3EE);
  ttIconLamp(s_ttTc,  sTc, tcStore.getBool(TC_CH_TC_ACTIVE)  ? 1 : 0, 0xFBBF24);
  ttIconLamp(s_ttAbs, sAb, tcStore.getBool(TC_CH_ABS_ACTIVE) ? 1 : 0, 0xF97316);
}
#endif // TRACECRAFT_EFFECT_TELLTALES

#if defined(TRACECRAFT_EFFECT_TIRE_TEMPS)
// --- Per-corner tire temperatures -------------------------------------------
// Writes each corner's temperature (°C) into its box (objects.tire_fl/fr/rl/rr)
// and colors the box by zone: blue (cold) -> green (optimal) -> amber (hot) ->
// red (over). The channels are integers and the boxes are PLAIN labels whose text
// the firmware sets — labels never bind to an int channel (LVGL labels are string-
// only, and there's no flow engine here to convert). You place four labels named
// tire_fl/tire_fr/tire_rl/tire_rr in EEZ; this fills + colors them.
static lv_color_t tireColor(int32_t t) {
  if (t < 70)   return lv_color_hex(0x2196F3);   // cold   (blue)
  if (t <= 100) return lv_color_hex(0x2E7D32);   // optimal (green)
  if (t <= 110) return lv_color_hex(0xF59E0B);   // hot    (amber)
  return lv_color_hex(0xD32F2F);                  // over   (red)
}
static void applyTireOne(lv_obj_t* box, int32_t t, int32_t* prev) {
  if (!box || t == *prev) return;
  *prev = t;
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld", (long)t);
  lv_label_set_text(box, buf);
  lv_obj_set_style_bg_color(box, tireColor(t), 0);
}
// Pressure readout (kPa channel -> PSI text) flanking each tire box. Text only —
// the pressure labels are plain (no colored box).
static void applyTirePress(lv_obj_t* lbl, int32_t kpa, int32_t* prev) {
  if (!lbl || kpa == *prev) return;
  *prev = kpa;
  int psi = (int)(kpa * 0.14504f + 0.5f);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", psi);
  lv_label_set_text(lbl, buf);
}
static void applyTireTemps() {
  static int32_t pfl = -1000, pfr = -1000, prl = -1000, prr = -1000;
  applyTireOne(objects.tire_fl, tcStore.getInt(TC_CH_TIRE_TEMP_FL), &pfl);
  applyTireOne(objects.tire_fr, tcStore.getInt(TC_CH_TIRE_TEMP_FR), &pfr);
  applyTireOne(objects.tire_rl, tcStore.getInt(TC_CH_TIRE_TEMP_RL), &prl);
  applyTireOne(objects.tire_rr, tcStore.getInt(TC_CH_TIRE_TEMP_RR), &prr);
  static int32_t qfl = -1000, qfr = -1000, qrl = -1000, qrr = -1000;
  applyTirePress(objects.tire_press_fl, tcStore.getInt(TC_CH_TIRE_PRESS_FL), &qfl);
  applyTirePress(objects.tire_press_fr, tcStore.getInt(TC_CH_TIRE_PRESS_FR), &qfr);
  applyTirePress(objects.tire_press_rl, tcStore.getInt(TC_CH_TIRE_PRESS_RL), &qrl);
  applyTirePress(objects.tire_press_rr, tcStore.getInt(TC_CH_TIRE_PRESS_RR), &qrr);
}
#endif // TIRE_TEMPS
