#pragma once

// The presentation clock: when to show a frame, and when to give up on one.
//
// Audio is the master. This is not a preference — a video frame shown 20 ms
// late is invisible, and an audio sample played 20 ms late is a click. So the
// timeline IS the audio device's played-sample count, and video is scheduled
// against it.
//
// Integer microseconds throughout, and no <chrono>, no clock reads, no
// floating point. Every decision this file makes is a pure function of numbers
// handed to it, which is what lets tests/clock_test.cc assert exact answers
// instead of sampling a real clock and hoping. A/V sync is invisible when it
// is slightly wrong, which is exactly why it is tested away from the GUI.

#include <cstdint>

namespace vp {

// What to do with a frame that has been decoded and is waiting.
enum class FrameAction {
    Wait,     // its time has not come; sleep until waitUs elapses
    Present,  // show it now
    Drop,     // it is too late to matter; discard and take the next one
};

struct FrameDecision {
    FrameAction action = FrameAction::Wait;
    int64_t     waitUs = 0;   // meaningful only for Wait
    int64_t     errorUs = 0;  // frame pts minus now; negative == late
};

class Clock {
public:
    // `dropThresholdUs` — how late a frame must be before showing it is worse
    // than skipping it. One frame at 24 fps is ~41667 us; the default is a
    // little under half of that, so a frame that would land visibly on the
    // wrong side of its slot goes rather than stutters.
    explicit Clock(int64_t dropThresholdUs = 20000);

    // ── The audio master ───────────────────────────────────────────────────
    // Called by the audio path with the presentation time of the sample the
    // device is playing RIGHT NOW (not the one last written — the difference
    // is the device's own buffer, and getting it wrong offsets every frame by
    // a constant nobody can see but everybody can feel).
    void setAudioClock(int64_t playedPtsUs);

    // A file with no audio track, or audio that has not started yet: the
    // timeline advances by however long the caller says has passed. Keeps the
    // video path identical in both cases rather than growing a second one.
    void advanceFreerun(int64_t deltaUs);

    // ── Adapting to the file ───────────────────────────────────────────────
    // How late a frame may be and still be worth showing. Fixed, this is the
    // one number that cannot serve every frame rate: 20 ms is half a frame at
    // 24 fps and two and a half frames at 120, so a constant tuned for one is
    // either trigger-happy or useless at the other. The player measures the
    // stream's frame period and sets HALF of it — a frame more than half a
    // slot late belongs in the next slot, at every rate.
    //
    // Clamped rather than trusted: a corrupt or wildly variable timestamp
    // sequence must not be able to disable dropping altogether (a huge
    // threshold) or drop everything (a threshold of zero).
    void setDropThresholdUs(int64_t us);
    int64_t dropThresholdUs() const;

    int64_t nowUs() const;

    // ── Transport ──────────────────────────────────────────────────────────
    void start();
    void pause();
    bool paused() const;
    // Re-bases the timeline. After a seek the audio clock restarts from the
    // landed keyframe, and every frame still in flight is stale.
    void reset(int64_t toUs);
    // Monotonic; bumped by reset(). A frame tagged with an older generation is
    // from before the seek and is discarded without being timed at all.
    uint64_t generation() const;

    // ── The one question the video path asks ───────────────────────────────
    FrameDecision decide(int64_t framePtsUs) const;

private:
    int64_t  nowUs_    = 0;
    int64_t  dropThresholdUs_;
    bool     paused_   = true;
    uint64_t generation_ = 0;
};

}  // namespace vp
