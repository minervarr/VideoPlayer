#pragma once

// Demuxer — the whole of this project's file reading, behind six methods.
//
// It owns a Matroska parser (src/mkv.cpp) and hands out Packets. It does not
// decode, does not schedule, and does not know what a frame looks like once a
// decoder has been at it. The Android decoder and a future desktop one consume
// exactly this and nothing else, which is what keeps `core/` free of every OS
// header.
//
// Reading is a std::ifstream, which is the one std facility here that touches
// a file. That is deliberate and is the boundary: no mmap, no AAsset, no
// content:// URI. The Host resolves whatever the platform hands it down to a
// path (or, later, to a Source interface) before Demuxer ever sees it.

#include <cstdint>
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

    // Reads the next Block for `trackNumber` in storage order. Returns false
    // at end of stream. Interleaving is the container's; the caller is
    // expected to pull both tracks and let core/clock.h reorder in time.
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
