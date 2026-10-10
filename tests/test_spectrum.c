// Host test for spectrum.c: feed known tones, check the right bands respond.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "spectrum.h"

#define FS 16000.0

static int fails = 0;
static void check(const char *what, int ok) {
    printf("%-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

// Feed a steady tone for `blocks` blocks so the envelope settles.
static void feed_tone(spectrum_t *s, double hz, double amp, int blocks) {
    static double ph = 0;
    int16_t pcm[SPEC_FFT_N];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < SPEC_FFT_N; i++) {
            pcm[i] = (int16_t)(amp * sin(ph));
            ph += 2.0 * M_PI * hz / FS;
        }
        spectrum_feed(pcm, SPEC_FFT_N, s);
    }
}

static void feed_silence(spectrum_t *s, int blocks) {
    int16_t pcm[SPEC_FFT_N];
    memset(pcm, 0, sizeof(pcm));
    for (int b = 0; b < blocks; b++) spectrum_feed(pcm, SPEC_FFT_N, s);
}

static int peak_band(const spectrum_t *s) {
    int best = 0;
    for (int b = 1; b < SPEC_BANDS; b++) if (s->band[b] > s->band[best]) best = b;
    return best;
}

static void dump(const char *tag, const spectrum_t *s) {
    printf("  %-12s ", tag);
    for (int b = 0; b < SPEC_BANDS; b++) printf("%3d ", s->band[b]);
    printf(" | bass=%3d mid=%3d treb=%3d loud=%3d raw=%u\n",
           s->bass, s->mid, s->treble, s->loud, (unsigned)s->raw_peak);
}

int main(void) {
    spectrum_init();
    spectrum_t s;
    memset(&s, 0, sizeof(s));

    // A low tone must peak in a low band, a high tone in a high band, and the
    // two peaks must be clearly separated - that is the whole point.
    feed_tone(&s, 120.0, 8000, 60);
    dump("120 Hz", &s);
    int lo = peak_band(&s);
    check("120 Hz tone peaks in the lower half", lo < SPEC_BANDS / 2);
    check("120 Hz tone drives bass > treble", s.bass > s.treble);

    feed_silence(&s, 200);
    feed_tone(&s, 5000.0, 8000, 60);
    dump("5 kHz", &s);
    int hi = peak_band(&s);
    check("5 kHz tone peaks in the upper half", hi > SPEC_BANDS / 2);
    check("5 kHz tone drives treble > bass", s.treble > s.bass);
    check("the two tones peak in different bands", hi > lo + 3);

    // A mid tone should land between them.
    feed_silence(&s, 200);
    feed_tone(&s, 1000.0, 8000, 60);
    dump("1 kHz", &s);
    int mid = peak_band(&s);
    check("1 kHz peaks between the 120 Hz and 5 kHz peaks", mid > lo && mid < hi);

    // Silence must actually go dark (the noise gate), or the LEDs never rest.
    feed_silence(&s, 400);
    dump("silence", &s);
    int maxv = 0;
    for (int b = 0; b < SPEC_BANDS; b++) if (s.band[b] > maxv) maxv = s.band[b];
    check("silence decays to dark", maxv < 20);

    // Loudness must track amplitude monotonically.
    feed_silence(&s, 300);
    feed_tone(&s, 800.0, 1000, 80);
    int quiet_loud = s.loud;
    feed_tone(&s, 800.0, 12000, 80);
    int loud_loud = s.loud;
    printf("  loud: quiet=%d loud=%d\n", quiet_loud, loud_loud);
    check("a louder tone reads louder", loud_loud > quiet_loud);

    // Full-scale input must not overflow into garbage: a 400 Hz tone at the
    // CIC's full scale must still peak in a low band, not wrap into a high one.
    feed_silence(&s, 300);
    feed_tone(&s, 400.0, 16383, 60);
    dump("full scale", &s);
    check("full-scale 400 Hz still peaks low (no overflow wrap)",
          peak_band(&s) < 8 && s.loud > 100);
    printf("  full-scale raw magnitude = %u (scale ref for SPEC_NOISE_FLOOR)\n",
           (unsigned)s.raw_peak);

    // A quiet VU must not read as a full one.
    feed_silence(&s, 400);
    feed_tone(&s, 300.0, 300, 60);
    int vu_quiet = s.loud;
    feed_silence(&s, 400);
    feed_tone(&s, 300.0, 14000, 60);
    int vu_full = s.loud;
    printf("  VU: quiet-tone=%d full-tone=%d\n", vu_quiet, vu_full);
    check("VU meter spans a useful range", vu_full > 140 && vu_quiet < vu_full);

    // Noise below the floor must not be auto-gained up to full brightness.
    feed_silence(&s, 500);
    feed_tone(&s, 700.0, 12, 200);          // a whisper, well under the floor
    dump("sub-floor", &s);
    int m2 = 0;
    for (int b = 0; b < SPEC_BANDS; b++) if (s.band[b] > m2) m2 = s.band[b];
    check("sub-floor noise is not amplified to full", m2 < 128);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
