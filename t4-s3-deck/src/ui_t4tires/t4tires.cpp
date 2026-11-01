// T4-S3 deck — TIRES + BRAKES page (450x600 portrait). A faithful LVGL port of
// the local dashboard's Tires + brakes panel (its
// corners panel): top-down car view, 2x2 corner tiles outboard with the two
// brake-disc bars of each axle side-by-side in the middle for left/right
// comparison. Same color vocabulary, same thresholds, same missing-data rules.
//
// Per corner tile: a vertical WEAR bar on the OUTBOARD edge (full = fresh
// tread, drains as the tire wears), tread-temperature band cells (I/M/O when
// the sim publishes inner/outer bands — iRacing carcass + LMU; a single
// full-width cell on ACC / AC EVO), and the pressure in psi (the unit racers
// actually set pressures in; the wire stays canonical kPa).
//
// Damage states (LMU only, live-only channels):
//   wheel_detached_* -> readings BLANKED, red X + DETACHED (absent beats wrong:
//                       once the wheel is off, its numbers measure nothing).
//   tire_flat_*      -> readings STAY LIVE, red outline + FLAT badge (pressure
//                       and temps are how you confirm a puncture — keep the
//                       evidence on screen).
//
// An unpublished channel decodes as 0 and an older recorder can't say it is absent
// (only one that sends presence can, Store::has()), so a zero reading is treated
// as "not reported" — a real tire is never 0 degC / 0 kPa.
// Missing values render as "--" in the dimmest ink, keeping their full slot so
// the geometry never shifts between sims.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "t4tires.h"
#include "tc_generated.h"

extern const lv_font_t ui_font_geist_16;       // deck heading voice (EEZ export; ASCII-only)
// Geist SemiBold subsets WITH the degree glyph (0x20-0x7E + 0xB0): 16/24 are
// shared with the P4 pages (ui_p4dash/fonts, pulled in by the env's
// build_src_filter); 10/36 live in this page's fonts/ dir. The EEZ-exported
// ui_font_geist_* have no degree glyph — don't print "NN°" with those.
LV_FONT_DECLARE(geist_sans_10);   // small labels, band letters, badges
LV_FONT_DECLARE(geist_sans_16);   // band + brake values
LV_FONT_DECLARE(geist_sans_24);   // pressure
LV_FONT_DECLARE(geist_sans_36);   // collapsed single-cell temp
// Montserrat appears ONLY for LV_SYMBOL_CLOSE (the detached X) — the
// FontAwesome symbol glyphs exist solely in LVGL's built-in fonts.

