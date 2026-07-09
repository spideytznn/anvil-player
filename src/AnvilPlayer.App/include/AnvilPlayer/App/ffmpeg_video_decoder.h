#pragma once

#include "AnvilPlayer/App/log_sink_ptr.h"
#include "AnvilPlayer/Playback/DolbyVisionMetadata.h"
#include "AnvilPlayer/Playback/Settings.h"
#include "AnvilPlayer/Playback/Types.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace anvil::app {

struct DoviLibplaceboFilterState;
class LibassSubtitleRenderer;

struct NativeSubtitleBitmap {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    int stride = 0;  // bytes per row, premultiplied BGRA
    uint64_t serial = 0;
    std::shared_ptr<const std::vector<uint8_t>> bgra;

    bool HasPixels() const {
        return bgra && !bgra->empty() && width > 0 && height > 0 && stride >= width * 4;
    }
};

// Raw 10-bit YUV 4:2:0 planar frame data (yuv420p10le layout) carried from the
// software decoder to the renderer without RGB conversion. Used for Dolby Vision
// streams, where the renderer must reshape the original IPT-encoded YUV on the
// GPU (swscale's YUV->RGB conversion would destroy the IPT structure).
struct NativeYuvPlanes {
    int width = 0;
    int height = 0;
    int yStride = 0;       // bytes per luma row
    int uvStride = 0;      // bytes per chroma row (half width)
    int bitDepth = 10;     // 8 or 10
    // Single packed buffer holding Y plane then UV plane (UV interleaved for
    // NV12/P010 upload). Layout: Y (yStride*height), then UV (uvStride*height/2).
    std::shared_ptr<const std::vector<uint8_t>> data;

    bool HasData() const {
        return data && !data->empty() && width > 0 && height > 0 && yStride > 0;
    }
};

// A decoded video frame: either a CPU-side BGRA buffer or a zero-copy D3D11
// hardware texture (D3D11VA), or raw YUV planes (DV software path), or both.
// The unit consumed by D3D11VideoRenderer.
struct NativeVideoFrame {
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row, == width * 4 for tight BGRA
    std::shared_ptr<const std::vector<uint8_t>> bgra;
    // Raw 10-bit YUV for the DV software path (reshaped on the GPU).
    // Mutually exclusive with bgra in the DV path.
    NativeYuvPlanes yuv;
    // Experimental Dolby Vision Profile 7 enhancement/FEL layer, packed as a
    // second P010 texture and sampled by the renderer as an overlay.
    NativeYuvPlanes enhancementYuv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTexture;
    UINT d3dArraySlice = 0;
    DXGI_FORMAT d3dFormat = DXGI_FORMAT_UNKNOWN;
    AVPixelFormat softwareFormat = AV_PIX_FMT_NONE;
    anvil::playback::VideoColorMetadata color;
    // Per-frame Dolby Vision metadata (reshaping curves + color matrices).
    // nullptr for non-DV streams. When present, the renderer must apply the
    // IPTPQc2 reshaping before treating the pixels as BT.2020 PQ.
    std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> dovi;
    std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> enhancementDovi;
    // Non-empty when dynamic metadata has already been consumed before the
    // renderer sees the frame, e.g. Dolby Vision processed by libplacebo.
    std::wstring dynamicMetadataPath;
    std::wstring dynamicMetadataDetails;
    std::wstring enhancementMetadataDetails;
    std::wstring subtitleText;
    std::vector<NativeSubtitleBitmap> subtitleBitmaps;
    bool subtitlesPrepared = false;
    std::shared_ptr<AVFrame> hardwareFrameRef;
    std::chrono::milliseconds pts{0};
    uint64_t serial = 0;
    uint64_t timelineSerial = 0;

    bool HasPixels() const {
        return bgra && !bgra->empty() && width > 0 && height > 0 && stride > 0;
    }

    bool HasYuv() const {
        return yuv.HasData();
    }

    bool HasEnhancementYuv() const {
        return enhancementYuv.HasData();
    }

    bool HasD3DTexture() const {
        return d3dTexture && width > 0 && height > 0 && d3dFormat != DXGI_FORMAT_UNKNOWN;
    }

