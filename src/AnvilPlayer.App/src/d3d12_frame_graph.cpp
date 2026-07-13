#include "AnvilPlayer/App/d3d12_frame_graph.h"

#include <limits>

namespace anvil::app {

std::size_t D3D12FrameGraph::QueueIndex(const GpuFrameQueue queue) {
    switch (queue) {
    case GpuFrameQueue::ComputeMl:
        return 0;
    case GpuFrameQueue::Graphics:
        return 1;
    case GpuFrameQueue::Copy:
        return 2;
    case GpuFrameQueue::VideoDecode:
        break;
    }
    return std::numeric_limits<std::size_t>::max();
}

bool D3D12FrameGraph::Initialize(ID3D12Device* device,
                                 ID3D12CommandQueue* computeQueue,
                                 ID3D12CommandQueue* graphicsQueue,
                                 ID3D12CommandQueue* copyQueue) {
    Reset();
    if (!device || !computeQueue || !graphicsQueue || !copyQueue) return false;
    device_ = device;
    ID3D12CommandQueue* queues[] = {computeQueue, graphicsQueue, copyQueue};
    for (std::size_t index = 0; index < timelines_.size(); ++index) {
        timelines_[index].queue = queues[index];
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&timelines_[index].fence)))) {
            Reset();
            return false;
        }
        timelines_[index].nextValue = 1;
    }
    epoch_.store(1, std::memory_order_release);
    return true;
}

void D3D12FrameGraph::Reset() {
    for (QueueTimeline& timeline : timelines_) {
        std::scoped_lock lock(timeline.submitMutex);
        timeline.queue.Reset();
        timeline.fence.Reset();
        timeline.nextValue = 1;
    }
    device_.Reset();
    epoch_.store(1, std::memory_order_release);
}

SubmitResult D3D12FrameGraph::Submit(
    const GpuFrameQueue queue,
    ID3D12CommandList* commandList,
    const std::span<const GpuFencePoint> dependencies) {
    SubmitResult result;
    const std::size_t index = QueueIndex(queue);
    if (index >= timelines_.size()) {
        result.reason = L"video_decode_queue_is_external";
        return result;
    }
    if (!commandList) {
        result.reason = L"missing_command_list";
        return result;
    }
    QueueTimeline& timeline = timelines_[index];
    std::scoped_lock lock(timeline.submitMutex);
    if (!timeline.queue || !timeline.fence) {
        result.reason = L"frame_graph_not_initialized";
        return result;
    }
    for (const GpuFencePoint& dependency : dependencies) {
        if (!dependency.IsValid()) continue;
        const HRESULT waitResult = timeline.queue->Wait(dependency.fence.Get(), dependency.value);
        if (FAILED(waitResult)) {
            result.reason = L"queue_dependency_wait_failed";
            return result;
        }
    }
    ID3D12CommandList* lists[] = {commandList};
    timeline.queue->ExecuteCommandLists(1, lists);
    const uint64_t value = timeline.nextValue++;
    if (FAILED(timeline.queue->Signal(timeline.fence.Get(), value))) {
        result.reason = L"queue_signal_failed";
        return result;
    }
    result.accepted = true;
    result.completion.fence = timeline.fence;
    result.completion.value = value;
    return result;
}

bool D3D12FrameGraph::WaitForIdle(const DWORD timeoutMilliseconds) {
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) return false;
    bool completed = true;
    for (QueueTimeline& timeline : timelines_) {
        std::scoped_lock lock(timeline.submitMutex);
        if (!timeline.queue || !timeline.fence) continue;
        const uint64_t value = timeline.nextValue++;
        if (FAILED(timeline.queue->Signal(timeline.fence.Get(), value))) {
            completed = false;
            break;
        }
        if (timeline.fence->GetCompletedValue() >= value) continue;
        if (FAILED(timeline.fence->SetEventOnCompletion(value, eventHandle)) ||
            WaitForSingleObject(eventHandle, timeoutMilliseconds) != WAIT_OBJECT_0) {
            completed = false;
            break;
        }
    }
    CloseHandle(eventHandle);
    return completed;
}

}  // namespace anvil::app
