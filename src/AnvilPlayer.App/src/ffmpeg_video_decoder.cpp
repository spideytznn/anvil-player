#include "AnvilPlayer/App/ffmpeg_video_decoder.h"

#include "AnvilPlayer/App/color_math.h"
#include "AnvilPlayer/App/color_metadata_util.h"
#include "AnvilPlayer/App/libass_subtitle_renderer.h"
#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/subtitle_util.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace anvil::app {

struct DoviLibplaceboFilterState {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    int width = 0;
    int height = 0;
    int format = AV_PIX_FMT_NONE;
    bool hdrOutput = false;
    AVRational timeBase{1, 1};
    AVRational sampleAspectRatio{1, 1};

    ~DoviLibplaceboFilterState() {
        if (graph) {
            avfilter_graph_free(&graph);
        }
    }
};

using anvil::playback::LogLevel;
using anvil::playback::DolbyVisionFrameMetadata;
using anvil::playback::DoviMappingMethod;
using anvil::playback::DoviNlqMethod;
using anvil::playback::DoviReshapingCurve;
using anvil::playback::DoviReshapingPiece;
using anvil::playback::kDoviMaxPieces;
using anvil::playback::kDoviMaxPivots;
using anvil::playback::kDoviMmrCoeffsPerOrder;
using anvil::playback::kDoviMmrMaxTerms;
using anvil::playback::kDoviNumComponents;
using anvil::playback::VideoColorMetadata;
using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

namespace {

int64_t SteadyClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool FitsWithinBudget(const std::size_t current,
                      const std::size_t incoming,
                      const std::size_t budget) {
    return incoming <= budget && current <= budget - incoming;
}

std::size_t SaturatingAddBytes(const std::size_t lhs, const std::size_t rhs) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

std::size_t BufferCapacityBytes(const std::shared_ptr<const std::vector<uint8_t>>& buffer) {
    return buffer ? buffer->capacity() : 0;
}

uint64_t HashBytes(uint64_t hash, const void* data, const std::size_t size) {
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

template <typename T>
uint64_t HashValue(uint64_t hash, const T& value) {
    return HashBytes(hash, &value, sizeof(value));
}

std::wstring Hex64(const uint64_t value) {
    std::wostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return stream.str();
}

std::wstring DoviDmLevelsSummary(const DolbyVisionFrameMetadata& metadata) {
    std::wstring levels;
    const auto append = [&levels](const std::wstring& value) {
        if (!levels.empty()) {
            levels += L",";
        }
        levels += value;
    };

    for (int level = 0; level < 64; ++level) {
        if ((metadata.dmLevelMaskLow & (uint64_t{1} << level)) == 0) {
            continue;
        }
        switch (level) {
        case 1:
            append(L"L1");
            break;
        case 2:
            append(metadata.dmLevel2Count > 1
                       ? L"L2x" + std::to_wstring(metadata.dmLevel2Count)
                       : L"L2");
            break;
        case 3:
            append(L"L3");
            break;
        case 5:
            append(L"L5");
            break;
        case 8:
            append(metadata.dmLevel8Count > 1
                       ? L"L8x" + std::to_wstring(metadata.dmLevel8Count)
                       : L"L8");
            break;
        default:
            append(L"L" + std::to_wstring(level));
            break;
        }
    }
    if (metadata.dmLevel254Present) append(L"L254");
    if (metadata.dmLevel255Present) append(L"L255");
    return levels.empty() ? L"none" : levels;
}

std::wstring DoviFrameSummary(const DolbyVisionFrameMetadata* metadata) {
    if (!metadata || !metadata->valid) {
        return L"none";
    }

    std::wostringstream stream;
    stream << L"profile=" << metadata->profile
           << L" compat=" << metadata->compatibilityId
           << L" source_max_pq=" << metadata->sourceMaxPq
           << L" dm_ext=" << metadata->dmExtensionBlockCount
           << L" dm_levels=" << DoviDmLevelsSummary(*metadata);
    return stream.str();
}

std::wstring DoviDynamicLogSummary(const DolbyVisionFrameMetadata& metadata) {
    std::wostringstream stream;
    stream << DoviFrameSummary(&metadata)
           << L" scene_refresh=" << metadata.sceneRefreshFlag
           << L" dm_hash=" << Hex64(metadata.dynamicMetadataFingerprint);
    if (metadata.nlqNumXPartitions > 0 || metadata.nlqNumYPartitions > 0) {
        stream << L" partitions=" << metadata.nlqNumXPartitions << L"x" << metadata.nlqNumYPartitions;
    }
    if (metadata.dmLevel1Present) {
        stream << L" L1(min=" << metadata.dmLevel1MinPq
               << L" max=" << metadata.dmLevel1MaxPq
               << L" avg=" << metadata.dmLevel1AvgPq << L")";
    }
    if (metadata.dmLevel2Present && metadata.dmLevel2Count > 0) {
        stream << L" L2(target=" << metadata.dmLevel2TargetMaxPq[0]
               << L" slope=" << metadata.dmLevel2TrimSlope[0]
               << L" offset=" << metadata.dmLevel2TrimOffset[0]
               << L" power=" << metadata.dmLevel2TrimPower[0]
               << L" sat=" << metadata.dmLevel2TrimSaturationGain[0] << L")";
    }
    if (metadata.dmLevel3Present) {
        stream << L" L3(min_off=" << metadata.dmLevel3MinPqOffset
               << L" max_off=" << metadata.dmLevel3MaxPqOffset
               << L" avg_off=" << metadata.dmLevel3AvgPqOffset << L")";
    }
    if (metadata.dmLevel5Present) {
        stream << L" L5(active_area left=" << metadata.dmLevel5LeftOffset
               << L" right=" << metadata.dmLevel5RightOffset
               << L" top=" << metadata.dmLevel5TopOffset
               << L" bottom=" << metadata.dmLevel5BottomOffset << L")";
    }
    if (metadata.dmLevel8Present) {
        stream << L" L8(target=" << static_cast<int>(metadata.dmLevel8TargetDisplayIndex)
               << L" slope=" << metadata.dmLevel8TrimSlope
               << L" offset=" << metadata.dmLevel8TrimOffset
               << L" power=" << metadata.dmLevel8TrimPower
               << L" sat=" << metadata.dmLevel8TrimSaturationGain << L")";
    }
    return stream.str();
}

uint16_t ReadLe16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(value[1]) << 8);
}

void WriteLe16(uint8_t* destination, const uint16_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xFF);
    destination[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint16_t Yuv420P10SampleToP010(const uint8_t* value) {
    // FFmpeg yuv420p10le stores the 10 useful bits in the low bits of a
    // 16-bit little-endian word. DXGI P010 expects those bits in the high
    // 10 bits, so shift before uploading to the P010 texture.
    return static_cast<uint16_t>((ReadLe16(value) & 0x03FFu) << 6);
}

void ConvertYuv420P10RowToP010(uint8_t* destination, const uint8_t* source, const int samples) {
    const __m128i mask10 = _mm_set1_epi16(static_cast<short>(0x03FF));
    int x = 0;
    for (; x + 8 <= samples; x += 8) {
        const __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + static_cast<std::size_t>(x) * 2));
        const __m128i out = _mm_slli_epi16(_mm_and_si128(src, mask10), 6);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + static_cast<std::size_t>(x) * 2), out);
    }
    for (; x < samples; ++x) {
        WriteLe16(destination + static_cast<std::size_t>(x) * 2,
                  Yuv420P10SampleToP010(source + static_cast<std::size_t>(x) * 2));
    }
}

void InterleaveYuv420P10RowToP010Uv(uint8_t* destination,
                                    const uint8_t* sourceU,
                                    const uint8_t* sourceV,
                                    const int chromaSamples) {
    const __m128i mask10 = _mm_set1_epi16(static_cast<short>(0x03FF));
    int x = 0;
    for (; x + 8 <= chromaSamples; x += 8) {
        const __m128i u = _mm_slli_epi16(
            _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sourceU + static_cast<std::size_t>(x) * 2)), mask10),
            6);
        const __m128i v = _mm_slli_epi16(
            _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sourceV + static_cast<std::size_t>(x) * 2)), mask10),
            6);
        const __m128i lo = _mm_unpacklo_epi16(u, v);
        const __m128i hi = _mm_unpackhi_epi16(u, v);
        uint8_t* dst = destination + static_cast<std::size_t>(x) * 4;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), lo);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), hi);
    }
    for (; x < chromaSamples; ++x) {
        WriteLe16(destination + static_cast<std::size_t>(x) * 4,
                  Yuv420P10SampleToP010(sourceU + static_cast<std::size_t>(x) * 2));
        WriteLe16(destination + static_cast<std::size_t>(x) * 4 + 2,
                  Yuv420P10SampleToP010(sourceV + static_cast<std::size_t>(x) * 2));
    }
}

bool PackYuv420P10FrameToP010(const AVFrame* frame,
                              NativeYuvPlanes& out,
                              const std::size_t maxBytes) {
    if (!frame ||
        frame->format != AV_PIX_FMT_YUV420P10LE ||
        frame->width <= 0 ||
        frame->height <= 0 ||
        (frame->width % 2) != 0 ||
        (frame->height % 2) != 0 ||
        !frame->data[0] ||
        !frame->data[1] ||
        !frame->data[2]) {
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    if (width > std::numeric_limits<int>::max() / 2) {
        return false;
    }
    const int yStride = width * 2;
    const int uvStride = yStride;
    const int uvHeight = height / 2;
    const std::size_t yBytes = static_cast<std::size_t>(yStride) * static_cast<std::size_t>(height);
    const std::size_t uvBytes = static_cast<std::size_t>(uvStride) * static_cast<std::size_t>(uvHeight);
    if (!FitsWithinBudget(yBytes, uvBytes, maxBytes)) {
        return false;
    }
    const std::size_t totalBytes = yBytes + uvBytes;
    std::shared_ptr<std::vector<uint8_t>> buffer;
    try {
        buffer = std::make_shared<std::vector<uint8_t>>();
        buffer->resize(totalBytes);
    } catch (const std::bad_alloc&) {
        return false;
    }

    const uint8_t* srcY = frame->data[0];
    const int srcYStride = frame->linesize[0];
    for (int row = 0; row < height; ++row) {
        const uint8_t* srcRow = srcY + static_cast<std::size_t>(row) * srcYStride;
        uint8_t* dstRow = buffer->data() + static_cast<std::size_t>(row) * yStride;
        ConvertYuv420P10RowToP010(dstRow, srcRow, width);
    }

    const uint8_t* srcU = frame->data[1];
    const uint8_t* srcV = frame->data[2];
    const int srcUStride = frame->linesize[1];
    const int srcVStride = frame->linesize[2];
    uint8_t* uvDst = buffer->data() + static_cast<std::size_t>(yStride) * height;
    for (int row = 0; row < uvHeight; ++row) {
        const uint8_t* uRow = srcU + static_cast<std::size_t>(row) * srcUStride;
        const uint8_t* vRow = srcV + static_cast<std::size_t>(row) * srcVStride;
        uint8_t* dstRow = uvDst + static_cast<std::size_t>(row) * uvStride;
        InterleaveYuv420P10RowToP010Uv(dstRow, uRow, vRow, width / 2);
    }

    out.width = width;
    out.height = height;
    out.yStride = yStride;
    out.uvStride = uvStride;
    out.bitDepth = 10;
    out.data = std::move(buffer);
    return true;
}

bool DoviSingleNlqPartition(const DolbyVisionFrameMetadata& metadata) {
    return metadata.nlqNumXPartitions <= 1 && metadata.nlqNumYPartitions <= 1;
}

int DoviBitDepthMax(const int bitDepth) {
    const int depth = std::clamp(bitDepth, 1, 16);
    return static_cast<int>((uint32_t{1} << depth) - 1u);
}

uint16_t ReadP010PackedCode(const NativeYuvPlanes& planes, const std::size_t offset) {
    if (!planes.data || offset + 2 > planes.data->size()) {
        return 0;
    }
    const int depth = std::clamp(planes.bitDepth, 8, 16);
    return static_cast<uint16_t>(ReadLe16(planes.data->data() + offset) >> (16 - depth));
}

uint16_t ReadP010YCode(const NativeYuvPlanes& planes, const int x, const int y) {
    if (!planes.HasData()) {
        return 0;
    }
    const int clampedX = std::clamp(x, 0, planes.width - 1);
    const int clampedY = std::clamp(y, 0, planes.height - 1);
    const std::size_t offset = static_cast<std::size_t>(clampedY) * planes.yStride +
                               static_cast<std::size_t>(clampedX) * 2;
    return ReadP010PackedCode(planes, offset);
}

uint16_t ReadP010UvCode(const NativeYuvPlanes& planes, const int chromaX, const int chromaY, const int component) {
    if (!planes.HasData()) {
        return 0;
    }
    const int chromaWidth = std::max(1, planes.width / 2);
    const int chromaHeight = std::max(1, planes.height / 2);
    const int clampedX = std::clamp(chromaX, 0, chromaWidth - 1);
    const int clampedY = std::clamp(chromaY, 0, chromaHeight - 1);
    const int uvComponent = component == 2 ? 1 : 0;
    const std::size_t uvBase = static_cast<std::size_t>(planes.yStride) * planes.height;
    const std::size_t offset = uvBase +
                               static_cast<std::size_t>(clampedY) * planes.uvStride +
                               static_cast<std::size_t>(clampedX) * 4 +
                               static_cast<std::size_t>(uvComponent) * 2;
    return ReadP010PackedCode(planes, offset);
}

double P010YCodeNorm(const NativeYuvPlanes& planes, const int x, const int y) {
    return static_cast<double>(ReadP010YCode(planes, x, y)) /
           static_cast<double>(std::max(1, DoviBitDepthMax(planes.bitDepth)));
}

double P010UvCodeNorm(const NativeYuvPlanes& planes, const int chromaX, const int chromaY, const int component) {
    return static_cast<double>(ReadP010UvCode(planes, chromaX, chromaY, component)) /
           static_cast<double>(std::max(1, DoviBitDepthMax(planes.bitDepth)));
}

double DoviCubicWeight(double x) {
    x = std::abs(x);
    if (x <= 1.0) {
        return ((1.5 * x - 2.5) * x * x + 1.0);
    }
    if (x < 2.0) {
        return (((-0.5 * x + 2.5) * x - 4.0) * x + 2.0);
    }
    return 0.0;
}

double SampleP010YCodeLinear(const NativeYuvPlanes& planes, const double uvX, const double uvY) {
    const double coordX = std::clamp(uvX, 0.0, 1.0) * planes.width - 0.5;
    const double coordY = std::clamp(uvY, 0.0, 1.0) * planes.height - 0.5;
    const int baseX = static_cast<int>(std::floor(coordX));
    const int baseY = static_cast<int>(std::floor(coordY));
    const double fracX = coordX - std::floor(coordX);
    const double fracY = coordY - std::floor(coordY);
    const double s00 = ReadP010YCode(planes, baseX, baseY);
    const double s10 = ReadP010YCode(planes, baseX + 1, baseY);
    const double s01 = ReadP010YCode(planes, baseX, baseY + 1);
    const double s11 = ReadP010YCode(planes, baseX + 1, baseY + 1);
    const double top = s00 + (s10 - s00) * fracX;
    const double bottom = s01 + (s11 - s01) * fracX;
    return top + (bottom - top) * fracY;
}

double SampleP010YCodeCubic(const NativeYuvPlanes& planes, const double uvX, const double uvY) {
    const double coordX = std::clamp(uvX, 0.0, 1.0) * planes.width - 0.5;
    const double coordY = std::clamp(uvY, 0.0, 1.0) * planes.height - 0.5;
    const int baseX = static_cast<int>(std::floor(coordX));
    const int baseY = static_cast<int>(std::floor(coordY));
    const double fracX = coordX - std::floor(coordX);
    const double fracY = coordY - std::floor(coordY);
    double sum = 0.0;
    double weightSum = 0.0;
    for (int j = -1; j <= 2; ++j) {
        const double wy = DoviCubicWeight(static_cast<double>(j) - fracY);
        for (int i = -1; i <= 2; ++i) {
            const double wx = DoviCubicWeight(static_cast<double>(i) - fracX);
            const double weight = wx * wy;
            sum += static_cast<double>(ReadP010YCode(planes, baseX + i, baseY + j)) * weight;
            weightSum += weight;
        }
    }
    return weightSum > 0.0 ? sum / weightSum : 0.0;
}

double SampleP010UvCodeLinear(const NativeYuvPlanes& planes, const double uvX, const double uvY, const int component) {
    const int chromaWidth = std::max(1, planes.width / 2);
    const int chromaHeight = std::max(1, planes.height / 2);
    const double coordX = std::clamp(uvX, 0.0, 1.0) * chromaWidth - 0.5;
    const double coordY = std::clamp(uvY, 0.0, 1.0) * chromaHeight - 0.5;
    const int baseX = static_cast<int>(std::floor(coordX));
    const int baseY = static_cast<int>(std::floor(coordY));
    const double fracX = coordX - std::floor(coordX);
    const double fracY = coordY - std::floor(coordY);
    const double s00 = ReadP010UvCode(planes, baseX, baseY, component);
    const double s10 = ReadP010UvCode(planes, baseX + 1, baseY, component);
    const double s01 = ReadP010UvCode(planes, baseX, baseY + 1, component);
    const double s11 = ReadP010UvCode(planes, baseX + 1, baseY + 1, component);
    const double top = s00 + (s10 - s00) * fracX;
    const double bottom = s01 + (s11 - s01) * fracX;
    return top + (bottom - top) * fracY;
}

double SampleP010UvCodeCubic(const NativeYuvPlanes& planes, const double uvX, const double uvY, const int component) {
    const int chromaWidth = std::max(1, planes.width / 2);
    const int chromaHeight = std::max(1, planes.height / 2);
    const double coordX = std::clamp(uvX, 0.0, 1.0) * chromaWidth - 0.5;
    const double coordY = std::clamp(uvY, 0.0, 1.0) * chromaHeight - 0.5;
    const int baseX = static_cast<int>(std::floor(coordX));
    const int baseY = static_cast<int>(std::floor(coordY));
    const double fracX = coordX - std::floor(coordX);
    const double fracY = coordY - std::floor(coordY);
    double sum = 0.0;
    double weightSum = 0.0;
    for (int j = -1; j <= 2; ++j) {
        const double wy = DoviCubicWeight(static_cast<double>(j) - fracY);
        for (int i = -1; i <= 2; ++i) {
            const double wx = DoviCubicWeight(static_cast<double>(i) - fracX);
            const double weight = wx * wy;
            sum += static_cast<double>(ReadP010UvCode(planes, baseX + i, baseY + j, component)) * weight;
            weightSum += weight;
        }
    }
    return weightSum > 0.0 ? sum / weightSum : 0.0;
}

uint16_t ClampRoundCode(const double value, const int maxCode) {
    const double clamped = std::clamp(value, 0.0, static_cast<double>(maxCode));
    return static_cast<uint16_t>(std::floor(clamped + 0.5));
}

uint16_t SampleFelYCodeReference(const NativeYuvPlanes& planes,
                                 const DolbyVisionFrameMetadata& metadata,
                                 const double uvX,
                                 const double uvY) {
    const int maxCode = DoviBitDepthMax(planes.bitDepth);
    const double sample = metadata.elSpatialResampling
                              ? SampleP010YCodeCubic(planes, uvX, uvY)
                              : SampleP010YCodeLinear(planes, uvX, uvY);
    return ClampRoundCode(sample, maxCode);
}

uint16_t SampleFelUvCodeReference(const NativeYuvPlanes& planes,
                                  const DolbyVisionFrameMetadata& metadata,
                                  const double uvX,
                                  const double uvY,
                                  const int component) {
    const int maxCode = DoviBitDepthMax(planes.bitDepth);
    const double sample = metadata.elSpatialResampling
                              ? SampleP010UvCodeCubic(planes, uvX, uvY, component)
                              : SampleP010UvCodeLinear(planes, uvX, uvY, component);
    return ClampRoundCode(sample, maxCode);
}

double DoviChromaSiteLumaNormReference(const NativeYuvPlanes& planes, const int x, const int y) {
    constexpr int weights[4] = {1, 3, 3, 1};
    double sum = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            sum += P010YCodeNorm(planes, x + i - 1, y + j - 1) *
                   static_cast<double>(weights[i] * weights[j]);
        }
    }
    return std::clamp(sum / 64.0, 0.0, 1.0);
}

int64_t DoviRoundSigned(const long double value) {
    if (value < 0.0L) {
        return -static_cast<int64_t>(std::floor(-value + 0.5L));
    }
    return static_cast<int64_t>(std::floor(value + 0.5L));
}

uint16_t ClampCode16(const int64_t value) {
    return static_cast<uint16_t>(std::clamp<int64_t>(value, 0, 65535));
}

int DoviFindPieceReference(const DoviReshapingCurve& curve, const double value) {
    const int numPivots = std::clamp(curve.numPivots, 2, kDoviMaxPivots);
    const int lastPiece = std::max(0, numPivots - 2);
    for (int piece = 0; piece < lastPiece; ++piece) {
        if (value < curve.pivots[piece + 1]) {
            return piece;
        }
    }
    return lastPiece;
}

double DoviMmrReference(const DoviReshapingPiece& piece, const double signal[3]) {
    double m[24] = {};
    m[0] = piece.mmrConstant;
    int index = 1;
    const int order = std::clamp(piece.mmrOrder, 1, kDoviMmrMaxTerms);
    for (int term = 0; term < order; ++term) {
        for (int coefficient = 0; coefficient < kDoviMmrCoeffsPerOrder; ++coefficient) {
            if (index < 24) {
                m[index] = piece.mmrCoef[term][coefficient];
            }
            ++index;
        }
    }

    const double x = signal[0];
    const double y = signal[1];
    const double z = signal[2];
    const double sigX[4] = {x * y, x * z, y * z, x * y * z};
    double result = m[0] + m[1] * x + m[2] * y + m[3] * z;
    result += m[4] * sigX[0] + m[5] * sigX[1] + m[6] * sigX[2] + m[7] * sigX[3];
    if (order >= 2) {
        const double x2 = x * x;
        const double y2 = y * y;
        const double z2 = z * z;
        const double sigX2[4] = {sigX[0] * sigX[0], sigX[1] * sigX[1], sigX[2] * sigX[2], sigX[3] * sigX[3]};
        result += m[8] * x2 + m[9] * y2 + m[10] * z2;
        result += m[11] * sigX2[0] + m[12] * sigX2[1] + m[13] * sigX2[2] + m[14] * sigX2[3];
        if (order >= 3) {
            result += m[15] * x2 * x + m[16] * y2 * y + m[17] * z2 * z;
            result += m[18] * sigX2[0] * sigX[0] +
                      m[19] * sigX2[1] * sigX[1] +
                      m[20] * sigX2[2] * sigX[2] +
                      m[21] * sigX2[3] * sigX[3];
        }
    }
    return std::isfinite(result) ? result : signal[0];
}

uint16_t DoviMapComponentCode16Reference(const DolbyVisionFrameMetadata& metadata,
                                         const int component,
                                         const double signal[3]) {
    const DoviReshapingCurve& curve = metadata.curves[component];
    const int pieceIndex = DoviFindPieceReference(curve, signal[component]);
    const DoviReshapingPiece& piece = curve.pieces[pieceIndex];
    if (piece.method == DoviMappingMethod::Polynomial) {
        const int blBits = std::clamp(metadata.blBitDepth, 8, 16);
        const double sCode = std::clamp(signal[component], 0.0, 1.0) *
                             static_cast<double>(DoviBitDepthMax(blBits));
        const double v16 = static_cast<double>(piece.polyCoef[0]) * static_cast<double>(1u << 20) +
                           static_cast<double>(piece.polyCoef[1]) * sCode * std::ldexp(1.0, 20 - blBits) +
                           static_cast<double>(piece.polyCoef[2]) * sCode * sCode * std::ldexp(1.0, 20 - 2 * blBits);
        return ClampCode16(static_cast<int64_t>(std::floor(std::clamp(v16 / 16.0, 0.0, 65535.0) + 0.5)));
    }
    if (piece.method == DoviMappingMethod::Mmr) {
        return ClampCode16(static_cast<int64_t>(std::floor(std::clamp(DoviMmrReference(piece, signal), 0.0, 1.0) *
                                                           65535.0 + 0.5)));
    }
    return ClampCode16(static_cast<int64_t>(std::floor(std::clamp(signal[component], 0.0, 1.0) * 65535.0 + 0.5)));
}

int64_t DoviInverseNlqResidualCodeReference(const DolbyVisionFrameMetadata& metadata,
                                            const int component,
                                            const uint16_t enhancementCode) {
    if (metadata.residualDisabled || metadata.nlqMethod != DoviNlqMethod::LinearDeadzone) {
        return 0;
    }
    const long double rr = static_cast<long double>(enhancementCode) -
                           static_cast<long double>(metadata.nlqOffset[component]);
    if (std::abs(rr) < 0.5L) {
        return 0;
    }
    const long double sign = rr < 0.0L ? -1.0L : 1.0L;
    const int elBits = std::clamp(metadata.elBitDepth, 8, 16);
    const long double bitScale = std::ldexp(1.0L, 10 - elBits);
    const long double rrLinear = sign * std::max(std::abs(rr) * 2.0L - 1.0L, 0.0L) * bitScale;
    long double dq = rrLinear * static_cast<long double>(metadata.nlqLinearDeadzoneSlope[component]) +
                     static_cast<long double>(metadata.nlqLinearDeadzoneThreshold[component]) *
                         std::ldexp(1.0L, 10 - elBits + 1) * sign;
    const long double limit = std::max(static_cast<long double>(metadata.nlqVdrInMax[component]) *
                                           std::ldexp(1.0L, 10 - elBits + 1),
                                       0.0L);
    dq = std::clamp(dq, -limit, limit);
    const int exponent = std::max(0, metadata.coefLog2Denom) - 5 - elBits;
    const long double residual16 = dq / std::ldexp(1.0L, exponent);
    return DoviRoundSigned(residual16);
}

uint16_t DoviVdrOutputCodeReference(const int64_t code16, const int vdrBitDepth) {
    const int bits = std::clamp(vdrBitDepth, 8, 16);
    const int maxCode = DoviBitDepthMax(bits);
    const long double quantStep = std::ldexp(1.0L, 16 - bits);
    const long double clamped = static_cast<long double>(std::clamp<int64_t>(code16, 0, 65535));
    const int64_t code = DoviRoundSigned(clamped / quantStep);
    return static_cast<uint16_t>(std::clamp<int64_t>(code, 0, maxCode));
}

struct DoviCpuReferenceStats {
    int gridWidth = 0;
    int gridHeight = 0;
    int vdrBitDepth = 0;
    uint64_t sampleCount = 0;
    uint64_t hash = 14695981039346656037ull;
    uint16_t minCode[3] = {
        std::numeric_limits<uint16_t>::max(),
        std::numeric_limits<uint16_t>::max(),
        std::numeric_limits<uint16_t>::max(),
    };
    uint16_t maxCode[3] = {};
    uint64_t sumCode[3] = {};
};

void AccumulateDoviReferenceStats(DoviCpuReferenceStats& stats,
                                  const uint16_t y,
                                  const uint16_t cb,
                                  const uint16_t cr) {
    const uint16_t values[3] = {y, cb, cr};
    for (int component = 0; component < 3; ++component) {
        stats.minCode[component] = std::min(stats.minCode[component], values[component]);
        stats.maxCode[component] = std::max(stats.maxCode[component], values[component]);
        stats.sumCode[component] += values[component];
        stats.hash = HashValue(stats.hash, values[component]);
    }
    ++stats.sampleCount;
}

bool BuildDolbyVisionCpuReferenceSample(const NativeVideoFrame& frame, DoviCpuReferenceStats& stats) {
    if (!frame.HasYuv() ||
        !frame.HasEnhancementYuv() ||
        !frame.enhancementDovi ||
        !frame.enhancementDovi->valid) {
        return false;
    }
    const DolbyVisionFrameMetadata& metadata = *frame.enhancementDovi;
    if (!DoviSingleNlqPartition(metadata) ||
        frame.yuv.width <= 0 ||
        frame.yuv.height <= 0 ||
        frame.enhancementYuv.width <= 0 ||
        frame.enhancementYuv.height <= 0) {
        return false;
    }

    constexpr int kReferenceGridWidth = 64;
    constexpr int kReferenceGridHeight = 36;
    stats.gridWidth = std::min(kReferenceGridWidth, frame.yuv.width);
    stats.gridHeight = std::min(kReferenceGridHeight, frame.yuv.height);
    stats.vdrBitDepth = std::clamp(metadata.vdrBitDepth, 8, 16);
    if (stats.gridWidth <= 0 || stats.gridHeight <= 0) {
        return false;
    }

    for (int gy = 0; gy < stats.gridHeight; ++gy) {
        const int y = stats.gridHeight == 1
                          ? frame.yuv.height / 2
                          : static_cast<int>((static_cast<int64_t>(gy) * (frame.yuv.height - 1)) / (stats.gridHeight - 1));
        for (int gx = 0; gx < stats.gridWidth; ++gx) {
            const int x = stats.gridWidth == 1
                              ? frame.yuv.width / 2
                              : static_cast<int>((static_cast<int64_t>(gx) * (frame.yuv.width - 1)) / (stats.gridWidth - 1));
            const double uvX = (static_cast<double>(x) + 0.5) / static_cast<double>(frame.yuv.width);
            const double uvY = (static_cast<double>(y) + 0.5) / static_cast<double>(frame.yuv.height);
            const int chromaX = x / 2;
            const int chromaY = y / 2;

            const double blPixel[3] = {
                P010YCodeNorm(frame.yuv, x, y),
                P010UvCodeNorm(frame.yuv, chromaX, chromaY, 1),
                P010UvCodeNorm(frame.yuv, chromaX, chromaY, 2),
            };
            const double blChroma[3] = {
                DoviChromaSiteLumaNormReference(frame.yuv, x, y),
                blPixel[1],
                blPixel[2],
            };

            const uint16_t mappedY = DoviMapComponentCode16Reference(metadata, 0, blPixel);
            const uint16_t mappedCb = DoviMapComponentCode16Reference(metadata, 1, blChroma);
            const uint16_t mappedCr = DoviMapComponentCode16Reference(metadata, 2, blChroma);
            const uint16_t elY = SampleFelYCodeReference(frame.enhancementYuv, metadata, uvX, uvY);
            const uint16_t elCb = SampleFelUvCodeReference(frame.enhancementYuv, metadata, uvX, uvY, 1);
            const uint16_t elCr = SampleFelUvCodeReference(frame.enhancementYuv, metadata, uvX, uvY, 2);

            const int64_t mergedY = static_cast<int64_t>(mappedY) + DoviInverseNlqResidualCodeReference(metadata, 0, elY);
            const int64_t mergedCb = static_cast<int64_t>(mappedCb) + DoviInverseNlqResidualCodeReference(metadata, 1, elCb);
            const int64_t mergedCr = static_cast<int64_t>(mappedCr) + DoviInverseNlqResidualCodeReference(metadata, 2, elCr);
            AccumulateDoviReferenceStats(stats,
                                         DoviVdrOutputCodeReference(mergedY, metadata.vdrBitDepth),
                                         DoviVdrOutputCodeReference(mergedCb, metadata.vdrBitDepth),
                                         DoviVdrOutputCodeReference(mergedCr, metadata.vdrBitDepth));
        }
    }
    return stats.sampleCount > 0;
}


