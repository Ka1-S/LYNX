#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Display ballistics of a peak meter (mixer strips): a fast-rising bar with a smooth falloff, a
// peak-hold line that stays put for a while before it falls, and a clip latch that stays lit until
// the user clicks the meter. Pure logic, ticked by the UI timer; independent of JUCE.
namespace lynx::meters
{
struct PeakHold
{
    float level = 0.0f;  // the bar (linear gain)
    float peak = 0.0f;   // the hold line (linear gain)
    int holdLeft = 0;    // ticks before the hold line starts to fall
    bool clipped = false;

    // One display tick with the loudest sample since the previous tick (linear gain).
    // decay: bar falloff per tick; holdTicks: how long the hold line stays; peakFall: its falloff per tick.
    void tick (float blockPeak, float decay = 0.86f, int holdTicks = 45, float peakFall = 0.92f) noexcept
    {
        blockPeak = std::max (0.0f, blockPeak);
        level = std::max (blockPeak, level * decay);
        if (level < 0.001f)
            level = 0.0f;

        if (blockPeak >= peak)
        {
            peak = blockPeak;
            holdLeft = holdTicks;
        }
        else if (holdLeft > 0)
            --holdLeft;
        else
        {
            peak = std::max (level, peak * peakFall);
            if (peak < 0.001f)
                peak = 0.0f;
        }

        if (blockPeak > 1.0f)
            clipped = true;
    }

    void resetClip() noexcept
    {
        clipped = false;
        peak = level;
        holdLeft = 0;
    }
};
// The mixer meters' dB scale: -60 dB at the bottom, +6 dB at the top, linear in dB. 0..1 from the bottom.
inline float scalePosition (float db) noexcept
{
    return std::clamp ((db + 60.0f) / 66.0f, 0.0f, 1.0f);
}

// The dB values printed beside a meter `heightPx` tall (round 162, issue #9: FL prints +3, 0, -3, -6, -12...):
// tried in order of importance, a value is kept when its label is at least `minGapPx` from every kept one, so a
// short meter shows only 0 / -12 / -24 and a tall one all of them. Returned from the top down.
inline std::vector<int> scaleMarks (float heightPx, float minGapPx)
{
    static const int candidates[] = { 0, -12, -24, -48, -6, -36, +3, -3, -18, -9 };
    std::vector<int> kept;

    for (auto db : candidates)
    {
        const auto y = scalePosition ((float) db) * heightPx;
        const bool fits = std::all_of (kept.begin(), kept.end(), [&] (int k) { return std::abs (scalePosition ((float) k) * heightPx - y) >= minGapPx; });
        if (fits)
            kept.push_back (db);
    }

    std::sort (kept.begin(), kept.end(), [] (int a, int b) { return a > b; });
    return kept;
}
} // namespace lynx::meters
