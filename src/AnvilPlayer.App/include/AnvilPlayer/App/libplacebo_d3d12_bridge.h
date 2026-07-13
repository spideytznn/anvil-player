#pragma once

#include "AnvilPlayer/App/log_sink_ptr.h"

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>

namespace anvil::app {

struct NativeVideoFrame;

// Optional libplacebo renderer hosted on D3D11On12. The bridge shares the
// application's D3D12 device and graphics queue, so decoded P010/NV12 surfaces
// and the scRGB swap-chain buffer remain GPU resident.
class LibplaceboD3D12Bridge {
public:
    explicit LibplaceboD3D12Bridge(LogSinkPtr logSink = nullptr);
    ~LibplaceboD3D12Bridge();
    LibplaceboD3D12Bridge(const LibplaceboD3D12Bridge&) = delete;
    LibplaceboD3D12Bridge& operator=(const LibplaceboD3D12Bridge&) = delete;

    static bool RequestedByEnvironment() noexcept;

    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue);
    void Reset() noexcept;
    bool IsReady() const noexcept;
    std::wstring Version() const;

    bool RenderDolbyVision(const NativeVideoFrame& frame,
                           ID3D12Resource* target,
                           UINT outputWidth,
                           UINT outputHeight,
                           bool hdrOutput,
                           float targetPeakNits,
                           ID3D12Fence* supplementalFence = nullptr,
                           std::uint64_t supplementalFenceValue = 0,
                           bool stretchToOutput = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anvil::app
