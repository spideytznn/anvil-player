#include "AnvilPlayer/App/d3d12_tensor_preprocessor.h"

#include "AnvilPlayer/App/playback_timing_math.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include <d3dcompiler.h>

namespace anvil::app {
namespace {

constexpr UINT64 kSceneChangeMetricBytes = 4 * sizeof(UINT);
constexpr double kSceneChangeMetricScale = 65535.0;

using anvil::playback::VideoColorPrimaries;
using anvil::playback::VideoColorRange;
using anvil::playback::VideoMatrixCoefficients;
using anvil::playback::VideoTransferCharacteristic;

struct PreprocessConstants {
    float firstSourceUv[4]{};
    float secondSourceUv[4]{};
    float firstEnhancementUv[4]{};
    float secondEnhancementUv[4]{};
    UINT firstModes[4]{};
    UINT secondModes[4]{};
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    float firstHlgPeakNits = 1000.0f;
    float secondHlgPeakNits = 1000.0f;
    float interpolationT = 0.5f;
    float padding = 0.0f;
};

static_assert(sizeof(PreprocessConstants) == 30 * sizeof(UINT));

UINT MatrixMode(const VideoMatrixCoefficients matrix) noexcept {
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

UINT TransferMode(const VideoTransferCharacteristic transfer) noexcept {
    if (transfer == VideoTransferCharacteristic::Pq) return 2;
    if (transfer == VideoTransferCharacteristic::Hlg) return 3;
    return 1;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                         const UINT subresource,
                                         const D3D12_RESOURCE_STATES before,
                                         const D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

bool IsSupportedFrame(const NativeVideoFrame& frame) noexcept {
    return frame.HasD3D12Texture() &&
           (frame.d3dFormat == DXGI_FORMAT_NV12 ||
            frame.d3dFormat == DXGI_FORMAT_P010 ||
            frame.d3dFormat == DXGI_FORMAT_P016);
}

}  // namespace

TensorShape SelectInterpolationTensorShape(
    const UINT width,
    const UINT height,
    const UINT maximumPictureHeight) noexcept {
    const InterpolationTensorExtent extent =
        SelectCappedInterpolationExtent(width, height, maximumPictureHeight);
    return {extent.width, extent.height};
}

SceneChangeProbeStatus ReadSceneChangeProbe(
    const SceneChangeProbe& probe,
    SceneChangeMetrics& metrics) noexcept {
    metrics = {};
    if (!probe.IsValid()) return SceneChangeProbeStatus::Invalid;
    if (probe.ready.fence->GetCompletedValue() < probe.ready.value) {
        return SceneChangeProbeStatus::Pending;
    }
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(kSceneChangeMetricBytes)};
    void* mapping = nullptr;
    if (FAILED(probe.readback->Map(0, &readRange, &mapping)) || !mapping) {
        return SceneChangeProbeStatus::Invalid;
    }
    std::array<UINT, 4> values{};
    std::memcpy(values.data(), mapping, sizeof(values));
    const D3D12_RANGE noWrite{0, 0};
    probe.readback->Unmap(0, &noWrite);
    const double samples = static_cast<double>(probe.sampleCount);
    metrics.meanAbsoluteLumaDifference =
        static_cast<double>(values[0]) / (samples * kSceneChangeMetricScale);
    metrics.changedSampleRatio = static_cast<double>(values[1]) / samples;
    metrics.meanLeftLuma =
        static_cast<double>(values[2]) / (samples * kSceneChangeMetricScale);
    metrics.meanRightLuma =
        static_cast<double>(values[3]) / (samples * kSceneChangeMetricScale);
    return SceneChangeProbeStatus::Ready;
}

bool D3D12TensorPreprocessor::Initialize(ID3D12Device* device,
                                         D3D12FrameGraph* frameGraph) {
    Reset();
    if (!device || !frameGraph) return false;
    device_ = device;
    frameGraph_ = frameGraph;
    descriptorIncrement_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!CreatePipeline()) {
        Reset();
        return false;
    }

    for (Slot& slot : slots_) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                   IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                               slot.allocator.Get(), pipeline_.Get(),
                                               IID_PPV_ARGS(&slot.commandList)))) {
            Reset();
            return false;
        }
        if (FAILED(slot.commandList->Close())) {
            Reset();
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 10;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heapDesc,
                                                 IID_PPV_ARGS(&slot.descriptors)))) {
            Reset();
            return false;
        }
        if (!CreateSceneChangeResources(slot)) {
            Reset();
            return false;
        }
        const UINT64 doviConstantBytes =
            (sizeof(DoviShaderConstantsPair) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
            ~(static_cast<UINT64>(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) - 1);
        const UINT64 displayConstantBytes =
            (sizeof(InterpolationDisplayMappingPair) +
             D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
            ~(static_cast<UINT64>(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) - 1);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = doviConstantBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&slot.doviConstants))) ||
            FAILED(slot.doviConstants->Map(0, &noRead, &slot.doviConstantsMapping))) {
            Reset();
            return false;
        }
        bufferDesc.Width = displayConstantBytes;
        if (FAILED(device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&slot.displayConstants))) ||
            FAILED(slot.displayConstants->Map(
                0, &noRead, &slot.displayConstantsMapping))) {
            Reset();
            return false;
        }
    }
    // Compile the native DV endpoint mapper in the background during renderer
    // warm-up. A Profile 7 stream must not lose its first otherwise-valid
    // interpolation pair merely because this much larger shader was lazy.
    StartDolbyVisionInitialization();
    return true;
}

void D3D12TensorPreprocessor::Reset() {
    if (doviInitializationThread_.joinable()) {
        doviInitializationThread_.join();
    }
    publishedDoviPipeline_.store(nullptr, std::memory_order_release);
    doviInitializationStarted_.store(false, std::memory_order_release);
    doviInitializationComplete_.store(false, std::memory_order_release);
    for (Slot& slot : slots_) {
        if (slot.doviConstants && slot.doviConstantsMapping) {
            slot.doviConstants->Unmap(0, nullptr);
        }
        if (slot.displayConstants && slot.displayConstantsMapping) {
            slot.displayConstants->Unmap(0, nullptr);
        }
        slot = {};
    }
    doviPipeline_.Reset();
    scRgbPipeline_.Reset();
    pipeline_.Reset();
    doviShaderSource_.clear();
    rootSignature_.Reset();
    device_.Reset();
    frameGraph_ = nullptr;
    descriptorIncrement_ = 0;
}

bool D3D12TensorPreprocessor::CreateSceneChangeResources(Slot& slot) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kSceneChangeMetricBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device_->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&slot.sceneMetrics)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return SUCCEEDED(device_->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&slot.sceneMetricsReadback)));
}

void D3D12TensorPreprocessor::CreateSceneChangeDescriptor(
    Slot& slot,
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(
        kSceneChangeMetricBytes / sizeof(UINT));
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(slot.sceneMetrics.Get(), nullptr, &uav, cpu);
}

void D3D12TensorPreprocessor::ClearSceneChangeProbe(Slot& slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        slot.descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(9) * descriptorIncrement_;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        slot.descriptors->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(9) * descriptorIncrement_;
    constexpr UINT zeros[4]{};
    slot.commandList->ClearUnorderedAccessViewUint(
        gpu, cpu, slot.sceneMetrics.Get(), zeros, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = slot.sceneMetrics.Get();
    slot.commandList->ResourceBarrier(1, &barrier);
}

void D3D12TensorPreprocessor::CopySceneChangeProbe(Slot& slot) {
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = slot.sceneMetrics.Get();
    barriers[1] = TransitionBarrier(
        slot.sceneMetrics.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    slot.commandList->ResourceBarrier(2, barriers);
    slot.commandList->CopyBufferRegion(
        slot.sceneMetricsReadback.Get(), 0, slot.sceneMetrics.Get(), 0,
        kSceneChangeMetricBytes);
    barriers[0] = TransitionBarrier(
        slot.sceneMetrics.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.commandList->ResourceBarrier(1, barriers);
}

bool D3D12TensorPreprocessor::CreatePipeline() {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;

    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 30;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 1;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor.ShaderRegister = 2;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(parameters));
    rootDesc.pParameters = parameters;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    Microsoft::WRL::ComPtr<ID3DBlob> rootBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rootBlob, &errors)) ||
        FAILED(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                            rootBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&rootSignature_)))) {
        return false;
    }

    // The same transform is compiled into the ordinary HDR and native Dolby
    // Vision preprocessors. Each endpoint is evaluated independently, but
    // both are mapped to the same output mode and physical target peak before
    // RIFE sees either image.
    const std::string displayMappingHlsl = R"(
