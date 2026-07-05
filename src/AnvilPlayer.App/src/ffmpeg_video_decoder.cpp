#include "AnvilPlayer/App/ffmpeg_video_decoder.h"

#include "AnvilPlayer/App/string_util.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace anvil::app {

using anvil::playback::LogLevel;
using anvil::playback::DolbyVisionFrameMetadata;
using anvil::playback::DoviMappingMethod;
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

double RationalToDouble(const AVRational value) {
    if (value.num <= 0 || value.den <= 0) {
        return 0.0;
    }
    const double result = av_q2d(value);
    return std::isfinite(result) ? result : 0.0;
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

anvil::playback::ChromaticityPoint MakePoint(const AVRational x, const AVRational y) {
    return anvil::playback::ChromaticityPoint{RationalToDouble(x), RationalToDouble(y)};
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

std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool IsAutoLanguage(const std::wstring& value) {
    const auto normalized = ToLowerWide(value);
    return normalized.empty() || normalized == L"auto" || normalized == L"default";
}

std::wstring StreamLanguage(const AVStream* stream) {
    if (!stream) {
        return {};
    }
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
    return language ? Utf8ToWide(language->value) : L"";
}

bool LanguageMatches(const std::wstring& desired, const std::wstring& actual) {
    if (IsAutoLanguage(desired)) {
        return true;
    }
    const auto wanted = ToLowerWide(desired);
    const auto candidate = ToLowerWide(actual);
    return !candidate.empty() &&
           (candidate == wanted ||
            candidate.rfind(wanted, 0) == 0 ||
           wanted.rfind(candidate, 0) == 0);
}

bool IsSupportedExternalSubtitleExtension(const std::filesystem::path& path) {
    const auto extension = ToLowerWide(path.extension().wstring());
    return extension == L".srt" ||
           extension == L".ass" ||
           extension == L".ssa" ||
           extension == L".vtt";
}

bool StartsWithSubtitleStem(const std::wstring& candidateStem, const std::wstring& mediaStem) {
    if (candidateStem == mediaStem) {
        return true;
    }
    if (candidateStem.size() <= mediaStem.size() ||
        candidateStem.rfind(mediaStem, 0) != 0) {
        return false;
    }

    const wchar_t separator = candidateStem[mediaStem.size()];
    return separator == L'.' || separator == L'-' || separator == L'_' || separator == L' ';
}

std::wstring SubtitleStemSuffix(std::wstring candidateStem, const std::wstring& mediaStem) {
    if (candidateStem.size() <= mediaStem.size()) {
        return {};
    }
    candidateStem.erase(0, mediaStem.size());
    while (!candidateStem.empty() &&
           (candidateStem.front() == L'.' ||
            candidateStem.front() == L'-' ||
            candidateStem.front() == L'_' ||
            candidateStem.front() == L' ')) {
        candidateStem.erase(candidateStem.begin());
    }
    return candidateStem;
}

int ExternalSubtitleScore(const std::filesystem::path& subtitlePath,
                          const std::wstring& mediaStem,
                          const std::wstring& preferredLanguage) {
    const auto candidateStem = ToLowerWide(subtitlePath.stem().wstring());
    if (!StartsWithSubtitleStem(candidateStem, mediaStem)) {
        return -1;
    }

    const auto suffix = SubtitleStemSuffix(candidateStem, mediaStem);
    int score = suffix.empty() ? 1000 : 500;
    if (!IsAutoLanguage(preferredLanguage) && LanguageMatches(preferredLanguage, suffix)) {
        score += 2000;
    }
    if (suffix.find(L"forced") != std::wstring::npos) {
        score += 10;
    }
    return score;
}

std::optional<std::filesystem::path> FindExternalSubtitleFile(const std::filesystem::path& mediaPath,
                                                              const std::wstring& preferredLanguage) {
    const auto directory = mediaPath.parent_path();
    if (directory.empty()) {
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        return std::nullopt;
    }

    const auto mediaStem = ToLowerWide(mediaPath.stem().wstring());
    std::optional<std::filesystem::path> bestPath;
    int bestScore = -1;
    std::wstring bestPathText;

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const auto path = entry.path();
        if (path == mediaPath || !IsSupportedExternalSubtitleExtension(path)) {
            continue;
        }
        const int score = ExternalSubtitleScore(path, mediaStem, preferredLanguage);
        if (score < 0) {
            continue;
        }
        const auto pathText = ToLowerWide(path.filename().wstring());
        if (score > bestScore || (score == bestScore && (bestPathText.empty() || pathText < bestPathText))) {
            bestScore = score;
            bestPath = path;
            bestPathText = pathText;
        }
    }
    return bestPath;
}

std::wstring TrimSubtitleLine(std::wstring value) {
    const auto notSpace = [](const wchar_t ch) {
        return !std::iswspace(ch) || ch == L'\n';
    };
    while (!value.empty() && !notSpace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && !notSpace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring NormalizeSubtitleText(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch == L'\r' || ch == L'\t') {
            ch = ch == L'\t' ? L' ' : L'\n';
        }
    }

    std::wstring normalized;
    bool previousSpace = false;
    bool previousNewline = false;
    for (const wchar_t ch : text) {
        if (ch == L'\n') {
            while (!normalized.empty() && normalized.back() == L' ') {
                normalized.pop_back();
            }
            if (!previousNewline && !normalized.empty()) {
                normalized.push_back(L'\n');
            }
            previousSpace = false;
            previousNewline = true;
            continue;
        }
        if (std::iswspace(ch)) {
            if (!previousSpace && !previousNewline) {
                normalized.push_back(L' ');
            }
            previousSpace = true;
            continue;
        }
        normalized.push_back(ch);
        previousSpace = false;
        previousNewline = false;
    }
    while (!normalized.empty() && (normalized.back() == L' ' || normalized.back() == L'\n')) {
        normalized.pop_back();
    }
    return TrimSubtitleLine(std::move(normalized));
}

std::string AssDialogueText(std::string value) {
    constexpr char kDialoguePrefix[] = "Dialogue:";
    if (value.rfind(kDialoguePrefix, 0) == 0) {
        value.erase(0, std::strlen(kDialoguePrefix));
        while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }
    }

    std::vector<std::size_t> commaPositions;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == ',') {
            commaPositions.push_back(index);
            if (commaPositions.size() >= 9) {
                break;
            }
        }
    }
    if (commaPositions.size() >= 2) {
        const std::string secondField =
            value.substr(commaPositions[0] + 1, commaPositions[1] - commaPositions[0] - 1);
        const std::size_t commasBeforeText = secondField.find(':') != std::string::npos ? 9 : 8;
        if (commaPositions.size() >= commasBeforeText) {
            return value.substr(commaPositions[commasBeforeText - 1] + 1);
        }
    }

    const auto lastComma = value.find_last_of(',');
    return lastComma == std::string::npos ? value : value.substr(lastComma + 1);
}

