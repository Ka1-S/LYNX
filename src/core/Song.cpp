#include "core/Song.h"
#include "core/FlSynths.h"

#include <algorithm>
#include <iterator>
#include <cmath>
#include <functional>
#include <limits>

namespace lynx
{
void Pattern::sortNotes()
{
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
}

std::int64_t Pattern::effectiveLength (int beatsPerBar) const noexcept
{
    if (lengthTicks > 0)
        return lengthTicks;

    const std::int64_t bar = (std::int64_t) ppq * std::max (1, beatsPerBar);
    std::int64_t end = 0;

    for (auto& n : notes)
        end = std::max (end, n.start + std::max<std::int64_t> (n.length, 1));

    return std::max<std::int64_t> (bar, (end + bar - 1) / bar * bar);
}

bool Pattern::hasStep (int channel, int step) const noexcept
{
    const auto tick = (std::int64_t) step * ticksPerStep;
    return std::any_of (notes.begin(), notes.end(),
                        [&] (const Note& n) { return n.channel == channel && n.start == tick; });
}

void Pattern::toggleStep (int channel, int step)
{
    const auto tick = (std::int64_t) step * ticksPerStep;
    const auto it = std::find_if (notes.begin(), notes.end(),
                                  [&] (const Note& n) { return n.channel == channel && n.start == tick; });

    if (it != notes.end())
    {
        notes.erase (it);
        return;
    }

    Note n;
    n.start = tick;
    n.channel = channel;
    notes.push_back (n);
    sortNotes();
}

std::int64_t Song::songLength() const noexcept
{
    const std::int64_t bar = (std::int64_t) ppq * std::max (1, beatsPerBar);
    std::int64_t end = 0;

    for (const auto& c : playlist)
        end = std::max (end, c.end());

    // an empty Playlist (e.g. recording the first take in Song mode) loops the current pattern's length, not one bar
    if (playlist.empty() && currentPattern >= 0 && currentPattern < (int) patterns.size())
        end = patterns[(std::size_t) currentPattern].effectiveLength (beatsPerBar);

    return std::max<std::int64_t> (bar, (end + bar - 1) / bar * bar);
}

void followPatternLengths (const std::vector<std::int64_t>& before, Song& song)
{
    if (before.size() != song.patterns.size())
        return; // patterns added, removed or moved: their clips are handled by that edit

    for (std::size_t i = 0; i < before.size(); ++i)
    {
        const auto now = song.patterns[i].effectiveLength (song.beatsPerBar);
        if (now == before[i] || before[i] <= 0)
            continue;

        song.forEachClipList ([&] (std::vector<Clip>& clips) {
            for (auto& c : clips)
                if (c.kind == ClipKind::pattern && c.pattern == (int) i && c.offset == 0 && c.length == before[i])
                    c.length = now;
        });
    }
}

std::vector<std::int64_t> patternLengths (const Song& song)
{
    std::vector<std::int64_t> lengths;
    lengths.reserve (song.patterns.size());
    for (const auto& p : song.patterns)
        lengths.push_back (p.effectiveLength (song.beatsPerBar));
    return lengths;
}

void Song::removePattern (int index)
{
    if (index < 0 || index >= (int) patterns.size())
        return;

    patterns.erase (patterns.begin() + index);

    forEachClipList ([index] (std::vector<Clip>& clips) {
        clips.erase (std::remove_if (clips.begin(), clips.end(),
                                     [index] (const Clip& c) { return c.kind == ClipKind::pattern && c.pattern == index; }),
                     clips.end());

        for (auto& c : clips)
            if (c.kind == ClipKind::pattern && c.pattern > index)
                --c.pattern;
    });

    if (patterns.empty())
        patterns.emplace_back().name = "Pattern 1";

    currentPattern = std::clamp (currentPattern > index ? currentPattern - 1 : currentPattern, 0, (int) patterns.size() - 1);
}

std::vector<MixerTrack> Song::defaultMixer (int inserts)
{
    std::vector<MixerTrack> mixer ((std::size_t) std::clamp (inserts + 1, 1, maxMixerTracks));
    mixer[0].name = "Master";

    for (std::size_t i = 1; i < mixer.size(); ++i)
        mixer[i].name = "Insert " + std::to_string (i);

    return mixer;
}

bool Song::insertInUse (int t) const
{
    // free = nothing uses it: no channel plays into it, no insert sends to it, no effects, input or Playlist track
    // link, the default name and routing. Round 157b: an FL project's effect buses (a vocal reverb/delay insert fed only
    // by sends) have no channel, and a sample added to the rack was routed into one (her beat through the vocal
    // H-Delay, reverb and Decapitator: "it sounds terrible").
    if (t <= 0 || t >= (int) mixer.size())
        return t == 0 && ! mixer.empty();
    auto used = [this] (int t) {
        const auto& m = mixer[(std::size_t) t];
        if (! m.effects.empty() || ! m.toMaster || m.inputChannel >= 0 || ! m.sends.empty()
            || (! m.name.empty() && m.name != "Insert " + std::to_string (t)))
            return true;
        if (std::any_of (channels.begin(), channels.end(), [t] (const ChannelDef& c) { return c.mixerTrack == t; }))
            return true;
        if (std::any_of (mixer.begin(), mixer.end(), [t] (const MixerTrack& o) {
                return std::any_of (o.sends.begin(), o.sends.end(), [t] (const MixerSend& send) { return send.target == t; });
            }))
            return true;
        return std::any_of (playlistTracks.begin(), playlistTracks.end(), [t] (const PlaylistTrack& p) { return p.mixerTrack == t; });
    };
    if (used (t))
        return true;
    // round 163 (hiding unused inserts): a fader, pan, mute or colour moved by hand, or automation on it, counts too
    const auto& m = mixer[(std::size_t) t];
    if (m.volume != unityFader || m.pan != 0.0f || m.muted || m.solo || m.colour != 0 || m.stereo != 0.0f || m.eq.active()
        || m.outputChannel != outputDefault)
        return true;
    if (std::any_of (mixer.begin(), mixer.end(), [t] (const MixerTrack& o) {
            return std::any_of (o.effects.begin(), o.effects.end(), [t] (const EffectSlot& e) { return e.sidechain == t || e.sidechain2 == t; });
        }))
        return true;
    return std::any_of (automations.begin(), automations.end(), [t] (const Automation& a) {
        const auto onTrack = a.target.kind == AutomationTargetKind::mixerVolume || a.target.kind == AutomationTargetKind::mixerPan
                             || a.target.kind == AutomationTargetKind::effectParam || a.target.kind == AutomationTargetKind::effectMix;
        return a.source.track == t || (onTrack && a.target.index == t);
    });
}

int Song::nextFreeInsert() const
{
    for (int t = 1; t < (int) mixer.size(); ++t)
        if (! insertInUse (t))
            return t;

    return 0; // every insert is in use: Master (FL's default for a new channel)
}

int Song::claimFreeInsert()
{
    if (const auto t = nextFreeInsert(); t > 0)
        return t;
    if (mixer.empty() || (int) mixer.size() >= maxMixerTracks)
        return 0;
    const auto name = "Insert " + std::to_string (mixer.size());
    mixer.emplace_back().name = name;
    return (int) mixer.size() - 1;
}

void Song::removeChannel (int index)
{
    if (index < 0 || index >= (int) channels.size())
        return;

    channels.erase (channels.begin() + index);

    for (auto& p : patterns)
    {
        p.notes.erase (std::remove_if (p.notes.begin(), p.notes.end(), [index] (const Note& n) { return n.channel == index; }),
                       p.notes.end());

        for (auto& n : p.notes)
            if (n.channel > index)
                --n.channel;
    }

    forEachClipList ([index] (std::vector<Clip>& clips) {
        clips.erase (std::remove_if (clips.begin(), clips.end(),
                                     [index] (const Clip& c) { return c.kind == ClipKind::audio && c.channel == index; }),
                     clips.end());

        for (auto& c : clips)
            if (c.kind == ClipKind::audio && c.channel > index)
                --c.channel;
    });

    forEachTarget ([index] (AutomationTarget& t) {
        const auto onChannel = t.kind == AutomationTargetKind::channelVolume || t.kind == AutomationTargetKind::channelPan
                               || t.kind == AutomationTargetKind::channelParam;

        if (onChannel && t.index == index)
            t.kind = AutomationTargetKind::none;
        else if (onChannel && t.index > index)
            --t.index;
    });

    for (auto& t : playlistTracks) // instrument tracks (REA round 146a)
        if (t.channel == index)
        {
            t.channel = -1;
            if (t.mode == PlaylistTrackMode::instrument)
                t.mode = PlaylistTrackMode::unassigned, t.mixerTrack = -1;
        }
        else if (t.channel > index)
            --t.channel;

    for (auto& automation : automations) // live source on a controller channel
        if (automation.source.channel == index)
            automation.source.channel = -1, automation.target.kind = AutomationTargetKind::none;
        else if (automation.source.channel > index)
            --automation.source.channel;

    for (auto& c : channels) // Layer children (round 164)
    {
        auto& kids = c.layer.children;
        kids.erase (std::remove (kids.begin(), kids.end(), index), kids.end());
        for (auto& k : kids)
            if (k > index)
                --k;
    }

    removeDeadMidiLinks();
}

void Song::removeDeadMidiLinks()
{
    midiLinks.erase (std::remove_if (midiLinks.begin(), midiLinks.end(),
                                     [] (const MidiLink& l) { return l.target.kind == AutomationTargetKind::none; }),
                     midiLinks.end());
    initialValues.erase (std::remove_if (initialValues.begin(), initialValues.end(),
                                         [] (const InitialValue& v) { return v.target.kind == AutomationTargetKind::none; }),
                         initialValues.end());
}

std::vector<MidiLink> linksWithGlobal (const std::vector<MidiLink>& project, const std::vector<MidiLink>& global)
{
    auto out = project;
    for (const auto& g : global)
    {
        const auto overridden = std::any_of (project.begin(), project.end(), [&] (const MidiLink& p) {
            return p.target == g.target || (p.cc == g.cc && (p.channel < 0 || g.channel < 0 || p.channel == g.channel));
        });
        if (! overridden && g.target.kind != AutomationTargetKind::none)
            out.push_back (g);
    }
    return out;
}

int Song::findInitialValue (const AutomationTarget& target) const noexcept
{
    for (int i = 0; i < (int) initialValues.size(); ++i)
        if (initialValues[(std::size_t) i].target == target)
            return i;
    return -1;
}

void Song::setInitialValue (const AutomationTarget& target, float value)
{
    if (target.kind == AutomationTargetKind::none)
        return;
    value = std::clamp (value, 0.0f, 1.0f);
    if (const auto i = findInitialValue (target); i >= 0)
        initialValues[(std::size_t) i].value = value;
    else
        initialValues.push_back ({ target, value });
}

void Song::removeInitialValue (const AutomationTarget& target)
{
    initialValues.erase (std::remove_if (initialValues.begin(), initialValues.end(),
                                         [&] (const InitialValue& v) { return v.target == target; }),
                         initialValues.end());
}

void Song::applyInitialValues (const std::function<void (const AutomationTarget&, float)>& plugin)
{
    for (const auto& v : initialValues)
        if (! setTargetValue (v.target, v.value) && plugin)
            plugin (v.target, v.value);
}

int fruitySendTarget (const EffectSlot& slot) noexcept
{
    if (slot.kind == EffectKind::patcher) // a Patcher's first extra output (REA_rodada141)
        return ! slot.patcher.outputTracks.empty() && slot.patcher.outputTracks.front() > 0 ? slot.patcher.outputTracks.front() : -1;
    if (slot.kind != EffectKind::fruitySend)
        return -1;
    const auto track = (int) std::lround (std::clamp ((double) slot.params[3], 0.0, 1.0) * (maxMixerTracks - 1));
    return track > 0 ? track : -1;
}

std::vector<std::pair<int, int>> fruitySendsOf (const Song& song, int track)
{
    std::vector<std::pair<int, int>> out;
    const auto n = (int) song.mixer.size();
    if (track <= 0 || track >= n)
        return out;
    const auto& slots = song.mixer[(std::size_t) track].effects;
    for (int s = 0; s < (int) slots.size() && s < maxEffectSlots; ++s)
        if (const auto target = fruitySendTarget (slots[(std::size_t) s]); target > 0 && target < n && target != track)
            out.emplace_back (s, target);
    return out;
}

bool Song::routes (int from, int to) const noexcept
{
    if (from <= 0 || from >= (int) mixer.size())
        return false;

    const auto& t = mixer[(std::size_t) from];

    if (to == 0)
        return t.toMaster;

    return std::any_of (t.sends.begin(), t.sends.end(), [to] (const MixerSend& s) { return s.target == to; });
}

float Song::routeLevel (int from, int to) const noexcept
{
    if (! routes (from, to))
        return 0.0f;

    const auto& t = mixer[(std::size_t) from];

    if (to == 0)
        return t.masterLevel;

    for (const auto& s : t.sends)
        if (s.target == to)
            return s.level;

    return 0.0f;
}

void Song::setRouteLevel (int from, int to, float level)
{
    if (! routes (from, to))
        return;

    const auto v = std::clamp (level, 0.0f, maxFaderGain);
    auto& t = mixer[(std::size_t) from];

    if (to == 0)
        t.masterLevel = v;
    else
        for (auto& s : t.sends)
            if (s.target == to)
                s.level = v;
}

bool Song::canRoute (int from, int to) const
{
    const auto n = (int) mixer.size();

    if (from <= 0 || from >= n || to < 0 || to >= n || from == to)
        return false;

    if (to == 0)
        return true;

    // cycle if `from` is already reachable from `to`
    std::vector<int> stack { to };
    std::vector<bool> seen ((std::size_t) n, false);

    while (! stack.empty())
    {
        const auto t = stack.back();
        stack.pop_back();

        if (t == from)
            return false;

        if (seen[(std::size_t) t])
            continue;

        seen[(std::size_t) t] = true;

        for (const auto& s : mixer[(std::size_t) t].sends)
            if (s.target > 0 && s.target < n)
                stack.push_back (s.target);
        for (const auto& fx : mixer[(std::size_t) t].effects)
            if (const auto target = fruitySendTarget (fx); target > 0 && target < n)
                stack.push_back (target);
    }

    return true;
}

void Song::setRoute (int from, int to, bool enabled, float level)
{
    if (from <= 0 || from >= (int) mixer.size() || to < 0 || to >= (int) mixer.size())
        return;

    auto& t = mixer[(std::size_t) from];

    if (to == 0)
    {
        t.toMaster = enabled;
        return;
    }

    auto it = std::find_if (t.sends.begin(), t.sends.end(), [to] (const MixerSend& s) { return s.target == to; });

    if (! enabled)
    {
        if (it != t.sends.end())
            t.sends.erase (it);
        return;
    }

    if (it != t.sends.end())
        it->level = std::clamp (level, 0.0f, maxFaderGain);
    else if (canRoute (from, to))
        t.sends.push_back ({ to, std::clamp (level, 0.0f, maxFaderGain) });
}

std::vector<int> Song::mixerOrder() const
{
    const auto n = (int) mixer.size();
    std::vector<int> incoming ((std::size_t) std::max (0, n), 0);

    // edges: sends and sidechains (the sidechain source must be processed before whoever listens to it)
    std::vector<std::vector<int>> next ((std::size_t) std::max (0, n));
    for (int t = 1; t < n; ++t)
    {
        for (const auto& s : mixer[(std::size_t) t].sends)
            if (s.target > 0 && s.target < n)
                next[(std::size_t) t].push_back (s.target);
        for (const auto& fx : mixer[(std::size_t) t].effects) // Fruity Send slots: the destination comes after
            if (const auto target = fruitySendTarget (fx); target > 0 && target < n && target != t)
                next[(std::size_t) t].push_back (target);
        for (const auto& fx : mixer[(std::size_t) t].effects)
            for (const auto source : { fx.sidechain, fx.sidechain2 })
                if (source > 0 && source < n && source != t)
                    next[(std::size_t) source].push_back (t);
    }
    for (int t = 1; t < n; ++t)
        for (const auto target : next[(std::size_t) t])
            ++incoming[(std::size_t) target];

    // Kahn: start with the tracks nothing feeds (in numeric order)
    std::vector<int> order, ready;

    for (int t = n - 1; t >= 1; --t)
        if (incoming[(std::size_t) t] == 0)
            ready.push_back (t);

    while (! ready.empty())
    {
        const auto t = ready.back();
        ready.pop_back();
        order.push_back (t);

        for (const auto target : next[(std::size_t) t])
            if (--incoming[(std::size_t) target] == 0)
            {
                // keep numeric order among those that become ready together
                auto pos = std::upper_bound (ready.begin(), ready.end(), target, std::greater<int>());
                ready.insert (pos, target);
            }
    }

    // cycle (corrupt file): the leftovers go at the end, without the sends that close the cycle
    for (int t = 1; t < n; ++t)
        if (std::find (order.begin(), order.end(), t) == order.end())
            order.push_back (t);

    return order;
}

std::vector<bool> Song::soloAudible() const
{
    const auto n = mixer.size();
    std::vector<bool> audible (n, true);
    std::vector<int> soloed;

    for (std::size_t t = 1; t < n; ++t)
        if (mixer[t].solo)
            soloed.push_back ((int) t);

    if (soloed.empty())
        return audible;

    std::fill (audible.begin(), audible.end(), false);

    if (n > 0)
        audible[0] = true;

    // downstream: the signal path from the soloed tracks to Master
    std::vector<int> stack = soloed;

    while (! stack.empty())
    {
        const auto t = stack.back();
        stack.pop_back();

        if (audible[(std::size_t) t])
            continue;

        audible[(std::size_t) t] = true;

        for (const auto& s : mixer[(std::size_t) t].sends)
            if (s.target > 0 && s.target < (int) n)
                stack.push_back (s.target);
        for (const auto& fx : mixer[(std::size_t) t].effects)
            if (const auto target = fruitySendTarget (fx); target > 0 && target < (int) n)
                stack.push_back (target);
    }

    // upstream: whoever feeds the soloed tracks
    std::vector<bool> feeds (n, false);
    stack = soloed;

    while (! stack.empty())
    {
        const auto t = stack.back();
        stack.pop_back();

        for (std::size_t src = 1; src < n; ++src)
            if (! feeds[src]
                && (routes ((int) src, t)
                    || std::any_of (mixer[src].effects.begin(), mixer[src].effects.end(),
                                    [t] (const EffectSlot& fx) { return fruitySendTarget (fx) == t; })))
            {
                feeds[src] = true;
                audible[src] = true;
                stack.push_back ((int) src);
            }
    }

    return audible;
}

void Song::swapEffectSlots (int track, int a, int b)
{
    if (track < 0 || track >= (int) mixer.size())
        return;

    auto& fx = mixer[(std::size_t) track].effects;

    if (a < 0 || b < 0 || a >= (int) fx.size() || b >= (int) fx.size())
        return;

    std::swap (fx[(std::size_t) a], fx[(std::size_t) b]);

    for (auto& automation : automations)
        if (automation.source.track == track)
            automation.source.slot = automation.source.slot == a ? b : automation.source.slot == b ? a : automation.source.slot;

    forEachTarget ([track, a, b] (AutomationTarget& t) {
        if ((t.kind == AutomationTargetKind::effectParam || t.kind == AutomationTargetKind::effectMix) && t.index == track)
            t.slot = t.slot == a ? b : t.slot == b ? a : t.slot;
    });
}

void Song::removeEffectSlot (int track, int slot)
{
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;

    auto& fx = mixer[(std::size_t) track].effects;
    fx.erase (fx.begin() + slot);

    for (auto& automation : automations)
        if (automation.source.track == track)
        {
            if (automation.source.slot == slot)
                automation.source.track = -1, automation.target.kind = AutomationTargetKind::none; // the source is gone
            else if (automation.source.slot > slot)
                --automation.source.slot;
        }

    forEachTarget ([track, slot] (AutomationTarget& t) {
        if ((t.kind == AutomationTargetKind::effectParam || t.kind == AutomationTargetKind::effectMix) && t.index == track)
        {
            if (t.slot == slot)
                t.kind = AutomationTargetKind::none;
            else if (t.slot > slot)
                --t.slot;
        }
    });

    removeDeadMidiLinks();
}

void Song::removeAutomation (int index)
{
    if (index < 0 || index >= (int) automations.size())
        return;

    automations.erase (automations.begin() + index);

    forEachClipList ([index] (std::vector<Clip>& clips) {
        clips.erase (std::remove_if (clips.begin(), clips.end(),
                                     [index] (const Clip& c) { return c.kind == ClipKind::automation && c.automation == index; }),
                     clips.end());

        for (auto& c : clips)
            if (c.kind == ClipKind::automation && c.automation > index)
                --c.automation;
    });
}

std::vector<Marker>& Song::markers()
{
    if (arrangements.empty())
        arrangements.push_back ({ "Arrangement 1", {}, 0, 0, {} });

    return arrangements[(std::size_t) std::clamp (currentArrangement, 0, (int) arrangements.size() - 1)].markers;
}

const std::vector<Marker>& Song::markers() const
{
    static const std::vector<Marker> none;
    return arrangements.empty() ? none : arrangements[(std::size_t) std::clamp (currentArrangement, 0, (int) arrangements.size() - 1)].markers;
}

void Song::addMarker (Marker marker)
{
    auto& list = markers();
    const auto at = std::upper_bound (list.begin(), list.end(), marker.tick, [] (std::int64_t t, const Marker& m) { return t < m.tick; });
    list.insert (at, std::move (marker));
}

std::vector<Bar> Song::barsBetween (std::int64_t from, std::int64_t to) const
{
    std::vector<Bar> bars;
    auto barLength = [] (int numerator, int denominator) {
        return std::max<std::int64_t> (1, (std::int64_t) ppq * 4 * std::max (1, numerator) / std::max (1, denominator));
    };

    Bar bar { 0, barLength (beatsPerBar, 4), 1 };
    const auto& list = markers();
    std::size_t next = 0;

    while (bar.tick < to)
    {
        // time signature changes falling at the start of this bar apply to it
        while (next < list.size() && (! list[next].isSignature() || list[next].tick <= bar.tick))
        {
            if (list[next].isSignature())
                bar.length = barLength (list[next].numerator, list[next].denominator);
            ++next;
        }

        // the next change cuts this bar short and starts a new one there
        const auto end = next < list.size() ? std::min (bar.tick + bar.length, list[next].tick) : bar.tick + bar.length;

        if (end > from)
            bars.push_back ({ bar.tick, end - bar.tick, bar.number });

        bar = { end, bar.length, bar.number + 1 };

        // no changes ahead: the rest is arithmetic (avoids walking bar by bar up to `from`)
        if (bar.tick < from && next >= list.size())
        {
            const auto skip = (from - bar.tick) / bar.length;
            bar.tick += skip * bar.length;
            bar.number += (int) skip;
        }
    }

    return bars;
}

Bar Song::barAt (std::int64_t tick) const
{
    const auto bars = barsBetween (std::max<std::int64_t> (0, tick), std::max<std::int64_t> (0, tick) + 1);
    return bars.empty() ? Bar { 0, (std::int64_t) ppq * std::max (1, beatsPerBar), 1 } : bars.front();
}

void Song::selectArrangement (int index)
{
    if (arrangements.empty())
        arrangements.push_back ({ "Arrangement 1", {}, 0, 0, {} });

    currentArrangement = std::clamp (currentArrangement, 0, (int) arrangements.size() - 1);
    index = std::clamp (index, 0, (int) arrangements.size() - 1);

    if (index == currentArrangement)
        return;

    auto& from = arrangements[(std::size_t) currentArrangement];
    from.clips = std::move (playlist);
    from.loopStart = loopStart;
    from.loopEnd = loopEnd;

    auto& to = arrangements[(std::size_t) index];
    playlist = std::move (to.clips);
    to.clips.clear();
    loopStart = to.loopStart;
    loopEnd = to.loopEnd;
    currentArrangement = index;
}

int Song::addArrangement (const std::string& name, bool copyCurrent)
{
    Arrangement a;
    a.name = name;

    if (copyCurrent)
    {
        a.clips = playlist;
        a.loopStart = loopStart;
        a.loopEnd = loopEnd;
        a.markers = markers();
    }

    arrangements.push_back (std::move (a));
    selectArrangement ((int) arrangements.size() - 1);
    return currentArrangement;
}

void Song::removeArrangement (int index)
{
    if (arrangements.size() <= 1 || index < 0 || index >= (int) arrangements.size())
        return;

    if (index == currentArrangement)
        selectArrangement (index == 0 ? 1 : index - 1);

    arrangements.erase (arrangements.begin() + index);

    if (currentArrangement > index)
        --currentArrangement;
}

float Song::targetValue (const AutomationTarget& t) const noexcept
{
    const auto validChannel = t.index >= 0 && t.index < (int) channels.size();
    const auto validTrack = t.index >= 0 && t.index < (int) mixer.size();

    switch (t.kind)
    {
        case AutomationTargetKind::channelVolume: return validChannel ? channels[(std::size_t) t.index].volume : -1.0f;
        case AutomationTargetKind::channelPan: return validChannel ? (channels[(std::size_t) t.index].pan + 1.0f) * 0.5f : -1.0f;
        case AutomationTargetKind::mixerVolume: return validTrack ? mixer[(std::size_t) t.index].volume : -1.0f;
        case AutomationTargetKind::mixerPan: return validTrack ? (mixer[(std::size_t) t.index].pan + 1.0f) * 0.5f : -1.0f;
        case AutomationTargetKind::effectParam:
            if (validTrack && t.slot >= 0 && t.slot < (int) mixer[(std::size_t) t.index].effects.size()
                && mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].kind != EffectKind::plugin && t.param >= 0
                && t.param < maxEffectParams)
                return mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].params[(std::size_t) t.param];
            return validTrack ? 0.5f : -1.0f; // plugin parameter: the value lives in the plugin
        case AutomationTargetKind::channelParam:
            if (! validChannel)
                return -1.0f;
            // an Envelope Controller's global Mod X / Y (round 164f): the value lives in its state
            if (channels[(std::size_t) t.index].instrument == InstrumentKind::controller
                && (t.param == envelopeControllerModXParam || t.param == envelopeControllerModYParam))
                if (const auto ec = parseEnvelopeController (channels[(std::size_t) t.index].controllerState))
                    return t.param == envelopeControllerModXParam ? ec->modX : ec->modY;
            return 0.5f;
        case AutomationTargetKind::effectMix:
            return validTrack && t.slot >= 0 && t.slot < (int) mixer[(std::size_t) t.index].effects.size()
                       ? mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].mix
                       : -1.0f;
        case AutomationTargetKind::tempo: return valueFromTempo (bpm);
        case AutomationTargetKind::none: break;
    }

    return -1.0f;
}

