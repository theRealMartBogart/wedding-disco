// wedding_disco — the wedding-party brain for the Free Wili 2.
//
// One animation drives every light in the room. Each frame renders into a
// 16-pixel master framebuffer (patterns.c), which is then pushed to:
//   * the 16 onboard WS2812s,
//   * the external neopixel strip — over the OneWili link to the main CPU by
//     default, or straight off a display-CPU GPIO if you enable strip.h,
//   * the DEFCON RF bracelets, as the nearest matching remote code sent over
//     the CC1101 (433.92 MHz OOK, codes.h),
//   * the LCD, as a live status panel.
//
// Three light modes, chosen from the on-screen button row:
//   MUSIC   — PDM mics -> beat detector -> flash on the beat, comet between.
//   PATTERN — six free-running animations that ignore the mics (dinner, slow
//             songs, speeches).
//   ACCEL   — BMI323 accelerometer: tilt for hue, shake to flash, or a
//             bubble-level dot. Hand the board to a guest.
//
// PIO map: onboard LEDs pio0 SM0, optional external strip pio0 SM1, PDM mics
// pio1, radio TX is SIO bit-bang (no PIO) — this sidesteps the pio1
// instruction-memory co-fit between the WS2812 and PDM programs.
// Core split: core0 owns mics/beat/LEDs/LCD/RF/IMU; core1 owns the OneWili
// link so a stalled main CPU can never starve the audio loop.
#include "fw2.h"
#include "hardware/pio.h"
#include "platform/diag.h"
#include "platform/ioexp.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "beat.h"
#include "codes.h"
#include "patterns.h"
#include "spectrum.h"
#include "strip.h"
#include "onewili.h"
#include "onewili_fwgui.h"

// ---- Audio / beat ----
#define HOP           256u        // samples per analysis block (~16 ms @ 16 kHz)
#define NMICS         PDM_NUM_MICS
#define NPIX          WS2812_NUM_PIXELS   // master framebuffer length (16)
// ---- Radio TX (kept short so a transmit can't starve the mic ring past its
// ~64 ms slack; see the partyremote docs' "PDM overrun semantics") ----
#define TX_FREQ_HZ    433920000u
#define TX_ANTENNA    ANT_CC1101_433
#define TX_REPEATS    2
#define TX_START_LEVEL true
#define TX_MIN_GAP_MS 150u
// ---- Quiet mode (MODE_MUSIC only): no beat for this long -> bracelets FADE ----
#define QUIET_AFTER_MS 20000u
// ---- Beat gate. beat.h ships noise_floor = 2e6, which on this board sits far
// below the room: measured on hardware 2026-08-07, an idle room already ran
// e = 1e5..6e7 with transients to 1.8e9, so the gate never closed and the
// detector free-ran at its 240 BPM refractory ceiling. This sits above the
// measured ambient and below music. Re-check it against the `e=` range in the
// RTT `spec` line at the venue. ----
#define BEAT_NOISE_FLOOR 200000000ull
#define BEAT_REFRACTORY_MS 250u
// beat.h defaults to 1.3x the running average, which on this board still let
// the detector fire on nearly every block (measured 2026-08-07: a continuous
// 180-250 BPM, i.e. pinned at the refractory ceiling, in a room with no music
// playing). 1.3x is simply not selective enough for a signal this spiky.
// Unlike the floor, this is a relative test, so it stays meaningful at any
// volume. Lower it toward 13 if the venue's music is too compressed to
// produce 1.9x transients. NOT yet verified on hardware.
#define BEAT_RATIO_NUM 19u
#define BEAT_RATIO_DEN 10u
// beat.h's EMA memory is alpha_shift=4, about 16 blocks / 256 ms. That is
// short enough that the running average chases the signal itself, so a beat
// never stands far above "recent loudness" and the ratio test degenerates.
// 6 gives ~64 blocks (~1 s) of memory, which is the timescale a beat should
// be measured against.
#define BEAT_AVG_SHIFT 6u
// ---- OneWili neopixel mirror ----
// LED-manager indices pushed per frame. SEVEN, not sixteen: 16 is the DISPLAY
// CPU's own WS2812 count, but ow_gui_set_led_color talks to the MAIN CPU's LED
// manager, and FreeWili's docs state "FREE-WILi has 7 full color LEDs ... The
// LED pattern can also be sent out GPIO pins to support LED Light strips."
// Driving 16 was wrong twice over: it more than doubled the per-frame link
// traffic, and indices 7..15 would very likely be rejected — which, before the
// consecutive-failure tolerance in core1_neopixel(), tore the link down and
// reopened it every single frame.
#define OW_LEDS         7
#define OW_POST_EVERY   3         // post every Nth frame (~20 Hz)
// ---- IMU ----
#define IMU_EVERY       3         // read the accelerometer every Nth frame (~20 Hz)

