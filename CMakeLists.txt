cmake_minimum_required(VERSION 3.22.1)
project(video_player CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# What this file is FOR.
#
# Android is the only host that builds a running player today, and it does not
# come through here — android/CMakeLists.txt is its entry point, reached from
# Gradle. This file exists so that core/ — the Matroska reader, the clock, the
# state machine — can be built and TESTED on a desktop, without a phone, an
# NDK, or a Vulkan device.
#
# That is not a stopgap. A container regression must be reproducible in one
# second on the machine you are typing on, and every rule in CLAUDE.md that
# keeps core/ free of OS headers exists to make this build possible. When a
# desktop HOST arrives later, it adds platform/linux/ and a gui/ target here;
# nothing about core/ changes.
# ---------------------------------------------------------------------------

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/framework/vk_canvas/core/CMakeLists.txt)
    message(FATAL_ERROR
        "framework/vk_canvas is empty — the first-party engines are submodules.\n"
        "Run:  git submodule update --init --recursive")
endif()

if(NOT WIN32)
    add_compile_options(-Wall)
endif()

add_subdirectory(core)

# Build the tests with:
#   cmake -B build/linux_debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/linux_debug
#   ctest --test-dir build/linux_debug --output-on-failure
enable_testing()
