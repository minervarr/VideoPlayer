#pragma once

// ANDROID's half of the container's colour description: what ColourInfo means
// to AMediaCodec.
//
// The other half — what it means to the SHADER — is core/shader_colour.h, and
// the two must not disagree: splitting them is how a player ends up decoding
// as BT.2020 and drawing as BT.709. They were one header until gui/ needed the
// shader half and could not reach into platform/android/ to get it (rule 2).
// Change either looking at the other; each says so.

#include <cstdint>
#include <vector>

#include "core/mkv.h"
#include "core/shader_colour.h"   // the other half; keep the two in step

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

}  // namespace vp
