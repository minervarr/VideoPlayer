#include "video_layer.hh"

#include <android/hardware_buffer.h>

#include "codec/hdr_metadata.hh"
#include "renderer.hh"

namespace vp {

VideoLayer::~VideoLayer() {
    std::lock_guard<std::mutex> lock(mu_);
    if (held_.release) held_.release();
}

void VideoLayer::configure(Renderer& renderer, const ColourInfo& colour) {
    if (configured_) return;
    configured_ = true;

    const ShaderColour sc = forShader(colour);

    // The matrix, applied by the sampler. Must be set before the first frame
    // is offered: the conversion object is created on the first import and
    // every cached image view references it.
    renderer.set_external_colour(
        sc.matrix == ShaderMatrix::BT2020NCL
            ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020
            : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        sc.fullRange ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                     : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);

    // The transfer, applied by the fragment stage — a sampler cannot do it.
    // The peak passed here is the DISPLAY's, not the content's: the tone map
    // compresses toward what the panel can show. We do not yet ask Android
    // what that is, so the content's own mastering peak is the stand-in, which
    // is correct whenever the two agree and conservative when they do not.
    renderer.set_external_transfer(
        sc.transfer == ShaderTransfer::PQ ? Renderer::ExternalTransfer::Pq
                                          : Renderer::ExternalTransfer::Sdr,
        sc.masteringPeakNits);
}

void VideoLayer::offer(DecodedFrame frame, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mu_);
    // Release the frame this one replaces, exactly once. Dropping a frame
    // without releasing it removes a buffer from a pool of six, and six such
    // drops stop decode permanently.
    if (held_.release) held_.release();
    held_ = std::move(frame);
    heldGeneration_ = generation;
}

bool VideoLayer::present(Renderer& renderer, const Clock& clock) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!held_.valid()) return false;

    // From before the last seek. The timestamp cannot say so on its own —
    // the new segment's timestamps can overlap the old one's.
    if (heldGeneration_ != clock.generation()) {
        held_.release();
        held_ = DecodedFrame{};
        return false;
    }

    const FrameDecision d = clock.decide(held_.ptsUs);
    if (d.action == FrameAction::Wait) return false;

    if (d.action == FrameAction::Drop) {
        held_.release();
        held_ = DecodedFrame{};
        return false;
    }

    // Present. The renderer takes its own reference on the AHardwareBuffer and
    // calls the callback when it is done with it, so ownership crosses here
    // and this side must not release it as well.
    AHardwareBuffer* hwb = static_cast<AHardwareBuffer*>(held_.handle);
    renderer.update_camera_frame(hwb, std::move(held_.release));
    held_ = DecodedFrame{};
    return true;
}

bool VideoLayer::hasFrame() const {
    std::lock_guard<std::mutex> lock(mu_);
    return held_.valid();
}

}  // namespace vp
