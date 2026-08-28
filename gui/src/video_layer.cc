#include "video_layer.hh"

#include <android/hardware_buffer.h>

#include "codec/hdr_metadata.hh"
#include "renderer.hh"

namespace vp {

VideoLayer::~VideoLayer() {
    std::lock_guard<std::mutex> lock(mu_);
    for (Queued& q : queue_)
        if (q.frame.release) q.frame.release();
}

void VideoLayer::configure(Renderer& renderer, const ColourInfo& colour,
                           int rotationDegrees) {
    if (configured_) return;
    configured_ = true;

    // What the container says, not what the camera path assumed. The renderer
    // defaults to a quarter turn because a camera preview needs one; a file
    // whose Projection element is absent needs none, and passing 0 explicitly
    // is how "the pixels are already upright" gets said out loud.
    renderer.set_external_rotation(rotationDegrees);

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

    // Full: refuse the NEW frame. Not the oldest.
    //
    // Dropping the oldest is the intuitive policy and it is exactly wrong
    // here. The decoder runs ahead of the clock, so the oldest queued frame is
    // the one about to come DUE — evicting it leaves a queue whose front is
    // always in the future, `decide()` says Wait every time, and nothing is
    // ever presented at all. Measured: a full queue of four whose front sat a
    // steady 300 ms ahead of the clock, forever.
    //
    // Refusing instead leaves the queue in order from the frame that is due,
    // and throttles decode for free: the decoder's output pool is six buffers,
    // so once four are held here it stalls on its own, which is precisely the
    // backpressure that should exist.
    if (queue_.size() >= kMaxQueued) {
        if (frame.release) frame.release();
        return;
    }
    queue_.push_back(Queued{std::move(frame), generation});
}

bool VideoLayer::present(Renderer& renderer, const Clock& clock) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t gen = clock.generation();
    // Everything from before the last seek. Timestamps cannot identify these:
    // the new segment's can overlap the old one's.
    while (!queue_.empty() && queue_.front().generation != gen) {
        if (queue_.front().frame.release) queue_.front().frame.release();
        queue_.pop_front();
    }

    // Walk forward while the NEXT frame is also due, so a backlog is skipped
    // in one step rather than played back one frame per vsync.
    DecodedFrame due;
    bool haveDue = false;
    while (!queue_.empty()) {
        const FrameDecision d = clock.decide(queue_.front().frame.ptsUs);
        if (d.action == FrameAction::Wait) break;

        if (haveDue && due.release) due.release();   // superseded before it was shown
        due = std::move(queue_.front().frame);
        haveDue = true;
        queue_.pop_front();

        // Present exactly on time: a frame that is due but whose successor is
        // not yet due is the one that belongs on screen now.
        if (d.action == FrameAction::Present) break;
        // Drop: keep going, the next one may be the current one.
    }

    if (!haveDue) return false;

    // How much of the buffer is picture. DecodedFrame::width/height come from
    // AImage_getWidth/Height, which report the decoder's CROP rectangle; the
    // AHardwareBuffer behind it is allocated aligned up (2040x1530 arrives in
    // a 2048x1536 buffer). Without this the padding is sampled and smeared
    // along the right and bottom edges.
    //
    // Set per frame rather than once: it costs two stores, and a stream whose
    // resolution changes mid-play would otherwise keep cropping to the old one.
    renderer.set_external_visible_size(due.width, due.height);

    // The renderer takes its own reference on the AHardwareBuffer and invokes
    // the callback when it is finished, so ownership crosses here and this
    // side must not release it as well.
    AHardwareBuffer* hwb = static_cast<AHardwareBuffer*>(due.handle);
    renderer.update_camera_frame(hwb, std::move(due.release));
    return true;
}

bool VideoLayer::hasFrame() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !queue_.empty();
}

bool VideoLayer::hasRoom() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size() < kMaxQueued;
}

}  // namespace vp
