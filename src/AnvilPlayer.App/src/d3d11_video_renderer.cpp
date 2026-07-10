#include "AnvilPlayer/App/d3d11_video_renderer.h"

#include "AnvilPlayer/App/color_math.h"
#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/ui_draw.h"

#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>

#include <process.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace anvil::app {

struct D3D11QueuedVideoFrame {
    explicit D3D11QueuedVideoFrame(const NativeVideoFrame& source) : frame(source) {}
    explicit D3D11QueuedVideoFrame(NativeVideoFrame&& source) noexcept : frame(std::move(source)) {}

    NativeVideoFrame frame;
};

static_assert(std::is_nothrow_move_constructible_v<NativeVideoFrame>);

using anvil::playback::LogLevel;
using anvil::playback::DoviMappingMethod;
using anvil::playback::DoviNlqMethod;
using anvil::playback::kDoviMaxPivots;
using anvil::playback::kDoviMaxPieces;
using anvil::playback::kDoviMmrCoeffsPerOrder;
using anvil::playback::kDoviMmrMaxTerms;
using anvil::playback::VideoColorMetadata;
using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

namespace {

// Releasing the last decoded-frame reference can transitively free large CPU
// planes, AVFrames, decoder surfaces, and D3D objects. The window thread must
// therefore only transfer ownership, never destroy a replaced mailbox frame.
//
// This service is deliberately process-lifetime: its object and worker are not
// statically destroyed, so shutdown cannot race a static destructor and no
// WndProc ever waits for it. The reservation count bounds all envelopes,
// including frames currently being rendered, waiting in a renderer mailbox,
// and queued for retirement. If the bound or an allocation is unavailable,
// the new frame is simply not submitted and the existing mailbox is untouched.
constexpr std::size_t kFrameRetirementCapacity = 8;

class FrameRetirementService {
public:
    bool Start() noexcept {
        const uintptr_t threadHandle = _beginthreadex(
            nullptr,
            0,
            &FrameRetirementService::ThreadEntry,
            this,
            0,
            nullptr);
        if (threadHandle == 0) {
            return false;
        }
        CloseHandle(reinterpret_cast<HANDLE>(threadHandle));
        return true;
    }

    D3D11QueuedVideoFrame* TryAcquire(const NativeVideoFrame& frame) noexcept {
        if (!TryReserve()) {
            return nullptr;
        }

        D3D11QueuedVideoFrame* queuedFrame = nullptr;
        try {
            queuedFrame = new (std::nothrow) D3D11QueuedVideoFrame(frame);
        } catch (...) {
            // A throwing frame-field copy leaves the source intact and the
            // mailbox unchanged. Partially copied references are not final
            // because the source still owns them.
        }
        if (!queuedFrame) {
            ReleaseReservation();
        }
        return queuedFrame;
    }

    D3D11QueuedVideoFrame* TryAcquireForRetirement(NativeVideoFrame&& frame) noexcept {
        if (!TryReserve()) {
            return nullptr;
        }

        // Allocation is sequenced before object initialization. Combined with
        // the statically guaranteed noexcept NativeVideoFrame move, a null
        // allocation is the only failure here and leaves the source untouched.
        D3D11QueuedVideoFrame* const queuedFrame =
            new (std::nothrow) D3D11QueuedVideoFrame(std::move(frame));
        if (!queuedFrame) {
            ReleaseReservation();
            return nullptr;
        }
        return queuedFrame;
    }

    void Retire(D3D11QueuedVideoFrame* frame) noexcept {
        if (!frame) {
            return;
        }

        AcquireSRWLockExclusive(&queueLock_);
        if (queueSize_ >= retirementQueue_.size()) {
            // This cannot occur for a correctly owned envelope: every queued
            // item and this caller are included in the same capacity counter.
            // In a corrupted/double-retire path, leak this already-bounded
            // envelope rather than destroy it on a latency-sensitive caller.
            ReleaseSRWLockExclusive(&queueLock_);
            return;
        }
        retirementQueue_[queueTail_] = frame;
        queueTail_ = (queueTail_ + 1) % retirementQueue_.size();
        ++queueSize_;
        ReleaseSRWLockExclusive(&queueLock_);
        WakeConditionVariable(&queueCv_);
    }

private:
    bool TryReserve() noexcept {
        std::size_t outstanding = outstanding_.load(std::memory_order_relaxed);
        while (outstanding < kFrameRetirementCapacity) {
            if (outstanding_.compare_exchange_weak(
                    outstanding,
                    outstanding + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void ReleaseReservation() noexcept {
        outstanding_.fetch_sub(1, std::memory_order_release);
    }

    static unsigned __stdcall ThreadEntry(void* context) noexcept {
        static_cast<FrameRetirementService*>(context)->Run();
    }

    [[noreturn]] void Run() noexcept {
        // The worker has process lifetime, so its COM apartment intentionally
        // has the same lifetime and is never torn down ahead of queued frames.
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        for (;;) {
            AcquireSRWLockExclusive(&queueLock_);
            while (queueSize_ == 0) {
                SleepConditionVariableSRW(&queueCv_, &queueLock_, INFINITE, 0);
            }
            D3D11QueuedVideoFrame* frame = retirementQueue_[queueHead_];
            retirementQueue_[queueHead_] = nullptr;
            queueHead_ = (queueHead_ + 1) % retirementQueue_.size();
            --queueSize_;
            ReleaseSRWLockExclusive(&queueLock_);

            delete frame;
            outstanding_.fetch_sub(1, std::memory_order_release);
        }
    }

    std::atomic<std::size_t> outstanding_{0};
    SRWLOCK queueLock_ = SRWLOCK_INIT;
    CONDITION_VARIABLE queueCv_ = CONDITION_VARIABLE_INIT;
    std::array<D3D11QueuedVideoFrame*, kFrameRetirementCapacity> retirementQueue_{};
    std::size_t queueHead_ = 0;
    std::size_t queueTail_ = 0;
    std::size_t queueSize_ = 0;
};

FrameRetirementService* FrameRetirement() noexcept {
    // The pointer itself has a trivial static lifetime. The pointee is leaked
    // by design so neither it nor its synchronization primitives can be torn
    // down while a detached retirement worker is still active.
    static FrameRetirementService* const service = []() noexcept {
        FrameRetirementService* candidate = new (std::nothrow) FrameRetirementService();
        if (!candidate) {
            return static_cast<FrameRetirementService*>(nullptr);
        }
        if (!candidate->Start()) {
            delete candidate;
            return static_cast<FrameRetirementService*>(nullptr);
        }
        return candidate;
    }();
    return service;
}

void RetireQueuedFrame(D3D11QueuedVideoFrame* frame) noexcept {
    if (!frame) {
        return;
    }
    if (FrameRetirementService* const retirement = FrameRetirement()) {
        retirement->Retire(frame);
    }
    // If the process-lifetime service could not start, no envelope can have
    // been acquired from it. A non-null frame here would indicate corruption;
    // deliberately retain it rather than release heavyweight resources on the
    // caller thread.
}

struct VideoColorConstants {
    int matrixType = 0;
    int rangeType = 0;
    int transferType = 1;
    int outputMode = 0;
    int primariesType = 0;
    int toneMapMode = 1;
    float sourcePeakNits = 1000.0f;
    float targetPeakNits = 100.0f;
    float displayPeakNits = 0.0f;
    float hdrCurveEnabled = 0.0f;
    float hdrCurvePointCount = 0.0f;
    int doviTrimEnabled = 0;
    float doviTrimSlope = 1.0f;
    float doviTrimOffset = 0.0f;
    float doviTrimPower = 1.0f;
    float doviTrimSaturation = 1.0f;
    float doviTrimChromaWeight = 1.0f;
    float doviTrimMsWeight = 1.0f;
    float doviTrimStrength = 0.0f;
    float doviTrimMidOffset = 0.0f;
    float doviTrimMidContrast = 0.0f;
    float doviTrimClip = 0.0f;
    float doviTrimReserved0 = 0.0f;
    float doviTrimReserved1 = 0.0f;
    float doviActiveArea[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // x0,y0,x1,y1 in source UV.
    float hdrToneCurve[anvil::playback::kHdrToneCurvePointCount][4] = {};
    float sourceUvRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};

static_assert(sizeof(VideoColorConstants) % 16 == 0);
static_assert(offsetof(VideoColorConstants, sourceUvRect) % 16 == 0);

constexpr DWORD kFrameLatencyWaitTimeoutMs = 8;
constexpr UINT kVideoPresentSyncInterval = 1;

struct SubtitleShaderConstants {
    float uvRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};

static_assert(sizeof(SubtitleShaderConstants) % 16 == 0);

// Dolby Vision reshaping constants uploaded to the GPU each frame.
//
// Layout (HLSL cbuffer, must match the shader-side declaration in psNv12Src).
// Packing follows HLSL rules: every float4 element occupies one 16-byte slot.
//
// The reshaping data covers up to 3 components, each with up to 9 pivots
// (8 piece-wise segments). Polynomial segments carry order + 3 coefficients;
// MMR segments carry a constant + up to 3 terms of 7 coefficients.
struct DoviShaderConstants {
    // Per-component pivot values (normalized to [0,1]). 9 pivots packed into
    // 3 float4s per component (3 values + 1 padding each).
    float pivots[3][3][4] = {};

    // Per-piece mapping metadata: x = mapping_idc (0=poly, 1=mmr, -1=none),
    // y = num_pivots for this component, z = poly_order (0/1/2), w = mmr_order (0..3).
    // 8 pieces per component, packed 4-per-float4 → 2 float4s per component.
    float pieceMeta[3][2][4] = {};

    // Polynomial coefficients per piece: xyz = c0,c1,c2; w = unused.
    // 8 pieces per component.
    float polyCoef[3][8][4] = {};

    // MMR coefficients per piece: float4[6] packs constant + 3 terms × 7 coeffs.
    // Layout within the 6 float4s (24 floats):
    //   [0]: mmrConstant, mmr_coef[t0][0..2]
    //   [1]: mmr_coef[t0][3..6]
    //   [2]: mmr_coef[t1][0..3]
    //   [3]: mmr_coef[t1][4..6], pad
    //   [4]: mmr_coef[t2][0..3]
    //   [5]: mmr_coef[t2][4..6], pad
    float mmrCoef[3][8][6][4] = {};

    // Color matrices (row-major 3x3) + offset.
    float yccToRgb[3][4] = {};       // each row: r,g,b + pad
    float yccOffset[4] = {};
    float rgbToLms[3][4] = {};       // each row: r,g,b + pad (CMv4.0, reserved)
    float curveMeta[4] = {};         // xyz = num_pivots per component, w = offset scale
    float nlqParams[3][4] = {};      // x=offset, y=vdr_in_max, z=slope, w=threshold
    float composerMeta[4] = {};      // residual_enabled, nlq_method, el_bit_depth, vdr_bit_depth
    float composerScale[4] = {};     // coef_log2_denom, el_sample_scale, bl_bit_depth, el_spatial_resample

    // Scalars.
    int profile = 0;
    int compatibilityId = 0;
    int enabled = 0;                 // 1 when DV reshaping should be applied
    float sampleScale = 1.0f;         // P010 R16_UNORM -> BL-normalized code range
};

static_assert(sizeof(DoviShaderConstants) % 16 == 0, "DoviShaderConstants must be 16-byte aligned");

uint64_t ElapsedMicroseconds(const std::chrono::steady_clock::time_point start,
                             const std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

void AccumulateRenderStats(D3D11RenderStats& destination, const D3D11RenderStats& source) {
    destination.frames += source.frames;
    destination.hardwareFrames += source.hardwareFrames;
    destination.bgraFrames += source.bgraFrames;
    destination.subtitleFrames += source.subtitleFrames;
    destination.slowFrames += source.slowFrames;
    destination.hardwareSrvCacheHits += source.hardwareSrvCacheHits;
    destination.hardwareSrvCacheMisses += source.hardwareSrvCacheMisses;
    destination.subtitleSurfaceRebuilds += source.subtitleSurfaceRebuilds;
    destination.subtitleBitmapRects += source.subtitleBitmapRects;
    destination.subtitleBitmapPixels += source.subtitleBitmapPixels;
    destination.totalRenderUs += source.totalRenderUs;
    destination.maxRenderUs = std::max(destination.maxRenderUs, source.maxRenderUs);
    destination.colorPipelineUs += source.colorPipelineUs;
    destination.hardwarePrepareUs += source.hardwarePrepareUs;
    destination.bgraUploadUs += source.bgraUploadUs;
    destination.subtitleUs += source.subtitleUs;
    destination.presentUs += source.presentUs;
    destination.maxPresentUs = std::max(destination.maxPresentUs, source.maxPresentUs);
    destination.presentSyncFrames += source.presentSyncFrames;
    destination.frameLatencyWaits += source.frameLatencyWaits;
    destination.frameLatencyWaitTimeouts += source.frameLatencyWaitTimeouts;
    destination.frameLatencyWaitUs += source.frameLatencyWaitUs;
    destination.maxFrameLatencyWaitUs =
        std::max(destination.maxFrameLatencyWaitUs, source.maxFrameLatencyWaitUs);
    destination.frameStatsSamples += source.frameStatsSamples;
    destination.frameStatsDisjoint += source.frameStatsDisjoint;
}

uint64_t HashCombine(const uint64_t seed, const uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

uint64_t HashDoubleBucket(const double value, const double scale = 1000.0) {
    if (!std::isfinite(value)) {
        return 0;
    }
    const auto bucket = static_cast<int64_t>(std::llround(value * scale));
    return static_cast<uint64_t>(bucket);
}

VideoColorMetadata MergeColorMetadata(VideoColorMetadata frame, const VideoColorMetadata& fallback) {
    if (frame.primaries == VideoColorPrimaries::Unknown) {
        frame.primaries = fallback.primaries;
    }
    if (frame.transfer == VideoTransferCharacteristic::Unknown) {
        frame.transfer = fallback.transfer;
    }
    if (frame.matrix == VideoMatrixCoefficients::Unknown) {
        frame.matrix = fallback.matrix;
    }
    if (frame.range == VideoColorRange::Unknown) {
        frame.range = fallback.range;
    }
    if (!frame.masteringDisplay.hasPrimaries && fallback.masteringDisplay.hasPrimaries) {
        frame.masteringDisplay.red = fallback.masteringDisplay.red;
        frame.masteringDisplay.green = fallback.masteringDisplay.green;
        frame.masteringDisplay.blue = fallback.masteringDisplay.blue;
        frame.masteringDisplay.whitePoint = fallback.masteringDisplay.whitePoint;
        frame.masteringDisplay.hasPrimaries = true;
    }
    if (!frame.masteringDisplay.hasLuminance && fallback.masteringDisplay.hasLuminance) {
        frame.masteringDisplay.minLuminanceNits = fallback.masteringDisplay.minLuminanceNits;
        frame.masteringDisplay.maxLuminanceNits = fallback.masteringDisplay.maxLuminanceNits;
        frame.masteringDisplay.hasLuminance = true;
    }
    if (!frame.contentLight.hasValues && fallback.contentLight.hasValues) {
        frame.contentLight = fallback.contentLight;
    }
    return frame;
}

int MatrixType(const VideoColorMetadata& color) {
    switch (color.matrix) {
    case VideoMatrixCoefficients::Bt709:
        return 1;
    case VideoMatrixCoefficients::Bt601:
        return 2;
    case VideoMatrixCoefficients::Bt2020Ncl:
    case VideoMatrixCoefficients::Bt2020Cl:
        return 3;
    case VideoMatrixCoefficients::Rgb:
    case VideoMatrixCoefficients::Unknown:
        break;
    }
    return color.primaries == VideoColorPrimaries::Bt2020 ? 3 : 1;
}

int PrimariesType(const VideoColorPrimaries primaries) {
    switch (primaries) {
    case VideoColorPrimaries::Bt709:
        return 1;
    case VideoColorPrimaries::Bt2020:
        return 2;
    case VideoColorPrimaries::DisplayP3:
        return 3;
    case VideoColorPrimaries::Unknown:
        break;
    }
    return 0;
}

int TransferType(const VideoTransferCharacteristic transfer) {
    switch (transfer) {
    case VideoTransferCharacteristic::Pq:
        return 2;
    case VideoTransferCharacteristic::Hlg:
        return 3;
    case VideoTransferCharacteristic::Bt709:
    case VideoTransferCharacteristic::Srgb:
    case VideoTransferCharacteristic::Unknown:
        break;
    }
    return 1;
}

int ToneMapType(const anvil::playback::ToneMappingMode mode) {
    switch (mode) {
    case anvil::playback::ToneMappingMode::Auto:
        return 0;
    case anvil::playback::ToneMappingMode::Balanced:
        return 1;
    case anvil::playback::ToneMappingMode::PreserveHighlights:
        return 2;
    case anvil::playback::ToneMappingMode::BrightRoom:
        return 3;
    }
    return 1;
}

float SourcePeakNits(const VideoColorMetadata& color,
                     const anvil::playback::VideoSettings& settings,
                     const anvil::playback::DolbyVisionFrameMetadata* dovi) {
    if (dovi && dovi->valid && dovi->sourceMaxNits > 1.0f) {
        return std::clamp(dovi->sourceMaxNits, 100.0f, 10000.0f);
    }
    if (color.contentLight.hasValues && color.contentLight.maxContentLightLevelNits > 0) {
        return static_cast<float>(color.contentLight.maxContentLightLevelNits);
    }
    if (color.masteringDisplay.hasLuminance && color.masteringDisplay.maxLuminanceNits > 0.0) {
        return static_cast<float>(color.masteringDisplay.maxLuminanceNits);
    }
    return static_cast<float>(std::max(100, settings.peakBrightnessNits));
}

float ClampHdrToneCurveNits(const double value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp(value, 0.0, 10000.0));
}

float HdrToneCurveOutputPeakNits(const anvil::playback::VideoSettings& settings) {
    if (settings.hdrToneCurve.empty()) {
        return static_cast<float>(std::max(100, settings.peakBrightnessNits));
    }
    return std::max(100.0f, ClampHdrToneCurveNits(settings.hdrToneCurve.back().outputNits));
}

struct DoviTrimSelection {
    bool enabled = false;
    float slope = 1.0f;
    float offset = 0.0f;
    float power = 1.0f;
    float saturation = 1.0f;
    float chromaWeight = 1.0f;
    float msWeight = 1.0f;
    float midOffset = 0.0f;
    float midContrast = 0.0f;
    float clip = 0.0f;
    bool includesLevel3 = false;
    const wchar_t* source = L"none";
};

bool ExperimentalDoviTrimEnabled() {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(L"ANVIL_EXPERIMENTAL_DOVI_TRIM", value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return false;
    }
    return value[0] == L'1' || value[0] == L'y' || value[0] == L'Y' || value[0] == L't' || value[0] == L'T';
}

bool IsDoviTrimNeutral(const uint16_t slope,
                       const uint16_t offset,
                       const uint16_t power,
                       const uint16_t chromaWeight,
                       const uint16_t saturationGain,
                       const int msWeight) {
    return slope == 2048 &&
           offset == 2048 &&
           power == 2048 &&
           chromaWeight == 2048 &&
           saturationGain == 2048 &&
           msWeight == 2048;
}

bool HasNonNeutralDoviTrim(const anvil::playback::DolbyVisionFrameMetadata* metadata) {
    if (!metadata || !metadata->valid) {
        return false;
    }
    if (metadata->dmLevel8Present &&
        !IsDoviTrimNeutral(metadata->dmLevel8TrimSlope,
                           metadata->dmLevel8TrimOffset,
                           metadata->dmLevel8TrimPower,
                           metadata->dmLevel8TrimChromaWeight,
                           metadata->dmLevel8TrimSaturationGain,
                           metadata->dmLevel8MsWeight)) {
        return true;
    }
    if (metadata->dmLevel2Present && metadata->dmLevel2Count > 0) {
        const int count = std::min(metadata->dmLevel2Count, anvil::playback::kDoviMaxTrimTargets);
        for (int index = 0; index < count; ++index) {
            if (!IsDoviTrimNeutral(metadata->dmLevel2TrimSlope[index],
                                   metadata->dmLevel2TrimOffset[index],
                                   metadata->dmLevel2TrimPower[index],
                                   metadata->dmLevel2TrimChromaWeight[index],
                                   metadata->dmLevel2TrimSaturationGain[index],
                                   metadata->dmLevel2MsWeight[index])) {
                return true;
            }
        }
    }
    return metadata->dmLevel3Present;
}

float DoviTrimDelta(const int value) {
    return std::clamp((static_cast<float>(value) - 2048.0f) / 4096.0f, -0.5f, 0.5f);
}

float DoviOptionalTrimDelta(const int value) {
    return value == 0 ? 0.0f : DoviTrimDelta(value);
}

void ApplyDoviTrimCodes(DoviTrimSelection& selection,
                        const uint16_t slope,
                        const uint16_t offset,
                        const uint16_t power,
                        const uint16_t chromaWeight,
                        const uint16_t saturationGain,
                        const int msWeight) {
    selection.enabled = true;
    selection.slope = std::clamp(1.0f + DoviTrimDelta(slope) * 0.45f, 0.75f, 1.25f);
    selection.offset = std::clamp(DoviTrimDelta(offset) * 0.16f, -0.10f, 0.10f);
    selection.power = std::clamp(1.0f - DoviTrimDelta(power) * 0.32f, 0.78f, 1.22f);
    selection.chromaWeight = std::clamp(1.0f + DoviTrimDelta(chromaWeight) * 0.25f, 0.88f, 1.12f);
    selection.saturation = std::clamp(1.0f + DoviTrimDelta(saturationGain) * 0.60f, 0.75f, 1.30f);
    selection.msWeight = std::clamp(1.0f + DoviTrimDelta(msWeight) * 0.20f, 0.90f, 1.10f);
    selection.midContrast = std::clamp(DoviTrimDelta(msWeight) * 0.20f, -0.12f, 0.12f);
}

void ApplyDoviLevel3(DoviTrimSelection& selection,
                     const anvil::playback::DolbyVisionFrameMetadata& metadata) {
    if (!metadata.dmLevel3Present) {
        return;
    }
    const float midOffset = std::clamp(DoviTrimDelta(metadata.dmLevel3AvgPqOffset) * 0.12f, -0.08f, 0.08f);
    const float rangeBias =
        std::clamp((DoviTrimDelta(metadata.dmLevel3MaxPqOffset) - DoviTrimDelta(metadata.dmLevel3MinPqOffset)) * 0.10f,
                   -0.07f,
                   0.07f);
    if (std::abs(midOffset) > 0.001f || std::abs(rangeBias) > 0.001f) {
        const bool wasEnabled = selection.enabled;
        selection.enabled = true;
        selection.midOffset = std::clamp(selection.midOffset + midOffset, -0.18f, 0.18f);
        selection.midContrast = std::clamp(selection.midContrast + rangeBias, -0.18f, 0.18f);
        selection.includesLevel3 = true;
        if (!wasEnabled) {
            selection.source = L"L3";
        }
    }
}

DoviTrimSelection SelectDoviTrim(const anvil::playback::DolbyVisionFrameMetadata* metadata,
                                 const float targetPeakNits,
                                 const bool cmv4ApproxEnabled) {
    DoviTrimSelection selection;
    if (!metadata || !metadata->valid || !cmv4ApproxEnabled) {
        return selection;
    }

    const bool level8NonNeutral =
        metadata->dmLevel8Present &&
        !IsDoviTrimNeutral(metadata->dmLevel8TrimSlope,
                           metadata->dmLevel8TrimOffset,
                           metadata->dmLevel8TrimPower,
                           metadata->dmLevel8TrimChromaWeight,
                           metadata->dmLevel8TrimSaturationGain,
                           metadata->dmLevel8MsWeight);
    bool usedLevel2Compat = false;

    if (level8NonNeutral) {
        ApplyDoviTrimCodes(selection,
                           metadata->dmLevel8TrimSlope,
                           metadata->dmLevel8TrimOffset,
                           metadata->dmLevel8TrimPower,
                           metadata->dmLevel8TrimChromaWeight,
                           metadata->dmLevel8TrimSaturationGain,
                           metadata->dmLevel8MsWeight);
        selection.midContrast = std::clamp(selection.midContrast + DoviOptionalTrimDelta(metadata->dmLevel8TargetMidContrast) * 0.35f,
                                          -0.20f,
                                          0.20f);
        selection.clip = std::clamp(DoviOptionalTrimDelta(metadata->dmLevel8ClipTrim) * 0.40f, -0.18f, 0.18f);
        selection.source = L"L8_cmv4";
    } else if (metadata->dmLevel3Present && metadata->dmLevel8Present) {
        // CMv4 carries the creative trims in L8/L3. If the L8 block itself is
        // neutral, still let L3 drive the frame-varying offset rather than
        // falling back to the CMv2.9-compatible L2 trims.
        selection.source = L"L3_cmv4";
    } else if (metadata->dmLevel2Present && metadata->dmLevel2Count > 0) {
        int selected = 0;
        float bestDistance = std::numeric_limits<float>::max();
        const int count = std::min(metadata->dmLevel2Count, anvil::playback::kDoviMaxTrimTargets);
        for (int index = 0; index < count; ++index) {
            const float trimTarget = Pq12CodeToNits(metadata->dmLevel2TargetMaxPq[index]);
            const float distance = std::abs(trimTarget - targetPeakNits);
            if (distance < bestDistance) {
                bestDistance = distance;
                selected = index;
            }
        }
        ApplyDoviTrimCodes(selection,
                           metadata->dmLevel2TrimSlope[selected],
                           metadata->dmLevel2TrimOffset[selected],
                           metadata->dmLevel2TrimPower[selected],
                           metadata->dmLevel2TrimChromaWeight[selected],
                           metadata->dmLevel2TrimSaturationGain[selected],
                           metadata->dmLevel2MsWeight[selected]);
        selection.source = L"L2_compat";
        usedLevel2Compat = true;
    }

    // Dolby's CMv4 workflow exposes the primary trims as L8/L3; L2 is a
    // backwards-compatible derivative, so do not stack L3 again when L2 is the
    // selected compatibility trim.
    if (!usedLevel2Compat) {
        ApplyDoviLevel3(selection, *metadata);
    }

    return selection;
}

VideoColorMetadata NormalizeDolbyVisionOutput(VideoColorMetadata color, const bool hasDolbyVision) {
    if (!hasDolbyVision) {
        return color;
    }

    // Matches libplacebo/FFmpeg apply_dolbyvision: reshaped DV is BT.2020 PQ,
    // independent of the often-unknown Profile 5 container color tags.
    color.primaries = VideoColorPrimaries::Bt2020;
    color.transfer = VideoTransferCharacteristic::Pq;
    color.matrix = VideoMatrixCoefficients::Rgb;
    color.range = VideoColorRange::Full;
    return color;
}

float DoviTextureSampleScale(const int bitDepth) {
    const int depth = std::clamp(bitDepth, 8, 16);
    const uint64_t codeMax = (uint64_t{1} << depth) - 1;
    const int bitShift = 16 - depth;
    const uint64_t shiftedMax = codeMax << bitShift;
    return shiftedMax > 0
               ? static_cast<float>(65535.0 / static_cast<double>(shiftedMax))
               : 1.0f;
}

bool DoviSingleNlqPartition(const anvil::playback::DolbyVisionFrameMetadata* metadata) {
    return !metadata || (metadata->nlqNumXPartitions <= 1 && metadata->nlqNumYPartitions <= 1);
}

bool HasDoviActiveAreaMask(const anvil::playback::DolbyVisionFrameMetadata* metadata,
                           const int width,
                           const int height) {
    if (!metadata || !metadata->valid || !metadata->dmLevel5Present || width <= 0 || height <= 0) {
        return false;
    }
    const uint32_t horizontal = static_cast<uint32_t>(metadata->dmLevel5LeftOffset) +
                                static_cast<uint32_t>(metadata->dmLevel5RightOffset);
    const uint32_t vertical = static_cast<uint32_t>(metadata->dmLevel5TopOffset) +
                              static_cast<uint32_t>(metadata->dmLevel5BottomOffset);
    return (horizontal > 0 || vertical > 0) &&
           horizontal < static_cast<uint32_t>(width) &&
           vertical < static_cast<uint32_t>(height);
}

float DoviOffsetScale() {
    return static_cast<float>(65536.0 / 65535.0);
}

bool WantsHdrOutput(const VideoColorMetadata& color,
                    const anvil::playback::VideoSettings& settings,
                    const anvil::playback::DisplayCapabilities& display) {
    if (!settings.dolbyVisionHdrOutput) {
        return false;
    }
    if (settings.hdrOutput == anvil::playback::HdrOutputMode::ForceSdr) {
        return false;
    }
    if (settings.hdrOutput == anvil::playback::HdrOutputMode::ForceHdr) {
        return display.hdrEnabled || display.hdrSupported;
    }
    return settings.dolbyVisionHdrOutput && color.IsHdr();
}

UINT16 ChromaticityToDxgi(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    return static_cast<UINT16>(std::clamp(value * 50000.0, 0.0, 65535.0) + 0.5);
}

UINT LuminanceToDxgi(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    return static_cast<UINT>(std::clamp(value * 10000.0, 0.0, 4294967295.0) + 0.5);
}

UINT16 LightLevelToDxgi(int value) {
    return static_cast<UINT16>(std::clamp(value, 0, 65535));
}

std::wstring ColorSpaceName(const DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return L"rgb_full_pq_bt2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return L"rgb_full_g22_bt709";
    default:
        return L"dxgi_color_space_" + std::to_wstring(static_cast<int>(colorSpace));
    }
}

std::wstring OutputModeName(const bool hdr) {
    return hdr ? L"hdr10" : L"sdr";
}

RECT ViewportRect(const D3D11_VIEWPORT& viewport) {
    return MakeRect(static_cast<int>(std::round(viewport.TopLeftX)),
                    static_cast<int>(std::round(viewport.TopLeftY)),
                    static_cast<int>(std::round(viewport.TopLeftX + viewport.Width)),
                    static_cast<int>(std::round(viewport.TopLeftY + viewport.Height)));
}

bool RectEquals(const RECT& lhs, const RECT& rhs) {
    return lhs.left == rhs.left &&
           lhs.top == rhs.top &&
           lhs.right == rhs.right &&
           lhs.bottom == rhs.bottom;
}

int CountSubtitleLines(const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(std::count(text.begin(), text.end(), L'\n')) + 1;
}

float SubtitleFontPixels(const D3D11_VIEWPORT& videoViewport, const double fontScale) {
    const float base = std::clamp(videoViewport.Height * 0.048f, 20.0f, 54.0f);
    return base * static_cast<float>(std::clamp(fontScale, 0.5, 2.0));
}

}  // namespace

D3D11VideoRenderer::~D3D11VideoRenderer() {
    // Normal window shutdown waits for the RequestStop completion message and
    // destroys the renderer on a non-window reaper. The public stop path never
    // joins; this only reaps the already-finished worker during destruction.
    StopRenderThread();
}

bool D3D11VideoRenderer::BeginInitialize(const HWND host,
                                         const HWND completionWindow,
                                         const UINT completionMessage,
                                         const uint64_t completionCookie) {
    if (!host) {
        state_.store(D3D11RendererState::Failed, std::memory_order_release);
        PostInitializationCompletion(
            D3D11RendererState::Failed,
            completionWindow,
            completionMessage,
            completionCookie);
        return false;
    }

    RECT rc{};
    GetClientRect(host, &rc);
    const UINT width = static_cast<UINT>(std::max(1, RectWidth(rc)));
    const UINT height = static_cast<UINT>(std::max(1, RectHeight(rc)));

    try {
        std::lock_guard lock(commandMutex_);
        if (renderThread_.joinable() || !stopped_.load(std::memory_order_acquire)) {
            return false;
        }

        host_.store(host, std::memory_order_release);
        initialWidth_ = width;
        initialHeight_ = height;
        initializationCompletionWindow_ = completionWindow;
        initializationCompletionMessage_ = completionMessage;
        initializationCompletionCookie_ = completionCookie;
        stopCompletionWindow_ = nullptr;
        stopCompletionMessage_ = 0;
        stopCompletionCookie_ = 0;
        stopRequested_ = false;
        acceptingCommands_ = true;
        publishedDevice_.store(nullptr, std::memory_order_release);
        stopped_.store(false, std::memory_order_release);
        state_.store(D3D11RendererState::Initializing, std::memory_order_release);
        renderThread_ = std::thread(&D3D11VideoRenderer::RenderThreadMain, this);
    } catch (const std::system_error& error) {
        {
            std::lock_guard lock(commandMutex_);
            stopRequested_ = true;
            acceptingCommands_ = false;
            host_.store(nullptr, std::memory_order_release);
            publishedDevice_.store(nullptr, std::memory_order_release);
            stopped_.store(true, std::memory_order_release);
            state_.store(D3D11RendererState::Failed, std::memory_order_release);
        }
        LogInfo(L"render thread start failed error=" + Utf8ToWide(error.what()));
        PostInitializationCompletion(
            D3D11RendererState::Failed,
            completionWindow,
            completionMessage,
            completionCookie);
        return false;
    }
    return true;
}

bool D3D11VideoRenderer::Initialize(const HWND host) {
    return BeginInitialize(host, nullptr, 0, 0);
}

D3D11RendererState D3D11VideoRenderer::State() const noexcept {
    return state_.load(std::memory_order_acquire);
}

bool D3D11VideoRenderer::IsReady() const noexcept {
    return State() == D3D11RendererState::Ready;
}

bool D3D11VideoRenderer::InitializationFailed() const noexcept {
    return State() == D3D11RendererState::Failed;
}

ID3D11Device* D3D11VideoRenderer::Device() const noexcept {
    if (state_.load(std::memory_order_acquire) != D3D11RendererState::Ready) {
        return nullptr;
    }
    return publishedDevice_.load(std::memory_order_acquire);
}

bool D3D11VideoRenderer::IsStopRequested() const {
    std::lock_guard lock(commandMutex_);
    return stopRequested_;
}

void D3D11VideoRenderer::PostInitializationCompletion(const D3D11RendererState state,
                                                      const HWND window,
                                                      const UINT message,
                                                      const uint64_t cookie) const noexcept {
    if (window && message != 0) {
        PostMessageW(window,
                     message,
                     static_cast<WPARAM>(cookie),
                     static_cast<LPARAM>(state));
    }
}

bool D3D11VideoRenderer::InitializeGpuOnRenderThread(const UINT width, const UINT height) {
    // Every GPU/driver/DirectComposition operation in this method executes on
    // the render worker. Cancellation is cooperative between calls: an
    // uninterruptible driver call may finish, after which the worker observes
    // stopRequested_ and immediately releases everything it created.
    if (IsStopRequested()) {
        return false;
    }

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) { LogHr(L"CreateDXGIFactory2", hr); return false; }
    if (IsStopRequested()) return false;

    hr = factory_->EnumAdapters(0, &adapter_);
    if (FAILED(hr)) { adapter_.Reset(); }
    if (IsStopRequested()) return false;

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    const UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(adapter_.Get(), adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           nullptr, createFlags, featureLevels, static_cast<UINT>(std::size(featureLevels)),
                           D3D11_SDK_VERSION, &device_, &obtained, &context_);
    if (IsStopRequested()) return false;
    if (FAILED(hr)) {
        // WARP fallback.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              featureLevels, static_cast<UINT>(std::size(featureLevels)),
                              D3D11_SDK_VERSION, &device_, &obtained, &context_);
        if (FAILED(hr)) { LogHr(L"D3D11CreateDevice", hr); return false; }
        if (IsStopRequested()) return false;
    }
    EnableMultithreadProtection();
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice)) && dxgiDevice) {
        dxgiDevice->SetMaximumFrameLatency(1);
    }
    if (IsStopRequested()) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    // Prefer a composition swap chain: DWM composites the video surface so GDI
    // sibling overlay windows (subtitle menu popup) render on top of it. A
    // HWND-bound flip-model swap chain would occlude those overlays. Composition
    // swap chains do not support ALLOW_MODE_SWITCH, but can use the frame
    // latency waitable flag on systems/drivers that expose it.
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr = factory_->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_);
    if (IsStopRequested()) return false;
    const bool createdWaitableComposition = SUCCEEDED(hr) && swapChain_ && CreateComposition();
    if (IsStopRequested()) return false;
    if (createdWaitableComposition) {
        useComposition_ = true;
        LogInfo(L"swap chain created via CreateSwapChainForComposition + DirectComposition waitable=true");
    } else {
        swapChain_.Reset();
        dcompVisual_.Reset();
        dcompTarget_.Reset();
        dcompDevice_.Reset();
        useComposition_ = false;

        desc.Flags = 0;
        hr = factory_->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_);
        if (IsStopRequested()) return false;
        const bool createdComposition = SUCCEEDED(hr) && swapChain_ && CreateComposition();
        if (IsStopRequested()) return false;
        if (createdComposition) {
            useComposition_ = true;
            LogInfo(L"swap chain created via CreateSwapChainForComposition + DirectComposition waitable=false");
        } else {
            // Fallback to a HWND-bound flip-model swap chain on systems/drivers
            // without composition support. Behavior reverts to the previous one
            // (overlay occlusion may recur on such systems).
            swapChain_.Reset();
            dcompVisual_.Reset();
            dcompTarget_.Reset();
            dcompDevice_.Reset();
            useComposition_ = false;

            desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            const HWND swapChainHost = host_.load(std::memory_order_acquire);
            if (IsStopRequested() || !swapChainHost || !IsWindow(swapChainHost)) return false;
            hr = factory_->CreateSwapChainForHwnd(device_.Get(), swapChainHost, &desc, nullptr, nullptr, &swapChain_);
            if (IsStopRequested()) return false;
            if (SUCCEEDED(hr)) {
                LogInfo(L"swap chain created via CreateSwapChainForHwnd (composition fallback) waitable=true");
            } else {
                desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
                if (IsStopRequested()) return false;
                hr = factory_->CreateSwapChainForHwnd(device_.Get(), swapChainHost, &desc, nullptr, nullptr, &swapChain_);
                if (FAILED(hr)) { LogHr(L"CreateSwapChainForHwnd", hr); return false; }
                if (IsStopRequested()) return false;
                LogInfo(L"swap chain created via CreateSwapChainForHwnd (composition fallback) waitable=false");
            }
        }
    }

    ConfigureFramePacing();
    if (IsStopRequested()) return false;
    if (!CreateRenderTarget()) return false;
    if (IsStopRequested()) return false;
    if (!CreatePipeline()) return false;
    if (IsStopRequested()) return false;

    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;
    if (!ApplySwapChainColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, mediaColor_)) {
        // Color-space support is best-effort and was not fatal in the previous
        // synchronous initialization path.
    }
    if (IsStopRequested()) return false;
    return true;
}

