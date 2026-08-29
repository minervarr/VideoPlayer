#include "audio/flac_output.hh"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>

#include "backends/aaudio/aaudio_sink.h"

#define LOG_TAG "VideoAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vp {
namespace {
constexpr const char* kFlacMime = "audio/flac";

// CLOCK_MONOTONIC, the same base AAudio timestamps are requested against and
// the same one std::chrono::steady_clock reads on this platform.
int64_t monotonicNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
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
    // Whether the consumer has asked for sound yet. The AAudio stream has to
    // be STARTED before write() will accept anything, so the decode thread
    // starts it as soon as it knows the format — long before the player is
    // ready to play. Without this it began making sound at that moment, which
    // is the one thing a prebuffer must not allow: audio is the clock, so
    // audio running is playback running.
    std::atomic<bool> playing{false};
    // Whether the end-of-stream buffer has been ACCEPTED. See
    // FlacOutput::signalEndOfStream().
    std::atomic<bool> eosSent{false};
    // CLOCK_MONOTONIC at the last transport change. A device timestamp taken
    // before it describes a different playback, and must not be carried
    // forward across it. See playedPtsUs().
    std::atomic<int64_t> transportEpochNanos{0};
    // The device's OUTPUT LATENCY, in frames: how far the stream's consumed
    // count runs ahead of what the DAC has actually presented. Measured
    // whenever both numbers are available, and used to correct the fallback
    // when only the consumed count is. Zero until first measured, which is
    // exactly the old behaviour. See playedPtsUs().
    std::atomic<int64_t> latencyFrames{0};

    // The timeline anchor: the presentation timestamp of the FIRST sample
    // written since the last flush, and the device's own count of frames
    // played since then. Everything the video clock needs follows from those
    // two, and neither can go backwards.
    std::atomic<int64_t> basePtsUs{0};
    std::atomic<bool>    haveBase{false};
    std::atomic<int>     rate{0};
    std::atomic<int64_t> framesAtBase{0};

    // The format the sink was last configured with, kept so it can be rebuilt
    // without waiting for another format-changed message that will never come.
    // Decode thread only.
    ae::AudioFormat lastFormat{};

    ~Impl() { stop(); }

    void stop() {
        running = false;
        if (codec) AMediaCodec_stop(codec);
        if (output.joinable()) output.join();
        if (codec) { AMediaCodec_delete(codec); codec = nullptr; }
        sink.stop();
    }

    void drain();
    bool recoverSink();
};

// ── Getting the sound back after the route changes ─────────────────────────
//
// Plugging in headphones, connecting Bluetooth or docking DISCONNECTS an
// AAudio stream rather than reconfiguring it. The stream stays open and
// useless, and because this player's master clock is the audio position, the
// whole thing freezes: the timeline stops advancing and the picture stops with
// it, with nothing in the log to say why.
//
// Rebuilding on the DECODE thread, which is the only thread that writes to the
// sink or configures it. Doing it from the render thread or from AAudio's
// error callback would race a write already in flight — and AAudio forbids
// touching the stream from inside the callback at all.
//
// The anchor is dropped along with the stream. basePtsUs names a sample by the
// frame index it was written at, and a new stream starts counting frames from
// zero, so the old pair means nothing. Clearing haveBase makes the next
// decoded buffer establish a fresh one, which is correct by construction: that
// buffer is the next audio to be heard.
bool FlacOutput::Impl::recoverSink() {
    LOGE("audio device disconnected (route change?) — rebuilding the stream");
    sink.stop();
    if (!lastFormat.valid() || !sink.configure(lastFormat) || !sink.start()) {
        LOGE("could not reopen the audio device at %d Hz x %d ch",
             lastFormat.sampleRate, lastFormat.channels);
        configured = false;
        return false;
    }
    if (!playing.load()) sink.pause();
    haveBase.store(false);
    transportEpochNanos.store(monotonicNanos());
    latencyFrames.store(0);   // a different device has a different latency
    LOGI("audio device rebuilt at %d Hz x %d ch",
         lastFormat.sampleRate, lastFormat.channels);
    return true;
}

