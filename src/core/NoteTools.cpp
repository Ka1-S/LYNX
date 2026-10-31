#include "core/NoteTools.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <tuple>

namespace lynx::notetools
{
namespace
{
bool valid (const Pattern& p, int i)
{
    return i >= 0 && i < (int) p.notes.size();
}

std::vector<int> validSorted (const Pattern& p, std::vector<int> indices)
{
    indices.erase (std::remove_if (indices.begin(), indices.end(), [&p] (int i) { return ! valid (p, i); }), indices.end());
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    return indices;
}

// small deterministic generator (the same seed gives the same result on every compiler)
struct Random
{
    std::uint32_t state;
    explicit Random (std::uint32_t seed) : state (seed * 2654435761u + 0x6d2b79f5u) {}

    float next() noexcept // 0..1
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (state >> 8) / (float) (1u << 24);
    }

    float bipolar (bool both) noexcept { return both ? next() * 2.0f - 1.0f : -next(); }
};
} // namespace

// --- levels -----------------------------------------------------------------------------------

const char* levelName (Level level)
{
    switch (level)
    {
        case Level::velocity: return TR_NOOP ("Velocity");
        case Level::pan: return TR_NOOP ("Pan");
        case Level::release: return TR_NOOP ("Release");
        case Level::finePitch: return TR_NOOP ("Fine pitch");
        case Level::modX: return TR_NOOP ("Mod X");
        case Level::modY: return TR_NOOP ("Mod Y");
    }
    return "";
}

float levelValue (const Note& n, Level level) noexcept
{
    switch (level)
    {
        case Level::velocity: return n.velocity;
        case Level::pan: return (n.pan + 1.0f) * 0.5f;
        case Level::release: return n.release;
        case Level::finePitch: return (n.finePitch + 120.0f) / 240.0f;
        case Level::modX: return n.modX;
        case Level::modY: return n.modY;
    }
    return 0.0f;
}

void setLevelValue (Note& n, Level level, float v) noexcept
{
    v = std::clamp (v, 0.0f, 1.0f);

    switch (level)
    {
        case Level::velocity: n.velocity = v; break;
        case Level::pan: n.pan = v * 2.0f - 1.0f; break;
        case Level::release: n.release = v; break;
        case Level::finePitch: n.finePitch = v * 240.0f - 120.0f; break;
        case Level::modX: n.modX = v; break;
        case Level::modY: n.modY = v; break;
    }
}

float levelDefault (Level level) noexcept
{
    return levelValue (Note {}, level);
}

bool levelIsBipolar (Level level) noexcept
{
    return level == Level::pan || level == Level::finePitch;
}

// --- scales -----------------------------------------------------------------------------------

const std::vector<ScaleInfo>& scales()
{
    static const std::vector<ScaleInfo> list {
        { TR_NOOP ("No scale"), {} },
        { TR_NOOP ("Major"), { 0, 2, 4, 5, 7, 9, 11 } },
        { TR_NOOP ("Natural minor"), { 0, 2, 3, 5, 7, 8, 10 } },
        { TR_NOOP ("Harmonic minor"), { 0, 2, 3, 5, 7, 8, 11 } },
        { TR_NOOP ("Major pentatonic"), { 0, 2, 4, 7, 9 } },
        { TR_NOOP ("Minor pentatonic"), { 0, 3, 5, 7, 10 } },
        { "Blues", { 0, 3, 5, 6, 7, 10 } },
        { TR_NOOP ("Dorian"), { 0, 2, 3, 5, 7, 9, 10 } },
        { TR_NOOP ("Phrygian"), { 0, 1, 3, 5, 7, 8, 10 } },
    };
    return list;
}

bool inScale (int key, int root, int scale) noexcept
{
    if (scale <= 0 || scale >= (int) scales().size())
        return true;

    const auto& intervals = scales()[(std::size_t) scale].intervals;
    const auto degree = ((key - root) % 12 + 12) % 12;
    return std::find (intervals.begin(), intervals.end(), degree) != intervals.end();
}

