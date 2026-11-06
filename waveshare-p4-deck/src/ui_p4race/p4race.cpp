// P4 deck — RACE page (1280x720 landscape).
//
//   RPM LIGHTS — two mirrored 8-LED banks across the top, meeting at the
//                centre. Fixed F1-wheel colours per slot (outer green x3,
//                yellow x2, red x3 toward centre); the banks FILL inward as
//                revs rise toward the car's shift point, and AT the shift rpm
//                (rpm_shift; ~93% of rpm_max as fallback) the whole bar
//                flashes BRIGHT BLUE — the shift cue.
//   GEAR       — huge, centred beneath the lights (geist_mono_240: the face
//                whose digits+N+R subset exists exactly for this).
//   POSITION   — beneath it: the driver's CLASS position ("of N" when the sim
//                publishes a class size), falling back to overall position on
//                sims without a per-class value. P1 gold, the podium warm.
//   TC LIGHTS  — four cyan lamps down each screen edge, flashing while
//                traction control is actively intervening.
//   THE FLAG   — a thick border framing the ENTIRE screen in the current
//                flag's colour. No flag, no border; green shows green;
//                checkered borders WHITE (its near-black would vanish).
//
// An older recorder can't express absence (only one that sends presence can,
// Store::has()), so this page keeps the rule that works with every recorder: a zero
// position / empty gear string means "not reported" and renders as a dash — never invented.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "p4race.h"
#include "tc_generated.h"

LV_FONT_DECLARE(geist_mono_22);    // page title (deck heading voice)
LV_FONT_DECLARE(geist_mono_384);   // giant gear (digits + N + R; page-local subset)
LV_FONT_DECLARE(geist_mono_slash_96); // position hero number (digits; page-local)
LV_FONT_DECLARE(geist_mono_slash_48); // the de-emphasised "/13" tail (half size)

namespace {

// ---- palette (house vocabulary) -------------------------------------------------
constexpr uint32_t kBg     = 0x0c0d10;
constexpr uint32_t kInk    = 0xf1f2f5;
constexpr uint32_t kDim    = 0x8a919c;
constexpr uint32_t kFaint  = 0x5a5d63;
constexpr uint32_t kGold   = 0xffd60a;   // P1
constexpr uint32_t kBronze = 0xE0A24E;   // podium
constexpr uint32_t kLedOff = 0x1c1f24;   // unlit lamp well
constexpr uint32_t kGreen  = 0x30d158;
constexpr uint32_t kYellow = 0xffd60a;
constexpr uint32_t kRed    = 0xff453a;
constexpr uint32_t kBlue   = 0x0a84ff;   // shift flash — the same blue as the blue flag
constexpr uint32_t kCyan   = 0x22d3ee;   // TC intervention

// The FULL canonical flag vocabulary (recorder FlagState): the border colour
// IS the message, so every value a sim can produce needs its own colour.
// Checkered and black borrow visible stand-ins (true black/chequer vanish on
// the dark page). The fallback for unrecognised strings is PURPLE — no real
// racing flag is purple, so an unmapped value reads as "odd state, look into
// it" instead of impersonating a yellow (an amber fallback did exactly that
// when a mis-mapped LMU blue arrived as "black").
struct FlagStyle { const char* name; uint32_t border; };
const FlagStyle kFlags[] = {
  { "green",     0x1f9d55 },
  { "yellow",    0xffd60a },
  { "blue",      0x0a84ff },
  { "white",     0xf1f2f5 },
  { "checkered", 0xf1f2f5 },
  { "red",       0xff453a },
  { "black",     0x8a919c },   // DQ — grey stand-in, visible on the dark page
  { "penalty",   0xff9f0a },   // drive-through / stop-go
  { "orange",    0xff9f0a },   // meatball (mechanical)
  { "unknown",   0xbf5af2 },
};
constexpr uint32_t kFlagUnknown = 0xbf5af2;

// ---- geometry (1280x720) --------------------------------------------------------
constexpr int kW = 1280, kH = 720;
constexpr int kBorderW = 24;                  // the flag frame

// RPM bar: 8 LEDs per bank, mirrored, meeting at the centre gap.
constexpr int kLedN    = 8;                   // per bank
constexpr int kLedD    = 40;                  // lamp diameter
constexpr int kLedGap  = 12;                  // between lamps
constexpr int kLedY    = 66;                  // row centreline
constexpr int kMidGap  = 72;                  // centre gap between the banks
// Slot colours, OUTER -> INNER (per the sketch): green x3, yellow x2, red x3.
const uint32_t kLedCol[kLedN] = { kGreen, kGreen, kGreen, kYellow, kYellow, kRed, kRed, kRed };
// The blue flash arms at the sim's RECOMMENDED upshift rpm (rpm_shift — the
// host falls back to ~96% of rpm_max when a sim lacks it), NOT at a fraction
// of rpm_max: the hard limiter often sits below 93% of the reported ceiling,
// which left the flash unreachable and the top lamps flickering in their own
// colours as the limiter bounced revs across their thresholds. The fill
// window rescales to the flash point, so the bar completes exactly as the
// blue hits: first lamp at 65% of the shift rpm, the last at ~96% of it.
constexpr float kFillLo = 0.65f;              // first lamp, as a fraction of the shift rpm
constexpr float kFlashFallback = 0.93f;       // of rpm_max, when rpm_shift is absent
constexpr uint32_t kFlashHalfMs = 170;        // blue pulse half-period (~3 Hz — a
                                              // deliberate strobe, not a flicker)
// The limiter bounces revs a few percent around the shift point many times a
// second; without a latch that flips the whole bar between BLUE and the slot
// colours every tick — a glitchy shimmer. Hysteresis: arm at the shift rpm,
// release only when revs fall clearly below it.
constexpr float kFlashRelease = 0.965f;       // disarm below this fraction of shift

// TC lamps: 4 down each edge.
constexpr int kTcN = 4;
constexpr int kTcD = 36;
constexpr int kTcX = 96;                      // left column centre (right mirrors)
constexpr int kTcY0 = 210, kTcDy = 62;
constexpr uint32_t kTcHalfMs = 90;

// ---- widgets + caches -----------------------------------------------------------
lv_obj_t* s_frame  = nullptr;
lv_obj_t* s_gear   = nullptr;
lv_obj_t* s_posVal = nullptr;
lv_obj_t* s_posSub = nullptr;   // the "/13" tail — half size, half opacity
void posLayout();               // defined after the helpers (measures + centres the pair)
lv_obj_t* s_rpmLed[2][kLedN];                 // [0]=left bank (outer->inner), [1]=right
lv_obj_t* s_rpmMid = nullptr;                 // the single centre lamp — the last to light
lv_obj_t* s_tcLed[2][kTcN];

int  s_posShown = INT32_MIN, s_ofShown = INT32_MIN;
char s_gearShown[8]  = { 1, 0 };
char s_flagShown[16] = { 1, 0 };
uint8_t s_rpmState[kLedN];                    // per-slot: 0 off, 1 colour, 2 blue, 3 blue-off
uint8_t s_midState = 0xFF;
uint32_t s_flashArmMs = 0;                    // 0 = blue flash not armed (latch + phase anchor)
bool s_tcShown[kTcN * 2 + 1];                 // last painted lamp states (+1: init marker)
uint32_t s_tickMs = 0;
constexpr uint32_t kTickMs = 33;              // ~30 Hz — lights need to feel live

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

lv_obj_t* mkLamp(lv_obj_t* p, int d) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_bg_color(o, hex(kLedOff), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, uint32_t col, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hex(col), 0);
  lv_label_set_text(l, t);
  lv_obj_set_width(l, kW);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  return l;
}


