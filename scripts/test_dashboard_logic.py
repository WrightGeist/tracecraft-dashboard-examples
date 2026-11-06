"""Run actual firmware formatting functions with a fake telemetry store on the host.

Requires a host C++ compiler (g++ by default). No board or simulator is needed.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r"^(?:static )?(?:void|bool|int32_t) " + name + r"\(", source, re.M).start()
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


rpm = (ROOT / "t4-s3-deck/src/eez_effects/eez_effects.h").read_text()
macro = (ROOT / "macropad-dash/src/main.cpp").read_text()
dash = (ROOT / "waveshare-p4-deck/src/ui_p4dash/p4dash.cpp").read_text()
strategy = (ROOT / "waveshare-p4-deck/src/ui_p4strategy/p4strategy.cpp").read_text()
functions = "\n".join([
    function(rpm, "dashRpmMax"), function(rpm, "dashRpmShift"),
    macro[macro.index("enum class Fmt"):macro.index("struct Page")],
    function(macro, "fmtCell"), function(dash, "formatLevel"),
    function(strategy, "number"), function(strategy, "p4strategy_tick"),
])
pages = macro[macro.index("struct Page"):macro.index("#undef TBD") + len("#undef TBD")]
layout = macro[macro.index("static constexpr int kColX"):macro.index("static void drawDashboard")]
views = sorted(set(re.findall(r"TC_VIEW_[A-Z_]+", pages)))
functions += "\n" + "\n".join(f"const uint8_t {view}[] = {{0}};" for view in views)
functions += "\n" + pages + "\n" + layout
channels = sorted(set(re.findall(r"TC_CH_[A-Z0-9_]+", functions)) | {"TC_CH_TC_LEVEL"})
program = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using std::isfinite;
struct FakeStore {
  bool present[256] = {};
  float value[256] = {};
  std::string strings[256];
  bool has(uint8_t c) { return present[c]; }
  int32_t getInt(uint8_t c) { return (int32_t)value[c]; }
  float getFloat(uint8_t c) { return value[c]; }
  bool getBool(uint8_t c) { return value[c] != 0; }
  const char* getStr(uint8_t c) { return strings[c].c_str(); }
  void put(uint8_t c, float v) { present[c] = true; value[c] = v; }
} tcStore;
std::string display[6], detail[6];
void text(int i, const char* v, const char* d) { display[i] = v; detail[i] = d; }
'''
program += "enum { " + ", ".join(channels) + " };\n" + functions
program += r'''
int main() {
  assert(dashRpmMax() == 8000 && dashRpmShift() == 7680);
  tcStore.put(TC_CH_RPM_MAX, 6200);
  tcStore.put(TC_CH_RPM_SHIFT, 5800);
  assert(dashRpmMax() == 6200 && dashRpmShift() == 5800);
  tcStore.put(TC_CH_RPM_SHIFT, 9000);
  assert(dashRpmShift() == 5952); // Invalid cue must not exceed the car's limiter.
  tcStore.put(TC_CH_RPM_MAX, 20000);
  tcStore.put(TC_CH_RPM_SHIFT, 19000);
  assert(dashRpmShift() == 19000);
  tcStore = FakeStore{};
  char output[32];
  Cell dial = {"TC", TC_CH_TC_LEVEL, Fmt::Aid, 0};
  fmtCell(output, sizeof output, dial, false);
  assert(!strcmp(output, "--"));
  tcStore.put(TC_CH_TC_LEVEL, 0);
  fmtCell(output, sizeof output, dial, false);
  assert(!strcmp(output, "OFF"));
  tcStore.put(TC_CH_TC_LEVEL, 7);
  fmtCell(output, sizeof output, dial, false);
  assert(!strcmp(output, "7"));
  fmtCell(output, sizeof output, dial, true);
  assert(!strcmp(output, "--"));
  dial.fmt = Fmt::Dial;
  tcStore.put(TC_CH_TC_LEVEL, 0);
  fmtCell(output, sizeof output, dial, false);
  assert(!strcmp(output, "0"));
  dial.fmt = Fmt::Bool;
  tcStore.put(TC_CH_TC_LEVEL, 0);
  fmtCell(output, sizeof output, dial, false);
  assert(!strcmp(output, "OFF"));
  tcStore.put(TC_CH_TC_LEVEL, 1);
  fmtCell(output, sizeof output, dial, false);
  assert(!strcmp(output, "ON"));
  formatLevel(output, sizeof output, INT32_MIN, true);
  assert(!strcmp(output, "--"));
  formatLevel(output, sizeof output, 0, true);
  assert(!strcmp(output, "OFF"));
  formatLevel(output, sizeof output, 0, false);
  assert(!strcmp(output, "0"));
  // Real MacroPad page definitions and layout: three-character OFF indicators
  // must fit the right edge of the 128-pixel OLED, alongside the dial labels.
  for (const Page& page : kPages) {
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 4; ++row) {
        const Cell& cell = page.cells[row][col];
        tcStore.put(cell.ch, cell.fmt == Fmt::Pct1 ? 55.5f : 0);
        fmtCell(output, sizeof output, cell, false);
        const int right = col == 2 ? 128 : kColX[col + 1];
        if (valueX(page, col) + textWidth(output) > right) fprintf(stderr, "%s row %d col %d label %s value %s ends %d limit %d\n", page.title, row, col, cell.label, output, valueX(page, col) + textWidth(output), right);
        assert(valueX(page, col) + textWidth(output) <= right);
      }
    }
  }
  tcStore = FakeStore{};
  p4strategy_tick(false);
  for (const auto& value : display) assert(value == "--");
  tcStore.put(TC_CH_FUEL_LITERS, 0);
  tcStore.put(TC_CH_VIRTUAL_ENERGY_PCT, 0);
  tcStore.put(TC_CH_SESSION_TIME_LEFT, 3661);
  tcStore.put(TC_CH_SESSION_LAPS_LEFT, 0);
  tcStore.put(TC_CH_PIT_REQUESTED, 1);
  tcStore.present[TC_CH_PENALTY_DUE] = true;
  tcStore.put(TC_CH_PIT_WAIT_S, 0);
  p4strategy_tick(false);
  assert(display[0] == "0.0 L" && display[1] == "0.0%");
  assert(display[2] == "1:01:01" && detail[2] == "0 laps left");
  assert(display[3] == "Stop requested" && display[4] == "None");
  assert(display[5] == "0 s");
  tcStore.strings[TC_CH_PENALTY_DUE] = "Drive-through";
  tcStore.put(TC_CH_PENALTY_LAPS_LEFT, 1);
  tcStore.put(TC_CH_IN_PIT, 1);
  tcStore.put(TC_CH_PIT_WAIT_S, 14);
  p4strategy_tick(false);
  assert(display[3] == "In pit lane" && display[4] == "Drive-through");
  assert(detail[4].find("1 laps") != std::string::npos && display[5] == "14 s");
  tcStore.put(TC_CH_PIT_SERVICE_ACTIVE, 1);
  p4strategy_tick(false);
  assert(display[3] == "Service active");
  tcStore.present[TC_CH_VIRTUAL_ENERGY_PCT] = false;
  tcStore.put(TC_CH_PIT_WAIT_S, NAN);
  p4strategy_tick(false);
  assert(display[1] == "--" && display[5] == "--");
  p4strategy_tick(true);
  for (const auto& value : display) assert(value == "--");
  puts("PASS: per-car RPM, missing vs zero/off, pit priority, penalties, repairs and disconnect state");
}
'''
with tempfile.TemporaryDirectory(prefix="tracecraft-logic-") as directory:
    source = Path(directory) / "check.cpp"
    binary = Path(directory) / ("check.exe" if os.name == "nt" else "check")
    source.write_text(program, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
