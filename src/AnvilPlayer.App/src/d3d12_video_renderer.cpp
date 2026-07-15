#include "AnvilPlayer/App/d3d12_video_renderer.h"

#include "AnvilPlayer/App/frame_interpolation_policy.h"
#include "AnvilPlayer/App/playback_timing_math.h"
#include "AnvilPlayer/App/software_yuv_upload_layout.h"
#include "AnvilPlayer/App/string_util.h"

#include <gdiplus.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <new>
#include <sstream>
#include <utility>

namespace anvil::app {
namespace {

using anvil::playback::HdrOutputMode;
using anvil::playback::LogLevel;
using anvil::playback::ToneMappingMode;
using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

// Match the known-good D3D11 presentation contract: shaders write display-ready
// G22/BT.709 or PQ/BT.2020 into an RGB10 swap chain. The libplacebo bridge keeps
// its scRGB intermediate and is converted explicitly before presentation.
constexpr DXGI_FORMAT kCompositionFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kLibplaceboTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr uint64_t k8KPixelCount = 7680ull * 4320ull;
constexpr DWORD kFrameLatencyWaitTimeoutMs = 8;

int SanitizeDisplayPeakNits(const int value) noexcept {
    return value > 0 ? std::clamp(value, 100, 10000) : 0;
}

int QueryMonitorPeakNits(IDXGIFactory4* factory, const HMONITOR monitor) {
    if (!factory || !monitor) return 0;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC outputDescription{};
            if (!output || FAILED(output->GetDesc(&outputDescription)) ||
                outputDescription.Monitor != monitor) {
                continue;
            }
            Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 description{};
            if (FAILED(output.As(&output6)) ||
                FAILED(output6->GetDesc1(&description)) ||
                !std::isfinite(description.MaxLuminance)) {
                return 0;
            }
            return SanitizeDisplayPeakNits(
                static_cast<int>(std::lround(description.MaxLuminance)));
        }
    }
    return 0;
}

double EffectiveRefreshRate(const DWORD nominalFrequency) noexcept {
    // EnumDisplaySettings reports NTSC-compatible modes using nominal integer
    // labels. Restore their effective rates before enforcing the fixed 2x
    // refresh-rate ceiling.
    switch (nominalFrequency) {
    case 23: return 24000.0 / 1001.0;
    case 29: return 30000.0 / 1001.0;
    case 47: return 48000.0 / 1001.0;
    case 59: return 60000.0 / 1001.0;
    case 71: return 72000.0 / 1001.0;
    case 95: return 96000.0 / 1001.0;
    case 119: return 120000.0 / 1001.0;
    default: return static_cast<double>(nominalFrequency);
    }
}

struct CompositionConstants {
    float sourceUv[4]{};
    float enhancementSourceUv[4]{};
    UINT matrix = 1;
    UINT range = 0;
    UINT transfer = 0;
    UINT primaries = 0;
    float hdrOutput = 0.0f;
    float sourcePeakNits = 1000.0f;
    float targetPeakNits = 100.0f;
    float padding = 0.0f;
    float hdr10PlusA[4]{};
    float hdr10PlusB[4]{};
    float hdr10PlusCurve[16]{};
    // Packed as five float4 values: (input, output) pairs 0..8. The final
    // float2 is unused so the layout matches HLSL constant-buffer packing.
    float hdrToneCurve[20]{};
};

struct TensorCompositionConstants {
    UINT tensorWidth = 0;
    UINT tensorHeight = 0;
    float hdrOutput = 0.0f;
    UINT transfer = 1;
    float endpointReference[4]{};
};

static_assert(sizeof(TensorCompositionConstants) == 8 * sizeof(UINT));

static_assert(sizeof(CompositionConstants) == 60 * sizeof(UINT));

UINT MatrixMode(const VideoMatrixCoefficients matrix) {
    switch (matrix) {
    case VideoMatrixCoefficients::Bt601:
        return 2;
    case VideoMatrixCoefficients::Bt2020Ncl:
    case VideoMatrixCoefficients::Bt2020Cl:
        return 3;
    default:
        return 1;
    }
}

UINT TransferMode(const VideoTransferCharacteristic transfer) {
    if (transfer == VideoTransferCharacteristic::Pq) return 2;
    if (transfer == VideoTransferCharacteristic::Hlg) return 3;
    return 1;
}

UINT ToneMappingModeBits(const ToneMappingMode mode) noexcept {
    UINT value = 1;
    switch (mode) {
    case ToneMappingMode::Auto:
        value = 0;
        break;
    case ToneMappingMode::Balanced:
        value = 1;
        break;
    case ToneMappingMode::PreserveHighlights:
        value = 2;
        break;
    case ToneMappingMode::BrightRoom:
        value = 3;
        break;
    }
    // modes.w bit 0 is high bit depth and bit 1 is BT.2020. Preserve that
    // layout and carry the user tone-map selection in bits 2-3.
    return value << 2;
}

int CountSubtitleLines(const std::wstring& text) noexcept {
    return text.empty()
        ? 0
        : static_cast<int>(std::count(text.begin(), text.end(), L'\n')) + 1;
}

bool SameViewport(const D3D12_VIEWPORT& lhs, const D3D12_VIEWPORT& rhs) noexcept {
    return lhs.TopLeftX == rhs.TopLeftX && lhs.TopLeftY == rhs.TopLeftY &&
           lhs.Width == rhs.Width && lhs.Height == rhs.Height;
}

D3D12_VIEWPORT AspectFitVideoViewport(const int videoWidth, const int videoHeight,
                                      const UINT outputWidth,
                                      const UINT outputHeight) noexcept {
    const float sourceAspect = static_cast<float>(std::max(1, videoWidth)) /
        static_cast<float>(std::max(1, videoHeight));
    const float outputAspect = static_cast<float>(std::max<UINT>(1, outputWidth)) /
        static_cast<float>(std::max<UINT>(1, outputHeight));
    D3D12_VIEWPORT viewport{};
    if (sourceAspect > outputAspect) {
        viewport.Width = static_cast<float>(outputWidth);
        viewport.Height = viewport.Width / sourceAspect;
        viewport.TopLeftY = (static_cast<float>(outputHeight) - viewport.Height) * 0.5f;
    } else {
        viewport.Height = static_cast<float>(outputHeight);
        viewport.Width = viewport.Height * sourceAspect;
        viewport.TopLeftX = (static_cast<float>(outputWidth) - viewport.Width) * 0.5f;
    }
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    return viewport;
}

float SourcePeakNits(const anvil::playback::VideoColorMetadata& color) {
    if (color.masteringDisplay.hasLuminance && color.masteringDisplay.maxLuminanceNits > 0.0) {
        return static_cast<float>(std::clamp(color.masteringDisplay.maxLuminanceNits, 100.0, 10000.0));
    }
    if (color.contentLight.hasValues && color.contentLight.maxContentLightLevelNits > 0) {
        return static_cast<float>(std::clamp(color.contentLight.maxContentLightLevelNits, 100, 10000));
    }
    return color.IsHdr() ? 1000.0f : 100.0f;
}

const anvil::playback::DolbyVisionFrameMetadata* FrameDolbyVisionMetadata(
    const NativeVideoFrame& frame) noexcept {
    if (frame.enhancementDovi && frame.enhancementDovi->valid) {
        return frame.enhancementDovi.get();
    }
    return frame.dovi && frame.dovi->valid ? frame.dovi.get() : nullptr;
}

bool FrameCarriesHdr(const NativeVideoFrame& frame) noexcept {
    return frame.color.IsHdr() || FrameDolbyVisionMetadata(frame) != nullptr;
}

float FrameSourcePeakNits(const NativeVideoFrame& frame) noexcept {
    const auto* dovi = FrameDolbyVisionMetadata(frame);
    return dovi && dovi->sourceMaxNits > 0.0f
        ? dovi->sourceMaxNits
        : SourcePeakNits(frame.color);
}

float TargetPeakNits(const anvil::playback::VideoSettings& settings,
                     const anvil::playback::DisplayCapabilities& display) noexcept {
    if (settings.displayPeakBrightnessNits >= 100) {
        return static_cast<float>(settings.displayPeakBrightnessNits);
    }
    if (display.reportedPeakBrightnessNits >= 100) {
        return static_cast<float>(display.reportedPeakBrightnessNits);
    }
    // Windows can publish Advanced Color before the asynchronous DXGI output
    // probe has completed. A 100-nit fallback silently turns HDR into SDR;
    // use the documented application default instead.
    return 1000.0f;
}

const wchar_t* DoviTrimSourceName(const DoviDisplayTrimSource source) noexcept {
    switch (source) {
    case DoviDisplayTrimSource::Level2:
        return L"L2_compat";
    case DoviDisplayTrimSource::Level3:
        return L"L3_cmv4";
    case DoviDisplayTrimSource::Level8:
        return L"L8_cmv4";
    default:
        return L"none";
    }
}

float SampleHdr10PlusBezier(const Hdr10PlusFrameMetadata& metadata,
                            const float position) noexcept {
    const int anchorCount = std::min<int>(metadata.anchorCount, 15);
    const int degree = anchorCount + 1;
    std::array<double, 17> points{};
    for (int index = 0; index < anchorCount; ++index) {
        points[index + 1] = std::clamp(
            static_cast<double>(metadata.bezierAnchors[index]), 0.0, 1.0);
    }
    points[degree] = 1.0;
    const double t = std::clamp(static_cast<double>(position), 0.0, 1.0);
    for (int level = degree; level > 0; --level) {
        for (int index = 0; index < level; ++index) {
            points[index] += (points[index + 1] - points[index]) * t;
        }
    }
    return static_cast<float>(std::clamp(points[0], 0.0, 1.0));
}

void FillHdr10PlusConstants(CompositionConstants& constants,
                            const NativeVideoFrame& frame,
                            const bool enabled) noexcept {
    const Hdr10PlusFrameMetadata* metadata = frame.hdr10Plus.get();
    if (!enabled || !metadata || !metadata->valid || !metadata->toneMappingPresent) return;
    constants.hdr10PlusA[0] = 1.0f;
    constants.hdr10PlusA[1] = std::max(metadata->targetedPeakNits, 1.0f);
    constants.hdr10PlusA[2] = std::max(metadata->sourcePeakNits, 1.0f);
    constants.hdr10PlusA[3] = std::clamp(metadata->kneePointX, 0.0001f, 0.9999f);
    constants.hdr10PlusB[0] = std::clamp(metadata->kneePointY, 0.0001f, 0.9999f);
    constants.hdr10PlusB[1] = static_cast<float>(std::min<int>(metadata->anchorCount, 15));
    constants.hdr10PlusB[2] = std::max(metadata->saturationWeight, 0.0f);
    for (std::size_t index = 0; index < std::size(constants.hdr10PlusCurve); ++index) {
        constants.hdr10PlusCurve[index] = SampleHdr10PlusBezier(
            *metadata, static_cast<float>(index) /
                static_cast<float>(std::size(constants.hdr10PlusCurve) - 1));
    }
}

uint64_t FillHdrToneCurveConstants(float (&packedCurve)[20],
                                   float& processingFlags,
                                   const NativeVideoFrame& frame,
                                   const anvil::playback::VideoSettings& settings,
                                   const bool hdrOutput,
                                   const bool forceAppSideDisplayMapping = false) noexcept {
    const bool dolbyVision = FrameDolbyVisionMetadata(frame) != nullptr;
    const bool hdr10Plus = frame.hdr10Plus && frame.hdr10Plus->valid;
    if (!hdrOutput || !frame.color.IsHdr() || dolbyVision || hdr10Plus ||
        (settings.displayMetadataPassthrough && !forceAppSideDisplayMapping)) {
        return 0;
    }

    double previousInput = 0.0;
    double previousOutput = 0.0;
    for (std::size_t index = 0; index < settings.hdrToneCurve.size(); ++index) {
        const auto& source = settings.hdrToneCurve[index];
        const double input = index == 0
            ? 0.0
            : std::clamp(std::max(
                  anvil::playback::kDefaultHdrToneCurve[index].inputNits,
                  previousInput + 0.001), 0.0, 10000.0);
        const double output = index == 0
            ? 0.0
            : std::clamp(std::max(source.outputNits, previousOutput), 0.0, 10000.0);
        packedCurve[index * 2] = static_cast<float>(input);
        packedCurve[index * 2 + 1] = static_cast<float>(output);
        previousInput = input;
        previousOutput = output;
    }
    // Bit 0: user-authored HDR curve. Bit 1 is reserved for the physical
    // display-peak mapping restored from the D3D11 path.
    processingFlags += 1.0f;

    uint64_t fingerprint = 1469598103934665603ull;
    for (const float value : packedCurve) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        fingerprint ^= bits;
        fingerprint *= 1099511628211ull;
    }
    return fingerprint;
}

float InitialHdrProcessingFlags(const NativeVideoFrame& frame,
                                const anvil::playback::VideoSettings& settings,
                                const bool hdrOutput,
                                const bool forceAppSideDisplayMapping = false) noexcept {
    const bool appSideDisplayMapping = hdrOutput && frame.color.IsHdr() &&
        (forceAppSideDisplayMapping || FrameDolbyVisionMetadata(frame) == nullptr) &&
        (forceAppSideDisplayMapping || !settings.displayMetadataPassthrough);
    return appSideDisplayMapping ? 2.0f : 0.0f;
}

void FillInterpolationDisplayEndpoint(
    InterpolationDisplayMappingPair& mapping,
    const std::size_t endpoint,
    const NativeVideoFrame& frame,
    const anvil::playback::VideoSettings& settings,
    const bool hdrInput,
    const bool hdrOutput,
    const float targetPeakNits) noexcept {
    if (endpoint >= 2) return;
    CompositionConstants constants{};
    constants.hdrOutput = hdrOutput ? 1.0f : 0.0f;
    constants.sourcePeakNits = FrameSourcePeakNits(frame);
    constants.targetPeakNits = targetPeakNits;
    constants.padding = hdrInput
        ? InitialHdrProcessingFlags(frame, settings, hdrOutput, true)
        : 0.0f;
    if (hdrInput) {
        // Dynamic metadata is deliberately internalized per endpoint. The
        // generated frame has no meaningful ST 2094-40 payload of its own.
        FillHdr10PlusConstants(constants, frame, true);
        FillHdrToneCurveConstants(constants.hdrToneCurve, constants.padding,
                                  frame, settings, hdrOutput, true);
    }
    mapping.tone[endpoint][0] = constants.hdrOutput;
    mapping.tone[endpoint][1] = constants.sourcePeakNits;
    mapping.tone[endpoint][2] = constants.targetPeakNits;
    mapping.tone[endpoint][3] = constants.padding;
    mapping.options[endpoint][0] = hdrInput ? 1.0f : 0.0f;
    mapping.options[endpoint][1] = static_cast<float>(
        ToneMappingModeBits(settings.toneMapping) >> 2);
    mapping.options[endpoint][2] = hdrInput &&
        FrameDolbyVisionMetadata(frame) && settings.dolbyVisionCmv4Approx
            ? 1.0f : 0.0f;
    std::copy(std::begin(constants.hdr10PlusA),
              std::end(constants.hdr10PlusA),
              std::begin(mapping.hdr10PlusA[endpoint]));
    std::copy(std::begin(constants.hdr10PlusB),
              std::end(constants.hdr10PlusB),
              std::begin(mapping.hdr10PlusB[endpoint]));
    std::copy(std::begin(constants.hdr10PlusCurve),
              std::end(constants.hdr10PlusCurve),
              &mapping.hdr10PlusCurve[endpoint][0][0]);
    std::copy(std::begin(constants.hdrToneCurve),
              std::end(constants.hdrToneCurve),
              &mapping.hdrToneCurve[endpoint][0][0]);
}

bool HdrOutputEnabled(const NativeVideoFrame& frame,
                      const anvil::playback::VideoSettings& settings,
                      const anvil::playback::DisplayCapabilities& display,
                      const bool hdr10ColorSpaceSupported) noexcept {
    return FrameCarriesHdr(frame) &&
        settings.hdrOutput != HdrOutputMode::ForceSdr &&
        (settings.dolbyVisionHdrOutput || settings.hdrOutput == HdrOutputMode::ForceHdr) &&
        (display.hdrEnabled || settings.hdrOutput == HdrOutputMode::ForceHdr) &&
        hdr10ColorSpaceSupported;
}

bool IsDeviceRemoved(const HRESULT result) {
    return result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_REMOVED ||
           result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                         const UINT subresource,
                                         const D3D12_RESOURCE_STATES before,
                                         const D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

uint64_t ShaderSourceHash(const std::string_view source) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char value : source) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::filesystem::path DolbyVisionShaderCachePath(const std::string_view source) {
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0 || length >= std::size(localAppData)) return {};
    std::wostringstream name;
    name << L"d3d12_dovi_ps5_" << std::hex << std::setfill(L'0')
         << std::setw(16) << ShaderSourceHash(source) << L".cso";
    return std::filesystem::path(localAppData) / L"AnvilPlayer" / L"ShaderCache" /
           name.str();
}

bool LoadShaderCache(const std::filesystem::path& path,
                     Microsoft::WRL::ComPtr<ID3DBlob>& shader) {
    if (path.empty()) return false;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamoff size = input.tellg();
    constexpr std::streamoff kMaximumShaderBytes = 16 * 1024 * 1024;
    if (size <= 0 || size > kMaximumShaderBytes ||
        FAILED(D3DCreateBlob(static_cast<SIZE_T>(size), &shader))) {
        shader.Reset();
        return false;
    }
    input.seekg(0, std::ios::beg);
    input.read(static_cast<char*>(shader->GetBufferPointer()), size);
    if (!input) {
        shader.Reset();
        return false;
    }
    return true;
}

void StoreShaderCache(const std::filesystem::path& path, ID3DBlob* shader) {
    if (path.empty() || !shader || shader->GetBufferSize() == 0) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    std::filesystem::path temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output.write(static_cast<const char*>(shader->GetBufferPointer()),
                     static_cast<std::streamsize>(shader->GetBufferSize()));
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

}  // namespace

D3D12VideoRenderer::D3D12VideoRenderer(LogSinkPtr logSink)
    : logSink_(std::move(logSink)) {}

D3D12VideoRenderer::~D3D12VideoRenderer() {
    RequestStop(nullptr, 0, 0);
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
}

bool D3D12VideoRenderer::BeginInitialize(const HWND host,
                                         const HWND completionWindow,
                                         const UINT completionMessage,
                                         const uint64_t completionCookie,
                                         const UINT deviceLostMessage) {
    if (!host || State() != VideoRendererState::Stopped || renderThread_.joinable()) {
        return false;
    }
    host_.store(host);
    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    completionCookie_ = completionCookie;
    deviceLostMessage_ = deviceLostMessage;
    deviceLossReported_.store(false, std::memory_order_release);
    stopRequested_ = false;
    stopped_.store(false);
    state_.store(VideoRendererState::Initializing);
    try {
        renderThread_ = std::thread(&D3D12VideoRenderer::RenderThreadMain, this);
    } catch (...) {
        stopped_.store(true);
        state_.store(VideoRendererState::Stopped);
        return false;
    }
    return true;
}

void D3D12VideoRenderer::RequestStop(const HWND completionWindow,
                                     const UINT completionMessage,
                                     const uint64_t completionCookie) {
    {
        std::scoped_lock lock(commandMutex_);
        stopWindow_ = completionWindow;
        stopMessage_ = completionMessage;
        stopCookie_ = completionCookie;
        stopRequested_ = true;
        publishedDevice_.store(nullptr);
        if (State() != VideoRendererState::Stopped && State() != VideoRendererState::Failed) {
            state_.store(VideoRendererState::Stopping);
        }
    }
    commandCv_.notify_all();
}

void D3D12VideoRenderer::ConfigureColorPipeline(
    const anvil::playback::VideoSettings& settings,
    const anvil::playback::DisplayCapabilities& display,
    const anvil::playback::VideoColorMetadata& mediaColor) {
    {
        std::scoped_lock lock(commandMutex_);
        int detectedPeakNits = detectedDisplayPeakNits_.load(std::memory_order_acquire);
        if (detectedPeakNits <= 0) {
            detectedPeakNits = SanitizeDisplayPeakNits(display.reportedPeakBrightnessNits);
            if (detectedPeakNits > 0) {
                detectedDisplayPeakNits_.store(detectedPeakNits, std::memory_order_release);
            }
        }
        anvil::playback::DisplayCapabilities normalizedDisplay = display;
        if (detectedPeakNits > 0) {
            normalizedDisplay.reportedPeakBrightnessNits = detectedPeakNits;
        }
        const bool interpolationPresentationChanged =
            InterpolationPresentationConfigChanged(
                videoSettings_, displayCapabilities_, mediaColor_,
                settings, normalizedDisplay, mediaColor);
        videoSettings_ = settings;
        displayCapabilities_ = std::move(normalizedDisplay);
        const int configuredPeakNits =
            SanitizeDisplayPeakNits(settings.displayPeakBrightnessNits);
        effectiveDisplayPeakNits_.store(
            configuredPeakNits > 0
                ? configuredPeakNits
                : (detectedPeakNits > 0 ? detectedPeakNits : 1000),
            std::memory_order_release);
        mediaColor_ = mediaColor;
        interpolationRequested_.store(settings.frameInterpolationEnabled,
                                      std::memory_order_release);
        pendingMlInitialization_ = pendingMlInitialization_ ||
            (settings.frameInterpolationEnabled &&
             !mlInitializationStarted_.load(std::memory_order_acquire));
        pendingInterpolationReset_ = pendingInterpolationReset_ ||
            interpolationPresentationChanged;
        pendingPresent_ = true;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::ConfigureSubtitleSettings(
    const anvil::playback::SubtitleSettings& settings) {
    {
        std::scoped_lock lock(commandMutex_);
        subtitleSettings_ = settings;
        pendingPresent_ = true;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::ConfigureUiOverlay(
    std::shared_ptr<const VideoUiOverlayBitmap> overlay,
    const bool requestImmediatePresent) {
    {
        std::scoped_lock lock(commandMutex_);
        overlaySlots_[0].bitmap = std::move(overlay);
        pendingPresent_ = pendingPresent_ || requestImmediatePresent;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::ConfigureUiMenuOverlay(
    std::shared_ptr<const VideoUiOverlayBitmap> overlay,
    const bool requestImmediatePresent) {
    {
        std::scoped_lock lock(commandMutex_);
        overlaySlots_[1].bitmap = std::move(overlay);
        pendingPresent_ = pendingPresent_ || requestImmediatePresent;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::ConfigureUiMenuOverlayPresentation(
    const int destinationX,
    const int destinationY,
    const int displayWidth,
    const int displayHeight,
    const float opacity,
    const bool requestImmediatePresent) {
    {
        std::scoped_lock lock(commandMutex_);
        overlaySlots_[1].presentation = {
            destinationX, destinationY, displayWidth, displayHeight, opacity};
        pendingPresent_ = pendingPresent_ || requestImmediatePresent;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::OnResize() {
    {
        std::scoped_lock lock(commandMutex_);
        pendingResize_ = true;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::SetDiagnosticsEnabled(const bool enabled) {
    std::scoped_lock lock(commandMutex_);
    diagnosticsEnabled_ = enabled;
}

void D3D12VideoRenderer::ResetRenderStats() {
    std::scoped_lock lock(statsMutex_);
    publishedStats_ = {};
    renderStats_ = {};
    if (IMlFrameInterpolationExecutor* executor =
            mlExecutor_.load(std::memory_order_acquire)) {
        renderStats_.interpolationBackend = executor->BackendName();
    } else if (interpolationRequested_.load(std::memory_order_acquire)) {
        renderStats_.interpolationBackend = L"initializing";
    }
    publishedStats_ = renderStats_;
}

VideoRenderStats D3D12VideoRenderer::TakeRenderStats() const {
    std::scoped_lock lock(statsMutex_);
    return publishedStats_;
}

bool D3D12VideoRenderer::Render(const NativeVideoFrame& frame) {
    if (!IsReady() ||
        (!frame.HasD3D12Texture() && !frame.HasPixels() && !frame.HasYuv())) {
        return false;
    }
    const auto hasValidSoftwareLayout = [](const NativeYuvPlanes& planes) {
        return planes.data && BuildSoftwareYuvUploadLayout(
            planes.width, planes.height, planes.yStride, planes.uvStride,
            planes.bitDepth, planes.data->size()).valid;
    };
    if ((!frame.HasD3D12Texture() && frame.HasYuv() &&
         !hasValidSoftwareLayout(frame.yuv)) ||
        (!frame.HasEnhancementD3D12Texture() && frame.HasEnhancementYuv() &&
         !hasValidSoftwareLayout(frame.enhancementYuv))) {
        Log(LogLevel::Warning, L"d3d12 frame mailbox rejected invalid software YUV");
        return false;
    }
    try {
        auto pending = std::make_unique<NativeVideoFrame>(frame);
        {
            std::scoped_lock lock(commandMutex_);
            pendingFrame_ = std::move(pending);
        }
        commandCv_.notify_one();
        return true;
    } catch (...) {
        Log(LogLevel::Warning, L"d3d12 frame mailbox allocation failed");
        return false;
    }
}

void D3D12VideoRenderer::QueueFrameGraphInput(const NativeVideoFrame& frame) {
    if (!IsReady() || !interpolationRequested_.load(std::memory_order_acquire) ||
        (!frame.HasD3D12Texture() && !frame.HasPixels())) return;
    try {
        auto input = std::make_unique<NativeVideoFrame>(frame);
        StripInterpolationPresentationData(*input);
        {
            std::scoped_lock lock(commandMutex_);
            if (pendingGraphInputs_.size() >= 6) {
                pendingGraphInputs_.clear();
            }
            pendingGraphInputs_.push_back(std::move(input));
        }
        commandCv_.notify_one();
    } catch (...) {
        Log(LogLevel::Warning, L"frame graph input mailbox allocation failed");
    }
}

bool D3D12VideoRenderer::RetireFrame(NativeVideoFrame&& frame) noexcept {
    try {
        std::scoped_lock lock(commandMutex_);
        if (retiredFrames_.size() >= 8) {
            return false;
        }
        // Allocation is completed before the noexcept NativeVideoFrame move;
        // a false return therefore leaves the caller's frame unchanged.
        retiredFrames_.push_back(std::make_unique<NativeVideoFrame>(std::move(frame)));
        commandCv_.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

void D3D12VideoRenderer::Clear() {
    {
        std::scoped_lock lock(commandMutex_);
        pendingClear_ = true;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::RenderThreadMain() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    const bool initialized = InitializeGpu();
    state_.store(initialized ? VideoRendererState::Ready : VideoRendererState::Failed);
    if (initialized) {
        publishedDevice_.store(device_.Get());
    }
    NotifyInitialization(state_.load());

    while (initialized) {
        std::unique_ptr<NativeVideoFrame> frame;
        std::deque<std::unique_ptr<NativeVideoFrame>> graphInputs;
        bool resize = false;
        bool clear = false;
        bool repeat = false;
        bool initializeMl = false;
        bool resetInterpolation = false;
        {
            std::unique_lock lock(commandMutex_);
            const auto hasCommand = [this]() {
                return stopRequested_ || pendingResize_ || pendingClear_ || pendingPresent_ ||
                       pendingMlInitialization_ || pendingInterpolationReset_ ||
                       pendingFrame_ || !pendingGraphInputs_.empty() || !retiredFrames_.empty();
            };
            if (pendingGeneratedSlots_.empty()) {
                commandCv_.wait(lock, hasCommand);
            } else {
                commandCv_.wait_for(lock, std::chrono::milliseconds{2}, hasCommand);
            }
            if (stopRequested_) break;
            frame = std::move(pendingFrame_);
            graphInputs.swap(pendingGraphInputs_);
            resize = std::exchange(pendingResize_, false);
            clear = std::exchange(pendingClear_, false);
            repeat = std::exchange(pendingPresent_, false);
            initializeMl = std::exchange(pendingMlInitialization_, false);
            resetInterpolation = std::exchange(pendingInterpolationReset_, false);
            retiredFrames_.clear();
            activeOverlaySlots_ = overlaySlots_;
            activeSubtitleSettings_ = subtitleSettings_;
        }

        if (resetInterpolation) {
            ResetInterpolationState();
        }
        if (initializeMl) {
            StartMlInitialization();
        }
        if (resize) {
            // Presentation-size and monitor changes can alter both the DV
            // endpoint render target and the effective HDR peak. Never allow
            // a generated frame prepared under the old contract to survive.
            ResetInterpolationState();
            RECT client{};
            GetClientRect(host_.load(), &client);
            Resize(static_cast<UINT>(std::max<LONG>(1, client.right - client.left)),
                   static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top)));
        }
        if (clear) {
            ClearFrame();
        }
        for (const auto& graphInput : graphInputs) {
            if (graphInput) ProcessFrameGraphInput(*graphInput);
        }
        if (frame) {
            ProcessGeneratedBefore(*frame);
            currentFrame_ = std::move(frame);
            RenderFrame(*currentFrame_);
        } else if (repeat && currentFrame_) {
            RenderLastFrame();
        }
        if (deviceLossReported_.load(std::memory_order_acquire)) break;
        ProcessReadyGeneratedFrame();
        if (deviceLossReported_.load(std::memory_order_acquire)) break;
    }

    publishedDevice_.store(nullptr);
    ReleaseGpu();
    currentFrame_.reset();
    pendingFrame_.reset();
    pendingGraphInputs_.clear();
    retiredFrames_.clear();
    stopped_.store(true);
    if (state_.load() != VideoRendererState::Failed) {
        state_.store(VideoRendererState::Stopped);
    }
    if (stopWindow_ && stopMessage_) {
        PostMessageW(stopWindow_, stopMessage_, static_cast<WPARAM>(stopCookie_), 0);
    }
    if (comInitialized) CoUninitialize();
}

bool D3D12VideoRenderer::InitializeGpu() {
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)))) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
    factory_.As(&factory6);
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        const HRESULT result = factory6
                                   ? factory6->EnumAdapterByGpuPreference(
                                         index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                         IID_PPV_ARGS(&candidate))
                                   : factory_->EnumAdapters1(index, &candidate);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)))) {
            adapter_ = candidate;
            Log(LogLevel::Info, L"d3d12 adapter=" + std::wstring(desc.Description));
            break;
        }
    }
    if (!device_) return false;

    const auto createQueue = [this](const D3D12_COMMAND_LIST_TYPE type,
                                    Microsoft::WRL::ComPtr<ID3D12CommandQueue>& output) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        return device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&output));
    };
    if (FAILED(createQueue(D3D12_COMMAND_LIST_TYPE_DIRECT, queue_)) ||
        FAILED(createQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, computeQueue_)) ||
        FAILED(createQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, mlQueue_)) ||
        FAILED(createQueue(D3D12_COMMAND_LIST_TYPE_COPY, copyQueue_))) {
        return false;
    }
    if (LibplaceboD3D12Bridge::RequestedByEnvironment()) {
        libplaceboBridge_ = std::make_unique<LibplaceboD3D12Bridge>(logSink_);
        if (!libplaceboBridge_->Initialize(device_.Get(), queue_.Get())) {
            libplaceboBridge_.reset();
            Log(LogLevel::Warning,
                L"optional libplacebo backend unavailable; using native D3D12 Dolby Vision path");
        }
    }

    RECT client{};
    GetClientRect(host_.load(), &client);
    width_ = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    height_ = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    if (!CreateSwapChain(width_, height_) || !CreatePipeline()) return false;

    // Compile the expensive DV shader as soon as the resident renderer exists,
    // in parallel with probing/decoder setup. A DV frame must never be shown by
    // the ordinary YCbCr pipeline: Profile 5 is IPT and Profiles 7/8 require the
    // RPU mapping even when an enhancement surface is not present.
    StartAdvancedPipelineInitialization();

    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&allocators_[index])))) {
            return false;
        }
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocators_[0].Get(), nullptr,
                                          IID_PPV_ARGS(&commandList_)))) {
        return false;
    }
    commandList_->Close();
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        return false;
    }
    if (!frameGraph_.Initialize(device_.Get(), computeQueue_.Get(), queue_.Get(), copyQueue_.Get())) {
        return false;
    }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) return false;
    Log(LogLevel::Info,
        L"d3d12 resident pipeline ready queues=graphics+preprocess+ml+copy output=" +
            std::wstring(hdr10ColorSpaceSupported_ ? L"rgb10_hdr10" : L"rgb10_sdr") +
            L" target_peak_nits=" +
            std::to_wstring(static_cast<int>(TargetPeakNits(videoSettings_, displayCapabilities_))) +
            L" ml=lazy_background dovi_shaders=background_precompile");
    return true;
}