bool Song::setTargetValue (const AutomationTarget& t, float v) noexcept
{
    v = std::clamp (v, 0.0f, 1.0f);
    const auto validChannel = t.index >= 0 && t.index < (int) channels.size();
    const auto validTrack = t.index >= 0 && t.index < (int) mixer.size();

    switch (t.kind)
    {
        case AutomationTargetKind::channelVolume: if (! validChannel) return false; channels[(std::size_t) t.index].volume = v; return true;
        case AutomationTargetKind::channelPan: if (! validChannel) return false; channels[(std::size_t) t.index].pan = v * 2.0f - 1.0f; return true;
        case AutomationTargetKind::mixerVolume: if (! validTrack) return false; mixer[(std::size_t) t.index].volume = v; return true;
        case AutomationTargetKind::mixerPan: if (! validTrack) return false; mixer[(std::size_t) t.index].pan = v * 2.0f - 1.0f; return true;
        case AutomationTargetKind::tempo: bpm = tempoFromValue (v); return true;
        case AutomationTargetKind::effectMix:
            if (! validTrack || t.slot < 0 || t.slot >= (int) mixer[(std::size_t) t.index].effects.size())
                return false;
            mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].mix = v;
            return true;
        case AutomationTargetKind::effectParam:
            if (validTrack && t.slot >= 0 && t.slot < (int) mixer[(std::size_t) t.index].effects.size()
                && mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].kind != EffectKind::plugin && t.param >= 0
                && t.param < maxEffectParams)
            {
                mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].params[(std::size_t) t.param] = v;
                return true;
            }
            return false;
        case AutomationTargetKind::channelParam:
            if (validChannel && channels[(std::size_t) t.index].instrument == InstrumentKind::controller
                && (t.param == envelopeControllerModXParam || t.param == envelopeControllerModYParam))
                if (auto ec = parseEnvelopeController (channels[(std::size_t) t.index].controllerState))
                {
                    (t.param == envelopeControllerModXParam ? ec->modX : ec->modY) = v;
                    channels[(std::size_t) t.index].controllerState = writeEnvelopeController (*ec);
                    return true;
                }
            return false;
        case AutomationTargetKind::none: break;
    }

    return false;
}

