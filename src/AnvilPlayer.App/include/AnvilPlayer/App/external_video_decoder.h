#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace anvil::app {

// A single decoded raw-video frame from the legacy ffmpeg raw-frame bridge.
// Fixed 960x540 BGRA.
struct VideoFrame {
    static constexpr int Width = 960;
    static constexpr int Height = 540;
    static constexpr int BytesPerPixel = 4;
    std::shared_ptr<const std::vector<unsigned char>> pixels;
    std::uint64_t serial = 0;

    bool HasPixels() const {
        return pixels && !pixels->empty();
    }
};

// Legacy raw-frame bridge backend. Spawns `ffmpeg -f rawvideo pipe:1` at
// 960x540 BGRA, reads the pipe on a worker thread, exposes the latest frame,
// and posts kVideoFrameReadyMessage to a notification window when a new frame
// arrives. Used only by the RawFrameBridge backend (--internal-playback).
class ExternalVideoDecoder {
public:
    ExternalVideoDecoder() = default;
    ~ExternalVideoDecoder();
    ExternalVideoDecoder(const ExternalVideoDecoder&) = delete;
    ExternalVideoDecoder& operator=(const ExternalVideoDecoder&) = delete;

    bool Start(const std::filesystem::path& mediaPath,
               std::chrono::milliseconds startPosition,
               HWND notificationWindow,
               UINT notificationMessage,
               uint64_t notificationCookie = 0);

    // Non-blocking first phase of shutdown. Safe to call repeatedly; prevents
    // further window notifications and cancels a pending pipe read.
    void RequestStop();

    // Completes shutdown and releases the worker/process resources. Call this
    // from a non-window thread after RequestStop when blocking is unacceptable.
    void Stop();

    bool IsRunning() const {
        return running_;
    }

    bool LatestFrame(VideoFrame& frame) const;

    // Executes a short read-only operation while the decoder retains frame
    // ownership.  Window paint code uses this instead of copying a shared_ptr
    // whose final large pixel-buffer release could otherwise occur in
    // WM_PAINT after the reader publishes the next frame.
    template <typename Visitor>
    bool VisitLatestFrame(Visitor&& visitor) const {
        std::unique_lock lock(frameMutex_, std::try_to_lock);
        if (!lock.owns_lock() || !frame_.HasPixels()) {
            return false;
        }
        visitor(static_cast<const VideoFrame&>(frame_));
        return true;
    }

    void ClearFrame();

    void AcknowledgeFrameNotification();

    std::wstring LastCommandLine() const;

private:
    struct StartRequest {
        std::uint64_t generation = 0;
        std::filesystem::path mediaPath;
        std::chrono::milliseconds startPosition{0};
        HWND notificationWindow = nullptr;
        UINT notificationMessage = 0;
        std::uint64_t notificationCookie = 0;
    };

    bool EnsureControlWorkerLocked();
    std::uint64_t RequestStopLocked();
    void ControlLoop();
    void ReaderLoop(StartRequest request, HANDLE stdoutRead, std::atomic_bool& readerDone);
    void NotifyFrameReady(const StartRequest& request);

    mutable std::mutex controlMutex_;
    std::condition_variable controlCv_;
    std::optional<StartRequest> pendingStart_;
    std::uint64_t desiredGeneration_ = 0;
    std::uint64_t settledGeneration_ = 0;
    bool desiredRunning_ = false;
    bool exitRequested_ = false;
    bool workerStarted_ = false;
    std::thread controlThread_;

    mutable std::mutex frameMutex_;
    VideoFrame frame_;
    std::atomic_bool running_ = false;
    std::atomic_bool frameMessagePending_ = false;

    mutable std::mutex commandLineMutex_;
    std::wstring lastCommandLine_;
};

}  // namespace anvil::app
