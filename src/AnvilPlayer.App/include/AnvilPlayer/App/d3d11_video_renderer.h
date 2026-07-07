#pragma once

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/log_sink_ptr.h"
#include "AnvilPlayer/Playback/CapabilityReport.h"
#include "AnvilPlayer/Playback/Settings.h"

#include <d3d11.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <windows.h>

#include <wrl/client.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace anvil::app {

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
    explicit D3D11VideoRenderer(LogSinkPtr logSink = nullptr) : logSink_(std::move(logSink)) {}
    ~D3D11VideoRenderer();
    D3D11VideoRenderer(const D3D11VideoRenderer&) = delete;
    D3D11VideoRenderer& operator=(const D3D11VideoRenderer&) = delete;

    bool Initialize(HWND host);

    void ConfigureColorPipeline(const anvil::playback::VideoSettings& settings,
                                const anvil::playback::DisplayCapabilities& display,
                                const anvil::playback::VideoColorMetadata& mediaColor);
    void ConfigureSubtitleSettings(const anvil::playback::SubtitleSettings& settings);

    void OnResize();

    void SetDiagnosticsEnabled(bool enabled);
    void ResetRenderStats();
    D3D11RenderStats TakeRenderStats();

    void Render(const NativeVideoFrame& frame);

    void Clear();

    ID3D11Device* Device() const {
        return device_.Get();
    }

private:
    void EnableMultithreadProtection();
    bool CreateRenderTarget();
    bool CreatePipeline();
    bool CreateComposition();
    bool UpdateColorPipeline(const NativeVideoFrame& frame);
    void UpdateDoviConstants(const NativeVideoFrame& frame);
    bool ApplySwapChainColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace, const anvil::playback::VideoColorMetadata& color);
    void ApplyHdrMetadata(const anvil::playback::VideoColorMetadata& color);
    bool ConfigureFramePacing();
    void WaitForFrameLatencyObject(bool collectStats);
    void PresentFrame(UINT syncInterval, bool collectStats, std::chrono::steady_clock::time_point stageStart);
    bool UpdateHardwareTexture(const NativeVideoFrame& frame);
    void UpdateTexture(const NativeVideoFrame& frame);
    bool UpdateYuvTexture(const NativeVideoFrame& frame);
    bool UpdateEnhancementYuvTexture(const NativeVideoFrame& frame);
    bool UpdateSubtitleOverlay(const NativeVideoFrame& frame, const D3D11_VIEWPORT& videoViewport);
    void DrawSubtitleOverlay();
    bool DrawSubtitleBitmapOverlays(const NativeVideoFrame& frame, const D3D11_VIEWPORT& videoViewport);
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

    HWND host_ = nullptr;
    LogSinkPtr logSink_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
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
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
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
    std::wstring activeSubtitleText_;
    std::wstring activeSubtitleBitmapKey_;
    RECT activeSubtitleViewport_{};
    double activeSubtitleFontScale_ = 0.0;
    int activeSubtitleOffsetXPx_ = 0;
    int activeSubtitleOffsetYPx_ = 0;
    DXGI_COLOR_SPACE_TYPE activeColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool hdrMetadataApplied_ = false;
    bool hdrColorSpaceFailureLogged_ = false;
    bool dolbyVisionMetadataLogged_ = false;
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
