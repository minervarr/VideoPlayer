#pragma once

// Matroska, as much of it as an HDR10 video player needs and not one element
// more.
//
// This header is VOCABULARY ONLY — plain structs over plain integers. It names
// no file handle, no OS type, no decoder and no Vulkan object, because the
// parser behind it (src/mkv.cpp) is the one piece of this project that can be
// tested exhaustively on a desktop against real files, and that property is
// only worth having if nothing here can drag a dependency in.
//
// ── Why we parse Matroska ourselves ────────────────────────────────────────
//
// The NDK's AMediaExtractor handles MP4 well and Matroska only partially: it
// will not hand back the Colour element's contents, which is precisely the
// information this player exists to honor. A file whose transfer function we
// have to guess is a file we cannot render correctly. So the container is
// ours, top to bottom, and no libavformat is involved anywhere.

#include <cstdint>
#include <string>
#include <vector>

namespace vp {

// ── Colour ────────────────────────────────────────────────────────────────
//
// Matroska's Colour element (0x55B0), whose numeric values are the SAME
// enumerations ISO/IEC 23001-8 (and so HEVC's VUI, and so Android's
// AMediaFormat HDR keys) use. They are stored, not translated: a value we do
// not recognize must survive the trip to the decoder unchanged rather than be
// flattened into a default here.
enum class Primaries : int {
    Unspecified = 2,
    BT709       = 1,
    BT2020      = 9,   // what HDR10 uses
};

enum class Transfer : int {
    Unspecified = 2,
    BT709       = 1,
    PQ          = 16,  // SMPTE ST 2084 — the one this player is built for
    HLG         = 18,
};

enum class MatrixCoeffs : int {
    Unspecified = 2,
    BT709       = 1,
    BT2020NCL   = 9,   // non-constant luminance, what HDR10 uses
};

enum class Range : int {
    Unspecified = 0,
    Limited     = 1,   // 64..940 at 10-bit; the common case
    Full        = 2,
};

// MasteringMetadata (0x55D0) + MaxCLL/MaxFALL. This is HDR10's STATIC
// metadata: it describes the display the content was graded on, and the
// renderer needs it to tone-map honestly rather than assume a reference
// monitor. Absent in plenty of real files, which is why `present` is a field
// and not an assumption — see the note on HDR() below.
struct MasteringMetadata {
    bool   present = false;
    double redX = 0, redY = 0;
    double greenX = 0, greenY = 0;
    double blueX = 0, blueY = 0;
    double whiteX = 0, whiteY = 0;
    double maxLuminance = 0;   // cd/m^2
    double minLuminance = 0;   // cd/m^2
};

struct ColourInfo {
    Primaries     primaries = Primaries::Unspecified;
    Transfer      transfer  = Transfer::Unspecified;
    MatrixCoeffs  matrix    = MatrixCoeffs::Unspecified;
    Range         range     = Range::Unspecified;
    int           bitDepth  = 0;   // BitsPerChannel; 10 for Main10
    MasteringMetadata mastering;
    int           maxCLL  = 0;     // cd/m^2, 0 == absent
    int           maxFALL = 0;     // cd/m^2, 0 == absent

    // The single question the render path asks. Deliberately BOTH conditions:
    // PQ alone with BT.709 primaries is not HDR10, and BT.2020 alone under a
    // BT.709 transfer is a wide-gamut SDR file. Neither is guessed at from the
    // bit depth, the resolution, or the file name — if the container does not
    // say so, this returns false and the SDR path draws it.
    bool isHdr10() const {
        return transfer == Transfer::PQ && primaries == Primaries::BT2020;
    }
};

// ── Tracks ────────────────────────────────────────────────────────────────

enum class TrackKind : int { Unknown = 0, Video = 1, Audio = 2, Subtitle = 17 };

// Matroska's CodecID string, narrowed to what this player accepts. Anything
// else is parsed, reported, and then declined by the player rather than
// silently opened and shown as nothing.
enum class Codec { Unknown, HEVC, FLAC };

struct TrackEntry {
    uint64_t  number  = 0;      // TrackNumber — what a Block references
    TrackKind kind    = TrackKind::Unknown;
    Codec     codec   = Codec::Unknown;
    std::string codecId;        // the raw string, kept for logging a decline
    std::string language;       // ISO 639-2, "und" when absent
    bool      isDefault = true;

    // Video
    uint32_t  width = 0, height = 0;             // PixelWidth/PixelHeight
    uint32_t  displayWidth = 0, displayHeight = 0;  // 0 == same as pixel
    // Clockwise degrees the picture must be turned to be upright, normalized
    // to one of 0/90/180/270. Read from Projection>ProjectionPoseRoll and
    // NOT guessed: a file that says nothing is 0, which is what "the pixels
    // are already upright" means.
    int       rotationDegrees = 0;
    ColourInfo colour;

    // Audio
    double    sampleRate = 0;
    uint32_t  channels   = 0;
    uint32_t  bitsPerSample = 0;

    // CodecPrivate: hvcC for HEVC, the FLAC STREAMINFO block for FLAC. Handed
    // to the decoder verbatim; this parser never interprets it.
    std::vector<uint8_t> codecPrivate;
};

// ── Cues ──────────────────────────────────────────────────────────────────
//
// Matroska's seek index. Without it, seeking means scanning from the start,
// which on a two-hour remux is not seeking at all. A file with no Cues is
// still playable start-to-finish; Demuxer reports it so the UI can say so.
struct CuePoint {
    uint64_t timeUs         = 0;
    uint64_t trackNumber    = 0;
    uint64_t clusterOffset  = 0;   // absolute byte offset of the Cluster
    uint64_t relativePosition = 0; // within the Cluster, 0 when absent
};

// ── Segment-level facts ───────────────────────────────────────────────────

struct SegmentInfo {
    uint64_t timecodeScaleNs = 1000000;  // Matroska's default: 1 ms per tick
    double   durationTicks   = 0;        // in TimecodeScale units, 0 == unknown
    std::string title;
    std::string muxingApp, writingApp;

    uint64_t durationUs() const {
        return static_cast<uint64_t>(durationTicks * timecodeScaleNs / 1000.0);
    }
};

}  // namespace vp
