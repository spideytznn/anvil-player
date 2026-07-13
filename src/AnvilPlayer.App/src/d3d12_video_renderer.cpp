#include "AnvilPlayer/App/d3d12_video_renderer.h"

#include "AnvilPlayer/App/string_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace anvil::app {
namespace {

using anvil::playback::HdrOutputMode;
using anvil::playback::LogLevel;
using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

constexpr DXGI_FORMAT kCompositionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

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
};

struct TensorCompositionConstants {
    UINT tensorWidth = 0;
    UINT tensorHeight = 0;
    float hdrOutput = 0.0f;
    float sourcePeakNits = 1000.0f;
    float targetPeakNits = 100.0f;
    float padding[3]{};
    float trimA[4]{};
    float trimB[4]{};
    float trimC[4]{};
};

static_assert(sizeof(TensorCompositionConstants) == 20 * sizeof(UINT));

static_assert(sizeof(CompositionConstants) == 16 * sizeof(UINT));

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

float SourcePeakNits(const anvil::playback::VideoColorMetadata& color) {
    if (color.masteringDisplay.hasLuminance && color.masteringDisplay.maxLuminanceNits > 0.0) {
        return static_cast<float>(std::clamp(color.masteringDisplay.maxLuminanceNits, 100.0, 10000.0));
    }
    if (color.contentLight.hasValues && color.contentLight.maxContentLightLevelNits > 0) {
        return static_cast<float>(std::clamp(color.contentLight.maxContentLightLevelNits, 100, 10000));
    }
    return color.IsHdr() ? 1000.0f : 100.0f;
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
        const bool interpolationChanged =
            videoSettings_.frameInterpolationEnabled != settings.frameInterpolationEnabled;
        videoSettings_ = settings;
        displayCapabilities_ = display;
        mediaColor_ = mediaColor;
        interpolationRequested_.store(settings.frameInterpolationEnabled,
                                      std::memory_order_release);
        pendingMlInitialization_ = pendingMlInitialization_ ||
            (settings.frameInterpolationEnabled &&
             !mlInitializationStarted_.load(std::memory_order_acquire));
        pendingInterpolationReset_ = pendingInterpolationReset_ || interpolationChanged;
        pendingPresent_ = true;
    }
    commandCv_.notify_one();
}

