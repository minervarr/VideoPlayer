#include "video_layer.hh"

namespace vp {

// NOT YET IMPLEMENTED — build steps 4-6.
//
// The engine-side prerequisite lands first: vk_canvas's import_ahb() currently
// refuses Y'CbCr and external-format buffers (see its comment at
// framework/vk_canvas/core/compute_context.hh), and a 10-bit P010 video frame
// is exactly that. That work belongs in the engine repo, with its own test —
// CLAUDE.md rule 4 — and this file is what consumes it afterwards.
struct VideoLayer::Impl {
    DecodedFrame held;
};

VideoLayer::VideoLayer() : impl_(new Impl) {}
VideoLayer::~VideoLayer() {
    if (impl_->held.release) impl_->held.release();
    delete impl_;
}

bool VideoLayer::create(Renderer&) { return false; }
void VideoLayer::destroy() {}

void VideoLayer::present(DecodedFrame frame) {
    // Release whichever frame this replaces, exactly once. The decoder's
    // output pool is small and fixed; a leak here is a stall, not a leak.
    if (impl_->held.release) impl_->held.release();
    impl_->held = std::move(frame);
}

void VideoLayer::draw(Canvas&, float, float) {}
bool VideoLayer::hasFrame() const { return impl_->held.valid(); }

}  // namespace vp
