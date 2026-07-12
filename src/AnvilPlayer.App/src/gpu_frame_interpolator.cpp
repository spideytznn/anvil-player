#include "AnvilPlayer/App/gpu_frame_interpolator.h"

#include <d3d10.h>
#include <dml_provider_factory.h>
#include <dxgi1_2.h>
#include <onnxruntime_cxx_api.h>
#include <ppl.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anvil::app {
namespace {

using Microsoft::WRL::ComPtr;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;

constexpr std::size_t kMaximumSourcePixels = 4096ull * 2160ull;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

double MillisecondsSince(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

float FiniteClamp(const float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

std::wstring SanitizeError(const char* message) {
    if (!message || *message == '\0') {
        return L"unknown_error";
    }
    std::wstring result;
    result.reserve(160);
    for (const unsigned char value : std::string(message)) {
        if (result.size() >= 160) {
            break;
        }
        if ((value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9')) {
            result.push_back(static_cast<wchar_t>(value));
        } else if (result.empty() || result.back() != L'_') {
            result.push_back(L'_');
        }
    }
    return result.empty() ? L"unknown_error" : result;
}

std::optional<int> AdapterIndexForDevice(ID3D11Device* device) {
    if (!device) {
        return std::nullopt;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> activeAdapter;
    DXGI_ADAPTER_DESC activeDesc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&activeAdapter)) ||
        FAILED(activeAdapter->GetDesc(&activeDesc))) {
        return std::nullopt;
    }
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            return std::nullopt;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.AdapterLuid.HighPart == activeDesc.AdapterLuid.HighPart &&
            desc.AdapterLuid.LowPart == activeDesc.AdapterLuid.LowPart) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

struct YuvConversion {
    float redCr = 1.5748f;
    float greenCb = -0.187324f;
    float greenCr = -0.468124f;
    float blueCb = 1.8556f;
};

YuvConversion ConversionFor(const VideoMatrixCoefficients matrix) {
    switch (matrix) {
        case VideoMatrixCoefficients::Bt601:
            return {1.402f, -0.344136f, -0.714136f, 1.772f};
        case VideoMatrixCoefficients::Bt2020Ncl:
        case VideoMatrixCoefficients::Bt2020Cl:
            return {1.4746f, -0.164553f, -0.571353f, 1.8814f};
        case VideoMatrixCoefficients::Bt709:
        case VideoMatrixCoefficients::Unknown:
        case VideoMatrixCoefficients::Rgb:
        default:
            return {};
    }
}

class ScopedMultithreadLock {
public:
    explicit ScopedMultithreadLock(ID3D10Multithread* multithread)
        : multithread_(multithread) {
        if (multithread_) {
            multithread_->Enter();
        }
    }
    ~ScopedMultithreadLock() {
        if (multithread_) {
            multithread_->Leave();
        }
    }
    ScopedMultithreadLock(const ScopedMultithreadLock&) = delete;
    ScopedMultithreadLock& operator=(const ScopedMultithreadLock&) = delete;

private:
    ID3D10Multithread* multithread_ = nullptr;
};

std::pair<UINT, UINT> SelectProcessingSize(const UINT width, const UINT height) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    UINT divisor = 1;
    if (pixels > 1920ull * 1080ull) {
        divisor = 4;
    } else if (pixels > 1280ull * 720ull) {
        divisor = 2;
    }
    return {
        std::max<UINT>(1, width / divisor),
        std::max<UINT>(1, height / divisor),
    };
}

}  // namespace

