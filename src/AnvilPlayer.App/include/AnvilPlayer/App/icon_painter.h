#pragma once

#include "AnvilPlayer/App/ui_draw.h"
#include "AnvilPlayer/App/ui_types.h"

#include <objidl.h>
#include <windows.h>
#include <gdiplus.h>

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace anvil::app {

// Renders transport/inspector icons. Loads PNG assets from disk on demand and
// caches them; falls back to hand-drawn GDI+ vector shapes when an asset is
// missing. Owns the icon asset-directory probe and the bitmap cache.
class IconPainter {
public:
    IconPainter() = default;
    ~IconPainter() = default;
    IconPainter(const IconPainter&) = delete;
    IconPainter& operator=(const IconPainter&) = delete;

    // Draws the icon into bounds. Returns true when a PNG asset was used,
    // false when the vector fallback was drawn (or the icon is None).
    // const-safe: lazy asset loading mutates only mutable cache members.
    bool Draw(HDC hdc, IconKind icon, RECT bounds, COLORREF color) const;

private:
    std::filesystem::path ModuleDirectory() const;
    const std::filesystem::path& IconAssetDirectory() const;
    Gdiplus::Bitmap* LoadIconBitmap(IconKind icon) const;
    bool DrawAsset(HDC hdc, IconKind icon, RECT bounds) const;
    void DrawVector(HDC hdc, IconKind icon, RECT bounds, COLORREF color) const;

    mutable std::filesystem::path iconAssetDirectory_;
    mutable std::vector<std::pair<IconKind, std::unique_ptr<Gdiplus::Bitmap>>> iconBitmaps_;
};

}  // namespace anvil::app
