/**
 * TraceCraft P4 deck (Waveshare ESP32-P4 5" 1280x720): eight pages, swiped like a
 * phone — race dash, timing, weather, opponents, host-PC system, race focus, frame
 * rate, g-force. Tap the left or right half of the screen for brightness.
 *
 * The shared runtime (USB, display, boot
 * splash, render loop) is tracecraft::App in <TraceCraftApp.h>; this file holds
 * only this dashboard's UI.
 */
#include <TraceCraftApp.h>
#include "boards/waveshare_p4_lcd5.h"
#include "boards/waveshare_p4_lcd5_controls.h"
#include "tc_generated.h"               // channel registry + per-page views
#include "ui_p4dash/p4dash.h"           // the page deck: race dash,
#include "ui_p4timing/p4timing.h"       // lap/sector timing,
#include "ui_p4weather/p4weather.h"     // weather/track conditions,
#include "ui_p4opponents/p4opponents.h" // opponents / live timing,
#include "ui_p4sys/p4sys.h"             // host-PC system diagnostics (CPU/RAM/GPU/VRAM traces)
#include "ui_p4race/p4race.h"           // gear / position / flag-frame race-focus page
#include "ui_p4fps/p4fps.h"             // game frame-rate hero + rolling trace
#include "ui_p4gforce/p4gforce.h"       // friction circle + separate vertical-G column

using namespace tracecraft;

#ifndef TRACECRAFT_DEVICE_NAME
  #define TRACECRAFT_DEVICE_NAME "TraceCraft P4 Dash Deck"
#endif

static App app;
static board::P4Controls controls(app);

// The deck carries the Geist Sans subsets (ui_p4dash/fonts/).
LV_FONT_DECLARE(geist_sans_24);
LV_FONT_DECLARE(geist_sans_14);
// ONE waiting-for-telemetry modal for every P4 page. The pages used to carry
// their own hint labels in three different places; this lives on
// lv_layer_top() so it centers over whichever page is loaded. Non-clickable,
// so taps still reach the page beneath (brightness everywhere, stop-time
// config on PIT).
static lv_obj_t* s_waitModal   = nullptr;
static lv_obj_t* s_waitDot     = nullptr;
static lv_obj_t* s_waitTitle   = nullptr;   // headline (swaps by link state)
static lv_obj_t* s_waitSub     = nullptr;   // supporting line
static bool      s_waitShown   = false;
static int8_t    s_waitMsg     = -1;        // which copy is shown (-1 none, 0 no-recorder, 1 no-sim)
static uint32_t  s_waitPulseMs = 0;

static void waitModalInit() {
  s_waitModal = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_waitModal);
  // Fit narrow panels: the 560px design is wider than a 480px screen.
  const lv_coord_t hDisp = lv_display_get_horizontal_resolution(NULL);
  lv_obj_set_size(s_waitModal, hDisp < 560 ? hDisp - 24 : 560, 118);
  lv_obj_set_style_radius(s_waitModal, 20, 0);
  lv_obj_set_style_bg_opa(s_waitModal, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_waitModal, lv_color_hex(0x1c1c1e), 0);
  lv_obj_set_style_border_width(s_waitModal, 1, 0);
  lv_obj_set_style_border_color(s_waitModal, lv_color_hex(0x2a2d35), 0);
  lv_obj_set_style_shadow_width(s_waitModal, 36, 0);
  lv_obj_set_style_shadow_opa(s_waitModal, 100, 0);
  lv_obj_set_style_shadow_color(s_waitModal, lv_color_black(), 0);
  lv_obj_remove_flag(s_waitModal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_waitModal, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(s_waitModal);
  lv_obj_add_flag(s_waitModal, LV_OBJ_FLAG_HIDDEN);
#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 5)
  // 9.5 frosted glass: blur the page behind the modal (mostly static while
  // waiting, so the backdrop blur is a one-time cost per redraw region).
  lv_obj_set_style_blur_radius(s_waitModal, 18, 0);
  lv_obj_set_style_blur_backdrop(s_waitModal, true, 0);
  lv_obj_set_style_blur_quality(s_waitModal, LV_BLUR_QUALITY_SPEED, 0);
  lv_obj_set_style_bg_opa(s_waitModal, 210, 0);
#endif

  s_waitDot = lv_obj_create(s_waitModal);
  lv_obj_remove_style_all(s_waitDot);
  lv_obj_set_size(s_waitDot, 12, 12);
  lv_obj_set_style_radius(s_waitDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_waitDot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_waitDot, lv_color_hex(0xfacc15), 0);
  lv_obj_set_pos(s_waitDot, 32, 34);

  s_waitTitle = lv_label_create(s_waitModal);
  lv_obj_set_style_text_font(s_waitTitle, &geist_sans_24, 0);
  lv_obj_set_style_text_color(s_waitTitle, lv_color_hex(0xf1f2f5), 0);
  lv_obj_set_pos(s_waitTitle, 58, 24);

  s_waitSub = lv_label_create(s_waitModal);
  lv_obj_set_style_text_font(s_waitSub, &geist_sans_14, 0);
  lv_obj_set_style_text_color(s_waitSub, lv_color_hex(0x98989e), 0);
  lv_obj_set_pos(s_waitSub, 58, 62);
  // Text is set on first tick (it depends on whether the recorder is connected).
}

