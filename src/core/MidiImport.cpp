#include "core/MidiImport.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "core/MidiControls.h"

namespace lynx::midiimport
{
namespace
{
bool selectedTrack (const Options& o, int track) noexcept { return o.track < 0 || o.track == track; }
bool selectedChannel (const Options& o, int midiChannel) noexcept { return (o.channels >> (midiChannel & 15) & 1) != 0; }

struct ParsedNote
{
    int track = 0, midiChannel = 0;
    Note note;
};

// The notes of the selected tracks / channels: note on ... note off (or note on with velocity 0) pairs, the oldest
// open note of a key first; with zeroVelocityNotes a note on with velocity 0 opens a note (velocity 0) that the next
// event of its key ends.
std::vector<ParsedNote> notesOf (const ParsedMidi& midi, const Options& o)
{
    std::vector<ParsedNote> out;
    std::map<std::tuple<int, int, int>, std::vector<std::size_t>> open; // (track, channel, key) → indices in out
    auto close = [&] (std::vector<std::size_t>& list, std::int64_t tick) {
        if (list.empty())
            return false;
        auto& n = out[list.front()].note;
        n.length = std::max<std::int64_t> (1, tick - n.start);
        list.erase (list.begin());
        return true;
    };
    for (const auto& e : midi.events)
    {
        const auto type = e.status & 0xF0;
        const auto ch = e.status & 0x0F;
        if ((type != 0x80 && type != 0x90) || ! selectedTrack (o, e.track) || ! selectedChannel (o, ch))
            continue;
        auto& list = open[{ e.track, ch, e.data1 }];
        const auto isOn = type == 0x90 && (e.data2 > 0 || o.zeroVelocityNotes);
        if (! isOn)
        {
            close (list, e.tick);
            continue;
        }
        // with zero-velocity notes, a note on ends the open notes of its key
        if (o.zeroVelocityNotes)
            while (close (list, e.tick))
            {
            }
        ParsedNote p;
        p.track = e.track;
        p.midiChannel = ch;
        p.note.start = e.tick;
        p.note.length = ticksPerStep; // until its note off
        p.note.key = e.data1 & 0x7F;
        p.note.velocity = e.data2 / 127.0f;
        list.push_back (out.size());
        out.push_back (p);
    }
    std::stable_sort (out.begin(), out.end(), [] (const ParsedNote& a, const ParsedNote& b) { return a.note.start < b.note.start; });
    return out;
}
} // namespace

AutomationTarget controllerTarget (int cc, int channel) noexcept
{
    if (cc == 7 || cc == 11)
        return { AutomationTargetKind::channelVolume, channel };
    if (cc == 10)
        return { AutomationTargetKind::channelPan, channel };
    return {};
}

std::vector<int> tracksWithContent (const ParsedMidi& midi)
{
    std::set<int> tracks;
    for (const auto& e : midi.events)
        if ((e.status & 0xF0) == 0x90 || (e.status & 0xF0) == 0xB0)
            tracks.insert (e.track);
    return { tracks.begin(), tracks.end() };
}

std::vector<Group> groups (const ParsedMidi& midi, const Options& o)
{
    const auto notes = notesOf (midi, o);
    std::vector<Group> out;
    std::map<int, std::size_t> byKey; // track or MIDI channel → group
    auto groupFor = [&] (int track, int midiChannel) -> Group& {
        const auto key = o.perTrack ? track : midiChannel;
        if (const auto it = byKey.find (key); it != byKey.end())
            return out[it->second];
        Group g;
        g.midiChannel = midiChannel;
        g.track = o.perTrack ? track : -1;
        if (o.perTrack)
        {
            const auto& name = track < (int) midi.trackNames.size() ? midi.trackNames[(std::size_t) track] : std::string();
            g.name = name.empty() ? "MIDI " + std::to_string (track + 1) : name;
        }
        else
            g.name = "MIDI channel " + std::to_string (midiChannel + 1);
        byKey[key] = out.size();
        out.push_back (std::move (g));
        return out.back();
    };
    for (const auto& p : notes)
        groupFor (p.track, p.midiChannel).notes.push_back (p.note);

    if (o.controller >= 0)
        for (const auto& e : midi.events)
            if ((e.status & 0xF0) == 0xB0 && e.data1 == o.controller && selectedTrack (o, e.track) && selectedChannel (o, e.status & 0x0F))
            {
                // only into groups that have notes (a controller alone makes no channel)
                const auto key = o.perTrack ? e.track : (e.status & 0x0F);
                if (const auto it = byKey.find (key); it != byKey.end())
                {
                    auto& events = out[it->second].controller;
                    const auto value = e.data2 / 127.0f;
                    if (! events.empty() && events.back().tick == e.tick)
                        events.back().value = value; // one event per tick
                    else
                        events.push_back ({ e.tick, value });
                }
            }
    // MIDI channel groups in channel order (FL), track groups in file order
    std::stable_sort (out.begin(), out.end(), [&] (const Group& a, const Group& b) {
        return o.perTrack ? a.track < b.track : a.midiChannel < b.midiChannel;
    });
    return out;
}

namespace
{
std::int64_t realignShift (const std::vector<Group>& gs, bool realign)
{
    if (! realign)
        return 0;
    auto shift = std::numeric_limits<std::int64_t>::max();
    for (const auto& g : gs)
        for (const auto& n : g.notes)
            shift = std::min (shift, n.start);
    return shift == std::numeric_limits<std::int64_t>::max() ? 0 : shift;
}

void addControllerLane (Pattern& pattern, const AutomationTarget& target, const std::vector<PatternEvent>& events, std::int64_t shift)
{
    if (target.kind == AutomationTargetKind::none || events.empty())
        return;
    auto it = std::find_if (pattern.events.begin(), pattern.events.end(), [&] (const PatternEvents& e) { return e.target == target; });
    if (it == pattern.events.end())
    {
        pattern.events.push_back ({ target, {} });
        it = pattern.events.end() - 1;
    }
    for (auto e : events)
    {
        e.tick = std::max<std::int64_t> (0, e.tick - shift);
        if (target.kind == AutomationTargetKind::channelVolume)
            e.value = std::clamp (e.value, 0.0f, 1.0f);
        auto pos = std::lower_bound (it->events.begin(), it->events.end(), e.tick, [] (const PatternEvent& a, std::int64_t t) { return a.tick < t; });
        if (pos != it->events.end() && pos->tick == e.tick)
            pos->value = e.value;
        else
            it->events.insert (pos, e);
    }
}
} // namespace

bool importInto (Song& song, const ParsedMidi& midi, const Options& o, const std::string& patternName, std::string& error)
{
    const auto gs = groups (midi, o);
    if (gs.empty())
    {
        error = "the selection has no notes";
        return false;
    }
    if (o.newProject)
    {
        song = Song();
        song.bpm = midi.bpm;
        song.mixer = Song::defaultMixer();
        song.patterns.clear();
        if (o.timeSignatures && midi.numerator > 0 && midi.denominator == 4)
            song.beatsPerBar = std::clamp (midi.numerator, 1, 16);
    }
    const auto shift = realignShift (gs, o.realign);

    Pattern pattern;
    pattern.name = patternName;
    for (const auto& g : gs)
    {
        ChannelDef c;
        c.name = g.name;
        if (o.midiOut)
        {
            c.instrument = InstrumentKind::midiOut;
            midictl::MidiOutSettings settings;
            settings.channel = g.midiChannel;
            midictl::writeMidiOut (c.pluginState, settings);
        }
        else
        {
            c.instrument = InstrumentKind::synth;
            c.volume = 0.6f;
        }
        c.mixerTrack = o.mixerTracks ? song.claimFreeInsert() : 0;
        song.channels.push_back (c);
        const auto channel = (int) song.channels.size() - 1;
        for (auto n : g.notes)
        {
            n.channel = channel;
            n.start = std::max<std::int64_t> (0, n.start - shift);
            pattern.notes.push_back (n);
        }
        addControllerLane (pattern, controllerTarget (o.controller, channel), g.controller, shift);
    }
    pattern.sortNotes();
    song.patterns.push_back (std::move (pattern));
    song.currentPattern = (int) song.patterns.size() - 1;
    return true;
}

std::vector<int> importIntoChannel (Song& song, int channel, const ParsedMidi& midi, const Options& o, std::string& error)
{
    if (song.patterns.empty() || channel < 0 || channel >= (int) song.channels.size())
    {
        error = "no channel";
        return {};
    }
    auto merged = o;
    merged.perTrack = true;
    const auto gs = groups (midi, merged);
    if (gs.empty())
    {
        error = "the selection has no notes";
        return {};
    }
    const auto shift = realignShift (gs, o.realign);
    auto& pattern = song.patterns[(std::size_t) std::clamp (song.currentPattern, 0, (int) song.patterns.size() - 1)];
    if (! o.blend)
        pattern.notes.erase (std::remove_if (pattern.notes.begin(), pattern.notes.end(), [channel] (const Note& n) { return n.channel == channel; }),
                             pattern.notes.end());
    std::vector<Note> added;
    std::vector<PatternEvent> controller;
    for (const auto& g : gs)
    {
        for (auto n : g.notes)
        {
            n.channel = channel;
            n.start = std::max<std::int64_t> (0, n.start - shift);
            added.push_back (n);
        }
        controller.insert (controller.end(), g.controller.begin(), g.controller.end());
    }
    std::stable_sort (controller.begin(), controller.end(), [] (const PatternEvent& a, const PatternEvent& b) { return a.tick < b.tick; });
    addControllerLane (pattern, controllerTarget (o.controller, channel), controller, shift);
    pattern.notes.insert (pattern.notes.end(), added.begin(), added.end());
    pattern.sortNotes();
    // the imported notes' indices after sorting
    std::vector<int> indices;
    for (int i = 0; i < (int) pattern.notes.size(); ++i)
    {
        const auto& n = pattern.notes[(std::size_t) i];
        for (auto it = added.begin(); it != added.end(); ++it)
            if (it->start == n.start && it->key == n.key && it->length == n.length && n.channel == channel)
            {
                indices.push_back (i);
                added.erase (it);
                break;
            }
    }
    return indices;
}
} // namespace lynx::midiimport
