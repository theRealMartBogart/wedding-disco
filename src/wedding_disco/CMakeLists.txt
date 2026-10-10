# wedding_disco — mic/accel -> animation -> {onboard LEDs, LCD, bracelet RF,
# neopixels (OneWili link or a local GPIO strip)}. Runs from SRAM.
add_executable(wedding_disco
    main.c
    patterns.c
    spectrum.c
    strip.c
)
target_link_libraries(wedding_disco freewili2_bsp beat onewili_fwgui pico_multicore)
# PIO program for the OPTIONAL local neopixel strip (see strip.h). Generated
# unconditionally; strip.c compiles it out unless STRIP_ENABLE is 1.
pico_generate_pio_header(wedding_disco ${CMAKE_CURRENT_SOURCE_DIR}/strip.pio)
# RAM-ONLY. The image is loaded straight into SRAM over SWD and never written
# to the QSPI flash, so the display CPU's flash contents are left alone and a
# power cycle returns the board to whatever was there before.
#
# Consequences worth remembering:
#   * The app is not persistent — it must be re-loaded after every power cycle.
#   * `program ... reset` is the WRONG way to load it; a reset drops the chip
#     back into the bootrom, which knows nothing about an image that only ever
#     existed in RAM. `tools/fw.py flash` does the load-and-start sequence.
#   * Nothing erases flash, which also removes the mid-erase brick risk that
#     bit us on 2026-08-07 (see the README's flashing notes).
pico_set_binary_type(wedding_disco no_flash)
pico_add_extra_outputs(wedding_disco)
