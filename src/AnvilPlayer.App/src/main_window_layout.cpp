#include "AnvilPlayer/App/main_window.h"

#include <algorithm>
#include <cmath>

namespace anvil::app {

using anvil::playback::PlaybackState;

void MainWindow::MarkLayoutDirty() {
    layoutDirty_ = true;
}

void MainWindow::EnsureLayout() {
    if (layoutDirty_) {
        UpdateLayout();
    }
}

void MainWindow::UpdateLayout() {
    layoutDirty_ = false;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int clientWidth = RectWidth(client);
    const int clientHeight = RectHeight(client);
    const bool compact = clientWidth < Scale(860) || clientHeight < Scale(560);
    const int margin = compact ? Scale(10) : Scale(16);
    const int gap = compact ? Scale(8) : Scale(12);
    const int topHeight = compact ? Scale(48) : Scale(56);
    const int bottomHeight = compact ? Scale(80) : Scale(88);
    const int topButtonSize = compact ? Scale(30) : Scale(32);
    const int transportButtonSize = compact ? Scale(32) : Scale(36);
    const int playButtonSize = compact ? Scale(40) : Scale(44);
    const int buttonGap = compact ? Scale(8) : Scale(10);
    const auto snapshot = controller_.Snapshot();
    const bool playing = snapshot.state == PlaybackState::Playing;
    const auto settings = controller_.Settings();
    const bool subtitlesEnabled = settings.subtitles.selectedTrackIndex != anvil::playback::kSubtitleTrackOff;
    const bool showSubtitleButton = snapshot.media.has_value();
    const auto rightClusterWidthFor = [transportButtonSize, buttonGap](const int buttonCount) {
        return buttonCount > 0 ? transportButtonSize * buttonCount + buttonGap * (buttonCount - 1) : 0;
    };

    buttons_.clear();

    if (fullscreen_) {
        topBar_ = RECT{};
        inspector_ = RECT{};
        showInspectorTabs_ = false;
        videoSurface_ = client;

        const bool showTransport = ShouldShowFullscreenTransport(snapshot);
        const int fullscreenInset = Scale(18);
        const int fullscreenBottomHeight = compact ? Scale(76) : Scale(84);
        transportBar_ = showTransport
                            ? MakeRect(client.left + fullscreenInset,
                                       client.bottom - fullscreenInset - fullscreenBottomHeight,
                                       client.right - fullscreenInset,
                                       client.bottom - fullscreenInset)
                            : RECT{};
        const int progressInset = compact ? Scale(18) : Scale(24);
        progress_ = showTransport
                        ? MakeRect(transportBar_.left + progressInset,
                                   transportBar_.top + (compact ? Scale(12) : Scale(14)),
                                   transportBar_.right - progressInset,
                                   transportBar_.top + (compact ? Scale(18) : Scale(20)))
                        : RECT{};

        if (showTransport) {
            const int controlCenterY = transportBar_.top + (compact ? Scale(50) : Scale(54));
            const int centerX = (transportBar_.left + transportBar_.right) / 2;
            const int clusterWidth = transportButtonSize * 3 + playButtonSize + buttonGap * 3;
            const int clusterMinLeft = static_cast<int>(transportBar_.left) + Scale(18);
            const int clusterMaxLeft = std::max(clusterMinLeft, static_cast<int>(transportBar_.right) - Scale(18) - clusterWidth);
            int x = std::clamp(centerX - clusterWidth / 2, clusterMinLeft, clusterMaxLeft);
            transportControlsLeft_ = x;
            const int smallTop = controlCenterY - transportButtonSize / 2;
            const int playTop = controlCenterY - playButtonSize / 2;
            buttons_.push_back(UiButton{Command::Back,
                                        MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Back 10 seconds",
                                        IconKind::Back10,
                                        ButtonKind::Icon,
                                        false,
                                        false});
            x += transportButtonSize + buttonGap;
            buttons_.push_back(UiButton{Command::PlayPause,
                                        MakeRect(x, playTop, x + playButtonSize, playTop + playButtonSize),
                                        L"",
                                        playing ? L"Pause" : L"Play",
                                        playing ? IconKind::Pause : IconKind::Play,
                                        ButtonKind::TransportPrimary,
                                        true,
                                        false});
            x += playButtonSize + buttonGap;
            buttons_.push_back(UiButton{Command::Stop,
                                        MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Stop",
                                        IconKind::Stop,
                                        ButtonKind::Icon,
                                        false,
                                        false});
            x += transportButtonSize + buttonGap;
            buttons_.push_back(UiButton{Command::Forward,
                                        MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Forward 10 seconds",
                                        IconKind::Forward10,
                                        ButtonKind::Icon,
                                        false,
                                        false});
            x += transportButtonSize;
            transportControlsRight_ = x;

            bool showFullscreenSubtitleButton = showSubtitleButton;
            int rightButtonCount = showFullscreenSubtitleButton ? 2 : 1;
            int rightX = transportBar_.right - Scale(22) - rightClusterWidthFor(rightButtonCount);
            if (rightX < transportControlsRight_ + Scale(12)) {
                showFullscreenSubtitleButton = false;
                rightButtonCount = 1;
                rightX = transportBar_.right - Scale(22) - rightClusterWidthFor(rightButtonCount);
            }
            transportRightControlsLeft_ = rightX;
            if (showFullscreenSubtitleButton) {
                buttons_.push_back(UiButton{Command::SubtitleMenu,
                                            MakeRect(rightX, smallTop, rightX + transportButtonSize, smallTop + transportButtonSize),
                                            L"",
                                            L"Subtitles",
                                            IconKind::Subtitles,
                                            ButtonKind::Icon,
                                            false,
                                            subtitlesEnabled});
                rightX += transportButtonSize + buttonGap;
            }
            buttons_.push_back(UiButton{Command::Fullscreen,
                                        MakeRect(rightX, smallTop, rightX + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Exit fullscreen",
                                        IconKind::Windowed,
                                        ButtonKind::Icon,
                                        false,
                                        false});
        }

        playbackPlayer_.SetBounds(PlaybackSurfaceBounds());
        UpdateVideoHost();
        UpdateTransportOverlay();
        UpdateFullscreenOverlay();
        return;
    }

    topBar_ = MakeRect(margin, margin, client.right - margin, margin + topHeight);
    transportBar_ = MakeRect(margin, client.bottom - margin - bottomHeight, client.right - margin, client.bottom - margin);
    const RECT content = MakeRect(margin, topBar_.bottom + gap, client.right - margin, transportBar_.top - gap);
    const bool sideInspector = RectWidth(content) >= Scale(820) && RectHeight(content) >= Scale(330);
    const bool settingsOverlay = !sideInspector && inspectorTab_ == InspectorTab::Settings;
    showInspectorTabs_ = false;

    if (sideInspector) {
        const int expandedInspectorWidth = std::clamp(clientWidth / 4, compact ? Scale(246) : Scale(286), Scale(324));
        const double collapse = std::clamp(inspectorCollapseAmount_, 0.0, 1.0);
        const int inspectorWidth = static_cast<int>(std::round(expandedInspectorWidth * (1.0 - collapse)));
        const int animatedGap = static_cast<int>(std::round(gap * (1.0 - collapse)));
        if (inspectorWidth > 0) {
            inspector_ = MakeRect(content.right - inspectorWidth, content.top, content.right, content.bottom);
            videoSurface_ = MakeRect(content.left, content.top, inspector_.left - animatedGap, content.bottom);
        } else {
            inspector_ = RECT{};
            videoSurface_ = content;
        }
        showInspectorTabs_ = inspectorWidth >= Scale(240) && collapse < 0.55;
    } else if (settingsOverlay) {
        inspector_ = content;
        videoSurface_ = RECT{};
    } else {
        inspector_ = RECT{};
        videoSurface_ = content;
    }
    const int progressInset = compact ? Scale(18) : Scale(24);
    progress_ = MakeRect(transportBar_.left + progressInset,
                         transportBar_.top + (compact ? Scale(12) : Scale(14)),
                         transportBar_.right - progressInset,
                         transportBar_.top + (compact ? Scale(18) : Scale(20)));
    const int topButtonY = topBar_.top + (RectHeight(topBar_) - topButtonSize) / 2;
    int topButtonX = topBar_.right - (compact ? Scale(10) : Scale(12)) - topButtonSize;
    buttons_.push_back(UiButton{Command::Settings,
                                MakeRect(topButtonX, topButtonY, topButtonX + topButtonSize, topButtonY + topButtonSize),
                                L"",
                                inspectorTab_ == InspectorTab::Settings ? L"Back to media" : L"Settings",
                                IconKind::Cog,
                                ButtonKind::Icon,
                                false,
                                inspectorTab_ == InspectorTab::Settings});
    topButtonX -= topButtonSize + (compact ? Scale(6) : Scale(8));
    buttons_.push_back(UiButton{Command::Open,
                                MakeRect(topButtonX, topButtonY, topButtonX + topButtonSize, topButtonY + topButtonSize),
                                L"",
                                L"Open media",
                                IconKind::Folder,
                                ButtonKind::Icon,
                                false,
                                false});
    if (sideInspector) {
        topButtonX -= topButtonSize + (compact ? Scale(6) : Scale(8));
        buttons_.push_back(UiButton{Command::ToggleSidebar,
                                    MakeRect(topButtonX, topButtonY, topButtonX + topButtonSize, topButtonY + topButtonSize),
                                    L"",
                                    inspectorCollapsed_ ? L"Show sidebar" : L"Hide sidebar",
                                    inspectorCollapsed_ ? IconKind::ChevronLeft : IconKind::ChevronRight,
                                    ButtonKind::Icon,
                                    false,
                                    false});
    }
    topControlsLeft_ = topButtonX;
    showTopBrand_ = RectWidth(topBar_) >= Scale(420);
    showTopState_ = RectWidth(topBar_) >= Scale(680);

    if (showInspectorTabs_) {
        const RECT inner = DeflateRectCopy(inspector_, Scale(16), Scale(16));
        const int tabTop = inner.top + Scale(38);
        const int tabHeight = Scale(30);
        const int tabGap = Scale(4);
        const int tabWidth = (RectWidth(inner) - tabGap * 2) / 3;
        int tabX = inner.left;
        buttons_.push_back(UiButton{Command::InspectorMedia,
                                    MakeRect(tabX, tabTop, tabX + tabWidth, tabTop + tabHeight),
                                    L"Media",
                                    L"",
                                    IconKind::None,
                                    ButtonKind::Tab,
                                    false,
                                    inspectorTab_ == InspectorTab::Media});
        tabX += tabWidth + tabGap;
        buttons_.push_back(UiButton{Command::InspectorDevice,
                                    MakeRect(tabX, tabTop, tabX + tabWidth, tabTop + tabHeight),
                                    L"Device",
                                    L"",
                                    IconKind::None,
                                    ButtonKind::Tab,
                                    false,
                                    inspectorTab_ == InspectorTab::Device});
        tabX += tabWidth + tabGap;
        buttons_.push_back(UiButton{Command::InspectorLog,
                                    MakeRect(tabX, tabTop, inner.right, tabTop + tabHeight),
                                    L"Log",
                                    L"",
                                    IconKind::None,
                                    ButtonKind::Tab,
                                    false,
                                    inspectorTab_ == InspectorTab::Log});
    }

    const int controlCenterY = transportBar_.top + (compact ? Scale(54) : Scale(58));
    const int centerX = (transportBar_.left + transportBar_.right) / 2;
    const int clusterWidth = transportButtonSize * 3 + playButtonSize + buttonGap * 3;
    const int clusterMinLeft = static_cast<int>(transportBar_.left) + Scale(18);
    const int clusterMaxLeft = static_cast<int>(transportBar_.right) - Scale(18) - clusterWidth;
    int x = clusterMaxLeft >= clusterMinLeft
                ? std::clamp(centerX - clusterWidth / 2, clusterMinLeft, clusterMaxLeft)
                : clusterMinLeft;
    transportControlsLeft_ = x;
    const int smallTop = controlCenterY - transportButtonSize / 2;
    const int playTop = controlCenterY - playButtonSize / 2;
    buttons_.push_back(UiButton{Command::Back,
                                MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                L"",
                                L"Back 10 seconds",
                                IconKind::Back10,
                                ButtonKind::Icon,
                                false,
                                false});
    x += transportButtonSize + buttonGap;
    buttons_.push_back(UiButton{Command::PlayPause,
                                MakeRect(x, playTop, x + playButtonSize, playTop + playButtonSize),
                                L"",
                                playing ? L"Pause" : L"Play",
                                playing ? IconKind::Pause : IconKind::Play,
                                ButtonKind::TransportPrimary,
                                true,
                                false});
    x += playButtonSize + buttonGap;
    buttons_.push_back(UiButton{Command::Stop,
                                MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                L"",
                                L"Stop",
                                IconKind::Stop,
                                ButtonKind::Icon,
                                false,
                                false});
    x += transportButtonSize + buttonGap;
    buttons_.push_back(UiButton{Command::Forward,
                                MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                L"",
                                L"Forward 10 seconds",
                                IconKind::Forward10,
                                ButtonKind::Icon,
                                false,
                                false});
    x += transportButtonSize;
    transportControlsRight_ = x;

    const int rightTop = smallTop;
    const int rightEdge = transportBar_.right - Scale(22);
    const int essentialRightButtonCount = showSubtitleButton ? 2 : 1;
    bool showVolumeButtons = rightEdge - rightClusterWidthFor(essentialRightButtonCount + 2) >= transportControlsRight_ + Scale(16);
    bool showTransportSubtitleButton = showSubtitleButton;
    int rightButtonCount = essentialRightButtonCount + (showVolumeButtons ? 2 : 0);
    if (rightEdge - rightClusterWidthFor(rightButtonCount) < transportControlsRight_ + Scale(12)) {
        showVolumeButtons = false;
        showTransportSubtitleButton = showSubtitleButton &&
                                      rightEdge - rightClusterWidthFor(2) >= transportControlsRight_ + Scale(12);
        rightButtonCount = showTransportSubtitleButton ? 2 : 1;
    }
    int rightX = rightEdge - rightClusterWidthFor(rightButtonCount);
    transportRightControlsLeft_ = rightX;

    if (showVolumeButtons) {
        buttons_.push_back(UiButton{Command::VolumeDown,
                                    MakeRect(rightX, rightTop, rightX + transportButtonSize, rightTop + transportButtonSize),
                                    L"",
                                    L"Volume down",
                                    IconKind::VolumeDown,
                                    ButtonKind::Icon,
                                    false,
                                    false});
        rightX += transportButtonSize + buttonGap;
        buttons_.push_back(UiButton{Command::VolumeUp,
                                    MakeRect(rightX, rightTop, rightX + transportButtonSize, rightTop + transportButtonSize),
                                    L"",
                                    L"Volume up",
                                    IconKind::VolumeUp,
                                    ButtonKind::Icon,
                                    false,
                                    false});
        rightX += transportButtonSize + buttonGap;
    }
    if (showTransportSubtitleButton) {
        buttons_.push_back(UiButton{Command::SubtitleMenu,
                                    MakeRect(rightX, rightTop, rightX + transportButtonSize, rightTop + transportButtonSize),
                                    L"",
                                    L"Subtitles",
                                    IconKind::Subtitles,
                                    ButtonKind::Icon,
                                    false,
                                    subtitlesEnabled});
        rightX += transportButtonSize + buttonGap;
    }
    buttons_.push_back(UiButton{Command::Fullscreen,
                                MakeRect(rightX, rightTop, rightX + transportButtonSize, rightTop + transportButtonSize),
                                L"",
                                fullscreen_ ? L"Exit fullscreen" : L"Fullscreen",
                                fullscreen_ ? IconKind::Windowed : IconKind::Fullscreen,
                                ButtonKind::Icon,
                                false,
                                false});
    playbackPlayer_.SetBounds(PlaybackSurfaceBounds());
    UpdateVideoHost();
    UpdateTransportOverlay();
    UpdateFullscreenOverlay();
}

void MainWindow::UpdateVideoHost() {
    if (!videoHostReady_ || !videoHost_) {
        return;
    }
    const RECT bounds = PlaybackSurfaceBounds();
    const int w = RectWidth(bounds);
    const int h = RectHeight(bounds);
    const auto snapshot = controller_.Snapshot();
    const bool nativeActive = nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning();
    const bool nativePausedFrame = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                                   snapshot.state == PlaybackState::Paused &&
                                   snapshot.media.has_value() &&
                                   snapshot.media->hasVideo;
    const bool nativeHeldFrame = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                                 nativeFrameHoldVisible_ &&
                                 heldNativeFrame_.has_value();
    if ((nativeActive || nativePausedFrame || nativeHeldFrame) && w > 0 && h > 0) {
        const bool wasVisible = IsWindowVisible(videoHost_) != FALSE;
        const bool boundsChanged = bounds.left != lastVideoHostBounds_.left ||
                                   bounds.top != lastVideoHostBounds_.top ||
                                   w != RectWidth(lastVideoHostBounds_) ||
                                   h != RectHeight(lastVideoHostBounds_);
        bool resized = false;
        if (boundsChanged || !wasVisible) {
            SetWindowPos(videoHost_, HWND_BOTTOM, bounds.left, bounds.top, w, h, SWP_NOACTIVATE);
            if (boundsChanged) {
                lastVideoHostBounds_ = bounds;
            }
            if (boundsChanged && d3dRenderer_) {
                d3dRenderer_->OnResize();
                resized = true;
            }
        }
        ShowWindow(videoHost_, SW_SHOW);
        if (!nativeActive &&
            heldNativeFrame_.has_value() &&
            (resized || !wasVisible || heldNativeFrameNeedsPresent_)) {
            RenderHeldNativeFrame();
        }
    } else {
        ShowWindow(videoHost_, SW_HIDE);
    }
}

void MainWindow::EnsureFullscreenOverlay() {
    if (fullscreenOverlay_) {
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &MainWindow::FullscreenOverlayProc;
    wc.lpszClassName = kFullscreenOverlayClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    fullscreenOverlay_ = CreateWindowExW(
        0,
        kFullscreenOverlayClassName,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        hwnd_,
        nullptr,
        instance_,
        this);
}

void MainWindow::EnsureTransportOverlay() {
    if (transportOverlay_) {
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &MainWindow::TransportOverlayProc;
    wc.lpszClassName = kTransportOverlayClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    transportOverlay_ = CreateWindowExW(
        0,
        kTransportOverlayClassName,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        hwnd_,
        nullptr,
        instance_,
        this);
}

void MainWindow::UpdateTransportOverlay() {
    const bool showOverlay = !fullscreen_ && RectWidth(transportBar_) > 0 && RectHeight(transportBar_) > 0;
    if (!showOverlay) {
        if (transportOverlay_) {
            ShowWindow(transportOverlay_, SW_HIDE);
        }
        lastTransportOverlayBounds_ = RECT{};
        return;
    }

    EnsureTransportOverlay();
    if (!transportOverlay_) {
        return;
    }

    const int w = RectWidth(transportBar_);
    const int h = RectHeight(transportBar_);
    const bool wasVisible = IsWindowVisible(transportOverlay_) != FALSE;
    const bool moved = transportBar_.left != lastTransportOverlayBounds_.left ||
                       transportBar_.top != lastTransportOverlayBounds_.top ||
                       w != RectWidth(lastTransportOverlayBounds_) ||
                       h != RectHeight(lastTransportOverlayBounds_);
    if (moved || !wasVisible) {
        SetWindowPos(transportOverlay_,
                     HWND_TOP,
                     transportBar_.left,
                     transportBar_.top,
                     w,
                     h,
                     SWP_NOACTIVATE);
    } else {
        SetWindowPos(transportOverlay_,
                     HWND_TOP,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    }

    if (moved) {
        HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, Scale(12), Scale(12));
        SetWindowRgn(transportOverlay_, region, TRUE);
        lastTransportOverlayBounds_ = transportBar_;
    }
    if (!wasVisible) {
        ShowWindow(transportOverlay_, SW_SHOWNOACTIVATE);
    }
    if (moved || !wasVisible) {
        RedrawWindow(transportOverlay_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
}

void MainWindow::UpdateFullscreenOverlay() {
    const bool showOverlay = fullscreen_ && RectWidth(transportBar_) > 0 && RectHeight(transportBar_) > 0;
    if (!showOverlay) {
        if (fullscreenOverlay_) {
            ShowWindow(fullscreenOverlay_, SW_HIDE);
        }
        lastFullscreenOverlayBounds_ = RECT{};
        return;
    }

    EnsureFullscreenOverlay();
    if (!fullscreenOverlay_) {
        return;
    }

    const int w = RectWidth(transportBar_);
    const int h = RectHeight(transportBar_);
    const bool wasVisible = IsWindowVisible(fullscreenOverlay_) != FALSE;
    const bool moved = transportBar_.left != lastFullscreenOverlayBounds_.left ||
                       transportBar_.top != lastFullscreenOverlayBounds_.top ||
                       w != RectWidth(lastFullscreenOverlayBounds_) ||
                       h != RectHeight(lastFullscreenOverlayBounds_);
    if (moved || !wasVisible) {
        SetWindowPos(fullscreenOverlay_,
                     HWND_TOP,
                     transportBar_.left,
                     transportBar_.top,
                     w,
                     h,
                     SWP_NOACTIVATE);
    } else {
        SetWindowPos(fullscreenOverlay_,
                     HWND_TOP,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    }

    if (moved) {
        HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, Scale(12), Scale(12));
        SetWindowRgn(fullscreenOverlay_, region, TRUE);
        lastFullscreenOverlayBounds_ = transportBar_;
    }
    if (!wasVisible) {
        ShowWindow(fullscreenOverlay_, SW_SHOWNOACTIVATE);
    }
    if (moved || !wasVisible) {
        RedrawWindow(fullscreenOverlay_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
}

int MainWindow::HitButton(const POINT point) const {
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        if (ContainsPoint(buttons_[index].bounds, point)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

}  // namespace anvil::app
