# Wedding Disco

Two apps, one lighting engine:

| Target | UI | RAM (of 512 K) |
|---|---|---|
| `wedding_disco` | Hand-drawn ST7796 rectangles and text | 98 K (19%) |
| `wedding_disco_lvgl` | LVGL 9.3 widgets — real buttons with press feedback, styled bar-widget analyser | 465 K (91%) |

`patterns.c`, `spectrum.c`, `strip.c` and `codes.h` are **compiled into both**
from `src/wedding_disco/` — shared source, not copies, so the lighting
behaviour can never drift between them. Only the screen differs.

The LVGL variant costs about 360 K of code, which is most of the SRAM budget
(everything runs `copy_to_ram`). It fits with ~47 K to spare. If you extend the
UI and the link overflows, shrink `PORT_BUF_ROWS` in
`src/wedding_disco_lvgl/lvgl_port.c` first — that only costs redraw speed,
whereas shrinking `LV_MEM_SIZE` risks a silent allocation failure with LVGL's
logging compiled out. The app reports its real heap high-water mark over RTT at
startup (`lvgl heap: ... used_pct=`) so you can see how much margin you have.

Two things the LVGL build has to be careful about, both handled:
the CC1101 shares SPI1 with the panel and the LVGL flush is an async DMA
transfer, so every radio transmit waits for the flush to drain; and
`lv_timer_handler()` is unbounded while the PDM ring only has ~64 ms of slack,
so rendering runs every third audio frame (~21 Hz) and only touches widgets
whose values actually changed.


A Free Wili 2 (RP2350B display CPU) app that turns the board into the brain of
a wedding disco. Every frame renders one animation into a 16-pixel master
framebuffer, which then drives all four outputs at once:

