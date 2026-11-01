// P4 deck — FPS page (1280x720 landscape). One question, answered big: is the
// game holding frame rate? A hero readout (color-coded: green >= 55, amber
// >= 30, red below) and a ~60 s rolling trace that fills the panel, so a
// stutter or a slow decline is visible as SHAPE, not just a number you had to
// be watching at the right moment.
//
// Reads game_fps — the ETW present-tracing frame rate measured on the host,
// which works on EVERY sim (unlike the retired iRacing-only fps channel).
// LIVE-ONLY: streamed to dashboards, never written to recordings. A 0 means
// "no game presenting" (the ETW monitor publishes nothing without a game), so
// zero gates the page into its idle state — the trace freezes rather than
// plotting phantom zeros, same convention as the SYSTEM page.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include "p4fps.h"
#include "tc_generated.h"

LV_FONT_DECLARE(geist_mono_22);    // page title (deck heading voice)
LV_FONT_DECLARE(geist_mono_240);   // hero fps readout (digits, 179px ink)
LV_FONT_DECLARE(geist_sans_16);    // subtitle + Y-axis numbers

namespace {

// ---- palette (house vocabulary) -------------------------------------------------
constexpr uint32_t kBg    = 0x0c0d10;
constexpr uint32_t kPlot  = 0x14161a;   // chart well
constexpr uint32_t kGrid  = 0x2a2e34;
constexpr uint32_t kInk   = 0xf1f2f5;
constexpr uint32_t kDim   = 0x8a919c;
constexpr uint32_t kMiss  = 0x5d5d61;
constexpr uint32_t kGreen = 0x30d158;   // healthy (also the trace color)
constexpr uint32_t kAmber = 0xffd60a;
constexpr uint32_t kRed   = 0xff453a;

// ---- geometry (1280x720) ----------------------------------------------------------
constexpr int kChartX = 100, kChartY = 214;
constexpr int kChartW = 1280 - kChartX - 36;    // 1144
constexpr int kChartH = 452;
constexpr int kYLabX = 40, kYLabW = 48;
constexpr int kRangeMax = 160;                  // Y axis 0..160 fps
constexpr int kPoints = 240;                    // 240 x 250 ms = 60 s window
constexpr uint32_t kTickMs = 250;
// NO SMOOTHING HERE. game_fps already arrives as a ~1.5 s trailing mean,
// refreshed about once a second by the host — the averaging this page needs
// has happened before the byte reaches the device.
//
// This used to average 6 samples x 250 ms and call it "a 1.5 s window"
//. Against a 1 Hz host update a 6-slot window at 250 ms holds at
// most TWO distinct readings — typically four copies of one and two of the
// next — so it was not 6 samples, it was ~1.5 host readings weighted by
// whenever this deck happened to boot. Two identical decks on the same PC
// could show different heroes for that reason alone. Stacked on the host's
// own window it also made the effective support ~3 s: the P4 lagged the
// AMOLED dash, off the same channel, on the same desk.
//
// Keep this at 1 unless the HOST stops smoothing.
constexpr int kAvgN = 1;

// ---- widgets + caches --------------------------------------------------------------
lv_obj_t* s_hero  = nullptr;
lv_obj_t* s_chart = nullptr;
lv_chart_series_t* s_ser = nullptr;
uint32_t s_tickMs = 0;
int  s_heroShown = INT32_MIN;
uint32_t s_heroColShown = 0;
float s_win[kAvgN];
int   s_winIdx = 0, s_winCount = 0;

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, uint32_t col, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hex(col), 0);
  lv_label_set_text(l, t);
  return l;
}

} // namespace

