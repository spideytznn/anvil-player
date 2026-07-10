#include "AnvilPlayer/App/main_window.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace anvil::app {

using anvil::playback::PlaybackState;
using anvil::playback::PlaybackSessionSnapshot;

namespace {

bool RectEquals(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

int LerpInt(const int from, const int to, const double amount) {
    return from + static_cast<int>(std::round((to - from) * amount));
}

RECT LerpRect(const RECT& from, const RECT& to, const double amount) {
    const double clamped = std::clamp(amount, 0.0, 1.0);
    return MakeRect(LerpInt(from.left, to.left, clamped),
                    LerpInt(from.top, to.top, clamped),
                    LerpInt(from.right, to.right, clamped),
                    LerpInt(from.bottom, to.bottom, clamped));
}

struct AccentPolicy {
    int accentState = 0;
    int accentFlags = 0;
    DWORD gradientColor = 0;
    int animationId = 0;
};

struct WindowCompositionAttribData {
    DWORD attribute = 0;
    PVOID data = nullptr;
    SIZE_T sizeOfData = 0;
};

bool EnableBackdropBlur(HWND hwnd) {
    constexpr DWORD kWcaAccentPolicy = 19;
    constexpr int kAccentEnableBlurBehind = 3;
    constexpr int kAccentEnableAcrylicBlurBehind = 4;
    constexpr DWORD kDarkFrostedTint = 0x52000000;

    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttribData*);
        auto* setWindowCompositionAttribute =
            reinterpret_cast<SetWindowCompositionAttributeFn>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
        if (setWindowCompositionAttribute) {
            AccentPolicy policy{};
            policy.accentState = kAccentEnableAcrylicBlurBehind;
            policy.accentFlags = 2;
            policy.gradientColor = kDarkFrostedTint;
            WindowCompositionAttribData data{};
            data.attribute = kWcaAccentPolicy;
            data.data = &policy;
            data.sizeOfData = sizeof(policy);
            if (setWindowCompositionAttribute(hwnd, &data)) {
                return true;
            }

            policy.accentState = kAccentEnableBlurBehind;
            policy.accentFlags = 0;
            policy.gradientColor = kDarkFrostedTint;
            if (setWindowCompositionAttribute(hwnd, &data)) {
                return true;
            }
        }
    }

    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = TRUE;
    return SUCCEEDED(DwmEnableBlurBehindWindow(hwnd, &blur));
}

}  // namespace

void MainWindow::MarkLayoutDirty() {
    layoutDirty_ = true;
}

void MainWindow::EnsureLayout() {
    if (layoutDirty_) {
        UpdateLayout();
    }
}

RECT MainWindow::SettingsContentViewport() const {
    if (RectWidth(inspector_) <= 0 || RectHeight(inspector_) <= 0 || inspectorTab_ != InspectorTab::Settings) {
        return RECT{};
    }

    RECT inner = DeflateRectCopy(inspector_, Scale(16), Scale(16));
    RECT viewport = inner;
    if (showInspectorTabs_ && inspectorTab_ != InspectorTab::Settings) {
        viewport.top = inner.top + Scale(70) + Scale(14);
    } else {
        viewport.top = inner.top + Scale(22) + Scale(16);
    }
    if (RectWidth(viewport) > Scale(180)) {
        viewport.right -= Scale(10);
    }
    return viewport;
}

int MainWindow::SettingsContentHeight(const anvil::playback::PlayerSettings& settings, const RECT viewport) const {
    if (RectWidth(viewport) <= 0) {
        return 0;
    }

    const int fieldHeight = RectWidth(viewport) < Scale(250) ? Scale(40) : Scale(28);
    int height = 0;
    height += Scale(24);                         // Video header
    height += fieldHeight * SettingsVideoFieldCount();
    if (HdrToneCurveAvailable(settings)) {
        height += Scale(8) + Scale(190) + Scale(12);
    }
    height += Scale(8) + Scale(24) + fieldHeight * 3;  // Audio
    height += Scale(8) + Scale(24) + fieldHeight * 4;  // Subtitles
    return height;
}

void MainWindow::UpdateSettingsScrollLayout(const anvil::playback::PlayerSettings& settings) {
    settingsContentViewport_ = SettingsContentViewport();
    settingsScrollTrack_ = RECT{};
    settingsScrollThumb_ = RECT{};
    settingsContentHeight_ = SettingsContentHeight(settings, settingsContentViewport_);
    settingsScrollMax_ = std::max(0, settingsContentHeight_ - RectHeight(settingsContentViewport_));
    settingsScrollOffset_ = std::clamp(settingsScrollOffset_, 0, settingsScrollMax_);

    if (settingsScrollMax_ <= 0 || RectHeight(settingsContentViewport_) <= Scale(40)) {
        settingsScrollOffset_ = 0;
        return;
    }

    RECT inner = DeflateRectCopy(inspector_, Scale(16), Scale(16));
    settingsScrollTrack_ = MakeRect(settingsContentViewport_.right + Scale(4),
                                    settingsContentViewport_.top,
                                    inner.right - Scale(2),
                                    settingsContentViewport_.bottom);
    if (RectWidth(settingsScrollTrack_) <= 0 || RectHeight(settingsScrollTrack_) <= 0) {
        settingsScrollTrack_ = RECT{};
        return;
    }

    const int trackHeight = RectHeight(settingsScrollTrack_);
    const int thumbHeight = std::clamp(
        static_cast<int>(std::round(static_cast<double>(trackHeight) *
                                    static_cast<double>(RectHeight(settingsContentViewport_)) /
                                    static_cast<double>(std::max(settingsContentHeight_, 1)))),
        Scale(28),
        trackHeight);
    const int travel = std::max(0, trackHeight - thumbHeight);
    const int thumbTop = settingsScrollTrack_.top +
                         (settingsScrollMax_ > 0
                              ? static_cast<int>(std::round(static_cast<double>(travel) *
                                                            static_cast<double>(settingsScrollOffset_) /
                                                            static_cast<double>(settingsScrollMax_)))
                              : 0);
    settingsScrollThumb_ = MakeRect(settingsScrollTrack_.left,
                                    thumbTop,
                                    settingsScrollTrack_.right,
                                    thumbTop + thumbHeight);
}