float display_hdr10plus_curve(uint endpoint, float t) {
    float position=saturate(t)*15.0;
    int lower=clamp((int)floor(position),0,14),upper=lower+1;
    float a=displayHdr10PlusCurve[endpoint][lower/4][lower%4];
    float b=displayHdr10PlusCurve[endpoint][upper/4][upper%4];
    return lerp(a,b,position-lower);
}
float3 display_apply_hdr10plus(uint endpoint,float3 sourceNits) {
    float4 a=displayHdr10PlusA[endpoint],b=displayHdr10PlusB[endpoint];
    if(a.x<0.5) return max(sourceNits,0.0);
    float3 nits=max(sourceNits,0.0);
    float maxRgb=max(nits.r,max(nits.g,nits.b));
    if(maxRgb<=0.0001) return nits;
    float x=saturate(maxRgb/max(a.z,1.0)),kx=a.w,ky=b.x;
    float y=x<=kx?x*ky/max(kx,0.0001):
        ky+(1.0-ky)*display_hdr10plus_curve(endpoint,(x-kx)/max(1.0-kx,0.0001));
    float3 mapped=nits*(y*max(a.y,1.0)/maxRgb);
    float luma=max(dot(mapped,float3(0.2627,0.6780,0.0593)),0.0);
    return max(luma.xxx+(mapped-luma.xxx)*max(b.z,0.0),0.0);
}
float2 display_hdr_curve_point(uint endpoint,int index) {
    float4 pairs=displayHdrToneCurve[endpoint][index/2];
    return (index&1)==0?pairs.xy:pairs.zw;
}
float display_hdr_curve_luma(uint endpoint,float lumaNits) {
    float2 previous=display_hdr_curve_point(endpoint,0);
    if(lumaNits<=previous.x) return max(previous.y,0.0);
    [unroll] for(int index=1;index<9;++index) {
        float2 next=display_hdr_curve_point(endpoint,index);
        if(lumaNits<=next.x) {
            float amount=saturate((lumaNits-previous.x)/
                                  max(next.x-previous.x,0.0001));
            return max(lerp(previous.y,next.y,amount),0.0);
        }
        previous=next;
    }
    return max(previous.y,0.0);
}
float3 display_apply_hdr_curve(uint endpoint,float3 sourceNits) {
    float3 nits=max(sourceNits,0.0);
    float flags=floor(displayTone[endpoint].w+0.5);
    if(fmod(flags,2.0)<0.5) return nits;
    float luma=max(dot(nits,float3(0.2627,0.6780,0.0593)),0.0);
    return nits*(display_hdr_curve_luma(endpoint,luma)/max(luma,0.0001));
}
float3 display_apply_peak_mapping(uint endpoint,float3 sourceNits) {
    float3 nits=max(sourceNits,0.0);
    float4 tone=displayTone[endpoint];
    float flags=floor(tone.w+0.5);
    float peak=max(nits.r,max(nits.g,nits.b));
    if(flags<2.0||peak<=0.0001) return nits;
    float target=max(tone.z,100.0),source=max(tone.y,peak);
    if(source<=target+1.0) return nits;
    uint mode=(uint)floor(displayOptions[endpoint].y+0.5);
    float knee=target*(mode==2?0.55:(mode==3?0.75:0.65));
    if(peak<=knee) return nits;
    float shoulder=max(target-knee,1.0);
    float denominator=max(1.0-exp(-(source-knee)/shoulder),0.0001);
    float mapped=knee+shoulder*(1.0-exp(-(min(peak,source)-knee)/shoulder))/denominator;
    return nits*(min(mapped,target)/peak);
}
float3 display_sdr_tone_map(uint endpoint,float3 sourceNits) {
    float3 nits=max(sourceNits,0.0);
    float luma=max(dot(nits,float3(0.2627,0.6780,0.0593)),0.000001);
    uint mode=(uint)floor(displayOptions[endpoint].y+0.5);
    float exposure=mode==2?0.72:(mode==3?0.95:0.80);
    float target=100.0;
    float knee=target*(mode==2?0.40:(mode==3?0.55:0.45));
    float shoulder=max(target-knee,0.0001),working=luma*exposure;
    if(working<=knee) return nits*exposure;
    float peak=max(max(displayTone[endpoint].y*exposure,working),target+1.0);
    float mapped=knee+shoulder*log(1.0+(working-knee)/shoulder)/
        max(log(1.0+(peak-knee)/shoulder),0.0001);
    return nits*(min(mapped,target)/luma);
}
float3 display_finish_mapping(uint endpoint,float3 nits) {
    nits=display_apply_hdr_curve(endpoint,nits);
    nits=display_apply_peak_mapping(endpoint,nits);
    if(displayTone[endpoint].x<0.5) nits=display_sdr_tone_map(endpoint,nits);
    return nits_to_pq(max(nits,0.0));
}
float3 display_map_encoded(uint endpoint,float3 encoded) {
    if(displayOptions[endpoint].x<0.5) return encoded;
    return display_finish_mapping(endpoint,
        display_apply_hdr10plus(endpoint,pq_to_nits(encoded)));
}
)";

    const std::string shader = std::string(R"(
Texture2DArray<float> firstY : register(t0);
Texture2DArray<float2> firstUv : register(t1);
Texture2DArray<float> secondY : register(t2);
Texture2DArray<float2> secondUv : register(t3);
Texture2DArray<float> firstElY : register(t4);
Texture2DArray<float2> firstElUv : register(t5);
Texture2DArray<float> secondElY : register(t6);
Texture2DArray<float2> secondElUv : register(t7);
RWByteAddressBuffer outputTensor : register(u0);
RWByteAddressBuffer sceneMetrics : register(u1);
SamplerState linearClamp : register(s0);
cbuffer PreprocessConstants : register(b0) {
    float4 firstSourceRect;
    float4 secondSourceRect;
    float4 firstEnhancementRect;
    float4 secondEnhancementRect;
    uint4 firstModes;
    uint4 secondModes;
    uint2 outputSize;
    float2 hlgPeakNits;
    float interpolationT;
    float preprocessPadding;
};
cbuffer DisplayMappingConstants : register(b2) {
    float4 displayTone[2];
    float4 displayOptions[2];
    float4 displayHdr10PlusA[2];
    float4 displayHdr10PlusB[2];
    float4 displayHdr10PlusCurve[2][4];
    float4 displayHdrToneCurve[2][5];
};
)") + std::string(D3D12DolbyVisionHlsl()) + R"(
float3 yuv_to_rgb(float y, float2 uv, uint4 modes) {
    bool full = modes.y != 0;
    bool tenBit = (modes.w & 1) != 0;
    float black = tenBit ? 64.0 / 1023.0 : 16.0 / 255.0;
    float lumaRange = tenBit ? 876.0 / 1023.0 : 219.0 / 255.0;
    float chromaRange = tenBit ? 896.0 / 1023.0 : 224.0 / 255.0;
    float yy = max(0.0, (y - (full ? 0.0 : black)) / (full ? 1.0 : lumaRange));
    float cb = (uv.x - 0.5) / (full ? 1.0 : chromaRange);
    float cr = (uv.y - 0.5) / (full ? 1.0 : chromaRange);
    float kr = modes.x == 2 ? 0.2990 : (modes.x == 3 ? 0.2627 : 0.2126);
    float kb = modes.x == 2 ? 0.1140 : (modes.x == 3 ? 0.0593 : 0.0722);
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
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);
}
float3 nits_to_pq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / max(1.0 + c3 * p, 0.000001), m2);
}
float3 hlg_to_nits(float3 v, float peak) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 scene = lerp((exp((v - c) / a) + b) / 12.0,
                        v * v / 3.0, step(v, 0.5));
    scene=max(scene,0.0);
    float sceneLuma=max(dot(scene,float3(0.2627,0.6780,0.0593)),0.000001);
    return scene*pow(sceneLuma,0.2)*max(peak,1000.0);
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274, 0.3293, 0.0433,
                       0.0691, 0.9195, 0.0114,
                       0.0164, 0.0880, 0.8956), v);
}
float3 canonical(float3 encoded, uint4 modes, float hlgPeak) {
    // Keep SDR in its native display-encoded RGB domain. The interpolation
    // model was trained on normalized SDR images; round-tripping these frames
    // through PQ made generated and original frames alternate in brightness.
    if (modes.z == 1) return saturate(encoded);
    float3 nits;
    if (modes.z == 2) nits = pq_to_nits(encoded);
    else if (modes.z == 3) nits = hlg_to_nits(encoded, hlgPeak);
    else nits = pow(saturate(encoded), 2.2) * 80.0;
    if (modes.w < 2) nits = max(rec709_to_rec2020(nits), 0.0);
    return nits_to_pq(min(nits, 10000.0));
}
)" + displayMappingHlsl + R"(
float cubic_weight(float x) {
    x=abs(x); if(x<=1.0) return (1.5*x-2.5)*x*x+1.0;
    if(x<2.0) return ((-0.5*x+2.5)*x-4.0)*x+2.0; return 0.0;
}
int2 clamp_texel(int2 p,uint w,uint h) { return int2(clamp(p.x,0,(int)w-1),clamp(p.y,0,(int)h-1)); }
float sample_el_y(Texture2DArray<float> tex,float2 uv,bool cubic) {
    if(!cubic) return tex.SampleLevel(linearClamp,float3(uv,0),0);
    uint w,h,layers,levels; tex.GetDimensions(0,w,h,layers,levels);
    float2 c=uv*float2(w,h)-0.5,f=frac(c); int2 base=int2(floor(c)); float sum=0.0,weight=0.0;
    [unroll] for(int j=-1;j<=2;++j) [unroll] for(int i=-1;i<=2;++i) {
        float q=cubic_weight(i-f.x)*cubic_weight(j-f.y);
        sum+=tex.Load(int4(clamp_texel(base+int2(i,j),w,h),0,0))*q; weight+=q;
    }
    return saturate(sum/max(weight,0.000001));
}
float2 left_chroma_uv(Texture2DArray<float2> tex,float2 uv) {
    uint w,h,layers,levels; tex.GetDimensions(0,w,h,layers,levels);
    uv.x+=0.25/max((float)w,1.0);
    return uv;
}
float2 sample_el_uv(Texture2DArray<float2> tex,float2 uv,bool cubic) {
    uv=left_chroma_uv(tex,uv);
    if(!cubic) return tex.SampleLevel(linearClamp,float3(uv,0),0);
    uint w,h,layers,levels; tex.GetDimensions(0,w,h,layers,levels);
    float2 c=uv*float2(w,h)-0.5,f=frac(c); int2 base=int2(floor(c)); float2 sum=0.0; float weight=0.0;
    [unroll] for(int j=-1;j<=2;++j) [unroll] for(int i=-1;i<=2;++i) {
        float q=cubic_weight(i-f.x)*cubic_weight(j-f.y);
        sum+=tex.Load(int4(clamp_texel(base+int2(i,j),w,h),0,0))*q; weight+=q;
    }
    return saturate(sum/max(weight,0.000001));
}
float chroma_site_y(Texture2DArray<float> tex,float2 uv) {
    uint w,h,layers,levels; tex.GetDimensions(0,w,h,layers,levels);
    float2 c=uv*float2(w,h)-0.5; int2 base=int2(floor(c)); float4 weights=float4(1,3,3,1)/8.0; float sum=0.0;
    [unroll] for(int j=0;j<4;++j) [unroll] for(int i=0;i<4;++i)
        sum+=tex.Load(int4(clamp_texel(base+int2(i-1,j-1),w,h),0,0))*weights[i]*weights[j];
    return saturate(sum);
}
float3 load_first(float2 uv,float2 displayUv) {
    if(doviSignalMeta[0].x>0.5 && (displayUv.x<doviTrimC[0].z || displayUv.y<doviTrimC[0].w ||
       displayUv.x>doviTrimD[0].x || displayUv.y>doviTrimD[0].y)) return 0.0;
    float y=firstY.SampleLevel(linearClamp,float3(uv,0),0);
    float2 chroma=firstUv.SampleLevel(linearClamp,float3(left_chroma_uv(firstUv,uv),0),0);
    if(doviSignalMeta[0].x>1.5) {
        float2 elUv=lerp(firstEnhancementRect.xy,firstEnhancementRect.zw,displayUv);
        float3 composed=dovi_compose_p7_fel(0,float3(y,chroma),float3(chroma_site_y(firstY,uv),chroma),
            float3(sample_el_y(firstElY,elUv,doviComposerScale[0].w>0.5),sample_el_uv(firstElUv,elUv,doviComposerScale[0].w>0.5)));
        float3 encoded=dovi_decode_reshaped(0,composed);
        if(displayOptions[0].x<0.5) return encoded;
        float3 nits=display_apply_hdr10plus(0,pq_to_nits(encoded));
        if(displayOptions[0].z>0.5)
            nits=dovi_apply_display_trim(0,nits,displayTone[0].z);
        return display_finish_mapping(0,nits);
    }
    if(doviSignalMeta[0].x>0.5) {
        float3 encoded=dovi_decode_single_layer(0,saturate(float3(y,chroma)*doviSignalMeta[0].w));
        if(displayOptions[0].x<0.5) return encoded;
        float3 nits=display_apply_hdr10plus(0,pq_to_nits(encoded));
        if(displayOptions[0].z>0.5)
            nits=dovi_apply_display_trim(0,nits,displayTone[0].z);
        return display_finish_mapping(0,nits);
    }
    return display_map_encoded(0,
        canonical(yuv_to_rgb(y,chroma,firstModes),firstModes,hlgPeakNits.x));
}
float3 load_second(float2 uv,float2 displayUv) {
    if(doviSignalMeta[1].x>0.5 && (displayUv.x<doviTrimC[1].z || displayUv.y<doviTrimC[1].w ||
       displayUv.x>doviTrimD[1].x || displayUv.y>doviTrimD[1].y)) return 0.0;
    float y=secondY.SampleLevel(linearClamp,float3(uv,0),0);
    float2 chroma=secondUv.SampleLevel(linearClamp,float3(left_chroma_uv(secondUv,uv),0),0);
    if(doviSignalMeta[1].x>1.5) {
        float2 elUv=lerp(secondEnhancementRect.xy,secondEnhancementRect.zw,displayUv);
        float3 composed=dovi_compose_p7_fel(1,float3(y,chroma),float3(chroma_site_y(secondY,uv),chroma),
            float3(sample_el_y(secondElY,elUv,doviComposerScale[1].w>0.5),sample_el_uv(secondElUv,elUv,doviComposerScale[1].w>0.5)));
        float3 encoded=dovi_decode_reshaped(1,composed);
        if(displayOptions[1].x<0.5) return encoded;
        float3 nits=display_apply_hdr10plus(1,pq_to_nits(encoded));
        if(displayOptions[1].z>0.5)
            nits=dovi_apply_display_trim(1,nits,displayTone[1].z);
        return display_finish_mapping(1,nits);
    }
    if(doviSignalMeta[1].x>0.5) {
        float3 encoded=dovi_decode_single_layer(1,saturate(float3(y,chroma)*doviSignalMeta[1].w));
        if(displayOptions[1].x<0.5) return encoded;
        float3 nits=display_apply_hdr10plus(1,pq_to_nits(encoded));
        if(displayOptions[1].z>0.5)
            nits=dovi_apply_display_trim(1,nits,displayTone[1].z);
        return display_finish_mapping(1,nits);
    }
    return display_map_encoded(1,
        canonical(yuv_to_rgb(y,chroma,secondModes),secondModes,hlgPeakNits.y));
}
uint pack_half2(float first, float second) {
    return f32tof16(first) | (f32tof16(second) << 16);
}
void store_pair(uint plane, uint y, uint x,
                float firstValue, float secondValue) {
    uint element = (plane * outputSize.y + y) * outputSize.x + x;
    outputTensor.Store(element * 2, pack_half2(firstValue, secondValue));
}
void accumulate_scene_metrics(uint x, uint y, float3 left, float3 right) {
    if ((x & 15u) != 0u || (y & 15u) != 0u) return;
    float leftLuma = saturate(dot(left, float3(0.2627, 0.6780, 0.0593)));
    float rightLuma = saturate(dot(right, float3(0.2627, 0.6780, 0.0593)));
    float difference = abs(leftLuma - rightLuma);
    uint ignored;
    sceneMetrics.InterlockedAdd(0, (uint)(difference * 65535.0 + 0.5), ignored);
    sceneMetrics.InterlockedAdd(4, difference >= 0.12 ? 1u : 0u, ignored);
    sceneMetrics.InterlockedAdd(8, (uint)(leftLuma * 65535.0 + 0.5), ignored);
    sceneMetrics.InterlockedAdd(12, (uint)(rightLuma * 65535.0 + 0.5), ignored);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 2;
    if (x >= outputSize.x || id.y >= outputSize.y) return;
    uint x1 = min(x + 1, outputSize.x - 1);
    float2 normalized0 = (float2(x, id.y) + 0.5) / float2(outputSize);
    float2 normalized1 = (float2(x1, id.y) + 0.5) / float2(outputSize);
    float2 firstUv0 = lerp(firstSourceRect.xy, firstSourceRect.zw, normalized0);
    float2 firstUv1 = lerp(firstSourceRect.xy, firstSourceRect.zw, normalized1);
    float2 secondUv0 = lerp(secondSourceRect.xy, secondSourceRect.zw, normalized0);
    float2 secondUv1 = lerp(secondSourceRect.xy, secondSourceRect.zw, normalized1);
    float3 a0 = load_first(firstUv0,normalized0), a1 = load_first(firstUv1,normalized1);
    float3 b0 = load_second(secondUv0,normalized0), b1 = load_second(secondUv1,normalized1);
    accumulate_scene_metrics(x, id.y, a0, b0);
    [unroll] for (uint channel = 0; channel < 3; ++channel) {
        store_pair(channel, id.y, x, a0[channel], a1[channel]);
        store_pair(channel + 3, id.y, x, b0[channel], b1[channel]);
    }
    store_pair(6, id.y, x, interpolationT, interpolationT);
}
)";
    doviShaderSource_ = shader;
    const std::string baseShader = std::string(R"(
Texture2DArray<float> firstY : register(t0);
Texture2DArray<float2> firstUv : register(t1);
Texture2DArray<float> secondY : register(t2);
Texture2DArray<float2> secondUv : register(t3);
RWByteAddressBuffer outputTensor : register(u0);
RWByteAddressBuffer sceneMetrics : register(u1);
SamplerState linearClamp : register(s0);
cbuffer PreprocessConstants : register(b0) {
    float4 firstSourceRect;
    float4 secondSourceRect;
    float4 firstEnhancementRect;
    float4 secondEnhancementRect;
    uint4 firstModes;
    uint4 secondModes;
    uint2 outputSize;
    float2 hlgPeakNits;
    float interpolationT;
    float preprocessPadding;
};
cbuffer DisplayMappingConstants : register(b2) {
    float4 displayTone[2];
    float4 displayOptions[2];
    float4 displayHdr10PlusA[2];
    float4 displayHdr10PlusB[2];
    float4 displayHdr10PlusCurve[2][4];
    float4 displayHdrToneCurve[2][5];
};
float3 yuv_to_rgb(float y, float2 uv, uint4 modes) {
    bool full = modes.y != 0;
    bool tenBit = (modes.w & 1) != 0;
    float black = tenBit ? 64.0 / 1023.0 : 16.0 / 255.0;
    float lumaRange = tenBit ? 876.0 / 1023.0 : 219.0 / 255.0;
    float chromaRange = tenBit ? 896.0 / 1023.0 : 224.0 / 255.0;
    float yy = max(0.0, (y - (full ? 0.0 : black)) /
                        (full ? 1.0 : lumaRange));
    float cb = (uv.x - 0.5) / (full ? 1.0 : chromaRange);
    float cr = (uv.y - 0.5) / (full ? 1.0 : chromaRange);
    float kr = modes.x == 2 ? 0.2990 : (modes.x == 3 ? 0.2627 : 0.2126);
    float kb = modes.x == 2 ? 0.1140 : (modes.x == 3 ? 0.0593 : 0.0722);
    float kg = 1.0 - kr - kb;
    return max(float3(yy + (2.0 - 2.0 * kr) * cr,
                      yy - 2.0 * kb * (1.0 - kb) / kg * cb -
                           2.0 * kr * (1.0 - kr) / kg * cr,
                      yy + (2.0 - 2.0 * kb) * cb), 0.0);
}
float3 pq_to_nits(float3 v) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0,
                c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(v), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) /
                         max(c2 - c3 * p, 0.000001), 1.0 / m1);
}
float3 nits_to_pq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0,
                c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / max(1.0 + c3 * p, 0.000001), m2);
}
float3 hlg_to_nits(float3 v, float peak) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 scene = lerp((exp((v - c) / a) + b) / 12.0,
                        v * v / 3.0, step(v, 0.5));
    scene=max(scene,0.0);
    float sceneLuma=max(dot(scene,float3(0.2627,0.6780,0.0593)),0.000001);
    return scene*pow(sceneLuma,0.2)*max(peak,1000.0);
}
float3 rec709_to_rec2020(float3 v) {
    return mul(float3x3(0.6274, 0.3293, 0.0433,
                       0.0691, 0.9195, 0.0114,
                       0.0164, 0.0880, 0.8956), v);
}
float3 canonical(float3 encoded, uint4 modes, float hlgPeak) {
    if (modes.z == 1) return saturate(encoded);
    float3 nits;
    if (modes.z == 2) nits = pq_to_nits(encoded);
    else if (modes.z == 3) nits = hlg_to_nits(encoded, hlgPeak);
    else nits = pow(saturate(encoded), 2.2) * 80.0;
    if (modes.w < 2) nits = max(rec709_to_rec2020(nits), 0.0);
    return nits_to_pq(min(nits, 10000.0));
}
)") + displayMappingHlsl + R"(
uint pack_half2(float first, float second) {
    return f32tof16(first) | (f32tof16(second) << 16);
}
void store_pair(uint plane, uint y, uint x,
                float firstValue, float secondValue) {
    uint element = (plane * outputSize.y + y) * outputSize.x + x;
    outputTensor.Store(element * 2, pack_half2(firstValue, secondValue));
}
void accumulate_scene_metrics(uint x, uint y, float3 left, float3 right) {
    if ((x & 15u) != 0u || (y & 15u) != 0u) return;
    float leftLuma = saturate(dot(left, float3(0.2627, 0.6780, 0.0593)));
    float rightLuma = saturate(dot(right, float3(0.2627, 0.6780, 0.0593)));
    float difference = abs(leftLuma - rightLuma);
    uint ignored;
    sceneMetrics.InterlockedAdd(0, (uint)(difference * 65535.0 + 0.5), ignored);
    sceneMetrics.InterlockedAdd(4, difference >= 0.12 ? 1u : 0u, ignored);
    sceneMetrics.InterlockedAdd(8, (uint)(leftLuma * 65535.0 + 0.5), ignored);
    sceneMetrics.InterlockedAdd(12, (uint)(rightLuma * 65535.0 + 0.5), ignored);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 2;
    if (x >= outputSize.x || id.y >= outputSize.y) return;
    uint x1 = min(x + 1, outputSize.x - 1);
    float2 p0 = (float2(x, id.y) + 0.5) / float2(outputSize);
    float2 p1 = (float2(x1, id.y) + 0.5) / float2(outputSize);
    float2 firstUv0 = lerp(firstSourceRect.xy, firstSourceRect.zw, p0);
    float2 firstUv1 = lerp(firstSourceRect.xy, firstSourceRect.zw, p1);
    float2 secondUv0 = lerp(secondSourceRect.xy, secondSourceRect.zw, p0);
    float2 secondUv1 = lerp(secondSourceRect.xy, secondSourceRect.zw, p1);
    float3 a0 = display_map_encoded(0,canonical(yuv_to_rgb(
        firstY.SampleLevel(linearClamp, float3(firstUv0, 0), 0),
        firstUv.SampleLevel(linearClamp, float3(firstUv0, 0), 0), firstModes),
        firstModes, hlgPeakNits.x));
    float3 a1 = display_map_encoded(0,canonical(yuv_to_rgb(
        firstY.SampleLevel(linearClamp, float3(firstUv1, 0), 0),
        firstUv.SampleLevel(linearClamp, float3(firstUv1, 0), 0), firstModes),
        firstModes, hlgPeakNits.x));
    float3 b0 = display_map_encoded(1,canonical(yuv_to_rgb(
        secondY.SampleLevel(linearClamp, float3(secondUv0, 0), 0),
        secondUv.SampleLevel(linearClamp, float3(secondUv0, 0), 0), secondModes),
        secondModes, hlgPeakNits.y));
    float3 b1 = display_map_encoded(1,canonical(yuv_to_rgb(
        secondY.SampleLevel(linearClamp, float3(secondUv1, 0), 0),
        secondUv.SampleLevel(linearClamp, float3(secondUv1, 0), 0), secondModes),
        secondModes, hlgPeakNits.y));
    accumulate_scene_metrics(x, id.y, a0, b0);
    [unroll] for (uint channel = 0; channel < 3; ++channel) {
        store_pair(channel, id.y, x, a0[channel], a1[channel]);
        store_pair(channel + 3, id.y, x, b0[channel], b1[channel]);
    }
    store_pair(6, id.y, x, interpolationT, interpolationT);
}
)";
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    if (FAILED(D3DCompile(baseShader.data(), baseShader.size(), nullptr, nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                          &bytecode, &errors))) {
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = rootSignature_.Get();
    pipelineDesc.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
    if (FAILED(device_->CreateComputePipelineState(&pipelineDesc,
                                                    IID_PPV_ARGS(&pipeline_)))) {
        return false;
    }

    const std::string scRgbShader = std::string(R"(
Texture2D<float4> firstRgb : register(t0);
Texture2D<float4> secondRgb : register(t2);
RWByteAddressBuffer outputTensor : register(u0);
RWByteAddressBuffer sceneMetrics : register(u1);
SamplerState linearClamp : register(s0);
cbuffer PreprocessConstants : register(b0) {
    float4 firstSourceRect;
    float4 secondSourceRect;
    float4 firstEnhancementRect;
    float4 secondEnhancementRect;
    uint4 firstModes;
    uint4 secondModes;
    uint2 outputSize;
    float2 hlgPeakNits;
    float interpolationT;
    float preprocessPadding;
};
cbuffer DisplayMappingConstants : register(b2) {
    float4 displayTone[2];
    float4 displayOptions[2];
    float4 displayHdr10PlusA[2];
    float4 displayHdr10PlusB[2];
    float4 displayHdr10PlusCurve[2][4];
    float4 displayHdrToneCurve[2][5];
};
float3 rec709_to_rec2020(float3 value) {
    return mul(float3x3(0.6274,0.3293,0.0433,
                       0.0691,0.9195,0.0114,
                       0.0164,0.0880,0.8956),value);
}
float3 nits_to_pq(float3 nits) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 p=pow(max(nits/10000.0,0.0),m1);
    return pow((c1+c2*p)/max(1.0+c3*p,0.000001),m2);
}
float3 pq_to_nits(float3 v) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    float3 p=pow(saturate(v),1.0/m2);
    return 10000.0*pow(max(p-c1,0.0)/max(c2-c3*p,0.000001),1.0/m1);
}
float3 canonical(float3 sampledRgb,uint mode) {
    if(mode==1||mode==2) return saturate(sampledRgb);
    float3 scRgb=max(sampledRgb,0.0);
    return nits_to_pq(max(rec709_to_rec2020(scRgb*80.0),0.0));
}
)") + displayMappingHlsl + R"(
uint pack_half2(float first,float second) {
    return f32tof16(first)|(f32tof16(second)<<16);
}
void store_pair(uint plane,uint y,uint x,float firstValue,float secondValue) {
    uint element=(plane*outputSize.y+y)*outputSize.x+x;
    outputTensor.Store(element*2,pack_half2(firstValue,secondValue));
}
void accumulate_scene_metrics(uint x,uint y,float3 left,float3 right) {
    if((x&15u)!=0u||(y&15u)!=0u) return;
    float leftLuma=saturate(dot(left,float3(0.2627,0.6780,0.0593)));
    float rightLuma=saturate(dot(right,float3(0.2627,0.6780,0.0593)));
    float difference=abs(leftLuma-rightLuma);
    uint ignored;
    sceneMetrics.InterlockedAdd(0,(uint)(difference*65535.0+0.5),ignored);
    sceneMetrics.InterlockedAdd(4,difference>=0.12?1u:0u,ignored);
    sceneMetrics.InterlockedAdd(8,(uint)(leftLuma*65535.0+0.5),ignored);
    sceneMetrics.InterlockedAdd(12,(uint)(rightLuma*65535.0+0.5),ignored);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x=id.x*2;
    if(x>=outputSize.x||id.y>=outputSize.y) return;
    uint x1=min(x+1,outputSize.x-1);
    float2 p0=(float2(x,id.y)+0.5)/float2(outputSize);
    float2 p1=(float2(x1,id.y)+0.5)/float2(outputSize);
    float3 a0=display_map_encoded(0,
        canonical(firstRgb.SampleLevel(linearClamp,p0,0).rgb,firstModes.z));
    float3 a1=display_map_encoded(0,
        canonical(firstRgb.SampleLevel(linearClamp,p1,0).rgb,firstModes.z));
    float3 b0=display_map_encoded(1,
        canonical(secondRgb.SampleLevel(linearClamp,p0,0).rgb,secondModes.z));
    float3 b1=display_map_encoded(1,
        canonical(secondRgb.SampleLevel(linearClamp,p1,0).rgb,secondModes.z));
    accumulate_scene_metrics(x,id.y,a0,b0);
    [unroll] for(uint channel=0;channel<3;++channel) {
        store_pair(channel,id.y,x,a0[channel],a1[channel]);
        store_pair(channel+3,id.y,x,b0[channel],b1[channel]);
    }
    store_pair(6,id.y,x,interpolationT,interpolationT);
}
)";
    bytecode.Reset();
    errors.Reset();
    if (FAILED(D3DCompile(scRgbShader.data(), scRgbShader.size(), nullptr, nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                          &bytecode, &errors))) {
        return false;
    }
    pipelineDesc.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
    return SUCCEEDED(device_->CreateComputePipelineState(
        &pipelineDesc, IID_PPV_ARGS(&scRgbPipeline_)));
}