// Measure the position pair and centre it as a group at the layout's position
// row. The tail bottom-aligns to the hero's baseline region (hero line 83px,
// tail line 41px), so "/13" reads as a subscript of the number, not a peer.
constexpr int kPosY = 533, kPosGap = 10;
void posLayoutImpl(lv_obj_t* hero, lv_obj_t* tail) {
  lv_obj_update_layout(hero);
  lv_obj_update_layout(tail);
  const int w1 = lv_obj_get_width(hero);
  const int w2 = lv_obj_get_width(tail);
  const int total = w2 > 0 ? w1 + kPosGap + w2 : w1;
  const int x0 = (kW - total) / 2;
  lv_obj_set_pos(hero, x0, kPosY);
  lv_obj_set_pos(tail, x0 + w1 + kPosGap, kPosY + (83 - 41) - 4);
}

void posLayout() { posLayoutImpl(s_posVal, s_posSub); }

} // namespace

lv_obj_t* p4race_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // RPM banks. Left bank runs outer(left) -> inner(right); the right bank
  // mirrors it, so the two red trios meet across the centre gap.
  const int bankW = kLedN * kLedD + (kLedN - 1) * kLedGap;
  const int leftX0  = kW / 2 - kMidGap / 2 - bankW;          // left bank leftmost
  const int rightX0 = kW / 2 + kMidGap / 2;                  // right bank leftmost
  for (int i = 0; i < kLedN; i++) {
    // Left bank: slot i (outer->inner) sits left-to-right.
    s_rpmLed[0][i] = mkLamp(scr, kLedD);
    lv_obj_set_pos(s_rpmLed[0][i], leftX0 + i * (kLedD + kLedGap), kLedY - kLedD / 2);
    // Right bank: slot i (outer->inner) sits right-to-left.
    s_rpmLed[1][i] = mkLamp(scr, kLedD);
    lv_obj_set_pos(s_rpmLed[1][i],
                   rightX0 + (kLedN - 1 - i) * (kLedD + kLedGap), kLedY - kLedD / 2);
  }

  // The centre lamp — a single red light in the gap where the banks meet,
  // the very last to come on before the blue flash.
  s_rpmMid = mkLamp(scr, kLedD);
  lv_obj_set_pos(s_rpmMid, kW / 2 - kLedD / 2, kLedY - kLedD / 2);

  // TC lamp columns, one per edge.
  for (int i = 0; i < kTcN; i++) {
    s_tcLed[0][i] = mkLamp(scr, kTcD);
    lv_obj_set_pos(s_tcLed[0][i], kTcX - kTcD / 2, kTcY0 + i * kTcDy);
    s_tcLed[1][i] = mkLamp(scr, kTcD);
    lv_obj_set_pos(s_tcLed[1][i], kW - kTcX - kTcD / 2, kTcY0 + i * kTcDy);
  }

  // Gear — the hero. 179px line -> 150..329.
  // Vertical rhythm: the content zone under the lamps (row bottom 86) down to
  // the frame interior (696) holds gear (287px) + position (83px) with the
  // remaining space split into three equal ~80px bands — above, between,
  // below — so the pair floats centred instead of hugging the frame.
  s_gear = mkLabel(scr, &geist_mono_384, kFaint, "-");
  lv_obj_set_pos(s_gear, 0, 166);      // 287px line -> 166..453

  // Position — "3/13" with the hero number full-size and the "/13" tail at
  // half size + half opacity. The pair is measured and centred as a group
  // in posLayout() whenever the text changes.
  s_posVal = lv_label_create(scr);
  lv_obj_set_style_text_font(s_posVal, &geist_mono_slash_96, 0);
  lv_obj_set_style_text_color(s_posVal, hex(kFaint), 0);
  lv_label_set_text(s_posVal, "--");
  s_posSub = lv_label_create(scr);
  lv_obj_set_style_text_font(s_posSub, &geist_mono_slash_48, 0);
  lv_obj_set_style_text_color(s_posSub, hex(kDim), 0);
  lv_obj_set_style_opa(s_posSub, LV_OPA_50, 0);
  lv_label_set_text(s_posSub, "");
  posLayout();

  // The flag frame — LAST, so it rides over everything at the screen edge.
  s_frame = lv_obj_create(scr);
  lv_obj_remove_style_all(s_frame);
  lv_obj_set_size(s_frame, kW, kH);
  lv_obj_set_pos(s_frame, 0, 0);
  lv_obj_set_style_border_width(s_frame, kBorderW, 0);
  lv_obj_set_style_border_color(s_frame, hex(kFlagUnknown), 0);
  lv_obj_set_style_radius(s_frame, 32, 0);   // rounded frame corners, per the sketch
  lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_frame, LV_OBJ_FLAG_HIDDEN);

  memset(s_rpmState, 0xFF, sizeof(s_rpmState));   // force first paint
  memset(s_tcShown, 0xFF, sizeof(s_tcShown));
  return scr;
}