void MainWindow::UpdateInspectorPathItems(const PlaybackSessionSnapshot& snapshot) {
    inspectorPathItems_.clear();
    if (!showInspectorTabs_ ||
        inspectorTab_ == InspectorTab::Settings ||
        RectWidth(inspector_) <= 0 ||
        RectHeight(inspector_) <= 0) {
        hoveredInspectorPathItem_ = -1;
        return;
    }

    RECT inner = DeflateRectCopy(inspector_, Scale(16), Scale(16));
    RECT cursor = inner;
    cursor.top = inner.top + Scale(68) + Scale(14);

    const auto advanceSectionHeader = [this](RECT& target) {
        target.top += Scale(24);
    };
    const auto advanceField = [this](RECT& target) {
        const bool stacked = RectWidth(target) < Scale(250);
        const int requiredHeight = stacked ? Scale(38) : Scale(24);
        if (target.top + requiredHeight <= target.bottom) {
            target.top += stacked ? Scale(40) : Scale(28);
        }
    };
    const auto appendRows = [this](const std::vector<std::filesystem::path>& paths, RECT& target) {
        const int rowHeight = Scale(44);
        const int rowGap = Scale(5);
        for (const auto& path : paths) {
            if (target.top + rowHeight > target.bottom) {
                break;
            }
            const RECT row = MakeRect(target.left, target.top, target.right, target.top + rowHeight);
            inspectorPathItems_.push_back(InspectorPathItem{row, path});
            target.top += rowHeight + rowGap;
        }
    };

    switch (inspectorTab_) {
    case InspectorTab::Recent:
        advanceSectionHeader(cursor);
        appendRows(recentMedia_, cursor);
        break;
    case InspectorTab::Folder:
        advanceSectionHeader(cursor);
        if (snapshot.media.has_value()) {
            advanceField(cursor);
            cursor.top += Scale(8);
            advanceSectionHeader(cursor);
            appendRows(currentFolderEntries_, cursor);
        }
        break;
    default:
        break;
    }

    if (hoveredInspectorPathItem_ >= static_cast<int>(inspectorPathItems_.size())) {
        hoveredInspectorPathItem_ = -1;
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
    const int bottomHeight = compact ? Scale(88) : Scale(96);
    const int topButtonSize = compact ? Scale(30) : Scale(32);
    const int transportButtonSize = compact ? Scale(30) : Scale(32);
    const int playButtonSize = compact ? Scale(38) : Scale(42);
    const int buttonGap = compact ? Scale(8) : Scale(12);
    const int rightButtonSize = compact ? Scale(28) : Scale(30);
    const int rightButtonGap = compact ? Scale(8) : Scale(12);
    const int volumeSliderWidth = compact ? Scale(158) : Scale(202);
    const auto snapshot = controller_.Snapshot();
    const bool playing = snapshot.state == PlaybackState::Playing;
    const auto settings = controller_.Settings();
    const bool subtitlesEnabled = snapshot.media.has_value() &&
                                  settings.subtitles.selectedTrackIndex != anvil::playback::kSubtitleTrackOff;
    // Keep the subtitle menu reachable even before embedded subtitle streams
    // are discovered; the menu also hosts external-subtitle actions.
    const bool showSubtitleButton = true;
    if (hdrToneCurveExpanded_ &&
        (inspectorTab_ != InspectorTab::Settings || !HdrToneCurveAvailable(settings))) {
        hdrToneCurveExpanded_ = false;
        if (hdrToneCurveWindow_) {
            ShowWindow(hdrToneCurveWindow_, SW_HIDE);
        }
    }
    const auto rightClusterWidthFor = [rightButtonGap](std::initializer_list<int> widths) {
        int total = 0;
        int count = 0;
        for (const int width : widths) {
            if (width <= 0) {
                continue;
            }
            total += width;
            ++count;
        }
        return count > 0 ? total + rightButtonGap * (count - 1) : 0;
    };

    buttons_.clear();
    inspectorPathItems_.clear();
    volumeSlider_ = RECT{};
    hdrToneCurvePlot_ = RECT{};
    hdrToneCurveExpandedEditor_ = RECT{};
    settingsContentViewport_ = RECT{};
    settingsScrollTrack_ = RECT{};
    settingsScrollThumb_ = RECT{};
    settingsContentHeight_ = 0;
    settingsScrollMax_ = 0;
    const bool showHdrButton = CurrentMediaHasHdrControls();
    const bool showCmv4Button = CurrentMediaHasCmv4Control();
    const bool cmv4ButtonEnabled = CurrentCmv4ControlEnabled(settings);
    const bool dolbyVisionButton = showCmv4Button;
    const int hdrButtonWidth = showHdrButton ? (dolbyVisionButton ? Scale(104) : Scale(48)) : 0;
    const int cmv4ButtonWidth = showCmv4Button ? Scale(86) : 0;
    const std::wstring hdrOutputTooltip =
        dolbyVisionButton
            ? (settings.video.dolbyVisionHdrOutput ? L"Disable Dolby Vision HDR output"
                                                    : L"Enable Dolby Vision HDR output")
            : (settings.video.dolbyVisionHdrOutput ? L"Disable HDR output" : L"Enable HDR output");
    const std::wstring cmv4Tooltip = settings.video.dolbyVisionCmv4Approx
                                         ? L"Disable Dolby Vision Enhanced"
                                         : L"Enable Dolby Vision Enhanced";
    const auto addDolbyVisionHdrButton = [&](const int left, const int top) {
        buttons_.push_back(UiButton{Command::ToggleDolbyVisionHdr,
                                    MakeRect(left, top, left + hdrButtonWidth, top + rightButtonSize),
                                    dolbyVisionButton ? L"Dolby Vision" : L"HDR",
                                    hdrOutputTooltip,
                                    IconKind::None,
                                    ButtonKind::TransportLabel,
                                    false,
                                    settings.video.dolbyVisionHdrOutput});
    };
    const auto addDolbyVisionCmv4Button = [&](const int left, const int top) {
        buttons_.push_back(UiButton{Command::ToggleDolbyVisionCmv4Approx,
                                    MakeRect(left, top, left + cmv4ButtonWidth, top + rightButtonSize),
                                    L"Enhanced",
                                    cmv4Tooltip,
                                    IconKind::None,
                                    ButtonKind::TransportLabel,
                                    false,
                                    settings.video.dolbyVisionCmv4Approx && cmv4ButtonEnabled,
                                    cmv4ButtonEnabled});
    };

    if (fullscreen_) {
        topBar_ = RECT{};
        inspector_ = RECT{};
        showInspectorTabs_ = false;
        videoSurface_ = client;
        playbackSurface_ = videoSurface_;

        const bool showTransport = ShouldShowFullscreenTransport(snapshot);
        const int fullscreenInset = Scale(18);
        const int fullscreenBottomHeight = compact ? Scale(112) : Scale(128);
        transportBar_ = showTransport
                            ? MakeRect(client.left + fullscreenInset,
                                       client.bottom - fullscreenInset - fullscreenBottomHeight,
                                       client.right - fullscreenInset,
                                       client.bottom - fullscreenInset)
                            : RECT{};
        const int progressInset = compact ? Scale(18) : Scale(24);
        progress_ = showTransport
                        ? MakeRect(transportBar_.left + progressInset,
                                   transportBar_.top + (compact ? Scale(36) : Scale(42)),
                                   transportBar_.right - progressInset,
                                   transportBar_.top + (compact ? Scale(42) : Scale(48)))
                        : RECT{};

        if (showTransport) {
            const int controlCenterY = transportBar_.bottom - (compact ? Scale(36) : Scale(42));
            const int controlLeft = static_cast<int>(transportBar_.left) + (compact ? Scale(18) : Scale(24));
            int x = controlLeft;
            transportControlsLeft_ = x;
            const int smallTop = controlCenterY - transportButtonSize / 2;
            const int playTop = controlCenterY - playButtonSize / 2;
            buttons_.push_back(UiButton{Command::Back,
                                        MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Back 10 seconds",
                                        IconKind::Back10,
                                        ButtonKind::TransportIcon,
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
            buttons_.push_back(UiButton{Command::Forward,
                                        MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Forward 10 seconds",
                                        IconKind::Forward10,
                                        ButtonKind::TransportIcon,
                                        false,
                                        false});
            x += transportButtonSize + buttonGap;
            buttons_.push_back(UiButton{Command::Stop,
                                        MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                        L"",
                                        L"Stop",
                                        IconKind::Stop,
                                        ButtonKind::TransportIcon,
                                        false,
                                        false});
            x += transportButtonSize + buttonGap;

            const int rightTop = controlCenterY - rightButtonSize / 2;
            bool showFullscreenVolumeSlider = true;
            bool showFullscreenSubtitleButton = showSubtitleButton;
            bool showFullscreenHdrButton = showHdrButton;
            bool showFullscreenCmv4Button = showCmv4Button;
            const auto fullscreenRightClusterWidth = [&]() {
                return rightClusterWidthFor({showFullscreenHdrButton ? hdrButtonWidth : 0,
                                             showFullscreenCmv4Button ? cmv4ButtonWidth : 0,
                                             showFullscreenSubtitleButton ? rightButtonSize : 0,
                                             rightButtonSize});
            };
            int rightX = transportBar_.right - Scale(22) - fullscreenRightClusterWidth();
            if (rightX < x + Scale(12) && showFullscreenSubtitleButton) {
                showFullscreenSubtitleButton = false;
                rightX = transportBar_.right - Scale(22) - fullscreenRightClusterWidth();
            }
            if (rightX < x + Scale(12) && showFullscreenCmv4Button) {
                showFullscreenCmv4Button = false;
                rightX = transportBar_.right - Scale(22) - fullscreenRightClusterWidth();
            }
            if (rightX < x + Scale(12) && showFullscreenHdrButton) {
                showFullscreenHdrButton = false;
                rightX = transportBar_.right - Scale(22) - fullscreenRightClusterWidth();
            }
            if (showFullscreenVolumeSlider) {
                volumeSlider_ = MakeRect(x, rightTop, x + volumeSliderWidth, rightTop + rightButtonSize);
                if (volumeSlider_.right > rightX - Scale(22)) {
                    volumeSlider_ = RECT{};
                    showFullscreenVolumeSlider = false;
                } else {
                    x += volumeSliderWidth + buttonGap;
                }
            }
            transportControlsRight_ = x;
            transportRightControlsLeft_ = rightX;
            if (showFullscreenHdrButton) {
                addDolbyVisionHdrButton(rightX, rightTop);
                rightX += hdrButtonWidth + rightButtonGap;
            }
            if (showFullscreenCmv4Button) {
                addDolbyVisionCmv4Button(rightX, rightTop);
                rightX += cmv4ButtonWidth + rightButtonGap;
            }
            if (showFullscreenSubtitleButton) {
                buttons_.push_back(UiButton{Command::SubtitleMenu,
                                            MakeRect(rightX, rightTop, rightX + rightButtonSize, rightTop + rightButtonSize),
                                            L"",
                                            L"Subtitles",
                                            IconKind::Subtitles,
                                            ButtonKind::TransportIcon,
                                            false,
                                            subtitlesEnabled});
                rightX += rightButtonSize + rightButtonGap;
            }
            buttons_.push_back(UiButton{Command::Fullscreen,
                                        MakeRect(rightX, rightTop, rightX + rightButtonSize, rightTop + rightButtonSize),
                                        L"",
                                        L"Exit fullscreen",
                                        IconKind::Windowed,
                                        ButtonKind::TransportIcon,
                                        false,
                                        false});
        }

        UpdateSubtitleMenuLayout();
        if (!SidebarAnimationActive()) {
            playbackPlayer_.SetBounds(webUiActive_ && !webUiPlayerRouteActive_ ? RECT{} : PlaybackSurfaceBounds());
        }
        UpdateVideoHost();
        UpdateTransportOverlay();
        UpdateFullscreenOverlay();
        UpdateSubtitleMenuOverlay();
        return;
    }

    topBar_ = MakeRect(margin, margin, client.right - margin, margin + topHeight);
    transportBar_ = MakeRect(margin, client.bottom - margin - bottomHeight, client.right - margin, client.bottom - margin);
    const RECT content = MakeRect(margin, topBar_.bottom + gap, client.right - margin, transportBar_.top - gap);
    const bool sideInspector = RectWidth(content) >= Scale(820) && RectHeight(content) >= Scale(330);
    const bool settingsOverlay = !sideInspector && inspectorTab_ == InspectorTab::Settings;
    showInspectorTabs_ = false;

    if (sideInspector) {
        const int expandedInspectorWidth = std::clamp(clientWidth / 4, compact ? Scale(286) : Scale(304), Scale(360));
        const double animatedCollapse = std::clamp(inspectorCollapseAmount_, 0.0, 1.0);
        const double layoutCollapse = std::clamp(inspectorCollapseTarget_, 0.0, 1.0);
        const int animatedInspectorWidth =
            static_cast<int>(std::round(expandedInspectorWidth * (1.0 - animatedCollapse)));
        const int animatedGap = static_cast<int>(std::round(gap * (1.0 - animatedCollapse)));
        const int layoutInspectorWidth =
            static_cast<int>(std::round(expandedInspectorWidth * (1.0 - layoutCollapse)));
        const int layoutGap = static_cast<int>(std::round(gap * (1.0 - layoutCollapse)));
        if (animatedInspectorWidth > 0) {
            inspector_ = MakeRect(content.right - animatedInspectorWidth, content.top, content.right, content.bottom);
            videoSurface_ = MakeRect(content.left, content.top, inspector_.left - animatedGap, content.bottom);
        } else {
            inspector_ = RECT{};
            videoSurface_ = content;
        }
        if (layoutInspectorWidth > 0) {
            playbackSurface_ = MakeRect(content.left, content.top, content.right - layoutInspectorWidth - layoutGap, content.bottom);
        } else {
            playbackSurface_ = content;
        }
        showInspectorTabs_ = !SidebarAnimationActive() && animatedInspectorWidth >= Scale(240) && animatedCollapse < 0.55;
    } else if (settingsOverlay) {
        inspector_ = content;
        videoSurface_ = RECT{};
        playbackSurface_ = videoSurface_;
    } else {
        inspector_ = RECT{};
        videoSurface_ = content;
        playbackSurface_ = videoSurface_;
    }
    if (webUiActive_ && webUiPlayerRouteActive_ && webUiVideoGeometryValid_ &&
        RectWidth(webUiVideoBounds_) > 0 && RectHeight(webUiVideoBounds_) > 0) {
        videoSurface_ = webUiVideoBounds_;
        playbackSurface_ = videoSurface_;
    }
    const int progressInset = compact ? Scale(18) : Scale(24);
    progress_ = MakeRect(transportBar_.left + progressInset,
                         transportBar_.top + (compact ? Scale(26) : Scale(30)),
                         transportBar_.right - progressInset,
                         transportBar_.top + (compact ? Scale(32) : Scale(36)));
    const int topButtonY = topBar_.top + (RectHeight(topBar_) - topButtonSize) / 2;
    int topButtonX = topBar_.right - (compact ? Scale(10) : Scale(12)) - topButtonSize;
    const bool settingsOpen = inspectorTab_ == InspectorTab::Settings;
    buttons_.push_back(UiButton{Command::Settings,
                                MakeRect(topButtonX, topButtonY, topButtonX + topButtonSize, topButtonY + topButtonSize),
                                L"",
                                settingsOpen ? L"Back to media" : L"Settings",
                                settingsOpen ? IconKind::ChevronLeft : IconKind::Cog,
                                ButtonKind::Icon,
                                false,
                                false});
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

    if (showInspectorTabs_ && !settingsOpen) {
        const RECT inner = DeflateRectCopy(inspector_, Scale(16), Scale(16));
        const int tabTop = inner.top + Scale(38);
        const int tabHeight = Scale(28);
        const int tabGap = Scale(3);
        struct InspectorTabSpec {
            Command command;
            InspectorTab tab;
            const wchar_t* label;
            const wchar_t* tooltip;
            int preferredWidth;
        };
        const InspectorTabSpec tabs[] = {
            {Command::InspectorRecent, InspectorTab::Recent, L"Recent", L"Recent playback", 54},
            {Command::InspectorFolder, InspectorTab::Folder, L"Folder", L"Current folder", 54},
            {Command::InspectorMedia, InspectorTab::Media, L"Media", L"Media info", 44},
            {Command::InspectorSystem, InspectorTab::System, L"System", L"System info", 56},
            {Command::InspectorLog, InspectorTab::Log, L"Log", L"Playback log", 32},
        };
        constexpr int tabCount = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));
        int preferredWidth = tabGap * (tabCount - 1);
        for (const auto& tab : tabs) {
            preferredWidth += Scale(tab.preferredWidth);
        }
        const int availableWidth = RectWidth(inner);
        const bool squeezeTabs = preferredWidth > availableWidth;
        const int uniformWidth = squeezeTabs
                                     ? std::max(Scale(32), (availableWidth - tabGap * (tabCount - 1)) / tabCount)
                                     : 0;
        const int spareWidth = squeezeTabs ? 0 : std::max(0, availableWidth - preferredWidth);
        int tabX = inner.left;
        for (int index = 0; index < tabCount; ++index) {
            const int extra = squeezeTabs ? 0 : spareWidth / tabCount + (index < spareWidth % tabCount ? 1 : 0);
            const int tabWidth = squeezeTabs ? uniformWidth : Scale(tabs[index].preferredWidth) + extra;
            const int tabRight = index + 1 == tabCount ? inner.right : tabX + tabWidth;
            buttons_.push_back(UiButton{tabs[index].command,
                                        MakeRect(tabX, tabTop, tabRight, tabTop + tabHeight),
                                        tabs[index].label,
                                        tabs[index].tooltip,
                                        IconKind::None,
                                        ButtonKind::InspectorTab,
                                        false,
                                        inspectorTab_ == tabs[index].tab});
            tabX = tabRight + tabGap;
        }
    }

    UpdateInspectorPathItems(snapshot);
    UpdateSettingsScrollLayout(settings);

    if (inspectorTab_ == InspectorTab::Settings &&
        HdrToneCurveAvailable(settings) &&
        RectWidth(settingsContentViewport_) > 0 &&
        RectHeight(settingsContentViewport_) > 0) {
        RECT cursor = settingsContentViewport_;
        cursor.top -= settingsScrollOffset_;
        cursor.bottom = cursor.top + std::max(settingsContentHeight_, RectHeight(settingsContentViewport_));
        const int settingsVideoFieldCount = SettingsVideoFieldCount();
        const bool stackedFields = RectWidth(cursor) < Scale(250);
        cursor.top += Scale(24);
        cursor.top += (stackedFields ? Scale(40) : Scale(28)) * settingsVideoFieldCount;
        cursor.top += Scale(8);

        const int editorHeight = Scale(190);
        const RECT editor = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + editorHeight);
        const RECT plot = MakeRect(editor.left + Scale(50),
                                   editor.top + Scale(56),
                                   editor.right - Scale(16),
                                   editor.bottom - Scale(34));
        const bool editorVisible = !hdrToneCurveExpanded_ &&
                                   editor.bottom > settingsContentViewport_.top &&
                                   editor.top < settingsContentViewport_.bottom;
        if (editorVisible && RectWidth(plot) >= Scale(120) && RectHeight(plot) >= Scale(64)) {
            hdrToneCurvePlot_ = plot;
            const int resetWidth = Scale(58);
            const int resetHeight = Scale(24);
            const int zoomWidth = Scale(58);
            const int hdrButtonGap = Scale(6);
            const RECT zoomButton = MakeRect(editor.right - Scale(10) - zoomWidth,
                                             editor.top + Scale(7),
                                             editor.right - Scale(10),
                                             editor.top + Scale(7) + resetHeight);
            const RECT resetButton = MakeRect(zoomButton.left - hdrButtonGap - resetWidth,
                                              editor.top + Scale(7),
                                              zoomButton.left - hdrButtonGap,
                                              editor.top + Scale(7) + resetHeight);
            if (resetButton.top >= settingsContentViewport_.top &&
                resetButton.bottom <= settingsContentViewport_.bottom &&
                zoomButton.bottom <= settingsContentViewport_.bottom) {
                buttons_.push_back(UiButton{Command::ResetHdrToneCurve,
                                            resetButton,
                                            L"Reset",
                                            L"Reset HDR tone curve",
                                            IconKind::None,
                                            ButtonKind::Tab,
                                            false,
                                            false});
                buttons_.push_back(UiButton{Command::ToggleHdrToneCurveExpanded,
                                            zoomButton,
                                            L"Zoom",
                                            L"Open large HDR curve editor",
                                            IconKind::None,
                                            ButtonKind::Tab,
                                            false,
                                            false});
            }
        }
    }

    const int controlCenterY = transportBar_.bottom - (compact ? Scale(28) : Scale(30));
    int x = static_cast<int>(transportBar_.left) + (compact ? Scale(18) : Scale(24));
    transportControlsLeft_ = x;
    const int smallTop = controlCenterY - transportButtonSize / 2;
    const int playTop = controlCenterY - playButtonSize / 2;
    buttons_.push_back(UiButton{Command::Back,
                                MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                L"",
                                L"Back 10 seconds",
                                IconKind::Back10,
                                ButtonKind::TransportIcon,
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
    buttons_.push_back(UiButton{Command::Forward,
                                MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                L"",
                                L"Forward 10 seconds",
                                IconKind::Forward10,
                                ButtonKind::TransportIcon,
                                false,
                                false});
    x += transportButtonSize + buttonGap;
    buttons_.push_back(UiButton{Command::Stop,
                                MakeRect(x, smallTop, x + transportButtonSize, smallTop + transportButtonSize),
                                L"",
                                L"Stop",
                                IconKind::Stop,
                                ButtonKind::TransportIcon,
                                false,
                                false});
    x += transportButtonSize + buttonGap;

    const int rightTop = controlCenterY - rightButtonSize / 2;
    const int rightEdge = transportBar_.right - Scale(22);
    bool showVolumeSlider = true;
    bool showTransportHdrButton = showHdrButton;
    bool showTransportCmv4Button = showCmv4Button;
    bool showTransportSubtitleButton = showSubtitleButton;
    const auto transportRightClusterWidth = [&]() {
        return rightClusterWidthFor({showTransportHdrButton ? hdrButtonWidth : 0,
                                     showTransportCmv4Button ? cmv4ButtonWidth : 0,
                                     showTransportSubtitleButton ? rightButtonSize : 0,
                                     rightButtonSize});
    };
    int rightWidth = transportRightClusterWidth();
    if (rightEdge - rightWidth < x + Scale(12) &&
        showTransportSubtitleButton) {
        showTransportSubtitleButton = false;
        rightWidth = transportRightClusterWidth();
    }
    if (rightEdge - rightWidth < x + Scale(12) &&
        showTransportCmv4Button) {
        showTransportCmv4Button = false;
        rightWidth = transportRightClusterWidth();
    }
    if (rightEdge - rightWidth < x + Scale(12) &&
        showTransportHdrButton) {
        showTransportHdrButton = false;
        rightWidth = transportRightClusterWidth();
    }
    int rightX = rightEdge - rightWidth;
    transportRightControlsLeft_ = rightX;

    if (showVolumeSlider) {
        volumeSlider_ = MakeRect(x, rightTop, x + volumeSliderWidth, rightTop + rightButtonSize);
        if (volumeSlider_.right > rightX - Scale(22)) {
            volumeSlider_ = RECT{};
            showVolumeSlider = false;
        } else {
            x += volumeSliderWidth + buttonGap;
        }
    }
    transportControlsRight_ = x;
    if (showTransportHdrButton) {
        addDolbyVisionHdrButton(rightX, rightTop);
        rightX += hdrButtonWidth + rightButtonGap;
    }
    if (showTransportCmv4Button) {
        addDolbyVisionCmv4Button(rightX, rightTop);
        rightX += cmv4ButtonWidth + rightButtonGap;
    }
    if (showTransportSubtitleButton) {
        buttons_.push_back(UiButton{Command::SubtitleMenu,
                                    MakeRect(rightX, rightTop, rightX + rightButtonSize, rightTop + rightButtonSize),
                                    L"",
                                    L"Subtitles",
                                    IconKind::Subtitles,
                                    ButtonKind::TransportIcon,
                                    false,
                                    subtitlesEnabled});
        rightX += rightButtonSize + rightButtonGap;
    }
    buttons_.push_back(UiButton{Command::Fullscreen,
                                MakeRect(rightX, rightTop, rightX + rightButtonSize, rightTop + rightButtonSize),
                                L"",
                                fullscreen_ ? L"Exit fullscreen" : L"Fullscreen",
                                fullscreen_ ? IconKind::Windowed : IconKind::Fullscreen,
                                ButtonKind::TransportIcon,
                                false,
                                false});

    UpdateSubtitleMenuLayout();

    if (hdrToneCurveExpanded_ && hdrToneCurveWindow_) {
        UpdateHdrToneCurveFloatingLayout();
    }

    if (!SidebarAnimationActive() || webUiActive_) {
        RECT playerBounds = PlaybackSurfaceBounds();
        if (webUiActive_ && !webUiPlayerRouteActive_) {
            playerBounds = RECT{};
        } else if (webUiActive_ && !fullscreen_ && RectWidth(videoSurface_) > 0 && RectHeight(videoSurface_) > 0) {
            playerBounds = DeflateRectCopy(videoSurface_, Scale(5), Scale(5));
        }
        playbackPlayer_.SetBounds(playerBounds);
    }
    UpdateVideoHost();
    UpdateTransportOverlay();
    UpdateFullscreenOverlay();
    UpdateSubtitleMenuOverlay();
}

void MainWindow::UpdateVideoHost() {
    if (!videoHostReady_ || !videoHost_) {
        return;
    }
    if (webUiActive_ && !webUiPlayerRouteActive_) {
        if (IsWindowVisible(videoHost_)) {
            ShowWindow(videoHost_, SW_HIDE);
        }
        const RECT empty{};
        if (!RectEquals(lastVideoHostBounds_, empty)) {
            lastVideoHostBounds_ = empty;
        }
        UpdateBufferingOverlay();
        return;
    }
    if (SidebarAnimationActive() && !webUiActive_) {
        UpdateBufferingOverlay();
        return;
    }
    RECT bounds = PlaybackSurfaceBounds();
    if (webUiActive_ && !fullscreen_ && RectWidth(videoSurface_) > 0 && RectHeight(videoSurface_) > 0) {
        bounds = DeflateRectCopy(videoSurface_, Scale(5), Scale(5));
    }
    const auto snapshot = controller_.Snapshot();
    const bool nativeActive = nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning();
    const bool nativeBuffering = nativeActive &&
                                 snapshot.media.has_value() &&
                                 snapshot.media->hasVideo &&
                                 nativeVideoDecoder_->Stats().buffering;
    const bool nativePausedFrame = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                                   snapshot.state == PlaybackState::Paused &&
                                   snapshot.media.has_value() &&
                                   snapshot.media->hasVideo;
    const bool nativeHeldFrame = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                                 nativeFrameHoldVisible_ &&
                                 heldNativeFrame_.has_value();
    const bool nativeVideoVisible = nativeActive || nativePausedFrame || nativeHeldFrame;
    const int w = RectWidth(bounds);
    const int h = RectHeight(bounds);
    if (nativeVideoVisible && w > 0 && h > 0) {
        const bool wasVisible = IsWindowVisible(videoHost_) != FALSE;
        const bool boundsChanged = bounds.left != lastVideoHostBounds_.left ||
                                   bounds.top != lastVideoHostBounds_.top ||
                                   w != RectWidth(lastVideoHostBounds_) ||
                                   h != RectHeight(lastVideoHostBounds_);
        bool resized = false;
        if (boundsChanged || !wasVisible) {
            SetWindowPos(videoHost_, webUiActive_ ? HWND_TOP : HWND_BOTTOM, bounds.left, bounds.top, w, h, SWP_NOACTIVATE);
            if (boundsChanged) {
                lastVideoHostBounds_ = bounds;
            }
            if (boundsChanged && d3dRenderer_) {
                d3dRenderer_->OnResize();
                resized = true;
            }
        }
        if (webUiActive_) {
            HRGN region = fullscreen_
                              ? CreateRectRgn(0, 0, w + 1, h + 1)
                              : CreateRoundRectRgn(0, 0, w + 1, h + 1, Scale(8), Scale(8));
            const auto subtractCutout = [&](RECT cutout, const int radius) {
                OffsetRect(&cutout, -bounds.left, -bounds.top);
                const RECT localBounds = MakeRect(0, 0, w, h);
                RECT clippedCutout{};
                if (IntersectRect(&clippedCutout, &cutout, &localBounds)) {
                    HRGN cutoutRegion = radius > 0
                                             ? CreateRoundRectRgn(clippedCutout.left,
                                                                  clippedCutout.top,
                                                                  clippedCutout.right + 1,
                                                                  clippedCutout.bottom + 1,
                                                                  radius,
                                                                  radius)
                                             : CreateRectRgn(clippedCutout.left,
                                                             clippedCutout.top,
                                                             clippedCutout.right + 1,
                                                             clippedCutout.bottom + 1);
                    CombineRgn(region, region, cutoutRegion, RGN_DIFF);
                    DeleteObject(cutoutRegion);
                }
            };
            if ((subtitleMenuTarget_ > 0.0 || subtitleMenuAmount_ > 0.01) &&
                RectWidth(subtitleMenu_) > 0 &&
                RectHeight(subtitleMenu_) > 0) {
                RECT subtitleAnchor = webUiSubtitleGeometryValid_ ? webUiSubtitleAnchor_ : RECT{};
                bool foundSubtitleAnchor = webUiSubtitleGeometryValid_;
                if (!foundSubtitleAnchor) {
                    for (const auto& button : buttons_) {
                        if (button.command == Command::SubtitleMenu) {
                            subtitleAnchor = button.bounds;
                            foundSubtitleAnchor = true;
                            break;
                        }
                    }
                }

                RECT cutout = subtitleMenu_;
                if (foundSubtitleAnchor &&
                    RectWidth(subtitleAnchor) > 0 &&
                    RectHeight(subtitleAnchor) > 0) {
                    cutout = LerpRect(subtitleAnchor, subtitleMenu_, subtitleMenuAmount_);
                }
                InflateRect(&cutout, 1, 1);
                subtractCutout(cutout, Scale(8));
            }
            if (fullscreen_ &&
                (fullscreenTransportTarget_ > 0.0 || fullscreenTransportAmount_ > 0.01)) {
                RECT transportCutout = webUiTransportGeometryValid_ ? webUiTransportBounds_ : transportBar_;
                if (RectWidth(transportCutout) > 0 && RectHeight(transportCutout) > 0) {
                    const int hiddenOffset = RectHeight(transportCutout) + Scale(24);
                    const int currentOffset = static_cast<int>(
                        std::round(hiddenOffset * (1.0 - std::clamp(fullscreenTransportAmount_, 0.0, 1.0))));
                    OffsetRect(&transportCutout, 0, currentOffset);
                    InflateRect(&transportCutout, 1, 1);
                    subtractCutout(transportCutout, Scale(8));
                }
            }
            if (fullscreen_ && refreshRateSyncUnavailable_) {
                subtractCutout(bounds, 0);
            }
            if (!SetWindowRgn(videoHost_, region, TRUE)) {
                DeleteObject(region);
            }
        } else {
            SetWindowRgn(videoHost_, nullptr, TRUE);
        }
        if (!wasVisible) {
            ShowWindow(videoHost_, SW_SHOW);
        }
        if ((snapshot.state == PlaybackState::Paused || !nativeActive || nativeBuffering) &&
            heldNativeFrame_.has_value() &&
            (resized || !wasVisible || heldNativeFrameNeedsPresent_)) {
            RenderHeldNativeFrame(false);
        }
    } else {
        if (IsWindowVisible(videoHost_)) {
            ShowWindow(videoHost_, SW_HIDE);
        }
        const RECT empty{};
        if (!RectEquals(lastVideoHostBounds_, empty)) {
            lastVideoHostBounds_ = empty;
        }
    }
    UpdateBufferingOverlay();
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

void MainWindow::EnsureBufferingOverlay() {
    if (bufferingOverlay_ && bufferingHudOverlay_) {
        return;
    }

    const auto registerOverlayClass = [&](const wchar_t* className, WNDPROC proc) -> bool {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance_, className, &existing)) {
            return true;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = proc;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            if (!bufferingOverlayCreateFailedLogged_) {
                LogApp(anvil::playback::LogLevel::Warning,
                       std::wstring(L"buffering overlay class register failed class=") + className +
                           L" error=" + std::to_wstring(GetLastError()));
                bufferingOverlayCreateFailedLogged_ = true;
            }
            return false;
        }
        return true;
    };

    if (!bufferingOverlay_) {
        if (!registerOverlayClass(kBufferingOverlayClassName, &MainWindow::BufferingOverlayProc)) {
            return;
        }

        SetLastError(ERROR_SUCCESS);
        bufferingOverlay_ = CreateWindowExW(
            WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kBufferingOverlayClassName,
            L"",
            WS_POPUP | WS_CLIPSIBLINGS,
            0, 0, 1, 1,
            hwnd_,
            nullptr,
            instance_,
            this);

        if (bufferingOverlay_) {
            if (!EnableBackdropBlur(bufferingOverlay_)) {
                LogApp(anvil::playback::LogLevel::Warning, L"buffering overlay blur unavailable");
            }
            LogApp(anvil::playback::LogLevel::Debug, L"buffering blur overlay window created");
        } else if (!bufferingOverlayCreateFailedLogged_) {
            LogApp(anvil::playback::LogLevel::Warning,
                   L"buffering blur overlay create failed error=" + std::to_wstring(GetLastError()));
            bufferingOverlayCreateFailedLogged_ = true;
            return;
        }
    }

    if (!bufferingHudOverlay_) {
        if (!registerOverlayClass(kBufferingHudOverlayClassName, &MainWindow::BufferingHudOverlayProc)) {
            return;
        }

        SetLastError(ERROR_SUCCESS);
        bufferingHudOverlay_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kBufferingHudOverlayClassName,
            L"",
            WS_POPUP | WS_CLIPSIBLINGS,
            0, 0, 1, 1,
            hwnd_,
            nullptr,
            instance_,
            this);

        if (bufferingHudOverlay_) {
            LogApp(anvil::playback::LogLevel::Debug, L"buffering hud overlay window created");
        } else if (!bufferingOverlayCreateFailedLogged_) {
            LogApp(anvil::playback::LogLevel::Warning,
                   L"buffering hud overlay create failed error=" + std::to_wstring(GetLastError()));
            bufferingOverlayCreateFailedLogged_ = true;
            return;
        }
    }

    bufferingOverlayCreateFailedLogged_ = false;
}

