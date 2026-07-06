#include "AnvilPlayer/App/d3d11_video_renderer.h"

#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/ui_draw.h"

#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

namespace anvil::app {

using anvil::playback::LogLevel;
using anvil::playback::DoviMappingMethod;
using anvil::playback::kDoviMaxPivots;
using anvil::playback::kDoviMaxPieces;
using anvil::playback::kDoviMmrCoeffsPerOrder;
using anvil::playback::kDoviMmrMaxTerms;
using anvil::playback::VideoColorMetadata;
using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

namespace {

struct VideoColorConstants {
    int matrixType = 0;
    int rangeType = 0;
    int transferType = 1;
    int outputMode = 0;
    int primariesType = 0;
    int toneMapMode = 1;
    float sourcePeakNits = 1000.0f;
    float targetPeakNits = 100.0f;
    float displayPeakNits = 0.0f;
    float hdrCurveEnabled = 0.0f;
    float hdrCurvePointCount = 0.0f;
    float reserved0 = 0.0f;
    float hdrToneCurve[anvil::playback::kHdrToneCurvePointCount][4] = {};
};

static_assert(sizeof(VideoColorConstants) % 16 == 0);

// Dolby Vision reshaping constants uploaded to the GPU each frame.
//
// Layout (HLSL cbuffer, must match the shader-side declaration in psNv12Src).
// Packing follows HLSL rules: every float4 element occupies one 16-byte slot.
//
// The reshaping data covers up to 3 components, each with up to 9 pivots
// (8 piece-wise segments). Polynomial segments carry order + 3 coefficients;
// MMR segments carry a constant + up to 3 terms of 7 coefficients.
struct DoviShaderConstants {
    // Per-component pivot values (normalized to [0,1]). 9 pivots packed into
    // 3 float4s per component (3 values + 1 padding each).
    float pivots[3][3][4] = {};

    // Per-piece mapping metadata: x = mapping_idc (0=poly, 1=mmr, -1=none),
    // y = num_pivots for this component, z = poly_order (0/1/2), w = mmr_order (0..3).
    // 8 pieces per component, packed 4-per-float4 → 2 float4s per component.
    float pieceMeta[3][2][4] = {};

    // Polynomial coefficients per piece: xyz = c0,c1,c2; w = unused.
    // 8 pieces per component.
    float polyCoef[3][8][4] = {};

    // MMR coefficients per piece: float4[6] packs constant + 3 terms × 7 coeffs.
    // Layout within the 6 float4s (24 floats):
    //   [0]: mmrConstant, mmr_coef[t0][0..2]
    //   [1]: mmr_coef[t0][3..6]
    //   [2]: mmr_coef[t1][0..3]
    //   [3]: mmr_coef[t1][4..6], pad
    //   [4]: mmr_coef[t2][0..3]
    //   [5]: mmr_coef[t2][4..6], pad
    float mmrCoef[3][8][6][4] = {};

    // Color matrices (row-major 3x3) + offset.
    float yccToRgb[3][4] = {};       // each row: r,g,b + pad
    float yccOffset[4] = {};
    float rgbToLms[3][4] = {};       // each row: r,g,b + pad (CMv4.0, reserved)
    float curveMeta[4] = {};         // xyz = num_pivots per component, w = offset scale

    // Scalars.
    int profile = 0;
    int compatibilityId = 0;
    int enabled = 0;                 // 1 when DV reshaping should be applied
    float sampleScale = 1.0f;         // P010 R16_UNORM -> BL-normalized code range
};

static_assert(sizeof(DoviShaderConstants) % 16 == 0, "DoviShaderConstants must be 16-byte aligned");

uint64_t ElapsedMicroseconds(const std::chrono::steady_clock::time_point start,
                             const std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

VideoColorMetadata MergeColorMetadata(VideoColorMetadata frame, const VideoColorMetadata& fallback) {
    if (frame.primaries == VideoColorPrimaries::Unknown) {
        frame.primaries = fallback.primaries;
    }
    if (frame.transfer == VideoTransferCharacteristic::Unknown) {
        frame.transfer = fallback.transfer;
    }
    if (frame.matrix == VideoMatrixCoefficients::Unknown) {
        frame.matrix = fallback.matrix;
    }
    if (frame.range == VideoColorRange::Unknown) {
        frame.range = fallback.range;
    }
    if (!frame.masteringDisplay.hasPrimaries && fallback.masteringDisplay.hasPrimaries) {
        frame.masteringDisplay.red = fallback.masteringDisplay.red;
        frame.masteringDisplay.green = fallback.masteringDisplay.green;
        frame.masteringDisplay.blue = fallback.masteringDisplay.blue;
        frame.masteringDisplay.whitePoint = fallback.masteringDisplay.whitePoint;
        frame.masteringDisplay.hasPrimaries = true;
    }
    if (!frame.masteringDisplay.hasLuminance && fallback.masteringDisplay.hasLuminance) {
        frame.masteringDisplay.minLuminanceNits = fallback.masteringDisplay.minLuminanceNits;
        frame.masteringDisplay.maxLuminanceNits = fallback.masteringDisplay.maxLuminanceNits;
        frame.masteringDisplay.hasLuminance = true;
    }
    if (!frame.contentLight.hasValues && fallback.contentLight.hasValues) {
        frame.contentLight = fallback.contentLight;
    }
    return frame;
}

int MatrixType(const VideoColorMetadata& color) {
    switch (color.matrix) {
    case VideoMatrixCoefficients::Bt709:
        return 1;
    case VideoMatrixCoefficients::Bt601:
        return 2;
    case VideoMatrixCoefficients::Bt2020Ncl:
    case VideoMatrixCoefficients::Bt2020Cl:
        return 3;
    case VideoMatrixCoefficients::Rgb:
    case VideoMatrixCoefficients::Unknown:
        break;
    }
    return color.primaries == VideoColorPrimaries::Bt2020 ? 3 : 1;
}

int PrimariesType(const VideoColorPrimaries primaries) {
    switch (primaries) {
    case VideoColorPrimaries::Bt709:
        return 1;
    case VideoColorPrimaries::Bt2020:
        return 2;
    case VideoColorPrimaries::DisplayP3:
        return 3;
    case VideoColorPrimaries::Unknown:
        break;
    }
    return 0;
}

int TransferType(const VideoTransferCharacteristic transfer) {
    switch (transfer) {
    case VideoTransferCharacteristic::Pq:
        return 2;
    case VideoTransferCharacteristic::Hlg:
        return 3;
    case VideoTransferCharacteristic::Bt709:
    case VideoTransferCharacteristic::Srgb:
    case VideoTransferCharacteristic::Unknown:
        break;
    }
    return 1;
}

int ToneMapType(const anvil::playback::ToneMappingMode mode) {
    switch (mode) {
    case anvil::playback::ToneMappingMode::Auto:
        return 0;
    case anvil::playback::ToneMappingMode::Balanced:
        return 1;
    case anvil::playback::ToneMappingMode::PreserveHighlights:
        return 2;
    case anvil::playback::ToneMappingMode::BrightRoom:
        return 3;
    }
    return 1;
}

float SourcePeakNits(const VideoColorMetadata& color,
                     const anvil::playback::VideoSettings& settings,
                     const anvil::playback::DolbyVisionFrameMetadata* dovi) {
    if (dovi && dovi->valid && dovi->sourceMaxNits > 1.0f) {
        return std::clamp(dovi->sourceMaxNits, 100.0f, 10000.0f);
    }
    if (color.contentLight.hasValues && color.contentLight.maxContentLightLevelNits > 0) {
        return static_cast<float>(color.contentLight.maxContentLightLevelNits);
    }
    if (color.masteringDisplay.hasLuminance && color.masteringDisplay.maxLuminanceNits > 0.0) {
        return static_cast<float>(color.masteringDisplay.maxLuminanceNits);
    }
    return static_cast<float>(std::max(100, settings.peakBrightnessNits));
}

float ClampHdrToneCurveNits(const double value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp(value, 0.0, 10000.0));
}

float HdrToneCurveOutputPeakNits(const anvil::playback::VideoSettings& settings) {
    if (settings.hdrToneCurve.empty()) {
        return static_cast<float>(std::max(100, settings.peakBrightnessNits));
    }
    return std::max(100.0f, ClampHdrToneCurveNits(settings.hdrToneCurve.back().outputNits));
}

VideoColorMetadata NormalizeDolbyVisionOutput(VideoColorMetadata color, const bool hasDolbyVision) {
    if (!hasDolbyVision) {
        return color;
    }

    // Matches libplacebo/FFmpeg apply_dolbyvision: reshaped DV is BT.2020 PQ,
    // independent of the often-unknown Profile 5 container color tags.
    color.primaries = VideoColorPrimaries::Bt2020;
    color.transfer = VideoTransferCharacteristic::Pq;
    color.matrix = VideoMatrixCoefficients::Rgb;
    color.range = VideoColorRange::Full;
    return color;
}

float DoviTextureSampleScale(const int bitDepth) {
    const int depth = std::clamp(bitDepth, 8, 16);
    const uint64_t codeMax = (uint64_t{1} << depth) - 1;
    const int bitShift = 16 - depth;
    const uint64_t shiftedMax = codeMax << bitShift;
    return shiftedMax > 0
               ? static_cast<float>(65535.0 / static_cast<double>(shiftedMax))
               : 1.0f;
}

float DoviOffsetScale() {
    return static_cast<float>(65536.0 / 65535.0);
}

bool WantsHdrOutput(const VideoColorMetadata& color,
                    const anvil::playback::VideoSettings& settings,
                    const anvil::playback::DisplayCapabilities& display) {
    if (settings.hdrOutput == anvil::playback::HdrOutputMode::ForceSdr) {
        return false;
    }
    if (settings.hdrOutput == anvil::playback::HdrOutputMode::ForceHdr) {
        return display.hdrEnabled || display.hdrSupported;
    }
    if (settings.dolbyVisionHdrOutput && color.IsHdr()) {
        return true;
    }
    return color.IsHdr() && display.hdrEnabled;
}

UINT16 ChromaticityToDxgi(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    return static_cast<UINT16>(std::clamp(value * 50000.0, 0.0, 65535.0) + 0.5);
}

UINT LuminanceToDxgi(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    return static_cast<UINT>(std::clamp(value * 10000.0, 0.0, 4294967295.0) + 0.5);
}

UINT16 LightLevelToDxgi(int value) {
    return static_cast<UINT16>(std::clamp(value, 0, 65535));
}

std::wstring ColorSpaceName(const DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return L"rgb_full_pq_bt2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return L"rgb_full_g22_bt709";
    default:
        return L"dxgi_color_space_" + std::to_wstring(static_cast<int>(colorSpace));
    }
}

std::wstring OutputModeName(const bool hdr) {
    return hdr ? L"hdr10" : L"sdr";
}

RECT ViewportRect(const D3D11_VIEWPORT& viewport) {
    return MakeRect(static_cast<int>(std::round(viewport.TopLeftX)),
                    static_cast<int>(std::round(viewport.TopLeftY)),
                    static_cast<int>(std::round(viewport.TopLeftX + viewport.Width)),
                    static_cast<int>(std::round(viewport.TopLeftY + viewport.Height)));
}

bool RectEquals(const RECT& lhs, const RECT& rhs) {
    return lhs.left == rhs.left &&
           lhs.top == rhs.top &&
           lhs.right == rhs.right &&
           lhs.bottom == rhs.bottom;
}

int CountSubtitleLines(const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(std::count(text.begin(), text.end(), L'\n')) + 1;
}

std::wstring SubtitleBitmapKey(const std::vector<NativeSubtitleBitmap>& bitmaps) {
    if (bitmaps.empty()) {
        return {};
    }

    std::wostringstream stream;
    for (const auto& bitmap : bitmaps) {
        stream << bitmap.serial << L':'
               << bitmap.x << L',' << bitmap.y << L','
               << bitmap.width << L'x' << bitmap.height << L','
               << bitmap.canvasWidth << L'x' << bitmap.canvasHeight << L';';
    }
    return stream.str();
}

uint8_t BlendPremultipliedChannel(const uint8_t source, const uint8_t destination, const uint8_t inverseAlpha) {
    return static_cast<uint8_t>(source + (static_cast<unsigned int>(destination) * inverseAlpha + 127) / 255);
}

uint8_t SampleBilinearChannel(const uint8_t* sourcePixels,
                              const int sourceStride,
                              const int sourceWidth,
                              const int sourceHeight,
                              const double sourceX,
                              const double sourceY,
                              const int channel) {
    const double clampedX = std::clamp(sourceX, 0.0, static_cast<double>(sourceWidth - 1));
    const double clampedY = std::clamp(sourceY, 0.0, static_cast<double>(sourceHeight - 1));
    const int x0 = static_cast<int>(std::floor(clampedX));
    const int y0 = static_cast<int>(std::floor(clampedY));
    const int x1 = std::min(x0 + 1, sourceWidth - 1);
    const int y1 = std::min(y0 + 1, sourceHeight - 1);
    const double tx = clampedX - static_cast<double>(x0);
    const double ty = clampedY - static_cast<double>(y0);

    const uint8_t* p00 = sourcePixels + static_cast<std::size_t>(sourceStride) * y0 + static_cast<std::size_t>(x0) * 4;
    const uint8_t* p10 = sourcePixels + static_cast<std::size_t>(sourceStride) * y0 + static_cast<std::size_t>(x1) * 4;
    const uint8_t* p01 = sourcePixels + static_cast<std::size_t>(sourceStride) * y1 + static_cast<std::size_t>(x0) * 4;
    const uint8_t* p11 = sourcePixels + static_cast<std::size_t>(sourceStride) * y1 + static_cast<std::size_t>(x1) * 4;

    const double top = static_cast<double>(p00[channel]) * (1.0 - tx) + static_cast<double>(p10[channel]) * tx;
    const double bottom = static_cast<double>(p01[channel]) * (1.0 - tx) + static_cast<double>(p11[channel]) * tx;
    const auto value = static_cast<long>(std::lround(top * (1.0 - ty) + bottom * ty));
    return static_cast<uint8_t>(std::clamp<long>(value, 0, 255));
}

void BlendSubtitleBitmap(std::vector<uint8_t>& destination,
                         const int destinationWidth,
                         const int destinationHeight,
                         const NativeSubtitleBitmap& source,
                         const D3D11_VIEWPORT& videoViewport,
                         const int frameWidth,
                         const int frameHeight) {
    if (!source.HasPixels() || destinationWidth <= 0 || destinationHeight <= 0) {
        return;
    }

    const int canvasWidth = source.canvasWidth > 0 ? source.canvasWidth : frameWidth;
    const int canvasHeight = source.canvasHeight > 0 ? source.canvasHeight : frameHeight;
    if (canvasWidth <= 0 || canvasHeight <= 0 || videoViewport.Width <= 0.0f || videoViewport.Height <= 0.0f) {
        return;
    }

    const double scaleX = static_cast<double>(videoViewport.Width) / static_cast<double>(canvasWidth);
    const double scaleY = static_cast<double>(videoViewport.Height) / static_cast<double>(canvasHeight);
    const int destLeft = static_cast<int>(std::round(videoViewport.TopLeftX + static_cast<float>(source.x) * scaleX));
    const int destTop = static_cast<int>(std::round(videoViewport.TopLeftY + static_cast<float>(source.y) * scaleY));
    const int destWidth = std::max(1, static_cast<int>(std::round(static_cast<double>(source.width) * scaleX)));
    const int destHeight = std::max(1, static_cast<int>(std::round(static_cast<double>(source.height) * scaleY)));
    const int clippedLeft = std::clamp(destLeft, 0, destinationWidth);
    const int clippedTop = std::clamp(destTop, 0, destinationHeight);
    const int clippedRight = std::clamp(destLeft + destWidth, 0, destinationWidth);
    const int clippedBottom = std::clamp(destTop + destHeight, 0, destinationHeight);
    if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) {
        return;
    }

