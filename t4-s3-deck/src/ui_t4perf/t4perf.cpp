// T4-S3 deck — PERFORMANCE page (450x600 portrait). Everything the AMOLED dash
// spreads over four screens (a perf overview plus a dedicated FPS / CPU / GPU
// trace) on ONE page, and the host memory metrics with it: five stacked rows —
// FPS, CPU, RAM, GPU, VRAM — each a name + live value + rolling trace with a
// labeled Y axis. Portrait height is what buys the consolidation; the AMOLED
// paged only because 536x240 landscape has room for three rows, not five.
//
// FPS is the sim's MEASURED frame rate (game_fps — the recorder counts the
// game's own present calls, so it works on every sim, not just the ones that
// publish a figure). CPU / RAM / GPU / VRAM are whole-host stats. All of it is
// LIVE-ONLY: streamed for the glass, never written to a session recording.
//
// Two rules this page is built around:
//
//   * No device-side smoothing of game_fps. The recorder publishes a ~1.5 s
//     trailing mean at ~1 Hz, so there is no raw per-sample signal on the wire
//     for us to average — smoothing it again only adds lag, and makes two decks
//     disagree by boot phase. Read it straight.
//     The sys_* metrics arrive the same way and get the same handling.
//   * An unpublished channel decodes as its type's zero, and an older recorder
//     can't say it is absent (only one that sends presence can, Store::has()). A live PC is never a true 0 % and a running game never a
//     true 0 fps, so a zero means "not reported": that row blanks to "--" and
//     its trace FREEZES rather than recording a phantom floor. Rows gate
//     independently — the host metrics stream with no sim attached, so CPU/RAM/
//     GPU/VRAM stay live while the FPS row sits blank until a game runs.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "t4perf.h"
#include "tc_generated.h"

// Geist SemiBold subsets, all ASCII + the degree glyph (0x20-0x7E,0xB0) — see
// each generated .c's Opts line. 16/24/28 are the shared p4dash faces, 10 is
// the t4tires page's; the deck env's build_src_filter already pulls all four.
LV_FONT_DECLARE(geist_sans_10);   // Y-axis numbers, secondary detail
LV_FONT_DECLARE(geist_sans_16);   // metric name
LV_FONT_DECLARE(geist_sans_24);   // page title
LV_FONT_DECLARE(geist_sans_28);   // live value

