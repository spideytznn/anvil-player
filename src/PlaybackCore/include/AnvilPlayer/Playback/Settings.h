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
    bool autoDisplayFormat = false;
    // Generate GPU motion-compensated intermediate frames. The saved refresh
    // rate synchronization preference remains intact, but is not effective
    // while this option is enabled.
    bool frameInterpolationEnabled = false;
    HardwareDecodeMode hardwareDecode = HardwareDecodeMode::Auto;
    std::wstring renderer = L"D3D12";
    int selectedTrackIndex = kVideoTrackAuto;
    HdrOutputMode hdrOutput = HdrOutputMode::Auto;
    ToneMappingMode toneMapping = ToneMappingMode::Balanced;
    DolbyVisionMode dolbyVision = DolbyVisionMode::FallbackOnly;
    // Pass source mastering-display and content-light metadata to the active
    // HDR swap chain. Color-space selection remains independent so disabling
    // this switch never silently turns HDR video into SDR. HDR10+ content uses
    // app-side ST 2094-40 tone mapping while passthrough is disabled.
    bool displayMetadataPassthrough = false;
    // Opt-in Windows MediaEngine + Dolby renderer-extension presentation.
    // The default remains the native FFmpeg/libplacebo RPU path.
    bool dolbyVisionSystemPipelineExperimental = false;
    // Prefer the Windows/native Dolby Vision presentation path when the
    // display and installed system components expose it. The FFmpeg/D3D12
    // renderer must fall back to software DV reshape when native signaling is
    // unavailable; it must never label HDR10 output as native Dolby Vision.
    bool dolbyVisionHdrOutput = false;
    // Dolby Vision creative trims and enhancement-layer processing are always
    // enabled for supported software presentation paths.
    bool dolbyVisionCmv4Approx = true;
    // Target peak of the physical HDR display. Zero selects the peak reported
    // by Windows for the monitor containing the player; invalid/missing OS
    // data falls back to 1000 nits in the renderer.
    int displayPeakBrightnessNits = 0;
    // Output peak of the optional user-authored HDR curve. This is separate
    // from the physical display target above.
    int peakBrightnessNits = 1000;
    std::array<HdrToneCurvePoint, kHdrToneCurvePointCount> hdrToneCurve = kDefaultHdrToneCurve;
};

struct AudioSettings {
    std::wstring outputDevice = L"Auto";
    AudioOutputMode outputMode = AudioOutputMode::Auto;
    WasapiMode wasapiMode = WasapiMode::Shared;
    int selectedTrackIndex = kAudioTrackAuto;
    // Prefer encoded IEC 61937 output for the current media. Unsupported
    // codecs, endpoint negotiation failures, and runtime failures fall back
    // to shared-mode PCM while preserving the user's preference.
    bool passthroughPreferred = false;
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
