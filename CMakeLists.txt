cmake_minimum_required(VERSION 3.13)

# RP2350B board header (PICO_RP2350A=0 -> 48 GPIO, 16MB flash). Comes from the
# vendored wilibsp BSP. Do NOT pass -DPICO_BOARD on the command line.
set(PICO_BOARD freewili2 CACHE STRING "Board type")
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/wilibsp/bsp/boards")

include(pico_sdk_import.cmake)
project(wedding C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Vendored dependencies: the Free Wili 2 board-support library, the beat
# detector, and the OneWili C API (main-CPU bridge over the FwGUI link).
add_subdirectory(third_party/wilibsp/bsp)
add_subdirectory(third_party/wilibsp/libs/beat)
add_subdirectory(third_party/wilibsp/libs/onewili)

# LVGL (v9.3.0, vendored trimmed: src/ + cmake glue only, no demos/examples/
# tests/docs) for the wedding_disco_lvgl variant. Its own demo/example/ThorVG
# targets are disabled — we only need the widget core.
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/src/wedding_disco_lvgl/lv_conf.h"
    CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/lvgl)

# The apps.
add_subdirectory(src)
