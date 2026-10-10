# wilibsp (vendored)

Free Wili 2 board-support code vendored from
[`freewili/wilibsp`](https://github.com/freewili/wilibsp). Only the pieces this
project builds against are kept:

- `bsp/` — the `freewili2_bsp` static library (display, radio, LEDs, input,
  audio, sensors, USB host, platform) plus the board header in `bsp/boards/`.
- `libs/` — `onewili` and `beat` support libraries.

The sample and diagnostic apps from the upstream tree are **not** vendored; this
repo builds its own app under `../../src`.

## Local modifications

- `bsp/radio/cc1101.c` — monitor-RX demod rate fixed (`MDMCFG4=0x87`,
  `MDMCFG3=0x43`, ~4 kbaud) so the OOK capture path decodes the 433.92 MHz
  remote instead of returning noise. Restored on `monitor_stop`.

To refresh from upstream, re-copy `bsp/` and `libs/` and re-apply the change
above.
