// TraceCraft P4 deck — WEATHER & TRACK CONDITIONS page. 1280x720 landscape.
//
// Every weather channel any supported sim publishes, grouped the
// way a phone weather app would: an AIR card (ambient conditions — temp, wind
// compass, humidity, pressure, cloud, fog), a TRACK card (what the tires feel
// — surface temp, wetness, grip/rubber; the wetness/grip words stand alone,
// no captions), and a full-width FORECAST strip along the bottom: a NOW→+35'
// timeline where sun/cloud/rain glyphs drift LEFT as in-sim time passes, so
// what's coming slides toward "now". The sim's +10'/+30' rain channels are
// ROLLING forecasts, so each glyph re-samples the live curve at its own
// drifting time offset — motion without ever contradicting the sim. Visual
// language follows iOS dark-mode conventions: rounded translucent cards,
// inset sub-tiles, uppercase letter-spaced captions, hero numerals, and the
// iOS system accent palette for state color.
//
// THE LIVING SKY. The page background is a sky that reflects the sim's
// conditions, driven only by channels the sim actually sent:
//   - time_of_day  -> sun elevation: blue day, golden dawn/dusk gradients,
//                     near-black night with a twinkling star field + moon
//   - cloud/fog    -> blends the clear sky toward bright overcast gray
//   - precip       -> a cloud with animated rain drops falling from it
// Cards sit at ~88% opacity so the sky glows through, and the header ink
// flips dark when the sky behind it is bright (overcast noon). No clock
// channel (or waiting for telemetry) means no sky: the page falls back to
// the neutral near-black backdrop rather than guessing.
//
// Sky redraw discipline: the full-screen gradient restyles ONLY when the
// quantized inputs change (once per in-sim minute / 5% cover step) — a
// full-screen invalidation at 5 Hz would be a constant ~50ms redraw tax.
// Stars twinkle and drops fall by touching only their own few-pixel areas.
//
// Per-sim honesty is unchanged: each tile is gated by its bit in the
// `weather_avail` presence mask (the host sets a bit only when it actually
// emitted that channel this frame). A clear bit dims the tile to the missing
// ink and blanks the value — AC EVO truthfully lights only the temps and the
// clock, iRacing nearly everything, and the +10'/+30' rain forecast is ACC's
// unique channel. The mask exists because an unsent channel decodes as zero,
// and zero is real data for most weather values (0% rain on a sunny iRacing day);
// it predates per-channel presence (Store::has()) and, unlike it, works with
// every recorder version. Never infer support from values.
//
// Typography: all Geist. Words + captions use Geist Sans semibold subsets
// (full ASCII + the degree sign); numerals stay Geist Mono (digit subsets).
// Both live in ui_p4dash/fonts/ (NOTICE.md has the regen commands), so the
// deck speaks one type family across every page.
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "p4weather.h"
#include "tc_generated.h"

LV_FONT_DECLARE(geist_mono_96);       // hero temperatures  (0-9 -)
LV_FONT_DECLARE(geist_mono_time_48);  // header clock + wind speed (0-9 : . + -)
LV_FONT_DECLARE(geist_mono_time_36);  // tile numerals      (0-9 : . + -)
LV_FONT_DECLARE(geist_mono_22);       // page title — the deck's shared heading voice
LV_FONT_DECLARE(geist_sans_16);       // compass cardinals
LV_FONT_DECLARE(geist_sans_24);       // tile captions
LV_FONT_DECLARE(geist_sans_28);       // group captions, units, rain-row labels
LV_FONT_DECLARE(geist_sans_32);       // forecast axis markers + (n/a)
LV_FONT_DECLARE(geist_sans_40);       // wetness / grip words + hero degree units

