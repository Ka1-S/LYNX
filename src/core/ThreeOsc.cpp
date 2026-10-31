#include "core/ThreeOsc.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

#include "core/FlVoiceProcessor.h"

// 1:1 port of our round-47 reference model (r47/py/osc3model.py, REA_rodada47). The model emulates the DLL's float32
// arithmetic with numpy float32 scalars, doubles where the DLL computes in double and 80-bit long doubles where its
// mixers run on the x87 (precision 64 bits: the DLL sets control word 0x33F at load); this file keeps those types and
// the operation order of every expression, so fusing a·b + c into one FMA would change the results: contraction stays
// off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int paramCount = 21;
constexpr int extraCount = 5; // invert ×3, AM, HQ
constexpr int snapshotSize = flvp::snapshotSize;
constexpr int stateSize = 0x60 + snapshotSize; // version 14
constexpr int maxVoices = 256;                 // voice records per instance (FL: unlimited; the oldest is cut beyond this)
constexpr int tableSize = 16384;

using flvp::bitsOf;
using flvp::roundEven;

std::int32_t i32At (const std::uint8_t* d) noexcept
{
    std::int32_t v;
    std::memcpy (&v, d, 4);
    return v;
}

// ------------------------------------------------------------------------------------------------------- state

struct Params
{
    std::array<std::int32_t, paramCount> P {};
    std::array<std::uint8_t, 3> inv {};
    bool am = false, hq = false;
};

// FUN_00738580 (load): versions below 10 (4 fields per oscillator, the version word doubling as osc 1's shape), 10 and
// 11 (20 parameters, no phase randomness; version 10 without the invert/AM bytes), 12 and 13 (21 parameters + invert,
// AM and the HQ byte), 14 and above (the same + the 364-byte snapshot)
bool parse (const std::vector<std::uint8_t>& d, Params& out, std::vector<std::uint8_t>* snapshot)
{
    if (d.size() < 4)
        return false;
    const auto version = i32At (d.data());
    const auto* body = d.data() + 4;
    const auto size = d.size() - 4;
    auto bodyInt = [&] (std::size_t i) { return 4 * i + 4 <= size ? i32At (body + 4 * i) : 0; };
    Params p;
    if (version < 10)
    {
        std::int32_t loc[11];
        loc[0] = version;
        for (int i = 0; i < 10; ++i)
            loc[i + 1] = bodyInt ((std::size_t) i);
        for (int i = 0; i < 3; ++i)
        {
            if (i > 0)
                p.P[(std::size_t) (7 * i - 1)] = loc[4 * i - 1];
            p.P[(std::size_t) (7 * i + 1)] = loc[4 * i];
            p.P[(std::size_t) (7 * i + 2)] = loc[4 * i + 1];
            p.P[(std::size_t) (7 * i + 3)] = loc[4 * i + 2];
        }
    }
    else if (version < 12)
    {
        for (int i = 0; i < 20; ++i)
            p.P[(std::size_t) i] = bodyInt ((std::size_t) i);
        if (version >= 11)
            for (int k = 0; k < 4; ++k)
            {
                const auto b = 80 + (std::size_t) k < size ? body[80 + k] : 0;
                if (k < 3)
                    p.inv[(std::size_t) k] = b;
                else
                    p.am = b != 0;
            }
    }
    else
    {
        for (int i = 0; i < 21; ++i)
            p.P[(std::size_t) i] = bodyInt ((std::size_t) i);
        auto byte = [&] (std::size_t at) { return at < size ? body[at] : (std::uint8_t) 0; };
        for (int k = 0; k < 3; ++k)
            p.inv[(std::size_t) k] = byte (84 + (std::size_t) k);
        p.am = byte (87) != 0;
        p.hq = (byte (88) & 1) != 0; // (a state without this byte: 0, not confirmed — the plugin's stack holds garbage)
        if (version >= 14 && size >= 0x1c8 && snapshot != nullptr)
            snapshot->assign (body + 0x5c, body + 0x5c + snapshotSize);
    }
    out = p;
    return true;
}

bool parseNormalised (const std::vector<std::uint8_t>& d, Params& out)
{
    return d.size() >= (std::size_t) stateSize && i32At (d.data()) == 14 && parse (d, out, nullptr);
}

std::pair<int, int> rangeOf (int index) noexcept
{
    if (index == 20)
        return { 0, 64 };
    if (index > 20)
        return { 0, 1 };
    switch ((index + 1) % 7)
    {
        case 0: return { 0, 128 };   // mix
        case 1: return { -64, 64 };  // pan
        case 2: return { 0, 6 };     // shape
        case 3: return { -24, 24 };  // coarse
        case 4: return { -100, 100 }; // fine
        case 5: return { -64, 64 };  // stereo phase offset
        default: return { -50, 50 }; // stereo detune
    }
}

// ------------------------------------------------------------------------------------------------------- host tables

