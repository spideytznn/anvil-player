#pragma once

#include "AnvilPlayer/App/ui_types.h"
// PlayerController.h transitively provides LogLevel/DefaultLogLevel/
// LogLevelFromString (Log.h) and the kVideoTrack* constants (Settings.h).
#include "AnvilPlayer/Playback/PlayerController.h"

#include <filesystem>

namespace anvil::app {

// Parsed command-line arguments shared by the entry point (first launch) and
// the single-instance command-line forwarder (second launch). Keeping this in a
// shared header lets a forwarded WM_COPYDATA payload reuse the same parsing.
struct AppArguments {
    std::filesystem::path mediaPath;
    bool autoplay = false;
    bool webUiEnabled = true;
    anvil::playback::LogLevel logLevel = anvil::playback::DefaultLogLevel();
    PlaybackBackend backend = PlaybackBackend::NativeFfmpegD3D12;
    int selectedVideoTrackIndex = anvil::playback::kVideoTrackAuto;
};

// Parses an argv array (as returned by CommandLineToArgvW) into AppArguments.
AppArguments ParseArguments(int argumentCount, wchar_t** arguments);

// Convenience wrapper: parses a raw command-line string by tokenizing it the
// same way the OS would (CommandLineToArgvW), then calls ParseArguments. Used
// when a forwarded command line arrives as a single string.
AppArguments ParseCommandLine(const std::wstring& commandLine);

}  // namespace anvil::app
