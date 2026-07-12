#pragma once

// Video colour-metadata mapping between FFmpeg and the app's playback types.
// Extracted from ffmpeg_video_decoder.cpp: these are pure functions that
// translate AVCodecParameters / AVFrame colour fields and mastering-display /
// content-light side data into VideoColorMetadata, plus the fixed SDR/HDR
// presets used by the software-frame publish paths.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>  // AVColorPrimaries, AVColorSpace, etc.
#include <libavutil/rational.h>
}

#include "AnvilPlayer/Playback/Types.h"

// FFmpeg forward declarations (C types in the global namespace).
struct AVCodecParameters;
struct AVFrame;

namespace anvil::app {

struct Hdr10PlusFrameMetadata {
    bool valid = false;
    bool toneMappingPresent = false;
    std::uint8_t applicationVersion = 0;
    std::uint8_t numWindows = 0;
    std::uint8_t anchorCount = 0;
    float targetedPeakNits = 0.0f;
    float sourcePeakNits = 0.0f;
    float averageMaxRgbNits = 0.0f;
    float kneePointX = 0.0f;
    float kneePointY = 0.0f;
    float saturationWeight = 1.0f;
    std::array<float, 15> bezierAnchors{};
    std::uint64_t fingerprint = 0;
};

// FFmpeg AVRational -> double (0 if invalid).
double RationalToDouble(AVRational value);

// FFmpeg enum -> playback enum mappings.
anvil::playback::VideoColorPrimaries MapColorPrimaries(AVColorPrimaries value);
anvil::playback::VideoTransferCharacteristic MapTransfer(AVColorTransferCharacteristic value);
anvil::playback::VideoMatrixCoefficients MapMatrix(AVColorSpace value);
anvil::playback::VideoColorRange MapColorRange(AVColorRange value);

// Builds colour metadata from codec parameters (stream-level defaults).
anvil::playback::VideoColorMetadata BuildColorMetadata(const AVCodecParameters* parameters);

// Merges per-frame colour metadata over stream-level defaults.
anvil::playback::VideoColorMetadata MergeFrameColorMetadata(
    const AVFrame* frame,
    const anvil::playback::VideoColorMetadata& defaults);

std::shared_ptr<const std::vector<std::uint8_t>> ExtractHdr10PlusPayload(const AVFrame* frame);
std::shared_ptr<const Hdr10PlusFrameMetadata> ExtractHdr10PlusMetadata(const AVFrame* frame);
std::wstring Hdr10PlusFrameSummary(const Hdr10PlusFrameMetadata* metadata);
std::shared_ptr<const std::vector<std::uint8_t>> ExtractDolbyVisionRpu(const AVFrame* frame);

// Fixed presets for software-rendered frames.
anvil::playback::VideoColorMetadata SdrBt709ColorMetadata();
anvil::playback::VideoColorMetadata HdrBt2020PqColorMetadata();

}  // namespace anvil::app