// `waiting` = no live telemetry; `recorderConnected` = the recorder is actually
// talking (link phase >= RecorderFound). The two are different states with
// different asks: no recorder at all -> "start the recorder"; recorder present
// but no sim -> "start your sim". Showing "waiting for telemetry" when nothing is
// even connected sends the user looking in the wrong place.
static void waitModalTick(bool waiting, bool recorderConnected) {
  if (!s_waitModal) return;
  if (waiting != s_waitShown) {
    s_waitShown = waiting;
    if (waiting) lv_obj_remove_flag(s_waitModal, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_waitModal, LV_OBJ_FLAG_HIDDEN);
  }
  if (waiting) {
    const int8_t msg = recorderConnected ? 1 : 0;
    if (msg != s_waitMsg) {
      s_waitMsg = msg;
      if (msg == 1) {
        // Recorder is talking, just no sim feeding it yet.
        lv_label_set_text(s_waitTitle, "Waiting for telemetry");
        lv_label_set_text(s_waitSub, "Start your sim and this display comes alive");
        lv_obj_remove_flag(s_waitDot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_waitTitle, 58, 24);
        lv_obj_set_pos(s_waitSub, 58, 62);
      } else {
        // Nothing is connected — point the user at the PC app, not the sim.
        // Reclaim the dot's gutter so the longer headline fits one line on the
        // narrow (480px) modal.
        lv_label_set_text(s_waitTitle, "Please start TraceCraft Recorder");
        lv_label_set_text(s_waitSub, "Launch it on your PC to stream live telemetry here");
        lv_obj_add_flag(s_waitDot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_waitTitle, 28, 24);
        lv_obj_set_pos(s_waitSub, 28, 62);
      }
    }
  }
  if (waiting) {
    const uint32_t now = millis();
    if (now - s_waitPulseMs >= 100) {   // gentle status-dot breathing
      s_waitPulseMs = now;
      const float w = 0.5f + 0.5f * sinf(now * 0.004f);
      lv_obj_set_style_bg_opa(s_waitDot, (lv_opa_t)(80 + 175 * w), 0);
    }
  }
}

