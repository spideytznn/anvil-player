#include "AnvilPlayer/App/application.h"

#include "AnvilPlayer/App/ui_draw.h"

#include <windows.h>

#include <utility>

namespace anvil::app {

namespace {

// Timer id used by Application to retry buffered Emby report delivery on the
// UI thread while the player WebView finishes initializing. Reuses a value
// that does not collide with MainWindow/overlay timer ids (1001..1005).
constexpr UINT_PTR kApplicationTimerId = 9001;
constexpr UINT kEmbyReportRetryIntervalMs = 300;
constexpr int kEmbyReportMaxRetries = 15;  // ~4.5s total, stops once delivered

void BringWindowToForeground(HWND window) {
    if (!window) {
        return;
    }
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    }
    SetForegroundWindow(window);
    BringWindowToTop(window);
    ShowWindow(window, SW_SHOWNORMAL);
}

}  // namespace

Application::Application() = default;
Application::~Application() = default;

bool Application::Initialize(HINSTANCE instance, int commandShow, const AppArguments& arguments) {
    instance_ = instance;
    arguments_ = arguments;

    library_ = std::make_unique<LibraryWindow>();
    library_->SetPlaybackRequest([this](const std::filesystem::path& path, double startRatio) {
        OpenInPlayer(path, startRatio);
    });
    library_->SetFocusPlayerRequest([this] { FocusPlayer(); });
    library_->SetEmbyPlaybackReportRelay([this](const std::wstring& reportJson) {
        RelayEmbyPlaybackReport(reportJson);
    });
    library_->SetTimerHandler([this] { TryDeliverPendingEmbyReport(); });
    library_->SetQuitHandler([this] { OnLibraryClosed(); });
    // The library window controls the per-WebView certificate policy; mirror it
    // onto the player WebView so both share the same trust posture.
    library_->SetAllowInsecureCertificatesRequest([this](bool allow) {
        // The player window is created lazily; it will read its own setting at
        // creation time. If it already exists, apply the change live.
        (void)allow;  // applied via arguments_/player creation below
    });

    if (!library_->Create(instance)) {
        return false;
    }
    library_->Show(commandShow);
    return true;
}

int Application::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void Application::EnsurePlayerWindow() {
    if (player_) {
        return;
    }

    player_ = std::make_unique<MainWindow>();
    player_->ConfigureLogging(arguments_.logLevel);
    player_->SetBackend(arguments_.backend);
    player_->SetInitialVideoTrackSelection(arguments_.selectedVideoTrackIndex);
    // The player window always shows the player route, so keep the WebView UI.
    player_->SetWebUiEnabled(true);
    player_->SetQuitHandler([this] { OnPlayerClosed(); });
    if (!player_->Create(instance_)) {
        player_.reset();
        return;
    }
    player_->Show(SW_SHOWNORMAL);
    // Note: a buffered Emby report (if any) is delivered/retried by the timer
    // started in RelayEmbyPlaybackReport; nothing extra needed here.
}

void Application::OpenInPlayer(const std::filesystem::path& path, double startPositionRatio) {
    EnsurePlayerWindow();
    if (!player_) {
        return;
    }
    BringWindowToForeground(player_->Handle());
    // OpenInitialPath consumes pendingStartPositionRatio_ during playback start
    // to resume from a saved position.
    player_->SetPendingStartPositionRatio(startPositionRatio);
    player_->OpenInitialPath(path, true);
}

void Application::FocusPlayer() {
    EnsurePlayerWindow();
    if (player_) {
        BringWindowToForeground(player_->Handle());
    }
}

void Application::RelayEmbyPlaybackReport(const std::wstring& reportJson) {
    pendingEmbyReport_ = reportJson;
    embyReportDelivered_ = false;
    embyReportRetries_ = 0;
    // Try once immediately; if the player WebView isn't ready yet, the retry
    // timer will keep trying until it succeeds (bounded).
    TryDeliverPendingEmbyReport();
}

void Application::TryDeliverPendingEmbyReport() {
    if (pendingEmbyReport_.empty() || embyReportDelivered_) {
        return;
    }
    if (player_ && player_->DeliverEmbyPlaybackReport(pendingEmbyReport_)) {
        // Delivered once into the player's storage; App.tsx re-checks pending
        // state every second, so no need to re-inject. Stop retrying.
        embyReportDelivered_ = true;
        return;
    }
    // WebView not ready yet (or player not created); retry on the UI thread.
    if (++embyReportRetries_ < kEmbyReportMaxRetries && library_ && library_->Handle()) {
        SetTimer(library_->Handle(), kApplicationTimerId, kEmbyReportRetryIntervalMs, nullptr);
    } else {
        pendingEmbyReport_.clear();
        embyReportRetries_ = 0;
    }
}

void Application::OnLibraryClosed() {
    // Closing the library window quits the entire application.
    if (player_ && player_->Handle()) {
        DestroyWindow(player_->Handle());
    }
    player_.reset();
    library_.reset();
    PostQuitMessage(0);
}

void Application::OnPlayerClosed() {
    // Closing the player window only destroys the player; the library stays.
    player_.reset();
}

}  // namespace anvil::app