    const uint8_t* sourcePixels = source.bgra->data();
    for (int y = clippedTop; y < clippedBottom; ++y) {
        const double sourceY = ((static_cast<double>(y - destTop) + 0.5) *
                                static_cast<double>(source.height) / static_cast<double>(destHeight)) - 0.5;
        for (int x = clippedLeft; x < clippedRight; ++x) {
            const double sourceX = ((static_cast<double>(x - destLeft) + 0.5) *
                                    static_cast<double>(source.width) / static_cast<double>(destWidth)) - 0.5;
            const uint8_t blue = SampleBilinearChannel(sourcePixels, source.stride, source.width, source.height, sourceX, sourceY, 0);
            const uint8_t green = SampleBilinearChannel(sourcePixels, source.stride, source.width, source.height, sourceX, sourceY, 1);
            const uint8_t red = SampleBilinearChannel(sourcePixels, source.stride, source.width, source.height, sourceX, sourceY, 2);
            const uint8_t alpha = SampleBilinearChannel(sourcePixels, source.stride, source.width, source.height, sourceX, sourceY, 3);
            if (alpha == 0) {
                continue;
            }

            uint8_t* destinationPixel = destination.data() +
                                        (static_cast<std::size_t>(destinationWidth) * y + x) * 4;
            const uint8_t inverseAlpha = static_cast<uint8_t>(255 - alpha);
            destinationPixel[0] = BlendPremultipliedChannel(blue, destinationPixel[0], inverseAlpha);
            destinationPixel[1] = BlendPremultipliedChannel(green, destinationPixel[1], inverseAlpha);
            destinationPixel[2] = BlendPremultipliedChannel(red, destinationPixel[2], inverseAlpha);
            destinationPixel[3] = BlendPremultipliedChannel(alpha, destinationPixel[3], inverseAlpha);
        }
    }
}

float SubtitleFontPixels(const D3D11_VIEWPORT& videoViewport, const double fontScale) {
    const float base = std::clamp(videoViewport.Height * 0.048f, 20.0f, 54.0f);
    return base * static_cast<float>(std::clamp(fontScale, 0.5, 2.0));
}

}  // namespace

D3D11VideoRenderer::~D3D11VideoRenderer() {
    ReleaseAll();
}

bool D3D11VideoRenderer::Initialize(HWND host) {
    ReleaseAll();
    host_ = host;
    if (!host_) return false;

    RECT rc{};
    GetClientRect(host_, &rc);
    const UINT width = static_cast<UINT>(std::max(1, RectWidth(rc)));
    const UINT height = static_cast<UINT>(std::max(1, RectHeight(rc)));

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) { LogHr(L"CreateDXGIFactory2", hr); return false; }

    hr = factory_->EnumAdapters(0, &adapter_);
    if (FAILED(hr)) { adapter_.Reset(); }

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    const UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(adapter_.Get(), adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           nullptr, createFlags, featureLevels, static_cast<UINT>(std::size(featureLevels)),
                           D3D11_SDK_VERSION, &device_, &obtained, &context_);
    if (FAILED(hr)) {
        // WARP fallback.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              featureLevels, static_cast<UINT>(std::size(featureLevels)),
                              D3D11_SDK_VERSION, &device_, &obtained, &context_);
        if (FAILED(hr)) { LogHr(L"D3D11CreateDevice", hr); return false; }
    }
    EnableMultithreadProtection();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    hr = factory_->CreateSwapChainForHwnd(device_.Get(), host_, &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) { LogHr(L"CreateSwapChainForHwnd", hr); return false; }

    if (!CreateRenderTarget()) return false;
    if (!CreatePipeline()) return false;
    ApplySwapChainColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, mediaColor_);

    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;
    return true;
}

void D3D11VideoRenderer::ConfigureColorPipeline(const anvil::playback::VideoSettings& settings,
                                                const anvil::playback::DisplayCapabilities& display,
                                                const anvil::playback::VideoColorMetadata& mediaColor) {
    videoSettings_ = settings;
    displayCapabilities_ = display;
    mediaColor_ = mediaColor;
    activePipelineLabel_.clear();
    hdrColorSpaceFailureLogged_ = false;
}

void D3D11VideoRenderer::ConfigureSubtitleSettings(const anvil::playback::SubtitleSettings& settings) {
    subtitleSettings_ = settings;
    activeSubtitleText_.clear();
    activeSubtitleBitmapKey_.clear();
    subtitleSrv_.Reset();
}

void D3D11VideoRenderer::OnResize() {
    if (!swapChain_ || !host_) return;
    RECT rc{};
    GetClientRect(host_, &rc);
    const UINT width = static_cast<UINT>(std::max(1, RectWidth(rc)));
    const UINT height = static_cast<UINT>(std::max(1, RectHeight(rc)));

    backBuffer_.Reset();
    rtv_.Reset();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    if (FAILED(hr)) { LogHr(L"ResizeBuffers", hr); return; }
    CreateRenderTarget();
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
}

void D3D11VideoRenderer::SetDiagnosticsEnabled(const bool enabled) {
    diagnosticsEnabled_ = enabled;
    ResetRenderStats();
}

void D3D11VideoRenderer::ResetRenderStats() {
    renderStats_ = {};
}

D3D11RenderStats D3D11VideoRenderer::TakeRenderStats() {
    const D3D11RenderStats stats = renderStats_;
    renderStats_ = {};
    return stats;
}

