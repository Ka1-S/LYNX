#include "core/ChannelLoops.h"

#include <algorithm>

namespace lynx::channelloops
{
namespace
{
const ChannelLoop* findEntry (const Pattern& pattern, int channel) noexcept
{
    for (const auto& l : pattern.channelLoops)
        if (l.channel == channel)
            return &l;
    return nullptr;
}

// FL's step channel (FUN_01001570: no note of its own length): the rack shows it as steps
bool isStepChannel (const Pattern& pattern, int channel) noexcept
{
    return std::none_of (pattern.notes.begin(), pattern.notes.end(), [channel] (const Note& n) {
        return n.channel == channel && (n.start % ticksPerStep != 0 || n.length > ticksPerStep || n.key != 60 || n.slide || n.porta);
    });
}

bool hasNotes (const Pattern& pattern, int channel) noexcept
{
    return std::any_of (pattern.notes.begin(), pattern.notes.end(), [channel] (const Note& n) { return n.channel == channel; });
}

std::int64_t barTicks (int beatsPerBar) noexcept { return (std::int64_t) ppq * std::max (1, beatsPerBar); }
} // namespace

int storedSetting (const Pattern& pattern, int channel) noexcept
{
    const auto* e = findEntry (pattern, channel);
    return e != nullptr ? e->setting : ChannelLoop::dontLoop;
}

bool hasStoredSetting (const Pattern& pattern, int channel) noexcept { return findEntry (pattern, channel) != nullptr; }

void setSetting (Pattern& pattern, int channel, int setting)
{
    for (auto& l : pattern.channelLoops)
        if (l.channel == channel)
        {
            l.setting = setting;
            return;
        }
    pattern.channelLoops.push_back ({ channel, setting });
}

int effectiveSetting (const Pattern& pattern, int channel) noexcept
{
    if (! pattern.loopOn)
        return ChannelLoop::dontLoop;
    if (const auto* e = findEntry (pattern, channel))
        return e->setting;
    // FL fills in "Bar" for the channels the type loops when it is switched on (FUN_01045450)
    if (pattern.loopMode == RackLoopMode::all || (pattern.loopMode == RackLoopMode::steps && isStepChannel (pattern, channel)))
        return ChannelLoop::nextBar;
    return ChannelLoop::dontLoop;
}

std::int64_t lastTick (const Pattern& pattern, int channel) noexcept
{
    std::int64_t last = -1;
    for (const auto& n : pattern.notes)
        if (n.channel == channel && ! n.muted)
            last = std::max (last, n.start + std::max<std::int64_t> (n.length - 1, 0));
    return last;
}

namespace
{
std::int64_t boundaryAfter (std::int64_t last, int setting, int beatsPerBar) noexcept
{
    last = std::max<std::int64_t> (last, 0);
    const auto unit = setting == ChannelLoop::nextStep ? (std::int64_t) ticksPerStep
                    : setting == ChannelLoop::nextBeat ? (std::int64_t) ppq
                                                       : barTicks (beatsPerBar);
    return (last / unit + 1) * unit;
}
} // namespace

std::int64_t loopLength (const Pattern& pattern, int channel, int setting, std::int64_t patternLength, int beatsPerBar) noexcept
{
    if (setting == ChannelLoop::dontLoop || patternLength <= 0)
        return 0;
    std::int64_t length = setting > 0 ? (std::int64_t) setting * ticksPerStep
                                      : boundaryAfter (lastTick (pattern, channel), setting, beatsPerBar);
    length = std::min (length, patternLength);
    return length > 0 ? length : patternLength;
}

int nextBoundarySteps (const Pattern& pattern, int channel, int boundary, int beatsPerBar) noexcept
{
    const auto ticks = boundaryAfter (lastTick (pattern, channel), boundary == ChannelLoop::nextBar ? ChannelLoop::nextBar : ChannelLoop::nextBeat, beatsPerBar);
    return (int) std::max<std::int64_t> (1, ticks / ticksPerStep);
}

void expandChannel (Pattern& pattern, int channel, std::int64_t length, std::int64_t patternLength)
{
    if (length <= 0 || length >= patternLength)
        return;
    std::vector<Note> source;
    std::vector<Note> kept;
    kept.reserve (pattern.notes.size());
    for (const auto& n : pattern.notes)
    {
        if (n.channel != channel)
            kept.push_back (n);
        else if (n.start < length) // FL plays only the notes before the loop point (FUN_01064140)
            source.push_back (n);
    }
    for (const auto& n : source)
        kept.push_back (n);
    for (std::int64_t offset = length; offset < patternLength; offset += length)
        for (auto n : source)
        {
            n.start += offset;
            if (n.start >= patternLength)
                continue;
            n.length = std::min (n.length, patternLength - n.start);
            kept.push_back (n);
        }
    pattern.notes = std::move (kept);
    pattern.sortNotes();
}

void expandPattern (Pattern& pattern, int beatsPerBar)
{
    if (! pattern.loopOn)
        return;
    const auto patternLength = pattern.effectiveLength (beatsPerBar);
    std::vector<int> channels;
    for (const auto& n : pattern.notes)
        if (std::find (channels.begin(), channels.end(), n.channel) == channels.end())
            channels.push_back (n.channel);
    std::vector<std::pair<int, std::int64_t>> lengths;
    for (auto c : channels)
        lengths.emplace_back (c, loopLength (pattern, c, effectiveSetting (pattern, c), patternLength, beatsPerBar));
    for (const auto& [c, length] : lengths)
        expandChannel (pattern, c, length, patternLength);
    if (pattern.lengthTicks <= 0)
        pattern.lengthTicks = patternLength; // the repeats never make an automatic length longer
    pattern.loopOn = false;
}

void expandSong (Song& song)
{
    for (auto& p : song.patterns)
        expandPattern (p, song.beatsPerBar);
}

std::vector<int> loopingChannels (const Pattern& pattern, int beatsPerBar)
{
    std::vector<int> out;
    if (! pattern.loopOn)
        return out;
    const auto patternLength = pattern.effectiveLength (beatsPerBar);
    std::vector<int> channels;
    for (const auto& n : pattern.notes)
        if (std::find (channels.begin(), channels.end(), n.channel) == channels.end())
            channels.push_back (n.channel);
    std::sort (channels.begin(), channels.end());
    for (auto c : channels)
    {
        const auto length = loopLength (pattern, c, effectiveSetting (pattern, c), patternLength, beatsPerBar);
        if (length > 0 && length < patternLength)
            out.push_back (c);
    }
    return out;
}

bool burnChannel (Pattern& pattern, int channel, int beatsPerBar)
{
    if (! pattern.loopOn || ! hasNotes (pattern, channel))
        return false;
    const auto setting = effectiveSetting (pattern, channel);
    if (setting == ChannelLoop::dontLoop)
        return false;
    const auto patternLength = pattern.effectiveLength (beatsPerBar);
    const auto length = loopLength (pattern, channel, setting, patternLength, beatsPerBar);
    const auto auto_ = pattern.lengthTicks <= 0;
    if (length > 0 && length < patternLength)
        expandChannel (pattern, channel, length, patternLength);
    // (FL's burn: the loop point becomes "Don't loop", FUN_012cf150)
    setSetting (pattern, channel, ChannelLoop::dontLoop);
    if (auto_ && pattern.effectiveLength (beatsPerBar) != patternLength)
        pattern.lengthTicks = patternLength;
    return true;
}

int burnChannels (Pattern& pattern, const std::vector<int>& channels, int beatsPerBar)
{
    int burnt = 0;
    const auto list = channels.empty() ? loopingChannels (pattern, beatsPerBar) : channels;
    for (auto c : list)
        if (burnChannel (pattern, c, beatsPerBar))
            ++burnt;
    return burnt;
}

std::string settingText (int setting)
{
    switch (setting)
    {
        case ChannelLoop::dontLoop: return "...";
        case ChannelLoop::nextStep: return "Step";
        case ChannelLoop::nextBeat: return "Beat";
        case ChannelLoop::nextBar: return "Bar";
        default: return std::to_string (setting);
    }
}

void removeChannel (Pattern& pattern, int channel)
{
    auto& loops = pattern.channelLoops;
    loops.erase (std::remove_if (loops.begin(), loops.end(), [channel] (const ChannelLoop& l) { return l.channel == channel; }), loops.end());
    for (auto& l : loops)
        if (l.channel > channel)
            --l.channel;
}

void remapChannels (Pattern& pattern, const std::vector<int>& newIndexOf)
{
    for (auto& l : pattern.channelLoops)
        if (l.channel >= 0 && l.channel < (int) newIndexOf.size())
            l.channel = newIndexOf[(std::size_t) l.channel];
}
} // namespace lynx::channelloops
