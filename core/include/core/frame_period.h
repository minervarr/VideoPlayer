#pragma once

// How fast is this file, measured from its timestamps?

//
// Nothing in this player is tuned for one frame rate — the feed's lead, the
// drop threshold and the rate handed to the display are all derived from the
// stream's own frame period. So the estimate has to be right for content
// nobody has tried yet, and it has to be right for content whose timestamps do
// not arrive in order.
//
// ── Why timestamps arrive out of order ─────────────────────────────────────
//
// Matroska block timestamps are PRESENTATION timestamps, stored in DECODE
// order. Any stream with B-frames therefore delivers them non-monotonically by
// design: an I P B B group arrives stamped for display at t, t+3, t+1, t+2.
//
// This is the whole reason the class exists. The estimator it replaces took
// the minimum of `pts - previousPts` over consecutive packets, which is the
// right statistic only while timestamps rise. On the group above, consecutive
// differences are +3, -2, +1 periods; the small positive one is what the
// minimum latched onto, collapsing the estimate to a fraction of the truth.
// That shortens the feed's lead and asks the compositor for a frame rate the
// file does not have. It went unnoticed because the only file this player had
// been tested against is all-intra, where nothing is ever reordered.
//
// SORTING a short window puts the timestamps back into display order, whatever
// order they arrived in, and adjacent differences there are frame periods
// again. Two statistics are then taken from the same window, deliberately:
//
//   shortest() — the smallest adjacent gap. Conservative, for the feed's lead:
//                underestimating the period only makes the lead shorter, and a
//                short lead costs a little buffering rather than correctness.
//   mean()     — the span divided by the count. For "what rate is this file",
//                which is what the display is told. The minimum is the wrong
//                statistic there and the phone said so: a 30 fps recording
//                contains the odd 25 ms gap, so the minimum settled on 25000
//                and the display was duly asked for 40 fps.
//
// Pure arithmetic over integers, no clock and no OS, which is what lets
// tests/frame_period_test.cc assert exact answers on a reordered sequence —
// including one this project has no sample of.

#include <cstddef>
#include <cstdint>

namespace vp {

class FramePeriod {
public:
    // How many recent timestamps are sorted to find the shortest gap. Covers
    // any reordering depth a real HEVC encoder produces — a B-pyramid is
    // typically 3 or 4 deep — with room to spare, and sorting 16 values a few
    // dozen times a second costs nothing measurable.
    static constexpr size_t kWindow = 16;

    // Before anything is known. A 30 fps guess, used for the first packets and
    // never reported as measured — see settled().
    static constexpr int64_t kAssumedUs = 33'333;

    void add(int64_t ptsUs);

    // Everything below is discarded. Called on a seek: timestamps either side
    // of one are unrelated, and a window straddling it describes neither.
    void reset();

    // The smallest positive gap between adjacent timestamps in display order.
    // kAssumedUs until at least two have been seen.
    int64_t shortestUs() const;

    // The mean gap over everything seen since the last reset. kAssumedUs until
    // settled(). Held as a span and a count rather than a running average, so
    // it costs two adds and never accumulates rounding.
    int64_t meanUs() const;

    // Whether meanUs() is measured rather than guessed.
    //
    // Two conditions, both learned on the phone: enough FRAMES for a mean to be
    // a mean, and enough of the stream's own SPAN behind it. With only the
    // frame count the estimate still crept — 34.3 fps down to 30.3 over nine
    // seconds — because the feed reads ahead in bursts and the first dozen
    // packets are not a second of anything.
    bool settled() const;

    // Where this stream's timeline BEGINS. Not assumed to be zero, because it
    // usually is not: this phone's own recordings start at 171 ms. The feed's
    // read-ahead is measured from here while the pipeline primes, since the
    // clock reads zero until playback starts.
    int64_t firstUs() const { return minUs_; }

    // The furthest point reached, which is what says how far ahead a reader
    // has got. NOT the last timestamp handed in: with reordering those are
    // different numbers.
    int64_t furthestUs() const { return maxUs_; }

    int64_t count() const { return count_; }

private:
    static constexpr int64_t kWarmupSpanUs = 1'000'000;
    static constexpr int64_t kWarmupFrames = 16;

    int64_t recent_[kWindow] = {};
    size_t  live_ = 0;    // how many slots hold a real timestamp
    size_t  next_ = 0;    // circular write cursor

    int64_t minUs_ = -1;
    int64_t maxUs_ = -1;
    int64_t count_ = 0;

    int64_t shortestUs_ = kAssumedUs;
};

}  // namespace vp
