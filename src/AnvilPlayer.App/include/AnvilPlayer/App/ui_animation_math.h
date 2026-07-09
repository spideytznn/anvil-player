#pragma once

// UI animation easing math.
// These motion-curve primitives and animation duration constants were
// inlined in main_window.cpp's anonymous namespace. They are pure math with
// no MainWindow dependency, so they collapse to a shared header that the
// window, paint and input files can all include.

#include <chrono>
#include <cmath>

namespace anvil::app {

// Animation duration constants (milliseconds).
constexpr auto kSidebarAnimationDuration = std::chrono::milliseconds{460};
constexpr auto kProgressHoverAnimationDuration = std::chrono::milliseconds{180};
constexpr auto kFullscreenTransportAnimationDuration = std::chrono::milliseconds{260};
constexpr auto kSubtitleMenuAnimationDuration = std::chrono::milliseconds{240};
constexpr auto kWebUiSubtitleMenuOpenAnimationDuration = std::chrono::milliseconds{560};
constexpr auto kWebUiSubtitleMenuCloseAnimationDuration = std::chrono::milliseconds{260};
constexpr auto kButtonHoverAnimationDuration = std::chrono::milliseconds{180};
constexpr auto kButtonPressAnimationDuration = std::chrono::milliseconds{220};
constexpr auto kVolumeHoverAnimationDuration = std::chrono::milliseconds{180};

// A single animated scalar with its start/target/progress state.
struct UiMotionValue {
    double amount = 0.0;
    double startAmount = 0.0;
    double target = 0.0;
    std::chrono::steady_clock::time_point startedAt{};
};

enum class MotionCurve {
    Fluid,
    Spring,
    Press,
    Fade,
    EaseIn,
};

inline double Cubic(const double a,
                    const double b,
                    const double c,
                    const double d,
                    const double t) {
    const double inverse = 1.0 - t;
    return inverse * inverse * inverse * a +
           3.0 * inverse * inverse * t * b +
           3.0 * inverse * t * t * c +
           t * t * t * d;
}

inline double CubicDerivative(const double p1, const double p2, const double t) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * p1 +
           6.0 * inverse * t * (p2 - p1) +
           3.0 * t * t * (1.0 - p2);
}

inline double CubicBezierEase(const double value,
                              const double x1,
                              const double y1,
                              const double x2,
                              const double y2) {
    const double x = std::clamp(value, 0.0, 1.0);
    if (x <= 0.0 || x >= 1.0) {
        return x;
    }

    double t = x;
    for (int i = 0; i < 5; ++i) {
        const double currentX = Cubic(0.0, x1, x2, 1.0, t);
        const double derivative = CubicDerivative(x1, x2, t);
        if (std::abs(derivative) < 0.000001) {
            break;
        }
        const double next = t - (currentX - x) / derivative;
        if (next < 0.0 || next > 1.0) {
            break;
        }
        t = next;
    }

    double lower = 0.0;
    double upper = 1.0;
    for (int i = 0; i < 8; ++i) {
        const double currentX = Cubic(0.0, x1, x2, 1.0, t);
        if (std::abs(currentX - x) < 0.00001) {
            break;
        }
        if (currentX < x) {
            lower = t;
        } else {
            upper = t;
        }
        t = (lower + upper) * 0.5;
    }

    return Cubic(0.0, y1, y2, 1.0, t);
}

inline double Ease(const double value, const MotionCurve curve) {
    switch (curve) {
    case MotionCurve::Fluid:
        return CubicBezierEase(value, 0.16, 1.0, 0.30, 1.0);
    case MotionCurve::Spring:
        return CubicBezierEase(value, 0.18, 1.35, 0.28, 1.0);
    case MotionCurve::Press:
        return CubicBezierEase(value, 0.18, 1.20, 0.28, 1.0);
    case MotionCurve::Fade:
        return CubicBezierEase(value, 0.22, 1.0, 0.36, 1.0);
    case MotionCurve::EaseIn:
        return CubicBezierEase(value, 0.32, 0.0, 0.67, 0.0);
    }
    return value;
}

inline double AnimatedValue(const double from,
                            const double to,
                            const std::chrono::steady_clock::time_point startedAt,
                            const std::chrono::milliseconds duration,
                            const std::chrono::steady_clock::time_point now,
                            const MotionCurve curve,
                            bool& complete) {
    if (duration.count() <= 0 || startedAt.time_since_epoch().count() == 0) {
        complete = true;
        return to;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt);
    const double t = std::clamp(static_cast<double>(elapsed.count()) / static_cast<double>(duration.count()), 0.0, 1.0);
    complete = t >= 1.0;
    return from + (to - from) * Ease(t, curve);
}

inline bool UpdateMotionValue(UiMotionValue& value,
                              const std::chrono::milliseconds duration,
                              const std::chrono::steady_clock::time_point now,
                              const MotionCurve curve,
                              bool& complete) {
    const double previous = value.amount;
    value.amount = AnimatedValue(value.startAmount,
                                 value.target,
                                 value.startedAt,
                                 duration,
                                 now,
                                 curve,
                                 complete);
    if (complete) {
        value.amount = value.target;
    }
    return std::abs(value.amount - previous) > 0.0001;
}

}  // namespace anvil::app