// The host's WaveTables[0…5] (FLEngine FUN_009cdfc0, REA_rodada47 §1.2)
struct HostTables
{
    std::array<std::array<float, tableSize>, 6> t {};
    HostTables()
    {
        t[0] = flvp::sineTable();
        for (int i = 0; i < 8192; ++i)
            t[1][(std::size_t) ((i + 0x3000) & 0x3fff)] = (float) ((double) (i * 4) / 16384.0 + -1.0);
        for (int i = 8192; i < tableSize; ++i)
            t[1][(std::size_t) ((i + 0x3000) & 0x3fff)] = (float) (1.0 - (double) ((i - 8192) * 4) / 16384.0);
        for (int i = 0; i < tableSize; ++i)
            t[2][(std::size_t) i] = i < 8192 ? 1.0f : -1.0f;
        for (int i = 0; i < tableSize; ++i)
            t[3][(std::size_t) i] = (float) ((double) (2 * i) / 16384.0 + -1.0);
        // table 4: 7600 points of a decaying exponential rising from −1, then +1 up to 13653, then −1
        auto v = 32767.0f;
        for (int i = 0; i < 0x1db0; ++i)
        {
            const auto x = 32767.0f - v * 2.0f;
            const auto q = (std::int16_t) (std::uint16_t) ((std::int64_t) (double) x & 0xffff);
            t[4][(std::size_t) i] = (float) ((double) q / 32767.0);
            v = v * 0.996999979f;
        }
        for (int i = 0x1db0; i < 0x3555; ++i)
            t[4][(std::size_t) i] = 1.0f;
        for (int i = 0x3555; i < tableSize; ++i)
            t[4][(std::size_t) i] = -1.0f;
        // table 5: noise from MT19937 seeded with 19650218
        std::array<std::uint32_t, 624> mt {};
        mt[0] = 0x12bd6aa;
        for (std::uint32_t i = 1; i < 624; ++i)
            mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
        std::size_t mi = 624;
        for (int i = 0; i < tableSize; ++i)
        {
            if (mi >= 624)
            {
                for (std::size_t k = 0; k < 624; ++k)
                {
                    const auto z = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7fffffffu);
                    mt[k] = mt[(k + 397) % 624] ^ (z >> 1) ^ ((z & 1u) != 0 ? 0x9908b0dfu : 0u);
                }
                mi = 0;
            }
            auto y = mt[mi++];
            y ^= y >> 11;
            y ^= (y << 7) & 0x9d2c5680u;
            y ^= (y << 15) & 0xefc60000u;
            y ^= y >> 18;
            t[5][(std::size_t) i] = (float) ((double) y * 4.656612874161595e-10 - 1.0);
        }
    }
};

const HostTables& hostTables()
{
    static const HostTables tables;
    return tables;
}

// ------------------------------------------------------------------------------------------------------- x87

// The oscillator mixers run on the x87 with 64-bit precision (the DLL's control word 0x33F): long double arithmetic
// reproduces them where long double is the x87 format; the guard makes sure the precision field is 64-bit.
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Extended
{
    unsigned short saved = 0;
    X87Extended() noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved));
        unsigned short cw = (unsigned short) (saved | 0x300);
        asm volatile ("fldcw %0" : : "m"(cw));
    }
    ~X87Extended() { asm volatile ("fldcw %0" : : "m"(saved)); }
};
#else
struct X87Extended
{
};
#endif

using ld = long double;

// ------------------------------------------------------------------------------------------------------- HQ

// The "HQ" oscillators of engine_x64.dll (REA_rodada47 §2.6): each shape as 2048 analytic points (FUN_1800011f0),
// upsampled ×16 by linear interpolation, then 44 band-limited tables of 32768 points keeping the harmonics 1…H with
// H = floor(1024·2^(−k/4)) (FUN_180002d90 + FUN_180001be0: bins cut on an FFT). FL computes the FFT with its own SSE
// code in single precision; LYNX with a double-precision radix-2 FFT (the tables agree to float rounding).
constexpr int hqSize = 32768, hqLevels = 44;

struct HqSet
{
    std::array<std::vector<float>, hqLevels> level; // hqSize + 2 points each (the last two repeat the first two)
};

// FUN_1800011f0: shapes 1 triangle, 2 square, 3 saw, 4 the exponential "rounded" shape, on a phase x in [0, 1)
float hqShape (int s, float x) noexcept
{
    switch (s)
    {
        case 1:
            if (x < 0.25f)
                return x * 4.0f;
            if (x < 0.75f)
                return 1.0f - (x - 0.25f) * 4.0f;
            return (x - 0.75f) * 4.0f - 1.0f;
        case 2: return x < 0.5f ? 1.0f : -1.0f;
        case 3: return (x + x) - 1.0f;
        case 4:
        {
            if ((double) x >= 0.8333333333333334)
                return -1.0f;
            if (x >= 0.4638671875f)
                return 1.0f;
            const auto p = std::pow (0.997, (double) x * 16384.0);
            return (float) (((1.0 - p) + (1.0 - p)) - 1.0);
        }
        default: return 0.0f;
    }
}

// radix-2 FFT of hqSize points (twiddles from one table)
void fftInPlace (std::vector<double>& re, std::vector<double>& im, bool inverse)
{
    static const auto twiddle = [] {
        std::vector<double> c ((std::size_t) hqSize / 2), sn ((std::size_t) hqSize / 2);
        for (std::size_t k = 0; k < c.size(); ++k)
            c[k] = std::cos (2.0 * 3.141592653589793 * (double) k / (double) hqSize),
            sn[k] = std::sin (2.0 * 3.141592653589793 * (double) k / (double) hqSize);
        return std::make_pair (c, sn);
    }();
    const auto n = re.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (re[i], re[j]), std::swap (im[i], im[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const auto stride = n / len;
        for (std::size_t i = 0; i < n; i += len)
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto wr = twiddle.first[k * stride], wi = inverse ? twiddle.second[k * stride] : -twiddle.second[k * stride];
                const auto ur = re[i + k], ui = im[i + k];
                const auto vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                const auto vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr, im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr, im[i + k + len / 2] = ui - vi;
            }
    }
}

