// TraceCraft P4 deck — SYSTEM diagnostics page. 1280x720 landscape.
//
// The host PC's vitals as rolling history graphs — CPU / RAM / GPU / VRAM — from the
// recorder's sys_* channels (GH: host-metrics). Four stacked rows, each with the
// metric name + a big live %, a secondary detail (CPU/GPU temperature, RAM/VRAM
// used / total GB), and a wide scrolling trace with a 0..100 Y axis so you can read
// the magnitude of a spike at a glance.
//
// These are LIVE-ONLY, sim-agnostic host stats (they stream even with no sim
// attached) and are never written to a session recording. Fields the machine can't
// report (a temp with no readable sensor, VRAM % with no driver capacity) simply
// don't arrive and show as "--" — never invented. Typography is Geist, the deck's
// house font.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "p4sys.h"
#include "tc_generated.h"

LV_FONT_DECLARE(geist_mono_22);   // page title — the deck's shared heading voice
LV_FONT_DECLARE(geist_sans_40);   // live value (big %)
LV_FONT_DECLARE(geist_sans_28);   // metric name
LV_FONT_DECLARE(geist_sans_24);   // secondary detail (temp / GB)
LV_FONT_DECLARE(geist_sans_16);   // Y-axis numbers

namespace {

// ---- palette — iOS dark-mode system colors (shared vocabulary with the weather page)
constexpr uint32_t kBg     = 0x0c0d10;   // page backdrop (matches kPageBg)
constexpr uint32_t kPlot   = 0x14161a;   // chart well
constexpr uint32_t kGrid   = 0x2a2e34;   // gridlines
constexpr uint32_t kInk    = 0xf1f2f5;   // primary label
constexpr uint32_t kDim    = 0x8a919c;   // secondary / captions
constexpr uint32_t kMiss   = 0x5d5d61;   // unavailable ink
// One identity color per metric (also the trace + value color).
constexpr uint32_t kCpu    = 0x0a84ff;   // blue
constexpr uint32_t kRam    = 0x30d158;   // green
constexpr uint32_t kGpu    = 0xff9f0a;   // amber
constexpr uint32_t kVram   = 0xbf5af2;   // purple

// ---- geometry (1280x720) --------------------------------------------------------
constexpr int kW = 1280;
constexpr int kRowTop = 56, kRowH = 162;          // 4 rows: 56, 218, 380, 542
constexpr int kColX = 28, kColW = 258;            // left column (name / value / detail)
constexpr int kYLabX = 290, kYLabW = 44;          // Y-axis number column
constexpr int kChartX = 340, kChartH = 132;
constexpr int kChartW = kW - kChartX - 28;        // 912
constexpr int kPoints = 120;                       // 120 x 500 ms ≈ 60 s window

constexpr uint32_t kTickMs = 500;   // host metrics update ~1 Hz; 2 Hz sampling is smooth

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// ---- metric table ---------------------------------------------------------------
// SEC_NONE: this metric has no second line. CPU is the only one — there is
// no host CPU temperature channel, because no user-mode source reports a
// trustworthy package temp (the ACPI zone reads a board sensor tens of
// degrees low). Better a row with one honest number than a second line
// quoting a wrong one.
enum SecKind { SEC_TEMP, SEC_GB, SEC_NONE };
struct Metric {
  const char* name; uint32_t color;
  int pctCh;                 // TC_CH_SYS_*_PCT
  SecKind sec; int aCh, bCh; // TEMP: aCh=temp; GB: aCh=used, bCh=total
};
const Metric kMetrics[4] = {
  { "CPU",  kCpu,  TC_CH_SYS_CPU_PCT,  SEC_NONE, -1, -1 },
  { "RAM",  kRam,  TC_CH_SYS_RAM_PCT,  SEC_GB,   TC_CH_SYS_RAM_USED_GB, TC_CH_SYS_RAM_TOTAL_GB },
  { "GPU",  kGpu,  TC_CH_SYS_GPU_PCT,  SEC_TEMP, TC_CH_SYS_GPU_TEMP_C, -1 },
  { "VRAM", kVram, TC_CH_SYS_VRAM_PCT, SEC_GB,   TC_CH_SYS_VRAM_USED_GB, TC_CH_SYS_VRAM_TOTAL_GB },
};

// ---- widgets + caches -----------------------------------------------------------
lv_obj_t* s_scr = nullptr;
lv_obj_t* s_val[4]  = { nullptr, nullptr, nullptr, nullptr };
lv_obj_t* s_sec[4]  = { nullptr, nullptr, nullptr, nullptr };
lv_obj_t* s_chart[4]= { nullptr, nullptr, nullptr, nullptr };
lv_chart_series_t* s_ser[4] = { nullptr, nullptr, nullptr, nullptr };
uint32_t s_tickMs = 0;
int  s_valShown[4] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
char s_secShown[4][24] = { "", "", "", "" };

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

lv_obj_t* p4sys_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);   // own screen — the deck pager loads it
  s_scr = scr;
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = mkLabel(scr, &geist_mono_22, kInk, "SYSTEM");
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_set_pos(title, kColX + 4, 16);
  lv_obj_t* sub = mkLabel(scr, &geist_sans_16, kDim, "HOST PC  /  LIVE ONLY");
  lv_obj_set_style_text_letter_space(sub, 1, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -28, 22);

  for (int i = 0; i < 4; i++) {
    const Metric& m = kMetrics[i];
    const int y = kRowTop + i * kRowH;

    // Left column: name / big value / secondary detail.
    lv_obj_t* name = mkLabel(scr, &geist_sans_28, m.color, m.name);
    lv_obj_set_style_text_letter_space(name, 1, 0);
    lv_obj_set_pos(name, kColX, y + 6);
    s_val[i] = mkLabel(scr, &geist_sans_40, kInk, "--");
    lv_obj_set_pos(s_val[i], kColX, y + 42);
    s_sec[i] = mkLabel(scr, &geist_sans_24, kDim, "--");
    lv_obj_set_pos(s_sec[i], kColX, y + 96);

    // Rolling trace (0..100 %).
    lv_obj_t* c = lv_chart_create(scr);
    lv_obj_set_size(c, kChartW, kChartH);
    lv_obj_set_pos(c, kChartX, y + 8);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, kPoints);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(c, 5, 0);          // 5 horizontal gridlines = the Y labels
    lv_obj_set_style_bg_color(c, hex(kPlot), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);          // trace line
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);           // hide point dots
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(c, hex(kGrid), LV_PART_MAIN);  // gridlines
    lv_obj_set_style_line_width(c, 1, LV_PART_MAIN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_chart[i] = c;
    s_ser[i] = lv_chart_add_series(c, hex(m.color), LV_CHART_AXIS_PRIMARY_Y);

    // Y-axis numbers at 0 / 25 / 50 / 75 / 100, aligned to the gridlines.
    for (int k = 0; k <= 4; k++) {
      char b[8]; snprintf(b, sizeof(b), "%d", 25 * k);
      lv_obj_t* yl = mkLabel(scr, &geist_sans_16, kDim, b);
      lv_obj_set_width(yl, kYLabW);
      lv_obj_set_style_text_align(yl, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_set_pos(yl, kYLabX, (y + 8) + kChartH * (4 - k) / 4 - 9);
    }
  }

  return scr;
}

