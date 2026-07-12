#include "AnvilPlayer/App/app_arguments.h"
#include "AnvilPlayer/App/library_commands.h"
#include "AnvilPlayer/App/main_window.h"
#include "AnvilPlayer/App/rect_util.h"
#include "AnvilPlayer/App/single_instance.h"
#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/window_chrome.h"
#include "AnvilPlayer/App/ui_animation_math.h"
#include "AnvilPlayer/App/web_ui_json.h"
#include "AnvilPlayer/App/webui_root.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <lm.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winnetwk.h>
#include <winhttp.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace anvil::app {

using anvil::playback::CapabilityReport;
using anvil::playback::FormatTimecode;
using anvil::playback::LogLevel;
using anvil::playback::PlaybackSessionSnapshot;
using anvil::playback::PlaybackState;
using anvil::playback::ToDisplayString;

namespace {

std::atomic_uint64_t gInspectorFolderScanGeneration{0};
std::atomic_uint64_t gWindowLifetimeCookie{0};
std::atomic_uint64_t gRecentMediaLoadGeneration{0};

constexpr auto kWebUiProgressUpdateInterval = std::chrono::milliseconds{200};
constexpr auto kNativeBufferingNoProgressTimeout = std::chrono::seconds{18};
constexpr auto kNativeBufferingHardTimeout = std::chrono::seconds{45};
constexpr auto kFullscreenTransportHideDelay = std::chrono::seconds{5};
constexpr auto kScrollbarVisibleDuration = std::chrono::milliseconds{650};
constexpr auto kScrollbarFadeDuration = std::chrono::milliseconds{300};
constexpr int kFullscreenTransportActivationHeight = 110;
constexpr std::size_t kMaxRecentMedia = 12;
constexpr std::size_t kMaxInspectorFolderEntries = 256;
constexpr unsigned int kMaxRuntimeStopWorkerStartFailures = 3;
constexpr auto kRecentMediaSaveCoalesceDelay = std::chrono::milliseconds{150};
constexpr UINT kRecentMediaLoadCompleteMessage = WM_APP + 14;

bool IsInspectorNetworkPath(const std::filesystem::path& path) {
    if (IsNetworkMediaPath(path)) {
        return true;
    }
    const std::wstring value = LowerCopy(path.wstring());
    return value.rfind(L"smb://", 0) == 0 ||
           value.rfind(L"\\\\", 0) == 0 ||
           value.rfind(L"//", 0) == 0;
}

RECT SidebarEdgeRect(const RECT& before, const RECT& after) {
    if (!HasArea(before) || !HasArea(after)) {
        return HasArea(before) ? before : after;
    }
    const int left = std::min(before.right, after.right);
    const int right = std::max(before.right, after.right);
    return MakeRect(left, std::min(before.top, after.top), right, std::max(before.bottom, after.bottom));
}

std::wstring FormatMillisecondsFromMicroseconds(const uint64_t microseconds) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << (static_cast<double>(microseconds) / 1000.0);
    return stream.str();
}

std::wstring FormatFixed2(const double value) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
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

std::filesystem::path AppDataStorageFolder() {
    wchar_t localAppData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        return std::filesystem::path(localAppData) / L"AnvilPlayer";
    }
    return std::filesystem::temp_directory_path() / L"anvil-player";
}

bool HdrFormatLooksHdr(const std::wstring& value) {
    return ContainsInsensitive(value, L"HDR") ||
           ContainsInsensitive(value, L"HLG") ||
           ContainsInsensitive(value, L"PQ") ||
           ContainsInsensitive(value, L"Dolby Vision");
}

bool MediaIsDolbyVision(const std::optional<anvil::playback::MediaDescriptor>& media) {
    return media.has_value() &&
           media->hasVideo &&
           (media->dolbyVisionDetected || ContainsInsensitive(media->hdrFormat, L"Dolby Vision"));
}

bool MediaHasDolbyVisionEnhancementStream(const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (!MediaIsDolbyVision(media)) {
        return false;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind == L"Video" && ContainsInsensitive(stream.details, L"EL-only")) {
            return true;
        }
    }
    return false;
}

bool MediaHasHdrSignal(const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (!media.has_value() || !media->hasVideo) {
        return false;
    }
    return media->dolbyVisionDetected ||
           media->videoColor.IsHdr() ||
           HdrFormatLooksHdr(media->hdrFormat);
}

std::wstring InspectorTabName(const InspectorTab tab) {
    switch (tab) {
    case InspectorTab::Recent: return L"recent";
    case InspectorTab::Folder: return L"folder";
    case InspectorTab::Media: return L"media";
    case InspectorTab::System: return L"system";
    case InspectorTab::Log: return L"log";
    case InspectorTab::Settings: return L"settings";
    }
    return L"recent";
}

std::wstring RecentLogLinesJson(const std::shared_ptr<anvil::playback::InMemoryLogSink>& sink, const std::size_t maxCount) {
    std::wostringstream json;
    json << L"[";
    if (sink) {
        const auto entries = sink->LatestEntries(maxCount);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index > 0) {
                json << L",";
            }
            json << L"\"" << JsonEscape(ToDisplayString(entries[index].level) + L" " +
                                        entries[index].category + L": " +
                                        entries[index].message)
                 << L"\"";
        }
    }
    json << L"]";
    return json.str();
}

std::wstring DisplayLanguageName(const std::wstring& language) {
    if (language.empty() || language == L"-") {
        return {};
    }

    const std::wstring normalized = LowerCopy(language);
    if (normalized == L"chi" || normalized == L"zho" || normalized == L"zh" ||
        normalized == L"chs" || normalized == L"cht" || normalized == L"cmn" ||
        normalized == L"cn" || normalized.rfind(L"zh-", 0) == 0 ||
        normalized.find(L"chinese") != std::wstring::npos) {
        if (normalized == L"cht" ||
            normalized.find(L"trad") != std::wstring::npos ||
            normalized.find(L"hant") != std::wstring::npos ||
            normalized.find(L"tw") != std::wstring::npos) {
            return L"Chinese Traditional";
        }
        if (normalized == L"chs" ||
            normalized.find(L"simp") != std::wstring::npos ||
            normalized.find(L"hans") != std::wstring::npos ||
            normalized.find(L"cn") != std::wstring::npos) {
            return L"Chinese Simplified";
        }
        return L"Chinese";
    }
    if (normalized == L"eng" || normalized == L"en") {
        return L"English";
    }
    if (normalized == L"jpn" || normalized == L"ja") {
        return L"Japanese";
    }
    if (normalized == L"kor" || normalized == L"ko") {
        return L"Korean";
    }
    return language;
}

std::wstring UpperCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    return value;
}

std::vector<int> TrackMenuItems(const std::optional<anvil::playback::MediaDescriptor>& media,
                                const std::wstring& kind,
                                const int offTrack,
                                const int autoTrack) {
    std::vector<int> tracks;
    tracks.push_back(offTrack);
    tracks.push_back(autoTrack);
    if (media.has_value()) {
        for (const auto& stream : media->streams) {
            if (stream.kind == kind) {
                tracks.push_back(stream.index);
            }
        }
    }
    return tracks;
}

const anvil::playback::MediaStreamSummary* FindStream(const std::optional<anvil::playback::MediaDescriptor>& media,
                                                      const std::wstring& kind,
                                                      const int index) {
    if (!media.has_value() || index < 0) {
        return nullptr;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind == kind && stream.index == index) {
            return &stream;
        }
    }
    return nullptr;
}

std::wstring TrackLabel(const std::optional<anvil::playback::MediaDescriptor>& media,
                        const std::wstring& kind,
                        const int index,
                        const int offTrack,
                        const int autoTrack) {
    if (index == offTrack) {
        return L"Off";
    }
    if (index == autoTrack) {
        return L"Auto";
    }

    std::wstring label = L"Stream " + std::to_wstring(index);
    if (const auto* stream = FindStream(media, kind, index)) {
        const std::wstring language = DisplayLanguageName(stream->language);
        if (!language.empty()) {
            label = language;
        }
    }
    return label;
}

std::wstring TrackDetail(const std::optional<anvil::playback::MediaDescriptor>& media,
                         const std::wstring& kind,
                         const int index) {
    if (const auto* stream = FindStream(media, kind, index)) {
        const std::wstring codec = UpperCopy(stream->codec);
        if (!codec.empty() && !stream->details.empty()) {
            return codec + L" / " + stream->details;
        }
        return !codec.empty() ? codec : stream->details;
    }
    return {};
}

std::wstring TrackItemsJson(const std::optional<anvil::playback::MediaDescriptor>& media,
                            const std::wstring& kind,
                            const int offTrack,
                            const int autoTrack) {
    const auto tracks = TrackMenuItems(media, kind, offTrack, autoTrack);
    std::wostringstream json;
    json << L"[";
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (index > 0) {
            json << L",";
        }
        const int track = tracks[index];
        json << L"{\"index\":" << track
             << L",\"label\":\"" << JsonEscape(TrackLabel(media, kind, track, offTrack, autoTrack)) << L"\""
             << L",\"detail\":\"" << JsonEscape(TrackDetail(media, kind, track)) << L"\"}";
    }
    json << L"]";
    return json.str();
}

std::wstring HdrToneCurveJson(const anvil::playback::VideoSettings& settings) {
    std::wostringstream json;
    json << L"[";
    for (std::size_t index = 0; index < settings.hdrToneCurve.size(); ++index) {
        if (index > 0) {
            json << L",";
        }
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : settings.hdrToneCurve[index].inputNits;
        json << L"{\"index\":" << index
             << L",\"inputNits\":" << std::fixed << std::setprecision(0) << inputNits
             << L",\"outputNits\":" << std::fixed << std::setprecision(0) << settings.hdrToneCurve[index].outputNits
             << L"}";
    }
    json << L"]";
    return json.str();
}

constexpr wchar_t kVideoSettingsRegistryPath[] = L"Software\\AnvilPlayer\\Video";
constexpr wchar_t kAudioSettingsRegistryPath[] = L"Software\\AnvilPlayer\\Audio";

std::optional<bool> LoadAudioPassthroughSetting() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     kAudioSettingsRegistryPath,
                     L"PassthroughEnabled",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value != 0;
}

std::optional<bool> LoadVideoBooleanSetting(const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     kVideoSettingsRegistryPath,
                     name,
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value != 0;
}

std::optional<int> LoadVideoDwordSetting(const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     kVideoSettingsRegistryPath,
                     name,
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return static_cast<int>(std::min<DWORD>(value, 10000u));
}

void SaveVideoBooleanSetting(const wchar_t* name, const bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        kVideoSettingsRegistryPath,
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD value = enabled ? 1u : 0u;
    RegSetValueExW(key,
                   name,
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

void SaveVideoDwordSetting(const wchar_t* name, const int setting) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        kVideoSettingsRegistryPath,
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD value = static_cast<DWORD>(std::clamp(setting, 0, 10000));
    RegSetValueExW(key,
                   name,
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

bool ResolveWindowDisplayTarget(HWND window,
                                LUID& adapterId,
                                UINT32& targetId,
                                bool& hdrSupported,
                                bool& hdrEnabled) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!window || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return false;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                           &pathCount,
                           paths.data(),
                           &modeCount,
                           modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return false;
    }

    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, monitor.szDevice) != 0) {
            continue;
        }

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = paths[index].targetInfo.adapterId;
        color.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) {
            return false;
        }
        adapterId = color.header.adapterId;
        targetId = color.header.id;
        hdrSupported = color.advancedColorSupported != 0;
        hdrEnabled = color.advancedColorEnabled != 0;
        return true;
    }
    return false;
}

bool SetDisplayHdrState(const LUID adapterId, const UINT32 targetId, const bool enabled) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state{};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = adapterId;
    state.header.id = targetId;
    state.enableAdvancedColor = enabled ? 1u : 0u;
    return DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS;
}

}  // namespace

MainWindow::MainWindow(std::shared_ptr<anvil::playback::InMemoryLogSink> logSink)
    : controller_(std::move(logSink)) {
    auto settings = controller_.Settings();
    if (const auto value = LoadVideoBooleanSetting(L"DisplayMetadataPassthrough")) {
        settings.video.displayMetadataPassthrough = *value;
    }
    if (const auto value = LoadVideoBooleanSetting(L"AutoDisplayFormat")) {
        settings.video.autoDisplayFormat = *value;
        if (*value) {
            settings.video.displayMetadataPassthrough = true;
            settings.video.dolbyVisionSystemPipelineExperimental = true;
        }
    }
    if (const auto value = LoadVideoBooleanSetting(L"DolbyVisionSystemPipelineExperimental")) {
        settings.video.dolbyVisionSystemPipelineExperimental =
            *value && anvil::playback::CapabilityDetector::IsHdrEnabledNow();
    }
    if (const auto value = LoadVideoDwordSetting(L"DisplayPeakBrightnessNits")) {
        settings.video.displayPeakBrightnessNits = *value == 0 ? 0 : std::clamp(*value, 100, 10000);
    }
    settings.audio.passthroughPreferred = LoadAudioPassthroughSetting().value_or(false);
    controller_.ApplySettings(settings);
}

MainWindow::~MainWindow() {
    InvalidateBackgroundListWorkers();
    // Application retires MainWindow on a background reaper. Joining here is
    // therefore safe even when a driver or backend ignored cancellation.
    if (runtimeStopThread_.joinable()) {
        runtimeStopThread_.join();
    }
}

void MainWindow::ReleaseUiThreadResourcesForBackgroundDestruction() noexcept {
    if (uiThreadResourcesReleased_) {
        return;
    }
    uiThreadResourcesReleased_ = true;
    RevokeMediaDropTarget();
    RestoreAutomaticDisplayFormat();
    refreshRateController_.Restore();

    // WebView2 controller/environment releases must stay on the apartment
    // that created them. Reset the host here as well as shutting it down so a
    // later background MainWindow destructor has no COM object to release.
    if (webUiHost_) {
        webUiHost_->Shutdown();
        webUiHost_.reset();
    }
    webUiActive_ = false;

    ClearPreviewBitmap();
    iconPainter_.ClearCache();
}

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

void MainWindow::SetWebUiEnabled(const bool enabled) {
    webUiRequested_ = enabled;
}

void MainWindow::SetQuitHandler(QuitHandler handler) {
    quitHandler_ = std::move(handler);
}

bool MainWindow::IsVisible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

bool MainWindow::DeliverEmbyPlaybackReport(const std::wstring& reportJson) const {
    const bool ready = webUiActive_ && webUiHost_ && webUiHost_->Ready();
    if (ready) {
        const std::wstring message = L"{\"type\":\"deliverEmbyPlaybackReport\",\"report\":" + reportJson + L"}";
        webUiHost_->PostJson(message);
    }
    return ready;
}

bool MainWindow::Create(HINSTANCE instance) {
    windowLifetimeCookie_ = gWindowLifetimeCookie.fetch_add(1) + 1;
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

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    constexpr DWORD windowStyle = WS_POPUP |
                                  WS_THICKFRAME |
                                  WS_SYSMENU |
                                  WS_MINIMIZEBOX |
                                  WS_MAXIMIZEBOX |
                                  WS_CLIPCHILDREN;
    constexpr DWORD windowExStyle = WS_EX_APPWINDOW;
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

    if (!hwnd_) {
        return false;
    }

    const LONG_PTR createdStyle = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, createdStyle & ~static_cast<LONG_PTR>(WS_CAPTION));
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyWindowChrome();

    playbackSupervisor_ = std::make_unique<PlaybackSupervisor>(controller_);
    if (!playbackSupervisor_->Start(
            hwnd_,
            kOpenMediaCompleteMessage,
            kPlaybackSupervisorStoppedMessage,
            windowLifetimeCookie_)) {
        LogApp(LogLevel::Error, L"playback supervisor failed to start");
        playbackSupervisor_.reset();
        DestroyWindow(hwnd_);
        return false;
    }

    LoadRecentMedia();
    TryCreateWebUi();
    return true;
}

void MainWindow::Show(const int commandShow) const {
    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);
}

std::filesystem::path MainWindow::WebUiRoot() const {
    wchar_t modulePath[MAX_PATH]{};
    constexpr DWORD modulePathCount = static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0]));
    const DWORD length = GetModuleFileNameW(instance_, modulePath, modulePathCount);
    if (length > 0 && length < modulePathCount) {
        // Build/package layout always places the compiled UI beside the exe.
        // Do not walk parent directories or the current directory here: either
        // can be an unavailable UNC path and this method runs on the UI thread.
        return std::filesystem::path(modulePath).parent_path() / L"webui";
    }
    return {};
}

void MainWindow::MoveToMonitorOf(const HWND sourceWindow) {
    if (!hwnd_ || !sourceWindow || fullscreen_ || IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        return;
    }

    const HMONITOR targetMonitor = MonitorFromWindow(sourceWindow, MONITOR_DEFAULTTONEAREST);
    const HMONITOR currentMonitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (!targetMonitor || targetMonitor == currentMonitor) {
        return;
    }

    MONITORINFO targetInfo{};
    targetInfo.cbSize = sizeof(targetInfo);
    RECT playerBounds{};
    if (!GetMonitorInfoW(targetMonitor, &targetInfo) || !GetWindowRect(hwnd_, &playerBounds)) {
        return;
    }

    const int width = std::min(RectWidth(playerBounds), RectWidth(targetInfo.rcWork));
    const int height = std::min(RectHeight(playerBounds), RectHeight(targetInfo.rcWork));
    const int x = targetInfo.rcWork.left + std::max(0, RectWidth(targetInfo.rcWork) - width) / 2;
    const int y = targetInfo.rcWork.top + std::max(0, RectHeight(targetInfo.rcWork) - height) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
    LogApp(LogLevel::Info, L"player moved to library monitor");
}

void MainWindow::SetPendingMediaTrackSelections(const int audioTrackIndex,
                                                const int subtitleTrackIndex) {
    auto settings = controller_.Settings();
    settings.audio.selectedTrackIndex = audioTrackIndex;
    settings.subtitles.selectedTrackIndex = subtitleTrackIndex;
    controller_.ApplySettings(settings);
    LogApp(LogLevel::Info,
           L"library track preselection audio=" + std::to_wstring(audioTrackIndex) +
           L" subtitle=" + std::to_wstring(subtitleTrackIndex));
}

bool MainWindow::TryCreateWebUi() {
    if (!webUiRequested_ || webUiActive_ || !hwnd_) {
        return false;
    }

    auto host = std::make_unique<WebUiHost>();
    const auto root = WebUiRoot();
    // MainWindow is the player window: it always navigates to the player route.
    // The media library lives in its own LibraryWindow. Use a dedicated profile
    // so the player and library WebView2 environments never collide.
    constexpr wchar_t kWebUiPlayerUrl[] = L"http://appassets.anvilplayer.local/index.html#/player";
    const bool started = host->Create(hwnd_, root, kWebUiPlayerUrl, L"Player",
                                      [this](const std::wstring& message) {
                                          HandleWebUiMessage(message);
                                      });
    if (!started) {
        LogApp(LogLevel::Warning, L"web ui unavailable root=" + root.wstring() + L" hr=0x" + HexHr(host->LastCreateResult()));
        return false;
    }

    webUiHost_ = std::move(host);
    webUiActive_ = true;
    LogApp(LogLevel::Info,
           L"web ui enabled root=" + root.wstring() +
               (webUiHost_->UsedDefaultOptionsFallback() ? L" options=default_after_retry" : L" options=custom"));
    return true;
}

