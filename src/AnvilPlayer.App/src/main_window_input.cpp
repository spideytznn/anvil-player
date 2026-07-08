#include "AnvilPlayer/App/main_window.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace anvil::app {

using anvil::playback::PlaybackState;

namespace {

constexpr double kHdrToneCurveMaxNits = 4000.0;
constexpr double kHdrToneCurveFocusNits = 1000.0;
constexpr double kHdrToneCurveFocusUnit = 0.72;

double ToneCurveNitsToUnit(const double nits) {
    const double clamped = std::clamp(nits, 0.0, kHdrToneCurveMaxNits);
    if (clamped <= kHdrToneCurveFocusNits) {
        return (clamped / kHdrToneCurveFocusNits) * kHdrToneCurveFocusUnit;
    }
    return kHdrToneCurveFocusUnit +
           ((clamped - kHdrToneCurveFocusNits) / (kHdrToneCurveMaxNits - kHdrToneCurveFocusNits)) *
               (1.0 - kHdrToneCurveFocusUnit);
}

double UnitToToneCurveNits(const double unit) {
    const double clamped = std::clamp(unit, 0.0, 1.0);
    if (clamped <= kHdrToneCurveFocusUnit) {
        return (clamped / kHdrToneCurveFocusUnit) * kHdrToneCurveFocusNits;
    }
    return kHdrToneCurveFocusNits +
           ((clamped - kHdrToneCurveFocusUnit) / (1.0 - kHdrToneCurveFocusUnit)) *
               (kHdrToneCurveMaxNits - kHdrToneCurveFocusNits);
}

int ToneCurveX(const RECT& plot, const double nits) {
    return plot.left + static_cast<int>(std::round(ToneCurveNitsToUnit(nits) * RectWidth(plot)));
}

int ToneCurveY(const RECT& plot, const double nits) {
    return plot.bottom - static_cast<int>(std::round(ToneCurveNitsToUnit(nits) * RectHeight(plot)));
}

double RoundToneCurveNits(const double nits) {
    const double clamped = std::clamp(nits, 0.0, kHdrToneCurveMaxNits);
    return std::round(clamped);
}

double ToneCurveYToNits(const RECT& plot, const int y) {
    if (RectHeight(plot) <= 0) {
        return 0.0;
    }
    const double unitY = 1.0 - static_cast<double>(y - plot.top) / static_cast<double>(RectHeight(plot));
    return UnitToToneCurveNits(unitY);
}

void NormalizeHdrToneCurveForEditing(std::array<anvil::playback::HdrToneCurvePoint,
                                                anvil::playback::kHdrToneCurvePointCount>& curve) {
    for (std::size_t i = 0; i < curve.size() && i < anvil::playback::kDefaultHdrToneCurve.size(); ++i) {
        curve[i].inputNits = anvil::playback::kDefaultHdrToneCurve[i].inputNits;
    }

    curve[0].outputNits = 0.0;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        curve[i].outputNits = std::clamp(RoundToneCurveNits(curve[i].outputNits),
                                         curve[i - 1].outputNits,
                                         kHdrToneCurveMaxNits);
    }
}

double ClampHdrToneCurveSelectionDelta(
    const std::array<anvil::playback::HdrToneCurvePoint, anvil::playback::kHdrToneCurvePointCount>& curve,
    const std::array<bool, anvil::playback::kHdrToneCurvePointCount>& selected,
    const std::array<double, anvil::playback::kHdrToneCurvePointCount>& startOutputs,
    const double desiredDelta) {
    double minDelta = -kHdrToneCurveMaxNits;
    double maxDelta = kHdrToneCurveMaxNits;

    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (!selected[i]) {
            continue;
        }

        const double lower = (i > 0 && !selected[i - 1]) ? curve[i - 1].outputNits : 0.0;
        const double upper = (i + 1 < curve.size() && !selected[i + 1]) ? curve[i + 1].outputNits : kHdrToneCurveMaxNits;
        minDelta = std::max(minDelta, lower - startOutputs[i]);
        maxDelta = std::min(maxDelta, upper - startOutputs[i]);
    }

    return std::clamp(desiredDelta, minDelta, std::max(minDelta, maxDelta));
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

std::vector<int> SubtitleTrackMenuItems(const std::optional<anvil::playback::MediaDescriptor>& media) {
    std::vector<int> tracks;
    tracks.push_back(anvil::playback::kSubtitleTrackOff);
    tracks.push_back(anvil::playback::kSubtitleTrackAuto);
    if (media.has_value()) {
        for (const auto& stream : media->streams) {
            if (stream.kind == L"Subtitle") {
                tracks.push_back(stream.index);
            }
        }
    }
    return tracks;
}

std::vector<int> AudioTrackMenuItems(const std::optional<anvil::playback::MediaDescriptor>& media) {
    std::vector<int> tracks;
    tracks.push_back(anvil::playback::kAudioTrackOff);
    tracks.push_back(anvil::playback::kAudioTrackAuto);
    if (media.has_value()) {
        for (const auto& stream : media->streams) {
            if (stream.kind == L"Audio") {
                tracks.push_back(stream.index);
            }
        }
    }
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

std::wstring AudioSelectionLogLabel(const int selection) {
    if (selection == anvil::playback::kAudioTrackAuto) {
        return L"auto";
    }
    if (selection == anvil::playback::kAudioTrackOff) {
        return L"off";
    }
    return L"stream=" + std::to_wstring(selection);
}

}  // namespace