void MainWindow::UpdateBufferingOverlay(const NativeVideoQueueStats* statsOverride) {
    const auto hideOverlay = [&]() {
        if (bufferingOverlayVisible_) {
            LogApp(anvil::playback::LogLevel::Debug, L"buffering overlay hide");
            bufferingOverlayVisible_ = false;
        }
        if (bufferingHudOverlay_) {
            KillTimer(bufferingHudOverlay_, 1);
            if (IsWindowVisible(bufferingHudOverlay_)) {
                ShowWindow(bufferingHudOverlay_, SW_HIDE);
            }
        }
        if (bufferingOverlay_) {
            if (IsWindowVisible(bufferingOverlay_)) {
                ShowWindow(bufferingOverlay_, SW_HIDE);
            }
        }
        lastBufferingOverlayBounds_ = RECT{};
        lastBufferingOverlayScreenBounds_ = RECT{};
    };

    const auto snapshot = controller_.Snapshot();
    NativeVideoQueueStats stats{};
    bool hasStats = false;
    if (statsOverride) {
        stats = *statsOverride;
        hasStats = true;
    } else if (nativeVideoDecoder_) {
        stats = nativeVideoDecoder_->Stats();
        hasStats = true;
    }
    if (webUiActive_ && !webUiPlayerRouteActive_) {
        hideOverlay();
        return;
    }
    const bool nativeDecoderRunning = nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning();
    const bool statePlaying = snapshot.state == PlaybackState::Playing;
    const bool hasVideoMedia = snapshot.media.has_value() && snapshot.media->hasVideo;
    const bool nativeBuffering = backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
                                 nativeDecoderRunning &&
                                 statePlaying &&
                                 hasVideoMedia &&
                                 hasStats &&
                                 stats.buffering;
    if (!nativeBuffering) {
        if (hasStats && stats.buffering && !bufferingOverlaySuppressedLogged_) {
            LogApp(anvil::playback::LogLevel::Debug,
                   std::wstring(L"buffering overlay skip running=") + (nativeDecoderRunning ? L"true" : L"false") +
                       L" playing=" + (statePlaying ? L"true" : L"false") +
                       L" has_video=" + (hasVideoMedia ? L"true" : L"false") +
                       L" webui=" + (webUiActive_ ? L"true" : L"false") +
                       L" player_route=" + (webUiPlayerRouteActive_ ? L"true" : L"false"));
            bufferingOverlaySuppressedLogged_ = true;
        } else if (!stats.buffering) {
            bufferingOverlaySuppressedLogged_ = false;
        }
        hideOverlay();
        return;
    }
    bufferingOverlaySuppressedLogged_ = false;
    bufferingOverlayStats_ = stats;

    RECT overlayBounds = PlaybackSurfaceBounds();
    if (webUiActive_ && !fullscreen_ && RectWidth(videoSurface_) > 0 && RectHeight(videoSurface_) > 0) {
        overlayBounds = DeflateRectCopy(videoSurface_, Scale(5), Scale(5));
    }
    const int w = RectWidth(overlayBounds);
    const int h = RectHeight(overlayBounds);
    if (w <= 0 || h <= 0) {
        hideOverlay();
        return;
    }

    EnsureBufferingOverlay();
    if (!bufferingOverlay_ || !bufferingHudOverlay_) {
        hideOverlay();
        return;
    }

    const bool wasVisible = IsWindowVisible(bufferingOverlay_) != FALSE &&
                            IsWindowVisible(bufferingHudOverlay_) != FALSE;
    const bool moved = overlayBounds.left != lastBufferingOverlayBounds_.left ||
                       overlayBounds.top != lastBufferingOverlayBounds_.top ||
                       w != RectWidth(lastBufferingOverlayBounds_) ||
                       h != RectHeight(lastBufferingOverlayBounds_);
    int hudWidth = std::min(w, Scale(220));
    if (w >= Scale(96)) {
        hudWidth = std::max(hudWidth, Scale(96));
    }
    int hudHeight = std::min(h, Scale(112));
    if (h >= Scale(82)) {
        hudHeight = std::max(hudHeight, Scale(82));
    }

    RECT windowBounds = overlayBounds;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&windowBounds), 2);
    const bool screenMoved = windowBounds.left != lastBufferingOverlayScreenBounds_.left ||
                             windowBounds.top != lastBufferingOverlayScreenBounds_.top ||
                             w != RectWidth(lastBufferingOverlayScreenBounds_) ||
                             h != RectHeight(lastBufferingOverlayScreenBounds_);
    const bool positionChanged = moved || screenMoved || !wasVisible;
    if (positionChanged) {
        const int hudLeft = windowBounds.left + (w - hudWidth) / 2;
        const int hudTop = windowBounds.top + (h - hudHeight) / 2;
        SetWindowPos(bufferingOverlay_,
                     HWND_TOP,
                     windowBounds.left,
                     windowBounds.top,
                     w,
                     h,
                     SWP_NOACTIVATE);
        SetWindowPos(bufferingHudOverlay_,
                     HWND_TOP,
                     hudLeft,
                     hudTop,
                     hudWidth,
                     hudHeight,
                     SWP_NOACTIVATE);
    } else {
        SetWindowPos(bufferingOverlay_, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(bufferingHudOverlay_, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }

    const bool fullscreenTransportCutoutVisible =
        fullscreen_ &&
        (fullscreenTransportTarget_ > 0.0 || fullscreenTransportAmount_ > 0.01) &&
        RectWidth(transportBar_) > 0 &&
        RectHeight(transportBar_) > 0;
    const bool regionChanged = moved || !wasVisible || fullscreenTransportCutoutVisible;
    if (regionChanged) {
        const auto createOverlayRegion = [&]() -> HRGN {
            return fullscreen_
                       ? CreateRectRgn(0, 0, w + 1, h + 1)
                       : CreateRoundRectRgn(0, 0, w + 1, h + 1, Scale(8), Scale(8));
        };
        if (HRGN blurRegion = createOverlayRegion()) {
            if (fullscreenTransportCutoutVisible) {
                RECT cutout = transportBar_;
                MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&cutout), 2);
                OffsetRect(&cutout, -windowBounds.left, -windowBounds.top);
                InflateRect(&cutout, Scale(2), Scale(2));
                if (HRGN cutoutRegion = CreateRoundRectRgn(cutout.left,
                                                           cutout.top,
                                                           cutout.right + 1,
                                                           cutout.bottom + 1,
                                                           Scale(12),
                                                           Scale(12))) {
                    CombineRgn(blurRegion, blurRegion, cutoutRegion, RGN_DIFF);
                    DeleteObject(cutoutRegion);
                }
            }
            if (!SetWindowRgn(bufferingOverlay_, blurRegion, TRUE)) {
                DeleteObject(blurRegion);
            }
        }
        if (HRGN hudRegion = CreateRectRgn(0, 0, hudWidth + 1, hudHeight + 1)) {
            if (!SetWindowRgn(bufferingHudOverlay_, hudRegion, TRUE)) {
                DeleteObject(hudRegion);
            }
        }
        lastBufferingOverlayBounds_ = overlayBounds;
    }
    if (positionChanged) {
        lastBufferingOverlayScreenBounds_ = windowBounds;
    }

    RenderBufferingHudOverlay(bufferingOverlayStats_);
    SetTimer(bufferingHudOverlay_, 1, kUiAnimationTimerMs, nullptr);
    if (!IsWindowVisible(bufferingOverlay_)) {
        ShowWindow(bufferingOverlay_, SW_SHOWNOACTIVATE);
    }
    if (!IsWindowVisible(bufferingHudOverlay_)) {
        ShowWindow(bufferingHudOverlay_, SW_SHOWNOACTIVATE);
    }
    if (!bufferingOverlayVisible_) {
        LogApp(anvil::playback::LogLevel::Debug,
               L"buffering overlay show bounds=" + std::to_wstring(w) + L"x" + std::to_wstring(h) +
                   L" net_kbps=" + std::to_wstring(stats.networkBytesPerSecond / 1024));
        bufferingOverlayVisible_ = true;
    }
}

