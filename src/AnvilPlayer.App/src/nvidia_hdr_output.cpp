#include "AnvilPlayer/App/nvidia_hdr_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace anvil::app {
namespace {

using NvStatus = int;
using NvU16 = std::uint16_t;
using NvU32 = std::uint32_t;
using QueryInterface = void*(__cdecl*)(NvU32);

constexpr NvStatus kNvOk = 0;
constexpr NvU32 kInitializeId = 0x0150e828;
constexpr NvU32 kUnloadId = 0xd22bdd7e;
constexpr NvU32 kGetDisplayIdId = 0xae457190;
constexpr NvU32 kGetHdrCapabilitiesId = 0x84f2a8df;
constexpr NvU32 kHdrColorControlId = 0x351da224;
constexpr NvU32 kSetSourceColorSpaceId = 0x473b6caf;
constexpr NvU32 kSetSourceHdrMetadataId = 0x905eb63b;
constexpr NvU32 kSetOutputModeId = 0x98e7661a;
constexpr NvU32 kGetHdrToneMappingId = 0xfbd36e71;
constexpr NvU32 kSetHdrToneMappingId = 0xdd6da362;

constexpr int kOutputModeHdr10 = 1;
constexpr int kOutputModeHdr10PlusGaming = 2;
constexpr int kColorSpaceRec2100 = 12;
constexpr int kToneMappingApp = 0;
constexpr int kToneMappingGpu = 1;
constexpr int kHdrCommandGet = 0;
constexpr int kHdrCommandSet = 1;
constexpr int kHdrModeUhdPassthrough = 5;

template <typename T>
constexpr NvU32 MakeVersion(const NvU32 version) {
    return static_cast<NvU32>(sizeof(T)) | (version << 16);
}

struct HdrMetadata {
    NvU32 version = MakeVersion<HdrMetadata>(1);
    NvU16 redX = 0;
    NvU16 redY = 0;
    NvU16 greenX = 0;
    NvU16 greenY = 0;
    NvU16 blueX = 0;
    NvU16 blueY = 0;
    NvU16 whiteX = 0;
    NvU16 whiteY = 0;
    NvU16 maxMastering = 0;
    NvU16 minMastering = 0;
    NvU16 maxCll = 0;
    NvU16 maxFall = 0;
};

// NV_HDR_COLOR_DATA_V1 from the public NVAPI SDK. The passthrough mode is
// appropriate for our R10G10B10A2/PQ/BT.2020 swap chain because its pixels are
// already encoded as HDR10 and must reach the display unmodified.
struct HdrColorDataV1 {
    NvU32 version = MakeVersion<HdrColorDataV1>(1);
    int command = kHdrCommandGet;
    int hdrMode = 0;
    int staticMetadataDescriptor = 0;
    std::array<NvU16, 12> metadata{};
};

struct HdrCapabilitiesV3 {
    NvU32 version = MakeVersion<HdrCapabilitiesV3>(3);
    NvU32 st2084 : 1;
    NvU32 traditionalHdr : 1;
    NvU32 edr : 1;
    NvU32 expandDefaults : 1;
    NvU32 traditionalSdr : 1;
    NvU32 dolbyVision : 1;
    NvU32 hdr10Plus : 1;
    NvU32 hdr10PlusGaming : 1;
    NvU32 nvidiaCertified : 1;
    NvU32 reserved : 23;
    NvU32 descriptor = 0;
    struct {
        NvU16 primary[8]{};
        NvU16 luminance[3]{};
    } display{};
    struct {
        NvU32 flags = 0;
        NvU16 values[10]{};
    } dolby{};
    NvU16 hdr10PlusVsvdb = 0;
};

static_assert(sizeof(HdrMetadata) == 28);
static_assert(sizeof(HdrColorDataV1) == 40);
static_assert(sizeof(HdrCapabilitiesV3) == 64);

template <typename Function>
Function Resolve(void* query, const NvU32 id) {
    return query ? reinterpret_cast<Function>(reinterpret_cast<QueryInterface>(query)(id)) : nullptr;
}

NvU16 Chromaticity(const double value) {
    return static_cast<NvU16>(std::clamp(std::lround(value * 50000.0), 0L, 50000L));
}

NvU16 Nits(const double value) {
    return static_cast<NvU16>(std::clamp(std::lround(value), 0L, 65535L));
}

NvU16 MinNits(const double value) {
    return static_cast<NvU16>(std::clamp(std::lround(value * 10000.0), 0L, 65535L));
}

std::uint64_t Fingerprint(const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

NvidiaHdrOutput::NvidiaHdrOutput() = default;

NvidiaHdrOutput::~NvidiaHdrOutput() {
    Restore();
    if (initialized_) {
        if (const auto unload = Resolve<NvStatus(__cdecl*)()>(queryInterface_, kUnloadId)) {
            unload();
        }
    }
    if (module_) FreeLibrary(module_);
}

void NvidiaHdrOutput::SetLogHandler(LogHandler handler) {
    logHandler_ = std::move(handler);
}

void NvidiaHdrOutput::Log(const std::wstring& message) const {
    if (logHandler_) logHandler_(message);
}

bool NvidiaHdrOutput::Initialize() {
    if (initialized_) return true;
    if (initializationAttempted_) return false;
    initializationAttempted_ = true;
    module_ = LoadLibraryW(L"nvapi64.dll");
    if (!module_) return false;
    queryInterface_ = reinterpret_cast<void*>(GetProcAddress(module_, "nvapi_QueryInterface"));
    const auto initialize = Resolve<NvStatus(__cdecl*)()>(queryInterface_, kInitializeId);
    if (!initialize || initialize() != kNvOk) return false;
    initialized_ = true;
    return true;
}

bool NvidiaHdrOutput::ResolveDisplay(const HWND window) {
    if (!Initialize() || !window) return false;
    const HMONITOR targetMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!targetMonitor) return false;
    if (monitor_ == targetMonitor && displayId_ != 0) return true;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(targetMonitor, &monitor)) return false;
    char displayName[32]{};
    if (WideCharToMultiByte(CP_ACP, 0, monitor.szDevice, -1, displayName,
                            static_cast<int>(sizeof(displayName)), nullptr, nullptr) <= 0) {
        return false;
    }
    const auto getDisplayId = Resolve<NvStatus(__cdecl*)(const char*, NvU32*)>(queryInterface_, kGetDisplayIdId);
    if (!getDisplayId) return false;
    NvStatus displayStatus = getDisplayId(displayName, &displayId_);
    if (displayStatus != kNvOk && std::string_view(displayName).starts_with(R"(\\.\DISPLAY)")) {
        char alternateName[32]{};
        alternateName[0] = '\\';
        std::copy(displayName + 3, displayName + std::char_traits<char>::length(displayName) + 1,
                  alternateName + 1);
        displayStatus = getDisplayId(alternateName, &displayId_);
    }
    if (displayStatus != kNvOk) {
        Log(L"nvapi display mapping failed status=" + std::to_wstring(displayStatus));
        return false;
    }
    monitor_ = targetMonitor;

