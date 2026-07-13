#include "AnvilPlayer/App/d3d12_dolby_vision.h"

#include "AnvilPlayer/App/color_math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace anvil::app {
namespace {

using anvil::playback::DoviMappingMethod;
using anvil::playback::DoviNlqMethod;
using anvil::playback::DolbyVisionFrameMetadata;
using anvil::playback::kDoviMaxPieces;
using anvil::playback::kDoviMaxPivots;
using anvil::playback::kDoviMaxTrimTargets;
using anvil::playback::kDoviMmrCoeffsPerOrder;
using anvil::playback::kDoviMmrMaxTerms;

float TextureSampleScale(const int bitDepth) noexcept {
    const int depth = std::clamp(bitDepth, 8, 16);
    const uint64_t codeMax = (uint64_t{1} << depth) - 1;
    const uint64_t shiftedMax = codeMax << (16 - depth);
    return shiftedMax ? static_cast<float>(65535.0 / static_cast<double>(shiftedMax)) : 1.0f;
}

float TrimDelta(const int value) noexcept {
    return std::clamp((static_cast<float>(value) - 2048.0f) / 4096.0f, -0.5f, 0.5f);
}

float OptionalTrimDelta(const int value) noexcept {
    return value == 0 ? 0.0f : TrimDelta(value);
}

bool NeutralTrim(const uint16_t slope, const uint16_t offset, const uint16_t power,
                 const uint16_t chroma, const uint16_t saturation, const int ms) noexcept {
    return slope == 2048 && offset == 2048 && power == 2048 && chroma == 2048 &&
           saturation == 2048 && ms == 2048;
}

void ApplyTrimCodes(DoviDisplayTrim& trim, const uint16_t slope, const uint16_t offset,
                    const uint16_t power, const uint16_t chroma,
                    const uint16_t saturation, const int ms) noexcept {
    trim.enabled = true;
    trim.slope = std::clamp(1.0f + TrimDelta(slope) * 0.45f, 0.75f, 1.25f);
    trim.offset = std::clamp(TrimDelta(offset) * 0.16f, -0.10f, 0.10f);
    trim.power = std::clamp(1.0f - TrimDelta(power) * 0.32f, 0.78f, 1.22f);
    trim.chromaWeight = std::clamp(1.0f + TrimDelta(chroma) * 0.25f, 0.88f, 1.12f);
    trim.saturation = std::clamp(1.0f + TrimDelta(saturation) * 0.60f, 0.75f, 1.30f);
    trim.msWeight = std::clamp(1.0f + TrimDelta(ms) * 0.20f, 0.90f, 1.10f);
    trim.midContrast = std::clamp(TrimDelta(ms) * 0.20f, -0.12f, 0.12f);
}

}  // namespace

