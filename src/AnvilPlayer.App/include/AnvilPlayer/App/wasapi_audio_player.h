#pragma once

#include "AnvilPlayer/App/log_sink_ptr.h"
#include "AnvilPlayer/Playback/Settings.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace anvil::app {

// WASAPI shared-mode PCM audio player with in-process FFmpeg audio decode and
// swresample. Owns its own playback clock; PlaybackClock() is the master clock
// consumed by the native video scheduler for A/V sync.
class WasapiAudioPlayer {
public:
    WasapiAudioPlayer() = default;
    ~WasapiAudioPlayer();
    WasapiAudioPlayer(const WasapiAudioPlayer&) = delete;
    WasapiAudioPlayer& operator=(const WasapiAudioPlayer&) = delete;

    void SetLogSink(LogSinkPtr logSink);

    bool Start(const std::filesystem::path& mediaPath,
               std::chrono::milliseconds startPosition,
               double volume,
               int selectedAudioTrackIndex = anvil::playback::kAudioTrackAuto);

    bool StartPacketStream(const std::filesystem::path& mediaPath,
                           const AVCodecParameters* codecParameters,
                           AVRational timeBase,
                           std::chrono::milliseconds startPosition,
                           double volume,
                           int streamIndex);

    bool QueuePacket(const AVPacket* packet);
    void ResetPacketStream(std::chrono::milliseconds position);
    void MarkPacketStreamEof();

    void Stop();
    void Pause(std::chrono::milliseconds position);
    bool Resume(std::chrono::milliseconds position);
    void HoldPacketStream(std::chrono::milliseconds position);
    bool ResumePacketStream();
    bool Seek(std::chrono::milliseconds position);
    void SetVolume(double volume);
    void SetPlaybackRate(double rate);

    bool IsRunning() const {
        return running_.load();
    }

    std::wstring LastStatus() const;

    // Returns the audio playback position, advancing at the current playback rate.
    std::optional<std::chrono::milliseconds> PlaybackClock() const;

private:
    struct CoInitScope {
        CoInitScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~CoInitScope() {
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
        }
        bool Ok() const {
            return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        }
        HRESULT hr = E_FAIL;
    };

    struct CoTaskMemDeleter {
        void operator()(WAVEFORMATEX* value) const {
            if (value) {
                CoTaskMemFree(value);
            }
        }
    };

    struct WasapiFormat {
        UINT32 sampleRate = 0;
        UINT32 channels = 0;
        UINT32 blockAlign = 0;
        AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE;
        std::wstring description;
    };

    struct ResamplerState {
        int sampleRate = 0;
        int sampleFormat = AV_SAMPLE_FMT_NONE;
        int channels = 0;
        double playbackRate = 1.0;
    };

    void PlaybackLoop();
    std::optional<std::chrono::milliseconds> TakePendingSeek();
    bool HasPendingSeek() const;
    bool HandlePause(IAudioClient* audioClient,
                     const WasapiFormat& outputFormat,
                     uint64_t& submittedFrames,
                     bool& audioClientStarted);
    bool ApplyPendingSeek(AVFormatContext* formatCtx,
                          AVCodecContext* codecCtx,
                          SwrContext*& swrCtx,
                          ResamplerState& resamplerState,
                          IAudioClient* audioClient,
                          const WasapiFormat& outputFormat,
                          uint64_t& submittedFrames,
                          bool& audioClientStarted);
    bool ApplyPendingPacketSeek(AVCodecContext* codecCtx,
                                SwrContext*& swrCtx,
                                ResamplerState& resamplerState,
                                IAudioClient* audioClient,
                                const WasapiFormat& outputFormat,
                                uint64_t& submittedFrames,
                                bool& audioClientStarted);

    bool InitializeWasapi(Microsoft::WRL::ComPtr<IAudioClient>& audioClient,
                          Microsoft::WRL::ComPtr<IAudioRenderClient>& renderClient,
                          WasapiFormat& outputFormat,
                          UINT32& bufferFrameCount) const;

    bool DescribeMixFormat(const WAVEFORMATEX* format, WasapiFormat& output) const;