void D3D11VideoRenderer::Render(const NativeVideoFrame& frame) {
    if (!device_ || !context_ || !swapChain_) return;
    if (frame.dovi && frame.dovi->valid && !dolbyVisionMetadataLogged_) {
        LogInfo(L"render_begin input=" +
                std::wstring(frame.HasD3DTexture() ? L"d3d11_texture" : (frame.HasYuv() ? L"p010_yuv" : L"bgra")) +
                L" serial=" + std::to_wstring(frame.serial));
    }
    constexpr uint64_t kSlowRenderFrameThresholdUs = 33000;
    const bool collectStats = diagnosticsEnabled_;
    const auto renderStart = collectStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto stageStart = renderStart;

    UpdateColorPipeline(frame);
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.colorPipelineUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    const bool hasHardwareTexture = frame.HasD3DTexture() && UpdateHardwareTexture(frame);
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.hardwarePrepareUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    bool hasBgraTexture = false;
    bool hasYuvTexture = false;
    if (!hasHardwareTexture && frame.HasYuv()) {
        hasYuvTexture = UpdateYuvTexture(frame);
    }
    if (!hasHardwareTexture && !hasYuvTexture && frame.HasPixels()) {
        UpdateTexture(frame);
        hasBgraTexture = srv_ != nullptr;
    }
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.bgraUploadUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    if (!hasHardwareTexture && !hasBgraTexture && !hasYuvTexture) {
        return;
    }

    float clearColor[4] = {0.02f, 0.03f, 0.04f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    const D3D11_VIEWPORT drawViewport = LetterboxedViewport(frame.width, frame.height);
    context_->RSSetViewports(1, &drawViewport);

    const bool useNv12Path = (hasHardwareTexture && hwSrvY_ && hwSrvUV_) ||
                             (hasYuvTexture && yuvSrvY_ && yuvSrvUV_);
    if (useNv12Path && psNv12_) {
        ID3D11ShaderResourceView* yView = hasHardwareTexture ? hwSrvY_.Get() : yuvSrvY_.Get();
        ID3D11ShaderResourceView* uvView = hasHardwareTexture ? hwSrvUV_.Get() : yuvSrvUV_.Get();
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(psNv12_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[2] = {yView, uvView};
        context_->PSSetShaderResources(0, 2, views);
        ID3D11Buffer* constants[2] = {colorConstants_.Get(), doviConstants_.Get()};
        context_->PSSetConstantBuffers(0, 2, constants);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullViews[2] = {};
        context_->PSSetShaderResources(0, 2, nullViews);
    } else if (hasBgraTexture) {
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, srv_.GetAddressOf());
        context_->PSSetConstantBuffers(0, 1, colorConstants_.GetAddressOf());
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullView[1] = {};
        context_->PSSetShaderResources(0, 1, nullView);
    }
    const bool drewSubtitle = UpdateSubtitleOverlay(frame, drawViewport);
    if (drewSubtitle) {
        DrawSubtitleOverlay();
    }
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        renderStats_.subtitleUs += ElapsedMicroseconds(stageStart, now);
        stageStart = now;
    }

    swapChain_->Present(1, 0);
    if (collectStats) {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t presentUs = ElapsedMicroseconds(stageStart, now);
        const uint64_t totalUs = ElapsedMicroseconds(renderStart, now);
        ++renderStats_.frames;
        if (hasHardwareTexture) ++renderStats_.hardwareFrames;
        if (hasBgraTexture) ++renderStats_.bgraFrames;
        if (drewSubtitle) ++renderStats_.subtitleFrames;
        if (totalUs > kSlowRenderFrameThresholdUs) ++renderStats_.slowFrames;
        renderStats_.presentUs += presentUs;
        renderStats_.totalRenderUs += totalUs;
        renderStats_.maxPresentUs = std::max(renderStats_.maxPresentUs, presentUs);
        renderStats_.maxRenderUs = std::max(renderStats_.maxRenderUs, totalUs);
    }
}

void D3D11VideoRenderer::Clear() {
    if (!device_ || !context_ || !swapChain_ || !rtv_) return;
    ResetRenderStats();
    hardwareSrvCache_.clear();
    hwSrvUV_.Reset();
    hwSrvY_.Reset();
    float clearColor[4] = {0.02f, 0.03f, 0.04f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    swapChain_->Present(1, 0);
}

void D3D11VideoRenderer::EnableMultithreadProtection() {
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (device_ && SUCCEEDED(device_.As(&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
        LogInfo(L"multithread protection enabled");
    }
}

bool D3D11VideoRenderer::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) { LogHr(L"GetBuffer", hr); return false; }
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT rtvHr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);
    if (FAILED(rtvHr)) {
        LogHr(L"CreateRenderTargetView", rtvHr);
        return false;
    }
    rtv_ = std::move(rtv);
    backBuffer_ = std::move(backBuffer);
    return true;
}

