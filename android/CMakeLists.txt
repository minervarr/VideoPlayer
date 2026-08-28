cmake_minimum_required(VERSION 3.22.1)
project(video_player_android)

# Reached from app/build.gradle's externalNativeBuild.cmake.path. This is a
# SEPARATE top-level project from the repo root's CMakeLists.txt, which builds
# only core/ and its desktop tests — see the note at the top of that file.
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# ---------------------------------------------------------------------------
# The three first-party submodules.
# ---------------------------------------------------------------------------

# audio_engine — for FLAC. Linked as a C++ target directly; this app's native
# code is C++ throughout, so there is no JNI language boundary to cross.
add_subdirectory(${REPO_ROOT}/framework/audio_engine ${CMAKE_BINARY_DIR}/audio_engine)

# vk_canvas — its own CMakeLists.txt already has an if(ANDROID) branch linking
# vulkan/android/log (the NDK provides libvulkan), so linking the target is
# enough.
add_subdirectory(${REPO_ROOT}/framework/vk_canvas/core ${CMAKE_BINARY_DIR}/vk_canvas_core)

# app_shell — the Host seam, the Android Host, and the JNI plumbing (safe area,
# storage permission, launch intent). The name below is the logcat tag.
set(APP_SHELL_APP_NAME "video_player" CACHE STRING "" FORCE)
set(APP_SHELL_NATIVE_APP_GLUE_DIR ${NATIVE_APP_GLUE} CACHE PATH "" FORCE)
add_subdirectory(${REPO_ROOT}/framework/app_shell ${CMAKE_BINARY_DIR}/app_shell)

# video_core — the Matroska reader, the clock, the state machine. Unmodified,
# and with no Android-specific branch anywhere in it. That is the point.
add_subdirectory(${REPO_ROOT}/core ${CMAKE_BINARY_DIR}/video_core)

# ---------------------------------------------------------------------------
# Shaders -> app/src/main/assets/shaders/, so AGP packages them into the APK.
# Three lists: the font engine's, vk_canvas's own, and this project's one
# video shader.
# ---------------------------------------------------------------------------
set(VP_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/assets/shaders)
include(${REPO_ROOT}/framework/vk_canvas/cmake/VceShaders.cmake)

vce_compile_slang(compile_font_shaders ${VP_SHADER_DIR}
    ${REPO_ROOT}/framework/vk_canvas/first_party/vulkan_font_engine/shaders_src
    composite_vert composite_frag tiling coverage
    msdf_vert msdf_frag glyph_area glyph_resolve)

vce_compile_slang(compile_vk_canvas_shaders ${VP_SHADER_DIR}
    ${REPO_ROOT}/framework/vk_canvas/shaders_src
    overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

# Ours. It lives in this repo, not in the engine, because BT.2020 + PQ is this
# application's colour path — see the header comment in video_frag.slang.
vce_compile_slang(compile_video_shaders ${VP_SHADER_DIR}
    ${REPO_ROOT}/shaders_src
    video_frag)

# ---------------------------------------------------------------------------
# The app.
# ---------------------------------------------------------------------------
set(VK_CANVAS_ANDROID_PLATFORM ${REPO_ROOT}/framework/vk_canvas/platform/android)

add_library(video_player_android SHARED
    src/main.cc
    ${REPO_ROOT}/gui/src/player_view.cc
    ${REPO_ROOT}/gui/src/video_layer.cc
    ${REPO_ROOT}/platform/android/codec/mediacodec_video.cc
    ${REPO_ROOT}/platform/android/codec/hdr_metadata.cc
    ${REPO_ROOT}/platform/android/audio/flac_output.cc
    ${VK_CANVAS_ANDROID_PLATFORM}/android_platform.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c)

target_include_directories(video_player_android PRIVATE
    ${NATIVE_APP_GLUE}
    ${VK_CANVAS_ANDROID_PLATFORM}
    ${REPO_ROOT}/gui/src
    ${REPO_ROOT}/platform/android)

target_link_libraries(video_player_android
    app_shell
    app_shell_android
    video_core
    vk_canvas_core
    # FLAC through audio_engine's own vendored libFLAC on every platform, not
    # through AMediaCodec — see platform/android/audio/flac_output.hh.
    ae_core ae_flac ae_aaudio
    # The NDK media library: AMediaCodec + AImageReader, the video decode path.
    mediandk
    android log)

add_dependencies(video_player_android
    compile_font_shaders compile_vk_canvas_shaders compile_video_shaders)

# Keep the glue's NativeActivity entry point alive against linker GC (the same
# rule framework/vk_canvas/platform/android/CMakeLists.txt documents).
target_link_options(video_player_android PRIVATE -u ANativeActivity_onCreate)
