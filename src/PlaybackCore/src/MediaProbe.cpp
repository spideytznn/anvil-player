#include "AnvilPlayer/Playback/MediaProbe.h"

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cwctype>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace anvil::playback {
namespace {

constexpr std::size_t kMaxReportedStreams = 256;

std::wstring Uppercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    return value;
}

std::wstring UppercaseExtension(std::filesystem::path path) {
    auto extension = path.extension().wstring();
    if (!extension.empty() && extension.front() == L'.') {
        extension.erase(extension.begin());
    }
    return extension.empty() ? L"Unknown" : Uppercase(extension);
}

std::wstring FirstFormatName(std::wstring formatNames) {
    const auto comma = formatNames.find(L',');
    if (comma != std::wstring::npos) {
        formatNames.resize(comma);
    }
    return Uppercase(formatNames.empty() ? L"Unknown" : formatNames);
}

std::wstring Utf8ToWide(const char* text) {
    if (!text || *text == '\0') {
        return {};
    }

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, text, -1, nullptr, 0);
    }
    if (length <= 1) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    const int written = MultiByteToWideChar(codePage, flags, text, -1, wide.data(), length);
    if (written <= 1) {
        return {};
    }
    wide.resize(static_cast<std::size_t>(written - 1));
    return wide;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }

    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), needed, nullptr, nullptr);
    if (written <= 1) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

std::wstring FfmpegErrorString(const int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, buffer, sizeof(buffer)) < 0) {
        std::wostringstream stream;
        stream << L"ffmpeg error " << error;
        return stream.str();
    }
    return Utf8ToWide(buffer);
}

std::wstring NormalizeCodecName(const std::wstring& codec) {
    if (codec == L"h264") {
        return L"H.264";
    }
    if (codec == L"hevc") {
        return L"HEVC";
    }
    if (codec == L"av1") {
        return L"AV1";
    }
    if (codec == L"vp9") {
        return L"VP9";
    }
    if (codec == L"mpeg4") {
        return L"MPEG-4 Part 2";
    }
    if (codec == L"aac") {
        return L"AAC";
    }
    if (codec == L"ac3") {
        return L"AC-3";
    }
    if (codec == L"eac3") {
        return L"E-AC-3";
    }
    if (codec == L"truehd") {
        return L"TrueHD";
    }
    if (codec == L"dts") {
        return L"DTS";
    }
    if (codec == L"opus") {
        return L"Opus";
    }
    if (codec == L"vorbis") {
        return L"Vorbis";
    }
    if (codec == L"ass") {
        return L"ASS";
    }
    if (codec == L"subrip") {
        return L"SRT";
    }
    return codec.empty() ? L"Unknown" : codec;
}

std::wstring GuessVideoCodec(const std::wstring& container) {
    if (container == L"WEBM") {
        return L"VP9 / AV1 probe pending";
    }
    if (container == L"AVI") {
        return L"Legacy codec probe pending";
    }
    return L"H.264 / HEVC probe pending";
}

std::wstring GuessAudioCodec(const std::wstring& container) {
    if (container == L"WEBM") {
        return L"Opus / Vorbis probe pending";
    }
    return L"AAC / Dolby / DTS probe pending";
}

std::wstring CodecName(const AVCodecParameters* parameters) {
    if (!parameters) {
        return L"Unknown";
    }
    return NormalizeCodecName(Utf8ToWide(avcodec_get_name(parameters->codec_id)));
}

std::wstring StreamLanguage(const AVStream* stream) {
    if (!stream) {
        return {};
    }
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
    return language ? Utf8ToWide(language->value) : L"";
}

std::wstring ProfileName(const AVCodecParameters* parameters) {
    if (!parameters || parameters->profile == AV_PROFILE_UNKNOWN) {
        return {};
    }
    return Utf8ToWide(avcodec_profile_name(parameters->codec_id, parameters->profile));
}

double RationalToDouble(const AVRational value) {
    if (value.num <= 0 || value.den <= 0) {
        return 0.0;
    }
    const double result = av_q2d(value);
    return std::isfinite(result) ? result : 0.0;
}

double ProbeFrameRate(const AVStream* stream, const AVCodecParameters* parameters) {
    if (stream) {
        const double average = RationalToDouble(stream->avg_frame_rate);
        if (average > 0.0) {
            return average;
        }
        const double real = RationalToDouble(stream->r_frame_rate);
        if (real > 0.0) {
            return real;
        }
    }
    return parameters ? RationalToDouble(parameters->framerate) : 0.0;
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

std::chrono::milliseconds DurationFromContext(const AVFormatContext* formatContext) {
    if (!formatContext) {
        return std::chrono::milliseconds{0};
    }
    if (formatContext->duration != AV_NOPTS_VALUE && formatContext->duration > 0) {
        return std::chrono::milliseconds{
            av_rescale_q(formatContext->duration, AVRational{1, AV_TIME_BASE}, AVRational{1, 1000})};
    }

    int64_t maxDurationMs = 0;
    for (unsigned int index = 0; index < formatContext->nb_streams; ++index) {
        const AVStream* stream = formatContext->streams[index];
        if (!stream || stream->duration == AV_NOPTS_VALUE || stream->duration <= 0) {
            continue;
        }
        const int64_t durationMs = av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000});
        maxDurationMs = std::max(maxDurationMs, durationMs);
    }
    return std::chrono::milliseconds{maxDurationMs};
}

