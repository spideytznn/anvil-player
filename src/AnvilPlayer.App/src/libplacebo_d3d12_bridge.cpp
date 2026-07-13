#include "AnvilPlayer/App/libplacebo_d3d12_bridge.h"

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/string_util.h"

#include <d3d11.h>
#include <d3d11on12.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/renderer.h>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace anvil::app {
namespace {

using anvil::playback::DolbyVisionFrameMetadata;
using anvil::playback::DoviMappingMethod;
using anvil::playback::DoviNlqMethod;
using anvil::playback::LogLevel;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kEnableVariable[] = L"ANVIL_DOVI_LIBPLACEBO_D3D12";
constexpr wchar_t kDllVariable[] = L"ANVIL_LIBPLACEBO_DLL";

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required <= 1) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

bool IsTruthy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value == L"1" || value == L"true" || value == L"on" || value == L"yes";
}

std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

LogLevel MapLogLevel(const pl_log_level level) {
    switch (level) {
    case PL_LOG_FATAL:
    case PL_LOG_ERR:
        return LogLevel::Error;
    case PL_LOG_WARN:
        return LogLevel::Warning;
    case PL_LOG_INFO:
        return LogLevel::Info;
    default:
        return LogLevel::Debug;
    }
}

const DolbyVisionFrameMetadata* RenderMetadata(const NativeVideoFrame& frame) noexcept {
    if (frame.enhancementDovi && frame.enhancementDovi->valid) {
        return frame.enhancementDovi.get();
    }
    return frame.dovi && frame.dovi->valid ? frame.dovi.get() : nullptr;
}

bool IsTrivialNlq(const DolbyVisionFrameMetadata& metadata) noexcept {
    if (metadata.coefLog2Denom < 0 || metadata.coefLog2Denom > 62) return true;
    const std::uint64_t unity = std::uint64_t{1} << metadata.coefLog2Denom;
    for (int component = 0; component < anvil::playback::kDoviNumComponents; ++component) {
        if (metadata.nlqOffset[component] != 0 ||
            metadata.nlqVdrInMax[component] != unity ||
            metadata.nlqLinearDeadzoneSlope[component] != 0 ||
            metadata.nlqLinearDeadzoneThreshold[component] != 0) {
            return false;
        }
    }
    return true;
}

void MapDolbyVisionMetadata(const DolbyVisionFrameMetadata& source,
                            const bool hasEnhancementLayer,
                            pl_dovi_metadata& target) noexcept {
    target = {};
    std::copy_n(source.yccOffset, 3, target.nonlinear_offset);
    std::copy_n(source.yccToRgb, 9, &target.nonlinear.m[0][0]);
    std::copy_n(source.rgbToLms, 9, &target.linear.m[0][0]);

    for (int component = 0; component < anvil::playback::kDoviNumComponents; ++component) {
        const auto& input = source.curves[component];
        auto& output = target.comp[component];
        output.num_pivots = static_cast<std::uint8_t>(
            std::clamp(input.numPivots, 0, anvil::playback::kDoviMaxPivots));
        std::copy_n(input.pivots, output.num_pivots, output.pivots);
        const int pieces = std::max(0, static_cast<int>(output.num_pivots) - 1);
        for (int piece = 0; piece < pieces; ++piece) {
            const auto& inputPiece = input.pieces[piece];
            output.method[piece] = inputPiece.method == DoviMappingMethod::Mmr ? 1 : 0;
            if (inputPiece.method == DoviMappingMethod::Polynomial) {
                std::copy_n(inputPiece.polyCoef, 3, output.poly_coeffs[piece]);
            } else if (inputPiece.method == DoviMappingMethod::Mmr) {
                output.mmr_order[piece] = static_cast<std::uint8_t>(
                    std::clamp(inputPiece.mmrOrder, 0, anvil::playback::kDoviMmrMaxTerms));
                output.mmr_constant[piece] = inputPiece.mmrConstant;
                for (int order = 0; order < output.mmr_order[piece]; ++order) {
                    std::copy_n(inputPiece.mmrCoef[order],
                                anvil::playback::kDoviMmrCoeffsPerOrder,
                                output.mmr_coeffs[piece][order]);
                }
            }
        }
    }

    target.nlq_active = hasEnhancementLayer && !source.residualDisabled &&
        source.nlqMethod == DoviNlqMethod::LinearDeadzone && !IsTrivialNlq(source) &&
        source.coefLog2Denom >= 0 && source.coefLog2Denom <= 62 &&
        source.elBitDepth > 0 && source.elBitDepth < 31;
    if (!target.nlq_active) return;

    const double coefficientScale =
        1.0 / static_cast<double>(std::uint64_t{1} << source.coefLog2Denom);
    const double maximumElCode =
        static_cast<double>((std::uint64_t{1} << source.elBitDepth) - 1);
    for (int component = 0; component < anvil::playback::kDoviNumComponents; ++component) {
        const double slope = static_cast<double>(source.nlqLinearDeadzoneSlope[component]);
        const double threshold =
            static_cast<double>(source.nlqLinearDeadzoneThreshold[component]);
        target.nlq[component].offset = static_cast<float>(
            static_cast<double>(source.nlqOffset[component]) / maximumElCode);
        target.nlq[component].deadzone_slope = static_cast<float>(
            maximumElCode * coefficientScale * slope);
        target.nlq[component].deadzone_threshold = static_cast<float>(
            coefficientScale * (threshold - 0.5 * slope));
    }
}