namespace {

// ---- palette — mirrors dashboard-ui COLORS + the bucket tables ------------------
constexpr uint32_t kPageBg   = 0x0c0d10;
constexpr uint32_t kTileBg   = 0x0d0e11;   // neutral tile / "unknown" bucket bg
constexpr uint32_t kBorder   = 0x2a2d35;
constexpr uint32_t kInk      = 0xf1f2f5;
constexpr uint32_t kInkMute  = 0xdde0e6;
constexpr uint32_t kInkDim   = 0xb8bbc1;
constexpr uint32_t kInkMiss  = 0x80848c;   // the em-dash ink (device uses "--")
constexpr uint32_t kRed      = 0xf87171;
constexpr uint32_t kAmber    = 0xfacc15;
constexpr uint32_t kBadgeInk = 0x0a0a0b;   // dark text on the red FLAT badge
constexpr uint32_t kBarBg    = 0x0a0b0d;   // wear/brake bar wells

struct Bucket { uint32_t bg, ink; };
// Tire tread temp buckets (degC) — generic GT3-ish envelope, same as local dash.
Bucket tireBucket(int t) {
  if (t <= 0)   return { kTileBg,  kInkMiss };   // unknown / not reported
  if (t < 70)   return { 0x1e3a8a, 0xdbeafe };   // cold (blue)
  if (t < 100)  return { 0x166534, 0xdcfce7 };   // optimal (green)
  if (t < 115)  return { 0x854d0e, 0xfef3c7 };   // hot (amber)
  return          { 0x7f1d1d, 0xfee2e2 };        // critical (red)
}
// Brake disc temp buckets (degC) — same palette family, its own thresholds.
Bucket brakeBucket(int t) {
  if (t <= 0)   return { kTileBg,  kInkMiss };
  if (t < 100)  return { 0x1e3a8a, 0xdbeafe };   // cold (blue)
  if (t < 300)  return { 0x155e75, 0xcffafe };   // warm (cyan)
  if (t < 650)  return { 0x166534, 0xdcfce7 };   // optimal (green)
  if (t < 800)  return { 0x854d0e, 0xfef3c7 };   // hot (amber)
  return          { 0x7f1d1d, 0xfee2e2 };        // critical (red)
}
constexpr int kBrakeFullScaleC = 1000;   // bar fill: temp/1000, clamped

// kPa -> psi, display only (wire stays canonical kPa). One decimal — whole psi
// is too coarse for the ~27 psi GT3 window.
constexpr float kKpaToPsi = 0.1450377f;

// ---- geometry (450x600) ---------------------------------------------------------
constexpr int kW = 450;
constexpr int kTileW = 162, kBrkW = 46, kGap = 6, kMar = 8;
constexpr int kTileLX = kMar;                          // 8
constexpr int kBrkLX  = kTileLX + kTileW + kGap;       // 176
constexpr int kBrkRX  = kBrkLX + kBrkW + kGap;         // 228
constexpr int kTileRX = kBrkRX + kBrkW + kGap;         // 280
constexpr int kRowY[2] = { 40, 308 };
constexpr int kRowH = 258;
constexpr int kWearW = 16;
constexpr int kBandY = 6, kBandH = 156;                // band-cell region in the tile
constexpr int kPressY = 176;                           // pressure row top

constexpr uint32_t kTickMs = 250;

// ---- per-corner channel map -----------------------------------------------------
// barLeft: wear bar on the OUTBOARD edge (left wheels -> left edge). The FLAT
// badge goes on the opposite (inner) top corner, exactly like the local dash.
struct CornerCh {
  int t, tin, tout, press, wear, brake, flat, det;
  bool barLeft;
};
const CornerCh kC[4] = {
  // FL
  { TC_CH_TIRE_TEMP_FL, TC_CH_TIRE_TEMP_IN_FL, TC_CH_TIRE_TEMP_OUT_FL,
    TC_CH_TIRE_PRESS_FL, TC_CH_TIRE_WEAR_FL, TC_CH_BRAKE_TEMP_FL,
    TC_CH_TIRE_FLAT_FL, TC_CH_WHEEL_DETACHED_FL, true },
  // FR
  { TC_CH_TIRE_TEMP_FR, TC_CH_TIRE_TEMP_IN_FR, TC_CH_TIRE_TEMP_OUT_FR,
    TC_CH_TIRE_PRESS_FR, TC_CH_TIRE_WEAR_FR, TC_CH_BRAKE_TEMP_FR,
    TC_CH_TIRE_FLAT_FR, TC_CH_WHEEL_DETACHED_FR, false },
  // RL
  { TC_CH_TIRE_TEMP_RL, TC_CH_TIRE_TEMP_IN_RL, TC_CH_TIRE_TEMP_OUT_RL,
    TC_CH_TIRE_PRESS_RL, TC_CH_TIRE_WEAR_RL, TC_CH_BRAKE_TEMP_RL,
    TC_CH_TIRE_FLAT_RL, TC_CH_WHEEL_DETACHED_RL, true },
  // RR
  { TC_CH_TIRE_TEMP_RR, TC_CH_TIRE_TEMP_IN_RR, TC_CH_TIRE_TEMP_OUT_RR,
    TC_CH_TIRE_PRESS_RR, TC_CH_TIRE_WEAR_RR, TC_CH_BRAKE_TEMP_RR,
    TC_CH_TIRE_FLAT_RR, TC_CH_WHEEL_DETACHED_RR, false },
};
// Grid slots: corner i -> tile x, brake x, row y.
const int kTileX4[4] = { kTileLX, kTileRX, kTileLX, kTileRX };
const int kBrkX4[4]  = { kBrkLX,  kBrkRX,  kBrkLX,  kBrkRX  };
const int kRow4[4]   = { kRowY[0], kRowY[0], kRowY[1], kRowY[1] };

// ---- widgets --------------------------------------------------------------------
lv_obj_t* s_tile[4];
lv_obj_t* s_bandCell[4][3];   // I/M/O cell bg
lv_obj_t* s_bandVal[4][3];    // I/M/O value labels
lv_obj_t* s_bandLtr[4][3];    // I/M/O letter labels
lv_obj_t* s_single[4];        // collapsed single-cell bg (no-band sims)
lv_obj_t* s_singleVal[4];
lv_obj_t* s_press[4];
lv_obj_t* s_wearBar[4];       // well
lv_obj_t* s_wearFill[4];
lv_obj_t* s_wearTxt[4];       // vertical "WEAR"
lv_obj_t* s_wearPct[4];       // % worn readout
lv_obj_t* s_flatBadge[4];
lv_obj_t* s_detX[4];
lv_obj_t* s_detTxt[4];
lv_obj_t* s_brkBase[4];
lv_obj_t* s_brkFill[4];
lv_obj_t* s_brkVal[4];
lv_obj_t* s_brkUnit[4];
lv_obj_t* s_brkX[4];          // detached X in the brake column

// ---- change caches --------------------------------------------------------------
uint32_t s_tickMs = 0;
int  s_shBand[4][3];          // shown band temps (INT32_MIN = "--")
int  s_shSingle[4];
int  s_shPress[4];            // shown pressure in kPa (INT32_MIN = "--")
int  s_shWear[4];
int  s_shBrake[4];
int8_t s_shBands[4];          // -1 unset, 0 single, 1 three-band
int8_t s_shFlat[4];           // -1 unset
int8_t s_shDet[4];

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, uint32_t col, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hex(col), 0);
  lv_label_set_text(l, t);
  return l;
}

