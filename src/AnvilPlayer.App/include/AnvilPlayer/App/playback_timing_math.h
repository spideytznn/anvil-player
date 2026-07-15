#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace anvil::app {

inline std::chrono::milliseconds EndpointClockMediaPosition(
    const std::chrono::milliseconds mediaAnchor,
    const uint64_t endpointAnchor,
    const uint64_t endpointPosition,
    const uint64_t endpointFrequency,
    const double playbackRate) noexcept {
    if (endpointFrequency == 0 || endpointPosition <= endpointAnchor) {
        return mediaAnchor;
    }
    const long double elapsedMilliseconds =
        static_cast<long double>(endpointPosition - endpointAnchor) * 1000.0L *
        static_cast<long double>(playbackRate) /
        static_cast<long double>(endpointFrequency);
    return mediaAnchor + std::chrono::milliseconds{
        static_cast<long long>(std::llround(elapsedMilliseconds))};
}

enum class NetworkRebufferTransition {
    None,
    Enter,
    Hold,
    Exit,
};

struct NetworkRebufferPolicyInput {
    bool networkSource = false;
    bool paused = false;
    bool inputEnded = false;
    bool seekPending = false;
    bool buffering = false;
    uint64_t renderedFrames = 0;
    std::size_t decodedFrameDepth = 0;
    std::size_t decodedFrameCapacity = 4;
    std::size_t packetDepth = 0;
    std::chrono::milliseconds bufferedDuration{0};
    std::chrono::milliseconds readAheadDuration{0};
};

struct InterpolationTensorExtent {
    uint32_t width = 0;
    uint32_t height = 0;
};

inline InterpolationTensorExtent SelectCappedInterpolationExtent(
    const uint32_t sourceWidth,
    const uint32_t sourceHeight,
    const uint32_t requestedMaximumPictureHeight) noexcept {
    if (sourceWidth == 0 || sourceHeight == 0 ||
        requestedMaximumPictureHeight == 0) {
        return {};
    }
    constexpr uint32_t kMinimumPictureHeight = 1080;
    constexpr uint32_t kMaximumPictureHeightLimit = 2160;
    constexpr uint32_t kAlignment = 32;
    const uint32_t maximumPictureHeight = std::clamp(
        requestedMaximumPictureHeight,
        kMinimumPictureHeight,
        kMaximumPictureHeightLimit);
    const uint32_t maximumWidth = maximumPictureHeight * 16 / 9;
    const uint32_t maximumAlignedHeight =
        ((maximumPictureHeight + kAlignment - 1) / kAlignment) * kAlignment;
    const double scale = std::min({
        1.0,
        static_cast<double>(maximumWidth) / sourceWidth,
        static_cast<double>(maximumPictureHeight) / sourceHeight});
    const uint32_t scaledWidth = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::llround(sourceWidth * scale)));
    const uint32_t scaledHeight = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::llround(sourceHeight * scale)));
    const auto alignUp = [](const uint32_t value) {
        return ((value + kAlignment - 1) / kAlignment) * kAlignment;
    };
    const auto alignNearest = [](const uint32_t value) {
        return std::max(kAlignment,
                        ((value + kAlignment / 2) / kAlignment) * kAlignment);
    };
    // Never reduce a source that is already within the selected cap. Larger
    // sources are scaled proportionally first, then rounded to the nearest
    // model-safe extent so ultrawide and portrait content retain their shape.
    const bool capped = scale < 0.999999;
    return {
        std::min(maximumWidth,
                 capped ? alignNearest(scaledWidth) : alignUp(scaledWidth)),
        std::min(maximumAlignedHeight,
                 capped ? alignNearest(scaledHeight) : alignUp(scaledHeight))};
}

inline NetworkRebufferTransition EvaluateNetworkRebuffer(
    const NetworkRebufferPolicyInput& input) noexcept {
    if (input.buffering) {
        if (!input.networkSource || input.paused || input.inputEnded || input.seekPending) {
            return NetworkRebufferTransition::Exit;
        }
        const std::size_t recoveryFrameTarget =
            std::clamp(input.decodedFrameCapacity, std::size_t{1}, std::size_t{4});
        const bool decodedReady =
            input.decodedFrameDepth >= std::min(std::size_t{2}, recoveryFrameTarget);
        const bool recoveryWatermark =
            input.decodedFrameDepth >= recoveryFrameTarget ||
            input.bufferedDuration >= std::chrono::milliseconds{750} ||
            input.readAheadDuration >= std::chrono::milliseconds{1000};
        return decodedReady && recoveryWatermark
            ? NetworkRebufferTransition::Exit
            : NetworkRebufferTransition::Hold;
    }

    const bool depleted = input.networkSource && !input.paused &&
        !input.inputEnded && !input.seekPending && input.renderedFrames > 0 &&
        input.decodedFrameDepth == 0 && input.packetDepth == 0 &&
        input.readAheadDuration <= std::chrono::milliseconds{100};
    return depleted
        ? NetworkRebufferTransition::Enter
        : NetworkRebufferTransition::None;
}

inline int Fixed2xInterpolationMultiplier(
    const double sourceFps,
    const double refreshHz) noexcept {
    if (!std::isfinite(sourceFps) || !std::isfinite(refreshHz) ||
        sourceFps <= 1.0 || refreshHz <= 1.0) {
        return 1;
    }
    // EnumDisplaySettings exposes fractional NTSC modes through nominal
    // integers. Allow only the tiny reconstruction tolerance already used by
    // the display controller; never schedule a 2x cadence above the display.
    constexpr double kRefreshToleranceHz = 0.015;
    return sourceFps * 2.0 <= refreshHz + kRefreshToleranceHz ? 2 : 1;
}

}  // namespace anvil::app