KeyRows keyRows (int root, int scale, bool fold) noexcept
{
    KeyRows rows;
    rows.count = 0;
    for (int key = 127; key >= 0; --key)
    {
        const auto shown = ! fold || scale <= 0 || inScale (key, root, scale);
        rows.rowOfKey[(std::size_t) key] = shown ? rows.count : -1;
        if (shown)
            rows.keyOfRow[(std::size_t) rows.count++] = key;
    }
    return rows;
}

int KeyRows::step (int key, int rows) const noexcept
{
    key = std::clamp (key, 0, 127);
    if (count <= 0)
        return key;
    auto row = rowOfKey[(std::size_t) key];
    if (row < 0) // a folded key sits between two rows: one step reaches the shown key above or below it
    {
        int above = key;
        while (above < 127 && rowOfKey[(std::size_t) above] < 0)
            ++above;
        row = rowOfKey[(std::size_t) above] >= 0 ? rowOfKey[(std::size_t) above] : -1; // −1: above the top row
        if (rows > 0)
            --rows;
        else if (rows == 0)
            row = std::max (row, 0);
    }
    return keyOfRow[(std::size_t) std::clamp (row - rows, 0, count - 1)];
}

int snapToScale (int key, int root, int scale) noexcept
{
    key = std::clamp (key, 0, 127);

    if (scale <= 0 || scale >= (int) scales().size())
        return key;

    for (int d = 0; d < 12; ++d)
    {
        if (key - d >= 0 && inScale (key - d, root, scale))
            return key - d;
        if (key + d <= 127 && inScale (key + d, root, scale))
            return key + d;
    }

    return key;
}

void snapNotesToScale (Pattern& pattern, const std::vector<int>& indices, int root, int scale)
{
    for (auto i : validSorted (pattern, indices))
    {
        auto& n = pattern.notes[(std::size_t) i];
        n.key = snapToScale (n.key, root, scale);
    }
}

void moveKeysOnScale (Pattern& pattern, const std::vector<int>& indices, const std::vector<Note>& originals, int deltaKeys, int root, int scale)
{
    if (deltaKeys == 0)
        return;
    for (std::size_t j = 0; j < indices.size() && j < originals.size(); ++j)
    {
        const auto i = indices[j];
        if (i < 0 || i >= (int) pattern.notes.size())
            continue;
        pattern.notes[(std::size_t) i].key = snapToScale (std::clamp (originals[j].key + deltaKeys, 0, 127), root, scale);
    }
}

int stepOnScale (int key, int direction, int root, int scale) noexcept
{
    if (direction == 0)
        return key;
    const auto step = direction > 0 ? 1 : -1;
    for (auto k = key + step; k >= 0 && k <= 127; k += step)
        if (scale <= 0 || inScale (k, root, scale))
            return k;
    return key;
}

// --- chords -----------------------------------------------------------------------------------

std::vector<std::vector<int>> chordsOf (const Pattern& pattern, const std::vector<int>& indices)
{
    std::map<std::tuple<std::int64_t, int>, std::vector<int>> groups; // (start, channel)

    for (auto i : validSorted (pattern, indices))
    {
        const auto& n = pattern.notes[(std::size_t) i];
        groups[{ n.start, n.channel }].push_back (i);
    }

    std::vector<std::vector<int>> out;

    for (auto& [key, group] : groups)
    {
        std::stable_sort (group.begin(), group.end(), [&pattern] (int a, int b) {
            return pattern.notes[(std::size_t) a].key < pattern.notes[(std::size_t) b].key;
        });
        out.push_back (std::move (group));
    }

    return out;
}

// --- strum ------------------------------------------------------------------------------------

