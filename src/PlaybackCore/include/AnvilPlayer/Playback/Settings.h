#pragma once

#include "AnvilPlayer/Playback/Types.h"

#include <string>

namespace anvil::playback {

inline constexpr int kSubtitleTrackAuto = -2;
inline constexpr int kSubtitleTrackOff = -1;

struct VideoSettings {
    HardwareDecodeMode hardwareDecode = HardwareDecodeMode::Auto;
    std::wstring renderer = L"D3D11";
    HdrOutputMode hdrOutput = HdrOutputMode::Auto;
    ToneMappingMode toneMapping = ToneMappingMode::Balanced;
    DolbyVisionMode dolbyVision = DolbyVisionMode::FallbackOnly;
    int peakBrightnessNits = 1000;
};

struct AudioSettings {
    std::wstring outputDevice = L"Auto";
    AudioOutputMode outputMode = AudioOutputMode::Auto;
    WasapiMode wasapiMode = WasapiMode::Shared;
    bool ac3Passthrough = true;
    bool eac3Passthrough = true;
    bool trueHdPassthrough = true;
    bool dtsPassthrough = true;
    bool dtsHdPassthrough = true;
};

struct SubtitleSettings {
    std::wstring preferredLanguage = L"Auto";
    int selectedTrackIndex = kSubtitleTrackAuto;
    double fontScale = 1.0;
    int subtitleDelayMs = 0;
    bool externalSubtitleAutoLoad = true;
};

struct DiagnosticsSettings {
    bool showPlaybackStats = false;
    bool saveDebugLog = true;
};

struct PlayerSettings {
    VideoSettings video;
    AudioSettings audio;
    SubtitleSettings subtitles;
    DiagnosticsSettings diagnostics;
};

PlayerSettings MakeDefaultSettings();

}  // namespace anvil::playback