#include "esp_heap_caps.h"
// --- Swipeable page deck: eight pages, see kPageN ------------------------------
// All pages build at boot and ALL tick every loop (change-detection makes an
// idle page nearly free). The host streams only the current page's view, so a
// hidden page ticks against the values it last had; what must stay current while
// hidden (history traces, the timing page's lap events) is in every view via the
// channel file's "always" list. A horizontal swipe/drag moves through the deck;
// the order wraps at both ends.
//
// Motion bypasses LVGL: lv_screen_load_anim(MOVE_*) software-redraws whole
// widget trees per animation frame (~40-80ms a frame at 1280x720 — a visibly
// steppy scroll). Pages are STATIC while moving, so each moves as a picture
// (below) and the split point is swept with board::slideBlit — two PPA DMA
// blits per frame, zero software rendering, panel-rate motion. The drag/glide
// loops are deliberately blocking: LVGL must not flush bands over the PPA
// composition, so it isn't serviced until motion lands, then the framebuffer is
// handed back with an ordinary screen load + full repaint of identical pixels.
static constexpr int kPageN   = 8;   // race dash / timing / weather / opponents / system / race-focus / fps / g-force
static lv_obj_t*     s_pages[kPageN] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
static int           s_page   = 0;

// --- A picture of every page, ready before the finger moves --------------------
// A drag can follow the finger only if the incoming page's pixels already exist,
// and drawing a hidden page is ~40-80ms of LVGL software rendering. It used to be
// drawn when a stroke began — but a swipe is over in ~150ms, so the finger had
// usually lifted before the pixels were ready: the drag never engaged, and the
// driver saw nothing move, then a pause, then a canned slide. So every page keeps
// a picture (1280x720 RGB565 in PSRAM, 1.8 MB; the eight plus the scratch grab
// are ~16 MB of the P4's 32). All eight are drawn at boot, one per loop lap,
// mostly under the splash; after that, the page you swipe away from is captured
// off the glass by the PPA grab the slide makes anyway (~3ms). A page slides in
// as it looked when you last left it, and goes live a frame or two after it
// lands, when its view's first frames arrive (PROTOCOL.md §4.7). Nothing is
// rendered on a swipe.
static uint16_t*     s_pic[kPageN]   = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
static bool          s_picOk[kPageN] = { false, false, false, false, false, false, false, false };
static uint16_t*     s_out     = nullptr;   // the outgoing page, grabbed off the glass for a slide
static lv_draw_buf_t s_picDb;
static int           s_picFill = 0;         // boot fill: the next page to draw

static int p4deckNext(int from) { return (from + 1) % kPageN; }
static int p4deckPrev(int from) { return (from + kPageN - 1) % kPageN; }

// --- Views: each page asks the host for its own channels (PROTOCOL.md §4.7) ---
// The deck declares all 91 channels its pages read, but the host streams only the
// current page's view (waveshare_p4_lcd5_deck.channels.json), so a page
// pays for its own data on the wire, not for all eight. Indexed like s_pages.
#ifndef TC_VIEW_COUNT
#error "the P4 deck needs its per-page views: build it from waveshare_p4_lcd5_deck.channels.json"
#endif
static const uint8_t* const kPageView[kPageN] = {
  TC_VIEW_DASH, TC_VIEW_TIMING, TC_VIEW_WEATHER, TC_VIEW_OPPONENTS,
  TC_VIEW_SYSTEM, TC_VIEW_RACE, TC_VIEW_FPS, TC_VIEW_GFORCE,
};

// The view for the page on the glass, announced as a page lands. A no-op when it
// is already current.
static void p4deckPageView() { app.dash().setView(kPageView[s_page]); }

static uint16_t* p4deckAlloc() {
  // 128-byte aligned: these buffers are PPA outputs (snapshotScreen) as well as
  // inputs, and the PPA rejects unaligned output buffers — a plain heap_caps_malloc
  // pointer made snapshotScreen fail silently and fall back to a ~60ms LVGL
  // render on every switch.
  return (uint16_t*)heap_caps_aligned_alloc(128, 1280 * 720 * 2, MALLOC_CAP_SPIRAM);
}

static void p4deckPicsInit() {
  int got = 0;
  for (int i = 0; i < kPageN; i++) if ((s_pic[i] = p4deckAlloc()) != nullptr) got++;
  if ((s_out = p4deckAlloc()) != nullptr) got++;
  // A page without a picture still pages, by LVGL's own slow animation, so a
  // shortfall would read as a sluggish deck rather than a failure. Say so.
  if (got < kPageN + 1)
    usb::log().printf("[deck] page pictures: only %d of %d PSRAM buffers allocated\n", got, kPageN + 1);
}

