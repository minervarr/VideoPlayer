// frame_period_test — the frame-rate estimator, over timestamps that do not
// arrive in order.
//
// This test exists because the bug it guards against was invisible on the only
// file the project had: an all-intra recording, where presentation order and
// storage order are the same. Any stream with B-frames delivers them
// differently, and there is no such sample here to try. The arithmetic is
// pure, so the sample can be written down instead.
//
//   ./build/linux_debug/core/frame_period_test

#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "core/frame_period.h"

using namespace vp;

namespace {

// One 30 fps frame, to the microsecond the container would hold.
constexpr int64_t kP = 33333;

}  // namespace

int main() {
    // ── Nothing seen yet: the guess, and nothing claimed ──────────────────
    {
        FramePeriod f;
        assert(f.shortestUs() == FramePeriod::kAssumedUs);
        assert(f.meanUs() == FramePeriod::kAssumedUs);
        assert(!f.settled());
        assert(f.firstUs() == -1);
        assert(f.count() == 0);
    }

    // ── Monotonic, the case that always worked ────────────────────────────
    {
        FramePeriod f;
        for (int i = 0; i < 60; ++i) f.add(i * kP);
        assert(f.shortestUs() == kP);
        assert(f.settled());
        assert(f.meanUs() == kP);
        assert(f.firstUs() == 0);
        assert(f.furthestUs() == 59 * kP);
    }

    // ── REORDERED: the case that did not ──────────────────────────────────
    //
    // I P B B, the classic group, delivered in decode order. Display order is
    // 0 1 2 3; storage order is 0 3 1 2. The old estimator took the minimum
    // CONSECUTIVE difference, which over this sequence includes +1 period
    // between the two B frames and would have been right by luck — so the
    // sequence below uses a deeper group where it is not.
    {
        FramePeriod f;
        // A 4-deep pyramid: display 0..7 stored as 0 4 2 1 3 8 6 5 7.
        const int display[] = {0, 4, 2, 1, 3, 8, 6, 5, 7,
                               12, 10, 9, 11, 16, 14, 13, 15};
        for (int d : display) f.add(static_cast<int64_t>(d) * kP);

        // The shortest gap in DISPLAY order is exactly one frame period, and
        // the estimator finds it despite never having seen two adjacent
        // frames adjacently.
        assert(f.shortestUs() == kP);

        // The furthest point reached is the largest timestamp, not the last
        // one handed in — the last was 15 and the furthest is 16.
        assert(f.furthestUs() == 16 * kP);
        assert(f.firstUs() == 0);
    }

    // ── The failure the old estimator actually had ────────────────────────
    //
    // Consecutive differences on a reordered stream include values SMALLER
    // than a frame period. Asserted here so the sequence is on record: a
    // minimum taken over these would report 1 period where the truth is 2.
    {
        FramePeriod f;
        // Display 0 2 4 6 stored as 0 4 2 6 — a stream at HALF the base rate,
        // reordered. Consecutive storage differences are +4, -2, +4 periods;
        // the true period is 2.
        f.add(0);
        f.add(4 * kP);
        f.add(2 * kP);
        f.add(6 * kP);
        assert(f.shortestUs() == 2 * kP);   // not kP, which is what a
                                            // consecutive minimum would find
    }

    // ── Duplicate timestamps cannot drive the period to zero ──────────────
    {
        FramePeriod f;
        f.add(0);
        f.add(0);
        f.add(kP);
        f.add(kP);
        assert(f.shortestUs() == kP);
    }

    // ── settled() needs both a frame count AND a span ─────────────────────
    {
        FramePeriod f;
        // Plenty of frames, but only 20 ms of content: a burst, not a second.
        for (int i = 0; i < 40; ++i) f.add(i * 500);
        assert(!f.settled());
        assert(f.meanUs() == FramePeriod::kAssumedUs);

        FramePeriod g;
        // A full second of span but only three frames.
        g.add(0);
        g.add(500000);
        g.add(1000000);
        assert(!g.settled());
    }

    // ── The mean is the mean, not the minimum ─────────────────────────────
    //
    // The measurement that motivated two statistics: a 30 fps recording with
    // the odd short gap. This is the file that made the player ask the display
    // for 40 fps, because the minimum settled on 25000 and stayed there.
    {
        FramePeriod f;
        int64_t t = 0;
        for (int i = 0; i < 40; ++i) {
            f.add(t);
            t += (i == 38) ? 25000 : kP;   // one short gap, near the end
        }
        // While the short gap is still inside the window it IS the shortest,
        // which is the conservative answer the feed's lead wants.
        assert(f.shortestUs() == 25000);
        assert(f.settled());

        // The mean is barely moved by it — one short gap in forty pulls it
        // down by about 200 us, well inside 1% — and nowhere near the 25000
        // the minimum reports. This is the number the display is told, and the
        // reason it is a different statistic: 33119 asks for 30.19 fps, while
        // 25000 asks for 40.
        const int64_t mean = f.meanUs();
        assert(mean > kP - kP / 100 && mean < kP + kP / 100);

        // And the shortest RECOVERS once the gap scrolls out of the window —
        // the old estimator latched its minimum for the life of the file, so a
        // single odd gap in the first second mis-set the frame rate for the
        // whole of it. A window forgets.
        for (size_t i = 0; i < FramePeriod::kWindow; ++i) { f.add(t); t += kP; }
        assert(f.shortestUs() == kP);
    }

    // ── reset() drops the accumulator and KEEPS the best answer so far ────
    {
        FramePeriod f;
        for (int i = 0; i < 60; ++i) f.add(i * kP);
        assert(f.settled());
        f.reset();
        assert(!f.settled());
        assert(f.count() == 0);
        assert(f.firstUs() == -1);
        // Still the period measured from this same stream: a seek does not
        // make the file a different frame rate.
        assert(f.shortestUs() == kP);
    }

    std::printf("frame_period_test: all assertions passed\n");
    return 0;
}
