# beat — pure integer beat detector (no Pico SDK deps). Apps link it; the host
# test tree (tests/test_beat.c) compiles beat.c directly for `fw test`.
add_library(beat STATIC beat.c)
target_include_directories(beat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
