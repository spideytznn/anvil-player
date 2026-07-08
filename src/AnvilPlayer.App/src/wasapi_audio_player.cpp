#include "AnvilPlayer/App/wasapi_audio_player.h"

#include "AnvilPlayer/App/string_util.h"

#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace anvil::app {

using anvil::playback::FormatTimecode;
using anvil::playback::LogLevel;

namespace {

constexpr std::size_t kPacketStreamMaxQueueBytes = 24ull * 1024ull * 1024ull;
constexpr auto kRuntimeSeekIoTimeout = std::chrono::milliseconds{4500};

int64_t SteadyClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::chrono::milliseconds ScaleDuration(const std::chrono::milliseconds value, const double rate) {
    return std::chrono::milliseconds{
        static_cast<long long>(std::llround(static_cast<double>(value.count()) * rate))};
}

uint64_t PlayedFramesFromCounters(const uint64_t submittedFrames, const UINT32 paddingFrames) {
    const uint64_t queuedFrames = std::min<uint64_t>(paddingFrames, submittedFrames);
    return submittedFrames - queuedFrames;
}

std::chrono::milliseconds FramesToMediaDuration(const uint64_t frames, const UINT32 sampleRate, const double rate) {
    if (sampleRate == 0 || frames == 0) {
        return std::chrono::milliseconds{0};
    }
    const long double milliseconds =
        (static_cast<long double>(frames) * 1000.0L * static_cast<long double>(rate)) /
        static_cast<long double>(sampleRate);
    return std::chrono::milliseconds{static_cast<long long>(std::llround(milliseconds))};
}

bool IsNetworkMediaPath(const std::filesystem::path& path) {
    auto value = path.wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value.rfind(L"http://", 0) == 0 ||
           value.rfind(L"https://", 0) == 0;
}

}  // namespace

WasapiAudioPlayer::~WasapiAudioPlayer() {
    Stop();
}

void WasapiAudioPlayer::SetLogSink(LogSinkPtr logSink) {
    logSink_ = std::move(logSink);
}

bool WasapiAudioPlayer::Start(const std::filesystem::path& mediaPath,
                              const std::chrono::milliseconds startPosition,
                              const double volume,
                              const int selectedAudioTrackIndex) {
    Stop();
    if (mediaPath.empty() || selectedAudioTrackIndex == anvil::playback::kAudioTrackOff) {
        return false;
    }

    path_ = mediaPath;
    startPosition_ = startPosition;
    selectedAudioTrackIndex_ = selectedAudioTrackIndex;
    volume_.store(std::clamp(volume, 0.0, 1.0));
    paused_.store(false);
    pausePositionMs_.store(startPosition.count());
    pendingSeekMs_.store(-1);
    ioInterruptAfterSteadyMs_.store(0);
    ResetPlaybackClock();
    {
        std::scoped_lock lock(stateMutex_);
        startResolved_ = false;
        startSucceeded_ = false;
        lastStatus_ = L"wasapi shared pcm initializing";
    }

    stopping_.store(false);
    running_.store(true);
    playbackThread_ = std::thread([this]() { PlaybackLoop(); });

    std::unique_lock lock(stateMutex_);
    const bool resolved = startCv_.wait_for(lock, std::chrono::seconds(3), [this]() {
        return startResolved_;
    });
    const bool started = resolved && startSucceeded_;
    lock.unlock();

    if (!started) {
        Stop();
    }
    return started;
}

bool WasapiAudioPlayer::StartPacketStream(const std::filesystem::path& mediaPath,
                                          const AVCodecParameters* codecParameters,
                                          const AVRational timeBase,
                                          const std::chrono::milliseconds startPosition,
                                          const double volume,
                                          const int streamIndex) {
    Stop();
    if (mediaPath.empty() || !codecParameters || codecParameters->codec_type != AVMEDIA_TYPE_AUDIO) {
        return false;
    }

    packetCodecParameters_ = avcodec_parameters_alloc();
    if (!packetCodecParameters_) {
        return false;
    }
    const int copyError = avcodec_parameters_copy(packetCodecParameters_, codecParameters);
    if (copyError < 0) {
        avcodec_parameters_free(&packetCodecParameters_);
        LogError(L"packet stream codec copy failed: " + FfmpegErrorString(copyError));
        return false;
    }

    path_ = mediaPath;
    startPosition_ = startPosition;
    selectedAudioTrackIndex_ = streamIndex;
    packetStreamIndex_ = streamIndex;
    packetTimeBase_ = timeBase;
    volume_.store(std::clamp(volume, 0.0, 1.0));
    paused_.store(false);
    packetInputMode_.store(true);
    packetStreamEof_.store(false);
    pausePositionMs_.store(startPosition.count());
    pendingSeekMs_.store(-1);
    ioInterruptAfterSteadyMs_.store(0);
    ResetPlaybackClock(startPosition);
    {
        std::scoped_lock lock(packetMutex_);
        ClearPacketQueueLocked();
    }
    {
        std::scoped_lock lock(stateMutex_);
        startResolved_ = false;
        startSucceeded_ = false;
        lastStatus_ = L"wasapi packet pcm initializing";
    }

    stopping_.store(false);
    running_.store(true);
    playbackThread_ = std::thread([this]() { PlaybackLoop(); });

    std::unique_lock lock(stateMutex_);
    const bool resolved = startCv_.wait_for(lock, std::chrono::seconds(3), [this]() {
        return startResolved_;
    });
    const bool started = resolved && startSucceeded_;
    lock.unlock();

    if (!started) {
        Stop();
    }
    return started;
}