std::shared_ptr<const HqSet> buildHq (const std::array<float, 2048>& base)
{
    std::vector<double> re ((std::size_t) hqSize), im ((std::size_t) hqSize, 0.0);
    for (int j = 0; j < hqSize; ++j)
    {
        const auto d = (double) j * (1.0 / 16.0);
        const auto i = (int) d;
        const auto nx = i + 1 < 2048 ? i + 1 : 0;
        const auto fr = (float) d - (float) i;
        re[(std::size_t) j] = (double) ((1.0f - fr) * base[(std::size_t) i] + fr * base[(std::size_t) nx]);
    }
    fftInPlace (re, im, false);
    auto set = std::make_shared<HqSet>();
    auto d = 1024.0;
    const auto r = std::pow (2.0, -0.25);
    std::array<int, hqLevels> H {};
    for (int g = 0; g < hqLevels / 4; ++g)
    {
        H[(std::size_t) (4 * g)] = std::max (1, (int) d);
        H[(std::size_t) (4 * g + 1)] = std::max (1, (int) (d * r));
        d = d * r * r;
        H[(std::size_t) (4 * g + 2)] = std::max (1, (int) d);
        d = d * r;
        H[(std::size_t) (4 * g + 3)] = std::max (1, (int) d);
        d = d * r;
    }
    for (int k = 0; k < hqLevels; ++k)
    {
        const auto h = (std::size_t) H[(std::size_t) k];
        auto yr = re, yi = im;
        for (std::size_t b = h + 1; b < (std::size_t) hqSize - h; ++b)
            yr[b] = yi[b] = 0.0;
        fftInPlace (yr, yi, true);
        auto& t = set->level[(std::size_t) k];
        t.resize ((std::size_t) hqSize + 2);
        for (std::size_t i = 0; i < (std::size_t) hqSize; ++i)
            t[i] = (float) (yr[i] / (double) hqSize);
        t[(std::size_t) hqSize] = t[0], t[(std::size_t) hqSize + 1] = t[1];
    }
    return set;
}

std::shared_ptr<const HqSet> hqFixed (int s)
{
    static const std::array<std::shared_ptr<const HqSet>, 4> sets = [] {
        std::array<std::shared_ptr<const HqSet>, 4> a;
        for (int k = 1; k <= 4; ++k)
        {
            std::array<float, 2048> base {};
            for (int i = 0; i < 2048; ++i)
                base[(std::size_t) i] = hqShape (k, (float) ((double) i * (1.0 / 2048.0)));
            a[(std::size_t) (k - 1)] = buildHq (base);
        }
        return a;
    }();
    return sets[(std::size_t) std::clamp (s, 1, 4) - 1];
}

// FUN_1800014f0 (Dispatcher case 3): the custom wave averaged by 8 into 2048 points
std::shared_ptr<const HqSet> hqCustom (const std::vector<float>& custom)
{
    if (custom.size() != (std::size_t) tableSize)
        return {};
    std::array<float, 2048> base {};
    for (int k = 0; k < 2048; ++k)
    {
        const auto* v = custom.data() + 8 * k; // (the DLL's order of the eight additions)
        const auto sum = ((((((v[2] + v[6]) + v[0]) + v[4]) + v[3]) + v[7]) + v[1]) + v[5];
        base[(std::size_t) k] = sum * 0.125f;
    }
    return buildHq (base);
}

// ------------------------------------------------------------------------------------------------------- engine

const double ln2Cents = (std::log (2.0) / 12.0) / 100.0; // 0x7a24a0
constexpr double c5Phase = 2247346493527.166;           // DAT_00738540: 523.2511306…·2^32

struct Osc
{
    std::int32_t cents = 0, det = 0, pan = 0;
    std::uint32_t spo = 0;
    const float* tab = nullptr;
    bool noise = false;
    bool aa = false; // P+0x1a1: neither noise nor the sine table — the host's HQ render oversamples it (FUN_0073a000)
    float g = 0, gL = 0, gR = 0;
};

struct Voice
{
    std::array<std::uint32_t, 3> phL {}, phR {};
    bool stereo = false, ended = false;
    float fpitch = 0, fpitchBase = 0, fvol = 1, fpan = 0;
    int key = 60;
    std::uint64_t serial = 0;
    flvp::Proc proc;
    HostSlide slide; // a slide note moving Final.Pitch (issue #14)
};

class Engine
{
public:
    void init (const Params& params, double sampleRate, const std::uint8_t* snapshot, int ppq, double tempo)
    {
        P = params;
        fs = (int) std::lround (sampleRate);
        rate = (float) (c5Phase / (double) fs);
        std::memcpy (snap.data(), snapshot, snap.size());
        timing (ppq, tempo);
        S.setRate ((double) fs); // FUN_005f8470 (Dispatcher 4): the library's rate ratio and ramp limit
        update();
    }

    void timing (int ppq, double tempo) noexcept { S.retime (snap.data(), ppq, tempo); }

    // ProcessParam for every parameter (FUN_00739060): pitch, phase offset, tables, levels (FUN_00738f10) through the
    // host's pan law (FUN_00738ea0 → host slot 31, law A), stereo flag (FUN_00738ff0)
    void update() noexcept
    {
        const auto& T = hostTables();
        const auto& p = P.P;
        for (int k = 0; k < 3; ++k)
        {
            auto& o = osc[(std::size_t) k];
            o.cents = p[(std::size_t) (7 * k + 2)] * 100 + p[(std::size_t) (7 * k + 3)];
            o.spo = (std::uint32_t) p[(std::size_t) (7 * k + 4)] * 0x1ffffffu;
            o.det = p[(std::size_t) (7 * k + 5)];
            o.pan = p[(std::size_t) (7 * k)];
            const auto sh = p[(std::size_t) (7 * k + 1)];
            if (sh >= 0 && sh < 6)
                o.tab = T.t[(std::size_t) sh].data();
            else if (sh >= 6)
                o.tab = custom != nullptr && custom->size() == (std::size_t) tableSize ? custom->data() : T.t[3].data();
            else
                o.tab = T.t[3].data(); // (negative shapes: outside the knob's range)
            o.noise = sh == 5;
            o.aa = ! o.noise && o.tab != T.t[0].data(); // FUN_00739060, parameter kind 2
        }
        const auto g3 = (float) p[13] * 0.0078125f;
        const auto rest = P.am ? 1.0f : 1.0f - g3;
        const auto g2 = (rest * (float) p[6]) * 0.0078125f;
        const auto g1 = rest - g2;
        const float g[3] = { g1, g2, g3 };
        stereo = false;
        randWidth = (std::uint32_t) p[20] << 18;
        for (int k = 0; k < 3; ++k)
        {
            auto& o = osc[(std::size_t) k];
            o.g = P.inv[(std::size_t) k] != 0 ? -g[k] : g[k];
            flvp::panLaw ((float) o.pan * 0.015625f, o.g, o.gL, o.gR);
            stereo = stereo || randWidth != 0 || o.noise || o.spo != 0 || o.det != 0 || o.pan != 0;
        }
    }

