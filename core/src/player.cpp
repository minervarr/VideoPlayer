#include "core/player.h"

#include <algorithm>

namespace vp {

const char* stateName(State s) {
    switch (s) {
        case State::Idle:    return "Idle";
        case State::Opening: return "Opening";
        case State::Playing: return "Playing";
        case State::Paused:  return "Paused";
        case State::Seeking: return "Seeking";
        case State::Ended:   return "Ended";
        case State::Failed:  return "Failed";
    }
    return "?";
}

struct Player::Impl {
    Demuxer  demux;
    Clock    clock;
    State    state = State::Idle;
    std::string err;

    std::unique_ptr<Sink> sink;
    FrameReady onFrame;

    int64_t positionUs = 0;
    int64_t durationUs = 0;

    void fail(std::string why) {
        err = std::move(why);
        state = State::Failed;
    }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() { close(); }

bool Player::open(const std::string& path, std::unique_ptr<Sink> sink, FrameReady onFrame) {
    close();
    impl_->state = State::Opening;
    impl_->sink = std::move(sink);
    impl_->onFrame = std::move(onFrame);

    if (!impl_->demux.open(path)) {
        impl_->fail(impl_->demux.error());
        return false;
    }

    const TrackEntry* video = impl_->demux.videoTrack();
    const TrackEntry* audio = impl_->demux.audioTrack();
    if (!video && !audio) {
        // Opened fine, nothing in it we decode. Says so rather than showing a
        // black window — the codec IDs are in the message because "unsupported"
        // without naming what is unsupported helps nobody.
        std::string names;
        for (const TrackEntry& t : impl_->demux.tracks()) {
            if (!names.empty()) names += ", ";
            names += t.codecId;
        }
        impl_->fail("no HEVC video or FLAC audio track in this file (found: " + names + ")");
        return false;
    }

    if (!impl_->sink || !impl_->sink->configure(video, audio)) {
        impl_->fail("the decoder refused this file");
        return false;
    }

    impl_->durationUs = static_cast<int64_t>(impl_->demux.info().durationUs());
    impl_->positionUs = 0;
    impl_->clock.reset(0);
    impl_->state = State::Paused;
    return true;
}

void Player::close() {
    if (impl_->sink) impl_->sink->flush();
    impl_->sink.reset();
    impl_->onFrame = nullptr;
    impl_->demux.close();
    impl_->state = State::Idle;
    impl_->err.clear();
    impl_->positionUs = impl_->durationUs = 0;
}

void Player::play() {
    // Playing from Ended means playing again from the top, which is what a
    // person pressing play on a finished file means. Everything else is
    // either already playing or has nothing to play.
    if (impl_->state == State::Ended) seek(0);
    if (impl_->state != State::Paused && impl_->state != State::Ended) return;
    impl_->state = State::Playing;
    impl_->clock.start();
}

void Player::pause() {
    if (impl_->state != State::Playing) return;
    impl_->state = State::Paused;
    impl_->clock.pause();
}

void Player::togglePause() {
    impl_->state == State::Playing ? pause() : play();
}

bool Player::seek(int64_t timeUs) {
    if (impl_->state == State::Idle || impl_->state == State::Failed ||
        impl_->state == State::Opening)
        return false;

    if (impl_->durationUs > 0) timeUs = std::clamp<int64_t>(timeUs, 0, impl_->durationUs);
    else timeUs = std::max<int64_t>(0, timeUs);

    const bool wasPlaying = impl_->state == State::Playing;
    impl_->state = State::Seeking;

    const int64_t landed = impl_->demux.seek(timeUs);
    if (landed < 0) {
        // Not a Failed state: the file is still perfectly playable forwards.
        // Restore what we interrupted and report the refusal.
        impl_->err = impl_->demux.error();
        impl_->state = wasPlaying ? State::Playing : State::Paused;
        return false;
    }

    // Order matters. Flush first, so nothing decoded before the seek can be
    // presented after it; then re-base the clock, which bumps the generation
    // every in-flight frame was tagged with.
    if (impl_->sink) impl_->sink->flush();
    impl_->clock.reset(landed);
    impl_->positionUs = landed;
    impl_->state = wasPlaying ? State::Playing : State::Paused;
    if (wasPlaying) impl_->clock.start();
    return true;
}

State Player::state() const { return impl_->state; }
const std::string& Player::error() const { return impl_->err; }
int64_t Player::positionUs() const { return impl_->clock.nowUs(); }
int64_t Player::durationUs() const { return impl_->durationUs; }
const Demuxer& Player::demuxer() const { return impl_->demux; }
Clock& Player::clock() { return impl_->clock; }

}  // namespace vp
