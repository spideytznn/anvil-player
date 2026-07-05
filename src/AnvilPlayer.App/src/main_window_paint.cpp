#include "AnvilPlayer/App/main_window.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>

namespace anvil::app {

using anvil::playback::CapabilityReport;
using anvil::playback::FormatTimecode;
using anvil::playback::PlaybackSessionSnapshot;
using anvil::playback::PlaybackState;
using anvil::playback::PlayerSettings;
using anvil::playback::ToDisplayString;

namespace {

COLORREF FadeForOpacity(const COLORREF color, const double opacity) {
    return BlendColor(RGB(0, 0, 0), color, std::clamp(opacity, 0.0, 1.0));
}

std::wstring PlaybackRateText(const double rate) {
    std::wostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << rate << L"x";
    return stream.str();
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

    if (RectWidth(topBar_) > 0 && RectHeight(topBar_) > 0) {
        DrawTopBar(bufferDc, snapshot);
    }
    if (RectWidth(videoSurface_) > 0 && RectHeight(videoSurface_) > 0) {
        DrawVideoSurface(bufferDc, snapshot);
    }
    if (!fullscreen_ && RectWidth(transportBar_) > 0 && RectHeight(transportBar_) > 0) {
        DrawTransport(bufferDc, snapshot);
    }
    if (RectWidth(inspector_) > 0 && RectHeight(inspector_) > 0) {
        DrawInspectorPanel(bufferDc, snapshot, settings, capabilities);
    }
    if (!fullscreen_) {
        DrawButtons(bufferDc);
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

    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(bufferDc, &paintRect, background);
    DeleteObject(background);

    POINT oldOrigin{};
    SetViewportOrgEx(bufferDc, -transportBar_.left - paintRect.left, -transportBar_.top - paintRect.top, &oldOrigin);

    const auto snapshot = controller_.Snapshot();
    DrawTransport(bufferDc, snapshot);
    DrawButtons(bufferDc);

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

void MainWindow::DrawTopBar(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
    FillRoundRect(hdc, topBar_, palette_.surface, Scale(12));
    StrokeRoundRect(hdc, topBar_, palette_.border, Scale(12));

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

void MainWindow::DrawButtons(HDC hdc) const {
    const double opacity = fullscreen_ ? std::clamp(fullscreenTransportAmount_, 0.0, 1.0) : 1.0;
    if (opacity <= 0.02) {
        return;
    }
    const auto fade = [opacity](const COLORREF color) {
        return FadeForOpacity(color, opacity);
    };

    HFONT buttonFont = CreateUiFont(Scale(11), FW_SEMIBOLD);
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        const auto& button = buttons_[index];
        const bool hovered = static_cast<int>(index) == hoveredButton_;

        if (button.kind == ButtonKind::Tab) {
            COLORREF fill = button.selected ? palette_.surfaceSoft : RGB(11, 13, 17);
            if (hovered && !button.selected) {
                fill = BlendColor(fill, palette_.text, 0.06);
            }
            FillRoundRect(hdc, button.bounds, fade(fill), Scale(7));
            StrokeRoundRect(hdc,
                            button.bounds,
                            fade(button.selected ? BlendColor(palette_.borderStrong, palette_.accent, 0.16) : palette_.border),
                            Scale(7));
            DrawTextInRect(hdc,
                           button.label,
                           DeflateRectCopy(button.bounds, Scale(6), 0),
                           buttonFont,
                           fade(button.selected ? palette_.text : palette_.muted),
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        }

        COLORREF fill = button.primary ? palette_.accent : palette_.surfaceRaised;
        if (hovered) {
            fill = BlendColor(fill, palette_.text, button.primary ? 0.08 : 0.06);
        }
        if (button.selected && !button.primary) {
            fill = BlendColor(fill, palette_.accent, 0.16);
        }

        const int radius = button.kind == ButtonKind::TransportPrimary ? Scale(12) : Scale(8);
        FillRoundRect(hdc, button.bounds, fade(fill), radius);
        StrokeRoundRect(hdc,
                        button.bounds,
                        fade(button.primary ? BlendColor(palette_.accent, palette_.text, 0.14) : BlendColor(palette_.border, palette_.text, 0.03)),
                        radius);

        const COLORREF iconColor = fade(button.primary ? RGB(255, 250, 248) : (button.selected ? palette_.text : palette_.muted));
        const int iconInset = button.kind == ButtonKind::TransportPrimary ? Scale(10) : Scale(8);
        RECT iconRect = DeflateRectCopy(button.bounds, iconInset, iconInset);
        iconPainter_.Draw(hdc, button.icon, iconRect, iconColor);
        if (!button.label.empty()) {
            DrawTextInRect(hdc, button.label, button.bounds, buttonFont, iconColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    DeleteObject(buttonFont);
}

void MainWindow::DrawTooltip(HDC hdc) const {
    if (fullscreen_ && fullscreenTransportAmount_ < 0.95) {
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

void MainWindow::DrawVideoSurface(HDC hdc, const PlaybackSessionSnapshot& snapshot) const {
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
    HFONT titleFont = CreateUiFont(compactSurface ? Scale(18) : Scale(22), FW_SEMIBOLD);
    HFONT emptyFont = CreateUiFont(compactSurface ? Scale(20) : Scale(24), FW_SEMIBOLD);
    HFONT bodyFont = CreateUiFont(compactSurface ? Scale(12) : Scale(13), FW_NORMAL);
    HFONT smallFont = CreateUiFont(Scale(11), FW_NORMAL);

    if (snapshot.media.has_value()) {
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
    } else {
        ClearPreviewBitmap();
        const int tileSize = compactSurface ? Scale(62) : Scale(74);
        RECT tile = MakeRect(inner.left + RectWidth(inner) / 2 - tileSize / 2,
                             inner.top + RectHeight(inner) / 2 - (compactSurface ? Scale(68) : Scale(82)),
                             inner.left + RectWidth(inner) / 2 + tileSize / 2,
                             inner.top + RectHeight(inner) / 2 - Scale(8));
        FillRoundRect(hdc, tile, RGB(13, 16, 20), Scale(12));
        StrokeRoundRect(hdc, tile, palette_.border, Scale(12));
        iconPainter_.Draw(hdc, IconKind::Play, DeflateRectCopy(tile, compactSurface ? Scale(17) : Scale(20), compactSurface ? Scale(17) : Scale(20)), palette_.text);

        RECT title = MakeRect(inner.left, tile.bottom + Scale(18), inner.right, tile.bottom + (compactSurface ? Scale(44) : Scale(54)));
        DrawTextInRect(hdc, L"No media loaded", title, emptyFont, palette_.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT subtitle = MakeRect(inner.left, title.bottom + Scale(2), inner.right, title.bottom + Scale(26));
        DrawTextInRect(hdc, L"Ready", subtitle, bodyFont, palette_.dim, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    DeleteObject(titleFont);
    DeleteObject(emptyFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
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

    const bool compactBar = RectHeight(transportBar_) <= Scale(82);
    const int horizontalInset = compactBar ? Scale(18) : Scale(24);
    HFONT smallFont = CreateUiFont(Scale(11), FW_NORMAL);
    HFONT labelFont = CreateUiFont(Scale(10), FW_SEMIBOLD);
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

    RECT timeLeft = MakeRect(transportBar_.left + horizontalInset, progress_.bottom + Scale(6), transportBar_.left + horizontalInset + Scale(86), progress_.bottom + Scale(24));
    const int transportRightEdge = static_cast<int>(transportBar_.right) - horizontalInset;
    const int durationRight = fullscreen_ && transportRightControlsLeft_ > 0
                                  ? std::min(transportRightControlsLeft_ - Scale(12), transportRightEdge)
                                  : transportRightEdge;
    RECT timeRight = MakeRect(durationRight - Scale(86), progress_.bottom + Scale(6), durationRight, progress_.bottom + Scale(24));
    DrawTextInRect(hdc, FormatTimecode(displayPosition), timeLeft, monoFont, fade(draggingProgress_ ? palette_.accent : palette_.muted), DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (RectWidth(progress_) >= Scale(230) && timeRight.left >= timeLeft.right + Scale(12)) {
        DrawTextInRect(hdc, durationText, timeRight, monoFont, fade(palette_.muted), DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    const int metaTop = transportBar_.top + (compactBar ? Scale(50) : Scale(54));
    const int metaBottom = transportBar_.bottom - (compactBar ? Scale(12) : Scale(16));
    const int fileLeft = transportBar_.left + horizontalInset;
    const int fileRight = transportControlsLeft_ - Scale(16);
    const int fileWidth = fileRight - fileLeft;
    if (snapshot.media.has_value() && fileWidth >= Scale(210)) {
        RECT fileLabel = MakeRect(fileLeft, metaTop, fileLeft + Scale(36), metaBottom);
        DrawTextInRect(hdc, L"FILE", fileLabel, labelFont, fade(palette_.dim), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT file = MakeRect(fileLabel.right + Scale(8), metaTop, fileRight, metaBottom);
        DrawTextInRect(hdc,
                       snapshot.media.has_value() ? snapshot.media->displayName : L"-",
                       file,
                       smallFont,
                       fade(palette_.muted),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (snapshot.media.has_value() && fileWidth >= Scale(112)) {
        RECT file = MakeRect(fileLeft, metaTop, fileRight, metaBottom);
        DrawTextInRect(hdc,
                       snapshot.media->displayName,
                       file,
                       smallFont,
                       fade(palette_.muted),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const int rightMetaLeft = transportControlsRight_ + Scale(16);
    const int rightMetaRight = transportRightControlsLeft_ - Scale(12);
    const int rightMetaWidth = rightMetaRight - rightMetaLeft;
    if (rightMetaWidth >= Scale(138)) {
        RECT speed = MakeRect(rightMetaLeft, metaTop, rightMetaLeft + Scale(52), metaBottom);
        FillRoundRect(hdc, speed, fade(RGB(11, 13, 17)), Scale(7));
        StrokeRoundRect(hdc, speed, fade(palette_.border), Scale(7));
        DrawTextInRect(hdc, PlaybackRateText(snapshot.playbackRate), speed, monoFont, fade(snapshot.playbackRate > 1.01 ? palette_.accent : palette_.muted), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT volume = MakeRect(speed.right + Scale(10), metaTop, rightMetaRight, metaBottom);
        DrawTextInRect(hdc, PercentText(snapshot.volume), volume, smallFont, fade(palette_.muted), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (rightMetaWidth >= Scale(58)) {
        RECT volume = MakeRect(rightMetaLeft, metaTop, rightMetaRight, metaBottom);
        DrawTextInRect(hdc, PercentText(snapshot.volume), volume, smallFont, fade(palette_.muted), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    DeleteObject(smallFont);
    DeleteObject(labelFont);
    DeleteObject(monoFont);
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
    FillRoundRect(hdc, inspector_, palette_.surface, Scale(12));
    StrokeRoundRect(hdc, inspector_, palette_.border, Scale(12));

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
    if (showInspectorTabs_) {
        RECT tabRail = MakeRect(inner.left, inner.top + Scale(38), inner.right, inner.top + Scale(70));
        FillRoundRect(hdc, tabRail, RGB(10, 12, 16), Scale(8));
        StrokeRoundRect(hdc, tabRail, palette_.border, Scale(8));
        cursor.top = tabRail.bottom + Scale(14);
    } else {
        cursor.top = title.bottom + Scale(16);
    }
    switch (inspectorTab_) {
    case InspectorTab::Media:
        DrawMediaContent(hdc, snapshot, cursor);
        break;
    case InspectorTab::Device:
        DrawDeviceContent(hdc, capabilities, cursor);
        break;
    case InspectorTab::Log:
        DrawLogContent(hdc, cursor);
        break;
    case InspectorTab::Settings:
        DrawSettingsContent(hdc, settings, cursor);
        break;
    }
}

void MainWindow::DrawMediaContent(HDC hdc, const PlaybackSessionSnapshot& snapshot, RECT cursor) const {
    DrawSectionHeader(hdc, L"Media", cursor);
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
}

void MainWindow::DrawDeviceContent(HDC hdc, const CapabilityReport& capabilities, RECT cursor) const {
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
    DrawSectionHeader(hdc, L"Video", cursor);
    DrawField(hdc, L"Decode", ToDisplayString(settings.video.hardwareDecode), cursor);
    DrawField(hdc, L"Renderer", settings.video.renderer, cursor);
    DrawField(hdc, L"HDR", ToDisplayString(settings.video.hdrOutput), cursor);
    DrawField(hdc, L"Tone map", ToDisplayString(settings.video.toneMapping), cursor);
    DrawField(hdc, L"Dolby Vision", ToDisplayString(settings.video.dolbyVision), cursor);

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
}

}  // namespace anvil::app
