// T-Display S3 (320x170 landscape) — the G-dot: a 2D friction-circle plot.
//
// Design: the whole panel is the G field. X = lateral (the axis a driver
// shapes lap-over-lap, so it gets the full 320px width — ~49 px/G), Y =
// longitudinal (braking UP, accel down — ~26 px/G; the compressed fidelity is
// a deliberate trade, lateral is what we read precisely). The dot is the
// DRIVER'S HEAD in the inertia frame — it goes where the forces throw you:
// forward (up) under braking, back (down) on power, outward in a corner. A
// fading trail keeps the last ~three-quarter second of the G story visible —
// the sweep through a corner reads at a glance.
//
// Around the field: graduated ticks on both axes (numerals on X at 1/2/3 G,
// on Y at 1/2 G), live numeric readouts floating in the top corners (LAT
// left, LON right), session-peak ghosts on the X axis for max-left/right
// lateral plus one on -Y for peak braking. ACC/BRK caption the Y direction.
//
// Sign conventions (channel manifest): accel_lat negative = LEFT, positive =
// RIGHT; accel_lon positive = accel, negative = braking. Screen mapping is the
// FELT force, consistent across both axes — see lonToY.
//
// Motion: the DISPLAYED position is low-passed (~1/3 per frame) so the dot
// glides; the peak ghosts track the RAW signal so a real spike is never
// smoothed out of the record. Dot color follows the combined |G| vector.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include "gmeter.h"
#include "tc_generated.h"
#include "tracecraft/usb/UsbHid.h"   // usb::log() -- CDC echo of the rate

LV_FONT_DECLARE(geist_mono_22);   // corner readouts (full ASCII — has '.')
LV_FONT_DECLARE(geist_sans_14);   // axis numerals + LAT/LON/ACC/BRK captions

