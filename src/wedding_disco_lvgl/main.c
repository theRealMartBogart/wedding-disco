// wedding_disco_lvgl — the LVGL-based variant of wedding_disco.
//
// Identical lighting engine to the original (same spectrum.c, patterns.c,
// strip.c and codes.h — those are shared source, not copies), but the LCD is
// driven by LVGL widgets instead of hand-drawn ST7796 rectangles. What that
// buys: real buttons with pressed-state feedback, proper hit-testing, styled
// bar widgets for the analyser, and a UI that can grow without hand-computing
// pixel coordinates.
//
// What it costs, and how it is handled here:
//   * Code size. LVGL is large and this firmware is copy_to_ram, so the draw
//     buffers are kept modest (lvgl_port.c) and unused widgets are compiled
//     out (lv_conf.h).
//   * SPI1 contention. The LCD flush is now an async DMA transfer that can
//     still be in flight when the app wants the bus, and the CC1101 shares
//     SPI1 with the panel. Every radio transmit waits for the flush to finish
//     first — see transmit().
//   * Latency. lv_timer_handler() is not bounded, and the PDM ring only has
//     ~64 ms of slack before it silently overwrites. Rendering is throttled
//     and the UI only touches widgets whose values actually changed, so a
//     typical frame does almost no work.
#include "fw2.h"
#include "hardware/pio.h"
#include "platform/diag.h"
#include "platform/ioexp.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "lvgl.h"
#include "beat.h"
#include "codes.h"
#include "patterns.h"
#include "spectrum.h"
#include "strip.h"
#include "lvgl_port.h"
#include "ui.h"
#include "onewili.h"
#include "onewili_fwgui.h"

#define HOP           256u
#define NMICS         PDM_NUM_MICS
#define NPIX          WS2812_NUM_PIXELS
#define TX_FREQ_HZ    433920000u
#define TX_ANTENNA    ANT_CC1101_433
#define TX_REPEATS    2
#define TX_START_LEVEL true
#define TX_MIN_GAP_MS 150u
#define QUIET_AFTER_MS 20000u
// See the note in wedding_disco/main.c: beat.h's default 2e6 gate sits far
// below this board's room floor and lets the detector free-run.
#define BEAT_NOISE_FLOOR 200000000ull
#define BEAT_REFRACTORY_MS 250u
// See wedding_disco/main.c: 1.3x let the detector free-run at its refractory
// ceiling on this board. Keep the two apps' tuning in step.
#define BEAT_RATIO_NUM 19u
#define BEAT_RATIO_DEN 10u
#define BEAT_AVG_SHIFT 6u
// 7, not 16 — see the long note in wedding_disco/main.c. 16 is the display
// CPU's own WS2812 count; the main CPU's LED manager has 7.
#define OW_LEDS       7
#define OW_FAIL_LIMIT 3
#define OW_POST_EVERY 3
#define IMU_EVERY     3
// Run LVGL every Nth audio frame (~21 Hz at a 16 ms frame). Rendering is the
// one unbounded thing in the loop; this keeps its share of the PDM ring's
// slack small while staying well above the rate the eye notices.
#define LVGL_EVERY    3

static int16_t s_pcm[NMICS][HOP];
static int16_t s_mono[HOP];
static rgb_t   s_fb[NPIX];
static spectrum_t s_spec;

