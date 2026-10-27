// VFX Sequencer (round 164b, docs/analise/REA_rodada164b.md §Sequencer): FL's port of BlueARP — a bank of step
// programs that turn held notes into arpeggios and sequences. The state is FL's (so FL's bank and projects load and
// write back as they are); the engine follows the behaviour measured in FL's plugin (REA, clean-room: our own code).

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Vfx.h"
#include "core/VfxDetail.h"

namespace lynx::vfx
{
using detail::Handoff;
using detail::put;
using detail::Reader;
using S = SequencerSettings;

namespace
{
// ---------------------------------------------------------------------------------------------------------------
// Parameters

struct ParamInfo
{
    int min, max, def;
    bool perProgram;
};

// FL param 39 + i (the program-level block)
constexpr ParamInfo programParams[43] = {
    { 0, 127, 17, false },  // 0 PatchVerProg
    { 0, 64, 16, true },    // 1 Steps
    { 1, 125, 90, true },   // 2 Gate %
    { 0, 12, 4, true },     // 3 Sync
    { 0, 1, 0, false },     // 4 Latch
    { 0, 4, 0, true },      // 5 Missing key substitution
    { 0, 127, 0, false },   // 6 Input range low
    { 0, 127, 127, false }, // 7 Input range high
    { 0, 127, 0, false },   // 8 Output wrap low
    { 0, 127, 127, false }, // 9 Output wrap high
    { 0, 7, 0, true },      // 10 Order algorithm
    { 0, 127, 64, true },   // 11 Fixed key
    { -3, 3, 0, true },     // 12 Transpose octaves
    { 0, 8, 1, false },     // 13 Input quantize
    { -12, 12, 0, false },  // 14 Transpose semitones
    { 0, 1, 0, false },     // 15 Page scroll
    { -50, 50, 0, true },   // 16 Swing
    { 0, 3, 0, false },     // 17 Restart on
    { 0, 13, 0, true },     // 18 Force scale: key
    { 0, 14, 0, true },     // 19 Force scale: scale
    { 0, 1, 0, true },      // 20 Force scale: mode
    { 0, 1, 0, false },     // 21 Page lock
    { 0, 3, 0, false },     // 22 Page selected
    { -8, 8, 0, false },    // 23 Pattern shift
    { 0, 127, 0, false },   // 24 Input wrap low
    { 0, 127, 127, false }, // 25 Input wrap high
    { 0, 1, 0, true },      // 26 Polyphonic octave
    { 0, 1, 0, true },      // 27 Polyphonic key select
    { 0, 2, 0, true },      // 28 Missing key octave
    { 0, 4, 0, false },     // 29 Selected layer
    { 0, 1, 0, true },      // 30 Gate lane mode
    { 0, 100, 0, true },    // 31 Randomize velocity
    { 0, 100, 0, true },    // 32 Randomize gate
    { 0, 100, 0, true },    // 33 Randomize start
    { 0, 1, 0, false },     // 34 Latch mode
    { 0, 1, 1, false },     // 35 Filter: pitch bend
    { 0, 1, 1, false },     // 36 Filter: mod wheel
    { 0, 1, 1, false },     // 37 Filter: aftertouch
    { 0, 1, 0, false },     // 38 Filter: other CC
    { 0, 3, 3, false },     // 39 Filter: sustain
    { 0, 1, 0, false },     // 40 Sustain polarity
    { -7, 7, 0, true },     // 41 Smart bend
    { 0, 1, 0, true },      // 42 Program mode
};

// FL params 0…19
constexpr ParamInfo globalParams[20] = {
    { 0, 127, 17, false }, { -1, 15, -1, false }, { 0, 15, 0, false }, { 0, 5, 0, false },  { 0, 13, 0, false },
    { 0, 6, 1, false },    { 0, 0, 0, false },    { 0, 120, 120, false }, { 0, 127, 3, false }, { 0, 5, 3, false },
    { 1, 16, 8, false },   { -1, 15, -1, false }, { 0, 8, 5, false },  { 0, 1, 1, false },   { 0, 3, 0, false },
    { 0, 2, 0, false },    { 0, 1, 0, false },    { 0, 2, 0, false },  { 0, 2, 0, false },   { 0, 1, 0, false },
};

const ParamInfo* info (int param) noexcept
{
    if (param >= 0 && param < 20)
        return &globalParams[param];
    if (param >= 39 && param < 39 + 43)
        return &programParams[param - 39];
    return nullptr;
}

// step length in beats per Sync index (1/64 … 3/8)
constexpr double syncBeats[13] = { 0.0625, 1.0 / 12.0, 0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0, 0.1875, 0.375, 0.75, 1.5 };
// input quantize grid in beats (index 2 "1/12" is 0.75 in FL's plugin; kept)
constexpr double quantizeBeats[9] = { 0.0, 0.25, 0.75, 0.5, 2.0 / 3.0, 1.0, 2.0, 4.0, 8.0 };

std::vector<std::uint8_t> chunk (const char* tag, const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> out (tag, tag + 4);
    put<std::uint32_t> (out, (std::uint32_t) data.size());
    out.insert (out.end(), data.begin(), data.end());
    return out;
}

template <typename T>
std::vector<std::uint8_t> bytes (const T& c)
{
    std::vector<std::uint8_t> out (c.size());
    std::memcpy (out.data(), c.data(), c.size());
    return out;
}

std::array<std::uint8_t, 48> nameBytes (const std::string& s)
{
    std::array<std::uint8_t, 48> out {};
    std::memcpy (out.data(), s.data(), std::min<std::size_t> (s.size(), 47));
    return out;
}

void appendChunk (std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& c) { out.insert (out.end(), c.begin(), c.end()); }
} // namespace

std::string S::Program::nameText() const
{
    std::string s;
    for (const auto c : name)
    {
        if (c == 0)
            break;
        s += (char) c;
    }
    return s;
}

void S::Program::setName (const std::string& n) { name = nameBytes (n); }

bool S::perProgram (int param) noexcept
{
    const auto* i = info (param);
    return i != nullptr && i->perProgram;
}

int S::minimum (int param) noexcept
{
    if (param >= 0x80 && param < 0x200)
    {
        static constexpr int lo[6] = { -12, 0, 0, 0, 0, -3 };
        return lo[(param - 0x80) / 64];
    }
    const auto* i = info (param);
    return i != nullptr ? i->min : 0;
}

int S::maximum (int param) noexcept
{
    if (param >= 0x80 && param < 0x200)
    {
        static constexpr int hi[6] = { 12, 127, 5, 127, 127, 16 };
        return hi[(param - 0x80) / 64];
    }
    const auto* i = info (param);
    return i != nullptr ? i->max : 0;
}

int S::program() const noexcept
{
    const auto p = fixp.size() > 8 ? (int) fixp[8] : 0;
    return programs.empty() ? 0 : std::clamp (p, 0, (int) programs.size() - 1);
}

S::Program& S::current() { return programs[(std::size_t) program()]; }
const S::Program& S::current() const { return programs[(std::size_t) program()]; }

int S::get (int param) const noexcept
{
    if (param >= 0x80 && param < 0x200)
    {
        if (programs.empty())
            return 0;
        return current().lanes[(std::size_t) ((param - 0x80) / 64)][(std::size_t) ((param - 0x80) % 64)];
    }
    const auto* i = info (param);
    if (i == nullptr)
        return 0;
    if (param < 20)
        return (std::size_t) param < fixp.size() ? std::clamp ((int) fixp[(std::size_t) param], i->min, i->max) : i->def;
    const auto k = (std::size_t) (param - 39);
    if (i->perProgram)
    {
        if (programs.empty())
            return i->def;
        const auto& p = current().params;
        return k < p.size() ? std::clamp ((int) p[k], i->min, i->max) : i->def;
    }
    return k < fpgp.size() ? std::clamp ((int) fpgp[k], i->min, i->max) : i->def;
}

void S::set (int param, int value)
{
    value = std::clamp (value, minimum (param), maximum (param));
    if (param >= 0x80 && param < 0x200)
    {
        if (! programs.empty())
            current().lanes[(std::size_t) ((param - 0x80) / 64)][(std::size_t) ((param - 0x80) % 64)] = (std::int8_t) value;
        return;
    }
    const auto* i = info (param);
    if (i == nullptr)
        return;
    auto store = [&] (std::vector<std::int8_t>& v, std::size_t k, int def) {
        if (v.size() <= k)
            v.resize (k + 1, (std::int8_t) def);
        v[k] = (std::int8_t) value;
    };
    if (param < 20)
        store (fixp, (std::size_t) param, i->def);
    else if (i->perProgram)
    {
        if (! programs.empty())
            store (current().params, (std::size_t) (param - 39), i->def);
    }
    else
        store (fpgp, (std::size_t) (param - 39), i->def);
}

void shiftPattern (SequencerSettings& s, int delta)
{
    if (s.programs.empty())
        return;
    const auto steps = std::clamp (s.get (S::steps), 1, 64);
    auto& p = s.current();
    for (auto& lane : p.lanes)
    {
        auto old = lane;
        for (int i = 0; i < steps; ++i)
            lane[(std::size_t) ((((i + delta) % steps) + steps) % steps)] = old[(std::size_t) i];
    }
}

// ---------------------------------------------------------------------------------------------------------------
// LYNX's own bank (a fresh plugin without FL Studio's factory bank): a few programs written for LYNX

SequencerSettings defaultSequencer()
{
    S s;
    s.fixp = { 17, -1, 0, 0, 0, 1, 0, 120, 2, 3, 8, -1, 5, 1, 0, 0, 0, 0, 0, 0 };
    s.fpgp = { 17, 16, 90, 4, 0, 0, 0, 127, 0, 127, 0, 64, 0, 1, 0, 0, 0, 0, 0, 0, 0,
               0,  0,  0,  0, 127, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 3, 0, 0 };
    for (int c = 0; c < 16; ++c)
    {
        S::Chain chain;
        chain.name = nameBytes ("Chain " + std::string (c < 9 ? "0" : "") + std::to_string (c + 1));
        s.chains.push_back (chain);
    }
    S::Program blank;
    blank.params = s.fpgp;
    for (auto& v : blank.lanes[S::octave]) v = 0x08;
    for (auto& v : blank.lanes[S::stepType]) v = S::normal;
    for (auto& v : blank.lanes[S::keySelect]) v = 0x04;
    for (auto& v : blank.lanes[S::velocity]) v = 96;
    blank.setName ("Initial");
    s.programs.assign (128, blank);

    auto make = [&] (int index, const std::string& name, auto&& fill) {
        auto p = blank;
        p.setName (name);
        fill (p);
        s.programs[(std::size_t) index] = p;
    };
    auto param = [] (S::Program& p, int flParam, int v) { p.params[(std::size_t) (flParam - 39)] = (std::int8_t) v; };
    auto keys = [] (S::Program& p, std::initializer_list<int> slots) { // 1…5 = N1…N5, 0 = Fixed, -1 = Root
        int i = 0;
        for (int step = 0; step < 64; ++step)
        {
            const auto k = *(slots.begin() + (i++ % (int) slots.size()));
            p.lanes[S::keySelect][(std::size_t) step] = (std::int8_t) (k == 0 ? 1 : k < 0 ? 2 : 1 << (k + 1));
        }
    };
    make (1, "Thru", [&] (S::Program& p) { param (p, S::steps, 0); });
    make (2, "Up 4 keys", [&] (S::Program& p) {
        param (p, S::gate, 50);
        param (p, S::missingKeys, 3);
        keys (p, { 1, 2, 3, 4 });
    });
    make (3, "Down 4 keys", [&] (S::Program& p) {
        param (p, S::gate, 50);
        param (p, S::missingKeys, 2);
        keys (p, { 4, 3, 2, 1 });
    });
    make (4, "Up and down 3 keys", [&] (S::Program& p) {
        param (p, S::gate, 60);
        param (p, S::missingKeys, 3);
        keys (p, { 1, 2, 3, 2 });
    });
    make (5, "Up 2 octaves", [&] (S::Program& p) {
        param (p, S::steps, 12);
        param (p, S::gate, 60);
        param (p, S::missingKeys, 1);
        keys (p, { 1, 2, 3 });
        for (int i = 3; i < 6; ++i)
            p.lanes[S::octave][(std::size_t) i] = p.lanes[S::octave][(std::size_t) (i + 6)] = 0x10;
    });
    make (6, "Chord stabs", [&] (S::Program& p) {
        param (p, S::gate, 40);
        const int pattern[16] = { 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0 };
        for (int i = 0; i < 64; ++i)
            p.lanes[S::stepType][(std::size_t) i] = (std::int8_t) (pattern[i % 16] ? S::chord : S::off);
    });
    make (7, "Gated chords", [&] (S::Program& p) {
        param (p, S::gate, 70);
        const int pattern[16] = { 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 1 };
        for (int i = 0; i < 64; ++i)
            p.lanes[S::stepType][(std::size_t) i] = (std::int8_t) (pattern[i % 16] ? S::chord : S::off);
    });
    make (8, "Random notes", [&] (S::Program& p) {
        param (p, S::gate, 50);
        for (auto& v : p.lanes[S::stepType]) v = S::random;
    });
    make (9, "Octave bass", [&] (S::Program& p) {
        param (p, S::gate, 40);
        for (int i = 0; i < 64; ++i)
        {
            p.lanes[S::octave][(std::size_t) i] = (std::int8_t) (i % 2 == 0 ? 0x08 : 0x10);
            p.lanes[S::velocity][(std::size_t) i] = (std::int8_t) (i % 4 == 0 ? 120 : 90);
        }
    });
    make (10, "A minor run", [&] (S::Program& p) {
        param (p, S::gate, 70);
        param (p, S::forceKey, 2 + 9);
        param (p, S::forceScale, 3);
        const int run[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1, 0, -1 };
        for (int i = 0; i < 64; ++i)
            p.lanes[S::scaleStep][(std::size_t) i] = (std::int8_t) run[i % 16];
    });
    make (11, "Scale of the chord", [&] (S::Program& p) {
        param (p, S::gate, 70);
        param (p, S::forceKey, 1);
        param (p, S::forceScale, 1);
        keys (p, { -1 });
        const int run[16] = { 0, 2, 4, 7, 4, 2, 0, -1, 0, 2, 4, 5, 7, 5, 4, 2 };
        for (int i = 0; i < 64; ++i)
            p.lanes[S::scaleStep][(std::size_t) i] = (std::int8_t) run[i % 16];
    });
    make (12, "Accents", [&] (S::Program& p) {
        param (p, S::gate, 55);
        param (p, S::missingKeys, 1);
        keys (p, { 1, 2, 3, 4 });
        const int vel[4] = { 127, 64, 96, 64 };
        for (int i = 0; i < 64; ++i)
            p.lanes[S::velocity][(std::size_t) i] = (std::int8_t) vel[i % 4];
    });
    make (13, "Legato ties", [&] (S::Program& p) {
        param (p, S::gate, 90);
        param (p, S::missingKeys, 1);
        keys (p, { 1, 2, 3, 2 });
        for (int i = 0; i < 64; ++i)
            if (i % 4 == 1)
                p.lanes[S::stepType][(std::size_t) i] = S::tie;
            else if (i % 8 == 7)
                p.lanes[S::stepType][(std::size_t) i] = S::rest;
    });
    make (14, "Triplets up", [&] (S::Program& p) {
        param (p, S::steps, 12);
        param (p, S::sync, 5);
        param (p, S::gate, 60);
        param (p, S::missingKeys, 1);
        keys (p, { 1, 2, 3 });
    });
    make (15, "Swing eighths", [&] (S::Program& p) {
        param (p, S::steps, 8);
        param (p, S::sync, 6);
        param (p, S::gate, 50);
        param (p, S::swing, 30);
        param (p, S::missingKeys, 1);
        keys (p, { 1, 3, 2, 3 });
    });
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// FL's state

std::optional<SequencerSettings> parseSequencer (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    S s;
    s.version = r.u32();
    if (r.failed || s.version != 2)
        return std::nullopt;
    s.gui.clear();
    bool bank = false;
    while (r.left() >= 12)
    {
        const auto id = r.u32();
        const auto size = r.get<std::uint64_t>();
        if (r.failed || size > r.left())
            return std::nullopt;
        const auto* data = fl.data() + r.pos;
        r.pos += (std::size_t) size;
        if (id == 2)
            s.gui.assign (data, data + size);
        else if (id == 3)
            s.options.assign (data, data + size);
        else if (id == 1)
        {
            Reader e { data, (std::size_t) size };
            s.engineVersion = e.u32();
            char tag[4];
            if (e.left() < 8)
                return std::nullopt;
            std::memcpy (tag, data + e.pos, 4);
            e.pos += 4;
            const auto bankSize = e.u32();
            if (e.failed || std::memcmp (tag, "bbnk", 4) != 0 || bankSize > e.left())
                return std::nullopt;
            // the bank's chunks
            auto walk = [&] (std::size_t from, std::size_t to, auto&& onChunk) {
                auto p = from;
                while (p + 8 <= to)
                {
                    std::uint32_t n = 0;
                    std::memcpy (&n, data + p + 4, 4);
                    if (n > to - p - 8)
                        return false;
                    onChunk (data + p, data + p + 8, (std::size_t) n);
                    p += 8 + n;
                }
                return p == to;
            };
            bool ok = true;
            ok = walk (e.pos, e.pos + bankSize, [&] (const std::uint8_t* head, const std::uint8_t* body, std::size_t n) {
                const std::string_view t ((const char*) head, 4);
                if (t == "fixp")
                    s.fixp.assign ((const std::int8_t*) body, (const std::int8_t*) body + n);
                else if (t == "fpgp")
                    s.fpgp.assign ((const std::int8_t*) body, (const std::int8_t*) body + n);
                else if (t == "chnp")
                {
                    s.chains.emplace_back();
                    s.chains.back().head.assign (body, body + n);
                }
                else if (t == "chns" && ! s.chains.empty())
                    std::memcpy (s.chains.back().slots.data(), body, std::min<std::size_t> (n, 8));
                else if (t == "chnn" && ! s.chains.empty())
                    std::memcpy (s.chains.back().name.data(), body, std::min<std::size_t> (n, 48));
                else if (t == "prog")
                {
                    S::Program p;
                    const auto from = (std::size_t) (body - data);
                    ok = ok && walk (from, from + n, [&] (const std::uint8_t* h, const std::uint8_t* b, std::size_t m) {
                        const std::string_view u ((const char*) h, 4);
                        if (u == "prgp")
                            p.params.assign ((const std::int8_t*) b, (const std::int8_t*) b + m);
                        else if (u == "pgnm")
                            std::memcpy (p.name.data(), b, std::min<std::size_t> (m, 48));
                        else if (u.substr (0, 3) == "stp" && u[3] >= '0' && u[3] <= '5' && m == 64)
                            std::memcpy (p.lanes[(std::size_t) (u[3] - '0')].data(), b, 64);
                        else
                            p.extra.emplace_back (h, b + m);
                    });
                    s.programs.push_back (std::move (p));
                }
                else
                    s.extra.emplace_back (head, body + n);
            });
            if (! ok)
                return std::nullopt;
            bank = true;
        }
    }
    if (! bank || s.programs.empty() || s.fixp.size() < 20)
        return std::nullopt;
    return s;
}

std::vector<std::uint8_t> writeSequencer (const SequencerSettings& s)
{
    std::vector<std::uint8_t> bank;
    std::vector<std::uint8_t> fixp (s.fixp.size()), fpgp (s.fpgp.size());
    std::memcpy (fixp.data(), s.fixp.data(), fixp.size());
    std::memcpy (fpgp.data(), s.fpgp.data(), fpgp.size());
    appendChunk (bank, chunk ("fixp", fixp));
    appendChunk (bank, chunk ("fpgp", fpgp));
    for (const auto& c : s.chains)
    {
        appendChunk (bank, chunk ("chnp", c.head));
        appendChunk (bank, chunk ("chns", bytes (c.slots)));
        appendChunk (bank, chunk ("chnn", bytes (c.name)));
    }
    for (const auto& p : s.programs)
    {
        std::vector<std::uint8_t> prog;
        std::vector<std::uint8_t> params (p.params.size());
        std::memcpy (params.data(), p.params.data(), params.size());
        appendChunk (prog, chunk ("prgp", params));
        appendChunk (prog, chunk ("pgnm", bytes (p.name)));
        for (int l = 0; l < S::laneCount; ++l)
        {
            const char tag[5] = { 's', 't', 'p', (char) ('0' + l), 0 };
            appendChunk (prog, chunk (tag, bytes (p.lanes[(std::size_t) l])));
        }
        for (const auto& x : p.extra)
            appendChunk (prog, x);
        appendChunk (bank, chunk ("prog", prog));
    }
    for (const auto& x : s.extra)
        appendChunk (bank, x);

    std::vector<std::uint8_t> engine;
    put<std::uint32_t> (engine, s.engineVersion);
    appendChunk (engine, chunk ("bbnk", bank));

    std::vector<std::uint8_t> out;
    put<std::uint32_t> (out, s.version);
    auto record = [&] (std::uint32_t id, const std::vector<std::uint8_t>& data) {
        put<std::uint32_t> (out, id);
        put<std::uint64_t> (out, (std::uint64_t) data.size());
        out.insert (out.end(), data.begin(), data.end());
    };
    record (1, engine);
    if (! s.gui.empty())
        record (2, s.gui);
    if (! s.options.empty())
        record (3, s.options);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// The engine

namespace
{
struct Chord
{
    std::uint16_t mask;
    bool inversion;
    int rootOffset, score;
    std::array<int, 2> completion;
    std::array<std::int8_t, 7> scale;
    int scaleCount;
};

constexpr std::uint16_t m (std::initializer_list<int> intervals)
{
    std::uint16_t v = 0;
    for (const auto i : intervals)
        v = (std::uint16_t) (v | (1u << i));
    return v;
}

// chord masks, whether found on a note that is not the lowest, root offset, score, completion notes (order
// "chord normalized"), and the scale each one implies (Force scale "detect")
const Chord chords[38] = {
    { m ({ 0, 7 }), false, 0, 20, { 0, 0 }, { 0, 0, 0, 0, 7, 7, 7 }, 7 },
    { m ({ 0, 5 }), false, 0, 20, { 0, 0 }, { 0, 0, 0, 5, 5, 5, 5 }, 7 },
    { m ({ 0, 4 }), false, 0, 20, { 7, 0 }, { 0, 2, 4, 5, 7, 7, 9 }, 7 },
    { m ({ 0, 3 }), false, 0, 20, { 7, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 4, 7 }), true, 0, 30, { 0, 0 }, { 0, 2, 4, 5, 7, 7, 9 }, 7 },
    { m ({ 0, 3, 7 }), true, 0, 30, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 5, 7 }), false, 0, 30, { 0, 0 }, { 0, 5, 5, 5, 7, 7, 7 }, 7 },
    { m ({ 0, 2, 7 }), false, 0, 30, { 0, 0 }, { 0, 2, 2, 2, 7, 7, 7 }, 7 },
    { m ({ 0, 3, 6 }), true, 0, 30, { 0, 0 }, { 0, 2, 3, 5, 6, 6, 6 }, 7 },
    { m ({ 0, 4, 6 }), true, 0, 30, { 0, 0 }, { 0, 2, 4, 6, 7, 7, 7 }, 7 },
    { m ({ 0, 4, 8 }), false, 0, 30, { 0, 0 }, { 0, 2, 5, 8, 9, 9, 11 }, 7 },
    { m ({ 0, 2, 4 }), true, 0, 30, { 7, 0 }, { 0, 2, 2, 4, 4, 7, 7 }, 7 },
    { m ({ 0, 2, 3 }), true, 0, 30, { 0, 0 }, { 0, 2, 2, 3, 3, 5, 7 }, 7 },
    { m ({ 0, 4, 11 }), true, 0, 30, { 7, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 3, 10 }), true, 0, 30, { 7, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 4, 10 }), true, 0, 30, { 7, 0 }, { 0, 2, 4, 5, 7, 9, 10 }, 7 },
    { m ({ 0, 4, 7, 11 }), true, 0, 40, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 4, 7, 9 }), false, 0, 40, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 4, 7, 10 }), true, 0, 40, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 10 }, 7 },
    { m ({ 0, 2, 4, 7 }), true, 0, 40, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 2, 3, 7 }), true, 0, 40, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 5, 7, 10 }), false, 0, 40, { 0, 0 }, { 0, 0, 5, 5, 7, 7, 10 }, 7 },
    { m ({ 0, 2, 7, 10 }), false, 0, 40, { 0, 0 }, { 0, 2, 5, 5, 7, 7, 10 }, 7 },
    { m ({ 0, 2, 4, 9 }), false, 2, 40, { 0, 0 }, { 0, 2, 5, 5, 7, 7, 10 }, 7 },
    { m ({ 0, 4, 5, 11 }), false, 0, 40, { 7, 9 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 2, 3, 10 }), true, 0, 40, { 7, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 3, 7, 9 }), true, 0, 40, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 3, 7, 10 }), true, 0, 40, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 3, 7, 11 }), true, 0, 40, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 11 }, 7 },
    { m ({ 0, 3, 6, 7 }), true, 0, 40, { 0, 0 }, { 0, 2, 3, 6, 7, 8, 10 }, 7 },
    { m ({ 0, 3, 6, 9 }), false, 0, 40, { 0, 0 }, { 0, 2, 3, 6, 7, 10, 0 }, 6 },
    { m ({ 0, 4, 6, 9 }), false, 0, 40, { 0, 0 }, { 0, 2, 4, 5, 6, 9, 0 }, 6 },
    { m ({ 0, 4, 8, 9 }), false, 0, 40, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 0 }, 6 },
    { m ({ 0, 2, 4, 7, 11 }), true, 0, 50, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 4, 5, 7, 11 }), true, 0, 50, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
    { m ({ 0, 2, 3, 7, 10 }), true, 0, 50, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 3, 5, 7, 10 }), true, 0, 50, { 0, 0 }, { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { m ({ 0, 4, 5, 7, 9, 11 }), true, 0, 60, { 0, 0 }, { 0, 2, 4, 5, 7, 9, 11 }, 7 },
};

