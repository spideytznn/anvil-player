#include "AnvilPlayer/App/library_window.h"

#include "AnvilPlayer/App/app_arguments.h"
#include "AnvilPlayer/App/app_messages.h"
#include "AnvilPlayer/App/library_commands.h"
#include "AnvilPlayer/App/rect_util.h"
#include "AnvilPlayer/App/single_instance.h"
#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/web_ui_json.h"
#include "AnvilPlayer/App/webui_root.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace anvil::app {

namespace {

constexpr wchar_t kWebUiLibraryUrl[] = L"http://appassets.anvilplayer.local/index.html#/library";
// Timer id reused by Application for the Emby report relay retry.
constexpr UINT_PTR kApplicationTimer = 9001;

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, dpi, 96);
}

}  // namespace

LibraryWindow::LibraryWindow() = default;
LibraryWindow::~LibraryWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void LibraryWindow::SetPlaybackRequest(PlaybackRequest callback) {
    playbackRequest_ = std::move(callback);
}

void LibraryWindow::SetAllowInsecureCertificatesRequest(std::function<void(bool)> callback) {
    allowInsecureCertificatesRequest_ = std::move(callback);
}

void LibraryWindow::SetFocusPlayerRequest(std::function<void()> callback) {
    focusPlayerRequest_ = std::move(callback);
}

void LibraryWindow::SetEmbyPlaybackReportRelay(std::function<void(const std::wstring&)> callback) {
    embyPlaybackReportRelay_ = std::move(callback);
}

void LibraryWindow::SetQuitHandler(QuitHandler handler) {
    quitHandler_ = std::move(handler);
}

void LibraryWindow::SetTimerHandler(std::function<void()> callback) {
    timerHandler_ = std::move(callback);
}

bool LibraryWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    dpi_ = GetDpiForSystem();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &LibraryWindow::WindowProc;
    wc.lpszClassName = kLibraryWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    constexpr DWORD windowExStyle = 0;
    const auto adjustedWindowRect = [this, windowStyle, windowExStyle](const int clientWidth, const int clientHeight) {
        RECT rect = MakeRect(0, 0, clientWidth, clientHeight);
        if (!AdjustWindowRectExForDpi(&rect, windowStyle, FALSE, windowExStyle, dpi_)) {
            AdjustWindowRectEx(&rect, windowStyle, FALSE, windowExStyle);
        }
        return rect;
    };

    const int targetClientWidth = ScaleForDpi(1280, dpi_);
    const int targetClientHeight = ScaleForDpi(800, dpi_);
    RECT initialRect = adjustedWindowRect(targetClientWidth, targetClientHeight);
    RECT workArea{};
    int initialX = CW_USEDEFAULT;
    int initialY = CW_USEDEFAULT;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        RectWidth(workArea) > 0 &&
        RectHeight(workArea) > 0) {
        initialX = workArea.left + std::max(0, RectWidth(workArea) - RectWidth(initialRect)) / 2;
        initialY = workArea.top + std::max(0, RectHeight(workArea) - RectHeight(initialRect)) / 2;
    }

    hwnd_ = CreateWindowExW(
        windowExStyle,
        kLibraryWindowClassName,
        L"Anvil Player",
        windowStyle,
        initialX,
        initialY,
        RectWidth(initialRect),
        RectHeight(initialRect),
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        return false;
    }

    TryCreateWebUi();
    return true;
}

void LibraryWindow::Show(const int commandShow) const {
    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);
}

std::filesystem::path LibraryWindow::WebUiRoot() const {
    wchar_t modulePath[MAX_PATH]{};
    constexpr DWORD modulePathCount = static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0]));
    const DWORD length = GetModuleFileNameW(instance_, modulePath, modulePathCount);
    if (length > 0 && length < modulePathCount) {
        const auto root = FindBuiltWebUiRootNear(std::filesystem::path(modulePath).parent_path());
        if (!root.empty()) {
            return root;
        }
    }

    std::error_code error;
    const auto currentRoot = FindBuiltWebUiRootNear(std::filesystem::current_path(error));
    if (!error && !currentRoot.empty()) {
        return currentRoot;
    }

    if (length > 0 && length < modulePathCount) {
        return std::filesystem::path(modulePath).parent_path() / L"webui";
    }
    return std::filesystem::current_path(error) / L"webui";
}

bool LibraryWindow::TryCreateWebUi() {
    if (webUiActive_ || !hwnd_) {
        return false;
    }

    auto host = std::make_unique<WebUiHost>();
    const auto root = WebUiRoot();
    // Reuse the legacy base WebView2 folder (empty profile name) so the
    // library preserves existing user data (Emby connections, TMDB settings,
    // view state) saved before the window split. The player window uses a
    // separate "Player" subfolder.
    const bool started = host->Create(hwnd_, root, kWebUiLibraryUrl, L"",
                                      [this](const std::wstring& message) {
                                          HandleWebUiMessage(message);
                                      });
    if (!started) {
        return false;
    }

    webUiHost_ = std::move(host);
    webUiActive_ = true;
    return true;
}

