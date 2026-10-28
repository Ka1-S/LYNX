#include "core/Recorder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lynx
{
std::int64_t patternPositionForTick (const Song& song, double tick, std::int64_t loopLength)
{
    const auto position = (std::int64_t) std::floor (std::max (0.0, tick));

    if (song.mode == PlayMode::song)
    {
        const auto songLength = std::max<std::int64_t> (1, song.songLength());
        const auto inSong = position % songLength; // the song plays in a loop

        for (const auto& c : song.playlist)
            if (c.kind == ClipKind::pattern && c.pattern == song.currentPattern && inSong >= c.start && inSong < c.end())
                return inSong - c.start + c.offset;
    }

    return position % std::max<std::int64_t> (1, loopLength);
}

void commitTake (Pattern& pattern, std::vector<Note> notes, RecordMode mode, std::int64_t grid, std::vector<int>& clearedChannels)
{
    if (mode == RecordMode::replace)
        for (const auto& n : notes)
            if (std::find (clearedChannels.begin(), clearedChannels.end(), n.channel) == clearedChannels.end())
            {
                clearedChannels.push_back (n.channel);
                auto& existing = pattern.notes;
                existing.erase (std::remove_if (existing.begin(), existing.end(), [&n] (const Note& o) { return o.channel == n.channel; }),
                                existing.end());
            }

    if (grid > 1)
        for (auto& n : notes)
            n.start = std::max<std::int64_t> (0, (n.start + grid / 2) / grid * grid);

    pattern.notes.insert (pattern.notes.end(), notes.begin(), notes.end());
    pattern.sortNotes();
}

void quantizeTake (std::vector<Note>& notes, const TakeQuantize& q)
{
    const auto nearest = [] (std::int64_t t, std::int64_t g) { return std::max<std::int64_t> (0, (t + g / 2) / g * g); };

    for (auto& n : notes)
    {
        const auto end = n.start + std::max<std::int64_t> (1, n.length);

        if (q.stepSequencer)
        {
            n.start = nearest (n.start, ticksPerStep);
            n.length = ticksPerStep;
            continue;
        }

        if (q.grid <= 1)
            continue;

        if (q.starts)
            n.start = nearest (n.start, q.grid);

        if (q.end == NoteEnd::quantize)
            n.length = std::max (q.grid, nearest (end, q.grid) - n.start);
        else if (q.end == NoteEnd::keepEnd)
            n.length = std::max<std::int64_t> (1, end - n.start);
        // keepLength: the length as played
    }
}

void commitTake (Pattern& pattern, std::vector<Note> notes, RecordMode mode, const TakeQuantize& q, std::vector<int>& clearedChannels)
{
    quantizeTake (notes, q);
    commitTake (pattern, std::move (notes), mode, 0, clearedChannels);
}

std::vector<PunchRange> punchRanges (const std::vector<Marker>& markers)
{
    std::vector<const Marker*> sorted;
    for (const auto& m : markers)
        if (m.action == Marker::punchIn || m.action == Marker::punchOut)
            sorted.push_back (&m);
    std::stable_sort (sorted.begin(), sorted.end(), [] (const Marker* a, const Marker* b) { return a->tick < b->tick; });

    std::vector<PunchRange> out;
    std::int64_t openAt = -1;
    bool any = false;
    for (const auto* m : sorted)
    {
        if (m->action == Marker::punchIn)
        {
            if (openAt < 0)
                openAt = m->tick;
        }
        else if (openAt >= 0 || ! any) // a stop with no start before it: from the song start
        {
            const auto from = openAt >= 0 ? openAt : 0;
            if (m->tick > from)
                out.push_back ({ from, m->tick });
            openAt = -1;
        }
        any = true;
    }
    if (openAt >= 0)
        out.push_back ({ openAt, std::numeric_limits<std::int64_t>::max() / 4 });
    if (out.empty() && any) // only markers that open and close nothing: an empty range (nothing recorded)
        out.push_back ({ 0, 0 });
    return out;
}

bool inPunchRanges (const std::vector<PunchRange>& ranges, double tick) noexcept
{
    if (ranges.empty())
        return true;
    return std::any_of (ranges.begin(), ranges.end(), [tick] (const PunchRange& r) { return tick >= (double) r.in && tick < (double) r.out; });
}

std::vector<ClipPart> punchParts (const std::vector<PunchRange>& ranges, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    if (ranges.empty())
        return { { start, length, offset } };

    std::vector<ClipPart> out;
    const auto end = start + length;
    for (const auto& r : ranges)
    {
        const auto from = std::max (start, r.in), to = std::min (end, r.out);
        if (to > from)
            out.push_back ({ from, to - from, offset + (from - start) });
    }
    return out;
}

bool placePatternClipAt (Song& song, int pattern, std::int64_t tick)
{
    if (pattern < 0 || pattern >= (int) song.patterns.size())
        return false;

    tick = std::max<std::int64_t> (0, tick);

    for (const auto& c : song.playlist)
        if (c.kind == ClipKind::pattern && c.pattern == pattern && tick >= c.start && tick < c.end())
            return false;

    const auto bar = song.barAt (tick);
    Clip clip;
    clip.kind = ClipKind::pattern;
    clip.pattern = pattern;
    clip.start = bar.tick;
    clip.length = song.patterns[(std::size_t) pattern].effectiveLength (song.beatsPerBar);

    for (int track = 0;; ++track)
    {
        const auto busy = std::any_of (song.playlist.begin(), song.playlist.end(), [&] (const Clip& c) {
            return c.track == track && c.start < clip.end() && c.end() > clip.start;
        });

        if (! busy)
        {
            clip.track = track;
            break;
        }
    }

    song.playlist.push_back (clip);
    return true;
}

std::int64_t countInLength (const Song& song, int bars, std::int64_t tick)
{
    return std::max (0, bars) * std::max<std::int64_t> (1, song.barAt (std::max<std::int64_t> (0, tick)).length);
}

// --- automation recording (REA round 143) ---------------------------------------------------------------

namespace
{
std::int64_t nearestOnGrid (std::int64_t t, std::int64_t grid) noexcept
{
    return grid > 1 ? std::max<std::int64_t> (0, (t + grid / 2) / grid * grid) : t;
}

struct LocalPoint
{
    std::int64_t tick = 0;
    float value = 0.0f;
};

// Splits points at the loop's wrap (a tick that goes back) into runs that move forward.
std::vector<std::vector<LocalPoint>> forwardRuns (const std::vector<LocalPoint>& points)
{
    std::vector<std::vector<LocalPoint>> runs;
    for (const auto& p : points)
    {
        if (runs.empty() || p.tick < runs.back().back().tick)
            runs.emplace_back();
        auto& run = runs.back();
        if (! run.empty() && run.back().tick == p.tick)
            run.back().value = p.value; // one value per tick: the last one
        else
            run.push_back (p);
    }
    return runs;
}

bool sameValue (float a, float b) noexcept { return std::abs (a - b) < 1.0e-6f; }
} // namespace

std::int64_t songTickForRecorded (const Song& song, double tick, bool linear)
{
    auto t = (std::int64_t) std::floor (std::max (0.0, tick));
    if (linear && ! song.hasLoop())
        return t;
    if (song.hasLoop())
    {
        if (t >= song.loopEnd)
            t = song.loopStart + (t - song.loopStart) % std::max<std::int64_t> (1, song.loopEnd - song.loopStart);
        return t;
    }
    if (const auto length = song.songLength(); length > 0)
        t %= length;
    return t;
}

void thinAutomation (std::vector<AutomationPoint>& points, std::int64_t from, std::int64_t to)
{
    for (std::size_t i = 1; i + 1 < points.size();)
    {
        const auto& p = points[i];
        if (p.tick >= from && p.tick <= to && sameValue (points[i - 1].value, p.value) && sameValue (points[i + 1].value, p.value)
            && points[i - 1].tension == 0.0f && p.tension == 0.0f)
            points.erase (points.begin() + (std::ptrdiff_t) i);
        else
            ++i;
    }
}

void growTakeClip (Song& song, const AutomationWrite& w, std::int64_t tick, bool finished)
{
    if (! w.createdByTake || w.automation < 0)
        return;
    for (auto& c : song.playlist)
        if (c.kind == ClipKind::automation && c.automation == w.automation)
        {
            if (tick <= c.start)
                return;
            auto end = std::max (c.end(), tick);
            if (finished)
            {
                const auto bar = song.barAt (std::max<std::int64_t> (c.start, end - 1));
                end = std::max (end, bar.tick + std::max<std::int64_t> (1, bar.length));
            }
            c.length = end - c.start;
            return;
        }
}

void commitAutomationPoints (Song& song, AutomationWrite& w, std::vector<RecordedValue> points, std::int64_t grid, bool linear)
{
    std::vector<LocalPoint> ticks;
    for (const auto& p : points)
        ticks.push_back ({ nearestOnGrid (songTickForRecorded (song, p.tick, linear), grid), std::clamp (p.value, 0.0f, 1.0f) });

    for (const auto& run : forwardRuns (ticks))
    {
        const auto t0 = run.front().tick, t1 = run.back().tick;
        auto covers = [&] (const Clip& c, bool own) {
            // (the take's own clip covers everything after its start: it grows with the take, even over a pause of
            // the gesture — issue #12, a new clip began after every pause)
            return c.kind == ClipKind::automation && c.start <= t0 && (own || t0 < c.end());
        };

        // the clip to write into: the one this take writes, else a clip of the parameter playing there, else a new one
        int clipIndex = -1;
        for (int i = 0; i < (int) song.playlist.size(); ++i)
            if (song.playlist[(std::size_t) i].automation == w.automation && w.automation >= 0 && covers (song.playlist[(std::size_t) i], w.createdByTake))
                clipIndex = i;
        if (clipIndex < 0)
            for (int i = 0; i < (int) song.playlist.size(); ++i)
            {
                const auto& c = song.playlist[(std::size_t) i];
                if (covers (c, false) && c.automation >= 0 && c.automation < (int) song.automations.size()
                    && song.automations[(std::size_t) c.automation].target == w.target && ! song.automations[(std::size_t) c.automation].isLive())
                    clipIndex = i;
            }
        if (clipIndex >= 0 && song.playlist[(std::size_t) clipIndex].automation != w.automation)
        {
            w.automation = song.playlist[(std::size_t) clipIndex].automation;
            w.createdByTake = false;
            w.lastLocal = -1;
        }
        if (clipIndex < 0)
        {
            Automation a;
            a.name = w.name;
            a.target = w.target;
            song.automations.push_back (std::move (a));
            const auto bar = song.barAt (t0);
            Clip clip;
            clip.kind = ClipKind::automation;
            clip.automation = (int) song.automations.size() - 1;
            clip.start = bar.tick;
            clip.length = std::max<std::int64_t> (1, bar.length);
            // the first track with nothing from the clip's start on (the clip grows while recording)
            for (int track = 0;; ++track)
                if (std::none_of (song.playlist.begin(), song.playlist.end(), [&] (const Clip& c) { return c.track == track && c.end() > clip.start; }))
                {
                    clip.track = track;
                    break;
                }
            song.playlist.push_back (clip);
            clipIndex = (int) song.playlist.size() - 1;
            w.automation = clip.automation;
            w.createdByTake = true;
            w.lastLocal = -1;
        }

        auto& clip = song.playlist[(std::size_t) clipIndex];
        if (w.createdByTake && t1 >= clip.end()) // the take's own clip grows by bars
        {
            const auto bar = song.barAt (t1);
            clip.length = std::max (t1 + 1, bar.tick + std::max<std::int64_t> (1, bar.length)) - clip.start;
        }

        auto& a = song.automations[(std::size_t) clip.automation];
        std::vector<AutomationPoint> fresh;
        for (const auto& p : run)
            if (p.tick >= clip.start && p.tick < clip.end()) // an existing clip is not stretched
                fresh.push_back ({ p.tick - clip.start + clip.offset, p.value, 0.0f });
        if (fresh.empty())
            continue;

        const auto l0 = fresh.front().tick, l1 = fresh.back().tick;
        const auto continuing = w.lastLocal >= 0 && w.lastLocal < l0;
        const auto from = continuing ? w.lastLocal + 1 : l0;
        const auto hasBefore = std::any_of (a.points.begin(), a.points.end(), [&] (const AutomationPoint& p) { return p.tick < from; });
        const auto hasAfter = std::any_of (a.points.begin(), a.points.end(), [&] (const AutomationPoint& p) { return p.tick > l1 + 1; });
        const auto oldAtFrom = a.valueAt ((double) from - 1.0);
        const auto oldAfter = a.valueAt ((double) l1 + 1.0);

        // what lay under the gesture is replaced; the curve before and after it stays
        a.points.erase (std::remove_if (a.points.begin(), a.points.end(), [&] (const AutomationPoint& p) { return p.tick >= from && p.tick <= l1 + 1; }),
                        a.points.end());
        if (! continuing && hasBefore && from > 0)
            a.points.push_back ({ from - 1, oldAtFrom, 0.0f });
        a.points.insert (a.points.end(), fresh.begin(), fresh.end());
        if (hasAfter)
            a.points.push_back ({ l1 + 1, oldAfter, 0.0f });
        std::stable_sort (a.points.begin(), a.points.end(), [] (const AutomationPoint& x, const AutomationPoint& y) { return x.tick < y.tick; });
        thinAutomation (a.points, from, l1);
        w.lastLocal = l1;
    }
}

void commitEventPoints (Song& song, int patternIndex, AutomationWrite& w, std::vector<RecordedValue> points, std::int64_t loopLength,
                        std::int64_t grid)
{
    if (patternIndex < 0 || patternIndex >= (int) song.patterns.size() || points.empty())
        return;

    loopLength = std::max<std::int64_t> (1, loopLength);
    std::vector<LocalPoint> ticks;
    for (const auto& p : points)
        ticks.push_back ({ nearestOnGrid (patternPositionForTick (song, p.tick, loopLength), grid) % loopLength, std::clamp (p.value, 0.0f, 1.0f) });

    auto& pattern = song.patterns[(std::size_t) patternIndex];
    auto lane = std::find_if (pattern.events.begin(), pattern.events.end(), [&] (const PatternEvents& e) { return e.target == w.target; });
    if (lane == pattern.events.end())
    {
        pattern.events.push_back ({ w.target, {} });
        lane = pattern.events.end() - 1;
        w.lastLocal = -1;
    }
    auto& events = lane->events;

    for (const auto& run : forwardRuns (ticks))
    {
        const auto l0 = run.front().tick, l1 = run.back().tick;
        const auto continuing = w.lastLocal >= 0 && w.lastLocal < l0;
        const auto from = continuing ? w.lastLocal + 1 : l0;
        const auto heldBefore = std::any_of (events.begin(), events.end(), [&] (const PatternEvent& e) { return e.tick < from; });
        const auto nextOld = std::find_if (events.begin(), events.end(), [&] (const PatternEvent& e) { return e.tick > l1 + 1; });
        // the value that held after the gesture before it was recorded (it comes back after the gesture)
        const auto oldAfter = lane->valueAt ((double) l1 + 1.0);
        const auto restore = heldBefore && (nextOld == events.end() || nextOld->tick > l1 + 1);

        events.erase (std::remove_if (events.begin(), events.end(), [&] (const PatternEvent& e) { return e.tick >= from && e.tick <= l1 + 1; }),
                      events.end());
        for (const auto& p : run)
            events.push_back ({ p.tick, p.value });
        if (restore && l1 + 1 < loopLength)
            events.push_back ({ l1 + 1, oldAfter });
        std::stable_sort (events.begin(), events.end(), [] (const PatternEvent& x, const PatternEvent& y) { return x.tick < y.tick; });
        // steps: an event that repeats the value before it changes nothing
        for (std::size_t i = 1; i < events.size();)
            if (events[i].tick >= from && events[i].tick <= l1 && sameValue (events[i].value, events[i - 1].value))
                events.erase (events.begin() + (std::ptrdiff_t) i);
            else
                ++i;
        w.lastLocal = l1;
    }
}

void NoteRecorder::begin (std::int64_t loopLength)
{
    active = true;
    loop = std::max<std::int64_t> (1, loopLength);
    held.clear();
}

void NoteRecorder::add (const Song& song, const RecordedEvent& e)
{
    if (! active)
        return;

    const auto id = std::make_pair (e.channel, e.key);

    if (e.on)
    {
        // repeated note without an "off" in between: close the previous one
        if (auto it = held.find (id); it != held.end())
            finish (song, e.channel, e.key, it->second, e.tick);

        held[id] = { e.tick, e.velocity };
        return;
    }

    if (auto it = held.find (id); it != held.end())
    {
        finish (song, e.channel, e.key, it->second, e.tick);
        held.erase (it);
    }
}

void NoteRecorder::end (const Song& song, double tick)
{
    for (const auto& [id, h] : held)
        finish (song, id.first, id.second, h, tick);

    held.clear();
    active = false;
}

void NoteRecorder::finish (const Song& song, int channel, int key, const Held& h, double endTick)
{
    Note n;
    n.start = patternPositionForTick (song, h.tick, loop);
    // Song mode loops (the loop region, or the whole song): a note held across the loop's end ends after the wrap
    auto played = endTick - h.tick;
    if (played < 0.0 && song.mode == PlayMode::song)
        played += (double) (song.hasLoop() ? song.loopEnd - song.loopStart : std::max<std::int64_t> (1, song.songLength()));
    n.length = std::max<std::int64_t> (ticksPerStep / 8, (std::int64_t) std::llround (played));
    n.channel = channel;
    n.key = std::clamp (key, 0, 127);
    n.velocity = std::clamp (h.velocity, 0.0f, 1.0f);
    finished.push_back (n);
}

std::vector<Note> NoteRecorder::takeFinished()
{
    std::vector<Note> out;
    out.swap (finished);
    return out;
}
} // namespace lynx