namespace {

// ---- palette — the deck's shared vocabulary (kPageBg + iOS dark system hues) ----
constexpr uint32_t kBg    = 0x0c0d10;   // page backdrop (matches ui_t4tires kPageBg)
constexpr uint32_t kPlot  = 0x14161a;   // chart well
constexpr uint32_t kGrid  = 0x2a2e34;   // gridlines
constexpr uint32_t kInk   = 0xf1f2f5;   // primary label
constexpr uint32_t kDim   = 0x8a919c;   // captions / secondary
constexpr uint32_t kMiss  = 0x5d5d61;   // unavailable ink ("--")
constexpr uint32_t kWarn  = 0xffd60a;   // value tint: getting heavy
constexpr uint32_t kCrit  = 0xff453a;   // value tint: pegged
// One identity color per metric (also its trace color).
constexpr uint32_t kFps   = 0x5ac8fa;   // cyan
constexpr uint32_t kCpu   = 0x0a84ff;   // blue
constexpr uint32_t kRam   = 0x30d158;   // green
constexpr uint32_t kGpu   = 0xff9f0a;   // amber
constexpr uint32_t kVram  = 0xbf5af2;   // purple

// ---- geometry (450x600) ---------------------------------------------------------
constexpr int kW      = 450;
constexpr int kRowTop = 38, kRowH = 112;          // 5 rows: 38 150 262 374 486 (ends 598)
constexpr int kMar    = 10;
constexpr int kYLabW  = 26;                        // Y-number column, right-aligned
constexpr int kChartX = 40, kChartY = 40, kChartH = 66;
constexpr int kChartW = kW - kChartX - kMar;      // 400
constexpr int kPoints = 100;                       // 100 x 500 ms = 50 s window

constexpr uint32_t kTickMs = 500;   // host publishes ~1 Hz; 2 Hz sampling keeps it smooth

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// ---- metric table ---------------------------------------------------------------
// SEC_NONE: no second line. CPU has none deliberately — there is no host CPU
// temperature channel, because no user-mode source reports a trustworthy
// package temp (the ACPI zone reads a board sensor tens of degrees low).
// Better one honest number than a second line quoting a wrong one.
enum SecKind { SEC_NONE, SEC_TEMP, SEC_GB };
// FPS is the only float channel and the only one whose scale isn't a percentage;
// isFps also flips the health ramp (more is better) and drops the % suffix.
struct Metric {
  const char* name; uint32_t color;
  int ch;                       // the value channel
  int max;                      // Y-axis top
  bool isFps;
  SecKind sec; int aCh, bCh;    // TEMP: aCh=temp; GB: aCh=used, bCh=total
};
const Metric kMetrics[5] = {
  { "FPS",  kFps,  TC_CH_GAME_FPS,     160, true,  SEC_NONE, -1, -1 },
  { "CPU",  kCpu,  TC_CH_SYS_CPU_PCT,  100, false, SEC_NONE, -1, -1 },
  { "RAM",  kRam,  TC_CH_SYS_RAM_PCT,  100, false, SEC_GB,   TC_CH_SYS_RAM_USED_GB,  TC_CH_SYS_RAM_TOTAL_GB },
  { "GPU",  kGpu,  TC_CH_SYS_GPU_PCT,  100, false, SEC_TEMP, TC_CH_SYS_GPU_TEMP_C,   -1 },
  { "VRAM", kVram, TC_CH_SYS_VRAM_PCT, 100, false, SEC_GB,   TC_CH_SYS_VRAM_USED_GB, TC_CH_SYS_VRAM_TOTAL_GB },
};
constexpr int kN = 5;

// ---- widgets + change caches (skip redundant repaints) --------------------------
lv_obj_t* s_val[kN]   = { nullptr };
lv_obj_t* s_sec[kN]   = { nullptr };
lv_obj_t* s_chart[kN] = { nullptr };
lv_chart_series_t* s_ser[kN] = { nullptr };
uint32_t s_tickMs = 0;
int      s_valShown[kN] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
uint32_t s_valColShown[kN] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
char     s_secShown[kN][24] = { "", "", "", "", "" };

lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, uint32_t col, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hex(col), 0);
  lv_label_set_text(l, t);
  return l;
}

void setText(lv_obj_t* l, char* cache, size_t cap, const char* txt) {
  if (strncmp(txt, cache, cap - 1) == 0) return;
  strncpy(cache, txt, cap - 1); cache[cap - 1] = '\0';
  lv_label_set_text(l, txt);
}

} // namespace

