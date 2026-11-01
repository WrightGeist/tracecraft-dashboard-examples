#pragma once
#include <Arduino.h>
#include <TraceCraftDisplay.h>

#include <Wire.h>                  // GT911 capacitive touch + ES8311 codec (shared I2C bus)
#include <ESP_I2S.h>               // audio out to the ES8311 (arduino-esp32 I2S driver)
#include "esp_ldo_regulator.h"     // MIPI DPHY power rail (on-chip LDO channel)
#include "esp_lcd_mipi_dsi.h"      // DSI bus + DBI io + DPI panel
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_hx8394.h"        // vendored Waveshare panel driver (lib/hx8394 — see its NOTICE.md)
#include "esp_cache.h"             // esp_cache_msync — commit CPU writes for the DPI DMA scan-out
#include "esp_heap_caps.h"
#include "driver/ppa.h"            // P4 Pixel-Processing Accelerator — hardware rotate into the FB
#include "esp_rom_sys.h"           // esp_rom_printf — wedge-hunt markers (TRACECRAFT_LOOP_WDT)

/**
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-5 — 5" 720x1280 IPS, HX8394 over MIPI-DSI
 * (2 lanes @ 700 Mbps), ESP32-P4 (32 MB in-package PSRAM, 32 MB flash).
 *
 * The panel is a DPI (video-mode) DSI display: the P4's DSI peripheral continuously
 * scans a full framebuffer out of PSRAM — there is no per-region "push" like an SPI
 * panel. And like the T-Display-S3-Long's controller, the raster is fixed native
 * PORTRAIT 720x1280; a landscape dash can't come from the panel. So this renderer
 * uses the same composition trick as boards/t_display_s3_long.h: LVGL lays out +
 * renders landscape 1280x720 (PARTIAL mode, small per-band buffers), and flush()
 * rotates each band into the portrait framebuffer.
 *
 * The rotation rides the P4's PPA (Pixel-Processing Accelerator): a hardware
 * scale-rotate-mirror engine that DMAs each band into the framebuffer already
 * rotated AND handles the cache/PSRAM coherency itself. Don't replace it with a
 * CPU transpose + whole-FB esp_cache_msync: the msync writeback walks the buffer
 * in portrait-row order — vertical lines on the landscape screen — while the DPI
 * engine keeps scanning, so every refresh drew a sweeping thin vertical tear line
 * through animated content, and the write-allocate cache traffic (64-byte line
 * fetches for 24-byte writes at a 1.4KB stride) made full-screen redraws crawl.
 * That was this renderer's first shipped implementation; it survives only as the
 * fallback below for the pathological case where the PPA rejects a transfer.
 *
 * Pin/timing facts (from Waveshare's BSP component, esp32_p4_wifi6_touch_lcd_5):
 *   backlight GPIO26 (LEDC PWM) · LCD reset GPIO27 (active HIGH) · DPHY power =
 *   on-chip LDO channel 3 @ 2500 mV · DPI 58 MHz RGB565 · GT911 touch on I2C 7/8
 *   (unused here — the SDK has no touch seam yet).
 *
 * If your unit shows the dash rotated 180° (USB on the wrong side), build with
 * -D TRACECRAFT_P4_LCD5_FLIP to spin the composition without touching the panel.
 */
namespace tracecraft {

// --- backlight ---------------------------------------------------------------
// LEDC PWM on GPIO26. Kept outside the renderer so board::setBrightness() is
// available to app code (the tap-to-dim control in TraceCraftApp.h) without going
// through the display seam.
constexpr int kP4Lcd5BacklightPin = 26;

inline uint8_t& p4Lcd5BrightnessRef() { static uint8_t pct = 100; return pct; }

inline void p4Lcd5SetBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  p4Lcd5BrightnessRef() = pct;
  ledcWrite(kP4Lcd5BacklightPin, (uint32_t)pct * 1023 / 100);
}

class WavesharePLcd5Renderer : public IRenderer {
  static constexpr int kNativeW = 720;    // portrait raster the DPI engine scans
  static constexpr int kNativeH = 1280;
  static constexpr int kRstPin  = 27;     // panel reset, active HIGH per Waveshare BSP

  static constexpr size_t kFbBytes = (size_t)kNativeW * kNativeH * 2;

  esp_lcd_panel_handle_t _panel = nullptr;
  uint16_t*              _fb    = nullptr;   // the DPI scan-out framebuffer (PSRAM)
  ppa_client_handle_t    _ppa   = nullptr;   // hardware rotate client (SRM operation)
  SemaphoreHandle_t _ppaDone   = nullptr;    // given by the PPA trans-done ISR
  bool _ppaDead    = false;                  // engine retired -> CPU path only
  uint8_t _ppaStrikes = 0;                   // consecutive lost completions
  SemaphoreHandle_t _frameDone  = nullptr;   // given by the DPI refresh-done ISR
  bool _vsyncOk    = false;                  // callback registered successfully
  bool _firstFlush = true;                   // next flush() starts a new refresh cycle

  static bool IRAM_ATTR onFrameDone(esp_lcd_panel_handle_t /*panel*/,
                                    esp_lcd_dpi_panel_event_data_t* /*edata*/, void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
  }

  static bool IRAM_ATTR onPpaDone(ppa_client_handle_t /*client*/,
                                  ppa_event_data_t* /*edata*/, void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
  }