void LibraryWindow::PostScanResult(const std::wstring& json) {
    if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
        webUiHost_->PostJson(json);
    }
}

void LibraryWindow::OpenLocalFolderDialog() {
    const auto postMessage = [this](const std::wstring& json) {
        PostScanResult(json);
    };

    BROWSEINFOW dialog{};
    dialog.hwndOwner = hwnd_;
    dialog.lpszTitle = L"选择本地媒体文件夹";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&dialog);
    if (!pidl) {
        postMessage(L"{\"type\":\"localFolderPickCancelled\"}");
        return;
    }

    wchar_t folderPath[MAX_PATH]{};
    const BOOL hasPath = SHGetPathFromIDListW(pidl, folderPath);
    CoTaskMemFree(pidl);

    if (!hasPath || folderPath[0] == L'\0') {
        postMessage(L"{\"type\":\"localFolderPickFailed\",\"message\":\"无法读取所选文件夹路径\"}");
        return;
    }

    const auto folder = NormalizeListPath(folderPath);
    std::error_code error;
    if (!std::filesystem::exists(folder, error) || !std::filesystem::is_directory(folder, error)) {
        postMessage(L"{\"type\":\"localFolderPickFailed\",\"message\":\"所选路径不是可读取的文件夹\"}");
        return;
    }

    postMessage(LocalFolderPickedJson(folder, {}, false, true));
    StartLocalFolderScanAsync(hwnd_, folder);
}

void LibraryWindow::OpenMediaFileDialog() {
    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Media Files\0*.mp4;*.mkv;*.mov;*.m2ts;*.ts;*.webm;*.avi\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog) && playbackRequest_) {
        playbackRequest_(std::filesystem::path(filePath.data()), 0.0);
    }
}

void LibraryWindow::ApplyWindowChrome() const {
    // Mirror MainWindow's dark chrome so the library window matches the
    // player's dark title bar / borders instead of the default white frame.
    const BOOL dark = TRUE;
    const int corner = kDwmCornerRound;
    const COLORREF border = RGB(38, 42, 50);
    const COLORREF caption = RGB(8, 10, 13);
    const COLORREF text = RGB(239, 241, 245);
    DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd_, kDwmCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, kDwmTextColor, &text, sizeof(text));
}