float Song::automationValueAt (int index, double tick) const
{
    if (index < 0 || index >= (int) automations.size())
        return std::numeric_limits<float>::quiet_NaN();

    std::vector<const Clip*> clips;

    for (const auto& c : playlist)
        if (c.kind == ClipKind::automation && c.automation == index && c.length > 0 && ! c.muted && ! isTrackMuted (c.track))
            clips.push_back (&c);

    if (clips.empty())
        return std::numeric_limits<float>::quiet_NaN();

    std::stable_sort (clips.begin(), clips.end(), [] (const Clip* a, const Clip* b) { return a->start < b->start; });
    const auto& automation = automations[(std::size_t) index];
    const Clip* before = nullptr;

    // same rule as playback: inside a clip the curve; between clips the last value; before, the first
    for (const auto* c : clips)
    {
        if (tick < (double) c->start)
            break;

        if (tick < (double) c->end())
            return automation.valueAt (tick - (double) c->start + (double) c->offset);

        before = c;
    }

    if (before != nullptr)
        return automation.valueAt ((double) (before->length + before->offset));

    return automation.valueAt ((double) clips.front()->offset);
}

double Song::tempoAt (double tick) const
{
    if (mode != PlayMode::song)
        return bpm;

    for (int i = (int) automations.size() - 1; i >= 0; --i) // the last tempo automation wins
        if (automations[(std::size_t) i].target.kind == AutomationTargetKind::tempo)
            if (const auto v = automationValueAt (i, tick); ! std::isnan (v))
                return tempoFromValue (v);

    return bpm;
}