void MainWindow::UpdateTransportOverlay() {
    if (webUiActive_) {
        if (transportOverlay_) {
            ShowWindow(transportOverlay_, SW_HIDE);
        }
        lastTransportOverlayBounds_ = RECT{};
        return;
    }

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

    RECT overlayBounds = transportBar_;

    const int w = RectWidth(overlayBounds);
    const int h = RectHeight(overlayBounds);
    const bool wasVisible = IsWindowVisible(transportOverlay_) != FALSE;
    const bool moved = overlayBounds.left != lastTransportOverlayBounds_.left ||
                       overlayBounds.top != lastTransportOverlayBounds_.top ||
                       w != RectWidth(lastTransportOverlayBounds_) ||
                       h != RectHeight(lastTransportOverlayBounds_);
    const bool regionChanged = moved;
    if (moved || !wasVisible) {
        SetWindowPos(transportOverlay_,
                     HWND_TOP,
                     overlayBounds.left,
                     overlayBounds.top,
                     w,
                     h,
                     SWP_NOACTIVATE);
    }

    if (regionChanged) {
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        HRGN transportRegion = CreateRoundRectRgn(transportBar_.left - overlayBounds.left,
                                                  transportBar_.top - overlayBounds.top,
                                                  transportBar_.right - overlayBounds.left + 1,
                                                  transportBar_.bottom - overlayBounds.top + 1,
                                                  Scale(12),
                                                  Scale(12));
        CombineRgn(region, region, transportRegion, RGN_OR);
        DeleteObject(transportRegion);
        SetWindowRgn(transportOverlay_, region, TRUE);
        lastTransportOverlayBounds_ = overlayBounds;
    }
    if (!wasVisible) {
        ShowWindow(transportOverlay_, SW_SHOWNOACTIVATE);
    }
    if (moved || regionChanged || !wasVisible) {
        InvalidateRect(transportOverlay_, nullptr, FALSE);
    }
}