// Draw a page into its picture through LVGL: the page alone, never what the top
// layer holds over it.
static bool p4deckRender(int page) {
  constexpr uint32_t kW = 1280, kH = 720;
  if (!s_pic[page]) return false;
  lv_draw_buf_init(&s_picDb, kW, kH, LV_COLOR_FORMAT_RGB565, kW * 2, s_pic[page], kW * kH * 2);
  s_picOk[page] = lv_snapshot_take_to_draw_buf(s_pages[page], LV_COLOR_FORMAT_RGB565, &s_picDb) == LV_RESULT_OK;
  return s_picOk[page];
}

// The glass as it is, for the outgoing half of a slide (PPA, ~3ms).
static bool p4deckGrabOut() { return s_out != nullptr && board::snapshotScreen(s_out); }

static bool p4deckOverlaysClear();   // with the touch controls, whose overlays it checks

// After a slide: what was grabbed off the glass is the newest picture of the page
// it showed — unless an overlay (the brightness chip, the wait card, the page
// dots) was up with it, since a picture keeps what it shows until its page is
// left again. Swapped, not copied: the old picture becomes the next grab's
// scratch.
static void p4deckKeepOut(int page, bool clean) {
  if (!clean || s_out == nullptr || s_pic[page] == nullptr) return;
  uint16_t* const old = s_pic[page];
  s_pic[page] = s_out;
  s_out = old;
  s_picOk[page] = true;
}

// --- Page dots: where you are in the deck -------------------------------------
// A row of eight dots at the bottom with yours lit: the phone convention for
// "there are more pages — swipe". Up for a few seconds as the splash clears (when
// a driver first sees the dash) and briefly after every page change; hidden the
// moment a finger lands, so a swipe grabs the page without them. On the top
// layer, like the brightness chip.
static constexpr uint32_t kDotsAtBootMs  = 3500;
static constexpr uint32_t kDotsAfterPageMs = 1500;
static lv_obj_t* s_dots = nullptr;
static lv_obj_t* s_dot[kPageN] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
static uint32_t  s_dotsShownMs = 0, s_dotsForMs = 0;
// Whether the last render put the dots on the glass. Trails the HIDDEN flag by up
// to a lap (hidden now, still on the panel until LVGL next draws), which is what
// the picture grab has to know.
static bool      s_dotsOnGlass = false;

static void p4deckDotsInit() {
  constexpr int kD = 10, kGap = 14, kPadX = 16, kPadY = 10;
  s_dots = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_dots);
  lv_obj_set_size(s_dots, kPadX * 2 + kPageN * kD + (kPageN - 1) * kGap, kPadY * 2 + kD);
  lv_obj_set_style_radius(s_dots, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_dots, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_dots, lv_color_hex(0x16181c), 0);
  lv_obj_set_style_border_width(s_dots, 1, 0);
  lv_obj_set_style_border_color(s_dots, lv_color_hex(0x2a2d35), 0);
  lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);   // taps reach the page (brightness)
  lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -16);
  for (int i = 0; i < kPageN; i++) {
    s_dot[i] = lv_obj_create(s_dots);
    lv_obj_remove_style_all(s_dot[i]);
    lv_obj_set_size(s_dot[i], kD, kD);
    lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_dot[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_dot[i], kPadX + i * (kD + kGap), kPadY);
  }
  lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
}

static void p4deckDotsShow(uint32_t forMs) {
  if (!s_dots) return;
  for (int i = 0; i < kPageN; i++)
    lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(i == s_page ? 0xf1f2f5 : 0x4a4d55), 0);
  lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
  s_dotsShownMs = millis();
  s_dotsForMs = forMs;
  s_dotsOnGlass = true;   // conservatively: the next grab must not keep them
}

static void p4deckDotsHide() {
  if (s_dots) lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
  s_dotsForMs = 0;
}