std::wstring PlainSubtitleText(const char* utf8, const bool ass) {
    if (!utf8 || *utf8 == '\0') {
        return {};
    }

    std::string value = ass ? AssDialogueText(utf8) : std::string(utf8);
    std::string plain;
    bool inAssOverride = false;
    bool inHtmlTag = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ass && ch == '{') {
            inAssOverride = true;
            continue;
        }
        if (inAssOverride) {
            if (ch == '}') {
                inAssOverride = false;
            }
            continue;
        }
        if (ch == '<') {
            inHtmlTag = true;
            continue;
        }
        if (inHtmlTag) {
            if (ch == '>') {
                inHtmlTag = false;
            }
            continue;
        }
        if (ch == '\\' && index + 1 < value.size()) {
            const char next = value[index + 1];
            if (next == 'N' || next == 'n') {
                plain.push_back('\n');
                ++index;
                continue;
            }
            if (next == 'h') {
                plain.push_back(' ');
                ++index;
                continue;
            }
        }
        plain.push_back(ch);
    }
    return NormalizeSubtitleText(Utf8ToWide(plain.c_str()));
}

std::wstring SubtitleTextFromDecoded(const AVSubtitle& subtitle) {
    std::vector<std::wstring> lines;
    for (unsigned index = 0; index < subtitle.num_rects; ++index) {
        const AVSubtitleRect* rect = subtitle.rects ? subtitle.rects[index] : nullptr;
        if (!rect) {
            continue;
        }
        std::wstring text;
        if (rect->type == SUBTITLE_TEXT) {
            text = PlainSubtitleText(rect->text, false);
        } else if (rect->type == SUBTITLE_ASS) {
            text = PlainSubtitleText(rect->ass, true);
        }
        if (!text.empty()) {
            lines.push_back(std::move(text));
        }
    }

    std::wstring joined;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            joined += L"\n";
        }
        joined += lines[index];
    }
    return NormalizeSubtitleText(std::move(joined));
}

uint8_t Premultiply(const uint8_t value, const uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<unsigned int>(value) * alpha + 127) / 255);
}

uint16_t ReadBigEndian16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<unsigned int>(data[0]) << 8) | data[1]);
}

bool ValidSubtitleCanvas(const int width, const int height) {
    constexpr int kMaxSubtitleCanvasDimension = 16384;
    return width > 0 && height > 0 &&
           width <= kMaxSubtitleCanvasDimension &&
           height <= kMaxSubtitleCanvasDimension;
}

std::optional<std::pair<int, int>> PgsCanvasFromPayload(const uint8_t* payload, const std::size_t payloadSize) {
    if (!payload || payloadSize < 4) {
        return std::nullopt;
    }

    const int width = ReadBigEndian16(payload);
    const int height = ReadBigEndian16(payload + 2);
    if (!ValidSubtitleCanvas(width, height)) {
        return std::nullopt;
    }
    return std::pair<int, int>{width, height};
}

