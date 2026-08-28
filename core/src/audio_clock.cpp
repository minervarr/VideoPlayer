#include "core/audio_clock.h"

namespace vp {
namespace {

// How much of the disagreement between device and prediction to absorb per
// update. Snapping the anchor straight onto every report would reintroduce the
// staircase in a subtler form — the prediction would be yanked back a
// fractional millisecond 50 times a second. An eighth converges within a few
// bursts and stays smooth in between.
constexpr int64_t kSlewDivisor = 8;

// The prediction is only ever allowed this far past the last thing the DEVICE
// actually said. Without a cap, an audio stall — a route change, an underrun,
// a stream that stops being fed — leaves this extrapolating forever, and the
// video races off into a file the speaker never reached. A quarter second is
// several bursts: long enough that normal jitter never touches it.
constexpr int64_t kMaxLeadUs = 250000;

}  // namespace

AudioClockInterpolator::AudioClockInterpolator(int64_t resyncThresholdUs)
    : resyncUs_(resyncThresholdUs) {}

void AudioClockInterpolator::reset() {
    have_ = false;
    anchorPts_ = anchorMono_ = lastRaw_ = lastOut_ = 0;
}

int64_t AudioClockInterpolator::update(int64_t rawPtsUs, int64_t monoUs) {
    // First call, or the device went BACKWARDS — which is a seek, not drift.
    if (!have_ || rawPtsUs < lastRaw_) {
        have_       = true;
        anchorPts_  = rawPtsUs;
        anchorMono_ = monoUs;
        lastRaw_    = rawPtsUs;
        lastOut_    = rawPtsUs;
        return rawPtsUs;
    }

    int64_t predicted = anchorPts_ + (monoUs - anchorMono_);

    if (rawPtsUs != lastRaw_) {
        // The device just told us something new. How wrong were we?
        const int64_t drift = rawPtsUs - predicted;
        const int64_t magnitude = drift < 0 ? -drift : drift;
        if (magnitude > resyncUs_) {
            // Too far to be drift: an underrun, a route change, a seek that
            // reset() did not cover. Believe the device outright.
            anchorPts_  = rawPtsUs;
            anchorMono_ = monoUs;
            lastOut_    = 0;          // a hard resync may legitimately go back
        } else {
            anchorPts_ += drift / kSlewDivisor;
        }
        lastRaw_  = rawPtsUs;
        predicted = anchorPts_ + (monoUs - anchorMono_);
    }

    // Never run away from what the speaker has actually reached.
    if (predicted > lastRaw_ + kMaxLeadUs) predicted = lastRaw_ + kMaxLeadUs;

    // Monotonic. A timeline that steps back makes every queued frame late at
    // once, and the catch-up that follows is exactly the stutter this class
    // exists to remove.
    if (predicted < lastOut_) predicted = lastOut_;
    lastOut_ = predicted;
    return predicted;
}

}  // namespace vp
