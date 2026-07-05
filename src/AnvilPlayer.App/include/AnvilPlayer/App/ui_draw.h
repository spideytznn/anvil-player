#pragma once

#include "AnvilPlayer/App/ui_types.h"
#include "AnvilPlayer/Playback/PlayerController.h"

#include <objidl.h>
#include <windows.h>
#include <gdiplus.h>

#include <cstddef>
#include <string>
#include <vector>

namespace anvil::app {

// UI color theme. Dark window/surfaces with dark-orange accents.
struct Palette {
    COLORREF background = RGB(8, 10, 13);
    COLORREF surface = RGB(15, 17, 22);
    COLORREF surfaceRaised = RGB(23, 26, 32);
    COLORREF surfaceSoft = RGB(31, 35, 43);
    COLORREF viewport = RGB(5, 7, 10);
    COLORREF border = RGB(38, 42, 50);
    COLORREF borderStrong = RGB(62, 67, 78);
    COLORREF accent = RGB(232, 112, 88);
    COLORREF accentDark = RGB(126, 62, 52);
    COLORREF accentSoft = RGB(48, 31, 32);
    COLORREF text = RGB(239, 241, 245);
    COLORREF muted = RGB(164, 168, 179);
    COLORREF dim = RGB(104, 109, 120);
    COLORREF success = RGB(45, 214, 159);
    COLORREF info = RGB(125, 144, 172);
    COLORREF track = RGB(66, 72, 82);
};

// Hit-tested button descriptor used by the layout/paint code.
struct UiButton {
    Command command = Command::Open;
    RECT bounds{};
    std::wstring label;
    std::wstring tooltip;
    IconKind icon = IconKind::None;
    ButtonKind kind = ButtonKind::Icon;
    bool primary = false;
    bool selected = false;
};

// RECT extent helpers.
int RectWidth(const RECT& rect);
int RectHeight(const RECT& rect);
RECT MakeRect(int left, int top, int right, int bottom);
RECT DeflateRectCopy(RECT rect, int dx, int dy);
bool ContainsPoint(const RECT& rect, POINT point);

// Color mixing + COLORREF -> Gdiplus::Color conversion.
COLORREF BlendColor(COLORREF from, COLORREF to, double amount);
Gdiplus::Color GdiplusColor(COLORREF color);

// Rounded-rect GDI+ primitives, reused everywhere in the paint code.
void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius);
void FillRoundRect(HDC hdc, const RECT& rect, COLORREF color, int radius);
void StrokeRoundRect(HDC hdc, const RECT& rect, COLORREF color, int radius, int width = 1);

// Plain GDI helpers.
void FillRectColor(HDC hdc, const RECT& rect, COLORREF color);
void DrawLine(HDC hdc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1);

// Font factories (Segoe UI / Cascadia Mono).
HFONT CreateUiFont(int pixelHeight, int weight = FW_NORMAL);
HFONT CreateBrandFont(int pixelHeight, int weight = FW_SEMIBOLD);
HFONT CreateMonoFont(int pixelHeight, int weight = FW_NORMAL);

// RAII for Gdiplus::GdiplusStartup/GdiplusShutdown. One instance per process.
class GdiplusSession {
public:
    GdiplusSession();
    ~GdiplusSession();

    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

    bool Ready() const;

private:
    ULONG_PTR token_ = 0;
    Gdiplus::Status status_ = Gdiplus::GenericError;
};

// Wraps DrawTextW with transparent BkMode.
void DrawTextInRect(HDC hdc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags);

// Formats a 0..1 ratio as "nn%".
std::wstring PercentText(double value);

// Maps an icon kind to its PNG filename (or L"" for None).
const wchar_t* IconAssetFileName(IconKind icon);

// Returns the snapshot display name, or "-" when no media is loaded.
std::wstring FileNameOrDash(const anvil::playback::PlaybackSessionSnapshot& snapshot);

// Truncating joiner for capability lists; appends "..." when truncated.
std::wstring JoinStrings(const std::vector<std::wstring>& values, const std::wstring& separator, std::size_t maxItems = 4);

}  // namespace anvil::app
