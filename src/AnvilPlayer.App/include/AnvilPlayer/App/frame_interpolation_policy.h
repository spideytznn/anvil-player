#pragma once

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/Playback/CapabilityReport.h"
#include "AnvilPlayer/Playback/Settings.h"

#include <algorithm>
#include <cmath>

namespace anvil::app {

// Interpolation endpoints carry video surfaces and color metadata only.
// Subtitles and UI/OSD are presentation-timeline data and are composed after
// the generated video frame has reached the final back buffer.
inline void StripInterpolationPresentationData(NativeVideoFrame& frame) noexcept {
    frame.subtitleText.clear();
    frame.subtitleBitmaps.clear();
    frame.subtitlesPrepared = false;
}

inline bool IsInterpolationDisplayDomainBoundary(
    const bool leftHdrInput,
    const bool rightHdrInput,
    const bool leftHdrOutput,
    const bool rightHdrOutput) noexcept {
    return leftHdrInput != rightHdrInput || leftHdrOutput != rightHdrOutput;
}

struct SceneChangeMetrics {
    double meanAbsoluteLumaDifference = 0.0;
    double changedSampleRatio = 0.0;
    double meanLeftLuma = 0.0;
    double meanRightLuma = 0.0;
};

// This deliberately favors false negatives over false positives. A camera pan
// may change many pixels, while a fade or exposure change can have a large
// absolute delta. Requiring both a broad change and a sizeable residual after
// subtracting the global luminance shift avoids treating either as a hard cut.
inline bool IsHardSceneCut(const SceneChangeMetrics& metrics) noexcept {
    if (!std::isfinite(metrics.meanAbsoluteLumaDifference) ||
        !std::isfinite(metrics.changedSampleRatio) ||
        !std::isfinite(metrics.meanLeftLuma) ||
        !std::isfinite(metrics.meanRightLuma) ||
        metrics.meanAbsoluteLumaDifference < 0.0 ||
        metrics.changedSampleRatio < 0.0 || metrics.changedSampleRatio > 1.0) {
        return false;
    }
    const double globalLumaShift =
        std::abs(metrics.meanLeftLuma - metrics.meanRightLuma);
    const double residualDifference =
        std::max(0.0, metrics.meanAbsoluteLumaDifference - globalLumaShift);
    return metrics.meanAbsoluteLumaDifference >= 0.22 &&
        metrics.changedSampleRatio >= 0.65 && residualDifference >= 0.12;
}

inline bool IsDolbyVisionSceneBoundary(
    const anvil::playback::DolbyVisionFrameMetadata* next) noexcept {
    return next && next->valid && next->sceneRefreshFlag != 0;
}

inline bool SameInterpolationColorMetadata(
    const anvil::playback::VideoColorMetadata& left,
    const anvil::playback::VideoColorMetadata& right) noexcept {
    const auto samePoint = [](const anvil::playback::ChromaticityPoint& a,
                              const anvil::playback::ChromaticityPoint& b) {
        return a.x == b.x && a.y == b.y;
    };
    const auto& leftMastering = left.masteringDisplay;
    const auto& rightMastering = right.masteringDisplay;
    const auto& leftLight = left.contentLight;
    const auto& rightLight = right.contentLight;
    return left.primaries == right.primaries &&
        left.transfer == right.transfer && left.matrix == right.matrix &&
        left.range == right.range &&
        leftMastering.hasPrimaries == rightMastering.hasPrimaries &&
        samePoint(leftMastering.red, rightMastering.red) &&
        samePoint(leftMastering.green, rightMastering.green) &&
        samePoint(leftMastering.blue, rightMastering.blue) &&
        samePoint(leftMastering.whitePoint, rightMastering.whitePoint) &&
        leftMastering.hasLuminance == rightMastering.hasLuminance &&
        leftMastering.minLuminanceNits == rightMastering.minLuminanceNits &&
        leftMastering.maxLuminanceNits == rightMastering.maxLuminanceNits &&
        leftLight.hasValues == rightLight.hasValues &&
        leftLight.maxContentLightLevelNits == rightLight.maxContentLightLevelNits &&
        leftLight.maxFrameAverageLightLevelNits ==
            rightLight.maxFrameAverageLightLevelNits;
}

inline bool InterpolationPresentationConfigChanged(
    const anvil::playback::VideoSettings& previousSettings,
    const anvil::playback::DisplayCapabilities& previousDisplay,
    const anvil::playback::VideoColorMetadata& previousColor,
    const anvil::playback::VideoSettings& nextSettings,
    const anvil::playback::DisplayCapabilities& nextDisplay,
    const anvil::playback::VideoColorMetadata& nextColor) noexcept {
    const bool sameCurve = std::equal(
        previousSettings.hdrToneCurve.begin(), previousSettings.hdrToneCurve.end(),
        nextSettings.hdrToneCurve.begin(),
        [](const anvil::playback::HdrToneCurvePoint& left,
           const anvil::playback::HdrToneCurvePoint& right) {
            return left.inputNits == right.inputNits &&
                left.outputNits == right.outputNits;
        });
    return previousSettings.frameInterpolationEnabled !=
            nextSettings.frameInterpolationEnabled ||
        previousSettings.frameInterpolationMaximumHeight !=
            nextSettings.frameInterpolationMaximumHeight ||
        previousSettings.hdrOutput != nextSettings.hdrOutput ||
        previousSettings.toneMapping != nextSettings.toneMapping ||
        previousSettings.dolbyVision != nextSettings.dolbyVision ||
        previousSettings.displayMetadataPassthrough !=
            nextSettings.displayMetadataPassthrough ||
        previousSettings.dolbyVisionHdrOutput !=
            nextSettings.dolbyVisionHdrOutput ||
        previousSettings.dolbyVisionCmv4Approx !=
            nextSettings.dolbyVisionCmv4Approx ||
        previousSettings.displayPeakBrightnessNits !=
            nextSettings.displayPeakBrightnessNits ||
        previousSettings.peakBrightnessNits != nextSettings.peakBrightnessNits ||
        !sameCurve || previousDisplay.hdrEnabled != nextDisplay.hdrEnabled ||
        previousDisplay.reportedPeakBrightnessNits !=
            nextDisplay.reportedPeakBrightnessNits ||
        !SameInterpolationColorMetadata(previousColor, nextColor);
}

}  // namespace anvil::app
