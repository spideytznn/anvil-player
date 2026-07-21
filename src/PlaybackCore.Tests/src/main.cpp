#include <winsock2.h>
#include <ws2tcpip.h>

#include "AnvilPlayer/Playback/PlayerController.h"
#include "AnvilPlayer/Playback/PlaybackPlan.h"
#include "AnvilPlayer/App/color_metadata_util.h"
#include "AnvilPlayer/App/emby_report_relay.h"
#include "AnvilPlayer/App/frame_interpolation_policy.h"
#include "AnvilPlayer/App/playback_timing_math.h"
#include "AnvilPlayer/App/software_yuv_upload_layout.h"
#include "AnvilPlayer/App/video_texture_sampling_math.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
}

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <process.h>
#include <thread>
#include <vector>

using anvil::playback::PlaybackState;
using anvil::playback::PlayerController;
using anvil::playback::InMemoryLogSink;
using anvil::playback::LogLevel;

namespace {

void AssertNear(const float actual, const float expected) {
    assert(std::abs(actual - expected) < 0.000001f);
}

void TestEmbyReportRelaySurvivesLostDeliveryUntilMatchingAck() {
    anvil::app::EmbyReportRelayState relay;
    relay.Queue(L"report-a", L"{\"id\":\"report-a\"}");

    assert(relay.HasPending());
    assert(relay.ReportId() == L"report-a");
    assert(relay.ReportJson() == L"{\"id\":\"report-a\"}");
    for (int attempt = 1; attempt <= 100; ++attempt) {
        assert(relay.NoteDeliveryAttempt() == attempt);
    }
    // Exhausting a delivery window must not discard a report: the player can
    // request it again after a cold start or WebView recreation.
    assert(relay.HasPending());
    relay.RestartDelivery();
    assert(relay.DeliveryAttempts() == 0);

    assert(!relay.Acknowledge(L""));
    assert(!relay.Acknowledge(L"report-b"));
    assert(relay.HasPending());
    assert(relay.Acknowledge(L"report-a"));
    assert(!relay.HasPending());

    // A newer playback supersedes the native pending copy, and a late ACK for
    // the previous playback must never consume the new report.
    relay.Queue(L"report-a", L"{\"id\":\"report-a\"}");
    relay.Queue(L"report-b", L"{\"id\":\"report-b\"}");
    assert(!relay.Acknowledge(L"report-a"));
    assert(relay.HasPending());
    assert(relay.ReportId() == L"report-b");
    assert(relay.Acknowledge(L"report-b"));
    assert(!relay.HasPending());
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

void TestSoftwareYuvUploadLayoutContract() {
    using anvil::app::BuildSoftwareYuvUploadLayout;
    using anvil::app::SoftwareYuvTextureFormat;

    const auto nv12 = BuildSoftwareYuvUploadLayout(4, 2, 4, 4, 8, 12);
    assert(nv12.valid);
    assert(nv12.format == SoftwareYuvTextureFormat::Nv12);
    assert(nv12.lumaOffset == 0);
    assert(nv12.chromaOffset == 8);
    assert(nv12.lumaRowBytes == 4);
    assert(nv12.chromaRowBytes == 4);
    assert(nv12.lumaRows == 2);
    assert(nv12.chromaRows == 1);

    const auto paddedP010 = BuildSoftwareYuvUploadLayout(4, 2, 12, 12, 10, 36);
    assert(paddedP010.valid);
    assert(paddedP010.format == SoftwareYuvTextureFormat::P010);
    assert(paddedP010.chromaOffset == 24);
    assert(paddedP010.lumaRowBytes == 8);
    assert(paddedP010.chromaRowBytes == 8);

    assert(!BuildSoftwareYuvUploadLayout(3, 2, 6, 6, 10, 18).valid);
    assert(!BuildSoftwareYuvUploadLayout(4, 3, 8, 8, 10, 40).valid);
    assert(!BuildSoftwareYuvUploadLayout(4, 2, 7, 8, 10, 24).valid);
    assert(!BuildSoftwareYuvUploadLayout(4, 2, 8, 7, 10, 24).valid);
    assert(!BuildSoftwareYuvUploadLayout(4, 2, 8, 8, 12, 24).valid);
    assert(!BuildSoftwareYuvUploadLayout(4, 2, 8, 8, 10, 23).valid);
}

void TestWasapiEndpointClockMath() {
    using namespace std::chrono_literals;

    const auto normal = anvil::app::EndpointClockMediaPosition(
        5s, 100, 144100, 48000, 1.0);
    assert(normal == 8s);

    const auto doubleRate = anvil::app::EndpointClockMediaPosition(
        5s, 100, 144100, 48000, 2.0);
    assert(doubleRate == 11s);
    assert(anvil::app::EndpointClockMediaPosition(5s, 100, 100, 48000, 1.0) == 5s);
    assert(anvil::app::EndpointClockMediaPosition(5s, 100, 200, 0, 1.0) == 5s);
}

void TestNetworkRebufferPolicyHysteresis() {
    using anvil::app::EvaluateNetworkRebuffer;
    using anvil::app::NetworkRebufferPolicyInput;
    using anvil::app::NetworkRebufferTransition;
    using namespace std::chrono_literals;

    NetworkRebufferPolicyInput input;
    input.networkSource = true;
    input.renderedFrames = 12;
    assert(EvaluateNetworkRebuffer(input) == NetworkRebufferTransition::Enter);

    input.buffering = true;
    input.decodedFrameDepth = 1;
    assert(EvaluateNetworkRebuffer(input) == NetworkRebufferTransition::Hold);

    input.decodedFrameDepth = 2;
    input.packetDepth = 5;
    input.readAheadDuration = 1000ms;
    assert(EvaluateNetworkRebuffer(input) == NetworkRebufferTransition::Exit);

    input.readAheadDuration = 0ms;
    input.packetDepth = 0;
    input.decodedFrameDepth = 1;
    input.decodedFrameCapacity = 1;
    assert(EvaluateNetworkRebuffer(input) == NetworkRebufferTransition::Exit);

    input.decodedFrameDepth = 0;
    input.paused = true;
    assert(EvaluateNetworkRebuffer(input) == NetworkRebufferTransition::Exit);

    input = {};
    input.renderedFrames = 12;
    assert(EvaluateNetworkRebuffer(input) == NetworkRebufferTransition::None);
}

void TestFixed2xInterpolationRespectsRefreshCeiling() {
    using anvil::app::Fixed2xInterpolationMultiplier;

    assert(Fixed2xInterpolationMultiplier(24.0, 48.0) == 2);
    assert(Fixed2xInterpolationMultiplier(24000.0 / 1001.0,
                                          48000.0 / 1001.0) == 2);
    assert(Fixed2xInterpolationMultiplier(30000.0 / 1001.0,
                                          60000.0 / 1001.0) == 2);
    assert(Fixed2xInterpolationMultiplier(30.0, 60000.0 / 1001.0) == 1);
    assert(Fixed2xInterpolationMultiplier(60.0, 120.0) == 2);
    assert(Fixed2xInterpolationMultiplier(60.0, 60.0) == 1);
    assert(Fixed2xInterpolationMultiplier(0.0, 120.0) == 1);
}

void TestInterpolationTensorExtentUsesConfigurableResolutionCap() {
    using anvil::app::SelectCappedInterpolationExtent;

    const auto uhd = SelectCappedInterpolationExtent(3840, 2160, 1080);
    assert(uhd.width == 1920 && uhd.height == 1088);
    const auto fullHd = SelectCappedInterpolationExtent(1920, 1080, 1080);
    assert(fullHd.width == 1920 && fullHd.height == 1088);
    const auto hd = SelectCappedInterpolationExtent(1280, 720, 1080);
    assert(hd.width == 1280 && hd.height == 736);
    const auto sd = SelectCappedInterpolationExtent(854, 480, 1080);
    assert(sd.width == 864 && sd.height == 480);
    const auto ultrawide = SelectCappedInterpolationExtent(2560, 1080, 1080);
    assert(ultrawide.width == 1920 && ultrawide.height == 800);
    const auto portrait = SelectCappedInterpolationExtent(1080, 1920, 1080);
    assert(portrait.width == 608 && portrait.height == 1088);
    assert(SelectCappedInterpolationExtent(0, 1080, 1080).width == 0);

    const auto uhdAt1440p =
        SelectCappedInterpolationExtent(3840, 2160, 1440);
    assert(uhdAt1440p.width == 2560 && uhdAt1440p.height == 1440);
    const auto qhdAt1440p =
        SelectCappedInterpolationExtent(2560, 1440, 1440);
    assert(qhdAt1440p.width == 2560 && qhdAt1440p.height == 1440);
    const auto uhdAt2160p =
        SelectCappedInterpolationExtent(3840, 2160, 2160);
    assert(uhdAt2160p.width == 3840 && uhdAt2160p.height == 2176);

    // Persisted or bridged values are bounded to the supported UI range.
    assert(SelectCappedInterpolationExtent(3840, 2160, 720).width == 1920);
    assert(SelectCappedInterpolationExtent(7680, 4320, 4320).width == 3840);
}

void TestInterpolationEndpointsExcludePresentationOverlays() {
    anvil::app::NativeVideoFrame frame;
    frame.subtitleText = L"presentation only";
    frame.subtitleBitmaps.emplace_back();
    frame.subtitlesPrepared = true;
    frame.color.transfer = anvil::playback::VideoTransferCharacteristic::Srgb;
    auto endpointDovi =
        std::make_shared<anvil::playback::DolbyVisionFrameMetadata>();
    endpointDovi->valid = true;
    endpointDovi->dynamicMetadataFingerprint = 84;
    frame.dovi = endpointDovi;
    frame.timelineSerial = 7;

    anvil::app::StripInterpolationPresentationData(frame);

    assert(frame.subtitleText.empty());
    assert(frame.subtitleBitmaps.empty());
    assert(!frame.subtitlesPrepared);
    // Endpoint color/RPU state remains independent. Only overlays are removed
    // before A and B are mapped into their shared target display domain.
    assert(frame.color.transfer ==
           anvil::playback::VideoTransferCharacteristic::Srgb);
    assert(frame.dovi == endpointDovi);
    assert(frame.dovi->dynamicMetadataFingerprint == 84);
    assert(frame.timelineSerial == 7);

    assert(!anvil::app::IsInterpolationDisplayDomainBoundary(
        true, true, true, true));
    assert(anvil::app::IsInterpolationDisplayDomainBoundary(
        true, false, true, false));
}

void TestInterpolationPresentationChangesInvalidateDisplayDomainFrames() {
    anvil::playback::VideoSettings settings;
    settings.frameInterpolationEnabled = true;
    anvil::playback::DisplayCapabilities display;
    display.hdrEnabled = true;
    display.reportedPeakBrightnessNits = 1000;
    anvil::playback::VideoColorMetadata color;
    color.primaries = anvil::playback::VideoColorPrimaries::Bt2020;
    color.transfer = anvil::playback::VideoTransferCharacteristic::Pq;

    assert(!anvil::app::InterpolationPresentationConfigChanged(
        settings, display, color, settings, display, color));

    auto unrelated = settings;
    unrelated.hardwareDecode = anvil::playback::HardwareDecodeMode::Off;
    assert(!anvil::app::InterpolationPresentationConfigChanged(
        settings, display, color, unrelated, display, color));

    auto toneMapping = settings;
    toneMapping.toneMapping = anvil::playback::ToneMappingMode::PreserveHighlights;
    assert(anvil::app::InterpolationPresentationConfigChanged(
        settings, display, color, toneMapping, display, color));

    auto interpolationResolution = settings;
    interpolationResolution.frameInterpolationMaximumHeight = 1440;
    assert(anvil::app::InterpolationPresentationConfigChanged(
        settings, display, color, interpolationResolution, display, color));

    auto movedDisplay = display;
    movedDisplay.reportedPeakBrightnessNits = 1600;
    assert(anvil::app::InterpolationPresentationConfigChanged(
        settings, display, color, settings, movedDisplay, color));

    auto changedMetadata = color;
    changedMetadata.contentLight.hasValues = true;
    changedMetadata.contentLight.maxContentLightLevelNits = 4000;
    assert(anvil::app::InterpolationPresentationConfigChanged(
        settings, display, color, settings, display, changedMetadata));
}

void TestInterpolationSceneChangePolicy() {
    using anvil::app::IsDolbyVisionSceneBoundary;
    using anvil::app::IsHardSceneCut;
    using anvil::app::SceneChangeMetrics;

    assert(!IsHardSceneCut(SceneChangeMetrics{0.05, 0.20, 0.45, 0.44}));
    // A uniform fade changes almost every sample but is explained by its
    // global luminance shift and must not be classified as a hard cut.
    assert(!IsHardSceneCut(SceneChangeMetrics{0.25, 0.90, 0.60, 0.35}));
    assert(IsHardSceneCut(SceneChangeMetrics{0.28, 0.82, 0.48, 0.50}));

    anvil::playback::DolbyVisionFrameMetadata dovi;
    dovi.valid = true;
    assert(!IsDolbyVisionSceneBoundary(&dovi));
    dovi.sceneRefreshFlag = 1;
    assert(IsDolbyVisionSceneBoundary(&dovi));
    dovi.valid = false;
    assert(!IsDolbyVisionSceneBoundary(&dovi));
    assert(!IsDolbyVisionSceneBoundary(nullptr));
}

std::filesystem::path MakeTempMediaFile() {
    static int counter = 0;
    auto path = std::filesystem::temp_directory_path() /
        (L"anvil-player-core-test-" + std::to_wstring(_getpid()) + L"-" + std::to_wstring(counter++) + L".mp4");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "anvil";
    return path;
}

void TestCapabilityProbeDoesNotBlockRepeatedPrepare() {
#if defined(_DEBUG)
    // The probe worker reads this once before it starts. If capability
    // collection ever moves back onto PrepareMedia's call stack, this test
    // deterministically exceeds the deadline below.
    assert(_wputenv_s(L"ANVIL_PLAYER_TEST_CAPABILITY_DELAY_MS", L"5000") == 0);
#endif

    const auto path = MakeTempMediaFile();
    const auto startedAt = std::chrono::steady_clock::now();
    PlayerController controller;
    const auto initialReport = controller.CollectCapabilityReport();
    const auto first = controller.PrepareMedia(
        path,
        false,
        anvil::playback::MediaProbeOptions{{}, std::chrono::milliseconds{250}});
    const auto second = controller.PrepareMedia(
        path,
        false,
        anvil::playback::MediaProbeOptions{{}, std::chrono::milliseconds{250}});
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

#if defined(_DEBUG)
    assert(_wputenv_s(L"ANVIL_PLAYER_TEST_CAPABILITY_DELAY_MS", L"") == 0);
#endif
    std::filesystem::remove(path);

    assert(first.succeeded);
    assert(second.succeeded);
    assert(!initialReport.gpu.adapterName.empty());
    assert(!initialReport.gpu.d3dFeatureLevel.empty());
    assert(!initialReport.gpu.hardwareDecodeProfiles.empty());
    assert(elapsed < std::chrono::milliseconds{2500});

#if defined(_DEBUG)
    // Keep the process alive until the deliberately delayed worker has run.
    // This catches ABI/lifetime regressions in the real _beginthreadex entry
    // instead of letting the test executable exit while the probe still sleeps.
    std::this_thread::sleep_for(std::chrono::milliseconds{5250});
    const auto completedReport = controller.CollectCapabilityReport();
    assert(!completedReport.display.colorSpace.empty());
    assert(!completedReport.gpu.adapterName.empty());
#endif
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

void TestStartupResumePositionSurvivesPlay() {
    using namespace std::chrono_literals;

    PlayerController controller;
    const auto path = MakeTempMediaFile();
    assert(controller.OpenMedia(path));

    constexpr auto resumePosition = 30min;
    controller.Seek(resumePosition);
    controller.Play();

    const auto snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Playing);
    assert(snapshot.position >= resumePosition);
    assert(snapshot.position < resumePosition + 1s);

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

void TestPrepareCommitKeepsControllerResponsive() {
    PlayerController controller;
    const auto path = MakeTempMediaFile();

    auto prepared = controller.PrepareMedia(path, false);
    assert(prepared.succeeded);
    // Preparation is side-effect free. A UI Snapshot never waits on or sees a
    // partially probed descriptor; only the short commit changes state.
    auto snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Empty);
    assert(!snapshot.media.has_value());

    assert(controller.CommitMedia(std::move(prepared)));
    snapshot = controller.Snapshot();
    assert(snapshot.state == PlaybackState::Ready);
    assert(snapshot.media.has_value());
    std::filesystem::remove(path);
}

void TestCancelledPrepareDoesNotCommit() {
    PlayerController controller;
    const auto path = MakeTempMediaFile();
    std::stop_source cancellation;
    cancellation.request_stop();

    const auto startedAt = std::chrono::steady_clock::now();
    auto prepared = controller.PrepareMedia(
        path,
        false,
        anvil::playback::MediaProbeOptions{cancellation.get_token(), std::chrono::seconds{1}});
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    assert(prepared.probe.cancelled);
    assert(!prepared.succeeded);
    assert(elapsed < std::chrono::milliseconds{100});
    assert(!controller.CommitMedia(std::move(prepared)));
    assert(controller.Snapshot().state == PlaybackState::Empty);
    std::filesystem::remove(path);
}

void TestProbeDeadlineDoesNotBlockSnapshots() {
    WSADATA winsock{};
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    int addressLength = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0);

    std::atomic_bool stopServer{false};
    std::atomic_bool accepted{false};
    std::thread server([&]() {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            accepted.store(true);
            while (!stopServer.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    });

    PlayerController controller;
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(ntohs(address.sin_port)) + L"/stall";
    const auto probeStartedAt = std::chrono::steady_clock::now();
    auto preparation = std::async(std::launch::async, [&]() {
        return controller.PrepareMedia(
            std::filesystem::path{url},
            false,
            anvil::playback::MediaProbeOptions{{}, std::chrono::milliseconds{250}});
    });

    const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!accepted.load() && std::chrono::steady_clock::now() < acceptDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    const auto snapshotStartedAt = std::chrono::steady_clock::now();
    const auto snapshot = controller.Snapshot();
    const auto snapshotElapsed = std::chrono::steady_clock::now() - snapshotStartedAt;
    assert(snapshot.state == PlaybackState::Empty);
    assert(snapshotElapsed < std::chrono::milliseconds{50});

    auto prepared = preparation.get();
    const auto probeElapsed = std::chrono::steady_clock::now() - probeStartedAt;
    stopServer.store(true);
    closesocket(listener);
    server.join();
    WSACleanup();

    assert(prepared.probe.timedOut);
    assert(!prepared.succeeded);
    assert(probeElapsed < std::chrono::seconds{2});
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
    assert(sink.Flush(std::chrono::seconds{2}));
    assert(std::filesystem::exists(logPath));
    assert(std::filesystem::file_size(logPath) > 0);

    sink.SetMinimumLevel(LogLevel::Debug);
    sink.Write(LogLevel::Debug, L"test", L"debug-visible");
    assert(sink.Entries().size() == 2);
    assert(sink.FormatLatest(2).find(L"debug-visible") != std::wstring::npos);

    assert(sink.Flush(std::chrono::seconds{2}));
    sink.SetFilePath({});
    assert(sink.Flush(std::chrono::seconds{2}));
    std::filesystem::remove(logPath);
}

void TestConcurrentLogWritesAreBufferedAndFlushed() {
    const auto logPath = std::filesystem::temp_directory_path() /
        (L"anvil-player-concurrent-log-test-" + std::to_wstring(_getpid()) + L".log");
    std::filesystem::remove(logPath);

    constexpr int threadCount = 4;
    constexpr int entriesPerThread = 100;
    InMemoryLogSink sink(LogLevel::Debug);
    sink.SetFilePath(logPath);

    std::vector<std::thread> writers;
    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        writers.emplace_back([&sink, threadIndex] {
            for (int entryIndex = 0; entryIndex < entriesPerThread; ++entryIndex) {
                sink.Write(LogLevel::Info,
                           L"concurrent",
                           L"thread=" + std::to_wstring(threadIndex) +
                               L" entry=" + std::to_wstring(entryIndex));
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }

    const auto entries = sink.Entries();
    assert(entries.size() == static_cast<std::size_t>(threadCount * entriesPerThread));
    assert(sink.Flush(std::chrono::seconds{5}));
    sink.SetFilePath({});
    assert(sink.Flush(std::chrono::seconds{2}));

    std::ifstream file(logPath, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    assert(contents.size() > 3);
    assert(static_cast<unsigned char>(contents[0]) == 0xEF);
    assert(static_cast<unsigned char>(contents[1]) == 0xBB);
    assert(static_cast<unsigned char>(contents[2]) == 0xBF);
    assert(std::count(contents.begin(), contents.end(), '\n') == threadCount * entriesPerThread);

    file.close();
    std::filesystem::remove(logPath);
}

void TestInMemoryLogRetentionIsBounded() {
    InMemoryLogSink sink(LogLevel::Debug);
    for (int index = 0; index < 700; ++index) {
        sink.Write(LogLevel::Debug, L"bounded", L"entry=" + std::to_wstring(index));
    }

    const auto entries = sink.Entries();
    assert(entries.size() == 500);
    assert(entries.front().message == L"entry=200");
    assert(entries.back().message == L"entry=699");
    const auto latest = sink.LatestEntries(14);
    assert(latest.size() == 14);
    assert(latest.front().message == L"entry=686");
    assert(latest.back().message == L"entry=699");
}

void TestLogRedactsUrlCredentials() {
    InMemoryLogSink sink(LogLevel::Debug);
    sink.Write(LogLevel::Info,
               L"network",
               L"open path=http://media-user:secret-password@example.test/library/movie.mkv");

    const auto entries = sink.Entries();
    assert(entries.size() == 1);
    assert(entries[0].message ==
           L"open path=http://[credentials]@example.test/library/movie.mkv");
    assert(entries[0].message.find(L"media-user") == std::wstring::npos);
    assert(entries[0].message.find(L"secret-password") == std::wstring::npos);

    sink.Write(LogLevel::Info,
               L"network",
               L"open path=https://example.test/stream.mkv?UserId=42&api_key=token-value&Static=true");
    const auto tokenEntries = sink.Entries();
    assert(tokenEntries.size() == 2);
    assert(tokenEntries[1].message.find(L"api_key=[redacted]") != std::wstring::npos);
    assert(tokenEntries[1].message.find(L"token-value") == std::wstring::npos);
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
    assert(plan.videoDecoder == L"ffmpeg_d3d12va");
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

void TestDolbyVisionPlaybackPlanPrefersSystemExtensions() {
    anvil::playback::MediaDescriptor media;
    media.hasVideo = true;
    media.videoCodec = L"HEVC";
    media.hdrFormat = L"Dolby Vision Profile 5";
    media.dolbyVisionDetected = true;

    anvil::playback::PlayerSettings settings;
    anvil::playback::CapabilityReport capabilities;
    capabilities.display.hdrEnabled = true;

    auto plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoDecoder == L"ffmpeg_d3d12va");
    assert(plan.videoReason == L"hardware_decode_candidate");
    assert(plan.videoMode == L"dolby_vision_software_reshape");

    capabilities.codecs.dolbyVisionExtensionDetected = true;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoMode == L"dolby_vision_software_reshape");

    settings.video.dolbyVisionSystemPipelineExperimental = true;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoMode == L"dolby_vision_system_extensions");

    settings.video.dolbyVision = anvil::playback::DolbyVisionMode::Off;
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.videoDecoder == L"ffmpeg_d3d12va");
    assert(plan.videoMode == L"dolby_vision_disabled_tone_map");
}

void TestDisplayMetadataPassthroughDefaults() {
    const auto settings = anvil::playback::MakeDefaultSettings();
    assert(!settings.video.frameInterpolationEnabled);
    assert(settings.video.frameInterpolationMaximumHeight == 1080);
    assert(!settings.video.displayMetadataPassthrough);
    assert(!settings.video.dolbyVisionSystemPipelineExperimental);
    // Zero means auto: use the player window's current monitor peak and let
    // the renderer fall back to 1000 nits only when Windows reports none.
    assert(settings.video.displayPeakBrightnessNits == 0);
}

void TestAudioPassthroughPlanningFallsBackToPcm() {
    anvil::playback::MediaDescriptor media;
    media.hasAudio = true;
    media.audioCodec = L"AC-3";
    auto settings = anvil::playback::MakeDefaultSettings();
    anvil::playback::CapabilityReport capabilities;

    auto plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.audioOutput == L"wasapi_shared_pcm");

    settings.audio.passthroughPreferred = true;
    for (const std::wstring codec : {L"AC-3", L"E-AC-3", L"TrueHD", L"DTS"}) {
        media.audioCodec = codec;
        plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
        assert(plan.audioOutput == L"wasapi_exclusive_bitstream");
    }

    media.audioCodec = L"AAC";
    plan = anvil::playback::PlaybackPlanner::Build(media, settings, capabilities);
    assert(plan.audioOutput == L"wasapi_shared_pcm");
    assert(plan.audioReason == L"codec_not_bitstream_candidate_fallback_pcm");
}

void TestHdr10PlusFrameMetadataExtraction() {
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    AVDynamicHDRPlus* source = av_dynamic_hdr_plus_create_side_data(frame);
    assert(source);
    source->itu_t_t35_country_code = 0xB5;
    source->application_version = 0;
    source->num_windows = 1;
    source->targeted_system_display_maximum_luminance = AVRational{1000, 1};

    auto& transform = source->params[0];
    transform.maxscl[0] = AVRational{1, 10};
    transform.maxscl[1] = AVRational{8, 100};
    transform.maxscl[2] = AVRational{6, 100};
    transform.average_maxrgb = AVRational{3, 100};
    transform.tone_mapping_flag = 1;
    transform.knee_point_x = AVRational{1, 4};
    transform.knee_point_y = AVRational{3, 4};
    transform.num_bezier_curve_anchors = 2;
    transform.bezier_curve_anchors[0] = AVRational{2, 5};
    transform.bezier_curve_anchors[1] = AVRational{4, 5};
    transform.color_saturation_mapping_flag = 1;
    transform.color_saturation_weight = AVRational{9, 8};

    const auto metadata = anvil::app::ExtractHdr10PlusMetadata(frame);
    assert(metadata);
    assert(metadata->valid);
    assert(metadata->numWindows == 1);
    assert(metadata->anchorCount == 2);
    AssertNear(metadata->targetedPeakNits, 1000.0f);
    assert(std::abs(metadata->sourcePeakNits - 1000.0f) < 0.01f);
    assert(std::abs(metadata->averageMaxRgbNits - 300.0f) < 0.01f);
    AssertNear(metadata->kneePointX, 0.25f);
    AssertNear(metadata->kneePointY, 0.75f);
    AssertNear(metadata->bezierAnchors[0], 0.4f);
    AssertNear(metadata->bezierAnchors[1], 0.8f);
    AssertNear(metadata->saturationWeight, 1.125f);

    transform.tone_mapping_flag = 0;
    const auto generated = anvil::app::ExtractHdr10PlusMetadata(frame);
    assert(generated);
    assert(generated->valid);
    assert(!generated->toneMappingPresent);
    assert(generated->anchorCount == 0);

    transform.maxscl[0] = AVRational{0, 1};
    transform.maxscl[1] = AVRational{0, 1};
    transform.maxscl[2] = AVRational{0, 1};
    transform.num_distribution_maxrgb_percentiles = 0;
    const auto unavailable = anvil::app::ExtractHdr10PlusMetadata(frame);
    assert(unavailable);
    assert(!unavailable->valid);
    av_frame_free(&frame);
}

}  // namespace

