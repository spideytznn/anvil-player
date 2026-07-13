#pragma once

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/d3d12_frame_graph.h"
#include "AnvilPlayer/App/d3d12_dolby_vision.h"
#include "AnvilPlayer/App/d3d12_tensor_preprocessor.h"
#include "AnvilPlayer/App/directml_frame_interpolation_executor.h"
#include "AnvilPlayer/App/libplacebo_d3d12_bridge.h"
#include "AnvilPlayer/App/nvidia_hdr_output.h"
#include "AnvilPlayer/App/windows_ml_frame_interpolation_executor.h"
#include "AnvilPlayer/App/log_sink_ptr.h"
#include "AnvilPlayer/App/video_renderer_types.h"
#include "AnvilPlayer/Playback/CapabilityReport.h"
#include "AnvilPlayer/Playback/Settings.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <map>
#include <string>
#include <thread>

namespace anvil::app {

// Native D3D12 renderer. FFmpeg D3D12VA surfaces stay resident on the single
// application device. The graphics queue waits on the decoder fence, samples
// the NV12/P010 planes directly and writes the final RGB10 SDR/HDR10 target.
class D3D12VideoRenderer {
public:
    explicit D3D12VideoRenderer(LogSinkPtr logSink = nullptr);
    ~D3D12VideoRenderer();
    D3D12VideoRenderer(const D3D12VideoRenderer&) = delete;
    D3D12VideoRenderer& operator=(const D3D12VideoRenderer&) = delete;

    bool BeginInitialize(HWND host,
                         HWND completionWindow,
                         UINT completionMessage,
                         uint64_t completionCookie,
                         UINT deviceLostMessage = 0);
    VideoRendererState State() const noexcept { return state_.load(); }
    bool IsReady() const noexcept { return State() == VideoRendererState::Ready; }
    bool InitializationFailed() const noexcept { return State() == VideoRendererState::Failed; }
    void RequestStop(HWND completionWindow, UINT completionMessage, uint64_t completionCookie = 0);
    bool IsStopped() const noexcept { return stopped_.load(); }

    void ConfigureColorPipeline(const anvil::playback::VideoSettings& settings,
                                const anvil::playback::DisplayCapabilities& display,
                                const anvil::playback::VideoColorMetadata& mediaColor);
    void ConfigureSubtitleSettings(const anvil::playback::SubtitleSettings& settings);
    void ConfigureUiOverlay(std::shared_ptr<const VideoUiOverlayBitmap> overlay,
                            bool requestImmediatePresent);
    void ConfigureUiMenuOverlay(std::shared_ptr<const VideoUiOverlayBitmap> overlay,
                                bool requestImmediatePresent);
    void ConfigureUiMenuOverlayPresentation(int destinationX,
                                            int destinationY,
                                            int displayWidth,
                                            int displayHeight,
                                            float opacity,
                                            bool requestImmediatePresent);
    void OnResize();
    void SetDiagnosticsEnabled(bool enabled);
    void ResetRenderStats();
    VideoRenderStats TakeRenderStats() const;
    int DetectedDisplayPeakNits() const noexcept {
        return detectedDisplayPeakNits_.load(std::memory_order_acquire);
    }
    int EffectiveDisplayPeakNits() const noexcept {
        return effectiveDisplayPeakNits_.load(std::memory_order_acquire);
    }
    void Render(const NativeVideoFrame& frame);
    void QueueFrameGraphInput(const NativeVideoFrame& frame);
    bool RetireFrame(NativeVideoFrame&& frame) noexcept;
    void Clear();
    ID3D12Device* Device() const noexcept { return publishedDevice_.load(); }

private:
    struct OverlayPresentation {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        float opacity = 1.0f;
    };
    struct OverlaySlot {
        std::shared_ptr<const VideoUiOverlayBitmap> bitmap;
        OverlayPresentation presentation;
    };
    struct GeneratedFrameSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> output;
        std::size_t capacityBytes = 0;
        TensorShape shape;
        int displayWidth = 0;
        int displayHeight = 0;
        std::chrono::milliseconds leftPts{0};
        std::chrono::milliseconds rightPts{0};
        std::chrono::milliseconds targetPts{0};
        uint64_t timelineSerial = 0;
        uint64_t epoch = 0;
        GpuFencePoint ready;
        GpuFencePoint reusable;
        std::unique_ptr<NativeVideoFrame> overlayFrame;
        std::chrono::steady_clock::time_point dueAt{};
        DoviDisplayTrim doviTrim;
        float doviSourcePeakNits = 1000.0f;
        UINT inputTransfer = 0;  // Zero derives the transfer from overlayFrame.
        float interpolationT = 0.5f;
        bool anchored = false;
        bool completionStatusConsumed = false;
        bool pending = false;
    };