double Song::secondsForTicks (std::int64_t ticks) const
{
    const auto hasTempo = mode == PlayMode::song
                          && std::any_of (automations.begin(), automations.end(),
                                          [] (const Automation& a) { return a.target.kind == AutomationTargetKind::tempo; });

    if (! hasTempo)
        return (double) ticks * 60.0 / (std::max (1.0, bpm) * ppq);

    // integrate in 1/16 steps (playback changes tempo block by block; the difference is minimal)
    double seconds = 0.0;
    constexpr std::int64_t step = ticksPerStep;

    for (std::int64_t t = 0; t < ticks; t += step)
    {
        const auto n = std::min (step, ticks - t);
        seconds += (double) n * 60.0 / (std::max (1.0, tempoAt ((double) t)) * ppq);
    }

    return seconds;
}

float Automation::mapLive (float v) const noexcept
{
    v = std::clamp (v, 0.0f, 1.0f);
    if (curve.size() < 2)
        return v;
    const auto x = v * (float) (curve.size() - 1);
    const auto i = std::min ((std::size_t) x, curve.size() - 2);
    const auto f = x - (float) i;
    return std::clamp (curve[i] + (curve[i + 1] - curve[i]) * f, 0.0f, 1.0f);
}

float PatternEvents::valueAt (double tick) const noexcept
{
    if (events.empty())
        return 0.5f;

    // the last event at or before `tick`
    auto it = std::upper_bound (events.begin(), events.end(), tick, [] (double t, const PatternEvent& e) { return t < (double) e.tick; });
    return it == events.begin() ? events.front().value : std::prev (it)->value;
}