bool WasapiAudioPlayer::QueuePacket(const AVPacket* packet) {
    if (!packetInputMode_.load() || stopping_.load() || !packet) {
        return false;
    }
    AVPacket* copy = av_packet_clone(packet);
    if (!copy) {
        return false;
    }

    std::unique_lock lock(packetMutex_);
    if (packetQueueBytes_ >= kPacketStreamMaxQueueBytes) {
        packetCv_.wait_for(lock, std::chrono::milliseconds{2}, [this]() {
            return stopping_.load() ||
                   packetQueueBytes_ < kPacketStreamMaxQueueBytes;
        });
    }
    if (stopping_.load() || !packetInputMode_.load() || packetQueueBytes_ >= kPacketStreamMaxQueueBytes) {
        av_packet_free(&copy);
        return false;
    }
    packetQueueBytes_ += static_cast<std::size_t>(std::max(0, copy->size));
    packetQueue_.push_back(copy);
    packetCv_.notify_all();
    return true;
}

void WasapiAudioPlayer::ResetPacketStream(const std::chrono::milliseconds position) {
    if (!packetInputMode_.load()) {
        Seek(position);
        return;
    }
    pendingSeekMs_.store(std::max<int64_t>(0, position.count()));
    pausePositionMs_.store(std::max<int64_t>(0, position.count()));
    packetStreamEof_.store(false);
    {
        std::scoped_lock lock(packetMutex_);
        ClearPacketQueueLocked();
    }
    ResetPlaybackClock(position);
    packetCv_.notify_all();
}

void WasapiAudioPlayer::MarkPacketStreamEof() {
    if (!packetInputMode_.load()) {
        return;
    }
    packetStreamEof_.store(true);
    packetCv_.notify_all();
}

void WasapiAudioPlayer::Stop() {
    stopping_.store(true);
    paused_.store(false);
    pendingSeekMs_.store(-1);
    ioInterruptAfterSteadyMs_.store(0);
    packetStreamEof_.store(true);
    packetCv_.notify_all();
    SignalStart(false);
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    running_.store(false);
    packetInputMode_.store(false);
    {
        std::scoped_lock lock(packetMutex_);
        ClearPacketQueueLocked();
    }
    if (packetCodecParameters_) {
        avcodec_parameters_free(&packetCodecParameters_);
    }
    SetPlaybackClockRunning(false);
}