void D3D11VideoRenderer::ConfigureColorPipeline(const anvil::playback::VideoSettings& settings,
                                                const anvil::playback::DisplayCapabilities& display,
                                                const anvil::playback::VideoColorMetadata& mediaColor) {
    {
        std::lock_guard lock(commandMutex_);
        pendingColorPipeline_ = PendingColorPipeline{settings, display, mediaColor};
    }
    commandCv_.notify_one();
}

void D3D11VideoRenderer::ApplyColorPipelineConfiguration(const PendingColorPipeline& configuration) {
    videoSettings_ = configuration.settings;
    displayCapabilities_ = configuration.display;
    mediaColor_ = configuration.mediaColor;
    activePipelineLabel_.clear();
    activePipelineSignature_ = 0;
    hdrMetadataApplied_ = false;
    hdrColorSpaceFailureLogged_ = false;
}

void D3D11VideoRenderer::ConfigureSubtitleSettings(const anvil::playback::SubtitleSettings& settings) {
    {
        std::lock_guard lock(commandMutex_);
        pendingSubtitleSettings_ = settings;
    }
    commandCv_.notify_one();
}

void D3D11VideoRenderer::ApplySubtitleConfiguration(const anvil::playback::SubtitleSettings& settings) {
    subtitleSettings_ = settings;
    activeSubtitleText_.clear();
    activeSubtitleBitmapKey_.clear();
    subtitleSrv_.Reset();
}

void D3D11VideoRenderer::OnResize() {
    {
        std::lock_guard lock(commandMutex_);
        if (!acceptingCommands_) {
            return;
        }
    }

    const HWND host = host_.load(std::memory_order_acquire);
    if (!host) {
        return;
    }
    RECT rc{};
    GetClientRect(host, &rc);
    const UINT width = static_cast<UINT>(std::max(1, RectWidth(rc)));
    const UINT height = static_cast<UINT>(std::max(1, RectHeight(rc)));

    {
        std::lock_guard lock(commandMutex_);
        if (!acceptingCommands_) {
            return;
        }
        pendingResizeWidth_ = width;
        pendingResizeHeight_ = height;
        pendingResize_ = true;
    }
    commandCv_.notify_one();
}

void D3D11VideoRenderer::ResizeOnRenderThread(const UINT width, const UINT height) {
    if (!swapChain_) return;
    backBuffer_.Reset();
    rtv_.Reset();
    // Composition swap chains do not use ALLOW_MODE_SWITCH; only set it for the
    // HWND-bound fallback path.
    UINT resizeFlags = useComposition_ ? 0 : DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (frameLatencyWaitable_) {
        resizeFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, resizeFlags);
    if (FAILED(hr) && frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
        resizeFlags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        LogInfo(L"frame pacing waitable disabled after ResizeBuffers failure");
        hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, resizeFlags);
    }
    if (FAILED(hr)) { LogHr(L"ResizeBuffers", hr); return; }
    CreateRenderTarget();
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    // For composition swap chains, the visual content tracks the new buffer
    // size automatically once content is presented, but committing here ensures
    // the surface is kept in sync on resize.
    if (useComposition_ && dcompDevice_) {
        dcompDevice_->Commit();
    }
}

void D3D11VideoRenderer::SetDiagnosticsEnabled(const bool enabled) {
    {
        std::lock_guard lock(commandMutex_);
        pendingDiagnosticsEnabled_ = enabled;
        pendingResetStats_ = true;
    }
    commandCv_.notify_one();
}

void D3D11VideoRenderer::SetDiagnosticsEnabledOnRenderThread(const bool enabled) {
    diagnosticsEnabled_ = enabled;
    ResetRenderStatsOnRenderThread();
}

void D3D11VideoRenderer::ResetRenderStats() {
    {
        std::lock_guard lock(commandMutex_);
        pendingResetStats_ = true;
    }
    commandCv_.notify_one();
}

void D3D11VideoRenderer::ResetRenderStatsOnRenderThread() {
    renderStats_ = {};
    std::lock_guard lock(publishedStatsMutex_);
    publishedRenderStats_ = {};
}

D3D11RenderStats D3D11VideoRenderer::TakeRenderStats() {
    std::lock_guard lock(publishedStatsMutex_);
    const D3D11RenderStats stats = publishedRenderStats_;
    publishedRenderStats_ = {};
    return stats;
}

void D3D11VideoRenderer::PublishRenderStats() {
    const D3D11RenderStats snapshot = renderStats_;
    renderStats_ = {};
    if (!diagnosticsEnabled_) {
        return;
    }
    std::lock_guard lock(publishedStatsMutex_);
    AccumulateRenderStats(publishedRenderStats_, snapshot);
}

bool D3D11VideoRenderer::HasPendingWorkLocked() const {
    return pendingFrame_ != nullptr ||
           pendingResize_ ||
           pendingColorPipeline_.has_value() ||
           pendingSubtitleSettings_.has_value() ||
           pendingDiagnosticsEnabled_.has_value() ||
           pendingResetStats_ ||
           pendingClear_;
}

void D3D11VideoRenderer::RequestStop(const HWND completionWindow,
                                     const UINT completionMessage,
                                     const uint64_t completionCookie) {
    bool postCompletionNow = false;
    {
        std::lock_guard lock(commandMutex_);
        acceptingCommands_ = false;
        if (stopped_.load(std::memory_order_acquire)) {
            postCompletionNow = completionWindow != nullptr && completionMessage != 0;
        } else {
            stopRequested_ = true;
            // Withdraw the borrowed HWND together with the device. A render
            // worker returning from a stalled driver call will observe null
            // before issuing any subsequent HWND-bound DComp/DXGI operation.
            host_.store(nullptr, std::memory_order_release);
            // Withdraw the borrowed device before the worker begins releasing
            // it. A driver call already in progress remains confined to the
            // render thread and is released immediately after it returns.
            publishedDevice_.store(nullptr, std::memory_order_release);
            state_.store(D3D11RendererState::Stopping, std::memory_order_release);
            if (completionWindow && completionMessage != 0) {
                stopCompletionWindow_ = completionWindow;
                stopCompletionMessage_ = completionMessage;
                stopCompletionCookie_ = completionCookie;
            }
            pendingResize_ = false;
            pendingClear_ = false;
        }
    }
    if (postCompletionNow) {
        PostMessageW(completionWindow,
                     completionMessage,
                     static_cast<WPARAM>(completionCookie),
                     0);
        return;
    }
    commandCv_.notify_all();
}

