#pragma once

#include "AnvilPlayer/Playback/Types.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace anvil::app {

// Optional NVIDIA display-output controller. It is dynamically loaded so the
// player remains portable to AMD/Intel systems and does not ship nvapi64.dll.
class NvidiaHdrOutput {
public:
    using LogHandler = std::function<void(const std::wstring&)>;

    NvidiaHdrOutput();
    ~NvidiaHdrOutput();
    NvidiaHdrOutput(const NvidiaHdrOutput&) = delete;
    NvidiaHdrOutput& operator=(const NvidiaHdrOutput&) = delete;

    void SetLogHandler(LogHandler handler);
    bool ApplyHdr10(HWND window, const anvil::playback::VideoColorMetadata& color);
    bool ApplyHdr10PlusGaming(HWND window, const anvil::playback::VideoColorMetadata& color);
    void Restore();
    bool Hdr10PlusSinkSupported() const { return hdr10PlusSinkSupported_; }
    bool Hdr10PlusGamingSinkSupported() const { return hdr10PlusGamingSinkSupported_; }
    bool Hdr10PlusGamingActive() const { return activeOutputMode_ == 2 && outputOverrideActive_; }

private:
    bool Initialize();
    bool ResolveDisplay(HWND window);
    bool ApplyOutput(HWND window,
                     const anvil::playback::VideoColorMetadata& color,
                     int outputMode,
                     bool gpuToneMapping);
    bool SetGpuToneMapping(bool enabled);
    void Log(const std::wstring& message) const;

    HMODULE module_ = nullptr;
    void* queryInterface_ = nullptr;
    HMONITOR monitor_ = nullptr;
    std::uint32_t displayId_ = 0;
    int originalOutputMode_ = 0;
    int activeOutputMode_ = 0;
    int originalToneMapping_ = 0;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
    bool outputOverrideActive_ = false;
    bool legacyHdrOverrideActive_ = false;
    bool hdr10PlusSinkSupported_ = false;
    bool hdr10PlusGamingSinkSupported_ = false;
    bool toneMappingOverrideActive_ = false;
    std::uint64_t lastMetadataFingerprint_ = 0;
    std::uint64_t lastFailureFingerprint_ = 0;
    std::array<std::uint8_t, 40> originalHdrColorData_{};
    LogHandler logHandler_;
};

}  // namespace anvil::app
