#include "core/Performance.h"
#include "core/NoteEdit.h"

#include <algorithm>
#include <cmath>

namespace lynx::perf
{
std::int64_t syncTicks (TrackPerformance::Sync sync, std::int64_t clipLength) noexcept
{
    using S = TrackPerformance::Sync;
    switch (sync)
    {
        case S::off: return 0;
        case S::quarterBeat: return ppq / 4;
        case S::halfBeat: return ppq / 2;
        case S::beat: return ppq;
        case S::twoBeats: return 2 * ppq;
        case S::fourBeats: return 4 * ppq;
        case S::autoLength: return std::max<std::int64_t> (1, clipLength);
    }
    return 0;
}

std::vector<int> zoneClips (const Song& song)
{
    std::vector<int> out;
    const auto end = song.performanceZoneEnd();
    if (end <= 0)
        return out;
    for (int i = 0; i < (int) song.playlist.size(); ++i)
        if (const auto& c = song.playlist[(std::size_t) i]; c.start < end && c.length > 0 && c.track >= 0 && c.track < maxTracks)
            out.push_back (i);
    std::sort (out.begin(), out.end(), [&] (int a, int b) {
        const auto& x = song.playlist[(std::size_t) a];
        const auto& y = song.playlist[(std::size_t) b];
        return x.track != y.track ? x.track < y.track : x.start < y.start;
    });
    return out;
}

namespace
{
const Clip& clipAt (const Song& song, int index) noexcept { return song.playlist[(std::size_t) index]; }

// the first zone entry of `track` (binary search on the sorted list)
std::size_t firstOfTrack (const Song& song, const std::vector<int>& zone, int track) noexcept
{
    return (std::size_t) (std::lower_bound (zone.begin(), zone.end(), track, [&] (int i, int t) { return clipAt (song, i).track < t; })
                          - zone.begin());
}

const TrackPerformance& settingsOf (const Song& song, int track) noexcept
{
    static const TrackPerformance defaults;
    return track >= 0 && track < (int) song.playlistTracks.size() ? song.playlistTracks[(std::size_t) track].performance : defaults;
}
} // namespace

int findZoneClip (const Song& song, const std::vector<int>& zone, int track, std::int64_t clipStart) noexcept
{
    for (auto k = firstOfTrack (song, zone, track); k < zone.size(); ++k)
    {
        const auto& c = clipAt (song, zone[k]);
        if (c.track != track || c.start > clipStart)
            break;
        if (c.start == clipStart)
            return zone[k];
    }
    return -1;
}

int trackZoneClips (const Song& song, const std::vector<int>& zone, int track, std::int64_t* out, int capacity) noexcept
{
    int n = 0;
    for (auto k = firstOfTrack (song, zone, track); k < zone.size() && n < capacity; ++k)
    {
        const auto& c = clipAt (song, zone[k]);
        if (c.track != track)
            break;
        if (n == 0 || out[n - 1] != c.start) // (two clips at one place on a track: the first one)
            out[n++] = c.start;
    }
    return n;
}

std::uint32_t Player::nextRandom() noexcept
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

std::int64_t Player::playingOn (int track) const noexcept
{
    for (int i = 0; i < numActive; ++i)
        if (active[(std::size_t) i].track == track)
            return active[(std::size_t) i].current.clipStart;
    return -1;
}

Player::TrackState* Player::stateOf (int track, bool create) noexcept
{
    for (int i = 0; i < numActive; ++i)
        if (active[(std::size_t) i].track == track)
            return &active[(std::size_t) i];
    if (! create || numActive >= maxActive)
        return nullptr;
    auto& t = active[(std::size_t) numActive++];
    t = TrackState {};
    t.track = track;
    return &t;
}

void Player::stopAll() noexcept
{
    numActive = 0;
    numVirtual = 0;
}

void Player::launch (const Song& song, const std::vector<int>& zone, TrackState& t, std::int64_t clipStart, double now,
                     bool immediate, double syncOverride) noexcept
{
    const auto index = findZoneClip (song, zone, t.track, clipStart);
    if (index < 0)
        return;
    const auto& clip = clipAt (song, index);
    const auto& s = settingsOf (song, t.track);
    const auto length = (double) std::max<std::int64_t> (1, clip.length);

    // when it starts: the next trigger sync grid point; with Queue, the end of the playing pass; Tolerant: a launch
    // within the first 1/8 of a grid step after a grid point is taken as on it (it starts now, already that far in)
    double from = std::ceil (now - 1.0e-6), grid = from;
    const auto step = syncOverride > 0.0 ? syncOverride : (double) syncTicks (s.triggerSync, clip.length);
    const auto& cur = t.current;
    if (! immediate && s.queue && cur.clipStart >= 0 && cur.until == never)
    {
        const auto curIndex = findZoneClip (song, zone, t.track, cur.clipStart);
        const auto curLength = curIndex >= 0 ? (double) std::max<std::int64_t> (1, clipAt (song, curIndex).length) : length;
        from = grid = cur.anchor + (std::floor ((now - cur.anchor) / curLength) + 1.0) * curLength;
    }
    else if (! immediate && step > 0.0)
    {
        const auto below = std::floor (now / step) * step;
        if (s.tolerant && now - below <= step / 8.0)
            grid = below; // late but tolerated: plays as if launched on the grid point
        else
            grid = std::ceil (now / step - 1.0e-9) * step;
        from = std::max (grid, std::ceil (now - 1.0e-6));
    }

    // where in the clip: its start at the grid point, or with position sync the song position modulo the sync length
    double anchor = grid;
    if (const auto p = (double) syncTicks (s.positionSync, clip.length); p > 0.0)
        anchor = grid - std::fmod (std::fmod (grid, p), length);

    t.next = Play { clipStart, anchor, from, never, false, 0.0 };
}

void Player::stopTrack (const Song& song, const std::vector<int>& zone, TrackState& t, double now, bool quantized) noexcept
{
    t.next = Play {};
    if (t.current.clipStart < 0)
        return;
    auto at = std::ceil (now - 1.0e-6);
    if (quantized)
    {
        const auto index = findZoneClip (song, zone, t.track, t.current.clipStart);
        const auto length = index >= 0 ? clipAt (song, index).length : 0;
        if (const auto step = (double) syncTicks (settingsOf (song, t.track).triggerSync, length); step > 0.0)
            at = std::max (at, std::ceil (now / step - 1.0e-9) * step);
    }
    t.current.until = std::min (t.current.until, std::max (at, t.current.from));
    t.motionAt = never;
}

void Player::scheduleMotion (const Song& song, const std::vector<int>& zone, TrackState& t) noexcept
{
    t.motionAt = never;
    const auto& p = t.current;
    if (p.clipStart < 0)
        return;
    const auto& s = settingsOf (song, t.track);
    using M = TrackPerformance::Motion;
    if (s.motion == M::stay)
        return;
    // Hold & motion: the clip loops while the trigger is held; the motion acts when it is released
    if (s.press == TrackPerformance::Press::holdMotion && t.held)
        return;
    const auto index = findZoneClip (song, zone, t.track, p.clipStart);
    if (index < 0)
        return;
    const auto length = (double) std::max<std::int64_t> (1, clipAt (song, index).length);
    // the end of the pass the play is in
    const auto base = std::max (p.from, lastClock);
    t.motionAt = p.anchor + (std::floor ((base - p.anchor) / length + 1.0e-9) + 1.0) * length;
}

void Player::recordPartial (const Song& song, const std::vector<int>& zone, const TrackState& t, const Play& p, double at,
                            Live* live) noexcept
{
    if (! p.recorded || p.clipStart < 0 || at <= p.from || p.until <= at - 1.0e-6)
        return; // (a play that ended already was recorded by its passes)
    const auto index = findZoneClip (song, zone, t.track, p.clipStart);
    if (index < 0)
        return;
    const auto length = (double) std::max<std::int64_t> (1, clipAt (song, index).length);
    const auto ws = p.anchor + std::floor ((at - 1.0e-6 - p.anchor) / length) * length;
    const auto vs = std::max (ws, p.from);
    if (at > vs + 0.5)
        live->recorded.push ({ t.track, p.clipStart, p.songFrom, at - vs, vs - ws });
}

void Player::apply (const Song& song, const std::vector<int>& zone, const Command& command, double now, Live* live) noexcept
{
    using P = TrackPerformance::Press;
    (void) live;
    auto c = command;
    if (c.column >= 0 && (c.type == Command::Type::press || c.type == Command::Type::release))
    {
        // a controller's column: the track's column-th zone clip
        std::array<std::int64_t, 256> clips {};
        const auto n = trackZoneClips (song, zone, c.track, clips.data(), (int) clips.size());
        if (c.column >= n)
            return;
        c.clipStart = clips[(std::size_t) c.column];
    }
    const auto barTicks = c.barSync ? (double) ppq * 4.0 * song.beatsPerBar / 4.0 : 0.0;
    if (c.type == Command::Type::press && c.scope != Command::Scope::track)
    {
        // a controller's column launch: every track's zone clip that starts within the pressed clip's span
        const auto index = findZoneClip (song, zone, c.track, c.clipStart);
        if (index < 0)
            return;
        const auto& pressed = clipAt (song, index);
        int lastTrack = -1;
        for (const auto i : zone)
        {
            const auto& clip = clipAt (song, i);
            if (clip.track == lastTrack || clip.start < pressed.start || clip.start >= pressed.start + std::max<std::int64_t> (1, pressed.length))
                continue;
            if (c.scope == Command::Scope::columnPlaying)
            {
                const auto* playing = stateOf (clip.track, false);
                if (playing == nullptr || playing->current.clipStart < 0)
                    continue;
            }
            lastTrack = clip.track;
            if (auto* t = stateOf (clip.track, true))
                launch (song, zone, *t, clip.start, now, false, barTicks);
        }
        return;
    }
    switch (c.type)
    {
        case Command::Type::stopAll:
            for (int i = 0; i < numActive; ++i)
                stopTrack (song, zone, active[(std::size_t) i], now, false);
            return;
        case Command::Type::stopTrack:
            if (auto* t = stateOf (c.track, false))
                stopTrack (song, zone, *t, now, true);
            return;
        case Command::Type::launchColumn:
        {
            // every track's first zone clip that starts in the column launches (FL: clicking a marker of the zone)
            int lastTrack = -1;
            for (const auto i : zone)
            {
                const auto& clip = clipAt (song, i);
                if (clip.track == lastTrack || clip.start < c.columnStart || clip.start >= c.columnEnd)
                    continue;
                lastTrack = clip.track;
                if (auto* t = stateOf (clip.track, true))
                    launch (song, zone, *t, clip.start, now, false);
            }
            return;
        }
        case Command::Type::press:
        {
            auto* t = stateOf (c.track, true);
            if (t == nullptr)
                return;
            const auto& s = settingsOf (song, c.track);
            if (s.press == P::latch && (t->current.clipStart == c.clipStart && t->current.until == never) && t->next.clipStart < 0)
            {
                stopTrack (song, zone, *t, now, true);
                return;
            }
            t->held = s.press == P::holdStop || s.press == P::holdMotion;
            launch (song, zone, *t, c.clipStart, now, false, barTicks);
            return;
        }
        case Command::Type::release:
        {
            auto* t = stateOf (c.track, false);
            if (t == nullptr || ! t->held)
                return;
            t->held = false;
            const auto& s = settingsOf (song, c.track);
            if (s.press == P::holdStop)
            {
                // the trigger let go: the clip stops right away (a waiting launch is dropped)
                stopTrack (song, zone, *t, now, false);
            }
            else if (s.press == P::holdMotion)
            {
                if (t->next.clipStart < 0 && t->current.clipStart >= 0)
                    scheduleMotion (song, zone, *t);
            }
            return;
        }
    }
}

void Player::process (const Song& song, const std::vector<int>& zone, Live* live, double clock, double songTick,
                      double blockTicks, bool playing) noexcept
{
    numVirtual = 0;
    if (! playing)
    {
        // the passes cut short by the stop are recorded up to there
        if (live != nullptr)
            for (int i = 0; i < numActive; ++i)
                recordPartial (song, zone, active[(std::size_t) i], active[(std::size_t) i].current, lastClock, live);
        stopAll();
        if (live != nullptr)
        {
            Command c;
            while (live->commands.pop (c)) {} // launches while stopped are dropped (FL launches only while playing)
            while (live->midiCommands.pop (c)) {}
            if (live->anyPlaying.load (std::memory_order_relaxed))
            {
                for (auto& v : live->tracks)
                {
                    v.playing.store (-1, std::memory_order_relaxed);
                    v.pending.store (-1, std::memory_order_relaxed);
                }
                live->anyPlaying.store (false, std::memory_order_relaxed);
            }
        }
        lastClock = clock;
        return;
    }

    const auto recording = live != nullptr && live->recording.load (std::memory_order_relaxed);
    const auto end = clock + blockTicks;
    const auto songAt = [&] (double c) { return songTick + (c - clock); };

    if (live != nullptr)
    {
        Command c;
        while (live->commands.pop (c))
            apply (song, zone, c, clock, live);
        while (live->midiCommands.pop (c))
            apply (song, zone, c, clock, live);
    }

    using M = TrackPerformance::Motion;
    for (int i = 0; i < numActive; ++i)
    {
        auto& t = active[(std::size_t) i];
        for (int guard = 0; guard < 8; ++guard)
        {
            const auto pending = t.next.clipStart >= 0 ? t.next.from : never;
            const auto ev = std::min ({ pending, t.current.until, t.motionAt });
            if (ev >= end)
                break;
            if (ev == pending)
            {
                if (t.current.clipStart >= 0)
                    t.current.until = std::min (t.current.until, ev);
                t.previous = t.current;
                t.current = t.next;
                t.next = Play {};
                t.current.recorded = recording;
                scheduleMotion (song, zone, t);
            }
            else if (ev == t.current.until)
            {
                t.previous = t.current;
                t.current = Play {};
                t.motionAt = never;
            }
            else // the end of the pass: the track's motion
            {
                const auto at = t.motionAt;
                t.motionAt = never;
                const auto& s = settingsOf (song, t.track);
                std::array<std::int64_t, 256> clips {};
                const auto n = trackZoneClips (song, zone, t.track, clips.data(), (int) clips.size());
                int here = 0;
                while (here < n && clips[(std::size_t) here] != t.current.clipStart)
                    ++here;
                int next = -1; // −1 = stop
                switch (s.motion)
                {
                    case M::stay: next = here; break;
                    case M::oneShot: next = -1; break;
                    case M::marchWrap: next = n > 0 ? (here + 1) % n : -1; break;
                    case M::marchStay: next = here + 1 < n ? here + 1 : here; break;
                    case M::marchStop: next = here + 1 < n ? here + 1 : -1; break;
                    case M::random: next = n > 0 ? (int) (nextRandom() % (std::uint32_t) n) : -1; break;
                    case M::exclusiveRandom:
                        if (n <= 1)
                            next = n == 1 ? 0 : -1;
                        else
                        {
                            next = (int) (nextRandom() % (std::uint32_t) (n - 1));
                            if (next >= here)
                                ++next;
                        }
                        break;
                }
                if (next < 0 || next >= n)
                    t.current.until = std::min (t.current.until, at);
                else if (clips[(std::size_t) next] == t.current.clipStart && s.motion != M::random && s.motion != M::exclusiveRandom)
                {
                    // stays on the same clip (March & stay at the last one): it just loops on
                }
                else
                {
                    t.next = Play { clips[(std::size_t) next], at, at, never, false, 0.0 };
                }
            }
        }
    }

    // this block's passes: each play's loop iterations that touch the block (the previous play's last one too, for its
    // note offs on the block's first sample)
    auto addPlay = [&] (const TrackState& t, Play& p) {
        if (p.clipStart < 0 || p.until < clock - 1.0 || p.from >= end)
            return;
        const auto index = findZoneClip (song, zone, t.track, p.clipStart);
        if (index < 0)
            return;
        const auto& clip = clipAt (song, index);
        if (clip.muted || song.isTrackMuted (clip.track))
            return;
        const auto length = (double) std::max<std::int64_t> (1, clip.length);
        auto k = std::floor ((std::max (p.from, clock - 1.0) - p.anchor) / length);
        for (int guard = 0; guard < 4 && numVirtual < maxVirtual; ++guard, k += 1.0)
        {
            const auto ws = p.anchor + k * length, we = ws + length;
            if (ws >= end)
                break;
            const auto vs = std::max (ws, p.from), ve = std::min (we, p.until);
            if (ve <= vs || ve < clock - 1.0)
                continue;
            // recording: a pass becomes a clip of the arrangement once it ends (the song position where it began)
            if (p.recorded && vs >= clock - 1.0e-6 && vs < end)
                p.songFrom = songAt (vs);
            if (p.recorded && recording && ve >= clock && ve < end && ve > vs)
                live->recorded.push ({ t.track, p.clipStart, p.songFrom, ve - vs, vs - ws });
            auto& v = virtuals[(std::size_t) numVirtual++];
            v.clip = clip;
            v.playlistIndex = (std::size_t) index;
            v.clip.start = (std::int64_t) std::llround (vs);
            v.clip.length = std::max<std::int64_t> (1, (std::int64_t) std::llround (ve) - v.clip.start);
            v.clip.offset = clip.offset + (std::int64_t) std::llround (vs - ws);
            if (clip.kind == ClipKind::audio)
            {
                // the fades belong to the clip's own start / end only
                if (vs > ws + 0.5)
                    v.clip.fadeIn = v.clip.crossfadeIn = 0;
                if (ve < we - 0.5)
                    v.clip.fadeOut = v.clip.crossfadeOut = 0;
            }
        }
    };

    bool any = false;
    for (int i = 0; i < numActive;)
    {
        auto& t = active[(std::size_t) i];
        addPlay (t, t.previous);
        addPlay (t, t.current);
        if (live != nullptr && t.track >= 0 && t.track < maxTracks)
        {
            auto& view = live->tracks[(std::size_t) t.track];
            const auto playingNow = t.current.clipStart >= 0 && t.current.from <= end ? t.current.clipStart : -1;
            view.playing.store (playingNow, std::memory_order_relaxed);
            view.pending.store (t.next.clipStart >= 0 ? t.next.clipStart : (t.current.from > end ? t.current.clipStart : -1),
                                std::memory_order_relaxed);
            if (playingNow >= 0)
                if (const auto index = findZoneClip (song, zone, t.track, playingNow); index >= 0)
                {
                    const auto length = (double) std::max<std::int64_t> (1, clipAt (song, index).length);
                    view.progress.store ((float) (std::fmod (std::max (0.0, clock - t.current.anchor), length) / length),
                                         std::memory_order_relaxed);
                }
        }
        const auto idle = t.current.clipStart < 0 && t.next.clipStart < 0 && (t.previous.clipStart < 0 || t.previous.until < clock - 1.0);
        if (idle && ! t.held)
        {
            if (live != nullptr && t.track >= 0 && t.track < maxTracks)
            {
                live->tracks[(std::size_t) t.track].playing.store (-1, std::memory_order_relaxed);
                live->tracks[(std::size_t) t.track].pending.store (-1, std::memory_order_relaxed);
            }
            active[(std::size_t) i] = active[(std::size_t) --numActive];
            continue;
        }
        any = any || t.current.clipStart >= 0 || t.next.clipStart >= 0;
        ++i;
    }
    if (live != nullptr)
        live->anyPlaying.store (any || numActive > 0, std::memory_order_relaxed);
    lastClock = end;
    lastSong = songAt (end);
}

void schedulePerformance (const Song& song, const Player& player, double clock, int numSamples, double tps, EventBuffer& out,
                          const ScheduleIndex* index) noexcept
{
    if (player.virtualCount() == 0)
        return;
    for (int i = 0; i < player.virtualCount(); ++i)
    {
        const auto& v = player.virtualClip (i);
        scheduleSingleClip (song, v.clip, v.playlistIndex, clock, numSamples, tps, out, index);
    }
    out.sortByOffset();
}

bool MidiMapping::note (int key, bool on, Command& out) noexcept
{
    if (key < 0 || key > 127)
        return false;
    if (key >= 125)
    {
        (key == 125 ? columnHeld : key == 126 ? playingHeld : barHeld) = on;
        return false;
    }
    if (key >= 120)
    {
        if (on)
            switch (key)
            {
                case 120: trackOffset = std::max (0, trackOffset - (bigSteps ? 10 : 1)); break;
                case 121: trackOffset = std::min (maxTracks, trackOffset + (bigSteps ? 10 : 1)); break;
                case 122: columnOffset = std::max (0, columnOffset - (bigSteps ? 12 : 1)); break;
                case 123: columnOffset = std::min (255, columnOffset + (bigSteps ? 12 : 1)); break;
                case 124: bigSteps = ! bigSteps; break;
                default: break;
            }
        return false;
    }
    out = Command {};
    out.type = on ? Command::Type::press : Command::Type::release;
    out.track = key / 12 + trackOffset;
    out.column = key % 12 + columnOffset;
    out.scope = playingHeld ? Command::Scope::columnPlaying : columnHeld ? Command::Scope::column : Command::Scope::track;
    out.barSync = barHeld;
    return out.track < maxTracks;
}

bool recordedClip (const Song& song, const RecordedPass& pass, Clip& out)
{
    const auto zone = zoneClips (song);
    const auto index = findZoneClip (song, zone, pass.track, pass.clipStart);
    if (index < 0 || pass.length < 1.0)
        return false;
    const auto& clip = song.playlist[(std::size_t) index];
    out = clip;
    out.start = std::max<std::int64_t> (0, (std::int64_t) std::llround (pass.songTick));
    out.length = std::max<std::int64_t> (1, (std::int64_t) std::llround (pass.length));
    out.offset = clip.offset + (std::int64_t) std::llround (pass.offset);
    return true;
}
} // namespace lynx::perf

namespace lynx::perf
{
void prepareForPerformance (Song& song, int count, std::int64_t spacing)
{
    count = std::clamp (count, 1, 256);
    spacing = std::max<std::int64_t> (1, spacing);
    const auto zone = (std::int64_t) count * spacing;
    noteedit::insertTime (song.playlist, song.markers(), 0, zone);
    for (int k = 0; k < count; ++k)
    {
        Marker m;
        m.tick = k * spacing;
        m.name = std::to_string (k + 1);
        song.addMarker (m);
    }
    Marker start;
    start.tick = zone;
    start.name = "Start";
    start.action = Marker::songStart;
    song.addMarker (start);
    song.performanceMode = true;
    song.mode = PlayMode::song;
}
} // namespace lynx::perf