bool D3D11VideoRenderer::IsStopped() const noexcept {
    return stopped_.load(std::memory_order_acquire);
}

void D3D11VideoRenderer::StopRenderThread() {
    RequestStop(nullptr, 0);
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
}

void D3D11VideoRenderer::RenderThreadMain() {
    const HRESULT apartmentHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeApartment = SUCCEEDED(apartmentHr);
    if (FAILED(apartmentHr) && apartmentHr != RPC_E_CHANGED_MODE) {
        LogHr(L"render thread CoInitializeEx", apartmentHr);
    }

    bool initialized = false;
    try {
        initialized = InitializeGpuOnRenderThread(initialWidth_, initialHeight_);
    } catch (const std::exception& error) {
        LogInfo(L"render initialization exception=" + Utf8ToWide(error.what()));
    } catch (...) {
        LogInfo(L"render initialization exception=unknown");
    }

    HWND initializationCompletionWindow = nullptr;
    UINT initializationCompletionMessage = 0;
    uint64_t initializationCompletionCookie = 0;
    bool ready = false;
    {
        std::lock_guard lock(commandMutex_);
        if (initialized && !stopRequested_) {
            // Publish the raw device before the release-store of Ready. Device()
            // performs the matching acquire-load and therefore cannot observe
            // a partially initialized pipeline.
            publishedDevice_.store(device_.Get(), std::memory_order_release);
            state_.store(D3D11RendererState::Ready, std::memory_order_release);
            initializationCompletionWindow = initializationCompletionWindow_;
            initializationCompletionMessage = initializationCompletionMessage_;
            initializationCompletionCookie = initializationCompletionCookie_;
            initializationCompletionWindow_ = nullptr;
            initializationCompletionMessage_ = 0;
            initializationCompletionCookie_ = 0;
            ready = true;
        }
    }

    if (ready) {
        PostInitializationCompletion(
            D3D11RendererState::Ready,
            initializationCompletionWindow,
            initializationCompletionMessage,
            initializationCompletionCookie);
    } else {
        D3D11QueuedVideoFrame* failedFrame = nullptr;
        {
            std::lock_guard lock(commandMutex_);
            acceptingCommands_ = false;
            failedFrame = pendingFrame_;
            pendingFrame_ = nullptr;
            pendingResize_ = false;
            pendingColorPipeline_.reset();
            pendingSubtitleSettings_.reset();
            pendingDiagnosticsEnabled_.reset();
            pendingResetStats_ = false;
            pendingClear_ = false;
        }
        RetireQueuedFrame(failedFrame);
        ReleaseAll();

        HWND stopCompletionWindow = nullptr;
        UINT stopCompletionMessage = 0;
        uint64_t stopCompletionCookie = 0;
        bool cancelled = false;
        {
            std::lock_guard lock(commandMutex_);
            cancelled = stopRequested_;
            publishedDevice_.store(nullptr, std::memory_order_release);
            if (cancelled) {
                state_.store(D3D11RendererState::Stopped, std::memory_order_release);
                stopCompletionWindow = stopCompletionWindow_;
                stopCompletionMessage = stopCompletionMessage_;
                stopCompletionCookie = stopCompletionCookie_;
            } else {
                state_.store(D3D11RendererState::Failed, std::memory_order_release);
                initializationCompletionWindow = initializationCompletionWindow_;
                initializationCompletionMessage = initializationCompletionMessage_;
                initializationCompletionCookie = initializationCompletionCookie_;
            }
            initializationCompletionWindow_ = nullptr;
            initializationCompletionMessage_ = 0;
            initializationCompletionCookie_ = 0;
            stopCompletionWindow_ = nullptr;
            stopCompletionMessage_ = 0;
            stopCompletionCookie_ = 0;
            stopped_.store(true, std::memory_order_release);
        }

        if (!cancelled) {
            PostInitializationCompletion(
                D3D11RendererState::Failed,
                initializationCompletionWindow,
                initializationCompletionMessage,
                initializationCompletionCookie);
        }
        if (stopCompletionWindow && stopCompletionMessage != 0) {
            PostMessageW(stopCompletionWindow,
                         stopCompletionMessage,
                         static_cast<WPARAM>(stopCompletionCookie),
                         0);
        }
        if (uninitializeApartment) {
            CoUninitialize();
        }
        return;
    }

    D3D11QueuedVideoFrame* shutdownFrame = nullptr;
    for (;;) {
        D3D11QueuedVideoFrame* frame = nullptr;
        std::optional<PendingColorPipeline> colorPipeline;
        std::optional<anvil::playback::SubtitleSettings> subtitleSettings;
        std::optional<bool> diagnosticsEnabled;
        bool resize = false;
        UINT resizeWidth = 1;
        UINT resizeHeight = 1;
        bool resetStats = false;
        bool clear = false;

        {
            std::unique_lock lock(commandMutex_);
            commandCv_.wait(lock, [this]() {
                return stopRequested_ || HasPendingWorkLocked();
            });
            if (stopRequested_) {
                shutdownFrame = pendingFrame_;
                pendingFrame_ = nullptr;
                break;
            }

            frame = pendingFrame_;
            pendingFrame_ = nullptr;
            colorPipeline = std::move(pendingColorPipeline_);
            pendingColorPipeline_.reset();
            subtitleSettings = std::move(pendingSubtitleSettings_);
            pendingSubtitleSettings_.reset();
            diagnosticsEnabled = pendingDiagnosticsEnabled_;
            pendingDiagnosticsEnabled_.reset();
            resize = pendingResize_;
            resizeWidth = pendingResizeWidth_;
            resizeHeight = pendingResizeHeight_;
            pendingResize_ = false;
            resetStats = pendingResetStats_;
            pendingResetStats_ = false;
            clear = pendingClear_;
            pendingClear_ = false;
        }

        if (colorPipeline) {
            ApplyColorPipelineConfiguration(*colorPipeline);
        }
        if (subtitleSettings) {
            ApplySubtitleConfiguration(*subtitleSettings);
        }
        if (diagnosticsEnabled) {
            SetDiagnosticsEnabledOnRenderThread(*diagnosticsEnabled);
        }
        if (resetStats) {
            ResetRenderStatsOnRenderThread();
        }
        if (resize) {
            ResizeOnRenderThread(resizeWidth, resizeHeight);
        }
        if (clear) {
            ClearOnRenderThread();
        }
        if (frame) {
            RenderOnRenderThread(frame->frame);
            PublishRenderStats();
            RetireQueuedFrame(frame);
        }
    }

    // Retire the final queued decoder/D3D references on this worker as part of
    // asynchronous shutdown, not on the window thread that called RequestStop.
    RetireQueuedFrame(shutdownFrame);
    ReleaseAll();

    HWND completionWindow = nullptr;
    UINT completionMessage = 0;
    uint64_t completionCookie = 0;
    {
        std::lock_guard lock(commandMutex_);
        completionWindow = stopCompletionWindow_;
        completionMessage = stopCompletionMessage_;
        completionCookie = stopCompletionCookie_;
        stopCompletionWindow_ = nullptr;
        stopCompletionMessage_ = 0;
        stopCompletionCookie_ = 0;
        publishedDevice_.store(nullptr, std::memory_order_release);
        state_.store(D3D11RendererState::Stopped, std::memory_order_release);
        stopped_.store(true, std::memory_order_release);
    }
    if (completionWindow && completionMessage != 0) {
        PostMessageW(completionWindow,
                     completionMessage,
                     static_cast<WPARAM>(completionCookie),
                     0);
    }
    if (uninitializeApartment) {
        CoUninitialize();
    }
}

bool D3D11VideoRenderer::ConfigureFramePacing() {
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    if (!swapChain_) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (FAILED(swapChain_.As(&swapChain2)) || !swapChain2) {
        if (!framePacingLogged_) {
            framePacingLogged_ = true;
            LogInfo(L"frame pacing waitable=unavailable reason=swapchain2");
        }
        return false;
    }

    const HRESULT latencyHr = swapChain2->SetMaximumFrameLatency(1);
    if (FAILED(latencyHr)) {
        LogHr(L"SetMaximumFrameLatency swapchain", latencyHr);
    }

    frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
    if (!frameLatencyWaitable_) {
        if (!framePacingLogged_) {
            framePacingLogged_ = true;
            LogInfo(L"frame pacing waitable=unavailable reason=no_handle present_sync_interval=1");
        }
        return false;
    }

    if (!framePacingLogged_) {
        framePacingLogged_ = true;
        LogInfo(L"frame pacing waitable=active max_frame_latency=1 present_sync_interval=1");
    }
    return true;
}

void D3D11VideoRenderer::WaitForFrameLatencyObject(const bool collectStats) {
    if (!frameLatencyWaitable_) {
        return;
    }

    const auto waitStart = collectStats ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    const DWORD result = WaitForSingleObjectEx(frameLatencyWaitable_, kFrameLatencyWaitTimeoutMs, TRUE);
    if (!collectStats) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint64_t waitUs = ElapsedMicroseconds(waitStart, now);
    ++renderStats_.frameLatencyWaits;
    renderStats_.frameLatencyWaitUs += waitUs;
    renderStats_.maxFrameLatencyWaitUs = std::max(renderStats_.maxFrameLatencyWaitUs, waitUs);
    if (result == WAIT_TIMEOUT || result == WAIT_FAILED) {
        ++renderStats_.frameLatencyWaitTimeouts;
    }
}

void D3D11VideoRenderer::PresentFrame(const UINT syncInterval,
                                      const bool collectStats,
                                      const std::chrono::steady_clock::time_point stageStart) {
    if (!swapChain_) {
        return;
    }

    const HRESULT presentHr = swapChain_->Present(syncInterval, 0);
    if (FAILED(presentHr)) {
        LogHr(L"Present", presentHr);
    }

    if (!collectStats) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint64_t presentUs = ElapsedMicroseconds(stageStart, now);
    renderStats_.presentUs += presentUs;
    renderStats_.maxPresentUs = std::max(renderStats_.maxPresentUs, presentUs);
    if (syncInterval > 0) {
        ++renderStats_.presentSyncFrames;
    }

    DXGI_FRAME_STATISTICS frameStatistics{};
    const HRESULT statisticsHr = swapChain_->GetFrameStatistics(&frameStatistics);
    if (SUCCEEDED(statisticsHr)) {
        ++renderStats_.frameStatsSamples;
    } else if (statisticsHr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        ++renderStats_.frameStatsDisjoint;
    }
}

void D3D11VideoRenderer::Render(const NativeVideoFrame& frame) {
    {
        std::lock_guard lock(commandMutex_);
        if (!acceptingCommands_) {
            return;
        }
    }

    FrameRetirementService* const retirement = FrameRetirement();
    if (!retirement) {
        return;
    }

    // All large pixel planes and decoder references in NativeVideoFrame are
    // shared_ptr/ComPtr-backed. The bounded envelope retains them without a
    // deep pixel copy and can always be handed to the retirement worker without
    // allocating after it replaces the mailbox entry.
    D3D11QueuedVideoFrame* const queuedFrame = retirement->TryAcquire(frame);
    if (!queuedFrame) {
        return;
    }

    D3D11QueuedVideoFrame* replacedFrame = nullptr;
    bool accepted = false;
    {
        std::lock_guard lock(commandMutex_);
        if (acceptingCommands_) {
            replacedFrame = pendingFrame_;
            pendingFrame_ = queuedFrame;
            accepted = true;
        }
    }
    retirement->Retire(replacedFrame);
    if (!accepted) {
        retirement->Retire(queuedFrame);
        return;
    }
    commandCv_.notify_one();
}

bool D3D11VideoRenderer::RetireFrame(NativeVideoFrame&& frame) noexcept {
    FrameRetirementService* const retirement = FrameRetirement();
    if (!retirement) {
        return false;
    }

    D3D11QueuedVideoFrame* const queuedFrame =
        retirement->TryAcquireForRetirement(std::move(frame));
    if (!queuedFrame) {
        return false;
    }
    retirement->Retire(queuedFrame);
    return true;
}

void D3D11VideoRenderer::RenderOnRenderThread(const NativeVideoFrame& frame) {
    if (!device_ || !context_ || !swapChain_) return;
    if (((frame.dovi && frame.dovi->valid) ||
         (frame.enhancementDovi && frame.enhancementDovi->valid) ||
         !frame.dynamicMetadataPath.empty()) &&
        !dolbyVisionMetadataLogged_) {
        LogInfo(L"render_begin input=" +
                std::wstring(frame.HasD3DTexture() ? L"d3d11_texture" : (frame.HasYuv() ? L"p010_yuv" : L"bgra")) +
                L" serial=" + std::to_wstring(frame.serial));
        if (((!frame.dovi || !frame.dovi->valid) &&
             (!frame.enhancementDovi || !frame.enhancementDovi->valid)) ||
            frame.dynamicMetadataPath.find(L"dolby_vision_libplacebo") != std::wstring::npos) {
            dolbyVisionMetadataLogged_ = true;
        }
    }
    constexpr uint64_t kSlowRenderFrameThresholdUs = 33000;
    const bool collectStats = diagnosticsEnabled_;
    WaitForFrameLatencyObject(collectStats);
    const auto renderStart = collectStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto stageStart = renderStart;

    UpdateColorPipeline(frame);
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.colorPipelineUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    const bool hasHardwareTexture = frame.HasD3DTexture() && UpdateHardwareTexture(frame);
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.hardwarePrepareUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    bool hasBgraTexture = false;
    bool hasYuvTexture = false;
    bool hasEnhancementYuvTexture = false;
    const bool enhancementYuvEnabled =
        (videoSettings_.dolbyVisionCmv4Approx || ExperimentalDoviTrimEnabled()) &&
        frame.HasEnhancementYuv();
    if (!hasHardwareTexture && frame.HasYuv()) {
        hasYuvTexture = UpdateYuvTexture(frame);
        if (hasYuvTexture && enhancementYuvEnabled) {
            hasEnhancementYuvTexture = UpdateEnhancementYuvTexture(frame);
            if (hasEnhancementYuvTexture && !felOverlayLogged_) {
                felOverlayLogged_ = true;
                const std::wstring mode =
                    frame.enhancementDovi && frame.enhancementDovi->valid ? L"nlq_merge" : L"experimental_overlay";
                LogInfo(L"dolby_vision_el_overlay upload=active mode=" + mode + L" size=" +
                        std::to_wstring(frame.enhancementYuv.width) + L"x" +
                        std::to_wstring(frame.enhancementYuv.height));
            }
        }
    }
    if (!hasHardwareTexture && !hasYuvTexture && frame.HasPixels()) {
        UpdateTexture(frame);
        hasBgraTexture = srv_ != nullptr;
    }
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.bgraUploadUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    if (!hasHardwareTexture && !hasBgraTexture && !hasYuvTexture) {
        return;
    }

    float clearColor[4] = {0.02f, 0.03f, 0.04f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    const D3D11_VIEWPORT drawViewport = LetterboxedViewport(frame.width, frame.height);
    context_->RSSetViewports(1, &drawViewport);

    const bool useNv12Path = (hasHardwareTexture && hwSrvY_ && hwSrvUV_) ||
                             (hasYuvTexture && yuvSrvY_ && yuvSrvUV_);
    if (useNv12Path && psNv12_) {
        ID3D11ShaderResourceView* yView = hasHardwareTexture ? hwSrvY_.Get() : yuvSrvY_.Get();
        ID3D11ShaderResourceView* uvView = hasHardwareTexture ? hwSrvUV_.Get() : yuvSrvUV_.Get();
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(psNv12_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[4] = {
            yView,
            uvView,
            hasEnhancementYuvTexture ? enhancementYuvSrvY_.Get() : nullptr,
            hasEnhancementYuvTexture ? enhancementYuvSrvUV_.Get() : nullptr,
        };
        context_->PSSetShaderResources(0, 4, views);
        ID3D11Buffer* constants[2] = {colorConstants_.Get(), doviConstants_.Get()};
        context_->PSSetConstantBuffers(0, 2, constants);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullViews[4] = {};
        context_->PSSetShaderResources(0, 4, nullViews);
    } else if (hasBgraTexture) {
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, srv_.GetAddressOf());
        context_->PSSetConstantBuffers(0, 1, colorConstants_.GetAddressOf());
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullView[1] = {};
        context_->PSSetShaderResources(0, 1, nullView);
    }
    const bool drewTextSubtitle = UpdateSubtitleOverlay(frame, drawViewport);
    if (drewTextSubtitle) {
        DrawSubtitleOverlay();
    }
    const bool drewBitmapSubtitle = DrawSubtitleBitmapOverlays(frame, drawViewport);
    const bool drewSubtitle = drewTextSubtitle || drewBitmapSubtitle;
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.subtitleUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    PresentFrame(kVideoPresentSyncInterval, collectStats, stageStart);
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t totalUs = ElapsedMicroseconds(renderStart, now);
        ++renderStats_.frames;
        if (hasHardwareTexture) ++renderStats_.hardwareFrames;
        if (hasBgraTexture) ++renderStats_.bgraFrames;
        if (drewSubtitle) ++renderStats_.subtitleFrames;
        if (totalUs > kSlowRenderFrameThresholdUs) ++renderStats_.slowFrames;
        renderStats_.totalRenderUs += totalUs;
        renderStats_.maxRenderUs = std::max(renderStats_.maxRenderUs, totalUs);
    }
}

void D3D11VideoRenderer::Clear() {
    D3D11QueuedVideoFrame* droppedFrame = nullptr;
    {
        std::lock_guard lock(commandMutex_);
        if (!acceptingCommands_) {
            return;
        }
        droppedFrame = pendingFrame_;
        pendingFrame_ = nullptr;
        pendingClear_ = true;
    }
    RetireQueuedFrame(droppedFrame);
    commandCv_.notify_one();
}

void D3D11VideoRenderer::ClearOnRenderThread() {
    if (!device_ || !context_ || !swapChain_ || !rtv_) return;
    ResetRenderStatsOnRenderThread();
    hardwareSrvCache_.clear();
    subtitleTextureCache_.clear();
    hwSrvUV_.Reset();
    hwSrvY_.Reset();
    float clearColor[4] = {0.02f, 0.03f, 0.04f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    swapChain_->Present(0, 0);
}

void D3D11VideoRenderer::EnableMultithreadProtection() {
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (device_ && SUCCEEDED(device_.As(&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
        LogInfo(L"multithread protection enabled");
    }
}

bool D3D11VideoRenderer::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) { LogHr(L"GetBuffer", hr); return false; }
    if (IsStopRequested()) return false;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT rtvHr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);
    if (FAILED(rtvHr)) {
        LogHr(L"CreateRenderTargetView", rtvHr);
        return false;
    }
    if (IsStopRequested()) return false;
    rtv_ = std::move(rtv);
    backBuffer_ = std::move(backBuffer);
    return true;
}

bool D3D11VideoRenderer::CreateComposition() {
    if (!swapChain_ || IsStopRequested()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) { LogHr(L"D3D device As IDXGIDevice", hr); return false; }
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_));
    if (FAILED(hr) || !dcompDevice_) { LogHr(L"DCompositionCreateDevice", hr); return false; }
    if (IsStopRequested()) return false;
    const HWND host = host_.load(std::memory_order_acquire);
    if (!host || !IsWindow(host)) return false;
    // CreateTargetForHwnd requires the target window to NOT have
    // WS_EX_NOREDIRECTIONBITMAP; videoHost_ uses dwExStyle = 0, so this holds.
    hr = dcompDevice_->CreateTargetForHwnd(host, TRUE, &dcompTarget_);
    if (FAILED(hr) || !dcompTarget_) { LogHr(L"CreateTargetForHwnd", hr); return false; }
    if (IsStopRequested()) return false;
    hr = dcompDevice_->CreateVisual(&dcompVisual_);
    if (FAILED(hr) || !dcompVisual_) { LogHr(L"CreateVisual", hr); return false; }
    if (IsStopRequested()) return false;
    hr = dcompVisual_->SetContent(swapChain_.Get());
    if (FAILED(hr)) { LogHr(L"SetContent swapchain", hr); return false; }
    if (IsStopRequested()) return false;
    hr = dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(hr)) { LogHr(L"SetRoot", hr); return false; }
    if (IsStopRequested()) return false;
    hr = dcompDevice_->Commit();
    if (FAILED(hr)) { LogHr(L"DComposition Commit", hr); return false; }
    if (IsStopRequested()) return false;
    return true;
}

