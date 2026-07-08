#pragma once

#include "AnvilPlayer/Playback/CapabilityReport.h"
#include "AnvilPlayer/Playback/Log.h"
#include "AnvilPlayer/Playback/Settings.h"
#include "AnvilPlayer/Playback/Types.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace anvil::playback {

class PlayerController {
public:
    explicit PlayerController(std::shared_ptr<InMemoryLogSink> logSink = std::make_shared<InMemoryLogSink>());

    bool OpenMedia(const std::filesystem::path& path, bool extractPreview = true);
    void Close();
    void Play();
    void Pause();
    void TogglePlayPause();
    void Stop();
    void Seek(std::chrono::milliseconds position);
    void SeekRelative(std::chrono::milliseconds delta);
    void SetVolume(double volume);
    void SetPlaybackRate(double rate);
    void SetError(std::wstring message);
    void UpdateClock();
    void SyncClock(std::chrono::milliseconds position);

    PlaybackSessionSnapshot Snapshot() const;
    CapabilityReport CollectCapabilityReport() const;
    PlayerSettings Settings() const;
    void ApplySettings(const PlayerSettings& settings);
    std::shared_ptr<InMemoryLogSink> LogSink() const;

private:
    std::chrono::milliseconds CurrentPositionLocked() const;
    void CommitPositionLocked(std::chrono::milliseconds value);
    void Log(LogLevel level, const std::wstring& category, const std::wstring& message) const;
    mutable std::mutex mutex_;
    PlaybackState state_ = PlaybackState::Empty;
    std::optional<MediaDescriptor> media_;
    std::chrono::milliseconds committedPosition_{0};
    std::chrono::steady_clock::time_point playStartedAt_{};
    double volume_ = 1.0;
    double playbackRate_ = 1.0;
    std::wstring lastError_;
    PlayerSettings settings_;
    std::shared_ptr<InMemoryLogSink> logSink_;
};

}  // namespace anvil::playback
