#include "player_view.hh"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/audio_clock.h"

#include "canvas.hh"
#include "keys.hh"
#include "renderer.hh"

#if defined(__ANDROID__)
#include <android/log.h>
#include <dlfcn.h>
#include "audio/flac_output.hh"
#include "codec/mediacodec_video.hh"
#include "fd_stream.hh"
#include "launch_intent.hh"   // app_shell
#include "os/android_host.hh"  // app_shell: androidApp()
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "video_player", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "video_player", __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#define LOGE(...) do {} while (0)
#endif

namespace vp {
namespace {

// How far ahead of the audio clock the feed may read, expressed in FRAMES.
//
// The pipeline has to be throttled somewhere, and it was two seconds of time:
// the decoder duly decoded everything it was given, the presentation queue
// could not hold it, and the excess was discarded in runs that punched holes
// in the timeline. A decoder producing a steady 30 fps put 12 on the screen.
//
// The unit matters. A duration is a different number of frames at every frame
// rate — 300 ms is 9 frames at 30 fps and 36 at 120 — so a constant tuned
// against a 30 fps file silently drops frames on a 60 fps one. Counting frames
// makes the lead scale with the content, and VideoLayer's queue only has to be
// deeper than this number, whatever the file's rate.
//
// Gating on queue depth directly was tried and measured WORSE (28 fps, 80 ms
// jitter, against 30 fps and 42 ms): the feed holds one packet, so a full
// video queue stalls audio too, and audio is the clock.
constexpr int     kFeedAheadFrames = 8;
// Until two video packets have been seen there is nothing to derive a frame
// period from. 33 ms is a 30 fps guess, used for the first few packets.
constexpr int64_t kAssumedFrameUs  = 33'333;

// ── The prebuffer ──────────────────────────────────────────────────────────
//
// How many decoded frames must be waiting before playback starts.
//
// Nothing waited before. openFile() called play() and started audio in the
// same breath as opening the file, so the timeline began advancing while the
// decoder was still bringing up the codec and decoding the first keyframe.
// Measured on the phone: the decoder's worst output gap in the first second
// was 2.87 s, and the first frames to arrive were already late by everything
// that had elapsed. The opening seconds of every file were spent catching up,
// which is exactly when someone is deciding whether the player is any good.
//
// In FRAMES, like every other quantity here, so the cushion is the same
// fraction of a second at every rate: 6 is ~200 ms at 30 fps and ~50 ms at
// 120. Half of VideoLayer's 12 slots, leaving as much headroom above the
// steady state as the cushion below it.
constexpr size_t  kPrimeFrames     = 6;
// How far the feed reads ahead WHILE priming. The normal lead is measured from
// the clock, and the clock sits at zero until playback starts, so the usual 8
// frames is the whole budget the prime gets — barely more than the 6 it needs.
// 10 gives it room. It must stay under VideoLayer's 12 slots: a frame refused
// by a full queue is discarded for good, and one discarded during the prime is
// a hole in the first second of the film.
constexpr int     kPrimeAheadFrames = 10;
// Enough frames is the normal answer. These two are the answers for when it
// never comes: a clip too short to hold six frames, and a decoder that has
// genuinely stalled. Playing badly beats not playing.
constexpr int64_t kPrimePatienceUs = 2'000'000;
constexpr int64_t kPrimeLimitUs    = 5'000'000;

}  // namespace

struct PlayerWindow::Impl {
    std::unique_ptr<Host>     host;
    std::unique_ptr<Renderer> renderer;

    Player     player;
    VideoLayer video;

    // Audio is NOT a Sink: Player's Sink is the video decoder, and the audio
    // path needs two things Sink has no business carrying — a clock and a
    // start/pause. So this side is owned here and fed alongside.
    std::unique_ptr<FlacOutput> audioOwned;
    FlacOutput* audio = nullptr;

    uint64_t videoTrack = 0, audioTrack = 0;
    bool     haveVideo = false, haveAudio = false;

    std::thread       feed;
    std::atomic<bool> running{false};
    std::atomic<bool> feeding{false};
    uint64_t          feedGeneration = 0;