void D3D12VideoRenderer::StartMlInitialization() {
    if (mlInitializationStarted_.exchange(true, std::memory_order_acq_rel) ||
        !device_ || !mlQueue_) {
        return;
    }
    const std::filesystem::path modelPath =
        DirectMlFrameInterpolationExecutor::DefaultModelPath();
    Microsoft::WRL::ComPtr<ID3D12Device> device = device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> mlQueue = mlQueue_;
    try {
        mlInitializationThread_ = std::thread(
            [this, device = std::move(device), mlQueue = std::move(mlQueue), modelPath]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                IMlFrameInterpolationExecutor* executor = nullptr;
                const bool preprocessorReady =
                    tensorPreprocessor_.Initialize(device.Get(), &frameGraph_);
                if (preprocessorReady) {
                    // ONNX Runtime parses the graph; DML1 executes it on the
                    // dedicated application-owned ML queue. Windows ML is the
                    // resource-native fallback.
                    if (directMlExecutor_.Initialize(
                            device.Get(), mlQueue.Get(), modelPath)) {
                        executor = &directMlExecutor_;
                    } else if (windowsMlExecutor_.Initialize(
                                   device.Get(), mlQueue.Get(), modelPath)) {
                        executor = &windowsMlExecutor_;
                    }
                }
                mlExecutor_.store(executor, std::memory_order_release);
                {
                    std::scoped_lock lock(statsMutex_);
                    renderStats_.interpolationBackend = executor
                        ? executor->BackendName() : L"unavailable";
                    publishedStats_ = renderStats_;
                }
                if (executor) {
                    Log(LogLevel::Info,
                        L"frame graph executor ready background=true backend=" +
                            executor->BackendName() + L" model=" +
                            modelPath.filename().wstring());
                    commandCv_.notify_one();
                } else {
                    Log(LogLevel::Warning,
                        L"frame graph ML executors unavailable preprocess=" +
                            std::wstring(preprocessorReady ? L"ready" : L"failed") +
                            L" windows_ml=" +
                            windowsMlExecutor_.LastError() + L" directml=" +
                            directMlExecutor_.LastError());
                }
                if (!executor) commandCv_.notify_one();
            });
        Log(LogLevel::Info, L"frame graph executor initialization queued background=true");
    } catch (...) {
        mlInitializationStarted_.store(false, std::memory_order_release);
        Log(LogLevel::Warning, L"frame graph executor background thread unavailable");
    }
}

void D3D12VideoRenderer::StartAdvancedPipelineInitialization() {
    if (advancedPipelineStarted_.exchange(true, std::memory_order_acq_rel) ||
        !device_ || !rootSignature_ ||
        !yuvVertexShader_ || doviPixelShaderSource_.empty()) {
        return;
    }
    try {
        advancedPipelineThread_ = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            if (CreateDolbyVisionPipeline()) {
                Log(LogLevel::Info,
                    L"d3d12 Dolby Vision pipeline ready background=true");
            } else {
                Log(LogLevel::Warning,
                    L"d3d12 Dolby Vision pipeline unavailable; base layer remains active");
            }
        });
    } catch (...) {
        advancedPipelineStarted_.store(false, std::memory_order_release);
        Log(LogLevel::Warning,
            L"d3d12 Dolby Vision pipeline background thread unavailable");
    }
}

bool D3D12VideoRenderer::CreateSwapChain(const UINT width, const UINT height) {
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kBufferCount;
    if (FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)))) return false;
    rtvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = kBufferCount * 4;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_)))) return false;
    srvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvDesc.NumDescriptors = kBufferCount * 4;
    if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&tensorSrvHeap_)))) return false;
    srvDesc.NumDescriptors = kBufferCount * kMaxOverlayTextures;
    if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&overlaySrvHeap_)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = kCompositionFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    const auto resetComposition = [this, &swapChain1]() {
        swapChain1.Reset();
        swapChain_.Reset();
        compositionVisual_.Reset();
        compositionTarget_.Reset();
        compositionDevice_.Reset();
        useComposition_ = false;
    };
    const auto tryComposition = [&](const UINT flags) {
        desc.Flags = flags;
        HRESULT result = factory_->CreateSwapChainForComposition(
            queue_.Get(), &desc, nullptr, &swapChain1);
        if (FAILED(result) || FAILED(swapChain1.As(&swapChain_)) ||
            !CreateComposition()) {
            resetComposition();
            return false;
        }
        useComposition_ = true;
        swapChainWaitable_ =
            (flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
        Log(LogLevel::Info,
            L"d3d12 swap chain created path=direct_composition waitable=" +
                std::wstring(swapChainWaitable_ ? L"true" : L"false"));
        return true;
    };
    bool created =
        tryComposition(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ||
        tryComposition(0);
    HRESULT createResult = S_OK;
    if (!created) {
        resetComposition();
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        createResult = factory_->CreateSwapChainForHwnd(
            queue_.Get(), host_.load(), &desc, nullptr, nullptr, &swapChain1);
        swapChainWaitable_ = SUCCEEDED(createResult);
    }
    if (!created && FAILED(createResult)) {
        desc.Flags = 0;
        swapChain1.Reset();
        createResult = factory_->CreateSwapChainForHwnd(
            queue_.Get(), host_.load(), &desc, nullptr, nullptr, &swapChain1);
    }
    if (!created &&
        (FAILED(createResult) || FAILED(swapChain1.As(&swapChain_)))) {
        return false;
    }
    if (!created) {
        Log(LogLevel::Warning,
            L"d3d12 DirectComposition unavailable; native P010 HLG presentation disabled");
    }
    ConfigureFramePacing();
    factory_->MakeWindowAssociation(host_.load(), DXGI_MWA_NO_ALT_ENTER);
    UINT sdrColorSpaceSupport = 0;
    const HRESULT sdrColorSpaceQuery = swapChain_->CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &sdrColorSpaceSupport);
    if (FAILED(sdrColorSpaceQuery) ||
        (sdrColorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0 ||
        FAILED(swapChain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709))) {
        Log(LogLevel::Warning, L"RGB10 SDR swap-chain color space unavailable");
    }
    UINT hdrColorSpaceSupport = 0;
    const HRESULT hdrColorSpaceQuery = swapChain_->CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &hdrColorSpaceSupport);
    hdr10ColorSpaceSupported_ =
        SUCCEEDED(hdrColorSpaceQuery) &&
        (hdrColorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
    if (!hdr10ColorSpaceSupported_) {
        Log(LogLevel::Warning,
            L"HDR10 swap-chain color space unavailable; HDR will use the SDR fallback");
    }

    // The process-wide capability probe can still be pending when playback
    // starts. Query the output which actually contains this swap chain so the
    // physical peak does not incorrectly remain at zero (and fall back to SDR).
    Microsoft::WRL::ComPtr<IDXGIOutput> containingOutput;
    Microsoft::WRL::ComPtr<IDXGIOutput6> containingOutput6;
    DXGI_OUTPUT_DESC1 outputDescription{};
    if (SUCCEEDED(swapChain_->GetContainingOutput(&containingOutput)) &&
        SUCCEEDED(containingOutput.As(&containingOutput6)) &&
        SUCCEEDED(containingOutput6->GetDesc1(&outputDescription))) {
        const bool outputAdvancedColorActive =
            outputDescription.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
            outputDescription.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
            outputDescription.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
        displayCapabilities_.hdrEnabled = outputAdvancedColorActive;
        displayCapabilities_.hdrSupported = outputAdvancedColorActive ||
            outputDescription.MaxLuminance >= 400.0f;
        if (outputDescription.MaxLuminance >= 100.0f) {
            displayCapabilities_.reportedPeakBrightnessNits =
                static_cast<int>(outputDescription.MaxLuminance + 0.5f);
        }
        Log(LogLevel::Info,
            L"d3d12 containing output advanced_color=" +
                std::wstring(outputAdvancedColorActive ? L"on" : L"off") +
                L" dxgi_color_space=" +
                std::to_wstring(static_cast<int>(outputDescription.ColorSpace)) +
                L" peak_nits=" +
                std::to_wstring(displayCapabilities_.reportedPeakBrightnessNits));
    }
    RefreshDisplayPeakNits();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffers_[index])))) return false;
        device_->CreateRenderTargetView(backBuffers_[index].Get(), nullptr, rtv);
        rtv.ptr += rtvIncrement_;
    }
    return CreateLibplaceboTargets(width, height);
}

bool D3D12VideoRenderer::CreateComposition() {
    const HWND window = host_.load(std::memory_order_acquire);
    if (!swapChain_ || !window || !IsWindow(window)) return false;
    // This device only owns visuals and swap-chain content. Passing nullptr is
    // the most compatible option for a D3D12 renderer because the original
    // DCompositionCreateDevice entry point accepts only IDXGIDevice (D3D11).
    HRESULT result = DCompositionCreateDevice(
        nullptr, IID_PPV_ARGS(&compositionDevice_));
    if (FAILED(result) || !compositionDevice_) return false;
    result = compositionDevice_->CreateTargetForHwnd(
        window, TRUE, &compositionTarget_);
    if (FAILED(result) || !compositionTarget_) return false;
    result = compositionDevice_->CreateVisual(&compositionVisual_);
    if (FAILED(result) || !compositionVisual_) return false;
    result = compositionVisual_->SetContent(swapChain_.Get());
    if (FAILED(result)) return false;
    result = compositionTarget_->SetRoot(compositionVisual_.Get());
    if (FAILED(result)) return false;
    return SUCCEEDED(compositionDevice_->Commit());
}

bool D3D12VideoRenderer::EnsureHlgPresentationResources(
    const UINT width,
    const UINT height,
    const DXGI_COLOR_SPACE_TYPE requestedColorSpace) {
    hlgInitializationAttempted_ = true;
    if (!useComposition_ || !compositionDevice_ || !compositionVisual_ ||
        !device_ || !adapter_ || width == 0 || height == 0) {
        return false;
    }
    const bool compatibleColorSpace =
        hlgColorSpace_ == requestedColorSpace ||
        (requestedColorSpace ==
                 DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 &&
         !hlgFullColorSpaceSupported_ && hlgStudioColorSpaceSupported_) ||
        (requestedColorSpace ==
                 DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020 &&
         !hlgStudioColorSpaceSupported_ && hlgFullColorSpaceSupported_);
    if (hlgSwapChain_ && hlgVideoProcessor_ && hlgSharedRgbResource_ &&
        hlgWidth_ == ((width + 1u) & ~1u) &&
        hlgHeight_ == ((height + 1u) & ~1u) &&
        compatibleColorSpace) {
        return true;
    }
    ReleaseHlgPresentationResources();

    hlgWidth_ = (width + 1u) & ~1u;
    hlgHeight_ = (height + 1u) & ~1u;

    D3D_FEATURE_LEVEL featureLevels[]{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtainedFeatureLevel{};
    HRESULT result = D3D11CreateDevice(
        adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels, static_cast<UINT>(std::size(featureLevels)),
        D3D11_SDK_VERSION, &hlgD3D11Device_, &obtainedFeatureLevel,
        &hlgD3D11Context_);
    if (FAILED(result) || !hlgD3D11Device_ || !hlgD3D11Context_ ||
        FAILED(hlgD3D11Context_.As(&hlgD3D11Context4_)) ||
        FAILED(hlgD3D11Device_.As(&hlgD3D11VideoDevice_)) ||
        FAILED(hlgD3D11Context_.As(&hlgD3D11VideoContext_))) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=native_d3d11_video_device");
        ReleaseHlgPresentationResources();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = hlgWidth_;
    desc.Height = hlgHeight_;
    desc.Format = DXGI_FORMAT_P010;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO |
                 DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO;

    const auto logSwapChainResult = [this](
                                       const wchar_t* path,
                                       const DXGI_SWAP_CHAIN_DESC1& candidate,
                                       const HRESULT createResult,
                                       const HRESULT interfaceResult) {
        std::wostringstream message;
        message << L"HLG P010 swap chain path=" << path
                << L" flags=0x" << std::hex << candidate.Flags
                << L" alpha=" << std::dec << candidate.AlphaMode
                << L" create=0x" << std::hex
                << static_cast<unsigned long>(createResult)
                << L" interface=0x"
                << static_cast<unsigned long>(interfaceResult);
        Log(FAILED(createResult) || FAILED(interfaceResult)
                ? LogLevel::Warning
                : LogLevel::Info,
            message.str());
    };
    const auto adoptSwapChain = [&](const wchar_t* path,
                                    const DXGI_SWAP_CHAIN_DESC1& candidate,
                                    const HRESULT createResult,
                                    IDXGISwapChain1* swapChain) {
        HRESULT interfaceResult = E_NOINTERFACE;
        if (SUCCEEDED(createResult) && swapChain) {
            interfaceResult = swapChain->QueryInterface(
                IID_PPV_ARGS(&hlgSwapChain_));
        }
        logSwapChainResult(
            path, candidate, createResult, interfaceResult);
        if (FAILED(createResult) || FAILED(interfaceResult)) {
            hlgSwapChain_.Reset();
            return false;
        }
        return true;
    };
    const auto createMediaSwapChain = [&](IDXGIFactoryMedia* factoryMedia,
                                          const wchar_t* path) {
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
        const HRESULT createResult =
            factoryMedia->CreateSwapChainForCompositionSurfaceHandle(
                hlgD3D11Device_.Get(), hlgCompositionSurfaceHandle_, &desc,
                nullptr, &swapChain);
        return adoptSwapChain(path, desc, createResult, swapChain.Get());
    };

    Microsoft::WRL::ComPtr<IDXGIFactoryMedia> factoryMedia;
    result = adapter_->GetParent(IID_PPV_ARGS(&factoryMedia));
    if (FAILED(result) || !factoryMedia) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=dxgi_factory_media");
        ReleaseHlgPresentationResources();
        return false;
    }
    result = DCompositionCreateSurfaceHandle(
        COMPOSITIONOBJECT_ALL_ACCESS, nullptr,
        &hlgCompositionSurfaceHandle_);
    if (FAILED(result) || !hlgCompositionSurfaceHandle_) {
        hlgCompositionSurfaceHandle_ = nullptr;
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=composition_surface_handle");
        ReleaseHlgPresentationResources();
        return false;
    }
    bool mediaSwapChain = createMediaSwapChain(
        factoryMedia.Get(), L"native_d3d11_media_fullscreen_yuv");
    if (!mediaSwapChain) {
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO;
        mediaSwapChain = createMediaSwapChain(
            factoryMedia.Get(), L"native_d3d11_media_yuv");
    }
    if (!mediaSwapChain) {
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        mediaSwapChain = createMediaSwapChain(
            factoryMedia.Get(), L"native_d3d11_media_yuv_unspecified_alpha");
    }
    if (!mediaSwapChain || !hlgSwapChain_) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=p010_native_d3d11_media_swap_chain");
        ReleaseHlgPresentationResources();
        return false;
    }
    result = compositionDevice_->CreateSurfaceFromHandle(
        hlgCompositionSurfaceHandle_, &hlgCompositionSurface_);
    if (FAILED(result) || !hlgCompositionSurface_) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=composition_surface_import");
        ReleaseHlgPresentationResources();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput4> targetOutput4;
    const HWND host = host_.load(std::memory_order_acquire);
    const HMONITOR targetMonitor = host
                                       ? MonitorFromWindow(
                                             host, MONITOR_DEFAULTTONEAREST)
                                       : nullptr;
    for (UINT outputIndex = 0;; ++outputIndex) {
        Microsoft::WRL::ComPtr<IDXGIOutput> candidate;
        if (adapter_->EnumOutputs(outputIndex, &candidate) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_OUTPUT_DESC outputDesc{};
        if (!candidate || FAILED(candidate->GetDesc(&outputDesc)) ||
            (targetMonitor && outputDesc.Monitor != targetMonitor)) {
            continue;
        }
        candidate.As(&targetOutput4);
        break;
    }
    const auto colorSpaceSupported = [this, &targetOutput4](
                                         const DXGI_COLOR_SPACE_TYPE colorSpace) {
        UINT swapSupport = 0;
        const HRESULT swapResult =
            hlgSwapChain_->CheckColorSpaceSupport(colorSpace, &swapSupport);
        UINT overlaySupport = 0;
        const HRESULT overlayResult = targetOutput4
                                          ? targetOutput4->CheckOverlayColorSpaceSupport(
                                                DXGI_FORMAT_P010, colorSpace,
                                                hlgD3D11Device_.Get(),
                                                &overlaySupport)
                                          : E_NOINTERFACE;
        constexpr UINT kPresentSupport =
            DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT |
            DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT;
        const bool supported =
            (SUCCEEDED(swapResult) &&
             (swapSupport & kPresentSupport) != 0) ||
            (SUCCEEDED(overlayResult) &&
             (overlaySupport &
              DXGI_OVERLAY_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0);
        std::wostringstream message;
        message << L"HLG color space=" << colorSpace
                << L" swap=0x" << std::hex << swapSupport
                << L" swap_hr=0x"
                << static_cast<unsigned long>(swapResult)
                << L" overlay=0x" << overlaySupport
                << L" overlay_hr=0x"
                << static_cast<unsigned long>(overlayResult)
                << L" supported=" << std::boolalpha << supported;
        Log(LogLevel::Info, message.str());
        return supported;
    };
    hlgStudioColorSpaceSupported_ = colorSpaceSupported(
        DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020);
    hlgFullColorSpaceSupported_ = colorSpaceSupported(
        DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020);
    if (!hlgStudioColorSpaceSupported_ && !hlgFullColorSpaceSupported_) {
        // A few drivers under-report media-overlay color spaces. A successful
        // SetColorSpace1 is authoritative and lets those drivers continue;
        // failures retain the conservative HDR10 fallback.
        const HRESULT probeResult =
            hlgSwapChain_->SetColorSpace1(requestedColorSpace);
        std::wostringstream message;
        message << L"HLG color space direct probe value="
                << requestedColorSpace << L" hr=0x" << std::hex
                << static_cast<unsigned long>(probeResult);
        Log(SUCCEEDED(probeResult) ? LogLevel::Info : LogLevel::Warning,
            message.str());
        if (SUCCEEDED(probeResult)) {
            hlgStudioColorSpaceSupported_ =
                requestedColorSpace ==
                DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
            hlgFullColorSpaceSupported_ =
                requestedColorSpace ==
                DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020;
        } else {
            Log(LogLevel::Warning,
                L"HLG native unavailable reason=hlg_color_space_support");
            ReleaseHlgPresentationResources();
            return false;
        }
    }
    hlgColorSpace_ =
        requestedColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 &&
                hlgFullColorSpaceSupported_
            ? requestedColorSpace
            : (hlgStudioColorSpaceSupported_
                   ? DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020
                   : DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020);
    if (FAILED(hlgSwapChain_->SetColorSpace1(hlgColorSpace_))) {
        ReleaseHlgPresentationResources();
        return false;
    }

    D3D12_HEAP_PROPERTIES sharedHeap{};
    sharedHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC sharedDesc{};
    sharedDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sharedDesc.Width = width;
    sharedDesc.Height = height;
    sharedDesc.DepthOrArraySize = 1;
    sharedDesc.MipLevels = 1;
    sharedDesc.Format = kCompositionFormat;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    sharedDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    result = device_->CreateCommittedResource(
        &sharedHeap, D3D12_HEAP_FLAG_SHARED, &sharedDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&hlgSharedRgbResource_));
    if (FAILED(result)) {
        std::wostringstream message;
        message << L"HLG native unavailable reason=shared_rgb10_resource hr=0x"
                << std::hex << static_cast<unsigned long>(result);
        Log(LogLevel::Warning,
            message.str());
        ReleaseHlgPresentationResources();
        return false;
    }
    HANDLE sharedTextureHandle = nullptr;
    result = device_->CreateSharedHandle(
        hlgSharedRgbResource_.Get(), nullptr, GENERIC_ALL,
        nullptr, &sharedTextureHandle);
    Microsoft::WRL::ComPtr<ID3D11Device1> d3d11Device1;
    if (SUCCEEDED(result)) result = hlgD3D11Device_.As(&d3d11Device1);
    if (SUCCEEDED(result) && sharedTextureHandle) {
        result = d3d11Device1->OpenSharedResource1(
            sharedTextureHandle, IID_PPV_ARGS(&hlgSharedRgbTexture_));
    }
    if (sharedTextureHandle) CloseHandle(sharedTextureHandle);
    if (FAILED(result) || !hlgSharedRgbTexture_) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=open_shared_rgb10_in_d3d11");
        ReleaseHlgPresentationResources();
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device5> d3d11Device5;
    HANDLE sharedFenceHandle = nullptr;
    result = device_->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&hlgSharedFence12_));
    if (SUCCEEDED(result)) {
        result = device_->CreateSharedHandle(
            hlgSharedFence12_.Get(), nullptr, GENERIC_ALL,
            nullptr, &sharedFenceHandle);
    }
    if (SUCCEEDED(result)) result = hlgD3D11Device_.As(&d3d11Device5);
    if (SUCCEEDED(result) && d3d11Device5 && sharedFenceHandle) {
        result = d3d11Device5->OpenSharedFence(
            sharedFenceHandle, IID_PPV_ARGS(&hlgSharedFence11_));
    }
    if (sharedFenceHandle) CloseHandle(sharedFenceHandle);
    if (FAILED(result) || !hlgSharedFence12_ || !hlgSharedFence11_) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=d3d11_d3d12_shared_fence");
        ReleaseHlgPresentationResources();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = hlgWidth_;
    content.OutputHeight = hlgHeight_;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    result = hlgD3D11VideoDevice_->CreateVideoProcessorEnumerator(
        &content, &hlgVideoProcessorEnumerator_);
    UINT inputSupport = 0;
    UINT outputSupport = 0;
    if (SUCCEEDED(result)) {
        result = hlgVideoProcessorEnumerator_->CheckVideoProcessorFormat(
            kCompositionFormat, &inputSupport);
    }
    if (SUCCEEDED(result)) {
        result = hlgVideoProcessorEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_P010, &outputSupport);
    }
    if (FAILED(result) ||
        (inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        (outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0 ||
        FAILED(hlgD3D11VideoDevice_->CreateVideoProcessor(
            hlgVideoProcessorEnumerator_.Get(), 0, &hlgVideoProcessor_))) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=native_d3d11_video_processor");
        ReleaseHlgPresentationResources();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    result = hlgD3D11VideoDevice_->CreateVideoProcessorInputView(
        hlgSharedRgbTexture_.Get(), hlgVideoProcessorEnumerator_.Get(),
        &inputViewDesc, &hlgVideoInputView_);
    if (SUCCEEDED(result)) {
        result = hlgSwapChain_->GetBuffer(
            0, IID_PPV_ARGS(&hlgD3D11BackBuffer_));
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result)) {
        result = hlgD3D11VideoDevice_->CreateVideoProcessorOutputView(
            hlgD3D11BackBuffer_.Get(), hlgVideoProcessorEnumerator_.Get(),
            &outputViewDesc, &hlgVideoOutputView_);
    }
    if (FAILED(result) || !hlgVideoInputView_ || !hlgVideoOutputView_) {
        Log(LogLevel::Warning,
            L"HLG native unavailable reason=native_d3d11_video_processor_views");
        ReleaseHlgPresentationResources();
        return false;
    }

    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&hlgAllocators_[index])))) {
            ReleaseHlgPresentationResources();
            return false;
        }
    }
    if (FAILED(device_->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, hlgAllocators_[0].Get(),
            nullptr, IID_PPV_ARGS(&hlgCopyCommandList_)))) {
        ReleaseHlgPresentationResources();
        return false;
    }
    hlgCopyCommandList_->Close();
    hlgSharedFenceValue_ = 1;
    hlgSharedAvailableFenceValue_ = 0;
    Log(LogLevel::Info,
        L"HLG native ready path=d3d12_rgb10_subtitle_ui_shared_to_native_d3d11_p010_present color_space=" +
            std::wstring(
                hlgColorSpace_ ==
                        DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020
                    ? L"ycbcr_full_hlg_bt2020"
                    : L"ycbcr_studio_hlg_bt2020"));
    return true;
}

void D3D12VideoRenderer::ReleaseHlgPresentationResources() {
    if (hlgCompositionSelected_) SelectCompositionSwapChain(false);
    if (fence_ && fenceEvent_) {
        for (UINT index = 0; index < kBufferCount; ++index) {
            WaitForHlgBackBuffer(index);
        }
    }
    if (hlgSharedFence12_ && fenceEvent_ &&
        hlgSharedAvailableFenceValue_ != 0 &&
        hlgSharedFence12_->GetCompletedValue() <
            hlgSharedAvailableFenceValue_ &&
        SUCCEEDED(hlgSharedFence12_->SetEventOnCompletion(
            hlgSharedAvailableFenceValue_, fenceEvent_))) {
        WaitForSingleObject(fenceEvent_, 2000);
    }
    hlgCopyCommandList_.Reset();
    for (auto& allocator : hlgAllocators_) allocator.Reset();
    hlgBufferFenceValues_.fill(0);
    hlgVideoOutputView_.Reset();
    hlgVideoInputView_.Reset();
    hlgVideoProcessor_.Reset();
    hlgVideoProcessorEnumerator_.Reset();
    hlgD3D11BackBuffer_.Reset();
    hlgSharedRgbResource_.Reset();
    hlgSharedRgbTexture_.Reset();
    hlgSharedFence11_.Reset();
    hlgSharedFence12_.Reset();
    hlgSwapChain_.Reset();
    hlgCompositionSurface_.Reset();
    if (hlgCompositionSurfaceHandle_) {
        CloseHandle(hlgCompositionSurfaceHandle_);
        hlgCompositionSurfaceHandle_ = nullptr;
    }
    hlgD3D11VideoContext_.Reset();
    hlgD3D11VideoDevice_.Reset();
    hlgD3D11Context4_.Reset();
    hlgD3D11Context_.Reset();
    hlgD3D11Device_.Reset();
    hlgSharedFenceValue_ = 1;
    hlgSharedAvailableFenceValue_ = 0;
    hlgWidth_ = 0;
    hlgHeight_ = 0;
    hlgStudioColorSpaceSupported_ = false;
    hlgFullColorSpaceSupported_ = false;
    hlgCompositionSelected_ = false;
    nativeHlgComposition_ = false;
}

