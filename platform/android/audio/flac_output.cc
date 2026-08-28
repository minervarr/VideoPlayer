#include "audio/flac_output.hh"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "backends/aaudio/aaudio_sink.h"

#define LOG_TAG "VideoAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vp {
namespace {
constexpr const char* kFlacMime = "audio/flac";
}

// ── Why AMediaCodec here, and not audio_engine's FlacDecoder ───────────────
//
// audio_engine decodes FLAC with its own vendored libFLAC on every platform,
// deliberately, so a music player sounds the same on any handset. That
// decoder's entry point is open(fd, offset, length): it decodes a FLAC FILE,
// reading through libFLAC's stream callbacks over a byte region.
//
// Matroska does not store a FLAC file. It stores raw FLAC FRAMES in Blocks,
// with STREAMINFO held separately in CodecPrivate — there is no contiguous
// region to point an fd at, and synthesising one would mean re-muxing a FLAC
// stream in memory just to hand it back to a decoder.
//
// AMediaCodec's input model is exactly the container's: csd-0 is STREAMINFO,
// each input buffer is one frame. So audio decode goes through the platform
// and audio OUTPUT still goes through audio_engine's AAudioSink, which is the
// half that carries the property the video path actually needs —
// pendingPlaybackMs(), the only honest way to know what the speaker is
// playing right now.
//
// This is a deviation from what CLAUDE.md's table says, and it is written down
// there too. Revisit if audio_engine ever grows a packet-fed FLAC entry point.

struct FlacOutput::Impl {
    AMediaCodec*  codec = nullptr;
    ae::AAudioSink sink;
    std::string   err;

    std::thread       output;
    std::atomic<bool> running{false};
    std::atomic<bool> configured{false};

    // The timeline anchor: the presentation timestamp of the FIRST sample
    // written since the last flush, and the device's own count of frames
    // played since then. Everything the video clock needs follows from those
    // two, and neither can go backwards.
    std::atomic<int64_t> basePtsUs{0};
    std::atomic<bool>    haveBase{false};
    std::atomic<int>     rate{0};
    std::atomic<int64_t> framesAtBase{0};

    ~Impl() { stop(); }

    void stop() {
        running = false;
        if (codec) AMediaCodec_stop(codec);
        if (output.joinable()) output.join();
        if (codec) { AMediaCodec_delete(codec); codec = nullptr; }
        sink.stop();
    }

    void drain();
};

void FlacOutput::Impl::drain() {
    while (running) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (idx >= 0) {
            // A CODEC_CONFIG buffer is not audio. It carries decoder setup
            // bytes, and writing them to the speaker is exactly what they
            // sound like. Nothing flags this as an error; you just hear it.
            const bool isConfig = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;

            size_t sz = 0;
            uint8_t* pcm = isConfig ? nullptr
                                    : AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(idx), &sz);
            // `configured` is not a nicety either: output can arrive before
            // the format-changed message that tells us the real sample rate
            // and channel count, and PCM written into a sink that has not been
            // told either is reinterpreted at whatever stride it defaulted to.
            if (pcm && info.size > 0 && configured) {
                // Blocking write, in a LOOP. write() returns the bytes it
                // actually consumed, and it can consume fewer than it was
                // given: AAudioStream_write is capped at 100 ms so that stop()
                // stays responsive, and a full device buffer makes it return
                // short. Ignoring that return dropped the tail of every short
                // write — which is not a dropout you hear as silence, it is a
                // discontinuity in the middle of a waveform, and it sounds
                // like noise.
                //
                // The loop is also what makes the sink the PACING: this thread
                // runs exactly as fast as the speaker consumes, which is what
                // makes writtenPtsUs a timestamp rather than a measure of how
                // fast the decoder happens to be.
                int written = 0;
                while (written < info.size && running) {
                    const int n = sink.write(pcm + info.offset + written, info.size - written);
                    if (n < 0) { LOGE("AAudioSink write failed"); break; }
                    if (n == 0) continue;   // device full; the next call blocks
                    written += n;
                }
                if (!haveBase.load()) {
                    basePtsUs.store(info.presentationTimeUs);
                    framesAtBase.store(sink.framesPlayed());
                    haveBase.store(true);
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), false);
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // The REAL output format, which is the one to configure the sink
            // against: a 24-bit FLAC can come back as 16-bit or 32-bit
            // depending on the device, and configuring from the container's
            // BitsPerSample would be a guess that plays as noise.
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec);
            int32_t rate_ = 0, channels = 0;
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate_);
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
            LOGI("audio output format: %s", AMediaFormat_toString(fmt));
            AMediaFormat_delete(fmt);

            ae::AudioFormat af{};
            af.sampleRate = rate_;
            af.channels   = channels;
            // AMediaCodec's FLAC decoder emits interleaved PCM16, whatever the
            // file's own bit depth was. subslotBytes is what the sink actually
            // strides by, so both fields have to say 16-bit or write() reads
            // the buffer at the wrong pitch and plays noise.
            af.bitDepth     = 16;
            af.subslotBytes = 2;
            if (!sink.configure(af) || !sink.start()) {
                LOGE("AAudioSink refused %d Hz x %d ch", rate_, channels);
                running = false;
                return;
            }
            rate.store(rate_);
            configured = true;
        }
    }
}