bool D3D11VideoRenderer::CreatePipeline() {
    const char* vsSrc =
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "  VSOut o;\n"
        "  // Fullscreen triangle without vertex buffer.\n"
        "  o.uv = float2((id << 1) & 2, id & 2);\n"
        "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "  return o;\n"
        "}\n";
    const char* psSrc =
        "Texture2D<float4> tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer ColorConstants : register(b0) {\n"
        "  int matrixType;\n"
        "  int rangeType;\n"
        "  int transferType;\n"
        "  int outputMode;\n"
        "  int primariesType;\n"
        "  int toneMapMode;\n"
        "  float sourcePeakNits;\n"
        "  float targetPeakNits;\n"
        "  float displayPeakNits;\n"
        "  float hdrCurveEnabled;\n"
        "  float hdrCurvePointCount;\n"
        "  int doviTrimEnabled;\n"
        "  float doviTrimSlope;\n"
        "  float doviTrimOffset;\n"
        "  float doviTrimPower;\n"
        "  float doviTrimSaturation;\n"
        "  float doviTrimChromaWeight;\n"
        "  float doviTrimMsWeight;\n"
        "  float doviTrimStrength;\n"
        "  float doviTrimMidOffset;\n"
        "  float doviTrimMidContrast;\n"
        "  float doviTrimClip;\n"
        "  float doviTrimReserved0;\n"
        "  float doviTrimReserved1;\n"
        "  float4 doviActiveArea;\n"
        "  float4 hdrToneCurve[9];\n"
        "  float4 sourceUvRect;\n"
        "};\n"
        "bool outside_dovi_active_area(float2 uv) {\n"
        "  return uv.x < doviActiveArea.x || uv.y < doviActiveArea.y || uv.x >= doviActiveArea.z || uv.y >= doviActiveArea.w;\n"
        "}\n"
        "float3 pq_to_nits(float3 v) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 p = pow(saturate(v), 1.0 / m2);\n"
        "  return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);\n"
        "}\n"
        "float3 nits_to_pq(float3 nits) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 y = pow(max(nits / 10000.0, 0.0), m1);\n"
        "  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);\n"
        "}\n"
        "float hdr_curve_luma(float lumaNits) {\n"
        "  if (hdrCurveEnabled < 0.5) return lumaNits;\n"
        "  int count = clamp((int)(hdrCurvePointCount + 0.5), 2, 9);\n"
        "  float2 prev = hdrToneCurve[0].xy;\n"
        "  if (lumaNits <= prev.x) return max(prev.y, 0.0);\n"
        "  for (int i = 1; i < 9; ++i) {\n"
        "    if (i >= count) break;\n"
        "    float2 next = hdrToneCurve[i].xy;\n"
        "    if (lumaNits <= next.x) {\n"
        "      float t = saturate((lumaNits - prev.x) / max(next.x - prev.x, 0.0001));\n"
        "      return max(lerp(prev.y, next.y, t), 0.0);\n"
        "    }\n"
        "    prev = next;\n"
        "  }\n"
        "  return max(prev.y, 0.0);\n"
        "}\n"
        "float3 apply_dovi_trim_linear(float3 linearRgb, float3 lumaWeights) {\n"
        "  if (doviTrimEnabled == 0 || doviTrimStrength <= 0.0) return linearRgb;\n"
        "  float3 src = saturate(linearRgb);\n"
        "  float luma = saturate(dot(src, lumaWeights));\n"
        "  float toeMask = smoothstep(0.02, 0.18, luma);\n"
        "  float shoulderMask = 1.0 - smoothstep(0.70, 0.98, luma);\n"
        "  float midMask = toeMask * shoulderMask;\n"
        "  float pivot = 0.45;\n"
        "  float trimmedLuma = saturate(luma + doviTrimMidOffset * midMask);\n"
        "  trimmedLuma = saturate(pivot + (trimmedLuma - pivot) * (1.0 + doviTrimMidContrast * midMask));\n"
        "  float slope = max(doviTrimSlope, 0.01);\n"
        "  trimmedLuma = saturate((trimmedLuma - 0.5) * slope + 0.5 + doviTrimOffset * toeMask);\n"
        "  trimmedLuma = pow(max(trimmedLuma, 0.0), max(doviTrimPower, 0.05));\n"
        "  float clipMask = smoothstep(0.62, 1.0, trimmedLuma);\n"
        "  trimmedLuma = saturate(trimmedLuma + doviTrimClip * clipMask * (1.0 - trimmedLuma) * 0.45);\n"
        "  float3 c = saturate(src * (trimmedLuma / max(luma, 0.0001)));\n"
        "  float outLuma = saturate(dot(c, lumaWeights));\n"
        "  float sat = clamp(doviTrimSaturation * lerp(1.0, doviTrimChromaWeight, 0.25) * lerp(1.0, doviTrimMsWeight, 0.10), 0.75, 1.25);\n"
        "  c = saturate(outLuma.xxx + (c - outLuma.xxx) * sat);\n"
        "  return lerp(src, c, saturate(doviTrimStrength));\n"
        "}\n"
        "float3 apply_dovi_trim_sdr_g22(float3 encodedRgb) {\n"
        "  float3 linearRgb = pow(saturate(encodedRgb), 2.2);\n"
        "  linearRgb = apply_dovi_trim_linear(linearRgb, float3(0.2126, 0.7152, 0.0722));\n"
        "  return pow(saturate(linearRgb), 1.0 / 2.2);\n"
        "}\n"
        "float3 apply_dovi_trim_pq(float3 pqRgb) {\n"
        "  float3 weights = primariesType == 2 ? float3(0.2627, 0.6780, 0.0593) : float3(0.2126, 0.7152, 0.0722);\n"
        "  return apply_dovi_trim_linear(saturate(pqRgb), weights);\n"
        "}\n"
        "float3 apply_dovi_trim_nits(float3 nits, float3 lumaWeights) {\n"
        "  return pq_to_nits(apply_dovi_trim_linear(nits_to_pq(nits), lumaWeights));\n"
        "}\n"
        "float3 apply_hdr_tone_curve_pq(float3 pqRgb) {\n"
        "  float3 trimmedPq = apply_dovi_trim_pq(pqRgb);\n"
        "  if (outputMode != 1 || transferType != 2 || primariesType != 2 || hdrCurveEnabled < 0.5) return trimmedPq;\n"
        "  float3 nits = max(pq_to_nits(trimmedPq), 0.0);\n"
        "  float luma = max(dot(nits, float3(0.2627, 0.6780, 0.0593)), 0.0);\n"
        "  float mapped = hdr_curve_luma(luma);\n"
        "  float scale = mapped / max(luma, 0.0001);\n"
        "  return saturate(nits_to_pq(nits * scale));\n"
        "}\n"
        "float tone_map_exposure() {\n"
        "  if (doviTrimEnabled != 0 && outputMode == 0) {\n"
        "    if (toneMapMode == 2) return 0.58;\n"
        "    if (toneMapMode == 3) return 0.82;\n"
        "    return 0.68;\n"
        "  }\n"
        "  if (toneMapMode == 2) return 0.72;\n"
        "  if (toneMapMode == 3) return 0.95;\n"
        "  return 0.80;\n"
        "}\n"
        "float tone_map_knee(float target) {\n"
        "  if (toneMapMode == 2) return target * 0.40;\n"
        "  if (toneMapMode == 3) return target * 0.55;\n"
        "  return target * 0.45;\n"
        "}\n"
        "float tone_map_luma_to_target(float lumaNits) {\n"
        "  float target = max(targetPeakNits, 1.0);\n"
        "  float source = max(sourcePeakNits * tone_map_exposure(), target + 1.0);\n"
        "  float knee = clamp(tone_map_knee(target), 1.0, target * 0.98);\n"
        "  float y = max(lumaNits, 0.0);\n"
        "  if (y <= knee) return y;\n"
        "  float shoulder = max(target - knee, 0.0001);\n"
        "  float peak = max(source, y);\n"
        "  float x = min(y, peak) - knee;\n"
        "  float mapped = knee + shoulder * log(1.0 + x / shoulder) / log(1.0 + (peak - knee) / shoulder);\n"
        "  return min(mapped, target);\n"
        "}\n"
        "float3 scale_luma_to_sdr(float3 nits, float3 lumaWeights) {\n"
        "  float exposure = tone_map_exposure();\n"
        "  float3 working = max(nits, 0.0) * exposure;\n"
        "  float luma = max(dot(working, lumaWeights), 0.0);\n"
        "  float mappedLuma = tone_map_luma_to_target(luma);\n"
        "  float scale = mappedLuma / max(luma, 0.0001);\n"
        "  return working * scale / max(targetPeakNits, 1.0);\n"
        "}\n"
        "float3 bt2020_to_bt709(float3 c) {\n"
        "  return float3(dot(float3(1.6605, -0.5876, -0.0728), c),\n"
        "                dot(float3(-0.1246, 1.1329, -0.0083), c),\n"
        "                dot(float3(-0.0182, -0.1006, 1.1187), c));\n"
        "}\n"
        "float3 compress_gamut_preserve_luma(float3 linear709) {\n"
        "  float luma = saturate(dot(linear709, float3(0.2126, 0.7152, 0.0722)));\n"
        "  float3 chroma = linear709 - float3(luma, luma, luma);\n"
        "  float scale = 1.0;\n"
        "  if (chroma.r > 0.0) scale = min(scale, (1.0 - luma) / chroma.r);\n"
        "  else if (chroma.r < 0.0) scale = min(scale, -luma / chroma.r);\n"
        "  if (chroma.g > 0.0) scale = min(scale, (1.0 - luma) / chroma.g);\n"
        "  else if (chroma.g < 0.0) scale = min(scale, -luma / chroma.g);\n"
        "  if (chroma.b > 0.0) scale = min(scale, (1.0 - luma) / chroma.b);\n"
        "  else if (chroma.b < 0.0) scale = min(scale, -luma / chroma.b);\n"
        "  scale = saturate(scale);\n"
        "  return saturate(float3(luma, luma, luma) + chroma * scale);\n"
        "}\n"
        "float srgb_encode_channel(float c) {\n"
        "  c = saturate(c);\n"
        "  return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
        "}\n"
        "float3 encode_sdr_g22(float3 linearRgb) {\n"
        "  return float3(srgb_encode_channel(linearRgb.r), srgb_encode_channel(linearRgb.g), srgb_encode_channel(linearRgb.b));\n"
        "}\n"
        "float3 apply_sdr_contrast_recovery(float3 encodedRgb) {\n"
        "  if (doviTrimEnabled == 0) return saturate(encodedRgb);\n"
        "  float3 src = saturate(encodedRgb);\n"
        "  float y = saturate(dot(src, float3(0.2126, 0.7152, 0.0722)));\n"
        "  float amount = 0.06;\n"
        "  float pivot = 0.42;\n"
        "  float linearContrast = saturate((y - pivot) * (1.0 + amount) + pivot);\n"
        "  float filmicContrast = saturate(y + amount * (y - pivot) * 4.0 * y * (1.0 - y));\n"
        "  float y2 = saturate(lerp(linearContrast, filmicContrast, 0.65));\n"
        "  return saturate(src * (y2 / max(y, 0.0001)));\n"
        "}\n"
        "float3 bt2020_nits_to_sdr_vivid(float3 bt2020Nits) {\n"
        "  float3 rn = max(bt2020Nits, 0.0) / 200.0;\n"
        "  float L = max(dot(rn, float3(0.2627, 0.6780, 0.0593)), 0.000001);\n"
        "  float Lt = L / (1.0 + L);\n"
        "  float3 linear709 = bt2020_to_bt709(rn * (Lt / L));\n"
        "  return encode_sdr_g22(saturate(linear709));\n"
        "}\n"
        "float3 bt2020_pq_to_sdr(float3 pqRgb) {\n"
        "  float3 trimmedPq = apply_dovi_trim_pq(pqRgb);\n"
        "  float3 trimmedNits = pq_to_nits(trimmedPq);\n"
        "  if (doviTrimEnabled != 0) return bt2020_nits_to_sdr_vivid(trimmedNits);\n"
        "  float3 linear2020 = scale_luma_to_sdr(trimmedNits, float3(0.2627, 0.6780, 0.0593));\n"
        "  float3 linear709 = bt2020_to_bt709(linear2020);\n"
        "  return apply_sdr_contrast_recovery(encode_sdr_g22(compress_gamut_preserve_luma(linear709)));\n"
        "}\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float2 displayUv = saturate(uv);\n"
        "  if (outside_dovi_active_area(displayUv)) return float4(0.0, 0.0, 0.0, 1.0);\n"
        "  float2 sampleUv = lerp(sourceUvRect.xy, sourceUvRect.zw, displayUv);\n"
        "  float4 c = tex.Sample(samp, sampleUv);\n"
        "  if (outputMode == 1) return float4(apply_hdr_tone_curve_pq(c.rgb), c.a);\n"
        "  if (transferType == 2 && primariesType == 2) return float4(bt2020_pq_to_sdr(c.rgb), c.a);\n"
        "  return float4(apply_dovi_trim_sdr_g22(c.rgb), c.a);\n"
        "}\n";
    const char* psNv12Src =
        "Texture2D<float> texY : register(t0);\n"
        "Texture2D<float2> texUV : register(t1);\n"
        "Texture2D<float> texFelY : register(t2);\n"
        "Texture2D<float2> texFelUV : register(t3);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer ColorConstants : register(b0) {\n"
        "  int matrixType;\n"
        "  int rangeType;\n"
        "  int transferType;\n"
        "  int outputMode;\n"
        "  int primariesType;\n"
        "  int toneMapMode;\n"
        "  float sourcePeakNits;\n"
        "  float targetPeakNits;\n"
        "  float displayPeakNits;\n"
        "  float hdrCurveEnabled;\n"
        "  float hdrCurvePointCount;\n"
        "  int doviTrimEnabled;\n"
        "  float doviTrimSlope;\n"
        "  float doviTrimOffset;\n"
        "  float doviTrimPower;\n"
        "  float doviTrimSaturation;\n"
        "  float doviTrimChromaWeight;\n"
        "  float doviTrimMsWeight;\n"
        "  float doviTrimStrength;\n"
        "  float doviTrimMidOffset;\n"
        "  float doviTrimMidContrast;\n"
        "  float doviTrimClip;\n"
        "  float doviTrimReserved0;\n"
        "  float doviTrimReserved1;\n"
        "  float4 doviActiveArea;\n"
        "  float4 hdrToneCurve[9];\n"
        "  float4 sourceUvRect;\n"
        "};\n"
        "bool outside_dovi_active_area(float2 uv) {\n"
        "  return uv.x < doviActiveArea.x || uv.y < doviActiveArea.y || uv.x >= doviActiveArea.z || uv.y >= doviActiveArea.w;\n"
        "}\n"
        "float3 ycbcr_to_rgb(float y, float2 cbcr) {\n"
        "  float yOffset = rangeType == 1 ? 0.0 : 16.0 / 255.0;\n"
        "  float yScale = rangeType == 1 ? 1.0 : 255.0 / 219.0;\n"
        "  float cScale = rangeType == 1 ? 1.0 : 255.0 / 224.0;\n"
        "  float yy = max(0.0, (y - yOffset) * yScale);\n"
        "  float cb = (cbcr.x - 0.5) * cScale;\n"
        "  float cr = (cbcr.y - 0.5) * cScale;\n"
        "  float kr = 0.2126;\n"
        "  float kb = 0.0722;\n"
        "  if (matrixType == 2) { kr = 0.2990; kb = 0.1140; }\n"
        "  else if (matrixType == 3) { kr = 0.2627; kb = 0.0593; }\n"
        "  float kg = 1.0 - kr - kb;\n"
        "  float3 rgb;\n"
        "  rgb.r = yy + (2.0 - 2.0 * kr) * cr;\n"
        "  rgb.b = yy + (2.0 - 2.0 * kb) * cb;\n"
        "  rgb.g = yy - ((2.0 * kb * (1.0 - kb)) / kg) * cb - ((2.0 * kr * (1.0 - kr)) / kg) * cr;\n"
        "  return max(rgb, 0.0);\n"
        "}\n"
        "void apply_fel_overlay(inout float y, inout float2 cbcr, float2 uv) {\n"
        "  if (doviTrimReserved0 < 0.5) return;\n"
        "  float strength = max(doviTrimReserved1, 0.0);\n"
        "  float felY = texFelY.Sample(samp, uv);\n"
        "  float2 felCbcr = texFelUV.Sample(samp, uv);\n"
        "  float lumaResidual = (felY - 0.5) * 2.0;\n"
        "  float2 chromaResidual = (felCbcr - float2(0.5, 0.5)) * 2.0;\n"
        "  y = saturate(y + lumaResidual * 0.10 * strength);\n"
        "  cbcr = saturate(cbcr + chromaResidual * 0.05 * strength);\n"
        "}\n"
        "float3 pq_to_nits(float3 v) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 p = pow(saturate(v), 1.0 / m2);\n"
        "  return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);\n"
        "}\n"
        "float3 nits_to_pq(float3 nits) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 y = pow(max(nits / 10000.0, 0.0), m1);\n"
        "  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);\n"
        "}\n"
        "float hdr_curve_luma(float lumaNits) {\n"
        "  if (hdrCurveEnabled < 0.5) return lumaNits;\n"
        "  int count = clamp((int)(hdrCurvePointCount + 0.5), 2, 9);\n"
        "  float2 prev = hdrToneCurve[0].xy;\n"
        "  if (lumaNits <= prev.x) return max(prev.y, 0.0);\n"
        "  for (int i = 1; i < 9; ++i) {\n"
        "    if (i >= count) break;\n"
        "    float2 next = hdrToneCurve[i].xy;\n"
        "    if (lumaNits <= next.x) {\n"
        "      float t = saturate((lumaNits - prev.x) / max(next.x - prev.x, 0.0001));\n"
        "      return max(lerp(prev.y, next.y, t), 0.0);\n"
        "    }\n"
        "    prev = next;\n"
        "  }\n"
        "  return max(prev.y, 0.0);\n"
        "}\n"
        "float3 apply_dovi_trim_linear(float3 linearRgb, float3 lumaWeights) {\n"
        "  if (doviTrimEnabled == 0 || doviTrimStrength <= 0.0) return linearRgb;\n"
        "  float3 src = saturate(linearRgb);\n"
        "  float luma = saturate(dot(src, lumaWeights));\n"
        "  float toeMask = smoothstep(0.02, 0.18, luma);\n"
        "  float shoulderMask = 1.0 - smoothstep(0.70, 0.98, luma);\n"
        "  float midMask = toeMask * shoulderMask;\n"
        "  float pivot = 0.45;\n"
        "  float trimmedLuma = saturate(luma + doviTrimMidOffset * midMask);\n"
        "  trimmedLuma = saturate(pivot + (trimmedLuma - pivot) * (1.0 + doviTrimMidContrast * midMask));\n"
        "  float slope = max(doviTrimSlope, 0.01);\n"
        "  trimmedLuma = saturate((trimmedLuma - 0.5) * slope + 0.5 + doviTrimOffset * toeMask);\n"
        "  trimmedLuma = pow(max(trimmedLuma, 0.0), max(doviTrimPower, 0.05));\n"
        "  float clipMask = smoothstep(0.62, 1.0, trimmedLuma);\n"
        "  trimmedLuma = saturate(trimmedLuma + doviTrimClip * clipMask * (1.0 - trimmedLuma) * 0.45);\n"
        "  float3 c = saturate(src * (trimmedLuma / max(luma, 0.0001)));\n"
        "  float outLuma = saturate(dot(c, lumaWeights));\n"
        "  float sat = clamp(doviTrimSaturation * lerp(1.0, doviTrimChromaWeight, 0.25) * lerp(1.0, doviTrimMsWeight, 0.10), 0.75, 1.25);\n"
        "  c = saturate(outLuma.xxx + (c - outLuma.xxx) * sat);\n"
        "  return lerp(src, c, saturate(doviTrimStrength));\n"
        "}\n"
        "float3 apply_dovi_trim_nits(float3 nits, float3 lumaWeights) {\n"
        "  return pq_to_nits(apply_dovi_trim_linear(nits_to_pq(nits), lumaWeights));\n"
        "}\n"
        "float3 apply_hdr_tone_curve_nits(float3 bt2020Nits) {\n"
        "  float3 nits = apply_dovi_trim_nits(max(bt2020Nits, 0.0), float3(0.2627, 0.6780, 0.0593));\n"
        "  if (hdrCurveEnabled < 0.5) return saturate(nits_to_pq(nits));\n"
        "  float luma = max(dot(nits, float3(0.2627, 0.6780, 0.0593)), 0.0);\n"
        "  float mapped = hdr_curve_luma(luma);\n"
        "  float scale = mapped / max(luma, 0.0001);\n"
        "  return saturate(nits_to_pq(nits * scale));\n"
        "}\n"
        "float3 apply_hdr_tone_curve_pq(float3 pqRgb) {\n"
        "  if (hdrCurveEnabled < 0.5) return saturate(pqRgb);\n"
        "  return apply_hdr_tone_curve_nits(pq_to_nits(pqRgb));\n"
        "}\n"
        "float3 hlg_to_nits(float3 v) {\n"
        "  const float a = 0.17883277;\n"
        "  const float b = 0.28466892;\n"
        "  const float c = 0.55991073;\n"
        "  float3 low = (v * v) / 3.0;\n"
        "  float3 high = (exp((v - c) / a) + b) / 12.0;\n"
        "  float3 scene = lerp(high, low, step(v, float3(0.5, 0.5, 0.5)));\n"
        "  return max(scene, 0.0) * max(sourcePeakNits, 100.0);\n"
        "}\n"
        "float3 bt2020_to_bt709(float3 c) {\n"
        "  return float3(dot(float3(1.6605, -0.5876, -0.0728), c),\n"
        "                dot(float3(-0.1246, 1.1329, -0.0083), c),\n"
        "                dot(float3(-0.0182, -0.1006, 1.1187), c));\n"
        "}\n"
        "float3 bt709_to_bt2020(float3 c) {\n"
        "  return float3(dot(float3(0.6274, 0.3293, 0.0433), c),\n"
        "                dot(float3(0.0691, 0.9195, 0.0114), c),\n"
        "                dot(float3(0.0164, 0.0880, 0.8956), c));\n"
        "}\n"
        "float3 dovi_lms_to_bt2020(float3 lms) {\n"
        "  return float3(dot(float3(3.0644188, -2.1659768, 0.1015582), lms),\n"
        "                dot(float3(-0.6561211, 1.7855412, -0.1294375), lms),\n"
        "                dot(float3(0.0173632, -0.0472515, 1.0300425), lms));\n"
        "}\n"
        "float3 encoded_to_nits(float3 rgb) {\n"
        "  if (transferType == 2) return pq_to_nits(rgb);\n"
        "  if (transferType == 3) return hlg_to_nits(rgb);\n"
        "  return pow(saturate(rgb), 2.2) * 100.0;\n"
        "}\n"
        "float tone_map_exposure() {\n"
        "  if (doviTrimEnabled != 0 && outputMode == 0) {\n"
        "    if (toneMapMode == 2) return 0.58;\n"
        "    if (toneMapMode == 3) return 0.82;\n"
        "    return 0.68;\n"
        "  }\n"
        "  if (toneMapMode == 2) return 0.72;\n"
        "  if (toneMapMode == 3) return 0.95;\n"
        "  return 0.80;\n"
        "}\n"
        "float tone_map_knee(float target) {\n"
        "  if (toneMapMode == 2) return target * 0.40;\n"
        "  if (toneMapMode == 3) return target * 0.55;\n"
        "  return target * 0.45;\n"
        "}\n"
        "float tone_map_luma_to_target(float lumaNits) {\n"
        "  float target = max(targetPeakNits, 1.0);\n"
        "  float source = max(sourcePeakNits * tone_map_exposure(), target + 1.0);\n"
        "  float knee = clamp(tone_map_knee(target), 1.0, target * 0.98);\n"
        "  float y = max(lumaNits, 0.0);\n"
        "  if (y <= knee) return y;\n"
        "  float shoulder = max(target - knee, 0.0001);\n"
        "  float peak = max(source, y);\n"
        "  float x = min(y, peak) - knee;\n"
        "  float mapped = knee + shoulder * log(1.0 + x / shoulder) / log(1.0 + (peak - knee) / shoulder);\n"
        "  return min(mapped, target);\n"
        "}\n"
        "float3 scale_luma_to_sdr(float3 nits, float3 lumaWeights) {\n"
        "  float exposure = tone_map_exposure();\n"
        "  float3 working = max(nits, 0.0) * exposure;\n"
        "  float luma = max(dot(working, lumaWeights), 0.0);\n"
        "  float mappedLuma = tone_map_luma_to_target(luma);\n"
        "  float scale = mappedLuma / max(luma, 0.0001);\n"
        "  return working * scale / max(targetPeakNits, 1.0);\n"
        "}\n"
        "float3 compress_gamut_preserve_luma(float3 linear709) {\n"
        "  float luma = saturate(dot(linear709, float3(0.2126, 0.7152, 0.0722)));\n"
        "  float3 chroma = linear709 - float3(luma, luma, luma);\n"
        "  float scale = 1.0;\n"
        "  if (chroma.r > 0.0) scale = min(scale, (1.0 - luma) / chroma.r);\n"
        "  else if (chroma.r < 0.0) scale = min(scale, -luma / chroma.r);\n"
        "  if (chroma.g > 0.0) scale = min(scale, (1.0 - luma) / chroma.g);\n"
        "  else if (chroma.g < 0.0) scale = min(scale, -luma / chroma.g);\n"
        "  if (chroma.b > 0.0) scale = min(scale, (1.0 - luma) / chroma.b);\n"
        "  else if (chroma.b < 0.0) scale = min(scale, -luma / chroma.b);\n"
        "  scale = saturate(scale);\n"
        "  return saturate(float3(luma, luma, luma) + chroma * scale);\n"
        "}\n"
        "float srgb_encode_channel(float c) {\n"
        "  c = saturate(c);\n"
        "  return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
        "}\n"
        "float3 encode_sdr_g22(float3 linearRgb) {\n"
        "  return float3(srgb_encode_channel(linearRgb.r), srgb_encode_channel(linearRgb.g), srgb_encode_channel(linearRgb.b));\n"
        "}\n"
        "float3 apply_sdr_contrast_recovery(float3 encodedRgb) {\n"
        "  if (doviTrimEnabled == 0) return saturate(encodedRgb);\n"
        "  float3 src = saturate(encodedRgb);\n"
        "  float y = saturate(dot(src, float3(0.2126, 0.7152, 0.0722)));\n"
        "  float amount = 0.06;\n"
        "  float pivot = 0.42;\n"
        "  float linearContrast = saturate((y - pivot) * (1.0 + amount) + pivot);\n"
        "  float filmicContrast = saturate(y + amount * (y - pivot) * 4.0 * y * (1.0 - y));\n"
        "  float y2 = saturate(lerp(linearContrast, filmicContrast, 0.65));\n"
        "  return saturate(src * (y2 / max(y, 0.0001)));\n"
        "}\n"
        "float3 bt2020_nits_to_sdr_vivid(float3 bt2020Nits) {\n"
        "  float3 rn = max(bt2020Nits, 0.0) / 200.0;\n"
        "  float L = max(dot(rn, float3(0.2627, 0.6780, 0.0593)), 0.000001);\n"
        "  float Lt = L / (1.0 + L);\n"
        "  float3 linear709 = bt2020_to_bt709(rn * (Lt / L));\n"
        "  return encode_sdr_g22(saturate(linear709));\n"
        "}\n"
        "float3 bt2020_nits_to_sdr(float3 bt2020Nits) {\n"
        "  float3 trimmedNits = apply_dovi_trim_nits(bt2020Nits, float3(0.2627, 0.6780, 0.0593));\n"
        "  if (doviTrimEnabled != 0) return bt2020_nits_to_sdr_vivid(trimmedNits);\n"
        "  float3 linear2020 = scale_luma_to_sdr(trimmedNits, float3(0.2627, 0.6780, 0.0593));\n"
        "  float3 linear709 = bt2020_to_bt709(linear2020);\n"
        "  return apply_sdr_contrast_recovery(encode_sdr_g22(compress_gamut_preserve_luma(linear709)));\n"
        "}\n"
        "float3 bt709_nits_to_sdr(float3 bt709Nits) {\n"
        "  float3 trimmedNits = apply_dovi_trim_nits(bt709Nits, float3(0.2126, 0.7152, 0.0722));\n"
        "  float3 linear709 = scale_luma_to_sdr(trimmedNits, float3(0.2126, 0.7152, 0.0722));\n"
        "  return apply_sdr_contrast_recovery(encode_sdr_g22(compress_gamut_preserve_luma(linear709)));\n"
        "}\n"
        // ---- Dolby Vision reshaping (register b1) ----
        // Constants packed to match DoviShaderConstants on the C++ side.
        "cbuffer DoviConstants : register(b1) {\n"
        "  float4 pivots[3][3];\n"
        "  float4 pieceMeta[3][2];\n"
        "  float4 polyCoef[3][8];\n"
        "  float4 mmrCoef[3][8][6];\n"
        "  float4 yccToRgb[3];\n"
        "  float4 yccOffset;\n"
        "  float4 rgbToLms[3];\n"
        "  float4 curveMeta;\n"
        "  float4 nlqParams[3];\n"
        "  float4 composerMeta;\n"
        "  float4 composerScale;\n"
        "  int doviProfile;\n"
        "  int doviCompatId;\n"
        "  int doviEnabled;\n"
        "  float doviSampleScale;\n"
        "};\n"
        "float3 dovi_ycc_to_rgb(float3 ycc) {\n"
        "  return float3(dot(yccToRgb[0].xyz, ycc),\n"
        "                dot(yccToRgb[1].xyz, ycc),\n"
        "                dot(yccToRgb[2].xyz, ycc));\n"
        "}\n"
        "float3 dovi_rgb_to_lms(float3 rgb) {\n"
        "  return float3(dot(rgbToLms[0].xyz, rgb),\n"
        "                dot(rgbToLms[1].xyz, rgb),\n"
        "                dot(rgbToLms[2].xyz, rgb));\n"
        "}\n"
        // Read pivot i for component c. Returns 1.0 for out-of-range indices.
        "float dovi_pivot(int c, int i) {\n"
        "  if (i >= 9) return 1.0;\n"
        "  int slot = i / 3;\n"
        "  int comp = i % 3;\n"
        "  return pivots[c][slot][comp];\n"
        "}\n"
        "int dovi_num_pivots(int c) {\n"
        "  return clamp((int)(curveMeta[c] + 0.5), 2, 9);\n"
        "}\n"
        "float dovi_offset_scale() {\n"
        "  return curveMeta.w > 0.0 ? curveMeta.w : 1.0;\n"
        "}\n"
        // Read mapping method for piece s of component c. Returns -1 if none.
        "int dovi_method(int c, int s) {\n"
        "  if (s >= 8) return -1;\n"
        "  int slot = s / 4;\n"
        "  int comp = s % 4;\n"
        "  return (int)pieceMeta[c][slot][comp];\n"
        "}\n"
        "float dovi_round_nearest(float v) {\n"
        "  return floor(v + 0.5);\n"
        "}\n"
        "float dovi_round_signed(float v) {\n"
        "  return v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5);\n"
        "}\n"
        "float dovi_norm16_to_code(float v) {\n"
        "  return dovi_round_nearest(saturate(v) * 65535.0);\n"
        "}\n"
        "float dovi_code16_to_norm(float code) {\n"
        "  return saturate(code / 65535.0);\n"
        "}\n"
        "float dovi_quantize_norm16(float v) {\n"
        "  return dovi_code16_to_norm(dovi_norm16_to_code(v));\n"
        "}\n"
        // Find the piece index whose [pivot[i], pivot[i+1]) range contains v.
        "int dovi_find_piece(int c, float v, int numPivots) {\n"
        "  int lastPiece = max(0, numPivots - 2);\n"
        "  for (int i = 0; i < 8; ++i) {\n"
        "    if (i >= lastPiece) return lastPiece;\n"
        "    float nextPivot = (i + 1 <= 8) ? dovi_pivot(c, i + 1) : 1.0;\n"
        "    if (v < nextPivot) return i;\n"
        "  }\n"
        "  return lastPiece;\n"
        "}\n"
        // Polynomial reshape: y = c0 + c1*x + c2*x^2.
        "float dovi_reshape_poly(int c, int s, float x) {\n"
        "  float3 coef = polyCoef[c][s].xyz;\n"
        "  return coef.x + coef.y * x + coef.z * x * x;\n"
        "}\n"
        "float dovi_reshape_poly_p7(int c, int s, float x) {\n"
        "  float3 coef = polyCoef[c][s].xyz;\n"
        "  float blBits = clamp(composerScale.z, 8.0, 16.0);\n"
        "  float sCode = saturate(x) * (exp2(blBits) - 1.0);\n"
        "  float v16 = coef.x * exp2(20.0) +\n"
        "              coef.y * sCode * exp2(20.0 - blBits) +\n"
        "              coef.z * sCode * sCode * exp2(20.0 - 2.0 * blBits);\n"
        "  return dovi_code16_to_norm(dovi_round_nearest(clamp(v16 / 16.0, 0.0, 65535.0)));\n"
        "}\n"
        // MMR reshape: needs the full reshaped RGB signal. Operates on the
        // per-component target using up to 3 terms of 7 coefficients over the
        // 3 RGB channels + constant. (Coefficients packed in mmrCoef[c][s][0..5].)
        "float dovi_reshape_mmr(int c, int s, float3 rgb) {\n"
        "  float m[24] = (float[24])0;\n"
        "  for (int f = 0; f < 6; ++f) {\n"
        "    m[f*4+0] = mmrCoef[c][s][f].x;\n"
        "    m[f*4+1] = mmrCoef[c][s][f].y;\n"
        "    m[f*4+2] = mmrCoef[c][s][f].z;\n"
        "    m[f*4+3] = mmrCoef[c][s][f].w;\n"
        "  }\n"
        "  int order = clamp((int)(polyCoef[c][s].w + 0.5), 1, 3);\n"
        "  float4 sigX = float4(rgb.x * rgb.y, rgb.x * rgb.z, rgb.y * rgb.z, rgb.x * rgb.y * rgb.z);\n"
        "  float result = m[0];\n"
        "  result += dot(float3(m[1], m[2], m[3]), rgb);\n"
        "  result += dot(float4(m[4], m[5], m[6], m[7]), sigX);\n"
        "  if (order >= 2) {\n"
        "    float3 rgb2 = rgb * rgb;\n"
        "    float4 sigX2 = sigX * sigX;\n"
        "    result += dot(float3(m[8], m[9], m[10]), rgb2);\n"
        "    result += dot(float4(m[11], m[12], m[13], m[14]), sigX2);\n"
        "    if (order >= 3) {\n"
        "      result += dot(float3(m[15], m[16], m[17]), rgb2 * rgb);\n"
        "      result += dot(float4(m[18], m[19], m[20], m[21]), sigX2 * sigX);\n"
        "    }\n"
        "  }\n"
        "  return result;\n"
        "}\n"
        // Apply the RPU reshaping curve to a (I, Ct, Cp) triplet in the
        // signaled PQ-domain sample space. The result is still PQ encoded and
        // still in Dolby's intermediate RGB/LMS-oriented representation; it
        // must be PQ-linearized and run through rgbToLms + LMS->BT.2020 before
        // presentation.
        // Order (per libplacebo pl_shader_dovi_reshape + pl_color_repr_decode):
        //   1. piece-wise reshape per component (poly or MMR), input is the
        //      raw signaled value; pivots are in the same signaled domain.
        //   2. subtract yccOffset (nonlinear neutral), then multiply by
        //      yccToRgb. (Equivalent to matrix*(x-offset); libplacebo folds
        //      the offset into the matrix constant term as -(M*offset).)
        "float dovi_map_component(int c, float3 signal, int clampToPivots, int p7Polynomial) {\n"
        "    int numPivots = dovi_num_pivots(c);\n"
        "    float v = signal[c];\n"
        "    int s = dovi_find_piece(c, v, numPivots);\n"
        "    int method = dovi_method(c, s);\n"
        "    float mapped = v;\n"
        "    if (method == 0) {\n"
        "      mapped = p7Polynomial != 0 ? dovi_reshape_poly_p7(c, s, v) : dovi_reshape_poly(c, s, v);\n"
        "    } else if (method == 1) {\n"
        "      mapped = dovi_reshape_mmr(c, s, signal);\n"
        "    }\n"
        "    return clampToPivots != 0 ? clamp(mapped, dovi_pivot(c, 0), dovi_pivot(c, numPivots - 1)) : saturate(mapped);\n"
        "}\n"
        "float3 dovi_apply_mapping(float3 signal, int clampToPivots) {\n"
        "  float3 mapped = signal;\n"
        "  for (int c = 0; c < 3; ++c) {\n"
        "    mapped[c] = dovi_map_component(c, signal, clampToPivots, 0);\n"
        "  }\n"
        "  return saturate(mapped);\n"
        "}\n"
        "float dovi_cubic_weight(float x) {\n"
        "  x = abs(x);\n"
        "  if (x <= 1.0) return ((1.5 * x - 2.5) * x * x + 1.0);\n"
        "  if (x < 2.0) return (((-0.5 * x + 2.5) * x - 4.0) * x + 2.0);\n"
        "  return 0.0;\n"
        "}\n"
        "int2 dovi_clamp_texel(int2 p, uint w, uint h) {\n"
        "  return int2(clamp(p.x, 0, (int)w - 1), clamp(p.y, 0, (int)h - 1));\n"
        "}\n"
        "float dovi_sample_fel_y(float2 uv) {\n"
        "  if (composerScale.w < 0.5) return texFelY.Sample(samp, uv);\n"
        "  uint w = 0;\n"
        "  uint h = 0;\n"
        "  texFelY.GetDimensions(w, h);\n"
        "  float2 coord = uv * float2((float)w, (float)h) - 0.5;\n"
        "  int2 base = int2(floor(coord));\n"
        "  float2 frac = coord - floor(coord);\n"
        "  float sum = 0.0;\n"
        "  float weightSum = 0.0;\n"
        "  [unroll] for (int j = -1; j <= 2; ++j) {\n"
        "    float wy = dovi_cubic_weight((float)j - frac.y);\n"
        "    [unroll] for (int i = -1; i <= 2; ++i) {\n"
        "      float wx = dovi_cubic_weight((float)i - frac.x);\n"
        "      float wt = wx * wy;\n"
        "      int2 p = dovi_clamp_texel(base + int2(i, j), w, h);\n"
        "      sum += texFelY.Load(int3(p, 0)) * wt;\n"
        "      weightSum += wt;\n"
        "    }\n"
        "  }\n"
        "  return saturate(sum / max(weightSum, 0.000001));\n"
        "}\n"
        "float2 dovi_sample_fel_uv(float2 uv) {\n"
        "  if (composerScale.w < 0.5) return texFelUV.Sample(samp, uv);\n"
        "  uint w = 0;\n"
        "  uint h = 0;\n"
        "  texFelUV.GetDimensions(w, h);\n"
        "  float2 coord = uv * float2((float)w, (float)h) - 0.5;\n"
        "  int2 base = int2(floor(coord));\n"
        "  float2 frac = coord - floor(coord);\n"
        "  float2 sum = float2(0.0, 0.0);\n"
        "  float weightSum = 0.0;\n"
        "  [unroll] for (int j = -1; j <= 2; ++j) {\n"
        "    float wy = dovi_cubic_weight((float)j - frac.y);\n"
        "    [unroll] for (int i = -1; i <= 2; ++i) {\n"
        "      float wx = dovi_cubic_weight((float)i - frac.x);\n"
        "      float wt = wx * wy;\n"
        "      int2 p = dovi_clamp_texel(base + int2(i, j), w, h);\n"
        "      sum += texFelUV.Load(int3(p, 0)) * wt;\n"
        "      weightSum += wt;\n"
        "    }\n"
        "  }\n"
        "  return saturate(sum / max(weightSum, 0.000001));\n"
        "}\n"
        "float dovi_chroma_site_luma(float2 uv) {\n"
        "  uint w = 0;\n"
        "  uint h = 0;\n"
        "  texY.GetDimensions(w, h);\n"
        "  float2 coord = uv * float2((float)w, (float)h) - 0.5;\n"
        "  int2 base = int2(floor(coord));\n"
        "  float4 weights = float4(1.0, 3.0, 3.0, 1.0) / 8.0;\n"
        "  float sum = 0.0;\n"
        "  [unroll] for (int j = 0; j < 4; ++j) {\n"
        "    [unroll] for (int i = 0; i < 4; ++i) {\n"
        "      int2 p = dovi_clamp_texel(base + int2(i - 1, j - 1), w, h);\n"
        "      sum += texY.Load(int3(p, 0)) * weights[i] * weights[j];\n"
        "    }\n"
        "  }\n"
        "  return saturate(sum * doviSampleScale);\n"
        "}\n"
        "float3 dovi_apply_mapping_p7(float3 pixelSignal, float3 chromaSignal) {\n"
        "  return float3(dovi_quantize_norm16(dovi_map_component(0, pixelSignal, 0, 1)),\n"
        "                dovi_quantize_norm16(dovi_map_component(1, chromaSignal, 0, 1)),\n"
        "                dovi_quantize_norm16(dovi_map_component(2, chromaSignal, 0, 1)));\n"
        "}\n"
        "float3 dovi_reshape(float3 ipt) {\n"
        "  float3 mapped = dovi_apply_mapping(ipt, 1);\n"
        "  float3 shifted = mapped - yccOffset.xyz * dovi_offset_scale();\n"
        "  float3 pqRgb = dovi_ycc_to_rgb(shifted);\n"
        "  return pqRgb;\n"
        "}\n"
        "float dovi_inverse_nlq_code(float eSample, int c) {\n"
        "  if (composerMeta.x < 0.5 || composerMeta.y != 0.0) return 0.0;\n"
        "  float elBits = clamp(composerMeta.z, 8.0, 16.0);\n"
        "  float coefDenom = max(composerScale.x, 0.0);\n"
        "  float elMax = exp2(elBits) - 1.0;\n"
        "  float eCode = floor(saturate(eSample * composerScale.y) * elMax + 0.5);\n"
        "  float offset = nlqParams[c].x;\n"
        "  float vdrMax = nlqParams[c].y;\n"
        "  float slope = nlqParams[c].z;\n"
        "  float threshold = nlqParams[c].w;\n"
        "  float rr = eCode - offset;\n"
        "  if (abs(rr) < 0.5) return 0.0;\n"
        "  float sgn = rr < 0.0 ? -1.0 : 1.0;\n"
        "  float bitScale = exp2(10.0 - elBits);\n"
        "  float rrLinear = sgn * max(abs(rr) * 2.0 - 1.0, 0.0) * bitScale;\n"
        "  float dq = rrLinear * slope + threshold * exp2(10.0 - elBits + 1.0) * sgn;\n"
        "  float limit = max(vdrMax * exp2(10.0 - elBits + 1.0), 0.0);\n"
        "  dq = clamp(dq, -limit, limit);\n"
        "  float residual16 = dq / exp2(coefDenom - 5.0 - elBits);\n"
        "  return dovi_round_signed(residual16);\n"
        "}\n"
        "float dovi_vdr_output_code(float code16) {\n"
        "  float bits = clamp(composerMeta.w, 8.0, 16.0);\n"
        "  float maxCode = exp2(bits) - 1.0;\n"
        "  float quantStep = exp2(16.0 - bits);\n"
        "  float code = dovi_round_nearest(clamp(code16, 0.0, 65535.0) / quantStep);\n"
        "  return saturate(code / max(maxCode, 1.0));\n"
        "}\n"
        "float3 dovi_compose_p7_fel(float y, float2 cbcr, float2 uv) {\n"
        "  float3 blPixel = saturate(float3(y, cbcr.x, cbcr.y) * doviSampleScale);\n"
        "  float3 blChroma = float3(dovi_chroma_site_luma(uv), blPixel.y, blPixel.z);\n"
        "  float3 mapped = dovi_apply_mapping_p7(blPixel, blChroma);\n"
        "  float felY = dovi_sample_fel_y(uv);\n"
        "  float2 felCbcr = dovi_sample_fel_uv(uv);\n"
        "  float3 mappedCode = float3(dovi_norm16_to_code(mapped.x),\n"
        "                             dovi_norm16_to_code(mapped.y),\n"
        "                             dovi_norm16_to_code(mapped.z));\n"
        "  float3 residualCode = float3(dovi_inverse_nlq_code(felY, 0),\n"
        "                              dovi_inverse_nlq_code(felCbcr.x, 1),\n"
        "                              dovi_inverse_nlq_code(felCbcr.y, 2));\n"
        "  float3 mergedCode = clamp(mappedCode + residualCode, 0.0, 65535.0);\n"
        "  return float3(dovi_vdr_output_code(mergedCode.x),\n"
        "                dovi_vdr_output_code(mergedCode.y),\n"
        "                dovi_vdr_output_code(mergedCode.z));\n"
        "}\n"
        "float3 dovi_to_bt2020_nits(float3 ipt) {\n"
        "  float3 pqRgb = dovi_reshape(ipt);\n"
        "  float3 linearRgb = pq_to_nits(pqRgb) / 10000.0;\n"
        "  float3 lms = dovi_rgb_to_lms(linearRgb);\n"
        "  return dovi_lms_to_bt2020(lms) * 10000.0;\n"
        "}\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float2 displayUv = saturate(uv);\n"
        "  if (outside_dovi_active_area(displayUv)) return float4(0.0, 0.0, 0.0, 1.0);\n"
        "  float2 sampleUv = lerp(sourceUvRect.xy, sourceUvRect.zw, displayUv);\n"
        "  float y = texY.Sample(samp, sampleUv);\n"
        "  float2 cbcr = texUV.Sample(samp, sampleUv);\n"
        "  if (doviEnabled == 2) {\n"
        "    float3 hdrYcc = dovi_compose_p7_fel(y, cbcr, displayUv);\n"
        "    float3 rgb = ycbcr_to_rgb(hdrYcc.x, hdrYcc.yz);\n"
        "    if (outputMode == 1) {\n"
        "      if (transferType == 2 && primariesType == 2) return float4(apply_hdr_tone_curve_pq(rgb), 1.0);\n"
        "      return float4(apply_hdr_tone_curve_nits(encoded_to_nits(rgb)), 1.0);\n"
        "    }\n"
        "    float3 nits = encoded_to_nits(rgb);\n"
        "    if (primariesType == 2) return float4(bt2020_nits_to_sdr(nits), 1.0);\n"
        "    return float4(bt709_nits_to_sdr(nits), 1.0);\n"
        "  }\n"
        "  // Dolby Vision path: the input YUV is IPT-PQ encoded (not standard\n"
        "  // YCbCr). Apply per-frame RPU reshaping to recover BT.2020 PQ RGB,\n"
        "  // then output as HDR10 PQ or tone-map to SDR.\n"
        "  if (doviEnabled == 1) {\n"
        "    float3 ipt = saturate(float3(y, cbcr.x, cbcr.y) * doviSampleScale);\n"
        "    float3 bt2020Nits = dovi_to_bt2020_nits(ipt);\n"
        "    if (outputMode == 1) {\n"
        "      return float4(apply_hdr_tone_curve_nits(bt2020Nits), 1.0);\n"
        "    }\n"
        "    return float4(bt2020_nits_to_sdr(bt2020Nits), 1.0);\n"
        "  }\n"
        "  apply_fel_overlay(y, cbcr, displayUv);\n"
        "  float3 rgb = ycbcr_to_rgb(y, cbcr);\n"
        "  if (outputMode == 1) {\n"
        "    if (transferType == 2 && primariesType == 2) return float4(apply_hdr_tone_curve_pq(rgb), 1.0);\n"
        "    float3 nits = encoded_to_nits(rgb);\n"
        "    if (primariesType == 1) nits = bt709_to_bt2020(nits);\n"
        "    return float4(apply_hdr_tone_curve_nits(nits), 1.0);\n"
        "  }\n"
        "  if (transferType == 2 || transferType == 3) {\n"
        "    float3 nits = encoded_to_nits(rgb);\n"
        "    if (primariesType == 2) return float4(bt2020_nits_to_sdr(nits), 1.0);\n"
        "    return float4(bt709_nits_to_sdr(nits), 1.0);\n"
        "  }\n"
        "  if (primariesType == 2) {\n"
        "    float3 linear709 = bt2020_to_bt709(pow(saturate(rgb), 2.2));\n"
        "    return float4(encode_sdr_g22(compress_gamut_preserve_luma(linear709)), 1.0);\n"
        "  }\n"
        "  return float4(saturate(rgb), 1.0);\n"
        "}\n";
    const char* psSubtitleSrc =
        "Texture2D<float4> tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer SubtitleConstants : register(b0) { float4 uvRect; };\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float2 sourceUv = lerp(uvRect.xy, uvRect.zw, saturate(uv));\n"
        "  return tex.Sample(samp, sourceUv);\n"
        "}\n";

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psNv12Blob;
    Microsoft::WRL::ComPtr<ID3DBlob> psSubtitleBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(vsSrc, static_cast<SIZE_T>(std::strlen(vsSrc)), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errors))) {
        LogHr(L"D3DCompile vs", E_FAIL);
        return false;
    }
    if (IsStopRequested()) return false;
    if (FAILED(D3DCompile(psSrc, static_cast<SIZE_T>(std::strlen(psSrc)), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errors))) {
        LogHr(L"D3DCompile ps", E_FAIL);
        return false;
    }
    if (IsStopRequested()) return false;
    if (FAILED(D3DCompile(psNv12Src, static_cast<SIZE_T>(std::strlen(psNv12Src)), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psNv12Blob, &errors))) {
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0) {
            const std::string errText(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            LogInfo(L"D3DCompile ps nv12 errors: " + Utf8ToWide(errText.c_str()));
        }
        LogHr(L"D3DCompile ps nv12", E_FAIL);
        return false;
    }
    if (IsStopRequested()) return false;
    if (FAILED(D3DCompile(psSubtitleSrc, static_cast<SIZE_T>(std::strlen(psSubtitleSrc)), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psSubtitleBlob, &errors))) {
        LogHr(L"D3DCompile ps subtitle", E_FAIL);
        return false;
    }
    if (IsStopRequested()) return false;
    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_))) return false;
    if (IsStopRequested()) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_))) return false;
    if (IsStopRequested()) return false;
    if (FAILED(device_->CreatePixelShader(psNv12Blob->GetBufferPointer(), psNv12Blob->GetBufferSize(), nullptr, &psNv12_))) return false;
    if (IsStopRequested()) return false;
    if (FAILED(device_->CreatePixelShader(psSubtitleBlob->GetBufferPointer(), psSubtitleBlob->GetBufferSize(), nullptr, &psSubtitle_))) return false;
    if (IsStopRequested()) return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_))) return false;
    if (IsStopRequested()) return false;

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blendDesc, &subtitleBlend_))) return false;
    if (IsStopRequested()) return false;

    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(VideoColorConstants);
    constantsDesc.Usage = D3D11_USAGE_DEFAULT;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&constantsDesc, nullptr, &colorConstants_))) return false;
    if (IsStopRequested()) return false;

    D3D11_BUFFER_DESC doviDesc{};
    doviDesc.ByteWidth = sizeof(DoviShaderConstants);
    doviDesc.Usage = D3D11_USAGE_DEFAULT;
    doviDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const HRESULT doviHr = device_->CreateBuffer(&doviDesc, nullptr, &doviConstants_);
    if (FAILED(doviHr)) {
        LogHr(L"CreateBuffer dovi", doviHr);
        return false;
    }
    if (IsStopRequested()) return false;

    D3D11_BUFFER_DESC subtitleConstantsDesc{};
    subtitleConstantsDesc.ByteWidth = sizeof(SubtitleShaderConstants);
    subtitleConstantsDesc.Usage = D3D11_USAGE_DEFAULT;
    subtitleConstantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&subtitleConstantsDesc, nullptr, &subtitleConstants_))) return false;
    if (IsStopRequested()) return false;
    return true;
}