bool D3D11VideoRenderer::CreatePipeline() {
    const char* vsSrc =
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "  VSOut o;\n"
        "  // Fullscreen triangle without vertex buffer.\n"
        "  o.uv = float2((id << 1) & 2, id & 2);\n"
        "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "  return o;\n"
        "}\n";
    const char* psSrc =
        "Texture2D<float4> tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer ColorConstants : register(b0) {\n"
        "  int matrixType;\n"
        "  int rangeType;\n"
        "  int transferType;\n"
        "  int outputMode;\n"
        "  int primariesType;\n"
        "  int toneMapMode;\n"
        "  float sourcePeakNits;\n"
        "  float targetPeakNits;\n"
        "  float displayPeakNits;\n"
        "  float hdrCurveEnabled;\n"
        "  float hdrCurvePointCount;\n"
        "  float reserved0;\n"
        "  float4 hdrToneCurve[9];\n"
        "};\n"
        "float3 pq_to_nits(float3 v) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 p = pow(saturate(v), 1.0 / m2);\n"
        "  return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);\n"
        "}\n"
        "float3 nits_to_pq(float3 nits) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 y = pow(max(nits / 10000.0, 0.0), m1);\n"
        "  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);\n"
        "}\n"
        "float hdr_curve_luma(float lumaNits) {\n"
        "  if (hdrCurveEnabled < 0.5) return lumaNits;\n"
        "  int count = clamp((int)(hdrCurvePointCount + 0.5), 2, 9);\n"
        "  float2 prev = hdrToneCurve[0].xy;\n"
        "  if (lumaNits <= prev.x) return max(prev.y, 0.0);\n"
        "  for (int i = 1; i < 9; ++i) {\n"
        "    if (i >= count) break;\n"
        "    float2 next = hdrToneCurve[i].xy;\n"
        "    if (lumaNits <= next.x) {\n"
        "      float t = saturate((lumaNits - prev.x) / max(next.x - prev.x, 0.0001));\n"
        "      return max(lerp(prev.y, next.y, t), 0.0);\n"
        "    }\n"
        "    prev = next;\n"
        "  }\n"
        "  return max(prev.y, 0.0);\n"
        "}\n"
        "float3 apply_hdr_tone_curve_pq(float3 pqRgb) {\n"
        "  if (outputMode != 1 || transferType != 2 || primariesType != 2 || hdrCurveEnabled < 0.5) return pqRgb;\n"
        "  float3 nits = max(pq_to_nits(pqRgb), 0.0);\n"
        "  float luma = max(dot(nits, float3(0.2627, 0.6780, 0.0593)), 0.0);\n"
        "  float mapped = hdr_curve_luma(luma);\n"
        "  float scale = mapped / max(luma, 0.0001);\n"
        "  return saturate(nits_to_pq(nits * scale));\n"
        "}\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float4 c = tex.Sample(samp, uv);\n"
        "  return float4(apply_hdr_tone_curve_pq(c.rgb), c.a);\n"
        "}\n";
    const char* psNv12Src =
        "Texture2D<float> texY : register(t0);\n"
        "Texture2D<float2> texUV : register(t1);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer ColorConstants : register(b0) {\n"
        "  int matrixType;\n"
        "  int rangeType;\n"
        "  int transferType;\n"
        "  int outputMode;\n"
        "  int primariesType;\n"
        "  int toneMapMode;\n"
        "  float sourcePeakNits;\n"
        "  float targetPeakNits;\n"
        "  float displayPeakNits;\n"
        "  float hdrCurveEnabled;\n"
        "  float hdrCurvePointCount;\n"
        "  float reserved0;\n"
        "  float4 hdrToneCurve[9];\n"
        "};\n"
        "float3 ycbcr_to_rgb(float y, float2 cbcr) {\n"
        "  float yOffset = rangeType == 1 ? 0.0 : 16.0 / 255.0;\n"
        "  float yScale = rangeType == 1 ? 1.0 : 255.0 / 219.0;\n"
        "  float cScale = rangeType == 1 ? 1.0 : 255.0 / 224.0;\n"
        "  float yy = max(0.0, (y - yOffset) * yScale);\n"
        "  float cb = (cbcr.x - 0.5) * cScale;\n"
        "  float cr = (cbcr.y - 0.5) * cScale;\n"
        "  float kr = 0.2126;\n"
        "  float kb = 0.0722;\n"
        "  if (matrixType == 2) { kr = 0.2990; kb = 0.1140; }\n"
        "  else if (matrixType == 3) { kr = 0.2627; kb = 0.0593; }\n"
        "  float kg = 1.0 - kr - kb;\n"
        "  float3 rgb;\n"
        "  rgb.r = yy + (2.0 - 2.0 * kr) * cr;\n"
        "  rgb.b = yy + (2.0 - 2.0 * kb) * cb;\n"
        "  rgb.g = yy - ((2.0 * kb * (1.0 - kb)) / kg) * cb - ((2.0 * kr * (1.0 - kr)) / kg) * cr;\n"
        "  return max(rgb, 0.0);\n"
        "}\n"
        "float3 pq_to_nits(float3 v) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 p = pow(saturate(v), 1.0 / m2);\n"
        "  return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);\n"
        "}\n"
        "float3 nits_to_pq(float3 nits) {\n"
        "  const float m1 = 2610.0 / 16384.0;\n"
        "  const float m2 = 2523.0 / 32.0;\n"
        "  const float c1 = 3424.0 / 4096.0;\n"
        "  const float c2 = 2413.0 / 128.0;\n"
        "  const float c3 = 2392.0 / 128.0;\n"
        "  float3 y = pow(max(nits / 10000.0, 0.0), m1);\n"
        "  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);\n"
        "}\n"
        "float hdr_curve_luma(float lumaNits) {\n"
        "  if (hdrCurveEnabled < 0.5) return lumaNits;\n"
        "  int count = clamp((int)(hdrCurvePointCount + 0.5), 2, 9);\n"
        "  float2 prev = hdrToneCurve[0].xy;\n"
        "  if (lumaNits <= prev.x) return max(prev.y, 0.0);\n"
        "  for (int i = 1; i < 9; ++i) {\n"
        "    if (i >= count) break;\n"
        "    float2 next = hdrToneCurve[i].xy;\n"
        "    if (lumaNits <= next.x) {\n"
        "      float t = saturate((lumaNits - prev.x) / max(next.x - prev.x, 0.0001));\n"
        "      return max(lerp(prev.y, next.y, t), 0.0);\n"
        "    }\n"
        "    prev = next;\n"
        "  }\n"
        "  return max(prev.y, 0.0);\n"
        "}\n"
        "float3 apply_hdr_tone_curve_nits(float3 bt2020Nits) {\n"
        "  float3 nits = max(bt2020Nits, 0.0);\n"
        "  if (hdrCurveEnabled < 0.5) return saturate(nits_to_pq(nits));\n"
        "  float luma = max(dot(nits, float3(0.2627, 0.6780, 0.0593)), 0.0);\n"
        "  float mapped = hdr_curve_luma(luma);\n"
        "  float scale = mapped / max(luma, 0.0001);\n"
        "  return saturate(nits_to_pq(nits * scale));\n"
        "}\n"
        "float3 apply_hdr_tone_curve_pq(float3 pqRgb) {\n"
        "  if (hdrCurveEnabled < 0.5) return saturate(pqRgb);\n"
        "  return apply_hdr_tone_curve_nits(pq_to_nits(pqRgb));\n"
        "}\n"
        "float3 hlg_to_nits(float3 v) {\n"
        "  const float a = 0.17883277;\n"
        "  const float b = 0.28466892;\n"
        "  const float c = 0.55991073;\n"
        "  float3 low = (v * v) / 3.0;\n"
        "  float3 high = (exp((v - c) / a) + b) / 12.0;\n"
        "  float3 scene = lerp(high, low, step(v, float3(0.5, 0.5, 0.5)));\n"
        "  return max(scene, 0.0) * max(sourcePeakNits, 100.0);\n"
        "}\n"
        "float3 bt2020_to_bt709(float3 c) {\n"
        "  return float3(dot(float3(1.6605, -0.5876, -0.0728), c),\n"
        "                dot(float3(-0.1246, 1.1329, -0.0083), c),\n"
        "                dot(float3(-0.0182, -0.1006, 1.1187), c));\n"
        "}\n"
        "float3 bt709_to_bt2020(float3 c) {\n"
        "  return float3(dot(float3(0.6274, 0.3293, 0.0433), c),\n"
        "                dot(float3(0.0691, 0.9195, 0.0114), c),\n"
        "                dot(float3(0.0164, 0.0880, 0.8956), c));\n"
        "}\n"
        "float3 dovi_lms_to_bt2020(float3 lms) {\n"
        "  return float3(dot(float3(3.0644188, -2.1659768, 0.1015582), lms),\n"
        "                dot(float3(-0.6561211, 1.7855412, -0.1294375), lms),\n"
        "                dot(float3(0.0173632, -0.0472515, 1.0300425), lms));\n"
        "}\n"
        "float3 encoded_to_nits(float3 rgb) {\n"
        "  if (transferType == 2) return pq_to_nits(rgb);\n"
        "  if (transferType == 3) return hlg_to_nits(rgb);\n"
        "  return pow(saturate(rgb), 2.2) * 100.0;\n"
        "}\n"
        "float tone_map_exposure() {\n"
        "  if (toneMapMode == 2) return 0.72;\n"
        "  if (toneMapMode == 3) return 0.95;\n"
        "  return 0.80;\n"
        "}\n"
        "float tone_map_knee(float target) {\n"
        "  if (toneMapMode == 2) return target * 0.40;\n"
        "  if (toneMapMode == 3) return target * 0.55;\n"
        "  return target * 0.45;\n"
        "}\n"
        "float tone_map_luma_to_target(float lumaNits) {\n"
        "  float target = max(targetPeakNits, 1.0);\n"
        "  float source = max(sourcePeakNits * tone_map_exposure(), target + 1.0);\n"
        "  float knee = clamp(tone_map_knee(target), 1.0, target * 0.98);\n"
        "  float y = max(lumaNits, 0.0);\n"
        "  if (y <= knee) return y;\n"
        "  float shoulder = max(target - knee, 0.0001);\n"
        "  float peak = max(source, y);\n"
        "  float x = min(y, peak) - knee;\n"
        "  float mapped = knee + shoulder * log(1.0 + x / shoulder) / log(1.0 + (peak - knee) / shoulder);\n"
        "  return min(mapped, target);\n"
        "}\n"
        "float3 scale_luma_to_sdr(float3 nits, float3 lumaWeights) {\n"
        "  float exposure = tone_map_exposure();\n"
        "  float3 working = max(nits, 0.0) * exposure;\n"
        "  float luma = max(dot(working, lumaWeights), 0.0);\n"
        "  float mappedLuma = tone_map_luma_to_target(luma);\n"
        "  float scale = mappedLuma / max(luma, 0.0001);\n"
        "  return working * scale / max(targetPeakNits, 1.0);\n"
        "}\n"
        "float3 compress_gamut_preserve_luma(float3 linear709) {\n"
        "  float luma = saturate(dot(linear709, float3(0.2126, 0.7152, 0.0722)));\n"
        "  float3 chroma = linear709 - float3(luma, luma, luma);\n"
        "  float scale = 1.0;\n"
        "  if (chroma.r > 0.0) scale = min(scale, (1.0 - luma) / chroma.r);\n"
        "  else if (chroma.r < 0.0) scale = min(scale, -luma / chroma.r);\n"
        "  if (chroma.g > 0.0) scale = min(scale, (1.0 - luma) / chroma.g);\n"
        "  else if (chroma.g < 0.0) scale = min(scale, -luma / chroma.g);\n"
        "  if (chroma.b > 0.0) scale = min(scale, (1.0 - luma) / chroma.b);\n"
        "  else if (chroma.b < 0.0) scale = min(scale, -luma / chroma.b);\n"
        "  scale = saturate(scale);\n"
        "  return saturate(float3(luma, luma, luma) + chroma * scale);\n"
        "}\n"
        "float3 encode_sdr_g22(float3 linearRgb) {\n"
        "  return pow(saturate(linearRgb), 1.0 / 2.2);\n"
        "}\n"
        "float3 bt2020_nits_to_sdr(float3 bt2020Nits) {\n"
        "  float3 linear2020 = scale_luma_to_sdr(bt2020Nits, float3(0.2627, 0.6780, 0.0593));\n"
        "  float3 linear709 = bt2020_to_bt709(linear2020);\n"
        "  return encode_sdr_g22(compress_gamut_preserve_luma(linear709));\n"
        "}\n"
        "float3 bt709_nits_to_sdr(float3 bt709Nits) {\n"
        "  float3 linear709 = scale_luma_to_sdr(bt709Nits, float3(0.2126, 0.7152, 0.0722));\n"
        "  return encode_sdr_g22(compress_gamut_preserve_luma(linear709));\n"
        "}\n"
        // ---- Dolby Vision reshaping (register b1) ----
        // Constants packed to match DoviShaderConstants on the C++ side.
        "cbuffer DoviConstants : register(b1) {\n"
        "  float4 pivots[3][3];\n"
        "  float4 pieceMeta[3][2];\n"
        "  float4 polyCoef[3][8];\n"
        "  float4 mmrCoef[3][8][6];\n"
        "  float4 yccToRgb[3];\n"
        "  float4 yccOffset;\n"
        "  float4 rgbToLms[3];\n"
        "  float4 curveMeta;\n"
        "  int doviProfile;\n"
        "  int doviCompatId;\n"
        "  int doviEnabled;\n"
        "  float doviSampleScale;\n"
        "};\n"
        "float3 dovi_ycc_to_rgb(float3 ycc) {\n"
        "  return float3(dot(yccToRgb[0].xyz, ycc),\n"
        "                dot(yccToRgb[1].xyz, ycc),\n"
        "                dot(yccToRgb[2].xyz, ycc));\n"
        "}\n"
        "float3 dovi_rgb_to_lms(float3 rgb) {\n"
        "  return float3(dot(rgbToLms[0].xyz, rgb),\n"
        "                dot(rgbToLms[1].xyz, rgb),\n"
        "                dot(rgbToLms[2].xyz, rgb));\n"
        "}\n"
        // Read pivot i for component c. Returns 1.0 for out-of-range indices.
        "float dovi_pivot(int c, int i) {\n"
        "  if (i >= 9) return 1.0;\n"
        "  int slot = i / 3;\n"
        "  int comp = i % 3;\n"
        "  return pivots[c][slot][comp];\n"
        "}\n"
        "int dovi_num_pivots(int c) {\n"
        "  return clamp((int)(curveMeta[c] + 0.5), 2, 9);\n"
        "}\n"
        "float dovi_offset_scale() {\n"
        "  return curveMeta.w > 0.0 ? curveMeta.w : 1.0;\n"
        "}\n"
        // Read mapping method for piece s of component c. Returns -1 if none.
        "int dovi_method(int c, int s) {\n"
        "  if (s >= 8) return -1;\n"
        "  int slot = s / 4;\n"
        "  int comp = s % 4;\n"
        "  return (int)pieceMeta[c][slot][comp];\n"
        "}\n"
        // Find the piece index whose [pivot[i], pivot[i+1]) range contains v.
        "int dovi_find_piece(int c, float v, int numPivots) {\n"
        "  int lastPiece = max(0, numPivots - 2);\n"
        "  for (int i = 0; i < 8; ++i) {\n"
        "    if (i >= lastPiece) return lastPiece;\n"
        "    float nextPivot = (i + 1 <= 8) ? dovi_pivot(c, i + 1) : 1.0;\n"
        "    if (v < nextPivot) return i;\n"
        "  }\n"
        "  return lastPiece;\n"
        "}\n"
        // Polynomial reshape: y = c0 + c1*x + c2*x^2.
        "float dovi_reshape_poly(int c, int s, float x) {\n"
        "  float3 coef = polyCoef[c][s].xyz;\n"
        "  return coef.x + coef.y * x + coef.z * x * x;\n"
        "}\n"
        // MMR reshape: needs the full reshaped RGB signal. Operates on the
        // per-component target using up to 3 terms of 7 coefficients over the
        // 3 RGB channels + constant. (Coefficients packed in mmrCoef[c][s][0..5].)
        "float dovi_reshape_mmr(int c, int s, float3 rgb) {\n"
        "  float m[24] = (float[24])0;\n"
        "  for (int f = 0; f < 6; ++f) {\n"
        "    m[f*4+0] = mmrCoef[c][s][f].x;\n"
        "    m[f*4+1] = mmrCoef[c][s][f].y;\n"
        "    m[f*4+2] = mmrCoef[c][s][f].z;\n"
        "    m[f*4+3] = mmrCoef[c][s][f].w;\n"
        "  }\n"
        "  int order = clamp((int)(polyCoef[c][s].w + 0.5), 1, 3);\n"
        "  float4 sigX = float4(rgb.x * rgb.y, rgb.x * rgb.z, rgb.y * rgb.z, rgb.x * rgb.y * rgb.z);\n"
        "  float result = m[0];\n"
        "  result += dot(float3(m[1], m[2], m[3]), rgb);\n"
        "  result += dot(float4(m[4], m[5], m[6], m[7]), sigX);\n"
        "  if (order >= 2) {\n"
        "    float3 rgb2 = rgb * rgb;\n"
        "    float4 sigX2 = sigX * sigX;\n"
        "    result += dot(float3(m[8], m[9], m[10]), rgb2);\n"
        "    result += dot(float4(m[11], m[12], m[13], m[14]), sigX2);\n"
        "    if (order >= 3) {\n"
        "      result += dot(float3(m[15], m[16], m[17]), rgb2 * rgb);\n"
        "      result += dot(float4(m[18], m[19], m[20], m[21]), sigX2 * sigX);\n"
        "    }\n"
        "  }\n"
        "  return result;\n"
        "}\n"
        // Apply the RPU reshaping curve to a (I, Ct, Cp) triplet in the
        // signaled PQ-domain sample space. The result is still PQ encoded and
        // still in Dolby's intermediate RGB/LMS-oriented representation; it
        // must be PQ-linearized and run through rgbToLms + LMS->BT.2020 before
        // presentation.
        // Order (per libplacebo pl_shader_dovi_reshape + pl_color_repr_decode):
        //   1. piece-wise reshape per component (poly or MMR), input is the
        //      raw signaled value; pivots are in the same signaled domain.
        //   2. subtract yccOffset (nonlinear neutral), then multiply by
        //      yccToRgb. (Equivalent to matrix*(x-offset); libplacebo folds
        //      the offset into the matrix constant term as -(M*offset).)
        "float3 dovi_reshape(float3 ipt) {\n"
        "  float3 outRgb = ipt;\n"
        "  for (int c = 0; c < 3; ++c) {\n"
        "    int numPivots = dovi_num_pivots(c);\n"
        "    float v = ipt[c];\n"
        "    int s = dovi_find_piece(c, v, numPivots);\n"
        "    int method = dovi_method(c, s);\n"
        "    if (method == 0) {\n"
        "      outRgb[c] = dovi_reshape_poly(c, s, v);\n"
        "    } else if (method == 1) {\n"
        "      outRgb[c] = dovi_reshape_mmr(c, s, ipt);\n"
        "    }\n"
        "    outRgb[c] = clamp(outRgb[c], dovi_pivot(c, 0), dovi_pivot(c, numPivots - 1));\n"
        "  }\n"
        "  float3 shifted = outRgb - yccOffset.xyz * dovi_offset_scale();\n"
        "  float3 pqRgb = dovi_ycc_to_rgb(shifted);\n"
        "  return pqRgb;\n"
        "}\n"
        "float3 dovi_to_bt2020_nits(float3 ipt) {\n"
        "  float3 pqRgb = dovi_reshape(ipt);\n"
        "  float3 linearRgb = pq_to_nits(pqRgb) / 10000.0;\n"
        "  float3 lms = dovi_rgb_to_lms(linearRgb);\n"
        "  return dovi_lms_to_bt2020(lms) * 10000.0;\n"
        "}\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float y = texY.Sample(samp, uv);\n"
        "  float2 cbcr = texUV.Sample(samp, uv);\n"
        "  // Dolby Vision path: the input YUV is IPT-PQ encoded (not standard\n"
        "  // YCbCr). Apply per-frame RPU reshaping to recover BT.2020 PQ RGB,\n"
        "  // then output as HDR10 PQ or tone-map to SDR.\n"
        "  if (doviEnabled) {\n"
        "    float3 ipt = saturate(float3(y, cbcr.x, cbcr.y) * doviSampleScale);\n"
        "    float3 bt2020Nits = dovi_to_bt2020_nits(ipt);\n"
        "    if (outputMode == 1) {\n"
        "      return float4(apply_hdr_tone_curve_nits(bt2020Nits), 1.0);\n"
        "    }\n"
        "    return float4(bt2020_nits_to_sdr(bt2020Nits), 1.0);\n"
        "  }\n"
        "  float3 rgb = ycbcr_to_rgb(y, cbcr);\n"
        "  if (outputMode == 1) {\n"
        "    if (transferType == 2 && primariesType == 2) return float4(apply_hdr_tone_curve_pq(rgb), 1.0);\n"
        "    float3 nits = encoded_to_nits(rgb);\n"
        "    if (primariesType == 1) nits = bt709_to_bt2020(nits);\n"
        "    return float4(apply_hdr_tone_curve_nits(nits), 1.0);\n"
        "  }\n"
        "  if (transferType == 2 || transferType == 3) {\n"
        "    float3 nits = encoded_to_nits(rgb);\n"
        "    if (primariesType == 2) return float4(bt2020_nits_to_sdr(nits), 1.0);\n"
        "    return float4(bt709_nits_to_sdr(nits), 1.0);\n"
        "  }\n"
        "  if (primariesType == 2) {\n"
        "    float3 linear709 = bt2020_to_bt709(pow(saturate(rgb), 2.2));\n"
        "    return float4(encode_sdr_g22(compress_gamut_preserve_luma(linear709)), 1.0);\n"
        "  }\n"
        "  return float4(saturate(rgb), 1.0);\n"
        "}\n";

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psNv12Blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(vsSrc, static_cast<SIZE_T>(std::strlen(vsSrc)), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errors))) {
        LogHr(L"D3DCompile vs", E_FAIL);
        return false;
    }
    if (FAILED(D3DCompile(psSrc, static_cast<SIZE_T>(std::strlen(psSrc)), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errors))) {
        LogHr(L"D3DCompile ps", E_FAIL);
        return false;
    }
    if (FAILED(D3DCompile(psNv12Src, static_cast<SIZE_T>(std::strlen(psNv12Src)), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psNv12Blob, &errors))) {
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0) {
            const std::string errText(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            LogInfo(L"D3DCompile ps nv12 errors: " + Utf8ToWide(errText.c_str()));
        }
        LogHr(L"D3DCompile ps nv12", E_FAIL);
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_))) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_))) return false;
    if (FAILED(device_->CreatePixelShader(psNv12Blob->GetBufferPointer(), psNv12Blob->GetBufferSize(), nullptr, &psNv12_))) return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_))) return false;

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blendDesc, &subtitleBlend_))) return false;

    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(VideoColorConstants);
    constantsDesc.Usage = D3D11_USAGE_DEFAULT;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&constantsDesc, nullptr, &colorConstants_))) return false;

    D3D11_BUFFER_DESC doviDesc{};
    doviDesc.ByteWidth = sizeof(DoviShaderConstants);
    doviDesc.Usage = D3D11_USAGE_DEFAULT;
    doviDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const HRESULT doviHr = device_->CreateBuffer(&doviDesc, nullptr, &doviConstants_);
    if (FAILED(doviHr)) {
        LogHr(L"CreateBuffer dovi", doviHr);
        return false;
    }
    return true;
}