void MainWindow::UpdateFullscreenOverlay() {
    if (webUiActive_) {
        if (fullscreenOverlay_) {
            ShowWindow(fullscreenOverlay_, SW_HIDE);
        }
        lastFullscreenOverlayBounds_ = RECT{};
        return;
    }

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

    RECT overlayBounds = transportBar_;

    const int w = RectWidth(overlayBounds);
    const int h = RectHeight(overlayBounds);
    const bool wasVisible = IsWindowVisible(fullscreenOverlay_) != FALSE;
    const bool moved = overlayBounds.left != lastFullscreenOverlayBounds_.left ||
                       overlayBounds.top != lastFullscreenOverlayBounds_.top ||
                       w != RectWidth(lastFullscreenOverlayBounds_) ||
                       h != RectHeight(lastFullscreenOverlayBounds_);
    const bool regionChanged = moved;
    if (moved || !wasVisible) {
        SetWindowPos(fullscreenOverlay_,
                     HWND_TOP,
                     overlayBounds.left,
                     overlayBounds.top,
                     w,
                     h,
                     SWP_NOACTIVATE);
    }

    if (regionChanged) {
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        HRGN transportRegion = CreateRoundRectRgn(transportBar_.left - overlayBounds.left,
                                                  transportBar_.top - overlayBounds.top,
                                                  transportBar_.right - overlayBounds.left + 1,
                                                  transportBar_.bottom - overlayBounds.top + 1,
                                                  Scale(12),
                                                  Scale(12));
        CombineRgn(region, region, transportRegion, RGN_OR);
        DeleteObject(transportRegion);
        SetWindowRgn(fullscreenOverlay_, region, TRUE);
        lastFullscreenOverlayBounds_ = overlayBounds;
    }
    if (!wasVisible) {
        ShowWindow(fullscreenOverlay_, SW_SHOWNOACTIVATE);
    }
    if (moved || regionChanged || !wasVisible) {
        InvalidateRect(fullscreenOverlay_, nullptr, FALSE);
    }
}

