#include "AnvilPlayer/App/main_window.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace anvil::app {

using anvil::playback::CapabilityReport;
using anvil::playback::FormatTimecode;
using anvil::playback::LogLevel;
using anvil::playback::PlaybackSessionSnapshot;
using anvil::playback::PlaybackState;
using anvil::playback::ToDisplayString;

namespace {

constexpr auto kSidebarAnimationDuration = std::chrono::milliseconds{340};
constexpr auto kProgressHoverAnimationDuration = std::chrono::milliseconds{140};
constexpr auto kFullscreenTransportAnimationDuration = std::chrono::milliseconds{180};
constexpr auto kFullscreenTransportHideDelay = std::chrono::seconds{5};
constexpr int kFullscreenTransportActivationHeight = 110;

double EaseOutCubic(const double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    const double inverse = 1.0 - clamped;
    return 1.0 - inverse * inverse * inverse;
}

double AnimatedValue(const double from,
                     const double to,
                     const std::chrono::steady_clock::time_point startedAt,
                     const std::chrono::milliseconds duration,
                     const std::chrono::steady_clock::time_point now,
                     bool& complete) {
    if (duration.count() <= 0 || startedAt.time_since_epoch().count() == 0) {
        complete = true;
        return to;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt);
    const double t = std::clamp(static_cast<double>(elapsed.count()) / static_cast<double>(duration.count()), 0.0, 1.0);
    complete = t >= 1.0;
    return from + (to - from) * EaseOutCubic(t);
}

std::wstring FormatMillisecondsFromMicroseconds(const uint64_t microseconds) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << (static_cast<double>(microseconds) / 1000.0);
    return stream.str();
}

std::wstring FormatAverageMilliseconds(const uint64_t totalMicroseconds, const uint64_t count) {
    if (count == 0) {
        return L"0.00";
    }
    return FormatMillisecondsFromMicroseconds(totalMicroseconds / count);
}

bool WantsDolbyVisionHdrOutput(const anvil::playback::VideoSettings& settings,
                               const anvil::playback::DisplayCapabilities& display) {
    (void)display;
    return settings.dolbyVisionHdrOutput;
}

bool ExperimentalDoviTrimEnabled() {
    wchar_t value[16]{};
    constexpr DWORD kValueCount = static_cast<DWORD>(sizeof(value) / sizeof(value[0]));
    const DWORD length = GetEnvironmentVariableW(L"ANVIL_EXPERIMENTAL_DOVI_TRIM", value, kValueCount);
    if (length == 0 || length >= kValueCount) {
        return false;
    }
    return value[0] == L'1' || value[0] == L't' || value[0] == L'T' ||
           value[0] == L'y' || value[0] == L'Y' || value[0] == L'o' ||
           value[0] == L'O';
}

bool WantsCmv4Approx(const anvil::playback::VideoSettings& settings) {
    return settings.dolbyVisionCmv4Approx || ExperimentalDoviTrimEnabled();
}

std::wstring LowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::filesystem::path NormalizeListPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

bool ContainsInsensitive(const std::wstring& value, const std::wstring& needle) {
    return LowerCopy(value).find(LowerCopy(needle)) != std::wstring::npos;
}

bool IsMediaFilePath(const std::filesystem::path& path) {
    const std::wstring extension = LowerCopy(path.extension().wstring());
    return extension == L".mp4" ||
           extension == L".mkv" ||
           extension == L".mov" ||
           extension == L".m4v" ||
           extension == L".m2ts" ||
           extension == L".ts" ||
           extension == L".webm" ||
           extension == L".avi" ||
           extension == L".wmv" ||
           extension == L".mpg" ||
           extension == L".mpeg";
}

std::vector<std::filesystem::path> MediaFilesInFolder(const std::filesystem::path& mediaPath) {
    std::vector<std::filesystem::path> entries;
    const auto folder = mediaPath.parent_path();
    if (folder.empty()) {
        return entries;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        std::error_code entryError;
        if (iterator->is_regular_file(entryError)) {
            const auto entryPath = NormalizeListPath(iterator->path());
            if (IsMediaFilePath(entryPath)) {
                entries.push_back(entryPath);
            }
        }
        iterator.increment(error);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return LowerCopy(lhs.filename().wstring()) < LowerCopy(rhs.filename().wstring());
    });
    return entries;
}

bool HdrFormatLooksHdr(const std::wstring& value) {
    return ContainsInsensitive(value, L"HDR") ||
           ContainsInsensitive(value, L"HLG") ||
           ContainsInsensitive(value, L"PQ") ||
           ContainsInsensitive(value, L"Dolby Vision");
}

bool MediaIsDolbyVision(const std::optional<anvil::playback::MediaDescriptor>& media) {
    return media.has_value() && media->hasVideo && media->dolbyVisionDetected;
}

bool MediaHasDolbyVisionEnhancementStream(const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (!MediaIsDolbyVision(media)) {
        return false;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind == L"Video" &&
            (ContainsInsensitive(stream.details, L"EL-only") ||
             ContainsInsensitive(stream.details, L"BL+EL") ||
             ContainsInsensitive(stream.details, L"Dolby Vision P7"))) {
            return true;
        }
    }
    return ContainsInsensitive(media->hdrFormat, L"Dolby Vision Profile 7");
}

bool MediaHasHdrSignal(const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (!media.has_value() || !media->hasVideo) {
        return false;
    }
    return media->dolbyVisionDetected ||
           media->videoColor.IsHdr() ||
           HdrFormatLooksHdr(media->hdrFormat);
}

}  // namespace

void MainWindow::ConfigureLogging(const LogLevel minimumLevel) {
    auto sink = controller_.LogSink();
    sink->SetMinimumLevel(minimumLevel);
    sink->SetFilePath(DefaultLogPath());
#if defined(_DEBUG)
    sink->EnableDebuggerOutput(true);
#else
    sink->EnableDebuggerOutput(false);
#endif
    nativeVideoDecoder_.emplace(sink);
    d3dRenderer_.emplace(sink);
    d3dRenderer_->SetDiagnosticsEnabled(minimumLevel == LogLevel::Debug);
    audioPlayer_.SetLogSink(sink);
    LogApp(LogLevel::Info, L"logging configured level=" + ToDisplayString(minimumLevel) + L" file=" + sink->FilePath().wstring());
    LogApp(LogLevel::Debug, L"debug logging enabled");
}

void MainWindow::SetBackend(const PlaybackBackend backend) {
    backend_ = backend;
    std::wstring name;
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: name = L"native_ffmpeg_d3d11"; break;
    case PlaybackBackend::EmbeddedFfplay: name = L"embedded_ffplay"; break;
    case PlaybackBackend::RawFrameBridge: name = L"raw_frame_bridge"; break;
    }
    LogApp(LogLevel::Info, L"playback backend=" + name);
}

void MainWindow::SetInitialVideoTrackSelection(const int selectedTrackIndex) {
    auto settings = controller_.Settings();
    settings.video.selectedTrackIndex = selectedTrackIndex;
    controller_.ApplySettings(settings);

    std::wstring label = L"auto";
    if (selectedTrackIndex == anvil::playback::kVideoTrackDolbyVisionEnhancement) {
        label = L"dolby_vision_enhancement";
    } else if (selectedTrackIndex >= 0) {
        label = L"stream=" + std::to_wstring(selectedTrackIndex);
    }
    LogApp(LogLevel::Info, L"initial video track=" + label);
}

