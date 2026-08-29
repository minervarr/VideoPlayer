#pragma once

// The ONLY file in this project that names AMediaCodec.
//
// It implements core/'s Sink by handing HEVC access units to the phone's
// hardware decoder and emitting DecodedFrames whose `handle` is an
// AHardwareBuffer. Neither `core/` nor `gui/player_view.cc` knows any of that;
// they see Sink and DecodedFrame. A desktop decoder later implements the same
// Sink, and nothing above this line changes. That is CLAUDE.md rule 2.
//
// ── Why AHardwareBuffer and not a Surface ─────────────────────────────────
//
// The obvious path — configure the codec with the SurfaceView's ANativeWindow
// — puts the frame on screen without us ever touching it, and is exactly wrong
// here: it hands the compositor a picture we never colour-managed. This player
// exists to apply BT.2020 -> display and the PQ transfer OURSELVES, in a
// vk_canvas shader, into an HDR10 swapchain. So the codec decodes into an
// ImageReader, we take the AHardwareBuffer out of it, and vk_canvas imports
// that as a zero-copy Y'CbCr image (ComputeContext::import_ahb /
// Renderer's SamplerYcbcrConversion path, both written for the camera).
//
// ── What is NOT here ──────────────────────────────────────────────────────
//
// No format fallback. If the device has no HEVC Main10 hardware decoder,
// configure() returns false and the player says so. A software fallback would
// be a second decode path with different colour behaviour, on the one axis
// this project refuses to be device-dependent about.

#include <memory>

#include "core/player.h"
#include "core/video_frame.h"

namespace vp {

class MediaCodecVideo : public Sink {
public:
    // `onFrame` is called from the decoder's own output thread. The consumer
    // owns calling DecodedFrame::release() — the codec's output pool is small
    // and fixed, and a frame held past its usefulness stalls decode.
    explicit MediaCodecVideo(Player::FrameReady onFrame);
    ~MediaCodecVideo() override;

    bool configure(const TrackEntry* video, const TrackEntry* audio) override;
    bool submit(const Packet& p) override;
    void flush() override;
    // Idempotent by SUCCESS, not by call: if the decoder's input queue is full
    // right now this does nothing and the next call tries again, so the feed
    // thread can simply keep calling it while it sits at end of file. flush()
    // clears the latch, because a seek means the stream is no longer over.
    void signalEndOfStream() override;

    // Why configure() refused, for the one logcat line that explains a black
    // screen. Empty when it did not.
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