void MainWindow::EnsureSubtitleMenuOverlay() {
    if (subtitleMenuOverlay_) {
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &MainWindow::SubtitleMenuOverlayProc;
    wc.lpszClassName = kSubtitleMenuOverlayClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    subtitleMenuOverlay_ = CreateWindowExW(
        0,
        kSubtitleMenuOverlayClassName,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        hwnd_,
        nullptr,
        instance_,
        this);
}

void MainWindow::UpdateSubtitleMenuOverlay() {
    if (webUiActive_) {
        if (subtitleMenuOverlay_) {
            ShowWindow(subtitleMenuOverlay_, SW_HIDE);
        }
        lastSubtitleMenuOverlayBounds_ = RECT{};
        return;
    }

    const bool showOverlay = ((subtitleMenuTarget_ > 0.0 && subtitleMenuAmount_ > 0.025) ||
                              subtitleMenuAmount_ > 0.04) &&
                             RectWidth(subtitleMenu_) > 0 &&
                             RectHeight(subtitleMenu_) > 0;
    if (!showOverlay) {
        if (subtitleMenuOverlay_) {
            ShowWindow(subtitleMenuOverlay_, SW_HIDE);
        }
        lastSubtitleMenuOverlayBounds_ = RECT{};
        return;
    }

    EnsureSubtitleMenuOverlay();
    if (!subtitleMenuOverlay_) {
        return;
    }

    const int w = RectWidth(subtitleMenu_);
    const int h = RectHeight(subtitleMenu_);
    const bool wasVisible = IsWindowVisible(subtitleMenuOverlay_) != FALSE;
    const bool moved = subtitleMenu_.left != lastSubtitleMenuOverlayBounds_.left ||
                       subtitleMenu_.top != lastSubtitleMenuOverlayBounds_.top ||
                       w != RectWidth(lastSubtitleMenuOverlayBounds_) ||
                       h != RectHeight(lastSubtitleMenuOverlayBounds_);

    if (moved || !wasVisible) {
        SetWindowPos(subtitleMenuOverlay_,
                     HWND_TOP,
                     subtitleMenu_.left,
                     subtitleMenu_.top,
                     w,
                     h,
                     SWP_NOACTIVATE);
    }

    if (moved || !wasVisible) {
        HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, Scale(9), Scale(9));
        SetWindowRgn(subtitleMenuOverlay_, region, TRUE);
        lastSubtitleMenuOverlayBounds_ = subtitleMenu_;
    }
    InvalidateRect(subtitleMenuOverlay_, nullptr, FALSE);
    if (!wasVisible) {
        ShowWindow(subtitleMenuOverlay_, SW_SHOWNOACTIVATE);
        UpdateWindow(subtitleMenuOverlay_);
    }
}