void strum (Pattern& pattern, const std::vector<int>& indices, const StrumOptions& o)
{
    const auto chords = chordsOf (pattern, indices);
    int chordNumber = 0;
    const auto exponent = std::pow (2.0, (double) std::clamp (o.curve, -1.0f, 1.0f) * 2.0);

    for (const auto& chord : chords)
    {
        if (chord.size() < 2)
            continue;

        auto downward = o.downward;
        if (o.alternate && (chordNumber % 2) == 1)
            downward = ! downward;
        ++chordNumber;

        const auto count = (int) chord.size();
        const auto total = (double) o.spread * (count - 1);

        for (int k = 0; k < count; ++k)
        {
            auto& n = pattern.notes[(std::size_t) chord[(std::size_t) (downward ? count - 1 - k : k)]];
            const auto x = (double) k / (double) (count - 1);
            const auto offset = std::max<std::int64_t> (0, (std::int64_t) std::llround (total * std::pow (x, exponent)));
            const auto end = n.start + n.length;
            n.start += offset;

            if (o.keepEnds)
                n.length = std::max<std::int64_t> (std::min<std::int64_t> (n.length, 10), end - n.start);

            n.velocity = std::clamp (n.velocity + o.velocityStep * (float) k, 0.0f, 1.0f);
        }
    }
}

// --- arpeggiate -------------------------------------------------------------------------------

std::vector<int> arpeggiate (Pattern& pattern, const std::vector<int>& indices, const ArpeggiateOptions& o)
{
    const auto step = std::max<std::int64_t> (1, o.step);
    const auto octaves = std::clamp (o.octaves, 1, 4);
    const auto noteLength = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) step * std::clamp (o.gate, 0.05f, 1.0f)));
    std::vector<int> doomed;
    std::vector<Note> created;

    for (const auto& chord : chordsOf (pattern, indices))
    {
        if (chord.size() < 2 && octaves == 1)
            continue;

        std::vector<Note> base;
        std::int64_t span = 0;

        for (auto i : chord)
        {
            base.push_back (pattern.notes[(std::size_t) i]);
            span = std::max (span, pattern.notes[(std::size_t) i].length);
            doomed.push_back (i);
        }

        // the ascending run over the octaves
        std::vector<Note> run;
        for (int oct = 0; oct < octaves; ++oct)
            for (auto n : base)
                if (n.key + 12 * oct <= 127)
                {
                    n.key += 12 * oct;
                    run.push_back (n);
                }

        std::vector<Note> sequence;
        auto down = run;
        std::reverse (down.begin(), down.end());

        switch (o.direction)
        {
            case ArpeggiateOptions::down: sequence = down; break;
            case ArpeggiateOptions::upDown:
                sequence = run;
                if (down.size() > 2)
                    sequence.insert (sequence.end(), down.begin() + 1, down.end() - 1);
                break;
            case ArpeggiateOptions::downUp:
                sequence = down;
                if (run.size() > 2)
                    sequence.insert (sequence.end(), run.begin() + 1, run.end() - 1);
                break;
            default: sequence = run; break;
        }

        if (sequence.empty())
            continue;

        const auto start = base.front().start;

        for (std::int64_t t = 0, k = 0; t < span; t += step, ++k)
        {
            auto n = sequence[(std::size_t) (k % (std::int64_t) sequence.size())];
            n.start = start + t;
            n.length = std::min (noteLength, span - t);
            created.push_back (n);
        }
    }

    std::sort (doomed.begin(), doomed.end());
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it)
        pattern.notes.erase (pattern.notes.begin() + *it);

    std::vector<int> added;
    for (const auto& n : created)
    {
        added.push_back ((int) pattern.notes.size());
        pattern.notes.push_back (n);
    }

    return added;
}

// --- flip -------------------------------------------------------------------------------------

void flip (Pattern& pattern, const std::vector<int>& indices, bool horizontal, bool vertical)
{
    const auto targets = validSorted (pattern, indices);

    if (targets.empty())
        return;

    std::int64_t from = INT64_MAX, to = INT64_MIN;
    int low = 127, high = 0;

    for (auto i : targets)
    {
        const auto& n = pattern.notes[(std::size_t) i];
        from = std::min (from, n.start);
        to = std::max (to, n.start + n.length);
        low = std::min (low, n.key);
        high = std::max (high, n.key);
    }

    for (auto i : targets)
    {
        auto& n = pattern.notes[(std::size_t) i];

        if (horizontal)
            n.start = from + (to - (n.start + n.length));
        if (vertical)
            n.key = std::clamp (low + high - n.key, 0, 127);
    }
}