    // Delphi's Random (the DLL's RandSeed, an LCG)
    std::uint32_t random (std::uint32_t range) noexcept
    {
        seed = seed * 0x8088405u + 1u;
        return (std::uint32_t) (((std::uint64_t) seed * range) >> 32);
    }

    // FUN_00725970
    std::uint32_t increment (float pitch) const noexcept
    {
        return (std::uint32_t) roundEven ((double) rate * std::exp ((double) pitch * ln2Cents));
    }

    // FUN_00739b80: past the max polyphony, the first unreleased voice (else the last one) starts its end fade
    void steal() noexcept
    {
        if (maxPoly <= 0 || maxPoly > count)
            return;
        auto* v = &voices[(std::size_t) order[0]];
        for (int i = 0; i < count; ++i)
        {
            v = &voices[(std::size_t) order[(std::size_t) i]];
            if (! v->proc.released)
                break;
        }
        v->proc.kill();
    }

    // FUN_00739a40 (TriggerVoice); returns the voice record index
    int trigger (int key, float finalPitch, float finalVol, float finalPan) noexcept
    {
        steal();
        if (count >= maxVoices)
            removeAt (0);
        int slot = 0;
        while (used[(std::size_t) slot])
            ++slot;
        used[(std::size_t) slot] = true;
        auto& v = voices[(std::size_t) slot];
        v = Voice {};
        v.key = key;
        v.serial = ++serials;
        for (int k = 0; k < 3; ++k)
        {
            v.phL[(std::size_t) k] = 0;
            v.phR[(std::size_t) k] = osc[(std::size_t) k].spo;
            if (randWidth != 0)
            {
                v.phL[(std::size_t) k] += (random (randWidth) - (randWidth >> 1)) * 0x100u;
                v.phR[(std::size_t) k] += (random (randWidth) - (randWidth >> 1)) * 0x100u;
            }
        }
        v.stereo = stereo;
        v.fpitch = v.fpitchBase = finalPitch;
        v.slide = {};
        v.fvol = finalVol;
        v.fpan = finalPan;
        v.proc.init (S);
        order[(std::size_t) count++] = slot;
        return slot;
    }

    // Voice_Release (FUN_00739c30 → the processor's FUN_0072c050)
    void release (int slot) noexcept { voices[(std::size_t) slot].proc.release (S); }

    // NewTick (FUN_0073ac60): flags the next render's articulator step; global LFOs advance once listed
    void newTick() noexcept
    {
        tickPending = true;
        if (listed)
            S.advanceGlobal();
    }

    // FUN_0073a230 (Voice_Render, non-HQ paths): the three oscillators into x (interleaved stereo)
    void voiceRender (Voice& v, float* x, int n) noexcept
    {
        X87Extended guard;
        const auto fp = v.fpitch;
        for (int side = 0; side < 2; ++side)
        {
            if (side == 1)
            {
                if (! (v.stereo || stereo))
                    break;
                v.stereo = true;
            }
            auto& ph = side == 0 ? v.phL : v.phR;
            bool first = true;
            for (int k = 0; k < 3; ++k)
            {
                const auto& o = osc[(std::size_t) k];
                std::uint32_t inc;
                if (o.noise)
                    inc = random (0x99326) + 0x99326u;
                else
                {
                    auto p = fp + (float) o.cents;
                    if (side == 1)
                        p = p + (float) o.det;
                    inc = increment (p);
                }
                auto phase = ph[(std::size_t) k];
                if (bitsOf (o.g) != 0 && P.hq) // FUN_00739e90 → engine_x64.dll (modes 10/11/12)
                {
                    const auto am = k == amIndex();
                    ph[(std::size_t) k] = hqRender (k, x + side, n, phase, inc, side == 0 ? o.gL : o.gR, am, first && ! am);
                    if (! am)
                        first = false;
                }
                else if (bitsOf (o.g) != 0 && hostHq && o.aa && k != amIndex())
                {
                    // FUN_0073a000 (FL's HQ render, Dispatcher 1 bit 1): k = max(inc >> 22, 1) copies of the
                    // oscillator at gain g/k (float), the first at the stored phase (mode 11 when it is the first
                    // written, else 10; its end phase is kept), copy j at that phase + j·(inc/k) (int division)
                    const auto copies = std::max<std::uint32_t> (inc >> 22, 1u);
                    const auto g = (ld) ((side == 0 ? o.gL : o.gR) / (float) copies);
                    const auto step = (std::uint32_t) ((std::int32_t) inc / (std::int32_t) copies);
                    float* y = x + side;
                    auto start = phase;
                    for (std::uint32_t c = 0; c < copies; ++c)
                    {
                        auto q = start;
                        if (first)
                        {
                            for (int i = 0; i < n; ++i, q += inc)
                                y[2 * i] = (float) ((ld) o.tab[q >> 18] * g);
                            first = false;
                        }
                        else
                            for (int i = 0; i < n; ++i, q += inc)
                                y[2 * i] = (float) ((ld) o.tab[q >> 18] * g + (ld) y[2 * i]);
                        if (c == 0)
                            ph[(std::size_t) k] = q;
                        start += step;
                    }
                }
                else if (bitsOf (o.g) != 0)
                {
                    const auto g = (ld) (side == 0 ? o.gL : o.gR);
                    float* y = x + side;
                    if (k == amIndex()) // mode 12: acc − (t·g)·acc
                        for (int i = 0; i < n; ++i, phase += inc)
                        {
                            const auto acc = (ld) y[2 * i];
                            y[2 * i] = (float) (acc - ((ld) o.tab[phase >> 18] * g) * acc);
                        }
                    else if (first) // mode 11: t·g
                    {
                        for (int i = 0; i < n; ++i, phase += inc)
                            y[2 * i] = (float) ((ld) o.tab[phase >> 18] * g);
                        first = false;
                    }
                    else // mode 10: t·g + acc
                        for (int i = 0; i < n; ++i, phase += inc)
                            y[2 * i] = (float) ((ld) o.tab[phase >> 18] * g + (ld) y[2 * i]);
                    ph[(std::size_t) k] = phase;
                }
                else
                    ph[(std::size_t) k] = phase + inc * (std::uint32_t) n;
            }
        }
        if (! (v.stereo || stereo))
        {
            v.phR = v.phL;
            for (int i = 0; i < n; ++i)
                x[2 * i + 1] = x[2 * i];
        }
    }

