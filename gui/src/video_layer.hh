#pragma once

// The one-frame holder that sits between the decoder thread and the render
// thread.
//
// It turned out to be much smaller than planned. The original design had this
// class importing the frame into Vulkan and drawing it through a shader of its
// own; in this engine none of that is the consumer's work. Renderer already
// owns the external-image path — import, Y'CbCr conversion, descriptor cache,
// composite pipeline — because the camera needed exactly the same thing. What
// is genuinely OURS is the handoff: taking a frame from a decoder thread,
// deciding with the clock whether it is the right one for this vsync, and
// making sure the one it replaces is released exactly once.
//
// ── The one-frame rule ────────────────────────────────────────────────────
//
// Exactly one frame is held at a time. Queueing more looks like smoother
// playback and is not: the decoder's output pool is six buffers, and a queue
// here just moves the stall from the queue into the codec. core/clock.h
// already decides which single frame belongs on screen.

#include <mutex>

#include "core/clock.h"
#include "core/video_frame.h"

class Renderer;

namespace vp {

class VideoLayer {
public:
    ~VideoLayer();

    // Called once, before the first frame. Tells the renderer what colour the
    // frames actually are — the driver's own suggestion for a decoder buffer
    // is BT.709 regardless of the truth, and the container knows better.
    void configure(Renderer& renderer, const ColourInfo& colour);

    // From the DECODER thread. Takes ownership; releases whatever it replaces.
    void offer(DecodedFrame frame, uint64_t generation);

    // From the RENDER thread. Hands the held frame to the renderer if the
    // clock says it is due, drops it if it is too late, leaves it if it is
    // early. Returns true when a new frame was submitted.
    bool present(Renderer& renderer, const Clock& clock);

    bool hasFrame() const;

private:
    mutable std::mutex mu_;
    DecodedFrame held_;
    uint64_t     heldGeneration_ = 0;
    bool         configured_ = false;
};

}  // namespace vp