std::wstring MainWindow::BuildWebUiStateJson() const {
    const auto snapshot = controller_.Snapshot();
    const auto settings = controller_.Settings();
    const auto capabilities = anvil::playback::CapabilityDetector::CollectBasic();
    const int detectedDisplayPeakNits = d3dRenderer_
                                            ? d3dRenderer_->DetectedDisplayPeakNits()
                                            : std::max(0, capabilities.display.reportedPeakBrightnessNits);
    const int effectiveDisplayPeakNits = settings.video.displayPeakBrightnessNits > 0
                                             ? settings.video.displayPeakBrightnessNits
                                             : (detectedDisplayPeakNits > 0 ? detectedDisplayPeakNits : 1000);

    std::wstring playbackState = ToDisplayString(snapshot.state);
    if (!snapshot.media.has_value()) {
        playbackState = L"Empty";
    }

    const std::wstring mediaName = snapshot.media.has_value() ? snapshot.media->displayName : L"No media loaded";
    long long positionMs = std::max<long long>(0, snapshot.position.count());
    const long long durationMs = snapshot.media.has_value() ? std::max<long long>(0, snapshot.media->duration.count()) : 0;
    long long bufferedEndMs = positionMs;
    bool buffering = false;
    uint64_t networkBytesPerSecond = 0;
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_ &&
        snapshot.media.has_value() &&
        snapshot.media->duration.count() > 0) {
        const auto stats = nativeVideoDecoder_->Stats();
        buffering = stats.buffering;
        networkBytesPerSecond = stats.networkBytesPerSecond;
        if (buffering) {
            positionMs = std::clamp<long long>(stats.clockPosition.count(), 0, durationMs);
            bufferedEndMs = positionMs;
        }
        if ((stats.queueDepth > 0 || stats.packetQueueDepth > 0) && stats.bufferedEnd.count() > positionMs) {
            bufferedEndMs = std::clamp<long long>(stats.bufferedEnd.count(), positionMs, durationMs);
        }
    }
    const bool windowsHdrEnabled = anvil::playback::CapabilityDetector::IsHdrEnabledNow();
    const bool hdrAvailable = CurrentMediaHasHdrControls();
    const bool dolbyVisionMedia = MediaIsDolbyVision(snapshot.media);
    const bool cmv4Available = CurrentMediaHasCmv4Control();
    const bool cmv4Enabled = CurrentCmv4ControlEnabled(settings) && settings.video.dolbyVisionCmv4Approx;
    const bool hdrToneCurveAvailable = HdrToneCurveAvailable(settings);
    const bool hasVideo = snapshot.media.has_value() && snapshot.media->hasVideo;
    const bool hasAudio = snapshot.media.has_value() && snapshot.media->hasAudio;
    const std::wstring mediaPath = snapshot.media.has_value() ? snapshot.media->path.wstring() : L"";
    const std::wstring container = snapshot.media.has_value() ? snapshot.media->container : L"";
    const std::wstring videoCodec = snapshot.media.has_value() ? snapshot.media->videoCodec : L"";
    const std::wstring audioCodec = snapshot.media.has_value() ? snapshot.media->audioCodec : L"";
    const std::wstring hdrFormat = snapshot.media.has_value() ? snapshot.media->hdrFormat : L"";
    std::wstring resolution;
    std::wstring frameRate;
    int streamCount = 0;
    if (snapshot.media.has_value()) {
        streamCount = static_cast<int>(snapshot.media->streams.size());
        if (snapshot.media->videoWidth > 0 && snapshot.media->videoHeight > 0) {
            resolution = std::to_wstring(snapshot.media->videoWidth) + L" x " + std::to_wstring(snapshot.media->videoHeight);
        }
        if (snapshot.media->videoFrameRate > 0.0) {
            std::wostringstream fps;
            fps << std::fixed << std::setprecision(3) << snapshot.media->videoFrameRate << L" fps";
            frameRate = fps.str();
        }
    }

    std::wostringstream json;
    json << L"{\"type\":\"state\",\"state\":{";
    json << L"\"uiLanguage\":\"" << DisplayRefreshRateController::LoadUiLanguage() << L"\",";
    json << L"\"playbackState\":\"" << JsonEscape(playbackState) << L"\",";
    json << L"\"lastError\":\"" << JsonEscape(snapshot.lastError) << L"\",";
    json << L"\"mediaName\":\"" << JsonEscape(mediaName) << L"\",";
    json << L"\"mediaPath\":\"" << JsonEscape(mediaPath) << L"\",";
    json << L"\"hasMedia\":" << (snapshot.media.has_value() ? L"true" : L"false") << L",";
    json << L"\"hasVideo\":" << (hasVideo ? L"true" : L"false") << L",";
    json << L"\"hasAudio\":" << (hasAudio ? L"true" : L"false") << L",";
    json << L"\"positionMs\":" << positionMs << L",";
    json << L"\"durationMs\":" << durationMs << L",";
    json << L"\"bufferedEndMs\":" << bufferedEndMs << L",";
    json << L"\"buffering\":" << (buffering ? L"true" : L"false") << L",";
    json << L"\"networkKbps\":" << (networkBytesPerSecond / 1024) << L",";
    json << L"\"volume\":" << std::fixed << std::setprecision(3) << std::clamp(snapshot.volume, 0.0, 1.0) << L",";
    json << L"\"runtimeLabel\":\"" << JsonEscape(RuntimeShortLabel()) << L"\",";
    json << L"\"backendLabel\":\"" << JsonEscape(RuntimeLabel()) << L"\",";
    json << L"\"sidebarCollapsed\":" << (inspectorCollapsed_ ? L"true" : L"false") << L",";
    json << L"\"fullscreen\":" << (fullscreen_ ? L"true" : L"false") << L",";
    json << L"\"customTitleBar\":true,";
    json << L"\"refreshRateSyncEnabled\":" << (refreshRateSyncEnabled_ ? L"true" : L"false") << L",";
    json << L"\"refreshRateMaximumMultiple\":" << (refreshRateMaximumMultiple_ ? L"true" : L"false") << L",";
    json << L"\"refreshRateSyncUnavailable\":" << (refreshRateSyncUnavailable_ ? L"true" : L"false") << L",";
    json << L"\"refreshRateSyncActive\":" << (refreshRateController_.Active() ? L"true" : L"false") << L",";
    json << L"\"refreshRateSyncHz\":" << std::fixed << std::setprecision(3) << refreshRateController_.AppliedRefreshRate() << L",";
    json << L"\"fullscreenTransportVisible\":"
         << ((!fullscreen_ || fullscreenTransportTarget_ > 0.0 || draggingProgress_ || draggingVolume_) ? L"true" : L"false")
         << L",";
    json << L"\"subtitleMenuOpen\":" << ((subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) ? L"true" : L"false") << L",";
    json << L"\"inspectorTab\":\"" << InspectorTabName(inspectorTab_) << L"\",";
    json << L"\"hdrAvailable\":" << (hdrAvailable ? L"true" : L"false") << L",";
    json << L"\"hdrOutput\":" << (settings.video.dolbyVisionHdrOutput ? L"true" : L"false") << L",";
    json << L"\"hdrOutputLocked\":"
         << ((settings.video.autoDisplayFormat ||
              (windowsHdrEnabled && (settings.video.displayMetadataPassthrough ||
                                    (dolbyVisionMedia && settings.video.dolbyVisionSystemPipelineExperimental)))
             )
                 ? L"true"
                 : L"false")
         << L",";
    json << L"\"windowsHdrEnabled\":" << (windowsHdrEnabled ? L"true" : L"false") << L",";
    json << L"\"hdrDisplayPeakAutomatic\":"
         << (settings.video.displayPeakBrightnessNits == 0 ? L"true" : L"false") << L",";
    json << L"\"hdrDisplayPeakConfiguredNits\":" << settings.video.displayPeakBrightnessNits << L",";
    json << L"\"hdrDisplayPeakDetectedNits\":" << detectedDisplayPeakNits << L",";
    json << L"\"hdrDisplayPeakEffectiveNits\":" << effectiveDisplayPeakNits << L",";
    json << L"\"autoDisplayFormat\":" << (settings.video.autoDisplayFormat ? L"true" : L"false") << L",";
    json << L"\"displayMetadataPassthrough\":" << (settings.video.displayMetadataPassthrough ? L"true" : L"false") << L",";
    json << L"\"dolbyVisionSystemPipelineExperimental\":"
         << (settings.video.dolbyVisionSystemPipelineExperimental ? L"true" : L"false") << L",";
    json << L"\"dolbyVisionSystemPipelineAvailable\":"
         << ((windowsHdrEnabled && cachedCapabilities_.codecs.dolbyVisionExtensionDetected) ? L"true" : L"false")
         << L",";
    json << L"\"dolbyVisionMedia\":" << (dolbyVisionMedia ? L"true" : L"false") << L",";
    json << L"\"cmv4Available\":" << (cmv4Available ? L"true" : L"false") << L",";
    json << L"\"cmv4Enabled\":" << (cmv4Enabled ? L"true" : L"false") << L",";
    const bool audioPassthroughRequested = settings.audio.passthroughPreferred;
    const bool audioPassthroughActive = audioPlayer_.IsPassthroughActive();
    std::wstring audioPassthroughReason = audioPassthroughRequested
                                              ? audioPlayer_.PassthroughReason()
                                              : L"disabled";
    if (audioPassthroughRequested && (!snapshot.media.has_value() || !snapshot.media->hasAudio)) {
        audioPassthroughReason = L"no_audio";
    } else if (audioPassthroughRequested &&
               !audioPlayer_.IsRunning() &&
               (snapshot.state == PlaybackState::Ready ||
                snapshot.state == PlaybackState::Paused ||
                snapshot.state == PlaybackState::Stopped)) {
        audioPassthroughReason = L"pending";
    }
    json << L"\"audioPassthroughRequested\":" << (audioPassthroughRequested ? L"true" : L"false") << L",";
    json << L"\"audioPassthroughActive\":" << (audioPassthroughActive ? L"true" : L"false") << L",";
    json << L"\"audioPassthroughReason\":\"" << JsonEscape(audioPassthroughReason) << L"\",";
    json << L"\"audioPassthroughCodec\":\"" << JsonEscape(audioPlayer_.PassthroughCodec()) << L"\",";
    json << L"\"audioPassthroughOutput\":\"" << JsonEscape(audioPlayer_.LastStatus()) << L"\",";
    json << L"\"audioSelectedTrack\":" << settings.audio.selectedTrackIndex << L",";
    json << L"\"subtitleSelectedTrack\":" << settings.subtitles.selectedTrackIndex << L",";
    json << L"\"subtitleDelayMs\":" << settings.subtitles.subtitleDelayMs << L",";
    json << L"\"subtitleFontScale\":" << std::fixed << std::setprecision(2) << settings.subtitles.fontScale << L",";
    json << L"\"subtitleOffsetX\":" << settings.subtitles.offsetXPx << L",";
    json << L"\"subtitleOffsetY\":" << settings.subtitles.offsetYPx << L",";
    json << L"\"danmakuEnabled\":" << (settings.danmaku.enabled ? L"true" : L"false") << L",";
    json << L"\"danmakuMode\":" << settings.danmaku.mode << L",";
    json << L"\"danmakuOpacityPercent\":" << settings.danmaku.opacityPercent << L",";
    json << L"\"danmakuSpeedPercent\":" << settings.danmaku.speedPercent << L",";
    json << L"\"danmakuPath\":\"" << JsonEscape(settings.danmaku.externalDanmakuPath.wstring()) << L"\",";
    json << L"\"audioTracks\":" << TrackItemsJson(snapshot.media,
                                                   L"Audio",
                                                   anvil::playback::kAudioTrackOff,
                                                  anvil::playback::kAudioTrackAuto) << L",";
    json << L"\"subtitleTracks\":" << TrackItemsJson(snapshot.media,
                                                     L"Subtitle",
                                                     anvil::playback::kSubtitleTrackOff,
                                                     anvil::playback::kSubtitleTrackAuto) << L",";
    json << L"\"hdrToneCurveAvailable\":" << (hdrToneCurveAvailable ? L"true" : L"false") << L",";
    json << L"\"hdrToneCurvePeakNits\":" << settings.video.peakBrightnessNits << L",";
    json << L"\"hdrToneCurve\":" << HdrToneCurveJson(settings.video) << L",";
    json << L"\"container\":\"" << JsonEscape(container) << L"\",";
    json << L"\"videoCodec\":\"" << JsonEscape(videoCodec) << L"\",";
    json << L"\"audioCodec\":\"" << JsonEscape(audioCodec) << L"\",";
    json << L"\"hdrFormat\":\"" << JsonEscape(hdrFormat) << L"\",";
    json << L"\"resolution\":\"" << JsonEscape(resolution) << L"\",";
    json << L"\"frameRate\":\"" << JsonEscape(frameRate) << L"\",";
    json << L"\"streamCount\":" << streamCount << L",";
    json << L"\"recentMedia\":" << MediaPathItemsJson(recentMedia_, 8) << L",";
    json << L"\"folderMedia\":" << MediaPathItemsJson(currentFolderEntries_, 16) << L",";
    json << L"\"logLines\":" << RecentLogLinesJson(controller_.LogSink(), 14);
    json << L"}}";
    return json.str();
}

void MainWindow::PostWebUiState(const bool force) const {
    if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            lastWebUiStatePostedAt_.time_since_epoch().count() != 0 &&
            now - lastWebUiStatePostedAt_ < kWebUiProgressUpdateInterval) {
            return;
        }
        lastWebUiStatePostedAt_ = now;
        webUiHost_->PostJson(BuildWebUiStateJson());
    }
}

