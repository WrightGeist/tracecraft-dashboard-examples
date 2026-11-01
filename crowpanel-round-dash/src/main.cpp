/**
 * TraceCraft round dash (Elecrow CrowPanel 1.28" round, 240x240, rotary encoder):
 * a dual-arc setting tweaker for brake bias and engine map, with tell-tales
 * across the top. Pure firmware, no EEZ.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/crowpanel_round.h"
#include "tc_generated.h"   // channel registry

using namespace tracecraft;

#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft Round Dash"
#endif

static App app;

#include "icons/telltale_icons.h"   // A8 blinker/headlight/wiper icons
// --- Round dual-arc setting tweaker (CrowPanel 1.28" GC9A01, 240x240) -------
// Two gauges flank screen center: brake bias on the left, engine map on the
// right (a notch per step). The rotary encoder nudges the ACTIVE gauge; the
// center push-button (the screen itself is the button) toggles which gauge is
// active. The knob is LOCAL ONLY — we can't write back to the sim — so each
// gauge tracks live telemetry until you touch the knob, then holds your value.
// The active gauge's caption lights up; the other dims. Pure firmware, no EEZ.
//
// A row of tell-tales sits across the top of the dial — turn signals (firmware-
// blinked, since sims report a steady "engaged" flag), headlight stage (low
// green / high blue), and wipers — recolorable A8 icons dim when idle.

// Encoder wiring on the CrowPanel header — adjust to match your solder:
static constexpr int kEncA  = 45;    // rotary A  (CLK)
static constexpr int kEncB  = 42;    // rotary B  (DT)
static constexpr int kEncSW = 41;    // center push-button

static constexpr float kBiasMin = 45.0f, kBiasMax = 75.0f;  // % toward front axle
static constexpr int   kMapMin  = 1,     kMapMax  = 12;      // engine-map steps
static constexpr int   kMapNotches = kMapMax - kMapMin + 1;  // 12
// Gauges are sized + offset so they DON'T touch at screen center: the MAP arc's
// notch-dot ring extends ~6px beyond the arc all the way around, so tangent
// gauges let the map dots (and the arc band itself) cross into the BIAS arc.
// 92px gauges 56px off-center leave an ~11px gap — the dot ring clears cleanly.
static constexpr int   kArcSz   = 92;                        // gauge diameter (px)
static constexpr int   kArcOff  = 56;                        // |x| from center per gauge

static lv_obj_t* s_biasArc = nullptr;
static lv_obj_t* s_mapArc  = nullptr;
static lv_obj_t* s_biasVal = nullptr;
static lv_obj_t* s_mapVal  = nullptr;
static lv_obj_t* s_biasCap = nullptr;
static lv_obj_t* s_mapCap  = nullptr;
static lv_obj_t* s_mapDots[kMapNotches] = { nullptr };

static int   s_active     = 0;       // 0 = brake bias, 1 = engine map
static bool  s_biasManual = false;   // true once the knob overrides live data
static bool  s_mapManual  = false;
static float s_biasLocal  = 60.0f;   // currently shown bias (live or overridden)
static int   s_mapLocal   = 6;

static uint8_t  s_swPrev   = 1;
static uint32_t s_swLastMs = 0;

// --- tell-tale row (turn signals / headlights / wipers) --------------------------
static lv_obj_t* s_ttTurnL  = nullptr;
static lv_obj_t* s_ttTurnR  = nullptr;
static lv_obj_t* s_ttLights = nullptr;
static lv_obj_t* s_ttWipers = nullptr;
static uint32_t  s_ttLtMs = 0, s_ttRtMs = 0;   // turn-signal engaged latches (firmware blink)
static uint8_t   s_ttLs = 0xFF, s_ttRs = 0xFF, s_ttHls = 0xFF, s_ttWps = 0xFF;
static constexpr uint32_t kCpTtDim = 0x2a2d35;   // idle icon color

static lv_obj_t* cpMakeTell(lv_obj_t* scr, const lv_image_dsc_t* src, int xoff, int yoff) {
  lv_obj_t* o = lv_image_create(scr);
  lv_image_set_src(o, src);
  lv_image_set_scale(o, 190);                    // ~74% of the baked icon
  lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(o, lv_color_hex(kCpTtDim), 0);
  lv_obj_align(o, LV_ALIGN_CENTER, xoff, yoff);
  return o;
}

static void cpTellLamp(lv_obj_t* ic, uint8_t& shown, uint8_t st, uint32_t lit) {
  if (st == shown) return;
  shown = st;
  lv_obj_set_style_image_recolor(ic, lv_color_hex(st ? lit : kCpTtDim), 0);
}

// Firmware-driven blink (sims send a steady "engaged" flag): latch engaged for a
// hold window, toggle on a fixed cadence, both sides sharing one phase.
static void cpTellBlink(lv_obj_t* ic, bool raw, uint32_t& lastTrue, uint8_t& shown, uint32_t lit) {
  const uint32_t now = millis();
  if (raw) lastTrue = now;
  const bool engaged = lastTrue != 0 && (now - lastTrue) < 700;
  cpTellLamp(ic, shown, (engaged && (now / 330) % 2 == 0) ? 1 : 0, lit);
}

// Bounce-tolerant quadrature decoder, driven by pin-change INTERRUPTS (not polled).
// LVGL rendering blocks loop() for tens of ms, so polling once per frame misses the
// intermediate gray-code states of a fast turn and the full-sequence check never
// completes (every detent gets dropped). Running the decoder in an ISR on every A/B
// edge means we never miss a transition. Each completed step accumulates into
// s_encDelta; tickCrowPanelUI drains it (so several detents in one frame all count).
// The table rejects illegal (bounce) transitions, so noise can't fake a step.
// If your encoder counts backwards, swap the kEncA/kEncB reads below.
static volatile int16_t s_encDelta = 0;

static void IRAM_ATTR encISR() {
  static const int8_t kValid[16] = { 0,1,1,0, 1,0,0,1, 1,0,0,1, 0,1,1,0 };
  static uint8_t prevNext = 0;
  static uint16_t store   = 0;
  prevNext <<= 2;
  if (digitalRead(kEncB)) prevNext |= 0x02;
  if (digitalRead(kEncA)) prevNext |= 0x01;
  prevNext &= 0x0f;
  if (kValid[prevNext]) {
    store = (store << 4) | prevNext;
    if ((store & 0xff) == 0x2b)      s_encDelta--;
    else if ((store & 0xff) == 0x17) s_encDelta++;
  }
}

static const lv_color_t kBiasCol = LV_COLOR_MAKE(0x81, 0x8c, 0xf8);  // indigo
static const lv_color_t kMapCol  = LV_COLOR_MAKE(0xfb, 0x92, 0x3c);  // orange

static lv_obj_t* makeArc(lv_obj_t* scr, int xoff, lv_color_t col) {
  lv_obj_t* a = lv_arc_create(scr);
  lv_obj_set_size(a, kArcSz, kArcSz);
  lv_obj_align(a, LV_ALIGN_CENTER, xoff, 0);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_arc_set_range(a, 0, 1000);                  // we scale our own range into 0..1000
  lv_arc_set_value(a, 0);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);  // value is set in code, not dragged
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);    // hide the drag knob entirely
  lv_obj_set_style_arc_width(a, 9, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 9, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, lv_color_hex(0x2b2b2b), LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, col, LV_PART_INDICATOR);
  return a;
}

static lv_obj_t* makeValLabel(lv_obj_t* scr, int xoff) {
  lv_obj_t* l = lv_label_create(scr);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  LV_FONT_DECLARE(geist_sans_24);
  lv_obj_set_style_text_font(l, &geist_sans_24, 0);
  lv_label_set_text(l, "--");
  lv_obj_align(l, LV_ALIGN_CENTER, xoff, -4);
  return l;
}

static lv_obj_t* makeCaption(lv_obj_t* scr, int xoff, const char* txt) {
  lv_obj_t* l = lv_label_create(scr);
  LV_FONT_DECLARE(geist_sans_14);
  lv_obj_set_style_text_font(l, &geist_sans_14, 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, LV_ALIGN_CENTER, xoff, 42);
  return l;
}

static void setupCrowPanelUI() {
  pinMode(kEncA,  INPUT_PULLUP);
  pinMode(kEncB,  INPUT_PULLUP);
  pinMode(kEncSW, INPUT_PULLUP);
  s_swPrev   = digitalRead(kEncSW);
  attachInterrupt(digitalPinToInterrupt(kEncA), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(kEncB), encISR, CHANGE);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  s_biasArc = makeArc(scr, -kArcOff, kBiasCol);
  s_mapArc  = makeArc(scr,  kArcOff, kMapCol);
  s_biasVal = makeValLabel(scr, -kArcOff);
  s_mapVal  = makeValLabel(scr,  kArcOff);
  s_biasCap = makeCaption(scr, -kArcOff, "BIAS");
  s_mapCap  = makeCaption(scr,  kArcOff, "MAP");

  // Engine-map notch dots, one per step, around the map arc's outer edge. The
  // dot for the current step lights up; the rest sit dim. Center of the map arc
  // is the screen center shifted right by kArcOff.
  const int mcx = 120 + kArcOff;
  const int mcy = 120;
  const float r = kArcSz / 2.0f + 6.0f;          // just outside the arc band
  for (int i = 0; i < kMapNotches; i++) {
    const float frac = (float)i / (kMapNotches - 1);
    const float ang  = (135.0f + 270.0f * frac) * 0.01745329f;  // deg -> rad
    const int x = mcx + (int)(r * cosf(ang));
    const int y = mcy + (int)(r * sinf(ang));
    lv_obj_t* d = lv_obj_create(scr);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 4, 4);
    lv_obj_set_style_radius(d, 2, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(0x555555), 0);
    lv_obj_set_pos(d, x - 2, y - 2);
    s_mapDots[i] = d;
  }

  // Tell-tales: turn signals flank the TOP (pulled in from the edge), headlights
  // + wipers sit along the BOTTOM, below the arcs.
  s_ttTurnL  = cpMakeTell(scr, &tc_icon_blinker_left,  -46, -78);
  s_ttTurnR  = cpMakeTell(scr, &tc_icon_blinker_right,  46, -78);
  s_ttLights = cpMakeTell(scr, &tc_icon_headlights,    -30,  74);
  s_ttWipers = cpMakeTell(scr, &tc_icon_wipers,         30,  74);
}

static void tickCrowPanelUI() {
  // --- input: rotary encoder + center button (active-low, INPUT_PULLUP) -------
  noInterrupts();
  const int dir = s_encDelta;                    // net detents since last frame (ISR-fed)
  s_encDelta = 0;
  interrupts();

  const uint8_t sw = digitalRead(kEncSW);
  const uint32_t nowMs = millis();
  if (s_swPrev == 1 && sw == 0 && (nowMs - s_swLastMs) > 200) {
    s_active = s_active ? 0 : 1;                  // toggle which gauge the knob drives
    s_swLastMs = nowMs;
  }
  s_swPrev = sw;

  if (dir != 0) {
    if (s_active == 0) {
      s_biasManual = true;
      s_biasLocal += dir * 0.5f;                  // 0.5 % front per detent
      s_biasLocal = fminf(kBiasMax, fmaxf(kBiasMin, s_biasLocal));
    } else {
      s_mapManual = true;
      s_mapLocal += dir;
      if (s_mapLocal < kMapMin) s_mapLocal = kMapMin;
      if (s_mapLocal > kMapMax) s_mapLocal = kMapMax;
    }
  }

  // --- live telemetry tracks the sim until the knob takes over ---------------
  const float liveBias = tcStore.getFloat(TC_CH_BRAKE_BIAS_PCT);
  const int   liveMap  = (int)tcStore.getInt(TC_CH_ENGINE_MAP);
  if (!s_biasManual && liveBias > 1.0f) s_biasLocal = liveBias;     // 0 == no data
  if (!s_mapManual  && liveMap >= kMapMin) s_mapLocal = liveMap;

  // --- render -----------------------------------------------------------------
  const int biasArc = (int)((s_biasLocal - kBiasMin) / (kBiasMax - kBiasMin) * 1000.0f);
  const int mapArc  = (int)((float)(s_mapLocal - kMapMin) / (kMapMax - kMapMin) * 1000.0f);
  lv_arc_set_value(s_biasArc, biasArc < 0 ? 0 : biasArc > 1000 ? 1000 : biasArc);
  lv_arc_set_value(s_mapArc,  mapArc  < 0 ? 0 : mapArc  > 1000 ? 1000 : mapArc);

  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f", (double)s_biasLocal);
  lv_label_set_text(s_biasVal, buf);
  snprintf(buf, sizeof(buf), "%d", s_mapLocal);
  lv_label_set_text(s_mapVal, buf);

  // active gauge: caption lit in its color; inactive: dim gray
  lv_obj_set_style_text_color(s_biasCap, s_active == 0 ? kBiasCol : lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_color(s_mapCap,  s_active == 1 ? kMapCol  : lv_color_hex(0x666666), 0);
  lv_obj_set_style_arc_opa(s_biasArc, s_active == 0 ? LV_OPA_COVER : LV_OPA_50, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(s_mapArc,  s_active == 1 ? LV_OPA_COVER : LV_OPA_50, LV_PART_INDICATOR);

  // light the notch dot for the current engine-map step
  const int lit = s_mapLocal - kMapMin;
  for (int i = 0; i < kMapNotches; i++) {
    if (!s_mapDots[i]) continue;
    lv_obj_set_style_bg_color(s_mapDots[i],
        i == lit ? kMapCol : lv_color_hex(0x555555), 0);
  }

  // --- tell-tales: blinkers (firmware blink), headlight stage, wipers ----------
  cpTellBlink(s_ttTurnL, tcStore.getBool(TC_CH_TURN_LEFT),  s_ttLtMs, s_ttLs, 0x35D07F);
  cpTellBlink(s_ttTurnR, tcStore.getBool(TC_CH_TURN_RIGHT), s_ttRtMs, s_ttRs, 0x35D07F);
  int hs = (int)tcStore.getInt(TC_CH_HEADLIGHTS);
  hs = hs < 0 ? 0 : hs > 2 ? 2 : hs;               // 0 off / 1 low / 2+ high
  if ((uint8_t)hs != s_ttHls) {
    s_ttHls = (uint8_t)hs;
    const uint32_t c = hs == 0 ? kCpTtDim : hs == 1 ? 0x35D07F : 0x4C9AFF;
    lv_obj_set_style_image_recolor(s_ttLights, lv_color_hex(c), 0);
  }
  cpTellLamp(s_ttWipers, s_ttWps, tcStore.getInt(TC_CH_WIPERS) > 0 ? 1 : 0, 0x22D3EE);
}

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  setupCrowPanelUI();
  app.ready();
}

void loop() {
  app.loop();
  tickCrowPanelUI();            // poll encoder/button + redraw the two gauges
}