bool MainWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    dpi_ = GetDpiForSystem();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &MainWindow::WindowProc;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc)) {
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

    const int targetClientWidth = Scale(1260);
    const int targetClientHeight = Scale(760);
    RECT initialRect = adjustedWindowRect(targetClientWidth, targetClientHeight);
    RECT workArea{};
    int initialX = CW_USEDEFAULT;
    int initialY = CW_USEDEFAULT;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        RectWidth(workArea) > 0 &&
        RectHeight(workArea) > 0) {
        const int maxWindowWidth = std::max(Scale(720), RectWidth(workArea) * 92 / 100);
        const int maxWindowHeight = std::max(Scale(500), RectHeight(workArea) * 92 / 100);
        const int windowWidth = RectWidth(initialRect);
        const int windowHeight = RectHeight(initialRect);
        if (windowWidth > maxWindowWidth || windowHeight > maxWindowHeight) {
            const double fit = std::min(static_cast<double>(maxWindowWidth) / std::max(1, windowWidth),
                                        static_cast<double>(maxWindowHeight) / std::max(1, windowHeight));
            const int fittedClientWidth = std::max(Scale(720), static_cast<int>(std::round(targetClientWidth * fit)));
            const int fittedClientHeight = std::max(Scale(500), static_cast<int>(std::round(targetClientHeight * fit)));
            initialRect = adjustedWindowRect(fittedClientWidth, fittedClientHeight);
        }
        initialX = workArea.left + std::max(0, RectWidth(workArea) - RectWidth(initialRect)) / 2;
        initialY = workArea.top + std::max(0, RectHeight(workArea) - RectHeight(initialRect)) / 2;
    }

    hwnd_ = CreateWindowExW(
        windowExStyle,
        kWindowClassName,
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

    return hwnd_ != nullptr;
}

void MainWindow::Show(const int commandShow) const {
    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);
}

