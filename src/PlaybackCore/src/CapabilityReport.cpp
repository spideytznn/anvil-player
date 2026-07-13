#include "AnvilPlayer/Playback/CapabilityReport.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <process.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d12.lib")
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

bool ContainsDolbyVisionLowLatencyVsvdb(const std::vector<BYTE>& edid) {
    // CTA Vendor-Specific Video Data Block uses Dolby's little-endian IEEE
    // OUI 00-D0-46 on the wire (46 D0 00). In VSVDB v2 the low bit of the
    // third payload byte is the LLDV-over-HDMI capability used by Windows.
    for (std::size_t index = 0; index + 5 < edid.size(); ++index) {
        if (edid[index] == 0x46 && edid[index + 1] == 0xD0 && edid[index + 2] == 0x00) {
            return (edid[index + 5] & 0x01) != 0;
        }
    }
    return false;
}

bool ReadBinaryRegistryValue(HKEY key, const wchar_t* name, std::vector<BYTE>& value) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_BINARY || size == 0) {
        return false;
    }
    value.resize(size);
    return RegQueryValueExW(key, name, nullptr, &type, value.data(), &size) == ERROR_SUCCESS;
}

std::wstring MonitorRegistryPathFromInterface(std::wstring path) {
    constexpr std::wstring_view prefix = L"\\\\?\\";
    if (path.rfind(prefix, 0) == 0) {
        path.erase(0, prefix.size());
    }
    const auto classGuid = path.find(L"#{");
    if (classGuid != std::wstring::npos) {
        path.resize(classGuid);
    }
    std::replace(path.begin(), path.end(), L'#', L'\\');
    return L"SYSTEM\\CurrentControlSet\\Enum\\" + path + L"\\Device Parameters";
}

bool ActiveDisplaySupportsDolbyVisionLowLatency() {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                           &pathCount,
                           paths.data(),
                           &modeCount,
                           modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return false;
    }

    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[index].targetInfo.adapterId;
        target.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || target.monitorDevicePath[0] == L'\0') {
            continue;
        }

        const auto registryPath = MonitorRegistryPathFromInterface(target.monitorDevicePath);
        HKEY parameters = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, registryPath.c_str(), 0, KEY_READ, &parameters) != ERROR_SUCCESS) {
            continue;
        }
        std::vector<BYTE> edid;
        const bool baseHasLldv = ReadBinaryRegistryValue(parameters, L"EDID", edid) &&
                                 ContainsDolbyVisionLowLatencyVsvdb(edid);
        HKEY overrideKey = nullptr;
        bool overrideHasLldv = false;
        if (RegOpenKeyExW(parameters, L"EDID_OVERRIDE", 0, KEY_READ, &overrideKey) == ERROR_SUCCESS) {
            for (DWORD block = 0; block < 8 && !overrideHasLldv; ++block) {
                const auto blockName = std::to_wstring(block);
                std::vector<BYTE> overrideBlock;
                overrideHasLldv = ReadBinaryRegistryValue(overrideKey, blockName.c_str(), overrideBlock) &&
                                  ContainsDolbyVisionLowLatencyVsvdb(overrideBlock);
            }
            RegCloseKey(overrideKey);
        }
        RegCloseKey(parameters);
        if (overrideHasLldv || baseHasLldv) {
            return true;
        }
    }
    return false;
}

bool ActiveDisplayAdvancedColorEnabled(const LUID& adapterLuid) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                           &pathCount,
                           paths.data(),
                           &modeCount,
                           modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return false;
    }

    for (UINT32 index = 0; index < pathCount; ++index) {
        if (paths[index].targetInfo.adapterId.HighPart != adapterLuid.HighPart ||
            paths[index].targetInfo.adapterId.LowPart != adapterLuid.LowPart) {
            continue;
        }
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo{};
        colorInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        colorInfo.header.size = sizeof(colorInfo);
        colorInfo.header.adapterId = paths[index].targetInfo.adapterId;
        colorInfo.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&colorInfo.header) == ERROR_SUCCESS &&
            colorInfo.advancedColorSupported && colorInfo.advancedColorEnabled) {
            return true;
        }
    }
    return false;
}

