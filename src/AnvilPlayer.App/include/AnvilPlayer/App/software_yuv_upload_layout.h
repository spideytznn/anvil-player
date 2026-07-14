#pragma once

#include <cstddef>
#include <limits>

namespace anvil::app {

enum class SoftwareYuvTextureFormat {
    Nv12,
    P010,
};

struct SoftwareYuvUploadLayout {
    bool valid = false;
    SoftwareYuvTextureFormat format = SoftwareYuvTextureFormat::Nv12;
    std::size_t lumaOffset = 0;
    std::size_t chromaOffset = 0;
    std::size_t lumaRowBytes = 0;
    std::size_t chromaRowBytes = 0;
    int lumaRows = 0;
    int chromaRows = 0;
};

inline SoftwareYuvUploadLayout BuildSoftwareYuvUploadLayout(
    const int width,
    const int height,
    const int yStride,
    const int uvStride,
    const int bitDepth,
    const std::size_t bufferSize) noexcept {
    if (width <= 0 || height <= 0 || yStride <= 0 || uvStride <= 0 ||
        (width % 2) != 0 || (height % 2) != 0 ||
        (bitDepth != 8 && bitDepth != 10)) {
        return {};
    }

    const std::size_t bytesPerSample = bitDepth == 8 ? 1u : 2u;
    const std::size_t widthBytes = static_cast<std::size_t>(width);
    if (widthBytes > std::numeric_limits<std::size_t>::max() / bytesPerSample) {
        return {};
    }
    const std::size_t activeRowBytes = widthBytes * bytesPerSample;
    if (static_cast<std::size_t>(yStride) < activeRowBytes ||
        static_cast<std::size_t>(uvStride) < activeRowBytes) {
        return {};
    }

    const std::size_t lumaRows = static_cast<std::size_t>(height);
    const std::size_t chromaRows = lumaRows / 2;
    const std::size_t yStrideBytes = static_cast<std::size_t>(yStride);
    const std::size_t uvStrideBytes = static_cast<std::size_t>(uvStride);
    if (yStrideBytes > std::numeric_limits<std::size_t>::max() / lumaRows ||
        uvStrideBytes > std::numeric_limits<std::size_t>::max() / chromaRows) {
        return {};
    }
    const std::size_t lumaBytes = yStrideBytes * lumaRows;
    const std::size_t chromaBytes = uvStrideBytes * chromaRows;
    if (lumaBytes > std::numeric_limits<std::size_t>::max() - chromaBytes ||
        lumaBytes + chromaBytes > bufferSize) {
        return {};
    }

    SoftwareYuvUploadLayout layout;
    layout.valid = true;
    layout.format = bitDepth == 8
        ? SoftwareYuvTextureFormat::Nv12
        : SoftwareYuvTextureFormat::P010;
    layout.chromaOffset = lumaBytes;
    layout.lumaRowBytes = activeRowBytes;
    layout.chromaRowBytes = activeRowBytes;
    layout.lumaRows = height;
    layout.chromaRows = height / 2;
    return layout;
}

}  // namespace anvil::app
