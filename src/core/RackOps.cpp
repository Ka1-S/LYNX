#include "core/RackOps.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <functional>
#include <numeric>

namespace lynx
{
const std::array<std::uint32_t, 12>& lynxPalette()
{
    static const std::array<std::uint32_t, 12> palette { 0xffd94a4fu, 0xffe07a3fu, 0xffd9b23cu, 0xff8fbf45u, 0xff3fae6au, 0xff35a8a0u,
                                                         0xff3d8fd1u, 0xff5b6ee0u, 0xff8a5cd6u, 0xffc25bb0u, 0xffd9668cu, 0xff7d8794u };
    return palette;
}

std::uint32_t randomPaletteColour (std::uint32_t current, std::uint32_t seed)
{
    const auto& palette = lynxPalette();
    // xorshift so consecutive seeds don't walk the palette in order
    auto x = seed * 2654435761u + 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    auto index = x % (std::uint32_t) palette.size();

    if (palette[index] == current)
        index = (index + 1 + (x >> 8) % (std::uint32_t) (palette.size() - 1)) % (std::uint32_t) palette.size();

    return palette[index];
}

// --- patterns ----------------------------------------------------------------------------------

bool isPatternEmpty (const Pattern& pattern) noexcept
{
    return pattern.notes.empty();
}

int stepPattern (const Song& song, int delta) noexcept
{
    if (song.patterns.empty())
        return 0;

    return std::clamp (song.currentPattern + delta, 0, (int) song.patterns.size() - 1);
}

int findNextEmptyPattern (const Song& song, int from) noexcept
{
    const auto count = (int) song.patterns.size();

    for (int i = 1; i < count; ++i)
    {
        const auto index = ((from + i) % count + count) % count;

        if (isPatternEmpty (song.patterns[(std::size_t) index]))
            return index;
    }

    return -1;
}

std::string newPatternName (const Song& song)
{
    for (int n = 1;; ++n)
    {
        const auto name = "Pattern " + std::to_string (n);

        if (std::none_of (song.patterns.begin(), song.patterns.end(), [&] (const Pattern& p) { return p.name == name; }))
            return name;
    }
}

int gotoNextEmptyPattern (Song& song)
{
    // the current pattern counts when it is empty and nothing else is
    auto index = findNextEmptyPattern (song, song.currentPattern);

    if (index < 0)
    {
        if (! song.patterns.empty() && isPatternEmpty (song.patterns[(std::size_t) song.currentPattern]))
            return song.currentPattern;

        Pattern p;
        p.name = newPatternName (song);
        song.patterns.push_back (std::move (p));
        index = (int) song.patterns.size() - 1;
    }

    song.currentPattern = index;
    return index;
}

namespace
{
// Shifts pattern clip references at or after `index` by `delta`.
void shiftPatternClips (Song& song, int index, int delta)
{
    song.forEachClipList ([index, delta] (std::vector<Clip>& clips) {
        for (auto& c : clips)
            if (c.kind == ClipKind::pattern && c.pattern >= index)
                c.pattern += delta;
    });
}
} // namespace

int insertPattern (Song& song, int index, const std::string& name)
{
    index = std::clamp (index, 0, (int) song.patterns.size());
    shiftPatternClips (song, index, 1);
    Pattern p;
    p.name = name;
    song.patterns.insert (song.patterns.begin() + index, std::move (p));
    song.currentPattern = index;
    return index;
}

int splitPatternByChannel (Song& song, int index)
{
    if (index < 0 || index >= (int) song.patterns.size())
        return 0;

    const auto source = song.patterns[(std::size_t) index];
    std::vector<int> channels;

    for (const auto& n : source.notes)
        if (std::find (channels.begin(), channels.end(), n.channel) == channels.end())
            channels.push_back (n.channel);

    std::sort (channels.begin(), channels.end());
    std::vector<Pattern> created;

    for (auto channel : channels)
    {
        Pattern p;
        const auto channelName = channel >= 0 && channel < (int) song.channels.size() ? song.channels[(std::size_t) channel].name
                                                                                         : "Channel " + std::to_string (channel + 1);
        p.name = source.name + " - " + channelName;
        p.lengthTicks = source.lengthTicks != 0 ? source.lengthTicks : source.effectiveLength (song.beatsPerBar);
        p.colour = channel >= 0 && channel < (int) song.channels.size() ? song.channels[(std::size_t) channel].colour : source.colour;

        for (const auto& n : source.notes)
            if (n.channel == channel)
                p.notes.push_back (n);

        created.push_back (std::move (p));
    }

    if (created.empty())
        return 0;

    shiftPatternClips (song, index + 1, (int) created.size());
    song.patterns.insert (song.patterns.begin() + index + 1, created.begin(), created.end());

    if (song.currentPattern > index)
        song.currentPattern += (int) created.size();

    return (int) created.size();
}

namespace
{
std::string nextCopyName (const std::string& base, const std::function<bool (const std::string&)>& taken)
{
    for (int n = 2;; ++n)
    {
        const auto name = base + " #" + std::to_string (n);
        if (! taken (name))
            return name;
    }
}
} // namespace

int clonePattern (Song& song, int index)
{
    if (index < 0 || index >= (int) song.patterns.size())
        return -1;

    auto copy = song.patterns[(std::size_t) index];
    copy.name = nextCopyName (copy.name, [&song] (const std::string& name) {
        return std::any_of (song.patterns.begin(), song.patterns.end(), [&] (const Pattern& p) { return p.name == name; });
    });
    shiftPatternClips (song, index + 1, 1);
    song.patterns.insert (song.patterns.begin() + index + 1, std::move (copy));
    song.currentPattern = index + 1;
    return index + 1;
}

bool movePattern (Song& song, int index, int delta)
{
    const auto other = index + (delta < 0 ? -1 : 1);

    if (delta == 0 || index < 0 || index >= (int) song.patterns.size() || other < 0 || other >= (int) song.patterns.size())
        return false;

    std::swap (song.patterns[(std::size_t) index], song.patterns[(std::size_t) other]);
    song.forEachClipList ([index, other] (std::vector<Clip>& clips) {
        for (auto& c : clips)
            if (c.kind == ClipKind::pattern)
                c.pattern = c.pattern == index ? other : c.pattern == other ? index : c.pattern;
    });

    if (song.currentPattern == index)
        song.currentPattern = other;
    else if (song.currentPattern == other)
        song.currentPattern = index;

    return true;
}

// --- step sequencer ----------------------------------------------------------------------------

int patternSteps (const Song& song, const Pattern& pattern) noexcept
{
    return std::clamp ((int) (pattern.effectiveLength (song.beatsPerBar) / ticksPerStep), 16, 512);
}

bool showsNotePreview (const Pattern& pattern, int channel) noexcept
{
    return std::any_of (pattern.notes.begin(), pattern.notes.end(), [channel] (const Note& n) {
        return n.channel == channel && (n.start % ticksPerStep != 0 || n.length > ticksPerStep || n.key != 60 || n.slide || n.porta);
    });
}

bool showsNotePreview (const Pattern& pattern, int channel, bool pianoRollView) noexcept
{
    return pianoRollView || showsNotePreview (pattern, channel);
}

std::pair<int, int> channelKeyRange (const Pattern& pattern, int channel) noexcept
{
    auto low = 128, high = -1;

    for (const auto& n : pattern.notes)
        if (n.channel == channel)
        {
            low = std::min (low, n.key);
            high = std::max (high, n.key);
        }

    return high < 0 ? std::pair { 60, 60 } : std::pair { low, high };
}

std::vector<bool> euclideanRhythm (int pulses, int steps, int rotation)
{
    if (steps <= 0)
        return {};

    pulses = std::clamp (pulses, 0, steps);
    std::vector<bool> out ((std::size_t) steps, false);

    // step i is an onset when the running count floor(i * pulses / steps) goes up there: the onsets
    // are spread as evenly as integers allow, and the first step is always one
    for (int i = 0; i < steps; ++i)
        out[(std::size_t) i] = pulses > 0 && (i * pulses) % steps < pulses;

    rotation = ((rotation % steps) + steps) % steps;
    std::rotate (out.rbegin(), out.rbegin() + rotation, out.rend());
    return out;
}

void fillEverySteps (Pattern& pattern, int channel, int every, int steps, int offset)
{
    every = std::max (1, every);

    for (int s = std::max (0, offset); s < steps; s += every)
        if (! pattern.hasStep (channel, s))
            pattern.toggleStep (channel, s);
}

std::int64_t lengthWithStep (std::int64_t lengthTicks, int step, std::int64_t ticksPerStep, std::int64_t barTicks) noexcept
{
    const auto end = (std::int64_t) (step + 1) * ticksPerStep;
    if (lengthTicks <= 0 || step < 0 || end <= lengthTicks || barTicks <= 0)
        return lengthTicks;
    return (end + barTicks - 1) / barTicks * barTicks;
}

void setStepRhythm (Pattern& pattern, int channel, const std::vector<bool>& rhythm, int steps)
{
    const auto end = (std::int64_t) steps * ticksPerStep;
    auto& notes = pattern.notes;
    notes.erase (std::remove_if (notes.begin(), notes.end(),
                                 [&] (const Note& n) { return n.channel == channel && n.start < end && n.start % ticksPerStep == 0; }),
                 notes.end());

    if (! rhythm.empty())
        for (int s = 0; s < steps; ++s)
            if (rhythm[(std::size_t) s % rhythm.size()])
            {
                Note n;
                n.start = (std::int64_t) s * ticksPerStep;
                n.channel = channel;
                notes.push_back (n);
            }

    pattern.sortNotes();
}

void rotateSteps (Pattern& pattern, int channel, int delta, int steps)
{
    if (steps <= 0)
        return;

    const auto length = (std::int64_t) steps * ticksPerStep;
    const auto shift = (std::int64_t) delta * ticksPerStep;

    for (auto& n : pattern.notes)
        if (n.channel == channel && n.start < length)
            n.start = ((n.start + shift) % length + length) % length;

    pattern.sortNotes();
}

bool hasPianoRollNotes (const Pattern& pattern) noexcept
{
    return std::any_of (pattern.notes.begin(), pattern.notes.end(), [] (const Note& n) {
        return n.start % ticksPerStep != 0 || n.key != 60 || n.length > ticksPerStep;
    });
}

std::vector<int> notesInStep (const Pattern& pattern, int channel, int step)
{
    const auto from = (std::int64_t) step * ticksPerStep;
    std::vector<int> out;

    for (int i = 0; i < (int) pattern.notes.size(); ++i)
    {
        const auto& n = pattern.notes[(std::size_t) i];
        if (n.channel == channel && n.start >= from && n.start < from + ticksPerStep)
            out.push_back (i);
    }

    return out;
}

// --- channel organisation ----------------------------------------------------------------------

std::vector<int> channelSortOrder (const Song& song, ChannelSort by)
{
    std::vector<int> order (song.channels.size());
    std::iota (order.begin(), order.end(), 0);

    auto lower = [] (std::string s) {
        for (auto& c : s)
            c = (char) std::tolower ((unsigned char) c);
        return s;
    };

    // colour order: by hue, uncoloured channels last
    auto hue = [] (std::uint32_t argb) {
        if (argb == 0)
            return 1000.0;
        const auto r = ((argb >> 16) & 0xff) / 255.0, g = ((argb >> 8) & 0xff) / 255.0, b = (argb & 0xff) / 255.0;
        const auto mx = std::max ({ r, g, b }), mn = std::min ({ r, g, b }), d = mx - mn;
        if (d <= 1e-9)
            return 400.0 + mx; // greys after the colours
        double h = mx == r ? std::fmod ((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0;
        h = h < 0.0 ? h * 60.0 + 360.0 : h * 60.0;
        return h >= 345.0 ? h - 360.0 : h; // reds just below 360 sort with the other reds, first
    };

    const auto& c = song.channels;
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) {
        const auto& x = c[(std::size_t) a];
        const auto& y = c[(std::size_t) b];
        switch (by)
        {
            case ChannelSort::name: return lower (x.name) < lower (y.name);
            case ChannelSort::colour: return hue (x.colour) < hue (y.colour);
            case ChannelSort::insert: return (x.mixerTrack <= 0 ? 1 << 20 : x.mixerTrack) < (y.mixerTrack <= 0 ? 1 << 20 : y.mixerTrack);
            case ChannelSort::type: return (int) x.instrument < (int) y.instrument;
        }
        return false;
    });

    return order;
}

std::vector<int> unusedChannels (const Song& song)
{
    std::vector<char> used (song.channels.size(), 0);
    auto mark = [&used] (int channel) {
        if (channel >= 0 && channel < (int) used.size())
            used[(std::size_t) channel] = 1;
    };

    for (const auto& p : song.patterns)
        for (const auto& n : p.notes)
            mark (n.channel);

    auto markClips = [&mark] (const std::vector<Clip>& clips) {
        for (const auto& clip : clips)
            if (clip.kind == ClipKind::audio)
                mark (clip.channel);
    };
    markClips (song.playlist);
    for (const auto& a : song.arrangements)
        markClips (a.clips);

    for (const auto& a : song.automations)
        mark (a.source.channel);

    // a used Layer's children play its notes (round 164)
    for (std::size_t c = 0; c < song.channels.size(); ++c)
        if (used[c] && song.channels[c].instrument == InstrumentKind::layer)
            for (const auto k : song.channels[c].layer.children)
                mark (k);

    std::vector<int> out;
    for (int i = 0; i < (int) used.size(); ++i)
        if (! used[(std::size_t) i])
            out.push_back (i);

    return out;
}

// --- channel selection -------------------------------------------------------------------------

bool ChannelSelection::contains (int row) const noexcept
{
    return std::binary_search (rows.begin(), rows.end(), row);
}

void ChannelSelection::selectOnly (int row)
{
    rows = { row };
    anchor = row;
}

void ChannelSelection::toggle (int row)
{
    if (const auto it = std::lower_bound (rows.begin(), rows.end(), row); it != rows.end() && *it == row)
    {
        if (rows.size() > 1) // one channel always stays selected (it is the keyboard/MIDI target)
            rows.erase (it);
    }
    else
        rows.insert (it, row);

    anchor = row;
}

void ChannelSelection::extendTo (int row)
{
    rows.clear();

    for (int r = std::min (anchor, row); r <= std::max (anchor, row); ++r)
        rows.push_back (r);
}

void ChannelSelection::selectAll (int count)
{
    rows.resize ((std::size_t) std::max (0, count));
    std::iota (rows.begin(), rows.end(), 0);
}

void ChannelSelection::clamp (int count)
{
    rows.erase (std::remove_if (rows.begin(), rows.end(), [count] (int r) { return r < 0 || r >= count; }), rows.end());

    if (rows.empty() && count > 0)
        rows = { std::clamp (anchor, 0, count - 1) };

    anchor = std::clamp (anchor, 0, std::max (0, count - 1));
}

// --- channel operations ------------------------------------------------------------------------

void permuteChannels (Song& song, const std::vector<int>& newOrder)
{
    const auto count = (int) song.channels.size();

    if ((int) newOrder.size() != count)
        return;

    std::vector<int> newIndexOf ((std::size_t) count, -1); // old -> new

    for (int i = 0; i < count; ++i)
    {
        const auto old = newOrder[(std::size_t) i];

        if (old < 0 || old >= count || newIndexOf[(std::size_t) old] >= 0)
            return; // not a permutation

        newIndexOf[(std::size_t) old] = i;
    }

    auto map = [&] (int channel) { return channel >= 0 && channel < count ? newIndexOf[(std::size_t) channel] : channel; };

    std::vector<ChannelDef> reordered;
    reordered.reserve ((std::size_t) count);

    for (auto old : newOrder)
        reordered.push_back (std::move (song.channels[(std::size_t) old]));

    song.channels = std::move (reordered);

    for (auto& p : song.patterns)
    {
        for (auto& n : p.notes)
            n.channel = map (n.channel);

        p.sortNotes();
    }

    song.forEachClipList ([&] (std::vector<Clip>& clips) {
        for (auto& c : clips)
            if (c.kind == ClipKind::audio)
                c.channel = map (c.channel);
    });

    song.forEachTarget ([&] (AutomationTarget& t) {
        if (t.kind == AutomationTargetKind::channelVolume || t.kind == AutomationTargetKind::channelPan
            || t.kind == AutomationTargetKind::channelParam)
            t.index = map (t.index);
    });

    for (auto& a : song.automations)
        if (a.source.channel >= 0)
            a.source.channel = map (a.source.channel);

    for (auto& c : song.channels) // Layer children (round 164)
        for (auto& k : c.layer.children)
            k = map (k);
}

bool moveChannels (Song& song, std::vector<int>& selection, int delta)
{
    const auto count = (int) song.channels.size();
    std::sort (selection.begin(), selection.end());
    selection.erase (std::unique (selection.begin(), selection.end()), selection.end());
    selection.erase (std::remove_if (selection.begin(), selection.end(), [count] (int r) { return r < 0 || r >= count; }), selection.end());

    if (selection.empty() || delta == 0 || (delta < 0 && selection.front() == 0) || (delta > 0 && selection.back() == count - 1))
        return false;

    std::vector<int> order ((std::size_t) count);
    std::iota (order.begin(), order.end(), 0);
    std::vector<char> selected ((std::size_t) count, 0);

    for (auto r : selection)
        selected[(std::size_t) r] = 1;

    // bubble each selected row one step, as a block (rows already blocked stay)
    if (delta < 0)
    {
        for (int i = 1; i < count; ++i)
            if (selected[(std::size_t) i] && ! selected[(std::size_t) i - 1])
            {
                std::swap (order[(std::size_t) i], order[(std::size_t) i - 1]);
                std::swap (selected[(std::size_t) i], selected[(std::size_t) i - 1]);
            }
    }
    else
    {
        for (int i = count - 2; i >= 0; --i)
            if (selected[(std::size_t) i] && ! selected[(std::size_t) i + 1])
            {
                std::swap (order[(std::size_t) i], order[(std::size_t) i + 1]);
                std::swap (selected[(std::size_t) i], selected[(std::size_t) i + 1]);
            }
    }

    permuteChannels (song, order);
    selection.clear();

    for (int i = 0; i < count; ++i)
        if (selected[(std::size_t) i])
            selection.push_back (i);

    return true;
}

int insertChannel (Song& song, int index, ChannelDef channel)
{
    const auto count = (int) song.channels.size();
    index = std::clamp (index, 0, count);
    song.channels.push_back (std::move (channel));

    // move the new last channel up to `index`
    std::vector<int> order;
    order.reserve ((std::size_t) count + 1);

    for (int i = 0; i < index; ++i)
        order.push_back (i);

    order.push_back (count);

    for (int i = index; i < count; ++i)
        order.push_back (i);

    permuteChannels (song, order);
    return index;
}

int cloneChannel (Song& song, int index)
{
    if (index < 0 || index >= (int) song.channels.size())
        return -1;

    auto copy = song.channels[(std::size_t) index];
    const auto base = copy.name;

    for (int n = 2;; ++n)
    {
        const auto name = base + " #" + std::to_string (n);

        if (std::none_of (song.channels.begin(), song.channels.end(), [&] (const ChannelDef& c) { return c.name == name; }))
        {
            copy.name = name;
            break;
        }
    }

    return insertChannel (song, index + 1, std::move (copy));
}

int removeChannels (Song& song, std::vector<int> channels)
{
    std::sort (channels.begin(), channels.end());
    channels.erase (std::unique (channels.begin(), channels.end()), channels.end());
    int removed = 0;

    for (auto it = channels.rbegin(); it != channels.rend(); ++it)
        if (*it >= 0 && *it < (int) song.channels.size())
        {
            song.removeChannel (*it);
            ++removed;
        }

    return removed;
}

bool isInsertFree (const Song& song, int track) noexcept
{
    if (track <= 0 || track >= (int) song.mixer.size())
        return false;

    if (std::any_of (song.channels.begin(), song.channels.end(), [track] (const ChannelDef& c) { return c.mixerTrack == track; }))
        return false;

    if (! song.mixer[(std::size_t) track].effects.empty())
        return false;

    for (int t = 1; t < (int) song.mixer.size(); ++t)
        if (t != track && song.routes (t, track))
            return false;

    return true;
}

std::vector<int> sendChannelsToFreeInserts (Song& song, const std::vector<int>& channels)
{
    std::vector<int> result;

    if (song.mixer.empty())
        song.mixer = Song::defaultMixer (0);

    for (auto channel : channels)
    {
        if (channel < 0 || channel >= (int) song.channels.size())
        {
            result.push_back (0);
            continue;
        }

        int track = 0;

        for (int t = 1; t < (int) song.mixer.size() && track == 0; ++t)
            if (isInsertFree (song, t))
                track = t;

        if (track == 0 && (int) song.mixer.size() < maxMixerTracks)
        {
            song.mixer.emplace_back();
            track = (int) song.mixer.size() - 1;
        }

        if (track > 0)
        {
            auto& c = song.channels[(std::size_t) channel];
            c.mixerTrack = track;
            song.mixer[(std::size_t) track].name = c.name;
            song.mixer[(std::size_t) track].colour = c.colour;
        }

        result.push_back (track);
    }

    return result;
}

// --- Channel settings "Misc" tab ------------------------------------------------------------------------------

bool channelHasMono (const ChannelDef& c) noexcept
{
    return c.instrument == InstrumentKind::synth;
}

bool channelHasPortamento (const ChannelDef& c) noexcept
{
    return c.instrument == InstrumentKind::synth || c.instrument == InstrumentKind::sampler;
}

bool channelMono (const ChannelDef& c) noexcept
{
    return c.instrument == InstrumentKind::synth && c.synth.mono;
}

void setChannelMono (ChannelDef& c, bool on) noexcept
{
    if (c.instrument == InstrumentKind::synth)
        c.synth.mono = on;
}

bool channelPortamento (const ChannelDef& c) noexcept
{
    return c.instrument == InstrumentKind::synth ? c.synth.porta : c.instrument == InstrumentKind::sampler && c.sampler.porta;
}

float channelPortamentoBeats (const ChannelDef& c) noexcept
{
    return c.instrument == InstrumentKind::sampler ? c.sampler.portaBeats : c.synth.portaBeats;
}

void setChannelPortamento (ChannelDef& c, bool on, float beats) noexcept
{
    beats = std::clamp (beats, 0.0f, 4.0f);
    if (c.instrument == InstrumentKind::synth)
    {
        c.synth.porta = on;
        c.synth.portaBeats = beats;
    }
    else if (c.instrument == InstrumentKind::sampler)
    {
        c.sampler.porta = on;
        c.sampler.portaBeats = beats;
    }
}

bool channelCutsItself (const ChannelDef& c) noexcept
{
    return c.cutGroup != 0 && c.cutGroup == c.cutBy;
}

void setChannelCutsItself (ChannelDef& c, int channel, bool on) noexcept
{
    c.cutGroup = c.cutBy = on ? 1000 + channel : 0;
}

std::string cutGroupText (int group, int channel)
{
    if (group <= 0)
        return "--";
    if (group == 1000 + channel)
        return "self";
    return std::to_string (group);
}

std::string stepPositionText (std::int64_t tick, int beatsPerBar)
{
    tick = std::max<std::int64_t> (0, tick);
    const auto barTicks = (std::int64_t) ppq * std::max (1, beatsPerBar);
    const auto bar = tick / barTicks + 1;
    const auto inBar = tick % barTicks;
    const auto step = inBar / ticksPerStep + 1;
    const auto flTick = (inBar % ticksPerStep) * 24 / ticksPerStep; // FL's 96 PPQ: 24 ticks per step
    char text[32];
    std::snprintf (text, sizeof (text), "%lld:%02lld:%02lld", (long long) bar, (long long) step, (long long) flTick);
    return text;
}
} // namespace lynx
