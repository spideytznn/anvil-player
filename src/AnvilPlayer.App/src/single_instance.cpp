#include "AnvilPlayer/App/single_instance.h"

#include "AnvilPlayer/App/app_messages.h"

#include <windows.h>

#include <cstring>

namespace anvil::app {

namespace {

HANDLE g_singleInstanceMutex = nullptr;
bool g_acquired = false;

// Restores and focuses a minimized/hidden window so the running instance
// becomes visible when a second launch forwards a command line to it.
void BringWindowToForeground(HWND window) {
    if (!window) {
        return;
    }
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    }
    // AllowSetSetForegroundWindow-style handoff: flash then bring forward.
    DWORD foregroundPid = 0;
    const DWORD foregroundTid = GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const DWORD ourTid = GetCurrentThreadId();
    if (foregroundTid != 0 && foregroundPid != GetCurrentProcessId()) {
        AttachThreadInput(ourTid, foregroundTid, TRUE);
    }
    SetForegroundWindow(window);
    BringWindowToTop(window);
    if (foregroundTid != 0 && foregroundPid != GetCurrentProcessId()) {
        AttachThreadInput(ourTid, foregroundTid, FALSE);
    }
    ShowWindow(window, SW_SHOWNORMAL);
}

}  // namespace

SingleInstanceGuard AcquireSingleInstance() {
    if (g_acquired) {
        return SingleInstanceGuard{true};
    }
    // Take ownership of the named mutex. If it already exists (another instance
    // holds it) GetLastError reports ERROR_ALREADY_EXISTS.
    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (g_singleInstanceMutex == nullptr) {
        g_acquired = false;
        return SingleInstanceGuard{false};
    }
    g_acquired = GetLastError() != ERROR_ALREADY_EXISTS;
    return SingleInstanceGuard{g_acquired};
}

bool ForwardCommandLineToRunningInstance(const std::wstring& commandLine) {
    // The library window is the always-present rendezvous point for a running
    // instance (the player window is created lazily). Fall back to the player
    // window class for robustness.
    HWND existing = FindWindowW(kLibraryWindowClassName, nullptr);
    if (!existing) {
        existing = FindWindowW(kWindowClassName, nullptr);
    }
    if (!existing) {
        // The mutex holder may still be initializing its window. Retry briefly.
        for (int attempt = 0; attempt < 20; ++attempt) {
            Sleep(50);
            existing = FindWindowW(kWindowClassName, nullptr);
            if (existing) {
                break;
            }
        }
        if (!existing) {
            return false;
        }
    }

    BringWindowToForeground(existing);

    const std::size_t byteCount = commandLine.size() * sizeof(wchar_t);
    COPYDATASTRUCT copyData{};
    copyData.dwData = kForwardCommandLineMagic;
    copyData.cbData = static_cast<DWORD>(byteCount);
    copyData.lpData = const_cast<wchar_t*>(commandLine.c_str());

    // WM_COPYDATA is the only message for which Windows marshals the pointed-to
    // data across the process boundary, so the registered-message id is only
    // used to distinguish this payload on the receiving side via dwData.
    DWORD_PTR result = 0;
    // SendMessageTimeout avoids hanging if the running instance is unresponsive.
    const LRESULT sent = SendMessageTimeoutW(
        existing,
        WM_COPYDATA,
        reinterpret_cast<WPARAM>(nullptr),
        reinterpret_cast<LPARAM>(&copyData),
        SMTO_NORMAL,
        3000,
        &result);

    // SendMessageTimeout returns nonzero on success (message processed),
    // or zero on timeout/failure.
    return sent != 0;
}

}  // namespace anvil::app
