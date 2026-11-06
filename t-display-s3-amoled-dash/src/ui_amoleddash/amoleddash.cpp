// TraceCraft T-Display S3 AMOLED dashboard — 536x240 landscape, 8 screens you page
// through by tapping the touch panel:
//
//   1. TIRE TEMP — the average of the four tire surface temps, with the WHOLE screen
//      color-coded to the operating window: flashing blue when cold, solid green in
//      the optimal band (no flash — you're where you want to be), flashing yellow when
//      hot, flashing red when overheating. The four corner temps sit in their physical
//      positions (FL/FR/RL/RR) around the big average.
//   2. PIT LIMITER — a big, unmissable state. Flashing amber "LIMITER ON" when engaged
//      (you're speed-capped), a quiet dim "OFF" otherwise.
//   3. FUEL — laps-left as the hero number, a fuel-level bar colored by how much is
//      left, and liters / % / per-lap underneath. Flashes red when critically low.
//
// Pure LVGL + the telemetry store (tcStore) — board-agnostic like the P4 UIs. The
// touch-to-page navigation is polled in src/main.cpp (which owns the board:: seam) and
// calls amoleddash_advance(); this file only builds + updates the screens.
//
// Honesty: an unsent channel decodes as its type's zero, so the render gates on
// dash.waitingForData() (passed in) and shows a neutral idle state until a recorder
// streams — it never invents a temperature or a fuel figure.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "amoleddash.h"
#include "tc_generated.h"

// Big, glanceable hero numerals for the DRIVER screens (GeistMono Bold 96px — 2x the
// built-in Montserrat max). Covers digits + A-Z + . - % so it serves the tire average,
// pit ON/OFF, and fuel laps-left. The diagnostic perf screens keep the small fonts.
// See fonts/NOTICE.md.
LV_FONT_DECLARE(geist_sans_16);   // perf captions/rows (shared p4dash subset)
LV_FONT_DECLARE(geist_sans_12);   // perf Y-axis labels (shared ui_fonts subset)
LV_FONT_DECLARE(geist_hero_96);   // GeistMono Bold — hero numerals
LV_FONT_DECLARE(geist_sans_24);   // Geist SemiBold — captions / labels
LV_FONT_DECLARE(geist_sans_32);
LV_FONT_DECLARE(geist_sans_40);