struct GpuFrameInterpolator::Impl {
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR, "AnvilPlayer.FrameInterpolation"};
    std::unique_ptr<Ort::Session> session;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<ID3D11Texture2D> stagingTexture;
    D3D11_TEXTURE2D_DESC stagingSourceDesc{};
    GpuFrameInterpolationConfig config{};
    std::vector<float> input;
    ComPtr<ID3D11Texture2D> cachedNextTexture;
    UINT cachedNextArraySlice = 0;
    bool hasCachedNext = false;

    bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc) {
        if (stagingTexture &&
            stagingSourceDesc.Width == sourceDesc.Width &&
            stagingSourceDesc.Height == sourceDesc.Height &&
            stagingSourceDesc.Format == sourceDesc.Format) {
            return true;
        }
        stagingTexture.Reset();
        D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture)) ||
            !stagingTexture) {
            return false;
        }
        stagingSourceDesc = sourceDesc;
        return true;
    }

    bool ReadSurfaceToRgb(const GpuInterpolationInputSurface& surface,
                          const std::size_t firstChannel,
                          std::wstring& failureReason) {
        if (!surface.texture || !device || !context) {
            failureReason = L"missing_input_surface";
            return false;
        }
        D3D11_TEXTURE2D_DESC sourceDesc{};
        surface.texture->GetDesc(&sourceDesc);
        if ((sourceDesc.Format != DXGI_FORMAT_NV12 && sourceDesc.Format != DXGI_FORMAT_P010) ||
            surface.arraySlice >= sourceDesc.ArraySize) {
            failureReason = L"unsupported_input_surface";
            return false;
        }
        if (!EnsureStagingTexture(sourceDesc)) {
            failureReason = L"staging_texture_create_failed";
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        {
            ScopedMultithreadLock lock(multithread.Get());
            const UINT subresource =
                D3D11CalcSubresource(0, surface.arraySlice, sourceDesc.MipLevels);
            context->CopySubresourceRegion(
                stagingTexture.Get(), 0, 0, 0, 0, surface.texture, subresource, nullptr);
            const HRESULT mapHr =
                context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(mapHr) || !mapped.pData || mapped.RowPitch == 0) {
                failureReason = L"decode_surface_map_failed";
                return false;
            }
        }

        const int cropLeft = std::clamp(
            static_cast<int>(std::lround(surface.sourceUvRect.left * sourceDesc.Width)),
            0,
            static_cast<int>(sourceDesc.Width) - 1);
        const int cropTop = std::clamp(
            static_cast<int>(std::lround(surface.sourceUvRect.top * sourceDesc.Height)),
            0,
            static_cast<int>(sourceDesc.Height) - 1);
        const int cropRight = std::clamp(
            static_cast<int>(std::lround(surface.sourceUvRect.right * sourceDesc.Width)),
            cropLeft + 1,
            static_cast<int>(sourceDesc.Width));
        const int cropBottom = std::clamp(
            static_cast<int>(std::lround(surface.sourceUvRect.bottom * sourceDesc.Height)),
            cropTop + 1,
            static_cast<int>(sourceDesc.Height));
        const int cropWidth = cropRight - cropLeft;
        const int cropHeight = cropBottom - cropTop;
        const std::size_t pixelCount =
            static_cast<std::size_t>(config.width) * config.height;
        float* red = input.data() + firstChannel * pixelCount;
        float* green = red + pixelCount;
        float* blue = green + pixelCount;
        const auto* yPlane = static_cast<const std::uint8_t*>(mapped.pData);
        const auto* uvPlane = yPlane + static_cast<std::size_t>(mapped.RowPitch) * sourceDesc.Height;
        const YuvConversion conversion = ConversionFor(surface.color.matrix);
        const bool fullRange = surface.color.range == VideoColorRange::Full;

        concurrency::parallel_for<UINT>(0, config.height, [&](const UINT y) {
            const int sourceY = cropTop + std::min(
                cropHeight - 1,
                static_cast<int>((static_cast<std::uint64_t>(y) * cropHeight) / config.height));
            const auto* yRow = yPlane + static_cast<std::size_t>(mapped.RowPitch) * sourceY;
            const auto* uvRow = uvPlane + static_cast<std::size_t>(mapped.RowPitch) * (sourceY / 2);
            for (UINT x = 0; x < config.width; ++x) {
                const int sourceX = cropLeft + std::min(
                    cropWidth - 1,
                    static_cast<int>((static_cast<std::uint64_t>(x) * cropWidth) / config.width));
                float luma = 0.0f;
                float cb = 0.0f;
                float cr = 0.0f;
                if (sourceDesc.Format == DXGI_FORMAT_NV12) {
                    const int yCode = yRow[sourceX];
                    const int uvOffset = (sourceX / 2) * 2;
                    const int uCode = uvRow[uvOffset];
                    const int vCode = uvRow[uvOffset + 1];
                    luma = fullRange ? static_cast<float>(yCode) / 255.0f
                                     : (static_cast<float>(yCode) - 16.0f) / 219.0f;
                    cb = fullRange ? static_cast<float>(uCode) / 255.0f - 0.5f
                                   : (static_cast<float>(uCode) - 128.0f) / 224.0f;
                    cr = fullRange ? static_cast<float>(vCode) / 255.0f - 0.5f
                                   : (static_cast<float>(vCode) - 128.0f) / 224.0f;
                } else {
                    const auto* y16 = reinterpret_cast<const std::uint16_t*>(yRow);
                    const auto* uv16 = reinterpret_cast<const std::uint16_t*>(uvRow);
                    const int yCode = y16[sourceX] >> 6;
                    const int uvOffset = (sourceX / 2) * 2;
                    const int uCode = uv16[uvOffset] >> 6;
                    const int vCode = uv16[uvOffset + 1] >> 6;
                    luma = fullRange ? static_cast<float>(yCode) / 1023.0f
                                     : (static_cast<float>(yCode) - 64.0f) / 876.0f;
                    cb = fullRange ? static_cast<float>(uCode) / 1023.0f - 0.5f
                                   : (static_cast<float>(uCode) - 512.0f) / 896.0f;
                    cr = fullRange ? static_cast<float>(vCode) / 1023.0f - 0.5f
                                   : (static_cast<float>(vCode) - 512.0f) / 896.0f;
                }
                const std::size_t index = static_cast<std::size_t>(y) * config.width + x;
                red[index] = FiniteClamp(luma + conversion.redCr * cr);
                green[index] = FiniteClamp(
                    luma + conversion.greenCb * cb + conversion.greenCr * cr);
                blue[index] = FiniteClamp(luma + conversion.blueCb * cb);
            }
        });
        {
            ScopedMultithreadLock lock(multithread.Get());
            context->Unmap(stagingTexture.Get(), 0);
        }
        return true;
    }

    std::vector<Ort::Value> RunModel(double& inferenceMilliseconds) {
        const std::array<std::int64_t, 4> shape{
            1, 7, static_cast<std::int64_t>(config.height), static_cast<std::int64_t>(config.width)};
        Ort::MemoryInfo memoryInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputValue = Ort::Value::CreateTensor<float>(
            memoryInfo, input.data(), input.size(), shape.data(), shape.size());
        constexpr const char* inputNames[] = {"input"};
        constexpr const char* outputNames[] = {"output"};
        Ort::RunOptions runOptions;
        const auto start = std::chrono::steady_clock::now();
        auto outputs = session->Run(
            runOptions, inputNames, &inputValue, 1, outputNames, 1);
        inferenceMilliseconds = MillisecondsSince(start);
        return outputs;
    }

    void Reset() {
        session.reset();
        stagingTexture.Reset();
        cachedNextTexture.Reset();
        context.Reset();
        multithread.Reset();
        device.Reset();
        stagingSourceDesc = {};
        config = {};
        input.clear();
        input.shrink_to_fit();
        cachedNextArraySlice = 0;
        hasCachedNext = false;
    }
};