void WasapiAudioPlayer::Pause(const std::chrono::milliseconds position) {
    if (!running_.load()) {
        SetPlaybackClockRunning(false);
        return;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pausePositionMs_.store(clamped.count());
    pendingSeekMs_.store(-1);
    ResetPlaybackClock(clamped);
    SetPlaybackClockRunning(false);
    paused_.store(true);
}

bool WasapiAudioPlayer::Resume(const std::chrono::milliseconds position) {
    if (!running_.load()) {
        return false;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pendingSeekMs_.store(clamped.count());
    paused_.store(false);
    return true;
}

void WasapiAudioPlayer::HoldPacketStream(const std::chrono::milliseconds position) {
    if (!packetInputMode_.load()) {
        Pause(position);
        return;
    }
    if (!running_.load()) {
        SetPlaybackClockRunning(false);
        return;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pausePositionMs_.store(clamped.count());
    paused_.store(true);
    ResetPlaybackClock(clamped);
    SetPlaybackClockRunning(false);
    packetCv_.notify_all();
}

bool WasapiAudioPlayer::ResumePacketStream() {
    if (!packetInputMode_.load()) {
        return Resume(std::chrono::milliseconds{std::max<int64_t>(0, pausePositionMs_.load())});
    }
    if (!running_.load()) {
        return false;
    }

    paused_.store(false);
    packetCv_.notify_all();
    return true;
}

bool WasapiAudioPlayer::Seek(const std::chrono::milliseconds position) {
    if (!running_.load()) {
        return false;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pendingSeekMs_.store(clamped.count());
    paused_.store(false);
    ResetPlaybackClock(clamped);
    return true;
}

void WasapiAudioPlayer::SetVolume(const double volume) {
    volume_.store(std::clamp(volume, 0.0, 1.0));
}

int WasapiAudioPlayer::InterruptCallback(void* opaque) {
    const auto* player = static_cast<const WasapiAudioPlayer*>(opaque);
    if (!player) {
        return 0;
    }
    if (player->stopping_.load() || player->paused_.load() || player->HasPendingSeek()) {
        return 1;
    }
    const int64_t interruptAfterMs = player->ioInterruptAfterSteadyMs_.load();
    return interruptAfterMs > 0 && SteadyClockMs() >= interruptAfterMs ? 1 : 0;
}

void WasapiAudioPlayer::SetPlaybackRate(const double rate) {
    const double clamped = std::clamp(rate, 0.25, 4.0);
    {
        std::scoped_lock lock(clockMutex_);
        if (clockValid_ && clockRunning_) {
            const uint64_t playedFrames = PlayedFramesFromCounters(clockSubmittedFrames_, clockPaddingFrames_);
            const uint64_t deltaFrames = playedFrames >= clockAnchorPlayedFrames_
                                             ? playedFrames - clockAnchorPlayedFrames_
                                             : 0;
            clockPosition_ = clockAnchorPosition_ +
                             FramesToMediaDuration(deltaFrames, clockSampleRate_, clockPlaybackRate_);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - clockUpdatedAt_);
            clockPosition_ += ScaleDuration(std::min(elapsed, std::chrono::milliseconds{100}), clockPlaybackRate_);
            clockAnchorPosition_ = clockPosition_;
            clockAnchorPlayedFrames_ = playedFrames;
            clockUpdatedAt_ = std::chrono::steady_clock::now();
        }
        clockPlaybackRate_ = clamped;
    }
    playbackRate_.store(clamped);
}

std::wstring WasapiAudioPlayer::LastStatus() const {
    std::scoped_lock lock(stateMutex_);
    return lastStatus_;
}

std::optional<std::chrono::milliseconds> WasapiAudioPlayer::PlaybackClock() const {
    std::scoped_lock lock(clockMutex_);
    if (!clockValid_ || !clockRunning_) {
        return std::nullopt;
    }

    auto position = clockPosition_;
    if (clockRunning_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - clockUpdatedAt_);
        position += ScaleDuration(std::min(elapsed, std::chrono::milliseconds{100}), clockPlaybackRate_);
    }
    return position;
}

void WasapiAudioPlayer::PlaybackLoop() {
    CoInitScope com;
    if (!com.Ok()) {
        LogError(L"CoInitializeEx failed hr=0x" + HexHr(com.hr));
        SignalStart(false);
        running_.store(false);
        return;
    }

    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swrCtx = nullptr;
    bool audioClientStarted = false;
    int audioStreamIndex = -1;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    WasapiFormat outputFormat;
    ResamplerState resamplerState;
    uint64_t submittedFrames = 0;
    AVRational audioTimeBase{1, 1};

    auto cleanup = [&]() {
        if (audioClientStarted && audioClient) {
            DrainWasapi(audioClient.Get());
            audioClient->Stop();
        }
        if (!audioClientStarted) {
            SignalStart(false);
        }
        SetPlaybackClockRunning(false);
        if (swrCtx) swr_free(&swrCtx);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codecCtx) avcodec_free_context(&codecCtx);
        if (formatCtx) avformat_close_input(&formatCtx);
        running_.store(false);
    };

    const bool packetInput = packetInputMode_.load();

    do {
        const std::string pathUtf8 = WideToUtf8(path_.wstring());
        const bool networkSource = IsNetworkMediaPath(path_);
        const AVCodecParameters* inputCodecParameters = nullptr;
        if (packetInput) {
            if (!packetCodecParameters_) {
                LogError(L"packet stream missing codec parameters");
                break;
            }
            audioStreamIndex = packetStreamIndex_;
            audioTimeBase = packetTimeBase_;
            inputCodecParameters = packetCodecParameters_;
            LogInfo(L"packet audio stream=" + std::to_wstring(audioStreamIndex));
        } else {
            formatCtx = avformat_alloc_context();
            if (!formatCtx) {
                LogError(L"avformat_alloc_context failed");
                break;
            }
            formatCtx->interrupt_callback.callback = &WasapiAudioPlayer::InterruptCallback;
            formatCtx->interrupt_callback.opaque = this;
            AVDictionary* options = nullptr;
            if (networkSource) {
                av_dict_set(&options, "rw_timeout", "15000000", 0);
                av_dict_set(&options, "seekable", "0", 0);
                av_dict_set(&options,
                            "user_agent",
                            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36",
                            0);
                av_dict_set(&options, "reconnect_on_network_error", "1", 0);
                av_dict_set(&options, "reconnect_streamed", "1", 0);
                av_dict_set(&options, "reconnect_delay_max", "2", 0);
                av_dict_set(&options, "reconnect_max_retries", "2", 0);
                LogInfo(L"network open mode=linear_stream seekable=0");
            }
            int error = avformat_open_input(&formatCtx, pathUtf8.c_str(), nullptr, &options);
            av_dict_free(&options);
            if (error < 0) {
                LogError(L"avformat_open_input failed: " + FfmpegErrorString(error));
                break;
            }
            error = avformat_find_stream_info(formatCtx, nullptr);
            if (error < 0) {
                LogError(L"avformat_find_stream_info failed: " + FfmpegErrorString(error));
                break;
            }
            if (selectedAudioTrackIndex_ >= 0 &&
                selectedAudioTrackIndex_ < static_cast<int>(formatCtx->nb_streams) &&
                formatCtx->streams[selectedAudioTrackIndex_] &&
                formatCtx->streams[selectedAudioTrackIndex_]->codecpar &&
                formatCtx->streams[selectedAudioTrackIndex_]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = selectedAudioTrackIndex_;
            } else {
                if (selectedAudioTrackIndex_ >= 0) {
                    LogInfo(L"requested audio stream unavailable stream=" + std::to_wstring(selectedAudioTrackIndex_) +
                            L" fallback=auto");
                }
                audioStreamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            }
            if (audioStreamIndex < 0) {
                LogError(L"no audio stream found");
                break;
            }
            LogInfo(L"audio stream=" + std::to_wstring(audioStreamIndex));

            const AVStream* audioStream = formatCtx->streams[audioStreamIndex];
            audioTimeBase = audioStream->time_base;
            inputCodecParameters = audioStream->codecpar;
        }
        const AVCodec* codec = avcodec_find_decoder(inputCodecParameters->codec_id);
        if (!codec) {
            LogError(L"avcodec_find_decoder failed");
            break;
        }
        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            LogError(L"avcodec_alloc_context3 failed");
            break;
        }
        int error = avcodec_parameters_to_context(codecCtx, inputCodecParameters);
        if (error < 0) {
            LogError(L"avcodec_parameters_to_context failed: " + FfmpegErrorString(error));
            break;
        }
        error = avcodec_open2(codecCtx, codec, nullptr);
        if (error < 0) {
            LogError(L"avcodec_open2 failed: " + FfmpegErrorString(error));
            break;
        }

        UINT32 bufferFrameCount = 0;
        if (!InitializeWasapi(audioClient, renderClient, outputFormat, bufferFrameCount)) {
            break;
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame) {
            LogError(L"audio packet/frame allocation failed");
            break;
        }

        const bool skipNetworkNearStartSeek =
            networkSource && startPosition_ < std::chrono::seconds{1};
        if (!packetInput && startPosition_.count() > 0 && !skipNetworkNearStartSeek) {
            const int64_t target = static_cast<int64_t>(startPosition_.count()) * AV_TIME_BASE / 1000;
            LogInfo(L"seek target=" + FormatTimecode(startPosition_));
            const int seekError = av_seek_frame(formatCtx, -1, target, AVSEEK_FLAG_BACKWARD);
            if (seekError < 0) {
                LogError(L"av_seek_frame failed: " + FfmpegErrorString(seekError));
            }
            avcodec_flush_buffers(codecCtx);
        } else if (!packetInput && startPosition_.count() > 0 && skipNetworkNearStartSeek) {
            LogInfo(L"seek skipped reason=network_near_start target=" + FormatTimecode(startPosition_));
        }

        if (!packetInput) {
            HRESULT hr = audioClient->Start();
            if (FAILED(hr)) {
                LogError(L"IAudioClient::Start failed hr=0x" + HexHr(hr));
                break;
            }
            audioClientStarted = true;
            UpdatePlaybackClock(outputFormat, submittedFrames, 0);
            SetPlaybackClockRunning(true);
        } else {
            SetPlaybackClockRunning(false);
        }
        {
            std::scoped_lock lock(stateMutex_);
            lastStatus_ = outputFormat.description;
        }
        SignalStart(true);

        while (!stopping_.load()) {
            if (!HandlePause(audioClient.Get(), outputFormat, submittedFrames, audioClientStarted)) {
                break;
            }
            const bool seekApplied = packetInput
                                         ? ApplyPendingPacketSeek(codecCtx,
                                                                  swrCtx,
                                                                  resamplerState,
                                                                  audioClient.Get(),
                                                                  outputFormat,
                                                                  submittedFrames,
                                                                  audioClientStarted)
                                         : ApplyPendingSeek(formatCtx,
                                                            codecCtx,
                                                            swrCtx,
                                                            resamplerState,
                                                            audioClient.Get(),
                                                            outputFormat,
                                                            submittedFrames,
                                                            audioClientStarted);
            if (!seekApplied) {
                break;
            }

            if (packetInput) {
                AVPacket* queuedPacket = TakeQueuedPacket();
                if (!queuedPacket) {
                    if (packetStreamEof_.load()) {
                        avcodec_send_packet(codecCtx, nullptr);
                        DrainDecoder(codecCtx,
                                     swrCtx,
                                     frame,
                                     audioTimeBase,
                                     resamplerState,
                                     outputFormat,
                                     renderClient.Get(),
                                     audioClient.Get(),
                                     submittedFrames,
                                     audioClientStarted);
                        break;
                    }
                    continue;
                }

                const int sendResult = avcodec_send_packet(codecCtx, queuedPacket);
                av_packet_free(&queuedPacket);
                if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
                    continue;
                }
                if (!ReceiveFrames(codecCtx,
                                   swrCtx,
                                   frame,
                                   audioTimeBase,
                                   resamplerState,
                                   outputFormat,
                                   renderClient.Get(),
                                   audioClient.Get(),
                                   submittedFrames,
                                   audioClientStarted)) {
                    break;
                }
                continue;
            }

            const int readResult = av_read_frame(formatCtx, packet);
            if (readResult < 0) {
                if (paused_.load()) {
                    continue;
                }
                if (HasPendingSeek() &&
                    ApplyPendingSeek(formatCtx,
                                     codecCtx,
                                     swrCtx,
                                     resamplerState,
                                     audioClient.Get(),
                                     outputFormat,
                                     submittedFrames,
                                     audioClientStarted)) {
                    continue;
                }
                avcodec_send_packet(codecCtx, nullptr);
                DrainDecoder(codecCtx,
                             swrCtx,
                             frame,
                             audioTimeBase,
                             resamplerState,
                             outputFormat,
                             renderClient.Get(),
                             audioClient.Get(),
                             submittedFrames,
                             audioClientStarted);
                break;
            }

            if (packet->stream_index != audioStreamIndex) {
                av_packet_unref(packet);
                continue;
            }

            const int sendResult = avcodec_send_packet(codecCtx, packet);
            av_packet_unref(packet);
            if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
                continue;
            }

            if (!ReceiveFrames(codecCtx,
                               swrCtx,
                               frame,
                               audioTimeBase,
                               resamplerState,
                               outputFormat,
                               renderClient.Get(),
                               audioClient.Get(),
                               submittedFrames,
                               audioClientStarted)) {
                break;
            }
        }
    } while (false);

    cleanup();
}

