#include "AnvilPlayer/App/subtitle_util.h"

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"  // NativeSubtitleBitmap
#include "AnvilPlayer/App/string_util.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <memory>
#include <tuple>

namespace anvil::app {

namespace {

std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
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

uint8_t Premultiply(const uint8_t value, const uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<unsigned int>(value) * alpha + 127) / 255);
}

uint16_t ReadBigEndian16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<unsigned int>(data[0]) << 8) | data[1]);
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

}  // namespace

bool IsSupportedExternalSubtitleExtension(const std::filesystem::path& path) {
    const auto extension = ToLowerWide(path.extension().wstring());
    return extension == L".srt" ||
           extension == L".ass" ||
           extension == L".ssa" ||
           extension == L".vtt";
}

bool IsAssSubtitleExtension(const std::filesystem::path& path) {
    const auto extension = ToLowerWide(path.extension().wstring());
    return extension == L".ass" || extension == L".ssa";
}

bool IsAssSubtitleCodec(const unsigned int codecId) {
    return codecId == AV_CODEC_ID_ASS || codecId == AV_CODEC_ID_SSA;
}

bool IsAutoLanguage(const std::wstring& value) {
    const auto normalized = ToLowerWide(value);
    return normalized.empty() || normalized == L"auto" || normalized == L"default";
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

bool ValidSubtitleCanvas(const int width, const int height) {
    constexpr int kMaxSubtitleCanvasDimension = 16384;
    return width > 0 && height > 0 &&
           width <= kMaxSubtitleCanvasDimension &&
           height <= kMaxSubtitleCanvasDimension;
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
                                                             std::uint64_t& serial) {
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

}  // namespace anvil::app