void D3D12VideoRenderer::ConfigureSubtitleSettings(
    const anvil::playback::SubtitleSettings& settings) {
    std::scoped_lock lock(commandMutex_);
    subtitleSettings_ = settings;
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

void D3D12VideoRenderer::Render(const NativeVideoFrame& frame) {
    if (!IsReady() || !frame.HasD3D12Texture()) {
        return;
    }
    try {
        auto pending = std::make_unique<NativeVideoFrame>(frame);
        {
            std::scoped_lock lock(commandMutex_);
            pendingFrame_ = std::move(pending);
        }
        commandCv_.notify_one();
    } catch (...) {
        Log(LogLevel::Warning, L"d3d12 frame mailbox allocation failed");
    }
}

void D3D12VideoRenderer::QueueFrameGraphInput(const NativeVideoFrame& frame) {
    if (!IsReady() || !interpolationRequested_.load(std::memory_order_acquire) ||
        !frame.HasD3D12Texture()) return;
    try {
        auto input = std::make_unique<NativeVideoFrame>(frame);
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
        }

        if (resetInterpolation) {
            ResetInterpolationState();
        }
        if (initializeMl) {
            StartMlInitialization();
        }
        if (resize) {
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
        ProcessReadyGeneratedFrame();
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
        L"d3d12 resident pipeline ready queues=graphics+preprocess+ml+copy "
        L"output=scRGB ml=lazy_background dovi_shaders=background_precompile");
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
    srvDesc.NumDescriptors = kBufferCount;
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
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), host_.load(), &desc, nullptr,
                                                nullptr, &swapChain1)) ||
        FAILED(swapChain1.As(&swapChain_))) {
        return false;
    }
    factory_->MakeWindowAssociation(host_.load(), DXGI_MWA_NO_ALT_ENTER);
    UINT colorSpaceSupport = 0;
    if (SUCCEEDED(swapChain_->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &colorSpaceSupport)) &&
        (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) {
        swapChain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffers_[index])))) return false;
        device_->CreateRenderTargetView(backBuffers_[index].Get(), nullptr, rtv);
        rtv.ptr += rtvIncrement_;
    }
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
    parameters[1].Constants.Num32BitValues = 16;
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
float3 hlg_to_nits(float3 v) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 scene = lerp((exp((v - c) / a) + b) / 12.0, v * v / 3.0, step(v, 0.5));
    return pow(max(scene, 0.0), 1.2) * max(tone.z, 1000.0);
}
float3 rec2020_to_scrgb(float3 v) {
    return mul(float3x3(1.6605, -0.5876, -0.0728,
                       -0.1246, 1.1329, -0.0083,
                       -0.0182, -0.1006, 1.1187), v);
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
    if (doviComposerScale[0].w<0.5) return enhancementUv.SampleLevel(linearClamp,float3(uv,0),0);
    uint w,h,layers,levels; enhancementUv.GetDimensions(0,w,h,layers,levels);
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
    float2 chroma=sourceUv.Sample(linearClamp,float3(sampleUv,0.0));
    float3 encoded;
    bool canonical2020=(modes.w&2)!=0;
    if (doviSignalMeta[0].x>1.5) {
        float2 elUv=lerp(enhancementRect.xy,enhancementRect.zw,displayUv);
        float3 composed=dovi_compose_p7_fel(0,float3(y,chroma),
            float3(dovi_chroma_site_luma(sampleUv),chroma),float3(dovi_sample_el_y(elUv),dovi_sample_el_uv(elUv)));
        encoded=yuv_to_rgb(composed.x,composed.yz,true); canonical2020=true;
    } else if (doviSignalMeta[0].x>0.5) {
        encoded=dovi_decode_single_layer(0,saturate(float3(y,chroma)*doviSignalMeta[0].w)); canonical2020=true;
    } else encoded=yuv_to_rgb(y,chroma,false);
    float3 linearNits;
    if (doviSignalMeta[0].x>0.5 || modes.z == 2) linearNits = pq_to_nits(encoded);
    else if (modes.z == 3) linearNits = hlg_to_nits(encoded);
    else linearNits = pow(saturate(encoded), 2.2) * 80.0;
    if (doviSignalMeta[0].x>0.5) linearNits=dovi_apply_display_trim(0,linearNits,tone.z);
    if (tone.x < 0.5 && (doviSignalMeta[0].x>0.5 || modes.z == 2 || modes.z == 3)) {
        float peak = max(tone.y, 100.0);
        float luma=max(dot(max(linearNits,0.0),canonical2020?float3(0.2627,0.6780,0.0593):float3(0.2126,0.7152,0.0722)),0.000001);
        float mapped=100.0*(luma/peak)/(1.0+luma/peak);
        linearNits=max(linearNits,0.0)*(mapped/luma);
    }
    // scRGB carries BT.2020 primaries through negative BT.709 components.
    // Clamping those components here changes hue/saturation before DWM's HDR
    // composition. Only clamp while tone-mapping to SDR; HDR scRGB is signed.
    if (canonical2020) linearNits = rec2020_to_scrgb(linearNits);
    if (tone.x < 0.5) linearNits=max(linearNits,0.0);
    return float4(linearNits / 80.0, 1.0);
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
    return pow(max(scene, 0.0), 1.2) * max(tone.z, 1000.0);
}
float3 rec2020_to_scrgb(float3 v) {
    return mul(float3x3(1.6605, -0.5876, -0.0728,
                       -0.1246, 1.1329, -0.0083,
                       -0.0182, -0.1006, 1.1187), v);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float2 sampleUv = lerp(sourceRect.xy, sourceRect.zw, saturate(uv));
    float3 encoded = yuv_to_rgb(
        sourceY.Sample(linearClamp, float3(sampleUv, 0.0)),
        sourceUv.Sample(linearClamp, float3(sampleUv, 0.0)));
    float3 linearNits = modes.z == 2 ? pq_to_nits(encoded) :
                        (modes.z == 3 ? hlg_to_nits(encoded) :
                         pow(saturate(encoded), 2.2) * 80.0);
    if (tone.x < 0.5 && (modes.z == 2 || modes.z == 3)) {
        float peak = max(tone.y, 100.0);
        float3 weights=(modes.w&2)!=0?float3(0.2627,0.6780,0.0593):float3(0.2126,0.7152,0.0722);
        float luma=max(dot(max(linearNits,0.0),weights),0.000001);
        float mapped=100.0*(luma/peak)/(1.0+luma/peak);
        linearNits=max(linearNits,0.0)*(mapped/luma);
    }
    if ((modes.w & 2) != 0) linearNits=rec2020_to_scrgb(linearNits);
    if (tone.x < 0.5) linearNits=max(linearNits,0.0);
    return float4(linearNits / 80.0, 1.0);
})";
    Microsoft::WRL::ComPtr<ID3DBlob> vs;
    Microsoft::WRL::ComPtr<ID3DBlob> ps;
    if (FAILED(D3DCompile(vertexShader, std::strlen(vertexShader), nullptr, nullptr, nullptr,
                          "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
        FAILED(D3DCompile(basePixelShader, std::strlen(basePixelShader), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors))) {
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

    D3D12_DESCRIPTOR_RANGE tensorRange{};
    tensorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tensorRange.NumDescriptors = 1;
    tensorRange.BaseShaderRegister = 0;
    tensorRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER tensorParameters[2]{};
    tensorParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tensorParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    tensorParameters[0].DescriptorTable.pDescriptorRanges = &tensorRange;
    tensorParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    tensorParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    tensorParameters[1].Constants.ShaderRegister = 0;
    tensorParameters[1].Constants.Num32BitValues = 20;
    tensorParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC tensorRootDesc{};
    tensorRootDesc.NumParameters = static_cast<UINT>(std::size(tensorParameters));
    tensorRootDesc.pParameters = tensorParameters;
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
cbuffer TensorCompositionConstants : register(b0) {
    uint2 tensorSize;
    float hdrOutput;
    float sourcePeakNits;
    float targetPeakNits;
    float3 padding;
    float4 trimA;
    float4 trimB;
    float4 trimC;
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
float3 pq_to_nits(float3 v) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(v), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);
}
float3 rec2020_to_scrgb(float3 v) {
    return mul(float3x3(1.6605, -0.5876, -0.0728,
                       -0.1246, 1.1329, -0.0083,
                       -0.0182, -0.1006, 1.1187), v);
}
float3 apply_dovi_trim(float3 nits) {
    if(trimA.x<0.5) return nits;
    float normalization=max(trimC.z,targetPeakNits);
    float3 src=saturate(nits/normalization); float l=max(dot(src,float3(0.2627,0.6780,0.0593)),0.000001);
    float mid=saturate(1.0-abs(l-0.45)*2.4),toe=saturate(1.0-l*2.2),outputLuma=saturate(l+trimB.w*mid);
    outputLuma=saturate(0.42+(outputLuma-0.42)*(1.0+trimC.x*mid));
    outputLuma=saturate((outputLuma-0.5)*max(trimA.y,0.01)+0.5+trimA.z*toe);
    outputLuma=pow(max(outputLuma,0.0),max(trimA.w,0.05));
    outputLuma=saturate(outputLuma+trimC.y*saturate((l-0.75)*4.0)*(1.0-outputLuma)*0.45);
    float3 color=src*(outputLuma/l); float gray=dot(color,float3(0.2627,0.6780,0.0593));
    float saturation=clamp(trimB.x*lerp(1.0,trimB.y,0.25)*lerp(1.0,trimB.z,0.10),0.75,1.25);
    return max(lerp(gray.xxx,color,saturation),0.0)*normalization;
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float3 nits = apply_dovi_trim(pq_to_nits(sample_rgb(uv)));
    if (hdrOutput < 0.5) {
        float peak = max(sourcePeakNits, 100.0);
        float luma=max(dot(max(nits,0.0),float3(0.2627,0.6780,0.0593)),0.000001);
        float mapped=100.0*(luma/peak)/(1.0+luma/peak);
        nits=max(nits,0.0)*(mapped/luma);
    }
    nits=rec2020_to_scrgb(nits);
    if (hdrOutput < 0.5) nits=max(nits,0.0);
    return float4(nits / 80.0, 1.0);
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
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    float4 color = source.Sample(linearClamp, saturate(uv));
    if (alphaFromRgb != 0) color.a = max(color.r, max(color.g, color.b));
    float alpha = saturate(color.a * opacity);
    if (alpha <= 0.00001) return 0.0;
    float3 straight = saturate(color.rgb / max(color.a, 0.00001));
    float3 linearRgb = srgb_to_linear(straight) * (max(sdrWhiteNits, 1.0) / 80.0);
    return float4(linearRgb * alpha, alpha);
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
    for (auto& buffer : backBuffers_) buffer.Reset();
    const HRESULT result = swapChain_->ResizeBuffers(kBufferCount, width, height,
                                                      kCompositionFormat, 0);
    if (FAILED(result)) return !DeviceLost(result, L"ResizeBuffers");
    width_ = width;
    height_ = height;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < kBufferCount; ++index) {
        if (FAILED(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffers_[index])))) return false;
        device_->CreateRenderTargetView(backBuffers_[index].Get(), nullptr, rtv);
        rtv.ptr += rtvIncrement_;
    }
    return true;
}