void LibraryWindow::HandleWebUiMessage(const std::wstring_view message) {
    if (MessageContains(message, L"\"type\":\"requestState\"")) {
        // The library route does not consume PlayerState; acknowledge with an
        // empty state so subscribeNativeState resolves without blocking.
        PostScanResult(L"{\"type\":\"state\",\"state\":{}}");
        return;
    }

    if (!MessageContains(message, L"\"type\":\"command\"")) {
        return;
    }

    if (MessageContains(message, L"\"command\":\"setAllowInsecureCertificates\"")) {
        const bool enabled = MessageContains(message, L"\"enabled\":true");
        if (webUiHost_) {
            webUiHost_->SetAllowInsecureCertificates(enabled);
        }
        if (allowInsecureCertificatesRequest_) {
            allowInsecureCertificatesRequest_(enabled);
        }
    } else if (MessageContains(message, L"\"command\":\"open\"")) {
        OpenMediaFileDialog();
    } else if (MessageContains(message, L"\"command\":\"pickLocalFolder\"")) {
        OpenLocalFolderDialog();
    } else if (MessageContains(message, L"\"command\":\"scanLocalFolder\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            const SmbCredentials credentials{
                ReadJsonString(message, L"username").value_or(L""),
                ReadJsonString(message, L"password").value_or(L""),
            };
            std::wstring pathText = *path;
            if (StartsWithInsensitive(pathText, L"smb://")) {
                pathText = NormalizeSmbPathText(pathText);
            }
            const auto folder = NormalizeListPath(pathText);
            if (const auto connectError = ConnectSmbPathForAccess(folder, credentials)) {
                PostScanResult(LocalFolderScanFailedJson(folder, *connectError));
                return;
            }
            std::error_code error;
            if (!std::filesystem::exists(folder, error) || !std::filesystem::is_directory(folder, error)) {
                PostScanResult(LocalFolderScanFailedJson(folder, L"所选路径不是可读取的文件夹"));
                return;
            }
            StartLocalFolderScanAsync(hwnd_, folder, credentials);
        }
    } else if (MessageContains(message, L"\"command\":\"listSmbDirectory\"")) {
        const auto requestId = ReadJsonString(message, L"requestId").value_or(L"");
        const auto host = ReadJsonString(message, L"host").value_or(L"");
        const auto path = ReadJsonString(message, L"path").value_or(L"");
        const SmbCredentials credentials{
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""),
        };
        StartSmbDirectoryListAsync(hwnd_, requestId, host, path, credentials);
    } else if (MessageContains(message, L"\"command\":\"connectSmbShare\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            const SmbCredentials credentials{
                ReadJsonString(message, L"username").value_or(L""),
                ReadJsonString(message, L"password").value_or(L""),
            };
            const auto normalizedPath = std::filesystem::path(StartsWithInsensitive(*path, L"smb://")
                ? NormalizeSmbPathText(*path)
                : *path);
            if (const auto connectError = ConnectSmbPathForAccess(normalizedPath, credentials)) {
                PostScanResult(L"{\"type\":\"debugLog\",\"message\":\"smb connect failed: " + *connectError + L"\"}");
            }
        }
    } else if (MessageContains(message, L"\"command\":\"listWebDavDirectory\"")) {
        const auto url = ReadJsonString(message, L"url").value_or(L"");
        StartWebDavDirectoryListAsync(
            hwnd_,
            ReadJsonString(message, L"requestId").value_or(L""),
            url,
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"scanWebDavFolder\"")) {
        const auto url = ReadJsonString(message, L"url").value_or(L"");
        StartWebDavScanAsync(
            hwnd_,
            url,
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"debugLog\"")) {
        if (const auto text = ReadJsonString(message, L"message")) {
            OutputDebugStringW((L"[library] " + *text + L"\n").c_str());
        }
    } else if (MessageContains(message, L"\"command\":\"requestPlayback\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            const double startRatio = ReadJsonNumber(message, L"startPositionRatio").value_or(0.0);
            if (playbackRequest_) {
                playbackRequest_(std::filesystem::path(*path), startRatio);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"focusPlayer\"")) {
        if (focusPlayerRequest_) {
            focusPlayerRequest_();
        }
    } else if (MessageContains(message, L"\"command\":\"deliverEmbyPlaybackReport\"")) {
        // Relay the full report object to the player window via Application so
        // the player's WebView (separate storage) can inject it.
        if (const auto reportJson = ReadJsonObject(message, L"report")) {
            if (embyPlaybackReportRelay_) {
                embyPlaybackReportRelay_(*reportJson);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"setWebUiRoute\"")) {
        // Legacy single-window route notification; a standalone library window
        // is always on the library route, so this is a no-op.
    }
}

LRESULT CALLBACK LibraryWindow::WindowProc(HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    LibraryWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<LibraryWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT LibraryWindow::HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        ApplyWindowChrome();
        DragAcceptFiles(hwnd_, TRUE);
        return 0;
    case WM_COPYDATA: {
        // Single-instance command-line forwarding from a second launch. Parse
        // the forwarded command line the same way the entry point does and, if
        // it carries a media path, hand it to the player via the playback
        // request callback. WM_COPYDATA is the only message Windows marshals
        // across the process boundary.
        auto* copyData = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (copyData && copyData->dwData == kForwardCommandLineMagic &&
            copyData->lpData != nullptr && copyData->cbData > 0 &&
            (copyData->cbData % sizeof(wchar_t)) == 0) {
            const std::size_t charCount = copyData->cbData / sizeof(wchar_t);
            std::wstring forwarded(static_cast<const wchar_t*>(copyData->lpData), charCount);
            const auto args = ParseCommandLine(forwarded);
            if (!args.mediaPath.empty() && playbackRequest_) {
                playbackRequest_(args.mediaPath, 0.0);
            }
        }
        return TRUE;
    }
    case WM_SIZE:
        if (webUiHost_) {
            RECT client{};
            GetClientRect(hwnd_, &client);
            webUiHost_->Resize(client);
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t filePath[MAX_PATH]{};
        const UINT queried = DragQueryFileW(drop, 0, filePath, static_cast<UINT>(std::size(filePath)));
        DragFinish(drop);
        if (queried > 0 && playbackRequest_) {
            playbackRequest_(std::filesystem::path(filePath), 0.0);
        }
        return 0;
    }
    case kLocalFolderScanResultMessage: {
        std::unique_ptr<std::wstring> json(reinterpret_cast<std::wstring*>(lParam));
        if (json) {
            PostScanResult(*json);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = ScaleForDpi(640, dpi_);
        info->ptMinTrackSize.y = ScaleForDpi(420, dpi_);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kApplicationTimer) {
            KillTimer(hwnd_, kApplicationTimer);
            if (timerHandler_) {
                timerHandler_();
            }
        }
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        if (quitHandler_) {
            quitHandler_();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace anvil::app
