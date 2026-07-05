#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
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
               double volume);

    void Stop();

    bool IsRunning() const;

    bool HasAttachedWindow() const;

    void SetBounds(RECT bounds);

    std::wstring LastCommandLine() const {
        return lastCommandLine_;
    }

private:
    void AttachWindowLoop(HWND parent, RECT bounds, DWORD processId, std::wstring title);
    void AttachWindow(HWND parent, HWND child, RECT bounds);

    PROCESS_INFORMATION process_{};
    HWND parent_ = nullptr;
    mutable std::mutex windowMutex_;
    HWND childWindow_ = nullptr;
    RECT pendingBounds_{};
    std::wstring windowTitle_;
    std::wstring lastCommandLine_;
    std::thread attachThread_;
    std::atomic_bool stopping_ = false;
    std::atomic_bool running_ = false;
    std::uint64_t launchSerial_ = 0;
};

}  // namespace anvil::app
