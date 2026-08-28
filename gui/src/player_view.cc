#include "player_view.hh"

#include <atomic>
#include <thread>
#include <vector>

#include "canvas.hh"
#include "keys.hh"
#include "renderer.hh"

#if defined(__ANDROID__)
#include <android/log.h>
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

// How far ahead of the audio clock the feed thread is allowed to run. Without
// a limit it reads the whole file into the decoders' input queues on a fast
// storage device; with one, memory stays flat and a seek discards almost
// nothing.
constexpr int64_t kFeedAheadUs = 2'000'000;

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

    std::string status;   // what to say when there is no picture

    // Reused per frame so the draw path allocates nothing.
    std::vector<float> curves, shapes;

    // Exactly one of the two is used: a path when we have one, otherwise a
    // stream the platform opened for us. See how create() chooses.
    bool openFile(const std::string& path, std::unique_ptr<std::istream> stream = nullptr);
    void runFeed();
    void drawFrame();
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
    Packet vp_pkt, ap_pkt;
    bool videoDone = false, audioDone = false;

    while (running) {
        if (!feeding) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        const uint64_t gen = player.clock().generation();
        if (gen != feedGeneration) {
            // A seek happened. Whatever is in hand belongs to the old segment.
            vp_pkt = Packet{};
            ap_pkt = Packet{};
            videoDone = audioDone = false;
            feedGeneration = gen;
        }

        const int64_t horizon = player.clock().nowUs() + kFeedAheadUs;
        bool didWork = false;

        if (haveVideo && !videoDone) {
            if (vp_pkt.empty() && !player.demuxer().nextPacket(videoTrack, vp_pkt))
                videoDone = true;
            if (!vp_pkt.empty() && vp_pkt.ptsUs < horizon) {
                Sink* sink = player.sink();
                if (sink && sink->submit(vp_pkt)) { vp_pkt = Packet{}; didWork = true; }
            }
        }
        if (haveAudio && audio && !audioDone) {
            if (ap_pkt.empty() && !player.demuxer().nextPacket(audioTrack, ap_pkt))
                audioDone = true;
            if (!ap_pkt.empty() && ap_pkt.ptsUs < horizon) {
                if (audio->submit(ap_pkt)) { ap_pkt = Packet{}; didWork = true; }
            }
        }

        // Nothing moved: either both decoders are full, or we are far enough
        // ahead. Either way, waiting is the correct thing and spinning is not.
        if (!didWork) std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
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
        video.configure(*renderer, v->colour);
        LOGI("video: %ux%u %s", v->width, v->height,
             v->colour.isHdr10() ? "HDR10 (PQ, BT.2020)" : "SDR");
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
    player.play();
    if (audio) audio->start();
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
    while (impl_->running) {
        // Always "have work": a playing video needs a frame per vsync. The
        // dirty-flag economy a music player uses does not apply here.
        impl_->host->pump(/*haveWork=*/true);
        if (impl_->host->quitRequested()) break;
        if (!impl_->renderer) continue;

        // Audio drives the clock; with no audio track the clock free-runs.
        if (impl_->haveAudio && impl_->audio)
            impl_->player.clock().setAudioClock(impl_->audio->playedPtsUs());

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

void PlayerWindow::onKeyDownPortable(int keyCode) {
    if (keyCode == key::Space) impl_->player.togglePause();
}

void PlayerWindow::onLButtonUp(int, int) { impl_->player.togglePause(); }

}  // namespace vp
