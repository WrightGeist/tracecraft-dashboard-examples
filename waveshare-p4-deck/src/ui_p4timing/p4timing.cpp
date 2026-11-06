// TraceCraft P4 timing dash — 1280x720 landscape, Waveshare ESP32-P4 5" DSI panel.
//
// PURE lap timing (no shift lights, no gear — the race dash page
// carries those). Same style guide as the race dash (the recorder's
// Diagnostic dashboard vocabulary — see p4dash.cpp):
//
//   top band        CURRENT lap clock (left) + live DELTA to session best
//                   (right, green/red, with a trend arrow: gaining/losing)
//   TIMING (left)   LAST lap + its delta vs the best it raced against,
//                   BEST lap (purple, PB flash), PREDICTED lap
//   SECTORS (right) one row per sector: palette chip, session-BEST split,
//                   live/last split with delta vs best-before
//   bottom          LAP counter (amber flash each S/F), sector disc, flag slot
//
// WHERE THE NUMBERS COME FROM — the HOST, not the device. The recorder streams
// every timing channel preformatted: lap_time_str / last_lap_str / best_lap_str,
// lap_delta(_str) + lap_delta_trend (sim-native where published, host-derived
// elsewhere), predicted_lap_str, sector_time_str (running current-sector clock),
// last_sector_str (the just-closed split) and best_s1..3_str (per-sector session
// bests). The old on-device sector clock — parse the lap clock, timestamp sector
// transitions, keep bests — is retired; the only arithmetic left on-device is
// string-parse subtraction for the two "delta vs best-BEFORE" readouts (a PB must
// not read +0.000, so the device caches the pre-refresh best and diffs against
// that). Absent channels render as blanks/dashes — never invented numbers.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "p4timing.h"
#include "tc_generated.h"

// Shared subsets.
LV_FONT_DECLARE(geist_mono_22);       // captions + absent-value hints (ASCII)
LV_FONT_DECLARE(geist_mono_time_36);  // row deltas (0-9 : . + -)
LV_FONT_DECLARE(geist_sans_arrows_48); // gaining/losing trend arrow (U+2191/U+2193)
// P4 (1280x720) hero fonts.
LV_FONT_DECLARE(geist_mono_96);       // lap number       (0-9 -)
LV_FONT_DECLARE(geist_mono_time_72);  // current lap + big delta (0-9 : . + -)
LV_FONT_DECLARE(geist_mono_time_48);  // last/best/pred/sector times

// Board-selected fonts — the tick swaps these in/out dynamically, so they must
// be pointers set by the setup, not hardcoded per call site.
const lv_font_t* g_fCur  = nullptr;   // current-lap clock + big delta value
const lv_font_t* g_fVal  = nullptr;   // last / best / pred values
const lv_font_t* g_fLap  = nullptr;   // bottom lap counter
const lv_font_t* g_fHint = &geist_mono_22;   // absent-value hint

// DELTA-value right-edge anchor — the tick re-applies it when the value font
// swaps in, so it must match the top-band position.
int g_deltaOffX = 0, g_deltaOffY = 0;   // set by the setup

