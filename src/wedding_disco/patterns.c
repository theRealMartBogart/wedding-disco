// patterns.c — see patterns.h. Pure integer renderers, no hardware.
#include "patterns.h"

rgb_t rgb_scale8(rgb_t c, uint8_t k) {
    rgb_t o = { (uint8_t)((c.r * k) / 255),
                (uint8_t)((c.g * k) / 255),
                (uint8_t)((c.b * k) / 255) };
    return o;
}

// Three 85-step ramps around the wheel: R->G, G->B, B->R.
rgb_t disco_wheel(uint8_t hue) {
    rgb_t c;
    if (hue < 85)        { c.r = (uint8_t)(255 - hue * 3); c.g = (uint8_t)(hue * 3);       c.b = 0; }
    else if (hue < 170)  { hue -= 85;  c.r = 0; c.g = (uint8_t)(255 - hue * 3); c.b = (uint8_t)(hue * 3); }
    else                 { hue -= 170; c.r = (uint8_t)(hue * 3); c.g = 0; c.b = (uint8_t)(255 - hue * 3); }
    return c;
}

// Cheap deterministic hash — sparkle/candle need per-pixel noise without state.
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Triangle wave 0..255 from a free-running counter, period `period` units.
static uint8_t tri(uint32_t t, uint32_t period) {
    if (period == 0) return 255;
    uint32_t p = t % period;
    uint32_t half = period / 2;
    if (half == 0) return 255;
    uint32_t up = (p < half) ? p : period - p;
    return (uint8_t)((up * 255) / half);
}

const char *disco_mode_name(disco_mode_t m) {
    switch (m) {
        case MODE_MUSIC:   return "MUSIC";
        case MODE_PATTERN: return "PATTERN";
        case MODE_ACCEL:   return "ACCEL";
        default:           return "?";
    }
}

uint8_t disco_variant_count(disco_mode_t m) {
    switch (m) {
        case MODE_MUSIC:   return MUS_COUNT;
        case MODE_PATTERN: return PAT_COUNT;
        case MODE_ACCEL:   return ACC_COUNT;
        default:           return 1;
    }
}

const char *disco_variant_name(disco_mode_t m, uint8_t v) {
    if (m == MODE_MUSIC) {
        static const char *n[MUS_COUNT] = { "SPECTRUM", "VU", "BASS", "BEAT" };
        return n[v % MUS_COUNT];
    }
    if (m == MODE_PATTERN) {
        static const char *n[PAT_COUNT] = {
            "RAINBOW", "CHASE", "WIPE", "SPARKLE", "CANDLE", "PULSE" };
        return n[v % PAT_COUNT];
    }
    if (m == MODE_ACCEL) {
        static const char *n[ACC_COUNT] = { "TILT HUE", "SHAKE", "LEVEL" };
        return n[v % ACC_COUNT];
    }
    return "";
}

// --- MODE_MUSIC ------------------------------------------------------------
// MUS_BEAT: dim base, bright comet head fading back over a quarter of the
// pixels, whole-strip flash for DISCO_FLASH_MS after each beat.
static void render_beat(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    if (f->ms_since_beat < DISCO_FLASH_MS) {
        for (uint32_t i = 0; i < n; i++) fb[i] = f->color;
        return;
    }
    uint32_t head = (f->pos_q8 >> 8) % n;
    uint32_t tail = n / 4; if (tail < 3) tail = 3;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = (head + n - i) % n;          // pixels behind the head
        uint32_t k = 40;                          // dim base glow
        if (d < tail) {
            uint32_t bright = 255 - (215 * d) / tail;
            if (bright > k) k = bright;
        }
        fb[i] = rgb_scale8(f->color, (uint8_t)k);
    }
}

// Read band `i` of `n` from the analyser, which always produces SPEC_BANDS.
static uint8_t band_at(const spectrum_t *s, uint32_t i, uint32_t n) {
    return s->band[(i * SPEC_BANDS) / (n ? n : 1)];
}

