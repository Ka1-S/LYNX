#include "core/FlCurve.h"

#include <algorithm>
#include <cmath>

namespace lynx::flcurve
{
namespace
{
constexpr double pi = 3.14159265358979323846;
const double lnMillion = std::log (1000001.0);

// FL's Round: to the nearest, ties to even (the FPU's mode)
double roundFl (double v) noexcept
{
    return std::nearbyint (v);
}

// the single-curve law of modes 0 / 1: c0 = the tension (its sign), c1 = 1 / k, c2 = ln (k + 1), k = 1000001^|t| − 1
double single (const Segment& s, double x) noexcept
{
    if (s.c0 == 0.0f)
        return x;
    return s.c0 > 0.0f ? 1.0 - (std::exp ((1.0 - x) * (double) s.c2) - 1.0) * (double) s.c1 : (std::exp (x * (double) s.c2) - 1.0) * (double) s.c1;
}
} // namespace

int count (int mode, float tension) noexcept
{
    if (mode < stairs || mode > wave)
        return -1;
    const auto e = (float) (1.0 - (double) std::abs (tension) * 0.6);
    const auto n = (int) roundFl ((double) (float) (std::exp ((double) e * lnMillion) - 1.0) * 0.001);
    return mode == stairs && tension >= 0.0f ? n + 1 : n;
}

Segment prepare (int mode, float tension) noexcept
{
    Segment s;
    s.mode = std::clamp (mode, 0, modeCount - 1);
    const auto t = std::isfinite (tension) ? std::clamp (tension, -1.0f, 1.0f) : 0.0f;
    s.sign = t == 0.0f ? 0 : t < 0.0f ? -1 : 1;
    const auto n = count (s.mode, t);
    const auto side = t < 0.0f ? -1 : 1;
    switch (s.mode)
    {
        case singleCurve:
        case doubleCurve:
        {
            const auto k = std::exp ((double) std::abs (t) * lnMillion) - 1.0;
            s.c0 = t;
            s.c1 = k > 0.0 ? (float) (1.0 / k) : 0.0f;
            s.c2 = (float) std::log (k + 1.0);
            break;
        }
        case stairs:
            s.c2 = (float) side * 0.5f;
            s.c0 = (float) (n + 1);
            s.c1 = 1.0f / (s.c0 - (float) side);
            break;
        case smoothStairs:
            s.c0 = (float) (n + 1);
            s.c1 = 1.0f / s.c0;
            break;
        case pulse:
            s.c2 = (float) side * 0.5f;
            s.c0 = (float) ((n + 1) * 2);
            s.c1 = 6.2831855f;
            break;
        case wave:
            s.c0 = (float) ((double) n + 0.5);
            s.c1 = 6.2831855f;
            if (n == 0 && s.sign > 0)
                s.sign = 2;
            break;
        case singleCurve2:
        case doubleCurve2: s.c0 = t; break;
        case halfSine: s.c0 = std::abs (t); break;
        case singleCurve3:
        case doubleCurve3:
        {
            const auto a = std::max ((double) std::abs (t) * 0.9, 1.52587890625e-05);
            const auto b = 1.0 - a;
            s.c0 = (float) (1.0 - b * b * b);
            s.c1 = (1.0f - s.c0) / s.c0;
            s.c2 = s.c1;
            if (s.sign > 0)
            {
                s.c1 = -s.c1;
                s.c2 = s.c1 - 1.0f;
            }
            if (s.mode == doubleCurve3)
            {
                s.c1 = (float) ((double) s.c1 * 0.5);
                s.c2 = (float) ((double) s.c2 * 0.5);
            }
            break;
        }
        default: s.sign = 2; break; // smooth (needs its neighbours), hold
    }
    // zero tension is straight in modes 0, 1, 7, 8, 9, 11, 12 (FL's mask 0x1B83)
    if (t == 0.0f && ((0x1B83 >> s.mode) & 1) != 0)
        s.sign = 2;
    if (s.mode == hold)
        s.sign = 0;
    return s;
}

double evaluate (const Segment& s, double x) noexcept
{
    x = std::clamp (x, 0.0, 1.0);
    if (s.sign == 2 && s.mode != hold)
        return x;
    switch (s.mode)
    {
        case singleCurve: return single (s, x);
        case doubleCurve: return x < 0.5 ? single (s, x * 2.0) * 0.5 : 1.0 - single (s, (1.0 - x) * 2.0) * 0.5;
        case hold: return 0.0;
        case stairs: return roundFl (x * (double) s.c0 - (double) s.c2) * (double) s.c1;
        case smoothStairs:
        {
            auto d = x * (double) s.c0 - 0.5;
            const auto i = roundFl (d - 0.5);
            d = (d - i) * 2.0 - 1.0;
            if (s.sign < 0)
            {
                const auto e = 1.0 - std::abs (d);
                d = (1.0 - e * e * e) * (d < 0.0 ? -1.0 : 1.0);
            }
            else
                d = d * d * d;
            return (i + d * 0.5 + 1.0) * (double) s.c1;
        }
        case pulse: return (double) ((long long) roundFl (x * (double) s.c0 - (double) s.c2) & 1);
        case wave:
            if (s.sign < 0)
                return (std::sin ((x * (double) s.c0 + 0.75) * (double) s.c1) + 1.0) * 0.5;
            else
            {
                const auto d = x * (double) s.c0 - 0.5;
                const auto i = roundFl (d - 0.5);
                return std::abs ((d - i) * 2.0 - 1.0);
            }
        case singleCurve2:
        {
            const auto u = s.sign > 0 ? 1.0 - x : x;
            const auto u3 = u * u * u;
            return x - (u3 * u3 - u) * (double) s.c0;
        }
        case doubleCurve2:
            if (x < 0.5)
            {
                const auto y = x * 2.0;
                const auto u = s.sign > 0 ? 1.0 - y : y;
                const auto u3 = u * u * u;
                return (y - (u3 * u3 - u) * (double) s.c0) * 0.5;
            }
            else
            {
                const auto y = (1.0 - x) * 2.0;
                const auto u = s.sign > 0 ? 1.0 - y : y;
                const auto u3 = u * u * u;
                return 1.0 - (y - (u3 * u3 - u) * (double) s.c0) * 0.5;
            }
        case halfSine:
        {
            const auto g = s.sign < 1 ? 1.0 - std::cos (x * (double) 1.5707963705062866) : std::sin (x * (double) 1.5707963705062866);
            return x - (x - g) * (double) s.c0;
        }
        case singleCurve3:
            return s.sign < 1 ? (double) s.c1 / (1.0 - (double) s.c0 * x) - (double) s.c2 : (double) s.c1 / (1.0 - (double) s.c0 * (1.0 - x)) - (double) s.c2;
        case doubleCurve3:
            if (x < 0.5)
            {
                const auto y = x * 2.0;
                return s.sign < 1 ? (double) s.c1 / (1.0 - (double) s.c0 * y) - (double) s.c2 : (double) s.c1 / (1.0 - (double) s.c0 * (1.0 - y)) - (double) s.c2;
            }
            else
            {
                const auto y = (1.0 - x) * 2.0;
                return s.sign < 1 ? 1.0 - (double) s.c1 / (1.0 - (double) s.c0 * y) + (double) s.c2
                                  : 1.0 - (double) s.c1 / (1.0 - (double) s.c0 * (1.0 - y)) + (double) s.c2;
            }
        default: return x;
    }
}

const char* modeName (int mode) noexcept
{
    static const char* names[] = { "Single curve", "Double curve", "Hold", "Stairs", "Smooth stairs", "Pulse", "Wave",
                                   "Single curve 2", "Double curve 2", "Half sine", "Smooth", "Single curve 3", "Double curve 3" };
    return names[std::clamp (mode, 0, modeCount - 1)];
}
} // namespace lynx::flcurve
