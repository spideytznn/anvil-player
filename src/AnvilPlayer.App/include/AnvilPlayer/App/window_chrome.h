#pragma once

#include "AnvilPlayer/App/app_messages.h"

#include <dwmapi.h>
#include <windows.h>

#include <algorithm>

namespace anvil::app {

// DWM border color is not consistently honored by every Windows 11 build.
// Disable DWM rounding and own the visible window shape with a region instead,
// so both native windows get identical corners without the system's bright arc.
inline void ApplyFramelessWindowChrome(const HWND hwnd,
                                       const COLORREF captionColor,
                                       const COLORREF textColor) {
    if (!hwnd) return;

    const BOOL dark = TRUE;
    const int corner = kDwmCornerDoNotRound;
    constexpr COLORREF noBorder = static_cast<COLORREF>(0xFFFFFFFE); // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &noBorder, sizeof(noBorder));
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, kDwmTextColor, &textColor, sizeof(textColor));
}

inline void UpdateFramelessWindowRegion(const HWND hwnd,
                                        const UINT dpi,
                                        const bool fullscreen = false) {
    if (!hwnd) return;

    if (fullscreen || IsZoomed(hwnd)) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return;
    const int width = std::max(1L, windowRect.right - windowRect.left);
    const int height = std::max(1L, windowRect.bottom - windowRect.top);
    const int radius = std::max(1, MulDiv(10, dpi > 0 ? static_cast<int>(dpi) : 96, 96));
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
    if (!region) return;
    if (!SetWindowRgn(hwnd, region, TRUE)) {
        DeleteObject(region);
    }
}

}  // namespace anvil::app