std::optional<std::pair<int, int>> PgsCanvasFromPacket(const AVPacket* packet) {
    if (!packet || !packet->data || packet->size <= 0) {
        return std::nullopt;
    }

    const auto* data = packet->data;
    const std::size_t size = static_cast<std::size_t>(packet->size);

    std::size_t offset = 0;
    while (offset + 13 <= size && data[offset] == 'P' && data[offset + 1] == 'G') {
        const uint8_t segmentType = data[offset + 10];
        const std::size_t segmentSize = ReadBigEndian16(data + offset + 11);
        const std::size_t payloadOffset = offset + 13;
        if (payloadOffset + segmentSize > size) {
            break;
        }
        if (segmentType == 0x16) {
            if (auto canvas = PgsCanvasFromPayload(data + payloadOffset, segmentSize)) {
                return canvas;
            }
        }
        offset = payloadOffset + segmentSize;
    }

    offset = 0;
    while (offset + 3 <= size) {
        const uint8_t segmentType = data[offset];
        const std::size_t segmentSize = ReadBigEndian16(data + offset + 1);
        const std::size_t payloadOffset = offset + 3;
        if (payloadOffset + segmentSize > size) {
            break;
        }
        if (segmentType == 0x16) {
            if (auto canvas = PgsCanvasFromPayload(data + payloadOffset, segmentSize)) {
                return canvas;
            }
        }
        offset = payloadOffset + segmentSize;
    }

    return std::nullopt;
}

std::vector<NativeSubtitleBitmap> SubtitleBitmapsFromDecoded(const AVSubtitle& subtitle,
                                                             const int canvasWidth,
                                                             const int canvasHeight,
                                                             uint64_t& serial) {
    std::vector<NativeSubtitleBitmap> bitmaps;
    for (unsigned rectIndex = 0; rectIndex < subtitle.num_rects; ++rectIndex) {
        const AVSubtitleRect* rect = subtitle.rects ? subtitle.rects[rectIndex] : nullptr;
        if (!rect || rect->type != SUBTITLE_BITMAP || rect->w <= 0 || rect->h <= 0 ||
            !rect->data[0] || !rect->data[1] || rect->linesize[0] == 0) {
            continue;
        }

        const int stride = rect->w * 4;
        const std::size_t needed = static_cast<std::size_t>(stride) * static_cast<std::size_t>(rect->h);
        auto pixels = std::make_shared<std::vector<uint8_t>>(needed);
        const auto* palette = reinterpret_cast<const uint32_t*>(rect->data[1]);
        const int colorCount = rect->nb_colors > 0 ? rect->nb_colors : 256;

        for (int y = 0; y < rect->h; ++y) {
            const uint8_t* sourceRow = rect->linesize[0] > 0
                                           ? rect->data[0] + static_cast<std::size_t>(rect->linesize[0]) * y
                                           : rect->data[0] + static_cast<std::size_t>(-rect->linesize[0]) * (rect->h - 1 - y);
            uint8_t* destinationRow = pixels->data() + static_cast<std::size_t>(stride) * y;
            for (int x = 0; x < rect->w; ++x) {
                const uint8_t paletteIndex = sourceRow[x];
                if (paletteIndex >= colorCount) {
                    continue;
                }
                const uint32_t color = palette[paletteIndex];
                const uint8_t alpha = static_cast<uint8_t>((color >> 24) & 0xff);
                const uint8_t red = static_cast<uint8_t>((color >> 16) & 0xff);
                const uint8_t green = static_cast<uint8_t>((color >> 8) & 0xff);
                const uint8_t blue = static_cast<uint8_t>(color & 0xff);
                uint8_t* destination = destinationRow + static_cast<std::size_t>(x) * 4;
                destination[0] = Premultiply(blue, alpha);
                destination[1] = Premultiply(green, alpha);
                destination[2] = Premultiply(red, alpha);
                destination[3] = alpha;
            }
        }

        NativeSubtitleBitmap bitmap;
        bitmap.x = rect->x;
        bitmap.y = rect->y;
        bitmap.width = rect->w;
        bitmap.height = rect->h;
        bitmap.canvasWidth = std::max(canvasWidth, rect->x + rect->w);
        bitmap.canvasHeight = std::max(canvasHeight, rect->y + rect->h);
        bitmap.stride = stride;
        bitmap.serial = ++serial;
        bitmap.bgra = std::move(pixels);
        bitmaps.push_back(std::move(bitmap));
    }
    return bitmaps;
}

std::chrono::milliseconds SubtitlePacketBasePts(const AVSubtitle& subtitle,
                                                const AVPacket* packet,
                                                const AVRational streamTimeBase) {
    if (subtitle.pts != AV_NOPTS_VALUE) {
        return std::chrono::milliseconds{
            av_rescale_q(subtitle.pts, AVRational{1, AV_TIME_BASE}, AVRational{1, 1000})};
    }
    if (packet && packet->pts != AV_NOPTS_VALUE) {
        return std::chrono::milliseconds{
            av_rescale_q(packet->pts, streamTimeBase, AVRational{1, 1000})};
    }
    if (packet && packet->dts != AV_NOPTS_VALUE) {
        return std::chrono::milliseconds{
            av_rescale_q(packet->dts, streamTimeBase, AVRational{1, 1000})};
    }
    return std::chrono::milliseconds{0};
}

