#pragma once

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/log_sink_ptr.h"
#include "AnvilPlayer/App/nvidia_hdr_output.h"
#include "AnvilPlayer/Playback/CapabilityReport.h"
#include "AnvilPlayer/Playback/Settings.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <windows.h>

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace anvil::app {

struct D3D11QueuedVideoFrame;

struct D3D11UiOverlayBitmap {
    int width = 0;
    int height = 0;
    int destinationX = 0;
    int destinationY = 0;
    bool alphaFromRgb = false;
    float opacity = 1.0f;
    std::shared_ptr<const std::vector<uint8_t>> bgraPremultiplied;
};

// Initialization and rendering are owned by the render worker. Failed is a
// terminal state for the current renderer instance (the worker has exited and
// all GPU resources have been released). Stopping means RequestStop has been
// observed but a driver call may still be returning on the worker.
enum class D3D11RendererState : uint32_t {
    Stopped = 0,
    Initializing,
    Ready,
    Failed,
    Stopping,
};

struct D3D11RenderStats {
    uint64_t frames = 0;
    uint64_t hardwareFrames = 0;
    uint64_t bgraFrames = 0;
    uint64_t subtitleFrames = 0;
    uint64_t slowFrames = 0;
    uint64_t hardwareSrvCacheHits = 0;
    uint64_t hardwareSrvCacheMisses = 0;
    uint64_t subtitleSurfaceRebuilds = 0;
    uint64_t subtitleBitmapRects = 0;
    uint64_t subtitleBitmapPixels = 0;
    uint64_t totalRenderUs = 0;
    uint64_t maxRenderUs = 0;
    uint64_t colorPipelineUs = 0;
    uint64_t hardwarePrepareUs = 0;
    uint64_t bgraUploadUs = 0;
    uint64_t subtitleUs = 0;
    uint64_t presentUs = 0;
    uint64_t maxPresentUs = 0;
    uint64_t presentSyncFrames = 0;
    uint64_t frameLatencyWaits = 0;
    uint64_t frameLatencyWaitTimeouts = 0;
    uint64_t frameLatencyWaitUs = 0;
    uint64_t maxFrameLatencyWaitUs = 0;
    uint64_t frameStatsSamples = 0;
    uint64_t frameStatsDisjoint = 0;
};

// D3D11 video renderer. Owns device/swapchain/VS+PS+NV12-PS pipeline, a
// dynamic BGRA texture, and HW NV12/P010/P016 SRVs. Renders to its own child
// HWND. Device() exposes the ID3D11Device for sharing with FfmpegVideoDecoder
// D3D11VA hardware decode (zero-copy path).
class D3D11VideoRenderer {
public:
    explicit D3D11VideoRenderer(LogSinkPtr logSink = nullptr);
    ~D3D11VideoRenderer();
    D3D11VideoRenderer(const D3D11VideoRenderer&) = delete;
    D3D11VideoRenderer& operator=(const D3D11VideoRenderer&) = delete;

    // Starts GPU initialization and returns as soon as the render worker has
    // been launched. A true result means "accepted", not "ready". The optional
    // completion message is posted for both Ready and Failed transitions with
    // the 64-bit lifetime cookie in wParam and D3D11RendererState in lParam.
    // The application is x64, so WPARAM carries the cookie without truncation.
    bool BeginInitialize(HWND host,
                         HWND completionWindow,
                         UINT completionMessage,
                         uint64_t completionCookie,
                         UINT deviceLostMessage = 0);

    // Compatibility entry point for callers that do not need a completion
    // message. This is equally non-blocking; poll State()/IsReady() before
    // requesting Device().
    bool Initialize(HWND host);

    D3D11RendererState State() const noexcept;
    bool IsReady() const noexcept;
    bool InitializationFailed() const noexcept;

    // Begins renderer shutdown without waiting for GPU work. The completion
    // message is posted only after all render-thread-owned resources are
    // released. Passing a null window or zero message requests no callback.
    void RequestStop(HWND completionWindow,
                     UINT completionMessage,
                     uint64_t completionCookie = 0);
    bool IsStopped() const noexcept;

    void ConfigureColorPipeline(const anvil::playback::VideoSettings& settings,
                                const anvil::playback::DisplayCapabilities& display,
                                const anvil::playback::VideoColorMetadata& mediaColor);
    void ConfigureSubtitleSettings(const anvil::playback::SubtitleSettings& settings);
    void ConfigureUiOverlay(std::shared_ptr<const D3D11UiOverlayBitmap> overlay,
                            bool requestImmediatePresent);
    void ConfigureUiMenuOverlay(std::shared_ptr<const D3D11UiOverlayBitmap> overlay,
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
    D3D11RenderStats TakeRenderStats();
    int DetectedDisplayPeakNits() const noexcept;
    int EffectiveDisplayPeakNits() const noexcept;
    void Render(const NativeVideoFrame& frame);

    // Transfers a frame to the bounded, process-lifetime retirement worker.
    // Returns true only after ownership has been transferred. On false the
    // source is completely unchanged; the caller must keep it alive and retry
    // later (or arrange destruction on a non-window thread).
    bool RetireFrame(NativeVideoFrame&& frame) noexcept;

    void Clear();

    // Borrowed pointer published only after the complete pipeline is ready.
    // RequestStop withdraws the publication before releasing GPU resources.
    ID3D11Device* Device() const noexcept;

private:
    struct PendingColorPipeline {
        anvil::playback::VideoSettings settings;
        anvil::playback::DisplayCapabilities display;
        anvil::playback::VideoColorMetadata mediaColor;
    };

    struct PendingUiOverlay {
        std::shared_ptr<const D3D11UiOverlayBitmap> overlay;
        bool requestImmediatePresent = false;
    };

    struct PendingUiOverlayPresentation {
        int destinationX = 0;
        int destinationY = 0;
        int displayWidth = 0;
        int displayHeight = 0;
        float opacity = 1.0f;
        bool requestImmediatePresent = false;
    };

    struct UiOverlaySlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        std::shared_ptr<const D3D11UiOverlayBitmap> active;
        std::shared_ptr<const D3D11UiOverlayBitmap> uploaded;
        int destinationX = 0;
        int destinationY = 0;
        int displayWidth = 0;
        int displayHeight = 0;
        float opacity = 1.0f;
        bool logged = false;
    };

    static constexpr std::size_t kTransportUiOverlaySlot = 0;
    static constexpr std::size_t kMenuUiOverlaySlot = 1;
    static constexpr std::size_t kUiOverlaySlotCount = 2;

    void RenderThreadMain();
    void StopRenderThread();
    bool InitializeGpuOnRenderThread(UINT width, UINT height);
    bool IsStopRequested() const;
    void PostInitializationCompletion(D3D11RendererState state,
                                      HWND window,
                                      UINT message,
                                      uint64_t cookie) const noexcept;
    bool HasPendingWorkLocked() const;
    void ApplyColorPipelineConfiguration(const PendingColorPipeline& configuration);
    void ApplySubtitleConfiguration(const anvil::playback::SubtitleSettings& settings);
    void ResizeOnRenderThread(UINT width, UINT height);
    void SetDiagnosticsEnabledOnRenderThread(bool enabled);
    void ResetRenderStatsOnRenderThread();
    void PublishRenderStats();
    bool RenderOnRenderThread(const NativeVideoFrame& frame);
    bool RepeatLastPresentOnRenderThread();
    void ClearOnRenderThread();

    void EnableMultithreadProtection();
    bool CreateRenderTarget();
    ID3D11RenderTargetView* ActiveRgbRenderTarget() const noexcept;
    ID3D11Texture2D* ActiveRgbRenderTexture() const noexcept;
    bool CreatePipeline();
    bool CreateComposition();
    bool CreateHlgPresentationResources(UINT width, UINT height);
    bool ResizeHlgPresentationResources(UINT width, UINT height);
    bool BlitHlgFrame();
    void SelectCompositionSwapChain(bool hlg);
    bool UpdateColorPipeline(const NativeVideoFrame& frame);
    void RefreshDisplayPeakNits();
    void UpdateDoviConstants(const NativeVideoFrame& frame);
    bool ApplySwapChainColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace,
                                  const anvil::playback::VideoColorMetadata& color,
                                  bool hdrToneCurveActive,
                                  bool suppressHdr10Metadata = false);
    void ApplyHdrMetadata(const anvil::playback::VideoColorMetadata& color,
                          bool hdrToneCurveActive);
    bool ConfigureFramePacing();
    void WaitForFrameLatencyObject(bool collectStats);
    bool PresentFrame(UINT syncInterval, bool collectStats, std::chrono::steady_clock::time_point stageStart);
    bool DetectDeviceLoss(HRESULT failure, const wchar_t* operation);
    bool UpdateHardwareTexture(const NativeVideoFrame& frame);
    void UpdateTexture(const NativeVideoFrame& frame);
    bool UpdateYuvTexture(const NativeVideoFrame& frame);
    bool UpdateEnhancementYuvTexture(const NativeVideoFrame& frame);
    bool UpdateSubtitleOverlay(const NativeVideoFrame& frame, const D3D11_VIEWPORT& videoViewport);
    void DrawSubtitleOverlay();
    bool DrawSubtitleBitmapOverlays(const NativeVideoFrame& frame, const D3D11_VIEWPORT& videoViewport);
    bool UpdateUiOverlayTexture(std::size_t slotIndex);
    bool DrawUiOverlaySlot(std::size_t slotIndex);
    bool DrawUiOverlay();
    void ReleaseAll();
    void LogHardwareTextureFailureOnce(const std::wstring& message);
    void LogInfo(const std::wstring& message) const;
    void LogHr(const std::wstring& what, HRESULT hr) const;
    D3D11_VIEWPORT LetterboxedViewport(int sourceWidth, int sourceHeight) const;

    struct HardwareSrvCacheEntry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT arraySlice = 0;
        DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uv;
    };

    struct SubtitleTextureCacheEntry {
        uint64_t serial = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint64_t lastUsedFrame = 0;
        std::size_t bytes = 0;
    };

    SubtitleTextureCacheEntry* EnsureSubtitleBitmapTexture(const NativeSubtitleBitmap& bitmap);
    void PruneSubtitleTextureCache();

    std::atomic<HWND> host_{nullptr};
    LogSinkPtr logSink_;

    // The public methods only update this bounded mailbox. There is never more
    // than one retained frame, resize, or instance of any coalesced command.
    // All D3D creation, immediate-context, swap-chain, Present and destruction
    // work is performed by renderThread_. Public methods only update this
    // mailbox, including while initialization is still in progress.
    mutable std::mutex commandMutex_;
    std::condition_variable commandCv_;
    std::thread renderThread_;
    bool stopRequested_ = true;
    bool acceptingCommands_ = false;
    std::atomic<bool> stopped_{true};
    std::atomic<D3D11RendererState> state_{D3D11RendererState::Stopped};
    std::atomic<ID3D11Device*> publishedDevice_{nullptr};
    std::atomic_bool deviceLossDetected_{false};
    std::atomic_long deviceLossReason_{S_OK};
    UINT initialWidth_ = 1;
    UINT initialHeight_ = 1;
    HWND initializationCompletionWindow_ = nullptr;
    UINT initializationCompletionMessage_ = 0;
    uint64_t initializationCompletionCookie_ = 0;
    HWND deviceLostNotificationWindow_ = nullptr;
    UINT deviceLostNotificationMessage_ = 0;
    uint64_t deviceLostNotificationCookie_ = 0;
    HWND stopCompletionWindow_ = nullptr;
    UINT stopCompletionMessage_ = 0;
    uint64_t stopCompletionCookie_ = 0;
    // Frame envelopes are allocated through a process-lifetime bounded pool
    // and retired by its independent worker. Keeping only a raw pointer here
    // is intentional: replacing the latest-frame mailbox must never release
    // decoder, AVFrame, or D3D references on the window thread.
    D3D11QueuedVideoFrame* pendingFrame_ = nullptr;
    bool pendingResize_ = false;
    UINT pendingResizeWidth_ = 1;
    UINT pendingResizeHeight_ = 1;
    std::optional<PendingColorPipeline> pendingColorPipeline_;
    std::optional<anvil::playback::SubtitleSettings> pendingSubtitleSettings_;
    std::array<std::optional<PendingUiOverlay>, kUiOverlaySlotCount> pendingUiOverlays_;
    std::optional<PendingUiOverlayPresentation> pendingUiMenuOverlayPresentation_;
    std::optional<bool> pendingDiagnosticsEnabled_;
    bool pendingResetStats_ = false;
    bool pendingClear_ = false;

    // Render-thread-owned transaction state. Once an immediate UI update has
    // been accepted it remains outstanding until a frame is actually
    // presented with that overlay. Resize/cache invalidation may defer the
    // transaction, but must never silently consume it.
    bool uiPresentRequired_ = false;

    // Published statistics have a lock independent from the GPU worker. The
    // render thread only holds it while adding an already-computed snapshot,
    // never across a wait, Present, ResizeBuffers, or immediate-context call.
    std::mutex publishedStatsMutex_;
    D3D11RenderStats publishedRenderStats_;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> hlgSwapChain_;
    // DirectComposition visual tree that binds the composition swap chain to
    // the host HWND. This lets DWM composite the video surface so that GDI
    // sibling overlay windows (e.g. the subtitle menu popup) render correctly
    // on top of it, which a HWND-bound flip-model swap chain would occlude.
    bool useComposition_ = false;
    HANDLE frameLatencyWaitable_ = nullptr;
    bool framePacingLogged_ = false;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
    Microsoft::WRL::ComPtr<IUnknown> hlgCompositionSurface_;
    HANDLE hlgCompositionSurfaceHandle_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hlgBackBuffer_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext1_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> hlgVideoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> hlgVideoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> hlgVideoInputView_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> hlgVideoOutputView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cachedOutputFrame_;
    bool cachedOutputFrameValid_ = false;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psNv12_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psSubtitle_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> subtitleBlend_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> colorConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> doviConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> subtitleConstants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hwSrvY_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hwSrvUV_;
    // DV software path: staging P010 texture + SRVs (separate from hwSrv to
    // avoid disturbing the zero-copy hardware cache).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> yuvTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yuvSrvY_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yuvSrvUV_;
    int yuvTextureW_ = 0;
    int yuvTextureH_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> enhancementYuvTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> enhancementYuvSrvY_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> enhancementYuvSrvUV_;
    int enhancementYuvTextureW_ = 0;
    int enhancementYuvTextureH_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> subtitleTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> subtitleSrv_;
    std::array<UiOverlaySlot, kUiOverlaySlotCount> uiOverlaySlots_;
    std::vector<HardwareSrvCacheEntry> hardwareSrvCache_;
    std::vector<SubtitleTextureCacheEntry> subtitleTextureCache_;
    D3D11_VIEWPORT viewport_{};
    int textureW_ = 0;
    int textureH_ = 0;
    DXGI_FORMAT textureFormat_ = DXGI_FORMAT_UNKNOWN;
    int subtitleTextureW_ = 0;
    int subtitleTextureH_ = 0;
    bool hardwareTextureFailureLogged_ = false;
    anvil::playback::VideoSettings videoSettings_;
    anvil::playback::SubtitleSettings subtitleSettings_;
    anvil::playback::DisplayCapabilities displayCapabilities_;
    anvil::playback::VideoColorMetadata mediaColor_;
    HMONITOR displayPeakMonitor_ = nullptr;
    std::atomic<int> detectedDisplayPeakNits_{0};
    std::atomic<int> effectiveDisplayPeakNits_{1000};
    std::wstring activeSubtitleText_;
    std::wstring activeSubtitleBitmapKey_;
    RECT activeSubtitleViewport_{};
    double activeSubtitleFontScale_ = 0.0;
    int activeSubtitleOffsetXPx_ = 0;
    int activeSubtitleOffsetYPx_ = 0;
    DXGI_COLOR_SPACE_TYPE activeColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    NvidiaHdrOutput nvidiaHdrOutput_;
    bool hdrMetadataApplied_ = false;
    bool suppressHdrMetadataForDolbyVision_ = false;
    uint64_t activeHdrMetadataSignature_ = 0;
    bool hdrColorSpaceFailureLogged_ = false;
    bool hlgColorSpaceFailureLogged_ = false;
    bool hlgStudioColorSpaceSupported_ = false;
    bool hlgFullColorSpaceSupported_ = false;
    bool hlgCompositionSelected_ = false;
    bool dolbyVisionMetadataLogged_ = false;
    bool hdr10PlusMetadataLogged_ = false;
    bool felOverlayLogged_ = false;
    bool subtitleBitmapOverlayLogged_ = false;
    uint64_t subtitleDrawFrame_ = 0;
    bool doviEnabledLastFrame_ = false;  // tracks DV state to skip non-DV updates
    std::wstring activePipelineLabel_;
    uint64_t activePipelineSignature_ = 0;
    bool diagnosticsEnabled_ = false;
    D3D11RenderStats renderStats_;
};

}  // namespace anvil::app