float Automation::valueAt (double tick) const noexcept
{
    if (points.empty())
        return 0.5f;

    if (tick <= (double) points.front().tick)
        return points.front().value;

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        const auto& a = points[i - 1];
        const auto& b = points[i];

        if (tick < (double) b.tick)
        {
            const auto span = (double) std::max<std::int64_t> (1, b.tick - a.tick);
            auto t = (float) ((tick - (double) a.tick) / span);

            // tension: curve exponent (positive = rises slowly then accelerates)
            if (a.tension != 0.0f)
                t = std::pow (t, std::pow (2.0f, a.tension * 3.0f));

            return a.value + (b.value - a.value) * t;
        }
    }

    return points.back().value;
}

std::int64_t Automation::length() const noexcept
{
    return points.empty() ? 1 : std::max<std::int64_t> (1, points.back().tick);
}

Clip splitClip (Clip& clip, std::int64_t tick)
{
    Clip rest = clip;
    const auto head = std::clamp<std::int64_t> (tick - clip.start, 1, std::max<std::int64_t> (1, clip.length - 1));
    clip.length = head;
    rest.start = clip.start + head;
    rest.offset = clip.offset + head;
    rest.length -= head;
    return rest;
}

double TrackEq::peakQ (float w) noexcept
{
    return 0.0999 * (std::pow (1001.0, std::clamp ((double) w, 0.0, 1.0)) - 1.0) + 0.1;
}