// Once a loop lap, after LVGL has drawn: the boot picture fill and the dots.
static void p4deckTick() {
  // LVGL has just drawn, so the glass shows the dots exactly when they are not
  // hidden now. Read before anything below hides them: a hide takes the glass
  // only at the next draw.
  s_dotsOnGlass = s_dots != nullptr && !lv_obj_has_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
  // Every page's picture, one per lap (~40-80ms each): under the splash that is
  // invisible, and a host that answers fast only leaves the last few for the
  // dash's first second. Never with a finger down — it would hold up a drag.
  int16_t fx, fy;
  if (s_picFill < kPageN && !board::dragState(fx, fy)) {
    if (!s_picOk[s_picFill]) p4deckRender(s_picFill);
    s_picFill++;
  }
  // The dots come up as the splash goes: the moment the deck is first seen.
  static bool s_splashUp = true;
  if (s_splashUp && !app.splash().active()) p4deckDotsShow(kDotsAtBootMs);
  s_splashUp = app.splash().active();
  if (s_dotsForMs != 0 && millis() - s_dotsShownMs >= s_dotsForMs) p4deckDotsHide();
}

static void p4deckSwitch(bool towardNext);

// Finger-tracked page drag — the phone-native gesture. Once a stroke reads as a
// horizontal drag, the page pair locks to the finger, paced by real frame
// boundaries. On release it decides the way the touch classifier does
// (board::releasedSwipe): far enough, or a flick — then completes, carrying on
// from where the finger let go, or snaps back. Deciding by distance alone made a
// caught flick fall short, glide back, and then replay as a canned slide: the lag
// a quick swipe felt. Blocking while the finger is down — the pages are pictures
// during the drag, so pausing LVGL is invisible; telemetry catches up on release.
// A flick that lifts before a loop lap sees it is played by p4deckSwitch instead.
//
// `fired`: a swipe the touch task already called for this stroke while the finger
// was still down (+1 toward next, -1 toward prev). It is the drag's to honor; if
// the drag can't take the stroke after all, it plays as a slide rather than being
// lost.
static constexpr int kDragSlopPx = 18;   // a stroke's travel before it counts as a drag
static void p4deckTrackDrag(int8_t fired) {
  auto giveUp = [fired]() { if (fired != 0) p4deckSwitch(fired > 0); };
  int16_t dx, dy;
  if (!board::dragState(dx, dy)) { giveUp(); return; }
  if (app.splash().active()) return;
  const int adx0 = dx < 0 ? -dx : dx, ady0 = dy < 0 ? -dy : dy;
  if (adx0 < kDragSlopPx || adx0 < ady0) { giveUp(); return; }   // not (yet) a horizontal drag
  // Direction is LOCKED at engage: it picks the target page (left drag -> next,
  // right drag -> prev), and each direction slides in a different picture.
  const bool leftward = dx < 0;
  const int  from = s_page;
  const int  to = leftward ? p4deckNext(from) : p4deckPrev(from);
#ifdef TRACECRAFT_TOUCH_DEBUG
  const uint32_t tEngage = millis();
  const bool hadPic = s_picOk[to];
#endif
  // Normally ready. Only a swipe in the dash's first second, before the boot fill
  // reached this page, has to draw it here.
  if (!s_picOk[to] && !p4deckRender(to)) { giveUp(); return; }
  // The finger may have lifted during that render. Bail WITHOUT touching the
  // queue: if the stroke qualified on release, the classifier queued it and
  // p4deckSwitch will play it; if it qualified earlier, `fired` says so.
  if (!board::dragState(dx, dy)) { giveUp(); return; }
  const bool clean = p4deckOverlaysClear();
  if (!p4deckGrabOut()) { giveUp(); return; }
  // The page sits under the finger less the slop, from the first frame: a fast
  // stroke the loop only caught well past the slop jumps to catch up rather than
  // trailing the finger by everything it had already traveled.
  const int16_t engageDx = leftward ? -kDragSlopPx : kDragSlopPx;
#ifdef TRACECRAFT_TOUCH_DEBUG
  usb::log().printf("[deck] t=%lu track engage dx=%d -> pg%d (picture %s, %lums)\n",
                    (unsigned long)millis(), dx, to, hadPic ? "ready" : "drawn now",
                    (unsigned long)(millis() - tEngage));
#endif
  int split = 0, shownSplit = -1;
  while (board::dragState(dx, dy)) {                       // finger-locked
    const int rel = dx - engageDx;
    split = leftward ? -rel : rel;                         // travel in the locked direction
    if (split < 0) split = 0;
    if (split > 1280) split = 1280;
    if (split != shownSplit) {                             // skip no-motion frames
      shownSplit = split;
      if (leftward) board::slideBlit(s_out, s_pic[to], split);
      else          board::slideBlit(s_pic[to], s_out, 1280 - split);
    }
    board::waitFrame();
  }
  // dx/dy are the stroke's travel at lift; the classifier's rule decides.
  const bool complete = board::releasedSwipe(dx, dy) == (leftward ? -1 : 1);
  // Glide the remainder: finish the switch or snap back, eased, frame-paced.
  const int target = complete ? 1280 : 0;
  const int start = split;
  const uint32_t t0 = millis(), dur = 140;
  while (true) {
    const uint32_t el = millis() - t0;
    float k = el >= dur ? 1.0f : (float)el / dur;
    k = 1.0f - (1.0f - k) * (1.0f - k);                    // ease-out quad
    const int sp = start + (int)((target - start) * k);
    if (leftward) board::slideBlit(s_out, s_pic[to], sp);
    else          board::slideBlit(s_pic[to], s_out, 1280 - sp);
    if (el >= dur) break;
    board::waitFrame();
  }
#ifdef TRACECRAFT_TOUCH_DEBUG
  usb::log().printf("[deck] t=%lu track release split=%d dx=%d v=%.2f -> %s\n",
                    (unsigned long)millis(), split, dx, (double)board::dragVelocity(),
                    complete ? "complete" : "snap-back");
#endif
  if (complete) s_page = to;
  p4deckKeepOut(from, clean);
  p4deckPageView();   // the page that landed (or stayed) asks for its channels
  lv_screen_load(s_pages[s_page]);
  lv_obj_invalidate(s_pages[s_page]);
  p4deckDotsShow(kDotsAfterPageMs);
  // The drag handled this stroke either way, by the classifier's own rule, so the
  // swipe it queued on release (if any) is already played or rightly declined.
  int16_t qx, qy;
  while (board::touchGesture(qx, qy) != board::TouchGesture::None) {}
}