GpuFrameInterpolator::GpuFrameInterpolator() : impl_(std::make_unique<Impl>()) {}

GpuFrameInterpolator::~GpuFrameInterpolator() = default;

std::filesystem::path GpuFrameInterpolator::DefaultModelPath() {
    std::array<wchar_t, 32768> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size()) {
        return {};
    }
    return std::filesystem::path(executablePath.data())
               .parent_path() / L"assets" / L"models" / L"rife_v4.25_lite.onnx";
}

GpuFrameInterpolationCapabilities GpuFrameInterpolator::Initialize(
    ID3D11Device* device,
    const GpuFrameInterpolationConfig& config) {
    Reset();
    capabilities_.backend = GpuFrameInterpolationBackend::DirectMlRife;
    capabilities_.backendName = L"directml_rife_v4.25_lite_balanced";
    capabilities_.outputRateMultiplier = config.outputRateMultiplier;
    capabilities_.usesFutureFrames = true;

    const std::size_t sourcePixelCount =
        static_cast<std::size_t>(config.width) * config.height;
    if (!device) {
        capabilities_.unavailableReason = L"missing_d3d11_device";
        return capabilities_;
    }
    if (config.width == 0 || config.height == 0 ||
        config.sourceFrameRateNumerator == 0 || config.sourceFrameRateDenominator == 0 ||
        config.outputRateMultiplier != 2) {
        capabilities_.unavailableReason = L"unsupported_configuration";
        return capabilities_;
    }
    if (sourcePixelCount > kMaximumSourcePixels) {
        capabilities_.unavailableReason = L"resolution_exceeds_directml_budget";
        return capabilities_;
    }

    impl_->config = config;
    const auto [processingWidth, processingHeight] =
        SelectProcessingSize(config.width, config.height);
    impl_->config.width = processingWidth;
    impl_->config.height = processingHeight;
    if (impl_->config.modelPath.empty()) {
        impl_->config.modelPath = DefaultModelPath();
    }
    std::error_code modelPathError;
    if (impl_->config.modelPath.empty() ||
        !std::filesystem::is_regular_file(impl_->config.modelPath, modelPathError)) {
        capabilities_.unavailableReason = L"rife_model_missing";
        return capabilities_;
    }
    const auto adapterIndex = AdapterIndexForDevice(device);
    if (!adapterIndex.has_value()) {
        capabilities_.unavailableReason = L"display_adapter_not_found";
        return capabilities_;
    }

    try {
        Ort::SessionOptions options;
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, *adapterIndex));
        impl_->session = std::make_unique<Ort::Session>(
            impl_->environment, impl_->config.modelPath.c_str(), options);
        if (impl_->session->GetInputCount() != 1 || impl_->session->GetOutputCount() != 1) {
            capabilities_.unavailableReason = L"unexpected_rife_model_interface";
            impl_->Reset();
            return capabilities_;
        }
        const auto inputInfo = impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto outputInfo = impl_->session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto inputShape = inputInfo.GetShape();
        const auto outputShape = outputInfo.GetShape();
        const auto dimensionMatches = [](const std::int64_t value,
                                         const std::int64_t expected) {
            return value <= 0 || value == expected;
        };
        if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            inputShape.size() != 4 || !dimensionMatches(inputShape[1], 7) ||
            outputShape.size() != 4 || !dimensionMatches(outputShape[1], 3)) {
            capabilities_.unavailableReason = L"unexpected_rife_model_tensors";
            impl_->Reset();
            return capabilities_;
        }
        const std::size_t processingPixelCount =
            static_cast<std::size_t>(processingWidth) * processingHeight;
        impl_->input.assign(processingPixelCount * 7, 0.0f);
        std::fill(
            impl_->input.begin() + static_cast<std::ptrdiff_t>(processingPixelCount * 6),
            impl_->input.end(),
            0.5f);
        double discardedWarmup = 0.0;
        impl_->RunModel(discardedWarmup);
        impl_->RunModel(capabilities_.warmupMilliseconds);
    } catch (const Ort::Exception& error) {
        capabilities_.unavailableReason = L"directml_session_failed_" + SanitizeError(error.what());
        impl_->Reset();
        return capabilities_;
    } catch (const std::exception& error) {
        capabilities_.unavailableReason =
            L"interpolator_initialize_failed_" + SanitizeError(error.what());
        impl_->Reset();
        return capabilities_;
    }

    impl_->device = device;
    device->GetImmediateContext(&impl_->context);
    (void)device->QueryInterface(IID_PPV_ARGS(&impl_->multithread));
    if (!impl_->context) {
        capabilities_.unavailableReason = L"missing_d3d11_context";
        impl_->Reset();
        return capabilities_;
    }
    capabilities_.available = true;
    capabilities_.zeroCopy = false;
    capabilities_.processingWidth = processingWidth;
    capabilities_.processingHeight = processingHeight;
    capabilities_.unavailableReason.clear();
    return capabilities_;
}

