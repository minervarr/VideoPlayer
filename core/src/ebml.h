#pragma once

// EBML primitives — src-private. Not in include/core/, because nothing outside
// the parser has any business knowing that Matroska is EBML underneath.
//
// Two variable-width integer encodings live here and they are NOT the same
// thing, which is the single most common way a hand-written Matroska parser
// goes wrong:
//
//   * an element ID keeps its length marker bit (0x1A45DFA3 is the EBML
//     header's ID, marker included — that is the value you compare against);
//   * an element SIZE has its marker bit stripped, and an all-ones payload
//     means "unknown length, read until a parent ends", which live-muxed
//     Segments really do use.

#include <cstdint>
#include <istream>

namespace vp::ebml {

inline constexpr uint64_t kUnknownSize = ~0ULL;

// Reads a class-ID varint, marker bit RETAINED. Returns 0 on EOF or on a
// leading byte of 0x00 (no marker in the first byte == not valid EBML here).
uint64_t readId(std::istream& in);

// Reads a size varint, marker bit STRIPPED. Returns kUnknownSize for the
// all-ones encoding, and kUnknownSize on EOF too — callers check the stream.
uint64_t readSize(std::istream& in);

// Fixed-width big-endian payloads, `len` bytes as Matroska stores them.
uint64_t readUInt(std::istream& in, uint64_t len);
int64_t  readInt(std::istream& in, uint64_t len);   // sign-extended
double   readFloat(std::istream& in, uint64_t len); // len must be 4 or 8
// Trailing NULs stripped: Matroska pads strings, and a language of "und\0" is
// not equal to "und".
std::string readString(std::istream& in, uint64_t len);

// A parsed element header and where its payload starts.
struct Element {
    // Where the element's ID byte is — NOT where its payload starts. Cues
    // point here, and so does "resume after this Cluster"; confusing the two
    // is a seek that lands inside a header.
    uint64_t startPos = 0;
    uint64_t id      = 0;
    uint64_t size    = 0;
    uint64_t dataPos = 0;
    bool     ok      = false;

    bool unknownSize() const { return size == kUnknownSize; }
    uint64_t endPos() const { return unknownSize() ? kUnknownSize : dataPos + size; }
};

Element readElement(std::istream& in);

// ── The IDs this parser knows ─────────────────────────────────────────────
// Only what an HDR10 + FLAC player reads. Everything else is skipped by size,
// which is why an unknown element is never an error.
enum : uint64_t {
    kEBMLHeader       = 0x1A45DFA3,
    kSegment          = 0x18538067,
    kSeekHead         = 0x114D9B74,
    kSeek             = 0x4DBB,
    kSeekID           = 0x53AB,
    kSeekPos          = 0x53AC,
    kInfo             = 0x1549A966,
    kTimecodeScale    = 0x2AD7B1,
    kDuration         = 0x4489,
    kTitle            = 0x7BA9,
    kMuxingApp        = 0x4D80,
    kWritingApp       = 0x5741,

    kTracks           = 0x1654AE6B,
    kTrackEntry       = 0xAE,
    kTrackNumber      = 0xD7,
    kTrackType        = 0x83,
    kCodecID          = 0x86,
    kCodecPrivate     = 0x63A2,
    kLanguage         = 0x22B59C,
    kFlagDefault      = 0x88,
    kVideo            = 0xE0,
    kPixelWidth       = 0xB0,
    kPixelHeight      = 0xBA,
    kDisplayWidth     = 0x54B0,
    kDisplayHeight    = 0x54BA,
    // Projection carries the display rotation. A phone recording landscape
    // while held upright stores the sensor's own orientation here rather than
    // rotating pixels, so a player that ignores it shows the picture sideways.
    kProjection       = 0x7670,
    kProjectionRoll   = 0x7675,
    kAudio            = 0xE1,
    kSamplingFreq     = 0xB5,
    kChannels         = 0x9F,
    kBitDepth         = 0x6264,

    kColour           = 0x55B0,
    kMatrixCoeffs     = 0x55B1,
    kBitsPerChannel   = 0x55B2,
    kChromaSitingHorz = 0x55B7,
    kChromaSitingVert = 0x55B8,
    kRange            = 0x55B9,
    kTransferChar     = 0x55BA,
    kPrimaries        = 0x55BB,
    kMaxCLL           = 0x55BC,
    kMaxFALL          = 0x55BD,
    kMasteringMeta    = 0x55D0,
    kPrimaryRChromX   = 0x55D1,
    kPrimaryRChromY   = 0x55D2,
    kPrimaryGChromX   = 0x55D3,
    kPrimaryGChromY   = 0x55D4,
    kPrimaryBChromX   = 0x55D5,
    kPrimaryBChromY   = 0x55D6,
    kWhitePointChromX = 0x55D7,
    kWhitePointChromY = 0x55D8,
    kLuminanceMax     = 0x55D9,
    kLuminanceMin     = 0x55DA,

    kCluster          = 0x1F43B675,
    kTimecode         = 0xE7,
    kSimpleBlock      = 0xA3,
    kBlockGroup       = 0xA0,
    kBlock            = 0xA1,
    kBlockDuration    = 0x9B,
    kReferenceBlock   = 0xFB,

    kCues             = 0x1C53BB6B,
    kCuePoint         = 0xBB,
    kCueTime          = 0xB3,
    kCueTrackPosition = 0xB7,
    kCueTrack         = 0xF7,
    kCueClusterPos    = 0xF1,
    kCueRelativePos   = 0xF0,
};

}  // namespace vp::ebml
