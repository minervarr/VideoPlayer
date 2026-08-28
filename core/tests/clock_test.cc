// clock_test — A/V sync arithmetic, asserted exactly.
//
// Debug-only, plain assert(), no framework. Same convention as the engines'
// own tests. Links src/clock.cpp ALONE: if this target ever needs another
// source, the clock has picked up a dependency it should not have.
//
//   ./build/linux_debug/core/clock_test

#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "core/audio_clock.h"
#include "core/clock.h"

using namespace vp;

int main() {
    // ── The clock is paused until told otherwise ──────────────────────────
    {
        Clock c;
        assert(c.paused());
        c.advanceFreerun(100000);
        assert(c.nowUs() == 0);  // a paused clock does not move
        c.start();
        c.advanceFreerun(100000);
        assert(c.nowUs() == 100000);
    }

    // ── Present / Wait / Drop ─────────────────────────────────────────────
    {
        Clock c(20000);
        c.start();
        c.setAudioClock(1000000);

        FrameDecision d = c.decide(1000000);      // exactly due
        assert(d.action == FrameAction::Present);
        assert(d.errorUs == 0);

        d = c.decide(1041667);                    // one 24 fps frame early
        assert(d.action == FrameAction::Wait);
        assert(d.waitUs == 41667);

        d = c.decide(990000);                     // 10 ms late, under threshold
        assert(d.action == FrameAction::Present);
        assert(d.errorUs == -10000);

        d = c.decide(979999);                     // 20001 us late, over it
        assert(d.action == FrameAction::Drop);

        // Exactly AT the threshold still presents. A boundary that drops is a
        // player that throws away a frame it had time to show.
        d = c.decide(980000);
        assert(d.action == FrameAction::Present);
    }

    // ── The drop threshold scales with the file's frame rate ─────────────
    //
    // A fixed threshold cannot serve every frame rate: 20 ms is half a frame
    // at 24 fps and two and a half frames at 120, so a 120 fps file with a
    // fixed threshold accumulates lateness instead of skipping a slot. The
    // player measures the stream's period and sets half of it.
    {
        Clock c;                       // default 20000
        assert(c.dropThresholdUs() == 20000);

        c.setDropThresholdUs(4166);    // half a 120 fps frame
        assert(c.dropThresholdUs() == 4166);
        c.start();
        c.setAudioClock(1000000);
        assert(c.decide(1000000 - 4166).action == FrameAction::Present);
        assert(c.decide(1000000 - 4167).action == FrameAction::Drop);

        // Out-of-range values are REFUSED, not clamped and not accepted: they
        // mean the caller measured nonsense out of a corrupt timestamp
        // sequence, and the previous value is a better answer than either
        // extreme. Zero would drop every frame that is not perfectly on time;
        // a huge one would disable dropping entirely and let the video drift
        // away from the audio with nothing to pull it back.
        c.setDropThresholdUs(0);
        assert(c.dropThresholdUs() == 4166);
        c.setDropThresholdUs(-1);
        assert(c.dropThresholdUs() == 4166);
        c.setDropThresholdUs(10 * 1000 * 1000);
        assert(c.dropThresholdUs() == 4166);
    }

    // ── A file with no audio still advances ──────────────────────────────
    //
    // The regression this pins: the render loop's comment said "with no audio
    // track the clock free-runs" and nothing ever called advanceFreerun(). The
    // timeline stayed at zero, so the first frame presented and every frame
    // after it waited for a clock that never moved — a silent video was a
    // still image with a working decoder behind it.
    {
        Clock c;
        c.start();
        // 30 fps, ten frames, advanced a frame at a time the way a render
        // loop with no audio does it.
        for (int i = 1; i <= 10; ++i) {
            c.advanceFreerun(33333);
            assert(c.decide(33333LL * i).action == FrameAction::Present);
        }
        assert(c.nowUs() == 333330);

        // Pausing stops it dead, and resuming continues from where it was —
        // the freerun path must not accumulate time the user was not watching.
        c.pause();
        c.advanceFreerun(5000000);
        assert(c.nowUs() == 333330);
        c.start();
        c.advanceFreerun(33333);
        assert(c.nowUs() == 366663);
    }

    // ── The audio device's staircase, made into a line ───────────────────
    //
    // The measurement this exists for: AAudio updates its played-frame count
    // once per burst — on the device this was written against, exactly 50
    // times a second in exactly 20000 us steps. Video frames at 33333 us
    // cannot be scheduled on a 20000 us grid, and the result was a permanent
    // 40/20 ms alternation that reads as judder.
    {
        AudioClockInterpolator ac;

        // The device stands still for 20 ms and then jumps. Between jumps the
        // interpolated timeline must keep moving, and it must track real time.
        int64_t raw = 0;
        for (int64_t mono = 0; mono <= 20000; mono += 1000) {
            const int64_t out = ac.update(raw, mono);
            assert(out == mono);     // first anchor is exact; then wall clock
        }
        // Now the device steps to 20000, having predicted 20000 already: no
        // correction, no jump.
        raw = 20000;
        assert(ac.update(raw, 20000) == 20000);
        assert(ac.update(raw, 25000) == 25000);   // still moving mid-tread

        // The key property: a 33333 us frame interval is nameable. On the raw
        // staircase, "now" is only ever a multiple of 20000.
        assert(ac.update(raw, 33333) == 33333);
    }

    // A device that runs slightly fast is absorbed by slewing, not by
    // snapping — snapping 50 times a second is the staircase again in
    // miniature.
    {
        AudioClockInterpolator ac;
        ac.update(0, 0);
        // Device reports 21000 where we predicted 20000: 1000 us of drift,
        // an eighth of which is taken now.
        const int64_t out = ac.update(21000, 20000);
        assert(out == 20000 + 1000 / 8);
    }

    // A jump too large to be drift is an EVENT — an underrun or a seek — and
    // is believed outright rather than slewed toward over several seconds.
    {
        AudioClockInterpolator ac;
        ac.update(1000000, 0);
        assert(ac.update(5000000, 1000) == 5000000);
    }

    // The device going backwards is a seek. Believe it immediately; slewing
    // toward it would hold the video in the pre-seek segment.
    {
        AudioClockInterpolator ac;
        ac.update(5000000, 0);
        assert(ac.update(1000000, 1000) == 1000000);
    }

    // Audio that STOPS must not let the video race away. Without a cap the
    // prediction extrapolates forever off a dead anchor.
    {
        AudioClockInterpolator ac;
        ac.update(1000000, 0);
        // Ten seconds of wall clock, no new device report.
        const int64_t out = ac.update(1000000, 10000000);
        assert(out == 1000000 + 250000);
    }

    // Never backwards. Every queued frame becomes late at once if it is, and
    // the catch-up that follows is the stutter itself.
    {
        AudioClockInterpolator ac;
        ac.update(0, 0);
        const int64_t a = ac.update(0, 10000);
        const int64_t b = ac.update(9000, 10000);   // device lags the estimate
        assert(b >= a);
    }

    // ── Paused never drops ────────────────────────────────────────────────
    // The bug this guards: pause for a minute, resume, and the frame that was
    // being held is now 60 seconds "late" — dropped, along with every frame
    // after it, until the clock catches up. A paused clock has no lateness.
    {
        Clock c(20000);
        c.start();
        c.setAudioClock(1000000);
        c.pause();
        FrameDecision d = c.decide(500000);   // half a second "late"
        assert(d.action == FrameAction::Present);
        assert(c.decide(2000000).action == FrameAction::Wait);  // still in the future
    }

    // ── The audio clock may go backwards ──────────────────────────────────
    // After a seek it legitimately does. A monotonic guard here would pin the
    // timeline to the pre-seek position and drop the whole new segment.
    {
        Clock c;
        c.start();
        c.setAudioClock(5000000);
        c.setAudioClock(1000000);
        assert(c.nowUs() == 1000000);
    }

    // ── Generations ───────────────────────────────────────────────────────
    // Every reset() invalidates frames decoded before it. The counter is what
    // a frame in flight is tagged with; it must move on every seek, including
    // a seek to where we already are.
    {
        Clock c;
        const uint64_t g0 = c.generation();
        c.reset(1000000);
        assert(c.generation() == g0 + 1);
        assert(c.nowUs() == 1000000);
        c.reset(1000000);
        assert(c.generation() == g0 + 2);
    }

    std::printf("clock_test: all assertions passed\n");
    return 0;
}