void p4sys_tick(bool waiting) {
  const uint32_t now = millis();
  if (now - s_tickMs < kTickMs) return;
  s_tickMs = now;

  for (int i = 0; i < 4; i++) {
    const Metric& m = kMetrics[i];
    const int pct = (int)tcStore.getInt(m.pctCh);
    // A metric the host isn't reporting arrives as 0 (the recorder omits unreal
    // fields). Treat waiting, or a 0 %, as "no data" — a running PC is never at a
    // true 0 % on CPU/RAM, and an idle GPU reads a small non-zero anyway.
    const bool on = !waiting && pct > 0;

    if (on) {
      const int p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
      lv_chart_set_next_value(s_chart[i], s_ser[i], p);
      if (pct != s_valShown[i]) {
        s_valShown[i] = pct;
        char b[8]; snprintf(b, sizeof(b), "%d%%", p);
        lv_label_set_text(s_val[i], b);
        lv_obj_set_style_text_color(s_val[i],
            hex(p >= 90 ? 0xff453a : p >= 75 ? 0xffd60a : kInk), 0);   // load tint
      }
      // Secondary detail.
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
    } else if (s_valShown[i] != INT32_MIN) {
      // Went idle: blank the value + detail, freeze the trace (no phantom 0s).
      s_valShown[i] = INT32_MIN;
      lv_label_set_text(s_val[i], "--");
      lv_obj_set_style_text_color(s_val[i], hex(kMiss), 0);
      setText(s_sec[i], s_secShown[i], sizeof(s_secShown[i]), "--");
    }
  }
}
