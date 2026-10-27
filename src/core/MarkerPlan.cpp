#include "core/MarkerPlan.h"

#include <algorithm>
#include <cmath>

#include "core/Song.h"

namespace lynx
{
namespace
{
// the markers of the current arrangement in tick order (Song::markers() is kept sorted)
const std::vector<Marker>& sorted (const Song& song) { return song.markers(); }

std::int64_t nextMarkerTick (const std::vector<Marker>& list, std::size_t i, std::int64_t songEnd)
{
    for (auto k = i + 1; k < list.size(); ++k)
        if (list[k].tick > list[i].tick)
            return list[k].tick;
    return songEnd;
}
} // namespace

MarkerPlan makeMarkerPlan (const Song& song)
{
    MarkerPlan plan;
    const auto& list = sorted (song);
    const auto songEnd = song.songLength();
    plan.songEnd = songEnd;

    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto& m = list[i];
        MarkerJump jump;
        jump.at = m.tick;

        switch (m.action)
        {
            case Marker::markerLoop:
                // the section from this marker to the next one (or the song's end) repeats: at its end playback goes
                // back to the marker [SHOT fl_marker_loop_section.png]
                jump.at = nextMarkerTick (list, i, songEnd);
                jump.to = m.tick;
                break;
            case Marker::markerSkip:
                // the section from this marker to the next one is skipped [SHOT fl_skip_pause_play.png]
                jump.to = nextMarkerTick (list, i, songEnd);
                break;
            case Marker::markerPause:
                // playback stops on the marker; Play goes on from there [SHOT fl_pause_resume.png]
                jump.kind = MarkerJump::Kind::pause;
                break;
            case Marker::songLoop:
                plan.wrapTo = m.tick;
                continue;
            case Marker::start:
                if (plan.start < 0)
                    plan.start = m.tick;
                continue;
            default:
                continue;
        }

        if (jump.kind == MarkerJump::Kind::jump && jump.to == jump.at)
            continue; // nowhere to go
        if (jump.at <= 0 && jump.kind == MarkerJump::Kind::pause)
            continue;
        if (plan.count < MarkerPlan::maxJumps)
            plan.jumps[(std::size_t) plan.count++] = jump;
    }

    std::stable_sort (plan.jumps.begin(), plan.jumps.begin() + plan.count, [] (const MarkerJump& a, const MarkerJump& b) { return a.at < b.at; });
    return plan;
}

namespace markers
{
std::int64_t quantizedJumpTick (const Song& song, double position, int quantize)
{
    const auto at = (std::int64_t) std::floor (std::max (0.0, position));
    if (quantize <= 0)
        return at;
    if (quantize == 1) // beats
        return (at / ppq + 1) * ppq;

    // bars (1, 2, 4, 8), counted from bar 1 and following the time signature changes
    const auto every = quantize == 2 ? 1 : quantize == 3 ? 2 : quantize == 4 ? 4 : 8;
    auto bar = song.barAt (at);
    auto tick = bar.tick + bar.length;
    auto number = bar.number + 1;
    for (int guard = 0; guard < 64 && (number - 1) % every != 0; ++guard)
    {
        bar = song.barAt (tick);
        tick = bar.tick + bar.length;
        number = bar.number + 1;
    }
    return tick;
}
void setAction (const Song& song, Marker& marker, int action)
{
    if (action == Marker::timeSignature)
    {
        if (! marker.isSignature())
        {
            const auto bar = song.barAt (marker.tick);
            marker.numerator = std::clamp ((int) (bar.length / ppq), 1, 32); // in quarter notes
            marker.denominator = 4;
        }
        marker.action = 0;
        return;
    }
    marker.numerator = 0;
    marker.denominator = 4;
    marker.action = std::clamp (action, 0, (int) Marker::keyScale);
}

namespace
{
bool hasMarkerAt (const Song& song, std::int64_t tick)
{
    for (const auto& m : song.markers())
        if (m.tick == tick)
            return true;
    return false;
}
} // namespace

void addEvery (Song& song, int bars)
{
    bars = std::max (1, bars);
    const auto from = song.hasLoop() ? song.loopStart : 0;
    const auto to = song.hasLoop() ? song.loopEnd : song.songLength();
    for (const auto& b : song.barsBetween (from, to))
    {
        if (b.tick < from || (b.number - 1) % bars != 0 || hasMarkerAt (song, b.tick))
            continue;
        Marker m;
        m.tick = b.tick;
        m.name = std::to_string (b.number);
        song.addMarker (m);
    }
}

void addJumpToNextBar (Song& song, std::int64_t tick)
{
    tick = std::max<std::int64_t> (0, tick);
    const auto length = std::max<std::int64_t> (1, song.barAt (tick).length);
    Marker skip;
    skip.tick = tick;
    skip.action = Marker::markerSkip;
    song.addMarker (skip);
    if (! hasMarkerAt (song, tick + length))
    {
        Marker next;
        next.tick = tick + length;
        song.addMarker (next);
    }
}

void changeToMarkerLoop (Song& song)
{
    if (! song.hasLoop())
        return;
    const auto from = song.loopStart, to = song.loopEnd;
    bool started = false;
    for (auto& m : song.markers())
        if (m.tick == from && ! m.isSignature())
        {
            m.action = Marker::markerLoop;
            started = true;
            break;
        }
    if (! started)
    {
        Marker loop;
        loop.tick = from;
        loop.action = Marker::markerLoop;
        song.addMarker (loop);
    }
    if (! hasMarkerAt (song, to))
    {
        Marker end;
        end.tick = to;
        song.addMarker (end);
    }
    song.loopStart = song.loopEnd = 0;
}

const char* actionName (int action) noexcept
{
    switch (action)
    {
        case Marker::start: return "Start";
        case Marker::songLoop: return "Loop";
        case Marker::patternLength: return "Pattern length";
        case Marker::markerLoop: return "Marker loop";
        case Marker::markerSkip: return "Marker skip";
        case Marker::markerPause: return "Marker pause";
        case Marker::barOverride: return "Bar override";
        case Marker::timeSignature: return "Time signature";
        case Marker::punchIn: return "Start recording";
        case Marker::punchOut: return "Stop recording";
        case Marker::loopLength: return "Loop length";
        case Marker::keyScale: return "Key/Scale";
        default: return "";
    }
}

const char* actionTag (int action) noexcept
{
    switch (action)
    {
        case Marker::start: return "\xe2\x96\xb6";       // ▶
        case Marker::songLoop: return "\xe2\x86\xba";    // ↺
        case Marker::markerLoop: return "\xe2\x86\xb6";  // ↶
        case Marker::markerSkip: return "\xe2\x86\xb7";  // ↷
        case Marker::markerPause: return "\xe2\x80\x96"; // ‖
        case Marker::barOverride: return "\xe2\x87\xa5"; // ⇥
        case Marker::patternLength: return "\xe2\x86\x94"; // ↔
        case Marker::keyScale: return "\xe2\x99\xaf";    // ♯
        case Marker::punchIn: case Marker::punchOut: return "\xe2\x97\x8f"; // ●
        default: return "";
    }
}
} // namespace markers
} // namespace lynx
