#include "AnvilPlayer/App/wasapi_audio_player.h"

#include "AnvilPlayer/App/string_util.h"

#include <ksmedia.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/opt.h>
}

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

std::wstring PassthroughCodecName(const AVCodecParameters* parameters) {
    if (!parameters) return {};
    switch (parameters->codec_id) {
    case AV_CODEC_ID_AC3: return L"AC-3";
    case AV_CODEC_ID_EAC3: return L"E-AC-3";
    case AV_CODEC_ID_TRUEHD: return L"TrueHD";
    case AV_CODEC_ID_DTS:
        return parameters->profile == AV_PROFILE_DTS_HD_HRA ||
                       parameters->profile == AV_PROFILE_DTS_HD_MA ||
                       parameters->profile == AV_PROFILE_DTS_HD_MA_X ||
                       parameters->profile == AV_PROFILE_DTS_HD_MA_X_IMAX
                   ? L"DTS-HD"
                   : L"DTS";
    default: return {};
    }
}

struct SpdifPacketizer {
    ~SpdifPacketizer() { Close(); }

    bool Open(const AVCodecParameters* parameters, const AVRational timeBase, std::wstring& error) {
        Close();
        if (!parameters) {
            error = L"missing codec parameters";
            return false;
        }
        int result = avformat_alloc_output_context2(&context, nullptr, "spdif", nullptr);
        if (result < 0 || !context) {
            error = L"FFmpeg spdif muxer unavailable";
            return false;
        }
        stream = avformat_new_stream(context, nullptr);
        if (!stream) {
            error = L"FFmpeg spdif stream allocation failed";
            Close();
            return false;
        }
        result = avcodec_parameters_copy(stream->codecpar, parameters);
        if (result < 0) {
            error = L"FFmpeg spdif codec copy failed";
            Close();
            return false;
        }
        stream->codecpar->codec_tag = 0;
        stream->time_base = timeBase.num > 0 && timeBase.den > 0 ? timeBase : AVRational{1, 48000};
        if (parameters->codec_id == AV_CODEC_ID_DTS &&
            (parameters->profile == AV_PROFILE_DTS_HD_HRA ||
             parameters->profile == AV_PROFILE_DTS_HD_MA ||
             parameters->profile == AV_PROFILE_DTS_HD_MA_X ||
             parameters->profile == AV_PROFILE_DTS_HD_MA_X_IMAX)) {
            // FFmpeg otherwise strips the HD extension and emits only DTS
            // core. 768 kHz is the IEC 60958 frame rate of an HBR 8-channel
            // 192 kHz carrier.
            av_opt_set_int(context->priv_data, "dtshd_rate", 768000, 0);
            av_opt_set_int(context->priv_data, "dtshd_fallback_time", -1, 0);
        }

        constexpr int kIoBufferSize = 32768;
        unsigned char* ioBuffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
        if (!ioBuffer) {
            error = L"FFmpeg spdif IO allocation failed";
            Close();
            return false;
        }
        io = avio_alloc_context(ioBuffer, kIoBufferSize, 1, this, nullptr, &Write, nullptr);
        if (!io) {
            av_free(ioBuffer);
            error = L"FFmpeg spdif IO context failed";
            Close();
            return false;
        }
        context->pb = io;
        context->flags |= AVFMT_FLAG_CUSTOM_IO;
        result = avformat_write_header(context, nullptr);
        if (result < 0) {
            error = L"FFmpeg spdif header failed";
            Close();
            return false;
        }
        headerWritten = true;
        bytes.clear();
        return true;
    }

    bool Packetize(const AVPacket* source, std::vector<uint8_t>& output, std::wstring& error) {
        output.clear();
        if (!context || !source) return false;
        AVPacket* packet = av_packet_clone(source);
        if (!packet) {
            error = L"FFmpeg spdif packet clone failed";
            return false;
        }
        packet->stream_index = stream->index;
        bytes.clear();
        const int result = av_write_frame(context, packet);
        av_packet_free(&packet);
        avio_flush(io);
        if (result < 0) {
            error = L"FFmpeg spdif packetization failed";
            return false;
        }
        output.swap(bytes);
        return true;
    }

    void Close() {
        if (context && headerWritten) av_write_trailer(context);
        headerWritten = false;
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        if (context) avformat_free_context(context);
        context = nullptr;
        stream = nullptr;
        bytes.clear();
    }

    static int Write(void* opaque, const uint8_t* data, const int size) {
        if (!opaque || !data || size <= 0) return AVERROR(EINVAL);
        auto& self = *static_cast<SpdifPacketizer*>(opaque);
        self.bytes.insert(self.bytes.end(), data, data + size);
        return size;
    }

