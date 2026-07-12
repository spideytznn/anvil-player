#include "AnvilPlayer/App/color_metadata_util.h"

#include "AnvilPlayer/App/string_util.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

namespace anvil::app {

using anvil::playback::ChromaticityPoint;
using anvil::playback::VideoColorMetadata;
using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

double RationalToDouble(const AVRational value) {
    if (value.num <= 0 || value.den <= 0) {
        return 0.0;
    }
    const double result = av_q2d(value);
    return std::isfinite(result) ? result : 0.0;
}

VideoColorPrimaries MapColorPrimaries(const AVColorPrimaries value) {
    switch (value) {
    case AVCOL_PRI_BT709:
        return VideoColorPrimaries::Bt709;
    case AVCOL_PRI_BT2020:
        return VideoColorPrimaries::Bt2020;
    case AVCOL_PRI_SMPTE432:
        return VideoColorPrimaries::DisplayP3;
    default:
        return VideoColorPrimaries::Unknown;
    }
}

VideoTransferCharacteristic MapTransfer(const AVColorTransferCharacteristic value) {
    switch (value) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_GAMMA22:
    case AVCOL_TRC_GAMMA28:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
        return VideoTransferCharacteristic::Bt709;
    case AVCOL_TRC_IEC61966_2_1:
        return VideoTransferCharacteristic::Srgb;
    case AVCOL_TRC_SMPTE2084:
        return VideoTransferCharacteristic::Pq;
    case AVCOL_TRC_ARIB_STD_B67:
        return VideoTransferCharacteristic::Hlg;
    default:
        return VideoTransferCharacteristic::Unknown;
    }
}

VideoMatrixCoefficients MapMatrix(const AVColorSpace value) {
    switch (value) {
    case AVCOL_SPC_RGB:
        return VideoMatrixCoefficients::Rgb;
    case AVCOL_SPC_BT709:
        return VideoMatrixCoefficients::Bt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return VideoMatrixCoefficients::Bt601;
    case AVCOL_SPC_BT2020_NCL:
        return VideoMatrixCoefficients::Bt2020Ncl;
    case AVCOL_SPC_BT2020_CL:
        return VideoMatrixCoefficients::Bt2020Cl;
    default:
        return VideoMatrixCoefficients::Unknown;
    }
}

VideoColorRange MapColorRange(const AVColorRange value) {
    switch (value) {
    case AVCOL_RANGE_MPEG:
        return VideoColorRange::Limited;
    case AVCOL_RANGE_JPEG:
        return VideoColorRange::Full;
    default:
        return VideoColorRange::Unknown;
    }
}

namespace {

ChromaticityPoint MakePoint(const AVRational x, const AVRational y) {
    return ChromaticityPoint{RationalToDouble(x), RationalToDouble(y)};
}

void ApplyMasteringDisplay(VideoColorMetadata& metadata, const AVMasteringDisplayMetadata* source) {
    if (!source) {
        return;
    }
    if (source->has_primaries) {
        metadata.masteringDisplay.hasPrimaries = true;
        metadata.masteringDisplay.red = MakePoint(source->display_primaries[0][0], source->display_primaries[0][1]);
        metadata.masteringDisplay.green = MakePoint(source->display_primaries[1][0], source->display_primaries[1][1]);
        metadata.masteringDisplay.blue = MakePoint(source->display_primaries[2][0], source->display_primaries[2][1]);
        metadata.masteringDisplay.whitePoint = MakePoint(source->white_point[0], source->white_point[1]);
    }
    if (source->has_luminance) {
        metadata.masteringDisplay.hasLuminance = true;
        metadata.masteringDisplay.minLuminanceNits = RationalToDouble(source->min_luminance);
        metadata.masteringDisplay.maxLuminanceNits = RationalToDouble(source->max_luminance);
    }
}

void ApplyContentLight(VideoColorMetadata& metadata, const AVContentLightMetadata* source) {
    if (!source) {
        return;
    }
    metadata.contentLight.hasValues = true;
    metadata.contentLight.maxContentLightLevelNits = static_cast<int>(source->MaxCLL);
    metadata.contentLight.maxFrameAverageLightLevelNits = static_cast<int>(source->MaxFALL);
}

}  // namespace

