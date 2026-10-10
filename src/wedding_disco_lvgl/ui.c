// ui.c — see ui.h. Widgets are created once in ui_build(); ui_update() only
// pushes values into them.
#include "ui.h"
#include "lvgl.h"
#include <stdio.h>

#define SCR_W 480
#define SCR_H 320

// Layout bands, top to bottom.
#define HDR_H     44
#define SPEC_Y    118
#define SPEC_H    76
#define BTN_Y     238
#define BTN_H     70
#define BTN_GAP   6

static lv_obj_t *s_bars[SPEC_BANDS];
static lv_obj_t *s_bpm, *s_bpm_cap;
static lv_obj_t *s_swatch, *s_mode_lbl, *s_variant_lbl;
static lv_obj_t *s_radio_lbl, *s_pixels_lbl, *s_state_lbl;
static lv_obj_t *s_hearts_l, *s_hearts_r;
static lv_obj_t *s_btn[4], *s_btn_lbl[4];

static ui_event_t s_pending = UI_EV_NONE;

// Cached last-drawn values: LVGL is happy to be told the same thing twice, but
// each set marks the widget dirty and costs a redraw of that area. The
// spectrum genuinely changes every frame; text mostly does not.
static uint32_t s_last_bpm = 0xFFFFFFFF;
static int      s_last_mode = -1, s_last_variant = -1;
static uint32_t s_last_swatch = 0xFFFFFFFF;
static bool     s_last_beat, s_last_strobe, s_last_quiet, s_last_auto;
static bool     s_last_pixels;
static int      s_last_bar[SPEC_BANDS];

static lv_color_t rgb_lv(rgb_t c) { return lv_color_make(c.r, c.g, c.b); }

static void btn_cb(lv_event_t *e) {
    s_pending = (ui_event_t)(uintptr_t)lv_event_get_user_data(e);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, const char *txt,
                            lv_color_t col, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

void ui_build(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header ----
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, SCR_W, HDR_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x1B2430), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    // Tapping the header toggles bracelet strobe, same as the original UI.
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hdr, btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_EV_TOGGLE_STROBE);

    s_hearts_l = make_label(hdr, 84, 12, LV_SYMBOL_OK, lv_color_hex(0x903040), NULL);
    lv_obj_t *title = make_label(hdr, 0, 10, "WEDDING DISCO",
                                 lv_color_white(), &lv_font_montserrat_28);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    s_hearts_r = make_label(hdr, SCR_W - 104, 12, LV_SYMBOL_OK,
                            lv_color_hex(0x903040), NULL);

    // ---- BPM ----
    s_bpm_cap = make_label(scr, 16, 52, "BPM", lv_color_hex(0x808080), NULL);
    s_bpm = make_label(scr, 16, 68, "--", lv_color_white(), &lv_font_montserrat_28);

    // ---- Color swatch + names ----
    make_label(scr, 150, 52, "COLOR", lv_color_hex(0x808080), NULL);
    s_swatch = lv_obj_create(scr);
    lv_obj_set_size(s_swatch, 96, 34);
    lv_obj_set_pos(s_swatch, 150, 68);
    lv_obj_set_style_border_width(s_swatch, 1, 0);
    lv_obj_set_style_border_color(s_swatch, lv_color_hex(0x404040), 0);
    lv_obj_set_style_radius(s_swatch, 4, 0);
    lv_obj_remove_flag(s_swatch, LV_OBJ_FLAG_SCROLLABLE);
    // Tapping the swatch steps the bracelet color by hand.
    lv_obj_add_flag(s_swatch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_swatch, btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_EV_NEXT_COLOR);

    // ---- Status column ----
    make_label(scr, 290, 52, "STATUS", lv_color_hex(0x808080), NULL);
    s_radio_lbl  = make_label(scr, 290, 68, "RADIO --",  lv_color_hex(0x808080), NULL);
    s_pixels_lbl = make_label(scr, 290, 86, "PIXELS --", lv_color_hex(0x808080), NULL);
    s_state_lbl  = make_label(scr, 400, 68, "",          lv_color_hex(0xFFE000), NULL);

    // ---- Mode / variant line ----
    s_mode_lbl    = make_label(scr, 16, 96, "MUSIC", lv_color_white(), NULL);
    s_variant_lbl = make_label(scr, 90, 96, "SPECTRUM", lv_color_hex(0x00C8FF), NULL);

    // ---- Spectrum: one bar per band, hue matching the LEDs ----
    int pitch = (SCR_W - 16) / SPEC_BANDS;
    for (int b = 0; b < SPEC_BANDS; b++) {
        lv_obj_t *bar = lv_bar_create(scr);
        lv_bar_set_range(bar, 0, 255);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_size(bar, pitch - 5, SPEC_H);
        lv_obj_set_pos(bar, 8 + b * pitch, SPEC_Y);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x101418), LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
        // Same red-through-violet ramp the LEDs use, so screen and room match.
        lv_obj_set_style_bg_color(bar, rgb_lv(disco_wheel((uint8_t)((b * 200) / SPEC_BANDS))),
                                  LV_PART_INDICATOR);
        s_bars[b] = bar;
        s_last_bar[b] = -1;
    }

    // ---- Button row ----
    static const char *labels[4] = { "MUSIC", "PATTERN", "ACCEL", "NEXT" };
    static const ui_event_t evs[4] = { UI_EV_MODE_MUSIC, UI_EV_MODE_PATTERN,
                                       UI_EV_MODE_ACCEL, UI_EV_NEXT_VARIANT };
    int bw = (SCR_W - 2 * 8 - 3 * BTN_GAP) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_size(b, bw, BTN_H);
        lv_obj_set_pos(b, 8 + i * (bw + BTN_GAP), BTN_Y);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3340), 0);
        // LVGL gives us pressed-state feedback for free — the hand-drawn UI
        // had none.
        lv_obj_set_style_bg_color(b, lv_color_hex(0x4A5A70), LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)evs[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
        s_btn[i] = b;
        s_btn_lbl[i] = l;
    }
}