    // Gen_Render (FUN_0073ab70): voices through the processor, mixed into l/r (overwrites)
    void render (float* l, float* r, int n) noexcept
    {
        std::fill (l, l + n, 0.0f);
        std::fill (r, r + n, 0.0f);
        if (count > 0)
            listed = true;
        const auto tick = tickPending;
        tickPending = false;
        if ((int) bufA.size() < 2 * n)
            return; // (prepared for at most `prepareBlock`)
        for (int i = 0; i < count; ++i)
        {
            auto& v = voices[(std::size_t) order[(std::size_t) i]];
            auto& pr = v.proc;
            if (tick || pr.first > 0)
            {
                pr.control (S);
                v.fpitch = v.fpitchBase + pr.pitch; // FL's host restores Final.Pitch every tick (inference, REA_rodada42)
            }
            voiceRender (v, bufA.data(), n);
            if (flvp::mixVoice (S, pr, bufA.data(), bufB.data(), n, v.fvol, v.fpan, l, r))
                v.ended = true;
        }
        // the plugin asks the host to kill finished voices, last to first; the host's kill removes them at once
        for (int i = count - 1; i >= 0; --i)
            if (voices[(std::size_t) order[(std::size_t) i]].ended)
                removeAt (i);
    }

    // ordered list (FUN_004e9960 → FUN_004e9470): the following voices move up
    void removeAt (int i) noexcept
    {
        if (i < 0 || i >= count)
            return;
        used[(std::size_t) order[(std::size_t) i]] = false;
        for (int j = i; j + 1 < count; ++j)
            order[(std::size_t) j] = order[(std::size_t) (j + 1)];
        --count;
    }

    void reset() noexcept
    {
        count = 0;
        used.fill (false);
        listed = false, tickPending = false;
        for (auto& r : S.recs)
            r.globalPhase = 0;
    }

    void prepareBlock (int maxBlock)
    {
        const auto m = (std::size_t) std::max (maxBlock, 1);
        bufA.assign (2 * m, 0.0f);
        bufB.assign (2 * m, 0.0f);
    }

    int amIndex() const noexcept { return P.am ? 2 : -1; } // FUN_007389d0: only osc 3 modulates

    // engine_x64.dll FUN_180001660: one oscillator into y (stride 2); returns the new phase. Sine through sinf, noise
    // from the DLL's own LCG, the other shapes from the band-limited level of the oscillator's frequency (below 0.0005
    // cycles per sample: the analytic shape), linear interpolation; float arithmetic (SSE)
    std::uint32_t hqRender (int k, float* y, int n, std::uint32_t phase, std::uint32_t inc, float g, bool am, bool write) noexcept
    {
        auto shape = P.P[(std::size_t) (7 * k + 1)];
        if (shape > 5 && hqCustomSet == nullptr)
            shape = 3; // FUN_00739610
        if (write)
            for (int i = 0; i < n; ++i)
                y[2 * i] = 0.0f;
        auto out = [&] (int i, float v) { y[2 * i] = am ? y[2 * i] - (y[2 * i] * v) * g : v * g + y[2 * i]; };
        if (shape == 0)
        {
            for (int i = 0; i < n; ++i, phase += inc)
                out (i, std::sin ((float) ((double) phase * 2.3283064365386963e-10) * 6.28318548f));
            return phase;
        }
        if (shape == 5)
        {
            for (int i = 0; i < n; ++i)
            {
                hqSeed = hqSeed * 0xbb38435u + 0x3619636bu;
                out (i, (float) (std::int32_t) hqSeed * 4.65661287e-10f);
            }
            return phase + inc * (std::uint32_t) n;
        }
        const auto* set = shape == 6 ? hqCustomSet.get() : hqFixed (shape).get();
        const auto f = (double) inc * 2.3283064365386963e-10;
        auto lv = f > 0.0 ? (float) (std::log10 ((0.1223091976516634 / (1.0 / (double) fs)) * f) * 13.287712379549449 - 5.0) : -1e30f;
        auto level = 0.0f;
        if (lv >= 0.0f)
            level = std::min ((float) hqLevels - 2.0f, lv);
        const auto& T = set->level[(std::size_t) (int) level];
        for (int i = 0; i < n; ++i, phase += inc)
        {
            const auto xf = (float) ((double) phase * 2.3283064365386963e-10);
            float v;
            if (! (0.0005 <= f))
                v = shape == 6 ? 0.0f : hqShape (shape, xf); // (the custom wave below the threshold: not modelled)
            else
            {
                const auto xx = (double) (xf * 32768.0f);
                const auto j = (int) xx;
                const auto fr = (float) (xx - (double) j);
                v = (1.0f - fr) * T[(std::size_t) j] + fr * T[(std::size_t) j + 1];
            }
            out (i, v);
        }
        return phase;
    }

