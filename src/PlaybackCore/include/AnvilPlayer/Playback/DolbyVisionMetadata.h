#pragma once

#include <cstdint>

namespace anvil::playback {

// Dolby Vision per-frame metadata extracted from FFmpeg's AVDOVIMetadata and
// reshaped into a renderer-friendly form (no libavutil dependency, all
// coefficients pre-converted to floats). This structure is uploaded to the
// GPU constant buffer each frame to drive the IPTPQc2 reshaping shader.
//
// Reference: libplacebo dovi_filter, FFmpeg libavutil/dovi_meta.h,
// Dolby Vision Bitstreams Within the ISO Base Media File Format v2.1.2.

constexpr int kDoviMaxPieces = 8;        // AV_DOVI_MAX_PIECES
constexpr int kDoviNumComponents = 3;    // I, Ct, Cp
constexpr int kDoviMaxPivots = kDoviMaxPieces + 1;          // 9
constexpr int kDoviMmrCoeffsPerOrder = 7;                    // 7 coefficients per MMR order term
constexpr int kDoviMmrMaxTerms = 3;                          // order - 1, i.e. up to 3 terms
constexpr int kDoviMaxTrimTargets = 4;                       // compact L2 target trims kept for display mapping

// Reshaping method for a single piece-wise segment of one component.
enum class DoviMappingMethod {
    None = -1,
    Polynomial = 0,   // AV_DOVI_MAPPING_POLYNOMIAL
    Mmr = 1,          // AV_DOVI_MAPPING_MMR
};

enum class DoviNlqMethod {
    None = -1,
    LinearDeadzone = 0,  // AV_DOVI_NLQ_LINEAR_DZ
};

// One piece-wise segment of a reshaping curve. Spans the value range between
// pivots[i] and pivots[i+1] (normalized to [0,1]).
struct DoviReshapingPiece {
    DoviMappingMethod method = DoviMappingMethod::None;

    // Polynomial (order 1 or 2): y = c0 + c1*x + c2*x^2
    // Coefficients already real-valued (divided by 2^coef_log2_denom by the decoder).
    int polyOrder = 0;                  // [1, 2]
    float polyCoef[3] = {0, 0, 0};      // x^0, x^1, x^2

    // MMR (multi-modulation regression, order 1..3).
    // Operates in RGB space (after ycc_to_rgb). Result is sum over terms.
    int mmrOrder = 0;                                   // [1, 3]
    float mmrConstant = 0.0f;                           // per-segment constant
    float mmrCoef[kDoviMmrMaxTerms][kDoviMmrCoeffsPerOrder] = {};
};

// One component (I, Ct, or Cp) of the reshaping curve: a piece-wise function
// with up to 8 segments defined by sorted pivot values.
struct DoviReshapingCurve {
    int numPivots = 0;                                  // [2, 9]
    float pivots[kDoviMaxPivots] = {0};                 // sorted ascending, normalized to [0,1]
    DoviReshapingPiece pieces[kDoviMaxPieces] = {};
};

// Per-frame Dolby Vision metadata, ready for GPU upload.
//
// The reshaping pipeline is:
//   1. Decode input YCbCr (IPT-like, "YCC" in DV terminology).
//   2. Optionally PQ-linearize (for CMv4.0 / certain profiles).
//   3. Per-component piece-wise reshape (poly or MMR).
//   4. Apply ycc_to_rgb matrix + offset to get BT.2020 PQ RGB.
//
// For Profile 5/8 (single layer, no EL) only steps 1, 3, 4 apply.
// For Profile 7 (BL+EL) step 3 is preceded by NLQ inverse-quantization and
// EL residual merge (handled by a separate struct, not yet used by the test
// source which is Profile 5 with el_present_flag=0).
struct DolbyVisionFrameMetadata {
    // Configuration record (from AVDOVIDecoderConfigurationRecord / stream tags).
    int profile = 0;                    // dv_profile (e.g. 5, 7, 8)
    int level = 0;                      // dv_level
    int compatibilityId = 0;            // dv_bl_signal_compatibility_id
    bool elPresent = false;             // el_present_flag
    bool blPresent = false;             // bl_present_flag

    // RPU header (from AVDOVIRpuDataHeader).
    int blBitDepth = 0;                 // [8, 16], typically 10
    int elBitDepth = 0;                 // [8, 16], typically 10
    int vdrBitDepth = 0;                // [8, 16], typically 12 (reshape output precision)
    bool blVideoFullRange = false;      // bl_video_full_range_flag (overrides container color_range for DV)
    bool vdrRpuNormalizedIdc = false;   // vdr_rpu_normalized_idc == 1
    int coefLog2Denom = 0;              // original fixed-point denominator (informational; coefs already real)
    bool residualDisabled = true;       // disable_residual_flag
    bool elSpatialResampling = false;   // el_spatial_resampling_filter_flag

