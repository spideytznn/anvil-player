#include "AnvilPlayer/App/icon_painter.h"

#include <algorithm>
#include <cmath>
#include <system_error>

namespace anvil::app {

std::filesystem::path IconPainter::ModuleDirectory() const {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(modulePath).parent_path();
}

const std::filesystem::path& IconPainter::IconAssetDirectory() const {
    if (!iconAssetDirectory_.empty()) {
        return iconAssetDirectory_;
    }

    const auto moduleDir = ModuleDirectory();
    std::vector<std::filesystem::path> candidates;
    if (!moduleDir.empty()) {
        candidates.push_back(moduleDir / L"assets" / L"icons");
        candidates.push_back(moduleDir.parent_path() / L"assets" / L"icons");
        candidates.push_back(moduleDir.parent_path().parent_path() / L"src" / L"AnvilPlayer.App" / L"assets" / L"icons");
    }
    const auto current = std::filesystem::current_path();
    candidates.push_back(current / L"assets" / L"icons");
    candidates.push_back(current / L"src" / L"AnvilPlayer.App" / L"assets" / L"icons");

    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate / L"play.png", error)) {
            iconAssetDirectory_ = candidate;
            break;
        }
    }
    return iconAssetDirectory_;
}

Gdiplus::Bitmap* IconPainter::LoadIconBitmap(const IconKind icon) const {
    for (const auto& cached : iconBitmaps_) {
        if (cached.first == icon) {
            return cached.second.get();
        }
    }

    const wchar_t* fileName = IconAssetFileName(icon);
    if (fileName[0] == L'\0') {
        return nullptr;
    }

    const auto& assetDirectory = IconAssetDirectory();
    if (assetDirectory.empty()) {
        return nullptr;
    }

    const auto assetPath = assetDirectory / fileName;
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(assetPath.c_str());
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok || bitmap->GetWidth() == 0 || bitmap->GetHeight() == 0) {
        return nullptr;
    }

    Gdiplus::Bitmap* raw = bitmap.get();
    iconBitmaps_.push_back(std::make_pair(icon, std::move(bitmap)));
    return raw;
}

