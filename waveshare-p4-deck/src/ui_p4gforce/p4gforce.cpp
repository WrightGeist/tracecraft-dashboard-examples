// P4 deck — G-FORCE page (1280x720 landscape).
//
// LEFT: the friction circle. A dot on lateral vs longitudinal G, with rings at
// 1/2/3 G and a comet trail, the same vocabulary as the T-Display gmeter dash.
//
// RIGHT: a separate vertical-G column. Deliberately NOT a third axis on the
// circle — grip is 2D, and the circle answers "am I using the tires", which is
// what a driver acts on mid-corner. Vertical answers "what is the track doing
// to the car": curb strikes, compressions, going light over a crest. Two
// questions, two readouts.
//
// SIGN CONVENTION — both indicators show where the driver's BODY is thrown (the
// inertia frame), not the raw acceleration vector, matching
// the G-meter dash:
//   braking      (lon < 0) -> dot UP toward the nose
//   acceleration (lon > 0) -> dot DOWN into the seat
// The COLUMN is the exception, and deliberately so: it does NOT use the inertia
// frame, because it is not a direction plot. It is a 1D magnitude readout, and
// for those, more reads as higher — the convention every aircraft G-meter uses
// (the instrument is a weight on a spring: pull positive G and the needle rises
// toward +4, push negative and it falls toward -1).
//   compression  (vert > 0) -> column dot UP    (more load)
//   unloading    (vert < 0) -> column dot DOWN  (less load)
// The deciding argument is internal consistency: the numeric readout beside it
// is POSITIVE under compression, so the dot has to move in the positive
// direction or the same page contradicts itself. An earlier cut had this
// inverted for consistency with the circle's inertia frame, which confused a 2D
// "where am I thrown" plot with a 1D "how loaded am I" gauge.
//
// accel_vert is GRAVITY-REMOVED (0 parked), so the column's center line is a
// true zero — no baseline offset to design around, and the resting dot sits
// exactly on the middle mark.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include "p4gforce.h"
#include "tc_generated.h"

LV_FONT_DECLARE(geist_mono_22);    // page title
LV_FONT_DECLARE(geist_sans_16);    // captions + axis numbers
// geist_mono_time_* is the family that carries '+', '-' AND '.' (it renders signed
// lap deltas). The plain geist_mono_56 is a digits+minus subset with NO plus and NO
// decimal point, so a "%+.1f" value rendered as box-0-box-3. Any signed decimal
// readout wants this family; check the -r range in a font's header before using it.
LV_FONT_DECLARE(geist_mono_time_72);   // value readouts (signed, one decimal)

namespace {

// ---- palette (house vocabulary) ---------------------------------------------
constexpr uint32_t kBg    = 0x0c0d10;
constexpr uint32_t kWell  = 0x14161a;
constexpr uint32_t kGrid  = 0x2a2e34;
constexpr uint32_t kInk   = 0xf1f2f5;
constexpr uint32_t kDim   = 0x8a919c;
constexpr uint32_t kCyan  = 0x22d3ee;   // the g-dot
constexpr uint32_t kAmber = 0xffd60a;   // compression peak
constexpr uint32_t kBlue  = 0x4da3ff;   // unloading peak

// ---- geometry (1280x720) -----------------------------------------------------
constexpr int kW = 1280, kH = 720;
constexpr int kCx = 430, kCy = 392;          // friction-circle center
constexpr int kR  = 250;                     // 3 G at the outer ring
constexpr float kRangeG = 3.0f;              // full scale, both plots
constexpr int kDotD = 26;
constexpr int kTrailN = 14;

// Vertical column, right-hand side.
constexpr int kColCx = 1108;                 // column center X
constexpr int kColW  = 74;
constexpr int kColTop = 150, kColBot = 634;
constexpr int kColCy = (kColTop + kColBot) / 2;
constexpr int kColHalf = (kColBot - kColTop) / 2;
constexpr int kVDotD = 34;

// ---- widgets + state ---------------------------------------------------------
lv_obj_t* s_dot = nullptr;
lv_obj_t* s_trail[kTrailN];
lv_obj_t* s_vdot = nullptr;
lv_obj_t* s_vPeakUp = nullptr;      // unloading peak marker
lv_obj_t* s_vPeakDn = nullptr;      // compression peak marker
lv_obj_t* s_latVal = nullptr;
lv_obj_t* s_lonVal = nullptr;
lv_obj_t* s_vertVal = nullptr;

float s_dispLat = 0, s_dispLon = 0, s_dispVert = 0;
float s_peakUp = 0, s_peakDn = 0;             // vertical session peaks (G)
uint32_t s_peakAtMs = 0;
int s_trailIdx = 0;
uint32_t s_lastTrailMs = 0;

// A peak that never fades turns into a high-water mark nobody can relate to a
// corner. Decay it slowly so it reads as "the worst hit recently", which is the
// useful version on a page you glance at.
constexpr uint32_t kPeakHoldMs = 4000;

lv_obj_t* mkBox(lv_obj_t* p, int x, int y, int w, int h, uint32_t color) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  return o;
}