bool AnyActiveDisplayAdvancedColorEnabled() {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                           &pathCount,
                           paths.data(),
                           &modeCount,
                           modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return false;
    }
    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo{};
        colorInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        colorInfo.header.size = sizeof(colorInfo);
        colorInfo.header.adapterId = paths[index].targetInfo.adapterId;
        colorInfo.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&colorInfo.header) == ERROR_SUCCESS &&
            colorInfo.advancedColorSupported && colorInfo.advancedColorEnabled) {
            return true;
        }
    }
    return false;
}

std::wstring DecoderProfileName(const GUID& profile) {
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_H264)) {
        return L"H.264";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN)) {
        return L"HEVC Main";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10)) {
        return L"HEVC Main10";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_VP9)) {
        return L"VP9 Profile 0";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_VP9_10BIT_PROFILE2)) {
        return L"VP9 Profile 2 10-bit";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0)) {
        return L"AV1 Profile 0";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1)) {
        return L"AV1 Profile 1";
    }
    if (IsEqualGUID(profile, D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2)) {
        return L"AV1 Profile 2";
    }
    return {};
}

void PopulateVideoDecodeProfiles(ID3D12Device* device, GpuCapabilities& gpu) {
    ComPtr<ID3D12VideoDevice> videoDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice)))) {
        gpu.hardwareDecodeProfiles = {L"D3D12 video device unavailable"};
        return;
    }
    D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count{};
    if (FAILED(videoDevice->CheckFeatureSupport(
            D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof(count))) ||
        count.ProfileCount == 0) {
        gpu.hardwareDecodeProfiles = {L"No D3D12 decode profiles reported"};
        return;
    }
    std::vector<GUID> profiles(count.ProfileCount);
    D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES query{};
    query.ProfileCount = count.ProfileCount;
    query.pProfiles = profiles.data();
    if (FAILED(videoDevice->CheckFeatureSupport(
            D3D12_FEATURE_VIDEO_DECODE_PROFILES, &query, sizeof(query)))) {
        gpu.hardwareDecodeProfiles = {L"D3D12 decode profile query failed"};
        return;
    }
    for (const GUID& profile : profiles) {
        auto name = DecoderProfileName(profile);
        if (!name.empty()) {
            AddUnique(gpu.hardwareDecodeProfiles, std::move(name));
        }
    }
    if (gpu.hardwareDecodeProfiles.empty()) {
        gpu.hardwareDecodeProfiles = {L"No mapped D3D12 profiles reported"};
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
    const bool pqDesktop =
        desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        desc.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
    DXGI_ADAPTER_DESC adapterDesc{};
    const bool advancedColorDesktop = SUCCEEDED(adapter->GetDesc(&adapterDesc)) &&
                                      ActiveDisplayAdvancedColorEnabled(adapterDesc.AdapterLuid);
    display.hdrEnabled = pqDesktop || advancedColorDesktop;
    display.hdrSupported = display.hdrEnabled || desc.MaxLuminance >= 400.0f;
    if (advancedColorDesktop && !pqDesktop) {
        display.colorSpace += L" (Advanced Color enabled)";
    }
}

bool TryPopulateD3D12(CapabilityReport& report) {
    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        report.gpu.adapterName = L"DXGI factory creation failed";
        report.gpu.d3dFeatureLevel = L"Unavailable";
        report.gpu.hardwareDecodeProfiles = {L"Unavailable"};
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            adapter = candidate;
            break;
        }
    }
    if (!adapter) {
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    }
    ComPtr<ID3D12Device> device;
    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
    for (const D3D_FEATURE_LEVEL level : requestedLevels) {
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device)))) {
            createdLevel = level;
            break;
        }
    }
    if (!device) {
        report.gpu.adapterName = L"D3D12 device creation failed";
        report.gpu.d3dFeatureLevel = L"Unavailable";
        report.gpu.hardwareDecodeProfiles = {L"Unavailable"};
        return false;
    }
    report.gpu.d3dFeatureLevel = FeatureLevelToString(createdLevel);
    PopulateVideoDecodeProfiles(device.Get(), report.gpu);
    DXGI_ADAPTER_DESC1 adapterDesc{};
    if (SUCCEEDED(adapter->GetDesc1(&adapterDesc))) {
        report.gpu.adapterName = adapterDesc.Description;
    }
    PopulateDisplayCapabilities(adapter.Get(), report.display);
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