namespace {

// ---- palette — the Diagnostic dashboard's colors (see p4dash.cpp) ----------
constexpr uint32_t kPageBg  = 0x0c0d10;
constexpr uint32_t kPanelBg = 0x16181c;
constexpr uint32_t kBorder  = 0x2a2d35;
constexpr uint32_t kBarBg   = 0x0a0b0d;
constexpr uint32_t kInk     = 0xf1f2f5;
constexpr uint32_t kInkMute = 0xdde0e6;
constexpr uint32_t kInkDim  = 0xb8bbc1;
constexpr uint32_t kInkMiss = 0x80848c;
constexpr uint32_t kGreen   = 0x4ade80;
constexpr uint32_t kAmber   = 0xfacc15;
constexpr uint32_t kRed     = 0xf87171;
constexpr uint32_t kPurple  = 0xa78bfa;   // best-lap / personal-best accent
constexpr uint32_t kDark    = 0x0a0a0b;
constexpr uint32_t kSectorColors[4] = { 0xd97c7c, 0x7aa3c8, 0xd4a05a, 0x7fb393 };

// ---- geometry (1280x720) ---------------------------------------------------------
constexpr int kW = 1280, kH = 720;
constexpr int kMargin = 28;
constexpr int kTopY = 30, kTopH = 150;        // CURRENT + DELTA band
constexpr int kMidY = 200, kMidH = 340;       // TIMING + SECTORS panels
constexpr int kBotY = 566, kBotH = 126;
constexpr int kMaxSectors = 3;                // rows rendered; >3-sector tracks show the first three

// ---- timing ----------------------------------------------------------------------
constexpr uint32_t kTextMs      = 50;         // fast labels update at most 20 Hz
constexpr uint32_t kLapFlashMs  = 1400;
constexpr uint32_t kSecFlashMs  = 550;
constexpr uint32_t kSplitFlashMs = 1600;      // just-closed split holds bright, then dims
constexpr uint32_t kPbFlashMs   = 1200;       // purple pill on a new personal best
constexpr float    kTrendDeadband = 0.05f;    // |s/s| below this: no arrow (absent reads 0)

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

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

lv_obj_t* makeCaption(lv_obj_t* parent, uint32_t color, const char* txt) {
  lv_obj_t* l = makeLabel(parent, &geist_mono_22, color, txt);
  lv_obj_set_style_text_letter_space(l, 3, 0);
  return l;
}

lv_obj_t* makePanel(lv_obj_t* parent, int x, int y, int w, int h, const char* title) {
  lv_obj_t* p = makeBox(parent, w, h, kPanelBg, 4);
  lv_obj_set_pos(p, x, y);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_color(p, hex(kBorder), 0);
  lv_obj_t* t = makeCaption(p, kInkMute, title);
  lv_obj_set_pos(t, 16, 12);
  return p;
}

// ---- time-string parsing / formatting ----------------------------------------------
// Lap times arrive "M:SS.mmm"; sector splits "SS.mmm" (or lap format for 60s+
// splits). Returns milliseconds, or -1 for missing/garbage.
int32_t parseLapMs(const char* s) {
  if (!s || !s[0] || s[0] == '-') return -1;
  const char* colon = strchr(s, ':');
  if (!colon) return -1;
  const long mins = strtol(s, nullptr, 10);
  const double secs = strtod(colon + 1, nullptr);
  if (mins < 0 || secs < 0) return -1;
  return (int32_t)(mins * 60000 + secs * 1000.0 + 0.5);
}

int32_t parseSectorMs(const char* s) {
  if (!s || !s[0] || s[0] == '-') return -1;
  if (strchr(s, ':')) return parseLapMs(s);
  const double secs = strtod(s, nullptr);
  if (secs <= 0) return -1;
  return (int32_t)(secs * 1000.0 + 0.5);
}

// Signed delta. Millisecond precision under 10s ("+0.231"); tenths above
// ("+12.3") so the string never outgrows its column.
void fmtDelta(char* out, size_t n, int32_t deltaMs) {
  if (deltaMs > 999900) deltaMs = 999900;
  if (deltaMs < -999900) deltaMs = -999900;
  if (deltaMs > -10000 && deltaMs < 10000) snprintf(out, n, "%+.3f", deltaMs / 1000.0);
  else snprintf(out, n, "%+.1f", deltaMs / 1000.0);
}

// ---- widgets --------------------------------------------------------------------------
lv_obj_t* s_curLap    = nullptr;   // running lap clock (top-left, huge)
lv_obj_t* s_deltaBig  = nullptr;   // live delta to best (top-right, huge, colored)
lv_obj_t* s_trendArrow = nullptr;  // gaining/losing arrow beside the big delta
lv_obj_t* s_lastVal   = nullptr;
lv_obj_t* s_lastDelta = nullptr;   // last lap vs the best it raced against
lv_obj_t* s_bestVal   = nullptr;
lv_obj_t* s_predVal   = nullptr;   // predicted lap
lv_obj_t* s_secBestLbl[kMaxSectors];   // session-best split column
lv_obj_t* s_secLive[kMaxSectors];      // running clock / just-closed split column
lv_obj_t* s_secDelta[kMaxSectors];     // split delta vs best-before
lv_obj_t* s_secChip[kMaxSectors];
lv_obj_t* s_lapVal   = nullptr;
lv_obj_t* s_secDisc  = nullptr;
lv_obj_t* s_secNum   = nullptr;
lv_obj_t* s_flagSlot = nullptr;
lv_obj_t* s_flagText = nullptr;

// ---- displayed-string caches (change detection — every value is host-fed) -----------
char     s_curPrev[16]   = "";
uint32_t s_curTxtMs      = 0;
char     s_deltaPrev[16] = "";
int      s_trendState    = 0;      // -1 gaining / 0 hidden / +1 losing
char     s_lastPrev[16]  = "";
char     s_bestPrev[16]  = "";
char     s_predPrev[16]  = "";
char     s_secBestPrev[kMaxSectors][16];
char     s_lastSplitPrev[16] = "";
uint32_t s_pbFlashAt     = 0;
int32_t  s_bestBeforeMs  = -1;     // best lap the CURRENT lap is racing against
int32_t  s_secBestBeforeMs[kMaxSectors];   // per-sector best-before (delta reference)

// Which row the NEXT last_sector_str belongs to: the sector we just left.
int      s_awaitSplitRow = -1;
uint32_t s_splitFlashAt[kMaxSectors];

// ---- misc change-detection ---------------------------------------------------------------------
int      s_sectorIdx   = -1;
int32_t  s_lapPrev     = INT32_MIN;
uint32_t s_lapFlashAt  = 0;
int      s_discIdx     = -1;
uint32_t s_discFlashAt = 0;
int      s_secRowShown = -1;     // active-row highlight cache
int      s_flagPhasePrev = -1;

void setDeltaLabel(lv_obj_t* l, int32_t deltaMs, bool valid) {
  if (!valid) { lv_label_set_text(l, ""); return; }
  char b[12];
  fmtDelta(b, sizeof(b), deltaMs);
  lv_label_set_text(l, b);
  lv_obj_set_style_text_color(l, hex(deltaMs <= 0 ? kGreen : kRed), 0);
}

} // namespace