bool D3D12VideoRenderer::WantsNativeHlgOutput(
    const NativeVideoFrame& frame) {
    if (!videoSettings_.displayMetadataPassthrough ||
        videoSettings_.hdrOutput == HdrOutputMode::ForceSdr ||
        videoSettings_.frameInterpolationEnabled ||
        frame.color.transfer != VideoTransferCharacteristic::Hlg ||
        frame.color.primaries != VideoColorPrimaries::Bt2020 ||
        !useComposition_) {
        nativeHlgComposition_ = false;
        return false;
    }
    const DXGI_COLOR_SPACE_TYPE requested =
        frame.color.range == VideoColorRange::Full
            ? DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
    if (hlgInitializationAttempted_ && !hlgSwapChain_) {
        nativeHlgComposition_ = false;
        return false;
    }
    const bool ready = EnsureHlgPresentationResources(width_, height_, requested);
    nativeHlgComposition_ = ready;
    if (!ready && !hlgFailureLogged_) {
        hlgFailureLogged_ = true;
        Log(LogLevel::Warning,
            L"HLG native passthrough unavailable fallback=rgb10_hdr10");
    }
    return ready;
}

bool D3D12VideoRenderer::SelectCompositionSwapChain(const bool hlg) {
    if (!useComposition_) return !hlg;
    if (!compositionVisual_ || !compositionDevice_) return false;
    if (hlgCompositionSelected_ == hlg) return true;
    IUnknown* selected = nullptr;
    if (hlg) {
        selected = hlgCompositionSurface_
                       ? static_cast<IUnknown*>(hlgCompositionSurface_.Get())
                       : static_cast<IUnknown*>(hlgSwapChain_.Get());
    } else {
        selected = static_cast<IUnknown*>(swapChain_.Get());
    }
    if (!selected || FAILED(compositionVisual_->SetContent(selected)) ||
        FAILED(compositionDevice_->Commit())) {
        return false;
    }
    hlgCompositionSelected_ = hlg;
    Log(LogLevel::Info,
        L"d3d12 composition content=" +
            std::wstring(hlg ? L"hlg_p010" : L"rgb10"));
    return true;
}

bool D3D12VideoRenderer::PresentComposedFrame(
    const UINT backBufferIndex,
    const GpuFencePoint& graphicsCompletion,
    const bool nativeHlg) {
    if (!nativeHlg) {
        nativeHlgComposition_ = false;
        if (!SelectCompositionSwapChain(false)) return false;
        const HRESULT presentResult = swapChain_->Present(1, 0);
        if (FAILED(presentResult)) {
            return !DeviceLost(presentResult, L"Present");
        }
        const uint64_t fenceValue = nextFenceValue_++;
        if (FAILED(queue_->Signal(fence_.Get(), fenceValue))) return false;
        bufferFenceValues_[backBufferIndex] = fenceValue;
        return true;
    }
    if (!hlgSwapChain_ || !hlgVideoProcessor_ || !hlgCopyCommandList_ ||
        !hlgSharedRgbResource_ || !hlgSharedFence12_ ||
        !hlgSharedFence11_ || !hlgD3D11Context4_ ||
        !hlgD3D11VideoContext_ || !hlgVideoInputView_ ||
        !hlgVideoOutputView_ || !graphicsCompletion.IsValid()) {
        return false;
    }
    if (backBufferIndex >= kBufferCount ||
        !WaitForHlgBackBuffer(backBufferIndex)) {
        Log(LogLevel::Warning,
            L"HLG native present failed reason=copy_allocator_wait");
        return false;
    }
    HRESULT hlgResult = queue_->Wait(
        graphicsCompletion.fence.Get(), graphicsCompletion.value);
    if (SUCCEEDED(hlgResult) && hlgSharedAvailableFenceValue_ != 0) {
        hlgResult = queue_->Wait(
            hlgSharedFence12_.Get(), hlgSharedAvailableFenceValue_);
    }
    if (SUCCEEDED(hlgResult)) {
        hlgResult = hlgAllocators_[backBufferIndex]->Reset();
    }
    if (SUCCEEDED(hlgResult)) {
        hlgResult = hlgCopyCommandList_->Reset(
            hlgAllocators_[backBufferIndex].Get(), nullptr);
    }
    if (FAILED(hlgResult)) {
        std::wostringstream message;
        message << L"HLG native present failed reason=copy_setup hr=0x"
                << std::hex << static_cast<unsigned long>(hlgResult);
        Log(LogLevel::Warning, message.str());
        return false;
    }
    const std::array beginBarriers{
        TransitionBarrier(
            hlgSharedRgbResource_.Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST)};
    hlgCopyCommandList_->ResourceBarrier(
        static_cast<UINT>(beginBarriers.size()), beginBarriers.data());
    hlgCopyCommandList_->CopyResource(
        hlgSharedRgbResource_.Get(), backBuffers_[backBufferIndex].Get());
    const std::array endBarriers{
        TransitionBarrier(
            backBuffers_[backBufferIndex].Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PRESENT),
        TransitionBarrier(
            hlgSharedRgbResource_.Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COMMON)};
    hlgCopyCommandList_->ResourceBarrier(
        static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(hlgCopyCommandList_->Close())) return false;
    ID3D12CommandList* commandLists[]{hlgCopyCommandList_.Get()};
    queue_->ExecuteCommandLists(1, commandLists);
    const uint64_t rgbReadyFenceValue = hlgSharedFenceValue_++;
    const uint64_t copyFenceValue = nextFenceValue_++;
    hlgResult = queue_->Signal(
        hlgSharedFence12_.Get(), rgbReadyFenceValue);
    if (SUCCEEDED(hlgResult)) {
        hlgResult = queue_->Signal(fence_.Get(), copyFenceValue);
    }
    if (SUCCEEDED(hlgResult)) {
        hlgResult = hlgD3D11Context4_->Wait(
            hlgSharedFence11_.Get(), rgbReadyFenceValue);
    }
    if (FAILED(hlgResult)) {
        std::wostringstream message;
        message << L"HLG native present failed reason=cross_api_wait hr=0x"
                << std::hex << static_cast<unsigned long>(hlgResult);
        Log(LogLevel::Warning, message.str());
        return false;
    }

    const RECT sourceRect{
        0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    const RECT outputRect{
        0, 0, static_cast<LONG>(hlgWidth_), static_cast<LONG>(hlgHeight_)};
    hlgD3D11VideoContext_->VideoProcessorSetStreamFrameFormat(
        hlgVideoProcessor_.Get(), 0,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    hlgD3D11VideoContext_->VideoProcessorSetStreamSourceRect(
        hlgVideoProcessor_.Get(), 0, TRUE, &sourceRect);
    hlgD3D11VideoContext_->VideoProcessorSetStreamDestRect(
        hlgVideoProcessor_.Get(), 0, TRUE, &outputRect);
    hlgD3D11VideoContext_->VideoProcessorSetOutputTargetRect(
        hlgVideoProcessor_.Get(), TRUE, &outputRect);
    hlgD3D11VideoContext_->VideoProcessorSetStreamAutoProcessingMode(
        hlgVideoProcessor_.Get(), 0, FALSE);
    hlgD3D11VideoContext_->VideoProcessorSetStreamColorSpace1(
        hlgVideoProcessor_.Get(), 0,
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020);
    hlgD3D11VideoContext_->VideoProcessorSetOutputColorSpace1(
        hlgVideoProcessor_.Get(), hlgColorSpace_);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = hlgVideoInputView_.Get();
    if (FAILED(hlgD3D11VideoContext_->VideoProcessorBlt(
            hlgVideoProcessor_.Get(), hlgVideoOutputView_.Get(),
            0, 1, &stream))) {
        Log(LogLevel::Warning,
            L"HLG native present failed reason=d3d11_video_processor_blt");
        return false;
    }
    const uint64_t rgbConsumedFenceValue = hlgSharedFenceValue_++;
    if (FAILED(hlgD3D11Context4_->Signal(
            hlgSharedFence11_.Get(), rgbConsumedFenceValue))) {
        Log(LogLevel::Warning,
            L"HLG native present failed reason=cross_api_signal");
        return false;
    }
    hlgSharedAvailableFenceValue_ = rgbConsumedFenceValue;
    if (!SelectCompositionSwapChain(true)) {
        Log(LogLevel::Warning,
            L"HLG native present failed reason=composition_select");
        return false;
    }
    const HRESULT presentResult = hlgSwapChain_->Present(1, 0);
    if (FAILED(presentResult)) {
        return !DeviceLost(presentResult, L"HLG Present");
    }
    bufferFenceValues_[backBufferIndex] = copyFenceValue;
    hlgBufferFenceValues_[backBufferIndex] = copyFenceValue;
    return true;
}

bool D3D12VideoRenderer::CreatePipeline() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 4;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    // 60 DWORDs plus the SRV table (1) and root CBV (2) stay within D3D12's
    // 64-DWORD root-signature limit.
    parameters[1].Constants.Num32BitValues = 60;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 1;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(parameters));
    rootDesc.pParameters = parameters;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> rootBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rootBlob, &errors)) ||
        FAILED(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                            rootBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&rootSignature_)))) {
        return false;
    }

    constexpr char vertexShader[] = R"(
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
})";
    const std::string pixelShader = std::string(R"(
Texture2DArray<float> sourceY : register(t0);
Texture2DArray<float2> sourceUv : register(t1);
Texture2DArray<float> enhancementY : register(t2);
Texture2DArray<float2> enhancementUv : register(t3);
SamplerState linearClamp : register(s0);
cbuffer CompositionConstants : register(b0) {
    float4 sourceRect;
    float4 enhancementRect;
    uint4 modes;
    float4 tone;
    float4 hdr10PlusA;
    float4 hdr10PlusB;
    float4 hdr10PlusCurve[4];
    float4 hdrToneCurve[5];
};
float3 yuv_to_rgb(float y, float2 uv, bool forceBt2020) {
    bool full = modes.y != 0;
    bool tenBit = (modes.w & 1) != 0;
    float black = tenBit ? 64.0 / 1023.0 : 16.0 / 255.0;
    float lumaRange = tenBit ? 876.0 / 1023.0 : 219.0 / 255.0;
    float chromaRange = tenBit ? 896.0 / 1023.0 : 224.0 / 255.0;
    float yy = max(0.0, (y - (full ? 0.0 : black)) / (full ? 1.0 : lumaRange));
    float cb = (uv.x - 0.5) / (full ? 1.0 : chromaRange);
    float cr = (uv.y - 0.5) / (full ? 1.0 : chromaRange);
    float kr = forceBt2020 || modes.x == 3 ? 0.2627 : (modes.x == 2 ? 0.2990 : 0.2126);
    float kb = forceBt2020 || modes.x == 3 ? 0.0593 : (modes.x == 2 ? 0.1140 : 0.0722);
    float kg = 1.0 - kr - kb;
    return max(float3(yy + (2.0 - 2.0 * kr) * cr,
                      yy - 2.0 * kb * (1.0 - kb) / kg * cb - 2.0 * kr * (1.0 - kr) / kg * cr,
                      yy + (2.0 - 2.0 * kb) * cb), 0.0);
}
float3 pq_to_nits(float3 v) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(v), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);
}
float3 nits_to_pq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 y = pow(max(nits / 10000.0, 0.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float3 hlg_to_nits(float3 v) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 scene = lerp((exp((v - c) / a) + b) / 12.0, v * v / 3.0, step(v, 0.5));
    scene=max(scene,0.0);
    float sceneLuma=max(dot(scene,float3(0.2627,0.6780,0.0593)),0.000001);
    return scene*pow(sceneLuma,0.2)*max(tone.z,1000.0);
}
float3 rec2020_to_scrgb(float3 v) {
    return mul(float3x3(1.6605, -0.5876, -0.0728,
                       -0.1246, 1.1329, -0.0083,
                       -0.0182, -0.1006, 1.1187), v);
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274, 0.3293, 0.0433,
                       0.0691, 0.9195, 0.0114,
                       0.0164, 0.0880, 0.8956), v);
}
float3 linear_to_srgb(float3 v) {
    v=max(v,0.0);
    return lerp(1.055*pow(v,1.0/2.4)-0.055,12.92*v,
                step(v,float3(0.0031308,0.0031308,0.0031308)));
}
float3 sdr_tone_map_nits(float3 nits,float sourcePeak,float3 weights) {
    nits=max(nits,0.0);
    float luma=max(dot(nits,weights),0.000001);
    uint toneMapMode=(modes.w>>2)&3;
    float exposure=toneMapMode==2?0.72:(toneMapMode==3?0.95:0.80);
    float target=100.0;
    float knee=target*(toneMapMode==2?0.40:(toneMapMode==3?0.55:0.45));
    float shoulder=max(target-knee,0.0001);
    float working=luma*exposure;
    if(working<=knee) return nits*exposure;
    float peak=max(max(sourcePeak*exposure,working),target+1.0);
    float mapped=knee+shoulder*log(1.0+(working-knee)/shoulder)/
        max(log(1.0+(peak-knee)/shoulder),0.0001);
    return nits*(min(mapped,target)/luma);
}
float hdr10plus_curve(float t) {
    float position=saturate(t)*15.0; int lower=clamp((int)floor(position),0,14);
    int upper=lower+1;
    float a=hdr10PlusCurve[lower/4][lower%4];
    float b=hdr10PlusCurve[upper/4][upper%4];
    return lerp(a,b,position-lower);
}
float3 apply_hdr10plus(float3 sourceNits) {
    if(hdr10PlusA.x<0.5) return max(sourceNits,0.0);
    float3 nits=max(sourceNits,0.0); float maxRgb=max(nits.r,max(nits.g,nits.b));
    if(maxRgb<=0.0001) return nits;
    float x=saturate(maxRgb/max(hdr10PlusA.z,1.0));
    float kx=hdr10PlusA.w,ky=hdr10PlusB.x;
    float y=x<=kx?x*ky/kx:ky+(1.0-ky)*hdr10plus_curve((x-kx)/(1.0-kx));
    float mappedMax=y*max(hdr10PlusA.y,1.0); float3 mapped=nits*(mappedMax/maxRgb);
    float luma=max(dot(mapped,float3(0.2627,0.6780,0.0593)),0.0);
    return max(luma.xxx+(mapped-luma.xxx)*max(hdr10PlusB.z,0.0),0.0);
}
float2 hdr_curve_point(int index) {
    float4 pairs=hdrToneCurve[index/2];
    return (index&1)==0?pairs.xy:pairs.zw;
}
float hdr_curve_luma(float lumaNits) {
    float2 previous=hdr_curve_point(0);
    if(lumaNits<=previous.x) return max(previous.y,0.0);
    [unroll] for(int index=1;index<9;++index) {
        float2 next=hdr_curve_point(index);
        if(lumaNits<=next.x) {
            float amount=saturate((lumaNits-previous.x)/max(next.x-previous.x,0.0001));
            return max(lerp(previous.y,next.y,amount),0.0);
        }
        previous=next;
    }
    return max(previous.y,0.0);
}
float3 apply_hdr_tone_curve(float3 sourceNits) {
    float3 nits=max(sourceNits,0.0);
    float flags=floor(tone.w+0.5);
    if(fmod(flags,2.0)<0.5) return nits;
    float luma=max(dot(nits,float3(0.2627,0.6780,0.0593)),0.0);
    return nits*(hdr_curve_luma(luma)/max(luma,0.0001));
}
float3 apply_hdr_display_mapping(float3 sourceNits) {
    float3 nits=max(sourceNits,0.0); float flags=floor(tone.w+0.5);
    float peak=max(nits.r,max(nits.g,nits.b));
    if(flags<2.0 || peak<=0.0001) return nits;
    float target=max(tone.z,100.0),source=max(tone.y,peak);
    if(source<=target+1.0) return nits;
    uint toneMapMode=(modes.w>>2)&3;
    float knee=target*(toneMapMode==2?0.55:(toneMapMode==3?0.75:0.65));
    if(peak<=knee) return nits;
    float shoulder=max(target-knee,1.0);
    float denominator=max(1.0-exp(-(source-knee)/shoulder),0.0001);
    float mapped=knee+shoulder*(1.0-exp(-(min(peak,source)-knee)/shoulder))/denominator;
    return nits*(min(mapped,target)/peak);
}
)") + std::string(D3D12DolbyVisionHlsl()) + R"(
float dovi_cubic_weight(float x) {
    x=abs(x); if (x<=1.0) return (1.5*x-2.5)*x*x+1.0;
    if (x<2.0) return ((-0.5*x+2.5)*x-4.0)*x+2.0; return 0.0;
}
int2 dovi_clamp_texel(int2 p,uint w,uint h) { return int2(clamp(p.x,0,(int)w-1),clamp(p.y,0,(int)h-1)); }
float dovi_sample_el_y(float2 uv) {
    if (doviComposerScale[0].w<0.5) return enhancementY.SampleLevel(linearClamp,float3(uv,0),0);
    uint w,h,layers,levels; enhancementY.GetDimensions(0,w,h,layers,levels);
    float2 c=uv*float2(w,h)-0.5,f=frac(c); int2 base=int2(floor(c)); float sum=0.0,weight=0.0;
    [unroll] for(int j=-1;j<=2;++j) [unroll] for(int i=-1;i<=2;++i) {
        float q=dovi_cubic_weight(i-f.x)*dovi_cubic_weight(j-f.y);
        sum+=enhancementY.Load(int4(dovi_clamp_texel(base+int2(i,j),w,h),0,0))*q; weight+=q;
    }
    return saturate(sum/max(weight,0.000001));
}
float2 dovi_sample_el_uv(float2 uv) {
    uint w,h,layers,levels; enhancementUv.GetDimensions(0,w,h,layers,levels);
    // HEVC 4:2:0 chroma is left-sited. libplacebo represents this as
    // plane.shift_x=-0.5; in normalized coordinates that is one quarter of an
    // UV texel towards the right. Apply it before both linear and cubic EL
    // reconstruction so BL and FEL stay registered.
    uv.x+=0.25/max((float)w,1.0);
    if (doviComposerScale[0].w<0.5) return enhancementUv.SampleLevel(linearClamp,float3(uv,0),0);
    float2 c=uv*float2(w,h)-0.5,f=frac(c); int2 base=int2(floor(c)); float2 sum=0.0; float weight=0.0;
    [unroll] for(int j=-1;j<=2;++j) [unroll] for(int i=-1;i<=2;++i) {
        float q=dovi_cubic_weight(i-f.x)*dovi_cubic_weight(j-f.y);
        sum+=enhancementUv.Load(int4(dovi_clamp_texel(base+int2(i,j),w,h),0,0))*q; weight+=q;
    }
    return saturate(sum/max(weight,0.000001));
}
float dovi_chroma_site_luma(float2 uv) {
    uint w,h,layers,levels; sourceY.GetDimensions(0,w,h,layers,levels);
    float2 c=uv*float2(w,h)-0.5; int2 base=int2(floor(c)); float4 weights=float4(1,3,3,1)/8.0; float sum=0.0;
    [unroll] for(int j=0;j<4;++j) [unroll] for(int i=0;i<4;++i)
        sum+=sourceY.Load(int4(dovi_clamp_texel(base+int2(i-1,j-1),w,h),0,0))*weights[i]*weights[j];
    return saturate(sum);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float2 displayUv=saturate(uv);
    if (doviSignalMeta[0].x>0.5 &&
        (displayUv.x<doviTrimC[0].z || displayUv.y<doviTrimC[0].w ||
         displayUv.x>doviTrimD[0].x || displayUv.y>doviTrimD[0].y)) return float4(0,0,0,1);
    float2 sampleUv = lerp(sourceRect.xy, sourceRect.zw, displayUv);
    float y=sourceY.Sample(linearClamp,float3(sampleUv,0.0));
    uint chromaW,chromaH,chromaLayers,chromaLevels;
    sourceUv.GetDimensions(0,chromaW,chromaH,chromaLayers,chromaLevels);
    float2 chromaUv=sampleUv;
    // Match libplacebo's PL_CHROMA_LEFT / shift_x=-0.5 convention. Omitting
    // this offset is especially visible for Profile 5 because its chroma
    // channels carry IPT rather than ordinary YCbCr.
    chromaUv.x+=0.25/max((float)chromaW,1.0);
    float2 chroma=sourceUv.Sample(linearClamp,float3(chromaUv,0.0));
    float3 encoded;
    bool canonical2020=(modes.w&2)!=0;
    if (doviSignalMeta[0].x>1.5) {
        float2 elUv=lerp(enhancementRect.xy,enhancementRect.zw,displayUv);
        float3 composed=dovi_compose_p7_fel(0,float3(y,chroma),
            float3(dovi_chroma_site_luma(sampleUv),chroma),float3(dovi_sample_el_y(elUv),dovi_sample_el_uv(elUv)));
        encoded=dovi_decode_reshaped(0,composed); canonical2020=true;
    } else if (doviSignalMeta[0].x>0.5) {
        encoded=dovi_decode_single_layer(0,saturate(float3(y,chroma)*doviSignalMeta[0].w)); canonical2020=true;
    } else encoded=yuv_to_rgb(y,chroma,false);
    float3 linearNits;
    if (doviSignalMeta[0].x>0.5 || modes.z == 2) linearNits = pq_to_nits(encoded);
    else if (modes.z == 3) linearNits = hlg_to_nits(encoded);
    else linearNits = pow(saturate(encoded), 2.2) * 80.0;
    linearNits=apply_hdr10plus(linearNits);
    // L2/L3/L8 are per-frame creative trims, not an SDR-only tone map. Apply
    // them before either the SDR roll-off or the HDR10 PQ conversion so CMv4
    // scene changes remain visible in both output modes.
    if (doviSignalMeta[0].x>0.5) linearNits=dovi_apply_display_trim(0,linearNits,tone.z);
    linearNits=apply_hdr_tone_curve(linearNits);
    linearNits=apply_hdr_display_mapping(linearNits);
    if (tone.x < 0.5 && (doviSignalMeta[0].x>0.5 || modes.z == 2 || modes.z == 3))
        linearNits=sdr_tone_map_nits(linearNits,tone.y,
            canonical2020?float3(0.2627,0.6780,0.0593):float3(0.2126,0.7152,0.0722));
    if (tone.x >= 0.5) {
        if (!canonical2020) linearNits=rec709_to_rec2020(linearNits);
        return float4(nits_to_pq(max(linearNits,0.0)),1.0);
    }
    if (canonical2020) linearNits=rec2020_to_scrgb(linearNits);
    return float4(linear_to_srgb(max(linearNits,0.0)/100.0),1.0);
})";
    doviPixelShaderSource_ = pixelShader;
    constexpr char basePixelShader[] = R"(