std::wstring ContainerName(const AVFormatContext* formatContext, const std::filesystem::path& path) {
    const auto extension = UppercaseExtension(path);
    if (extension != L"Unknown") {
        return extension;
    }
    if (formatContext && formatContext->iformat && formatContext->iformat->name) {
        return FirstFormatName(Utf8ToWide(formatContext->iformat->name));
    }
    return L"Unknown";
}

std::wstring PixelFormatName(const AVCodecParameters* parameters) {
    if (!parameters || parameters->format == AV_PIX_FMT_NONE) {
        return {};
    }
    return Utf8ToWide(av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format)));
}

const AVDOVIDecoderConfigurationRecord* DolbyVisionConfiguration(const AVCodecParameters* parameters) {
    if (!parameters) {
        return nullptr;
    }
    for (int index = 0; index < parameters->nb_coded_side_data; ++index) {
        const AVPacketSideData& sideData = parameters->coded_side_data[index];
        if (sideData.type != AV_PKT_DATA_DOVI_CONF ||
            sideData.size < static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
            continue;
        }
        return reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(sideData.data);
    }
    return nullptr;
}

std::wstring DolbyVisionProfile(const AVCodecParameters* parameters) {
    const auto* dovi = DolbyVisionConfiguration(parameters);
    if (!dovi) {
        return {};
    }
    return L"Dolby Vision Profile " + std::to_wstring(static_cast<int>(dovi->dv_profile));
}

std::wstring DolbyVisionStreamDetails(const AVCodecParameters* parameters) {
    const auto* dovi = DolbyVisionConfiguration(parameters);
    if (!dovi) {
        return {};
    }

    std::wstring details = L"Dolby Vision P" + std::to_wstring(static_cast<int>(dovi->dv_profile));
    if (dovi->el_present_flag && !dovi->bl_present_flag) {
        details += L" EL-only";
    } else if (dovi->el_present_flag && dovi->bl_present_flag) {
        details += L" BL+EL";
    } else if (dovi->bl_present_flag) {
        details += L" BL";
    }
    if (dovi->rpu_present_flag) {
        details += L" RPU";
    }
    details += L" compat " + std::to_wstring(static_cast<int>(dovi->dv_bl_signal_compatibility_id));
    return details;
}

std::wstring DetectHdrFormat(const AVCodecParameters* parameters, const VideoColorMetadata& color) {
    if (!parameters) {
        return L"SDR / unknown";
    }

    const auto dovi = DolbyVisionProfile(parameters);
    if (!dovi.empty()) {
        return dovi;
    }
    for (int index = 0; parameters->coded_side_data && index < parameters->nb_coded_side_data; ++index) {
        if (parameters->coded_side_data[index].type == AV_PKT_DATA_DYNAMIC_HDR10_PLUS) {
            return L"HDR10+ / PQ";
        }
    }
    if (color.transfer == VideoTransferCharacteristic::Pq) {
        if (color.primaries == VideoColorPrimaries::Bt2020) {
            return color.contentLight.hasValues || color.masteringDisplay.hasLuminance ? L"HDR10 / PQ" : L"PQ HDR";
        }
        return L"PQ HDR";
    }
    if (color.transfer == VideoTransferCharacteristic::Hlg) {
        return L"HLG";
    }
    return L"SDR / unknown";
}

void AppendPart(std::vector<std::wstring>& parts, std::wstring value) {
    if (!value.empty() && value != L"unknown") {
        parts.push_back(std::move(value));
    }
}

std::wstring JoinParts(const std::vector<std::wstring>& parts, const std::wstring& fallback) {
    std::wstring details;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            details += L" ";
        }
        details += parts[index];
    }
    return details.empty() ? fallback : details;
}

struct ProbeInterruptState {
    std::stop_token stopToken;
    std::chrono::steady_clock::time_point deadline{};

    bool StopRequested() const {
        return stopToken.stop_requested();
    }

    bool DeadlineExpired() const {
        return deadline.time_since_epoch().count() != 0 &&
               std::chrono::steady_clock::now() >= deadline;
    }

    bool Interrupted() const {
        return StopRequested() || DeadlineExpired();
    }
};

