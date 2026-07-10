#pragma once

#include "AnvilPlayer/App/app_arguments.h"
#include "AnvilPlayer/App/library_window.h"
#include "AnvilPlayer/App/main_window.h"

#include <memory>
#include <string>

namespace anvil::app {

// Process-level coordinator that owns the media-library window and the player
// window, and routes playback requests between them. There is exactly one
// Application per process; it owns the message loop lifecycle.
//
// Window model:
//   - The library window is always created at startup and shown.
//   - The player window is created on demand (first playback request) and may
//     be shown/hidden; closing it does NOT quit the app.
//   - Closing the library window quits the whole application.
class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Initializes COM/GDI+ and creates the library window. Returns false on
    // fatal init failure (caller should exit without running the loop).
    bool Initialize(HINSTANCE instance, int commandShow, const AppArguments& arguments);

    // Runs the modal message loop until the library window closes. Returns the
    // process exit code.
    int Run();

    // Opens (or switches) the player window to play `path`, optionally seeking
    // to startPositionRatio. Safe to call from the library window's message
    // handler (same thread). If the player window does not exist yet it is
    // created and shown; otherwise it is brought to the foreground.
    void OpenInPlayer(const std::filesystem::path& path, double startPositionRatio);

    // Brings the player window to the foreground, creating it if necessary.
    void FocusPlayer();

    // Relays a serialized Emby playback report (JSON object) from the library
    // window to the player window's WebView.
    void RelayEmbyPlaybackReport(const std::wstring& reportJson);

private:
    void EnsurePlayerWindow();
    void OnLibraryClosed();
    void OnPlayerClosed();
    void TryDeliverPendingEmbyReport();

    HINSTANCE instance_ = nullptr;
    AppArguments arguments_{};
    std::unique_ptr<LibraryWindow> library_;
    std::unique_ptr<MainWindow> player_;
    // Emby report relay: buffered when it arrives before the player WebView is
    // ready, then redelivered on a timer until consumed once.
    std::wstring pendingEmbyReport_;
    bool embyReportDelivered_ = false;
    int embyReportRetries_ = 0;
};

}  // namespace anvil::app