void MainWindow::EnsureHdrToneCurveWindow() {
    if (hdrToneCurveWindow_) {
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = &MainWindow::HdrToneCurveWindowProc;
        wc.lpszClassName = kHdrToneCurveWindowClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hbrBackground = nullptr;
        registered = RegisterClassExW(&wc) != FALSE || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
    constexpr DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    RECT rect = MakeRect(0, 0, Scale(900), Scale(620));
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi_)) {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
    const int windowWidth = RectWidth(rect);
    const int windowHeight = RectHeight(rect);

    RECT owner{};
    GetWindowRect(hwnd_, &owner);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int x = owner.right + Scale(16);
    int y = owner.top + Scale(60);
    if (x + windowWidth > workArea.right - Scale(12)) {
        x = owner.left + Scale(52);
        y = owner.top + Scale(72);
    }
    x = std::clamp(x, static_cast<int>(workArea.left) + Scale(8), static_cast<int>(workArea.right) - windowWidth - Scale(8));
    y = std::clamp(y, static_cast<int>(workArea.top) + Scale(8), static_cast<int>(workArea.bottom) - windowHeight - Scale(8));

    hdrToneCurveWindow_ = CreateWindowExW(
        exStyle,
        kHdrToneCurveWindowClassName,
        L"HDR Curve Editor",
        style,
        x,
        y,
        windowWidth,
        windowHeight,
        hwnd_,
        nullptr,
        instance_,
        this);
    UpdateHdrToneCurveFloatingLayout();
}