    AVFormatContext* context = nullptr;
    AVStream* stream = nullptr;
    AVIOContext* io = nullptr;
    bool headerWritten = false;
    std::vector<uint8_t> bytes;
};

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
                              const int selectedAudioTrackIndex,
                              const bool preferPassthrough) {
    if (!PrepareWorkerForStart()) {
        return false;
    }
    if (mediaPath.empty() || selectedAudioTrackIndex == anvil::playback::kAudioTrackOff) {
        return false;
    }

    path_ = mediaPath;
    startPosition_ = startPosition;
    selectedAudioTrackIndex_ = selectedAudioTrackIndex;
    volume_.store(std::clamp(volume, 0.0, 1.0));
    passthroughRequested_.store(preferPassthrough);
    SetPassthroughRuntime(false, preferPassthrough ? L"negotiating" : L"disabled");
    paused_.store(false);
    pausePositionMs_.store(startPosition.count());
    pendingSeekMs_.store(-1);
    ioInterruptAfterSteadyMs_.store(0);
    ResetPlaybackClock();
    {
        std::scoped_lock lock(stateMutex_);
        startResolved_ = false;
        startSucceeded_ = false;
        lastStatus_ = preferPassthrough
                          ? L"wasapi exclusive bitstream initializing"
                          : L"wasapi shared pcm initializing";
    }

    {
        std::scoped_lock lock(workerMutex_);
        const uint64_t workerGeneration = startGeneration_.fetch_add(1) + 1;
        stopping_.store(false);
        running_.store(true);
        runtimeState_.store(WasapiRuntimeState::Starting);
        workerFinished_.store(false);
        try {
            playbackThread_ = std::thread([this, workerGeneration]() {
                PlaybackLoop(workerGeneration);
            });
        } catch (...) {
            workerFinished_.store(true);
            running_.store(false);
            stopping_.store(true);
            runtimeState_.store(WasapiRuntimeState::Failed);
            return false;
        }
    }
    return true;
}

bool WasapiAudioPlayer::StartPacketStream(const std::filesystem::path& mediaPath,
                                          const AVCodecParameters* codecParameters,
                                          const AVRational timeBase,
                                          const std::chrono::milliseconds startPosition,
                                          const double volume,
                                          const int streamIndex,
                                          const uint64_t startGeneration,
                                          const bool preferPassthrough) {
    const auto failCurrentStart = [this, startGeneration](const std::wstring& status) {
        FailPendingStart(startGeneration, status);
        return false;
    };
    if (startGeneration == 0 || startGeneration != startGeneration_.load() || stopping_.load()) {
        return false;
    }
    if (runtimeState_.load() != WasapiRuntimeState::Starting) {
        return failCurrentStart(L"wasapi packet start state invalid");
    }
    if (!PrepareWorkerForStart()) {
        return failCurrentStart(L"wasapi previous worker is still retiring");
    }
    if (mediaPath.empty() || !codecParameters || codecParameters->codec_type != AVMEDIA_TYPE_AUDIO) {
        return failCurrentStart(L"wasapi packet stream parameters invalid");
    }

    AVCodecParameters* preparedParameters = avcodec_parameters_alloc();
    if (!preparedParameters) {
        return failCurrentStart(L"wasapi packet codec allocation failed");
    }
    const int copyError = avcodec_parameters_copy(preparedParameters, codecParameters);
    if (copyError < 0) {
        avcodec_parameters_free(&preparedParameters);
        LogError(L"packet stream codec copy failed: " + FfmpegErrorString(copyError));
        return failCurrentStart(L"wasapi packet codec copy failed");
    }

    {
        std::scoped_lock lock(workerMutex_);
        // The generation is checked while holding the same short lock used by
        // Stop(). If cancellation won the race, no late audio worker may be
        // created for the retired playback session.
        if (startGeneration != startGeneration_.load() ||
            runtimeState_.load() != WasapiRuntimeState::Starting || stopping_.load()) {
            avcodec_parameters_free(&preparedParameters);
            return false;
        }
        packetCodecParameters_ = preparedParameters;
        path_ = mediaPath;
        const int64_t armedPositionMs = pausePositionMs_.load();
        startPosition_ = armedPositionMs >= 0
                             ? std::chrono::milliseconds{armedPositionMs}
                             : std::max(startPosition, std::chrono::milliseconds{0});
        selectedAudioTrackIndex_ = streamIndex;
        packetStreamIndex_ = streamIndex;
        packetTimeBase_ = timeBase;
        volume_.store(std::clamp(volume, 0.0, 1.0));
        passthroughRequested_.store(preferPassthrough);
        SetPassthroughRuntime(false, preferPassthrough ? L"negotiating" : L"disabled");
        packetInputMode_.store(true);
        packetStreamEof_.store(false);
        pendingSeekMs_.store(-1);
        ioInterruptAfterSteadyMs_.store(0);
        ResetPlaybackClock(startPosition_);
        {
            std::scoped_lock packetLock(packetMutex_);
            ClearPacketQueueLocked();
        }
        {
            std::scoped_lock stateLock(stateMutex_);
            startResolved_ = false;
            startSucceeded_ = false;
            lastStatus_ = preferPassthrough
                              ? L"wasapi packet bitstream initializing"
                              : L"wasapi packet pcm initializing";
        }
        running_.store(true);
        workerFinished_.store(false);
        try {
            playbackThread_ = std::thread([this, startGeneration]() {
                PlaybackLoop(startGeneration);
            });
        } catch (...) {
            workerFinished_.store(true);
            running_.store(false);
            packetInputMode_.store(false);
            return failCurrentStart(L"wasapi packet worker creation failed");
        }
    }
    return true;
}