// MUS_SPECTRUM: one pixel per frequency band. Hue walks red (bass) through the
// spectrum to violet (cymbals), brightness is that band's live level, so the
// whole strip is a moving picture of the music.
static void render_spectrum(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    const spectrum_t *s = f->spec;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = band_at(s, i, n);
        uint8_t hue = (uint8_t)((i * 200) / n);       // red -> green -> blue
        uint8_t k = v < 12 ? 12 : v;                  // floor so it never dies
        fb[i] = rgb_scale8(disco_wheel(hue), k);
    }
}

// MUS_VU: classic loudness meter, growing out from the centre in both
// directions, green through yellow to red at the extremes.
static void render_vu(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    const spectrum_t *s = f->spec;
    uint32_t half = (n + 1) / 2;
    uint32_t lit = (s->loud * half) / 255;            // pixels lit each side
    for (uint32_t i = 0; i < n; i++) {
        // Distance from the centre, 0 at the middle out to half-1 at the ends.
        uint32_t d = (i < half) ? (half - 1 - i) : (i - half);
        if (d < lit) {
            uint8_t frac = (uint8_t)((d * 255) / (half ? half : 1));
            rgb_t c = { frac, (uint8_t)(255 - frac), 0 };   // green -> red
            fb[i] = c;
        } else {
            rgb_t off = { 6, 2, 0 };
            fb[i] = off;
        }
    }
}

// MUS_BASS: the kick drives the whole strip in the current palette color,
// while hi-hats and cymbals throw white glints on top.
static void render_bass(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    const spectrum_t *s = f->spec;
    uint8_t k = s->bass < 20 ? 20 : s->bass;
    rgb_t body = rgb_scale8(f->color, k);
    uint32_t tick = f->now_ms / 50;
    for (uint32_t i = 0; i < n; i++) {
        fb[i] = body;
        if (s->treble > 90) {
            uint32_t h = hash32(i * 2654435761u + tick);
            if ((h & 0xFF) < (uint32_t)(s->treble - 90)) {
                uint8_t w = s->treble;
                rgb_t glint = { w, w, (uint8_t)(w > 200 ? w : 200) };
                fb[i] = glint;
            }
        }
    }
}

static void render_music(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    // Every spectrum-driven variant needs the analyser; fall back to the
    // beat-only look if it isn't running.
    if (!f->spec) { render_beat(f, fb, n); return; }
    switch (f->variant % MUS_COUNT) {
        case MUS_SPECTRUM: render_spectrum(f, fb, n); break;
        case MUS_VU:       render_vu(f, fb, n);       break;
        case MUS_BASS:     render_bass(f, fb, n);     break;
        case MUS_BEAT:
        default:           render_beat(f, fb, n);     break;
    }
}

