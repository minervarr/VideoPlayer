# Install script for directory: /home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/opt/android-sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/nava/Files/code/active/VideoPlayer/android/app/.cxx/Release/195n3fz4/arm64-v8a/third_party/flac/src/cmake_install.cmake")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/FLAC" TYPE FILE FILES
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/all.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/assert.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/callback.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/export.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/format.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/metadata.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/ordinals.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/stream_decoder.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC/stream_encoder.h"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/FLAC++" TYPE FILE FILES
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC++/all.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC++/decoder.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC++/encoder.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC++/export.h"
    "/home/nava/Files/code/active/VideoPlayer/framework/audio_engine/third_party/flac/include/FLAC++/metadata.h"
    )
endif()