  // Run one SRM op with a BOUNDED completion wait. The IDF PPA driver can
  // lose a completion under back-to-back band rotates (observed on IDF 5.5.4:
  // blocking-mode ppa_do_scale_rotate_mirror parks forever — loopTask blocked,
  // both CPUs idle; the deck's splash+verdict+telemetry wedge). Never block
  // the render loop on it unbounded again: 100 ms is ~6 frames late, i.e.
  // already broken — log loudly, retire the PPA for this boot, and let every
  // caller fall through to its CPU path. Degraded beats frozen. (A late
  // completion after the timeout may scribble one stale band — a one-frame
  // artifact on an already-retiring path.)
  bool srmRun(ppa_srm_oper_config_t& op) {
    if (!_ppa || _ppaDead || !_ppaDone) return false;
#ifdef TRACECRAFT_LOOP_WDT
    // Per-second path census on the UART: proves whether bands ride the PPA
    // or fall to CPU (o=PPA ok, f=submit fail, s=strike). Diagnosis builds.
    static uint32_t s_okN = 0, s_failN = 0, s_lastN = 0;
    const uint32_t nowN = millis();
    if (nowN - s_lastN >= 1000) {
      esp_rom_printf("[srm] ok=%u fail=%u strikes=%u\n",
                     (unsigned)s_okN, (unsigned)s_failN, (unsigned)_ppaStrikes);
      s_okN = s_failN = 0; s_lastN = nowN;
    }
#endif
    op.mode      = PPA_TRANS_MODE_NON_BLOCKING;
    op.user_data = _ppaDone;                        // routed to onPpaDone's user_data
    xSemaphoreTake(_ppaDone, 0);                    // clear any stale signal
    if (ppa_do_scale_rotate_mirror(_ppa, &op) != ESP_OK) {
#ifdef TRACECRAFT_LOOP_WDT
      s_failN++;
#endif
      return false;
    }
    if (xSemaphoreTake(_ppaDone, pdMS_TO_TICKS(100)) == pdTRUE) {
      _ppaStrikes = 0;                              // healthy op resets the count
#ifdef TRACECRAFT_LOOP_WDT
      s_okN++;
#endif
      return true;
    }
    // One lost completion falls back for THIS op only (a rare driver hiccup
    // shouldn't condemn the whole boot to CPU rendering); three in a row
    // means the engine is genuinely stuck — retire it.
    if (++_ppaStrikes >= 3) {
      _ppaDead = true;
      esp_rom_printf("[TraceCraft] PPA completion lost x3 - CPU rotate fallback engaged for this boot\n");
    } else {
      esp_rom_printf("[TraceCraft] PPA completion lost (strike %d)\n", _ppaStrikes);
    }
    return false;
  }

public:
  void init() override {
    // MIPI DPHY power: "No Power" -> "Shutdown" state (Waveshare BSP: LDO ch 3, 2.5 V).
    static esp_ldo_channel_handle_t phyPwr = nullptr;
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id    = 3;
    ldo.voltage_mv = 2500;
    esp_ldo_acquire_channel(&ldo, &phyPwr);

    // DSI bus (2 data lanes @ 700 Mbps — HX8394_PANEL_BUS_DSI_2CH_CONFIG).
    esp_lcd_dsi_bus_handle_t bus = nullptr;
    esp_lcd_dsi_bus_config_t busCfg = {};
    busCfg.bus_id             = 0;
    busCfg.num_data_lanes     = 2;
    busCfg.phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    busCfg.lane_bit_rate_mbps = 700;
    if (esp_lcd_new_dsi_bus(&busCfg, &bus) != ESP_OK) return;

    // DBI io — the command/parameter channel the init sequence goes down.
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_dbi_io_config_t dbiCfg = {};
    dbiCfg.virtual_channel = 0;
    dbiCfg.lcd_cmd_bits    = 8;
    dbiCfg.lcd_param_bits  = 8;
    if (esp_lcd_new_panel_io_dbi(bus, &dbiCfg, &io) != ESP_OK) return;

    // DPI (video-mode) config — HX8394_720_1280_PANEL_30HZ_DPI_CONFIG values,
    // spelled out because the macro's nested C designated initializers don't
    // compile in a C++ translation unit.
    esp_lcd_dpi_panel_config_t dpiCfg = {};
    dpiCfg.dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpiCfg.dpi_clock_freq_mhz = 58;
    dpiCfg.virtual_channel    = 0;
    dpiCfg.pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpiCfg.num_fbs            = 1;
    dpiCfg.video_timing.h_size            = 720;
    dpiCfg.video_timing.v_size            = 1280;
    dpiCfg.video_timing.hsync_back_porch  = 20;
    dpiCfg.video_timing.hsync_pulse_width = 20;
    dpiCfg.video_timing.hsync_front_porch = 40;
    dpiCfg.video_timing.vsync_back_porch  = 10;
    dpiCfg.video_timing.vsync_pulse_width = 4;
    dpiCfg.video_timing.vsync_front_porch = 24;
    dpiCfg.flags.use_dma2d = true;

    hx8394_vendor_config_t vendorCfg = {};
    vendorCfg.mipi_config.dsi_bus    = bus;
    vendorCfg.mipi_config.dpi_config = &dpiCfg;
    vendorCfg.mipi_config.lane_num   = 2;

    esp_lcd_panel_dev_config_t devCfg = {};
    devCfg.bits_per_pixel          = 16;
    devCfg.rgb_ele_order           = LCD_RGB_ELEMENT_ORDER_RGB;
    devCfg.reset_gpio_num          = kRstPin;
    devCfg.vendor_config           = &vendorCfg;
    devCfg.flags.reset_active_high = 1;              // per the Waveshare BSP

    if (esp_lcd_new_panel_hx8394(io, &devCfg, &_panel) != ESP_OK) return;
    esp_lcd_panel_reset(_panel);
    esp_lcd_panel_init(_panel);

    // Compose straight into the DPI scan-out buffer (PSRAM, cache-line aligned by
    // the driver). Clear it before the backlight comes up so boot shows black, not
    // whatever PSRAM woke up holding.
    esp_lcd_dpi_panel_get_frame_buffer(_panel, 1, (void**)&_fb);
    if (_fb) {
      memset(_fb, 0, kFbBytes);
      esp_cache_msync(_fb, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    ppa_client_config_t ppaCfg = {};
    ppaCfg.oper_type = PPA_OPERATION_SRM;
    // Queue headroom: srmRun submits NON_BLOCKING and a timed-out (lost)
    // transaction can leave its slot occupied — with the default depth of 1
    // a single loss would make every later submit fail. 4 slots ride through
    // the three-strike window.
    ppaCfg.max_pending_trans_num = 4;
    // Every failure here is LOUD (UART): a silent nullptr/_ppaDead at init
    // demotes the whole boot to CPU rendering and reads as "mystery
    // artifacts + slowness" — which is exactly how it shipped once.
    esp_err_t perr = ppa_register_client(&ppaCfg, &_ppa);
    if (perr != ESP_OK) {
      _ppa = nullptr;
      esp_rom_printf("[TraceCraft] PPA client register FAILED: 0x%x - CPU rendering\n", perr);
    }
    if (_ppa) {
      // Completion signal for the bounded wait in srmRun() — see its comment.
      _ppaDone = xSemaphoreCreateBinary();
      ppa_event_callbacks_t pcbs = {};
      pcbs.on_trans_done = onPpaDone;
      perr = _ppaDone ? ppa_client_register_event_callbacks(_ppa, &pcbs) : ESP_ERR_NO_MEM;
      if (perr != ESP_OK) {
        _ppaDead = true;   // no completion signal -> never submit, CPU path only
        esp_rom_printf("[TraceCraft] PPA callback register FAILED: 0x%x - CPU rendering\n", perr);
      }
    }

    // Vsync: the DPI engine scans continuously at the panel rate (~57 Hz); its
    // refresh-done interrupt is our frame boundary. Updates that BEGIN right
    // after it rarely collide with the scan position — see waitFrame()/flush().
    _frameDone = xSemaphoreCreateBinary();
    if (_frameDone) {
      esp_lcd_dpi_panel_event_callbacks_t cbs = {};
      cbs.on_refresh_done = onFrameDone;
      _vsyncOk = esp_lcd_dpi_panel_register_event_callbacks(_panel, &cbs, _frameDone) == ESP_OK;
    }

    ledcAttach(kP4Lcd5BacklightPin, 5000, 10);   // arduino-esp32 3.x LEDC API (pin-keyed)
    p4Lcd5SetBrightness(100);
  }

  // Landscape size LVGL renders at; flush() rotates each band into the portrait _fb.
  uint16_t width()  override { return kNativeH; }   // 1280
  uint16_t height() override { return kNativeW; }   // 720

  // Rotate one landscape band into the portrait framebuffer with the PPA. The band
  // buffer (internal DMA RAM) is the whole input picture; the output block lands in
  // the scan-out FB already rotated, coherent with the DPI engine's DMA reads (the
  // PPA driver does the cache maintenance on both buffers). PPA angles count
  // counterclockwise, so the default landscape mapping (lx,ly)->(719-ly,lx) is
  // ANGLE_270 and the FLIP build is ANGLE_90. No byte swap: PPA and DPI both use
  // little-endian RGB565, same as LVGL renders.
  // Frame-boundary waits, two semantics (both no-ops without the callback,
  // both timing out at ~1.5 frames so a hiccup can never wedge rendering):
  //
  // waitFrame — clears any pending signal FIRST, then waits for a fresh
  // boundary. For episodic work (LVGL's refresh gate): guarantees the update
  // starts right behind the scan head.
  //
  // nextFrame — takes the signal as-is: a boundary that fired while the caller
  // was busy compositing counts. For STREAMING loops (finger drag, slides) —
  // the clear-first version silently discards the boundary that elapses during
  // the ~3-6ms blit and waits a whole extra frame, quantizing a 57fps loop
  // down to ~25-30fps ("framey" scrolling).
  void waitFrame() {
    if (!_vsyncOk) return;
    xSemaphoreTake(_frameDone, 0);
    xSemaphoreTake(_frameDone, pdMS_TO_TICKS(26));
  }

  void nextFrame() {
    if (!_vsyncOk) return;
    xSemaphoreTake(_frameDone, pdMS_TO_TICKS(26));
  }

  void flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* px) override {
    if (!_fb) return;
    // Align each LVGL refresh cycle to the frame boundary: wait once, on the
    // FIRST band of the cycle (flushCommit re-arms). Bands then land in the
    // ~17ms window behind the scan position instead of racing it — the cheap
    // four-line version of tear-free (true double-buffered flips would be the
    // full fix if this ever proves insufficient).
    if (_firstFlush) { _firstFlush = false; waitFrame(); }
    if (_ppa) {
      ppa_srm_oper_config_t op = {};
      op.in.buffer         = px;
      op.in.pic_w          = w;
      op.in.pic_h          = h;
      op.in.block_w        = w;
      op.in.block_h        = h;
      op.in.block_offset_x = 0;
      op.in.block_offset_y = 0;
      op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
      op.out.buffer        = _fb;
      op.out.buffer_size   = kFbBytes;
      op.out.pic_w         = kNativeW;
      op.out.pic_h         = kNativeH;
      op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
#ifdef TRACECRAFT_P4_LCD5_FLIP
      op.out.block_offset_x = y;
      op.out.block_offset_y = kNativeH - x - w;
      op.rotation_angle     = PPA_SRM_ROTATION_ANGLE_90;
#else
      op.out.block_offset_x = kNativeW - y - h;
      op.out.block_offset_y = x;
      op.rotation_angle     = PPA_SRM_ROTATION_ANGLE_270;
#endif
      op.scale_x = 1.0f;
      op.scale_y = 1.0f;
      if (srmRun(op)) return;
    }
    // CPU fallback (PPA unavailable, retired, or rejected the block): transpose
    // by hand, then write back ONLY the portrait rows this band touched. The
    // old whole-framebuffer msync in flushCommit was a latent corruptor once
    // CPU and PPA bands could MIX in one refresh: it wrote back stale cached
    // copies of regions the PPA had since redrawn in RAM — thin-line
    // artifacts wherever the two paths interleaved. Outer loop walks band
    // COLUMNS because one landscape column is one contiguous run inside a
    // portrait framebuffer row — consecutive inner-loop writes share cache lines.
    for (uint16_t c = 0; c < w; c++) {
      const int lx = (int)x + c;                     // landscape X = portrait row
      if (lx < 0 || lx >= kNativeH) continue;
#ifdef TRACECRAFT_P4_LCD5_FLIP
      uint16_t* const rowStart = _fb + (size_t)(kNativeH - 1 - lx) * kNativeW + (int)y;
      uint16_t* dst = rowStart;
      const uint16_t* src = px + c;
      for (uint16_t r = 0; r < h; r++) { *dst++ = *src; src += w; }
#else
      uint16_t* const rowStart = _fb + (size_t)lx * kNativeW + (kNativeW - (int)y - (int)h);
      uint16_t* dst = _fb + (size_t)lx * kNativeW + (kNativeW - 1 - (int)y);
      const uint16_t* src = px + c;
      for (uint16_t r = 0; r < h; r++) { *dst-- = *src; src += w; }
#endif
      // Unaligned start/size is fine: the driver rounds out to cache lines,
      // and every byte in those lines within this row is CPU-authored.
      esp_cache_msync(rowStart, (size_t)h * 2,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
  }

  void flushCommit() override {
    _firstFlush = true;   // next flush() opens a new refresh cycle — re-arm vsync
  }

  // Inverse of the display rotation: copy the CURRENT framebuffer content out
  // as a landscape 1280x720 image (dst must be 1.8MB, cache-line aligned — a
  // plain heap_caps_malloc PSRAM block is fine). Lets a page transition reuse
  // the on-screen pixels (~3ms of PPA DMA) instead of re-rendering the whole
  // outgoing page through LVGL (~40ms). PPA path only — returns false without
  // it, and the caller falls back to an LVGL render.
  bool snapshotScreen(uint16_t* dst) {
    if (!_fb || !_ppa || !dst) return false;
    ppa_srm_oper_config_t op = {};
    op.in.buffer         = _fb;
    op.in.pic_w          = kNativeW;
    op.in.pic_h          = kNativeH;
    op.in.block_w        = kNativeW;
    op.in.block_h        = kNativeH;
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = dst;
    op.out.buffer_size   = kFbBytes;
    op.out.pic_w         = kNativeH;       // landscape
    op.out.pic_h         = kNativeW;
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
#ifdef TRACECRAFT_P4_LCD5_FLIP
    op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_270;   // inverse of the FLIP display rotation
#else
    op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_90;    // inverse of the default rotation
#endif
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    return srmRun(op);
  }

  // Rotate a full-height vertical slice of a landscape page image (1280x720
  // RGB565) into the framebuffer: source columns [srcX, srcX+w) land at screen
  // columns [dstX, dstX+w). Backs board::slideBlit() — see its comment. Same
  // rotation/offset math as flush() with y=0, h=720.
  void blitLandscapeSlice(const uint16_t* page, int srcX, int dstX, int w) {
    if (!_fb || !page || w <= 0) return;
    if (_ppa) {
      ppa_srm_oper_config_t op = {};
      op.in.buffer         = page;
      op.in.pic_w          = kNativeH;        // landscape width  (1280)
      op.in.pic_h          = kNativeW;        // landscape height (720)
      op.in.block_w        = w;
      op.in.block_h        = kNativeW;
      op.in.block_offset_x = srcX;
      op.in.block_offset_y = 0;
      op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
      op.out.buffer        = _fb;
      op.out.buffer_size   = kFbBytes;
      op.out.pic_w         = kNativeW;
      op.out.pic_h         = kNativeH;
      op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
#ifdef TRACECRAFT_P4_LCD5_FLIP
      op.out.block_offset_x = 0;
      op.out.block_offset_y = kNativeH - dstX - w;
      op.rotation_angle     = PPA_SRM_ROTATION_ANGLE_90;
#else
      op.out.block_offset_x = 0;
      op.out.block_offset_y = dstX;
      op.rotation_angle     = PPA_SRM_ROTATION_ANGLE_270;
#endif
      op.scale_x = 1.0f;
      op.scale_y = 1.0f;
      if (srmRun(op)) return;
    }
    // CPU fallback: per-column transpose + immediate cache writeback of the rows
    // we touched (slow, but only runs if the PPA rejected the transfer).
    for (int c = 0; c < w; c++) {
      const int lx = dstX + c;
      if (lx < 0 || lx >= kNativeH) continue;
      const uint16_t* src = page + srcX + c;
#ifdef TRACECRAFT_P4_LCD5_FLIP
      uint16_t* dst = _fb + (size_t)(kNativeH - 1 - lx) * kNativeW;
      for (int r = 0; r < kNativeW; r++) { *dst++ = *src; src += kNativeH; }
#else
      uint16_t* dst = _fb + (size_t)lx * kNativeW + (kNativeW - 1);
      for (int r = 0; r < kNativeW; r++) { *dst-- = *src; src += kNativeH; }
#endif
    }
    esp_cache_msync(_fb, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
};

// --- GT911 capacitive touch (I2C SDA=7 SCL=8; INT/RST not wired on this board) --
// Minimal tap reader — enough for the tap-zone controls (brightness) without
// vendoring the esp_lcd_touch component stack. The controller reports native
// PORTRAIT coordinates; board::touchTap() maps them through the same rotation
// as the renderer so app code works in landscape screen coords.
namespace p4gt911 {

struct State {
  bool     inited = false;
  uint8_t  addr   = 0;      // 0x5D or 0x14 (strap-dependent); 0 = not found
  volatile bool down = false;
  bool     swipeFired = false;       // this stroke already produced a swipe
  int16_t  startX = 0, startY = 0;   // landscape coords at touch-down
  int16_t  lastX  = 0, lastY  = 0;   // latest sample while held
  uint32_t lastT  = 0;               // ms of the latest sample (velocity estimate)
  volatile float velX = 0.0f;       // smoothed horizontal velocity, px/ms (read cross-task)
  // Live drag vector (relative to touch-down), written by the touch task each
  // sample and read cross-task by finger-tracked UI (the deck page drag).
  volatile int16_t liveDx = 0, liveDy = 0;
};
inline State& st() { static State s; return s; }

// Gesture events, produced on the touch task, consumed by board::touchGesture().
// kGestDown fires at every touch-down — consumers use it to do speculative work
// (the deck build pre-renders the incoming page snapshot) while the finger is
// still traveling, so a swipe that follows starts with zero visible latency.
enum : uint8_t { kGestTap = 1, kGestSwipeL = 2, kGestSwipeR = 3, kGestDown = 4 };
struct GestureEvent { uint8_t type; int16_t x, y; };
inline QueueHandle_t& gestureQueue() { static QueueHandle_t q = nullptr; return q; }

// Swipe commit distance. Low enough that a casual flick crosses it mid-stroke
// (discoverability), high enough that a sloppy tap can't; the 2:1 horizontal
// dominance test does the real disambiguation.
constexpr int kSwipePx = 80;

// A flick: a stroke that ends FAST — smoothed horizontal speed at least this, with
// enough travel to rule out a twitchy tap. Fast flicks lift within a couple of
// samples and their measured distance badly undershoots the physical stroke, so
// speed is what identifies them. The release classifier below and the deck's
// finger-locked drag (board::releasedSwipe) decide by these same numbers.
constexpr float kFlickPxPerMs = 0.6f;
constexpr int   kFlickMinPx   = 30;

inline bool readReg(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

inline void ackStatus(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)0x81);
  Wire.write((uint8_t)0x4E);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission(true);
}

inline void init() {
  State& s = st();
  s.inited = true;
  Wire.begin(7, 8, 400000);
  for (uint8_t a : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    uint8_t id[4] = {0};
    if (readReg(a, 0x8140, id, 4) && id[0] == '9') { s.addr = a; break; }   // "911"
  }
}

// TEMPORARY gesture instrumentation (TRACECRAFT_TOUCH_DEBUG): raw samples +
// classification decisions to the USB-CDC console. Cross-layer reach into the
// usb seam — debug-build only, stripped with the flag.
#ifdef TRACECRAFT_TOUCH_DEBUG
} } // close p4gt911 + tracecraft
#include "tracecraft/tc_hal.h"
namespace tracecraft { namespace usb { hal::ILog& log(); } }
namespace tracecraft { namespace p4gt911 {
#endif

// Touch sampling runs on its OWN task: the render loop can spend 20-35ms per
// lap (software render + the vsync wait), and gesture classification sampled
// from that loop sees a fast flick only 2-4 times — the threshold-crossing
// sample ends up being the release-adjacent one, so swipes felt like they
// fired on finger-lift. A dedicated task samples the GT911 at a real ~100 Hz
// regardless of render load. All Wire traffic stays on this task (codec init
// happens in setup(), before the task exists).
inline void touchTask(void*) {
  init();
  State& s = st();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(s.down ? 8 : 15));
    if (s.addr == 0) continue;
    uint8_t status = 0;
    if (!readReg(s.addr, 0x814E, &status, 1)) continue;
#ifdef TRACECRAFT_TOUCH_DEBUG
    if (status != 0 || s.down)
      usb::log().printf("[gt] t=%lu st=%02X\n", (unsigned long)millis(), status);
#endif
    if (!(status & 0x80)) continue;                     // no fresh scan yet
    const uint8_t count = status & 0x0F;
    GestureEvent ev{ 0, 0, 0 };
    if (count > 0) {
      uint8_t p[4];
      if (readReg(s.addr, 0x8150, p, 4)) {
        const uint16_t px = (uint16_t)(p[0] | (p[1] << 8));   // native portrait
        const uint16_t py = (uint16_t)(p[2] | (p[3] << 8));
#ifdef TRACECRAFT_P4_LCD5_FLIP
        const int16_t lx = (int16_t)(1279 - py), ly = (int16_t)px;
#else
        const int16_t lx = (int16_t)py, ly = (int16_t)(719 - px);
#endif
        const uint32_t nowT = millis();
        if (!s.down) {
          s.down = true; s.swipeFired = false;
          s.startX = lx; s.startY = ly;
          s.velX = 0.0f; s.lastT = nowT;
          GestureEvent down{ kGestDown, lx, ly };
          xQueueSend(gestureQueue(), &down, 0);
        } else if (nowT > s.lastT) {
          // Smoothed horizontal velocity (px/ms) — fast flicks lift within a
          // couple of samples and their measured DISTANCE badly undershoots,
          // so release classification needs speed, not just travel.
          const float vx = (float)(lx - s.lastX) / (float)(nowT - s.lastT);
          s.velX = 0.6f * s.velX + 0.4f * vx;
          s.lastT = nowT;
        }
        s.lastX = lx; s.lastY = ly;
        s.liveDx = (int16_t)(lx - s.startX);
        s.liveDy = (int16_t)(ly - s.startY);
#ifdef TRACECRAFT_TOUCH_DEBUG
        usb::log().printf("[gt] t=%lu pt n=%u x=%d y=%d dx=%d\n",
                          (unsigned long)millis(), count, lx, ly, lx - s.startX);
#endif
        if (!s.swipeFired) {                            // fire MID-DRAG at the threshold
          const int dx = lx - s.startX, dy = ly - s.startY;
          const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
          if (adx >= kSwipePx && adx >= 2 * ady) {
            s.swipeFired = true;
            ev.type = dx < 0 ? kGestSwipeL : kGestSwipeR;
#ifdef TRACECRAFT_TOUCH_DEBUG
            usb::log().printf("[gt] t=%lu FIRE mid-drag adx=%d\n", (unsigned long)millis(), adx);
#endif
          }
        }
      }
    } else if (s.down) {                                // release
      s.down = false;
      const int dx = s.lastX - s.startX, dy = s.lastY - s.startY;
      const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
      // A stroke is a swipe on release if it traveled the commit distance OR
      // ended FAST (a flick: high smoothed velocity, horizontally dominant,
      // enough travel to rule out a twitchy tap). Distance alone misses quick
      // flicks entirely — the finger lifts within a couple of samples and the
      // measured travel badly undershoots the physical stroke (see velX).
      const float av = s.velX < 0 ? -s.velX : s.velX;
      const bool flick = av >= kFlickPxPerMs && adx >= kFlickMinPx && adx >= ady;
      if (!s.swipeFired && ((adx >= kSwipePx && adx >= 2 * ady) || flick)) {
        const bool goLeft = flick ? (s.velX < 0) : (dx < 0);
        ev.type = goLeft ? kGestSwipeL : kGestSwipeR;
#ifdef TRACECRAFT_TOUCH_DEBUG
        usb::log().printf("[gt] t=%lu FIRE on-release adx=%d v=%.2f\n",
                          (unsigned long)millis(), adx, s.velX);
#endif
      } else if (!s.swipeFired && adx < 16 && ady < 40) {
        ev.type = kGestTap;
        ev.x = s.startX; ev.y = s.startY;
      }
    }
    ackStatus(s.addr);
    if (ev.type) xQueueSend(gestureQueue(), &ev, 0);    // drop if the app is behind
  }
}

} // namespace p4gt911

// --- ES8311 codec + speaker (I2S BCLK=12 WS=10 DOUT=9 DIN=11, PA enable GPIO53) --
// Minimal playback-only bring-up for UI cues, transcribed from Espressif's es8311
// component (esp-bsp, Apache-2.0): slave mode, I2S 16-bit stereo @ 32 kHz, with
// the codec's master clock taken FROM THE BIT CLOCK (reg01 bit7) so the MCLK line
// isn't needed — BCLK at 32k x 16 x 2 = 1.024 MHz matches the driver's 32x
// coefficient row exactly. The codec shares the touch panel's I2C bus (addr 0x18).
namespace p4audio {

constexpr uint8_t kCodecAddr = 0x18;
constexpr int     kPaPin     = 53;      // speaker power-amp enable
constexpr uint32_t kRate     = 32000;

inline I2SClass& i2s() { static I2SClass i; return i; }

inline void wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kCodecAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission(true);
}

inline uint8_t rd(uint8_t reg) {
  Wire.beginTransmission(kCodecAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((int)kCodecAddr, 1) != 1) return 0;
  return (uint8_t)Wire.read();
}

inline bool init() {
  static bool inited = false, ok = false;
  if (inited) return ok;
  inited = true;

  Wire.begin(7, 8, 400000);
  Wire.beginTransmission(kCodecAddr);
  if (Wire.endTransmission(true) != 0) return false;   // codec absent — stay silent

  // Reset + power-on (es8311_init).
  wr(0x00, 0x1F); delay(20);
  wr(0x00, 0x00);
  wr(0x00, 0x80);
  // Clocks: all on, MCLK sourced from the BCLK pin (bit7).
  wr(0x01, 0xBF);
  // Divider row for mclk = 32 x fs (pre_div 1, pre_multi 3, adc/dac_div 1,
  // fs_mode 0, lrck 0x00ff, bclk_div 4, osr 0x10/0x10).
  wr(0x02, (uint8_t)((rd(0x02) & 0x07) | 0x18));
  wr(0x03, 0x10);
  wr(0x04, 0x10);
  wr(0x05, 0x00);
  wr(0x06, (uint8_t)((rd(0x06) & 0xC0) | 0x03));       // bclk_div 4, sclk not inverted
  wr(0x07, (uint8_t)(rd(0x07) & 0xC0));
  wr(0x08, 0xFF);
  // Format: slave, I2S, 16-bit in/out.
  wr(0x09, 0x0C);
  wr(0x0A, 0x0C);
  // Analog power-up + DAC path (verbatim from es8311_init).
  wr(0x0D, 0x01);
  wr(0x0E, 0x02);
  wr(0x12, 0x00);
  wr(0x13, 0x10);
  wr(0x1C, 0x6A);
  wr(0x37, 0x08);
  wr(0x32, 0xBF);                                      // DAC volume: 0 dB (clean maximum;
                                                       // higher values add digital gain + clipping)

  i2s().setPins(12, 10, 9, 11, -1);                    // BCLK, WS, DOUT, DIN, no MCLK
  ok = i2s().begin(I2S_MODE_STD, kRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (ok) {
    pinMode(kPaPin, OUTPUT);
    digitalWrite(kPaPin, HIGH);                        // PA on last (avoids the power-up pop)
  }
  return ok;
}

// --- async playback -----------------------------------------------------------
// All synthesis + I2S writes happen on a dedicated low-priority task fed by a
// queue, so board::tone()/beep() return in microseconds — safe to call from the
// render loop (shift beeps, alerts) without stalling a frame. Codec/I2S init
// stays on the CALLER's first tone() (the main loop also owns the GT911 on the
// same I2C bus — keeping all Wire traffic on one task sidesteps races). Tones
// are synthesized in small chunks (no per-beep allocations); every tone ends on
// a zero-amplitude ramp, so post-tone DMA underrun replays silence, not buzz.
struct ToneReq { uint16_t f0, f1, ms; };   // f0 == 0 -> timed rest (silence)

inline QueueHandle_t& toneQueue() { static QueueHandle_t q = nullptr; return q; }

inline void toneTask(void*) {
  static int16_t chunk[256 * 2];
  for (;;) {
    ToneReq r;
    if (xQueueReceive(toneQueue(), &r, portMAX_DELAY) != pdTRUE) continue;
    const size_t total = (size_t)kRate * r.ms / 1000;
    const size_t ramp  = kRate * 6 / 1000;             // 6 ms raised-cosine anti-click
    const float  ratio = (r.f0 && r.f1) ? (float)r.f1 / r.f0 : 1.0f;
    float phase = 0.0f;
    size_t done = 0;
    while (done < total) {
      const size_t n = (total - done > 256) ? 256 : total - done;
      for (size_t i = 0; i < n; i++) {
        int16_t s = 0;
        if (r.f0) {
          const size_t idx = done + i;
          float a = 1.0f;
          if (idx < ramp)               a = 0.5f - 0.5f * cosf((float)M_PI * idx / ramp);
          else if (idx > total - ramp)  a = 0.5f - 0.5f * cosf((float)M_PI * (total - idx) / ramp);
          const float f = r.f0 * powf(ratio, (float)idx / total);
          phase += 2.0f * (float)M_PI * f / kRate;
          if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
          // ~-7 dB from full-scale. The digital path is clean well above this, but
          // the little onboard speaker/PA audibly distorts past roughly this drive
          // level — this constant is the loudness knob, tuned by ear on hardware.
          s = (int16_t)(12000.0f * a * sinf(phase));
        }
        chunk[i * 2] = s;
        chunk[i * 2 + 1] = s;
      }
      i2s().write((uint8_t*)chunk, n * 2 * sizeof(int16_t));   // blocks HERE, not the caller
      done += n;
    }
  }
}

} // namespace p4audio

namespace board {
  inline IRenderer& renderer() { static WavesharePLcd5Renderer r; return r; }
  inline bool hasPSRAM() { return true; }   // 32 MB in-package — always present

  // Queue a sine tone that glides from f0 to f1 Hz (equal = steady beep; f0 = 0
  // = timed rest) on the onboard speaker. FIRE-AND-FORGET: returns immediately,
  // playback runs on the audio task, tones/rests play back-to-back in queue
  // order — safe to call from the render loop (shift beeps, alerts). The glide
  // is phase-accumulated with an exponential frequency curve, so sweeps sound
  // like a motor spooling rather than a slide whistle, and never click. First
  // call does the codec bring-up (~50 ms, once, on the caller); a board variant
  // without the codec stays silent instead of failing. A full queue drops the
  // newest tone rather than blocking.
  inline void tone(uint16_t f0, uint16_t f1, uint16_t ms) {
    if (!p4audio::init()) return;
    QueueHandle_t& q = p4audio::toneQueue();
    if (!q) {
      q = xQueueCreate(8, sizeof(p4audio::ToneReq));
      if (!q) return;
      xTaskCreatePinnedToCore(p4audio::toneTask, "tc_tone", 8192, nullptr, 3, nullptr, 0);
    }
    p4audio::ToneReq r{ f0, f1, ms };
    xQueueSend(q, &r, 0);
  }

  inline void beep(uint16_t hz, uint16_t ms) { tone(hz, hz, ms); }

  // Frame pacing for code that composes into the framebuffer directly (the deck
  // drag/slide loops): STREAMING semantics — a frame boundary that fired while
  // the caller was compositing counts, so back-to-back callers run at the full
  // panel rate. (LVGL's own refresh gate uses the renderer's clearing variant.)
  inline void waitFrame() {
    static_cast<WavesharePLcd5Renderer&>(renderer()).nextFrame();
  }

  // Copy what's on screen right now into a landscape 1280x720 RGB565 buffer
  // (PPA inverse-rotate; false without PPA). See renderer::snapshotScreen.
  inline bool snapshotScreen(uint16_t* dst) {
    return static_cast<WavesharePLcd5Renderer&>(renderer()).snapshotScreen(dst);
  }

  // Live drag state for finger-tracked UI: returns true while a finger is on
  // the glass, with its travel since touch-down (landscape px). Updated at
  // ~100 Hz by the touch task; safe to poll from the render loop.
  inline bool dragState(int16_t& dx, int16_t& dy) {
    p4gt911::State& s = p4gt911::st();
    dx = s.liveDx;
    dy = s.liveDy;
    return s.down;
  }

  // The stroke's smoothed horizontal speed, px/ms, negative leftward: live while a
  // finger is down, the last measured once it lifts. Updated on the touch task.
  inline float dragVelocity() { return p4gt911::st().velX; }

  // For a stroke just let go with this travel (dragState's last dx/dy): was it a
  // swipe, by the release classifier's own rule — far enough, or a flick by the
  // speed the touch task measured? -1 leftward, +1 rightward, 0 not a swipe. The
  // deck's drag asks this on release, so a stroke the classifier calls a swipe is
  // one the drag completes rather than snapping back.
  inline int releasedSwipe(int16_t dx, int16_t dy) {
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    const float v = p4gt911::st().velX;
    const float av = v < 0 ? -v : v;
    if (av >= p4gt911::kFlickPxPerMs && adx >= p4gt911::kFlickMinPx && adx >= ady) return v < 0 ? -1 : 1;
    if (adx >= p4gt911::kSwipePx && adx >= 2 * ady) return dx < 0 ? -1 : 1;
    return 0;
  }

  // Compose a horizontal slide between two FULL-SCREEN landscape page images
  // (1280x720 RGB565, e.g. lv_snapshot output in PSRAM) directly into the DPI
  // scan-out framebuffer with the PPA — zero software rendering. The screen
  // shows columns [split..1280) of `left` followed by columns [0..split) of
  // `right`; sweep split 0 -> 1280 for a push-left, or swap the pages and sweep
  // 1280 -> 0 for a push-right. Used by the deck build's swipe transition: LVGL's
  // own screen-move animation software-redraws BOTH pages every frame (~60 bands
  // x 2 widget trees at this resolution — the "slow sideways scroll"), while two
  // PPA blits move the same million pixels in a few milliseconds. The caller
  // must NOT run lv_timer_handler() while driving this (LVGL would flush stale
  // bands over the composition).
  inline void slideBlit(const uint16_t* left, const uint16_t* right, int split) {
    IRenderer& base = renderer();
    auto& r = static_cast<WavesharePLcd5Renderer&>(base);
    if (split < 0) split = 0;
    if (split > 1280) split = 1280;
    struct Half { const uint16_t* page; int srcX; int dstX; int w; };
    const Half halves[2] = {
      { left,  split, 0,            1280 - split },   // tail of the outgoing page
      { right, 0,     1280 - split, split        },   // head of the incoming page
    };
    for (const Half& h : halves) {
      if (h.w <= 0) continue;
      r.blitLandscapeSlice(h.page, h.srcX, h.dstX, h.w);
    }
  }

  // Backlight, 0..100 (%). The renderer boots it at 100.
  inline void setBrightness(uint8_t pct) { p4Lcd5SetBrightness(pct); }
  inline uint8_t brightness() { return p4Lcd5BrightnessRef(); }

  // Gesture stream, sampled on a dedicated ~100 Hz touch task (see touchTask):
  //   SwipeLeft/  fired MID-DRAG the instant the finger has traveled the
  //   SwipeRight  commit distance predominantly horizontally — transitions
  //               start while the flick is still in motion (once per stroke)
  //   Tap         small movement, classified on RELEASE (a swipe must never
  //               double as a tap) — x/y report the touch-DOWN point
  //   Down        every touch-down, before classification — the hook for
  //               speculative work (pre-rendering a page that a swipe might
  //               need). Every stroke emits Down first, then at most one of
  //               Tap/Swipe.
  // Landscape screen coordinates (0..1279 x, 0..719 y). Non-blocking: drains
  // one queued event per call — call in a loop until None to stay current; the
  // first call spawns the touch task.
  enum class TouchGesture : uint8_t { None, Tap, SwipeLeft, SwipeRight, Down };

  inline TouchGesture touchGesture(int16_t& x, int16_t& y) {
    using namespace p4gt911;
    QueueHandle_t& q = gestureQueue();
    if (!q) {
      q = xQueueCreate(4, sizeof(GestureEvent));
      if (!q) return TouchGesture::None;
      xTaskCreatePinnedToCore(touchTask, "tc_touch", 4096, nullptr, 4, nullptr, 0);
    }
    GestureEvent ev;
    if (xQueueReceive(q, &ev, 0) != pdTRUE) return TouchGesture::None;
    x = ev.x; y = ev.y;
    switch (ev.type) {
      case kGestTap:    return TouchGesture::Tap;
      case kGestSwipeL: return TouchGesture::SwipeLeft;
      case kGestSwipeR: return TouchGesture::SwipeRight;
      case kGestDown:   return TouchGesture::Down;
    }
    return TouchGesture::None;
  }
} // namespace board

} // namespace tracecraft