DoviDisplayTrim SelectDoviDisplayTrim(const DolbyVisionFrameMetadata* metadata,
                                      const float targetPeakNits) noexcept {
    DoviDisplayTrim trim;
    if (!metadata || !metadata->valid) return trim;
    bool usedLevel2 = false;
    const bool useLevel8 = metadata->dmLevel8Present &&
        !NeutralTrim(metadata->dmLevel8TrimSlope, metadata->dmLevel8TrimOffset,
                     metadata->dmLevel8TrimPower, metadata->dmLevel8TrimChromaWeight,
                     metadata->dmLevel8TrimSaturationGain, metadata->dmLevel8MsWeight);
    if (useLevel8) {
        ApplyTrimCodes(trim, metadata->dmLevel8TrimSlope, metadata->dmLevel8TrimOffset,
                       metadata->dmLevel8TrimPower, metadata->dmLevel8TrimChromaWeight,
                       metadata->dmLevel8TrimSaturationGain, metadata->dmLevel8MsWeight);
        trim.source = DoviDisplayTrimSource::Level8;
        trim.midContrast = std::clamp(
            trim.midContrast + OptionalTrimDelta(metadata->dmLevel8TargetMidContrast) * 0.35f,
            -0.20f, 0.20f);
        trim.clip = std::clamp(OptionalTrimDelta(metadata->dmLevel8ClipTrim) * 0.40f,
                               -0.18f, 0.18f);
    } else if (metadata->dmLevel3Present && metadata->dmLevel8Present) {
        // Match the old D3D11 CMv4 rule. L2 is a CMv2.9 compatibility
        // derivative; when a CMv4 stream carries neutral L8 plus L3, L3 must
        // drive the scene instead of falling back to the much stronger L2.
    } else if (metadata->dmLevel2Present && metadata->dmLevel2Count > 0) {
        int selected = 0;
        float distance = std::numeric_limits<float>::max();
        const int count = std::min(metadata->dmLevel2Count, kDoviMaxTrimTargets);
        for (int index = 0; index < count; ++index) {
            const float candidate = Pq12CodeToNits(metadata->dmLevel2TargetMaxPq[index]);
            const float candidateDistance = std::abs(candidate - targetPeakNits);
            if (candidateDistance < distance) {
                distance = candidateDistance;
                selected = index;
            }
        }
        ApplyTrimCodes(trim, metadata->dmLevel2TrimSlope[selected],
                       metadata->dmLevel2TrimOffset[selected], metadata->dmLevel2TrimPower[selected],
                       metadata->dmLevel2TrimChromaWeight[selected],
                       metadata->dmLevel2TrimSaturationGain[selected],
                       metadata->dmLevel2MsWeight[selected]);
        trim.source = DoviDisplayTrimSource::Level2;
        usedLevel2 = true;
    }
    if (!usedLevel2 && metadata->dmLevel3Present) {
        const float middle = std::clamp(TrimDelta(metadata->dmLevel3AvgPqOffset) * 0.12f,
                                        -0.08f, 0.08f);
        const float contrast = std::clamp(
            (TrimDelta(metadata->dmLevel3MaxPqOffset) -
             TrimDelta(metadata->dmLevel3MinPqOffset)) * 0.10f,
            -0.07f, 0.07f);
        if (std::abs(middle) > 0.001f || std::abs(contrast) > 0.001f) {
            trim.enabled = true;
            trim.includesLevel3 = true;
            if (trim.source == DoviDisplayTrimSource::None) {
                trim.source = DoviDisplayTrimSource::Level3;
            }
            trim.midOffset = std::clamp(trim.midOffset + middle, -0.18f, 0.18f);
            trim.midContrast = std::clamp(trim.midContrast + contrast, -0.18f, 0.18f);
        }
    }
    return trim;
}

