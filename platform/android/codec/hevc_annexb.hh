#pragma once

// hvcC -> Annex B. The one format translation between the container and the
// decoder, and the reason the first real file decoded nothing at all.
//
// Matroska stores HEVC the way MP4 does: each access unit is a sequence of NAL
// units, each preceded by a LENGTH field (usually 4 bytes), and the parameter
// sets live separately in CodecPrivate as an hvcC configuration record.
//
// AMediaCodec wants neither. It wants the Annex B byte stream: every NAL unit
// preceded by the start code 00 00 00 01, and csd-0 as the VPS/SPS/PPS
// concatenated in that same form.
//
// Getting this wrong does not produce an error. The codec accepts every input
// buffer, reports no fault, and simply never emits an output buffer — which
// looks exactly like a decoder that is slow, or a feed thread that is stuck.
// That is what happened: 1.4 MB packets went in at full speed and nothing came
// out, with nothing in any log to say why.
//
// Pure functions over bytes: no Android header, no Vulkan, nothing to mock.

#include <cstdint>
#include <vector>

namespace vp {

// Parses an hvcC record and returns its VPS/SPS/PPS as one Annex B blob, ready
// to hand to AMediaFormat as csd-0. Empty when `hvcc` is not a well-formed
// record — the caller treats that as "this track cannot be configured" rather
// than guessing at parameter sets.
std::vector<uint8_t> hvccToAnnexB(const std::vector<uint8_t>& hvcc);

// The NAL length field size an hvcC declares, in bytes (1, 2 or 4). Returns 0
// when the record is malformed. Read once at configure time and passed to
// every convertSample() call, because it is a property of the TRACK and
// re-deriving it per frame would be a parse per frame.
int hvccLengthSize(const std::vector<uint8_t>& hvcc);

// Rewrites one length-prefixed access unit in place into Annex B form.
// `lengthSize` comes from hvccLengthSize().
//
// Output is written to `out`, which is resized as needed and reused across
// calls — at 1.5 MB per frame, allocating per frame is a real cost.
//
// Returns false when the sample is truncated or self-inconsistent (a declared
// NAL length that runs past the end). A malformed access unit is dropped
// rather than passed on: a NAL boundary in the wrong place desynchronises the
// decoder for everything that follows.
bool annexBFromLengthPrefixed(const uint8_t* data, size_t size, int lengthSize,
                              std::vector<uint8_t>& out);

}  // namespace vp