void SetPlaneComponents(pl_plane& plane, const std::initializer_list<int> components) {
    plane.components = static_cast<int>(components.size());
    std::fill(std::begin(plane.component_mapping), std::end(plane.component_mapping),
              PL_CHANNEL_NONE);
    std::copy(components.begin(), components.end(), plane.component_mapping);
}

pl_rect2df VideoViewport(const int videoWidth,
                         const int videoHeight,
                         const UINT outputWidth,
                         const UINT outputHeight) noexcept {
    const float sourceAspect = static_cast<float>(std::max(1, videoWidth)) /
        static_cast<float>(std::max(1, videoHeight));
    const float outputAspect = static_cast<float>(std::max<UINT>(1, outputWidth)) /
        static_cast<float>(std::max<UINT>(1, outputHeight));
    pl_rect2df viewport{};
    if (sourceAspect > outputAspect) {
        const float height = static_cast<float>(outputWidth) / sourceAspect;
        viewport.x1 = static_cast<float>(outputWidth);
        viewport.y0 = (static_cast<float>(outputHeight) - height) * 0.5f;
        viewport.y1 = viewport.y0 + height;
    } else {
        const float width = static_cast<float>(outputHeight) * sourceAspect;
        viewport.x0 = (static_cast<float>(outputWidth) - width) * 0.5f;
        viewport.x1 = viewport.x0 + width;
        viewport.y1 = static_cast<float>(outputHeight);
    }
    return viewport;
}

}  // namespace

struct LibplaceboD3D12Bridge::Impl {
    struct Api {
        HMODULE module = nullptr;
        decltype(&pl_version) version = nullptr;
        decltype(&pl_log_create) logCreate = nullptr;
        decltype(&pl_log_destroy) logDestroy = nullptr;
        decltype(&pl_d3d11_create) d3d11Create = nullptr;
        decltype(&pl_d3d11_destroy) d3d11Destroy = nullptr;
        decltype(&pl_d3d11_wrap) d3d11Wrap = nullptr;
        decltype(&pl_renderer_create) rendererCreate = nullptr;
        decltype(&pl_renderer_destroy) rendererDestroy = nullptr;
        decltype(&pl_render_image) renderImage = nullptr;
        decltype(&pl_tex_destroy) texDestroy = nullptr;
        decltype(&pl_gpu_flush) gpuFlush = nullptr;
        const pl_render_params* defaultRenderParams = nullptr;

        template <typename T>
        bool Load(T& output, const char* name) const noexcept {
            output = reinterpret_cast<T>(GetProcAddress(module, name));
            return output != nullptr;
        }

