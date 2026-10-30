#include "core/ChannelBurn.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/Arpeggiator.h"
#include "core/ChannelLoops.h"
#include "core/ChannelMiscFx.h"
#include "core/RackOps.h"
#include "core/Scheduler.h"

namespace lynx::channelburn
{
namespace
{
bool validPattern (const Song& song, int pattern) noexcept { return pattern >= 0 && pattern < (int) song.patterns.size(); }
bool validChannel (const Song& song, int channel) noexcept { return channel >= 0 && channel < (int) song.channels.size(); }

NoteEvent eventOf (const Note& n, NoteEvent::Type type, int channel)
{
    NoteEvent e;
    e.type = type;
    e.channel = channel;
    e.key = n.key;
    e.velocity = type == NoteEvent::Type::off ? 0.0f : n.velocity;
    e.pan = n.pan;
    e.cents = n.finePitch;
    e.porta = n.porta;
    e.modX = n.modX;
    e.modY = n.modY;
    e.release = n.release;
    e.colour = (std::int8_t) (n.colour & 15);
    return e;
}

struct Timed
{
    std::int64_t tick = 0;
    NoteEvent event;
};

// a sounding note built from the output events
struct Open
{
    Note note;
    int key = 0; // the event key (before the cents fold into it)
};

Note noteFrom (const NoteEvent& e, std::int64_t tick, float gain)
{
    Note n;
    n.start = tick;
    n.channel = e.channel;
    const auto cents = (double) e.cents;
    const auto semis = (int) std::lround (cents / 100.0);
    n.key = std::clamp (e.key + semis, 0, 127);
    n.finePitch = (float) std::clamp (cents - 100.0 * semis, -120.0, 120.0);
    n.velocity = std::clamp (e.velocity * gain, 0.0f, 1.0f);
    n.pan = std::clamp (e.pan, -1.0f, 1.0f);
    n.porta = e.porta;
    n.modX = e.modX;
    n.modY = e.modY;
    n.release = e.release;
    n.colour = e.colour & 15;
    return n;
}
} // namespace

Baked whatBurns (const Song& song, int pattern, int channel)
{
    Baked b;
    if (! validPattern (song, pattern) || ! validChannel (song, channel))
        return b;
    const auto& c = song.channels[(std::size_t) channel];
    const auto& p = song.patterns[(std::size_t) pattern];
    b.arpeggiator = c.arp.enabled();
    b.echo = c.misc.echo.enabled();
    b.swing = song.swing > 0.0f && c.swingMix > 0.0f;
    b.time = c.misc.shiftBeats > 0.0f || c.misc.gateBeats > 0.0f;
    const auto length = p.effectiveLength (song.beatsPerBar);
    const auto loop = channelloops::loopLength (p, channel, channelloops::effectiveSetting (p, channel), length, song.beatsPerBar);
    b.loop = loop > 0 && loop < length;
    return b;
}

std::vector<Note> playedNotes (const Song& song, int patternIndex, int channel)
{
    std::vector<Note> out;
    if (! validPattern (song, patternIndex) || ! validChannel (song, channel))
        return out;
    const auto& c = song.channels[(std::size_t) channel];
    auto pattern = song.patterns[(std::size_t) patternIndex];
    const auto patternLength = pattern.effectiveLength (song.beatsPerBar);
    channelloops::expandPattern (pattern, song.beatsPerBar); // the loop repeats, as the engine plays them

    std::vector<Note> source;
    for (const auto& n : pattern.notes)
        if (n.channel == channel && ! n.muted && n.start < patternLength)
            source.push_back (n);
    if (source.empty())
        return out;

    const auto arp = c.arp.enabled();
    const auto echo = c.misc.echo.enabled();

    if (! arp && ! echo)
    {
        // only the timing: swing, shift, gate
        for (auto n : source)
        {
            const auto t = playedTiming (n, song.swing, &c);
            n.start = t.start;
            n.length = std::max<std::int64_t> (1, t.length);
            if (n.start < patternLength)
                out.push_back (n);
        }
        std::stable_sort (out.begin(), out.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
        return out;
    }

    // the events the scheduler would send (slides stay as they are: the arpeggiator / echo don't take them)
    std::vector<Timed> events;
    std::int64_t lastEvent = 0;
    for (const auto& n : source)
    {
        const auto t = playedTiming (n, song.swing, &c);
        if (n.slide)
        {
            auto copy = n;
            copy.start = t.start;
            copy.length = std::max<std::int64_t> (1, t.length);
            out.push_back (copy);
            continue;
        }
        events.push_back ({ t.start, eventOf (n, NoteEvent::Type::on, channel) });
        events.push_back ({ t.start + std::max<std::int64_t> (1, t.length), eventOf (n, NoteEvent::Type::off, channel) });
        lastEvent = std::max (lastEvent, t.start + std::max<std::int64_t> (1, t.length));
    }
    std::stable_sort (events.begin(), events.end(), [] (const Timed& a, const Timed& b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.event.type != NoteEvent::Type::on && b.event.type == NoteEvent::Type::on; // "off" first, as the scheduler
    });

    // the arpeggiator and echo run as on the audio thread, one sample per tick
    auto arpeggiator = std::make_unique<Arpeggiator>();
    auto noteEcho = std::make_unique<channelmisc::NoteEcho>();
    constexpr int block = ticksPerStep * 2;
    const auto echoTail = echo ? (std::int64_t) std::ceil ((double) c.misc.echo.timeBeats * ppq * (std::clamp (c.misc.echo.echoes, 0, 16) + 1)) : 0;
    const auto arpTail = arp ? (std::int64_t) std::ceil ((double) c.arp.stepBeats * ppq * std::max (1.0f, c.arp.gate) * 2.0) : 0;
    const auto end = std::max (lastEvent, patternLength) + echoTail + arpTail + block;

    std::vector<Open> open;
    auto emit = [&] (const NoteEvent& e, std::int64_t tick, float gain) {
        if (e.channel != channel)
            return;
        if (e.type == NoteEvent::Type::on)
        {
            open.push_back ({ noteFrom (e, tick, gain), e.key });
            return;
        }
        if (e.type == NoteEvent::Type::slide)
        {
            auto n = noteFrom (e, tick, gain);
            n.slide = true;
            n.length = std::max<std::int64_t> (1, (std::int64_t) std::lround (e.slideSamples));
            out.push_back (n);
            return;
        }
        if (e.type == NoteEvent::Type::off || e.type == NoteEvent::Type::choke)
        {
            // the earliest sounding note of that key ends (a choke ends every one)
            for (auto it = open.begin(); it != open.end();)
            {
                if (e.type == NoteEvent::Type::choke || it->key == e.key)
                {
                    it->note.length = std::max<std::int64_t> (1, tick - it->note.start);
                    out.push_back (it->note);
                    it = open.erase (it);
                    if (e.type != NoteEvent::Type::choke)
                        break;
                    continue;
                }
                ++it;
            }
        }
    };

    std::size_t next = 0;
    std::vector<NoteEvent> in;
    for (std::int64_t t0 = 0; t0 < end; t0 += block)
    {
        in.clear();
        while (next < events.size() && events[next].tick < t0 + block)
        {
            auto e = events[next].event;
            e.sampleOffset = (int) std::max<std::int64_t> (0, events[next].tick - t0);
            in.push_back (e);
            ++next;
        }
        const NoteEvent* list = in.data();
        int count = (int) std::min<std::size_t> (in.size(), (std::size_t) Arpeggiator::maxEvents);
        if (arp)
        {
            int outCount = 0;
            std::copy (list, list + count, arpeggiator->input.begin());
            arpeggiator->process (channel, arpeggiator->input.data(), count, (double) t0, 1.0, block, c.arp, arpeggiator->output.data(), outCount);
            list = arpeggiator->output.data();
            count = outCount;
        }
        const float* gains = nullptr;
        if (echo)
        {
            int outCount = 0;
            const auto n = std::min (count, channelmisc::NoteEcho::maxEvents);
            std::copy (list, list + n, noteEcho->input.begin());
            noteEcho->process (channel, noteEcho->input.data(), n, (double) t0, 1.0, block, c.misc.echo, noteEcho->output.data(),
                               noteEcho->outputGains.data(), outCount);
            list = noteEcho->output.data();
            count = outCount;
            gains = noteEcho->outputGains.data();
        }
        for (int k = 0; k < count; ++k)
            emit (list[k], t0 + list[k].sampleOffset, gains != nullptr ? gains[k] : 1.0f);
    }
    for (auto& o : open) // still sounding at the end
    {
        o.note.length = std::max<std::int64_t> (1, end - o.note.start);
        out.push_back (o.note);
    }

    out.erase (std::remove_if (out.begin(), out.end(), [patternLength] (const Note& n) { return n.start >= patternLength || n.start < 0; }), out.end());
    std::stable_sort (out.begin(), out.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    return out;
}

bool burnMidi (Song& song, int channel, bool toNewPattern, const Reset& reset)
{
    const auto current = song.currentPattern;
    if (! validPattern (song, current) || ! validChannel (song, channel))
        return false;
    auto notes = playedNotes (song, current, channel);
    if (notes.empty())
        return false;
    const auto baked = whatBurns (song, current, channel);
    const auto sourceLength = song.patterns[(std::size_t) current].effectiveLength (song.beatsPerBar);

    if (toNewPattern)
    {
        const auto& c = song.channels[(std::size_t) channel];
        const auto index = insertPattern (song, current + 1, song.patterns[(std::size_t) current].name + " - " + c.name);
        auto& p = song.patterns[(std::size_t) index];
        p.notes = std::move (notes);
        p.lengthTicks = sourceLength;
        p.sortNotes();
    }
    else
    {
        auto& p = song.patterns[(std::size_t) current];
        const auto autoLength = p.lengthTicks <= 0;
        p.notes.erase (std::remove_if (p.notes.begin(), p.notes.end(), [channel] (const Note& n) { return n.channel == channel; }), p.notes.end());
        p.notes.insert (p.notes.end(), notes.begin(), notes.end());
        p.sortNotes();
        // the channel's loop is written out: its loop point becomes "Don't loop" (FL: FUN_012cf150)
        if (baked.loop || channelloops::hasStoredSetting (p, channel))
            channelloops::setSetting (p, channel, ChannelLoop::dontLoop);
        if (autoLength && p.effectiveLength (song.beatsPerBar) != sourceLength)
            p.lengthTicks = sourceLength; // (a long echo never makes the pattern longer)
    }

    auto& c = song.channels[(std::size_t) channel];
    if (reset.arpeggiator && baked.arpeggiator)
        c.arp.direction = 0;
    if (reset.echo && baked.echo)
        c.misc.echo.feedback = 0.0f;
    if (reset.swing && baked.swing)
        c.swingMix = 0.0f;
    if (reset.time && baked.time)
        c.misc.shiftBeats = c.misc.gateBeats = 0.0f;
    return true;
}
} // namespace lynx::channelburn