double TrackEq::shelfSlope (float w) noexcept
{
    return 1.9 * std::clamp ((double) w, 0.0, 1.0) + 0.1;
}

std::int64_t swingDelay (std::int64_t start, float swing, float channelMix) noexcept
{
    if (swing <= 0.0f || channelMix <= 0.0f)
        return 0;

    constexpr std::int64_t s = ticksPerStep;
    const auto k = (start + s / 2) / s;

    if (k % 2 == 0)
        return 0;

    const auto w = s - 2 * std::abs (start - k * s);
    return (std::int64_t) std::llround ((double) w * std::clamp (swing, 0.0f, 1.0f) * std::clamp (channelMix, 0.0f, 1.0f) * 0.5);
}

bool PatcherDoc::operator== (const PatcherDoc& o) const
{
    return nodes == o.nodes && wires == o.wires && published == o.published && inputX == o.inputX && inputY == o.inputY && outputX == o.outputX && outputY == o.outputY
           && outputTracks == o.outputTracks && surface == o.surface && surfaceX == o.surfaceX && surfaceY == o.surfaceY
           && fromMidiPorts == o.fromMidiPorts && toMidiPorts == o.toMidiPorts && fromNotesHidden == o.fromNotesHidden;
}

bool PatchNode::BusHead::operator== (const BusHead& o) const { return bus == o.bus && head == o.head && gain == o.gain; }