lv_obj_t* mkLabel(lv_obj_t* p, int x, int y, const char* txt,
                  const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

// Hollow ring at a given G radius.
void mkRing(lv_obj_t* p, int g) {
  const int r = (int)lroundf(kR * (g / kRangeG));
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, r * 2, r * 2);
  lv_obj_set_pos(o, kCx - r, kCy - r);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, g == 3 ? 2 : 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(kGrid), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}

int gToX(float g) {
  float f = g / kRangeG;
  if (f < -1.0f) f = -1.0f;
  if (f > 1.0f) f = 1.0f;
  return kCx + (int)lroundf(f * kR);
}
// Inertia frame: positive lon (acceleration) presses you back -> DOWN.
int gToY(float g) {
  float f = g / kRangeG;
  if (f < -1.0f) f = -1.0f;
  if (f > 1.0f) f = 1.0f;
  return kCy + (int)lroundf(f * kR);
}
// NOT the same logic as the circle: the column is a magnitude gauge, so MORE
// load reads HIGHER (minus, because screen Y grows downward). Matches the
// positive sign of the readout beside it, and every G-meter ever built.
int vertToY(float g) {
  float f = g / kRangeG;
  if (f < -1.0f) f = -1.0f;
  if (f > 1.0f) f = 1.0f;
  return kColCy - (int)lroundf(f * kColHalf);
}

}  // namespace

lv_obj_t* p4gforce_setup() {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, kW, kH);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), 0);

  mkLabel(scr, 36, 28, "G-FORCE", &geist_mono_22, kInk);

  // ---- friction circle -------------------------------------------------------
  mkRing(scr, 1); mkRing(scr, 2); mkRing(scr, 3);
  mkBox(scr, kCx - kR, kCy, kR * 2, 1, kGrid);          // horizontal axis
  mkBox(scr, kCx, kCy - kR, 1, kR * 2, kGrid);          // vertical axis

  mkLabel(scr, kCx - 16, kCy - kR - 30, "BRAKE", &geist_sans_16, kDim);
  mkLabel(scr, kCx - 12, kCy + kR + 12, "ACCEL", &geist_sans_16, kDim);
  mkLabel(scr, kCx - kR - 46, kCy - 10, "LEFT", &geist_sans_16, kDim);
  mkLabel(scr, kCx + kR + 12, kCy - 10, "RIGHT", &geist_sans_16, kDim);

  for (int i = 0; i < kTrailN; i++) {
    s_trail[i] = lv_obj_create(scr);
    lv_obj_remove_style_all(s_trail[i]);
    lv_obj_set_size(s_trail[i], 10, 10);
    lv_obj_set_style_radius(s_trail[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_trail[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_trail[i], lv_color_hex(kCyan), 0);
    lv_obj_set_pos(s_trail[i], kCx - 5, kCy - 5);
  }

  s_dot = lv_obj_create(scr);
  lv_obj_remove_style_all(s_dot);
  lv_obj_set_size(s_dot, kDotD, kDotD);
  lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_dot, lv_color_hex(kCyan), 0);
  lv_obj_set_pos(s_dot, kCx - kDotD / 2, kCy - kDotD / 2);

  // ---- vertical column -------------------------------------------------------
  mkBox(scr, kColCx - kColW / 2, kColTop, kColW, kColBot - kColTop, kWell);
  mkBox(scr, kColCx - kColW / 2, kColCy, kColW, 2, kGrid);   // the zero line

  for (int g = 1; g <= 3; g++) {                             // ticks both ways
    for (int sign = -1; sign <= 1; sign += 2) {
      const int y = vertToY((float)(g * sign));
      mkBox(scr, kColCx - kColW / 2, y, g == 3 ? 14 : 9, 1, kGrid);
      mkBox(scr, kColCx + kColW / 2 - (g == 3 ? 14 : 9), y, g == 3 ? 14 : 9, 1, kGrid);
    }
  }

  // Ends labeled anyway — the scale is signed and unlabeled ticks are ambiguous.
  mkLabel(scr, kColCx - 52, kColTop - 34, "COMPRESSION", &geist_sans_16, kDim);
  mkLabel(scr, kColCx - 30, kColBot + 12, "LIGHT", &geist_sans_16, kDim);

  s_vPeakUp = mkBox(scr, kColCx - kColW / 2, kColCy, kColW, 2, kBlue);
  lv_obj_set_style_bg_opa(s_vPeakUp, LV_OPA_TRANSP, 0);
  s_vPeakDn = mkBox(scr, kColCx - kColW / 2, kColCy, kColW, 2, kAmber);
  lv_obj_set_style_bg_opa(s_vPeakDn, LV_OPA_TRANSP, 0);

  s_vdot = lv_obj_create(scr);
  lv_obj_remove_style_all(s_vdot);
  lv_obj_set_size(s_vdot, kVDotD, kVDotD);
  lv_obj_set_style_radius(s_vdot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_vdot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_vdot, lv_color_hex(kCyan), 0);
  lv_obj_set_pos(s_vdot, kColCx - kVDotD / 2, kColCy - kVDotD / 2);

  // ---- readouts --------------------------------------------------------------
  mkLabel(scr, 812, 176, "LAT", &geist_sans_16, kDim);
  s_latVal = mkLabel(scr, 812, 196, "--", &geist_mono_time_72, kInk);
  mkLabel(scr, 812, 316, "LON", &geist_sans_16, kDim);
  s_lonVal = mkLabel(scr, 812, 336, "--", &geist_mono_time_72, kInk);
  mkLabel(scr, 812, 456, "VERT", &geist_sans_16, kDim);
  s_vertVal = mkLabel(scr, 812, 476, "--", &geist_mono_time_72, kInk);

  return scr;
}