void MainWindow::OpenInitialPath(const std::filesystem::path& path, const bool autoplay) {
    if (!path.empty()) {
        LogApp(LogLevel::Info, L"initial path=" + path.wstring() + L" autoplay=" + (autoplay ? L"true" : L"false"));
        OpenPath(path, autoplay);
    }
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::VideoHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::FullscreenOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }
    if (window && message == WM_MOUSEWHEEL) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }

    if (window && message == WM_PAINT) {
        window->PaintFullscreenOverlay(hwnd);
        return 0;
    }

    if (window && message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window->hwnd_, &point);
        SetCursor(LoadCursorW(nullptr, window->IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::TransportOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        return SendMessageW(window->hwnd_, message, wParam, MAKELPARAM(point.x, point.y));
    }
    if (window && message == WM_MOUSEWHEEL) {
        return SendMessageW(window->hwnd_, message, wParam, lParam);
    }

    if (window && message == WM_PAINT) {
        window->PaintTransportOverlay(hwnd);
        return 0;
    }

    if (window && message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window->hwnd_, &point);
        SetCursor(LoadCursorW(nullptr, window->IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::HdrToneCurveWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hdrToneCurveWindow_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleHdrToneCurveWindowMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void CALLBACK MainWindow::PlaybackTimerQueueProc(PVOID context, BOOLEAN) {
    auto* window = static_cast<MainWindow*>(context);
    if (window && window->hwnd_) {
        PostMessageW(window->hwnd_, kPlaybackTimerTickMessage, 0, 0);
    }
}

LRESULT MainWindow::HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        ApplyWindowChrome();
        DragAcceptFiles(hwnd_, TRUE);
        SetPlaybackTimer(false);
        MarkLayoutDirty();
        EnsureLayout();
        return 0;
    case kVideoFrameReadyMessage:
        videoDecoder_.AcknowledgeFrameNotification();
        if (backend_ == PlaybackBackend::RawFrameBridge) {
            controller_.UpdateClock();
            const auto snapshot = controller_.Snapshot();
            RenderPlaybackTick(snapshot, false);
            InvalidateVideoSurface();
        }
        return 0;
    case kNativeVideoFrameReadyMessage: {
        if (nativeVideoDecoder_) {
            nativeVideoDecoder_->AcknowledgeFrameNotification();
            NativeVideoFrame frame;
            const auto snapshot = controller_.Snapshot();
            const bool shouldRenderFrame = snapshot.state == PlaybackState::Playing || pendingPausedFrameRefresh_;
            if (shouldRenderFrame && nativeVideoDecoder_->LatestFrame(frame) && d3dRenderer_) {
                d3dRenderer_->Render(frame);
                heldNativeFrame_ = frame;
                heldNativeFrameNeedsPresent_ = false;
                nativeFrameHoldVisible_ = false;
                MaybeLogNativeSchedulerStats(nativeVideoDecoder_->Stats());
                if (pendingPausedFrameRefresh_) {
                    pendingPausedFrameRefresh_ = false;
                    nativeFrameHoldVisible_ = true;
                    MarkLayoutDirty();
                    EnsureLayout();
                    RenderHeldNativeFrame();
                    LogApp(LogLevel::Debug, L"paused native frame refresh completed");
                }
            }
            if (snapshot.state == PlaybackState::Playing) {
                RenderPlaybackTick(snapshot, false);
            }
        }
        return 0;
    }
    case WM_DPICHANGED:
        dpi_ = HIWORD(wParam);
        if (auto* suggested = reinterpret_cast<RECT*>(lParam)) {
            SetWindowPos(
                hwnd_,
                nullptr,
                suggested->left,
                suggested->top,
                RectWidth(*suggested),
                RectHeight(*suggested),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        MarkLayoutDirty();
        EnsureLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(720);
        info->ptMinTrackSize.y = Scale(500);
        return 0;
    }
    case WM_SIZE:
        MarkLayoutDirty();
        EnsureLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        hoveredButton_ = -1;
        hoveredInspectorPathItem_ = -1;
        hoveredHdrToneCurvePoint_ = -1;
        volumeSliderHovered_ = false;
        SetProgressHover(false);
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            SetCursor(LoadCursorW(nullptr, IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        OnLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), point);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != hwnd_) {
            CancelProgressDrag();
            CancelVolumeDrag();
            CancelHdrToneCurveInteraction();
            CancelSettingsScrollDrag();
            CancelVideoPress();
        }
        return 0;
    case WM_CANCELMODE:
        CancelProgressDrag();
        CancelVolumeDrag();
        CancelHdrToneCurveInteraction();
        CancelSettingsScrollDrag();
        CancelVideoPress();
        return 0;
    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;
    case WM_DROPFILES:
        OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kPlaybackTimerTickMessage:
        OnPlaybackTimerTick();
        return 0;
    case WM_TIMER:
        if (wParam == kUiAnimationTimer) {
            UpdateUiAnimations();
            return 0;
        }
        if (wParam == kFullscreenChromeHideTimer) {
            HideFullscreenTransportIfIdle();
            return 0;
        }
        if (wParam == kVideoPressTimer) {
            CompleteVideoLongPress();
            return 0;
        }
        if (wParam == kPlaybackTimer) {
            OnPlaybackTimerTick();
            return 0;
        }
        break;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_DESTROY:
        SetPlaybackTimer(false);
        KillTimer(hwnd_, kPlaybackTimer);
        KillTimer(hwnd_, kUiAnimationTimer);
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        KillTimer(hwnd_, kVideoPressTimer);
        DragAcceptFiles(hwnd_, FALSE);
        SetTemporaryPlaybackRate(1.0);
        if (hdrToneCurveWindow_) {
            DestroyWindow(hdrToneCurveWindow_);
            hdrToneCurveWindow_ = nullptr;
        }
        StopRuntime();
        ClearPreviewBitmap();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT MainWindow::HandleHdrToneCurveWindowMessage(HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(460);
        info->ptMinTrackSize.y = Scale(340);
        return 0;
    }
    case WM_SIZE:
        UpdateHdrToneCurveFloatingLayout();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (selectingHdrToneCurveRange_) {
            UpdateHdrToneCurveRangeSelection(point);
            return 0;
        }
        if (draggingHdrToneCurve_) {
            hdrToneCurveDragMoved_ = true;
            UpdateHdrToneCurveDrag(point);
            return 0;
        }
        if (!hdrToneCurveWindowTrackingMouseLeave_) {
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = window;
            TrackMouseEvent(&event);
            hdrToneCurveWindowTrackingMouseLeave_ = true;
        }
        UpdateHdrToneCurveHover(point);
        return 0;
    }
    case WM_MOUSELEAVE:
        hdrToneCurveWindowTrackingMouseLeave_ = false;
        hoveredHdrToneCurvePoint_ = -1;
        InvalidateHdrToneCurveEditor();
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (ContainsPoint(hdrToneCurveFloatingReset_, point)) {
            ResetHdrToneCurve();
            return 0;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 && BeginHdrToneCurveRangeSelection(point)) {
            return 0;
        }
        if (BeginHdrToneCurveDrag(point)) {
            return 0;
        }
        if (!ContainsPoint(hdrToneCurvePlot_, point)) {
            ClearHdrToneCurveSelection();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (selectingHdrToneCurveRange_) {
            UpdateHdrToneCurveRangeSelection(point);
            EndHdrToneCurveRangeSelection();
            return 0;
        }
        if (draggingHdrToneCurve_) {
            if (hdrToneCurveDragMoved_) {
                UpdateHdrToneCurveDrag(point);
            }
            EndHdrToneCurveDrag();
            return 0;
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window) {
            CancelHdrToneCurveInteraction();
        }
        return 0;
    case WM_CANCELMODE:
        CancelHdrToneCurveInteraction();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_UP) {
            NudgeHdrToneCurveSelection(1.0);
            return 0;
        }
        if (wParam == VK_DOWN) {
            NudgeHdrToneCurveSelection(-1.0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideHdrToneCurveWindow();
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            RECT hitRect = hdrToneCurvePlot_;
            InflateRect(&hitRect, Scale(18), Scale(18));
            SetCursor(LoadCursorW(nullptr,
                                  ContainsPoint(hitRect, point) ||
                                          ContainsPoint(hdrToneCurveFloatingReset_, point)
                                      ? IDC_HAND
                                      : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_PAINT:
        PaintHdrToneCurveWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        HideHdrToneCurveWindow();
        return 0;
    case WM_DESTROY:
        if (window == hdrToneCurveWindow_) {
            hdrToneCurveWindow_ = nullptr;
            hdrToneCurveExpanded_ = false;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::filesystem::path MainWindow::DefaultLogPath() {
    return std::filesystem::temp_directory_path() / L"anvil-player" / L"anvil-player.log";
}

void MainWindow::LogApp(const LogLevel level, const std::wstring& message) const {
    if (const auto sink = controller_.LogSink()) {
        sink->Write(level, L"app", message);
    }
}

void MainWindow::LogRuntime(const LogLevel level, const std::wstring& category, const std::wstring& message) const {
    if (const auto sink = controller_.LogSink()) {
        sink->Write(level, category, message);
    }
}

void MainWindow::MaybeLogNativeSchedulerStats(const NativeVideoQueueStats& stats) {
    const auto now = std::chrono::steady_clock::now();
    if (lastNativeStatsLog_.time_since_epoch().count() != 0 &&
        now - lastNativeStatsLog_ < std::chrono::seconds{2}) {
        return;
    }
    lastNativeStatsLog_ = now;

    const std::wstring master = stats.usingAudioClock ? L"audio" : L"wall";
    const std::wstring scheduler = stats.usingAudioClock ? L"audio_clock" : L"wall_clock";
    LogRuntime(LogLevel::Debug,
               L"clock",
               L"master=" + master +
                   L" position=" + FormatTimecode(stats.clockPosition) +
                   L" drift_ms=" + std::to_wstring(stats.driftMs));
    LogRuntime(LogLevel::Debug,
               L"video",
                L"decoder=" + stats.decoder +
                    L" scheduler=" + scheduler +
                    L" queue_depth=" + std::to_wstring(stats.queueDepth) +
                    L" packet_depth=" + std::to_wstring(stats.packetQueueDepth) +
                    L" packet_mb=" + std::to_wstring(stats.packetQueueBytes / (1024 * 1024)) +
                    L" buffered_end=" + FormatTimecode(stats.bufferedEnd) +
                    L" buffered_ms=" + std::to_wstring(stats.bufferedDuration.count()) +
                    L" read_ahead_ms=" + std::to_wstring(stats.readAheadDuration.count()) +
                    L" rendered=" + std::to_wstring(stats.rendered) +
                   L" hardware_frames=" + std::to_wstring(stats.hardwareFrames) +
                   L" zero_copy_frames=" + std::to_wstring(stats.zeroCopyFrames) +
                   L" cpu_transfer_frames=" + std::to_wstring(stats.cpuTransferFrames) +
                   L" dropped_late=" + std::to_wstring(stats.droppedLate) +
                   L" dropped_queue_full=" + std::to_wstring(stats.droppedQueueFull) +
                   (stats.fallbackReason.empty() ? L"" : L" fallback_reason=" + stats.fallbackReason));

    if (!d3dRenderer_) {
        return;
    }

    const D3D11RenderStats renderStats = d3dRenderer_->TakeRenderStats();
    if (renderStats.frames == 0) {
        return;
    }

    std::wstring renderMessage =
        L"frames=" + std::to_wstring(renderStats.frames) +
        L" hw_frames=" + std::to_wstring(renderStats.hardwareFrames) +
        L" bgra_frames=" + std::to_wstring(renderStats.bgraFrames) +
        L" avg_ms=" + FormatAverageMilliseconds(renderStats.totalRenderUs, renderStats.frames) +
        L" max_ms=" + FormatMillisecondsFromMicroseconds(renderStats.maxRenderUs) +
        L" present_avg_ms=" + FormatAverageMilliseconds(renderStats.presentUs, renderStats.frames) +
        L" present_max_ms=" + FormatMillisecondsFromMicroseconds(renderStats.maxPresentUs) +
        L" slow_frames=" + std::to_wstring(renderStats.slowFrames);

    if (renderStats.hardwareFrames > 0) {
        renderMessage +=
            L" hw_prepare_avg_ms=" + FormatAverageMilliseconds(renderStats.hardwarePrepareUs, renderStats.hardwareFrames) +
            L" srv_cache_hits=" + std::to_wstring(renderStats.hardwareSrvCacheHits) +
            L" srv_cache_misses=" + std::to_wstring(renderStats.hardwareSrvCacheMisses);
    }
    if (renderStats.bgraFrames > 0) {
        renderMessage +=
            L" bgra_upload_avg_ms=" + FormatAverageMilliseconds(renderStats.bgraUploadUs, renderStats.bgraFrames);
    }
    if (renderStats.subtitleFrames > 0 || renderStats.subtitleSurfaceRebuilds > 0) {
        const uint64_t subtitleCount = std::max<uint64_t>(1, renderStats.subtitleFrames);
        renderMessage +=
            L" subtitle_avg_ms=" + FormatAverageMilliseconds(renderStats.subtitleUs, subtitleCount) +
            L" subtitle_frames=" + std::to_wstring(renderStats.subtitleFrames) +
            L" subtitle_rebuilds=" + std::to_wstring(renderStats.subtitleSurfaceRebuilds);
    }

    LogRuntime(LogLevel::Debug, L"renderer", renderMessage);
}

void MainWindow::StartUiAnimationTimer() const {
    if (hwnd_) {
        SetTimer(hwnd_, kUiAnimationTimer, kUiAnimationTimerMs, nullptr);
    }
}

void MainWindow::UpdateUiAnimations() {
    const auto now = std::chrono::steady_clock::now();
    bool sidebarComplete = true;
    bool hoverComplete = true;
    bool fullscreenTransportComplete = true;
    bool subtitleMenuComplete = true;

    inspectorCollapseAmount_ = AnimatedValue(inspectorCollapseStartAmount_,
                                             inspectorCollapseTarget_,
                                             inspectorAnimationStartedAt_,
                                             kSidebarAnimationDuration,
                                             now,
                                             sidebarComplete);
    progressHoverAmount_ = AnimatedValue(progressHoverStartAmount_,
                                         progressHoverTarget_,
                                         progressHoverAnimationStartedAt_,
                                         kProgressHoverAnimationDuration,
                                         now,
                                         hoverComplete);
    fullscreenTransportAmount_ = AnimatedValue(fullscreenTransportStartAmount_,
                                               fullscreenTransportTarget_,
                                               fullscreenTransportAnimationStartedAt_,
                                               kFullscreenTransportAnimationDuration,
                                               now,
                                               fullscreenTransportComplete);
    subtitleMenuAmount_ = subtitleMenuTarget_;
    subtitleMenuComplete = true;

    if (sidebarComplete) {
        inspectorCollapseAmount_ = inspectorCollapseTarget_;
    }
    if (hoverComplete) {
        progressHoverAmount_ = progressHoverTarget_;
    }
    if (fullscreenTransportComplete) {
        fullscreenTransportAmount_ = fullscreenTransportTarget_;
    }
    if (subtitleMenuComplete) {
        subtitleMenuAmount_ = subtitleMenuTarget_;
        if (subtitleMenuTarget_ <= 0.0) {
            subtitleMenuOpen_ = false;
            hoveredSubtitleMenuItem_ = -1;
        }
    }

    MarkLayoutDirty();
    EnsureLayout();
    if (fullscreenOverlay_ && IsWindowVisible(fullscreenOverlay_)) {
        InvalidateRect(fullscreenOverlay_, nullptr, FALSE);
    } else if (transportOverlay_ && IsWindowVisible(transportOverlay_)) {
        InvalidateRect(transportOverlay_, nullptr, FALSE);
    }
    if (!fullscreen_ && (!transportOverlay_ || !IsWindowVisible(transportOverlay_))) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (sidebarComplete && hoverComplete && fullscreenTransportComplete && subtitleMenuComplete) {
        KillTimer(hwnd_, kUiAnimationTimer);
    } else {
        StartUiAnimationTimer();
    }
}

void MainWindow::SetSubtitleMenuTarget(const bool visible) {
    subtitleMenuOpen_ = visible;
    subtitleMenuAmount_ = visible ? 1.0 : 0.0;
    subtitleMenuTarget_ = visible ? 1.0 : 0.0;
    if (!visible) {
        hoveredSubtitleMenuItem_ = -1;
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::HideSubtitleMenu() {
    if (subtitleMenuTarget_ <= 0.0 && subtitleMenuAmount_ <= 0.0) {
        return;
    }
    SetSubtitleMenuTarget(false);
}

bool MainWindow::IsPointInteractive(const POINT point) const {
    if (HitButton(point) >= 0) {
        return true;
    }
    if (HitInspectorPathItem(point) >= 0) {
        return true;
    }
    if (ContainsPoint(VolumeSliderHitRect(), point)) {
        return true;
    }
    if (IsPointInSubtitleMenu(point)) {
        return true;
    }
    if (settingsScrollMax_ > 0 && ContainsPoint(settingsScrollThumb_, point)) {
        return true;
    }
    if (IsHdrToneCurveVisible()) {
        RECT hitRect = hdrToneCurvePlot_;
        InflateRect(&hitRect, Scale(18), Scale(18));
        if (ContainsPoint(hitRect, point)) {
            return true;
        }
    }
    const auto snapshot = controller_.Snapshot();
    return snapshot.media.has_value() &&
           snapshot.media->duration.count() > 0 &&
           ContainsPoint(ProgressHitRect(), point);
}

void MainWindow::SetProgressHover(const bool hovered) {
    if (progressHovered_ == hovered && progressHoverTarget_ == (hovered ? 1.0 : 0.0)) {
        return;
    }

    progressHovered_ = hovered;
    progressHoverStartAmount_ = progressHoverAmount_;
    progressHoverTarget_ = hovered ? 1.0 : 0.0;
    progressHoverAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
}

void MainWindow::ToggleSidebar() {
    inspectorCollapsed_ = !inspectorCollapsed_;
    inspectorCollapseStartAmount_ = inspectorCollapseAmount_;
    inspectorCollapseTarget_ = inspectorCollapsed_ ? 1.0 : 0.0;
    inspectorAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::ShouldShowFullscreenTransport(const PlaybackSessionSnapshot&) const {
    if (!fullscreen_) {
        return true;
    }
    return draggingProgress_ ||
           draggingVolume_ ||
           fullscreenTransportTarget_ > 0.0 ||
           fullscreenTransportAmount_ > 0.01;
}

bool MainWindow::IsFullscreenTransportActivationPoint(const POINT point) const {
    if (!fullscreen_) {
        return false;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!ContainsPoint(client, point)) {
        return false;
    }

    return point.y >= client.bottom - Scale(kFullscreenTransportActivationHeight);
}

void MainWindow::UpdateFullscreenTransportCursorPolling() {
    if (!fullscreen_) {
        hasLastFullscreenCursorClient_ = false;
        return;
    }

    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(hwnd_, &point)) {
        return;
    }

    const bool moved = !hasLastFullscreenCursorClient_ ||
                       point.x != lastFullscreenCursorClient_.x ||
                       point.y != lastFullscreenCursorClient_.y;
    lastFullscreenCursorClient_ = point;
    hasLastFullscreenCursorClient_ = true;

    if (moved &&
        !ShouldShowFullscreenTransport(controller_.Snapshot()) &&
        IsFullscreenTransportActivationPoint(point)) {
        ShowFullscreenTransport();
    }
}

void MainWindow::ShowFullscreenTransport() {
    if (!fullscreen_) {
        return;
    }

    SetFullscreenTransportTarget(true);
    fullscreenTransportLastShownAt_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kFullscreenChromeHideTimer, 120, nullptr);
}

void MainWindow::HideFullscreenTransportIfIdle() {
    if (!fullscreen_) {
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        return;
    }

    UpdateFullscreenTransportCursorPolling();

    if (draggingProgress_ || draggingVolume_) {
        SetFullscreenTransportTarget(true);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (fullscreenTransportLastShownAt_.time_since_epoch().count() != 0 &&
        now - fullscreenTransportLastShownAt_ < kFullscreenTransportHideDelay) {
        return;
    }

    SetFullscreenTransportTarget(false);
    hoveredButton_ = -1;
    SetProgressHover(false);
}

void MainWindow::SetFullscreenTransportTarget(const bool visible) {
    fullscreenTransportVisible_ = visible;
    const double target = visible ? 1.0 : 0.0;
    if (std::abs(fullscreenTransportTarget_ - target) < 0.001) {
        return;
    }

    fullscreenTransportStartAmount_ = fullscreenTransportAmount_;
    fullscreenTransportTarget_ = target;
    fullscreenTransportAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetTemporaryPlaybackRate(const double rate) {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        audioPlayer_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
        return;
    }

    const double clamped = std::clamp(rate, 0.25, 4.0);
    if (std::abs(snapshot.playbackRate - clamped) < 0.001) {
        temporaryRateActive_ = clamped > 1.01;
        audioPlayer_.SetPlaybackRate(clamped);
        return;
    }

    controller_.SetPlaybackRate(clamped);
    audioPlayer_.SetPlaybackRate(clamped);
    temporaryRateActive_ = clamped > 1.01;
    InvalidateTransportArea();
}

std::wstring MainWindow::RuntimeLabel() const {
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: return kNativeFfmpegD3D11RuntimeLabel;
    case PlaybackBackend::EmbeddedFfplay: return kExternalPlaybackRuntimeLabel;
    case PlaybackBackend::RawFrameBridge: return kInternalPlaybackRuntimeLabel;
    }
    return kNativeFfmpegD3D11RuntimeLabel;
}

std::wstring MainWindow::RuntimeShortLabel() const {
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: return L"Native FFmpeg";
    case PlaybackBackend::EmbeddedFfplay: return L"External FFplay";
    case PlaybackBackend::RawFrameBridge: return L"Internal FFmpeg";
    }
    return L"Native FFmpeg";
}

void MainWindow::SetPlaybackTimer(const bool playing) {
    if (!hwnd_) {
        return;
    }

    if (playbackTimerQueueTimer_) {
        DeleteTimerQueueTimer(nullptr, playbackTimerQueueTimer_, INVALID_HANDLE_VALUE);
        playbackTimerQueueTimer_ = nullptr;
    }
    KillTimer(hwnd_, kPlaybackTimer);

    if (playing) {
        if (!CreateTimerQueueTimer(&playbackTimerQueueTimer_,
                                   nullptr,
                                   &MainWindow::PlaybackTimerQueueProc,
                                   this,
                                   kPlayingPlaybackTimerMs,
                                   kPlayingPlaybackTimerMs,
                                   WT_EXECUTEDEFAULT)) {
            playbackTimerQueueTimer_ = nullptr;
            SetTimer(hwnd_, kPlaybackTimer, kPlayingPlaybackTimerMs, nullptr);
        }
    } else {
        SetTimer(hwnd_, kPlaybackTimer, kIdlePlaybackTimerMs, nullptr);
    }
}

void MainWindow::OnPlaybackTimerTick() {
    controller_.UpdateClock();
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing) {
        SetTemporaryPlaybackRate(1.0);
        if (snapshot.state == PlaybackState::Paused &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning()) {
            MaybeLogNativeSchedulerStats(nativeVideoDecoder_->Stats());
            InvalidateRect(hwnd_, &transportBar_, FALSE);
            InvalidateFullscreenOverlay();
            return;
        }
        // Paused keeps the freeze frame captured by PausePlayback; other
        // stopped states clear the runtime frame as before.
        const bool keepFrame = (snapshot.state == PlaybackState::Paused);
        StopRuntime(keepFrame ? false : true);
        SetPlaybackTimer(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        RenderPlaybackTick(snapshot);
    }
}

void MainWindow::InvalidatePlaybackAreas() const {
    InvalidateRect(hwnd_, &videoSurface_, FALSE);
    InvalidateRect(hwnd_, &transportBar_, FALSE);
}

void MainWindow::InvalidateVideoSurface() const {
    InvalidateRect(hwnd_, &videoSurface_, FALSE);
}

void MainWindow::InvalidateTransportArea() const {
    if (transportOverlay_ && IsWindowVisible(transportOverlay_)) {
        InvalidateRect(transportOverlay_, nullptr, FALSE);
        return;
    }
    if (fullscreenOverlay_ && IsWindowVisible(fullscreenOverlay_)) {
        InvalidateFullscreenOverlay();
        return;
    }
    if (RectWidth(transportBar_) > 0 && RectHeight(transportBar_) > 0) {
        InvalidateRect(hwnd_, &transportBar_, FALSE);
    }
}

void MainWindow::InvalidateFullscreenOverlay() const {
    if (fullscreenOverlay_ && IsWindowVisible(fullscreenOverlay_)) {
        InvalidateRect(fullscreenOverlay_, nullptr, FALSE);
    }
}

void MainWindow::InvalidateHdrToneCurveEditor() const {
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (hdrToneCurveWindow_ && IsWindowVisible(hdrToneCurveWindow_)) {
        InvalidateRect(hdrToneCurveWindow_, nullptr, FALSE);
    }
}

const CapabilityReport& MainWindow::CachedCapabilities() {
    if (!capabilitiesCached_) {
        cachedCapabilities_ = controller_.CollectCapabilityReport();
        capabilitiesCached_ = true;
        LogApp(LogLevel::Debug, L"capability report cached");
    }
    return cachedCapabilities_;
}

void MainWindow::RefreshCapabilityCache() {
    cachedCapabilities_ = controller_.CollectCapabilityReport();
    capabilitiesCached_ = true;
    LogApp(LogLevel::Debug, L"capability report refreshed");
}

bool MainWindow::CurrentMediaHasHdrControls() const {
    return MediaHasHdrSignal(controller_.Snapshot().media);
}

bool MainWindow::CurrentMediaHasCmv4Control() const {
    return MediaIsDolbyVision(controller_.Snapshot().media);
}

bool MainWindow::CurrentCmv4ControlEnabled(const anvil::playback::PlayerSettings& settings) const {
    (void)settings;
    return CurrentMediaHasCmv4Control();
}

bool MainWindow::HdrToneCurveAvailable(const anvil::playback::PlayerSettings& settings) const {
    if (!CurrentMediaHasHdrControls() || !settings.video.dolbyVisionHdrOutput) {
        return false;
    }
    return !CurrentMediaHasCmv4Control() || !WantsCmv4Approx(settings.video);
}

bool MainWindow::Cmv4ApproxActiveForPlayback(const PlaybackSessionSnapshot& snapshot,
                                             const anvil::playback::VideoSettings& settings) const {
    return MediaIsDolbyVision(snapshot.media) &&
           WantsCmv4Approx(settings);
}

bool MainWindow::ApplyNativeColorSettingsLive(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !d3dRenderer_) {
        return false;
    }

    const auto settings = controller_.Settings();
    d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
    d3dRenderer_->ResetRenderStats();
    lastNativeStatsLog_ = {};

    if (snapshot.state == PlaybackState::Playing) {
        LogApp(LogLevel::Debug, L"native color settings queued live");
        return true;
    }

    NativeVideoFrame frame;
    const bool haveLatestFrame = nativeVideoDecoder_ &&
                                 nativeVideoDecoder_->LatestFrame(frame) &&
                                 frame.HasContent();
    if (haveLatestFrame) {
        d3dRenderer_->Render(frame);
        heldNativeFrame_ = frame;
        heldNativeFrameNeedsPresent_ = false;
        if (snapshot.state == PlaybackState::Paused) {
            nativeFrameHoldVisible_ = true;
            pendingPausedFrameRefresh_ = false;
        } else {
            nativeFrameHoldVisible_ = false;
        }
    } else if (heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
        RenderHeldNativeFrame();
        if (snapshot.state == PlaybackState::Paused) {
            nativeFrameHoldVisible_ = true;
            pendingPausedFrameRefresh_ = false;
        }
    }

    LogApp(LogLevel::Debug, L"native color settings applied live");
    return true;
}

bool MainWindow::NativeHdrOutputToggleRequiresDecoderRestart(const PlaybackSessionSnapshot& snapshot) const {
    (void)snapshot;
    // The native DV path keeps decoder-side libplacebo output stable as
    // BT.2020/PQ; the button only changes the renderer's final SDR/HDR output.
    return false;
}

bool MainWindow::NativeCmv4ToggleRequiresDecoderRestart(const PlaybackSessionSnapshot& snapshot) const {
    return backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
           snapshot.media.has_value() &&
           snapshot.media->hasVideo &&
           MediaHasDolbyVisionEnhancementStream(snapshot.media);
}

int MainWindow::SettingsVideoFieldCount() const {
    int count = 6;
    if (CurrentMediaHasHdrControls()) {
        ++count;
    }
    if (CurrentMediaHasCmv4Control()) {
        ++count;
    }
    return count;
}

void MainWindow::ApplyDefaultHdrControlsForCurrentMedia() {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        return;
    }

    auto settings = controller_.Settings();
    const bool windowsHdrEnabled = CachedCapabilities().display.hdrEnabled;
    const bool hdrControlVisible = MediaHasHdrSignal(snapshot.media);
    const bool defaultHdrOutput = hdrControlVisible && windowsHdrEnabled;
    bool changed = false;

    if (settings.video.dolbyVisionHdrOutput != defaultHdrOutput) {
        settings.video.dolbyVisionHdrOutput = defaultHdrOutput;
        changed = true;
    }
    if (settings.video.dolbyVisionCmv4Approx) {
        settings.video.dolbyVisionCmv4Approx = false;
        changed = true;
    }

    if (changed) {
        controller_.ApplySettings(settings);
    }
    if (!HdrToneCurveAvailable(settings)) {
        HideHdrToneCurveWindow();
    }

    LogApp(LogLevel::Info,
           L"media hdr controls hdr=" +
               std::wstring(hdrControlVisible ? (settings.video.dolbyVisionHdrOutput ? L"on" : L"off") : L"hidden") +
               L" cm4=" +
               std::wstring(MediaIsDolbyVision(snapshot.media) ? L"visible_off" : L"hidden") +
               L" curve=" +
               std::wstring(HdrToneCurveAvailable(settings) ? L"visible" : L"hidden") +
               L" windows_hdr=" +
               std::wstring(windowsHdrEnabled ? L"on" : L"off"));
}

void MainWindow::RenderPlaybackTick(const PlaybackSessionSnapshot& snapshot, const bool forceRefresh) {
    if (!hwnd_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh &&
        lastPlaybackUiRefreshAt_.time_since_epoch().count() != 0 &&
        now - lastPlaybackUiRefreshAt_ < std::chrono::milliseconds{kPlayingPlaybackTimerMs}) {
        return;
    }
    lastPlaybackUiRefreshAt_ = now;
    InvalidateTransportArea();
    if (!snapshot.media.has_value() || !snapshot.media->hasVideo) {
        InvalidateVideoSurface();
    }
}

int MainWindow::Scale(const int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

RECT MainWindow::PlaybackSurfaceBounds() const {
    if (RectWidth(videoSurface_) <= 0 || RectHeight(videoSurface_) <= 0) {
        return RECT{};
    }
    if (fullscreen_) {
        return videoSurface_;
    }
    return DeflateRectCopy(videoSurface_, Scale(2), Scale(2));
}

void MainWindow::ApplyWindowChrome() const {
    const BOOL dark = TRUE;
    const int corner = kDwmCornerRound;
    const COLORREF border = palette_.border;
    const COLORREF caption = palette_.background;
    const COLORREF text = palette_.text;
    DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd_, kDwmCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd_, kDwmTextColor, &text, sizeof(text));
}

void MainWindow::OpenFileDialog() {
    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Media Files\0*.mp4;*.mkv;*.mov;*.m2ts;*.ts;*.webm;*.avi\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog)) {
        OpenPath(filePath.data());
    }
}

void MainWindow::StopRuntime(const bool clearVideoFrame) {
    playbackPlayer_.Stop();
    videoDecoder_.Stop();
    if (nativeVideoDecoder_) {
        nativeVideoDecoder_->Stop();
    }
    audioPlayer_.Stop();
    if (clearVideoFrame) {
        videoDecoder_.ClearFrame();
        if (nativeVideoDecoder_) nativeVideoDecoder_->ClearFrame();
        heldNativeFrame_.reset();
        nativeFrameHoldVisible_ = false;
        pendingPausedFrameRefresh_ = false;
        heldNativeFrameNeedsPresent_ = false;
    } else if (backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
        heldNativeFrameNeedsPresent_ = heldNativeFrame_.has_value() &&
                                       heldNativeFrame_->HasPixels();
    }
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 && clearVideoFrame) {
        if (d3dRenderer_) d3dRenderer_->Clear();
        if (videoHost_) {
            ShowWindow(videoHost_, SW_HIDE);
        }
    }
}

bool MainWindow::CaptureLatestNativeFrame() {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 || !nativeVideoDecoder_) {
        return heldNativeFrame_.has_value();
    }

    NativeVideoFrame frame;
    if (nativeVideoDecoder_->LatestFrame(frame)) {
        heldNativeFrame_ = frame;
        return true;
    }
    return heldNativeFrame_.has_value();
}

void MainWindow::RenderHeldNativeFrame() {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 || !d3dRenderer_) {
        return;
    }

    if (heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
        LogApp(LogLevel::Debug,
               L"repainting cached native frame pixels=" +
                   std::wstring(heldNativeFrame_->HasPixels() ? L"true" : L"false") +
                   L" texture=" +
                   std::wstring(heldNativeFrame_->HasD3DTexture() ? L"true" : L"false"));
        d3dRenderer_->Render(*heldNativeFrame_);
    } else {
        LogApp(LogLevel::Debug, L"no cached native frame available to repaint");
    }
    heldNativeFrameNeedsPresent_ = false;
}

