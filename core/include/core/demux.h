#pragma once

// Demuxer — the whole of this project's file reading, behind six methods.
//
// It owns a Matroska parser (src/mkv.cpp) and hands out Packets. It does not
// decode, does not schedule, and does not know what a frame looks like once a
// decoder has been at it. The Android decoder and a future desktop one consume
// exactly this and nothing else, which is what keeps `core/` free of every OS
// header.
//
// Reading is a std::istream, which is the one std facility here that touches a
// file, and the boundary: no mmap, no AAsset, no content:// URI, no file
// descriptor. The platform resolves whatever it was handed down to a path or a
// stream before Demuxer ever sees it — see the second open() below for the
// case that forced the distinction.

#include <cstdint>
#include <istream>
#include <memory>
#include <string>
#include <vector>

#include "core/mkv.h"

namespace vp {

// One compressed unit: an HEVC access unit or a FLAC frame. `bytes` is owned,
// because the alternative — a view into a buffer the demuxer keeps reusing —
// makes the decoder's lifetime depend on the demuxer's read cadence, and the
// two run on different threads.
struct Packet {
    uint64_t trackNumber = 0;
    int64_t  ptsUs       = 0;
    int64_t  dtsUs       = 0;   // == ptsUs when the container gives no separate DTS
    bool     keyframe    = false;
    std::vector<uint8_t> bytes;

    bool empty() const { return bytes.empty(); }

    // Empties the packet for reuse while KEEPING the byte buffer's capacity,
    // so Demuxer can fill it again instead of allocating another. Assigning
    // Packet{} over it instead — which is what the feed thread did — frees a
    // buffer that is about to be asked for again, one 1.5 MB mmap/munmap pair
    // per frame.
    void reset() {
        trackNumber = 0;
        ptsUs = dtsUs = 0;
        keyframe = false;
        bytes.clear();
    }
};

class Demuxer {
public:
    Demuxer();
    ~Demuxer();
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    // Parses EBML header, Segment Info, Tracks and Cues. Does NOT read any
    // media data. False when the file is not Matroska or is structurally
    // unreadable; error() then says why, in words meant for a human.
    bool open(const std::string& path);

    // The same, over a stream the caller already has open.
    //
    // Android is why this exists: a file manager hands a viewer a content://
    // URI, which names a row in another app's ContentProvider rather than
    // anything on a filesystem. There is no path to pass — only a descriptor —
    // and reading a multi-gigabyte recording into memory to get at its header
    // is not a trade. The platform wraps its descriptor in a stream and this
    // takes it.
    //
    // The stream must be SEEKABLE. Matroska is not a format you can parse
    // forwards: Cues routinely sit after the clusters and are reached through
    // SeekHead, and seeking is the entire point of having them.
    bool open(std::unique_ptr<std::istream> stream);
    void close();

    const SegmentInfo& info() const;
    const std::vector<TrackEntry>& tracks() const;
    bool seekable() const;              // Cues were found
    const std::string& error() const;

    // The first default track of each kind whose codec we accept, or nullptr.
    // "Accept" is narrow on purpose (see Codec in mkv.h) — a file with an
    // AV1 video track opens fine and reports no playable video, which is a
    // better failure than a black window.
    const TrackEntry* videoTrack() const;
    const TrackEntry* audioTrack() const;

    // The next packet of ANY track, in the order the file stores them.
    //
    // This is what a player should use. Asking per-track looks convenient and
    // is a trap: the tracks are interleaved, so reaching the next audio packet
    // means reading — and holding — every video packet in between. With 1.5 MB
    // intra frames that queue grows by tens of megabytes a second the moment
    // one track is consumed slower than the other, and it grew until the
    // process died. Reading in storage order holds exactly one packet.
    bool nextPacket(Packet& out);

    // The next packet of ONE track, reading past and BUFFERING the others.
    //
    // Only safe when the caller drains every track it opened at roughly the
    // rate the file interleaves them. Kept because it is what a test wants —
    // ask for track 1 and assert on track 1 — and because it is the honest
    // shape of "I only care about this track" for a file with one.
    bool nextPacket(uint64_t trackNumber, Packet& out);

    // Seeks to the nearest Cue at or before `timeUs`. Returns the timestamp
    // actually landed on — always a keyframe, so always <= what was asked
    // for. Fails (returns a negative value) when !seekable().
    int64_t seek(int64_t timeUs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