void p4gforce_tick(bool waiting) {
  const float rawLat  = waiting ? 0.0f : tcStore.getFloat(TC_CH_ACCEL_LAT);
  const float rawLon  = waiting ? 0.0f : tcStore.getFloat(TC_CH_ACCEL_LON);
  const float rawVert = waiting ? 0.0f : tcStore.getFloat(TC_CH_ACCEL_VERT);

  // Displayed position glides; peaks track the RAW signal. Vertical is the
  // twitchiest of the three (curb strikes are milliseconds), so the dot is
  // damped harder than the circle's — otherwise it is a blur — and the peak
  // markers are what actually carry the transient.
  s_dispLat  += (rawLat  - s_dispLat)  * 0.18f;
  s_dispLon  += (rawLon  - s_dispLon)  * 0.18f;
  s_dispVert += (rawVert - s_dispVert) * 0.14f;

  const uint32_t now = millis();
  if (!waiting) {
    if (rawVert > s_peakDn) { s_peakDn = rawVert; s_peakAtMs = now; }
    if (rawVert < s_peakUp) { s_peakUp = rawVert; s_peakAtMs = now; }
  }
  if (now - s_peakAtMs > kPeakHoldMs) {        // bleed back toward zero
    s_peakDn *= 0.97f;
    s_peakUp *= 0.97f;
  }

  lv_obj_set_pos(s_dot, gToX(s_dispLat) - kDotD / 2, gToY(s_dispLon) - kDotD / 2);
  lv_obj_set_pos(s_vdot, kColCx - kVDotD / 2, vertToY(s_dispVert) - kVDotD / 2);

  // Peak markers: hidden until there is something worth marking, so a parked
  // car shows a clean column rather than two lines pinned to the center.
  // Naming note: "Dn"/"Up" are the SIGNS (compression / unloading), not screen
  // directions — since the flip, compression draws at the TOP.
  const bool showDn = s_peakDn > 0.15f, showUp = s_peakUp < -0.15f;
  lv_obj_set_style_bg_opa(s_vPeakDn, showDn ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(s_vPeakUp, showUp ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  if (showDn) lv_obj_set_pos(s_vPeakDn, kColCx - kColW / 2, vertToY(s_peakDn));
  if (showUp) lv_obj_set_pos(s_vPeakUp, kColCx - kColW / 2, vertToY(s_peakUp));

  // Trail: sample the smoothed position into a ring every few frames.
  if (now - s_lastTrailMs > 60) {
    s_lastTrailMs = now;
    s_trailIdx = (s_trailIdx + 1) % kTrailN;
    lv_obj_set_pos(s_trail[s_trailIdx], gToX(s_dispLat) - 5, gToY(s_dispLon) - 5);
    for (int i = 0; i < kTrailN; i++) {
      const int age = (s_trailIdx - i + kTrailN) % kTrailN;
      lv_obj_set_style_bg_opa(s_trail[i],
          waiting ? LV_OPA_TRANSP : (lv_opa_t)(LV_OPA_COVER * (kTrailN - age) / (kTrailN * 3)), 0);
    }
  }

  char buf[16];
  if (waiting) {
    lv_label_set_text(s_latVal, "--");
    lv_label_set_text(s_lonVal, "--");
    lv_label_set_text(s_vertVal, "--");
  } else {
    snprintf(buf, sizeof buf, "%+.1f", (double)rawLat);  lv_label_set_text(s_latVal, buf);
    snprintf(buf, sizeof buf, "%+.1f", (double)rawLon);  lv_label_set_text(s_lonVal, buf);
    snprintf(buf, sizeof buf, "%+.1f", (double)rawVert); lv_label_set_text(s_vertVal, buf);
  }
}
