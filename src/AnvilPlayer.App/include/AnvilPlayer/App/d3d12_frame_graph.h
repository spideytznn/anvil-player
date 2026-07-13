#pragma once

#include "AnvilPlayer/Playback/Types.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>

namespace anvil::app {

enum class GpuFrameQueue {
    VideoDecode,
    ComputeMl,
    Graphics,
    Copy,
};

struct GpuFencePoint {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;

    bool IsValid() const noexcept { return fence && value != 0; }
};

// Resource-bearing node passed between decode, preprocess/ML and composition.
// Epoch invalidation is CPU-only: old resources finish naturally on their
// producing queue and are recycled after the fence reaches ready.value.
struct GpuFrameNode {
    std::chrono::milliseconds pts{0};
    uint64_t epoch = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    UINT subresource = 0;
    GpuFrameQueue producingQueue = GpuFrameQueue::VideoDecode;
    GpuFencePoint ready;
    anvil::playback::VideoColorMetadata color;
    bool generated = false;
    float confidence = 1.0f;
    float sceneCutConfidence = 0.0f;
};

struct SubmitResult {
    bool accepted = false;
    GpuFencePoint completion;
    std::wstring reason;
};

// Every executor must consume and produce application-owned D3D12 buffers.
// A backend that cannot bind them directly is rejected; CPU tensors and API
// bridge textures are deliberately not part of this contract.
class IMlFrameInterpolationExecutor {
public:
    virtual ~IMlFrameInterpolationExecutor() = default;

    virtual bool IsReady() const noexcept = 0;
    // Cheap admission check used before preprocessing. Executors must bound
    // outstanding work so a slow model can never build an inference backlog
    // that delays original-frame presentation.
    virtual bool CanSubmit() const noexcept = 0;
    virtual std::wstring BackendName() const = 0;

    virtual SubmitResult Submit(
        ID3D12Resource* inputTensor,
        ID3D12Resource* outputTensor,
        UINT tensorWidth,
        UINT tensorHeight,
        ID3D12Fence* dependencyFence,
        uint64_t dependencyValue) = 0;

    // Completion errors are associated with a single fence value. Consuming a
    // status prevents an old failed inference from poisoning later frames.
    virtual std::wstring TakeCompletionError(uint64_t completionValue) = 0;
};

class D3D12FrameGraph {
public:
    D3D12FrameGraph() = default;
    D3D12FrameGraph(const D3D12FrameGraph&) = delete;
    D3D12FrameGraph& operator=(const D3D12FrameGraph&) = delete;

    bool Initialize(ID3D12Device* device,
                    ID3D12CommandQueue* computeQueue,
                    ID3D12CommandQueue* graphicsQueue,
                    ID3D12CommandQueue* copyQueue);
    void Reset();

    SubmitResult Submit(GpuFrameQueue queue,
                        ID3D12CommandList* commandList,
                        std::span<const GpuFencePoint> dependencies = {});
    bool WaitForIdle(DWORD timeoutMilliseconds);

    uint64_t CurrentEpoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
    uint64_t AdvanceEpoch() noexcept {
        return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    bool IsCurrent(const GpuFrameNode& node) const noexcept {
        return node.epoch == CurrentEpoch();
    }

private:
    struct QueueTimeline {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t nextValue = 1;
        std::mutex submitMutex;
    };

    static std::size_t QueueIndex(GpuFrameQueue queue);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    std::array<QueueTimeline, 3> timelines_{};
    std::atomic_uint64_t epoch_{1};
};

}  // namespace anvil::app
