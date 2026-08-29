#pragma once

// The audio half of the platform seam, and Sink's counterpart.
//
// Sink (core/player.h) is the VIDEO decoder: configure, submit, flush. Audio
// needs two things Sink has no business carrying — a transport, and a clock —
// which is why it is a separate interface rather than a second Sink. The clock
// is the important one: audio is this player's master timeline, so
// playedPtsUs() is the single number every video frame is scheduled against.
//
// It is an interface at all because gui/ must not name a platform type.
// player_view.cc held a concrete FlacOutput — the Android AMediaCodec + AAudio
// path — which compiled nowhere else and was the last thing keeping gui/ from
// building for a second host.
//
// No OS header, no decoder type: a Packet in, a TrackEntry to configure from,
// microseconds out.

#include <cstdint>
#include <string>

#include "core/demux.h"
#include "core/mkv.h"

namespace vp {

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // Once, before any packet. False means this track cannot be played, and
    // error() says why — the player continues without sound rather than
    // refusing the file, because a video that plays silently is better than a
    // video that does not play.
    virtual bool configure(const TrackEntry& audio) = 0;

    // False when the decoder's input queue is full. The caller retries rather
    // than dropping: a dropped compressed packet is audible.
    virtual bool submit(const Packet& p) = 0;

    // Drop everything in flight — the decoder's queues and the device's own
    // buffer. Called on seek, where anything still queued belongs to the
    // segment just left.
    virtual void flush() = 0;

    // The transport. Nothing is audible until start(), which is what lets the
    // player fill its pipeline before the timeline begins to move.
    virtual void start() = 0;
    virtual void pause() = 0;

    // True once the first decoded buffer has reached the device, so
    // playedPtsUs() answers with a position rather than with zero. What the
    // prebuffer waits on.
    virtual bool ready() const = 0;

    // The presentation time of the sample the speaker is playing RIGHT NOW —
    // not the one last written. The difference is the device's own buffer, and
    // getting it wrong offsets every video frame by a constant nobody can see
    // and everybody can feel.
    //
    // Implementations report the device's own position, however coarsely they
    // can. Smoothing it is the caller's job and belongs in one place:
    // core/audio_clock.h.
    virtual int64_t playedPtsUs() const = 0;

    virtual const std::string& error() const = 0;
};

}  // namespace vp
