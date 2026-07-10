#include "AnvilPlayer/Playback/CapabilityReport.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <process.h>
#include <string>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace anvil::playback {
namespace {

using Microsoft::WRL::ComPtr;

CapabilityReport MakeConservativeReport() {
    CapabilityReport report;
    report.display.colorSpace = L"Probe pending";
    report.gpu.adapterName = L"Probe pending";
    report.gpu.d3dFeatureLevel = L"Pending";
    report.gpu.hardwareDecodeProfiles = {L"Probe pending"};
    report.audio.endpointName = L"Default Windows endpoint";
    report.audio.encodedFormats = {L"AC-3", L"E-AC-3", L"TrueHD", L"DTS", L"DTS-HD"};
    report.codecs.mediaFoundationTransforms = {L"Probe pending"};
    return report;
}

std::wstring FeatureLevelToString(const D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_12_1:
        return L"12.1";
    case D3D_FEATURE_LEVEL_12_0:
        return L"12.0";
    case D3D_FEATURE_LEVEL_11_1:
        return L"11.1";
    case D3D_FEATURE_LEVEL_11_0:
        return L"11.0";
    case D3D_FEATURE_LEVEL_10_1:
        return L"10.1";
    case D3D_FEATURE_LEVEL_10_0:
        return L"10.0";
    case D3D_FEATURE_LEVEL_9_3:
        return L"9.3";
    case D3D_FEATURE_LEVEL_9_2:
        return L"9.2";
    case D3D_FEATURE_LEVEL_9_1:
        return L"9.1";
    default:
        return L"Unknown";
    }
}

std::wstring ColorSpaceToString(const DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return L"RGB SDR BT.709 full";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return L"RGB SDR BT.709 studio";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return L"RGB HDR10 BT.2020 full";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return L"RGB HDR10 BT.2020 studio";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return L"RGB linear BT.709";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return L"RGB SDR BT.2020 full";
    default:
        return L"DXGI color space " + std::to_wstring(static_cast<int>(colorSpace));
    }
}

void AddUnique(std::vector<std::wstring>& values, std::wstring value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

bool ContainsInsensitive(std::wstring value, std::wstring needle) {
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    return value.find(needle) != std::wstring::npos;
}

std::wstring DecoderProfileName(const GUID& profile) {
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_H264_VLD_NOFGT) ||
        IsEqualGUID(profile, D3D11_DECODER_PROFILE_H264_VLD_FGT)) {
        return L"H.264";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_HEVC_VLD_MAIN)) {
        return L"HEVC Main";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10)) {
        return L"HEVC Main10";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0)) {
        return L"VP9 Profile 0";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2)) {
        return L"VP9 Profile 2 10-bit";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0)) {
        return L"AV1 Profile 0";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1)) {
        return L"AV1 Profile 1";
    }
    if (IsEqualGUID(profile, D3D11_DECODER_PROFILE_AV1_VLD_PROFILE2)) {
        return L"AV1 Profile 2";
    }
    return {};
}

void PopulateVideoDecodeProfiles(ID3D11Device* device, GpuCapabilities& gpu) {
    ComPtr<ID3D11VideoDevice> videoDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice)))) {
        gpu.hardwareDecodeProfiles = {L"D3D11 video device unavailable"};
        return;
    }

    const UINT count = videoDevice->GetVideoDecoderProfileCount();
    for (UINT index = 0; index < count; ++index) {
        GUID profile{};
        if (SUCCEEDED(videoDevice->GetVideoDecoderProfile(index, &profile))) {
            auto name = DecoderProfileName(profile);
            if (!name.empty()) {
                AddUnique(gpu.hardwareDecodeProfiles, std::move(name));
            }
        }
    }

    if (gpu.hardwareDecodeProfiles.empty()) {
        gpu.hardwareDecodeProfiles = {L"No mapped D3D11 profiles reported"};
    }
}

void PopulateDisplayCapabilities(IDXGIAdapter* adapter, DisplayCapabilities& display) {
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        display.colorSpace = L"No attached DXGI output";
        return;
    }

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6))) {
        display.colorSpace = L"DXGI output HDR query unavailable";
        return;
    }

    DXGI_OUTPUT_DESC1 desc{};
    if (FAILED(output6->GetDesc1(&desc))) {
        display.colorSpace = L"DXGI output description unavailable";
        return;
    }

    display.colorSpace = ColorSpaceToString(desc.ColorSpace);
    display.reportedPeakBrightnessNits = static_cast<int>(desc.MaxLuminance + 0.5f);
    display.hdrEnabled =
        desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        desc.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
    display.hdrSupported = display.hdrEnabled || desc.MaxLuminance >= 400.0f;
}

