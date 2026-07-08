#include "AnvilPlayer/App/embedded_ffplay.h"

#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/ui_draw.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace anvil::app {

namespace {

std::filesystem::path FindBundledFfplayFrom(std::filesystem::path start) {
    std::error_code error;
    for (int depth = 0; depth < 6 && !start.empty(); ++depth) {
        const auto candidate = start / L"third_party" / L"ffmpeg" / L"bin" / L"ffplay.exe";
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }

        const auto parent = start.parent_path();
        if (parent == start) {
            break;
        }
        start = parent;
    }
    return {};
}

std::filesystem::path FfplayExecutablePath() {
    std::error_code error;

    wchar_t modulePath[MAX_PATH]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (moduleLength > 0 && moduleLength < std::size(modulePath)) {
        const auto executableDir = std::filesystem::path(modulePath).parent_path();
        const auto besideExecutable = executableDir / L"ffplay.exe";
        if (std::filesystem::exists(besideExecutable, error)) {
            return besideExecutable;
        }
        if (const auto bundled = FindBundledFfplayFrom(executableDir); !bundled.empty()) {
            return bundled;
        }
    }

    if (const auto bundled = FindBundledFfplayFrom(std::filesystem::current_path(error)); !bundled.empty()) {
        return bundled;
    }

    return L"ffplay.exe";
}

bool IsNetworkMediaPath(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value.rfind(L"http://", 0) == 0 ||
           value.rfind(L"https://", 0) == 0;
}

std::wstring DolbyVisionLibplaceboFilter() {
    return L"libplacebo="
           L"apply_dolbyvision=1:"
           L"tonemapping=auto:"
           L"gamut_mode=perceptual:"
           L"peak_detect=1:"
           L"colorspace=bt709:"
           L"color_primaries=bt709:"
           L"color_trc=bt709:"
           L"range=pc";
}

}  // namespace

BOOL CALLBACK FindProcessWindowProc(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<WindowSearch*>(lParam);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (!search->title || search->title->empty() || *search->title == title) {
        search->window = hwnd;
        return FALSE;
    }
    return TRUE;
}

EmbeddedFfplayPlayer::~EmbeddedFfplayPlayer() {
    Stop();
}

bool EmbeddedFfplayPlayer::Start(HWND parent,
                                 RECT bounds,
                                 const std::filesystem::path& mediaPath,
                                 const std::chrono::milliseconds startPosition,
                                 const double volume,
                                 const bool useLibplaceboDolbyVision) {
    Stop();
    if (!parent || mediaPath.empty()) {
        return false;
    }

    windowTitle_ = L"Anvil Player Playback " +
                   std::to_wstring(GetCurrentProcessId()) +
                   L"-" +
                   std::to_wstring(++launchSerial_);

    const int volumePercent = std::clamp(static_cast<int>(volume * 100.0 + 0.5), 0, 100);
    const int playbackWidth = std::max(1, RectWidth(bounds));
    const int playbackHeight = std::max(1, RectHeight(bounds));
    const bool networkMedia = IsNetworkMediaPath(mediaPath);
    std::wstring commandLine =
        QuoteArgument(FfplayExecutablePath().wstring()) +
        L" -hide_banner -loglevel warning -autoexit "
        L"-hwaccel d3d11va -framedrop -noborder "
        L"-window_title " +
        QuoteArgument(windowTitle_) +
        L" -x " +
        std::to_wstring(playbackWidth) +
        L" -y " +
        std::to_wstring(playbackHeight) +
        L" -ss " +
        FormatFfmpegSeekTime(startPosition) +
        L" -volume " + std::to_wstring(volumePercent);
    if (networkMedia) {
        commandLine +=
            L" -rw_timeout 15000000"
            L" -seekable 1"
            L" -http_seekable 1"
            L" -reconnect_on_network_error 1"
            L" -reconnect_streamed 1"
            L" -reconnect_delay_max 2"
            L" -user_agent " +
            QuoteArgument(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36");
    }
    if (useLibplaceboDolbyVision) {
        commandLine += L" -vf " + QuoteArgument(DolbyVisionLibplaceboFilter());
    }
    commandLine +=
        L" " + QuoteArgument(mediaPath.wstring());
    lastCommandLine_ = commandLine;

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags |= STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    if (!created) {
        return false;
    }

    process_ = processInfo;
    parent_ = parent;
    {
        std::scoped_lock lock(windowMutex_);
        childWindow_ = nullptr;
        pendingBounds_ = bounds;
    }
    stopping_ = false;
    running_ = true;
    attachThread_ = std::thread([this, parent, bounds, processId = processInfo.dwProcessId, title = windowTitle_]() {
        AttachWindowLoop(parent, bounds, processId, title);
    });
    return true;
}

void EmbeddedFfplayPlayer::Stop() {
    stopping_ = true;
    if (process_.hProcess) {
        TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 2000);
    }
    if (attachThread_.joinable()) {
        attachThread_.join();
    }
    {
        std::scoped_lock lock(windowMutex_);
        childWindow_ = nullptr;
    }
    if (process_.hThread) {
        CloseHandle(process_.hThread);
        process_.hThread = nullptr;
    }
    if (process_.hProcess) {
        CloseHandle(process_.hProcess);
        process_.hProcess = nullptr;
    }
    running_ = false;
}