        bool Open(const std::filesystem::path& path) {
            module = LoadLibraryExW(path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) return false;
            constexpr char logCreateName[] =
                "pl_log_create_" PL_TOSTRING(PL_API_VER);
            return Load(version, "pl_version") &&
                Load(logCreate, logCreateName) &&
                Load(logDestroy, "pl_log_destroy") &&
                Load(d3d11Create, "pl_d3d11_create") &&
                Load(d3d11Destroy, "pl_d3d11_destroy") &&
                Load(d3d11Wrap, "pl_d3d11_wrap") &&
                Load(rendererCreate, "pl_renderer_create") &&
                Load(rendererDestroy, "pl_renderer_destroy") &&
                Load(renderImage, "pl_render_image") &&
                Load(texDestroy, "pl_tex_destroy") &&
                Load(gpuFlush, "pl_gpu_flush") &&
                Load(defaultRenderParams, "pl_render_default_params");
        }

        void Close() noexcept {
            *this = {};
        }
    } api;

    struct WrappedFrame {
        ComPtr<ID3D11Resource> resource;
        std::array<pl_tex, 2> textures{};
        pl_frame description{};
    };

    explicit Impl(LogSinkPtr sink) : logSink(std::move(sink)) {}

    static void LogCallback(void* privateData, const pl_log_level level, const char* message) {
        auto* self = static_cast<Impl*>(privateData);
        if (!self || !self->logSink || !message) return;
        self->logSink->Write(MapLogLevel(level), L"libplacebo_d3d12", Utf8ToWide(message));
    }

    void Log(const LogLevel level, const std::wstring& message) const {
        if (logSink) logSink->Write(level, L"libplacebo_d3d12", message);
    }