void MainWindow::StartRuntime(const PlaybackSessionSnapshot& snapshot, const bool restart) {
    if (!snapshot.media.has_value()) {
        return;
    }

    pendingPausedFrameRefresh_ = false;
    StopRuntime(false);

    if (backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        StartNativeRuntime(snapshot, restart);
        return;
    }

    if (backend_ == PlaybackBackend::EmbeddedFfplay) {
        const bool started = playbackPlayer_.Start(hwnd_,
                                                   PlaybackSurfaceBounds(),
                                                   snapshot.media->path,
                                                   snapshot.position,
                                                   snapshot.volume,
                                                   snapshot.media->dolbyVisionDetected);
        LogApp(started ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error,
               std::wstring(L"external ffplay playback ") + (restart ? L"restart=" : L"start=") + (started ? L"true" : L"false"));
        LogApp(LogLevel::Debug, L"ffplay command=" + playbackPlayer_.LastCommandLine());
        return;
    }

    // RawFrameBridge (--internal-playback): legacy ffmpeg raw-pipe path.
    bool videoStarted = true;
    bool audioStarted = true;
    if (snapshot.media->hasVideo) {
        videoStarted = videoDecoder_.Start(snapshot.media->path,
                                          snapshot.position,
                                          hwnd_,
                                          kVideoFrameReadyMessage);
    }
    if (snapshot.media->hasAudio) {
        audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
        audioStarted = audioPlayer_.Start(snapshot.media->path, snapshot.position, snapshot.volume);
    }

    const LogLevel level = videoStarted && audioStarted ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error;
    LogApp(level,
           std::wstring(L"internal viewport playback ") +
               (restart ? L"restart" : L"start") +
               L" video=" + (videoStarted ? L"true" : L"false") +
               L" audio=" + (audioStarted ? L"true" : L"false"));
    if (snapshot.media->hasVideo) {
        LogApp(LogLevel::Debug, L"ffmpeg video command=" + videoDecoder_.LastCommandLine());
    }
    if (snapshot.media->hasAudio) {
        LogApp(LogLevel::Debug, L"wasapi audio=" + audioPlayer_.LastStatus());
    }
}