void p4race_tick(bool waiting) {
  const uint32_t now = millis();
  if (now - s_tickMs < kTickMs) return;
  s_tickMs = now;

  // ---- RPM bar ---------------------------------------------------------------------
  const int rpm      = waiting ? 0 : (int)tcStore.getInt(TC_CH_RPM);
  const int rpmMax   = waiting ? 0 : (int)tcStore.getInt(TC_CH_RPM_MAX);
  const int rpmShift = waiting ? 0 : (int)tcStore.getInt(TC_CH_RPM_SHIFT);
  const float flashRpm = rpmShift > 0 ? (float)rpmShift
                       : rpmMax > 0   ? (float)rpmMax * kFlashFallback : 0.0f;
  const float frac = (flashRpm > 0.0f && rpm > 0) ? (float)rpm / flashRpm : 0.0f;
  // Latch with hysteresis so the limiter's rpm bounce can't flicker the mode,
  // and anchor the pulse phase to the arm instant so it always opens BRIGHT.
  if (flashRpm > 0.0f && frac >= 1.0f) { if (!s_flashArmMs) s_flashArmMs = now; }
  else if (s_flashArmMs && frac < kFlashRelease) s_flashArmMs = 0;
  const bool flash = s_flashArmMs != 0;
  const bool flashOn = flash && (((now - s_flashArmMs) / kFlashHalfMs) & 1u) == 0;
  for (int i = 0; i < kLedN; i++) {
    // Slot i lights once revs cross its share of the 65%..96% (of shift) window.
    const float thr = kFillLo + (0.96f - kFillLo) * (float)i / (float)kLedN;
    uint8_t st;
    if (flash)          st = flashOn ? 2 : 3;   // whole bar flashes blue
    else if (frac >= thr) st = 1;
    else                st = 0;
    if (st != s_rpmState[i]) {
      s_rpmState[i] = st;
      const uint32_t col = st == 2 ? kBlue : st == 1 ? kLedCol[i] : kLedOff;
      lv_obj_set_style_bg_color(s_rpmLed[0][i], hex(col), 0);
      lv_obj_set_style_bg_color(s_rpmLed[1][i], hex(col), 0);
    }
  }

  // The centre lamp lights just under the shift rpm — the "last light" — and
  // joins the blue flash with everything else.
  {
    uint8_t st;
    if (flash)               st = flashOn ? 2 : 3;
    else if (frac >= 0.985f) st = 1;
    else                     st = 0;
    if (st != s_midState) {
      s_midState = st;
      lv_obj_set_style_bg_color(s_rpmMid,
          hex(st == 2 ? kBlue : st == 1 ? kRed : kLedOff), 0);
    }
  }

  // ---- TC lamps ---------------------------------------------------------------------
  const bool tc = !waiting && (tcStore.getBool(TC_CH_TC_ACTIVE) || tcStore.getBool(TC_CH_TC_PULSED));
  static uint32_t tcLastMs = 0;
  if (tc) tcLastMs = now;
  if (waiting) tcLastMs = 0;
  const bool tcOn = tc || (tcLastMs != 0 && now - tcLastMs < 150);
  if ((uint8_t)tcOn != s_tcShown[0] || s_tcShown[kTcN * 2] == 0xFF) {
    s_tcShown[0] = (uint8_t)tcOn;
    s_tcShown[kTcN * 2] = 1;
    const uint32_t col = tcOn ? kCyan : kLedOff;
    for (int i = 0; i < kTcN; i++) {
      lv_obj_set_style_bg_color(s_tcLed[0][i], hex(col), 0);
      lv_obj_set_style_bg_color(s_tcLed[1][i], hex(col), 0);
    }
  }

  // ---- the flag frame ---------------------------------------------------------------
  const char* flag = waiting ? "" : tcStore.getStr(TC_CH_FLAG);
  if (strncmp(flag, s_flagShown, sizeof(s_flagShown) - 1) != 0) {
    strncpy(s_flagShown, flag, sizeof(s_flagShown) - 1);
    s_flagShown[sizeof(s_flagShown) - 1] = '\0';
    if (flag[0] == '\0') {
      lv_obj_add_flag(s_frame, LV_OBJ_FLAG_HIDDEN);
    } else {
      uint32_t col = kFlagUnknown;
      for (const FlagStyle& f : kFlags) if (strcmp(flag, f.name) == 0) { col = f.border; break; }
      lv_obj_set_style_border_color(s_frame, hex(col), 0);
      lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // ---- gear -------------------------------------------------------------------------
  const char* gear = waiting ? "-" : tcStore.getStr(TC_CH_GEAR);
  if (gear[0] == '\0') gear = "-";
  if (strncmp(gear, s_gearShown, sizeof(s_gearShown) - 1) != 0) {
    strncpy(s_gearShown, gear, sizeof(s_gearShown) - 1);
    s_gearShown[sizeof(s_gearShown) - 1] = '\0';
    lv_label_set_text(s_gear, gear);
    lv_obj_set_style_text_color(s_gear, hex(waiting || gear[0] == '-' ? kFaint : kInk), 0);
  }

  // ---- position: class first, overall fallback --------------------------------------
  const int clsPos = waiting ? 0 : (int)tcStore.getInt(TC_CH_CLASS_POSITION);
  const int ovrPos = waiting ? 0 : (int)tcStore.getInt(TC_CH_POSITION);
  const int pos    = clsPos > 0 ? clsPos : ovrPos;
  const int clsTot = waiting ? 0 : (int)tcStore.getInt(TC_CH_CLASS_POSITIONS_TOTAL);
  const int ovrTot = waiting ? 0 : (int)tcStore.getInt(TC_CH_POSITIONS_TOTAL);
  const int tot    = clsPos > 0 ? clsTot : ovrTot;
  if (pos != s_posShown || tot != s_ofShown) {
    s_posShown = pos; s_ofShown = tot;
    char big[8], tail[12];
    if (pos > 0) snprintf(big, sizeof(big), "%d", pos);
    else         snprintf(big, sizeof(big), "--");
    if (pos > 0 && tot > 0) snprintf(tail, sizeof(tail), "/%d", tot);
    else                    tail[0] = ' ';
    lv_label_set_text(s_posVal, big);
    lv_label_set_text(s_posSub, tail);
    lv_obj_set_style_text_color(s_posVal,
        hex(pos == 1 ? kGold : (pos > 0 && pos <= 3) ? kBronze : pos > 0 ? kInk : kFaint), 0);
    posLayout();
  }
}
