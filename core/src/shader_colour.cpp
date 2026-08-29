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
    // Unstated stays unstated: the platform's suggestion is a better answer
    // than a guess of ours, and it is the previously tested behaviour.
    s.sitingHorz = c.sitingHorz == ChromaSiting::Collocated ? ShaderSiting::Collocated
                 : c.sitingHorz == ChromaSiting::Half       ? ShaderSiting::Half
                                                            : ShaderSiting::Unstated;
    s.sitingVert = c.sitingVert == ChromaSiting::Collocated ? ShaderSiting::Collocated
                 : c.sitingVert == ChromaSiting::Half       ? ShaderSiting::Half
                                                            : ShaderSiting::Unstated;

    if (c.mastering.present && c.mastering.maxLuminance > 0)
        s.masteringPeakNits = static_cast<float>(c.mastering.maxLuminance);

    // MaxCLL, when the file states it and it is lower.
    //
    // Mastering luminance is a property of the GRADING MONITOR — "this was
    // graded on a 1000-nit display" — and says nothing about whether anything
    // in the picture is that bright. MaxCLL is the brightest pixel actually in
    // the file. Tone-mapping toward the monitor's peak when the content tops
    // out well below it compresses highlights that never needed compressing:
    // on a 4000-nit master whose content reaches 600, every specular
    // reflection is pushed down a curve built for four thousand.
    //
    // Only ever TIGHTENS, and only from a number the container stated. A file
    // with no MaxCLL, or one whose MaxCLL exceeds its mastering peak (which
    // happens, and means the grade clipped), keeps the mastering value.
    if (c.maxCLL > 0 && static_cast<float>(c.maxCLL) < s.masteringPeakNits)
        s.masteringPeakNits = static_cast<float>(c.maxCLL);
    return s;
}

}  // namespace vp