Texture2DArray<float> sourceY : register(t0);
Texture2DArray<float2> sourceUv : register(t1);
SamplerState linearClamp : register(s0);
cbuffer CompositionConstants : register(b0) {
    float4 sourceRect;
    float4 enhancementRect;
    uint4 modes;
    float4 tone;
    float4 hdr10PlusA;
    float4 hdr10PlusB;
    float4 hdr10PlusCurve[4];
    float4 hdrToneCurve[5];
};
float3 yuv_to_rgb(float y, float2 uv) {
    bool full = modes.y != 0;
    bool tenBit = (modes.w & 1) != 0;
    float black = tenBit ? 64.0 / 1023.0 : 16.0 / 255.0;
    float lumaRange = tenBit ? 876.0 / 1023.0 : 219.0 / 255.0;
    float chromaRange = tenBit ? 896.0 / 1023.0 : 224.0 / 255.0;
    float yy = max(0.0, (y - (full ? 0.0 : black)) / (full ? 1.0 : lumaRange));
    float cb = (uv.x - 0.5) / (full ? 1.0 : chromaRange);
    float cr = (uv.y - 0.5) / (full ? 1.0 : chromaRange);
    float kr = modes.x == 3 ? 0.2627 : (modes.x == 2 ? 0.2990 : 0.2126);
    float kb = modes.x == 3 ? 0.0593 : (modes.x == 2 ? 0.1140 : 0.0722);
    float kg = 1.0 - kr - kb;
    return max(float3(yy + (2.0 - 2.0 * kr) * cr,
                      yy - 2.0 * kb * (1.0 - kb) / kg * cb -
                           2.0 * kr * (1.0 - kr) / kg * cr,
                      yy + (2.0 - 2.0 * kb) * cb), 0.0);
}
float3 pq_to_nits(float3 v) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(v), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) /
                         max(c2 - c3 * p, 0.000001), 1.0 / m1);
}
float3 hlg_to_nits(float3 v) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 scene = lerp((exp((v - c) / a) + b) / 12.0,
                        v * v / 3.0, step(v, 0.5));
    scene=max(scene,0.0);
    float sceneLuma=max(dot(scene,float3(0.2627,0.6780,0.0593)),0.000001);
    return scene*pow(sceneLuma,0.2)*max(tone.z,1000.0);
}
float3 hlg_to_g22_bt2020(float3 v) {
    const float a=0.17883277,b=0.28466892,c=0.55991073;
    v=saturate(v);
    float3 scene=lerp((exp((v-c)/a)+b)/12.0,v*v/3.0,step(v,0.5));
    return pow(saturate(scene),1.0/2.2);
}
float3 rec2020_to_scrgb(float3 v) {
    return mul(float3x3(1.6605, -0.5876, -0.0728,
                       -0.1246, 1.1329, -0.0083,
                       -0.0182, -0.1006, 1.1187), v);
}
float3 nits_to_pq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 y=pow(max(nits/10000.0,0.0),m1);
    return pow((c1+c2*y)/(1.0+c3*y),m2);
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274,0.3293,0.0433,
                       0.0691,0.9195,0.0114,
                       0.0164,0.0880,0.8956),v);
}
float3 linear_to_srgb(float3 v) {
    v=max(v,0.0);
    return lerp(1.055*pow(v,1.0/2.4)-0.055,12.92*v,
                step(v,float3(0.0031308,0.0031308,0.0031308)));
}
float3 sdr_tone_map_nits(float3 nits,float sourcePeak,float3 weights) {
    nits=max(nits,0.0); float luma=max(dot(nits,weights),0.000001);
    uint toneMapMode=(modes.w>>2)&3;
    float exposure=toneMapMode==2?0.72:(toneMapMode==3?0.95:0.80);
    float target=100.0;
    float knee=target*(toneMapMode==2?0.40:(toneMapMode==3?0.55:0.45));
    float shoulder=max(target-knee,0.0001); float working=luma*exposure;
    if(working<=knee) return nits*exposure;
    float peak=max(max(sourcePeak*exposure,working),target+1.0);
    float mapped=knee+shoulder*log(1.0+(working-knee)/shoulder)/
        max(log(1.0+(peak-knee)/shoulder),0.0001);
    return nits*(min(mapped,target)/luma);
}
float hdr10plus_curve(float t) {
    float position=saturate(t)*15.0; int lower=clamp((int)floor(position),0,14),upper=lower+1;
    float a=hdr10PlusCurve[lower/4][lower%4],b=hdr10PlusCurve[upper/4][upper%4];
    return lerp(a,b,position-lower);
}
float3 apply_hdr10plus(float3 sourceNits) {
    if(hdr10PlusA.x<0.5) return max(sourceNits,0.0);
    float3 nits=max(sourceNits,0.0); float maxRgb=max(nits.r,max(nits.g,nits.b));
    if(maxRgb<=0.0001) return nits;
    float x=saturate(maxRgb/max(hdr10PlusA.z,1.0)),kx=hdr10PlusA.w,ky=hdr10PlusB.x;
    float y=x<=kx?x*ky/kx:ky+(1.0-ky)*hdr10plus_curve((x-kx)/(1.0-kx));
    float3 mapped=nits*(y*max(hdr10PlusA.y,1.0)/maxRgb);
    float luma=max(dot(mapped,float3(0.2627,0.6780,0.0593)),0.0);
    return max(luma.xxx+(mapped-luma.xxx)*max(hdr10PlusB.z,0.0),0.0);
}
float2 hdr_curve_point(int index) {
    float4 pairs=hdrToneCurve[index/2];
    return (index&1)==0?pairs.xy:pairs.zw;
}
float hdr_curve_luma(float lumaNits) {
    float2 previous=hdr_curve_point(0);
    if(lumaNits<=previous.x) return max(previous.y,0.0);
    [unroll] for(int index=1;index<9;++index) {
        float2 next=hdr_curve_point(index);
        if(lumaNits<=next.x) {
            float amount=saturate((lumaNits-previous.x)/max(next.x-previous.x,0.0001));
            return max(lerp(previous.y,next.y,amount),0.0);
        }
        previous=next;
    }
    return max(previous.y,0.0);
}
float3 apply_hdr_tone_curve(float3 sourceNits) {
    float3 nits=max(sourceNits,0.0);
    float flags=floor(tone.w+0.5);
    if(fmod(flags,2.0)<0.5) return nits;
    float luma=max(dot(nits,float3(0.2627,0.6780,0.0593)),0.0);
    return nits*(hdr_curve_luma(luma)/max(luma,0.0001));
}
float3 apply_hdr_display_mapping(float3 sourceNits) {
    float3 nits=max(sourceNits,0.0); float flags=floor(tone.w+0.5);
    float peak=max(nits.r,max(nits.g,nits.b));
    if(flags<2.0 || peak<=0.0001) return nits;
    float target=max(tone.z,100.0),source=max(tone.y,peak);
    if(source<=target+1.0) return nits;
    uint toneMapMode=(modes.w>>2)&3;
    float knee=target*(toneMapMode==2?0.55:(toneMapMode==3?0.75:0.65));
    if(peak<=knee) return nits;
    float shoulder=max(target-knee,1.0);
    float denominator=max(1.0-exp(-(source-knee)/shoulder),0.0001);
    float mapped=knee+shoulder*(1.0-exp(-(min(peak,source)-knee)/shoulder))/denominator;
    return nits*(min(mapped,target)/peak);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float2 sampleUv = lerp(sourceRect.xy, sourceRect.zw, saturate(uv));
    float3 encoded = yuv_to_rgb(
        sourceY.Sample(linearClamp, float3(sampleUv, 0.0)),
        sourceUv.Sample(linearClamp, float3(sampleUv, 0.0)));
    // RGB10 is the subtitle/UI composition surface for native HLG. The video
    // processor performs the only RGB-to-P010 conversion after this pass.
    if (tone.x > 1.5 && modes.z == 3)
        return float4(hlg_to_g22_bt2020(encoded),1.0);
    if (tone.x < 0.5 && modes.z == 1 && (modes.w & 2) == 0)
        return float4(saturate(encoded),1.0);
    float3 linearNits = modes.z == 2 ? pq_to_nits(encoded) :
                        (modes.z == 3 ? hlg_to_nits(encoded) :
                         pow(saturate(encoded), 2.2) * 80.0);
    linearNits=apply_hdr10plus(linearNits);
    linearNits=apply_hdr_tone_curve(linearNits);
    linearNits=apply_hdr_display_mapping(linearNits);
    if (tone.x < 0.5 && (modes.z == 2 || modes.z == 3))
        linearNits=sdr_tone_map_nits(linearNits,tone.y,
            (modes.w&2)!=0?float3(0.2627,0.6780,0.0593):float3(0.2126,0.7152,0.0722));
    bool canonical2020=(modes.w&2)!=0;
    if (tone.x >= 0.5) {
        if (!canonical2020) linearNits=rec709_to_rec2020(linearNits);
        return float4(nits_to_pq(max(linearNits,0.0)),1.0);
    }
    if (canonical2020) linearNits=rec2020_to_scrgb(linearNits);
    return float4(linear_to_srgb(max(linearNits,0.0)/100.0),1.0);
})";
    const std::string rgbPixelShader = std::string(R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer CompositionConstants : register(b0) {
    float4 sourceRect;
    float4 enhancementRect;
    uint4 modes;
    float4 tone;
    float4 hdr10PlusA;
    float4 hdr10PlusB;
    float4 hdr10PlusCurve[4];
    float4 hdrToneCurve[5];
};
float3 pq_to_nits(float3 v) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 p=pow(saturate(v),1.0/m2);
    return 10000.0*pow(max(p-c1,0.0)/max(c2-c3*p,0.000001),1.0/m1);
}
float3 nits_to_pq(float3 nits) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 y=pow(max(nits/10000.0,0.0),m1);
    return pow((c1+c2*y)/(1.0+c3*y),m2);
}
float3 rec2020_to_rec709(float3 v) {
    return mul(float3x3(1.6605,-0.5876,-0.0728,
                       -0.1246,1.1329,-0.0083,
                       -0.0182,-0.1006,1.1187),v);
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274,0.3293,0.0433,
                       0.0691,0.9195,0.0114,
                       0.0164,0.0880,0.8956),v);
}
float3 srgb_to_linear(float3 v) {
    return lerp(pow((v+0.055)/1.055,2.4),v/12.92,
                step(v,float3(0.04045,0.04045,0.04045)));
}
float3 linear_to_srgb(float3 v) {
    v=max(v,0.0);
    return lerp(1.055*pow(v,1.0/2.4)-0.055,12.92*v,
                step(v,float3(0.0031308,0.0031308,0.0031308)));
}
float3 hlg_to_scene(float3 v) {
    const float a=0.17883277,b=0.28466892,c=0.55991073;
    v=saturate(v);
    return lerp((exp((v-c)/a)+b)/12.0,v*v/3.0,step(v,0.5));
}
float3 hlg_to_nits(float3 v) {
    float3 scene=max(hlg_to_scene(v),0.0);
    float luma=max(dot(scene,float3(0.2627,0.6780,0.0593)),0.000001);
    return scene*pow(luma,0.2)*max(tone.z,1000.0);
}
float3 sdr_tone_map_nits(float3 nits,float sourcePeak) {
    nits=max(nits,0.0); float luma=max(dot(nits,float3(0.2627,0.6780,0.0593)),0.000001);
    uint toneMapMode=(modes.w>>2)&3;
    float exposure=toneMapMode==2?0.72:(toneMapMode==3?0.95:0.80);
    float target=100.0;
    float knee=target*(toneMapMode==2?0.40:(toneMapMode==3?0.55:0.45));
    float shoulder=max(target-knee,0.0001); float working=luma*exposure;
    if(working<=knee) return nits*exposure;
    float peak=max(max(sourcePeak*exposure,working),target+1.0);
    float mapped=knee+shoulder*log(1.0+(working-knee)/shoulder)/
        max(log(1.0+(peak-knee)/shoulder),0.0001);
    return nits*(min(mapped,target)/luma);
}
float hdr10plus_curve(float t) {
    float position=saturate(t)*15.0; int lower=clamp((int)floor(position),0,14),upper=lower+1;
    float a=hdr10PlusCurve[lower/4][lower%4],b=hdr10PlusCurve[upper/4][upper%4];
    return lerp(a,b,position-lower);
}
float3 apply_hdr10plus(float3 sourceNits) {
    if(hdr10PlusA.x<0.5) return max(sourceNits,0.0);
    float3 nits=max(sourceNits,0.0); float maxRgb=max(nits.r,max(nits.g,nits.b));
    if(maxRgb<=0.0001) return nits;
    float x=saturate(maxRgb/max(hdr10PlusA.z,1.0)),kx=hdr10PlusA.w,ky=hdr10PlusB.x;
    float y=x<=kx?x*ky/kx:ky+(1.0-ky)*hdr10plus_curve((x-kx)/(1.0-kx));
    float3 mapped=nits*(y*max(hdr10PlusA.y,1.0)/maxRgb);
    float luma=max(dot(mapped,float3(0.2627,0.6780,0.0593)),0.0);
    return max(luma.xxx+(mapped-luma.xxx)*max(hdr10PlusB.z,0.0),0.0);
}
float2 hdr_curve_point(int index) {
    float4 pairs=hdrToneCurve[index/2];
    return (index&1)==0?pairs.xy:pairs.zw;
}
float hdr_curve_luma(float lumaNits) {
    float2 previous=hdr_curve_point(0);
    if(lumaNits<=previous.x) return max(previous.y,0.0);
    [unroll] for(int index=1;index<9;++index) {
        float2 next=hdr_curve_point(index);
        if(lumaNits<=next.x) {
            float amount=saturate((lumaNits-previous.x)/max(next.x-previous.x,0.0001));
            return max(lerp(previous.y,next.y,amount),0.0);
        }
        previous=next;
    }
    return max(previous.y,0.0);
}
float3 apply_hdr_tone_curve(float3 sourceNits) {
    float3 nits=max(sourceNits,0.0);
    float flags=floor(tone.w+0.5);
    if(fmod(flags,2.0)<0.5) return nits;
    float luma=max(dot(nits,float3(0.2627,0.6780,0.0593)),0.0);
    return nits*(hdr_curve_luma(luma)/max(luma,0.0001));
}
float3 apply_hdr_display_mapping(float3 sourceNits) {
    float3 nits=max(sourceNits,0.0); float flags=floor(tone.w+0.5);
    float peak=max(nits.r,max(nits.g,nits.b));
    if(flags<2.0 || peak<=0.0001) return nits;
    float target=max(tone.z,100.0),source=max(tone.y,peak);
    if(source<=target+1.0) return nits;
    uint toneMapMode=(modes.w>>2)&3;
    float knee=target*(toneMapMode==2?0.55:(toneMapMode==3?0.75:0.65));
    if(peak<=knee) return nits;
    float shoulder=max(target-knee,1.0);
    float denominator=max(1.0-exp(-(source-knee)/shoulder),0.0001);
    float mapped=knee+shoulder*(1.0-exp(-(min(peak,source)-knee)/shoulder))/denominator;
    return nits*(min(mapped,target)/peak);
}
)") + std::string(D3D12DolbyVisionHlsl()) + R"(
float4 main(float4 position : SV_POSITION,float2 uv : TEXCOORD0) : SV_Target {
    float2 displayUv=saturate(uv);
    if(doviSignalMeta[0].x>0.5 &&
       (displayUv.x<doviTrimC[0].z || displayUv.y<doviTrimC[0].w ||
        displayUv.x>doviTrimD[0].x || displayUv.y>doviTrimD[0].y))
        return float4(0,0,0,1);
    float3 encoded=source.Sample(linearClamp,lerp(sourceRect.xy,sourceRect.zw,displayUv)).rgb;
    if(modes.z==3) {
        if(tone.x>1.5)
            return float4(pow(saturate(hlg_to_scene(encoded)),1.0/2.2),1.0);
        float3 nits=hlg_to_nits(encoded);
        if(tone.x>=0.5)
            return float4(nits_to_pq(max(nits,0.0)),1.0);
        nits=rec2020_to_rec709(sdr_tone_map_nits(nits,tone.y));
        return float4(linear_to_srgb(max(nits,0.0)/100.0),1.0);
    }
    if(modes.z==2) {
        float3 nits=apply_hdr10plus(pq_to_nits(encoded));
        if(doviSignalMeta[0].x>0.5)
            nits=dovi_apply_display_trim(0,nits,tone.z);
        nits=apply_hdr_tone_curve(nits);
        nits=apply_hdr_display_mapping(nits);
        if(tone.x>=0.5)
            return float4(nits_to_pq(max(nits,0.0)),1.0);
        nits=rec2020_to_rec709(sdr_tone_map_nits(nits,tone.y));
        return float4(linear_to_srgb(max(nits,0.0)/100.0),1.0);
    }
    if(modes.z==4) {
        float3 nits709=encoded*80.0;
        if(tone.x>=0.5)
            return float4(nits_to_pq(rec709_to_rec2020(nits709)),1.0);
        return float4(linear_to_srgb(max(nits709,0.0)/100.0),1.0);
    }
    if(tone.x>=0.5) {
        float3 nits709=srgb_to_linear(saturate(encoded))*80.0;
        return float4(nits_to_pq(rec709_to_rec2020(nits709)),1.0);
    }
    return float4(saturate(encoded),1.0);
})";
    Microsoft::WRL::ComPtr<ID3DBlob> vs;
    Microsoft::WRL::ComPtr<ID3DBlob> ps;
    Microsoft::WRL::ComPtr<ID3DBlob> rgbPs;
    if (FAILED(D3DCompile(vertexShader, std::strlen(vertexShader), nullptr, nullptr, nullptr,
                          "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
        FAILED(D3DCompile(basePixelShader, std::strlen(basePixelShader), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors)) ||
        FAILED(D3DCompile(rgbPixelShader.data(), rgbPixelShader.size(), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &rgbPs, &errors))) {
        if (errors && errors->GetBufferPointer()) {
            Log(LogLevel::Error,
                L"d3d12 shader compile failed: " +
                    Utf8ToWide(static_cast<const char*>(errors->GetBufferPointer())));
        }
        return false;
    }
    yuvVertexShader_ = vs;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature_.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = kCompositionFormat;
    desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&yuvPipeline_)))) {
        return false;
    }
    desc.PS = {rgbPs->GetBufferPointer(), rgbPs->GetBufferSize()};
    if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&rgbPipeline_)))) {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE tensorRange{};
    tensorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tensorRange.NumDescriptors = 4;
    tensorRange.BaseShaderRegister = 0;
    tensorRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER tensorParameters[2]{};
    tensorParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tensorParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    tensorParameters[0].DescriptorTable.pDescriptorRanges = &tensorRange;
    tensorParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    tensorParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    tensorParameters[1].Constants.ShaderRegister = 0;
    tensorParameters[1].Constants.Num32BitValues = 8;
    tensorParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC tensorRootDesc{};
    tensorRootDesc.NumParameters = static_cast<UINT>(std::size(tensorParameters));
    tensorRootDesc.pParameters = tensorParameters;
    D3D12_STATIC_SAMPLER_DESC tensorSampler{};
    tensorSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    tensorSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    tensorSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    tensorSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    tensorSampler.ShaderRegister = 0;
    tensorSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    tensorSampler.MaxLOD = D3D12_FLOAT32_MAX;
    tensorRootDesc.NumStaticSamplers = 1;
    tensorRootDesc.pStaticSamplers = &tensorSampler;
    tensorRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rootBlob.Reset();
    errors.Reset();
    if (FAILED(D3D12SerializeRootSignature(&tensorRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rootBlob, &errors)) ||
        FAILED(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                            rootBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&tensorRootSignature_)))) {
        return false;
    }
    constexpr char tensorPixelShader[] = R"(
ByteAddressBuffer generated : register(t0);
ByteAddressBuffer endpoints : register(t1);
Texture2D<float4> leftEndpointReference : register(t2);
Texture2D<float4> rightEndpointReference : register(t3);
SamplerState endpointLinearClamp : register(s0);
cbuffer TensorCompositionConstants : register(b0) {
    uint2 tensorSize;
    float hdrOutput;
    uint inputTransfer;
    float4 endpointReference;
};
float load_half(uint plane, uint2 position) {
    uint element = (plane * tensorSize.y + position.y) * tensorSize.x + position.x;
    uint byteOffset = element * 2;
    uint packed = generated.Load(byteOffset & ~3u);
    uint value = (byteOffset & 2u) != 0 ? packed >> 16 : packed & 0xffffu;
    return f16tof32(value);
}
float3 load_rgb(uint2 position) {
    return float3(load_half(0, position), load_half(1, position), load_half(2, position));
}
float load_endpoint_half(uint plane, uint2 position) {
    uint element = (plane * tensorSize.y + position.y) * tensorSize.x + position.x;
    uint byteOffset = element * 2;
    uint packed = endpoints.Load(byteOffset & ~3u);
    uint value = (byteOffset & 2u) != 0 ? packed >> 16 : packed & 0xffffu;
    return f16tof32(value);
}
float3 load_endpoint_rgb(uint firstPlane, uint2 position) {
    return float3(load_endpoint_half(firstPlane, position),
                  load_endpoint_half(firstPlane + 1, position),
                  load_endpoint_half(firstPlane + 2, position));
}
float3 sample_rgb(float2 uv) {
    float2 p = saturate(uv) * float2(tensorSize) - 0.5;
    int2 p0 = int2(floor(p));
    float2 f = frac(p);
    uint2 a = uint2(clamp(p0, int2(0, 0), int2(tensorSize) - 1));
    uint2 b = uint2(clamp(p0 + int2(1, 0), int2(0, 0), int2(tensorSize) - 1));
    uint2 c = uint2(clamp(p0 + int2(0, 1), int2(0, 0), int2(tensorSize) - 1));
    uint2 d = uint2(clamp(p0 + int2(1, 1), int2(0, 0), int2(tensorSize) - 1));
    return lerp(lerp(load_rgb(a), load_rgb(b), f.x),
                lerp(load_rgb(c), load_rgb(d), f.x), f.y);
}
float3 sample_endpoint_rgb(float2 uv, uint firstPlane) {
    float2 p = saturate(uv) * float2(tensorSize) - 0.5;
    int2 p0 = int2(floor(p));
    float2 f = frac(p);
    uint2 a = uint2(clamp(p0, int2(0, 0), int2(tensorSize) - 1));
    uint2 b = uint2(clamp(p0 + int2(1, 0), int2(0, 0), int2(tensorSize) - 1));
    uint2 c = uint2(clamp(p0 + int2(0, 1), int2(0, 0), int2(tensorSize) - 1));
    uint2 d = uint2(clamp(p0 + int2(1, 1), int2(0, 0), int2(tensorSize) - 1));
    return lerp(lerp(load_endpoint_rgb(firstPlane, a),
                     load_endpoint_rgb(firstPlane, b), f.x),
                lerp(load_endpoint_rgb(firstPlane, c),
                     load_endpoint_rgb(firstPlane, d), f.x), f.y);
}
float3 pq_to_nits(float3 v) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(v), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);
}
float3 nits_to_pq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 y=pow(max(nits/10000.0,0.0),m1);
    return pow((c1+c2*y)/(1.0+c3*y),m2);
}
float3 rec2020_to_rec709(float3 v) {
    return mul(float3x3(1.6605, -0.5876, -0.0728,
                       -0.1246, 1.1329, -0.0083,
                       -0.0182, -0.1006, 1.1187), v);
}
float3 linear_to_srgb(float3 v) {
    v=max(v,0.0);
    return lerp(1.055*pow(v,1.0/2.4)-0.055,12.92*v,
                step(v,float3(0.0031308,0.0031308,0.0031308)));
}
float3 srgb_to_linear(float3 v) {
    return lerp(pow((v+0.055)/1.055,2.4),v/12.92,
                step(v,float3(0.04045,0.04045,0.04045)));
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274,0.3293,0.0433,
                       0.0691,0.9195,0.0114,
                       0.0164,0.0880,0.8956),v);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 generatedRgb=sample_rgb(uv);
    if (inputTransfer == 1) {
        if (hdrOutput < 0.5) return float4(saturate(generatedRgb),1.0);
        float3 nits2020=rec709_to_rec2020(srgb_to_linear(saturate(generatedRgb))*80.0);
        return float4(nits_to_pq(nits2020),1.0);
    }
    if (inputTransfer == 4) {
        float3 mappedNits=pq_to_nits(generatedRgb);
        // RIFE is not constrained to preserve HDR energy. Plane 6 holds the
        // exact interpolation time used for this inference, so the original
        // DV endpoints can provide a stable exposure reference. Never replace
        // the generated luma with an endpoint cross-fade: luma carries most of
        // the motion structure and doing so makes valid interpolation look like
        // ordinary playback. Apply only a small bounded exposure correction.
        float3 leftNits;
        float3 rightNits;
        if(endpointReference.x>=0.5) {
            // These are the same presentation-resolution scRGB endpoints used
            // by the original DV path (linear BT.709, 1.0 = 80 nits). Sampling
            // them here prevents a 4K original from alternating with a
            // low-resolution tensor-derived luminance field in fullscreen.
            leftNits=rec709_to_rec2020(
                max(leftEndpointReference.SampleLevel(endpointLinearClamp,saturate(uv),0).rgb,0.0)*80.0);
            rightNits=rec709_to_rec2020(
                max(rightEndpointReference.SampleLevel(endpointLinearClamp,saturate(uv),0).rgb,0.0)*80.0);
        } else {
            leftNits=pq_to_nits(sample_endpoint_rgb(uv,0));
            rightNits=pq_to_nits(sample_endpoint_rgb(uv,3));
        }
        float3 weights=float3(0.2627,0.6780,0.0593);
        float generatedLuma=max(dot(mappedNits,weights),0.0);
        float interpolationT=saturate(load_endpoint_half(6,uint2(0,0)));
        float3 referenceNits=lerp(leftNits,rightNits,interpolationT);
        float referenceLuma=max(dot(referenceNits,weights),0.0);
        if(generatedLuma>0.0001) {
            float logExposureDelta=clamp(
                log2(max(referenceLuma,0.01)/max(generatedLuma,0.01)),
                -0.35,0.35);
            mappedNits*=exp2(logExposureDelta*0.25);
        }
        if (hdrOutput >= 0.5) return float4(nits_to_pq(max(mappedNits,0.0)),1.0);
        mappedNits=rec2020_to_rec709(mappedNits);
        return float4(linear_to_srgb(max(mappedNits,0.0)/100.0),1.0);
    }
    // No source-domain HDR input is legal here. All HDR/DV transforms belong
    // to the two endpoint preprocessors so the ML result cannot be tone-mapped
    // a second time with either endpoint's metadata.
    return float4(0.0,0.0,0.0,1.0);
})";
    Microsoft::WRL::ComPtr<ID3DBlob> tensorPs;
    errors.Reset();
    if (FAILED(D3DCompile(tensorPixelShader, std::strlen(tensorPixelShader), nullptr,
                          nullptr, nullptr, "main", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &tensorPs, &errors))) {
        if (errors && errors->GetBufferPointer()) {
            Log(LogLevel::Error,
                L"d3d12 tensor shader compile failed: " +
                    Utf8ToWide(static_cast<const char*>(errors->GetBufferPointer())));
        }
        return false;
    }
    desc.pRootSignature = tensorRootSignature_.Get();
    desc.PS = {tensorPs->GetBufferPointer(), tensorPs->GetBufferSize()};
    if (FAILED(device_->CreateGraphicsPipelineState(&desc,
                                                     IID_PPV_ARGS(&tensorPipeline_)))) {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE overlayRange{};
    overlayRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    overlayRange.NumDescriptors = 1;
    overlayRange.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER overlayParameters[2]{};
    overlayParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    overlayParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    overlayParameters[0].DescriptorTable.pDescriptorRanges = &overlayRange;
    overlayParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    overlayParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    overlayParameters[1].Constants.ShaderRegister = 0;
    overlayParameters[1].Constants.Num32BitValues = 8;
    overlayParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC overlaySampler{};
    overlaySampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    overlaySampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    overlaySampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    overlaySampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    overlaySampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    overlaySampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC overlayRootDesc{};
    overlayRootDesc.NumParameters = static_cast<UINT>(std::size(overlayParameters));
    overlayRootDesc.pParameters = overlayParameters;
    overlayRootDesc.NumStaticSamplers = 1;
    overlayRootDesc.pStaticSamplers = &overlaySampler;
    overlayRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rootBlob.Reset();
    errors.Reset();
    if (FAILED(D3D12SerializeRootSignature(&overlayRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rootBlob, &errors)) ||
        FAILED(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                            rootBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&overlayRootSignature_)))) {
        return false;
    }
    constexpr char overlayPixelShader[] = R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer OverlayConstants : register(b0) {
    float opacity;
    uint alphaFromRgb;
    float sdrWhiteNits;
    float hdrComposition;
    float4 padding;
};
float3 srgb_to_linear(float3 c) {
    return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92,
                step(c, float3(0.04045, 0.04045, 0.04045)));
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274,0.3293,0.0433,
                       0.0691,0.9195,0.0114,
                       0.0164,0.0880,0.8956),v);
}
float3 nits_to_pq(float3 nits) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 y=pow(max(nits/10000.0,0.0),m1);
    return pow((c1+c2*y)/(1.0+c3*y),m2);
}
float3 linear_to_g22(float3 linearRgb) {
    return pow(saturate(linearRgb),1.0/2.2);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float4 color = source.Sample(linearClamp, saturate(uv));
    float alpha;
    float3 straight;
    if (alphaFromRgb != 0) {
        // GDI leaves the alpha channel at zero. Match the D3D11 path: RGB is a
        // binary coverage mask, while the original RGB values remain the UI
        // color. Deriving continuous alpha from RGB makes dark menu panels
        // nearly transparent and then incorrectly normalizes their color.
        alpha = max(color.r, max(color.g, color.b)) > (0.5 / 255.0)
            ? saturate(opacity) : 0.0;
        straight = saturate(color.rgb);
    } else {
        alpha = saturate(color.a * opacity);
        straight = saturate(color.rgb / max(color.a, 0.00001));
    }
    if (alpha <= 0.00001) return 0.0;
    if (hdrComposition < 0.5) return float4(straight * alpha, alpha);
    float3 linear2020=rec709_to_rec2020(srgb_to_linear(straight));
    if (hdrComposition > 1.5) {
        float3 g22=linear_to_g22(
            linear2020*max(sdrWhiteNits,1.0)/1000.0);
        return float4(g22*alpha,alpha);
    }
    float3 nits2020=linear2020*max(sdrWhiteNits,1.0);
    return float4(nits_to_pq(nits2020)*alpha,alpha);
})";
    Microsoft::WRL::ComPtr<ID3DBlob> overlayPs;
    errors.Reset();
    if (FAILED(D3DCompile(overlayPixelShader, std::strlen(overlayPixelShader), nullptr,
                          nullptr, nullptr, "main", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &overlayPs, &errors))) {
        return false;
    }
    desc.pRootSignature = overlayRootSignature_.Get();
    desc.PS = {overlayPs->GetBufferPointer(), overlayPs->GetBufferSize()};
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (FAILED(device_->CreateGraphicsPipelineState(&desc,
                                                     IID_PPV_ARGS(&overlayPipeline_)))) {
        return false;
    }
    const UINT64 constantBytes =
        (sizeof(DoviShaderConstantsPair) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
        ~(static_cast<UINT64>(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) - 1);
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = constantBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RANGE noRead{0, 0};
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&doviConstantBuffers_[index]))) ||
            FAILED(doviConstantBuffers_[index]->Map(
                0, &noRead, &doviConstantMappings_[index]))) {
            return false;
        }
    }
    return true;
}

bool D3D12VideoRenderer::CreateDolbyVisionPipeline() {
    if (!device_ || !rootSignature_ || !yuvVertexShader_ ||
        doviPixelShaderSource_.empty()) {
        return false;
    }

    const std::filesystem::path cachePath =
        DolbyVisionShaderCachePath(doviPixelShaderSource_);
    Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
    bool cacheHit = LoadShaderCache(cachePath, pixelShader);
    const auto compileShader = [&]() {
        pixelShader.Reset();
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            doviPixelShaderSource_.data(), doviPixelShaderSource_.size(), nullptr,
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL2,
            0, &pixelShader, &errors);
        if (FAILED(result)) {
            if (errors && errors->GetBufferPointer()) {
                Log(LogLevel::Error,
                    L"d3d12 Dolby Vision shader compile failed: " +
                        Utf8ToWide(static_cast<const char*>(errors->GetBufferPointer())));
            }
            pixelShader.Reset();
            return false;
        }
        return true;
    };
    if (!cacheHit && !compileShader()) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature_.Get();
    desc.VS = {yuvVertexShader_->GetBufferPointer(), yuvVertexShader_->GetBufferSize()};
    desc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = kCompositionFormat;
    desc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    HRESULT pipelineResult =
        device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(pipelineResult) && cacheHit) {
        // A partial/stale cache file must not make DV unavailable. Recompile it
        // from the embedded source and replace the cache only after PSO success.
        std::error_code error;
        std::filesystem::remove(cachePath, error);
        cacheHit = false;
        if (!compileShader()) return false;
        desc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        pipelineResult = device_->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&pipeline));
    }
    if (FAILED(pipelineResult)) return false;
    if (!cacheHit) StoreShaderCache(cachePath, pixelShader.Get());
    doviPipeline_ = std::move(pipeline);
    publishedDoviPipeline_.store(doviPipeline_.Get(), std::memory_order_release);
    Log(LogLevel::Info,
        std::wstring(L"d3d12 Dolby Vision shader cache=") +
            (cacheHit ? L"hit" : L"miss_compiled"));
    return true;
}

