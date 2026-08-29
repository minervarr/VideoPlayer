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

    // How long after decide() is asked the chosen frame actually appears.
    //
    // Zero is wrong whenever presentation is paced, and it was the default
    // because nothing was: with a mailbox swapchain the render loop free-ran at
    // ~900 Hz, so "now" and "when this is shown" were a millisecond apart and
    // the distinction did not exist.
    //
    // Under FIFO it does. The frame handed to the renderer now is displayed at
    // the NEXT vsync, so scheduling it against `now` centres the acceptance
    // window on a moment already past: a frame falling due just after a look
    // has to wait a whole refresh, and by the next look it is a full period
    // late and gets dropped. Measured on a 30 Hz panel with 30 fps content and
    // a queue seven frames deep — 21 frames shown a second, 12 dropped, every
    // one of them late by 31-36 ms, which is one frame exactly. The same build
    // at 60 Hz showed all 30 and dropped none, because looking twice per frame
    // halves the phase error and it stayed inside the threshold.
    //
    // Set it to one presentation interval and the window is centred on when
    // the frame will really be shown. Measured from the render loop rather
    // than assumed, so it is right whether the panel ends up at the content's
    // rate, at a multiple of it, or somewhere else entirely.
    void setPresentationLeadUs(int64_t us);
    int64_t presentationLeadUs() const;

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
    int64_t  presentationLeadUs_ = 0;
    bool     paused_   = true;
    uint64_t generation_ = 0;
};

}  // namespace vp