std::optional<std::chrono::milliseconds> PacketDuration(const AVPacket* packet, const AVRational streamTimeBase) {
    if (!packet || packet->duration <= 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{
        av_rescale_q(packet->duration, streamTimeBase, AVRational{1, 1000})};
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
                               ClockCallback clockCallback,
                               const bool preferHardwareDecode,
                               ID3D11Device* sharedD3DDevice,
                               std::wstring preferredSubtitleLanguage,
                               const int selectedSubtitleTrackIndex,
                               const std::chrono::milliseconds subtitleDelay,
                               const bool autoLoadExternalSubtitles,
                               const bool oneShotFrame) {
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
    bitmapSubtitleLogged_ = false;
    streamColorMetadata_ = {};
    preferredSubtitleLanguage_ = std::move(preferredSubtitleLanguage);
    selectedSubtitleTrackIndex_ = selectedSubtitleTrackIndex;
    subtitleDelay_ = subtitleDelay;
    autoLoadExternalSubtitles_ = autoLoadExternalSubtitles;
    oneShotFrame_ = oneShotFrame;
    subtitleCanvasWidth_ = 0;
    subtitleCanvasHeight_ = 0;
    subtitleCanvasLogged_ = false;
    subtitleBitmapSerial_ = 0;
    subtitleCues_.clear();
    clockCallback_ = std::move(clockCallback);
    fallbackClockAnchor_.reset();
    fallbackClockBasePts_ = std::chrono::milliseconds{0};
    notificationWindow_.store(notificationWindow);
    notificationMessage_.store(notificationMessage);
    frameMessagePending_.store(false);
    stopping_.store(false);
    schedulePrimed_ = false;
    {
        std::scoped_lock lock(mutex_);
        latestFrame_.bgra.reset();
        latestFrame_.serial = 0;
        latestFrame_.width = latestFrame_.height = latestFrame_.stride = 0;
        latestFrame_.d3dTexture.Reset();
        latestFrame_.hardwareFrameRef.reset();
        latestFrame_.d3dArraySlice = 0;
        latestFrame_.d3dFormat = DXGI_FORMAT_UNKNOWN;
        latestFrame_.softwareFormat = AV_PIX_FMT_NONE;
        latestFrame_.color = {};
        latestFrame_.subtitleText.clear();
        latestFrame_.subtitleBitmaps.clear();
        frameQueue_.clear();
        stats_ = {};
        stats_.decoder = preferHardwareDecode_ ? L"ffmpeg_d3d11va_pending" : L"ffmpeg_software";
    }
    running_.store(true);
    decodeThread_ = std::thread([this]() { DecodeLoop(); });
    return true;
}

void FfmpegVideoDecoder::Stop() {
    stopping_.store(true);
    notificationWindow_.store(nullptr);
    notificationMessage_.store(0);
    frameMessagePending_.store(false);
    if (decodeThread_.joinable()) {
        decodeThread_.join();
    }
    running_.store(false);
    clockCallback_ = {};
    sharedD3DDevice_.Reset();
}

int FfmpegVideoDecoder::InterruptCallback(void* opaque) {
    const auto* decoder = static_cast<const FfmpegVideoDecoder*>(opaque);
    return decoder && decoder->stopping_.load() ? 1 : 0;
}

bool FfmpegVideoDecoder::LatestFrame(NativeVideoFrame& frame) const {
    std::scoped_lock lock(mutex_);
    if (!latestFrame_.HasContent()) {
        return false;
    }
    frame = latestFrame_;
    return true;
}

void FfmpegVideoDecoder::ClearFrame() {
    std::scoped_lock lock(mutex_);
    latestFrame_.bgra.reset();
    latestFrame_.serial = 0;
    latestFrame_.width = latestFrame_.height = latestFrame_.stride = 0;
    latestFrame_.d3dTexture.Reset();
    latestFrame_.hardwareFrameRef.reset();
    latestFrame_.d3dArraySlice = 0;
    latestFrame_.d3dFormat = DXGI_FORMAT_UNKNOWN;
    latestFrame_.softwareFormat = AV_PIX_FMT_NONE;
    latestFrame_.color = {};
    latestFrame_.subtitleText.clear();
    latestFrame_.subtitleBitmaps.clear();
    frameQueue_.clear();
    stats_.queueDepth = 0;
}

void FfmpegVideoDecoder::AcknowledgeFrameNotification() {
    frameMessagePending_.store(false);
}

NativeVideoQueueStats FfmpegVideoDecoder::Stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}