std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool IsNetworkMediaPath(const std::filesystem::path& path) {
    const auto value = ToLowerWide(path.wstring());
    return value.rfind(L"http://", 0) == 0 ||
           value.rfind(L"https://", 0) == 0;
}

std::wstring StreamLanguage(const AVStream* stream) {
    if (!stream) {
        return {};
    }
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
    return language ? Utf8ToWide(language->value) : L"";
}

std::wstring PacketStreamTypeName(const AVFormatContext* formatCtx, const int streamIndex) {
    if (!formatCtx || streamIndex < 0 || streamIndex >= static_cast<int>(formatCtx->nb_streams)) {
        return L"unknown";
    }
    const AVStream* stream = formatCtx->streams[streamIndex];
    if (!stream || !stream->codecpar) {
        return L"unknown";
    }
    const char* name = av_get_media_type_string(stream->codecpar->codec_type);
    if (name && *name) {
        return Utf8ToWide(name);
    }
    return L"unknown(" + std::to_wstring(static_cast<int>(stream->codecpar->codec_type)) + L")";
}

std::wstring FormatIoState(const AVFormatContext* formatCtx) {
    if (!formatCtx || !formatCtx->pb) {
        return L"io=none";
    }
    AVIOContext* io = formatCtx->pb;
    std::wstring state = L"pos=" + std::to_wstring(io->pos) +
                         L" eof=" + std::to_wstring(io->eof_reached) +
                         L" seekable=" + std::to_wstring(io->seekable);
    if (io->error != 0) {
        state += L" io_error=" + FfmpegErrorString(io->error);
    }
    return state;
}

bool ClearNetworkIoState(AVFormatContext* formatCtx) {
    if (!formatCtx || !formatCtx->pb) {
        return false;
    }
    AVIOContext* io = formatCtx->pb;
    const bool changed = io->eof_reached != 0 || io->error != 0;
    io->eof_reached = 0;
    io->error = 0;
    return changed;
}


constexpr std::chrono::milliseconds kSubtitleCueRetention{30000};
constexpr std::chrono::milliseconds kDefaultBitmapSubtitleDuration{5000};
constexpr std::chrono::milliseconds kMaxBitmapSubtitleDuration{30000};


std::optional<std::chrono::milliseconds> PacketDuration(const AVPacket* packet, const AVRational streamTimeBase) {
    if (!packet || packet->duration <= 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{
        av_rescale_q(packet->duration, streamTimeBase, AVRational{1, 1000})};
}

std::optional<std::chrono::milliseconds> PacketTimestamp(const AVPacket* packet, const AVRational streamTimeBase) {
    if (!packet) {
        return std::nullopt;
    }
    const int64_t ticks = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (ticks == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{
        av_rescale_q(ticks, streamTimeBase, AVRational{1, 1000})};
}

std::chrono::milliseconds EstimatedVideoPacketDuration(const AVStream* stream) {
    if (stream) {
        const AVRational rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
                                    ? stream->avg_frame_rate
                                    : stream->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) {
            const auto duration = std::chrono::milliseconds{
                av_rescale_q(1, AVRational{rate.den, rate.num}, AVRational{1, 1000})};
            if (duration > std::chrono::milliseconds{0} && duration < std::chrono::milliseconds{1000}) {
                return duration;
            }
        }
    }
    return std::chrono::milliseconds{40};
}

std::size_t PacketCacheCostBytes(const AVPacket* packet) {
    if (!packet) {
        return 0;
    }
    std::size_t bytes = sizeof(AVPacket) + 256;
    std::size_t payloadBytes = 0;
    if (packet->size > 0) {
        payloadBytes = static_cast<std::size_t>(packet->size);
    }
    if (packet->buf) {
        payloadBytes = std::max(payloadBytes, packet->buf->size);
    }
    bytes = SaturatingAddBytes(bytes, payloadBytes);
    for (int index = 0; index < packet->side_data_elems; ++index) {
        bytes = SaturatingAddBytes(bytes, sizeof(AVPacketSideData));
        if (packet->side_data[index].size > 0) {
            bytes = SaturatingAddBytes(bytes, static_cast<std::size_t>(packet->side_data[index].size));
        }
    }
    return bytes;
}

std::size_t PacketReadAheadBudgetBytes(const std::size_t minBudget, const std::size_t maxBudget) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        const auto adaptive = static_cast<std::uint64_t>(memory.ullAvailPhys / 6);
        const auto clamped = std::clamp(adaptive,
                                        static_cast<std::uint64_t>(minBudget),
                                        static_cast<std::uint64_t>(maxBudget));
        return static_cast<std::size_t>(clamped);
    }
    return maxBudget;
}

struct CachedVideoPacket {
    AVPacket* packet = nullptr;
    std::chrono::milliseconds pts{0};
    std::chrono::milliseconds end{0};
    std::size_t bytes = 0;
    uint64_t timelineSerial = 0;

    CachedVideoPacket() = default;
    CachedVideoPacket(const CachedVideoPacket&) = delete;
    CachedVideoPacket& operator=(const CachedVideoPacket&) = delete;

    CachedVideoPacket(CachedVideoPacket&& other) noexcept
        : packet(std::exchange(other.packet, nullptr)),
          pts(other.pts),
          end(other.end),
          bytes(std::exchange(other.bytes, 0)),
          timelineSerial(std::exchange(other.timelineSerial, 0)) {}

    CachedVideoPacket& operator=(CachedVideoPacket&& other) noexcept {
        if (this != &other) {
            Reset();
            packet = std::exchange(other.packet, nullptr);
            pts = other.pts;
            end = other.end;
            bytes = std::exchange(other.bytes, 0);
            timelineSerial = std::exchange(other.timelineSerial, 0);
        }
        return *this;
    }

    ~CachedVideoPacket() {
        Reset();
    }

    void Reset() {
        if (packet) {
            av_packet_free(&packet);
        }
        bytes = 0;
    }

    static std::optional<CachedVideoPacket> MoveFrom(AVPacket* source,
                                                     const AVRational timeBase,
                                                     const std::chrono::milliseconds fallbackStart,
                                                     const std::chrono::milliseconds fallbackDuration,
                                                     const uint64_t timelineSerial) {
        if (!source) {
            return std::nullopt;
        }

        AVPacket* owned = av_packet_alloc();
        if (!owned) {
            return std::nullopt;
        }

        CachedVideoPacket cached;
        cached.bytes = PacketCacheCostBytes(source);
        auto start = PacketTimestamp(source, timeBase).value_or(fallbackStart);
        if (start < fallbackStart) {
            start = fallbackStart;
        }
        auto duration = PacketDuration(source, timeBase).value_or(fallbackDuration);
        if (duration <= std::chrono::milliseconds{0}) {
            duration = fallbackDuration;
        }
        cached.pts = start;
        cached.end = std::max(start + duration, start + std::chrono::milliseconds{1});
        cached.timelineSerial = timelineSerial;
        av_packet_move_ref(owned, source);
        cached.packet = owned;
        return cached;
    }
};

const AVDOVIDecoderConfigurationRecord* DolbyVisionConfig(const AVCodecParameters* parameters) {
    if (!parameters) {
        return nullptr;
    }
    for (int index = 0; index < parameters->nb_coded_side_data; ++index) {
        const AVPacketSideData& sideData = parameters->coded_side_data[index];
        if (sideData.type == AV_PKT_DATA_DOVI_CONF &&
            sideData.size >= static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
            return reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(sideData.data);
        }
    }
    return nullptr;
}

bool IsDolbyVisionEnhancementLayer(const AVCodecParameters* parameters) {
    const auto* dovi = DolbyVisionConfig(parameters);
    return dovi &&
           dovi->el_present_flag != 0 &&
           dovi->bl_present_flag == 0;
}

std::wstring DoviNlqMethodName(const AVDOVINLQMethod method) {
    switch (method) {
    case AV_DOVI_NLQ_NONE:
        return L"none";
    case AV_DOVI_NLQ_LINEAR_DZ:
        return L"linear_dz";
    default:
        return L"unknown(" + std::to_wstring(static_cast<int>(method)) + L")";
    }
}

std::wstring DoviResidualSummary(const AVFrame* frame) {
    const AVFrameSideData* sideData = frame ? av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA) : nullptr;
    if (!sideData || sideData->size < sizeof(AVDOVIMetadata)) {
        return L" residual=unknown";
    }

    const auto* dovi = reinterpret_cast<const AVDOVIMetadata*>(sideData->data);
    const AVDOVIRpuDataHeader* header = av_dovi_get_header(dovi);
    const AVDOVIDataMapping* mapping = av_dovi_get_mapping(dovi);
    if (!header || !mapping) {
        return L" residual=unknown";
    }

    std::wostringstream stream;
    stream << L" residual=" << (header->disable_residual_flag ? L"disabled" : L"enabled")
           << L" el_spatial=" << static_cast<int>(header->el_spatial_resampling_filter_flag)
           << L" nlq=" << DoviNlqMethodName(mapping->nlq_method_idc)
           << L" partitions=" << mapping->num_x_partitions << L"x" << mapping->num_y_partitions;
    if (mapping->nlq_method_idc != AV_DOVI_NLQ_NONE) {
        stream << L" nlq0(offset=" << mapping->nlq[0].nlq_offset
               << L" vdr_max=" << mapping->nlq[0].vdr_in_max
               << L" slope=" << mapping->nlq[0].linear_deadzone_slope
               << L" threshold=" << mapping->nlq[0].linear_deadzone_threshold << L")";
    }
    return stream.str();
}

std::wstring VideoStreamDescription(const AVStream* stream) {
    if (!stream || !stream->codecpar) {
        return L"unavailable";
    }
    const AVCodecParameters* parameters = stream->codecpar;
    std::wstring details =
        L"stream=" + std::to_wstring(stream->index >= 0 ? stream->index : -1) +
        L" codec=" + Utf8ToWide(avcodec_get_name(parameters->codec_id)) +
        L" size=" + std::to_wstring(parameters->width) + L"x" + std::to_wstring(parameters->height);
    if (const auto* dovi = DolbyVisionConfig(parameters)) {
        details +=
            L" dv_profile=" + std::to_wstring(dovi->dv_profile) +
            L" el=" + std::to_wstring(dovi->el_present_flag) +
            L" bl=" + std::to_wstring(dovi->bl_present_flag);
    }
    return details;
}

}  // namespace

FfmpegVideoDecoder::FfmpegVideoDecoder(LogSinkPtr logSink) : logSink_(std::move(logSink)) {}

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    Stop();
}

bool FfmpegVideoDecoder::Start(const std::filesystem::path& mediaPath,
                               const std::chrono::milliseconds startPosition,
                               HWND notificationWindow,
                               const UINT notificationMessage,
                               const UINT failureMessage,
                               ClockCallback clockCallback,
                               const bool preferHardwareDecode,
                               ID3D11Device* sharedD3DDevice,
                               const int selectedVideoTrackIndex,
                               std::wstring preferredSubtitleLanguage,
                               const int selectedSubtitleTrackIndex,
                               const std::chrono::milliseconds subtitleDelay,
                               const bool autoLoadExternalSubtitles,
                               std::filesystem::path externalSubtitlePath,
                               const bool oneShotFrame,
                               const bool preferDolbyVisionHdrOutput,
                               const bool enableDolbyVisionEnhancementDecode,
                               NativeAudioPacketSink audioPacketSink,
                               const uint64_t notificationCookie) {
    Stop();
    if (mediaPath.empty()) {
        return false;
    }

    path_ = mediaPath;
    startPosition_ = startPosition;
    preferHardwareDecode_ = preferHardwareDecode;
    sharedD3DDevice_.Reset();
    if (sharedD3DDevice) {
        sharedD3DDevice_ = sharedD3DDevice;
    }
    hardwarePixelFormat_ = AV_PIX_FMT_NONE;
    hardwareDecodeActive_ = false;
    hardwareFormatLogged_ = false;
    zeroCopyFallbackLogged_ = false;
    firstDecodedFrameLogged_ = false;
    firstHardwareFrameLogged_ = false;
    firstCpuTransferFrameLogged_ = false;
    receiveEagainLogCount_ = 0;
    receiveEofLogged_ = false;
    sendPacketFailureLogged_ = false;
    sendPacketBackpressureLogged_ = false;
    bitmapSubtitleLogged_ = false;
    frameSubtitleBitmapLogged_ = false;
    dolbyVisionLibplaceboFailed_ = false;
    dolbyVisionLibplaceboFrameLogged_ = false;
    dolbyVisionDynamicMetadataLogged_ = false;
    dolbyVisionLastDynamicMetadataFingerprint_ = 0;
    doviLibplaceboFilter_.reset();
    streamColorMetadata_ = {};
    preferredSubtitleLanguage_ = std::move(preferredSubtitleLanguage);
    selectedVideoTrackIndex_ = selectedVideoTrackIndex;
    selectedSubtitleTrackIndex_ = selectedSubtitleTrackIndex;
    audioPacketSink_ = std::move(audioPacketSink);
    subtitleDelay_ = subtitleDelay;
    autoLoadExternalSubtitles_ = autoLoadExternalSubtitles;
    externalSubtitlePath_ = std::move(externalSubtitlePath);
    oneShotFrame_ = oneShotFrame;
    preferDolbyVisionHdrOutput_ = preferDolbyVisionHdrOutput;
    enableDolbyVisionEnhancementDecode_ = enableDolbyVisionEnhancementDecode;
    dolbyVisionEnhancementActive_ = false;
    dolbyVisionEnhancementFirstFrameLogged_ = false;
    dolbyVisionEnhancementDynamicMetadataLogged_ = false;
    dolbyVisionEnhancementFailureLogged_ = false;
    dolbyVisionEnhancementOverlayLogged_ = false;
    dolbyVisionEnhancementNoMatchLogged_ = false;
    dolbyVisionEnhancementFirstPackedLogged_ = false;
    dolbyVisionCpuReferenceLogged_ = false;
    dolbyVisionMultiPartitionFallbackLogged_ = false;
    dolbyVisionEnhancementStartupFallbackLogged_ = false;
    dolbyVisionEnhancementBaseOnlyDropLogged_ = false;
    dolbyVisionEnhancementStartupMisses_ = 0;
    dolbyVisionEnhancementLastDynamicMetadataFingerprint_ = 0;
    dolbyVisionEnhancementFramesDecoded_ = 0;
    dolbyVisionEnhancementStreamIndex_ = -1;
    dolbyVisionEnhancementProfile_ = 0;
    dolbyVisionEnhancementLevel_ = 0;
    dolbyVisionEnhancementCompatId_ = 0;
    dolbyVisionEnhancementElPresent_ = false;
    dolbyVisionEnhancementBlPresent_ = false;
    latestDolbyVisionEnhancementMetadata_.reset();
    latestDolbyVisionEnhancementMetadataPts_ = std::chrono::milliseconds{0};
    dolbyVisionEnhancementFrames_.clear();
    pendingDolbyVisionBaseFrame_.reset();
    subtitleCanvasWidth_ = 0;
    subtitleCanvasHeight_ = 0;
    subtitleCanvasLogged_ = false;
    subtitleBitmapSerial_ = 0;
    subtitleAssActive_ = false;
    subtitleAssExternalFullTrack_ = false;
    subtitleAssLogged_ = false;
    subtitleAssRenderer_.reset();
    externalSubtitlesActive_ = false;
    subtitleCues_.clear();
    externalSubtitleCues_.clear();
    {
        std::scoped_lock lock(clockCallbackMutex_);
        clockCallback_ = std::move(clockCallback);
    }
    fallbackClockAnchor_.reset();
    fallbackClockBasePts_ = std::chrono::milliseconds{0};
    notificationWindow_.store(notificationWindow);
    notificationMessage_.store(notificationMessage);
    failureMessage_.store(failureMessage);
    notificationCookie_.store(notificationCookie);
    frameMessagePending_.store(false);
    pendingSeekMs_.store(-1);
    pendingSeekInterruptsEnabled_.store(false);
    ioInterruptAfterSteadyMs_.store(0);
    activeTimelineSerial_.store(1);
    interruptReturnCount_.store(0);
    hdr10PlusDetected_.store(false);
    playbackPaused_.store(false);
    pausedPositionMs_.store(startPosition.count());
    seekFastResumeFramesRemaining_.store(0);
    seekFastResumeLogged_.store(false);
    seekPrerollPending_.store(false);
    seekRecoveryActiveSnapshot_.store(false);
    seekAudioHandoffReadySnapshot_.store(true);
    seekRecoveryTargetMs_.store(startPosition.count() > 0 ? startPosition.count() : -1);
    seekRecoveryDropLogged_.store(false);
    pendingClockResetMs_.store(-1);
    stopping_.store(false);
    schedulePrimed_ = false;
    {
        std::scoped_lock lock(mutex_);
        latestFrame_.bgra.reset();
        latestFrame_.yuv = {};
        latestFrame_.serial = 0;
        latestFrame_.timelineSerial = 0;
        latestFrame_.width = latestFrame_.height = latestFrame_.stride = 0;
        latestFrame_.d3dTexture.Reset();
        latestFrame_.hardwareFrameRef.reset();
        latestFrame_.d3dArraySlice = 0;
        latestFrame_.d3dFormat = DXGI_FORMAT_UNKNOWN;
        latestFrame_.softwareFormat = AV_PIX_FMT_NONE;
        latestFrame_.color = {};
        latestFrame_.dynamicMetadataPath.clear();
        latestFrame_.dynamicMetadataDetails.clear();
        latestFrame_.subtitleText.clear();
        latestFrame_.subtitleBitmaps.clear();
        latestFrame_.subtitlesPrepared = false;
        frameQueue_.clear();
        stats_ = {};
        stats_.decoder = preferHardwareDecode_ ? L"ffmpeg_d3d11va_pending" : L"ffmpeg_software";
        ResetSeekRecoveryLocked();
    }
    running_.store(true);
    {
        std::scoped_lock lock(schedulerMutex_);
        schedulerWakeRequested_ = true;
    }
    try {
        schedulerThread_ = std::thread([this]() { SchedulerLoop(); });
        decodeThread_ = std::thread([this]() { DecodeLoop(); });
    } catch (const std::system_error& error) {
        Stop();
        LogThread(LogLevel::Error,
                  L"decoder",
                  L"worker_start_failed reason=" + Utf8ToWide(error.what()));
        return false;
    }
    return true;
}

void FfmpegVideoDecoder::RequestStop() {
    stopping_.store(true);
    pendingSeekMs_.store(-1);
    pendingSeekInterruptsEnabled_.store(false);
    ioInterruptAfterSteadyMs_.store(0);
    activeTimelineSerial_.store(0);
    playbackPaused_.store(false);
    notificationWindow_.store(nullptr);
    notificationMessage_.store(0);
    failureMessage_.store(0);
    frameMessagePending_.store(false);
    seekFastResumeFramesRemaining_.store(0);
    seekFastResumeLogged_.store(false);
    seekPrerollPending_.store(false);
    seekRecoveryActiveSnapshot_.store(false);
    seekAudioHandoffReadySnapshot_.store(true);
    seekRecoveryTargetMs_.store(-1);
    seekRecoveryDropLogged_.store(false);
    pendingClockResetMs_.store(-1);
    WakeScheduler();
    frameQueueCv_.notify_all();
}

void FfmpegVideoDecoder::Stop() {
    RequestStop();
    if (schedulerThread_.joinable()) {
        schedulerThread_.join();
    }
    if (decodeThread_.joinable()) {
        decodeThread_.join();
    }
    running_.store(false);
    {
        std::scoped_lock lock(mutex_);
        ResetSeekRecoveryLocked();
    }
    {
        std::scoped_lock lock(clockCallbackMutex_);
        clockCallback_ = {};
    }

    // Everything below is session-owned and may retain tens of MiB. Stop() is
    // invoked by MainWindow's background stop worker, so retire it here rather
    // than making the next window-thread Start() pay for the previous session.
    std::deque<DolbyVisionEnhancementFrame> retiredEnhancementFrames;
    std::optional<NativeVideoFrame> retiredPendingDolbyVisionBaseFrame;
    std::deque<NativeSubtitleCue> retiredSubtitleCues;
    std::deque<NativeSubtitleCue> retiredExternalSubtitleCues;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> retiredReusableBuffers;
    retiredEnhancementFrames.swap(dolbyVisionEnhancementFrames_);
    retiredPendingDolbyVisionBaseFrame.swap(pendingDolbyVisionBaseFrame_);
    retiredSubtitleCues.swap(subtitleCues_);
    retiredExternalSubtitleCues.swap(externalSubtitleCues_);
    {
        std::scoped_lock lock(mutex_);
        retiredReusableBuffers.swap(reusableBgraBuffers_);
    }
    latestDolbyVisionEnhancementMetadata_.reset();
    latestDolbyVisionEnhancementMetadataPts_ = std::chrono::milliseconds{0};
    doviLibplaceboFilter_.reset();
    sharedD3DDevice_.Reset();
    subtitleAssRenderer_.reset();
    subtitleAssActive_ = false;
    subtitleAssExternalFullTrack_ = false;
}

bool FfmpegVideoDecoder::Seek(const std::chrono::milliseconds position) {
    if (!running_.load() || oneShotFrame_) {
        return false;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    const bool prerollAfterSeek = !playbackPaused_.load();
    pendingSeekMs_.store(clamped.count());
    // Publish the request before invalidating the old timeline.  Producers
    // stop enqueueing first; once the serial advances, only background
    // scheduler/decode workers can observe and retire the stale frames.  The
    // caller (normally the window thread) updates scalar control state only.
    const uint64_t seekTimelineSerial = AdvanceTimelineSerial();
    frameMessagePending_.store(false);
    seekRecoveryTargetMs_.store(clamped.count());
    seekRecoveryDropLogged_.store(false);
    seekPrerollPending_.store(prerollAfterSeek);
    seekRecoveryActiveSnapshot_.store(prerollAfterSeek);
    seekAudioHandoffReadySnapshot_.store(!prerollAfterSeek);
    if (prerollAfterSeek) {
        seekFastResumeFramesRemaining_.store(kSeekFastResumeFrameCount);
        seekFastResumeLogged_.store(false);
    }
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            WakeScheduler();
            frameQueueCv_.notify_all();
            return true;
        }
        schedulePrimed_ = false;
        fallbackClockAnchor_.reset();
        fallbackClockBasePts_ = clamped;
        const auto decoder = stats_.decoder;
        const auto fallbackReason = stats_.fallbackReason;
        const bool usingHardware = stats_.usingHardwareDecode;
        const uint64_t networkBytesPerSecond = stats_.networkBytesPerSecond;
        stats_ = {};
        stats_.decoder = decoder;
        stats_.fallbackReason = fallbackReason;
        stats_.usingHardwareDecode = usingHardware;
        stats_.networkBytesPerSecond = networkBytesPerSecond;
        stats_.clockPosition = clamped;
        BeginSeekRecoveryLocked(clamped, prerollAfterSeek, seekTimelineSerial);
        UpdateBufferedStatsLocked();
    }
    WakeScheduler();
    frameQueueCv_.notify_all();
    return true;
}

void FfmpegVideoDecoder::SetPaused(const bool paused, const std::chrono::milliseconds position) {
    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pausedPositionMs_.store(clamped.count());
    playbackPaused_.store(paused);
    pendingClockResetMs_.store(clamped.count());
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            WakeScheduler();
            frameQueueCv_.notify_all();
            return;
        }
        ApplyPendingClockResetLocked();
        UpdateBufferedStatsLocked();
    }
    WakeScheduler();
    frameQueueCv_.notify_all();
}

int FfmpegVideoDecoder::InterruptCallback(void* opaque) {
    const auto* decoder = static_cast<const FfmpegVideoDecoder*>(opaque);
    bool interrupted = false;
    if (decoder) {
        interrupted = decoder->stopping_.load() ||
                      (decoder->pendingSeekInterruptsEnabled_.load() && decoder->HasPendingSeek());
        const int64_t interruptAfterMs = decoder->ioInterruptAfterSteadyMs_.load();
        if (!interrupted && interruptAfterMs > 0 && SteadyClockMs() >= interruptAfterMs) {
            interrupted = true;
        }
    }
    if (interrupted) {
        decoder->interruptReturnCount_.fetch_add(1);
    }
    return interrupted ? 1 : 0;
}

bool FfmpegVideoDecoder::LatestFrame(NativeVideoFrame& frame) const {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !latestFrame_.HasContent()) {
        return false;
    }
    const uint64_t activeSerial = CurrentTimelineSerial();
    if (latestFrame_.timelineSerial != 0 && latestFrame_.timelineSerial != activeSerial) {
        return false;
    }
    frame = latestFrame_;
    return true;
}

std::wstring FfmpegVideoDecoder::LatestDynamicMetadata() const {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !latestFrame_.HasContent()) {
        return {};
    }
    const uint64_t activeSerial = CurrentTimelineSerial();
    if (latestFrame_.timelineSerial != 0 && latestFrame_.timelineSerial != activeSerial) {
        return {};
    }

    std::wstring metadata = latestFrame_.dynamicMetadataPath;
    if (metadata.empty() && latestFrame_.dovi && latestFrame_.dovi->valid) {
        metadata = L"dolby_vision_shader";
    }
    if (!metadata.empty() && !latestFrame_.dynamicMetadataDetails.empty()) {
        metadata += L" " + latestFrame_.dynamicMetadataDetails;
    }
    return metadata;
}

void FfmpegVideoDecoder::ClearFrame() {
    NativeVideoFrame retiredLatestFrame;
    std::deque<NativeVideoFrame> retiredFrameQueue;
    {
        std::scoped_lock lock(mutex_);
        ResetSeekRecoveryLocked();
        retiredLatestFrame = std::move(latestFrame_);
        retiredFrameQueue.swap(frameQueue_);
        stats_.queueDepth = 0;
        stats_.packetQueueDepth = 0;
        stats_.packetQueueBytes = 0;
        stats_.readAheadEnd = std::chrono::milliseconds{0};
        stats_.readAheadDuration = std::chrono::milliseconds{0};
        UpdateBufferedStatsLocked();
    }
    frameQueueCv_.notify_all();
}

void FfmpegVideoDecoder::AcknowledgeFrameNotification() {
    frameMessagePending_.store(false);
}

uint64_t FfmpegVideoDecoder::CurrentTimelineSerial() const {
    return activeTimelineSerial_.load(std::memory_order_acquire);
}