namespace {

// ---- palette (house vocabulary) -------------------------------------------------
constexpr uint32_t kBg      = 0x0c0d10;
constexpr uint32_t kInk     = 0xf1f2f5;   // dot + readout values (calm)
constexpr uint32_t kDim     = 0x5a5d63;   // numerals, captions, major ticks
constexpr uint32_t kTick    = 0x3a3e46;   // axis lines + minor ticks
constexpr uint32_t kGhost   = 0x854d0e;   // session-peak ghosts (dim amber)

// Dot color ramp: green (coasting) -> yellow (~1.5 G) -> red (the grip edge).
// Continuous, not bucketed — the hue itself is the load meter.
constexpr uint8_t kRampG[3] = { 0x30, 0xd1, 0x58 };   // green  (0 G)
constexpr uint8_t kRampY[3] = { 0xfa, 0xcc, 0x15 };   // yellow (mid)
constexpr uint8_t kRampR[3] = { 0xf8, 0x71, 0x71 };   // red    (edge)
constexpr float   kRampFullG = 2.7f;                  // fully red from here up

// Combined |G| -> ramp color. Piecewise RGB lerp, green->yellow->red.
uint32_t rampColor(float mag) {
  float t = mag / kRampFullG;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const uint8_t* a; const uint8_t* b; float f;
  if (t < 0.5f) { a = kRampG; b = kRampY; f = t * 2.0f; }
  else          { a = kRampY; b = kRampR; f = (t - 0.5f) * 2.0f; }
  const uint8_t rr = (uint8_t)(a[0] + (b[0] - a[0]) * f);
  const uint8_t gg = (uint8_t)(a[1] + (b[1] - a[1]) * f);
  const uint8_t bb = (uint8_t)(a[2] + (b[2] - a[2]) * f);
  return ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | bb;
}

// ---- geometry (320x170) ---------------------------------------------------------
constexpr int kW = 320, kH = 170;
constexpr int kCx = 160, kCy = 85;               // plot origin (0 G, 0 G)
constexpr float kRange = 3.0f;                   // ±3 G both axes
constexpr int kXHalf = 148;                      // ±3 G lateral -> ±148 px (~49 px/G)
constexpr int kYHalf = 77;                       // ±3 G longitudinal -> ±77 px (~26 px/G)

constexpr int kDotD   = 24;                      // dot diameter
// The trail is a comet tail: many closely-sampled dots that taper in size and
// fade with age, so it reads as one smooth ribbon rather than scattered dots.
constexpr int kTrailN = 32;                      // trail dots (~1.5 s of history)
constexpr int kTrailEveryTicks = 2;              // sample every other frame

// G -> screen, clamped to the field.
int latToX(float g) {
  float f = g / kRange;
  if (f < -1.0f) f = -1.0f;
  if (f >  1.0f) f =  1.0f;
  return kCx + (int)lroundf(f * kXHalf);
}
// The dot maps to where the driver's HEAD is thrown (the inertia frame), not
// the acceleration vector: braking (negative lon) pitches you FORWARD -> dot
// UP toward the nose; acceleration presses you back -> dot DOWN. This matches
// how the lateral axis reads on screen.
int lonToY(float g) {
  float f = g / kRange;
  if (f < -1.0f) f = -1.0f;
  if (f >  1.0f) f =  1.0f;
  return kCy + (int)lroundf(f * kYHalf);
}

// ---- widgets + state ------------------------------------------------------------
lv_obj_t* s_dot    = nullptr;
lv_obj_t* s_trail[kTrailN];
lv_obj_t* s_latVal = nullptr;
lv_obj_t* s_lonVal = nullptr;
lv_obj_t* s_ghostL = nullptr;    // session peak lateral, left
lv_obj_t* s_ghostR = nullptr;    // session peak lateral, right
lv_obj_t* s_ghostB = nullptr;    // session peak braking (on -Y)
lv_obj_t* s_rate   = nullptr;    // measured link rate, bottom-center
lv_obj_t* s_lost   = nullptr;    // dropped frames/s, under the rate
float s_dispLat = 0.0f, s_dispLon = 0.0f;
float s_peakL = 0.0f, s_peakR = 0.0f, s_peakB = 0.0f;
int16_t s_histX[kTrailN], s_histY[kTrailN];      // trail ring (newest at s_histHead)
uint8_t s_histHead = 0, s_histCount = 0, s_tickN = 0;
int  s_latShown = INT32_MIN, s_lonShown = INT32_MIN;   // centi-G caches
uint32_t s_dotColShown = 0;
bool s_wasWaiting = true;

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

lv_obj_t* mkRect(lv_obj_t* p, int w, int h, uint32_t col, int radius) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, uint32_t col, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hex(col), 0);
  lv_label_set_text(l, t);
  return l;
}

} // namespace

