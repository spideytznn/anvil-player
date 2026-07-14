#include "AnvilPlayer/App/application.h"

#include "AnvilPlayer/App/ui_draw.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <process.h>
#include <utility>

namespace anvil::app {

enum class PlayerReaperStatus : std::uint8_t {
    Idle,
    Running,
    Completed,
    Failed,
};

struct PlayerReaperState {
    std::atomic<PlayerReaperStatus> status{PlayerReaperStatus::Idle};
};

namespace {

// Timer id used by Application to retry buffered Emby report delivery on the
// UI thread while the player WebView finishes initializing. Reuses a value
// that does not collide with MainWindow/overlay timer ids (1001..1005).
constexpr UINT_PTR kApplicationTimerId = 9001;
constexpr UINT kEmbyReportRetryIntervalMs = 300;
constexpr int kEmbyReportMaxRetries = 15;  // ~4.5s total, stops once delivered
constexpr UINT kPlayerReaperPollIntervalMs = 50;

struct PlayerReaperContext {
    MainWindow* player = nullptr;
    std::shared_ptr<PlayerReaperState> state;
};

unsigned __stdcall RunPlayerReaper(void* opaque) noexcept {
    std::unique_ptr<PlayerReaperContext> context(static_cast<PlayerReaperContext*>(opaque));
    MainWindow* player = context->player;
    auto state = std::move(context->state);
    context.reset();

    // ReleaseUiThreadResourcesForBackgroundDestruction has already removed
    // every COM/GDI+-affine object. Only backend-owned waits and plain C++
    // state are allowed to remain here.
    delete player;
    state->status.store(PlayerReaperStatus::Completed, std::memory_order_release);
    return 0;
}

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

Application::Application() {
    try {
        playerReaperState_ = std::make_shared<PlayerReaperState>();
    } catch (...) {
        // Without process-lifetime state there is no safe way to observe a
        // detached destructor. Refuse to create players instead of allowing
        // an unbounded chain of leaked/overlapping instances.
        playerReaperUnavailable_ = true;
        OutputDebugStringW(L"Anvil Player: unable to allocate player reaper state; player creation disabled\n");
    }
}

Application::~Application() {
    // An abnormal message-loop exit must not turn Application destruction into
    // a UI-thread backend join. Release apartment/GDI resources while their
    // runtimes are alive, then let process teardown reclaim the backend state.
    if (player_) {
        player_->ReleaseUiThreadResourcesForBackgroundDestruction();
        (void)player_.release();
    }
}

bool Application::Initialize(HINSTANCE instance, int commandShow, const AppArguments& arguments) {
    instance_ = instance;
    arguments_ = arguments;

    logSink_ = std::make_shared<anvil::playback::InMemoryLogSink>(arguments_.logLevel);
    logSink_->SetFilePath(std::filesystem::temp_directory_path() / L"anvil-player" / L"anvil-player.log");
#if defined(_DEBUG)
    logSink_->EnableDebuggerOutput(true);
#endif
    logSink_->Write(anvil::playback::LogLevel::Info, L"library", L"library logging initialized");

    library_ = std::make_unique<LibraryWindow>();
    library_->SetDebugLogHandler([this](const std::wstring& message) {
        if (logSink_) logSink_->Write(anvil::playback::LogLevel::Debug, L"library", message);
    });
    library_->SetPlaybackRequest([this](const std::filesystem::path& path,
                                       double startRatio,
                                       int audioTrackIndex,
                                       int subtitleTrackIndex) {
        OpenInPlayer(path, startRatio, audioTrackIndex, subtitleTrackIndex);
    });
    library_->SetFocusPlayerRequest([this] { FocusPlayer(); });
    library_->SetUiLanguageChangedRequest([this] {
        if (player_) player_->RefreshUiLanguage();
    });
    library_->SetRefreshRatePreferencesChangedRequest([this] {
        if (player_) player_->ApplyGlobalRefreshRatePreferences();
    });
    library_->SetVideoPassthroughPreferencesChangedRequest([this] {
        if (player_) player_->ApplyGlobalVideoPassthroughPreferences();
    });
    library_->SetAudioPassthroughPreferencesChangedRequest([this] {
        if (player_) player_->ApplyGlobalAudioPassthroughPreferences();
    });
    library_->SetEmbyPlaybackReportRelay([this](const std::wstring& reportJson) {
        RelayEmbyPlaybackReport(reportJson);
    });
    library_->SetTimerHandler([this] { OnApplicationTimer(); });
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
        // Window callbacks run inside DispatchMessage. Deferring ownership
        // changes prevents releasing a window while its HandleMessage frame is
        // still on the stack.
        ProcessDeferredWindowLifetime();
    }
    return static_cast<int>(message.wParam);
}

void Application::EnsurePlayerWindow() {
    if (player_ || PlayerReaperPreventsCreation()) {
        return;
    }

    player_ = std::make_unique<MainWindow>(logSink_);
    player_->ConfigureLogging(arguments_.logLevel);
    player_->SetBackend(arguments_.backend);
    player_->SetInitialVideoTrackSelection(arguments_.selectedVideoTrackIndex);
    // The player window always shows the player route, so keep the WebView UI.
    player_->SetWebUiEnabled(true);
    player_->SetQuitHandler([this] { OnPlayerClosed(); });
    player_->SetLocalPlaybackProgressRelay([this](const std::wstring& progressJson) {
        if (library_) library_->DeliverLocalPlaybackProgress(progressJson);
    });
    if (!player_->Create(instance_)) {
        player_.reset();
        return;
    }
    player_->ApplyGlobalRefreshRatePreferences();
    player_->ApplyGlobalAudioPassthroughPreferences();
    player_->Show(SW_SHOWNORMAL);
    // Note: a buffered Emby report (if any) is delivered/retried by the timer
    // started in RelayEmbyPlaybackReport; nothing extra needed here.
}

void Application::OpenInPlayer(const std::filesystem::path& path,
                               const double startPositionRatio,
                               const int audioTrackIndex,
                               const int subtitleTrackIndex) {
    const double validatedStartPositionRatio = std::isfinite(startPositionRatio)
        ? std::clamp(startPositionRatio, 0.0, 0.99)
        : 0.0;
    if (logSink_) {
        logSink_->Write(
            anvil::playback::LogLevel::Info, L"library",
            L"playback request path=" + path.wstring() +
                L" start_ratio=" + std::to_wstring(validatedStartPositionRatio));
    }
    if ((player_ && player_->IsClosing()) || PlayerReaperPreventsCreation()) {
        pendingPlayerOpenPath_ = path;
        pendingPlayerOpenRatio_ = validatedStartPositionRatio;
        pendingPlayerAudioTrackIndex_ = audioTrackIndex;
        pendingPlayerSubtitleTrackIndex_ = subtitleTrackIndex;
        ArmPlayerReaperPollIfNeeded();
        return;
    }
    EnsurePlayerWindow();
    if (!player_) {
        return;
    }
    player_->MoveToMonitorOf(library_ ? library_->Handle() : nullptr);
    BringWindowToForeground(player_->Handle());
    // OpenInitialPath consumes pendingStartPositionRatio_ before playback starts
    // so every runtime receives the same saved position in its first snapshot.
    player_->SetPendingStartPositionRatio(validatedStartPositionRatio);
    player_->SetPendingMediaTrackSelections(audioTrackIndex, subtitleTrackIndex);
    player_->OpenInitialPath(path, true);
}

void Application::FocusPlayer() {
    if ((player_ && player_->IsClosing()) || PlayerReaperPreventsCreation()) {
        pendingPlayerFocus_ = true;
        ArmPlayerReaperPollIfNeeded();
        return;
    }
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

void Application::OnApplicationTimer() {
    TryDeliverPendingEmbyReport();
    ProcessDeferredWindowLifetime();
}

void Application::OnLibraryClosed() {
    // Closing the library quits the process after the player has completed its
    // asynchronous shutdown.
    shutdownRequested_ = true;
    libraryResetPending_ = true;
    if (player_ && player_->Handle()) {
        SendMessageW(player_->Handle(), WM_CLOSE, 0, 0);
    }
}

void Application::OnPlayerClosed() {
    // Closing the player window only destroys the player; the library stays.
    playerResetPending_ = true;
}

bool Application::PlayerReaperPreventsCreation() const noexcept {
    if (playerReaperUnavailable_ || !playerReaperState_) {
        return true;
    }
    return playerReaperState_->status.load(std::memory_order_acquire) != PlayerReaperStatus::Idle;
}

void Application::ArmPlayerReaperPollIfNeeded() const noexcept {
    if (!playerReaperState_ || !library_ || !library_->Handle()) {
        return;
    }
    const auto status = playerReaperState_->status.load(std::memory_order_acquire);
    if (status == PlayerReaperStatus::Running || status == PlayerReaperStatus::Completed) {
        SetTimer(library_->Handle(), kApplicationTimerId, kPlayerReaperPollIntervalMs, nullptr);
    }
}

void Application::StartPlayerReaper(MainWindow* retiredPlayer) noexcept {
    if (!retiredPlayer) {
        return;
    }
    if (playerReaperUnavailable_ || !playerReaperState_) {
        // Intentionally leak this one retired player and permanently block new
        // ones. Deleting here could join a stuck backend on the UI thread.
        playerReaperUnavailable_ = true;
        return;
    }

    PlayerReaperStatus expected = PlayerReaperStatus::Idle;
    if (!playerReaperState_->status.compare_exchange_strong(
            expected,
            PlayerReaperStatus::Running,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // This should be unreachable because creation is gated on Idle. Keep
        // both instances isolated and permanently stop further accumulation.
        playerReaperUnavailable_ = true;
        OutputDebugStringW(L"Anvil Player: overlapping player reaper prevented; player creation disabled\n");
        return;
    }

    auto* context = new (std::nothrow) PlayerReaperContext{retiredPlayer, playerReaperState_};
    if (!context) {
        playerReaperState_->status.store(PlayerReaperStatus::Failed, std::memory_order_release);
        playerReaperUnavailable_ = true;
        OutputDebugStringW(L"Anvil Player: unable to allocate player reaper context; player creation disabled\n");
        return;
    }

    const uintptr_t threadHandle = _beginthreadex(nullptr, 0, &RunPlayerReaper, context, 0, nullptr);
    if (threadHandle == 0) {
        delete context;
        playerReaperState_->status.store(PlayerReaperStatus::Failed, std::memory_order_release);
        playerReaperUnavailable_ = true;
        OutputDebugStringW(L"Anvil Player: unable to start player reaper; player creation disabled\n");
        return;
    }

    CloseHandle(reinterpret_cast<HANDLE>(threadHandle));
    ArmPlayerReaperPollIfNeeded();
}

void Application::ReplayPendingPlayerRequest() {
    if (shutdownRequested_ || player_ || PlayerReaperPreventsCreation()) {
        return;
    }
    if (pendingPlayerOpenPath_) {
        const auto path = std::move(*pendingPlayerOpenPath_);
        const double ratio = pendingPlayerOpenRatio_;
        const int audioTrackIndex = pendingPlayerAudioTrackIndex_;
        const int subtitleTrackIndex = pendingPlayerSubtitleTrackIndex_;
        pendingPlayerOpenPath_.reset();
        pendingPlayerOpenRatio_ = 0.0;
        pendingPlayerAudioTrackIndex_ = -2;
        pendingPlayerSubtitleTrackIndex_ = -2;
        pendingPlayerFocus_ = false;
        OpenInPlayer(path, ratio, audioTrackIndex, subtitleTrackIndex);
    } else if (pendingPlayerFocus_) {
        pendingPlayerFocus_ = false;
        FocusPlayer();
    }
}

void Application::ProcessDeferredWindowLifetime() {
    if (playerResetPending_) {
        playerResetPending_ = false;
        // MainWindow may own workers that are retiring a blocked driver or I/O
        // call. Destruction therefore belongs to a reaper, never the message
        // thread. WebView/ HWND teardown has already run on this thread.
        if (player_) {
            player_->ReleaseUiThreadResourcesForBackgroundDestruction();
        }
        MainWindow* retiredPlayer = player_.release();
        if (retiredPlayer && !shutdownRequested_) {
            StartPlayerReaper(retiredPlayer);
        } else if (retiredPlayer) {
            // Process shutdown must not race a detached MainWindow destructor
            // against COM/GDI+ teardown in wWinMain. The OS reclaims the
            // remaining process resources after the message loop exits.
            (void)retiredPlayer;
        }
    }

    if (playerReaperState_) {
        const auto status = playerReaperState_->status.load(std::memory_order_acquire);
        if (status == PlayerReaperStatus::Completed) {
            playerReaperState_->status.store(PlayerReaperStatus::Idle, std::memory_order_release);
        } else if (status == PlayerReaperStatus::Failed) {
            playerReaperUnavailable_ = true;
        }
    }
    ArmPlayerReaperPollIfNeeded();

    ReplayPendingPlayerRequest();
    if (libraryResetPending_) {
        libraryResetPending_ = false;
        library_.reset();
    }
    if (shutdownRequested_ && !player_ && !quitPosted_) {
        quitPosted_ = true;
        PostQuitMessage(0);
    }
}

}  // namespace anvil::app