uint64_t FfmpegVideoDecoder::AdvanceTimelineSerial() {
    return activeTimelineSerial_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void FfmpegVideoDecoder::BeginSeekRecoveryLocked(const std::chrono::milliseconds target,
                                                 const bool prerollAfterSeek,
                                                 const uint64_t timelineSerial) {
    seekRecovery_ = {};
    seekPrerollPending_.store(prerollAfterSeek);
    stats_.seekRecoveryActive = prerollAfterSeek;
    stats_.seekRecoveryAudioHandoffReady = !prerollAfterSeek;
    seekRecoveryActiveSnapshot_.store(prerollAfterSeek);
    seekAudioHandoffReadySnapshot_.store(!prerollAfterSeek);
    stats_.timelineSerial = timelineSerial;
    stats_.buffering = prerollAfterSeek;
    if (!prerollAfterSeek) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    seekRecovery_.phase = SeekRecoveryPhase::Preroll;
    seekRecovery_.timelineSerial = timelineSerial;
    seekRecovery_.target = target;
    seekRecovery_.startedAt = now;
    seekRecovery_.clockAnchorPts = target;
    seekRecovery_.clockAnchorTime = now;
}

void FfmpegVideoDecoder::ResetSeekRecoveryLocked() {
    seekRecovery_ = {};
    seekPrerollPending_.store(false);
    stats_.seekRecoveryActive = false;
    stats_.seekRecoveryAudioHandoffReady = true;
    seekRecoveryActiveSnapshot_.store(false);
    seekAudioHandoffReadySnapshot_.store(true);
    stats_.timelineSerial = CurrentTimelineSerial();
    stats_.buffering = false;
}

void FfmpegVideoDecoder::ApplyPendingClockResetLocked() {
    const int64_t resetMs = pendingClockResetMs_.exchange(-1);
    if (resetMs < 0) {
        return;
    }
    const std::chrono::milliseconds position{resetMs};
    fallbackClockAnchor_.reset();
    fallbackClockBasePts_ = position;
    stats_.clockPosition = position;
}

void FfmpegVideoDecoder::DropStaleFramesLocked() {
    const uint64_t activeSerial = CurrentTimelineSerial();
    if (latestFrame_.HasContent() &&
        latestFrame_.timelineSerial != 0 &&
        latestFrame_.timelineSerial != activeSerial) {
        latestFrame_ = {};
        ++stats_.droppedStale;
    }

    for (auto it = frameQueue_.begin(); it != frameQueue_.end();) {
        if (it->timelineSerial != 0 && it->timelineSerial != activeSerial) {
            it = frameQueue_.erase(it);
            ++stats_.droppedStale;
        } else {
            ++it;
        }
    }
    stats_.timelineSerial = activeSerial;
}

void FfmpegVideoDecoder::StartSeekRecoveryVisualWarmupLocked(const std::chrono::steady_clock::time_point now) {
    DropStaleFramesLocked();
    if (frameQueue_.empty()) {
        ResetSeekRecoveryLocked();
        return;
    }

    seekRecovery_.timelineSerial = CurrentTimelineSerial();
    seekRecovery_.phase = SeekRecoveryPhase::VisualWarmup;
    seekRecovery_.visualStartedAt = now;
    seekRecovery_.clockAnchorTime = now;
    seekRecovery_.clockAnchorPts = frameQueue_.front().pts;
    seekRecovery_.firstPublishedPts.reset();
    seekRecovery_.publishedFrames = 0;
    stats_.seekRecoveryActive = true;
    stats_.seekRecoveryAudioHandoffReady = false;
    seekRecoveryActiveSnapshot_.store(true);
    seekAudioHandoffReadySnapshot_.store(false);
    stats_.buffering = false;
    fallbackClockAnchor_.reset();
    fallbackClockBasePts_ = frameQueue_.front().pts;
}

FfmpegVideoDecoder::SchedulerClock FfmpegVideoDecoder::SeekRecoverySchedulerClockLocked(
    const std::chrono::milliseconds firstQueuedPts,
    const std::optional<std::chrono::milliseconds>& audioClock) {
    if (seekRecovery_.phase != SeekRecoveryPhase::VisualWarmup) {
        return CurrentSchedulerClockLocked(firstQueuedPts, audioClock);
    }
    if (seekRecovery_.timelineSerial != CurrentTimelineSerial()) {
        return CurrentSchedulerClockLocked(firstQueuedPts, audioClock);
    }

    const auto now = std::chrono::steady_clock::now();
    if (seekRecovery_.clockAnchorTime.time_since_epoch().count() <= 0) {
        seekRecovery_.clockAnchorTime = now;
        seekRecovery_.clockAnchorPts = firstQueuedPts;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - seekRecovery_.clockAnchorTime);
    const auto position = seekRecovery_.clockAnchorPts +
                          std::max(elapsed, std::chrono::milliseconds{0});
    return {position, false};
}

void FfmpegVideoDecoder::UpdateSeekRecoveryAfterPublishLocked(
    const std::chrono::milliseconds publishedPts,
    const std::chrono::steady_clock::time_point now) {
    if (seekRecovery_.phase != SeekRecoveryPhase::VisualWarmup) {
        return;
    }
    if (seekRecovery_.timelineSerial != CurrentTimelineSerial()) {
        ResetSeekRecoveryLocked();
        return;
    }

    if (!seekRecovery_.firstPublishedPts.has_value()) {
        seekRecovery_.firstPublishedPts = publishedPts;
    }
    ++seekRecovery_.publishedFrames;

    const auto publishedSpan = publishedPts - *seekRecovery_.firstPublishedPts;
    const auto elapsed = seekRecovery_.visualStartedAt.time_since_epoch().count() > 0
                             ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - seekRecovery_.visualStartedAt)
                             : std::chrono::milliseconds{0};
    const bool softwareFrame =
        latestFrame_.HasYuv() ||
        latestFrame_.HasPixels() ||
        (!frameQueue_.empty() && (frameQueue_.front().HasYuv() || frameQueue_.front().HasPixels()));
    const int warmupFrameCount = softwareFrame
                                     ? kSeekRecoverySoftwareWarmupFrameCount
                                     : kSeekRecoveryWarmupFrameCount;
    const auto warmupMinSpan = softwareFrame
                                   ? kSeekRecoverySoftwareWarmupMinSpan
                                   : kSeekRecoveryWarmupMinSpan;
    const auto handoffMinReadAhead = softwareFrame
                                         ? kSeekRecoverySoftwareHandoffMinReadAhead
                                         : kSeekRecoveryHandoffMinReadAhead;
    const auto warmupMaxWait = softwareFrame
                                   ? kSeekRecoverySoftwareWarmupMaxWait
                                   : kSeekRecoveryWarmupMaxWait;
    const bool hasPublishedContinuity =
        seekRecovery_.publishedFrames >= warmupFrameCount ||
        publishedSpan >= warmupMinSpan;
    std::size_t minQueuedLead = kSeekRecoveryHandoffMinQueuedFrames;
    if (!frameQueue_.empty()) {
        const auto capacity = MaxQueueDepthForFrame(frameQueue_.front());
        minQueuedLead = std::min(minQueuedLead, capacity > 1 ? capacity - 1 : capacity);
    }
    const bool hasQueuedLead = !frameQueue_.empty() && frameQueue_.size() >= minQueuedLead;
    const bool hasReadAheadLead = stats_.readAheadDuration >= handoffMinReadAhead;
    const bool warmupTimedOut =
        elapsed >= warmupMaxWait &&
        hasPublishedContinuity;
    if (!hasPublishedContinuity) {
        return;
    }
    if (softwareFrame && !hasReadAheadLead && !warmupTimedOut) {
        return;
    }
    if (!hasQueuedLead && !hasReadAheadLead && !warmupTimedOut) {
        return;
    }

    LogThread(LogLevel::Debug,
              L"decoder",
              L"seek_recovery_audio_handoff frames=" +
                  std::to_wstring(seekRecovery_.publishedFrames) +
                  L" span_ms=" + std::to_wstring(std::max(publishedSpan, std::chrono::milliseconds{0}).count()) +
                  L" elapsed_ms=" + std::to_wstring(std::max(elapsed, std::chrono::milliseconds{0}).count()) +
                  L" queue_depth=" + std::to_wstring(frameQueue_.size()) +
                  L" read_ahead_ms=" + std::to_wstring(stats_.readAheadDuration.count()) +
                  (softwareFrame ? L" software=true" : L"") +
                  (warmupTimedOut ? L" timeout=true" : L""));

    seekRecovery_ = {};
    stats_.seekRecoveryActive = false;
    stats_.seekRecoveryAudioHandoffReady = true;
    seekRecoveryActiveSnapshot_.store(false);
    seekAudioHandoffReadySnapshot_.store(true);
    fallbackClockAnchor_ = now;
    fallbackClockBasePts_ = publishedPts;
}

NativeVideoQueueStats FfmpegVideoDecoder::Stats() const {
    NativeVideoQueueStats snapshot;
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            std::unique_lock snapshotLock(statsSnapshotMutex_, std::try_to_lock);
            if (snapshotLock.owns_lock()) {
                snapshot = lastStatsSnapshot_;
            }
        } else {
            snapshot = stats_;
        }
    }
    {
        std::unique_lock snapshotLock(statsSnapshotMutex_, std::try_to_lock);
        if (snapshotLock.owns_lock()) {
            lastStatsSnapshot_ = snapshot;
        }
    }
    // These two gates are control-plane state, not telemetry. They must be
    // current even when the decoder frame mutex is busy so the window can
    // never resume shared-demux audio before seek visual warm-up completes.
    snapshot.seekRecoveryActive = seekRecoveryActiveSnapshot_.load();
    snapshot.seekRecoveryAudioHandoffReady = seekAudioHandoffReadySnapshot_.load();
    return snapshot;
}

