#pragma once

#include "AnvilPlayer/App/d3d12_frame_graph.h"
#include "AnvilPlayer/App/d3d12_dolby_vision.h"
#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/frame_interpolation_policy.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace anvil::app {

struct TensorShape {
    UINT width = 0;
    UINT height = 0;

    bool IsValid() const noexcept { return width != 0 && height != 0; }
};

// Selects a source-resolution inference extent capped at the configured
// picture height and aligned for the interpolation model.
TensorShape SelectInterpolationTensorShape(UINT width,
                                           UINT height,
                                           UINT maximumPictureHeight) noexcept;

enum class SceneChangeProbeStatus {
    Invalid,
    Pending,
    Ready,
};

// Complete per-endpoint transform into one immutable presentation target.
// Every leaf is a float4 so this structure can be copied directly to the b2
// HLSL constant buffer without packing ambiguity. HDR endpoints leave the
// preprocessor as display-mapped BT.2020/PQ; SDR endpoints remain in their
// native encoded model domain.
struct InterpolationDisplayMappingPair {
    float tone[2][4]{};
    float options[2][4]{};
    float hdr10PlusA[2][4]{};
    float hdr10PlusB[2][4]{};
    float hdr10PlusCurve[2][4][4]{};
    float hdrToneCurve[2][5][4]{};
};

static_assert(sizeof(InterpolationDisplayMappingPair) == 26 * 4 * sizeof(float));

struct SceneChangeProbe {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    GpuFencePoint ready;
    UINT sampleCount = 0;

    bool IsValid() const noexcept {
        return readback && ready.IsValid() && sampleCount != 0;
    }
};

SceneChangeProbeStatus ReadSceneChangeProbe(
    const SceneChangeProbe& probe,
    SceneChangeMetrics& metrics) noexcept;

struct TensorPreprocessResult {
    bool accepted = false;
    GpuFrameNode tensor;
    TensorShape shape;
    UINT batch = 1;
    UINT channels = 7;
    SceneChangeProbe sceneChange;
    std::wstring reason;
};

// Converts two D3D12VA NV12/P010 surfaces directly into one application-owned
// FP16 NCHW buffer. Channels 0..2 and 3..5 contain the two SDR-encoded or
// fixed-target display-mapped BT.2020/PQ endpoints; channel 6 is the
// interpolation time plane. No RGBA bridge texture or CPU-visible resource is
// created.
class D3D12TensorPreprocessor {
public:
    D3D12TensorPreprocessor() = default;
    D3D12TensorPreprocessor(const D3D12TensorPreprocessor&) = delete;
    D3D12TensorPreprocessor& operator=(const D3D12TensorPreprocessor&) = delete;

    bool Initialize(ID3D12Device* device, D3D12FrameGraph* frameGraph);
    void Reset();
    bool DolbyVisionPipelineReady() const noexcept {
        return publishedDoviPipeline_.load(std::memory_order_acquire) != nullptr;
    }
    bool DolbyVisionPipelineInitializationComplete() const noexcept {
        return doviInitializationComplete_.load(std::memory_order_acquire);
    }

    TensorPreprocessResult SubmitPair(const NativeVideoFrame& first,
                                      const NativeVideoFrame& second,
                                      TensorShape outputShape,
                                      float interpolationT,
                                      float hlgPeakNits,
                                      const InterpolationDisplayMappingPair& displayMapping,
                                      uint64_t epoch,
                                      const GpuFencePoint& orderingDependency = {});
    // Consumes two libplacebo scRGB intermediates (linear BT.709, 1.0 = 80
    // nits) and converts them to the same display-mapped BT.2020/PQ tensor
    // domain used by the HDR interpolation model.
    TensorPreprocessResult SubmitScRgbPair(ID3D12Resource* first,
                                           ID3D12Resource* second,
                                           TensorShape outputShape,
                                           float interpolationT,
                                           uint64_t epoch,
                                           const GpuFencePoint& orderingDependency = {});
    // Uploads libplacebo's CPU RGB output (BGRA SDR or X2BGR10LE BT.2020/PQ)
    // directly into the RGB tensor path. This keeps P5/P8 interpolation on the
    // same decoded colors as the original frames.
    TensorPreprocessResult SubmitPixelPair(const NativeVideoFrame& first,
                                           const NativeVideoFrame& second,
                                           TensorShape outputShape,
                                           float interpolationT,
                                           const InterpolationDisplayMappingPair& displayMapping,
                                           uint64_t epoch,
                                           const GpuFencePoint& orderingDependency = {});
    bool RetainUntil(ID3D12Resource* tensor, const GpuFencePoint& completion);
    bool ReleaseRetained(ID3D12Resource* tensor, const GpuFencePoint& completion);

private:
    static constexpr std::size_t kSlotCount = 8;

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
        Microsoft::WRL::ComPtr<ID3D12Resource> output;
        Microsoft::WRL::ComPtr<ID3D12Resource> sceneMetrics;
        Microsoft::WRL::ComPtr<ID3D12Resource> sceneMetricsReadback;
        Microsoft::WRL::ComPtr<ID3D12Resource> doviConstants;
        void* doviConstantsMapping = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> displayConstants;
        void* displayConstantsMapping = nullptr;
        std::size_t outputBytes = 0;
        GpuFencePoint completion;
        bool retained = false;
        Microsoft::WRL::ComPtr<ID3D12Resource> firstTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> secondTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> firstEnhancementTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> secondEnhancementTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> ownedFirstRgbTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> ownedSecondRgbTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> firstRgbUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> secondRgbUpload;
        UINT64 firstRgbUploadCapacity = 0;
        UINT64 secondRgbUploadCapacity = 0;
        std::shared_ptr<AVFrame> firstFrameRef;
        std::shared_ptr<AVFrame> secondFrameRef;
        std::shared_ptr<AVFrame> firstEnhancementFrameRef;
        std::shared_ptr<AVFrame> secondEnhancementFrameRef;
    };

    bool CreatePipeline();
    bool CreateSceneChangeResources(Slot& slot);
    void CreateSceneChangeDescriptor(Slot& slot,
                                     D3D12_CPU_DESCRIPTOR_HANDLE cpu);
    void ClearSceneChangeProbe(Slot& slot);
    void CopySceneChangeProbe(Slot& slot);
    void StartDolbyVisionInitialization();
    bool InitializeDolbyVisionPipeline();
    bool EnsureOutput(Slot& slot, std::size_t bytes);
    Slot* AcquireSlot();

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    D3D12FrameGraph* frameGraph_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scRgbPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> doviPipeline_;
    std::atomic<ID3D12PipelineState*> publishedDoviPipeline_{nullptr};
    std::atomic_bool doviInitializationStarted_{false};
    std::atomic_bool doviInitializationComplete_{false};
    std::string doviShaderSource_;
    std::thread doviInitializationThread_;
    std::array<Slot, kSlotCount> slots_{};
    UINT descriptorIncrement_ = 0;
};

}  // namespace anvil::app