    // Turns the audio device's 20 ms staircase into a line. See
    // core/audio_clock.h — it is the difference between a steady 33.3 ms
    // cadence and one that alternates 40 and 20 around the same average.
    // Render thread only.
    AudioClockInterpolator audioClock;

    // The stream's frame period, measured on the feed thread and read by the
    // render thread. Everything that must scale with the file's frame rate is
    // derived from this one number: the feed's lead, the clock's drop
    // threshold, and the frame rate handed to the display.
    std::atomic<int64_t> framePeriodUs{kAssumedFrameUs};

    // Set while the pipeline fills and playback has not begun. See
    // kPrimeFrames.
    std::atomic<bool>                     priming{false};
    std::chrono::steady_clock::time_point primeStart{};

    std::string status;   // what to say when there is no picture

    // Reused per frame so the draw path allocates nothing.
    std::vector<float> curves, shapes;

    // Exactly one of the two is used: a path when we have one, otherwise a
    // stream the platform opened for us. See how create() chooses.
    bool openFile(const std::string& path, std::unique_ptr<std::istream> stream = nullptr);
    void runFeed();
    void drawFrame();

    // Asks Android to run the panel at the content's rate. See the definition.
    void applyDisplayFrameRate(int64_t periodUs);

    // Starts playback once the pipeline has filled. Called every render loop
    // while `priming`. `force` starts it regardless, which is what a person
    // tapping the screen during the prime means.
    void tryStartAfterPriming(bool force = false);
};

// ── The feed thread ────────────────────────────────────────────────────────
//
// Pulls packets and pushes each into the decoder that wants it. It never DROPS
// one: a compressed packet dropped is everything corrupted up to the next
// keyframe, so a full decoder input queue is retried, not skipped. Each track
// therefore holds ONE packet in hand until it lands.
//
// It stays a bounded distance ahead of what is audible rather than reading as
// fast as storage allows — otherwise a fast device buffers the whole file into
// the decoders and a seek throws all of it away.
void PlayerWindow::Impl::runFeed() {
    Packet pkt;
    bool eof = false;

    // The file's frame period, learned from consecutive video timestamps
    // rather than read from the container: DefaultDuration is optional and
    // often absent, while the timestamps are what playback actually follows.
    // The smallest positive gap is the frame period — smallest, because a
    // reordered or missing packet only makes a gap LARGER, so the minimum
    // converges on the truth from above.
    int64_t framePeriodUs = kAssumedFrameUs;
    int64_t lastVideoPts  = -1;

    // The SECOND estimate, and a different statistic on purpose.
    //
    // The minimum gap above is what the feed lead needs: conservative, because
    // underestimating the period only makes the lead shorter. It is the wrong
    // answer for "what rate is this file", and the phone said so — a 30 fps
    // recording contains the odd 25 ms gap, so the minimum settled on 25000
    // and the display was asked for 40 fps.
    //
    // The MEAN over everything seen so far is the right statistic for that
    // question: exactly the nominal period for constant-rate content, and the
    // true average rate for variable-rate content, with no single short gap
    // able to move it. Kept as a span and a count rather than a running
    // average so it costs two adds and never accumulates rounding.
    int64_t firstVideoPts = -1;
    int64_t videoFrames   = 0;

    // ── TEMPORARY instrumentation ──────────────────────────────────────────
    // Where does the time actually go? The feed does three things that can
    // each stall — read from storage, wait on the lead gate, wait on a full
    // decoder input — and a 44 ms gap at the far end names none of them.
    auto statWindow = std::chrono::steady_clock::now();
    int64_t readMaxUs = 0, readTotalUs = 0, readCount = 0;
    int64_t gateWaits = 0, submitRefusals = 0, videoSubmitted = 0;

    while (running) {
        if (!feeding || eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        const uint64_t gen = player.clock().generation();
        if (gen != feedGeneration) {
            pkt = Packet{};          // belongs to the segment we just left
            eof = false;
            lastVideoPts = -1;       // the next gap would span the seek
            firstVideoPts = -1;      // and the mean would span it too
            videoFrames = 0;
            feedGeneration = gen;
        }

        const int64_t leadUs =
            framePeriodUs * (priming.load(std::memory_order_relaxed) ? kPrimeAheadFrames
                                                                    : kFeedAheadFrames);

        // ONE packet in hand, read in the order the file stores it, dispatched
        // to whichever decoder it belongs to.
        //
        // This used to pull each track separately, which is a trap: the tracks
        // are interleaved, so reaching the next audio packet means reading and
        // BUFFERING every video packet in between. The moment video ran far
        // enough ahead for the horizon below to stop submitting it, audio kept
        // pulling — and dragged 1.5 MB intra frames into a queue that had no
        // bound, at roughly 40 MB a second, until the process died. That read
        // on screen as playback simply stopping after about nine seconds.
        if (pkt.empty()) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool got = player.demuxer().nextPacket(pkt);
            const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - t0).count();
            readTotalUs += us; ++readCount;
            if (us > readMaxUs) readMaxUs = us;
            if (!got) { eof = true; continue; }
        }

        // Stay a bounded distance ahead of what is audible. player.clock()
        // moves with the audio device, so this throttles to real time and
        // keeps the decoders' input queues — and this thread's memory — flat.
        // Applied to the ONE packet in hand, so waiting here reads nothing
        // further rather than buffering behind the gate.
        if (pkt.ptsUs > player.clock().nowUs() + leadUs) {
            ++gateWaits;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }

        if (pkt.trackNumber == videoTrack && lastVideoPts >= 0) {
            const int64_t gap = pkt.ptsUs - lastVideoPts;
            if (gap > 0 && gap < framePeriodUs) framePeriodUs = gap;
        }
        if (pkt.trackNumber == videoTrack) {
            lastVideoPts = pkt.ptsUs;
            if (firstVideoPts < 0) firstVideoPts = pkt.ptsUs;
            ++videoFrames;
            // Publish the mean once there is enough of the stream behind it to
            // mean something: at least a second of content AND enough frames
            // for the mean to be a mean. Measured with only the frame count,
            // the estimate still crept — 34.3 fps down to 30.3 over nine
            // seconds — because the feed reads ahead in bursts and the first
            // dozen packets are not a second of anything.
            constexpr int64_t kWarmupSpanUs = 1'000'000;
            if (videoFrames > 16 && lastVideoPts - firstVideoPts >= kWarmupSpanUs) {
                const int64_t mean = (lastVideoPts - firstVideoPts) / (videoFrames - 1);
                if (mean > 0) this->framePeriodUs.store(mean, std::memory_order_relaxed);
            }
        }

        bool taken = false;
        if (haveVideo && pkt.trackNumber == videoTrack) {
            Sink* sink = player.sink();
            taken = sink && sink->submit(pkt);
        } else if (haveAudio && audio && pkt.trackNumber == audioTrack) {
            taken = audio->submit(pkt);
        } else {
            // A track we do not decode — a subtitle stream, say. Dropping it
            // is what "taken" means here: it must not stay in hand forever.
            taken = true;
        }

        if (taken && pkt.trackNumber == videoTrack) ++videoSubmitted;
        if (!taken) ++submitRefusals;

        {
            const auto now2 = std::chrono::steady_clock::now();
            if (now2 - statWindow >= std::chrono::seconds(1)) {
                statWindow = now2;
                LOGI("feed: read avg %lld us max %lld us over %lld reads | "
                     "gate waits %lld | submit refusals %lld | video submitted %lld",
                     (long long)(readCount ? readTotalUs / readCount : 0),
                     (long long)readMaxUs, (long long)readCount,
                     (long long)gateWaits, (long long)submitRefusals,
                     (long long)videoSubmitted);
                readMaxUs = readTotalUs = readCount = 0;
                gateWaits = submitRefusals = videoSubmitted = 0;
            }
        }

        if (taken) pkt = Packet{};
        // Not taken means the decoder's input queue is full. Keep the packet
        // and retry: a compressed packet dropped corrupts everything up to the
        // next keyframe.
        else std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

// ── Matching the panel to the file ─────────────────────────────────────────
//
// A 120 Hz phone does not show 30 fps content in even 4-vsync steps just
// because 120 divides by 30. The compositor picks a refresh rate for whatever
// is on screen, and a player that never states its rate gets whatever the
// system guessed — commonly 60 Hz, where a 24 fps film lands on a 3:2 cadence
// and judders, or 120 Hz, where every frame is held for a slightly different
// number of vsyncs.
//
// ANativeWindow_setFrameRate() is how an app says "this is the rate, pick a
// mode that divides it evenly". It is the single largest smoothness win
// available to a video player on a variable-refresh panel, and it costs one
// call. COMPATIBILITY_FIXED_SOURCE is the correct constant: it means the
// content has a fixed rate that should NOT be resampled — as opposed to a
// game, which can be asked to run at whatever the display prefers.
//
// It arrived in API 30 and this app's minimum is 28, so it is resolved at run
// time rather than linked. A device that does not have it plays exactly as it
// did before; nothing about correctness depends on this call succeeding.
void PlayerWindow::Impl::applyDisplayFrameRate(int64_t periodUs) {
#if defined(__ANDROID__)
    if (periodUs <= 0) return;
    android_app* app = static_cast<AndroidHost*>(host.get())->androidApp();
    if (!app || !app->window) return;

    using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
    static SetFrameRate setFrameRate = [] {
        void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libandroid.so", RTLD_NOW);
        return lib ? reinterpret_cast<SetFrameRate>(
                         dlsym(lib, "ANativeWindow_setFrameRate"))
                   : nullptr;
    }();
    if (!setFrameRate) {
        LOGI("display rate: ANativeWindow_setFrameRate unavailable (pre-API-30)");
        return;
    }

    const float fps = 1000000.0f / static_cast<float>(periodUs);
    // ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE == 1. Spelled as a
    // literal because the enum lives in an API-30 header this file may be
    // compiled against an older copy of.
    const int32_t rc = setFrameRate(app->window, fps, /*FIXED_SOURCE=*/1);
    LOGI("display rate: asked for %.3f fps, rc=%d", fps, rc);
#else
    (void)periodUs;
#endif
}

// ── Starting, once there is something to start with ───────────────────────
//
// The condition is "enough decoded frames, and an audio clock that can answer".
// Both halves matter. Frames alone would start the timeline against a
// playedPtsUs() of zero, and every queued frame would be judged late at once.
void PlayerWindow::Impl::tryStartAfterPriming(bool force) {
    if (!priming) return;

    const int64_t waitedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - primeStart).count();
    const size_t  have     = video.queued();
    const bool    audioOk  = !haveAudio || !audio || audio->ready();

    const bool filled   = have >= kPrimeFrames && audioOk;
    // A clip with fewer frames in it than the cushion asks for would otherwise
    // wait forever for a sixth frame that does not exist.
    const bool patience = waitedUs > kPrimePatienceUs && have >= 1 && audioOk;
    // Something is actually wrong. Start anyway and let the rest of the
    // pipeline cope, rather than sitting on a black screen indefinitely.
    const bool giveUp   = waitedUs > kPrimeLimitUs;
    if (!force && !filled && !patience && !giveUp) return;

    priming = false;
    player.play();
    if (audio) audio->start();
    LOGI("prebuffer: %zu frames%s after %lld ms%s", have,
         audioOk ? " + audio" : " (audio not ready)",
         (long long)(waitedUs / 1000),
         force ? " — asked to start" :
         (giveUp && !filled && !patience) ? " — gave up waiting" : "");
}

// ── Opening ────────────────────────────────────────────────────────────────
bool PlayerWindow::Impl::openFile(const std::string& path,
                                 std::unique_ptr<std::istream> stream) {
#if defined(__ANDROID__)
    // The video Sink is what Player owns and drives. Frames come back on the
    // decoder's own thread, tagged with the clock generation current AT THAT
    // MOMENT, so a frame decoded before a seek can be recognised after it.
    auto sink = std::make_unique<MediaCodecVideo>([this](DecodedFrame f) {
        if (!f.valid()) return;              // end-of-stream marker
        video.offer(std::move(f), player.clock().generation());
    });
    MediaCodecVideo* videoSink = sink.get();

    const bool opened = stream ? player.open(std::move(stream), std::move(sink), nullptr)
                               : player.open(path, std::move(sink), nullptr);
    if (!opened) {
        status = player.error();
        // The Sink's own message is the specific one ("no HEVC decoder on this
        // device"); Player's is the generic wrapper. Prefer the specific.
        if (!videoSink->error().empty()) status = videoSink->error();
        LOGE("open failed: %s", status.c_str());
        return false;
    }

    const TrackEntry* v = player.demuxer().videoTrack();
    const TrackEntry* a = player.demuxer().audioTrack();
    haveVideo = v != nullptr;
    haveAudio = false;
    if (v) {
        videoTrack = v->number;
        // Tell the renderer what colour these frames are BEFORE the first one
        // arrives — the conversion object is built on the first import.
        video.configure(*renderer, v->colour, v->rotationDegrees);
        LOGI("video: %ux%u %s rotation=%d", v->width, v->height,
             v->colour.isHdr10() ? "HDR10 (PQ, BT.2020)" : "SDR",
             v->rotationDegrees);
    }
    if (a) {
        audioOwned = std::make_unique<FlacOutput>();
        if (audioOwned->configure(*a)) {
            audio = audioOwned.get();
            audioTrack = a->number;
            haveAudio = true;
            LOGI("audio: FLAC %.0f Hz x %u", a->sampleRate, a->channels);
        } else {
            // Playable without sound is better than not playable. Say so once.
            LOGE("audio disabled: %s", audioOwned->error().c_str());
            audioOwned.reset();
        }
    }

    feedGeneration = player.clock().generation();
    feeding = true;
    feed = std::thread([this] { runFeed(); });
    // Feed, but do NOT play yet. The pipeline fills first; run() starts
    // playback when there is a cushion behind it. See kPrimeFrames.
    priming = true;
    primeStart = std::chrono::steady_clock::now();
    status.clear();
    return true;
#else
    (void)path;
    (void)stream;
    status = "no decoder on this platform";
    return false;
#endif
}

PlayerWindow::PlayerWindow() : impl_(std::make_unique<Impl>()) {}
PlayerWindow::~PlayerWindow() { shutdown(); }

bool PlayerWindow::create(std::unique_ptr<Host> host) {
    impl_->host = std::move(host);
    if (!impl_->host) return false;
    if (!impl_->host->init(this)) return false;

    // Vulkan. Hdr10PQ is REQUESTED; what we actually got is hdrActive(), and
    // the engine logs the full surface-format enumeration either way. A device
    // that falls back to the SDR pin still plays the file — tone-mapped to SDR
    // by the same shader path — it just is not what this project is for.
    try {
        impl_->renderer = std::make_unique<Renderer>(
            impl_->host->surfaceProvider(), impl_->host->assetReader(),
            /*desiredSwapchainImages=*/4, OutputTarget::Hdr10PQ);
    } catch (const std::exception& e) {
        LOGE("Vulkan initialization failed: %s", e.what());
        impl_->host->showErrorMessage("Vulkan initialization failed", e.what());
        return false;
    }
    LOGI("swapchain: hdr=%d target=%d", impl_->renderer->hdrActive() ? 1 : 0,
         static_cast<int>(impl_->renderer->activeTarget()));

    impl_->host->showWindow();
    impl_->running = true;

    openWhateverWeWereLaunchedWith();
    return true;
}

// ── The three ways this app is handed a video ──────────────────────────────
//
//   1. an intent EXTRA — `am start --es video_path /sdcard/...`. How a
//      developer launches it, and the only one that existed at first.
//   2. a file:// data URI — some file managers still send these, and it is
//      what `am start -d file://...` produces. A real path; open it directly.
//   3. a content:// data URI — what a modern file manager actually sends when
//      someone taps a video and picks this app. It names a row in another
//      app's ContentProvider: there is no path behind it, and the read grant
//      belongs to the Intent rather than to us. A descriptor is the only
//      handle that comes back.
//
// The first two are paths and the third is not, which is the whole reason
// Demuxer grew a stream-shaped open().
void PlayerWindow::openWhateverWeWereLaunchedWith() {
#if defined(__ANDROID__)
    android_app* app = static_cast<AndroidHost*>(impl_->host.get())->androidApp();

    const std::string extra = impl_->host->launchArgument();
    // launchArgument() answers with its FALLBACK when the extra is absent,
    // and this app's fallback is a directory. Treating that as a file is what
    // produced "not a Matroska file (no EBML header)" on every launch from a
    // file manager — a true statement about a directory, and a useless one.
    if (!extra.empty() && extra.rfind(".mkv") == extra.size() - 4) {
        LOGI("opening from intent extra: %s", extra.c_str());
        impl_->openFile(extra);
        return;
    }

    const std::string dataPath = intent_data_path(app);
    if (!dataPath.empty()) {
        LOGI("opening from file:// data URI: %s", dataPath.c_str());
        impl_->openFile(dataPath);
        return;
    }

    const int fd = open_intent_data_fd(app);
    if (fd >= 0) {
        LOGI("opening from content:// data URI, fd %d", fd);
        auto stream = streamFromFd(fd);   // takes ownership of fd either way
        if (stream) {
            impl_->openFile(std::string(), std::move(stream));
            return;
        }
        LOGE("the descriptor from the content:// URI is not seekable");
        impl_->status = "this file cannot be read as a stream";
        return;
    }
#endif
    impl_->status = "no file to play";
    LOGI("launched with nothing to open: no video_path extra and no data URI");
}

void PlayerWindow::run() {
    // The wall-clock reference for a file with no audio. Only this thread
    // touches it, and only the freerun branch below reads it.
    auto lastTick = std::chrono::steady_clock::now();
    int64_t appliedPeriodUs = 0;
    uint64_t lastGeneration = impl_->player.clock().generation();

    while (impl_->running) {
        // Always "have work": a playing video needs a frame per vsync. The
        // dirty-flag economy a music player uses does not apply here.
        impl_->host->pump(/*haveWork=*/true);
        if (impl_->host->quitRequested()) break;
        if (!impl_->renderer) continue;

        // Everything that has to match the file's frame rate, applied once the
        // feed has actually measured it and again whenever the estimate
        // improves. Doing it here rather than at open() is what lets a file
        // whose rate the container never states still be played at its rate.
        // Re-applied only on a MEANINGFUL change. The mean moves by a
        // microsecond or two every frame, and acting on that would ask the
        // compositor to reconsider its refresh rate several hundred times a
        // second. 2% is well inside the gap between any two standard rates
        // (24, 25, 30, 50, 60, 120) and well outside the estimator's noise.
        const int64_t periodUs = impl_->framePeriodUs.load(std::memory_order_relaxed);
        const int64_t delta = periodUs > appliedPeriodUs ? periodUs - appliedPeriodUs
                                                         : appliedPeriodUs - periodUs;
        if (periodUs > 0 && delta * 50 > appliedPeriodUs) {
            appliedPeriodUs = periodUs;
            // Half a frame: late by more than that and the frame belongs in
            // the next slot, not this one. The Clock clamps what it accepts.
            impl_->player.clock().setDropThresholdUs(periodUs / 2);
            impl_->applyDisplayFrameRate(periodUs);
        }

        impl_->tryStartAfterPriming();

        // A seek restarts the audio device's position, so no anchor from
        // before it means anything.
        const uint64_t gen = impl_->player.clock().generation();
        if (gen != lastGeneration) {
            lastGeneration = gen;
            impl_->audioClock.reset();
        }

        if (impl_->haveAudio && impl_->audio) {
            // Audio is the master: the timeline IS the sample the speaker is
            // playing now.
            //
            // Through the interpolator, not raw. The device reports its
            // position once per BURST — measured here at exactly 50 times a
            // second, in steps of exactly 20000 us — and stands still in
            // between. Scheduling 33333 us frames against a clock that only
            // exists at multiples of 20000 means no frame can ever come due at
            // its own time: each lands one tread late, so presentations
            // alternate 40 ms and 20 ms around a correct-looking average. That
            // was this player's judder, and it is structural rather than a
            // matter of tuning. core/audio_clock.h has the full measurement.
            const auto nowTp = std::chrono::steady_clock::now();
            const int64_t monoUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                       nowTp.time_since_epoch()).count();
            impl_->player.clock().setAudioClock(
                impl_->audioClock.update(impl_->audio->playedPtsUs(), monoUs));
            lastTick = nowTp;
        } else {
            // No audio track, or audio that failed to open. The comment here
            // used to claim the clock free-ran in this case and nothing ever
            // advanced it: nowUs stayed at 0, so the first frame presented,
            // every later one waited forever, and a silent video was a still
            // image. It free-runs for real now.
            const auto now = std::chrono::steady_clock::now();
            int64_t deltaUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  now - lastTick).count();
            lastTick = now;

            // A backgrounded app, a stalled decoder or a debugger breakpoint
            // all produce one enormous delta, and applying it would jump the
            // timeline past every frame in flight and drop the lot. Cap the
            // step at a few frames: the timeline then runs slow for a moment
            // instead of tearing a hole in the playback.
            const int64_t maxStepUs = (appliedPeriodUs > 0 ? appliedPeriodUs
                                                           : kAssumedFrameUs) * 4;
            if (deltaUs > maxStepUs) deltaUs = maxStepUs;
            if (deltaUs < 0) deltaUs = 0;

            // Not before the first frame exists. Starting the timeline while
            // the first keyframe is still decoding spends that time as
            // playback position, and everything decoded during it is already
            // late by the time it arrives.
            if (impl_->video.hasFrame()) impl_->player.clock().advanceFreerun(deltaUs);
        }