void FfmpegVideoDecoder::DecodeLoop() {
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVCodecContext* subtitleCodecCtx = nullptr;
    AVBufferRef* hwDeviceCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* softwareFrame = nullptr;
    AVPacket* packet = nullptr;
    std::vector<uint8_t> bgraBuffer;
    int videoStreamIndex = -1;
    int subtitleStreamIndex = -1;
    AVRational streamTimeBase{1, 1};
    AVRational subtitleTimeBase{1, 1};
    uint64_t serial = 0;
    bool firstPacketSeen = false;

    const std::string pathUtf8 = WideToUtf8(path_.wstring());

    do {
        formatCtx = avformat_alloc_context();
        if (!formatCtx) {
            LogThreadError(L"avformat_alloc_context failed");
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
        const int openResult = avformat_open_input(&formatCtx, pathUtf8.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (openResult < 0) {
            LogThreadError(L"avformat_open_input failed");
            break;
        }
        if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
            LogThreadError(L"avformat_find_stream_info failed");
            break;
        }
        videoStreamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStreamIndex < 0) {
            LogThreadError(L"no video stream found");
            break;
        }
        streamTimeBase = formatCtx->streams[videoStreamIndex]->time_base;
        AVCodecParameters* codecpar = formatCtx->streams[videoStreamIndex]->codecpar;
        subtitleCanvasWidth_ = codecpar ? codecpar->width : 0;
        subtitleCanvasHeight_ = codecpar ? codecpar->height : 0;
        bool externalSubtitlesLoaded = false;
        if (selectedSubtitleTrackIndex_ == anvil::playback::kSubtitleTrackOff) {
            LogThread(LogLevel::Info, L"subtitle", L"selected=off");
        } else if (selectedSubtitleTrackIndex_ == anvil::playback::kSubtitleTrackAuto && autoLoadExternalSubtitles_) {
            const auto externalSubtitle = FindExternalSubtitleFile(path_, preferredSubtitleLanguage_);
            if (externalSubtitle.has_value()) {
                externalSubtitlesLoaded = DecodeExternalSubtitleFile(*externalSubtitle);
            } else {
                LogThread(LogLevel::Debug, L"subtitle", L"external=none");
            }
        }
        if (selectedSubtitleTrackIndex_ != anvil::playback::kSubtitleTrackOff && !externalSubtitlesLoaded) {
            OpenSubtitleDecoder(formatCtx, subtitleStreamIndex, subtitleTimeBase, subtitleCodecCtx);
        }

        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            LogThreadError(L"avcodec_find_decoder failed");
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

        if (!OpenVideoDecoder(codec, codecpar, codecCtx, hwDeviceCtx)) {
            break;
        }

        frame = av_frame_alloc();
        softwareFrame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!frame || !softwareFrame || !packet) {
            break;
        }

        if (startPosition_.count() > 0) {
            const int64_t seekTarget = static_cast<int64_t>(startPosition_.count()) *
                                       AV_TIME_BASE / 1000;
            av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
            if (subtitleCodecCtx) {
                avcodec_flush_buffers(subtitleCodecCtx);
            }
        }

        while (!stopping_.load()) {
            const int readResult = av_read_frame(formatCtx, packet);
            if (readResult < 0) {
                // EOF or error: drain decoder then stop.
                avcodec_send_packet(codecCtx, nullptr);
                if (DrainDecoder(codecCtx, swsCtx, frame, softwareFrame, bgraBuffer, streamTimeBase, serial)) {
                    // swsCtx may have been allocated inside DrainDecoder.
                }
                break;
            }

            if (packet->stream_index == subtitleStreamIndex && subtitleCodecCtx) {
                DecodeSubtitlePacket(subtitleCodecCtx, packet, subtitleTimeBase);
                av_packet_unref(packet);
                continue;
            }

            if (packet->stream_index != videoStreamIndex) {
                av_packet_unref(packet);
                continue;
            }

            firstPacketSeen = true;
            const int sendResult = avcodec_send_packet(codecCtx, packet);
            av_packet_unref(packet);
            if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
                continue;
            }

            if (!ReceiveFrames(codecCtx, swsCtx, frame, softwareFrame, bgraBuffer, streamTimeBase, serial)) {
                break;
            }
        }
        (void)firstPacketSeen;
    } while (false);

    if (swsCtx) sws_freeContext(swsCtx);
    if (frame) av_frame_free(&frame);
    if (softwareFrame) av_frame_free(&softwareFrame);
    if (packet) av_packet_free(&packet);
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    if (subtitleCodecCtx) avcodec_free_context(&subtitleCodecCtx);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (formatCtx) avformat_close_input(&formatCtx);
    DrainQueuedFrames();
    running_.store(false);
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

    if (preferHardwareDecode_ && !dolbyVisionStream_) {
        hardwareConfigured = ConfigureD3D11VA(codec, codecCtx, hwDeviceCtx);
    } else {
        SetDecodeBackend(L"ffmpeg_software", false, {});
        LogThread(LogLevel::Info, L"decoder",
                  L"selected=ffmpeg_software reason=" +
                      std::wstring(dolbyVisionStream_ ? L"dolby_vision_software_decode_for_reshape"
                                                       : L"hardware_decode_not_requested"));
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
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) {
            return false;
        }
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (!context) {
            return false;
        }
        const int paramsError = avcodec_parameters_to_context(context, parameters);
        if (paramsError < 0) {
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
                      L" language=" + (StreamLanguage(stream).empty() ? L"-" : StreamLanguage(stream)));
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

bool FfmpegVideoDecoder::DecodeExternalSubtitleFile(const std::filesystem::path& subtitlePath) {
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
                                       std::vector<uint8_t>& bgraBuffer, AVRational timeBase,
                                       uint64_t& serial) {
    while (!stopping_.load()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            return false;
        }
        if (!PublishFrame(frame, softwareFrame, swsCtx, bgraBuffer, timeBase, serial)) {
            av_frame_unref(frame);
            return false;
        }
        av_frame_unref(frame);
    }
    return true;
}

// Drain decoder after EOF (no more packets).
bool FfmpegVideoDecoder::DrainDecoder(AVCodecContext* codecCtx, SwsContext*& swsCtx, AVFrame* frame, AVFrame* softwareFrame,
                                      std::vector<uint8_t>& bgraBuffer, AVRational timeBase,
                                      uint64_t& serial) {
    while (!stopping_.load()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            return false;
        }
        if (!PublishFrame(frame, softwareFrame, swsCtx, bgraBuffer, timeBase, serial)) {
            av_frame_unref(frame);
            return false;
        }
        av_frame_unref(frame);
    }
    return true;
}

