#pragma once

#include "AnvilPlayer/App/d3d12_frame_graph.h"

#include <d3d12.h>

#include <filesystem>
#include <memory>
#include <string>

namespace anvil::app {

// Windows.AI.MachineLearning backend bound to the player's own D3D12 compute
// queue. ITensorStaticsNative wraps the application's FP16 input/output
// resources directly; no CPU tensor or cross-API texture is permitted.
class WindowsMlFrameInterpolationExecutor final : public IMlFrameInterpolationExecutor {
public:
    WindowsMlFrameInterpolationExecutor();
    ~WindowsMlFrameInterpolationExecutor() override;
    WindowsMlFrameInterpolationExecutor(const WindowsMlFrameInterpolationExecutor&) = delete;
    WindowsMlFrameInterpolationExecutor& operator=(const WindowsMlFrameInterpolationExecutor&) = delete;

    bool Initialize(ID3D12Device* device,
                    ID3D12CommandQueue* computeQueue,
                    const std::filesystem::path& modelPath);
    void Reset();
    bool IsReady() const noexcept override;
    bool CanSubmit() const noexcept override;
    std::wstring BackendName() const override;
    std::wstring LastError() const;

    SubmitResult Submit(ID3D12Resource* inputTensor,
                        ID3D12Resource* outputTensor,
                        UINT tensorWidth,
                        UINT tensorHeight,
                        ID3D12Fence* dependencyFence,
                        uint64_t dependencyValue) override;
    std::wstring TakeCompletionError(uint64_t completionValue) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anvil::app
