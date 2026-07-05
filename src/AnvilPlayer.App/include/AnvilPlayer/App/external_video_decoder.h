#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
               UINT notificationMessage);

    void Stop();

    bool IsRunning() const {
        return running_;
    }

    bool LatestFrame(VideoFrame& frame) const;

    void ClearFrame();

    void AcknowledgeFrameNotification();

    std::wstring LastCommandLine() const {
        return lastCommandLine_;
    }

private:
    void NotifyFrameReady();
    void ReaderLoop();

    mutable std::mutex mutex_;
    VideoFrame frame_;
    std::atomic_bool stopping_ = false;
    std::atomic_bool running_ = false;
    std::atomic<HWND> notificationWindow_ = nullptr;
    std::atomic_uint notificationMessage_ = 0;
    std::atomic_bool frameMessagePending_ = false;
    PROCESS_INFORMATION process_{};
    HANDLE stdoutRead_ = nullptr;
    std::thread readerThread_;
    std::wstring lastCommandLine_;
};

}  // namespace anvil::app
