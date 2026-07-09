#pragma once

#include "AnvilPlayer/App/app_messages.h"
#include "AnvilPlayer/App/d3d11_video_renderer.h"
#include "AnvilPlayer/App/embedded_ffplay.h"
#include "AnvilPlayer/App/external_video_decoder.h"
#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/icon_painter.h"
#include "AnvilPlayer/App/log_sink_ptr.h"
#include "AnvilPlayer/App/ui_draw.h"
#include "AnvilPlayer/App/ui_types.h"
#include "AnvilPlayer/App/wasapi_audio_player.h"
#include "AnvilPlayer/App/web_ui_host.h"
#include "AnvilPlayer/Playback/PlayerController.h"
#include "resource.h"

#include <shellapi.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace anvil::app {

struct UiMotionValue {
    double amount = 0.0;
    double startAmount = 0.0;
    double target = 0.0;
    std::chrono::steady_clock::time_point startedAt{};
};

// The native Win32 main window. Owns the PlayerController, the three playback
// backends (native FFmpeg/D3D11, embedded ffplay, raw-frame bridge), the
// D3D11 video host window, layout state, and the GDI+ paint pipeline.
// Implementation is split across main_window.cpp (lifecycle/messages/runtime/
// transport/chrome), main_window_layout.cpp (layout + video host sizing),
// main_window_input.cpp (mouse/key/drag handlers), and main_window_paint.cpp
// (Paint + all Draw* methods).
class MainWindow {
public:
    void ConfigureLogging(anvil::playback::LogLevel minimumLevel);
    void SetBackend(PlaybackBackend backend);
    void SetInitialVideoTrackSelection(int selectedTrackIndex);
    void SetWebUiEnabled(bool enabled);