lv_obj_t* p4timing_setup() {
  g_fCur = &geist_mono_time_72;
  g_fVal = &geist_mono_time_48;
  g_fLap = &geist_mono_96;
  g_deltaOffX = kMargin + 16; g_deltaOffY = kTopY + 34;
  lv_obj_t* scr = lv_obj_create(NULL);   // own screen — the caller loads/pages it
  lv_obj_set_style_bg_color(scr, hex(kPageBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // --- top band: CURRENT (left) + live DELTA with trend arrow (right) --------------
  lv_obj_t* curCap = makeCaption(scr, kInkDim, "CURRENT");
  lv_obj_set_pos(curCap, kMargin + 16, kTopY);
  s_curLap = makeLabel(scr, &geist_mono_time_72, kInk, "-:--.---");
  lv_obj_set_pos(s_curLap, kMargin + 16, kTopY + 34);

  lv_obj_t* dltCap = makeCaption(scr, kInkDim, "DELTA");
  lv_obj_align(dltCap, LV_ALIGN_TOP_RIGHT, -(kMargin + 16), kTopY);
  // Content-sized + right-edge-pinned (persistent align) — a fixed-width label
  // wraps the moment the text outgrows it by a pixel.
  // Absent-value slots carry a HINT (what earns the value) instead of a
  // dead dash — the driver learns the fix, not just the absence. Hints
  // render in the small ASCII caption font, nudged down to sit near the
  // slot's optical middle; the value font/position is restored the
  // moment a real value arrives.
  s_deltaBig = makeLabel(scr, &geist_mono_22, kInkMiss, "finish a clean lap first");
  lv_obj_set_style_translate_y(s_deltaBig, 24, 0);
  lv_obj_align(s_deltaBig, LV_ALIGN_TOP_RIGHT, -(kMargin + 16), kTopY + 34);
  // Arrow sits LEFT of the delta value. The value is right-pinned and content-
  // sized, so anchor the arrow to a fixed x that clears the widest delta string
  // ("+12.3" in time_72 ≈ 6 glyphs ≈ 260px) — overlap-free without re-layout.
  s_trendArrow = makeLabel(scr, &geist_sans_arrows_48, kInkMiss, "");
  lv_obj_align(s_trendArrow, LV_ALIGN_TOP_RIGHT, -(kMargin + 16 + 300), kTopY + 46);

  // --- TIMING panel (left): LAST / BEST / PRED --------------------------------------
  lv_obj_t* timing = makePanel(scr, kMargin, kMidY, 560, kMidH, "TIMING");
  const int tRowY0 = 56, tRowH = 94;
  const char* tCaps[3] = { "LAST", "BEST", "PRED" };
  for (int r = 0; r < 3; r++) {
    lv_obj_t* cap = makeCaption(timing, kInkDim, tCaps[r]);
    lv_obj_set_pos(cap, 16, tRowY0 + r * tRowH);
  }
  s_lastVal = makeLabel(timing, &geist_mono_22, kInkMiss, "finish a lap");
  lv_obj_set_style_translate_y(s_lastVal, 14, 0);
  lv_obj_set_pos(s_lastVal, 16, tRowY0 + 28);
  s_lastDelta = makeLabel(timing, &geist_mono_time_36, kInkDim, "");
  lv_obj_align(s_lastDelta, LV_ALIGN_TOP_RIGHT, -16, tRowY0 + 36);

  s_bestVal = makeLabel(timing, &geist_mono_22, kInkMiss, "finish a clean lap");
  lv_obj_set_style_translate_y(s_bestVal, 14, 0);
  lv_obj_set_style_radius(s_bestVal, 8, 0);          // purple pill on a new PB
  lv_obj_set_style_bg_opa(s_bestVal, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_hor(s_bestVal, 6, 0);
  lv_obj_set_pos(s_bestVal, 10, tRowY0 + tRowH + 28);

  s_predVal = makeLabel(timing, &geist_mono_22, kInkMiss, "comes with your best lap");
  lv_obj_set_style_translate_y(s_predVal, 14, 0);
  lv_obj_set_pos(s_predVal, 16, tRowY0 + 2 * tRowH + 28);

  // --- SECTORS panel (right): chip | session best | live/last | delta ----------------
  lv_obj_t* sectors = makePanel(scr, 620, kMidY, kW - kMargin - 620, kMidH, "SECTORS");
  lv_obj_t* bestCap = makeCaption(sectors, kInkMiss, "BEST");
  lv_obj_set_pos(bestCap, 84, 34);
  lv_obj_t* liveCap = makeCaption(sectors, kInkMiss, "LIVE / LAST");
  lv_obj_set_pos(liveCap, 300, 34);
  const int rowY0 = 68, rowH = 90;
  for (int i = 0; i < kMaxSectors; i++) {
    s_secChip[i] = makeBox(sectors, 48, 48, kSectorColors[i % 4], 12);
    lv_obj_set_pos(s_secChip[i], 16, rowY0 + i * rowH + 8);
    char n[4];
    snprintf(n, sizeof(n), "%d", i + 1);
    lv_obj_t* num = makeCaption(s_secChip[i], kDark, n);
    lv_obj_center(num);
    // Columns — sized for the WORST case, not the pretty one: a minute-plus
    // sector ("1:41.286", 8 glyphs) is normal on long tracks, and 8 glyphs of
    // 48pt mono (~230px) collides with any neighbor this panel can afford.
    // So both value columns use the 36pt time font (~22px/glyph → 8 glyphs ≈
    // 174px): BEST x84..~258, LIVE/LAST x300..~474, delta right-pinned from
    // ~486 (fmtDelta caps at 6 glyphs ≈ 130px) — three clean gaps at every
    // possible width. (First cut used 48pt and overlapped on real data.)
    s_secBestLbl[i] = makeLabel(sectors, &geist_mono_time_36, kInkMiss, "--.---");
    lv_obj_set_pos(s_secBestLbl[i], 84, rowY0 + i * rowH + 12);
    s_secLive[i] = makeLabel(sectors, &geist_mono_time_36, kInkMiss, "--.---");
    lv_obj_set_pos(s_secLive[i], 300, rowY0 + i * rowH + 12);
    s_secDelta[i] = makeLabel(sectors, &geist_mono_time_36, kInkDim, "");
    lv_obj_align(s_secDelta[i], LV_ALIGN_TOP_RIGHT, -16, rowY0 + i * rowH + 12);
    s_secBestPrev[i][0] = 0;
    s_secBestBeforeMs[i] = -1;
    s_splitFlashAt[i] = 0;
  }

  // --- bottom band: LAP + sector disc + flag slot ---------------------------------------
  lv_obj_t* lapCap = makeCaption(scr, kInkDim, "LAP");
  lv_obj_set_pos(lapCap, kMargin + 8, kBotY + 4);
  s_lapVal = makeLabel(scr, &geist_mono_96, kInk, "-");
  lv_obj_set_style_radius(s_lapVal, 8, 0);
  lv_obj_set_style_bg_opa(s_lapVal, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_hor(s_lapVal, 10, 0);
  lv_obj_set_pos(s_lapVal, kMargin, kBotY + 26);

  s_secDisc = makeBox(scr, 110, 110, 0x1c1c1e, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_secDisc, 320, kBotY + (kBotH - 110) / 2);
  s_secNum = makeLabel(s_secDisc, &geist_mono_96, kInkMiss, "-");
  lv_obj_center(s_secNum);

  s_flagSlot = makeBox(scr, 280, 80, kBarBg, 8);
  lv_obj_set_pos(s_flagSlot, kW - kMargin - 280, kBotY + (kBotH - 80) / 2);
  lv_obj_set_style_border_width(s_flagSlot, 1, 0);
  lv_obj_set_style_border_color(s_flagSlot, hex(kBorder), 0);
  s_flagText = makeCaption(s_flagSlot, kInkMiss, "FLAG");
  lv_obj_center(s_flagText);

  return scr;
}

void p4timing_tick(bool waitingForData) {
  const uint32_t nowMs = millis();


  // --- CURRENT lap clock -----------------------------------------------------------
  const char* curStr = tcStore.getStr(TC_CH_LAP_TIME_STR);
  if (curStr && curStr[0] && strncmp(curStr, s_curPrev, sizeof(s_curPrev) - 1) != 0
      && nowMs - s_curTxtMs >= kTextMs) {
    strncpy(s_curPrev, curStr, sizeof(s_curPrev) - 1);
    s_curTxtMs = nowMs;
    lv_label_set_text(s_curLap, curStr);
    lv_obj_set_style_text_color(s_curLap, hex(kInk), 0);
  }

  // --- live DELTA + trend arrow ------------------------------------------------------
  // Host-fed: lap_delta_str is sim-native where the sim publishes a trustworthy
  // delta (iRacing/ACC/EVO/LMU) and absent otherwise — absent shows the dim
  // placeholder, never an invented number.
  const char* deltaStr = tcStore.getStr(TC_CH_LAP_DELTA_STR);
  if (strncmp(deltaStr ? deltaStr : "", s_deltaPrev, sizeof(s_deltaPrev) - 1) != 0) {
    strncpy(s_deltaPrev, deltaStr ? deltaStr : "", sizeof(s_deltaPrev) - 1);
    if (deltaStr && deltaStr[0]) {
      lv_obj_set_style_text_font(s_deltaBig, g_fCur, 0);
      lv_obj_set_style_translate_y(s_deltaBig, 0, 0);
      lv_label_set_text(s_deltaBig, deltaStr);
      const float d = tcStore.getFloat(TC_CH_LAP_DELTA);
      lv_obj_set_style_text_color(s_deltaBig, hex(d <= 0 ? kGreen : kRed), 0);
    } else {
      lv_obj_set_style_text_font(s_deltaBig, g_fHint, 0);
      lv_obj_set_style_translate_y(s_deltaBig, 24, 0);
      lv_label_set_text(s_deltaBig, "finish a clean lap first");
      lv_obj_set_style_text_color(s_deltaBig, hex(kInkMiss), 0);
    }
    lv_obj_align(s_deltaBig, LV_ALIGN_TOP_RIGHT, -g_deltaOffX, g_deltaOffY);
  }
  const float trend = tcStore.getFloat(TC_CH_LAP_DELTA_TREND);
  const bool haveDelta = s_deltaPrev[0] != 0;
  const int trendState = (!haveDelta || (trend > -kTrendDeadband && trend < kTrendDeadband))
                           ? 0 : (trend > 0 ? 1 : -1);
  if (trendState != s_trendState) {
    s_trendState = trendState;
    if (trendState == 0) {
      lv_label_set_text(s_trendArrow, "");
    } else if (trendState > 0) {                 // delta rising = losing time
      lv_label_set_text(s_trendArrow, "\xE2\x86\x91");   // U+2191 up arrow (Geist)
      lv_obj_set_style_text_color(s_trendArrow, hex(kRed), 0);
    } else {                                     // delta falling = gaining
      lv_label_set_text(s_trendArrow, "\xE2\x86\x93");   // U+2193 down arrow (Geist)
      lv_obj_set_style_text_color(s_trendArrow, hex(kGreen), 0);
    }
  }

  // --- LAST / BEST / PRED --------------------------------------------------------------
  const char* lastStr = tcStore.getStr(TC_CH_LAST_LAP_STR);
  const char* bestStr = tcStore.getStr(TC_CH_BEST_LAP_STR);
  const int32_t lastMs = parseLapMs(lastStr);
  if (lastStr && lastStr[0] && strncmp(lastStr, s_lastPrev, sizeof(s_lastPrev) - 1) != 0) {
    strncpy(s_lastPrev, lastStr, sizeof(s_lastPrev) - 1);
    lv_obj_set_style_text_font(s_lastVal, g_fVal, 0);
    lv_obj_set_style_translate_y(s_lastVal, 0, 0);
    lv_label_set_text(s_lastVal, lastStr);
    lv_obj_set_style_text_color(s_lastVal, hex(lastMs > 0 ? kInk : kInkMiss), 0);
    // Delta vs the best this lap RACED AGAINST (best-before, not the refreshed
    // best — a PB would otherwise always read +0.000).
    if (lastMs > 0 && s_bestBeforeMs > 0)
      setDeltaLabel(s_lastDelta, lastMs - s_bestBeforeMs, true);
    if (lastMs > 0 && (s_bestBeforeMs <= 0 || lastMs < s_bestBeforeMs)) s_pbFlashAt = nowMs;
    s_bestBeforeMs = parseLapMs(bestStr);   // next lap races the refreshed best
  } else if (s_bestBeforeMs <= 0) {
    s_bestBeforeMs = parseLapMs(bestStr);   // first best arrives without a lap edge
  }

  if (bestStr && bestStr[0] && strncmp(bestStr, s_bestPrev, sizeof(s_bestPrev) - 1) != 0) {
    strncpy(s_bestPrev, bestStr, sizeof(s_bestPrev) - 1);
    lv_obj_set_style_text_font(s_bestVal, g_fVal, 0);
    lv_obj_set_style_translate_y(s_bestVal, 0, 0);
    lv_obj_set_style_text_color(s_bestVal, hex(kPurple), 0);
    lv_label_set_text(s_bestVal, bestStr);
  }
  if (s_pbFlashAt != 0) {                       // purple PB pill, decaying
    const uint32_t el = nowMs - s_pbFlashAt;
    if (el < kPbFlashMs) {
      const float k = (float)el / kPbFlashMs;
      lv_obj_set_style_bg_opa(s_bestVal, (lv_opa_t)(255 * (1.0f - k)), 0);
      lv_obj_set_style_bg_color(s_bestVal, hex(kPurple), 0);
      lv_obj_set_style_text_color(s_bestVal, lerpHex(kDark, kPurple, k), 0);
    } else {
      lv_obj_set_style_bg_opa(s_bestVal, LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(s_bestVal, hex(kPurple), 0);
      s_pbFlashAt = 0;
    }
  }

  const char* predStr = tcStore.getStr(TC_CH_PREDICTED_LAP_STR);
  if (strncmp(predStr ? predStr : "", s_predPrev, sizeof(s_predPrev) - 1) != 0) {
    strncpy(s_predPrev, predStr ? predStr : "", sizeof(s_predPrev) - 1);
    if (predStr && predStr[0]) {
      lv_obj_set_style_text_font(s_predVal, g_fVal, 0);
      lv_obj_set_style_translate_y(s_predVal, 0, 0);
      lv_label_set_text(s_predVal, predStr);
      lv_obj_set_style_text_color(s_predVal, hex(kInkMute), 0);
    } else {
      lv_obj_set_style_text_font(s_predVal, g_fHint, 0);
      lv_obj_set_style_translate_y(s_predVal, 14, 0);
      lv_label_set_text(s_predVal, "comes with your best lap");
      lv_obj_set_style_text_color(s_predVal, hex(kInkMiss), 0);
    }
  }

  // --- sector index (display-clamped) ---------------------------------------------------
  int sc = (int)tcStore.getInt(TC_CH_SECTOR_COUNT);
  if (sc < 1) sc = 3;
  const int   sRaw = (int)tcStore.getInt(TC_CH_SECTOR);
  const float pct  = tcStore.getFloat(TC_CH_LAP_DIST_PCT);
  // Sector 0 is a real sector: ask the store whether the sim sent one (an older
  // recorder can't say, so there a 0 still falls back to thirds).
  const bool hasSector = tcStore.presenceKnown() ? tcStore.has(TC_CH_SECTOR) : sRaw > 0;
  int idx = hasSector ? sRaw : (int)(pct * sc);
  if (idx < 0) idx = 0;
  if (idx >= sc) idx = sc - 1;

  if (!waitingForData && idx != s_sectorIdx) {
    // The sector we just LEFT owns the next last_sector_str the host emits.
    if (s_sectorIdx >= 0 && s_sectorIdx < kMaxSectors) s_awaitSplitRow = s_sectorIdx;
    s_sectorIdx = idx;
  }

  // --- SECTORS panel: session bests + just-closed split + running clock -----------------
  // Cache each row's best BEFORE applying the host's refreshed value, so the
  // split delta compares against the best the sector was driven AGAINST.
  const int bestCh[kMaxSectors] = { TC_CH_BEST_S1_STR, TC_CH_BEST_S2_STR, TC_CH_BEST_S3_STR };
  for (int i = 0; i < kMaxSectors; i++) {
    const char* b = tcStore.getStr(bestCh[i]);
    if (strncmp(b ? b : "", s_secBestPrev[i], sizeof(s_secBestPrev[i]) - 1) != 0) {
      s_secBestBeforeMs[i] = parseSectorMs(s_secBestPrev[i]);
      strncpy(s_secBestPrev[i], b ? b : "", sizeof(s_secBestPrev[i]) - 1);
      if (b && b[0]) {
        lv_label_set_text(s_secBestLbl[i], b);
        lv_obj_set_style_text_color(s_secBestLbl[i], hex(kInkDim), 0);
      }
    }
  }

  const char* lastSplit = tcStore.getStr(TC_CH_LAST_SECTOR_STR);
  if (lastSplit && lastSplit[0]
      && strncmp(lastSplit, s_lastSplitPrev, sizeof(s_lastSplitPrev) - 1) != 0) {
    strncpy(s_lastSplitPrev, lastSplit, sizeof(s_lastSplitPrev) - 1);
    const int r = s_awaitSplitRow;
    if (r >= 0 && r < kMaxSectors) {
      lv_label_set_text(s_secLive[r], lastSplit);
      lv_obj_set_style_text_color(s_secLive[r], hex(kInkMute), 0);
      // Delta vs that row's best-before (falls back to the current best when
      // no refresh happened — i.e. the split didn't improve the best).
      const int32_t splitMs = parseSectorMs(lastSplit);
      int32_t refMs = s_secBestBeforeMs[r];
      if (refMs <= 0) refMs = parseSectorMs(s_secBestPrev[r]);
      const bool improved = refMs > 0 && splitMs > 0 && splitMs < refMs;
      setDeltaLabel(s_secDelta[r], splitMs - refMs, refMs > 0 && splitMs > 0);
      s_splitFlashAt[r] = improved ? nowMs : 0;
      s_awaitSplitRow = -1;
    }
  }
  for (int i = 0; i < kMaxSectors; i++) {          // purple decay on an improved split
    if (s_splitFlashAt[i] == 0) continue;
    const uint32_t el = nowMs - s_splitFlashAt[i];
    if (el < kSplitFlashMs) {
      lv_obj_set_style_text_color(s_secLive[i], lerpHex(kPurple, kInkMute, (float)el / kSplitFlashMs), 0);
    } else {
      lv_obj_set_style_text_color(s_secLive[i], hex(kInkMute), 0);
      s_splitFlashAt[i] = 0;
    }
  }

  // Active row: the HOST's running sector clock in ink; the chip of the running
  // sector brightens via a border in its own color.
  const int active = waitingForData ? -1 : (idx < kMaxSectors ? idx : -1);
  if (active >= 0) {
    const char* run = tcStore.getStr(TC_CH_SECTOR_TIME_STR);
    if (run && run[0] && nowMs - s_curTxtMs < kTextMs + 20) {   // piggyback the 20 Hz cadence
      lv_label_set_text(s_secLive[active], run);
      lv_obj_set_style_text_color(s_secLive[active], hex(kInk), 0);
    }
  }
  if (active != s_secRowShown) {
    s_secRowShown = active;
    for (int i = 0; i < kMaxSectors; i++) {
      lv_obj_set_style_border_width(s_secChip[i], i == active ? 3 : 0, 0);
      lv_obj_set_style_border_color(s_secChip[i], hex(kInk), 0);
    }
  }

  // --- lap counter (amber flash) + sector disc + flag ------------------------------------
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

  if (waitingForData) {
    if (s_discIdx != -1) {
      s_discIdx = -1;
      lv_label_set_text(s_secNum, "-");
      lv_obj_set_style_bg_color(s_secDisc, hex(0x1c1c1e), 0);
      lv_obj_set_style_text_color(s_secNum, hex(kInkMiss), 0);
    }
  } else {
    if (idx != s_discIdx) {
      if (s_discIdx >= 0) s_discFlashAt = nowMs;
      s_discIdx = idx;
      char b[8];
      snprintf(b, sizeof(b), "%d", idx + 1);
      lv_label_set_text(s_secNum, b);
      lv_obj_set_style_text_color(s_secNum, hex(kDark), 0);
    }
    const uint32_t base = kSectorColors[idx % 4];
    const uint32_t el = nowMs - s_discFlashAt;
    lv_color_t fill = (s_discFlashAt != 0 && el < kSecFlashMs)
                        ? lerpHex(0xffffff, base, (float)el / kSecFlashMs)
                        : hex(base);
    lv_obj_set_style_bg_color(s_secDisc, fill, 0);
  }

  const bool caution = tcStore.getBool(TC_CH_CAUTION);
  const int flagPhase = caution ? (int)((nowMs / 25) % 20) : -1;
  if (flagPhase != s_flagPhasePrev) {
    s_flagPhasePrev = flagPhase;
    if (caution) {
      const float k = (flagPhase < 10 ? flagPhase : 20 - flagPhase) / 10.0f;
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