std::optional<std::chrono::milliseconds> WasapiAudioPlayer::TakePendingSeek() {
    const int64_t ms = pendingSeekMs_.exchange(-1);
    if (ms < 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{ms};
}

bool WasapiAudioPlayer::HasPendingSeek() const {
    return pendingSeekMs_.load() >= 0;
}

bool WasapiAudioPlayer::HandlePause(IAudioClient* audioClient,
                                    const WasapiFormat& outputFormat,
                                    uint64_t& submittedFrames,
                                    bool& audioClientStarted) {
    if (!paused_.load()) {
        return true;
    }

    const auto position = std::chrono::milliseconds{std::max<int64_t>(0, pausePositionMs_.load())};
    startPosition_ = position;
    if (audioClientStarted && audioClient) {
        audioClient->Stop();
        audioClient->Reset();
        audioClientStarted = false;
    }
    submittedFrames = 0;
    ResetPlaybackClock(position);
    SetPlaybackClockRunning(false);

    while (paused_.load() && !stopping_.load() && !HasPendingSeek()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    if (stopping_.load()) {
        return false;
    }
    if (HasPendingSeek()) {
        return true;
    }
    if (!audioClient) {
        return false;
    }

    const HRESULT hr = audioClient->Start();
    if (FAILED(hr)) {
        LogError(L"IAudioClient::Start after pause failed hr=0x" + HexHr(hr));
        return false;
    }
    audioClientStarted = true;
    UpdatePlaybackClock(outputFormat, submittedFrames, 0);
    SetPlaybackClockRunning(true);
    return true;
}

bool WasapiAudioPlayer::ApplyPendingSeek(AVFormatContext* formatCtx,
                                         AVCodecContext* codecCtx,
                                         SwrContext*& swrCtx,
                                         ResamplerState& resamplerState,
                                         IAudioClient* audioClient,
                                         const WasapiFormat& outputFormat,
                                         uint64_t& submittedFrames,
                                         bool& audioClientStarted) {
    const auto target = TakePendingSeek();
    if (!target.has_value()) {
        return true;
    }
    if (!formatCtx || !codecCtx || !audioClient) {
        return false;
    }

    startPosition_ = *target;
    LogInfo(L"runtime seek target=" + FormatTimecode(*target));
    if (audioClientStarted) {
        audioClient->Stop();
        audioClient->Reset();
        audioClientStarted = false;
    }

    const int64_t seekTarget = static_cast<int64_t>(target->count()) * AV_TIME_BASE / 1000;
    const auto seekStart = std::chrono::steady_clock::now();
    ioInterruptAfterSteadyMs_.store(SteadyClockMs() + kRuntimeSeekIoTimeout.count());
    int seekError = av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (seekError < 0) {
        seekError = avformat_seek_file(formatCtx, -1, INT64_MIN, seekTarget, INT64_MAX, 0);
    }
    ioInterruptAfterSteadyMs_.store(0);
    const auto seekElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - seekStart).count();
    if (seekError < 0) {
        const bool seekInterrupted = seekError == AVERROR_EXIT ||
                                     seekElapsedMs >= kRuntimeSeekIoTimeout.count();
        LogError(L"runtime seek failed: " + FfmpegErrorString(seekError) +
                 L" seek_ms=" + std::to_wstring(seekElapsedMs) +
                 (seekInterrupted ? L" interrupted=true" : L""));
        if (stopping_.load()) {
            return false;
        }
        return HasPendingSeek();
    }
    LogInfo(L"runtime seek completed seek_ms=" + std::to_wstring(seekElapsedMs));

    avformat_flush(formatCtx);
    avcodec_flush_buffers(codecCtx);
    if (swrCtx) {
        swr_free(&swrCtx);
    }
    resamplerState = {};
    submittedFrames = 0;
    ResetPlaybackClock();

    const HRESULT hr = audioClient->Start();
    if (FAILED(hr)) {
        LogError(L"IAudioClient::Start after seek failed hr=0x" + HexHr(hr));
        return false;
    }
    audioClientStarted = true;
    UpdatePlaybackClock(outputFormat, submittedFrames, 0);
    SetPlaybackClockRunning(true);
    return true;
}

bool WasapiAudioPlayer::ApplyPendingPacketSeek(AVCodecContext* codecCtx,
                                               SwrContext*& swrCtx,
                                               ResamplerState& resamplerState,
                                               IAudioClient* audioClient,
                                               const WasapiFormat& outputFormat,
                                               uint64_t& submittedFrames,
                                               bool& audioClientStarted) {
    const auto target = TakePendingSeek();
    if (!target.has_value()) {
        return true;
    }
    if (!codecCtx || !audioClient) {
        return false;
    }

    startPosition_ = *target;
    LogInfo(L"packet runtime seek target=" + FormatTimecode(*target));
    if (audioClientStarted) {
        audioClient->Stop();
        audioClient->Reset();
        audioClientStarted = false;
    }
    {
        std::scoped_lock lock(packetMutex_);
        ClearPacketQueueLocked();
    }
    packetStreamEof_.store(false);
    avcodec_flush_buffers(codecCtx);
    if (swrCtx) {
        swr_free(&swrCtx);
    }
    resamplerState = {};
    submittedFrames = 0;
    ResetPlaybackClock(*target);
    SetPlaybackClockRunning(false);
    packetCv_.notify_all();
    return true;
}

bool WasapiAudioPlayer::InitializeWasapi(Microsoft::WRL::ComPtr<IAudioClient>& audioClient,
                                         Microsoft::WRL::ComPtr<IAudioRenderClient>& renderClient,
                                         WasapiFormat& outputFormat,
                                         UINT32& bufferFrameCount) const {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        LogError(L"CoCreateInstance(MMDeviceEnumerator) failed hr=0x" + HexHr(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        LogError(L"GetDefaultAudioEndpoint failed hr=0x" + HexHr(hr));
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
    if (FAILED(hr)) {
        LogError(L"IMMDevice::Activate(IAudioClient) failed hr=0x" + HexHr(hr));
        return false;
    }

    WAVEFORMATEX* rawMixFormat = nullptr;
    hr = audioClient->GetMixFormat(&rawMixFormat);
    std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> mixFormat(rawMixFormat);
    if (FAILED(hr) || !mixFormat) {
        LogError(L"IAudioClient::GetMixFormat failed hr=0x" + HexHr(hr));
        return false;
    }

    if (!DescribeMixFormat(mixFormat.get(), outputFormat)) {
        LogError(L"unsupported WASAPI mix format");
        return false;
    }

    constexpr REFERENCE_TIME bufferDuration = 10000000 / 10;  // 100 ms.
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 0,
                                 bufferDuration,
                                 0,
                                 mixFormat.get(),
                                 nullptr);
    if (FAILED(hr)) {
        LogError(L"IAudioClient::Initialize failed hr=0x" + HexHr(hr));
        return false;
    }

    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        LogError(L"IAudioClient::GetBufferSize failed hr=0x" + HexHr(hr));
        return false;
    }

    hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr)) {
        LogError(L"IAudioClient::GetService(IAudioRenderClient) failed hr=0x" + HexHr(hr));
        return false;
    }
    return true;
}

