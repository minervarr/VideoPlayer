#pragma once

// Turning the audio device's staircase into a line.
//
// AAudio reports how many frames the speaker has consumed, and it updates that
// count once per BURST — not continuously. Measured on a Galaxy S23 Ultra at
// 48 kHz: exactly 50 updates a second, each exactly 20000 us. The timeline
// does not advance between them; it stands still and then jumps.
//
// Scheduling video against that is the whole reason this player juddered. A 30
// fps file needs frames at 33333 us intervals, and a clock that only exists at
// multiples of 20000 can never name those instants: every frame comes due one
// tread late, so the gaps between presentations alternate 40 ms and 20 ms
// around a correct-looking 33.3 ms average. Measured before this class
// existed: avg 33343 us, worst 40-42 ms, with 10 frames a second judged 17-19
// ms late — one tread's worth, exactly.
//
// The fix is what every mature player does: treat the device's report as an
// ANCHOR rather than as the time, and read the wall clock between anchors.
// The device says where the speaker was and when; the monotonic clock says how
// long ago that was.
//
// Pure integer arithmetic on microseconds, and the caller supplies the
// monotonic reading — so this has no <chrono>, no clock of its own, and
// tests can assert exact answers instead of sampling a real one (rule 1).

#include <cstdint>

namespace vp {

class AudioClockInterpolator {
public:
    // `resyncThresholdUs` — how far the device may disagree with the
    // prediction before the disagreement is treated as an EVENT rather than
    // as drift. A seek, an underrun or a route change moves the audio
    // position by much more than a burst; slewing gently toward those would
    // take seconds to converge, and during those seconds the video is wrong.
    explicit AudioClockInterpolator(int64_t resyncThresholdUs = 100000);

    // Forget everything. After a seek the device's position restarts and no
    // anchor from before it means anything.
    void reset();

    // `rawPtsUs` — the presentation time the device reports right now, the
    // staircase. `monoUs` — any monotonic microsecond counter; only its
    // DIFFERENCES are used, so its origin does not matter.
    //
    // Returns the interpolated timeline. Never moves backwards.
    int64_t update(int64_t rawPtsUs, int64_t monoUs);

private:
    bool    have_       = false;
    int64_t anchorPts_  = 0;   // device position at the last anchor
    int64_t anchorMono_ = 0;   // monotonic reading at that anchor
    int64_t lastRaw_    = 0;   // to notice the device actually moved
    int64_t lastOut_    = 0;   // monotonicity guard
    int64_t resyncUs_;
};

}  // namespace vp
