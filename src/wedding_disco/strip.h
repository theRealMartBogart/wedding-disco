// strip.h - OPTIONAL external neopixel strip driven directly from a
// display-CPU GPIO.
//
// ===========================================================================
//  THIS IS THE FALLBACK PATH FOR THE NEOPIXELS.  Configure it here.
// ===========================================================================
// By default the app mirrors its animation to the neopixels over the OneWili
// link: display CPU -> main CPU LED manager -> the main CPU's GPIO light-strip
// output. That path depends on the stock main-CPU firmware having "light strip
// control" enabled, and on setBoardLED still reaching the strip after it
// disables the Light Show app - both unverified on this board.
//
// If the OneWili mirror doesn't light your strip, wire the strip to a spare
// display-CPU GPIO instead and turn this on. It is fully independent of the
// main CPU: our own PIO state machine, our own pixel count, any length. Both
// paths can run at once - they render from the same framebuffer.
//
//   1. Set STRIP_ENABLE to 1.
//   2. Set STRIP_PIN to the GPIO you wired the strip's DATA line to.
//      *** VERIFY THE PIN AGAINST YOUR BOARD FIRST. *** Every GPIO used by the
//      BSP is listed in third_party/wilibsp/bsp/platform/board.h; GPIOs 23, 31,
//      34, 35, 36, 41, 44 and 45 are not claimed by any driver there, but the
//      BSP docs do not say which (if any) reach an accessible pad, so confirm
//      with a meter or the schematic before soldering.
//   3. Set STRIP_LEDS to your strip length.
//   4. Power the strip from its own 5 V supply (not the board) beyond a few
//      pixels, and tie its ground to the board's ground.
//
// Note: the board's own 16 LEDs sit behind an inverting buffer, which is why
// the BSP driver flips the output. A bare strip is NOT inverted, so this
// driver drives the pin straight. If your strip shows inverted colors, set
// STRIP_INVERT to 1.
//
// Cost: pushing the strip is blocking, about 30 us per pixel. 60 pixels is
// ~1.8 ms of the 16 ms frame - fine. Past ~200 pixels you will start eating
// into the mic budget; drop to every other frame if so.
#ifndef WEDDING_STRIP_H
#define WEDDING_STRIP_H
#include <stdint.h>
#include <stdbool.h>
#include "leds/led_color.h"

#define STRIP_ENABLE  0      // <-- 1 to drive a local strip from STRIP_PIN
#define STRIP_PIN     31     // <-- GPIO wired to the strip DATA line (VERIFY!)
#define STRIP_LEDS    60     // <-- pixels on the strip
#define STRIP_INVERT  0      // <-- 1 if your strip needs an inverted data line

// Must be called BEFORE ws2812_init() when STRIP_PIN >= 32: a PIO block can
// only address 32 consecutive GPIOs, so reaching a high pin means moving pio0's
// window to 16..47. GPIO 21 (the onboard LEDs) is inside both windows, so the
// onboard ring is unaffected either way. No-op when disabled or unnecessary.
void strip_pio_base_setup(void);

// Claims a pio0 state machine and starts the strip. Returns false when
// STRIP_ENABLE is 0 (nothing is claimed) or no SM is free.
bool strip_init(void);
bool strip_present(void);

// Push one frame. `fb` holds `n` pixels of the master animation; it is
// resampled across STRIP_LEDS, so the strip shows the same pattern as the
// onboard ring at whatever length it happens to be. No-op when disabled.
void strip_show(const rgb_t *fb, uint32_t n, uint8_t brightness);

#endif // WEDDING_STRIP_H
