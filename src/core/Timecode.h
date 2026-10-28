#pragma once

// SMPTE time code labels for the toolbar's time display (FL's time display modes, REA round 165f [DFM main form:
// "SMPTE 23.976 fps" … "SMPTE 72 fps"]). Header-only, no dependencies.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace lynx::timecode
{
struct Rate
{
    const char* name;
    double fps;       // real frames per second
    int nominal;      // frames counted per labelled second
    bool dropFrame;   // 29.97 / 59.94 drop: frame numbers 0-1 (0-3) skipped each minute except every tenth
};

inline constexpr Rate rates[] = {
    { "23.976", 24000.0 / 1001.0, 24, false }, { "24", 24.0, 24, false },  { "25", 25.0, 25, false },
    { "29.97 drop", 30000.0 / 1001.0, 30, true }, { "29.97 non-drop", 30000.0 / 1001.0, 30, false },
    { "30", 30.0, 30, false },  { "48", 48.0, 48, false },  { "50", 50.0, 50, false },
    { "59.94 drop", 60000.0 / 1001.0, 60, true }, { "59.94 non-drop", 60000.0 / 1001.0, 60, false },
    { "60", 60.0, 60, false },  { "72", 72.0, 72, false },
};
inline constexpr int rateCount = (int) (sizeof (rates) / sizeof (rates[0]));

// The label of the frame count `frames` (from zero): hours, minutes, seconds, frames.
struct Label
{
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
};

inline Label label (std::int64_t frames, const Rate& rate) noexcept
{
    if (frames < 0)
        frames = 0;
    const auto n = (std::int64_t) rate.nominal;
    if (rate.dropFrame)
    {
        const std::int64_t drop = n / 15;                        // 2 at 29.97, 4 at 59.94
        const std::int64_t perMinute = n * 60 - drop;            // 1798 / 3596 frames in a dropped minute
        const std::int64_t perTen = n * 600 - 9 * drop;          // 17982 / 35964 frames in ten minutes
        const auto tens = frames / perTen, rest = frames % perTen;
        frames += 9 * drop * tens + (rest > drop ? drop * ((rest - drop) / perMinute) : 0);
    }
    Label l;
    l.frames = (int) (frames % n);
    const auto secondsTotal = frames / n;
    l.seconds = (int) (secondsTotal % 60);
    l.minutes = (int) (secondsTotal / 60 % 60);
    l.hours = (int) (secondsTotal / 3600);
    return l;
}

inline std::int64_t framesAt (double seconds, const Rate& rate) noexcept
{
    return (std::int64_t) std::floor (std::max (0.0, seconds) * rate.fps + 1e-9);
}

// "01:02:03:04" (drop frame: "01:02:03;04").
inline std::string format (double seconds, int rateIndex)
{
    const auto& rate = rates[rateIndex < 0 || rateIndex >= rateCount ? 1 : rateIndex];
    const auto l = label (framesAt (seconds, rate), rate);
    char text[32];
    std::snprintf (text, sizeof text, "%02d:%02d:%02d%c%02d", l.hours, l.minutes, l.seconds, rate.dropFrame ? ';' : ':', l.frames);
    return text;
}
} // namespace lynx::timecode
