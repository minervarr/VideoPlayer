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