bool WasapiAudioPlayer::DescribeMixFormat(const WAVEFORMATEX* format, WasapiFormat& output) const {
    if (!format || format->nChannels == 0 || format->nSamplesPerSec == 0 || format->nBlockAlign == 0) {
        return false;
    }

    bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        isFloat = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        isPcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }

    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE;
    std::wstring sampleName;
    if (isFloat && format->wBitsPerSample == 32) {
        sampleFormat = AV_SAMPLE_FMT_FLT;
        sampleName = L"float32";
    } else if (isPcm && format->wBitsPerSample == 16) {
        sampleFormat = AV_SAMPLE_FMT_S16;
        sampleName = L"s16";
    } else if (isPcm && format->wBitsPerSample == 32) {
        sampleFormat = AV_SAMPLE_FMT_S32;
        sampleName = L"s32";
    } else {
        return false;
    }

    output.sampleRate = format->nSamplesPerSec;
    output.channels = format->nChannels;
    output.blockAlign = format->nBlockAlign;
    output.sampleFormat = sampleFormat;
    output.description =
        L"wasapi shared pcm " +
        std::to_wstring(output.sampleRate) +
        L" Hz " +
        std::to_wstring(output.channels) +
        L" ch " +
        sampleName;
    return true;
}

bool WasapiAudioPlayer::ReceiveFrames(AVCodecContext* codecCtx,
                                      SwrContext*& swrCtx,
                                      AVFrame* frame,
                                      const AVRational timeBase,
                                      ResamplerState& resamplerState,
                                      const WasapiFormat& outputFormat,
                                      IAudioRenderClient* renderClient,
                                      IAudioClient* audioClient,
                                      uint64_t& submittedFrames,
                                      bool& audioClientStarted) {
    while (!stopping_.load() && !paused_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            LogError(L"avcodec_receive_frame failed: " + FfmpegErrorString(ret));
            return false;
        }
        if (!RenderFrame(frame,
                         timeBase,
                         swrCtx,
                         resamplerState,
                         outputFormat,
                         renderClient,
                         audioClient,
                         submittedFrames,
                         audioClientStarted)) {
            return false;
        }
    }
    return true;
}

