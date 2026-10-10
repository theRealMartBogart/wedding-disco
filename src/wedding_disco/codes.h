// codes.h — compact form: each code is its 56-bit OOK frame body
// (7 bytes). The full duration timeline is synthesized at TX from one
// shared PWM template (code_synth). ~98% smaller than storing arrays.
// Bytes decoded from the verified duration captures by scratchpad/gen_compact.py.
#ifndef RF_REPLAY_CODES_H
#define RF_REPLAY_CODES_H
#include <stdint.h>
#include <stdbool.h>

#define CODE_BITS   56
#define CODE_EDGES  (4 + CODE_BITS * 2)   // 2-pulse preamble + 56 x (high,low) = 116

typedef struct { const char *label; const char *cname; uint8_t body[7]; } code_t;
static const code_t g_codes[] = {
    { "OFF", "off", { 0x00,0xFF,0xFF,0x0F,0x00,0xAA,0xB7 } },
    { "RED", "red", { 0x00,0xFF,0xFF,0x0F,0x01,0x00,0x0E } },
    { "GREEN", "green", { 0x00,0xFF,0xFF,0x0F,0x01,0x01,0x0F } },
    { "BLUE", "blue", { 0x00,0xFF,0xFF,0x0F,0x01,0x02,0x10 } },
    { "CYAN", "cyan", { 0x00,0xFF,0xFF,0x0F,0x01,0x0B,0x19 } },
    { "ORANGE", "orange", { 0x00,0xFF,0xFF,0x0F,0x01,0x09,0x17 } },
    { "PINK", "pink", { 0x00,0xFF,0xFF,0x0F,0x01,0x03,0x11 } },
    { "PURPLE", "purple", { 0x00,0xFF,0xFF,0x0F,0x01,0x08,0x16 } },
    { "YELLOW", "yellow", { 0x00,0xFF,0xFF,0x0F,0x01,0x05,0x13 } },
    { "WHITE", "white", { 0x00,0xFF,0xFF,0x0F,0x01,0x04,0x12 } },
    // FADE (0xAABD): a "mode" command — one send makes the strip breathe the
    // current color on its own. Live-captured clean frame (the remote streams it
    // repeatedly while active, but the single frame is the command).
    { "FADE", "fade", { 0x00,0xFF,0xFF,0x0F,0x06,0xAA,0xBD } },
    { "RAINBOW", "rainbow", { 0xC0,0x01,0x53,0xBD,0x98,0x24,0x8D } },
    { "TURQUOISE", "turquoise", { 0x00,0xFF,0xFF,0x0F,0x01,0x0D,0x1B } },
    { "DK GREEN", "dark_green", { 0x00,0xFF,0xFF,0x0F,0x01,0x0E,0x1C } },
    { "LT GREEN", "light_green", { 0x00,0xFF,0xFF,0x0F,0x01,0x07,0x15 } },
    { "LT YELLOW", "light_yellow", { 0x00,0xFF,0xFF,0x0F,0x01,0x0C,0x1A } },
    // Effect modes: one send triggers the strip's built-in effect.
    { "STROBE", "strobe", { 0x00,0xFF,0xFF,0x0F,0x44,0xAA,0xFB } },   // 0xAAFB
    { "QBLINK", "qblink", { 0x00,0xFF,0xFF,0x0F,0x33,0xAA,0xEA } },   // 0xAAEA
};
#define NCODES ((int)(sizeof(g_codes)/sizeof(g_codes[0])))

// PWM timing template (us), median of this remote's captured frames.
// Synthesize a code's full edge timeline into `durs` (>= CODE_EDGES); returns count.
static inline uint32_t code_synth(const code_t *c, uint32_t *durs) {
    uint32_t n = 0;
    durs[n++] = 1490; durs[n++] = 970; durs[n++] = 1045; durs[n++] = 500;  // 2-pulse preamble
    for (int i = 0; i < CODE_BITS; i++) {
        int bit = (c->body[i >> 3] >> (7 - (i & 7))) & 1;
        if (bit) { durs[n++] = 527; durs[n++] = 218; }   // '1' long-high
        else     { durs[n++] = 278; durs[n++] = 466; }   // '0' short-high
    }
    return n;
}
static inline bool code_present(const code_t *c) { (void)c; return true; }
#endif