bool D3D11VideoRenderer::UpdateColorPipeline(const NativeVideoFrame& frame) {
    if (!context_ || !colorConstants_) {
        return false;
    }

    const bool cmv4ApproxEnabled = videoSettings_.dolbyVisionCmv4Approx || ExperimentalDoviTrimEnabled();
    const bool enhancementYuvEnabled = cmv4ApproxEnabled && frame.HasEnhancementYuv();
    const bool hasDolbyVisionMetadata = frame.dovi && frame.dovi->valid;
    const bool hasEnhancementDolbyVisionMetadata = frame.enhancementDovi && frame.enhancementDovi->valid;
    const bool libplaceboProcessedDolbyVision =
        frame.dynamicMetadataPath.find(L"dolby_vision_libplacebo") != std::wstring::npos;
    const bool rawDolbyVisionInput = hasDolbyVisionMetadata && !libplaceboProcessedDolbyVision;
    const bool felComposerInput = enhancementYuvEnabled && hasEnhancementDolbyVisionMetadata;
    const bool felOverlayInput = enhancementYuvEnabled && !felComposerInput;
    const bool libplaceboCmv4TrimInput =
        libplaceboProcessedDolbyVision && cmv4ApproxEnabled && hasDolbyVisionMetadata;
    const auto* doviForDisplay = rawDolbyVisionInput
                                     ? frame.dovi.get()
                                     : (felComposerInput ? frame.enhancementDovi.get()
                                                         : (libplaceboCmv4TrimInput ? frame.dovi.get() : nullptr));
    const auto* doviForActiveArea = hasDolbyVisionMetadata
                                        ? frame.dovi.get()
                                        : (felComposerInput ? frame.enhancementDovi.get() : nullptr);
    const bool felSinglePartition = DoviSingleNlqPartition(felComposerInput ? frame.enhancementDovi.get() : nullptr);
    const bool activeAreaMask = HasDoviActiveAreaMask(doviForActiveArea, frame.width, frame.height);
    const VideoColorMetadata color = NormalizeDolbyVisionOutput(
        MergeColorMetadata(frame.color, mediaColor_),
        rawDolbyVisionInput);
    const bool cmv4LibplaceboIntermediate =
        cmv4ApproxEnabled &&
        libplaceboProcessedDolbyVision &&
        color.primaries == VideoColorPrimaries::Bt2020 &&
        color.transfer == VideoTransferCharacteristic::Pq;
    bool hdrOutput = WantsHdrOutput(color, videoSettings_, displayCapabilities_);
    DXGI_COLOR_SPACE_TYPE colorSpace = hdrOutput
                                           ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                           : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (!ApplySwapChainColorSpace(colorSpace, color) && hdrOutput) {
        hdrOutput = false;
        colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        ApplySwapChainColorSpace(colorSpace, color);
    }

    VideoColorConstants constants;
    constants.sourceUvRect[0] = frame.sourceUvRect.left;
    constants.sourceUvRect[1] = frame.sourceUvRect.top;
    constants.sourceUvRect[2] = frame.sourceUvRect.right;
    constants.sourceUvRect[3] = frame.sourceUvRect.bottom;
    constants.matrixType = MatrixType(color);
    constants.rangeType = color.range == VideoColorRange::Full ? 1 : 0;
    constants.transferType = TransferType(color.transfer);
    constants.outputMode = hdrOutput ? 1 : 0;
    constants.primariesType = PrimariesType(color.primaries);
    constants.toneMapMode = ToneMapType(videoSettings_.toneMapping);
    constants.sourcePeakNits = SourcePeakNits(color, videoSettings_, doviForDisplay);
    const bool hdrToneCurveEnabled = hdrOutput && videoSettings_.dolbyVisionHdrOutput && !cmv4LibplaceboIntermediate;
    constants.targetPeakNits = hdrOutput
                                    ? (hdrToneCurveEnabled
                                           ? HdrToneCurveOutputPeakNits(videoSettings_)
                                           : std::max(100.0f, static_cast<float>(displayCapabilities_.reportedPeakBrightnessNits)))
                                    : 100.0f;
    constants.displayPeakNits = static_cast<float>(std::max(0, displayCapabilities_.reportedPeakBrightnessNits));
    constants.hdrCurveEnabled = hdrToneCurveEnabled ? 1.0f : 0.0f;
    constants.hdrCurvePointCount = static_cast<float>(videoSettings_.hdrToneCurve.size());
    const bool doviTrimMetadataDetected = HasNonNeutralDoviTrim(doviForDisplay);
    const DoviTrimSelection doviTrim = SelectDoviTrim(doviForDisplay,
                                                      constants.targetPeakNits,
                                                      cmv4ApproxEnabled);
    constants.doviTrimEnabled = doviTrim.enabled ? 1 : 0;
    constants.doviTrimSlope = doviTrim.slope;
    constants.doviTrimOffset = doviTrim.offset;
    constants.doviTrimPower = doviTrim.power;
    constants.doviTrimSaturation = doviTrim.saturation;
    constants.doviTrimChromaWeight = doviTrim.chromaWeight;
    constants.doviTrimMsWeight = doviTrim.msWeight;
    constants.doviTrimStrength = doviTrim.enabled ? 1.0f : 0.0f;
    constants.doviTrimMidOffset = doviTrim.midOffset;
    constants.doviTrimMidContrast = doviTrim.midContrast;
    constants.doviTrimClip = doviTrim.clip;
    constants.doviTrimReserved0 = felOverlayInput ? 1.0f : 0.0f;
    constants.doviTrimReserved1 = 1.0f;
    constants.doviActiveArea[0] = 0.0f;
    constants.doviActiveArea[1] = 0.0f;
    constants.doviActiveArea[2] = 1.0f;
    constants.doviActiveArea[3] = 1.0f;
    if (activeAreaMask) {
        constants.doviActiveArea[0] =
            std::clamp(static_cast<float>(doviForActiveArea->dmLevel5LeftOffset) / static_cast<float>(frame.width), 0.0f, 1.0f);
        constants.doviActiveArea[1] =
            std::clamp(static_cast<float>(doviForActiveArea->dmLevel5TopOffset) / static_cast<float>(frame.height), 0.0f, 1.0f);
        constants.doviActiveArea[2] =
            std::clamp(1.0f - static_cast<float>(doviForActiveArea->dmLevel5RightOffset) / static_cast<float>(frame.width), 0.0f, 1.0f);
        constants.doviActiveArea[3] =
            std::clamp(1.0f - static_cast<float>(doviForActiveArea->dmLevel5BottomOffset) / static_cast<float>(frame.height), 0.0f, 1.0f);
        if (constants.doviActiveArea[2] <= constants.doviActiveArea[0] ||
            constants.doviActiveArea[3] <= constants.doviActiveArea[1]) {
            constants.doviActiveArea[0] = 0.0f;
            constants.doviActiveArea[1] = 0.0f;
            constants.doviActiveArea[2] = 1.0f;
            constants.doviActiveArea[3] = 1.0f;
        }
    }
    float previousInput = 0.0f;
    float previousOutput = 0.0f;
    for (std::size_t index = 0; index < videoSettings_.hdrToneCurve.size(); ++index) {
        float input = ClampHdrToneCurveNits(index < anvil::playback::kDefaultHdrToneCurve.size()
                                                ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                                : videoSettings_.hdrToneCurve[index].inputNits);
        float output = ClampHdrToneCurveNits(videoSettings_.hdrToneCurve[index].outputNits);
        if (index == 0) {
            input = 0.0f;
            output = 0.0f;
        } else {
            input = std::max(input, previousInput + 0.001f);
            output = std::max(output, previousOutput);
        }
        constants.hdrToneCurve[index][0] = input;
        constants.hdrToneCurve[index][1] = output;
        previousInput = input;
        previousOutput = output;
    }
    context_->UpdateSubresource(colorConstants_.Get(), 0, nullptr, &constants, 0, 0);
    // Only update the (large) DV constants buffer when DV metadata is actually
    // present; non-DV frames keep the previous disabled state.
    if (rawDolbyVisionInput || felComposerInput) {
        UpdateDoviConstants(frame);
    } else if (doviEnabledLastFrame_) {
        // One-shot transition: disable on the frame after DV ends.
        UpdateDoviConstants(frame);
        doviEnabledLastFrame_ = false;
    }

    const int inputKind = frame.HasD3DTexture() ? 1 : (frame.HasYuv() ? 2 : 3);
    const int dynamicMetadataKind = libplaceboProcessedDolbyVision
                                        ? 1
                                        : (rawDolbyVisionInput ? 2 : (felComposerInput ? 4 : (hasDolbyVisionMetadata ? 3 : 0)));
    uint64_t pipelineSignature = 0xcbf29ce484222325ull;
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(inputKind));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(hdrOutput ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(colorSpace));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(color.primaries));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(color.transfer));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(color.matrix));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(color.range));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(videoSettings_.toneMapping));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(videoSettings_.dolbyVisionHdrOutput ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(cmv4ApproxEnabled ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(cmv4LibplaceboIntermediate ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(dynamicMetadataKind));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(activeAreaMask ? 1 : 0));
    if (activeAreaMask && doviForActiveArea) {
        pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviForActiveArea->dmLevel5LeftOffset));
        pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviForActiveArea->dmLevel5RightOffset));
        pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviForActiveArea->dmLevel5TopOffset));
        pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviForActiveArea->dmLevel5BottomOffset));
    }
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(felOverlayInput ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(felComposerInput ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(felSinglePartition ? 1 : 0));
    if (felComposerInput && frame.enhancementDovi) {
        pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(frame.enhancementDovi->nlqNumXPartitions));
        pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(frame.enhancementDovi->nlqNumYPartitions));
    }
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviTrim.enabled ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviTrim.includesLevel3 ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, static_cast<uint64_t>(doviTrimMetadataDetected ? 1 : 0));
    pipelineSignature = HashCombine(pipelineSignature, HashDoubleBucket(constants.targetPeakNits, 1.0));
    pipelineSignature = HashCombine(pipelineSignature, HashDoubleBucket(constants.sourcePeakNits, 1.0));

    if (pipelineSignature != activePipelineSignature_) {
        activePipelineSignature_ = pipelineSignature;
        const std::wstring dynamicMetadata =
            !frame.dynamicMetadataPath.empty()
                ? frame.dynamicMetadataPath
                : (hasDolbyVisionMetadata ? L"dolby_vision_shader" : L"none");
        const std::wstring felDetail =
            felComposerInput
                ? (felSinglePartition
                       ? L" fel_merge=nlq composer=code_merge+fixed16_poly+vdr_quant+chroma_site_luma+el_bicubic"
                       : L" fel_merge=mapping_only multi_partition=unsupported composer=code_mapping+vdr_quant")
                : (felOverlayInput ? L" fel_overlay=experimental" : L"");
        const std::wstring activeAreaDetail =
            activeAreaMask && doviForActiveArea
                ? L" dv_active_area_mask=on l5_offsets=" +
                      std::to_wstring(doviForActiveArea->dmLevel5LeftOffset) + L"," +
                      std::to_wstring(doviForActiveArea->dmLevel5RightOffset) + L"," +
                      std::to_wstring(doviForActiveArea->dmLevel5TopOffset) + L"," +
                      std::to_wstring(doviForActiveArea->dmLevel5BottomOffset)
                : L"";
        const std::wstring label =
            std::wstring(L"input=") + (frame.HasD3DTexture() ? L"d3d11_texture" : (frame.HasYuv() ? L"p010_yuv" : L"bgra")) +
            L" output=" + OutputModeName(hdrOutput) +
            L" color_space=" + ColorSpaceName(colorSpace) +
            L" primaries=" + anvil::playback::ToDisplayString(color.primaries) +
            L" transfer=" + anvil::playback::ToDisplayString(color.transfer) +
            L" matrix=" + anvil::playback::ToDisplayString(color.matrix) +
            L" range=" + anvil::playback::ToDisplayString(color.range) +
            L" tone_mapping=" + anvil::playback::ToDisplayString(videoSettings_.toneMapping) +
            (cmv4LibplaceboIntermediate ? (hdrOutput ? L" cmv4_final=hdr_pq" : L" cmv4_final=sdr") : L"") +
            (doviTrim.enabled ? L" trim_domain=pq dv_trim=" + std::wstring(doviTrim.source) : (doviTrimMetadataDetected ? L" dv_trim=detected_unapplied" : L"")) +
            activeAreaDetail +
            (!hdrOutput && doviTrim.enabled ? L" cmv4_sdr=vivid_reinhard_200nits" : L"") +
            (doviTrim.includesLevel3 ? L" l3=applied" : L"") +
            (hdrOutput ? L" hdr_curve_peak=" + std::to_wstring(static_cast<int>(std::round(constants.targetPeakNits))) : L"") +
            (hdrOutput && videoSettings_.dolbyVisionHdrOutput && cmv4LibplaceboIntermediate ? L" hdr_curve=bypassed_for_cmv4" : L"") +
            L" dolby_vision=" + (libplaceboProcessedDolbyVision
                                      ? L"processed"
                                      : (rawDolbyVisionInput ? L"raw_rpu" : (felComposerInput ? L"profile7_fel" : L"none"))) +
            L" dynamic_metadata=" + dynamicMetadata +
            felDetail +
            (rawDolbyVisionInput
                 ? L" dv_reshape=true st2084_correction=true tone_map=luma_log_bt2446_fit gamut=luma_preserving"
                 : L"");
        activePipelineLabel_ = label;
        LogInfo(label);
    }
    // Log DV reshaping metadata once per stream (first frame carrying it).
    if (rawDolbyVisionInput && !dolbyVisionMetadataLogged_) {
        dolbyVisionMetadataLogged_ = true;
        std::wostringstream ss;
        ss << L"dolby_vision_metadata profile=" << frame.dovi->profile
           << L" bl_bit_depth=" << frame.dovi->blBitDepth
           << L" vdr_bit_depth=" << frame.dovi->vdrBitDepth
           << L" full_range=" << (frame.dovi->blVideoFullRange ? 1 : 0)
           << L" coef_log2_denom=" << frame.dovi->coefLog2Denom
           << L" source_max_pq=" << frame.dovi->sourceMaxPq
           << L" source_max_nits=" << static_cast<int>(std::round(frame.dovi->sourceMaxNits))
           << L" pivots=";
        for (int c = 0; c < anvil::playback::kDoviNumComponents; ++c) {
            ss << L"[" << frame.dovi->curves[c].numPivots << L"]";
        }
        ss << L" methods=";
        for (int c = 0; c < anvil::playback::kDoviNumComponents; ++c) {
            int poly = 0, mmr = 0;
            const int pieces = std::max(0, frame.dovi->curves[c].numPivots - 1);
            for (int s = 0; s < pieces; ++s) {
                if (frame.dovi->curves[c].pieces[s].method == anvil::playback::DoviMappingMethod::Polynomial) ++poly;
                else if (frame.dovi->curves[c].pieces[s].method == anvil::playback::DoviMappingMethod::Mmr) ++mmr;
            }
            ss << L"[" << poly << L"p/" << mmr << L"m]";
        }
        LogInfo(ss.str());
    }
    if (felComposerInput && !dolbyVisionMetadataLogged_) {
        dolbyVisionMetadataLogged_ = true;
        const auto& dovi = *frame.enhancementDovi;
        const bool singlePartition = DoviSingleNlqPartition(&dovi);
        std::wostringstream ss;
        ss << L"dolby_vision_metadata profile=" << dovi.profile
           << L" mode=" << (singlePartition ? L"profile7_fel_nlq_merge" : L"profile7_fel_mapping_only_multi_partition")
           << L" bl_bit_depth=" << dovi.blBitDepth
           << L" el_bit_depth=" << dovi.elBitDepth
           << L" vdr_bit_depth=" << dovi.vdrBitDepth
           << L" coef_log2_denom=" << dovi.coefLog2Denom
           << L" residual=" << (dovi.residualDisabled ? L"disabled" : L"enabled")
           << L" el_spatial=" << (dovi.elSpatialResampling ? 1 : 0)
           << L" partitions=" << dovi.nlqNumXPartitions << L"x" << dovi.nlqNumYPartitions
           << L" composer=" << (singlePartition
                                  ? L"code_merge+fixed16_poly+vdr_quant+chroma_site_luma+el_bicubic"
                                  : L"mapping_only_multi_partition_unsupported")
           << L" nlq_method=" << static_cast<int>(dovi.nlqMethod)
           << L" nlq0(offset=" << dovi.nlqOffset[0]
           << L" vdr_max=" << dovi.nlqVdrInMax[0]
           << L" slope=" << dovi.nlqLinearDeadzoneSlope[0]
           << L" threshold=" << dovi.nlqLinearDeadzoneThreshold[0] << L")";
        LogInfo(ss.str());
    }
    return true;
}

