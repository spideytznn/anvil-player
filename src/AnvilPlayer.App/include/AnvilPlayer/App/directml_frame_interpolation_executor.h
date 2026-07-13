#pragma once

#include "AnvilPlayer/App/d3d12_frame_graph.h"
#include "AnvilPlayer/App/d3d12_tensor_preprocessor.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>
#include <string>

namespace anvil::app {

// ONNX Runtime graph deployment through its DML1 API on an application-owned
// DirectML device and compute queue. OrtValues wrap application D3D12 buffers;
// the execution path never allocates a CPU tensor or bridge texture.
class DirectMlFrameInterpolationExecutor final : public IMlFrameInterpolationExecutor {
public:
    DirectMlFrameInterpolationExecutor();
    ~DirectMlFrameInterpolationExecutor() override;
    DirectMlFrameInterpolationExecutor(const DirectMlFrameInterpolationExecutor&) = delete;
    DirectMlFrameInterpolationExecutor& operator=(const DirectMlFrameInterpolationExecutor&) = delete;

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

    static std::filesystem::path DefaultModelPath();
    static std::size_t OutputBufferBytes(TensorShape shape) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anvil::app