    struct OverlayTextureCache {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload;
        void* mappedUpload = nullptr;
        const void* sourceIdentity = nullptr;
        uint64_t sourceSerial = 0;
        UINT width = 0;
        UINT height = 0;
        UINT64 uploadCapacity = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    };

    struct PixelFrameUploadCache {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload;
        void* mappedUpload = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 uploadCapacity = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    };

    void RenderThreadMain();
    bool InitializeGpu();
    void StartMlInitialization();
    void StartAdvancedPipelineInitialization();
    bool CreateDolbyVisionPipeline();
    void ResetInterpolationState();
    bool CreateSwapChain(UINT width, UINT height);
    bool ConfigureFramePacing();
    void WaitForFrameLatencyObject();
    void RefreshDisplayPeakNits();
    bool CreateLibplaceboTargets(UINT width, UINT height);
    bool CreatePipeline();
    bool Resize(UINT width, UINT height);
    bool RenderFrame(const NativeVideoFrame& frame);
    bool RenderPixelFrame(const NativeVideoFrame& frame);
    bool TryRenderDolbyVisionWithLibplacebo(
        const NativeVideoFrame& frame,
        UINT backBufferIndex,
        std::chrono::steady_clock::time_point startedAt);
    bool RenderGeneratedFrame(std::size_t slotIndex);
    bool SetHdrOutputState(const NativeVideoFrame& frame, bool enabled);
    void ProcessReadyGeneratedFrame();
    void ProcessGeneratedBefore(const NativeVideoFrame& nextOriginal);
    void ProcessFrameGraphInput(const NativeVideoFrame& frame);
    void AnchorGeneratedFrames(const NativeVideoFrame& original,
                               std::chrono::steady_clock::time_point presentedAt);
    int InterpolationMultiplier(const NativeVideoFrame& first,
                                const NativeVideoFrame& second) const;
    void RetireGeneratedSlot(std::size_t slotIndex, bool consumeStatus);
    void RecordGeneratedDeadline(bool met);
    bool EnsureGeneratedOutput(std::size_t slotIndex, TensorShape shape);
    std::optional<std::size_t> AcquireGeneratedSlot();
    bool DrawOverlays(const NativeVideoFrame* frame,
                      const D3D12_VIEWPORT& videoViewport,
                      UINT backBufferIndex);
    bool RenderLastFrame();
    void ClearFrame();
    bool WaitForBackBuffer(UINT index);
    void ReleaseGpu();
    void NotifyInitialization(VideoRendererState state) const;
    bool DeviceLost(HRESULT result, const wchar_t* operation);
    void Log(anvil::playback::LogLevel level, const std::wstring& message) const;

    LogSinkPtr logSink_;
    std::atomic<HWND> host_{nullptr};
    std::atomic<VideoRendererState> state_{VideoRendererState::Stopped};
    std::atomic_bool stopped_{true};
    std::atomic<ID3D12Device*> publishedDevice_{nullptr};
    std::thread renderThread_;
    mutable std::mutex commandMutex_;
    std::condition_variable commandCv_;
    bool stopRequested_ = false;
    bool pendingResize_ = false;
    bool pendingClear_ = false;
    bool pendingPresent_ = false;
    bool pendingMlInitialization_ = false;
    bool pendingInterpolationReset_ = false;
    bool diagnosticsEnabled_ = false;
    std::unique_ptr<NativeVideoFrame> pendingFrame_;
    std::deque<std::unique_ptr<NativeVideoFrame>> pendingGraphInputs_;
    std::unique_ptr<NativeVideoFrame> currentFrame_;
    std::array<std::unique_ptr<NativeVideoFrame>, 2> inFlightFrames_;
    std::deque<std::unique_ptr<NativeVideoFrame>> retiredFrames_;
    anvil::playback::VideoSettings videoSettings_;
    anvil::playback::DisplayCapabilities displayCapabilities_;
    anvil::playback::VideoColorMetadata mediaColor_;
    HMONITOR displayPeakMonitor_ = nullptr;
    bool displayPeakQueryAttempted_ = false;
    std::atomic<int> detectedDisplayPeakNits_{0};
    std::atomic<int> effectiveDisplayPeakNits_{1000};
    anvil::playback::SubtitleSettings subtitleSettings_;
    std::array<OverlaySlot, 2> overlaySlots_{};
    std::array<OverlaySlot, 2> activeOverlaySlots_{};
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    uint64_t completionCookie_ = 0;
    UINT deviceLostMessage_ = 0;
    HWND stopWindow_ = nullptr;
    UINT stopMessage_ = 0;
    uint64_t stopCookie_ = 0;

