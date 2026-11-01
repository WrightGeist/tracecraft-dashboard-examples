/**
 * TraceCraft LVGL configuration (LVGL v9).
 *
 * Only the values we care about are overridden here; everything else falls back
 * to LVGL's built-in defaults (lv_conf_internal.h). TraceCraft always drives an
 * EEZ-Studio-designed UI, so the full Montserrat range is enabled to avoid link
 * errors no matter which font size you pick in the GUI.
 */
#if 1 /* keep enabled */
#ifndef LV_CONF_H
#define LV_CONF_H

/* NOTE: do not #include C headers (e.g. <stdint.h>) here. This file is pulled
   into LVGL's assembly (.S) build via lv_conf_internal.h, and the assembler
   cannot parse C declarations. */

/* 16-bit color (RGB565). The AMOLED flush callback handles the byte-swap. */
#define LV_COLOR_DEPTH 16

/* Memory: LVGL's internal allocator. 64 KB headroom for dashboards with many
 * objects (packed shift-light dots + tire boxes + caution strips). */
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING   LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_BUILTIN
/* Overridable per env (-D LV_MEM_SIZE=...): 64KB suits the single-page S3 boards;
 * multi-page boards (P4 deck) raise it — every page's widget tree lives in this
 * pool full-time, at roughly 20-25KB per rich 1280x720 page. */
#ifndef LV_MEM_SIZE
  #define LV_MEM_SIZE          (64U * 1024U)
#endif

/* Pool placement (-D TRACECRAFT_LVGL_POOL_PSRAM): allocate the widget pool at
 * RUNTIME from internal RAM first, falling back to PSRAM only if internal
 * can't host it. History: the pool outgrew its static internal allocation
 * twice (sky layer, then LVGL 9.5's heavier per-object footprint) and the
 * STATIC (.bss) route tops out ~160KB at link — but the runtime heap has far
 * more internal room than the link-time ceiling suggests, and a PURE-PSRAM
 * pool proved to be a real performance cliff: every render walks widget
 * objects + styles, and PSRAM-resident walks made full-page renders (swipe
 * prefetch, the widget-heavy weather page) visibly slow. Internal-first gets
 * the speed back; the PSRAM fallback keeps an oversized pool booting (slow
 * beats wedged) — and the boot log prints which one you got. */
#ifdef TRACECRAFT_LVGL_POOL_PSRAM
  #define LV_MEM_POOL_INCLUDE  "esp_heap_caps.h"
  #define LV_MEM_POOL_ALLOC(size) ({ \
      void* _tc_pool = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); \
      if (!_tc_pool) _tc_pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM); \
      _tc_pool; })
#endif

/* An exhausted pool must be LOUD. LVGL's default assert handler is `while(1);`
 * — the loop wedges silently: frozen panel, USB still enumerated, 1200-baud
 * flash touch dead (it's serviced by the wedged loop), manual BOOT+RST needed.
 * We hit that failure mode three times before changing this. abort() instead:
 * the chip panics with a backtrace on the UART console (the UART Type-C port)
 * and reboots — observable and recoverable, per the instrumentation
 * rule ("if a hardware path can fail, make the failure observable"). */
#define LV_ASSERT_HANDLER_INCLUDE  <stdlib.h>
#define LV_ASSERT_HANDLER          abort();

/* Tick: we feed LVGL from Arduino millis() via lv_tick_set_cb() in code. */
#define LV_USE_OS              LV_OS_NONE

/* Fonts: enable the full Montserrat range so any size chosen in EEZ-Studio links.
   (EEZ also embeds custom TTF fonts as source when you add them to the project.) */
#define LV_FONT_MONTSERRAT_8   1
#define LV_FONT_MONTSERRAT_10  1
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_22  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_26  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_30  1
#define LV_FONT_MONTSERRAT_32  1
#define LV_FONT_MONTSERRAT_34  1
#define LV_FONT_MONTSERRAT_36  1
#define LV_FONT_MONTSERRAT_38  1
#define LV_FONT_MONTSERRAT_40  1
#define LV_FONT_MONTSERRAT_42  1
#define LV_FONT_MONTSERRAT_44  1
#define LV_FONT_MONTSERRAT_46  1
#define LV_FONT_MONTSERRAT_48  1

/* 32-bit glyph offsets for very large compiled fonts (the P4 race page's 384px
 * gear face exceeds the default 16-bit format's 64KB bitmap limit). Costs a few
 * bytes per font descriptor; small fonts are unaffected. */
#define LV_FONT_FMT_TXT_LARGE  1

/* Quieter build; flip on while bringing up new hardware. */
#define LV_USE_LOG             0

/* Screen snapshots (lv_snapshot_take_to_draw_buf) — used by the P4 deck build's
 * hardware slide transition to rasterize a page once into a PSRAM buffer, then
 * animate with PPA blits instead of per-frame software redraws. Cheap to enable
 * globally; the linker drops it from builds that never call it. */
#define LV_USE_SNAPSHOT        1

/* nanovg draw unit — new in LVGL 9.5 and ON by default, but it links against an
 * external nanovg backend we don't ship, so its lv_draw_nanovg_*.c units fail at
 * link ("Require LV_USE_NANOVG = 1" gate compiles, then undefined nanovg refs).
 * We render with the software draw unit + the per-board PPA/blitter, not nanovg,
 * so keep it off. */
#define LV_USE_NANOVG          0

/* FPS + CPU overlay (bottom-right) for render-performance tuning. Opt-in per
 * env with -D TRACECRAFT_PERF_OVERLAY — a measurement tool, not a shipping
 * feature; drop the flag once the numbers are in. */
#ifdef TRACECRAFT_PERF_OVERLAY
  #define LV_USE_SYSMON        1
  #define LV_USE_PERF_MONITOR  1
#endif

#endif /*LV_CONF_H*/
#endif /*end of "#if 1"*/
