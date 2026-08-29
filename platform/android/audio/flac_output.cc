#include "audio/flac_output.hh"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <cstring>
#include <ctime>
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
                // The segment's anchor, taken BEFORE the first write and from
                // the WRITTEN counter.
                //
                // It used to be taken after the write and from framesPlayed().
                // Both halves were wrong. framesPlayed() and the timestamp
                // counter below both count frames since the stream started,
                // and neither says which frame carries which audio; only
                // framesWritten(), read immediately before handing over a
                // buffer, does — that buffer's first sample IS frame
                // framesWritten(). Taken afterwards it is already past the
                // buffer it is supposed to name.
                if (!haveBase.load()) {
                    basePtsUs.store(info.presentationTimeUs);
                    framesAtBase.store(sink.framesWritten());
                    haveBase.store(true);
                }
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

namespace {
// CLOCK_MONOTONIC, the same base AAudio timestamps are requested against and
// the same one std::chrono::steady_clock reads on this platform.
int64_t monotonicNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// How far a device timestamp may be carried forward before it is treated as
// stale rather than as merely recent. Some devices refresh getTimestamp()
// lazily, and extrapolating an old reading indefinitely is how a video clock
// runs off into a file the speaker never reached. A fifth of a second is
// several bursts — far outside normal jitter, far inside a real stall.
constexpr int64_t kMaxTimestampAgeUs = 200000;
}  // namespace

int64_t FlacOutput::playedPtsUs() const {
    // The presentation time of the sample the speaker is playing RIGHT NOW.
    //
    // Anchored: basePtsUs is the timestamp of the first sample written since
    // the last flush, and framesAtBase is the stream frame index that sample
    // was written at. Everything else is counting forward from there.
    //
    // The obvious formula — last written timestamp, minus what the device
    // still holds — is what this did first, and it is wrong in a way that only
    // shows up in motion. The two terms are sampled on different threads at
    // different instants: between reading the anchor and reading the pending
    // count, the audio thread writes more, so the pending count includes audio
    // NEWER than the anchor it is subtracted from. The result went backwards
    // by tens of milliseconds, and since video is scheduled against it, the
    // frame scheduler's horizon collapsed, feeding stopped, and playback froze
    // for about a second at a time before bursting to catch up.
    if (!impl_->haveBase.load()) return 0;
    const int r = impl_->rate.load();
    const int64_t base = impl_->basePtsUs.load();
    if (r <= 0) return base;
    const int64_t f0 = impl_->framesAtBase.load();

    // What the DAC has actually PRESENTED, when the device will say.
    //
    // Preferred over framesPlayed() because that counts frames the stream has
    // consumed from its buffer rather than frames that have reached the
    // speaker, and the difference is the device's output latency. Video
    // scheduled against the consumed count runs that far ahead of its own
    // sound — a constant offset, so it reads as "something is subtly off"
    // rather than as drift.
    int64_t frames = 0, atNanos = 0;
    if (impl_->sink.presentedFrames(frames, atNanos)) {
        // The reading describes a moment already past. Carrying it forward to
        // now is a correction, not smoothing: without it the clock is late by
        // however long ago the device last looked, which varies.
        int64_t ageUs = (monotonicNanos() - atNanos) / 1000;
        if (ageUs < 0) ageUs = 0;                                  // clocks disagreeing
        if (ageUs > kMaxTimestampAgeUs) ageUs = kMaxTimestampAgeUs;  // a stale reading
        return base + (frames - f0) * 1000000 / r + ageUs;
    }

    // No timestamp yet — the first few hundred milliseconds of a stream. The
    // consumed count is the honest fallback, and it only ever increases, so a
    // clock derived from it cannot go backwards.
    return base + (impl_->sink.framesPlayed() - f0) * 1000000 / r;
}

const std::string& FlacOutput::error() const { return impl_->err; }

}  // namespace vp
