// beat.h — integer energy-based beat detector for PartyLEDs (Phase C).
//
// Pure logic, no Pico SDK / hardware / float-print dependency, so it builds for
// the RP2350 target AND runs under `fw test` on the host. The idea is the
// classic "sound energy" beat detector: track a slow running average of
// per-block energy; a block whose energy jumps well above that average (and
// isn't inside the refractory window after the last beat) is a beat.
#ifndef BEAT_H
#define BEAT_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint64_t avg_energy;    // EMA of per-block energy (the "recent loudness")
    uint32_t last_beat_ms;  // timestamp of the last detected beat
    bool     primed;        // false until the first block seeds avg_energy
} beat_state_t;

typedef struct {
    uint32_t alpha_shift;   // EMA smoothing: avg += (e-avg) >> alpha_shift (larger = slower)
    uint32_t refractory_ms; // minimum spacing between beats (debounces one hit)
    uint32_t ratio_num;     // beat when e*ratio_den > avg*ratio_num ...
    uint32_t ratio_den;     //   e.g. 13/10 => energy must exceed 1.3x the average
    uint64_t noise_floor;   // ... and e must clear this (ignore ambient/silence)
} beat_cfg_t;

// Sensible starting point; tune noise_floor to your room from the RTT energy log.
static inline beat_cfg_t beat_cfg_default(void) {
    beat_cfg_t c;
    c.alpha_shift   = 4;        // ~16-block memory
    c.refractory_ms = 200;      // <= 300 BPM ceiling; blocks double-triggers
    c.ratio_num     = 13;       // 1.3x
    c.ratio_den     = 10;
    c.noise_floor   = 2000000u; // depends on block size/mic gain — calibrate
    return c;
}

void beat_init(beat_state_t *s);

// Sum-of-squares energy of one PCM block (host-tested helper).
uint64_t beat_energy(const int16_t *pcm, unsigned n);

// Feed one block's energy at time now_ms. Returns true exactly on a detected
// beat. Updates the running average every call regardless.
bool beat_feed(beat_state_t *s, const beat_cfg_t *cfg, uint64_t e, uint32_t now_ms);

#endif // BEAT_H