    bool LoadApi() {
        std::vector<std::filesystem::path> candidates;
        const std::wstring configured = EnvironmentValue(kDllVariable);
        if (!configured.empty()) candidates.emplace_back(configured);
        const std::filesystem::path executableDirectory = ExecutableDirectory();
        if (!executableDirectory.empty()) {
            candidates.push_back(executableDirectory / L"libplacebo-371.dll");
            candidates.push_back(executableDirectory / L"libplacebo.dll");
        }
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (!std::filesystem::exists(candidate, error) || error) continue;
            if (api.Open(candidate)) {
                loadedPath = candidate;
                return true;
            }
            if (api.module) FreeLibrary(api.module);
            api.Close();
        }
        Log(LogLevel::Warning,
            L"requested backend unavailable: libplacebo 7.371 DLL was not found or has an incompatible API");
        return false;
    }

    bool WrapVideoFrame(const NativeVideoFrame& input, WrappedFrame& output) {
        if (!input.HasD3D12Texture() ||
            (input.d3dFormat != DXGI_FORMAT_NV12 && input.d3dFormat != DXGI_FORMAT_P010)) {
            return false;
        }
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(on12Device->CreateWrappedResource(
                input.d3d12Texture.Get(), &flags, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&output.resource)))) {
            return false;
        }

        const D3D12_RESOURCE_DESC resource = input.d3d12Texture->GetDesc();
        const int width = static_cast<int>(std::min<UINT64>(
            resource.Width, static_cast<UINT64>(std::numeric_limits<int>::max())));
        const int height = static_cast<int>(resource.Height);
        const int arraySlice = static_cast<int>(std::min<UINT>(
            input.d3d12Subresource, std::max<UINT>(1, resource.DepthOrArraySize) - 1));
        const bool tenBit = input.d3dFormat == DXGI_FORMAT_P010;
        const std::array<DXGI_FORMAT, 2> formats{
            tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
            tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM};
        const std::array<int, 2> widths{width, (width + 1) / 2};
        const std::array<int, 2> heights{height, (height + 1) / 2};
        for (int plane = 0; plane < 2; ++plane) {
            pl_d3d11_wrap_params parameters{};
            parameters.tex = output.resource.Get();
            parameters.array_slice = arraySlice;
            parameters.fmt = formats[plane];
            parameters.w = widths[plane];
            parameters.h = heights[plane];
            output.textures[plane] = api.d3d11Wrap(d3d11->gpu, &parameters);
            if (!output.textures[plane]) return false;
        }

        output.description.num_planes = 2;
        output.description.planes[0].texture = output.textures[0];
        SetPlaneComponents(output.description.planes[0], {PL_CHANNEL_Y});
        output.description.planes[1].texture = output.textures[1];
        SetPlaneComponents(output.description.planes[1], {PL_CHANNEL_CB, PL_CHANNEL_CR});
        output.description.planes[1].shift_x = -0.5f;
        output.description.repr.levels = PL_COLOR_LEVELS_LIMITED;
        output.description.repr.alpha = PL_ALPHA_NONE;
        output.description.repr.bits.sample_depth = tenBit ? 16 : 8;
        output.description.repr.bits.color_depth = tenBit ? 10 : 8;
        output.description.repr.bits.bit_shift = tenBit ? 6 : 0;
        output.description.crop = {
            input.sourceUvRect.left * width,
            input.sourceUvRect.top * height,
            input.sourceUvRect.right * width,
            input.sourceUvRect.bottom * height};
        return true;
    }

    void DestroyWrappedFrame(WrappedFrame& frame) noexcept {
        if (d3d11) {
            for (auto& texture : frame.textures) {
                if (texture) api.texDestroy(d3d11->gpu, &texture);
            }
        }
        frame = {};
    }

    bool CreateTarget(ID3D12Resource* target,
                      const UINT width,
                      const UINT height,
                      const bool hdrOutput,
                      const float targetPeakNits,
                      WrappedFrame& output) {
        if (!target || target->GetDesc().Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET |
                          D3D11_BIND_SHADER_RESOURCE |
                          D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(on12Device->CreateWrappedResource(
                target, &flags, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                IID_PPV_ARGS(&output.resource)))) {
            return false;
        }
        pl_d3d11_wrap_params parameters{};
        parameters.tex = output.resource.Get();
        output.textures[0] = api.d3d11Wrap(d3d11->gpu, &parameters);
        if (!output.textures[0]) return false;

        output.description.num_planes = 1;
        output.description.planes[0].texture = output.textures[0];
        SetPlaneComponents(output.description.planes[0],
                           {PL_CHANNEL_R, PL_CHANNEL_G, PL_CHANNEL_B, PL_CHANNEL_A});
        output.description.repr.sys = PL_COLOR_SYSTEM_RGB;
        output.description.repr.levels = PL_COLOR_LEVELS_FULL;
        output.description.repr.alpha = PL_ALPHA_NONE;
        output.description.color.primaries = PL_COLOR_PRIM_BT_709;
        output.description.color.transfer = PL_COLOR_TRC_SCRGB;
        output.description.color.hdr.min_luma = PL_COLOR_HDR_BLACK;
        output.description.color.hdr.max_luma = hdrOutput
            ? std::max(100.0f, targetPeakNits)
            : 100.0f;
        output.description.crop = {0.0f, 0.0f, static_cast<float>(width),
                                   static_cast<float>(height)};
        return true;
    }

    LogSinkPtr logSink;
    std::filesystem::path loadedPath;
    std::wstring version;
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    ComPtr<ID3D11On12Device> on12Device;
    ComPtr<ID3D12CommandQueue> graphicsQueue;
    pl_log log = nullptr;
    pl_d3d11 d3d11 = nullptr;
    pl_renderer renderer = nullptr;
    bool firstRenderLogged = false;
};

LibplaceboD3D12Bridge::LibplaceboD3D12Bridge(LogSinkPtr logSink)
    : impl_(std::make_unique<Impl>(std::move(logSink))) {}

LibplaceboD3D12Bridge::~LibplaceboD3D12Bridge() {
    Reset();
}

bool LibplaceboD3D12Bridge::RequestedByEnvironment() noexcept {
    const std::wstring configured = EnvironmentValue(kEnableVariable);
    // libplacebo is the authoritative Dolby Vision renderer. It may be disabled
    // explicitly for diagnostics, but ordinary playback must not silently fall
    // back to a separately implemented color pipeline.
    return configured.empty() || IsTruthy(configured);
}