// --- randomise --------------------------------------------------------------------------------

void randomise (Pattern& pattern, const std::vector<int>& indices, const RandomiseOptions& o)
{
    Random random (o.seed);

    for (auto i : validSorted (pattern, indices))
    {
        auto& n = pattern.notes[(std::size_t) i];
        // every note draws the same number of values, so changing one amount doesn't reshuffle the others
        const auto rv = random.bipolar (o.bipolar), rp = random.bipolar (true), rf = random.bipolar (true);
        const auto rx = random.bipolar (true), ry = random.bipolar (true), rt = random.bipolar (o.bipolar);
        const auto rl = random.bipolar (true);

        n.velocity = std::clamp (n.velocity + o.velocity * rv, 0.0f, 1.0f);
        n.pan = std::clamp (n.pan + 2.0f * o.pan * rp, -1.0f, 1.0f);
        n.finePitch = std::clamp (n.finePitch + 120.0f * o.finePitch * rf, -120.0f, 120.0f);
        n.modX = std::clamp (n.modX + o.modX * rx, 0.0f, 1.0f);
        n.modY = std::clamp (n.modY + o.modY * ry, 0.0f, 1.0f);

        if (o.timing > 0)
            n.start = std::max<std::int64_t> (0, n.start + (std::int64_t) std::llround ((double) o.timing * (o.bipolar ? rt : -rt)));

        if (o.length > 0.0f)
            n.length = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) n.length * (1.0 + (double) o.length * rl)));
    }
}

// --- scale levels -----------------------------------------------------------------------------

void scaleLevels (Pattern& pattern, const std::vector<int>& indices, Level level, float scaleStart, float scaleEnd, float offset)
{
    const auto targets = validSorted (pattern, indices);

    if (targets.empty())
        return;

    std::int64_t first = INT64_MAX, last = INT64_MIN;
    for (auto i : targets)
    {
        first = std::min (first, pattern.notes[(std::size_t) i].start);
        last = std::max (last, pattern.notes[(std::size_t) i].start);
    }

    const auto centre = levelIsBipolar (level) ? 0.5f : 0.0f;

    for (auto i : targets)
    {
        auto& n = pattern.notes[(std::size_t) i];
        const auto t = last > first ? (float) (n.start - first) / (float) (last - first) : 0.0f;
        const auto scale = scaleStart + (scaleEnd - scaleStart) * t;
        setLevelValue (n, level, centre + (levelValue (n, level) - centre) * scale + offset);
    }
}

// --- limit ------------------------------------------------------------------------------------

void limit (Pattern& pattern, const std::vector<int>& indices, const LimitOptions& o)
{
    const auto low = std::clamp (std::min (o.lowKey, o.highKey), 0, 127);
    const auto high = std::clamp (std::max (o.lowKey, o.highKey), 0, 127);
    const auto minV = std::clamp (std::min (o.minVelocity, o.maxVelocity), 0.0f, 1.0f);
    const auto maxV = std::clamp (std::max (o.minVelocity, o.maxVelocity), 0.0f, 1.0f);

    for (auto i : validSorted (pattern, indices))
    {
        auto& n = pattern.notes[(std::size_t) i];

        if (high - low >= 11) // an octave fits: keep the pitch class
        {
            while (n.key < low)
                n.key += 12;
            while (n.key > high)
                n.key -= 12;
        }

        n.key = std::clamp (n.key, low, high);
        n.velocity = std::clamp (n.velocity, minV, maxV);
    }
}

// --- LYNX's script-like tools (REA round 143) --------------------------------------------------------

bool euclideanHit (int steps, int pulses, int rotation, int step) noexcept
{
    if (steps <= 0 || pulses <= 0)
        return false;
    pulses = std::min (pulses, steps);
    const auto s = (((step + rotation) % steps) + steps) % steps;
    // Bresenham form of the Euclidean (Bjorklund) rhythm: a hit where the running multiple wraps
    return (s * pulses) % steps < pulses;
}