    bool HasContent() const {
        return HasD3DTexture() || HasPixels() || HasYuv();
    }
};

// Queue/scheduler telemetry surfaced to the inspector and logs.
struct NativeVideoQueueStats {
    std::size_t queueDepth = 0;
    std::size_t packetQueueDepth = 0;
    std::size_t packetQueueBytes = 0;
    std::chrono::milliseconds bufferedEnd{0};
    std::chrono::milliseconds bufferedDuration{0};
    std::chrono::milliseconds readAheadEnd{0};
    std::chrono::milliseconds readAheadDuration{0};
    uint64_t rendered = 0;
    uint64_t droppedLate = 0;
    uint64_t droppedStale = 0;
    uint64_t droppedSuperseded = 0;
    uint64_t droppedQueueFull = 0;
    uint64_t hardwareFrames = 0;
    uint64_t zeroCopyFrames = 0;
    uint64_t cpuTransferFrames = 0;
    std::chrono::milliseconds clockPosition{0};
    int driftMs = 0;
    int frameCadenceMs = 0;
    int earlyToleranceMs = 0;
    bool buffering = false;
    bool seekRecoveryActive = false;
    bool seekRecoveryAudioHandoffReady = true;
    uint64_t timelineSerial = 0;
    uint64_t networkBytesPerSecond = 0;
    bool usingAudioClock = false;
    bool usingHardwareDecode = false;
    std::wstring decoder = L"ffmpeg_software";
    std::wstring fallbackReason;
};

struct NativeAudioPacketSink {
    int selectedTrackIndex = anvil::playback::kAudioTrackOff;
    std::function<bool(const AVCodecParameters*, AVRational, std::chrono::milliseconds, int)> start;
    std::function<bool(const AVPacket*)> pushPacket;
    std::function<void(std::chrono::milliseconds)> reset;
    std::function<void()> endOfStream;

    bool Enabled() const {
        return selectedTrackIndex != anvil::playback::kAudioTrackOff &&
               static_cast<bool>(start) &&
               static_cast<bool>(pushPacket);
    }
};

struct NativeDecodeFailure {
    std::filesystem::path path;
    std::wstring message;
};

// Native in-process FFmpeg video decoder. Demux+decode on a worker thread,
// optional D3D11VA hardware decode (zero-copy via a shared device, with
// CPU-transfer fallback), swscale -> BGRA for software frames, frame queue
// with PTS-based scheduling against an audio clock (or wall-clock fallback).
// Posts kNativeVideoFrameReadyMessage to the notification window when a frame
// is ready to render.
class FfmpegVideoDecoder {
public:
    using ClockCallback = std::function<std::optional<std::chrono::milliseconds>()>;

    explicit FfmpegVideoDecoder(LogSinkPtr logSink = nullptr);
    ~FfmpegVideoDecoder();
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    bool Start(const std::filesystem::path& mediaPath,
               std::chrono::milliseconds startPosition,
               HWND notificationWindow,
               UINT notificationMessage,
               UINT failureMessage = 0,
               ClockCallback clockCallback = {},
               bool preferHardwareDecode = false,
               ID3D11Device* sharedD3DDevice = nullptr,
               int selectedVideoTrackIndex = anvil::playback::kVideoTrackAuto,
               std::wstring preferredSubtitleLanguage = L"Auto",
               int selectedSubtitleTrackIndex = anvil::playback::kSubtitleTrackAuto,
               std::chrono::milliseconds subtitleDelay = std::chrono::milliseconds{0},
               bool autoLoadExternalSubtitles = true,
               std::filesystem::path externalSubtitlePath = {},
               bool oneShotFrame = false,
               bool preferDolbyVisionHdrOutput = false,
               bool enableDolbyVisionEnhancementDecode = false,
               NativeAudioPacketSink audioPacketSink = {});

    void Stop();
    bool Seek(std::chrono::milliseconds position);
    void SetPaused(bool paused, std::chrono::milliseconds position);

    bool IsRunning() const {
        return running_.load();
    }

    bool DolbyVisionEnhancementDecodeEnabled() const {
        return enableDolbyVisionEnhancementDecode_;
    }

    bool OneShotFrame() const {
        return oneShotFrame_;
    }

