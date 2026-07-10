#pragma once

#include "AnvilPlayer/Playback/PlayerController.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace anvil::app {

// Serializes blocking open/probe operations on one worker. New requests cancel
// and supersede the active generation; only the latest result is posted. The
// controller lock is never held while the worker performs I/O.
class PlaybackSupervisor {
public:
    struct OpenCompletion {
        uint64_t windowCookie = 0;
        uint64_t generation = 0;
        bool autoplay = true;
        anvil::playback::PreparedMediaOpen prepared;
    };

    explicit PlaybackSupervisor(anvil::playback::PlayerController& controller);
    ~PlaybackSupervisor();

    PlaybackSupervisor(const PlaybackSupervisor&) = delete;
    PlaybackSupervisor& operator=(const PlaybackSupervisor&) = delete;

    bool Start(HWND notificationWindow,
               UINT openCompleteMessage,
               UINT stoppedMessage,
               uint64_t windowCookie);
    uint64_t Open(std::filesystem::path path,
                  bool autoplay,
                  std::chrono::milliseconds timeout = std::chrono::seconds{15});
    void CancelOpen();
    void RequestShutdown();
    std::optional<OpenCompletion> TakeCompletion();
    uint64_t TakeFailedGeneration();

    uint64_t CurrentGeneration() const {
        return generation_.load();
    }
    bool IsStopped() const {
        return stopped_.load();
    }

private:
    struct OpenRequest {
        uint64_t generation = 0;
        std::filesystem::path path;
        bool autoplay = true;
        std::chrono::milliseconds timeout{15000};
    };

    void WorkerLoop();

    anvil::playback::PlayerController& controller_;
    HWND notificationWindow_ = nullptr;
    UINT openCompleteMessage_ = 0;
    UINT stoppedMessage_ = 0;
    uint64_t windowCookie_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<OpenRequest> pending_;
    std::optional<OpenCompletion> completed_;
    std::stop_source activeStopSource_;
    bool active_ = false;
    bool shutdownRequested_ = false;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> failedGeneration_{0};
    std::atomic_bool stopped_{true};
    std::thread worker_;
};

}  // namespace anvil::app
