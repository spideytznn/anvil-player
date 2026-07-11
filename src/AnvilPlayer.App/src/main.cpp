#include "AnvilPlayer/App/app_arguments.h"
#include "AnvilPlayer/App/application.h"
#include "AnvilPlayer/App/single_instance.h"
#include "AnvilPlayer/App/ui_draw.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <filesystem>
#include <array>
#include <stdexcept>
#include <string>

namespace anvil::app {
}  // namespace anvil::app

namespace {

bool SetRegistryString(HKEY root,
                       const std::wstring& subkey,
                       const wchar_t* valueName,
                       const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root,
                        subkey.c_str(),
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(key,
                                       valueName,
                                       0,
                                       REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()),
                                       bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SetRegistryNone(HKEY root, const std::wstring& subkey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root,
                        subkey.c_str(),
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(key, valueName, 0, REG_NONE, nullptr, 0);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void RegisterCurrentUserOpenWith() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr,
                                            executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return;
    }

    const std::wstring executablePath(executable.data(), length);
    constexpr wchar_t kCapabilitiesPath[] = L"Software\\Classes\\AnvilPlayer\\Capabilities";
    constexpr wchar_t kProgId[] = L"AnvilPlayer.Media";
    constexpr std::array<const wchar_t*, 7> kExtensions = {
        L".mp4", L".mkv", L".mov", L".m2ts", L".ts", L".webm", L".avi"};

    bool changed = false;
    changed |= SetRegistryString(HKEY_CURRENT_USER,
                                 L"Software\\RegisteredApplications",
                                 L"AnvilPlayer",
                                 kCapabilitiesPath);
    changed |= SetRegistryString(HKEY_CURRENT_USER, kCapabilitiesPath, L"ApplicationName", L"Anvil Player");
    changed |= SetRegistryString(HKEY_CURRENT_USER,
                                 kCapabilitiesPath,
                                 L"ApplicationDescription",
                                 L"Anvil Player media playback application");
    changed |= SetRegistryString(HKEY_CURRENT_USER,
                                 L"Software\\Classes\\AnvilPlayer.Media\\DefaultIcon",
                                 nullptr,
                                 executablePath + L",0");
    changed |= SetRegistryString(HKEY_CURRENT_USER,
                                 L"Software\\Classes\\AnvilPlayer.Media\\shell\\open\\command",
                                 nullptr,
                                 L"\"" + executablePath + L"\" --play \"%1\"");

    for (const wchar_t* extension : kExtensions) {
        changed |= SetRegistryString(HKEY_CURRENT_USER,
                                     std::wstring(kCapabilitiesPath) + L"\\FileAssociations",
                                     extension,
                                     kProgId);
        changed |= SetRegistryNone(HKEY_CURRENT_USER,
                                   L"Software\\Classes\\" + std::wstring(extension) + L"\\OpenWithProgids",
                                   kProgId);
    }
    if (changed) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int commandShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // OLE initialization includes STA COM and is required for RegisterDragDrop.
    const HRESULT comResult = OleInitialize(nullptr);
    const bool comInitialized = SUCCEEDED(comResult);
    RegisterCurrentUserOpenWith();

    anvil::app::GdiplusSession gdiplus;
    if (!gdiplus.Ready()) {
        MessageBoxW(nullptr, L"Unable to initialize the icon renderer.", L"Anvil Player", MB_ICONERROR | MB_OK);
        if (comInitialized) {
            OleUninitialize();
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
            OleUninitialize();
        }
        return 0;
    }

    if (!comInitialized) {
        appArguments.webUiEnabled = false;
    }

    int exitCode = 1;
    {
        // Keep every Application-owned WebView/COM object inside this scope.
        // Its destructor must run before the apartment is uninitialized.
        anvil::app::Application app;
        if (!app.Initialize(instance, commandShow, appArguments)) {
            MessageBoxW(nullptr,
                        L"Unable to create Anvil Player library window.",
                        L"Anvil Player",
                        MB_ICONERROR | MB_OK);
        } else {
            // A media path on the command line (first launch) opens the player.
            if (!appArguments.mediaPath.empty()) {
                app.OpenInPlayer(appArguments.mediaPath, 0.0);
            }
            exitCode = app.Run();
        }
    }
    if (comInitialized) {
        OleUninitialize();
    }
    return exitCode;
}