bool D3D12TensorPreprocessor::InitializeDolbyVisionPipeline() {
    if (!device_ || !rootSignature_ || doviShaderSource_.empty()) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(doviShaderSource_.data(), doviShaderSource_.size(), nullptr,
                          nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &bytecode, &errors))) {
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = rootSignature_.Get();
    pipelineDesc.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device_->CreateComputePipelineState(&pipelineDesc,
                                                    IID_PPV_ARGS(&pipeline)))) {
        return false;
    }
    doviPipeline_ = std::move(pipeline);
    publishedDoviPipeline_.store(doviPipeline_.Get(), std::memory_order_release);
    return true;
}

void D3D12TensorPreprocessor::StartDolbyVisionInitialization() {
    if (doviInitializationStarted_.exchange(true, std::memory_order_acq_rel) ||
        !device_ || !rootSignature_ || doviShaderSource_.empty()) {
        return;
    }
    try {
        doviInitializationThread_ = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            InitializeDolbyVisionPipeline();
            doviInitializationComplete_.store(true, std::memory_order_release);
        });
    } catch (...) {
        doviInitializationStarted_.store(false, std::memory_order_release);
        doviInitializationComplete_.store(true, std::memory_order_release);
    }
}

bool D3D12TensorPreprocessor::EnsureOutput(Slot& slot, const std::size_t bytes) {
    if (slot.output && slot.outputBytes >= bytes) return true;
    slot.output.Reset();
    slot.outputBytes = 0;
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
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&slot.output)))) {
        return false;
    }
    slot.outputBytes = bytes;
    return true;
}