int ProbeInterruptCallback(void* opaque) {
    const auto* state = static_cast<const ProbeInterruptState*>(opaque);
    return state && state->Interrupted() ? 1 : 0;
}

MediaProbeResult InterruptedProbeResult(const std::filesystem::path& path,
                                        const ProbeInterruptState& interrupt) {
    MediaProbeResult result;
    result.probeTool = L"avformat";
    result.descriptor.path = path;
    result.descriptor.displayName = path.filename().wstring();
    result.cancelled = interrupt.StopRequested();
    result.timedOut = !result.cancelled && interrupt.DeadlineExpired();
    result.diagnostic = result.cancelled ? L"media probe cancelled" : L"media probe deadline exceeded";
    return result;
}

std::wstring BuildVideoDetails(const AVStream* stream, const AVCodecParameters* parameters) {
    std::vector<std::wstring> parts;
    if (parameters && parameters->width > 0 && parameters->height > 0) {
        parts.push_back(std::to_wstring(parameters->width) + L"x" + std::to_wstring(parameters->height));
    }
    AppendPart(parts, PixelFormatName(parameters));
    AppendPart(parts, ProfileName(parameters));
    AppendPart(parts, DolbyVisionStreamDetails(parameters));
    if (parameters) {
        const auto color = BuildColorMetadata(parameters);
        if (color.IsHdr()) {
            parts.push_back(ToDisplayString(color.transfer));
        }
        if (color.primaries != VideoColorPrimaries::Unknown) {
            parts.push_back(ToDisplayString(color.primaries));
        }
    }

    const double frameRate = ProbeFrameRate(stream, parameters);
    if (frameRate > 0.0) {
        std::wostringstream frameRateText;
        frameRateText.precision(2);
        frameRateText << std::fixed << frameRate << L" fps";
        parts.push_back(frameRateText.str());
    }

    return JoinParts(parts, L"Video stream");
}

std::wstring BuildAudioDetails(const AVCodecParameters* parameters) {
    std::vector<std::wstring> parts;
    AppendPart(parts, ProfileName(parameters));
    if (parameters && parameters->sample_rate > 0) {
        parts.push_back(std::to_wstring(parameters->sample_rate) + L" Hz");
    }
    if (parameters && parameters->ch_layout.nb_channels > 0) {
        parts.push_back(std::to_wstring(parameters->ch_layout.nb_channels) + L" ch");
    }
    return JoinParts(parts, L"Audio stream");
}

