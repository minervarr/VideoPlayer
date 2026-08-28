#pragma once

// Two translations of the SAME facts, kept side by side on purpose.
//
// core/mkv.h's ColourInfo came out of the container. It has to reach two very
// different places, and they must not disagree:
//
//   1. the DECODER, as AMediaFormat HDR static-info keys, so the codec knows
//      what it is producing (and so any device-side processing leaves it
//      alone);
//   2. the SHADER, as the matrix and transfer the video fragment stage runs.
//
// Splitting these across two files is how a player ends up decoding as BT.2020
// and drawing as BT.709. They live in one header so that a change to either
// is made looking at the other.

#include <cstdint>
#include <vector>

#include "core/mkv.h"

struct AMediaFormat;

namespace vp {

// Writes COLOR_STANDARD / COLOR_TRANSFER / COLOR_RANGE and, when the container
// carried it, the HDR static info blob (SMPTE ST 2086 mastering display +
// MaxCLL/MaxFALL, in the byte layout AMEDIAFORMAT_KEY_HDR_STATIC_INFO wants).
// Silently writes nothing for values the container left Unspecified: an
// invented default here is a guess dressed as metadata (CLAUDE.md rule 3).
void applyToFormat(const ColourInfo& c, AMediaFormat* fmt);

// The ST 2086 blob itself, exposed separately because it is pure byte-layout
// arithmetic over ColourInfo and is therefore the half that can be tested
// without a device.
std::vector<uint8_t> hdrStaticInfo(const ColourInfo& c);

// ── The shader's half ─────────────────────────────────────────────────────

// Which Y'CbCr -> RGB matrix the fragment stage uses. Mirrors the constant in
// shaders_src/video_frag.slang; the two are compared in one place and only
// here.
enum class ShaderMatrix : int { BT709 = 0, BT2020NCL = 1 };
enum class ShaderTransfer : int { SDR = 0, PQ = 1 };

struct ShaderColour {
    ShaderMatrix   matrix   = ShaderMatrix::BT709;
    ShaderTransfer transfer = ShaderTransfer::SDR;
    bool           fullRange = false;
    // Peak the content was graded for, cd/m^2. From MasteringMetadata when
    // present; 1000 when it is not, which is what the overwhelming majority of
    // HDR10 masters actually use. This is the ONE default in this file, and it
    // is a tone-mapping parameter rather than a claim about the file's colour.
    float          masteringPeakNits = 1000.0f;
};

ShaderColour forShader(const ColourInfo& c);

}  // namespace vp
