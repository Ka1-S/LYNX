#pragma once

// Round 166f (issue #16): Parametric EQ 2's spectrum heatmap — what FL Studio 2026's Parametric EQ 2 shows with
// Options › Heatmap › Enabled (REA_rodada166f §2, [SHOT] + [DECOMP]): every frequency column of the graph lit by the
// long-term level there, through a cold → hot colour ramp (FL: dark violet → red → orange, a 512-colour gradient over
// its analyser's −96 … 0 dB range), so a band's boost makes its region hotter and a cut colder (it shows the EQ's
// output). Full height by default, or fading from the top / the bottom. LYNX's own colour ramp and averaging; header
// only, no JUCE (tests/Eq166fTests.inc).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace lynx::spectrumheat
{
constexpr double floorDb = -96.0, topDb = 0.0; // the analyser's range (FL's scope: −96 dB over 96 dB)

// 0 … 1 for a level in dB (sine peak = 0 dB)
inline double heat01 (double db) noexcept
{
    if (! std::isfinite (db)) return 0.0;
    return std::clamp ((db - floorDb) / (topDb - floorDb), 0.0, 1.0);
}

// What the heatmap colours (LYNX's take on FL's look, where every column is lit and a tonal peak glows red against a
// violet field): the column's level tilted by `dbPerOctave` around `pivotHz` (music falls about 4.5 dB per octave, so the
// tilt evens it out, like FL's Histogram › Pivot slope), then placed 0 … 1 over `rangeDb` below the loudest column.
// A +12 dB band therefore moves its region 12 / rangeDb hotter whatever the song's overall level.
inline double tilt (double hz, double dbPerOctave = 4.5, double pivotHz = 1000.0) noexcept
{
    return hz > 0.0 ? dbPerOctave * std::log2 (hz / pivotHz) : 0.0;
}

inline double relative01 (double db, double loudestDb, double rangeDb = 48.0) noexcept
{
    if (! std::isfinite (db) || ! std::isfinite (loudestDb) || db < -190.0) return 0.0;
    return std::clamp (1.0 + (db - loudestDb) / rangeDb, 0.0, 1.0);
}

struct Rgb { std::uint8_t r, g, b; };

// LYNX's cold → hot ramp: near black, indigo, violet, wine, crimson, orange, pale gold
inline Rgb colour (double h) noexcept
{
    struct Stop { double at; double r, g, b; };
    static constexpr std::array<Stop, 7> stops { { { 0.00, 6, 5, 12 }, { 0.30, 22, 14, 48 }, { 0.50, 58, 20, 78 }, { 0.66, 118, 22, 74 },
                                                   { 0.80, 200, 38, 44 }, { 0.92, 244, 122, 40 }, { 1.00, 255, 218, 128 } } };
    h = std::clamp (h, 0.0, 1.0);
    for (std::size_t i = 1; i < stops.size(); ++i)
        if (h <= stops[i].at)
        {
            const auto& a = stops[i - 1];
            const auto& b = stops[i];
            const double t = (h - a.at) / (b.at - a.at);
            auto mix = [t] (double x, double y) { return (std::uint8_t) std::lround (x + (y - x) * t); };
            return { mix (a.r, b.r), mix (a.g, b.g), mix (a.b, b.b) };
        }
    return { 255, 218, 128 };
}

// The heat a column keeps: the power average over about `seconds` (frames at `fps`), in dB
struct Average
{
    double power = 0.0;
    void add (double db, double fps, double seconds) noexcept
    {
        const double p = std::pow (10.0, std::clamp (db, -200.0, 60.0) / 10.0);
        const double a = 1.0 - std::exp (-1.0 / std::max (1e-3, fps * seconds));
        power += a * (p - power);
        if (! (power > 1e-30)) power = 0.0;
    }
    double db() const noexcept { return power > 0.0 ? 10.0 * std::log10 (power) : -200.0; }
};

// Heatmap position (FL's Options › Heatmap › Heatmap position): the strength of the heat at height y01 (0 = top of the
// graph, 1 = bottom) — Full: everywhere; Top: from the top, fading out by the middle; Bottom: the same from the bottom
enum class Position { full, top, bottom };
inline double strength (Position p, double y01) noexcept
{
    y01 = std::clamp (y01, 0.0, 1.0);
    if (p == Position::full) return 1.0;
    const double d = p == Position::top ? y01 : 1.0 - y01;
    return std::clamp (1.0 - d / 0.6, 0.0, 1.0);
}
} // namespace lynx::spectrumheat