void MainWindow::EnsureVideoHost() {
    if (videoHostReady_) {
        return;
    }
    if (!videoHost_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = &MainWindow::VideoHostProc;
        wc.lpszClassName = kVideoHostClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);

        const RECT hostBounds = PlaybackSurfaceBounds();
        const int hostWidth = std::max(1, RectWidth(hostBounds));
        const int hostHeight = std::max(1, RectHeight(hostBounds));
        videoHost_ = CreateWindowExW(
            0,
            kVideoHostClassName,
            L"",
            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            hostBounds.left,
            hostBounds.top,
            hostWidth,
            hostHeight,
            hwnd_,
            nullptr,
            instance_,
            this);

        if (!videoHost_) {
            LogApp(LogLevel::Error, L"failed to create native video host window");
            return;
        }
    }

    if (!d3dRenderer_) {
        LogApp(LogLevel::Error, L"native d3d renderer unavailable");
        return;
    }

    if (!d3dRenderer_->Initialize(videoHost_)) {
        LogApp(LogLevel::Error, L"native d3d renderer initialization failed");
        ShowWindow(videoHost_, SW_HIDE);
        return;
    }

    videoHostReady_ = true;
    UpdateVideoHost();
    LogApp(LogLevel::Info, L"native video host window created");
}

