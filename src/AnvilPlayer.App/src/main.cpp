#include "AnvilPlayer/App/main_window.h"
#include "AnvilPlayer/App/ui_draw.h"
#include "AnvilPlayer/App/ui_types.h"
#include "AnvilPlayer/Playback/PlayerController.h"

#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace anvil::app {

using anvil::playback::DefaultLogLevel;
using anvil::playback::LogLevel;
using anvil::playback::LogLevelFromString;

struct AppArguments {
    std::filesystem::path mediaPath;
    bool autoplay = false;
    LogLevel logLevel = DefaultLogLevel();
    PlaybackBackend backend = PlaybackBackend::NativeFfmpegD3D11;
    int selectedVideoTrackIndex = anvil::playback::kVideoTrackAuto;
};

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
            parsed.logLevel = LogLevelFromString(arguments[++index], parsed.logLevel);
            continue;
        }
        if (argument == L"--debug-log") {
            parsed.logLevel = LogLevel::Debug;
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
            parsed.logLevel = LogLevel::Info;
            continue;
        }
        if (argument == L"--native-playback") {
            parsed.backend = PlaybackBackend::NativeFfmpegD3D11;
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

}  // namespace anvil::app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int commandShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    anvil::app::GdiplusSession gdiplus;
    if (!gdiplus.Ready()) {
        MessageBoxW(nullptr, L"Unable to initialize the icon renderer.", L"Anvil Player", MB_ICONERROR | MB_OK);
        return 1;
    }

    anvil::app::AppArguments appArguments;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments) {
        appArguments = anvil::app::ParseArguments(argumentCount, arguments);
        LocalFree(arguments);
    }

    anvil::app::MainWindow window;
    window.ConfigureLogging(appArguments.logLevel);
    window.SetBackend(appArguments.backend);
    window.SetInitialVideoTrackSelection(appArguments.selectedVideoTrackIndex);
    if (!window.Create(instance)) {
        MessageBoxW(nullptr, L"Unable to create Anvil Player window.", L"Anvil Player", MB_ICONERROR | MB_OK);
        return 1;
    }

    window.Show(commandShow);
    window.OpenInitialPath(appArguments.mediaPath, appArguments.autoplay);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
