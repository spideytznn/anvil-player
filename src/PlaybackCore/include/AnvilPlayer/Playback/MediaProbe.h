#pragma once

#include "AnvilPlayer/Playback/Types.h"

#include <filesystem>
#include <chrono>
#include <stop_token>
#include <string>

namespace anvil::playback {

struct MediaProbeResult {
    MediaDescriptor descriptor;
    bool fallbackUsed = false;
    bool failed = false;
    bool cancelled = false;
    bool timedOut = false;
    std::wstring probeTool;
    std::wstring diagnostic;
};

struct MediaProbeOptions {
    std::stop_token stopToken;
    // Total wall-clock budget shared by avformat_open_input and
    // avformat_find_stream_info. A non-positive value disables the deadline,
    // but cancellation is still honoured.
    std::chrono::milliseconds timeout{15000};
};

class MediaProbe {
public:
    static MediaProbeResult Probe(const std::filesystem::path& path,
                                  const MediaProbeOptions& options = {});
    static MediaProbeResult ExtensionFallback(const std::filesystem::path& path, std::wstring diagnostic);
};

}  // namespace anvil::playback