D3D12TensorPreprocessor::Slot* D3D12TensorPreprocessor::AcquireSlot() {
    for (Slot& slot : slots_) {
        if (!slot.retained && (!slot.completion.IsValid() ||
            slot.completion.fence->GetCompletedValue() >= slot.completion.value)) {
            slot.completion = {};
            slot.firstTexture.Reset();
            slot.secondTexture.Reset();
            slot.firstEnhancementTexture.Reset();
            slot.secondEnhancementTexture.Reset();
            slot.firstFrameRef.reset();
            slot.secondFrameRef.reset();
            slot.firstEnhancementFrameRef.reset();
            slot.secondEnhancementFrameRef.reset();
            return &slot;
        }
    }
    return nullptr;
}

bool D3D12TensorPreprocessor::RetainUntil(ID3D12Resource* tensor,
                                         const GpuFencePoint& completion) {
    if (!tensor || !completion.IsValid()) return false;
    for (Slot& slot : slots_) {
        if (slot.output.Get() == tensor) {
            slot.completion = completion;
            slot.retained = true;
            return true;
        }
    }
    return false;
}

bool D3D12TensorPreprocessor::ReleaseRetained(
    ID3D12Resource* tensor,
    const GpuFencePoint& completion) {
    if (!tensor) return false;
    for (Slot& slot : slots_) {
        if (slot.output.Get() == tensor) {
            slot.completion = completion;
            slot.retained = false;
            return true;
        }
    }
    return false;
}

