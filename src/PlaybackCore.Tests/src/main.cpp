#include "AnvilPlayer/Playback/PlayerController.h"
#include "AnvilPlayer/Playback/PlaybackPlan.h"
#include "AnvilPlayer/App/video_texture_sampling_math.h"

#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <process.h>

using anvil::playback::PlaybackState;
using anvil::playback::PlayerController;
using anvil::playback::InMemoryLogSink;
using anvil::playback::LogLevel;

namespace {

void AssertNear(const float actual, const float expected) {
    assert(std::abs(actual - expected) < 0.000001f);
}

void TestVideoTextureSamplingRegion() {
    const auto padded = anvil::app::BuildVideoTextureSamplingRegion(3832, 1592, 3840, 1664);
    assert(padded.valid);
    assert(padded.visibleWidth == 3832);
    assert(padded.visibleHeight == 1592);
    AssertNear(padded.uvRect.left, 0.0f);
    AssertNear(padded.uvRect.top, 0.0f);
    AssertNear(padded.uvRect.right, 3832.0f / 3840.0f);
    AssertNear(padded.uvRect.bottom, 1592.0f / 1664.0f);

    const auto exact = anvil::app::BuildVideoTextureSamplingRegion(3840, 2160, 3840, 2160);
    assert(exact.valid);
    AssertNear(exact.uvRect.left, 0.0f);
    AssertNear(exact.uvRect.top, 0.0f);
    AssertNear(exact.uvRect.right, 1.0f);
    AssertNear(exact.uvRect.bottom, 1.0f);

    const auto cropped = anvil::app::BuildVideoTextureSamplingRegion(
        3824, 2148, 3840, 2176, 8, 4);
    assert(cropped.valid);
    assert(cropped.visibleWidth == 3816);
    assert(cropped.visibleHeight == 2144);
    AssertNear(cropped.uvRect.left, 8.0f / 3840.0f);
    AssertNear(cropped.uvRect.top, 4.0f / 2176.0f);
    AssertNear(cropped.uvRect.right, 3824.0f / 3840.0f);
    AssertNear(cropped.uvRect.bottom, 2148.0f / 2176.0f);

    assert(!anvil::app::BuildVideoTextureSamplingRegion(3840, 2160, 3839, 2160).valid);
    assert(!anvil::app::BuildVideoTextureSamplingRegion(0, 2160, 3840, 2160).valid);
    assert(!anvil::app::BuildVideoTextureSamplingRegion(3840, 2160, 3840, 2160, -1, 0).valid);
}

std::filesystem::path MakeTempMediaFile() {
    static int counter = 0;
    auto path = std::filesystem::temp_directory_path() /
        (L"anvil-player-core-test-" + std::to_wstring(_getpid()) + L"-" + std::to_wstring(counter++) + L".mp4");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "anvil";
    return path;
}

void TestOpenAndTransport() {
    PlayerController controller;
    const auto path = MakeTempMediaFile();

    assert(controller.OpenMedia(path));
    auto snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Ready);
    assert(snapshot.media.has_value());
    assert(snapshot.media->container == L"MP4");

    controller.Play();
    snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Playing);

    controller.Pause();
    snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Paused);

    controller.Stop();
    snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Stopped);
    assert(snapshot.position.count() == 0);

    std::filesystem::remove(path);
}

void TestSeekAndVolumeClamp() {
    PlayerController controller;
    const auto path = MakeTempMediaFile();
    assert(controller.OpenMedia(path));

    controller.Seek(std::chrono::hours{10});
    auto snapshot = controller.Snapshot();
    assert(snapshot.media.has_value());
    assert(snapshot.position == snapshot.media->duration);

    controller.SeekRelative(-std::chrono::hours{20});
    snapshot = controller.Snapshot();
    assert(snapshot.position.count() == 0);

    controller.SetVolume(2.0);
    assert(controller.Snapshot().volume == 1.0);

    controller.SetVolume(-1.0);
    assert(controller.Snapshot().volume == 0.0);

    controller.SetPlaybackRate(2.0);
    assert(controller.Snapshot().playbackRate == 2.0);

    controller.SetPlaybackRate(10.0);
    assert(controller.Snapshot().playbackRate == 4.0);

    std::filesystem::remove(path);
}

void TestMissingFileLogsError() {
    PlayerController controller;
    const auto missing = std::filesystem::temp_directory_path() /
        (L"anvil-player-missing-" + std::to_wstring(_getpid()) + L".mkv");
    std::filesystem::remove(missing);
    assert(!controller.OpenMedia(missing));
    const auto snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Error);
    assert(!snapshot.lastError.empty());
    assert(!controller.LogSink()->Entries().empty());
}

void TestLogFilteringAndFileOutput() {
    auto logPath = std::filesystem::temp_directory_path() /
        (L"anvil-player-log-test-" + std::to_wstring(_getpid()) + L".log");
    std::filesystem::remove(logPath);

    InMemoryLogSink sink(LogLevel::Info);
    sink.SetFilePath(logPath);
    sink.Write(LogLevel::Debug, L"test", L"hidden");
    assert(sink.Entries().empty());
    assert(!std::filesystem::exists(logPath));

    sink.Write(LogLevel::Info, L"test", L"visible");
    assert(sink.Entries().size() == 1);
    assert(std::filesystem::exists(logPath));
    assert(std::filesystem::file_size(logPath) > 0);

    sink.SetMinimumLevel(LogLevel::Debug);
    sink.Write(LogLevel::Debug, L"test", L"debug-visible");
    assert(sink.Entries().size() == 2);
    assert(sink.FormatLatest(2).find(L"debug-visible") != std::wstring::npos);

    std::filesystem::remove(logPath);
}