    HdrCapabilitiesV3 capabilities{};
    const auto getCapabilities = Resolve<NvStatus(__cdecl*)(NvU32, HdrCapabilitiesV3*)>(queryInterface_, kGetHdrCapabilitiesId);
    if (getCapabilities && getCapabilities(displayId_, &capabilities) == kNvOk) {
        hdr10PlusSinkSupported_ = capabilities.hdr10Plus != 0;
        hdr10PlusGamingSinkSupported_ = capabilities.hdr10PlusGaming != 0;
        Log(L"nvapi hdr capabilities hdr10plus=" +
            std::wstring(hdr10PlusSinkSupported_ ? L"true" : L"false") +
            L" hdr10plus_gaming=" +
            std::wstring(hdr10PlusGamingSinkSupported_ ? L"true" : L"false"));
    }
    return true;
}

bool NvidiaHdrOutput::ApplyHdr10(const HWND window, const anvil::playback::VideoColorMetadata& color) {
    return ApplyOutput(window, color, kOutputModeHdr10, false);
}

bool NvidiaHdrOutput::ApplyHdr10PlusGaming(const HWND window,
                                           const anvil::playback::VideoColorMetadata& color) {
    return ApplyOutput(window, color, kOutputModeHdr10PlusGaming, true);
}

bool NvidiaHdrOutput::SetGpuToneMapping(const bool enabled) {
    if (!enabled && !toneMappingOverrideActive_) return true;
    const auto setToneMapping =
        Resolve<NvStatus(__cdecl*)(NvU32, int)>(queryInterface_, kSetHdrToneMappingId);
    if (!setToneMapping) return false;

    if (enabled) {
        if (!toneMappingOverrideActive_) {
            if (const auto getToneMapping =
                    Resolve<NvStatus(__cdecl*)(NvU32, int*)>(queryInterface_, kGetHdrToneMappingId)) {
                int original = kToneMappingApp;
                if (getToneMapping(displayId_, &original) == kNvOk) {
                    originalToneMapping_ = original;
                }
            }
        }
        const NvStatus status = setToneMapping(displayId_, kToneMappingGpu);
        if (status != kNvOk) {
            Log(L"nvapi hdr10plus gaming gpu tone mapping failed status=" + std::to_wstring(status));
            return false;
        }
        toneMappingOverrideActive_ = true;
        return true;
    }

    const NvStatus status = setToneMapping(displayId_, originalToneMapping_);
    Log(L"nvapi hdr tone mapping restored status=" + std::to_wstring(status));
    if (status == kNvOk) {
        toneMappingOverrideActive_ = false;
    }
    return status == kNvOk;
}