// RGB565 -> panel wire order (big-endian).
#define BE(v) ((uint16_t)(((uint16_t)(v) >> 8) | ((uint16_t)(v) << 8)))
#define COL_BLACK BE(0x0000)
#define COL_WHITE BE(0xFFFF)
#define COL_HDR   BE(0x18E3)
#define COL_BAR   BE(0x07E0)
#define COL_BARBG BE(0x2104)
#define COL_DIM   BE(0x8410)
#define COL_RED   BE(0xF800)
#define COL_YEL   BE(0xFFE0)
#define COL_HEART     BE(0xF80B)
#define COL_HEART_DIM BE(0x9008)
#define COL_BTN     BE(0x2945)    // idle button face
#define COL_BTN_ON  BE(0x051F)    // selected button face

#define BAR_X 8
#define BAR_Y 146
#define BAR_W 464
#define BAR_H 34

#define BTN_Y 240
#define BTN_H 72
#define BTN_W 113
#define BTN_PITCH 119
#define BTN_X0 6
#define NBTN 4
enum { BTN_MUSIC = 0, BTN_PATTERN, BTN_ACCEL, BTN_NEXT };
static const char *k_btn_label[NBTN] = { "MUSIC", "PATTERN", "ACCEL", "NEXT" };

static int16_t s_pcm[NMICS][HOP];
static int16_t s_mono[HOP];       // the four mics summed, for the analyser
static rgb_t   s_fb[NPIX];        // master framebuffer
static spectrum_t s_spec;         // live frequency analysis