void FillDoviShaderConstants(DoviShaderConstantsPair& dc,
                             const std::size_t endpoint,
                             const NativeVideoFrame& frame,
                             const float targetPeakNits) noexcept {
    if (endpoint >= 2) return;

    // The final compositor consumes active-area bounds for every frame. Set a
    // neutral identity state before the metadata early return so ordinary
    // SDR/HDR10 video cannot inherit a zero-sized Dolby Vision active area.
    dc.trimA[endpoint][1] = 1.0f;
    dc.trimA[endpoint][3] = 1.0f;
    dc.trimB[endpoint][0] = 1.0f;
    dc.trimB[endpoint][1] = 1.0f;
    dc.trimB[endpoint][2] = 1.0f;
    dc.trimC[endpoint][2] = 0.0f;
    dc.trimC[endpoint][3] = 0.0f;
    dc.trimD[endpoint][0] = 1.0f;
    dc.trimD[endpoint][1] = 1.0f;
    dc.trimD[endpoint][2] = frame.color.IsHdr() ? 1000.0f : 100.0f;
    dc.trimD[endpoint][3] = frame.color.range ==
        anvil::playback::VideoColorRange::Full ? 1.0f : 0.0f;

    const bool p7 = frame.HasEnhancementD3D12Texture() && frame.enhancementDovi &&
                    frame.enhancementDovi->valid;
    const DolbyVisionFrameMetadata* source = p7
        ? frame.enhancementDovi.get()
        : (frame.dovi && frame.dovi->valid ? frame.dovi.get() : nullptr);
    if (!source) return;
    const DolbyVisionFrameMetadata& dovi = *source;
    for (int c = 0; c < 3; ++c) {
        const auto& curve = dovi.curves[c];
        const int numPivots = std::clamp(curve.numPivots, 2, kDoviMaxPivots);
        dc.curveMeta[endpoint][c] = static_cast<float>(numPivots);
        for (int pivot = 0; pivot < kDoviMaxPivots; ++pivot) {
            dc.pivots[endpoint][c][pivot / 3][pivot % 3] =
                pivot < numPivots ? curve.pivots[pivot] : 1.0f;
        }
        const int pieces = numPivots - 1;
        for (int piece = 0; piece < kDoviMaxPieces; ++piece) {
            dc.pieceMeta[endpoint][c][piece / 4][piece % 4] = piece < pieces
                ? static_cast<float>(static_cast<int>(curve.pieces[piece].method)) : -1.0f;
            if (piece >= pieces) continue;
            const auto& mapping = curve.pieces[piece];
            if (mapping.method == DoviMappingMethod::Polynomial) {
                dc.polyCoef[endpoint][c][piece][0] = mapping.polyCoef[0];
                dc.polyCoef[endpoint][c][piece][1] = mapping.polyCoef[1];
                dc.polyCoef[endpoint][c][piece][2] = mapping.polyCoef[2];
                dc.polyCoef[endpoint][c][piece][3] = static_cast<float>(mapping.polyOrder);
            } else if (mapping.method == DoviMappingMethod::Mmr) {
                dc.polyCoef[endpoint][c][piece][3] =
                    static_cast<float>(std::clamp(mapping.mmrOrder, 1, kDoviMmrMaxTerms));
                float flat[24]{};
                flat[0] = mapping.mmrConstant;
                int output = 1;
                for (int term = 0; term < mapping.mmrOrder && term < kDoviMmrMaxTerms; ++term) {
                    for (int coefficient = 0; coefficient < kDoviMmrCoeffsPerOrder; ++coefficient) {
                        if (output < 24) flat[output] = mapping.mmrCoef[term][coefficient];
                        ++output;
                    }
                }
                for (int group = 0; group < 6; ++group) {
                    for (int lane = 0; lane < 4; ++lane) {
                        dc.mmrCoef[endpoint][c][piece][group][lane] = flat[group * 4 + lane];
                    }
                }
            }
        }
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            dc.yccToRgb[endpoint][row][column] = dovi.yccToRgb[row * 3 + column];
            dc.rgbToLms[endpoint][row][column] = dovi.rgbToLms[row * 3 + column];
        }
        dc.yccOffset[endpoint][row] = dovi.yccOffset[row];
        dc.nlqParams[endpoint][row][0] = static_cast<float>(dovi.nlqOffset[row]);
        dc.nlqParams[endpoint][row][1] = static_cast<float>(dovi.nlqVdrInMax[row]);
        dc.nlqParams[endpoint][row][2] = static_cast<float>(dovi.nlqLinearDeadzoneSlope[row]);
        dc.nlqParams[endpoint][row][3] = static_cast<float>(dovi.nlqLinearDeadzoneThreshold[row]);
    }
    dc.curveMeta[endpoint][3] = static_cast<float>(65536.0 / 65535.0);
    dc.composerMeta[endpoint][0] = p7 &&
        !dovi.residualDisabled && dovi.nlqMethod == DoviNlqMethod::LinearDeadzone ? 1.0f : 0.0f;
    dc.composerMeta[endpoint][1] = static_cast<float>(static_cast<int>(dovi.nlqMethod));
    dc.composerMeta[endpoint][2] = static_cast<float>(std::clamp(dovi.elBitDepth, 8, 16));
    dc.composerMeta[endpoint][3] = static_cast<float>(std::clamp(dovi.vdrBitDepth, 8, 16));
    dc.composerScale[endpoint][0] = static_cast<float>(std::max(0, dovi.coefLog2Denom));
    dc.composerScale[endpoint][1] = TextureSampleScale(dovi.elBitDepth);
    dc.composerScale[endpoint][2] = static_cast<float>(std::clamp(dovi.blBitDepth, 8, 16));
    dc.composerScale[endpoint][3] = dovi.elSpatialResampling ? 1.0f : 0.0f;
    dc.signalMeta[endpoint][0] = p7 ? 2.0f : 1.0f;
    dc.signalMeta[endpoint][1] = static_cast<float>(dovi.profile);
    dc.signalMeta[endpoint][2] = static_cast<float>(dovi.compatibilityId);
    dc.signalMeta[endpoint][3] = TextureSampleScale(dovi.blBitDepth);

    const DoviDisplayTrim trim = SelectDoviDisplayTrim(source, targetPeakNits);
    dc.trimA[endpoint][0] = trim.enabled ? 1.0f : 0.0f;
    dc.trimA[endpoint][1] = trim.slope;
    dc.trimA[endpoint][2] = trim.offset;
    dc.trimA[endpoint][3] = trim.power;
    dc.trimB[endpoint][0] = trim.saturation;
    dc.trimB[endpoint][1] = trim.chromaWeight;
    dc.trimB[endpoint][2] = trim.msWeight;
    dc.trimB[endpoint][3] = trim.midOffset;
    dc.trimC[endpoint][0] = trim.midContrast;
    dc.trimC[endpoint][1] = trim.clip;
    dc.trimC[endpoint][2] = 0.0f;
    dc.trimC[endpoint][3] = 0.0f;
    dc.trimD[endpoint][0] = 1.0f;
    dc.trimD[endpoint][1] = 1.0f;
    if (dovi.dmLevel5Present && frame.width > 0 && frame.height > 0) {
        dc.trimC[endpoint][2] = std::clamp(
            static_cast<float>(dovi.dmLevel5LeftOffset) / frame.width, 0.0f, 1.0f);
        dc.trimC[endpoint][3] = std::clamp(
            static_cast<float>(dovi.dmLevel5TopOffset) / frame.height, 0.0f, 1.0f);
        dc.trimD[endpoint][0] = std::clamp(
            1.0f - static_cast<float>(dovi.dmLevel5RightOffset) / frame.width, 0.0f, 1.0f);
        dc.trimD[endpoint][1] = std::clamp(
            1.0f - static_cast<float>(dovi.dmLevel5BottomOffset) / frame.height, 0.0f, 1.0f);
    }
    dc.trimD[endpoint][2] = dovi.sourceMaxNits > 0.0f ? dovi.sourceMaxNits : 1000.0f;
    dc.trimD[endpoint][3] = dovi.blVideoFullRange ? 1.0f : 0.0f;
}