struct Scale
{
    std::array<std::int8_t, 7> degrees;
    int count;
};
// Force scale 2…14
const Scale scales[13] = {
    { { 0, 2, 4, 5, 7, 9, 11 }, 7 }, { { 0, 2, 3, 5, 7, 8, 10 }, 7 }, { { 0, 2, 3, 5, 7, 8, 11 }, 7 },
    { { 0, 2, 3, 5, 7, 9, 11 }, 7 }, { { 0, 2, 4, 7, 9 }, 5 },          { { 0, 3, 5, 7, 10 }, 5 },
    { { 0, 2, 5, 7, 10 }, 5 },       { { 0, 3, 5, 6, 7, 10 }, 6 },      { { 0, 2, 3, 5, 7, 9, 10 }, 7 },
    { { 0, 1, 3, 5, 7, 8, 10 }, 7 }, { { 0, 2, 4, 6, 7, 9, 11 }, 7 },   { { 0, 2, 4, 5, 7, 9, 10 }, 7 },
    { { 0, 1, 3, 5, 6, 8, 10 }, 7 },
};

// The current program with every value resolved (built on the UI thread)
struct Runtime
{
    int midiIn = -1, arpMode = 1, steps = 16, gate = 90, sync = 4, latch = 0, missing = 0, inLow = 0, inHigh = 127,
        outLow = 0, outHigh = 127, order = 0, fixedKey = 64, transposeOctaves = 0, inputQuantize = 1, transposeSemitones = 0,
        swing = 0, restartOn = 0, forceKey = 0, forceScale = 0, forceMode = 0, pageLock = 0, page = 0, wrapLow = 0,
        wrapHigh = 127, polyOctave = 0, polyKeys = 0, missingOctave = 0, gateLaneMode = 0, randomVelocity = 0,
        randomGate = 0, randomStart = 0, smartBend = 0, programMode = 0, inputRangeMode = 0, velocityMode = 0;
    std::array<std::array<std::int8_t, 64>, 6> lanes {};
};

