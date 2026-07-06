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
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTexture;
    UINT d3dArraySlice = 0;
    DXGI_FORMAT d3dFormat = DXGI_FORMAT_UNKNOWN;
    AVPixelFormat softwareFormat = AV_PIX_FMT_NONE;
    anvil::playback::VideoColorMetadata color;
    // Per-frame Dolby Vision metadata (reshaping curves + color matrices).
    // nullptr for non-DV streams. When present, the renderer must apply the
    // IPTPQc2 reshaping before treating the pixels as BT.2020 PQ.
    std::shared_ptr<const anvil::playback::DolbyVisionFrameMetadata> dovi;
    std::wstring subtitleText;
    std::vector<NativeSubtitleBitmap> subtitleBitmaps;
    std::shared_ptr<AVFrame> hardwareFrameRef;
    std::chrono::milliseconds pts{0};
    uint64_t serial = 0;

    bool HasPixels() const {
        return bgra && !bgra->empty() && width > 0 && height > 0 && stride > 0;
    }

    bool HasYuv() const {
        return yuv.HasData();
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
    uint64_t rendered = 0;
    uint64_t droppedLate = 0;
    uint64_t droppedQueueFull = 0;
    uint64_t hardwareFrames = 0;
    uint64_t zeroCopyFrames = 0;
    uint64_t cpuTransferFrames = 0;
    std::chrono::milliseconds clockPosition{0};
    int driftMs = 0;
    bool usingAudioClock = false;
    bool usingHardwareDecode = false;
    std::wstring decoder = L"ffmpeg_software";
    std::wstring fallbackReason;
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
               ClockCallback clockCallback = {},
               bool preferHardwareDecode = false,
               ID3D11Device* sharedD3DDevice = nullptr,
               int selectedVideoTrackIndex = anvil::playback::kVideoTrackAuto,
               std::wstring preferredSubtitleLanguage = L"Auto",
               int selectedSubtitleTrackIndex = anvil::playback::kSubtitleTrackAuto,
               std::chrono::milliseconds subtitleDelay = std::chrono::milliseconds{0},
               bool autoLoadExternalSubtitles = true,
               bool oneShotFrame = false,
               bool preferDolbyVisionHdrOutput = false);

    void Stop();

    bool IsRunning() const {
        return running_.load();
    }

    bool LatestFrame(NativeVideoFrame& frame) const;

    void ClearFrame();

    void AcknowledgeFrameNotification();

    NativeVideoQueueStats Stats() const;

    const std::filesystem::path& Path() const { return path_; }