bool TryPopulateD3D11(CapabilityReport& report) {
    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL createdLevel{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        requestedLevels,
        static_cast<UINT>(sizeof(requestedLevels) / sizeof(requestedLevels[0])),
        D3D11_SDK_VERSION,
        &device,
        &createdLevel,
        &context);

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requestedLevels,
            static_cast<UINT>(sizeof(requestedLevels) / sizeof(requestedLevels[0])),
            D3D11_SDK_VERSION,
            &device,
            &createdLevel,
            &context);
    }

    if (FAILED(hr)) {
        report.gpu.adapterName = L"D3D11 device creation failed";
        report.gpu.d3dFeatureLevel = L"Unavailable";
        report.gpu.hardwareDecodeProfiles = {L"Unavailable"};
        return false;
    }

    report.gpu.d3dFeatureLevel = FeatureLevelToString(createdLevel);
    PopulateVideoDecodeProfiles(device.Get(), report.gpu);

    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device.As(&dxgiDevice))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC adapterDesc{};
            if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                report.gpu.adapterName = adapterDesc.Description;
            }
            PopulateDisplayCapabilities(adapter.Get(), report.display);
        }
    }

    return true;
}

class MediaFoundationSession {
public:
    MediaFoundationSession()
        : result_(MFStartup(MF_VERSION, MFSTARTUP_LITE)) {
    }

    ~MediaFoundationSession() {
        if (SUCCEEDED(result_)) {
            MFShutdown();
        }
    }

    HRESULT Result() const {
        return result_;
    }

private:
    HRESULT result_ = E_FAIL;
};

struct MediaFoundationActivations {
    ~MediaFoundationActivations() {
        if (!values) {
            return;
        }
        for (UINT32 index = 0; index < count; ++index) {
            if (values[index]) {
                values[index]->Release();
            }
        }
        CoTaskMemFree(values);
    }

    IMFActivate** values = nullptr;
    UINT32 count = 0;
};

struct CoTaskMemWideString {
    ~CoTaskMemWideString() {
        CoTaskMemFree(value);
    }

    wchar_t* value = nullptr;
};

void PopulateMediaFoundationTransforms(CodecCapabilities& codecs) {
    codecs.mediaFoundationTransforms.clear();

    MediaFoundationSession mediaFoundation;
    HRESULT hr = mediaFoundation.Result();
    if (FAILED(hr)) {
        codecs.mediaFoundationTransforms = {L"Media Foundation startup failed " + std::to_wstring(static_cast<unsigned long>(hr))};
        return;
    }

    MFT_REGISTER_TYPE_INFO inputType{};
    inputType.guidMajorType = MFMediaType_Video;
    inputType.guidSubtype = MFVideoFormat_HEVC;

    MediaFoundationActivations activations;
    const UINT32 flags =
        MFT_ENUM_FLAG_SYNCMFT |
        MFT_ENUM_FLAG_ASYNCMFT |
        MFT_ENUM_FLAG_HARDWARE |
        MFT_ENUM_FLAG_LOCALMFT |
        MFT_ENUM_FLAG_SORTANDFILTER;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        flags,
        &inputType,
        nullptr,
        &activations.values,
        &activations.count);
    if (FAILED(hr)) {
        codecs.mediaFoundationTransforms = {L"HEVC decoder MFT enumeration failed " + std::to_wstring(static_cast<unsigned long>(hr))};
        return;
    }

    for (UINT32 index = 0; index < activations.count; ++index) {
        if (!activations.values[index]) {
            continue;
        }

        CoTaskMemWideString name;
        UINT32 nameLength = 0;
        std::wstring friendlyName = L"Unnamed HEVC decoder MFT";
        if (SUCCEEDED(activations.values[index]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute,
                &name.value,
                &nameLength)) &&
            name.value) {
            friendlyName.assign(name.value, name.value + nameLength);
        }

        GUID clsid{};
        if (SUCCEEDED(activations.values[index]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            wchar_t clsidText[64]{};
            if (StringFromGUID2(clsid, clsidText, static_cast<int>(std::size(clsidText))) > 0) {
                friendlyName += L" ";
                friendlyName += clsidText;
            }
        }

        if (ContainsInsensitive(friendlyName, L"dolby")) {
            codecs.dolbyVisionExtensionDetected = true;
        }
        AddUnique(codecs.mediaFoundationTransforms, std::move(friendlyName));
    }

    if (codecs.mediaFoundationTransforms.empty()) {
        codecs.mediaFoundationTransforms = {L"No HEVC video decoder MFT reported"};
    }
}