void WasapiAudioPlayer::DrainDecoder(AVCodecContext* codecCtx,
                                     SwrContext*& swrCtx,
                                     AVFrame* frame,
                                     const AVRational timeBase,
                                     ResamplerState& resamplerState,
                                     const WasapiFormat& outputFormat,
                                     IAudioRenderClient* renderClient,
                                     IAudioClient* audioClient,
                                     uint64_t& submittedFrames,
                                     bool& audioClientStarted) {
    while (!stopping_.load() && !paused_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        if (ret < 0) {
            return;
        }
        if (!RenderFrame(frame,
                         timeBase,
                         swrCtx,
                         resamplerState,
                         outputFormat,
                         renderClient,
                         audioClient,
                         submittedFrames,
                         audioClientStarted)) {
            return;
        }
    }
}

bool WasapiAudioPlayer::RenderFrame(AVFrame* frame,
                                    const AVRational timeBase,
                                    SwrContext*& swrCtx,
                                    ResamplerState& resamplerState,
                                    const WasapiFormat& outputFormat,
                                    IAudioRenderClient* renderClient,
                                    IAudioClient* audioClient,
                                    uint64_t& submittedFrames,
                                    bool& audioClientStarted) {
    if (!frame || frame->nb_samples <= 0 || !renderClient || !audioClient) {
        return true;
    }
    if (paused_.load() || HasPendingSeek()) {
        return true;
    }
    if (ShouldDropSeekPreroll(frame, timeBase, outputFormat)) {
        return true;
    }

    const double rate = std::clamp(playbackRate_.load(), 0.25, 4.0);
    const int inputSampleRate = frame->sample_rate > 0 ? frame->sample_rate : static_cast<int>(outputFormat.sampleRate);
    const int resampleInputSampleRate = std::max(1, static_cast<int>(std::lround(static_cast<double>(inputSampleRate) * rate)));
    const int inputFormat = frame->format;
    const int inputChannels = frame->ch_layout.nb_channels > 0
                                  ? frame->ch_layout.nb_channels
                                  : static_cast<int>(outputFormat.channels);

    if (!swrCtx ||
        resamplerState.sampleRate != inputSampleRate ||
        resamplerState.sampleFormat != inputFormat ||
        resamplerState.channels != inputChannels ||
        std::abs(resamplerState.playbackRate - rate) > 0.001) {
        if (swrCtx) {
            swr_free(&swrCtx);
        }

        AVChannelLayout inputLayout{};
        if (frame->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&inputLayout, &frame->ch_layout);
        } else {
            av_channel_layout_default(&inputLayout, inputChannels);
        }
        AVChannelLayout outputLayout{};
        av_channel_layout_default(&outputLayout, static_cast<int>(outputFormat.channels));

        const int error = swr_alloc_set_opts2(&swrCtx,
                                              &outputLayout,
                                              outputFormat.sampleFormat,
                                              static_cast<int>(outputFormat.sampleRate),
                                              &inputLayout,
                                              static_cast<AVSampleFormat>(inputFormat),
                                              resampleInputSampleRate,
                                              0,
                                              nullptr);
        av_channel_layout_uninit(&inputLayout);
        av_channel_layout_uninit(&outputLayout);
        if (error < 0 || !swrCtx) {
            LogError(L"swr_alloc_set_opts2 failed: " + FfmpegErrorString(error));
            return false;
        }
        const int initError = swr_init(swrCtx);
        if (initError < 0) {
            LogError(L"swr_init failed: " + FfmpegErrorString(initError));
            return false;
        }

        resamplerState.sampleRate = inputSampleRate;
        resamplerState.sampleFormat = inputFormat;
        resamplerState.channels = inputChannels;
        resamplerState.playbackRate = rate;
    }

    const int maxOutputSamples = static_cast<int>(
        av_rescale_rnd(swr_get_delay(swrCtx, resampleInputSampleRate) + frame->nb_samples,
                       outputFormat.sampleRate,
                       resampleInputSampleRate,
                       AV_ROUND_UP));
    if (maxOutputSamples <= 0) {
        return true;
    }

    const int bytesPerSample = av_get_bytes_per_sample(outputFormat.sampleFormat);
    if (bytesPerSample <= 0) {
        return false;
    }

    std::vector<uint8_t> pcm(static_cast<std::size_t>(maxOutputSamples) *
                             static_cast<std::size_t>(outputFormat.channels) *
                             static_cast<std::size_t>(bytesPerSample));
    uint8_t* outData[1] = {pcm.data()};
    const int convertedSamples = swr_convert(swrCtx,
                                             outData,
                                             maxOutputSamples,
                                             const_cast<const uint8_t**>(frame->extended_data),
                                             frame->nb_samples);
    if (convertedSamples < 0) {
        LogError(L"swr_convert failed: " + FfmpegErrorString(convertedSamples));
        return false;
    }
    if (convertedSamples == 0) {
        return true;
    }

    const std::size_t bytes = static_cast<std::size_t>(convertedSamples) *
                              static_cast<std::size_t>(outputFormat.channels) *
                              static_cast<std::size_t>(bytesPerSample);
    pcm.resize(bytes);
    ApplyVolume(pcm, outputFormat.sampleFormat);
    return WritePcm(renderClient,
                    audioClient,
                    outputFormat,
                    pcm.data(),
                    static_cast<UINT32>(convertedSamples),
                    submittedFrames,
                    audioClientStarted);
}

