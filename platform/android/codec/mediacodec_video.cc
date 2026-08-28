#include "codec/mediacodec_video.hh"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "codec/hdr_metadata.hh"
#include "codec/hevc_annexb.hh"

#define LOG_TAG "VideoCodec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vp {
namespace {

constexpr const char* kHevcMime = "video/hevc";

// How many decoded frames may be checked out at once.
//
// This has to exceed what the CONSUMER holds, or the decoder has nothing left
// to decode into and throughput collapses to the rate frames are returned.
// That is not a theory: at 6, with VideoLayer holding 4 and the renderer
// holding 2 (current plus pending), exactly zero were free and a 30 fps file
// played at 12 — with every other number in the system looking healthy.
//
// 16 leaves ten free with the queue full. The cost is address space for
// buffers the decoder mostly does not use; a 2040x1530 10-bit frame is about
// 6 MB, so the ceiling is ~96 MB of GPU-shared memory and the steady state is
// far below it. Every AImage handed out must still be closed exactly once —
// leak six of them at 6, or sixteen here, and decode stops permanently.
constexpr int kImageReaderMaxImages = 16;

}  // namespace

struct MediaCodecVideo::Impl {
    Player::FrameReady onFrame;
    std::string        err;

    AMediaCodec*  codec  = nullptr;
    AImageReader* reader = nullptr;
    ANativeWindow* window = nullptr;

    ColourInfo colour;
    uint32_t   displayWidth = 0, displayHeight = 0;

    std::thread       output;
    std::atomic<bool> running{false};
    // Bumped by flush(). A frame carrying an older value was decoded before a
    // seek and must never reach the screen — the timestamp alone cannot say
    // so, because the new segment's timestamps can overlap the old one's.
    std::atomic<uint64_t> generation{0};
    // hvcC's NAL length field size, read once at configure. Reused scratch for
    // the Annex B rewrite: at 1.5 MB per access unit, allocating per frame is
    // a real cost.
    int nalLengthSize = 4;
    std::vector<uint8_t> annexb;

    ~Impl() { stop(); }

    void stop() {
        running = false;
        if (codec) AMediaCodec_stop(codec);
        if (output.joinable()) output.join();
        if (codec)  { AMediaCodec_delete(codec); codec = nullptr; }
        if (reader) {
            // Clear the listener FIRST: AImageReader_delete does not wait for
            // an in-flight callback, and one firing into a half-destroyed Impl
            // is a crash on exit that only happens sometimes.
            AImageReader_setImageListener(reader, nullptr);
            AImageReader_delete(reader);
            reader = nullptr;
        }
        // The window belongs to the reader and dies with it.
        window = nullptr;
    }

    void drainOutput();
    void onImageAvailable();
    void emit(AImage* image, int64_t ptsUs, uint64_t gen);
};

// ── The output thread ──────────────────────────────────────────────────────
//
// A thread rather than polling from the render loop, because
// AMediaCodec_dequeueOutputBuffer with a zero timeout in a loop that also
// presents means the decoder only ever runs as fast as the display does — and
// on a seek, where several frames must be decoded and discarded to reach the
// target, that is the difference between an instant seek and a visible one.
void MediaCodecVideo::Impl::drainOutput() {
    while (running) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (idx >= 0) {
            const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            // `true` renders into the ImageReader's surface. The AImage does
            // NOT appear there synchronously — it arrives on the reader's own
            // callback (onImageAvailable below), which is why nothing is
            // acquired here. Acquiring right after this call raced the reader
            // and returned NO_BUFFER_AVAILABLE for most frames.
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), info.size > 0);
            if (eos && onFrame) onFrame(endOfStream(info.presentationTimeUs));
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec);
            LOGI("output format: %s", AMediaFormat_toString(fmt));
            AMediaFormat_delete(fmt);
        }
        // AMEDIACODEC_INFO_TRY_AGAIN_LATER is the normal idle case.
    }
}