void MainWindow::StartNativeRuntime(const PlaybackSessionSnapshot& snapshot, const bool restart) {
    bool videoStarted = true;
    bool audioStarted = true;
    bool preferDolbyVisionHdrOutput = false;
    bool enableDolbyVisionEnhancementDecode = false;
    lastNativeStatsLog_ = {};
    if (snapshot.media->hasVideo) {
        if (d3dRenderer_) {
            const auto settings = controller_.Settings();
            d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
            d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
            d3dRenderer_->ResetRenderStats();
        }
        MarkLayoutDirty();
        EnsureLayout();
        EnsureVideoHost();
        const auto settings = controller_.Settings();
        preferDolbyVisionHdrOutput = snapshot.media->dolbyVisionDetected;
        const bool preferHardwareDecode = snapshot.media->selectedDecodePath == L"ffmpeg_d3d11va";
        enableDolbyVisionEnhancementDecode =
            MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
            settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
            Cmv4ApproxActiveForPlayback(snapshot, settings.video);
        videoStarted = videoHostReady_ && nativeVideoDecoder_ &&
                       nativeVideoDecoder_->Start(snapshot.media->path,
                                                  snapshot.position,
                                                  hwnd_,
                                                  kNativeVideoFrameReadyMessage,
                                                  [this]() {
                                                      if (const auto audioClock = audioPlayer_.PlaybackClock()) {
                                                          return audioClock;
                                                      }
                                                      const auto clockSnapshot = controller_.Snapshot();
                                                       if (clockSnapshot.state == PlaybackState::Playing) {
                                                           return std::optional<std::chrono::milliseconds>{clockSnapshot.position};
                                                       }
                                                       if (clockSnapshot.state == PlaybackState::Paused) {
                                                           return std::optional<std::chrono::milliseconds>{clockSnapshot.position};
                                                       }
                                                       return std::optional<std::chrono::milliseconds>{};
                                                   },
                                                  preferHardwareDecode,
                                                  d3dRenderer_ ? d3dRenderer_->Device() : nullptr,
                                                  settings.video.selectedTrackIndex,
                                                  settings.subtitles.preferredLanguage,
                                                  settings.subtitles.selectedTrackIndex,
                                                  std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                                  settings.subtitles.externalSubtitleAutoLoad,
                                                  false,
                                                  preferDolbyVisionHdrOutput,
                                                  enableDolbyVisionEnhancementDecode);
        if (videoStarted) {
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
        }
    }
    if (videoStarted && snapshot.media->hasVideo && snapshot.media->hasAudio && nativeVideoDecoder_) {
        nativeVideoDecoder_->WaitForPreroll(std::chrono::milliseconds{250},
                                            restart ? std::chrono::milliseconds{140}
                                                    : std::chrono::milliseconds{260});
    }
    if (snapshot.media->hasAudio) {
        audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
        audioStarted = audioPlayer_.Start(snapshot.media->path, snapshot.position, snapshot.volume);
    }

    const LogLevel level = videoStarted && audioStarted ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error;
    LogApp(level,
           std::wstring(L"native ffmpeg/d3d11 playback ") +
               (restart ? L"restart" : L"start") +
               L" video=" + (videoStarted ? L"true" : L"false") +
               L" audio=" + (audioStarted ? L"true" : L"false"));
    if (snapshot.media->hasVideo && videoStarted) {
        LogApp(LogLevel::Debug, L"native decode path=" + nativeVideoDecoder_->Path().wstring());
        if (snapshot.media->dolbyVisionDetected) {
            const auto runtimeSettings = controller_.Settings();
            const bool cmv4Intermediate = Cmv4ApproxActiveForPlayback(snapshot, runtimeSettings.video);
            const bool finalHdrOutput = WantsDolbyVisionHdrOutput(runtimeSettings.video, CachedCapabilities().display);
            LogApp(LogLevel::Info,
                   L"dolby vision libplacebo intermediate=hdr_bt2020_pq final=" +
                       std::wstring(finalHdrOutput ? L"hdr10" : L"sdr") +
                       (cmv4Intermediate ? L" cmv4=on" : L" cmv4=off") +
                       (enableDolbyVisionEnhancementDecode ? L" el_decode=on" : L" el_decode=off"));
        }
    }
    if (snapshot.media->hasAudio) {
        LogApp(LogLevel::Debug, L"wasapi audio=" + audioPlayer_.LastStatus());
    }
}