    // Data mapping: 3 per-component piece-wise reshaping curves.
    DoviReshapingCurve curves[kDoviNumComponents] = {};

    // Profile 7 enhancement-layer residual metadata. NLQ values are kept in
    // their FFmpeg/bitstream fixed-point domain so the renderer can mirror the
    // composer arithmetic before normalizing back to shader floats.
    DoviNlqMethod nlqMethod = DoviNlqMethod::None;
    uint32_t nlqNumXPartitions = 0;
    uint32_t nlqNumYPartitions = 0;
    uint16_t nlqOffset[kDoviNumComponents] = {};
    uint64_t nlqVdrInMax[kDoviNumComponents] = {};
    uint64_t nlqLinearDeadzoneSlope[kDoviNumComponents] = {};
    uint64_t nlqLinearDeadzoneThreshold[kDoviNumComponents] = {};
    uint16_t nlqPivots[2] = {};

    // Color metadata (from AVDOVIColorMetadata).
    // ycc_to_rgb: applied to the reshaped YCC signal to recover RGB.
    // rgb_to_lms: applied after PQ linearization for CMv4.0 IPT reshaping
    //             (not used by CMv2.9 Profile 5; reserved for future use).
    float yccToRgb[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};    // row-major 3x3
    float yccOffset[3] = {0, 0, 0};                      // neutral offset (pre-reshape)
    float rgbToLms[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};    // row-major 3x3 (CMv4.0)

    // Source display peak metadata from AVDOVIColorMetadata. PQ codes are
    // 12-bit ST 2084 values; nits are precomputed for renderer tone mapping.
    uint16_t sourceMinPq = 0;
    uint16_t sourceMaxPq = 0;
    float sourceMinNits = 0.0f;
    float sourceMaxNits = 0.0f;

    // Dolby display-management extension blocks. These carry the dynamic trim
    // metadata (for example CMv4 L3/L8) that is distinct from the BL reshaping
    // curve above. The renderer fallback does not consume every field yet, but
    // the decoder logs these values so we can verify that frame-varying dynamic
    // metadata is present and changing.
    int dmMetadataId = 0;
    int sceneRefreshFlag = 0;
    int dmExtensionBlockCount = 0;
    uint64_t dmLevelMaskLow = 0;         // Levels 0..63 when present.
    bool dmLevel1Present = false;
    bool dmLevel2Present = false;
    bool dmLevel3Present = false;
    bool dmLevel5Present = false;
    bool dmLevel8Present = false;
    bool dmLevel254Present = false;
    bool dmLevel255Present = false;
    int dmLevel2Count = 0;
    int dmLevel8Count = 0;
    uint16_t dmLevel1MinPq = 0;
    uint16_t dmLevel1MaxPq = 0;
    uint16_t dmLevel1AvgPq = 0;
    uint16_t dmLevel2TargetMaxPq[kDoviMaxTrimTargets] = {};
    uint16_t dmLevel2TrimSlope[kDoviMaxTrimTargets] = {};
    uint16_t dmLevel2TrimOffset[kDoviMaxTrimTargets] = {};
    uint16_t dmLevel2TrimPower[kDoviMaxTrimTargets] = {};
    uint16_t dmLevel2TrimChromaWeight[kDoviMaxTrimTargets] = {};
    uint16_t dmLevel2TrimSaturationGain[kDoviMaxTrimTargets] = {};
    int16_t dmLevel2MsWeight[kDoviMaxTrimTargets] = {};
    uint16_t dmLevel3MinPqOffset = 0;
    uint16_t dmLevel3MaxPqOffset = 0;
    uint16_t dmLevel3AvgPqOffset = 0;
    uint16_t dmLevel5LeftOffset = 0;
    uint16_t dmLevel5RightOffset = 0;
    uint16_t dmLevel5TopOffset = 0;
    uint16_t dmLevel5BottomOffset = 0;
    uint8_t dmLevel8TargetDisplayIndex = 0;
    uint16_t dmLevel8TrimSlope = 0;
    uint16_t dmLevel8TrimOffset = 0;
    uint16_t dmLevel8TrimPower = 0;
    uint16_t dmLevel8TrimChromaWeight = 0;
    uint16_t dmLevel8TrimSaturationGain = 0;
    uint16_t dmLevel8MsWeight = 0;
    uint16_t dmLevel8TargetMidContrast = 0;
    uint16_t dmLevel8ClipTrim = 0;
    uint64_t dynamicMetadataFingerprint = 0;

    bool valid = false;                 // true if metadata was successfully extracted for this frame
};

}  // namespace anvil::playback
