#pragma once

#include "AnvilPlayer/App/video_texture_sampling_math.h"
#include "AnvilPlayer/Playback/Types.h"

#include <d3d11.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace anvil::app {

enum class GpuFrameInterpolationBackend {
    None,
    DirectMlRife,
};

struct GpuFrameInterpolationConfig {
    UINT width = 0;
    UINT height = 0;
    UINT32 sourceFrameRateNumerator = 0;
    UINT32 sourceFrameRateDenominator = 1;
    UINT32 outputRateMultiplier = 2;
    std::filesystem::path modelPath;
};

struct GpuFrameInterpolationCapabilities {
    bool available = false;
    bool zeroCopy = false;
    bool usesFutureFrames = false;
    UINT32 outputRateMultiplier = 1;
    GpuFrameInterpolationBackend backend = GpuFrameInterpolationBackend::None;
    std::wstring backendName = L"unavailable";
    std::wstring unavailableReason;
    double warmupMilliseconds = 0.0;
    UINT processingWidth = 0;
    UINT processingHeight = 0;
};

struct GpuInterpolationInputSurface {
    ID3D11Texture2D* texture = nullptr;
    UINT arraySlice = 0;
    VideoTextureUvRect sourceUvRect;
    anvil::playback::VideoColorMetadata color;
};

struct GpuInterpolationDiagnostics {
    double inferenceMilliseconds = 0.0;
    double totalMilliseconds = 0.0;
    double sourceDifference = 0.0;
    double midpointToPreviousDifference = 0.0;
    double midpointToNextDifference = 0.0;
    std::uint64_t outputFingerprint = 0;
};

struct GpuInterpolationOutputSurface {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> bgra;
    GpuInterpolationDiagnostics diagnostics;
};

// Vendor-neutral optical-flow interpolation. The RIFE network runs through
// ONNX Runtime's DirectML execution provider. The processing resolution is
// bounded so the optical-flow model can stay inside the source-frame budget
// across vendors; D3D11 scales the generated midpoint at presentation time.
// A driver rate-conversion blit is intentionally not used as a fallback: it
// cannot guarantee that a real midpoint was made.
class GpuFrameInterpolator {
public:
    GpuFrameInterpolator();
    ~GpuFrameInterpolator();
    GpuFrameInterpolator(const GpuFrameInterpolator&) = delete;
    GpuFrameInterpolator& operator=(const GpuFrameInterpolator&) = delete;

    GpuFrameInterpolationCapabilities Initialize(
        ID3D11Device* device,
        const GpuFrameInterpolationConfig& config);
    bool InterpolateMidpoint(
        const GpuInterpolationInputSurface& previous,
        const GpuInterpolationInputSurface& next,
        GpuInterpolationOutputSurface& output);
    void Reset();

    const GpuFrameInterpolationCapabilities& Capabilities() const { return capabilities_; }
    const std::wstring& LastFailureReason() const { return lastFailureReason_; }
    static std::filesystem::path DefaultModelPath();

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
    GpuFrameInterpolationCapabilities capabilities_{};
    std::wstring lastFailureReason_;
};

}  // namespace anvil::app
