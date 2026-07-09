#pragma once

// Shared RECT geometry helpers used across the main window implementation
// files. main_window.cpp and main_window_paint.cpp each carried a copy of
// RectsIntersect (main_window.cpp via HasArea, paint.cpp inline); these are
// behaviourally identical, so they collapse to one definition here.

#include "AnvilPlayer/App/ui_draw.h"

namespace anvil::app {

// True when the rect has positive width and height.
inline bool HasArea(const RECT& rect) {
    return RectWidth(rect) > 0 && RectHeight(rect) > 0;
}

// Memberwise equality.
inline bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// True when both rects have area and their intersection is non-empty.
inline bool RectsIntersect(const RECT& a, const RECT& b) {
    RECT intersection{};
    return HasArea(a) && HasArea(b) && IntersectRect(&intersection, &a, &b) != FALSE;
}

// Returns a copy of rect inflated by dx/dy (matches Win32 InflateRect semantics).
inline RECT InflateRectCopy(RECT rect, const int dx, const int dy) {
    InflateRect(&rect, dx, dy);
    return rect;
}

// Invalidates a rect region of a window if the rect has area, padded outward
// by `padding` pixels. No-op for empty rects or null windows.
inline void InvalidateIfVisible(HWND hwnd, RECT rect, const int padding) {
    if (!hwnd || !HasArea(rect)) {
        return;
    }
    rect = InflateRectCopy(rect, padding, padding);
    InvalidateRect(hwnd, &rect, FALSE);
}

}  // namespace anvil::app
