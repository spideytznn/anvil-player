#include "AnvilPlayer/App/main_window.h"

#include "AnvilPlayer/App/hdr_tone_curve_math.h"
#include "AnvilPlayer/App/rect_util.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <string>
#include <system_error>

namespace anvil::app {

using anvil::playback::CapabilityReport;
using anvil::playback::FormatTimecode;
using anvil::playback::PlaybackSessionSnapshot;
using anvil::playback::PlaybackState;
using anvil::playback::PlayerSettings;
using anvil::playback::ToDisplayString;

namespace {

constexpr auto kScrollbarVisibleDuration = std::chrono::milliseconds{650};
constexpr auto kScrollbarFadeDuration = std::chrono::milliseconds{300};

double ScrollbarOpacity(const std::chrono::steady_clock::time_point lastActiveAt,
                        const bool dragging,
                        const std::chrono::steady_clock::time_point now) {
    if (dragging) {
        return 1.0;
    }
    if (lastActiveAt.time_since_epoch().count() == 0) {
        return 0.0;
    }
    const auto age = now - lastActiveAt;
    if (age < kScrollbarVisibleDuration) {
        return 1.0;
    }
    if (age >= kScrollbarVisibleDuration + kScrollbarFadeDuration) {
        return 0.0;
    }
    const double t = static_cast<double>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             age - kScrollbarVisibleDuration).count()) /
                     static_cast<double>(kScrollbarFadeDuration.count());
    return 1.0 - std::clamp(t, 0.0, 1.0);
}

COLORREF FadeForOpacity(const COLORREF color, const double opacity) {
    return BlendColor(RGB(0, 0, 0), color, std::clamp(opacity, 0.0, 1.0));
}

void DrawSurfaceRim(HDC hdc,
                    const RECT& rect,
                    const COLORREF topColor,
                    const COLORREF coolColor,
                    const COLORREF warmColor,
                    const int inset,
                    const int width = 1) {
    if (RectWidth(rect) <= inset * 2 || RectHeight(rect) <= inset * 2) {
        return;
    }

    DrawLine(hdc, rect.left + inset, rect.top + width, rect.right - inset, rect.top + width, topColor, width);
    DrawLine(hdc, rect.left + width, rect.top + inset, rect.left + width, rect.bottom - inset, coolColor, width);
    DrawLine(hdc, rect.right - width, rect.top + inset, rect.right - width, rect.bottom - inset, warmColor, width);
}

std::wstring PlaybackRateText(const double rate) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << rate << L"x";
    return stream.str();
}

std::wstring NetworkSpeedText(const uint64_t bytesPerSecond) {
    const uint64_t kbps = bytesPerSecond / 1024;
    return std::to_wstring(kbps) + L" KB/s";
}

std::wstring SubtitleSelectionText(const int selectedTrackIndex) {
    if (selectedTrackIndex == anvil::playback::kSubtitleTrackAuto) {
        return L"Auto";
    }
    if (selectedTrackIndex == anvil::playback::kSubtitleTrackOff) {
        return L"Off";
    }
    return L"Stream " + std::to_wstring(selectedTrackIndex);
}

std::wstring LowercaseCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring UppercaseCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    return value;
}

std::wstring SubtitleLanguageName(const std::wstring& language) {
    if (language.empty() || language == L"-") {
        return {};
    }

    const std::wstring normalized = LowercaseCopy(language);
    if (normalized == L"chi" || normalized == L"zho" || normalized == L"zh" ||
        normalized.rfind(L"zh-", 0) == 0 ||
        normalized == L"chs" || normalized == L"cht" || normalized == L"cmn" || normalized == L"cn" ||
        normalized.find(L"chinese") != std::wstring::npos) {
        if (normalized.find(L"trad") != std::wstring::npos ||
            normalized.find(L"hant") != std::wstring::npos ||
            normalized.find(L"tw") != std::wstring::npos ||
            normalized == L"cht") {
            return L"Chinese Traditional";
        }
        if (normalized.find(L"simp") != std::wstring::npos ||
            normalized.find(L"hans") != std::wstring::npos ||
            normalized.find(L"cn") != std::wstring::npos ||
            normalized == L"chs") {
            return L"Chinese Simplified";
        }
        return L"Chinese";
    }
    if (normalized == L"eng" || normalized == L"en") {
        return L"English";
    }
    if (normalized == L"jpn" || normalized == L"ja") {
        return L"Japanese";
    }
    if (normalized == L"kor" || normalized == L"ko") {
        return L"Korean";
    }
    if (normalized == L"fre" || normalized == L"fra" || normalized == L"fr") {
        return L"French";
    }
    if (normalized == L"ger" || normalized == L"deu" || normalized == L"de") {
        return L"German";
    }
    if (normalized == L"spa" || normalized == L"es") {
        return L"Spanish";
    }
    return language;
}

std::wstring SubtitleMenuPrimaryLabel(const int selection, const std::optional<anvil::playback::MediaDescriptor>& media) {
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
        const std::wstring language = SubtitleLanguageName(stream.language);
        if (!language.empty()) {
            label = language;
        }
        break;
    }
    return label;
}

std::wstring SubtitleMenuCodecLabel(const int selection, const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (selection < 0 || !media.has_value()) {
        return {};
    }

    for (const auto& stream : media->streams) {
        if (stream.kind == L"Subtitle" && stream.index == selection) {
            return UppercaseCopy(stream.codec);
        }
    }
    return {};
}

std::wstring AudioMenuPrimaryLabel(const int selection, const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (selection == anvil::playback::kAudioTrackAuto) {
        return L"Auto";
    }
    if (selection == anvil::playback::kAudioTrackOff) {
        return L"Off";
    }

    std::wstring label = L"Stream " + std::to_wstring(selection);
    if (!media.has_value()) {
        return label;
    }
    for (const auto& stream : media->streams) {
        if (stream.kind != L"Audio" || stream.index != selection) {
            continue;
        }
        const std::wstring language = SubtitleLanguageName(stream.language);
        if (!language.empty()) {
            label = language;
        }
        break;
    }
    return label;
}

std::wstring AudioMenuCodecLabel(const int selection, const std::optional<anvil::playback::MediaDescriptor>& media) {
    if (selection < 0 || !media.has_value()) {
        return {};
    }

    for (const auto& stream : media->streams) {
        if (stream.kind == L"Audio" && stream.index == selection) {
            return UppercaseCopy(stream.codec);
        }
    }
    return {};
}

std::wstring SubtitleDelayText(const int delayMs) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << static_cast<double>(delayMs) / 1000.0 << L" s";
    return stream.str();
}

std::wstring PercentTextFromInt(const int value) {
    return std::to_wstring(value) + L"%";
}

std::wstring SubtitleScaleText(const double scale) {
    return std::to_wstring(static_cast<int>(std::round(scale * 100.0))) + L"%";
}

std::wstring PixelOffsetText(const int value) {
    return (value > 0 ? L"+" : L"") + std::to_wstring(value) + L" px";
}

std::wstring DanmakuModeText(const int mode) {
    switch (mode) {
    case 1: return L"Top";
    case 2: return L"Bottom";
    default: return L"Scroll";
    }
}

void DrawCheckMark(HDC hdc, const POINT center, const int size, const COLORREF color) {
    const int width = std::max(1, size / 7);
    DrawLine(hdc,
             center.x - size / 2,
             center.y,
             center.x - size / 8,
             center.y + size / 3,
             color,
             width);
    DrawLine(hdc,
             center.x - size / 8,
             center.y + size / 3,
             center.x + size / 2,
             center.y - size / 3,
             color,
             width);
}

void DrawPlusMark(HDC hdc, const POINT center, const int size, const COLORREF color) {
    const int width = std::max(1, size / 8);
    DrawLine(hdc, center.x - size / 2, center.y, center.x + size / 2, center.y, color, width);
    DrawLine(hdc, center.x, center.y - size / 2, center.x, center.y + size / 2, color, width);
}

std::wstring VideoSelectionText(const int selectedTrackIndex) {
    if (selectedTrackIndex == anvil::playback::kVideoTrackAuto) {
        return L"Auto";
    }
    if (selectedTrackIndex == anvil::playback::kVideoTrackDolbyVisionEnhancement) {
        return L"DV EL";
    }
    return L"Stream " + std::to_wstring(selectedTrackIndex);
}

std::wstring NitsLabel(const double nits) {
    return std::to_wstring(static_cast<int>(std::round(nits))) + L" nits";
}

std::wstring ToneCurveAxisLabel(const double nits) {
    if (nits >= 1000.0) {
        return std::to_wstring(static_cast<int>(nits / 1000.0)) + L"k";
    }
    return std::to_wstring(static_cast<int>(nits));
}

std::filesystem::path ComparablePath(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

}  // namespace

void MainWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC windowDc = BeginPaint(hwnd_, &paint);
    EnsureLayout();

    RECT paintRect = paint.rcPaint;
    if (RectWidth(paintRect) <= 0 || RectHeight(paintRect) <= 0) {
        EndPaint(hwnd_, &paint);
        return;
    }

    HDC bufferDc = CreateCompatibleDC(windowDc);
    HBITMAP bufferBitmap = CreateCompatibleBitmap(windowDc, std::max(1, RectWidth(paintRect)), std::max(1, RectHeight(paintRect)));
    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    POINT oldOrigin{};
    SetViewportOrgEx(bufferDc, -paintRect.left, -paintRect.top, &oldOrigin);
    SetBkMode(bufferDc, TRANSPARENT);

    HBRUSH background = CreateSolidBrush(palette_.background);
    FillRect(bufferDc, &paintRect, background);
    DeleteObject(background);

    const auto snapshot = controller_.Snapshot();
    const auto settings = controller_.Settings();
    const auto& capabilities = CachedCapabilities();

    if (webUiActive_) {
        BitBlt(windowDc, paintRect.left, paintRect.top, RectWidth(paintRect), RectHeight(paintRect), bufferDc, 0, 0, SRCCOPY);
        SetViewportOrgEx(bufferDc, oldOrigin.x, oldOrigin.y, nullptr);
        SelectObject(bufferDc, oldBitmap);
        DeleteObject(bufferBitmap);
        DeleteDC(bufferDc);
        EndPaint(hwnd_, &paint);
        return;
    }

    if (RectWidth(topBar_) > 0 && RectHeight(topBar_) > 0 && RectsIntersect(paintRect, topBar_)) {
        DrawTopBar(bufferDc, snapshot);
    }
    if (RectWidth(videoSurface_) > 0 && RectHeight(videoSurface_) > 0 && RectsIntersect(paintRect, videoSurface_)) {
        DrawVideoSurface(bufferDc, snapshot, paintRect);
    }
    if (!fullscreen_ &&
        RectWidth(transportBar_) > 0 &&
        RectHeight(transportBar_) > 0 &&
        RectsIntersect(paintRect, transportBar_)) {
        DrawTransport(bufferDc, snapshot);
    }
    if (RectWidth(inspector_) > 0 && RectHeight(inspector_) > 0 && RectsIntersect(paintRect, inspector_)) {
        DrawInspectorPanel(bufferDc, snapshot, settings, capabilities);
    }
    if (!fullscreen_ &&
        (RectsIntersect(paintRect, topBar_) ||
         RectsIntersect(paintRect, transportBar_) ||
         RectsIntersect(paintRect, inspector_))) {
        DrawButtons(bufferDc, snapshot);
        DrawTooltip(bufferDc);
    }

    BitBlt(windowDc, paintRect.left, paintRect.top, RectWidth(paintRect), RectHeight(paintRect), bufferDc, 0, 0, SRCCOPY);
    SetViewportOrgEx(bufferDc, oldOrigin.x, oldOrigin.y, nullptr);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
    EndPaint(hwnd_, &paint);
}

void MainWindow::PaintFullscreenOverlay(HWND overlay) {
    PAINTSTRUCT paint{};
    HDC windowDc = BeginPaint(overlay, &paint);

    RECT paintRect = paint.rcPaint;
    if (RectWidth(paintRect) <= 0 || RectHeight(paintRect) <= 0) {
        EndPaint(overlay, &paint);
        return;
    }

    HDC bufferDc = CreateCompatibleDC(windowDc);
    HBITMAP bufferBitmap = CreateCompatibleBitmap(windowDc, std::max(1, RectWidth(paintRect)), std::max(1, RectHeight(paintRect)));
    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    POINT oldPaintOrigin{};
    SetViewportOrgEx(bufferDc, -paintRect.left, -paintRect.top, &oldPaintOrigin);
    SetBkMode(bufferDc, TRANSPARENT);

    HBRUSH background = CreateSolidBrush(fullscreen_ ? RGB(0, 0, 0) : palette_.background);
    FillRect(bufferDc, &paintRect, background);
    DeleteObject(background);

    POINT overlayOrigin{};
    RECT overlayWindow{};
    if (GetWindowRect(overlay, &overlayWindow)) {
        overlayOrigin = POINT{overlayWindow.left, overlayWindow.top};
        ScreenToClient(hwnd_, &overlayOrigin);
    } else {
        overlayOrigin = POINT{transportBar_.left, transportBar_.top};
    }

    POINT oldOrigin{};
    SetViewportOrgEx(bufferDc, -overlayOrigin.x - paintRect.left, -overlayOrigin.y - paintRect.top, &oldOrigin);

    const auto snapshot = controller_.Snapshot();
    DrawTransport(bufferDc, snapshot);
    DrawButtons(bufferDc, snapshot);

    SetViewportOrgEx(bufferDc, oldOrigin.x, oldOrigin.y, nullptr);
    BitBlt(windowDc, paintRect.left, paintRect.top, RectWidth(paintRect), RectHeight(paintRect), bufferDc, 0, 0, SRCCOPY);

    SetViewportOrgEx(bufferDc, oldPaintOrigin.x, oldPaintOrigin.y, nullptr);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
    EndPaint(overlay, &paint);
}

void MainWindow::PaintTransportOverlay(HWND overlay) {
    PaintFullscreenOverlay(overlay);
}

