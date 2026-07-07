#pragma once

#include "AnvilPlayer/Playback/Types.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

namespace anvil::playback {

inline constexpr int kVideoTrackDolbyVisionEnhancement = -2;
inline constexpr int kVideoTrackAuto = -1;
inline constexpr int kAudioTrackAuto = -2;
inline constexpr int kAudioTrackOff = -1;
inline constexpr int kSubtitleTrackAuto = -2;
inline constexpr int kSubtitleTrackOff = -1;
inline constexpr std::size_t kHdrToneCurvePointCount = 9;

struct HdrToneCurvePoint {
    double inputNits = 0.0;
    double outputNits = 0.0;
};

inline constexpr std::array<HdrToneCurvePoint, kHdrToneCurvePointCount> kDefaultHdrToneCurve = {{
    {0.0, 0.0},
    {50.0, 50.0},
    {100.0, 100.0},
    {250.0, 235.0},
    {400.0, 360.0},
    {700.0, 540.0},
    {1000.0, 700.0},
    {2000.0, 880.0},
    {4000.0, 1000.0},
}};

struct VideoSettings {
    HardwareDecodeMode hardwareDecode = HardwareDecodeMode::Auto;
    std::wstring renderer = L"D3D11";
    int selectedTrackIndex = kVideoTrackAuto;
    HdrOutputMode hdrOutput = HdrOutputMode::Auto;
    ToneMappingMode toneMapping = ToneMappingMode::Balanced;
    DolbyVisionMode dolbyVision = DolbyVisionMode::FallbackOnly;
    bool dolbyVisionHdrOutput = false;
    bool dolbyVisionCmv4Approx = false;
    int peakBrightnessNits = 1000;
    std::array<HdrToneCurvePoint, kHdrToneCurvePointCount> hdrToneCurve = kDefaultHdrToneCurve;
};

struct AudioSettings {
    std::wstring outputDevice = L"Auto";
    AudioOutputMode outputMode = AudioOutputMode::Auto;
    WasapiMode wasapiMode = WasapiMode::Shared;
    int selectedTrackIndex = kAudioTrackAuto;
    bool ac3Passthrough = true;
    bool eac3Passthrough = true;
    bool trueHdPassthrough = true;
    bool dtsPassthrough = true;
    bool dtsHdPassthrough = true;
};

struct SubtitleSettings {
    std::wstring preferredLanguage = L"Auto";
    int selectedTrackIndex = kSubtitleTrackAuto;
    std::filesystem::path externalSubtitlePath;
    double fontScale = 1.0;
    int offsetXPx = 0;
    int offsetYPx = 0;
    int subtitleDelayMs = 0;
    bool externalSubtitleAutoLoad = true;
};

struct DanmakuSettings {
    bool enabled = false;
    int mode = 0;
    int opacityPercent = 70;
    int speedPercent = 100;
    std::filesystem::path externalDanmakuPath;
};

struct DiagnosticsSettings {
    bool showPlaybackStats = false;
    bool saveDebugLog = true;
};

struct PlayerSettings {
    VideoSettings video;
    AudioSettings audio;
    SubtitleSettings subtitles;
    DanmakuSettings danmaku;
    DiagnosticsSettings diagnostics;
};

PlayerSettings MakeDefaultSettings();

}  // namespace anvil::playback
