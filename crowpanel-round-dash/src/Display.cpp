// The CrowPanel's GC9A01 round panel, brought up with esp_lcd and bound to LVGL by
// esp_lvgl_port, and the three screens the dial button cycles through:
//   0  brake bias (top), TC (lower left) and ABS (lower right), each a small dial
//   1  engine map, full screen
//   2  brake bias, full screen
// The dials only show what the sim reports. The encoder changes the sim through its own
// button bindings, and the new value comes back over USB, so nothing on the board can
// drift from the car.
#include "Display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

// Geist, compiled for LVGL (fonts/).
LV_FONT_DECLARE(geist_sans_14);
LV_FONT_DECLARE(geist_sans_16);
LV_FONT_DECLARE(geist_sans_20);
LV_FONT_DECLARE(geist_sans_40);

#include "tc_generated.h"      // tcStore + TC_CH_*, generated from custom_channels
#include <TraceCraftDisplay.h> // the boot splash
#include <cstdio>
#include <initializer_list>

namespace {
constexpr int PIN_SCLK = 10, PIN_MOSI = 11, PIN_DC = 3, PIN_CS = 9, PIN_RST = 14;
constexpr int PIN_BL = 46;
constexpr int PIN_LCD_PWR = 1, PIN_LED_PWR = 2;   // power switches: high, or the panel stays dark
constexpr int LCD_W = 240, LCD_H = 240;

// --- palette ------------------------------------------------------------------
const lv_color_t kBg       = lv_color_hex(0x08090c);   // near-black face
const lv_color_t kDialFace = lv_color_hex(0x14151a);   // recessed sub-dial
const lv_color_t kTrack    = lv_color_hex(0x2a2d34);   // arc track
const lv_color_t kTickMin  = lv_color_hex(0x3a3d44);   // minor tick
const lv_color_t kValue    = lv_color_hex(0xF3F5F7);   // value text
const lv_color_t kCaption  = lv_color_hex(0x82868f);   // label text
const lv_color_t kAccBias  = lv_color_hex(0xE0A24E);   // amber-gold (hero)
const lv_color_t kAccTc    = lv_color_hex(0x54D6B6);   // teal
const lv_color_t kAccAbs   = lv_color_hex(0x6BA6FF);   // blue
const lv_color_t kAccMap   = lv_color_hex(0xfb923c);   // orange (engine map)
const lv_color_t kHot      = lv_color_hex(0xFFCF5C);   // "intervening now" glow

constexpr int TC_ABS_MAX = 11;   // TC/ABS level dial range (typical sim max)
constexpr int MAP_MIN = 1, MAP_MAX = 12;               // engine-map dial range
constexpr float BIAS_LO = 40.0f, BIAS_SPAN = 30.0f;    // bias arc maps 40..70 %

struct Dial { lv_obj_t* arc = nullptr; lv_obj_t* val = nullptr; lv_obj_t* face = nullptr; };
Dial s_bias, s_tc, s_abs;      // page 0 — chronometer
Dial s_bigMap, s_bigBias;      // pages 1/2 — full-screen dials

lv_obj_t* s_scr[3] = { nullptr, nullptr, nullptr };
uint8_t   s_page = 0;

tracecraft::Splash s_splash;   // on the top layer until the recorder connects

// A sub-dial: recessed face + one outer ring that BOTH fills with the value AND
// carries the tick marks (no separate inner arc), + center number + caption.
Dial makeDial(lv_obj_t* scr, int xoff, int yoff, lv_color_t accent, const char* cap) {
    constexpr int SZ = 88;

    lv_obj_t* face = lv_obj_create(scr);       // recessed backdrop
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, SZ, SZ);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(face, kDialFace, 0);
    lv_obj_align(face, LV_ALIGN_CENTER, xoff, yoff);

    // Value fill: a THICK ring in the tick region. Unfilled part = the face color
    // (so only the filled arc reads); the ticks (below) overlay it.
    lv_obj_t* arc = lv_arc_create(scr);
    lv_obj_set_size(arc, SZ - 6, SZ - 6);
    lv_obj_align(arc, LV_ALIGN_CENTER, xoff, yoff);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, kDialFace, LV_PART_MAIN);      // unfilled blends into the face
    lv_obj_set_style_arc_color(arc, accent, LV_PART_INDICATOR);

    // Watch-face tick marks, on TOP of the fill: 20 minor, every 5th major.
    lv_obj_t* scale = lv_scale_create(scr);
    lv_obj_remove_style_all(scale);
    lv_obj_set_size(scale, SZ - 6, SZ - 6);
    lv_obj_align(scale, LV_ALIGN_CENTER, xoff, yoff);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(scale, 135);
    lv_scale_set_angle_range(scale, 270);
    lv_scale_set_range(scale, 0, 100);
    lv_scale_set_total_tick_count(scale, 21);
    lv_scale_set_major_tick_every(scale, 5);
    lv_scale_set_label_show(scale, false);
    lv_obj_set_style_line_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);   // no base line
    lv_obj_set_style_length(scale, 6, LV_PART_ITEMS);               // minor
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x9498a0), LV_PART_ITEMS);  // light: visible over fill + face
    lv_obj_set_style_length(scale, 11, LV_PART_INDICATOR);          // major
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0xe8eaee), LV_PART_INDICATOR);

    lv_obj_t* val = lv_label_create(scr);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &geist_sans_20, 0);
    lv_obj_set_style_text_color(val, kValue, 0);
    lv_obj_align(val, LV_ALIGN_CENTER, xoff, yoff - 8);

    lv_obj_t* c = lv_label_create(scr);
    lv_label_set_text(c, cap);
    lv_obj_set_style_text_font(c, &geist_sans_14, 0);
    lv_obj_set_style_text_color(c, kCaption, 0);
    lv_obj_align(c, LV_ALIGN_CENTER, xoff, yoff + 16);

    return { arc, val, face };
}