    bool LatestFrame(NativeVideoFrame& frame) const;

    void ClearFrame();

    void AcknowledgeFrameNotification();

    NativeVideoQueueStats Stats() const;
    bool WaitForPreroll(std::chrono::milliseconds targetDuration, std::chrono::milliseconds timeout) const;
    bool WaitForEnhancementPreroll(std::chrono::milliseconds minPts,
                                    std::chrono::milliseconds timeout,
                                    bool publishReadyFrame = true);

    const std::filesystem::path& Path() const { return path_; }

private:
    // Queue depth is selected per frame type: hardware texture refs are cheap,
    // while CPU BGRA/YUV frames can be tens of MB each for 4K+ sources.
    static constexpr std::size_t kMaxHardwareQueuedFrames = 6;
    static constexpr std::size_t kMaxYuvQueuedFrames = 10;
    static constexpr std::size_t kMaxSmallBgraQueuedFrames = 12;
    static constexpr std::size_t kMaxLargeBgraQueuedFrames = 6;
    static constexpr std::size_t kMaxCpuQueuedFrameBytes = 256ull * 1024ull * 1024ull;
    static constexpr std::size_t kMinPacketReadAheadBytes = 128ull * 1024ull * 1024ull;
    static constexpr std::size_t kMaxPacketReadAheadBytes = 1024ull * 1024ull * 1024ull;
    static constexpr std::size_t kMaxReusableBgraBuffers = 12;
    static constexpr int kStartupPacketReadAheadBatch = 4;
    static constexpr int kPlayingPacketReadAheadBatch = 4;
    static constexpr int kPausedPacketReadAheadBatch = 256;
    static constexpr std::chrono::milliseconds kPlayingPacketReadAheadTarget{15000};
    static constexpr int kDolbyVisionEnhancementStartupPacketReadAheadBatch = 6;
    static constexpr int kDolbyVisionEnhancementPlayingPacketReadAheadBatch = 4;
    static constexpr int kDolbyVisionEnhancementPausedPacketReadAheadBatch = 24;
    static constexpr std::chrono::milliseconds kDolbyVisionEnhancementPacketReadAheadTarget{1500};
    static constexpr std::size_t kMaxDolbyVisionEnhancementQueuedFrames = 160;
    static constexpr std::size_t kMaxDolbyVisionEnhancementQueuedBytes = 768ull * 1024ull * 1024ull;
    static constexpr std::chrono::milliseconds kDolbyVisionEnhancementMatchDelta{80};
    static constexpr std::chrono::milliseconds kMinFrameEarlyTolerance{2};
    static constexpr std::chrono::milliseconds kMaxFrameEarlyTolerance{6};
    static constexpr std::chrono::milliseconds kFrameLateDropThreshold{120};
    static constexpr std::chrono::milliseconds kMaxMeasuredFrameCadence{250};
    static constexpr int kSeekStartupPacketReadAheadBatch = 1;
    static constexpr int kSeekFastResumeFrameCount = 4;
    static constexpr int kSeekRecoveryWarmupFrameCount = 3;
    static constexpr std::size_t kSeekPrerollMinQueuedFrames = 4;
    static constexpr std::size_t kSeekPrerollSoftwareMinQueuedFrames = 9;
    static constexpr std::size_t kSeekPrerollMinPacketDepth = 24;
    static constexpr std::chrono::milliseconds kSeekPrerollMinReadAhead{1200};
    static constexpr std::chrono::milliseconds kSeekPrerollSoftwareMinReadAhead{15000};
    static constexpr std::chrono::milliseconds kSeekPrerollTimeout{1800};
    static constexpr std::chrono::milliseconds kSeekPrerollTimeoutMinReadAhead{900};
    static constexpr std::chrono::milliseconds kSeekPrerollSoftwareTimeout{10000};
    static constexpr std::chrono::milliseconds kSeekPrerollSoftwareTimeoutMinReadAhead{6000};
    static constexpr std::size_t kSeekPrerollEnhancementMinQueuedFrames = 2;
    static constexpr std::chrono::milliseconds kSeekPrerollEnhancementMinReadAhead{900};
    static constexpr std::chrono::milliseconds kSeekPrerollEnhancementTimeoutMinReadAhead{500};
    static constexpr std::size_t kSeekRecoveryHandoffMinQueuedFrames = 3;
    static constexpr std::chrono::milliseconds kSeekRecoveryHandoffMinReadAhead{900};
    static constexpr std::chrono::milliseconds kSeekRecoveryWarmupMinSpan{80};
    static constexpr std::chrono::milliseconds kSeekRecoveryWarmupMaxWait{650};
    static constexpr std::chrono::milliseconds kRuntimeSeekIoTimeout{4500};