bool WasapiAudioPlayer::WritePcm(IAudioRenderClient* renderClient,
                                 IAudioClient* audioClient,
                                 const WasapiFormat& outputFormat,
                                 const uint8_t* data,
                                 UINT32 frames,
                                 uint64_t& submittedFrames,
                                 bool& audioClientStarted) {
    UINT32 offsetFrames = 0;
    while (offsetFrames < frames && !stopping_.load() && !paused_.load() && !HasPendingSeek()) {
        UINT32 bufferFrames = 0;
        HRESULT hr = audioClient->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || bufferFrames == 0) {
            LogError(L"IAudioClient::GetBufferSize failed hr=0x" + HexHr(hr));
            return false;
        }

        UINT32 padding = 0;
        hr = audioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            LogError(L"IAudioClient::GetCurrentPadding failed hr=0x" + HexHr(hr));
            return false;
        }
        if (audioClientStarted) {
            UpdatePlaybackClock(outputFormat, submittedFrames, padding);
        }

        const UINT32 available = bufferFrames > padding ? bufferFrames - padding : 0;
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const UINT32 framesToWrite = std::min(available, frames - offsetFrames);
        BYTE* buffer = nullptr;
        hr = renderClient->GetBuffer(framesToWrite, &buffer);
        if (FAILED(hr)) {
            LogError(L"IAudioRenderClient::GetBuffer failed hr=0x" + HexHr(hr));
            return false;
        }

        const std::size_t byteOffset = static_cast<std::size_t>(offsetFrames) *
                                       static_cast<std::size_t>(outputFormat.blockAlign);
        const std::size_t byteCount = static_cast<std::size_t>(framesToWrite) *
                                      static_cast<std::size_t>(outputFormat.blockAlign);
        std::memcpy(buffer, data + byteOffset, byteCount);
        hr = renderClient->ReleaseBuffer(framesToWrite, 0);
        if (FAILED(hr)) {
            LogError(L"IAudioRenderClient::ReleaseBuffer failed hr=0x" + HexHr(hr));
            return false;
        }

        offsetFrames += framesToWrite;
        submittedFrames += framesToWrite;
        if (!audioClientStarted) {
            hr = audioClient->Start();
            if (FAILED(hr)) {
                LogError(L"IAudioClient::Start after packet preroll failed hr=0x" + HexHr(hr));
                return false;
            }
            audioClientStarted = true;
        }
        UpdatePlaybackClock(outputFormat, submittedFrames, padding + framesToWrite);
    }
    return !stopping_.load();
}

void WasapiAudioPlayer::ApplyVolume(std::vector<uint8_t>& pcm, const AVSampleFormat format) const {
    const double volume = volume_.load();
    if (std::abs(volume - 1.0) < 0.0001) {
        return;
    }

    if (format == AV_SAMPLE_FMT_FLT) {
        auto* samples = reinterpret_cast<float*>(pcm.data());
        const std::size_t count = pcm.size() / sizeof(float);
        for (std::size_t index = 0; index < count; ++index) {
            samples[index] = static_cast<float>(samples[index] * volume);
        }
    } else if (format == AV_SAMPLE_FMT_S16) {
        auto* samples = reinterpret_cast<int16_t*>(pcm.data());
        const std::size_t count = pcm.size() / sizeof(int16_t);
        for (std::size_t index = 0; index < count; ++index) {
            const double scaled = static_cast<double>(samples[index]) * volume;
            samples[index] = static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0));
        }
    } else if (format == AV_SAMPLE_FMT_S32) {
        auto* samples = reinterpret_cast<int32_t*>(pcm.data());
        const std::size_t count = pcm.size() / sizeof(int32_t);
        constexpr double minSample = static_cast<double>((std::numeric_limits<int32_t>::min)());
        constexpr double maxSample = static_cast<double>((std::numeric_limits<int32_t>::max)());
        for (std::size_t index = 0; index < count; ++index) {
            const double scaled = static_cast<double>(samples[index]) * volume;
            samples[index] = static_cast<int32_t>(std::clamp(scaled, minSample, maxSample));
        }
    }
}

bool WasapiAudioPlayer::ShouldDropSeekPreroll(const AVFrame* frame,
                                              const AVRational timeBase,
                                              const WasapiFormat& outputFormat) const {
    if (startPosition_.count() <= 0 || !frame || frame->nb_samples <= 0) {
        return false;
    }

    const auto pts = FramePts(frame, timeBase);
    if (pts.count() <= 0) {
        return false;
    }

    constexpr std::chrono::milliseconds kAudioPrerollTolerance{20};
    return pts + FrameDuration(frame, outputFormat) + kAudioPrerollTolerance < startPosition_;
}

