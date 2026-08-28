// video_frag — Y'CbCr to display, for one HDR10 frame.
//
// This shader lives in THIS repo and not in vk_canvas, deliberately. The
// engine draws images, text and shapes for any consumer; the BT.2020 matrix
// and the PQ transfer are this application's colour path, and putting them in
// the engine would make every other consumer carry them.
//
// It ends by calling the ENGINE's output encode, exactly like vk_canvas's own
// image_frag.slang does, so that what this writes matches the swapchain the
// engine actually resolved (see framework/vk_canvas/core/output_target.hh and
// USAGE_hdr_output.md). Never encode PQ here directly: OUTPUT_ENCODE is a
// specialization constant precisely so a device that fell back to the SDR pin
// still gets a correct picture rather than a dark one.

// Matches vp::ShaderMatrix / vp::ShaderTransfer in
// platform/android/codec/hdr_metadata.hh. Those two enums and these two
// constants are one decision written twice; change them together.
static const int MATRIX_BT709     = 0;
static const int MATRIX_BT2020NCL = 1;
static const int TRANSFER_SDR     = 0;
static const int TRANSFER_PQ      = 1;

[[vk::binding(0, 0)]] Sampler2D videoY;    // the imported AHardwareBuffer,
                                            // sampled through the Y'CbCr
                                            // conversion the engine created

struct PushConstants {
    float2 scale;
    float2 offset;
    int    matrixId;
    int    transferId;
    int    fullRange;
    float  masteringPeakNits;
    float  displayPeakNits;
    float  _pad0, _pad1, _pad2;
};
[[vk::push_constant]] PushConstants pc;

// ST 2084 EOTF: code value -> absolute luminance, normalized so 1.0 is
// 10000 cd/m^2. This is the INVERSE of what the output encode does; the two
// are not the same function and swapping them is a picture that looks
// plausibly wrong rather than obviously wrong.
float3 pqToLinear(float3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 ep = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(ep - c1, 0.0);
    float3 den = c2 - c3 * ep;
    return pow(num / max(den, 1e-6), 1.0 / m1);
}

float3 ycbcrToRgb(float3 ycbcr, int matrixId, bool fullRange) {
    // Limited range at 10 bits is 64..940 for luma and 64..960 for chroma,
    // expressed here as the normalized values the sampler already produced.
    float y  = ycbcr.x;
    float cb = ycbcr.y - 0.5;
    float cr = ycbcr.z - 0.5;
    if (!fullRange) {
        y  = (y - 16.0 / 255.0) * (255.0 / 219.0);
        cb = cb * (255.0 / 224.0);
        cr = cr * (255.0 / 224.0);
    }

    // Non-constant-luminance coefficients. BT.2020's differ from BT.709's by
    // enough to tint skin tones visibly, which is the failure mode that gets
    // shipped because it does not look like a bug.
    float kr = (matrixId == MATRIX_BT2020NCL) ? 0.2627 : 0.2126;
    float kb = (matrixId == MATRIX_BT2020NCL) ? 0.0593 : 0.0722;
    float kg = 1.0 - kr - kb;

    float r = y + 2.0 * (1.0 - kr) * cr;
    float b = y + 2.0 * (1.0 - kb) * cb;
    float g = (y - kr * r - kb * b) / kg;
    return float3(r, g, b);
}

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target {
    float3 ycbcr = videoY.Sample(input.uv).rgb;
    float3 rgb = ycbcrToRgb(ycbcr, pc.matrixId, pc.fullRange != 0);

    if (pc.transferId == TRANSFER_PQ) {
        // To absolute luminance, then to the display's range. Scene values
        // above what the panel can show are compressed rather than clipped;
        // clipping specular highlights is what makes HDR content read as
        // "grey" on a phone.
        float3 linearNits = pqToLinear(rgb) * 10000.0;
        float peak = max(pc.displayPeakNits, 1.0);
        // Reinhard with a knee at the display peak. Deliberately the simplest
        // curve that is monotonic and preserves hue; a better operator is a
        // real decision to make against real content, not a default to guess.
        linearNits = linearNits / (1.0 + linearNits / peak);
        rgb = linearNits / peak;
    }

    // The engine's OUTPUT_ENCODE takes it from here — sRGB, scRGB linear or
    // ST 2084, whichever swapchain resolve_output_target() actually landed on.
    return float4(rgb, 1.0);
}