    Params P;
    bool hostHq = false; // FL's "HQ render" host mode (P+0x1fd): set while LYNX exports, as FL's renders do
    flvp::Settings S;
    std::array<std::uint8_t, snapshotSize> snap = flvp::defaultSnapshot();
    std::shared_ptr<const std::vector<float>> custom; // the channel's sample as a custom wave (16384 points), if any
    std::shared_ptr<const HqSet> hqCustomSet;
    std::uint32_t hqSeed = 0x1099e; // DAT_180024a1c (one per process in FL)
    std::array<Osc, 3> osc {};
    int fs = 44100, maxPoly = 0;
    float rate = 0;
    std::uint32_t randWidth = 0, seed = 0;
    bool stereo = false, listed = false, tickPending = false;
    std::array<Voice, maxVoices> voices {};
    std::array<bool, maxVoices> used {};
    std::array<int, maxVoices> order {};
    int count = 0;
    std::uint64_t serials = 0;
    std::vector<float> bufA, bufB;
};

// ------------------------------------------------------------------------------------------------------- custom wave

std::vector<std::uint8_t> readFileBytes (std::string path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        std::replace (path.begin(), path.end(), '\\', '/');
        in.open (path, std::ios::binary);
    }
    if (! in)
        return {};
    return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
}

std::uint32_t le32 (const std::uint8_t* p) noexcept { return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24; }

// mono (mean of the channels) samples of a PCM (8/16/24/32-bit) or float WAV file
std::vector<float> readWaveMono (const std::vector<std::uint8_t>& b)
{
    if (b.size() < 12 || std::memcmp (b.data(), "RIFF", 4) != 0 || std::memcmp (b.data() + 8, "WAVE", 4) != 0)
        return {};
    int tag = 0, ch = 0, bits = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    for (std::size_t i = 12; i + 8 <= b.size();)
    {
        const auto n = (std::size_t) le32 (b.data() + i + 4);
        if (std::memcmp (b.data() + i, "fmt ", 4) == 0 && i + 8 + 16 <= b.size())
        {
            tag = b[i + 8] | b[i + 9] << 8;
            ch = b[i + 10] | b[i + 11] << 8;
            bits = b[i + 22] | b[i + 23] << 8;
            if (tag == 0xfffe && n >= 26 && i + 8 + 26 <= b.size())
                tag = b[i + 8 + 24] | b[i + 8 + 25] << 8; // extensible: the sub-format's tag
        }
        else if (std::memcmp (b.data() + i, "data", 4) == 0)
        {
            data = b.data() + i + 8;
            dataSize = std::min (n, b.size() - (i + 8));
        }
        i += 8 + n + (n & 1);
    }
    if (data == nullptr || ch <= 0 || bits <= 0 || bits % 8 != 0)
        return {};
    const auto bytes = (std::size_t) bits / 8, frame = bytes * (std::size_t) ch;
    const auto frames = dataSize / frame;
    std::vector<float> x (frames);
    for (std::size_t k = 0; k < frames; ++k)
    {
        double sum = 0.0;
        for (int c = 0; c < ch; ++c)
        {
            const auto* s = data + k * frame + (std::size_t) c * bytes;
            double v = 0.0;
            if (tag == 3 && bits == 32)
            {
                float f;
                std::memcpy (&f, s, 4);
                v = f;
            }
            else if (tag == 3 && bits == 64)
            {
                double f;
                std::memcpy (&f, s, 8);
                v = f;
            }
            else if (tag == 1 && bits == 8)
                v = ((double) s[0] - 128.0) / 128.0;
            else if (tag == 1 && bits == 16)
                v = (double) (std::int16_t) (s[0] | s[1] << 8) / 32768.0;
            else if (tag == 1 && bits == 24)
                v = (double) ((std::int32_t) ((std::uint32_t) s[0] << 8 | (std::uint32_t) s[1] << 16 | (std::uint32_t) s[2] << 24) >> 8) / 8388608.0;
            else if (tag == 1 && bits == 32)
                v = (double) (std::int32_t) le32 (s) / 2147483648.0;
            else
                return {};
            sum += v;
        }
        x[k] = (float) (sum / ch);
    }
    return x;
}
} // namespace

// ------------------------------------------------------------------------------------------------------- public

bool threeOscStateUsable (const std::vector<std::uint8_t>& state)
{
    Params p;
    return parse (state, p, nullptr);
}

std::vector<std::uint8_t> threeOscNormalisedState (const std::vector<std::uint8_t>& state, const std::vector<std::uint8_t>& snapshot)
{
    Params p;
    std::vector<std::uint8_t> own;
    if (! parse (state, p, &own))
        return {};
    std::vector<std::uint8_t> out ((std::size_t) stateSize, 0);
    const std::int32_t version = 14;
    std::memcpy (out.data(), &version, 4);
    std::memcpy (out.data() + 4, p.P.data(), 4 * (std::size_t) paramCount);
    for (int k = 0; k < 3; ++k)
        out[0x58 + (std::size_t) k] = p.inv[(std::size_t) k];
    out[0x5b] = p.am ? 1 : 0;
    out[0x5c] = p.hq ? 1 : 0;
    if (snapshot.size() >= (std::size_t) snapshotSize)
        std::memcpy (out.data() + 0x60, snapshot.data(), (std::size_t) snapshotSize);
    else if (own.size() == (std::size_t) snapshotSize)
        std::memcpy (out.data() + 0x60, own.data(), (std::size_t) snapshotSize);
    else
        std::memcpy (out.data() + 0x60, flvp::defaultSnapshot().data(), (std::size_t) snapshotSize);
    return out;
}

int threeOscParamCount() noexcept { return paramCount + extraCount; }

std::pair<int, int> threeOscParamRange (int index) noexcept { return rangeOf (index); }

std::optional<int> threeOscParam (const std::vector<std::uint8_t>& state, int index)
{
    Params p;
    if (! parseNormalised (state, p) || index < 0 || index >= paramCount + extraCount)
        return std::nullopt;
    if (index < paramCount)
        return p.P[(std::size_t) index];
    if (index < paramCount + 3)
        return p.inv[(std::size_t) (index - paramCount)] != 0 ? 1 : 0;
    return index == paramCount + 3 ? (p.am ? 1 : 0) : (p.hq ? 1 : 0);
}

