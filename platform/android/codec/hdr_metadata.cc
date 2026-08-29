#include "codec/hdr_metadata.hh"

#include <media/NdkMediaFormat.h>

#include <cstring>

namespace vp {
namespace {

// AMEDIAFORMAT_KEY_COLOR_* take MediaFormat's own small enumerations, NOT the
// ISO/IEC 23001-8 numbers the container stores. This is the whole reason this
// file exists: the two vocabularies look interchangeable and are not.
// Values from Android's MediaFormat (COLOR_STANDARD_*, COLOR_TRANSFER_*,
// COLOR_RANGE_*).
constexpr int32_t kColorStandardBT709      = 1;
constexpr int32_t kColorStandardBT2020     = 6;
constexpr int32_t kColorTransferSDRVideo   = 3;
constexpr int32_t kColorTransferST2084     = 6;
constexpr int32_t kColorTransferHLG        = 7;
constexpr int32_t kColorRangeFull          = 1;
constexpr int32_t kColorRangeLimited       = 2;

// ST 2086 stores chromaticity in units of 0.00002 and luminance in 0.0001
// cd/m^2, as 16-bit little-endian fields. The blob AMEDIAFORMAT_KEY_HDR_
// STATIC_INFO wants is 25 bytes: a type byte, then R/G/B/W x,y, then max/min
// luminance, then MaxCLL and MaxFALL.
void put16(uint8_t*& p, uint32_t v) {
    *p++ = static_cast<uint8_t>(v & 0xFF);
    *p++ = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint32_t chroma(double v) { return static_cast<uint32_t>(v / 0.00002 + 0.5); }

}  // namespace

std::vector<uint8_t> hdrStaticInfo(const ColourInfo& c) {
    if (!c.mastering.present && c.maxCLL == 0 && c.maxFALL == 0) return {};

    std::vector<uint8_t> blob(25, 0);
    uint8_t* p = blob.data();
    *p++ = 0;  // type: "static metadata type 1"

    const MasteringMetadata& m = c.mastering;
    put16(p, chroma(m.redX));   put16(p, chroma(m.redY));
    put16(p, chroma(m.greenX)); put16(p, chroma(m.greenY));
    put16(p, chroma(m.blueX));  put16(p, chroma(m.blueY));
    put16(p, chroma(m.whiteX)); put16(p, chroma(m.whiteY));
    // Max luminance is whole cd/m^2 here; min is in 0.0001 cd/m^2.
    put16(p, static_cast<uint32_t>(m.maxLuminance + 0.5));
    put16(p, static_cast<uint32_t>(m.minLuminance * 10000.0 + 0.5));
    put16(p, static_cast<uint32_t>(c.maxCLL));
    put16(p, static_cast<uint32_t>(c.maxFALL));
    return blob;
}

void applyToFormat(const ColourInfo& c, AMediaFormat* fmt) {
    if (!fmt) return;

    // Every write below is guarded on the container having actually said so.
    // An invented default here is a guess dressed as metadata, and the decoder
    // would have no way to tell the difference (CLAUDE.md rule 3).
    if (c.primaries == Primaries::BT2020)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_STANDARD, kColorStandardBT2020);
    else if (c.primaries == Primaries::BT709)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_STANDARD, kColorStandardBT709);

    if (c.transfer == Transfer::PQ)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_TRANSFER, kColorTransferST2084);
    else if (c.transfer == Transfer::HLG)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_TRANSFER, kColorTransferHLG);
    else if (c.transfer == Transfer::BT709)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_TRANSFER, kColorTransferSDRVideo);

    if (c.range == Range::Full)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_RANGE, kColorRangeFull);
    else if (c.range == Range::Limited)
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_RANGE, kColorRangeLimited);

    std::vector<uint8_t> blob = hdrStaticInfo(c);
    if (!blob.empty())
        AMediaFormat_setBuffer(fmt, "hdr-static-info", blob.data(), blob.size());
}

}  // namespace vp
