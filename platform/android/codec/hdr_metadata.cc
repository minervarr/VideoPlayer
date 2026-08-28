#include "codec/hdr_metadata.hh"

namespace vp {

ShaderColour forShader(const ColourInfo& c) {
    ShaderColour s;
    s.matrix = c.matrix == MatrixCoeffs::BT2020NCL ? ShaderMatrix::BT2020NCL
                                                   : ShaderMatrix::BT709;
    s.transfer = c.transfer == Transfer::PQ ? ShaderTransfer::PQ : ShaderTransfer::SDR;
    s.fullRange = c.range == Range::Full;
    if (c.mastering.present && c.mastering.maxLuminance > 0)
        s.masteringPeakNits = static_cast<float>(c.mastering.maxLuminance);
    return s;
}

// NOT YET IMPLEMENTED — build step 3 (see docs/superpowers/specs/).
// The ST 2086 byte layout and the AMediaFormat keys land together with
// mediacodec_video.cc, because the only way to check either is to hand a real
// format to a real codec and read back what it accepted.
std::vector<uint8_t> hdrStaticInfo(const ColourInfo&) { return {}; }
void applyToFormat(const ColourInfo&, AMediaFormat*) {}

}  // namespace vp