bool FfmpegVideoDecoder::WaitForPreroll(const std::chrono::milliseconds targetDuration,
                                        const std::chrono::milliseconds timeout) const {
    if (oneShotFrame_ || timeout.count() <= 0) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stopping_.load() && running_.load() && !HasPendingSeek() && std::chrono::steady_clock::now() < deadline) {
        const auto stats = Stats();
        if (stats.queueDepth > 0 && (stats.bufferedDuration >= targetDuration || stats.queueDepth >= 2)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

bool FfmpegVideoDecoder::WaitForEnhancementPreroll(const std::chrono::milliseconds minPts,
                                                   const std::chrono::milliseconds timeout,
                                                   const bool publishReadyFrame) {
    if (oneShotFrame_ || !enableDolbyVisionEnhancementDecode_ || timeout.count() <= 0) {
        return false;
    }

    struct EnhancementPrerollWaitScope {
        std::atomic_bool& active;
        explicit EnhancementPrerollWaitScope(std::atomic_bool& value) : active(value) {
            active.store(true);
        }
        ~EnhancementPrerollWaitScope() {
            active.store(false);
        }
    } waitScope{enhancementPrerollWaitActive_};

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool baseOnlyDropLogged = false;
    while (!stopping_.load() && running_.load() && std::chrono::steady_clock::now() < deadline) {
        if (HasPendingSeek()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }
        bool ready = false;
        bool notify = false;
        bool queueChanged = false;
        {
            std::scoped_lock lock(mutex_);
            const auto closeToTarget = [minPts](const NativeVideoFrame& frame) {
                constexpr std::chrono::milliseconds kBehindTolerance{120};
                constexpr std::chrono::milliseconds kAheadTolerance{500};
                return frame.pts + kBehindTolerance >= minPts &&
                       frame.pts <= minPts + kAheadTolerance;
            };
            const auto enhanced = std::find_if(frameQueue_.begin(), frameQueue_.end(), [minPts](const NativeVideoFrame& frame) {
                constexpr std::chrono::milliseconds kBehindTolerance{120};
                constexpr std::chrono::milliseconds kAheadTolerance{500};
                return frame.HasEnhancementYuv() &&
                       frame.pts + kBehindTolerance >= minPts &&
                       frame.pts <= minPts + kAheadTolerance;
            });
            if (enhanced != frameQueue_.end()) {
                const std::size_t framesToDrop =
                    static_cast<std::size_t>(std::distance(frameQueue_.begin(), enhanced));
                for (std::size_t index = 0; index < framesToDrop; ++index) {
                    frameQueue_.pop_front();
                    ++stats_.droppedLate;
                }
                queueChanged = framesToDrop > 0;
                if (publishReadyFrame) {
                    NativeVideoFrame frameToPublish = std::move(frameQueue_.front());
                    frameQueue_.pop_front();
                    latestFrame_ = std::move(frameToPublish);
                    ++stats_.rendered;
                    stats_.buffering = false;
                    schedulePrimed_ = true;
                    notify = true;
                    queueChanged = true;
                }
                UpdateBufferedStatsLocked();
                ready = true;
            } else if (!publishReadyFrame &&
                       latestFrame_.HasEnhancementYuv() &&
                       closeToTarget(latestFrame_)) {
                ready = true;
            } else if (!frameQueue_.empty()) {
                const auto base = std::find_if(frameQueue_.begin(), frameQueue_.end(), closeToTarget);
                if (base != frameQueue_.end()) {
                    const std::size_t framesToDrop =
                        static_cast<std::size_t>(std::distance(frameQueue_.begin(), base)) + 1;
                    for (std::size_t index = 0; index < framesToDrop; ++index) {
                        frameQueue_.pop_front();
                        ++stats_.droppedLate;
                    }
                    queueChanged = framesToDrop > 0;
                    if (!baseOnlyDropLogged) {
                        baseOnlyDropLogged = true;
                        LogThread(LogLevel::Debug,
                                  L"decoder",
                                  L"dolby_vision_el_overlay enhancement_preroll_drop_base_only"
                                      L" dropped=" + std::to_wstring(framesToDrop));
                    }
                } else {
                    const std::size_t dropped = frameQueue_.size();
                    frameQueue_.clear();
                    stats_.droppedLate += dropped;
                    queueChanged = dropped > 0;
                }
                UpdateBufferedStatsLocked();
            }
        }
        if (queueChanged) {
            frameQueueCv_.notify_all();
        }
        if (notify) {
            NotifyFrameReady();
        }
        if (ready) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

int FfmpegVideoDecoder::SelectVideoStream(AVFormatContext* formatCtx) const {
    if (!formatCtx) {
        return AVERROR(EINVAL);
    }

    const auto streamAt = [formatCtx](const int streamIndex) -> AVStream* {
        if (streamIndex < 0 || static_cast<unsigned int>(streamIndex) >= formatCtx->nb_streams) {
            return nullptr;
        }
        return formatCtx->streams[streamIndex];
    };

    if (selectedVideoTrackIndex_ >= 0) {
        AVStream* stream = streamAt(selectedVideoTrackIndex_);
        if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            LogThread(LogLevel::Info, L"decoder", L"video_stream selected=requested " + VideoStreamDescription(stream));
            return selectedVideoTrackIndex_;
        }
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"video_stream requested_unavailable stream=" + std::to_wstring(selectedVideoTrackIndex_));
    } else if (selectedVideoTrackIndex_ == anvil::playback::kVideoTrackDolbyVisionEnhancement) {
        for (unsigned int index = 0; index < formatCtx->nb_streams; ++index) {
            AVStream* stream = formatCtx->streams[index];
            if (!stream || !stream->codecpar ||
                stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                !IsDolbyVisionEnhancementLayer(stream->codecpar)) {
                continue;
            }
            LogThread(LogLevel::Info, L"decoder", L"video_stream selected=dolby_vision_enhancement " + VideoStreamDescription(stream));
            return static_cast<int>(index);
        }
        LogThread(LogLevel::Warning, L"decoder", L"video_stream dolby_vision_enhancement_unavailable fallback=auto");
    } else if (selectedVideoTrackIndex_ != anvil::playback::kVideoTrackAuto) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"video_stream unknown_selection=" + std::to_wstring(selectedVideoTrackIndex_) + L" fallback=auto");
    }

    const int best = av_find_best_stream(formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (best >= 0) {
        LogThread(LogLevel::Info, L"decoder", L"video_stream selected=auto " + VideoStreamDescription(streamAt(best)));
    }
    return best;
}

void FfmpegVideoDecoder::DecodeLoop() {
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVCodecContext* enhancementCodecCtx = nullptr;
    AVCodecContext* subtitleCodecCtx = nullptr;
    AVBufferRef* hwDeviceCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* softwareFrame = nullptr;
    AVFrame* enhancementFrame = nullptr;
    AVPacket* packet = nullptr;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    int enhancementStreamIndex = -1;
    int subtitleStreamIndex = -1;
    AVRational streamTimeBase{1, 1};
    AVRational audioTimeBase{1, 1};
    AVRational enhancementTimeBase{1, 1};
    AVRational subtitleTimeBase{1, 1};
    uint64_t serial = 0;
    bool firstPacketSeen = false;
    bool audioPacketSinkActive = false;
    bool startupComplete = false;
    std::wstring startupFailure;
    auto failStartup = [&](std::wstring message) {
        startupFailure = std::move(message);
        LogThreadError(startupFailure);
    };

    const std::string pathUtf8 = WideToUtf8(path_.wstring());
    const bool networkSource = IsNetworkMediaPath(path_);

    do {
        formatCtx = avformat_alloc_context();
        if (!formatCtx) {
            failStartup(L"avformat_alloc_context failed");
            break;
        }
        formatCtx->interrupt_callback.callback = &FfmpegVideoDecoder::InterruptCallback;
        formatCtx->interrupt_callback.opaque = this;
        // Probe cost limits (must be set before avformat_open_input to take
        // effect during probing). Smaller = faster startup on large files.
        formatCtx->probesize = 2 * 1024 * 1024;             // 2 MB
        formatCtx->max_analyze_duration = 2 * AV_TIME_BASE;  // 2 s
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "15000000", 0);  // 15 s IO timeout
        if (networkSource) {
            av_dict_set(&options, "seekable", "1", 0);
            av_dict_set(&options,
                        "user_agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36",
                        0);
            av_dict_set(&options, "reconnect_on_network_error", "1", 0);
            av_dict_set(&options, "reconnect_streamed", "1", 0);
            av_dict_set(&options, "reconnect_delay_max", "2", 0);
            av_dict_set(&options, "reconnect_max_retries", "2", 0);
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"network_open mode=range_seek seekable=1");
        }
        const int openResult = avformat_open_input(&formatCtx, pathUtf8.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (openResult < 0) {
            failStartup(L"avformat_open_input failed: " + FfmpegErrorString(openResult));
            break;
        }
        const int streamInfoResult = avformat_find_stream_info(formatCtx, nullptr);
        if (streamInfoResult < 0) {
            failStartup(L"avformat_find_stream_info failed: " + FfmpegErrorString(streamInfoResult));
            break;
        }
        if (networkSource) {
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"stream_info complete mode=bounded_probe interrupts=" +
                          std::to_wstring(interruptReturnCount_.load()) +
                          L" " + FormatIoState(formatCtx));
            if (formatCtx->pb && formatCtx->pb->eof_reached && formatCtx->pb->error == 0) {
                formatCtx->pb->eof_reached = 0;
                LogThread(LogLevel::Warning,
                          L"decoder",
                          L"stream_info eof cleared for network source");
            }
        }
        videoStreamIndex = SelectVideoStream(formatCtx);
        if (videoStreamIndex < 0) {
            failStartup(L"no video stream found");
            break;
        }
        streamTimeBase = formatCtx->streams[videoStreamIndex]->time_base;
        const AVRational sourceFrameRate =
            formatCtx->streams[videoStreamIndex]->avg_frame_rate.num > 0 &&
                    formatCtx->streams[videoStreamIndex]->avg_frame_rate.den > 0
                ? formatCtx->streams[videoStreamIndex]->avg_frame_rate
                : formatCtx->streams[videoStreamIndex]->r_frame_rate;
        videoFrameRateNumerator_ = sourceFrameRate.num > 0
                                       ? static_cast<UINT32>(sourceFrameRate.num)
                                       : 24000u;
        videoFrameRateDenominator_ = sourceFrameRate.den > 0
                                         ? static_cast<UINT32>(sourceFrameRate.den)
                                         : 1001u;
        AVCodecParameters* codecpar = formatCtx->streams[videoStreamIndex]->codecpar;
        if (audioPacketSink_.Enabled()) {
            const int requestedAudioStream = audioPacketSink_.selectedTrackIndex;
            if (requestedAudioStream >= 0 &&
                requestedAudioStream < static_cast<int>(formatCtx->nb_streams) &&
                formatCtx->streams[requestedAudioStream] &&
                formatCtx->streams[requestedAudioStream]->codecpar &&
                formatCtx->streams[requestedAudioStream]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = requestedAudioStream;
            } else {
                if (requestedAudioStream >= 0) {
                    LogThread(LogLevel::Warning,
                              L"audio",
                              L"packet_sink requested_unavailable stream=" +
                                  std::to_wstring(requestedAudioStream) +
                                  L" fallback=auto");
                }
                audioStreamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            }
            if (audioStreamIndex >= 0 &&
                audioStreamIndex < static_cast<int>(formatCtx->nb_streams) &&
                formatCtx->streams[audioStreamIndex] &&
                formatCtx->streams[audioStreamIndex]->codecpar) {
                audioTimeBase = formatCtx->streams[audioStreamIndex]->time_base;
                audioPacketSinkActive =
                    audioPacketSink_.start(formatCtx->streams[audioStreamIndex]->codecpar,
                                           audioTimeBase,
                                           startPosition_,
                                           audioStreamIndex);
                LogThread(audioPacketSinkActive ? LogLevel::Info : LogLevel::Warning,
                          L"audio",
                          L"packet_sink stream=" + std::to_wstring(audioStreamIndex) +
                              L" active=" + (audioPacketSinkActive ? L"true" : L"false"));
                if (!audioPacketSinkActive && audioPacketSink_.startFailed) {
                    audioPacketSink_.startFailed();
                }
            } else {
                LogThread(LogLevel::Warning, L"audio", L"packet_sink unavailable reason=no_audio_stream");
                if (audioPacketSink_.startFailed) {
                    audioPacketSink_.startFailed();
                }
            }
        }
        subtitleCanvasWidth_ = codecpar ? codecpar->width : 0;
        subtitleCanvasHeight_ = codecpar ? codecpar->height : 0;
        bool externalSubtitlesLoaded = false;
        if (selectedSubtitleTrackIndex_ == anvil::playback::kSubtitleTrackOff) {
            LogThread(LogLevel::Info, L"subtitle", L"selected=off");
        } else if (selectedSubtitleTrackIndex_ == anvil::playback::kSubtitleTrackAuto &&
                   !externalSubtitlePath_.empty()) {
            externalSubtitlesLoaded = DecodeExternalSubtitleFile(externalSubtitlePath_);
            if (externalSubtitlesLoaded) {
                externalSubtitlesActive_ = true;
                externalSubtitleCues_ = subtitleCues_;
            }
        } else if (selectedSubtitleTrackIndex_ == anvil::playback::kSubtitleTrackAuto && autoLoadExternalSubtitles_) {
            const auto externalSubtitle = FindExternalSubtitleFile(path_, preferredSubtitleLanguage_);
            if (externalSubtitle.has_value()) {
                externalSubtitlesLoaded = DecodeExternalSubtitleFile(*externalSubtitle);
                if (externalSubtitlesLoaded) {
                    externalSubtitlesActive_ = true;
                    externalSubtitleCues_ = subtitleCues_;
                }
            } else {
                LogThread(LogLevel::Debug, L"subtitle", L"external=none");
            }
        }
        if (selectedSubtitleTrackIndex_ != anvil::playback::kSubtitleTrackOff && !externalSubtitlesLoaded) {
            OpenSubtitleDecoder(formatCtx, subtitleStreamIndex, subtitleTimeBase, subtitleCodecCtx);
            if (networkSource) {
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"after_subtitle_open " + FormatIoState(formatCtx));
            }
        }

        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            failStartup(L"avcodec_find_decoder failed");
            break;
        }
        streamColorMetadata_ = BuildColorMetadata(codecpar);
        LogThread(LogLevel::Info,
                  L"decoder",
                  L"input_color primaries=" + anvil::playback::ToDisplayString(streamColorMetadata_.primaries) +
                      L" transfer=" + anvil::playback::ToDisplayString(streamColorMetadata_.transfer) +
                      L" matrix=" + anvil::playback::ToDisplayString(streamColorMetadata_.matrix) +
                      L" range=" + anvil::playback::ToDisplayString(streamColorMetadata_.range));

        // Stream-level Dolby Vision detection via AV_PKT_DATA_DOVI_CONF side data.
        dolbyVisionStream_ = false;
        dolbyVisionFirstFrameLogged_ = false;
        dolbyVisionFirstPackedLogged_ = false;
        dolbyVisionFirstQueueLogged_ = false;
        dolbyVisionLibplaceboFailed_ = false;
        dolbyVisionLibplaceboFrameLogged_ = false;
        dolbyVisionDynamicMetadataLogged_ = false;
        dolbyVisionLastDynamicMetadataFingerprint_ = 0;
        doviLibplaceboFilter_.reset();
        dolbyVisionProfile_ = 0;
        dolbyVisionLevel_ = 0;
        dolbyVisionCompatId_ = 0;
        dolbyVisionElPresent_ = false;
        dolbyVisionBlPresent_ = false;
        for (int i = 0; i < codecpar->nb_coded_side_data; ++i) {
            const AVPacketSideData& sd = codecpar->coded_side_data[i];
            if (sd.type == AV_PKT_DATA_DOVI_CONF &&
                sd.size >= static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
                const auto* conf = reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(sd.data);
                dolbyVisionStream_ = true;
                dolbyVisionProfile_ = conf->dv_profile;
                dolbyVisionLevel_ = conf->dv_level;
                dolbyVisionCompatId_ = conf->dv_bl_signal_compatibility_id;
                dolbyVisionElPresent_ = conf->el_present_flag != 0;
                dolbyVisionBlPresent_ = conf->bl_present_flag != 0;
                LogThread(LogLevel::Info, L"decoder",
                          L"dolby_vision_stream profile=" + std::to_wstring(conf->dv_profile) +
                              L" level=" + std::to_wstring(conf->dv_level) +
                              L" el=" + std::to_wstring(conf->el_present_flag) +
                              L" bl=" + std::to_wstring(conf->bl_present_flag) +
                              L" compat_id=" + std::to_wstring(conf->dv_bl_signal_compatibility_id));
                break;
            }
        }
        if (dolbyVisionStream_) {
            LogThread(LogLevel::Info,
                      L"decoder",
                      std::wstring(L"dolby_vision_processing primary=") +
                          (ShouldUsePrimaryDoviLibplacebo() ? L"libplacebo" : L"renderer_fallback") +
                          L" reason=" +
                          (ShouldUsePrimaryDoviLibplacebo()
                               ? L"single_layer_profile_5_or_8"
                               : L"profile_or_enhancement_layer_requires_future_merge"));
        }

        if (enableDolbyVisionEnhancementDecode_ &&
            selectedVideoTrackIndex_ != anvil::playback::kVideoTrackDolbyVisionEnhancement) {
            OpenDolbyVisionEnhancementDecoder(formatCtx, videoStreamIndex, enhancementCodecCtx, enhancementTimeBase);
            enhancementStreamIndex = dolbyVisionEnhancementStreamIndex_;
        } else if (enableDolbyVisionEnhancementDecode_) {
            LogThread(LogLevel::Info,
                      L"decoder",
                      L"dolby_vision_el disabled reason=primary_stream_is_enhancement_layer");
        }

        if (!OpenVideoDecoder(codec, codecpar, codecCtx, hwDeviceCtx)) {
            startupFailure = L"video decoder open failed";
            break;
        }
        if (networkSource) {
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"after_video_decoder_open " + FormatIoState(formatCtx));
        }

        frame = av_frame_alloc();
        softwareFrame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!frame || !softwareFrame || !packet) {
            failStartup(L"av frame allocation failed");
            break;
        }
        if (enhancementCodecCtx) {
            enhancementFrame = av_frame_alloc();
            if (!enhancementFrame) {
                LogThread(LogLevel::Warning,
                          L"decoder",
                          L"dolby_vision_el disabled reason=frame_alloc_failed");
                avcodec_free_context(&enhancementCodecCtx);
                enhancementStreamIndex = -1;
                dolbyVisionEnhancementActive_ = false;
            }
        }
        startupComplete = true;

        const bool skipNetworkNearStartSeek =
            networkSource && startPosition_ < std::chrono::seconds{1};
        if (startPosition_.count() > 0 && !skipNetworkNearStartSeek) {
            const int64_t seekTarget = static_cast<int64_t>(startPosition_.count()) *
                                       AV_TIME_BASE / 1000;
            const auto seekIoTimeout = networkSource ? kRuntimeNetworkSeekIoTimeout : kRuntimeSeekIoTimeout;
            const auto initialSeekStart = std::chrono::steady_clock::now();
            ioInterruptAfterSteadyMs_.store(SteadyClockMs() + seekIoTimeout.count());
            const int seekResult = av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
            ioInterruptAfterSteadyMs_.store(0);
            const auto initialSeekElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - initialSeekStart).count();
            if (seekResult < 0) {
                LogThread(LogLevel::Warning,
                          L"decoder",
                          L"initial_seek failed reason=" + FfmpegErrorString(seekResult) +
                              L" target=" + anvil::playback::FormatTimecode(startPosition_) +
                              L" seek_ms=" + std::to_wstring(initialSeekElapsedMs) +
                              (seekResult == AVERROR_EXIT ||
                                       initialSeekElapsedMs >= seekIoTimeout.count()
                                   ? L" interrupted=true"
                                   : L"") +
                              L" " + FormatIoState(formatCtx));
                if (networkSource) {
                    NotifyDecodeFailure(L"Initial network seek failed at " +
                                        anvil::playback::FormatTimecode(startPosition_) +
                                        L": " + FfmpegErrorString(seekResult));
                    break;
                }
            } else if (networkSource) {
                ClearNetworkIoState(formatCtx);
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"initial_seek target=" + anvil::playback::FormatTimecode(startPosition_) +
                              L" seek_ms=" + std::to_wstring(initialSeekElapsedMs) +
                              L" " + FormatIoState(formatCtx));
            }
            if (subtitleCodecCtx) {
                avcodec_flush_buffers(subtitleCodecCtx);
            }
            if (enhancementCodecCtx) {
                avcodec_flush_buffers(enhancementCodecCtx);
            }
        } else if (startPosition_.count() > 0 && skipNetworkNearStartSeek) {
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"initial_seek skipped reason=network_near_start target=" +
                          anvil::playback::FormatTimecode(startPosition_) +
                          L" " + FormatIoState(formatCtx));
        }

        std::deque<CachedVideoPacket> packetQueue;
        std::size_t packetQueueBytes = 0;
        std::chrono::milliseconds packetTimelineEnd = startPosition_;
        bool inputEof = false;
        bool packetBudgetExceeded = false;
        bool readFrameWaitLogged = false;
        bool firstReadFrameLogged = false;
        bool networkEofClearAttempted = false;
        bool networkEofSeekResetAttempted = false;
        bool networkInterruptedIoResetAttempted = false;
        int nonVideoPacketLogCount = 0;
        int videoPacketCacheLogCount = 0;
        int readFrameErrorLogCount = 0;
        bool sharedDemuxCapacityReadAheadLogged = false;
        bool packetBudgetLimitLogged = false;
        uint64_t networkBytesWindow = 0;
        auto networkBytesWindowStartedAt = std::chrono::steady_clock::now();
        const auto fallbackPacketDuration = EstimatedVideoPacketDuration(formatCtx->streams[videoStreamIndex]);
        const std::size_t packetCacheTargetBytes =
            PacketReadAheadBudgetBytes(kMinPacketReadAheadBytes, kMaxPacketReadAheadBytes);
        auto enhancementOverlayReadAheadActive = [&]() {
            return enableDolbyVisionEnhancementDecode_ &&
                   enhancementCodecCtx &&
                   enhancementFrame &&
                   dolbyVisionEnhancementActive_;
        };
        auto activePacketReadAheadTarget = [&]() {
            return enhancementOverlayReadAheadActive()
                       ? kDolbyVisionEnhancementPacketReadAheadTarget
                       : kPlayingPacketReadAheadTarget;
        };
        LogThread(LogLevel::Info,
                  L"decoder",
                  L"packet_read_ahead mode=adaptive target_mb=" +
                      std::to_wstring(packetCacheTargetBytes / (1024 * 1024)) +
                      L" hard_budget_mb=" + std::to_wstring(kPacketMemoryBudgetBytes / (1024 * 1024)) +
                      L" max_packet_mb=" + std::to_wstring(kMaxCachedPacketBytes / (1024 * 1024)) +
                      L" playing_target_ms=" + std::to_wstring(activePacketReadAheadTarget().count()) +
                      (enhancementOverlayReadAheadActive() ? L" el_overlay=low_latency" : L"") +
                      L" estimated_packet_ms=" + std::to_wstring(fallbackPacketDuration.count()));
        pendingSeekInterruptsEnabled_.store(true);

        auto packetQueueEnd = [&]() {
            std::chrono::milliseconds end{0};
            for (const auto& cached : packetQueue) {
                end = std::max(end, cached.end);
            }
            return end;
        };

        auto updatePacketStats = [&]() {
            {
                std::scoped_lock lock(mutex_);
                stats_.packetQueueDepth = packetQueue.size();
                stats_.packetQueueBytes = packetQueueBytes;
                if (packetQueue.empty()) {
                    stats_.readAheadEnd = std::chrono::milliseconds{0};
                    stats_.readAheadDuration = std::chrono::milliseconds{0};
                } else {
                    stats_.readAheadEnd = packetQueueEnd();
                    stats_.readAheadDuration =
                        std::max(std::chrono::milliseconds{0}, stats_.readAheadEnd - packetQueue.front().pts);
                }
                UpdateBufferedStatsLocked();
            }
        };

        auto clearPacketQueue = [&]() {
            packetQueue.clear();
            packetQueueBytes = 0;
            updatePacketStats();
        };

        auto packetQueueAtTarget = [&]() {
            return packetQueueBytes >= packetCacheTargetBytes;
        };

        auto packetQueueDuration = [&]() {
            if (packetQueue.empty()) {
                return std::chrono::milliseconds{0};
            }
            return std::max(std::chrono::milliseconds{0}, packetQueueEnd() - packetQueue.front().pts);
        };

        auto shouldReadPlayingPackets = [&]() {
            return !inputEof &&
                   !packetQueueAtTarget() &&
                   packetQueueDuration() < activePacketReadAheadTarget();
        };

        auto hasDecodedPreroll = [&]() {
            std::scoped_lock lock(mutex_);
            return latestFrame_.HasContent() || !frameQueue_.empty() || stats_.rendered > 0;
        };

        auto decodedQueueNearCapacity = [&]() {
            std::scoped_lock lock(mutex_);
            if (frameQueue_.empty()) {
                return false;
            }
            const std::size_t capacity = MaxQueueDepthForFrame(frameQueue_.back());
            return frameQueue_.size() + 1 >= capacity;
        };

        auto absEnhancementDelta = [](const std::chrono::milliseconds lhs, const std::chrono::milliseconds rhs) {
            return lhs >= rhs ? lhs - rhs : rhs - lhs;
        };

        auto enhancementQueueSaturated = [&]() {
            if (dolbyVisionEnhancementFrames_.empty()) {
                return false;
            }
            std::size_t queuedBytes = 0;
            for (const auto& queued : dolbyVisionEnhancementFrames_) {
                queuedBytes = SaturatingAddBytes(queuedBytes,
                                                 BufferCapacityBytes(queued.yuv.data));
            }
            const std::size_t representativeFrameBytes =
                BufferCapacityBytes(dolbyVisionEnhancementFrames_.front().yuv.data);
            return dolbyVisionEnhancementFrames_.size() >= kMaxDolbyVisionEnhancementQueuedFrames ||
                   !FitsWithinBudget(queuedBytes,
                                     representativeFrameBytes,
                                     kMaxDolbyVisionEnhancementQueuedBytes);
        };

        auto enhancementReadyForNextVideoPacket = [&]() {
            if (!enhancementOverlayReadAheadActive() || packetQueue.empty()) {
                return true;
            }
            if (dolbyVisionEnhancementFrames_.empty()) {
                return inputEof;
            }

            auto targetPts = packetQueue.front().pts;
            if (pendingDolbyVisionBaseFrame_.has_value() &&
                pendingDolbyVisionBaseFrame_->timelineSerial == CurrentTimelineSerial()) {
                targetPts = std::max(targetPts, pendingDolbyVisionBaseFrame_->pts);
            }
            auto bestDelta = absEnhancementDelta(dolbyVisionEnhancementFrames_.front().pts, targetPts);
            for (std::size_t index = 1; index < dolbyVisionEnhancementFrames_.size(); ++index) {
                bestDelta = std::min(bestDelta,
                                     absEnhancementDelta(dolbyVisionEnhancementFrames_[index].pts, targetPts));
            }
            if (bestDelta <= kDolbyVisionEnhancementMatchDelta) {
                return true;
            }

            const auto firstPts = dolbyVisionEnhancementFrames_.front().pts;
            const auto lastPts = dolbyVisionEnhancementFrames_.back().pts;
            if (targetPts + kDolbyVisionEnhancementMatchDelta < firstPts) {
                return true;
            }
            if (lastPts + kDolbyVisionEnhancementMatchDelta < targetPts) {
                // Let the BL decoder consume one packet and prune its stale EL
                // window when the bounded EL queue cannot advance further.
                return inputEof || packetQueueAtTarget() || enhancementQueueSaturated();
            }
            return true;
        };

        auto cacheVideoPacket = [&](AVPacket* source) {
            const std::size_t packetBytes = PacketCacheCostBytes(source);
            if (packetBytes > kMaxCachedPacketBytes ||
                !FitsWithinBudget(packetQueueBytes, packetBytes, kPacketMemoryBudgetBytes)) {
                if (!packetBudgetLimitLogged) {
                    packetBudgetLimitLogged = true;
                    LogThread(LogLevel::Warning,
                              L"decoder",
                              L"packet_cache rejected bytes=" + std::to_wstring(packetBytes) +
                                  L" queued_bytes=" + std::to_wstring(packetQueueBytes) +
                                  L" hard_budget_bytes=" + std::to_wstring(kPacketMemoryBudgetBytes));
                }
                packetBudgetExceeded = true;
                inputEof = true;
                av_packet_unref(source);
                return false;
            }
            const auto fallbackStart = packetQueue.empty() ? packetTimelineEnd : packetQueue.back().end;
            auto cached = CachedVideoPacket::MoveFrom(source,
                                                      streamTimeBase,
                                                      fallbackStart,
                                                      fallbackPacketDuration,
                                                      CurrentTimelineSerial());
            if (!cached.has_value()) {
                av_packet_unref(source);
                return false;
            }
            packetTimelineEnd = std::max(packetTimelineEnd, cached->end);
            packetQueueBytes += cached->bytes;
            packetQueue.push_back(std::move(*cached));
            updatePacketStats();
            return true;
        };

        auto readAheadPackets = [&](const int maxVideoPackets,
                                    const bool allowBeyondDurationTarget = false) {
            int videoPacketsRead = 0;
            while (!stopping_.load() &&
                   !HasPendingSeek() &&
                   !inputEof &&
                   (allowBeyondDurationTarget || !packetQueueAtTarget()) &&
                   videoPacketsRead < maxVideoPackets) {
                if (enhancementOverlayReadAheadActive() &&
                    !packetQueue.empty() &&
                    enhancementQueueSaturated()) {
                    break;
                }
                if (!readFrameWaitLogged) {
                    readFrameWaitLogged = true;
                    LogThread(LogLevel::Debug,
                              L"decoder",
                              L"read_frame_wait begin stream=" + std::to_wstring(videoStreamIndex) +
                                  L" max_video_packets=" + std::to_wstring(maxVideoPackets) +
                                  L" " + FormatIoState(formatCtx));
                }
                const int readResult = av_read_frame(formatCtx, packet);
                if (readResult < 0) {
                    if (readResult == AVERROR_EOF &&
                        networkSource &&
                        !HasPendingSeek()) {
                        if (!networkEofClearAttempted && formatCtx->pb) {
                            networkEofClearAttempted = true;
                            ClearNetworkIoState(formatCtx);
                            LogThread(LogLevel::Warning,
                                      L"decoder",
                                      L"read_frame eof_reset method=clear_eof " + FormatIoState(formatCtx));
                            continue;
                        }
                    }
                    if (readResult == AVERROR_EOF &&
                        networkSource &&
                        videoPacketsRead == 0 &&
                        !firstReadFrameLogged &&
                        !networkEofSeekResetAttempted &&
                        !HasPendingSeek()) {
                        networkEofSeekResetAttempted = true;
                        const int64_t seekTarget = static_cast<int64_t>(startPosition_.count()) *
                                                   AV_TIME_BASE / 1000;
                        const auto seekIoTimeout = networkSource ? kRuntimeNetworkSeekIoTimeout : kRuntimeSeekIoTimeout;
                        const auto resetSeekStart = std::chrono::steady_clock::now();
                        ioInterruptAfterSteadyMs_.store(SteadyClockMs() + seekIoTimeout.count());
                        const int seekError = av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
                        ioInterruptAfterSteadyMs_.store(0);
                        const auto resetSeekElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - resetSeekStart).count();
                        if (seekError >= 0) {
                            avformat_flush(formatCtx);
                            ClearNetworkIoState(formatCtx);
                            LogThread(LogLevel::Warning,
                                      L"decoder",
                                      L"read_frame eof_reset method=seek target=" +
                                          anvil::playback::FormatTimecode(startPosition_) +
                                          L" seek_ms=" + std::to_wstring(resetSeekElapsedMs));
                            continue;
                        }
                        LogThread(LogLevel::Warning,
                                  L"decoder",
                                  L"read_frame eof_reset failed reason=" + FfmpegErrorString(seekError) +
                                      L" target=" + anvil::playback::FormatTimecode(startPosition_) +
                                      L" seek_ms=" + std::to_wstring(resetSeekElapsedMs) +
                                      (seekError == AVERROR_EXIT ||
                                               resetSeekElapsedMs >= seekIoTimeout.count()
                                           ? L" interrupted=true"
                                           : L"") +
                                      L" " + FormatIoState(formatCtx));
                    }
                    if (readResult == AVERROR_EXIT &&
                        networkSource &&
                        !networkInterruptedIoResetAttempted &&
                        !stopping_.load() &&
                        !HasPendingSeek() &&
                        ioInterruptAfterSteadyMs_.load() == 0) {
                        networkInterruptedIoResetAttempted = true;
                        if (ClearNetworkIoState(formatCtx)) {
                            LogThread(LogLevel::Warning,
                                      L"decoder",
                                      L"read_frame interrupt_reset method=clear_io_state " +
                                          FormatIoState(formatCtx));
                            continue;
                        }
                    }
                    if (readFrameErrorLogCount < 3) {
                        ++readFrameErrorLogCount;
                        LogThread(LogLevel::Warning,
                                  L"decoder",
                                  L"read_frame result=error reason=" + FfmpegErrorString(readResult) +
                                      L" video_packets_this_call=" + std::to_wstring(videoPacketsRead) +
                                      L" pending_seek=" + (HasPendingSeek() ? L"true" : L"false") +
                                      L" interrupts=" + std::to_wstring(interruptReturnCount_.load()) +
                                      L" " + FormatIoState(formatCtx));
                    }
                    if (!HasPendingSeek()) {
                        inputEof = true;
                    }
                    break;
                }
                if (!firstReadFrameLogged) {
                    firstReadFrameLogged = true;
                    LogThread(LogLevel::Debug,
                              L"decoder",
                              L"read_frame first stream=" + std::to_wstring(packet->stream_index) +
                                  L" type=" + PacketStreamTypeName(formatCtx, packet->stream_index) +
                                  L" bytes=" + std::to_wstring(packet->size));
                }
                if (networkSource && packet->size > 0) {
                    networkBytesWindow += static_cast<uint64_t>(packet->size);
                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - networkBytesWindowStartedAt).count();
                    if (elapsedMs >= 1000) {
                        const uint64_t bytesPerSecond =
                            networkBytesWindow * 1000ull / static_cast<uint64_t>(std::max<int64_t>(1, elapsedMs));
                        {
                            std::scoped_lock lock(mutex_);
                            stats_.networkBytesPerSecond = bytesPerSecond;
                        }
                        networkBytesWindow = 0;
                        networkBytesWindowStartedAt = now;
                    }
                }

                if (packet->stream_index == enhancementStreamIndex &&
                    enhancementCodecCtx &&
                    enhancementFrame &&
                    dolbyVisionEnhancementActive_) {
                    DecodeDolbyVisionEnhancementPacket(enhancementCodecCtx, packet, enhancementFrame, enhancementTimeBase);
                    av_packet_unref(packet);
                    continue;
                }

                if (audioPacketSinkActive && packet->stream_index == audioStreamIndex) {
                    if (!audioPacketSink_.pushPacket(packet) && nonVideoPacketLogCount < 8) {
                        ++nonVideoPacketLogCount;
                        LogThread(LogLevel::Debug,
                                  L"audio",
                                  L"packet_sink drop stream=" + std::to_wstring(packet->stream_index) +
                                      L" bytes=" + std::to_wstring(packet->size));
                    }
                    av_packet_unref(packet);
                    continue;
                }

                if (packet->stream_index == subtitleStreamIndex && (subtitleCodecCtx || subtitleAssActive_)) {
                    DecodeSubtitlePacket(subtitleCodecCtx, packet, subtitleTimeBase);
                    av_packet_unref(packet);
                    continue;
                }

                if (packet->stream_index != videoStreamIndex) {
                    if (nonVideoPacketLogCount < 8) {
                        ++nonVideoPacketLogCount;
                        LogThread(LogLevel::Debug,
                                  L"decoder",
                                  L"read_frame skip stream=" + std::to_wstring(packet->stream_index) +
                                      L" type=" + PacketStreamTypeName(formatCtx, packet->stream_index) +
                                      L" bytes=" + std::to_wstring(packet->size));
                    }
                    av_packet_unref(packet);
                    continue;
                }

                if (!cacheVideoPacket(packet)) {
                    break;
                }
                if (videoPacketCacheLogCount < 5) {
                    ++videoPacketCacheLogCount;
                    const auto& cached = packetQueue.back();
                    LogThread(LogLevel::Debug,
                              L"decoder",
                              L"read_frame cached_video pts_ms=" + std::to_wstring(cached.pts.count()) +
                                  L" end_ms=" + std::to_wstring(cached.end.count()) +
                                  L" bytes=" + std::to_wstring(cached.bytes) +
                                  L" depth=" + std::to_wstring(packetQueue.size()));
                }
                ++videoPacketsRead;
            }
        };

        auto readAheadForEnhancementMatch = [&]() {
            if (!enhancementOverlayReadAheadActive() ||
                packetQueue.empty() ||
                enhancementReadyForNextVideoPacket() ||
                inputEof ||
                packetQueueAtTarget()) {
                return false;
            }
            // A pending BL/EL synchronization point takes precedence over the
            // normal packet-duration target. The packet byte budget remains a
            // hard bound, so reading a few more interleaved packets cannot make
            // the queue unbounded.
            readAheadPackets(kDolbyVisionEnhancementPlayingPacketReadAheadBatch, true);
            return true;
        };

        auto decodeNextCachedPacket = [&]() {
            if (packetQueue.empty()) {
                return true;
            }

            CachedVideoPacket cached = std::move(packetQueue.front());
            if (packetQueueBytes >= cached.bytes) {
                packetQueueBytes -= cached.bytes;
            } else {
                packetQueueBytes = 0;
            }
            packetQueue.pop_front();
            updatePacketStats();
            if (cached.timelineSerial != CurrentTimelineSerial()) {
                {
                    std::scoped_lock lock(mutex_);
                    ++stats_.droppedStale;
                    UpdateBufferedStatsLocked();
                }
                return true;
            }

            if (!firstPacketSeen) {
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"first_video_packet pts_ms=" + std::to_wstring(cached.pts.count()) +
                              L" end_ms=" + std::to_wstring(cached.end.count()) +
                              L" bytes=" + std::to_wstring(cached.bytes) +
                              L" timeline_serial=" + std::to_wstring(cached.timelineSerial) +
                              L" key=" + ((cached.packet && (cached.packet->flags & AV_PKT_FLAG_KEY) != 0) ? L"true" : L"false"));
            }
            firstPacketSeen = true;
            int eagainCount = 0;
            int backpressureCount = 0;
            constexpr int kSendPacketBackpressureRetries = 250;
            while (!stopping_.load() && !HasPendingSeek()) {
                const int sendResult = avcodec_send_packet(codecCtx, cached.packet);
                if (sendResult == AVERROR(EAGAIN)) {
                    if (!ReceiveFrames(codecCtx, swsCtx, frame, softwareFrame, streamTimeBase, serial)) {
                        return false;
                    }
                    if (++eagainCount > 8) {
                        return true;
                    }
                    continue;
                }
                if (sendResult == AVERROR(ENOMEM)) {
                    if (!sendPacketBackpressureLogged_) {
                        sendPacketBackpressureLogged_ = true;
                        LogThread(LogLevel::Debug,
                                  L"decoder",
                                  L"send_packet_backpressure reason=" + FfmpegErrorString(sendResult) +
                                      L" pts_ms=" + std::to_wstring(cached.pts.count()));
                    }
                    WakeScheduler();
                    if (!ReceiveFrames(codecCtx, swsCtx, frame, softwareFrame, streamTimeBase, serial)) {
                        return false;
                    }
                    if (++backpressureCount > kSendPacketBackpressureRetries) {
                        if (!sendPacketFailureLogged_) {
                            sendPacketFailureLogged_ = true;
                            LogThread(LogLevel::Warning,
                                      L"decoder",
                                      L"send_packet_failed reason=" + FfmpegErrorString(sendResult) +
                                          L" pts_ms=" + std::to_wstring(cached.pts.count()) +
                                          L" retries=" + std::to_wstring(backpressureCount));
                        }
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    continue;
                }
                if (sendResult < 0) {
                    if (!sendPacketFailureLogged_) {
                        sendPacketFailureLogged_ = true;
                        LogThread(LogLevel::Warning,
                                  L"decoder",
                                  L"send_packet_failed reason=" + FfmpegErrorString(sendResult) +
                                      L" pts_ms=" + std::to_wstring(cached.pts.count()));
                    }
                    return true;
                }
                break;
            }

            if (!ReceiveFrames(codecCtx, swsCtx, frame, softwareFrame, streamTimeBase, serial)) {
                return false;
            }
            return true;
        };

        while (!stopping_.load()) {
            if (HasPendingSeek()) {
                clearPacketQueue();
                inputEof = false;
                packetBudgetExceeded = false;
                if (!ApplyPendingSeek(formatCtx,
                                      codecCtx,
                                      subtitleCodecCtx,
                                      enhancementCodecCtx,
                                      videoStreamIndex,
                                      streamTimeBase)) {
                    break;
                }
                packetTimelineEnd = startPosition_;
                continue;
            }

            const bool paused = playbackPaused_.load();
            const bool hasPreroll = hasDecodedPreroll();
            const bool elOverlay = enhancementOverlayReadAheadActive();
            const bool enhancementPrerollWaitActive = enhancementPrerollWaitActive_.load();
            const bool seekPrerollWaiting = seekPrerollPending_.load();
            if (paused && hasPreroll && !enhancementPrerollWaitActive) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                continue;
            }

            const bool decodedNearCapacity = !paused && hasPreroll && decodedQueueNearCapacity();
            if (HasPendingSeek()) {
                continue;
            }

            if (!packetQueue.empty() && !decodedNearCapacity) {
                if (readAheadForEnhancementMatch()) {
                    continue;
                }
                if (!decodeNextCachedPacket()) {
                    break;
                }
                continue;
            }

            if (decodedNearCapacity && !seekPrerollWaiting) {
                if (audioPacketSinkActive && shouldReadPlayingPackets()) {
                    if (!sharedDemuxCapacityReadAheadLogged) {
                        sharedDemuxCapacityReadAheadLogged = true;
                        LogThread(LogLevel::Debug,
                                  L"decoder",
                                  L"shared_demux read_ahead while video_queue_full");
                    }
                    readAheadPackets(elOverlay ? kDolbyVisionEnhancementPlayingPacketReadAheadBatch
                                               : kPlayingPacketReadAheadBatch);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                continue;
            }

            if (paused) {
                readAheadPackets(elOverlay ? kDolbyVisionEnhancementPausedPacketReadAheadBatch
                                           : kPausedPacketReadAheadBatch);
            } else if (!hasPreroll) {
                const bool seekFastResume = seekFastResumeFramesRemaining_.load() > 0;
                readAheadPackets(seekFastResume
                                     ? kSeekStartupPacketReadAheadBatch
                                     : (elOverlay ? kDolbyVisionEnhancementStartupPacketReadAheadBatch
                                                  : kStartupPacketReadAheadBatch));
            } else if (shouldReadPlayingPackets()) {
                readAheadPackets(elOverlay ? kDolbyVisionEnhancementPlayingPacketReadAheadBatch
                                           : (decodedNearCapacity ? 1 : kPlayingPacketReadAheadBatch));
            }
            if (HasPendingSeek()) {
                continue;
            }

            if (!packetQueue.empty() && !decodedNearCapacity) {
                if (readAheadForEnhancementMatch()) {
                    continue;
                }
                if (!decodeNextCachedPacket()) {
                    break;
                }
                continue;
            }

            if (inputEof) {
                if (packetBudgetExceeded) {
                    NotifyDecodeFailure(L"Video packet exceeds the decoder's bounded memory budget");
                    break;
                }
                if (networkSource && !stopping_.load() && !HasPendingSeek()) {
                    std::chrono::milliseconds eofPosition{0};
                    {
                        std::scoped_lock lock(mutex_);
                        stats_.buffering = false;
                        stats_.clockPosition = latestFrame_.HasContent() ? latestFrame_.pts : startPosition_;
                        eofPosition = stats_.clockPosition;
                        UpdateBufferedStatsLocked();
                    }
                    LogThread(LogLevel::Warning,
                              L"decoder",
                              L"network eof while playing; holding clock at " +
                                  anvil::playback::FormatTimecode(eofPosition));
                    NotifyDecodeFailure(L"Network stream ended before playback completed at " +
                                        anvil::playback::FormatTimecode(eofPosition));
                    break;
                }
                // EOF or error: drain decoder then stop.
                if (audioPacketSinkActive && audioPacketSink_.endOfStream) {
                    audioPacketSink_.endOfStream();
                }
                if (enhancementCodecCtx && enhancementFrame && dolbyVisionEnhancementActive_) {
                    avcodec_send_packet(enhancementCodecCtx, nullptr);
                    ReceiveDolbyVisionEnhancementFrames(enhancementCodecCtx, enhancementFrame, enhancementTimeBase);
                }
                avcodec_send_packet(codecCtx, nullptr);
                if (DrainDecoder(codecCtx, swsCtx, frame, softwareFrame, streamTimeBase, serial)) {
                    // swsCtx may have been allocated inside DrainDecoder.
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        clearPacketQueue();
        (void)firstPacketSeen;
    } while (false);

    pendingSeekInterruptsEnabled_.store(false);
    ioInterruptAfterSteadyMs_.store(0);
    if (swsCtx) sws_freeContext(swsCtx);
    if (frame) av_frame_free(&frame);
    if (softwareFrame) av_frame_free(&softwareFrame);
    if (enhancementFrame) av_frame_free(&enhancementFrame);
    if (packet) av_packet_free(&packet);
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    if (subtitleCodecCtx) avcodec_free_context(&subtitleCodecCtx);
    if (enhancementCodecCtx) avcodec_free_context(&enhancementCodecCtx);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (formatCtx) avformat_close_input(&formatCtx);
    doviLibplaceboFilter_.reset();
    DrainQueuedFrames();
    running_.store(false);
    WakeScheduler();
    if (!startupComplete && !startupFailure.empty()) {
        NotifyDecodeFailure(startupFailure);
    }
}

bool FfmpegVideoDecoder::OpenVideoDecoder(const AVCodec* codec,
                                          const AVCodecParameters* codecpar,
                                          AVCodecContext*& codecCtx,
                                          AVBufferRef*& hwDeviceCtx) {
    bool hardwareConfigured = false;
    codecCtx = AllocateVideoCodecContext(codec, codecpar);
    if (!codecCtx) {
        return false;
    }

    if (preferHardwareDecode_ && !dolbyVisionStream_ && !enableDolbyVisionEnhancementDecode_) {
        hardwareConfigured = ConfigureD3D11VA(codec, codecCtx, hwDeviceCtx);
    } else {
        SetDecodeBackend(L"ffmpeg_software", false, {});
        LogThread(LogLevel::Info, L"decoder",
                  L"selected=ffmpeg_software reason=" +
                      std::wstring(dolbyVisionStream_
                                       ? L"dolby_vision_software_decode_for_reshape"
                                       : (enableDolbyVisionEnhancementDecode_
                                              ? L"dolby_vision_el_overlay_requires_software_decode"
                                              : L"hardware_decode_not_requested")));
    }

    int openError = avcodec_open2(codecCtx, codec, nullptr);
    if (openError >= 0) {
        if (hardwareConfigured) {
            SetDecodeBackend(L"ffmpeg_d3d11va", true, {});
            LogThread(LogLevel::Info, L"decoder", L"selected=ffmpeg_d3d11va handoff=zero_copy_or_cpu_transfer");
        }
        return true;
    }

    if (!hardwareConfigured) {
        LogThreadError(L"avcodec_open2 failed: " + FfmpegErrorString(openError));
        avcodec_free_context(&codecCtx);
        return false;
    }

    const std::wstring reason = L"d3d11va_open_failed:" + FfmpegErrorString(openError);
    LogThread(LogLevel::Warning, L"decoder", L"fallback=ffmpeg_software reason=" + reason);
    SetDecodeBackend(L"ffmpeg_software", false, reason);
    avcodec_free_context(&codecCtx);
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
    }
    hardwarePixelFormat_ = AV_PIX_FMT_NONE;
    hardwareDecodeActive_ = false;
    hardwareFormatLogged_ = false;
    zeroCopyFallbackLogged_ = false;

    codecCtx = AllocateVideoCodecContext(codec, codecpar);
    if (!codecCtx) {
        return false;
    }
    openError = avcodec_open2(codecCtx, codec, nullptr);
    if (openError < 0) {
        LogThreadError(L"software avcodec_open2 failed: " + FfmpegErrorString(openError));
        avcodec_free_context(&codecCtx);
        return false;
    }
    LogThread(LogLevel::Info, L"decoder", L"selected=ffmpeg_software fallback_from=d3d11va");
    return true;
}

bool FfmpegVideoDecoder::OpenDolbyVisionEnhancementDecoder(AVFormatContext* formatCtx,
                                                           const int primaryStreamIndex,
                                                           AVCodecContext*& codecCtx,
                                                           AVRational& timeBase) {
    codecCtx = nullptr;
    timeBase = AVRational{1, 1};
    dolbyVisionEnhancementActive_ = false;
    dolbyVisionEnhancementStreamIndex_ = -1;
    dolbyVisionEnhancementProfile_ = 0;
    dolbyVisionEnhancementLevel_ = 0;
    dolbyVisionEnhancementCompatId_ = 0;
    dolbyVisionEnhancementElPresent_ = false;
    dolbyVisionEnhancementBlPresent_ = false;

    if (!formatCtx) {
        return false;
    }

    AVStream* enhancementStream = nullptr;
    for (unsigned int index = 0; index < formatCtx->nb_streams; ++index) {
        AVStream* stream = formatCtx->streams[index];
        if (!stream || !stream->codecpar ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            stream->index == primaryStreamIndex ||
            !IsDolbyVisionEnhancementLayer(stream->codecpar)) {
            continue;
        }
        enhancementStream = stream;
        break;
    }

    if (!enhancementStream || !enhancementStream->codecpar) {
        LogThread(LogLevel::Info, L"decoder", L"dolby_vision_el unavailable reason=no_el_only_stream");
        return false;
    }

    AVCodecParameters* codecpar = enhancementStream->codecpar;
    const auto* dovi = DolbyVisionConfig(codecpar);
    if (!dovi) {
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_el unavailable reason=decoder_missing codec=" +
                      Utf8ToWide(avcodec_get_name(codecpar->codec_id)));
        return false;
    }

    AVCodecContext* context = AllocateVideoCodecContext(codec, codecpar);
    if (!context) {
        LogThread(LogLevel::Warning, L"decoder", L"dolby_vision_el unavailable reason=context_alloc_failed");
        return false;
    }
    context->pkt_timebase = enhancementStream->time_base;

    const int openError = avcodec_open2(context, codec, nullptr);
    if (openError < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_el unavailable reason=open_failed detail=" + FfmpegErrorString(openError));
        avcodec_free_context(&context);
        return false;
    }

    codecCtx = context;
    timeBase = enhancementStream->time_base;
    dolbyVisionEnhancementActive_ = true;
    dolbyVisionEnhancementStreamIndex_ = enhancementStream->index;
    dolbyVisionEnhancementProfile_ = dovi->dv_profile;
    dolbyVisionEnhancementLevel_ = dovi->dv_level;
    dolbyVisionEnhancementCompatId_ = dovi->dv_bl_signal_compatibility_id;
    dolbyVisionEnhancementElPresent_ = dovi->el_present_flag != 0;
    dolbyVisionEnhancementBlPresent_ = dovi->bl_present_flag != 0;

    LogThread(LogLevel::Info,
              L"decoder",
              L"dolby_vision_el active " + VideoStreamDescription(enhancementStream) +
                  L" level=" + std::to_wstring(dovi->dv_level) +
                  L" rpu=" + std::to_wstring(dovi->rpu_present_flag) +
                  L" compat_id=" + std::to_wstring(dovi->dv_bl_signal_compatibility_id));
    return true;
}

bool FfmpegVideoDecoder::EnsureAssSubtitleRenderer() {
    if (!subtitleAssRenderer_) {
        subtitleAssRenderer_ = std::make_unique<LibassSubtitleRenderer>(logSink_);
    }
    if (subtitleAssRenderer_->IsAvailable()) {
        return true;
    }
    return subtitleAssRenderer_->Initialize(std::filesystem::current_path());
}

