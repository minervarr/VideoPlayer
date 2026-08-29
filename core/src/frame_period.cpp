#include "core/frame_period.h"

#include <algorithm>

namespace vp {

void FramePeriod::reset() {
    live_ = next_ = 0;
    minUs_ = maxUs_ = -1;
    count_ = 0;
    // The published shortest gap deliberately SURVIVES a reset. It was measured
    // from this same stream and is still the best answer available; only the
    // accumulator restarts, so the next estimate is not computed across the
    // jump. Falling back to the 30 fps guess after every seek would be a
    // worse answer than the one already in hand.
}

void FramePeriod::add(int64_t ptsUs) {
    recent_[next_] = ptsUs;
    next_ = (next_ + 1) % kWindow;
    if (live_ < kWindow) ++live_;

    if (minUs_ < 0 || ptsUs < minUs_) minUs_ = ptsUs;
    if (maxUs_ < 0 || ptsUs > maxUs_) maxUs_ = ptsUs;
    ++count_;

    if (live_ < 2) return;

    int64_t sorted[kWindow];
    std::copy(recent_, recent_ + live_, sorted);
    std::sort(sorted, sorted + live_);

    // Adjacent differences in DISPLAY order. Duplicates — a stream that stamps
    // two blocks alike, which damaged files do — give zero and are skipped, so
    // they cannot drive the period to nothing.
    int64_t smallest = 0;
    for (size_t i = 1; i < live_; ++i) {
        const int64_t d = sorted[i] - sorted[i - 1];
        if (d > 0 && (smallest == 0 || d < smallest)) smallest = d;
    }
    if (smallest > 0) shortestUs_ = smallest;
}

int64_t FramePeriod::shortestUs() const { return shortestUs_; }

bool FramePeriod::settled() const {
    return count_ > kWarmupFrames && maxUs_ - minUs_ >= kWarmupSpanUs;
}

int64_t FramePeriod::meanUs() const {
    if (!settled()) return kAssumedUs;
    const int64_t mean = (maxUs_ - minUs_) / (count_ - 1);
    return mean > 0 ? mean : kAssumedUs;
}

}  // namespace vp