void MainWindow::OnMouseMove(const int x, const int y) {
    const POINT point{x, y};
    if (fullscreen_) {
        lastFullscreenCursorClient_ = point;
        hasLastFullscreenCursorClient_ = true;
    }
    const bool fullscreenActivationPoint = fullscreen_ && IsFullscreenTransportActivationPoint(point);
    const bool fullscreenTransportHit = fullscreen_ &&
                                        (ContainsPoint(transportBar_, point) ||
                                         IsPointInSubtitleMenu(point));
    if (fullscreen_ && (fullscreenActivationPoint || fullscreenTransportHit)) {
        ShowFullscreenTransport(fullscreenActivationPoint ? L"mouse_move_activation" : L"mouse_move_transport");
    }

    if (draggingSettingsScrollThumb_) {
        UpdateSettingsScrollDrag(point);
        return;
    }
    if (draggingVolume_) {
        UpdateVolumeDrag(point);
        if (fullscreen_) {
            ShowFullscreenTransport(L"volume_drag");
        }
        return;
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
    if (selectingHdrToneCurveRange_) {
        UpdateHdrToneCurveRangeSelection(point);
        return;
    }
    if (draggingHdrToneCurve_) {
        hdrToneCurveDragMoved_ = true;
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
    const bool volumeHovered = ContainsPoint(VolumeSliderHitRect(), point);
    SetVolumeSliderHover(volumeHovered);
    const int subtitleHit = HitSubtitleMenuItem(point);
    if (subtitleHit != hoveredSubtitleMenuItem_) {
        hoveredSubtitleMenuItem_ = subtitleHit;
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!hdrToneCurveExpanded_) {
        UpdateHdrToneCurveHover(point);
    }

    int hit = HitButton(point);
    if (hit != hoveredButton_) {
        if (hoveredButton_ >= 0) {
            SetButtonHoverTarget(hoveredButton_, false);
        }
        if (hit >= 0) {
            SetButtonHoverTarget(hit, true);
        }
        hoveredButton_ = hit;
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    const int pathHit = HitInspectorPathItem(point);
    if (pathHit != hoveredInspectorPathItem_) {
        hoveredInspectorPathItem_ = pathHit;
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
        ShowFullscreenTransport(fullscreenTransportWasHidden ? L"left_down_reveal" : L"left_down_transport");
        if (fullscreenTransportWasHidden) {
            return;
        }
    }

    if (BeginSettingsScrollDrag(point)) {
        return;
    }
    if (BeginVolumeDrag(point)) {
        return;
    }

    if (subtitleMenuAmount_ > 0.01 || subtitleMenuTarget_ > 0.0) {
        const int item = HitSubtitleMenuItem(point);
        if (item >= 0) {
            if (subtitleMenuPage_ == SubtitleMenuPage::Audio) {
                if (item < static_cast<int>(audioMenuTracks_.size())) {
                    const int selectedTrack = audioMenuTracks_[static_cast<std::size_t>(item)];
                    HideSubtitleMenu();
                    ApplyAudioSelection(selectedTrack);
                }
            } else if (subtitleMenuPage_ == SubtitleMenuPage::Subtitles) {
                if (item < static_cast<int>(subtitleMenuTracks_.size())) {
                    const int selectedTrack = subtitleMenuTracks_[static_cast<std::size_t>(item)];
                    HideSubtitleMenu();
                    ApplySubtitleSelection(selectedTrack);
                }
            }
            return;
        }
        switch (HitSubtitleMenuAction(point)) {
        case SubtitleMenuAction::TabAudio:
            subtitleMenuPage_ = SubtitleMenuPage::Audio;
            hoveredSubtitleMenuItem_ = -1;
            subtitleMenuScrollOffset_ = 0;
            MarkLayoutDirty();
            EnsureLayout();
            InvalidateTransportArea();
            return;
        case SubtitleMenuAction::TabSubtitles:
            subtitleMenuPage_ = SubtitleMenuPage::Subtitles;
            hoveredSubtitleMenuItem_ = -1;
            subtitleMenuScrollOffset_ = 0;
            MarkLayoutDirty();
            EnsureLayout();
            InvalidateTransportArea();
            return;
        case SubtitleMenuAction::TabDanmaku:
            subtitleMenuPage_ = SubtitleMenuPage::Danmaku;
            hoveredSubtitleMenuItem_ = -1;
            subtitleMenuScrollOffset_ = 0;
            MarkLayoutDirty();
            EnsureLayout();
            InvalidateTransportArea();
            return;
        case SubtitleMenuAction::DelayDown:
            ApplySubtitleDelayDelta(-100);
            return;
        case SubtitleMenuAction::DelayUp:
            ApplySubtitleDelayDelta(100);
            return;
        case SubtitleMenuAction::SubtitleSizeDown:
            ApplySubtitleFontScaleDelta(-0.05);
            return;
        case SubtitleMenuAction::SubtitleSizeUp:
            ApplySubtitleFontScaleDelta(0.05);
            return;
        case SubtitleMenuAction::SubtitleOffsetXDown:
            ApplySubtitleOffsetDelta(-10, 0);
            return;
        case SubtitleMenuAction::SubtitleOffsetXUp:
            ApplySubtitleOffsetDelta(10, 0);
            return;
        case SubtitleMenuAction::SubtitleOffsetYDown:
            ApplySubtitleOffsetDelta(0, -10);
            return;
        case SubtitleMenuAction::SubtitleOffsetYUp:
            ApplySubtitleOffsetDelta(0, 10);
            return;
        case SubtitleMenuAction::AddFile:
            HideSubtitleMenu();
            if (transportOverlay_) {
                UpdateWindow(transportOverlay_);
            }
            if (fullscreenOverlay_) {
                UpdateWindow(fullscreenOverlay_);
            }
            if (subtitleMenuOverlay_) {
                UpdateWindow(subtitleMenuOverlay_);
            }
            UpdateWindow(hwnd_);
            OpenSubtitleFileDialog();
            return;
        case SubtitleMenuAction::DanmakuToggle:
            ToggleDanmakuEnabled();
            return;
        case SubtitleMenuAction::DanmakuMode:
            CycleDanmakuMode();
            return;
        case SubtitleMenuAction::DanmakuOpacityDown:
            ApplyDanmakuOpacityDelta(-5);
            return;
        case SubtitleMenuAction::DanmakuOpacityUp:
            ApplyDanmakuOpacityDelta(5);
            return;
        case SubtitleMenuAction::DanmakuSpeedDown:
            ApplyDanmakuSpeedDelta(-10);
            return;
        case SubtitleMenuAction::DanmakuSpeedUp:
            ApplyDanmakuSpeedDelta(10);
            return;
        case SubtitleMenuAction::AddDanmakuFile:
            HideSubtitleMenu();
            if (transportOverlay_) {
                UpdateWindow(transportOverlay_);
            }
            if (fullscreenOverlay_) {
                UpdateWindow(fullscreenOverlay_);
            }
            if (subtitleMenuOverlay_) {
                UpdateWindow(subtitleMenuOverlay_);
            }
            UpdateWindow(hwnd_);
            OpenDanmakuFileDialog();
            return;
        case SubtitleMenuAction::None:
            break;
        }
        if (IsPointInSubtitleMenu(point)) {
            return;
        }

        const int buttonHit = HitButton(point);
        const bool subtitleButtonHit = buttonHit >= 0 &&
                                       buttons_[static_cast<std::size_t>(buttonHit)].command == Command::SubtitleMenu;
        if (!subtitleButtonHit) {
            HideSubtitleMenu();
            return;
        }
    }

    const bool hdrToneCurveVisible = !hdrToneCurveExpanded_ && IsHdrToneCurveVisible();
    if (hdrToneCurveVisible && !ContainsPoint(hdrToneCurvePlot_, point) && HasHdrToneCurveSelection()) {
        ClearHdrToneCurveSelection();
    }

    const int hit = HitButton(point);
    if (hit >= 0) {
        TriggerButtonPress(hit);
        Execute(buttons_[static_cast<std::size_t>(hit)].command);
        return;
    }
    const int pathHit = HitInspectorPathItem(point);
    if (pathHit >= 0) {
        OpenInspectorPathItem(pathHit);
        return;
    }
    if (hdrToneCurveVisible) {
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 && BeginHdrToneCurveRangeSelection(point)) {
            return;
        }
        if (BeginHdrToneCurveDrag(point)) {
            return;
        }
        if (HasHdrToneCurveSelection()) {
            ClearHdrToneCurveSelection();
        }
        if (ContainsPoint(hdrToneCurvePlot_, point)) {
            return;
        }
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
    if (draggingSettingsScrollThumb_) {
        EndSettingsScrollDrag();
        return;
    }
    if (draggingVolume_) {
        EndVolumeDrag(POINT{x, y});
        if (fullscreen_) {
            ShowFullscreenTransport(L"volume_drag_end");
        }
        return;
    }
    if (selectingHdrToneCurveRange_) {
        UpdateHdrToneCurveRangeSelection(POINT{x, y});
        EndHdrToneCurveRangeSelection();
        return;
    }
    if (draggingHdrToneCurve_) {
        if (hdrToneCurveDragMoved_) {
            UpdateHdrToneCurveDrag(POINT{x, y});
        }
        EndHdrToneCurveDrag();
        return;
    }
    if (draggingProgress_) {
        dragSeekPosition_ = PositionFromProgressX(x);
        CommitProgressDrag();
        if (fullscreen_) {
            ShowFullscreenTransport(L"progress_drag_end");
        }
        return;
    }
    if (videoPressActive_) {
        FinishVideoPress(POINT{x, y});
    }
}

void MainWindow::OnMouseWheel(const int delta, POINT screenPoint) {
    ScreenToClient(hwnd_, &screenPoint);
    EnsureLayout();
    if ((subtitleMenuAmount_ > 0.01 || subtitleMenuTarget_ > 0.0) &&
        IsPointInSubtitleMenu(screenPoint) &&
        subtitleMenuVisibleItemCount_ > 0) {
        const int trackCount = subtitleMenuPage_ == SubtitleMenuPage::Audio
                                   ? static_cast<int>(audioMenuTracks_.size())
                                   : static_cast<int>(subtitleMenuTracks_.size());
        if (trackCount <= subtitleMenuVisibleItemCount_) {
            return;
        }
        MarkSubtitleMenuScrollbarActive();
        const int direction = delta > 0 ? -1 : 1;
        const int maxOffset = std::max(0, trackCount - subtitleMenuVisibleItemCount_);
        const int nextOffset = std::clamp(subtitleMenuScrollOffset_ + direction, 0, maxOffset);
        if (nextOffset != subtitleMenuScrollOffset_) {
            subtitleMenuScrollOffset_ = nextOffset;
            hoveredSubtitleMenuItem_ = HitSubtitleMenuItem(screenPoint);
        }
        InvalidateTransportArea();
        InvalidateFullscreenOverlay();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (ContainsPoint(VolumeSliderHitRect(), screenPoint)) {
        const auto snapshot = controller_.Snapshot();
        const double steps = std::clamp(static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA), -3.0, 3.0);
        if (std::abs(steps) > 0.001) {
            ApplyVolume(snapshot.volume + steps * 0.05, true);
        }
        SetVolumeSliderHover(true);
        if (fullscreen_) {
            ShowFullscreenTransport(L"wheel_volume");
        }
        InvalidateTransportArea();
        return;
    }
    if (inspectorTab_ != InspectorTab::Settings ||
        settingsScrollMax_ <= 0 ||
        (!ContainsPoint(inspector_, screenPoint) && !ContainsPoint(settingsContentViewport_, screenPoint))) {
        return;
    }

    const int scrollDelta = -delta * Scale(96) / WHEEL_DELTA;
    ScrollSettingsBy(scrollDelta);
}

bool MainWindow::ScrollSettingsBy(const int delta) {
    if (inspectorTab_ != InspectorTab::Settings || settingsScrollMax_ <= 0 || delta == 0) {
        return false;
    }

    const int next = std::clamp(settingsScrollOffset_ + delta, 0, settingsScrollMax_);
    MarkSettingsScrollbarActive();
    if (next == settingsScrollOffset_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    settingsScrollOffset_ = next;
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool MainWindow::BeginSettingsScrollDrag(const POINT point) {
    if (inspectorTab_ != InspectorTab::Settings ||
        settingsScrollMax_ <= 0 ||
        RectHeight(settingsScrollTrack_) <= 0 ||
        !ContainsPoint(settingsScrollTrack_, point)) {
        return false;
    }

    if (!ContainsPoint(settingsScrollThumb_, point)) {
        const int travel = std::max(1, RectHeight(settingsScrollTrack_) - RectHeight(settingsScrollThumb_));
        const int desiredTop = std::clamp(static_cast<int>(point.y) - RectHeight(settingsScrollThumb_) / 2,
                                          static_cast<int>(settingsScrollTrack_.top),
                                          static_cast<int>(settingsScrollTrack_.bottom) - RectHeight(settingsScrollThumb_));
        settingsScrollOffset_ = std::clamp(
            static_cast<int>(std::round(static_cast<double>(desiredTop - settingsScrollTrack_.top) *
                                        static_cast<double>(settingsScrollMax_) /
                                        static_cast<double>(travel))),
            0,
            settingsScrollMax_);
        MarkLayoutDirty();
        EnsureLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    MarkSettingsScrollbarActive();
    draggingSettingsScrollThumb_ = true;
    settingsScrollDragStartY_ = point.y;
    settingsScrollDragStartOffset_ = settingsScrollOffset_;
    SetCapture(hwnd_);
    return true;
}

void MainWindow::UpdateSettingsScrollDrag(const POINT point) {
    if (!draggingSettingsScrollThumb_ ||
        settingsScrollMax_ <= 0 ||
        RectHeight(settingsScrollTrack_) <= 0 ||
        RectHeight(settingsScrollThumb_) <= 0) {
        return;
    }

    const int travel = std::max(1, RectHeight(settingsScrollTrack_) - RectHeight(settingsScrollThumb_));
    const int dy = point.y - settingsScrollDragStartY_;
    const int next = std::clamp(
        settingsScrollDragStartOffset_ +
            static_cast<int>(std::round(static_cast<double>(dy) *
                                        static_cast<double>(settingsScrollMax_) /
                                        static_cast<double>(travel))),
        0,
        settingsScrollMax_);
    if (next == settingsScrollOffset_) {
        MarkSettingsScrollbarActive();
        return;
    }

    settingsScrollOffset_ = next;
    MarkSettingsScrollbarActive();
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EndSettingsScrollDrag() {
    if (!draggingSettingsScrollThumb_) {
        return;
    }

    draggingSettingsScrollThumb_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CancelSettingsScrollDrag() {
    if (!draggingSettingsScrollThumb_) {
        return;
    }

    draggingSettingsScrollThumb_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

RECT MainWindow::ProgressHitRect() const {
    RECT hit = progress_;
    const int padY = Scale(10);
    hit.top -= padY;
    hit.bottom += padY;
    return hit;
}

RECT MainWindow::VolumeSliderTrackRect() const {
    if (RectWidth(volumeSlider_) <= 0 || RectHeight(volumeSlider_) <= 0) {
        return RECT{};
    }

    const int centerY = volumeSlider_.top + RectHeight(volumeSlider_) / 2;
    const int trackLeft = volumeSlider_.left + Scale(36);
    const int trackRight = volumeSlider_.right - Scale(8);
    if (trackRight <= trackLeft) {
        return RECT{};
    }
    return MakeRect(trackLeft, centerY - Scale(2), trackRight, centerY + Scale(2));
}

RECT MainWindow::VolumeSliderHitRect() const {
    if (RectWidth(volumeSlider_) <= 0 || RectHeight(volumeSlider_) <= 0) {
        return RECT{};
    }

    RECT hit = volumeSlider_;
    InflateRect(&hit, Scale(4), Scale(8));
    return hit;
}

bool MainWindow::IsPointInSubtitleMenu(const POINT point) const {
    if (subtitleMenuAmount_ <= 0.01 || RectWidth(subtitleMenu_) <= 0 || RectHeight(subtitleMenu_) <= 0) {
        return false;
    }
    return ContainsPoint(subtitleMenu_, point);
}

int MainWindow::HitSubtitleMenuItem(const POINT point) const {
    if (!IsPointInSubtitleMenu(point) || subtitleMenuPage_ == SubtitleMenuPage::Danmaku) {
        return -1;
    }

    const int count = subtitleMenuPage_ == SubtitleMenuPage::Audio
                          ? static_cast<int>(audioMenuTracks_.size())
                          : static_cast<int>(subtitleMenuTracks_.size());
    if (count <= 0) {
        return -1;
    }

    const int headerHeight = Scale(62);
    const int listGap = Scale(8);
    const int itemHeight = Scale(42);
    const int y = point.y - subtitleMenu_.top - headerHeight - listGap;
    if (y < 0) {
        return -1;
    }
    const int localIndex = y / itemHeight;
    const int visibleCount = subtitleMenuVisibleItemCount_ > 0
                                 ? std::clamp(subtitleMenuVisibleItemCount_, 1, count)
                                 : count;
    if (localIndex < 0 || localIndex >= visibleCount) {
        return -1;
    }
    const int index = std::clamp(subtitleMenuScrollOffset_, 0, std::max(0, count - visibleCount)) + localIndex;
    if (index < 0 || index >= count) {
        return -1;
    }
    return index;
}

MainWindow::SubtitleMenuAction MainWindow::HitSubtitleMenuAction(const POINT point) const {
    if (!IsPointInSubtitleMenu(point)) {
        return SubtitleMenuAction::None;
    }

    const int headerHeight = Scale(62);
    const RECT header = MakeRect(subtitleMenu_.left,
                                 subtitleMenu_.top,
                                 subtitleMenu_.right,
                                 subtitleMenu_.top + headerHeight);
    const int tabGap = Scale(8);
    const RECT tabRail = MakeRect(header.left + Scale(14),
                                  header.top + Scale(12),
                                  header.right - Scale(14),
                                  header.bottom - Scale(14));
    const int tabWidth = (RectWidth(tabRail) - tabGap * 2) / 3;
    for (int index = 0; index < 3; ++index) {
        const RECT tab = MakeRect(tabRail.left + index * (tabWidth + tabGap),
                                  tabRail.top,
                                  tabRail.left + index * (tabWidth + tabGap) + tabWidth,
                                  tabRail.bottom);
        if (!ContainsPoint(tab, point)) {
            continue;
        }
        if (index == 0) {
            return SubtitleMenuAction::TabAudio;
        }
        if (index == 1) {
            return SubtitleMenuAction::TabSubtitles;
        }
        return SubtitleMenuAction::TabDanmaku;
    }

    const int listGap = Scale(8);
    const int itemHeight = Scale(42);
    const int listBottomGap = Scale(6);
    const int delayHeight = Scale(56);
    const int actionHeight = Scale(44);
    const int styleHeight = Scale(48);

    if (subtitleMenuPage_ == SubtitleMenuPage::Danmaku) {
        int rowTop = subtitleMenu_.top + headerHeight + Scale(8);
        const auto row = [&](const int height) {
            RECT result = MakeRect(subtitleMenu_.left, rowTop, subtitleMenu_.right, rowTop + height);
            rowTop += height;
            return result;
        };
        if (ContainsPoint(row(actionHeight), point)) {
            return SubtitleMenuAction::DanmakuToggle;
        }
        if (ContainsPoint(row(actionHeight), point)) {
            return SubtitleMenuAction::DanmakuMode;
        }
        const RECT opacityRow = row(styleHeight);
        if (ContainsPoint(opacityRow, point)) {
            if (point.x < opacityRow.left + Scale(76)) {
                return SubtitleMenuAction::DanmakuOpacityDown;
            }
            if (point.x > opacityRow.right - Scale(76)) {
                return SubtitleMenuAction::DanmakuOpacityUp;
            }
            return SubtitleMenuAction::None;
        }
        const RECT speedRow = row(styleHeight);
        if (ContainsPoint(speedRow, point)) {
            if (point.x < speedRow.left + Scale(76)) {
                return SubtitleMenuAction::DanmakuSpeedDown;
            }
            if (point.x > speedRow.right - Scale(76)) {
                return SubtitleMenuAction::DanmakuSpeedUp;
            }
            return SubtitleMenuAction::None;
        }
        if (ContainsPoint(row(actionHeight), point)) {
            return SubtitleMenuAction::AddDanmakuFile;
        }
        return SubtitleMenuAction::None;
    }

    if (subtitleMenuPage_ == SubtitleMenuPage::Audio) {
        return SubtitleMenuAction::None;
    }

    const int count = std::max(1, static_cast<int>(subtitleMenuTracks_.size()));
    const int visibleCount = subtitleMenuVisibleItemCount_ > 0
                                 ? std::clamp(subtitleMenuVisibleItemCount_, 1, count)
                                 : count;
    const int listTop = subtitleMenu_.top + headerHeight + listGap;
    const int listBottom = listTop + itemHeight * visibleCount + listBottomGap;
    const RECT delayRow = MakeRect(subtitleMenu_.left, listBottom, subtitleMenu_.right, listBottom + delayHeight);
    if (ContainsPoint(delayRow, point)) {
        const RECT minusHit = MakeRect(delayRow.left, delayRow.top, delayRow.left + Scale(76), delayRow.bottom);
        const RECT plusHit = MakeRect(delayRow.right - Scale(76), delayRow.top, delayRow.right, delayRow.bottom);
        if (ContainsPoint(minusHit, point)) {
            return SubtitleMenuAction::DelayDown;
        }
        if (ContainsPoint(plusHit, point)) {
            return SubtitleMenuAction::DelayUp;
        }
        return SubtitleMenuAction::None;
    }

    const RECT addRow = MakeRect(subtitleMenu_.left, delayRow.bottom, subtitleMenu_.right, delayRow.bottom + actionHeight);
    if (ContainsPoint(addRow, point)) {
        return SubtitleMenuAction::AddFile;
    }

    const RECT sizeRow = MakeRect(subtitleMenu_.left, addRow.bottom, subtitleMenu_.right, addRow.bottom + styleHeight);
    if (ContainsPoint(sizeRow, point)) {
        if (point.x < sizeRow.left + Scale(76)) {
            return SubtitleMenuAction::SubtitleSizeDown;
        }
        if (point.x > sizeRow.right - Scale(76)) {
            return SubtitleMenuAction::SubtitleSizeUp;
        }
        return SubtitleMenuAction::None;
    }

    const RECT offsetXRow = MakeRect(subtitleMenu_.left, sizeRow.bottom, subtitleMenu_.right, sizeRow.bottom + styleHeight);
    if (ContainsPoint(offsetXRow, point)) {
        if (point.x < offsetXRow.left + Scale(76)) {
            return SubtitleMenuAction::SubtitleOffsetXDown;
        }
        if (point.x > offsetXRow.right - Scale(76)) {
            return SubtitleMenuAction::SubtitleOffsetXUp;
        }
        return SubtitleMenuAction::None;
    }

    const RECT offsetYRow = MakeRect(subtitleMenu_.left, offsetXRow.bottom, subtitleMenu_.right, offsetXRow.bottom + styleHeight);
    if (ContainsPoint(offsetYRow, point)) {
        if (point.x < offsetYRow.left + Scale(76)) {
            return SubtitleMenuAction::SubtitleOffsetYDown;
        }
        if (point.x > offsetYRow.right - Scale(76)) {
            return SubtitleMenuAction::SubtitleOffsetYUp;
        }
        return SubtitleMenuAction::None;
    }

    return SubtitleMenuAction::None;
}

double MainWindow::VolumeFromSliderX(const int x) const {
    const RECT track = VolumeSliderTrackRect();
    if (RectWidth(track) <= 0) {
        return controller_.Snapshot().volume;
    }

    return std::clamp(static_cast<double>(x - track.left) / static_cast<double>(RectWidth(track)), 0.0, 1.0);
}

bool MainWindow::ApplyVolume(const double volume, const bool restartExternalNow) {
    const int nextPercent = std::clamp(static_cast<int>(std::round(volume * 100.0)), 0, 100);
    const auto snapshot = controller_.Snapshot();
    const int currentPercent = std::clamp(static_cast<int>(std::round(snapshot.volume * 100.0)), 0, 100);
    if (nextPercent == currentPercent) {
        return false;
    }

    controller_.SetVolume(static_cast<double>(nextPercent) / 100.0);
    const auto updated = controller_.Snapshot();
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 ||
        backend_ == PlaybackBackend::RawFrameBridge) {
        audioPlayer_.SetVolume(updated.volume);
    } else if (restartExternalNow) {
        RestartPlaybackIfPlaying();
    }

    if (draggingVolume_) {
        volumeDragChanged_ = true;
    }
    InvalidateTransportArea();
    PostWebUiState();
    return true;
}

bool MainWindow::BeginVolumeDrag(const POINT point) {
    if (!ContainsPoint(VolumeSliderHitRect(), point)) {
        return false;
    }

    draggingVolume_ = true;
    volumeDragChanged_ = false;
    SetVolumeSliderHover(true);
    SetCapture(hwnd_);
    UpdateVolumeDrag(point);
    InvalidateTransportArea();
    return true;
}

void MainWindow::UpdateVolumeDrag(const POINT point) {
    if (!draggingVolume_ || RectWidth(volumeSlider_) <= 0) {
        return;
    }

    ApplyVolume(VolumeFromSliderX(point.x), false);
}

void MainWindow::EndVolumeDrag(const POINT point) {
    if (!draggingVolume_) {
        return;
    }

    UpdateVolumeDrag(point);
    draggingVolume_ = false;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    if (volumeDragChanged_ &&
        backend_ != PlaybackBackend::NativeFfmpegD3D11 &&
        backend_ != PlaybackBackend::RawFrameBridge) {
        RestartPlaybackIfPlaying();
    }
    volumeDragChanged_ = false;
    SetVolumeSliderHover(ContainsPoint(VolumeSliderHitRect(), point));
    InvalidateTransportArea();
}

void MainWindow::CancelVolumeDrag() {
    if (!draggingVolume_) {
        return;
    }

    draggingVolume_ = false;
    volumeDragChanged_ = false;
    SetVolumeSliderHover(false);
    InvalidateTransportArea();
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
           HdrToneCurveAvailable(settings) &&
           RectWidth(hdrToneCurvePlot_) > 0 &&
           RectHeight(hdrToneCurvePlot_) > 0;
}

HWND MainWindow::HdrToneCurveInteractionWindow() const {
    return hdrToneCurveExpanded_ && hdrToneCurveWindow_ ? hdrToneCurveWindow_ : hwnd_;
}

int MainWindow::HitHdrToneCurvePoint(const POINT point, const int maxDistancePx) const {
    if (!IsHdrToneCurveVisible()) {
        return -1;
    }

    const auto settings = controller_.Settings();
    int nearest = -1;
    int nearestDistanceSq = maxDistancePx * maxDistancePx;
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

    return nearest;
}

void MainWindow::UpdateHdrToneCurveHover(const POINT point) {
    const int previous = hoveredHdrToneCurvePoint_;
    hoveredHdrToneCurvePoint_ = HitHdrToneCurvePoint(point, Scale(18));
    if (previous != hoveredHdrToneCurvePoint_) {
        InvalidateHdrToneCurveEditor();
    }
}

bool MainWindow::HasHdrToneCurveSelection() const {
    for (std::size_t index = 1; index < selectedHdrToneCurvePoints_.size(); ++index) {
        if (selectedHdrToneCurvePoints_[index]) {
            return true;
        }
    }
    return false;
}

int MainWindow::HdrToneCurveSelectionCount() const {
    int count = 0;
    for (std::size_t index = 1; index < selectedHdrToneCurvePoints_.size(); ++index) {
        if (selectedHdrToneCurvePoints_[index]) {
            ++count;
        }
    }
    return count;
}

void MainWindow::ClearHdrToneCurveSelection() {
    if (!HasHdrToneCurveSelection()) {
        return;
    }
    selectedHdrToneCurvePoints_.fill(false);
    draggedHdrToneCurvePoint_ = -1;
    InvalidateHdrToneCurveEditor();
}

void MainWindow::SelectHdrToneCurvePoint(const int pointIndex) {
    selectedHdrToneCurvePoints_.fill(false);
    if (pointIndex > 0 && pointIndex < static_cast<int>(selectedHdrToneCurvePoints_.size())) {
        selectedHdrToneCurvePoints_[static_cast<std::size_t>(pointIndex)] = true;
        draggedHdrToneCurvePoint_ = pointIndex;
    } else {
        draggedHdrToneCurvePoint_ = -1;
    }
    InvalidateHdrToneCurveEditor();
}

void MainWindow::SelectHdrToneCurveRange(const int leftX, const int rightX) {
    if (!IsHdrToneCurveVisible()) {
        return;
    }

    const int rangeLeft = std::min(leftX, rightX) - Scale(4);
    const int rangeRight = std::max(leftX, rightX) + Scale(4);
    selectedHdrToneCurvePoints_.fill(false);
    draggedHdrToneCurvePoint_ = -1;

    const auto settings = controller_.Settings();
    for (std::size_t index = 1; index < settings.video.hdrToneCurve.size(); ++index) {
        const auto& curvePoint = settings.video.hdrToneCurve[index];
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : curvePoint.inputNits;
        const int x = ToneCurveX(hdrToneCurvePlot_, inputNits);
        if (x >= rangeLeft && x <= rangeRight) {
            selectedHdrToneCurvePoints_[index] = true;
            if (draggedHdrToneCurvePoint_ < 0) {
                draggedHdrToneCurvePoint_ = static_cast<int>(index);
            }
        }
    }

    InvalidateHdrToneCurveEditor();
}

bool MainWindow::BeginHdrToneCurveRangeSelection(const POINT point) {
    if (!IsHdrToneCurveVisible() || !ContainsPoint(hdrToneCurvePlot_, point)) {
        return false;
    }

    selectingHdrToneCurveRange_ = true;
    hdrToneCurveSelectionStartX_ = std::clamp(static_cast<int>(point.x),
                                              static_cast<int>(hdrToneCurvePlot_.left),
                                              static_cast<int>(hdrToneCurvePlot_.right));
    hdrToneCurveSelectionCurrentX_ = hdrToneCurveSelectionStartX_;
    SelectHdrToneCurveRange(hdrToneCurveSelectionStartX_, hdrToneCurveSelectionCurrentX_);
    SetCapture(HdrToneCurveInteractionWindow());
    return true;
}

void MainWindow::UpdateHdrToneCurveRangeSelection(const POINT point) {
    if (!selectingHdrToneCurveRange_ || !IsHdrToneCurveVisible()) {
        return;
    }

    hdrToneCurveSelectionCurrentX_ = std::clamp(static_cast<int>(point.x),
                                                static_cast<int>(hdrToneCurvePlot_.left),
                                                static_cast<int>(hdrToneCurvePlot_.right));
    SelectHdrToneCurveRange(hdrToneCurveSelectionStartX_, hdrToneCurveSelectionCurrentX_);
}

void MainWindow::EndHdrToneCurveRangeSelection() {
    if (!selectingHdrToneCurveRange_) {
        return;
    }

    selectingHdrToneCurveRange_ = false;
    if (!HasHdrToneCurveSelection()) {
        draggedHdrToneCurvePoint_ = -1;
    }
    if (GetCapture() == HdrToneCurveInteractionWindow()) {
        ReleaseCapture();
    }
    InvalidateHdrToneCurveEditor();
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

    const int nearest = HitHdrToneCurvePoint(point, Scale(24));
    if (nearest > 0) {
        const bool clickedSelected = nearest < static_cast<int>(selectedHdrToneCurvePoints_.size()) &&
                                     selectedHdrToneCurvePoints_[static_cast<std::size_t>(nearest)];
        const bool groupDrag = clickedSelected && HdrToneCurveSelectionCount() > 1;
        if (!clickedSelected) {
            SelectHdrToneCurvePoint(nearest);
        }
        StartHdrToneCurveDrag(nearest, groupDrag, point);
        return true;
    }

    return false;
}

void MainWindow::StartHdrToneCurveDrag(const int pointIndex, const bool groupDrag, const POINT point) {
    if (pointIndex <= 0 || pointIndex >= static_cast<int>(selectedHdrToneCurvePoints_.size())) {
        return;
    }

    if (!groupDrag) {
        SelectHdrToneCurvePoint(pointIndex);
    } else if (!HasHdrToneCurveSelection()) {
        selectedHdrToneCurvePoints_[static_cast<std::size_t>(pointIndex)] = true;
    }

    auto settings = controller_.Settings();
    auto curve = settings.video.hdrToneCurve;
    NormalizeHdrToneCurveForEditing(curve);
    for (std::size_t index = 0; index < curve.size(); ++index) {
        hdrToneCurveDragStartOutputs_[index] = curve[index].outputNits;
    }

    draggingHdrToneCurve_ = true;
    draggingHdrToneCurveSelection_ = groupDrag;
    hdrToneCurveDragMoved_ = false;
    draggedHdrToneCurvePoint_ = pointIndex;
    hoveredHdrToneCurvePoint_ = pointIndex;
    hdrToneCurveDragStart_ = point;
    hdrToneCurveDragStartPointerNits_ = ToneCurveYToNits(hdrToneCurvePlot_, point.y);
    SetCapture(HdrToneCurveInteractionWindow());
    InvalidateHdrToneCurveEditor();
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
    NormalizeHdrToneCurveForEditing(curve);

    if (draggingHdrToneCurveSelection_) {
        const double currentPointerNits = ToneCurveYToNits(hdrToneCurvePlot_, point.y);
        const double desiredDelta = std::round(currentPointerNits - hdrToneCurveDragStartPointerNits_);
        const double delta = ClampHdrToneCurveSelectionDelta(curve, selectedHdrToneCurvePoints_, hdrToneCurveDragStartOutputs_, desiredDelta);
        for (std::size_t index = 1; index < curve.size(); ++index) {
            if (selectedHdrToneCurvePoints_[index]) {
                curve[index].outputNits = RoundToneCurveNits(hdrToneCurveDragStartOutputs_[index] + delta);
            }
        }
    } else {
        const std::size_t index = static_cast<std::size_t>(draggedHdrToneCurvePoint_);
        if (index >= curve.size()) {
            return;
        }

        const double minOutput = curve[index - 1].outputNits;
        const double maxOutput = index + 1 < curve.size()
                                     ? curve[index + 1].outputNits
                                     : kHdrToneCurveMaxNits;
        const double outputNits = std::clamp(RoundToneCurveNits(ToneCurveYToNits(hdrToneCurvePlot_, point.y)),
                                             minOutput,
                                             std::max(minOutput, maxOutput));
        curve[index].outputNits = outputNits;
    }

    settings.video.peakBrightnessNits = static_cast<int>(std::clamp(curve.back().outputNits, 100.0, kHdrToneCurveMaxNits));

    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
}

void MainWindow::EndHdrToneCurveDrag() {
    if (!draggingHdrToneCurve_) {
        return;
    }

    draggingHdrToneCurve_ = false;
    draggingHdrToneCurveSelection_ = false;
    hdrToneCurveDragMoved_ = false;
    draggedHdrToneCurvePoint_ = -1;
    if (GetCapture() == HdrToneCurveInteractionWindow()) {
        ReleaseCapture();
    }

    const auto settings = controller_.Settings();
    LogApp(anvil::playback::LogLevel::Info,
           L"hdr tone curve peak=" + std::to_wstring(settings.video.peakBrightnessNits) + L" nits");
    InvalidateHdrToneCurveEditor();
}

void MainWindow::CancelHdrToneCurveInteraction() {
    if (!draggingHdrToneCurve_ && !selectingHdrToneCurveRange_) {
        return;
    }

    draggingHdrToneCurve_ = false;
    draggingHdrToneCurveSelection_ = false;
    selectingHdrToneCurveRange_ = false;
    hdrToneCurveDragMoved_ = false;
    draggedHdrToneCurvePoint_ = -1;
    if (GetCapture() == HdrToneCurveInteractionWindow()) {
        ReleaseCapture();
    }
    InvalidateHdrToneCurveEditor();
}

bool MainWindow::NudgeHdrToneCurveSelection(const double deltaNits) {
    if (!IsHdrToneCurveVisible() || !HasHdrToneCurveSelection()) {
        return false;
    }

    auto settings = controller_.Settings();
    auto& curve = settings.video.hdrToneCurve;
    NormalizeHdrToneCurveForEditing(curve);

    std::array<double, anvil::playback::kHdrToneCurvePointCount> startOutputs{};
    for (std::size_t index = 0; index < curve.size(); ++index) {
        startOutputs[index] = curve[index].outputNits;
    }

    const double delta = ClampHdrToneCurveSelectionDelta(curve, selectedHdrToneCurvePoints_, startOutputs, deltaNits);
    if (std::abs(delta) < 0.5) {
        return true;
    }

    for (std::size_t index = 1; index < curve.size(); ++index) {
        if (selectedHdrToneCurvePoints_[index]) {
            curve[index].outputNits = RoundToneCurveNits(startOutputs[index] + delta);
        }
    }
    settings.video.peakBrightnessNits = static_cast<int>(std::clamp(curve.back().outputNits, 100.0, kHdrToneCurveMaxNits));
    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
    return true;
}

void MainWindow::ResetHdrToneCurve() {
    ClearHdrToneCurveSelection();
    auto settings = controller_.Settings();
    settings.video.hdrToneCurve = anvil::playback::kDefaultHdrToneCurve;
    settings.video.peakBrightnessNits =
        static_cast<int>(std::clamp(settings.video.hdrToneCurve.back().outputNits, 100.0, kHdrToneCurveMaxNits));
    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
    LogApp(anvil::playback::LogLevel::Info, L"hdr tone curve reset");
}

void MainWindow::ShowHdrToneCurveWindow() {
    const auto settings = controller_.Settings();
    if (!HdrToneCurveAvailable(settings)) {
        return;
    }

    hdrToneCurveExpanded_ = true;
    EnsureHdrToneCurveWindow();
    UpdateHdrToneCurveFloatingLayout();
    if (hdrToneCurveWindow_) {
        ShowWindow(hdrToneCurveWindow_, SW_SHOWNORMAL);
        SetWindowPos(hdrToneCurveWindow_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(hdrToneCurveWindow_);
        InvalidateRect(hdrToneCurveWindow_, nullptr, FALSE);
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::HideHdrToneCurveWindow() {
    const bool wasExpanded = hdrToneCurveExpanded_;
    const bool wasVisible = hdrToneCurveWindow_ && IsWindowVisible(hdrToneCurveWindow_);
    hdrToneCurveExpanded_ = false;
    ClearHdrToneCurveSelection();
    if (hdrToneCurveWindow_) {
        ShowWindow(hdrToneCurveWindow_, SW_HIDE);
    }
    if (!wasExpanded && !wasVisible) {
        return;
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateHdrToneCurveEditor();
    PostWebUiState();
}

void MainWindow::ApplyHdrToneCurvePoint(const int pointIndex, const double outputNits) {
    auto settings = controller_.Settings();
    if (!HdrToneCurveAvailable(settings) ||
        pointIndex <= 0 ||
        pointIndex >= static_cast<int>(settings.video.hdrToneCurve.size())) {
        return;
    }

    auto& curve = settings.video.hdrToneCurve;
    NormalizeHdrToneCurveForEditing(curve);

    const std::size_t index = static_cast<std::size_t>(pointIndex);
    const double minOutput = curve[index - 1].outputNits;
    const double maxOutput = index + 1 < curve.size()
                                 ? curve[index + 1].outputNits
                                 : kHdrToneCurveMaxNits;
    const double nextOutput = std::clamp(RoundToneCurveNits(outputNits),
                                         minOutput,
                                         std::max(minOutput, maxOutput));
    if (std::abs(nextOutput - curve[index].outputNits) < 0.5) {
        return;
    }

    curve[index].outputNits = nextOutput;
    settings.video.peakBrightnessNits =
        static_cast<int>(std::clamp(curve.back().outputNits, 100.0, kHdrToneCurveMaxNits));
    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
}

void MainWindow::ApplyHdrToneCurvePoints(
    const std::array<double, anvil::playback::kHdrToneCurvePointCount>& outputNits,
    const std::array<bool, anvil::playback::kHdrToneCurvePointCount>& hasOutput) {
    auto settings = controller_.Settings();
    if (!HdrToneCurveAvailable(settings)) {
        return;
    }

    auto& curve = settings.video.hdrToneCurve;
    NormalizeHdrToneCurveForEditing(curve);

    std::array<double, anvil::playback::kHdrToneCurvePointCount> nextOutputs{};
    for (std::size_t index = 0; index < curve.size(); ++index) {
        nextOutputs[index] = curve[index].outputNits;
    }

    bool hasAnyOutput = false;
    for (std::size_t index = 1; index < curve.size() && index < hasOutput.size(); ++index) {
        if (!hasOutput[index]) {
            continue;
        }
        nextOutputs[index] = RoundToneCurveNits(outputNits[index]);
        hasAnyOutput = true;
    }
    if (!hasAnyOutput) {
        return;
    }

    std::size_t segmentIndex = 1;
    while (segmentIndex < curve.size() && segmentIndex < hasOutput.size()) {
        if (!hasOutput[segmentIndex]) {
            ++segmentIndex;
            continue;
        }

        const std::size_t start = segmentIndex;
        while (segmentIndex + 1 < curve.size() &&
               segmentIndex + 1 < hasOutput.size() &&
               hasOutput[segmentIndex + 1]) {
            ++segmentIndex;
        }
        const std::size_t end = segmentIndex;
        const double lower = nextOutputs[start - 1];
        const double upper = end + 1 < curve.size()
                                 ? nextOutputs[end + 1]
                                 : kHdrToneCurveMaxNits;
        const double boundedUpper = std::max(lower, upper);

        for (std::size_t index = start; index <= end; ++index) {
            nextOutputs[index] = std::clamp(nextOutputs[index], lower, boundedUpper);
            if (index > start) {
                nextOutputs[index] = std::max(nextOutputs[index], nextOutputs[index - 1]);
            }
        }
        ++segmentIndex;
    }

    bool changed = false;
    for (std::size_t index = 1; index < curve.size() && index < hasOutput.size(); ++index) {
        if (!hasOutput[index]) {
            continue;
        }
        if (std::abs(nextOutputs[index] - curve[index].outputNits) >= 0.5) {
            curve[index].outputNits = nextOutputs[index];
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    settings.video.peakBrightnessNits =
        static_cast<int>(std::clamp(curve.back().outputNits, 100.0, kHdrToneCurveMaxNits));
    controller_.ApplySettings(settings);
    ApplyLiveHdrToneCurveSettings();
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
        ShowFullscreenTransport(L"video_long_press");
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
    case VK_UP:
        if (NudgeHdrToneCurveSelection(1.0)) {
            break;
        }
        break;
    case VK_DOWN:
        if (NudgeHdrToneCurveSelection(-1.0)) {
            break;
        }
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
        } else if (subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) {
            HideSubtitleMenu();
        } else if (hdrToneCurveExpanded_) {
            HideHdrToneCurveWindow();
        }
        break;
    default:
        break;
    }
    if (refreshFullscreenTransport && fullscreen_) {
        ShowFullscreenTransport(L"key_down");
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
        RefreshPausedNativeFrame(updated, true);
    } else {
        RestartPlaybackIfPlaying();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::ApplyAudioSelection(const int selectedTrackIndex) {
    auto settings = controller_.Settings();
    if (settings.audio.selectedTrackIndex == selectedTrackIndex) {
        return;
    }

    settings.audio.selectedTrackIndex = selectedTrackIndex;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"audio track=" + AudioSelectionLogLabel(settings.audio.selectedTrackIndex));

    const auto updated = controller_.Snapshot();
    if (updated.state == PlaybackState::Playing) {
        RestartPlaybackIfPlaying();
    } else if (updated.state == PlaybackState::Paused &&
               updated.media.has_value() &&
               updated.media->hasAudio &&
               backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        audioPlayer_.Stop();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplySubtitleDelayDelta(const int deltaMs) {
    auto settings = controller_.Settings();
    const int nextDelay = std::clamp(settings.subtitles.subtitleDelayMs + deltaMs, -600000, 600000);
    if (nextDelay == settings.subtitles.subtitleDelayMs) {
        return;
    }

    settings.subtitles.subtitleDelayMs = nextDelay;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info, L"subtitle delay_ms=" + std::to_wstring(nextDelay));

    const auto updated = controller_.Snapshot();
    if (updated.state == PlaybackState::Paused &&
        updated.media.has_value() &&
        updated.media->hasVideo &&
        backend_ == PlaybackBackend::NativeFfmpegD3D11) {
        RefreshPausedNativeFrame(updated, true);
    } else {
        RestartPlaybackIfPlaying();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplySubtitleFontScaleDelta(const double delta) {
    auto settings = controller_.Settings();
    const double nextScale = std::clamp(std::round((settings.subtitles.fontScale + delta) * 100.0) / 100.0, 0.50, 2.00);
    if (std::abs(nextScale - settings.subtitles.fontScale) < 0.001) {
        return;
    }

    settings.subtitles.fontScale = nextScale;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"subtitle font_scale=" + std::to_wstring(static_cast<int>(std::round(nextScale * 100.0))) + L"%");
    ApplyLiveSubtitleStyleSettings();
}

void MainWindow::ApplySubtitleOffsetDelta(const int deltaX, const int deltaY) {
    auto settings = controller_.Settings();
    const int nextX = std::clamp(settings.subtitles.offsetXPx + deltaX, -400, 400);
    const int nextY = std::clamp(settings.subtitles.offsetYPx + deltaY, -400, 400);
    if (nextX == settings.subtitles.offsetXPx && nextY == settings.subtitles.offsetYPx) {
        return;
    }

    settings.subtitles.offsetXPx = nextX;
    settings.subtitles.offsetYPx = nextY;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"subtitle offset x=" + std::to_wstring(nextX) + L" y=" + std::to_wstring(nextY));
    ApplyLiveSubtitleStyleSettings();
}

void MainWindow::ApplyLiveSubtitleStyleSettings() {
    const auto settings = controller_.Settings();
    if (d3dRenderer_) {
        d3dRenderer_->ConfigureSubtitleSettings(settings.subtitles);
    }
    const auto snapshot = controller_.Snapshot();
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        snapshot.state == PlaybackState::Paused &&
        heldNativeFrame_.has_value()) {
        RenderHeldNativeFrame();
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleDanmakuEnabled() {
    auto settings = controller_.Settings();
    settings.danmaku.enabled = !settings.danmaku.enabled;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"danmaku enabled=" + std::wstring(settings.danmaku.enabled ? L"true" : L"false"));
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
}

void MainWindow::CycleDanmakuMode() {
    auto settings = controller_.Settings();
    settings.danmaku.mode = (settings.danmaku.mode + 1) % 3;
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"danmaku mode=" + std::to_wstring(settings.danmaku.mode));
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
}

void MainWindow::ApplyDanmakuOpacityDelta(const int deltaPercent) {
    auto settings = controller_.Settings();
    const int next = std::clamp(settings.danmaku.opacityPercent + deltaPercent, 20, 100);
    if (next == settings.danmaku.opacityPercent) {
        return;
    }
    settings.danmaku.opacityPercent = next;
    controller_.ApplySettings(settings);
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
}

void MainWindow::ApplyDanmakuSpeedDelta(const int deltaPercent) {
    auto settings = controller_.Settings();
    const int next = std::clamp(settings.danmaku.speedPercent + deltaPercent, 50, 200);
    if (next == settings.danmaku.speedPercent) {
        return;
    }
    settings.danmaku.speedPercent = next;
    controller_.ApplySettings(settings);
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
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
    if (!CurrentMediaHasHdrControls()) {
        return;
    }

    auto settings = controller_.Settings();
    settings.video.dolbyVisionHdrOutput = !settings.video.dolbyVisionHdrOutput;
    if (!settings.video.dolbyVisionHdrOutput) {
        HideHdrToneCurveWindow();
    }
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"dolby vision hdr output=" +
               std::wstring(settings.video.dolbyVisionHdrOutput ? L"on" : L"off"));

    const auto snapshot = controller_.Snapshot();
    bool handledLive = false;
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        snapshot.media.has_value() &&
        snapshot.media->hasVideo &&
        !NativeHdrOutputToggleRequiresDecoderRestart(snapshot)) {
        handledLive = ApplyNativeColorSettingsLive(snapshot);
    }
    if (!handledLive) {
        if (snapshot.state == PlaybackState::Paused &&
            snapshot.media.has_value() &&
            snapshot.media->hasVideo &&
            backend_ == PlaybackBackend::NativeFfmpegD3D11) {
            RefreshPausedNativeFrame(snapshot);
        } else {
            RestartPlaybackIfPlaying();
        }
    }
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
    PostWebUiState();
}

void MainWindow::ToggleDolbyVisionCmv4Approx() {
    auto settings = controller_.Settings();
    if (!CurrentCmv4ControlEnabled(settings)) {
        return;
    }

    settings.video.dolbyVisionCmv4Approx = !settings.video.dolbyVisionCmv4Approx;
    if (settings.video.dolbyVisionCmv4Approx) {
        HideHdrToneCurveWindow();
    }
    controller_.ApplySettings(settings);
    LogApp(anvil::playback::LogLevel::Info,
           L"dolby vision cmv4 approx=" +
               std::wstring(settings.video.dolbyVisionCmv4Approx ? L"on" : L"off"));

    const auto snapshot = controller_.Snapshot();
    const bool requiresDecoderRefresh = NativeCmv4ToggleNeedsDecoderRefresh(snapshot, settings.video);
    ScheduleNativeColorSettingsRefresh(requiresDecoderRefresh);
    MarkLayoutDirty();
    EnsureLayout();
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateHdrToneCurveEditor();
    if (transportOverlay_ && IsWindowVisible(transportOverlay_)) {
        UpdateWindow(transportOverlay_);
    }
    if (fullscreenOverlay_ && IsWindowVisible(fullscreenOverlay_)) {
        UpdateWindow(fullscreenOverlay_);
    }
    UpdateWindow(hwnd_);
    PostWebUiState();
}

void MainWindow::ShowSubtitleMenu() {
    EnsureLayout();

    const auto snapshot = controller_.Snapshot();
    if (subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) {
        HideSubtitleMenu();
        return;
    }

    audioMenuTracks_.clear();
    audioMenuTracks_ = AudioTrackMenuItems(snapshot.media);
    subtitleMenuTracks_.clear();
    subtitleMenuTracks_ = SubtitleTrackMenuItems(snapshot.media);
    subtitleMenuVisibleItemCount_ = 0;
    subtitleMenuScrollOffset_ = 0;
    subtitleMenuScrollLastActiveAt_ = {};
    const auto settings = controller_.Settings();
    const auto& selectedTracks = subtitleMenuPage_ == SubtitleMenuPage::Audio ? audioMenuTracks_ : subtitleMenuTracks_;
    const int selectedTrack = subtitleMenuPage_ == SubtitleMenuPage::Audio
                                  ? settings.audio.selectedTrackIndex
                                  : settings.subtitles.selectedTrackIndex;
    const auto selected = std::find(selectedTracks.begin(), selectedTracks.end(), selectedTrack);
    if (selected != selectedTracks.end()) {
        subtitleMenuScrollOffset_ = std::max(0, static_cast<int>(std::distance(selectedTracks.begin(), selected)) - 1);
    }
    hoveredSubtitleMenuItem_ = -1;
    SetSubtitleMenuTarget(true);
    if (fullscreen_) {
        ShowFullscreenTransport(L"subtitle_menu_show");
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

void MainWindow::OpenInspectorPathItem(const int itemIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(inspectorPathItems_.size())) {
        return;
    }

    const std::filesystem::path path = inspectorPathItems_[static_cast<std::size_t>(itemIndex)].path;
    if (path.empty()) {
        return;
    }
    OpenPath(path, true);
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
        ApplyVolume(snapshot.volume - 0.05, true);
        break;
    }
    case Command::VolumeUp: {
        const auto snapshot = controller_.Snapshot();
        ApplyVolume(snapshot.volume + 0.05, true);
        break;
    }
    case Command::SubtitleMenu:
        ShowSubtitleMenu();
        break;
    case Command::Fullscreen:
        ToggleFullscreen();
        break;
    case Command::Settings:
        HideHdrToneCurveWindow();
        if (inspectorCollapsed_) {
            inspectorTab_ = InspectorTab::Settings;
            ToggleSidebar();
            break;
        }
        inspectorTab_ = inspectorTab_ == InspectorTab::Settings ? InspectorTab::Recent : InspectorTab::Settings;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::ToggleSidebar:
        ToggleSidebar();
        break;
    case Command::InspectorRecent:
        HideHdrToneCurveWindow();
        inspectorTab_ = InspectorTab::Recent;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::InspectorFolder:
        HideHdrToneCurveWindow();
        inspectorTab_ = InspectorTab::Folder;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::InspectorMedia:
        HideHdrToneCurveWindow();
        inspectorTab_ = InspectorTab::Media;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::InspectorSystem:
        HideHdrToneCurveWindow();
        inspectorTab_ = InspectorTab::System;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::InspectorLog:
        HideHdrToneCurveWindow();
        inspectorTab_ = InspectorTab::Log;
        MarkLayoutDirty();
        EnsureLayout();
        break;
    case Command::ToggleDolbyVisionHdr:
        ToggleDolbyVisionHdrOutput();
        break;
    case Command::ToggleDolbyVisionCmv4Approx:
        ToggleDolbyVisionCmv4Approx();
        break;
    case Command::ResetHdrToneCurve:
        ResetHdrToneCurve();
        break;
    case Command::ToggleHdrToneCurveExpanded:
        if (hdrToneCurveExpanded_) {
            HideHdrToneCurveWindow();
        } else {
            ShowHdrToneCurveWindow();
        }
        break;
    }
    InvalidateTransportArea();
    InvalidateFullscreenOverlay();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace anvil::app