std::chrono::milliseconds WasapiAudioPlayer::FramePts(const AVFrame* frame, const AVRational timeBase) {
    if (!frame) {
        return std::chrono::milliseconds{0};
    }
    const int64_t ptsTicks = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                 ? frame->best_effort_timestamp
                                 : frame->pts;
    if (ptsTicks == AV_NOPTS_VALUE) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::milliseconds{av_rescale_q(ptsTicks, timeBase, {1, 1000})};
}

std::chrono::milliseconds WasapiAudioPlayer::FrameDuration(const AVFrame* frame, const WasapiFormat& outputFormat) {
    if (!frame || frame->nb_samples <= 0) {
        return std::chrono::milliseconds{0};
    }
    const int sampleRate = frame->sample_rate > 0 ? frame->sample_rate : static_cast<int>(outputFormat.sampleRate);
    if (sampleRate <= 0) {
        return std::chrono::milliseconds{0};
    }
    const int64_t ms = av_rescale_q(frame->nb_samples, {1, sampleRate}, {1, 1000});
    return std::chrono::milliseconds{ms};
}

void WasapiAudioPlayer::DrainWasapi(IAudioClient* audioClient) const {
    if (!audioClient || stopping_.load()) {
        return;
    }
    for (int attempts = 0; attempts < 200 && !stopping_.load(); ++attempts) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding)) || padding == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

AVPacket* WasapiAudioPlayer::TakeQueuedPacket() {
    std::unique_lock lock(packetMutex_);
    if (packetQueue_.empty() && !packetStreamEof_.load() && !stopping_.load() && !paused_.load() && !HasPendingSeek()) {
        packetCv_.wait_for(lock, std::chrono::milliseconds{10});
    }
    if (packetQueue_.empty()) {
        return nullptr;
    }
    AVPacket* packet = packetQueue_.front();
    packetQueue_.pop_front();
    const std::size_t bytes = static_cast<std::size_t>(std::max(0, packet ? packet->size : 0));
    packetQueueBytes_ = packetQueueBytes_ >= bytes ? packetQueueBytes_ - bytes : 0;
    packetCv_.notify_all();
    return packet;
}

void WasapiAudioPlayer::ClearPacketQueueLocked() {
    for (AVPacket* packet : packetQueue_) {
        av_packet_free(&packet);
    }
    packetQueue_.clear();
    packetQueueBytes_ = 0;
    packetCv_.notify_all();
}

void WasapiAudioPlayer::ResetPlaybackClock() {
    ResetPlaybackClock(startPosition_);
}

void WasapiAudioPlayer::ResetPlaybackClock(const std::chrono::milliseconds position) {
    std::scoped_lock lock(clockMutex_);
    clockValid_ = false;
    clockRunning_ = false;
    clockPlaybackRate_ = std::clamp(playbackRate_.load(), 0.25, 4.0);
    clockPosition_ = position;
    clockAnchorPosition_ = position;
    clockAnchorPlayedFrames_ = 0;
    clockSubmittedFrames_ = 0;
    clockPaddingFrames_ = 0;
    clockSampleRate_ = 0;
    clockUpdatedAt_ = std::chrono::steady_clock::now();
}

void WasapiAudioPlayer::SetPlaybackClockRunning(const bool running) {
    std::scoped_lock lock(clockMutex_);
    clockRunning_ = running && clockValid_;
    clockUpdatedAt_ = std::chrono::steady_clock::now();
}

void WasapiAudioPlayer::UpdatePlaybackClock(const WasapiFormat& outputFormat,
                                            const uint64_t submittedFrames,
                                            const UINT32 paddingFrames) {
    if (outputFormat.sampleRate == 0) {
        return;
    }

    std::scoped_lock lock(clockMutex_);
    const uint64_t playedFrames = PlayedFramesFromCounters(submittedFrames, paddingFrames);
    if (!clockValid_ || clockSampleRate_ != outputFormat.sampleRate) {
        clockAnchorPosition_ = startPosition_;
        clockAnchorPlayedFrames_ = 0;
        clockPlaybackRate_ = std::clamp(playbackRate_.load(), 0.25, 4.0);
    }
    clockSubmittedFrames_ = submittedFrames;
    clockPaddingFrames_ = paddingFrames;
    clockSampleRate_ = outputFormat.sampleRate;
    const uint64_t deltaFrames = playedFrames >= clockAnchorPlayedFrames_
                                     ? playedFrames - clockAnchorPlayedFrames_
                                     : 0;
    clockValid_ = true;
    clockRunning_ = true;
    clockPosition_ = clockAnchorPosition_ +
                     FramesToMediaDuration(deltaFrames, outputFormat.sampleRate, clockPlaybackRate_);
    clockUpdatedAt_ = std::chrono::steady_clock::now();
}

void WasapiAudioPlayer::SignalStart(const bool success) {
    std::scoped_lock lock(stateMutex_);
    if (!startResolved_) {
        startSucceeded_ = success;
        startResolved_ = true;
        startCv_.notify_all();
    }
}

void WasapiAudioPlayer::LogError(const std::wstring& message) const {
    if (logSink_) {
        logSink_->Write(LogLevel::Error, L"wasapi", message);
    }
    OutputDebugStringW((L"[wasapi] " + message + L"\n").c_str());
}

void WasapiAudioPlayer::LogInfo(const std::wstring& message) const {
    if (logSink_) {
        logSink_->Write(LogLevel::Debug, L"wasapi", message);
    }
    OutputDebugStringW((L"[wasapi] " + message + L"\n").c_str());
}

}  // namespace anvil::app