lv_obj_t* t4perf_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);   // own screen — the deck pager loads it
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = mkLabel(scr, &geist_sans_24, kInk, "PERFORMANCE");
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_set_pos(title, kMar, 6);
  lv_obj_t* sub = mkLabel(scr, &geist_sans_10, kDim, "LIVE ONLY");
  lv_obj_set_style_text_letter_space(sub, 1, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -kMar, 18);

  for (int i = 0; i < kN; i++) {
    const Metric& m = kMetrics[i];
    const int y = kRowTop + i * kRowH;

    // Header line: name left, live value right. The secondary detail sits under
    // the name so a long "12.4 / 32 GB" never collides with the value.
    lv_obj_t* name = mkLabel(scr, &geist_sans_16, m.color, m.name);
    lv_obj_set_style_text_letter_space(name, 1, 0);
    lv_obj_set_pos(name, kMar, y + 4);
    s_sec[i] = mkLabel(scr, &geist_sans_10, kDim, "");
    lv_obj_set_pos(s_sec[i], kMar, y + 24);
    s_val[i] = mkLabel(scr, &geist_sans_28, kMiss, "--");
    lv_obj_set_width(s_val[i], 160);
    lv_obj_set_style_text_align(s_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_val[i], kW - kMar - 160, y + 2);

    // Rolling trace. Three gridlines (not the AMOLED's five) — at 66 px tall,
    // five 10 px labels would sit on top of each other.
    lv_obj_t* c = lv_chart_create(scr);
    lv_obj_set_size(c, kChartW, kChartH);
    lv_obj_set_pos(c, kChartX, y + kChartY);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, kPoints);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, m.max);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_obj_set_style_bg_color(c, hex(kPlot), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);          // trace line
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);           // hide point dots
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(c, hex(kGrid), LV_PART_MAIN);  // gridlines
    lv_obj_set_style_line_width(c, 1, LV_PART_MAIN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_chart[i] = c;
    s_ser[i] = lv_chart_add_series(c, hex(m.color), LV_CHART_AXIS_PRIMARY_Y);

    // Y numbers at 0 / half / max, on the gridlines (max at top).
    for (int k = 0; k <= 2; k++) {
      char b[8]; snprintf(b, sizeof(b), "%d", m.max * k / 2);
      lv_obj_t* yl = mkLabel(scr, &geist_sans_10, kDim, b);
      lv_obj_set_width(yl, kYLabW);
      lv_obj_set_style_text_align(yl, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(yl, kChartX - kYLabW - 4, y + kChartY + kChartH * (2 - k) / 2 - 6);
    }
  }

  return scr;
}

void t4perf_tick(bool waiting) {
  const uint32_t now = millis();
  if (now - s_tickMs < kTickMs) return;
  s_tickMs = now;

  for (int i = 0; i < kN; i++) {
    const Metric& m = kMetrics[i];
    // game_fps is the one float; the sys_* percentages arrive as whole ints.
    const int v = m.isFps ? (int)lroundf(tcStore.getFloat(m.ch)) : (int)tcStore.getInt(m.ch);
    const bool on = !waiting && v > 0;   // 0 = not reported (see the header note)

    if (!on) {
      if (s_valShown[i] == INT32_MIN) continue;   // already blank
      // Went idle: blank the readouts, FREEZE the trace (no phantom zeros).
      s_valShown[i] = INT32_MIN;
      s_valColShown[i] = 0xFFFFFFFF;
      lv_label_set_text(s_val[i], "--");
      lv_obj_set_style_text_color(s_val[i], hex(kMiss), 0);
      setText(s_sec[i], s_secShown[i], sizeof(s_secShown[i]), "");
      continue;
    }

    const int clamped = v > m.max ? m.max : v;
    lv_chart_set_next_value(s_chart[i], s_ser[i], clamped);

    if (v != s_valShown[i]) {
      s_valShown[i] = v;
      char b[12];
      snprintf(b, sizeof(b), m.isFps ? "%d" : "%d%%", v);
      lv_label_set_text(s_val[i], b);
      // Health tint: for FPS more is better, for every load metric less is.
      const uint32_t col = m.isFps
          ? (v >= 55 ? kInk : v >= 30 ? kWarn : kCrit)
          : (v >= 90 ? kCrit : v >= 75 ? kWarn : kInk);
      if (col != s_valColShown[i]) {
        s_valColShown[i] = col;
        lv_obj_set_style_text_color(s_val[i], hex(col), 0);
      }
    }

    // Secondary detail — its own presence gate, so a machine that reports load
    // but no temperature still shows the load.
    char s[24];
    if (m.sec == SEC_NONE) {
      s[0] = '\0';
    } else if (m.sec == SEC_TEMP) {
      const int t = (int)tcStore.getInt(m.aCh);
      if (t > 0) snprintf(s, sizeof(s), "%d\xC2\xB0""C", t);
      else       snprintf(s, sizeof(s), "temp n/a");
    } else {
      const float used = tcStore.getFloat(m.aCh), total = tcStore.getFloat(m.bCh);
      if (total > 0) snprintf(s, sizeof(s), "%.1f / %.0f GB", used < 0 ? 0.0f : used, total);
      else           snprintf(s, sizeof(s), "-- GB");
    }
    setText(s_sec[i], s_secShown[i], sizeof(s_secShown[i]), s);
  }
}