bool IconPainter::DrawAsset(HDC hdc, const IconKind icon, RECT bounds) const {
    Gdiplus::Bitmap* bitmap = LoadIconBitmap(icon);
    if (!bitmap) {
        return false;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const int sourceWidth = static_cast<int>(bitmap->GetWidth());
    const int sourceHeight = static_cast<int>(bitmap->GetHeight());
    const int targetWidth = RectWidth(bounds);
    const int targetHeight = RectHeight(bounds);
    if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
        return false;
    }

    const float scale = std::min(static_cast<float>(targetWidth) / static_cast<float>(sourceWidth),
                                 static_cast<float>(targetHeight) / static_cast<float>(sourceHeight));
    const int drawWidth = std::max(1, static_cast<int>(std::round(sourceWidth * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::round(sourceHeight * scale)));
    const int drawLeft = bounds.left + (targetWidth - drawWidth) / 2;
    const int drawTop = bounds.top + (targetHeight - drawHeight) / 2;
    const Gdiplus::Rect destination(drawLeft, drawTop, drawWidth, drawHeight);

    graphics.DrawImage(bitmap, destination, 0, 0, sourceWidth, sourceHeight, Gdiplus::UnitPixel);
    return true;
}

bool IconPainter::Draw(HDC hdc, const IconKind icon, RECT bounds, const COLORREF color) const {
    if (DrawAsset(hdc, icon, bounds)) {
        return true;
    }
    DrawVector(hdc, icon, bounds, color);
    return false;
}

void IconPainter::DrawVector(HDC hdc, const IconKind icon, RECT bounds, const COLORREF color) const {
    const int width = RectWidth(bounds);
    const int height = RectHeight(bounds);
    const int cx = bounds.left + width / 2;
    const int cy = bounds.top + height / 2;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const float unit = static_cast<float>(std::min(width, height)) / 24.0f;
    const Gdiplus::Color iconColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
    Gdiplus::Pen pen(iconColor, std::max(1.0f, std::round(unit * 1.9f)));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(iconColor);

    auto x = [&](const float value) {
        return static_cast<Gdiplus::REAL>(cx + value * unit);
    };
    auto y = [&](const float value) {
        return static_cast<Gdiplus::REAL>(cy + value * unit);
    };
    auto p = [&](const float pointX, const float pointY) {
        return Gdiplus::PointF(x(pointX), y(pointY));
    };
    auto rect = [&](const float left, const float top, const float rectWidth, const float rectHeight) {
        return Gdiplus::RectF(x(left), y(top), rectWidth * unit, rectHeight * unit);
    };
    auto addRoundedRect = [&](Gdiplus::GraphicsPath& path, const Gdiplus::RectF& roundedRect, const float radius) {
        const float diameter = radius * 2.0f;
        path.AddArc(roundedRect.X, roundedRect.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(roundedRect.X + roundedRect.Width - diameter, roundedRect.Y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(roundedRect.X + roundedRect.Width - diameter,
                    roundedRect.Y + roundedRect.Height - diameter,
                    diameter,
                    diameter,
                    0.0f,
                    90.0f);
        path.AddArc(roundedRect.X, roundedRect.Y + roundedRect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
    };
    auto strokeRoundedRect = [&](const Gdiplus::RectF& roundedRect, const float radius) {
        Gdiplus::GraphicsPath path;
        addRoundedRect(path, roundedRect, radius);
        graphics.DrawPath(&pen, &path);
    };

    switch (icon) {
    case IconKind::Folder: {
        Gdiplus::GraphicsPath path;
        path.StartFigure();
        path.AddLine(p(-10.0f, -5.0f), p(-5.3f, -5.0f));
        path.AddLine(p(-5.3f, -5.0f), p(-3.1f, -7.4f));
        path.AddLine(p(-3.1f, -7.4f), p(2.7f, -7.4f));
        path.AddLine(p(2.7f, -7.4f), p(5.1f, -5.0f));
        path.AddLine(p(5.1f, -5.0f), p(10.0f, -5.0f));
        path.AddLine(p(10.0f, -5.0f), p(10.0f, 8.0f));
        path.AddLine(p(10.0f, 8.0f), p(-10.0f, 8.0f));
        path.CloseFigure();
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, p(-10.0f, -1.2f), p(10.0f, -1.2f));
        break;
    }
    case IconKind::Cog: {
        graphics.DrawLine(&pen, p(-9.5f, -6.5f), p(9.5f, -6.5f));
        graphics.DrawLine(&pen, p(-9.5f, 0.0f), p(9.5f, 0.0f));
        graphics.DrawLine(&pen, p(-9.5f, 6.5f), p(9.5f, 6.5f));
        graphics.FillEllipse(&brush, rect(-5.5f, -9.0f, 5.0f, 5.0f));
        graphics.FillEllipse(&brush, rect(3.0f, -2.5f, 5.0f, 5.0f));
        graphics.FillEllipse(&brush, rect(-7.5f, 4.0f, 5.0f, 5.0f));
        break;
    }
    case IconKind::Back10:
    case IconKind::Forward10: {
        const bool back = icon == IconKind::Back10;
        graphics.DrawArc(&pen, rect(-8.2f, -8.2f, 16.4f, 16.4f), back ? 118.0f : -62.0f, back ? 255.0f : 255.0f);
        if (back) {
            graphics.DrawLine(&pen, p(-6.4f, -8.3f), p(-11.5f, -8.3f));
            graphics.DrawLine(&pen, p(-11.5f, -8.3f), p(-11.5f, -3.2f));
        } else {
            graphics.DrawLine(&pen, p(6.4f, -8.3f), p(11.5f, -8.3f));
            graphics.DrawLine(&pen, p(11.5f, -8.3f), p(11.5f, -3.2f));
        }

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font numberFont(&family, 8.0f * unit, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(L"10",
                            -1,
                            &numberFont,
                            rect(-6.0f, -5.8f, 12.0f, 12.0f),
                            &format,
                            &brush);
        break;
    }
    case IconKind::Play: {
        Gdiplus::GraphicsPath path;
        path.AddLine(p(-5.2f, -8.8f), p(-5.2f, 8.8f));
        path.AddLine(p(-5.2f, 8.8f), p(9.5f, 0.0f));
        path.CloseFigure();
        graphics.DrawPath(&pen, &path);
        break;
    }
    case IconKind::Pause:
        strokeRoundedRect(rect(-7.4f, -8.8f, 4.8f, 17.6f), 1.8f * unit);
        strokeRoundedRect(rect(2.6f, -8.8f, 4.8f, 17.6f), 1.8f * unit);
        break;
    case IconKind::Stop:
        strokeRoundedRect(rect(-7.2f, -7.2f, 14.4f, 14.4f), 2.0f * unit);
        break;
    case IconKind::VolumeDown:
    case IconKind::VolumeUp: {
        Gdiplus::GraphicsPath speakerPath;
        Gdiplus::PointF speakerPoints[] = {
            p(-11.0f, -4.8f),
            p(-5.8f, -4.8f),
            p(1.5f, -10.2f),
            p(1.5f, 10.2f),
            p(-5.8f, 4.8f),
            p(-11.0f, 4.8f),
        };
        speakerPath.AddPolygon(speakerPoints, static_cast<int>(std::size(speakerPoints)));
        graphics.DrawPath(&pen, &speakerPath);
        graphics.DrawArc(&pen, rect(2.0f, -6.3f, 7.8f, 12.6f), -43.0f, 86.0f);
        if (icon == IconKind::VolumeUp) {
            graphics.DrawArc(&pen, rect(4.8f, -9.4f, 10.5f, 18.8f), -45.0f, 90.0f);
        }
        break;
    }
    case IconKind::Subtitles:
        strokeRoundedRect(rect(-10.0f, -7.0f, 20.0f, 14.0f), 2.2f * unit);
        graphics.DrawLine(&pen, p(-6.0f, -1.8f), p(-0.8f, -1.8f));
        graphics.DrawLine(&pen, p(2.5f, -1.8f), p(7.0f, -1.8f));
        graphics.DrawLine(&pen, p(-6.0f, 4.2f), p(1.2f, 4.2f));
        graphics.DrawLine(&pen, p(4.0f, 4.2f), p(7.0f, 4.2f));
        break;
    case IconKind::Fullscreen: {
        graphics.DrawLine(&pen, p(-2.5f, -2.5f), p(-9.2f, -9.2f));
        graphics.DrawLine(&pen, p(-9.2f, -9.2f), p(-9.2f, -4.2f));
        graphics.DrawLine(&pen, p(-9.2f, -9.2f), p(-4.2f, -9.2f));
        graphics.DrawLine(&pen, p(2.5f, -2.5f), p(9.2f, -9.2f));
        graphics.DrawLine(&pen, p(9.2f, -9.2f), p(9.2f, -4.2f));
        graphics.DrawLine(&pen, p(9.2f, -9.2f), p(4.2f, -9.2f));
        graphics.DrawLine(&pen, p(2.5f, 2.5f), p(9.2f, 9.2f));
        graphics.DrawLine(&pen, p(9.2f, 9.2f), p(9.2f, 4.2f));
        graphics.DrawLine(&pen, p(9.2f, 9.2f), p(4.2f, 9.2f));
        graphics.DrawLine(&pen, p(-2.5f, 2.5f), p(-9.2f, 9.2f));
        graphics.DrawLine(&pen, p(-9.2f, 9.2f), p(-9.2f, 4.2f));
        graphics.DrawLine(&pen, p(-9.2f, 9.2f), p(-4.2f, 9.2f));
        break;
    }
    case IconKind::Windowed:
        strokeRoundedRect(rect(-9.4f, -4.0f, 14.6f, 13.4f), 2.0f * unit);
        strokeRoundedRect(rect(-3.5f, -9.4f, 13.0f, 12.6f), 2.0f * unit);
        break;
    case IconKind::ChevronLeft:
        graphics.DrawLine(&pen, p(5.0f, -8.5f), p(-4.0f, 0.0f));
        graphics.DrawLine(&pen, p(-4.0f, 0.0f), p(5.0f, 8.5f));
        break;
    case IconKind::ChevronRight:
        graphics.DrawLine(&pen, p(-5.0f, -8.5f), p(4.0f, 0.0f));
        graphics.DrawLine(&pen, p(4.0f, 0.0f), p(-5.0f, 8.5f));
        break;
    case IconKind::None:
        break;
    }
}

}  // namespace anvil::app
