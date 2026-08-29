#include "video_layer.hh"

#include <iterator>   // std::prev, for offer()'s ordered insert

#include "core/shader_colour.h"
#include "log.hh"          // vk_canvas: VCE_LOGI, whichever host this is
#include "renderer.hh"

#if VP_STATS
#define LOGI(...) VCE_LOGI("video_player", __VA_ARGS__)
#endif

namespace vp {

VideoLayer::~VideoLayer() {
    std::lock_guard<std::mutex> lock(mu_);
    for (Queued& q : queue_)
        if (q.frame.release) q.frame.release();
}

void VideoLayer::configure(Renderer& renderer, const ColourInfo& colour,
                           int rotationDegrees, float displayPeakNits,
                           float pixelAspect) {
    if (configured_) return;
    configured_ = true;

    // What the container says, not what the camera path assumed. The renderer
    // defaults to a quarter turn because a camera preview needs one; a file
    // whose Projection element is absent needs none, and passing 0 explicitly
    // is how "the pixels are already upright" gets said out loud.
    renderer.set_external_rotation(rotationDegrees);

    // The shape the container asks for, which is not always the shape it
    // stored. DisplayWidth/DisplayHeight were parsed and unused for a long
    // time, so a file with non-square pixels was drawn at its pixel aspect —
    // the wrong shape, in a way that reads as a bad encode.
    renderer.set_external_pixel_aspect(pixelAspect);

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
    //
    // The peak passed here is the DISPLAY's: the tone map compresses toward
    // what the panel can actually show, and everything above that is clipped
    // by the panel rather than rolled off by us — which is exactly how
    // specular highlights turn into flat white blobs.
    //
    // The content's own mastering peak was the stand-in for a long time, and
    // it is correct whenever the two agree. They do not agree on a 4000-nit
    // master shown on a 1000-nit phone: the knee lands at 3000, almost nothing
    // is compressed, and the top three quarters of the highlight range is
    // clipped flat by the display.
    //
    // Only ever TIGHTENS. A display that will not say (or an SDR swapchain,
    // where the previously-tested behaviour is the one to keep) leaves the
    // content's peak in place, so the worst case of asking is that nothing
    // changes.
    float peakNits = sc.masteringPeakNits;
    if (displayPeakNits > 0.0f && displayPeakNits < peakNits)
        peakNits = displayPeakNits;

    renderer.set_external_transfer(
        sc.transfer == ShaderTransfer::PQ ? Renderer::ExternalTransfer::Pq
                                          : Renderer::ExternalTransfer::Sdr,
        peakNits);
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
        VP_STAT(++refused_);
        if (frame.release) frame.release();
        return;
    }

    // Inserted in TIMESTAMP order, scanning back from the end.
    //
    // present() walks this queue forward and stops at the first frame that is
    // not yet due, which is only correct if the queue is sorted. It is sorted
    // today by luck rather than by construction: AMediaCodec emits in DISPLAY
    // order, having done the reordering itself, so appending happened to be
    // right. Nothing here said so, and nothing enforced it — a decoder that
    // emitted in decode order would have produced a queue whose front is a
    // future frame, and present() would have sat on Wait forever with a full
    // queue behind it. That failure has been seen once already in this file,
    // from the opposite cause, and it looks like a frozen picture.
    //
    // Nearly always a single comparison: the common case is a frame newer than
    // everything queued, which stops the loop immediately.
    //
    // The scan stops at a GENERATION boundary as well. Timestamps either side
    // of a seek are unrelated — the new segment's can overlap the old one's,
    // which is why generations exist at all — so ordering across one would be
    // arithmetic on two different timelines. present() drops the stale ones
    // from the front before it looks at anything.
    auto pos = queue_.end();
    while (pos != queue_.begin()) {
        const auto prev = std::prev(pos);
        if (prev->generation != generation || prev->frame.ptsUs <= frame.ptsUs) break;
        pos = prev;
    }
    queue_.insert(pos, Queued{std::move(frame), generation});
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
        // HOW late? A frame a few ms past the threshold means the threshold is
        // too tight for the decoder's jitter and frames we had time to show
        // are being discarded. A frame 100 ms late means something actually
        // stalled. The two have opposite fixes, so the range is worth keeping.
        VP_STAT(++dropped_);
        VP_STAT(if (-d.errorUs > dropWorstUs_) dropWorstUs_ = -d.errorUs);
        VP_STAT(if (dropBestUs_ == 0 || -d.errorUs < dropBestUs_) dropBestUs_ = -d.errorUs);
        // Drop: keep going, the next one may be the current one.
    }

#if VP_STATS
    // The one measurement that describes the whole pipeline: how evenly frames
    // actually reach the screen. The queue holds up to twelve, so a decoder
    // hiccup should be invisible here — the front frame comes due on the
    // clock's schedule regardless of when it arrived. Gaps that track the
    // decoder's anyway mean the queue is not doing its job, and gaps that
    // alternate around a correct-looking average mean the CLOCK is quantized
    // (which is what core/audio_clock.h was written for).
    {
        const auto now = std::chrono::steady_clock::now();
        if (haveDue) {
            if (lastPresentTp_.time_since_epoch().count() != 0) {
                const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                                       now - lastPresentTp_).count();
                if (us > presentMaxUs_) presentMaxUs_ = us;
                presentTotalUs_ += us; ++presentCount_;
            }
            lastPresentTp_ = now;
        }
        if (now - presentWindow_ >= std::chrono::seconds(1)) {
            presentWindow_ = now;
            LOGI("present: %lld shown, avg %lld us, WORST %lld us | dropped %lld"
                 " | depth %zu | refused %lld | dropped-late-by %lld..%lld us"
                 " | lead %lld us",
                 (long long)presentCount_,
                 (long long)(presentCount_ ? presentTotalUs_ / presentCount_ : 0),
                 (long long)presentMaxUs_, (long long)dropped_, queue_.size(),
                 (long long)refused_, (long long)dropBestUs_, (long long)dropWorstUs_,
                 (long long)clock.presentationLeadUs());
            presentCount_ = presentTotalUs_ = presentMaxUs_ = dropped_ = refused_ = 0;
            dropWorstUs_ = dropBestUs_ = 0;
        }
    }
#endif

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

    // The renderer takes its own reference on the platform buffer and invokes
    // the callback when it is finished, so ownership crosses here and this
    // side must not release it as well.
    //
    // update_external_frame(), not update_camera_frame(): this file used to
    // name AHardwareBuffer, which is the one Android type in all of gui/ and
    // the reason gui/ could not compile for a second host at all. What the
    // handle actually is stays the engine's business — an AHardwareBuffer
    // here, a VkImage from a desktop decoder — which is exactly what
    // core/video_frame.h's void* was always for.
    renderer.update_external_frame(due.handle, std::move(due.release));
    return true;
}

bool VideoLayer::hasFrame() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !queue_.empty();
}

size_t VideoLayer::queued() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

bool VideoLayer::hasRoom() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size() < kMaxQueued;
}

}  // namespace vp