    enum class SeekRecoveryPhase {
        None,
        Preroll,
        VisualWarmup,
    };

    struct SeekRecoveryState {
        SeekRecoveryPhase phase = SeekRecoveryPhase::None;
        uint64_t timelineSerial = 0;
        std::chrono::milliseconds target{0};
        std::chrono::milliseconds clockAnchorPts{0};
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::time_point visualStartedAt{};
        std::chrono::steady_clock::time_point clockAnchorTime{};
        std::optional<std::chrono::milliseconds> firstPublishedPts;
        int publishedFrames = 0;
    };

    struct NativeSubtitleCue {
        std::chrono::milliseconds start{0};
        std::chrono::milliseconds end{0};
        std::wstring text;
        std::vector<NativeSubtitleBitmap> bitmaps;
    };

    struct DolbyVisionEnhancementFrame {
        std::chrono::milliseconds pts{0};
        NativeYuvPlanes yuv;
        std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> dovi;
        std::wstring details;
    };

    void DecodeLoop();
    int SelectVideoStream(AVFormatContext* formatCtx) const;
    bool OpenVideoDecoder(const AVCodec* codec,
                          const AVCodecParameters* codecpar,
                          AVCodecContext*& codecCtx,
                          AVBufferRef*& hwDeviceCtx);
    bool OpenDolbyVisionEnhancementDecoder(AVFormatContext* formatCtx,
                                           int primaryStreamIndex,
                                           AVCodecContext*& codecCtx,
                                           AVRational& timeBase);
    bool OpenSubtitleDecoder(AVFormatContext* formatCtx,
                             int& subtitleStreamIndex,
                             AVRational& subtitleTimeBase,
                             AVCodecContext*& subtitleCodecCtx);
    bool DecodeExternalSubtitleFile(const std::filesystem::path& subtitlePath);
    bool DecodeExternalAssSubtitleFile(const std::filesystem::path& subtitlePath);
    bool EnsureAssSubtitleRenderer();
    bool ConfigureAssSubtitleStream(AVFormatContext* formatCtx, const AVStream* stream);
    void AddAssFontAttachments(AVFormatContext* formatCtx);
    AVCodecContext* AllocateVideoCodecContext(const AVCodec* codec, const AVCodecParameters* codecpar) const;
    bool ConfigureD3D11VA(const AVCodec* codec, AVCodecContext* codecCtx, AVBufferRef*& hwDeviceCtx);
    int CreateD3D11VADeviceContext(AVBufferRef** device, std::wstring& deviceMode);

    static AVPixelFormat ChooseHardwarePixelFormat(AVCodecContext* codecCtx, const AVPixelFormat* pixelFormats);