bool PatchNode::operator== (const PatchNode& o) const
{
    return generator == o.generator && label == o.label && x == o.x && y == o.y && busHeads == o.busHeads
           && (generator ? channel == o.channel : effect == o.effect);
}

bool EffectSlot::operator== (const EffectSlot& o) const
{
    if (kind != o.kind || enabled != o.enabled || params != o.params || mix != o.mix || ! (plugin == o.plugin) || sidechain != o.sidechain
        || sidechain2 != o.sidechain2 || state != o.state || samplePath != o.samplePath || ! (substituteFor == o.substituteFor)
        || presetName != o.presetName || abOther != o.abOther || abShowingB != o.abShowingB || branches.size() != o.branches.size()
        || links.size() != o.links.size() || ! (patcher == o.patcher))
        return false;
    for (std::size_t i = 0; i < branches.size(); ++i)
        if (branches[i].gain != o.branches[i].gain || branches[i].chain != o.branches[i].chain)
            return false;
    for (std::size_t i = 0; i < links.size(); ++i)
    {
        const auto& a = links[i];
        const auto& b = o.links[i];
        if (a.branch != b.branch || a.source != b.source || a.target != b.target || a.param != b.param || a.curve != b.curve)
            return false;
    }
    return true;
}

float channelGain (float volume) noexcept
{
    const auto v = std::clamp (volume, 0.0f, 1.0f);
    return (std::pow (11.0f, v) - 1.0f) / 10.0f;
}