bool MainWindow::SeekNativeRuntime(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        (!snapshot.media->hasVideo && !snapshot.media->hasAudio)) {
        return false;
    }

    bool videoSeeked = true;
    if (snapshot.media->hasVideo) {
        videoSeeked = nativeVideoDecoder_ &&
                      nativeVideoDecoder_->IsRunning() &&
                      nativeVideoDecoder_->Seek(snapshot.position);
    }
    if (!videoSeeked) {
        return false;
    }

    bool audioSeeked = true;
    if (snapshot.media->hasAudio) {
        if (audioPlayer_.IsRunning()) {
            audioSeeked = audioPlayer_.Seek(snapshot.position);
        } else {
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            audioSeeked = audioPlayer_.Start(snapshot.media->path, snapshot.position, snapshot.volume);
        }
    }
    if (!audioSeeked) {
        return false;
    }

    pendingPausedFrameRefresh_ = false;
    nativeFrameHoldVisible_ = false;
    heldNativeFrameNeedsPresent_ = false;
    lastNativeStatsLog_ = {};
    if (d3dRenderer_) {
        d3dRenderer_->ResetRenderStats();
    }
    LogApp(LogLevel::Debug, L"native runtime seek position=" + FormatTimecode(snapshot.position));
    SetPlaybackTimer(true);
    return true;
}

void MainWindow::RefreshPausedNativeFrame(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !nativeVideoDecoder_) {
        return;
    }

    if (nativeVideoDecoder_->IsRunning()) {
        nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
        pendingPausedFrameRefresh_ = true;
        lastNativeStatsLog_ = {};
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        if (nativeVideoDecoder_->Seek(snapshot.position)) {
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            SetPlaybackTimer(false);
            LogApp(LogLevel::Debug, L"paused native runtime seek position=" + FormatTimecode(snapshot.position));
            return;
        }
    }

    StopRuntime(false);
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    pendingPausedFrameRefresh_ = true;
    lastNativeStatsLog_ = {};

    if (d3dRenderer_) {
        const auto settings = controller_.Settings();
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }
    MarkLayoutDirty();
    EnsureLayout();
    EnsureVideoHost();

    const auto settings = controller_.Settings();
    const bool preferDolbyVisionHdrOutput = snapshot.media->dolbyVisionDetected;
    const bool preferHardwareDecode = snapshot.media->selectedDecodePath == L"ffmpeg_d3d11va";
    const bool enableDolbyVisionEnhancementDecode =
        MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video);
    const bool started = videoHostReady_ &&
                         d3dRenderer_ &&
                         nativeVideoDecoder_->Start(snapshot.media->path,
                                                    snapshot.position,
                                                    hwnd_,
                                                    kNativeVideoFrameReadyMessage,
                                                    {},
                                                    preferHardwareDecode,
                                                    d3dRenderer_->Device(),
                                                    settings.video.selectedTrackIndex,
                                                    settings.subtitles.preferredLanguage,
                                                    settings.subtitles.selectedTrackIndex,
                                                    std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                                    settings.subtitles.externalSubtitleAutoLoad,
                                                    true,
                                                    preferDolbyVisionHdrOutput,
                                                    enableDolbyVisionEnhancementDecode);
    if (started) {
        nativeFrameHoldVisible_ = true;
        MarkLayoutDirty();
        EnsureLayout();
        UpdateVideoHost();
        LogApp(LogLevel::Debug, L"paused native frame refresh start position=" + FormatTimecode(snapshot.position));
    } else {
        pendingPausedFrameRefresh_ = false;
        LogApp(LogLevel::Warning, L"paused native frame refresh failed position=" + FormatTimecode(snapshot.position));
    }
}