bool D3D12VideoRenderer::Resize(const UINT width, const UINT height) {
    if (!swapChain_ || width == 0 || height == 0 || (width == width_ && height == height_)) {
        return true;
    }
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (!WaitForBackBuffer(index)) return false;
    }
    ReleaseHlgPresentationResources();
    hlgInitializationAttempted_ = false;
    hlgFailureLogged_ = false;
    for (auto& target : libplaceboTargets_) target.Reset();
    for (auto& buffer : backBuffers_) buffer.Reset();
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    const HRESULT result = swapChain_->ResizeBuffers(kBufferCount, width, height,
                                                      kCompositionFormat,
                                                      swapChainWaitable_
                                                          ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                                                          : 0);
    if (FAILED(result)) return !DeviceLost(result, L"ResizeBuffers");
    ConfigureFramePacing();
    const DXGI_COLOR_SPACE_TYPE colorSpace = swapChainHdr_
        ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (FAILED(swapChain_->SetColorSpace1(colorSpace))) {
        if (swapChainHdr_) hdr10ColorSpaceSupported_ = false;
        swapChainHdr_ = false;
        swapChain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        Log(LogLevel::Warning,
            L"RGB10 swap-chain color space was lost after resize; using SDR fallback");
    }
    width_ = width;
    height_ = height;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffers_[index])))) return false;
        device_->CreateRenderTargetView(backBuffers_[index].Get(), nullptr, rtv);
        rtv.ptr += rtvIncrement_;
    }
    RefreshDisplayPeakNits();
    return CreateLibplaceboTargets(width, height);
}

bool D3D12VideoRenderer::CreateLibplaceboTargets(const UINT width, const UINT height) {
    for (auto& target : libplaceboTargets_) target.Reset();
    if (!libplaceboBridge_ || !libplaceboBridge_->IsReady()) return true;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = std::max<UINT>(1, width);
    desc.Height = std::max<UINT>(1, height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kLibplaceboTargetFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kLibplaceboTargetFormat;
    for (auto& target : libplaceboTargets_) {
        if (FAILED(device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                &clear, IID_PPV_ARGS(&target)))) {
            for (auto& created : libplaceboTargets_) created.Reset();
            libplaceboBridge_->Reset();
            libplaceboBridge_.reset();
            Log(LogLevel::Warning,
                L"libplacebo composition targets unavailable; using native D3D12 Dolby Vision path");
            return true;
        }
    }
    return true;
}

bool D3D12VideoRenderer::ConfigureFramePacing() {
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    if (!swapChain_ || !swapChainWaitable_) {
        if (!framePacingLogged_) {
            framePacingLogged_ = true;
            Log(LogLevel::Info,
                L"d3d12 frame pacing waitable=unavailable reason=swap_chain_flag "
                L"present_sync_interval=1");
        }
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (FAILED(swapChain_.As(&swapChain2)) || !swapChain2) {
        if (!framePacingLogged_) {
            framePacingLogged_ = true;
            Log(LogLevel::Info,
                L"d3d12 frame pacing waitable=unavailable reason=swapchain2 "
                L"present_sync_interval=1");
        }
        return false;
    }

    const HRESULT latencyResult = swapChain2->SetMaximumFrameLatency(1);
    if (FAILED(latencyResult)) {
        Log(LogLevel::Warning,
            L"d3d12 SetMaximumFrameLatency(1) failed; using driver presentation pacing");
    }
    frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
    if (!frameLatencyWaitable_) {
        if (!framePacingLogged_) {
            framePacingLogged_ = true;
            Log(LogLevel::Info,
                L"d3d12 frame pacing waitable=unavailable reason=no_handle "
                L"present_sync_interval=1");
        }
        return false;
    }

    if (!framePacingLogged_) {
        framePacingLogged_ = true;
        Log(LogLevel::Info,
            L"d3d12 frame pacing waitable=active max_frame_latency=1 "
            L"present_sync_interval=1");
    }
    return true;
}

void D3D12VideoRenderer::WaitForFrameLatencyObject() {
    // While the P010 media swap chain is selected, the ordinary RGB swap
    // chain is intentionally not presented and its waitable object will not
    // advance. The media Present call provides the pacing for this path.
    if (!frameLatencyWaitable_ || nativeHlgComposition_) return;
    const auto waitStartedAt = std::chrono::steady_clock::now();
    const DWORD waitResult = WaitForSingleObjectEx(
        frameLatencyWaitable_, kFrameLatencyWaitTimeoutMs, TRUE);
    const uint64_t waitUs = static_cast<uint64_t>(std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - waitStartedAt).count()));
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.frameLatencyWaits;
        renderStats_.frameLatencyWaitUs += waitUs;
        renderStats_.maxFrameLatencyWaitUs = std::max(
            renderStats_.maxFrameLatencyWaitUs, waitUs);
        if (waitResult == WAIT_TIMEOUT || waitResult == WAIT_FAILED) {
            ++renderStats_.frameLatencyWaitTimeouts;
        }
        publishedStats_ = renderStats_;
    }
}

void D3D12VideoRenderer::RefreshDisplayPeakNits() {
    const HWND window = host_.load(std::memory_order_acquire);
    const HMONITOR monitor = window && IsWindow(window)
                                 ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                 : nullptr;
    const bool monitorChanged = monitor != displayPeakMonitor_;
    if (monitorChanged || !displayPeakQueryAttempted_) {
        displayPeakMonitor_ = monitor;
        displayPeakQueryAttempted_ = true;
        const int detectedPeakNits = QueryMonitorPeakNits(factory_.Get(), monitor);
        detectedDisplayPeakNits_.store(detectedPeakNits, std::memory_order_release);
        displayCapabilities_.reportedPeakBrightnessNits = detectedPeakNits;
    }

    const int configuredPeakNits =
        SanitizeDisplayPeakNits(videoSettings_.displayPeakBrightnessNits);
    const int detectedPeakNits =
        detectedDisplayPeakNits_.load(std::memory_order_acquire);
    const int effectivePeakNits = configuredPeakNits > 0
                                      ? configuredPeakNits
                                      : (detectedPeakNits > 0 ? detectedPeakNits : 1000);
    const int previousPeakNits = effectiveDisplayPeakNits_.exchange(
        effectivePeakNits, std::memory_order_acq_rel);
    if (monitorChanged || previousPeakNits != effectivePeakNits) {
        Log(LogLevel::Info,
            L"hdr display peak mode=" +
                std::wstring(configuredPeakNits > 0 ? L"manual" : L"auto") +
                L" detected=" +
                (detectedPeakNits > 0 ? std::to_wstring(detectedPeakNits)
                                      : std::wstring(L"unavailable")) +
                L" effective=" + std::to_wstring(effectivePeakNits) + L" nits" +
                (configuredPeakNits == 0 && detectedPeakNits == 0
                     ? std::wstring(L" fallback=1000")
                     : std::wstring{}));
    }
}

bool D3D12VideoRenderer::TryRenderDolbyVisionWithLibplacebo(
    const NativeVideoFrame& frame,
    const UINT backBufferIndex,
    const std::chrono::steady_clock::time_point startedAt) {
    if (!libplaceboBridge_ || !libplaceboBridge_->IsReady() ||
        backBufferIndex >= kBufferCount || !libplaceboTargets_[backBufferIndex]) {
        return false;
    }

    GpuFencePoint computeDependency;
    const auto computeCompletion = sourceComputeCompletions_.find(
        {frame.timelineSerial, frame.serial});
    if (computeCompletion != sourceComputeCompletions_.end()) {
        computeDependency = computeCompletion->second;
    }
    const bool hdrOutput = SetHdrOutputState(
        frame, HdrOutputEnabled(frame, videoSettings_, displayCapabilities_,
                                hdr10ColorSpaceSupported_));
    const float targetPeakNits = TargetPeakNits(videoSettings_, displayCapabilities_);
    if (!libplaceboBridge_->RenderDolbyVision(
            frame, libplaceboTargets_[backBufferIndex].Get(), width_, height_, hdrOutput,
            targetPeakNits, computeDependency.fence.Get(), computeDependency.value)) {
        if (!libplaceboRenderFailureLogged_) {
            libplaceboRenderFailureLogged_ = true;
            Log(LogLevel::Warning,
                L"libplacebo frame render failed; retrying with native D3D12 Dolby Vision shader");
        }
        return false;
    }
    libplaceboRenderFailureLogged_ = false;
    if (computeCompletion != sourceComputeCompletions_.end()) {
        sourceComputeCompletions_.erase(computeCompletion);
    }

    if (FAILED(allocators_[backBufferIndex]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[backBufferIndex].Get(), rgbPipeline_.Get()))) {
        return false;
    }
    const std::array<D3D12_RESOURCE_BARRIER, 2> beginBarriers{
        TransitionBarrier(
            backBuffers_[backBufferIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
        TransitionBarrier(
            libplaceboTargets_[backBufferIndex].Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    commandList_->ResourceBarrier(
        static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(backBufferIndex * 4) * srvIncrement_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kLibplaceboTargetFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(libplaceboTargets_[backBufferIndex].Get(), &srv, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvIncrement_;
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[]{srvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(backBufferIndex * 4) * srvIncrement_;
    commandList_->SetGraphicsRootDescriptorTable(0, srvGpu);
    CompositionConstants constants{};
    constants.sourceUv[2] = 1.0f;
    constants.sourceUv[3] = 1.0f;
    constants.transfer = 4u;  // libplacebo scRGB intermediate
    constants.primaries = ToneMappingModeBits(videoSettings_.toneMapping);
    constants.hdrOutput = hdrOutput ? 1.0f : 0.0f;
    constants.sourcePeakNits = FrameSourcePeakNits(frame);
    constants.targetPeakNits = targetPeakNits;
    const bool fixedInterpolationDisplayDomain =
        interpolationRequested_.load(std::memory_order_acquire);
    constants.padding = InitialHdrProcessingFlags(
        frame, videoSettings_, hdrOutput, fixedInterpolationDisplayDomain);
    FillHdr10PlusConstants(constants, frame,
                           !nvidiaHdrOutput_.Hdr10PlusGamingActive());
    const uint64_t hdrToneCurveFingerprint = FillHdrToneCurveConstants(
        constants.hdrToneCurve, constants.padding, frame, videoSettings_, hdrOutput,
        fixedInterpolationDisplayDomain);
    if (hdrToneCurveFingerprint != 0 &&
        hdrToneCurveFingerprint != loggedHdrToneCurveFingerprint_) {
        loggedHdrToneCurveFingerprint_ = hdrToneCurveFingerprint;
        Log(LogLevel::Info, L"HDR custom tone curve active pipeline=libplacebo_scrgb");
    }
    // This bridge already owns the complete BL(+EL/FEL)+RPU composition. Its
    // Profile 7 L5 offsets describe the active image inside the source (often
    // an existing letterbox), so applying them again here would manufacture a
    // second post-reshape mask. Decoder-side P5/P8 RGB10 frames take the
    // separate RenderPixelFrame path, where synthetic L5 masks are handled.
    DoviShaderConstantsPair doviConstants{};
    std::memcpy(doviConstantMappings_[backBufferIndex], &doviConstants,
                sizeof(doviConstants));
    commandList_->SetGraphicsRoot32BitConstants(1, 60, &constants, 0);
    commandList_->SetGraphicsRootConstantBufferView(
        2, doviConstantBuffers_[backBufferIndex]->GetGPUVirtualAddress());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);
    BindPresentedSubtitles(frame);
    const D3D12_VIEWPORT subtitleViewport = AspectFitVideoViewport(
        frame.width, frame.height, width_, height_);
    DrawOverlays(subtitleViewport, backBufferIndex);

    const std::array<D3D12_RESOURCE_BARRIER, 2> endBarriers{
        TransitionBarrier(libplaceboTargets_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT)};
    commandList_->ResourceBarrier(
        static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(commandList_->Close())) return false;
    const SubmitResult graphSubmit = frameGraph_.Submit(
        GpuFrameQueue::Graphics, commandList_.Get());
    if (!graphSubmit.accepted) {
        Log(LogLevel::Error,
            L"frame graph libplacebo overlay submit failed reason=" + graphSubmit.reason);
        return false;
    }
    sourceGraphicsCompletions_[{frame.timelineSerial, frame.serial}] =
        graphSubmit.completion;

    if (!PresentComposedFrame(
            backBufferIndex, graphSubmit.completion, false)) {
        return false;
    }
    inFlightFrames_[backBufferIndex] = std::make_unique<NativeVideoFrame>(frame);
    AnchorGeneratedFrames(frame, std::chrono::steady_clock::now());

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.frames;
        ++renderStats_.hardwareFrames;
        renderStats_.totalRenderUs += static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
        renderStats_.maxRenderUs = std::max(
            renderStats_.maxRenderUs,
            static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
        publishedStats_ = renderStats_;
    }
    return true;
}

bool D3D12VideoRenderer::RenderPixelFrame(const NativeVideoFrame& frame) {
    if (!frame.HasPixels() || !rgbPipeline_ || !frame.bgra ||
        frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4) {
        return false;
    }
    const auto startedAt = std::chrono::steady_clock::now();
    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!WaitForBackBuffer(backBufferIndex)) return false;
    inFlightFrames_[backBufferIndex].reset();
    transients_[backBufferIndex].clear();

    const bool packedPq = frame.softwareFormat == AV_PIX_FMT_X2BGR10LE;
    const DXGI_FORMAT textureFormat = packedPq
        ? DXGI_FORMAT_R10G10B10A2_UNORM
        : DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT64>(frame.width);
    textureDesc.Height = static_cast<UINT>(frame.height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = textureFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    PixelFrameUploadCache& cache = pixelFrameUploadCaches_[backBufferIndex];
    const bool recreate = !cache.texture || !cache.upload || !cache.mappedUpload ||
        cache.width != static_cast<UINT>(frame.width) ||
        cache.height != static_cast<UINT>(frame.height) || cache.format != textureFormat;
    if (recreate) {
        if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
        cache = {};

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&cache.texture)))) {
            return false;
        }

        UINT64 uploadBytes = 0;
        device_->GetCopyableFootprints(
            &textureDesc, 0, 1, 0, &cache.footprint, &cache.rowCount,
            &cache.rowSize, &uploadBytes);
        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cache.upload)))) {
            cache.texture.Reset();
            return false;
        }
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(cache.upload->Map(0, &noRead, &cache.mappedUpload))) {
            cache = {};
            return false;
        }
        cache.format = textureFormat;
        cache.width = static_cast<UINT>(frame.width);
        cache.height = static_cast<UINT>(frame.height);
        cache.uploadCapacity = uploadBytes;
    }

    const auto* source = frame.bgra->data();
    auto* destination = static_cast<std::uint8_t*>(cache.mappedUpload) +
        cache.footprint.Offset;
    const std::size_t copyBytes = static_cast<std::size_t>(
        std::min<UINT64>(cache.rowSize, static_cast<UINT64>(frame.stride)));
    for (UINT row = 0; row < cache.rowCount; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) *
                        cache.footprint.Footprint.RowPitch,
                    source + static_cast<std::size_t>(row) * frame.stride,
                    copyBytes);
    }

    if (FAILED(allocators_[backBufferIndex]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[backBufferIndex].Get(), rgbPipeline_.Get()))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
    destinationLocation.pResource = cache.texture.Get();
    destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destinationLocation.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = cache.upload.Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = cache.footprint;
    commandList_->CopyTextureRegion(
        &destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
    const std::array beginBarriers{
        TransitionBarrier(cache.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET)};
    commandList_->ResourceBarrier(
        static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(backBufferIndex * 4) * srvIncrement_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = textureFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(cache.texture.Get(), &srv, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvIncrement_;
    constexpr float clearColor[4]{};
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    const D3D12_VIEWPORT viewport = AspectFitVideoViewport(
        frame.width, frame.height, width_, height_);
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[]{srvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(backBufferIndex * 4) * srvIncrement_;
    commandList_->SetGraphicsRootDescriptorTable(0, srvGpu);

    CompositionConstants constants{};
    constants.sourceUv[2] = 1.0f;
    constants.sourceUv[3] = 1.0f;
    constants.transfer = packedPq ? 2u : TransferMode(frame.color.transfer);
    constants.primaries =
        (frame.color.primaries == VideoColorPrimaries::Bt2020 ? 2u : 0u) |
        ToneMappingModeBits(videoSettings_.toneMapping);
    const bool nativeHlg = WantsNativeHlgOutput(frame);
    const bool hdrOutput = SetHdrOutputState(
        frame, !nativeHlg &&
                   HdrOutputEnabled(frame, videoSettings_, displayCapabilities_,
                                    hdr10ColorSpaceSupported_));
    constants.hdrOutput = nativeHlg ? 2.0f : (hdrOutput ? 1.0f : 0.0f);
    constants.sourcePeakNits = FrameSourcePeakNits(frame);
    constants.targetPeakNits = TargetPeakNits(videoSettings_, displayCapabilities_);
    const bool fixedInterpolationDisplayDomain =
        interpolationRequested_.load(std::memory_order_acquire);
    constants.padding = InitialHdrProcessingFlags(
        frame, videoSettings_, hdrOutput, fixedInterpolationDisplayDomain);
    FillHdr10PlusConstants(constants, frame,
                           !nvidiaHdrOutput_.Hdr10PlusGamingActive());
    const uint64_t hdrToneCurveFingerprint = FillHdrToneCurveConstants(
        constants.hdrToneCurve, constants.padding, frame, videoSettings_, hdrOutput,
        fixedInterpolationDisplayDomain);
    if (hdrToneCurveFingerprint != 0 &&
        hdrToneCurveFingerprint != loggedHdrToneCurveFingerprint_) {
        loggedHdrToneCurveFingerprint_ = hdrToneCurveFingerprint;
        Log(LogLevel::Info, L"HDR custom tone curve active pipeline=rgb");
    }

    // P5/P8 use FFmpeg/libplacebo to perform the RPU reshaping before this
    // RGB10 upload. L5 active-area masking and the display-target CMv4 trims
    // still have to run after that conversion, just as they did in the D3D11
    // compositor. Populate b1 instead of leaving the bound buffer stale.
    DoviShaderConstantsPair doviConstants{};
    const auto* dovi = FrameDolbyVisionMetadata(frame);
    const bool hasDoviCompositionMetadata = packedPq && dovi;
    const bool applyDoviDisplayTrim = hasDoviCompositionMetadata &&
        videoSettings_.dolbyVisionCmv4Approx;
    if (hasDoviCompositionMetadata) {
        FillDoviShaderConstants(doviConstants, 0, frame,
                                constants.targetPeakNits);
        if (!applyDoviDisplayTrim) {
            // L5 active-area masking is part of Dolby Vision composition, not
            // a CMv4 creative trim. Keep its bounds while disabling L2/L3/L8.
            doviConstants.trimA[0][0] = 0.0f;
        }
        if (dovi->sceneRefreshFlag != 0 && dovi->dmLevel5Present &&
            (dovi->dmLevel5LeftOffset != 0 || dovi->dmLevel5RightOffset != 0 ||
             dovi->dmLevel5TopOffset != 0 || dovi->dmLevel5BottomOffset != 0)) {
            Log(LogLevel::Info,
                L"Dolby Vision active-area mask submitted left=" +
                    std::to_wstring(dovi->dmLevel5LeftOffset) +
                    L" right=" + std::to_wstring(dovi->dmLevel5RightOffset) +
                    L" top=" + std::to_wstring(dovi->dmLevel5TopOffset) +
                    L" bottom=" + std::to_wstring(dovi->dmLevel5BottomOffset));
        }
    }
    if (applyDoviDisplayTrim) {
        const DoviDisplayTrim trim = SelectDoviDisplayTrim(
            dovi, constants.targetPeakNits);
        if (trim.enabled &&
            (!dolbyVisionDynamicTrimLogged_ ||
             (dovi->sceneRefreshFlag != 0 &&
              dovi->dynamicMetadataFingerprint != loggedDoviTrimFingerprint_))) {
            Log(LogLevel::Info,
                L"Dolby Vision dynamic trim submitted profile=" +
                    std::to_wstring(dovi->profile) +
                    L" fingerprint=" +
                    std::to_wstring(dovi->dynamicMetadataFingerprint) +
                    L" selected=" +
                    std::wstring(DoviTrimSourceName(trim.source)) +
                    (trim.includesLevel3 &&
                             trim.source != DoviDisplayTrimSource::Level3
                         ? L"+L3"
                         : L"") +
                    L" target_peak_nits=" +
                    std::to_wstring(static_cast<int>(
                        constants.targetPeakNits + 0.5f)));
            dolbyVisionDynamicTrimLogged_ = true;
        }
        loggedDoviTrimFingerprint_ = dovi->dynamicMetadataFingerprint;
    }
    std::memcpy(doviConstantMappings_[backBufferIndex], &doviConstants,
                sizeof(doviConstants));
    commandList_->SetGraphicsRoot32BitConstants(1, 60, &constants, 0);
    commandList_->SetGraphicsRootConstantBufferView(
        2, doviConstantBuffers_[backBufferIndex]->GetGPUVirtualAddress());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);
    BindPresentedSubtitles(frame);
    DrawOverlays(viewport, backBufferIndex);

    const std::array endBarriers{
        TransitionBarrier(cache.texture.Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_DEST),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          nativeHlg ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                    : D3D12_RESOURCE_STATE_PRESENT)};
    commandList_->ResourceBarrier(
        static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(commandList_->Close())) return false;
    const SubmitResult graphSubmit = frameGraph_.Submit(
        GpuFrameQueue::Graphics, commandList_.Get());
    if (!graphSubmit.accepted) {
        Log(LogLevel::Error,
            L"RGB upload composite submit failed reason=" + graphSubmit.reason);
        return false;
    }
    sourceGraphicsCompletions_[{frame.timelineSerial, frame.serial}] =
        graphSubmit.completion;
    if (!PresentComposedFrame(
            backBufferIndex, graphSubmit.completion, nativeHlg)) {
        return false;
    }
    inFlightFrames_[backBufferIndex] = std::make_unique<NativeVideoFrame>(frame);
    AnchorGeneratedFrames(frame, std::chrono::steady_clock::now());

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.frames;
        ++renderStats_.bgraFrames;
        renderStats_.bgraUploadUs += static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
        renderStats_.totalRenderUs += static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
        renderStats_.maxRenderUs = std::max(
            renderStats_.maxRenderUs,
            static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
        publishedStats_ = renderStats_;
    }
    return true;
}

bool D3D12VideoRenderer::PrepareSoftwareYuvUpload(
    const NativeYuvPlanes& planes,
    YuvFrameUploadCache& cache,
    NativeVideoFrame& residentFrame) {
    if (!planes.data) return false;
    const SoftwareYuvUploadLayout layout = BuildSoftwareYuvUploadLayout(
        planes.width, planes.height, planes.yStride, planes.uvStride,
        planes.bitDepth, planes.data->size());
    if (!layout.valid) {
        Log(LogLevel::Warning, L"software YUV frame rejected because its layout is invalid");
        return false;
    }

    const DXGI_FORMAT textureFormat = layout.format == SoftwareYuvTextureFormat::Nv12
        ? DXGI_FORMAT_NV12
        : DXGI_FORMAT_P010;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT64>(planes.width);
    textureDesc.Height = static_cast<UINT>(planes.height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = textureFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const bool recreate = !cache.texture || !cache.upload || !cache.mappedUpload ||
        cache.width != static_cast<UINT>(planes.width) ||
        cache.height != static_cast<UINT>(planes.height) ||
        cache.format != textureFormat;
    if (recreate) {
        if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
        cache = {};

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&cache.texture)))) {
            Log(LogLevel::Warning, L"software YUV texture allocation failed");
            return false;
        }

        UINT64 uploadBytes = 0;
        device_->GetCopyableFootprints(
            &textureDesc, 0, 2, 0, cache.footprints.data(), cache.rowCounts.data(),
            cache.rowSizes.data(), &uploadBytes);
        if (uploadBytes == 0 ||
            cache.rowCounts[0] < static_cast<UINT>(layout.lumaRows) ||
            cache.rowCounts[1] < static_cast<UINT>(layout.chromaRows) ||
            cache.rowSizes[0] < layout.lumaRowBytes ||
            cache.rowSizes[1] < layout.chromaRowBytes) {
            cache = {};
            Log(LogLevel::Warning, L"software YUV texture footprint is incompatible");
            return false;
        }

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cache.upload)))) {
            cache.texture.Reset();
            Log(LogLevel::Warning, L"software YUV upload allocation failed");
            return false;
        }
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(cache.upload->Map(0, &noRead, &cache.mappedUpload))) {
            cache = {};
            Log(LogLevel::Warning, L"software YUV upload mapping failed");
            return false;
        }
        cache.format = textureFormat;
        cache.width = static_cast<UINT>(planes.width);
        cache.height = static_cast<UINT>(planes.height);
        cache.uploadCapacity = uploadBytes;
    }

    const std::array sourceOffsets{layout.lumaOffset, layout.chromaOffset};
    const std::array sourceStrides{
        static_cast<std::size_t>(planes.yStride),
        static_cast<std::size_t>(planes.uvStride)};
    const std::array sourceRowBytes{layout.lumaRowBytes, layout.chromaRowBytes};
    const std::array sourceRowCounts{
        static_cast<UINT>(layout.lumaRows),
        static_cast<UINT>(layout.chromaRows)};
    const std::uint8_t* source = planes.data->data();
    auto* upload = static_cast<std::uint8_t*>(cache.mappedUpload);
    for (std::size_t plane = 0; plane < 2; ++plane) {
        auto* destination = upload + cache.footprints[plane].Offset;
        for (UINT row = 0; row < cache.rowCounts[plane]; ++row) {
            auto* destinationRow = destination + static_cast<std::size_t>(row) *
                cache.footprints[plane].Footprint.RowPitch;
            std::memset(destinationRow, 0,
                        static_cast<std::size_t>(cache.rowSizes[plane]));
            if (row < sourceRowCounts[plane]) {
                std::memcpy(destinationRow,
                            source + sourceOffsets[plane] +
                                static_cast<std::size_t>(row) * sourceStrides[plane],
                            sourceRowBytes[plane]);
            }
        }
    }

    residentFrame.width = planes.width;
    residentFrame.height = planes.height;
    residentFrame.d3dFormat = textureFormat;
    residentFrame.d3dTextureWidth = planes.width;
    residentFrame.d3dTextureHeight = planes.height;
    residentFrame.sourceUvRect = {};
    residentFrame.d3d12Texture = cache.texture;
    residentFrame.d3d12Subresource = 0;
    residentFrame.d3d12ReadyFence.Reset();
    residentFrame.d3d12ReadyFenceValue = 0;
    return true;
}

void D3D12VideoRenderer::RecordSoftwareYuvUpload(
    const YuvFrameUploadCache& cache) {
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = cache.texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = cache.upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = cache.footprints[plane];
        commandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
}

