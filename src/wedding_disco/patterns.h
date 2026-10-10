// patterns.h — the three LED modes and their variants.
//
// Every renderer is pure integer math that fills a caller-supplied framebuffer
// of arbitrary length, so the same animation drives the 16 onboard LEDs, the
// external neopixel strip (any length), and the OneWili mirror from one call.
// No hardware, no floats — easy to reason about and cheap in the 16 ms budget.
#ifndef WEDDING_PATTERNS_H
#define WEDDING_PATTERNS_H
#include <stdint.h>
#include <stdbool.h>
#include "leds/led_color.h"
#include "spectrum.h"

typedef enum {
    MODE_MUSIC = 0,   // reactive: follows the live spectrum and the beat
    MODE_PATTERN,     // free-running animations, ignores the mics
    MODE_ACCEL,       // driven by the BMI323 accelerometer
    MODE_COUNT
} disco_mode_t;

// MODE_MUSIC variants — how the lights react to the sound.
typedef enum {
    MUS_SPECTRUM = 0, // one pixel per frequency band: bass low, cymbals high
    MUS_VU,           // loudness meter growing out from the centre
    MUS_BASS,         // bass drives the palette color, hats sparkle white
    MUS_BEAT,         // flash on the beat, comet between beats
    MUS_COUNT
} disco_music_t;

// MODE_PATTERN variants
typedef enum {
    PAT_RAINBOW = 0, PAT_CHASE, PAT_WIPE, PAT_SPARKLE, PAT_CANDLE, PAT_PULSE,
    PAT_COUNT
} disco_pattern_t;

// MODE_ACCEL variants
typedef enum {
    ACC_TILT_HUE = 0,   // tilt left/right sweeps the color wheel
    ACC_SHAKE,          // shake the board to flash the room
    ACC_LEVEL,          // bubble-level: a lit dot tracks the tilt
    ACC_COUNT
} disco_accel_t;

typedef struct {
    rgb_t    color;          // current palette color (MODE_MUSIC / PAT_WIPE)
    uint8_t  level;          // 0..255 room energy (drives brightness)
    uint32_t pos_q8;         // animation phase in 8.8 fixed-point pixels
    uint32_t ms_since_beat;
    uint32_t now_ms;
    int16_t  tilt_x, tilt_y; // -256..256, from the accelerometer
    uint8_t  shake;          // 0..255 shake intensity
    uint8_t  variant;        // variant index within the mode
    const spectrum_t *spec;  // live frequency analysis; NULL if unavailable
} disco_frame_t;

// Whole-ring flash window right after a beat.
#define DISCO_FLASH_MS 90u

const char *disco_mode_name(disco_mode_t m);
const char *disco_variant_name(disco_mode_t m, uint8_t v);
uint8_t     disco_variant_count(disco_mode_t m);

// Fill fb[0..n-1] for this mode/frame.
void disco_render(disco_mode_t m, const disco_frame_t *f, rgb_t *fb, uint32_t n);

// The single color that best represents a rendered frame — what the bracelets
// and the LCD swatch show when the strip is doing something more elaborate.
rgb_t disco_frame_average(const rgb_t *fb, uint32_t n);

rgb_t rgb_scale8(rgb_t c, uint8_t k);   // c * k/255, per channel
rgb_t disco_wheel(uint8_t hue);         // 0..255 hue -> saturated rgb

#endif // WEDDING_PATTERNS_H
