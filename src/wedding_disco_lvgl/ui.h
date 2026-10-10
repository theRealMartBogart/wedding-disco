// ui.h — the LVGL screen for wedding_disco_lvgl.
//
// Same information as the hand-drawn ST7796 UI in the original app, but built
// from LVGL widgets: a 16-column spectrum analyser, the BPM readout, a live
// color swatch, status lines, and a real button row with press feedback and
// active-state styling.
//
// The build/update split matters for the real-time loop: ui_build() creates
// every widget once at startup, and ui_update() only sets values and styles on
// widgets that already exist. Nothing is allocated or freed per frame, so
// LVGL's heap never churns while the mics are running.
#ifndef WEDDING_UI_H
#define WEDDING_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "patterns.h"
#include "spectrum.h"

// What the app tells the UI each frame.
typedef struct {
    disco_mode_t      mode;
    uint8_t           variant;
    const spectrum_t *spec;
    rgb_t             color;        // current bracelet/palette color
    const char       *color_name;
    uint32_t          bpm;          // 0 = no lock yet
    bool              beat;         // true briefly after each detected beat
    bool              auto_cycle;
    bool              strobe;
    bool              quiet;
    bool              radio_ok;
    bool              pixels_ok;    // OneWili link up, or a local strip present
    const char       *pixels_label;
    int16_t           tilt_x;       // for the ACCEL-mode bar
} ui_state_t;

// What the user did. The app polls this and clears it.
typedef enum {
    UI_EV_NONE = 0,
    UI_EV_MODE_MUSIC, UI_EV_MODE_PATTERN, UI_EV_MODE_ACCEL,
    UI_EV_NEXT_VARIANT,
    UI_EV_NEXT_COLOR,
    UI_EV_TOGGLE_STROBE,
} ui_event_t;

void       ui_build(void);
void       ui_set_radio(bool ok); // one-shot startup status, never changes after
void       ui_update(const ui_state_t *s);
ui_event_t ui_take_event(void);   // returns and clears the pending event

#endif // WEDDING_UI_H