void D3D11VideoRenderer::UpdateDoviConstants(const NativeVideoFrame& frame) {
    DoviShaderConstants dc{};

    const bool cmv4ApproxEnabled = videoSettings_.dolbyVisionCmv4Approx || ExperimentalDoviTrimEnabled();
    const bool p7Composer = cmv4ApproxEnabled &&
                            frame.HasEnhancementYuv() &&
                            frame.enhancementDovi &&
                            frame.enhancementDovi->valid;
    const auto* doviSource = p7Composer
                                 ? frame.enhancementDovi.get()
                                 : (frame.dovi && frame.dovi->valid ? frame.dovi.get() : nullptr);
    if (!doviConstants_) {
        return;
    }
    if (!doviSource) {
        // No DV metadata: disable reshaping in the shader.
        context_->UpdateSubresource(doviConstants_.Get(), 0, nullptr, &dc, 0, 0);
        return;
    }

    const auto& dovi = *doviSource;

    for (int c = 0; c < 3; ++c) {
        const auto& curve = dovi.curves[c];
        const int numPivots = std::min(curve.numPivots, static_cast<int>(kDoviMaxPivots));
        dc.curveMeta[c] = static_cast<float>(std::max(2, numPivots));
        // Pivots: pack 9 values into 3 float4s (3 per float4, padding the 4th).
        for (int p = 0; p < 9; ++p) {
            const int slot = p / 3;
            const int comp = p % 3;
            dc.pivots[c][slot][comp] = (p < numPivots) ? curve.pivots[p] : 1.0f;
        }
        const int numPieces = std::max(0, numPivots - 1);
        // pieceMeta: per-piece mapping_idc / poly_order / mmr_order, plus num_pivots.
        // Packed 4-per-float4: [piece0_idc, piece1_idc, piece2_idc, piece3_idc] etc.
        for (int slot = 0; slot < 2; ++slot) {
            for (int comp = 0; comp < 4; ++comp) {
                const int pieceIdx = slot * 4 + comp;
                if (pieceIdx >= numPieces) {
                    dc.pieceMeta[c][slot][comp] = -1.0f;
                } else {
                    const auto& piece = curve.pieces[pieceIdx];
                    dc.pieceMeta[c][slot][comp] = static_cast<float>(static_cast<int>(piece.method));
                }
            }
        }
        // Polynomial coefficients per piece (xyz = c0,c1,c2; w = poly_order).
        for (int s = 0; s < kDoviMaxPieces; ++s) {
            if (s < numPieces && curve.pieces[s].method == DoviMappingMethod::Polynomial) {
                dc.polyCoef[c][s][0] = curve.pieces[s].polyCoef[0];
                dc.polyCoef[c][s][1] = curve.pieces[s].polyCoef[1];
                dc.polyCoef[c][s][2] = curve.pieces[s].polyCoef[2];
                dc.polyCoef[c][s][3] = static_cast<float>(curve.pieces[s].polyOrder);
            } else {
                dc.polyCoef[c][s][3] = 0.0f;
            }
        }
        // MMR coefficients per piece: pack constant + 3 terms × 7 coeffs into 6 float4s (24 floats).
        // [t0_c0..t0_c6] [t1_c0..t1_c6] [t2_c0..t2_c6] conceptually; here laid out linearly.
        for (int s = 0; s < kDoviMaxPieces; ++s) {
            if (s < numPieces && curve.pieces[s].method == DoviMappingMethod::Mmr) {
                const auto& piece = curve.pieces[s];
                dc.polyCoef[c][s][3] = static_cast<float>(std::clamp(piece.mmrOrder, 1, kDoviMmrMaxTerms));
                float flat[24] = {};
                flat[0] = piece.mmrConstant;
                int idx = 1;
                for (int t = 0; t < piece.mmrOrder && t < kDoviMmrMaxTerms; ++t) {
                    for (int k = 0; k < kDoviMmrCoeffsPerOrder; ++k) {
                        if (idx < 24) flat[idx] = piece.mmrCoef[t][k];
                        ++idx;
                    }
                }
                for (int f = 0; f < 6; ++f) {
                    dc.mmrCoef[c][s][f][0] = flat[f * 4 + 0];
                    dc.mmrCoef[c][s][f][1] = flat[f * 4 + 1];
                    dc.mmrCoef[c][s][f][2] = flat[f * 4 + 2];
                    dc.mmrCoef[c][s][f][3] = flat[f * 4 + 3];
                }
            }
        }
    }

    // Color matrices (row-major 3x3 packed into float4[3]).
    for (int r = 0; r < 3; ++r) {
        dc.yccToRgb[r][0] = dovi.yccToRgb[r * 3 + 0];
        dc.yccToRgb[r][1] = dovi.yccToRgb[r * 3 + 1];
        dc.yccToRgb[r][2] = dovi.yccToRgb[r * 3 + 2];
        dc.rgbToLms[r][0] = dovi.rgbToLms[r * 3 + 0];
        dc.rgbToLms[r][1] = dovi.rgbToLms[r * 3 + 1];
        dc.rgbToLms[r][2] = dovi.rgbToLms[r * 3 + 2];
    }
    dc.yccOffset[0] = dovi.yccOffset[0];
    dc.yccOffset[1] = dovi.yccOffset[1];
    dc.yccOffset[2] = dovi.yccOffset[2];
    for (int c = 0; c < 3; ++c) {
        dc.nlqParams[c][0] = static_cast<float>(dovi.nlqOffset[c]);
        dc.nlqParams[c][1] = static_cast<float>(dovi.nlqVdrInMax[c]);
        dc.nlqParams[c][2] = static_cast<float>(dovi.nlqLinearDeadzoneSlope[c]);
        dc.nlqParams[c][3] = static_cast<float>(dovi.nlqLinearDeadzoneThreshold[c]);
    }
    const bool singlePartition = DoviSingleNlqPartition(&dovi);
    dc.composerMeta[0] = (p7Composer &&
                          singlePartition &&
                          !dovi.residualDisabled &&
                          dovi.nlqMethod == DoviNlqMethod::LinearDeadzone)
                             ? 1.0f
                             : 0.0f;
    dc.composerMeta[1] = static_cast<float>(static_cast<int>(dovi.nlqMethod));
    dc.composerMeta[2] = static_cast<float>(std::clamp(dovi.elBitDepth, 8, 16));
    dc.composerMeta[3] = static_cast<float>(std::clamp(dovi.vdrBitDepth, 8, 16));
    dc.composerScale[0] = static_cast<float>(std::max(0, dovi.coefLog2Denom));
    dc.composerScale[1] = DoviTextureSampleScale(dovi.elBitDepth);
    dc.composerScale[2] = static_cast<float>(std::clamp(dovi.blBitDepth, 8, 16));
    dc.composerScale[3] = dovi.elSpatialResampling ? 1.0f : 0.0f;

    dc.profile = dovi.profile;
    dc.compatibilityId = dovi.compatibilityId;
    dc.enabled = p7Composer ? 2 : 1;
    dc.sampleScale = DoviTextureSampleScale(dovi.blBitDepth);
    dc.curveMeta[3] = DoviOffsetScale();

    context_->UpdateSubresource(doviConstants_.Get(), 0, nullptr, &dc, 0, 0);
    doviEnabledLastFrame_ = true;
}

