#pragma once

// FL Studio's score logger (REA round 165f, docs/analise/REA_rodada165f.md §3.1): every note played on a MIDI input
// or the typing keyboard is remembered, recording or not, so "Tools › Dump score log to selected pattern" can turn
// the last minutes of a jam into a pattern. Times are wall-clock milliseconds; the dump converts them to ticks at the
// project tempo, as FL does.

#include "core/Song.h"

#include <cstddef>
#include <vector>

namespace lynx::scorelog
{
struct Entry
{
    double startMs = 0.0;
    double endMs = -1.0; // < 0 while the key is still held
    int channel = 0;
    int key = 60;
    float velocity = 0.78f;
};

// A fixed-capacity ring of played notes (no allocation after construction). Not thread-safe: the owner locks.
class Log
{
public:
    explicit Log (std::size_t capacity = 65536);

    void noteOn (double ms, int channel, int key, float velocity) noexcept;
    // Closes the most recent open note of (channel, key); a release without a note-on is ignored.
    void noteOff (double ms, int channel, int key) noexcept;
    void clear() noexcept;

    std::size_t size() const noexcept { return count; }
    std::size_t capacity() const noexcept { return ring.size(); }
    // Oldest first.
    std::vector<Entry> snapshot() const;

private:
    std::vector<Entry> ring;
    std::size_t head = 0, count = 0; // head = the next slot written
};

struct DumpOptions
{
    double minutes = 5.0;       // FL's menu: the last 2, 5, 10, 20 or 30 minutes
    double bpm = 130.0;         // milliseconds become ticks at this tempo
    int beatsPerBar = 4;        // a silence longer than two bars is shortened to two bars
    std::int64_t minLength = ticksPerStep / 6; // FL's shortest note: a sixth of a step
};

// The notes of the log as pattern notes, FL's way [DECOMP FLEngine FUN_0124c060]: notes still held are left out;
// start and end are converted to ticks (rounded); the window reaches back from the end of the last note; the first
// note starts the pattern (tick 0) and every silence longer than two bars is shortened to two bars.
std::vector<Note> dump (const std::vector<Entry>& log, const DumpOptions& options);
} // namespace lynx::scorelog
