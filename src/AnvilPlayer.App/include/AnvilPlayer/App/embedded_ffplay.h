#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace anvil::app {

// Scratch state for the EnumWindows callback that locates the ffplay child window.
struct WindowSearch {
    DWORD processId = 0;
    const std::wstring* title = nullptr;
    HWND window = nullptr;
};

// EnumWindows callback matching a visible top-level window by PID and optional title.
BOOL CALLBACK FindProcessWindowProc(HWND hwnd, LPARAM lParam);

// External ffplay backend. Spawns `ffplay`, then EnumWindows+SetParent to
// reparent the ffplay HWND into the main window as a child. SetBounds resizes
// the child. Used by the EmbeddedFfplay backend (--external-playback).
class EmbeddedFfplayPlayer {
public:
    EmbeddedFfplayPlayer() = default;
    ~EmbeddedFfplayPlayer();
    EmbeddedFfplayPlayer(const EmbeddedFfplayPlayer&) = delete;
    EmbeddedFfplayPlayer& operator=(const EmbeddedFfplayPlayer&) = delete;

    bool Start(HWND parent,
               RECT bounds,
               const std::filesystem::path& mediaPath,
               std::chrono::milliseconds startPosition,
               double volume,
               bool useLibplaceboDolbyVision);

    void RequestStop();
    void Stop();

    bool IsRunning() const;

    bool HasAttachedWindow() const;

    void SetBounds(RECT bounds);

    std::wstring LastCommandLine() const;

private:
    struct StartRequest {
        std::uint64_t generation = 0;
        HWND parent = nullptr;
        RECT bounds{};
        std::filesystem::path mediaPath;
        std::chrono::milliseconds startPosition{0};
        double volume = 1.0;
        bool useLibplaceboDolbyVision = false;
    };

    bool EnsureControlWorkerLocked();
    std::uint64_t RequestStopLocked();
    void ControlLoop();
    static void AttachWindow(HWND parent, HWND child, RECT bounds);
    static void ApplyBounds(HWND child, RECT bounds);

    mutable std::mutex controlMutex_;
    std::condition_variable controlCv_;
    std::optional<StartRequest> pendingStart_;
    RECT pendingBounds_{};
    std::uint64_t boundsGeneration_ = 0;
    std::uint64_t desiredGeneration_ = 0;
    std::uint64_t settledGeneration_ = 0;
    bool desiredRunning_ = false;
    bool exitRequested_ = false;
    bool workerStarted_ = false;
    std::thread controlThread_;

    mutable std::mutex commandLineMutex_;
    std::wstring lastCommandLine_;
    std::atomic_bool running_ = false;
    std::atomic_bool attached_ = false;
};

}  // namespace anvil::app