bool D3D11VideoRenderer::ApplySwapChainColorSpace(const DXGI_COLOR_SPACE_TYPE colorSpace, const VideoColorMetadata& color) {
    if (!swapChain_) {
        return false;
    }
    if (activeColorSpace_ == colorSpace) {
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && !hdrMetadataApplied_) {
            ApplyHdrMetadata(color);
        }
        return true;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain_.As(&swapChain3)) || !swapChain3) {
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || !hdrColorSpaceFailureLogged_) {
            LogInfo(L"color_space unsupported reason=swapchain3_unavailable");
        }
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdrColorSpaceFailureLogged_ = true;
        }
        return false;
    }

    UINT support = 0;
    HRESULT hr = swapChain3->CheckColorSpaceSupport(colorSpace, &support);
    if (FAILED(hr) || (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || !hdrColorSpaceFailureLogged_) {
            LogInfo(L"color_space unsupported target=" + ColorSpaceName(colorSpace));
        }
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdrColorSpaceFailureLogged_ = true;
        }
        return false;
    }

    hr = swapChain3->SetColorSpace1(colorSpace);
    if (FAILED(hr)) {
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || !hdrColorSpaceFailureLogged_) {
            LogHr(L"SetColorSpace1 " + ColorSpaceName(colorSpace), hr);
        }
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdrColorSpaceFailureLogged_ = true;
        }
        return false;
    }

    activeColorSpace_ = colorSpace;
    hdrMetadataApplied_ = false;
    if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        ApplyHdrMetadata(color);
    } else {
        Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain4;
        if (SUCCEEDED(swapChain_.As(&swapChain4)) && swapChain4) {
            swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
        }
    }
    LogInfo(L"color_space active=" + ColorSpaceName(colorSpace));
    return true;
}

void D3D11VideoRenderer::ApplyHdrMetadata(const VideoColorMetadata& color) {
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain4;
    if (!swapChain_ || FAILED(swapChain_.As(&swapChain4)) || !swapChain4) {
        return;
    }

    DXGI_HDR_METADATA_HDR10 metadata{};
    const auto& mastering = color.masteringDisplay;
    const auto setPrimary = [](UINT16 (&target)[2], const anvil::playback::ChromaticityPoint& point) {
        target[0] = ChromaticityToDxgi(point.x);
        target[1] = ChromaticityToDxgi(point.y);
    };

    if (mastering.hasPrimaries) {
        setPrimary(metadata.RedPrimary, mastering.red);
        setPrimary(metadata.GreenPrimary, mastering.green);
        setPrimary(metadata.BluePrimary, mastering.blue);
        setPrimary(metadata.WhitePoint, mastering.whitePoint);
    } else {
        metadata.RedPrimary[0] = ChromaticityToDxgi(0.708);
        metadata.RedPrimary[1] = ChromaticityToDxgi(0.292);
        metadata.GreenPrimary[0] = ChromaticityToDxgi(0.170);
        metadata.GreenPrimary[1] = ChromaticityToDxgi(0.797);
        metadata.BluePrimary[0] = ChromaticityToDxgi(0.131);
        metadata.BluePrimary[1] = ChromaticityToDxgi(0.046);
        metadata.WhitePoint[0] = ChromaticityToDxgi(0.3127);
        metadata.WhitePoint[1] = ChromaticityToDxgi(0.3290);
    }

    const bool cmv4ApproxEnabled = videoSettings_.dolbyVisionCmv4Approx || ExperimentalDoviTrimEnabled();
    const bool curveActive = videoSettings_.dolbyVisionHdrOutput && !cmv4ApproxEnabled;
    const double curvePeak = curveActive
                                 ? static_cast<double>(HdrToneCurveOutputPeakNits(videoSettings_))
                                 : 10000.0;
    const double sourceMaxMastering = mastering.hasLuminance && mastering.maxLuminanceNits > 0.0
                                          ? mastering.maxLuminanceNits
                                          : static_cast<double>(std::max(100, videoSettings_.peakBrightnessNits));
    const double maxMastering = curveActive
                                    ? std::clamp(sourceMaxMastering, 100.0, curvePeak)
                                    : sourceMaxMastering;
    const double minMastering = mastering.hasLuminance && mastering.minLuminanceNits > 0.0
                                    ? mastering.minLuminanceNits
                                    : 0.0001;
    metadata.MaxMasteringLuminance = LuminanceToDxgi(maxMastering);
    metadata.MinMasteringLuminance = LuminanceToDxgi(minMastering);
    metadata.MaxContentLightLevel = LightLevelToDxgi(static_cast<int>(std::round(
        color.contentLight.hasValues
            ? std::min(static_cast<double>(color.contentLight.maxContentLightLevelNits), curvePeak)
            : maxMastering)));
    metadata.MaxFrameAverageLightLevel = LightLevelToDxgi(static_cast<int>(std::round(
        color.contentLight.hasValues
            ? std::min(static_cast<double>(color.contentLight.maxFrameAverageLightLevelNits), curvePeak)
            : maxMastering / 2.0)));

    const HRESULT hr = swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata);
    if (SUCCEEDED(hr)) {
        hdrMetadataApplied_ = true;
    }
}

bool D3D11VideoRenderer::UpdateHardwareTexture(const NativeVideoFrame& frame) {
    constexpr std::size_t kMaxHardwareSrvCacheEntries = 32;

    hwSrvY_.Reset();
    hwSrvUV_.Reset();
    if (!frame.HasD3DTexture()) {
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    frame.d3dTexture->GetDesc(&textureDesc);
    if (frame.d3dArraySlice >= textureDesc.ArraySize) {
        LogHardwareTextureFailureOnce(L"invalid_array_slice");
        return false;
    }

    for (const auto& cached : hardwareSrvCache_) {
        if (cached.texture.Get() == frame.d3dTexture.Get() &&
            cached.arraySlice == frame.d3dArraySlice &&
            cached.sourceFormat == textureDesc.Format &&
            cached.y &&
            cached.uv) {
            hwSrvY_ = cached.y;
            hwSrvUV_ = cached.uv;
            if (diagnosticsEnabled_) {
                ++renderStats_.hardwareSrvCacheHits;
            }
            return true;
        }
    }

    DXGI_FORMAT yFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uvFormat = DXGI_FORMAT_UNKNOWN;
    if (textureDesc.Format == DXGI_FORMAT_NV12) {
        yFormat = DXGI_FORMAT_R8_UNORM;
        uvFormat = DXGI_FORMAT_R8G8_UNORM;
    } else if (textureDesc.Format == DXGI_FORMAT_P010 || textureDesc.Format == DXGI_FORMAT_P016) {
        yFormat = DXGI_FORMAT_R16_UNORM;
        uvFormat = DXGI_FORMAT_R16G16_UNORM;
    } else {
        LogHardwareTextureFailureOnce(L"unsupported_hardware_texture_format");
        return false;
    }
    if (diagnosticsEnabled_) {
        ++renderStats_.hardwareSrvCacheMisses;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC yDesc{};
    yDesc.Format = yFormat;
    D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc{};
    uvDesc.Format = uvFormat;
    if (textureDesc.ArraySize > 1) {
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        yDesc.Texture2DArray.MostDetailedMip = 0;
        yDesc.Texture2DArray.MipLevels = 1;
        yDesc.Texture2DArray.FirstArraySlice = frame.d3dArraySlice;
        yDesc.Texture2DArray.ArraySize = 1;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        uvDesc.Texture2DArray.MostDetailedMip = 0;
        uvDesc.Texture2DArray.MipLevels = 1;
        uvDesc.Texture2DArray.FirstArraySlice = frame.d3dArraySlice;
        uvDesc.Texture2DArray.ArraySize = 1;
    } else {
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yDesc.Texture2D.MostDetailedMip = 0;
        yDesc.Texture2D.MipLevels = 1;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvDesc.Texture2D.MostDetailedMip = 0;
        uvDesc.Texture2D.MipLevels = 1;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvView;
    const HRESULT yHr = device_->CreateShaderResourceView(frame.d3dTexture.Get(), &yDesc, &yView);
    if (FAILED(yHr)) {
        LogHardwareTextureFailureOnce(L"CreateShaderResourceView Y hr=0x" + HexHr(yHr));
        return false;
    }
    const HRESULT uvHr = device_->CreateShaderResourceView(frame.d3dTexture.Get(), &uvDesc, &uvView);
    if (FAILED(uvHr)) {
        LogHardwareTextureFailureOnce(L"CreateShaderResourceView UV hr=0x" + HexHr(uvHr));
        return false;
    }

    hwSrvY_ = yView;
    hwSrvUV_ = uvView;
    if (hardwareSrvCache_.size() >= kMaxHardwareSrvCacheEntries) {
        hardwareSrvCache_.erase(hardwareSrvCache_.begin());
    }
    hardwareSrvCache_.push_back(HardwareSrvCacheEntry{
        frame.d3dTexture,
        frame.d3dArraySlice,
        textureDesc.Format,
        std::move(yView),
        std::move(uvView),
    });
    return true;
}

void D3D11VideoRenderer::UpdateTexture(const NativeVideoFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4) return;
    const DXGI_FORMAT uploadFormat = frame.softwareFormat == AV_PIX_FMT_X2BGR10LE
                                         ? DXGI_FORMAT_R10G10B10A2_UNORM
                                         : DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool needsRecreate = !texture_ ||
                               textureW_ != frame.width ||
                               textureH_ != frame.height ||
                               textureFormat_ != uploadFormat;
    if (needsRecreate) {
        texture_.Reset();
        srv_.Reset();
        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = static_cast<UINT>(frame.width);
        tdesc.Height = static_cast<UINT>(frame.height);
        tdesc.MipLevels = 1;
        tdesc.ArraySize = 1;
        tdesc.Format = uploadFormat;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DYNAMIC;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&tdesc, nullptr, &texture_))) {
            LogHr(L"CreateTexture2D", E_FAIL);
            return;
        }
        textureW_ = frame.width;
        textureH_ = frame.height;
        textureFormat_ = uploadFormat;
        if (FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_))) {
            texture_.Reset();
            textureFormat_ = DXGI_FORMAT_UNKNOWN;
            return;
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    const int dstStride = static_cast<int>(mapped.RowPitch);
    const int srcStride = frame.stride;
    const uint8_t* src = frame.bgra->data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    if (dstStride == srcStride) {
        std::memcpy(dst, src, static_cast<std::size_t>(srcStride) * static_cast<std::size_t>(frame.height));
    } else {
        const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 4;
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(dstStride) * y,
                        src + static_cast<std::size_t>(srcStride) * y,
                        rowBytes);
        }
    }
    context_->Unmap(texture_.Get(), 0);
}

bool D3D11VideoRenderer::UpdateYuvTexture(const NativeVideoFrame& frame) {
    if (!frame.HasYuv() || !device_ || !context_) {
        return false;
    }
    const int w = frame.yuv.width;
    const int h = frame.yuv.height;
    if (w <= 0 || h <= 0) {
        return false;
    }

    // (Re)create the staging P010 texture if dimensions changed.
    // P010 = NV12 layout with 16-bit samples: Y plane (R16) + interleaved UV
    // plane (R16G16). Plane selection is implied by the compatible SRV format
    // (R16_UNORM for Y, R16G16_UNORM for UV), not by array slices.
    const bool needsRecreate = !yuvTexture_ || yuvTextureW_ != w || yuvTextureH_ != h;
    if (needsRecreate) {
        yuvTexture_.Reset();
        yuvSrvY_.Reset();
        yuvSrvUV_.Reset();
        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = static_cast<UINT>(w);
        tdesc.Height = static_cast<UINT>(h);
        tdesc.MipLevels = 1;
        tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_P010;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DYNAMIC;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&tdesc, nullptr, &yuvTexture_))) {
            LogHr(L"CreateTexture2D P010", E_FAIL);
            return false;
        }
        yuvTextureW_ = w;
        yuvTextureH_ = h;

        D3D11_SHADER_RESOURCE_VIEW_DESC yDesc{};
        yDesc.Format = DXGI_FORMAT_R16_UNORM;
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yDesc.Texture2D.MostDetailedMip = 0;
        yDesc.Texture2D.MipLevels = 1;
        HRESULT srvHr = device_->CreateShaderResourceView(yuvTexture_.Get(), &yDesc, &yuvSrvY_);
        if (FAILED(srvHr)) {
            LogHr(L"CreateShaderResourceView P010 Y", srvHr);
            yuvTexture_.Reset();
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc{};
        uvDesc.Format = DXGI_FORMAT_R16G16_UNORM;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvDesc.Texture2D.MostDetailedMip = 0;
        uvDesc.Texture2D.MipLevels = 1;
        srvHr = device_->CreateShaderResourceView(yuvTexture_.Get(), &uvDesc, &yuvSrvUV_);
        if (FAILED(srvHr)) {
            LogHr(L"CreateShaderResourceView P010 UV", srvHr);
            yuvTexture_.Reset();
            yuvSrvY_.Reset();
            return false;
        }
    }

    // Upload packed Y + interleaved UV into the P010 texture.
    // P010 layout: width bytes-per-row = w*2 (Y), with UV plane starting at
    // row h (each UV row also w*2 bytes, half as many rows). The mapped
    // subresource exposes both planes contiguously via RowPitch.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(yuvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const int dstStride = static_cast<int>(mapped.RowPitch);
    const int yStride = frame.yuv.yStride;
    const int uvStride = frame.yuv.uvStride;
    const int uvHeight = h / 2;
    const uint8_t* src = frame.yuv.data->data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

    // Y plane.
    if (dstStride == yStride) {
        std::memcpy(dst, src, static_cast<std::size_t>(yStride) * h);
    } else {
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(dstStride) * y,
                        src + static_cast<std::size_t>(yStride) * y,
                        yStride);
        }
    }
    // UV plane (offset dst by dstStride*h rows, src by yStride*h bytes).
    uint8_t* uvDst = dst + static_cast<std::size_t>(dstStride) * h;
    const uint8_t* uvSrc = src + static_cast<std::size_t>(yStride) * h;
    if (dstStride == uvStride) {
        std::memcpy(uvDst, uvSrc, static_cast<std::size_t>(uvStride) * uvHeight);
    } else {
        for (int y = 0; y < uvHeight; ++y) {
            std::memcpy(uvDst + static_cast<std::size_t>(dstStride) * y,
                        uvSrc + static_cast<std::size_t>(uvStride) * y,
                        uvStride);
        }
    }
    context_->Unmap(yuvTexture_.Get(), 0);
    return true;
}