std::wstring ActivationFriendlyName(IMFActivate* activation,
                                    const wchar_t* fallbackName) {
    CoTaskMemWideString name;
    UINT32 nameLength = 0;
    std::wstring friendlyName = fallbackName;
    if (activation &&
        SUCCEEDED(activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                 &name.value,
                                                 &nameLength)) &&
        name.value) {
        friendlyName.assign(name.value, name.value + nameLength);
    }
    GUID clsid{};
    if (activation &&
        SUCCEEDED(activation->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
        wchar_t clsidText[64]{};
        if (StringFromGUID2(clsid, clsidText, static_cast<int>(std::size(clsidText))) > 0) {
            friendlyName += L" ";
            friendlyName += clsidText;
        }
    }
    return friendlyName;
}

void PopulateDolbyVisionRendererEffects(CodecCapabilities& codecs,
                                        const UINT32 flags) {
    MediaFoundationActivations activations;
    const HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_RENDERER_EFFECT,
                                     flags,
                                     nullptr,
                                     nullptr,
                                     &activations.values,
                                     &activations.count);
    if (FAILED(result)) {
        AddUnique(codecs.mediaFoundationTransforms,
                  L"Video renderer effect enumeration failed " +
                      std::to_wstring(static_cast<unsigned long>(result)));
        return;
    }

    for (UINT32 index = 0; index < activations.count; ++index) {
        IMFActivate* activation = activations.values[index];
        if (!activation) {
            continue;
        }
        PROPVARIANT profiles{};
        PropVariantInit(&profiles);
        const HRESULT profileResult = activation->GetItem(
            MFT_ENUM_VIDEO_RENDERER_EXTENSION_PROFILE,
            &profiles);
        std::wstring advertisedProfiles;
        bool advertisesDolbyVision = false;
        if (SUCCEEDED(profileResult) && profiles.vt == (VT_VECTOR | VT_LPWSTR)) {
            for (ULONG profileIndex = 0;
                 profileIndex < profiles.calpwstr.cElems;
                 ++profileIndex) {
                const wchar_t* profile = profiles.calpwstr.pElems[profileIndex];
                if (!profile) {
                    continue;
                }
                if (!advertisedProfiles.empty()) {
                    advertisedProfiles += L", ";
                }
                advertisedProfiles += profile;
                if (_wcsicmp(profile, L"dvhe.05") == 0 ||
                    _wcsicmp(profile, L"dvhe.08") == 0 ||
                    _wcsicmp(profile, L"dvav.09") == 0) {
                    advertisesDolbyVision = true;
                }
            }
        }
        PropVariantClear(&profiles);
        if (!advertisesDolbyVision) {
            continue;
        }
        codecs.dolbyVisionExtensionDetected = true;
        std::wstring name = ActivationFriendlyName(
            activation,
            L"Unnamed Dolby Vision renderer effect");
        if (!advertisedProfiles.empty()) {
            name += L" [renderer profiles: " + advertisedProfiles + L"]";
        }
        AddUnique(codecs.mediaFoundationTransforms, std::move(name));
    }
}

void PopulateMediaFoundationTransforms(CodecCapabilities& codecs) {
    codecs.mediaFoundationTransforms.clear();
    codecs.dolbyVisionExtensionDetected = false;

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
    const UINT32 flags = MFT_ENUM_FLAG_ALL;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        flags,
        &inputType,
        nullptr,
        &activations.values,
        &activations.count);
    if (FAILED(hr)) {
        codecs.mediaFoundationTransforms = {
            L"HEVC decoder MFT enumeration failed " +
                std::to_wstring(static_cast<unsigned long>(hr)),
        };
    }

    for (UINT32 index = 0; index < activations.count; ++index) {
        if (!activations.values[index]) {
            continue;
        }

        std::wstring friendlyName = ActivationFriendlyName(
            activations.values[index],
            L"Unnamed HEVC decoder MFT");
        AddUnique(codecs.mediaFoundationTransforms, std::move(friendlyName));
    }

    PopulateDolbyVisionRendererEffects(codecs, flags);

    if (codecs.mediaFoundationTransforms.empty()) {
        codecs.mediaFoundationTransforms = {
            L"No HEVC decoder or Dolby Vision renderer effect MFT reported",
        };
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
    TryPopulateD3D12(report);
    report.display.dolbyVisionSignalAvailable = ActiveDisplaySupportsDolbyVisionLowLatency();
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

bool CapabilityDetector::IsHdrEnabledNow() {
    return AnyActiveDisplayAdvancedColorEnabled();
}

}  // namespace anvil::playback