void MainWindow::RenderBufferingHudOverlay(const NativeVideoQueueStats& stats) {
    if (!bufferingHudOverlay_) {
        return;
    }

    RECT client{};
    GetClientRect(bufferingHudOverlay_, &client);
    const int width = RectWidth(client);
    const int height = RectHeight(client);
    if (width <= 0 || height <= 0) {
        return;
    }

    RECT windowRect{};
    if (!GetWindowRect(bufferingHudOverlay_, &windowRect)) {
        return;
    }

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return;
    }

    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (!memoryDc) {
        ReleaseDC(nullptr, screenDc);
        return;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    std::memset(pixels, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    {
        constexpr int kRenderScale = 3;
        Gdiplus::Bitmap hudBitmap(width * kRenderScale, height * kRenderScale, PixelFormat32bppPARGB);
        Gdiplus::Graphics graphics(&hudBitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.ScaleTransform(static_cast<Gdiplus::REAL>(kRenderScale),
                                static_cast<Gdiplus::REAL>(kRenderScale));

        const int spinnerRoom = std::max(Scale(32), height - Scale(52));
        const int spinnerSize = std::clamp(spinnerRoom, Scale(38), Scale(48));
        const int gap = Scale(12);
        const int textHeight = Scale(22);
        const int totalHeight = spinnerSize + gap + textHeight;
        const int centerX = width / 2;
        const int groupTop = std::max(Scale(10), (height - totalHeight) / 2);
        const int spinnerLeft = centerX - spinnerSize / 2;
        const int spinnerTop = groupTop;
        const int penWidth = std::max(2, Scale(3));
        const Gdiplus::REAL inset = static_cast<Gdiplus::REAL>(penWidth) * 0.5f + 1.0f;
        const Gdiplus::RectF arcRect(static_cast<Gdiplus::REAL>(spinnerLeft) + inset,
                                     static_cast<Gdiplus::REAL>(spinnerTop) + inset,
                                     static_cast<Gdiplus::REAL>(spinnerSize) - inset * 2.0f,
                                     static_cast<Gdiplus::REAL>(spinnerSize) - inset * 2.0f);

        const COLORREF spinnerAccent = BlendColor(palette_.accent, RGB(255, 222, 214), 0.22);
        Gdiplus::Pen track(Gdiplus::Color(38, 255, 255, 255), static_cast<Gdiplus::REAL>(penWidth));
        track.SetStartCap(Gdiplus::LineCapRound);
        track.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawArc(&track, arcRect, 0.0f, 360.0f);

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const double elapsedMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        const double headAngle = std::fmod(elapsedMs * 0.31, 360.0);
        const double tailDegrees = 226.0;
        const int segmentCount = 28;
        for (int index = 0; index < segmentCount; ++index) {
            const double t = static_cast<double>(index + 1) / static_cast<double>(segmentCount);
            const double eased = t * t;
            const double segmentStart = headAngle - tailDegrees + tailDegrees * static_cast<double>(index) /
                                                                     static_cast<double>(segmentCount);
            const double segmentSpan = tailDegrees / static_cast<double>(segmentCount) * 0.94;
            const BYTE alpha = static_cast<BYTE>(std::clamp(14.0 + eased * 205.0, 0.0, 255.0));
            const COLORREF segmentColor = BlendColor(spinnerAccent, RGB(255, 238, 232), 0.10 + t * 0.18);
            Gdiplus::Pen segmentPen(
                Gdiplus::Color(alpha, GetRValue(segmentColor), GetGValue(segmentColor), GetBValue(segmentColor)),
                static_cast<Gdiplus::REAL>(penWidth) * static_cast<Gdiplus::REAL>(0.78 + t * 0.12));
            segmentPen.SetStartCap(Gdiplus::LineCapRound);
            segmentPen.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawArc(&segmentPen,
                             arcRect,
                             static_cast<Gdiplus::REAL>(segmentStart),
                             static_cast<Gdiplus::REAL>(segmentSpan));
        }

        const int horizontalInset = Scale(24);
        const int textWidth = std::max(1, std::min(Scale(260), width - horizontalInset * 2));
        const Gdiplus::RectF textRect(static_cast<Gdiplus::REAL>(centerX - textWidth / 2),
                                      static_cast<Gdiplus::REAL>(spinnerTop + spinnerSize + gap),
                                      static_cast<Gdiplus::REAL>(textWidth),
                                      static_cast<Gdiplus::REAL>(textHeight));
        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font detailFont(&fontFamily,
                                 static_cast<Gdiplus::REAL>(Scale(13)),
                                 Gdiplus::FontStyleBold,
                                 Gdiplus::UnitPixel);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(245, 238, 242, 249));
        const std::wstring speedText = NetworkSpeedText(stats.networkBytesPerSecond);
        graphics.DrawString(speedText.c_str(), -1, &detailFont, textRect, &format, &textBrush);

        Gdiplus::Graphics outputGraphics(memoryDc);
        outputGraphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        outputGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        outputGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        outputGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::Rect destinationRect(0, 0, width, height);
        outputGraphics.DrawImage(&hudBitmap,
                                 destinationRect,
                                 0,
                                 0,
                                 width * kRenderScale,
                                 height * kRenderScale,
                                 Gdiplus::UnitPixel);
    }

    POINT destination{windowRect.left, windowRect.top};
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(bufferingHudOverlay_, screenDc, &destination, &size, memoryDc, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
}

void MainWindow::PaintSubtitleMenuOverlay(HWND overlay) {
    PAINTSTRUCT paint{};
    HDC windowDc = BeginPaint(overlay, &paint);

    RECT paintRect = paint.rcPaint;
    if (RectWidth(paintRect) <= 0 || RectHeight(paintRect) <= 0) {
        EndPaint(overlay, &paint);
        return;
    }

    HDC bufferDc = CreateCompatibleDC(windowDc);
    HBITMAP bufferBitmap = CreateCompatibleBitmap(windowDc, std::max(1, RectWidth(paintRect)), std::max(1, RectHeight(paintRect)));
    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    POINT oldPaintOrigin{};
    SetViewportOrgEx(bufferDc, -paintRect.left, -paintRect.top, &oldPaintOrigin);
    SetBkMode(bufferDc, TRANSPARENT);

    POINT overlayOrigin{};
    RECT overlayWindow{};
    if (GetWindowRect(overlay, &overlayWindow)) {
        overlayOrigin = POINT{overlayWindow.left, overlayWindow.top};
        ScreenToClient(hwnd_, &overlayOrigin);
    } else {
        overlayOrigin = POINT{subtitleMenu_.left, subtitleMenu_.top};
    }

    POINT oldOrigin{};
    SetViewportOrgEx(bufferDc, -overlayOrigin.x - paintRect.left, -overlayOrigin.y - paintRect.top, &oldOrigin);

    const auto snapshot = controller_.Snapshot();
    DrawSubtitleMenu(bufferDc, snapshot);

    SetViewportOrgEx(bufferDc, oldOrigin.x, oldOrigin.y, nullptr);
    BitBlt(windowDc, paintRect.left, paintRect.top, RectWidth(paintRect), RectHeight(paintRect), bufferDc, 0, 0, SRCCOPY);

    SetViewportOrgEx(bufferDc, oldPaintOrigin.x, oldPaintOrigin.y, nullptr);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
    EndPaint(overlay, &paint);
}

void MainWindow::PaintHdrToneCurveWindow(HWND window) {
    PAINTSTRUCT paint{};
    HDC windowDc = BeginPaint(window, &paint);
    UpdateHdrToneCurveFloatingLayout();

    RECT paintRect = paint.rcPaint;
    if (RectWidth(paintRect) <= 0 || RectHeight(paintRect) <= 0) {
        EndPaint(window, &paint);
        return;
    }

    HDC bufferDc = CreateCompatibleDC(windowDc);
    HBITMAP bufferBitmap = CreateCompatibleBitmap(windowDc, std::max(1, RectWidth(paintRect)), std::max(1, RectHeight(paintRect)));
    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    POINT oldOrigin{};
    SetViewportOrgEx(bufferDc, -paintRect.left, -paintRect.top, &oldOrigin);
    SetBkMode(bufferDc, TRANSPARENT);

    DrawHdrToneCurveExpandedEditor(bufferDc, controller_.Settings());

    BitBlt(windowDc, paintRect.left, paintRect.top, RectWidth(paintRect), RectHeight(paintRect), bufferDc, 0, 0, SRCCOPY);
    SetViewportOrgEx(bufferDc, oldOrigin.x, oldOrigin.y, nullptr);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
    EndPaint(window, &paint);
}

void MainWindow::DrawTopBar(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
    FillRoundRect(hdc, topBar_, palette_.surface, Scale(12));
    StrokeRoundRect(hdc, topBar_, palette_.border, Scale(12));
    DrawSurfaceRim(hdc,
                   topBar_,
                   BlendColor(palette_.surface, palette_.text, 0.10),
                   BlendColor(palette_.border, palette_.info, 0.22),
                   BlendColor(palette_.border, palette_.accent, 0.16),
                   Scale(14));

    HFONT markFont = CreateUiFont(Scale(17), FW_BOLD);
    HFONT brandFont = CreateUiFont(Scale(15), FW_SEMIBOLD);
    HFONT bodyFont = CreateUiFont(Scale(12), FW_NORMAL);
    HFONT smallFont = CreateUiFont(Scale(11), FW_SEMIBOLD);

    const int markSize = std::min(Scale(32), std::max(Scale(28), RectHeight(topBar_) - Scale(18)));
    RECT mark = MakeRect(topBar_.left + Scale(12),
                         topBar_.top + (RectHeight(topBar_) - markSize) / 2,
                         topBar_.left + Scale(12) + markSize,
                         topBar_.top + (RectHeight(topBar_) + markSize) / 2);
    FillRoundRect(hdc, mark, palette_.accent, Scale(8));
    StrokeRoundRect(hdc, mark, BlendColor(palette_.accent, palette_.text, 0.14), Scale(8));
    DrawTextInRect(hdc, L"A", mark, markFont, RGB(255, 250, 248), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    COLORREF stateColor = palette_.dim;
    switch (snapshot.state) {
    case PlaybackState::Playing:
        stateColor = palette_.success;
        break;
    case PlaybackState::Ready:
    case PlaybackState::Paused:
        stateColor = palette_.accent;
        break;
    case PlaybackState::Error:
        stateColor = RGB(218, 94, 90);
        break;
    default:
        break;
    }

    const int contentRight = (topControlsLeft_ > 0 ? topControlsLeft_ : topBar_.right) - Scale(12);
    int cursorX = mark.right + Scale(10);
    if (showTopBrand_ && cursorX + Scale(88) < contentRight) {
        const int brandRight = std::min(cursorX + Scale(138), contentRight);
        RECT brand = MakeRect(cursorX, topBar_.top, brandRight, topBar_.bottom);
        DrawTextInRect(hdc, L"Anvil Player", brand, brandFont, palette_.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        cursorX = brandRight + Scale(14);
    }

    if (showTopState_ && cursorX + Scale(132) < contentRight - Scale(80)) {
        DrawLine(hdc, cursorX - Scale(7), topBar_.top + Scale(14), cursorX - Scale(7), topBar_.bottom - Scale(14), palette_.border);
        RECT statePill = MakeRect(cursorX,
                                  topBar_.top + Scale(14),
                                  cursorX + Scale(104),
                                  topBar_.bottom - Scale(14));
        FillRoundRect(hdc, statePill, palette_.surfaceRaised, Scale(7));
        StrokeRoundRect(hdc, statePill, BlendColor(palette_.border, stateColor, 0.18), Scale(7));

        RECT dot = MakeRect(statePill.left + Scale(12),
                            statePill.top + (RectHeight(statePill) - Scale(7)) / 2,
                            statePill.left + Scale(20),
                            statePill.top + (RectHeight(statePill) + Scale(7)) / 2);
        HBRUSH dotBrush = CreateSolidBrush(stateColor);
        HGDIOBJ oldBrush = SelectObject(hdc, dotBrush);
        HPEN nullPen = CreatePen(PS_NULL, 0, stateColor);
        HGDIOBJ oldPen = SelectObject(hdc, nullPen);
        Ellipse(hdc, dot.left, dot.top, dot.right, dot.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(nullPen);
        DeleteObject(dotBrush);

        RECT stateText = MakeRect(statePill.left + Scale(28), statePill.top, statePill.right - Scale(10), statePill.bottom);
        DrawTextInRect(hdc, ToDisplayString(snapshot.state), stateText, smallFont, palette_.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        cursorX = statePill.right + Scale(14);
    }

    if (contentRight > cursorX + Scale(32)) {
        RECT title = MakeRect(cursorX, topBar_.top, contentRight, topBar_.bottom);
        DrawTextInRect(hdc,
                       snapshot.media.has_value() ? snapshot.media->displayName : L"No media loaded",
                       title,
                       bodyFont,
                       snapshot.media.has_value() ? palette_.text : palette_.muted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    DeleteObject(brandFont);
    DeleteObject(markFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
}

void MainWindow::DrawButtons(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
    const double opacity = fullscreen_ ? std::clamp(fullscreenTransportAmount_, 0.0, 1.0) : 1.0;
    if (opacity <= 0.02) {
        return;
    }
    const auto fade = [opacity](const COLORREF color) {
        return FadeForOpacity(color, opacity);
    };

    HFONT buttonFont = CreateUiFont(Scale(11), FW_SEMIBOLD);
    const auto visualButtonBounds = [this](RECT bounds, const double hover, const double press) {
        const int width = RectWidth(bounds);
        const int height = RectHeight(bounds);
        if (width <= 0 || height <= 0) {
            return bounds;
        }

        const int hoverGrow = static_cast<int>(std::round(Scale(1) * hover));
        const int pressInsetX = static_cast<int>(std::round(std::max(1.0, width * 0.006) * press));
        const int pressInsetY = static_cast<int>(std::round(std::max(1.0, height * 0.012) * press));
        InflateRect(&bounds, hoverGrow - pressInsetX, hoverGrow - pressInsetY);
        OffsetRect(&bounds,
                   0,
                   -static_cast<int>(std::round(Scale(1) * hover)) +
                       static_cast<int>(std::round(Scale(1) * press)));
        return bounds;
    };

    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        const auto& button = buttons_[index];
        const bool hovered = button.enabled && static_cast<int>(index) == hoveredButton_;
        const double hoverMotion = button.enabled ? ButtonHoverAmount(button.command) : 0.0;
        const double pressMotion = button.enabled ? ButtonPressAmount(button.command) : 0.0;
        const bool visuallyHovered = hovered || hoverMotion > 0.02;
        const RECT bounds = visualButtonBounds(button.bounds, hoverMotion, pressMotion);

        if (button.kind == ButtonKind::TransportIcon || button.kind == ButtonKind::TransportLabel) {
            const int radius = std::max(Scale(8), std::min(RectWidth(bounds), RectHeight(bounds)) / 2);
            if (visuallyHovered || button.selected) {
                const COLORREF fill = button.selected
                                          ? BlendColor(palette_.accentSoft, palette_.surfaceRaised, 0.12)
                                          : BlendColor(palette_.surfaceRaised, palette_.text, 0.025 + 0.055 * hoverMotion);
                FillRoundRect(hdc, bounds, fade(fill), radius);
                StrokeRoundRect(hdc,
                                bounds,
                                fade(BlendColor(palette_.border, palette_.text, 0.04 + 0.16 * hoverMotion)),
                                radius);
            }

            COLORREF contentColor = !button.enabled
                                        ? palette_.dim
                                        : (button.selected ? palette_.accent : palette_.text);
            if (!button.selected && button.kind == ButtonKind::TransportIcon) {
                contentColor = BlendColor(palette_.muted, palette_.text, hoverMotion);
            }
            if (button.kind == ButtonKind::TransportLabel) {
                DrawTextInRect(hdc,
                               button.label,
                               DeflateRectCopy(bounds, Scale(2), 0),
                               buttonFont,
                               fade(contentColor),
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                RECT iconRect = DeflateRectCopy(bounds, Scale(7), Scale(7));
                IconKind icon = button.icon;
                if (button.command == Command::PlayPause) {
                    icon = snapshot.state == PlaybackState::Playing ? IconKind::Pause : IconKind::Play;
                }
                iconPainter_.Draw(hdc, icon, iconRect, fade(contentColor));
            }
            continue;
        }

        if (button.kind == ButtonKind::InspectorTab) {
            COLORREF textColor = !button.enabled
                                     ? palette_.dim
                                     : (button.selected ? palette_.text : palette_.muted);
            if (visuallyHovered && !button.selected) {
                FillRoundRect(hdc,
                              bounds,
                              fade(BlendColor(palette_.surfaceRaised, palette_.text, 0.04 + 0.04 * hoverMotion)),
                              Scale(6));
                textColor = BlendColor(palette_.muted, palette_.text, hoverMotion);
            }
            if (button.selected) {
                FillRoundRect(hdc,
                              bounds,
                              fade(BlendColor(palette_.accentSoft, palette_.surfaceRaised, 0.18)),
                              Scale(6));
                const int underlineY = bounds.bottom - Scale(3);
                DrawLine(hdc,
                         bounds.left + Scale(8),
                         underlineY,
                         bounds.right - Scale(8),
                         underlineY,
                         fade(palette_.accent),
                         std::max(1, Scale(2)));
            }
            DrawTextInRect(hdc,
                           button.label,
                           DeflateRectCopy(bounds, Scale(2), 0),
                           buttonFont,
                           fade(textColor),
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        }

        if (button.kind == ButtonKind::Tab) {
            COLORREF fill = button.selected ? palette_.surfaceSoft : RGB(11, 13, 17);
            if (!button.enabled) {
                fill = BlendColor(fill, palette_.background, 0.35);
            }
            if (visuallyHovered && !button.selected) {
                fill = BlendColor(fill, palette_.text, 0.04 + 0.04 * hoverMotion);
            }
            FillRoundRect(hdc, bounds, fade(fill), Scale(7));
            StrokeRoundRect(hdc,
                            bounds,
                            fade(!button.enabled
                                     ? BlendColor(palette_.border, palette_.background, 0.25)
                                     : (button.selected
                                            ? BlendColor(palette_.borderStrong, palette_.accent, 0.16)
                                            : BlendColor(palette_.border, palette_.text, 0.10 * hoverMotion))),
                            Scale(7));
            DrawTextInRect(hdc,
                           button.label,
                           DeflateRectCopy(bounds, Scale(6), 0),
                           buttonFont,
                           fade(!button.enabled
                                    ? palette_.dim
                                    : (button.selected ? palette_.text : BlendColor(palette_.muted, palette_.text, hoverMotion))),
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        }

        COLORREF fill = button.primary ? palette_.accent : palette_.surfaceRaised;
        if (!button.enabled) {
            fill = BlendColor(palette_.surface, palette_.background, 0.35);
        } else {
            if (visuallyHovered) {
                fill = BlendColor(fill, palette_.text, (button.primary ? 0.08 : 0.05) * std::max(0.3, hoverMotion));
            }
            if (button.selected && !button.primary) {
                fill = BlendColor(fill, palette_.accent, 0.16);
            }
        }

        const int radius = button.kind == ButtonKind::TransportPrimary
                               ? std::max(Scale(12), std::min(RectWidth(bounds), RectHeight(bounds)) / 2)
                               : Scale(8);
        FillRoundRect(hdc, bounds, fade(fill), radius);
        StrokeRoundRect(hdc,
                        bounds,
                        fade(!button.enabled
                                 ? BlendColor(palette_.border, palette_.background, 0.25)
                                 : (button.primary
                                        ? BlendColor(palette_.accent, palette_.text, 0.14 + 0.10 * hoverMotion)
                                        : BlendColor(palette_.border, palette_.text, 0.03 + 0.12 * hoverMotion))),
                        radius);

        const COLORREF iconColor = fade(!button.enabled
                                            ? palette_.dim
                                            : (button.primary ? RGB(255, 250, 248)
                                                              : (button.selected
                                                                     ? palette_.text
                                                                     : BlendColor(palette_.muted, palette_.text, hoverMotion))));
        const bool wideTextIcon = button.icon == IconKind::HdrColor || button.icon == IconKind::DolbyVisionColor;
        const int iconInset = wideTextIcon
                                  ? Scale(3)
                                  : (button.kind == ButtonKind::TransportPrimary ? Scale(10) : Scale(8));
        RECT iconRect = DeflateRectCopy(bounds, iconInset, iconInset);
        IconKind icon = button.icon;
        if (button.command == Command::PlayPause) {
            icon = snapshot.state == PlaybackState::Playing ? IconKind::Pause : IconKind::Play;
        }
        iconPainter_.Draw(hdc, icon, iconRect, iconColor);
        if (!button.label.empty()) {
            DrawTextInRect(hdc, button.label, bounds, buttonFont, iconColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    DeleteObject(buttonFont);
}

void MainWindow::DrawTooltip(HDC hdc) const {
    if (fullscreen_ && fullscreenTransportAmount_ < 0.95) {
        return;
    }
    if (subtitleMenuOpen_ || subtitleMenuAmount_ > 0.01 || subtitleMenuTarget_ > 0.0) {
        return;
    }
    if (hoveredButton_ < 0 || hoveredButton_ >= static_cast<int>(buttons_.size())) {
        return;
    }

    const auto& button = buttons_[static_cast<std::size_t>(hoveredButton_)];
    if (button.tooltip.empty()) {
        return;
    }

    HFONT font = CreateUiFont(Scale(10), FW_SEMIBOLD);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(hdc, button.tooltip.c_str(), static_cast<int>(button.tooltip.size()), &textSize);
    SelectObject(hdc, oldFont);

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int paddingX = Scale(9);
    const int paddingY = Scale(5);
    const int tipWidth = textSize.cx + paddingX * 2;
    const int tipHeight = textSize.cy + paddingY * 2;
    int left = button.bounds.left + RectWidth(button.bounds) / 2 - tipWidth / 2;
    int top = button.bounds.top - tipHeight - Scale(8);
    if (top < client.top + Scale(6)) {
        top = button.bounds.bottom + Scale(8);
    }
    left = std::clamp(left,
                      static_cast<int>(client.left) + Scale(8),
                      static_cast<int>(client.right) - tipWidth - Scale(8));
    RECT tooltip = MakeRect(left, top, left + tipWidth, top + tipHeight);

    FillRoundRect(hdc, tooltip, RGB(12, 14, 18), Scale(6));
    StrokeRoundRect(hdc, tooltip, palette_.border, Scale(6));
    DrawTextInRect(hdc,
                   button.tooltip,
                   DeflateRectCopy(tooltip, paddingX, paddingY),
                   font,
                   palette_.text,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(font);
}

void MainWindow::ClearPreviewBitmap() const {
    if (previewBitmap_) {
        DeleteObject(previewBitmap_);
        previewBitmap_ = nullptr;
    }
    previewBitmapPath_.clear();
}

HBITMAP MainWindow::LoadPreviewBitmap(const std::filesystem::path& imagePath) const {
    if (imagePath.empty()) {
        ClearPreviewBitmap();
        return nullptr;
    }
    if (previewBitmap_ && previewBitmapPath_ == imagePath) {
        return previewBitmap_;
    }

    ClearPreviewBitmap();
    const auto pathText = imagePath.wstring();
    previewBitmap_ = static_cast<HBITMAP>(LoadImageW(
        nullptr,
        pathText.c_str(),
        IMAGE_BITMAP,
        0,
        0,
        LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    if (previewBitmap_) {
        previewBitmapPath_ = imagePath;
    }
    return previewBitmap_;
}

void MainWindow::DrawPreviewBitmap(HDC hdc, RECT target, const std::filesystem::path& imagePath) const {
    HBITMAP bitmap = LoadPreviewBitmap(imagePath);
    if (!bitmap) {
        return;
    }

    BITMAP bitmapInfo{};
    if (GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo) == 0 || bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight <= 0) {
        return;
    }

    target = fullscreen_ ? target : DeflateRectCopy(target, Scale(2), Scale(2));
    const double scale = std::min(
        static_cast<double>(RectWidth(target)) / static_cast<double>(bitmapInfo.bmWidth),
        static_cast<double>(RectHeight(target)) / static_cast<double>(bitmapInfo.bmHeight));
    const int drawWidth = std::max(1, static_cast<int>(bitmapInfo.bmWidth * scale));
    const int drawHeight = std::max(1, static_cast<int>(bitmapInfo.bmHeight * scale));
    const int drawLeft = target.left + (RectWidth(target) - drawWidth) / 2;
    const int drawTop = target.top + (RectHeight(target) - drawHeight) / 2;
    RECT drawRect = MakeRect(drawLeft, drawTop, drawLeft + drawWidth, drawTop + drawHeight);

    HDC imageDc = CreateCompatibleDC(hdc);
    HGDIOBJ oldBitmap = SelectObject(imageDc, bitmap);
    const int oldStretchMode = SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);

    HRGN clip = fullscreen_
                    ? CreateRectRgn(target.left, target.top, target.right + 1, target.bottom + 1)
                    : CreateRoundRectRgn(target.left, target.top, target.right + 1, target.bottom + 1, Scale(8), Scale(8));
    SelectClipRgn(hdc, clip);
    StretchBlt(hdc,
               drawRect.left,
               drawRect.top,
               RectWidth(drawRect),
               RectHeight(drawRect),
               imageDc,
               0,
               0,
               bitmapInfo.bmWidth,
               bitmapInfo.bmHeight,
               SRCCOPY);
    SelectClipRgn(hdc, nullptr);
    DeleteObject(clip);

    SetStretchBltMode(hdc, oldStretchMode);
    SelectObject(imageDc, oldBitmap);
    DeleteDC(imageDc);
}

void MainWindow::DrawDecodedVideoFrame(HDC hdc, RECT target, const VideoFrame& frame) const {
    if (!frame.HasPixels()) {
        return;
    }

    target = fullscreen_ ? target : DeflateRectCopy(target, Scale(2), Scale(2));
    const double scale = std::min(
        static_cast<double>(RectWidth(target)) / static_cast<double>(VideoFrame::Width),
        static_cast<double>(RectHeight(target)) / static_cast<double>(VideoFrame::Height));
    const int drawWidth = std::max(1, static_cast<int>(VideoFrame::Width * scale));
    const int drawHeight = std::max(1, static_cast<int>(VideoFrame::Height * scale));
    const int drawLeft = target.left + (RectWidth(target) - drawWidth) / 2;
    const int drawTop = target.top + (RectHeight(target) - drawHeight) / 2;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = VideoFrame::Width;
    bitmapInfo.bmiHeader.biHeight = -VideoFrame::Height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HRGN clip = fullscreen_
                    ? CreateRectRgn(target.left, target.top, target.right + 1, target.bottom + 1)
                    : CreateRoundRectRgn(target.left, target.top, target.right + 1, target.bottom + 1, Scale(8), Scale(8));
    SelectClipRgn(hdc, clip);
    const int oldStretchMode = SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc,
                  drawLeft,
                  drawTop,
                  drawWidth,
                  drawHeight,
                  0,
                  0,
                  VideoFrame::Width,
                  VideoFrame::Height,
                  frame.pixels->data(),
                  &bitmapInfo,
                  DIB_RGB_COLORS,
                  SRCCOPY);
    SetStretchBltMode(hdc, oldStretchMode);
    SelectClipRgn(hdc, nullptr);
    DeleteObject(clip);
}

void MainWindow::DrawVideoSurface(HDC hdc, const PlaybackSessionSnapshot& snapshot, const RECT& paintRect) const {
    if (fullscreen_) {
        FillRectColor(hdc, videoSurface_, RGB(0, 0, 0));
        if (snapshot.media.has_value()) {
            VideoFrame decodedFrame;
            if (backend_ == PlaybackBackend::RawFrameBridge && videoDecoder_.LatestFrame(decodedFrame)) {
                DrawDecodedVideoFrame(hdc, videoSurface_, decodedFrame);
            } else if (!(backend_ == PlaybackBackend::NativeFfmpegD3D11 && nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning()) &&
                       !snapshot.media->previewImagePath.empty()) {
                DrawPreviewBitmap(hdc, videoSurface_, snapshot.media->previewImagePath);
            }
        }
        return;
    }

    FillRoundRect(hdc, videoSurface_, palette_.viewport, Scale(8));
    StrokeRoundRect(hdc, videoSurface_, BlendColor(palette_.border, palette_.accent, 0.16), Scale(8), Scale(1));

    const bool compactSurface = RectWidth(videoSurface_) < Scale(720) || RectHeight(videoSurface_) < Scale(280);
    RECT inner = DeflateRectCopy(videoSurface_, compactSurface ? Scale(18) : Scale(24), compactSurface ? Scale(16) : Scale(22));

    if (snapshot.media.has_value()) {
        if (SidebarAnimationActive()) {
            return;
        }

        HFONT titleFont = CreateUiFont(compactSurface ? Scale(18) : Scale(22), FW_SEMIBOLD);
        HFONT bodyFont = CreateUiFont(compactSurface ? Scale(12) : Scale(13), FW_NORMAL);
        HFONT smallFont = CreateUiFont(Scale(11), FW_NORMAL);
        VideoFrame decodedFrame;
        const bool nativeActive = (backend_ == PlaybackBackend::NativeFfmpegD3D11) &&
                                  nativeVideoDecoder_ && nativeVideoDecoder_->IsRunning();
        if (backend_ == PlaybackBackend::RawFrameBridge && videoDecoder_.LatestFrame(decodedFrame)) {
            DrawDecodedVideoFrame(hdc, videoSurface_, decodedFrame);
        } else if (!nativeActive && !snapshot.media->previewImagePath.empty()) {
            DrawPreviewBitmap(hdc, videoSurface_, snapshot.media->previewImagePath);
        } else if (!nativeActive) {
            ClearPreviewBitmap();
        }
        RECT titleScrim = MakeRect(inner.left - Scale(8),
                                   inner.top - Scale(6),
                                   inner.right,
                                   inner.top + (compactSurface ? Scale(48) : Scale(58)));
        FillRoundRect(hdc, titleScrim, RGB(7, 9, 12), Scale(7));

        RECT title = MakeRect(inner.left, inner.top, inner.right - Scale(18), inner.top + (compactSurface ? Scale(26) : Scale(32)));
        DrawTextInRect(hdc, snapshot.media->displayName, title, titleFont, palette_.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT mode = MakeRect(inner.left, title.bottom + Scale(1), inner.right, title.bottom + Scale(22));
        std::wstring pipeline = snapshot.media->container + L" / " + RuntimeShortLabel() + L" / " + snapshot.media->hdrFormat;
        DrawTextInRect(hdc, pipeline, mode, bodyFont, palette_.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (snapshot.state == PlaybackState::Error) {
            RECT center = MakeRect(inner.left,
                                   inner.top + RectHeight(inner) / 2 - Scale(22),
                                   inner.right,
                                   inner.top + RectHeight(inner) / 2 + Scale(22));
            DrawTextInRect(hdc, ToDisplayString(snapshot.state), center, titleFont, BlendColor(palette_.accent, palette_.text, 0.12), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        if (RectHeight(inner) >= Scale(150)) {
            RECT badge = MakeRect(inner.left,
                                  inner.bottom - Scale(28),
                                  inner.left + (compactSurface ? Scale(136) : Scale(160)),
                                  inner.bottom);
            FillRoundRect(hdc, badge, RGB(12, 15, 18), Scale(7));
            StrokeRoundRect(hdc, badge, palette_.border, Scale(7));
            DrawTextInRect(hdc, RuntimeShortLabel(), DeflateRectCopy(badge, Scale(10), Scale(6)), smallFont, palette_.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        DeleteObject(titleFont);
        DeleteObject(bodyFont);
        DeleteObject(smallFont);
    } else {
        ClearPreviewBitmap();
        const int tileSize = compactSurface ? Scale(62) : Scale(74);
        RECT tile = MakeRect(inner.left + RectWidth(inner) / 2 - tileSize / 2,
                             inner.top + RectHeight(inner) / 2 - (compactSurface ? Scale(68) : Scale(82)),
                             inner.left + RectWidth(inner) / 2 + tileSize / 2,
                             inner.top + RectHeight(inner) / 2 - Scale(8));
        RECT title = MakeRect(inner.left, tile.bottom + Scale(18), inner.right, tile.bottom + (compactSurface ? Scale(44) : Scale(54)));
        RECT subtitle = MakeRect(inner.left, title.bottom + Scale(2), inner.right, title.bottom + Scale(26));
        RECT placeholder = tile;
        UnionRect(&placeholder, &placeholder, &title);
        UnionRect(&placeholder, &placeholder, &subtitle);
        if (!RectsIntersect(paintRect, placeholder)) {
            return;
        }

        HFONT emptyFont = CreateUiFont(compactSurface ? Scale(20) : Scale(24), FW_SEMIBOLD);
        HFONT bodyFont = CreateUiFont(compactSurface ? Scale(12) : Scale(13), FW_NORMAL);
        FillRoundRect(hdc, tile, RGB(13, 16, 20), Scale(12));
        StrokeRoundRect(hdc, tile, palette_.border, Scale(12));
        iconPainter_.Draw(hdc, IconKind::Play, DeflateRectCopy(tile, compactSurface ? Scale(17) : Scale(20), compactSurface ? Scale(17) : Scale(20)), palette_.text);

        DrawTextInRect(hdc, L"No media loaded", title, emptyFont, palette_.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextInRect(hdc, L"Ready", subtitle, bodyFont, palette_.dim, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        DeleteObject(emptyFont);
        DeleteObject(bodyFont);
    }
}

void MainWindow::DrawTransport(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
    const double opacity = fullscreen_ ? std::clamp(fullscreenTransportAmount_, 0.0, 1.0) : 1.0;
    if (opacity <= 0.02) {
        return;
    }
    const auto fade = [opacity](const COLORREF color) {
        return FadeForOpacity(color, opacity);
    };

    FillRoundRect(hdc, transportBar_, fade(palette_.surface), Scale(12));
    StrokeRoundRect(hdc, transportBar_, fade(palette_.border), Scale(12));
    DrawSurfaceRim(hdc,
                   transportBar_,
                   fade(BlendColor(palette_.surface, palette_.text, 0.10)),
                   fade(BlendColor(palette_.border, palette_.info, 0.20)),
                   fade(BlendColor(palette_.border, palette_.accent, 0.15)),
                   Scale(14));

    const bool compactBar = RectHeight(transportBar_) <= Scale(82);
    const int horizontalInset = compactBar ? Scale(18) : Scale(24);
    HFONT monoFont = CreateMonoFont(Scale(11));

    const double progressHover = std::clamp(progressHoverAmount_, 0.0, 1.0);
    const int progressGrow = static_cast<int>(std::round(Scale(3) * progressHover));
    RECT progressTrack = progress_;
    progressTrack.top -= progressGrow;
    progressTrack.bottom += progressGrow;
    const COLORREF trackColor = BlendColor(palette_.track, BlendColor(palette_.accentSoft, palette_.accent, 0.18), 0.55 * progressHover);
    FillRoundRect(hdc, progressTrack, fade(trackColor), Scale(3 + static_cast<int>(std::round(2.0 * progressHover))));
    double progressRatio = 0.0;
    std::wstring durationText = L"0:00";
    std::chrono::milliseconds displayPosition = snapshot.position;
    if (draggingProgress_ && snapshot.media.has_value()) {
        displayPosition = dragSeekPosition_;
    }
    if (snapshot.media.has_value() && snapshot.media->duration.count() > 0) {
        progressRatio = std::clamp(
            static_cast<double>(displayPosition.count()) / static_cast<double>(snapshot.media->duration.count()),
            0.0,
            1.0);
        durationText = FormatTimecode(snapshot.media->duration);
    }

    double bufferedRatio = progressRatio;
    if (backend_ == PlaybackBackend::NativeFfmpegD3D11 &&
        nativeVideoDecoder_ &&
        snapshot.media.has_value() &&
        snapshot.media->duration.count() > 0) {
        const auto stats = nativeVideoDecoder_->Stats();
        if ((stats.queueDepth > 0 || stats.packetQueueDepth > 0) && stats.bufferedEnd > displayPosition) {
            bufferedRatio = std::clamp(
                static_cast<double>(stats.bufferedEnd.count()) / static_cast<double>(snapshot.media->duration.count()),
                progressRatio,
                1.0);
        }
    }
    RECT bufferedFill = progressTrack;
    bufferedFill.right = bufferedFill.left + static_cast<int>(RectWidth(progressTrack) * bufferedRatio);
    const int progressRight = progressTrack.left + static_cast<int>(RectWidth(progressTrack) * progressRatio);
    if (bufferedFill.right > progressRight) {
        const int minVisibleBufferWidth = Scale(6);
        bufferedFill.right = std::min(static_cast<int>(progressTrack.right),
                                      std::max(static_cast<int>(bufferedFill.right),
                                               progressRight + minVisibleBufferWidth));
        FillRoundRect(hdc,
                      bufferedFill,
                      fade(BlendColor(palette_.success, palette_.info, 0.28 + 0.12 * progressHover)),
                      Scale(3 + static_cast<int>(std::round(2.0 * progressHover))));
    }

    RECT progressFill = progressTrack;
    progressFill.right = progressFill.left + static_cast<int>(RectWidth(progressTrack) * progressRatio);
    if (RectWidth(progressFill) > 0) {
        FillRoundRect(hdc,
                      progressFill,
                      fade(BlendColor(palette_.accent, RGB(255, 244, 240), 0.16 * progressHover)),
                      Scale(3 + static_cast<int>(std::round(2.0 * progressHover))));
    }

    const int knobX = progressTrack.left + static_cast<int>(RectWidth(progressTrack) * progressRatio);
    const int knobRadius = Scale(4) + static_cast<int>(std::round(Scale(2) * progressHover));
    RECT knob = MakeRect(knobX - knobRadius, progress_.top - knobRadius, knobX + knobRadius, progress_.bottom + knobRadius);
    FillRoundRect(hdc, knob, fade(RGB(255, 244, 240)), knobRadius);
    StrokeRoundRect(hdc, knob, fade(palette_.accent), knobRadius);

    RECT timeLeft = MakeRect(transportBar_.left + horizontalInset,
                             transportBar_.top + Scale(11),
                             transportBar_.left + horizontalInset + Scale(100),
                             progress_.top - Scale(6));
    const int transportRightEdge = static_cast<int>(transportBar_.right) - horizontalInset;
    int durationRight = transportRightEdge;
    RECT timeRight = MakeRect(durationRight - Scale(110),
                              transportBar_.top + Scale(11),
                              durationRight,
                              progress_.top - Scale(6));
    DrawTextInRect(hdc, FormatTimecode(displayPosition), timeLeft, monoFont, fade(draggingProgress_ ? palette_.accent : palette_.text), DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (RectWidth(progress_) >= Scale(230) && timeRight.left >= timeLeft.right + Scale(12)) {
        DrawTextInRect(hdc, durationText, timeRight, monoFont, fade(palette_.muted), DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    DrawVolumeSlider(hdc, snapshot);
    DeleteObject(monoFont);
}

void MainWindow::DrawVolumeSlider(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
    if (RectWidth(volumeSlider_) <= 0 || RectHeight(volumeSlider_) <= 0) {
        return;
    }

    const double opacity = fullscreen_ ? std::clamp(fullscreenTransportAmount_, 0.0, 1.0) : 1.0;
    if (opacity <= 0.02) {
        return;
    }
    const auto fade = [opacity](const COLORREF color) {
        return FadeForOpacity(color, opacity);
    };

    const double activeAmount = draggingVolume_ ? 1.0 : std::clamp(volumeHoverAmount_, 0.0, 1.0);

    const double volume = std::clamp(snapshot.volume, 0.0, 1.0);
    const int centerY = volumeSlider_.top + RectHeight(volumeSlider_) / 2;
    RECT iconRect = MakeRect(volumeSlider_.left,
                             centerY - Scale(12),
                             volumeSlider_.left + Scale(28),
                             centerY + Scale(12));
    iconPainter_.Draw(hdc,
                      volume < 0.5 ? IconKind::VolumeDown : IconKind::VolumeUp,
                      iconRect,
                      fade(volume <= 0.0 ? palette_.dim : palette_.text));

    const RECT track = VolumeSliderTrackRect();
    if (RectWidth(track) > Scale(12) && RectHeight(track) > 0) {
        FillRoundRect(hdc,
                      track,
                      fade(BlendColor(palette_.track, RGB(255, 255, 255), 0.12 + 0.14 * activeAmount)),
                      Scale(3));
        RECT fillRect = track;
        fillRect.right = fillRect.left + static_cast<int>(std::round(RectWidth(track) * volume));
        if (RectWidth(fillRect) > 0) {
            FillRoundRect(hdc,
                          fillRect,
                          fade(BlendColor(palette_.text, BlendColor(palette_.accent, RGB(255, 255, 255), 0.16), activeAmount)),
                          Scale(3));
        }

        const int knobX = track.left + static_cast<int>(std::round(RectWidth(track) * volume));
        const int knobRadius = Scale(5) + static_cast<int>(std::round(Scale(1) * activeAmount));
        RECT knob = MakeRect(knobX - knobRadius,
                             centerY - knobRadius,
                             knobX + knobRadius,
                             centerY + knobRadius);
        FillRoundRect(hdc, knob, fade(RGB(255, 255, 255)), knobRadius);
        StrokeRoundRect(hdc,
                        knob,
                        fade(BlendColor(RGB(255, 255, 255), palette_.accent, activeAmount)),
                        knobRadius);

        if (draggingVolume_) {
            HFONT tipFont = CreateMonoFont(Scale(10), FW_SEMIBOLD);
            const std::wstring value = PercentText(volume);
            HGDIOBJ oldFont = SelectObject(hdc, tipFont);
            SIZE textSize{};
            GetTextExtentPoint32W(hdc, value.c_str(), static_cast<int>(value.size()), &textSize);
            SelectObject(hdc, oldFont);

            const int tipWidth = std::max(Scale(44), static_cast<int>(textSize.cx) + Scale(16));
            const int tipHeight = Scale(24);
            int tipLeft = knobX - tipWidth / 2;
            tipLeft = std::clamp(tipLeft,
                                 static_cast<int>(volumeSlider_.left),
                                 static_cast<int>(volumeSlider_.right) - tipWidth);
            const int tipTop = volumeSlider_.top - tipHeight - Scale(8);
            RECT tip = MakeRect(tipLeft, tipTop, tipLeft + tipWidth, tipTop + tipHeight);
            FillRoundRect(hdc, tip, fade(RGB(13, 16, 21)), Scale(7));
            StrokeRoundRect(hdc, tip, fade(BlendColor(palette_.borderStrong, palette_.accent, 0.18)), Scale(7));
            DrawTextInRect(hdc,
                           value,
                           DeflateRectCopy(tip, Scale(8), Scale(4)),
                           tipFont,
                           fade(palette_.text),
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(tipFont);
        }
    }
}

void MainWindow::DrawSubtitleMenu(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
    const double amount = std::clamp(subtitleMenuAmount_, 0.0, 1.0);
    if (amount <= 0.01 || RectWidth(subtitleMenu_) <= 0 || RectHeight(subtitleMenu_) <= 0) {
        return;
    }

    const double opacity = (fullscreen_ ? std::clamp(fullscreenTransportAmount_, 0.0, 1.0) : 1.0) * amount;
    if (opacity <= 0.02) {
        return;
    }
    const auto fade = [opacity](const COLORREF color) {
        return FadeForOpacity(color, opacity);
    };

    if (RectHeight(subtitleMenu_) <= 0) {
        return;
    }

    const int savedDc = SaveDC(hdc);

    const COLORREF panel = RGB(16, 19, 24);
    const COLORREF panelLine = RGB(39, 44, 53);
    const COLORREF panelRaised = RGB(21, 25, 34);
    const COLORREF panelHover = RGB(25, 30, 40);
    const COLORREF panelMuted = RGB(152, 161, 175);
    const COLORREF panelText = RGB(238, 241, 245);
    const COLORREF panelAccent = RGB(223, 118, 95);

    FillRoundRect(hdc, subtitleMenu_, fade(panel), Scale(8));
    StrokeRoundRect(hdc, subtitleMenu_, fade(BlendColor(panelLine, panelText, 0.08)), Scale(8));
    DrawSurfaceRim(hdc,
                   subtitleMenu_,
                   fade(BlendColor(panel, panelText, 0.14)),
                   fade(BlendColor(panelLine, panelAccent, 0.18)),
                   fade(BlendColor(panelLine, palette_.accent, 0.14)),
                   Scale(10));

    HFONT tabFont = CreateUiFont(Scale(13), FW_SEMIBOLD);
    HFONT itemFont = CreateUiFont(Scale(13), FW_NORMAL);
    HFONT selectedItemFont = CreateUiFont(Scale(13), FW_SEMIBOLD);
    HFONT metaFont = CreateUiFont(Scale(10), FW_NORMAL);
    HFONT smallFont = CreateUiFont(Scale(10), FW_SEMIBOLD);
    HFONT valueFont = CreateUiFont(Scale(13), FW_SEMIBOLD);
    const auto settings = controller_.Settings();
    const auto finish = [&]() {
        RestoreDC(hdc, savedDc);
        DeleteObject(valueFont);
        DeleteObject(smallFont);
        DeleteObject(metaFont);
        DeleteObject(selectedItemFont);
        DeleteObject(itemFont);
        DeleteObject(tabFont);
    };
    const int headerHeight = Scale(62);
    const int itemHeight = Scale(42);
    const int listGap = Scale(8);
    const int rowInset = Scale(8);
    const int delayHeight = Scale(56);
    const int actionHeight = Scale(44);
    const int styleHeight = Scale(48);
    const bool audioPage = subtitleMenuPage_ == SubtitleMenuPage::Audio;
    const bool subtitlePage = subtitleMenuPage_ == SubtitleMenuPage::Subtitles;
    const bool danmakuPage = subtitleMenuPage_ == SubtitleMenuPage::Danmaku;
    const std::vector<int>& menuTracks = audioPage ? audioMenuTracks_ : subtitleMenuTracks_;
    const int count = std::max(1, static_cast<int>(menuTracks.size()));
    const int visibleItemCount = danmakuPage
                                     ? 0
                                     : (subtitleMenuVisibleItemCount_ > 0
                                            ? std::clamp(subtitleMenuVisibleItemCount_, 1, count)
                                            : count);
    const int firstItem = std::clamp(subtitleMenuScrollOffset_, 0, std::max(0, count - visibleItemCount));

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
    const int activeTab = audioPage ? 0 : (subtitlePage ? 1 : 2);
    const wchar_t* tabLabels[] = {L"Audio", L"Subtitles", L"Danmaku"};
    for (int index = 0; index < 3; ++index) {
        const RECT tab = MakeRect(tabRail.left + index * (tabWidth + tabGap),
                                  tabRail.top,
                                  tabRail.left + index * (tabWidth + tabGap) + tabWidth,
                                  tabRail.bottom);
        if (index == activeTab) {
            FillRoundRect(hdc, tab, fade(panelRaised), Scale(6));
        }
        DrawTextInRect(hdc,
                       tabLabels[index],
                       tab,
                       tabFont,
                       fade(index == activeTab ? panelText : panelMuted),
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DrawLine(hdc,
             subtitleMenu_.left,
             header.bottom,
             subtitleMenu_.right,
             header.bottom,
             fade(panelLine));

    const int listTop = header.bottom + listGap;
    for (int localIndex = 0; localIndex < visibleItemCount; ++localIndex) {
        const int index = firstItem + localIndex;
        const RECT item = MakeRect(subtitleMenu_.left + rowInset,
                                   listTop + itemHeight * localIndex,
                                   subtitleMenu_.right - rowInset,
                                   listTop + itemHeight * (localIndex + 1));
        if (item.bottom < subtitleMenu_.top || item.top > subtitleMenu_.bottom) {
            continue;
        }

        const bool hasTrack = index < static_cast<int>(menuTracks.size());
        const int track = hasTrack ? menuTracks[static_cast<std::size_t>(index)] : anvil::playback::kSubtitleTrackOff;
        const bool selected = hasTrack &&
                              track == (audioPage ? settings.audio.selectedTrackIndex
                                                  : settings.subtitles.selectedTrackIndex);
        const bool hovered = hasTrack && index == hoveredSubtitleMenuItem_;
        if (selected || hovered) {
            FillRoundRect(hdc,
                          item,
                          fade(selected ? panelRaised : panelHover),
                          Scale(5));
        }

        if (selected) {
            DrawCheckMark(hdc,
                          POINT{item.left + Scale(26), item.top + RectHeight(item) / 2},
                          Scale(16),
                          fade(panelAccent));
        }

        const std::wstring label = hasTrack ? SubtitleMenuPrimaryLabel(track, snapshot.media) : L"No subtitles";
        const std::wstring codec = hasTrack
                                       ? (audioPage ? AudioMenuCodecLabel(track, snapshot.media)
                                                    : SubtitleMenuCodecLabel(track, snapshot.media))
                                       : L"";
        const std::wstring displayLabel = hasTrack
                                              ? (audioPage ? AudioMenuPrimaryLabel(track, snapshot.media)
                                                           : label)
                                              : (audioPage ? L"No audio tracks" : L"No subtitles");
        const RECT codecText = MakeRect(item.right - Scale(76), item.top, item.right - Scale(12), item.bottom);
        const int labelLeft = hasTrack ? item.left + Scale(50) : item.left + Scale(16);
        RECT text = MakeRect(labelLeft,
                             item.top,
                             codec.empty() ? item.right - Scale(12) : codecText.left - Scale(8),
                             item.bottom);
        DrawTextInRect(hdc,
                       displayLabel,
                       text,
                       selected ? selectedItemFont : itemFont,
                       fade(hasTrack ? (selected ? panelText : RGB(226, 226, 225)) : panelMuted),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!codec.empty()) {
            DrawTextInRect(hdc,
                           codec,
                           codecText,
                           metaFont,
                           fade(panelMuted),
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    const int listContentBottom = listTop + itemHeight * visibleItemCount;
    const double scrollbarOpacity = ScrollbarOpacity(subtitleMenuScrollLastActiveAt_,
                                                     false,
                                                     std::chrono::steady_clock::now());
    if (visibleItemCount > 0 &&
        count > visibleItemCount &&
        scrollbarOpacity > 0.02) {
        const auto fadeScrollbar = [opacity, scrollbarOpacity](const COLORREF color) {
            return FadeForOpacity(color, opacity * scrollbarOpacity);
        };
        const RECT scrollTrack = MakeRect(subtitleMenu_.right - Scale(8),
                                          listTop + Scale(5),
                                          subtitleMenu_.right - Scale(5),
                                          listContentBottom - Scale(5));
        if (RectHeight(scrollTrack) > Scale(18)) {
            FillRoundRect(hdc, scrollTrack, fadeScrollbar(BlendColor(panelLine, panel, 0.35)), Scale(2));
            const int trackHeight = RectHeight(scrollTrack);
            const int thumbHeight = std::clamp(static_cast<int>(std::round(
                                             static_cast<double>(trackHeight) *
                                             static_cast<double>(visibleItemCount) /
                                             static_cast<double>(count))),
                                             Scale(18),
                                             trackHeight);
            const int maxOffset = std::max(1, count - visibleItemCount);
            const int thumbTop = scrollTrack.top +
                                 static_cast<int>(std::round(
                                     static_cast<double>(trackHeight - thumbHeight) *
                                     static_cast<double>(firstItem) /
                                     static_cast<double>(maxOffset)));
            const RECT thumb = MakeRect(scrollTrack.left,
                                        thumbTop,
                                        scrollTrack.right,
                                        thumbTop + thumbHeight);
            FillRoundRect(hdc, thumb, fadeScrollbar(panelMuted), Scale(2));
        }
    }

    const auto drawStepperRow = [&](const RECT& row, const std::wstring& title, const std::wstring& value) {
        DrawTextInRect(hdc,
                       L"-",
                       MakeRect(row.left + Scale(24), row.top, row.left + Scale(58), row.bottom),
                       valueFont,
                       fade(panelText),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextInRect(hdc,
                       title,
                       MakeRect(row.left + Scale(70), row.top + Scale(7), row.right - Scale(70), row.top + Scale(25)),
                       smallFont,
                       fade(panelMuted),
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextInRect(hdc,
                       value,
                       MakeRect(row.left + Scale(70), row.top + Scale(24), row.right - Scale(70), row.bottom - Scale(6)),
                       valueFont,
                       fade(panelText),
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawPlusMark(hdc,
                     POINT{row.right - Scale(38), row.top + RectHeight(row) / 2},
                     Scale(13),
                     fade(panelText));
    };
    const auto drawMenuActionRow = [&](const RECT& row, const IconKind icon, const std::wstring& label, const std::wstring& meta) {
        const RECT iconRect = MakeRect(row.left + Scale(24),
                                       row.top + RectHeight(row) / 2 - Scale(12),
                                       row.left + Scale(48),
                                       row.top + RectHeight(row) / 2 + Scale(12));
        iconPainter_.Draw(hdc, icon, iconRect, fade(panelMuted));
        DrawTextInRect(hdc,
                       label,
                       MakeRect(row.left + Scale(62), row.top, meta.empty() ? row.right - Scale(18) : row.right - Scale(96), row.bottom),
                       itemFont,
                       fade(panelText),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!meta.empty()) {
            DrawTextInRect(hdc,
                           meta,
                           MakeRect(row.right - Scale(94), row.top, row.right - Scale(18), row.bottom),
                           metaFont,
                           fade(panelMuted),
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    };

    if (audioPage) {
        finish();
        return;
    }

    if (danmakuPage) {
        int rowTop = header.bottom + Scale(8);
        const auto nextRow = [&](const int height) {
            RECT row = MakeRect(subtitleMenu_.left, rowTop, subtitleMenu_.right, rowTop + height);
            rowTop += height;
            return row;
        };
        const RECT toggleRow = nextRow(actionHeight);
        if (settings.danmaku.enabled) {
            DrawCheckMark(hdc,
                          POINT{toggleRow.left + Scale(32), toggleRow.top + RectHeight(toggleRow) / 2},
                          Scale(16),
                          fade(panelAccent));
        }
        DrawTextInRect(hdc,
                       L"Enable danmaku",
                       MakeRect(toggleRow.left + Scale(62), toggleRow.top, toggleRow.right - Scale(18), toggleRow.bottom),
                       itemFont,
                       fade(panelText),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawLine(hdc, subtitleMenu_.left, toggleRow.bottom, subtitleMenu_.right, toggleRow.bottom, fade(panelLine));

        const RECT modeRow = nextRow(actionHeight);
        drawMenuActionRow(modeRow, IconKind::Subtitles, L"Display mode", DanmakuModeText(settings.danmaku.mode));
        DrawLine(hdc, subtitleMenu_.left, modeRow.bottom, subtitleMenu_.right, modeRow.bottom, fade(panelLine));

        const RECT opacityRow = nextRow(styleHeight);
        drawStepperRow(opacityRow, L"Opacity", PercentTextFromInt(settings.danmaku.opacityPercent));
        DrawLine(hdc, subtitleMenu_.left, opacityRow.bottom, subtitleMenu_.right, opacityRow.bottom, fade(panelLine));

        const RECT speedRow = nextRow(styleHeight);
        drawStepperRow(speedRow, L"Speed", PercentTextFromInt(settings.danmaku.speedPercent));
        DrawLine(hdc, subtitleMenu_.left, speedRow.bottom, subtitleMenu_.right, speedRow.bottom, fade(panelLine));

        const std::wstring danmakuMeta = settings.danmaku.externalDanmakuPath.empty()
                                             ? L"XML/JSON/ASS"
                                             : settings.danmaku.externalDanmakuPath.filename().wstring();
        drawMenuActionRow(nextRow(actionHeight), IconKind::Folder, L"Add danmaku file...", danmakuMeta);
        finish();
        return;
    }

    const int listBottom = listContentBottom + Scale(6);
    DrawLine(hdc,
             subtitleMenu_.left,
             listBottom,
             subtitleMenu_.right,
             listBottom,
             fade(panelLine));

    const RECT delayRow = MakeRect(subtitleMenu_.left,
                                   listBottom,
                                   subtitleMenu_.right,
                                   listBottom + delayHeight);
    DrawTextInRect(hdc,
                   L"-",
                   MakeRect(delayRow.left + Scale(24), delayRow.top, delayRow.left + Scale(58), delayRow.bottom),
                   valueFont,
                   fade(panelText),
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextInRect(hdc,
                   L"Subtitle delay",
                   MakeRect(delayRow.left + Scale(70), delayRow.top + Scale(8), delayRow.right - Scale(70), delayRow.top + Scale(26)),
                   smallFont,
                   fade(panelMuted),
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextInRect(hdc,
                   SubtitleDelayText(settings.subtitles.subtitleDelayMs),
                   MakeRect(delayRow.left + Scale(70), delayRow.top + Scale(25), delayRow.right - Scale(70), delayRow.bottom - Scale(7)),
                   valueFont,
                   fade(panelText),
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawPlusMark(hdc,
                 POINT{delayRow.right - Scale(38), delayRow.top + RectHeight(delayRow) / 2},
                 Scale(13),
                 fade(panelText));
    DrawLine(hdc,
             subtitleMenu_.left,
             delayRow.bottom,
             subtitleMenu_.right,
             delayRow.bottom,
             fade(panelLine));

    const RECT addRow = MakeRect(subtitleMenu_.left, delayRow.bottom, subtitleMenu_.right, delayRow.bottom + actionHeight);
    const RECT sizeRow = MakeRect(subtitleMenu_.left, addRow.bottom, subtitleMenu_.right, addRow.bottom + styleHeight);
    const RECT offsetXRow = MakeRect(subtitleMenu_.left, sizeRow.bottom, subtitleMenu_.right, sizeRow.bottom + styleHeight);
    const RECT offsetYRow = MakeRect(subtitleMenu_.left, offsetXRow.bottom, subtitleMenu_.right, offsetXRow.bottom + styleHeight);
    const auto drawActionRow = [&](const RECT& row, const IconKind icon, const std::wstring& label, const bool plus) {
        const RECT iconRect = MakeRect(row.left + Scale(24),
                                       row.top + RectHeight(row) / 2 - Scale(12),
                                       row.left + Scale(48),
                                       row.top + RectHeight(row) / 2 + Scale(12));
        iconPainter_.Draw(hdc, icon, iconRect, fade(panelMuted));
        if (plus) {
            DrawPlusMark(hdc,
                         POINT{iconRect.right - Scale(2), iconRect.top + Scale(6)},
                         Scale(7),
                         fade(panelMuted));
        }
        DrawTextInRect(hdc,
                       label,
                       MakeRect(row.left + Scale(62), row.top, row.right - Scale(18), row.bottom),
                       itemFont,
                       fade(panelMuted),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    drawActionRow(addRow, IconKind::Folder, L"Add subtitle file...", true);
    DrawLine(hdc,
             subtitleMenu_.left,
             addRow.bottom,
             subtitleMenu_.right,
             addRow.bottom,
             fade(panelLine));
    drawStepperRow(sizeRow, L"Subtitle size", SubtitleScaleText(settings.subtitles.fontScale));
    DrawLine(hdc,
             subtitleMenu_.left,
             sizeRow.bottom,
             subtitleMenu_.right,
             sizeRow.bottom,
             fade(panelLine));
    drawStepperRow(offsetXRow, L"Horizontal offset", PixelOffsetText(settings.subtitles.offsetXPx));
    DrawLine(hdc,
             subtitleMenu_.left,
             offsetXRow.bottom,
             subtitleMenu_.right,
             offsetXRow.bottom,
             fade(panelLine));
    drawStepperRow(offsetYRow, L"Vertical offset", PixelOffsetText(settings.subtitles.offsetYPx));

    finish();
}

void MainWindow::DrawSectionHeader(HDC hdc, const std::wstring& text, RECT& cursor) const {
    HFONT headerFont = CreateUiFont(Scale(12), FW_SEMIBOLD);
    RECT header = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + Scale(20));
    DrawTextInRect(hdc, text, header, headerFont, palette_.accent, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    cursor.top += Scale(24);
    DeleteObject(headerFont);
}

void MainWindow::DrawField(HDC hdc, const std::wstring& label, const std::wstring& value, RECT& cursor) const {
    const bool stacked = RectWidth(cursor) < Scale(250);
    const int requiredHeight = stacked ? Scale(38) : Scale(24);
    if (cursor.top + requiredHeight > cursor.bottom) {
        return;
    }
    HFONT labelFont = CreateUiFont(Scale(11), FW_NORMAL);
    HFONT valueFont = CreateUiFont(Scale(12), FW_SEMIBOLD);
    if (stacked) {
        RECT labelRect = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + Scale(15));
        RECT valueRect = MakeRect(cursor.left, labelRect.bottom + Scale(1), cursor.right, labelRect.bottom + Scale(20));
        DrawTextInRect(hdc, label, labelRect, labelFont, palette_.dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextInRect(hdc, value, valueRect, valueFont, palette_.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawLine(hdc, cursor.left, cursor.top + Scale(37), cursor.right, cursor.top + Scale(37), BlendColor(palette_.border, palette_.background, 0.35));
        cursor.top += Scale(40);
    } else {
        RECT labelRect = MakeRect(cursor.left, cursor.top, cursor.left + Scale(88), cursor.top + Scale(24));
        RECT valueRect = MakeRect(labelRect.right + Scale(8), cursor.top, cursor.right, cursor.top + Scale(24));
        DrawTextInRect(hdc, label, labelRect, labelFont, palette_.dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextInRect(hdc, value, valueRect, valueFont, palette_.text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawLine(hdc, cursor.left, cursor.top + Scale(25), cursor.right, cursor.top + Scale(25), BlendColor(palette_.border, palette_.background, 0.35));
        cursor.top += Scale(28);
    }
    DeleteObject(labelFont);
    DeleteObject(valueFont);
}

void MainWindow::DrawInspectorPanel(HDC hdc,
                                    const PlaybackSessionSnapshot& snapshot,
                                    const PlayerSettings& settings,
                                    const CapabilityReport& capabilities) const {
    if (SidebarAnimationActive()) {
        FillRectColor(hdc, inspector_, palette_.surface);
        DrawLine(hdc, inspector_.left, inspector_.top, inspector_.left, inspector_.bottom, palette_.border);
        return;
    }

    FillRoundRect(hdc, inspector_, palette_.surface, Scale(12));
    StrokeRoundRect(hdc, inspector_, palette_.border, Scale(12));
    DrawSurfaceRim(hdc,
                   inspector_,
                   BlendColor(palette_.surface, palette_.text, 0.09),
                   BlendColor(palette_.border, palette_.info, 0.20),
                   BlendColor(palette_.border, palette_.accent, 0.14),
                   Scale(14));

    RECT inner = DeflateRectCopy(inspector_, Scale(16), Scale(16));
    HFONT titleFont = CreateUiFont(Scale(16), FW_SEMIBOLD);
    RECT title = MakeRect(inner.left, inner.top, inner.right, inner.top + Scale(22));
    DrawTextInRect(hdc,
                   inspectorTab_ == InspectorTab::Settings ? L"Settings" : L"Inspector",
                   title,
                   titleFont,
                   palette_.text,
                   DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    DeleteObject(titleFont);

    RECT cursor = inner;
    if (showInspectorTabs_ && inspectorTab_ != InspectorTab::Settings) {
        RECT tabRail = MakeRect(inner.left, inner.top + Scale(36), inner.right, inner.top + Scale(68));
        FillRoundRect(hdc, tabRail, BlendColor(palette_.surfaceRaised, palette_.background, 0.28), Scale(7));
        DrawLine(hdc,
                 tabRail.left + Scale(4),
                 tabRail.bottom - Scale(1),
                 tabRail.right - Scale(4),
                 tabRail.bottom - Scale(1),
                 BlendColor(palette_.border, palette_.background, 0.24));
        cursor.top = tabRail.bottom + Scale(14);
    } else {
        cursor.top = title.bottom + Scale(16);
    }
    switch (inspectorTab_) {
    case InspectorTab::Recent:
        DrawRecentContent(hdc, snapshot, cursor);
        break;
    case InspectorTab::Folder:
        DrawFolderContent(hdc, snapshot, cursor);
        break;
    case InspectorTab::Media:
        DrawMediaInfoContent(hdc, snapshot, cursor);
        break;
    case InspectorTab::System:
        DrawSystemContent(hdc, capabilities, cursor);
        break;
    case InspectorTab::Log:
        DrawLogContent(hdc, cursor);
        break;
    case InspectorTab::Settings:
        DrawSettingsContent(hdc,
                            settings,
                            RectWidth(settingsContentViewport_) > 0 ? settingsContentViewport_ : cursor);
        break;
    }
}

void MainWindow::DrawListMessage(HDC hdc, const std::wstring& message, RECT cursor) const {
    const int height = std::min(Scale(56), RectHeight(cursor));
    if (height <= 0) {
        return;
    }

    RECT box = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + height);
    FillRoundRect(hdc, box, BlendColor(palette_.surfaceRaised, palette_.background, 0.16), Scale(7));
    StrokeRoundRect(hdc, box, BlendColor(palette_.border, palette_.background, 0.18), Scale(7));

    HFONT font = CreateUiFont(Scale(12), FW_SEMIBOLD);
    DrawTextInRect(hdc,
                   message,
                   DeflateRectCopy(box, Scale(10), 0),
                   font,
                   palette_.muted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DeleteObject(font);
}

void MainWindow::DrawPathList(HDC hdc,
                              const std::vector<std::filesystem::path>& paths,
                              const std::optional<anvil::playback::MediaDescriptor>& media,
                              RECT& cursor) const {
    if (paths.empty()) {
        return;
    }

    const std::filesystem::path activePath = media.has_value() ? ComparablePath(media->path) : std::filesystem::path{};
    const int rowHeight = Scale(44);
    const int rowGap = Scale(5);
    int visibleRows = 0;

    HFONT titleFont = CreateUiFont(Scale(12), FW_SEMIBOLD);
    HFONT detailFont = CreateUiFont(Scale(10), FW_NORMAL);
    for (const auto& path : paths) {
        if (cursor.top + rowHeight > cursor.bottom) {
            break;
        }

        const bool active = !activePath.empty() && ComparablePath(path) == activePath;
        const bool hovered = visibleRows == hoveredInspectorPathItem_;
        RECT row = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + rowHeight);
        const COLORREF fill = active
                                  ? BlendColor(palette_.accentSoft, palette_.surfaceRaised, hovered ? 0.12 : 0.20)
                                  : (hovered
                                         ? BlendColor(palette_.surfaceRaised, palette_.text, 0.05)
                                         : BlendColor(palette_.surfaceRaised, palette_.background, 0.10));
        FillRoundRect(hdc, row, fill, Scale(7));
        StrokeRoundRect(hdc,
                        row,
                        active ? BlendColor(palette_.borderStrong, palette_.accent, 0.18)
                               : (hovered
                                      ? BlendColor(palette_.borderStrong, palette_.text, 0.08)
                                      : BlendColor(palette_.border, palette_.background, 0.16)),
                        Scale(7));
        if (active) {
            RECT mark = MakeRect(row.left + Scale(7), row.top + Scale(10), row.left + Scale(10), row.bottom - Scale(10));
            FillRoundRect(hdc, mark, palette_.accent, Scale(2));
        }

        const int textLeft = row.left + (active ? Scale(18) : Scale(12));
        RECT nameRect = MakeRect(textLeft, row.top + Scale(6), row.right - Scale(10), row.top + Scale(23));
        RECT pathRect = MakeRect(textLeft, row.top + Scale(24), row.right - Scale(10), row.bottom - Scale(5));
        const std::wstring filename = path.filename().empty() ? path.wstring() : path.filename().wstring();
        const std::wstring parent = path.parent_path().empty() ? L"-" : path.parent_path().wstring();
        DrawTextInRect(hdc,
                       filename,
                       nameRect,
                       titleFont,
                       (active || hovered) ? palette_.text : palette_.muted,
                       DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextInRect(hdc,
                       parent,
                       pathRect,
                       detailFont,
                       active ? BlendColor(palette_.muted, palette_.text, 0.16) : palette_.dim,
                       DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        cursor.top += rowHeight + rowGap;
        ++visibleRows;
    }

    if (visibleRows < static_cast<int>(paths.size()) && cursor.top + Scale(22) <= cursor.bottom) {
        const int remaining = static_cast<int>(paths.size()) - visibleRows;
        RECT moreRect = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + Scale(20));
        DrawTextInRect(hdc,
                       L"+" + std::to_wstring(remaining) + L" more",
                       moreRect,
                       detailFont,
                       palette_.dim,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        cursor.top += Scale(22);
    }

    DeleteObject(titleFont);
    DeleteObject(detailFont);
}

void MainWindow::DrawRecentContent(HDC hdc, const PlaybackSessionSnapshot& snapshot, RECT cursor) const {
    DrawSectionHeader(hdc, L"Recent", cursor);
    if (recentMedia_.empty()) {
        DrawListMessage(hdc, L"No recent media", cursor);
        return;
    }

    DrawPathList(hdc, recentMedia_, snapshot.media, cursor);
}

void MainWindow::DrawFolderContent(HDC hdc, const PlaybackSessionSnapshot& snapshot, RECT cursor) const {
    DrawSectionHeader(hdc, L"Folder", cursor);
    if (!snapshot.media.has_value()) {
        DrawListMessage(hdc, L"No media loaded", cursor);
        return;
    }

    const std::filesystem::path folder = snapshot.media->path.parent_path();
    DrawField(hdc, L"Path", folder.empty() ? L"-" : folder.wstring(), cursor);
    cursor.top += Scale(8);
    DrawSectionHeader(hdc, L"Files", cursor);
    if (currentFolderEntries_.empty()) {
        DrawListMessage(hdc, L"No media files", cursor);
        return;
    }

    DrawPathList(hdc, currentFolderEntries_, snapshot.media, cursor);
}

void MainWindow::DrawMediaInfoContent(HDC hdc, const PlaybackSessionSnapshot& snapshot, RECT cursor) const {
    DrawSectionHeader(hdc, L"Media Info", cursor);
    DrawField(hdc, L"File", FileNameOrDash(snapshot), cursor);
    DrawField(hdc, L"State", ToDisplayString(snapshot.state), cursor);
    DrawField(hdc, L"Position", FormatTimecode(snapshot.position), cursor);
    if (!snapshot.media.has_value()) {
        return;
    }

    cursor.top += Scale(10);
    DrawSectionHeader(hdc, L"Format", cursor);
    DrawField(hdc, L"Container", snapshot.media->container, cursor);
    DrawField(hdc, L"Duration", FormatTimecode(snapshot.media->duration), cursor);
    DrawField(hdc, L"Video", snapshot.media->videoCodec, cursor);
    DrawField(hdc, L"Audio", snapshot.media->audioCodec, cursor);
    DrawField(hdc, L"HDR", snapshot.media->hdrFormat, cursor);
    DrawField(hdc, L"Primaries", ToDisplayString(snapshot.media->videoColor.primaries), cursor);
    DrawField(hdc, L"Transfer", ToDisplayString(snapshot.media->videoColor.transfer), cursor);
    DrawField(hdc, L"Matrix", ToDisplayString(snapshot.media->videoColor.matrix), cursor);
    DrawField(hdc, L"Range", ToDisplayString(snapshot.media->videoColor.range), cursor);
    DrawField(hdc, L"Master", FormatMasteringDisplay(snapshot.media->videoColor.masteringDisplay), cursor);
    DrawField(hdc, L"Light", FormatContentLight(snapshot.media->videoColor.contentLight), cursor);
    DrawField(hdc, L"Plan", snapshot.media->selectedDecodePath, cursor);
    DrawField(hdc, L"Runtime", RuntimeLabel(), cursor);
    if (nativeVideoDecoder_) {
        NativeVideoFrame latest;
        if (nativeVideoDecoder_->LatestFrame(latest)) {
            std::wstring dynamicMetadata = latest.dynamicMetadataPath;
            if (dynamicMetadata.empty() && latest.dovi && latest.dovi->valid) {
                dynamicMetadata = L"dolby_vision_shader";
            }
            if (!dynamicMetadata.empty()) {
                if (!latest.dynamicMetadataDetails.empty()) {
                    dynamicMetadata += L" " + latest.dynamicMetadataDetails;
                }
                DrawField(hdc, L"Dynamic", dynamicMetadata, cursor);
            }
        }
    }
}

void MainWindow::DrawSystemContent(HDC hdc, const CapabilityReport& capabilities, RECT cursor) const {
    DrawSectionHeader(hdc, L"Display", cursor);
    DrawField(hdc, L"HDR", capabilities.display.hdrEnabled ? L"On" : (capabilities.display.hdrSupported ? L"Supported" : L"Off"), cursor);
    DrawField(hdc, L"Color", capabilities.display.colorSpace, cursor);
    DrawField(hdc,
              L"Peak",
              capabilities.display.reportedPeakBrightnessNits > 0 ? std::to_wstring(capabilities.display.reportedPeakBrightnessNits) + L" nits" : L"-",
              cursor);

    cursor.top += Scale(8);
    DrawSectionHeader(hdc, L"GPU", cursor);
    DrawField(hdc, L"Adapter", capabilities.gpu.adapterName, cursor);
    DrawField(hdc, L"Feature", capabilities.gpu.d3dFeatureLevel, cursor);
    DrawField(hdc, L"Decode", JoinStrings(capabilities.gpu.hardwareDecodeProfiles, L", "), cursor);

    cursor.top += Scale(8);
    DrawSectionHeader(hdc, L"Audio", cursor);
    DrawField(hdc, L"Endpoint", capabilities.audio.endpointName, cursor);
    DrawField(hdc, L"Formats", JoinStrings(capabilities.audio.encodedFormats, L", "), cursor);
}

void MainWindow::DrawLogContent(HDC hdc, RECT cursor) const {
    DrawSectionHeader(hdc, L"Log", cursor);
    HFONT logFont = CreateMonoFont(Scale(11));
    RECT logRect = MakeRect(cursor.left, cursor.top, cursor.right, cursor.bottom);
    FillRoundRect(hdc, logRect, RGB(11, 13, 17), Scale(7));
    StrokeRoundRect(hdc, logRect, palette_.border, Scale(7));
    const auto latestLog = controller_.LogSink()->FormatLatest(16);
    DrawTextInRect(hdc,
                   latestLog.empty() ? L"-" : latestLog,
                   DeflateRectCopy(logRect, Scale(10), Scale(9)),
                   logFont,
                   palette_.muted,
                   DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    DeleteObject(logFont);
}

void MainWindow::DrawSettingsContent(HDC hdc, const PlayerSettings& settings, RECT cursor) const {
    const RECT viewport = RectWidth(settingsContentViewport_) > 0 ? settingsContentViewport_ : cursor;
    HRGN clip = CreateRectRgn(viewport.left, viewport.top, viewport.right + 1, viewport.bottom + 1);
    SelectClipRgn(hdc, clip);

    cursor = viewport;
    cursor.top -= settingsScrollOffset_;
    cursor.bottom = cursor.top + std::max(settingsContentHeight_, RectHeight(viewport));

    DrawSectionHeader(hdc, L"Video", cursor);
    DrawField(hdc, L"Decode", ToDisplayString(settings.video.hardwareDecode), cursor);
    DrawField(hdc, L"Track", VideoSelectionText(settings.video.selectedTrackIndex), cursor);
    DrawField(hdc, L"Renderer", settings.video.renderer, cursor);
    DrawField(hdc, L"HDR mode", ToDisplayString(settings.video.hdrOutput), cursor);
    DrawField(hdc, L"Tone map", ToDisplayString(settings.video.toneMapping), cursor);
    DrawField(hdc, L"Dolby Vision", ToDisplayString(settings.video.dolbyVision), cursor);
    if (CurrentMediaHasHdrControls()) {
        DrawField(hdc,
                  CurrentMediaHasCmv4Control() ? L"Dolby Vision" : L"HDR Output",
                  settings.video.dolbyVisionHdrOutput ? L"On" : L"Off",
                  cursor);
    }
    if (CurrentMediaHasCmv4Control()) {
        DrawField(hdc,
                  L"Dolby Vision Enhanced",
                  CurrentCmv4ControlEnabled(settings)
                      ? (settings.video.dolbyVisionCmv4Approx ? L"On" : L"Off")
                      : L"Disabled",
                  cursor);
    }
    if (HdrToneCurveAvailable(settings)) {
        cursor.top += Scale(8);
        DrawHdrToneCurveEditor(hdc, settings, cursor);
    }

    cursor.top += Scale(8);
    DrawSectionHeader(hdc, L"Audio", cursor);
    DrawField(hdc, L"Output", settings.audio.outputDevice, cursor);
    DrawField(hdc, L"Mode", ToDisplayString(settings.audio.outputMode), cursor);
    DrawField(hdc, L"WASAPI", ToDisplayString(settings.audio.wasapiMode), cursor);

    cursor.top += Scale(8);
    DrawSectionHeader(hdc, L"Subtitles", cursor);
    DrawField(hdc, L"Track", SubtitleSelectionText(settings.subtitles.selectedTrackIndex), cursor);
    DrawField(hdc, L"Language", settings.subtitles.preferredLanguage, cursor);
    std::wostringstream scale;
    scale << static_cast<int>(settings.subtitles.fontScale * 100.0) << L"%";
    DrawField(hdc, L"Font", scale.str(), cursor);
    DrawField(hdc, L"External", settings.subtitles.externalSubtitleAutoLoad ? L"On" : L"Off", cursor);

    SelectClipRgn(hdc, nullptr);
    DeleteObject(clip);
    DrawSettingsScrollbar(hdc);
}

void MainWindow::DrawSettingsScrollbar(HDC hdc) const {
    if (settingsScrollMax_ <= 0 ||
        RectWidth(settingsScrollTrack_) <= 0 ||
        RectHeight(settingsScrollTrack_) <= 0 ||
        RectWidth(settingsScrollThumb_) <= 0 ||
        RectHeight(settingsScrollThumb_) <= 0) {
        return;
    }
    const double scrollbarOpacity = ScrollbarOpacity(settingsScrollLastActiveAt_,
                                                     draggingSettingsScrollThumb_,
                                                     std::chrono::steady_clock::now());
    if (scrollbarOpacity <= 0.02) {
        return;
    }

    FillRoundRect(hdc, settingsScrollTrack_, FadeForOpacity(RGB(10, 12, 16), scrollbarOpacity), Scale(3));
    FillRoundRect(hdc,
                  settingsScrollThumb_,
                  FadeForOpacity(draggingSettingsScrollThumb_
                                     ? BlendColor(palette_.accent, palette_.text, 0.16)
                                     : BlendColor(palette_.borderStrong, palette_.muted, 0.24),
                                 scrollbarOpacity),
                  Scale(3));
}

void MainWindow::DrawHdrToneCurveEditor(HDC hdc, const PlayerSettings& settings, RECT& cursor) const {
    const int editorHeight = Scale(190);
    if (hdrToneCurveExpanded_) {
        cursor.top += editorHeight + Scale(12);
        return;
    }
    if (cursor.top + editorHeight > cursor.bottom ||
        RectWidth(hdrToneCurvePlot_) <= 0 ||
        RectHeight(hdrToneCurvePlot_) <= 0) {
        return;
    }

    const RECT editor = MakeRect(cursor.left, cursor.top, cursor.right, cursor.top + editorHeight);
    FillRoundRect(hdc, editor, RGB(11, 13, 17), Scale(7));
    StrokeRoundRect(hdc, editor, palette_.border, Scale(7));

    HFONT titleFont = CreateUiFont(Scale(11), FW_SEMIBOLD);
    HFONT valueFont = CreateMonoFont(Scale(10), FW_SEMIBOLD);
    HFONT axisFont = CreateUiFont(Scale(9), FW_NORMAL);

    RECT title = MakeRect(editor.left + Scale(10), editor.top + Scale(8), editor.right - Scale(78), editor.top + Scale(28));
    DrawTextInRect(hdc, L"HDR Curve", title, titleFont, palette_.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT peak = MakeRect(editor.left + Scale(10), editor.top + Scale(30), editor.right - Scale(12), editor.top + Scale(46));
    DrawTextInRect(hdc,
                   L"Peak " + NitsLabel(std::clamp(settings.video.hdrToneCurve.back().outputNits, 0.0, kHdrToneCurveMaxNits)),
                   peak,
                   valueFont,
                   palette_.muted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const RECT plot = hdrToneCurvePlot_;
    FillRoundRect(hdc, plot, RGB(6, 8, 11), Scale(5));
    StrokeRoundRect(hdc, plot, BlendColor(palette_.border, palette_.text, 0.05), Scale(5));

    const double gridNits[] = {0.0, 250.0, 500.0, 750.0, 1000.0, 2000.0, 4000.0};
    for (const double nits : gridNits) {
        const int x = ToneCurveX(plot, nits);
        const int y = ToneCurveY(plot, nits);
        const COLORREF gridColor = nits == 0.0 || nits == 1000.0
                                       ? BlendColor(palette_.borderStrong, palette_.background, 0.18)
                                       : BlendColor(palette_.border, palette_.background, 0.35);
        DrawLine(hdc, x, plot.top, x, plot.bottom, gridColor);
        DrawLine(hdc, plot.left, y, plot.right, y, gridColor);

        RECT xLabel = MakeRect(x - Scale(20), plot.bottom + Scale(5), x + Scale(20), plot.bottom + Scale(20));
        DrawTextInRect(hdc, ToneCurveAxisLabel(nits), xLabel, axisFont, palette_.dim, DT_CENTER | DT_TOP | DT_SINGLELINE);
        RECT yLabel = MakeRect(editor.left + Scale(8), y - Scale(7), plot.left - Scale(7), y + Scale(8));
        DrawTextInRect(hdc, ToneCurveAxisLabel(nits), yLabel, axisFont, palette_.dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    if (selectingHdrToneCurveRange_) {
        const int left = std::clamp(std::min(hdrToneCurveSelectionStartX_, hdrToneCurveSelectionCurrentX_),
                                    static_cast<int>(plot.left),
                                    static_cast<int>(plot.right));
        const int right = std::clamp(std::max(hdrToneCurveSelectionStartX_, hdrToneCurveSelectionCurrentX_),
                                     static_cast<int>(plot.left),
                                     static_cast<int>(plot.right));
        if (right > left) {
            RECT selection = MakeRect(left, plot.top, right, plot.bottom);
            FillRectColor(hdc, selection, BlendColor(RGB(6, 8, 11), palette_.accent, 0.16));
            DrawLine(hdc, left, plot.top, left, plot.bottom, BlendColor(palette_.accent, palette_.text, 0.22));
            DrawLine(hdc, right, plot.top, right, plot.bottom, BlendColor(palette_.accent, palette_.text, 0.22));
        }
    }

    HPEN referencePen = CreatePen(PS_SOLID, Scale(1), BlendColor(palette_.info, palette_.muted, 0.35));
    HGDIOBJ oldPen = SelectObject(hdc, referencePen);
    bool first = true;
    for (const auto& curvePoint : anvil::playback::kDefaultHdrToneCurve) {
        const int x = ToneCurveX(plot, curvePoint.inputNits);
        const int y = ToneCurveY(plot, curvePoint.outputNits);
        if (first) {
            MoveToEx(hdc, x, y, nullptr);
            first = false;
        } else {
            LineTo(hdc, x, y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(referencePen);

    HPEN curvePen = CreatePen(PS_SOLID, Scale(2), palette_.accent);
    oldPen = SelectObject(hdc, curvePen);
    first = true;
    for (std::size_t index = 0; index < settings.video.hdrToneCurve.size(); ++index) {
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : settings.video.hdrToneCurve[index].inputNits;
        const auto& curvePoint = settings.video.hdrToneCurve[index];
        const int x = ToneCurveX(plot, inputNits);
        const int y = ToneCurveY(plot, curvePoint.outputNits);
        if (first) {
            MoveToEx(hdc, x, y, nullptr);
            first = false;
        } else {
            LineTo(hdc, x, y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(curvePen);

    for (std::size_t index = 0; index < settings.video.hdrToneCurve.size(); ++index) {
        const auto& curvePoint = settings.video.hdrToneCurve[index];
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : curvePoint.inputNits;
        const int x = ToneCurveX(plot, inputNits);
        const int y = ToneCurveY(plot, curvePoint.outputNits);
        const bool selected = index < selectedHdrToneCurvePoints_.size() && selectedHdrToneCurvePoints_[index];
        const bool active = hoveredHdrToneCurvePoint_ == static_cast<int>(index) ||
                            (draggingHdrToneCurve_ && draggedHdrToneCurvePoint_ == static_cast<int>(index));
        const int radius = active ? Scale(6) : (selected ? Scale(5) : Scale(4));
        const COLORREF fill = index == 0
                                  ? palette_.dim
                                  : (selected ? BlendColor(palette_.accent, palette_.text, 0.36) : (active ? palette_.text : palette_.accent));
        HBRUSH brush = CreateSolidBrush(fill);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HPEN pen = CreatePen(PS_SOLID, selected || active ? Scale(2) : Scale(1), BlendColor(fill, RGB(0, 0, 0), 0.22));
        oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, x - radius, y - radius, x + radius + 1, y + radius + 1);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    const int valuePoint = draggingHdrToneCurve_ && draggedHdrToneCurvePoint_ > 0
                               ? draggedHdrToneCurvePoint_
                               : hoveredHdrToneCurvePoint_;
    if (valuePoint > 0 && valuePoint < static_cast<int>(settings.video.hdrToneCurve.size())) {
        const std::size_t index = static_cast<std::size_t>(valuePoint);
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : settings.video.hdrToneCurve[index].inputNits;
        const int x = ToneCurveX(plot, inputNits);
        const int y = ToneCurveY(plot, settings.video.hdrToneCurve[index].outputNits);
        const std::wstring valueText = NitsLabel(settings.video.hdrToneCurve[index].outputNits);

        HGDIOBJ oldFont = SelectObject(hdc, valueFont);
        SIZE textSize{};
        GetTextExtentPoint32W(hdc, valueText.c_str(), static_cast<int>(valueText.size()), &textSize);
        SelectObject(hdc, oldFont);

        const int tipWidth = textSize.cx + Scale(14);
        const int tipHeight = textSize.cy + Scale(8);
        int tipLeft = x - tipWidth / 2;
        int tipTop = y - tipHeight - Scale(10);
        tipLeft = std::clamp(tipLeft,
                             static_cast<int>(editor.left) + Scale(6),
                             static_cast<int>(editor.right) - tipWidth - Scale(6));
        if (tipTop < editor.top + Scale(6)) {
            tipTop = y + Scale(10);
        }
        RECT tip = MakeRect(tipLeft, tipTop, tipLeft + tipWidth, tipTop + tipHeight);
        FillRoundRect(hdc, tip, RGB(13, 16, 21), Scale(6));
        StrokeRoundRect(hdc, tip, BlendColor(palette_.borderStrong, palette_.accent, 0.18), Scale(6));
        DrawTextInRect(hdc,
                       valueText,
                       DeflateRectCopy(tip, Scale(7), Scale(4)),
                       valueFont,
                       palette_.text,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DeleteObject(titleFont);
    DeleteObject(valueFont);
    DeleteObject(axisFont);
    cursor.top = editor.bottom + Scale(12);
}

void MainWindow::DrawHdrToneCurveExpandedEditor(HDC hdc, const PlayerSettings& settings) const {
    if (RectWidth(hdrToneCurveExpandedEditor_) <= 0 ||
        RectHeight(hdrToneCurveExpandedEditor_) <= 0 ||
        RectWidth(hdrToneCurvePlot_) <= 0 ||
        RectHeight(hdrToneCurvePlot_) <= 0) {
        return;
    }

    RECT client{};
    GetClientRect(hdrToneCurveWindow_ ? hdrToneCurveWindow_ : hwnd_, &client);
    FillRectColor(hdc, client, RGB(5, 7, 10));

    const RECT editor = hdrToneCurveExpandedEditor_;
    FillRoundRect(hdc, editor, palette_.surface, Scale(10));
    StrokeRoundRect(hdc, editor, palette_.borderStrong, Scale(10));

    HFONT titleFont = CreateUiFont(Scale(17), FW_SEMIBOLD);
    HFONT valueFont = CreateMonoFont(Scale(11), FW_SEMIBOLD);
    HFONT axisFont = CreateUiFont(Scale(9), FW_NORMAL);

    RECT title = MakeRect(editor.left + Scale(22), editor.top + Scale(16), editor.right - Scale(180), editor.top + Scale(42));
    DrawTextInRect(hdc, L"HDR Curve Editor", title, titleFont, palette_.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT peak = MakeRect(editor.left + Scale(22), editor.top + Scale(44), editor.right - Scale(30), editor.top + Scale(62));
    DrawTextInRect(hdc,
                   L"Peak " + NitsLabel(std::clamp(settings.video.hdrToneCurve.back().outputNits, 0.0, kHdrToneCurveMaxNits)),
                   peak,
                   valueFont,
                   palette_.muted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (RectWidth(hdrToneCurveFloatingReset_) > 0 && RectHeight(hdrToneCurveFloatingReset_) > 0) {
        FillRoundRect(hdc, hdrToneCurveFloatingReset_, RGB(10, 12, 16), Scale(7));
        StrokeRoundRect(hdc, hdrToneCurveFloatingReset_, palette_.border, Scale(7));
        DrawTextInRect(hdc,
                       L"Reset",
                       hdrToneCurveFloatingReset_,
                       valueFont,
                       palette_.muted,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const RECT plot = hdrToneCurvePlot_;
    FillRoundRect(hdc, plot, RGB(6, 8, 11), Scale(6));
    StrokeRoundRect(hdc, plot, BlendColor(palette_.borderStrong, palette_.text, 0.08), Scale(6));

    for (int nits = 0; nits <= 1000; nits += 50) {
        const bool major = nits % 250 == 0;
        const COLORREF gridColor = major
                                       ? BlendColor(palette_.borderStrong, palette_.background, 0.18)
                                       : BlendColor(palette_.border, palette_.background, 0.58);
        const int x = ToneCurveX(plot, static_cast<double>(nits));
        const int y = ToneCurveY(plot, static_cast<double>(nits));
        DrawLine(hdc, x, plot.top, x, plot.bottom, gridColor);
        DrawLine(hdc, plot.left, y, plot.right, y, gridColor);
        if (major) {
            RECT xLabel = MakeRect(x - Scale(22), plot.bottom + Scale(6), x + Scale(22), plot.bottom + Scale(22));
            DrawTextInRect(hdc, ToneCurveAxisLabel(nits), xLabel, axisFont, palette_.dim, DT_CENTER | DT_TOP | DT_SINGLELINE);
            RECT yLabel = MakeRect(editor.left + Scale(16), y - Scale(7), plot.left - Scale(8), y + Scale(8));
            DrawTextInRect(hdc, ToneCurveAxisLabel(nits), yLabel, axisFont, palette_.dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    const double highTicks[] = {1500.0, 2000.0, 3000.0, 4000.0};
    for (const double nits : highTicks) {
        const int x = ToneCurveX(plot, nits);
        const int y = ToneCurveY(plot, nits);
        const COLORREF gridColor = BlendColor(palette_.border, palette_.background, 0.42);
        DrawLine(hdc, x, plot.top, x, plot.bottom, gridColor);
        DrawLine(hdc, plot.left, y, plot.right, y, gridColor);
        RECT xLabel = MakeRect(x - Scale(24), plot.bottom + Scale(6), x + Scale(24), plot.bottom + Scale(22));
        DrawTextInRect(hdc, ToneCurveAxisLabel(nits), xLabel, axisFont, palette_.dim, DT_CENTER | DT_TOP | DT_SINGLELINE);
        RECT yLabel = MakeRect(editor.left + Scale(16), y - Scale(7), plot.left - Scale(8), y + Scale(8));
        DrawTextInRect(hdc, ToneCurveAxisLabel(nits), yLabel, axisFont, palette_.dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    if (selectingHdrToneCurveRange_) {
        const int left = std::clamp(std::min(hdrToneCurveSelectionStartX_, hdrToneCurveSelectionCurrentX_),
                                    static_cast<int>(plot.left),
                                    static_cast<int>(plot.right));
        const int right = std::clamp(std::max(hdrToneCurveSelectionStartX_, hdrToneCurveSelectionCurrentX_),
                                     static_cast<int>(plot.left),
                                     static_cast<int>(plot.right));
        if (right > left) {
            RECT selection = MakeRect(left, plot.top, right, plot.bottom);
            FillRectColor(hdc, selection, BlendColor(RGB(6, 8, 11), palette_.accent, 0.18));
            DrawLine(hdc, left, plot.top, left, plot.bottom, BlendColor(palette_.accent, palette_.text, 0.22));
            DrawLine(hdc, right, plot.top, right, plot.bottom, BlendColor(palette_.accent, palette_.text, 0.22));
        }
    }

    HPEN referencePen = CreatePen(PS_SOLID, Scale(1), BlendColor(palette_.info, palette_.muted, 0.35));
    HGDIOBJ oldPen = SelectObject(hdc, referencePen);
    bool first = true;
    for (const auto& curvePoint : anvil::playback::kDefaultHdrToneCurve) {
        const int x = ToneCurveX(plot, curvePoint.inputNits);
        const int y = ToneCurveY(plot, curvePoint.outputNits);
        if (first) {
            MoveToEx(hdc, x, y, nullptr);
            first = false;
        } else {
            LineTo(hdc, x, y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(referencePen);

    HPEN curvePen = CreatePen(PS_SOLID, Scale(2), palette_.accent);
    oldPen = SelectObject(hdc, curvePen);
    first = true;
    for (std::size_t index = 0; index < settings.video.hdrToneCurve.size(); ++index) {
        const auto& curvePoint = settings.video.hdrToneCurve[index];
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : curvePoint.inputNits;
        const int x = ToneCurveX(plot, inputNits);
        const int y = ToneCurveY(plot, curvePoint.outputNits);
        if (first) {
            MoveToEx(hdc, x, y, nullptr);
            first = false;
        } else {
            LineTo(hdc, x, y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(curvePen);

    for (std::size_t index = 0; index < settings.video.hdrToneCurve.size(); ++index) {
        const auto& curvePoint = settings.video.hdrToneCurve[index];
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : curvePoint.inputNits;
        const int x = ToneCurveX(plot, inputNits);
        const int y = ToneCurveY(plot, curvePoint.outputNits);
        const bool selected = index < selectedHdrToneCurvePoints_.size() && selectedHdrToneCurvePoints_[index];
        const bool active = hoveredHdrToneCurvePoint_ == static_cast<int>(index) ||
                            (draggingHdrToneCurve_ && draggedHdrToneCurvePoint_ == static_cast<int>(index));
        const int radius = active ? Scale(7) : (selected ? Scale(6) : Scale(5));
        const COLORREF fill = index == 0
                                  ? palette_.dim
                                  : (selected ? BlendColor(palette_.accent, palette_.text, 0.36) : (active ? palette_.text : palette_.accent));
        HBRUSH brush = CreateSolidBrush(fill);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HPEN pen = CreatePen(PS_SOLID, selected || active ? Scale(2) : Scale(1), BlendColor(fill, RGB(0, 0, 0), 0.22));
        oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, x - radius, y - radius, x + radius + 1, y + radius + 1);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    const int valuePoint = draggingHdrToneCurve_ && draggedHdrToneCurvePoint_ > 0
                               ? draggedHdrToneCurvePoint_
                               : hoveredHdrToneCurvePoint_;
    if (valuePoint > 0 && valuePoint < static_cast<int>(settings.video.hdrToneCurve.size())) {
        const std::size_t index = static_cast<std::size_t>(valuePoint);
        const double inputNits = index < anvil::playback::kDefaultHdrToneCurve.size()
                                     ? anvil::playback::kDefaultHdrToneCurve[index].inputNits
                                     : settings.video.hdrToneCurve[index].inputNits;
        const int x = ToneCurveX(plot, inputNits);
        const int y = ToneCurveY(plot, settings.video.hdrToneCurve[index].outputNits);
        const std::wstring valueText = NitsLabel(settings.video.hdrToneCurve[index].outputNits);

        HGDIOBJ oldFont = SelectObject(hdc, valueFont);
        SIZE textSize{};
        GetTextExtentPoint32W(hdc, valueText.c_str(), static_cast<int>(valueText.size()), &textSize);
        SelectObject(hdc, oldFont);

        const int tipWidth = textSize.cx + Scale(16);
        const int tipHeight = textSize.cy + Scale(10);
        int tipLeft = x - tipWidth / 2;
        int tipTop = y - tipHeight - Scale(12);
        tipLeft = std::clamp(tipLeft,
                             static_cast<int>(editor.left) + Scale(8),
                             static_cast<int>(editor.right) - tipWidth - Scale(8));
        if (tipTop < editor.top + Scale(8)) {
            tipTop = y + Scale(12);
        }
        RECT tip = MakeRect(tipLeft, tipTop, tipLeft + tipWidth, tipTop + tipHeight);
        FillRoundRect(hdc, tip, RGB(13, 16, 21), Scale(6));
        StrokeRoundRect(hdc, tip, BlendColor(palette_.borderStrong, palette_.accent, 0.18), Scale(6));
        DrawTextInRect(hdc,
                       valueText,
                       DeflateRectCopy(tip, Scale(8), Scale(5)),
                       valueFont,
                       palette_.text,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    DeleteObject(titleFont);
    DeleteObject(valueFont);
    DeleteObject(axisFont);
}

}  // namespace anvil::app