std::vector<int> euclidean (Pattern& pattern, const std::vector<int>& replace, const EuclideanOptions& o)
{
    auto doomed = validSorted (pattern, replace);
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it)
        pattern.notes.erase (pattern.notes.begin() + *it);

    const auto stepTicks = std::max<std::int64_t> (1, o.stepTicks);
    const auto count = (int) (std::max<std::int64_t> (0, o.length) / stepTicks);
    std::vector<Note> added;
    bool first = true;
    for (int step = 0; step < count; ++step)
    {
        if (step % std::max (1, o.steps) == 0)
            first = true;
        if (! euclideanHit (o.steps, o.pulses, o.rotation, step))
            continue;
        Note n;
        n.start = o.start + step * stepTicks;
        n.length = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) stepTicks * std::clamp (o.gate, 0.05f, 1.0f)));
        n.channel = o.channel;
        n.key = std::clamp (o.key, 0, 127);
        n.velocity = std::clamp (o.velocity + (first ? o.accent : 0.0f), 0.0f, 1.0f);
        first = false;
        added.push_back (n);
    }

    const auto base = (int) pattern.notes.size();
    pattern.notes.insert (pattern.notes.end(), added.begin(), added.end());
    std::vector<int> out;
    for (int k = 0; k < (int) added.size(); ++k)
        out.push_back (base + k);
    return out;
}

void humanize (Pattern& pattern, const std::vector<int>& indices, const HumanizeOptions& o)
{
    Random random (o.seed);
    std::map<std::int64_t, std::int64_t> chordShift; // notes starting together move together
    for (auto i : validSorted (pattern, indices))
    {
        auto& n = pattern.notes[(std::size_t) i];
        // a softer, triangular spread (two draws) instead of a flat one: most notes move a little
        const auto rt = (random.bipolar (true) + random.bipolar (true)) * 0.5f;
        const auto rv = (random.bipolar (true) + random.bipolar (true)) * 0.5f;
        const auto rl = random.bipolar (true);
        if (o.timing > 0)
        {
            auto found = chordShift.find (n.start);
            const auto shift = found != chordShift.end() ? found->second : (std::int64_t) std::llround ((double) o.timing * rt);
            chordShift.emplace (n.start, shift);
            n.start = std::max<std::int64_t> (0, n.start + shift);
        }
        auto v = n.velocity + o.velocity * rv;
        if (o.softerOffbeats && (n.start % ppq) != 0)
            v -= o.velocity * 0.5f;
        n.velocity = std::clamp (v, 0.0f, 1.0f);
        if (o.length > 0.0f)
            n.length = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) n.length * (1.0 + (double) o.length * rl)));
    }
}

std::vector<int> repeatNotes (Pattern& pattern, const std::vector<int>& indices, const RepeatOptions& o)
{
    const auto rate = std::max<std::int64_t> (1, o.rate);
    const auto targets = validSorted (pattern, indices);
    std::vector<Note> added;
    for (auto i : targets)
    {
        auto& n = pattern.notes[(std::size_t) i];
        const auto end = n.start + n.length;
        const auto gate = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) rate * std::clamp (o.gate, 0.05f, 1.0f)));
        auto velocity = n.velocity;
        int k = 1;
        for (auto t = n.start + rate; t < end; t += rate, ++k)
        {
            velocity *= std::clamp (o.decay, 0.0f, 1.5f);
            auto copy = n;
            copy.start = t;
            copy.length = std::min (gate, end - t);
            copy.velocity = std::clamp (velocity, 0.0f, 1.0f);
            copy.key = std::clamp (n.key + k * o.pitchStep, 0, 127);
            added.push_back (copy);
        }
        n.length = std::min (gate, n.length);
    }
    std::vector<int> out = targets;
    const auto base = (int) pattern.notes.size();
    pattern.notes.insert (pattern.notes.end(), added.begin(), added.end());
    for (int k = 0; k < (int) added.size(); ++k)
        out.push_back (base + k);
    return out;
}