bool LibplaceboD3D12Bridge::Initialize(ID3D12Device* device,
                                       ID3D12CommandQueue* graphicsQueue) {
    Reset();
    if (!device || !graphicsQueue || !RequestedByEnvironment() || !impl_->LoadApi()) return false;

    IUnknown* queues[]{graphicsQueue};
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11On12CreateDevice(
            device, flags, nullptr, 0, queues, 1, 0, &impl_->d3d11Device,
            &impl_->d3d11Context, nullptr)) ||
        FAILED(impl_->d3d11Device.As(&impl_->on12Device))) {
        impl_->Log(LogLevel::Warning, L"D3D11On12 device creation failed; native D3D12 fallback remains active");
        Reset();
        return false;
    }
    impl_->graphicsQueue = graphicsQueue;

    pl_log_params logParameters{};
    logParameters.log_cb = &Impl::LogCallback;
    logParameters.log_priv = impl_.get();
    logParameters.log_level = PL_LOG_INFO;
    impl_->log = impl_->api.logCreate(PL_API_VER, &logParameters);

    pl_d3d11_params deviceParameters{};
    deviceParameters.device = impl_->d3d11Device.Get();
    impl_->d3d11 = impl_->api.d3d11Create(impl_->log, &deviceParameters);
    if (!impl_->d3d11 || !impl_->d3d11->gpu) {
        impl_->Log(LogLevel::Warning, L"libplacebo rejected the D3D11On12 device");
        Reset();
        return false;
    }
    impl_->renderer = impl_->api.rendererCreate(impl_->log, impl_->d3d11->gpu);
    if (!impl_->renderer) {
        impl_->Log(LogLevel::Warning, L"libplacebo renderer creation failed");
        Reset();
        return false;
    }
    if (const char* value = impl_->api.version()) impl_->version = Utf8ToWide(value);
    impl_->Log(LogLevel::Info,
               L"backend ready version=" + impl_->version +
                   L" interop=D3D11On12 resources=GPU-resident");
    return true;
}

void LibplaceboD3D12Bridge::Reset() noexcept {
    if (!impl_) return;
    if (impl_->d3d11Context) {
        impl_->d3d11Context->ClearState();
        impl_->d3d11Context->Flush();
    }
    if (impl_->renderer && impl_->api.rendererDestroy) {
        impl_->api.rendererDestroy(&impl_->renderer);
    }
    if (impl_->d3d11 && impl_->api.d3d11Destroy) {
        impl_->api.d3d11Destroy(&impl_->d3d11);
    }
    if (impl_->log && impl_->api.logDestroy) impl_->api.logDestroy(&impl_->log);
    impl_->on12Device.Reset();
    impl_->d3d11Context.Reset();
    impl_->d3d11Device.Reset();
    impl_->graphicsQueue.Reset();
    if (impl_->api.module) FreeLibrary(impl_->api.module);
    impl_->api.Close();
    impl_->loadedPath.clear();
    impl_->version.clear();
    impl_->firstRenderLogged = false;
}

bool LibplaceboD3D12Bridge::IsReady() const noexcept {
    return impl_ && impl_->renderer && impl_->d3d11 && impl_->on12Device &&
        impl_->graphicsQueue;
}

std::wstring LibplaceboD3D12Bridge::Version() const {
    return impl_ ? impl_->version : std::wstring{};
}