// ---------------------------------------------------------------------------
static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static rgb_t led_for(const char *cname) {
    static const struct { const char *n; rgb_t c; } map[] = {
        { "red",          { 255, 0,   0   } }, { "orange",       { 255, 80,  0   } },
        { "yellow",       { 255, 180, 0   } }, { "light_yellow", { 255, 255, 120 } },
        { "light_green",  { 120, 255, 60  } }, { "green",        { 0,   255, 0   } },
        { "dark_green",   { 0,   140, 40  } }, { "turquoise",    { 0,   255, 160 } },
        { "cyan",         { 0,   220, 255 } }, { "blue",         { 0,   0,   255 } },
        { "purple",       { 150, 0,   255 } }, { "pink",         { 255, 40,  120 } },
        { "white",        { 200, 200, 200 } },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (str_eq(cname, map[i].n)) return map[i].c;
    rgb_t d = { 120, 0, 160 };
    return d;
}

static int code_index(const char *cname) {
    for (int i = 0; i < NCODES; i++)
        if (str_eq(g_codes[i].cname, cname)) return i;
    return -1;
}

static bool s_radio_ok = false;
static uint32_t s_last_tx_ms = 0;

static void transmit(const code_t *c) {
    if (!s_radio_ok || !code_present(c)) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_last_tx_ms < TX_MIN_GAP_MS) return;
    s_last_tx_ms = now;
    // The CC1101 shares SPI1 with the panel, and the LVGL flush is a DMA
    // transfer that may still be running. Let it drain before taking the bus.
    while (lvgl_port_flush_busy()) tight_loop_contents();
    uint32_t durs[CODE_EDGES];
    uint32_t n = code_synth(c, durs);
    cc1101_tx_ook_start(TX_FREQ_HZ);
    for (int r = 0; r < TX_REPEATS; r++) ook_tx_send(durs, n, TX_START_LEVEL);
    cc1101_tx_ook_stop();
}

// ---------------------------------------------------------------------------
// OneWili neopixel mirror on core1 — identical to the original app.
// ---------------------------------------------------------------------------
static volatile uint32_t s_ow_seq = 0;
static volatile uint8_t  s_ow_px[OW_LEDS][3];
static volatile bool     s_ow_link_up = false;

static void ow_post(const rgb_t *fb, uint32_t n) {
    s_ow_seq++;
    __compiler_memory_barrier();
    for (uint32_t i = 0; i < OW_LEDS; i++) {
        rgb_t c = fb[(i * n) / OW_LEDS];
        s_ow_px[i][0] = c.r; s_ow_px[i][1] = c.g; s_ow_px[i][2] = c.b;
    }
    __compiler_memory_barrier();
    s_ow_seq++;
}

static void core1_neopixel(void) {
    ow_device dev;
    bool link = false;
    uint32_t sent_seq = 0;
    int consecutive_fail = 0;
    for (;;) {
        if (!link) {
            link = (ow_open_fwgui(&dev) == OW_OK);
            s_ow_link_up = link;
            consecutive_fail = 0;
            // Report the transition once. Silence is otherwise ambiguous: a
            // main CPU that never answers and one whose LED writes all succeed
            // look identical from here.
            if (link) {
                DIAG("onewili: link up, mirroring %d LEDs\n", OW_LEDS);
            } else {
                static bool moaned;
                if (!moaned) {
                    moaned = true;
                    DIAG("onewili: link down (main CPU not answering); retrying\n");
                }
            }
            if (!link) { sleep_ms(2000); continue; }
        }
        uint32_t s0 = s_ow_seq;
        if ((s0 & 1u) || s0 == sent_seq) { sleep_ms(2); continue; }
        uint8_t px[OW_LEDS][3];
        for (uint32_t i = 0; i < OW_LEDS; i++) {
            px[i][0] = s_ow_px[i][0]; px[i][1] = s_ow_px[i][1]; px[i][2] = s_ow_px[i][2];
        }
        if (s_ow_seq != s0) continue;
        sent_seq = s0;
        // A rejected command != a dead link; only give up after whole frames
        // fail in a row. See wedding_disco/main.c.
        int ok = 0;
        for (int i = 0; i < OW_LEDS; i++) {
            if (ow_gui_set_led_color(&dev, i, px[i][0], px[i][1], px[i][2], 0,
                                     OW_OW_LED_MANAGER_LED_MODE_SIMPLEVALUE) == OW_OK)
                ok++;
        }
        if (ok == 0) {
            if (++consecutive_fail >= OW_FAIL_LIMIT) {
                link = false; s_ow_link_up = false;
                DIAG("onewili: link dropped after %d dead frames\n", consecutive_fail);
            }
        } else {
            consecutive_fail = 0;
        }
    }
}