void FlacOutput::Impl::drain() {
    while (running) {
        // Before anything is written, and on the thread that owns the sink.
        // A failed rebuild clears `configured`, so the writes below are
        // skipped and the video falls back to the free-running clock rather
        // than freezing on a device that will never answer again.
        if (configured && sink.disconnected() && !recoverSink()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

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
            lastFormat = af;   // so recoverSink() can rebuild from it
            // Started so write() is legal, then held if nobody has asked to
            // play yet. A paused stream still accepts writes until its buffer
            // fills, and AAudioStream_write blocks in 100 ms slices after
            // that, so this thread primes the device buffer and then waits
            // rather than spinning. Priming the speaker's own buffer is part
            // of the point: when playback does start, it starts full.
            if (!playing.load()) sink.pause();
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

// The audio counterpart to MediaCodecVideo::signalEndOfStream(). A FLAC frame
// is a fixed block size and the last one in a file is usually short, so a
// decoder that is never told the input has finished can sit on a partial
// buffer — the final fraction of a second, which is exactly where a fade-out
// lives.
void FlacOutput::signalEndOfStream() {
    if (!impl_->codec || impl_->eosSent.load()) return;

    const ssize_t idx = AMediaCodec_dequeueInputBuffer(impl_->codec, 5000);
    if (idx < 0) return;   // full; the caller calls again.

    const media_status_t st = AMediaCodec_queueInputBuffer(
        impl_->codec, static_cast<size_t>(idx), 0, 0, 0,
        AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    if (st != AMEDIA_OK) {
        LOGE("queueing audio end-of-stream failed: %d", (int)st);
        return;
    }
    impl_->eosSent.store(true);
    LOGI("end of stream signalled to the FLAC decoder");
}

void FlacOutput::flush() {
    if (impl_->codec) AMediaCodec_flush(impl_->codec);
    impl_->sink.flush();
    // The anchor belongs to the segment that was just discarded. The next
    // buffer written establishes a new one.
    impl_->haveBase.store(false);
    // A seek means this stream has an end still to come. Same reasoning as
    // MediaCodecVideo::flush().
    impl_->eosSent.store(false);
    impl_->transportEpochNanos.store(monotonicNanos());
}

void FlacOutput::start() {
    // Set FIRST, so a format-changed message arriving on the decode thread
    // right now does not pause a stream we have just asked to run.
    impl_->playing.store(true);
    // Before the stream resumes, so no timestamp taken during the resume can
    // slip in under an epoch stamped after it.
    impl_->transportEpochNanos.store(monotonicNanos());
    if (impl_->configured) impl_->sink.resume();
}

void FlacOutput::pause() {
    impl_->playing.store(false);
    impl_->transportEpochNanos.store(monotonicNanos());
    if (impl_->configured) impl_->sink.pause();
}

bool FlacOutput::ready() const {
    // The first PCM buffer has reached the sink, so basePtsUs is real and
    // playedPtsUs() will answer with a position rather than with zero. Until
    // then the audio master clock has nothing to say and starting playback
    // against it means scheduling video against 0.
    return impl_->haveBase.load();
}

namespace {
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
    // A timestamp from BEFORE the last pause, resume or flush describes a
    // different playback and must not be carried forward across it.
    //
    // AAudioStream_getTimestamp keeps answering while the stream is paused,
    // with the position and the instant both frozen where the pause left them.
    // The age correction below then measures the whole length of the pause and
    // clamps to its 200 ms ceiling, so the first read after a resume reports
    // the clock 200 ms further on than the speaker actually is — and every
    // frame queued during the pause is judged late at once. Measured on the
    // phone: a four-second pause resumed with 7 frames dropped, late by
    // 120..320 ms, from a queue that had held them perfectly the whole time.
    //
    // Refusing the stale reading falls through to the consumed-frame count
    // below, which does not advance while paused and so resumes exactly where
    // it stopped. The device path resumes on its own the moment AAudio
    // produces a genuinely new timestamp.
    if (impl_->sink.presentedFrames(frames, atNanos) &&
        atNanos >= impl_->transportEpochNanos.load()) {
        // The reading describes a moment already past. Carrying it forward to
        // now is a correction, not smoothing: without it the clock is late by
        // however long ago the device last looked, which varies.
        int64_t ageUs = (monotonicNanos() - atNanos) / 1000;
        if (ageUs < 0) ageUs = 0;                                  // clocks disagreeing
        if (ageUs > kMaxTimestampAgeUs) ageUs = kMaxTimestampAgeUs;  // a stale reading

        // While both numbers are in hand, note how far apart they are. That
        // difference IS the device's output latency — the frames the stream has
        // handed over that the speaker has not reached yet — and it is what
        // makes the fallback below usable rather than merely monotonic.
        const int64_t consumed = impl_->sink.framesPlayed();
        if (consumed > frames) impl_->latencyFrames.store(consumed - frames);

        return base + (frames - f0) * 1000000 / r + ageUs;
    }

    // No usable device timestamp: the first few hundred milliseconds of a
    // stream, or the moment just after a resume, before AAudio has produced a
    // reading newer than the transport change.
    //
    // The consumed count, CORRECTED by the latency measured above. Uncorrected
    // it is systematically ahead of the sound by exactly that latency — which
    // is the whole reason the device path is preferred — and on a resume that
    // error lands on a queue that is already full: every frame held across the
    // pause is judged late at once and dropped. Measured on the phone at
    // 20..140 ms of false lateness, which is four frames' worth at 30 fps.
    //
    // Before the first measurement latencyFrames is 0 and this is exactly the
    // uncorrected formula it replaces. It still only increases, so a clock
    // derived from it still cannot go backwards.
    const int64_t consumed = impl_->sink.framesPlayed();
    int64_t elapsed = consumed - f0 - impl_->latencyFrames.load();
    if (elapsed < 0) elapsed = 0;   // the correction outruns a just-started stream
    return base + elapsed * 1000000 / r;
}

const std::string& FlacOutput::error() const { return impl_->err; }

}  // namespace vp