lv_obj_t* p4fps_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = mkLabel(scr, &geist_mono_22, kInk, "FPS");
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_set_pos(title, 36, 28);
  // States the HOST's window, which is where the averaging happens. The old
  // text claimed "1.5s AVG" while the hero was really ~3 s wide —
  // a number on glass that was wrong about itself.
  lv_obj_t* sub = mkLabel(scr, &geist_sans_16, kDim, "GAME FRAME RATE  /  ~1.5s AVG (HOST)  /  LIVE ONLY");
  lv_obj_set_style_text_letter_space(sub, 1, 0);
  lv_obj_set_pos(sub, 36, 62);

  // Hero readout, right-aligned across the top — the 240px digit face
  // (179px ink -> rows 14..193; the chart starts below at 214).
  s_hero = mkLabel(scr, &geist_mono_240, kMiss, "--");
  lv_obj_set_width(s_hero, 460);
  lv_obj_set_style_text_align(s_hero, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_hero, 1280 - 36 - 460, 14);

  // The trace: one big rolling chart, 0..160 fps over ~60 s.
  lv_obj_t* c = lv_chart_create(scr);
  lv_obj_set_size(c, kChartW, kChartH);
  lv_obj_set_pos(c, kChartX, kChartY);
  lv_chart_set_type(c, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(c, kPoints);
  lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, kRangeMax);
  lv_chart_set_div_line_count(c, 5, 0);          // gridlines at the Y labels
  lv_obj_set_style_bg_color(c, hex(kPlot), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 8, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_set_style_line_width(c, 3, LV_PART_ITEMS);          // trace line
  lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);           // no point dots
  lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(c, hex(kGrid), LV_PART_MAIN);
  lv_obj_set_style_line_width(c, 1, LV_PART_MAIN);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  s_chart = c;
  s_ser = lv_chart_add_series(c, hex(kGreen), LV_CHART_AXIS_PRIMARY_Y);

  // Y-axis numbers at 0/40/80/120/160, aligned to the gridlines.
  for (int k = 0; k <= 4; k++) {
    char b[8]; snprintf(b, sizeof(b), "%d", kRangeMax * k / 4);
    lv_obj_t* yl = mkLabel(scr, &geist_sans_16, kDim, b);
    lv_obj_set_width(yl, kYLabW);
    lv_obj_set_style_text_align(yl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(yl, kYLabX, kChartY + kChartH * (4 - k) / 4 - 9);
  }

  return scr;
}

void p4fps_tick(bool waiting) {
  const uint32_t now = millis();
  if (now - s_tickMs < kTickMs) return;
  s_tickMs = now;

  const int fps = waiting ? 0 : (int)lroundf(tcStore.getFloat(TC_CH_GAME_FPS));
  // 0 = no game presenting (ETW publishes nothing without one) — idle, not data.
  const bool on = !waiting && fps > 0;

  if (on) {
    // Trace: the RAW sample, so real stutters dent the line.
    const int v = fps > kRangeMax ? kRangeMax : fps;
    lv_chart_set_next_value(s_chart, s_ser, v);

    // Hero: the host's value, rounded. kAvgN is 1 — see the note above; the
    // ring stays so re-enabling smoothing is a one-constant change if the
    // host ever stops doing it.
    s_win[s_winIdx] = (float)fps;
    s_winIdx = (s_winIdx + 1) % kAvgN;
    if (s_winCount < kAvgN) s_winCount++;
    float sum = 0;
    for (int i = 0; i < s_winCount; i++) sum += s_win[i];
    const int avg = (int)lroundf(sum / (float)s_winCount);
    if (avg != s_heroShown) {
      s_heroShown = avg;
      char b[8]; snprintf(b, sizeof(b), "%d", avg);
      lv_label_set_text(s_hero, b);
      const uint32_t col = avg >= 55 ? kGreen : avg >= 30 ? kAmber : kRed;
      if (col != s_heroColShown) {
        s_heroColShown = col;
        lv_obj_set_style_text_color(s_hero, hex(col), 0);
      }
    }
  } else if (s_heroShown != INT32_MIN) {
    // Game gone: blank the hero, freeze the trace (no phantom zeros), and
    // drop the window so a returning game starts a fresh average.
    s_heroShown = INT32_MIN;
    s_heroColShown = 0;
    s_winIdx = s_winCount = 0;
    lv_label_set_text(s_hero, "--");
    lv_obj_set_style_text_color(s_hero, hex(kMiss), 0);
  }
}