// The reader's callback, on its own thread. This is where a decoded picture
// actually becomes available.
//
// The timestamp comes from the IMAGE, not from the codec's buffer info, and it
// has to: this callback is decoupled from drainOutput() above, so there is no
// buffer info in scope to read and no guarantee the two are in step. The codec
// propagates the presentation timestamp it was queued with into the image, in
// nanoseconds.
void MediaCodecVideo::Impl::onImageAvailable() {
    AImage* image = nullptr;
    while (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK && image) {
        int64_t tsNs = 0;
        AImage_getTimestamp(image, &tsNs);
        emit(image, tsNs / 1000, generation.load());
        image = nullptr;
    }
}

void MediaCodecVideo::Impl::emit(AImage* image, int64_t ptsUs, uint64_t gen) {
    AHardwareBuffer* hwb = nullptr;
    if (AImage_getHardwareBuffer(image, &hwb) != AMEDIA_OK || !hwb) {
        AImage_delete(image);
        return;
    }

    // A frame from before the last flush(). Drop it here rather than let the
    // renderer decide: the clock's generation check is about frames in flight
    // ACROSS the seam, and this one never needs to cross it.
    if (gen != generation.load()) {
        AImage_delete(image);
        return;
    }

    int32_t w = 0, h = 0;
    AImage_getWidth(image, &w);
    AImage_getHeight(image, &h);

    DecodedFrame f;
    f.handle = hwb;
    f.ptsUs  = ptsUs;
    f.width  = static_cast<uint32_t>(w);
    f.height = static_cast<uint32_t>(h);
    f.displayWidth  = displayWidth;
    f.displayHeight = displayHeight;
    f.colour = colour;
    // Closing the AImage is what returns the buffer to the decoder's pool.
    // Exactly one call, from whoever finishes with the frame — the pool holds
    // kImageReaderMaxImages and decode stops dead when they are all out.
    f.release = [image]() { AImage_delete(image); };

    if (onFrame) onFrame(std::move(f));
    else AImage_delete(image);
}

MediaCodecVideo::MediaCodecVideo(Player::FrameReady onFrame)
    : impl_(std::make_unique<Impl>()) {
    impl_->onFrame = std::move(onFrame);
}

MediaCodecVideo::~MediaCodecVideo() = default;

bool MediaCodecVideo::configure(const TrackEntry* video, const TrackEntry* /*audio*/) {
    if (!video) {
        impl_->err = "this file has no video track we can decode";
        return false;
    }
    if (video->codec != Codec::HEVC) {
        impl_->err = "video track is " + video->codecId + ", not HEVC";
        return false;
    }

    impl_->colour = video->colour;
    impl_->displayWidth  = video->displayWidth;
    impl_->displayHeight = video->displayHeight;

    // ── The ImageReader the codec decodes into ─────────────────────────────
    //
    // PRIVATE format, not YUV_420_888: private is what lets the decoder pick
    // its own layout (P010 for 10-bit) and keeps the buffer on the GPU, which
    // is the whole reason for going this way instead of onto a Surface.
    // GPU_SAMPLED_IMAGE is what makes the AHardwareBuffer importable into
    // Vulkan on the other side.
    media_status_t st = AImageReader_newWithUsage(
        static_cast<int32_t>(video->width), static_cast<int32_t>(video->height),
        AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        kImageReaderMaxImages, &impl_->reader);
    if (st != AMEDIA_OK || !impl_->reader) {
        impl_->err = "AImageReader_newWithUsage failed";
        return false;
    }
    if (AImageReader_getWindow(impl_->reader, &impl_->window) != AMEDIA_OK) {
        impl_->err = "AImageReader_getWindow failed";
        return false;
    }
    AImageReader_ImageListener listener{
        impl_.get(),
        [](void* ctx, AImageReader*) { static_cast<Impl*>(ctx)->onImageAvailable(); }};
    AImageReader_setImageListener(impl_->reader, &listener);

    // ── The codec ──────────────────────────────────────────────────────────
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, kHevcMime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,  static_cast<int32_t>(video->width));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(video->height));
    // csd-0 as Annex B, converted from the container's hvcC.
    //
    // NOT verbatim, which is what this code did at first and what a comment
    // here confidently claimed was correct. Matroska stores HEVC the way MP4
    // does — length-prefixed NALs, parameter sets in an hvcC record — and
    // AMediaCodec wants the Annex B byte stream. Handing it hvcC is not an
    // error it reports: it accepts every input buffer and never emits an
    // output buffer.
    std::vector<uint8_t> csd;
    if (!video->codecPrivate.empty()) {
        impl_->nalLengthSize = hvccLengthSize(video->codecPrivate);
        csd = hvccToAnnexB(video->codecPrivate);
        if (impl_->nalLengthSize == 0 || csd.empty()) {
            AMediaFormat_delete(fmt);
            impl_->err = "the video track's CodecPrivate is not a readable hvcC record";
            return false;
        }
        AMediaFormat_setBuffer(fmt, "csd-0", csd.data(), csd.size());
        LOGI("hvcC: %zu bytes -> %zu bytes Annex B, NAL length size %d",
             video->codecPrivate.size(), csd.size(), impl_->nalLengthSize);
    }
    // What the container said about colour, so the decoder and any downstream
    // processing leave it alone. Never a guess — see hdr_metadata.hh.
    applyToFormat(video->colour, fmt);

    impl_->codec = AMediaCodec_createDecoderByType(kHevcMime);
    if (!impl_->codec) {
        AMediaFormat_delete(fmt);
        impl_->err = "no HEVC decoder on this device";
        return false;
    }

    st = AMediaCodec_configure(impl_->codec, fmt, impl_->window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        // No software fallback, on purpose: a second decode path would have
        // different colour behaviour on the one axis this project refuses to
        // be device-dependent about.
        impl_->err = "this device's HEVC decoder refused Main10 " +
                     std::to_string(video->width) + "x" + std::to_string(video->height);
        return false;
    }
    if (AMediaCodec_start(impl_->codec) != AMEDIA_OK) {
        impl_->err = "AMediaCodec_start failed";
        return false;
    }

    LOGI("HEVC decoder started: %ux%u, %d-bit, transfer %d primaries %d",
         video->width, video->height, video->colour.bitDepth,
         static_cast<int>(video->colour.transfer),
         static_cast<int>(video->colour.primaries));

    impl_->running = true;
    impl_->output = std::thread([this] { impl_->drainOutput(); });
    return true;
}

