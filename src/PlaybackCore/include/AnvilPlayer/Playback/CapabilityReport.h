#pragma once

#include <string>
#include <vector>

namespace anvil::playback {

struct DisplayCapabilities {
    bool hdrSupported = false;
    bool hdrEnabled = false;
    bool dolbyVisionSignalAvailable = false;
    // True when the player can decode DV in software and reshape on the GPU,
    // producing correct-color HDR10 output (regardless of native DV signaling).
    // Always true when FFmpeg with dovi_meta.h is linked.
    bool dolbyVisionSoftwareReshapeAvailable = true;
    int reportedPeakBrightnessNits = 0;
    std::wstring colorSpace = L"Unknown";
};

struct GpuCapabilities {
    std::wstring adapterName = L"Unknown";
    std::wstring driverVersion = L"Unknown";
    std::wstring d3dFeatureLevel = L"Pending";
    std::vector<std::wstring> hardwareDecodeProfiles;
};

struct AudioCapabilities {
    std::wstring endpointName = L"Auto";
    bool sharedModeAvailable = true;
    bool exclusiveModeAvailable = false;
    std::vector<std::wstring> encodedFormats;
};

struct CodecCapabilities {
    std::wstring ffmpegVersion = L"Not linked";
    std::vector<std::wstring> mediaFoundationTransforms;
    bool dolbyVisionExtensionDetected = false;
};

struct CapabilityReport {
    DisplayCapabilities display;
    GpuCapabilities gpu;
    AudioCapabilities audio;
    CodecCapabilities codecs;
};

class CapabilityDetector {
public:
    static CapabilityReport CollectBasic();
};

}  // namespace anvil::playback
