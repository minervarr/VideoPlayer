# The desktop player.
#
# Everything here is the SAME source Android builds — player_view.cc,
# video_layer.cc and core/ — plus a different Host and a different entry point.
# That is the whole point of the seam: gui/ names no OS type, so a second host
# is a link line rather than a port.
#
# What is not here yet is a decoder. platform/android/codec/ has no sibling,
# so openFile() takes its "no decoder on this platform" branch and the player
# opens a window, parses the container and plays nothing. That is an honest
# intermediate state and a compiling one, which is what keeps the seam from
# rotting while the Vulkan Video path is written.

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(APP_SHELL_APP_NAME "video_player" CACHE STRING "" FORCE)
set(APP_SHELL_EXTRA_INCLUDE ${REPO_ROOT}/android/generated)
add_subdirectory(${REPO_ROOT}/framework/audio_engine ${CMAKE_BINARY_DIR}/audio_engine)
add_subdirectory(${REPO_ROOT}/framework/vk_canvas/core ${CMAKE_BINARY_DIR}/vk_canvas_core)
# The Wayland window, display and surface provider the Host drives.
add_subdirectory(${REPO_ROOT}/framework/vk_canvas/platform/linux
                 ${CMAKE_BINARY_DIR}/vk_canvas_wayland)
add_subdirectory(${REPO_ROOT}/framework/app_shell ${CMAKE_BINARY_DIR}/app_shell)
target_include_directories(app_shell PUBLIC ${REPO_ROOT}/android/generated)

# app_shell's Debug tests do add_dependencies(ui_metrics_test
# generate_ui_min_text_size), and that target normally comes from
# app_shell_generate_min_text_size() over the faces an app ships. This player
# draws no text yet and supplies the header itself as a documented stand-in
# (android/generated/ui_min_text_size.gen.h, included via the line above), so
# there is nothing to measure and the target is a no-op that exists only to
# satisfy that dependency. When the player draws its first string, delete the
# stand-in and replace this with the real call over the faces it loads —
# assets/fonts is the shared `fonts` submodule and already has them.
# ui_metrics_test does not inherit app_shell's include directories — it compiles
# ui_metrics.cc itself and looks for the header in ${CMAKE_BINARY_DIR}/generated
# — so the stand-in has to be staged there too, not just put on app_shell's
# include path. Same shape ImagesLogosCreator uses for its static floor: the
# "generation" is a copy.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ANDROID)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
    configure_file(${REPO_ROOT}/android/generated/ui_min_text_size.gen.h
                   ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h COPYONLY)
    add_custom_target(generate_ui_min_text_size
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${REPO_ROOT}/android/generated/ui_min_text_size.gen.h
            ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h)
endif()

# Shaders -> beside the binary, where the desktop AssetReader looks.
set(VP_SHADER_DIR ${CMAKE_BINARY_DIR}/assets/shaders)
include(${REPO_ROOT}/framework/vk_canvas/cmake/VceShaders.cmake)
vce_compile_slang(compile_font_shaders_desktop ${VP_SHADER_DIR}
    ${REPO_ROOT}/framework/vk_canvas/first_party/vulkan_font_engine/shaders_src
    composite_vert composite_frag tiling coverage
    msdf_vert msdf_frag glyph_area glyph_resolve)
vce_compile_slang(compile_vk_canvas_shaders_desktop ${VP_SHADER_DIR}
    ${REPO_ROOT}/framework/vk_canvas/shaders_src
    overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

add_executable(video_player
    src/gui_main.cc
    src/player_view.cc
    src/video_layer.cc)

target_include_directories(video_player PRIVATE
    ${REPO_ROOT}/gui/src
    ${REPO_ROOT}/framework/audio_engine)

target_link_libraries(video_player PRIVATE
    app_shell
    app_shell_wayland
    vk_canvas_wayland
    video_core
    vk_canvas_core
    ae_core ae_flac ae_alsa)

add_dependencies(video_player
    compile_font_shaders_desktop compile_vk_canvas_shaders_desktop)