void MainWindow::OpenPath(const std::filesystem::path& path, const bool autoplay) {
    LogApp(LogLevel::Info, L"open path=" + path.wstring());
    SetTemporaryPlaybackRate(1.0);
    StopRuntime();
    SetPlaybackTimer(false);
    RefreshCapabilityCache();
    const bool opened = controller_.OpenMedia(path, false);
    LogApp(opened ? LogLevel::Info : LogLevel::Error,
           std::wstring(L"open result=") + (opened ? L"true" : L"false") + L" autoplay=" + (autoplay ? L"true" : L"false"));
    if (opened) {
        UpdateInspectorMediaLists(path);
        ApplyDefaultHdrControlsForCurrentMedia();
        MarkLayoutDirty();
        EnsureLayout();
    }
    if (opened && autoplay) {
        StartPlayback();
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateInspectorMediaLists(const std::filesystem::path& path) {
    constexpr std::size_t kMaxRecentMedia = 12;
    const auto normalizedPath = NormalizeListPath(path);
    if (normalizedPath.empty()) {
        return;
    }

    recentMedia_.erase(std::remove(recentMedia_.begin(), recentMedia_.end(), normalizedPath), recentMedia_.end());
    recentMedia_.insert(recentMedia_.begin(), normalizedPath);
    if (recentMedia_.size() > kMaxRecentMedia) {
        recentMedia_.resize(kMaxRecentMedia);
    }

    currentFolderEntries_ = MediaFilesInFolder(normalizedPath);
    if (std::find(currentFolderEntries_.begin(), currentFolderEntries_.end(), normalizedPath) == currentFolderEntries_.end()) {
        currentFolderEntries_.insert(currentFolderEntries_.begin(), normalizedPath);
    }
}

void MainWindow::StartPlayback() {
    const auto before = controller_.Snapshot();
    controller_.Play();
    const auto snapshot = controller_.Snapshot();
    LogApp(LogLevel::Info, L"start playback state=" + ToDisplayString(snapshot.state) + L" hasMedia=" + (snapshot.media.has_value() ? L"true" : L"false"));
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        const bool resumedNativeRuntime =
            before.state == PlaybackState::Paused &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning();
        if (resumedNativeRuntime) {
            nativeVideoDecoder_->SetPaused(false, snapshot.position);
            pendingPausedFrameRefresh_ = false;
            nativeFrameHoldVisible_ = false;
            heldNativeFrameNeedsPresent_ = false;
            if (snapshot.media->hasAudio) {
                audioPlayer_.Stop();
                audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
                const bool audioStarted = audioPlayer_.Start(snapshot.media->path, snapshot.position, snapshot.volume);
                LogApp(audioStarted ? LogLevel::Debug : LogLevel::Warning,
                       L"native paused runtime resume audio=" + std::wstring(audioStarted ? L"true" : L"false"));
            }
        } else {
            StartRuntime(snapshot, false);
        }
        SetPlaybackTimer(true);
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::PausePlayback() {
    controller_.Pause();
    SetTemporaryPlaybackRate(1.0);
    const auto snapshot = controller_.Snapshot();
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_ &&
        nativeVideoDecoder_->IsRunning() &&
        d3dRenderer_) {
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        NativeVideoFrame frame;
        if (nativeVideoDecoder_->LatestFrame(frame)) {
            d3dRenderer_->Render(frame);
            heldNativeFrame_ = frame;
            nativeFrameHoldVisible_ = true;
            heldNativeFrameNeedsPresent_ = false;
            LogApp(LogLevel::Debug,
                   L"rendered native pause freeze frame pixels=" +
                        std::wstring(frame.HasPixels() ? L"true" : L"false"));
        }
        audioPlayer_.Stop();
    } else {
        StopRuntime(false);
    }
    SetPlaybackTimer(false);
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::TogglePlayback() {
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing) {
        PausePlayback();
    } else {
        StartPlayback();
    }
}

void MainWindow::StopPlayback() {
    controller_.Stop();
    SetTemporaryPlaybackRate(1.0);
    StopRuntime();
    SetPlaybackTimer(false);
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RestartPlaybackIfPlaying() {
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        StartRuntime(snapshot, true);
        SetPlaybackTimer(true);
    }
}

void MainWindow::SeekRelative(const std::chrono::milliseconds delta) {
    const auto before = controller_.Snapshot();
    controller_.SeekRelative(delta);
    const auto after = controller_.Snapshot();
    if (before.state == PlaybackState::Paused &&
        after.media.has_value() &&
        after.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(after);
    } else if (!(before.state == PlaybackState::Playing && SeekNativeRuntime(after))) {
        RestartPlaybackIfPlaying();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SeekToPosition(const std::chrono::milliseconds position) {
    const auto before = controller_.Snapshot();
    controller_.Seek(position);
    const auto after = controller_.Snapshot();
    if (before.state == PlaybackState::Paused &&
        after.media.has_value() &&
        after.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(after);
    } else if (!(before.state == PlaybackState::Playing && SeekNativeRuntime(after))) {
        RestartPlaybackIfPlaying();
    }
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SeekFromProgress(const int x) {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value() || RectWidth(progress_) <= 0) {
        return;
    }
    const double ratio = std::clamp(
        static_cast<double>(x - progress_.left) / static_cast<double>(RectWidth(progress_)),
        0.0,
        1.0);
    const auto duration = snapshot.media->duration;
    SeekToPosition(std::chrono::milliseconds(static_cast<long long>(duration.count() * ratio)));
}

void MainWindow::ToggleFullscreen() {
    if (!fullscreen_) {
        previousStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
        previousExStyle_ = GetWindowLongW(hwnd_, GWL_EXSTYLE);
        previousPlacement_.length = sizeof(previousPlacement_);
        GetWindowPlacement(hwnd_, &previousPlacement_);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        SetWindowLongW(hwnd_, GWL_STYLE, previousStyle_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, previousExStyle_ & ~WS_EX_WINDOWEDGE);
        SetWindowPos(hwnd_,
                     HWND_TOP,
                     monitorInfo.rcMonitor.left,
                     monitorInfo.rcMonitor.top,
                     RectWidth(monitorInfo.rcMonitor),
                     RectHeight(monitorInfo.rcMonitor),
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
        fullscreenTransportVisible_ = false;
        fullscreenTransportAmount_ = 0.0;
        fullscreenTransportStartAmount_ = fullscreenTransportAmount_;
        fullscreenTransportTarget_ = fullscreenTransportAmount_;
        fullscreenTransportLastShownAt_ = {};
        POINT cursor{};
        hasLastFullscreenCursorClient_ = GetCursorPos(&cursor) && ScreenToClient(hwnd_, &cursor);
        if (hasLastFullscreenCursorClient_) {
            lastFullscreenCursorClient_ = cursor;
        }
        SetTimer(hwnd_, kFullscreenChromeHideTimer, 120, nullptr);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, previousStyle_);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, previousExStyle_);
        SetWindowPlacement(hwnd_, &previousPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
        fullscreenTransportVisible_ = false;
        fullscreenTransportAmount_ = 0.0;
        fullscreenTransportStartAmount_ = 0.0;
        fullscreenTransportTarget_ = 0.0;
        hasLastFullscreenCursorClient_ = false;
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        if (fullscreenOverlay_) {
            ShowWindow(fullscreenOverlay_, SW_HIDE);
        }
    }
    ApplyWindowChrome();
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace anvil::app