void FfmpegVideoDecoder::AddAssFontAttachments(AVFormatContext* formatCtx) {
    if (!formatCtx || !subtitleAssRenderer_ || !subtitleAssRenderer_->IsAvailable()) {
        return;
    }

    auto hasSuffix = [](const std::wstring& value, const std::wstring& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    int added = 0;
    for (unsigned int index = 0; index < formatCtx->nb_streams; ++index) {
        const AVStream* stream = formatCtx->streams[index];
        const AVCodecParameters* parameters = stream ? stream->codecpar : nullptr;
        if (!stream || !parameters ||
            parameters->codec_type != AVMEDIA_TYPE_ATTACHMENT ||
            !parameters->extradata ||
            parameters->extradata_size <= 0) {
            continue;
        }

        const AVDictionaryEntry* filename = av_dict_get(stream->metadata, "filename", nullptr, 0);
        const AVDictionaryEntry* mimetype = av_dict_get(stream->metadata, "mimetype", nullptr, 0);
        const std::wstring filenameWide = filename ? Utf8ToWide(filename->value) : L"attachment-" + std::to_wstring(index);
        const std::wstring lowerFilename = ToLowerWide(filenameWide);
        const std::wstring lowerMime = mimetype ? ToLowerWide(Utf8ToWide(mimetype->value)) : L"";
        const bool looksLikeFont =
            hasSuffix(lowerFilename, L".ttf") ||
            hasSuffix(lowerFilename, L".otf") ||
            hasSuffix(lowerFilename, L".ttc") ||
            hasSuffix(lowerFilename, L".otc") ||
            lowerMime.find(L"font") != std::wstring::npos ||
            lowerMime.find(L"opentype") != std::wstring::npos ||
            lowerMime.find(L"truetype") != std::wstring::npos;
        if (!looksLikeFont) {
            continue;
        }

        if (subtitleAssRenderer_->AddFont(WideToUtf8(filenameWide),
                                          parameters->extradata,
                                          parameters->extradata_size)) {
            ++added;
        }
    }

    if (added > 0) {
        LogThread(LogLevel::Info, L"subtitle", L"libass_fonts attachments=" + std::to_wstring(added));
    }
}

bool FfmpegVideoDecoder::ConfigureAssSubtitleStream(AVFormatContext* formatCtx, const AVStream* stream) {
    const AVCodecParameters* parameters = stream ? stream->codecpar : nullptr;
    if (!stream || !parameters || !IsAssSubtitleCodec(parameters->codec_id)) {
        return false;
    }
    if (!EnsureAssSubtitleRenderer()) {
        return false;
    }

    AddAssFontAttachments(formatCtx);
    const uint8_t* extradata = parameters->extradata_size > 0 ? parameters->extradata : nullptr;
    const int extradataSize = parameters->extradata_size > 0 ? parameters->extradata_size : 0;
    if (!subtitleAssRenderer_->ConfigureTrackFromCodecPrivate(extradata, extradataSize)) {
        return false;
    }

    subtitleAssActive_ = true;
    subtitleAssExternalFullTrack_ = false;
    LogThread(LogLevel::Info,
              L"subtitle",
              L"libass stream=embedded codec=" + Utf8ToWide(avcodec_get_name(parameters->codec_id)) +
                  L" stream=" + std::to_wstring(stream->index));
    return true;
}

bool FfmpegVideoDecoder::OpenSubtitleDecoder(AVFormatContext* formatCtx,
                                             int& subtitleStreamIndex,
                                             AVRational& subtitleTimeBase,
                                             AVCodecContext*& subtitleCodecCtx) {
    subtitleStreamIndex = -1;
    subtitleTimeBase = AVRational{1, 1};
    subtitleCodecCtx = nullptr;
    if (!formatCtx) {
        return false;
    }

    auto openStream = [&](const int streamIndex) -> bool {
        if (streamIndex < 0 || static_cast<unsigned int>(streamIndex) >= formatCtx->nb_streams) {
            return false;
        }
        AVStream* stream = formatCtx->streams[streamIndex];
        AVCodecParameters* parameters = stream ? stream->codecpar : nullptr;
        if (!stream || !parameters || parameters->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            return false;
        }
        const bool assStream = ConfigureAssSubtitleStream(formatCtx, stream);
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) {
            if (assStream) {
                subtitleStreamIndex = streamIndex;
                subtitleTimeBase = stream->time_base;
                subtitleCodecCtx = nullptr;
                LogThread(LogLevel::Info,
                          L"subtitle",
                          L"selected stream=" + std::to_wstring(streamIndex) +
                              L" codec=" + Utf8ToWide(avcodec_get_name(parameters->codec_id)) +
                              L" language=" + (StreamLanguage(stream).empty() ? L"-" : StreamLanguage(stream)) +
                              L" renderer=libass decoder=raw_packet");
                return true;
            }
            return false;
        }
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (!context) {
            if (assStream && subtitleAssRenderer_) {
                subtitleAssActive_ = false;
                subtitleAssRenderer_->ResetTrack();
            }
            return false;
        }
        const int paramsError = avcodec_parameters_to_context(context, parameters);
        if (paramsError < 0) {
            if (assStream && subtitleAssRenderer_) {
                subtitleAssActive_ = false;
                subtitleAssRenderer_->ResetTrack();
            }
            avcodec_free_context(&context);
            return false;
        }
        context->pkt_timebase = stream->time_base;

        const int openError = avcodec_open2(context, codec, nullptr);
        if (openError < 0) {
            LogThread(LogLevel::Warning,
                      L"subtitle",
                      L"decoder_open_failed stream=" + std::to_wstring(streamIndex) +
                          L" codec=" + Utf8ToWide(avcodec_get_name(parameters->codec_id)) +
                      L" reason=" + FfmpegErrorString(openError));
            if (assStream && subtitleAssRenderer_) {
                subtitleAssActive_ = false;
                subtitleAssRenderer_->ResetTrack();
            }
            avcodec_free_context(&context);
            return false;
        }

        subtitleStreamIndex = streamIndex;
        subtitleTimeBase = stream->time_base;
        subtitleCodecCtx = context;
        LogThread(LogLevel::Info,
                  L"subtitle",
                  L"selected stream=" + std::to_wstring(streamIndex) +
                      L" codec=" + Utf8ToWide(avcodec_get_name(parameters->codec_id)) +
                      L" language=" + (StreamLanguage(stream).empty() ? L"-" : StreamLanguage(stream)) +
                      (assStream ? L" renderer=libass" : L""));
        return true;
    };

    if (selectedSubtitleTrackIndex_ >= 0) {
        if (openStream(selectedSubtitleTrackIndex_)) {
            return true;
        }
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"selected_stream_unavailable stream=" + std::to_wstring(selectedSubtitleTrackIndex_));
        return false;
    }

    const bool hasLanguagePreference = !IsAutoLanguage(preferredSubtitleLanguage_);
    if (hasLanguagePreference) {
        for (unsigned int index = 0; index < formatCtx->nb_streams; ++index) {
            const AVStream* stream = formatCtx->streams[index];
            if (stream && stream->codecpar &&
                stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
                LanguageMatches(preferredSubtitleLanguage_, StreamLanguage(stream)) &&
                openStream(static_cast<int>(index))) {
                return true;
            }
        }
    }

    for (unsigned int index = 0; index < formatCtx->nb_streams; ++index) {
        const AVStream* stream = formatCtx->streams[index];
        if (stream && stream->codecpar &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            (stream->disposition & AV_DISPOSITION_DEFAULT) != 0 &&
            openStream(static_cast<int>(index))) {
            return true;
        }
    }

    for (unsigned int index = 0; index < formatCtx->nb_streams; ++index) {
        const AVStream* stream = formatCtx->streams[index];
        if (stream && stream->codecpar &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            openStream(static_cast<int>(index))) {
            return true;
        }
    }

    LogThread(LogLevel::Debug, L"subtitle", L"selected=none");
    return false;
}

bool FfmpegVideoDecoder::DecodeExternalAssSubtitleFile(const std::filesystem::path& subtitlePath) {
    if (!IsAssSubtitleExtension(subtitlePath) || !EnsureAssSubtitleRenderer()) {
        return false;
    }

    std::ifstream file(subtitlePath, std::ios::binary | std::ios::ate);
    if (!file) {
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"external_ass_open_failed file=" + subtitlePath.filename().wstring());
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size <= 0) {
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"external_ass_empty file=" + subtitlePath.filename().wstring());
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"external_ass_read_failed file=" + subtitlePath.filename().wstring());
        return false;
    }

    if (!subtitleAssRenderer_->ConfigureTrackFromMemory(bytes.data(), bytes.size())) {
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"external_ass_parse_failed file=" + subtitlePath.filename().wstring());
        return false;
    }

    subtitleAssActive_ = true;
    subtitleAssExternalFullTrack_ = true;
    subtitleCues_.clear();
    LogThread(LogLevel::Info,
              L"subtitle",
              L"libass external file=" + subtitlePath.filename().wstring() +
                  L" bytes=" + std::to_wstring(bytes.size()));
    return true;
}

bool FfmpegVideoDecoder::DecodeExternalSubtitleFile(const std::filesystem::path& subtitlePath) {
    if (DecodeExternalAssSubtitleFile(subtitlePath)) {
        return true;
    }

    AVFormatContext* subtitleFormatCtx = nullptr;
    AVCodecContext* subtitleCodecCtx = nullptr;
    AVPacket* packet = nullptr;
    int subtitleStreamIndex = -1;
    AVRational subtitleTimeBase{1, 1};
    const std::size_t cueCountBefore = subtitleCues_.size();

    const std::string pathUtf8 = WideToUtf8(subtitlePath.wstring());
    subtitleFormatCtx = avformat_alloc_context();
    if (!subtitleFormatCtx) {
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"external_alloc_failed file=" + subtitlePath.filename().wstring());
        return false;
    }
    subtitleFormatCtx->interrupt_callback.callback = &FfmpegVideoDecoder::InterruptCallback;
    subtitleFormatCtx->interrupt_callback.opaque = this;
    int error = avformat_open_input(&subtitleFormatCtx, pathUtf8.c_str(), nullptr, nullptr);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"subtitle",
                  L"external_open_failed file=" + subtitlePath.filename().wstring() +
                      L" reason=" + FfmpegErrorString(error));
        avformat_close_input(&subtitleFormatCtx);
        return false;
    }

    bool loaded = false;
    do {
        error = avformat_find_stream_info(subtitleFormatCtx, nullptr);
        if (error < 0) {
            LogThread(LogLevel::Warning,
                      L"subtitle",
                      L"external_stream_info_failed file=" + subtitlePath.filename().wstring() +
                          L" reason=" + FfmpegErrorString(error));
            break;
        }

        if (!OpenSubtitleDecoder(subtitleFormatCtx, subtitleStreamIndex, subtitleTimeBase, subtitleCodecCtx)) {
            LogThread(LogLevel::Warning,
                      L"subtitle",
                      L"external_decoder_unavailable file=" + subtitlePath.filename().wstring());
            break;
        }

        packet = av_packet_alloc();
        if (!packet) {
            LogThread(LogLevel::Warning, L"subtitle", L"external_packet_alloc_failed");
            break;
        }

        while (!stopping_.load() && av_read_frame(subtitleFormatCtx, packet) >= 0) {
            if (packet->stream_index == subtitleStreamIndex) {
                DecodeSubtitlePacket(subtitleCodecCtx, packet, subtitleTimeBase);
            }
            av_packet_unref(packet);
        }

        std::sort(subtitleCues_.begin(), subtitleCues_.end(), [](const NativeSubtitleCue& lhs, const NativeSubtitleCue& rhs) {
            if (lhs.start != rhs.start) {
                return lhs.start < rhs.start;
            }
            return lhs.end < rhs.end;
        });
        loaded = true;
    } while (false);

    if (packet) {
        av_packet_free(&packet);
    }
    if (subtitleCodecCtx) {
        avcodec_free_context(&subtitleCodecCtx);
    }
    if (subtitleFormatCtx) {
        avformat_close_input(&subtitleFormatCtx);
    }

    if (loaded) {
        LogThread(LogLevel::Info,
                  L"subtitle",
                  L"external file=" + subtitlePath.filename().wstring() +
                      L" cues=" + std::to_wstring(subtitleCues_.size() - cueCountBefore));
    }
    return loaded;
}

AVCodecContext* FfmpegVideoDecoder::AllocateVideoCodecContext(const AVCodec* codec, const AVCodecParameters* codecpar) const {
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        LogThreadError(L"avcodec_alloc_context3 failed");
        return nullptr;
    }
    const int paramsError = avcodec_parameters_to_context(context, codecpar);
    if (paramsError < 0) {
        LogThreadError(L"avcodec_parameters_to_context failed: " + FfmpegErrorString(paramsError));
        avcodec_free_context(&context);
        return nullptr;
    }
    context->thread_count = 0;
    context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    return context;
}

bool FfmpegVideoDecoder::ConfigureD3D11VA(const AVCodec* codec, AVCodecContext* codecCtx, AVBufferRef*& hwDeviceCtx) {
    const AVCodecHWConfig* selectedConfig = nullptr;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
        if (!config) {
            break;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
            selectedConfig = config;
            break;
        }
    }

    if (!selectedConfig) {
        const std::wstring reason = L"codec_has_no_d3d11va_hw_device_config";
        SetDecodeBackend(L"ffmpeg_software", false, reason);
        LogThread(LogLevel::Warning, L"decoder", L"fallback=ffmpeg_software reason=" + reason);
        return false;
    }

    AVBufferRef* device = nullptr;
    std::wstring deviceMode;
    const int deviceError = CreateD3D11VADeviceContext(&device, deviceMode);
    if (deviceError < 0 || !device) {
        const std::wstring reason = L"d3d11va_device_create_failed:" + FfmpegErrorString(deviceError);
        SetDecodeBackend(L"ffmpeg_software", false, reason);
        LogThread(LogLevel::Warning, L"decoder", L"fallback=ffmpeg_software reason=" + reason);
        return false;
    }

    codecCtx->hw_device_ctx = av_buffer_ref(device);
    if (!codecCtx->hw_device_ctx) {
        const std::wstring reason = L"av_buffer_ref_hw_device_failed";
        av_buffer_unref(&device);
        SetDecodeBackend(L"ffmpeg_software", false, reason);
        LogThread(LogLevel::Warning, L"decoder", L"fallback=ffmpeg_software reason=" + reason);
        return false;
    }

    hwDeviceCtx = device;
    hardwarePixelFormat_ = selectedConfig->pix_fmt;
    hardwareDecodeActive_ = false;
    hardwareFormatLogged_ = false;
    codecCtx->opaque = this;
    codecCtx->get_format = &FfmpegVideoDecoder::ChooseHardwarePixelFormat;
    LogThread(LogLevel::Info,
              L"decoder",
              L"d3d11va candidate pix_fmt=" + PixelFormatName(hardwarePixelFormat_) +
                  L" device=" + deviceMode);
    return true;
}

int FfmpegVideoDecoder::CreateD3D11VADeviceContext(AVBufferRef** device, std::wstring& deviceMode) {
    if (!device) {
        return AVERROR(EINVAL);
    }
    *device = nullptr;

    if (!sharedD3DDevice_) {
        deviceMode = L"ffmpeg_owned";
        return av_hwdevice_ctx_create(device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    }

    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) {
        return AVERROR(ENOMEM);
    }

    auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* d3dctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    d3dctx->device = sharedD3DDevice_.Get();
    d3dctx->device->AddRef();
    d3dctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    const int initError = av_hwdevice_ctx_init(ref);
    if (initError < 0) {
        av_buffer_unref(&ref);
        return initError;
    }

    *device = ref;
    deviceMode = L"renderer_shared";
    return 0;
}

AVPixelFormat FfmpegVideoDecoder::ChooseHardwarePixelFormat(AVCodecContext* codecCtx, const AVPixelFormat* pixelFormats) {
    auto* self = static_cast<FfmpegVideoDecoder*>(codecCtx ? codecCtx->opaque : nullptr);
    if (!self) {
        return avcodec_default_get_format(codecCtx, pixelFormats);
    }

    for (const AVPixelFormat* format = pixelFormats; format && *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == self->hardwarePixelFormat_) {
            self->hardwareDecodeActive_ = true;
            if (!self->hardwareFormatLogged_) {
                self->hardwareFormatLogged_ = true;
                self->LogThread(LogLevel::Info,
                                L"decoder",
                                L"d3d11va format selected=" + PixelFormatName(*format));
            }
            return *format;
        }
    }

    self->hardwareDecodeActive_ = false;
    self->SetDecodeBackend(L"ffmpeg_software", false, L"d3d11va_pix_fmt_not_offered");
    self->LogThread(LogLevel::Warning,
                    L"decoder",
                    L"fallback=ffmpeg_software reason=d3d11va_pix_fmt_not_offered");
    return avcodec_default_get_format(codecCtx, pixelFormats);
}

// Returns true if at least one frame was published.
bool FfmpegVideoDecoder::ReceiveFrames(AVCodecContext* codecCtx, SwsContext*& swsCtx, AVFrame* frame, AVFrame* softwareFrame,
                                       const AVRational timeBase, uint64_t& serial) {
    while (!stopping_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN)) {
            if (receiveEagainLogCount_ < 3) {
                ++receiveEagainLogCount_;
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"receive_frame status=eagain count=" + std::to_wstring(receiveEagainLogCount_));
            }
            return true;
        }
        if (ret == AVERROR_EOF) {
            if (!receiveEofLogged_) {
                receiveEofLogged_ = true;
                LogThread(LogLevel::Debug, L"decoder", L"receive_frame status=eof");
            }
            return true;
        }
        if (ret < 0) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"receive_frame_failed reason=" + FfmpegErrorString(ret));
            return false;
        }
        if (!firstDecodedFrameLogged_) {
            firstDecodedFrameLogged_ = true;
            LogThread(LogLevel::Info,
                      L"decoder",
                      L"first_decoded_frame format=" +
                          PixelFormatName(static_cast<AVPixelFormat>(frame->format)) +
                          L" sw_format=" + PixelFormatName(HardwareFrameSoftwareFormat(frame)) +
                          L" size=" + std::to_wstring(frame->width) + L"x" + std::to_wstring(frame->height) +
                          L" pts_ms=" + std::to_wstring(FramePts(frame, timeBase).count()));
        }
        if (!PublishFrame(frame, softwareFrame, swsCtx, timeBase, serial)) {
            av_frame_unref(frame);
            return false;
        }
        av_frame_unref(frame);
        if (playbackPaused_.load()) {
            return true;
        }
    }
    return true;
}

// Drain decoder after EOF (no more packets).
bool FfmpegVideoDecoder::DrainDecoder(AVCodecContext* codecCtx, SwsContext*& swsCtx, AVFrame* frame, AVFrame* softwareFrame,
                                      const AVRational timeBase, uint64_t& serial) {
    while (!stopping_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"drain_receive_failed reason=" + FfmpegErrorString(ret));
            return false;
        }
        if (!PublishFrame(frame, softwareFrame, swsCtx, timeBase, serial)) {
            av_frame_unref(frame);
            return false;
        }
        av_frame_unref(frame);
    }
    return true;
}

bool FfmpegVideoDecoder::DecodeDolbyVisionEnhancementPacket(AVCodecContext* codecCtx,
                                                            const AVPacket* packet,
                                                            AVFrame* frame,
                                                            const AVRational timeBase) {
    if (!codecCtx || !packet || !frame || !dolbyVisionEnhancementActive_) {
        return true;
    }

    int eagainCount = 0;
    while (!stopping_.load() && !HasPendingSeek()) {
        const int sendResult = avcodec_send_packet(codecCtx, packet);
        if (sendResult == AVERROR(EAGAIN)) {
            if (!ReceiveDolbyVisionEnhancementFrames(codecCtx, frame, timeBase)) {
                return true;
            }
            if (++eagainCount > 8) {
                return true;
            }
            continue;
        }
        if (sendResult < 0) {
            if (!dolbyVisionEnhancementFailureLogged_) {
                dolbyVisionEnhancementFailureLogged_ = true;
                LogThread(LogLevel::Warning,
                          L"decoder",
                          L"dolby_vision_el disabled reason=send_packet_failed detail=" +
                              FfmpegErrorString(sendResult));
            }
            dolbyVisionEnhancementActive_ = false;
            return true;
        }
        break;
    }

    ReceiveDolbyVisionEnhancementFrames(codecCtx, frame, timeBase);
    return true;
}

bool FfmpegVideoDecoder::ReceiveDolbyVisionEnhancementFrames(AVCodecContext* codecCtx,
                                                             AVFrame* frame,
                                                             const AVRational timeBase) {
    if (!codecCtx || !frame || !dolbyVisionEnhancementActive_) {
        return true;
    }

    while (!stopping_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            if (!dolbyVisionEnhancementFailureLogged_) {
                dolbyVisionEnhancementFailureLogged_ = true;
                LogThread(LogLevel::Warning,
                          L"decoder",
                          L"dolby_vision_el disabled reason=receive_frame_failed detail=" +
                              FfmpegErrorString(ret));
            }
            dolbyVisionEnhancementActive_ = false;
            return true;
        }

        const auto pts = FramePts(frame, timeBase);
        auto metadata = ExtractEnhancementDolbyVisionMetadata(frame);
        if (metadata && metadata->valid) {
            latestDolbyVisionEnhancementMetadata_ = metadata;
            latestDolbyVisionEnhancementMetadataPts_ = pts;
            const bool metadataChanged =
                !dolbyVisionEnhancementDynamicMetadataLogged_ ||
                metadata->dynamicMetadataFingerprint != dolbyVisionEnhancementLastDynamicMetadataFingerprint_;
            if (metadataChanged) {
                LogThread(dolbyVisionEnhancementDynamicMetadataLogged_ ? LogLevel::Debug : LogLevel::Info,
                          L"decoder",
                          L"dolby_vision_dynamic_metadata path=enhancement_layer " +
                              DoviDynamicLogSummary(*metadata));
                dolbyVisionEnhancementDynamicMetadataLogged_ = true;
                dolbyVisionEnhancementLastDynamicMetadataFingerprint_ = metadata->dynamicMetadataFingerprint;
            }
        } else if (!dolbyVisionEnhancementDynamicMetadataLogged_) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"dolby_vision_dynamic_metadata path=enhancement_layer frame_side_data=missing");
            dolbyVisionEnhancementDynamicMetadataLogged_ = true;
        }

        NativeYuvPlanes enhancementYuv;
        const auto packStart = std::chrono::steady_clock::now();
        if (PackYuv420P10FrameToP010(frame, enhancementYuv, kEnhancementMemoryBudgetBytes)) {
            const std::size_t incomingBytes = BufferCapacityBytes(enhancementYuv.data);
            std::size_t queuedBytes = 0;
            for (const auto& queuedFrame : dolbyVisionEnhancementFrames_) {
                queuedBytes = SaturatingAddBytes(queuedBytes, BufferCapacityBytes(queuedFrame.yuv.data));
            }
            const bool queueWouldOverflow =
                dolbyVisionEnhancementFrames_.size() >= kMaxDolbyVisionEnhancementQueuedFrames ||
                !FitsWithinBudget(queuedBytes, incomingBytes, kMaxDolbyVisionEnhancementQueuedBytes);
            // Before the first BL/EL match, keep the oldest EL window. The BL
            // decoder can start later than the EL decoder after a keyframe
            // seek; sliding this queue forward used to discard EL PTS 0 (or
            // the seek target) before the first BL frame was produced.
            const bool preserveInitialWindow =
                queueWouldOverflow &&
                !dolbyVisionEnhancementOverlayLogged_ &&
                !pendingDolbyVisionBaseFrame_.has_value() &&
                !dolbyVisionEnhancementFrames_.empty();
            if (!preserveInitialWindow) {
                while (!dolbyVisionEnhancementFrames_.empty() &&
                       (dolbyVisionEnhancementFrames_.size() >= kMaxDolbyVisionEnhancementQueuedFrames ||
                        !FitsWithinBudget(queuedBytes, incomingBytes, kMaxDolbyVisionEnhancementQueuedBytes))) {
                    const std::size_t removedBytes =
                        BufferCapacityBytes(dolbyVisionEnhancementFrames_.front().yuv.data);
                    queuedBytes = removedBytes <= queuedBytes ? queuedBytes - removedBytes : 0;
                    dolbyVisionEnhancementFrames_.pop_front();
                }
            } else if (!dolbyVisionEnhancementStartupFallbackLogged_) {
                dolbyVisionEnhancementStartupFallbackLogged_ = true;
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"dolby_vision_el_overlay hold_initial_window frames=" +
                              std::to_wstring(dolbyVisionEnhancementFrames_.size()));
            }
            if (preserveInitialWindow) {
                // Drop the newest EL frame only until the first BL frame is
                // available. Once a BL is pending, the queue slides toward its
                // exact PTS and each new EL frame is matched immediately.
            } else if (!FitsWithinBudget(queuedBytes, incomingBytes, kMaxDolbyVisionEnhancementQueuedBytes)) {
                if (!dolbyVisionEnhancementFailureLogged_) {
                    dolbyVisionEnhancementFailureLogged_ = true;
                    LogThread(LogLevel::Warning,
                              L"decoder",
                              L"dolby_vision_el_overlay unavailable reason=frame_exceeds_memory_budget bytes=" +
                                  std::to_wstring(incomingBytes));
                }
                av_frame_unref(frame);
                continue;
            }
            DolbyVisionEnhancementFrame queued;
            queued.pts = pts;
            queued.yuv = std::move(enhancementYuv);
                queued.dovi = (metadata && metadata->valid) ? metadata : latestDolbyVisionEnhancementMetadata_;
                queued.details = DoviFrameSummary(queued.dovi.get());
                dolbyVisionEnhancementFrames_.push_back(std::move(queued));
                TryPublishPendingDolbyVisionBaseFrame();
                if (!dolbyVisionEnhancementFirstPackedLogged_) {
                dolbyVisionEnhancementFirstPackedLogged_ = true;
                const auto packMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - packStart).count();
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"dolby_vision_el_packed bytes=" +
                              std::to_wstring(incomingBytes) +
                              L" pack_ms=" + std::to_wstring(packMs));
            }
        } else if (!dolbyVisionEnhancementFailureLogged_) {
            dolbyVisionEnhancementFailureLogged_ = true;
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"dolby_vision_el_overlay unavailable reason=unsupported_el_format format=" +
                          PixelFormatName(static_cast<AVPixelFormat>(frame->format)));
        }

        ++dolbyVisionEnhancementFramesDecoded_;
        if (!dolbyVisionEnhancementFirstFrameLogged_) {
            dolbyVisionEnhancementFirstFrameLogged_ = true;
            LogThread(LogLevel::Info,
                      L"decoder",
                      L"dolby_vision_el_frame w=" + std::to_wstring(frame->width) +
                          L" h=" + std::to_wstring(frame->height) +
                          L" format=" + PixelFormatName(static_cast<AVPixelFormat>(frame->format)) +
                          L" pts_ms=" + std::to_wstring(pts.count()) +
                          L" dovi=" + std::wstring(metadata && metadata->valid ? L"yes" : L"no") +
                          DoviResidualSummary(frame) +
                          L" merge=nlq_merge_pending");
        }

        av_frame_unref(frame);
    }
    return true;
}

bool FfmpegVideoDecoder::ShouldUsePrimaryDoviLibplacebo() const {
    if (!dolbyVisionStream_) {
        return false;
    }

    // Profiles 5 and 8 are the practical single-layer fallback targets:
    // libplacebo can consume the per-frame RPU and output display-ready SDR or
    // HDR10/PQ. Profile 7 with EL/FEL stays on the explicit fallback path until
    // BL+EL composition is implemented.
    const bool singleLayer = dolbyVisionBlPresent_ && !dolbyVisionElPresent_;
    return singleLayer && (dolbyVisionProfile_ == 5 || dolbyVisionProfile_ == 8);
}

bool FfmpegVideoDecoder::EnsureDoviLibplaceboFilter(AVFrame* frame, const AVRational timeBase) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    const int format = frame->format;
    const AVRational sampleAspect = frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0
                                        ? frame->sample_aspect_ratio
                                        : AVRational{1, 1};
    if (doviLibplaceboFilter_ &&
        doviLibplaceboFilter_->width == frame->width &&
        doviLibplaceboFilter_->height == frame->height &&
        doviLibplaceboFilter_->format == format &&
        doviLibplaceboFilter_->hdrOutput == preferDolbyVisionHdrOutput_ &&
        doviLibplaceboFilter_->timeBase.num == timeBase.num &&
        doviLibplaceboFilter_->timeBase.den == timeBase.den &&
        doviLibplaceboFilter_->sampleAspectRatio.num == sampleAspect.num &&
        doviLibplaceboFilter_->sampleAspectRatio.den == sampleAspect.den) {
        return true;
    }

    auto state = std::make_unique<DoviLibplaceboFilterState>();
    state->width = frame->width;
    state->height = frame->height;
    state->format = format;
    state->hdrOutput = preferDolbyVisionHdrOutput_;
    state->timeBase = timeBase;
    state->sampleAspectRatio = sampleAspect;
    state->graph = avfilter_graph_alloc();
    if (!state->graph) {
        LogThread(LogLevel::Warning, L"decoder", L"dolby_vision_libplacebo unavailable=graph_alloc_failed");
        return false;
    }

    const AVFilter* bufferSource = avfilter_get_by_name("buffer");
    const AVFilter* bufferSink = avfilter_get_by_name("buffersink");
    if (!bufferSource || !bufferSink) {
        LogThread(LogLevel::Warning, L"decoder", L"dolby_vision_libplacebo unavailable=missing_buffer_filters");
        return false;
    }

    char sourceArgs[512]{};
    std::snprintf(sourceArgs,
                  sizeof(sourceArgs),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                  frame->width,
                  frame->height,
                  format,
                  timeBase.num,
                  timeBase.den,
                  sampleAspect.num,
                  sampleAspect.den);

    int error = avfilter_graph_create_filter(&state->source, bufferSource, "in", sourceArgs, nullptr, state->graph);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_libplacebo unavailable=buffer_source_failed reason=" + FfmpegErrorString(error));
        return false;
    }

    AVBufferSrcParameters* params = av_buffersrc_parameters_alloc();
    if (!params) {
        LogThread(LogLevel::Warning, L"decoder", L"dolby_vision_libplacebo unavailable=buffer_params_alloc_failed");
        return false;
    }
    params->format = format;
    params->time_base = timeBase;
    params->width = frame->width;
    params->height = frame->height;
    params->sample_aspect_ratio = sampleAspect;
    params->color_space = frame->colorspace;
    params->color_range = frame->color_range;
    error = av_buffersrc_parameters_set(state->source, params);
    av_free(params);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_libplacebo unavailable=buffer_params_failed reason=" + FfmpegErrorString(error));
        return false;
    }

    error = avfilter_graph_create_filter(&state->sink, bufferSink, "out", nullptr, nullptr, state->graph);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_libplacebo unavailable=buffer_sink_failed reason=" + FfmpegErrorString(error));
        return false;
    }

    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        LogThread(LogLevel::Warning, L"decoder", L"dolby_vision_libplacebo unavailable=inout_alloc_failed");
        return false;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = state->source;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = state->sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    const char* filterDescription = preferDolbyVisionHdrOutput_
        ? "libplacebo="
          "apply_dolbyvision=1:"
          "colorspace=gbr:"
          "color_primaries=bt2020:"
          "color_trc=smpte2084:"
          "range=pc,"
          "format=x2bgr10le"
        : "libplacebo="
          "apply_dolbyvision=1:"
          "tonemapping=auto:"
          "gamut_mode=perceptual:"
          "peak_detect=1:"
          "colorspace=bt709:"
          "color_primaries=bt709:"
          "color_trc=bt709:"
          "range=pc,"
          "format=bgra";

    error = avfilter_graph_parse_ptr(state->graph, filterDescription, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_libplacebo unavailable=parse_failed reason=" + FfmpegErrorString(error));
        return false;
    }

    error = avfilter_graph_config(state->graph, nullptr);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_libplacebo unavailable=config_failed reason=" + FfmpegErrorString(error));
        return false;
    }

    doviLibplaceboFilter_ = std::move(state);
    LogThread(LogLevel::Info,
              L"decoder",
              std::wstring(L"dolby_vision_libplacebo active target=") +
                  (preferDolbyVisionHdrOutput_ ? L"bt2020_pq_hdr format=x2bgr10le"
                                                : L"bt709_sdr tonemapping=auto gamut=perceptual"));
    return true;
}