FlacOutput::FlacOutput() : impl_(std::make_unique<Impl>()) {}
FlacOutput::~FlacOutput() = default;

bool FlacOutput::configure(const TrackEntry& audio) {
    if (audio.codec != Codec::FLAC) {
        impl_->err = "audio track is " + audio.codecId + ", not FLAC";
        return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, kFlacMime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE,
                          static_cast<int32_t>(audio.sampleRate));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                          static_cast<int32_t>(audio.channels));
    // CodecPrivate for A_FLAC is the STREAMINFO block, which is exactly what
    // csd-0 wants.
    if (!audio.codecPrivate.empty())
        AMediaFormat_setBuffer(fmt, "csd-0",
                               const_cast<uint8_t*>(audio.codecPrivate.data()),
                               audio.codecPrivate.size());

    impl_->codec = AMediaCodec_createDecoderByType(kFlacMime);
    if (!impl_->codec) {
        AMediaFormat_delete(fmt);
        impl_->err = "no FLAC decoder on this device";
        return false;
    }
    const media_status_t st = AMediaCodec_configure(impl_->codec, fmt, nullptr, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        impl_->err = "the FLAC decoder refused this track";
        return false;
    }
    if (AMediaCodec_start(impl_->codec) != AMEDIA_OK) {
        impl_->err = "AMediaCodec_start failed for audio";
        return false;
    }

    impl_->err.clear();
    impl_->running = true;
    impl_->output = std::thread([this] { impl_->drain(); });
    return true;
}

bool FlacOutput::submit(const Packet& p) {
    if (!impl_->codec) return false;
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(impl_->codec, 5000);
    if (idx < 0) return false;

    size_t capacity = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(impl_->codec, static_cast<size_t>(idx), &capacity);
    if (!buf || capacity < p.bytes.size()) {
        AMediaCodec_queueInputBuffer(impl_->codec, static_cast<size_t>(idx), 0, 0, 0, 0);
        return false;
    }
    std::memcpy(buf, p.bytes.data(), p.bytes.size());
    AMediaCodec_queueInputBuffer(impl_->codec, static_cast<size_t>(idx), 0, p.bytes.size(),
                                 static_cast<uint64_t>(p.ptsUs), 0);
    return true;
}

void FlacOutput::flush() {
    if (impl_->codec) AMediaCodec_flush(impl_->codec);
    impl_->sink.flush();
    // The anchor belongs to the segment that was just discarded. The next
    // buffer written establishes a new one.
    impl_->haveBase.store(false);
}

void FlacOutput::start() { if (impl_->configured) impl_->sink.resume(); }
void FlacOutput::pause() { if (impl_->configured) impl_->sink.pause(); }

int64_t FlacOutput::playedPtsUs() const {
    // Derived from the DEVICE's own count of frames played, anchored to the
    // timestamp of the first sample written after the last flush.
    //
    // The obvious formula — last written timestamp, minus what the device
    // still holds — is what this did first, and it is wrong in a way that only
    // shows up in motion. The two terms are sampled on different threads at
    // different instants: between reading the anchor and reading the pending
    // count, the audio thread writes more, so the pending count includes audio
    // NEWER than the anchor it is subtracted from. The result went backwards
    // by tens of milliseconds, and since video is scheduled against it, the
    // frame scheduler's horizon collapsed, feeding stopped, and playback
    // froze for about a second at a time before bursting to catch up.
    //
    // framesPlayed() only increases, so this cannot.
    if (!impl_->haveBase.load()) return 0;
    const int r = impl_->rate.load();
    if (r <= 0) return impl_->basePtsUs.load();
    const int64_t played = impl_->sink.framesPlayed() - impl_->framesAtBase.load();
    if (played <= 0) return impl_->basePtsUs.load();
    return impl_->basePtsUs.load() + played * 1000000 / r;
}

const std::string& FlacOutput::error() const { return impl_->err; }

}  // namespace vp
