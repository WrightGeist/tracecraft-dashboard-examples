// TraceCraft P4 deck — OPPONENTS / live-timing page. 1280x720 landscape.
//
// The race-awareness page: who you're fighting, and where the whole field is.
// Three regions, all fed by the opponent channels the recorder derives
// from the live driver roster (LMU today — the sim that publishes a scored
// roster; other sims send nothing and the page falls back to its idle state):
//
//   - BATTLE STRIP (top, the hero): two big cards — the car directly AHEAD and
//     directly BEHIND you on the road, each with driver name, class tag, and
//     the live gap ("+1.3s", "+1 LAP"). This is the glance you take mid-corner:
//     am I catching them / are they catching me.
//   - CLASS BADGE (header right): your position within your OWN class — the
//     multi-class sleeper ("P2 / 9 LMGT3"). In a GT3 car buried in an LMP2
//     field, overall position lies; class position is the race you're in.
//   - RUNNING ORDER (bottom): a mini timing tower, top six overall, each row
//     position / name / class / gap-to-leader. The leader's gap reads "LEADER"
//     (the recorder sends an em-dash there, which isn't in the ASCII font
//     subset, so the first row is special-cased to a word).
//
// Honesty rule (same as the weather page): an unsent channel decodes as its type's
// zero, and an older recorder can't say it is absent (only one that sends presence
// can, Store::has()), so empty string is "no data" whichever recorder is driving.
// So a blank name means that slot has no car (you're leading -> no car ahead;
// fewer than six cars -> short tower), and the whole page shows a WAITING
// overlay until any opponent name arrives. We never invent a rival.
//
// Typography follows the deck: the title borrows the shared Geist Mono heading
// voice (uppercase); everything else — names, gaps, class tags — is Geist Sans,
// because the gap/class strings mix letters and digits ("+1 LAP", "LMGT3") and
// the mono subsets carry digits only. Fonts live in ui_p4dash/fonts/, declared
// once and shared across every deck page.
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include "p4opponents.h"
#include "tc_generated.h"

LV_FONT_DECLARE(geist_mono_22);   // page title — the deck's shared heading voice
LV_FONT_DECLARE(geist_sans_16);   // chip text (class tags)
LV_FONT_DECLARE(geist_sans_24);   // captions, tower name/gap, badge
LV_FONT_DECLARE(geist_sans_28);   // battle caption, tower position
LV_FONT_DECLARE(geist_sans_32);   // section caption, class badge
LV_FONT_DECLARE(geist_sans_40);   // battle driver name + gap (the hero rows)

namespace {

// ---- palette — iOS dark-mode system colors (shared with the weather page) ------
constexpr uint32_t kBg    = 0x000000;  // page backdrop
constexpr uint32_t kCard  = 0x1c1c1e;  // cards
constexpr uint32_t kInset = 0x2c2c2e;  // sub-tiles / position badge
constexpr uint32_t kWell  = 0x3a3a3c;  // row separators
constexpr uint32_t kInk   = 0xffffff;  // primary label
constexpr uint32_t kInk2  = 0x98989e;  // secondary label
constexpr uint32_t kMiss  = 0x5d5d61;  // absent / placeholder ink
constexpr uint32_t kBlue  = 0x0a84ff;  // AHEAD accent — the car you're chasing
constexpr uint32_t kOrange= 0xff9f0a;  // BEHIND accent — the car defending against

// Class-tag palette — a stable color per car class so multi-class fields read
// at a glance (LMU: Hypercar / LMP2 / LMGT3). Hash the class string into this.
const uint32_t kClsPalette[8] = {
  0x0a84ff, 0x30d158, 0xff9f0a, 0xff453a,
  0xbf5af2, 0x64d2ff, 0xffd60a, 0xff6482,
};

// ---- geometry (1280x720) --------------------------------------------------------
constexpr int kM = 32;                        // outer margin
constexpr int kBattleY = 104, kBattleH = 224;
constexpr int kCardW = 596;                   // (1280 - 2*kM - 24 gutter) / 2
constexpr int kAheadX = kM, kBehindX = kM + kCardW + 24;
constexpr int kTwrY = kBattleY + kBattleH + 20;   // 348
constexpr int kTwrH = 720 - kTwrY - kM;           // 340
constexpr int kTwrW = 1280 - 2 * kM;              // 1216
constexpr int kRowTop = 70, kRowH = 44;           // 6 rows inside the tower card
// Tower column x-origins (row-container local).
constexpr int kColName = 68, kColClass = 700;
constexpr int kNameW   = 620;

constexpr uint32_t kTextMs = 150;   // race timing moves at a few Hz

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

void setHidden(lv_obj_t* o, bool hide) {
  if (!o) return;
  if (hide && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  if (!hide && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* makeBox(lv_obj_t* parent, int w, int h, uint32_t fill, int radius) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, hex(fill), 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* txt) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, hex(color), 0);
  lv_label_set_text(l, txt);
  return l;
}

// Uppercase letter-spaced caption — the iOS section-header voice.
lv_obj_t* makeCaption(lv_obj_t* parent, const lv_font_t* font, uint32_t color,
                      const char* txt, int ls = 1) {
  lv_obj_t* l = makeLabel(parent, font, color, txt);
  lv_obj_set_style_text_letter_space(l, ls, 0);
  return l;
}

// A class tag: a small rounded label with a translucent fill of its class color
// and matching bright ink. Auto-sizes to its text; hidden when the class is blank.
lv_obj_t* makeChip(lv_obj_t* parent, const lv_font_t* font) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_pad_hor(l, 12, 0);
  lv_obj_set_style_pad_ver(l, 5, 0);
  lv_obj_set_style_radius(l, 8, 0);
  lv_obj_set_style_bg_opa(l, 48, 0);
  lv_label_set_text(l, "");
  return l;
}

