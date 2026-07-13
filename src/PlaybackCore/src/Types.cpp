#include "AnvilPlayer/Playback/Types.h"

#include <iomanip>
#include <sstream>

namespace anvil::playback {

std::wstring ToDisplayString(const PlaybackState state) {
    switch (state) {
    case PlaybackState::Empty:
        return L"Empty";
    case PlaybackState::Ready:
        return L"Ready";
    case PlaybackState::Playing:
        return L"Playing";
    case PlaybackState::Paused:
        return L"Paused";
    case PlaybackState::Stopped:
        return L"Stopped";
    case PlaybackState::Error:
        return L"Error";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const HardwareDecodeMode mode) {
    switch (mode) {
    case HardwareDecodeMode::Auto:
        return L"Auto";
    case HardwareDecodeMode::D3D12VA:
        return L"D3D12VA";
    case HardwareDecodeMode::Off:
        return L"Off";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const HdrOutputMode mode) {
    switch (mode) {
    case HdrOutputMode::Auto:
        return L"Auto";
    case HdrOutputMode::ForceHdr:
        return L"Force HDR";
    case HdrOutputMode::ForceSdr:
        return L"Force SDR";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const ToneMappingMode mode) {
    switch (mode) {
    case ToneMappingMode::Auto:
        return L"Auto";
    case ToneMappingMode::Balanced:
        return L"Balanced";
    case ToneMappingMode::PreserveHighlights:
        return L"Preserve highlights";
    case ToneMappingMode::BrightRoom:
        return L"Bright room";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const DolbyVisionMode mode) {
    switch (mode) {
    case DolbyVisionMode::FallbackOnly:
        return L"Fallback only";
    case DolbyVisionMode::ExperimentalPassthrough:
        return L"Experimental passthrough";
    case DolbyVisionMode::Off:
        return L"Off";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const AudioOutputMode mode) {
    switch (mode) {
    case AudioOutputMode::Auto:
        return L"Auto";
    case AudioOutputMode::Pcm:
        return L"PCM";
    case AudioOutputMode::Passthrough:
        return L"Passthrough";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const WasapiMode mode) {
    switch (mode) {
    case WasapiMode::Shared:
        return L"Shared";
    case WasapiMode::Exclusive:
        return L"Exclusive";
    }
    return L"Unknown";
}

std::wstring ToDisplayString(const VideoColorPrimaries value) {
    switch (value) {
    case VideoColorPrimaries::Bt709:
        return L"bt709";
    case VideoColorPrimaries::Bt2020:
        return L"bt2020";
    case VideoColorPrimaries::DisplayP3:
        return L"display-p3";
    case VideoColorPrimaries::Unknown:
        break;
    }
    return L"unknown";
}

std::wstring ToDisplayString(const VideoTransferCharacteristic value) {
    switch (value) {
    case VideoTransferCharacteristic::Bt709:
        return L"bt709";
    case VideoTransferCharacteristic::Srgb:
        return L"srgb";
    case VideoTransferCharacteristic::Pq:
        return L"pq";
    case VideoTransferCharacteristic::Hlg:
        return L"hlg";
    case VideoTransferCharacteristic::Unknown:
        break;
    }
    return L"unknown";
}

std::wstring ToDisplayString(const VideoMatrixCoefficients value) {
    switch (value) {
    case VideoMatrixCoefficients::Bt709:
        return L"bt709";
    case VideoMatrixCoefficients::Bt601:
        return L"bt601";
    case VideoMatrixCoefficients::Bt2020Ncl:
        return L"bt2020nc";
    case VideoMatrixCoefficients::Bt2020Cl:
        return L"bt2020c";
    case VideoMatrixCoefficients::Rgb:
        return L"rgb";
    case VideoMatrixCoefficients::Unknown:
        break;
    }
    return L"unknown";
}

std::wstring ToDisplayString(const VideoColorRange value) {
    switch (value) {
    case VideoColorRange::Limited:
        return L"limited";
    case VideoColorRange::Full:
        return L"full";
    case VideoColorRange::Unknown:
        break;
    }
    return L"unknown";
}

std::wstring FormatMasteringDisplay(const MasteringDisplayMetadata& metadata) {
    if (!metadata.hasLuminance && !metadata.hasPrimaries) {
        return L"-";
    }

    std::wostringstream stream;
    stream << std::fixed << std::setprecision(4);
    if (metadata.hasLuminance) {
        stream << L"min=" << metadata.minLuminanceNits << L" max=" << metadata.maxLuminanceNits << L" nits";
    }
    if (metadata.hasPrimaries) {
        if (metadata.hasLuminance) {
            stream << L" ";
        }
        stream << L"R(" << metadata.red.x << L"," << metadata.red.y << L") "
               << L"G(" << metadata.green.x << L"," << metadata.green.y << L") "
               << L"B(" << metadata.blue.x << L"," << metadata.blue.y << L") "
               << L"W(" << metadata.whitePoint.x << L"," << metadata.whitePoint.y << L")";
    }
    return stream.str();
}

std::wstring FormatContentLight(const ContentLightMetadata& metadata) {
    if (!metadata.hasValues) {
        return L"-";
    }
    return L"max_cll=" + std::to_wstring(metadata.maxContentLightLevelNits) +
           L" max_fall=" + std::to_wstring(metadata.maxFrameAverageLightLevelNits) +
           L" nits";
}

std::wstring FormatTimecode(const std::chrono::milliseconds value) {
    const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(value).count();
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;

    std::wostringstream stream;
    stream << std::setfill(L'0');
    if (hours > 0) {
        stream << hours << L':' << std::setw(2) << minutes << L':' << std::setw(2) << seconds;
    } else {
        stream << minutes << L':' << std::setw(2) << seconds;
    }
    return stream.str();
}

}  // namespace anvil::playback
