# wedding_disco_lvgl — the LVGL variant. Same lighting engine as
# wedding_disco (patterns/spectrum/strip are compiled from that directory, not
# copied) with an LVGL widget UI in place of the hand-drawn ST7796 screen.
set(DISCO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../wedding_disco)

add_executable(wedding_disco_lvgl
    main.c
    lvgl_port.c
    ui.c
    ${DISCO_SRC}/patterns.c
    ${DISCO_SRC}/spectrum.c
    ${DISCO_SRC}/strip.c
)
# codes.h plus the shared engine headers live with the original app.
target_include_directories(wedding_disco_lvgl PRIVATE ${DISCO_SRC})
target_link_libraries(wedding_disco_lvgl
    freewili2_bsp beat onewili_fwgui lvgl pico_multicore)

# strip.pio is compiled from the shared strip.c, so its header must be
# generated for this target too.
pico_generate_pio_header(wedding_disco_lvgl ${DISCO_SRC}/strip.pio)

# The default 2 KB stack is tight for LVGL's recursive draw/layout code.
# crt0.S is an INTERFACE source compiled per target, so this only affects this
# executable. The core0 stack sits in a 4 KB SCRATCH_Y bank, so 0x1000 is the
# ceiling without a custom linker script.
target_compile_definitions(wedding_disco_lvgl PRIVATE PICO_STACK_SIZE=0x1000)

# RAM-ONLY — see the note in ../wedding_disco/CMakeLists.txt. This matters more
# here: at ~465 KB the LVGL image is most of the 512 KB SRAM, and a no_flash
# build has no flash copy to fall back on, so a link-time overflow is the only
# warning you get.
pico_set_binary_type(wedding_disco_lvgl no_flash)
pico_add_extra_outputs(wedding_disco_lvgl)
