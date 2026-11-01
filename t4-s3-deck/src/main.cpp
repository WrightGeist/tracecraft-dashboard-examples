/**
 * TraceCraft T4-S3 deck (Lilygo T4-S3 AMOLED, 450x600): four pages, swipe to move
 * between them — the EEZ dashboard, a tires-and-brakes diagnostic, host-PC
 * performance traces, and a big sector-number badge.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/t4_s3.h"
#include "tc_generated.h"         // channel registry + EEZ get_var_* glue
#include "ui/ui.h"                // the 450x600 EEZ dashboard (run EEZ Build first)
#include "ui_t4tires/t4tires.h"   // per-corner tires + brakes diagnostic (deck page)
#include "ui_t4perf/t4perf.h"     // FPS + host CPU/RAM/GPU/VRAM traces (deck page)

using namespace tracecraft;

#include "eez_effects/eez_effects.h"

#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft Dash"
#endif

static App app;

// --- Big sector-number badge (a T4 deck page) -------------------------------
// One large circular badge in the style of the recorder's Windows driver dashboard
// (its sector tile): a disc filled with the web app's sector
// color, a big dark number, and a BRIGHT white flash on every sector change. Reads
// the canonical `sector` when the sim publishes it (ACC + LMU), else derives the
// sector from lap_dist_pct thirds (AC EVO + iRacing) — the same fallback the app
// uses. Pure firmware, no EEZ.

// TraceCraft's sector palette. Cycles mod 4.
static const uint32_t kSectorColors[4] = { 0xd97c7c, 0x7aa3c8, 0xd4a05a, 0x7fb393 };
static constexpr uint32_t kSecFlashDur = 650;   // white-flash length on change (ms)
static constexpr uint32_t kSecDark     = 0x0a0a0b;  // dark number color (matches recorder)

static lv_obj_t* s_secDisc = nullptr;
static lv_obj_t* s_secNum  = nullptr;
static int       s_secIdx  = -1;                // last shown sector index (change detect)
static uint32_t  s_secFlashMs = 0;              // millis() when the current flash began
static uint32_t  s_secBaseShown = 0xFFFFFFFF;   // last applied disc color (change detect)

// Lerp two 0xRRGGBB colors (t: 0 -> a, 1 -> b).
static lv_color_t lerpHex(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0; if (t > 1) t = 1;
  const int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
  const int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
  return lv_color_make((uint8_t)(ar + (br - ar) * t),
                       (uint8_t)(ag + (bg - ag) * t),
                       (uint8_t)(ab + (bb - ab) * t));
}

static void setupSectorUI(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);   // AMOLED true black
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_update_layout(scr);
  const int W = lv_obj_get_width(scr);
  const int H = lv_obj_get_height(scr);
  int D = (W < H ? W : H) - 70;                 // disc diameter (room for the label above)
  if (D < 80) D = 80;

  s_secDisc = lv_obj_create(scr);
  lv_obj_remove_style_all(s_secDisc);
  // The 5x-scaled digit visually overflows the disc; with a pointer indev
  // present (T4 deck), the default SCROLLABLE flag would let drags scroll
  // the number around inside the disc. It's a badge, not a list.
  lv_obj_remove_flag(s_secDisc, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_secDisc, D, D);
  lv_obj_set_style_radius(s_secDisc, D / 2, 0);
  lv_obj_set_style_bg_opa(s_secDisc, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_secDisc, lv_color_hex(0x1c1c1e), 0);   // neutral until data
  lv_obj_set_style_border_width(s_secDisc, 4, 0);
  lv_obj_set_style_border_color(s_secDisc, lv_color_hex(0x3a3a3c), 0);
  lv_obj_align(s_secDisc, LV_ALIGN_CENTER, 0, 20);   // nudged down so the label above balances

  // "SECTOR" caption ABOVE the circle. It sits on the black background now, so it's
  // light-colored (a dark caption would vanish); tracked-out to read as a header.
  lv_obj_t* cap = lv_label_create(scr);
  LV_FONT_DECLARE(geist_sans_28);
  lv_obj_set_style_text_font(cap, &geist_sans_28, 0);
  lv_obj_set_style_text_color(cap, lv_color_hex(0xc8c8c8), 0);
  lv_obj_set_style_text_letter_space(cap, 6, 0);
  lv_label_set_text(cap, "SECTOR");
  lv_obj_align_to(cap, s_secDisc, LV_ALIGN_OUT_TOP_MID, 3, -14);

  // Big central number: the 240px Geist Mono digit face at a light 1.5x scale
  // (~268px visual, matching the old Montserrat-48-at-5.1x) — a real font at
  // display size keeps the sw transform blit small (AGENTS rule 3). Mono digits
  // are fixed-width, so the center-aligned label stays centered on any numeral.
  s_secNum = lv_label_create(s_secDisc);
  lv_obj_set_width(s_secNum, D);
  lv_obj_set_style_text_align(s_secNum, LV_TEXT_ALIGN_CENTER, 0);
  LV_FONT_DECLARE(geist_mono_240);
  lv_obj_set_style_text_font(s_secNum, &geist_mono_240, 0);
  lv_obj_set_style_text_color(s_secNum, lv_color_hex(kSecDark), 0);
  lv_label_set_text(s_secNum, "-");
  lv_obj_update_layout(s_secNum);
  // NO transform scale here, deliberately. A transformed object forces LVGL to
  // allocate an intermediate layer covering its whole area, and while normal
  // rendering does that per render BAND (cheap), the deck's page transition
  // snapshots the entire screen in ONE pass -- so the layer is the full scaled
  // glyph, ~360x360, which at ARGB8888 is ~518 KB against 327 KB of internal DRAM
  // on this chip. It can never be satisfied, and LVGL's allocation assert spins in
  // while(1), so the symptom was the whole deck freezing on the swipe INTO this
  // page while USB stayed enumerated. Raising LV_MEM_SIZE cannot fix it.
  //
  // The rule: scale only on small panels, and on a big
  // one compile a real font at display size. geist_mono_240 IS a real font at
  // display size; the 1.5x on top of it was the whole bug.
  lv_obj_align(s_secNum, LV_ALIGN_CENTER, 0, 0);
}

static void tickSectorUI() {
  // --- resolve the sector index (0-based) cross-sim ---
  int sc = (int)tcStore.getInt(TC_CH_SECTOR_COUNT);
  if (sc < 1) sc = 3;                                  // iRacing leaves it null -> assume 3
  const int s = (int)tcStore.getInt(TC_CH_SECTOR);
  const float pct = tcStore.getFloat(TC_CH_LAP_DIST_PCT);
  int idx = (s > 0) ? s                                // sim published a definite sector
                    : (int)(pct * sc);                 // fallback (covers sector 0 + EVO/iR)
  if (idx < 0) idx = 0;
  if (idx >= sc) idx = sc - 1;

  // Idle state until telemetry arrives: neutral disc + app.dash(). Painted ONCE on
  // the transition — an unconditional repaint here invalidated the huge
  // transform-scaled digit every loop and dragged the idle page to ~5 FPS
  // (sw transform blits are the priciest thing this chip renders).
  if (app.waiting()) {
    if (s_secIdx != -1) {
      lv_label_set_text(s_secNum, "-");
      lv_obj_set_style_bg_color(s_secDisc, lv_color_hex(0x1c1c1e), 0);
      lv_obj_set_style_border_color(s_secDisc, lv_color_hex(0x3a3a3c), 0);
      s_secIdx = -1;
      s_secBaseShown = 0xFFFFFFFF;
    }
    return;
  }

  // Change -> update the number and start a bright flash (but not on the first
  // resolve after connecting, which isn't really a "change").
  if (idx != s_secIdx) {
    if (s_secIdx >= 0) s_secFlashMs = millis();
    s_secIdx = idx;
    char b[8];
    snprintf(b, sizeof(b), "%d", idx + 1);            // display 1-based
    lv_label_set_text(s_secNum, b);
  }

  // Disc fill: the sector color, or a white flash decaying to it after a
  // change. Continuous repaints ONLY while the flash lerp is live; the
  // settled color is applied once per change (idle page = zero redraws).
  const uint32_t base = kSectorColors[((idx % 4) + 4) % 4];
  const uint32_t elapsed = millis() - s_secFlashMs;
  if (s_secFlashMs != 0 && elapsed < kSecFlashDur) {
    lv_obj_set_style_bg_color(s_secDisc, lerpHex(0xffffff, base, (float)elapsed / kSecFlashDur), 0);
    lv_obj_set_style_border_color(s_secDisc, lv_color_hex(base), 0);
    s_secBaseShown = 0xFFFFFFFF;            // force the settle paint after the flash
  } else if (base != s_secBaseShown) {
    s_secBaseShown = base;
    lv_obj_set_style_bg_color(s_secDisc, lv_color_hex(base), 0);
    lv_obj_set_style_border_color(s_secDisc, lv_color_hex(base), 0);
  }
}

// --- T4-S3 4-in-1 deck: EEZ dash / tires+brakes / performance / sector badge ---
// The P4 deck's shape at S3 scale: every page is its own lv screen, built once
// at boot and ticked every loop (change-detection keeps hidden pages cheap),
// paged by finger swipes on the CST226 touch panel. No blitter on this chip,
// so transitions use LVGL's MOVE screen animation — fine at 450x600 (it is the
// 1280px panel where per-frame tree redraws cost 40-80ms).
static constexpr uint8_t kT4PageN = 4;
static lv_obj_t* s_t4Pages[kT4PageN];
static uint8_t   s_t4Page = 0;
static volatile int8_t s_t4SwipeEvt = 0;   // set by the touch task: +1 next, -1 prev

// Touch sampling lives on its OWN 100 Hz task (the P4's lesson:
// nothing latency-sensitive lives in the render loop): loop-rate polling
// missed fast swipes whenever the active page rendered slowly — the sector
// page's 5x-scaled digit made back-swipes need a crawl to register. The
// task owns ALL runtime I2C on this board (CST226 touch; the PMU is only
// touched at init), keeping shared-bus traffic single-tasked. Release-
// classified swipe: track the stroke while down, decide on lift.
// Latest touch sample, published by the task for LVGL's pointer indev (the
// read_cb runs on the LVGL/loop context and must NOT touch the I2C bus).
static volatile int16_t s_t4TX = 0, s_t4TY = 0;
static volatile bool    s_t4TDown = false;

static void t4TouchTask(void*) {
  bool down = false;
  int16_t x0 = 0, y0 = 0, xl = 0, yl = 0;
  int peakDx = 0, peakDy = 0;          // furthest travel seen during the stroke
  for (;;) {
    int16_t x, y;
    if (board::touchPoint(x, y)) {
      if (!down) { down = true; x0 = x; y0 = y; peakDx = peakDy = 0; }
      xl = x; yl = y;
      // Classify on the FURTHEST point reached, not on where the finger happened
      // to be at the last poll before lifting. A flick decelerates and often drifts
      // back a few pixels at the end, and the final sample can land anywhere in the
      // 5 ms before release -- so last-minus-first systematically under-reports a
      // fast stroke, which is exactly the gesture that felt unreliable.
      const int dxNow = x - x0, dyNow = y - y0;
      if (abs(dxNow) > abs(peakDx)) peakDx = dxNow;
      if (abs(dyNow) > abs(peakDy)) peakDy = dyNow;
      s_t4TX = x; s_t4TY = y; s_t4TDown = true;
    } else if (down) {
      down = false;
      s_t4TDown = false;
      // 45px on a 450px panel is 10% of the width -- enough to stay clear of a tap
      // or a vertical scroll, low enough that a deliberate flick always clears it.
      // Was 60, which a quick stroke could miss entirely.
      if (abs(peakDx) >= 45 && abs(peakDx) >= 2 * abs(peakDy))
        s_t4SwipeEvt = (peakDx < 0) ? 1 : -1;        // left = next, right = prev
    }
    // 200 Hz, not 100. A fast flick lasts 60-80 ms, so at 10 ms polling it was only
    // 6-8 samples and a really quick one could begin and end between two polls --
    // invisible, no gesture at all. Halving the period halves that miss window; the
    // task does nothing but one I2C read, so the cost is negligible.
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// LVGL pointer device fed from the task's shared sample — gives any scrollable
// page native vertical scrolling (LVGL screens scroll by default, but only if
// an indev delivers the drags; today's three pages are all fixed-layout, so
// this mostly future-proofs the deck). Vertical drags scroll; horizontal
// swipes still page via the classifier (its 2:1 horizontal-dominance guard
// keeps the two gestures apart).
static void t4IndevRead(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  data->point.x = s_t4TX;
  data->point.y = s_t4TY;
  data->state = s_t4TDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Reveal-wipe transition, shaped around the display link's hard ceiling:
// this QSPI bus moves ~13 MB/s no matter what clock the board config asks
// for (GPIO-matrix routing pins the real clock at ~26.7 MHz — A/B-verified),
// so a full-screen push costs ~40 ms and any transition that re-sends
// moving content every frame (true slide: ~6 screenfuls; cover-slide:
// ~3) strobes somewhere. The wipe sends exactly ONE screenful total:
// the incoming page (rasterized ONCE into a PSRAM snapshot) sits at its
// final position and a seam sweeps across, exposing it strip by strip —
// each frame pushes only the newly revealed strip, a few ms regardless
// of progress, so the sweep stays smooth start to finish. Falls back to
// an instant swap if the snapshot fails.
static lv_draw_buf_t s_t4SnapIn;
static bool      s_t4SnapReady = false;
static uint16_t* s_t4Band = nullptr;      // internal compose band (W x kT4BandRows)
static int32_t   s_t4W = 0, s_t4H = 0;
static constexpr int kT4BandRows = 60;    // even (panel alignment) divisor of 600

// The wipe bypasses LVGL entirely: per frame, only the NEWLY revealed strip
// of the incoming snapshot is repacked into an internal band and pushed —
// everything already on the glass stays put and never re-sends. LVGL stays
// idle during the wipe (nothing invalidates), and app.dash().loop() runs between
// frames so telemetry never stalls.
static void t4SlideRun(uint8_t target, int8_t dir) {
  const uint16_t* in = (const uint16_t*)s_t4SnapIn.data;
  const int W = s_t4W, H = s_t4H;
  const uint32_t t0 = millis();
  const uint32_t dur = 240;
  AmoledRenderer& r = board::amoled();
  int revealed = 0;                     // incoming columns already on the glass
  for (;;) {
    const uint32_t el = millis() - t0;
    float k = el >= dur ? 1.0f : (float)el / dur;
    k = 1.0f - (1.0f - k) * (1.0f - k);                  // ease-out: sweep settles to rest
    const int v = (int)(k * W + 0.5f) & ~1;              // even columns (alignment 2)
    // NB: this skip must never bypass the exit — the loop terminates only
    // via the revealed >= W break below, which the final k=1 frame reaches
    // (v is monotonic in el and hits exactly W at k=1).
    if (v <= revealed) { app.dash().loop(); continue; }        // sub-pixel progress this pass
    const int dv = v - revealed;                         // even - even = even
    // New strip only. The incoming page sits at its FINAL screen position
    // (source column == screen column), so the strip needs no shifting.
    // dir>0 (next): seam sweeps right -> left; dir<0 (prev): left -> right.
    const int x0 = dir > 0 ? W - v : revealed;
    for (int y0 = 0; y0 < H; y0 += kT4BandRows) {
      for (int row = 0; row < kT4BandRows; row++) {
        memcpy(s_t4Band + (size_t)row * dv,
               in + (size_t)(y0 + row) * W + x0, (size_t)dv * 2);
      }
      // Snapshot was pre-swapped to panel byte order at wipe start, so
      // bands go straight out — no per-frame rgb565 swap.
      r.flushPreSwapped((uint16_t)x0, (uint16_t)y0, (uint16_t)dv, kT4BandRows, s_t4Band);
    }
    r.flushCommit();
    revealed = v;
    app.dash().loop();                                          // keep USB serviced
    if (revealed >= (W & ~1)) break;                      // fully revealed — done
  }
  s_t4Page = target;
  lv_screen_load(s_t4Pages[s_t4Page]);
  lv_obj_invalidate(s_t4Pages[s_t4Page]);                 // full repaint over our pixels
}

static void t4deckConsumeSwipe() {
  const int8_t evt = s_t4SwipeEvt;
  if (!evt) return;
  s_t4SwipeEvt = 0;
  const uint8_t target = (uint8_t)((s_t4Page + (evt > 0 ? 1 : kT4PageN - 1)) % kT4PageN);
#ifdef TRACECRAFT_TOUCH_DEBUG
  // The T4 had NO transition tracing: every [deck] line in this file sits inside
  // the P4DECK block, so TRACECRAFT_TOUCH_DEBUG compiled to nothing here and the
  // console stayed silent whether the board was healthy or wedged. That cost an
  // hour of flashing instruments that could not fire.
  usb::log().printf("[t4deck] swipe pg%d->pg%d (evt=%d)\n",
                    (int)s_t4Page, (int)target, (int)evt);
#endif

  if (!s_t4SnapReady) {                    // one-time buffers (snapshot + band)
    IRenderer& r = board::renderer();
    s_t4W = r.width();
    s_t4H = r.height();
    const uint32_t stride = (uint32_t)s_t4W * 2, size = stride * s_t4H;
    void* b = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    s_t4Band = (uint16_t*)heap_caps_malloc((size_t)s_t4W * kT4BandRows * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_t4Band) s_t4Band = (uint16_t*)heap_caps_malloc((size_t)s_t4W * kT4BandRows * 2, MALLOC_CAP_SPIRAM);
    if (!b || !s_t4Band) {
      usb::log().printf("[slide] buffer alloc FAILED (snap=%d band=%d) - instant swaps\n",
                        b != nullptr, s_t4Band != nullptr);
    } else if (lv_draw_buf_init(&s_t4SnapIn, s_t4W, s_t4H, LV_COLOR_FORMAT_RGB565, stride, b, size) != LV_RESULT_OK) {
      usb::log().printf("[slide] draw_buf init FAILED (w=%d h=%d stride=%u) - instant swaps\n",
                        (int)s_t4W, (int)s_t4H, (unsigned)stride);
    } else {
      s_t4SnapReady = true;
    }
#ifdef TRACECRAFT_TOUCH_DEBUG
    usb::log().printf("[t4deck] buffers ready=%d w=%d h=%d (w parity=%s)\n",
                      (int)s_t4SnapReady, (int)s_t4W, (int)s_t4H,
                      (s_t4W & 1) ? "ODD" : "even");
#endif
  }
  bool snapped = false;
  if (s_t4SnapReady) {
    // Only the INCOMING page needs rasterizing — the outgoing page is
    // already on the glass and stays put under the cover-slide.
#ifdef TRACECRAFT_TOUCH_DEBUG
    // The prime suspect. A full-screen snapshot renders the incoming page through
    // LVGL, which draws from the LV_MEM pool even though the destination buffer is
    // ours in PSRAM. If the pool cannot serve it, LVGL's malloc assert spins in
    // while(1) -- the main loop dies WITHOUT returning here, so the absence of the
    // matching "snapshot took" line below is itself the diagnosis.
    const uint32_t tSnap = millis();
    usb::log().printf("[t4deck] snapshot pg%d start\n", (int)target);
#endif
    const lv_result_t r1 = lv_snapshot_take_to_draw_buf(s_t4Pages[target], LV_COLOR_FORMAT_RGB565, &s_t4SnapIn);
    snapped = (r1 == LV_RESULT_OK);
#ifdef TRACECRAFT_TOUCH_DEBUG
    usb::log().printf("[t4deck] snapshot took %lums ok=%d\n",
                      (unsigned long)(millis() - tSnap), (int)snapped);
#endif
    if (!snapped)
      usb::log().printf("[slide] snapshot FAILED (%d) - instant swap\n", (int)r1);
    if (snapped) {
      // Swap the snapshot to panel byte order ONCE (~0.5 MB, a couple ms)
      // instead of re-swapping every composed band on every slide frame.
      lv_draw_sw_rgb565_swap(s_t4SnapIn.data, (uint32_t)s_t4W * s_t4H);
    }
  }
  if (!snapped) {                          // degraded: instant swap (logged above)
    s_t4Page = target;
    lv_screen_load(s_t4Pages[s_t4Page]);
    return;
  }
#ifdef TRACECRAFT_TOUCH_DEBUG
  const uint32_t tSlide = millis();
  usb::log().printf("[t4deck] slide start\n");
#endif
  t4SlideRun(target, evt > 0 ? 1 : -1);
#ifdef TRACECRAFT_TOUCH_DEBUG
  usb::log().printf("[t4deck] slide done %lums, now on pg%d\n",
                    (unsigned long)(millis() - tSlide), (int)s_t4Page);
#endif
}

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  ui_init();                          // builds + loads the EEZ screens
  tuneDashGauges();                   // fuel strip == arc-track thickness
#ifdef TRACECRAFT_EFFECT_RPM_ARC
  captureArcColors();
#endif
#ifdef TRACECRAFT_EFFECT_RPM_DOTS
  setupRpmDots();                     // dotted RPM arc (hides the solid arc)
#endif
#ifdef TRACECRAFT_EFFECT_SHIFT_LIGHTS
  setupShiftLights();
#endif
#ifdef TRACECRAFT_EFFECT_CAUTION_FLAG
  setupCautionFlag();
#endif
#ifdef TRACECRAFT_EFFECT_TELLTALES
  setupTellTales();                   // lamps flanking the speed readout
#endif
  s_t4Pages[0] = objects.main;        // the EEZ export's main screen
  s_t4Pages[1] = t4tires_setup();     // tires + brakes diagnostic page
  s_t4Pages[2] = t4perf_setup();      // FPS + host CPU/RAM/GPU/VRAM traces
  s_t4Pages[3] = lv_obj_create(NULL);
  setupSectorUI(s_t4Pages[3]);        // sector badge page
  s_t4Page = 1;                       // tires + brakes is the boot page
  lv_screen_load(s_t4Pages[1]);       // supersedes ui_init's FADE load
  xTaskCreate(t4TouchTask, "t4touch", 3072, nullptr, 2, nullptr);
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, t4IndevRead);
  app.ready();
}

void loop() {
  app.loop();
  t4deckConsumeSwipe();         // swipe left/right = page (sampled on its own task)
  ui_tick();                    // tick EVERY page — hidden pages stay warm
#ifdef TRACECRAFT_EFFECT_RPM_ARC
  applyArcLimitEffect();
#endif
#ifdef TRACECRAFT_EFFECT_RPM_DOTS
  applyRpmDots();
#endif
#ifdef TRACECRAFT_EFFECT_SHIFT_LIGHTS
  applyShiftLights();
#endif
#ifdef TRACECRAFT_EFFECT_CAUTION_FLAG
  applyCautionFlag();
#endif
#ifdef TRACECRAFT_EFFECT_FUEL_BAR
  applyFuelBar();
#endif
#ifdef TRACECRAFT_EFFECT_TIRE_TEMPS
  applyTireTemps();
#endif
#ifdef TRACECRAFT_EFFECT_TELLTALES
  applyTellTales();
#endif
  t4tires_tick(app.waiting());
  t4perf_tick(app.waiting());
  tickSectorUI();
}
