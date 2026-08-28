#include "audio/flac_output.hh"

namespace vp {

// NOT YET IMPLEMENTED — build step 7.
//
// An adapter over audio_engine's existing FLAC decoder and AAudio backend, so
// the work here is wiring, not decoding. The one thing it must get right is
// playedPtsUs(): the timestamp reaching the speaker, not the last one written.
struct FlacOutput::Impl {
    std::string err = "FLAC output is not implemented yet (build step 7)";
    int64_t playedPtsUs = 0;
};

FlacOutput::FlacOutput() : impl_(std::make_unique<Impl>()) {}
FlacOutput::~FlacOutput() = default;

bool FlacOutput::configure(const TrackEntry&) { return false; }
bool FlacOutput::submit(const Packet&) { return false; }
void FlacOutput::flush() {}
void FlacOutput::start() {}
void FlacOutput::pause() {}
int64_t FlacOutput::playedPtsUs() const { return impl_->playedPtsUs; }
const std::string& FlacOutput::error() const { return impl_->err; }

}  // namespace vp
