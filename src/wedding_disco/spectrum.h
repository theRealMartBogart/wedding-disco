// spectrum.h — live frequency analysis of the room, for genuinely reactive
// lighting.
//
// The beat detector answers "is there a beat right now?" — one bit, a few times
// a second. That is not enough to make lights feel alive between beats. This
// runs a 256-point integer FFT on every 16 ms mic block and hands back 16
// log-spaced band levels, so the LEDs can follow the actual sound continuously:
// bass in the low pixels, vocals in the middle, hats and cymbals up top.
//
// Everything is fixed-point and self-contained (only the twiddle tables are
// built with libm at init), so it costs well under a millisecond per frame and
// can be reasoned about without hardware.
#ifndef WEDDING_SPECTRUM_H
#define WEDDING_SPECTRUM_H
#include <stdint.h>

#define SPEC_FFT_N   256    // = HOP: one analysis block, ~16 ms @ 16 kHz
#define SPEC_BANDS   16     // = the onboard LED count, one band per pixel

// TUNE THIS TO THE VENUE. Each band is auto-gained against its own decaying
// peak, which is what lets quiet cymbals show up next to a loud kick — but it
// also means that without a floor, room hiss would eventually be amplified to
// full brightness. A band whose raw magnitude never reaches this stays dark.
// Too low and chatter lights the strip between songs; too high and quiet
// passages go black. Calibrate from the `spec raw=lo..hi` line in the RTT log:
// take the range in an empty room, then with the PA at dance volume, and set
// this comfortably above the quiet range. Content above the floor still
// normalises to full brightness (the per-band peak takes over), so the floor
// only decides what counts as "too quiet to show".
//
// For scale: a full-scale input tone reads about 4000. Measured on hardware
// 2026-08-07, an idle room ran raw = 5..61 — the original value of 64 sat
// right in that band and lit the strip on ambient noise alone. 400 leaves
// that ambient at under 15% brightness.
#define SPEC_NOISE_FLOOR 400u

typedef struct {
    uint8_t  band[SPEC_BANDS];  // 0..255 per band, attack-fast / decay-smooth
    uint8_t  bass, mid, treble; // aggregates of the low / middle / high bands
    uint8_t  loud;              // overall loudness, for the VU meter
    uint32_t raw_peak;          // loudest band BEFORE auto-gain — for tuning
                                // SPEC_NOISE_FLOOR against your room
} spectrum_t;

// Builds the twiddle/window/bit-reversal tables. Call once after board_init().
void spectrum_init(void);

// Analyse one block of `n` samples (n is clamped to SPEC_FFT_N; anything
// shorter is zero-padded) and update `out`. The caller keeps `out` between
// calls — the smoothing and auto-gain state lives in this module.
void spectrum_feed(const int16_t *pcm, unsigned n, spectrum_t *out);

#endif // WEDDING_SPECTRUM_H