bool MediaCodecVideo::submit(const Packet& p) {
    if (!impl_->codec) return false;

    // A short timeout, not zero and not infinite: zero makes a full input
    // queue look like an error to the caller, and infinite deadlocks the
    // demux thread when the decoder is stopped underneath it.
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(impl_->codec, 5000);
    if (idx < 0) return false;   // full; the caller retries. Never drop a
                                 // compressed packet — everything up to the
                                 // next keyframe depends on it.

    // Same conversion as csd-0 above, per access unit.
    if (!annexBFromLengthPrefixed(p.bytes.data(), p.bytes.size(),
                                  impl_->nalLengthSize, impl_->annexb)) {
        // A malformed access unit is dropped rather than passed on — a NAL
        // boundary in the wrong place desynchronises everything after it. This
        // returns TRUE so the feed thread advances: retrying a packet that can
        // never be converted would wedge playback forever.
        LOGE("dropping unconvertible access unit at pts %lld (%zu bytes)",
             (long long)p.ptsUs, p.bytes.size());
        AMediaCodec_queueInputBuffer(impl_->codec, static_cast<size_t>(idx), 0, 0, 0, 0);
        return true;
    }

    size_t capacity = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(impl_->codec, static_cast<size_t>(idx), &capacity);
    if (!buf || capacity < impl_->annexb.size()) {
        LOGE("input buffer too small: need %zu, have %zu", impl_->annexb.size(), capacity);
        AMediaCodec_queueInputBuffer(impl_->codec, static_cast<size_t>(idx), 0, 0, 0, 0);
        return false;
    }
    std::memcpy(buf, impl_->annexb.data(), impl_->annexb.size());
    AMediaCodec_queueInputBuffer(impl_->codec, static_cast<size_t>(idx), 0, impl_->annexb.size(),
                                 static_cast<uint64_t>(p.ptsUs),
                                 p.keyframe ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0);
    return true;
}

void MediaCodecVideo::flush() {
    if (!impl_->codec) return;
    // Bump BEFORE flushing, so a frame already dequeued on the output thread
    // and racing toward emit() carries the old generation and is dropped.
    impl_->generation.fetch_add(1);
    AMediaCodec_flush(impl_->codec);
}

const std::string& MediaCodecVideo::error() const { return impl_->err; }

}  // namespace vp
