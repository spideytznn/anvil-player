#pragma once

// Video colour-metadata mapping between FFmpeg and the app's playback types.
// Extracted from ffmpeg_video_decoder.cpp: these are pure functions that
// translate AVCodecParameters / AVFrame colour fields and mastering-display /
// content-light side data into VideoColorMetadata, plus the fixed SDR/HDR
// presets used by the software-frame publish paths.

#include <cstdint>
#include <memory>
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
std::shared_ptr<const std::vector<std::uint8_t>> ExtractDolbyVisionRpu(const AVFrame* frame);

// Fixed presets for software-rendered frames.
anvil::playback::VideoColorMetadata SdrBt709ColorMetadata();
anvil::playback::VideoColorMetadata HdrBt2020PqColorMetadata();

}  // namespace anvil::app