void setLevel(Dial& d, int level, int maxLevel, lv_color_t base, bool active) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", level);
    lv_label_set_text(d.val, buf);
    int v = maxLevel > 0 ? level * 100 / maxLevel : 0;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    lv_arc_set_value(d.arc, v);
    if (active) {
        // "It kicked in": the WHOLE background circle lights up its color. The ring
        // blends into the lit face and the number flips dark for contrast.
        lv_obj_set_style_bg_color(d.face, base, 0);
        lv_obj_set_style_arc_color(d.arc, base, LV_PART_MAIN);
        lv_obj_set_style_arc_color(d.arc, base, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(d.val, kBg, 0);
    } else {
        lv_obj_set_style_bg_color(d.face, kDialFace, 0);
        lv_obj_set_style_arc_color(d.arc, kDialFace, LV_PART_MAIN);
        lv_obj_set_style_arc_color(d.arc, base, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(d.val, kValue, 0);
    }
}

// A FULL-SCREEN dial for the single-setting pages: the same recessed-face +
// value-ring + tick vocabulary as the sub-dials, scaled to own the whole round
// panel, with the value in the big 40pt face. `ticks`/`majorEvery` shape the
// scale to the setting's step count (engine map: a tick per step).
Dial makeBigDial(lv_obj_t* scr, lv_color_t accent, const char* cap,
                 int ticks, int majorEvery) {
    constexpr int SZ = 216;

    lv_obj_t* face = lv_obj_create(scr);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, SZ, SZ);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(face, kDialFace, 0);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* arc = lv_arc_create(scr);
    lv_obj_set_size(arc, SZ - 8, SZ - 8);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, kDialFace, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, accent, LV_PART_INDICATOR);

    lv_obj_t* scale = lv_scale_create(scr);
    lv_obj_remove_style_all(scale);
    lv_obj_set_size(scale, SZ - 8, SZ - 8);
    lv_obj_align(scale, LV_ALIGN_CENTER, 0, 0);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(scale, 135);
    lv_scale_set_angle_range(scale, 270);
    lv_scale_set_range(scale, 0, 100);
    lv_scale_set_total_tick_count(scale, ticks);
    lv_scale_set_major_tick_every(scale, majorEvery);
    lv_scale_set_label_show(scale, false);
    lv_obj_set_style_line_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_length(scale, 9, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x9498a0), LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 17, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0xe8eaee), LV_PART_INDICATOR);

    lv_obj_t* val = lv_label_create(scr);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &geist_sans_40, 0);
    lv_obj_set_style_text_color(val, kValue, 0);
    lv_obj_align(val, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t* c = lv_label_create(scr);
    lv_label_set_text(c, cap);
    lv_obj_set_style_text_font(c, &geist_sans_16, 0);
    lv_obj_set_style_text_color(c, kCaption, 0);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 34);

    return { arc, val, face };
}
} // namespace