bool NvidiaHdrOutput::ApplyOutput(const HWND window,
                                  const anvil::playback::VideoColorMetadata& color,
                                  const int outputMode,
                                  const bool gpuToneMapping) {
    HdrMetadata metadata{};
    const auto& mastering = color.masteringDisplay;
    const auto red = mastering.hasPrimaries ? mastering.red : anvil::playback::ChromaticityPoint{0.708, 0.292};
    const auto green = mastering.hasPrimaries ? mastering.green : anvil::playback::ChromaticityPoint{0.170, 0.797};
    const auto blue = mastering.hasPrimaries ? mastering.blue : anvil::playback::ChromaticityPoint{0.131, 0.046};
    const auto white = mastering.hasPrimaries ? mastering.whitePoint : anvil::playback::ChromaticityPoint{0.3127, 0.3290};
    metadata.redX = Chromaticity(red.x);
    metadata.redY = Chromaticity(red.y);
    metadata.greenX = Chromaticity(green.x);
    metadata.greenY = Chromaticity(green.y);
    metadata.blueX = Chromaticity(blue.x);
    metadata.blueY = Chromaticity(blue.y);
    metadata.whiteX = Chromaticity(white.x);
    metadata.whiteY = Chromaticity(white.y);
    metadata.maxMastering = Nits(mastering.hasLuminance ? mastering.maxLuminanceNits : 1000.0);
    metadata.minMastering = MinNits(mastering.hasLuminance ? mastering.minLuminanceNits : 0.0001);
    metadata.maxCll = Nits(color.contentLight.hasValues ? color.contentLight.maxContentLightLevelNits : metadata.maxMastering);
    metadata.maxFall = Nits(color.contentLight.hasValues ? color.contentLight.maxFrameAverageLightLevelNits : metadata.maxMastering / 2);

    std::uint64_t fingerprint = Fingerprint(&metadata, sizeof(metadata));
    fingerprint ^= static_cast<std::uint64_t>(outputMode);
    fingerprint *= 1099511628211ull;
    fingerprint ^= gpuToneMapping ? 1ull : 0ull;
    fingerprint *= 1099511628211ull;
    const HMONITOR targetMonitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!targetMonitor) return false;

    // An output-mode override belongs to one physical display. Restore it
    // before following a player window moved to another monitor.
    if (monitor_ && targetMonitor != monitor_ &&
        (outputOverrideActive_ || legacyHdrOverrideActive_ || toneMappingOverrideActive_)) {
        Restore();
    }
    if (!ResolveDisplay(window)) return false;
    if (outputMode == kOutputModeHdr10PlusGaming && !hdr10PlusGamingSinkSupported_) {
        if (fingerprint != lastFailureFingerprint_) {
            Log(L"nvapi hdr10plus gaming unavailable reason=sink_capability_false");
            lastFailureFingerprint_ = fingerprint;
        }
        return false;
    }
    if (targetMonitor == monitor_ && outputOverrideActive_ &&
        activeOutputMode_ == outputMode && fingerprint == lastMetadataFingerprint_) {
        return true;
    }
    if (targetMonitor == monitor_ && fingerprint == lastFailureFingerprint_) return false;

    const auto setOutputMode = Resolve<NvStatus(__cdecl*)(NvU32, int*)>(queryInterface_, kSetOutputModeId);
    const auto setColorSpace = Resolve<NvStatus(__cdecl*)(NvU32, int)>(queryInterface_, kSetSourceColorSpaceId);
    const auto setMetadata = Resolve<NvStatus(__cdecl*)(NvU32, HdrMetadata*)>(queryInterface_, kSetSourceHdrMetadataId);
    if (!setOutputMode || !setColorSpace || !setMetadata) return false;

    if (!outputOverrideActive_ || activeOutputMode_ != outputMode) {
        int requested = outputMode;
        const NvStatus status = setOutputMode(displayId_, &requested);
        if (status != kNvOk) {
            Log(L"nvapi output mode failed target=" + std::to_wstring(outputMode) +
                L" status=" + std::to_wstring(status));
            // The fingerprint includes outputMode, so suppressing this failed
            // request does not block the caller's HDR10 fallback.
            lastFailureFingerprint_ = fingerprint;
            return false;
        }
        if (!outputOverrideActive_) {
            originalOutputMode_ = requested;
        }
        outputOverrideActive_ = true;
        activeOutputMode_ = outputMode;
        Log(L"nvapi output override=" +
            std::wstring(outputMode == kOutputModeHdr10PlusGaming ? L"hdr10plus_gaming" : L"hdr10") +
            L" previous_mode=" + std::to_wstring(originalOutputMode_));
    }

    if (!SetGpuToneMapping(gpuToneMapping)) {
        lastFailureFingerprint_ = fingerprint;
        return false;
    }

    const NvStatus colorStatus = setColorSpace(displayId_, kColorSpaceRec2100);
    const NvStatus metadataStatus = setMetadata(displayId_, &metadata);
    if (colorStatus == kNvOk && metadataStatus == kNvOk) {
        lastMetadataFingerprint_ = fingerprint;
        lastFailureFingerprint_ = 0;
        Log(L"nvapi output=" +
            std::wstring(outputMode == kOutputModeHdr10PlusGaming
                             ? L"hdr10plus_gaming_sstm"
                             : L"hdr10") +
            L" hdr10plus_sink=" + std::wstring(hdr10PlusSinkSupported_ ? L"true" : L"false") +
            L" hdr10plus_gaming_sink=" +
            std::wstring(hdr10PlusGamingSinkSupported_ ? L"true" : L"false"));
        return true;
    }

    // Some Dolby Vision desktop configurations accept the HDR10 output-mode
    // override and metadata but reject the separate source-color-space call.
    // Fall back to NVIDIA's public combined HDR control API in passthrough mode.
    if (outputMode == kOutputModeHdr10) {
        if (const auto hdrColorControl =
                Resolve<NvStatus(__cdecl*)(NvU32, HdrColorDataV1*)>(queryInterface_, kHdrColorControlId)) {
            HdrColorDataV1 original{};
            original.command = kHdrCommandGet;
            const NvStatus getStatus = hdrColorControl(displayId_, &original);

            HdrColorDataV1 requested = original;
            requested.version = MakeVersion<HdrColorDataV1>(1);
            requested.command = kHdrCommandSet;
            requested.hdrMode = kHdrModeUhdPassthrough;
            requested.staticMetadataDescriptor = 0;
            std::memcpy(requested.metadata.data(), &metadata.redX, requested.metadata.size() * sizeof(NvU16));
            const NvStatus fallbackStatus = hdrColorControl(displayId_, &requested);
            if (fallbackStatus == kNvOk) {
                if (!legacyHdrOverrideActive_ && getStatus == kNvOk) {
                    std::memcpy(originalHdrColorData_.data(), &original, sizeof(original));
                    legacyHdrOverrideActive_ = true;
                }
                lastMetadataFingerprint_ = fingerprint;
                lastFailureFingerprint_ = 0;
                Log(L"nvapi output=hdr10_passthrough fallback=hdr_color_control hdr10plus_sink=" +
                    std::wstring(hdr10PlusSinkSupported_ ? L"true" : L"false"));
                return true;
            }
            Log(L"nvapi hdr color control fallback failed status=" + std::to_wstring(fallbackStatus) +
                L" get_status=" + std::to_wstring(getStatus));
        }
    }
    lastFailureFingerprint_ = fingerprint;
    Log(L"nvapi hdr10 source signaling failed color_status=" + std::to_wstring(colorStatus) +
        L" metadata_status=" + std::to_wstring(metadataStatus));
    return false;
}

