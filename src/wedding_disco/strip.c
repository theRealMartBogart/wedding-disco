// strip.c — see strip.h for the wiring/config instructions. Compiles to
// nothing but stubs when STRIP_ENABLE is 0.
#include "strip.h"

#if STRIP_ENABLE

#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "strip.pio.h"

static PIO  s_pio = pio0;
static uint s_sm;
static bool s_up = false;

void strip_pio_base_setup(void) {
#if STRIP_PIN >= 32
    // Window 16..47 still covers PIN_LED_DATA (21), so the onboard ring is fine.
    pio_set_gpio_base(pio0, 16);
#endif
}

bool strip_init(void) {
    int sm = pio_claim_unused_sm(s_pio, false);
    if (sm < 0) return false;
    s_sm = (uint)sm;
    uint offset = pio_add_program(s_pio, &strip_ws2812_program);
    strip_ws2812_program_init(s_pio, s_sm, offset, STRIP_PIN, 800000.0f);
#if STRIP_INVERT
    gpio_set_outover(STRIP_PIN, GPIO_OVERRIDE_INVERT);
#endif
    s_up = true;
    return true;
}

bool strip_present(void) { return s_up; }

void strip_show(const rgb_t *fb, uint32_t n, uint8_t brightness) {
    if (!s_up || n == 0) return;
    for (uint32_t i = 0; i < STRIP_LEDS; i++) {
        rgb_t src = fb[(i * n) / STRIP_LEDS];       // nearest-neighbour resample
        rgb_t s = { led_color_scale(src.r, brightness),
                    led_color_scale(src.g, brightness),
                    led_color_scale(src.b, brightness) };
        pio_sm_put_blocking(s_pio, s_sm, led_color_pack_grb(s) << 8u);
    }
    // The >=280 us WS2812 latch is covered by the caller's frame time (~16 ms).
}

#else   /* STRIP_ENABLE == 0 — the OneWili mirror is doing the job */

void strip_pio_base_setup(void) {}
bool strip_init(void) { return false; }
bool strip_present(void) { return false; }
void strip_show(const rgb_t *fb, uint32_t n, uint8_t brightness) {
    (void)fb; (void)n; (void)brightness;
}

#endif
