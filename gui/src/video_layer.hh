#pragma once

// The only file in gui/ that knows a DecodedFrame is a GPU image.
//
// PlayerWindow draws controls, a seek bar and a title through Canvas like any
// other app. This draws the picture: it takes the opaque handle out of a
// DecodedFrame, asks vk_canvas to import it as a zero-copy Y'CbCr image, and
// runs shaders_src/video_frag.slang over it with the matrix and transfer the
// CONTAINER named.
//
// ── The one-frame rule ────────────────────────────────────────────────────
//
// Exactly one frame is held for presentation at a time, and the previous one's
// release() runs when it is replaced. Queueing more looks like smoother
// playback and is not: the decoder's output pool is small, and a queue here
// just moves the stall from the queue to the codec. core/clock.h already
// decides which single frame is the right one for this vsync.

#include "core/video_frame.h"

class Canvas;
class Renderer;

namespace vp {

class VideoLayer {
public:
    VideoLayer();
    ~VideoLayer();

    // Must be called once with the renderer that will draw. Reads
    // Renderer::hdrActive() to pick the OUTPUT_ENCODE the pipeline is built
    // with — a PQ-encoding shader writing into an SDR sRGB swapchain is a
    // washed-out picture, and that exact mistake is what
    // framework/vk_canvas/USAGE_hdr_output.md was written after.
    bool create(Renderer& renderer);
    void destroy();

    // Takes ownership of `frame`, releasing whichever one it replaces. Safe to
    // call from the decoder thread; the import happens at draw time.
    void present(DecodedFrame frame);

    // Draws the held frame letterboxed into `dstW` x `dstH`, honoring the
    // container's display aspect when it differs from the coded one. Does
    // nothing when no frame has arrived yet — a black rectangle, not an
    // assertion, is the right behaviour before the first keyframe decodes.
    void draw(Canvas& canvas, float dstW, float dstH);

    bool hasFrame() const;

private:
    struct Impl;
    Impl* impl_;   // not unique_ptr: this header is included where <memory> is not
};

}  // namespace vp
