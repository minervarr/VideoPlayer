#include "core/shader_colour.h"

namespace vp {

ShaderColour forShader(const ColourInfo& c) {
    ShaderColour s;
    s.matrix = c.matrix == MatrixCoeffs::BT2020NCL ? ShaderMatrix::BT2020NCL
                                                   : ShaderMatrix::BT709;
    s.transfer = c.transfer == Transfer::PQ ? ShaderTransfer::PQ : ShaderTransfer::SDR;
    // Matroska's Range::Unspecified means limited for video, which is what
    // essentially every encoder writes and what the decoder will produce.
    s.fullRange = c.range == Range::Full;
    if (c.mastering.present && c.mastering.maxLuminance > 0)
        s.masteringPeakNits = static_cast<float>(c.mastering.maxLuminance);
    return s;
}

}  // namespace vp