bool D3D11VideoRenderer::UpdateColorPipeline(const NativeVideoFrame& frame) {
    if (!context_ || !colorConstants_) {
        return false;
    }

    const VideoColorMetadata color = NormalizeDolbyVisionOutput(
        MergeColorMetadata(frame.color, mediaColor_),
        frame.dovi && frame.dovi->valid);
    bool hdrOutput = WantsHdrOutput(color, videoSettings_, displayCapabilities_);
    DXGI_COLOR_SPACE_TYPE colorSpace = hdrOutput
                                           ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                           : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (!ApplySwapChainColorSpace(colorSpace, color) && hdrOutput) {
        hdrOutput = false;
        colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        ApplySwapChainColorSpace(colorSpace, color);
    }

    VideoColorConstants constants;
    constants.matrixType = MatrixType(color);
    constants.rangeType = color.range == VideoColorRange::Full ? 1 : 0;
    constants.transferType = TransferType(color.transfer);
    constants.outputMode = hdrOutput ? 1 : 0;
    constants.primariesType = PrimariesType(color.primaries);
    constants.toneMapMode = ToneMapType(videoSettings_.toneMapping);
    constants.sourcePeakNits = SourcePeakNits(color, videoSettings_, frame.dovi.get());
    const bool hdrToneCurveEnabled = hdrOutput && videoSettings_.dolbyVisionHdrOutput;
    constants.targetPeakNits = hdrOutput
                                    ? (hdrToneCurveEnabled
                                           ? HdrToneCurveOutputPeakNits(videoSettings_)
                                           : std::max(100.0f, static_cast<float>(displayCapabilities_.reportedPeakBrightnessNits)))
                                    : 100.0f;
    constants.displayPeakNits = static_cast<float>(std::max(0, displayCapabilities_.reportedPeakBrightnessNits));
    constants.hdrCurveEnabled = hdrToneCurveEnabled ? 1.0f : 0.0f;
    constants.hdrCurvePointCount = static_cast<float>(videoSettings_.hdrToneCurve.size());
    float previousInput = 0.0f;
    float previousOutput = 0.0f;
    for (std::size_t index = 0; index < videoSettings_.hdrToneCurve.size(); ++index) {
        float input = ClampHdrToneCurveNits(index < anvil::playback::kDefaultHdrToneCurve.size()
                                                ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                                : videoSettings_.hdrToneCurve[index].inputNits);
        float output = ClampHdrToneCurveNits(videoSettings_.hdrToneCurve[index].outputNits);
        if (index == 0) {
            input = 0.0f;
            output = 0.0f;
        } else {
            input = std::max(input, previousInput + 0.001f);
            output = std::max(output, previousOutput);
        }
        constants.hdrToneCurve[index][0] = input;
        constants.hdrToneCurve[index][1] = output;
        previousInput = input;
        previousOutput = output;
    }
    context_->UpdateSubresource(colorConstants_.Get(), 0, nullptr, &constants, 0, 0);
    // Only update the (large) DV constants buffer when DV metadata is actually
    // present; non-DV frames keep the previous disabled state.
    if (frame.dovi && frame.dovi->valid) {
        UpdateDoviConstants(frame);
    } else if (doviEnabledLastFrame_) {
        // One-shot transition: disable on the frame after DV ends.
        UpdateDoviConstants(frame);
        doviEnabledLastFrame_ = false;
    }

    const bool hasDolbyVision = frame.dovi && frame.dovi->valid;
    const std::wstring label =
        std::wstring(L"input=") + (frame.HasD3DTexture() ? L"d3d11_texture" : (frame.HasYuv() ? L"p010_yuv" : L"bgra")) +
        L" output=" + OutputModeName(hdrOutput) +
        L" color_space=" + ColorSpaceName(colorSpace) +
        L" primaries=" + anvil::playback::ToDisplayString(color.primaries) +
        L" transfer=" + anvil::playback::ToDisplayString(color.transfer) +
        L" matrix=" + anvil::playback::ToDisplayString(color.matrix) +
        L" range=" + anvil::playback::ToDisplayString(color.range) +
        L" tone_mapping=" + anvil::playback::ToDisplayString(videoSettings_.toneMapping) +
        (hdrOutput ? L" hdr_curve_peak=" + std::to_wstring(static_cast<int>(std::round(constants.targetPeakNits))) : L"") +
        L" dolby_vision=" + (hasDolbyVision ? L"present" : L"none") +
        (hasDolbyVision
             ? L" dv_reshape=true st2084_correction=true tone_map=luma_log_bt2446_fit gamut=luma_preserving"
             : L"");
    if (label != activePipelineLabel_) {
        activePipelineLabel_ = label;
        LogInfo(label);
    }
    // Log DV reshaping metadata once per stream (first frame carrying it).
    if (frame.dovi && frame.dovi->valid && !dolbyVisionMetadataLogged_) {
        dolbyVisionMetadataLogged_ = true;
        std::wostringstream ss;
        ss << L"dolby_vision_metadata profile=" << frame.dovi->profile
           << L" bl_bit_depth=" << frame.dovi->blBitDepth
           << L" vdr_bit_depth=" << frame.dovi->vdrBitDepth
           << L" full_range=" << (frame.dovi->blVideoFullRange ? 1 : 0)
           << L" coef_log2_denom=" << frame.dovi->coefLog2Denom
           << L" source_max_pq=" << frame.dovi->sourceMaxPq
           << L" source_max_nits=" << static_cast<int>(std::round(frame.dovi->sourceMaxNits))
           << L" pivots=";
        for (int c = 0; c < anvil::playback::kDoviNumComponents; ++c) {
            ss << L"[" << frame.dovi->curves[c].numPivots << L"]";
        }
        ss << L" methods=";
        for (int c = 0; c < anvil::playback::kDoviNumComponents; ++c) {
            int poly = 0, mmr = 0;
            const int pieces = std::max(0, frame.dovi->curves[c].numPivots - 1);
            for (int s = 0; s < pieces; ++s) {
                if (frame.dovi->curves[c].pieces[s].method == anvil::playback::DoviMappingMethod::Polynomial) ++poly;
                else if (frame.dovi->curves[c].pieces[s].method == anvil::playback::DoviMappingMethod::Mmr) ++mmr;
            }
            ss << L"[" << poly << L"p/" << mmr << L"m]";
        }
        LogInfo(ss.str());
    }
    return true;
}