bool threeOscSetParam (std::vector<std::uint8_t>& state, int index, int value)
{
    Params p;
    if (! parseNormalised (state, p) || index < 0 || index >= paramCount + extraCount)
        return false;
    const auto [lo, hi] = rangeOf (index);
    value = std::clamp (value, lo, hi);
    if (index < paramCount)
    {
        const auto v = (std::int32_t) value;
        std::memcpy (state.data() + 4 + 4 * (std::size_t) index, &v, 4);
    }
    else
        state[0x58 + (std::size_t) (index - paramCount)] = (std::uint8_t) value;
    return true;
}

std::vector<float> threeOscCustomWave (const std::string& wavPath)
{
    const auto x = readWaveMono (readFileBytes (wavPath));
    if (x.empty())
        return {};
    // FL's host resamples the channel's whole sample to 16384 points (FLEngine FUN_00694c80, interpolation mode 6, not
    // decoded) before handing it over (Dispatcher 10); here: linear interpolation over the sample's length
    std::vector<float> w ((std::size_t) tableSize);
    const auto len = x.size();
    for (std::size_t i = 0; i < (std::size_t) tableSize; ++i)
    {
        const auto pos = (double) i * (double) len / (double) tableSize;
        const auto i0 = std::min ((std::size_t) pos, len - 1);
        const auto i1 = std::min (i0 + 1, len - 1);
        const auto f = pos - (double) i0;
        w[i] = (float) ((1.0 - f) * x[i0] + f * x[i1]);
    }
    return w;
}

namespace
{
// ------------------------------------------------------------------------------------------------------- instrument

struct Setup
{
    Params P;
    std::array<std::uint8_t, snapshotSize> snap = flvp::defaultSnapshot();
    std::shared_ptr<const std::vector<float>> custom;
    std::shared_ptr<const HqSet> hq;
    int ppq = 96;
    double tempo = 140.0;
};

class ThreeOscInstrument final : public Instrument
{
public:
    bool (*hostHq)() noexcept = nullptr; // FL's HQ-render host mode while this returns true (checked on prepare)

    ThreeOscInstrument()
    {
        engine.prepareBlock (blockSize);
        engine.init (Params {}, 44100.0, flvp::defaultSnapshot().data(), 96, 140.0);
    }

    ~ThreeOscInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        rate = sr;
        take();
        engine.custom = current.custom, engine.hqCustomSet = current.hq;
        engine.hostHq = hostHq != nullptr && hostHq();
        engine.init (current.P, sr, current.snap.data(), current.ppq, current.tempo);
        engine.reset();
        tickPhase = 0.0;
        lastTicks = -1.0;
    }

    // UI thread: the parsed state goes through a mailbox (the audio thread takes it, this thread frees it)
    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        maxPoly.store (channel.synth.mono ? 1 : 0);
        const auto ppq = channel.modelParams.empty() ? 96 : std::clamp ((int) channel.modelParams[0], 1, 1 << 16);
        const auto tempo = bpm > 0.0 ? bpm : 140.0;
        if (channel.pluginState == state && ppq == lastPpq && tempo == lastTempo && channel.samplePath == wavePath)
            return;
        Params p;
        if (! parseNormalised (channel.pluginState, p))
            return;
        auto s = std::make_unique<Setup>();
        s->P = p;
        std::memcpy (s->snap.data(), channel.pluginState.data() + 0x60, snapshotSize);
        s->ppq = ppq, s->tempo = tempo;
        if (channel.samplePath == wavePath)
            s->custom = uiCustom;
        else if (auto w = channel.samplePath.empty() ? std::vector<float> {} : threeOscCustomWave (channel.samplePath); ! w.empty())
            s->custom = std::make_shared<const std::vector<float>> (std::move (w));
        uiCustom = s->custom;
        if (p.hq)
            hqFixed (1); // (builds the band-limited tables here, not on the audio thread)
        if (p.hq && s->custom != nullptr)
            s->hq = hqCustom (*s->custom); // (the custom wave's band-limited tables: only for HQ states)
        state = channel.pluginState, lastPpq = ppq, lastTempo = tempo, wavePath = channel.samplePath;
        delete incoming.exchange (s.release());
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        const auto to = (float) (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents);
        for (int i = 0; i < engine.count; ++i)
        {
            auto& v = engine.voices[(std::size_t) engine.order[(std::size_t) i]];
            if (! v.proc.released)
                v.slide.start (to - (v.fpitchBase - v.slide.cents), samples);
        }
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (float) ((std::pow (11.0, 1.28 * vel) - 1.0) / 10.0);
        const auto k = std::clamp (key, 0, 127);
        const auto finalPitch = (float) (100.0 * (k - 60) + (double) pitchCents.load() + (double) nextCents);
        const auto slot = engine.trigger (k, finalPitch, (float) (channelVol * (double) initVol),
                                          (float) std::clamp ((double) nextPan + channelPan, -1.0, 1.0));
        volFactor[(std::size_t) slot] = initVol;
        notePan[(std::size_t) slot] = nextPan;
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        for (int i = 0; i < engine.count; ++i)
        {
            const auto slot = engine.order[(std::size_t) i];
            const auto& v = engine.voices[(std::size_t) slot];
            if (v.key == key && ! v.proc.released)
                engine.release (slot);
        }
    }

    void allNotesOff() noexcept override
    {
        for (int i = 0; i < engine.count; ++i)
        {
            const auto slot = engine.order[(std::size_t) i];
            if (! engine.voices[(std::size_t) slot].proc.released)
                engine.release (slot);
        }
    }

    bool idle() const noexcept override { return engine.count == 0; }

    // while the transport moves, FL's sequencer ticks fall on the song's PPQ grid
    void setSongPosition (double ticks) noexcept override
    {
        const auto playing = lastTicks >= 0.0 && ticks != lastTicks;
        lastTicks = ticks;
        if (playing)
            tickPhase = ticks / (double) ppq * (double) current.ppq;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        engine.maxPoly = maxPoly.load();
        // FL's channel volume/pan from LYNX's gains (gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…)); FL recomputes the
        // voices' FinalLevels when the channel level changes
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int i = 0; i < engine.count; ++i)
            {
                const auto slot = engine.order[(std::size_t) i];
                auto& v = engine.voices[(std::size_t) slot];
                v.fvol = (float) (vol * (double) volFactor[(std::size_t) slot]);
                v.fpan = (float) std::clamp ((double) notePan[(std::size_t) slot] + pan, -1.0, 1.0);
            }
        }
        // FL's sequencer ticks (PPQ per beat): a tick boundary starts a new host block, NewTick first
        const auto perSample = current.tempo * (double) current.ppq / (60.0 * rate);
        for (int done = 0; done < n;)
        {
            if (std::fabs (tickPhase - std::round (tickPhase)) < 1e-9)
            {
                tickPhase = std::round (tickPhase);
                engine.newTick();
            }
            const auto next = std::floor (tickPhase) + 1.0;
            const auto toNext = std::max (1, (int) std::ceil ((next - tickPhase) / perSample - 1e-9));
            const auto len = std::min ({ blockSize, n - done, toNext });
            engine.render (bufL.data(), bufR.data(), len);
            for (int i = 0; i < engine.count; ++i) // a slide note moves the voices' Final.Pitch (issue #14)
            {
                auto& v = engine.voices[(std::size_t) engine.order[(std::size_t) i]];
                if (v.slide.moving())
                {
                    const auto was = v.slide.cents;
                    const auto d = v.slide.advance ((float) len) - was;
                    v.fpitchBase += d;
                    v.fpitch += d;
                }
            }
            for (int j = 0; j < len; ++j)
            {
                left[done + j] += bufL[(std::size_t) j];
                right[done + j] += bufR[(std::size_t) j];
            }
            tickPhase = len == toNext ? next : tickPhase + (double) len * perSample;
            done += len;
        }
    }

