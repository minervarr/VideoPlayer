# TODO

Written 2026-08-28, at the end of a full review pass. What is *done* is in the
git log and in CLAUDE.md's "Current state"; this file is only what is not.

Ordered by what the project is for: Android has to be perfect first, and Linux
is parked deliberately rather than half-built.

---

## Android — verify on the device

**Nothing in this pass has been run on a phone.** Everything below builds for
both ABIs, and the two desktop tests pass, but no device was attached while it
was written. That is the single biggest outstanding item, and until it is done
every claim here is "compiles and reasons correctly", not "works".

```bash
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-arm64-v8a-debug.apk
adb logcat -s VideoMain:V AndroidHost:V video_player:V VideoCodec:V VideoAudio:V
```

What to look for, in order:

| Change | The line that proves it | What failure looks like |
|---|---|---|
| Shaders restored | `video: … HDR10 (PQ, BT.2020) rotation=0`, `hdr=1`, `fellBack == false` | picture sideways, or washed out, or red speckle on hard edges |
| Audio clock interpolated | presentation gaps steady at the content period | gaps alternating 40 ms / 20 ms around a correct-looking average |
| Prebuffer | `prebuffer: 6 frames + audio after N ms` | the line says "gave up waiting", or N is large |
| FIFO present | `Swapchain present mode=2 (vsync-paced)` | mode=1, and the loop still free-running |
| Display peak | `display peak N nits` on the `video:` line | `(unknown; using the content's)` — see below |
| Pixel aspect | `par=1.0000` on the `video:` line | anything else on a square-pixel file |

The cadence numbers need the statistics build, which is off by default:

```bash
cd android && ./gradlew assembleDebug -PVP_STATS=1
```

That prints one line a second: frames shown, average and worst gap, drops, queue
depth, and how late the dropped ones were.

### Specifically worth watching

- **Whether FIFO fights `ANativeWindow_setFrameRate`.** If the panel lands at
  exactly the content rate rather than a multiple of it, the two are not phase
  locked, and frames will occasionally fall two-to-a-vsync or none. The drop
  threshold (half a period) absorbs it, but the symptom would be an occasional
  single-frame hitch on otherwise clean playback. If it shows up, the fix is
  `AChoreographer` — deliberately NOT done yet, because FIFO plus the rate pin
  should be enough and a second pacer on top of a sufficient one is worse than
  none. Measure before building it.
- **Whether `presentedFrames()` (AAudio `getTimestamp`) is available at all** on
  the S23. It returns false for the first few hundred ms by design and the code
  falls back to `framesPlayed()`, but if it *never* succeeds the constant A/V
  offset it was added to remove is still there. Worth one log line to confirm.
- **Whether the S23 actually reports a display peak.** The tone map now targets
  the panel's range rather than the content's, via
  `activity::display_hdr_headroom()`. It only ever tightens, and a display that
  will not say leaves the previous behaviour exactly in place — so the risk is
  not that it breaks, it is that it silently does nothing. The startup line
  says which happened. If it reports a peak much BELOW the content's and the
  picture now looks dimmer than it did, that is the tone map doing its job, but
  it is worth looking at against the stock player before believing it.
- **Whether the prebuffer's 6 frames is the right cushion** at 60 and 120 fps.
  It is ~200 ms at 30 and ~50 ms at 120, which is the intended scaling, but
  50 ms may be too thin. `kPrimeFrames` in `gui/src/player_view.cc`.

---

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
