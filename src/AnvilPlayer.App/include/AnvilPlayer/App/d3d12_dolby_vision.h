#pragma once

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"

#include <cstddef>
#include <string_view>

namespace anvil::app {

// One upload contains both interpolation endpoints. The renderer uses element
// zero; the tensor preprocessor uses both. Every leaf is a float4 so the C++
// layout is identical to the HLSL cbuffer layout.
struct DoviShaderConstantsPair {
    float pivots[2][3][3][4] = {};
    float pieceMeta[2][3][2][4] = {};
    float polyCoef[2][3][8][4] = {};
    float mmrCoef[2][3][8][6][4] = {};
    float yccToRgb[2][3][4] = {};
    float yccOffset[2][4] = {};
    float rgbToLms[2][3][4] = {};
    float curveMeta[2][4] = {};
    float nlqParams[2][3][4] = {};
    // residual enabled, NLQ method, EL bit depth, VDR bit depth.
    float composerMeta[2][4] = {};
    // coefficient denominator, EL sample scale, BL bit depth, spatial filter.
    float composerScale[2][4] = {};
    // enabled (0/1/2), profile, compatibility id, BL sample scale.
    float signalMeta[2][4] = {};
    // enabled, slope, offset, power.
    float trimA[2][4] = {};
    // saturation, chroma weight, MS weight, middle offset.
    float trimB[2][4] = {};
    // middle contrast, clip, active-area left, active-area top.
    float trimC[2][4] = {};
    // active-area right, active-area bottom, source max nits, BL full range.
    float trimD[2][4] = {};
};

static_assert(sizeof(DoviShaderConstantsPair) == 402 * sizeof(float) * 4,
              "Dolby Vision constants must exactly match the HLSL float4 layout");
static_assert(offsetof(DoviShaderConstantsPair, yccToRgb) == 366 * sizeof(float) * 4);
static_assert(offsetof(DoviShaderConstantsPair, signalMeta) == 392 * sizeof(float) * 4);
static_assert(offsetof(DoviShaderConstantsPair, trimD) == 400 * sizeof(float) * 4);

enum class DoviDisplayTrimSource {
    None,
    Level2,
    Level3,
    Level8,
};

struct DoviDisplayTrim {
    bool enabled = false;
    DoviDisplayTrimSource source = DoviDisplayTrimSource::None;
    bool includesLevel3 = false;
    float slope = 1.0f;
    float offset = 0.0f;
    float power = 1.0f;
    float saturation = 1.0f;
    float chromaWeight = 1.0f;
    float msWeight = 1.0f;
    float midOffset = 0.0f;
    float midContrast = 0.0f;
    float clip = 0.0f;
};

DoviDisplayTrim SelectDoviDisplayTrim(
    const anvil::playback::DolbyVisionFrameMetadata* metadata,
    float targetPeakNits) noexcept;

void FillDoviShaderConstants(DoviShaderConstantsPair& constants,
                             std::size_t endpoint,
                             const NativeVideoFrame& frame,
                             float targetPeakNits) noexcept;

// Mapping/composer library. Callers provide the four textures and sampling
// policy, then call dovi_decode_single_layer or dovi_compose_p7_fel.
std::string_view D3D12DolbyVisionHlsl() noexcept;

}  // namespace anvil::app