bool FfmpegVideoDecoder::TryPublishDoviLibplaceboFrame(AVFrame* frame,
                                                       const AVRational timeBase,
                                                       const std::chrono::milliseconds pts,
                                                       uint64_t& serial) {
    if (!frame || dolbyVisionLibplaceboFailed_) {
        return false;
    }
    if (!EnsureDoviLibplaceboFilter(frame, timeBase)) {
        dolbyVisionLibplaceboFailed_ = true;
        return false;
    }

    const auto inputDovi = ExtractFrameDolbyVisionMetadata(frame);
    if (inputDovi && inputDovi->valid) {
        const bool metadataChanged =
            !dolbyVisionDynamicMetadataLogged_ ||
            inputDovi->dynamicMetadataFingerprint != dolbyVisionLastDynamicMetadataFingerprint_;
        if (metadataChanged) {
            LogThread(dolbyVisionDynamicMetadataLogged_ ? LogLevel::Debug : LogLevel::Info,
                      L"decoder",
                      L"dolby_vision_dynamic_metadata path=libplacebo " +
                          DoviDynamicLogSummary(*inputDovi));
            dolbyVisionDynamicMetadataLogged_ = true;
            dolbyVisionLastDynamicMetadataFingerprint_ = inputDovi->dynamicMetadataFingerprint;
        }
    } else if (!dolbyVisionDynamicMetadataLogged_) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_dynamic_metadata path=libplacebo frame_side_data=missing");
        dolbyVisionDynamicMetadataLogged_ = true;
    }

    int error = av_buffersrc_add_frame_flags(doviLibplaceboFilter_->source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (error < 0) {
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_libplacebo disabled=push_failed reason=" + FfmpegErrorString(error));
        dolbyVisionLibplaceboFailed_ = true;
        return false;
    }

    bool produced = false;
    while (!stopping_.load() && !HasPendingSeek()) {
        AVFrame* filtered = av_frame_alloc();
        if (!filtered) {
            LogThread(LogLevel::Warning, L"decoder", L"dolby_vision_libplacebo disabled=filtered_frame_alloc_failed");
            dolbyVisionLibplaceboFailed_ = true;
            return false;
        }

        error = av_buffersink_get_frame(doviLibplaceboFilter_->sink, filtered);
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
            av_frame_free(&filtered);
            break;
        }
        if (error < 0) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"dolby_vision_libplacebo disabled=pull_failed reason=" + FfmpegErrorString(error));
            av_frame_free(&filtered);
            dolbyVisionLibplaceboFailed_ = true;
            return false;
        }

        produced = true;
        const int outW = filtered->width;
        const int outH = filtered->height;
        const auto outputFormat = static_cast<AVPixelFormat>(filtered->format);
        const bool hdrOutput = outputFormat == AV_PIX_FMT_X2BGR10LE;
        const bool sdrOutput = outputFormat == AV_PIX_FMT_BGRA;
        if (outW <= 0 || outH <= 0 || (!sdrOutput && !hdrOutput) || !filtered->data[0] || filtered->linesize[0] < outW * 4) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"dolby_vision_libplacebo disabled=unexpected_output format=" +
                          PixelFormatName(outputFormat));
            av_frame_free(&filtered);
            dolbyVisionLibplaceboFailed_ = true;
            return false;
        }

        const int stride = outW * 4;
        const std::size_t needed = static_cast<std::size_t>(stride) * static_cast<std::size_t>(outH);
        auto pixels = AcquireReusableBgraBuffer(needed);
        if (!pixels || pixels->size() < needed) {
            LogThreadError(L"BGRA frame buffer allocation failed");
            av_frame_free(&filtered);
            return false;
        }

        for (int row = 0; row < outH; ++row) {
            uint8_t* dstRow = pixels->data() + static_cast<std::size_t>(row) * stride;
            const uint8_t* srcRow = filtered->data[0] + static_cast<std::size_t>(row) * filtered->linesize[0];
            if (hdrOutput) {
                for (int x = 0; x < outW; ++x) {
                    uint32_t packed = 0;
                    std::memcpy(&packed, srcRow + static_cast<std::size_t>(x) * 4, sizeof(packed));
                    packed |= 0xC0000000u;  // x2bgr10le -> DXGI R10G10B10A2 with opaque alpha.
                    std::memcpy(dstRow + static_cast<std::size_t>(x) * 4, &packed, sizeof(packed));
                }
            } else {
                std::memcpy(dstRow, srcRow, stride);
            }
        }

        NativeVideoFrame queued;
        queued.width = outW;
        queued.height = outH;
        queued.stride = stride;
        queued.bgra = std::move(pixels);
        queued.softwareFormat = outputFormat;
        queued.color = hdrOutput ? HdrBt2020PqColorMetadata() : SdrBt709ColorMetadata();
        queued.dovi = inputDovi;
        queued.dynamicMetadataPath =
            inputDovi && inputDovi->valid && (inputDovi->dmLevel2Present || inputDovi->dmLevel3Present || inputDovi->dmLevel8Present)
                ? L"dolby_vision_libplacebo+trim"
                : L"dolby_vision_libplacebo";
        queued.dynamicMetadataDetails = DoviFrameSummary(inputDovi.get());
        queued.pts = pts;
        queued.serial = ++serial;
        queued.timelineSerial = CurrentTimelineSerial();
        RefreshFrameSubtitles(queued);

        if (!dolbyVisionLibplaceboFrameLogged_) {
            dolbyVisionLibplaceboFrameLogged_ = true;
            LogThread(LogLevel::Info,
                      L"decoder",
                      L"dolby_vision_libplacebo_frame w=" + std::to_wstring(outW) +
                          L" h=" + std::to_wstring(outH) +
                          L" output=" + (hdrOutput ? L"x2bgr10le_hdr_bt2020_pq" : L"bgra_sdr_bt709"));
        }

        av_frame_free(&filtered);
        const bool published = oneShotFrame_ ? PublishImmediateFrame(std::move(queued)) : EnqueueFrame(std::move(queued));
        if (!published) {
            return false;
        }
    }

    return produced || !dolbyVisionLibplaceboFailed_;
}

void FfmpegVideoDecoder::AttachDolbyVisionEnhancementFrame(NativeVideoFrame& frame,
                                                           const std::chrono::milliseconds pts) {
    if (!enableDolbyVisionEnhancementDecode_ ||
        !dolbyVisionEnhancementActive_ ||
        dolbyVisionEnhancementFrames_.empty()) {
        return;
    }

    auto absDelta = [](const std::chrono::milliseconds lhs, const std::chrono::milliseconds rhs) {
        return lhs >= rhs ? lhs - rhs : rhs - lhs;
    };

    std::size_t bestIndex = 0;
    auto bestDelta = absDelta(dolbyVisionEnhancementFrames_.front().pts, pts);
    for (std::size_t index = 1; index < dolbyVisionEnhancementFrames_.size(); ++index) {
        const auto delta = absDelta(dolbyVisionEnhancementFrames_[index].pts, pts);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = index;
        }
    }

    if (bestDelta > kDolbyVisionEnhancementPairDelta) {
        while (!dolbyVisionEnhancementFrames_.empty() &&
               dolbyVisionEnhancementFrames_.front().pts + kDolbyVisionEnhancementMatchDelta < pts) {
            dolbyVisionEnhancementFrames_.pop_front();
        }
        if (!dolbyVisionEnhancementNoMatchLogged_) {
            dolbyVisionEnhancementNoMatchLogged_ = true;
            const auto frontPts = dolbyVisionEnhancementFrames_.empty()
                                      ? std::chrono::milliseconds{-1}
                                      : dolbyVisionEnhancementFrames_.front().pts;
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"dolby_vision_el_overlay wait_for_match bl_pts_ms=" + std::to_wstring(pts.count()) +
                          L" el_front_pts_ms=" + std::to_wstring(frontPts.count()) +
                          L" best_delta_ms=" + std::to_wstring(bestDelta.count()));
        }
        return;
    }

    for (std::size_t index = 0; index < bestIndex; ++index) {
        dolbyVisionEnhancementFrames_.pop_front();
    }
    DolbyVisionEnhancementFrame matched = std::move(dolbyVisionEnhancementFrames_.front());
    dolbyVisionEnhancementFrames_.pop_front();

    frame.enhancementYuv = std::move(matched.yuv);
    frame.enhancementDovi = std::move(matched.dovi);
    frame.enhancementMetadataDetails = std::move(matched.details);
    const bool enhancementMetadataValid = frame.enhancementDovi && frame.enhancementDovi->valid;
    const bool enhancementSinglePartition =
        enhancementMetadataValid && DoviSingleNlqPartition(*frame.enhancementDovi);
    if (frame.dynamicMetadataPath.empty()) {
        frame.dynamicMetadataPath = enhancementSinglePartition
                                        ? L"dolby_vision_p7_fel_nlq_merge"
                                        : L"dolby_vision_p7_fel_mapping_only_multi_partition";
    } else if (frame.dynamicMetadataPath.find(L"fel_") == std::wstring::npos) {
        frame.dynamicMetadataPath += enhancementSinglePartition
                                         ? L"+fel_nlq_merge"
                                         : L"+fel_mapping_only_multi_partition";
    }
    if (!frame.enhancementMetadataDetails.empty()) {
        if (!frame.dynamicMetadataDetails.empty()) {
            frame.dynamicMetadataDetails += L" | ";
        }
        frame.dynamicMetadataDetails += L"el=" + frame.enhancementMetadataDetails;
    }

    if (frame.enhancementDovi && frame.enhancementDovi->valid &&
        !DoviSingleNlqPartition(*frame.enhancementDovi) &&
        !dolbyVisionMultiPartitionFallbackLogged_) {
        dolbyVisionMultiPartitionFallbackLogged_ = true;
        LogThread(LogLevel::Warning,
                  L"decoder",
                  L"dolby_vision_fel_multi_partition unsupported_via_ffmpeg_public_metadata partitions=" +
                      std::to_wstring(frame.enhancementDovi->nlqNumXPartitions) +
                      L"x" + std::to_wstring(frame.enhancementDovi->nlqNumYPartitions) +
                      L" fallback=mapping_only residual=disabled");
    }

    LogDolbyVisionCpuReferenceSample(frame);

    if (!dolbyVisionEnhancementOverlayLogged_) {
        dolbyVisionEnhancementOverlayLogged_ = true;
        const bool singlePartition = frame.enhancementDovi &&
                                     frame.enhancementDovi->valid &&
                                     DoviSingleNlqPartition(*frame.enhancementDovi);
        const std::wstring mergeMode =
            frame.enhancementDovi && frame.enhancementDovi->valid
                ? (singlePartition ? L"nlq_merge" : L"mapping_only_multi_partition")
                : L"experimental_overlay";
        LogThread(LogLevel::Info,
                  L"decoder",
                  L"dolby_vision_el_overlay active mode=" + mergeMode +
                      L" bl_pts_ms=" +
                      std::to_wstring(pts.count()) +
                      L" el_pts_ms=" + std::to_wstring(matched.pts.count()) +
                      L" delta_ms=" + std::to_wstring(bestDelta.count()) +
                      L" size=" + std::to_wstring(frame.enhancementYuv.width) +
                      L"x" + std::to_wstring(frame.enhancementYuv.height));
    }
}

bool FfmpegVideoDecoder::PublishPreparedDolbyVisionFrame(NativeVideoFrame&& frame) {
    const bool logFirstDoviQueue = !dolbyVisionFirstQueueLogged_;
    if (logFirstDoviQueue) {
        LogThread(LogLevel::Debug,
                  L"decoder",
                  L"dolby_vision_yuv_queue_ready serial=" + std::to_wstring(frame.serial) +
                      L" pts_ms=" + std::to_wstring(frame.pts.count()) +
                      L" fel_overlay=" + (frame.HasEnhancementYuv() ? L"yes" : L"no") +
                      L" subtitles_deferred=true");
    }
    const bool published = oneShotFrame_
                               ? PublishImmediateFrame(std::move(frame))
                               : EnqueueFrame(std::move(frame));
    if (logFirstDoviQueue) {
        dolbyVisionFirstQueueLogged_ = true;
        LogThread(LogLevel::Debug,
                  L"decoder",
                  L"dolby_vision_yuv_queue_result enqueued=" +
                      std::wstring(published ? L"true" : L"false"));
    }
    return published;
}

void FfmpegVideoDecoder::TryPublishPendingDolbyVisionBaseFrame() {
    if (!pendingDolbyVisionBaseFrame_.has_value()) {
        return;
    }
    if (pendingDolbyVisionBaseFrame_->timelineSerial != CurrentTimelineSerial()) {
        pendingDolbyVisionBaseFrame_.reset();
        return;
    }

    AttachDolbyVisionEnhancementFrame(*pendingDolbyVisionBaseFrame_,
                                      pendingDolbyVisionBaseFrame_->pts);
    if (!pendingDolbyVisionBaseFrame_->HasEnhancementYuv()) {
        // If the EL has already advanced beyond this BL PTS, that exact pair
        // cannot arrive later. Release the retained BL so the next decoded BL
        // can become the new synchronization candidate.
        if (!dolbyVisionEnhancementFrames_.empty() &&
            dolbyVisionEnhancementFrames_.front().pts >
                pendingDolbyVisionBaseFrame_->pts + kDolbyVisionEnhancementMatchDelta) {
            pendingDolbyVisionBaseFrame_.reset();
        }
        return;
    }

    NativeVideoFrame ready = std::move(*pendingDolbyVisionBaseFrame_);
    pendingDolbyVisionBaseFrame_.reset();
    dolbyVisionEnhancementStartupMisses_ = 0;
    dolbyVisionEnhancementStartupFallbackLogged_ = false;
    dolbyVisionEnhancementBaseOnlyDropLogged_ = false;
    (void)PublishPreparedDolbyVisionFrame(std::move(ready));
}

void FfmpegVideoDecoder::LogDolbyVisionCpuReferenceSample(const NativeVideoFrame& frame) {
    if (dolbyVisionCpuReferenceLogged_ ||
        !frame.enhancementDovi ||
        !frame.enhancementDovi->valid ||
        !DoviSingleNlqPartition(*frame.enhancementDovi)) {
        return;
    }

    DoviCpuReferenceStats stats;
    if (!BuildDolbyVisionCpuReferenceSample(frame, stats)) {
        return;
    }

    dolbyVisionCpuReferenceLogged_ = true;
    const auto average = [&stats](const int component) {
        return stats.sampleCount > 0
                   ? static_cast<double>(stats.sumCode[component]) / static_cast<double>(stats.sampleCount)
                   : 0.0;
    };
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(1)
           << L"dolby_vision_cpu_reference active mode=sampled_grid"
           << L" grid=" << stats.gridWidth << L"x" << stats.gridHeight
           << L" samples=" << stats.sampleCount
           << L" hash=" << Hex64(stats.hash)
           << L" vdr_bit_depth=" << stats.vdrBitDepth
           << L" composer=code_merge+fixed16_poly+vdr_quant+chroma_site_luma+el_"
           << (frame.enhancementDovi->elSpatialResampling ? L"bicubic" : L"linear")
           << L" y(min=" << stats.minCode[0] << L" max=" << stats.maxCode[0] << L" avg=" << average(0) << L")"
           << L" cb(min=" << stats.minCode[1] << L" max=" << stats.maxCode[1] << L" avg=" << average(1) << L")"
           << L" cr(min=" << stats.minCode[2] << L" max=" << stats.maxCode[2] << L" avg=" << average(2) << L")";
    LogThread(LogLevel::Info, L"decoder", stream.str());
}

bool FfmpegVideoDecoder::PublishFrame(AVFrame* frame, AVFrame* softwareFrame, SwsContext*& swsCtx,
                                      const AVRational timeBase, uint64_t& serial) {
    const auto pts = FramePts(frame, timeBase);
    if (ShouldDropSeekPreroll(pts)) {
        return true;
    }
    const uint64_t frameTimelineSerial = CurrentTimelineSerial();

    AVFrame* conversionFrame = frame;
    if (frame && frame->format == hardwarePixelFormat_ && hardwarePixelFormat_ != AV_PIX_FMT_NONE) {
        NativeVideoFrame textureFrame;
        if (TryBuildD3DTextureFrame(frame, pts, serial, textureFrame)) {
            textureFrame.timelineSerial = frameTimelineSerial;
            if (!firstHardwareFrameLogged_) {
                firstHardwareFrameLogged_ = true;
                LogThread(LogLevel::Info,
                          L"decoder",
                          L"hardware_frame path=zero_copy pts_ms=" + std::to_wstring(pts.count()) +
                              L" size=" + std::to_wstring(textureFrame.width) + L"x" +
                              std::to_wstring(textureFrame.height) + L" texture=" +
                              std::to_wstring(textureFrame.d3dTextureWidth) + L"x" +
                              std::to_wstring(textureFrame.d3dTextureHeight) + L" uv_rect=" +
                              std::to_wstring(textureFrame.sourceUvRect.left) + L"," +
                              std::to_wstring(textureFrame.sourceUvRect.top) + L"," +
                              std::to_wstring(textureFrame.sourceUvRect.right) + L"," +
                              std::to_wstring(textureFrame.sourceUvRect.bottom) +
                              L" dxgi=" + DxgiFormatName(textureFrame.d3dFormat));
            }
            if (oneShotFrame_) {
                return PublishImmediateFrame(std::move(textureFrame));
            }
            return EnqueueFrame(std::move(textureFrame));
        }

        if (!softwareFrame) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"hardware_frame fallback=cpu_transfer failed reason=missing_software_frame");
            return false;
        }
        av_frame_unref(softwareFrame);
        const int transferError = av_hwframe_transfer_data(softwareFrame, frame, 0);
        if (transferError < 0) {
            LogThreadError(L"av_hwframe_transfer_data failed: " + FfmpegErrorString(transferError));
            return false;
        }
        softwareFrame->pts = frame->pts;
        softwareFrame->best_effort_timestamp = frame->best_effort_timestamp;
        softwareFrame->sample_aspect_ratio = frame->sample_aspect_ratio;
        conversionFrame = softwareFrame;
        if (!firstCpuTransferFrameLogged_) {
            firstCpuTransferFrameLogged_ = true;
            LogThread(LogLevel::Info,
                      L"decoder",
                      L"hardware_frame path=cpu_transfer format=" +
                          PixelFormatName(static_cast<AVPixelFormat>(softwareFrame->format)) +
                          L" pts_ms=" + std::to_wstring(pts.count()) +
                          L" size=" + std::to_wstring(softwareFrame->width) + L"x" +
                          std::to_wstring(softwareFrame->height));
        }
        {
            std::scoped_lock lock(mutex_);
            ++stats_.hardwareFrames;
            ++stats_.cpuTransferFrames;
            stats_.usingHardwareDecode = true;
            stats_.decoder = L"ffmpeg_d3d11va";
        }
    }

    frame = conversionFrame;
    const int srcW = frame->width;
    const int srcH = frame->height;
    if (srcW <= 0 || srcH <= 0) {
        return true;
    }

    if (ShouldUsePrimaryDoviLibplacebo() && TryPublishDoviLibplaceboFrame(frame, timeBase, pts, serial)) {
        return true;
    }

    // Dolby Vision software path: carry the raw 10-bit YUV planes to the GPU
    // without swscale's YUV->RGB conversion, which would destroy the IPT
    // structure that the renderer must reshape. We pack Y + interleaved UV
    // (NV12/P010 layout) into a single buffer the renderer uploads as a
    // P010 texture.
    const bool enhancementOverlayPath = enableDolbyVisionEnhancementDecode_ && dolbyVisionEnhancementActive_;
    if ((dolbyVisionStream_ || enhancementOverlayPath) && frame->format == AV_PIX_FMT_YUV420P10LE) {
        NativeVideoFrame queued;
        queued.width = srcW;
        queued.height = srcH;
        queued.color = MergeFrameColorMetadata(frame, streamColorMetadata_);
        queued.hdr10PlusPayload = ExtractHdr10PlusPayload(frame);
        queued.hdr10Plus = ExtractHdr10PlusMetadata(frame);
        if (queued.hdr10Plus) {
            hdr10PlusDetected_.store(true);
            queued.dynamicMetadataPath = L"hdr10plus_st2094_40";
            queued.dynamicMetadataDetails = Hdr10PlusFrameSummary(queued.hdr10Plus.get());
        }
        queued.dolbyVisionRpu = ExtractDolbyVisionRpu(frame);
        queued.dovi = dolbyVisionStream_ ? ExtractFrameDolbyVisionMetadata(frame) : nullptr;
        if (queued.dovi && queued.dovi->valid) {
            queued.dynamicMetadataPath = L"dolby_vision_shader";
            queued.dynamicMetadataDetails = DoviFrameSummary(queued.dovi.get());
        }
        if (!dolbyVisionFirstFrameLogged_) {
            dolbyVisionFirstFrameLogged_ = true;
            LogThread(LogLevel::Info, L"decoder",
                      L"dolby_vision_yuv_path w=" + std::to_wstring(srcW) + L" h=" + std::to_wstring(srcH) +
                          L" dovi=" + (queued.dovi ? L"yes" : L"no") +
                          L" fel_overlay=" + (enhancementOverlayPath ? L"enabled" : L"disabled"));
        }

        NativeYuvPlanes packedYuv;
        const auto packStart = std::chrono::steady_clock::now();
        if (!PackYuv420P10FrameToP010(frame, packedYuv, kDecodedFrameMemoryBudgetBytes)) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"dolby_vision_yuv_path fallback=bgra reason=pack_p010_failed");
        } else {
            if (!dolbyVisionFirstPackedLogged_) {
                dolbyVisionFirstPackedLogged_ = true;
                const auto packMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - packStart).count();
                LogThread(LogLevel::Debug,
                          L"decoder",
                          L"dolby_vision_yuv_packed bytes=" +
                              std::to_wstring(packedYuv.data ? packedYuv.data->size() : 0) +
                              L" pack_ms=" + std::to_wstring(packMs));
            }

            queued.yuv = std::move(packedYuv);
            queued.pts = pts;
            queued.serial = ++serial;
            queued.timelineSerial = frameTimelineSerial;
            RefreshFrameSubtitles(queued);
            AttachDolbyVisionEnhancementFrame(queued, pts);
            if (enhancementOverlayPath && !queued.HasEnhancementYuv()) {
                bool hasRenderedFrame = false;
                {
                    std::scoped_lock lock(mutex_);
                    hasRenderedFrame = stats_.rendered > 0 || latestFrame_.HasContent();
                }
                ++dolbyVisionEnhancementStartupMisses_;
                const bool retainedBase = !pendingDolbyVisionBaseFrame_.has_value();
                if (retainedBase) {
                    pendingDolbyVisionBaseFrame_ = std::move(queued);
                }
                if (!hasRenderedFrame && dolbyVisionEnhancementStartupMisses_ == 1) {
                    LogThread(LogLevel::Debug,
                              L"decoder",
                              L"dolby_vision_el_overlay startup_wait_for_match bl_pts_ms=" +
                                  std::to_wstring(pts.count()) +
                                  L" retained=" + (retainedBase ? L"true" : L"false"));
                } else if (hasRenderedFrame && !dolbyVisionEnhancementBaseOnlyDropLogged_) {
                    dolbyVisionEnhancementBaseOnlyDropLogged_ = true;
                    LogThread(LogLevel::Debug,
                              L"decoder",
                              L"dolby_vision_el_overlay hold_base_for_match bl_pts_ms=" +
                                  std::to_wstring(pts.count()) +
                                  L" retained=" + (retainedBase ? L"true" : L"false"));
                }
                return true;
            } else if (enhancementOverlayPath) {
                dolbyVisionEnhancementStartupMisses_ = 0;
                dolbyVisionEnhancementStartupFallbackLogged_ = false;
                dolbyVisionEnhancementBaseOnlyDropLogged_ = false;
            }
            return PublishPreparedDolbyVisionFrame(std::move(queued));
        }
    }


    SwsContext* cached = sws_getCachedContext(swsCtx,
                                              srcW,
                                              srcH,
                                              static_cast<AVPixelFormat>(frame->format),
                                              srcW,
                                              srcH,
                                              AV_PIX_FMT_BGRA,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
    if (!cached) {
        LogThreadError(L"sws_getCachedContext failed");
        return false;
    }
    swsCtx = cached;

    const int stride = srcW * 4;
    const std::size_t needed = static_cast<std::size_t>(stride) * static_cast<std::size_t>(srcH);
    auto pixels = AcquireReusableBgraBuffer(needed);
    if (!pixels || pixels->size() < needed) {
        LogThreadError(L"BGRA frame buffer allocation failed");
        return false;
    }

    uint8_t* dst[1] = {pixels->data()};
    int dstStride[1] = {stride};
    sws_scale(swsCtx, frame->data, frame->linesize, 0, srcH, dst, dstStride);

    NativeVideoFrame queued;
    queued.width = srcW;
    queued.height = srcH;
    queued.stride = stride;
    queued.bgra = std::move(pixels);
    queued.color = MergeFrameColorMetadata(frame, streamColorMetadata_);
    queued.hdr10PlusPayload = ExtractHdr10PlusPayload(frame);
    queued.hdr10Plus = ExtractHdr10PlusMetadata(frame);
    if (queued.hdr10Plus) {
        hdr10PlusDetected_.store(true);
        queued.dynamicMetadataPath = L"hdr10plus_st2094_40";
        queued.dynamicMetadataDetails = Hdr10PlusFrameSummary(queued.hdr10Plus.get());
    }
    queued.dolbyVisionRpu = ExtractDolbyVisionRpu(frame);
    if (dolbyVisionStream_) {
        queued.dovi = ExtractFrameDolbyVisionMetadata(frame);
        if (queued.dovi && queued.dovi->valid) {
            queued.dynamicMetadataPath = L"dolby_vision_shader";
            queued.dynamicMetadataDetails = DoviFrameSummary(queued.dovi.get());
        }
    }
    queued.pts = pts;
    queued.serial = ++serial;
    queued.timelineSerial = frameTimelineSerial;
    RefreshFrameSubtitles(queued);
    if (oneShotFrame_) {
        return PublishImmediateFrame(std::move(queued));
    }
    return EnqueueFrame(std::move(queued));
}

bool FfmpegVideoDecoder::TryBuildD3DTextureFrame(AVFrame* frame, std::chrono::milliseconds pts, uint64_t& serial, NativeVideoFrame& out) {
    if (!frame || !sharedD3DDevice_ || frame->format != hardwarePixelFormat_ || hardwarePixelFormat_ == AV_PIX_FMT_NONE) {
        return false;
    }

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    if (!texture) {
        LogZeroCopyFallbackOnce(L"missing_d3d11_texture");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!IsSupportedHardwareTextureFormat(desc.Format)) {
        LogZeroCopyFallbackOnce(L"unsupported_texture_format:" + DxgiFormatName(desc.Format));
        return false;
    }
    if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        LogZeroCopyFallbackOnce(L"texture_missing_shader_resource_bind");
        return false;
    }

    const auto cropToInt = [](const std::size_t value) {
        return value <= static_cast<std::size_t>(std::numeric_limits<int>::max())
                   ? static_cast<int>(value)
                   : -1;
    };
    const bool textureDimensionsFit =
        desc.Width <= static_cast<UINT>(std::numeric_limits<int>::max()) &&
        desc.Height <= static_cast<UINT>(std::numeric_limits<int>::max());
    const int textureWidth = textureDimensionsFit ? static_cast<int>(desc.Width) : 0;
    const int textureHeight = textureDimensionsFit ? static_cast<int>(desc.Height) : 0;
    const auto samplingRegion = BuildVideoTextureSamplingRegion(
        frame->width,
        frame->height,
        textureWidth,
        textureHeight,
        cropToInt(frame->crop_left),
        cropToInt(frame->crop_top),
        cropToInt(frame->crop_right),
        cropToInt(frame->crop_bottom));
    if (!samplingRegion.valid) {
        LogZeroCopyFallbackOnce(
            L"invalid_visible_texture_region frame=" + std::to_wstring(frame->width) + L"x" +
            std::to_wstring(frame->height) + L" texture=" + std::to_wstring(desc.Width) + L"x" +
            std::to_wstring(desc.Height));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (!textureDevice || textureDevice.Get() != sharedD3DDevice_.Get()) {
        LogZeroCopyFallbackOnce(L"texture_device_mismatch");
        return false;
    }

    AVFrame* retained = av_frame_alloc();
    if (!retained) {
        return false;
    }
    const int refError = av_frame_ref(retained, frame);
    if (refError < 0) {
        av_frame_free(&retained);
        LogZeroCopyFallbackOnce(L"av_frame_ref_failed:" + FfmpegErrorString(refError));
        return false;
    }

    out.width = samplingRegion.visibleWidth;
    out.height = samplingRegion.visibleHeight;
    out.stride = 0;
    out.d3dTexture = texture;
    out.d3dArraySlice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    out.d3dFormat = desc.Format;
    out.d3dTextureWidth = textureWidth;
    out.d3dTextureHeight = textureHeight;
    out.sourceUvRect = samplingRegion.uvRect;
    out.softwareFormat = HardwareFrameSoftwareFormat(frame);
    out.color = MergeFrameColorMetadata(frame, streamColorMetadata_);
    out.hdr10PlusPayload = ExtractHdr10PlusPayload(frame);
    out.hdr10Plus = ExtractHdr10PlusMetadata(frame);
    if (out.hdr10Plus) {
        hdr10PlusDetected_.store(true);
        out.dynamicMetadataPath = L"hdr10plus_st2094_40";
        out.dynamicMetadataDetails = Hdr10PlusFrameSummary(out.hdr10Plus.get());
    }
    out.dolbyVisionRpu = ExtractDolbyVisionRpu(frame);
    if (dolbyVisionStream_) {
        out.dovi = ExtractFrameDolbyVisionMetadata(frame);
        if (out.dovi && out.dovi->valid) {
            out.dynamicMetadataPath = L"dolby_vision_shader";
            out.dynamicMetadataDetails = DoviFrameSummary(out.dovi.get());
        }
    }
    out.hardwareFrameRef = std::shared_ptr<AVFrame>(retained, [](AVFrame* value) {
        if (value) {
            av_frame_free(&value);
        }
    });
    out.pts = pts;
    out.serial = ++serial;
    out.timelineSerial = CurrentTimelineSerial();
    RefreshFrameSubtitles(out);

    {
        std::scoped_lock lock(mutex_);
        ++stats_.hardwareFrames;
        ++stats_.zeroCopyFrames;
        stats_.usingHardwareDecode = true;
        stats_.decoder = L"ffmpeg_d3d11va";
    }
    return true;
}