bool FfmpegVideoDecoder::PublishFrame(AVFrame* frame, AVFrame* softwareFrame, SwsContext*& swsCtx, std::vector<uint8_t>& bgraBuffer,
                                      AVRational timeBase, uint64_t& serial) {
    const auto pts = FramePts(frame, timeBase);
    if (ShouldDropSeekPreroll(pts)) {
        return true;
    }

    AVFrame* conversionFrame = frame;
    if (frame && frame->format == hardwarePixelFormat_ && hardwarePixelFormat_ != AV_PIX_FMT_NONE) {
        NativeVideoFrame textureFrame;
        if (TryBuildD3DTextureFrame(frame, pts, serial, textureFrame)) {
            if (oneShotFrame_) {
                return PublishImmediateFrame(std::move(textureFrame));
            }
            return EnqueueFrame(std::move(textureFrame));
        }

        if (!softwareFrame) {
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

    // Dolby Vision software path: carry the raw 10-bit YUV planes to the GPU
    // without swscale's YUV->RGB conversion, which would destroy the IPT
    // structure that the renderer must reshape. We pack Y + interleaved UV
    // (NV12/P010 layout) into a single buffer the renderer uploads as a
    // P010 texture.
    if (dolbyVisionStream_ && frame->format == AV_PIX_FMT_YUV420P10LE) {
        NativeVideoFrame queued;
        queued.width = srcW;
        queued.height = srcH;
        queued.color = MergeFrameColorMetadata(frame, streamColorMetadata_);
        queued.dovi = ExtractFrameDolbyVisionMetadata(frame);
        if (!dolbyVisionFirstFrameLogged_) {
            dolbyVisionFirstFrameLogged_ = true;
            LogThread(LogLevel::Info, L"decoder",
                      L"dolby_vision_yuv_path w=" + std::to_wstring(srcW) + L" h=" + std::to_wstring(srcH) +
                          L" dovi=" + (queued.dovi ? L"yes" : L"no"));
        }

        const int yStride = srcW * 2;                    // 10-bit Y: 2 bytes/sample
        const int uvStride = (srcW / 2) * 4;             // P010 UV: 2 bytes/U + 2 bytes/V per sample
        const int uvHeight = srcH / 2;
        const auto packStart = std::chrono::steady_clock::now();
        // Use the actual luma linesize from FFmpeg for the source copy, but
        // pack tightly into yStride for upload.
        auto buffer = std::make_shared<std::vector<uint8_t>>();
        const std::size_t totalBytes = static_cast<std::size_t>(yStride) * srcH +
                                       static_cast<std::size_t>(uvStride) * uvHeight;
        buffer->resize(totalBytes);

        // Copy Y plane (tighten to yStride) while converting yuv420p10le's
        // low-bit 10-bit samples into DXGI P010's high-bit 10-bit layout.
        const uint8_t* srcY = frame->data[0];
        const int srcYStride = frame->linesize[0];
        for (int row = 0; row < srcH; ++row) {
            const uint8_t* srcRow = srcY + static_cast<std::size_t>(row) * srcYStride;
            uint8_t* dstRow = buffer->data() + static_cast<std::size_t>(row) * yStride;
            ConvertYuv420P10RowToP010(dstRow, srcRow, srcW);
        }
        // Interleave U and V planes into a single UV plane (P010 layout).
        const uint8_t* srcU = frame->data[1];
        const uint8_t* srcV = frame->data[2];
        const int srcUStride = frame->linesize[1];
        const int srcVStride = frame->linesize[2];
        uint8_t* uvDst = buffer->data() + static_cast<std::size_t>(yStride) * srcH;
        for (int row = 0; row < uvHeight; ++row) {
            const uint8_t* uRow = srcU + static_cast<std::size_t>(row) * srcUStride;
            const uint8_t* vRow = srcV + static_cast<std::size_t>(row) * srcVStride;
            uint8_t* dstRow = uvDst + static_cast<std::size_t>(row) * uvStride;
            InterleaveYuv420P10RowToP010Uv(dstRow, uRow, vRow, srcW / 2);
        }
        if (!dolbyVisionFirstPackedLogged_) {
            dolbyVisionFirstPackedLogged_ = true;
            const auto packMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - packStart).count();
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"dolby_vision_yuv_packed bytes=" + std::to_wstring(totalBytes) +
                          L" pack_ms=" + std::to_wstring(packMs));
        }

        queued.yuv.width = srcW;
        queued.yuv.height = srcH;
        queued.yuv.yStride = yStride;
        queued.yuv.uvStride = uvStride;
        queued.yuv.bitDepth = 10;
        queued.yuv.data = std::move(buffer);
        queued.subtitleText = SubtitleTextForPts(pts);
        queued.subtitleBitmaps = SubtitleBitmapsForPts(pts, srcW, srcH);
        queued.pts = pts;
        queued.serial = ++serial;
        const bool logFirstDoviQueue = !dolbyVisionFirstQueueLogged_;
        if (logFirstDoviQueue) {
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"dolby_vision_yuv_queue_ready serial=" + std::to_wstring(queued.serial) +
                          L" pts_ms=" + std::to_wstring(queued.pts.count()) +
                          L" subtitles_text=" + (queued.subtitleText.empty() ? L"no" : L"yes") +
                          L" subtitles_bitmap=" + (queued.subtitleBitmaps.empty() ? L"no" : L"yes"));
        }
        if (oneShotFrame_) {
            return PublishImmediateFrame(std::move(queued));
        }
        const bool enqueued = EnqueueFrame(std::move(queued));
        if (logFirstDoviQueue) {
            dolbyVisionFirstQueueLogged_ = true;
            LogThread(LogLevel::Debug,
                      L"decoder",
                      L"dolby_vision_yuv_queue_result enqueued=" + std::wstring(enqueued ? L"true" : L"false"));
        }
        return enqueued;
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
    if (dolbyVisionStream_) {
        queued.dovi = ExtractFrameDolbyVisionMetadata(frame);
    }
    queued.subtitleText = SubtitleTextForPts(pts);
    queued.subtitleBitmaps = SubtitleBitmapsForPts(pts, srcW, srcH);
    queued.pts = pts;
    queued.serial = ++serial;
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

    out.width = frame->width;
    out.height = frame->height;
    out.stride = 0;
    out.d3dTexture = texture;
    out.d3dArraySlice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    out.d3dFormat = desc.Format;
    out.softwareFormat = HardwareFrameSoftwareFormat(frame);
    out.color = MergeFrameColorMetadata(frame, streamColorMetadata_);
    if (dolbyVisionStream_) {
        out.dovi = ExtractFrameDolbyVisionMetadata(frame);
    }
    out.subtitleText = SubtitleTextForPts(pts);
    out.subtitleBitmaps = SubtitleBitmapsForPts(pts, out.width, out.height);
    out.hardwareFrameRef = std::shared_ptr<AVFrame>(retained, [](AVFrame* value) {
        if (value) {
            av_frame_free(&value);
        }
    });
    out.pts = pts;
    out.serial = ++serial;

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
    {
        std::scoped_lock lock(mutex_);
        stats_.queueDepth = 0;
        stats_.clockPosition = frame.pts;
        stats_.usingAudioClock = false;
        stats_.driftMs = 0;
        ++stats_.rendered;
        latestFrame_ = frame;
    }
    NotifyFrameReady();
    stopping_.store(true);
    return true;
}