        impl_->video.present(*impl_->renderer, impl_->player.clock());
        impl_->drawFrame();
    }
}

void PlayerWindow::Impl::drawFrame() {
    curves.clear();
    shapes.clear();
    // The picture itself is composited by the renderer from the external image
    // handed to it in present(); this pass is the UI over the top. With no
    // frame yet there is nothing but the clear colour, which is the honest
    // thing to show while the first keyframe decodes.
    renderer->draw(curves, 0, {}, {}, {}, shapes);
}

void PlayerWindow::shutdown() {
    impl_->running = false;
    impl_->feeding = false;
    if (impl_->feed.joinable()) impl_->feed.join();
    impl_->player.close();
    // video_ is not reset: it holds a mutex and is not assignable, and its
    // destructor already releases whatever frame it is holding.
    impl_->audioOwned.reset();
    impl_->audio = nullptr;
    impl_->renderer.reset();
}

void PlayerWindow::onHostResized() {}
void PlayerWindow::onHostLayoutInvalidated() {}

void PlayerWindow::onSurfaceLost() {
    // Android takes the swapchain, every texture and every imported buffer
    // when the user leaves the app. CPU state survives; GPU state does not.
    impl_->renderer.reset();
}

bool PlayerWindow::onSurfaceRecreated() {
    try {
        impl_->renderer = std::make_unique<Renderer>(
            impl_->host->surfaceProvider(), impl_->host->assetReader(), 4,
            OutputTarget::Hdr10PQ);
    } catch (const std::exception& e) {
        LOGE("renderer rebuild failed: %s", e.what());
        return false;
    }
    return true;
}

// Pause, or — while the pipeline is still filling — "start now".
//
// Not togglePause() in that state. Player is Paused until the prime completes,
// so a tap would reach play(), start the clock against an audio device still
// deliberately held silent, and leave nowUs at zero: a still image, from the
// one gesture the person expects to fix it.
void PlayerWindow::togglePlayback() {
    if (impl_->priming) {
        impl_->tryStartAfterPriming(/*force=*/true);
        return;
    }
    impl_->player.togglePause();
}

void PlayerWindow::onKeyDownPortable(int keyCode) {
    if (keyCode == key::Space) togglePlayback();
}

void PlayerWindow::onLButtonUp(int, int) { togglePlayback(); }

}  // namespace vp