bool LibplaceboD3D12Bridge::RenderDolbyVision(
    const NativeVideoFrame& frame,
    ID3D12Resource* target,
    const UINT outputWidth,
    const UINT outputHeight,
    const bool hdrOutput,
    const float targetPeakNits,
    ID3D12Fence* supplementalFence,
    const std::uint64_t supplementalFenceValue,
    const bool stretchToOutput) {
    if (!IsReady() || outputWidth == 0 || outputHeight == 0) return false;
    const DolbyVisionFrameMetadata* metadata = RenderMetadata(frame);
    if (!metadata) return false;

    Impl::WrappedFrame source;
    Impl::WrappedFrame enhancement;
    Impl::WrappedFrame destination;
    const bool hasEnhancement = metadata->profile == 7 && frame.HasEnhancementD3D12Texture();
    if (!impl_->WrapVideoFrame(frame, source) ||
        (hasEnhancement && !impl_->WrapVideoFrame(*frame.enhancementFrame, enhancement)) ||
        !impl_->CreateTarget(target, outputWidth, outputHeight, hdrOutput,
                            targetPeakNits, destination)) {
        impl_->DestroyWrappedFrame(destination);
        impl_->DestroyWrappedFrame(enhancement);
        impl_->DestroyWrappedFrame(source);
        return false;
    }

    pl_dovi_metadata dovi{};
    MapDolbyVisionMetadata(*metadata, hasEnhancement, dovi);
    source.description.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
    source.description.repr.levels = metadata->blVideoFullRange
        ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    source.description.repr.dovi = &dovi;
    source.description.color.primaries = PL_COLOR_PRIM_BT_2020;
    source.description.color.transfer = PL_COLOR_TRC_PQ;
    source.description.color.hdr.min_luma = std::max(PL_COLOR_HDR_BLACK,
                                                     metadata->sourceMinNits);
    source.description.color.hdr.max_luma = metadata->sourceMaxNits > 0.0f
        ? metadata->sourceMaxNits : 1000.0f;
    if (metadata->dmLevel1Present) {
        source.description.color.hdr.max_pq_y = metadata->dmLevel1MaxPq / 4095.0f;
        source.description.color.hdr.avg_pq_y = metadata->dmLevel1AvgPq / 4095.0f;
    }
    source.description.enhancement_layer = dovi.nlq_active
        ? &enhancement.description : nullptr;
    destination.description.crop = stretchToOutput
        ? pl_rect2df{0.0f, 0.0f, static_cast<float>(outputWidth),
                    static_cast<float>(outputHeight)}
        : VideoViewport(frame.width, frame.height, outputWidth, outputHeight);

    const std::array<std::pair<ID3D12Fence*, std::uint64_t>, 3> dependencies{{
        {frame.d3d12ReadyFence.Get(), frame.d3d12ReadyFenceValue},
        {hasEnhancement ? frame.enhancementFrame->d3d12ReadyFence.Get() : nullptr,
         hasEnhancement ? frame.enhancementFrame->d3d12ReadyFenceValue : 0},
        {supplementalFence, supplementalFenceValue}}};
    for (const auto& [fence, value] : dependencies) {
        if (fence && value != 0 && FAILED(impl_->graphicsQueue->Wait(fence, value))) {
            impl_->DestroyWrappedFrame(destination);
            impl_->DestroyWrappedFrame(enhancement);
            impl_->DestroyWrappedFrame(source);
            return false;
        }
    }

    std::array<ID3D11Resource*, 3> acquired{
        source.resource.Get(), hasEnhancement ? enhancement.resource.Get() : nullptr,
        destination.resource.Get()};
    ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(impl_->d3d11Device->CreateRenderTargetView(
            destination.resource.Get(), nullptr, &targetView))) {
        impl_->DestroyWrappedFrame(destination);
        impl_->DestroyWrappedFrame(enhancement);
        impl_->DestroyWrappedFrame(source);
        return false;
    }
    UINT acquiredCount = 0;
    std::array<ID3D11Resource*, 3> compact{};
    for (ID3D11Resource* resource : acquired) {
        if (resource) compact[acquiredCount++] = resource;
    }
    impl_->on12Device->AcquireWrappedResources(compact.data(), acquiredCount);
    constexpr float clearColor[4]{};
    impl_->d3d11Context->ClearRenderTargetView(targetView.Get(), clearColor);
    const bool rendered = impl_->api.renderImage(
        impl_->renderer, &source.description, &destination.description,
        impl_->api.defaultRenderParams);
    impl_->api.gpuFlush(impl_->d3d11->gpu);
    impl_->on12Device->ReleaseWrappedResources(compact.data(), acquiredCount);
    impl_->d3d11Context->Flush();

    impl_->DestroyWrappedFrame(destination);
    impl_->DestroyWrappedFrame(enhancement);
    impl_->DestroyWrappedFrame(source);
    if (rendered && !impl_->firstRenderLogged) {
        impl_->firstRenderLogged = true;
        impl_->Log(LogLevel::Info,
                   L"Dolby Vision render active profile=" +
                       std::to_wstring(metadata->profile) +
                       (dovi.nlq_active ? L" path=BL+EL/FEL+NLQ" : L" path=BL+RPU") +
                       L" target=scRGB");
    }
    return rendered;
}

}  // namespace anvil::app
