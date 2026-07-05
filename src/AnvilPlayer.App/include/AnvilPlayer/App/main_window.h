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
#include "AnvilPlayer/Playback/PlayerController.h"
#include "resource.h"

#include <shellapi.h>
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace anvil::app {

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

    bool Create(HINSTANCE instance);
    void Show(int commandShow) const;
    void OpenInitialPath(const std::filesystem::path& path, bool autoplay);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK VideoHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK FullscreenOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TransportOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void CALLBACK PlaybackTimerQueueProc(PVOID context, BOOLEAN timerOrWaitFired);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    static std::filesystem::path DefaultLogPath();
    void LogApp(anvil::playback::LogLevel level, const std::wstring& message) const;
    void LogRuntime(anvil::playback::LogLevel level, const std::wstring& category, const std::wstring& message) const;
    void MaybeLogNativeSchedulerStats(const NativeVideoQueueStats& stats);
    void StartUiAnimationTimer() const;
    void UpdateUiAnimations();
    bool IsPointInteractive(POINT point) const;
    void SetProgressHover(bool hovered);
    void ToggleSidebar();
    bool ShouldShowFullscreenTransport(const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    bool IsFullscreenTransportActivationPoint(POINT point) const;
    void UpdateFullscreenTransportCursorPolling();
    void ShowFullscreenTransport();
    void HideFullscreenTransportIfIdle();
    void SetFullscreenTransportTarget(bool visible);
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

    const anvil::playback::CapabilityReport& CachedCapabilities();
    void RefreshCapabilityCache();
    void RenderPlaybackTick(const anvil::playback::PlaybackSessionSnapshot& snapshot, bool forceRefresh = true);

    int Scale(int value) const;
    RECT PlaybackSurfaceBounds() const;
    void ApplyWindowChrome() const;

    // main_window_layout.cpp
    void MarkLayoutDirty();
    void EnsureLayout();
    void UpdateLayout();
    void UpdateVideoHost();
    void EnsureFullscreenOverlay();
    void UpdateFullscreenOverlay();
    void EnsureTransportOverlay();
    void UpdateTransportOverlay();
    int HitButton(POINT point) const;

    // main_window_input.cpp
    void OnMouseMove(int x, int y);
    void OnLeftButtonDown(int x, int y);
    void OnLeftButtonUp(int x, int /*y*/);
    RECT ProgressHitRect() const;
    std::chrono::milliseconds PositionFromProgressX(int x) const;
    void BeginProgressDrag(int x);
    void CancelProgressDrag();
    void CommitProgressDrag();
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP drop);
    void Execute(Command command);
    void OpenFileDialog();

    // main_window.cpp runtime + transport
    void StopRuntime(bool clearVideoFrame = true);
    void StartRuntime(const anvil::playback::PlaybackSessionSnapshot& snapshot, bool restart);
    void EnsureVideoHost();
    void StartNativeRuntime(const anvil::playback::PlaybackSessionSnapshot& snapshot, bool restart);
    bool CaptureLatestNativeFrame();
    void RenderHeldNativeFrame();
    void RefreshPausedNativeFrame(const anvil::playback::PlaybackSessionSnapshot& snapshot);
    void OpenPath(const std::filesystem::path& path, bool autoplay = true);
    void StartPlayback();
    void PausePlayback();
    void TogglePlayback();
    void StopPlayback();
    void RestartPlaybackIfPlaying();
    void SeekRelative(std::chrono::milliseconds delta);
    void SeekToPosition(std::chrono::milliseconds position);
    void SeekFromProgress(int x);
    void ApplySubtitleSelection(int selectedTrackIndex);
    void CycleSubtitleTrack();
    void ShowSubtitleMenu();
    void ToggleFullscreen();

    // main_window_paint.cpp
    void Paint();
    void PaintFullscreenOverlay(HWND overlay);
    void PaintTransportOverlay(HWND overlay);
    void DrawTopBar(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawButtons(HDC hdc) const;
    void DrawTooltip(HDC hdc) const;
    void ClearPreviewBitmap() const;
    HBITMAP LoadPreviewBitmap(const std::filesystem::path& imagePath) const;
    void DrawPreviewBitmap(HDC hdc, RECT target, const std::filesystem::path& imagePath) const;
    void DrawDecodedVideoFrame(HDC hdc, RECT target, const VideoFrame& frame) const;
    void DrawVideoSurface(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawTransport(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot) const;
    void DrawSectionHeader(HDC hdc, const std::wstring& text, RECT& cursor) const;
    void DrawField(HDC hdc, const std::wstring& label, const std::wstring& value, RECT& cursor) const;
    void DrawInspectorPanel(HDC hdc,
                            const anvil::playback::PlaybackSessionSnapshot& snapshot,
                            const anvil::playback::PlayerSettings& settings,
                            const anvil::playback::CapabilityReport& capabilities) const;
    void DrawMediaContent(HDC hdc, const anvil::playback::PlaybackSessionSnapshot& snapshot, RECT cursor) const;
    void DrawDeviceContent(HDC hdc, const anvil::playback::CapabilityReport& capabilities, RECT cursor) const;
    void DrawLogContent(HDC hdc, RECT cursor) const;
    void DrawSettingsContent(HDC hdc, const anvil::playback::PlayerSettings& settings, RECT cursor) const;

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
    HWND videoHost_ = nullptr;
    HWND fullscreenOverlay_ = nullptr;
    HWND transportOverlay_ = nullptr;
    bool videoHostReady_ = false;
    bool layoutDirty_ = true;
    bool nativeFrameHoldVisible_ = false;
    bool pendingPausedFrameRefresh_ = false;
    bool heldNativeFrameNeedsPresent_ = false;
    std::optional<NativeVideoFrame> heldNativeFrame_;
    RECT lastVideoHostBounds_{};
    RECT lastFullscreenOverlayBounds_{};
    RECT lastTransportOverlayBounds_{};
    bool trackingMouseLeave_ = false;
    int hoveredButton_ = -1;
    InspectorTab inspectorTab_ = InspectorTab::Media;
    PlaybackBackend backend_ = PlaybackBackend::NativeFfmpegD3D11;
    bool fullscreen_ = false;
    LONG previousStyle_ = 0;
    LONG previousExStyle_ = 0;
    WINDOWPLACEMENT previousPlacement_{sizeof(WINDOWPLACEMENT)};
    std::chrono::steady_clock::time_point lastNativeStatsLog_{};
    RECT topBar_{};
    RECT videoSurface_{};
    RECT transportBar_{};
    RECT inspector_{};
    RECT progress_{};
    int topControlsLeft_ = 0;
    int transportControlsLeft_ = 0;
    int transportControlsRight_ = 0;
    int transportRightControlsLeft_ = 0;
    bool showTopBrand_ = true;
    bool showTopState_ = true;
    bool showInspectorTabs_ = false;
    bool draggingProgress_ = false;
    bool inspectorCollapsed_ = false;
    bool fullscreenTransportVisible_ = false;
    bool progressHovered_ = false;
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
    std::chrono::steady_clock::time_point inspectorAnimationStartedAt_{};
    std::chrono::steady_clock::time_point progressHoverAnimationStartedAt_{};
    std::chrono::steady_clock::time_point fullscreenTransportAnimationStartedAt_{};
    std::chrono::steady_clock::time_point fullscreenTransportLastShownAt_{};
    std::chrono::steady_clock::time_point lastPlaybackUiRefreshAt_{};
    HANDLE playbackTimerQueueTimer_ = nullptr;
    POINT lastFullscreenCursorClient_{};
    bool hasLastFullscreenCursorClient_ = false;
    POINT videoPressStart_{};
    std::chrono::milliseconds dragSeekPosition_{0};
    std::vector<UiButton> buttons_;
    mutable HBITMAP previewBitmap_ = nullptr;
    mutable std::filesystem::path previewBitmapPath_;
};

}  // namespace anvil::app
