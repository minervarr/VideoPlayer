# ---------------------------------------------------------------------------
# video_core — the portable half of the player: Matroska, timing, state.
#
# Zero OS headers, zero Vulkan, zero Canvas. It parses, schedules, and decides;
# it never decodes and never draws. See CLAUDE.md rule 1 — if this target ever
# needs an OS library on its link line, something leaked in.
# ---------------------------------------------------------------------------

add_library(video_core STATIC
    src/ebml.cpp
    src/mkv.cpp
    src/demux.cpp
    src/clock.cpp
    src/audio_clock.cpp
    src/frame_period.cpp
    src/shader_colour.cpp
    src/player.cpp)

target_include_directories(video_core PUBLIC include)

if(MSVC)
    target_compile_options(video_core PRIVATE /W3 /utf-8)
else()
    target_compile_options(video_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tests: assert-based, Debug-only, no framework — the house convention. Each
# links the REAL sources it exercises and NOTHING else, which is the property
# worth protecting more than the assertions themselves.
#
#   ./build/linux_debug/core/mkv_test
#   ./build/linux_debug/core/clock_test
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    enable_testing()

    # The container, over a file the test writes itself. No player, no clock.
    add_executable(mkv_test tests/mkv_test.cc src/ebml.cpp src/mkv.cpp src/demux.cpp)
    target_include_directories(mkv_test PRIVATE include src)

    # A/V sync arithmetic. Links clock.cpp ALONE — no demuxer, no file I/O.
    add_executable(clock_test tests/clock_test.cc src/clock.cpp src/audio_clock.cpp)
    target_include_directories(clock_test PRIVATE include)

    # The frame-rate estimator, over timestamps that arrive out of order. One
    # source, like the others: it is arithmetic and must stay that way.
    add_executable(frame_period_test tests/frame_period_test.cc src/frame_period.cpp)
    target_include_directories(frame_period_test PRIVATE include)

    # mkvdump — point it at a REAL file and see what the parser makes of it.
    # Debug-only like the tests, and for the same reason: it is a development
    # instrument, not part of the player.
    add_executable(mkvdump ${CMAKE_CURRENT_SOURCE_DIR}/../tools/mkvdump.cpp)
    target_link_libraries(mkvdump PRIVATE video_core)
    if(NOT MSVC)
        target_compile_options(mkvdump PRIVATE -Wall)
    endif()

    foreach(_t mkv_test clock_test frame_period_test)
        if(MSVC)
            target_compile_options(${_t} PRIVATE /W3 /utf-8)
        else()
            target_compile_options(${_t} PRIVATE -Wall -Wextra)
        endif()
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
endif()