namespace {

// ---- palette --------------------------------------------------------------------
constexpr uint32_t kBlack  = 0x000000;
constexpr uint32_t kWhite  = 0xffffff;
constexpr uint32_t kDim    = 0x7a828c;   // muted label / "off" ink
constexpr uint32_t kIdleBg = 0x141619;   // neutral backdrop while waiting
constexpr uint32_t kBlue   = 0x1e6bff;   // tires cold
constexpr uint32_t kGreen  = 0x1db954;   // tires optimal / fuel healthy
constexpr uint32_t kYellow = 0xd9a300;   // tires hot (dark gold — white stays legible)
constexpr uint32_t kRed    = 0xe11d2a;   // tires overheating / fuel critical
constexpr uint32_t kAmber  = 0xff8a00;   // pit limiter engaged

// ---- tire-temp operating window (average surface °C) — tunable ------------------
constexpr int kTireCold  = 80;    // below: cold      (flash blue)
constexpr int kTireOptHi = 100;   // 80..100: optimal (solid green, no flash)
constexpr int kTireHot   = 112;   // 100..112: hot    (flash yellow); above: overheat (flash red)

// ---- fuel thresholds ------------------------------------------------------------
constexpr int   kFuelLow  = 30;   // % — bar turns amber below this
constexpr int   kFuelCrit = 10;   // % — bar turns red below this
constexpr float kLapsCrit = 2.0f; // laps-left at/under which the screen alarms

constexpr uint32_t kFlashMs = 380;   // flash half-period (~1.3 Hz on/off)
constexpr uint32_t kTickMs  = 40;    // render cadence (25 Hz — smooth flash, responsive text)
// The "flash" is a tear-free brightness PULSE, not a framebuffer repaint: the screen
// keeps its steady color and we pulse panel brightness between these two levels.
constexpr uint8_t  kBriFull = 255;   // steady / bright half of the pulse
constexpr uint8_t  kBriDim  = 70;    // dim half of the pulse

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// ---- screens + widgets ----------------------------------------------------------
constexpr int kPageN = 8;   // tire / pit / fuel / perf-overview / fps / cpu / gpu / session-pit
lv_obj_t* s_pages[kPageN] = { nullptr };
int       s_page = 0;
lv_obj_t* s_strategy = nullptr;
lv_obj_t* s_strategyRows[5] = {};
char s_strategyShown[5][96] = {};


// screen 1 — tires
lv_obj_t* s_tScr = nullptr; lv_obj_t* s_tCap = nullptr; lv_obj_t* s_tBig = nullptr;
lv_obj_t* s_tZone = nullptr; lv_obj_t* s_tCorner[4] = { nullptr, nullptr, nullptr, nullptr };
// screen 2 — pit limiter
lv_obj_t* s_pScr = nullptr; lv_obj_t* s_pCap = nullptr; lv_obj_t* s_pState = nullptr;
// screen 3 — fuel
lv_obj_t* s_fScr = nullptr; lv_obj_t* s_fCap = nullptr; lv_obj_t* s_fPct = nullptr;
lv_obj_t* s_fBig = nullptr; lv_obj_t* s_fBigCap = nullptr; lv_obj_t* s_fBar = nullptr;
lv_obj_t* s_fFoot = nullptr;
// screen 4 — system performance (host CPU/GPU + cross-sim FPS): a rolling trace + live value per metric
lv_obj_t* s_gScr = nullptr; lv_obj_t* s_gCap = nullptr;
lv_obj_t* s_gName[3]  = { nullptr, nullptr, nullptr };   // FPS / CPU / GPU row labels
lv_obj_t* s_gVal[3]   = { nullptr, nullptr, nullptr };   // live value per row
lv_obj_t* s_gChart[3] = { nullptr, nullptr, nullptr };
lv_chart_series_t* s_gSer[3] = { nullptr, nullptr, nullptr };
// screens 5-7 — a dedicated full-screen trace per metric, with Y-axis numbers
lv_obj_t* s_dScr[3]   = { nullptr, nullptr, nullptr };   // FPS / CPU / GPU
lv_obj_t* s_dChart[3] = { nullptr, nullptr, nullptr };
lv_chart_series_t* s_dSer[3] = { nullptr, nullptr, nullptr };
lv_obj_t* s_dVal[3]   = { nullptr, nullptr, nullptr };

// ---- change caches (avoid redundant full-screen invalidations) ------------------
uint32_t s_tickMs = 0;
uint8_t  s_briWant = kBriFull;   // desired panel brightness this frame (see amoleddash_brightness)
bool     s_fCrit   = false;      // fuel currently critical (drives the fuel-screen pulse)
uint32_t s_tBgShown = 0xFFFFFFFF, s_pBgShown = 0xFFFFFFFF, s_fBgShown = 0xFFFFFFFF;
int  s_tAvgShown = INT32_MIN, s_tZoneShown = -1;
int  s_tCornerShown[4] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
int  s_pStateShown = -1;   // -1 unset, 0 off, 1 on, 2 waiting
char s_fBigPrev[12] = "", s_fFootPrev[40] = "", s_fPctPrev[8] = "";
int  s_fBarShown = INT32_MIN; uint32_t s_fBarColShown = 0xFFFFFFFF;
int  s_gValShown[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
uint32_t s_gValColShown[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
uint32_t s_gTraceMs = 0;
int  s_dValShown[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
uint32_t s_dValColShown[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };

lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, uint32_t col, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hex(col), 0);
  lv_label_set_text(l, t);
  return l;
}

lv_obj_t* mkScreen(uint32_t bg) {
  lv_obj_t* s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, hex(bg), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s, 0, 0);
  lv_obj_set_style_border_width(s, 0, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

// Apply a screen background only when it actually changes — a full-screen bg change
// is a full AMOLED flush, so the flash toggles (a few Hz) stay cheap.
void setBg(lv_obj_t* scr, uint32_t& cache, uint32_t color) {
  if (color == cache) return;
  cache = color;
  lv_obj_set_style_bg_color(scr, hex(color), 0);
}

void setText(lv_obj_t* l, char* cache, size_t cap, const char* txt) {
  if (strncmp(txt, cache, cap - 1) == 0) return;
  strncpy(cache, txt, cap - 1); cache[cap - 1] = '\0';
  lv_label_set_text(l, txt);
}

} // namespace

lv_obj_t* amoleddash_setup() {
  const int W = 536, H = 240;

  // ===== Screen 1 — TIRE TEMP =====================================================
  s_tScr = mkScreen(kIdleBg);
  s_tCap = mkLabel(s_tScr, &geist_sans_24, kWhite, "AVG TIRE TEMP");
  lv_obj_set_style_text_letter_space(s_tCap, 2, 0);
  lv_obj_align(s_tCap, LV_ALIGN_TOP_MID, 0, 8);
  s_tBig = mkLabel(s_tScr, &geist_hero_96, kWhite, "--");   // big glanceable average
  lv_obj_align(s_tBig, LV_ALIGN_CENTER, 0, -12);
  s_tZone = mkLabel(s_tScr, &geist_sans_40, kWhite, "WAITING");
  lv_obj_set_style_text_letter_space(s_tZone, 2, 0);
  lv_obj_align(s_tZone, LV_ALIGN_CENTER, 0, 58);
  // Corner temps in physical positions: FL FR / RL RR.
  const lv_align_t ca[4] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT,
                             LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT };
  const int cdx[4] = { 14, -14, 14, -14 }, cdy[4] = { 12, 12, -12, -12 };
  for (int i = 0; i < 4; i++) {
    s_tCorner[i] = mkLabel(s_tScr, &geist_sans_24, kWhite, "--");
    lv_obj_align(s_tCorner[i], ca[i], cdx[i], cdy[i]);
  }

  // ===== Screen 2 — PIT LIMITER ===================================================
  s_pScr = mkScreen(kBlack);
  s_pCap = mkLabel(s_pScr, &geist_sans_32, kDim, "PIT LIMITER");
  lv_obj_set_style_text_letter_space(s_pCap, 3, 0);
  lv_obj_align(s_pCap, LV_ALIGN_CENTER, 0, -58);
  s_pState = mkLabel(s_pScr, &geist_hero_96, kDim, "--");
  lv_obj_align(s_pState, LV_ALIGN_CENTER, 0, 24);

  // ===== Screen 3 — FUEL ==========================================================
  s_fScr = mkScreen(kBlack);
  s_fCap = mkLabel(s_fScr, &geist_sans_32, kDim, "FUEL");
  lv_obj_set_style_text_letter_space(s_fCap, 2, 0);
  lv_obj_align(s_fCap, LV_ALIGN_TOP_LEFT, 20, 12);
  s_fPct = mkLabel(s_fScr, &geist_sans_24, kWhite, "--");
  lv_obj_align(s_fPct, LV_ALIGN_TOP_RIGHT, -20, 18);
  s_fBig = mkLabel(s_fScr, &geist_hero_96, kWhite, "--");   // big glanceable laps-left
  lv_obj_align(s_fBig, LV_ALIGN_CENTER, 0, -22);
  s_fBigCap = mkLabel(s_fScr, &geist_sans_24, kDim, "LAPS LEFT");
  lv_obj_set_style_text_letter_space(s_fBigCap, 2, 0);
  lv_obj_align(s_fBigCap, LV_ALIGN_CENTER, 0, 44);
  // Fuel-level bar across the bottom.
  s_fBar = lv_bar_create(s_fScr);
  lv_obj_set_size(s_fBar, W - 40, 14);
  lv_obj_align(s_fBar, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_set_style_bg_color(s_fBar, hex(0x2a2e34), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_fBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_fBar, 4, LV_PART_MAIN);
  lv_obj_set_style_radius(s_fBar, 4, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_fBar, hex(kGreen), LV_PART_INDICATOR);
  lv_bar_set_range(s_fBar, 0, 100);
  lv_bar_set_value(s_fBar, 0, LV_ANIM_OFF);
  s_fFoot = mkLabel(s_fScr, &geist_sans_24, kDim, "--");
  lv_obj_align(s_fFoot, LV_ALIGN_BOTTOM_MID, 0, -8);

  // ===== Screen 4 — PERFORMANCE ===================================================
  // Unified system metrics: game frame rate (game_fps — ETW-measured, works on every
  // sim) + whole-PC CPU / GPU load (sys_cpu_pct / sys_gpu_pct) from the recorder's
  // host-metrics feed. Sim-agnostic — replaced the iRacing-only fps/cpu_usage/gpu_usage.
  // Three stacked rows: a metric name + live value on the left, a rolling ~24 s
  // history trace on the right (lv_chart in SHIFT mode).
  s_gScr = mkScreen(kBlack);
  s_gCap = mkLabel(s_gScr, &geist_sans_16, kDim, "PERFORMANCE");
  lv_obj_set_style_text_letter_space(s_gCap, 3, 0);
  lv_obj_align(s_gCap, LV_ALIGN_TOP_MID, 0, 4);

  static const char*    gName[3] = { "FPS", "CPU", "GPU" };
  static const uint32_t gCol[3]  = { kGreen, kBlue, kAmber };  // trace/identity color
  static const int      gMax[3]  = { 160, 100, 100 };          // Y range (fps / % / %)
  const int rowTop = 26, rowH = 71, chartX = 92, chartW = W - chartX - 10;
  for (int i = 0; i < 3; i++) {
    const int y = rowTop + i * rowH;
    s_gName[i] = mkLabel(s_gScr, &geist_sans_16, gCol[i], gName[i]);
    lv_obj_set_pos(s_gName[i], 10, y + 6);
    s_gVal[i] = mkLabel(s_gScr, &geist_sans_32, kWhite, "--");
    lv_obj_set_pos(s_gVal[i], 10, y + 24);

    lv_obj_t* c = lv_chart_create(s_gScr);
    lv_obj_set_size(c, chartW, rowH - 12);
    lv_obj_set_pos(c, chartX, y + 2);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, 96);                 // 96 x 250 ms ≈ 24 s window
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, gMax[i]);
    lv_chart_set_div_line_count(c, 0, 0);
    lv_obj_set_style_bg_color(c, hex(0x101216), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);     // trace line thickness
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);      // hide the per-point dots
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_gChart[i] = c;
    s_gSer[i] = lv_chart_add_series(c, hex(gCol[i]), LV_CHART_AXIS_PRIMARY_Y);
  }

  // ===== Screens 5-7 — a dedicated full-screen trace per metric, WITH Y-axis numbers.
  // Same rolling data as the overview, but one metric fills the screen and the left
  // edge is labeled (0..max) so you can read the magnitude of a change.
  const int dcX = 44, dcY = 44, dcW = W - dcX - 10, dcH = 168;
  for (int i = 0; i < 3; i++) {
    lv_obj_t* s = mkScreen(kBlack);
    lv_obj_t* title = mkLabel(s, &geist_sans_24, gCol[i], gName[i]);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_set_pos(title, 8, 8);
    s_dVal[i] = mkLabel(s, &geist_sans_32, kWhite, "--");
    lv_obj_align(s_dVal[i], LV_ALIGN_TOP_RIGHT, -10, 6);

    lv_obj_t* c = lv_chart_create(s);
    lv_obj_set_size(c, dcW, dcH);
    lv_obj_set_pos(c, dcX, dcY);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, 160);                // 160 x 250 ms ≈ 40 s window
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, gMax[i]);
    lv_chart_set_div_line_count(c, 5, 0);            // 5 horizontal gridlines = the Y labels
    lv_obj_set_style_bg_color(c, hex(0x101216), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);            // series line
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);             // hide point dots
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(c, hex(0x2a2e34), LV_PART_MAIN); // gridlines
    lv_obj_set_style_line_width(c, 1, LV_PART_MAIN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_dChart[i] = c;
    s_dSer[i] = lv_chart_add_series(c, hex(gCol[i]), LV_CHART_AXIS_PRIMARY_Y);

    // Y-axis labels at 0, 1/4, 1/2, 3/4, max — aligned to the gridlines (max at top).
    for (int k = 0; k <= 4; k++) {
      char b[8]; snprintf(b, sizeof(b), "%d", gMax[i] * k / 4);
      lv_obj_t* yl = mkLabel(s, &geist_sans_12, kDim, b);
      lv_obj_set_width(yl, dcX - 8);
      lv_obj_set_style_text_align(yl, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(yl, 0, dcY + dcH * (4 - k) / 4 - 7);
    }
    s_dScr[i] = s;
  }

  s_pages[0] = s_tScr; s_pages[1] = s_pScr; s_pages[2] = s_fScr; s_pages[3] = s_gScr;
  s_pages[4] = s_dScr[0]; s_pages[5] = s_dScr[1]; s_pages[6] = s_dScr[2];
  s_strategy = mkScreen(kBlack);
  lv_obj_t* strategyTitle = mkLabel(s_strategy, &geist_sans_24, kWhite, "SESSION / PIT");
  lv_obj_set_pos(strategyTitle, 12, 8);
  for (int i = 0; i < 5; ++i) {
    s_strategyRows[i] = mkLabel(s_strategy, &lv_font_montserrat_20, kWhite, "--");
    lv_obj_set_width(s_strategyRows[i], W - 24);
    lv_label_set_long_mode(s_strategyRows[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_strategyRows[i], 12, 48 + i * 36);
  }
  s_pages[7] = s_strategy;
  s_page = 0;
  lv_screen_load(s_pages[0]);
  return s_pages[0];
}

void amoleddash_advance() {
  if (!s_pages[0]) return;
  s_page = (s_page + 1) % kPageN;
  lv_screen_load(s_pages[s_page]);
}

void amoleddash_tick(bool waiting) {
  const uint32_t now = millis();
  if (now - s_tickMs < kTickMs) return;
  s_tickMs = now;
  const bool flashOn = (now / kFlashMs) % 2 == 0;

  // ---- Screen 1: tire temps -------------------------------------------------------
  {
    int fl = (int)tcStore.getInt(TC_CH_TIRE_TEMP_FL);
    int fr = (int)tcStore.getInt(TC_CH_TIRE_TEMP_FR);
    int rl = (int)tcStore.getInt(TC_CH_TIRE_TEMP_RL);
    int rr = (int)tcStore.getInt(TC_CH_TIRE_TEMP_RR);
    const int corners[4] = { fl, fr, rl, rr };
    const int avg = (fl + fr + rl + rr) / 4;

    if (waiting) {
      setBg(s_tScr, s_tBgShown, kIdleBg);
      if (s_tZoneShown != -2) {
        s_tZoneShown = -2; s_tAvgShown = INT32_MIN;
        lv_label_set_text(s_tBig, "--");
        lv_label_set_text(s_tZone, "WAITING");
        lv_obj_set_style_text_color(s_tZone, hex(kDim), 0);
        for (int i = 0; i < 4; i++) { lv_label_set_text(s_tCorner[i], "--"); s_tCornerShown[i] = INT32_MIN; }
      }
    } else {
      // zone: 0 cold / 1 optimal / 2 hot / 3 overheat
      int zone; uint32_t col; const char* word;
      if      (avg <  kTireCold)  { zone = 0; col = kBlue;   word = "COLD"; }
      else if (avg <= kTireOptHi) { zone = 1; col = kGreen;  word = "OPTIMAL"; }
      else if (avg <= kTireHot)   { zone = 2; col = kYellow; word = "HOT"; }
      else                        { zone = 3; col = kRed;    word = "OVERHEAT"; }
      // Steady zone color — the "flash" for non-optimal zones is a brightness pulse
      // (applied globally at the end of tick), so we never repaint the whole frame.
      setBg(s_tScr, s_tBgShown, col);

      if (avg != s_tAvgShown) {
        s_tAvgShown = avg;
        char b[12]; snprintf(b, sizeof(b), "%d", avg);
        lv_label_set_text(s_tBig, b);
      }
      if (zone != s_tZoneShown) {
        s_tZoneShown = zone;
        lv_label_set_text(s_tZone, word);
        lv_obj_set_style_text_color(s_tZone, hex(kWhite), 0);
      }
      for (int i = 0; i < 4; i++) {
        if (corners[i] == s_tCornerShown[i]) continue;
        s_tCornerShown[i] = corners[i];
        static const char* tag[4] = { "FL", "FR", "RL", "RR" };
        char b[16]; snprintf(b, sizeof(b), "%s %d", tag[i], corners[i]);
        lv_label_set_text(s_tCorner[i], b);
      }
    }
  }

  // ---- Screen 2: pit limiter ------------------------------------------------------
  {
    const int state = waiting ? 2 : (tcStore.getBool(TC_CH_PIT_LIMITER) ? 1 : 0);
    // Engaged = steady amber (the pulse comes from the brightness flash); else black.
    setBg(s_pScr, s_pBgShown, state == 1 ? kAmber : kBlack);
    if (state != s_pStateShown) {
      s_pStateShown = state;
      if (state == 2) {
        lv_label_set_text(s_pCap, "PIT LIMITER");
        lv_obj_set_style_text_color(s_pCap, hex(kDim), 0);
        lv_label_set_text(s_pState, "--");
        lv_obj_set_style_text_color(s_pState, hex(kDim), 0);
      } else if (state == 1) {
        lv_label_set_text(s_pCap, "PIT LIMITER");
        lv_obj_set_style_text_color(s_pCap, hex(kWhite), 0);
        lv_label_set_text(s_pState, "ON");
        lv_obj_set_style_text_color(s_pState, hex(kWhite), 0);
      } else {
        lv_label_set_text(s_pCap, "PIT LIMITER");
        lv_obj_set_style_text_color(s_pCap, hex(kDim), 0);
        lv_label_set_text(s_pState, "OFF");
        lv_obj_set_style_text_color(s_pState, hex(kDim), 0);
      }
    }
  }

  // Session and pit data never infer zero/none from an absent channel.
  for (int i = 0; i < 5; ++i) {
    char b[96] = "--";
    if (!waiting) {
      switch (i) {
        case 0:
          if (tcStore.has(TC_CH_VIRTUAL_ENERGY_PCT)) snprintf(b, sizeof(b), "Energy: %.1f%%", (double)tcStore.getFloat(TC_CH_VIRTUAL_ENERGY_PCT));
          else snprintf(b, sizeof(b), "Energy: --");
          break;
        case 1: {
          char time[24] = "--", laps[24] = "--";
          const float secs = tcStore.getFloat(TC_CH_SESSION_TIME_LEFT);
          if (tcStore.has(TC_CH_SESSION_TIME_LEFT) && isfinite(secs) && secs >= 0) {
            const unsigned t = (unsigned)fminf(secs, 359999);
            snprintf(time, sizeof(time), "%u:%02u:%02u", t / 3600, t / 60 % 60, t % 60);
          }
          if (tcStore.has(TC_CH_SESSION_LAPS_LEFT)) snprintf(laps, sizeof(laps), "%ld", (long)tcStore.getInt(TC_CH_SESSION_LAPS_LEFT));
          snprintf(b, sizeof(b), "Left: %s | %s laps", time, laps);
          break;
        }
        case 2:
          snprintf(b, sizeof(b), "Pit request: %s", tcStore.has(TC_CH_PIT_REQUESTED) ? (tcStore.getBool(TC_CH_PIT_REQUESTED) ? "YES" : "NO") : "--");
          break;
        case 3: {
          const char* due = tcStore.has(TC_CH_PENALTY_DUE) ? tcStore.getStr(TC_CH_PENALTY_DUE) : "--";
          if (!*due) due = "None";
          if (tcStore.has(TC_CH_PENALTY_LAPS_LEFT)) snprintf(b, sizeof(b), "%s | %ld laps to serve", due, (long)tcStore.getInt(TC_CH_PENALTY_LAPS_LEFT));
          else snprintf(b, sizeof(b), "Penalty: %s", due);
          break;
        }
        case 4:
          if (tcStore.has(TC_CH_PIT_WAIT_S)) snprintf(b, sizeof(b), "%s: %.0f s", tcStore.has(TC_CH_PIT_WAIT_FOR) ? tcStore.getStr(TC_CH_PIT_WAIT_FOR) : "Repairs", (double)tcStore.getFloat(TC_CH_PIT_WAIT_S));
          else snprintf(b, sizeof(b), "Repair countdown: --");
          break;
      }
    }
    setText(s_strategyRows[i], s_strategyShown[i], sizeof(s_strategyShown[i]), b);
  }

  // ---- Screen 3: fuel -------------------------------------------------------------
  {
    if (waiting) {
      s_fCrit = false;
      setBg(s_fScr, s_fBgShown, kBlack);
      setText(s_fBig, s_fBigPrev, sizeof(s_fBigPrev), "--");
      setText(s_fPct, s_fPctPrev, sizeof(s_fPctPrev), "--");
      setText(s_fFoot, s_fFootPrev, sizeof(s_fFootPrev), "waiting for telemetry");
      if (s_fBarShown != 0) { s_fBarShown = 0; lv_bar_set_value(s_fBar, 0, LV_ANIM_OFF); }
    } else {
      const int   pct   = (int)tcStore.getInt(TC_CH_FUEL_PCT);
      const float lit   = tcStore.getFloat(TC_CH_FUEL_LITERS);
      const float laps  = tcStore.getFloat(TC_CH_FUEL_LAPS_LEFT);
      const float per   = tcStore.getFloat(TC_CH_FUEL_PER_LAP);
      const int   pctC  = pct < 0 ? 0 : pct > 100 ? 100 : pct;
      const bool  crit  = (tcStore.has(TC_CH_FUEL_PCT) && pctC <= kFuelCrit) || (tcStore.has(TC_CH_FUEL_LAPS_LEFT) && laps >= 0 && laps <= kLapsCrit);
      s_fCrit = crit;

      // Critical: steady red behind the readouts; the alarm pulse is the brightness
      // flash (applied at end of tick), so we don't repaint the frame every toggle.
      setBg(s_fScr, s_fBgShown, crit ? kRed : kBlack);

      char b[12];
      if (tcStore.has(TC_CH_FUEL_LAPS_LEFT)) snprintf(b, sizeof(b), "%.1f", laps); else snprintf(b, sizeof(b), "--");
      setText(s_fBig, s_fBigPrev, sizeof(s_fBigPrev), b);
      char p[8];
      if (tcStore.has(TC_CH_FUEL_PCT)) snprintf(p, sizeof(p), "%d%%", pctC); else snprintf(p, sizeof(p), "--");
      setText(s_fPct, s_fPctPrev, sizeof(s_fPctPrev), p);
      char l[12] = "--", burn[12] = "--", f[40];
      if (tcStore.has(TC_CH_FUEL_LITERS)) snprintf(l, sizeof(l), "%.1f", lit);
      if (tcStore.has(TC_CH_FUEL_PER_LAP)) snprintf(burn, sizeof(burn), "%.1f", per);
      snprintf(f, sizeof(f), "%s L   |   %s L/lap", l, burn);
      setText(s_fFoot, s_fFootPrev, sizeof(s_fFootPrev), f);

      const uint32_t barCol = pctC <= kFuelCrit ? kRed : pctC < kFuelLow ? kAmber : kGreen;
      if (barCol != s_fBarColShown) {
        s_fBarColShown = barCol;
        lv_obj_set_style_bg_color(s_fBar, hex(barCol), LV_PART_INDICATOR);
      }
      if (pctC != s_fBarShown) { s_fBarShown = pctC; lv_bar_set_value(s_fBar, pctC, LV_ANIM_OFF); }
    }
  }

  // ---- Screens 4-7: system performance traces (host CPU/GPU + cross-sim FPS) -------
  {
    // Unified metrics — game_fps is a plain fps float; sys_*_pct arrive already as
    // whole percentages (no 0..1 scaling like the old iRacing cpu_usage/gpu_usage).
    const int vals[3] = {
      (int)lroundf(tcStore.getFloat(TC_CH_GAME_FPS)),   // fps  (cross-sim, ETW)
      (int)tcStore.getInt(TC_CH_SYS_CPU_PCT),           // cpu% (host)
      (int)tcStore.getInt(TC_CH_SYS_GPU_PCT),           // gpu% (host)
    };

    // Push one trace sample + refresh the live values ~4 Hz (independent of the
    // faster flash cadence). We keep sampling even when this page is hidden, so
    // swiping to it shows real history, not an empty chart.
    if (now - s_gTraceMs >= 250) {
      s_gTraceMs = now;
      for (int i = 0; i < 3; i++) {
        // Per-metric gate: host CPU/GPU stream even with no sim; game_fps only while a
        // game runs. A 0 reading means "not reported" (a live PC is never a true 0%,
        // a running game never 0 fps), so 0 or waiting = blank that row independently.
        const bool on = !waiting && vals[i] > 0;
        if (on) {
          const int v = vals[i] < 0 ? 0 : vals[i];
          lv_chart_set_next_value(s_gChart[i], s_gSer[i], v);   // overview sparkline
          lv_chart_set_next_value(s_dChart[i], s_dSer[i], v);   // dedicated trace
          if (vals[i] != s_gValShown[i]) {
            s_gValShown[i] = s_dValShown[i] = vals[i];
            char b[8];
            if (i == 0) snprintf(b, sizeof(b), "%d", vals[i]);
            else        snprintf(b, sizeof(b), "%d%%", vals[i] < 0 ? 0 : vals[i]);
            lv_label_set_text(s_gVal[i], b);
            lv_label_set_text(s_dVal[i], b);
            // color by health: fps higher = better; cpu/gpu higher = worse
            const uint32_t col = (i == 0)
                ? (vals[0] >= 55 ? kGreen : vals[0] >= 30 ? kYellow : kRed)
                : (vals[i] >= 90 ? kRed : vals[i] >= 70 ? kYellow : kWhite);
            if (col != s_gValColShown[i]) {
              s_gValColShown[i] = s_dValColShown[i] = col;
              lv_obj_set_style_text_color(s_gVal[i], hex(col), 0);
              lv_obj_set_style_text_color(s_dVal[i], hex(col), 0);
            }
          }
        } else if (s_gValShown[i] != INT32_MIN) {
          // Went idle: blank the values, freeze the traces (don't inject phantom 0s).
          s_gValShown[i] = s_dValShown[i] = INT32_MIN;
          s_gValColShown[i] = s_dValColShown[i] = 0xFFFFFFFF;
          lv_label_set_text(s_gVal[i], "--");   lv_obj_set_style_text_color(s_gVal[i], hex(kDim), 0);
          lv_label_set_text(s_dVal[i], "--");   lv_obj_set_style_text_color(s_dVal[i], hex(kDim), 0);
        }
      }
    }
  }

  // ---- brightness pulse ("flash") for the ACTIVE screen ---------------------------
  // Brightness is global, so only the visible screen's flash state matters: tire when
  // out of the optimal band, pit when the limiter is engaged, fuel when critical.
  // src/main.cpp applies s_briWant via board::amoled().setBrightness() when it changes.
  const bool flashing =
      (s_page == 0 && s_tZoneShown >= 0 && s_tZoneShown != 1) ||   // tire cold/hot/overheat
      (s_page == 1 && s_pStateShown == 1) ||                        // pit limiter engaged
      (s_page == 2 && s_fCrit);                                     // fuel critical
  s_briWant = flashing ? (flashOn ? kBriFull : kBriDim) : kBriFull;
}

unsigned char amoleddash_brightness() { return s_briWant; }