// ---------------------------------------------------------------------------
int main(void) {
    board_init();
    st7796_init();
    board_backlight_set(1);
    bool touch_ok = ft6336_init();
    bool imu_ok   = bmi323_init();

    strip_pio_base_setup();
    ws2812_init(pio0, 0, PIN_LED_DATA);
    ws2812_set_brightness(0);
    bool strip_ok = strip_init();

    pdm_capture_init();
    spectrum_init();

    ioexp_antenna(TX_ANTENNA);
    s_radio_ok = cc1101_init();

    lvgl_port_init();
    ui_build();
    ui_set_radio(s_radio_ok);
    // LV_USE_LOG is off to save code space, so a heap that is too small would
    // otherwise fail silently (widgets simply not appearing). Report the real
    // high-water mark once, after every widget exists — if used_pct is close
    // to 100 here, raise LV_MEM_SIZE in lv_conf.h.
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        DIAG("lvgl heap: used=%u free=%u frag=%u%% used_pct=%u%%\n",
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.free_size,
             (unsigned)mon.frag_pct, (unsigned)mon.used_pct);
    }

    multicore_launch_core1(core1_neopixel);

    static const char *palette[] = {
        "red", "orange", "yellow", "light_yellow", "light_green", "green",
        "dark_green", "turquoise", "cyan", "blue", "purple", "pink",
    };
    int color_idx[16]; int ncolor = 0;
    for (unsigned i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
        int idx = code_index(palette[i]);
        if (idx >= 0 && code_present(&g_codes[idx])) color_idx[ncolor++] = idx;
    }
    if (!ncolor) color_idx[0] = 0;
    int strobe_idx = code_index("strobe");
    int fade_idx   = code_index("fade");
    static const char *pat_code[PAT_COUNT] = {
        "rainbow", "qblink", NULL, "strobe", "fade", "fade" };

    DIAG("wedding_disco_lvgl up: radio=%s touch=%s imu=%s strip=%s colors=%d\n",
         s_radio_ok ? "ok" : "FAIL", touch_ok ? "ok" : "FAIL",
         imu_ok ? "ok" : "FAIL", strip_ok ? "local" : "onewili", ncolor);

    beat_state_t bs; beat_init(&bs);
    beat_cfg_t cfg = beat_cfg_default();
    cfg.noise_floor   = BEAT_NOISE_FLOOR;
    cfg.refractory_ms = BEAT_REFRACTORY_MS;
    cfg.ratio_num     = BEAT_RATIO_NUM;
    cfg.alpha_shift   = BEAT_AVG_SHIFT;
    cfg.ratio_den     = BEAT_RATIO_DEN;

    disco_mode_t mode = MODE_MUSIC;
    uint8_t variant[MODE_COUNT] = { 0, 0, 0 };
    bool auto_mode = true, strobe_on = false, quiet = false;
    int  pal = 0;
    uint32_t peak = 1, level = 0, beats = 0, frame = 0;
    uint32_t last_beat_ms = to_ms_since_boot(get_absolute_time());
    uint32_t beat_ema_ms = 0, pos_q8 = 0;
    int16_t tilt_x = 0, tilt_y = 0;
    uint8_t shake = 0;
    float px_g = 0, py_g = 0, pz_g = 1.0f;
    bool imu_primed = false;
    int  last_sent_pal = -1;
    uint32_t raw_lo = 0xFFFFFFFFu, raw_hi = 0, e_lo = 0xFFFFFFFFu, e_hi = 0;

    rgb_t cur = led_for(g_codes[color_idx[0]].cname);

    for (;;) {
        frame++;
        int16_t *dst[NMICS] = { s_pcm[0], s_pcm[1], s_pcm[2], s_pcm[3] };
        pdm_capture_block(dst, HOP);
        uint64_t e = 0;
        for (int m = 0; m < NMICS; m++) {
            dcblock_inplace(s_pcm[m], HOP);
            e += beat_energy(s_pcm[m], HOP);
        }
        for (unsigned i = 0; i < HOP; i++) {
            int32_t v = (int32_t)s_pcm[0][i] + s_pcm[1][i] + s_pcm[2][i] + s_pcm[3][i];
            s_mono[i] = (int16_t)(v >> 2);
        }
        spectrum_feed(s_mono, HOP, &s_spec);
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool is_beat = beat_feed(&bs, &cfg, e, now) && ncolor > 0;

        if (e > peak) peak = (uint32_t)(e > 0xFFFFFFFFu ? 0xFFFFFFFFu : e);
        else peak -= (peak >> 6);
        if (peak < 1) peak = 1;
        uint32_t target = (e >= peak) ? 255u : (uint32_t)((e * 255u) / peak);
        if (target > level) level = target;
        else level = level - ((level - target) >> 2);

        if (imu_ok && (frame % IMU_EVERY) == 0) {
            bmi323_reading_t r;
            if (bmi323_read(&r) && r.valid) {
                int32_t tx_ = (int32_t)(r.ax * 256.0f), ty_ = (int32_t)(r.ay * 256.0f);
                tilt_x = (int16_t)(tx_ < -256 ? -256 : tx_ > 256 ? 256 : tx_);
                tilt_y = (int16_t)(ty_ < -256 ? -256 : ty_ > 256 ? 256 : ty_);
                if (imu_primed) {
                    float d = (r.ax - px_g) * (r.ax - px_g)
                            + (r.ay - py_g) * (r.ay - py_g)
                            + (r.az - pz_g) * (r.az - pz_g);
                    int32_t sv = (int32_t)(d * 900.0f);
                    if (sv > 255) sv = 255;
                    if (sv > (int32_t)shake) shake = (uint8_t)sv;
                }
                px_g = r.ax; py_g = r.ay; pz_g = r.az;
                imu_primed = true;
            }
        }
        if (shake) shake -= (uint8_t)(shake > 12 ? shake / 8 + 1 : shake);

        // --- UI events (LVGL did the hit-testing for us) ---
        ui_event_t ev = ui_take_event();
        switch (ev) {
        case UI_EV_TOGGLE_STROBE:
            strobe_on = !strobe_on;
            if (strobe_on && strobe_idx >= 0) transmit(&g_codes[strobe_idx]);
            else { transmit(&g_codes[color_idx[pal]]); last_sent_pal = pal; }
            break;
        case UI_EV_NEXT_COLOR:
            if (ncolor) {
                pal = (pal + 1) % ncolor;
                auto_mode = false;
                cur = led_for(g_codes[color_idx[pal]].cname);
                if (!strobe_on) { transmit(&g_codes[color_idx[pal]]); last_sent_pal = pal; }
            }
            break;
        case UI_EV_NEXT_VARIANT: {
            uint8_t n = disco_variant_count(mode);
            variant[mode] = (uint8_t)((variant[mode] + 1) % n);
            if (mode == MODE_PATTERN && !strobe_on) {
                const char *cn = pat_code[variant[mode] % PAT_COUNT];
                int ci = cn ? code_index(cn) : color_idx[pal];
                if (ci >= 0) transmit(&g_codes[ci]);
            }
            break;
        }
        case UI_EV_MODE_MUSIC:
        case UI_EV_MODE_PATTERN:
        case UI_EV_MODE_ACCEL: {
            disco_mode_t want = (ev == UI_EV_MODE_MUSIC)   ? MODE_MUSIC
                              : (ev == UI_EV_MODE_PATTERN) ? MODE_PATTERN
                                                           : MODE_ACCEL;
            // Re-tapping the active MUSIC button toggles auto-cycle, matching
            // the original app.
            if (want == mode && mode == MODE_MUSIC) {
                auto_mode = !auto_mode;
            } else if (want != mode) {
                mode = want;
                if (mode == MODE_MUSIC) quiet = false;
                if (mode == MODE_PATTERN && !strobe_on) {
                    const char *cn = pat_code[variant[mode] % PAT_COUNT];
                    int ci = cn ? code_index(cn) : color_idx[pal];
                    if (ci >= 0) transmit(&g_codes[ci]);
                }
            }
            break;
        }
        default: break;
        }

        if (is_beat) {
            beats++;
            uint32_t dt = now - last_beat_ms;
            last_beat_ms = now;
            if (dt >= 200 && dt <= 2000)
                beat_ema_ms = beat_ema_ms ? (beat_ema_ms * 3 + dt) / 4 : dt;
            quiet = false;
            if (mode == MODE_MUSIC && auto_mode) {
                pal = (pal + 1) % ncolor;
                cur = led_for(g_codes[color_idx[pal]].cname);
                if (!strobe_on) { transmit(&g_codes[color_idx[pal]]); last_sent_pal = pal; }
            }
            level = 255;
        }

        if (mode == MODE_MUSIC) {
            if (!quiet && now - last_beat_ms > QUIET_AFTER_MS) {
                quiet = true;
                if (!strobe_on && fade_idx >= 0) transmit(&g_codes[fade_idx]);
            }
            if (quiet) {
                uint32_t t = (now >> 4) & 0x1FF;
                uint32_t tri_ = t < 256 ? t : 511 - t;
                level = 30 + (tri_ >> 2);
            }
        } else if (quiet) {
            quiet = false;
        }

        uint32_t interval = beat_ema_ms ? beat_ema_ms : 500;
        pos_q8 += (mode == MODE_MUSIC) ? (NPIX * 16u * 256u) / (4u * interval) : 90u;
        uint8_t vshow = quiet ? (uint8_t)MUS_BEAT : variant[mode];
        disco_frame_t f = {
            .color = cur, .level = (uint8_t)level, .pos_q8 = pos_q8,
            .ms_since_beat = now - last_beat_ms, .now_ms = now,
            .tilt_x = tilt_x, .tilt_y = tilt_y, .shake = shake,
            .variant = vshow, .spec = &s_spec,
        };
        disco_render(mode, &f, s_fb, NPIX);

        bool energy_scaled = (mode == MODE_MUSIC && vshow == MUS_BEAT);
        uint8_t master = energy_scaled ? (uint8_t)(level < 24 ? 24 : level) : 200;
        ws2812_set_brightness(master);
        for (uint32_t i = 0; i < NPIX; i++) ws2812_set_pixel(i, s_fb[i]);
        ws2812_show();
        strip_show(s_fb, NPIX, master);
        if ((frame % OW_POST_EVERY) == 0) ow_post(s_fb, NPIX);

        if (mode == MODE_ACCEL && !strobe_on && ncolor) {
            rgb_t avg = disco_frame_average(s_fb, NPIX);
            int best = 0; int32_t bestd = 0x7FFFFFFF;
            for (int i = 0; i < ncolor; i++) {
                rgb_t p = led_for(g_codes[color_idx[i]].cname);
                int32_t dr = (int32_t)avg.r - p.r, dg = (int32_t)avg.g - p.g,
                        db = (int32_t)avg.b - p.b;
                int32_t d = dr * dr + dg * dg + db * db;
                if (d < bestd) { bestd = d; best = i; }
            }
            if (best != last_sent_pal) {
                pal = best;
                cur = led_for(g_codes[color_idx[pal]].cname);
                uint32_t before = s_last_tx_ms;
                transmit(&g_codes[color_idx[pal]]);
                if (s_last_tx_ms != before) last_sent_pal = best;
            }
        }

        // --- Soundcheck aid, same as the original app ---
        {
            uint32_t r = s_spec.raw_peak, es = (uint32_t)(e >> 8);
            if (r < raw_lo) raw_lo = r;
            if (r > raw_hi) raw_hi = r;
            if (es < e_lo) e_lo = es;
            if (es > e_hi) e_hi = es;
        }
        if ((frame % 128) == 0) {
            DIAG("spec raw=%u..%u e=%u..%u | bands b/m/t=%u/%u/%u beats=%u\n",
                 (unsigned)raw_lo, (unsigned)raw_hi, (unsigned)e_lo, (unsigned)e_hi,
                 s_spec.bass, s_spec.mid, s_spec.treble, (unsigned)beats);
            raw_lo = 0xFFFFFFFFu; raw_hi = 0; e_lo = 0xFFFFFFFFu; e_hi = 0;
        }

        // --- LVGL ---
        if ((frame % LVGL_EVERY) == 0) {
            ui_state_t st = {
                .mode = mode, .variant = variant[mode], .spec = &s_spec,
                .color = cur, .color_name = g_codes[color_idx[pal]].label,
                .bpm = (beat_ema_ms && !quiet) ? 60000 / beat_ema_ms : 0,
                .beat = (now - last_beat_ms) < 120,
                .auto_cycle = auto_mode, .strobe = strobe_on, .quiet = quiet,
                .radio_ok = s_radio_ok,
                .pixels_ok = strip_ok || s_ow_link_up,
                .pixels_label = strip_ok ? "PIXELS GPIO"
                                         : (s_ow_link_up ? "PIXELS OK" : "PIXELS --"),
                .tilt_x = tilt_x,
            };
            ui_update(&st);
            lv_timer_handler();
        }
    }
}