bool WasapiAudioPlayer::PrepareWorkerForStart() {
    {
        std::scoped_lock lock(workerMutex_);
        if (playbackThread_.joinable()) {
            if (!workerFinished_.load()) {
                return false;
            }
            playbackThread_.detach();
        }
    }
    packetInputMode_.store(false);
    {
        std::scoped_lock packetLock(packetMutex_);
        ClearPacketQueueLocked();
    }
    if (packetCodecParameters_) {
        avcodec_parameters_free(&packetCodecParameters_);
    }
    return true;
}

bool WasapiAudioPlayer::HasStartFailed() const {
    const auto state = runtimeState_.load();
    return state == WasapiRuntimeState::Failed || state == WasapiRuntimeState::Ended;
}

uint64_t WasapiAudioPlayer::ArmPendingStart(const std::chrono::milliseconds position) {
    const uint64_t generation = startGeneration_.fetch_add(1) + 1;
    stopping_.store(false);
    paused_.store(false);
    pendingSeekMs_.store(-1);
    pausePositionMs_.store(std::max(position, std::chrono::milliseconds{0}).count());
    {
        std::scoped_lock lock(stateMutex_);
        startResolved_ = false;
        startSucceeded_ = false;
        lastStatus_ = L"wasapi packet start armed";
    }
    runtimeState_.store(WasapiRuntimeState::Starting);
    return generation;
}