VideoColorMetadata BuildColorMetadata(const AVCodecParameters* parameters) {
    VideoColorMetadata metadata;
    if (!parameters) {
        return metadata;
    }

    metadata.primaries = MapColorPrimaries(parameters->color_primaries);
    metadata.transfer = MapTransfer(parameters->color_trc);
    metadata.matrix = MapMatrix(parameters->color_space);
    metadata.range = MapColorRange(parameters->color_range);

    for (int index = 0; index < parameters->nb_coded_side_data; ++index) {
        const AVPacketSideData& sideData = parameters->coded_side_data[index];
        if (sideData.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA &&
            sideData.size >= static_cast<int>(sizeof(AVMasteringDisplayMetadata))) {
            ApplyMasteringDisplay(metadata, reinterpret_cast<const AVMasteringDisplayMetadata*>(sideData.data));
        } else if (sideData.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL &&
                   sideData.size >= static_cast<int>(sizeof(AVContentLightMetadata))) {
            ApplyContentLight(metadata, reinterpret_cast<const AVContentLightMetadata*>(sideData.data));
        }
    }
    return metadata;
}

VideoColorMetadata MergeFrameColorMetadata(const AVFrame* frame, const VideoColorMetadata& defaults) {
    VideoColorMetadata metadata = defaults;
    if (!frame) {
        return metadata;
    }

    const auto primaries = MapColorPrimaries(frame->color_primaries);
    const auto transfer = MapTransfer(frame->color_trc);
    const auto matrix = MapMatrix(frame->colorspace);
    const auto range = MapColorRange(frame->color_range);
    if (primaries != VideoColorPrimaries::Unknown) {
        metadata.primaries = primaries;
    }
    if (transfer != VideoTransferCharacteristic::Unknown) {
        metadata.transfer = transfer;
    }
    if (matrix != VideoMatrixCoefficients::Unknown) {
        metadata.matrix = matrix;
    }
    if (range != VideoColorRange::Unknown) {
        metadata.range = range;
    }

    const AVFrameSideData* mastering = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (mastering && mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
        ApplyMasteringDisplay(metadata, reinterpret_cast<const AVMasteringDisplayMetadata*>(mastering->data));
    }

    const AVFrameSideData* light = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (light && light->size >= sizeof(AVContentLightMetadata)) {
        ApplyContentLight(metadata, reinterpret_cast<const AVContentLightMetadata*>(light->data));
    }

    return metadata;
}

std::shared_ptr<const std::vector<std::uint8_t>> ExtractHdr10PlusPayload(const AVFrame* frame) {
    const AVFrameSideData* sideData = frame
                                          ? av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS)
                                          : nullptr;
    if (!sideData || sideData->size < sizeof(AVDynamicHDRPlus)) return {};

    const auto* metadata = reinterpret_cast<const AVDynamicHDRPlus*>(sideData->data);
    std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
    if (av_dynamic_hdr_plus_to_t35(metadata, &payload, &payloadSize) < 0 || !payload || payloadSize == 0) {
        av_free(payload);
        return {};
    }
    auto result = std::make_shared<std::vector<std::uint8_t>>(payload, payload + payloadSize);
    av_free(payload);
    return result;
}