void MainWindow::HandleWebUiMessage(const std::wstring_view message) {
    if (MessageContains(message, L"\"command\":\"localPlaybackProgress\"")) {
        if (localPlaybackProgressRelay_) localPlaybackProgressRelay_(std::wstring(message));
        return;
    }
    if (MessageContains(message, L"\"type\":\"requestState\"")) {
        PostWebUiState();
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
        LogApp(LogLevel::Info,
               std::wstring(L"web ui insecure certificates ") + (enabled ? L"enabled" : L"disabled"));
    } else if (MessageContains(message, L"\"command\":\"setWebUiRoute\"")) {
        // Legacy single-window route notification. MainWindow is always the
        // player window now, so the route never changes here. The standalone
        // LibraryWindow owns the library route.
    } else if (MessageContains(message, L"\"command\":\"beginWindowDrag\"")) {
        if (!fullscreen_ && !IsZoomed(hwnd_)) {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    } else if (MessageContains(message, L"\"command\":\"minimizeWindow\"")) {
        ShowWindow(hwnd_, SW_MINIMIZE);
    } else if (MessageContains(message, L"\"command\":\"toggleMaximizeWindow\"")) {
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (MessageContains(message, L"\"command\":\"closeWindow\"")) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (MessageContains(message, L"\"command\":\"open\"")) {
        OpenFileDialog();
    } else if (MessageContains(message, L"\"command\":\"pickLocalFolder\"") ||
               MessageContains(message, L"\"command\":\"scanLocalFolder\"") ||
               MessageContains(message, L"\"command\":\"listSmbDirectory\"") ||
               MessageContains(message, L"\"command\":\"connectSmbShare\"") ||
               MessageContains(message, L"\"command\":\"listWebDavDirectory\"") ||
               MessageContains(message, L"\"command\":\"scanWebDavFolder\"")) {
        // Source management is owned by the standalone LibraryWindow. Never
        // let discovery/network commands enter the player message pump.
        LogApp(LogLevel::Debug, L"ignored library command on player webview");
        return;
    } else if (MessageContains(message, L"\"command\":\"debugLog\"")) {
        if (const auto text = ReadJsonString(message, L"message")) {
            LogApp(LogLevel::Debug, L"web ui debug: " + *text);
        }
        // Debug logging is telemetry rather than a state-changing command. Do
        // not echo a state snapshot: Web UI state handlers may log diagnostics,
        // and reflecting those logs back as state creates a feedback loop.
        return;
    } else if (MessageContains(message, L"\"command\":\"openPath\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            pendingStartPositionRatio_ = ReadJsonNumber(message, L"startPositionRatio").value_or(0.0);
            OpenPath(std::filesystem::path(*path), true);
        }
    } else if (MessageContains(message, L"\"command\":\"playPause\"")) {
        TogglePlayback();
    } else if (MessageContains(message, L"\"command\":\"stop\"")) {
        StopPlayback();
    } else if (MessageContains(message, L"\"command\":\"back\"")) {
        SeekRelative(-std::chrono::seconds{10});
    } else if (MessageContains(message, L"\"command\":\"forward\"")) {
        SeekRelative(std::chrono::seconds{10});
    } else if (MessageContains(message, L"\"command\":\"toggleSidebar\"")) {
        ToggleSidebar();
    } else if (MessageContains(message, L"\"command\":\"toggleFullscreen\"")) {
        ToggleFullscreen();
    } else if (MessageContains(message, L"\"command\":\"setRefreshRateSync\"")) {
        SetRefreshRateSyncEnabled(MessageContains(message, L"\"enabled\":true"));
    } else if (MessageContains(message, L"\"command\":\"setRefreshRateMaximumMultiple\"")) {
        SetRefreshRateMaximumMultiple(MessageContains(message, L"\"enabled\":true"));
    } else if (MessageContains(message, L"\"command\":\"dismissRefreshRateSyncUnavailable\"")) {
        refreshRateSyncUnavailable_ = false;
        MarkLayoutDirty();
        EnsureLayout();
        UpdateVideoHost();
    } else if (MessageContains(message, L"\"command\":\"showFullscreenTransport\"")) {
        ShowFullscreenTransport(L"web_ui_activation");
    } else if (MessageContains(message, L"\"command\":\"subtitleGeometry\"")) {
        const double scale = std::clamp(ReadJsonNumber(message, L"scale").value_or(1.0), 0.25, 4.0);
        const auto scaled = [scale](const std::optional<double>& value) {
            return static_cast<int>(std::round(value.value_or(0.0) * scale));
        };
        const auto anchorLeft = ReadJsonNumber(message, L"anchorLeft");
        const auto anchorTop = ReadJsonNumber(message, L"anchorTop");
        const auto anchorWidth = ReadJsonNumber(message, L"anchorWidth");
        const auto anchorHeight = ReadJsonNumber(message, L"anchorHeight");
        const auto popoverLeft = ReadJsonNumber(message, L"popoverLeft");
        const auto popoverTop = ReadJsonNumber(message, L"popoverTop");
        const auto popoverWidth = ReadJsonNumber(message, L"popoverWidth");
        const auto popoverHeight = ReadJsonNumber(message, L"popoverHeight");
        if (anchorLeft && anchorTop && anchorWidth && anchorHeight &&
            popoverLeft && popoverTop && popoverWidth && popoverHeight) {
            const int anchorX = scaled(anchorLeft);
            const int anchorY = scaled(anchorTop);
            const int anchorW = std::max(1, scaled(anchorWidth));
            const int anchorH = std::max(1, scaled(anchorHeight));
            const int popoverX = scaled(popoverLeft);
            const int popoverY = scaled(popoverTop);
            const int popoverW = std::max(1, scaled(popoverWidth));
            const int popoverH = std::max(1, scaled(popoverHeight));
            webUiSubtitleAnchor_ = MakeRect(anchorX, anchorY, anchorX + anchorW, anchorY + anchorH);
            webUiSubtitlePopover_ = MakeRect(popoverX, popoverY, popoverX + popoverW, popoverY + popoverH);
            webUiSubtitleGeometryValid_ = true;
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (MessageContains(message, L"\"command\":\"transportGeometry\"")) {
        const double scale = std::clamp(ReadJsonNumber(message, L"scale").value_or(1.0), 0.25, 4.0);
        const auto scaled = [scale](const std::optional<double>& value) {
            return static_cast<int>(std::round(value.value_or(0.0) * scale));
        };
        const auto left = ReadJsonNumber(message, L"left");
        const auto top = ReadJsonNumber(message, L"top");
        const auto width = ReadJsonNumber(message, L"width");
        const auto height = ReadJsonNumber(message, L"height");
        if (left && top && width && height) {
            const int x = scaled(left);
            const int y = scaled(top);
            const int w = std::max(1, scaled(width));
            const int h = std::max(1, scaled(height));
            webUiTransportBounds_ = MakeRect(x, y, x + w, y + h);
            webUiTransportGeometryValid_ = true;
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (MessageContains(message, L"\"command\":\"videoGeometry\"")) {
        const double scale = std::clamp(ReadJsonNumber(message, L"scale").value_or(1.0), 0.25, 4.0);
        const auto scaled = [scale](const std::optional<double>& value) {
            return static_cast<int>(std::round(value.value_or(0.0) * scale));
        };
        const auto left = ReadJsonNumber(message, L"left");
        const auto top = ReadJsonNumber(message, L"top");
        const auto width = ReadJsonNumber(message, L"width");
        const auto height = ReadJsonNumber(message, L"height");
        if (left && top && width && height) {
            const int x = scaled(left);
            const int y = scaled(top);
            const int w = std::max(1, scaled(width));
            const int h = std::max(1, scaled(height));
            webUiVideoBounds_ = MakeRect(x, y, x + w, y + h);
            webUiVideoGeometryValid_ = true;
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    } else if (MessageContains(message, L"\"command\":\"subtitleMenu\"")) {
        ShowSubtitleMenu();
    } else if (MessageContains(message, L"\"command\":\"hideSubtitleMenu\"")) {
        HideSubtitleMenu();
    } else if (MessageContains(message, L"\"command\":\"settings\"")) {
        Execute(Command::Settings);
    } else if (MessageContains(message, L"\"command\":\"inspectorRecent\"")) {
        Execute(Command::InspectorRecent);
    } else if (MessageContains(message, L"\"command\":\"inspectorFolder\"")) {
        Execute(Command::InspectorFolder);
    } else if (MessageContains(message, L"\"command\":\"inspectorMedia\"")) {
        Execute(Command::InspectorMedia);
    } else if (MessageContains(message, L"\"command\":\"inspectorSystem\"")) {
        Execute(Command::InspectorSystem);
    } else if (MessageContains(message, L"\"command\":\"inspectorLog\"")) {
        Execute(Command::InspectorLog);
    } else if (MessageContains(message, L"\"command\":\"toggleHdr\"")) {
        Execute(Command::ToggleDolbyVisionHdr);
    } else if (MessageContains(message, L"\"command\":\"toggleCmv4\"")) {
        Execute(Command::ToggleDolbyVisionCmv4Approx);
    } else if (MessageContains(message, L"\"command\":\"setAutoDisplayFormat\"")) {
        SetAutomaticDisplayFormat(MessageContains(message, L"\"enabled\":true"));
    } else if (MessageContains(message, L"\"command\":\"setDisplayPeakBrightness\"")) {
        if (const auto peakNits = ReadJsonNumber(message, L"peakNits")) {
            SetDisplayPeakBrightnessNits(static_cast<int>(std::round(*peakNits)));
        }
    } else if (MessageContains(message, L"\"command\":\"setDisplayMetadataPassthrough\"")) {
        SetDisplayMetadataPassthrough(MessageContains(message, L"\"enabled\":true"));
    } else if (MessageContains(message, L"\"command\":\"setDolbyVisionSystemPipelineExperimental\"")) {
        SetDolbyVisionSystemPipelineExperimental(MessageContains(message, L"\"enabled\":true"));
    } else if (MessageContains(message, L"\"command\":\"setCurrentAudioPassthrough\"")) {
        SetCurrentAudioPassthrough(MessageContains(message, L"\"enabled\":true"));
    } else if (MessageContains(message, L"\"command\":\"setAudioTrack\"")) {
        if (const auto index = ReadJsonNumber(message, L"index")) {
            ApplyAudioSelection(static_cast<int>(std::round(*index)));
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleTrack\"")) {
        if (const auto index = ReadJsonNumber(message, L"index")) {
            ApplySubtitleSelection(static_cast<int>(std::round(*index)));
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleDelay\"")) {
        if (const auto delayMs = ReadJsonNumber(message, L"delayMs")) {
            const auto settings = controller_.Settings();
            ApplySubtitleDelayDelta(static_cast<int>(std::round(*delayMs)) - settings.subtitles.subtitleDelayMs);
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleFontScale\"")) {
        if (const auto scale = ReadJsonNumber(message, L"scale")) {
            const auto settings = controller_.Settings();
            ApplySubtitleFontScaleDelta(std::clamp(*scale, 0.5, 2.0) - settings.subtitles.fontScale);
        }
    } else if (MessageContains(message, L"\"command\":\"setSubtitleOffset\"")) {
        const auto x = ReadJsonNumber(message, L"x");
        const auto y = ReadJsonNumber(message, L"y");
        if (x && y) {
            const auto settings = controller_.Settings();
            ApplySubtitleOffsetDelta(static_cast<int>(std::round(*x)) - settings.subtitles.offsetXPx,
                                     static_cast<int>(std::round(*y)) - settings.subtitles.offsetYPx);
        }
    } else if (MessageContains(message, L"\"command\":\"openSubtitleFile\"")) {
        OpenSubtitleFileDialog();
    } else if (MessageContains(message, L"\"command\":\"toggleDanmakuEnabled\"")) {
        ToggleDanmakuEnabled();
    } else if (MessageContains(message, L"\"command\":\"cycleDanmakuMode\"")) {
        CycleDanmakuMode();
    } else if (MessageContains(message, L"\"command\":\"setDanmakuOpacity\"")) {
        if (const auto opacityPercent = ReadJsonNumber(message, L"opacityPercent")) {
            const auto settings = controller_.Settings();
            ApplyDanmakuOpacityDelta(static_cast<int>(std::round(*opacityPercent)) - settings.danmaku.opacityPercent);
        }
    } else if (MessageContains(message, L"\"command\":\"setDanmakuSpeed\"")) {
        if (const auto speedPercent = ReadJsonNumber(message, L"speedPercent")) {
            const auto settings = controller_.Settings();
            ApplyDanmakuSpeedDelta(static_cast<int>(std::round(*speedPercent)) - settings.danmaku.speedPercent);
        }
    } else if (MessageContains(message, L"\"command\":\"openDanmakuFile\"")) {
        OpenDanmakuFileDialog();
    } else if (MessageContains(message, L"\"command\":\"resetHdrToneCurve\"")) {
        ResetHdrToneCurve();
    } else if (MessageContains(message, L"\"command\":\"toggleHdrToneCurveExpanded\"")) {
        Execute(Command::ToggleHdrToneCurveExpanded);
    } else if (MessageContains(message, L"\"command\":\"setHdrToneCurvePoints\"")) {
        std::array<double, anvil::playback::kHdrToneCurvePointCount> outputNits{};
        std::array<bool, anvil::playback::kHdrToneCurvePointCount> hasOutput{};
        for (std::size_t index = 1; index < outputNits.size(); ++index) {
            const std::wstring field = L"output" + std::to_wstring(index);
            if (const auto value = ReadJsonNumber(message, field.c_str())) {
                outputNits[index] = *value;
                hasOutput[index] = true;
            }
        }
        ApplyHdrToneCurvePoints(outputNits, hasOutput);
    } else if (MessageContains(message, L"\"command\":\"setHdrToneCurvePoint\"")) {
        const auto index = ReadJsonNumber(message, L"index");
        const auto outputNits = ReadJsonNumber(message, L"outputNits");
        if (index && outputNits) {
            ApplyHdrToneCurvePoint(static_cast<int>(std::round(*index)), *outputNits);
        }
    } else if (MessageContains(message, L"\"command\":\"setVolume\"")) {
        if (const auto volume = ReadJsonNumber(message, L"volume")) {
            ApplyVolume(std::clamp(*volume, 0.0, 1.0), true);
        }
    } else if (MessageContains(message, L"\"command\":\"seekToRatio\"")) {
        const auto snapshot = controller_.Snapshot();
        if (snapshot.media.has_value() && snapshot.media->duration.count() > 0) {
            if (const auto ratio = ReadJsonNumber(message, L"ratio")) {
                const auto duration = snapshot.media->duration;
                SeekToPosition(std::chrono::milliseconds{
                    static_cast<long long>(static_cast<double>(duration.count()) * std::clamp(*ratio, 0.0, 1.0))});
            }
        }
    }

    PostWebUiState();
}

void MainWindow::OpenInitialPath(const std::filesystem::path& path, const bool autoplay) {
    if (closePending_) {
        return;
    }
    if (!path.empty()) {
        LogApp(LogLevel::Info, L"initial path=" + path.wstring() + L" autoplay=" + (autoplay ? L"true" : L"false"));
        OpenPath(path, autoplay);
    }
    PostWebUiState();
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
        try {
            return window->HandleMessage(message, wParam, lParam);
        } catch (const std::exception& error) {
            window->LogApp(LogLevel::Error,
                           L"window message exception message=0x" + std::to_wstring(message) +
                               L" error=" + Utf8ToWide(error.what()));
            return 0;
        } catch (...) {
            window->LogApp(LogLevel::Error,
                           L"window message exception message=0x" + std::to_wstring(message) +
                               L" error=unknown");
            return 0;
        }
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

    if (window && message == WM_MOUSEMOVE && !window->videoHostTrackingMouseLeave_) {
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd;
        window->videoHostTrackingMouseLeave_ = TrackMouseEvent(&event) != FALSE;
    }
    if (window && message == WM_MOUSEMOVE) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        window->OnMouseMove(point.x, point.y);
        return 0;
    }
    if (window && message == WM_LBUTTONDOWN) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        window->OnLeftButtonDown(point.x, point.y);
        return 0;
    }
    if (window && message == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(hwnd, window->hwnd_, &point, 1);
        window->OnLeftButtonUp(point.x, point.y);
        return 0;
    }
    if (window && message == WM_MOUSELEAVE) {
        window->videoHostTrackingMouseLeave_ = false;
        window->OnMouseLeave();
        return 0;
    }
    if (window && message == WM_MOUSEWHEEL) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        window->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), point);
        return 0;
    }
    if (window && message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window->hwnd_, &point);
        SetCursor(LoadCursorW(nullptr, window->IsPointInteractive(point) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    if (window &&
        (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        (wParam == VK_ESCAPE || wParam == VK_SPACE || wParam == VK_LEFT || wParam == VK_RIGHT)) {
        window->OnKeyDown(wParam);
        return 0;
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
    if (window && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
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
    if (window && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
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

LRESULT CALLBACK MainWindow::BufferingOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::BufferingHudOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (window && message == WM_TIMER) {
        window->RenderBufferingHudOverlay(window->bufferingOverlayStats_);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SubtitleMenuOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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
        window->PaintSubtitleMenuOverlay(hwnd);
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

LRESULT MainWindow::HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    if (closePending_) {
        switch (message) {
        case WM_COPYDATA:
            return FALSE;
        case kOpenPathMessage:
            delete reinterpret_cast<std::filesystem::path*>(lParam);
            return 0;
        case kOpenMediaCompleteMessage:
            return 0;
        case kNativeVideoDecodeFailedMessage:
            delete reinterpret_cast<NativeDecodeFailure*>(lParam);
            return 0;
        case WM_DROPFILES:
            DragFinish(reinterpret_cast<HDROP>(wParam));
            return 0;
        case kVideoFrameReadyMessage:
        case kNativeVideoFrameReadyMessage:
        case kPlaybackTimerTickMessage:
        case kNativeColorSettingsRefreshMessage:
        case kRenderInitializationCompleteMessage:
            return 0;
        default:
            break;
        }
    }
    // Single-instance command-line forwarding: a second launch sends its raw
    // command line (UTF-16) to this running instance via WM_COPYDATA. Windows
    // only marshals COPYDATASTRUCT payloads for WM_COPYDATA across the process
    // boundary, so we validate via the dwData magic rather than a registered
    // message id. When a media path is present, open it here.
    if (message == WM_COPYDATA) {
        auto* copyData = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (copyData && copyData->dwData == kForwardCommandLineMagic &&
            copyData->lpData != nullptr && copyData->cbData > 0 &&
            (copyData->cbData % sizeof(wchar_t)) == 0) {
            const std::size_t charCount = copyData->cbData / sizeof(wchar_t);
            std::wstring forwarded(static_cast<const wchar_t*>(copyData->lpData), charCount);
            const auto arguments = ParseCommandLine(forwarded);
            LogApp(LogLevel::Info, L"forwarded command line=" + forwarded);
            if (!arguments.mediaPath.empty()) {
                OpenInitialPath(arguments.mediaPath, arguments.autoplay);
            }
            return TRUE;
        }
    }

    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam != FALSE && IsZoomed(hwnd_)) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                params->rgrc[0] = monitor.rcWork;
            }
        }
        return 0;
    case WM_NCHITTEST:
        if (!fullscreen_ && !IsZoomed(hwnd_)) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT windowRect{};
            GetWindowRect(hwnd_, &windowRect);
            const int edge = Scale(6);
            const bool left = point.x < windowRect.left + edge;
            const bool right = point.x >= windowRect.right - edge;
            const bool top = point.y < windowRect.top + edge;
            const bool bottom = point.y >= windowRect.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        return HTCLIENT;
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        ApplyWindowChrome();
        DragAcceptFiles(hwnd_, TRUE);
        RegisterMediaDropTarget();
        SetPlaybackTimer(false);
        MarkLayoutDirty();
        EnsureLayout();
        return 0;
    case kVideoFrameReadyMessage:
        if (static_cast<uint64_t>(wParam) != windowLifetimeCookie_) {
            return 0;
        }
        if (runtimeStopAsyncInProgress_.load()) {
            return 0;
        }
        videoDecoder_.AcknowledgeFrameNotification();
        if (backend_ == PlaybackBackend::RawFrameBridge) {
            if (audioPlayer_.IsStarting()) {
                if (const auto audioPosition = audioPlayer_.PlaybackClock()) {
                    controller_.SyncClock(*audioPosition);
                }
            } else {
                controller_.UpdateClock();
            }
            const auto snapshot = controller_.Snapshot();
            RenderPlaybackTick(snapshot, false);
            InvalidateVideoSurface();
        }
        return 0;
    case kNativeVideoFrameReadyMessage: {
        if (static_cast<uint64_t>(wParam) != windowLifetimeCookie_) {
            return 0;
        }
        if (runtimeStopAsyncInProgress_.load()) {
            return 0;
        }
        if (nativeVideoDecoder_) {
            nativeVideoDecoder_->AcknowledgeFrameNotification();
            if (SidebarAnimationActive() && !webUiActive_) {
                return 0;
            }
            const auto snapshot = controller_.Snapshot();
            const auto stats = nativeVideoDecoder_->Stats();
            const bool nativeBuffering = snapshot.state == PlaybackState::Playing && stats.buffering;
            if (!nativeBuffering) {
                if (nativeSeekPrerollHoldingAudio_ && !stats.seekRecoveryAudioHandoffReady) {
                    controller_.SyncClock(stats.clockPosition);
                } else {
                    ResumeNativeSeekPrerollAudio(stats.clockPosition);
                }
            }
            const bool shouldRenderFrame =
                (snapshot.state == PlaybackState::Playing || pendingPausedFrameRefresh_) &&
                !nativeBuffering;
            const bool submittedLatestFrame =
                shouldRenderFrame && d3dRenderer_ &&
                nativeVideoDecoder_->VisitLatestFrame([&](const NativeVideoFrame& frame) {
                    d3dRenderer_->Render(frame);
                    (void)ReplaceHeldNativeFrame(frame);
                });
            if (submittedLatestFrame) {
                heldNativeFrameNeedsPresent_ = false;
                nativeFrameHoldVisible_ = false;
                MaybeLogNativeSchedulerStats(stats);
                if (pendingPausedFrameRefresh_) {
                    pendingPausedFrameRefresh_ = false;
                    nativeFrameHoldVisible_ = true;
                    MarkLayoutDirty();
                    EnsureLayout();
                    RenderHeldNativeFrame();
                    LogApp(LogLevel::Debug, L"paused native frame refresh completed");
                }
                if (webUiActive_ && webUiPlayerRouteActive_) {
                    UpdateVideoHost();
                }
            } else if (nativeBuffering) {
                MaybeLogNativeSchedulerStats(stats);
                if (webUiActive_ && webUiPlayerRouteActive_) {
                    UpdateVideoHost();
                }
                UpdateBufferingOverlay(&stats);
            }
            if (snapshot.state == PlaybackState::Playing) {
                RenderPlaybackTick(snapshot, false);
            }
        }
        return 0;
    }
    case kNativeVideoDecodeFailedMessage: {
        std::unique_ptr<NativeDecodeFailure> failure(reinterpret_cast<NativeDecodeFailure*>(lParam));
        const std::wstring decodeMessage = failure && !failure->message.empty()
                                               ? failure->message
                                               : L"native video decoder failed";
        const auto snapshot = controller_.Snapshot();
        if (!snapshot.media.has_value() ||
            (failure && (failure->windowCookie != windowLifetimeCookie_ ||
                         snapshot.media->path != failure->path))) {
            LogApp(LogLevel::Debug, L"ignored stale native decode failure message=" + decodeMessage);
            return 0;
        }

        if (failure && TryRecoverNativeSeekFailure(*failure, decodeMessage)) {
            return 0;
        }

        LogApp(LogLevel::Error, L"native decode failed message=" + decodeMessage);
        FailPlaybackRuntime(decodeMessage);
        return 0;
    }
    case kRuntimeStopCompleteMessage:
        if (static_cast<uint64_t>(wParam) != windowLifetimeCookie_ ||
            !runtimeStopWorkerDone_.load()) {
            return 0;
        }
        CompleteAsyncRuntimeStop();
        if (closePending_) {
            TryFinishClose();
        } else {
            ContinueRuntimeAfterAsyncStop();
        }
        return 0;
    case kOpenPathMessage: {
        std::unique_ptr<std::filesystem::path> path(reinterpret_cast<std::filesystem::path*>(lParam));
        if (path && !path->empty()) {
            OpenPath(*path, wParam != 0);
        }
        return 0;
    }
    case kOpenMediaCompleteMessage: {
        PollPlaybackSupervisorCompletion();
        return 0;
    }
    case kPlaybackSupervisorStoppedMessage:
        if (static_cast<uint64_t>(wParam) != windowLifetimeCookie_) {
            return 0;
        }
        TryFinishClose();
        return 0;
    case kRecentMediaLoadCompleteMessage:
        if (recentMediaWriter_) {
            std::vector<std::filesystem::path> loadedItems;
            bool accepted = false;
            {
                std::scoped_lock lock(recentMediaWriter_->mutex);
                if (recentMediaWriter_->completedLoadGeneration ==
                    static_cast<uint64_t>(wParam)) {
                    loadedItems.swap(recentMediaWriter_->loadedItems);
                    accepted = true;
                }
            }
            if (accepted) {
                const bool mergeWithNewerItems = !recentMedia_.empty();
                if (!mergeWithNewerItems) {
                    recentMedia_.swap(loadedItems);
                } else {
                    for (auto& item : loadedItems) {
                        if (recentMedia_.size() >= kMaxRecentMedia) {
                            break;
                        }
                        if (std::find(recentMedia_.begin(), recentMedia_.end(), item) ==
                            recentMedia_.end()) {
                            recentMedia_.push_back(std::move(item));
                        }
                    }
                    SaveRecentMedia();
                }
                LogApp(LogLevel::Debug,
                       L"recent media loaded count=" + std::to_wstring(recentMedia_.size()));
                MarkLayoutDirty();
                EnsureLayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
                PostWebUiState();
            }
        }
        return 0;
    case kInspectorFolderScanCompleteMessage:
        if (inspectorFolderScan_ &&
            static_cast<uint64_t>(wParam) == inspectorFolderScanGeneration_) {
            bool accepted = false;
            {
                std::scoped_lock lock(inspectorFolderScan_->mutex);
                if (inspectorFolderScan_->completedGeneration ==
                    static_cast<uint64_t>(wParam)) {
                    currentFolderEntries_.swap(inspectorFolderScan_->entries);
                    accepted = true;
                }
            }
            if (accepted) {
                MarkLayoutDirty();
                EnsureLayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
                PostWebUiState();
            }
        }
        return 0;
    case kRenderThreadStoppedMessage:
        if (static_cast<uint64_t>(wParam) != windowLifetimeCookie_) {
            return 0;
        }
        TryFinishClose();
        return 0;
    case kRenderInitializationCompleteMessage: {
        if (static_cast<uint64_t>(wParam) != windowLifetimeCookie_ || !d3dRenderer_) {
            return 0;
        }
        CompleteRendererInitialization(static_cast<D3D11RendererState>(lParam));
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
        UpdateFramelessWindowRegion(hwnd_, dpi_, fullscreen_);
        MarkLayoutDirty();
        EnsureLayout();
        if (webUiActive_ && webUiHost_) {
            RECT client{};
            GetClientRect(hwnd_, &client);
            webUiHost_->Resize(client);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        UpdateBufferingOverlay();
        UpdateFullscreenOverlay();
        return 0;
    case WM_MOUSEMOVE:
        if (!trackingMouseLeave_) {
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd_;
            trackingMouseLeave_ = TrackMouseEvent(&event) != FALSE;
        }
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        OnMouseLeave();
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
    case WM_SYSKEYDOWN:
        if (wParam == VK_ESCAPE) {
            OnKeyDown(wParam);
            return 0;
        }
        break;
    case WM_DROPFILES:
        OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kPlaybackTimerTickMessage:
        OnPlaybackTimerTick();
        return 0;
    case kNativeColorSettingsRefreshMessage:
        if (static_cast<UINT_PTR>(wParam) == nativeColorSettingsRefreshSerial_) {
            const bool requiresDecoderRefresh = nativeColorSettingsRefreshRequiresDecoderRefresh_;
            nativeColorSettingsRefreshRequiresDecoderRefresh_ = false;
            ApplyNativeColorSettingsRefresh(requiresDecoderRefresh);
        }
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
        if (wParam == kScrollbarAutoHideTimer) {
            KillTimer(hwnd_, kScrollbarAutoHideTimer);
            InvalidateTransportArea();
            InvalidateFullscreenOverlay();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wParam == kAsyncCompletionPollTimer) {
            if (runtimeStopRetryPending_ && !RuntimeStopInProgress()) {
                StopRuntimeAsync(runtimeStopAsyncClearFrame_.load());
            }
            if (RuntimeStopInProgress() && runtimeStopWorkerDone_.load()) {
                CompleteAsyncRuntimeStop();
                if (!closePending_) {
                    ContinueRuntimeAfterAsyncStop();
                }
            }
            if (closePending_) {
                TryFinishClose();
            } else if (!RuntimeStopInProgress() && !runtimeStopRetryPending_) {
                KillTimer(hwnd_, kAsyncCompletionPollTimer);
            }
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
    case WM_CLOSE:
        if (closeReady_) {
            DestroyWindow(hwnd_);
        } else {
            BeginClose();
        }
        return 0;
    case WM_DESTROY:
        SetPlaybackTimer(false);
        KillTimer(hwnd_, kPlaybackTimer);
        KillTimer(hwnd_, kUiAnimationTimer);
        KillTimer(hwnd_, kFullscreenChromeHideTimer);
        KillTimer(hwnd_, kVideoPressTimer);
        KillTimer(hwnd_, kScrollbarAutoHideTimer);
        KillTimer(hwnd_, kAsyncCompletionPollTimer);
        RevokeMediaDropTarget();
        DragAcceptFiles(hwnd_, FALSE);
        if (hdrToneCurveWindow_) {
            DestroyWindow(hdrToneCurveWindow_);
            hdrToneCurveWindow_ = nullptr;
        }
        ReleaseUiThreadResourcesForBackgroundDestruction();
        if (quitHandler_) {
            quitHandler_();
        } else {
            PostQuitMessage(0);
        }
        return 0;
    case WM_NCDESTROY: {
        const HWND destroyedWindow = hwnd_;
        SetWindowLongPtrW(destroyedWindow, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return DefWindowProcW(destroyedWindow, message, wParam, lParam);
    }
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
        if (HasHdrToneCurveSelection()) {
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

std::filesystem::path MainWindow::RecentMediaPath() {
    return AppDataStorageFolder() / L"recent-media.txt";
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
                    L" timeline_serial=" + std::to_wstring(stats.timelineSerial) +
                    L" queue_depth=" + std::to_wstring(stats.queueDepth) +
                    L" packet_depth=" + std::to_wstring(stats.packetQueueDepth) +
                    L" packet_mb=" + std::to_wstring(stats.packetQueueBytes / (1024 * 1024)) +
                    L" buffered_end=" + FormatTimecode(stats.bufferedEnd) +
                    L" buffered_ms=" + std::to_wstring(stats.bufferedDuration.count()) +
                    L" read_ahead_ms=" + std::to_wstring(stats.readAheadDuration.count()) +
                    L" buffering=" + std::wstring(stats.buffering ? L"true" : L"false") +
                    L" net_kbps=" + std::to_wstring(stats.networkBytesPerSecond / 1024) +
                    L" rendered=" + std::to_wstring(stats.rendered) +
                   L" hardware_frames=" + std::to_wstring(stats.hardwareFrames) +
                   L" zero_copy_frames=" + std::to_wstring(stats.zeroCopyFrames) +
                   L" cpu_transfer_frames=" + std::to_wstring(stats.cpuTransferFrames) +
                   L" dropped_late=" + std::to_wstring(stats.droppedLate) +
                   L" dropped_stale=" + std::to_wstring(stats.droppedStale) +
                   L" dropped_superseded=" + std::to_wstring(stats.droppedSuperseded) +
                   L" dropped_queue_full=" + std::to_wstring(stats.droppedQueueFull) +
                   L" cadence_ms=" + std::to_wstring(stats.frameCadenceMs) +
                   L" early_tolerance_ms=" + std::to_wstring(stats.earlyToleranceMs) +
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
            L" subtitle_rebuilds=" + std::to_wstring(renderStats.subtitleSurfaceRebuilds) +
            L" subtitle_bitmap_rects=" + std::to_wstring(renderStats.subtitleBitmapRects) +
            L" subtitle_bitmap_mpixels=" +
                FormatFixed2(static_cast<double>(renderStats.subtitleBitmapPixels) / 1000000.0);
    }
    if (renderStats.presentSyncFrames > 0 ||
        renderStats.frameLatencyWaits > 0 ||
        renderStats.frameStatsSamples > 0 ||
        renderStats.frameStatsDisjoint > 0) {
        renderMessage +=
            L" present_sync_frames=" + std::to_wstring(renderStats.presentSyncFrames) +
            L" frame_latency_waits=" + std::to_wstring(renderStats.frameLatencyWaits) +
            L" frame_latency_wait_avg_ms=" +
                FormatAverageMilliseconds(renderStats.frameLatencyWaitUs, renderStats.frameLatencyWaits) +
            L" frame_latency_wait_max_ms=" + FormatMillisecondsFromMicroseconds(renderStats.maxFrameLatencyWaitUs) +
            L" frame_latency_wait_timeouts=" + std::to_wstring(renderStats.frameLatencyWaitTimeouts) +
            L" frame_stats_samples=" + std::to_wstring(renderStats.frameStatsSamples) +
            L" frame_stats_disjoint=" + std::to_wstring(renderStats.frameStatsDisjoint);
    }

    LogRuntime(LogLevel::Debug, L"renderer", renderMessage);
}

void MainWindow::ResetNativeBufferingWatchdog() {
    nativeBufferingStartedAt_ = {};
    nativeBufferingLastProgressAt_ = {};
    nativeBufferingLastRendered_ = 0;
    nativeBufferingLastQueueDepth_ = 0;
    nativeBufferingLastPacketDepth_ = 0;
    nativeBufferingLastPacketBytes_ = 0;
    nativeBufferingLastReadAhead_ = std::chrono::milliseconds{0};
    nativeBufferingLastBufferedEnd_ = std::chrono::milliseconds{0};
    nativeBufferingLastClockPosition_ = std::chrono::milliseconds{0};
}

bool MainWindow::CheckNativeBufferingWatchdog(const PlaybackSessionSnapshot& snapshot,
                                              const NativeVideoQueueStats& stats) {
    if (snapshot.state != PlaybackState::Playing || !stats.buffering) {
        ResetNativeBufferingWatchdog();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool firstSample = nativeBufferingStartedAt_.time_since_epoch().count() == 0;
    const bool progressed =
        firstSample ||
        stats.rendered != nativeBufferingLastRendered_ ||
        stats.queueDepth != nativeBufferingLastQueueDepth_ ||
        stats.packetQueueDepth != nativeBufferingLastPacketDepth_ ||
        stats.packetQueueBytes != nativeBufferingLastPacketBytes_ ||
        stats.readAheadDuration != nativeBufferingLastReadAhead_ ||
        stats.bufferedEnd != nativeBufferingLastBufferedEnd_ ||
        stats.clockPosition != nativeBufferingLastClockPosition_;

    if (firstSample) {
        nativeBufferingStartedAt_ = now;
    }
    if (progressed) {
        nativeBufferingLastProgressAt_ = now;
        nativeBufferingLastRendered_ = stats.rendered;
        nativeBufferingLastQueueDepth_ = stats.queueDepth;
        nativeBufferingLastPacketDepth_ = stats.packetQueueDepth;
        nativeBufferingLastPacketBytes_ = stats.packetQueueBytes;
        nativeBufferingLastReadAhead_ = stats.readAheadDuration;
        nativeBufferingLastBufferedEnd_ = stats.bufferedEnd;
        nativeBufferingLastClockPosition_ = stats.clockPosition;
        return false;
    }

    const auto waitingFor = now - nativeBufferingStartedAt_;
    const auto noProgressFor = nativeBufferingLastProgressAt_.time_since_epoch().count() == 0
                                   ? waitingFor
                                   : now - nativeBufferingLastProgressAt_;
    if (waitingFor < kNativeBufferingHardTimeout &&
        noProgressFor < kNativeBufferingNoProgressTimeout) {
        return false;
    }

    std::wstring message =
        L"Playback stalled while waiting for native frames at " +
        FormatTimecode(stats.clockPosition) +
        L" decoder=" + stats.decoder +
        L" rendered=" + std::to_wstring(stats.rendered) +
        L" queue=" + std::to_wstring(stats.queueDepth) +
        L" packets=" + std::to_wstring(stats.packetQueueDepth) +
        L" read_ahead_ms=" + std::to_wstring(stats.readAheadDuration.count()) +
        L" wait_ms=" + std::to_wstring(
            std::chrono::duration_cast<std::chrono::milliseconds>(waitingFor).count()) +
        L" no_progress_ms=" + std::to_wstring(
            std::chrono::duration_cast<std::chrono::milliseconds>(noProgressFor).count());
    FailPlaybackRuntime(message);
    return true;
}

void MainWindow::FailPlaybackRuntime(const std::wstring& message) {
    LogApp(LogLevel::Error, L"playback runtime failed message=" + message);
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    StopRuntimeAsync(true);
    controller_.SetError(message);
    SetPlaybackTimer(false);
    ResetNativeBufferingWatchdog();
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

bool MainWindow::TryRecoverNativeSeekFailure(const NativeDecodeFailure& failure, const std::wstring& message) {
    if (message.rfind(L"Runtime seek failed at ", 0) != 0) {
        return false;
    }

    const auto snapshot = controller_.Snapshot();
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        snapshot.media->path != failure.path ||
        !IsNetworkMediaPath(snapshot.media->path)) {
        return false;
    }

    LogApp(LogLevel::Warning,
           L"recovering native runtime after network seek failure position=" +
               FormatTimecode(snapshot.position) +
               L" message=" + message);
    nativeSeekPrerollHoldingAudio_ = false;
    ResetNativeBufferingWatchdog();
    RequestRuntimeStart(true, false);
    SetPlaybackTimer(true);
    return true;
}

void MainWindow::StartUiAnimationTimer() const {
    if (hwnd_) {
        SetTimer(hwnd_, kUiAnimationTimer, kUiAnimationTimerMs, nullptr);
    }
}

bool MainWindow::SidebarAnimationActive() const {
    return !fullscreen_ && std::abs(inspectorCollapseAmount_ - inspectorCollapseTarget_) > 0.001;
}

void MainWindow::SetButtonHoverTarget(const int buttonIndex, const bool hovered) {
    if (buttonIndex < 0 || buttonIndex >= static_cast<int>(buttons_.size())) {
        return;
    }

    const std::size_t slot = static_cast<std::size_t>(buttons_[static_cast<std::size_t>(buttonIndex)].command);
    if (slot >= buttonHoverAnimations_.size()) {
        return;
    }

    UiMotionValue& animation = buttonHoverAnimations_[slot];
    const double target = hovered ? 1.0 : 0.0;
    if (std::abs(animation.target - target) < 0.001) {
        return;
    }

    if (UsesGpuFullscreenUiOverlay()) {
        animation.amount = target;
        animation.startAmount = target;
        animation.target = target;
        InvalidateTransportArea();
        return;
    }

    animation.startAmount = animation.amount;
    animation.target = target;
    animation.startedAt = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ClearButtonHoverTargets() {
    if (UsesGpuFullscreenUiOverlay()) {
        bool changed = false;
        for (auto& animation : buttonHoverAnimations_) {
            changed = changed || animation.amount > 0.001 || animation.target > 0.001;
            animation.amount = 0.0;
            animation.startAmount = 0.0;
            animation.target = 0.0;
        }
        if (changed) {
            InvalidateTransportArea();
        }
        return;
    }

    bool changed = false;
    const auto now = std::chrono::steady_clock::now();
    for (auto& animation : buttonHoverAnimations_) {
        if (animation.target <= 0.001 && animation.amount <= 0.001) {
            continue;
        }
        animation.startAmount = animation.amount;
        animation.target = 0.0;
        animation.startedAt = now;
        changed = true;
    }
    if (!changed) {
        return;
    }

    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::TriggerButtonPress(const int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex >= static_cast<int>(buttons_.size())) {
        return;
    }

    const std::size_t slot = static_cast<std::size_t>(buttons_[static_cast<std::size_t>(buttonIndex)].command);
    if (slot >= buttonPressAnimations_.size()) {
        return;
    }

    UiMotionValue& animation = buttonPressAnimations_[slot];
    if (UsesGpuFullscreenUiOverlay()) {
        animation = {};
        return;
    }
    animation.amount = 1.0;
    animation.startAmount = 1.0;
    animation.target = 0.0;
    animation.startedAt = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

double MainWindow::ButtonHoverAmount(const Command command) const {
    const std::size_t slot = static_cast<std::size_t>(command);
    if (slot >= buttonHoverAnimations_.size()) {
        return 0.0;
    }
    return std::clamp(buttonHoverAnimations_[slot].amount, 0.0, 1.0);
}

double MainWindow::ButtonPressAmount(const Command command) const {
    const std::size_t slot = static_cast<std::size_t>(command);
    if (slot >= buttonPressAnimations_.size()) {
        return 0.0;
    }
    return std::clamp(buttonPressAnimations_[slot].amount, 0.0, 1.0);
}

void MainWindow::SetVolumeSliderHover(const bool hovered) {
    if (volumeSliderHovered_ == hovered && volumeHoverTarget_ == (hovered ? 1.0 : 0.0)) {
        return;
    }

    volumeSliderHovered_ = hovered;
    if (UsesGpuFullscreenUiOverlay()) {
        volumeHoverAmount_ = hovered ? 1.0 : 0.0;
        volumeHoverStartAmount_ = volumeHoverAmount_;
        volumeHoverTarget_ = volumeHoverAmount_;
        InvalidateTransportArea();
        return;
    }
    volumeHoverStartAmount_ = volumeHoverAmount_;
    volumeHoverTarget_ = hovered ? 1.0 : 0.0;
    volumeHoverAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
}

void MainWindow::UpdateUiAnimations() {
    const auto now = std::chrono::steady_clock::now();
    const bool gpuFullscreenUiOverlay = UsesGpuFullscreenUiOverlay();
    const RECT previousVideoSurface = videoSurface_;
    const RECT previousInspector = inspector_;
    const double previousInspectorCollapseAmount = inspectorCollapseAmount_;
    const double previousProgressHoverAmount = progressHoverAmount_;
    const double previousFullscreenTransportAmount = fullscreenTransportAmount_;
    const double previousSubtitleMenuAmount = subtitleMenuAmount_;
    const double previousVolumeHoverAmount = volumeHoverAmount_;
    bool sidebarComplete = true;
    bool hoverComplete = true;
    bool fullscreenTransportComplete = true;
    bool subtitleMenuComplete = true;
    bool volumeHoverComplete = true;
    bool commandAnimationsComplete = true;
    const bool sidebarWasActive = SidebarAnimationActive();

    inspectorCollapseAmount_ = AnimatedValue(inspectorCollapseStartAmount_,
                                             inspectorCollapseTarget_,
                                             inspectorAnimationStartedAt_,
                                             kSidebarAnimationDuration,
                                             now,
                                             MotionCurve::Fluid,
                                             sidebarComplete);
    progressHoverAmount_ = AnimatedValue(progressHoverStartAmount_,
                                         progressHoverTarget_,
                                         progressHoverAnimationStartedAt_,
                                         kProgressHoverAnimationDuration,
                                         now,
                                         MotionCurve::Press,
                                         hoverComplete);
    fullscreenTransportAmount_ = AnimatedValue(fullscreenTransportStartAmount_,
                                               fullscreenTransportTarget_,
                                               fullscreenTransportAnimationStartedAt_,
                                               kFullscreenTransportAnimationDuration,
                                               now,
                                               MotionCurve::Fluid,
                                               fullscreenTransportComplete);
    const auto subtitleMenuDuration = webUiActive_
                                          ? (subtitleMenuTarget_ > subtitleMenuStartAmount_
                                                 ? kWebUiSubtitleMenuOpenAnimationDuration
                                                 : kWebUiSubtitleMenuCloseAnimationDuration)
                                          : kSubtitleMenuAnimationDuration;
    const MotionCurve subtitleMenuCurve = webUiActive_ &&
                                                  subtitleMenuTarget_ <= subtitleMenuStartAmount_
                                              ? MotionCurve::EaseIn
                                              : MotionCurve::Fluid;
    subtitleMenuAmount_ = AnimatedValue(subtitleMenuStartAmount_,
                                        subtitleMenuTarget_,
                                        subtitleMenuAnimationStartedAt_,
                                        subtitleMenuDuration,
                                        now,
                                        subtitleMenuCurve,
                                        subtitleMenuComplete);
    volumeHoverAmount_ = AnimatedValue(volumeHoverStartAmount_,
                                       volumeHoverTarget_,
                                       volumeHoverAnimationStartedAt_,
                                       kVolumeHoverAnimationDuration,
                                       now,
                                       MotionCurve::Press,
                                       volumeHoverComplete);
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
        if (subtitleMenuTarget_ <= 0.001) {
            subtitleMenuOpen_ = false;
        }
    }
    if (volumeHoverComplete) {
        volumeHoverAmount_ = volumeHoverTarget_;
    }

    bool commandAnimationValueChanged = false;
    for (auto& animation : buttonHoverAnimations_) {
        bool complete = true;
        commandAnimationValueChanged =
            UpdateMotionValue(animation, kButtonHoverAnimationDuration, now, MotionCurve::Press, complete) ||
            commandAnimationValueChanged;
        commandAnimationsComplete = commandAnimationsComplete && complete;
    }
    for (auto& animation : buttonPressAnimations_) {
        bool complete = true;
        commandAnimationValueChanged =
            UpdateMotionValue(animation, kButtonPressAnimationDuration, now, MotionCurve::Press, complete) ||
            commandAnimationValueChanged;
        commandAnimationsComplete = commandAnimationsComplete && complete;
    }

    const bool sidebarValueChanged = std::abs(inspectorCollapseAmount_ - previousInspectorCollapseAmount) > 0.0001;
    const bool hoverValueChanged = std::abs(progressHoverAmount_ - previousProgressHoverAmount) > 0.0001;
    const bool fullscreenTransportValueChanged =
        std::abs(fullscreenTransportAmount_ - previousFullscreenTransportAmount) > 0.0001;
    const bool subtitleMenuValueChanged = std::abs(subtitleMenuAmount_ - previousSubtitleMenuAmount) > 0.0001;
    const bool volumeHoverValueChanged = std::abs(volumeHoverAmount_ - previousVolumeHoverAmount) > 0.0001;
    const bool scrollbarFadeActive = ScrollbarFadeActive(now);

    if (sidebarValueChanged ||
        fullscreenTransportValueChanged ||
        (subtitleMenuValueChanged && (!gpuFullscreenUiOverlay || subtitleMenuComplete))) {
        MarkLayoutDirty();
        EnsureLayout();
    }

    const int invalidationPadding = Scale(4);
    const auto noMediaPlaceholderRect = [this](const RECT& surface) {
        if (!HasArea(surface)) {
            return RECT{};
        }
        const bool compactSurface = RectWidth(surface) < Scale(720) || RectHeight(surface) < Scale(280);
        const RECT inner = DeflateRectCopy(surface,
                                           compactSurface ? Scale(18) : Scale(24),
                                           compactSurface ? Scale(16) : Scale(22));
        const int centerX = inner.left + RectWidth(inner) / 2;
        const int centerY = inner.top + RectHeight(inner) / 2;
        const int width = std::min(RectWidth(inner), compactSurface ? Scale(280) : Scale(340));
        const int topOffset = compactSurface ? Scale(88) : Scale(104);
        const int bottomOffset = compactSurface ? Scale(56) : Scale(64);
        return MakeRect(centerX - width / 2, centerY - topOffset, centerX + width / 2, centerY + bottomOffset);
    };

    if (hoverValueChanged) {
        InvalidateTransportArea();
    }
    if (volumeHoverValueChanged || commandAnimationValueChanged) {
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (fullscreenTransportValueChanged) {
        InvalidateFullscreenOverlay();
    }
    if (subtitleMenuValueChanged) {
        if (gpuFullscreenUiOverlay) {
            UpdateGpuFullscreenSubtitleMenuPresentation();
            if (subtitleMenuComplete && subtitleMenuTarget_ <= 0.001) {
                QueueGpuFullscreenSubtitleMenuOverlay();
                // The transport subtitle button is intentionally omitted
                // while the menu is layered over it. Restore it once the
                // closing animation has completely vacated the footer.
                InvalidateTransportArea();
            }
        } else {
            UpdateSubtitleMenuOverlay();
            InvalidateTransportArea();
            InvalidateFullscreenOverlay();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    if (scrollbarFadeActive) {
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (!fullscreen_) {
        const bool inspectorGeometryChanged = !SameRect(previousInspector, inspector_);
        const bool videoGeometryChanged = !SameRect(previousVideoSurface, videoSurface_);
        if (inspectorGeometryChanged || videoGeometryChanged) {
            InvalidateIfVisible(hwnd_, previousInspector, invalidationPadding);
            InvalidateIfVisible(hwnd_, inspector_, invalidationPadding);
            if (videoGeometryChanged) {
                const auto snapshot = controller_.Snapshot();
                InvalidateIfVisible(hwnd_, SidebarEdgeRect(previousVideoSurface, videoSurface_), invalidationPadding);
                if (!snapshot.media.has_value()) {
                    InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(previousVideoSurface), invalidationPadding);
                    InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(videoSurface_), invalidationPadding);
                }
            }
        }
    }

    if (sidebarWasActive && sidebarComplete) {
        UpdateVideoHost();
        if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning() &&
            d3dRenderer_) {
            if (nativeVideoDecoder_->VisitLatestFrame([&](const NativeVideoFrame& frame) {
                    d3dRenderer_->Render(frame);
                    (void)ReplaceHeldNativeFrame(frame);
                })) {
                heldNativeFrameNeedsPresent_ = false;
                nativeFrameHoldVisible_ = false;
            }
        }
        InvalidateVideoSurface();
    }

    if (sidebarComplete &&
        hoverComplete &&
        fullscreenTransportComplete &&
        subtitleMenuComplete &&
        volumeHoverComplete &&
        commandAnimationsComplete &&
        !scrollbarFadeActive) {
        KillTimer(hwnd_, kUiAnimationTimer);
    } else {
        StartUiAnimationTimer();
    }
}

void MainWindow::SetSubtitleMenuTarget(const bool visible) {
    const RECT oldMenu = subtitleMenu_;
    const double target = visible ? 1.0 : 0.0;
    if (std::abs(subtitleMenuTarget_ - target) < 0.001 &&
        std::abs(subtitleMenuAmount_ - target) < 0.001) {
        return;
    }

    subtitleMenuOpen_ = visible || subtitleMenuAmount_ > 0.001;
    if (UsesGpuFullscreenUiOverlay()) {
        subtitleMenuStartAmount_ = subtitleMenuAmount_;
        subtitleMenuTarget_ = target;
        subtitleMenuAnimationStartedAt_ = std::chrono::steady_clock::now();
        if (!visible) {
            hoveredSubtitleMenuItem_ = -1;
        }
        StartUiAnimationTimer();
        MarkLayoutDirty();
        EnsureLayout();
        UpdateVideoHost();
        UpdateSubtitleMenuOverlay();
        InvalidateTransportArea();
        QueueGpuFullscreenSubtitleMenuOverlay(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostWebUiState();
        return;
    }
    subtitleMenuStartAmount_ = subtitleMenuAmount_;
    subtitleMenuTarget_ = target;
    subtitleMenuAnimationStartedAt_ = std::chrono::steady_clock::now();
    if (!visible) {
        hoveredSubtitleMenuItem_ = -1;
    }
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    UpdateVideoHost();
    UpdateSubtitleMenuOverlay();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    if (!visible && RectWidth(oldMenu) > 0 && RectHeight(oldMenu) > 0) {
        InvalidateRect(hwnd_, &oldMenu, FALSE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
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
    if (UsesGpuFullscreenUiOverlay()) {
        progressHoverAmount_ = hovered ? 1.0 : 0.0;
        progressHoverStartAmount_ = progressHoverAmount_;
        progressHoverTarget_ = progressHoverAmount_;
        InvalidateTransportArea();
        return;
    }
    progressHoverStartAmount_ = progressHoverAmount_;
    progressHoverTarget_ = hovered ? 1.0 : 0.0;
    progressHoverAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    InvalidateTransportArea();
}

void MainWindow::ToggleSidebar() {
    const RECT previousVideoSurface = videoSurface_;
    const RECT previousInspector = inspector_;
    const RECT previousTopBar = topBar_;
    const auto snapshot = controller_.Snapshot();
    inspectorCollapsed_ = !inspectorCollapsed_;
    inspectorCollapseStartAmount_ = inspectorCollapseAmount_;
    inspectorCollapseTarget_ = inspectorCollapsed_ ? 1.0 : 0.0;
    inspectorAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    const int invalidationPadding = Scale(4);
    InvalidateIfVisible(hwnd_, previousTopBar, invalidationPadding);
    InvalidateIfVisible(hwnd_, topBar_, invalidationPadding);
    InvalidateIfVisible(hwnd_, previousInspector, invalidationPadding);
    InvalidateIfVisible(hwnd_, inspector_, invalidationPadding);
    InvalidateIfVisible(hwnd_, SidebarEdgeRect(previousVideoSurface, videoSurface_), invalidationPadding);
    if (!snapshot.media.has_value()) {
        const auto noMediaPlaceholderRect = [this](const RECT& surface) {
            if (!HasArea(surface)) {
                return RECT{};
            }
            const bool compactSurface = RectWidth(surface) < Scale(720) || RectHeight(surface) < Scale(280);
            const RECT inner = DeflateRectCopy(surface,
                                               compactSurface ? Scale(18) : Scale(24),
                                               compactSurface ? Scale(16) : Scale(22));
            const int centerX = inner.left + RectWidth(inner) / 2;
            const int centerY = inner.top + RectHeight(inner) / 2;
            const int width = std::min(RectWidth(inner), compactSurface ? Scale(280) : Scale(340));
            const int topOffset = compactSurface ? Scale(88) : Scale(104);
            const int bottomOffset = compactSurface ? Scale(56) : Scale(64);
            return MakeRect(centerX - width / 2, centerY - topOffset, centerX + width / 2, centerY + bottomOffset);
        };
        InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(previousVideoSurface), invalidationPadding);
        InvalidateIfVisible(hwnd_, noMediaPlaceholderRect(videoSurface_), invalidationPadding);
    }
    PostWebUiState();
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
        ShowFullscreenTransport(L"cursor_poll_activation");
    }
}

std::wstring MainWindow::FullscreenTransportDebugState(const wchar_t* reason) const {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << L" reason=" << (reason ? reason : L"unspecified")
           << L" target=" << fullscreenTransportTarget_
           << L" amount=" << fullscreenTransportAmount_;
    if (fullscreenTransportLastShownAt_.time_since_epoch().count() != 0) {
        const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fullscreenTransportLastShownAt_).count();
        stream << L" idle_ms=" << idleMs;
    } else {
        stream << L" idle_ms=none";
    }
    if (hasLastFullscreenCursorClient_) {
        const bool activation = IsFullscreenTransportActivationPoint(lastFullscreenCursorClient_);
        const bool transportHit = ContainsPoint(transportBar_, lastFullscreenCursorClient_);
        const bool menuHit = IsPointInSubtitleMenu(lastFullscreenCursorClient_);
        stream << L" cursor=" << lastFullscreenCursorClient_.x << L"," << lastFullscreenCursorClient_.y
               << L" activation=" << (activation ? L"true" : L"false")
               << L" transport_hit=" << (transportHit ? L"true" : L"false")
               << L" menu_hit=" << (menuHit ? L"true" : L"false");
    } else {
        stream << L" cursor=none";
    }
    stream << L" dragging_progress=" << (draggingProgress_ ? L"true" : L"false")
           << L" dragging_volume=" << (draggingVolume_ ? L"true" : L"false")
           << L" subtitle_menu=" << ((subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) ? L"true" : L"false");
    return stream.str();
}

void MainWindow::ShowFullscreenTransport(const wchar_t* reason) {
    if (!fullscreen_) {
        return;
    }

    if (fullscreenTransportTarget_ <= 0.001 && fullscreenTransportAmount_ <= 0.001) {
        LogApp(LogLevel::Debug, L"fullscreen_transport show" + FullscreenTransportDebugState(reason));
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
        if (fullscreenTransportTarget_ <= 0.001) {
            LogApp(LogLevel::Debug,
                   L"fullscreen_transport keep" +
                       FullscreenTransportDebugState(draggingProgress_ ? L"dragging_progress" : L"dragging_volume"));
        }
        SetFullscreenTransportTarget(true);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) {
        if (fullscreenTransportTarget_ <= 0.001) {
            LogApp(LogLevel::Debug, L"fullscreen_transport keep" + FullscreenTransportDebugState(L"subtitle_menu"));
        }
        SetFullscreenTransportTarget(true);
        fullscreenTransportLastShownAt_ = now;
        return;
    }

    if (fullscreenTransportLastShownAt_.time_since_epoch().count() != 0 &&
        now - fullscreenTransportLastShownAt_ < kFullscreenTransportHideDelay) {
        return;
    }

    if (fullscreenTransportTarget_ > 0.001 || fullscreenTransportAmount_ > 0.001) {
        LogApp(LogLevel::Debug, L"fullscreen_transport hide" + FullscreenTransportDebugState(L"idle_timeout"));
    }
    SetFullscreenTransportTarget(false);
    ClearButtonHoverTargets();
    hoveredButton_ = -1;
    SetVolumeSliderHover(false);
    SetProgressHover(false);
}

void MainWindow::SetFullscreenTransportTarget(const bool visible) {
    fullscreenTransportVisible_ = visible;
    const double target = visible ? 1.0 : 0.0;
    if (std::abs(fullscreenTransportTarget_ - target) < 0.001 &&
        (!UsesGpuFullscreenUiOverlay() || std::abs(fullscreenTransportAmount_ - target) < 0.001)) {
        return;
    }

    if (UsesGpuFullscreenUiOverlay()) {
        fullscreenTransportAmount_ = target;
        fullscreenTransportStartAmount_ = target;
        fullscreenTransportTarget_ = target;
        MarkLayoutDirty();
        EnsureLayout();
        UpdateVideoHost();
        // Visibility transitions must not wait for the next decoded frame. A
        // stalled stream or a just-paused decoder may not produce one.
        QueueGpuFullscreenUiOverlay(true);
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostWebUiState();
        return;
    }

    fullscreenTransportStartAmount_ = fullscreenTransportAmount_;
    fullscreenTransportTarget_ = target;
    fullscreenTransportAnimationStartedAt_ = std::chrono::steady_clock::now();
    StartUiAnimationTimer();
    MarkLayoutDirty();
    EnsureLayout();
    UpdateVideoHost();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::MarkSettingsScrollbarActive() {
    settingsScrollLastActiveAt_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kScrollbarAutoHideTimer, kScrollbarAutoHideTimerMs, nullptr);
    StartUiAnimationTimer();
}

void MainWindow::MarkSubtitleMenuScrollbarActive() {
    subtitleMenuScrollLastActiveAt_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kScrollbarAutoHideTimer, kScrollbarAutoHideTimerMs, nullptr);
    StartUiAnimationTimer();
}

bool MainWindow::ScrollbarFadeActive(const std::chrono::steady_clock::time_point now) const {
    const auto active = [now](const std::chrono::steady_clock::time_point lastActiveAt) {
        return lastActiveAt.time_since_epoch().count() != 0 &&
               now - lastActiveAt < kScrollbarVisibleDuration + kScrollbarFadeDuration;
    };
    return draggingSettingsScrollThumb_ ||
           active(settingsScrollLastActiveAt_) ||
           active(subtitleMenuScrollLastActiveAt_);
}

void MainWindow::SetTemporaryPlaybackRate(const double rate) {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        audioPlayer_.SetPlaybackRate(1.0);
        systemDolbyVisionPlayer_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
        return;
    }

    const double clamped = std::clamp(rate, 0.25, 4.0);
    if (std::abs(snapshot.playbackRate - clamped) < 0.001) {
        temporaryRateActive_ = clamped > 1.01;
        audioPlayer_.SetPlaybackRate(clamped);
        systemDolbyVisionPlayer_.SetPlaybackRate(clamped);
        return;
    }

    controller_.SetPlaybackRate(clamped);
    audioPlayer_.SetPlaybackRate(clamped);
    systemDolbyVisionPlayer_.SetPlaybackRate(clamped);
    temporaryRateActive_ = clamped > 1.01;
    InvalidateTransportArea();
}

std::wstring MainWindow::RuntimeLabel() const {
    if (systemDolbyVisionPlayer_.IsActive()) {
        return L"Windows Media Foundation / Dolby Vision";
    }
    switch (backend_) {
    case PlaybackBackend::NativeFfmpegD3D11: return kNativeFfmpegD3D11RuntimeLabel;
    case PlaybackBackend::EmbeddedFfplay: return kExternalPlaybackRuntimeLabel;
    case PlaybackBackend::RawFrameBridge: return kInternalPlaybackRuntimeLabel;
    }
    return kNativeFfmpegD3D11RuntimeLabel;
}

std::wstring MainWindow::RuntimeShortLabel() const {
    if (systemDolbyVisionPlayer_.IsActive()) {
        return L"System Dolby Vision";
    }
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

    KillTimer(hwnd_, kPlaybackTimer);
    SetTimer(hwnd_,
             kPlaybackTimer,
             playing ? kPlayingPlaybackTimerMs : kIdlePlaybackTimerMs,
             nullptr);
}

void MainWindow::OnPlaybackTimerTick() {
    // WebView2 creates its input child windows asynchronously. Keep the native
    // file-drop target attached to newly-created player descendants as well as
    // the top-level window.
    RegisterMediaDropTarget();
    PollPlaybackSupervisorCompletion();
    if (hdrToneCurveExpanded_ && CurrentMediaIsHdr10Plus()) {
        HideHdrToneCurveWindow();
    }
    {
        auto settings = controller_.Settings();
        if (settings.video.displayMetadataPassthrough &&
            anvil::playback::CapabilityDetector::IsHdrEnabledNow() &&
            !settings.video.dolbyVisionHdrOutput) {
            settings.video.dolbyVisionHdrOutput = true;
            controller_.ApplySettings(settings);
            const auto current = controller_.Snapshot();
            if (current.media.has_value() && current.media->hasVideo) {
                ApplyNativeColorSettingsLive(current);
            }
            LogApp(LogLevel::Info, L"hdr output forced on by display metadata passthrough");
        }
    }
    if (d3dRenderer_ && !videoHostReady_) {
        const auto rendererState = d3dRenderer_->State();
        if (rendererState == D3D11RendererState::Ready ||
            rendererState == D3D11RendererState::Failed) {
            CompleteRendererInitialization(rendererState);
        }
    }
    bool nativeBuffering = false;
    bool nativeDecoderClockActive = false;
    const bool runtimeTransition = RuntimeStopInProgress() || deferredRuntimeStart_;
    const bool systemDolbyVisionActive = systemDolbyVisionPlayer_.IsActive();
    if (systemDolbyVisionActive && !runtimeTransition) {
        if (systemDolbyVisionPlayer_.HasFailed()) {
            systemDolbyVisionFallbackForCurrentMedia_ = true;
            LogApp(LogLevel::Warning,
                   L"system dolby vision playback failed; fallback=ffmpeg_hdr10 reason=" +
                       systemDolbyVisionPlayer_.LastError());
            QueueDeferredRuntimeStart(true, false);
            StopRuntimeAsync(true);
            return;
        }
        const auto systemSnapshot = controller_.Snapshot();
        if (systemDolbyVisionPlayer_.IsPlaying() && !systemDolbyVisionPlayingLogged_) {
            systemDolbyVisionPlayingLogged_ = true;
            LogApp(LogLevel::Info,
                   L"system dolby vision media engine event=playing position=" +
                       FormatTimecode(systemDolbyVisionPlayer_.Position()));
        } else if (systemSnapshot.state == PlaybackState::Playing &&
                   !systemDolbyVisionPlayer_.IsPlaying() &&
                   systemDolbyVisionStartedAt_.time_since_epoch().count() != 0 &&
                   std::chrono::steady_clock::now() - systemDolbyVisionStartedAt_ >
                       std::chrono::seconds{10}) {
            systemDolbyVisionFallbackForCurrentMedia_ = true;
            LogApp(LogLevel::Warning,
                   L"system dolby vision media engine startup timeout; fallback=ffmpeg_hdr10");
            QueueDeferredRuntimeStart(true, false);
            StopRuntimeAsync(true);
            return;
        }
        controller_.SyncClock(systemDolbyVisionPlayer_.Position());
        if (systemDolbyVisionPlayer_.HasEnded()) {
            controller_.UpdateClock();
        }
        nativeDecoderClockActive = true;
    }
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        !systemDolbyVisionActive &&
        nativeVideoDecoder_) {
        const auto current = controller_.Snapshot();
        if (current.state == PlaybackState::Playing &&
            current.media.has_value() &&
            current.media->hasVideo) {
            if (!nativeVideoDecoder_->IsRunning()) {
                const auto remaining =
                    current.media->duration.count() > 0
                        ? current.media->duration - current.position
                        : std::chrono::milliseconds{0};
                if (runtimeTransition) {
                    nativeBuffering = true;
                } else if (remaining > std::chrono::seconds{2} || current.media->duration.count() <= 0) {
                    FailPlaybackRuntime(L"Native video decoder stopped before playback completed at " +
                                        FormatTimecode(current.position));
                    return;
                }
            } else {
                const auto stats = nativeVideoDecoder_->Stats();
                nativeBuffering = stats.buffering;
                if (nativeBuffering) {
                    controller_.SyncClock(stats.clockPosition);
                    nativeDecoderClockActive = true;
                    if (CheckNativeBufferingWatchdog(current, stats)) {
                        return;
                    }
                } else {
                    ResetNativeBufferingWatchdog();
                    if (nativeSeekPrerollHoldingAudio_ && !stats.seekRecoveryAudioHandoffReady) {
                        controller_.SyncClock(stats.clockPosition);
                        nativeDecoderClockActive = true;
                    } else {
                        ResumeNativeSeekPrerollAudio(stats.clockPosition);
                    }
                }
            }
        }
    }
    if (!nativeBuffering && !runtimeTransition && !nativeDecoderClockActive) {
        const auto current = controller_.Snapshot();
        const auto settings = controller_.Settings();
        const bool holdForAudioStart =
            current.state == PlaybackState::Playing &&
            current.media.has_value() &&
            current.media->hasAudio &&
            backend_ != PlaybackBackend::EmbeddedFfplay &&
            settings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff &&
            audioPlayer_.IsStarting();
        if (holdForAudioStart) {
            if (const auto audioPosition = audioPlayer_.PlaybackClock()) {
                controller_.SyncClock(*audioPosition);
            }
        } else {
            controller_.UpdateClock();
        }
    }
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing) {
        if (runtimeStopAsyncInProgress_.load()) {
            PostWebUiState(false);
            return;
        }
        SetTemporaryPlaybackRate(1.0);
        if (snapshot.state == PlaybackState::Paused &&
            systemDolbyVisionPlayer_.IsActive()) {
            InvalidateRect(hwnd_, &transportBar_, FALSE);
            InvalidateFullscreenOverlay();
            PostWebUiState(false);
            return;
        }
        if (snapshot.state == PlaybackState::Paused &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning()) {
            MaybeLogNativeSchedulerStats(nativeVideoDecoder_->Stats());
            InvalidateRect(hwnd_, &transportBar_, FALSE);
            InvalidateFullscreenOverlay();
            UpdateBufferingOverlay();
            PostWebUiState(false);
            return;
        }
        RestoreAutomaticDisplayFormat();
        // Paused keeps the freeze frame captured by PausePlayback; other
        // stopped states clear the runtime frame as before.
        const bool keepFrame = (snapshot.state == PlaybackState::Paused);
        if (RuntimeBackendsRunning()) {
            StopRuntimeAsync(keepFrame ? false : true);
        } else {
            FinishRuntimeStopVisuals(keepFrame ? false : true);
        }
        SetPlaybackTimer(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostWebUiState();
    } else {
        if (snapshot.media.has_value()) {
            const auto remaining =
                snapshot.media->duration.count() > 0
                    ? snapshot.media->duration - snapshot.position
                    : std::chrono::milliseconds{0};
            const bool beforeEnd = remaining > std::chrono::seconds{2} ||
                                   snapshot.media->duration.count() <= 0;
            if (beforeEnd && !runtimeTransition) {
                if (backend_ == PlaybackBackend::EmbeddedFfplay &&
                    !playbackPlayer_.IsRunning()) {
                    FailPlaybackRuntime(L"External FFplay process exited before playback completed at " +
                                        FormatTimecode(snapshot.position));
                    return;
                }
                if (backend_ == PlaybackBackend::RawFrameBridge &&
                    snapshot.media->hasVideo &&
                    !videoDecoder_.IsRunning()) {
                    FailPlaybackRuntime(L"Internal video decoder stopped before playback completed at " +
                                        FormatTimecode(snapshot.position));
                    return;
                }

                const auto runtimeSettings = controller_.Settings();
                const bool audioRequired =
                    snapshot.media->hasAudio &&
                    runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff;
                const bool audioOwnedByNativeVideoDemuxer =
                    backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                    snapshot.media->hasVideo &&
                    snapshot.media->hasAudio &&
                    IsNetworkMediaPath(snapshot.media->path);
                if (audioRequired &&
                    !systemDolbyVisionPlayer_.IsActive() &&
                    backend_ != PlaybackBackend::EmbeddedFfplay &&
                    !audioPlayer_.IsRunning() &&
                    (!audioOwnedByNativeVideoDemuxer || audioPlayer_.HasStartFailed())) {
                    FailPlaybackRuntime(L"Audio renderer stopped before playback completed at " +
                                        FormatTimecode(snapshot.position));
                    return;
                }
            }
        }
        if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning() &&
            snapshot.media.has_value() &&
            snapshot.media->hasVideo) {
            const auto stats = nativeVideoDecoder_->Stats();
            MaybeLogNativeSchedulerStats(stats);
            if (webUiActive_ && webUiPlayerRouteActive_) {
                UpdateVideoHost();
            }
            UpdateBufferingOverlay(&stats);
        }
        RenderPlaybackTick(snapshot);
        PostWebUiState(false);
    }
}

void MainWindow::InvalidatePlaybackAreas() const {
    InvalidateRect(hwnd_, &videoSurface_, FALSE);
    InvalidateRect(hwnd_, &transportBar_, FALSE);
}

void MainWindow::InvalidateVideoSurface() const {
    InvalidateRect(hwnd_, &videoSurface_, FALSE);
}

void MainWindow::InvalidateTransportArea() {
    QueueGpuFullscreenUiOverlay();
    if (subtitleMenuOverlay_ && IsWindowVisible(subtitleMenuOverlay_)) {
        InvalidateRect(subtitleMenuOverlay_, nullptr, FALSE);
    }
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

void MainWindow::InvalidateFullscreenOverlay() {
    QueueGpuFullscreenSubtitleMenuOverlay();
    if (subtitleMenuOverlay_ && IsWindowVisible(subtitleMenuOverlay_)) {
        InvalidateRect(subtitleMenuOverlay_, nullptr, FALSE);
    }
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
        // Capability collection probes D3D/MFT and may load COM components.
        // Media preparation performs it on PlaybackSupervisor; paint uses a
        // cheap placeholder until that result is committed.
        cachedCapabilities_ = {};
        capabilitiesCached_ = true;
        LogApp(LogLevel::Debug, L"capability report pending async media preparation");
    }
    // The cached report is collected asynchronously and can predate a Windows
    // HDR/Dolby Vision desktop-mode transition. Output configuration needs the
    // current Advanced Color state, not the media-open snapshot.
    cachedCapabilities_.display.hdrEnabled =
        anvil::playback::CapabilityDetector::IsHdrEnabledNow();
    return cachedCapabilities_;
}

bool MainWindow::CurrentMediaHasHdrControls() const {
    return anvil::playback::CapabilityDetector::IsHdrEnabledNow() &&
           MediaHasHdrSignal(controller_.Snapshot().media);
}

bool MainWindow::CurrentMediaIsHdr10Plus() const {
    const auto media = controller_.Snapshot().media;
    if (media.has_value() && ContainsInsensitive(media->hdrFormat, L"HDR10+")) {
        return true;
    }
    return nativeVideoDecoder_ && nativeVideoDecoder_->Hdr10PlusDetected();
}

bool MainWindow::CurrentMediaHasCmv4Control() const {
    const auto settings = controller_.Settings();
    return !settings.video.dolbyVisionSystemPipelineExperimental &&
           MediaIsDolbyVision(controller_.Snapshot().media) &&
           !systemDolbyVisionPlayer_.IsActive();
}

bool MainWindow::CurrentCmv4ControlEnabled(const anvil::playback::PlayerSettings&) const {
    return CurrentMediaHasCmv4Control();
}

bool MainWindow::HdrToneCurveAvailable(const anvil::playback::PlayerSettings& settings) const {
    if (settings.video.autoDisplayFormat ||
        settings.video.displayMetadataPassthrough ||
        !CurrentMediaHasHdrControls() ||
        !settings.video.dolbyVisionHdrOutput ||
        MediaIsDolbyVision(controller_.Snapshot().media) ||
        CurrentMediaIsHdr10Plus()) {
        return false;
    }
    return true;
}

bool MainWindow::Cmv4ApproxActiveForPlayback(const PlaybackSessionSnapshot& snapshot,
                                             const anvil::playback::VideoSettings& settings) const {
    return MediaIsDolbyVision(snapshot.media) &&
           !settings.dolbyVisionSystemPipelineExperimental &&
           !systemDolbyVisionPlayer_.IsActive() &&
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

    const bool haveLatestFrame =
        nativeVideoDecoder_ &&
        nativeVideoDecoder_->VisitLatestFrame([&](const NativeVideoFrame& frame) {
            if (!frame.HasContent()) {
                return;
            }
            d3dRenderer_->Render(frame);
            (void)ReplaceHeldNativeFrame(frame);
        });
    if (haveLatestFrame) {
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
    } else {
        LogApp(LogLevel::Debug, L"native color settings live apply has no frame to repaint");
        return false;
    }

    LogApp(LogLevel::Debug, L"native color settings applied live");
    return true;
}

void MainWindow::ScheduleNativeColorSettingsRefresh(const bool requiresDecoderRefresh) {
    if (!hwnd_) {
        return;
    }
    ++nativeColorSettingsRefreshSerial_;
    nativeColorSettingsRefreshRequiresDecoderRefresh_ = requiresDecoderRefresh;
    PostMessageW(hwnd_, kNativeColorSettingsRefreshMessage, nativeColorSettingsRefreshSerial_, 0);
}

void MainWindow::ApplyNativeColorSettingsRefresh(const bool requiresDecoderRefresh) {
    const auto snapshot = controller_.Snapshot();
    const bool nativeVideo = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                             snapshot.media.has_value() &&
                             snapshot.media->hasVideo;
    bool handledLive = false;
    if (nativeVideo && !requiresDecoderRefresh) {
        handledLive = ApplyNativeColorSettingsLive(snapshot);
    }
    if (!handledLive) {
        if (snapshot.state == PlaybackState::Paused && nativeVideo) {
            RefreshPausedNativeFrame(snapshot, requiresDecoderRefresh);
        } else {
            RestartPlaybackIfPlaying(false);
        }
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateHdrToneCurveEditor();
    InvalidateRect(hwnd_, nullptr, FALSE);
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

bool MainWindow::NativeCmv4ToggleNeedsDecoderRefresh(const PlaybackSessionSnapshot& snapshot,
                                                     const anvil::playback::VideoSettings& settings) const {
    if (!NativeCmv4ToggleRequiresDecoderRestart(snapshot) || !WantsCmv4Approx(settings)) {
        return false;
    }
    if (heldNativeFrame_.has_value() && heldNativeFrame_->HasEnhancementYuv()) {
        return false;
    }
    return !nativeVideoDecoder_ ||
           !nativeVideoDecoder_->IsRunning() ||
           !nativeVideoDecoder_->DolbyVisionEnhancementDecodeEnabled();
}

int MainWindow::SettingsVideoFieldCount() const {
    int count = 8;
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
    const bool windowsHdrEnabled = anvil::playback::CapabilityDetector::IsHdrEnabledNow();
    const bool hdrSignalPresent = MediaHasHdrSignal(snapshot.media);
    const bool hdrControlVisible = windowsHdrEnabled && hdrSignalPresent;
    const bool defaultHdrOutput = hdrControlVisible &&
                                  (settings.video.displayMetadataPassthrough || hdrSignalPresent);
    const bool defaultEnhanced = MediaIsDolbyVision(snapshot.media) &&
                                 !settings.video.dolbyVisionSystemPipelineExperimental;
    bool changed = false;

    if (settings.video.dolbyVisionHdrOutput != defaultHdrOutput) {
        settings.video.dolbyVisionHdrOutput = defaultHdrOutput;
        changed = true;
    }
    if (settings.video.dolbyVisionCmv4Approx != defaultEnhanced) {
        settings.video.dolbyVisionCmv4Approx = defaultEnhanced;
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
               L" enhanced=" +
               std::wstring(MediaIsDolbyVision(snapshot.media) ? (settings.video.dolbyVisionCmv4Approx ? L"on" : L"off") : L"hidden") +
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
    const RECT surface = (RectWidth(playbackSurface_) > 0 && RectHeight(playbackSurface_) > 0)
                             ? playbackSurface_
                             : videoSurface_;
    if (RectWidth(surface) <= 0 || RectHeight(surface) <= 0) {
        return RECT{};
    }
    if (fullscreen_) {
        return surface;
    }
    return DeflateRectCopy(surface, Scale(2), Scale(2));
}

void MainWindow::ApplyWindowChrome() const {
    ApplyFramelessWindowChrome(hwnd_, RGB(1, 3, 6), palette_.text);
    UpdateFramelessWindowRegion(hwnd_, dpi_, fullscreen_);
}

bool MainWindow::ApplyAutomaticDisplayFormatForCurrentMedia() {
    auto settings = controller_.Settings();
    const auto snapshot = controller_.Snapshot();
    if (!settings.video.autoDisplayFormat || !snapshot.media.has_value() || !snapshot.media->hasVideo) {
        return true;
    }

    LUID adapterId{};
    UINT32 targetId = 0;
    bool hdrSupported = false;
    bool hdrEnabled = false;
    if (!ResolveWindowDisplayTarget(hwnd_, adapterId, targetId, hdrSupported, hdrEnabled)) {
        LogApp(LogLevel::Warning, L"auto display format unavailable reason=display_target_not_found");
        return false;
    }

    const bool sameTarget = automaticDisplayLeaseActive_ &&
                            automaticDisplayAdapterId_.HighPart == adapterId.HighPart &&
                            automaticDisplayAdapterId_.LowPart == adapterId.LowPart &&
                            automaticDisplayTargetId_ == targetId;
    if (automaticDisplayLeaseActive_ && !sameTarget) {
        RestoreAutomaticDisplayFormat();
    }
    if (!automaticDisplayLeaseActive_) {
        automaticDisplayLeaseActive_ = true;
        automaticDisplayAdapterId_ = adapterId;
        automaticDisplayTargetId_ = targetId;
        automaticDisplayOriginalHdrEnabled_ = hdrEnabled;
        automaticDisplayChanged_ = false;
    }

    const bool wantsHdr = MediaHasHdrSignal(snapshot.media) && hdrSupported;
    if (hdrEnabled != wantsHdr) {
        if (!SetDisplayHdrState(adapterId, targetId, wantsHdr)) {
            LogApp(LogLevel::Warning,
                   L"auto display format switch failed target=" + std::wstring(wantsHdr ? L"hdr" : L"sdr"));
            return false;
        }
        automaticDisplayChanged_ = true;
        for (int attempt = 0; attempt < 20; ++attempt) {
            Sleep(100);
            bool currentSupported = false;
            bool currentEnabled = false;
            LUID currentAdapter{};
            UINT32 currentTarget = 0;
            if (ResolveWindowDisplayTarget(hwnd_, currentAdapter, currentTarget, currentSupported, currentEnabled) &&
                currentEnabled == wantsHdr) {
                hdrEnabled = currentEnabled;
                break;
            }
        }
    }

    cachedCapabilities_.display.hdrEnabled = wantsHdr;
    LogApp(LogLevel::Info,
           L"auto display format media=" + snapshot.media->hdrFormat +
               L" windows_hdr=" + std::wstring(wantsHdr ? L"on" : L"off") +
               (MediaIsDolbyVision(snapshot.media) && !cachedCapabilities_.display.dolbyVisionSignalAvailable
                    ? L" dolby_vision=fallback_hdr10"
                    : L""));
    return true;
}

void MainWindow::RestoreAutomaticDisplayFormat() {
    if (!automaticDisplayLeaseActive_) {
        return;
    }
    if (automaticDisplayChanged_) {
        SetDisplayHdrState(automaticDisplayAdapterId_,
                           automaticDisplayTargetId_,
                           automaticDisplayOriginalHdrEnabled_);
        LogApp(LogLevel::Info,
               L"auto display format restored windows_hdr=" +
                   std::wstring(automaticDisplayOriginalHdrEnabled_ ? L"on" : L"off"));
    }
    automaticDisplayLeaseActive_ = false;
    automaticDisplayChanged_ = false;
}

void MainWindow::SetAutomaticDisplayFormat(const bool enabled) {
    auto settings = controller_.Settings();
    if (settings.video.autoDisplayFormat == enabled) {
        return;
    }
    const bool systemPipelineWasEnabled = settings.video.dolbyVisionSystemPipelineExperimental;
    if (!enabled) {
        RestoreAutomaticDisplayFormat();
    }
    settings.video.autoDisplayFormat = enabled;
    if (enabled) {
        settings.video.displayMetadataPassthrough = true;
        settings.video.dolbyVisionSystemPipelineExperimental = true;
    } else {
        settings.video.displayMetadataPassthrough =
            LoadVideoBooleanSetting(L"DisplayMetadataPassthrough").value_or(false);
        settings.video.dolbyVisionSystemPipelineExperimental =
            LoadVideoBooleanSetting(L"DolbyVisionSystemPipelineExperimental").value_or(false);
    }
    controller_.ApplySettings(settings);
    SaveVideoBooleanSetting(L"AutoDisplayFormat", enabled);
    const auto snapshot = controller_.Snapshot();
    if (snapshot.media.has_value()) {
        if (enabled) {
            ApplyAutomaticDisplayFormatForCurrentMedia();
        }
        // Auto mode selects the system Dolby pipeline and disables CMv4.
        // Recompute both defaults when leaving auto mode as well, otherwise
        // the restored native pipeline inherits CMv4=off from auto mode.
        ApplyDefaultHdrControlsForCurrentMedia();
    }
    if (!enabled && systemPipelineWasEnabled &&
        !controller_.Settings().video.dolbyVisionSystemPipelineExperimental) {
        systemDolbyVisionFallbackForCurrentMedia_ = false;
    }
    if (snapshot.media.has_value() && snapshot.media->hasVideo) {
        ScheduleNativeColorSettingsRefresh(true);
    }
    MarkLayoutDirty();
    EnsureLayout();
    PostWebUiState();
}

void MainWindow::SetDisplayPeakBrightnessNits(const int peakNits) {
    const int normalized = peakNits <= 0 ? 0 : std::clamp(peakNits, 100, 10000);
    auto settings = controller_.Settings();
    if (settings.video.displayPeakBrightnessNits == normalized) {
        return;
    }
    settings.video.displayPeakBrightnessNits = normalized;
    controller_.ApplySettings(settings);
    SaveVideoDwordSetting(L"DisplayPeakBrightnessNits", normalized);

    const auto snapshot = controller_.Snapshot();
    if (snapshot.media.has_value() && snapshot.media->hasVideo) {
        ApplyNativeColorSettingsLive(snapshot);
    }
    LogApp(LogLevel::Info,
           normalized == 0
               ? L"hdr display peak setting=auto"
               : L"hdr display peak setting=manual peak=" + std::to_wstring(normalized) + L" nits");
    PostWebUiState(true);
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

void MainWindow::OpenLocalFolderDialog() {
#if 0
    const auto postMessage = [this](const std::wstring& json) {
        if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
            webUiHost_->PostJson(json);
        }
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
    if (folder.empty()) {
        postMessage(L"{\"type\":\"localFolderPickFailed\",\"message\":\"所选路径不是可读取的文件夹\"}");
        return;
    }

    postMessage(LocalFolderPickedJson(folder, {}, false, true));

    StartLocalFolderScanAsync(hwnd_, folder);
#endif
}

void MainWindow::OpenSubtitleFileDialog() {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        return;
    }

    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Subtitle Files\0*.srt;*.ass;*.ssa;*.vtt\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    auto settings = controller_.Settings();
    settings.subtitles.externalSubtitlePath = filePath.data();
    settings.subtitles.selectedTrackIndex = anvil::playback::kSubtitleTrackAuto;
    controller_.ApplySettings(settings);
    LogApp(LogLevel::Info, L"subtitle external=" + settings.subtitles.externalSubtitlePath.filename().wstring());

    const auto updated = controller_.Snapshot();
    if (updated.state == PlaybackState::Paused &&
        updated.media.has_value() &&
        updated.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(updated, true);
    } else {
        RestartPlaybackIfPlaying();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenDanmakuFileDialog() {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value()) {
        return;
    }

    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Danmaku Files\0*.xml;*.json;*.ass\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    auto settings = controller_.Settings();
    settings.danmaku.externalDanmakuPath = filePath.data();
    settings.danmaku.enabled = true;
    controller_.ApplySettings(settings);
    LogApp(LogLevel::Info, L"danmaku external=" + settings.danmaku.externalDanmakuPath.filename().wstring());

    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StopRuntimeAsync(const bool clearVideoFrame) {
    if (runtimeStopAsyncInProgress_.load()) {
        runtimeStopAsyncClearFrame_.store(runtimeStopAsyncClearFrame_.load() || clearVideoFrame);
        FinishRuntimeStopVisuals(clearVideoFrame);
        LogApp(LogLevel::Debug, L"runtime async stop already in progress");
        return;
    }

    nativeSeekPrerollHoldingAudio_ = false;
    ResetNativeBufferingWatchdog();
    runtimeStopWorkerDone_.store(false);
    runtimeStopAsyncClearFrame_.store(clearVideoFrame);
    runtimeStopAsyncInProgress_.store(true);
    runtimeStopRetryPending_ = false;
    FinishRuntimeStopVisuals(clearVideoFrame);
    LogApp(LogLevel::Debug, L"runtime async stop begin");
    // Broadcast cancellation before the background worker waits for any
    // component. This wakes FFmpeg/WASAPI/pipe I/O as one coordinated phase.
    playbackPlayer_.RequestStop();
    videoDecoder_.RequestStop();
    systemDolbyVisionPlayer_.RequestStop();
    if (nativeVideoDecoder_) {
        nativeVideoDecoder_->RequestStop();
    }
    audioPlayer_.RequestStop();
    const HWND completionWindow = hwnd_;
    const uint64_t completionCookie = windowLifetimeCookie_;
    try {
        runtimeStopThread_ = std::thread([this, completionWindow, completionCookie]() {
            StopRuntimeBackends();
            runtimeStopWorkerDone_.store(true);
            if (completionWindow) {
                PostMessageW(completionWindow,
                             kRuntimeStopCompleteMessage,
                             static_cast<WPARAM>(completionCookie),
                             0);
            }
        });
        runtimeStopWorkerStartFailureCount_ = 0;
    } catch (const std::system_error& error) {
        runtimeStopAsyncInProgress_.store(false);
        runtimeStopRetryPending_ = true;
        ++runtimeStopWorkerStartFailureCount_;
        LogApp(LogLevel::Error, L"runtime stop worker start failed error=" + Utf8ToWide(error.what()));
    }
    const bool pollTimerAvailable = SetTimer(hwnd_,
                                             kAsyncCompletionPollTimer,
                                             kAsyncCompletionPollTimerMs,
                                             nullptr) != 0;
    if (!pollTimerAvailable) {
        LogApp(LogLevel::Warning, L"runtime stop poll timer unavailable");
    }
    if (runtimeStopRetryPending_ &&
        (!pollTimerAvailable ||
         runtimeStopWorkerStartFailureCount_ >= kMaxRuntimeStopWorkerStartFailures)) {
        RetireAfterRuntimeStopSchedulingFailure();
    }
}

void MainWindow::RetireAfterRuntimeStopSchedulingFailure() {
    runtimeStopRetryPending_ = false;
    // There is deliberately no UI-thread Stop() fallback. Cancellation was
    // already broadcast, so retire this player through Application's
    // background reaper. This is finite and cannot create a WM_TIMER feedback
    // loop when USER handles and worker threads are both exhausted.
    runtimeStopAsyncInProgress_.store(true);
    LogApp(LogLevel::Error,
           L"runtime stop could not schedule a worker; retiring player through background reaper");
    if (!closePending_) {
        BeginClose();
        return;
    }
    closeStartedAt_ = std::chrono::steady_clock::now() - std::chrono::seconds{2};
}

void MainWindow::StopRuntimeBackends() {
    playbackPlayer_.Stop();
    videoDecoder_.Stop();
    systemDolbyVisionPlayer_.Stop();
    if (nativeVideoDecoder_) {
        nativeVideoDecoder_->Stop();
    }
    audioPlayer_.Stop();

    // Stop() joins the producers first, so decoder-owned frame queues can now
    // be destroyed entirely on this background worker.  Releasing the final
    // AVFrame/D3D reference or a large CPU plane is never window-thread work.
    videoDecoder_.ClearFrame();
    if (nativeVideoDecoder_) {
        nativeVideoDecoder_->ClearFrame();
    }
    runtimeCleanupPending_.store(false, std::memory_order_release);
}

void MainWindow::ResumeNativeSeekPrerollAudio(const std::chrono::milliseconds position) {
    if (!nativeSeekPrerollHoldingAudio_) {
        return;
    }
    nativeSeekPrerollHoldingAudio_ = false;

    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasAudio ||
        !audioPlayer_.IsRunning()) {
        return;
    }

    const auto settings = controller_.Settings();
    if (settings.audio.selectedTrackIndex == anvil::playback::kAudioTrackOff) {
        return;
    }

    const auto resumePosition = position.count() >= 0 ? position : snapshot.position;
    audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
    const bool resumed = audioPlayer_.ResumePacketStream();
    LogApp(resumed ? LogLevel::Debug : LogLevel::Warning,
           L"native seek preroll resume audio=" + std::wstring(resumed ? L"true" : L"false") +
               L" position=" + FormatTimecode(resumePosition));
}

void MainWindow::FinishRuntimeStopVisuals(const bool clearVideoFrame) {
    if (clearVideoFrame) {
        RetireHeldNativeFrame();
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
    UpdateBufferingOverlay();
}

void MainWindow::CompleteAsyncRuntimeStop() {
    if (runtimeStopThread_.joinable()) {
        // StopRuntimeBackends posts completion only after every Stop returned.
        // Detach releases the completed thread handle without a WndProc join.
        runtimeStopThread_.detach();
    }
    if (runtimeStopAsyncInProgress_.exchange(false)) {
        LogApp(LogLevel::Debug, L"runtime async stop complete");
        FinishRuntimeStopVisuals(runtimeStopAsyncClearFrame_.load());
    }
    runtimeStopWorkerDone_.store(false);
    runtimeStopWorkerStartFailureCount_ = 0;
}

void MainWindow::PollPlaybackSupervisorCompletion() {
    if (!playbackSupervisor_) {
        return;
    }
    auto completion = playbackSupervisor_->TakeCompletion();
    if (completion && completion->windowCookie == windowLifetimeCookie_ &&
        completion->generation == playbackSupervisor_->CurrentGeneration()) {
        CompleteOpenPath(std::move(*completion));
        return;
    }
    const uint64_t failedGeneration = playbackSupervisor_->TakeFailedGeneration();
    if (failedGeneration != 0 && failedGeneration == playbackSupervisor_->CurrentGeneration()) {
        controller_.SetError(L"Open operation failed in the background worker");
        LogApp(LogLevel::Error,
               L"open worker exception generation=" + std::to_wstring(failedGeneration));
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostWebUiState();
    }
}

void MainWindow::CompleteRendererInitialization(const D3D11RendererState state) {
    if (closePending_ || !d3dRenderer_ || state != d3dRenderer_->State()) {
        return;
    }
    if (state == D3D11RendererState::Ready) {
        if (videoHostReady_) {
            return;
        }
        videoHostReady_ = true;
        videoHostInitializationFailureHandled_ = false;
        UpdateVideoHost();
        LogApp(LogLevel::Info, L"native d3d renderer initialized asynchronously");
        if (deferredRuntimeStart_ || deferredPausedFrameRefresh_) {
            ContinueRuntimeAfterAsyncStop();
        }
        return;
    }
    if (state != D3D11RendererState::Failed || videoHostInitializationFailureHandled_) {
        return;
    }
    videoHostInitializationFailureHandled_ = true;
    videoHostReady_ = false;
    if (systemDolbyVisionPlayer_.IsActive()) {
        LogApp(LogLevel::Warning,
               L"native d3d renderer initialization failed while system dolby vision remains active");
        UpdateVideoHost();
        return;
    }
    if (videoHost_) {
        ShowWindow(videoHost_, SW_HIDE);
    }
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    LogApp(LogLevel::Error, L"native d3d renderer asynchronous initialization failed");
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing) {
        FailPlaybackRuntime(L"Native D3D renderer failed to initialize");
    } else {
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostWebUiState();
    }
}

void MainWindow::BeginClose() {
    if (closePending_) {
        return;
    }
    closePending_ = true;
    closeStartedAt_ = std::chrono::steady_clock::now();
    // Acknowledge close before touching any backend or COM object. Even if a
    // third-party component retires slowly, no apparently-live player window
    // remains on screen accepting input.
    ShowWindow(hwnd_, SW_HIDE);
    EnableWindow(hwnd_, FALSE);
    DragAcceptFiles(hwnd_, FALSE);
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    SetPlaybackTimer(false);
    KillTimer(hwnd_, kPlaybackTimer);
    KillTimer(hwnd_, kUiAnimationTimer);
    KillTimer(hwnd_, kFullscreenChromeHideTimer);
    KillTimer(hwnd_, kVideoPressTimer);
    KillTimer(hwnd_, kScrollbarAutoHideTimer);
    if (SetTimer(hwnd_, kAsyncCompletionPollTimer, kAsyncCompletionPollTimerMs, nullptr) == 0) {
        // Without a polling timer there may be no future message with which to
        // enforce the close deadline. Fall through to the background reaper
        // immediately rather than leave a hidden, immortal window.
        LogApp(LogLevel::Warning, L"close poll timer unavailable; using immediate reaper fallback");
        closeStartedAt_ = std::chrono::steady_clock::now() - std::chrono::seconds{2};
    }
    if (playbackSupervisor_) {
        playbackSupervisor_->RequestShutdown();
    }
    InvalidateBackgroundListWorkers();
    if (RuntimeBackendsRunning() || RuntimeStopInProgress()) {
        StopRuntimeAsync(true);
    } else {
        FinishRuntimeStopVisuals(true);
    }
    if (d3dRenderer_) {
        d3dRenderer_->RequestStop(hwnd_,
                                  kRenderThreadStoppedMessage,
                                  windowLifetimeCookie_);
    }
    // WebView callbacks are invalidated only after all backend cancellation
    // has been broadcast. Controller teardown remains on its owning apartment.
    if (webUiHost_) {
        webUiHost_->Shutdown();
    }
    TryFinishClose();
}

void MainWindow::TryFinishClose() {
    if (!closePending_ || closeReady_) {
        return;
    }
    const bool closeDeadlineExpired =
        closeStartedAt_.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - closeStartedAt_ >= std::chrono::seconds{2};
    if (!closeDeadlineExpired) {
        if (RuntimeStopInProgress()) {
            return;
        }
        if (RuntimeBackendsRunning()) {
            StopRuntimeAsync(true);
            return;
        }
        if (playbackSupervisor_ && !playbackSupervisor_->IsStopped()) {
            return;
        }
        if (d3dRenderer_ && !d3dRenderer_->IsStopped()) {
            return;
        }
    } else {
        LogApp(LogLevel::Warning, L"close deadline reached; remaining workers moved to background reaper");
    }
    closeReady_ = true;
    MSG queued{};
    while (PeekMessageW(&queued, hwnd_, kOpenMediaCompleteMessage, kOpenMediaCompleteMessage, PM_REMOVE)) {
    }
    while (PeekMessageW(&queued, hwnd_, kOpenPathMessage, kOpenPathMessage, PM_REMOVE)) {
        delete reinterpret_cast<std::filesystem::path*>(queued.lParam);
    }
    while (PeekMessageW(&queued,
                        hwnd_,
                        kNativeVideoDecodeFailedMessage,
                        kNativeVideoDecodeFailedMessage,
                        PM_REMOVE)) {
        delete reinterpret_cast<NativeDecodeFailure*>(queued.lParam);
    }
    DestroyWindow(hwnd_);
}

bool MainWindow::RuntimeStopInProgress() const {
    return runtimeStopAsyncInProgress_.load();
}

bool MainWindow::RuntimeBackendsRunning() const {
    return runtimeCleanupPending_.load(std::memory_order_acquire) ||
           playbackPlayer_.IsRunning() ||
           videoDecoder_.IsRunning() ||
           systemDolbyVisionPlayer_.IsActive() ||
           audioPlayer_.IsRunning() ||
           (nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning());
}

void MainWindow::ClearDeferredRuntimeStart() {
    deferredRuntimeStart_ = false;
    deferredRuntimeRestart_ = false;
    deferredRuntimeWaitForPreroll_ = false;
}

void MainWindow::QueueDeferredRuntimeStart(const bool restart, const bool waitForPreroll) {
    if (!deferredRuntimeStart_) {
        deferredRuntimeWaitForPreroll_ = waitForPreroll;
    } else {
        deferredRuntimeWaitForPreroll_ = deferredRuntimeWaitForPreroll_ && waitForPreroll;
    }
    deferredRuntimeStart_ = true;
    deferredRuntimeRestart_ = deferredRuntimeRestart_ || restart;
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
}

void MainWindow::QueuePausedNativeFrameRefresh(const bool forceDecoderRestart) {
    deferredPausedFrameRefresh_ = true;
    deferredPausedFrameRefreshForceRestart_ =
        deferredPausedFrameRefreshForceRestart_ || forceDecoderRestart;
}

void MainWindow::RequestRuntimeStart(const bool restart, const bool waitForPreroll) {
    if (closePending_) {
        return;
    }
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        (!snapshot.media->hasVideo && !snapshot.media->hasAudio)) {
        ClearDeferredRuntimeStart();
        return;
    }

    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    if (RuntimeStopInProgress()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        LogApp(LogLevel::Debug,
               L"runtime start deferred while stop is pending restart=" +
                   std::wstring(restart ? L"true" : L"false"));
        SetPlaybackTimer(true);
        return;
    }

    if (RuntimeBackendsRunning()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        LogApp(LogLevel::Debug,
               L"runtime restart queued restart=" +
                   std::wstring(restart ? L"true" : L"false"));
        StopRuntimeAsync(false);
        SetPlaybackTimer(true);
        return;
    }

    if (StartRuntime(snapshot, restart, waitForPreroll)) {
        SetPlaybackTimer(true);
    }
}

void MainWindow::ContinueRuntimeAfterAsyncStop() {
    const bool startRequested = deferredRuntimeStart_;
    bool restart = deferredRuntimeRestart_;
    const bool waitForPreroll = deferredRuntimeWaitForPreroll_;
    const bool refreshPausedFrame = deferredPausedFrameRefresh_;
    const bool forcePausedFrameRefresh = deferredPausedFrameRefreshForceRestart_;
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;

    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        if (!startRequested) {
            restart = true;
        }
        if (StartRuntime(snapshot, restart, waitForPreroll)) {
            SetPlaybackTimer(true);
        }
    } else {
        SetPlaybackTimer(false);
        if (snapshot.state == PlaybackState::Paused &&
            refreshPausedFrame &&
            snapshot.media.has_value() &&
            snapshot.media->hasVideo &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11) {
            RefreshPausedNativeFrame(snapshot, forcePausedFrameRefresh);
        }
    }

    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

bool MainWindow::CaptureLatestNativeFrame() {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 || !nativeVideoDecoder_) {
        return heldNativeFrame_.has_value();
    }

    bool replaced = false;
    const bool visited = nativeVideoDecoder_->VisitLatestFrame([&](const NativeVideoFrame& frame) {
        replaced = ReplaceHeldNativeFrame(frame);
    });
    return (visited && replaced) || heldNativeFrame_.has_value();
}

bool MainWindow::ReplaceHeldNativeFrame(const NativeVideoFrame& frame) {
    if (!heldNativeFrame_) {
        try {
            heldNativeFrame_.emplace(frame);
            return true;
        } catch (...) {
            return false;
        }
    }
    if (!d3dRenderer_ || !d3dRenderer_->RetireFrame(std::move(*heldNativeFrame_))) {
        // Keep the existing owner when the bounded retirement queue is under
        // pressure. Replacing this optional must never release a GPU frame on
        // the window thread.
        return false;
    }
    try {
        *heldNativeFrame_ = frame;
        return true;
    } catch (...) {
        // The previous owner was already transferred to the retirement
        // worker. Resetting its moved-from shell is lightweight and leaves the
        // source frame owned by the decoder worker.
        heldNativeFrame_.reset();
        return false;
    }
}

void MainWindow::RetireHeldNativeFrame() {
    if (!heldNativeFrame_) {
        return;
    }
    if (d3dRenderer_ && d3dRenderer_->RetireFrame(std::move(*heldNativeFrame_))) {
        // Resetting the moved-from value releases no decoder/GPU ownership.
        heldNativeFrame_.reset();
    }
}

void MainWindow::RenderHeldNativeFrame(const bool logRepaint) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 || !d3dRenderer_) {
        return;
    }

    if (heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
        if (logRepaint) {
            LogApp(LogLevel::Debug,
                   L"repainting cached native frame pixels=" +
                       std::wstring(heldNativeFrame_->HasPixels() ? L"true" : L"false") +
                       L" texture=" +
                       std::wstring(heldNativeFrame_->HasD3DTexture() ? L"true" : L"false"));
        }
        d3dRenderer_->Render(*heldNativeFrame_);
    } else if (logRepaint) {
        LogApp(LogLevel::Debug, L"no cached native frame available to repaint");
    }
    heldNativeFrameNeedsPresent_ = false;
}

bool MainWindow::StartRuntime(const PlaybackSessionSnapshot& snapshot,
                              const bool restart,
                              const bool waitForPreroll) {
    if (closePending_) {
        return false;
    }
    if (!snapshot.media.has_value()) {
        return false;
    }

    pendingPausedFrameRefresh_ = false;
    ResetNativeBufferingWatchdog();
    if (RuntimeStopInProgress()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        return false;
    }
    if (RuntimeBackendsRunning()) {
        QueueDeferredRuntimeStart(restart, waitForPreroll);
        StopRuntimeAsync(false);
        return false;
    }

    if (backend_ == PlaybackBackend::EmbeddedFfplay) {
        runtimeCleanupPending_.store(true, std::memory_order_release);
        const bool started = playbackPlayer_.Start(hwnd_,
                                                   PlaybackSurfaceBounds(),
                                                   snapshot.media->path,
                                                   snapshot.position,
                                                   snapshot.volume,
                                                   snapshot.media->dolbyVisionDetected);
        LogApp(started ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error,
               std::wstring(L"external ffplay playback ") + (restart ? L"restart=" : L"start=") + (started ? L"true" : L"false"));
        LogApp(LogLevel::Debug, L"ffplay command=" + playbackPlayer_.LastCommandLine());
        if (!started) {
            FailPlaybackRuntime(L"External FFplay failed to start");
            return false;
        }
        return true;
    }

    if (backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        return StartNativeRuntime(snapshot, restart, waitForPreroll);
    }

    // RawFrameBridge (--internal-playback): legacy ffmpeg raw-pipe path.
    runtimeCleanupPending_.store(true, std::memory_order_release);
    bool videoStarted = true;
    bool audioStarted = true;
    if (snapshot.media->hasVideo) {
        videoStarted = videoDecoder_.Start(snapshot.media->path,
                                          snapshot.position,
                                          hwnd_,
                                          kVideoFrameReadyMessage,
                                          windowLifetimeCookie_);
    }
    const auto runtimeSettings = controller_.Settings();
    if (snapshot.media->hasAudio &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
        audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
        audioStarted = audioPlayer_.Start(snapshot.media->path,
                                          snapshot.position,
                                          snapshot.volume,
                                          runtimeSettings.audio.selectedTrackIndex,
                                          runtimeSettings.audio.passthroughPreferred);
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
    if (!videoStarted || !audioStarted) {
        FailPlaybackRuntime(L"Internal playback runtime failed to start video=" +
                            std::wstring(videoStarted ? L"true" : L"false") +
                            L" audio=" + std::wstring(audioStarted ? L"true" : L"false"));
        return false;
    }
    return true;
}

void MainWindow::EnsureVideoHost() {
    if (videoHostReady_ || closePending_) {
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

    const auto rendererState = d3dRenderer_->State();
    if (rendererState == D3D11RendererState::Ready) {
        // A completion PostMessage can be lost during queue teardown. A caller
        // that is already starting the runtime may adopt the published ready
        // state directly; it must not recursively replay deferred work here.
        videoHostReady_ = true;
        videoHostInitializationFailureHandled_ = false;
        UpdateVideoHost();
        return;
    }
    if (rendererState == D3D11RendererState::Initializing) {
        return;
    }
    if (rendererState == D3D11RendererState::Failed ||
        rendererState == D3D11RendererState::Stopping) {
        LogApp(LogLevel::Error, L"native d3d renderer is not available for initialization");
        ShowWindow(videoHost_, SW_HIDE);
        return;
    }
    if (!d3dRenderer_->BeginInitialize(videoHost_,
                                       hwnd_,
                                       kRenderInitializationCompleteMessage,
                                       windowLifetimeCookie_)) {
        LogApp(LogLevel::Error, L"native d3d renderer worker failed to start");
        ShowWindow(videoHost_, SW_HIDE);
        return;
    }
    videoHostInitializationFailureHandled_ = false;
    LogApp(LogLevel::Info, L"native video host created; d3d initialization queued");
}

bool MainWindow::StartNativeRuntime(const PlaybackSessionSnapshot& snapshot,
                                    const bool restart,
                                    const bool waitForPreroll) {
    if (closePending_) {
        return false;
    }
    nativeSeekPrerollHoldingAudio_ = false;
    bool videoStarted = true;
    bool audioStarted = true;
    bool preferDolbyVisionHdrOutput = false;
    bool enableDolbyVisionEnhancementDecode = false;
    const auto runtimeSettings = controller_.Settings();
    const auto capabilities = CachedCapabilities();
    // Dolby Vision owns a separate Windows Media Foundation presentation
    // pipeline. The system HEVC decoder and the registered Dolby renderer
    // extension stay inside MediaEngine; the FFmpeg/D3D renderer is used only
    // as a one-shot compatibility fallback when that pipeline is unavailable.
    const bool requestSystemDolbyVision =
        snapshot.media->hasVideo &&
        snapshot.media->dolbyVisionDetected &&
        (runtimeSettings.video.autoDisplayFormat ||
         runtimeSettings.video.dolbyVisionSystemPipelineExperimental) &&
        runtimeSettings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
        anvil::playback::CapabilityDetector::IsHdrEnabledNow() &&
        capabilities.codecs.dolbyVisionExtensionDetected &&
        capabilities.display.dolbyVisionSignalAvailable &&
        !systemDolbyVisionFallbackForCurrentMedia_;
    if (snapshot.media->dolbyVisionDetected) {
        LogApp(LogLevel::Info,
               L"dolby vision system pipeline requested=" +
                   std::wstring(requestSystemDolbyVision ? L"true" : L"false") +
                   L" profile=" + snapshot.media->hdrFormat +
                   L" experimental=" +
                   std::wstring(runtimeSettings.video.dolbyVisionSystemPipelineExperimental ? L"on" : L"off") +
                   L" extension_reported=" +
                   std::wstring(capabilities.codecs.dolbyVisionExtensionDetected ? L"true" : L"false") +
                   L" edid_lldv=" +
                   std::wstring(capabilities.display.dolbyVisionSignalAvailable ? L"true" : L"false") +
                   L" session_fallback=" +
                   std::wstring(systemDolbyVisionFallbackForCurrentMedia_ ? L"true" : L"false"));
    }
    if (requestSystemDolbyVision) {
        MarkLayoutDirty();
        EnsureLayout();
        EnsureVideoHost();
        if (videoHost_ && systemDolbyVisionPlayer_.Start(videoHost_,
                                                         snapshot.media->path,
                                                         snapshot.position,
                                                         snapshot.volume)) {
            systemDolbyVisionPlayer_.SetPlaybackRate(snapshot.playbackRate);
            runtimeCleanupPending_.store(true, std::memory_order_release);
            systemDolbyVisionPlayingLogged_ = false;
            systemDolbyVisionStartedAt_ = std::chrono::steady_clock::now();
            ShowWindow(videoHost_, SW_SHOWNA);
            UpdateVideoHost();
            LogApp(restart ? LogLevel::Debug : LogLevel::Info,
                   L"system dolby vision playback start=true pipeline=media_engine+dolby_renderer_extension profile=" +
                       snapshot.media->hdrFormat +
                       L" source=" + snapshot.media->container);
            return true;
        }
        systemDolbyVisionFallbackForCurrentMedia_ = true;
        LogApp(LogLevel::Warning,
               L"system dolby vision playback start=false fallback=ffmpeg_hdr10 reason=" +
                   systemDolbyVisionPlayer_.LastError());
    } else if (snapshot.media->dolbyVisionDetected &&
               runtimeSettings.video.dolbyVisionSystemPipelineExperimental &&
               runtimeSettings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
               !systemDolbyVisionFallbackForCurrentMedia_) {
        systemDolbyVisionFallbackForCurrentMedia_ = true;
        LogApp(LogLevel::Warning,
               L"system dolby vision playback unavailable reason=" +
                   std::wstring(!anvil::playback::CapabilityDetector::IsHdrEnabledNow()
                                    ? L"windows_hdr_off"
                                    : L"renderer_extension_not_registered") +
                   L" fallback=ffmpeg_hdr10");
    }
    if (snapshot.media->hasVideo) {
        MarkLayoutDirty();
        EnsureLayout();
        EnsureVideoHost();
        if (!videoHostReady_ || !d3dRenderer_ || !d3dRenderer_->IsReady()) {
            if (d3dRenderer_ && d3dRenderer_->State() == D3D11RendererState::Initializing) {
                QueueDeferredRuntimeStart(restart, waitForPreroll);
                LogApp(LogLevel::Debug, L"native runtime deferred while d3d initializes");
            } else {
                FailPlaybackRuntime(L"Native D3D renderer is unavailable");
            }
            return false;
        }
    }
    runtimeCleanupPending_.store(true, std::memory_order_release);
    const bool useSharedNetworkDemuxer =
        snapshot.media->hasVideo &&
        snapshot.media->hasAudio &&
        IsNetworkMediaPath(snapshot.media->path) &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff;
    NativeAudioPacketSink audioPacketSink;
    if (useSharedNetworkDemuxer) {
        audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
        const uint64_t audioStartGeneration = audioPlayer_.ArmPendingStart(snapshot.position);
        audioPacketSink.selectedTrackIndex = runtimeSettings.audio.selectedTrackIndex;
        audioPacketSink.start =
            [this,
             path = snapshot.media->path,
             volume = snapshot.volume,
             audioStartGeneration,
             preferPassthrough = runtimeSettings.audio.passthroughPreferred](
                const AVCodecParameters* codecParameters,
                const AVRational timeBase,
                const std::chrono::milliseconds startPosition,
                const int streamIndex) {
                return audioPlayer_.StartPacketStream(path,
                                                      codecParameters,
                                                      timeBase,
                                                      startPosition,
                                                      volume,
                                                      streamIndex,
                                                      audioStartGeneration,
                                                      preferPassthrough);
            };
        audioPacketSink.pushPacket = [this](const AVPacket* packet) {
            return audioPlayer_.QueuePacket(packet);
        };
        audioPacketSink.reset = [this](const std::chrono::milliseconds position) {
            audioPlayer_.ResetPacketStream(position);
        };
        audioPacketSink.endOfStream = [this]() {
            audioPlayer_.MarkPacketStreamEof();
        };
        audioPacketSink.startFailed = [this, audioStartGeneration]() {
            audioPlayer_.FailPendingStart(audioStartGeneration,
                                          L"wasapi packet stream unavailable");
        };
    }
    lastNativeStatsLog_ = {};
    if (snapshot.media->hasVideo) {
        if (d3dRenderer_) {
            const auto& settings = runtimeSettings;
            d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
            d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
            d3dRenderer_->ResetRenderStats();
        }
        const auto& settings = runtimeSettings;
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
                                                  kNativeVideoDecodeFailedMessage,
                                                  [this]() {
                                                      return audioPlayer_.PlaybackClock();
                                                   },
                                                  preferHardwareDecode,
                                                  d3dRenderer_ ? d3dRenderer_->Device() : nullptr,
                                                  settings.video.selectedTrackIndex,
                                                  settings.subtitles.preferredLanguage,
                                                  settings.subtitles.selectedTrackIndex,
                                                  std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                                  settings.subtitles.externalSubtitleAutoLoad,
                                                  settings.subtitles.externalSubtitlePath,
                                                   false,
                                                   preferDolbyVisionHdrOutput,
                                                   enableDolbyVisionEnhancementDecode,
                                                  std::move(audioPacketSink),
                                                  windowLifetimeCookie_);
        if (videoStarted) {
            MarkLayoutDirty();
            EnsureLayout();
            UpdateVideoHost();
        }
    }
    const bool waitForEnhancedPreroll =
        waitForPreroll &&
        restart &&
        videoStarted &&
        snapshot.media->hasVideo &&
        nativeVideoDecoder_ &&
        enableDolbyVisionEnhancementDecode &&
        Cmv4ApproxActiveForPlayback(snapshot, runtimeSettings.video);
    if (waitForEnhancedPreroll) {
        LogApp(LogLevel::Debug, L"native enhanced preroll continues asynchronously");
    } else if (waitForPreroll && videoStarted && snapshot.media->hasVideo && snapshot.media->hasAudio && nativeVideoDecoder_) {
        LogApp(LogLevel::Debug, L"native preroll continues asynchronously");
    }
    if (snapshot.media->hasAudio &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
        if (useSharedNetworkDemuxer) {
            audioStarted = videoStarted;
        } else {
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            audioStarted = audioPlayer_.Start(snapshot.media->path,
                                              snapshot.position,
                                              snapshot.volume,
                                              runtimeSettings.audio.selectedTrackIndex,
                                              runtimeSettings.audio.passthroughPreferred);
        }
    }

    const LogLevel level = videoStarted && audioStarted ? (restart ? LogLevel::Debug : LogLevel::Info) : LogLevel::Error;
    LogApp(level,
           std::wstring(L"native ffmpeg/d3d11 playback ") +
               (restart ? L"restart" : L"start") +
               L" video=" + (videoStarted ? L"true" : L"false") +
               L" audio=" + (audioStarted ? L"true" : L"false") +
               L" shared_demux=" + (useSharedNetworkDemuxer ? L"true" : L"false"));
    if (snapshot.media->hasVideo && videoStarted) {
        LogApp(LogLevel::Debug, L"native decode path=" + nativeVideoDecoder_->Path().wstring());
        if (snapshot.media->dolbyVisionDetected) {
            const bool cmv4Intermediate = Cmv4ApproxActiveForPlayback(snapshot, runtimeSettings.video);
            const bool finalHdrOutput = WantsDolbyVisionHdrOutput(runtimeSettings.video, CachedCapabilities().display);
            LogApp(LogLevel::Info,
                   L"dolby vision fallback path=ffmpeg_libplacebo final=" +
                       std::wstring(finalHdrOutput ? L"windows_hdr10" : L"sdr") +
                       (cmv4Intermediate ? L" cmv4=on" : L" cmv4=off") +
                       (enableDolbyVisionEnhancementDecode ? L" el_decode=on" : L" el_decode=off"));
        }
    }
    if (snapshot.media->hasAudio) {
        LogApp(LogLevel::Debug, L"wasapi audio=" + audioPlayer_.LastStatus());
    }
    if (!videoStarted || !audioStarted) {
        FailPlaybackRuntime(L"Native playback runtime failed to start video=" +
                            std::wstring(videoStarted ? L"true" : L"false") +
                            L" audio=" + std::wstring(audioStarted ? L"true" : L"false"));
        return false;
    }
    return true;
}

bool MainWindow::SeekNativeRuntime(const PlaybackSessionSnapshot& snapshot) {
    if (closePending_) {
        return false;
    }
    if (snapshot.state == PlaybackState::Playing &&
        systemDolbyVisionPlayer_.IsActive()) {
        return systemDolbyVisionPlayer_.Seek(snapshot.position);
    }
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        snapshot.state != PlaybackState::Playing ||
        !snapshot.media.has_value() ||
        (!snapshot.media->hasVideo && !snapshot.media->hasAudio)) {
        return false;
    }
    ResetNativeBufferingWatchdog();
    const auto runtimeSettings = controller_.Settings();
    const bool useSharedNetworkDemuxer =
        snapshot.media->hasVideo &&
        snapshot.media->hasAudio &&
        IsNetworkMediaPath(snapshot.media->path) &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff;
    bool videoSeeked = true;
    if (snapshot.media->hasVideo) {
        videoSeeked = nativeVideoDecoder_ &&
                      nativeVideoDecoder_->IsRunning() &&
                      nativeVideoDecoder_->Seek(snapshot.position);
    }
    if (!videoSeeked) {
        nativeSeekPrerollHoldingAudio_ = false;
        return false;
    }

    bool audioSeeked = true;
    if (snapshot.media->hasAudio &&
        runtimeSettings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
        if (useSharedNetworkDemuxer) {
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            if (snapshot.media->hasVideo && audioPlayer_.IsRunning()) {
                audioPlayer_.HoldPacketStream(snapshot.position);
                nativeSeekPrerollHoldingAudio_ = true;
                LogApp(LogLevel::Debug,
                       L"native seek preroll hold audio position=" + FormatTimecode(snapshot.position));
            } else {
                nativeSeekPrerollHoldingAudio_ = false;
            }
            audioSeeked = true;
        } else if (audioPlayer_.IsRunning()) {
            nativeSeekPrerollHoldingAudio_ = false;
            audioSeeked = audioPlayer_.Seek(snapshot.position);
        } else {
            nativeSeekPrerollHoldingAudio_ = false;
            audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
            audioSeeked = audioPlayer_.Start(snapshot.media->path,
                                             snapshot.position,
                                             snapshot.volume,
                                             runtimeSettings.audio.selectedTrackIndex,
                                             runtimeSettings.audio.passthroughPreferred);
        }
    } else {
        nativeSeekPrerollHoldingAudio_ = false;
        audioPlayer_.RequestStop();
    }
    if (!audioSeeked) {
        nativeSeekPrerollHoldingAudio_ = false;
        return false;
    }

    pendingPausedFrameRefresh_ = false;
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    heldNativeFrameNeedsPresent_ = heldNativeFrame_.has_value() && heldNativeFrame_->HasContent();
    lastNativeStatsLog_ = {};
    if (d3dRenderer_) {
        d3dRenderer_->ResetRenderStats();
    }
    if (webUiActive_ && webUiPlayerRouteActive_) {
        UpdateVideoHost();
    }
    UpdateBufferingOverlay();
    LogApp(LogLevel::Debug, L"native runtime seek position=" + FormatTimecode(snapshot.position));
    SetPlaybackTimer(true);
    return true;
}

bool MainWindow::PrepareNativeEnhancedPlaybackBeforePlay(const PlaybackSessionSnapshot& snapshot) {
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !nativeVideoDecoder_) {
        return true;
    }

    const auto settings = controller_.Settings();
    const bool wantsEnhancedPlayback =
        NativeCmv4ToggleRequiresDecoderRestart(snapshot) &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off;
    if (!wantsEnhancedPlayback) {
        return true;
    }

    const bool enableDolbyVisionEnhancementDecode =
        MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video);
    if (!enableDolbyVisionEnhancementDecode) {
        return true;
    }

    audioPlayer_.RequestStop();
    pendingPausedFrameRefresh_ = false;
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    heldNativeFrameNeedsPresent_ = false;
    lastNativeStatsLog_ = {};

    if (d3dRenderer_) {
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }
    MarkLayoutDirty();
    EnsureLayout();
    EnsureVideoHost();

    bool videoReadyToPreroll =
        nativeVideoDecoder_->IsRunning() &&
        !nativeVideoDecoder_->OneShotFrame() &&
        nativeVideoDecoder_->DolbyVisionEnhancementDecodeEnabled();

    if (videoReadyToPreroll) {
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        LogApp(LogLevel::Debug,
               L"native enhanced preroll gate reuse decoder position=" + FormatTimecode(snapshot.position));
    } else {
        QueueDeferredRuntimeStart(true, true);
        StopRuntimeAsync(false);
        return false;
    }
    // Enhancement readiness is observed through normal frame notifications;
    // the window thread never waits for decoder preroll.
    return true;
}

void MainWindow::RefreshPausedNativeFrame(const PlaybackSessionSnapshot& snapshot,
                                          const bool forceDecoderRestart) {
    if (closePending_) {
        return;
    }
    if (backend_ != PlaybackBackend::NativeFfmpegD3D11 ||
        !snapshot.media.has_value() ||
        !snapshot.media->hasVideo ||
        !nativeVideoDecoder_) {
        return;
    }
    if (RuntimeStopInProgress()) {
        QueuePausedNativeFrameRefresh(forceDecoderRestart);
        return;
    }
    EnsureVideoHost();
    if (!videoHostReady_ || !d3dRenderer_ || !d3dRenderer_->IsReady()) {
        if (d3dRenderer_ && d3dRenderer_->State() == D3D11RendererState::Initializing) {
            QueuePausedNativeFrameRefresh(forceDecoderRestart);
            LogApp(LogLevel::Debug, L"paused frame refresh deferred while d3d initializes");
        } else {
            LogApp(LogLevel::Error, L"paused frame refresh skipped because d3d is unavailable");
        }
        return;
    }

    if (!forceDecoderRestart && d3dRenderer_) {
        const auto settings = controller_.Settings();
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }

    if (!forceDecoderRestart && nativeVideoDecoder_->IsRunning()) {
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

    if (forceDecoderRestart) {
        LogApp(LogLevel::Debug, L"paused native frame refresh requires decoder restart");
    }
    if (RuntimeBackendsRunning()) {
        QueuePausedNativeFrameRefresh(forceDecoderRestart);
        StopRuntimeAsync(false);
        return;
    }
    nativeFrameHoldVisible_ = heldNativeFrame_.has_value();
    pendingPausedFrameRefresh_ = true;
    heldNativeFrameNeedsPresent_ = false;
    lastNativeStatsLog_ = {};
    MarkLayoutDirty();
    EnsureLayout();
    const auto settings = controller_.Settings();
    if (d3dRenderer_) {
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
        d3dRenderer_->ResetRenderStats();
    }
    const bool preferDolbyVisionHdrOutput = snapshot.media->dolbyVisionDetected;
    const bool preferHardwareDecode = snapshot.media->selectedDecodePath == L"ffmpeg_d3d11va";
    const bool enableDolbyVisionEnhancementDecode =
        MediaHasDolbyVisionEnhancementStream(snapshot.media) &&
        settings.video.dolbyVision != anvil::playback::DolbyVisionMode::Off &&
        Cmv4ApproxActiveForPlayback(snapshot, settings.video);
    if (videoHostReady_ && d3dRenderer_) {
        runtimeCleanupPending_.store(true, std::memory_order_release);
    }
    const bool started = videoHostReady_ &&
                         d3dRenderer_ &&
                         nativeVideoDecoder_->Start(snapshot.media->path,
                                                    snapshot.position,
                                                    hwnd_,
                                                    kNativeVideoFrameReadyMessage,
                                                    0,
                                                    {},
                                                    preferHardwareDecode,
                                                    d3dRenderer_->Device(),
                                                    settings.video.selectedTrackIndex,
                                                    settings.subtitles.preferredLanguage,
                                                    settings.subtitles.selectedTrackIndex,
                                                    std::chrono::milliseconds{settings.subtitles.subtitleDelayMs},
                                                    settings.subtitles.externalSubtitleAutoLoad,
                                                    settings.subtitles.externalSubtitlePath,
                                                     true,
                                                     preferDolbyVisionHdrOutput,
                                                     enableDolbyVisionEnhancementDecode,
                                                    NativeAudioPacketSink{},
                                                    windowLifetimeCookie_);
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
    if (closePending_) {
        return;
    }
    LogApp(LogLevel::Info, L"open path=" + path.wstring());
    refreshRateController_.Restore();
    refreshRateSyncEnabled_ = DisplayRefreshRateController::LoadGlobalEnabled();
    refreshRateMaximumMultiple_ = DisplayRefreshRateController::LoadMaximumMultipleEnabled();
    refreshRateSyncOverridden_ = false;
    refreshRateMaximumMultipleOverridden_ = false;
    refreshRateSyncUnavailable_ = false;
    audioPassthroughOverridden_ = false;
    {
        auto settings = controller_.Settings();
        settings.audio.passthroughPreferred = LoadAudioPassthroughSetting().value_or(false);
        controller_.ApplySettings(settings);
    }
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    if (RuntimeBackendsRunning() || RuntimeStopInProgress()) {
        StopRuntimeAsync(true);
    } else {
        FinishRuntimeStopVisuals(true);
    }
    SetPlaybackTimer(false);
    controller_.BeginOpen();
    const uint64_t generation = playbackSupervisor_ ? playbackSupervisor_->Open(path, autoplay) : 0;
    const bool queued = generation != 0;
    LogApp(queued ? LogLevel::Info : LogLevel::Error,
           L"open queued=" + std::wstring(queued ? L"true" : L"false") +
               L" generation=" + std::to_wstring(generation) +
               L" autoplay=" + (autoplay ? L"true" : L"false"));
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::CompleteOpenPath(PlaybackSupervisor::OpenCompletion completion) {
    const auto path = completion.prepared.path;
    if (completion.prepared.succeeded) {
        cachedCapabilities_ = completion.prepared.capabilities;
        capabilitiesCached_ = true;
    }
    const bool opened = controller_.CommitMedia(std::move(completion.prepared));
    LogApp(opened ? LogLevel::Info : LogLevel::Error,
           std::wstring(L"open result=") + (opened ? L"true" : L"false") +
               L" generation=" + std::to_wstring(completion.generation) +
               L" autoplay=" + (completion.autoplay ? L"true" : L"false"));
    if (opened) {
        systemDolbyVisionFallbackForCurrentMedia_ = false;
        auto settings = controller_.Settings();
        bool settingsChanged = false;
        if (!settings.subtitles.externalSubtitlePath.empty()) {
            settings.subtitles.externalSubtitlePath.clear();
            settingsChanged = true;
        }
        if (!settings.danmaku.externalDanmakuPath.empty()) {
            settings.danmaku.externalDanmakuPath.clear();
            settings.danmaku.enabled = false;
            settingsChanged = true;
        }
        if (settingsChanged) {
            controller_.ApplySettings(settings);
        }
        UpdateInspectorMediaLists(path);
        ApplyAutomaticDisplayFormatForCurrentMedia();
        ApplyDefaultHdrControlsForCurrentMedia();
        MarkLayoutDirty();
        EnsureLayout();
    }
    if (opened && completion.autoplay) {
        StartPlayback();
        const double startRatio = std::exchange(pendingStartPositionRatio_, 0.0);
        if (startRatio > 0.001) {
            const auto snapshot = controller_.Snapshot();
            if (snapshot.media.has_value() && snapshot.media->duration.count() > 0) {
                SeekToPosition(std::chrono::milliseconds{
                    static_cast<long long>(static_cast<double>(snapshot.media->duration.count()) *
                                           std::clamp(startRatio, 0.0, 0.99))});
                LogApp(LogLevel::Info, L"resume from ratio=" + std::to_wstring(startRatio));
            }
        }
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::LoadRecentMedia() {
    recentMedia_.clear();
    if (!EnsureRecentMediaWorker()) {
        return;
    }

    const auto state = recentMediaWriter_;
    const uint64_t generation = gRecentMediaLoadGeneration.fetch_add(1) + 1;
    state->notificationWindow.store(hwnd_);
    {
        std::scoped_lock lock(state->mutex);
        if (state->stopping.load()) {
            return;
        }
        state->loadRequested = true;
        state->loadGeneration = generation;
    }
    state->wake.notify_one();
}

bool MainWindow::EnsureRecentMediaWorker() const {
    if (recentMediaWriter_ && !recentMediaWriter_->stopping.load()) {
        return true;
    }

    auto state = std::make_shared<RecentMediaWriterState>();
    state->outputPath = RecentMediaPath();
    state->logSink = controller_.LogSink();
    state->notificationWindow.store(hwnd_);

    try {
        std::thread worker([state]() {
            for (;;) {
                bool loadRequested = false;
                uint64_t loadGeneration = 0;
                std::optional<std::vector<std::filesystem::path>> itemsToWrite;
                {
                    std::unique_lock lock(state->mutex);
                    state->wake.wait(lock, [&]() {
                        return state->stopping.load() ||
                               state->loadRequested ||
                               state->pendingItems.has_value();
                    });

                    if (state->stopping.load()) {
                        state->loadRequested = false;
                        if (!state->pendingItems.has_value()) {
                            return;
                        }
                    } else if (state->loadRequested) {
                        loadRequested = true;
                        loadGeneration = state->loadGeneration;
                        state->loadRequested = false;
                    }

                    if (!loadRequested && state->pendingItems.has_value()) {
                        const uint64_t observedGeneration = state->pendingGeneration;
                        if (!state->stopping.load()) {
                            state->wake.wait_for(lock, kRecentMediaSaveCoalesceDelay, [&]() {
                                return state->stopping.load() ||
                                       state->loadRequested ||
                                       state->pendingGeneration != observedGeneration;
                            });
                            if (!state->stopping.load() &&
                                (state->loadRequested ||
                                 state->pendingGeneration != observedGeneration)) {
                                continue;
                            }
                        }
                        itemsToWrite.emplace(std::move(*state->pendingItems));
                        state->pendingItems.reset();
                    }
                }

                if (loadRequested) {
                    std::vector<std::filesystem::path> loadedItems;
                    std::ifstream input(state->outputPath, std::ios::binary);
                    std::string line;
                    while (!state->stopping.load() &&
                           loadedItems.size() < kMaxRecentMedia &&
                           std::getline(input, line)) {
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        const auto decoded = Utf8ToWide(line.c_str());
                        if (decoded.empty()) {
                            continue;
                        }
                        const auto normalizedPath = NormalizeListPath(decoded);
                        if (normalizedPath.empty() ||
                            !IsMediaFilePath(normalizedPath) ||
                            std::find(loadedItems.begin(), loadedItems.end(), normalizedPath) != loadedItems.end()) {
                            continue;
                        }
                        loadedItems.push_back(normalizedPath);
                    }

                    HWND target = nullptr;
                    {
                        std::scoped_lock lock(state->mutex);
                        if (!state->stopping.load() && loadGeneration == state->loadGeneration) {
                            state->loadedItems = std::move(loadedItems);
                            state->completedLoadGeneration = loadGeneration;
                            target = state->notificationWindow.load();
                        }
                    }
                    if (target) {
                        PostMessageW(target,
                                     kRecentMediaLoadCompleteMessage,
                                     static_cast<WPARAM>(loadGeneration),
                                     0);
                    }
                    continue;
                }

                if (!itemsToWrite.has_value()) {
                    continue;
                }

                std::error_code error;
                std::filesystem::create_directories(state->outputPath.parent_path(), error);
                if (error) {
                    if (state->logSink) {
                        state->logSink->Write(
                            LogLevel::Warning,
                            L"app",
                            L"recent media directory failed path=" +
                                state->outputPath.parent_path().wstring());
                    }
                    continue;
                }

                std::ofstream output(state->outputPath, std::ios::binary | std::ios::trunc);
                if (!output) {
                    if (state->logSink) {
                        state->logSink->Write(LogLevel::Warning,
                                              L"app",
                                              L"recent media save failed path=" +
                                                  state->outputPath.wstring());
                    }
                    continue;
                }
                for (const auto& item : *itemsToWrite) {
                    output << WideToUtf8(item.wstring()) << '\n';
                }
            }
        });
        worker.detach();
    } catch (...) {
        LogApp(LogLevel::Warning, L"recent media worker failed to start");
        return false;
    }

    recentMediaWriter_ = std::move(state);
    return true;
}

void MainWindow::SaveRecentMedia() const {
    if (!EnsureRecentMediaWorker()) {
        return;
    }
    const auto state = recentMediaWriter_;
    {
        std::scoped_lock lock(state->mutex);
        if (state->stopping.load()) {
            return;
        }
        state->pendingItems.emplace(recentMedia_);
        ++state->pendingGeneration;
    }
    state->wake.notify_one();
}

void MainWindow::StartInspectorFolderScan(const std::filesystem::path& mediaPath) {
    if (closePending_ || IsInspectorNetworkPath(mediaPath)) {
        return;
    }

    if (!inspectorFolderScan_) {
        auto state = std::make_shared<InspectorFolderScanState>();
        state->notificationWindow.store(hwnd_);
        try {
            std::thread worker([state]() {
                for (;;) {
                    std::filesystem::path requestedPath;
                    uint64_t generation = 0;
                    {
                        std::unique_lock lock(state->mutex);
                        state->wake.wait(lock, [&]() {
                            return state->stopping.load() || state->pendingPath.has_value();
                        });
                        if (state->stopping.load()) {
                            return;
                        }
                        requestedPath = std::move(*state->pendingPath);
                        state->pendingPath.reset();
                        generation = state->requestedGeneration.load();
                    }

                    const auto superseded = [&]() {
                        return state->stopping.load() ||
                               state->requestedGeneration.load() != generation;
                    };
                    std::vector<std::filesystem::path> entries;
                    entries.reserve(kMaxInspectorFolderEntries);
                    if (!IsInspectorNetworkPath(requestedPath)) {
                        const auto folder = requestedPath.parent_path();
                        std::error_code error;
                        std::filesystem::directory_iterator iterator(
                            folder,
                            std::filesystem::directory_options::skip_permission_denied,
                            error);
                        const std::filesystem::directory_iterator end;
                        std::size_t inspectedEntryCount = 0;
                        while (!error && iterator != end &&
                               inspectedEntryCount < kMaxInspectorFolderEntries &&
                               !superseded()) {
                            ++inspectedEntryCount;
                            std::error_code entryError;
                            if (iterator->is_regular_file(entryError)) {
                                const auto entryPath = NormalizeListPath(iterator->path());
                                if (IsMediaFilePath(entryPath)) {
                                    entries.push_back(entryPath);
                                }
                            }
                            iterator.increment(error);
                        }
                    }
                    if (superseded()) {
                        continue;
                    }

                    if (std::find(entries.begin(), entries.end(), requestedPath) == entries.end()) {
                        if (entries.size() == kMaxInspectorFolderEntries) {
                            entries.pop_back();
                        }
                        entries.push_back(requestedPath);
                    }
                    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
                        return LowerCopy(lhs.filename().wstring()) < LowerCopy(rhs.filename().wstring());
                    });

                    HWND target = nullptr;
                    {
                        std::scoped_lock lock(state->mutex);
                        if (superseded()) {
                            continue;
                        }
                        state->entries = std::move(entries);
                        state->completedGeneration = generation;
                        target = state->notificationWindow.load();
                    }
                    if (target) {
                        PostMessageW(target,
                                     kInspectorFolderScanCompleteMessage,
                                     static_cast<WPARAM>(generation),
                                     0);
                    }
                }
            });
            worker.detach();
        } catch (...) {
            LogApp(LogLevel::Warning, L"inspector folder worker failed to start");
            return;
        }
        inspectorFolderScan_ = std::move(state);
    }

    const auto state = inspectorFolderScan_;
    const uint64_t generation = gInspectorFolderScanGeneration.fetch_add(1) + 1;
    state->notificationWindow.store(hwnd_);
    {
        std::scoped_lock lock(state->mutex);
        if (state->stopping.load()) {
            return;
        }
        state->pendingPath = mediaPath;
        state->requestedGeneration.store(generation);
    }
    inspectorFolderScanGeneration_ = generation;
    state->wake.notify_one();
}

void MainWindow::InvalidateBackgroundListWorkers() noexcept {
    if (inspectorFolderScan_) {
        inspectorFolderScan_->notificationWindow.store(nullptr);
        inspectorFolderScan_->stopping.store(true);
        inspectorFolderScan_->wake.notify_all();
        inspectorFolderScan_.reset();
    }
    if (recentMediaWriter_) {
        recentMediaWriter_->notificationWindow.store(nullptr);
        recentMediaWriter_->stopping.store(true);
        recentMediaWriter_->wake.notify_all();
        recentMediaWriter_.reset();
    }
}

void MainWindow::UpdateInspectorMediaLists(const std::filesystem::path& path) {
    if (IsInspectorNetworkPath(path)) {
        if (inspectorFolderScan_) {
            const uint64_t generation = gInspectorFolderScanGeneration.fetch_add(1) + 1;
            {
                std::scoped_lock lock(inspectorFolderScan_->mutex);
                inspectorFolderScan_->pendingPath.reset();
                inspectorFolderScan_->requestedGeneration.store(generation);
            }
            inspectorFolderScanGeneration_ = generation;
        }
        currentFolderEntries_.clear();
        PostWebUiState();
        return;
    }

    const auto normalizedPath = NormalizeListPath(path);
    if (normalizedPath.empty()) {
        return;
    }

    recentMedia_.erase(std::remove(recentMedia_.begin(), recentMedia_.end(), normalizedPath), recentMedia_.end());
    recentMedia_.insert(recentMedia_.begin(), normalizedPath);
    if (recentMedia_.size() > kMaxRecentMedia) {
        recentMedia_.resize(kMaxRecentMedia);
    }
    SaveRecentMedia();

    currentFolderEntries_.clear();
    StartInspectorFolderScan(normalizedPath);
}

void MainWindow::StartPlayback() {
    if (closePending_) {
        return;
    }
    nativeSeekPrerollHoldingAudio_ = false;
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    const auto before = controller_.Snapshot();

    controller_.Play();
    const auto snapshot = controller_.Snapshot();
    if (fullscreen_ && refreshRateSyncEnabled_ && snapshot.media.has_value() && snapshot.media->videoFrameRate > 0.0) {
        refreshRateSyncUnavailable_ =
            !refreshRateController_.ApplyForWindow(hwnd_, snapshot.media->videoFrameRate, refreshRateMaximumMultiple_);
    }
    LogApp(LogLevel::Info, L"start playback state=" + ToDisplayString(snapshot.state) + L" hasMedia=" + (snapshot.media.has_value() ? L"true" : L"false"));
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        const auto settings = controller_.Settings();
        const bool enhancedPlaybackNeedsRestart = NativeCmv4ToggleNeedsDecoderRefresh(snapshot, settings.video);
        const bool resumedSystemDolbyVision =
            before.state == PlaybackState::Paused &&
            !RuntimeStopInProgress() &&
            systemDolbyVisionPlayer_.IsActive();
        const bool resumedNativeRuntime =
            before.state == PlaybackState::Paused &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
            !RuntimeStopInProgress() &&
            nativeVideoDecoder_ &&
            nativeVideoDecoder_->IsRunning() &&
            !nativeVideoDecoder_->OneShotFrame() &&
            !audioPlayer_.IsStopping() &&
            !enhancedPlaybackNeedsRestart;
        if (resumedSystemDolbyVision) {
            systemDolbyVisionPlayer_.SetVolume(snapshot.volume);
            systemDolbyVisionPlayer_.SetPlaybackRate(snapshot.playbackRate);
            if (!systemDolbyVisionPlayer_.Play()) {
                systemDolbyVisionFallbackForCurrentMedia_ = true;
                RequestRuntimeStart(true, false);
            }
        } else if (resumedNativeRuntime) {
            if (d3dRenderer_) {
                d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
                d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
                d3dRenderer_->ResetRenderStats();
            }
            nativeVideoDecoder_->SetPaused(false, snapshot.position);
            pendingPausedFrameRefresh_ = false;
            nativeFrameHoldVisible_ = false;
            heldNativeFrameNeedsPresent_ = false;
            if (snapshot.media->hasAudio &&
                settings.audio.selectedTrackIndex != anvil::playback::kAudioTrackOff) {
                audioPlayer_.SetPlaybackRate(snapshot.playbackRate);
                bool audioStarted = audioPlayer_.IsRunning() && audioPlayer_.ResumePacketStream();
                if (!audioStarted) {
                    audioStarted = audioPlayer_.Start(snapshot.media->path,
                                                      snapshot.position,
                                                      snapshot.volume,
                                                      settings.audio.selectedTrackIndex,
                                                      settings.audio.passthroughPreferred);
                }
                LogApp(audioStarted ? LogLevel::Debug : LogLevel::Warning,
                       L"native paused runtime resume audio=" + std::wstring(audioStarted ? L"true" : L"false"));
                if (!audioStarted) {
                    FailPlaybackRuntime(L"Native audio failed to resume");
                    return;
                }
            } else {
                audioPlayer_.RequestStop();
            }
        } else {
            if (before.state == PlaybackState::Paused &&
                backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                nativeVideoDecoder_ &&
                nativeVideoDecoder_->IsRunning()) {
                LogApp(LogLevel::Debug,
                       L"native paused runtime resume requires restart one_shot=" +
                           std::wstring(nativeVideoDecoder_->OneShotFrame() ? L"true" : L"false") +
                           L" enhanced_decoder=" +
                           std::wstring(nativeVideoDecoder_->DolbyVisionEnhancementDecodeEnabled() ? L"true" : L"false"));
            }
            RequestRuntimeStart(false, false);
        }
        SetPlaybackTimer(true);
    }
    MarkLayoutDirty();
    EnsureLayout();
    if (fullscreen_ && before.state == PlaybackState::Paused) {
        ShowFullscreenTransport(L"resume");
    }
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::PausePlayback() {
    nativeSeekPrerollHoldingAudio_ = false;
    ClearDeferredRuntimeStart();
    controller_.Pause();
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    const auto snapshot = controller_.Snapshot();
    if (RuntimeStopInProgress()) {
        SetPlaybackTimer(false);
    } else if (systemDolbyVisionPlayer_.IsActive()) {
        systemDolbyVisionPlayer_.Pause();
    } else if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_ &&
        nativeVideoDecoder_->IsRunning() &&
        d3dRenderer_) {
        const bool hadHeldFrame = heldNativeFrame_.has_value() && heldNativeFrame_->HasContent();
        nativeVideoDecoder_->SetPaused(true, snapshot.position);
        bool hasCpuPixels = false;
        if (nativeVideoDecoder_->VisitLatestFrame([&](const NativeVideoFrame& frame) {
                hasCpuPixels = frame.HasPixels();
                if (!hadHeldFrame) {
                    d3dRenderer_->Render(frame);
                }
                (void)ReplaceHeldNativeFrame(frame);
            })) {
            nativeFrameHoldVisible_ = true;
            heldNativeFrameNeedsPresent_ = false;
            LogApp(LogLevel::Debug,
                   L"captured native pause freeze frame pixels=" +
                       std::wstring(hasCpuPixels ? L"true" : L"false") +
                       L" rendered=" +
                       std::wstring(!hadHeldFrame ? L"true" : L"false"));
        }
        audioPlayer_.HoldPacketStream(snapshot.position);
    } else {
        StopRuntimeAsync(false);
    }
    SetPlaybackTimer(false);
    MarkLayoutDirty();
    EnsureLayout();
    if (fullscreen_) {
        ShowFullscreenTransport(L"pause");
    }
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
    if (fullscreen_ && heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
        // Pausing stops decoder-driven presents. Submit the retained frame once
        // after queuing the new UI bitmap so the transport is composited even
        // when the renderer's cached output was invalidated by a recent resize.
        RenderHeldNativeFrame(false);
        LogApp(LogLevel::Debug, L"fullscreen_transport paused frame submitted with ui overlay");
    }
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
    RestoreAutomaticDisplayFormat();
    refreshRateController_.Restore();
    ClearDeferredRuntimeStart();
    deferredPausedFrameRefresh_ = false;
    deferredPausedFrameRefreshForceRestart_ = false;
    controller_.Stop();
    if (RuntimeStopInProgress()) {
        controller_.SetPlaybackRate(1.0);
        temporaryRateActive_ = false;
    } else {
        SetTemporaryPlaybackRate(1.0);
    }
    StopRuntimeAsync(true);
    SetPlaybackTimer(false);
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::RestartPlaybackIfPlaying(const bool waitForPreroll) {
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Playing &&
        snapshot.media.has_value() &&
        (snapshot.media->hasVideo || snapshot.media->hasAudio)) {
        RequestRuntimeStart(true, waitForPreroll);
    }
}

void MainWindow::SeekRelative(const std::chrono::milliseconds delta) {
    const auto before = controller_.Snapshot();
    controller_.SeekRelative(delta);
    const auto after = controller_.Snapshot();
    if (systemDolbyVisionPlayer_.IsActive()) {
        systemDolbyVisionPlayer_.Seek(after.position);
    } else if (after.state == PlaybackState::Paused &&
        after.media.has_value() &&
        after.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(after);
    } else if (after.state == PlaybackState::Playing &&
               !(before.state == PlaybackState::Playing &&
                 !RuntimeStopInProgress() &&
                 SeekNativeRuntime(after))) {
        RestartPlaybackIfPlaying(false);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::SeekToPosition(const std::chrono::milliseconds position) {
    const auto before = controller_.Snapshot();
    controller_.Seek(position);
    const auto after = controller_.Snapshot();
    if (systemDolbyVisionPlayer_.IsActive()) {
        systemDolbyVisionPlayer_.Seek(after.position);
    } else if (after.state == PlaybackState::Paused &&
        after.media.has_value() &&
        after.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(after);
    } else if (after.state == PlaybackState::Playing &&
               !(before.state == PlaybackState::Playing &&
                 !RuntimeStopInProgress() &&
                 SeekNativeRuntime(after))) {
        RestartPlaybackIfPlaying(false);
    }
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
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
        refreshRateSyncUnavailable_ = false;
        const auto snapshot = controller_.Snapshot();
        if (refreshRateSyncEnabled_ && snapshot.media.has_value() && snapshot.media->videoFrameRate > 0.0) {
            const bool matched = refreshRateController_.ApplyForWindow(hwnd_, snapshot.media->videoFrameRate, refreshRateMaximumMultiple_);
            refreshRateSyncUnavailable_ = !matched;
            LogApp(matched ? LogLevel::Info : LogLevel::Warning,
                   matched
                       ? L"refresh rate sync applied source_fps=" + std::to_wstring(snapshot.media->videoFrameRate) +
                             L" target_hz=" + std::to_wstring(refreshRateController_.AppliedRefreshRate())
                       : L"refresh rate sync exact mode unavailable source_fps=" + std::to_wstring(snapshot.media->videoFrameRate));
        }
        previousStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
        previousExStyle_ = GetWindowLongW(hwnd_, GWL_EXSTYLE);
        previousPlacement_.length = sizeof(previousPlacement_);
        GetWindowPlacement(hwnd_, &previousPlacement_);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        // A maximized window keeps WS_MAXIMIZE even after the overlapped-window
        // chrome is removed. Clear the show-state bits so fullscreen always has
        // the same monitor-sized client area, regardless of how it was entered.
        const LONG fullscreenStyle =
            (previousStyle_ & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE | WS_MINIMIZE)) | WS_POPUP;
        fullscreen_ = true;
        SetWindowLongW(hwnd_, GWL_STYLE, fullscreenStyle);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, previousExStyle_ & ~WS_EX_WINDOWEDGE);
        SetWindowPos(hwnd_,
                     HWND_TOP,
                     monitorInfo.rcMonitor.left,
                     monitorInfo.rcMonitor.top,
                     RectWidth(monitorInfo.rcMonitor),
                     RectHeight(monitorInfo.rcMonitor),
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
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
        // Refresh-rate matching belongs to exclusive player fullscreen only.
        // Restore the desktop mode before returning to a windowed/maximized
        // presentation so playback cannot leave the desktop at the media rate.
        refreshRateController_.Restore();
        refreshRateSyncUnavailable_ = false;
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
    const auto fullscreenSnapshot = controller_.Snapshot();
    if (fullscreen_ && fullscreenSnapshot.state == PlaybackState::Paused) {
        ShowFullscreenTransport(L"enter_fullscreen_paused");
        if (heldNativeFrame_.has_value() && heldNativeFrame_->HasContent()) {
            // ResizeBuffers invalidates the renderer's cached output. A paused
            // decoder will not produce another frame, so explicitly rebuild it.
            RenderHeldNativeFrame(false);
        } else {
            // Windowed pause can race runtime retirement and leave no retained
            // frame. ResizeBuffers has already invalidated the renderer cache,
            // so request a one-shot paused frame instead of waiting forever.
            LogApp(LogLevel::Debug, L"fullscreen_transport paused resize requires frame refresh");
            RefreshPausedNativeFrame(fullscreenSnapshot);
        }
    } else if (fullscreen_ &&
               hasLastFullscreenCursorClient_ &&
               IsFullscreenTransportActivationPoint(lastFullscreenCursorClient_)) {
        ShowFullscreenTransport(L"enter_fullscreen_activation");
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::SetRefreshRateSyncEnabled(const bool enabled) {
    refreshRateSyncOverridden_ = true;
    refreshRateSyncEnabled_ = enabled;
    if (!enabled) {
        refreshRateController_.Restore();
        refreshRateSyncUnavailable_ = false;
        MarkLayoutDirty();
        EnsureLayout();
        UpdateVideoHost();
    } else if (fullscreen_) {
        const auto snapshot = controller_.Snapshot();
        if (snapshot.media.has_value() && snapshot.media->videoFrameRate > 0.0) {
            refreshRateSyncUnavailable_ =
                !refreshRateController_.ApplyForWindow(hwnd_, snapshot.media->videoFrameRate, refreshRateMaximumMultiple_);
        } else {
            refreshRateSyncUnavailable_ = false;
        }
    } else {
        refreshRateSyncUnavailable_ = false;
    }
    PostWebUiState();
}

void MainWindow::SetRefreshRateMaximumMultiple(const bool enabled) {
    refreshRateMaximumMultipleOverridden_ = true;
    refreshRateMaximumMultiple_ = enabled;
    if (refreshRateSyncEnabled_ && fullscreen_) {
        refreshRateController_.Restore();
        const auto snapshot = controller_.Snapshot();
        if (snapshot.media.has_value() && snapshot.media->videoFrameRate > 0.0) {
            refreshRateSyncUnavailable_ = !refreshRateController_.ApplyForWindow(hwnd_, snapshot.media->videoFrameRate, refreshRateMaximumMultiple_);
        } else {
            refreshRateSyncUnavailable_ = false;
        }
    } else if (!fullscreen_) {
        refreshRateController_.Restore();
        refreshRateSyncUnavailable_ = false;
    }
    PostWebUiState();
}

void MainWindow::ApplyGlobalRefreshRatePreferences() {
    const bool globalSyncEnabled = DisplayRefreshRateController::LoadGlobalEnabled();
    const bool globalMaximumMultiple = DisplayRefreshRateController::LoadMaximumMultipleEnabled();
    bool changed = false;
    if (!refreshRateSyncOverridden_ && refreshRateSyncEnabled_ != globalSyncEnabled) {
        refreshRateSyncEnabled_ = globalSyncEnabled;
        changed = true;
    }
    if (!refreshRateMaximumMultipleOverridden_ && refreshRateMaximumMultiple_ != globalMaximumMultiple) {
        refreshRateMaximumMultiple_ = globalMaximumMultiple;
        changed = true;
    }
    if (changed) {
        refreshRateController_.Restore();
        const auto snapshot = controller_.Snapshot();
        if (fullscreen_ && refreshRateSyncEnabled_ && snapshot.media.has_value() && snapshot.media->videoFrameRate > 0.0) {
            refreshRateSyncUnavailable_ = !refreshRateController_.ApplyForWindow(hwnd_, snapshot.media->videoFrameRate, refreshRateMaximumMultiple_);
        } else {
            refreshRateSyncUnavailable_ = false;
        }
    }
    PostWebUiState(true);
}

void MainWindow::ApplyGlobalVideoPassthroughPreferences() {
    const bool autoDisplayFormat = LoadVideoBooleanSetting(L"AutoDisplayFormat").value_or(false);
    const bool displayMetadata = LoadVideoBooleanSetting(L"DisplayMetadataPassthrough").value_or(false);
    const bool dolbySystemPipeline =
        LoadVideoBooleanSetting(L"DolbyVisionSystemPipelineExperimental").value_or(false);
    const int displayPeakBrightnessNits =
        LoadVideoDwordSetting(L"DisplayPeakBrightnessNits").value_or(0);
    SetAutomaticDisplayFormat(autoDisplayFormat);
    if (!autoDisplayFormat) {
        SetDisplayMetadataPassthrough(displayMetadata);
        SetDolbyVisionSystemPipelineExperimental(dolbySystemPipeline);
    }
    SetDisplayPeakBrightnessNits(displayPeakBrightnessNits);
}

void MainWindow::ApplyGlobalAudioPassthroughPreferences() {
    const bool enabled = LoadAudioPassthroughSetting().value_or(false);
    const auto snapshot = controller_.Snapshot();
    if (!audioPassthroughOverridden_ && !snapshot.media.has_value()) {
        auto settings = controller_.Settings();
        settings.audio.passthroughPreferred = enabled;
        controller_.ApplySettings(settings);
    }
    LogApp(LogLevel::Info,
           L"audio passthrough global_default=" + std::wstring(enabled ? L"preferred" : L"disabled") +
               (snapshot.media.has_value() ? L" applies_to=next_media" : L" applies_to=current_empty_session"));
    PostWebUiState(true);
}

}  // namespace anvil::app