void WasapiAudioPlayer::FailPendingStart(const uint64_t startGeneration,
                                         std::wstring status) {
    if (startGeneration == 0 ||
        startGeneration != startGeneration_.load() ||
        stopping_.load()) {
        return;
    }

    std::scoped_lock lock(stateMutex_);
    if (startGeneration != startGeneration_.load() ||
        stopping_.load() ||
        runtimeState_.load() != WasapiRuntimeState::Starting) {
        return;
    }
    startSucceeded_ = false;
    startResolved_ = true;
    lastStatus_ = status.empty() ? L"wasapi packet start failed" : std::move(status);
    runtimeState_.store(WasapiRuntimeState::Failed);
    startCv_.notify_all();
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

void WasapiAudioPlayer::RequestStop() {
    const uint64_t cancelledGeneration = startGeneration_.load();
    stopping_.store(true);
    runtimeState_.store(WasapiRuntimeState::Stopping);
    paused_.store(false);
    pendingSeekMs_.store(-1);
    ioInterruptAfterSteadyMs_.store(0);
    packetStreamEof_.store(true);
    packetCv_.notify_all();
    SignalStart(false, cancelledGeneration);
    startGeneration_.fetch_add(1);

    const DWORD playbackThreadId = playbackThreadId_.load();
    if (playbackThreadId != 0) {
        // WASAPI endpoint discovery and activation use COM. Enable/disable is
        // owned by PlaybackLoop; this requests cancellation without waiting.
        CoCancelCall(playbackThreadId, 0);
    }

    std::unique_lock lock(workerMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    if (playbackThread_.joinable()) {
        // FFmpeg's interrupt callback handles cooperative cancellation. This
        // additionally wakes synchronous Windows I/O issued by that thread.
        CancelSynchronousIo(playbackThread_.native_handle());
    }
}

void WasapiAudioPlayer::Stop() {
    RequestStop();

    std::scoped_lock lock(workerMutex_);
    if (playbackThread_.joinable()) {
        // Repeat cancellation immediately before joining so a new blocking I/O
        // operation cannot slip between the first request and the join.
        CancelSynchronousIo(playbackThread_.native_handle());
    }
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    running_.store(false);
    runtimeState_.store(WasapiRuntimeState::Stopped);
    packetInputMode_.store(false);
    {
        std::scoped_lock packetLock(packetMutex_);
        ClearPacketQueueLocked();
    }
    if (packetCodecParameters_) {
        avcodec_parameters_free(&packetCodecParameters_);
    }
    SetPlaybackClockRunning(false);
}

void WasapiAudioPlayer::Pause(const std::chrono::milliseconds position) {
    if (!running_.load() && runtimeState_.load() != WasapiRuntimeState::Starting) {
        SetPlaybackClockRunning(false);
        return;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    paused_.store(true);
    pausePositionMs_.store(clamped.count());
    pendingSeekMs_.store(-1);
    ResetPlaybackClock(clamped);
    SetPlaybackClockRunning(false);
}

bool WasapiAudioPlayer::Resume(const std::chrono::milliseconds position) {
    const bool pendingStart = !running_.load() && runtimeState_.load() == WasapiRuntimeState::Starting;
    if ((!running_.load() && !pendingStart) || stopping_.load()) {
        return false;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pausePositionMs_.store(clamped.count());
    if (pendingStart) {
        pendingSeekMs_.store(-1);
        ResetPlaybackClock(clamped);
        SetPlaybackClockRunning(false);
    } else {
        pendingSeekMs_.store(clamped.count());
    }
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
    paused_.store(true);
    pausePositionMs_.store(clamped.count());
    ResetPlaybackClock(clamped);
    SetPlaybackClockRunning(false);
    packetCv_.notify_all();
}

bool WasapiAudioPlayer::ResumePacketStream() {
    if (stopping_.load()) {
        return false;
    }
    if (!packetInputMode_.load()) {
        return Resume(std::chrono::milliseconds{std::max<int64_t>(0, pausePositionMs_.load())});
    }
    if (!running_.load() || stopping_.load()) {
        return false;
    }

    paused_.store(false);
    packetCv_.notify_all();
    return true;
}

bool WasapiAudioPlayer::Seek(const std::chrono::milliseconds position) {
    if (!running_.load() || stopping_.load()) {
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
    if (runtimeState_.load() == WasapiRuntimeState::Starting) {
        return std::chrono::milliseconds{std::max<int64_t>(0, pausePositionMs_.load())};
    }
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

void WasapiAudioPlayer::PlaybackLoop(const uint64_t workerGeneration) {
    playbackThreadId_.store(GetCurrentThreadId());
    CoInitScope com;
    if (!com.Ok()) {
        LogError(L"CoInitializeEx failed hr=0x" + HexHr(com.hr));
        SignalStart(false, workerGeneration);
        running_.store(false);
        playbackThreadId_.store(0);
        workerFinished_.store(true);
        return;
    }
    CoCallCancellationScope callCancellation;

    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swrCtx = nullptr;
    bool audioClientStarted = false;
    HANDLE bitstreamEvent = nullptr;
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
            SignalStart(false, workerGeneration);
        }
        SetPlaybackClockRunning(false);
        if (swrCtx) swr_free(&swrCtx);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codecCtx) avcodec_free_context(&codecCtx);
        if (formatCtx) avformat_close_input(&formatCtx);
        if (bitstreamEvent) {
            CloseHandle(bitstreamEvent);
            bitstreamEvent = nullptr;
        }
        if (outputFormat.bitstream && PassthroughReason() == L"active") {
            SetPassthroughRuntime(false,
                                  stopping_.load() ? L"stopped" : L"ended",
                                  PassthroughCodec());
        }
        running_.store(false);
    };

    const bool packetInput = packetInputMode_.load();

    auto releaseAudioOutput = [&]() {
        if (audioClientStarted && audioClient) {
            audioClient->Stop();
        }
        audioClientStarted = false;
        SetPlaybackClockRunning(false);
        renderClient.Reset();
        audioClient.Reset();
        outputFormat = {};
        submittedFrames = 0;
        if (bitstreamEvent) {
            CloseHandle(bitstreamEvent);
            bitstreamEvent = nullptr;
        }
    };

    auto preparePcmFallback = [&](std::wstring reason,
                                  const std::wstring& codec,
                                  const bool preservePlaybackPosition) {
        if (preservePlaybackPosition) {
            if (const auto position = PlaybackClock()) {
                startPosition_ = std::max(*position, std::chrono::milliseconds{0});
            }
        }
        releaseAudioOutput();
        ResetPlaybackClock(startPosition_);
        SetPassthroughRuntime(false, reason, codec);
        {
            std::scoped_lock lock(stateMutex_);
            lastStatus_ = L"wasapi shared pcm fallback initializing";
        }
        LogInfo(L"audio passthrough fallback=shared_pcm reason=" + reason +
                (codec.empty() ? L"" : L" codec=" + codec) +
                L" position=" + FormatTimecode(startPosition_));
    };

    do {
        if (stopping_.load()) {
            break;
        }
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
            if (stopping_.load()) {
                break;
            }
            error = avformat_find_stream_info(formatCtx, nullptr);
            if (error < 0) {
                LogError(L"avformat_find_stream_info failed: " + FfmpegErrorString(error));
                break;
            }
            if (stopping_.load()) {
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

        bool useBitstream = false;
        if (passthroughRequested_.load()) {
            const std::wstring codecName = PassthroughCodecName(inputCodecParameters);
            if (codecName.empty()) {
                preparePcmFallback(L"unsupported_codec", codecName, false);
            } else if (std::abs(playbackRate_.load() - 1.0) > 0.001) {
                preparePcmFallback(L"playback_rate_unsupported", codecName, false);
            } else {
                UINT32 bitstreamBufferFrames = 0;
                std::wstring failureReason;
                bitstreamEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!bitstreamEvent) {
                    preparePcmFallback(L"event_creation_failed", codecName, false);
                }
                if (bitstreamEvent &&
                    InitializeBitstreamWasapi(inputCodecParameters,
                                              audioClient,
                                              renderClient,
                                              outputFormat,
                                              bitstreamBufferFrames,
                                              bitstreamEvent,
                                              failureReason)) {
                    useBitstream = true;
                    SetPassthroughRuntime(true, L"active", codecName);
                    LogInfo(L"audio passthrough active codec=" + codecName +
                            L" carrier=" + std::to_wstring(outputFormat.sampleRate) + L"Hz/" +
                            std::to_wstring(outputFormat.channels) + L"ch");
                } else if (bitstreamEvent) {
                    preparePcmFallback(failureReason.empty() ? L"initialization_failed" : failureReason,
                                       codecName,
                                       false);
                }
            }
        } else {
            SetPassthroughRuntime(false, L"disabled");
        }

        if (useBitstream) {
            SpdifPacketizer packetizer;
            std::wstring packetizerError;
            if (!packetizer.Open(inputCodecParameters, audioTimeBase, packetizerError)) {
                LogError(packetizerError);
                preparePcmFallback(L"packetizer_initialization_failed",
                                   PassthroughCodecName(inputCodecParameters),
                                   false);
            } else {
                bool fallbackToPcm = false;
                std::wstring fallbackReason;
                if (!packetInput) {
                    packet = av_packet_alloc();
                    if (!packet) {
                        LogError(L"bitstream packet allocation failed");
                        fallbackToPcm = true;
                        fallbackReason = L"packet_allocation_failed";
                    }
                    const bool skipNetworkNearStartSeek = networkSource && startPosition_ < std::chrono::seconds{1};
                    if (!fallbackToPcm && startPosition_.count() > 0 && !skipNetworkNearStartSeek) {
                        const int64_t target = static_cast<int64_t>(startPosition_.count()) * AV_TIME_BASE / 1000;
                        const int seekError = av_seek_frame(formatCtx, -1, target, AVSEEK_FLAG_BACKWARD);
                        if (seekError < 0) LogInfo(L"bitstream initial seek failed: " + FfmpegErrorString(seekError));
                    }
                }
                {
                    std::scoped_lock lock(stateMutex_);
                    lastStatus_ = outputFormat.description;
                }
                SetPlaybackClockRunning(false);
                std::vector<uint8_t> pendingBitstream;
                std::size_t pendingBitstreamOffset = 0;
                bool startSignaled = false;

                while (!fallbackToPcm && !stopping_.load()) {
                    if (!HandlePause(audioClient.Get(), outputFormat, submittedFrames, audioClientStarted)) break;

                    if (const auto target = TakePendingSeek()) {
                        startPosition_ = *target;
                        if (audioClientStarted) {
                            audioClient->Stop();
                            audioClient->Reset();
                            audioClientStarted = false;
                        }
                        submittedFrames = 0;
                        pendingBitstream.clear();
                        pendingBitstreamOffset = 0;
                        if (packetInput) {
                            std::scoped_lock lock(packetMutex_);
                            ClearPacketQueueLocked();
                            packetStreamEof_.store(false);
                            packetCv_.notify_all();
                        } else {
                            const int64_t seekTarget = static_cast<int64_t>(target->count()) * AV_TIME_BASE / 1000;
                            int seekError = av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
                            if (seekError < 0) {
                                seekError = avformat_seek_file(formatCtx, -1, INT64_MIN, seekTarget, INT64_MAX, 0);
                            }
                            if (seekError < 0) {
                                LogError(L"bitstream seek failed: " + FfmpegErrorString(seekError));
                                fallbackToPcm = true;
                                fallbackReason = L"bitstream_seek_failed";
                                break;
                            }
                            avformat_flush(formatCtx);
                        }
                        if (!packetizer.Open(inputCodecParameters, audioTimeBase, packetizerError)) {
                            LogError(L"bitstream packetizer reset failed: " + packetizerError);
                            fallbackToPcm = true;
                            fallbackReason = L"packetizer_failed";
                            break;
                        }
                        ResetPlaybackClock(*target);
                        SetPlaybackClockRunning(false);
                    }

                    AVPacket* sourcePacket = nullptr;
                    bool ownedPacket = false;
                    if (packetInput) {
                        sourcePacket = TakeQueuedPacket();
                        ownedPacket = true;
                        if (!sourcePacket) {
                            if (packetStreamEof_.load()) {
                                if (!WriteBitstream(renderClient.Get(),
                                                    audioClient.Get(),
                                                    outputFormat,
                                                    bitstreamEvent,
                                                    pendingBitstream,
                                                    pendingBitstreamOffset,
                                                    nullptr,
                                                    0,
                                                    true,
                                                    submittedFrames,
                                                    audioClientStarted)) {
                                    fallbackToPcm = true;
                                    fallbackReason = L"runtime_write_failed";
                                }
                                if (audioClientStarted && !startSignaled) {
                                    SignalStart(true, workerGeneration);
                                    startSignaled = true;
                                }
                                break;
                            }
                            continue;
                        }
                    } else {
                        const int readResult = av_read_frame(formatCtx, packet);
                        if (readResult < 0) {
                            if (!WriteBitstream(renderClient.Get(),
                                                audioClient.Get(),
                                                outputFormat,
                                                bitstreamEvent,
                                                pendingBitstream,
                                                pendingBitstreamOffset,
                                                nullptr,
                                                0,
                                                true,
                                                submittedFrames,
                                                audioClientStarted)) {
                                fallbackToPcm = true;
                                fallbackReason = L"runtime_write_failed";
                            }
                            if (audioClientStarted && !startSignaled) {
                                SignalStart(true, workerGeneration);
                                startSignaled = true;
                            }
                            break;
                        }
                        if (packet->stream_index != audioStreamIndex) {
                            av_packet_unref(packet);
                            continue;
                        }
                        sourcePacket = packet;
                    }

                    std::vector<uint8_t> burst;
                    const bool packetized = packetizer.Packetize(sourcePacket, burst, packetizerError);
                    if (ownedPacket) av_packet_free(&sourcePacket);
                    else av_packet_unref(packet);
                    if (!packetized) {
                        LogError(packetizerError);
                        fallbackToPcm = true;
                        fallbackReason = L"packetizer_failed";
                        break;
                    }
                    if (burst.empty()) continue;
                    if (burst.size() % outputFormat.blockAlign != 0) {
                        LogError(L"IEC 61937 burst is not carrier-frame aligned bytes=" +
                                 std::to_wstring(burst.size()));
                        fallbackToPcm = true;
                        fallbackReason = L"unaligned_iec61937_burst";
                        break;
                    }
                    if (!WriteBitstream(renderClient.Get(),
                                        audioClient.Get(),
                                        outputFormat,
                                        bitstreamEvent,
                                        pendingBitstream,
                                        pendingBitstreamOffset,
                                        burst.data(),
                                        burst.size(),
                                        false,
                                        submittedFrames,
                                        audioClientStarted)) {
                        fallbackToPcm = true;
                        fallbackReason = L"runtime_write_failed";
                        break;
                    }
                    if (audioClientStarted && !startSignaled) {
                        SignalStart(true, workerGeneration);
                        startSignaled = true;
                    }
                }
                if (!fallbackToPcm) {
                    break;
                }
                if (packet) {
                    av_packet_free(&packet);
                }
                preparePcmFallback(fallbackReason.empty() ? L"runtime_failed" : fallbackReason,
                                   PassthroughCodecName(inputCodecParameters),
                                   audioClientStarted);
            }
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
        if (stopping_.load()) {
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
        SignalStart(true, workerGeneration);

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
    playbackThreadId_.store(0);
    if (workerGeneration == startGeneration_.load()) {
        const auto finalState = runtimeState_.load();
        if (stopping_.load()) {
            runtimeState_.store(WasapiRuntimeState::Stopped);
        } else if (finalState == WasapiRuntimeState::Starting) {
            runtimeState_.store(WasapiRuntimeState::Failed);
        } else if (finalState == WasapiRuntimeState::Ready) {
            runtimeState_.store(WasapiRuntimeState::Ended);
        }
    }
    workerFinished_.store(true);
}

std::wstring WasapiAudioPlayer::PassthroughReason() const {
    std::scoped_lock lock(stateMutex_);
    return passthroughReason_;
}

std::wstring WasapiAudioPlayer::PassthroughCodec() const {
    std::scoped_lock lock(stateMutex_);
    return passthroughCodec_;
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
    if (outputFormat.bitstream) {
        // Exclusive event-driven output must be primed with one complete
        // endpoint buffer before Start. WriteBitstream performs that priming.
        return true;
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
    (void)outputFormat;
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

bool WasapiAudioPlayer::InitializeBitstreamWasapi(
    const AVCodecParameters* codecParameters,
    Microsoft::WRL::ComPtr<IAudioClient>& audioClient,
    Microsoft::WRL::ComPtr<IAudioRenderClient>& renderClient,
    WasapiFormat& outputFormat,
    UINT32& bufferFrameCount,
    HANDLE eventHandle,
    std::wstring& failureReason) const {
    const std::wstring codecName = PassthroughCodecName(codecParameters);
    if (codecName.empty()) {
        failureReason = L"unsupported_codec";
        return false;
    }

    const bool highBitRate = codecParameters->codec_id == AV_CODEC_ID_TRUEHD || codecName == L"DTS-HD";
    const UINT32 encodedRate = codecParameters->sample_rate > 0
                                   ? static_cast<UINT32>(codecParameters->sample_rate)
                                   : 48000u;
    const UINT32 encodedChannels = codecParameters->ch_layout.nb_channels > 0
                                       ? static_cast<UINT32>(codecParameters->ch_layout.nb_channels)
                                       : 6u;
    UINT32 carrierRate = encodedRate;
    UINT32 carrierChannels = 2;
    GUID subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL;
    if (codecParameters->codec_id == AV_CODEC_ID_EAC3) {
        carrierRate = encodedRate % 44100u == 0 ? 176400u : 192000u;
        subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS;
    } else if (codecParameters->codec_id == AV_CODEC_ID_TRUEHD) {
        carrierRate = encodedRate % 44100u == 0 ? 176400u : 192000u;
        carrierChannels = 8;
        subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP;
    } else if (codecParameters->codec_id == AV_CODEC_ID_DTS) {
        if (highBitRate) {
            carrierRate = encodedRate % 44100u == 0 ? 176400u : 192000u;
            carrierChannels = 8;
            subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DTS_HD;
        } else {
            subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DTS;
        }
    }

    WAVEFORMATEXTENSIBLE_IEC61937 format{};
    format.FormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.FormatExt.Format.nChannels = static_cast<WORD>(carrierChannels);
    format.FormatExt.Format.nSamplesPerSec = carrierRate;
    format.FormatExt.Format.wBitsPerSample = 16;
    format.FormatExt.Format.nBlockAlign = static_cast<WORD>(carrierChannels * sizeof(int16_t));
    format.FormatExt.Format.nAvgBytesPerSec = carrierRate * format.FormatExt.Format.nBlockAlign;
    // The base WAVEFORMATEXTENSIBLE size is the most broadly compatible form
    // used by mature WASAPI sinks. The IEC fields remain populated for drivers
    // that inspect the extended structure.
    format.FormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.FormatExt.Samples.wValidBitsPerSample = 16;
    format.FormatExt.dwChannelMask = carrierChannels == 8
                                         ? KSAUDIO_SPEAKER_7POINT1_SURROUND
                                         : KSAUDIO_SPEAKER_STEREO;
    format.FormatExt.SubFormat = subFormat;
    format.dwEncodedSamplesPerSec = encodedRate;
    format.dwEncodedChannelCount = encodedChannels;
    format.dwAverageBytesPerSec = codecParameters->bit_rate > 0
                                      ? static_cast<DWORD>(codecParameters->bit_rate / 8)
                                      : 0;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        failureReason = L"endpoint_enumerator_failed";
        return false;
    }
    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        failureReason = L"endpoint_unavailable";
        return false;
    }
    const auto activateClient = [&]() -> HRESULT {
        audioClient.Reset();
        return device->Activate(__uuidof(IAudioClient),
                                CLSCTX_ALL,
                                nullptr,
                                reinterpret_cast<void**>(audioClient.GetAddressOf()));
    };
    hr = activateClient();
    if (FAILED(hr)) {
        failureReason = L"endpoint_activation_failed";
        return false;
    }
    hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        &format.FormatExt.Format,
                                        nullptr);
    if (hr != S_OK) {
        failureReason = L"endpoint_format_unsupported";
        LogInfo(L"bitstream format unsupported codec=" + codecName + L" hr=0x" + HexHr(hr));
        audioClient.Reset();
        return false;
    }

    REFERENCE_TIME defaultPeriod = 0;
    hr = audioClient->GetDevicePeriod(&defaultPeriod, nullptr);
    if (FAILED(hr)) {
        failureReason = L"endpoint_period_failed";
        audioClient.Reset();
        return false;
    }
    if (defaultPeriod <= 0) {
        failureReason = L"endpoint_period_invalid";
        audioClient.Reset();
        return false;
    }
    constexpr REFERENCE_TIME kPassthroughTargetPeriod = 500000;  // 50 ms.
    const REFERENCE_TIME periodMultiplier =
        std::max<REFERENCE_TIME>(1, (kPassthroughTargetPeriod + defaultPeriod - 1) / defaultPeriod);
    REFERENCE_TIME period = periodMultiplier * defaultPeriod;
    bool reactivate = false;
    do {
        if (reactivate) {
            hr = activateClient();
            if (FAILED(hr)) break;
            reactivate = false;
        }
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                     period,
                                     period,
                                     &format.FormatExt.Format,
                                     nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 alignedFrames = 0;
            if (FAILED(audioClient->GetBufferSize(&alignedFrames)) || alignedFrames == 0) break;
            period = static_cast<REFERENCE_TIME>(
                (10000000.0 * static_cast<double>(alignedFrames) / static_cast<double>(carrierRate)) + 0.5);
            reactivate = true;
            continue;
        }
        if ((hr == AUDCLNT_E_BUFFER_SIZE_ERROR ||
             hr == AUDCLNT_E_INVALID_DEVICE_PERIOD ||
             hr == E_OUTOFMEMORY) &&
            period > defaultPeriod) {
            period -= defaultPeriod;
            continue;
        }
        break;
    } while (period >= defaultPeriod);
    if (FAILED(hr)) {
        failureReason = hr == AUDCLNT_E_DEVICE_IN_USE ? L"exclusive_mode_unavailable" : L"exclusive_initialize_failed";
        LogError(L"bitstream initialize failed codec=" + codecName + L" hr=0x" + HexHr(hr));
        audioClient.Reset();
        return false;
    }
    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        failureReason = L"exclusive_buffer_failed";
        audioClient.Reset();
        return false;
    }
    hr = audioClient->SetEventHandle(eventHandle);
    if (FAILED(hr)) {
        failureReason = L"exclusive_event_handle_failed";
        audioClient.Reset();
        return false;
    }
    hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr)) {
        failureReason = L"exclusive_render_client_failed";
        audioClient.Reset();
        return false;
    }

    outputFormat.sampleRate = carrierRate;
    outputFormat.channels = carrierChannels;
    outputFormat.blockAlign = format.FormatExt.Format.nBlockAlign;
    outputFormat.bufferFrameCount = bufferFrameCount;
    outputFormat.sampleFormat = AV_SAMPLE_FMT_NONE;
    outputFormat.bitstream = true;
    outputFormat.description = L"wasapi exclusive bitstream " + codecName;
    LogInfo(L"bitstream endpoint initialized codec=" + codecName +
            L" buffer_frames=" + std::to_wstring(bufferFrameCount) +
            L" frame_bytes=" + std::to_wstring(outputFormat.blockAlign) +
            L" period_ms=" + std::to_wstring(static_cast<double>(period) / 10000.0));
    failureReason = L"active";
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

bool WasapiAudioPlayer::WriteBitstream(IAudioRenderClient* renderClient,
                                       IAudioClient* audioClient,
                                       const WasapiFormat& outputFormat,
                                       HANDLE eventHandle,
                                       std::vector<uint8_t>& pending,
                                       std::size_t& pendingOffset,
                                       const uint8_t* data,
                                       const std::size_t bytes,
                                       const bool flush,
                                       uint64_t& submittedFrames,
                                       bool& audioClientStarted) {
    if (!renderClient || !audioClient || !eventHandle ||
        outputFormat.bufferFrameCount == 0 || outputFormat.blockAlign == 0) {
        LogError(L"bitstream writer is not initialized");
        return false;
    }
    if (data && bytes > 0) pending.insert(pending.end(), data, data + bytes);

    const std::size_t bufferBytes = static_cast<std::size_t>(outputFormat.bufferFrameCount) *
                                    static_cast<std::size_t>(outputFormat.blockAlign);
    while (!stopping_.load()) {
        std::size_t availableBytes = pending.size() - pendingOffset;
        if (availableBytes < bufferBytes) {
            if (!flush || availableBytes == 0) break;
            pending.resize(pendingOffset + bufferBytes, 0);
            availableBytes = bufferBytes;
        }

        if (audioClientStarted) {
            DWORD waitedMs = 0;
            while (!stopping_.load() && !paused_.load() && !HasPendingSeek()) {
                const DWORD waitResult = WaitForSingleObject(eventHandle, 20);
                if (waitResult == WAIT_OBJECT_0) break;
                if (waitResult == WAIT_FAILED) {
                    LogError(L"bitstream endpoint event wait failed win32=" +
                             std::to_wstring(GetLastError()));
                    return false;
                }
                waitedMs += 20;
                if (waitedMs >= 1100) {
                    LogError(L"bitstream endpoint buffer timed out");
                    return false;
                }
            }
            if (stopping_.load()) return false;
            if (paused_.load() || HasPendingSeek()) return true;
        }

        BYTE* buffer = nullptr;
        HRESULT hr = renderClient->GetBuffer(outputFormat.bufferFrameCount, &buffer);
        if (FAILED(hr)) {
            LogError(L"bitstream GetBuffer failed hr=0x" + HexHr(hr) +
                     L" requested_frames=" + std::to_wstring(outputFormat.bufferFrameCount) +
                     L" pending_bytes=" + std::to_wstring(availableBytes) +
                     L" frame_bytes=" + std::to_wstring(outputFormat.blockAlign));
            return false;
        }
        std::memcpy(buffer, pending.data() + pendingOffset, bufferBytes);
        hr = renderClient->ReleaseBuffer(outputFormat.bufferFrameCount, 0);
        if (FAILED(hr)) {
            LogError(L"bitstream ReleaseBuffer failed hr=0x" + HexHr(hr) +
                     L" frames=" + std::to_wstring(outputFormat.bufferFrameCount));
            return false;
        }

        pendingOffset += bufferBytes;
        submittedFrames += outputFormat.bufferFrameCount;
        if (!audioClientStarted) {
            hr = audioClient->Start();
            if (FAILED(hr)) {
                LogError(L"bitstream IAudioClient::Start failed hr=0x" + HexHr(hr));
                return false;
            }
            audioClientStarted = true;
            SetPlaybackClockRunning(true);
        }
        UpdatePlaybackClock(outputFormat, submittedFrames, outputFormat.bufferFrameCount);

        if (pendingOffset == pending.size()) {
            pending.clear();
            pendingOffset = 0;
        } else if (pendingOffset >= 1024 * 1024) {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pendingOffset));
            pendingOffset = 0;
        }
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

void WasapiAudioPlayer::SignalStart(const bool success,
                                    const uint64_t workerGeneration) {
    std::scoped_lock lock(stateMutex_);
    if (workerGeneration != startGeneration_.load()) {
        return;
    }
    const bool acceptedSuccess = success &&
                                 !stopping_.load() &&
                                 runtimeState_.load() == WasapiRuntimeState::Starting;
    if (!startResolved_) {
        startSucceeded_ = acceptedSuccess;
        startResolved_ = true;
        startCv_.notify_all();
    }
    if (acceptedSuccess) {
        runtimeState_.store(WasapiRuntimeState::Ready);
    } else if (!stopping_.load()) {
        runtimeState_.store(WasapiRuntimeState::Failed);
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

void WasapiAudioPlayer::SetPassthroughRuntime(const bool active,
                                              std::wstring reason,
                                              std::wstring codec) {
    passthroughActive_.store(active);
    std::scoped_lock lock(stateMutex_);
    passthroughReason_ = std::move(reason);
    passthroughCodec_ = std::move(codec);
}

}  // namespace anvil::app
