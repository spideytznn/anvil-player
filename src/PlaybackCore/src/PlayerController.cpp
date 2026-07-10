#include "AnvilPlayer/Playback/PlayerController.h"

#include "AnvilPlayer/Playback/MediaPreview.h"
#include "AnvilPlayer/Playback/MediaProbe.h"
#include "AnvilPlayer/Playback/PlaybackPlan.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <utility>

namespace anvil::playback {
namespace {

std::chrono::milliseconds ClampDuration(const std::chrono::milliseconds value,
                                        const std::chrono::milliseconds duration) {
    if (value < std::chrono::milliseconds{0}) {
        return std::chrono::milliseconds{0};
    }
    if (duration.count() > 0 && value > duration) {
        return duration;
    }
    return value;
}

std::chrono::milliseconds ScaleDuration(const std::chrono::milliseconds value, const double rate) {
    return std::chrono::milliseconds{
        static_cast<long long>(std::llround(static_cast<double>(value.count()) * rate))};
}

}  // namespace

PlayerController::PlayerController(std::shared_ptr<InMemoryLogSink> logSink)
    : settings_(MakeDefaultSettings()), logSink_(std::move(logSink)) {
    // Start the process-wide capability probe before any media I/O so it can
    // overlap the first open. CollectBasic is a non-blocking snapshot read.
    (void)CapabilityDetector::CollectBasic();
    Log(LogLevel::Info, L"core", L"playback controller initialized");
}

bool PlayerController::OpenMedia(const std::filesystem::path& path, const bool extractPreview) {
    return CommitMedia(PrepareMedia(path, extractPreview));
}

PreparedMediaOpen PlayerController::PrepareMedia(const std::filesystem::path& path,
                                                  const bool extractPreview,
                                                  const MediaProbeOptions& options) const {
    PreparedMediaOpen prepared;
    prepared.path = path;
    prepared.extractPreview = extractPreview;
    if (options.stopToken.stop_requested()) {
        prepared.probe.cancelled = true;
        prepared.error = L"Open cancelled";
        return prepared;
    }

    prepared.probe = MediaProbe::Probe(path, options);
    if (prepared.probe.cancelled || prepared.probe.timedOut) {
        prepared.error = prepared.probe.diagnostic.empty()
                             ? (prepared.probe.cancelled ? L"Open cancelled" : L"Media probe timed out")
                             : prepared.probe.diagnostic;
        return prepared;
    }
    if (prepared.probe.failed) {
        prepared.error = prepared.probe.diagnostic.empty() ? L"Unable to open media" : prepared.probe.diagnostic;
        return prepared;
    }
    if (options.stopToken.stop_requested()) {
        prepared.probe.cancelled = true;
        prepared.error = L"Open cancelled";
        return prepared;
    }

    prepared.capabilities = CapabilityDetector::CollectBasic();
    PlayerSettings settings;
    {
        std::scoped_lock lock(mutex_);
        settings = settings_;
    }
    prepared.plan = PlaybackPlanner::Build(prepared.probe.descriptor, settings, prepared.capabilities);
    prepared.probe.descriptor.selectedDecodePath = prepared.plan.videoDecoder;
    if (extractPreview && prepared.probe.descriptor.hasVideo && !options.stopToken.stop_requested()) {
        prepared.preview = MediaPreview::ExtractFirstFrame(path);
        if (prepared.preview.generated) {
            prepared.probe.descriptor.previewImagePath = prepared.preview.imagePath;
        }
    }
    if (options.stopToken.stop_requested()) {
        prepared.probe.cancelled = true;
        prepared.error = L"Open cancelled";
        return prepared;
    }
    prepared.succeeded = true;
    return prepared;
}

bool PlayerController::CommitMedia(PreparedMediaOpen prepared) {
    if (!prepared.succeeded) {
        if (prepared.probe.cancelled) {
            return false;
        }
        const std::wstring error = prepared.error.empty() ? L"Unable to open media" : prepared.error;
        {
            std::scoped_lock lock(mutex_);
            media_.reset();
            committedPosition_ = std::chrono::milliseconds{0};
            playbackRate_ = 1.0;
            state_ = PlaybackState::Error;
            lastError_ = error;
        }
        Log(LogLevel::Error, L"open", error + L": " + prepared.path.wstring());
        return false;
    }

    MediaDescriptor media = std::move(prepared.probe.descriptor);
    PlaybackPlan plan;
    ToneMappingMode toneMapping = ToneMappingMode::Auto;
    {
        std::scoped_lock lock(mutex_);
        plan = PlaybackPlanner::Build(media, settings_, prepared.capabilities);
        toneMapping = settings_.video.toneMapping;
        media.selectedDecodePath = plan.videoDecoder;
        media_ = media;
        committedPosition_ = std::chrono::milliseconds{0};
        playbackRate_ = 1.0;
        state_ = PlaybackState::Ready;
        lastError_.clear();
    }

    Log(LogLevel::Info, L"open", L"file=" + prepared.path.wstring());
    Log(LogLevel::Info,
        L"probe",
        L"tool=" + (prepared.probe.probeTool.empty() ? L"unknown" : prepared.probe.probeTool) +
            L" container=" + media.container +
            L" duration=" + FormatTimecode(media.duration));
    if (prepared.probe.fallbackUsed && !prepared.probe.diagnostic.empty()) {
        Log(LogLevel::Warning, L"probe", L"fallback reason=" + prepared.probe.diagnostic);
    }
    if (media.hasVideo) {
        const auto& color = media.videoColor;
        Log(LogLevel::Info,
            L"probe",
            L"color primaries=" + ToDisplayString(color.primaries) +
                L" transfer=" + ToDisplayString(color.transfer) +
                L" matrix=" + ToDisplayString(color.matrix) +
                L" range=" + ToDisplayString(color.range));
        if (color.masteringDisplay.hasPrimaries || color.masteringDisplay.hasLuminance) {
            Log(LogLevel::Info, L"probe", L"mastering_display " + FormatMasteringDisplay(color.masteringDisplay));
        }
        if (color.contentLight.hasValues) {
            Log(LogLevel::Info, L"probe", L"content_light " + FormatContentLight(color.contentLight));
        }
    }
    for (const auto& stream : media.streams) {
        Log(LogLevel::Info,
            stream.kind,
            L"stream=" + std::to_wstring(stream.index) +
                L" codec=" + stream.codec +
                L" details=" + stream.details);
    }
    if (prepared.extractPreview && media.hasVideo) {
        if (prepared.preview.generated) {
            Log(LogLevel::Info, L"preview", L"image=" + prepared.preview.imagePath.wstring());
        } else {
            Log(LogLevel::Warning, L"preview", L"skipped reason=" + prepared.preview.diagnostic);
        }
    } else if (media.hasVideo) {
        Log(LogLevel::Debug, L"preview", L"skipped reason=deferred");
    }
    Log(LogLevel::Info, L"decoder", L"planned=" + plan.videoDecoder + L" reason=" + plan.videoReason);
    Log(LogLevel::Info, L"renderer", L"mode=" + plan.videoMode + L" tone_mapping=" + ToDisplayString(toneMapping));
    Log(LogLevel::Info, L"audio", L"output=" + plan.audioOutput + L" reason=" + plan.audioReason);
    return true;
}

void PlayerController::BeginOpen() {
    std::scoped_lock lock(mutex_);
    media_.reset();
    committedPosition_ = std::chrono::milliseconds{0};
    playbackRate_ = 1.0;
    state_ = PlaybackState::Empty;
    lastError_.clear();
}

void PlayerController::Close() {
    std::scoped_lock lock(mutex_);
    media_.reset();
    committedPosition_ = std::chrono::milliseconds{0};
    playbackRate_ = 1.0;
    state_ = PlaybackState::Empty;
    lastError_.clear();
    Log(LogLevel::Info, L"core", L"media closed");
}

void PlayerController::Play() {
    std::scoped_lock lock(mutex_);
    if (!media_.has_value()) {
        return;
    }
    if (state_ == PlaybackState::Playing) {
        return;
    }
    if (state_ == PlaybackState::Stopped && media_->duration.count() > 0 && committedPosition_ >= media_->duration) {
        committedPosition_ = std::chrono::milliseconds{0};
    }
    playStartedAt_ = std::chrono::steady_clock::now();
    state_ = PlaybackState::Playing;
    Log(LogLevel::Info, L"transport", L"play");
}

void PlayerController::Pause() {
    std::scoped_lock lock(mutex_);
    if (state_ != PlaybackState::Playing) {
        return;
    }
    CommitPositionLocked(CurrentPositionLocked());
    state_ = PlaybackState::Paused;
    Log(LogLevel::Info, L"transport", L"pause position=" + FormatTimecode(committedPosition_));
}

void PlayerController::TogglePlayPause() {
    const auto snapshot = Snapshot();
    if (snapshot.state == PlaybackState::Playing) {
        Pause();
    } else {
        Play();
    }
}

void PlayerController::Stop() {
    std::scoped_lock lock(mutex_);
    if (!media_.has_value()) {
        return;
    }
    committedPosition_ = std::chrono::milliseconds{0};
    state_ = PlaybackState::Stopped;
    Log(LogLevel::Info, L"transport", L"stop");
}

void PlayerController::Seek(const std::chrono::milliseconds position) {
    std::scoped_lock lock(mutex_);
    if (!media_.has_value()) {
        return;
    }
    CommitPositionLocked(position);
    if (state_ == PlaybackState::Playing) {
        playStartedAt_ = std::chrono::steady_clock::now();
    }
    Log(LogLevel::Info, L"transport", L"seek position=" + FormatTimecode(committedPosition_));
}

void PlayerController::SeekRelative(const std::chrono::milliseconds delta) {
    std::scoped_lock lock(mutex_);
    if (!media_.has_value()) {
        return;
    }
    CommitPositionLocked(CurrentPositionLocked() + delta);
    if (state_ == PlaybackState::Playing) {
        playStartedAt_ = std::chrono::steady_clock::now();
    }
    Log(LogLevel::Info, L"transport", L"seek_relative position=" + FormatTimecode(committedPosition_));
}

void PlayerController::SetVolume(const double volume) {
    std::scoped_lock lock(mutex_);
    volume_ = std::clamp(volume, 0.0, 1.0);
    std::wostringstream stream;
    stream << L"volume=" << static_cast<int>(volume_ * 100.0) << L"%";
    Log(LogLevel::Info, L"audio", stream.str());
}

void PlayerController::SetPlaybackRate(const double rate) {
    std::scoped_lock lock(mutex_);
    const double clamped = std::clamp(rate, 0.25, 4.0);
    if (std::abs(playbackRate_ - clamped) < 0.001) {
        return;
    }

    if (state_ == PlaybackState::Playing) {
        CommitPositionLocked(CurrentPositionLocked());
        playStartedAt_ = std::chrono::steady_clock::now();
    }
    playbackRate_ = clamped;

    std::wostringstream stream;
    stream << L"rate=" << playbackRate_ << L"x";
    Log(LogLevel::Info, L"transport", stream.str());
}

void PlayerController::SetError(std::wstring message) {
    std::scoped_lock lock(mutex_);
    if (!media_.has_value()) {
        return;
    }
    CommitPositionLocked(CurrentPositionLocked());
    state_ = PlaybackState::Error;
    lastError_ = std::move(message);
    Log(LogLevel::Error, L"transport", L"error " + lastError_);
}

void PlayerController::UpdateClock() {
    std::scoped_lock lock(mutex_);
    if (state_ != PlaybackState::Playing || !media_.has_value() || media_->duration.count() <= 0) {
        return;
    }

    const auto position = CurrentPositionLocked();
    if (position >= media_->duration) {
        committedPosition_ = media_->duration;
        state_ = PlaybackState::Stopped;
        Log(LogLevel::Info, L"transport", L"completed");
    }
}

void PlayerController::SyncClock(const std::chrono::milliseconds position) {
    std::scoped_lock lock(mutex_);
    if (!media_.has_value()) {
        return;
    }
    CommitPositionLocked(position);
    if (state_ == PlaybackState::Playing) {
        playStartedAt_ = std::chrono::steady_clock::now();
    }
}

PlaybackSessionSnapshot PlayerController::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return PlaybackSessionSnapshot{
        state_,
        media_,
        CurrentPositionLocked(),
        volume_,
        playbackRate_,
        lastError_,
    };
}

