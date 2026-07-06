#include "AnvilPlayer/Playback/CapabilityReport.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
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

void PopulateMediaFoundationTransforms(CodecCapabilities& codecs) {
    codecs.mediaFoundationTransforms.clear();

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        codecs.mediaFoundationTransforms = {L"Media Foundation startup failed " + std::to_wstring(static_cast<unsigned long>(hr))};
        return;
    }

    MFT_REGISTER_TYPE_INFO inputType{};
    inputType.guidMajorType = MFMediaType_Video;
    inputType.guidSubtype = MFVideoFormat_HEVC;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const UINT32 flags =
        MFT_ENUM_FLAG_SYNCMFT |
        MFT_ENUM_FLAG_ASYNCMFT |
        MFT_ENUM_FLAG_HARDWARE |
        MFT_ENUM_FLAG_LOCALMFT |
        MFT_ENUM_FLAG_SORTANDFILTER;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inputType, nullptr, &activates, &count);
    if (FAILED(hr)) {
        codecs.mediaFoundationTransforms = {L"HEVC decoder MFT enumeration failed " + std::to_wstring(static_cast<unsigned long>(hr))};
        MFShutdown();
        return;
    }

    for (UINT32 index = 0; index < count; ++index) {
        if (!activates[index]) {
            continue;
        }

        wchar_t* name = nullptr;
        UINT32 nameLength = 0;
        std::wstring friendlyName = L"Unnamed HEVC decoder MFT";
        if (SUCCEEDED(activates[index]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLength)) && name) {
            friendlyName.assign(name, name + nameLength);
            CoTaskMemFree(name);
        }

        GUID clsid{};
        if (SUCCEEDED(activates[index]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
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
        activates[index]->Release();
    }
    CoTaskMemFree(activates);

    if (codecs.mediaFoundationTransforms.empty()) {
        codecs.mediaFoundationTransforms = {L"No HEVC video decoder MFT reported"};
    }

    MFShutdown();
}

}  // namespace

CapabilityReport CapabilityDetector::CollectBasic() {
    CapabilityReport report;
    report.display.colorSpace = L"Probe pending";
    report.gpu.adapterName = L"Probe pending";
    report.gpu.d3dFeatureLevel = L"D3D11 target";
    report.gpu.hardwareDecodeProfiles = {L"Probe pending"};
    TryPopulateD3D11(report);
    report.audio.endpointName = L"Default Windows endpoint";
    report.audio.encodedFormats = {L"AC-3", L"E-AC-3", L"TrueHD", L"DTS", L"DTS-HD"};
    PopulateMediaFoundationTransforms(report.codecs);
    return report;
}

}  // namespace anvil::playback
