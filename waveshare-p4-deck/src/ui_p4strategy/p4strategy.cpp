#include "p4strategy.h"
#include <TraceCraft.h>
#include "tc_generated.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

using namespace tracecraft;
namespace {
lv_obj_t* values[6];
lv_obj_t* details[6];
char shown[6][64] = {};
char footShown[6][96] = {};
void text(int i, const char* value, const char* detail) {
  if (strcmp(shown[i], value)) {
    snprintf(shown[i], sizeof(shown[i]), "%s", value);
    lv_label_set_text(values[i], shown[i]);
  }
  if (strcmp(footShown[i], detail)) {
    snprintf(footShown[i], sizeof(footShown[i]), "%s", detail);
    lv_label_set_text(details[i], footShown[i]);
  }
}
bool number(uint8_t channel, char* out, size_t size, const char* format) {
  if (!tcStore.has(channel)) { snprintf(out, size, "--"); return false; }
  const float value = tcStore.getFloat(channel);
  if (!isfinite(value) || value < 0) { snprintf(out, size, "--"); return false; }
  snprintf(out, size, format, (double)value);
  return true;
}
}

lv_obj_t* p4strategy_setup() {
  lv_obj_t* screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101216), 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* title = lv_label_create(screen);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xf1f2f5), 0);
  lv_label_set_text(title, "FUEL / SESSION / PIT");
  lv_obj_set_pos(title, 30, 22);
  const char* headings[] = {"FUEL", "VIRTUAL ENERGY", "SESSION LEFT", "PIT STATUS", "PENALTY DUE", "MANDATORY REPAIRS"};
  for (int i = 0; i < 6; ++i) {
    lv_obj_t* card = lv_obj_create(screen);
    lv_obj_set_size(card, 392, 270);
    lv_obj_set_pos(card, 30 + (i % 3) * 414, 90 + (i / 3) * 292);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1b2028), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* heading = lv_label_create(card);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x9aa8ba), 0);
    lv_label_set_text(heading, headings[i]);
    values[i] = lv_label_create(card);
    lv_obj_set_width(values[i], 350);
    lv_obj_set_pos(values[i], 0, 62);
    lv_obj_set_style_text_font(values[i], &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(values[i], lv_color_hex(0xf1f2f5), 0);
    lv_label_set_text(values[i], "--");
    details[i] = lv_label_create(card);
    lv_obj_set_width(details[i], 350);
    lv_obj_set_pos(details[i], 0, 150);
    lv_obj_set_style_text_font(details[i], &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(details[i], lv_color_hex(0x9aa8ba), 0);
    lv_label_set_text(details[i], "");
  }
  return screen;
}

void p4strategy_tick(bool waiting) {
  if (waiting) { for (int i = 0; i < 6; ++i) text(i, "--", "Waiting for telemetry"); return; }
  char a[64], b[96], x[24], y[24];
  number(TC_CH_FUEL_LITERS, a, sizeof(a), "%.1f L");
  number(TC_CH_FUEL_LAPS_LEFT, x, sizeof(x), "%.1f laps");
  number(TC_CH_FUEL_PER_LAP, y, sizeof(y), "%.2f L/lap");
  snprintf(b, sizeof(b), "%s | %s", x, y); text(0, a, b);
  number(TC_CH_VIRTUAL_ENERGY_PCT, a, sizeof(a), "%.1f%%");
  text(1, a, "Per-stint budget, where supplied");
  if (tcStore.has(TC_CH_SESSION_TIME_LEFT) && isfinite(tcStore.getFloat(TC_CH_SESSION_TIME_LEFT)) && tcStore.getFloat(TC_CH_SESSION_TIME_LEFT) >= 0) {
    const unsigned seconds = (unsigned)fminf(tcStore.getFloat(TC_CH_SESSION_TIME_LEFT), 359999);
    snprintf(a, sizeof(a), "%u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
  } else snprintf(a, sizeof(a), "--");
  if (tcStore.has(TC_CH_SESSION_LAPS_LEFT)) snprintf(b, sizeof(b), "%ld laps left", (long)tcStore.getInt(TC_CH_SESSION_LAPS_LEFT));
  else snprintf(b, sizeof(b), "Laps left: --");
  text(2, a, b);
  const char* pit = "--";
  if (tcStore.has(TC_CH_PIT_SERVICE_ACTIVE) && tcStore.getBool(TC_CH_PIT_SERVICE_ACTIVE)) pit = "Service active";
  else if (tcStore.has(TC_CH_IN_PIT_BOX) && tcStore.getBool(TC_CH_IN_PIT_BOX)) pit = "In box";
  else if (tcStore.has(TC_CH_IN_PIT) && tcStore.getBool(TC_CH_IN_PIT)) pit = "In pit lane";
  else if (tcStore.has(TC_CH_PIT_ENTRY) && tcStore.getBool(TC_CH_PIT_ENTRY)) pit = "Pit entry";
  else if (tcStore.has(TC_CH_PIT_REQUESTED) && tcStore.getBool(TC_CH_PIT_REQUESTED)) pit = "Stop requested";
  else if (tcStore.has(TC_CH_IN_PIT)) pit = "Outside pit lane";
  text(3, pit, tcStore.has(TC_CH_PIT_LIMITER) ? (tcStore.getBool(TC_CH_PIT_LIMITER) ? "Limiter ON" : "Limiter OFF") : "Limiter: --");
  const char* penalty = tcStore.has(TC_CH_PENALTY_DUE) ? tcStore.getStr(TC_CH_PENALTY_DUE) : "--";
  if (!*penalty) penalty = "None";
  if (tcStore.has(TC_CH_PENALTY_LAPS_LEFT)) snprintf(b, sizeof(b), "Serve within %ld laps (incl. this lap)", (long)tcStore.getInt(TC_CH_PENALTY_LAPS_LEFT));
  else b[0] = '\0';
  text(4, penalty, b);
  number(TC_CH_PIT_WAIT_S, a, sizeof(a), "%.0f s");
  text(5, a, tcStore.has(TC_CH_PIT_WAIT_FOR) ? tcStore.getStr(TC_CH_PIT_WAIT_FOR) : "No repair countdown supplied");
}