void ui_update(const ui_state_t *s) {
    // --- Spectrum bars: the one thing that really does change every frame.
    // In ACCEL mode the analyser is irrelevant, so the bars become a tilt
    // indicator instead of sitting dead.
    for (int b = 0; b < SPEC_BANDS; b++) {
        int v;
        if (s->mode == MODE_ACCEL) {
            int dot = ((s->tilt_x + 256) * (SPEC_BANDS - 1)) / 512;
            if (dot < 0) dot = 0;
            if (dot > SPEC_BANDS - 1) dot = SPEC_BANDS - 1;
            int d = b - dot; if (d < 0) d = -d;
            v = d == 0 ? 255 : d == 1 ? 110 : d == 2 ? 45 : 12;
        } else {
            v = s->spec ? s->spec->band[b] : 0;
        }
        if (v != s_last_bar[b]) {
            lv_bar_set_value(s_bars[b], v, LV_ANIM_OFF);
            s_last_bar[b] = v;
        }
    }

    // --- BPM ---
    if (s->bpm != s_last_bpm) {
        char buf[8];
        if (s->bpm) snprintf(buf, sizeof(buf), "%u", (unsigned)s->bpm);
        else        snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(s_bpm, buf);
        s_last_bpm = s->bpm;
    }

    // --- Beat: pulse the hearts and the BPM color ---
    if (s->beat != s_last_beat) {
        lv_color_t c = s->beat ? lv_color_hex(0xFF3060) : lv_color_hex(0x903040);
        lv_obj_set_style_text_color(s_hearts_l, c, 0);
        lv_obj_set_style_text_color(s_hearts_r, c, 0);
        s_last_beat = s->beat;
    }

    // --- Color swatch ---
    uint32_t rgbkey = ((uint32_t)s->color.r << 16) | ((uint32_t)s->color.g << 8) | s->color.b;
    if (rgbkey != s_last_swatch) {
        lv_obj_set_style_bg_color(s_swatch, rgb_lv(s->color), 0);
        s_last_swatch = rgbkey;
    }

    // --- Mode / variant text + which button reads as active ---
    if ((int)s->mode != s_last_mode || (int)s->variant != s_last_variant) {
        lv_label_set_text(s_mode_lbl, disco_mode_name(s->mode));
        const char *vn = (s->mode == MODE_MUSIC && s->color_name && !s->auto_cycle)
                           ? s->color_name
                           : disco_variant_name(s->mode, s->variant);
        lv_label_set_text(s_variant_lbl, vn);
        for (int i = 0; i < 3; i++) {
            bool on = (i == (int)s->mode);
            lv_obj_set_style_bg_color(s_btn[i],
                lv_color_hex(on ? 0x0A6EA0 : 0x2A3340), 0);
        }
        s_last_mode = (int)s->mode;
        s_last_variant = (int)s->variant;
    }

    // --- Status ---
    if (s->pixels_ok != s_last_pixels) {
        lv_label_set_text(s_pixels_lbl, s->pixels_label);
        lv_obj_set_style_text_color(s_pixels_lbl,
            s->pixels_ok ? lv_color_white() : lv_color_hex(0x808080), 0);
        s_last_pixels = s->pixels_ok;
    }
    if (s->strobe != s_last_strobe || s->quiet != s_last_quiet ||
        s->auto_cycle != s_last_auto) {
        const char *t = s->strobe ? "STROBE" : s->quiet ? "QUIET"
                      : (s->mode == MODE_MUSIC && !s->auto_cycle) ? "HOLD" : "";
        lv_label_set_text(s_state_lbl, t);
        s_last_strobe = s->strobe; s_last_quiet = s->quiet;
        s_last_auto = s->auto_cycle;
    }
}

ui_event_t ui_take_event(void) {
    ui_event_t e = s_pending;
    s_pending = UI_EV_NONE;
    return e;
}

// Called once from main after ui_build() to set the static status text that
// never changes after startup.
void ui_set_radio(bool ok) {
    lv_label_set_text(s_radio_lbl, ok ? "RADIO OK" : "RADIO FAIL");
    lv_obj_set_style_text_color(s_radio_lbl,
        ok ? lv_color_white() : lv_color_hex(0xFF4040), 0);
}