bool FfmpegVideoDecoder::PublishImmediateFrame(NativeVideoFrame&& frame) {
    frame.frameRateNumerator = videoFrameRateNumerator_;
    frame.frameRateDenominator = videoFrameRateDenominator_;
    RefreshFrameSubtitles(frame);
    {
        std::scoped_lock lock(mutex_);
        stats_.queueDepth = 0;
        stats_.clockPosition = frame.pts;
        stats_.usingAudioClock = false;
        stats_.driftMs = 0;
        stats_.buffering = false;
        stats_.timelineSerial = frame.timelineSerial != 0 ? frame.timelineSerial : CurrentTimelineSerial();
        ++stats_.rendered;
        latestFrame_ = frame;
    }
    NotifyFrameReady();
    stopping_.store(true);
    return true;
}

std::optional<std::chrono::milliseconds> FfmpegVideoDecoder::TakePendingSeek() {
    const int64_t ms = pendingSeekMs_.exchange(-1);
    if (ms < 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{ms};
}

bool FfmpegVideoDecoder::HasPendingSeek() const {
    return pendingSeekMs_.load() >= 0;
}

bool FfmpegVideoDecoder::ApplyPendingSeek(AVFormatContext* formatCtx,
                                          AVCodecContext* codecCtx,
                                          AVCodecContext* subtitleCodecCtx,
                                          AVCodecContext* enhancementCodecCtx,
                                          const int videoStreamIndex,
                                          const AVRational videoTimeBase) {
    const auto target = TakePendingSeek();
    if (!target.has_value()) {
        return true;
    }
    if (!formatCtx || !codecCtx) {
        return false;
    }

    startPosition_ = *target;
    if (playbackPaused_.load()) {
        pausedPositionMs_.store(target->count());
    }
    const bool networkSource = IsNetworkMediaPath(path_);
    const auto seekIoTimeout = networkSource ? kRuntimeNetworkSeekIoTimeout : kRuntimeSeekIoTimeout;
    const auto seekStart = std::chrono::steady_clock::now();
    const int64_t globalSeekTarget = static_cast<int64_t>(target->count()) * AV_TIME_BASE / 1000;
    std::wstring seekMethod = L"global";
    int seekError = AVERROR(EINVAL);
    const auto armSeekInterrupt = [&]() {
        ioInterruptAfterSteadyMs_.store(SteadyClockMs() + seekIoTimeout.count());
    };
    if (videoStreamIndex >= 0 && videoTimeBase.num > 0 && videoTimeBase.den > 0) {
        const int64_t videoSeekTarget = av_rescale_q(target->count(), AVRational{1, 1000}, videoTimeBase);
        armSeekInterrupt();
        seekError = av_seek_frame(formatCtx, videoStreamIndex, videoSeekTarget, AVSEEK_FLAG_BACKWARD);
        seekMethod = L"video_stream";
    }
    if (seekError < 0) {
        seekMethod = L"global";
        armSeekInterrupt();
        seekError = av_seek_frame(formatCtx, -1, globalSeekTarget, AVSEEK_FLAG_BACKWARD);
    }
    if (seekError < 0) {
        seekMethod = L"global_file";
        armSeekInterrupt();
        seekError = avformat_seek_file(formatCtx, -1, INT64_MIN, globalSeekTarget, INT64_MAX, 0);
    }
    ioInterruptAfterSteadyMs_.store(0);
    const auto seekElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - seekStart).count();
    const bool seekInterrupted = seekError == AVERROR_EXIT ||
                                 (seekError < 0 && seekElapsedMs >= seekIoTimeout.count());
    // Seek() already advanced the generation before returning to its caller,
    // making every pre-seek frame stale immediately.  Reuse that serial here
    // so rapid latest-only seek requests cannot briefly revive an older
    // timeline while FFmpeg is between two I/O operations.
    const uint64_t seekTimelineSerial = CurrentTimelineSerial();
    LogThread(seekError < 0 ? LogLevel::Warning : LogLevel::Info,
              L"decoder",
              L"runtime_seek target=" + anvil::playback::FormatTimecode(*target) +
                  L" method=" + seekMethod +
                  L" seek_ms=" + std::to_wstring(seekElapsedMs) +
                  L" timeout_ms=" + std::to_wstring(seekIoTimeout.count()) +
                  L" source=" + std::wstring(networkSource ? L"network" : L"local") +
                  (seekError >= 0 ? L" timeline_serial=" + std::to_wstring(seekTimelineSerial) : L"") +
                  (seekInterrupted ? L" interrupted=true" : L""));
    if (seekError < 0) {
        LogThread(LogLevel::Warning, L"decoder", L"runtime_seek failed reason=" + FfmpegErrorString(seekError));
        {
            std::scoped_lock lock(mutex_);
            // Keep the audio handoff gate closed until either a newer seek
            // completes or the failing session is stopped. Resetting recovery
            // here can overwrite the atomics published by a concurrently
            // queued seek and briefly resume audio on the wrong timeline.
            stats_.buffering = false;
        }
        if (stopping_.load()) {
            return false;
        }
        if (HasPendingSeek()) {
            return true;
        }
        NotifyDecodeFailure(L"Runtime seek failed at " +
                            anvil::playback::FormatTimecode(*target) +
                            L": " + FfmpegErrorString(seekError));
        return false;
    }

    avformat_flush(formatCtx);
    const bool clearedIoState = ClearNetworkIoState(formatCtx);
    if (clearedIoState) {
        LogThread(LogLevel::Debug,
                  L"decoder",
                  L"runtime_seek cleared_io_state " + FormatIoState(formatCtx));
    }
    avcodec_flush_buffers(codecCtx);
    if (subtitleCodecCtx) {
        avcodec_flush_buffers(subtitleCodecCtx);
    }
    if (enhancementCodecCtx) {
        avcodec_flush_buffers(enhancementCodecCtx);
    }
    if (audioPacketSink_.Enabled() && audioPacketSink_.reset) {
        audioPacketSink_.reset(*target);
    }
    doviLibplaceboFilter_.reset();
    dolbyVisionEnhancementFirstFrameLogged_ = false;
    dolbyVisionEnhancementDynamicMetadataLogged_ = false;
    dolbyVisionEnhancementFailureLogged_ = false;
    dolbyVisionEnhancementOverlayLogged_ = false;
    dolbyVisionEnhancementNoMatchLogged_ = false;
    dolbyVisionEnhancementFirstPackedLogged_ = false;
    dolbyVisionCpuReferenceLogged_ = false;
    dolbyVisionMultiPartitionFallbackLogged_ = false;
    dolbyVisionEnhancementStartupFallbackLogged_ = false;
    dolbyVisionEnhancementBaseOnlyDropLogged_ = false;
    dolbyVisionEnhancementStartupMisses_ = 0;
    dolbyVisionEnhancementLastDynamicMetadataFingerprint_ = 0;
    dolbyVisionEnhancementFramesDecoded_ = 0;
    latestDolbyVisionEnhancementMetadata_.reset();
    latestDolbyVisionEnhancementMetadataPts_ = std::chrono::milliseconds{0};
    dolbyVisionEnhancementFrames_.clear();
    pendingDolbyVisionBaseFrame_.reset();
    if (externalSubtitlesActive_) {
        subtitleCues_ = externalSubtitleCues_;
    } else {
        subtitleCues_.clear();
    }
    if (subtitleAssRenderer_) {
        if (subtitleAssActive_ && !subtitleAssExternalFullTrack_) {
            subtitleAssRenderer_->FlushEvents();
        } else {
            subtitleAssRenderer_->ResetRenderCache();
        }
    }

    NativeVideoFrame retiredLatestFrame;
    std::deque<NativeVideoFrame> retiredFrameQueue;
    {
        std::scoped_lock lock(mutex_);
        const bool prerollAfterSeek = !playbackPaused_.load();
        retiredLatestFrame = std::move(latestFrame_);
        retiredFrameQueue.swap(frameQueue_);
        schedulePrimed_ = false;
        fallbackClockAnchor_.reset();
        fallbackClockBasePts_ = *target;
        stats_.queueDepth = 0;
        stats_.packetQueueDepth = 0;
        stats_.packetQueueBytes = 0;
        stats_.readAheadEnd = std::chrono::milliseconds{0};
        stats_.readAheadDuration = std::chrono::milliseconds{0};
        stats_.clockPosition = *target;
        stats_.driftMs = 0;
        stats_.rendered = 0;
        stats_.droppedStale = 0;
        BeginSeekRecoveryLocked(*target, prerollAfterSeek, seekTimelineSerial);
        UpdateBufferedStatsLocked();
    }
    seekRecoveryTargetMs_.store(target->count());
    seekRecoveryDropLogged_.store(false);
    return true;
}

bool FfmpegVideoDecoder::DecodeSubtitlePacket(AVCodecContext* subtitleCodecCtx,
                                              const AVPacket* packet,
                                              const AVRational subtitleTimeBase) {
    if (!packet) {
        return false;
    }

    if (subtitleAssActive_ && !subtitleAssExternalFullTrack_) {
        const auto pts = PacketTimestamp(packet, subtitleTimeBase).value_or(std::chrono::milliseconds{0});
        const auto duration = PacketDuration(packet, subtitleTimeBase).value_or(std::chrono::milliseconds{4000});
        if (subtitleAssRenderer_ &&
            subtitleAssRenderer_->ProcessPacket(packet->data, packet->size, pts, duration)) {
            if (RefreshQueuedFrameSubtitles()) {
                NotifyFrameReady();
            }
            return true;
        }
        return false;
    }

    if (!subtitleCodecCtx) {
        return false;
    }

    AVSubtitle subtitle{};
    int gotSubtitle = 0;
    const int decodeResult = avcodec_decode_subtitle2(subtitleCodecCtx, &subtitle, &gotSubtitle, packet);
    if (decodeResult < 0) {
        LogThread(LogLevel::Warning, L"subtitle", L"decode_failed reason=" + FfmpegErrorString(decodeResult));
        return false;
    }
    if (!gotSubtitle) {
        return true;
    }

    const bool isPgsSubtitle = subtitleCodecCtx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE;
    int canvasWidth = subtitleCanvasWidth_;
    int canvasHeight = subtitleCanvasHeight_;
    std::wstring canvasSource = L"video";
    if (ValidSubtitleCanvas(subtitleCodecCtx->width, subtitleCodecCtx->height)) {
        canvasWidth = subtitleCodecCtx->width;
        canvasHeight = subtitleCodecCtx->height;
        canvasSource = L"codec";
    }
    if (isPgsSubtitle) {
        if (const auto packetCanvas = PgsCanvasFromPacket(packet)) {
            canvasWidth = packetCanvas->first;
            canvasHeight = packetCanvas->second;
            canvasSource = L"pgs_packet";
        }
    }
    if (ValidSubtitleCanvas(canvasWidth, canvasHeight)) {
        subtitleCanvasWidth_ = canvasWidth;
        subtitleCanvasHeight_ = canvasHeight;
    }

    const auto basePts = SubtitlePacketBasePts(subtitle, packet, subtitleTimeBase);
    auto start = basePts + std::chrono::milliseconds{subtitle.start_display_time};
    auto end = basePts + std::chrono::milliseconds{subtitle.end_display_time};
    const std::wstring text = SubtitleTextFromDecoded(subtitle);
    auto bitmaps = SubtitleBitmapsFromDecoded(subtitle, canvasWidth, canvasHeight, subtitleBitmapSerial_);
    const bool decodedDurationLooksInvalid =
        end <= start ||
        (!bitmaps.empty() && isPgsSubtitle && end - start > kMaxBitmapSubtitleDuration);
    if (decodedDurationLooksInvalid) {
        if (const auto duration = PacketDuration(packet, subtitleTimeBase);
            duration.has_value() && *duration > std::chrono::milliseconds{0}) {
            end = start + *duration;
        } else if (isPgsSubtitle) {
            end = start + kDefaultBitmapSubtitleDuration;
        } else {
            end = start + std::chrono::seconds{4};
        }
    }
    if (!bitmaps.empty() && !subtitleCanvasLogged_ && ValidSubtitleCanvas(canvasWidth, canvasHeight)) {
        subtitleCanvasLogged_ = true;
        LogThread(LogLevel::Debug,
                  L"subtitle",
                  L"canvas=" + std::to_wstring(canvasWidth) + L"x" + std::to_wstring(canvasHeight) +
                      L" source=" + canvasSource);
    }
    if (text.empty() && bitmaps.empty()) {
        avsubtitle_free(&subtitle);
        return true;
    }
    if (!bitmaps.empty() && !bitmapSubtitleLogged_) {
        bitmapSubtitleLogged_ = true;
        LogThread(LogLevel::Info,
                  L"subtitle",
                  L"bitmap_overlay active=true rects=" + std::to_wstring(bitmaps.size()) +
                      L" start=" + anvil::playback::FormatTimecode(start) +
                      L" end=" + anvil::playback::FormatTimecode(end));
    }

    if (!bitmaps.empty() && isPgsSubtitle) {
        TrimActiveBitmapSubtitleCues(start);
    }

    bool refreshedLatestFrame = false;
    if (end > startPosition_) {
        subtitleCues_.push_back(NativeSubtitleCue{start, end, text, std::move(bitmaps)});
        refreshedLatestFrame = RefreshQueuedFrameSubtitles();
    }
    avsubtitle_free(&subtitle);
    if (refreshedLatestFrame) {
        NotifyFrameReady();
    }
    return true;
}

void FfmpegVideoDecoder::TrimActiveBitmapSubtitleCues(const std::chrono::milliseconds time) {
    for (auto& cue : subtitleCues_) {
        if (!cue.bitmaps.empty() && cue.start < time && time < cue.end) {
            cue.end = time;
        }
    }
}

void FfmpegVideoDecoder::PruneExpiredSubtitleCues(const std::chrono::milliseconds effectivePts) {
    while (!subtitleCues_.empty() && subtitleCues_.front().end + kSubtitleCueRetention <= effectivePts) {
        subtitleCues_.pop_front();
    }
}

void FfmpegVideoDecoder::RefreshFrameSubtitles(NativeVideoFrame& frame, const bool force) {
    if (!frame.HasContent()) {
        return;
    }
    if (!force && frame.subtitlesPrepared) {
        return;
    }
    if (!force) {
        PruneExpiredSubtitleCues(frame.pts - subtitleDelay_);
    }
    frame.subtitleText = SubtitleTextForPts(frame.pts);
    const bool seekFastResumeFrame =
        !force &&
        seekFastResumeFramesRemaining_.load() > 0;
    const bool skipAssForSeek = seekFastResumeFrame && subtitleAssActive_;
    frame.subtitleBitmaps = SubtitleBitmapsForPts(frame.pts, frame.width, frame.height, !skipAssForSeek);
    if (seekFastResumeFrame) {
        seekFastResumeFramesRemaining_.fetch_sub(1);
        if (!seekFastResumeLogged_.exchange(true)) {
            LogThread(LogLevel::Debug,
                      L"subtitle",
                      L"seek_fast_resume frames=" +
                          std::to_wstring(kSeekFastResumeFrameCount) +
                          L" skip_ass=" + std::wstring(skipAssForSeek ? L"true" : L"false") +
                          L" first_pts=" + anvil::playback::FormatTimecode(frame.pts));
        }
    }
    frame.subtitlesPrepared = true;
    if (!frame.subtitleBitmaps.empty() && !frameSubtitleBitmapLogged_) {
        frameSubtitleBitmapLogged_ = true;
        LogThread(LogLevel::Debug,
                  L"subtitle",
                  L"frame_bitmap matched pts=" + anvil::playback::FormatTimecode(frame.pts) +
                  L" rects=" + std::to_wstring(frame.subtitleBitmaps.size()));
    }
}

bool FfmpegVideoDecoder::RefreshLatestFrameSubtitles() {
    auto bitmapFingerprint = [](const std::vector<NativeSubtitleBitmap>& bitmaps) {
        uint64_t fingerprint = static_cast<uint64_t>(bitmaps.size());
        for (const auto& bitmap : bitmaps) {
            fingerprint ^= bitmap.serial + 0x9e3779b97f4a7c15ull + (fingerprint << 6) + (fingerprint >> 2);
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.x, 0)) << 1;
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.y, 0)) << 17;
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.width, 0)) << 33;
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.height, 0)) << 49;
        }
        return fingerprint;
    };

    std::scoped_lock lock(mutex_);
    if (!latestFrame_.HasContent()) {
        return false;
    }
    const std::wstring previousText = latestFrame_.subtitleText;
    const uint64_t previousBitmapFingerprint = bitmapFingerprint(latestFrame_.subtitleBitmaps);
    RefreshFrameSubtitles(latestFrame_, true);
    return latestFrame_.subtitleText != previousText ||
           bitmapFingerprint(latestFrame_.subtitleBitmaps) != previousBitmapFingerprint;
}

bool FfmpegVideoDecoder::RefreshQueuedFrameSubtitles() {
    auto bitmapFingerprint = [](const std::vector<NativeSubtitleBitmap>& bitmaps) {
        uint64_t fingerprint = static_cast<uint64_t>(bitmaps.size());
        for (const auto& bitmap : bitmaps) {
            fingerprint ^= bitmap.serial + 0x9e3779b97f4a7c15ull + (fingerprint << 6) + (fingerprint >> 2);
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.x, 0)) << 1;
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.y, 0)) << 17;
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.width, 0)) << 33;
            fingerprint ^= static_cast<uint64_t>(std::max(bitmap.height, 0)) << 49;
        }
        return fingerprint;
    };

    auto refreshChanged = [&](NativeVideoFrame& frame) {
        const std::wstring previousText = frame.subtitleText;
        const uint64_t previousBitmapFingerprint = bitmapFingerprint(frame.subtitleBitmaps);
        RefreshFrameSubtitles(frame, true);
        return frame.subtitleText != previousText ||
               bitmapFingerprint(frame.subtitleBitmaps) != previousBitmapFingerprint;
    };

    bool latestChanged = false;
    std::scoped_lock lock(mutex_);
    if (latestFrame_.HasContent()) {
        latestChanged = refreshChanged(latestFrame_);
    }
    for (auto& frame : frameQueue_) {
        refreshChanged(frame);
    }
    return latestChanged;
}

std::wstring FfmpegVideoDecoder::SubtitleTextForPts(const std::chrono::milliseconds pts) {
    const auto effectivePts = pts - subtitleDelay_;

    std::wstring text;
    for (const auto& cue : subtitleCues_) {
        if (cue.start <= effectivePts && effectivePts < cue.end && !cue.text.empty()) {
            if (!text.empty()) {
                text += L"\n";
            }
            text += cue.text;
        }
    }
    return text;
}

std::vector<NativeSubtitleBitmap> FfmpegVideoDecoder::SubtitleBitmapsForPts(const std::chrono::milliseconds pts,
                                                                            const int frameWidth,
                                                                            const int frameHeight,
                                                                            const bool includeAss) {
    const auto effectivePts = pts - subtitleDelay_;

    std::vector<NativeSubtitleBitmap> bitmaps;
    for (const auto& cue : subtitleCues_) {
        if (cue.start <= effectivePts && effectivePts < cue.end) {
            bitmaps.insert(bitmaps.end(), cue.bitmaps.begin(), cue.bitmaps.end());
        }
    }
    if (includeAss && subtitleAssActive_ && subtitleAssRenderer_) {
        auto assBitmaps = subtitleAssRenderer_->Render(effectivePts, frameWidth, frameHeight, subtitleBitmapSerial_);
        if (!assBitmaps.empty() && !subtitleAssLogged_) {
            subtitleAssLogged_ = true;
            LogThread(LogLevel::Info,
                      L"subtitle",
                      L"libass_bitmap_overlay active=true rects=" + std::to_wstring(assBitmaps.size()) +
                          L" frame=" + std::to_wstring(frameWidth) + L"x" + std::to_wstring(frameHeight));
        }
        bitmaps.insert(bitmaps.end(),
                       std::make_move_iterator(assBitmaps.begin()),
                       std::make_move_iterator(assBitmaps.end()));
    }
    return bitmaps;
}

bool FfmpegVideoDecoder::ShouldDropSeekPreroll(const std::chrono::milliseconds pts) {
    if (pts.count() <= 0) {
        return false;
    }
    const int64_t recoveryTargetMs = seekRecoveryTargetMs_.load();
    const auto target = recoveryTargetMs >= 0 ? std::chrono::milliseconds{recoveryTargetMs} : startPosition_;
    if (target.count() <= 0) {
        if (recoveryTargetMs >= 0) {
            seekRecoveryTargetMs_.store(-1);
            seekRecoveryDropLogged_.store(false);
        }
        return false;
    }

    constexpr std::chrono::milliseconds kBehindTolerance{80};
    constexpr std::chrono::milliseconds kAheadTolerance{1500};
    if (pts + kBehindTolerance < target) {
        return true;
    }
    if (recoveryTargetMs >= 0 && pts > target + kAheadTolerance) {
        if (!seekRecoveryDropLogged_.exchange(true)) {
            LogThread(LogLevel::Warning,
                      L"decoder",
                      L"seek_recovery drop_future_frame target=" +
                          anvil::playback::FormatTimecode(target) +
                          L" pts=" + anvil::playback::FormatTimecode(pts));
        }
        return true;
    }
    if (recoveryTargetMs >= 0) {
        seekRecoveryTargetMs_.store(-1);
        seekRecoveryDropLogged_.store(false);
    }
    return false;
}

std::chrono::milliseconds FfmpegVideoDecoder::FramePts(const AVFrame* frame, AVRational timeBase) {
    if (!frame) {
        return std::chrono::milliseconds{0};
    }
    const int64_t ptsTicks = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                 ? frame->best_effort_timestamp
                                 : frame->pts;
    if (ptsTicks == AV_NOPTS_VALUE) {
        return std::chrono::milliseconds{0};
    }
    const int64_t ms = av_rescale_q(ptsTicks, timeBase, {1, 1000});
    return std::chrono::milliseconds(ms);
}

AVPixelFormat FfmpegVideoDecoder::HardwareFrameSoftwareFormat(const AVFrame* frame) {
    if (!frame || !frame->hw_frames_ctx) {
        return AV_PIX_FMT_NONE;
    }
    const auto* framesContext = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    return framesContext ? framesContext->sw_format : AV_PIX_FMT_NONE;
}

VideoColorMetadata FfmpegVideoDecoder::BuildColorMetadata(const AVCodecParameters* parameters) {
    return anvil::app::BuildColorMetadata(parameters);
}

VideoColorMetadata FfmpegVideoDecoder::MergeFrameColorMetadata(const AVFrame* frame, const VideoColorMetadata& defaults) {
    return anvil::app::MergeFrameColorMetadata(frame, defaults);
}

VideoColorMetadata FfmpegVideoDecoder::SdrBt709ColorMetadata() {
    return anvil::app::SdrBt709ColorMetadata();
}

VideoColorMetadata FfmpegVideoDecoder::HdrBt2020PqColorMetadata() {
    return anvil::app::HdrBt2020PqColorMetadata();
}

std::shared_ptr<const DolbyVisionFrameMetadata> FfmpegVideoDecoder::ExtractDolbyVisionMetadata(const AVFrame* frame) {
    if (!frame) {
        return nullptr;
    }
    const AVFrameSideData* sideData = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!sideData || sideData->size < sizeof(AVDOVIMetadata)) {
        return nullptr;
    }

    const auto* dovi = reinterpret_cast<const AVDOVIMetadata*>(sideData->data);
    const AVDOVIRpuDataHeader* header = av_dovi_get_header(dovi);
    const AVDOVIDataMapping* mapping = av_dovi_get_mapping(dovi);
    const AVDOVIColorMetadata* color = av_dovi_get_color(dovi);
    if (!header || !mapping || !color) {
        return nullptr;
    }

    auto out = std::make_shared<DolbyVisionFrameMetadata>();
    out->valid = true;

    // Header / configuration.
    out->blBitDepth = header->bl_bit_depth > 0 ? header->bl_bit_depth : 10;
    out->elBitDepth = header->el_bit_depth > 0 ? header->el_bit_depth : 10;
    out->vdrBitDepth = header->vdr_bit_depth > 0 ? header->vdr_bit_depth : 12;
    out->blVideoFullRange = header->bl_video_full_range_flag != 0;
    out->vdrRpuNormalizedIdc = header->vdr_rpu_normalized_idc == 1;
    out->coefLog2Denom = header->coef_log2_denom;
    out->residualDisabled = header->disable_residual_flag != 0;
    out->elSpatialResampling = header->el_spatial_resampling_filter_flag != 0;
    if (out->coefLog2Denom <= 0) {
        out->coefLog2Denom = 0;  // signals "no fixed-point" — coefficients already real
    }
    const double coefScale = out->coefLog2Denom > 0
        ? 1.0 / static_cast<double>(1ULL << out->coefLog2Denom)
        : 1.0;

    // Pivot normalization range matches libplacebo: [0, 2^bl_bit_depth - 1].
    const int pivotDepth = std::clamp(out->blBitDepth, 1, 16);
    const double pivotMax = static_cast<double>((1ULL << pivotDepth) - 1);

    // Per-component piece-wise reshaping curves.
    for (int c = 0; c < kDoviNumComponents; ++c) {
        const AVDOVIReshapingCurve& src = mapping->curves[c];
        DoviReshapingCurve& dst = out->curves[c];
        dst.numPivots = std::clamp(static_cast<int>(src.num_pivots), 0, kDoviMaxPivots);
        for (int p = 0; p < dst.numPivots; ++p) {
            dst.pivots[p] = pivotMax > 0.0
                ? static_cast<float>(static_cast<double>(src.pivots[p]) / pivotMax)
                : 0.0f;
        }
        const int numPieces = std::max(0, dst.numPivots - 1);
        for (int s = 0; s < numPieces && s < kDoviMaxPieces; ++s) {
            DoviReshapingPiece& piece = dst.pieces[s];
            const auto method = src.mapping_idc[s];
            piece.method = (method == AV_DOVI_MAPPING_POLYNOMIAL)
                ? DoviMappingMethod::Polynomial
                : (method == AV_DOVI_MAPPING_MMR ? DoviMappingMethod::Mmr : DoviMappingMethod::None);
            if (piece.method == DoviMappingMethod::Polynomial) {
                piece.polyOrder = std::clamp(static_cast<int>(src.poly_order[s]), 1, 2);
                for (int k = 0; k <= piece.polyOrder && k < 3; ++k) {
                    piece.polyCoef[k] = static_cast<float>(static_cast<double>(src.poly_coef[s][k]) * coefScale);
                }
            } else if (piece.method == DoviMappingMethod::Mmr) {
                piece.mmrOrder = std::clamp(static_cast<int>(src.mmr_order[s]), 1, kDoviMmrMaxTerms);
                piece.mmrConstant = static_cast<float>(static_cast<double>(src.mmr_constant[s]) * coefScale);
                for (int t = 0; t < piece.mmrOrder && t < kDoviMmrMaxTerms; ++t) {
                    for (int k = 0; k < kDoviMmrCoeffsPerOrder; ++k) {
                        piece.mmrCoef[t][k] = static_cast<float>(static_cast<double>(src.mmr_coef[s][t][k]) * coefScale);
                    }
                }
            }
        }
    }

    out->nlqMethod = mapping->nlq_method_idc == AV_DOVI_NLQ_LINEAR_DZ
                         ? DoviNlqMethod::LinearDeadzone
                         : DoviNlqMethod::None;
    out->nlqNumXPartitions = mapping->num_x_partitions;
    out->nlqNumYPartitions = mapping->num_y_partitions;
    for (int c = 0; c < kDoviNumComponents; ++c) {
        out->nlqOffset[c] = mapping->nlq[c].nlq_offset;
        out->nlqVdrInMax[c] = mapping->nlq[c].vdr_in_max;
        out->nlqLinearDeadzoneSlope[c] = mapping->nlq[c].linear_deadzone_slope;
        out->nlqLinearDeadzoneThreshold[c] = mapping->nlq[c].linear_deadzone_threshold;
    }
    out->nlqPivots[0] = mapping->nlq_pivots[0];
    out->nlqPivots[1] = mapping->nlq_pivots[1];

    // Color matrices (YCC<->RGB and RGB->LMS, both row-major 3x3).
    for (int i = 0; i < 9; ++i) {
        out->yccToRgb[i] = static_cast<float>(RationalToDouble(color->ycc_to_rgb_matrix[i]));
        out->rgbToLms[i] = static_cast<float>(RationalToDouble(color->rgb_to_lms_matrix[i]));
        out->yccToRgbCode[i] = static_cast<int16_t>(std::clamp(
            std::llround(RationalToDouble(color->ycc_to_rgb_matrix[i]) * static_cast<double>(1 << 13)),
            static_cast<long long>(std::numeric_limits<int16_t>::min()),
            static_cast<long long>(std::numeric_limits<int16_t>::max())));
        out->rgbToLmsCode[i] = static_cast<int16_t>(std::clamp(
            std::llround(RationalToDouble(color->rgb_to_lms_matrix[i]) * static_cast<double>(1 << 14)),
            static_cast<long long>(std::numeric_limits<int16_t>::min()),
            static_cast<long long>(std::numeric_limits<int16_t>::max())));
    }
    for (int i = 0; i < 3; ++i) {
        out->yccOffset[i] = static_cast<float>(RationalToDouble(color->ycc_to_rgb_offset[i]));
        out->yccOffsetCode[i] = static_cast<uint32_t>(std::clamp(
            std::llround(RationalToDouble(color->ycc_to_rgb_offset[i]) * static_cast<double>(uint64_t{1} << 28)),
            0ll,
            static_cast<long long>(std::numeric_limits<uint32_t>::max())));
    }
    out->signalEotf = color->signal_eotf;
    out->signalEotfParam0 = color->signal_eotf_param0;
    out->signalEotfParam1 = color->signal_eotf_param1;
    out->signalEotfParam2 = color->signal_eotf_param2;
    out->signalBitDepth = color->signal_bit_depth;
    out->signalColorSpace = color->signal_color_space;
    out->signalChromaFormat = color->signal_chroma_format;
    out->signalFullRangeFlag = color->signal_full_range_flag;
    out->sourceDiagonal = color->source_diagonal;
    out->sourceMinPq = color->source_min_pq;
    out->sourceMaxPq = color->source_max_pq;
    out->sourceMinNits = Pq12CodeToNits(out->sourceMinPq);
    out->sourceMaxNits = Pq12CodeToNits(out->sourceMaxPq);

    out->dmMetadataId = color->dm_metadata_id;
    out->sceneRefreshFlag = color->scene_refresh_flag;
    uint64_t fingerprint = 14695981039346656037ull;
    fingerprint = HashValue(fingerprint, out->dmMetadataId);
    fingerprint = HashValue(fingerprint, out->sceneRefreshFlag);
    fingerprint = HashValue(fingerprint, out->sourceMinPq);
    fingerprint = HashValue(fingerprint, out->sourceMaxPq);

    const int extBlockCount = std::clamp(dovi->num_ext_blocks, 0, AV_DOVI_MAX_EXT_BLOCKS);
    for (int index = 0; index < extBlockCount; ++index) {
        const AVDOVIDmData* ext = av_dovi_get_ext(dovi, index);
        if (!ext) {
            continue;
        }
        ++out->dmExtensionBlockCount;
        if (ext->level < 64) {
            out->dmLevelMaskLow |= uint64_t{1} << ext->level;
        }
        if (dovi->ext_block_size > 0) {
            fingerprint = HashBytes(fingerprint, ext, dovi->ext_block_size);
        } else {
            fingerprint = HashValue(fingerprint, ext->level);
        }

        switch (ext->level) {
        case 1:
            out->dmLevel1Present = true;
            out->dmLevel1MinPq = ext->l1.min_pq;
            out->dmLevel1MaxPq = ext->l1.max_pq;
            out->dmLevel1AvgPq = ext->l1.avg_pq;
            break;
        case 2: {
            out->dmLevel2Present = true;
            const int targetIndex = out->dmLevel2Count;
            ++out->dmLevel2Count;
            if (targetIndex >= 0 && targetIndex < anvil::playback::kDoviMaxTrimTargets) {
                out->dmLevel2TargetMaxPq[targetIndex] = ext->l2.target_max_pq;
                out->dmLevel2TrimSlope[targetIndex] = ext->l2.trim_slope;
                out->dmLevel2TrimOffset[targetIndex] = ext->l2.trim_offset;
                out->dmLevel2TrimPower[targetIndex] = ext->l2.trim_power;
                out->dmLevel2TrimChromaWeight[targetIndex] = ext->l2.trim_chroma_weight;
                out->dmLevel2TrimSaturationGain[targetIndex] = ext->l2.trim_saturation_gain;
                out->dmLevel2MsWeight[targetIndex] = ext->l2.ms_weight;
            }
            break;
        }
        case 3:
            out->dmLevel3Present = true;
            out->dmLevel3MinPqOffset = ext->l3.min_pq_offset;
            out->dmLevel3MaxPqOffset = ext->l3.max_pq_offset;
            out->dmLevel3AvgPqOffset = ext->l3.avg_pq_offset;
            break;
        case 5:
            out->dmLevel5Present = true;
            out->dmLevel5LeftOffset = ext->l5.left_offset;
            out->dmLevel5RightOffset = ext->l5.right_offset;
            out->dmLevel5TopOffset = ext->l5.top_offset;
            out->dmLevel5BottomOffset = ext->l5.bottom_offset;
            break;
        case 8:
            out->dmLevel8Present = true;
            ++out->dmLevel8Count;
            // Keep the first L8 block in the compact summary; the level count
            // still tells us when multiple target-display trims are present.
            if (out->dmLevel8Count == 1) {
                out->dmLevel8TargetDisplayIndex = ext->l8.target_display_index;
                out->dmLevel8TrimSlope = ext->l8.trim_slope;
                out->dmLevel8TrimOffset = ext->l8.trim_offset;
                out->dmLevel8TrimPower = ext->l8.trim_power;
                out->dmLevel8TrimChromaWeight = ext->l8.trim_chroma_weight;
                out->dmLevel8TrimSaturationGain = ext->l8.trim_saturation_gain;
                out->dmLevel8MsWeight = ext->l8.ms_weight;
                out->dmLevel8TargetMidContrast = ext->l8.target_mid_contrast;
                out->dmLevel8ClipTrim = ext->l8.clip_trim;
            }
            break;
        case 254:
            out->dmLevel254Present = true;
            break;
        case 255:
            out->dmLevel255Present = true;
            break;
        default:
            break;
        }
    }
    out->dynamicMetadataFingerprint = fingerprint;

    return out;
}

