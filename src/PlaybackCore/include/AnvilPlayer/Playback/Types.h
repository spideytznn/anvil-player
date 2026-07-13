#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::playback {

enum class PlaybackState {
    Empty,
    Ready,
    Playing,
    Paused,
    Stopped,
    Error,
};

enum class HardwareDecodeMode {
    Auto,
    D3D12VA,
    Off,
};

enum class HdrOutputMode {
    Auto,
    ForceHdr,
    ForceSdr,
};

enum class ToneMappingMode {
    Auto,
    Balanced,
    PreserveHighlights,
    BrightRoom,
};

enum class DolbyVisionMode {
    FallbackOnly,
    ExperimentalPassthrough,
    Off,
};

enum class AudioOutputMode {
    Auto,
    Pcm,
    Passthrough,
};

enum class WasapiMode {
    Shared,
    Exclusive,
};

enum class VideoColorPrimaries {
    Unknown,
    Bt709,
    Bt2020,
    DisplayP3,
};

enum class VideoTransferCharacteristic {
    Unknown,
    Bt709,
    Srgb,
    Pq,
    Hlg,
};

enum class VideoMatrixCoefficients {
    Unknown,
    Bt709,
    Bt601,
    Bt2020Ncl,
    Bt2020Cl,
    Rgb,
};

enum class VideoColorRange {
    Unknown,
    Limited,
    Full,
};

struct ChromaticityPoint {
    double x = 0.0;
    double y = 0.0;
};

struct MasteringDisplayMetadata {
    bool hasPrimaries = false;
    ChromaticityPoint red;
    ChromaticityPoint green;
    ChromaticityPoint blue;
    ChromaticityPoint whitePoint;
    bool hasLuminance = false;
    double minLuminanceNits = 0.0;
    double maxLuminanceNits = 0.0;
};

struct ContentLightMetadata {
    bool hasValues = false;
    int maxContentLightLevelNits = 0;
    int maxFrameAverageLightLevelNits = 0;
};

struct VideoColorMetadata {
    VideoColorPrimaries primaries = VideoColorPrimaries::Unknown;
    VideoTransferCharacteristic transfer = VideoTransferCharacteristic::Unknown;
    VideoMatrixCoefficients matrix = VideoMatrixCoefficients::Unknown;
    VideoColorRange range = VideoColorRange::Unknown;
    MasteringDisplayMetadata masteringDisplay;
    ContentLightMetadata contentLight;

    bool IsHdr() const {
        return transfer == VideoTransferCharacteristic::Pq ||
               transfer == VideoTransferCharacteristic::Hlg;
    }
};

struct MediaStreamSummary {
    int index = -1;
    std::wstring kind;
    std::wstring codec;
    std::wstring language;
    std::wstring details;
};

struct MediaDescriptor {
    std::filesystem::path path;
    std::wstring displayName;
    std::wstring container;
    std::wstring videoCodec;
    std::wstring audioCodec;
    std::wstring hdrFormat;
    std::wstring selectedDecodePath;
    VideoColorMetadata videoColor;
    std::filesystem::path previewImagePath;
    std::chrono::milliseconds duration{0};
    int videoWidth = 0;
    int videoHeight = 0;
    double videoFrameRate = 0.0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool dolbyVisionDetected = false;
    std::vector<MediaStreamSummary> streams;
};

struct PlaybackSessionSnapshot {
    PlaybackState state = PlaybackState::Empty;
    std::optional<MediaDescriptor> media;
    std::chrono::milliseconds position{0};
    double volume = 1.0;
    double playbackRate = 1.0;
    std::wstring lastError;
};

std::wstring ToDisplayString(PlaybackState state);
std::wstring ToDisplayString(HardwareDecodeMode mode);
std::wstring ToDisplayString(HdrOutputMode mode);
std::wstring ToDisplayString(ToneMappingMode mode);
std::wstring ToDisplayString(DolbyVisionMode mode);
std::wstring ToDisplayString(AudioOutputMode mode);
std::wstring ToDisplayString(WasapiMode mode);
std::wstring ToDisplayString(VideoColorPrimaries value);
std::wstring ToDisplayString(VideoTransferCharacteristic value);
std::wstring ToDisplayString(VideoMatrixCoefficients value);
std::wstring ToDisplayString(VideoColorRange value);
std::wstring FormatMasteringDisplay(const MasteringDisplayMetadata& metadata);
std::wstring FormatContentLight(const ContentLightMetadata& metadata);
std::wstring FormatTimecode(std::chrono::milliseconds value);

}  // namespace anvil::playback
