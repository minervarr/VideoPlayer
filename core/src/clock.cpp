#include "core/clock.h"

namespace vp {

Clock::Clock(int64_t dropThresholdUs) : dropThresholdUs_(dropThresholdUs) {}

void Clock::setAudioClock(int64_t playedPtsUs) {
    // Assignment, not a max(): after a seek the audio clock legitimately goes
    // backwards, and a monotonic guard here would pin the timeline to the
    // pre-seek position and drop every frame of the new one.
    nowUs_ = playedPtsUs;
}

void Clock::advanceFreerun(int64_t deltaUs) {
    if (!paused_) nowUs_ += deltaUs;
}

void Clock::setDropThresholdUs(int64_t us) {
    // 2 ms is under a frame at any rate a display can show, and 100 ms is
    // already three frames at 30 fps — outside that range the caller has
    // measured nonsense, and the previous value is a better answer.
    if (us < 2000 || us > 100000) return;
    dropThresholdUs_ = us;
}

int64_t Clock::dropThresholdUs() const { return dropThresholdUs_; }

void Clock::setPresentationLeadUs(int64_t us) {
    // Negative is meaningless; a lead longer than a few frames is a measurement
    // gone wrong (a stalled loop, a debugger) and would schedule the whole
    // pipeline into the future.
    if (us < 0 || us > 100000) return;
    presentationLeadUs_ = us;
}

int64_t Clock::presentationLeadUs() const { return presentationLeadUs_; }

int64_t Clock::nowUs() const { return nowUs_; }

void Clock::start() { paused_ = false; }
void Clock::pause() { paused_ = true; }
bool Clock::paused() const { return paused_; }

void Clock::reset(int64_t toUs) {
    nowUs_ = toUs;
    ++generation_;
}

uint64_t Clock::generation() const { return generation_; }

FrameDecision Clock::decide(int64_t framePtsUs) const {
    FrameDecision d;
    // Against when the frame will be SHOWN, not when it was asked about. See
    // setPresentationLeadUs() — the difference is one refresh interval, and
    // ignoring it drops about a third of the frames on a panel running at the
    // content's own rate.
    d.errorUs = framePtsUs - (nowUs_ + presentationLeadUs_);

    // Paused: the clock does not move, so a frame is never late and never
    // becomes late. Present the one that is due and hold it. Without this the
    // first frame after a pause would be dropped the moment the user resumed,
    // because it had been "late" for however long they were away.
    if (paused_) {
        d.action = d.errorUs <= 0 ? FrameAction::Present : FrameAction::Wait;
        d.waitUs = 0;
        return d;
    }

    // A window CENTRED on the moment the frame will be shown, half a frame
    // either side. Present the one nearest that moment; wait if the nearest is
    // still more than half a slot away; drop it if it is more than half a slot
    // past.
    //
    // The window used to end at zero — wait for anything not yet due, present
    // anything up to half a slot late. That is right only when the caller looks
    // far more often than frames arrive, which was true under a mailbox
    // swapchain running at ~900 Hz and stopped being true under FIFO. Looking
    // once per frame at a 16 ms window inside a 33 ms interval catches half the
    // frames and drops the rest: measured at 31 shown and 30 dropped a second
    // on 30 fps content, which consumed the stream at twice its rate and ran a
    // 233 MB file dry in fifteen seconds.
    //
    // Symmetric, the window is exactly one frame period wide, so each look
    // consumes exactly one frame. Presenting one up to half a slot early is not
    // a compromise: it IS the frame closest to the instant being filled, and
    // the alternative is showing its predecessor a second time.
    if (d.errorUs > dropThresholdUs_) {
        d.action = FrameAction::Wait;
        // Until it enters the window, not until its timestamp.
        d.waitUs = d.errorUs - dropThresholdUs_;
    } else if (-d.errorUs > dropThresholdUs_) {
        d.action = FrameAction::Drop;
    } else {
        d.action = FrameAction::Present;
    }
    return d;
}

}  // namespace vp