// The fired-swipe fallback: a flick that lifted before the drag engaged. The same
// pictures, played as a short eased slide.
static void p4deckSwitch(bool towardNext) {
  const int from = s_page;
  const int to = towardNext ? p4deckNext(from) : p4deckPrev(from);
#ifdef TRACECRAFT_TOUCH_DEBUG
  const uint32_t tSw = millis();
  const bool hadPic = s_picOk[to];
#endif
  const bool clean = p4deckOverlaysClear();
  const bool okOut = p4deckGrabOut();
  const bool okIn = s_picOk[to] || p4deckRender(to);
#ifdef TRACECRAFT_TOUCH_DEBUG
  usb::log().printf("[deck] t=%lu switch pg%d->pg%d ready in %lums (picture %s)\n",
                    (unsigned long)millis(), from, to, (unsigned long)(millis() - tSw),
                    hadPic ? "ready" : "drawn now");
#endif
  s_page = to;
  p4deckPageView();
  if (!okOut || !okIn) {
    // No picture buffers (PSRAM ran short at boot — logged there): LVGL's own
    // animation still works, it's just the slow software path.
    lv_screen_load_anim(s_pages[to],
        towardNext ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
        220, 0, false);
    p4deckDotsShow(kDotsAfterPageMs);
    return;
  }
  const uint32_t kDurMs = 180;   // short enough to feel immediate, long enough to read as motion
  const uint32_t t0 = millis();
  while (true) {
    const uint32_t el = millis() - t0;
    float k = el >= kDurMs ? 1.0f : (float)el / kDurMs;
    k = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);        // ease-out cubic
    const int split = (int)(k * 1280.0f + 0.5f);
    if (towardNext) board::slideBlit(s_out, s_pic[to], split);
    else            board::slideBlit(s_pic[to], s_out, 1280 - split);
    if (el >= kDurMs) break;
    board::waitFrame();     // pace to the panel's real frame boundary (tear-free)
  }
  p4deckKeepOut(from, clean);
  lv_screen_load(s_pages[to]);
  lv_obj_invalidate(s_pages[to]);   // LVGL repaints the live page and owns the FB again
  p4deckDotsShow(kDotsAfterPageMs);
}