Runtime resolve (const S& s)
{
    Runtime r;
    r.midiIn = s.get (S::midiIn);
    r.arpMode = s.get (S::arpMode);
    r.steps = s.get (S::steps);
    r.gate = s.get (S::gate);
    r.sync = s.get (S::sync);
    r.latch = s.get (S::latch);
    r.missing = s.get (S::missingKeys);
    r.inLow = s.get (S::inputLow);
    r.inHigh = s.get (S::inputHigh);
    r.outLow = s.get (S::outputLow);
    r.outHigh = s.get (S::outputHigh);
    r.order = s.get (S::order);
    r.fixedKey = s.get (S::fixedKey);
    r.transposeOctaves = s.get (S::transposeOctaves);
    r.inputQuantize = s.get (S::inputQuantize);
    r.transposeSemitones = s.get (S::transposeSemitones);
    r.swing = s.get (S::swing);
    r.restartOn = s.get (S::restartOn);
    r.forceKey = s.get (S::forceKey);
    r.forceScale = s.get (S::forceScale);
    r.forceMode = s.get (S::forceMode);
    r.pageLock = s.get (S::pageLock);
    r.page = s.get (S::page);
    r.wrapLow = s.get (S::inputWrapLow);
    r.wrapHigh = s.get (S::inputWrapHigh);
    r.polyOctave = s.get (S::polyOctave);
    r.polyKeys = s.get (S::polyKeys);
    r.missingOctave = s.get (S::missingOctave);
    r.gateLaneMode = s.get (S::gateLaneMode);
    r.randomVelocity = s.get (S::randomVelocity);
    r.randomGate = s.get (S::randomGate);
    r.randomStart = s.get (S::randomStart);
    r.smartBend = s.get (S::smartBend);
    r.programMode = s.get (S::programMode);
    r.inputRangeMode = s.get (S::inputRangeMode);
    r.velocityMode = s.get (S::velocityMode);
    if (! s.programs.empty())
        r.lanes = s.current().lanes;
    return r;
}

