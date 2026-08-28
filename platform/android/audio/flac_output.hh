#pragma once

// FLAC, through audio_engine — not through AMediaCodec.
//
// The engine in framework/audio_engine already decodes FLAC with its own
// vendored libFLAC on every platform it runs on, deliberately: some handsets
// ship no FLAC decoder, and a per-device decode path makes the audio depend on
// the phone. Matrix Player made that call for the same library and wrote down
// why (framework/audio_engine/CLAUDE.md); this project inherits it rather than
// re-deciding it.
//
// So this file is thin on purpose. It is an ADAPTER: core Packets in,
// audio_engine's decoder and output backend out, plus the one thing the video
// path needs back — the presentation timestamp of the sample the DAC is
// playing right now, which is what drives core/clock.h.

#include <memory>
#include <string>

#include "core/player.h"

namespace vp {

class FlacOutput {
public:
    FlacOutput();
    ~FlacOutput();

    bool configure(const TrackEntry& audio);
    bool submit(const Packet& p);
    void flush();
    void start();
    void pause();

    // The timestamp actually reaching the speaker, NOT the last one written.
    // The difference is the output device's own buffer; treating them as the
    // same offsets every video frame by a constant that nobody can see and
    // everybody can feel. Handed straight to Clock::setAudioClock().
    int64_t playedPtsUs() const;

    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