bool GpuFrameInterpolator::InterpolateMidpoint(
    const GpuInterpolationInputSurface& previous,
    const GpuInterpolationInputSurface& next,
    GpuInterpolationOutputSurface& output) {
    output = {};
    lastFailureReason_.clear();
    if (!capabilities_.available || !impl_->session || !impl_->context) {
        lastFailureReason_ = L"interpolator_not_ready";
        return false;
    }

    const auto totalStart = std::chrono::steady_clock::now();
    const std::size_t pixelCount =
        static_cast<std::size_t>(impl_->config.width) * impl_->config.height;
    if (impl_->hasCachedNext &&
        impl_->cachedNextTexture.Get() == previous.texture &&
        impl_->cachedNextArraySlice == previous.arraySlice) {
        std::memmove(
            impl_->input.data(),
            impl_->input.data() + pixelCount * 3,
            pixelCount * 3 * sizeof(float));
    } else if (!impl_->ReadSurfaceToRgb(previous, 0, lastFailureReason_)) {
        return false;
    }
    if (!impl_->ReadSurfaceToRgb(next, 3, lastFailureReason_)) {
        return false;
    }
    std::fill(
        impl_->input.begin() + static_cast<std::ptrdiff_t>(pixelCount * 6),
        impl_->input.end(),
        0.5f);

    std::vector<Ort::Value> modelOutputs;
    try {
        modelOutputs = impl_->RunModel(output.diagnostics.inferenceMilliseconds);
    } catch (const Ort::Exception& error) {
        lastFailureReason_ = L"directml_inference_failed_" + SanitizeError(error.what());
        return false;
    } catch (const std::exception& error) {
        lastFailureReason_ = L"interpolation_failed_" + SanitizeError(error.what());
        return false;
    }
    if (modelOutputs.size() != 1 || !modelOutputs.front().IsTensor()) {
        lastFailureReason_ = L"invalid_rife_output";
        return false;
    }
    const auto outputShape = modelOutputs.front().GetTensorTypeAndShapeInfo().GetShape();
    if (outputShape.size() != 4 || outputShape[0] != 1 || outputShape[1] != 3 ||
        outputShape[2] != impl_->config.height || outputShape[3] != impl_->config.width) {
        lastFailureReason_ = L"unexpected_rife_output_shape";
        return false;
    }

    const float* modelOutput = modelOutputs.front().GetTensorData<float>();
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(pixelCount * 4);
    concurrency::parallel_for<UINT>(0, impl_->config.height, [&](const UINT y) {
        const std::size_t rowStart = static_cast<std::size_t>(y) * impl_->config.width;
        const std::size_t rowEnd = rowStart + impl_->config.width;
        for (std::size_t index = rowStart; index < rowEnd; ++index) {
            const float red = FiniteClamp(modelOutput[index]);
            const float green = FiniteClamp(modelOutput[pixelCount + index]);
            const float blue = FiniteClamp(modelOutput[pixelCount * 2 + index]);
            (*pixels)[index * 4] =
                static_cast<std::uint8_t>(std::lround(blue * 255.0f));
            (*pixels)[index * 4 + 1] =
                static_cast<std::uint8_t>(std::lround(green * 255.0f));
            (*pixels)[index * 4 + 2] =
                static_cast<std::uint8_t>(std::lround(red * 255.0f));
            (*pixels)[index * 4 + 3] = 255;
        }
    });

    const std::size_t sampleStep = std::max<std::size_t>(1, pixelCount / 4096);
    double sourceDifference = 0.0;
    double previousDifference = 0.0;
    double nextDifference = 0.0;
    std::size_t sampleCount = 0;
    std::uint64_t fingerprint = kFnvOffset;
    for (std::size_t index = 0; index < pixelCount; index += sampleStep) {
        const float red = FiniteClamp(modelOutput[index]);
        const float green = FiniteClamp(modelOutput[pixelCount + index]);
        const float blue = FiniteClamp(modelOutput[pixelCount * 2 + index]);
        const float previousRed = impl_->input[index];
        const float previousGreen = impl_->input[pixelCount + index];
        const float previousBlue = impl_->input[pixelCount * 2 + index];
        const float nextRed = impl_->input[pixelCount * 3 + index];
        const float nextGreen = impl_->input[pixelCount * 4 + index];
        const float nextBlue = impl_->input[pixelCount * 5 + index];
        sourceDifference +=
            (std::abs(previousRed - nextRed) +
             std::abs(previousGreen - nextGreen) +
             std::abs(previousBlue - nextBlue)) /
            3.0;
        previousDifference +=
            (std::abs(red - previousRed) +
             std::abs(green - previousGreen) +
             std::abs(blue - previousBlue)) /
            3.0;
        nextDifference +=
            (std::abs(red - nextRed) +
             std::abs(green - nextGreen) +
             std::abs(blue - nextBlue)) /
            3.0;
        for (const std::size_t channelOffset : {2ull, 1ull, 0ull}) {
            fingerprint ^= (*pixels)[index * 4 + channelOffset];
            fingerprint *= kFnvPrime;
        }
        ++sampleCount;
    }

    const double divisor = static_cast<double>(std::max<std::size_t>(1, sampleCount));
    output.width = static_cast<int>(impl_->config.width);
    output.height = static_cast<int>(impl_->config.height);
    output.stride = output.width * 4;
    output.bgra = std::move(pixels);
    output.diagnostics.sourceDifference = sourceDifference / divisor;
    output.diagnostics.midpointToPreviousDifference = previousDifference / divisor;
    output.diagnostics.midpointToNextDifference = nextDifference / divisor;
    output.diagnostics.outputFingerprint = fingerprint;
    output.diagnostics.totalMilliseconds = MillisecondsSince(totalStart);
    impl_->cachedNextTexture = next.texture;
    impl_->cachedNextArraySlice = next.arraySlice;
    impl_->hasCachedNext = true;
    return true;
}

void GpuFrameInterpolator::Reset() {
    if (impl_) {
        impl_->Reset();
    }
    capabilities_ = {};
    lastFailureReason_.clear();
}

}  // namespace anvil::app
