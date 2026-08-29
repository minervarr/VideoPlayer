#pragma once

// FLAC: DECODED by AMediaCodec, PLAYED through audio_engine.
//
// This header used to claim the opposite — that decode went through
// audio_engine's vendored libFLAC, as it does for a music player built on the
// same engine. It does not, and the .cc has carried the full reasoning for as
// long as the claim stood here: audio_engine's decoder is open(fd, offset,
// length), which decodes a FLAC FILE, while Matroska stores raw FLAC FRAMES
// with STREAMINFO off in CodecPrivate. There is no contiguous region to point
// an fd at. AMediaCodec's input model is exactly the container's.
//
// This is CLAUDE.md's one documented deviation, and it is written down there
// too. Revisit if audio_engine ever grows a packet-fed FLAC entry point.
//
// So this file is thin on purpose. It is an ADAPTER: core Packets in,
// AMediaCodec's decoder and audio_engine's AAudioSink out, plus the one thing
// the video path needs back — the presentation timestamp of the sample the DAC
// is playing right now, which is what drives core/clock.h.

#include "core/audio_output.h"
#include <memory>
#include <string>

#include "core/player.h"

namespace vp {

class FlacOutput final : public AudioOutput {
public:
    FlacOutput();
    ~FlacOutput();

    bool configure(const TrackEntry& audio) override;
    bool submit(const Packet& p) override;
    void flush() override;
    void start() override;
    void pause() override;
    // Idempotent by SUCCESS, as MediaCodecVideo's is: a full input queue means
    // nothing was sent and the next call retries. flush() clears the latch.
    void signalEndOfStream() override;

    // The timestamp actually reaching the speaker, NOT the last one written.
    // The difference is the output device's own buffer; treating them as the
    // same offsets every video frame by a constant that nobody can see and
    // everybody can feel. Handed straight to Clock::setAudioClock().
    // True once the first decoded buffer has reached the sink, so playedPtsUs()
    // answers with a real position instead of zero. What a prebuffer waits on.
    bool ready() const override;

    int64_t playedPtsUs() const override;

    const std::string& error() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