MediaProbeResult ProbeWithAvformat(const std::filesystem::path& path,
                                   const MediaProbeOptions& options) {
    av_log_set_level(AV_LOG_QUIET);

    ProbeInterruptState interrupt;
    interrupt.stopToken = options.stopToken;
    if (options.timeout.count() > 0) {
        interrupt.deadline = std::chrono::steady_clock::now() + options.timeout;
    }
    if (interrupt.Interrupted()) {
        return InterruptedProbeResult(path, interrupt);
    }

    MediaProbeResult result;
    result.probeTool = L"avformat";
    result.descriptor.path = path;
    result.descriptor.displayName = path.filename().wstring();
    result.descriptor.container = UppercaseExtension(path);
    result.descriptor.hdrFormat = L"SDR / unknown";
    result.descriptor.selectedDecodePath = L"Probe only";

    AVFormatContext* formatContext = avformat_alloc_context();
    if (!formatContext) {
        return MediaProbe::ExtensionFallback(path, L"avformat_alloc_context failed");
    }
    formatContext->interrupt_callback.callback = &ProbeInterruptCallback;
    formatContext->interrupt_callback.opaque = &interrupt;
    const std::string pathUtf8 = WideToUtf8(path.wstring());
    AVDictionary* inputOptions = nullptr;
    if (options.timeout.count() > 0) {
        const auto timeoutUs = std::chrono::duration_cast<std::chrono::microseconds>(options.timeout).count();
        const std::string timeoutValue = std::to_string(std::max<int64_t>(1, timeoutUs));
        av_dict_set(&inputOptions, "rw_timeout", timeoutValue.c_str(), 0);
        av_dict_set(&inputOptions, "timeout", timeoutValue.c_str(), 0);
    }
    int error = avformat_open_input(&formatContext, pathUtf8.c_str(), nullptr, &inputOptions);
    av_dict_free(&inputOptions);
    if (error < 0) {
        if (interrupt.Interrupted()) {
            avformat_close_input(&formatContext);
            return InterruptedProbeResult(path, interrupt);
        }
        avformat_close_input(&formatContext);
        if (error == AVERROR(ENOENT) || error == AVERROR(EACCES) || error == AVERROR(EISDIR)) {
            result.failed = true;
            result.diagnostic = L"avformat_open_input failed: " + FfmpegErrorString(error);
            return result;
        }
        return MediaProbe::ExtensionFallback(path, L"avformat_open_input failed: " + FfmpegErrorString(error));
    }

    error = avformat_find_stream_info(formatContext, nullptr);
    if (error < 0) {
        avformat_close_input(&formatContext);
        if (interrupt.Interrupted()) {
            return InterruptedProbeResult(path, interrupt);
        }
        return MediaProbe::ExtensionFallback(path, L"avformat_find_stream_info failed: " + FfmpegErrorString(error));
    }

    result.descriptor.container = ContainerName(formatContext, path);
    result.descriptor.duration = DurationFromContext(formatContext);

    for (unsigned int index = 0; index < formatContext->nb_streams; ++index) {
        const AVStream* stream = formatContext->streams[index];
        const AVCodecParameters* parameters = stream ? stream->codecpar : nullptr;
        if (!stream || !parameters) {
            continue;
        }

        const int streamIndex = stream->index >= 0 ? stream->index : static_cast<int>(index);
        const auto codec = CodecName(parameters);
        const auto language = StreamLanguage(stream);

        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            result.descriptor.hasVideo = true;
            if (const auto dovi = DolbyVisionProfile(parameters); !dovi.empty()) {
                result.descriptor.dolbyVisionDetected = true;
                result.descriptor.hdrFormat = dovi;
            }
            if (result.descriptor.videoCodec.empty()) {
                const auto color = BuildColorMetadata(parameters);
                result.descriptor.videoCodec = codec;
                result.descriptor.videoColor = color;
                if (!result.descriptor.dolbyVisionDetected) {
                    result.descriptor.hdrFormat = DetectHdrFormat(parameters, color);
                    result.descriptor.dolbyVisionDetected =
                        result.descriptor.hdrFormat.find(L"Dolby Vision") != std::wstring::npos;
                }
                result.descriptor.videoWidth = parameters->width;
                result.descriptor.videoHeight = parameters->height;
                result.descriptor.videoFrameRate = ProbeFrameRate(stream, parameters);
            }
            if (result.descriptor.streams.size() < kMaxReportedStreams) {
                result.descriptor.streams.push_back(MediaStreamSummary{
                    streamIndex,
                    L"Video",
                    codec,
                    language,
                    BuildVideoDetails(stream, parameters),
                });
            }
        } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            result.descriptor.hasAudio = true;
            if (result.descriptor.audioCodec.empty()) {
                result.descriptor.audioCodec = codec;
            }
            if (result.descriptor.streams.size() < kMaxReportedStreams) {
                result.descriptor.streams.push_back(MediaStreamSummary{
                    streamIndex,
                    L"Audio",
                    codec,
                    language,
                    BuildAudioDetails(parameters),
                });
            }
        } else if (parameters->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            if (result.descriptor.streams.size() < kMaxReportedStreams) {
                result.descriptor.streams.push_back(MediaStreamSummary{
                    streamIndex,
                    L"Subtitle",
                    codec,
                    language,
                    L"Subtitle stream",
                });
            }
        }
    }

    avformat_close_input(&formatContext);

    if (!result.descriptor.hasVideo && !result.descriptor.hasAudio) {
        return MediaProbe::ExtensionFallback(path, L"avformat returned no playable streams");
    }

    if (result.descriptor.videoCodec.empty()) {
        result.descriptor.videoCodec = L"No video";
    }
    if (result.descriptor.audioCodec.empty()) {
        result.descriptor.audioCodec = L"No audio";
    }
    return result;
}

}  // namespace

MediaProbeResult MediaProbe::Probe(const std::filesystem::path& path,
                                   const MediaProbeOptions& options) {
    return ProbeWithAvformat(path, options);
}

MediaProbeResult MediaProbe::ExtensionFallback(const std::filesystem::path& path, std::wstring diagnostic) {
    MediaProbeResult result;
    result.fallbackUsed = true;
    result.probeTool = L"extension_fallback";
    result.diagnostic = std::move(diagnostic);
    result.descriptor.path = path;
    result.descriptor.displayName = path.filename().wstring();
    result.descriptor.container = UppercaseExtension(path);
    result.descriptor.videoCodec = GuessVideoCodec(result.descriptor.container);
    result.descriptor.audioCodec = GuessAudioCodec(result.descriptor.container);
    result.descriptor.hdrFormat = L"Probe pending";
    result.descriptor.selectedDecodePath = L"Not initialized";
    result.descriptor.duration = std::chrono::minutes{90};
    result.descriptor.hasVideo = true;
    result.descriptor.hasAudio = true;
    result.descriptor.streams = {
        MediaStreamSummary{0, L"Video", result.descriptor.videoCodec, L"", L"SDR/HDR probe pending"},
        MediaStreamSummary{1, L"Audio", result.descriptor.audioCodec, L"Auto", L"WASAPI PCM pending"},
    };
    return result;
}

}  // namespace anvil::playback