TensorPreprocessResult D3D12TensorPreprocessor::SubmitPair(
    const NativeVideoFrame& first,
    const NativeVideoFrame& second,
    const TensorShape outputShape,
    const float interpolationT,
    const float hlgPeakNits,
    const InterpolationDisplayMappingPair& displayMapping,
    const uint64_t epoch,
    const GpuFencePoint& orderingDependency) {
    TensorPreprocessResult result;
    if (!device_ || !frameGraph_ || !pipeline_) {
        result.reason = L"tensor_preprocessor_not_initialized";
        return result;
    }
    if (!IsSupportedFrame(first) || !IsSupportedFrame(second)) {
        result.reason = L"unsupported_decode_surface";
        return result;
    }
    const bool requiresDolbyVisionPipeline =
        (first.dovi && first.dovi->valid) ||
        (first.enhancementDovi && first.enhancementDovi->valid) ||
        first.HasEnhancementD3D12Texture() ||
        (second.dovi && second.dovi->valid) ||
        (second.enhancementDovi && second.enhancementDovi->valid) ||
        second.HasEnhancementD3D12Texture();
    ID3D12PipelineState* preprocessPipeline = pipeline_.Get();
    if (requiresDolbyVisionPipeline) {
        preprocessPipeline =
            publishedDoviPipeline_.load(std::memory_order_acquire);
        if (!preprocessPipeline) {
            StartDolbyVisionInitialization();
            result.reason = doviInitializationComplete_.load(
                                std::memory_order_acquire)
                ? L"dolby_vision_tensor_pipeline_unavailable"
                : L"dolby_vision_tensor_pipeline_initializing";
            return result;
        }
    }
    const TensorShape shape = outputShape;
    if (!shape.IsValid()) {
        result.reason = L"no_tensor_shape_bucket";
        return result;
    }
    Slot* slot = AcquireSlot();
    if (!slot) {
        result.reason = L"tensor_preprocess_gpu_busy";
        return result;
    }
    constexpr std::size_t batch = 1;
    constexpr std::size_t channels = 7;
    constexpr std::size_t fp16Bytes = 2;
    const std::size_t bytes = batch * channels * shape.width * shape.height * fp16Bytes;
    if (!EnsureOutput(*slot, bytes) || FAILED(slot->allocator->Reset()) ||
        FAILED(slot->commandList->Reset(slot->allocator.Get(), preprocessPipeline))) {
        result.reason = L"tensor_preprocess_resource_reset_failed";
        return result;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto createPlaneViews = [&](const NativeVideoFrame& frame) {
        const D3D12_RESOURCE_DESC sourceDesc = frame.d3d12Texture->GetDesc();
        const UINT arraySize = std::max<UINT>(1, sourceDesc.DepthOrArraySize);
        const UINT arraySlice = std::min(frame.d3d12Subresource, arraySize - 1);
        const bool highBitDepth = frame.d3dFormat == DXGI_FORMAT_P010 ||
                                  frame.d3dFormat == DXGI_FORMAT_P016;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.FirstArraySlice = arraySlice;
        view.Texture2DArray.ArraySize = 1;
        view.Texture2DArray.PlaneSlice = 0;
        view.Format = highBitDepth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        device_->CreateShaderResourceView(frame.d3d12Texture.Get(), &view, cpu);
        cpu.ptr += descriptorIncrement_;
        view.Texture2DArray.PlaneSlice = 1;
        view.Format = highBitDepth ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        device_->CreateShaderResourceView(frame.d3d12Texture.Get(), &view, cpu);
        cpu.ptr += descriptorIncrement_;
    };
    createPlaneViews(first);
    createPlaneViews(second);
    const NativeVideoFrame& firstEnhancement = first.HasEnhancementD3D12Texture()
        ? *first.enhancementFrame : first;
    const NativeVideoFrame& secondEnhancement = second.HasEnhancementD3D12Texture()
        ? *second.enhancementFrame : second;
    createPlaneViews(firstEnhancement);
    createPlaneViews(secondEnhancement);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(bytes / sizeof(UINT));
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(slot->output.Get(), nullptr, &uav, cpu);
    cpu.ptr += descriptorIncrement_;
    CreateSceneChangeDescriptor(*slot, cpu);

    const auto planeSubresources = [](const NativeVideoFrame& frame) {
        const UINT arraySize = std::max<UINT>(1, frame.d3d12Texture->GetDesc().DepthOrArraySize);
        const UINT slice = std::min(frame.d3d12Subresource, arraySize - 1);
        return std::array<UINT, 2>{slice, slice + arraySize};
    };
    const auto firstPlanes = planeSubresources(first);
    const auto secondPlanes = planeSubresources(second);
    std::vector<D3D12_RESOURCE_BARRIER> beginBarriers{
        TransitionBarrier(first.d3d12Texture.Get(), firstPlanes[0], D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(first.d3d12Texture.Get(), firstPlanes[1], D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(second.d3d12Texture.Get(), secondPlanes[0], D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(second.d3d12Texture.Get(), secondPlanes[1], D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    const auto addEnhancementBarriers = [&](const NativeVideoFrame& owner,
                                            const NativeVideoFrame& enhancementFrame) {
        if (!owner.HasEnhancementD3D12Texture()) return;
        const auto planes = planeSubresources(enhancementFrame);
        beginBarriers.push_back(TransitionBarrier(
            enhancementFrame.d3d12Texture.Get(), planes[0], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        beginBarriers.push_back(TransitionBarrier(
            enhancementFrame.d3d12Texture.Get(), planes[1], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    };
    addEnhancementBarriers(first, firstEnhancement);
    addEnhancementBarriers(second, secondEnhancement);
    slot->commandList->ResourceBarrier(static_cast<UINT>(beginBarriers.size()),
                                       beginBarriers.data());
    slot->commandList->SetComputeRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {slot->descriptors.Get()};
    slot->commandList->SetDescriptorHeaps(1, heaps);
    ClearSceneChangeProbe(*slot);
    slot->commandList->SetComputeRootDescriptorTable(
        0, slot->descriptors->GetGPUDescriptorHandleForHeapStart());

    PreprocessConstants constants{};
    const auto fillFrameConstants = [](const NativeVideoFrame& frame,
                                       float (&uv)[4], UINT (&modes)[4]) {
        uv[0] = frame.sourceUvRect.left;
        uv[1] = frame.sourceUvRect.top;
        uv[2] = frame.sourceUvRect.right;
        uv[3] = frame.sourceUvRect.bottom;
        modes[0] = MatrixMode(frame.color.matrix);
        modes[1] = frame.color.range == VideoColorRange::Full ? 1u : 0u;
        modes[2] = TransferMode(frame.color.transfer);
        const UINT highBitDepth =
            frame.d3dFormat == DXGI_FORMAT_P010 || frame.d3dFormat == DXGI_FORMAT_P016
                ? 1u : 0u;
        const UINT bt2020 = frame.color.primaries == VideoColorPrimaries::Bt2020 ? 2u : 0u;
        modes[3] = highBitDepth | bt2020;
    };
    fillFrameConstants(first, constants.firstSourceUv, constants.firstModes);
    fillFrameConstants(second, constants.secondSourceUv, constants.secondModes);
    const auto fillUv = [](const NativeVideoFrame& frame, float (&uv)[4]) {
        uv[0] = frame.sourceUvRect.left;
        uv[1] = frame.sourceUvRect.top;
        uv[2] = frame.sourceUvRect.right;
        uv[3] = frame.sourceUvRect.bottom;
    };
    fillUv(firstEnhancement, constants.firstEnhancementUv);
    fillUv(secondEnhancement, constants.secondEnhancementUv);
    constants.outputWidth = shape.width;
    constants.outputHeight = shape.height;
    // HLG's OOTF is display-dependent. Match the original-frame shader's
    // active display target before converting both endpoints to BT.2020/PQ.
    // Using mastering metadata or a fixed 1000-nit value here made original
    // and generated frames alternate in brightness on high-peak displays.
    const float effectiveHlgPeakNits = std::clamp(hlgPeakNits, 1000.0f, 10000.0f);
    constants.firstHlgPeakNits = effectiveHlgPeakNits;
    constants.secondHlgPeakNits = effectiveHlgPeakNits;
    constants.interpolationT = std::clamp(interpolationT, 0.0f, 1.0f);
    slot->commandList->SetComputeRoot32BitConstants(1, 30, &constants, 0);
    DoviShaderConstantsPair doviConstants{};
    FillDoviShaderConstants(
        doviConstants, 0, first, std::max(displayMapping.tone[0][2], 100.0f));
    FillDoviShaderConstants(
        doviConstants, 1, second, std::max(displayMapping.tone[1][2], 100.0f));
    std::memcpy(slot->doviConstantsMapping, &doviConstants, sizeof(doviConstants));
    slot->commandList->SetComputeRootConstantBufferView(
        2, slot->doviConstants->GetGPUVirtualAddress());
    std::memcpy(slot->displayConstantsMapping, &displayMapping,
                sizeof(displayMapping));
    slot->commandList->SetComputeRootConstantBufferView(
        3, slot->displayConstants->GetGPUVirtualAddress());
    slot->commandList->Dispatch((shape.width + 15) / 16, (shape.height + 7) / 8, 1);
    CopySceneChangeProbe(*slot);

    std::vector<D3D12_RESOURCE_BARRIER> endBarriers{
        TransitionBarrier(first.d3d12Texture.Get(), firstPlanes[0],
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(first.d3d12Texture.Get(), firstPlanes[1],
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(second.d3d12Texture.Get(), secondPlanes[0],
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(second.d3d12Texture.Get(), secondPlanes[1],
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON)
    };
    const auto addEnhancementEndBarriers = [&](const NativeVideoFrame& owner,
                                               const NativeVideoFrame& enhancementFrame) {
        if (!owner.HasEnhancementD3D12Texture()) return;
        const auto planes = planeSubresources(enhancementFrame);
        endBarriers.push_back(TransitionBarrier(
            enhancementFrame.d3d12Texture.Get(), planes[0],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
        endBarriers.push_back(TransitionBarrier(
            enhancementFrame.d3d12Texture.Get(), planes[1],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
    };
    addEnhancementEndBarriers(first, firstEnhancement);
    addEnhancementEndBarriers(second, secondEnhancement);
    slot->commandList->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
    if (FAILED(slot->commandList->Close())) {
        result.reason = L"tensor_preprocess_command_close_failed";
        return result;
    }

    std::array<GpuFencePoint, 5> dependencies{};
    dependencies[0].fence = first.d3d12ReadyFence;
    dependencies[0].value = first.d3d12ReadyFenceValue;
    dependencies[1].fence = second.d3d12ReadyFence;
    dependencies[1].value = second.d3d12ReadyFenceValue;
    if (first.HasEnhancementD3D12Texture()) {
        dependencies[2].fence = firstEnhancement.d3d12ReadyFence;
        dependencies[2].value = firstEnhancement.d3d12ReadyFenceValue;
    }
    if (second.HasEnhancementD3D12Texture()) {
        dependencies[3].fence = secondEnhancement.d3d12ReadyFence;
        dependencies[3].value = secondEnhancement.d3d12ReadyFenceValue;
    }
    dependencies[4] = orderingDependency;
    SubmitResult submit = frameGraph_->Submit(GpuFrameQueue::ComputeMl,
                                               slot->commandList.Get(), dependencies);
    if (!submit.accepted) {
        result.reason = std::move(submit.reason);
        return result;
    }
    slot->completion = submit.completion;
    slot->firstTexture = first.d3d12Texture;
    slot->secondTexture = second.d3d12Texture;
    if (first.HasEnhancementD3D12Texture()) {
        slot->firstEnhancementTexture = firstEnhancement.d3d12Texture;
        slot->firstEnhancementFrameRef = firstEnhancement.hardwareFrameRef;
    }
    if (second.HasEnhancementD3D12Texture()) {
        slot->secondEnhancementTexture = secondEnhancement.d3d12Texture;
        slot->secondEnhancementFrameRef = secondEnhancement.hardwareFrameRef;
    }
    slot->firstFrameRef = first.hardwareFrameRef;
    slot->secondFrameRef = second.hardwareFrameRef;

    result.accepted = true;
    result.shape = shape;
    result.tensor.pts = second.pts;
    result.tensor.epoch = epoch;
    result.tensor.resource = slot->output;
    result.tensor.producingQueue = GpuFrameQueue::ComputeMl;
    result.tensor.ready = submit.completion;
    result.tensor.color = second.color;
    result.sceneChange.readback = slot->sceneMetricsReadback;
    result.sceneChange.ready = submit.completion;
    result.sceneChange.sampleCount =
        ((shape.width + 15) / 16) * ((shape.height + 15) / 16);
    return result;
}

TensorPreprocessResult D3D12TensorPreprocessor::SubmitScRgbPair(
    ID3D12Resource* first,
    ID3D12Resource* second,
    const TensorShape outputShape,
    const float interpolationT,
    const uint64_t epoch,
    const GpuFencePoint& orderingDependency) {
    TensorPreprocessResult result;
    if (!device_ || !frameGraph_ || !scRgbPipeline_) {
        result.reason = L"scrgb_tensor_preprocessor_not_initialized";
        return result;
    }
    if (!first || !second) {
        result.reason = L"scrgb_tensor_missing_input";
        return result;
    }
    const D3D12_RESOURCE_DESC firstDesc = first->GetDesc();
    const D3D12_RESOURCE_DESC secondDesc = second->GetDesc();
    if (firstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        secondDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        firstDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        secondDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        firstDesc.Width != secondDesc.Width || firstDesc.Height != secondDesc.Height ||
        firstDesc.Width > std::numeric_limits<UINT>::max()) {
        result.reason = L"unsupported_scrgb_tensor_input";
        return result;
    }
    const TensorShape shape = outputShape;
    if (!shape.IsValid()) {
        result.reason = L"invalid_scrgb_tensor_shape";
        return result;
    }
    Slot* slot = AcquireSlot();
    if (!slot) {
        result.reason = L"tensor_preprocess_gpu_busy";
        return result;
    }
    constexpr std::size_t channels = 7;
    constexpr std::size_t fp16Bytes = 2;
    const std::size_t bytes = channels * shape.width * shape.height * fp16Bytes;
    if (!EnsureOutput(*slot, bytes) || FAILED(slot->allocator->Reset()) ||
        FAILED(slot->commandList->Reset(slot->allocator.Get(), scRgbPipeline_.Get()))) {
        result.reason = L"scrgb_tensor_resource_reset_failed";
        return result;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < 8; ++index) {
        device_->CreateShaderResourceView(nullptr, &srv, cpu);
        cpu.ptr += descriptorIncrement_;
    }
    cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(first, &srv, cpu);
    cpu.ptr += static_cast<SIZE_T>(2) * descriptorIncrement_;
    device_->CreateShaderResourceView(second, &srv, cpu);
    cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(8) * descriptorIncrement_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(bytes / sizeof(UINT));
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(slot->output.Get(), nullptr, &uav, cpu);
    cpu.ptr += descriptorIncrement_;
    CreateSceneChangeDescriptor(*slot, cpu);

    const std::array beginBarriers{
        TransitionBarrier(first, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(second, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    slot->commandList->ResourceBarrier(static_cast<UINT>(beginBarriers.size()),
                                       beginBarriers.data());
    slot->commandList->SetComputeRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {slot->descriptors.Get()};
    slot->commandList->SetDescriptorHeaps(1, heaps);
    ClearSceneChangeProbe(*slot);
    slot->commandList->SetComputeRootDescriptorTable(
        0, slot->descriptors->GetGPUDescriptorHandleForHeapStart());
    PreprocessConstants constants{};
    constants.firstSourceUv[2] = 1.0f;
    constants.firstSourceUv[3] = 1.0f;
    constants.secondSourceUv[2] = 1.0f;
    constants.secondSourceUv[3] = 1.0f;
    constants.firstModes[2] = 4;
    constants.secondModes[2] = 4;
    constants.outputWidth = shape.width;
    constants.outputHeight = shape.height;
    constants.interpolationT = std::clamp(interpolationT, 0.0f, 1.0f);
    slot->commandList->SetComputeRoot32BitConstants(1, 30, &constants, 0);
    slot->commandList->SetComputeRootConstantBufferView(
        2, slot->doviConstants->GetGPUVirtualAddress());
    const InterpolationDisplayMappingPair displayMapping{};
    std::memcpy(slot->displayConstantsMapping, &displayMapping,
                sizeof(displayMapping));
    slot->commandList->SetComputeRootConstantBufferView(
        3, slot->displayConstants->GetGPUVirtualAddress());
    slot->commandList->Dispatch((shape.width + 15) / 16,
                                (shape.height + 7) / 8, 1);
    CopySceneChangeProbe(*slot);
    const std::array endBarriers{
        TransitionBarrier(first, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(second, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON)};
    slot->commandList->ResourceBarrier(static_cast<UINT>(endBarriers.size()),
                                       endBarriers.data());
    if (FAILED(slot->commandList->Close())) {
        result.reason = L"scrgb_tensor_command_close_failed";
        return result;
    }
    const std::array dependencies{orderingDependency};
    SubmitResult submit = frameGraph_->Submit(
        GpuFrameQueue::ComputeMl, slot->commandList.Get(), dependencies);
    if (!submit.accepted) {
        result.reason = std::move(submit.reason);
        return result;
    }
    slot->completion = submit.completion;
    slot->firstTexture = first;
    slot->secondTexture = second;
    result.accepted = true;
    result.shape = shape;
    result.tensor.epoch = epoch;
    result.tensor.resource = slot->output;
    result.tensor.producingQueue = GpuFrameQueue::ComputeMl;
    result.tensor.ready = submit.completion;
    result.sceneChange.readback = slot->sceneMetricsReadback;
    result.sceneChange.ready = submit.completion;
    result.sceneChange.sampleCount =
        ((shape.width + 15) / 16) * ((shape.height + 15) / 16);
    return result;
}

TensorPreprocessResult D3D12TensorPreprocessor::SubmitPixelPair(
    const NativeVideoFrame& first,
    const NativeVideoFrame& second,
    const TensorShape outputShape,
    const float interpolationT,
    const InterpolationDisplayMappingPair& displayMapping,
    const uint64_t epoch,
    const GpuFencePoint& orderingDependency) {
    TensorPreprocessResult result;
    if (!device_ || !frameGraph_ || !scRgbPipeline_) {
        result.reason = L"rgb_tensor_preprocessor_not_initialized";
        return result;
    }
    const auto supported = [](const NativeVideoFrame& frame) {
        return frame.HasPixels() && frame.stride >= frame.width * 4 &&
            (frame.softwareFormat == AV_PIX_FMT_X2BGR10LE ||
             frame.softwareFormat == AV_PIX_FMT_BGRA ||
             frame.softwareFormat == AV_PIX_FMT_NONE);
    };
    if (!supported(first) || !supported(second)) {
        result.reason = L"unsupported_rgb_tensor_input";
        return result;
    }
    // Pixel-backed and decode-surface inputs share one resolution contract.
    // Presentation keeps using the original full-resolution frame.
    const TensorShape shape = outputShape;
    if (!shape.IsValid()) {
        result.reason = L"no_tensor_shape_bucket";
        return result;
    }
    Slot* slot = AcquireSlot();
    if (!slot) {
        result.reason = L"tensor_preprocess_gpu_busy";
        return result;
    }
    constexpr std::size_t channels = 7;
    constexpr std::size_t fp16Bytes = 2;
    const std::size_t bytes = channels * shape.width * shape.height * fp16Bytes;
    if (!EnsureOutput(*slot, bytes) || FAILED(slot->allocator->Reset()) ||
        FAILED(slot->commandList->Reset(slot->allocator.Get(), scRgbPipeline_.Get()))) {
        result.reason = L"rgb_tensor_resource_reset_failed";
        return result;
    }

    struct UploadedInput {
        ID3D12Resource* texture = nullptr;
        ID3D12Resource* upload = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool textureCreated = false;
    };
    const auto prepareInput = [&](const NativeVideoFrame& frame,
                                  Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                                  Microsoft::WRL::ComPtr<ID3D12Resource>& upload,
                                  UINT64& uploadCapacity,
                                  UploadedInput& prepared) {
        const DXGI_FORMAT format = frame.softwareFormat == AV_PIX_FMT_X2BGR10LE
            ? DXGI_FORMAT_R10G10B10A2_UNORM
            : DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = shape.width;
        textureDesc.Height = shape.height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        bool createTexture = true;
        if (texture) {
            const D3D12_RESOURCE_DESC existing = texture->GetDesc();
            createTexture = existing.Width != textureDesc.Width ||
                existing.Height != textureDesc.Height || existing.Format != format;
        }
        if (createTexture) {
            texture.Reset();
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device_->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&texture)))) {
                return false;
            }
        }
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 uploadBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        device_->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint,
                                       &rowCount, &rowSize, &uploadBytes);
        if (!upload || uploadCapacity < uploadBytes) {
            upload.Reset();
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC uploadDesc{};
            uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDesc.Width = uploadBytes;
            uploadDesc.Height = 1;
            uploadDesc.DepthOrArraySize = 1;
            uploadDesc.MipLevels = 1;
            uploadDesc.SampleDesc.Count = 1;
            uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device_->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&upload)))) {
                return false;
            }
            uploadCapacity = uploadBytes;
        }
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(upload->Map(0, &noRead, &mapped))) return false;
        const auto* source = frame.bgra->data();
        auto* destination = static_cast<std::uint8_t*>(mapped) + footprint.Offset;
        for (UINT row = 0; row < rowCount; ++row) {
            auto* destinationRow = destination +
                static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
            const std::size_t sourceY = static_cast<std::size_t>(row) *
                static_cast<std::size_t>(frame.height) / std::max<UINT>(1, rowCount);
            const auto* sourceRow = source + sourceY * static_cast<std::size_t>(frame.stride);
            for (UINT x = 0; x < shape.width; ++x) {
                const std::size_t sourceX = static_cast<std::size_t>(x) *
                    static_cast<std::size_t>(frame.width) / std::max<UINT>(1, shape.width);
                std::memcpy(destinationRow + static_cast<std::size_t>(x) * 4,
                            sourceRow + sourceX * 4, 4);
            }
        }
        upload->Unmap(0, nullptr);
        prepared.texture = texture.Get();
        prepared.upload = upload.Get();
        prepared.footprint = footprint;
        prepared.format = format;
        prepared.textureCreated = createTexture;
        return true;
    };

    UploadedInput firstInput;
    UploadedInput secondInput;
    if (!prepareInput(first, slot->ownedFirstRgbTexture, slot->firstRgbUpload,
                      slot->firstRgbUploadCapacity, firstInput) ||
        !prepareInput(second, slot->ownedSecondRgbTexture, slot->secondRgbUpload,
                      slot->secondRgbUploadCapacity, secondInput)) {
        result.reason = L"rgb_tensor_upload_allocation_failed";
        return result;
    }
    std::vector<D3D12_RESOURCE_BARRIER> uploadBarriers;
    if (!firstInput.textureCreated) {
        uploadBarriers.push_back(TransitionBarrier(
            firstInput.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    }
    if (!secondInput.textureCreated) {
        uploadBarriers.push_back(TransitionBarrier(
            secondInput.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    }
    if (!uploadBarriers.empty()) {
        slot->commandList->ResourceBarrier(static_cast<UINT>(uploadBarriers.size()),
                                           uploadBarriers.data());
    }
    const auto copyInput = [&](const UploadedInput& input) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = input.texture;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = input.upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = input.footprint;
        slot->commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    };
    copyInput(firstInput);
    copyInput(secondInput);
    const std::array readBarriers{
        TransitionBarrier(firstInput.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(secondInput.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    slot->commandList->ResourceBarrier(static_cast<UINT>(readBarriers.size()),
                                       readBarriers.data());

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < 8; ++index) {
        device_->CreateShaderResourceView(nullptr, &nullSrv, cpu);
        cpu.ptr += descriptorIncrement_;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = nullSrv;
    srv.Format = firstInput.format;
    cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(firstInput.texture, &srv, cpu);
    srv.Format = secondInput.format;
    cpu.ptr += static_cast<SIZE_T>(2) * descriptorIncrement_;
    device_->CreateShaderResourceView(secondInput.texture, &srv, cpu);
    cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(8) * descriptorIncrement_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(bytes / sizeof(UINT));
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(slot->output.Get(), nullptr, &uav, cpu);
    cpu.ptr += descriptorIncrement_;
    CreateSceneChangeDescriptor(*slot, cpu);

    slot->commandList->SetComputeRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* heaps[] = {slot->descriptors.Get()};
    slot->commandList->SetDescriptorHeaps(1, heaps);
    ClearSceneChangeProbe(*slot);
    slot->commandList->SetComputeRootDescriptorTable(
        0, slot->descriptors->GetGPUDescriptorHandleForHeapStart());
    PreprocessConstants constants{};
    constants.firstSourceUv[2] = 1.0f;
    constants.firstSourceUv[3] = 1.0f;
    constants.secondSourceUv[2] = 1.0f;
    constants.secondSourceUv[3] = 1.0f;
    constants.firstModes[2] = first.softwareFormat == AV_PIX_FMT_X2BGR10LE ? 2u : 1u;
    constants.secondModes[2] = second.softwareFormat == AV_PIX_FMT_X2BGR10LE ? 2u : 1u;
    constants.outputWidth = shape.width;
    constants.outputHeight = shape.height;
    constants.interpolationT = std::clamp(interpolationT, 0.0f, 1.0f);
    slot->commandList->SetComputeRoot32BitConstants(1, 30, &constants, 0);
    slot->commandList->SetComputeRootConstantBufferView(
        2, slot->doviConstants->GetGPUVirtualAddress());
    std::memcpy(slot->displayConstantsMapping, &displayMapping,
                sizeof(displayMapping));
    slot->commandList->SetComputeRootConstantBufferView(
        3, slot->displayConstants->GetGPUVirtualAddress());
    slot->commandList->Dispatch((shape.width + 15) / 16,
                                (shape.height + 7) / 8, 1);
    CopySceneChangeProbe(*slot);
    const std::array endBarriers{
        TransitionBarrier(firstInput.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON),
        TransitionBarrier(secondInput.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COMMON)};
    slot->commandList->ResourceBarrier(static_cast<UINT>(endBarriers.size()),
                                       endBarriers.data());
    if (FAILED(slot->commandList->Close())) {
        result.reason = L"rgb_tensor_command_close_failed";
        return result;
    }
    const std::array dependencies{orderingDependency};
    SubmitResult submit = frameGraph_->Submit(
        GpuFrameQueue::ComputeMl, slot->commandList.Get(), dependencies);
    if (!submit.accepted) {
        result.reason = std::move(submit.reason);
        return result;
    }
    slot->completion = submit.completion;
    slot->firstTexture = slot->ownedFirstRgbTexture;
    slot->secondTexture = slot->ownedSecondRgbTexture;
    result.accepted = true;
    result.shape = shape;
    result.tensor.pts = second.pts;
    result.tensor.epoch = epoch;
    result.tensor.resource = slot->output;
    result.tensor.producingQueue = GpuFrameQueue::ComputeMl;
    result.tensor.ready = submit.completion;
    result.tensor.color = second.color;
    result.sceneChange.readback = slot->sceneMetricsReadback;
    result.sceneChange.ready = submit.completion;
    result.sceneChange.sampleCount =
        ((shape.width + 15) / 16) * ((shape.height + 15) / 16);
    return result;
}

}  // namespace anvil::app