float faderGain (float position) noexcept
{
    const auto v = std::clamp (position, 0.0f, 1.0f) * 1.25f;
    return (std::pow (11.0f, v) - 1.0f) / 10.0f;
}

float faderPosition (float gain) noexcept
{
    if (! (gain > 0.0f))
        return 0.0f;
    return std::clamp ((float) (std::log (10.0 * (double) gain + 1.0) / std::log (11.0) / 1.25), 0.0f, 1.0f);
}

Song makeExportSong (const Song& song, bool wholeSong, std::int64_t tailTicks, std::int64_t& musicLength)
{
    Song out = song;

    if (out.patterns.empty())
        out.patterns.emplace_back().name = "Pattern 1";

    if (! wholeSong || out.playlist.empty())
    {
        const auto index = std::clamp (out.currentPattern, 0, (int) out.patterns.size() - 1);
        Clip clip;
        clip.pattern = index;
        clip.length = out.patterns[(std::size_t) index].effectiveLength (out.beatsPerBar);
        out.playlist = { clip };
        musicLength = clip.length;
    }
    else
    {
        musicLength = 0;

        for (const auto& c : out.playlist)
            musicLength = std::max (musicLength, c.end());
    }

    // empty pattern in the tail: extends the song without playing anything. One extra bar of slack
    // because the last rendered block goes slightly past the end and must not wrap to the start.
    if (tailTicks > 0)
    {
        tailTicks += (std::int64_t) ppq * std::max (1, out.beatsPerBar);
        out.patterns.emplace_back().name = "(tail)";
        Clip tail;
        tail.pattern = (int) out.patterns.size() - 1;
        tail.start = musicLength;
        tail.length = tailTicks;
        out.playlist.push_back (tail);
    }

    out.mode = PlayMode::song;
    out.loopStart = out.loopEnd = 0; // export ignores the loop region
    return out;
}

Song makeBounceSong (const Song& song, int track)
{
    Song out = song;

    if (track <= 0 || track >= (int) out.mixer.size())
        return out;

    for (auto& t : out.mixer)
        t.solo = false;

    auto& insert = out.mixer[(std::size_t) track];
    insert.sends.clear();
    insert.toMaster = true;
    insert.muted = false;
    insert.solo = true; // it and whatever feeds it are audible

    auto& master = out.mixer[0];
    master.effects.clear();
    master.volume = unityFader;
    master.pan = 0.0f;
    master.muted = false;
    out.mode = PlayMode::song;
    return out;
}

int applyBounce (Song& song, int track, const std::string& samplePath, std::int64_t length)
{
    if (track <= 0 || track >= (int) song.mixer.size() || (int) song.mixer.size() >= maxMixerTracks)
        return -1;

    auto& original = song.mixer[(std::size_t) track];
    MixerTrack copy;
    copy.name = original.name + " (audio)";
    copy.toMaster = original.toMaster;
    copy.masterLevel = original.masterLevel;
    copy.sends = original.sends;
    original.muted = true;
    song.mixer.push_back (copy);
    const auto newTrack = (int) song.mixer.size() - 1;

    ChannelDef c;
    c.name = copy.name;
    c.instrument = InstrumentKind::audioClip;
    c.samplePath = samplePath;
    c.mixerTrack = newTrack;
    c.volume = 1.0f;
    song.channels.push_back (c);

    int lane = 0;
    for (const auto& clip : song.playlist)
        lane = std::max (lane, clip.track + 1);

    Clip clip;
    clip.kind = ClipKind::audio;
    clip.channel = (int) song.channels.size() - 1;
    clip.track = lane;
    clip.length = length;
    song.playlist.push_back (clip);
    return newTrack;
}

Song Song::makeDefault()
{
    Song song;
    song.mixer = defaultMixer();
    auto channel = [] (const char* name, InstrumentKind kind, float volume, float pan) {
        ChannelDef c;
        c.name = name;
        c.instrument = kind;
        c.volume = volume;
        c.pan = pan;
        return c;
    };

    song.channels = {
        channel ("Kick", InstrumentKind::kick, 0.9f, 0.0f),
        channel ("Clap", InstrumentKind::clap, 0.8f, 0.0f),
        channel ("Hat", InstrumentKind::hat, 0.7f, 0.1f),
        channel ("Snare", InstrumentKind::snare, 0.8f, 0.0f),
        channel ("Synth", InstrumentKind::synth, 0.78f, 0.0f),
    };

    for (std::size_t i = 0; i < song.channels.size(); ++i)
        song.channels[i].mixerTrack = (int) i + 1;

    // like FL's template: a limiter on Master (transparent up to -0.3 dB)
    EffectSlot limiter;
    limiter.kind = EffectKind::limiter;
    limiter.params = { 11.7f / 12.0f, 0.0f, 0.5f };
    song.mixer[0].effects.push_back (limiter);

    auto& p = song.patterns.emplace_back();
    p.name = "Pattern 1";

    for (int step : { 0, 4, 8, 12 })
        p.toggleStep (0, step);

    for (int step : { 4, 12 })
        p.toggleStep (1, step);

    for (int step : { 2, 6, 10, 14 })
        p.toggleStep (2, step);

    return song;
}
} // namespace lynx