void D3D11VideoRenderer::UpdateDoviConstants(const NativeVideoFrame& frame) {
    DoviShaderConstants dc{};

    if (!frame.dovi || !frame.dovi->valid || !doviConstants_) {
        // No DV metadata: disable reshaping in the shader.
        context_->UpdateSubresource(doviConstants_.Get(), 0, nullptr, &dc, 0, 0);
        return;
    }

    const auto& dovi = *frame.dovi;

    for (int c = 0; c < 3; ++c) {
        const auto& curve = dovi.curves[c];
        const int numPivots = std::min(curve.numPivots, static_cast<int>(kDoviMaxPivots));
        dc.curveMeta[c] = static_cast<float>(std::max(2, numPivots));
        // Pivots: pack 9 values into 3 float4s (3 per float4, padding the 4th).
        for (int p = 0; p < 9; ++p) {
            const int slot = p / 3;
            const int comp = p % 3;
            dc.pivots[c][slot][comp] = (p < numPivots) ? curve.pivots[p] : 1.0f;
        }
        const int numPieces = std::max(0, numPivots - 1);
        // pieceMeta: per-piece mapping_idc / poly_order / mmr_order, plus num_pivots.
        // Packed 4-per-float4: [piece0_idc, piece1_idc, piece2_idc, piece3_idc] etc.
        for (int slot = 0; slot < 2; ++slot) {
            for (int comp = 0; comp < 4; ++comp) {
                const int pieceIdx = slot * 4 + comp;
                if (pieceIdx >= numPieces) {
                    dc.pieceMeta[c][slot][comp] = -1.0f;
                } else {
                    const auto& piece = curve.pieces[pieceIdx];
                    dc.pieceMeta[c][slot][comp] = static_cast<float>(static_cast<int>(piece.method));
                }
            }
        }
        // Polynomial coefficients per piece (xyz = c0,c1,c2; w = poly_order).
        for (int s = 0; s < kDoviMaxPieces; ++s) {
            if (s < numPieces && curve.pieces[s].method == DoviMappingMethod::Polynomial) {
                dc.polyCoef[c][s][0] = curve.pieces[s].polyCoef[0];
                dc.polyCoef[c][s][1] = curve.pieces[s].polyCoef[1];
                dc.polyCoef[c][s][2] = curve.pieces[s].polyCoef[2];
                dc.polyCoef[c][s][3] = static_cast<float>(curve.pieces[s].polyOrder);
            } else {
                dc.polyCoef[c][s][3] = 0.0f;
            }
        }
        // MMR coefficients per piece: pack constant + 3 terms × 7 coeffs into 6 float4s (24 floats).
        // [t0_c0..t0_c6] [t1_c0..t1_c6] [t2_c0..t2_c6] conceptually; here laid out linearly.
        for (int s = 0; s < kDoviMaxPieces; ++s) {
            if (s < numPieces && curve.pieces[s].method == DoviMappingMethod::Mmr) {
                const auto& piece = curve.pieces[s];
                dc.polyCoef[c][s][3] = static_cast<float>(std::clamp(piece.mmrOrder, 1, kDoviMmrMaxTerms));
                float flat[24] = {};
                flat[0] = piece.mmrConstant;
                int idx = 1;
                for (int t = 0; t < piece.mmrOrder && t < kDoviMmrMaxTerms; ++t) {
                    for (int k = 0; k < kDoviMmrCoeffsPerOrder; ++k) {
                        if (idx < 24) flat[idx] = piece.mmrCoef[t][k];
                        ++idx;
                    }
                }
                for (int f = 0; f < 6; ++f) {
                    dc.mmrCoef[c][s][f][0] = flat[f * 4 + 0];
                    dc.mmrCoef[c][s][f][1] = flat[f * 4 + 1];
                    dc.mmrCoef[c][s][f][2] = flat[f * 4 + 2];
                    dc.mmrCoef[c][s][f][3] = flat[f * 4 + 3];
                }
            }
        }
    }

    // Color matrices (row-major 3x3 packed into float4[3]).
    for (int r = 0; r < 3; ++r) {
        dc.yccToRgb[r][0] = dovi.yccToRgb[r * 3 + 0];
        dc.yccToRgb[r][1] = dovi.yccToRgb[r * 3 + 1];
        dc.yccToRgb[r][2] = dovi.yccToRgb[r * 3 + 2];
        dc.rgbToLms[r][0] = dovi.rgbToLms[r * 3 + 0];
        dc.rgbToLms[r][1] = dovi.rgbToLms[r * 3 + 1];
        dc.rgbToLms[r][2] = dovi.rgbToLms[r * 3 + 2];
    }
    dc.yccOffset[0] = dovi.yccOffset[0];
    dc.yccOffset[1] = dovi.yccOffset[1];
    dc.yccOffset[2] = dovi.yccOffset[2];

    dc.profile = dovi.profile;
    dc.compatibilityId = dovi.compatibilityId;
    dc.enabled = 1;
    dc.sampleScale = DoviTextureSampleScale(dovi.blBitDepth);
    dc.curveMeta[3] = DoviOffsetScale();

    context_->UpdateSubresource(doviConstants_.Get(), 0, nullptr, &dc, 0, 0);
    doviEnabledLastFrame_ = true;
}

bool D3D11VideoRenderer::ApplySwapChainColorSpace(const DXGI_COLOR_SPACE_TYPE colorSpace, const VideoColorMetadata& color) {
    if (!swapChain_) {
        return false;
    }
    if (activeColorSpace_ == colorSpace) {
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && !hdrMetadataApplied_) {
            ApplyHdrMetadata(color);
        }
        return true;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain_.As(&swapChain3)) || !swapChain3) {
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || !hdrColorSpaceFailureLogged_) {
            LogInfo(L"color_space unsupported reason=swapchain3_unavailable");
        }
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdrColorSpaceFailureLogged_ = true;
        }
        return false;
    }

    UINT support = 0;
    HRESULT hr = swapChain3->CheckColorSpaceSupport(colorSpace, &support);
    if (FAILED(hr) || (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || !hdrColorSpaceFailureLogged_) {
            LogInfo(L"color_space unsupported target=" + ColorSpaceName(colorSpace));
        }
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdrColorSpaceFailureLogged_ = true;
        }
        return false;
    }

    hr = swapChain3->SetColorSpace1(colorSpace);
    if (FAILED(hr)) {
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || !hdrColorSpaceFailureLogged_) {
            LogHr(L"SetColorSpace1 " + ColorSpaceName(colorSpace), hr);
        }
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdrColorSpaceFailureLogged_ = true;
        }
        return false;
    }

    activeColorSpace_ = colorSpace;
    hdrMetadataApplied_ = false;
    if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        ApplyHdrMetadata(color);
    } else {
        Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain4;
        if (SUCCEEDED(swapChain_.As(&swapChain4)) && swapChain4) {
            swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
        }
    }
    LogInfo(L"color_space active=" + ColorSpaceName(colorSpace));
    return true;
}

