#pragma once

// The seam between "a decoder produced a picture" and "the canvas drew one".
//
// This is the single most important type in the project to keep honest,
// because it is where an Android type would most naturally leak into portable
// code. It does not: `handle` is an opaque void* whose real identity
// (AHardwareBuffer* today) is known ONLY to platform/android/codec/ on the
// producing side and gui/video_layer.cc on the consuming side. `core/` passes
// it, times it, and drops it, and never dereferences it.
//
// The release callback is not a courtesy. A decoder's output buffers are a
// small fixed pool; hold one past its usefulness and decode stalls. Whoever
// finishes with a DecodedFrame calls release(), exactly once.

#include <cstdint>
#include <functional>

#include "core/mkv.h"

namespace vp {

struct DecodedFrame {
    void*    handle = nullptr;     // AHardwareBuffer* on Android. Opaque here.
    int64_t  ptsUs  = 0;

    uint32_t width  = 0;
    uint32_t height = 0;
    // Display geometry, when the container asks for a different aspect than
    // the coded one. 0 means "same as width/height".
    uint32_t displayWidth  = 0;
    uint32_t displayHeight = 0;

    // Copied from the TRACK, not sniffed from the picture — the container is
    // the authority (rule 3). The render path reads exactly this to decide the
    // matrix, the transfer function, and whether the PQ pipeline runs at all.
    ColourInfo colour;

    // Called once when the consumer is done with `handle`. Empty on a frame
    // that owns nothing (the end-of-stream marker below).
    std::function<void()> release;

    bool valid() const { return handle != nullptr; }
};

// A frame with no handle and no release, carrying only a timestamp: what a
// decoder emits when the stream ends, so the clock can run out the last real
// frame's duration instead of stopping one frame early.
inline DecodedFrame endOfStream(int64_t ptsUs) {
    DecodedFrame f;
    f.ptsUs = ptsUs;
    return f;
}

}  // namespace vp