private:
    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            current = *s;
            engine.P = current.P;
            engine.custom = current.custom, engine.hqCustomSet = current.hq;
            engine.hostHq = hostHq != nullptr && hostHq();
            std::memcpy (engine.snap.data(), current.snap.data(), snapshotSize);
            engine.timing (current.ppq, current.tempo); // (FL: parameters and tempo reach the plugin while voices play)
            engine.update();
            retired.store (s);
        }
    }

    static constexpr int blockSize = 512;
    Engine engine;
    Setup current;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::atomic<float> pitchCents { 0.0f };
    std::atomic<int> maxPoly { 0 };
    std::vector<std::uint8_t> state;
    std::string wavePath;
    std::shared_ptr<const std::vector<float>> uiCustom;
    int lastPpq = -1;
    double lastTempo = -1.0;
    std::array<float, blockSize> bufL {}, bufR {};
    std::array<float, maxVoices> volFactor {}, notePan {};
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0, tickPhase = 0.0, lastTicks = -1.0;
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

std::unique_ptr<Instrument> makeThreeOscInstrument (bool (*hostHq)() noexcept)
{
    auto i = std::make_unique<ThreeOscInstrument>();
    i->hostHq = hostHq;
    return i;
}

namespace threeosc
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, const RefOptions& opt, const std::vector<float>& custom)
{
    Params P;
    if (! parseNormalised (state, P) || length <= 0)
        return {};
    auto engine = std::make_unique<Engine>();
    if (custom.size() == (std::size_t) tableSize)
    {
        engine->custom = std::make_shared<const std::vector<float>> (custom);
        if (P.hq)
            engine->hqCustomSet = hqCustom (custom);
    }
    engine->init (P, sampleRate, state.data() + 0x60, opt.ppq, opt.tempo);
    engine->maxPoly = opt.maxPoly;
    engine->seed = opt.seed;
    const auto block = std::max (1, opt.block);
    engine->prepareBlock (block);
    std::vector<int> slot (notes.size(), -1);
    std::vector<std::uint64_t> serial (notes.size(), 0);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r ((std::size_t) block);
    // events at a sample in the model's order: sorted by sample, then in list order (stable)
    std::vector<std::pair<int, int>> order; // (sample, 2·note + isOff)
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        order.push_back ({ notes[i].start, 2 * (int) i });
        if (notes[i].release >= 0)
            order.push_back ({ notes[i].release, 2 * (int) i + 1 });
    }
    std::stable_sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    int n0 = 0;
    std::size_t next = 0;
    while (n0 < length)
    {
        auto ln = std::min (block, length - n0);
        for (const auto& e : order)
            if (n0 < e.first && e.first < n0 + ln)
                ln = e.first - n0;
        if (opt.tick > 0)
        {
            const auto nt = (n0 / opt.tick + 1) * opt.tick;
            if (nt < n0 + ln)
                ln = nt - n0;
            if (n0 % opt.tick == 0)
            {
                for (int i = 0; i < engine->count; ++i) // the host restores Final.Pitch before the tick
                {
                    auto& v = engine->voices[(std::size_t) engine->order[(std::size_t) i]];
                    v.fpitch = v.fpitchBase;
                }
                engine->newTick();
            }
        }
        while (next < order.size() && order[next].first < n0)
            ++next;
        while (next < order.size() && order[next].first == n0)
        {
            const auto i = (std::size_t) (order[next].second / 2);
            const auto& nt = notes[i];
            if (order[next].second % 2 == 0)
            {
                slot[i] = engine->trigger (nt.key, (float) (100.0 * nt.key - 6000.0 + nt.pitchOffset), (float) nt.finalVol, (float) nt.finalPan);
                serial[i] = engine->voices[(std::size_t) slot[i]].serial;
            }
            else if (slot[i] >= 0 && engine->used[(std::size_t) slot[i]] && engine->voices[(std::size_t) slot[i]].serial == serial[i])
                engine->release (slot[i]);
            ++next;
        }
        engine->render (l.data(), r.data(), ln);
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace threeosc
} // namespace lynx