bool D3D12VideoRenderer::RenderFrame(const NativeVideoFrame& frame) {
    RefreshDisplayPeakNits();
    WaitForFrameLatencyObject();
    if (frame.HasPixels()) {
        return RenderPixelFrame(frame);
    }
    const bool softwareBase = !frame.HasD3D12Texture() && frame.HasYuv();
    if (!softwareBase &&
        (!frame.HasD3D12Texture() ||
         (frame.d3dFormat != DXGI_FORMAT_NV12 &&
          frame.d3dFormat != DXGI_FORMAT_P010 &&
          frame.d3dFormat != DXGI_FORMAT_P016))) {
        Log(LogLevel::Warning, L"d3d12 compositor rejected non-resident or unsupported frame");
        return false;
    }
    const auto startedAt = std::chrono::steady_clock::now();
    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!WaitForBackBuffer(backBufferIndex)) return false;
    inFlightFrames_[backBufferIndex].reset();
    transients_[backBufferIndex].clear();

    const bool softwareEnhancement =
        !frame.HasEnhancementD3D12Texture() && frame.HasEnhancementYuv();
    NativeVideoFrame residentFrame;
    NativeVideoFrame residentEnhancement;
    YuvFrameUploadCache* baseUpload = nullptr;
    YuvFrameUploadCache* enhancementUpload = nullptr;
    if (softwareBase || softwareEnhancement) {
        residentFrame = frame;
        if (softwareBase) {
            baseUpload = &yuvFrameUploadCaches_[backBufferIndex][0];
            if (!PrepareSoftwareYuvUpload(frame.yuv, *baseUpload, residentFrame)) {
                return false;
            }
        }
        if (softwareEnhancement) {
            enhancementUpload = &yuvFrameUploadCaches_[backBufferIndex][1];
            if (!PrepareSoftwareYuvUpload(
                    frame.enhancementYuv, *enhancementUpload, residentEnhancement)) {
                return false;
            }
            try {
                residentFrame.enhancementFrame =
                    std::make_shared<NativeVideoFrame>(std::move(residentEnhancement));
            } catch (const std::bad_alloc&) {
                Log(LogLevel::Warning,
                    L"software YUV enhancement frame allocation failed");
                return false;
            }
        }
    }
    const NativeVideoFrame& renderFrame = softwareBase || softwareEnhancement
        ? residentFrame
        : frame;
    const bool requiresDolbyVisionPipeline =
        (renderFrame.dovi && renderFrame.dovi->valid) ||
        (renderFrame.enhancementDovi && renderFrame.enhancementDovi->valid) ||
        renderFrame.HasEnhancementD3D12Texture();
    if (!baseUpload && !enhancementUpload && requiresDolbyVisionPipeline &&
        libplaceboBridge_ &&
        TryRenderDolbyVisionWithLibplacebo(renderFrame, backBufferIndex, startedAt)) {
        return true;
    }
    ID3D12PipelineState* compositionPipeline = yuvPipeline_.Get();
    if (requiresDolbyVisionPipeline) {
        StartAdvancedPipelineInitialization();
        ID3D12PipelineState* doviPipeline =
            publishedDoviPipeline_.load(std::memory_order_acquire);
        if (!doviPipeline && advancedPipelineThread_.joinable()) {
            if (!dolbyVisionPipelineWaitLogged_) {
                dolbyVisionPipelineWaitLogged_ = true;
                Log(LogLevel::Info,
                    L"first Dolby Vision frame waiting for the RPU pipeline; "
                    L"ordinary YCbCr fallback is forbidden");
            }
            advancedPipelineThread_.join();
            doviPipeline = publishedDoviPipeline_.load(std::memory_order_acquire);
        }
        if (!doviPipeline) {
            Log(LogLevel::Error,
                L"Dolby Vision frame rejected because the RPU pipeline is unavailable");
            return false;
        }
        compositionPipeline = doviPipeline;
    }
    if (FAILED(allocators_[backBufferIndex]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[backBufferIndex].Get(), compositionPipeline))) {
        return false;
    }

    if (baseUpload) RecordSoftwareYuvUpload(*baseUpload);
    if (enhancementUpload) RecordSoftwareYuvUpload(*enhancementUpload);

    const D3D12_RESOURCE_DESC sourceDesc = renderFrame.d3d12Texture->GetDesc();
    const UINT arraySize = std::max<UINT>(1, sourceDesc.DepthOrArraySize);
    const UINT plane0 = std::min(renderFrame.d3d12Subresource, arraySize - 1);
    const UINT plane1 = plane0 + arraySize;
    const NativeVideoFrame* enhancement = renderFrame.HasEnhancementD3D12Texture()
        ? renderFrame.enhancementFrame.get() : nullptr;
    UINT enhancementPlane0 = 0;
    UINT enhancementPlane1 = 0;
    if (enhancement) {
        const UINT enhancementArray =
            std::max<UINT>(1, enhancement->d3d12Texture->GetDesc().DepthOrArraySize);
        enhancementPlane0 = std::min(enhancement->d3d12Subresource, enhancementArray - 1);
        enhancementPlane1 = enhancementPlane0 + enhancementArray;
    }
    std::vector<D3D12_RESOURCE_BARRIER> beginBarriers{
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
        TransitionBarrier(renderFrame.d3d12Texture.Get(), plane0,
                          baseUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                                     : D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(renderFrame.d3d12Texture.Get(), plane1,
                          baseUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                                     : D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    if (enhancement) {
        beginBarriers.push_back(TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane0,
            enhancementUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                              : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        beginBarriers.push_back(TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane1,
            enhancementUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                              : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    }
    commandList_->ResourceBarrier(static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(backBufferIndex * 4) * srvIncrement_;
    const auto makeSrv = [&](const NativeVideoFrame& source, const DXGI_FORMAT format,
                             const UINT planeSlice, const D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        const UINT sourceArray =
            std::max<UINT>(1, source.d3d12Texture->GetDesc().DepthOrArraySize);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.FirstArraySlice =
            std::min(source.d3d12Subresource, sourceArray - 1);
        view.Texture2DArray.ArraySize = 1;
        view.Texture2DArray.PlaneSlice = planeSlice;
        device_->CreateShaderResourceView(source.d3d12Texture.Get(), &view, handle);
    };
    const bool highBitDepth = renderFrame.d3dFormat == DXGI_FORMAT_P010 ||
                              renderFrame.d3dFormat == DXGI_FORMAT_P016;
    makeSrv(renderFrame, highBitDepth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
            0, srvCpu);
    srvCpu.ptr += srvIncrement_;
    makeSrv(renderFrame,
            highBitDepth ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM,
            1, srvCpu);
    srvCpu.ptr += srvIncrement_;
    const NativeVideoFrame& enhancementSource = enhancement ? *enhancement : renderFrame;
    const bool enhancementHighBitDepth = enhancementSource.d3dFormat == DXGI_FORMAT_P010 ||
                                         enhancementSource.d3dFormat == DXGI_FORMAT_P016;
    makeSrv(enhancementSource,
            enhancementHighBitDepth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, 0, srvCpu);
    srvCpu.ptr += srvIncrement_;
    makeSrv(enhancementSource,
            enhancementHighBitDepth ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, 1, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvIncrement_;
    constexpr float clearColor[4]{};
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    const D3D12_VIEWPORT viewport = AspectFitVideoViewport(
        renderFrame.width, renderFrame.height, width_, height_);
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(backBufferIndex * 4) * srvIncrement_;
    commandList_->SetGraphicsRootDescriptorTable(0, srvGpu);

    CompositionConstants constants{};
    constants.sourceUv[0] = renderFrame.sourceUvRect.left;
    constants.sourceUv[1] = renderFrame.sourceUvRect.top;
    constants.sourceUv[2] = renderFrame.sourceUvRect.right;
    constants.sourceUv[3] = renderFrame.sourceUvRect.bottom;
    const VideoTextureUvRect enhancementRect = enhancement
        ? enhancement->sourceUvRect : renderFrame.sourceUvRect;
    constants.enhancementSourceUv[0] = enhancementRect.left;
    constants.enhancementSourceUv[1] = enhancementRect.top;
    constants.enhancementSourceUv[2] = enhancementRect.right;
    constants.enhancementSourceUv[3] = enhancementRect.bottom;
    constants.matrix = MatrixMode(renderFrame.color.matrix);
    const auto* reconstructionMetadata =
        enhancement && renderFrame.enhancementDovi && renderFrame.enhancementDovi->valid
            ? renderFrame.enhancementDovi.get()
            : (renderFrame.dovi && renderFrame.dovi->valid
                   ? renderFrame.dovi.get()
                   : nullptr);
    constants.range = reconstructionMetadata
        ? (reconstructionMetadata->blVideoFullRange ? 1u : 0u)
        : (renderFrame.color.range == VideoColorRange::Full ? 1u : 0u);
    constants.transfer = TransferMode(renderFrame.color.transfer);
    constants.primaries = (highBitDepth ? 1u : 0u) |
        (renderFrame.color.primaries == VideoColorPrimaries::Bt2020 ? 2u : 0u) |
        ToneMappingModeBits(videoSettings_.toneMapping);
    const bool nativeHlg = WantsNativeHlgOutput(renderFrame);
    const bool hdrOutput = SetHdrOutputState(
        renderFrame, !nativeHlg &&
                         HdrOutputEnabled(renderFrame, videoSettings_, displayCapabilities_,
                                          hdr10ColorSpaceSupported_));
    constants.hdrOutput = nativeHlg ? 2.0f : (hdrOutput ? 1.0f : 0.0f);
    constants.sourcePeakNits = FrameSourcePeakNits(renderFrame);
    constants.targetPeakNits = TargetPeakNits(videoSettings_, displayCapabilities_);
    const bool fixedInterpolationDisplayDomain =
        interpolationRequested_.load(std::memory_order_acquire);
    constants.padding = InitialHdrProcessingFlags(
        renderFrame, videoSettings_, hdrOutput, fixedInterpolationDisplayDomain);
    FillHdr10PlusConstants(constants, renderFrame,
                           !nvidiaHdrOutput_.Hdr10PlusGamingActive());
    const uint64_t hdrToneCurveFingerprint = FillHdrToneCurveConstants(
        constants.hdrToneCurve, constants.padding, renderFrame, videoSettings_, hdrOutput,
        fixedInterpolationDisplayDomain);
    if (hdrToneCurveFingerprint != 0 &&
        hdrToneCurveFingerprint != loggedHdrToneCurveFingerprint_) {
        loggedHdrToneCurveFingerprint_ = hdrToneCurveFingerprint;
        Log(LogLevel::Info, L"HDR custom tone curve active pipeline=yuv");
    }
    commandList_->SetGraphicsRoot32BitConstants(1, 60, &constants, 0);
    DoviShaderConstantsPair doviConstants{};
    FillDoviShaderConstants(doviConstants, 0, renderFrame, constants.targetPeakNits);
    std::memcpy(doviConstantMappings_[backBufferIndex], &doviConstants,
                sizeof(doviConstants));
    commandList_->SetGraphicsRootConstantBufferView(
        2, doviConstantBuffers_[backBufferIndex]->GetGPUVirtualAddress());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);
    BindPresentedSubtitles(frame);
    DrawOverlays(viewport, backBufferIndex);

    std::vector<D3D12_RESOURCE_BARRIER> endBarriers{
        TransitionBarrier(renderFrame.d3d12Texture.Get(), plane0,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          baseUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                                     : D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(renderFrame.d3d12Texture.Get(), plane1,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          baseUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                                     : D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          nativeHlg ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                    : D3D12_RESOURCE_STATE_PRESENT)};
    if (enhancement) {
        endBarriers.insert(endBarriers.end() - 1, TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane0,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            enhancementUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                              : D3D12_RESOURCE_STATE_COMMON));
        endBarriers.insert(endBarriers.end() - 1, TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane1,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            enhancementUpload ? D3D12_RESOURCE_STATE_COPY_DEST
                              : D3D12_RESOURCE_STATE_COMMON));
    }
    commandList_->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(commandList_->Close())) return false;
    GpuFencePoint decodeDependency;
    decodeDependency.fence = renderFrame.d3d12ReadyFence;
    decodeDependency.value = renderFrame.d3d12ReadyFenceValue;
    GpuFencePoint frameComputeDependency;
    const auto computeCompletion = sourceComputeCompletions_.find(
        {renderFrame.timelineSerial, renderFrame.serial});
    if (computeCompletion != sourceComputeCompletions_.end()) {
        frameComputeDependency = computeCompletion->second;
        sourceComputeCompletions_.erase(computeCompletion);
    }
    GpuFencePoint enhancementDependency;
    if (enhancement) {
        enhancementDependency.fence = enhancement->d3d12ReadyFence;
        enhancementDependency.value = enhancement->d3d12ReadyFenceValue;
    }
    const std::array dependencies{
        decodeDependency, enhancementDependency, frameComputeDependency};
    const SubmitResult graphSubmit = frameGraph_.Submit(
        GpuFrameQueue::Graphics, commandList_.Get(), dependencies);
    if (!graphSubmit.accepted) {
        Log(LogLevel::Error, L"frame graph composite submit failed reason=" + graphSubmit.reason);
        return false;
    }
    sourceGraphicsCompletions_[{renderFrame.timelineSerial, renderFrame.serial}] =
        graphSubmit.completion;
    if (!dolbyVisionD3D12Logged_ && doviConstants.signalMeta[0][0] > 0.5f) {
        dolbyVisionD3D12Logged_ = true;
        Log(LogLevel::Info,
            doviConstants.signalMeta[0][0] > 1.5f
                ? L"Dolby Vision reconstruction path=d3d12_rpu+el_fel+nlq canonical=bt2020_pq"
                : L"Dolby Vision reconstruction path=d3d12_rpu_reshape canonical=bt2020_pq");
    }

    if (!PresentComposedFrame(
            backBufferIndex, graphSubmit.completion, nativeHlg)) {
        return false;
    }
    inFlightFrames_[backBufferIndex] = std::make_unique<NativeVideoFrame>(frame);
    AnchorGeneratedFrames(frame, std::chrono::steady_clock::now());

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.frames;
        if (baseUpload || enhancementUpload) {
            ++renderStats_.softwareYuvFrames;
            renderStats_.yuvUploadUs +=
                static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
        } else {
            ++renderStats_.hardwareFrames;
        }
        renderStats_.totalRenderUs += static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
        renderStats_.maxRenderUs = std::max(renderStats_.maxRenderUs,
                                            static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
        publishedStats_ = renderStats_;
    }
    return true;
}

int D3D12VideoRenderer::InterpolationMultiplier(const NativeVideoFrame& first,
                                                const NativeVideoFrame& second) const {
    double sourceFps = 0.0;
    if (second.frameRateNumerator != 0 && second.frameRateDenominator != 0) {
        sourceFps = static_cast<double>(second.frameRateNumerator) /
            static_cast<double>(second.frameRateDenominator);
    }
    if (sourceFps <= 1.0) {
        const auto interval = second.pts - first.pts;
        if (interval.count() > 0) sourceFps = 1000.0 / interval.count();
    }

    double refreshHz = 0.0;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(host_.load(), MONITOR_DEFAULTTONEAREST), &monitor)) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1) {
            refreshHz = EffectiveRefreshRate(mode.dmDisplayFrequency);
        }
    }
    return Fixed2xInterpolationMultiplier(sourceFps, refreshHz);
}

void D3D12VideoRenderer::ProcessFrameGraphInput(const NativeVideoFrame& frame) {
    RefreshDisplayPeakNits();
    if (!tensorDolbyVisionPipelineLogged_ &&
        tensorPreprocessor_.DolbyVisionPipelineInitializationComplete()) {
        tensorDolbyVisionPipelineLogged_ = true;
        const bool ready = tensorPreprocessor_.DolbyVisionPipelineReady();
        Log(ready ? LogLevel::Info : LogLevel::Warning,
            std::wstring(L"Dolby Vision display-domain tensor pipeline=") +
                (ready ? L"ready background=true"
                       : L"unavailable shader_compile_failed=true"));
    }
    IMlFrameInterpolationExecutor* executor =
        mlExecutor_.load(std::memory_order_acquire);
    if (!interpolationRequested_.load(std::memory_order_acquire) || !executor ||
        !executor->IsReady() || (!frame.HasD3D12Texture() && !frame.HasPixels())) {
        tensorPreviousFrame_.reset();
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        sourceComputeCompletions_.clear();
        sourceGraphicsCompletions_.clear();
        return;
    }
    const bool dolbyVisionFrame =
        (frame.dovi && frame.dovi->valid) ||
        (frame.enhancementDovi && frame.enhancementDovi->valid) ||
        frame.HasEnhancementD3D12Texture();
    const bool libplaceboDolbyVisionInput = dolbyVisionFrame && frame.HasD3D12Texture() &&
        libplaceboBridge_ && libplaceboBridge_->IsReady();
    dolbyVisionInterpolationColorBypassLogged_ = false;
    const auto nominalInterval = frame.frameRateNumerator != 0
        ? std::chrono::milliseconds{static_cast<int64_t>(std::llround(
              1000.0 * frame.frameRateDenominator / frame.frameRateNumerator))}
        : std::chrono::milliseconds{42};
    const bool continuous = tensorPreviousFrame_ &&
        tensorPreviousFrame_->timelineSerial == frame.timelineSerial &&
        tensorPreviousFrame_->pts < frame.pts &&
        frame.pts - tensorPreviousFrame_->pts <=
            std::max(std::chrono::milliseconds{250}, nominalInterval * 3);
    if (!continuous && tensorPreviousFrame_) {
        Log(LogLevel::Info,
            L"interpolation timeline discontinuity old=" +
                std::to_wstring(tensorPreviousFrame_->timelineSerial) + L" new=" +
                std::to_wstring(frame.timelineSerial) +
                L" endpoint_cache=invalidated");
        frameGraph_.AdvanceEpoch();
        tensorPreviousFrame_.reset();
        // The cached target is immutable and keyed to the old endpoint. Never
        // render a new timeline into it while an earlier preprocess dispatch
        // can still be reading it on the compute queue.
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        sourceComputeCompletions_.clear();
        sourceGraphicsCompletions_.clear();
    }
    if (!tensorPreviousFrame_) {
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
            StripInterpolationPresentationData(*tensorPreviousFrame_);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        return;
    }

    const NativeVideoFrame& first = *tensorPreviousFrame_;
    const auto* firstDovi = FrameDolbyVisionMetadata(first);
    const uint64_t firstDoviFingerprint = firstDovi
        ? firstDovi->dynamicMetadataFingerprint : 0;
    const bool pixelInterpolationInput = first.HasPixels() && frame.HasPixels();
    const bool libplaceboPixelDolbyVisionInput =
        dolbyVisionFrame && pixelInterpolationInput;
    const auto pixelIsPq = [](const NativeVideoFrame& endpoint) {
        return endpoint.softwareFormat == AV_PIX_FMT_X2BGR10LE;
    };
    const bool firstHdrTensorInput = pixelInterpolationInput
        ? pixelIsPq(first) : FrameCarriesHdr(first);
    const bool secondHdrTensorInput = pixelInterpolationInput
        ? pixelIsPq(frame) : FrameCarriesHdr(frame);
    const int multiplier = InterpolationMultiplier(first, frame);
    if (multiplier <= 1) {
        sourceComputeCompletions_.clear();
        sourceGraphicsCompletions_.clear();
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
            StripInterpolationPresentationData(*tensorPreviousFrame_);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        std::scoped_lock lock(statsMutex_);
        renderStats_.interpolationMultiplier = 1;
        publishedStats_ = renderStats_;
        return;
    }
    const bool interpolationHdrOutput = HdrOutputEnabled(
        first, videoSettings_, displayCapabilities_, hdr10ColorSpaceSupported_);
    const bool secondHdrOutput = HdrOutputEnabled(
        frame, videoSettings_, displayCapabilities_, hdr10ColorSpaceSupported_);
    if (IsInterpolationDisplayDomainBoundary(
            firstHdrTensorInput, secondHdrTensorInput,
            interpolationHdrOutput, secondHdrOutput)) {
        Log(LogLevel::Info,
            L"frame interpolation display-domain boundary left_hdr=" +
                std::wstring(firstHdrTensorInput ? L"true" : L"false") +
                L" right_hdr=" +
                std::wstring(secondHdrTensorInput ? L"true" : L"false") +
                L" left_output=" +
                std::wstring(interpolationHdrOutput ? L"hdr" : L"sdr") +
                L" right_output=" +
                std::wstring(secondHdrOutput ? L"hdr" : L"sdr"));
        {
            std::scoped_lock lock(statsMutex_);
            renderStats_.generatedDroppedSceneCut +=
                static_cast<uint64_t>(multiplier - 1);
            publishedStats_ = renderStats_;
        }
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
            StripInterpolationPresentationData(*tensorPreviousFrame_);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        return;
    }
    if (IsDolbyVisionSceneBoundary(FrameDolbyVisionMetadata(frame))) {
        Log(LogLevel::Info,
            L"frame interpolation scene boundary source=dolby_vision_scene_refresh "
            L"left_pts_ms=" + std::to_wstring(first.pts.count()) +
            L" right_pts_ms=" + std::to_wstring(frame.pts.count()));
        {
            std::scoped_lock lock(statsMutex_);
            renderStats_.generatedDroppedSceneCut +=
                static_cast<uint64_t>(multiplier - 1);
            publishedStats_ = renderStats_;
        }
        sourceComputeCompletions_.clear();
        sourceGraphicsCompletions_.clear();
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
            StripInterpolationPresentationData(*tensorPreviousFrame_);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        return;
    }
    if (!executor->CanSubmit()) {
        // Keep only the newest endpoint while inference is saturated. Most
        // importantly, do not enqueue preprocessing on the shared resource:
        // original frames must continue directly to graphics without waiting
        // behind old ML work.
        sourceGraphicsCompletions_.erase({first.timelineSerial, first.serial});
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
            StripInterpolationPresentationData(*tensorPreviousFrame_);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        return;
    }
    const float interpolationHlgPeakNits = std::max(
        TargetPeakNits(videoSettings_, displayCapabilities_), 1000.0f);
    const float interpolationTargetPeakNits =
        TargetPeakNits(videoSettings_, displayCapabilities_);
    InterpolationDisplayMappingPair interpolationDisplayMapping{};
    FillInterpolationDisplayEndpoint(
        interpolationDisplayMapping, 0, first, videoSettings_,
        firstHdrTensorInput, interpolationHdrOutput,
        interpolationTargetPeakNits);
    FillInterpolationDisplayEndpoint(
        interpolationDisplayMapping, 1, frame, videoSettings_,
        secondHdrTensorInput, interpolationHdrOutput,
        interpolationTargetPeakNits);
    if (frame.color.transfer == VideoTransferCharacteristic::Hlg &&
        !hlgInterpolationColorPathLogged_) {
        hlgInterpolationColorPathLogged_ = true;
        Log(LogLevel::Info,
            L"HLG interpolation active tensor_domain=display_mapped_bt2020_pq "
            L"shared_ootf_peak_nits=" +
                std::to_wstring(static_cast<int>(interpolationHlgPeakNits + 0.5f)) +
                L" cadence=x" + std::to_wstring(multiplier));
    }
    GpuFencePoint orderingDependency;
    const auto takeGraphicsDependency = [&](const NativeVideoFrame& source) {
        const auto found = sourceGraphicsCompletions_.find(
            {source.timelineSerial, source.serial});
        if (found == sourceGraphicsCompletions_.end()) return;
        if (!orderingDependency.IsValid() ||
            (orderingDependency.fence.Get() == found->second.fence.Get() &&
             orderingDependency.value < found->second.value)) {
            orderingDependency = found->second;
        }
        sourceGraphicsCompletions_.erase(found);
    };
    takeGraphicsDependency(first);
    takeGraphicsDependency(frame);

    Microsoft::WRL::ComPtr<ID3D12Resource> currentDoviTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> currentDoviLuminanceTarget;
    const UINT interpolationMaximumHeight = static_cast<UINT>(std::clamp(
        videoSettings_.frameInterpolationMaximumHeight,
        anvil::playback::kMinimumFrameInterpolationMaximumHeight,
        anvil::playback::kMaximumFrameInterpolationMaximumHeight));
    const TensorShape interpolationShape = SelectInterpolationTensorShape(
        static_cast<UINT>(std::max(first.width, frame.width)),
        static_cast<UINT>(std::max(first.height, frame.height)),
        interpolationMaximumHeight);
    if (!interpolationShape.IsValid()) {
        Log(LogLevel::Warning,
            L"frame interpolation skipped reason=no_tensor_shape_bucket");
        return;
    }
    if (libplaceboDolbyVisionInput) {
        const UINT endpointWidth = std::max<UINT>(1, width_);
        const UINT endpointHeight = std::max<UINT>(1, height_);
        const auto createTarget = [&](
            Microsoft::WRL::ComPtr<ID3D12Resource>& target,
            const UINT targetWidth, const UINT targetHeight) {
            if (!interpolationShape.IsValid()) return false;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = targetWidth;
            desc.Height = targetHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kLibplaceboTargetFormat;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = kLibplaceboTargetFormat;
            return SUCCEEDED(device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                &clear, IID_PPV_ARGS(&target)));
        };
        const auto renderEndpoint = [&](const NativeVideoFrame& endpoint,
                                        Microsoft::WRL::ComPtr<ID3D12Resource>& target,
                                        const UINT targetWidth,
                                        const UINT targetHeight) {
            return (target || createTarget(target, targetWidth, targetHeight)) &&
                libplaceboBridge_->RenderDolbyVision(
                    endpoint, target.Get(), targetWidth, targetHeight,
                    interpolationHdrOutput, interpolationTargetPeakNits,
                    orderingDependency.fence.Get(),
                    orderingDependency.value, true);
        };
        const D3D12_RESOURCE_DESC cachedTensorDesc = tensorPreviousDoviTarget_
            ? tensorPreviousDoviTarget_->GetDesc() : D3D12_RESOURCE_DESC{};
        const D3D12_RESOURCE_DESC cachedLuminanceDesc =
            tensorPreviousDoviLuminanceTarget_
                ? tensorPreviousDoviLuminanceTarget_->GetDesc()
                : D3D12_RESOURCE_DESC{};
        const bool cachedFirstEndpoint = tensorPreviousDoviTarget_ &&
            tensorPreviousDoviLuminanceTarget_ &&
            tensorPreviousDoviTargetTimeline_ == first.timelineSerial &&
            tensorPreviousDoviTargetFrameSerial_ == first.serial &&
            tensorPreviousDoviMetadataFingerprint_ == firstDoviFingerprint &&
            tensorPreviousDoviHdrOutput_ == interpolationHdrOutput &&
            std::abs(tensorPreviousDoviTargetPeakNits_ -
                     interpolationTargetPeakNits) <= 0.5f &&
            cachedTensorDesc.Width == interpolationShape.width &&
            cachedTensorDesc.Height == interpolationShape.height &&
            cachedLuminanceDesc.Width == endpointWidth &&
            cachedLuminanceDesc.Height == endpointHeight;
        if (cachedFirstEndpoint && !dolbyVisionImmutableEndpointLogged_) {
            dolbyVisionImmutableEndpointLogged_ = true;
            Log(LogLevel::Info,
                L"Dolby Vision interpolation endpoint cache="
                L"immutable_per_frame timeline_guard=enabled "
                L"mapping=independent_per_endpoint fixed_display_target=true");
        }
        if (!cachedFirstEndpoint) {
            tensorPreviousDoviTarget_.Reset();
            tensorPreviousDoviLuminanceTarget_.Reset();
            tensorPreviousDoviTargetTimeline_ = 0;
            tensorPreviousDoviTargetFrameSerial_ = 0;
            tensorPreviousDoviMetadataFingerprint_ = 0;
        }
        if ((!cachedFirstEndpoint &&
            (!renderEndpoint(first, tensorPreviousDoviTarget_,
                              interpolationShape.width,
                              interpolationShape.height) ||
              !renderEndpoint(first, tensorPreviousDoviLuminanceTarget_,
                              endpointWidth, endpointHeight))) ||
            !renderEndpoint(frame, currentDoviTarget,
                            interpolationShape.width,
                            interpolationShape.height) ||
            !renderEndpoint(frame, currentDoviLuminanceTarget,
                            endpointWidth, endpointHeight)) {
            Log(LogLevel::Warning,
                L"Dolby Vision interpolation skipped reason=libplacebo_tensor_endpoint_failed");
            ResetInterpolationState();
            return;
        }
        const uint64_t doviReadyValue = nextFenceValue_++;
        if (FAILED(queue_->Signal(fence_.Get(), doviReadyValue))) {
            ResetInterpolationState();
            return;
        }
        orderingDependency.fence = fence_;
        orderingDependency.value = doviReadyValue;
        if (!dolbyVisionInterpolationColorPathLogged_) {
            dolbyVisionInterpolationColorPathLogged_ = true;
            Log(LogLevel::Info,
                L"Dolby Vision interpolation active color_source=libplacebo_scrgb "
                L"tensor_domain=display_mapped_bt2020_pq "
                L"endpoint_metadata=independent");
        }
    }
    if (libplaceboPixelDolbyVisionInput && !dolbyVisionInterpolationColorPathLogged_) {
        dolbyVisionInterpolationColorPathLogged_ = true;
        Log(LogLevel::Info,
            L"Dolby Vision interpolation active color_source=decoder_libplacebo_rgb "
            L"tensor_domain=sdr_encoded_or_display_mapped_bt2020_pq "
            L"endpoint_metadata=independent");
    }
    for (int sample = 1; sample < multiplier; ++sample) {
        if (!executor->CanSubmit()) break;
        const std::optional<std::size_t> generatedSlot = AcquireGeneratedSlot();
        if (!generatedSlot) break;
        const float interpolationT = static_cast<float>(sample) /
            static_cast<float>(multiplier);
        TensorPreprocessResult preprocess;
        if (libplaceboDolbyVisionInput) {
            preprocess = tensorPreprocessor_.SubmitScRgbPair(
                tensorPreviousDoviTarget_.Get(), currentDoviTarget.Get(),
                interpolationShape, interpolationT, frameGraph_.CurrentEpoch(),
                orderingDependency);
        } else if (pixelInterpolationInput) {
            preprocess = tensorPreprocessor_.SubmitPixelPair(
                first, frame, interpolationShape, interpolationT,
                interpolationDisplayMapping,
                frameGraph_.CurrentEpoch(), orderingDependency);
        } else {
            preprocess = tensorPreprocessor_.SubmitPair(
                first, frame, interpolationShape, interpolationT,
                interpolationHlgPeakNits,
                interpolationDisplayMapping,
                frameGraph_.CurrentEpoch(), orderingDependency);
        }
        if (!preprocess.accepted) {
            if (preprocess.reason != L"tensor_preprocess_gpu_busy") {
                Log(LogLevel::Warning,
                    L"frame graph preprocess skipped reason=" + preprocess.reason);
            }
            break;
        }
        orderingDependency = preprocess.tensor.ready;
        if (!libplaceboDolbyVisionInput && !pixelInterpolationInput) {
            sourceComputeCompletions_[{first.timelineSerial, first.serial}] =
                preprocess.tensor.ready;
            sourceComputeCompletions_[{frame.timelineSerial, frame.serial}] =
                preprocess.tensor.ready;
        }
        if (!EnsureGeneratedOutput(*generatedSlot, preprocess.shape)) break;

        GeneratedFrameSlot& output = generatedSlots_[*generatedSlot];
        SubmitResult inference = executor->Submit(
            preprocess.tensor.resource.Get(), output.output.Get(),
            preprocess.shape.width, preprocess.shape.height,
            preprocess.tensor.ready.fence.Get(), preprocess.tensor.ready.value);
        if (!inference.accepted) {
            if (inference.reason.find(L"busy") == std::wstring::npos) {
                Log(LogLevel::Warning, L"ML inference skipped reason=" + inference.reason);
            }
            break;
        }
        if (!tensorPreprocessor_.RetainUntil(
                preprocess.tensor.resource.Get(), inference.completion)) {
            generatedSlots_[*generatedSlot].reusable = inference.completion;
            Log(LogLevel::Warning,
                L"ML inference skipped reason=tensor_endpoint_retention_failed");
            break;
        }
        output.shape = preprocess.shape;
        output.inputTensor = preprocess.tensor.resource;
        output.sceneChangeProbe = preprocess.sceneChange;
        if (libplaceboDolbyVisionInput) {
            output.luminanceLeft = tensorPreviousDoviLuminanceTarget_;
            output.luminanceRight = currentDoviLuminanceTarget;
        } else {
            output.luminanceLeft.Reset();
            output.luminanceRight.Reset();
        }
        output.displayWidth = frame.width;
        output.displayHeight = frame.height;
        output.leftPts = first.pts;
        output.rightPts = frame.pts;
        output.targetPts = first.pts + std::chrono::milliseconds{
            static_cast<int64_t>(std::llround(
                static_cast<double>((frame.pts - first.pts).count()) * interpolationT))};
        output.timelineSerial = frame.timelineSerial;
        output.interpolationT = interpolationT;
        output.epoch = preprocess.tensor.epoch;
        output.ready = inference.completion;
        output.reusable = inference.completion;
        output.completionStatusConsumed = false;
        // The presentation target is immutable for the lifetime of this slot,
        // but A and B have already been mapped with their own metadata. The
        // left endpoint remains only a cadence anchor, never a color authority.
        output.hdrOutput = interpolationHdrOutput;
        output.targetPeakNits = interpolationTargetPeakNits;
        const bool displayMappedTensor = libplaceboDolbyVisionInput ||
            firstHdrTensorInput;
        output.inputTransfer = displayMappedTensor ? 4u : 1u;
        output.displayDomainReady = true;
        if (!interpolationDisplayDomainLogged_) {
            interpolationDisplayDomainLogged_ = true;
            Log(LogLevel::Info,
                L"interpolation color contract=display_domain "
                L"endpoint_mapping=independent final_tone_mapping=disabled "
                L"target_peak_nits=" +
                    std::to_wstring(static_cast<int>(output.targetPeakNits + 0.5f)));
        }
        if (presentedOriginalTimeline_ == first.timelineSerial &&
            presentedOriginalPts_ == first.pts) {
            output.anchored = true;
            output.dueAt = presentedOriginalAt_ + (output.targetPts - output.leftPts);
        }
        output.pending = true;
        pendingGeneratedSlots_.push_back(*generatedSlot);
        {
            std::scoped_lock lock(statsMutex_);
            ++renderStats_.generatedSubmitted;
            renderStats_.interpolationMultiplier = multiplier;
            renderStats_.interpolationBackend = executor->BackendName();
            publishedStats_ = renderStats_;
        }
        if (!directMlSubmitLogged_) {
            directMlSubmitLogged_ = true;
            Log(LogLevel::Info,
                L"ML zero-copy inference submitted backend=" + executor->BackendName() +
                    L" input=d3d12_buffer output=d3d12_buffer cadence=x" +
                    std::to_wstring(multiplier));
        }
        if (!tensorPreprocessorLogged_) {
            tensorPreprocessorLogged_ = true;
            Log(LogLevel::Info,
                L"frame graph preprocess active shape=" +
                    std::to_wstring(preprocess.shape.width) + L"x" +
                    std::to_wstring(preprocess.shape.height) +
                    L" maximum_picture_height=" +
                    std::to_wstring(interpolationMaximumHeight) +
                    L" layout=fp16_nchw_7 "
                    L"domain=sdr_encoded_or_display_mapped_bt2020_pq variable_t=true");
        }
        if (!sceneChangeProbeLogged_) {
            sceneChangeProbeLogged_ = true;
            Log(LogLevel::Info,
                L"frame interpolation scene protection=active "
                L"probe=sparse_bt2020_luma grid=16x16 async_readback=true");
        }
    }
    if (libplaceboDolbyVisionInput) {
        const auto* currentDovi = FrameDolbyVisionMetadata(frame);
        const uint64_t currentDoviFingerprint = currentDovi
            ? currentDovi->dynamicMetadataFingerprint : 0;
        // B was rendered with B's own metadata into the immutable target, so
        // it is exactly the next pair's A and can always be reused.
        tensorPreviousDoviTarget_ = std::move(currentDoviTarget);
        tensorPreviousDoviLuminanceTarget_ =
            std::move(currentDoviLuminanceTarget);
        tensorPreviousDoviTargetTimeline_ = frame.timelineSerial;
        tensorPreviousDoviTargetFrameSerial_ = frame.serial;
        tensorPreviousDoviMetadataFingerprint_ = currentDoviFingerprint;
        tensorPreviousDoviTargetPeakNits_ = interpolationTargetPeakNits;
        tensorPreviousDoviHdrOutput_ = interpolationHdrOutput;
    } else {
        tensorPreviousDoviTarget_.Reset();
        tensorPreviousDoviLuminanceTarget_.Reset();
        tensorPreviousDoviTargetTimeline_ = 0;
        tensorPreviousDoviTargetFrameSerial_ = 0;
        tensorPreviousDoviMetadataFingerprint_ = 0;
    }
    try {
        tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
        StripInterpolationPresentationData(*tensorPreviousFrame_);
    } catch (...) {
        tensorPreviousFrame_.reset();
    }
}

