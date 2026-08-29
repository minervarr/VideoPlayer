# TODO

Written 2026-08-28 and extended 2026-08-29 after a second pass. What is *done*
is in the git log and in CLAUDE.md's "Current state"; this file is only what is
not.

Ordered by what the project is for: Android has to be perfect first, and Linux
is parked deliberately rather than half-built.

---

## Android — verified on the device

Run on the Galaxy S23 Ultra (SM-S918B, arm64-v8a) on 2026-08-29, against a
2040x1530 HEVC Main10 HDR10 / FLAC recording, ~227 Mbps all-intra. Everything
below was measured, not reasoned about.

```
Swapchain: target=Hdr10PQ encode=PQ fmt=64 colorspace=1000104008 hdr=1
Swapchain present mode=2 (vsync-paced), images=4
video: 2040x1530 HDR10 (PQ, BT.2020) rotation=0 par=1.0000 | display peak 450 nits
prebuffer: 6 frames + audio after 27 ms
display rate: asked for 30.013 fps, rc=0
present: 31 shown, avg 33334 us, WORST 36154 us | dropped 0 | depth 6 | refused 0
```

Full-file totals: **339 frames shown, 0 dropped, 0 refused**, worst gap 42 ms
across the entire run. A screenshot confirms the picture is upright, correctly
letterboxed, natural in colour, and free of the focus-peaking speckle the
broken push block produced.

| Change | Verified by |
|---|---|
| Shaders restored | `hdr=1`, `fellBack == false`, picture upright, no peaking artefacts |
| Audio clock interpolated | avg gap 33334 us against a 33333 us content period |
| Prebuffer | `prebuffer: 6 frames + audio after 27 ms` |
| FIFO present | `present mode=2 (vsync-paced)`, render 30 fps rather than ~900 |
| Display peak | `display peak 450 nits` — the panel answered, and the tone map now targets it |
| Pixel aspect | `par=1.0000` on a square-pixel file |

Two bugs were found BY this testing and fixed, both in what the FIFO change had
just introduced — see the git log for `Schedule frames against when they will
be SHOWN`. The measurement that mattered:

```
60 Hz panel:  30 shown, avg 33373 us, WORST 35653 us, dropped 0
30 Hz panel:  21 shown, avg 47758 us, WORST 67937 us, dropped 12-19
```

Same build, same file, seconds apart, with the queue seven frames deep
throughout. The panel had just been pinned to 30 Hz by this player's own
setFrameRate call, and every dropped frame was late by 31-36 us — one frame
period exactly.

### Verified in the second pass (2026-08-29)

A later run against the same clip plus a 3.9 GB, ~5 minute recording from the
same camera:

```
orientation: displayed 2040x1530 (1.333:1) -> sensor landscape
tone map: 450 nits (content 1000, display 450) — the panel is the limit
prebuffer: 10 frames + audio after 67 ms
soak: 29 s played | 891 shown, 4 dropped, 0 refused | worst gap 72760 us | drift -110 ms
end of stream: 19453 ms played, holding the last frame
```

| Change | Verified by |
|---|---|
| Pause really pauses | picture stops AND `dumpsys audio` shows `state:paused`; resume continues in sync |
| Backgrounding pauses | HOME logs `paused: the window is no longer in front`; no sound away; no auto-resume |
| End of stream | last frame reached, `Ended`, holds the picture; tap replays |
| Orientation from content | a 4:3 file rotates a portrait-LOCKED phone to landscape; 35% of the panel becomes 62%, nothing cropped |
| Prebuffer | 6-10 frames in 53-84 ms, from a horizon anchored to the content's own start |
| Long run | 938 shown / 4 dropped over 30 s, against 972 / 5 for the same file before the change |

### Still worth watching

- **A route change has never actually happened.** The disconnect detection,
  the rebuild and the stall watchdog are written and none has fired. Play
  something, plug in headphones, and look for `audio device disconnected`
  followed by `audio device rebuilt`. Until that is seen this is code that
  compiles, not a feature that works.
- **A REAL soak.** Thirty seconds is not an hour. Drift moved from -105 ms to
  -117 ms across one 30-second interval — a constant offset plus something
  small. Whether the something small accumulates is exactly what a long run
  answers and a short one cannot.
- **B-frames.** The estimator is correct by construction now and
  `core/tests/frame_period_test.cc` asserts it against reordered sequences
  written down by hand. No actual B-frame FILE has been played; everything
  from this camera is all-intra.
- **60 and 120 fps content.** Still untried. Everything scales in frames and
  the prebuffer is measured from the content's start, so it should follow —
  "should" is still not "did".
- **Files that state ChromaSiting or MaxCLL.** Both are parsed and wired, and
  the one file available states neither, so both new paths have only ever run
  in their fallback.
- **Cold storage costs frames.** The first play of the 3.9 GB file dropped 198
  frames in 29 s; the second dropped 4. The queue never emptied (`refused 0`,
  depth 4-6 throughout), so that is read latency reaching the feed, not a
  scheduling fault. Worth knowing before reading a first-run measurement as a
  regression — it looked exactly like one, and was chased as one.
- **`AChoreographer` is still not needed.** FIFO plus the rate pin plus the
  presentation lead gives zero drops in steady state. Do not add a second pacer.

## Android — not done

- **No UI at all.** Tap or Space toggles pause, and that is the whole
  interface. `Player::seek()` works, is correct on both sides of the A/V seam,
  and nothing calls it. A seek bar is the obvious next feature and has been out
  of scope for two passes running.
- **No zoom or fill mode, deliberately.** The picture is never cropped to fill
  the panel: a 4:3 file in a 2.17:1 window would lose 38% of the frame, and the
  bars are the honest rendering of that. Orientation was the part that could be
  recovered without cost, and it has been.
