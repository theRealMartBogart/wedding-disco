# Host-side logic tests — no Pico SDK, no hardware. Run with:
#   python tools/fw.py test
# The DSP in src/wedding_disco/spectrum.c is deliberately free of any SDK or
# hardware dependency so it can be verified here rather than on the dance floor.
cmake_minimum_required(VERSION 3.13)
project(wedding_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()

add_executable(test_spectrum
    test_spectrum.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/wedding_disco/spectrum.c)
target_include_directories(test_spectrum PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/wedding_disco)
if(NOT MSVC)
    target_compile_options(test_spectrum PRIVATE -Wall -Wextra)
    target_link_libraries(test_spectrum m)
endif()
add_test(NAME spectrum COMMAND test_spectrum)