// Deterministic class -> palette index (djb2). Same class always same color.
uint32_t classColor(const char* s) {
  uint32_t h = 5381;
  for (const char* p = s; *p; ++p) h = ((h << 5) + h) + (uint8_t)*p;
  return kClsPalette[h & 7];
}

// ---- widgets --------------------------------------------------------------------
lv_obj_t* s_scr    = nullptr;
lv_obj_t* s_title  = nullptr;
lv_obj_t* s_badge  = nullptr;                  // class-position pill, header right
// Battle strip — index 0 = AHEAD, 1 = BEHIND.
lv_obj_t* s_bCard[2]  = { nullptr, nullptr };
lv_obj_t* s_bName[2]  = { nullptr, nullptr };
lv_obj_t* s_bClass[2] = { nullptr, nullptr };
lv_obj_t* s_bGap[2]   = { nullptr, nullptr };
// Running-order tower.
lv_obj_t* s_twr       = nullptr;
lv_obj_t* s_row[6]    = { nullptr };
lv_obj_t* s_rowPos[6] = { nullptr };           // number inside the position badge
lv_obj_t* s_rowName[6]= { nullptr };
lv_obj_t* s_rowCls[6] = { nullptr };
lv_obj_t* s_rowGap[6] = { nullptr };
// Idle overlay — shown until any opponent name arrives.
lv_obj_t* s_empty     = nullptr;

// ---- change caches --------------------------------------------------------------
uint32_t s_txtMs = 0;
int  s_emptyShown = -1;                        // -1 = force first paint
char s_badgePrev[40] = "";
char s_bNamePrev[2][40] = { "", "" };
char s_bClsPrev[2][24]  = { "", "" };
char s_bGapPrev[2][20]  = { "", "" };
char s_rowNamePrev[6][40]= { { 0 } };
char s_rowClsPrev[6][24] = { { 0 } };
char s_rowGapPrev[6][20] = { { 0 } };
int  s_rowPosPrev[6]     = { -1, -1, -1, -1, -1, -1 };

// Channel-index tables (schema order is fixed at generation, but reading by the
// TC_CH_* enum keeps this robust to reorders).
const int kAheadName[2] = { TC_CH_AHEAD_NAME, TC_CH_BEHIND_NAME };
const int kAheadCls[2]  = { TC_CH_AHEAD_CLS,  TC_CH_BEHIND_CLS  };
const int kAheadGap[2]  = { TC_CH_AHEAD_GAP_STR, TC_CH_BEHIND_GAP_STR };
const int kRowPos[6] = { TC_CH_P1_POS, TC_CH_P2_POS, TC_CH_P3_POS,
                         TC_CH_P4_POS, TC_CH_P5_POS, TC_CH_P6_POS };
const int kRowName[6]= { TC_CH_P1_NAME, TC_CH_P2_NAME, TC_CH_P3_NAME,
                         TC_CH_P4_NAME, TC_CH_P5_NAME, TC_CH_P6_NAME };
const int kRowCls[6] = { TC_CH_P1_CLS, TC_CH_P2_CLS, TC_CH_P3_CLS,
                         TC_CH_P4_CLS, TC_CH_P5_CLS, TC_CH_P6_CLS };
const int kRowGap[6] = { TC_CH_P1_GAP_STR, TC_CH_P2_GAP_STR, TC_CH_P3_GAP_STR,
                         TC_CH_P4_GAP_STR, TC_CH_P5_GAP_STR, TC_CH_P6_GAP_STR };

// Set a label's text + cache only when it changed. Returns whether it changed.
bool setText(lv_obj_t* l, char* cache, size_t cap, const char* txt) {
  if (strncmp(txt, cache, cap - 1) == 0) return false;
  strncpy(cache, txt, cap - 1);
  cache[cap - 1] = '\0';
  lv_label_set_text(l, txt);
  return true;
}

