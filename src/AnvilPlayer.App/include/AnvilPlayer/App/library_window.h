#pragma once

#include "AnvilPlayer/App/web_ui_host.h"

#include <shellapi.h>
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::app {

struct LibraryWindowAsyncState;

// A lightweight top-level window that hosts the WebView2 media-library UI
// (the #/library route). It owns its own WebUiHost and handles only the
// library-source commands (open file, pick/scan local folder, SMB/WebDAV
// listing, insecure-certificate toggle, debug logging). Playback is delegated
// to the player window via the PlaybackRequest callback.
//
// Lifecycle note: closing this window is treated as "close the application".
// The message loop should continue running as long as either the library or
// the player window is alive.
class LibraryWindow {
public:
    using PlaybackRequest = std::function<void(const std::filesystem::path& path,
                                               double startPositionRatio,
                                               int audioTrackIndex,
                                               int subtitleTrackIndex)>;
    using QuitHandler = std::function<void()>;

    LibraryWindow();
    ~LibraryWindow();

    LibraryWindow(const LibraryWindow&) = delete;
    LibraryWindow& operator=(const LibraryWindow&) = delete;

    void SetPlaybackRequest(PlaybackRequest callback);
    void SetAllowInsecureCertificatesRequest(std::function<void(bool)> callback);
    void SetFocusPlayerRequest(std::function<void()> callback);
    void SetUiLanguageChangedRequest(std::function<void()> callback);
    void SetRefreshRatePreferencesChangedRequest(std::function<void()> callback);
    void SetVideoPassthroughPreferencesChangedRequest(std::function<void()> callback);
    // Relays a serialized Emby playback report (from the library WebView) to
    // the player window so it can report progress despite separate storage.
    void SetEmbyPlaybackReportRelay(std::function<void(const std::wstring&)> callback);
    void SetQuitHandler(QuitHandler handler);
    // Fired from the library window's WM_TIMER (used for the Emby report retry).
    void SetTimerHandler(std::function<void()> callback);
    void SetDebugLogHandler(std::function<void(const std::wstring&)> callback);

    bool Create(HINSTANCE instance);
    void Show(int commandShow) const;
    HWND Handle() const { return hwnd_; }
    bool DeliverLocalPlaybackProgress(const std::wstring& progressJson) const;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool TryCreateWebUi();
    std::filesystem::path WebUiRoot() const;
    void HandleWebUiMessage(std::wstring_view message);
    void StartMediaDetailsProbe(std::wstring requestId, std::filesystem::path path);
    void StartBilibiliTrailerSearch(std::wstring requestId, std::wstring keyword);
    void PostScanResult(const std::wstring& json);
    void StartLocalFolderScan(std::filesystem::path folder,
                              std::wstring username,
                              std::wstring password,
                              bool postPickedResult);
    void StartSmbDirectoryList(std::wstring requestId,
                               std::wstring host,
                               std::wstring path,
                               std::wstring username,
                               std::wstring password);
    void StartSmbConnection(std::filesystem::path path,
                            std::wstring username,
                            std::wstring password);
    void StartWebDavDirectoryList(std::wstring requestId,
                                  std::wstring url,
                                  std::wstring username,
                                  std::wstring password);
    void StartWebDavScan(std::wstring url,
                         std::wstring username,
                         std::wstring password);
    void HandleAsyncIoResult(std::uintptr_t resultId);
    void CancelAsyncIo() noexcept;
    bool DeferPlaybackForPendingSmbConnection(const std::filesystem::path& path,
                                              double startPositionRatio,
                                              int audioTrackIndex,
                                              int subtitleTrackIndex);
    void CompleteDeferredSmbPlayback();
    void OpenLocalFolderDialog();
    void OpenMediaFileDialog();
    void ApplyWindowChrome() const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = 96;
    std::unique_ptr<WebUiHost> webUiHost_;
    std::shared_ptr<LibraryWindowAsyncState> asyncIoState_;
    bool webUiActive_ = false;
    PlaybackRequest playbackRequest_;
    std::function<void()> uiLanguageChangedRequest_;
    std::function<void()> refreshRatePreferencesChangedRequest_;
    std::function<void()> videoPassthroughPreferencesChangedRequest_;
    std::function<void(bool)> allowInsecureCertificatesRequest_;
    std::function<void()> focusPlayerRequest_;
    std::function<void(const std::wstring&)> embyPlaybackReportRelay_;
    QuitHandler quitHandler_;
    std::function<void()> timerHandler_;
    std::function<void(const std::wstring&)> debugLogHandler_;
};

}  // namespace anvil::app
