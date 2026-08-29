#include "player_view.hh"

#include <utility>    // std::swap — applyOrientationFor()
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/audio_clock.h"
#include "core/frame_period.h"
#include "core/audio_output.h"

#include "canvas.hh"
#include "keys.hh"
#include "log.hh"        // vk_canvas: logcat on Android, stderr elsewhere
#include "renderer.hh"

// Logging goes through the engine's shim rather than <android/log.h>, so the
// messages below exist on every host. They used to compile to nothing off
// Android, which would have made bringing up a second host an exercise in
// silence — the one situation where the startup trace matters most.
#define LOGI(...) VCE_LOGI("video_player", __VA_ARGS__)
#define LOGE(...) VCE_LOGE("video_player", __VA_ARGS__)

#if defined(__ANDROID__)
#include <dlfcn.h>
#include "activity_bridge.hh"   // app_shell: display_hdr_headroom()
#include "audio/flac_output.hh"
#include "codec/mediacodec_video.hh"
#include "fd_stream.hh"
#include "launch_intent.hh"   // app_shell
#include "os/android_host.hh"  // app_shell: androidApp()
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
// Taken FROM the estimator rather than written again beside it: two copies of
// the same constant is how they end up disagreeing.
constexpr int64_t kAssumedFrameUs  = FramePeriod::kAssumedUs;

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
    //
    // Held as the interface, not as the platform's class. This was a concrete
    // FlacOutput — Android's AMediaCodec plus AAudio — named in a file that is
    // supposed to reach the OS only through Host, and it was the last thing
    // stopping gui/ from compiling for any other host.
    std::unique_ptr<AudioOutput> audioOwned;
    AudioOutput* audio = nullptr;

    uint64_t videoTrack = 0, audioTrack = 0;
    bool     haveVideo = false, haveAudio = false;

    std::thread       feed;
    std::atomic<bool> running{false};
    // Whether the feed THREAD should be alive, as distinct from whether it
    // should currently be reading. runFeed() used to loop on `running`, which
    // is the whole application's flag — so the only way to join the feed was to
    // shut the player down, and any attempt to stop it for a new file
    // deadlocked: `feeding` merely puts the loop to sleep.
    std::atomic<bool> feedRunning{false};
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
    // Whether the value above came from the STREAM or is still the 33 ms
    // guess. The display rate must not be asked for until it is real: the
    // guess pins the panel to 30 Hz, and a 60 fps file then spends its first
    // second showing every other frame — during the prebuffer, which exists to
    // make exactly that second clean. Seen on the device, with no file open at
    // all: "display rate: asked for 30.000 fps" before anything was loaded.
    std::atomic<bool>    framePeriodMeasured{false};

    // Set while the pipeline fills and playback has not begun. See
    // kPrimeFrames.
    std::atomic<bool>                     priming{false};
    std::chrono::steady_clock::time_point primeStart{};

    // The decoder has emitted its end-of-stream marker: everything it will ever
    // produce has been offered to VideoLayer. Written on the decoder's output
    // thread, read by the render loop. Playback is over once this is set AND
    // the queue has drained — the two halves are why core/ cannot decide it
    // (see Player::markEnded).
    std::atomic<bool> sawEndOfStream{false};

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

    // Asks Android for the orientation this video wastes least screen in. See
    // the definition.
    void applyOrientationFor(const TrackEntry& video);

    // Puts the audio device where the state machine now says it should be.
    //
    // Player owns the transport and knows nothing about an audio device — that
    // is rule 1, and it is why Clock::pause() only sets a flag. Something has
    // to carry the decision across, and this is it: called after every
    // transition, from the one place that holds both.
    void syncAudioTransport();

    // Pause because something outside the app took over — the user left, a
    // dialog appeared, the surface went away. Distinct from the user's own
    // pause only in that it never TOGGLES: an interruption arriving while
    // already paused must not start playback.
    void pauseForInterruption();

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

    // The file's frame rate, learned from the video timestamps rather than
    // read from the container: DefaultDuration is optional and often absent,
    // while the timestamps are what playback actually follows.
    //
    // core/frame_period.h, not a few locals here, because the arithmetic has
    // to be right for streams this project has no sample of — anything with
    // B-frames delivers presentation timestamps in decode order, and the
    // estimator this replaces silently collapsed on exactly that. In core/ it
    // is covered by tests/frame_period_test.cc, which writes down the
    // reordered sequences instead of waiting for a file that contains one.
    FramePeriod period;

    while (feedRunning) {
        if (!feeding || eof) {
            // The file has run out, so tell both decoders — every time round,
            // not once. A hardware decoder's input queue can be full at the
            // moment the last packet lands, and both implementations are
            // idempotent by SUCCESS: they do nothing once the end-of-stream
            // buffer has actually been accepted, and retry until it has.
            //
            // Without this the decoder never drains its reorder buffer, so the
            // tail of every file is decoded and silently never shown, and the
            // EOS marker the render loop waits on to reach Ended never comes.
            if (eof && feeding) {
                if (Sink* sink = player.sink()) sink->signalEndOfStream();
                if (haveAudio && audio) audio->signalEndOfStream();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        const uint64_t gen = player.clock().generation();
        if (gen != feedGeneration) {
            pkt.reset();             // belongs to the segment we just left
            eof = false;
            // Everything measured either side of a seek describes a
            // different part of the file. reset() keeps the shortest gap it
            // has already found, for the reason written down beside it: a seek
            // does not make the file a different frame rate.
            period.reset();
            feedGeneration = gen;
        }

        const int64_t leadUs =
            period.shortestUs() * (priming.load(std::memory_order_relaxed)
                                       ? kPrimeAheadFrames : kFeedAheadFrames);

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
        if (pkt.empty() && !player.demuxer().nextPacket(pkt)) {
            eof = true;
            continue;
        }

        // Stay a bounded distance ahead of what is audible. player.clock()
        // moves with the audio device, so this throttles to real time and
        // keeps the decoders' input queues — and this thread's memory — flat.
        // Applied to the ONE packet in hand, so waiting here reads nothing
        // further rather than buffering behind the gate.
        //
        // For VIDEO, measured against the furthest point reached rather than
        // against the packet in hand. With B-frames the two differ: a reordered
        // block carries a timestamp behind blocks already submitted, so gating
        // on it alone asks "is THIS picture due soon" when the question is "how
        // far ahead has this thread read". Identical on a monotonic stream,
        // where the packet in hand always IS the furthest.
        //
        // For EVERY OTHER TRACK, against the packet's own timestamp — and the
        // distinction is not a nicety. Gating audio on the video read-ahead was
        // measured on the phone and is worse than the problem it was meant to
        // fix: once video reaches the horizon the mark stays there, so the gate
        // closes on audio too, and audio is the clock. The prebuffer went from
        // "6 frames + audio after 27 ms" to five frames and the 2-second
        // patience timeout, and steady state from 0 drops to 4-9 a second, each
        // late by about half a frame. One horizon for two tracks couples them;
        // they are interleaved in the file precisely so they need not be.
        //
        // And measured from where the CONTENT starts while priming, not from
        // the clock. The clock reads 0 until playback begins, but a stream's
        // timestamps do not begin at 0 — this phone's recordings start at
        // 171 ms — so a horizon of "0 + ten frames" admits only the frames
        // before 333 ms, which for a file starting at 171 ms is five of them.
        // The prebuffer asks for six, never gets a sixth, and starts on the
        // 2-second patience timeout with a thin queue instead of in 38 ms with
        // a full one. Measured on the phone, both ways.
        //
        // This was previously hidden rather than handled: the render loop
        // assigned the audio device's position into the clock even while
        // paused, and before playback that position is the first buffer's
        // timestamp — so nowUs happened to hold roughly the stream's start.
        // Pausing properly removed the accident, which is the right time to
        // replace it with the thing it was standing in for.
        {
            const bool isVideo = pkt.trackNumber == videoTrack;
            const int64_t horizon = (isVideo && period.furthestUs() > pkt.ptsUs)
                                        ? period.furthestUs() : pkt.ptsUs;
            const int64_t from = (priming.load(std::memory_order_relaxed) &&
                                  period.firstUs() >= 0)
                                     ? period.firstUs()
                                     : player.clock().nowUs();
            if (horizon > from + leadUs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
        }

        if (pkt.trackNumber == videoTrack) {
            period.add(pkt.ptsUs);
            // Publish the MEAN, which is the right statistic for "what rate is
            // this file" and the one the display is told. The shortest gap
            // above is the conservative one, and it stays local to the feed's
            // lead. See core/frame_period.h for why they are different
            // numbers, and what happened when they were not.
            if (period.settled()) {
                framePeriodUs.store(period.meanUs(), std::memory_order_relaxed);
                framePeriodMeasured.store(true, std::memory_order_release);
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

        // reset(), not Packet{}: the buffer goes back to the demuxer's pool on
        // the next nextPacket() rather than back to the allocator.
        if (taken) pkt.reset();
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

// ── Choosing which way up to be ────────────────────────────────────────────
//
// A 4:3 video on a 19.5:9 phone held upright uses about a third of the panel.
// The same file in landscape uses nearly two thirds — 2040x1530 into 1440x3088
// scales by 0.706 and fills 1440x1080; into 3088x1440 it scales by 0.941 and
// fills 1920x1440. Nothing is cropped either way. The bars are simply smaller,
// because the window is a better fit for the picture.
//
// So the orientation is asked for from the CONTENT's shape, not fixed in the
// manifest. A manifest-level `screenOrientation="sensorLandscape"` would be one
// line and would be wrong for a portrait-shot phone video, which would then
// letterbox badly the other way.
//
// The displayed aspect, not the coded one: the same quantity the renderer's
// letterbox computes, which is width scaled by the pixel aspect and swapped
// when the container asks for a quarter turn. A file that is anamorphic or
// rotated is a different shape on screen than it is in the buffer, and it is
// the shape on screen that decides which way up wastes least.
//
// A REQUEST. The system may decline it, and nothing here depends on it having
// worked — the manifest declares configChanges for orientation and screenSize,
// so a rotation arrives as an ordinary window resize, the swapchain comes back
// VK_ERROR_OUT_OF_DATE_KHR, and the engine rebuilds it. The decoder, the audio
// device and the clock never learn it happened.
void PlayerWindow::Impl::applyOrientationFor(const TrackEntry& v) {
#if defined(__ANDROID__)
    if (v.width == 0 || v.height == 0) return;

    const bool quarterTurn = (v.rotationDegrees % 180) != 0;
    double w = static_cast<double>(v.width) * v.pixelAspect();
    double h = static_cast<double>(v.height);
    if (quarterTurn) std::swap(w, h);
    if (w <= 0.0 || h <= 0.0) return;

    const double aspect = w / h;

    // Square-ish content asks for NOTHING. Within a few percent of 1:1 neither
    // orientation wastes less, so forcing one would override somebody's
    // rotation lock on a coin flip. 3% is far tighter than the gap between 1:1
    // and any real aspect ratio (4:3 is 1.33, 16:9 is 1.78).
    int mode = activity::OrientationUnspecified;
    if      (aspect > 1.03) mode = activity::OrientationSensorLandscape;
    else if (aspect < 0.97) mode = activity::OrientationSensorPortrait;

    activity::request_orientation(mode);
    LOGI("orientation: displayed %.0fx%.0f (%.3f:1) -> %s", w, h, aspect,
         mode == activity::OrientationSensorLandscape ? "sensor landscape" :
         mode == activity::OrientationSensorPortrait  ? "sensor portrait"
                                                      : "unspecified (square-ish)");
#else
    (void)v;
#endif
}

// ── The transport, on both sides of the seam ───────────────────────────────
void PlayerWindow::Impl::syncAudioTransport() {
    if (!audio) return;
    // Playing is the only state that should be making sound. Ended is
    // deliberately NOT included: the file has run out, and whatever the device
    // still holds should play out rather than be cut off a buffer early.
    if (player.state() == State::Playing) audio->start();
    else if (player.state() != State::Ended) audio->pause();
}

void PlayerWindow::Impl::pauseForInterruption() {
    // Still filling the pipeline: there is nothing to pause, and forcing the
    // prime to complete because the user walked away is the opposite of what
    // they asked for.
    if (priming) return;
    if (player.state() != State::Playing) return;
    player.pause();
    syncAudioTransport();
    LOGI("paused: the window is no longer in front");
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
    // Stop whatever is already playing, FIRST.
    //
    // Player::open() calls close() on itself, which tears down the demuxer the
    // feed thread is at that moment reading from — and assigning over a
    // joinable std::thread is an immediate terminate() besides. Nothing has
    // opened a second file yet, so neither has happened; both are one line of
    // caller away, and the second file is the obvious next feature.
    feedRunning = false;
    feeding = false;
    priming = false;
    sawEndOfStream = false;
    framePeriodMeasured = false;
    framePeriodUs = kAssumedFrameUs;
    // run() owns appliedDisplayPeriodUs and will re-apply for the new file
    // once it has measured one; nothing to reset here beyond the above.
    if (feed.joinable()) feed.join();
    if (audio) audio->pause();
    audioOwned.reset();
    audio = nullptr;
    haveVideo = haveAudio = false;

#if defined(__ANDROID__)
    // The video Sink is what Player owns and drives. Frames come back on the
    // decoder's own thread, tagged with the clock generation current AT THAT
    // MOMENT, so a frame decoded before a seek can be recognised after it.
    auto sink = std::make_unique<MediaCodecVideo>([this](DecodedFrame f) {
        // The end-of-stream marker: no handle, no release, just a timestamp.
        // It used to be discarded here, which is why nothing ever reached
        // Ended — the decoder said it was finished and the one line that could
        // hear it threw the message away.
        if (!f.valid()) {
            sawEndOfStream.store(true, std::memory_order_release);
            return;
        }
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
        // What the PANEL can show, for the tone map. Asked only when we
        // actually got an HDR swapchain: on the SDR fallback the previously
        // tested behaviour is the one worth keeping, and a headroom report is
        // about the DISPLAY rather than about what this window was granted.
        //
        // app_shell answers in multiples of SDR white, which Android takes as
        // 203 cd/m^2 (the ITU-R BT.2408 reference). A headroom of exactly 1.0
        // means "no headroom" or "will not say", and both are answered here by
        // leaving the content's own peak in place.
        float displayPeakNits = 0.0f;
        if (renderer->hdrActive()) {
            const float headroom = activity::display_hdr_headroom();
            if (headroom > 1.0f) displayPeakNits = headroom * 203.0f;
        }
        // Tell the renderer what colour these frames are BEFORE the first one
        // arrives — the conversion object is built on the first import.
        const float par = static_cast<float>(v->pixelAspect());
        video.configure(*renderer, v->colour, v->rotationDegrees, displayPeakNits, par);
        applyOrientationFor(*v);
        LOGI("video: %ux%u %s rotation=%d par=%.4f | display peak %.0f nits%s",
             v->width, v->height,
             v->colour.isHdr10() ? "HDR10 (PQ, BT.2020)" : "SDR",
             v->rotationDegrees, par, displayPeakNits,
             displayPeakNits > 0.0f ? "" : " (unknown; using the content's)");
    }
    if (a) {
        audioOwned = std::make_unique<FlacOutput>();
        if (audioOwned->configure(*a)) {
            audio = audioOwned.get();
            audioTrack = a->number;
            haveAudio = true;
            // So a seek discards the audio in flight along with the video.
            // Audio is the master clock; flushing only one side moves the
            // picture and leaves the timeline where the sound was.
            AudioOutput* av = audio;
            player.setAudioFlush([av] { av->flush(); });
            LOGI("audio: FLAC %.0f Hz x %u", a->sampleRate, a->channels);
        } else {
            // Playable without sound is better than not playable. Say so once.
            LOGE("audio disabled: %s", audioOwned->error().c_str());
            audioOwned.reset();
        }
    }

    feedGeneration = player.clock().generation();
    feedRunning = true;
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
            /*desiredSwapchainImages=*/4, OutputTarget::Hdr10PQ,
            // FIFO. Frames here are scheduled by a clock for particular
            // instants, so there is no "newest frame" for MAILBOX to prefer —
            // only a render loop free-running at ~900 fps over identical
            // content, and the heat that makes is the decoder's problem too.
            PresentPolicy::Vsync);
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

bool PlayerWindow::openPath(const std::string& path) {
    return impl_->openFile(path);
}

void PlayerWindow::run() {
    // The wall-clock reference for a file with no audio. Only this thread
    // touches it, and only the freerun branch below reads it.
    auto lastTick = std::chrono::steady_clock::now();
    int64_t appliedPeriodUs = 0;
    // Tracked SEPARATELY from appliedPeriodUs, because the two are gated on
    // different things. The drop threshold is applied immediately, from the
    // 33 ms guess if that is all there is; the display rate waits for a real
    // measurement. Sharing one variable meant the guess satisfied the 2%
    // re-apply gate, so when the true period arrived it was only 400 us away
    // and the rate was never asked for at all.
    int64_t appliedDisplayPeriodUs = 0;
    uint64_t lastGeneration = impl_->player.clock().generation();

    // How long one turn of this loop takes, which under a vsync-paced
    // swapchain IS the display's refresh interval — vkQueuePresentKHR blocks
    // until the next one. Measured rather than assumed, because the panel does
    // not necessarily end up at the rate we asked for: it may run at a
    // multiple of the content's rate, or refuse the request entirely.
    //
    // Smoothed over eight iterations so a single hitch does not move it, and
    // seeded from the assumed frame period so the first few turns are sane.
    auto lastLoopTp = std::chrono::steady_clock::now();
    int64_t loopPeriodUs = kAssumedFrameUs;

    // Edge detection on the clock's paused flag, so the interpolator is reset
    // once on resume rather than on every iteration of a pause.
    bool wasPaused = impl_->player.clock().paused();

    // ── The master-clock watchdog ─────────────────────────────────────────
    //
    // Audio is the timeline, which is the right design and also a single point
    // of failure: if the audio position stops answering, nothing advances and
    // the picture freezes with no error anywhere. A route change does exactly
    // that (see AAudioSink::disconnected()), and it is not the only thing that
    // could — a wedged decoder or a device that stops reporting would look
    // identical from here.
    //
    // So this does not detect a CAUSE. It detects the symptom, which is the
    // only thing that can be checked without knowing what went wrong, and
    // falls back to the wall clock so the film keeps playing while the audio
    // path sorts itself out. Playing on with a temporary sync error beats
    // stopping dead.
    int64_t lastAudioPtsUs = -1;
    auto    audioMovedTp   = std::chrono::steady_clock::now();
    bool    audioStalled   = false;
    // The device reports once per burst, measured on this phone at 50 times a
    // second. Half a second is twenty-five bursts — far outside any jitter,
    // far inside the time it takes a viewer to conclude the player has hung.
    constexpr int64_t kAudioStallUs = 500'000;

#if VP_STATS
    // ── The soak report ───────────────────────────────────────────────────
    //
    // The once-a-second line answers "is it smooth right now". It cannot
    // answer "what does an hour look like": a single bad second scrolls away,
    // and a drift is not visible in three thousand separate lines.
    //
    // Drift is the number this exists for. Audio is the master clock, so the
    // timeline should advance at exactly the rate the wall clock does; the
    // difference between them, measured over minutes, is the one thing that
    // says whether the device's own sense of time and ours are diverging.
    // Re-anchored on every pause, since paused time is not drift.
    auto    soakReportTp  = std::chrono::steady_clock::now();
    auto    soakStartTp   = soakReportTp;
    int64_t soakStartPtsUs = 0;
    bool    soakAnchored   = false;
#endif

    while (impl_->running) {
        // Always "have work": a playing video needs a frame per vsync. The
        // dirty-flag economy a music player uses does not apply here.
        impl_->host->pump(/*haveWork=*/true);
        if (impl_->host->quitRequested()) break;
        // No surface — backgrounded, or between onSurfaceLost() and
        // onSurfaceRecreated(). There is nothing to present to, and this used
        // to spin the branch below at whatever rate pump() returned: a busy
        // loop with no vkQueuePresentKHR to pace it, burning a core in an app
        // the user has already left. Roughly a frame's worth of sleep, since
        // nothing here is latency-sensitive while there is no window.
        if (!impl_->renderer) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

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
            // Safe to set from the guess — it is a sane threshold at any rate,
            // and it only decides when to give up on a frame.
            impl_->player.clock().setDropThresholdUs(periodUs / 2);
        }

        // The display rate, on its own gate. NOT safe to set from the guess:
        // telling the compositor "this is 30 fps" while still assuming it is
        // what pins a 120 Hz panel to 30 Hz for a 60 fps file.
        if (impl_->framePeriodMeasured.load(std::memory_order_acquire)) {
            const int64_t dispDelta = periodUs > appliedDisplayPeriodUs
                                          ? periodUs - appliedDisplayPeriodUs
                                          : appliedDisplayPeriodUs - periodUs;
            // Once because it has never been set, and thereafter only on a
            // change worth asking the compositor to reconsider a mode for.
            if (appliedDisplayPeriodUs == 0 || dispDelta * 50 > appliedDisplayPeriodUs) {
                appliedDisplayPeriodUs = periodUs;
                impl_->applyDisplayFrameRate(periodUs);
            }
        }

        // The frame chosen this turn is displayed at the NEXT present, one
        // loop period from now. Telling the clock so is what centres its
        // acceptance window on when the frame is really shown; without it the
        // window sits entirely in the past and a third of the frames are
        // dropped for being "late" when nothing was ever short of them.
        {
            const auto loopNow = std::chrono::steady_clock::now();
            const int64_t dtUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                     loopNow - lastLoopTp).count();
            lastLoopTp = loopNow;
            // A turn longer than a few frames is a stall, a resume or a
            // debugger, not a refresh interval. Let it move the average and it
            // schedules the whole pipeline into the future.
            if (dtUs > 0 && dtUs < 60000)
                loopPeriodUs = (loopPeriodUs * 7 + dtUs) / 8;
            impl_->player.clock().setPresentationLeadUs(loopPeriodUs);
        }

        impl_->tryStartAfterPriming();

        // A seek restarts the audio device's position, so no anchor from
        // before it means anything.
        const uint64_t gen = impl_->player.clock().generation();
        if (gen != lastGeneration) {
            lastGeneration = gen;
            impl_->audioClock.reset();
            // The stream is no longer over: whatever we have seeked to has its
            // own end still to come. The sinks clear their end-of-stream latch
            // in flush() for the same reason, and the feed thread clears its
            // `eof`. Missing this one is what would make a replay end
            // instantly, having marked itself finished before a frame arrived.
            impl_->sawEndOfStream.store(false, std::memory_order_release);
        }

        // Paused really means paused.
        //
        // Clock::pause() only sets a flag, because core/ has no audio device to
        // stop — and the branch below then assigned the device's advancing
        // position straight over nowUs_ on every iteration, so the flag decided
        // nothing at all. With audio present, pause was a no-op: the picture
        // kept moving, the sound kept playing, and the one gesture this app has
        // did nothing. The device itself is stopped by syncAudioTransport();
        // this is the other half, and either alone is not enough.
        const bool clockPaused = impl_->player.clock().paused();
        if (clockPaused != wasPaused) {
            wasPaused = clockPaused;
            // Resuming: the device's reported position stood still across the
            // pause, so the interpolator's slope through that gap describes
            // nothing. Re-anchor rather than extrapolate from it.
            if (!clockPaused) impl_->audioClock.reset();
        }

        if (clockPaused) {
            // Hold the picture. decide() presents the frame that is due and
            // then keeps it, since a frozen clock never makes it late.
            impl_->video.present(*impl_->renderer, impl_->player.clock());
            impl_->drawFrame();
            // So the freerun branch does not measure the whole pause as one
            // enormous elapsed step the moment playback resumes.
            lastTick = std::chrono::steady_clock::now();
            VP_STAT(soakAnchored = false);   // paused time is not drift
            continue;
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
            const int64_t rawPtsUs = impl_->audio->playedPtsUs();

            if (rawPtsUs != lastAudioPtsUs) {
                lastAudioPtsUs = rawPtsUs;
                audioMovedTp   = nowTp;
                if (audioStalled) {
                    audioStalled = false;
                    // The position is answering again, but from a rebuilt
                    // stream re-anchored to whatever it is about to play — not
                    // from where the wall clock carried the timeline to. The
                    // interpolator must not draw a line between the two.
                    impl_->audioClock.reset();
                    LOGI("audio clock recovered");
                }
            } else if (!audioStalled &&
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           nowTp - audioMovedTp).count() > kAudioStallUs) {
                audioStalled = true;
                LOGE("audio clock has not moved for %lld ms — running the "
                     "timeline off the wall clock until it does",
                     (long long)(kAudioStallUs / 1000));
            }

            if (audioStalled) {
                int64_t deltaUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      nowTp - lastTick).count();
                const int64_t maxStepUs =
                    (appliedPeriodUs > 0 ? appliedPeriodUs : kAssumedFrameUs) * 4;
                if (deltaUs > maxStepUs) deltaUs = maxStepUs;
                if (deltaUs < 0) deltaUs = 0;
                impl_->player.clock().advanceFreerun(deltaUs);
            } else {
                impl_->player.clock().setAudioClock(
                    impl_->audioClock.update(rawPtsUs, monoUs));
            }
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

        // The end, which takes BOTH halves to know.
        //
        // The decoder's end-of-stream marker says nothing more will ever be
        // produced; the queue draining says everything produced has been shown.
        // Neither alone is the end — the marker arrives while several frames
        // are still waiting, and an empty queue during playback is just a
        // decoder that is briefly behind. Player cannot work this out for
        // itself: core/ sees packets going in and never sees a frame come out,
        // which is why markEnded() is told rather than deduced.
        //
        // markEnded() ignores everything but Playing, so this may fire on every
        // iteration from here on and the first one is the only one that counts.
#if VP_STATS
        {
            const auto soakNow = std::chrono::steady_clock::now();
            if (!soakAnchored && impl_->player.state() == State::Playing) {
                soakAnchored   = true;
                soakStartTp    = soakNow;
                soakStartPtsUs = impl_->player.clock().nowUs();
            }
            if (soakAnchored && soakNow - soakReportTp >= std::chrono::seconds(30)) {
                soakReportTp = soakNow;
                const int64_t wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                           soakNow - soakStartTp).count();
                const int64_t clockUs = impl_->player.clock().nowUs() - soakStartPtsUs;
                int64_t shown = 0, dropped = 0, refused = 0, worstUs = 0;
                impl_->video.lifetime(shown, dropped, refused, worstUs);
                LOGI("soak: %lld s played | %lld shown, %lld dropped, %lld refused"
                     " | worst gap %lld us | drift %+lld ms",
                     (long long)(wallUs / 1000000), (long long)shown,
                     (long long)dropped, (long long)refused, (long long)worstUs,
                     (long long)((clockUs - wallUs) / 1000));
            }
        }
#endif

        if (impl_->sawEndOfStream.load(std::memory_order_acquire) &&
            impl_->video.queued() == 0 &&
            impl_->player.state() == State::Playing) {
            impl_->player.markEnded();
            impl_->syncAudioTransport();
            LOGI("end of stream: %lld ms played, holding the last frame",
                 (long long)(impl_->player.positionUs() / 1000));
        }
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
    impl_->feedRunning = false;
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

void PlayerWindow::onHostFocusLost() {
    // The earliest and most reliable signal that the user has looked away: a
    // notification shade, a call, the recents switcher, or simply leaving.
    // Earlier than losing the surface, which for a backgrounded app can be
    // seconds later or not at all — so playback that should stop when the user
    // stops watching has to hang off this rather than off onSurfaceLost().
    //
    // Deliberately NOT resumed on focus gained. A player that restarts itself
    // because a notification was dismissed takes a decision that is the
    // viewer's to take.
    impl_->pauseForInterruption();
}

void PlayerWindow::onSurfaceLost() {
    // Android takes the swapchain, every texture and every imported buffer
    // when the user leaves the app. CPU state survives; GPU state does not.
    //
    // Paused first, and this is what makes the frames already queued still
    // valid when the surface comes back: a frozen clock does not age them past
    // the drop threshold while the app is away, so returning shows the picture
    // where it was left instead of discarding a queue's worth of frames for
    // being late by however long the user was gone.
    impl_->pauseForInterruption();
    impl_->renderer.reset();
}

bool PlayerWindow::onSurfaceRecreated() {
    try {
        impl_->renderer = std::make_unique<Renderer>(
            impl_->host->surfaceProvider(), impl_->host->assetReader(), 4,
            OutputTarget::Hdr10PQ, PresentPolicy::Vsync);
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
    // The half core/ cannot do. Without it the state machine paused and the
    // speaker carried on, which — since audio is the master clock — meant
    // nothing paused at all.
    impl_->syncAudioTransport();
}

void PlayerWindow::onKeyDownPortable(int keyCode) {
    if (keyCode == key::Space) togglePlayback();
}

void PlayerWindow::onLButtonUp(int, int) { togglePlayback(); }

}  // namespace vp