void NvidiaHdrOutput::Restore() {
    if (toneMappingOverrideActive_) {
        SetGpuToneMapping(false);
    }
    if (legacyHdrOverrideActive_) {
        if (const auto hdrColorControl =
                Resolve<NvStatus(__cdecl*)(NvU32, HdrColorDataV1*)>(queryInterface_, kHdrColorControlId)) {
            HdrColorDataV1 original{};
            std::memcpy(&original, originalHdrColorData_.data(), sizeof(original));
            original.command = kHdrCommandSet;
            NvStatus status = hdrColorControl(displayId_, &original);
            Log(L"nvapi hdr color restored status=" + std::to_wstring(status));
        }
        legacyHdrOverrideActive_ = false;
        originalHdrColorData_.fill(0);
    }
    if (!outputOverrideActive_) {
        lastMetadataFingerprint_ = 0;
        lastFailureFingerprint_ = 0;
        return;
    }
    if (const auto setOutputMode = Resolve<NvStatus(__cdecl*)(NvU32, int*)>(queryInterface_, kSetOutputModeId)) {
        int mode = originalOutputMode_;
        const NvStatus status = setOutputMode(displayId_, &mode);
        Log(L"nvapi output restored status=" + std::to_wstring(status));
    }
    outputOverrideActive_ = false;
    activeOutputMode_ = 0;
    toneMappingOverrideActive_ = false;
    monitor_ = nullptr;
    lastMetadataFingerprint_ = 0;
    lastFailureFingerprint_ = 0;
}

}  // namespace anvil::app