    bool ReceiveFrames(AVCodecContext* codecCtx, SwsContext*& swsCtx, AVFrame* frame, AVFrame* softwareFrame,
                       std::vector<uint8_t>& bgraBuffer, AVRational timeBase,
                       uint64_t& serial);
    bool DrainDecoder(AVCodecContext* codecCtx, SwsContext*& swsCtx, AVFrame* frame, AVFrame* softwareFrame,
                      std::vector<uint8_t>& bgraBuffer, AVRational timeBase,
                      uint64_t& serial);
    bool DecodeDolbyVisionEnhancementPacket(AVCodecContext* codecCtx,
                                            const AVPacket* packet,
                                            AVFrame* frame,
                                            AVRational timeBase);
    bool ReceiveDolbyVisionEnhancementFrames(AVCodecContext* codecCtx, AVFrame* frame, AVRational timeBase);
    bool PublishFrame(AVFrame* frame, AVFrame* softwareFrame, SwsContext*& swsCtx, std::vector<uint8_t>& bgraBuffer,
                      AVRational timeBase, uint64_t& serial);
    void AttachDolbyVisionEnhancementFrame(NativeVideoFrame& frame, std::chrono::milliseconds pts);
    void LogDolbyVisionCpuReferenceSample(const NativeVideoFrame& frame);
    bool ShouldUsePrimaryDoviLibplacebo() const;
    bool TryPublishDoviLibplaceboFrame(AVFrame* frame,
                                       AVRational timeBase,
                                       std::chrono::milliseconds pts,
                                       uint64_t& serial);
    bool EnsureDoviLibplaceboFilter(AVFrame* frame, AVRational timeBase);
    bool TryBuildD3DTextureFrame(AVFrame* frame, std::chrono::milliseconds pts, uint64_t& serial, NativeVideoFrame& out);
    bool PublishImmediateFrame(NativeVideoFrame&& frame);
    bool DecodeSubtitlePacket(AVCodecContext* subtitleCodecCtx, const AVPacket* packet, AVRational subtitleTimeBase);
    bool ApplyPendingSeek(AVFormatContext* formatCtx,
                          AVCodecContext* codecCtx,
                          AVCodecContext* subtitleCodecCtx,
                          AVCodecContext* enhancementCodecCtx,
                          int videoStreamIndex,
                          AVRational videoTimeBase);
    std::optional<std::chrono::milliseconds> TakePendingSeek();
    bool HasPendingSeek() const;
    void TrimActiveBitmapSubtitleCues(std::chrono::milliseconds time);
    void PruneExpiredSubtitleCues(std::chrono::milliseconds effectivePts);
    void RefreshFrameSubtitles(NativeVideoFrame& frame, bool force = false);
    bool RefreshLatestFrameSubtitles();
    bool RefreshQueuedFrameSubtitles();
    std::wstring SubtitleTextForPts(std::chrono::milliseconds pts);
    std::vector<NativeSubtitleBitmap> SubtitleBitmapsForPts(std::chrono::milliseconds pts,
                                                            int frameWidth,
                                                            int frameHeight,
                                                            bool includeAss = true);

    bool ShouldDropSeekPreroll(std::chrono::milliseconds pts);

    static std::chrono::milliseconds FramePts(const AVFrame* frame, AVRational timeBase);
    static AVPixelFormat HardwareFrameSoftwareFormat(const AVFrame* frame);
    static anvil::playback::VideoColorMetadata BuildColorMetadata(const AVCodecParameters* parameters);
    static anvil::playback::VideoColorMetadata MergeFrameColorMetadata(
        const AVFrame* frame,
        const anvil::playback::VideoColorMetadata& defaults);
    static anvil::playback::VideoColorMetadata SdrBt709ColorMetadata();
    static anvil::playback::VideoColorMetadata HdrBt2020PqColorMetadata();
    // Extracts Dolby Vision reshaping metadata from AV_FRAME_DATA_DOVI_METADATA
    // side data. Returns nullptr if the frame has no DV metadata.
    static std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> ExtractDolbyVisionMetadata(const AVFrame* frame);
    // Frame-level DV metadata extraction, seeded with stream-level config
    // (profile/level/compat_id/el/bl flags from AV_PKT_DATA_DOVI_CONF, which
    // are not present in per-frame AV_FRAME_DATA_DOVI_METADATA).
    std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> ExtractFrameDolbyVisionMetadata(const AVFrame* frame);
    std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> ExtractEnhancementDolbyVisionMetadata(const AVFrame* frame);
    static bool IsSupportedHardwareTextureFormat(DXGI_FORMAT format);
    static std::wstring DxgiFormatName(DXGI_FORMAT format);
    static std::wstring PixelFormatName(AVPixelFormat format);

    void LogZeroCopyFallbackOnce(const std::wstring& reason);
    std::shared_ptr<std::vector<uint8_t>> AcquireReusableBgraBuffer(std::size_t needed);
    static std::size_t FrameQueueCostBytes(const NativeVideoFrame& frame);
    static std::size_t MaxQueueDepthForFrame(const NativeVideoFrame& frame);
    bool HasQueueCapacityLocked(const NativeVideoFrame& frame) const;
    void UpdateBufferedStatsLocked();
    bool SeekPrerollReadyLocked() const;

