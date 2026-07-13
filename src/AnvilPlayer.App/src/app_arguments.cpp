#include "AnvilPlayer/App/app_arguments.h"

#include <windows.h>
#include <shellapi.h>

#include <stdexcept>
#include <string>

namespace anvil::app {

AppArguments ParseArguments(const int argumentCount, wchar_t** arguments) {
    AppArguments parsed;
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring argument = arguments[index];
        if ((argument == L"--play" || argument == L"--autoplay") && index + 1 < argumentCount) {
            parsed.autoplay = true;
            parsed.mediaPath = arguments[++index];
            continue;
        }
        if (argument == L"--log-level" && index + 1 < argumentCount) {
            parsed.logLevel = anvil::playback::LogLevelFromString(arguments[++index], parsed.logLevel);
            continue;
        }
        if (argument == L"--debug-log") {
            parsed.logLevel = anvil::playback::LogLevel::Debug;
            continue;
        }
        if (argument == L"--native-ui") {
            parsed.webUiEnabled = false;
            continue;
        }
        if (argument == L"--web-ui") {
            parsed.webUiEnabled = true;
            continue;
        }
        if (argument == L"--video-stream" && index + 1 < argumentCount) {
            try {
                const int streamIndex = std::stoi(arguments[++index]);
                if (streamIndex >= 0) {
                    parsed.selectedVideoTrackIndex = streamIndex;
                }
            } catch (const std::exception&) {
            }
            continue;
        }
        if (argument == L"--dolby-vision-el" || argument == L"--dv-el") {
            parsed.selectedVideoTrackIndex = anvil::playback::kVideoTrackDolbyVisionEnhancement;
            continue;
        }
        if (argument == L"--info-log") {
            parsed.logLevel = anvil::playback::LogLevel::Info;
            continue;
        }
        if (argument == L"--native-playback") {
            parsed.backend = PlaybackBackend::NativeFfmpegD3D12;
            continue;
        }
        if (argument == L"--external-playback") {
            parsed.backend = PlaybackBackend::EmbeddedFfplay;
            continue;
        }
        if (argument == L"--internal-playback" || argument == L"--in-player") {
            parsed.backend = PlaybackBackend::RawFrameBridge;
            continue;
        }
        if (parsed.mediaPath.empty()) {
            parsed.mediaPath = argument;
        }
    }
    return parsed;
}

AppArguments ParseCommandLine(const std::wstring& commandLine) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(commandLine.c_str(), &argumentCount);
    if (!arguments) {
        return AppArguments{};
    }
    AppArguments parsed = ParseArguments(argumentCount, arguments);
    LocalFree(arguments);
    return parsed;
}

}  // namespace anvil::app
