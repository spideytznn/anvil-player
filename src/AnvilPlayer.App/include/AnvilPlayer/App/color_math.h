#pragma once

// Shared colour-math helpers.
// Pq12CodeToNits existed in two files with a subtle difference: the decoder
// short-circuited code==0 and used a 1e-6 denominator floor, while the
// renderer used a 1e-9 floor. The merged version keeps the more conservative
// decoder behaviour (code==0 fast path + 1e-6 floor) since that is the path
// that actually feeds frame decoding.

#include <cmath>
#include <cstdint>

namespace anvil::app {

// Converts a 12-bit PQ code value to absolute luminance in nits (0..10000).
inline float Pq12CodeToNits(const uint16_t code) {
    if (code == 0) {
        return 0.0f;
    }

    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;

    const double v = std::clamp(static_cast<double>(code) / 4095.0, 0.0, 1.0);
    const double p = std::pow(v, 1.0 / m2);
    const double numerator = std::max(p - c1, 0.0);
    const double denominator = std::max(c2 - c3 * p, 0.000001);
    const double nits = 10000.0 * std::pow(numerator / denominator, 1.0 / m1);
    return std::isfinite(nits) ? static_cast<float>(nits) : 0.0f;
}

}  // namespace anvil::app