    bool EnqueueFrame(NativeVideoFrame&& frame);
    void DrainQueuedFrames();
    void ScheduleDueFrames();
    static int InterruptCallback(void* opaque);

    struct SchedulerClock {
        std::chrono::milliseconds position{0};
        bool usingAudioClock = false;
    };

    SchedulerClock CurrentSchedulerClockLocked(std::chrono::milliseconds firstQueuedPts);

    void NotifyFrameReady();
    void NotifyDecodeFailure(const std::wstring& message) const;
    void SetDecodeBackend(const std::wstring& decoder, bool usingHardware, const std::wstring& fallbackReason);
    void LogThread(anvil::playback::LogLevel level, const std::wstring& category, const std::wstring& message) const;
    void LogThreadError(const std::wstring& message) const;
    uint64_t CurrentTimelineSerial() const;
    uint64_t AdvanceTimelineSerial();
    void BeginSeekRecoveryLocked(std::chrono::milliseconds target, bool prerollAfterSeek, uint64_t timelineSerial);
    void ResetSeekRecoveryLocked();
    void DropStaleFramesLocked();
    void StartSeekRecoveryVisualWarmupLocked(std::chrono::steady_clock::time_point now);
    SchedulerClock SeekRecoverySchedulerClockLocked(std::chrono::milliseconds firstQueuedPts);
    void UpdateSeekRecoveryAfterPublishLocked(std::chrono::milliseconds publishedPts,
                                              std::chrono::steady_clock::time_point now);