bool EmbeddedFfplayPlayer::IsRunning() const {
    if (!process_.hProcess) {
        return false;
    }
    return WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT;
}

bool EmbeddedFfplayPlayer::HasAttachedWindow() const {
    std::scoped_lock lock(windowMutex_);
    return childWindow_ && IsWindow(childWindow_);
}

void EmbeddedFfplayPlayer::SetBounds(const RECT bounds) {
    std::scoped_lock lock(windowMutex_);
    pendingBounds_ = bounds;
    if (childWindow_ && IsWindow(childWindow_)) {
        if (RectWidth(bounds) <= 0 || RectHeight(bounds) <= 0) {
            ShowWindow(childWindow_, SW_HIDE);
            return;
        }
        SetWindowPos(childWindow_,
                     HWND_TOP,
                     bounds.left,
                     bounds.top,
                     RectWidth(bounds),
                     RectHeight(bounds),
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ShowWindow(childWindow_, SW_SHOWNOACTIVATE);
    }
}

void EmbeddedFfplayPlayer::AttachWindowLoop(HWND parent, RECT bounds, const DWORD processId, const std::wstring title) {
    constexpr int kAttempts = 80;
    constexpr DWORD kDelayMs = 25;
    for (int attempt = 0; attempt < kAttempts && !stopping_; ++attempt) {
        if (!process_.hProcess || WaitForSingleObject(process_.hProcess, 0) != WAIT_TIMEOUT) {
            return;
        }

        WindowSearch search{processId, &title, nullptr};
        EnumWindows(FindProcessWindowProc, reinterpret_cast<LPARAM>(&search));
        if (!search.window) {
            Sleep(kDelayMs);
            continue;
        }

        AttachWindow(parent, search.window, bounds);
        return;
    }
}

void EmbeddedFfplayPlayer::AttachWindow(HWND parent, HWND child, RECT bounds) {
    if (!parent || !child || stopping_) {
        return;
    }

    // Hide the ffplay window before reparenting so it never flashes as a
    // standalone top-level window while play/seek restarts.
    ShowWindow(child, SW_HIDE);
    SetParent(child, parent);
    LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(child, GWL_EXSTYLE, exStyle);

    {
        std::scoped_lock lock(windowMutex_);
        childWindow_ = child;
        if (RectWidth(pendingBounds_) > 0 && RectHeight(pendingBounds_) > 0) {
            bounds = pendingBounds_;
        }
    }

    const int width = RectWidth(bounds);
    const int height = RectHeight(bounds);
    if (width <= 0 || height <= 0) {
        MoveWindow(child, 0, 0, 1, 1, TRUE);
        ShowWindow(child, SW_HIDE);
        return;
    }

    SetWindowPos(child,
                 HWND_TOP,
                 bounds.left,
                 bounds.top,
                 width,
                 height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // Reveal only after the child is correctly parented and sized. WebView is
    // opaque, so external playback has to sit above the playback surface.
    ShowWindow(child, SW_SHOWNOACTIVATE);
}

}  // namespace anvil::app
