#include "AnvilPlayer/App/wasapi_audio_player.h"

#include "AnvilPlayer/App/string_util.h"

#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace anvil::app {

using anvil::playback::FormatTimecode;
using anvil::playback::LogLevel;

namespace {

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

}  // namespace

WasapiAudioPlayer::~WasapiAudioPlayer() {
    Stop();
}

void WasapiAudioPlayer::SetLogSink(LogSinkPtr logSink) {
    logSink_ = std::move(logSink);
}

bool WasapiAudioPlayer::Start(const std::filesystem::path& mediaPath, const std::chrono::milliseconds startPosition, const double volume) {
    Stop();
    if (mediaPath.empty()) {
        return false;
    }

    path_ = mediaPath;
    startPosition_ = startPosition;
    volume_.store(std::clamp(volume, 0.0, 1.0));
    pendingSeekMs_.store(-1);
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

void WasapiAudioPlayer::Stop() {
    stopping_.store(true);
    pendingSeekMs_.store(-1);
    SignalStart(false);
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    running_.store(false);
    SetPlaybackClockRunning(false);
}

bool WasapiAudioPlayer::Seek(const std::chrono::milliseconds position) {
    if (!running_.load()) {
        return false;
    }

    const auto clamped = std::max(position, std::chrono::milliseconds{0});
    pendingSeekMs_.store(clamped.count());
    ResetPlaybackClock(clamped);
    return true;
}

void WasapiAudioPlayer::SetVolume(const double volume) {
    volume_.store(std::clamp(volume, 0.0, 1.0));
}

int WasapiAudioPlayer::InterruptCallback(void* opaque) {
    const auto* player = static_cast<const WasapiAudioPlayer*>(opaque);
    return player && (player->stopping_.load() || player->HasPendingSeek()) ? 1 : 0;
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

    do {
        const std::string pathUtf8 = WideToUtf8(path_.wstring());
        formatCtx = avformat_alloc_context();
        if (!formatCtx) {
            LogError(L"avformat_alloc_context failed");
            break;
        }
        formatCtx->interrupt_callback.callback = &WasapiAudioPlayer::InterruptCallback;
        formatCtx->interrupt_callback.opaque = this;
        int error = avformat_open_input(&formatCtx, pathUtf8.c_str(), nullptr, nullptr);
        if (error < 0) {
            LogError(L"avformat_open_input failed: " + FfmpegErrorString(error));
            break;
        }
        error = avformat_find_stream_info(formatCtx, nullptr);
        if (error < 0) {
            LogError(L"avformat_find_stream_info failed: " + FfmpegErrorString(error));
            break;
        }
        audioStreamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audioStreamIndex < 0) {
            LogError(L"no audio stream found");
            break;
        }

        const AVStream* audioStream = formatCtx->streams[audioStreamIndex];
        audioTimeBase = audioStream->time_base;
        const AVCodec* codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
        if (!codec) {
            LogError(L"avcodec_find_decoder failed");
            break;
        }
        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            LogError(L"avcodec_alloc_context3 failed");
            break;
        }
        error = avcodec_parameters_to_context(codecCtx, audioStream->codecpar);
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

        if (startPosition_.count() > 0) {
            const int64_t target = static_cast<int64_t>(startPosition_.count()) * AV_TIME_BASE / 1000;
            LogInfo(L"seek target=" + FormatTimecode(startPosition_));
            const int seekError = av_seek_frame(formatCtx, -1, target, AVSEEK_FLAG_BACKWARD);
            if (seekError < 0) {
                LogError(L"av_seek_frame failed: " + FfmpegErrorString(seekError));
            }
            avcodec_flush_buffers(codecCtx);
        }

        HRESULT hr = audioClient->Start();
        if (FAILED(hr)) {
            LogError(L"IAudioClient::Start failed hr=0x" + HexHr(hr));
            break;
        }
        audioClientStarted = true;
        UpdatePlaybackClock(outputFormat, submittedFrames, 0);
        SetPlaybackClockRunning(true);
        {
            std::scoped_lock lock(stateMutex_);
            lastStatus_ = outputFormat.description;
        }
        SignalStart(true);

        while (!stopping_.load()) {
            if (!ApplyPendingSeek(formatCtx,
                                  codecCtx,
                                  swrCtx,
                                  resamplerState,
                                  audioClient.Get(),
                                  outputFormat,
                                  submittedFrames,
                                  audioClientStarted)) {
                break;
            }

            const int readResult = av_read_frame(formatCtx, packet);
            if (readResult < 0) {
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
                DrainDecoder(codecCtx, swrCtx, frame, audioTimeBase, resamplerState, outputFormat, renderClient.Get(), audioClient.Get(), submittedFrames);
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

            if (!ReceiveFrames(codecCtx, swrCtx, frame, audioTimeBase, resamplerState, outputFormat, renderClient.Get(), audioClient.Get(), submittedFrames)) {
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
    int seekError = av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (seekError < 0) {
        seekError = avformat_seek_file(formatCtx, -1, INT64_MIN, seekTarget, INT64_MAX, 0);
    }
    if (seekError < 0) {
        LogError(L"runtime seek failed: " + FfmpegErrorString(seekError));
        const HRESULT restartHr = audioClient->Start();
        if (SUCCEEDED(restartHr)) {
            audioClientStarted = true;
            SetPlaybackClockRunning(true);
        } else {
            LogError(L"IAudioClient::Start after failed seek failed hr=0x" + HexHr(restartHr));
        }
        return true;
    }

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
                                      uint64_t& submittedFrames) {
    while (!stopping_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            LogError(L"avcodec_receive_frame failed: " + FfmpegErrorString(ret));
            return false;
        }
        if (!RenderFrame(frame, timeBase, swrCtx, resamplerState, outputFormat, renderClient, audioClient, submittedFrames)) {
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
                                     uint64_t& submittedFrames) {
    while (!stopping_.load() && !HasPendingSeek()) {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        if (ret < 0) {
            return;
        }
        if (!RenderFrame(frame, timeBase, swrCtx, resamplerState, outputFormat, renderClient, audioClient, submittedFrames)) {
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
                                    uint64_t& submittedFrames) {
    if (!frame || frame->nb_samples <= 0 || !renderClient || !audioClient) {
        return true;
    }
    if (HasPendingSeek()) {
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
    return WritePcm(renderClient, audioClient, outputFormat, pcm.data(), static_cast<UINT32>(convertedSamples), submittedFrames);
}

bool WasapiAudioPlayer::WritePcm(IAudioRenderClient* renderClient,
                                 IAudioClient* audioClient,
                                 const WasapiFormat& outputFormat,
                                 const uint8_t* data,
                                 UINT32 frames,
                                 uint64_t& submittedFrames) {
    UINT32 offsetFrames = 0;
    while (offsetFrames < frames && !stopping_.load() && !HasPendingSeek()) {
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
        UpdatePlaybackClock(outputFormat, submittedFrames, padding);

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