    std::filesystem::path path_;
    std::chrono::milliseconds startPosition_{0};
    LogSinkPtr logSink_;
    ClockCallback clockCallback_;
    Microsoft::WRL::ComPtr<ID3D11Device> sharedD3DDevice_;
    bool preferHardwareDecode_ = false;
    AVPixelFormat hardwarePixelFormat_ = AV_PIX_FMT_NONE;
    bool hardwareDecodeActive_ = false;
    bool hardwareFormatLogged_ = false;
    bool zeroCopyFallbackLogged_ = false;
    bool firstDecodedFrameLogged_ = false;
    bool firstHardwareFrameLogged_ = false;
    bool firstCpuTransferFrameLogged_ = false;
    int receiveEagainLogCount_ = 0;
    bool receiveEofLogged_ = false;
    bool sendPacketFailureLogged_ = false;
    bool sendPacketBackpressureLogged_ = false;
    bool bitmapSubtitleLogged_ = false;
    bool frameSubtitleBitmapLogged_ = false;
    anvil::playback::VideoColorMetadata streamColorMetadata_;
    bool dolbyVisionStream_ = false;        // stream-level DV detection (AV_PKT_DATA_DOVI_CONF)
    bool dolbyVisionFirstFrameLogged_ = false;
    bool dolbyVisionFirstPackedLogged_ = false;
    bool dolbyVisionFirstQueueLogged_ = false;
    bool dolbyVisionLibplaceboFailed_ = false;
    bool dolbyVisionLibplaceboFrameLogged_ = false;
    bool dolbyVisionDynamicMetadataLogged_ = false;
    uint64_t dolbyVisionLastDynamicMetadataFingerprint_ = 0;
    bool preferDolbyVisionHdrOutput_ = false;
    bool enableDolbyVisionEnhancementDecode_ = false;
    int selectedVideoTrackIndex_ = anvil::playback::kVideoTrackAuto;
    std::unique_ptr<DoviLibplaceboFilterState> doviLibplaceboFilter_;
    // Stream-level DV configuration (not present in per-frame metadata).
    int dolbyVisionProfile_ = 0;
    int dolbyVisionLevel_ = 0;
    int dolbyVisionCompatId_ = 0;
    bool dolbyVisionElPresent_ = false;
    bool dolbyVisionBlPresent_ = false;
    bool dolbyVisionEnhancementActive_ = false;
    bool dolbyVisionEnhancementFirstFrameLogged_ = false;
    bool dolbyVisionEnhancementDynamicMetadataLogged_ = false;
    bool dolbyVisionEnhancementFailureLogged_ = false;
    bool dolbyVisionEnhancementOverlayLogged_ = false;
    bool dolbyVisionEnhancementNoMatchLogged_ = false;
    bool dolbyVisionEnhancementFirstPackedLogged_ = false;
    bool dolbyVisionCpuReferenceLogged_ = false;
    bool dolbyVisionMultiPartitionFallbackLogged_ = false;
    bool dolbyVisionEnhancementStartupFallbackLogged_ = false;
    bool dolbyVisionEnhancementBaseOnlyDropLogged_ = false;
    int dolbyVisionEnhancementStartupMisses_ = 0;
    uint64_t dolbyVisionEnhancementLastDynamicMetadataFingerprint_ = 0;
    uint64_t dolbyVisionEnhancementFramesDecoded_ = 0;
    int dolbyVisionEnhancementStreamIndex_ = -1;
    int dolbyVisionEnhancementProfile_ = 0;
    int dolbyVisionEnhancementLevel_ = 0;
    int dolbyVisionEnhancementCompatId_ = 0;
    bool dolbyVisionEnhancementElPresent_ = false;
    bool dolbyVisionEnhancementBlPresent_ = false;
    std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> latestDolbyVisionEnhancementMetadata_;
    std::chrono::milliseconds latestDolbyVisionEnhancementMetadataPts_{0};
    std::deque<DolbyVisionEnhancementFrame> dolbyVisionEnhancementFrames_;
    std::wstring preferredSubtitleLanguage_ = L"Auto";
    int selectedSubtitleTrackIndex_ = anvil::playback::kSubtitleTrackAuto;
    NativeAudioPacketSink audioPacketSink_;
    std::chrono::milliseconds subtitleDelay_{0};
    bool autoLoadExternalSubtitles_ = true;
    std::filesystem::path externalSubtitlePath_;
    bool oneShotFrame_ = false;
    int subtitleCanvasWidth_ = 0;
    int subtitleCanvasHeight_ = 0;
    bool subtitleCanvasLogged_ = false;
    uint64_t subtitleBitmapSerial_ = 0;
    bool subtitleAssActive_ = false;
    bool subtitleAssExternalFullTrack_ = false;
    bool subtitleAssLogged_ = false;
    std::unique_ptr<LibassSubtitleRenderer> subtitleAssRenderer_;
    bool schedulePrimed_ = false;   // startup warm-up gate for the scheduler
    bool externalSubtitlesActive_ = false;
    std::deque<NativeSubtitleCue> subtitleCues_;
    std::deque<NativeSubtitleCue> externalSubtitleCues_;
    std::optional<std::chrono::steady_clock::time_point> fallbackClockAnchor_;
    std::chrono::milliseconds fallbackClockBasePts_{0};
    SeekRecoveryState seekRecovery_;
    mutable std::mutex mutex_;
    NativeVideoFrame latestFrame_;
    std::deque<NativeVideoFrame> frameQueue_;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> reusableBgraBuffers_;
    NativeVideoQueueStats stats_;
    std::atomic_bool stopping_{false};
    std::atomic_bool running_{false};
    std::atomic<HWND> notificationWindow_{nullptr};
    std::atomic_uint notificationMessage_{0};
    std::atomic_uint failureMessage_{0};
    std::atomic_bool frameMessagePending_{false};
    std::atomic<int64_t> pendingSeekMs_{-1};
    std::atomic_bool pendingSeekInterruptsEnabled_{false};
    std::atomic<int64_t> ioInterruptAfterSteadyMs_{0};
    std::atomic<uint64_t> activeTimelineSerial_{1};
    mutable std::atomic<int> interruptReturnCount_{0};
    std::atomic_bool playbackPaused_{false};
    std::atomic_bool enhancementPrerollWaitActive_{false};
    std::atomic<int> seekFastResumeFramesRemaining_{0};
    std::atomic_bool seekFastResumeLogged_{false};
    std::atomic_bool seekPrerollPending_{false};
    std::atomic<int64_t> seekRecoveryTargetMs_{-1};
    std::atomic_bool seekRecoveryDropLogged_{false};
    std::atomic<int64_t> pausedPositionMs_{0};
    std::thread decodeThread_;
};

}  // namespace anvil::app