void display_begin(void) {
    // Power gates + backlight ON.
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << PIN_LCD_PWR) | (1ULL << PIN_LED_PWR) | (1ULL << PIN_BL);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level((gpio_num_t)PIN_LCD_PWR, 1);
    gpio_set_level((gpio_num_t)PIN_LED_PWR, 1);
    gpio_set_level((gpio_num_t)PIN_BL, 1);

    // SPI bus (SPI2_HOST).
    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_SCLK;
    bus.mosi_io_num = PIN_MOSI;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = LCD_W * 80 * (int)sizeof(uint16_t);
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    // Panel IO + GC9A01.
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_CS;
    io_cfg.dc_gpio_num = PIN_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = PIN_RST;
    pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    pcfg.bits_per_pixel = 16;
    esp_lcd_new_panel_gc9a01(io, &pcfg, &panel);
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    // Bring up LVGL and bind it to the panel (its own task + tick).
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle     = io;
    disp_cfg.panel_handle  = panel;
    disp_cfg.buffer_size   = LCD_W * 40;
    disp_cfg.double_buffer = true;
    disp_cfg.hres          = LCD_W;
    disp_cfg.vres          = LCD_H;
    disp_cfg.color_format  = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma   = true;
    disp_cfg.flags.swap_bytes = true;        // the panel takes RGB565 big-endian
    lv_display_t* disp = lvgl_port_add_disp(&disp_cfg);

    // The CrowPanel's panel is mirrored in X. Set it on the panel, after add_disp:
    // esp_lvgl_port's own mirror setting turns the picture upside down.
    esp_lcd_panel_mirror(panel, true, false);

    // Three screens, cycled by the dial-press. Page 0 is the chronometer
    // (tri-compax sub-dials); pages 1/2 are full-screen single-setting dials.
    lvgl_port_lock(0);
    for (int i = 0; i < 3; i++) {
        s_scr[i] = (i == 0) ? lv_display_get_screen_active(disp) : lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_scr[i], kBg, 0);
        lv_obj_set_style_bg_opa(s_scr[i], LV_OPA_COVER, 0);
    }

    s_bias = makeDial(s_scr[0],   0, -48, kAccBias, "BIAS");   // top (hero)
    s_tc   = makeDial(s_scr[0], -46,  40, kAccTc,   "TC");     // lower-left
    s_abs  = makeDial(s_scr[0],  46,  40, kAccAbs,  "ABS");    // lower-right

    // Engine map: a tick per step (1..12 -> 12 ticks, all major).
    s_bigMap  = makeBigDial(s_scr[1], kAccMap,  "MAP",  MAP_MAX, 1);
    // Brake bias: same 21-tick shape as the small bias dial.
    s_bigBias = makeBigDial(s_scr[2], kAccBias, "BIAS", 21, 5);

    s_page = 0;
    s_splash.begin();   // covers the dials until the recorder connects
    lvgl_port_unlock();
}

uint8_t display_next_page(void) {
    lvgl_port_lock(0);
    s_page = (uint8_t)((s_page + 1) % 3);
    lv_screen_load(s_scr[s_page]);
    lvgl_port_unlock();
    return s_page;
}

// The boot splash. Locked, because esp_lvgl_port runs LVGL on a task of its own.
void display_splash_tick(uint8_t phase, uint8_t verdict, const char* latestVer) {
    lvgl_port_lock(0);
    s_splash.tick(phase, verdict, latestVer);
    lvgl_port_unlock();
}

void display_update(bool waiting) {
    if (!s_bias.arc) return;   // display_begin not run
    lvgl_port_lock(0);
    if (waiting) {
        for (Dial* d : { &s_bias, &s_tc, &s_abs, &s_bigMap, &s_bigBias }) {
            lv_arc_set_value(d->arc, 0);
            lv_label_set_text(d->val, "--");
            lv_obj_set_style_text_color(d->val, kValue, 0);
            lv_obj_set_style_bg_color(d->face, kDialFace, 0);            // clear any active glow
            lv_obj_set_style_arc_color(d->arc, kDialFace, LV_PART_MAIN);
        }
        lv_obj_set_style_arc_color(s_bias.arc,    kAccBias, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_tc.arc,      kAccTc,   LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_abs.arc,     kAccAbs,  LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_bigMap.arc,  kAccMap,  LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_bigBias.arc, kAccBias, LV_PART_INDICATOR);
    } else {
        using namespace tracecraft;
        char buf[8];

        // Bias appears twice (chronometer sub-dial + full-screen page) — same
        // value, same 40..70% arc mapping, both updated every pass (hidden
        // screens' widgets are cheap to set; nothing renders until shown).
        const float bias = tcStore.getFloat(TC_CH_BRAKE_BIAS_PCT);
        snprintf(buf, sizeof(buf), "%.1f", (double)bias);
        int bv = (int)((bias - BIAS_LO) / BIAS_SPAN * 100.0f);
        if (bv < 0) bv = 0; else if (bv > 100) bv = 100;
        lv_label_set_text(s_bias.val, buf);
        lv_arc_set_value(s_bias.arc, bv);
        lv_label_set_text(s_bigBias.val, buf);
        lv_arc_set_value(s_bigBias.arc, bv);

        setLevel(s_tc,  tcStore.getInt(TC_CH_TC_LEVEL),  TC_ABS_MAX, kAccTc,
                 tcStore.getBool(TC_CH_TC_ACTIVE));
        setLevel(s_abs, tcStore.getInt(TC_CH_ABS_LEVEL), TC_ABS_MAX, kAccAbs,
                 tcStore.getBool(TC_CH_ABS_ACTIVE));

        // Engine map: 0 = sim doesn't publish it -> keep the dash.
        const int map = (int)tcStore.getInt(TC_CH_ENGINE_MAP);
        if (map >= MAP_MIN) {
            snprintf(buf, sizeof(buf), "%d", map);
            lv_label_set_text(s_bigMap.val, buf);
            int mv = (map - MAP_MIN) * 100 / (MAP_MAX - MAP_MIN);
            if (mv < 0) mv = 0; else if (mv > 100) mv = 100;
            lv_arc_set_value(s_bigMap.arc, mv);
        } else {
            lv_label_set_text(s_bigMap.val, "--");
            lv_arc_set_value(s_bigMap.arc, 0);
        }
    }
    lvgl_port_unlock();
}