// ---------------------------------------------------------------------------
// Bracelet codes
// ---------------------------------------------------------------------------
static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static rgb_t led_for(const char *cname) {
    static const struct { const char *n; rgb_t c; } map[] = {
        { "red",          { 255, 0,   0   } },
        { "orange",       { 255, 80,  0   } },
        { "yellow",       { 255, 180, 0   } },
        { "light_yellow", { 255, 255, 120 } },
        { "light_green",  { 120, 255, 60  } },
        { "green",        { 0,   255, 0   } },
        { "dark_green",   { 0,   140, 40  } },
        { "turquoise",    { 0,   255, 160 } },
        { "cyan",         { 0,   220, 255 } },
        { "blue",         { 0,   0,   255 } },
        { "purple",       { 150, 0,   255 } },
        { "pink",         { 255, 40,  120 } },
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

static uint16_t rgb_to_be565(rgb_t c) {
    uint16_t v = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    return BE(v);
}

static bool s_radio_ok = false;
static uint32_t s_last_tx_ms = 0;

static void transmit(const code_t *c) {
    if (!s_radio_ok || !code_present(c)) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_last_tx_ms < TX_MIN_GAP_MS) return;
    s_last_tx_ms = now;
    uint32_t durs[CODE_EDGES];
    uint32_t n = code_synth(c, durs);
    cc1101_tx_ook_start(TX_FREQ_HZ);
    for (int r = 0; r < TX_REPEATS; r++) ook_tx_send(durs, n, TX_START_LEVEL);
    cc1101_tx_ook_stop();
}

// ---------------------------------------------------------------------------
// OneWili neopixel mirror (core1). Core0 publishes the whole 16-pixel frame
// through a seqlock mailbox; core1 owns the FwGUI link and forwards each pixel
// to the main CPU's LED manager, which mirrors the pattern out to the light
// strip on its IO header. Core1 only ever sends the newest frame, so a slow
// link drops frames instead of queueing them.
// ---------------------------------------------------------------------------
static volatile uint32_t s_ow_seq = 0;     // even = stable; core0 is sole writer
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

// A rejected command is not the same thing as a dead link — the main CPU may
// simply not like one index or argument. Tearing the link down on the first
// non-OK meant one persistently-unhappy LED index reopened the link every
// frame and the mirror never ran. Only give up after this many frames in a row
// fail completely.
#define OW_FAIL_LIMIT 3

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
        if (s_ow_seq != s0) continue;              // torn read — retry
        sent_seq = s0;

        int ok = 0;
        for (int i = 0; i < OW_LEDS; i++) {
            if (ow_gui_set_led_color(&dev, i, px[i][0], px[i][1], px[i][2], 0,
                                     OW_OW_LED_MANAGER_LED_MODE_SIMPLEVALUE) == OW_OK)
                ok++;
        }
        if (ok == 0) {
            if (++consecutive_fail >= OW_FAIL_LIMIT) {
                link = false;                      // reopen loop retries
                s_ow_link_up = false;
                DIAG("onewili: link dropped after %d dead frames\n", consecutive_fail);
            }
        } else {
            consecutive_fail = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// LCD
// ---------------------------------------------------------------------------
static void draw_heart(int x, int y, int s, uint16_t col) {
    static const uint8_t rows[7] = { 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18 };
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 8; c++)
            st7796_fill_rect(x + c * s, y + r * s, s, s,
                             (rows[r] >> (7 - c)) & 1 ? col : COL_HDR);
}

static void draw_bar(uint32_t level /*0..255*/) {
    uint32_t fill = (BAR_W * level) / 255;
    if (fill > (uint32_t)BAR_W) fill = BAR_W;
    st7796_fill_rect(BAR_X, BAR_Y, (int)fill, BAR_H, COL_BAR);
    st7796_fill_rect(BAR_X + (int)fill, BAR_Y, BAR_W - (int)fill, BAR_H, COL_BARBG);
}

// 16-band spectrum analyser across the bar area, colored like the LEDs so the
// screen and the room match.
#define SBAR_PITCH (BAR_W / SPEC_BANDS)
#define SBAR_W     (SBAR_PITCH - 4)
static void draw_spectrum(const spectrum_t *s) {
    for (int b = 0; b < SPEC_BANDS; b++) {
        int x = BAR_X + b * SBAR_PITCH;
        int h = (s->band[b] * BAR_H) / 255;
        if (h > BAR_H) h = BAR_H;
        rgb_t c = rgb_scale8(disco_wheel((uint8_t)((b * 200) / SPEC_BANDS)), 255);
        st7796_fill_rect(x, BAR_Y, SBAR_W, BAR_H - h, COL_BARBG);
        if (h) st7796_fill_rect(x, BAR_Y + BAR_H - h, SBAR_W, h, rgb_to_be565(c));
    }
}

// Draw `s` padded/truncated to `width` characters so it erases what was there.
static void draw_padded(int x, int y, int scale, uint16_t fg, uint16_t bg,
                        const char *s, int width) {
    char buf[40];
    if (width > (int)sizeof(buf) - 1) width = (int)sizeof(buf) - 1;
    int i = 0;
    while (i < width && s[i]) { buf[i] = s[i]; i++; }
    while (i < width) buf[i++] = ' ';
    buf[i] = 0;
    st7796_draw_text(x, y, scale, fg, bg, buf);
}

static void draw_button(int i, bool selected) {
    int x = BTN_X0 + i * BTN_PITCH;
    st7796_fill_rect(x, BTN_Y, BTN_W, BTN_H, selected ? COL_BTN_ON : COL_BTN);
    const char *s = k_btn_label[i];
    int len = 0; while (s[len]) len++;
    int tw = len * 12;                              // scale 2 -> 12 px per char
    st7796_draw_text(x + (BTN_W - tw) / 2, BTN_Y + (BTN_H - 14) / 2, 2,
                     COL_WHITE, selected ? COL_BTN_ON : COL_BTN, s);
}

// ---------------------------------------------------------------------------
int main(void) {
    board_init();
    st7796_init();
    board_backlight_set(1);
    bool touch_ok = ft6336_init();
    bool imu_ok   = bmi323_init();

    // Onboard LEDs: pio0 SM0. strip_pio_base_setup() first — it widens pio0's
    // GPIO window when the optional external strip lives above GPIO 31.
    strip_pio_base_setup();
    ws2812_init(pio0, 0, PIN_LED_DATA);
    ws2812_set_brightness(0);
    bool strip_ok = strip_init();      // pio0 SM1; false unless enabled in strip.h

    pdm_capture_init();                // mics on pio1 + free-running DMA
    spectrum_init();                   // FFT twiddle/window tables

    ioexp_antenna(TX_ANTENNA);
    s_radio_ok = cc1101_init();

    multicore_launch_core1(core1_neopixel);

    // Bracelet colors to cycle through on the beat (present ones only).
    static const char *palette[] = {
        "red", "orange", "yellow", "light_yellow", "light_green", "green",
        "dark_green", "turquoise", "cyan", "blue", "purple", "pink",
    };
    int color_idx[16]; int ncolor = 0;
    for (unsigned i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
        int idx = code_index(palette[i]);
        if (idx >= 0 && code_present(&g_codes[idx])) color_idx[ncolor++] = idx;
    }
    if (!ncolor) color_idx[0] = 0;     // keep color_idx[pal] in-bounds either way
    int strobe_idx = code_index("strobe");
    int fade_idx   = code_index("fade");
    // Bracelet command that best matches each free-running pattern.
    static const char *pat_code[PAT_COUNT] = {
        "rainbow", "qblink", NULL /*wipe: use the palette color*/, "strobe",
        "fade", "fade" };

    // ---- Static screen ----
    st7796_fill_screen(COL_BLACK);
    st7796_fill_rect(0, 0, ST7796_W, 44, COL_HDR);
    st7796_draw_text(124, 11, 3, COL_WHITE, COL_HDR, "WEDDING DISCO");
    draw_heart(60, 8, 4, COL_HEART_DIM);
    draw_heart(388, 8, 4, COL_HEART_DIM);
    st7796_draw_text(16,  52, 1, COL_DIM, COL_BLACK, "BPM");
    st7796_draw_text(200, 52, 1, COL_DIM, COL_BLACK, "COLOR");
    st7796_draw_text(352, 52, 1, COL_DIM, COL_BLACK, "STATUS");
    st7796_draw_text(352, 66, 2, s_radio_ok ? COL_WHITE : COL_RED, COL_BLACK,
                     s_radio_ok ? "RADIO OK  " : "RADIO FAIL");
    st7796_draw_text(352, 102, 2, imu_ok ? COL_WHITE : COL_DIM, COL_BLACK,
                     imu_ok ? "IMU OK    " : "IMU --    ");
    st7796_draw_text(8, 222, 1, COL_DIM, COL_BLACK,
                     "TITLE BAR = BRACELET STROBE   MIDDLE = NEXT COLOR");
    if (!ncolor)
        st7796_draw_text(8, 232, 1, COL_RED, COL_BLACK, "NO COLORS IN codes.h");
    if (!touch_ok)
        st7796_draw_text(200, 232, 1, COL_RED, COL_BLACK, "TOUCH INIT FAILED");
    for (int i = 0; i < NBTN; i++) draw_button(i, i == BTN_MUSIC);
    DIAG("wedding_disco up: radio=%s touch=%s imu=%s strip=%s colors=%d\n",
         s_radio_ok ? "ok" : "FAIL", touch_ok ? "ok" : "FAIL",
         imu_ok ? "ok" : "FAIL", strip_ok ? "local" : "onewili", ncolor);

    beat_state_t bs; beat_init(&bs);
    beat_cfg_t cfg = beat_cfg_default();
    cfg.noise_floor   = BEAT_NOISE_FLOOR;  // see the notes by the #defines
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
    uint32_t beat_ema_ms = 0, shown_bpm = 0xFFFFFFFF, pos_q8 = 0;
    int16_t tilt_x = 0, tilt_y = 0;
    uint8_t shake = 0;
    float px_g = 0, py_g = 0, pz_g = 1.0f;
    bool imu_primed = false;
    int  last_sent_pal = -1;
    bool hearts_lit = false, was_touched = false, link_shown = false;
    bool force_status = true;
    int  shown_btn = BTN_MUSIC;
    uint32_t raw_lo = 0xFFFFFFFFu, raw_hi = 0, e_lo = 0xFFFFFFFFu, e_hi = 0;
    absolute_time_t next_lcd = get_absolute_time();

    rgb_t cur = led_for(g_codes[color_idx[0]].cname);

    for (;;) {
        frame++;
        // --- One gap-free analysis block from all mics (also our frame clock:
        // 256 samples @ 16 kHz = 16 ms, ~62 fps, in every mode) ---
        int16_t *dst[NMICS] = { s_pcm[0], s_pcm[1], s_pcm[2], s_pcm[3] };
        pdm_capture_block(dst, HOP);
        uint64_t e = 0;
        for (int m = 0; m < NMICS; m++) {
            dcblock_inplace(s_pcm[m], HOP);
            e += beat_energy(s_pcm[m], HOP);
        }
        // Sum the four mics to one channel (better SNR than any single mic)
        // and run the frequency analysis that drives the reactive modes.
        for (unsigned i = 0; i < HOP; i++) {
            int32_t v = (int32_t)s_pcm[0][i] + s_pcm[1][i] + s_pcm[2][i] + s_pcm[3][i];
            s_mono[i] = (int16_t)(v >> 2);
        }
        spectrum_feed(s_mono, HOP, &s_spec);
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool is_beat = beat_feed(&bs, &cfg, e, now) && ncolor > 0;

        // --- Auto-gain the level (snap up, ease down) ---
        if (e > peak) peak = (uint32_t)(e > 0xFFFFFFFFu ? 0xFFFFFFFFu : e);
        else peak -= (peak >> 6);
        if (peak < 1) peak = 1;
        uint32_t target = (e >= peak) ? 255u : (uint32_t)((e * 255u) / peak);
        if (target > level) level = target;
        else level = level - ((level - target) >> 2);

        // --- Accelerometer ---
        if (imu_ok && (frame % IMU_EVERY) == 0) {
            bmi323_reading_t r;
            if (bmi323_read(&r) && r.valid) {
                int32_t tx_ = (int32_t)(r.ax * 256.0f);
                int32_t ty_ = (int32_t)(r.ay * 256.0f);
                tilt_x = (int16_t)(tx_ < -256 ? -256 : tx_ > 256 ? 256 : tx_);
                tilt_y = (int16_t)(ty_ < -256 ? -256 : ty_ > 256 ? 256 : ty_);
                if (imu_primed) {
                    // Shake = how fast the acceleration vector is changing.
                    float d = (r.ax - px_g) * (r.ax - px_g)
                            + (r.ay - py_g) * (r.ay - py_g)
                            + (r.az - pz_g) * (r.az - pz_g);
                    int32_t s = (int32_t)(d * 900.0f);
                    if (s > 255) s = 255;
                    if (s > (int32_t)shake) shake = (uint8_t)s;
                }
                px_g = r.ax; py_g = r.ay; pz_g = r.az;
                imu_primed = true;
            }
        }
        if (shake) shake -= (uint8_t)(shake > 12 ? shake / 8 + 1 : shake);

        // --- Touch (edge-triggered) ---
        uint16_t tx, ty;
        bool touched = touch_ok && ft6336_poll(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < 44) {                                    // title bar: strobe
                strobe_on = !strobe_on;
                if (strobe_on && strobe_idx >= 0) transmit(&g_codes[strobe_idx]);
                else { transmit(&g_codes[color_idx[pal]]); last_sent_pal = pal; }
                force_status = true;
                DIAG("strobe %s\n", strobe_on ? "ON" : "off");
            } else if (ty >= BTN_Y) {                         // button row
                int b = (tx - BTN_X0) / BTN_PITCH;
                if (b < 0) b = 0;
                if (b >= NBTN) b = NBTN - 1;
                if (b == BTN_NEXT) {
                    uint8_t n = disco_variant_count(mode);
                    variant[mode] = (uint8_t)((variant[mode] + 1) % n);
                    if (mode == MODE_PATTERN && !strobe_on) {
                        const char *cn = pat_code[variant[mode] % PAT_COUNT];
                        int ci = cn ? code_index(cn) : color_idx[pal];
                        if (ci >= 0) transmit(&g_codes[ci]);
                    }
                } else {
                    disco_mode_t want = (disco_mode_t)b;       // 0/1/2 map to the modes
                    if (want == mode && mode == MODE_MUSIC) auto_mode = !auto_mode;
                    else if (want != mode) {
                        mode = want;
                        if (mode == MODE_MUSIC) quiet = false;
                        if (mode == MODE_PATTERN && !strobe_on) {
                            const char *cn = pat_code[variant[mode] % PAT_COUNT];
                            int ci = cn ? code_index(cn) : color_idx[pal];
                            if (ci >= 0) transmit(&g_codes[ci]);
                        }
                    }
                }
                force_status = true;
            } else if (ncolor) {              // middle of the screen: next color
                pal = (pal + 1) % ncolor;
                auto_mode = false;            // picking a color pins it
                cur = led_for(g_codes[color_idx[pal]].cname);
                if (!strobe_on) { transmit(&g_codes[color_idx[pal]]); last_sent_pal = pal; }
                force_status = true;
            }
        }
        was_touched = touched;

        // --- Beat handling (MODE_MUSIC drives the palette; the others just
        // keep the BPM readout and the beat flash alive) ---
        if (is_beat) {
            beats++;
            uint32_t dt = now - last_beat_ms;
            last_beat_ms = now;
            if (dt >= 200 && dt <= 2000)
                beat_ema_ms = beat_ema_ms ? (beat_ema_ms * 3 + dt) / 4 : dt;
            if (quiet) { quiet = false; force_status = true; }
            if (mode == MODE_MUSIC && auto_mode) {
                pal = (pal + 1) % ncolor;
                cur = led_for(g_codes[color_idx[pal]].cname);
                if (!strobe_on) { transmit(&g_codes[color_idx[pal]]); last_sent_pal = pal; }
                force_status = true;
            }
            level = 255;
            DIAG("beat #%u %s e=%u bpm~%u\n", beats, g_codes[color_idx[pal]].label,
                 (unsigned)(e >> 8), beat_ema_ms ? (unsigned)(60000 / beat_ema_ms) : 0);
        }

        // --- Quiet mode: only meaningful when we're following the music ---
        if (mode == MODE_MUSIC) {
            if (!quiet && now - last_beat_ms > QUIET_AFTER_MS) {
                quiet = true;
                if (!strobe_on && fade_idx >= 0) transmit(&g_codes[fade_idx]);
                force_status = true;
                DIAG("quiet -> FADE\n");
            }
            if (quiet) {
                uint32_t t = (now >> 4) & 0x1FF;
                uint32_t tri_ = t < 256 ? t : 511 - t;
                level = 30 + (tri_ >> 2);           // gentle breathe
            }
        } else if (quiet) {
            quiet = false;
        }

        // --- Render one frame of the current mode ---
        uint32_t interval = beat_ema_ms ? beat_ema_ms : 500;
        // One comet revolution per ~4 beats in MUSIC; a steady crawl otherwise.
        pos_q8 += (mode == MODE_MUSIC)
                    ? (NPIX * 16u * 256u) / (4u * interval)
                    : 90u;
        // In quiet mode the spectrum is gated to nothing, so fall back to the
        // beat look — that's what carries the gentle breathing.
        uint8_t vshow = quiet ? (uint8_t)MUS_BEAT : variant[mode];
        disco_frame_t f = {
            .color = cur, .level = (uint8_t)level, .pos_q8 = pos_q8,
            .ms_since_beat = now - last_beat_ms, .now_ms = now,
            .tilt_x = tilt_x, .tilt_y = tilt_y, .shake = shake,
            .variant = vshow, .spec = &s_spec,
        };
        disco_render(mode, &f, s_fb, NPIX);

        // Master brightness. The spectrum-driven variants already shade every
        // pixel by its own band level, so scaling them again by the overall
        // level would flatten the very detail we're trying to show — only the
        // beat look wants a global energy envelope.
        bool energy_scaled = (mode == MODE_MUSIC && vshow == MUS_BEAT);
        uint8_t master = energy_scaled ? (uint8_t)(level < 24 ? 24 : level) : 200;
        ws2812_set_brightness(master);
        for (uint32_t i = 0; i < NPIX; i++) ws2812_set_pixel(i, s_fb[i]);
        ws2812_show();
        strip_show(s_fb, NPIX, master);
        if ((frame % OW_POST_EVERY) == 0) ow_post(s_fb, NPIX);

        // --- Bracelets follow the animation in the non-music modes ---
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
                if (s_last_tx_ms != before) {      // actually went out (not rate-limited)
                    last_sent_pal = best;
                    force_status = true;
                }
            }
        }

        // --- Soundcheck aid: the numbers for SPEC_NOISE_FLOOR (spectrum.h) and
        // BEAT_NOISE_FLOOR (above). Report the min/max across the whole window
        // rather than an instantaneous sample — a snapshot lands wherever it
        // lands, and a quiet block caught just after a loud one reads as a low
        // `raw` beside still-decaying bands, which looks like a bug and isn't.
        // Read it in silence first, then with the PA at dance volume: set each
        // floor between the two windows' ranges. ---
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

        // --- LCD at ~15 Hz ---
        if (absolute_time_diff_us(get_absolute_time(), next_lcd) <= 0) {
            next_lcd = make_timeout_time_ms(66);
            if (mode == MODE_ACCEL)      draw_bar((uint32_t)((tilt_x + 256) * 255 / 512));
            else if (mode == MODE_MUSIC) draw_spectrum(&s_spec);
            else                         draw_bar(level);
            uint32_t bpm = (beat_ema_ms && !quiet) ? 60000 / beat_ema_ms : 0;
            if (bpm != shown_bpm) {
                char t[4];
                if (bpm) {
                    t[0] = (char)('0' + (bpm / 100) % 10);
                    t[1] = (char)('0' + (bpm / 10) % 10);
                    t[2] = (char)('0' + bpm % 10);
                    if (t[0] == '0') t[0] = ' ';
                } else { t[0] = ' '; t[1] = '-'; t[2] = '-'; }
                t[3] = 0;
                st7796_draw_text(16, 66, 5, COL_WHITE, COL_BLACK, t);
                shown_bpm = bpm;
            }
            bool lit = (now - last_beat_ms) < 120;
            if (lit != hearts_lit) {
                hearts_lit = lit;
                uint16_t hc = lit ? COL_HEART : COL_HEART_DIM;
                draw_heart(60, 8, 4, hc);
                draw_heart(388, 8, 4, hc);
            }
            if (s_ow_link_up != link_shown || force_status) {
                link_shown = s_ow_link_up;
                const char *s = strip_ok ? "PIXELS GPIO"
                                         : (link_shown ? "PIXELS OK  " : "PIXELS --  ");
                st7796_draw_text(352, 84, 2, (strip_ok || link_shown) ? COL_WHITE : COL_DIM,
                                 COL_BLACK, s);
            }
            // The swatch tracks whatever the animation is actually showing.
            rgb_t swatch = (mode == MODE_MUSIC) ? cur : disco_frame_average(s_fb, NPIX);
            st7796_fill_rect(200, 64, 120, 38, rgb_to_be565(swatch));

            if (shown_btn != (int)mode) {          // repaint only on a mode change
                for (int i = 0; i < NBTN; i++)
                    if (i != BTN_NEXT) draw_button(i, i == (int)mode);
                shown_btn = (int)mode;
            }
            if (force_status) {
                char line[32]; int p = 0;
                const char *mn = disco_mode_name(mode);
                while (*mn && p < 24) line[p++] = *mn++;
                line[p++] = ':'; line[p++] = ' ';
                const char *vn = disco_variant_name(mode, variant[mode]);
                while (*vn && p < 30) line[p++] = *vn++;
                line[p] = 0;
                draw_padded(16, 112, 2, COL_WHITE, COL_BLACK, line, 26);
                draw_padded(16, 190, 2, COL_WHITE, COL_BLACK,
                            mode == MODE_MUSIC
                              ? (auto_mode ? g_codes[color_idx[pal]].label : "HOLD COLOR")
                              : "", 12);
                draw_padded(200, 190, 2, strobe_on ? COL_YEL : COL_DIM, COL_BLACK,
                            strobe_on ? "STROBE!" : quiet ? "QUIET" : "", 10);
                force_status = false;
            }
        }
    }
}
