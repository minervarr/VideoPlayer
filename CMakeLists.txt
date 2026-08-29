cmake_minimum_required(VERSION 3.22.1)
# C as well as C++: the desktop player pulls in wayland protocol glue and
# audio_engine's vendored libraries, which are C. core/ alone needs only CXX,
# and enabling C costs a compiler check it would otherwise skip — worth it to
# have one project() rather than two.
project(video_player C CXX)

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

# ── The desktop player ─────────────────────────────────────────────────────
#
# Off unless the pieces are actually present, so the one-second `ctest` of
# core/ on any machine stays a one-second ctest. app_shell has had a complete
# Wayland Host all along; what was missing was a gui/ free of Android headers,
# which is now true, so this builds a real binary.
#
# It has no decoder yet — see gui/CMakeLists.txt. Building it anyway is the
# point: a portability seam nothing compiles against stops being one.
find_package(PkgConfig QUIET)
set(_vp_desktop_ok OFF)
if(PkgConfig_FOUND AND NOT ANDROID AND NOT WIN32)
    pkg_check_modules(VP_WAYLAND QUIET wayland-client)
    pkg_check_modules(VP_VULKAN   QUIET vulkan)
    if(VP_WAYLAND_FOUND AND VP_VULKAN_FOUND)
        set(_vp_desktop_ok ON)
    endif()
endif()

# VceShaders.cmake defaults VCE_SLANGC to a Windows Vulkan SDK path, and
# android/app/build.gradle passes the right one explicitly. A desktop build had
# nowhere to get it and failed with exit 127 — "command not found" from a
# custom command, which names nothing. Look where the Linux package puts it.
if(_vp_desktop_ok AND NOT VCE_SLANGC)
    find_program(_vp_slangc NAMES slangc
                 HINTS /opt/shader-slang/bin $ENV{VULKAN_SDK}/bin)
    if(_vp_slangc)
        set(VCE_SLANGC ${_vp_slangc} CACHE FILEPATH "Path to slangc" FORCE)
    else()
        message(STATUS "VideoPlayer: desktop player off (no slangc; "
                       "pass -DVCE_SLANGC=/path/to/slangc)")
        set(_vp_desktop_ok OFF)
    endif()
endif()

# OFF by default even when the pieces are present.
#
# The desktop player is PARKED, not abandoned: it links and opens a window, and
# it has no decoder, so it plays nothing (see gui/CMakeLists.txt and TODO.md).
# Building it by default would turn the root build — whose whole job is to make
# a container regression reproducible in one second, with no NDK and no GPU —
# into a full engine build including msdfgen, libusb and libFLAC. Turn it on
# deliberately:
#
#   cmake -B build/desktop -DVP_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
option(VP_BUILD_DESKTOP "Build the desktop (Wayland) player" OFF)
if(VP_BUILD_DESKTOP AND NOT _vp_desktop_ok)
    message(FATAL_ERROR
        "VP_BUILD_DESKTOP is ON but the desktop player's prerequisites are "
        "missing (wayland-client, vulkan, slangc).")
endif()
if(VP_BUILD_DESKTOP)
    message(STATUS "VideoPlayer: building the desktop player")
    add_subdirectory(gui)
else()
    message(STATUS "VideoPlayer: desktop player off (-DVP_BUILD_DESKTOP=ON to build it)")
endif()

# Build the tests with:
#   cmake -B build/linux_debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/linux_debug
#   ctest --test-dir build/linux_debug --output-on-failure
enable_testing()