CapabilityReport PlayerController::CollectCapabilityReport() const {
    return CapabilityDetector::CollectBasic();
}

PlayerSettings PlayerController::Settings() const {
    std::scoped_lock lock(mutex_);
    return settings_;
}

void PlayerController::ApplySettings(const PlayerSettings& settings) {
    std::scoped_lock lock(mutex_);
    settings_ = settings;
    Log(LogLevel::Info, L"settings", L"settings applied");
}

std::shared_ptr<InMemoryLogSink> PlayerController::LogSink() const {
    return logSink_;
}

std::chrono::milliseconds PlayerController::CurrentPositionLocked() const {
    if (state_ != PlaybackState::Playing) {
        return committedPosition_;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - playStartedAt_);
    const auto duration = media_.has_value() ? media_->duration : std::chrono::milliseconds{0};
    return ClampDuration(committedPosition_ + ScaleDuration(elapsed, playbackRate_), duration);
}

void PlayerController::CommitPositionLocked(const std::chrono::milliseconds value) {
    const auto duration = media_.has_value() ? media_->duration : std::chrono::milliseconds{0};
    committedPosition_ = ClampDuration(value, duration);
}

void PlayerController::Log(const LogLevel level, const std::wstring& category, const std::wstring& message) const {
    if (logSink_) {
        logSink_->Write(level, category, message);
    }
}

}  // namespace anvil::playback
