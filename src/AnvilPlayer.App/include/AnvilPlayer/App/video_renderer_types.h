#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace anvil::app {

struct VideoUiOverlayBitmap {
    int width = 0;
    int height = 0;
    int destinationX = 0;
    int destinationY = 0;
    bool alphaFromRgb = false;
    float opacity = 1.0f;
    std::shared_ptr<const std::vector<uint8_t>> bgraPremultiplied;
};

enum class VideoRendererState : uint32_t {
    Stopped = 0,
    Initializing,
    Ready,
    Failed,
    Stopping,
};

struct VideoRenderStats {
    uint64_t frames = 0;
    uint64_t hardwareFrames = 0;
    uint64_t softwareYuvFrames = 0;
    uint64_t generatedFrames = 0;
    uint64_t generatedSubmitted = 0;
    uint64_t generatedDroppedNotReady = 0;
    uint64_t generatedDroppedSceneCut = 0;
    uint64_t generatedDroppedSceneProbeUnavailable = 0;
    uint64_t inferenceFailures = 0;
    int interpolationMultiplier = 1;
    std::wstring interpolationBackend = L"inactive";
    uint64_t bgraFrames = 0;
    uint64_t subtitleFrames = 0;
    uint64_t slowFrames = 0;
    uint64_t hardwareSrvCacheHits = 0;
    uint64_t hardwareSrvCacheMisses = 0;
    uint64_t subtitleSurfaceRebuilds = 0;
    uint64_t subtitleBitmapRects = 0;
    uint64_t subtitleBitmapPixels = 0;
    uint64_t totalRenderUs = 0;
    uint64_t maxRenderUs = 0;
    uint64_t colorPipelineUs = 0;
    uint64_t hardwarePrepareUs = 0;
    uint64_t bgraUploadUs = 0;
    uint64_t yuvUploadUs = 0;
    uint64_t subtitleUs = 0;
    uint64_t presentUs = 0;
    uint64_t maxPresentUs = 0;
    uint64_t presentSyncFrames = 0;
    uint64_t frameLatencyWaits = 0;
    uint64_t frameLatencyWaitTimeouts = 0;
    uint64_t frameLatencyWaitUs = 0;
    uint64_t maxFrameLatencyWaitUs = 0;
    uint64_t frameStatsSamples = 0;
    uint64_t frameStatsDisjoint = 0;
};

}  // namespace anvil::app