- **16 onboard WS2812 LEDs** — the reference display.
- **External neopixel strip** — mirrored over the **OneWili API** to the main
  CPU's LED manager (which sends the pattern out its GPIO to the light strip),
  or driven straight off a display-CPU GPIO instead — see
  [the strip fallback](#the-neopixel-fallback).
- **DEFCON RF LED bracelets** — the nearest matching remote code, replayed over
  the on-board CC1101 at 433.92 MHz OOK. Codes verified in the `partyremote`
  project; `src/wedding_disco/codes.h` is a copy of its `rf_replay/codes.h`.
- **LCD** — title with beat-pulsing hearts, live BPM, VU/tilt bar, color
  swatch, mode line, status column, and the touch button row.

## Three light modes

Pick one from the on-screen buttons. **NEXT** always steps the current mode's
variant, so one button does the right thing everywhere.

| Mode | What it does | NEXT steps |
|---|---|---|
| **MUSIC** | Reactive. A 256-point FFT on every 16 ms mic block splits the room into 16 frequency bands, and the lights follow the sound continuously. | SPECTRUM → VU → BASS → BEAT |
| **PATTERN** | Six free-running animations that ignore the mics — for dinner, speeches, and slow songs. | RAINBOW → CHASE → WIPE → SPARKLE → CANDLE → PULSE |
| **ACCEL** | BMI323 accelerometer. Hand the board to a guest. | TILT HUE (tilt sweeps the color wheel) → SHAKE (shake to flash the room) → LEVEL (a lit dot tracks the tilt) |

The four music variants:

- **SPECTRUM** — one pixel per frequency band, hue running red at the bass end
  through to violet at the cymbals, each pixel's brightness its own band's live
  level. The strip becomes a moving picture of the music.
- **VU** — a loudness meter growing out from the centre, green through yellow
  to red at the extremes.
- **BASS** — the kick drives the whole strip in the current color while hats
  and cymbals throw white glints on top.
- **BEAT** — the original: full-color flash on each detected beat, BPM-paced
  comet between beats.

Each band is auto-gained against its own decaying peak, so a quiet hi-hat still
registers next to a loud kick, and the envelope attacks instantly but glides
down — that's what makes it feel reactive rather than twitchy.

Tapping **MUSIC** while already in music mode toggles AUTO CYCLE / HOLD COLOR.
Tapping the **title bar** toggles bracelet STROBE in any mode. Tapping the
**middle of the screen** steps the bracelet color by hand.

In MUSIC mode, twenty seconds without a beat drops into quiet mode: the
bracelets get one FADE command and the LEDs breathe until the music returns.
The other two modes never go quiet.

Each pattern also sends the bracelets a matching command once when you select
it (RAINBOW→rainbow, CHASE→qblink, SPARKLE→strobe, CANDLE/PULSE→fade), and
ACCEL mode continuously maps the animation's average color to the nearest
bracelet color.

## The neopixel fallback

The default neopixel path goes through OneWili to the main CPU.

**The link itself is verified working** (2026-08-07): the app reports
`onewili: link up, mirroring 7 LEDs` over RTT and every LED write succeeds, so
the FreeWili's own 7 colour LEDs follow the animation. What is still unproven
is only the last hop — whether the main CPU's stock firmware then mirrors that
LED pattern out the GPIO header to a strip. FreeWili's docs say the pattern
*can* be sent out GPIO to support light strips, but also that `setBoardLED`
disables the Light Show, and the Light Show is what does the mirroring.

Auditing the API surface for this turned up two real bugs, both fixed:

- **7 LEDs, not 16.** `ow_gui_set_led_color` talks to the *main* CPU's LED
  manager, which has 7; 16 is the *display* CPU's own WS2812 count. Driving 16
  more than doubled the per-frame link traffic and sent 9 indices that almost
  certainly got rejected.
- **A rejected command was treated as a dead link.** Any single non-OK reply
  tore the link down and reopened it — so those 9 bad indices would have made
  the mirror reconnect every frame and never actually work. It now only gives
  up after `OW_FAIL_LIMIT` whole frames fail in a row.

There is exactly **one** LED command in the entire 158-function OneWili API
(`ow_gui_set_led_color`, wire `g\s`). Everything else matching "color" is LCD
widget styling, and `ow_gui_controls_add_led` is a drawn GUI indicator, not a
physical LED. There are no LED events. So this is the only door, and the app is
already using it.

If it doesn't work, wire the strip to a spare display-CPU GPIO and flip one
switch: **`src/wedding_disco/strip.h`**, set `STRIP_ENABLE` to 1 and set
`STRIP_PIN` / `STRIP_LEDS`. That path is fully independent of the main CPU —
its own PIO state machine on pio0 SM1, any strip length, same animation. It's
build-tested at both low and high GPIO numbers (pins above 31 need pio0's GPIO
window moved, which `strip_pio_base_setup()` handles). Both paths can run
simultaneously.

The LCD status column tells you which is live: `PIXELS GPIO` (local strip),
`PIXELS OK` (OneWili link up), or `PIXELS --` (no link — everything else still
works).

## Requirements at the venue

1. This app flashed to the **display CPU** over SWD (CMSIS-DAP probe).
2. For the OneWili path only: the **main CPU running stock FreeWili 2
   firmware** with light strip control enabled.
3. Neopixels powered from their own 5 V supply beyond a few pixels, grounds
   tied together.

## Build & flash

Requires Pico SDK 2.2.0 + Arm GCC (both under `~/.pico-sdk`). Build and flash
are separate steps:

Both apps are **RAM-only** (`pico_set_binary_type(no_flash)`): the image is
loaded straight into SRAM over SWD and the QSPI flash is never written. The
board reverts to whatever is in flash on the next power cycle, and nothing can
brick it, because nothing erases anything.

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.2.0"
$env:PATH = "$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:PATH"

cmake --preset target                                      # configure
cmake --build --preset target                              # build both apps
python tools/fw.py run wedding_disco                       # load + stream RTT
python tools/fw.py run wedding_disco_lvgl                  # ...or the LVGL one
```

**Use `fw run`, not `fw flash` + `fw rtt`.** `fw flash` still works (it loads
and starts the image, despite the name — it programs no flash), but attaching a
*second* time resets the chip, and a reset throws a RAM-only image away: the
bootrom has nowhere to find it, so the board silently comes up running whatever
is still in flash instead. `fw run` loads and serves RTT from a single attach.

Three things had to be right to start a RAM image, all in `tools/fw.py`:

- Start core 0 at the ELF entry `_entry_point`, **not** the vector table's
  `_reset_handler` — the latter assumes a real hardware reset and bounces
  non-core0 back into the bootrom.
- Let **nothing** run on core 0 between `reset halt` and that resume.
  `_entry_point` enables the FPU via CPACR and immediately uses FP
  instructions, which needs the pristine Secure state a reset leaves. An
  earlier version resumed core 1 first (to park it in the bootrom wait loop);
  that also let the bootrom run on core 0, and core 0 then HardFaulted the
  moment it was resumed. Resuming core 0 alone releases core 1 anyway.
- **No `verify_image`.** OpenOCD verifies by uploading a CRC routine and
  running it *on the target*, which scribbles over the RAM image you just
  loaded and leaves core 0 running.

### Flashing notes (learned the hard way on this board)

- The on-board probe is a **"FreeWili Debug Probe (CMSIS-DAP)"**, `2e8a:000c`,
  exposing four CMSIS-DAP interfaces. `tools/openocd/freewili2.cfg` names the
  VID/PID and lets OpenOCD pick; the `cmsis-dap usb interface 0` line it used
  to carry makes this OpenOCD build reject the device outright.
- **Kill stale OpenOCD before flashing** — an RTT session holds the probe and
  the next flash fails with "Access denied":
  `Get-Process openocd | Stop-Process -Force`.
- The RAM-only build makes the following irrelevant, but it is kept because it
  is how the board gets recovered if anything ever *does* write flash again.
  If a flash write is interrupted mid-erase the boot image is corrupt, the core
  double-faults on reset, and further attempts fail with "Failed to invoke
  rcp_init" / "clearing lockup after double fault". Recover by parking both
  cores in the bootrom first, then programming:

  ```powershell
  $ocd="$env:USERPROFILE\.pico-sdk\openocd\0.12.0+dev"
  & "$ocd\openocd.exe" -s "$ocd\scripts" `
      -c "source [find interface/cmsis-dap.cfg]" -c "cmsis-dap vid_pid 0x2e8a 0x000c" `
      -c "adapter speed 1000" -c "transport select swd" `
      -c "source [find target/rp2350-rescue.cfg]" -c init -c shutdown
  # then flash as usual
  ```

## Tuning for the room

**Do this during soundcheck — it's the difference between lights that dance and
lights that twitch.** Stream RTT and watch the `spec` line, which prints twice a
second:

```
spec raw=412 loud=138 bass=201 mid=96 treb=44 | e=8123
```

Read it twice: once in an empty room, then with the PA at dance volume.

- **`SPEC_NOISE_FLOOR`** (`spectrum.h`) is the one to get right. Set it a little
  above the `raw=` value you see in silence. Too low and chatter lights the
  strip between songs; too high and quiet passages go black. A full-scale input
  tone reads about 4000, so the scale is `64` ≈ 1.5% of full scale.
- **`BEAT_AVG_SHIFT` / `BEAT_NOISE_FLOOR` / `BEAT_RATIO_NUM`** (top of
  `main.c`, overriding `beat_cfg_default()`). With beat.h's shipped defaults
  the detector free-ran at a continuous 180–250 BPM in a room with no music —
  pinned at its refractory ceiling. Raising the floor (2e6 → 2e8) and the
  ratio (1.3x → 1.9x) barely helped; **the fix that mattered was
  `BEAT_AVG_SHIFT` 4 → 6.** beat.h averages energy over only ~16 blocks
  (256 ms), which is short enough that the running average chases the signal
  itself, so a beat never stands out from "recent loudness" and the ratio test
  degenerates. ~1 s of memory is the right timescale. Verified on hardware:
  the same room went from continuous 200+ BPM to 12 beats in 25 s. Expect to
  fine-tune the floor once against real music.
- **`TX_REPEATS`** (`main.c`) — raise to 3 if far bracelets miss codes (costs
  ~46 ms of mic time per extra repeat).
- **`OW_POST_EVERY`** — neopixel refresh over OneWili, in frames (3 ≈ 20 Hz).
  Raise it if the main-CPU link can't keep up.

## Tests

The DSP has no SDK or hardware dependency, so it's verified on the host:

```powershell
python tools/fw.py test
```

`tests/test_spectrum.c` feeds known tones through the analyser and checks that
low and high tones land in the right bands and stay separated, that silence
decays to dark, that sub-floor noise isn't auto-gained up to full brightness,
that the VU spans a useful range, and that a full-scale input doesn't overflow
the fixed-point FFT. It also prints the band table, which is the quickest way
to sanity-check a change to the band layout.

## Layout

```
Wedding/
├─ src/wedding_disco/
│  ├─ main.c        # setup, frame loop, mode/touch state machine, LCD, RF
│  ├─ spectrum.c/h  # 256-pt integer FFT -> 16 bands; TUNE SPEC_NOISE_FLOOR here
│  ├─ patterns.c/h  # the three modes and their variants (pure integer math)
│  ├─ strip.c/h     # OPTIONAL local neopixel strip — the fallback, configure here
│  ├─ strip.pio     # WS2812 program for that strip
│  └─ codes.h       # bracelet OOK codes (copied from partyremote/rf_replay)
├─ src/wedding_disco_lvgl/ # LVGL variant — same engine, widget UI
│  ├─ main.c        # loop (shares patterns/spectrum/strip from ../wedding_disco)
│  ├─ lvgl_port.c/h # ST7796 async-DMA flush + FT6336 indev + tick
│  ├─ ui.c/h        # the screen: build once, update in place
│  └─ lv_conf.h     # trimmed LVGL config (heap size, disabled widgets)
├─ third_party/lvgl/       # vendored LVGL 9.3.0 (src + cmake only)
├─ tests/                  # host tests for the DSP (python tools/fw.py test)
├─ third_party/wilibsp/    # vendored BSP + libs/beat + libs/onewili
├─ tools/                  # fw.py task runner, OpenOCD config
├─ CMakeLists.txt
└─ CMakePresets.json       # "target" preset -> build/ (Ninja, RP2350B)
```

Because every renderer in `patterns.c` is pure integer math over a
caller-supplied framebuffer, adding your own pattern means writing one function
and adding it to the `PAT_*` enum and the name table — no hardware knowledge
needed.

Vendored tree copied from the `partyremote` project (which includes its local
CC1101 monitor-RX patch); treat `third_party/` as external code.
