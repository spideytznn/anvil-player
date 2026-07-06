#include "AnvilPlayer/App/main_window.h"

#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>

namespace anvil::app {

using anvil::playback::PlaybackState;

namespace {

constexpr UINT kSubtitleMenuCommandBase = 0x5000;
constexpr double kHdrToneCurveMaxNits = 10000.0;

double ToneCurveNitsToUnit(const double nits) {
    const double clamped = std::clamp(nits, 0.0, kHdrToneCurveMaxNits);
    return std::log10(clamped + 1.0) / std::log10(kHdrToneCurveMaxNits + 1.0);
}

double UnitToToneCurveNits(const double unit) {
    const double clamped = std::clamp(unit, 0.0, 1.0);
    return std::pow(10.0, clamped * std::log10(kHdrToneCurveMaxNits + 1.0)) - 1.0;
}

int ToneCurveX(const RECT& plot, const double nits) {
    return plot.left + static_cast<int>(std::round(ToneCurveNitsToUnit(nits) * RectWidth(plot)));
}

int ToneCurveY(const RECT& plot, const double nits) {
    return plot.bottom - static_cast<int>(std::round(ToneCurveNitsToUnit(nits) * RectHeight(plot)));
}

double RoundToneCurveNits(const double nits) {
    const double clamped = std::clamp(nits, 0.0, kHdrToneCurveMaxNits);
    const double step = clamped < 100.0 ? 5.0 : (clamped < 1000.0 ? 10.0 : (clamped < 4000.0 ? 50.0 : 100.0));
    return std::round(clamped / step) * step;
}

std::vector<int> SubtitleTrackCycle(const std::optional<anvil::playback::MediaDescriptor>& media) {
    std::vector<int> tracks;
    tracks.push_back(anvil::playback::kSubtitleTrackAuto);
    if (media.has_value()) {
        for (const auto& stream : media->streams) {
            if (stream.kind == L"Subtitle") {
                tracks.push_back(stream.index);
            }
        }
    }
    tracks.push_back(anvil::playback::kSubtitleTrackOff);
    return tracks;
}

std::wstring SubtitleSelectionLogLabel(const int selection) {
    if (selection == anvil::playback::kSubtitleTrackAuto) {
        return L"auto";
    }
    if (selection == anvil::playback::kSubtitleTrackOff) {
        return L"off";
    }
    return L"stream=" + std::to_wstring(selection);
}

std::wstring SubtitleMenuLabel(const int selection, const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (selection == anvil::playback::kSubtitleTrackAuto) {
        return L"Auto";
    }
    if (selection == anvil::playback::kSubtitleTrackOff) {
        return L"Off";
    }

    std::wstring label = L"Stream " + std::to_wstring(selection);
    if (!media.has_value()) {
        return label;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind != L"Subtitle" || stream.index != selection) {
            continue;
        }
        if (!stream.language.empty() && stream.language != L"-") {
            label += L" - " + stream.language;
        }
        if (!stream.codec.empty()) {
            label += L" - " + stream.codec;
        }
        break;
    }
    return label;
}

}  // namespace