// Whether the glass shows the page alone — no brightness chip, no wait card, no
// page dots — so a grab of it can be kept as that page's picture (p4deckKeepOut).
static bool p4deckOverlaysClear() {
  return !controls.brightnessChipShown() && !s_waitShown && !s_dotsOnGlass;
}

// The deck's own gestures, ahead of the board's brightness tap.
static int8_t s_heldSwipe = 0;   // a swipe called mid-stroke, left to the drag
static bool p4deckGesture(board::TouchGesture g, int16_t, int16_t) {
  if (g == board::TouchGesture::None) {   // queue drained
    p4deckTrackDrag(s_heldSwipe);          // finger-locked page drag (blocks while dragging)
    s_heldSwipe = 0;
    return true;
  }
  if (g == board::TouchGesture::Down) { p4deckDotsHide(); return false; }   // off the glass before a swipe grabs it
  if (g == board::TouchGesture::SwipeLeft || g == board::TouchGesture::SwipeRight) {
    // A swipe called while the finger is still down (it passed the swipe
    // distance) is the drag's: p4deckTrackDrag moves the page with the finger.
    // Only a stroke already lifted — a flick quicker than a loop lap — plays as
    // a slide here.
    const int8_t dir = g == board::TouchGesture::SwipeLeft ? 1 : -1;
    int16_t hx, hy;
    if (board::dragState(hx, hy)) s_heldSwipe = dir;
    else                          p4deckSwitch(dir > 0);
    return true;
  }
  return false;
}

void setup() {
  app.begin(TRACECRAFT_DEVICE_NAME, board::renderer(), board::hasPSRAM());
  controls.setChipFont(&geist_sans_24);
  controls.onGesture(p4deckGesture);
  s_pages[0] = p4dash_setup();        // deck order: race / timing / weather / opponents / system
  s_pages[1] = p4timing_setup();
  s_pages[2] = p4weather_setup();
  s_pages[3] = p4opponents_setup();
  s_pages[4] = p4sys_setup();
  s_pages[5] = p4race_setup();
  s_pages[6] = p4fps_setup();
  s_pages[7] = p4gforce_setup();
  s_page = 5;                     // RACE (gear/position/lights) boots first
  lv_screen_load(s_pages[s_page]);
  p4deckPageView();               // announced once a host has HELLOed
  p4deckPicsInit();               // a picture per page; drawn one per lap by p4deckTick
  waitModalInit();                // before the splash, which must draw above it
  p4deckDotsInit();
  app.ready();
  controls.bootCue();
}

void loop() {
  controls.serviceTouch();
  app.loop();
  controls.readyCue();
  TC_LOOPMARK('d');
  p4dash_tick(app.waiting());     // tick EVERY page — see the deck comment above
  TC_LOOPMARK('e');
  p4timing_tick(app.waiting());
  TC_LOOPMARK('f');
  p4weather_tick(app.waiting());
  TC_LOOPMARK('g');
  p4opponents_tick(app.waiting());
  TC_LOOPMARK('h');
  p4sys_tick(app.waiting());
  p4race_tick(app.waiting());
  p4fps_tick(app.waiting());
  p4gforce_tick(app.waiting());
  TC_LOOPMARK('i');
  waitModalTick(app.waiting(), (uint8_t)app.dash().linkPhase() >= 2);
  p4deckTick();                   // page pictures at boot; page dots
  TC_LOOPMARK('j');
  controls.serviceTouch();        // drain gestures again: halves touch-to-action latency
}