void D3D11VideoRenderer::ApplyHdrMetadata(const VideoColorMetadata& color) {
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain4;
    if (!swapChain_ || FAILED(swapChain_.As(&swapChain4)) || !swapChain4) {
        return;
    }

    DXGI_HDR_METADATA_HDR10 metadata{};
    const auto& mastering = color.masteringDisplay;
    const auto setPrimary = [](UINT16 (&target)[2], const anvil::playback::ChromaticityPoint& point) {
        target[0] = ChromaticityToDxgi(point.x);
        target[1] = ChromaticityToDxgi(point.y);
    };

    if (mastering.hasPrimaries) {
        setPrimary(metadata.RedPrimary, mastering.red);
        setPrimary(metadata.GreenPrimary, mastering.green);
        setPrimary(metadata.BluePrimary, mastering.blue);
        setPrimary(metadata.WhitePoint, mastering.whitePoint);
    } else {
        metadata.RedPrimary[0] = ChromaticityToDxgi(0.708);
        metadata.RedPrimary[1] = ChromaticityToDxgi(0.292);
        metadata.GreenPrimary[0] = ChromaticityToDxgi(0.170);
        metadata.GreenPrimary[1] = ChromaticityToDxgi(0.797);
        metadata.BluePrimary[0] = ChromaticityToDxgi(0.131);
        metadata.BluePrimary[1] = ChromaticityToDxgi(0.046);
        metadata.WhitePoint[0] = ChromaticityToDxgi(0.3127);
        metadata.WhitePoint[1] = ChromaticityToDxgi(0.3290);
    }

    const bool curveActive = videoSettings_.dolbyVisionHdrOutput;
    const double curvePeak = curveActive
                                 ? static_cast<double>(HdrToneCurveOutputPeakNits(videoSettings_))
                                 : 10000.0;
    const double sourceMaxMastering = mastering.hasLuminance && mastering.maxLuminanceNits > 0.0
                                          ? mastering.maxLuminanceNits
                                          : static_cast<double>(std::max(100, videoSettings_.peakBrightnessNits));
    const double maxMastering = curveActive
                                    ? std::clamp(sourceMaxMastering, 100.0, curvePeak)
                                    : sourceMaxMastering;
    const double minMastering = mastering.hasLuminance && mastering.minLuminanceNits > 0.0
                                    ? mastering.minLuminanceNits
                                    : 0.0001;
    metadata.MaxMasteringLuminance = LuminanceToDxgi(maxMastering);
    metadata.MinMasteringLuminance = LuminanceToDxgi(minMastering);
    metadata.MaxContentLightLevel = LightLevelToDxgi(static_cast<int>(std::round(
        color.contentLight.hasValues
            ? std::min(static_cast<double>(color.contentLight.maxContentLightLevelNits), curvePeak)
            : maxMastering)));
    metadata.MaxFrameAverageLightLevel = LightLevelToDxgi(static_cast<int>(std::round(
        color.contentLight.hasValues
            ? std::min(static_cast<double>(color.contentLight.maxFrameAverageLightLevelNits), curvePeak)
            : maxMastering / 2.0)));

    const HRESULT hr = swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata);
    if (SUCCEEDED(hr)) {
        hdrMetadataApplied_ = true;
    }
}

bool D3D11VideoRenderer::UpdateHardwareTexture(const NativeVideoFrame& frame) {
    constexpr std::size_t kMaxHardwareSrvCacheEntries = 32;

    hwSrvY_.Reset();
    hwSrvUV_.Reset();
    if (!frame.HasD3DTexture()) {
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    frame.d3dTexture->GetDesc(&textureDesc);
    if (frame.d3dArraySlice >= textureDesc.ArraySize) {
        LogHardwareTextureFailureOnce(L"invalid_array_slice");
        return false;
    }

    for (const auto& cached : hardwareSrvCache_) {
        if (cached.texture.Get() == frame.d3dTexture.Get() &&
            cached.arraySlice == frame.d3dArraySlice &&
            cached.sourceFormat == textureDesc.Format &&
            cached.y &&
            cached.uv) {
            hwSrvY_ = cached.y;
            hwSrvUV_ = cached.uv;
            if (diagnosticsEnabled_) {
                ++renderStats_.hardwareSrvCacheHits;
            }
            return true;
        }
    }

    DXGI_FORMAT yFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uvFormat = DXGI_FORMAT_UNKNOWN;
    if (textureDesc.Format == DXGI_FORMAT_NV12) {
        yFormat = DXGI_FORMAT_R8_UNORM;
        uvFormat = DXGI_FORMAT_R8G8_UNORM;
    } else if (textureDesc.Format == DXGI_FORMAT_P010 || textureDesc.Format == DXGI_FORMAT_P016) {
        yFormat = DXGI_FORMAT_R16_UNORM;
        uvFormat = DXGI_FORMAT_R16G16_UNORM;
    } else {
        LogHardwareTextureFailureOnce(L"unsupported_hardware_texture_format");
        return false;
    }
    if (diagnosticsEnabled_) {
        ++renderStats_.hardwareSrvCacheMisses;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC yDesc{};
    yDesc.Format = yFormat;
    D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc{};
    uvDesc.Format = uvFormat;
    if (textureDesc.ArraySize > 1) {
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        yDesc.Texture2DArray.MostDetailedMip = 0;
        yDesc.Texture2DArray.MipLevels = 1;
        yDesc.Texture2DArray.FirstArraySlice = frame.d3dArraySlice;
        yDesc.Texture2DArray.ArraySize = 1;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        uvDesc.Texture2DArray.MostDetailedMip = 0;
        uvDesc.Texture2DArray.MipLevels = 1;
        uvDesc.Texture2DArray.FirstArraySlice = frame.d3dArraySlice;
        uvDesc.Texture2DArray.ArraySize = 1;
    } else {
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yDesc.Texture2D.MostDetailedMip = 0;
        yDesc.Texture2D.MipLevels = 1;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvDesc.Texture2D.MostDetailedMip = 0;
        uvDesc.Texture2D.MipLevels = 1;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvView;
    const HRESULT yHr = device_->CreateShaderResourceView(frame.d3dTexture.Get(), &yDesc, &yView);
    if (FAILED(yHr)) {
        LogHardwareTextureFailureOnce(L"CreateShaderResourceView Y hr=0x" + HexHr(yHr));
        return false;
    }
    const HRESULT uvHr = device_->CreateShaderResourceView(frame.d3dTexture.Get(), &uvDesc, &uvView);
    if (FAILED(uvHr)) {
        LogHardwareTextureFailureOnce(L"CreateShaderResourceView UV hr=0x" + HexHr(uvHr));
        return false;
    }

    hwSrvY_ = yView;
    hwSrvUV_ = uvView;
    if (hardwareSrvCache_.size() >= kMaxHardwareSrvCacheEntries) {
        hardwareSrvCache_.erase(hardwareSrvCache_.begin());
    }
    hardwareSrvCache_.push_back(HardwareSrvCacheEntry{
        frame.d3dTexture,
        frame.d3dArraySlice,
        textureDesc.Format,
        std::move(yView),
        std::move(uvView),
    });
    return true;
}

void D3D11VideoRenderer::UpdateTexture(const NativeVideoFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4) return;
    const DXGI_FORMAT uploadFormat = frame.softwareFormat == AV_PIX_FMT_X2BGR10LE
                                         ? DXGI_FORMAT_R10G10B10A2_UNORM
                                         : DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool needsRecreate = !texture_ ||
                               textureW_ != frame.width ||
                               textureH_ != frame.height ||
                               textureFormat_ != uploadFormat;
    if (needsRecreate) {
        texture_.Reset();
        srv_.Reset();
        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = static_cast<UINT>(frame.width);
        tdesc.Height = static_cast<UINT>(frame.height);
        tdesc.MipLevels = 1;
        tdesc.ArraySize = 1;
        tdesc.Format = uploadFormat;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DYNAMIC;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&tdesc, nullptr, &texture_))) {
            LogHr(L"CreateTexture2D", E_FAIL);
            return;
        }
        textureW_ = frame.width;
        textureH_ = frame.height;
        textureFormat_ = uploadFormat;
        if (FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_))) {
            texture_.Reset();
            textureFormat_ = DXGI_FORMAT_UNKNOWN;
            return;
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    const int dstStride = static_cast<int>(mapped.RowPitch);
    const int srcStride = frame.stride;
    const uint8_t* src = frame.bgra->data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    if (dstStride == srcStride) {
        std::memcpy(dst, src, static_cast<std::size_t>(srcStride) * static_cast<std::size_t>(frame.height));
    } else {
        const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 4;
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(dstStride) * y,
                        src + static_cast<std::size_t>(srcStride) * y,
                        rowBytes);
        }
    }
    context_->Unmap(texture_.Get(), 0);
}

bool D3D11VideoRenderer::UpdateYuvTexture(const NativeVideoFrame& frame) {
    if (!frame.HasYuv() || !device_ || !context_) {
        return false;
    }
    const int w = frame.yuv.width;
    const int h = frame.yuv.height;
    if (w <= 0 || h <= 0) {
        return false;
    }

    // (Re)create the staging P010 texture if dimensions changed.
    // P010 = NV12 layout with 16-bit samples: Y plane (R16) + interleaved UV
    // plane (R16G16). Plane selection is implied by the compatible SRV format
    // (R16_UNORM for Y, R16G16_UNORM for UV), not by array slices.
    const bool needsRecreate = !yuvTexture_ || yuvTextureW_ != w || yuvTextureH_ != h;
    if (needsRecreate) {
        yuvTexture_.Reset();
        yuvSrvY_.Reset();
        yuvSrvUV_.Reset();
        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = static_cast<UINT>(w);
        tdesc.Height = static_cast<UINT>(h);
        tdesc.MipLevels = 1;
        tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_P010;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage = D3D11_USAGE_DYNAMIC;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&tdesc, nullptr, &yuvTexture_))) {
            LogHr(L"CreateTexture2D P010", E_FAIL);
            return false;
        }
        yuvTextureW_ = w;
        yuvTextureH_ = h;

        D3D11_SHADER_RESOURCE_VIEW_DESC yDesc{};
        yDesc.Format = DXGI_FORMAT_R16_UNORM;
        yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yDesc.Texture2D.MostDetailedMip = 0;
        yDesc.Texture2D.MipLevels = 1;
        HRESULT srvHr = device_->CreateShaderResourceView(yuvTexture_.Get(), &yDesc, &yuvSrvY_);
        if (FAILED(srvHr)) {
            LogHr(L"CreateShaderResourceView P010 Y", srvHr);
            yuvTexture_.Reset();
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc{};
        uvDesc.Format = DXGI_FORMAT_R16G16_UNORM;
        uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvDesc.Texture2D.MostDetailedMip = 0;
        uvDesc.Texture2D.MipLevels = 1;
        srvHr = device_->CreateShaderResourceView(yuvTexture_.Get(), &uvDesc, &yuvSrvUV_);
        if (FAILED(srvHr)) {
            LogHr(L"CreateShaderResourceView P010 UV", srvHr);
            yuvTexture_.Reset();
            yuvSrvY_.Reset();
            return false;
        }
    }

    // Upload packed Y + interleaved UV into the P010 texture.
    // P010 layout: width bytes-per-row = w*2 (Y), with UV plane starting at
    // row h (each UV row also w*2 bytes, half as many rows). The mapped
    // subresource exposes both planes contiguously via RowPitch.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(yuvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const int dstStride = static_cast<int>(mapped.RowPitch);
    const int yStride = frame.yuv.yStride;
    const int uvStride = frame.yuv.uvStride;
    const int uvHeight = h / 2;
    const uint8_t* src = frame.yuv.data->data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

    // Y plane.
    if (dstStride == yStride) {
        std::memcpy(dst, src, static_cast<std::size_t>(yStride) * h);
    } else {
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(dstStride) * y,
                        src + static_cast<std::size_t>(yStride) * y,
                        yStride);
        }
    }
    // UV plane (offset dst by dstStride*h rows, src by yStride*h bytes).
    uint8_t* uvDst = dst + static_cast<std::size_t>(dstStride) * h;
    const uint8_t* uvSrc = src + static_cast<std::size_t>(yStride) * h;
    if (dstStride == uvStride) {
        std::memcpy(uvDst, uvSrc, static_cast<std::size_t>(uvStride) * uvHeight);
    } else {
        for (int y = 0; y < uvHeight; ++y) {
            std::memcpy(uvDst + static_cast<std::size_t>(dstStride) * y,
                        uvSrc + static_cast<std::size_t>(uvStride) * y,
                        uvStride);
        }
    }
    context_->Unmap(yuvTexture_.Get(), 0);
    return true;
}

