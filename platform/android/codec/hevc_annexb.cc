#include "codec/hevc_annexb.hh"

#include <cstring>

namespace vp {
namespace {

// Four bytes, not three. Both are legal Annex B start codes and decoders take
// either; four is what every muxer-to-decoder converter emits and what leaves
// the parameter-set blob byte-identical to what a reference tool produces,
// which matters the first time you have to diff one.
constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

void appendStartCode(std::vector<uint8_t>& out) {
    out.insert(out.end(), kStartCode, kStartCode + 4);
}

// The fixed part of an HEVCDecoderConfigurationRecord, up to and including
// numOfArrays. Layout is ISO/IEC 14496-15 8.3.3.1:
//   [0]      configurationVersion (must be 1)
//   [1..20]  profile/tier/level, chroma and bit-depth fields
//   [21]     ...ends with lengthSizeMinusOne in the low 2 bits
//   [22]     numOfArrays
// then, per array: one byte (array_completeness | reserved | NAL_unit_type),
// a 16-bit numNalus, and then that many (16-bit length, NAL) pairs.
constexpr size_t kHvccHeaderSize = 23;
constexpr size_t kLengthSizeByte = 21;
constexpr size_t kNumArraysByte  = 22;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

}  // namespace

int hvccLengthSize(const std::vector<uint8_t>& hvcc) {
    if (hvcc.size() < kHvccHeaderSize || hvcc[0] != 1) return 0;
    const int n = (hvcc[kLengthSizeByte] & 0x03) + 1;
    // 3 is not a legal encoding (lengthSizeMinusOne == 2 is reserved), and a
    // record claiming it is malformed rather than merely unusual.
    return (n == 1 || n == 2 || n == 4) ? n : 0;
}

std::vector<uint8_t> hvccToAnnexB(const std::vector<uint8_t>& hvcc) {
    std::vector<uint8_t> out;
    if (hvccLengthSize(hvcc) == 0) return out;

    const size_t numArrays = hvcc[kNumArraysByte];
    size_t p = kHvccHeaderSize;

    for (size_t a = 0; a < numArrays; ++a) {
        if (p + 3 > hvcc.size()) return {};
        // The NAL type byte is read past deliberately: every array in the
        // record belongs in csd-0, and filtering to VPS/SPS/PPS by type would
        // silently drop a fourth array a future encoder adds.
        ++p;
        const uint16_t numNalus = be16(&hvcc[p]);
        p += 2;

        for (uint16_t n = 0; n < numNalus; ++n) {
            if (p + 2 > hvcc.size()) return {};
            const uint16_t len = be16(&hvcc[p]);
            p += 2;
            if (p + len > hvcc.size()) return {};
            appendStartCode(out);
            out.insert(out.end(), hvcc.begin() + p, hvcc.begin() + p + len);
            p += len;
        }
    }
    return out;
}

bool annexBFromLengthPrefixed(const uint8_t* data, size_t size, int lengthSize,
                              std::vector<uint8_t>& out) {
    out.clear();
    if (lengthSize != 1 && lengthSize != 2 && lengthSize != 4) return false;

    size_t p = 0;
    while (p + static_cast<size_t>(lengthSize) <= size) {
        uint32_t nalLen = 0;
        for (int i = 0; i < lengthSize; ++i)
            nalLen = (nalLen << 8) | data[p + i];
        p += static_cast<size_t>(lengthSize);

        // A length that runs past the buffer means the sample is truncated or
        // was never length-prefixed at all. Refuse the whole access unit: a
        // partial one leaves the decoder mid-NAL for everything after it.
        if (nalLen == 0 || p + nalLen > size) return false;

        appendStartCode(out);
        out.insert(out.end(), data + p, data + p + nalLen);
        p += nalLen;
    }
    // Trailing bytes that are not a whole NAL: same reasoning as above.
    return p == size && !out.empty();
}

}  // namespace vp