std::shared_ptr<const Hdr10PlusFrameMetadata> ExtractHdr10PlusMetadata(const AVFrame* frame) {
    const AVFrameSideData* sideData = frame
                                          ? av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS)
                                          : nullptr;
    if (!sideData || !sideData->data || sideData->size < sizeof(AVDynamicHDRPlus)) return {};

    const auto* source = reinterpret_cast<const AVDynamicHDRPlus*>(sideData->data);
    if (source->num_windows == 0 || source->num_windows > 3) return {};

    const AVHDRPlusColorTransformParams& transform = source->params[0];
    auto metadata = std::make_shared<Hdr10PlusFrameMetadata>();
    metadata->applicationVersion = source->application_version;
    metadata->numWindows = source->num_windows;
    metadata->targetedPeakNits = static_cast<float>(RationalToDouble(
        source->targeted_system_display_maximum_luminance));
    const double maxSclRed = RationalToDouble(transform.maxscl[0]);
    const double maxSclGreen = RationalToDouble(transform.maxscl[1]);
    const double maxSclBlue = RationalToDouble(transform.maxscl[2]);
    double maxRgb = std::max({maxSclRed, maxSclGreen, maxSclBlue});
    double normalizedSourcePeak = 0.0;
    if (maxRgb > 0.0) {
        // SMPTE ST 2094-40 Annex B defines SMC as max(MaxSCL). Per-channel
        // luma weighting underestimates saturated highlights and can make the
        // display adaptation believe a scene is much dimmer than it is.
        normalizedSourcePeak = maxRgb;
        metadata->averageMaxRgbNits = static_cast<float>(
            std::max(0.0, RationalToDouble(transform.average_maxrgb)) * 10000.0);
    } else {
        for (std::size_t index = 0;
             index < std::min<std::uint8_t>(transform.num_distribution_maxrgb_percentiles, 15);
             ++index) {
            maxRgb = std::max(maxRgb, RationalToDouble(transform.distribution_maxrgb[index].percentile));
        }
        normalizedSourcePeak = maxRgb;
        metadata->averageMaxRgbNits = static_cast<float>(
            std::max(0.0, RationalToDouble(transform.average_maxrgb)) * 10000.0);
    }
    metadata->sourcePeakNits = static_cast<float>(
        std::clamp(normalizedSourcePeak * 10000.0, 0.0, 10000.0));
    metadata->toneMappingPresent = transform.tone_mapping_flag != 0;
    metadata->kneePointX = static_cast<float>(RationalToDouble(transform.knee_point_x));
    metadata->kneePointY = static_cast<float>(RationalToDouble(transform.knee_point_y));
    metadata->anchorCount = std::min<std::uint8_t>(transform.num_bezier_curve_anchors, 15);
    for (std::size_t index = 0; index < metadata->anchorCount; ++index) {
        metadata->bezierAnchors[index] = static_cast<float>(
            std::clamp(RationalToDouble(transform.bezier_curve_anchors[index]), 0.0, 1.0));
    }
    if (transform.color_saturation_mapping_flag != 0) {
        metadata->saturationWeight = static_cast<float>(
            std::clamp(RationalToDouble(transform.color_saturation_weight), 0.0, 7.875));
    }

    const bool validExplicitCurve = metadata->toneMappingPresent &&
                                    metadata->kneePointX > 0.0f && metadata->kneePointX < 1.0f &&
                                    metadata->kneePointY > 0.0f && metadata->kneePointY < 1.0f;
    const bool validPeaks = metadata->targetedPeakNits > 0.0f && metadata->sourcePeakNits > 0.0f;
    if (!validExplicitCurve && validPeaks) {
        // ST 2094-40 permits scene statistics without an authored curve. Use
        // a two-segment curve that is linear through half of the target range,
        // then compresses smoothly to the scene peak. When the authored target
        // exceeds the scene peak this reduces to an identity mapping.
        metadata->targetedPeakNits = std::min(metadata->targetedPeakNits, metadata->sourcePeakNits);
        metadata->kneePointX = std::clamp(
            0.5f * metadata->targetedPeakNits / metadata->sourcePeakNits,
            0.0001f,
            0.5f);
        metadata->kneePointY = 0.5f;
        metadata->anchorCount = 0;
    }
    metadata->valid = source->application_version <= 1 && validPeaks &&
                      metadata->kneePointX > 0.0f && metadata->kneePointX < 1.0f &&
                      metadata->kneePointY > 0.0f && metadata->kneePointY < 1.0f;

    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < sideData->size; ++index) {
        hash ^= sideData->data[index];
        hash *= 1099511628211ull;
    }
    metadata->fingerprint = hash;
    return metadata;
}

std::wstring Hdr10PlusFrameSummary(const Hdr10PlusFrameMetadata* metadata) {
    if (!metadata) return L"metadata=missing";
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(1)
           << L"windows=" << static_cast<unsigned>(metadata->numWindows)
           << L" source_peak=" << metadata->sourcePeakNits
           << L" target_peak=" << metadata->targetedPeakNits
           << std::setprecision(4)
           << L" knee=" << metadata->kneePointX << L"," << metadata->kneePointY
           << L" anchors=" << static_cast<unsigned>(metadata->anchorCount)
           << L" curve=" << (metadata->valid
                                  ? (metadata->toneMappingPresent ? L"authored" : L"generated")
                                  : L"unavailable");
    return stream.str();
}

std::shared_ptr<const std::vector<std::uint8_t>> ExtractDolbyVisionRpu(const AVFrame* frame) {
    const AVFrameSideData* sideData = frame
                                          ? av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER)
                                          : nullptr;
    if (!sideData || !sideData->data || sideData->size == 0) {
        return {};
    }
    return std::make_shared<std::vector<std::uint8_t>>(
        sideData->data,
        sideData->data + sideData->size);
}

VideoColorMetadata SdrBt709ColorMetadata() {
    VideoColorMetadata metadata;
    metadata.primaries = VideoColorPrimaries::Bt709;
    metadata.transfer = VideoTransferCharacteristic::Bt709;
    metadata.matrix = VideoMatrixCoefficients::Rgb;
    metadata.range = VideoColorRange::Full;
    return metadata;
}

VideoColorMetadata HdrBt2020PqColorMetadata() {
    VideoColorMetadata metadata;
    metadata.primaries = VideoColorPrimaries::Bt2020;
    metadata.transfer = VideoTransferCharacteristic::Pq;
    metadata.matrix = VideoMatrixCoefficients::Rgb;
    metadata.range = VideoColorRange::Full;
    return metadata;
}

}  // namespace anvil::app
