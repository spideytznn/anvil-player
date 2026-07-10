#pragma once

namespace anvil::app {

struct VideoTextureUvRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 1.0f;
    float bottom = 1.0f;
};

struct VideoTextureSamplingRegion {
    bool valid = false;
    int visibleWidth = 0;
    int visibleHeight = 0;
    VideoTextureUvRect uvRect;
};

// Builds a normalized visible rectangle for a decoded frame stored inside a
// potentially larger, alignment-padded hardware texture. For FFmpeg hardware
// frames, right/bottom cropping may already be reflected in frameWidth/Height,
// while left/top cropping remains in the AVFrame crop fields. Subtracting the
// crop values that remain is therefore valid both before and after FFmpeg's
// default hardware-frame cropping step.
constexpr VideoTextureSamplingRegion BuildVideoTextureSamplingRegion(
    const int frameWidth,
    const int frameHeight,
    const int textureWidth,
    const int textureHeight,
    const int cropLeft = 0,
    const int cropTop = 0,
    const int cropRight = 0,
    const int cropBottom = 0) noexcept {
    if (frameWidth <= 0 || frameHeight <= 0 ||
        textureWidth <= 0 || textureHeight <= 0 ||
        cropLeft < 0 || cropTop < 0 || cropRight < 0 || cropBottom < 0) {
        return {};
    }
    if (cropLeft > frameWidth || cropRight > frameWidth - cropLeft ||
        cropTop > frameHeight || cropBottom > frameHeight - cropTop) {
        return {};
    }

    const int visibleWidth = frameWidth - cropLeft - cropRight;
    const int visibleHeight = frameHeight - cropTop - cropBottom;
    if (visibleWidth <= 0 || visibleHeight <= 0 ||
        cropLeft > textureWidth || visibleWidth > textureWidth - cropLeft ||
        cropTop > textureHeight || visibleHeight > textureHeight - cropTop) {
        return {};
    }

    return {
        true,
        visibleWidth,
        visibleHeight,
        {
            static_cast<float>(cropLeft) / static_cast<float>(textureWidth),
            static_cast<float>(cropTop) / static_cast<float>(textureHeight),
            static_cast<float>(cropLeft + visibleWidth) / static_cast<float>(textureWidth),
            static_cast<float>(cropTop + visibleHeight) / static_cast<float>(textureHeight),
        },
    };
}

}  // namespace anvil::app
