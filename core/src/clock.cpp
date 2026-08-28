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
    d.errorUs = framePtsUs - nowUs_;

    // Paused: the clock does not move, so a frame is never late and never
    // becomes late. Present the one that is due and hold it. Without this the
    // first frame after a pause would be dropped the moment the user resumed,
    // because it had been "late" for however long they were away.
    if (paused_) {
        d.action = d.errorUs <= 0 ? FrameAction::Present : FrameAction::Wait;
        d.waitUs = 0;
        return d;
    }

    if (d.errorUs > 0) {
        d.action = FrameAction::Wait;
        d.waitUs = d.errorUs;
    } else if (-d.errorUs > dropThresholdUs_) {
        d.action = FrameAction::Drop;
    } else {
        d.action = FrameAction::Present;
    }
    return d;
}

}  // namespace vp