// --- MODE_PATTERN ----------------------------------------------------------
static void render_pattern(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    uint32_t phase = f->pos_q8 >> 8;
    switch (f->variant % PAT_COUNT) {

    case PAT_RAINBOW:                             // full spectrum, rotating
        for (uint32_t i = 0; i < n; i++)
            fb[i] = disco_wheel((uint8_t)(phase + (i * 256) / n));
        break;

    case PAT_CHASE: {                             // theater chase, every 3rd lit
        uint32_t off = phase % 3;
        rgb_t lit = disco_wheel((uint8_t)(f->now_ms / 40));
        rgb_t off_c = { 0, 0, 0 };
        for (uint32_t i = 0; i < n; i++) fb[i] = (i % 3 == off) ? lit : off_c;
        break;
    }

    case PAT_WIPE: {                              // color wipes across, new hue each pass
        uint32_t pass = phase / n;
        uint32_t head = phase % n;
        rgb_t now_c  = disco_wheel((uint8_t)(pass * 40));
        rgb_t prev_c = disco_wheel((uint8_t)((pass - 1) * 40));
        for (uint32_t i = 0; i < n; i++) fb[i] = (i <= head) ? now_c : prev_c;
        break;
    }

    case PAT_SPARKLE: {                           // white glints over a dim wash
        rgb_t wash = rgb_scale8(disco_wheel((uint8_t)(f->now_ms / 60)), 30);
        uint32_t tick = f->now_ms / 70;           // new sparkle set ~14x/sec
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = hash32(i * 2654435761u + tick);
            if ((h & 0xFF) < 28) { rgb_t w = { 255, 255, 230 }; fb[i] = w; }
            else fb[i] = wash;
        }
        break;
    }

    case PAT_CANDLE: {                            // warm flicker — dinner / slow songs
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = hash32(i * 0x9E3779B9u + f->now_ms / 110);
            uint8_t k = (uint8_t)(150 + (h & 0x69));      // 150..255
            rgb_t warm = { 255, (uint8_t)(110 + (h >> 8 & 0x1F)), 20 };
            fb[i] = rgb_scale8(warm, k);
        }
        break;
    }

    case PAT_PULSE:                               // whole strip breathing
    default: {
        uint8_t k = tri(f->now_ms, 2600);
        if (k < 25) k = 25;
        rgb_t c = disco_wheel((uint8_t)(f->now_ms / 90));
        for (uint32_t i = 0; i < n; i++) fb[i] = rgb_scale8(c, k);
        break;
    }
    }
}

// --- MODE_ACCEL ------------------------------------------------------------
static void render_accel(const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    switch (f->variant % ACC_COUNT) {

    case ACC_TILT_HUE: {                          // tilt sweeps the color wheel
        uint8_t hue = (uint8_t)(((f->tilt_x + 256) * 255) / 512);
        uint8_t k   = (uint8_t)(60 + (f->tilt_y + 256) * 195 / 512);
        rgb_t c = rgb_scale8(disco_wheel(hue), k);
        for (uint32_t i = 0; i < n; i++) fb[i] = c;
        break;
    }

    case ACC_SHAKE: {                             // shake it to flash the room
        uint8_t k = f->shake < 20 ? 20 : f->shake;
        rgb_t c = disco_wheel((uint8_t)(f->now_ms / 25));
        rgb_t lit = rgb_scale8(c, k);
        if (f->shake > 160) { rgb_t w = { 255, 255, 255 }; lit = w; }
        for (uint32_t i = 0; i < n; i++) fb[i] = lit;
        break;
    }

    case ACC_LEVEL:                               // bubble level: dot follows tilt
    default: {
        int32_t dot = ((f->tilt_x + 256) * (int32_t)(n - 1)) / 512;
        if (dot < 0) dot = 0;
        if (dot > (int32_t)n - 1) dot = (int32_t)n - 1;
        rgb_t dim = rgb_scale8(f->color, 18);
        for (uint32_t i = 0; i < n; i++) {
            int32_t d = (int32_t)i - dot; if (d < 0) d = -d;
            if (d == 0)      fb[i] = f->color;
            else if (d == 1) fb[i] = rgb_scale8(f->color, 90);
            else if (d == 2) fb[i] = rgb_scale8(f->color, 35);
            else             fb[i] = dim;
        }
        break;
    }
    }
}

void disco_render(disco_mode_t m, const disco_frame_t *f, rgb_t *fb, uint32_t n) {
    if (n == 0) return;
    switch (m) {
        case MODE_PATTERN: render_pattern(f, fb, n); break;
        case MODE_ACCEL:   render_accel(f, fb, n);   break;
        case MODE_MUSIC:
        default:           render_music(f, fb, n);   break;
    }
}

rgb_t disco_frame_average(const rgb_t *fb, uint32_t n) {
    if (n == 0) { rgb_t z = { 0, 0, 0 }; return z; }
    uint32_t r = 0, g = 0, b = 0;
    for (uint32_t i = 0; i < n; i++) { r += fb[i].r; g += fb[i].g; b += fb[i].b; }
    rgb_t o = { (uint8_t)(r / n), (uint8_t)(g / n), (uint8_t)(b / n) };
    return o;
}