std::vector<int> selectByVelocity (const Pattern& pattern, const std::vector<int>& indices, float low, float high)
{
    std::vector<int> out;
    for (auto i : validSorted (pattern, indices))
        if (pattern.notes[(std::size_t) i].velocity >= low - 1.0e-6f && pattern.notes[(std::size_t) i].velocity <= high + 1.0e-6f)
            out.push_back (i);
    return out;
}

std::vector<int> selectLoudest (const Pattern& pattern, const std::vector<int>& indices, float fraction, bool loudest)
{
    auto list = validSorted (pattern, indices);
    if (list.empty())
        return list;
    std::stable_sort (list.begin(), list.end(), [&pattern, loudest] (int a, int b) {
        const auto va = pattern.notes[(std::size_t) a].velocity, vb = pattern.notes[(std::size_t) b].velocity;
        return loudest ? va > vb : va < vb;
    });
    const auto keep = std::max<std::size_t> (1, (std::size_t) std::llround ((double) list.size() * std::clamp (fraction, 0.0f, 1.0f)));
    list.resize (std::min (keep, list.size()));
    std::sort (list.begin(), list.end());
    return list;
}

// --- selection helpers ------------------------------------------------------------------------

std::vector<int> invertSelection (const std::vector<int>& all, const std::vector<int>& selected)
{
    auto a = all, b = selected;
    std::sort (a.begin(), a.end());
    a.erase (std::unique (a.begin(), a.end()), a.end());
    std::sort (b.begin(), b.end());
    std::vector<int> out;
    std::set_difference (a.begin(), a.end(), b.begin(), b.end(), std::back_inserter (out));
    return out;
}

std::vector<int> selectByColour (const Pattern& pattern, const std::vector<int>& indices, int colour)
{
    std::vector<int> out;
    for (auto i : validSorted (pattern, indices))
        if (pattern.notes[(std::size_t) i].colour == colour)
            out.push_back (i);
    return out;
}

namespace
{
std::vector<int> inTimeOrder (const Pattern& pattern, const std::vector<int>& indices)
{
    auto list = validSorted (pattern, indices);
    std::stable_sort (list.begin(), list.end(), [&pattern] (int a, int b) {
        const auto& na = pattern.notes[(std::size_t) a];
        const auto& nb = pattern.notes[(std::size_t) b];
        return std::tie (na.start, na.key) < std::tie (nb.start, nb.key);
    });
    return list;
}
} // namespace

std::vector<int> selectOdd (const Pattern& pattern, const std::vector<int>& indices)
{
    const auto list = inTimeOrder (pattern, indices);
    std::vector<int> out;
    for (std::size_t k = 0; k < list.size(); k += 2)
        out.push_back (list[k]);
    std::sort (out.begin(), out.end());
    return out;
}

std::vector<int> selectRandom (const Pattern& pattern, const std::vector<int>& indices, float fraction, std::uint32_t seed)
{
    Random random (seed);
    const auto f = std::clamp (fraction, 0.0f, 1.0f);
    std::vector<int> out;
    for (auto i : validSorted (pattern, indices))
        if (random.next() < f)
            out.push_back (i);
    return out;
}

std::vector<int> selectMuted (const Pattern& pattern, const std::vector<int>& indices)
{
    std::vector<int> out;
    for (auto i : validSorted (pattern, indices))
        if (pattern.notes[(std::size_t) i].muted)
            out.push_back (i);
    return out;
}

std::vector<int> selectOverlapping (const Pattern& pattern, const std::vector<int>& indices)
{
    // per key, in time order: a note starting before the furthest end so far overlaps an earlier one
    const auto list = inTimeOrder (pattern, indices);
    std::map<int, std::int64_t> reach; // key -> furthest end of the notes seen
    std::vector<int> out;
    for (auto i : list)
    {
        const auto& n = pattern.notes[(std::size_t) i];
        const auto it = reach.find (n.key);
        if (it != reach.end() && n.start < it->second)
        {
            out.push_back (i);
            it->second = std::max (it->second, (n.start + n.length));
        }
        else
            reach[n.key] = (n.start + n.length);
    }
    std::sort (out.begin(), out.end());
    return out;
}