bool D3D11VideoRenderer::UpdateSubtitleOverlay(const NativeVideoFrame& frame, const D3D11_VIEWPORT& videoViewport) {
    const std::wstring& text = frame.subtitleText;
    const auto& bitmaps = frame.subtitleBitmaps;
    const std::wstring bitmapKey = SubtitleBitmapKey(bitmaps);
    if (!device_ || !context_ || (text.empty() && bitmaps.empty()) || viewport_.Width <= 0.0f || viewport_.Height <= 0.0f) {
        activeSubtitleText_.clear();
        activeSubtitleBitmapKey_.clear();
        return false;
    }

    const RECT videoRect = ViewportRect(videoViewport);
    const bool textChanged = text != activeSubtitleText_;
    const bool bitmapsChanged = bitmapKey != activeSubtitleBitmapKey_;
    const bool bitmapBecameActive = bitmapsChanged && activeSubtitleBitmapKey_.empty() && !bitmapKey.empty();
    if (subtitleSrv_ &&
        text == activeSubtitleText_ &&
        bitmapKey == activeSubtitleBitmapKey_ &&
        RectEquals(videoRect, activeSubtitleViewport_) &&
        std::abs(activeSubtitleFontScale_ - subtitleSettings_.fontScale) < 0.001) {
        return true;
    }
    if (diagnosticsEnabled_) {
        ++renderStats_.subtitleSurfaceRebuilds;
    }

    const int surfaceWidth = std::max(1, static_cast<int>(std::round(viewport_.Width)));
    const int surfaceHeight = std::max(1, static_cast<int>(std::round(viewport_.Height)));
    Gdiplus::Bitmap bitmap(surfaceWidth, surfaceHeight, PixelFormat32bppPARGB);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const int lineCount = CountSubtitleLines(text);
    if (!text.empty()) {
        const float fontPixels = SubtitleFontPixels(videoViewport, subtitleSettings_.fontScale);
        const float maxTextWidth = std::max(1.0f, videoViewport.Width * 0.84f);
        const float marginBottom = std::clamp(videoViewport.Height * 0.085f, 22.0f, 86.0f);
        const float layoutHeight = std::min(videoViewport.Height * 0.34f,
                                            std::max(fontPixels * 2.1f, fontPixels * (static_cast<float>(lineCount) + 1.2f)));
        const float layoutLeft = videoViewport.TopLeftX + (videoViewport.Width - maxTextWidth) * 0.5f;
        const float layoutTop = std::max(videoViewport.TopLeftY,
                                         videoViewport.TopLeftY + videoViewport.Height - marginBottom - layoutHeight);
        Gdiplus::RectF layout(layoutLeft, layoutTop, maxTextWidth, layoutHeight);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentFar);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);

        Gdiplus::GraphicsPath textPath;
        textPath.AddString(text.c_str(),
                           -1,
                           &family,
                           Gdiplus::FontStyleBold,
                           fontPixels,
                           layout,
                           &format);
        const float outlineWidth = std::clamp(fontPixels * 0.16f, 3.0f, 8.0f);
        Gdiplus::Pen outline(Gdiplus::Color(220, 0, 0, 0), outlineWidth);
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush fill(Gdiplus::Color(245, 255, 255, 255));
        graphics.DrawPath(&outline, &textPath);
        graphics.FillPath(&fill, &textPath);
    }

    Gdiplus::BitmapData bitmapData{};
    Gdiplus::Rect lockRect(0, 0, surfaceWidth, surfaceHeight);
    if (bitmap.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bitmapData) != Gdiplus::Ok) {
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<std::size_t>(surfaceWidth) * static_cast<std::size_t>(surfaceHeight) * 4);
    const auto* source = static_cast<const uint8_t*>(bitmapData.Scan0);
    const int sourceStride = bitmapData.Stride;
    for (int y = 0; y < surfaceHeight; ++y) {
        const uint8_t* sourceRow = sourceStride >= 0
                                       ? source + static_cast<std::size_t>(sourceStride) * y
                                       : source + static_cast<std::size_t>(-sourceStride) * (surfaceHeight - 1 - y);
        std::memcpy(pixels.data() + static_cast<std::size_t>(surfaceWidth) * 4 * y,
                    sourceRow,
                    static_cast<std::size_t>(surfaceWidth) * 4);
    }
    bitmap.UnlockBits(&bitmapData);

    for (const auto& subtitleBitmap : bitmaps) {
        BlendSubtitleBitmap(pixels, surfaceWidth, surfaceHeight, subtitleBitmap, videoViewport, frame.width, frame.height);
    }

    if (!subtitleTexture_ || subtitleTextureW_ != surfaceWidth || subtitleTextureH_ != surfaceHeight || !subtitleSrv_) {
        subtitleTexture_.Reset();
        subtitleSrv_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(surfaceWidth);
        desc.Height = static_cast<UINT>(surfaceHeight);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &subtitleTexture_))) {
            LogHr(L"CreateTexture2D subtitle", E_FAIL);
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(subtitleTexture_.Get(), nullptr, &subtitleSrv_))) {
            subtitleTexture_.Reset();
            LogHr(L"CreateShaderResourceView subtitle", E_FAIL);
            return false;
        }
        subtitleTextureW_ = surfaceWidth;
        subtitleTextureH_ = surfaceHeight;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(subtitleTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const int destinationStride = static_cast<int>(mapped.RowPitch);
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    const int sourcePitch = surfaceWidth * 4;
    for (int y = 0; y < surfaceHeight; ++y) {
        std::memcpy(destination + static_cast<std::size_t>(destinationStride) * y,
                    pixels.data() + static_cast<std::size_t>(sourcePitch) * y,
                    static_cast<std::size_t>(sourcePitch));
    }
    context_->Unmap(subtitleTexture_.Get(), 0);

    activeSubtitleText_ = text;
    activeSubtitleBitmapKey_ = bitmapKey;
    activeSubtitleViewport_ = videoRect;
    activeSubtitleFontScale_ = subtitleSettings_.fontScale;
    if (textChanged || bitmapBecameActive) {
        LogInfo(L"subtitle_overlay active=true lines=" + std::to_wstring(lineCount) +
                L" bitmap_rects=" + std::to_wstring(bitmaps.size()) +
                L" surface=" + std::to_wstring(surfaceWidth) + L"x" + std::to_wstring(surfaceHeight));
    }
    return true;
}

void D3D11VideoRenderer::DrawSubtitleOverlay() {
    if (!context_ || !subtitleSrv_ || !subtitleBlend_ || !vs_ || !ps_) {
        return;
    }

    context_->RSSetViewports(1, &viewport_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, subtitleSrv_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    float blendFactor[4] = {};
    context_->OMSetBlendState(subtitleBlend_.Get(), blendFactor, 0xffffffff);
    context_->Draw(3, 0);
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    ID3D11ShaderResourceView* nullView[1] = {};
    context_->PSSetShaderResources(0, 1, nullView);
}

void D3D11VideoRenderer::ReleaseAll() {
    ResetRenderStats();
    hardwareSrvCache_.clear();
    subtitleSrv_.Reset();
    subtitleTexture_.Reset();
    hwSrvUV_.Reset();
    hwSrvY_.Reset();
    yuvSrvUV_.Reset();
    yuvSrvY_.Reset();
    yuvTexture_.Reset();
    yuvTextureW_ = 0;
    yuvTextureH_ = 0;
    srv_.Reset();
    texture_.Reset();
    textureW_ = 0;
    textureH_ = 0;
    textureFormat_ = DXGI_FORMAT_UNKNOWN;
    colorConstants_.Reset();
    doviConstants_.Reset();
    subtitleBlend_.Reset();
    sampler_.Reset();
    psNv12_.Reset();
    ps_.Reset();
    vs_.Reset();
    rtv_.Reset();
    backBuffer_.Reset();
    swapChain_.Reset();
    context_.Reset();
    adapter_.Reset();
    device_.Reset();
    factory_.Reset();
    hardwareTextureFailureLogged_ = false;
    subtitleTextureW_ = 0;
    subtitleTextureH_ = 0;
    activeSubtitleText_.clear();
    activeSubtitleBitmapKey_.clear();
    activeSubtitleViewport_ = {};
    activeSubtitleFontScale_ = 0.0;
    activeColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    hdrMetadataApplied_ = false;
    hdrColorSpaceFailureLogged_ = false;
    activePipelineLabel_.clear();
}

void D3D11VideoRenderer::LogHardwareTextureFailureOnce(const std::wstring& message) {
    if (hardwareTextureFailureLogged_) {
        return;
    }
    hardwareTextureFailureLogged_ = true;
    LogHr(L"hardware texture " + message, E_FAIL);
}

void D3D11VideoRenderer::LogInfo(const std::wstring& message) const {
    if (logSink_) {
        logSink_->Write(LogLevel::Debug, L"d3d11", message);
    }
    OutputDebugStringW((L"[d3d11] " + message + L"\n").c_str());
}

void D3D11VideoRenderer::LogHr(const std::wstring& what, HRESULT hr) const {
    std::wostringstream ss;
    ss << what << L" hr=0x" << std::hex << static_cast<unsigned long>(hr);
    const std::wstring msg = ss.str();
    if (logSink_) {
        logSink_->Write(LogLevel::Error, L"d3d11", msg);
    }
    OutputDebugStringW((L"[d3d11] " + msg + L"\n").c_str());
}

D3D11_VIEWPORT D3D11VideoRenderer::LetterboxedViewport(const int sourceWidth, const int sourceHeight) const {
    if (sourceWidth <= 0 || sourceHeight <= 0 || viewport_.Width <= 0.0f || viewport_.Height <= 0.0f) {
        return viewport_;
    }

    const float scale = std::min(viewport_.Width / static_cast<float>(sourceWidth),
                                 viewport_.Height / static_cast<float>(sourceHeight));
    const float width = std::max(1.0f, std::round(static_cast<float>(sourceWidth) * scale));
    const float height = std::max(1.0f, std::round(static_cast<float>(sourceHeight) * scale));

    D3D11_VIEWPORT result = viewport_;
    result.TopLeftX = std::round((viewport_.Width - width) * 0.5f);
    result.TopLeftY = std::round((viewport_.Height - height) * 0.5f);
    result.Width = width;
    result.Height = height;
    return result;
}

}  // namespace anvil::app