// Recolor + retext a class chip; hide it when the class is blank.
void setChip(lv_obj_t* chip, char* cache, size_t cap, const char* cls) {
  const bool on = cls && cls[0];
  setHidden(chip, !on);
  if (!on) { cache[0] = '\0'; return; }
  if (!setText(chip, cache, cap, cls)) return;
  const uint32_t c = classColor(cls);
  lv_obj_set_style_bg_color(chip, hex(c), 0);
  lv_obj_set_style_text_color(chip, hex(c), 0);
}

} // namespace

lv_obj_t* p4opponents_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);   // own screen — the deck pager loads it
  s_scr = scr;
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // --- header: title + class-position pill ----------------------------------------
  s_title = makeCaption(scr, &geist_mono_22, kInk, "OPPONENTS", 3);
  lv_obj_set_pos(s_title, kM + 4, 40);
  s_badge = makeChip(scr, &geist_sans_32);
  lv_obj_set_style_bg_opa(s_badge, 40, 0);
  lv_obj_align(s_badge, LV_ALIGN_TOP_RIGHT, -(kM + 4), 28);
  lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

  // --- battle strip: AHEAD (left) / BEHIND (right) --------------------------------
  const char* bCap[2]  = { "AHEAD", "BEHIND" };
  const int   bX[2]    = { kAheadX, kBehindX };
  const uint32_t bAcc[2] = { kBlue, kOrange };
  for (int i = 0; i < 2; i++) {
    lv_obj_t* card = makeBox(scr, kCardW, kBattleH, kCard, 24);
    lv_obj_set_pos(card, bX[i], kBattleY);
    s_bCard[i] = card;
    // A thin accent bar down the inner edge keys each card to its role color.
    lv_obj_t* bar = makeBox(card, 6, kBattleH - 44, bAcc[i], 3);
    lv_obj_set_pos(bar, 22, 22);
    lv_obj_t* cap = makeCaption(card, &geist_sans_28, bAcc[i], bCap[i], 2);
    lv_obj_set_pos(cap, 44, 22);
    s_bClass[i] = makeChip(card, &geist_sans_24);
    lv_obj_align(s_bClass[i], LV_ALIGN_TOP_RIGHT, -22, 22);
    s_bName[i] = makeLabel(card, &geist_sans_40, kInk, "");
    lv_label_set_long_mode(s_bName[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_bName[i], kCardW - 68);
    lv_obj_set_pos(s_bName[i], 44, 78);
    s_bGap[i] = makeLabel(card, &geist_sans_40, bAcc[i], "");
    lv_obj_set_pos(s_bGap[i], 44, 150);
  }

  // --- running-order tower --------------------------------------------------------
  s_twr = makeBox(scr, kTwrW, kTwrH, kCard, 24);
  lv_obj_set_pos(s_twr, kM, kTwrY);
  lv_obj_t* twrCap = makeCaption(s_twr, &geist_sans_32, kInk2, "RUNNING ORDER", 2);
  lv_obj_set_pos(twrCap, 26, 18);
  for (int i = 0; i < 6; i++) {
    lv_obj_t* row = lv_obj_create(s_twr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, kTwrW - 40, kRowH - 4);
    lv_obj_set_pos(row, 20, kRowTop + i * kRowH);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    // Hairline separator above every row but the first.
    if (i > 0) {
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, hex(kWell), 0);
    }
    s_row[i] = row;
    lv_obj_t* badge = makeBox(row, 48, 34, kInset, 9);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
    s_rowPos[i] = makeLabel(badge, &geist_sans_28, kInk, "");
    lv_obj_center(s_rowPos[i]);
    s_rowName[i] = makeLabel(row, &geist_sans_28, kInk, "");
    lv_label_set_long_mode(s_rowName[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_rowName[i], kNameW);
    lv_obj_align(s_rowName[i], LV_ALIGN_LEFT_MID, kColName, 0);
    s_rowCls[i] = makeChip(row, &geist_sans_16);
    lv_obj_align(s_rowCls[i], LV_ALIGN_LEFT_MID, kColClass, 0);
    s_rowGap[i] = makeLabel(row, &geist_sans_24, kInk2, "");
    lv_obj_align(s_rowGap[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }

  // --- idle overlay ---------------------------------------------------------------
  s_empty = makeBox(scr, 780, 200, kCard, 24);
  lv_obj_center(s_empty);
  lv_obj_t* et = makeLabel(s_empty, &geist_sans_40, kInk, "WAITING FOR RACE DATA");
  lv_obj_set_style_text_letter_space(et, 1, 0);
  lv_obj_align(et, LV_ALIGN_CENTER, 0, -22);
  lv_obj_t* es = makeLabel(s_empty, &geist_sans_28, kInk2,
                           "Opponent timing appears during a race session.");
  lv_obj_align(es, LV_ALIGN_CENTER, 0, 30);

  return scr;
}

void p4opponents_tick(bool waitingForData) {
  const uint32_t nowMs = millis();
  if (nowMs - s_txtMs < kTextMs) return;
  s_txtMs = nowMs;

  // Presence test: any opponent name (battle or tower) means we have a scored
  // roster. While waiting for the link, force the idle state.
  const char* aheadNm  = tcStore.getStr(TC_CH_AHEAD_NAME);
  const char* behindNm = tcStore.getStr(TC_CH_BEHIND_NAME);
  const char* p1Nm     = tcStore.getStr(TC_CH_P1_NAME);
  const bool hasData = !waitingForData &&
      ((aheadNm && aheadNm[0]) || (behindNm && behindNm[0]) || (p1Nm && p1Nm[0]));

  const int empty = hasData ? 0 : 1;
  if (empty != s_emptyShown) {
    s_emptyShown = empty;
    setHidden(s_empty, !empty);
    for (int i = 0; i < 2; i++) setHidden(s_bCard[i], empty);
    setHidden(s_twr, empty);
    setHidden(s_badge, empty || s_badgePrev[0] == '\0');
  }
  if (empty) return;   // nothing more to paint until a roster arrives

  // --- class-position pill --------------------------------------------------------
  const char* cps = tcStore.getStr(TC_CH_CLASS_POSITION_STR);
  const bool cpOn = cps && cps[0];
  setHidden(s_badge, !cpOn);
  if (cpOn && setText(s_badge, s_badgePrev, sizeof(s_badgePrev), cps))
    lv_obj_align(s_badge, LV_ALIGN_TOP_RIGHT, -(kM + 4), 28);
  else if (!cpOn) s_badgePrev[0] = '\0';

  // --- battle strip ---------------------------------------------------------------
  const uint32_t bAcc[2] = { kBlue, kOrange };
  for (int i = 0; i < 2; i++) {
    const char* nm  = tcStore.getStr(kAheadName[i]);
    const char* cls = tcStore.getStr(kAheadCls[i]);
    const char* gap = tcStore.getStr(kAheadGap[i]);
    const bool present = nm && nm[0];
    if (present) {
      if (setText(s_bName[i], s_bNamePrev[i], sizeof(s_bNamePrev[i]), nm))
        lv_obj_set_style_text_color(s_bName[i], hex(kInk), 0);
      setChip(s_bClass[i], s_bClsPrev[i], sizeof(s_bClsPrev[i]), cls);
      if (setText(s_bGap[i], s_bGapPrev[i], sizeof(s_bGapPrev[i]), gap ? gap : ""))
        lv_obj_set_style_text_color(s_bGap[i], hex(bAcc[i]), 0);
    } else {
      // Real race state: no car in this slot (you lead / you're last).
      const char* placeholder = (i == 0) ? "Clear ahead" : "Clear behind";
      if (setText(s_bName[i], s_bNamePrev[i], sizeof(s_bNamePrev[i]), placeholder))
        lv_obj_set_style_text_color(s_bName[i], hex(kMiss), 0);
      setChip(s_bClass[i], s_bClsPrev[i], sizeof(s_bClsPrev[i]), "");
      setText(s_bGap[i], s_bGapPrev[i], sizeof(s_bGapPrev[i]), "");
    }
  }

  // --- running-order tower --------------------------------------------------------
  for (int i = 0; i < 6; i++) {
    const char* nm  = tcStore.getStr(kRowName[i]);
    const bool present = nm && nm[0];
    setHidden(s_row[i], !present);
    if (!present) { s_rowNamePrev[i][0] = '\0'; continue; }

    const int pos = (int)tcStore.getInt(kRowPos[i]);
    if (pos != s_rowPosPrev[i]) {
      s_rowPosPrev[i] = pos;
      char b[8];
      snprintf(b, sizeof(b), "%d", pos);
      lv_label_set_text(s_rowPos[i], b);
      lv_obj_center(s_rowPos[i]);
    }
    setText(s_rowName[i], s_rowNamePrev[i], sizeof(s_rowNamePrev[i]), nm);
    setChip(s_rowCls[i], s_rowClsPrev[i], sizeof(s_rowClsPrev[i]), tcStore.getStr(kRowCls[i]));
    // Row 0 is always the overall leader; the recorder marks its gap with an
    // em-dash (outside the ASCII subset) — show the word instead.
    const char* gap = (i == 0) ? "LEADER" : tcStore.getStr(kRowGap[i]);
    if (setText(s_rowGap[i], s_rowGapPrev[i], sizeof(s_rowGapPrev[i]), gap ? gap : ""))
      lv_obj_set_style_text_color(s_rowGap[i], hex(i == 0 ? kInk : kInk2), 0);
  }
}