// FL's "MIDI velocity" here is the voice volume × 100 (velocity 100/128 = 100)
int midiVelocity (float velocity) noexcept
{
    const auto volume = (std::pow (11.0, 1.28 * (double) velocity) - 1.0) / 10.0;
    return (int) std::clamp (volume * 100.0 + 1e-9, 0.0, 127.0);
}

float voiceVelocity (int midi) noexcept
{
    const auto volume = std::max (0, midi) / 100.0;
    return (float) std::clamp (std::log (10.0 * volume + 1.0) / std::log (11.0) / 1.28, 0.0, 1.0);
}

class Sequencer final : public Processor
{
public:
    explicit Sequencer (const S& s) : settings (resolve (s)) {}

    Kind kind() const noexcept override { return Kind::sequencer; }

    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        const auto s = parseSequencer (fl);
        if (! s)
            return false;
        settings.publish (resolve (*s));
        return true;
    }

    bool busy() const noexcept override { return heldCount > 0 || arpCount > 0 || soundingCount > 0 || thruCount > 0 || freeRun != 0; }

    void releaseAll (EventList& out) noexcept override
    {
        killAll (0, out);
        heldCount = arpCount = keyCount = 0;
        freeRun = 0;
        lastStep = -1;
    }

    void process (const Event* in, int count, int n, const Transport& t, EventList& out) noexcept override
    {
        if (settings.take())
        {
            const auto& r = settings.get();
            if (r.arpMode != lastArpMode && lastArpMode >= 0)
            {
                killAll (0, out);
                lastStep = -1;
            }
            keysDirty = arpCount > 0;
        }
        const auto& r = settings.get();
        lastArpMode = r.arpMode;
        sampleRate = t.sampleRate > 0.0 ? t.sampleRate : 44100.0;
        const auto bpm = std::max (1.0, t.bpm);
        const auto beatsPerSample = bpm / 60.0 / sampleRate;
        stepSamples = sampleRate * 60.0 / bpm * syncBeats[std::clamp (r.sync, 0, 12)];

        // transport start / stop
        const auto songBeats = t.ticks / 960.0;
        if (t.playing && ! wasPlaying)
        {
            anchored = false;
            restartOffset = 0.0;
            if (r.restartOn == 3)
            {
                restartOffset = quantizeUp (songBeats, r);
                anchored = true;
            }
            freeRun = 0;
            lastStep = -1;
        }
        else if (! t.playing && wasPlaying)
        {
            killAll (0, out);
            heldCount = arpCount = keyCount = 0;
            chordIndex = -1;
            freeRun = 0;
            lastStep = -1;
        }
        else if (t.playing && songBeats < lastSongBeats - 1e-6 && r.restartOn != 0) // (a loop jumped back)
            restartOffset = std::min (restartOffset, songBeats);
        wasPlaying = t.playing;

        if (count == 0 && ! busy())
        {
            lastSongBeats = songBeats + n * beatsPerSample;
            return;
        }

        int next = 0;
        for (int s = 0; s < n; ++s)
        {
            while (next < count && in[next].offset <= s)
                input (in[next++], s, songBeats + s * beatsPerSample, t.playing, r, out);

            const auto playingArp = mode (r) == Mode::arp;
            if (playingArp && arpCount > 0 && (t.playing || freeRun != 0))
            {
                const auto pos = t.playing ? songBeats + s * beatsPerSample : freeBeats;
                const auto rel = t.playing ? pos - (r.restartOn == 0 ? 0.0 : restartOffset) : pos;
                if (rel >= -1e-9)
                {
                    const auto step = stepIndex (std::max (0.0, rel), r);
                    if (step != lastStep || regenerate)
                    {
                        const auto grid = quantizeBeats[std::clamp (r.inputQuantize, 0, 8)];
                        const auto cell = grid > 0.0 ? (long long) std::floor (rel / grid + 1e-9) : -1;
                        if (keysDirty && (grid <= 0.0 || cell != lastCell || keyCount == 0))
                        {
                            buildKeys (r);
                            keysDirty = false;
                        }
                        lastCell = cell;
                        if (step != lastStep || regenerate)
                            generate (step, s, r, out);
                        lastStep = step;
                        regenerate = false;
                    }
                }
            }
            schedule (s, out);
            if (! t.playing && freeRun != 0)
            {
                freeBeats += beatsPerSample;
                if (freeRun == 2 && --tailSamples <= 0)
                {
                    freeRun = 0;
                    freeBeats = 0.0;
                    lastStep = -1;
                }
            }
        }
        while (next < count)
            input (in[next++], std::max (0, n - 1), songBeats + n * beatsPerSample, t.playing, r, out);
        lastSongBeats = songBeats + n * beatsPerSample;
    }