class ComApartment {
public:
    ComApartment()
        : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
    }

    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    bool Available() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    HRESULT Result() const {
        return result_;
    }

private:
    HRESULT result_ = E_FAIL;
};

CapabilityReport BuildCapabilityReport(const ComApartment& apartment) {
    CapabilityReport report = MakeConservativeReport();
    TryPopulateD3D11(report);
    if (apartment.Available()) {
        PopulateMediaFoundationTransforms(report.codecs);
    } else {
        report.codecs.mediaFoundationTransforms = {
            L"COM initialization failed " +
            std::to_wstring(static_cast<unsigned long>(apartment.Result())),
        };
    }
    return report;
}

// This state and its published report intentionally have process lifetime.
// A driver or MFT call may never return; leaking one bounded state object keeps
// readers safe without a shutdown join or a static-destruction race.
struct CapabilityProbeState {
    CapabilityProbeState()
        : conservative(MakeConservativeReport()) {
    }

    std::atomic_bool launchAttempted{false};
    std::atomic<const CapabilityReport*> published{nullptr};
    DWORD testDelayMs = 0;
    CapabilityReport conservative;
};

CapabilityProbeState* ProcessCapabilityProbeState() noexcept {
    static CapabilityProbeState* const state = []() noexcept -> CapabilityProbeState* {
        try {
            return new CapabilityProbeState();
        } catch (...) {
            OutputDebugStringW(L"[capabilities] unable to allocate process probe state\n");
            return nullptr;
        }
    }();
    return state;
}

DWORD CapabilityProbeDelayForTesting() noexcept {
#if defined(_DEBUG)
    wchar_t value[32]{};
    constexpr DWORD valueCapacity = static_cast<DWORD>(sizeof(value) / sizeof(value[0]));
    const DWORD length = GetEnvironmentVariableW(
        L"ANVIL_PLAYER_TEST_CAPABILITY_DELAY_MS",
        value,
        valueCapacity);
    if (length > 0 && length < valueCapacity) {
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(value, &end, 10);
        if (end != value) {
            return static_cast<DWORD>(std::min<unsigned long>(parsed, 30000UL));
        }
    }
#endif
    return 0;
}

unsigned __stdcall RunCapabilityProbe(void* opaque) noexcept {
    auto* state = static_cast<CapabilityProbeState*>(opaque);
    if (!state) {
        return 0;
    }
    if (state->testDelayMs > 0) {
        Sleep(state->testDelayMs);
    }

    ComApartment apartment;
    try {
        auto report = std::make_unique<CapabilityReport>(BuildCapabilityReport(apartment));
        const CapabilityReport* published = report.release();
        state->published.store(published, std::memory_order_release);
    } catch (...) {
        // The conservative snapshot remains valid. No worker exception may
        // escape a thread entry point and terminate the process.
        OutputDebugStringW(L"[capabilities] background probe failed with an exception\n");
    }
    return 0;
}

void StartCapabilityProbe(CapabilityProbeState* state) noexcept {
    if (!state) {
        return;
    }
    bool expected = false;
    if (!state->launchAttempted.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    state->testDelayMs = CapabilityProbeDelayForTesting();
    const uintptr_t threadHandle = _beginthreadex(
        nullptr,
        0,
        &RunCapabilityProbe,
        state,
        0,
        nullptr);
    if (threadHandle == 0) {
        OutputDebugStringW(L"[capabilities] unable to start background probe\n");
        return;
    }
    CloseHandle(reinterpret_cast<HANDLE>(threadHandle));
}

}  // namespace

CapabilityReport CapabilityDetector::CollectBasic() {
    CapabilityProbeState* const state = ProcessCapabilityProbeState();
    if (!state) {
        return MakeConservativeReport();
    }

    if (const CapabilityReport* const published =
            state->published.load(std::memory_order_acquire)) {
        return *published;
    }

    StartCapabilityProbe(state);
    if (const CapabilityReport* const published =
            state->published.load(std::memory_order_acquire)) {
        return *published;
    }
    return state->conservative;
}

}  // namespace anvil::playback
