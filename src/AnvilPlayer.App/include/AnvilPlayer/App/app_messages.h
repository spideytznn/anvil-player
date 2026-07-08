#pragma once

#include <windows.h>

namespace anvil::app {

// Window class names.
constexpr wchar_t kWindowClassName[] = L"AnvilPlayerWindow";
constexpr wchar_t kVideoHostClassName[] = L"AnvilVideoHostWindow";
constexpr wchar_t kFullscreenOverlayClassName[] = L"AnvilFullscreenOverlayWindow";
constexpr wchar_t kTransportOverlayClassName[] = L"AnvilTransportOverlayWindow";
constexpr wchar_t kSubtitleMenuOverlayClassName[] = L"AnvilSubtitleMenuOverlayWindow";
constexpr wchar_t kHdrToneCurveWindowClassName[] = L"AnvilHdrToneCurveWindow";

// Playback UI timer.
constexpr UINT_PTR kPlaybackTimer = 1001;
constexpr UINT_PTR kUiAnimationTimer = 1002;
constexpr UINT_PTR kFullscreenChromeHideTimer = 1003;
constexpr UINT_PTR kVideoPressTimer = 1004;
constexpr UINT kIdlePlaybackTimerMs = 250;
constexpr UINT kPlayingPlaybackTimerMs = 100;
constexpr UINT kUiAnimationTimerMs = 16;
constexpr UINT kVideoLongPressTimerMs = 320;

// Custom WM_APP messages posted from decode threads to the UI thread.
constexpr UINT kVideoFrameReadyMessage = WM_APP + 1;
constexpr UINT kNativeVideoFrameReadyMessage = WM_APP + 2;
constexpr UINT kPlaybackTimerTickMessage = WM_APP + 3;
constexpr UINT kNativeColorSettingsRefreshMessage = WM_APP + 4;
constexpr UINT kRuntimeStopCompleteMessage = WM_APP + 5;
constexpr UINT kOpenPathMessage = WM_APP + 6;

// DWM attribute constants for window chrome customization.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor = 36;
constexpr int kDwmCornerRound = 2;

// Runtime labels shown in the inspector / used in logs.
constexpr wchar_t kNativeFfmpegD3D11RuntimeLabel[] = L"native ffmpeg + d3d11 software decode";
constexpr wchar_t kInternalPlaybackRuntimeLabel[] = L"in-player ffmpeg software decode";
constexpr wchar_t kExternalPlaybackRuntimeLabel[] = L"external ffplay software decode";

}  // namespace anvil::app
