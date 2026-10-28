#include "core/ScoreLog.h"

#include <algorithm>
#include <cmath>

namespace lynx::scorelog
{
Log::Log (std::size_t capacity) : ring (std::max<std::size_t> (1, capacity)) {}

void Log::noteOn (double ms, int channel, int key, float velocity) noexcept
{
    ring[head] = { ms, -1.0, channel, key, velocity };
    head = (head + 1) % ring.size();
    count = std::min (count + 1, ring.size());
}

void Log::noteOff (double ms, int channel, int key) noexcept
{
    // the newest open note of that key: a short search back (a key is rarely held across thousands of notes)
    const auto limit = std::min<std::size_t> (count, 4096);
    for (std::size_t i = 1; i <= limit; ++i)
    {
        auto& e = ring[(head + ring.size() - i) % ring.size()];
        if (e.endMs < 0.0 && e.channel == channel && e.key == key)
        {
            e.endMs = std::max (ms, e.startMs);
            return;
        }
    }
}

void Log::clear() noexcept
{
    head = count = 0;
}

std::vector<Entry> Log::snapshot() const
{
    std::vector<Entry> out;
    out.reserve (count);
    for (std::size_t i = 0; i < count; ++i)
        out.push_back (ring[(head + ring.size() - count + i) % ring.size()]);
    return out;
}

std::vector<Note> dump (const std::vector<Entry>& log, const DumpOptions& options)
{
    const auto ticksPerMs = std::max (1.0, options.bpm) * ppq / 60000.0;
    auto toTicks = [ticksPerMs] (double ms) { return (std::int64_t) std::llround (ms * ticksPerMs); };

    std::vector<Note> notes;
    std::int64_t lastEnd = 0;
    for (const auto& e : log)
    {
        if (e.endMs < 0.0) // still held: FL leaves it out
            continue;
        Note n;
        n.start = toTicks (e.startMs);
        n.length = std::max (options.minLength, toTicks (e.endMs) - n.start);
        n.channel = e.channel;
        n.key = std::clamp (e.key, 0, 127);
        n.velocity = std::clamp (e.velocity, 0.0f, 1.0f);
        lastEnd = notes.empty() ? n.start + n.length : std::max (lastEnd, n.start + n.length);
        notes.push_back (n);
    }

    // the window reaches back from the end of the last note
    const auto window = toTicks (std::max (0.0, options.minutes) * 60000.0);
    notes.erase (std::remove_if (notes.begin(), notes.end(), [&] (const Note& n) { return n.start < lastEnd - window; }), notes.end());
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });

    // the first note at the pattern start, silences longer than two bars shortened to two bars
    const auto maxGap = 2 * (std::int64_t) std::max (1, options.beatsPerBar) * ppq;
    std::int64_t shift = 0, coveredUntil = 0;
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        auto& n = notes[i];
        const auto start = n.start, end = n.start + n.length; // before shifting
        if (i == 0)
            shift = -start;
        else if (start > coveredUntil + maxGap)
            shift += coveredUntil + maxGap - start;
        coveredUntil = i == 0 ? end : std::max (coveredUntil, end);
        n.start += shift;
    }
    return notes;
}
} // namespace lynx::scorelog
