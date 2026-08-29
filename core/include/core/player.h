#pragma once

// Player — the state machine, and the ONE place a transition is legal.
//
// Everything a media player gets wrong under stress is a state question: a
// seek arriving mid-open, a pause during a seek, an end-of-stream racing a
// user's scrub. Scattering those checks across the UI and the decoder is how
// players acquire their reputation. They live here instead, in a file with no
// threads, no OS, and no decoder, so the rules can be read in one sitting and
// asserted in a test.
//
// This type does not own the decoder or the audio device. It owns the Demuxer
// and the Clock, and it tells the platform layer what to do through the
// callbacks in Sink. The Android layer implements Sink; a desktop one later
// implements the same three methods and nothing above changes.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/clock.h"
#include "core/demux.h"
#include "core/video_frame.h"

namespace vp {

enum class State {
    Idle,     // nothing open
    Opening,  // demuxing headers, starting decoders
    Playing,
    Paused,
    Seeking,  // decoders flushed, waiting for the first frame past the target
    Ended,    // ran to the end; still open, seek returns to Playing
    Failed,   // open() or a decoder refused; error() says why
};

const char* stateName(State s);

// What the platform must provide. Three methods, because a video player only
// really asks the OS for three things: decode this, play this, and tell me
// when you are done with it.
class Sink {
public:
    virtual ~Sink() = default;

    // Called once per open, after the tracks are known and before any packet.
    // False means "I cannot decode this" and puts the player in Failed.
    virtual bool configure(const TrackEntry* video, const TrackEntry* audio) = 0;

    // Hand a compressed packet to the matching decoder. False when the
    // decoder's input queue is full — the caller retries rather than dropping,
    // since a dropped compressed packet corrupts everything until the next
    // keyframe.
    virtual bool submit(const Packet& p) = 0;

    // Drop everything in flight. Called on seek and on close.
    virtual void flush() = 0;
};

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // The frame the render thread should draw, handed over as it becomes
    // ready. Called from whichever thread the decoder emits on — the consumer
    // is responsible for getting it to the renderer, and for calling
    // release() when the frame has been presented.
    using FrameReady = std::function<void(DecodedFrame)>;

    bool open(const std::string& path, std::unique_ptr<Sink> sink, FrameReady onFrame);
    // Over a stream the platform already has open — an Android file manager
    // hands a viewer a content:// URI, which has no path behind it. See
    // Demuxer::open(std::unique_ptr<std::istream>).
    bool open(std::unique_ptr<std::istream> stream, std::unique_ptr<Sink> sink,
              FrameReady onFrame);
    void close();

    void play();
    void pause();
    void togglePause();
    // Clamped to [0, duration]. On a file with no Cues this is refused and
    // returns false rather than silently scanning for minutes.
    bool seek(int64_t timeUs);

    // What to run when a seek discards the audio already in flight.
    //
    // Audio is not a Sink here — Player's Sink is the video decoder, and the
    // audio path carries a clock and a transport that Sink has no business
    // knowing about. That left seek() flushing only video: the audio decoder
    // and the device's own buffer kept playing the segment that had just been
    // left, and since audio is the master clock, the timeline stayed with the
    // sound while the picture jumped. Nothing had noticed because nothing
    // called seek() yet.
    void setAudioFlush(std::function<void()> flush);

    State state() const;
    const std::string& error() const;
    int64_t positionUs() const;
    int64_t durationUs() const;
    // Mutable: the caller pulls packets out of it on its own thread. Player
    // deliberately owns no thread — feeding is the application's job, and
    // core/ is called FROM threads rather than starting any.
    Demuxer& demuxer();
    Clock& clock();
    // The Sink this player was opened with, borrowed. The application needs it
    // to submit packets; Player keeps ownership so that close() can flush it
    // before anything else goes away.
    Sink* sink();

private:
    // Track selection, sink configuration and the initial state — identical
    // whichever open() got here.
    bool finishOpen();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