void TestInvalidMediaFallsBackToExtensionProbe() {
    PlayerController controller;
    const auto path = MakeTempMediaFile();
    assert(controller.OpenMedia(path));
    const auto snapshot = controller.Snapshot();
    assert(snapshot.media.has_value());
    assert(snapshot.media->duration == std::chrono::minutes{90});
    assert(snapshot.media->videoCodec.find(L"probe pending") != std::wstring::npos);
    assert(snapshot.media->previewImagePath.empty());
    std::filesystem::remove(path);
}

void TestRealAvformatProbeWhenFfmpegToolIsAvailable() {
    if (_wsystem(L"ffmpeg -version >NUL 2>NUL") != 0) {
        return;
    }

    const auto path = std::filesystem::temp_directory_path() /
        (L"anvil-player-core-real-probe-" + std::to_wstring(_getpid()) + L".mp4");
    const std::wstring command =
        L"ffmpeg -hide_banner -loglevel error -y "
        L"-f lavfi -i testsrc2=size=64x36:rate=15:duration=1 "
        L"-f lavfi -i sine=frequency=440:duration=1 "
        L"-c:v mpeg4 -c:a aac \"" +
        path.wstring() + L"\" >NUL 2>NUL";

    if (_wsystem(command.c_str()) != 0) {
        return;
    }

    PlayerController controller;
    assert(controller.OpenMedia(path));
    const auto snapshot = controller.Snapshot();
    assert(snapshot.media.has_value());
    assert(snapshot.media->container == L"MP4");
    assert(snapshot.media->duration >= std::chrono::milliseconds{900});
    assert(snapshot.media->duration <= std::chrono::milliseconds{1200});
    assert(snapshot.media->videoCodec == L"MPEG-4 Part 2");
    assert(snapshot.media->videoWidth == 64);
    assert(snapshot.media->videoHeight == 36);
    assert(snapshot.media->videoFrameRate >= 14.5);
    assert(snapshot.media->videoFrameRate <= 15.5);
    assert(snapshot.media->audioCodec == L"AAC");
    assert(snapshot.media->selectedDecodePath == L"ffmpeg_software");
    assert(snapshot.media->streams.size() >= 2);
    assert(!snapshot.media->previewImagePath.empty());
    assert(std::filesystem::exists(snapshot.media->previewImagePath));
    assert(std::filesystem::file_size(snapshot.media->previewImagePath) > 0);

    std::filesystem::remove(path);
}

void TestCapabilityReportHasD3DShape() {
    PlayerController controller;
    const auto report = controller.CollectCapabilityReport();
    assert(!report.gpu.adapterName.empty());
    assert(!report.gpu.d3dFeatureLevel.empty());
    assert(!report.gpu.hardwareDecodeProfiles.empty());
    assert(!report.display.colorSpace.empty());
}

void TestHdrPlaybackPlanUsesStructuredColorMetadata() {
    anvil::playback::MediaDescriptor media;
    media.hasVideo = true;
    media.videoCodec = L"HEVC";
    media.videoColor.primaries = anvil::playback::VideoColorPrimaries::Bt2020;
    media.videoColor.transfer = anvil::playback::VideoTransferCharacteristic::Pq;
    media.videoColor.matrix = anvil::playback::VideoMatrixCoefficients::Bt2020Ncl;
    media.videoColor.range = anvil::playback::VideoColorRange::Limited;

    anvil::playback::PlayerSettings settings;
    anvil::playback::CapabilityReport capabilities;
    capabilities.display.hdrEnabled = true;

    auto plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoDecoder == L"ffmpeg_d3d11va");
    assert(plan.videoMode == L"hdr10_output");

    media.videoColor.transfer = anvil::playback::VideoTransferCharacteristic::Hlg;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoMode == L"hlg_output");

    capabilities.display.hdrEnabled = false;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoMode == L"tone_mapped_sdr");

    capabilities.display.hdrEnabled = true;
    settings.video.hdrOutput = anvil::playback::HdrOutputMode::ForceSdr;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoMode == L"tone_mapped_sdr");
}

void TestDolbyVisionPlaybackPlanUsesSoftwareReshape() {
    anvil::playback::MediaDescriptor media;
    media.hasVideo = true;
    media.videoCodec = L"HEVC";
    media.hdrFormat = L"Dolby Vision Profile 5";
    media.dolbyVisionDetected = true;

    anvil::playback::PlayerSettings settings;
    anvil::playback::CapabilityReport capabilities;
    capabilities.display.hdrEnabled = true;

    auto plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoDecoder == L"ffmpeg_software");
    assert(plan.videoReason == L"dolby_vision_software_decode_for_reshape");
    assert(plan.videoMode == L"dolby_vision_software_reshape");

    settings.video.dolbyVision = anvil::playback::DolbyVisionMode::Off;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoDecoder == L"ffmpeg_d3d11va");
    assert(plan.videoMode == L"dolby_vision_disabled_tone_map");
}

}  // namespace

int main() {
    TestVideoTextureSamplingRegion();
    TestOpenAndTransport();
    TestSeekAndVolumeClamp();
    TestMissingFileLogsError();
    TestLogFilteringAndFileOutput();
    TestInvalidMediaFallsBackToExtensionProbe();
    TestRealAvformatProbeWhenFfmpegToolIsAvailable();
    TestCapabilityReportHasD3DShape();
    TestHdrPlaybackPlanUsesStructuredColorMetadata();
    TestDolbyVisionPlaybackPlanUsesSoftwareReshape();
    std::cout << "PlaybackCore tests passed\n";
    return 0;
}