int main() {
    TestEmbyReportRelaySurvivesLostDeliveryUntilMatchingAck();
    TestVideoTextureSamplingRegion();
    TestSoftwareYuvUploadLayoutContract();
    TestWasapiEndpointClockMath();
    TestNetworkRebufferPolicyHysteresis();
    TestFixed2xInterpolationRespectsRefreshCeiling();
    TestInterpolationTensorExtentUsesConfigurableResolutionCap();
    TestInterpolationEndpointsExcludePresentationOverlays();
    TestInterpolationPresentationChangesInvalidateDisplayDomainFrames();
    TestInterpolationSceneChangePolicy();
    TestCapabilityProbeDoesNotBlockRepeatedPrepare();
    TestOpenAndTransport();
    TestStartupResumePositionSurvivesPlay();
    TestSeekAndVolumeClamp();
    TestMissingFileLogsError();
    TestPrepareCommitKeepsControllerResponsive();
    TestCancelledPrepareDoesNotCommit();
    TestProbeDeadlineDoesNotBlockSnapshots();
    TestLogFilteringAndFileOutput();
    TestConcurrentLogWritesAreBufferedAndFlushed();
    TestInMemoryLogRetentionIsBounded();
    TestLogRedactsUrlCredentials();
    TestInvalidMediaFallsBackToExtensionProbe();
    TestRealAvformatProbeWhenFfmpegToolIsAvailable();
    TestCapabilityReportHasD3DShape();
    TestHdrPlaybackPlanUsesStructuredColorMetadata();
    TestDolbyVisionPlaybackPlanPrefersSystemExtensions();
    TestDisplayMetadataPassthroughDefaults();
    TestAudioPassthroughPlanningFallsBackToPcm();
    TestHdr10PlusFrameMetadataExtraction();
    std::cout << "PlaybackCore tests passed\n";
    return 0;
}