    bool Create(HINSTANCE instance);
    void Show(int commandShow) const;
    void OpenInitialPath(const std::filesystem::path& path, bool autoplay);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK VideoHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK FullscreenOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TransportOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK BufferingOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK BufferingHudOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SubtitleMenuOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HdrToneCurveWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void CALLBACK PlaybackTimerQueueProc(PVOID context, BOOLEAN timerOrWaitFired);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleHdrToneCurveWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    static std::filesystem::path DefaultLogPath();
    static std::filesystem::path RecentMediaPath();
    void LogApp(anvil::playback::LogLevel level, const std::wstring& message) const;
    void LogRuntime(anvil::playback::LogLevel level, const std::wstring& category, const std::wstring& message) const;
    void MaybeLogNativeSchedulerStats(const NativeVideoQueueStats& stats);
    void ResetNativeBufferingWatchdog();
    bool CheckNativeBufferingWatchdog(const anvil::playback::PlaybackSessionSnapshot& snapshot,
                                      const NativeVideoQueueStats& stats);
    void FailPlaybackRuntime(const std::wstring& message);
    void StartUiAnimationTimer() const;
    bool SidebarAnimationActive() const;
    void UpdateUiAnimations();
    bool TryCreateWebUi();
    std::filesystem::path WebUiRoot() const;
    void HandleWebUiMessage(std::wstring_view message);
    void PostWebUiState(bool force = true) const;
    std::wstring BuildWebUiStateJson() const;
    void SetButtonHoverTarget(int buttonIndex, bool hovered);
    void ClearButtonHoverTargets();
    void TriggerButtonPress(int buttonIndex);
    double ButtonHoverAmount(Command command) const;
    double ButtonPressAmount(Command command) const;
    void SetVolumeSliderHover(bool hovered);
    bool IsPointInteractive(POINT point) const;
    void SetProgressHover(bool hovered);
    void ToggleSidebar();
    void SetSubtitleMenuTarget(bool visible);
    void HideSubtitleMenu();
    bool ShouldShowFullscreenTransport(const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    bool IsFullscreenTransportActivationPoint(POINT point) const;
    void UpdateFullscreenTransportCursorPolling();
    void ShowFullscreenTransport(const wchar_t* reason = L"unspecified");
    void HideFullscreenTransportIfIdle();
    void SetFullscreenTransportTarget(bool visible);
    std::wstring FullscreenTransportDebugState(const wchar_t* reason) const;
    void MarkSettingsScrollbarActive();
    void MarkSubtitleMenuScrollbarActive();
    bool ScrollbarFadeActive(std::chrono::steady_clock::time_point now) const;
    void BeginVideoPress(POINT point);
    void CompleteVideoLongPress();
    void FinishVideoPress(POINT point);
    void CancelVideoPress();
    void SetTemporaryPlaybackRate(double rate);

    std::wstring RuntimeLabel() const;
    std::wstring RuntimeShortLabel() const;

    void SetPlaybackTimer(bool playing);
    void OnPlaybackTimerTick();
    void InvalidatePlaybackAreas() const;
    void InvalidateVideoSurface() const;
    void InvalidateTransportArea() const;
    void InvalidateFullscreenOverlay() const;
    void InvalidateHdrToneCurveEditor() const;

    const anvil::playback::CapabilityReport& CachedCapabilities();
    void RefreshCapabilityCache();
    void RenderPlaybackTick(const anvil::playback::PlaybackSessionSnapshot& snapshot, bool forceRefresh = true);
    bool CurrentMediaHasHdrControls() const;
    bool CurrentMediaHasCmv4Control() const;
    bool CurrentCmv4ControlEnabled(const anvil::playback::PlayerSettings& settings) const;
    bool HdrToneCurveAvailable(const anvil::playback::PlayerSettings& settings) const;
    bool Cmv4ApproxActiveForPlayback(const anvil::playback::PlaybackSessionSnapshot& snapshot,
                                     const anvil::playback::VideoSettings& settings) const;
    bool ApplyNativeColorSettingsLive(const anvil::playback::PlaybackSessionSnapshot& snapshot);
    void ScheduleNativeColorSettingsRefresh(bool requiresDecoderRefresh);
    void ApplyNativeColorSettingsRefresh(bool requiresDecoderRefresh);
    bool NativeHdrOutputToggleRequiresDecoderRestart(const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    bool NativeCmv4ToggleRequiresDecoderRestart(const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    bool NativeCmv4ToggleNeedsDecoderRefresh(const anvil::playback::PlaybackSessionSnapshot& snapshot,
                                             const anvil::playback::VideoSettings& settings) const;
    int SettingsVideoFieldCount() const;
    void ApplyDefaultHdrControlsForCurrentMedia();

    int Scale(int value) const;
    RECT PlaybackSurfaceBounds() const;
    void ApplyWindowChrome() const;

    // main_window_layout.cpp
    void MarkLayoutDirty();
    void EnsureLayout();
    RECT SettingsContentViewport() const;
    int SettingsContentHeight(const anvil::playback::PlayerSettings& settings, RECT viewport) const;
    void UpdateSettingsScrollLayout(const anvil::playback::PlayerSettings& settings);
    void UpdateLayout();
    void UpdateInspectorPathItems(const anvil::playback::PlaybackSessionSnapshot& snapshot);
    void UpdateVideoHost();
    void EnsureFullscreenOverlay();
    void UpdateFullscreenOverlay();
    void EnsureTransportOverlay();
    void UpdateTransportOverlay();
    void EnsureBufferingOverlay();
    void UpdateBufferingOverlay(const NativeVideoQueueStats* statsOverride = nullptr);
    void EnsureSubtitleMenuOverlay();
    void UpdateSubtitleMenuOverlay();
    void EnsureHdrToneCurveWindow();
    void UpdateHdrToneCurveFloatingLayout();
    void UpdateSubtitleMenuLayout();
    int HitButton(POINT point) const;
    int HitInspectorPathItem(POINT point) const;

    // main_window_input.cpp
    void OnMouseMove(int x, int y);
    void OnLeftButtonDown(int x, int y);
    void OnLeftButtonUp(int x, int /*y*/);
    void OnMouseWheel(int delta, POINT screenPoint);
    bool ScrollSettingsBy(int delta);
    bool BeginSettingsScrollDrag(POINT point);
    void UpdateSettingsScrollDrag(POINT point);
    void EndSettingsScrollDrag();
    void CancelSettingsScrollDrag();
    RECT ProgressHitRect() const;
    RECT VolumeSliderTrackRect() const;
    RECT VolumeSliderHitRect() const;
    double VolumeFromSliderX(int x) const;
    bool ApplyVolume(double volume, bool restartExternalNow);
    bool BeginVolumeDrag(POINT point);
    void UpdateVolumeDrag(POINT point);
    void EndVolumeDrag(POINT point);
    void CancelVolumeDrag();
    int HitSubtitleMenuItem(POINT point) const;
    bool IsPointInSubtitleMenu(POINT point) const;
    enum class SubtitleMenuAction {
        None,
        TabAudio,
        TabSubtitles,
        TabDanmaku,
        DelayDown,
        DelayUp,
        AddFile,
        SubtitleSizeDown,
        SubtitleSizeUp,
        SubtitleOffsetXDown,
        SubtitleOffsetXUp,
        SubtitleOffsetYDown,
        SubtitleOffsetYUp,
        DanmakuToggle,
        DanmakuMode,
        DanmakuOpacityDown,
        DanmakuOpacityUp,
        DanmakuSpeedDown,
        DanmakuSpeedUp,
        AddDanmakuFile,
    };
    SubtitleMenuAction HitSubtitleMenuAction(POINT point) const;
    HWND HdrToneCurveInteractionWindow() const;
    std::chrono::milliseconds PositionFromProgressX(int x) const;
    void BeginProgressDrag(int x);
    void CancelProgressDrag();
    void CommitProgressDrag();
    bool IsHdrToneCurveVisible() const;
    int HitHdrToneCurvePoint(POINT point, int maxDistancePx) const;
    void UpdateHdrToneCurveHover(POINT point);
    bool HasHdrToneCurveSelection() const;
    int HdrToneCurveSelectionCount() const;
    void ClearHdrToneCurveSelection();
    void SelectHdrToneCurvePoint(int pointIndex);
    void SelectHdrToneCurveRange(int leftX, int rightX);
    bool BeginHdrToneCurveRangeSelection(POINT point);
    void UpdateHdrToneCurveRangeSelection(POINT point);
    void EndHdrToneCurveRangeSelection();
    bool BeginHdrToneCurveDrag(POINT point);
    void StartHdrToneCurveDrag(int pointIndex, bool groupDrag, POINT point);
    void UpdateHdrToneCurveDrag(POINT point);
    void EndHdrToneCurveDrag();
    void CancelHdrToneCurveInteraction();
    bool NudgeHdrToneCurveSelection(double deltaNits);
    void ApplyLiveHdrToneCurveSettings();
    void ApplyHdrToneCurvePoint(int pointIndex, double outputNits);
    void ApplyHdrToneCurvePoints(
        const std::array<double, anvil::playback::kHdrToneCurvePointCount>& outputNits,
        const std::array<bool, anvil::playback::kHdrToneCurvePointCount>& hasOutput);
    void ResetHdrToneCurve();
    void ShowHdrToneCurveWindow();
    void HideHdrToneCurveWindow();
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP drop);
    void Execute(Command command);
    void OpenInspectorPathItem(int itemIndex);
    void OpenFileDialog();
    void OpenLocalFolderDialog();
    void OpenSubtitleFileDialog();
    void OpenDanmakuFileDialog();

    // main_window.cpp runtime + transport
    void StopRuntime(bool clearVideoFrame = true);
    void StopRuntimeAsync(bool clearVideoFrame = true);
    void StopRuntimeBackends();
    void FinishRuntimeStopVisuals(bool clearVideoFrame, bool clearDecoderFrames);
    void WaitForAsyncRuntimeStop();
    bool RuntimeStopInProgress() const;
    bool RuntimeBackendsRunning() const;
    void ClearDeferredRuntimeStart();
    void QueueDeferredRuntimeStart(bool restart, bool waitForPreroll);
    void ContinueRuntimeAfterAsyncStop();
    void RequestRuntimeStart(bool restart, bool waitForPreroll = false);
    void QueuePausedNativeFrameRefresh(bool forceDecoderRestart);
    bool StartRuntime(const anvil::playback::PlaybackSessionSnapshot& snapshot,
                      bool restart,
                      bool waitForPreroll = false);
    void EnsureVideoHost();
    bool StartNativeRuntime(const anvil::playback::PlaybackSessionSnapshot& snapshot,
                            bool restart,
                            bool waitForPreroll = true);
    bool SeekNativeRuntime(const anvil::playback::PlaybackSessionSnapshot& snapshot);
    void ResumeNativeSeekPrerollAudio(std::chrono::milliseconds position);
    bool PrepareNativeEnhancedPlaybackBeforePlay(const anvil::playback::PlaybackSessionSnapshot& snapshot);
    bool CaptureLatestNativeFrame();
    void RenderHeldNativeFrame(bool logRepaint = true);
    void RefreshPausedNativeFrame(const anvil::playback::PlaybackSessionSnapshot& snapshot,
                                  bool forceDecoderRestart = false);
    void OpenPath(const std::filesystem::path& path, bool autoplay = true);
    void LoadRecentMedia();
    void SaveRecentMedia() const;
    void StartPlayback();
    void PausePlayback();
    void TogglePlayback();
    void StopPlayback();
    void RestartPlaybackIfPlaying(bool waitForPreroll = false);
    void SeekRelative(std::chrono::milliseconds delta);
    void SeekToPosition(std::chrono::milliseconds position);
    void SeekFromProgress(int x);
    void ApplyAudioSelection(int selectedTrackIndex);
    void ApplySubtitleSelection(int selectedTrackIndex);
    void ApplySubtitleDelayDelta(int deltaMs);
    void ApplySubtitleFontScaleDelta(double delta);
    void ApplySubtitleOffsetDelta(int deltaX, int deltaY);
    void ApplyLiveSubtitleStyleSettings();
    void ToggleDanmakuEnabled();
    void CycleDanmakuMode();
    void ApplyDanmakuOpacityDelta(int deltaPercent);
    void ApplyDanmakuSpeedDelta(int deltaPercent);
    void CycleSubtitleTrack();
    void ShowSubtitleMenu();
    void ToggleDolbyVisionHdrOutput();
    void ToggleDolbyVisionCmv4Approx();
    void ToggleFullscreen();
    void UpdateInspectorMediaLists(const std::filesystem::path& path);

    // main_window_paint.cpp
    void Paint();
    void PaintFullscreenOverlay(HWND overlay);
    void PaintTransportOverlay(HWND overlay);
    void RenderBufferingHudOverlay(const NativeVideoQueueStats& stats);
    void PaintSubtitleMenuOverlay(HWND overlay);
    void PaintHdrToneCurveWindow(HWND window);
    void DrawTopBar(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawButtons(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawTooltip(HDC hdc) const;
    void ClearPreviewBitmap() const;
    HBITMAP LoadPreviewBitmap(const std::filesystem::path& imagePath) const;
    void DrawPreviewBitmap(HDC hdc, RECT target, const std::filesystem::path& imagePath) const;
    void DrawDecodedVideoFrame(HDC hdc, RECT target, const VideoFrame& frame) const;
    void DrawVideoSurface(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot, const RECT& paintRect) const;
    void DrawTransport(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawVolumeSlider(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawSubtitleMenu(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawSectionHeader(HDC hdc, const std::wstring& text, RECT& cursor) const;
    void DrawField(HDC hdc, const std::wstring& label, const std::wstring& value, RECT& cursor) const;
    void DrawInspectorPanel(HDC hdc,
                            const anvil::playback::PlaybackSessionSnapshot& snapshot,
                            const anvil::playback::PlayerSettings& settings,
                            const anvil::playback::CapabilityReport& capabilities) const;
    void DrawListMessage(HDC hdc, const std::wstring& message, RECT cursor) const;
    void DrawPathList(HDC hdc,
                      const std::vector<std::filesystem::path>& paths,
                      const std::optional<anvil::playback::MediaDescriptor>& media,
                      RECT& cursor) const;
    void DrawRecentContent(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot, RECT cursor) const;
    void DrawFolderContent(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot, RECT cursor) const;
    void DrawMediaInfoContent(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot, RECT cursor) const;
    void DrawSystemContent(HDC hdc, const anvil::playback::CapabilityReport& capabilities, RECT cursor) const;
    void DrawLogContent(HDC hdc, RECT cursor) const;
    void DrawSettingsContent(HDC hdc, const anvil::playback::PlayerSettings& settings, RECT cursor) const;
    void DrawSettingsScrollbar(HDC hdc) const;
    void DrawHdrToneCurveEditor(HDC hdc, const anvil::playback::PlayerSettings& settings, RECT& cursor) const;
    void DrawHdrToneCurveExpandedEditor(HDC hdc, const anvil::playback::PlayerSettings& settings) const;

    struct InspectorPathItem {
        RECT bounds{};
        std::filesystem::path path;
    };

    static constexpr std::size_t kCommandAnimationSlotCount = 32;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = 96;
    Palette palette_;
    anvil::playback::PlayerController controller_;
    anvil::playback::CapabilityReport cachedCapabilities_;
    bool capabilitiesCached_ = false;
    EmbeddedFfplayPlayer playbackPlayer_;
    ExternalVideoDecoder videoDecoder_;
    WasapiAudioPlayer audioPlayer_;
    std::optional<FfmpegVideoDecoder> nativeVideoDecoder_;
    std::optional<D3D11VideoRenderer> d3dRenderer_;
    IconPainter iconPainter_;
    std::unique_ptr<WebUiHost> webUiHost_;
    HWND videoHost_ = nullptr;
    HWND fullscreenOverlay_ = nullptr;
    HWND transportOverlay_ = nullptr;
    HWND bufferingOverlay_ = nullptr;
    HWND bufferingHudOverlay_ = nullptr;
    HWND subtitleMenuOverlay_ = nullptr;
    HWND hdrToneCurveWindow_ = nullptr;
    bool videoHostReady_ = false;
    bool webUiRequested_ = true;
    bool webUiActive_ = false;
    bool webUiPlayerRouteActive_ = false;
    mutable std::chrono::steady_clock::time_point lastWebUiStatePostedAt_{};
    bool layoutDirty_ = true;
    bool nativeFrameHoldVisible_ = false;
    bool pendingPausedFrameRefresh_ = false;
    bool heldNativeFrameNeedsPresent_ = false;
    bool nativeSeekPrerollHoldingAudio_ = false;
    bool bufferingOverlayVisible_ = false;
    bool bufferingOverlayCreateFailedLogged_ = false;
    bool bufferingOverlaySuppressedLogged_ = false;
    NativeVideoQueueStats bufferingOverlayStats_{};
    std::thread runtimeStopThread_;
    std::atomic_bool runtimeStopAsyncInProgress_{false};
    std::atomic_bool runtimeStopAsyncClearFrame_{true};
    bool deferredRuntimeStart_ = false;
    bool deferredRuntimeRestart_ = false;
    bool deferredRuntimeWaitForPreroll_ = false;
    bool deferredPausedFrameRefresh_ = false;
    bool deferredPausedFrameRefreshForceRestart_ = false;
    UINT_PTR nativeColorSettingsRefreshSerial_ = 0;
    bool nativeColorSettingsRefreshRequiresDecoderRefresh_ = false;
    std::optional<NativeVideoFrame> heldNativeFrame_;
    RECT lastVideoHostBounds_{};
    RECT lastFullscreenOverlayBounds_{};
    RECT lastTransportOverlayBounds_{};
    RECT lastBufferingOverlayBounds_{};
    RECT lastSubtitleMenuOverlayBounds_{};
    bool trackingMouseLeave_ = false;
    bool hdrToneCurveWindowTrackingMouseLeave_ = false;
    int hoveredButton_ = -1;
    int hoveredInspectorPathItem_ = -1;
    InspectorTab inspectorTab_ = InspectorTab::Recent;
    PlaybackBackend backend_ = PlaybackBackend::NativeFfmpegD3D11;
    bool fullscreen_ = false;
    LONG previousStyle_ = 0;
    LONG previousExStyle_ = 0;
    WINDOWPLACEMENT previousPlacement_{sizeof(WINDOWPLACEMENT)};
    std::chrono::steady_clock::time_point lastNativeStatsLog_{};
    std::chrono::steady_clock::time_point nativeBufferingStartedAt_{};
    std::chrono::steady_clock::time_point nativeBufferingLastProgressAt_{};
    uint64_t nativeBufferingLastRendered_ = 0;
    std::size_t nativeBufferingLastQueueDepth_ = 0;
    std::size_t nativeBufferingLastPacketDepth_ = 0;
    std::size_t nativeBufferingLastPacketBytes_ = 0;
    std::chrono::milliseconds nativeBufferingLastReadAhead_{0};
    std::chrono::milliseconds nativeBufferingLastBufferedEnd_{0};
    std::chrono::milliseconds nativeBufferingLastClockPosition_{0};
    RECT topBar_{};
    RECT videoSurface_{};
    RECT playbackSurface_{};
    RECT transportBar_{};
    RECT inspector_{};
    RECT progress_{};
    RECT volumeSlider_{};
    RECT subtitleMenu_{};
    RECT webUiSubtitleAnchor_{};
    RECT webUiSubtitlePopover_{};
    RECT webUiTransportBounds_{};
    RECT hdrToneCurvePlot_{};
    RECT hdrToneCurveExpandedEditor_{};
    RECT hdrToneCurveFloatingReset_{};
    RECT settingsContentViewport_{};
    RECT settingsScrollTrack_{};
    RECT settingsScrollThumb_{};
    int topControlsLeft_ = 0;
    int transportControlsLeft_ = 0;
    int transportControlsRight_ = 0;
    int transportRightControlsLeft_ = 0;
    bool showTopBrand_ = true;
    bool showTopState_ = true;
    bool showInspectorTabs_ = false;
    bool draggingProgress_ = false;
    bool draggingVolume_ = false;
    bool draggingHdrToneCurve_ = false;
    bool draggingHdrToneCurveSelection_ = false;
    bool selectingHdrToneCurveRange_ = false;
    bool hdrToneCurveDragMoved_ = false;
    bool draggingSettingsScrollThumb_ = false;
    bool hdrToneCurveExpanded_ = false;
    bool inspectorCollapsed_ = false;
    bool fullscreenTransportVisible_ = false;
    bool subtitleMenuOpen_ = false;
    bool webUiSubtitleGeometryValid_ = false;
    bool webUiTransportGeometryValid_ = false;
    bool progressHovered_ = false;
    bool volumeSliderHovered_ = false;
    bool volumeDragChanged_ = false;
    bool videoPressActive_ = false;
    bool videoPressLongActive_ = false;
    bool videoPressMoved_ = false;
    bool temporaryRateActive_ = false;
    double inspectorCollapseAmount_ = 0.0;
    double inspectorCollapseStartAmount_ = 0.0;
    double inspectorCollapseTarget_ = 0.0;
    double progressHoverAmount_ = 0.0;
    double progressHoverStartAmount_ = 0.0;
    double progressHoverTarget_ = 0.0;
    double fullscreenTransportAmount_ = 0.0;
    double fullscreenTransportStartAmount_ = 0.0;
    double fullscreenTransportTarget_ = 0.0;
    double subtitleMenuAmount_ = 0.0;
    double subtitleMenuStartAmount_ = 0.0;
    double subtitleMenuTarget_ = 0.0;
    double volumeHoverAmount_ = 0.0;
    double volumeHoverStartAmount_ = 0.0;
    double volumeHoverTarget_ = 0.0;
    int settingsScrollOffset_ = 0;
    int settingsScrollMax_ = 0;
    int settingsContentHeight_ = 0;
    int settingsScrollDragStartY_ = 0;
    int settingsScrollDragStartOffset_ = 0;
    std::chrono::steady_clock::time_point inspectorAnimationStartedAt_{};
    std::chrono::steady_clock::time_point progressHoverAnimationStartedAt_{};
    std::chrono::steady_clock::time_point fullscreenTransportAnimationStartedAt_{};
    std::chrono::steady_clock::time_point subtitleMenuAnimationStartedAt_{};
    std::chrono::steady_clock::time_point volumeHoverAnimationStartedAt_{};
    std::chrono::steady_clock::time_point fullscreenTransportLastShownAt_{};
    std::chrono::steady_clock::time_point settingsScrollLastActiveAt_{};
    std::chrono::steady_clock::time_point subtitleMenuScrollLastActiveAt_{};
    std::chrono::steady_clock::time_point lastPlaybackUiRefreshAt_{};
    HANDLE playbackTimerQueueTimer_ = nullptr;
    POINT lastFullscreenCursorClient_{};
    bool hasLastFullscreenCursorClient_ = false;
    POINT videoPressStart_{};
    std::chrono::milliseconds dragSeekPosition_{0};
    int draggedHdrToneCurvePoint_ = -1;
    int hoveredSubtitleMenuItem_ = -1;
    int subtitleMenuScrollOffset_ = 0;
    int subtitleMenuVisibleItemCount_ = 0;
    int hoveredHdrToneCurvePoint_ = -1;
    int hdrToneCurveSelectionStartX_ = 0;
    int hdrToneCurveSelectionCurrentX_ = 0;
    POINT hdrToneCurveDragStart_{};
    double hdrToneCurveDragStartPointerNits_ = 0.0;
    std::array<double, anvil::playback::kHdrToneCurvePointCount> hdrToneCurveDragStartOutputs_{};
    std::array<bool, anvil::playback::kHdrToneCurvePointCount> selectedHdrToneCurvePoints_{};
    std::vector<int> subtitleMenuTracks_;
    std::vector<int> audioMenuTracks_;
    std::array<UiMotionValue, kCommandAnimationSlotCount> buttonHoverAnimations_{};
    std::array<UiMotionValue, kCommandAnimationSlotCount> buttonPressAnimations_{};
    enum class SubtitleMenuPage {
        Audio,
        Subtitles,
        Danmaku,
    };
    SubtitleMenuPage subtitleMenuPage_ = SubtitleMenuPage::Subtitles;
    std::vector<std::filesystem::path> recentMedia_;
    std::vector<std::filesystem::path> currentFolderEntries_;
    std::vector<InspectorPathItem> inspectorPathItems_;
    std::vector<UiButton> buttons_;
    mutable HBITMAP previewBitmap_ = nullptr;
    mutable std::filesystem::path previewBitmapPath_;
};

}  // namespace anvil::app