void MainWindow::OnMouseMove(const int x, const int y) {
    const POINT point{x, y};
    if (fullscreen_) {
        lastFullscreenCursorClient_ = point;
        hasLastFullscreenCursorClient_ = true;
    }
    const bool fullscreenTransportActive = fullscreen_ &&
                                           ShouldShowFullscreenTransport(controller_.Snapshot());
    if (fullscreen_ &&
        (fullscreenTransportActive ||
         IsFullscreenTransportActivationPoint(point) ||
         ContainsPoint(transportBar_, point))) {
        ShowFullscreenTransport();
    }

    if (videoPressActive_) {
        const int dx = point.x - videoPressStart_.x;
        const int dy = point.y - videoPressStart_.y;
        if (dx * dx + dy * dy > Scale(8) * Scale(8)) {
            videoPressMoved_ = true;
        }
    }

    if (draggingProgress_) {
        dragSeekPosition_ = PositionFromProgressX(x);
        SetProgressHover(true);
        InvalidateTransportArea();
        return;
    }
    if (draggingHdrToneCurve_) {
        UpdateHdrToneCurveDrag(point);
        return;
    }

    if (!trackingMouseLeave_) {
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd_;
        TrackMouseEvent(&event);
        trackingMouseLeave_ = true;
    }

    SetProgressHover(ContainsPoint(ProgressHitRect(), point));

    const int hit = HitButton(point);
    if (hit != hoveredButton_) {
        hoveredButton_ = hit;
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnLeftButtonDown(const int x, const int y) {
    SetFocus(hwnd_);
    const POINT point{x, y};
    const bool fullscreenTransportWasHidden = fullscreen_ &&
                                              !ShouldShowFullscreenTransport(controller_.Snapshot());
    if (fullscreen_ &&
        (!fullscreenTransportWasHidden ||
         IsFullscreenTransportActivationPoint(point) ||
         ContainsPoint(transportBar_, point))) {
        ShowFullscreenTransport();
        if (fullscreenTransportWasHidden) {
            return;
        }
    }

    const int hit = HitButton(point);
    if (hit >= 0) {
        Execute(buttons_[static_cast<std::size_t>(hit)].command);
        return;
    }
    if (BeginHdrToneCurveDrag(point)) {
        return;
    }
    if (ContainsPoint(ProgressHitRect(), point)) {
        BeginProgressDrag(point.x);
        return;
    }

    const auto snapshot = controller_.Snapshot();
    if (snapshot.media.has_value() && ContainsPoint(PlaybackSurfaceBounds(), point)) {
        BeginVideoPress(point);
    }
}

void MainWindow::OnLeftButtonUp(const int x, const int y) {
    if (draggingHdrToneCurve_) {
        UpdateHdrToneCurveDrag(POINT{x, y});
        EndHdrToneCurveDrag();
        return;
    }
    if (draggingProgress_) {
        dragSeekPosition_ = PositionFromProgressX(x);
        CommitProgressDrag();
        if (fullscreen_) {
            ShowFullscreenTransport();
        }
        return;
    }
    if (videoPressActive_) {
        FinishVideoPress(POINT{x, y});
    }
}

RECT MainWindow::ProgressHitRect() const {
    RECT hit = progress_;
    const int padY = Scale(10);
    hit.top -= padY;
    hit.bottom += padY;
    return hit;
}

std::chrono::milliseconds MainWindow::PositionFromProgressX(const int x) const {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value() || RectWidth(progress_) <= 0 || snapshot.media->duration.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    const double ratio = std::clamp(
        static_cast<double>(x - progress_.left) / static_cast<double>(RectWidth(progress_)),
        0.0,
        1.0);
    return std::chrono::milliseconds(static_cast<long long>(snapshot.media->duration.count() * ratio));
}

void MainWindow::BeginProgressDrag(const int x) {
    const auto snapshot = controller_.Snapshot();
    if (!snapshot.media.has_value() || snapshot.media->duration.count() <= 0 || RectWidth(progress_) <= 0) {
        return;
    }
    draggingProgress_ = true;
    dragSeekPosition_ = PositionFromProgressX(x);
    SetCapture(hwnd_);
    InvalidateTransportArea();
}

void MainWindow::CancelProgressDrag() {
    if (!draggingProgress_) {
        return;
    }
    draggingProgress_ = false;
    SetProgressHover(false);
    InvalidateTransportArea();
}

void MainWindow::CommitProgressDrag() {
    if (!draggingProgress_) {
        return;
    }
    draggingProgress_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    SeekToPosition(dragSeekPosition_);
}

bool MainWindow::IsHdrToneCurveVisible() const {
    const auto settings = controller_.Settings();
    return inspectorTab_ == InspectorTab::Settings &&
           settings.video.dolbyVisionHdrOutput &&
           RectWidth(hdrToneCurvePlot_) > 0 &&
           RectHeight(hdrToneCurvePlot_) > 0;
}

bool MainWindow::BeginHdrToneCurveDrag(const POINT point) {
    if (!IsHdrToneCurveVisible()) {
        return false;
    }

    RECT hitRect = hdrToneCurvePlot_;
    InflateRect(&hitRect, Scale(18), Scale(18));
    if (!ContainsPoint(hitRect, point)) {
        return false;
    }

    const auto settings = controller_.Settings();
    int nearest = -1;
    int nearestDistanceSq = Scale(24) * Scale(24);
    for (std::size_t index = 1; index < settings.video.hdrToneCurve.size(); ++index) {
        const auto& curvePoint = settings.video.hdrToneCurve[index];
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : curvePoint.inputNits;
        const int dx = point.x - ToneCurveX(hdrToneCurvePlot_, inputNits);
        const int dy = point.y - ToneCurveY(hdrToneCurvePlot_, curvePoint.outputNits);
        const int distanceSq = dx * dx + dy * dy;
        if (distanceSq <= nearestDistanceSq) {
            nearest = static_cast<int>(index);
            nearestDistanceSq = distanceSq;
        }
    }

    if (nearest < 0) {
        return false;
    }

    draggingHdrToneCurve_ = true;
    draggedHdrToneCurvePoint_ = nearest;
    SetCapture(hwnd_);
    UpdateHdrToneCurveDrag(point);
    return true;
}

void MainWindow::UpdateHdrToneCurveDrag(const POINT point) {
    if (!draggingHdrToneCurve_ ||
        draggedHdrToneCurvePoint_ <= 0 ||
        !IsHdrToneCurveVisible() ||
        RectWidth(hdrToneCurvePlot_) <= 0 ||
        RectHeight(hdrToneCurvePlot_) <= 0) {
        return;
    }

    auto settings = controller_.Settings();
    auto& curve = settings.video.hdrToneCurve;
    const std::size_t index = static_cast<std::size_t>(draggedHdrToneCurvePoint_);
    if (index >= curve.size()) {
        return;
    }

    const double unitY = 1.0 - static_cast<double>(point.y - hdrToneCurvePlot_.top) / static_cast<double>(RectHeight(hdrToneCurvePlot_));
    double outputNits = RoundToneCurveNits(UnitToToneCurveNits(unitY));

    for (std::size_t i = 0; i < curve.size() && i < anvil::playback::kDefaultHdrToneCurve.size(); ++i) {
        curve[i].inputNits = anvil::playback::kDefaultHdrToneCurve[i].inputNits;
    }

    const double minOutput = curve[index - 1].outputNits;
    const double maxOutput = index + 1 < curve.size()
                                 ? curve[index + 1].outputNits
                                 : kHdrToneCurveMaxNits;

    outputNits = std::clamp(outputNits, minOutput, std::max(minOutput, maxOutput));
    curve[index].outputNits = outputNits;
    settings.video.peakBrightnessNits = static_cast<int>(std::clamp(curve.back().outputNits, 100.0, 10000.0));

    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
}

void MainWindow::EndHdrToneCurveDrag() {
    if (!draggingHdrToneCurve_) {
        return;
    }

    draggingHdrToneCurve_ = false;
    draggedHdrToneCurvePoint_ = -1;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }

    const auto settings = controller_.Settings();
    LogApp(anvil::playback::LogLevel::Info,
           L"hdr tone curve peak=" + std::to_wstring(settings.video.peakBrightnessNits) + L" nits");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ResetHdrToneCurve() {
    auto settings = controller_.Settings();
    settings.video.hdrToneCurve = anvil::playback::kDefaultHdrToneCurve;
    settings.video.peakBrightnessNits =
        static_cast<int>(std::clamp(settings.video.hdrToneCurve.back().outputNits, 100.0, 10000.0));
    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
    LogApp(anvil::playback::LogLevel::Info, L"hdr tone curve reset");
}

void MainWindow::ApplyLiveHdrToneCurveSettings() {
    const auto snapshot = controller_.Snapshot();
    const auto settings = controller_.Settings();
    if (d3dRenderer_ && snapshot.media.has_value()) {
        d3dRenderer_->ConfigureColorPipeline(settings.video, CachedCapabilities().display, snapshot.media->videoColor);
    }
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        snapshot.state == PlaybackState::Paused &&
        heldNativeFrame_.has_value()) {
        RenderHeldNativeFrame();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::BeginVideoPress(const POINT point) {
    videoPressActive_ = true;
    videoPressLongActive_ = false;
    videoPressMoved_ = false;
    videoPressStart_ = point;
    SetCapture(hwnd_);
    SetTimer(hwnd_, kVideoPressTimer, kVideoLongPressTimerMs, nullptr);
}

void MainWindow::CompleteVideoLongPress() {
    if (!videoPressActive_ || videoPressLongActive_) {
        return;
    }

    KillTimer(hwnd_, kVideoPressTimer);
    const auto snapshot = controller_.Snapshot();
    if (snapshot.state != PlaybackState::Playing || !snapshot.media.has_value()) {
        return;
    }

    videoPressLongActive_ = true;
    SetTemporaryPlaybackRate(2.0);
    if (fullscreen_ && ShouldShowFullscreenTransport(snapshot)) {
        ShowFullscreenTransport();
    }
}

void MainWindow::FinishVideoPress(const POINT point) {
    if (!videoPressActive_) {
        return;
    }

    KillTimer(hwnd_, kVideoPressTimer);
    const bool longPress = videoPressLongActive_;
    const bool moved = videoPressMoved_;
    videoPressActive_ = false;
    videoPressLongActive_ = false;
    videoPressMoved_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }

    if (longPress) {
        SetTemporaryPlaybackRate(1.0);
        return;
    }

    if (!moved && ContainsPoint(PlaybackSurfaceBounds(), point)) {
        TogglePlayback();
    }
}

void MainWindow::CancelVideoPress() {
    if (!videoPressActive_ && !videoPressLongActive_) {
        return;
    }

    KillTimer(hwnd_, kVideoPressTimer);
    const bool longPress = videoPressLongActive_;
    videoPressActive_ = false;
    videoPressLongActive_ = false;
    videoPressMoved_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    if (longPress || temporaryRateActive_) {
        SetTemporaryPlaybackRate(1.0);
    }
}

void MainWindow::OnKeyDown(const WPARAM key) {
    const bool refreshFullscreenTransport = fullscreen_ &&
                                            ShouldShowFullscreenTransport(controller_.Snapshot());
    switch (key) {
    case VK_SPACE:
        TogglePlayback();
        break;
    case VK_LEFT:
        SeekRelative(-std::chrono::seconds{10});
        break;
    case VK_RIGHT:
        SeekRelative(std::chrono::seconds{10});
        break;
    case 'O':
        OpenFileDialog();
        break;
    case 'F':
        ToggleFullscreen();
        break;
    case 'H':
        ToggleDolbyVisionHdrOutput();
        break;
    case 'S':
        CycleSubtitleTrack();
        break;
    case VK_ESCAPE:
        if (fullscreen_) {
            ToggleFullscreen();
        }
        break;
    default:
        break;
    }
    if (refreshFullscreenTransport && fullscreen_) {
        ShowFullscreenTransport();
    }
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplySubtitleSelection(const int selectedTrackIndex) {
    auto settings = controller_.Settings();
    if (settings.subtitles.selectedTrackIndex == selectedTrackIndex) {
        return;
    }

    settings.subtitles.selectedTrackIndex = selectedTrackIndex;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"subtitle track=" + SubtitleSelectionLogLabel(settings.subtitles.selectedTrackIndex));

    const auto updated = controller_.Snapshot();
    if (updated.state == PlaybackState::Paused &&
        updated.media.has_value() &&
        updated.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(updated);
    } else {
        RestartPlaybackIfPlaying();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CycleSubtitleTrack() {
    const auto snapshot = controller_.Snapshot();
    const auto settings = controller_.Settings();
    const auto tracks = SubtitleTrackCycle(snapshot.media);
    auto current = std::find(tracks.begin(), tracks.end(), settings.subtitles.selectedTrackIndex);
    if (current == tracks.end() || ++current == tracks.end()) {
        current = tracks.begin();
    }
    ApplySubtitleSelection(*current);
}

void MainWindow::ToggleDolbyVisionHdrOutput() {
    auto settings = controller_.Settings();
    settings.video.dolbyVisionHdrOutput = !settings.video.dolbyVisionHdrOutput;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"dolby vision hdr output=" +
               std::wstring(settings.video.dolbyVisionHdrOutput ? L"on" : L"off"));

    const auto snapshot = controller_.Snapshot();
    if (snapshot.state == PlaybackState::Paused &&
        snapshot.media.has_value() &&
        snapshot.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(snapshot);
    } else {
        RestartPlaybackIfPlaying();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowSubtitleMenu() {
    EnsureLayout();

    const auto snapshot = controller_.Snapshot();
    const auto settings = controller_.Settings();
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    std::vector<int> tracks;
    if (snapshot.media.has_value()) {
        tracks = SubtitleTrackCycle(snapshot.media);
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            const UINT id = kSubtitleMenuCommandBase + static_cast<UINT>(index);
            UINT flags = MF_STRING;
            if (tracks[index] == settings.subtitles.selectedTrackIndex) {
                flags |= MF_CHECKED;
            }
            const auto label = SubtitleMenuLabel(tracks[index], snapshot.media);
            AppendMenuW(menu, flags, id, label.c_str());
        }
    } else {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No media loaded");
    }

    RECT anchor = transportBar_;
    for (const auto& button : buttons_) {
        if (button.command == Command::SubtitleMenu) {
            anchor = button.bounds;
            break;
        }
    }

    POINT popupPoint{anchor.right, fullscreen_ ? anchor.top : anchor.bottom};
    ClientToScreen(hwnd_, &popupPoint);
    SetForegroundWindow(hwnd_);
    const UINT align = fullscreen_ ? TPM_BOTTOMALIGN : TPM_TOPALIGN;
    const UINT selected = TrackPopupMenuEx(menu,
                                           TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | align,
                                           popupPoint.x,
                                           popupPoint.y,
                                           hwnd_,
                                           nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (selected >= kSubtitleMenuCommandBase) {
        const std::size_t index = static_cast<std::size_t>(selected - kSubtitleMenuCommandBase);
        if (index < tracks.size()) {
            ApplySubtitleSelection(tracks[index]);
        }
    }
}

void MainWindow::OnDropFiles(HDROP drop) {
    const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
    if (length > 0) {
        std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1);
        if (DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size())) > 0) {
            OpenPath(path.data());
        }
    }
    DragFinish(drop);
}

void MainWindow::Execute(const Command command) {
    switch (command) {
    case Command::Open:
        OpenFileDialog();
        break;
    case Command::Back:
        SeekRelative(-std::chrono::seconds{10});
        break;
    case Command::PlayPause:
        TogglePlayback();
        break;
    case Command::Stop:
        StopPlayback();
        break;
    case Command::Forward:
        SeekRelative(std::chrono::seconds{10});
        break;
    case Command::VolumeDown: {
        const auto snapshot = controller_.Snapshot();
        controller_.SetVolume(snapshot.volume - 0.05);
        RestartPlaybackIfPlaying();
        break;
    }
    case Command::VolumeUp: {
        const auto snapshot = controller_.Snapshot();
        controller_.SetVolume(snapshot.volume + 0.05);
        RestartPlaybackIfPlaying();
        break;
    }
    case Command::SubtitleMenu:
        ShowSubtitleMenu();
        break;
    case Command::Fullscreen:
        ToggleFullscreen();
        break;
    case Command::Settings:
        if (inspectorCollapsed_) {
            inspectorTab_ = InspectorTab::Settings;
            ToggleSidebar();
            break;
        }
        inspectorTab_ = inspectorTab_ == InspectorTab::Settings ? InspectorTab::Media : InspectorTab::Settings;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::ToggleSidebar:
        ToggleSidebar();
        break;
    case Command::InspectorMedia:
        inspectorTab_ = InspectorTab::Media;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::InspectorDevice:
        inspectorTab_ = InspectorTab::Device;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::InspectorLog:
        inspectorTab_ = InspectorTab::Log;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::ToggleDolbyVisionHdr:
        ToggleDolbyVisionHdrOutput();
        break;
    case Command::ResetHdrToneCurve:
        ResetHdrToneCurve();
        break;
    }
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace anvil::app