    mutable std::mutex statsMutex_;
    VideoRenderStats publishedStats_{};
    VideoRenderStats renderStats_{};

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> computeQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> mlQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> copyQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    HANDLE frameLatencyWaitable_ = nullptr;
    bool swapChainWaitable_ = false;
    bool framePacingLogged_ = false;
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual_;
    static constexpr UINT kBufferCount = 2;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> backBuffers_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> libplaceboTargets_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kBufferCount> allocators_;
    std::array<uint64_t, kBufferCount> bufferFenceValues_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t nextFenceValue_ = 1;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> tensorSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> overlaySrvHeap_;
    UINT rtvIncrement_ = 0;
    UINT srvIncrement_ = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> rgbPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> yuvPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> doviPipeline_;
    std::atomic<ID3D12PipelineState*> publishedDoviPipeline_{nullptr};
    std::atomic_bool advancedPipelineStarted_{false};
    Microsoft::WRL::ComPtr<ID3DBlob> yuvVertexShader_;
    std::string doviPixelShaderSource_;
    std::thread advancedPipelineThread_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> doviConstantBuffers_;
    std::array<void*, kBufferCount> doviConstantMappings_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> overlayPipeline_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> overlayRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> tensorRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> tensorPipeline_;
    D3D12FrameGraph frameGraph_;
    std::unique_ptr<LibplaceboD3D12Bridge> libplaceboBridge_;
    NvidiaHdrOutput nvidiaHdrOutput_;
    bool libplaceboRenderFailureLogged_ = false;
    D3D12TensorPreprocessor tensorPreprocessor_;
    WindowsMlFrameInterpolationExecutor windowsMlExecutor_;
    DirectMlFrameInterpolationExecutor directMlExecutor_;
    std::atomic<IMlFrameInterpolationExecutor*> mlExecutor_{nullptr};
    std::atomic_bool mlInitializationStarted_{false};
    std::atomic_bool interpolationRequested_{false};
    std::thread mlInitializationThread_;
    std::unique_ptr<NativeVideoFrame> tensorPreviousFrame_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tensorPreviousDoviTarget_;
    std::map<std::pair<uint64_t, uint64_t>, GpuFencePoint> sourceComputeCompletions_;
    std::map<std::pair<uint64_t, uint64_t>, GpuFencePoint> sourceGraphicsCompletions_;
    std::chrono::milliseconds presentedOriginalPts_{0};
    uint64_t presentedOriginalTimeline_ = 0;
    std::chrono::steady_clock::time_point presentedOriginalAt_{};
    std::wstring presentedOriginalSubtitleText_;
    std::vector<NativeSubtitleBitmap> presentedOriginalSubtitleBitmaps_;
    bool presentedOriginalSubtitlesPrepared_ = false;
    bool tensorPreprocessorLogged_ = false;
    bool directMlSubmitLogged_ = false;
    bool generatedPresentLogged_ = false;
    bool dolbyVisionD3D12Logged_ = false;
    bool dolbyVisionPipelineWaitLogged_ = false;
    static constexpr int kMaximumInterpolationMultiplier = 5;
    int adaptiveMultiplierCap_ = kMaximumInterpolationMultiplier;
    int adaptiveDeadlineSamples_ = 0;
    int adaptiveDeadlineHits_ = 0;
    int adaptiveStableWindows_ = 0;
    std::chrono::steady_clock::time_point adaptiveCalibrationStartedAt_{};
    bool adaptiveCadenceLocked_ = false;
    static constexpr std::size_t kGeneratedSlotCount = 8;
    static constexpr UINT kMaxOverlayTextures = 32;
    std::array<GeneratedFrameSlot, kGeneratedSlotCount> generatedSlots_{};
    std::deque<std::size_t> pendingGeneratedSlots_;
    std::array<std::array<OverlayTextureCache, kMaxOverlayTextures>, kBufferCount>
        overlayTextureCaches_{};
    std::array<OverlayTextureCache, kMaxOverlayTextures> subtitleTextureCaches_{};
    std::array<PixelFrameUploadCache, kBufferCount> pixelFrameUploadCaches_{};
    UINT width_ = 1;
    UINT height_ = 1;
    std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, kBufferCount> transients_;
    bool hdr10ColorSpaceSupported_ = false;
    bool swapChainHdr_ = false;
    int loggedHdrOutputState_ = -1;
    bool hlgInterpolationColorPathLogged_ = false;
    bool dolbyVisionInterpolationColorBypassLogged_ = false;
    bool dolbyVisionInterpolationColorPathLogged_ = false;
    bool hdr10PlusInterpolationBypassLogged_ = false;
    bool dolbyVisionDynamicTrimLogged_ = false;
    bool interpolatedSubtitleCompositionLogged_ = false;
    bool interpolatedSubtitlePresentationSyncLogged_ = false;
    bool sharedSubtitleTextureLogged_ = false;
    uint64_t loggedDoviTrimFingerprint_ = 0;
    uint64_t loggedHdr10PlusFingerprint_ = 0;
    uint64_t loggedHdrToneCurveFingerprint_ = 0;
};

}  // namespace anvil::app
