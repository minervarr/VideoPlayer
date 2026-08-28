# VideoPlayer

A native Android video player built on Vulkan, with no FFmpeg.

It plays one thing and tries to play it correctly: **HEVC Main10 in Matroska,
HDR10 — ST 2084 PQ, BT.2020 — with FLAC audio.**

- **Container:** parsed here, in ~600 lines of portable C++17 (`core/`). The NDK's
  `AMediaExtractor` will not hand back Matroska's `Colour` element, which is the one
  piece of information this player exists to honor.
- **Video:** the phone's own HEVC hardware decoder via `AMediaCodec`, decoding into an
  `AHardwareBuffer` — *not* onto a Surface, so the frame can be colour-managed here
  rather than by the compositor.
- **Colour:** BT.2020 → PQ in a Vulkan shader, into an HDR10 swapchain
  (`VK_COLOR_SPACE_HDR10_ST2084_EXT`).
- **Audio:** FLAC through a vendored libFLAC on every platform, so the sound does not
  depend on the handset.

Built on three first-party libraries, each its own repository:
[vk_canvas](https://github.com/minervarr/Vk_Canvas_Lb_LAW),
[audio_engine](https://github.com/minervarr/audio_engine), and
[app_shell](https://github.com/minervarr/App_shell).

## Setup

```bash
git submodule update --init --recursive
```

## Build

```bash
# Desktop — the container reader and the clock, plus their tests.
# No NDK, no GPU, no phone.
cmake -B build/linux_debug -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build/linux_debug
ctest --test-dir build/linux_debug --output-on-failure

# Android — the app.
cd android && ./gradlew assembleDebug
```

`CLAUDE.md` documents the layout and the five rules the layout exists to enforce.

## License

Copyright (C) 2026 nava.

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU Affero General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.
See the [`LICENSE`](LICENSE) file for the full text.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See the GNU Affero General Public License for more details.
