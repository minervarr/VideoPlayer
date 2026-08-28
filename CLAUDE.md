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
video and FLAC are both decoded by the phone's own hardware through `AMediaCodec`,
and audio is played through `audio_engine`'s AAudio sink. See rule 5, and the
deviation note under it about why FLAC is not `audio_engine`'s libFLAC.

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
    video_layer.{hh,cc}       the decoder->renderer handoff: one frame, released once
    gui_main.cc               portable entry point (no desktop Host yet)

  platform/android/        the only place AMediaCodec/AHardwareBuffer/JNI appear
    codec/mediacodec_video.{hh,cc}   HEVC Main10 → AHardwareBuffer, implements Sink
    codec/hdr_metadata.{hh,cc}       ColourInfo → AMediaFormat AND → the shader
    audio/flac_output.{hh,cc}        AMediaCodec FLAC -> audio_engine's AAudioSink

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
`vk_canvas` or `audio_engine`, it is committed *there*, in that repo. Twice so far:
`composite_frag.slang` gained a PQ transfer, and `Renderer` gained
`set_external_colour()`. This project ships **no shaders of its own** — it nearly
had a `video_frag.slang`, which was the wrong shape, because in this engine the
Y'CbCr matrix belongs to the sampler and not to shader code.

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
| PQ encode into the swapchain | `OutputEncode::PQ` + `shaders_src/output_encode.slang`, a specialization constant. The app never encodes PQ itself. |
| PQ *decode* of the frame | `composite_frag.slang`'s TRANSFER_PQ path, selected by `Renderer::set_external_transfer()`. |
| Zero-copy frame import + Y'CbCr matrix | `Renderer::update_camera_frame()` and its `SamplerYcbcrConversion` path. Written for the camera; a decoded video frame is the same kind of object. Tell it the colour first with `set_external_colour()` — the driver's suggestion is BT.709 regardless of the truth. |
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

It plays. Verified on a Galaxy S23 Ultra (SM-S918B, Android 16, arm64-v8a) on a
2040x1530 HEVC Main10 HDR10 file with FLAC audio, ~227 Mbps all-intra:

```
Swapchain: target=Hdr10PQ encode=PQ fmt=64 colorspace=1000104008 hdr=1
video: 2040x1530 HDR10 (PQ, BT.2020) rotation=0
display rate: asked for 30.000 fps, rc=0
preview-IN: 30-31 fps, worst gap 40-44 ms
```

`hdr=1` is the proof the HDR10 swapchain resolved rather than falling back, and
the Y'CbCr model reported on import is BT.2020 from the container rather than
the driver's BT.709 suggestion. `core/`'s two tests pass on the desktop.

### Nothing here is tuned for one frame rate

Every number that has to match the content is measured from the stream, not
assumed, because a constant that works at 30 fps is wrong at 60 and 120:

| Quantity | Where it comes from |
|---|---|
| Feed lead | 8 FRAMES, not milliseconds. A duration is a different number of frames at every rate. |
| Frame period, for the lead | the SMALLEST positive gap between video timestamps — conservative, since a missing packet only makes a gap larger. |
| Frame period, for everything else | the MEAN gap over the stream so far. The minimum is the wrong statistic here: a 30 fps recording contains the odd 25 ms gap, and the display was duly asked for 40 fps. |
| Drop threshold | half the measured period. Fixed at 20 ms it is half a frame at 24 fps and two and a half frames at 120. |
| Display refresh | `ANativeWindow_setFrameRate(fps, FIXED_SOURCE)`, resolved with `dlsym` because it is API 30 and the minimum is 28. |

Two things were tried and measured WORSE, and the measurements are in the code
next to what replaced them: gating the feed on queue depth (28 fps / 80 ms
jitter, because the feed holds one packet, so a full video queue stalls audio
and audio is the clock), and dropping the OLDEST frame from a full queue (the
oldest is the one about to come due).

### Two things that were wrong for real

**A video with no audio was a still image.** `run()`'s comment said the clock
free-ran without an audio track and nothing ever called `advanceFreerun()`. The
timeline sat at zero, so the first frame presented and every later one waited
forever. It free-runs off `steady_clock` now, capped at four frames per step so
a backgrounded app does not jump the timeline past everything in flight, and it
does not start until the first frame exists.

**Rotation was the camera's, not the file's.** `composite_vert.slang` turned
every external image a quarter turn and the letterbox swapped width and height
to match — correct for the camera preview the path was written for, wrong for a
video whose container says 0. `Colour` was already read from the container
(rule 3); `Projection > ProjectionPoseRoll` now is too.

### Not done

No UI at all. No seek bar, no controls, no on-screen state — tap or Space
toggles pause and that is the entire interface. `Player::seek()` works and
nothing calls it.

The tone map reads the CONTENT's mastering peak as a stand-in for the DISPLAY's,
because nothing asks Android what the panel can do. Correct whenever the two
agree, conservative when they do not.

The render loop runs free at ~900 fps redrawing identical content, because the
swapchain is in mailbox mode. Harmless to correctness and a waste of power; on a
long file the heat it makes is the decoder's problem too.

Anamorphic content is not handled: `DisplayWidth`/`DisplayHeight` are parsed and
unused, so a file whose pixels are not square is shown at its pixel aspect.

The `content://` launch path has never been exercised by a real file manager.
