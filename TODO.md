# TODO

Written 2026-08-28, at the end of a full review pass. What is *done* is in the
git log and in CLAUDE.md's "Current state"; this file is only what is not.

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

### Still worth watching

- **A long soak.** The test file is about twelve seconds. Nothing here says
  what an hour looks like: thermal throttling, clock drift over minutes, or
  `presentedFrames()` behaviour after a route change (headphones, Bluetooth).
- **60 and 120 fps content.** Everything scales in frames rather than
  milliseconds and the presentation lead is measured from the loop, so it
  should follow — but "should" is not "did". The prebuffer's six frames is
  ~50 ms at 120 fps, which may be thin.
- **What happens at the end.** The player holds the last frame and keeps
  running; `Player` never reaches `Ended`. No crash, no leak seen, but nothing
  says the file is over either.
- **`AChoreographer` is still not needed.** FIFO plus the rate pin plus the
  presentation lead gives zero drops. Do not add a second pacer.

## Android — not done

- **No UI at all.** Tap or Space toggles pause, and that is the whole
  interface. `Player::seek()` works, is now correct on both sides of the A/V
  seam, and nothing calls it. A seek bar is the obvious next feature and was
  explicitly out of scope for the review pass.
- **`AImageReader_acquireNextImage` returning MAX_IMAGES_ACQUIRED is not
  handled explicitly.** The listener drains what it can and returns, and a new
  callback arrives with the next frame, so it recovers on its own: the consumer
  releasing a buffer lets the decoder produce again, which fires the callback,
  which drains the pending image too. Not a deadlock as far as the reasoning
  goes, and not something anybody has forced.
- **The `content://` launch path has never been exercised by a real file
  manager.** The code is there and the fd-to-stream path is written; nobody has
  tapped a video in Files and picked this app.
- **`AImageReader_acquireNextImage` failing with MAX_IMAGES_ACQUIRED is not
  handled.** The listener drains what it can and returns; a new callback comes
  with the next frame, so it self-corrects in practice. Under sustained
  pressure it may not.

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