void D3D12VideoRenderer::AnchorGeneratedFrames(
    const NativeVideoFrame& original,
    const std::chrono::steady_clock::time_point presentedAt) {
    presentedOriginalPts_ = original.pts;
    presentedOriginalTimeline_ = original.timelineSerial;
    presentedOriginalAt_ = presentedAt;
    for (const std::size_t slotIndex : pendingGeneratedSlots_) {
        GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
        if (!slot.pending ||
            slot.timelineSerial != original.timelineSerial || slot.leftPts != original.pts) {
            continue;
        }
        if (!slot.anchored) {
            slot.anchored = true;
            slot.dueAt = presentedAt + (slot.targetPts - slot.leftPts);
        }
    }
}

std::optional<std::size_t> D3D12VideoRenderer::AcquireGeneratedSlot() {
    for (std::size_t index = 0; index < generatedSlots_.size(); ++index) {
        GeneratedFrameSlot& slot = generatedSlots_[index];
        if (slot.pending) continue;
        if (slot.reusable.IsValid() &&
            slot.reusable.fence->GetCompletedValue() < slot.reusable.value) {
            continue;
        }
        if (IMlFrameInterpolationExecutor* executor =
                mlExecutor_.load(std::memory_order_acquire);
            !slot.completionStatusConsumed && slot.ready.IsValid() && executor) {
            executor->TakeCompletionError(slot.ready.value);
        }
        slot.ready = {};
        slot.reusable = {};
        slot.sceneChangeProbe = {};
        slot.anchored = false;
        slot.completionStatusConsumed = false;
        slot.inputTransfer = 0;
        slot.hdrOutput = false;
        slot.displayDomainReady = false;
        return index;
    }
    return std::nullopt;
}

bool D3D12VideoRenderer::EnsureGeneratedOutput(const std::size_t slotIndex,
                                               const TensorShape shape) {
    if (slotIndex >= generatedSlots_.size() || !shape.IsValid()) return false;
    GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
    const std::size_t bytes = DirectMlFrameInterpolationExecutor::OutputBufferBytes(shape);
    if (slot.output && slot.capacityBytes >= bytes) return true;
    slot.output.Reset();
    slot.capacityBytes = 0;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(bytes);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&slot.output)))) {
        return false;
    }
    slot.capacityBytes = bytes;
    return true;
}

void D3D12VideoRenderer::ProcessReadyGeneratedFrame() {
    while (!pendingGeneratedSlots_.empty()) {
        const std::size_t slotIndex = pendingGeneratedSlots_.front();
        GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
        if (slot.epoch != frameGraph_.CurrentEpoch()) {
            RetireGeneratedSlot(slotIndex, false);
            pendingGeneratedSlots_.pop_front();
            continue;
        }
        if (!slot.anchored || std::chrono::steady_clock::now() < slot.dueAt) return;
        if (!slot.ready.IsValid() ||
            slot.ready.fence->GetCompletedValue() < slot.ready.value) {
            return;
        }
        IMlFrameInterpolationExecutor* executor =
            mlExecutor_.load(std::memory_order_acquire);
        const std::wstring inferenceError = executor
            ? executor->TakeCompletionError(slot.ready.value)
            : L"ml_executor_unavailable";
        slot.completionStatusConsumed = true;
        if (!inferenceError.empty()) {
            Log(LogLevel::Warning, L"ML generated frame dropped reason=" + inferenceError);
            {
                std::scoped_lock lock(statsMutex_);
                ++renderStats_.inferenceFailures;
                publishedStats_ = renderStats_;
            }
            RetireGeneratedSlot(slotIndex, false);
            pendingGeneratedSlots_.pop_front();
            continue;
        }
        SceneChangeMetrics sceneMetrics;
        const SceneChangeProbeStatus sceneStatus =
            ReadSceneChangeProbe(slot.sceneChangeProbe, sceneMetrics);
        if (sceneStatus != SceneChangeProbeStatus::Ready) {
            Log(LogLevel::Warning,
                L"generated frame dropped reason=scene_probe_" +
                    std::wstring(sceneStatus == SceneChangeProbeStatus::Pending
                        ? L"not_ready" : L"invalid"));
            {
                std::scoped_lock lock(statsMutex_);
                ++renderStats_.generatedDroppedSceneProbeUnavailable;
                publishedStats_ = renderStats_;
            }
            RetireGeneratedSlot(slotIndex, false);
            pendingGeneratedSlots_.pop_front();
            continue;
        }
        if (IsHardSceneCut(sceneMetrics)) {
            Log(LogLevel::Info,
                L"generated frame dropped reason=scene_cut mean_luma_diff=" +
                    std::to_wstring(sceneMetrics.meanAbsoluteLumaDifference) +
                    L" changed_ratio=" +
                    std::to_wstring(sceneMetrics.changedSampleRatio) +
                    L" global_luma_shift=" +
                    std::to_wstring(std::abs(
                        sceneMetrics.meanLeftLuma - sceneMetrics.meanRightLuma)));
            {
                std::scoped_lock lock(statsMutex_);
                ++renderStats_.generatedDroppedSceneCut;
                publishedStats_ = renderStats_;
            }
            RetireGeneratedSlot(slotIndex, false);
            pendingGeneratedSlots_.pop_front();
            continue;
        }
        const bool presented = RenderGeneratedFrame(slotIndex);
        if (!presented) {
            Log(LogLevel::Warning, L"generated tensor composite failed");
        }
        RetireGeneratedSlot(slotIndex, false);
        pendingGeneratedSlots_.pop_front();
    }
}

void D3D12VideoRenderer::ProcessGeneratedBefore(const NativeVideoFrame& nextOriginal) {
    while (!pendingGeneratedSlots_.empty()) {
        const std::size_t slotIndex = pendingGeneratedSlots_.front();
        GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
        if (slot.epoch != frameGraph_.CurrentEpoch() ||
            slot.timelineSerial != nextOriginal.timelineSerial) {
            RetireGeneratedSlot(slotIndex, false);
            pendingGeneratedSlots_.pop_front();
            continue;
        }
        if (slot.targetPts >= nextOriginal.pts) return;
        // The next original frame has reached the presentation queue, so every
        // earlier intermediate frame is already outside its cadence slot. Do
        // not present ready-but-late frames in a burst immediately before the
        // original; that inflated the measured multiplier while looking more
        // juddery than a lower, evenly paced cadence.
        {
            std::scoped_lock lock(statsMutex_);
            ++renderStats_.generatedDroppedNotReady;
            publishedStats_ = renderStats_;
        }
        RetireGeneratedSlot(slotIndex, false);
        pendingGeneratedSlots_.pop_front();
    }
}

void D3D12VideoRenderer::RetireGeneratedSlot(const std::size_t slotIndex,
                                             const bool consumeStatus) {
    if (slotIndex >= generatedSlots_.size()) return;
    GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
    IMlFrameInterpolationExecutor* executor =
        mlExecutor_.load(std::memory_order_acquire);
    if (consumeStatus && !slot.completionStatusConsumed && slot.ready.IsValid() &&
        slot.ready.fence->GetCompletedValue() >= slot.ready.value && executor) {
        executor->TakeCompletionError(slot.ready.value);
        slot.completionStatusConsumed = true;
    }
    if (slot.inputTensor) {
        const GpuFencePoint completion = slot.reusable.IsValid()
            ? slot.reusable : slot.ready;
        tensorPreprocessor_.ReleaseRetained(slot.inputTensor.Get(), completion);
        slot.inputTensor.Reset();
    }
    slot.luminanceLeft.Reset();
    slot.luminanceRight.Reset();
    slot.sceneChangeProbe = {};
    slot.pending = false;
    slot.anchored = false;
    slot.displayDomainReady = false;
}

