// spectrum.c — 256-point integer FFT -> 16 log-spaced bands. See spectrum.h.
//
// Fixed-point notes. Input from the CIC decimator peaks near +/-16384, and each
// FFT stage scales its output by 1/2, so magnitudes never grow: every
// intermediate stays inside +/-16384 and the twiddle products stay under 2^30,
// well clear of int32 overflow. The 1/2-per-stage scaling costs low-level
// detail, which the per-band auto-gain below wins back.
#include "spectrum.h"
#include <math.h>
#include <string.h>

#define HALF (SPEC_FFT_N / 2)

static int32_t s_re[SPEC_FFT_N], s_im[SPEC_FFT_N];
static int16_t s_cos[HALF], s_sin[HALF];   // Q15 twiddles
static int16_t s_win[SPEC_FFT_N];          // Q15 Hann window
static uint8_t s_brev[SPEC_FFT_N];
static uint8_t s_edge[SPEC_BANDS + 1];     // band -> first FFT bin
static uint32_t s_peak[SPEC_BANDS];        // per-band decaying peak (auto-gain)
static uint8_t s_prev[SPEC_BANDS];         // for decay smoothing
static int s_ready = 0;

void spectrum_init(void) {
    for (int i = 0; i < HALF; i++) {
        double a = (2.0 * M_PI * i) / SPEC_FFT_N;
        s_cos[i] = (int16_t)lrint(cos(a) * 32767.0);
        s_sin[i] = (int16_t)lrint(sin(a) * 32767.0);
    }
    for (int i = 0; i < SPEC_FFT_N; i++) {
        double w = 0.5 - 0.5 * cos((2.0 * M_PI * i) / (SPEC_FFT_N - 1));
        s_win[i] = (int16_t)lrint(w * 32767.0);
        unsigned r = 0, v = (unsigned)i;
        for (int b = 0; b < 8; b++) { r = (r << 1) | (v & 1u); v >>= 1; }
        s_brev[i] = (uint8_t)r;
    }
    // Log-spaced band edges over bins 1..HALF-1 (62.5 Hz .. 7.94 kHz).
    for (int b = 0; b <= SPEC_BANDS; b++) {
        double f = (double)b / SPEC_BANDS;
        double bin = 1.0 * pow((double)(HALF - 1), f);   // 1 -> 127, geometric
        int v = (int)lrint(bin);
        if (v < b + 1) v = b + 1;                        // keep bands non-empty
        if (v > HALF - 1) v = HALF - 1;
        s_edge[b] = (uint8_t)v;
    }
    for (int b = 0; b < SPEC_BANDS; b++) { s_peak[b] = 1; s_prev[b] = 0; }
    s_ready = 1;
}

// |z| ~= max + 3/8*min — the alpha-max-beta-min estimate, ~4% error, no sqrt.
static uint32_t magnitude(int32_t re, int32_t im) {
    uint32_t a = (uint32_t)(re < 0 ? -re : re);
    uint32_t b = (uint32_t)(im < 0 ? -im : im);
    uint32_t mx = a > b ? a : b, mn = a > b ? b : a;
    return mx + (mn * 3) / 8;
}

void spectrum_feed(const int16_t *pcm, unsigned n, spectrum_t *out) {
    if (!s_ready) { memset(out, 0, sizeof(*out)); return; }
    if (n > SPEC_FFT_N) n = SPEC_FFT_N;

    // Window into bit-reversed order; zero-pad a short block.
    for (unsigned i = 0; i < SPEC_FFT_N; i++) {
        int32_t v = (i < n) ? ((int32_t)pcm[i] * s_win[i]) >> 15 : 0;
        s_re[s_brev[i]] = v;
        s_im[s_brev[i]] = 0;
    }

    // Radix-2 decimation-in-time, 1/2 scaling per stage.
    for (int s = 1; s <= 8; s++) {
        int m = 1 << s, half = m >> 1, step = SPEC_FFT_N / m;
        for (int k = 0; k < SPEC_FFT_N; k += m) {
            for (int j = 0; j < half; j++) {
                int t = j * step;
                int32_t wr = s_cos[t], wi = -s_sin[t];      // e^(-i2pi j/m)
                int32_t xr = s_re[k + j + half], xi = s_im[k + j + half];
                int32_t tr = (xr * wr - xi * wi) >> 15;
                int32_t ti = (xr * wi + xi * wr) >> 15;
                int32_t ur = s_re[k + j], ui = s_im[k + j];
                s_re[k + j]        = (ur + tr) >> 1;
                s_im[k + j]        = (ui + ti) >> 1;
                s_re[k + j + half] = (ur - tr) >> 1;
                s_im[k + j + half] = (ui - ti) >> 1;
            }
        }
    }

    // Bins -> bands, each with its own decaying peak so quiet high bands still
    // show detail instead of sitting dark under the bass.
    uint32_t sum = 0, peak_v = 0, raw_peak = 0;
    for (int b = 0; b < SPEC_BANDS; b++) {
        int lo = s_edge[b], hi = s_edge[b + 1];
        if (hi <= lo) hi = lo + 1;
        uint32_t acc = 0;
        for (int k = lo; k < hi; k++) acc += magnitude(s_re[k], s_im[k]);
        acc /= (uint32_t)(hi - lo);
        if (acc > raw_peak) raw_peak = acc;

        if (acc > s_peak[b]) s_peak[b] = acc;
        else s_peak[b] -= (s_peak[b] >> 7);              // slow release (~1.5 s)
        if (s_peak[b] < SPEC_NOISE_FLOOR) s_peak[b] = SPEC_NOISE_FLOOR;

        uint32_t v = (acc >= s_peak[b]) ? 255u : (acc * 255u) / s_peak[b];
        // Reactive envelope: jump straight to a rise, glide down from a fall.
        uint8_t prev = s_prev[b];
        uint8_t nv = (v > prev) ? (uint8_t)v : (uint8_t)(prev - ((prev - v) >> 2));
        s_prev[b] = nv;
        out->band[b] = nv;
        sum += nv;
        if (nv > peak_v) peak_v = nv;
    }
    out->raw_peak = raw_peak;

    // Aggregates. The splits are by musical role, not equal thirds:
    // bass = bins 1..4 (62-250 Hz, the kick), treble = bins 38..126
    // (2.4-7.9 kHz, hats and cymbals), mid = everything between.
    uint32_t lo_s = 0, mid_s = 0, hi_s = 0;
    for (int b = 0;  b < 4;  b++) lo_s  += out->band[b];
    for (int b = 4;  b < 12; b++) mid_s += out->band[b];
    for (int b = 12; b < SPEC_BANDS; b++) hi_s += out->band[b];
    out->bass   = (uint8_t)(lo_s / 4);
    out->mid    = (uint8_t)(mid_s / 8);
    out->treble = (uint8_t)(hi_s / 4);
    // Blend the mean with the loudest band: a plain mean of 16 independently
    // gained bands barely moves off the floor, so a VU built on it would only
    // ever fill a third of the strip. This tracks the way a meter should.
    out->loud   = (uint8_t)((sum / SPEC_BANDS + peak_v) / 2);
}