private:
    // Decoded-frame queue depth. Larger = more resilience to IO/decode jitter
    // at the cost of memory (each entry holds a BGRA buffer or D3D11 texture
    // ref). 48 frames ≈ 2 s at 24 fps. For 4K BGRA this is ~1.5 GB worst case,
    // but the hardware path uses zero-copy textures (GPU memory only).
    // Decoded-frame queue depth.
    static constexpr std::size_t kMaxQueuedFrames = 6;
    static constexpr std::size_t kMaxReusableBgraBuffers = 12;
    static constexpr std::chrono::milliseconds kFrameEarlyTolerance{12};
    static constexpr std::chrono::milliseconds kFrameLateDropThreshold{120};

    struct NativeSubtitleCue {
        std::chrono::milliseconds start{0};
        std::chrono::milliseconds end{0};
        std::wstring text;
        std::vector<NativeSubtitleBitmap> bitmaps;
    };

    void DecodeLoop();
    int SelectVideoStream(AVFormatContext* formatCtx) const;
    bool OpenVideoDecoder(const AVCodec* codec,
                          const AVCodecParameters* codecpar,
                          AVCodecContext*& codecCtx,
                          AVBufferRef*& hwDeviceCtx);
    bool OpenSubtitleDecoder(AVFormatContext* formatCtx,
                             int& subtitleStreamIndex,
                             AVRational& subtitleTimeBase,
                             AVCodecContext*& subtitleCodecCtx);
    bool DecodeExternalSubtitleFile(const std::filesystem::path& subtitlePath);
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
    bool PublishFrame(AVFrame* frame, AVFrame* softwareFrame, SwsContext*& swsCtx, std::vector<uint8_t>& bgraBuffer,
                      AVRational timeBase, uint64_t& serial);
    bool TryPublishDoviLibplaceboFrame(AVFrame* frame,
                                       AVRational timeBase,
                                       std::chrono::milliseconds pts,
                                       uint64_t& serial);
    bool EnsureDoviLibplaceboFilter(AVFrame* frame, AVRational timeBase);
    bool TryBuildD3DTextureFrame(AVFrame* frame, std::chrono::milliseconds pts, uint64_t& serial, NativeVideoFrame& out);
    bool PublishImmediateFrame(NativeVideoFrame&& frame);
    bool DecodeSubtitlePacket(AVCodecContext* subtitleCodecCtx, const AVPacket* packet, AVRational subtitleTimeBase);
    void TrimActiveBitmapSubtitleCues(std::chrono::milliseconds time);
    std::wstring SubtitleTextForPts(std::chrono::milliseconds pts);
    std::vector<NativeSubtitleBitmap> SubtitleBitmapsForPts(std::chrono::milliseconds pts, int frameWidth, int frameHeight);

    bool ShouldDropSeekPreroll(std::chrono::milliseconds pts) const;

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
    static bool IsSupportedHardwareTextureFormat(DXGI_FORMAT format);
    static std::wstring DxgiFormatName(DXGI_FORMAT format);
    static std::wstring PixelFormatName(AVPixelFormat format);

    void LogZeroCopyFallbackOnce(const std::wstring& reason);
    std::shared_ptr<std::vector<uint8_t>> AcquireReusableBgraBuffer(std::size_t needed);

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
    void SetDecodeBackend(const std::wstring& decoder, bool usingHardware, const std::wstring& fallbackReason);
    void LogThread(anvil::playback::LogLevel level, const std::wstring& category, const std::wstring& message) const;
    void LogThreadError(const std::wstring& message) const;

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
    bool bitmapSubtitleLogged_ = false;
    anvil::playback::VideoColorMetadata streamColorMetadata_;
    bool dolbyVisionStream_ = false;        // stream-level DV detection (AV_PKT_DATA_DOVI_CONF)
    bool dolbyVisionFirstFrameLogged_ = false;
    bool dolbyVisionFirstPackedLogged_ = false;
    bool dolbyVisionFirstQueueLogged_ = false;
    bool dolbyVisionLibplaceboFailed_ = false;
    bool dolbyVisionLibplaceboFrameLogged_ = false;
    bool preferDolbyVisionHdrOutput_ = false;
    int selectedVideoTrackIndex_ = anvil::playback::kVideoTrackAuto;
    std::unique_ptr<DoviLibplaceboFilterState> doviLibplaceboFilter_;
    // Stream-level DV configuration (not present in per-frame metadata).
    int dolbyVisionProfile_ = 0;
    int dolbyVisionLevel_ = 0;
    int dolbyVisionCompatId_ = 0;
    bool dolbyVisionElPresent_ = false;
    bool dolbyVisionBlPresent_ = false;
    std::wstring preferredSubtitleLanguage_ = L"Auto";
    int selectedSubtitleTrackIndex_ = anvil::playback::kSubtitleTrackAuto;
    std::chrono::milliseconds subtitleDelay_{0};
    bool autoLoadExternalSubtitles_ = true;
    bool oneShotFrame_ = false;
    int subtitleCanvasWidth_ = 0;
    int subtitleCanvasHeight_ = 0;
    bool subtitleCanvasLogged_ = false;
    uint64_t subtitleBitmapSerial_ = 0;
    bool schedulePrimed_ = false;   // startup warm-up gate for the scheduler
    std::deque<NativeSubtitleCue> subtitleCues_;
    std::optional<std::chrono::steady_clock::time_point> fallbackClockAnchor_;
    std::chrono::milliseconds fallbackClockBasePts_{0};
    mutable std::mutex mutex_;
    NativeVideoFrame latestFrame_;
    std::deque<NativeVideoFrame> frameQueue_;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> reusableBgraBuffers_;
    NativeVideoQueueStats stats_;
    std::atomic_bool stopping_{false};
    std::atomic_bool running_{false};
    std::atomic<HWND> notificationWindow_{nullptr};
    std::atomic_uint notificationMessage_{0};
    std::atomic_bool frameMessagePending_{false};
    std::thread decodeThread_;
};

}  // namespace anvil::app
