#pragma once

// Transport and editing helpers of round 162 (issue #9): partial quantize ("Quantize 50%"), the Piano Roll's
// playhead in Song mode, the editors' "Follow" (auto scrolling) and the meter scale of the Mixer. Pure logic,
// no JUCE (unit-tested in tests/Transport162Tests.inc). Behaviour interpreted from FL Studio (Playlist / Piano Roll
// "Auto scrolling", quick quantize), clean-room.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Song.h"

namespace lynx::transportedit
{
// --- quantize -------------------------------------------------------------------------------------------------

// `tick` moved `strength` (0..1) of the way to the nearest multiple of `grid`: 1 = onto the grid line, 0.5 = halfway
// ("Quantize 50%": applied again it keeps halving the distance). Never below 0.
inline std::int64_t quantizeTick (std::int64_t tick, std::int64_t grid, double strength) noexcept
{
    grid = std::max<std::int64_t> (1, grid);
    strength = std::clamp (strength, 0.0, 1.0);
    const auto target = (tick + grid / 2) / grid * grid;
    const auto moved = tick + (std::int64_t) std::llround ((double) (target - tick) * strength);
    return std::max<std::int64_t> (0, moved);
}

// Moves the starts of the notes `indices` toward the grid (lengths kept).
inline void quantizeNoteStarts (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid, double strength)
{
    for (auto i : indices)
        if (i >= 0 && i < (int) pattern.notes.size())
        {
            auto& n = pattern.notes[(std::size_t) i];
            n.start = quantizeTick (n.start, grid, strength);
        }
}

// Moves the starts of the clips `indices` toward the grid (lengths and contents kept): the Playlist's quick
// quantize (FL: Ctrl+Q in the Playlist quantizes the clips' start times).
inline void quantizeClipStarts (std::vector<Clip>& clips, const std::vector<int>& indices, std::int64_t grid, double strength)
{
    for (auto i : indices)
        if (i >= 0 && i < (int) clips.size())
        {
            auto& c = clips[(std::size_t) i];
            c.start = quantizeTick (c.start, grid, strength);
        }
}

// --- playhead -------------------------------------------------------------------------------------------------

// Where the Piano Roll's playhead is inside `pattern` (in ticks of the pattern), or -1 when that pattern is not
// playing: in Pattern mode the current pattern loops; in Song mode the playhead shows while the song position is
// inside a clip of that pattern (the clip's offset counts; the clip shown first in the Playlist wins).
inline double patternPlayhead (const Song& song, int pattern, double transportTick)
{
    if (pattern < 0 || pattern >= (int) song.patterns.size() || transportTick < 0.0)
        return -1.0;

    if (song.mode == PlayMode::pattern)
    {
        if (pattern != song.currentPattern)
            return -1.0;
        const auto length = (double) std::max<std::int64_t> (1, song.patterns[(std::size_t) pattern].effectiveLength (song.beatsPerBar));
        return std::fmod (transportTick, length);
    }

    const auto inSong = std::fmod (transportTick, (double) std::max<std::int64_t> (1, song.songLength())); // the song loops
    const Clip* best = nullptr;

    for (const auto& c : song.playlist)
        if (c.kind == ClipKind::pattern && c.pattern == pattern && inSong >= (double) c.start && inSong < (double) c.end())
            if (best == nullptr || c.track < best->track)
                best = &c;

    return best != nullptr ? inSong - (double) best->start + (double) best->offset : -1.0;
}

// Where the transport goes when the Piano Roll's ruler is clicked at `patternTick` (round 163, FL: clicking the
// Piano Roll's timeline moves the position): in Pattern mode that pattern position; in Song mode the same place in
// the clip of `pattern` that plays at `transportTick`, else in its earliest clip (upper track first). -1 in Song
// mode when the pattern has no clip in the Playlist.
inline double patternSeekTarget (const Song& song, int pattern, double patternTick, double transportTick)
{
    patternTick = std::max (0.0, patternTick);
    if (song.mode == PlayMode::pattern)
        return patternTick;

    const auto inSong = transportTick >= 0.0 ? std::fmod (transportTick, (double) std::max<std::int64_t> (1, song.songLength())) : -1.0;
    const Clip *playing = nullptr, *earliest = nullptr;
    for (const auto& c : song.playlist)
    {
        if (c.kind != ClipKind::pattern || c.pattern != pattern)
            continue;
        if (inSong >= (double) c.start && inSong < (double) c.end() && (playing == nullptr || c.track < playing->track))
            playing = &c;
        if (earliest == nullptr || c.start < earliest->start || (c.start == earliest->start && c.track < earliest->track))
            earliest = &c;
    }
    const auto* clip = playing != nullptr ? playing : earliest;
    if (clip == nullptr)
        return -1.0;
    const auto inClip = std::clamp (patternTick - (double) clip->offset, 0.0, (double) std::max<std::int64_t> (0, clip->length - 1));
    return (double) clip->start + inClip;
}

// --- follow (auto scrolling) ----------------------------------------------------------------------------------

// The editors' "Follow": when the playhead at `tick` leaves the view [viewStart, viewStart + viewTicks), the view
// jumps a page so the playhead is `leadTicks` from its left edge (FL's page-wise auto scrolling). Returns the new
// view start, or `viewStart` when the playhead is in view (or there is none).
inline double followViewStart (double viewStart, double viewTicks, double tick, double leadTicks, double rightMarginTicks = 0.0)
{
    if (tick < 0.0 || viewTicks <= 0.0)
        return viewStart;

    if (tick < viewStart || tick > viewStart + viewTicks - rightMarginTicks)
        return std::max (0.0, tick - leadTicks);

    return viewStart;
}

// --- Stop ------------------------------------------------------------------------------------------------------

// Where Stop sends the playhead. `alreadyStopped`: Stop pressed while nothing plays (or its second click of a
// double click) always goes back to the very start (the song or the pattern start), even with "Remember seek
// time" on; otherwise the remembered seek position (song mode with the option on), else the start.
inline std::int64_t stopTarget (bool alreadyStopped, bool rememberSeek, std::int64_t seekMark) noexcept
{
    if (alreadyStopped)
        return 0;
    return rememberSeek && seekMark > 0 ? seekMark : 0;
}
} // namespace lynx::transportedit
