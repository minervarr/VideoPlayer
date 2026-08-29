#pragma once

// The seam between the two halves of the Matroska reader, src-private.
//
// mkv.cpp reads everything that is true for the WHOLE file — Info, Tracks,
// Colour, Cues — once, at open. demux.cpp reads Clusters, which is everything
// that is true for a moment. Splitting them is what lets tests/mkv_test.cc
// assert against a file's headers without a byte of playback machinery, which
// is the half most likely to be wrong on a real-world remux.

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "core/mkv.h"

namespace vp {

struct MkvHeaders {
    SegmentInfo             info;
    std::vector<TrackEntry> tracks;
    std::vector<CuePoint>   cues;

    // Where Segment's payload begins. Every CueClusterPosition in the file is
    // relative to THIS, not to the start of the file — an off-by-this-much is
    // the classic Matroska seek bug, and it does not show up until you seek.
    uint64_t segmentDataPos  = 0;
    uint64_t firstClusterPos = 0;   // 0 == none found

    // How long the stream actually is.
    //
    // The upper bound on every allocation the reader makes. Sizes come out of
    // the file — a CodecPrivate's length, a string's, a Block's payload — and
    // a truncated download or a corrupt byte turns one of them into a request
    // for gigabytes that std::vector will faithfully attempt. Nothing can
    // legitimately be longer than the file containing it.
    uint64_t fileEnd = 0;
};

// Parses from the start of `in`. False on anything structurally unreadable,
// with `err` set to something a person could act on. Unknown elements are
// skipped by size and are never an error.
bool parseHeaders(std::istream& in, MkvHeaders& out, std::string& err);

// CodecID string -> the narrow set this player accepts. Exposed for the test.
Codec codecFromId(const std::string& codecId);

}  // namespace vp
