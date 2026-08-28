#include "codec/mediacodec_video.hh"

namespace vp {

// NOT YET IMPLEMENTED — build step 3.
//
// The shape is settled (see the header): AMediaCodec configured for
// video/hevc Main10, decoding into an AImageReader, whose AHardwareBuffer
// becomes DecodedFrame::handle. What is deliberately absent until this is
// real: any software fallback. A device with no HEVC Main10 hardware decoder
// gets a refusal and a message, not a second decode path with different
// colour behaviour.
struct MediaCodecVideo::Impl {
    Player::FrameReady onFrame;
    std::string err;
};

MediaCodecVideo::MediaCodecVideo(Player::FrameReady onFrame)
    : impl_(std::make_unique<Impl>()) {
    impl_->onFrame = std::move(onFrame);
}
MediaCodecVideo::~MediaCodecVideo() = default;

bool MediaCodecVideo::configure(const TrackEntry*, const TrackEntry*) {
    impl_->err = "the HEVC decoder is not implemented yet (build step 3)";
    return false;
}
bool MediaCodecVideo::submit(const Packet&) { return false; }
void MediaCodecVideo::flush() {}
const std::string& MediaCodecVideo::error() const { return impl_->err; }

}  // namespace vp