bool FfmpegVideoDecoder::DecodeSubtitlePacket(AVCodecContext* subtitleCodecCtx,
                                              const AVPacket* packet,
                                              const AVRational subtitleTimeBase) {
    if (!subtitleCodecCtx || !packet) {
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
    if (end <= start) {
        if (const auto duration = PacketDuration(packet, subtitleTimeBase);
            duration.has_value() && *duration > std::chrono::milliseconds{0}) {
            end = start + *duration;
        } else {
            end = start + std::chrono::seconds{4};
        }
    }

    const std::wstring text = SubtitleTextFromDecoded(subtitle);
    auto bitmaps = SubtitleBitmapsFromDecoded(subtitle, canvasWidth, canvasHeight, subtitleBitmapSerial_);
    if (!bitmaps.empty() && !subtitleCanvasLogged_ && ValidSubtitleCanvas(canvasWidth, canvasHeight)) {
        subtitleCanvasLogged_ = true;
        LogThread(LogLevel::Debug,
                  L"subtitle",
                  L"canvas=" + std::to_wstring(canvasWidth) + L"x" + std::to_wstring(canvasHeight) +
                      L" source=" + canvasSource);
    }
    if (text.empty() && bitmaps.empty()) {
        if (isPgsSubtitle) {
            TrimActiveBitmapSubtitleCues(start);
        }
        avsubtitle_free(&subtitle);
        return true;
    }
    if (!bitmaps.empty() && !bitmapSubtitleLogged_) {
        bitmapSubtitleLogged_ = true;
        LogThread(LogLevel::Info, L"subtitle", L"bitmap_overlay active=true rects=" + std::to_wstring(bitmaps.size()));
    }

    if (!bitmaps.empty() && isPgsSubtitle) {
        TrimActiveBitmapSubtitleCues(start);
    }

    if (end > startPosition_) {
        subtitleCues_.push_back(NativeSubtitleCue{start, end, text, std::move(bitmaps)});
    }
    avsubtitle_free(&subtitle);
    return true;
}

void FfmpegVideoDecoder::TrimActiveBitmapSubtitleCues(const std::chrono::milliseconds time) {
    for (auto& cue : subtitleCues_) {
        if (!cue.bitmaps.empty() && cue.start < time && time < cue.end) {
            cue.end = time;
        }
    }
    while (!subtitleCues_.empty() && subtitleCues_.front().end <= time) {
        subtitleCues_.pop_front();
    }
}

std::wstring FfmpegVideoDecoder::SubtitleTextForPts(const std::chrono::milliseconds pts) {
    const auto effectivePts = pts - subtitleDelay_;
    while (!subtitleCues_.empty() && subtitleCues_.front().end <= effectivePts) {
        subtitleCues_.pop_front();
    }

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
                                                                            const int frameHeight) {
    const auto effectivePts = pts - subtitleDelay_;
    while (!subtitleCues_.empty() && subtitleCues_.front().end <= effectivePts) {
        subtitleCues_.pop_front();
    }

    std::vector<NativeSubtitleBitmap> bitmaps;
    for (const auto& cue : subtitleCues_) {
        if (cue.start <= effectivePts && effectivePts < cue.end) {
            bitmaps.insert(bitmaps.end(), cue.bitmaps.begin(), cue.bitmaps.end());
        }
    }
    return bitmaps;
}

bool FfmpegVideoDecoder::ShouldDropSeekPreroll(const std::chrono::milliseconds pts) const {
    if (startPosition_.count() <= 0 || pts.count() <= 0) {
        return false;
    }
    return pts + std::chrono::milliseconds{80} < startPosition_;
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

VideoColorMetadata FfmpegVideoDecoder::MergeFrameColorMetadata(const AVFrame* frame, const VideoColorMetadata& defaults) {
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

    // Color matrices (YCC<->RGB and RGB->LMS, both row-major 3x3).
    for (int i = 0; i < 9; ++i) {
        out->yccToRgb[i] = static_cast<float>(RationalToDouble(color->ycc_to_rgb_matrix[i]));
        out->rgbToLms[i] = static_cast<float>(RationalToDouble(color->rgb_to_lms_matrix[i]));
    }
    for (int i = 0; i < 3; ++i) {
        out->yccOffset[i] = static_cast<float>(RationalToDouble(color->ycc_to_rgb_offset[i]));
    }

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
    if (needed == 0) {
        return {};
    }

    std::scoped_lock lock(mutex_);
    for (const auto& buffer : reusableBgraBuffers_) {
        if (buffer && buffer.use_count() == 1 && buffer->capacity() >= needed) {
            buffer->resize(needed);
            return buffer;
        }
    }

    for (const auto& buffer : reusableBgraBuffers_) {
        if (buffer && buffer.use_count() == 1) {
            buffer->resize(needed);
            return buffer;
        }
    }

    if (reusableBgraBuffers_.size() < kMaxReusableBgraBuffers) {
        auto buffer = std::make_shared<std::vector<uint8_t>>();
        buffer->resize(needed);
        reusableBgraBuffers_.push_back(buffer);
        return buffer;
    }

    auto transient = std::make_shared<std::vector<uint8_t>>();
    transient->resize(needed);
    return transient;
}

bool FfmpegVideoDecoder::EnqueueFrame(NativeVideoFrame&& frame) {
    while (!stopping_.load()) {
        ScheduleDueFrames();
        bool queued = false;
        {
            std::scoped_lock lock(mutex_);
            if (frameQueue_.size() < kMaxQueuedFrames) {
                frameQueue_.push_back(std::move(frame));
                stats_.queueDepth = frameQueue_.size();
                queued = true;
            }
        }
        if (queued) {
            ScheduleDueFrames();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return false;
}

void FfmpegVideoDecoder::DrainQueuedFrames() {
    while (!stopping_.load()) {
        ScheduleDueFrames();
        {
            std::scoped_lock lock(mutex_);
            if (frameQueue_.empty()) {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

void FfmpegVideoDecoder::ScheduleDueFrames() {
    NativeVideoFrame frameToPublish;
    bool hasFrame = false;
    bool firstPublishedFrame = false;
    std::size_t publishedQueueDepth = 0;
    std::chrono::milliseconds publishedClockPosition{0};

    {
        std::scoped_lock lock(mutex_);
        if (frameQueue_.empty()) {
            stats_.queueDepth = 0;
            return;
        }

        auto clock = CurrentSchedulerClockLocked(frameQueue_.front().pts);
        if (!schedulePrimed_ && stats_.rendered == 0) {
            clock.position = frameQueue_.front().pts;
            clock.usingAudioClock = false;
            schedulePrimed_ = true;
        }
        stats_.clockPosition = clock.position;
        stats_.usingAudioClock = clock.usingAudioClock;

        while (!frameQueue_.empty()) {
            const NativeVideoFrame& front = frameQueue_.front();
            const auto earlyBy = front.pts - clock.position;
            const bool frameIsDue = earlyBy <= kFrameEarlyTolerance;
            if (!frameIsDue) {
                break;
            }

            NativeVideoFrame candidate = std::move(frameQueue_.front());
            frameQueue_.pop_front();
            stats_.queueDepth = frameQueue_.size();

            const bool anotherFrameDue = !frameQueue_.empty() &&
                (frameQueue_.front().pts - clock.position) <= kFrameEarlyTolerance;
            if (anotherFrameDue) {
                ++stats_.droppedLate;
                continue;
            }

            frameToPublish = std::move(candidate);
            hasFrame = true;
            break;
        }

        if (hasFrame) {
            stats_.driftMs = static_cast<int>((frameToPublish.pts - clock.position).count());
            stats_.queueDepth = frameQueue_.size();
            ++stats_.rendered;
            firstPublishedFrame = stats_.rendered == 1;
            publishedQueueDepth = stats_.queueDepth;
            publishedClockPosition = clock.position;
            latestFrame_ = frameToPublish;
        }
    }

    if (hasFrame) {
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

FfmpegVideoDecoder::SchedulerClock FfmpegVideoDecoder::CurrentSchedulerClockLocked(const std::chrono::milliseconds firstQueuedPts) {
    if (clockCallback_) {
        if (const auto audioClock = clockCallback_()) {
            return {*audioClock, true};
        }
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
    PostMessageW(window, message, 0, 0);
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
