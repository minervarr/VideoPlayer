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
// ── Why a small queue, and not one frame ──────────────────────────────────
//
// This held exactly ONE frame at first, on the reasoning that queueing more
// only moves a stall from the queue into the codec. That reasoning was wrong,
// and the phone said so: decode ran at 38 fps and the screen updated twice a
// second.
//
// The decoder legitimately runs AHEAD of the clock — it must, or a slow frame
// has no slack. With one slot, every frame that arrives before its time
// replaces a frame that had not come due yet, and the replaced one is gone.
// Almost nothing is ever presented; what reaches the screen is whichever frame
// happens to be in hand at the moment the clock crosses it.
//
// So: a few frames deep, presented in order, with everything older than `now`
// dropped in one go rather than one per vsync. Deep enough to always hold the
// frame that is due; shallow enough that the decoder's own six-buffer pool
// still provides the backpressure that keeps the feed thread honest.

#include <deque>
#include <mutex>

// Playback statistics, off by default.
//
// These ran unconditionally while the pipeline was being diagnosed: three
// log lines a second and eleven members' worth of counters in the hot path,
// committed and left there. What they measured is genuinely useful when
// something is wrong and pure overhead when nothing is, which is what a
// compile-time switch is for. Build with -DVP_STATS=1 to get the one line
// that describes the whole pipeline back.
#ifndef VP_STATS
#define VP_STATS 0
#endif
#if VP_STATS
#include <chrono>
#define VP_STAT(expr) do { expr; } while (0)
#else
#define VP_STAT(expr) do {} while (0)
#endif

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
    // `rotationDegrees` is the container's, clockwise, already snapped to a
    // quadrant by the parser.
    void configure(Renderer& renderer, const ColourInfo& colour,
                   int rotationDegrees);

    // From the DECODER thread. Takes ownership. Refuses the NEW frame when the
    // queue is full — never the oldest, which is the one about to come due.
    // (This comment said the exact opposite of what offer() does for a while;
    // the long note above the check in video_layer.cc explains why dropping
    // the oldest was measured to be exactly wrong, and is the version to
    // trust.)
    void offer(DecodedFrame frame, uint64_t generation);

    // From the RENDER thread. Discards every queued frame the clock has
    // already passed, and hands the newest due one to the renderer. Returns
    // true when a frame was submitted.
    //
    // Catching up in ONE call rather than one frame per vsync matters after a
    // stall: presenting the backlog one frame at a time would play it back in
    // slow motion instead of skipping to where the audio already is.
    bool present(Renderer& renderer, const Clock& clock);

    bool hasFrame() const;

    // How many decoded frames are waiting. What the prebuffer counts: playback
    // should not begin until there is a cushion, and a cushion is measured in
    // FRAMES rather than milliseconds for the same reason everything else here
    // is — 200 ms is 6 frames at 30 fps and 24 at 120.
    size_t queued() const;

    // Room for another decoded frame. Nothing gates on this today — the feed
    // paces itself in frames instead, because gating here stalls audio along
    // with video (the feed holds one packet) and audio is the clock. Kept
    // because "is the consumer keeping up" is a fair question to ask, and the
    // measurement that settled it is in player_view.cc.
    bool hasRoom() const;

private:
    struct Queued {
        DecodedFrame frame;
        uint64_t     generation = 0;
    };

    mutable std::mutex mu_;
    std::deque<Queued> queue_;
    bool               configured_ = false;

    // A FRAME COUNT, and the pipeline's real bound. Deliberately not derived
    // from a duration: 300 ms is 9 frames at 30 fps and 36 at 120, so a queue
    // sized for one frame rate silently drops frames at another. The feed asks
    // hasRoom() instead, so the same 8 slots mean 260 ms at 30 fps and 65 ms at
    // 120 — less lead at high frame rates, which is exactly right, because a
    // frame period is the unit that matters.
    //
    // 12, against the decoder's 16-buffer pool. Measured, not reasoned: 8 was
    // tried and cost a third of the frame rate, because the feed holds ONE
    // packet and a full video queue therefore stalls AUDIO too — and audio is
    // the clock. A queue deep enough that backpressure is rare is worth more
    // than a queue sized to the minimum that should theoretically work.
    //
    // Dropping is still implemented for what it is for — falling genuinely
    // behind — but it should not be reached by simply playing.
    static constexpr size_t kMaxQueued = 12;

#if VP_STATS
    std::chrono::steady_clock::time_point lastPresentTp_{};
    std::chrono::steady_clock::time_point presentWindow_{};
    int64_t presentMaxUs_ = 0, presentTotalUs_ = 0, presentCount_ = 0;
    int64_t dropped_ = 0, refused_ = 0, dropWorstUs_ = 0, dropBestUs_ = 0;
#endif
};

}  // namespace vp
