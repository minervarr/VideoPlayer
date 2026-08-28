# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this project is

A native C++17 video player for Android that plays **one thing, correctly**:
**HEVC Main10 in Matroska, HDR10 — ST 2084 PQ transfer, BT.2020 primaries — with
FLAC audio**. It renders through a first-party Vulkan engine (`vk_canvas`), decodes
audio through a first-party audio engine (`audio_engine`), and reaches the OS only
through a first-party application shell (`app_shell`). All three are git submodules
authored by "minervarr" and developed independently — read their own `CLAUDE.md`
files before touching anything inside them.

**There is no FFmpeg in this project.** The container is parsed here (`core/src/mkv.cpp`),
video is decoded by the phone's own HEVC hardware through `AMediaCodec`, and FLAC is
decoded by `audio_engine`'s vendored libFLAC. See rule 5.

Android is the only host that builds a running player today. The repo root's
`CMakeLists.txt` builds `core/` and its tests **on a desktop**, without an NDK or a
GPU — that is what makes a container regression reproducible in one second.

---

## Repository layout

```
VideoPlayer/
  core/                    pure C++17: Matroska, timing, state. Zero OS headers.
    include/core/mkv.h        container vocabulary — ColourInfo is the point
    include/core/demux.h      Demuxer: open, tracks, nextPacket, seek
    include/core/video_frame.h  the decode↔render seam; an opaque handle
    include/core/clock.h      audio-master presentation clock, integer µs
    include/core/player.h     the state machine + the Sink interface
    src/ebml.{h,cpp}          EBML primitives, src-private
    src/mkv_parser.h          the seam between headers and clusters
    src/mkv.cpp               Info/Tracks/Colour/Cues, read once at open
    src/demux.cpp             Clusters, Blocks, lacing, seek
    tests/mkv_test.cc         builds its own .mkv fixture and asserts on it
    tests/clock_test.cc       A/V sync arithmetic, exactly

  gui/src/                 the app. No OS headers; reaches the OS via Host.
    player_view.{hh,cc}       layout, controls, transport
    video_layer.{hh,cc}       the ONLY file in gui/ that knows a frame is a GPU image
    gui_main.cc               portable entry point (no desktop Host yet)

  platform/android/        the only place AMediaCodec/AHardwareBuffer/JNI appear
    codec/mediacodec_video.{hh,cc}   HEVC Main10 → AHardwareBuffer, implements Sink
    codec/hdr_metadata.{hh,cc}       ColourInfo → AMediaFormat AND → the shader
    audio/flac_output.{hh,cc}        adapter onto audio_engine's FLAC + AAudio

  shaders_src/video_frag.slang   Y'CbCr → BT.2020 → PQ → the engine's OUTPUT_ENCODE
  android/                 the Gradle project; android/CMakeLists.txt is the NDK entry
  framework/               vk_canvas, audio_engine, app_shell (submodules)
  reference/               mpv + mpv-android, on disk, gitignored, never built
```

---

## The five rules

**1. `core/` includes no OS header, no Vulkan header, no Canvas header.**
It parses, schedules, and decides. It never decodes and never draws. Its tests link
one set of real sources each and nothing more. If `video_core` ever needs an OS
library on its link line, something leaked in.

**2. `platform/android/` is the only place `AMediaCodec`, `AHardwareBuffer`, or JNI
appear.** A second Host later adds a sibling directory; nothing in `core/` or
`gui/src/player_view.cc` changes. `core/include/core/video_frame.h` is where this is
most easily broken — `handle` is a `void*` on purpose.

**3. Colour comes from the container.** `mkv.cpp` reads `Colour` and stores its
enumerations *unvalidated*; nothing sniffs, guesses, or defaults. If the file does not
say PQ **and** BT.2020, `ColourInfo::isHdr10()` is false and the SDR path draws it.
The one default in the whole colour path is `ShaderColour::masteringPeakNits`, and it
is a tone-mapping parameter rather than a claim about the file.

