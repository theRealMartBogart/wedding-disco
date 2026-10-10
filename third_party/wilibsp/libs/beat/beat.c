// beat.c — see beat.h. Pure integer, no SDK dependency.
#include "beat.h"

void beat_init(beat_state_t *s) {
    s->avg_energy   = 0;
    s->last_beat_ms = 0;
    s->primed       = false;
}

uint64_t beat_energy(const int16_t *pcm, unsigned n) {
    uint64_t acc = 0;
    for (unsigned i = 0; i < n; i++) {
        int64_t v = pcm[i];
        acc += (uint64_t)(v * v);
    }
    return acc;
}

bool beat_feed(beat_state_t *s, const beat_cfg_t *cfg, uint64_t e, uint32_t now_ms) {
    // Seed the average on the very first block so we don't fire on startup.
    if (!s->primed) {
        s->avg_energy   = e;
        s->last_beat_ms = now_ms - cfg->refractory_ms;  // allow an immediate first beat
        s->primed       = true;
        return false;
    }

    bool is_beat = false;
    // now_ms is monotonic; unsigned wrap on the difference is fine for our spans.
    uint32_t since = now_ms - s->last_beat_ms;
    if (e > cfg->noise_floor &&
        e * cfg->ratio_den > s->avg_energy * cfg->ratio_num &&
        since >= cfg->refractory_ms) {
        is_beat = true;
        s->last_beat_ms = now_ms;
    }

    // Update the running average toward the current block (EMA via shifts).
    if (e > s->avg_energy) s->avg_energy += (e - s->avg_energy) >> cfg->alpha_shift;
    else                   s->avg_energy -= (s->avg_energy - e) >> cfg->alpha_shift;

    return is_beat;
}
