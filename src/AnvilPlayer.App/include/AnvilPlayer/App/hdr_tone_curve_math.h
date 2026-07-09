#pragma once

// Shared HDR tone-curve coordinate math.
// paint.cpp and input.cpp carried byte-identical copies of these constants and
// nits<->unit/plot conversions; consolidated here so the draw and hit-test
// paths always agree on the projection.

#include "AnvilPlayer/App/ui_draw.h"

#include <algorithm>
#include <cmath>

namespace anvil::app {

constexpr double kHdrToneCurveMaxNits = 4000.0;
constexpr double kHdrToneCurveFocusNits = 1000.0;
constexpr double kHdrToneCurveFocusUnit = 0.72;

inline double ToneCurveNitsToUnit(const double nits) {
    const double clamped = std::clamp(nits, 0.0, kHdrToneCurveMaxNits);
    if (clamped <= kHdrToneCurveFocusNits) {
        return (clamped / kHdrToneCurveFocusNits) * kHdrToneCurveFocusUnit;
    }
    return kHdrToneCurveFocusUnit +
           ((clamped - kHdrToneCurveFocusNits) / (kHdrToneCurveMaxNits - kHdrToneCurveFocusNits)) *
               (1.0 - kHdrToneCurveFocusUnit);
}

inline double UnitToToneCurveNits(const double unit) {
    const double clamped = std::clamp(unit, 0.0, 1.0);
    if (clamped <= kHdrToneCurveFocusUnit) {
        return (clamped / kHdrToneCurveFocusUnit) * kHdrToneCurveFocusNits;
    }
    return kHdrToneCurveFocusNits +
           ((clamped - kHdrToneCurveFocusUnit) / (1.0 - kHdrToneCurveFocusUnit)) *
               (kHdrToneCurveMaxNits - kHdrToneCurveFocusNits);
}

inline double RoundToneCurveNits(const double nits) {
    const double clamped = std::clamp(nits, 0.0, kHdrToneCurveMaxNits);
    return std::round(clamped);
}

inline int ToneCurveX(const RECT& plot, const double nits) {
    return plot.left + static_cast<int>(std::round(ToneCurveNitsToUnit(nits) * RectWidth(plot)));
}

inline int ToneCurveY(const RECT& plot, const double nits) {
    return plot.bottom - static_cast<int>(std::round(ToneCurveNitsToUnit(nits) * RectHeight(plot)));
}

inline double ToneCurveYToNits(const RECT& plot, const int y) {
    if (RectHeight(plot) <= 0) {
        return 0.0;
    }
    const double unitY = 1.0 - static_cast<double>(y - plot.top) / static_cast<double>(RectHeight(plot));
    return UnitToToneCurveNits(unitY);
}

}  // namespace anvil::app