void MainWindow::UpdateHdrToneCurveFloatingLayout() {
    if (!hdrToneCurveWindow_) {
        if (hdrToneCurveExpanded_) {
            return;
        }
        hdrToneCurveExpandedEditor_ = RECT{};
        hdrToneCurveFloatingReset_ = RECT{};
        return;
    }

    RECT client{};
    GetClientRect(hdrToneCurveWindow_, &client);
    if (RectWidth(client) <= 0 || RectHeight(client) <= 0) {
        hdrToneCurveExpandedEditor_ = RECT{};
        hdrToneCurvePlot_ = RECT{};
        hdrToneCurveFloatingReset_ = RECT{};
        return;
    }

    hdrToneCurveExpandedEditor_ = DeflateRectCopy(client, Scale(14), Scale(14));
    const int resetWidth = Scale(72);
    const int resetHeight = Scale(28);
    hdrToneCurveFloatingReset_ = MakeRect(hdrToneCurveExpandedEditor_.right - Scale(24) - resetWidth,
                                          hdrToneCurveExpandedEditor_.top + Scale(14),
                                          hdrToneCurveExpandedEditor_.right - Scale(24),
                                          hdrToneCurveExpandedEditor_.top + Scale(14) + resetHeight);
    hdrToneCurvePlot_ = MakeRect(hdrToneCurveExpandedEditor_.left + Scale(64),
                                 hdrToneCurveExpandedEditor_.top + Scale(82),
                                 hdrToneCurveExpandedEditor_.right - Scale(30),
                                 hdrToneCurveExpandedEditor_.bottom - Scale(48));
}

void MainWindow::UpdateSubtitleMenuLayout() {
    subtitleMenu_ = RECT{};
    if (subtitleMenuTarget_ <= 0.0 && subtitleMenuAmount_ <= 0.001) {
        return;
    }

    RECT anchor{};
    bool found = false;
    for (const auto& button : buttons_) {
        if (button.command == Command::SubtitleMenu) {
            anchor = button.bounds;
            found = true;
            break;
        }
    }
    if (!found || RectWidth(anchor) <= 0 || RectHeight(anchor) <= 0) {
        return;
    }

    if (webUiActive_ && webUiSubtitleGeometryValid_) {
        subtitleMenu_ = webUiSubtitlePopover_;
        return;
    }

    if (webUiActive_) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int viewportWidth = RectWidth(client);
        const int viewportHeight = RectHeight(client);
        if (viewportWidth <= 0 || viewportHeight <= 0) {
            return;
        }

        const bool compact = viewportWidth <= Scale(900) || viewportHeight <= Scale(560);
        const int margin = compact ? Scale(10) : Scale(16);
        const int width = std::min(compact ? Scale(380) : Scale(420),
                                   std::max(Scale(280), viewportWidth - margin * 2));
        const int height = std::min(compact ? Scale(480) : Scale(430),
                                    std::max(Scale(260), viewportHeight - (compact ? Scale(126) : Scale(152))));
        const auto clampPanelPosition = [](const int value, const int size, const int viewportSize, const int marginValue) {
            const int maxValue = std::max(marginValue, viewportSize - size - marginValue);
            return std::clamp(value, marginValue, maxValue);
        };
        const int left = clampPanelPosition(anchor.right - width, width, viewportWidth, margin);
        const int top = clampPanelPosition(anchor.bottom - height, height, viewportHeight, margin);
        subtitleMenu_ = MakeRect(left, top, left + width, top + height);
        return;
    }

    const int headerHeight = Scale(62);
    const int listGap = Scale(8);
    const int itemHeight = Scale(42);
    const int listBottomGap = Scale(6);
    const int delayHeight = Scale(56);
    const int actionHeight = Scale(44);
    const int styleHeight = Scale(48);
    const int desiredPanelWidth = Scale(366);
    int itemCount = 0;
    int fixedPanelHeight = headerHeight;
    int maxVisibleRows = 0;
    if (subtitleMenuPage_ == SubtitleMenuPage::Audio) {
        itemCount = std::max(1, static_cast<int>(audioMenuTracks_.size()));
        fixedPanelHeight = headerHeight + listGap + listBottomGap;
        maxVisibleRows = 7;
    } else if (subtitleMenuPage_ == SubtitleMenuPage::Subtitles) {
        itemCount = std::max(1, static_cast<int>(subtitleMenuTracks_.size()));
        fixedPanelHeight = headerHeight + listGap + listBottomGap + delayHeight + actionHeight + styleHeight * 3;
        maxVisibleRows = 6;
    } else {
        itemCount = 0;
        fixedPanelHeight = headerHeight + Scale(8) + actionHeight * 3 + styleHeight * 2;
        maxVisibleRows = 0;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    int maxRight = std::min(static_cast<int>(transportBar_.right) - Scale(18), static_cast<int>(client.right) - Scale(10));
    const int minLeft = std::max(static_cast<int>(transportBar_.left) + Scale(18),
                                 static_cast<int>(client.left) + Scale(10));
    const int availableWidth = std::max(Scale(280), maxRight - minLeft);
    const int panelWidth = std::min(desiredPanelWidth, availableWidth);
    int right = std::min(maxRight, static_cast<int>(anchor.right) + Scale(6));
    int left = right - panelWidth;
    if (left < minLeft) {
        left = minLeft;
        right = left + panelWidth;
    }
    if (right > maxRight) {
        right = maxRight;
        left = right - panelWidth;
    }
    if (left < client.left + Scale(10)) {
        left = static_cast<int>(client.left) + Scale(10);
        right = std::min(left + panelWidth, static_cast<int>(client.right) - Scale(10));
    }

    int bottom = anchor.top - Scale(12);
    const int minTop = (!fullscreen_ && RectHeight(topBar_) > 0)
                           ? static_cast<int>(topBar_.bottom) + Scale(10)
                           : static_cast<int>(client.top) + Scale(10);
    const int availableHeight = std::max(fixedPanelHeight + (maxVisibleRows > 0 ? itemHeight : 0),
                                         bottom - minTop);
    const int maxVisibleRowsBySpace = maxVisibleRows > 0
                                          ? std::max(1, (availableHeight - fixedPanelHeight) / itemHeight)
                                          : 0;
    subtitleMenuVisibleItemCount_ = maxVisibleRows > 0
                                        ? std::clamp(std::min(itemCount, maxVisibleRowsBySpace), 1, maxVisibleRows)
                                        : 0;
    const int panelHeight = fixedPanelHeight + itemHeight * subtitleMenuVisibleItemCount_;
    subtitleMenuScrollOffset_ = std::clamp(subtitleMenuScrollOffset_,
                                           0,
                                           std::max(0, itemCount - subtitleMenuVisibleItemCount_));
    int top = bottom - panelHeight;
    if (top < minTop) {
        top = minTop;
        bottom = top + panelHeight;
    }
    subtitleMenu_ = MakeRect(left, top, right, bottom);
}

int MainWindow::HitButton(const POINT point) const {
    for (std::size_t index = buttons_.size(); index > 0; --index) {
        const std::size_t buttonIndex = index - 1;
        if (buttons_[buttonIndex].enabled && ContainsPoint(buttons_[buttonIndex].bounds, point)) {
            return static_cast<int>(buttonIndex);
        }
    }
    return -1;
}

int MainWindow::HitInspectorPathItem(const POINT point) const {
    for (std::size_t index = 0; index < inspectorPathItems_.size(); ++index) {
        if (ContainsPoint(inspectorPathItems_[index].bounds, point)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

}  // namespace anvil::app