std::vector<int> selectStacked (const Pattern& pattern, const std::vector<int>& indices)
{
    const auto list = inTimeOrder (pattern, indices);
    std::vector<int> out;
    for (std::size_t a = 0; a < list.size(); ++a)
    {
        const auto& n = pattern.notes[(std::size_t) list[a]];
        for (std::size_t b = 0; b < list.size(); ++b)
        {
            if (a == b)
                continue;
            const auto& o = pattern.notes[(std::size_t) list[b]];
            if (o.key != n.key || o.start > n.start || (o.start + o.length) < (n.start + n.length))
                continue;
            // inside `o`; exact duplicates: only the later ones (in time order) count as stacked
            const auto same = o.start == n.start && (o.start + o.length) == (n.start + n.length);
            if (! same || b < a)
            {
                out.push_back (list[a]);
                break;
            }
        }
    }
    std::sort (out.begin(), out.end());
    return out;
}

namespace
{
int nextGroup (const Pattern& pattern)
{
    auto top = 0;
    for (const auto& n : pattern.notes)
        top = std::max (top, n.group);
    return top + 1;
}
} // namespace

void groupNotes (Pattern& pattern, const std::vector<int>& indices)
{
    const auto list = validSorted (pattern, indices);
    if (list.size() < 2)
        return;
    const auto id = nextGroup (pattern);
    for (auto i : list)
        pattern.notes[(std::size_t) i].group = id;
}

void ungroupNotes (Pattern& pattern, const std::vector<int>& indices)
{
    for (auto i : validSorted (pattern, indices))
        pattern.notes[(std::size_t) i].group = 0;
}

std::vector<int> groupOf (const Pattern& pattern, int index)
{
    if (! valid (pattern, index))
        return {};
    const auto& n = pattern.notes[(std::size_t) index];
    if (n.group == 0)
        return { index };
    std::vector<int> out;
    for (int i = 0; i < (int) pattern.notes.size(); ++i)
        if (pattern.notes[(std::size_t) i].group == n.group && pattern.notes[(std::size_t) i].channel == n.channel)
            out.push_back (i);
    return out;
}

void regroup (Pattern& pattern, const std::vector<int>& indices)
{
    std::map<int, int> renamed;
    auto next = nextGroup (pattern);
    for (auto i : validSorted (pattern, indices))
    {
        auto& g = pattern.notes[(std::size_t) i].group;
        if (g == 0)
            continue;
        const auto old = g;
        if (renamed.find (old) == renamed.end())
            renamed[old] = next++;
        g = renamed[old];
    }
}

const char* generalMidiDrumName (int key) noexcept
{
    // the General MIDI 1 percussion key map (a public standard)
    static const char* names[] = { "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare", "Hand Clap", "Electric Snare",
                                   "Low Floor Tom", "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat", "Low Tom", "Open Hi-Hat",
                                   "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1", "High Tom", "Ride Cymbal 1", "Chinese Cymbal",
                                   "Ride Bell", "Tambourine", "Splash Cymbal", "Cowbell", "Crash Cymbal 2", "Vibraslap",
                                   "Ride Cymbal 2", "Hi Bongo", "Low Bongo", "Mute Hi Conga", "Open Hi Conga", "Low Conga",
                                   "High Timbale", "Low Timbale", "High Agogo", "Low Agogo", "Cabasa", "Maracas",
                                   "Short Whistle", "Long Whistle", "Short Guiro", "Long Guiro", "Claves", "Hi Wood Block",
                                   "Low Wood Block", "Mute Cuica", "Open Cuica", "Mute Triangle", "Open Triangle" };
    return key >= 35 && key <= 81 ? names[key - 35] : nullptr;
}

void setColour (Pattern& pattern, const std::vector<int>& indices, int colour)
{
    const auto c = std::clamp (colour, 0, noteColourCount - 1);
    for (auto i : validSorted (pattern, indices))
        pattern.notes[(std::size_t) i].colour = c;
}
} // namespace lynx::notetools