void gmeter_setup(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Axis lines through the origin.
  lv_obj_t* ax = mkRect(scr, kXHalf * 2 + 1, 1, kTick, 0);
  lv_obj_set_pos(ax, kCx - kXHalf, kCy);
  lv_obj_t* ay = mkRect(scr, 1, kYHalf * 2 + 1, kTick, 0);
  lv_obj_set_pos(ay, kCx, kCy - kYHalf);

  // X graduations: minor every 0.5 G, major every 1 G with a numeral below.
  for (int h = -6; h <= 6; h++) {
    if (h == 0) continue;                        // the Y line marks zero
    const float g = h * 0.5f;
    const int x = latToX(g);
    const bool major = (h % 2 == 0);
    lv_obj_t* t = mkRect(scr, major ? 2 : 1, major ? 11 : 7, major ? kDim : kTick, 0);
    lv_obj_set_pos(t, x - (major ? 1 : 0), kCy - (major ? 5 : 3));
    if (major) {
      char b[4]; snprintf(b, sizeof(b), "%d", abs(h) / 2);
      lv_obj_t* n = mkLabel(scr, &geist_sans_14, kDim, b);
      lv_obj_set_width(n, 20);
      lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(n, x - 10, kCy + 8);
    }
  }
  // Y graduations: majors only (1/2/3 G each way); numerals at 1 and 2 —
  // the 3 G ticks hug the panel edges where a numeral would clip.
  for (int g = -3; g <= 3; g++) {
    if (g == 0) continue;
    const int y = lonToY((float)g);
    lv_obj_t* t = mkRect(scr, 11, 2, kDim, 0);
    lv_obj_set_pos(t, kCx - 5, y - 1);
    if (abs(g) <= 2) {
      char b[4]; snprintf(b, sizeof(b), "%d", abs(g));
      lv_obj_t* n = mkLabel(scr, &geist_sans_14, kDim, b);
      lv_obj_set_width(n, 16);
      lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(n, kCx - 26, y - 7);
    }
  }

  // Y-direction captions, tucked right of the axis at its ends. BRK on top:
  // braking throws the head forward, so the dot climbs under braking.
  lv_obj_t* brk = mkLabel(scr, &geist_sans_14, kDim, "BRK");
  lv_obj_set_pos(brk, kCx + 8, kCy - kYHalf - 1);
  lv_obj_t* acc = mkLabel(scr, &geist_sans_14, kDim, "ACC");
  lv_obj_set_pos(acc, kCx + 8, kCy + kYHalf - 12);

  // Corner readouts: LAT top-left, LON top-right (floating over the field).
  lv_obj_t* latCap = mkLabel(scr, &geist_sans_14, kDim, "LAT");
  lv_obj_set_pos(latCap, 12, 8);
  s_latVal = mkLabel(scr, &geist_mono_22, kDim, "--");
  lv_obj_set_pos(s_latVal, 12, 24);
  // Link-rate readout, bottom-center under the circle. Deliberately dim and small:
  // it is instrumentation, not a driving readout, and the G dot must stay the thing
  // your eye lands on. Sits at the bottom because the corners are already LAT/LON
  // and the vertical axis runs through the middle.
  s_rate = mkLabel(scr, &geist_sans_14, kDim, "-- Hz");
  lv_obj_align(s_rate, LV_ALIGN_BOTTOM_MID, 0, -14);
  s_lost = mkLabel(scr, &geist_sans_14, kDim, "");
  lv_obj_align(s_lost, LV_ALIGN_BOTTOM_MID, 0, -2);

  lv_obj_t* lonCap = mkLabel(scr, &geist_sans_14, kDim, "LON");
  lv_obj_set_width(lonCap, 40);
  lv_obj_set_style_text_align(lonCap, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(lonCap, kW - 12 - 40, 8);
  s_lonVal = mkLabel(scr, &geist_mono_22, kDim, "--");
  lv_obj_set_width(s_lonVal, 72);
  lv_obj_set_style_text_align(s_lonVal, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_lonVal, kW - 12 - 72, 24);

  // Session-peak ghosts (hidden until a real peak exists).
  s_ghostL = mkRect(scr, 3, 14, kGhost, 1);
  s_ghostR = mkRect(scr, 3, 14, kGhost, 1);
  s_ghostB = mkRect(scr, 14, 3, kGhost, 1);
  lv_obj_add_flag(s_ghostL, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_ghostR, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_ghostB, LV_OBJ_FLAG_HIDDEN);

  // Trail dots (oldest drawn first so the live dot rides on top), then the dot.
  // Comet taper: newest ~11px at ~50% opacity, shrinking + fading to 3px / ~2%.
  for (int i = 0; i < kTrailN; i++) {
    const int d = 11 - (i * 8) / (kTrailN - 1);             // 11 -> 3 px
    s_trail[i] = mkRect(scr, d, d, kInk, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_trail[i], (lv_opa_t)(130 - i * 4), 0);   // 130 -> 6
    lv_obj_add_flag(s_trail[i], LV_OBJ_FLAG_HIDDEN);
  }
  s_dot = mkRect(scr, kDotD, kDotD, kInk, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_dot, kCx - kDotD / 2, kCy - kDotD / 2);
}

void gmeter_rate(uint32_t framesApplied, uint8_t seq) {
  static uint32_t winMs   = 0;      // start of the current one-second window
  static uint32_t frames0 = 0;      // framesApplied at the window start
  static uint32_t drops   = 0;      // SEQ gaps seen this window
  static uint8_t  lastSeq = 0;
  static bool     primed  = false;

  const uint32_t now = millis();
  if (!primed) {
    primed = true; lastSeq = seq; frames0 = framesApplied; winMs = now;
    return;
  }

  if (seq != lastSeq) {
    // Unsigned 8-bit subtraction wraps for free, so 254 -> 1 reads as a gap of 3
    // and needs no special case.
    const uint8_t step = (uint8_t)(seq - lastSeq);
    if (step > 1) drops += (uint32_t)(step - 1);
    lastSeq = seq;
  }

  if (now - winMs < 1000) return;

  const uint32_t got = framesApplied - frames0;
  char buf[32];
  if (got == 0 && drops == 0) {
    lv_label_set_text(s_rate, "-- Hz");
    lv_label_set_text(s_lost, "");
  } else {
    snprintf(buf, sizeof(buf), "%lu Hz", (unsigned long)got);
    lv_label_set_text(s_rate, buf);
    // Only shown when something was actually lost. A permanent "0 lost" trains the
    // eye to skip the line, which is the one line that must be noticed if it moves.
    if (drops) {
      snprintf(buf, sizeof(buf), "%lu lost/s", (unsigned long)drops);
      lv_label_set_text(s_lost, buf);
      lv_obj_set_style_text_color(s_lost, lv_color_hex(0xF87171), 0);
    } else {
      lv_label_set_text(s_lost, "");
    }
  }
  // Echo to the CDC console as well as the glass. The screen is the point of this
  // readout, but a number you have to photograph is a number nobody records: the
  // same line on serial makes the measurement capturable from the PC, which is what
  // turns "it looked fast" into a logged result.
  tracecraft::usb::log().printf("[rate] %lu Hz  %lu lost/s\n",
                    (unsigned long)got, (unsigned long)drops);

  winMs = now; frames0 = framesApplied; drops = 0;
}

void gmeter_tick(bool waiting) {
  const float rawLat = waiting ? 0.0f : tcStore.getFloat(TC_CH_ACCEL_LAT);
  const float rawLon = waiting ? 0.0f : tcStore.getFloat(TC_CH_ACCEL_LON);

  // Displayed position glides; peaks track the raw signal.
  // Heavier damping than the first cut (0.35): the dot glides — readable
  // motion, no telemetry jitter — and the trail it tows stays a clean arc.
  s_dispLat += (rawLat - s_dispLat) * 0.18f;
  s_dispLon += (rawLon - s_dispLon) * 0.18f;
  if (!waiting) {
    if (rawLat < s_peakL) s_peakL = rawLat;
    if (rawLat > s_peakR) s_peakR = rawLat;
    if (rawLon < s_peakB) s_peakB = rawLon;      // braking is negative lon
  }

  const int dx = latToX(s_dispLat), dy = lonToY(s_dispLon);
  lv_obj_set_pos(s_dot, dx - kDotD / 2, dy - kDotD / 2);

  // Trail: sample the smoothed position every few ticks into a ring, then lay
  // the dots newest-to-oldest behind the live dot. Hidden while waiting.
  if (!waiting && ++s_tickN >= kTrailEveryTicks) {
    s_tickN = 0;
    s_histHead = (uint8_t)((s_histHead + 1) % kTrailN);
    s_histX[s_histHead] = (int16_t)dx;
    s_histY[s_histHead] = (int16_t)dy;
    if (s_histCount < kTrailN) s_histCount++;
    for (int i = 0; i < kTrailN; i++) {
      if (i >= s_histCount) { lv_obj_add_flag(s_trail[i], LV_OBJ_FLAG_HIDDEN); continue; }
      const int idx = (s_histHead - i + kTrailN) % kTrailN;
      lv_obj_remove_flag(s_trail[i], LV_OBJ_FLAG_HIDDEN);
      const int d = 11 - (i * 8) / (kTrailN - 1);
      lv_obj_set_pos(s_trail[i], s_histX[idx] - d / 2, s_histY[idx] - d / 2);
    }
  }

  // Peak ghosts (>0.3 G keeps noise off the axes).
  if (s_peakL < -0.3f) {
    lv_obj_remove_flag(s_ghostL, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_ghostL, latToX(s_peakL) - 1, kCy - 7);
  }
  if (s_peakR > 0.3f) {
    lv_obj_remove_flag(s_ghostR, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_ghostR, latToX(s_peakR) - 1, kCy - 7);
  }
  if (s_peakB < -0.3f) {
    lv_obj_remove_flag(s_ghostB, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_ghostB, kCx - 7, lonToY(s_peakB) - 1);
  }

  // Corner readouts, cached in centi-G. Lat shows magnitude (direction is the
  // dot); lon keeps its sign — negative IS the braking number drivers quote.
  const int latC = waiting ? INT32_MIN + 1 : (int)lroundf(fabsf(s_dispLat) * 100.0f);
  if (latC != s_latShown) {
    s_latShown = latC;
    char b[10];
    if (waiting) snprintf(b, sizeof(b), "--");
    else         snprintf(b, sizeof(b), "%d.%02d", latC / 100, latC % 100);
    lv_label_set_text(s_latVal, b);
  }
  const int lonC = waiting ? INT32_MIN + 1 : (int)lroundf(s_dispLon * 100.0f);
  if (lonC != s_lonShown) {
    s_lonShown = lonC;
    char b[10];
    if (waiting) {
      snprintf(b, sizeof(b), "--");
    } else {
      // Explicit sign: "%+d" on lonC/100 prints "+0" for -0.99..-0.01 (integer
      // division rounds toward zero), showing braking as positive. Never that.
      snprintf(b, sizeof(b), "%c%d.%02d",
               lonC < 0 ? '-' : '+', abs(lonC) / 100, abs(lonC) % 100);
    }
    lv_label_set_text(s_lonVal, b);
  }

  // Dot + trail color rides the ramp with the combined vector. The ramp is
  // continuous, so quantize |G| to ~0.05 G steps before comparing — otherwise
  // every frame is a "change" and 17 style writes/frame invalidate constantly.
  const float mag = sqrtf(s_dispLat * s_dispLat + s_dispLon * s_dispLon);
  const uint32_t col = waiting ? kDim : rampColor(mag);
  const uint32_t colQ = waiting ? kDim
      : rampColor((float)((int)(mag * 20.0f)) / 20.0f);
  if (colQ != s_dotColShown) {
    s_dotColShown = colQ;
    lv_obj_set_style_bg_color(s_dot, hex(col), 0);
    lv_obj_set_style_text_color(s_latVal, hex(col == kDim ? kDim : kInk), 0);
    lv_obj_set_style_text_color(s_lonVal, hex(col == kDim ? kDim : kInk), 0);
    for (int i = 0; i < kTrailN; i++) lv_obj_set_style_bg_color(s_trail[i], hex(col), 0);
  }

  // Entering the waiting state: park the dot, hide the trail.
  if (waiting != s_wasWaiting) {
    s_wasWaiting = waiting;
    if (waiting) {
      s_dispLat = s_dispLon = 0.0f;
      s_histCount = 0; s_tickN = 0;
      for (int i = 0; i < kTrailN; i++) lv_obj_add_flag(s_trail[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}