private:
    enum class Mode { off, arp, thru };
    struct Key
    {
        int note = 0, velocity = 96, order = 0;
    };
    struct Sounding
    {
        int note = 0, velocity = 0, id = 0;
        double startDelay = 0.0, gate = 0.0;
        bool started = false;
    };
    struct Thru
    {
        int input = 0, id = 0, note = 0;
    };

    Mode mode (const Runtime& r) const noexcept
    {
        switch (r.arpMode)
        {
            case 0: return Mode::off;
            case 2: return Mode::thru;
            case 3: case 4: case 5: case 6:
            {
                const auto threshold = r.arpMode <= 4 ? 2 : 3;
                if (heldCount >= threshold)
                    return r.steps < 2 ? Mode::thru : Mode::arp;
                return r.arpMode == 4 || r.arpMode == 6 ? Mode::thru : Mode::off;
            }
            default: return r.steps < 2 ? Mode::thru : Mode::arp;
        }
    }

    double quantizeUp (double beats, const Runtime& r) const noexcept
    {
        const auto grid = quantizeBeats[std::clamp (r.inputQuantize, 0, 8)];
        return grid > 0.0 ? std::ceil (beats / grid - 1e-9) * grid : beats;
    }

    int stepIndex (double rel, const Runtime& r) const noexcept
    {
        const auto steps = std::max (1, r.steps);
        const auto stepBeats = syncBeats[std::clamp (r.sync, 0, 12)];
        const auto perBeat = 1.0 / stepBeats;
        auto x = rel * perBeat;
        if (r.sync > 7) // (FL's plugin wraps the long syncs this way)
        {
            const auto span = std::floor (steps * perBeat) * perBeat;
            if (span > 0.0)
                x = std::fmod (x, span);
        }
        if (r.swing != 0)
        {
            const auto k = std::floor (x / 2.0);
            const auto f = x - 2.0 * k;
            const auto threshold = r.swing >= 0 ? 1.0 : 0.0; // (FL: (s + 100) div 100)
            if (f >= threshold)
                x = 2.0 * k + 2.0 - (100.0 / (100.0 - r.swing)) * (2.0 - f);
            else
                x = 2.0 * k + (100.0 / (100.0 + r.swing)) * f;
        }
        auto step = (long long) std::floor (x + 1e-9);
        if (r.pageLock != 0)
            return std::min (steps - 1, r.page * 16 + (int) (step % 16));
        return (int) (step % steps);
    }

    // --- input ------------------------------------------------------------------------------------------------

    void input (const Event& e, int offset, double songBeats, bool playing, const Runtime& r, EventList& out) noexcept
    {
        const auto key = std::clamp (e.voice.key, 0, 127);
        if (e.type == Event::Type::off)
        {
            for (int i = 0; i < thruCount; ++i)
                if (thru[(std::size_t) i].input == e.id)
                {
                    off (thru[(std::size_t) i].id, thru[(std::size_t) i].note, offset, out);
                    thru[(std::size_t) i] = thru[(std::size_t) --thruCount];
                    break;
                }
            const auto before = mode (r);
            removeKey (held, heldCount, key);
            if (r.latch == 0)
            {
                removeKey (arp, arpCount, key);
                keysDirty = true;
            }
            if (mode (r) != before)
                modeChanged (offset, out);
            if (heldCount == 0 && arpCount == 0)
            {
                anchoredByKey = false;
                if (! playing && freeRun == 1)
                {
                    if (r.restartOn == 1)
                    {
                        freeRun = 0;
                        freeBeats = 0.0;
                        lastStep = -1;
                    }
                    else
                    {
                        freeRun = 2;
                        tailSamples = (long long) sampleRate;
                    }
                }
                keyCount = 0;
                if (r.restartOn == 1 || r.restartOn == 2)
                    lastStep = -1;
            }
            return;
        }
        if (e.type != Event::Type::on)
            return;
        const auto inRange = key >= r.inLow && key <= r.inHigh;
        const auto velocity = midiVelocity (e.voice.velocity);
        if (! inRange && r.inputRangeMode == 0)
            return;
        if (! inRange || r.arpMode == 2)
        {
            if (r.arpMode != 0)
                thruNote (e, key, offset, r, out);
            return;
        }
        const auto before = mode (r);
        if (r.latch != 0 && heldCount == 0) // (a new chord replaces the latched one)
            arpCount = 0;
        const auto wasEmpty = arpCount == 0;
        addKey (held, heldCount, key, velocity);
        addKey (arp, arpCount, key, velocity);
        keysDirty = true;
        const auto now = mode (r);
        if (now != before)
            modeChanged (offset, out);
        if (now == Mode::thru)
        {
            thruNote (e, key, offset, r, out);
            return;
        }
        if (now != Mode::arp)
            return;
        if (! playing)
        {
            if (freeRun != 2)
            {
                freeBeats = 0.0;
                lastStep = -1;
            }
            freeRun = 1;
        }
        else if (wasEmpty && ((r.restartOn == 1) || (r.restartOn == 2 && ! anchored)))
        {
            restartOffset = quantizeUp (songBeats, r);
            anchored = true;
            anchoredByKey = true;
            lastStep = -1;
        }
        if (wasEmpty)
        {
            buildKeys (r);
            keysDirty = false;
            regenerate = true;
        }
    }

    void addKey (std::array<Key, 16>& list, int& count, int note, int velocity) noexcept
    {
        removeKey (list, count, note);
        if (count >= 16)
            return;
        list[(std::size_t) count++] = { note, velocity, ++arrivals };
    }
    static void removeKey (std::array<Key, 16>& list, int& count, int note) noexcept
    {
        for (int i = 0; i < count; ++i)
            if (list[(std::size_t) i].note == note)
            {
                for (int j = i + 1; j < count; ++j)
                    list[(std::size_t) (j - 1)] = list[(std::size_t) j];
                --count;
                return;
            }
    }
    void modeChanged (int offset, EventList& out) noexcept
    {
        killAll (offset, out);
        lastStep = -1;
        regenerate = true;
    }

    void thruNote (const Event& e, int key, int offset, const Runtime& r, EventList& out) noexcept
    {
        if (thruCount >= (int) thru.size())
            return;
        const auto note = pitch (key, 0, false, r);
        Voice v;
        v.key = note;
        v.velocity = e.voice.velocity;
        const auto id = newId();
        out.add (onEvent (offset, id, v));
        thru[(std::size_t) thruCount++] = { e.id, id, note };
    }

    // --- the key list (FL: rebuilt on the input-quantize grid) ---------------------------------------------------

    void buildKeys (const Runtime& r) noexcept
    {
        // pitch-sorted copy for chord detection
        std::array<Key, 16> sorted = arp;
        const auto n = arpCount;
        for (int i = 1; i < n; ++i)
        {
            const auto k = sorted[(std::size_t) i];
            auto j = i;
            for (; j > 0 && sorted[(std::size_t) (j - 1)].note > k.note; --j)
                sorted[(std::size_t) j] = sorted[(std::size_t) (j - 1)];
            sorted[(std::size_t) j] = k;
        }
        detectChord (sorted, n);

        keyCount = 0;
        auto push = [&] (int note, int velocity) {
            if (keyCount < 16)
                keys[(std::size_t) keyCount++] = { std::clamp (note, 0, 127), velocity, 0 };
        };
        if (r.programMode == 1)
        {
            const auto* scale = activeScale (r);
            push (r.fixedKey, 96);
            if (scale != nullptr && scale->count > 5)
                for (const auto d : { 2, 4, 5 })
                    push (r.fixedKey + scale->degrees[(std::size_t) d], 96);
            else
            {
                push (r.fixedKey + 5, 96);
                push (r.fixedKey + 7, 96);
            }
        }
        else if (r.order == 6 && chordIndex >= 0 && n > 0)
        {
            const auto& c = chords[chordIndex];
            int v = 0;
            for (int i = 0; i < 12; ++i)
                if (c.mask & (1u << i))
                    push (root + i, sorted[(std::size_t) (v++ % n)].velocity);
            for (const auto x : c.completion)
                if (x != 0)
                    push (root + x, sorted[(std::size_t) (v++ % n)].velocity);
        }
        else if (r.order == 7 && chordIndex >= 0)
        {
            const auto& c = chords[chordIndex];
            for (int i = 0; i < n; ++i)
            {
                const auto rel = ((sorted[(std::size_t) i].note - root) % 12 + 12) % 12;
                if (c.mask & (1u << rel))
                    push (root + rel, sorted[(std::size_t) i].velocity);
            }
        }
        else
        {
            std::array<Key, 16> list = r.order == 0 || r.order == 1 || r.order >= 6 ? sorted : arp;
            if (r.order == 4 || r.order == 5) // by velocity (ties keep their order)
                for (int i = 1; i < n; ++i)
                {
                    const auto k = list[(std::size_t) i];
                    auto j = i;
                    for (; j > 0 && list[(std::size_t) (j - 1)].velocity > k.velocity; --j)
                        list[(std::size_t) j] = list[(std::size_t) (j - 1)];
                    list[(std::size_t) j] = k;
                }
            const auto reverse = r.order == 1 || r.order == 3 || r.order == 5;
            for (int i = 0; i < n; ++i)
            {
                const auto& k = list[(std::size_t) (reverse ? n - 1 - i : i)];
                push (k.note, k.velocity);
            }
        }
        // input range wrap
        for (int i = 0; i < keyCount; ++i)
        {
            auto& k = keys[(std::size_t) i].note;
            while (k < r.wrapLow && k + 12 <= 127)
                k += 12;
            while (k > r.wrapHigh && k - 12 >= 0)
                k -= 12;
            k = std::clamp (k, std::min (r.wrapLow, r.wrapHigh), std::max (r.wrapLow, r.wrapHigh));
        }
        realKeys = keyCount;
        // the missing keys N1…N5
        filled = 0;
        if (keyCount > 0 && keyCount < 5 && r.missing != 0)
        {
            const auto octave = r.missingOctave == 1 ? -12 : r.missingOctave == 2 ? 12 : 0;
            for (int i = keyCount; i < 5; ++i)
            {
                Key k;
                switch (r.missing)
                {
                    case 1: k = keys[(std::size_t) (i % realKeys)]; break;
                    case 2: k = keys[0]; break;
                    case 3: k = keys[(std::size_t) (realKeys - 1)]; break;
                    default: k = { r.fixedKey, 96, 0 }; break;
                }
                k.note = std::clamp (k.note + octave, 0, 127);
                keys[(std::size_t) i] = k;
            }
            filled = 5 - keyCount;
        }
    }

    void detectChord (const std::array<Key, 16>& sorted, int n) noexcept
    {
        chordIndex = -1;
        root = n > 0 ? sorted[0].note : 60;
        int best = 0;
        for (int b = 0; b < n; ++b)
        {
            const auto candidate = sorted[(std::size_t) b].note;
            std::uint16_t mask = 0;
            for (int i = 0; i < n; ++i)
                mask = (std::uint16_t) (mask | (1u << (((sorted[(std::size_t) i].note - candidate) % 12 + 12) % 12)));
            for (int c = 0; c < 38; ++c)
            {
                const auto& entry = chords[c];
                if ((b == 0 || entry.inversion) && (entry.mask & ~mask) == 0 && entry.score > best)
                {
                    best = entry.score;
                    chordIndex = c;
                    root = candidate + entry.rootOffset;
                }
            }
        }
        if (chordIndex >= 0)
            detectedScale = { chords[chordIndex].scale, chords[chordIndex].scaleCount };
    }

    const Scale* activeScale (const Runtime& r) const noexcept
    {
        if (r.forceScale == 1 && chordIndex >= 0)
            return &detectedScale;
        if (r.forceScale >= 2 && r.forceScale <= 14)
            return &scales[r.forceScale - 2];
        return nullptr;
    }

    // FL's pitch pipeline: scale forcing (snap down, move by scale steps), transpose, the output range
    int pitch (int note, int scaleStep, bool force, const Runtime& r) const noexcept
    {
        auto key = r.forceKey - 2;
        if (key == -1)
            key = ((root % 12) + 12) % 12;
        if (key == -2 && r.forceScale > 0 && keyCount > 0)
            key = keys[0].note % 12;
        const auto* scale = activeScale (r);
        if (r.forceMode == 1 && scaleStep == 0)
            force = false;
        int n;
        if (! force || scale == nullptr || key < 0)
            n = note + scaleStep;
        else
        {
            const auto rr = note - key + 12;
            const auto octave = (int) std::floor (rr / 12.0) - 1;
            const auto pc = ((rr % 12) + 12) % 12;
            int d = 0;
            for (int i = 0; i < scale->count; ++i)
                if (scale->degrees[(std::size_t) i] <= pc)
                    d = i;
            d += scaleStep;
            const auto carry = (int) std::floor ((double) d / scale->count);
            d -= carry * scale->count;
            n = std::clamp (scale->degrees[(std::size_t) d] + key + (octave + carry) * 12, 0, 127);
        }
        n += r.transposeSemitones + 12 * r.transposeOctaves;
        if (n < r.outLow)
        {
            n += 12 * (int) std::ceil ((r.outLow - n) / 12.0);
            if (n > r.outHigh)
                n = r.outLow;
        }
        if (n > r.outHigh)
        {
            n -= 12 * (int) std::ceil ((n - r.outHigh) / 12.0);
            if (n < r.outLow)
                n = r.outHigh;
        }
        return std::clamp (n, 0, 127);
    }

    // --- one step ------------------------------------------------------------------------------------------------

    int rand15() noexcept
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (int) (seed & 0x7fff);
    }

    void generate (int step, int offset, const Runtime& r, EventList& out) noexcept
    {
        if (keyCount == 0 && r.programMode == 0)
            return;
        const auto s = (std::size_t) std::clamp (step, 0, 63);
        const auto type = (int) r.lanes[S::stepType][s];
        if (type == S::off || type == S::rest)
            return;
        struct Note
        {
            int note, velocity;
        };
        std::array<Note, 32> notes {};
        int count = 0;
        auto add = [&] (int note, int velocity) {
            if (count < (int) notes.size())
                notes[(std::size_t) count++] = { note, velocity };
        };
        if (type == S::chord)
            for (int i = 0; i < realKeys; ++i)
                add (keys[(std::size_t) i].note, keys[(std::size_t) i].velocity);
        else if (type == S::random)
        {
            int pick = 0;
            if (realKeys == 2)
            {
                pick = rand15() % 2;
                if (pick == lastRandom && ++randomRepeats >= 3)
                    pick = 1 - pick;
                if (pick != lastRandom)
                    randomRepeats = 0;
            }
            else if (realKeys > 2)
            {
                pick = rand15() % (realKeys - 1);
                if (pick >= lastRandom && lastRandom >= 0)
                    ++pick;
            }
            lastRandom = pick;
            if (realKeys > 0)
                add (keys[(std::size_t) pick].note, keys[(std::size_t) pick].velocity);
        }
        else
        {
            const auto select = (int) (std::uint8_t) r.lanes[S::keySelect][s];
            const auto first = keyCount > 0 ? keys[0].velocity : 96;
            for (int bit = 0; bit < 7; ++bit)
            {
                if ((select & (1 << bit)) == 0)
                    continue;
                if (bit == 0)
                    add (r.fixedKey, 96);
                else if (bit == 1)
                    add (root, first);
                else if (bit - 2 < keyCount + filled && bit - 2 < 5)
                    add (keys[(std::size_t) (bit - 2)].note, keys[(std::size_t) (bit - 2)].velocity);
            }
        }
        if (count == 0)
            return;
        // octaves (bit k = octave k − 3)
        const auto octaves = (int) (std::uint8_t) r.lanes[S::octave][s];
        if (r.polyOctave == 0)
        {
            int shift = 0;
            for (int k = 0; k < 7; ++k)
                if (octaves & (1 << k))
                    shift += 12 * (k - 3);
            for (int i = 0; i < count; ++i)
                notes[(std::size_t) i].note += shift;
        }
        else
        {
            bool firstBit = true;
            const auto base = notes[0];
            for (int k = 0; k < 7; ++k)
            {
                if ((octaves & (1 << k)) == 0)
                    continue;
                if (firstBit)
                {
                    for (int i = 0; i < count; ++i)
                        notes[(std::size_t) i].note += 12 * (k - 3);
                    firstBit = false;
                }
                else
                    add (base.note + 12 * (k - 3), base.velocity);
            }
        }
        auto scaleStep = (int) r.lanes[S::scaleStep][s] + r.smartBend;
        // gate (in steps)
        auto gatePercent = (double) r.gate;
        if (r.gateLaneMode == 0)
            gatePercent = std::min (std::floor (std::pow (2.0, (double) r.lanes[S::gateStep][s]) * gatePercent + 0.5), r.steps * 100.0 - 1.0);
        auto gate = gatePercent / 100.0;
        if (r.randomGate > 0)
            gate += r.randomGate * (rand15() - 16383) / 100.0 / 32767.0;
        gate = std::max (0.01, gate);
        // the following rests lengthen it; a tie makes it overlap the next note (legato)
        for (int k = 1; k < r.steps; ++k)
        {
            const auto t = (int) r.lanes[S::stepType][(std::size_t) ((step + k) % std::max (1, r.steps))];
            if (t == S::rest)
            {
                gate += 1.0;
                continue;
            }
            if (t == S::tie && gate < 1.25)
                gate = 1.25;
            break;
        }
        double delay = 0.0;
        if (r.randomStart > 0 && r.smartBend == 0)
            delay = rand15() / 32767.0 * stepSamples * r.randomStart / 200.0;
        for (int i = 0; i < count; ++i)
        {
            const auto note = pitch (notes[(std::size_t) i].note, scaleStep, true, r);
            int velocity = r.lanes[S::velocity][s];
            if (r.velocityMode == 1)
                velocity = notes[(std::size_t) i].velocity;
            else if (r.velocityMode == 2)
                velocity = std::clamp (notes[(std::size_t) i].velocity + velocity - 96, 0, 127);
            if (r.randomVelocity > 0)
                velocity = std::clamp ((int) std::lround (velocity + r.randomVelocity * ((rand15() - 16383.5) / 32767.0) * 1.27), 0, 127);
            start (note, velocity, offset, delay, gate * stepSamples, out);
        }
    }

    void start (int note, int velocity, int offset, double delay, double gateSamples, EventList& out) noexcept
    {
        for (int i = 0; i < soundingCount; ++i)
        {
            auto& e = sounding[(std::size_t) i];
            if (e.note != note)
                continue;
            if (e.started) // (the same pitch: retriggered)
                off (e.id, e.note, offset, out);
            e = { note, velocity, newId(), delay, gateSamples, false };
            if (delay <= 0.0)
                begin (e, offset, out);
            return;
        }
        if (soundingCount >= (int) sounding.size())
            return;
        auto& e = sounding[(std::size_t) soundingCount++];
        e = { note, velocity, newId(), delay, gateSamples, false };
        if (delay <= 0.0)
            begin (e, offset, out);
    }

    void begin (Sounding& e, int offset, EventList& out) noexcept
    {
        Voice v;
        v.key = e.note;
        v.velocity = voiceVelocity (e.velocity);
        out.add (onEvent (offset, e.id, v));
        e.started = true;
    }

    // one sample of the sounding notes: delayed starts, gate countdowns
    void schedule (int offset, EventList& out) noexcept
    {
        for (int i = 0; i < soundingCount;)
        {
            auto& e = sounding[(std::size_t) i];
            if (! e.started)
            {
                e.startDelay -= 1.0;
                if (e.startDelay <= 0.0)
                    begin (e, offset, out);
                ++i;
                continue;
            }
            e.gate -= 1.0;
            if (e.gate < 0.01)
            {
                off (e.id, e.note, offset, out);
                sounding[(std::size_t) i] = sounding[(std::size_t) --soundingCount];
                continue;
            }
            ++i;
        }
    }

    void killAll (int offset, EventList& out) noexcept
    {
        for (int i = 0; i < soundingCount; ++i)
            if (sounding[(std::size_t) i].started)
                off (sounding[(std::size_t) i].id, sounding[(std::size_t) i].note, offset, out);
        soundingCount = 0;
        for (int i = 0; i < thruCount; ++i)
            off (thru[(std::size_t) i].id, thru[(std::size_t) i].note, offset, out);
        thruCount = 0;
    }

    int newId() noexcept
    {
        const auto id = nextId++;
        if (nextId > 1 << 30)
            nextId = 1;
        return id;
    }

    static Event onEvent (int offset, int id, const Voice& v) noexcept
    {
        Event e;
        e.type = Event::Type::on;
        e.offset = offset;
        e.id = id;
        e.voice = v;
        return e;
    }

    static void off (int id, int note, int offset, EventList& out) noexcept
    {
        Event e;
        e.type = Event::Type::off;
        e.offset = offset;
        e.id = id;
        e.voice.key = note;
        out.add (e);
    }

    Handoff<Runtime> settings;
    std::array<Key, 16> held {}, arp {}, keys {};
    int heldCount = 0, arpCount = 0, keyCount = 0, realKeys = 0, filled = 0;
    int chordIndex = -1, root = 60;
    std::array<Sounding, 16> sounding {};
    int soundingCount = 0;
    std::array<Thru, 64> thru {};
    int thruCount = 0;
    int nextId = 1, lastStep = -1, lastArpMode = -1, lastRandom = -1, randomRepeats = 0;
    long long lastCell = -1, tailSamples = 0;
    bool keysDirty = false, regenerate = false, wasPlaying = false, anchored = false, anchoredByKey = false;
    int freeRun = 0; // 0 off, 1 running, 2 the second after the last key
    double freeBeats = 0.0, restartOffset = 0.0, lastSongBeats = 0.0, stepSamples = 1.0, sampleRate = 44100.0;
    std::uint32_t seed = 0x2545f491u;
    int arrivals = 0;
    Scale detectedScale {};
};
} // namespace

std::unique_ptr<Processor> makeSequencer (const std::vector<std::uint8_t>& fl)
{
    return std::make_unique<Sequencer> (parseSequencer (fl).value_or (defaultSequencer()));
}
} // namespace lynx::vfx