bool D3D12VideoRenderer::RenderFrame(const NativeVideoFrame& frame) {
    if (!frame.HasD3D12Texture() ||
        (frame.d3dFormat != DXGI_FORMAT_NV12 && frame.d3dFormat != DXGI_FORMAT_P010)) {
        Log(LogLevel::Warning, L"d3d12 compositor rejected non-resident or unsupported frame");
        return false;
    }
    const auto startedAt = std::chrono::steady_clock::now();
    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!WaitForBackBuffer(backBufferIndex)) return false;
    inFlightFrames_[backBufferIndex].reset();
    transients_[backBufferIndex].clear();
    const bool requiresDolbyVisionPipeline =
        (frame.dovi && frame.dovi->valid) ||
        (frame.enhancementDovi && frame.enhancementDovi->valid) ||
        frame.HasEnhancementD3D12Texture();
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

    const D3D12_RESOURCE_DESC sourceDesc = frame.d3d12Texture->GetDesc();
    const UINT arraySize = std::max<UINT>(1, sourceDesc.DepthOrArraySize);
    const UINT plane0 = std::min(frame.d3d12Subresource, arraySize - 1);
    const UINT plane1 = plane0 + arraySize;
    const NativeVideoFrame* enhancement = frame.HasEnhancementD3D12Texture()
        ? frame.enhancementFrame.get() : nullptr;
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
        TransitionBarrier(frame.d3d12Texture.Get(), plane0, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(frame.d3d12Texture.Get(), plane1, D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    if (enhancement) {
        beginBarriers.push_back(TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane0,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        beginBarriers.push_back(TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane1,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
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
    const bool tenBit = frame.d3dFormat == DXGI_FORMAT_P010;
    makeSrv(frame, tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, 0, srvCpu);
    srvCpu.ptr += srvIncrement_;
    makeSrv(frame, tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, 1, srvCpu);
    srvCpu.ptr += srvIncrement_;
    const NativeVideoFrame& enhancementSource = enhancement ? *enhancement : frame;
    const bool enhancementTenBit = enhancementSource.d3dFormat == DXGI_FORMAT_P010;
    makeSrv(enhancementSource,
            enhancementTenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, 0, srvCpu);
    srvCpu.ptr += srvIncrement_;
    makeSrv(enhancementSource,
            enhancementTenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, 1, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvIncrement_;
    constexpr float clearColor[4]{};
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    const float sourceAspect = static_cast<float>(frame.width) / std::max(1, frame.height);
    const float outputAspect = static_cast<float>(width_) / std::max<UINT>(1, height_);
    D3D12_VIEWPORT viewport{};
    if (sourceAspect > outputAspect) {
        viewport.Width = static_cast<float>(width_);
        viewport.Height = viewport.Width / sourceAspect;
        viewport.TopLeftY = (static_cast<float>(height_) - viewport.Height) * 0.5f;
    } else {
        viewport.Height = static_cast<float>(height_);
        viewport.Width = viewport.Height * sourceAspect;
        viewport.TopLeftX = (static_cast<float>(width_) - viewport.Width) * 0.5f;
    }
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
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
    constants.sourceUv[0] = frame.sourceUvRect.left;
    constants.sourceUv[1] = frame.sourceUvRect.top;
    constants.sourceUv[2] = frame.sourceUvRect.right;
    constants.sourceUv[3] = frame.sourceUvRect.bottom;
    const VideoTextureUvRect enhancementRect = enhancement
        ? enhancement->sourceUvRect : frame.sourceUvRect;
    constants.enhancementSourceUv[0] = enhancementRect.left;
    constants.enhancementSourceUv[1] = enhancementRect.top;
    constants.enhancementSourceUv[2] = enhancementRect.right;
    constants.enhancementSourceUv[3] = enhancementRect.bottom;
    constants.matrix = MatrixMode(frame.color.matrix);
    const auto* reconstructionMetadata =
        enhancement && frame.enhancementDovi && frame.enhancementDovi->valid
            ? frame.enhancementDovi.get()
            : (frame.dovi && frame.dovi->valid ? frame.dovi.get() : nullptr);
    constants.range = reconstructionMetadata
        ? (reconstructionMetadata->blVideoFullRange ? 1u : 0u)
        : (frame.color.range == VideoColorRange::Full ? 1u : 0u);
    constants.transfer = TransferMode(frame.color.transfer);
    constants.primaries = (tenBit ? 1u : 0u) |
        (frame.color.primaries == VideoColorPrimaries::Bt2020 ? 2u : 0u);
    const bool hdrOutput = frame.color.IsHdr() &&
        videoSettings_.hdrOutput != HdrOutputMode::ForceSdr &&
        (displayCapabilities_.hdrEnabled || videoSettings_.hdrOutput == HdrOutputMode::ForceHdr);
    constants.hdrOutput = hdrOutput ? 1.0f : 0.0f;
    constants.sourcePeakNits = SourcePeakNits(frame.color);
    constants.targetPeakNits = static_cast<float>(
        videoSettings_.displayPeakBrightnessNits > 0
            ? videoSettings_.displayPeakBrightnessNits
            : std::max(100, displayCapabilities_.reportedPeakBrightnessNits));
    commandList_->SetGraphicsRoot32BitConstants(1, 16, &constants, 0);
    DoviShaderConstantsPair doviConstants{};
    FillDoviShaderConstants(doviConstants, 0, frame, constants.targetPeakNits);
    std::memcpy(doviConstantMappings_[backBufferIndex], &doviConstants,
                sizeof(doviConstants));
    commandList_->SetGraphicsRootConstantBufferView(
        2, doviConstantBuffers_[backBufferIndex]->GetGPUVirtualAddress());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);
    DrawOverlays(&frame, viewport, backBufferIndex);

    std::vector<D3D12_RESOURCE_BARRIER> endBarriers{
        TransitionBarrier(frame.d3d12Texture.Get(), plane0,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(frame.d3d12Texture.Get(), plane1,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT)};
    if (enhancement) {
        endBarriers.insert(endBarriers.end() - 1, TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane0,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
        endBarriers.insert(endBarriers.end() - 1, TransitionBarrier(
            enhancement->d3d12Texture.Get(), enhancementPlane1,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
    }
    commandList_->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(commandList_->Close())) return false;
    GpuFencePoint decodeDependency;
    decodeDependency.fence = frame.d3d12ReadyFence;
    decodeDependency.value = frame.d3d12ReadyFenceValue;
    GpuFencePoint frameComputeDependency;
    const auto computeCompletion = sourceComputeCompletions_.find(
        {frame.timelineSerial, frame.serial});
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
    sourceGraphicsCompletions_[{frame.timelineSerial, frame.serial}] =
        graphSubmit.completion;
    if (!dolbyVisionD3D12Logged_ && doviConstants.signalMeta[0][0] > 0.5f) {
        dolbyVisionD3D12Logged_ = true;
        Log(LogLevel::Info,
            doviConstants.signalMeta[0][0] > 1.5f
                ? L"Dolby Vision reconstruction path=d3d12_rpu+el_fel+nlq canonical=bt2020_pq"
                : L"Dolby Vision reconstruction path=d3d12_rpu_reshape canonical=bt2020_pq");
    }

    const HRESULT presentResult = swapChain_->Present(1, 0);
    if (FAILED(presentResult)) return !DeviceLost(presentResult, L"Present");
    const uint64_t fenceValue = nextFenceValue_++;
    if (FAILED(queue_->Signal(fence_.Get(), fenceValue))) return false;
    bufferFenceValues_[backBufferIndex] = fenceValue;
    inFlightFrames_[backBufferIndex] = std::make_unique<NativeVideoFrame>(frame);
    AnchorGeneratedFrames(frame, std::chrono::steady_clock::now());

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.frames;
        ++renderStats_.hardwareFrames;
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
            refreshHz = static_cast<double>(mode.dmDisplayFrequency);
        }
    }
    int multiplier = 2;
    if (sourceFps > 1.0 && refreshHz > 1.0) {
        const double ratio = refreshHz / sourceFps;
        multiplier = ratio < 1.25 ? 1 : std::clamp(static_cast<int>(std::lround(ratio)), 2, 5);
    }
    const int maxDimension = std::max(second.width, second.height);
    if (maxDimension > 2560) multiplier = std::min(multiplier, 2);
    else if (maxDimension > 1920) multiplier = std::min(multiplier, 3);
    return std::min(multiplier, adaptiveMultiplierCap_);
}

void D3D12VideoRenderer::ProcessFrameGraphInput(const NativeVideoFrame& frame) {
    IMlFrameInterpolationExecutor* executor =
        mlExecutor_.load(std::memory_order_acquire);
    if (!interpolationRequested_.load(std::memory_order_acquire) || !executor ||
        !executor->IsReady() || !frame.HasD3D12Texture()) {
        tensorPreviousFrame_.reset();
        return;
    }
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
        frameGraph_.AdvanceEpoch();
        tensorPreviousFrame_.reset();
        sourceComputeCompletions_.clear();
        sourceGraphicsCompletions_.clear();
    }
    if (!tensorPreviousFrame_) {
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        return;
    }

    const NativeVideoFrame& first = *tensorPreviousFrame_;
    if (!executor->CanSubmit()) {
        // Keep only the newest endpoint while inference is saturated. Most
        // importantly, do not enqueue preprocessing on the shared resource:
        // original frames must continue directly to graphics without waiting
        // behind old ML work.
        sourceGraphicsCompletions_.erase({first.timelineSerial, first.serial});
        try {
            tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
        } catch (...) {
            tensorPreviousFrame_.reset();
        }
        return;
    }
    const int multiplier = InterpolationMultiplier(first, frame);
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
    for (int sample = 1; sample < multiplier; ++sample) {
        if (!executor->CanSubmit()) break;
        const std::optional<std::size_t> generatedSlot = AcquireGeneratedSlot();
        if (!generatedSlot) break;
        const float interpolationT = static_cast<float>(sample) /
            static_cast<float>(multiplier);
        TensorPreprocessResult preprocess = tensorPreprocessor_.SubmitPair(
            first, frame, interpolationT, frameGraph_.CurrentEpoch(), orderingDependency);
        if (!preprocess.accepted) {
            if (preprocess.reason != L"tensor_preprocess_gpu_busy") {
                Log(LogLevel::Warning,
                    L"frame graph preprocess skipped reason=" + preprocess.reason);
            }
            break;
        }
        orderingDependency = preprocess.tensor.ready;
        sourceComputeCompletions_[{first.timelineSerial, first.serial}] =
            preprocess.tensor.ready;
        sourceComputeCompletions_[{frame.timelineSerial, frame.serial}] =
            preprocess.tensor.ready;
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
        tensorPreprocessor_.RetainUntil(preprocess.tensor.resource.Get(), inference.completion);
        output.shape = preprocess.shape;
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
        try {
            output.overlayFrame = std::make_unique<NativeVideoFrame>(frame);
        } catch (...) {
            output.overlayFrame.reset();
        }
        const auto* generatedDovi =
            frame.enhancementDovi && frame.enhancementDovi->valid
                ? frame.enhancementDovi.get()
                : (frame.dovi && frame.dovi->valid ? frame.dovi.get() : nullptr);
        const float interpolationTargetPeak = static_cast<float>(
            videoSettings_.displayPeakBrightnessNits > 0
                ? videoSettings_.displayPeakBrightnessNits
                : std::max(100, displayCapabilities_.reportedPeakBrightnessNits));
        output.doviTrim = SelectDoviDisplayTrim(generatedDovi, interpolationTargetPeak);
        output.doviSourcePeakNits = generatedDovi && generatedDovi->sourceMaxNits > 0.0f
            ? generatedDovi->sourceMaxNits : SourcePeakNits(frame.color);
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
                    L" layout=fp16_nchw_7 domain=bt2020_pq variable_t=true");
        }
    }
    try {
        tensorPreviousFrame_ = std::make_unique<NativeVideoFrame>(frame);
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
        if (!slot.pending || slot.anchored ||
            slot.timelineSerial != original.timelineSerial || slot.leftPts != original.pts) {
            continue;
        }
        slot.anchored = true;
        slot.dueAt = presentedAt + (slot.targetPts - slot.leftPts);
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
        slot.overlayFrame.reset();
        slot.anchored = false;
        slot.completionStatusConsumed = false;
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
        const bool presented = RenderGeneratedFrame(slotIndex);
        RecordGeneratedDeadline(presented);
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
        const bool ready = slot.ready.IsValid() &&
            slot.ready.fence->GetCompletedValue() >= slot.ready.value;
        if (ready) {
            IMlFrameInterpolationExecutor* executor =
                mlExecutor_.load(std::memory_order_acquire);
            const std::wstring error = executor
                ? executor->TakeCompletionError(slot.ready.value)
                : L"ml_executor_unavailable";
            slot.completionStatusConsumed = true;
            if (error.empty()) {
                RecordGeneratedDeadline(RenderGeneratedFrame(slotIndex));
            }
            else {
                Log(LogLevel::Warning, L"ML generated frame dropped reason=" + error);
                std::scoped_lock lock(statsMutex_);
                ++renderStats_.inferenceFailures;
                publishedStats_ = renderStats_;
            }
        } else {
            RecordGeneratedDeadline(false);
            std::scoped_lock lock(statsMutex_);
            ++renderStats_.generatedDroppedNotReady;
            publishedStats_ = renderStats_;
        }
        RetireGeneratedSlot(slotIndex, false);
        pendingGeneratedSlots_.pop_front();
    }
}

void D3D12VideoRenderer::RecordGeneratedDeadline(const bool met) {
    ++adaptiveDeadlineSamples_;
    if (met) ++adaptiveDeadlineHits_;
    if (adaptiveDeadlineSamples_ < 16) return;

    const int missed = adaptiveDeadlineSamples_ - adaptiveDeadlineHits_;
    if (missed >= 4 && adaptiveMultiplierCap_ > 2) {
        --adaptiveMultiplierCap_;
        adaptiveRecoveryWindows_ = 0;
        Log(LogLevel::Info,
            L"adaptive interpolation cadence reduced cap=x" +
                std::to_wstring(adaptiveMultiplierCap_) +
                L" deadline_misses=" + std::to_wstring(missed) + L"/16");
    } else if (missed == 0 && adaptiveMultiplierCap_ < 5) {
        if (++adaptiveRecoveryWindows_ >= 32) {
            ++adaptiveMultiplierCap_;
            adaptiveRecoveryWindows_ = 0;
            Log(LogLevel::Info,
                L"adaptive interpolation cadence increased cap=x" +
                    std::to_wstring(adaptiveMultiplierCap_));
        }
    } else {
        adaptiveRecoveryWindows_ = 0;
    }
    adaptiveDeadlineSamples_ = 0;
    adaptiveDeadlineHits_ = 0;
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
    slot.pending = false;
    slot.anchored = false;
    slot.overlayFrame.reset();
}

bool D3D12VideoRenderer::RenderGeneratedFrame(const std::size_t slotIndex) {
    if (slotIndex >= generatedSlots_.size() || !tensorPipeline_ || !tensorSrvHeap_) {
        return false;
    }
    GeneratedFrameSlot& slot = generatedSlots_[slotIndex];
    if (!slot.output || !slot.shape.IsValid()) return false;
    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!WaitForBackBuffer(backBufferIndex)) return false;
    inFlightFrames_[backBufferIndex].reset();
    transients_[backBufferIndex].clear();
    if (FAILED(allocators_[backBufferIndex]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[backBufferIndex].Get(), tensorPipeline_.Get()))) {
        return false;
    }
    std::array<D3D12_RESOURCE_BARRIER, 2> beginBarriers{
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET),
        TransitionBarrier(slot.output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    commandList_->ResourceBarrier(static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
        tensorSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(backBufferIndex) * srvIncrement_;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = static_cast<UINT>(slot.capacityBytes / sizeof(UINT));
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(slot.output.Get(), &view, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvIncrement_;
    constexpr float clearColor[4]{};
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    const float sourceAspect = static_cast<float>(std::max(1, slot.displayWidth)) /
        static_cast<float>(std::max(1, slot.displayHeight));
    const float outputAspect = static_cast<float>(width_) / std::max<UINT>(1, height_);
    D3D12_VIEWPORT viewport{};
    if (sourceAspect > outputAspect) {
        viewport.Width = static_cast<float>(width_);
        viewport.Height = viewport.Width / sourceAspect;
        viewport.TopLeftY = (static_cast<float>(height_) - viewport.Height) * 0.5f;
    } else {
        viewport.Height = static_cast<float>(height_);
        viewport.Width = viewport.Height * sourceAspect;
        viewport.TopLeftX = (static_cast<float>(width_) - viewport.Width) * 0.5f;
    }
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->SetGraphicsRootSignature(tensorRootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {tensorSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu =
        tensorSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(backBufferIndex) * srvIncrement_;
    commandList_->SetGraphicsRootDescriptorTable(0, srvGpu);
    TensorCompositionConstants constants{};
    constants.tensorWidth = slot.shape.width;
    constants.tensorHeight = slot.shape.height;
    const bool hdrOutput = videoSettings_.hdrOutput != HdrOutputMode::ForceSdr &&
        (displayCapabilities_.hdrEnabled ||
         videoSettings_.hdrOutput == HdrOutputMode::ForceHdr);
    constants.hdrOutput = hdrOutput ? 1.0f : 0.0f;
    constants.sourcePeakNits = SourcePeakNits(mediaColor_);
    constants.targetPeakNits = static_cast<float>(
        videoSettings_.displayPeakBrightnessNits > 0
            ? videoSettings_.displayPeakBrightnessNits
            : std::max(100, displayCapabilities_.reportedPeakBrightnessNits));
    constants.trimA[0] = slot.doviTrim.enabled ? 1.0f : 0.0f;
    constants.trimA[1] = slot.doviTrim.slope;
    constants.trimA[2] = slot.doviTrim.offset;
    constants.trimA[3] = slot.doviTrim.power;
    constants.trimB[0] = slot.doviTrim.saturation;
    constants.trimB[1] = slot.doviTrim.chromaWeight;
    constants.trimB[2] = slot.doviTrim.msWeight;
    constants.trimB[3] = slot.doviTrim.midOffset;
    constants.trimC[0] = slot.doviTrim.midContrast;
    constants.trimC[1] = slot.doviTrim.clip;
    constants.trimC[2] = slot.doviSourcePeakNits;
    commandList_->SetGraphicsRoot32BitConstants(1, 20, &constants, 0);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);
    DrawOverlays(slot.overlayFrame.get(), viewport, backBufferIndex);

    std::array<D3D12_RESOURCE_BARRIER, 2> endBarriers{
        TransitionBarrier(slot.output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        TransitionBarrier(backBuffers_[backBufferIndex].Get(),
                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT),
    };
    commandList_->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(commandList_->Close())) return false;
    const std::array dependencies{slot.ready};
    SubmitResult graphSubmit = frameGraph_.Submit(
        GpuFrameQueue::Graphics, commandList_.Get(), dependencies);
    if (!graphSubmit.accepted) return false;
    if (FAILED(swapChain_->Present(1, 0))) return false;
    const uint64_t fenceValue = nextFenceValue_++;
    if (FAILED(queue_->Signal(fence_.Get(), fenceValue))) return false;
    bufferFenceValues_[backBufferIndex] = fenceValue;
    slot.reusable = graphSubmit.completion;
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

bool D3D12VideoRenderer::DrawOverlays(const NativeVideoFrame* frame,
                                      const D3D12_VIEWPORT& videoViewport,
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
        bool alphaFromRgb = false;
        bool subtitle = false;
    };
    std::vector<DrawItem> items;
    items.reserve(kMaxOverlayTextures);
    if (frame) {
        for (const NativeSubtitleBitmap& bitmap : frame->subtitleBitmaps) {
            if (!bitmap.HasPixels() || items.size() >= kMaxOverlayTextures) continue;
            const int canvasWidth = std::max(1, bitmap.canvasWidth > 0
                                                    ? bitmap.canvasWidth
                                                    : frame->width);
            const int canvasHeight = std::max(1, bitmap.canvasHeight > 0
                                                     ? bitmap.canvasHeight
                                                     : frame->height);
            DrawItem item;
            item.pixels = bitmap.bgra;
            item.width = bitmap.width;
            item.height = bitmap.height;
            item.stride = bitmap.stride;
            item.serial = bitmap.serial;
            item.x = videoViewport.TopLeftX + videoViewport.Width * bitmap.x / canvasWidth;
            item.y = videoViewport.TopLeftY + videoViewport.Height * bitmap.y / canvasHeight;
            item.displayWidth = videoViewport.Width * bitmap.width / canvasWidth;
            item.displayHeight = videoViewport.Height * bitmap.height / canvasHeight;
            item.subtitle = true;
            items.push_back(std::move(item));
        }
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
        item.opacity = std::clamp(slot.bitmap->opacity *
                                      (hasPresentation ? slot.presentation.opacity : 1.0f),
                                  0.0f, 1.0f);
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
    for (UINT itemIndex = 0; itemIndex < static_cast<UINT>(items.size()); ++itemIndex) {
        const DrawItem& item = items[itemIndex];
        OverlayTextureCache& cache = overlayTextureCaches_[backBufferIndex][itemIndex];
        const void* sourceIdentity = item.pixels.get();
        const bool dimensionsChanged = !cache.texture ||
            cache.width != static_cast<UINT>(item.width) ||
            cache.height != static_cast<UINT>(item.height);
        const bool contentChanged = dimensionsChanged ||
            cache.sourceIdentity != sourceIdentity || cache.sourceSerial != item.serial;
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
                    swapChainHdr_ ? 1.0f : 0.0f, {}};
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
    }
    if (drewSubtitle) {
        std::scoped_lock lock(statsMutex_);
        ++renderStats_.subtitleFrames;
    }
    return true;
}

bool D3D12VideoRenderer::RenderLastFrame() {
    return currentFrame_ && RenderFrame(*currentFrame_);
}

void D3D12VideoRenderer::ResetInterpolationState() {
    tensorPreviousFrame_.reset();
    sourceComputeCompletions_.clear();
    sourceGraphicsCompletions_.clear();
    for (const std::size_t slotIndex : pendingGeneratedSlots_) {
        RetireGeneratedSlot(slotIndex, false);
    }
    pendingGeneratedSlots_.clear();
    tensorPreprocessorLogged_ = false;
    directMlSubmitLogged_ = false;
    generatedPresentLogged_ = false;
    adaptiveMultiplierCap_ = 2;
    adaptiveDeadlineSamples_ = 0;
    adaptiveDeadlineHits_ = 0;
    adaptiveRecoveryWindows_ = 0;
    frameGraph_.AdvanceEpoch();
    {
        std::scoped_lock lock(statsMutex_);
        renderStats_.interpolationMultiplier = 1;
        publishedStats_ = renderStats_;
    }
}

void D3D12VideoRenderer::ClearFrame() {
    currentFrame_.reset();
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
    swapChain_->Present(1, 0);
    const uint64_t value = nextFenceValue_++;
    if (SUCCEEDED(queue_->Signal(fence_.Get(), value))) bufferFenceValues_[index] = value;
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

void D3D12VideoRenderer::ReleaseGpu() {
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
    tensorPreviousFrame_.reset();
    sourceComputeCompletions_.clear();
    sourceGraphicsCompletions_.clear();
    tensorPreprocessor_.Reset();
    pendingGeneratedSlots_.clear();
    for (GeneratedFrameSlot& slot : generatedSlots_) slot = {};
    for (auto& backBufferCaches : overlayTextureCaches_) {
        for (OverlayTextureCache& cache : backBufferCaches) {
            if (cache.upload && cache.mappedUpload) cache.upload->Unmap(0, nullptr);
            cache = {};
        }
    }
    for (auto& frame : inFlightFrames_) frame.reset();
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
    swapChain_.Reset();
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
    HRESULT reason = result;
    if (device_) {
        const HRESULT removedReason = device_->GetDeviceRemovedReason();
        if (FAILED(removedReason)) reason = removedReason;
    }
    Log(LogLevel::Error, std::wstring(operation) + L" lost d3d12 device");
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