std::string_view D3D12DolbyVisionHlsl() noexcept {
    static constexpr char library[] = R"DOVI(
cbuffer DoviConstants : register(b1) {
    float4 doviPivots[2][3][3];
    float4 doviPieceMeta[2][3][2];
    float4 doviPolyCoef[2][3][8];
    float4 doviMmrCoef[2][3][8][6];
    float4 doviYccToRgb[2][3];
    float4 doviYccOffset[2];
    float4 doviRgbToLms[2][3];
    float4 doviCurveMeta[2];
    float4 doviNlqParams[2][3];
    float4 doviComposerMeta[2];
    float4 doviComposerScale[2];
    float4 doviSignalMeta[2];
    float4 doviTrimA[2];
    float4 doviTrimB[2];
    float4 doviTrimC[2];
    float4 doviTrimD[2];
};
float dovi_pivot(int f, int c, int i) {
    if (i >= 9) return 1.0;
    return doviPivots[f][c][i / 3][i % 3];
}
int dovi_num_pivots(int f, int c) { return clamp((int)(doviCurveMeta[f][c] + 0.5), 2, 9); }
int dovi_method(int f, int c, int s) { return s < 8 ? (int)doviPieceMeta[f][c][s / 4][s % 4] : -1; }
float dovi_round(float v) { return floor(v + 0.5); }
float dovi_round_signed(float v) { return v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5); }
float dovi_code16(float v) { return dovi_round(saturate(v) * 65535.0); }
float dovi_norm16(float v) { return saturate(v / 65535.0); }
int dovi_find_piece(int f, int c, float v) {
    int last = max(0, dovi_num_pivots(f, c) - 2);
    [unroll] for (int i = 0; i < 8; ++i) {
        if (i >= last || v < dovi_pivot(f, c, i + 1)) return i;
    }
    return last;
}
float dovi_mmr(int f, int c, int s, float3 rgb) {
    float m[24];
    [unroll] for (int q = 0; q < 6; ++q) {
        m[q*4] = doviMmrCoef[f][c][s][q].x; m[q*4+1] = doviMmrCoef[f][c][s][q].y;
        m[q*4+2] = doviMmrCoef[f][c][s][q].z; m[q*4+3] = doviMmrCoef[f][c][s][q].w;
    }
    int order = clamp((int)(doviPolyCoef[f][c][s].w + 0.5), 1, 3);
    float4 cross = float4(rgb.x*rgb.y, rgb.x*rgb.z, rgb.y*rgb.z, rgb.x*rgb.y*rgb.z);
    float result = m[0] + dot(float3(m[1],m[2],m[3]), rgb) + dot(float4(m[4],m[5],m[6],m[7]), cross);
    if (order >= 2) {
        float3 rgb2 = rgb*rgb; float4 cross2 = cross*cross;
        result += dot(float3(m[8],m[9],m[10]),rgb2) + dot(float4(m[11],m[12],m[13],m[14]),cross2);
        if (order >= 3) result += dot(float3(m[15],m[16],m[17]),rgb2*rgb) + dot(float4(m[18],m[19],m[20],m[21]),cross2*cross);
    }
    return result;
}
float dovi_map_component(int f, int c, float3 signal, bool p7) {
    float v = signal[c]; int s = dovi_find_piece(f,c,v); int method = dovi_method(f,c,s); float mapped = v;
    if (method == 0) {
        float3 k = doviPolyCoef[f][c][s].xyz;
        if (p7) {
            float bits = clamp(doviComposerScale[f].z,8.0,16.0); float code = saturate(v)*(exp2(bits)-1.0);
            mapped = dovi_norm16(dovi_round(clamp((k.x*exp2(20.0)+k.y*code*exp2(20.0-bits)+k.z*code*code*exp2(20.0-2.0*bits))/16.0,0.0,65535.0)));
        } else mapped = k.x + k.y*v + k.z*v*v;
    } else if (method == 1) mapped = dovi_mmr(f,c,s,signal);
    return p7 ? saturate(mapped) : clamp(mapped,dovi_pivot(f,c,0),dovi_pivot(f,c,dovi_num_pivots(f,c)-1));
}
float3 dovi_map(int f, float3 signal, bool p7) {
    return float3(dovi_map_component(f,0,signal,p7), dovi_map_component(f,1,signal,p7), dovi_map_component(f,2,signal,p7));
}
float3 dovi_pq_to_nits(float3 v) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0,c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 p=pow(saturate(v),1.0/m2); return 10000.0*pow(max(p-c1,0.0)/max(c2-c3*p,0.000001),1.0/m1);
}
float3 dovi_nits_to_pq(float3 n) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0,c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 p=pow(saturate(n/10000.0),m1); return pow((c1+c2*p)/max(1.0+c3*p,0.000001),m2);
}
float3 dovi_decode_reshaped(int f, float3 reshaped) {
    float3 mapped=reshaped-doviYccOffset[f].xyz*doviCurveMeta[f].w;
    float3 pq=float3(dot(doviYccToRgb[f][0].xyz,mapped),dot(doviYccToRgb[f][1].xyz,mapped),dot(doviYccToRgb[f][2].xyz,mapped));
    float3 linearRgb=dovi_pq_to_nits(pq)/10000.0;
    float3 lms=float3(dot(doviRgbToLms[f][0].xyz,linearRgb),dot(doviRgbToLms[f][1].xyz,linearRgb),dot(doviRgbToLms[f][2].xyz,linearRgb));
    // Dolby's LMS-to-BT.2020 matrix. Keep the full reference coefficients:
    // even small row-sum errors become a visible neutral-axis tint after PQ.
    float3 bt2020=mul(float3x3(3.06441879,-2.16597676,0.10155818,-0.65612108,1.78554118,-0.12943749,0.01736321,-0.04725154,1.03004253),lms);
    return dovi_nits_to_pq(max(bt2020,0.0)*10000.0);
}
float3 dovi_decode_single_layer(int f, float3 signal) {
    return dovi_decode_reshaped(f,dovi_map(f,signal,false));
}
float dovi_inverse_nlq(int f, float sample, int c) {
    if (doviComposerMeta[f].x<0.5 || doviComposerMeta[f].y!=0.0) return 0.0;
    float elBits=clamp(doviComposerMeta[f].z,8.0,16.0), elMax=exp2(elBits)-1.0;
    float e=dovi_round(saturate(sample*doviComposerScale[f].y)*elMax), rr=e-doviNlqParams[f][c].x;
    if (abs(rr)<0.5) return 0.0;
    float signValue=rr<0.0?-1.0:1.0, bitScale=exp2(10.0-elBits);
    float linearResidual=signValue*max(abs(rr)*2.0-1.0,0.0)*bitScale;
    float dq=linearResidual*doviNlqParams[f][c].z+doviNlqParams[f][c].w*exp2(11.0-elBits)*signValue;
    float limit=max(doviNlqParams[f][c].y*exp2(11.0-elBits),0.0);
    return dovi_round_signed(clamp(dq,-limit,limit)/exp2(doviComposerScale[f].x-5.0-elBits));
}
float dovi_vdr_code(int f, float code16) {
    float bits=clamp(doviComposerMeta[f].w,8.0,16.0), maxCode=exp2(bits)-1.0, stepCode=exp2(16.0-bits);
    return saturate(dovi_round(clamp(code16,0.0,65535.0)/stepCode)/max(maxCode,1.0));
}
float3 dovi_compose_p7_fel(int f, float3 blPixel, float3 blChroma, float3 fel) {
    blPixel=saturate(blPixel*doviSignalMeta[f].w); blChroma=saturate(blChroma*doviSignalMeta[f].w);
    float3 mapped=float3(dovi_map_component(f,0,blPixel,true),dovi_map_component(f,1,blChroma,true),dovi_map_component(f,2,blChroma,true));
    float3 mappedCode=float3(dovi_code16(mapped.x),dovi_code16(mapped.y),dovi_code16(mapped.z));
    float3 residual=float3(dovi_inverse_nlq(f,fel.x,0),dovi_inverse_nlq(f,fel.y,1),dovi_inverse_nlq(f,fel.z,2));
    float3 merged=clamp(mappedCode+residual,0.0,65535.0);
    return float3(dovi_vdr_code(f,merged.x),dovi_vdr_code(f,merged.y),dovi_vdr_code(f,merged.z));
}
float3 dovi_apply_display_trim(int f, float3 nits, float targetPeak) {
    if (doviTrimA[f].x<0.5) return nits;
    // Keep the old known-good D3D11 domain exactly: creative trim parameters
    // operate on normalized ST 2084 code values, not on nits divided by the
    // display peak. Linear-nits normalization makes slope/power scene changes
    // explode into alternating over-bright and crushed frames.
    float3 src=saturate(dovi_nits_to_pq(max(nits,0.0)));
    float l=saturate(dot(src,float3(0.2627,0.6780,0.0593)));
    float toeMask=smoothstep(0.02,0.18,l);
    float shoulderMask=1.0-smoothstep(0.70,0.98,l);
    float midMask=toeMask*shoulderMask;
    float outputLuma=saturate(l+doviTrimB[f].w*midMask);
    outputLuma=saturate(0.45+(outputLuma-0.45)*(1.0+doviTrimC[f].x*midMask));
    outputLuma=saturate((outputLuma-0.5)*max(doviTrimA[f].y,0.01)+0.5+doviTrimA[f].z*toeMask);
    outputLuma=pow(max(outputLuma,0.0),max(doviTrimA[f].w,0.05));
    float clipMask=smoothstep(0.62,1.0,outputLuma);
    outputLuma=saturate(outputLuma+doviTrimC[f].y*clipMask*(1.0-outputLuma)*0.45);
    float3 color=saturate(src*(outputLuma/max(l,0.0001)));
    float gray=saturate(dot(color,float3(0.2627,0.6780,0.0593)));
    float saturation=clamp(doviTrimB[f].x*lerp(1.0,doviTrimB[f].y,0.25)*lerp(1.0,doviTrimB[f].z,0.10),0.75,1.25);
    color=saturate(gray.xxx+(color-gray.xxx)*saturation);
    return dovi_pq_to_nits(color);
}
)DOVI";
    return library;
}

}  // namespace anvil::app
