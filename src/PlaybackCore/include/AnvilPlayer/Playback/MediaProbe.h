#pragma once

#include "AnvilPlayer/Playback/Types.h"

#include <filesystem>
#include <string>

namespace anvil::playback {

struct MediaProbeResult {
    MediaDescriptor descriptor;
    bool fallbackUsed = false;
    std::wstring probeTool;
    std::wstring diagnostic;
};

class MediaProbe {
public:
    static MediaProbeResult Probe(const std::filesystem::path& path);
    static MediaProbeResult ExtensionFallback(const std::filesystem::path& path, std::wstring diagnostic);
};

}  // namespace anvil::playback
