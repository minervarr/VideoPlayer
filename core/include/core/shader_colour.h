#pragma once

// What the container's colour description means to the RENDERER.
//
// core/mkv.h's ColourInfo came out of the file, unvalidated, exactly as
// written (rule 3). It has to reach two very different places, and they must
// not disagree:
//
//   1. the DECODER, so the codec knows what it is producing and any
//      device-side processing leaves it alone. That translation is
//      necessarily per-platform — AMediaFormat's HDR static-info keys on
//      Android, the Vulkan video profile elsewhere — and lives with the host
//      that needs it, next to the decoder it configures. See
//      platform/android/codec/hdr_metadata.hh for Android's.
//
//   2. the SHADER: the Y'CbCr matrix, the transfer function and the range.
//      That is the same answer on every platform, so it lives here.
//
// The two used to be one header under platform/android/, on the reasoning
// that splitting them is how a player ends up decoding as BT.2020 and drawing
// as BT.709. The reasoning still holds and the pairing is now stated in both
// files instead of implied by a directory: change either half looking at the
// other. What forced the split is that gui/ needs this half and must not
// reach into platform/android/ to get it (rule 2) — which is also what stops
// a second host from ever building.
//
// Pure functions over ColourInfo: no OS header, no Vulkan, nothing to mock.

#include "core/mkv.h"

namespace vp {

// Which Y'CbCr -> RGB matrix the sampler's conversion applies.
enum class ShaderMatrix : int { BT709 = 0, BT2020NCL = 1 };
// Which transfer function the fragment stage undoes. A sampler cannot do this
// one, which is why it is a separate decision from the matrix.
enum class ShaderTransfer : int { SDR = 0, PQ = 1 };

struct ShaderColour {
    ShaderMatrix   matrix    = ShaderMatrix::BT709;
    ShaderTransfer transfer  = ShaderTransfer::SDR;
    bool           fullRange = false;
    // Peak the content was graded for, cd/m^2. From MasteringMetadata when
    // present; 1000 when it is not, which is what the overwhelming majority of
    // HDR10 masters actually use. This is the ONE default in the whole colour
    // path, and it is a tone-mapping parameter rather than a claim about the
    // file's colour.
    float          masteringPeakNits = 1000.0f;
};

ShaderColour forShader(const ColourInfo& c);

}  // namespace vp