lv_obj_t* mkBox(lv_obj_t* p, uint32_t bg, uint32_t border, int radius) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_style_bg_color(o, hex(bg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  if (border) {
    lv_obj_set_style_border_color(o, hex(border), 0);
    lv_obj_set_style_border_width(o, 1, 0);
  }
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

} // namespace

lv_obj_t* t4tires_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, hex(kPageBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = mkLabel(scr, &ui_font_geist_16, kInkMute, "TIRES + BRAKES");
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  for (int i = 0; i < 4; i++) {
    const CornerCh& c = kC[i];
    const int tx = kTileX4[i], bx = kBrkX4[i], y = kRow4[i];

    // ---- corner tile ------------------------------------------------------------
    lv_obj_t* tile = mkBox(scr, kTileBg, kBorder, 4);
    lv_obj_set_size(tile, kTileW, kRowH);
    lv_obj_set_pos(tile, tx, y);
    s_tile[i] = tile;

    // Wear bar on the OUTBOARD edge; value column fills the rest.
    const int wearX = c.barLeft ? 4 : kTileW - kWearW - 4;
    const int colX  = c.barLeft ? 4 + kWearW + 6 : 6;
    const int colW  = kTileW - kWearW - 16;   // 130

    lv_obj_t* wb = mkBox(tile, kBarBg, kBorder, 3);
    lv_obj_set_size(wb, kWearW, kRowH - 8);
    lv_obj_set_pos(wb, wearX, 4);
    lv_obj_set_style_clip_corner(wb, true, 0);
    s_wearBar[i] = wb;
    lv_obj_t* wf = mkBox(wb, 0x5a5d63, 0, 0);   // neutral fill (worn tints amber/red)
    lv_obj_set_size(wf, kWearW - 2, 0);
    lv_obj_align(wf, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_wearFill[i] = wf;
    s_wearTxt[i] = mkLabel(wb, &geist_sans_10, kInkMiss, "W\nE\nA\nR");
    lv_obj_set_style_text_align(s_wearTxt[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_wearTxt[i], 1, 0);
    lv_obj_align(s_wearTxt[i], LV_ALIGN_CENTER, 0, -14);
    s_wearPct[i] = mkLabel(wb, &geist_sans_10, kInk, "--");
    lv_obj_align(s_wearPct[i], LV_ALIGN_BOTTOM_MID, 0, -3);

    // Tread-temp band cells: three I/M/O cells AND one collapsed cell, built
    // once; tick shows whichever matches what the sim publishes.
    const int cellW = (colW - 2 * 3) / 3;   // 3px gaps
    static const char* kLtr[3] = { "I", "M", "O" };
    for (int b = 0; b < 3; b++) {
      lv_obj_t* cell = mkBox(tile, kTileBg, 0, 3);
      lv_obj_set_size(cell, cellW, kBandH);
      lv_obj_set_pos(cell, colX + b * (cellW + 3), kBandY);
      s_bandCell[i][b] = cell;
      s_bandLtr[i][b] = mkLabel(cell, &geist_sans_10, 0xffffff, kLtr[b]);
      lv_obj_set_style_opa(s_bandLtr[i][b], (lv_opa_t)178, 0);   // 0.7
      lv_obj_align(s_bandLtr[i][b], LV_ALIGN_TOP_MID, 0, 6);
      s_bandVal[i][b] = mkLabel(cell, &geist_sans_16, 0xffffff, "--");
      lv_obj_align(s_bandVal[i][b], LV_ALIGN_CENTER, 0, 4);
    }
    lv_obj_t* single = mkBox(tile, kTileBg, 0, 3);
    lv_obj_set_size(single, colW, kBandH);
    lv_obj_set_pos(single, colX, kBandY);
    s_single[i] = single;
    s_singleVal[i] = mkLabel(single, &geist_sans_36, 0xffffff, "--");
    lv_obj_align(s_singleVal[i], LV_ALIGN_CENTER, 0, 0);
    // Default to the collapsed cell until bands arrive.
    for (int b = 0; b < 3; b++) lv_obj_add_flag(s_bandCell[i][b], LV_OBJ_FLAG_HIDDEN);

    // Pressure (psi, one decimal) centered under the bands.
    s_press[i] = mkLabel(tile, &geist_sans_24, kInkMiss, "-- psi");
    lv_obj_set_width(s_press[i], colW);
    lv_obj_set_style_text_align(s_press[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_press[i], colX, kPressY + 24);

    // FLAT badge — top INNER corner (opposite the wear bar), hidden until told.
    lv_obj_t* fb = mkBox(tile, kRed, 0, 3);
    lv_obj_set_size(fb, 40, 16);
    lv_obj_align(fb, c.barLeft ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                 c.barLeft ? 1 : -1, -1);
    lv_obj_t* fbTxt = mkLabel(fb, &geist_sans_10, kBadgeInk, "FLAT");
    lv_obj_center(fbTxt);
    lv_obj_add_flag(fb, LV_OBJ_FLAG_HIDDEN);
    s_flatBadge[i] = fb;

    // DETACHED overlay — red X + word, hidden until told.
    s_detX[i] = mkLabel(tile, &lv_font_montserrat_24, kRed, LV_SYMBOL_CLOSE);
    lv_obj_align(s_detX[i], LV_ALIGN_CENTER, 0, -24);
    lv_obj_add_flag(s_detX[i], LV_OBJ_FLAG_HIDDEN);
    s_detTxt[i] = mkLabel(tile, &geist_sans_10, kRed, "DETACHED");
    lv_obj_set_style_text_letter_space(s_detTxt[i], 2, 0);
    lv_obj_align(s_detTxt[i], LV_ALIGN_CENTER, 0, 4);
    lv_obj_add_flag(s_detTxt[i], LV_OBJ_FLAG_HIDDEN);

    // ---- brake bar ---------------------------------------------------------------
    lv_obj_t* bb = mkBox(scr, kBarBg, kBorder, 4);
    lv_obj_set_size(bb, kBrkW, kRowH);
    lv_obj_set_pos(bb, bx, y);
    lv_obj_set_style_clip_corner(bb, true, 0);
    s_brkBase[i] = bb;
    lv_obj_t* bf = mkBox(bb, kTileBg, 0, 0);
    lv_obj_set_size(bf, kBrkW - 2, 0);
    lv_obj_align(bf, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_brkFill[i] = bf;
    s_brkVal[i] = mkLabel(bb, &geist_sans_16, kInkMiss, "--");
    lv_obj_align(s_brkVal[i], LV_ALIGN_CENTER, 0, -10);
    s_brkUnit[i] = mkLabel(bb, &geist_sans_10, kInkMiss, "\xC2\xB0""C");
    lv_obj_set_style_opa(s_brkUnit[i], (lv_opa_t)204, 0);   // 0.8
    lv_obj_align(s_brkUnit[i], LV_ALIGN_CENTER, 0, 8);
    s_brkX[i] = mkLabel(bb, &lv_font_montserrat_14, kRed, LV_SYMBOL_CLOSE);
    lv_obj_center(s_brkX[i]);
    lv_obj_add_flag(s_brkX[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Single shared "BRK" label at the geometric center of the 2x2 brake cluster.
  lv_obj_t* brk = mkBox(scr, kBarBg, 0, 3);
  lv_obj_set_size(brk, 44, 18);
  lv_obj_set_pos(brk, kW / 2 - 22, (kRowY[0] + kRowY[1] + kRowH) / 2 - 9);
  lv_obj_t* brkTxt = mkLabel(brk, &geist_sans_10, kInkDim, "BRK");
  lv_obj_set_style_text_letter_space(brkTxt, 2, 0);
  lv_obj_center(brkTxt);

  // Init caches to "unset" so the first tick paints everything.
  for (int i = 0; i < 4; i++) {
    for (int b = 0; b < 3; b++) s_shBand[i][b] = INT32_MIN;
    s_shSingle[i] = s_shPress[i] = s_shWear[i] = s_shBrake[i] = INT32_MIN;
    s_shBands[i] = s_shFlat[i] = s_shDet[i] = -1;
  }
  return scr;
}

void t4tires_tick(bool waiting) {
  const uint32_t now = millis();
  if (now - s_tickMs < kTickMs) return;
  s_tickMs = now;

  for (int i = 0; i < 4; i++) {
    const CornerCh& c = kC[i];
    const int  tM   = waiting ? 0 : (int)tcStore.getInt(c.t);
    const int  tIn  = waiting ? 0 : (int)tcStore.getInt(c.tin);
    const int  tOut = waiting ? 0 : (int)tcStore.getInt(c.tout);
    const int  pKpa = waiting ? 0 : (int)tcStore.getInt(c.press);
    const int  wear = waiting ? 0 : (int)tcStore.getInt(c.wear);   // % remaining
    const int  brk  = waiting ? 0 : (int)tcStore.getInt(c.brake);
    const bool flat = !waiting && tcStore.getBool(c.flat);
    const bool det  = !waiting && tcStore.getBool(c.det);

    // ---- detached: blank the tile + brake bar (absent beats wrong) --------------
    if ((int8_t)det != s_shDet[i]) {
      s_shDet[i] = (int8_t)det;
      lv_obj_set_style_bg_opa(s_tile[i], det ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(s_tile[i], hex(det ? kRed : kBorder), 0);
      for (int b = 0; b < 3; b++)
        (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_bandCell[i][b], LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_single[i],  LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_press[i],   LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_wearBar[i], LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_remove_flag : lv_obj_add_flag)(s_detX[i],   LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_remove_flag : lv_obj_add_flag)(s_detTxt[i], LV_OBJ_FLAG_HIDDEN);
      // Brake column mirrors: blank base + red border + X only.
      lv_obj_set_style_bg_opa(s_brkBase[i], det ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(s_brkBase[i], hex(det ? kRed : kBorder), 0);
      (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_brkFill[i], LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_brkVal[i],  LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_add_flag : lv_obj_remove_flag)(s_brkUnit[i], LV_OBJ_FLAG_HIDDEN);
      (det ? lv_obj_remove_flag : lv_obj_add_flag)(s_brkX[i],    LV_OBJ_FLAG_HIDDEN);
      if (det) { s_shBands[i] = -1; s_shFlat[i] = -1; }   // repaint on re-attach
    }
    if (det) continue;   // nothing else renders on a detached corner

    // ---- flat: annotate, never blank --------------------------------------------
    if ((int8_t)flat != s_shFlat[i]) {
      s_shFlat[i] = (int8_t)flat;
      (flat ? lv_obj_remove_flag : lv_obj_add_flag)(s_flatBadge[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_border_color(s_tile[i], hex(flat ? kRed : kBorder), 0);
    }

    // ---- band mode: 3 cells when the sim publishes inner/outer, else 1 ----------
    const int8_t hasBands = (tIn > 0 || tOut > 0) ? 1 : 0;
    if (hasBands != s_shBands[i]) {
      s_shBands[i] = hasBands;
      for (int b = 0; b < 3; b++)
        (hasBands ? lv_obj_remove_flag : lv_obj_add_flag)(s_bandCell[i][b], LV_OBJ_FLAG_HIDDEN);
      (hasBands ? lv_obj_add_flag : lv_obj_remove_flag)(s_single[i], LV_OBJ_FLAG_HIDDEN);
      // Force value repaint in the newly-visible widgets.
      for (int b = 0; b < 3; b++) s_shBand[i][b] = INT32_MIN;
      s_shSingle[i] = INT32_MIN;
    }
    char buf[16];
    if (hasBands) {
      const int bv[3] = { tIn, tM, tOut };   // fixed I-M-O order, never mirrored
      for (int b = 0; b < 3; b++) {
        if (bv[b] == s_shBand[i][b]) continue;
        s_shBand[i][b] = bv[b];
        const Bucket bk = tireBucket(bv[b]);
        lv_obj_set_style_bg_color(s_bandCell[i][b], hex(bk.bg), 0);
        if (bv[b] > 0) snprintf(buf, sizeof(buf), "%d\xC2\xB0", bv[b]);
        else           snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(s_bandVal[i][b], buf);
        lv_obj_set_style_text_color(s_bandVal[i][b], hex(bv[b] > 0 ? 0xffffff : kInkMiss), 0);
      }
    } else if (tM != s_shSingle[i]) {
      s_shSingle[i] = tM;
      const Bucket bk = tireBucket(tM);
      lv_obj_set_style_bg_color(s_single[i], hex(bk.bg), 0);
      if (tM > 0) snprintf(buf, sizeof(buf), "%d\xC2\xB0", tM);
      else        snprintf(buf, sizeof(buf), "--");
      lv_label_set_text(s_singleVal[i], buf);
      lv_obj_set_style_text_color(s_singleVal[i], hex(tM > 0 ? 0xffffff : kInkMiss), 0);
    }

    // ---- pressure (psi, one decimal; iRacing publishes none live) ---------------
    if (pKpa != s_shPress[i]) {
      s_shPress[i] = pKpa;
      if (pKpa > 0) snprintf(buf, sizeof(buf), "%.1f psi", pKpa * kKpaToPsi);
      else          snprintf(buf, sizeof(buf), "-- psi");
      lv_label_set_text(s_press[i], buf);
      lv_obj_set_style_text_color(s_press[i], hex(pKpa > 0 ? 0xffffff : kInkMiss), 0);
    }

    // ---- wear (wire = % remaining; ACC/EVO send a constant 100 — shown as-is) ---
    if (wear != s_shWear[i]) {
      s_shWear[i] = wear;
      const int rem  = wear < 0 ? 0 : wear > 100 ? 100 : wear;
      const int worn = 100 - rem;
      if (wear > 0) {
        lv_obj_set_height(s_wearFill[i], (kRowH - 10) * rem / 100);
        lv_obj_align(s_wearFill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(s_wearFill[i],
            hex(worn > 90 ? kRed : worn > 75 ? kAmber : 0x5a5d63), 0);
        snprintf(buf, sizeof(buf), "%d", worn);
        lv_label_set_text(s_wearPct[i], buf);
        lv_obj_set_style_text_color(s_wearTxt[i], hex(kInkDim), 0);
        lv_obj_set_style_text_color(s_wearPct[i], hex(kInk), 0);
      } else {
        lv_obj_set_height(s_wearFill[i], 0);
        lv_label_set_text(s_wearPct[i], "--");
        lv_obj_set_style_text_color(s_wearTxt[i], hex(kInkMiss), 0);
        lv_obj_set_style_text_color(s_wearPct[i], hex(kInkMiss), 0);
      }
    }

    // ---- brake temp (iRacing publishes none live) -------------------------------
    if (brk != s_shBrake[i]) {
      s_shBrake[i] = brk;
      const Bucket bk = brakeBucket(brk);
      if (brk > 0) {
        const int fill = brk > kBrakeFullScaleC ? kBrakeFullScaleC : brk;
        lv_obj_set_height(s_brkFill[i], (kRowH - 2) * fill / kBrakeFullScaleC);
        lv_obj_align(s_brkFill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(s_brkFill[i], hex(bk.bg), 0);
        snprintf(buf, sizeof(buf), "%d", brk);
        lv_label_set_text(s_brkVal[i], buf);
        lv_obj_set_style_text_color(s_brkVal[i], hex(bk.ink), 0);
        lv_obj_set_style_text_color(s_brkUnit[i], hex(bk.ink), 0);
      } else {
        lv_obj_set_height(s_brkFill[i], 0);
        lv_label_set_text(s_brkVal[i], "--");
        lv_obj_set_style_text_color(s_brkVal[i], hex(kInkMiss), 0);
        lv_obj_set_style_text_color(s_brkUnit[i], hex(kInkMiss), 0);
      }
    }
  }
}