namespace {

// ---- palette — iOS dark-mode system colors -------------------------------------
constexpr uint32_t kBg     = 0x000000;  // idle backdrop (no sky data)
constexpr uint32_t kCard   = 0x1c1c1e;  // secondarySystemBackground (cards)
constexpr uint32_t kInset  = 0x2c2c2e;  // tertiarySystemBackground (sub-tiles)
constexpr uint32_t kWell   = 0x3a3a3c;  // bar tracks / unlit segments / ring
constexpr uint32_t kInk    = 0xffffff;  // label
constexpr uint32_t kInk2   = 0x98989e;  // secondaryLabel
constexpr uint32_t kMiss   = 0x5d5d61;  // channel-absent ink
constexpr uint32_t kBlue   = 0x0a84ff;  // systemBlue   — precipitation
constexpr uint32_t kGreen  = 0x30d158;  // systemGreen  — dry surface / grip
constexpr uint32_t kOrange = 0xff9f0a;  // systemOrange — hot-track tint
constexpr uint32_t kRed    = 0xff453a;  // systemRed    — extreme wetness

// ---- geometry (1280x720) --------------------------------------------------------
constexpr int kM = 32;                     // outer margin
constexpr int kCardY = 100, kCardH = 436;
constexpr int kAirX = kM,   kAirW = 780;
constexpr int kTrkX = 832,  kTrkW = 416;   // kAirX + kAirW + 20 gutter
constexpr int kPad = 24;                   // card padding
constexpr int kRowBY = 280, kRowBH = 140;  // stat-tile row (AIR card)
constexpr int kSurfY = 256, kGripY = 344, kTileH2 = 76;  // TRACK value tiles

// Wind compass (AIR-card-local).
constexpr int kRingD = 160, kRingX = 572, kRingY = 52;

// Forecast strip: full width under both cards. Timeline runs NOW..~2 h with
// markers every 30 minutes. The sim's rolling forecast only reaches +30',
// so markers beyond that render as dim "beyond the horizon" rings — they
// drift left with the clock and RESOLVE into a real condition glyph the
// moment they cross the +30' boundary. Nothing past the horizon is invented.
constexpr int kFcY = 556, kFcH = 132, kFcW = 1216;
constexpr int kFcX0 = 70;                  // panel-local x of T=0 ("NOW")
constexpr int kFcPxPerMin = 8;             // 120 min -> 960 px of axis
constexpr int kFcGlyphs = 5;               // markers, 30 min apart
constexpr float kFcSpanMin = 150.0f;       // wrap period: 5 markers x 30 min

// Sky layer.
constexpr int kStars = 22;
constexpr int kDrops = 14;                   // page-wide rain field ceiling
                                             // (region count per frame is the
                                             // scarce resource on this panel)
constexpr int kCloudX = 600, kCloudY = 10;   // rain cloud, header strip

// weather_avail bit assignments — MUST mirror the manifest entry / mapper.
enum WxBit {
  BitAirTemp = 0, BitTrackTemp = 1, BitHumidity = 2, BitPressure = 3,
  BitWind = 4, BitPrecip = 5, BitRain10 = 6, BitRain30 = 7,
  BitCloud = 8, BitFog = 9, BitWetness = 10, BitGrip = 11, BitClock = 12,
};

// Title-case forms of the canonical 1..7 wetness vocabulary
// (tcr-parser trackWetnessLabel).
const char* kWetnessWord[8] = {
  "-", "Dry", "Mostly Dry", "Barely Wet", "Lightly Wet",
  "Moderately Wet", "Very Wet", "Extremely Wet",
};
// LMU grip: raw byte; 0..4 map onto the label scale, larger shows numeric.
const char* kGripWord[5] = { "Green", "Low", "Medium", "High", "Saturated" };

constexpr uint32_t kTextMs = 200;   // weather moves slowly — 5 Hz label updates

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

uint32_t lerp32(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0; if (t > 1) t = 1;
  const int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
  const int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
  return ((uint32_t)(ar + (br - ar) * t) << 16) |
         ((uint32_t)(ag + (bg - ag) * t) << 8)  |
          (uint32_t)(ab + (bb - ab) * t);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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

// ---- widgets --------------------------------------------------------------------------
lv_obj_t* s_scr       = nullptr;
lv_obj_t* s_title     = nullptr;
lv_obj_t* s_clock     = nullptr;                 // in-sim time, header right
// Sky layer (created bottom-of-stack; drops are created last, in front).
lv_obj_t* s_star[kStars] = { nullptr };
uint8_t   s_starBase[kStars]; uint8_t s_starPh[kStars];
lv_obj_t* s_moon = nullptr; lv_obj_t* s_moonPart[3] = { nullptr, nullptr, nullptr };
lv_obj_t* s_sun  = nullptr;
lv_obj_t* s_cloud = nullptr; lv_obj_t* s_cloudPart[4] = { nullptr, nullptr, nullptr, nullptr };
lv_obj_t* s_drop[kDrops] = { nullptr };
float     s_dropY[kDrops]; int16_t s_dropX[kDrops]; uint16_t s_dropV[kDrops];
// Card + inset-tile handles for sky-adaptive translucency.
lv_obj_t* s_card[3] = { nullptr, nullptr, nullptr };   // AIR, TRACK, FORECAST
lv_obj_t* s_tile[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
// AIR card
lv_obj_t* s_airVal    = nullptr; lv_obj_t* s_airDeg  = nullptr;
lv_obj_t* s_windCap   = nullptr; lv_obj_t* s_windRing = nullptr;
lv_obj_t* s_windArrow = nullptr; lv_obj_t* s_cardinal[4] = { nullptr, nullptr, nullptr, nullptr };
lv_obj_t* s_windVal   = nullptr; lv_obj_t* s_windUnit = nullptr;
lv_obj_t* s_statVal[4]  = { nullptr, nullptr, nullptr, nullptr };  // HUM / PRESS / CLOUD / FOG
lv_obj_t* s_statCap[4]  = { nullptr, nullptr, nullptr, nullptr };
lv_obj_t* s_statUnit[4] = { nullptr, nullptr, nullptr, nullptr };
// TRACK card
lv_obj_t* s_trkVal    = nullptr; lv_obj_t* s_trkDeg = nullptr;
lv_obj_t* s_wetVal    = nullptr;
lv_obj_t* s_wetSeg[7] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
lv_obj_t* s_gripVal   = nullptr;
lv_obj_t* s_gripSeg[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
// FORECAST strip
lv_obj_t* s_fcCap = nullptr; lv_obj_t* s_fcNa = nullptr;
lv_obj_t* s_fcAxis[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };  // NOW..+120
lv_obj_t* s_fcG[kFcGlyphs]      = { nullptr };
lv_obj_t* s_fcSun[kFcGlyphs]    = { nullptr };
lv_obj_t* s_fcCloudA[kFcGlyphs] = { nullptr };
lv_obj_t* s_fcCloudB[kFcGlyphs] = { nullptr };
lv_obj_t* s_fcDrop[kFcGlyphs][2] = { { nullptr } };
lv_obj_t* s_fcRing[kFcGlyphs]   = { nullptr };  // beyond-the-horizon marker

lv_point_precise_t s_arrowPts[6];
// Shared drop shape — a slightly slanted stroke (wind-driven rain, not a
// mechanical curtain — the ConditionsBadge move). lv_line keeps a POINTER
// to these, so they live at file scope.
lv_point_precise_t s_dropShape[2] = { { 0, 0 }, { 3, 15 } };

// ---- change caches --------------------------------------------------------------------
uint32_t s_txtMs = 0;
uint32_t s_maskShown = 0xFFFFFFFF;   // force first paint
int  s_airPrev = INT32_MIN, s_trkPrev = INT32_MIN, s_windPrev = INT32_MIN;
int  s_dirPrev = INT32_MIN;
int  s_wetPrev = -1, s_gripPrev = -1;
// Forecast caches: each glyph's time offset (minutes ahead), glided x, and
// last-rendered condition class.
float s_fcT[kFcGlyphs]; float s_fcX[kFcGlyphs];
int   s_fcClass[kFcGlyphs] = { -1, -1, -1, -1, -1 };
int   s_fcMinPrev = -1;
bool  s_fcShown = true;   // force first paint
int  s_statPrev[4] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
char s_clockPrev[8] = "";
// Sky caches — the full-screen gradient restyles only when these move.
int      s_skyMinShown = -2, s_ovcShown = -1, s_prcShown = -1;
float    s_starVis = 0;
int      s_dropsActive = 0;
uint32_t s_dropMs = 0;
uint32_t s_headerInk = kInk;   // flips dark over a bright (overcast noon) sky
uint32_t s_lcg = 0x2f6e2b1u;   // deterministic layout/spawn randomness

uint32_t rnd() { s_lcg = s_lcg * 1664525u + 1013904223u; return s_lcg >> 8; }

bool avail(uint32_t mask, int bit) { return (mask & (1u << bit)) != 0; }

void setWindArrow(int deg) {
  // Arrow shows where the wind blows TOWARD (dir + 180, matching the
  // recorder's Diagnostic arrow). Shaft through the ring center + two barbs.
  const float a  = (float)(((deg + 180) % 360)) * 0.0174533f;
  const float cx = kRingD / 2.0f, cy = kRingD / 2.0f, r = 52;
  const float tx = cx + r * sinf(a),  ty = cy - r * cosf(a);
  const float bx = cx - r * sinf(a),  by = cy + r * cosf(a);
  const float ba1 = a + 2.65f, ba2 = a - 2.65f, br_ = 16;
  s_arrowPts[0] = { (lv_value_precise_t)bx, (lv_value_precise_t)by };
  s_arrowPts[1] = { (lv_value_precise_t)tx, (lv_value_precise_t)ty };
  s_arrowPts[2] = { (lv_value_precise_t)(tx + br_ * sinf(ba1)), (lv_value_precise_t)(ty - br_ * cosf(ba1)) };
  s_arrowPts[3] = { (lv_value_precise_t)tx, (lv_value_precise_t)ty };
  s_arrowPts[4] = { (lv_value_precise_t)(tx + br_ * sinf(ba2)), (lv_value_precise_t)(ty - br_ * cosf(ba2)) };
  lv_line_set_points(s_windArrow, s_arrowPts, 5);
}

// Hero temperature: mono digits + a Montserrat degree unit re-hung off the
// digits' right edge after every text change (digit count varies).
void alignHeroDeg(lv_obj_t* val, lv_obj_t* deg) {
  lv_obj_update_layout(val);
  lv_obj_align_to(deg, val, LV_ALIGN_OUT_RIGHT_TOP, 6, 10);
}

// Frosted glass was tried here (LVGL 9.5 backdrop blur) and WALKED BACK:
// software-blurring the backdrop re-runs on every invalidated region under a
// card, and the rain field invalidates constantly — the page crawled. Flat
// translucency is the perf-honest material at 720p; the waiting modal keeps
// its frost (small, static backdrop). Revisit only with a hardware blur.
void applyFrost(lv_obj_t* card) { (void)card; }

// ---- the living sky -------------------------------------------------------------------
struct Sky { uint32_t top, bot; };

Sky mixSky(const Sky& a, const Sky& b, float t) {
  return { lerp32(a.top, b.top, t), lerp32(a.bot, b.bot, t) };
}

// The ConditionsBadge sky vocabulary (apps/web ConditionsBadge.css) — the
// canonical phase palette now shared by the web badge, the recorder's
// diagnostic weather page, and this panel. Phase thresholds use the same
// racer clock vocabulary as the badge (dawn 5-7, golden 7-9, day 9-17,
// golden 17-19, dusk 19-21, night otherwise), interpolated between stops
// so the sky drifts through phases instead of snapping.
constexpr Sky kSkyNight = { 0x131c38, 0x103258 };
constexpr Sky kSkyDawn  = { 0x585a96, 0xd28a86 };
constexpr Sky kSkyGold  = { 0xb1532f, 0xe0904a };
constexpr Sky kSkyDay   = { 0x1c63a8, 0x3f8ed0 };
constexpr Sky kSkyDusk  = { 0x46366b, 0xb5536a };
constexpr Sky kSkyOvc   = { 0x495462, 0x6a7583 };
constexpr Sky kSkyRain  = { 0x324356, 0x4a5c6d };

struct SkyStop { float h; Sky s; };
constexpr SkyStop kSkyStops[] = {
  { 0.0f,  kSkyNight }, { 5.0f,   kSkyNight }, { 6.25f, kSkyDawn },
  { 8.0f,  kSkyGold },  { 10.0f,  kSkyDay },   { 16.5f, kSkyDay },
  { 18.0f, kSkyGold },  { 19.75f, kSkyDusk },  { 21.5f, kSkyNight },
  { 24.0f, kSkyNight },
};

// Sky for an in-sim hour + cover + rain. Cover pulls toward overcast
// slate; rain dominates the hue outright (the badge rule) — beyond a
// light shower the sky reads rain-slate no matter what the clock says.
Sky skyGrad(float hour, float ovc, float rain01) {
  const int n = (int)(sizeof(kSkyStops) / sizeof(kSkyStops[0]));
  int i = 0;
  while (i < n - 2 && kSkyStops[i + 1].h <= hour) i++;
  const float span = kSkyStops[i + 1].h - kSkyStops[i].h;
  const float t = span > 0 ? clampf((hour - kSkyStops[i].h) / span, 0, 1) : 0;
  Sky s = mixSky(kSkyStops[i].s, kSkyStops[i + 1].s, t);
  s = mixSky(s, kSkyOvc, ovc * 0.7f);
  s = mixSky(s, kSkyRain, clampf(rain01 * 4.0f, 0, 1) * 0.85f);
  return s;
}

int16_t dropSpawnX() { return (int16_t)(6 + rnd() % 1268); }   // full page width

// Restyle the whole sky layer. Called ONLY when an input steps (in-sim
// minute, 5% cover, 5% precip) — it invalidates the full screen.
void updateSky(int minOfDay, int ovcPct, int precipPct) {
  const bool  skyOn = minOfDay >= 0;
  const float t     = minOfDay / 60.0f;
  const float ovc   = ovcPct / 100.0f;
  const float elev  = skyOn ? sinf((t - 6.0f) * (3.14159265f / 12.0f)) : 0.0f;

  const Sky sky = skyOn ? skyGrad(t, ovc, precipPct / 100.0f) : Sky{ kBg, kBg };
  lv_obj_set_style_bg_color(s_scr, hex(sky.top), 0);
  lv_obj_set_style_bg_grad_color(s_scr, hex(sky.bot), 0);

  // Header ink: white fails on a bright overcast noon — flip dark when the
  // top-of-sky luminance says so. (Bright sky implies skyOn, so the clock is
  // lit whenever this repaints it.)
  const int lum = (((sky.top >> 16) & 0xff) * 3 + ((sky.top >> 8) & 0xff) * 6 + (sky.top & 0xff)) / 10;
  s_headerInk = lum > 118 ? 0x14161au : kInk;
  lv_obj_set_style_text_color(s_title, hex(s_headerInk), 0);
  if (skyOn) lv_obj_set_style_text_color(s_clock, hex(s_headerInk), 0);

  // Stars: only in a dark, reasonably clear sky. Opacity is animated by the
  // twinkle pass; here we just gate visibility.
  const float dark = elev < 0 ? clampf(-elev / 0.12f, 0, 1) : 0;
  s_starVis = skyOn ? dark * (1.0f - ovc) : 0;
  for (int i = 0; i < kStars; i++) setHidden(s_star[i], s_starVis < 0.02f);

  // Sun: tracks the day across the header strip, fades near the horizon and
  // behind heavy cover.
  const bool sunOn = skyOn && elev > 0.03f && ovc < 0.65f;
  setHidden(s_sun, !sunOn);
  if (sunOn) {
    const float p = clampf((t - 6.0f) / 12.0f, 0, 1);
    lv_obj_set_pos(s_sun, 130 + (int)(p * (980 - 130)), 22);
    const float f = clampf(elev / 0.18f, 0, 1) * (1.0f - ovc);
    lv_obj_set_style_bg_opa(s_sun, (lv_opa_t)(90 + 165 * f), 0);
    lv_obj_set_style_shadow_opa(s_sun, (lv_opa_t)(130 * f), 0);
  }

  // Moon: mirrors the sun's path through the night hours.
  const bool moonOn = skyOn && elev < -0.08f && ovc < 0.8f;
  setHidden(s_moon, !moonOn);
  if (moonOn) {
    const float tn = t < 12.0f ? t + 24.0f : t;
    const float p  = clampf((tn - 19.0f) / 10.0f, 0, 1);   // 19:00 -> 05:00
    lv_obj_set_pos(s_moon, 160 + (int)(p * (980 - 160)), 26);
    const lv_opa_t o = (lv_opa_t)(240 * (1.0f - 0.5f * ovc));
    lv_obj_set_style_bg_opa(s_moonPart[0], o, 0);
  }

  // Cloud: shown for rain or heavy cover; day-lit white to night slate, and
  // darkened further the harder it rains.
  const bool cloudOn = skyOn && (precipPct > 0 || ovc >= 0.6f);
  setHidden(s_cloud, !cloudOn);
  if (cloudOn) {
    const float day = clampf((elev + 0.12f) / 0.3f, 0, 1);
    uint32_t c = lerp32(0x3a414d, 0xeff2f7, day);
    c = lerp32(c, 0x77808d, clampf(precipPct / 70.0f, 0, 1));
    for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(s_cloudPart[i], hex(c), 0);
  }

  // Card translucency rides the sky's brightness: over a dark sky the cards
  // dissolve so stars and gradient show through; a bright overcast noon
  // firms them back up so the white readouts keep their contrast. Idle
  // (no sky) goes near-opaque — the classic dark-mode look.
  const int lumB = (((sky.bot >> 16) & 0xff) * 3 + ((sky.bot >> 8) & 0xff) * 6 + (sky.bot & 0xff)) / 10;
  const int lumAvg = (lum + lumB) / 2;
  const lv_opa_t cardOpa = skyOn ? (lv_opa_t)(150 + lumAvg * 60 / 255) : 235;
  const lv_opa_t tileOpa = skyOn ? (lv_opa_t)(140 + lumAvg * 55 / 255) : 255;
  for (int i = 0; i < 3; i++) lv_obj_set_style_bg_opa(s_card[i], cardOpa, 0);
  for (int i = 0; i < 6; i++) lv_obj_set_style_bg_opa(s_tile[i], tileOpa, 0);

  // Rain drops — the ConditionsBadge intensity model: count, fall speed,
  // and opacity all scale with precipitation, with a severity factor
  // that is 0 at/below light rain (15%) and 1 at storm, so the drama
  // arrives beyond the perceived "light rain" threshold. Activation
  // staggers each drop down the fall so a shower never starts as a
  // marching row.
  const float strength = precipPct / 100.0f;
  const float sev = clampf((strength - 0.15f) / 0.85f, 0, 1);
  int want = (skyOn && precipPct > 0)
           ? (int)((5.0f + strength * 6.0f) * (1.0f + sev * 0.4f)) : 0;
  if (want > kDrops) want = kDrops;
  s_dropsActive = want;
  const lv_opa_t dropOpa = (lv_opa_t)(255.0f * (0.25f + 0.45f * strength));
  for (int i = 0; i < kDrops; i++) {
    const bool on = i < s_dropsActive;
    if (on) {
      if (lv_obj_has_flag(s_drop[i], LV_OBJ_FLAG_HIDDEN)) {
        s_dropX[i] = dropSpawnX();
        s_dropY[i] = (float)(rnd() % 700);
        lv_obj_set_pos(s_drop[i], s_dropX[i], (int)s_dropY[i]);
      }
      // Per-drop speed variation (±15%) on a ~280 px/s base, sped up by
      // storm severity — the badge's velocity model, tuned down so the
      // ~30 Hz position updates step ≤ ~14 px (silky, not strobed).
      s_dropV[i] = (uint16_t)((240 + rnd() % 84) * (1.0f + sev * 0.4f));
      lv_obj_set_style_line_opa(s_drop[i], dropOpa, 0);
    }
    setHidden(s_drop[i], !on);
  }
}

} // namespace

lv_obj_t* p4weather_setup() {
  lv_obj_t* scr = lv_obj_create(NULL);   // own screen — the deck pager loads it
  s_scr = scr;
  lv_obj_set_style_bg_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_grad_color(scr, hex(kBg), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // --- sky layer (bottom of the stack) --------------------------------------------
  // Stars: two thirds seeded into the header strip where sky is fully
  // exposed, the rest scattered (visible in margins + faintly through cards).
  for (int i = 0; i < kStars; i++) {
    const int sz = 2 + (int)(rnd() % 2);
    s_star[i] = makeBox(scr, sz, sz, kInk, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_star[i], (int)(rnd() % 1276), (i % 3) ? (int)(rnd() % 92) : (int)(rnd() % 700));
    s_starBase[i] = 110 + (uint8_t)(rnd() % 120);
    s_starPh[i]   = (uint8_t)(rnd() % 251);
    lv_obj_add_flag(s_star[i], LV_OBJ_FLAG_HIDDEN);
  }
  // Moon: soft disc + two craters.
  s_moon = lv_obj_create(scr);
  lv_obj_remove_style_all(s_moon);
  lv_obj_set_size(s_moon, 44, 44);
  lv_obj_remove_flag(s_moon, LV_OBJ_FLAG_SCROLLABLE);
  s_moonPart[0] = makeBox(s_moon, 44, 44, 0xe8ebf4, LV_RADIUS_CIRCLE);
  s_moonPart[1] = makeBox(s_moon, 9, 9, 0xc7ccd9, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_moonPart[1], 10, 12);
  s_moonPart[2] = makeBox(s_moon, 6, 6, 0xc7ccd9, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_moonPart[2], 26, 24);
  lv_obj_set_style_bg_opa(s_moonPart[1], 140, 0);
  lv_obj_set_style_bg_opa(s_moonPart[2], 140, 0);
  lv_obj_add_flag(s_moon, LV_OBJ_FLAG_HIDDEN);
  // Sun: warm disc with a shadow-glow halo.
  s_sun = makeBox(scr, 64, 64, 0xffe9a0, LV_RADIUS_CIRCLE);
  lv_obj_set_style_shadow_color(s_sun, hex(0xffd76a), 0);
  lv_obj_set_style_shadow_width(s_sun, 46, 0);
  lv_obj_set_style_shadow_spread(s_sun, 6, 0);
  lv_obj_add_flag(s_sun, LV_OBJ_FLAG_HIDDEN);
  // Rain cloud: three lobes on a rounded base, header strip.
  s_cloud = lv_obj_create(scr);
  lv_obj_remove_style_all(s_cloud);
  lv_obj_set_size(s_cloud, 190, 84);
  lv_obj_set_pos(s_cloud, kCloudX, kCloudY);
  lv_obj_remove_flag(s_cloud, LV_OBJ_FLAG_SCROLLABLE);
  s_cloudPart[0] = makeBox(s_cloud, 52, 52, 0xeff2f7, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_cloudPart[0], 2, 22);
  s_cloudPart[1] = makeBox(s_cloud, 68, 68, 0xeff2f7, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_cloudPart[1], 38, 2);
  s_cloudPart[2] = makeBox(s_cloud, 56, 56, 0xeff2f7, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(s_cloudPart[2], 92, 18);
  s_cloudPart[3] = makeBox(s_cloud, 172, 36, 0xeff2f7, 18);
  lv_obj_set_pos(s_cloudPart[3], 8, 44);
  lv_obj_add_flag(s_cloud, LV_OBJ_FLAG_HIDDEN);
  // Rain drops — part of the sky layer, BEHIND every card and readout
  // (the ConditionsBadge model: rain is ambient atmosphere, not
  // foreground). Fully visible in the header strip, gutters, and
  // margins; a faint presence through the translucent cards. Slanted
  // strokes; count/speed/opacity scale with intensity in updateSky.
  for (int i = 0; i < kDrops; i++) {
    s_drop[i] = lv_line_create(scr);
    lv_line_set_points(s_drop[i], s_dropShape, 2);
    lv_obj_set_style_line_width(s_drop[i], 2, 0);
    lv_obj_set_style_line_rounded(s_drop[i], true, 0);
    lv_obj_set_style_line_color(s_drop[i], hex(0xdfeaff), 0);
    lv_obj_set_style_line_opa(s_drop[i], 150, 0);
    lv_obj_add_flag(s_drop[i], LV_OBJ_FLAG_HIDDEN);
  }

  // --- header: large title + in-sim clock -----------------------------------------
  s_title = makeCaption(scr, &geist_mono_22, kInk, "WEATHER", 3);
  lv_obj_set_pos(s_title, kM + 4, 40);
  s_clock = makeLabel(scr, &geist_mono_time_48, kMiss, "--:--");
  lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -(kM + 4), 24);

  // ================= AIR card =====================================================
  lv_obj_t* air = makeBox(scr, kAirW, kCardH, kCard, 24);
  lv_obj_set_pos(air, kAirX, kCardY);
  s_card[0] = air;
  lv_obj_set_style_bg_opa(air, 235, 0);   // sky-adaptive — updateSky retunes this
  applyFrost(air);
  lv_obj_t* airCap = makeCaption(air, &geist_sans_28, kInk2, "AIR", 2);
  lv_obj_set_pos(airCap, kPad, 18);

  // Hero: ambient temperature.
  s_airVal = makeLabel(air, &geist_mono_96, kMiss, "-");
  lv_obj_set_pos(s_airVal, kPad, 84);
  s_airDeg = makeLabel(air, &geist_sans_40, kMiss, "\xC2\xB0""C");
  alignHeroDeg(s_airVal, s_airDeg);

  // Wind compass: ring + cardinal marks + through-arrow, speed hung below.
  s_windCap = makeCaption(air, &geist_sans_24, kMiss, "WIND", 1);
  lv_obj_set_width(s_windCap, kRingD);
  lv_obj_set_style_text_align(s_windCap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_windCap, kRingX, 20);
  s_windRing = lv_obj_create(air);
  lv_obj_remove_style_all(s_windRing);
  lv_obj_set_size(s_windRing, kRingD, kRingD);
  lv_obj_set_pos(s_windRing, kRingX, kRingY);
  lv_obj_set_style_radius(s_windRing, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(s_windRing, 2, 0);
  lv_obj_set_style_border_color(s_windRing, hex(kWell), 0);
  lv_obj_remove_flag(s_windRing, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_windRing, LV_OBJ_FLAG_CLICKABLE);
  const char* cardTxt[4] = { "N", "E", "S", "W" };
  const lv_align_t cardAlign[4] = { LV_ALIGN_TOP_MID, LV_ALIGN_RIGHT_MID,
                                    LV_ALIGN_BOTTOM_MID, LV_ALIGN_LEFT_MID };
  const int cardDx[4] = { 0, -12, 0, 12 }, cardDy[4] = { 10, 0, -10, 0 };
  for (int i = 0; i < 4; i++) {
    s_cardinal[i] = makeLabel(s_windRing, &geist_sans_16, kMiss, cardTxt[i]);
    lv_obj_align(s_cardinal[i], cardAlign[i], cardDx[i], cardDy[i]);
  }
  s_windArrow = lv_line_create(s_windRing);
  lv_obj_set_pos(s_windArrow, 0, 0);
  lv_obj_set_style_line_width(s_windArrow, 6, 0);
  lv_obj_set_style_line_rounded(s_windArrow, true, 0);
  lv_obj_set_style_line_color(s_windArrow, hex(kMiss), 0);
  setWindArrow(0);
  s_windVal = makeLabel(air, &geist_mono_time_48, kMiss, "--");
  s_windUnit = makeLabel(air, &geist_sans_28, kMiss, "km/h");
  lv_obj_align_to(s_windVal, s_windRing, LV_ALIGN_OUT_BOTTOM_MID, -34, 8);
  lv_obj_align_to(s_windUnit, s_windVal, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

  // Small-stat tiles: humidity / pressure / cloud / fog.
  const char* statTitles[4] = { "HUMIDITY", "PRESSURE", "CLOUD", "FOG" };
  const char* statUnits[4]  = { "%", "hPa", "%", "%" };
  for (int i = 0; i < 4; i++) {
    lv_obj_t* t = makeBox(air, 174, kRowBH, kInset, 16);
    lv_obj_set_pos(t, kPad + i * 186, kRowBY);
    s_tile[i] = t;
    s_statCap[i] = makeCaption(t, &geist_sans_24, kMiss, statTitles[i], 1);
    lv_obj_set_pos(s_statCap[i], 14, 10);
    s_statVal[i] = makeLabel(t, &geist_mono_time_36, kMiss, "--");
    lv_obj_set_pos(s_statVal[i], 14, 84);
    s_statUnit[i] = makeLabel(t, &geist_sans_28, kMiss, statUnits[i]);
    lv_obj_align_to(s_statUnit[i], s_statVal[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);
  }

  // ================= TRACK card ===================================================
  lv_obj_t* trk = makeBox(scr, kTrkW, kCardH, kCard, 24);
  lv_obj_set_pos(trk, kTrkX, kCardY);
  s_card[1] = trk;
  lv_obj_set_style_bg_opa(trk, 235, 0);   // sky-adaptive — updateSky retunes this
  applyFrost(trk);
  lv_obj_t* trkCap = makeCaption(trk, &geist_sans_28, kInk2, "TRACK", 2);
  lv_obj_set_pos(trkCap, kPad, 18);

  // Hero: surface temperature (heat-tinted when hot).
  s_trkVal = makeLabel(trk, &geist_mono_96, kMiss, "-");
  lv_obj_set_pos(s_trkVal, kPad, 84);
  s_trkDeg = makeLabel(trk, &geist_sans_40, kMiss, "\xC2\xB0""C");
  alignHeroDeg(s_trkVal, s_trkDeg);

  // Surface wetness: caption-less — the word IS the tile ("Mostly Dry"),
  // over a 7-step segment gauge (canonical 1..7 scale). (n/a) when unsent.
  lv_obj_t* surf = makeBox(trk, kTrkW - 2 * kPad, kTileH2, kInset, 16);
  lv_obj_set_pos(surf, kPad, kSurfY);
  s_tile[4] = surf;
  s_wetVal = makeLabel(surf, &geist_sans_40, kMiss, "(n/a)");
  lv_obj_set_pos(s_wetVal, 14, 8);
  for (int i = 0; i < 7; i++) {
    s_wetSeg[i] = makeBox(surf, 43, 8, kWell, 4);
    lv_obj_set_pos(s_wetSeg[i], 14 + i * 49, 58);
  }

  // Grip / rubbering: caption-less word (Green .. Saturated) + 5-step gauge.
  lv_obj_t* grip = makeBox(trk, kTrkW - 2 * kPad, kTileH2, kInset, 16);
  lv_obj_set_pos(grip, kPad, kGripY);
  s_tile[5] = grip;
  s_gripVal = makeLabel(grip, &geist_sans_40, kMiss, "(n/a)");
  lv_obj_set_pos(s_gripVal, 14, 8);
  for (int i = 0; i < 5; i++) {
    s_gripSeg[i] = makeBox(grip, 63, 8, kWell, 4);
    lv_obj_set_pos(s_gripSeg[i], 14 + i * 69, 58);
  }

  // ================= FORECAST strip ===============================================
  // NOW at the left, +35 minutes at the right. Sample glyphs ride the axis
  // and drift LEFT as in-sim time passes; each re-samples the live
  // now/+10'/+30' forecast at its own drifting offset (the +10/+30 channels
  // are ACC's — without any rain channel the strip shows (n/a)).
  lv_obj_t* fc = makeBox(scr, kFcW, kFcH, kCard, 24);
  lv_obj_set_pos(fc, kM, kFcY);
  s_card[2] = fc;
  applyFrost(fc);
  s_fcCap = makeCaption(fc, &geist_sans_24, kInk2, "FORECAST", 1);
  lv_obj_set_pos(s_fcCap, kPad, 10);
  const char* axisTxt[5] = { "NOW", "+30 min", "+60 min", "+90 min", "+120 min" };
  for (int i = 0; i < 5; i++) {
    s_fcAxis[i] = makeCaption(fc, &geist_sans_32, kMiss, axisTxt[i], 1);
    lv_obj_set_width(s_fcAxis[i], 140);
    lv_obj_set_style_text_align(s_fcAxis[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_fcAxis[i], kFcX0 + i * 30 * kFcPxPerMin - 70, 96);
  }
  s_fcNa = makeLabel(fc, &geist_sans_32, kMiss, "(n/a)");
  lv_obj_align(s_fcNa, LV_ALIGN_CENTER, 0, 6);
  lv_obj_add_flag(s_fcNa, LV_OBJ_FLAG_HIDDEN);
  for (int g = 0; g < kFcGlyphs; g++) {
    lv_obj_t* box = lv_obj_create(fc);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 76, 64);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    s_fcG[g] = box;
    s_fcSun[g] = makeBox(box, 30, 30, 0xffd60a, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_fcSun[g], 23, 14);
    s_fcCloudA[g] = makeBox(box, 34, 34, 0xd7dce4, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_fcCloudA[g], 13, 4);
    s_fcCloudB[g] = makeBox(box, 56, 20, 0xd7dce4, 10);
    lv_obj_set_pos(s_fcCloudB[g], 8, 22);
    for (int d = 0; d < 2; d++) {
      s_fcDrop[g][d] = makeBox(box, 4, 12, kBlue, 2);
      lv_obj_set_pos(s_fcDrop[g][d], 24 + d * 18, 46);
    }
    // Beyond-the-horizon marker: a dim hollow ring where a condition
    // can't honestly be claimed yet (the sim forecasts 30' out, no more).
    s_fcRing[g] = lv_obj_create(box);
    lv_obj_remove_style_all(s_fcRing[g]);
    lv_obj_set_size(s_fcRing[g], 24, 24);
    lv_obj_set_pos(s_fcRing[g], 26, 16);
    lv_obj_set_style_radius(s_fcRing[g], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_fcRing[g], 2, 0);
    lv_obj_set_style_border_color(s_fcRing[g], hex(kMiss), 0);
    lv_obj_remove_flag(s_fcRing[g], LV_OBJ_FLAG_SCROLLABLE);
    s_fcT[g] = g * 30.0f;   // one marker every 30 minutes; drift takes over
    s_fcX[g] = kFcX0 + s_fcT[g] * kFcPxPerMin;
    lv_obj_set_pos(box, (int)s_fcX[g] - 38, 26);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  }

  return scr;
}

void p4weather_tick(bool waitingForData) {
  const uint32_t nowMs = millis();

  // Rain-drop animation runs every loop lap (not throttled to 5 Hz — a
  // falling drop at 5 Hz is a slideshow). Scattered invalid regions are
  // what cost frames on this panel (every region re-walks the widget
  // tree), so the budget goes to a SMALLER field moving EVERY pass with
  // small steps — 14 smooth drops read better than 22 strobing ones.
  // (An alternating-halves scheme was tried first: half the regions,
  // but each drop stepped twice as far and the motion read as stutter.)
  if (s_dropsActive > 0) {
    const uint32_t dm = nowMs - s_dropMs;
    if (dm >= 16) {
      s_dropMs = nowMs;
      const float dt = (dm > 100 ? 100 : dm) * 0.001f;
      for (int i = 0; i < s_dropsActive; i++) {
        s_dropY[i] += s_dropV[i] * dt;
        if (s_dropY[i] > 724) {
          s_dropY[i] = -(float)(18 + rnd() % 80);   // rejoin above the sky
          s_dropX[i] = dropSpawnX();
        }
        lv_obj_set_pos(s_drop[i], s_dropX[i], (int)s_dropY[i]);
      }
    }
  }

  if (nowMs - s_txtMs < kTextMs) return;   // weather is slow; 5 Hz is plenty
  s_txtMs = nowMs;

  // Presence mask — the page's whole honesty story. While waiting, treat
  // everything as absent so every tile dims to the idle state.
  const uint32_t mask = waitingForData ? 0u : (uint32_t)tcStore.getInt(TC_CH_WEATHER_AVAIL);

  // --- group chrome that only depends on the mask -----------------------------------
  if (mask != s_maskShown) {
    const bool windOn = avail(mask, BitWind);
    lv_obj_set_style_text_color(s_windCap, hex(windOn ? kInk2 : kMiss), 0);
    lv_obj_set_style_border_color(s_windRing, hex(windOn ? kWell : kInset), 0);
    for (int i = 0; i < 4; i++)
      lv_obj_set_style_text_color(s_cardinal[i], hex(windOn ? kInk2 : kMiss), 0);
  }

  const char* tod = tcStore.getStr(TC_CH_TIME_OF_DAY);
  const bool clockOn = avail(mask, BitClock) && tod && tod[0];
  const char* clockTxt = clockOn ? tod : "--:--";
  int minOfDay = -1;   // in-sim minute — drives the sky AND the forecast drift
  { int hh = 0, mn = 0;
    if (clockOn && sscanf(tod, "%d:%d", &hh, &mn) == 2)
      minOfDay = (hh % 24) * 60 + (mn % 60); }

  // --- sky ------------------------------------------------------------------------
  // Quantized inputs: in-sim minute + 5% steps of cover/precip. The heavy
  // full-screen restyle in updateSky only runs when one of them moves.
  {
    int prc = avail(mask, BitPrecip) ? (int)tcStore.getInt(TC_CH_PRECIP_PCT) : 0;
    prc = prc < 0 ? 0 : prc > 100 ? 100 : prc;
    int oc = prc;
    if (avail(mask, BitCloud)) { const int v = (int)tcStore.getInt(TC_CH_CLOUD_PCT); if (v > oc) oc = v; }
    if (avail(mask, BitFog))   { const int v = (int)tcStore.getInt(TC_CH_FOG_PCT);   if (v > oc) oc = v; }
    oc = oc < 0 ? 0 : oc > 100 ? 100 : oc;
    const int ovcQ = (oc / 5) * 5, prcQ = (prc / 5) * 5;
    if (minOfDay != s_skyMinShown || ovcQ != s_ovcShown || prcQ != s_prcShown) {
      s_skyMinShown = minOfDay; s_ovcShown = ovcQ; s_prcShown = prcQ;
      updateSky(minOfDay, ovcQ, prcQ);
    }
    // Star twinkle: tiny per-star opacity waves, 5 Hz is plenty for a slow
    // shimmer. Touches only each star's few pixels.
    if (s_starVis > 0.02f) {
      const float ph = nowMs * 0.0026f;
      for (int i = 0; i < kStars; i++) {
        const float w = 0.55f + 0.45f * sinf(ph + s_starPh[i] * 0.0246f);
        lv_obj_set_style_bg_opa(s_star[i], (lv_opa_t)(s_starBase[i] * s_starVis * w), 0);
      }
    }
  }

  // --- header clock ---------------------------------------------------------------
  if (strncmp(clockTxt, s_clockPrev, sizeof(s_clockPrev) - 1) != 0 ||
      mask != s_maskShown) {
    strncpy(s_clockPrev, clockTxt, sizeof(s_clockPrev) - 1);
    lv_label_set_text(s_clock, clockTxt);
    lv_obj_set_style_text_color(s_clock, hex(clockOn ? s_headerInk : kMiss), 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -(kM + 4), 24);
  }

  // --- hero temperatures ------------------------------------------------------------
  const bool airOn = avail(mask, BitAirTemp);
  const int  airT  = (int)tcStore.getInt(TC_CH_AIR_TEMP);
  if (airT != s_airPrev || mask != s_maskShown) {
    s_airPrev = airT;
    char b[8];
    if (airOn) snprintf(b, sizeof(b), "%d", airT); else snprintf(b, sizeof(b), "-");
    lv_label_set_text(s_airVal, b);
    lv_obj_set_style_text_color(s_airVal, hex(airOn ? kInk : kMiss), 0);
    lv_obj_set_style_text_color(s_airDeg, hex(airOn ? kInk2 : kMiss), 0);
    alignHeroDeg(s_airVal, s_airDeg);
  }
  const bool trkOn = avail(mask, BitTrackTemp);
  const int  trkT  = (int)tcStore.getInt(TC_CH_TRACK_TEMP);
  if (trkT != s_trkPrev || mask != s_maskShown) {
    s_trkPrev = trkT;
    char b[8];
    if (trkOn) snprintf(b, sizeof(b), "%d", trkT); else snprintf(b, sizeof(b), "-");
    lv_label_set_text(s_trkVal, b);
    // Heat tint: white below 30, ramping to systemOrange at 45+.
    uint32_t c = kMiss;
    if (trkOn) c = (trkT >= 30) ? lerp32(kInk, kOrange, (trkT - 30) / 15.0f) : kInk;
    lv_obj_set_style_text_color(s_trkVal, hex(c), 0);
    lv_obj_set_style_text_color(s_trkDeg, hex(trkOn ? kInk2 : kMiss), 0);
    alignHeroDeg(s_trkVal, s_trkDeg);
  }

  // --- wind -----------------------------------------------------------------------
  const bool windOn = avail(mask, BitWind);
  const int  kmh    = (int)lroundf(tcStore.getFloat(TC_CH_WIND_KMH));
  const int  dirDeg = (int)tcStore.getInt(TC_CH_WIND_DIR_DEG);
  if (kmh != s_windPrev || dirDeg != s_dirPrev || mask != s_maskShown) {
    s_windPrev = kmh; s_dirPrev = dirDeg;
    char b[8];
    if (windOn) snprintf(b, sizeof(b), "%d", kmh); else snprintf(b, sizeof(b), "--");
    lv_label_set_text(s_windVal, b);
    lv_obj_set_style_text_color(s_windVal, hex(windOn ? kInk : kMiss), 0);
    lv_obj_set_style_text_color(s_windUnit, hex(windOn ? kInk2 : kMiss), 0);
    lv_obj_set_style_line_color(s_windArrow, hex(windOn ? kInk : kMiss), 0);
    if (windOn) setWindArrow(dirDeg);
    lv_obj_update_layout(s_windVal);
    lv_obj_align_to(s_windVal, s_windRing, LV_ALIGN_OUT_BOTTOM_MID, -34, 8);
    lv_obj_align_to(s_windUnit, s_windVal, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);
  }

  // --- forecast strip ---------------------------------------------------------------
  {
    const bool nowOn = avail(mask, BitPrecip);
    const bool fcOn  = nowOn || avail(mask, BitRain10) || avail(mask, BitRain30);
    if (fcOn != s_fcShown || mask != s_maskShown) {
      s_fcShown = fcOn;
      setHidden(s_fcNa, fcOn);
      lv_obj_set_style_text_color(s_fcCap, hex(fcOn ? kInk2 : kMiss), 0);
      for (int i = 0; i < 5; i++)
        lv_obj_set_style_text_color(s_fcAxis[i], hex(fcOn ? kInk2 : kMiss), 0);
      for (int g = 0; g < kFcGlyphs; g++) setHidden(s_fcG[g], !fcOn);
    }
    if (fcOn) {
      // Live forecast curve — gaps fill from the nearest published point.
      int pNow = nowOn ? (int)tcStore.getInt(TC_CH_PRECIP_PCT) : -1;
      int p10  = avail(mask, BitRain10) ? (int)tcStore.getInt(TC_CH_RAIN_10MIN_PCT) : -1;
      int p30  = avail(mask, BitRain30) ? (int)tcStore.getInt(TC_CH_RAIN_30MIN_PCT) : -1;
      if (pNow < 0) pNow = p10 >= 0 ? p10 : p30;
      if (p10 < 0)  p10  = pNow;
      if (p30 < 0)  p30  = p10;
      pNow = pNow < 0 ? 0 : pNow > 100 ? 100 : pNow;
      p10  = p10  < 0 ? 0 : p10  > 100 ? 100 : p10;
      p30  = p30  < 0 ? 0 : p30  > 100 ? 100 : p30;

      // Drift: each passed in-sim minute slides every glyph one step left.
      // A big jump (session change, midnight wrap) re-seeds instead.
      if (minOfDay >= 0) {
        if (s_fcMinPrev >= 0 && minOfDay != s_fcMinPrev) {
          const int dm = (minOfDay - s_fcMinPrev + 1440) % 1440;
          for (int g = 0; g < kFcGlyphs; g++) {
            if (dm <= 5) s_fcT[g] -= dm;
            else         s_fcT[g] = g * 30.0f;
          }
        }
        s_fcMinPrev = minOfDay;
      }
      for (int g = 0; g < kFcGlyphs; g++) {
        if (s_fcT[g] < -1.5f) s_fcT[g] += kFcSpanMin;   // exited left — rejoin right
        const float xt = kFcX0 + s_fcT[g] * kFcPxPerMin;
        s_fcX[g] += (xt - s_fcX[g]) * 0.25f;       // glide, don't jump
        if (fabsf(xt - s_fcX[g]) < 1) s_fcX[g] = xt;
        lv_obj_set_pos(s_fcG[g], (int)s_fcX[g] - 38, 26);
        // Sample the live curve at this glyph's drifting offset. Past the
        // sim's +30' horizon nothing can honestly be claimed — class 4
        // shows the hollow ring until the marker drifts into range.
        const float T = s_fcT[g] < 0 ? 0 : s_fcT[g];
        int cls;
        if (T > 30.5f) {
          cls = 4;                                  // beyond the horizon
        } else {
          float p;
          if (T <= 10) p = pNow + (p10 - pNow) * (T / 10.0f);
          else         p = p10 + (p30 - p10) * ((T - 10) / 20.0f);
          cls = p < 5 ? 0 : p < 40 ? 1 : p < 70 ? 2 : 3;  // sun/cloud/rain/heavy
        }
        if (cls != s_fcClass[g]) {
          s_fcClass[g] = cls;
          setHidden(s_fcSun[g], cls != 0);
          setHidden(s_fcCloudA[g], cls == 0 || cls == 4);
          setHidden(s_fcCloudB[g], cls == 0 || cls == 4);
          setHidden(s_fcDrop[g][0], cls < 2 || cls == 4);
          setHidden(s_fcDrop[g][1], cls < 3 || cls == 4);
          setHidden(s_fcRing[g], cls != 4);
          const uint32_t cc = cls == 3 ? 0x9aa4b2 : cls == 2 ? 0xbfc7d2 : 0xd7dce4;
          lv_obj_set_style_bg_color(s_fcCloudA[g], hex(cc), 0);
          lv_obj_set_style_bg_color(s_fcCloudB[g], hex(cc), 0);
        }
      }
    }
  }

  // --- surface: wetness word + 7-step gauge ---------------------------------------------
  const bool wetOn = avail(mask, BitWetness);
  const int  wet   = (int)tcStore.getInt(TC_CH_TRACK_WETNESS);
  if (wet != s_wetPrev || mask != s_maskShown) {
    s_wetPrev = wet;
    const int wi = (wet >= 0 && wet <= 7) ? wet : 0;
    lv_label_set_text(s_wetVal, wetOn ? kWetnessWord[wi] : "(n/a)");
    // Condition color: dry green, ramping to systemBlue, extremes red.
    uint32_t c = kMiss;
    if (wetOn) {
      if (wi == 0)      c = kInk;
      else if (wi <= 1) c = kGreen;
      else if (wi >= 7) c = kRed;
      else              c = lerp32(kGreen, kBlue, (wi - 1) / 5.0f);
    }
    lv_obj_set_style_text_color(s_wetVal, hex(c), 0);
    for (int i = 0; i < 7; i++)
      lv_obj_set_style_bg_color(s_wetSeg[i],
          hex((wetOn && wi > 0 && i < wi) ? c : kWell), 0);
  }

  // --- grip word + 5-step gauge ---------------------------------------------------------
  const bool gripOn = avail(mask, BitGrip);
  const int  grip   = (int)tcStore.getInt(TC_CH_TRACK_GRIP);
  if (grip != s_gripPrev || mask != s_maskShown) {
    s_gripPrev = grip;
    char b[16];
    if (!gripOn) snprintf(b, sizeof(b), "(n/a)");
    else if (grip >= 0 && grip <= 4) snprintf(b, sizeof(b), "%s", kGripWord[grip]);
    else snprintf(b, sizeof(b), "%d", grip);   // raw byte outside the label scale
    lv_label_set_text(s_gripVal, b);
    lv_obj_set_style_text_color(s_gripVal, hex(gripOn ? kInk : kMiss), 0);
    const int lit = (gripOn && grip >= 0 && grip <= 4) ? grip + 1 : 0;
    for (int i = 0; i < 5; i++)
      lv_obj_set_style_bg_color(s_gripSeg[i], hex(i < lit ? kGreen : kWell), 0);
  }

  // --- small stats: humidity / pressure / cloud / fog -----------------------------------
  const int statBits[4] = { BitHumidity, BitPressure, BitCloud, BitFog };
  for (int i = 0; i < 4; i++) {
    const bool on = avail(mask, statBits[i]);
    int v = 0;
    switch (i) {
      case 0: v = (int)tcStore.getInt(TC_CH_HUMIDITY_PCT); break;
      case 1: v = (int)lroundf(tcStore.getFloat(TC_CH_AIR_PRESSURE_HPA)); break;
      case 2: v = (int)tcStore.getInt(TC_CH_CLOUD_PCT); break;
      case 3: v = (int)tcStore.getInt(TC_CH_FOG_PCT); break;
    }
    if (v == s_statPrev[i] && mask == s_maskShown) continue;
    s_statPrev[i] = v;
    char b[8];
    if (on) snprintf(b, sizeof(b), "%d", v); else snprintf(b, sizeof(b), "--");
    lv_label_set_text(s_statVal[i], b);
    lv_obj_set_style_text_color(s_statVal[i], hex(on ? kInk : kMiss), 0);
    lv_obj_set_style_text_color(s_statCap[i], hex(on ? kInk2 : kMiss), 0);
    lv_obj_set_style_text_color(s_statUnit[i], hex(on ? kInk2 : kMiss), 0);
    lv_obj_update_layout(s_statVal[i]);
    lv_obj_align_to(s_statUnit[i], s_statVal[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);
  }

  s_maskShown = mask;
}
