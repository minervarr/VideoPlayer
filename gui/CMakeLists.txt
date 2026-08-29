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