    bool ReceiveFrames(AVCodecContext* codecCtx,
                       SwrContext*& swrCtx,
                       AVFrame* frame,
                       AVRational timeBase,
                       ResamplerState& resamplerState,
                       const WasapiFormat& outputFormat,
                       IAudioRenderClient* renderClient,
                       IAudioClient* audioClient,
                       uint64_t& submittedFrames,
                       bool& audioClientStarted);

    void DrainDecoder(AVCodecContext* codecCtx,
                      SwrContext*& swrCtx,
                      AVFrame* frame,
                      AVRational timeBase,
                      ResamplerState& resamplerState,
                      const WasapiFormat& outputFormat,
                      IAudioRenderClient* renderClient,
                      IAudioClient* audioClient,
                      uint64_t& submittedFrames,
                      bool& audioClientStarted);

    bool RenderFrame(AVFrame* frame,
                     AVRational timeBase,
                     SwrContext*& swrCtx,
                     ResamplerState& resamplerState,
                     const WasapiFormat& outputFormat,
                     IAudioRenderClient* renderClient,
                     IAudioClient* audioClient,
                     uint64_t& submittedFrames,
                     bool& audioClientStarted);

    bool WritePcm(IAudioRenderClient* renderClient,
                  IAudioClient* audioClient,
                  const WasapiFormat& outputFormat,
                  const uint8_t* data,
                  UINT32 frames,
                  uint64_t& submittedFrames,
                  bool& audioClientStarted);

    void ApplyVolume(std::vector<uint8_t>& pcm, AVSampleFormat format) const;
    bool ShouldDropSeekPreroll(const AVFrame* frame, AVRational timeBase, const WasapiFormat& outputFormat) const;
    static std::chrono::milliseconds FramePts(const AVFrame* frame, AVRational timeBase);
    static std::chrono::milliseconds FrameDuration(const AVFrame* frame, const WasapiFormat& outputFormat);

    void DrainWasapi(IAudioClient* audioClient) const;
    AVPacket* TakeQueuedPacket();
    void ClearPacketQueueLocked();

    void ResetPlaybackClock();
    void ResetPlaybackClock(std::chrono::milliseconds position);
    void SetPlaybackClockRunning(bool running);
    void UpdatePlaybackClock(const WasapiFormat& outputFormat,
                             uint64_t submittedFrames,
                             UINT32 paddingFrames);

    static int InterruptCallback(void* opaque);

    void SignalStart(bool success);

    void LogError(const std::wstring& message) const;
    void LogInfo(const std::wstring& message) const;

    std::filesystem::path path_;
    std::chrono::milliseconds startPosition_{0};
    int selectedAudioTrackIndex_ = anvil::playback::kAudioTrackAuto;
    int packetStreamIndex_ = -1;
    AVRational packetTimeBase_{1, 1};
    AVCodecParameters* packetCodecParameters_ = nullptr;
    LogSinkPtr logSink_;
    std::atomic<double> volume_{1.0};
    std::atomic<double> playbackRate_{1.0};
    std::atomic_bool stopping_{false};
    std::atomic_bool running_{false};
    std::atomic_bool paused_{false};
    std::atomic_bool packetInputMode_{false};
    std::atomic_bool packetStreamEof_{false};
    std::atomic<int64_t> pausePositionMs_{0};
    std::atomic<int64_t> pendingSeekMs_{-1};
    std::atomic<int64_t> ioInterruptAfterSteadyMs_{0};
    std::mutex packetMutex_;
    std::condition_variable packetCv_;
    std::deque<AVPacket*> packetQueue_;
    std::size_t packetQueueBytes_ = 0;
    mutable std::mutex stateMutex_;
    std::condition_variable startCv_;
    bool startResolved_ = false;
    bool startSucceeded_ = false;
    std::wstring lastStatus_ = L"wasapi shared pcm";
    mutable std::mutex clockMutex_;
    bool clockValid_ = false;
    bool clockRunning_ = false;
    double clockPlaybackRate_ = 1.0;
    std::chrono::milliseconds clockPosition_{0};
    std::chrono::milliseconds clockAnchorPosition_{0};
    uint64_t clockAnchorPlayedFrames_ = 0;
    uint64_t clockSubmittedFrames_ = 0;
    UINT32 clockPaddingFrames_ = 0;
    UINT32 clockSampleRate_ = 0;
    std::chrono::steady_clock::time_point clockUpdatedAt_{};
    std::thread playbackThread_;
};

}  // namespace anvil::app
