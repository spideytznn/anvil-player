#include "AnvilPlayer/App/app_arguments.h"
#include "AnvilPlayer/App/application.h"
#include "AnvilPlayer/App/single_instance.h"
#include "AnvilPlayer/App/ui_draw.h"

#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace anvil::app {
}  // namespace anvil::app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int commandShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(comResult);

    anvil::app::GdiplusSession gdiplus;
    if (!gdiplus.Ready()) {
        MessageBoxW(nullptr, L"Unable to initialize the icon renderer.", L"Anvil Player", MB_ICONERROR | MB_OK);
        if (comInitialized) {
            CoUninitialize();
        }
        return 1;
    }

    anvil::app::AppArguments appArguments;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments) {
        appArguments = anvil::app::ParseArguments(argumentCount, arguments);
        LocalFree(arguments);
    }

    // Single-instance enforcement: if another process already holds the mutex,
    // forward this launch's command line to it and exit without creating a
    // window. The first instance owns the mutex for the lifetime of the process
    // via the static guard held in single_instance.cpp.
    const auto singleInstance = anvil::app::AcquireSingleInstance();
    if (!singleInstance.acquired) {
        anvil::app::ForwardCommandLineToRunningInstance(GetCommandLineW());
        if (comInitialized) {
            CoUninitialize();
        }
        return 0;
    }

    if (!comInitialized) {
        appArguments.webUiEnabled = false;
    }

    anvil::app::Application app;
    if (!app.Initialize(instance, commandShow, appArguments)) {
        MessageBoxW(nullptr, L"Unable to create Anvil Player library window.", L"Anvil Player", MB_ICONERROR | MB_OK);
        if (comInitialized) {
            CoUninitialize();
        }
        return 1;
    }

    // A media path on the command line (first launch) opens the player.
    if (!appArguments.mediaPath.empty()) {
        app.OpenInPlayer(appArguments.mediaPath, 0.0);
    }

    const int exitCode = app.Run();
    if (comInitialized) {
        CoUninitialize();
    }
    return exitCode;
}