**4. Engine work goes in the engine.** If the video path needs something from
`vk_canvas` or `audio_engine`, it is committed *there*, in that repo, with its own
test. The known example: `import_ahb()` today refuses Y'CbCr and external-format
buffers, and a 10-bit P010 video frame is exactly that.

**5. No FFmpeg.** Not as a submodule, not as a prebuilt, not "temporarily".

### One deviation, on purpose

FLAC **decodes through `AMediaCodec`**, not through `audio_engine`'s vendored
libFLAC — which is the opposite of what a music player built on this engine does.
`audio_engine`'s decoder is `open(fd, offset, length)`: it decodes a FLAC *file*,
read through libFLAC's stream callbacks over a byte region. Matroska stores raw
FLAC *frames*, with STREAMINFO held separately in CodecPrivate — there is no
contiguous region to point an fd at, and synthesising one would mean re-muxing a
FLAC stream in memory to hand it back to a decoder. `AMediaCodec`'s input model
is exactly the container's: `csd-0` is STREAMINFO, one input buffer is one frame.

Audio **output** still goes through `audio_engine`'s `AAudioSink`, because that
is the half carrying what the video path actually needs. Revisit if
`audio_engine` ever grows a packet-fed FLAC entry point.

---

## Where the engines already do the work

Do not rebuild any of this:

| Need | Where it already is |
|---|---|
| HDR10 PQ swapchain | `vk_canvas` `core/output_target.hh` + `Renderer::resolve_output_target()` (`core/renderer.cc:958`). Construct `Renderer` with `OutputTarget::Hdr10PQ`; it enumerates, prefers `A2B10G10R10_UNORM_PACK32 + HDR10_ST2084`, falls back to the SDR pin, and reports through `hdrActive()`. |
| PQ encode | `OutputEncode::PQ` + `shaders_src/output_encode.slang`, a specialization constant. Never encode PQ in `video_frag.slang` directly. |
| Zero-copy frame import | `ComputeContext::import_ahb()` (`core/compute_context.hh:67`) and the renderer's `SamplerYcbcrConversion` path (`renderer.hh:211`), both written for the camera. |
| Android Host, entry point, safe insets, storage permission | `app_shell`: `host.hh`, `app_view.hh`, `os/android_host.cc`, `os/storage_permission.cc`. |
| AAudio output + `pendingPlaybackMs()` | `audio_engine` `backends/aaudio/`. The last is the audio clock: what the speaker is playing *now*, not what was last written. |

---

## Building

```bash
git submodule update --init --recursive

# Desktop: core/ and its tests. No NDK, no GPU.
cmake -B build/linux_debug -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build/linux_debug
ctest --test-dir build/linux_debug --output-on-failure

# Android: the app.
cd android && ./gradlew assembleDebug
```

`adb logcat -s VideoMain:V AndroidHost:V video_player:V` is the startup trace.
The line `resolve_output_target()` emits, naming the resolved format and colourspace,
is the HDR proof — **`fellBack == true` is a hard failure for this project**, not a
degraded-but-acceptable state.

---

## Current state

The whole path is written and the APK builds clean. **It has never run on
hardware** — everything below "builds" is unverified.

Verified: `core/`'s two tests pass on the desktop, and the arm64-v8a APK packages
the native library, the compiled shaders and `AppShellActivity`.

Not verified: that a frame reaches the screen, that the colours are right, that
A/V stays in sync, that HDR10 is actually resolved rather than falling back. The
first run is a debugging session, not a demo — `scripts/android/build.sh <device
path>` installs, grants storage, launches on a file and opens the logcat with
every tag that would explain a failure.

Not built at all yet: any UI. No seek bar, no controls, no on-screen state — tap
or Space toggles pause and that is the entire interface. `Player::seek()` works
and nothing calls it.

The one engine-side thing left undone: the tone map reads the CONTENT's mastering
peak as a stand-in for the DISPLAY's, because nothing asks Android what the panel
can do. Correct whenever the two agree, conservative when they do not.
