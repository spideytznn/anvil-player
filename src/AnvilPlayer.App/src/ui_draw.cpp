#include "AnvilPlayer/App/ui_draw.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace anvil::app {

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

RECT MakeRect(const int left, const int top, const int right, const int bottom) {
    return RECT{left, top, right, bottom};
}

RECT DeflateRectCopy(RECT rect, const int dx, const int dy) {
    InflateRect(&rect, -dx, -dy);
    return rect;
}

bool ContainsPoint(const RECT& rect, const POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

COLORREF BlendColor(const COLORREF from, const COLORREF to, const double amount) {
    const auto blend = [amount](const int a, const int b) {
        return static_cast<int>(a + (b - a) * amount);
    };
    return RGB(
        blend(GetRValue(from), GetRValue(to)),
        blend(GetGValue(from), GetGValue(to)),
        blend(GetBValue(from), GetBValue(to)));
}

Gdiplus::Color GdiplusColor(const COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    if (rect.Width <= 0.0f || rect.Height <= 0.0f) {
        return;
    }

    radius = std::max(0.0f, std::min(radius, std::min(rect.Width, rect.Height) * 0.5f));
    if (radius <= 0.5f) {
        path.AddRectangle(rect);
        return;
    }

    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(HDC hdc, const RECT& rect, const COLORREF color, const int radius) {
    if (RectWidth(rect) <= 0 || RectHeight(rect) <= 0) {
        return;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path,
                       Gdiplus::RectF(static_cast<float>(rect.left),
                                      static_cast<float>(rect.top),
                                      static_cast<float>(RectWidth(rect)),
                                      static_cast<float>(RectHeight(rect))),
                       static_cast<float>(radius));
    Gdiplus::SolidBrush brush(GdiplusColor(color));
    graphics.FillPath(&brush, &path);
}

void StrokeRoundRect(HDC hdc, const RECT& rect, const COLORREF color, const int radius, const int width) {
    if (RectWidth(rect) <= 0 || RectHeight(rect) <= 0 || width <= 0) {
        return;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    const float strokeWidth = static_cast<float>(width);
    const float halfStroke = strokeWidth * 0.5f;
    Gdiplus::RectF strokeRect(static_cast<float>(rect.left) + halfStroke,
                              static_cast<float>(rect.top) + halfStroke,
                              static_cast<float>(RectWidth(rect)) - strokeWidth,
                              static_cast<float>(RectHeight(rect)) - strokeWidth);
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, strokeRect, std::max(0.0f, static_cast<float>(radius) - halfStroke));
    Gdiplus::Pen pen(GdiplusColor(color), strokeWidth);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &path);
}

void FillRectColor(HDC hdc, const RECT& rect, const COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void DrawLine(HDC hdc, const int x1, const int y1, const int x2, const int y2, const COLORREF color, const int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

HFONT CreateUiFont(const int pixelHeight, const int weight) {
    return CreateFontW(
        -pixelHeight,
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

HFONT CreateBrandFont(const int pixelHeight, const int weight) {
    return CreateFontW(
        -pixelHeight,
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

HFONT CreateMonoFont(const int pixelHeight, const int weight) {
    return CreateFontW(
        -pixelHeight,
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN,
        L"Cascadia Mono");
}

GdiplusSession::GdiplusSession() {
    Gdiplus::GdiplusStartupInput input{};
    status_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
}

GdiplusSession::~GdiplusSession() {
    if (token_ != 0) {
        Gdiplus::GdiplusShutdown(token_);
    }
}

bool GdiplusSession::Ready() const {
    return status_ == Gdiplus::Ok;
}

void DrawTextInRect(HDC hdc,
                    const std::wstring& text,
                    RECT rect,
                    HFONT font,
                    const COLORREF color,
                    const UINT flags) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    DrawTextW(hdc, text.c_str(), -1, &rect, flags);
    SelectObject(hdc, oldFont);
}

std::wstring PercentText(const double value) {
    std::wostringstream stream;
    stream << static_cast<int>(std::clamp(value, 0.0, 1.0) * 100.0 + 0.5) << L"%";
    return stream.str();
}

const wchar_t* IconAssetFileName(const IconKind icon) {
    switch (icon) {
    case IconKind::Folder:
        return L"folder.png";
    case IconKind::Cog:
        return L"settings.png";
    case IconKind::Back10:
        return L"back10.png";
    case IconKind::Play:
        return L"play.png";
    case IconKind::Pause:
        return L"pause.png";
    case IconKind::Stop:
        return L"stop.png";
    case IconKind::Forward10:
        return L"forward10.png";
    case IconKind::VolumeDown:
        return L"volume-down.png";
    case IconKind::VolumeUp:
        return L"volume-up.png";
    case IconKind::Subtitles:
        return L"";
    case IconKind::Fullscreen:
        return L"fullscreen.png";
    case IconKind::Windowed:
        return L"windowed.png";
    case IconKind::ChevronLeft:
    case IconKind::ChevronRight:
        return L"";
    case IconKind::HdrColor:
        return L"hdr-color.png";
    case IconKind::DolbyVisionColor:
        return L"dolby-vision-color.png";
    case IconKind::None:
        return L"";
    }
    return L"";
}

std::wstring FileNameOrDash(const anvil::playback::PlaybackSessionSnapshot& snapshot) {
    if (!snapshot.media.has_value()) {
        return L"-";
    }
    return snapshot.media->displayName;
}

std::wstring JoinStrings(const std::vector<std::wstring>& values, const std::wstring& separator, const std::size_t maxItems) {
    if (values.empty()) {
        return L"-";
    }

    std::wstring joined;
    const std::size_t count = std::min(values.size(), maxItems);
    for (std::size_t index = 0; index < count; ++index) {
        if (index > 0) {
            joined += separator;
        }
        joined += values[index];
    }
    if (values.size() > maxItems) {
        joined += separator + L"...";
    }
    return joined;
}

}  // namespace anvil::app