bool D3D12VideoRenderer::RenderGeneratedFrame(const std::size_t slotIndex) {
    if (slotIndex >= generatedSlots_.size() || !tensorPipeline_ || !tensorSrvHeap_) {
        return false;
    }
    GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
    if (!slot.output || !slot.inputTensor || !slot.shape.IsValid()) return false;
    if (slot.inputTransfer != 1u && slot.inputTransfer != 4u) return false;
    // A generated frame may only be presented while its left endpoint is the
    // active cadence anchor. Color authority is the fixed display-domain
    // contract shared by A and B. Subtitle state is composed independently.
    if (!slot.displayDomainReady || presentedOriginalTimeline_ != slot.timelineSerial ||
        presentedOriginalPts_ != slot.leftPts) {
        return false;
    }
    // The left original already established the swap-chain color space and
    // display metadata. A generated frame must inherit that state, never
    // switch it independently. If an external change invalidated the state,
    // drop the generated frame instead of risking a brightness pulse.
    if (swapChainHdr_ != slot.hdrOutput) return false;
    RefreshDisplayPeakNits();
    if (std::abs(TargetPeakNits(videoSettings_, displayCapabilities_) -
                 slot.targetPeakNits) > 0.5f) {
        return false;
    }
    WaitForFrameLatencyObject();
    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!WaitForBackBuffer(backBufferIndex)) return false;
    inFlightFrames_[backBufferIndex].reset();
    transients_[backBufferIndex].clear();
    const bool fullResolutionEndpointLuma =
        slot.luminanceLeft && slot.luminanceRight && slot.inputTransfer == 4u;
    if (fullResolutionEndpointLuma) {
        try {
            // The slot is retired as soon as its frame is presented. Keep the
            // sampled endpoint resources alive until this backbuffer's fence is
            // complete, just like other command-list transients.
            transients_[backBufferIndex].push_back(slot.luminanceLeft);
            transients_[backBufferIndex].push_back(slot.luminanceRight);
        } catch (...) {
            return false;
        }
    }
    if (FAILED(allocators_[backBufferIndex]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[backBufferIndex].Get(), tensorPipeline_.Get()))) {
        return false;
    }
    std::array<D3D12_RESOURCE_BARRIER, 5> beginBarriers{
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET),
        TransitionBarrier(slot.output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(slot.inputTensor.Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    UINT beginBarrierCount = 3;
    if (fullResolutionEndpointLuma) {
        beginBarriers[beginBarrierCount++] = TransitionBarrier(
            slot.luminanceLeft.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        beginBarriers[beginBarrierCount++] = TransitionBarrier(
            slot.luminanceRight.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    commandList_->ResourceBarrier(beginBarrierCount, beginBarriers.data());

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
        tensorSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(backBufferIndex * 4) * srvIncrement_;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = static_cast<UINT>(slot.capacityBytes / sizeof(UINT));
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(slot.output.Get(), &view, srvCpu);
    srvCpu.ptr += srvIncrement_;
    view.Buffer.NumElements = static_cast<UINT>(
        slot.inputTensor->GetDesc().Width / sizeof(UINT));
    device_->CreateShaderResourceView(slot.inputTensor.Get(), &view, srvCpu);
    srvCpu.ptr += srvIncrement_;
    D3D12_SHADER_RESOURCE_VIEW_DESC endpointView{};
    endpointView.Format = kLibplaceboTargetFormat;
    endpointView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    endpointView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    endpointView.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        fullResolutionEndpointLuma ? slot.luminanceLeft.Get() : nullptr,
        &endpointView, srvCpu);
    srvCpu.ptr += srvIncrement_;
    device_->CreateShaderResourceView(
        fullResolutionEndpointLuma ? slot.luminanceRight.Get() : nullptr,
        &endpointView, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvIncrement_;
    constexpr float clearColor[4]{};
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    const D3D12_VIEWPORT viewport = AspectFitVideoViewport(
        slot.displayWidth, slot.displayHeight, width_, height_);
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->SetGraphicsRootSignature(tensorRootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {tensorSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu =
        tensorSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(backBufferIndex * 4) * srvIncrement_;
    commandList_->SetGraphicsRootDescriptorTable(0, srvGpu);
    TensorCompositionConstants constants{};
    constants.tensorWidth = slot.shape.width;
    constants.tensorHeight = slot.shape.height;
    constants.hdrOutput = slot.hdrOutput ? 1.0f : 0.0f;
    constants.transfer = slot.inputTransfer;
    constants.endpointReference[0] = fullResolutionEndpointLuma ? 1.0f : 0.0f;
    const D3D12_RESOURCE_DESC referenceDesc = fullResolutionEndpointLuma
        ? slot.luminanceLeft->GetDesc() : D3D12_RESOURCE_DESC{};
    const bool luminanceReferenceChanged =
        fullResolutionEndpointLuma &&
        (referenceDesc.Width != loggedEndpointLuminanceReferenceWidth_ ||
         referenceDesc.Height != loggedEndpointLuminanceReferenceHeight_);
    if (constants.transfer == 4u &&
        (!displayDomainInterpolationLuminanceGuardLogged_ ||
         luminanceReferenceChanged)) {
        displayDomainInterpolationLuminanceGuardLogged_ = true;
        if (fullResolutionEndpointLuma) {
            loggedEndpointLuminanceReferenceWidth_ =
                static_cast<UINT>(referenceDesc.Width);
            loggedEndpointLuminanceReferenceHeight_ = referenceDesc.Height;
            Log(LogLevel::Info,
                L"HDR display-domain interpolation luminance contract="
                L"motion_preserving_full_resolution_exposure_guard "
                L"domain=display_mapped_bt2020_pq "
                L"reference=" + std::to_wstring(referenceDesc.Width) + L"x" +
                std::to_wstring(referenceDesc.Height) + L" tensor=" +
                std::to_wstring(slot.shape.width) + L"x" +
                std::to_wstring(slot.shape.height));
        } else {
            loggedEndpointLuminanceReferenceWidth_ = 0;
            loggedEndpointLuminanceReferenceHeight_ = 0;
            Log(LogLevel::Info,
                L"HDR display-domain interpolation luminance contract="
                L"motion_preserving_temporal_exposure_guard "
                L"domain=display_mapped_bt2020_pq");
        }
    }
    // There are intentionally no tone-map or metadata fields in this final
    // interface. Endpoint preprocessing owns the complete display transform.
    commandList_->SetGraphicsRoot32BitConstants(1, 8, &constants, 0);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);
    DrawOverlays(viewport, backBufferIndex);

    std::array<D3D12_RESOURCE_BARRIER, 5> endBarriers{
        TransitionBarrier(slot.output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        TransitionBarrier(slot.inputTensor.Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT),
    };
    UINT endBarrierCount = 3;
    if (fullResolutionEndpointLuma) {
        endBarriers[endBarrierCount++] = TransitionBarrier(
            slot.luminanceLeft.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
        endBarriers[endBarrierCount++] = TransitionBarrier(
            slot.luminanceRight.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    }
    commandList_->ResourceBarrier(endBarrierCount, endBarriers.data());
    if (FAILED(commandList_->Close())) return false;
    const std::array dependencies{slot.ready};
    SubmitResult graphSubmit = frameGraph_.Submit(
        GpuFrameQueue::Graphics, commandList_.Get(), dependencies);
    if (!graphSubmit.accepted) return false;
    // Both the generated output and retained endpoint tensor are sampled by
    // this submission. Publish their reuse fence before Present, because a
    // presentation failure must not allow either resource to be recycled while
    // the graphics queue is still consuming it.
    slot.reusable = graphSubmit.completion;
    if (!PresentComposedFrame(
            backBufferIndex, graphSubmit.completion, false)) {
        return false;
    }
    if (!generatedPresentLogged_) {
        generatedPresentLogged_ = true;
        Log(LogLevel::Info,
            L"generated frame presented path=dml_output_buffer_to_final_composite");
    }
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.frames;
        ++renderStats_.generatedFrames;
        publishedStats_ = renderStats_;
    }
    return true;
}

bool D3D12VideoRenderer::EnsureTextSubtitleBitmap(
    const std::wstring& text,
    const D3D12_VIEWPORT& videoViewport) {
    if (text.empty() || width_ == 0 || height_ == 0 ||
        static_cast<uint64_t>(width_) * height_ > k8KPixelCount) {
        textSubtitlePixels_.reset();
        textSubtitleText_.clear();
        return false;
    }

    const double fontScale = std::clamp(activeSubtitleSettings_.fontScale, 0.5, 2.0);
    const int offsetX = activeSubtitleSettings_.offsetXPx;
    const int offsetY = activeSubtitleSettings_.offsetYPx;
    if (textSubtitlePixels_ && textSubtitleText_ == text &&
        textSubtitleSurfaceWidth_ == width_ && textSubtitleSurfaceHeight_ == height_ &&
        SameViewport(textSubtitleViewport_, videoViewport) &&
        std::abs(textSubtitleFontScale_ - fontScale) < 0.001 &&
        textSubtitleOffsetXPx_ == offsetX && textSubtitleOffsetYPx_ == offsetY) {
        return true;
    }

    const std::size_t pixelBytes = static_cast<std::size_t>(width_) * height_ * 4;
    auto pixels = std::make_shared<std::vector<uint8_t>>(pixelBytes, 0);
    Gdiplus::Bitmap bitmap(static_cast<INT>(width_), static_cast<INT>(height_),
                           PixelFormat32bppPARGB);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;

    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float baseFontPixels = std::clamp(videoViewport.Height * 0.048f, 20.0f, 54.0f);
    const float fontPixels = baseFontPixels * static_cast<float>(fontScale);
    const float maxTextWidth = std::max(1.0f, videoViewport.Width * 0.84f);
    const float marginBottom = std::clamp(videoViewport.Height * 0.085f, 22.0f, 86.0f);
    const float layoutHeight = std::min(
        videoViewport.Height * 0.34f,
        std::max(fontPixels * 2.1f,
                 fontPixels * (static_cast<float>(CountSubtitleLines(text)) + 1.2f)));
    const float layoutLeft = videoViewport.TopLeftX +
        (videoViewport.Width - maxTextWidth) * 0.5f + static_cast<float>(offsetX);
    const float layoutTop = std::max(
        videoViewport.TopLeftY,
        videoViewport.TopLeftY + videoViewport.Height - marginBottom - layoutHeight) +
        static_cast<float>(offsetY);
    const Gdiplus::RectF layout(layoutLeft, layoutTop, maxTextWidth, layoutHeight);

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentFar);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
    Gdiplus::GraphicsPath textPath;
    textPath.AddString(text.c_str(), -1, &family, Gdiplus::FontStyleBold,
                       fontPixels, layout, &format);
    const float outlineWidth = std::clamp(fontPixels * 0.16f, 3.0f, 8.0f);
    Gdiplus::Pen outline(Gdiplus::Color(220, 0, 0, 0), outlineWidth);
    outline.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush fill(Gdiplus::Color(245, 255, 255, 255));
    graphics.DrawPath(&outline, &textPath);
    graphics.FillPath(&fill, &textPath);

    Gdiplus::BitmapData bitmapData{};
    Gdiplus::Rect lockRect(0, 0, static_cast<INT>(width_), static_cast<INT>(height_));
    if (bitmap.LockBits(&lockRect, Gdiplus::ImageLockModeRead,
                        PixelFormat32bppPARGB, &bitmapData) != Gdiplus::Ok) {
        return false;
    }
    const auto* source = static_cast<const uint8_t*>(bitmapData.Scan0);
    const int sourceStride = bitmapData.Stride;
    const std::size_t destinationStride = static_cast<std::size_t>(width_) * 4;
    for (UINT row = 0; row < height_; ++row) {
        const uint8_t* sourceRow = sourceStride >= 0
            ? source + static_cast<std::size_t>(sourceStride) * row
            : source + static_cast<std::size_t>(-sourceStride) * (height_ - 1 - row);
        std::memcpy(pixels->data() + destinationStride * row, sourceRow,
                    destinationStride);
    }
    bitmap.UnlockBits(&bitmapData);

    textSubtitlePixels_ = std::move(pixels);
    textSubtitleText_ = text;
    textSubtitleViewport_ = videoViewport;
    textSubtitleSurfaceWidth_ = width_;
    textSubtitleSurfaceHeight_ = height_;
    textSubtitleFontScale_ = fontScale;
    textSubtitleOffsetXPx_ = offsetX;
    textSubtitleOffsetYPx_ = offsetY;
    if (++textSubtitleSerial_ == 0) ++textSubtitleSerial_;
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.subtitleSurfaceRebuilds;
    }
    Log(LogLevel::Debug,
        L"subtitle text fallback surface=" + std::to_wstring(width_) + L"x" +
            std::to_wstring(height_) + L" lines=" +
            std::to_wstring(CountSubtitleLines(text)));
    return true;
}

void D3D12VideoRenderer::BindPresentedSubtitles(const NativeVideoFrame& frame) {
    // Keep subtitle composition outside the interpolation slots. A generated
    // video frame reuses this latest presentation-timeline snapshot instead of
    // carrying either endpoint's subtitle state through the ML pipeline.
    presentedSubtitleText_ = frame.subtitleText;
    presentedSubtitleBitmaps_ = frame.subtitleBitmaps;
    presentedSubtitleSourceWidth_ = frame.width;
    presentedSubtitleSourceHeight_ = frame.height;
}

bool D3D12VideoRenderer::DrawOverlays(const D3D12_VIEWPORT& videoViewport,
                                      const UINT backBufferIndex) {
    if (!overlayPipeline_ || !overlayRootSignature_ || !overlaySrvHeap_ ||
        backBufferIndex >= kBufferCount) {
        return false;
    }
    struct DrawItem {
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        int width = 0;
        int height = 0;
        int stride = 0;
        float x = 0.0f;
        float y = 0.0f;
        float displayWidth = 0.0f;
        float displayHeight = 0.0f;
        float opacity = 1.0f;
        uint64_t serial = 0;
        std::size_t overlaySlotIndex = 0;
        bool alphaFromRgb = false;
        bool subtitle = false;
    };
    std::vector<DrawItem> items;
    items.reserve(kMaxOverlayTextures);
    std::size_t activeUiOverlayCount = 0;
    for (const OverlaySlot& slot : activeOverlaySlots_) {
        if (slot.bitmap && slot.bitmap->bgraPremultiplied &&
            slot.bitmap->width > 0 && slot.bitmap->height > 0) {
            ++activeUiOverlayCount;
        }
    }
    const std::size_t subtitleItemLimit = kMaxOverlayTextures -
        std::min<std::size_t>(activeUiOverlayCount, kMaxOverlayTextures);
    bool hasDrawableSubtitleBitmap = false;
    for (const NativeSubtitleBitmap& bitmap : presentedSubtitleBitmaps_) {
        if (!bitmap.HasPixels() || items.size() >= subtitleItemLimit) continue;
        hasDrawableSubtitleBitmap = true;
        const int canvasWidth = std::max(
            1, bitmap.canvasWidth > 0 ? bitmap.canvasWidth
                                     : presentedSubtitleSourceWidth_);
        const int canvasHeight = std::max(
            1, bitmap.canvasHeight > 0 ? bitmap.canvasHeight
                                      : presentedSubtitleSourceHeight_);
        DrawItem item;
        item.pixels = bitmap.bgra;
        item.width = bitmap.width;
        item.height = bitmap.height;
        item.stride = bitmap.stride;
        item.serial = bitmap.serial;
        const float rawX = videoViewport.TopLeftX +
            videoViewport.Width * bitmap.x / canvasWidth;
        const float rawY = videoViewport.TopLeftY +
            videoViewport.Height * bitmap.y / canvasHeight;
        const float rawWidth = videoViewport.Width * bitmap.width / canvasWidth;
        const float rawHeight = videoViewport.Height * bitmap.height / canvasHeight;
        const float scale = static_cast<float>(
            std::clamp(activeSubtitleSettings_.fontScale, 0.5, 2.0));
        const float pivotX = videoViewport.TopLeftX + videoViewport.Width * 0.5f;
        const float pivotY = videoViewport.TopLeftY + videoViewport.Height;
        item.x = pivotX + (rawX - pivotX) * scale +
            static_cast<float>(activeSubtitleSettings_.offsetXPx);
        item.y = pivotY + (rawY - pivotY) * scale +
            static_cast<float>(activeSubtitleSettings_.offsetYPx);
        item.displayWidth = rawWidth * scale;
        item.displayHeight = rawHeight * scale;
        item.subtitle = true;
        items.push_back(std::move(item));
    }
    if (!hasDrawableSubtitleBitmap && items.size() < subtitleItemLimit &&
        EnsureTextSubtitleBitmap(presentedSubtitleText_, videoViewport)) {
        DrawItem item;
        item.pixels = textSubtitlePixels_;
        item.width = static_cast<int>(width_);
        item.height = static_cast<int>(height_);
        item.stride = static_cast<int>(width_ * 4);
        item.serial = textSubtitleSerial_;
        item.displayWidth = static_cast<float>(width_);
        item.displayHeight = static_cast<float>(height_);
        item.subtitle = true;
        items.push_back(std::move(item));
    }
    for (std::size_t slotIndex = 0;
         slotIndex < activeOverlaySlots_.size() && items.size() < kMaxOverlayTextures;
         ++slotIndex) {
        const OverlaySlot& slot = activeOverlaySlots_[slotIndex];
        if (!slot.bitmap || !slot.bitmap->bgraPremultiplied ||
            slot.bitmap->width <= 0 || slot.bitmap->height <= 0) {
            continue;
        }
        const std::size_t required = static_cast<std::size_t>(slot.bitmap->width) *
            slot.bitmap->height * 4;
        if (slot.bitmap->bgraPremultiplied->size() < required) continue;
        DrawItem item;
        item.pixels = slot.bitmap->bgraPremultiplied;
        item.width = slot.bitmap->width;
        item.height = slot.bitmap->height;
        item.stride = slot.bitmap->width * 4;
        item.serial = reinterpret_cast<uintptr_t>(slot.bitmap->bgraPremultiplied.get());
        const bool hasPresentation = slotIndex == 1 &&
            slot.presentation.width > 0 && slot.presentation.height > 0;
        item.x = static_cast<float>(hasPresentation
                                        ? slot.presentation.x
                                        : slot.bitmap->destinationX);
        item.y = static_cast<float>(hasPresentation
                                        ? slot.presentation.y
                                        : slot.bitmap->destinationY);
        item.displayWidth = static_cast<float>(hasPresentation
                                                   ? slot.presentation.width
                                                   : slot.bitmap->width);
        item.displayHeight = static_cast<float>(hasPresentation
                                                    ? slot.presentation.height
                                                    : slot.bitmap->height);
        // D3D11 treats the live presentation opacity as the current animation
        // value. Multiplying it by the bitmap opacity applies the fade twice.
        item.opacity = std::clamp(hasPresentation
                                      ? slot.presentation.opacity
                                      : slot.bitmap->opacity,
                                  0.0f, 1.0f);
        item.overlaySlotIndex = slotIndex;
        item.alphaFromRgb = slot.bitmap->alphaFromRgb;
        items.push_back(std::move(item));
    }
    if (items.empty()) return true;

    ID3D12DescriptorHeap* heaps[] = {overlaySrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, heaps);
    commandList_->SetGraphicsRootSignature(overlayRootSignature_.Get());
    commandList_->SetPipelineState(overlayPipeline_.Get());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetScissorRects(1, &scissor);

    bool drewSubtitle = false;
    bool drewUiOverlay = false;
    UINT subtitleItemIndex = 0;
    for (UINT itemIndex = 0; itemIndex < static_cast<UINT>(items.size()); ++itemIndex) {
        const DrawItem& item = items[itemIndex];
        OverlayTextureCache& cache = item.subtitle
            ? subtitleTextureCaches_[backBufferIndex][subtitleItemIndex++]
            : overlayTextureCaches_[backBufferIndex][item.overlaySlotIndex];
        const void* sourceIdentity = item.pixels.get();
        const bool identityChanged = cache.sourceIdentity != sourceIdentity ||
            cache.sourceSerial != item.serial;
        // Subtitle textures are owned by the current back buffer. The caller
        // waited for that buffer's fence before recording, so changing a cue can
        // never mutate a texture still sampled by the other back buffer.
        const bool dimensionsChanged = !cache.texture ||
            cache.width != static_cast<UINT>(item.width) ||
            cache.height != static_cast<UINT>(item.height);
        const bool contentChanged = dimensionsChanged || identityChanged;
        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = static_cast<UINT64>(item.width);
        textureDesc.Height = static_cast<UINT>(item.height);
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (dimensionsChanged) {
            cache.texture.Reset();
            cache.sourceIdentity = nullptr;
            cache.sourceSerial = 0;
            if (FAILED(device_->CreateCommittedResource(
                    &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&cache.texture)))) {
                continue;
            }
            UINT rows = 0;
            UINT64 rowSize = 0;
            UINT64 uploadBytes = 0;
            device_->GetCopyableFootprints(&textureDesc, 0, 1, 0, &cache.footprint,
                                           &rows, &rowSize, &uploadBytes);
            if (!cache.upload || cache.uploadCapacity < uploadBytes) {
                if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
                cache.upload.Reset();
                cache.mappedUpload = nullptr;
                D3D12_RESOURCE_DESC uploadDesc{};
                uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                uploadDesc.Width = uploadBytes;
                uploadDesc.Height = 1;
                uploadDesc.DepthOrArraySize = 1;
                uploadDesc.MipLevels = 1;
                uploadDesc.SampleDesc.Count = 1;
                uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                D3D12_HEAP_PROPERTIES uploadHeap{};
                uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
                if (FAILED(device_->CreateCommittedResource(
                        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                        IID_PPV_ARGS(&cache.upload)))) {
                    cache.texture.Reset();
                    continue;
                }
                const D3D12_RANGE noRead{0, 0};
                if (FAILED(cache.upload->Map(0, &noRead, &cache.mappedUpload))) {
                    cache.upload.Reset();
                    cache.texture.Reset();
                    continue;
                }
                cache.uploadCapacity = uploadBytes;
            }
            cache.width = static_cast<UINT>(item.width);
            cache.height = static_cast<UINT>(item.height);
        }
        if (!cache.texture || !cache.upload || !cache.mappedUpload) continue;
        if (contentChanged) {
            if (!dimensionsChanged) {
                D3D12_RESOURCE_BARRIER writable = TransitionBarrier(
                    cache.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
                commandList_->ResourceBarrier(1, &writable);
            }
            auto* mapped = static_cast<uint8_t*>(cache.mappedUpload);
            const std::size_t copyBytes = static_cast<std::size_t>(item.width) * 4;
            for (int row = 0; row < item.height; ++row) {
                std::memcpy(mapped + cache.footprint.Offset +
                                static_cast<std::size_t>(row) *
                                    cache.footprint.Footprint.RowPitch,
                            item.pixels->data() + static_cast<std::size_t>(row) * item.stride,
                            copyBytes);
            }
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = cache.texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = cache.upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = cache.footprint;
            commandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            D3D12_RESOURCE_BARRIER readyBarrier = TransitionBarrier(
                cache.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList_->ResourceBarrier(1, &readyBarrier);
            cache.sourceIdentity = sourceIdentity;
            cache.sourceSerial = item.serial;
        }

        const UINT descriptorIndex = backBufferIndex * kMaxOverlayTextures + itemIndex;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            overlaySrvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(descriptorIndex) * srvIncrement_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = textureDesc.Format;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(cache.texture.Get(), &srv, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            overlaySrvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(descriptorIndex) * srvIncrement_;
        commandList_->SetGraphicsRootDescriptorTable(0, gpu);
        struct OverlayConstants {
            float opacity;
            UINT alphaFromRgb;
            float sdrWhiteNits;
            float hdrComposition;
            float padding[4];
        } constants{item.opacity, item.alphaFromRgb ? 1u : 0u, 203.0f,
                    nativeHlgComposition_ ? 2.0f
                                          : (swapChainHdr_ ? 1.0f : 0.0f),
                    {}};
        commandList_->SetGraphicsRoot32BitConstants(1, 8, &constants, 0);
        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = item.x;
        viewport.TopLeftY = item.y;
        viewport.Width = std::max(1.0f, item.displayWidth);
        viewport.Height = std::max(1.0f, item.displayHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        commandList_->RSSetViewports(1, &viewport);
        commandList_->DrawInstanced(3, 1, 0, 0);
        drewSubtitle = drewSubtitle || item.subtitle;
        drewUiOverlay = drewUiOverlay || !item.subtitle;
    }
    if (drewSubtitle) {
        if (!independentSubtitleCompositionLogged_) {
            independentSubtitleCompositionLogged_ = true;
            Log(LogLevel::Info,
                L"subtitle composition active mode=independent_post_interpolation "
                L"authority=presented_timeline viewport=aspect_fit output=" +
                    std::to_wstring(width_) + L"x" + std::to_wstring(height_) +
                    L" video_rect=" +
                    std::to_wstring(static_cast<int>(std::lround(
                        videoViewport.TopLeftX))) + L"," +
                    std::to_wstring(static_cast<int>(std::lround(
                        videoViewport.TopLeftY))) + L"," +
                    std::to_wstring(static_cast<int>(std::lround(
                        videoViewport.Width))) + L"x" +
                    std::to_wstring(static_cast<int>(std::lround(
                        videoViewport.Height))));
        }
        if (!perBackBufferSubtitleTextureLogged_) {
            perBackBufferSubtitleTextureLogged_ = true;
            Log(LogLevel::Info,
                L"subtitle texture cache active mode=per_backbuffer_fence_owned");
        }
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.subtitleFrames;
    }
    if (drewUiOverlay && !independentUiOverlayCompositionLogged_) {
        independentUiOverlayCompositionLogged_ = true;
        Log(LogLevel::Info,
            L"ui overlay composition active mode=independent_post_interpolation "
            L"source=presentation_snapshot");
    }
    return true;
}

bool D3D12VideoRenderer::RenderLastFrame() {
    return currentFrame_ && RenderFrame(*currentFrame_);
}

bool D3D12VideoRenderer::SetHdrOutputState(const NativeVideoFrame& frame,
                                           const bool enabled) {
    const bool fixedInterpolationDisplayDomain =
        interpolationRequested_.load(std::memory_order_acquire);
    bool active = enabled && hdr10ColorSpaceSupported_;
    const DXGI_COLOR_SPACE_TYPE desiredColorSpace = active
        ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (swapChain_ && (swapChainHdr_ != active || loggedHdrOutputState_ < 0) &&
        FAILED(swapChain_->SetColorSpace1(desiredColorSpace))) {
        if (active) {
            hdr10ColorSpaceSupported_ = false;
            active = false;
            swapChain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
    }
    swapChainHdr_ = active;

    nvidiaHdrOutput_.SetLogHandler(
        [this](const std::wstring& message) { Log(LogLevel::Info, message); });
    bool hdr10PlusDisplayPath = false;
    if (active) {
        hdr10PlusDisplayPath = !fixedInterpolationDisplayDomain &&
            videoSettings_.displayMetadataPassthrough &&
            frame.hdr10PlusPayload && !frame.hdr10PlusPayload->empty() &&
            nvidiaHdrOutput_.ApplyHdr10PlusGaming(host_.load(), frame.color);
        if (!hdr10PlusDisplayPath) {
            auto outputColor = frame.color;
            if (fixedInterpolationDisplayDomain) {
                const double targetPeak =
                    TargetPeakNits(videoSettings_, displayCapabilities_);
                outputColor.masteringDisplay.hasPrimaries = true;
                outputColor.masteringDisplay.red = {0.708, 0.292};
                outputColor.masteringDisplay.green = {0.170, 0.797};
                outputColor.masteringDisplay.blue = {0.131, 0.046};
                outputColor.masteringDisplay.whitePoint = {0.3127, 0.3290};
                outputColor.masteringDisplay.hasLuminance = true;
                outputColor.masteringDisplay.minLuminanceNits = 0.005;
                outputColor.masteringDisplay.maxLuminanceNits = targetPeak;
                outputColor.contentLight.hasValues = true;
                outputColor.contentLight.maxContentLightLevelNits =
                    static_cast<int>(std::lround(targetPeak));
                outputColor.contentLight.maxFrameAverageLightLevelNits =
                    static_cast<int>(std::lround(targetPeak));
            }
            nvidiaHdrOutput_.ApplyHdr10(host_.load(), outputColor);
        }
    } else {
        nvidiaHdrOutput_.Restore();
    }
    if (frame.hdr10Plus && frame.hdr10Plus->valid &&
        loggedHdr10PlusFingerprint_ == 0) {
        loggedHdr10PlusFingerprint_ = frame.hdr10Plus->fingerprint;
        Log(LogLevel::Info,
            L"HDR10+ dynamic metadata frame fingerprint=" +
                std::to_wstring(frame.hdr10Plus->fingerprint) +
                L" path=" +
                std::wstring(hdr10PlusDisplayPath
                                 ? L"nvapi_hdr10plus_gaming"
                                 : L"shader_st2094_40"));
    }

    // Dolby Vision is already display-mapped by libplacebo/RPU processing, so
    // retain the old D3D11 rule of signaling PQ/BT.2020 without stacking static
    // HDR10 mastering metadata. Ordinary HDR10 keeps its stream metadata.
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain4;
    if (swapChain_ && SUCCEEDED(swapChain_.As(&swapChain4)) && swapChain4) {
        const auto* dovi = FrameDolbyVisionMetadata(frame);
        if (!active || dovi) {
            swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
        } else {
            const auto chromaticity = [](const double value) {
                return static_cast<UINT16>(
                    std::clamp(value * 50000.0, 0.0, 65535.0) + 0.5);
            };
            const auto luminance = [](const double value) {
                return static_cast<UINT>(
                    std::clamp(value * 10000.0, 0.0, 4294967295.0) + 0.5);
            };
            DXGI_HDR_METADATA_HDR10 metadata{};
            const auto& mastering = frame.color.masteringDisplay;
            const auto setPrimary = [&chromaticity](UINT16 (&target)[2],
                                                    const auto& point) {
                target[0] = chromaticity(point.x);
                target[1] = chromaticity(point.y);
            };
            if (mastering.hasPrimaries && !fixedInterpolationDisplayDomain) {
                setPrimary(metadata.RedPrimary, mastering.red);
                setPrimary(metadata.GreenPrimary, mastering.green);
                setPrimary(metadata.BluePrimary, mastering.blue);
                setPrimary(metadata.WhitePoint, mastering.whitePoint);
            } else {
                metadata.RedPrimary[0] = chromaticity(0.708);
                metadata.RedPrimary[1] = chromaticity(0.292);
                metadata.GreenPrimary[0] = chromaticity(0.170);
                metadata.GreenPrimary[1] = chromaticity(0.797);
                metadata.BluePrimary[0] = chromaticity(0.131);
                metadata.BluePrimary[1] = chromaticity(0.046);
                metadata.WhitePoint[0] = chromaticity(0.3127);
                metadata.WhitePoint[1] = chromaticity(0.3290);
            }
            const bool appSideDisplayMapping = fixedInterpolationDisplayDomain ||
                !videoSettings_.displayMetadataPassthrough;
            const double targetPeakNits =
                TargetPeakNits(videoSettings_, displayCapabilities_);
            const double sourceMasteringPeak =
                mastering.hasLuminance ? mastering.maxLuminanceNits : 1000.0;
            const double signaledPeak = fixedInterpolationDisplayDomain
                ? targetPeakNits
                : (appSideDisplayMapping
                       ? std::clamp(sourceMasteringPeak, 100.0, targetPeakNits)
                       : sourceMasteringPeak);
            metadata.MaxMasteringLuminance = luminance(signaledPeak);
            metadata.MinMasteringLuminance = luminance(
                fixedInterpolationDisplayDomain
                    ? 0.005
                    : (mastering.hasLuminance
                           ? mastering.minLuminanceNits : 0.005));
            if (fixedInterpolationDisplayDomain) {
                metadata.MaxContentLightLevel = static_cast<UINT16>(
                    std::clamp(targetPeakNits, 0.0, 65535.0));
                metadata.MaxFrameAverageLightLevel =
                    metadata.MaxContentLightLevel;
            } else if (frame.color.contentLight.hasValues) {
                metadata.MaxContentLightLevel = static_cast<UINT16>(std::clamp(
                    appSideDisplayMapping
                        ? std::min<double>(
                              frame.color.contentLight.maxContentLightLevelNits,
                              targetPeakNits)
                        : frame.color.contentLight.maxContentLightLevelNits,
                    0.0, 65535.0));
                metadata.MaxFrameAverageLightLevel = static_cast<UINT16>(std::clamp(
                    appSideDisplayMapping
                        ? std::min<double>(
                              frame.color.contentLight.maxFrameAverageLightLevelNits,
                              targetPeakNits)
                        : frame.color.contentLight.maxFrameAverageLightLevelNits,
                    0.0, 65535.0));
            }
            swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                       sizeof(metadata), &metadata);
        }
    }

    const int state = active ? 1 : 0;
    if (loggedHdrOutputState_ == state) {
        return active;
    }
    loggedHdrOutputState_ = state;

    const auto* dovi = FrameDolbyVisionMetadata(frame);
    std::wstring source;
    if (dovi) {
        source = L"dolby_vision_p" + std::to_wstring(dovi->profile);
    } else if (frame.color.transfer == VideoTransferCharacteristic::Pq) {
        source = L"pq";
    } else if (frame.color.transfer == VideoTransferCharacteristic::Hlg) {
        source = L"hlg";
    } else {
        source = L"sdr";
    }

    Log(LogLevel::Info,
        L"HDR composition mode=" +
            std::wstring(active ? L"active_hdr10_pq" : L"sdr_tonemap") +
            L" source=" + source +
            L" ui_hdr=" +
            std::wstring(videoSettings_.dolbyVisionHdrOutput ? L"on" : L"off") +
            L" advanced_color=" +
            std::wstring(displayCapabilities_.hdrEnabled ? L"on" : L"off") +
            L" hdr10_space=" + std::wstring(hdr10ColorSpaceSupported_ ? L"on" : L"off") +
            L" source_peak_nits=" +
            std::to_wstring(static_cast<int>(FrameSourcePeakNits(frame) + 0.5f)) +
            L" target_peak_nits=" +
            std::to_wstring(static_cast<int>(
                TargetPeakNits(videoSettings_, displayCapabilities_) + 0.5f)));
    return active;
}

void D3D12VideoRenderer::ResetInterpolationState() {
    tensorPreviousFrame_.reset();
    tensorPreviousDoviTarget_.Reset();
    tensorPreviousDoviLuminanceTarget_.Reset();
    tensorPreviousDoviTargetTimeline_ = 0;
    tensorPreviousDoviTargetFrameSerial_ = 0;
    tensorPreviousDoviMetadataFingerprint_ = 0;
    tensorPreviousDoviTargetPeakNits_ = 0.0f;
    tensorPreviousDoviHdrOutput_ = false;
    sourceComputeCompletions_.clear();
    sourceGraphicsCompletions_.clear();
    for (const std::size_t slotIndex : pendingGeneratedSlots_) {
        RetireGeneratedSlot(slotIndex, false);
    }
    pendingGeneratedSlots_.clear();
    tensorPreprocessorLogged_ = false;
    sceneChangeProbeLogged_ = false;
    directMlSubmitLogged_ = false;
    generatedPresentLogged_ = false;
    interpolationDisplayDomainLogged_ = false;
    dolbyVisionInterpolationColorPathLogged_ = false;
    displayDomainInterpolationLuminanceGuardLogged_ = false;
    loggedEndpointLuminanceReferenceWidth_ = 0;
    loggedEndpointLuminanceReferenceHeight_ = 0;
    hlgInterpolationColorPathLogged_ = false;
    frameGraph_.AdvanceEpoch();
    {
        std::scoped_lock lock(statsMutex_);
        renderStats_.interpolationMultiplier = 1;
        publishedStats_ = renderStats_;
    }
}

void D3D12VideoRenderer::ClearFrame() {
    currentFrame_.reset();
    presentedSubtitleText_.clear();
    presentedSubtitleBitmaps_.clear();
    presentedSubtitleSourceWidth_ = 0;
    presentedSubtitleSourceHeight_ = 0;
    swapChainHdr_ = false;
    nativeHlgComposition_ = false;
    SelectCompositionSwapChain(false);
    loggedHdrOutputState_ = -1;
    presentedOriginalTimeline_ = 0;
    presentedOriginalPts_ = {};
    ResetInterpolationState();
    if (!swapChain_ || !commandList_) return;
    const UINT index = swapChain_->GetCurrentBackBufferIndex();
    if (!WaitForBackBuffer(index)) return;
    if (FAILED(allocators_[index]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[index].Get(), nullptr))) return;
    auto begin = TransitionBarrier(backBuffers_[index].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &begin);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * rtvIncrement_;
    constexpr float black[4]{};
    commandList_->ClearRenderTargetView(rtv, black, 0, nullptr);
    auto end = TransitionBarrier(backBuffers_[index].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    commandList_->ResourceBarrier(1, &end);
    if (FAILED(commandList_->Close())) return;
    const SubmitResult graphSubmit = frameGraph_.Submit(
        GpuFrameQueue::Graphics, commandList_.Get());
    if (!graphSubmit.accepted) return;
    PresentComposedFrame(index, graphSubmit.completion, false);
}

bool D3D12VideoRenderer::WaitForBackBuffer(const UINT index) {
    if (!fence_ || index >= kBufferCount) return true;
    const uint64_t value = bufferFenceValues_[index];
    if (value == 0 || fence_->GetCompletedValue() >= value) return true;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_))) {
        if (WaitForSingleObject(fenceEvent_, 2000) == WAIT_OBJECT_0) return true;
    }
    Log(LogLevel::Error, L"d3d12 back buffer fence timeout");
    return false;
}

bool D3D12VideoRenderer::WaitForHlgBackBuffer(const UINT index) {
    if (!fence_ || index >= kBufferCount) return true;
    const uint64_t value = hlgBufferFenceValues_[index];
    if (value == 0 || fence_->GetCompletedValue() >= value) return true;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_)) &&
        WaitForSingleObject(fenceEvent_, 2000) == WAIT_OBJECT_0) {
        return true;
    }
    Log(LogLevel::Error, L"d3d12 HLG back buffer fence timeout");
    return false;
}

void D3D12VideoRenderer::ReleaseGpu() {
    nvidiaHdrOutput_.Restore();
    if (queue_ && fence_ && fenceEvent_) {
        const uint64_t value = nextFenceValue_++;
        if (SUCCEEDED(queue_->Signal(fence_.Get(), value)) &&
            fence_->GetCompletedValue() < value &&
            SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_))) {
            WaitForSingleObject(fenceEvent_, 2000);
        }
    }
    if (mlInitializationThread_.joinable()) {
        mlInitializationThread_.join();
    }
    if (advancedPipelineThread_.joinable()) {
        advancedPipelineThread_.join();
    }
    advancedPipelineStarted_.store(false, std::memory_order_release);
    mlExecutor_.store(nullptr, std::memory_order_release);
    publishedDoviPipeline_.store(nullptr, std::memory_order_release);
    windowsMlExecutor_.Reset();
    directMlExecutor_.Reset();
    frameGraph_.WaitForIdle(2000);
    ReleaseHlgPresentationResources();
    if (libplaceboBridge_) {
        libplaceboBridge_->Reset();
        libplaceboBridge_.reset();
    }
    libplaceboRenderFailureLogged_ = false;
    tensorPreviousFrame_.reset();
    tensorPreviousDoviTarget_.Reset();
    tensorPreviousDoviLuminanceTarget_.Reset();
    tensorPreviousDoviTargetTimeline_ = 0;
    tensorPreviousDoviTargetFrameSerial_ = 0;
    tensorPreviousDoviMetadataFingerprint_ = 0;
    tensorPreviousDoviTargetPeakNits_ = 0.0f;
    tensorPreviousDoviHdrOutput_ = false;
    displayDomainInterpolationLuminanceGuardLogged_ = false;
    loggedEndpointLuminanceReferenceWidth_ = 0;
    loggedEndpointLuminanceReferenceHeight_ = 0;
    sourceComputeCompletions_.clear();
    sourceGraphicsCompletions_.clear();
    tensorPreprocessor_.Reset();
    tensorDolbyVisionPipelineLogged_ = false;
    pendingGeneratedSlots_.clear();
    for (GeneratedFrameSlot& slot : generatedSlots_) slot = {};
    for (auto& backBufferCaches : overlayTextureCaches_) {
        for (OverlayTextureCache& cache : backBufferCaches) {
            if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
            cache = {};
        }
    }
    for (auto& backBufferCaches : subtitleTextureCaches_) {
        for (OverlayTextureCache& cache : backBufferCaches) {
            if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
            cache = {};
        }
    }
    textSubtitlePixels_.reset();
    textSubtitleText_.clear();
    textSubtitleSurfaceWidth_ = 0;
    textSubtitleSurfaceHeight_ = 0;
    presentedSubtitleText_.clear();
    presentedSubtitleBitmaps_.clear();
    presentedSubtitleSourceWidth_ = 0;
    presentedSubtitleSourceHeight_ = 0;
    independentSubtitleCompositionLogged_ = false;
    interpolationDisplayDomainLogged_ = false;
    independentUiOverlayCompositionLogged_ = false;
    perBackBufferSubtitleTextureLogged_ = false;
    for (PixelFrameUploadCache& cache : pixelFrameUploadCaches_) {
        if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
        cache = {};
    }
    for (auto& backBufferCaches : yuvFrameUploadCaches_) {
        for (YuvFrameUploadCache& cache : backBufferCaches) {
            if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
            cache = {};
        }
    }
    for (auto& frame : inFlightFrames_) frame.reset();
    for (auto& target : libplaceboTargets_) target.Reset();
    for (auto& buffer : backBuffers_) buffer.Reset();
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (doviConstantBuffers_[index] && doviConstantMappings_[index]) {
            doviConstantBuffers_[index]->Unmap(0, nullptr);
        }
        doviConstantMappings_[index] = nullptr;
        doviConstantBuffers_[index].Reset();
    }
    commandList_.Reset();
    for (auto& allocator : allocators_) allocator.Reset();
    yuvPipeline_.Reset();
    doviPipeline_.Reset();
    yuvVertexShader_.Reset();
    doviPixelShaderSource_.clear();
    rgbPipeline_.Reset();
    overlayPipeline_.Reset();
    tensorPipeline_.Reset();
    tensorRootSignature_.Reset();
    overlayRootSignature_.Reset();
    rootSignature_.Reset();
    srvHeap_.Reset();
    tensorSrvHeap_.Reset();
    overlaySrvHeap_.Reset();
    rtvHeap_.Reset();
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    swapChainWaitable_ = false;
    framePacingLogged_ = false;
    swapChain_.Reset();
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    useComposition_ = false;
    frameGraph_.Reset();
    copyQueue_.Reset();
    mlQueue_.Reset();
    computeQueue_.Reset();
    queue_.Reset();
    fence_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    mlInitializationStarted_.store(false, std::memory_order_release);
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

void D3D12VideoRenderer::NotifyInitialization(const VideoRendererState state) const {
    if (completionWindow_ && completionMessage_) {
        PostMessageW(completionWindow_, completionMessage_,
                     static_cast<WPARAM>(completionCookie_), static_cast<LPARAM>(state));
    }
}

bool D3D12VideoRenderer::DeviceLost(const HRESULT result, const wchar_t* operation) {
    if (!IsDeviceRemoved(result)) return false;
    if (deviceLossReported_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    HRESULT reason = result;
    if (device_) {
        const HRESULT removedReason = device_->GetDeviceRemovedReason();
        if (FAILED(removedReason)) reason = removedReason;
    }
    Log(LogLevel::Error,
        std::wstring(operation) + L" lost d3d12 device reason=0x" +
            [&reason]() {
                std::wostringstream stream;
                stream << std::hex << static_cast<unsigned long>(reason);
                return stream.str();
            }());
    {
        std::scoped_lock lock(commandMutex_);
        stopRequested_ = true;
    }
    commandCv_.notify_one();
    if (completionWindow_ && deviceLostMessage_) {
        PostMessageW(completionWindow_, deviceLostMessage_,
                     static_cast<WPARAM>(completionCookie_), static_cast<LPARAM>(reason));
    }
    return true;
}

void D3D12VideoRenderer::Log(const LogLevel level, const std::wstring& message) const {
    if (logSink_) logSink_->Write(level, L"d3d12_renderer", message);
}

}  // namespace anvil::app