bool D3D11VideoRenderer::UpdateEnhancementYuvTexture(const NativeVideoFrame& frame) {
    if (!frame.HasEnhancementYuv() || !device_ || !context_) {
        return false;
    }
    const NativeYuvPlanes& yuv = frame.enhancementYuv;
    const int w = yuv.width;
    const int h = yuv.height;
    if (w <= 0 || h <= 0 || !yuv.data) {
        return false;
    }

    const bool needsRecreate =
        !enhancementYuvTexture_ || enhancementYuvTextureW_ != w || enhancementYuvTextureH_ != h;
    if (needsRecreate) {
        enhancementYuvTexture_.Reset();
        enhancementYuvSrvY_.Reset();
        enhancementYuvSrvUV_.Reset();
        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = static_cast<UINT>(w);
        tdesc.Height = static_cast<UINT>(h);
        tdesc.MipLevels = 1;
        tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_P010;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DYNAMIC;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device_->CreateTexture2D(&tdesc, nullptr, &enhancementYuvTexture_);
        if (FAILED(hr)) {
            LogHr(L"CreateTexture2D FEL P010", hr);
            return false;
        }
        enhancementYuvTextureW_ = w;
        enhancementYuvTextureH_ = h;

        D3D11_SHADER_RESOURCE_VIEW_DESC yDesc{};
        yDesc.Format = DXGI_FORMAT_R16_UNORM;
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yDesc.Texture2D.MostDetailedMip = 0;
        yDesc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(enhancementYuvTexture_.Get(), &yDesc, &enhancementYuvSrvY_);
        if (FAILED(hr)) {
            LogHr(L"CreateShaderResourceView FEL P010 Y", hr);
            enhancementYuvTexture_.Reset();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc{};
        uvDesc.Format = DXGI_FORMAT_R16G16_UNORM;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvDesc.Texture2D.MostDetailedMip = 0;
        uvDesc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(enhancementYuvTexture_.Get(), &uvDesc, &enhancementYuvSrvUV_);
        if (FAILED(hr)) {
            LogHr(L"CreateShaderResourceView FEL P010 UV", hr);
            enhancementYuvTexture_.Reset();
            enhancementYuvSrvY_.Reset();
            return false;
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(enhancementYuvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const int dstStride = static_cast<int>(mapped.RowPitch);
    const int yStride = yuv.yStride;
    const int uvStride = yuv.uvStride;
    const int uvHeight = h / 2;
    const uint8_t* src = yuv.data->data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

    if (dstStride == yStride) {
        std::memcpy(dst, src, static_cast<std::size_t>(yStride) * h);
    } else {
        for (int row = 0; row < h; ++row) {
            std::memcpy(dst + static_cast<std::size_t>(dstStride) * row,
                        src + static_cast<std::size_t>(yStride) * row,
                        yStride);
        }
    }

    uint8_t* uvDst = dst + static_cast<std::size_t>(dstStride) * h;
    const uint8_t* uvSrc = src + static_cast<std::size_t>(yStride) * h;
    if (dstStride == uvStride) {
        std::memcpy(uvDst, uvSrc, static_cast<std::size_t>(uvStride) * uvHeight);
    } else {
        for (int row = 0; row < uvHeight; ++row) {
            std::memcpy(uvDst + static_cast<std::size_t>(dstStride) * row,
                        uvSrc + static_cast<std::size_t>(uvStride) * row,
                        uvStride);
        }
    }
    context_->Unmap(enhancementYuvTexture_.Get(), 0);
    return true;
}

D3D11VideoRenderer::SubtitleTextureCacheEntry* D3D11VideoRenderer::EnsureSubtitleBitmapTexture(
    const NativeSubtitleBitmap& bitmap) {
    if (!device_ || !bitmap.HasPixels()) {
        return nullptr;
    }

    for (auto& entry : subtitleTextureCache_) {
        if (entry.serial == bitmap.serial &&
            entry.width == bitmap.width &&
            entry.height == bitmap.height &&
            entry.stride == bitmap.stride &&
            entry.pixels == bitmap.bgra &&
            entry.srv) {
            entry.lastUsedFrame = subtitleDrawFrame_;
            return &entry;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(bitmap.width);
    desc.Height = static_cast<UINT>(bitmap.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = bitmap.bgra->data();
    initialData.SysMemPitch = static_cast<UINT>(bitmap.stride);

    SubtitleTextureCacheEntry entry{};
    entry.serial = bitmap.serial;
    entry.width = bitmap.width;
    entry.height = bitmap.height;
    entry.stride = bitmap.stride;
    entry.pixels = bitmap.bgra;
    entry.lastUsedFrame = subtitleDrawFrame_;
    entry.bytes = bitmap.bgra->size();

    const HRESULT textureHr = device_->CreateTexture2D(&desc, &initialData, &entry.texture);
    if (FAILED(textureHr)) {
        LogHr(L"CreateTexture2D subtitle bitmap", textureHr);
        return nullptr;
    }
    const HRESULT srvHr = device_->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.srv);
    if (FAILED(srvHr)) {
        LogHr(L"CreateShaderResourceView subtitle bitmap", srvHr);
        return nullptr;
    }
    if (diagnosticsEnabled_) {
        ++renderStats_.subtitleSurfaceRebuilds;
    }

    subtitleTextureCache_.push_back(std::move(entry));
    return &subtitleTextureCache_.back();
}

void D3D11VideoRenderer::PruneSubtitleTextureCache() {
    constexpr std::size_t kMaxSubtitleTextureEntries = 512;
    constexpr std::size_t kMaxSubtitleTextureBytes = 96ull * 1024ull * 1024ull;

    auto cacheBytes = [this]() {
        std::size_t total = 0;
        for (const auto& entry : subtitleTextureCache_) {
            total += entry.bytes;
        }
        return total;
    };

    while (subtitleTextureCache_.size() > kMaxSubtitleTextureEntries ||
           cacheBytes() > kMaxSubtitleTextureBytes) {
        auto oldest = std::min_element(subtitleTextureCache_.begin(),
                                       subtitleTextureCache_.end(),
                                       [](const SubtitleTextureCacheEntry& lhs,
                                          const SubtitleTextureCacheEntry& rhs) {
                                           return lhs.lastUsedFrame < rhs.lastUsedFrame;
                                       });
        if (oldest == subtitleTextureCache_.end() || oldest->lastUsedFrame == subtitleDrawFrame_) {
            break;
        }
        subtitleTextureCache_.erase(oldest);
    }
}

bool D3D11VideoRenderer::DrawSubtitleBitmapOverlays(const NativeVideoFrame& frame,
                                                    const D3D11_VIEWPORT& videoViewport) {
    if (!context_ ||
        !subtitleBlend_ ||
        !vs_ ||
        !psSubtitle_ ||
        !sampler_ ||
        !subtitleConstants_ ||
        frame.subtitleBitmaps.empty() ||
        viewport_.Width <= 0.0f ||
        viewport_.Height <= 0.0f ||
        videoViewport.Width <= 0.0f ||
        videoViewport.Height <= 0.0f) {
        return false;
    }

    ++subtitleDrawFrame_;
    if (subtitleDrawFrame_ == 0) {
        subtitleDrawFrame_ = 1;
    }

    bool drew = false;
    float blendFactor[4] = {};
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(psSubtitle_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->OMSetBlendState(subtitleBlend_.Get(), blendFactor, 0xffffffff);

    const double surfaceLeft = viewport_.TopLeftX;
    const double surfaceTop = viewport_.TopLeftY;
    const double surfaceRight = viewport_.TopLeftX + viewport_.Width;
    const double surfaceBottom = viewport_.TopLeftY + viewport_.Height;

    int attemptedRects = 0;
    uint64_t attemptedPixels = 0;
    for (const auto& bitmap : frame.subtitleBitmaps) {
        if (!bitmap.HasPixels()) {
            continue;
        }
        const int canvasWidth = bitmap.canvasWidth > 0 ? bitmap.canvasWidth : frame.width;
        const int canvasHeight = bitmap.canvasHeight > 0 ? bitmap.canvasHeight : frame.height;
        if (canvasWidth <= 0 || canvasHeight <= 0) {
            continue;
        }

        auto* entry = EnsureSubtitleBitmapTexture(bitmap);
        if (!entry || !entry->srv) {
            continue;
        }

        const double scaleX = static_cast<double>(videoViewport.Width) / static_cast<double>(canvasWidth);
        const double scaleY = static_cast<double>(videoViewport.Height) / static_cast<double>(canvasHeight);
        const double destLeft = static_cast<double>(videoViewport.TopLeftX) + static_cast<double>(bitmap.x) * scaleX +
                                static_cast<double>(subtitleSettings_.offsetXPx);
        const double destTop = static_cast<double>(videoViewport.TopLeftY) + static_cast<double>(bitmap.y) * scaleY +
                               static_cast<double>(subtitleSettings_.offsetYPx);
        const double destWidth = std::max(1.0, static_cast<double>(bitmap.width) * scaleX);
        const double destHeight = std::max(1.0, static_cast<double>(bitmap.height) * scaleY);
        const double destRight = destLeft + destWidth;
        const double destBottom = destTop + destHeight;

        const double clippedLeft = std::clamp(destLeft, surfaceLeft, surfaceRight);
        const double clippedTop = std::clamp(destTop, surfaceTop, surfaceBottom);
        const double clippedRight = std::clamp(destRight, surfaceLeft, surfaceRight);
        const double clippedBottom = std::clamp(destBottom, surfaceTop, surfaceBottom);
        if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) {
            continue;
        }

        SubtitleShaderConstants constants{};
        constants.uvRect[0] = static_cast<float>((clippedLeft - destLeft) / destWidth);
        constants.uvRect[1] = static_cast<float>((clippedTop - destTop) / destHeight);
        constants.uvRect[2] = static_cast<float>((clippedRight - destLeft) / destWidth);
        constants.uvRect[3] = static_cast<float>((clippedBottom - destTop) / destHeight);
        context_->UpdateSubresource(subtitleConstants_.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* constantBuffers[1] = {subtitleConstants_.Get()};
        context_->PSSetConstantBuffers(0, 1, constantBuffers);

        D3D11_VIEWPORT bitmapViewport{};
        bitmapViewport.TopLeftX = static_cast<float>(clippedLeft);
        bitmapViewport.TopLeftY = static_cast<float>(clippedTop);
        bitmapViewport.Width = static_cast<float>(clippedRight - clippedLeft);
        bitmapViewport.Height = static_cast<float>(clippedBottom - clippedTop);
        bitmapViewport.MinDepth = 0.0f;
        bitmapViewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &bitmapViewport);

        ID3D11ShaderResourceView* srv = entry->srv.Get();
        context_->PSSetShaderResources(0, 1, &srv);
        context_->Draw(3, 0);
        drew = true;
        ++attemptedRects;
        attemptedPixels += static_cast<uint64_t>(bitmap.width) * static_cast<uint64_t>(bitmap.height);
    }

    ID3D11ShaderResourceView* nullSrv[1] = {};
    context_->PSSetShaderResources(0, 1, nullSrv);
    ID3D11Buffer* nullConstants[1] = {};
    context_->PSSetConstantBuffers(0, 1, nullConstants);
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context_->RSSetViewports(1, &viewport_);
    PruneSubtitleTextureCache();
    if (diagnosticsEnabled_ && drew) {
        renderStats_.subtitleBitmapRects += static_cast<uint64_t>(attemptedRects);
        renderStats_.subtitleBitmapPixels += attemptedPixels;
    }

    if (drew && !subtitleBitmapOverlayLogged_) {
        subtitleBitmapOverlayLogged_ = true;
        LogInfo(L"subtitle_bitmap_overlay gpu=active rects=" + std::to_wstring(attemptedRects));
    }
    return drew;
}

bool D3D11VideoRenderer::UpdateSubtitleOverlay(const NativeVideoFrame& frame, const D3D11_VIEWPORT& videoViewport) {
    const std::wstring& text = frame.subtitleText;
    if (!device_ || !context_ || text.empty() || viewport_.Width <= 0.0f || viewport_.Height <= 0.0f) {
        activeSubtitleText_.clear();
        activeSubtitleBitmapKey_.clear();
        return false;
    }

    const RECT videoRect = ViewportRect(videoViewport);
    const bool textChanged = text != activeSubtitleText_;
    if (subtitleSrv_ &&
        text == activeSubtitleText_ &&
        RectEquals(videoRect, activeSubtitleViewport_) &&
        std::abs(activeSubtitleFontScale_ - subtitleSettings_.fontScale) < 0.001 &&
        activeSubtitleOffsetXPx_ == subtitleSettings_.offsetXPx &&
        activeSubtitleOffsetYPx_ == subtitleSettings_.offsetYPx) {
        return true;
    }
    if (diagnosticsEnabled_) {
        ++renderStats_.subtitleSurfaceRebuilds;
    }

    const int surfaceWidth = std::max(1, static_cast<int>(std::round(viewport_.Width)));
    const int surfaceHeight = std::max(1, static_cast<int>(std::round(viewport_.Height)));
    const int lineCount = CountSubtitleLines(text);
    std::vector<uint8_t> pixels(static_cast<std::size_t>(surfaceWidth) * static_cast<std::size_t>(surfaceHeight) * 4, 0);
    if (!text.empty()) {
        Gdiplus::Bitmap bitmap(surfaceWidth, surfaceHeight, PixelFormat32bppPARGB);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }

        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        const float fontPixels = SubtitleFontPixels(videoViewport, subtitleSettings_.fontScale);
        const float maxTextWidth = std::max(1.0f, videoViewport.Width * 0.84f);
        const float marginBottom = std::clamp(videoViewport.Height * 0.085f, 22.0f, 86.0f);
        const float layoutHeight = std::min(videoViewport.Height * 0.34f,
                                            std::max(fontPixels * 2.1f, fontPixels * (static_cast<float>(lineCount) + 1.2f)));
        const float layoutLeft = videoViewport.TopLeftX +
                                 (videoViewport.Width - maxTextWidth) * 0.5f +
                                 static_cast<float>(subtitleSettings_.offsetXPx);
        const float layoutTop = std::max(videoViewport.TopLeftY,
                                         videoViewport.TopLeftY + videoViewport.Height - marginBottom - layoutHeight) +
                                static_cast<float>(subtitleSettings_.offsetYPx);
        Gdiplus::RectF layout(layoutLeft, layoutTop, maxTextWidth, layoutHeight);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentFar);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);

        Gdiplus::GraphicsPath textPath;
        textPath.AddString(text.c_str(),
                           -1,
                           &family,
                           Gdiplus::FontStyleBold,
                           fontPixels,
                           layout,
                           &format);
        const float outlineWidth = std::clamp(fontPixels * 0.16f, 3.0f, 8.0f);
        Gdiplus::Pen outline(Gdiplus::Color(220, 0, 0, 0), outlineWidth);
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush fill(Gdiplus::Color(245, 255, 255, 255));
        graphics.DrawPath(&outline, &textPath);
        graphics.FillPath(&fill, &textPath);

        Gdiplus::BitmapData bitmapData{};
        Gdiplus::Rect lockRect(0, 0, surfaceWidth, surfaceHeight);
        if (bitmap.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bitmapData) != Gdiplus::Ok) {
            return false;
        }

        const auto* source = static_cast<const uint8_t*>(bitmapData.Scan0);
        const int sourceStride = bitmapData.Stride;
        for (int y = 0; y < surfaceHeight; ++y) {
            const uint8_t* sourceRow = sourceStride >= 0
                                           ? source + static_cast<std::size_t>(sourceStride) * y
                                           : source + static_cast<std::size_t>(-sourceStride) * (surfaceHeight - 1 - y);
            std::memcpy(pixels.data() + static_cast<std::size_t>(surfaceWidth) * 4 * y,
                        sourceRow,
                        static_cast<std::size_t>(surfaceWidth) * 4);
        }
        bitmap.UnlockBits(&bitmapData);
    }

    if (!subtitleTexture_ || subtitleTextureW_ != surfaceWidth || subtitleTextureH_ != surfaceHeight || !subtitleSrv_) {
        subtitleTexture_.Reset();
        subtitleSrv_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(surfaceWidth);
        desc.Height = static_cast<UINT>(surfaceHeight);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &subtitleTexture_))) {
            LogHr(L"CreateTexture2D subtitle", E_FAIL);
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(subtitleTexture_.Get(), nullptr, &subtitleSrv_))) {
            subtitleTexture_.Reset();
            LogHr(L"CreateShaderResourceView subtitle", E_FAIL);
            return false;
        }
        subtitleTextureW_ = surfaceWidth;
        subtitleTextureH_ = surfaceHeight;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(subtitleTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const int destinationStride = static_cast<int>(mapped.RowPitch);
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    const int sourcePitch = surfaceWidth * 4;
    for (int y = 0; y < surfaceHeight; ++y) {
        std::memcpy(destination + static_cast<std::size_t>(destinationStride) * y,
                    pixels.data() + static_cast<std::size_t>(sourcePitch) * y,
                    static_cast<std::size_t>(sourcePitch));
    }
    context_->Unmap(subtitleTexture_.Get(), 0);

    activeSubtitleText_ = text;
    activeSubtitleBitmapKey_.clear();
    activeSubtitleViewport_ = videoRect;
    activeSubtitleFontScale_ = subtitleSettings_.fontScale;
    activeSubtitleOffsetXPx_ = subtitleSettings_.offsetXPx;
    activeSubtitleOffsetYPx_ = subtitleSettings_.offsetYPx;
    if (textChanged) {
        LogInfo(L"subtitle_overlay active=true lines=" + std::to_wstring(lineCount) +
                L" bitmap_rects=0" +
                L" surface=" + std::to_wstring(surfaceWidth) + L"x" + std::to_wstring(surfaceHeight));
    }
    return true;
}

void D3D11VideoRenderer::DrawSubtitleOverlay() {
    if (!context_ || !subtitleSrv_ || !subtitleBlend_ || !vs_ || !psSubtitle_ || !subtitleConstants_) {
        return;
    }

    SubtitleShaderConstants constants{};
    context_->UpdateSubresource(subtitleConstants_.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer* constantBuffers[1] = {subtitleConstants_.Get()};
    context_->PSSetConstantBuffers(0, 1, constantBuffers);

    context_->RSSetViewports(1, &viewport_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(psSubtitle_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, subtitleSrv_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    float blendFactor[4] = {};
    context_->OMSetBlendState(subtitleBlend_.Get(), blendFactor, 0xffffffff);
    context_->Draw(3, 0);
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    ID3D11ShaderResourceView* nullView[1] = {};
    context_->PSSetShaderResources(0, 1, nullView);
    ID3D11Buffer* nullConstants[1] = {};
    context_->PSSetConstantBuffers(0, 1, nullConstants);
}

void D3D11VideoRenderer::ReleaseAll() {
    publishedDevice_.store(nullptr, std::memory_order_release);
    host_.store(nullptr, std::memory_order_release);
    ResetRenderStatsOnRenderThread();
    hardwareSrvCache_.clear();
    subtitleTextureCache_.clear();
    subtitleSrv_.Reset();
    subtitleTexture_.Reset();
    hwSrvUV_.Reset();
    hwSrvY_.Reset();
    yuvSrvUV_.Reset();
    yuvSrvY_.Reset();
    yuvTexture_.Reset();
    yuvTextureW_ = 0;
    yuvTextureH_ = 0;
    enhancementYuvSrvUV_.Reset();
    enhancementYuvSrvY_.Reset();
    enhancementYuvTexture_.Reset();
    enhancementYuvTextureW_ = 0;
    enhancementYuvTextureH_ = 0;
    srv_.Reset();
    texture_.Reset();
    textureW_ = 0;
    textureH_ = 0;
    textureFormat_ = DXGI_FORMAT_UNKNOWN;
    subtitleConstants_.Reset();
    colorConstants_.Reset();
    doviConstants_.Reset();
    subtitleBlend_.Reset();
    sampler_.Reset();
    psSubtitle_.Reset();
    psNv12_.Reset();
    ps_.Reset();
    vs_.Reset();
    rtv_.Reset();
    backBuffer_.Reset();
    if (dcompTarget_) {
        dcompTarget_->SetRoot(nullptr);
    }
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    useComposition_ = false;
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    framePacingLogged_ = false;
    swapChain_.Reset();
    context_.Reset();
    adapter_.Reset();
    device_.Reset();
    factory_.Reset();
    hardwareTextureFailureLogged_ = false;
    subtitleBitmapOverlayLogged_ = false;
    subtitleDrawFrame_ = 0;
    subtitleTextureW_ = 0;
    subtitleTextureH_ = 0;
    activeSubtitleText_.clear();
    activeSubtitleBitmapKey_.clear();
    activeSubtitleViewport_ = {};
    activeSubtitleFontScale_ = 0.0;
    activeSubtitleOffsetXPx_ = 0;
    activeSubtitleOffsetYPx_ = 0;
    activeColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    hdrMetadataApplied_ = false;
    hdrColorSpaceFailureLogged_ = false;
    felOverlayLogged_ = false;
    activePipelineLabel_.clear();
    activePipelineSignature_ = 0;
}

void D3D11VideoRenderer::LogHardwareTextureFailureOnce(const std::wstring& message) {
    if (hardwareTextureFailureLogged_) {
        return;
    }
    hardwareTextureFailureLogged_ = true;
    LogHr(L"hardware texture " + message, E_FAIL);
}

void D3D11VideoRenderer::LogInfo(const std::wstring& message) const {
    if (logSink_) {
        logSink_->Write(LogLevel::Debug, L"d3d11", message);
    }
    OutputDebugStringW((L"[d3d11] " + message + L"\n").c_str());
}

void D3D11VideoRenderer::LogHr(const std::wstring& what, HRESULT hr) const {
    std::wostringstream ss;
    ss << what << L" hr=0x" << std::hex << static_cast<unsigned long>(hr);
    const std::wstring msg = ss.str();
    if (logSink_) {
        logSink_->Write(LogLevel::Error, L"d3d11", msg);
    }
    OutputDebugStringW((L"[d3d11] " + msg + L"\n").c_str());
}

D3D11_VIEWPORT D3D11VideoRenderer::LetterboxedViewport(const int sourceWidth, const int sourceHeight) const {
    if (sourceWidth <= 0 || sourceHeight <= 0 || viewport_.Width <= 0.0f || viewport_.Height <= 0.0f) {
        return viewport_;
    }

    const float scale = std::min(viewport_.Width / static_cast<float>(sourceWidth),
                                 viewport_.Height / static_cast<float>(sourceHeight));
    const float width = std::max(1.0f, std::round(static_cast<float>(sourceWidth) * scale));
    const float height = std::max(1.0f, std::round(static_cast<float>(sourceHeight) * scale));

    D3D11_VIEWPORT result = viewport_;
    result.TopLeftX = std::round((viewport_.Width - width) * 0.5f);
    result.TopLeftY = std::round((viewport_.Height - height) * 0.5f);
    result.Width = width;
    result.Height = height;
    return result;
}

}  // namespace anvil::app