std::shared_ptr<const DolbyVisionFrameMetadata> FfmpegVideoDecoder::ExtractFrameDolbyVisionMetadata(const AVFrame* frame) {
    auto metadata = ExtractDolbyVisionMetadata(frame);
    if (!metadata) {
        return nullptr;
    }
    // Seed with stream-level configuration (not present in per-frame metadata).
    // shared_ptr<const> prevents in-place mutation, so copy-construct a mutable
    // version, patch the stream fields, and re-wrap.
    auto seeded = std::make_shared<DolbyVisionFrameMetadata>(*metadata);
    seeded->profile = dolbyVisionProfile_;
    seeded->level = dolbyVisionLevel_;
    seeded->compatibilityId = dolbyVisionCompatId_;
    seeded->elPresent = dolbyVisionElPresent_;
    seeded->blPresent = dolbyVisionBlPresent_;
    return seeded;
}

std::shared_ptr<const DolbyVisionFrameMetadata> FfmpegVideoDecoder::ExtractEnhancementDolbyVisionMetadata(const AVFrame* frame) {
    auto metadata = ExtractDolbyVisionMetadata(frame);
    if (!metadata) {
        return nullptr;
    }

    auto seeded = std::make_shared<DolbyVisionFrameMetadata>(*metadata);
    seeded->profile = dolbyVisionEnhancementProfile_;
    seeded->level = dolbyVisionEnhancementLevel_;
    seeded->compatibilityId = dolbyVisionEnhancementCompatId_;
    seeded->elPresent = dolbyVisionEnhancementElPresent_;
    seeded->blPresent = dolbyVisionEnhancementBlPresent_;
    return seeded;
}

bool FfmpegVideoDecoder::IsSupportedHardwareTextureFormat(const DXGI_FORMAT format) {
    return format == DXGI_FORMAT_NV12 ||
           format == DXGI_FORMAT_P010 ||
           format == DXGI_FORMAT_P016;
}

std::wstring FfmpegVideoDecoder::DxgiFormatName(const DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_NV12: return L"NV12";
    case DXGI_FORMAT_P010: return L"P010";
    case DXGI_FORMAT_P016: return L"P016";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return L"BGRA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return L"RGBA8";
    default: return L"DXGI_FORMAT_" + std::to_wstring(static_cast<int>(format));
    }
}

std::wstring FfmpegVideoDecoder::PixelFormatName(const AVPixelFormat format) {
    const char* name = av_get_pix_fmt_name(format);
    if (name && *name) {
        return Utf8ToWide(name);
    }
    return L"unknown(" + std::to_wstring(static_cast<int>(format)) + L")";
}

void FfmpegVideoDecoder::LogZeroCopyFallbackOnce(const std::wstring& reason) {
    if (zeroCopyFallbackLogged_) {
        return;
    }
    zeroCopyFallbackLogged_ = true;
    LogThread(LogLevel::Warning, L"decoder", L"zero_copy_fallback=cpu_transfer reason=" + reason);
}

std::shared_ptr<std::vector<uint8_t>> FfmpegVideoDecoder::AcquireReusableBgraBuffer(const std::size_t needed) {
    if (needed == 0 || needed > kDecodedFrameMemoryBudgetBytes) {
        return {};
    }

    std::shared_ptr<std::vector<uint8_t>> buffer;
    bool retainedInPool = false;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& retained : reusableBgraBuffers_) {
            if (retained && retained.use_count() == 1 && retained->capacity() >= needed) {
                buffer = retained;
                retainedInPool = true;
                break;
            }
        }

        if (!buffer && needed <= kReusableBufferMemoryBudgetBytes) {
            for (auto it = reusableBgraBuffers_.begin(); it != reusableBgraBuffers_.end(); ++it) {
                if (*it && it->use_count() == 1) {
                    buffer = *it;
                    reusableBgraBuffers_.erase(it);
                    break;
                }
            }
        }
    }

    try {
        if (!buffer) {
            buffer = std::make_shared<std::vector<uint8_t>>();
        }
        buffer->resize(needed);
    } catch (const std::bad_alloc&) {
        return {};
    }

    if (buffer->capacity() > kDecodedFrameMemoryBudgetBytes) {
        return {};
    }

    if (!retainedInPool && buffer->capacity() <= kReusableBufferMemoryBudgetBytes) {
        std::scoped_lock lock(mutex_);
        std::size_t retainedBytes = 0;
        for (const auto& retained : reusableBgraBuffers_) {
            retainedBytes = SaturatingAddBytes(retainedBytes, retained ? retained->capacity() : 0);
        }
        if (reusableBgraBuffers_.size() < kMaxReusableBgraBuffers &&
            FitsWithinBudget(retainedBytes, buffer->capacity(), kReusableBufferMemoryBudgetBytes)) {
            reusableBgraBuffers_.push_back(buffer);
        }
    }
    return buffer;
}

std::size_t FfmpegVideoDecoder::FrameQueueCostBytes(const NativeVideoFrame& frame) {
    std::size_t bytes = 0;
    bytes = SaturatingAddBytes(bytes, BufferCapacityBytes(frame.yuv.data));
    bytes = SaturatingAddBytes(bytes, BufferCapacityBytes(frame.bgra));
    bytes = SaturatingAddBytes(bytes, BufferCapacityBytes(frame.enhancementYuv.data));
    return bytes;
}

std::size_t FfmpegVideoDecoder::MaxQueueDepthForFrame(const NativeVideoFrame& frame) {
    if (frame.HasD3DTexture()) {
        return kMaxHardwareQueuedFrames;
    }
    if (frame.HasYuv()) {
        return kMaxYuvQueuedFrames;
    }

    const std::size_t bytes = FrameQueueCostBytes(frame);
    constexpr std::size_t kLargeFrameBytes = 24ull * 1024ull * 1024ull;
    return bytes >= kLargeFrameBytes ? kMaxLargeBgraQueuedFrames : kMaxSmallBgraQueuedFrames;
}

bool FfmpegVideoDecoder::HasQueueCapacityLocked(const NativeVideoFrame& frame) const {
    if (frameQueue_.size() >= MaxQueueDepthForFrame(frame)) {
        return false;
    }

    const std::size_t incomingBytes = FrameQueueCostBytes(frame);
    if (incomingBytes > kMaxCpuQueuedFrameBytes) {
        return false;
    }

    std::size_t queuedBytes = FrameQueueCostBytes(latestFrame_);
    if (queuedBytes > kMaxCpuQueuedFrameBytes) {
        return false;
    }
    for (const auto& queued : frameQueue_) {
        const std::size_t queuedFrameBytes = FrameQueueCostBytes(queued);
        if (!FitsWithinBudget(queuedBytes, queuedFrameBytes, kMaxCpuQueuedFrameBytes)) {
            return false;
        }
        queuedBytes += queuedFrameBytes;
    }

    return FitsWithinBudget(queuedBytes, incomingBytes, kMaxCpuQueuedFrameBytes);
}

void FfmpegVideoDecoder::UpdateBufferedStatsLocked() {
    stats_.timelineSerial = CurrentTimelineSerial();
    stats_.queueDepth = frameQueue_.size();
    const auto decodedStart = latestFrame_.HasContent()
                                  ? latestFrame_.pts
                                  : (frameQueue_.empty() ? stats_.clockPosition : frameQueue_.front().pts);
    std::chrono::milliseconds decodedEnd = decodedStart;
    std::chrono::milliseconds decodedDuration{0};
    if (frameQueue_.empty()) {
        if (latestFrame_.HasContent()) {
            decodedEnd = latestFrame_.pts;
        }
        stats_.bufferedEnd = std::max(decodedEnd, stats_.readAheadEnd);
        stats_.bufferedDuration = std::max(stats_.readAheadDuration,
                                           std::max(std::chrono::milliseconds{0},
                                                    stats_.bufferedEnd - decodedStart));
        return;
    }

    const auto end = frameQueue_.back().pts;
    decodedEnd = std::max(decodedStart, end);
    decodedDuration = std::max(std::chrono::milliseconds{0}, decodedEnd - decodedStart);
    stats_.bufferedEnd = std::max(decodedEnd, stats_.readAheadEnd);
    stats_.bufferedDuration = std::max(decodedDuration,
                                       std::max(stats_.readAheadDuration,
                                                std::max(std::chrono::milliseconds{0},
                                                         stats_.bufferedEnd - decodedStart)));
}

bool FfmpegVideoDecoder::SeekPrerollReadyLocked() const {
    if (!seekPrerollPending_.load() || playbackPaused_.load()) {
        return true;
    }
    if (frameQueue_.empty()) {
        return false;
    }

    const bool softwareFrame = frameQueue_.front().HasYuv() || frameQueue_.front().HasPixels();
    const bool enhancementOverlayFrame =
        softwareFrame && enableDolbyVisionEnhancementDecode_ && dolbyVisionEnhancementActive_;
    const auto maxQueueDepth = MaxQueueDepthForFrame(frameQueue_.front());
    const auto effectiveMaxQueueDepth = maxQueueDepth > 1 ? maxQueueDepth - 1 : maxQueueDepth;
    const auto minQueuedFrames = enhancementOverlayFrame
                                     ? std::min(kSeekPrerollEnhancementMinQueuedFrames, effectiveMaxQueueDepth)
                                     : (softwareFrame
                                            ? std::min(kSeekPrerollSoftwareMinQueuedFrames,
                                                       effectiveMaxQueueDepth)
                                            : kSeekPrerollMinQueuedFrames);
    const auto minReadAhead = enhancementOverlayFrame
                                  ? kSeekPrerollEnhancementMinReadAhead
                                  : (softwareFrame ? kSeekPrerollSoftwareMinReadAhead
                                                   : kSeekPrerollMinReadAhead);
    const auto timeout = enhancementOverlayFrame
                             ? kSeekPrerollTimeout
                             : (softwareFrame ? kSeekPrerollSoftwareTimeout
                                              : kSeekPrerollTimeout);
    const auto minDecodedSpan = enhancementOverlayFrame
                                    ? std::chrono::milliseconds{80}
                                    : (softwareFrame ? std::chrono::milliseconds{300}
                                                     : std::chrono::milliseconds{160});
    const auto decodedSpan = frameQueue_.back().pts - frameQueue_.front().pts;
    const bool decodedQueueFull = frameQueue_.size() >= effectiveMaxQueueDepth;
    const bool decodedQueueAtCapacity = frameQueue_.size() >= maxQueueDepth;
    // Large 4K libplacebo frames usually hit the 128 MiB byte budget before
    // the nominal six-frame depth. Treat that byte saturation as a full queue;
    // otherwise seek preroll waits for a fifth frame that can never be queued.
    const bool softwareQueueSaturated =
        softwareFrame &&
        (decodedQueueFull || !HasQueueCapacityLocked(frameQueue_.front()));
    const bool queuedFramesReady = frameQueue_.size() >= minQueuedFrames;
    const bool videoReady = softwareFrame
                                ? (queuedFramesReady || softwareQueueSaturated)
                                : (queuedFramesReady || decodedSpan >= minDecodedSpan);
    const bool softwareQueueFullWithLead =
        softwareQueueSaturated &&
        stats_.readAheadDuration >= kSeekPrerollSoftwareQueueFullMinReadAhead;
    const bool hardwareQueueFull =
        !softwareFrame &&
        decodedQueueAtCapacity;
    const bool readAheadReady =
        stats_.readAheadDuration >= minReadAhead ||
        softwareQueueFullWithLead ||
        hardwareQueueFull ||
        (!softwareFrame && stats_.packetQueueDepth >= kSeekPrerollMinPacketDepth);
    if (videoReady && readAheadReady) {
        return true;
    }

    if (seekRecovery_.startedAt.time_since_epoch().count() <= 0) {
        return false;
    }
    if (std::chrono::steady_clock::now() - seekRecovery_.startedAt < timeout) {
        return false;
    }
    if (enhancementOverlayFrame) {
        return videoReady && stats_.readAheadDuration >= kSeekPrerollEnhancementTimeoutMinReadAhead;
    }
    if (softwareFrame) {
        // After the (long) software-frame timeout, release as soon as we have
        // enough decoded frames. Requiring a large read-ahead here deadlocks
        // network sources whose read-ahead stalls while the decode loop is
        // blocked on a full frame queue.
        return videoReady;
    }
    return videoReady && stats_.readAheadDuration >= kSeekPrerollTimeoutMinReadAhead;
}

bool FfmpegVideoDecoder::EnqueueFrame(NativeVideoFrame&& frame) {
    frame.frameRateNumerator = videoFrameRateNumerator_;
    frame.frameRateDenominator = videoFrameRateDenominator_;
    if (!frame.subtitlesPrepared) {
        RefreshFrameSubtitles(frame);
    }
    const std::size_t incomingBytes = FrameQueueCostBytes(frame);
    if (incomingBytes > kMaxCpuQueuedFrameBytes) {
        {
            std::scoped_lock lock(mutex_);
            ++stats_.droppedQueueFull;
        }
        LogThread(LogLevel::Error,
                  L"decoder",
                  L"decoded_frame exceeds_memory_budget bytes=" + std::to_wstring(incomingBytes) +
                      L" budget_bytes=" + std::to_wstring(kMaxCpuQueuedFrameBytes));
        return false;
    }

    while (!stopping_.load()) {
        if (HasPendingSeek()) {
            return true;
        }
        const uint64_t activeSerial = CurrentTimelineSerial();
        if (frame.timelineSerial != 0 && frame.timelineSerial != activeSerial) {
            std::scoped_lock lock(mutex_);
            ++stats_.droppedStale;
            UpdateBufferedStatsLocked();
            return true;
        }
        WakeScheduler();
        bool queued = false;
        {
            std::unique_lock lock(mutex_);
            DropStaleFramesLocked();
            const uint64_t currentSerial = CurrentTimelineSerial();
            if (frame.timelineSerial != 0 && frame.timelineSerial != currentSerial) {
                ++stats_.droppedStale;
                UpdateBufferedStatsLocked();
                return true;
            }
            if (HasQueueCapacityLocked(frame)) {
                frameQueue_.push_back(std::move(frame));
                stats_.buffering = seekPrerollPending_.load();
                UpdateBufferedStatsLocked();
                queued = true;
            } else if (playbackPaused_.load()) {
                ++stats_.droppedQueueFull;
                queued = true;
            } else {
                frameQueueCv_.wait_for(lock, std::chrono::milliseconds{2}, [this, &frame]() {
                    return stopping_.load() ||
                           HasPendingSeek() ||
                           playbackPaused_.load() ||
                           HasQueueCapacityLocked(frame);
                });
            }
        }
        if (queued) {
            WakeScheduler();
            return true;
        }
    }
    return false;
}

void FfmpegVideoDecoder::DrainQueuedFrames() {
    WakeScheduler();
    std::unique_lock lock(mutex_);
    frameQueueCv_.wait(lock, [this]() {
        return stopping_.load() || frameQueue_.empty();
    });
}

void FfmpegVideoDecoder::WakeScheduler() {
    {
        std::scoped_lock lock(schedulerMutex_);
        schedulerWakeRequested_ = true;
    }
    schedulerCv_.notify_one();
}

void FfmpegVideoDecoder::SchedulerLoop() {
    std::unique_lock lock(schedulerMutex_);
    while (!stopping_.load() && running_.load()) {
        schedulerWakeRequested_ = false;
        lock.unlock();
        ScheduleDueFrames();
        bool hasQueuedFrames = false;
        {
            std::scoped_lock queueLock(mutex_);
            hasQueuedFrames = !frameQueue_.empty();
        }
        lock.lock();
        const auto wakeRequested = [this]() {
            return stopping_.load() || !running_.load() || schedulerWakeRequested_;
        };
        if (hasQueuedFrames) {
            schedulerCv_.wait_for(lock, std::chrono::milliseconds{2}, wakeRequested);
        } else {
            schedulerCv_.wait(lock, wakeRequested);
        }
    }
}

void FfmpegVideoDecoder::ScheduleDueFrames() {
    NativeVideoFrame frameToPublish;
    bool hasFrame = false;
    bool firstPublishedFrame = false;
    bool seekPrerollReleased = false;
    std::size_t publishedQueueDepth = 0;
    std::size_t seekPrerollQueueDepth = 0;
    std::size_t seekPrerollPacketDepth = 0;
    int seekPrerollReadAheadMs = 0;
    int seekPrerollWaitMs = 0;
    std::chrono::milliseconds publishedClockPosition{0};
    bool queueChanged = false;

    // Seek() is a window-thread control operation.  Once it publishes a
    // pending request, never let this worker publish an old queued frame while
    // the decode worker is still interrupting or completing FFmpeg seek I/O.
    if (HasPendingSeek()) {
        return;
    }

    {
        std::scoped_lock lock(mutex_);
        ApplyPendingClockResetLocked();
        if (HasPendingSeek()) {
            return;
        }
        if (frameQueue_.empty()) {
            UpdateBufferedStatsLocked();
            return;
        }
    }

    std::optional<std::chrono::milliseconds> audioClock;
    if (!playbackPaused_.load()) {
        ClockCallback callback;
        {
            std::scoped_lock lock(clockCallbackMutex_);
            callback = clockCallback_;
        }
        if (callback) {
            audioClock = callback();
        }
    }

    {
        std::scoped_lock lock(mutex_);
        ApplyPendingClockResetLocked();
        if (HasPendingSeek()) {
            return;
        }
        const std::size_t queueDepthBeforeStaleDrop = frameQueue_.size();
        DropStaleFramesLocked();
        queueChanged = frameQueue_.size() != queueDepthBeforeStaleDrop;
        if (frameQueue_.empty()) {
            UpdateBufferedStatsLocked();
            if (queueChanged) {
                frameQueueCv_.notify_all();
            }
            return;
        }

        if (seekPrerollPending_.load()) {
            UpdateBufferedStatsLocked();
            if (!SeekPrerollReadyLocked()) {
                stats_.buffering = true;
                if (queueChanged) {
                    frameQueueCv_.notify_all();
                }
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            seekPrerollPending_.store(false);
            seekPrerollReleased = true;
            seekPrerollQueueDepth = frameQueue_.size();
            seekPrerollPacketDepth = stats_.packetQueueDepth;
            seekPrerollReadAheadMs = static_cast<int>(stats_.readAheadDuration.count());
            if (seekRecovery_.startedAt.time_since_epoch().count() > 0) {
                seekPrerollWaitMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - seekRecovery_.startedAt).count());
            }
            StartSeekRecoveryVisualWarmupLocked(now);
            schedulePrimed_ = false;
        }

        const bool seekRecoveryVisualWarmup = seekRecovery_.phase == SeekRecoveryPhase::VisualWarmup;
        auto clock = seekRecoveryVisualWarmup
                         ? SeekRecoverySchedulerClockLocked(frameQueue_.front().pts, audioClock)
                         : CurrentSchedulerClockLocked(frameQueue_.front().pts, audioClock);
        if (!seekRecoveryVisualWarmup && !schedulePrimed_ && stats_.rendered == 0) {
            clock.position = frameQueue_.front().pts;
            clock.usingAudioClock = false;
            schedulePrimed_ = true;
        } else if (seekRecoveryVisualWarmup) {
            schedulePrimed_ = true;
        }
        stats_.clockPosition = clock.position;
        stats_.usingAudioClock = clock.usingAudioClock;

        auto frameEarlyTolerance = [&]() {
            auto tolerance = kMaxFrameEarlyTolerance;
            std::chrono::milliseconds cadence{0};
            if (frameQueue_.size() >= 2) {
                const auto measured = frameQueue_[1].pts - frameQueue_[0].pts;
                if (measured > std::chrono::milliseconds{0} && measured < kMaxMeasuredFrameCadence) {
                    cadence = measured;
                    tolerance = std::clamp(measured / 4, kMinFrameEarlyTolerance, kMaxFrameEarlyTolerance);
                }
            }
            stats_.frameCadenceMs = static_cast<int>(cadence.count());
            stats_.earlyToleranceMs = static_cast<int>(tolerance.count());
            return tolerance;
        };

        while (!frameQueue_.empty()) {
            const NativeVideoFrame& front = frameQueue_.front();
            const auto earlyBy = front.pts - clock.position;
            const auto lateBy = clock.position - front.pts;
            if (!seekRecoveryVisualWarmup &&
                lateBy > kFrameLateDropThreshold &&
                frameQueue_.size() > 1) {
                frameQueue_.pop_front();
                queueChanged = true;
                UpdateBufferedStatsLocked();
                ++stats_.droppedLate;
                continue;
            }

            const auto earlyTolerance = frameEarlyTolerance();
            const bool frameIsDue = earlyBy <= earlyTolerance;
            if (!frameIsDue) {
                break;
            }

            NativeVideoFrame candidate = std::move(frameQueue_.front());
            frameQueue_.pop_front();
            queueChanged = true;
            UpdateBufferedStatsLocked();

            const bool anotherFrameDue = !frameQueue_.empty() &&
                (frameQueue_.front().pts - clock.position) <= earlyTolerance;
            if (anotherFrameDue) {
                ++stats_.droppedLate;
                ++stats_.droppedSuperseded;
                continue;
            }

            frameToPublish = std::move(candidate);
            hasFrame = true;
            break;
        }

        if (hasFrame) {
            stats_.driftMs = static_cast<int>((frameToPublish.pts - clock.position).count());
            ++stats_.rendered;
            firstPublishedFrame = stats_.rendered == 1;
            publishedClockPosition = clock.position;
            latestFrame_ = frameToPublish;
            UpdateBufferedStatsLocked();
            UpdateSeekRecoveryAfterPublishLocked(frameToPublish.pts, std::chrono::steady_clock::now());
            publishedQueueDepth = stats_.queueDepth;
        }
    }

    if (queueChanged) {
        frameQueueCv_.notify_all();
    }

    if (hasFrame) {
        if (seekPrerollReleased) {
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"seek_preroll_ready frames=" + std::to_wstring(seekPrerollQueueDepth) +
                          L" packets=" + std::to_wstring(seekPrerollPacketDepth) +
                          L" read_ahead_ms=" + std::to_wstring(seekPrerollReadAheadMs) +
                          L" wait_ms=" + std::to_wstring(seekPrerollWaitMs));
        }
        if (firstPublishedFrame) {
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"schedule_publish_first pts_ms=" + std::to_wstring(frameToPublish.pts.count()) +
                          L" clock_ms=" + std::to_wstring(publishedClockPosition.count()) +
                          L" queue_depth=" + std::to_wstring(publishedQueueDepth));
        }
        NotifyFrameReady();
    }
}

FfmpegVideoDecoder::SchedulerClock FfmpegVideoDecoder::CurrentSchedulerClockLocked(
    const std::chrono::milliseconds firstQueuedPts,
    const std::optional<std::chrono::milliseconds>& audioClock) {
    if (playbackPaused_.load()) {
        return {std::chrono::milliseconds{pausedPositionMs_.load()}, false};
    }

    if (audioClock.has_value()) {
        return {*audioClock, true};
    }

    const auto now = std::chrono::steady_clock::now();
    if (!fallbackClockAnchor_.has_value()) {
        fallbackClockAnchor_ = now;
        fallbackClockBasePts_ = firstQueuedPts;
        return {firstQueuedPts, false};
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *fallbackClockAnchor_);
    return {fallbackClockBasePts_ + elapsed, false};
}

void FfmpegVideoDecoder::NotifyFrameReady() {
    const HWND window = notificationWindow_.load();
    const UINT message = notificationMessage_.load();
    if (!window || message == 0 || frameMessagePending_.exchange(true)) {
        return;
    }
    if (!PostMessageW(window, message, static_cast<WPARAM>(notificationCookie_.load()), 0)) {
        frameMessagePending_.store(false);
    }
}

void FfmpegVideoDecoder::NotifyDecodeFailure(const std::wstring& message) const {
    const HWND window = notificationWindow_.load();
    const UINT failureMessage = failureMessage_.load();
    if (!window || failureMessage == 0 || oneShotFrame_ || stopping_.load()) {
        return;
    }

    auto* payload = new (std::nothrow) NativeDecodeFailure{path_, message, notificationCookie_.load()};
    if (!payload) {
        return;
    }
    if (!PostMessageW(window, failureMessage, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void FfmpegVideoDecoder::SetDecodeBackend(const std::wstring& decoder, const bool usingHardware, const std::wstring& fallbackReason) {
    std::scoped_lock lock(mutex_);
    stats_.decoder = decoder;
    stats_.usingHardwareDecode = usingHardware;
    stats_.fallbackReason = fallbackReason;
}

void FfmpegVideoDecoder::LogThread(const LogLevel level, const std::wstring& category, const std::wstring& message) const {
    if (logSink_) {
        logSink_->Write(level, category, message);
    }
    OutputDebugStringW((L"[" + category + L"] " + message + L"\n").c_str());
}

void FfmpegVideoDecoder::LogThreadError(const std::wstring& message) const {
    LogThread(LogLevel::Error, L"native_decode", message);
}

}  // namespace anvil::app