- **A mid-stream resolution change is reported, not handled.** `emit()` says so
  once and keeps going; the `AImageReader` is fixed at its first size. If that
  line ever appears in a real log, that is the moment to build the rebuild.
  Note the check compares the CROP, not the format's width and height — those
  are the aligned buffer (a 2040x1530 stream reports 2048x1536) and comparing
  them fires on every ordinary file.
- **Minification quality.** `minFilter` is `VK_FILTER_LINEAR` with no mip
  chain, which aliases beyond about 2x. Invisible at the ~0.94 scale of the
  only content available. Needs a clip that actually minifies — 4K in a smaller
  window — before any shader work is spent on it.
- **The `content://` launch path has never been exercised by a real file
  manager.** The code is there and the fd-to-stream path is written; nobody has
  tapped a video in Files and picked this app.

---

## Linux — parked, on purpose

The seam is finished and the skeleton compiles and runs. **It has no decoder,
so it plays nothing.** This is a deliberate stopping point, not an abandoned
attempt: a portability seam nothing compiles against stops being one.

```bash
cmake -B build/desktop -DVP_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build/desktop && ./build/desktop/gui/video_player file.mkv
```

Off by default so the root build stays the one-second `ctest` it exists to be.

### What is already there

`framework/app_shell/os/wayland_host.cc` is a complete Host — window, input,
surface provider, asset reader, `main()`. `gui/` compiles with no Android
header in it. `core/` never had one. `ae_alsa` exists in audio_engine. The
window opens and the swapchain resolves.

### What is missing, in order

1. **`core/hevc/` — VPS/SPS/PPS and slice-header parsing.** Vulkan Video hands
   the driver parsed parameter sets; the parsing is yours. Pure arithmetic over
   bytes, so it belongs in `core/` under rule 1 and earns a
   `core/tests/hevc_test.cc` that runs in one second with no GPU, no NDK and no
   phone. Roughly 80% of the Vulkan Video effort, and **none of it needs a
   driver** — it is the part that can be built and tested on this laptop today.
2. **A video-decode queue in `vk_canvas`.** Device creation must request
   `VK_KHR_video_queue`, `VK_KHR_video_decode_queue` and
   `VK_KHR_video_decode_h265`, and find a queue family with `VIDEO_DECODE_BIT`
   — all optional, so Android and non-supporting desktops are unaffected.
   Engine work, so it lands in that repo (rule 4).
3. **`platform/linux/codec/vulkan_video.{hh,cc}`.** `VkVideoSessionKHR`, the
   DPB as a `VkImage` array, reference-picture management,
   `vkCmdDecodeVideoKHR`. Mirrors `platform/android/codec/mediacodec_video.cc`
   in shape, implements the same `Sink`, emits the same `DecodedFrame`. Simpler
   than Android on one axis: the output is already a `VkImage`, so there is no
   import step at all — `Renderer::update_external_frame()` takes the handle
   either way.
4. **`platform/linux/audio/` — a packet-fed FLAC decoder over libFLAC.**
   audio_engine's own FLAC decoder is `open(fd, offset, length)`: it decodes a
   FLAC *file*. Matroska stores raw frames with STREAMINFO in CodecPrivate, so
   Android went through `AMediaCodec` instead (the one documented deviation).
   libFLAC's `FLAC__StreamDecoder` supports exactly this push model, and
   writing that adapter would eventually let Android drop its deviation too.
   Implement `core/audio_output.h`, as `FlacOutput` does.

### The prerequisite nobody can work around

**No GPU on the development machine exposes Vulkan video decode.** `vulkaninfo`
reports zero `VK_KHR_video*` extensions and zero queues with
`VIDEO_DECODE_BIT`, on both:

- Intel TigerLake UHD, Mesa ANV 26.2.1 — including with `ANV_VIDEO_DECODE=1`
- NVIDIA RTX 3050, NVK (`vulkan-nouveau`)

The RTX 3050 **does** support `VK_KHR_video_decode_h265` under NVIDIA's
proprietary driver. Installing `nvidia`/`nvidia-utils` alongside the current
`vulkan-nouveau` is what makes steps 2–4 testable here. Step 1 is testable
today regardless, which is why it is first.

VA-API (libva 1.24 is installed and the Intel iGPU decodes HEVC Main10) was
considered and rejected in favour of Vulkan Video: no new dependency, zero-copy
by construction, and the right fit for what this project is.

---

## Engine repositories — unpushed

Every commit in `framework/vk_canvas`, `framework/vk_canvas/first_party/
vulkan_font_engine` and `framework/audio_engine` is **local**. Nothing has been
pushed, because pushing is outward-facing and `vulkan_font_engine`'s `main` has
diverged from `origin/main` — the merge that recovered this project's shader
work is a real merge of two lines of development, and it should be looked at
before it goes anywhere.

```bash
git -C framework/vk_canvas/first_party/vulkan_font_engine log --oneline origin/main..main
git -C framework/vk_canvas log --oneline origin/main..main
git -C framework/audio_engine log --oneline origin/main..main
```

### And the thing that caused all of it

`vulkan_font_engine` was pinned at a commit that did not contain this project's
shader work, which survived only as a dangling object. A build from the tree
would have produced a player that rotated every frame, skipped its PQ decode
and ran the camera's focus-peaking filter over the picture — silently, because
a push block is a memcpy into a struct nothing validates.

`framework/vk_canvas/cmake/check_composite_pc.cmake` now fails the build when
the shader's push block and `CompositePush` in `renderer.cc` disagree. It does
not, and cannot, catch a submodule pin moving backwards. **Check the pins after
any submodule operation**, and prefer `git submodule update --init --recursive`
over a manual checkout inside one.
