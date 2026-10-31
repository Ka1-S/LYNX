#include "core/Morphine.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

// 1:1 port of our round-39 reference model (r39/py/mormodel.py + morpatch.py, morfx.py). The model mixes Python floats
// (doubles) rounded to float32 at chosen places with F(), and numpy float32 arrays (operations with Python floats stay
// float32, NEP 50). This file keeps those types — `double` + F() where the model holds Python floats, `float` where it
// holds float32 — and the operation order of every expression, so contraction into FMAs stays off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr double twoPi = 6.283185307179586;
constexpr int partials = 128;
constexpr int voiceCount = 32;
constexpr double tinyD = 9.999999717180685e-10; // DAT_180176f78 / DAT_1801773c4 (flush-to-zero band)

double F (double x) noexcept { return (double) (float) x; } // round to float32, keep as a Python float
double flush (double x) noexcept { return (-tinyD < x && x < tinyD) ? 0.0 : x; }
double clampD (double x, double lo, double hi) noexcept { return std::min (std::max (x, lo), hi); }
int pyInt (double x) noexcept // Python int() of a float (truncation), saturated
{
    if (! (x == x))
        return 0;
    return (int) std::clamp (x, -2147483648.0, 2147483647.0);
}
long long pyMod (long long a, long long b) noexcept // Python's % (sign of the divisor)
{
    if (b == 0)
        return 0;
    auto r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}

// ------------------------------------------------------------------------------------------------------- state

struct Frame
{
    int dur = 1;
    double gain = 0.0;
    std::array<float, partials> amp {}, pan {}, det {}; // MBP0 [0] amplitude (cube root), [1] pan offset, [2] detune
};
struct ZoneOpts
{
    int o40 = 0, o44 = 0, loopMode = 0;
    bool relLoop = false, tsync = false;
    double rate = 0.0, keytrk = 0.0, veltrk = 0.0;
    std::array<float, partials> arr60 {}, arr68 {}; // per-partial velocity sensitivity, per-partial gain
};
struct Zone
{
    int key = 1;
    double level = 0.0;
    std::vector<Frame> frames;
    ZoneOpts opt;
};
struct Gen
{
    std::vector<Zone> zones;
    double pwmRate = 0, pwmMode = 0, pwmPhase = 0, detuneRange = 0, panRange = 0;
    bool pwmFree = false;
};
struct MorphLine
{
    std::vector<std::array<double, 3>> pts; // X, Y, duration
    int o40 = 0, o44 = 0, loopMode = 0, o80 = 0;
    bool tsync = false;
    double mix[4] {}, pan[4] {}, rate = 0, keytrk = 0, veltrk = 0;
    bool mute[4] {};
};
struct Env
{
    std::vector<std::array<double, 2>> pts; // level, duration
    int o40 = 0, o44 = 0, loopMode = 0;
    bool relLoop = false, tsync = false, o49 = false;
    double velocity = 0, rate = 0, veltrk = 0, keytrk = 0;
};
struct Slot
{
    int src = 0, dst = 0;
    double amt = 0;
};
struct Patch
{
    Gen gens[4];
    MorphLine morph;
    std::vector<Env> envs;
    std::vector<Slot> matrix;
    double A[40] {};
    double f6a8 = 0, f6ac = 0, f6b0 = 0, f6b4 = 0, f6b8 = 0, f6bc = 0, f6c0 = 0;
    std::string noise, name;
    int globalOffset = -1; // offset of A[0] inside the 0xD5 blob
};

struct Reader
{
    const std::uint8_t* d;
    std::size_t n;
    void need (std::size_t o, std::size_t k) const
    {
        if (o > n || k > n - o)
            throw std::runtime_error ("Morphine state truncated");
    }
    std::int32_t i32 (std::size_t o) const
    {
        need (o, 4);
        std::int32_t v;
        std::memcpy (&v, d + o, 4);
        return v;
    }
    double f32 (std::size_t o) const
    {
        need (o, 4);
        float v;
        std::memcpy (&v, d + o, 4);
        return (double) v;
    }
    void tag (std::size_t o, const char* t) const // chunk tags are stored reversed ("0roM" for Mor0)
    {
        need (o, 4);
        for (int i = 0; i < 4; ++i)
            if (d[o + (std::size_t) i] != (std::uint8_t) t[3 - i])
                throw std::runtime_error ("Morphine tag");
    }
    // `size` bytes from o, zero-padded to `pad`
    std::vector<std::uint8_t> block (std::size_t o, std::int32_t size, std::size_t pad) const
    {
        if (size < 0)
            throw std::runtime_error ("Morphine size");
        need (o, (std::size_t) size);
        std::vector<std::uint8_t> b (std::max ((std::size_t) size, pad), 0);
        std::memcpy (b.data(), d + o, (std::size_t) size);
        return b;
    }
};
double cl01 (double x) { return std::min (std::max (x, 0.0), 1.0); }
std::size_t at (std::size_t o, std::int32_t sz)
{
    if (sz < 0)
        throw std::runtime_error ("Morphine size");
    return o + (std::size_t) sz;
}
std::int32_t wi (const std::vector<std::uint8_t>& b, std::size_t o) { std::int32_t v; std::memcpy (&v, b.data() + o, 4); return v; }
double wf (const std::vector<std::uint8_t>& b, std::size_t o) { float v; std::memcpy (&v, b.data() + o, 4); return (double) v; }

void mbp (const Reader& r, std::size_t o, std::int32_t n, Frame& fr)
{
    r.tag (o, "MBP0");
    const auto e = at (o, n);
    o += 4;
    std::array<float, partials>* arrs[3] = { &fr.amp, &fr.pan, &fr.det };
    for (auto* a : arrs)
    {
        int k = 0;
        while (k < partials)
        {
            if (o >= e)
                return;
            r.need (o, 1);
            const auto m = r.d[o++];
            for (int b = 0; b < 8; ++b)
            {
                if ((m >> b) & 1)
                {
                    const auto v = (float) r.f32 (o);
                    if (k < partials)
                        (*a)[(std::size_t) k] = v;
                    o += 4;
                }
                ++k;
            }
        }
    }
}
void msp (const Reader& r, std::size_t o, std::int32_t n, Zone& z)
{
    r.tag (o, "MSP0");
    const auto e = at (o, n);
    const auto cnt = r.i32 (o + 4);
    o += 8;
    for (int k = 0; k < cnt; ++k)
    {
        Frame fr;
        fr.dur = std::max (1, r.i32 (o));
        fr.gain = cl01 (r.f32 (o + 4));
        const auto sz = r.i32 (o + 8);
        mbp (r, o + 12, sz, fr);
        o = at (o + 12, sz);
        z.frames.push_back (fr);
    }
    const auto sz = r.i32 (o);
    const auto opt = r.block (o, sz, 0x424);
    o = at (o, sz);
    if (o != e)
        throw std::runtime_error ("MSP0 size");
    auto& so = z.opt;
    so.o40 = wi (opt, 4), so.o44 = wi (opt, 8), so.relLoop = wi (opt, 12) != 0, so.loopMode = wi (opt, 16);
    so.rate = wf (opt, 20), so.keytrk = wf (opt, 24), so.veltrk = wf (opt, 28), so.tsync = wi (opt, 32) != 0;
    for (int k = 0; k < partials; ++k)
    {
        so.arr60[(std::size_t) k] = (float) wf (opt, 36 + 4 * (std::size_t) k);
        so.arr68[(std::size_t) k] = (float) wf (opt, 548 + 4 * (std::size_t) k);
    }
}
void mzh (const Reader& r, std::size_t o, std::int32_t n, Gen& g)
{
    r.tag (o, "MZH0");
    const auto e = at (o, n);
    const auto cnt = r.i32 (o + 4);
    o += 8;
    for (int k = 0; k < cnt; ++k)
    {
        Zone z;
        z.key = std::max (1, r.i32 (o));
        z.level = cl01 (r.f32 (o + 4));
        const auto sz = r.i32 (o + 8);
        msp (r, o + 12, sz, z);
        o = at (o + 12, sz);
        g.zones.push_back (std::move (z));
    }
    if (o != e)
        throw std::runtime_error ("MZH0 size");
}
void mvc (const Reader& r, std::size_t o, std::int32_t n, Gen& g)
{
    r.tag (o, "MVC0");
    const auto e = at (o, n);
    const auto sz = r.i32 (o + 4);
    mzh (r, o + 8, sz, g);
    o = at (o + 8, sz);
    const auto osz = r.i32 (o);
    const auto opt = r.block (o, osz, 28);
    auto bitsF = [&] (std::size_t off) { return wf (opt, off); };
    g.pwmRate = bitsF (4);
    g.pwmMode = wi (opt, 8) * 0.5;
    g.pwmPhase = bitsF (12);
    g.pwmFree = wi (opt, 16) != 0;
    g.detuneRange = bitsF (20);
    g.panRange = bitsF (24);
    o = at (o, osz);
    if (o != e)
        throw std::runtime_error ("MVC0 size");
}
void mml (const Reader& r, std::size_t o, std::int32_t n, MorphLine& ml)
{
    r.tag (o, "MML0");
    const auto e = at (o, n);
    const auto cnt = r.i32 (o + 4), step = r.i32 (o + 8);
    o += 12;
    for (int k = 0; k < cnt; ++k)
    {
        ml.pts.push_back ({ cl01 (r.f32 (o)), cl01 (r.f32 (o + 4)), (double) std::max (1, r.i32 (o + 8)) });
        o = at (o, step);
    }
    const auto osz = r.i32 (o);
    const auto raw = r.block (o, osz, 0x54);
    o = at (o, osz);
    if (o != e)
        throw std::runtime_error ("MML0 size");
    ml.o40 = wi (raw, 4), ml.o44 = wi (raw, 8), ml.loopMode = wi (raw, 12), ml.tsync = wi (raw, 16) != 0;
    for (int k = 0; k < 4; ++k)
    {
        ml.mix[k] = wf (raw, 20 + 4 * (std::size_t) k);
        ml.pan[k] = wf (raw, 36 + 4 * (std::size_t) k);
        ml.mute[k] = wi (raw, 52 + 4 * (std::size_t) k) != 0;
    }
    ml.rate = wf (raw, 68), ml.keytrk = wf (raw, 72), ml.veltrk = wf (raw, 76), ml.o80 = wi (raw, 80);
}
void meg (const Reader& r, std::size_t o, std::int32_t n, Env& ev)
{
    r.tag (o, "MEG0");
    const auto e = at (o, n);
    const auto cnt = r.i32 (o + 4), step = r.i32 (o + 8);
    o += 12;
    for (int k = 0; k < cnt; ++k)
    {
        ev.pts.push_back ({ cl01 (r.f32 (o)), (double) std::max (1, r.i32 (o + 4)) });
        o = at (o, step);
    }
    const auto osz = r.i32 (o);
    const auto raw = r.block (o, osz, 0x2c);
    o = at (o, osz);
    if (o != e)
        throw std::runtime_error ("MEG0 size");
    ev.o40 = wi (raw, 4), ev.o44 = wi (raw, 8), ev.relLoop = wi (raw, 12) != 0, ev.velocity = wf (raw, 16), ev.loopMode = wi (raw, 20);
    ev.rate = wf (raw, 24), ev.tsync = wi (raw, 28) != 0, ev.veltrk = wf (raw, 32), ev.keytrk = wf (raw, 36), ev.o49 = wi (raw, 40) != 0;
}
void mmd (const Reader& r, std::size_t o, std::int32_t n, Patch& p)
{
    r.tag (o, "MMD0");
    const auto e = at (o, n);
    const auto cnt = r.i32 (o + 4);
    o += 8;
    for (int k = 0; k < cnt; ++k)
    {
        const auto sz = r.i32 (o);
        Env ev;
        meg (r, o + 4, sz, ev);
        p.envs.push_back (std::move (ev));
        o = at (o + 4, sz);
    }
    const auto osz = r.i32 (o);
    const auto raw = r.block (o, osz, 0x94);
    o = at (o, osz);
    if (o != e)
        throw std::runtime_error ("MMD0 size");
    for (int k = 0; k < 12; ++k)
        p.matrix.push_back ({ wi (raw, 4 + 4 * (std::size_t) k), wi (raw, 52 + 4 * (std::size_t) k), wf (raw, 100 + 4 * (std::size_t) k) });
}

std::optional<Patch> parseStateThrowing (const std::vector<std::uint8_t>& blob)
{
    if (blob.size() < 8 || std::memcmp (blob.data(), "SSLF", 4) != 0)
        return std::nullopt;
    std::uint32_t n = 0;
    std::memcpy (&n, blob.data() + 4, 4);
    if (blob.size() < 8 + (std::size_t) n)
        return std::nullopt;
    const Reader r { blob.data() + 8, n };
    r.tag (0, "Mor0");
    r.tag (4, "MPR0");
    Patch p;
    std::size_t o = 8;
    for (auto& g : p.gens)
    {
        const auto sz = r.i32 (o);
        mvc (r, o + 4, sz, g);
        o = at (o + 4, sz);
    }
    auto sz = r.i32 (o);
    mml (r, o + 4, sz, p.morph);
    o = at (o + 4, sz);
    sz = r.i32 (o);
    mmd (r, o + 4, sz, p);
    o = at (o + 4, sz);
    const auto bsz = r.i32 (o);
    const auto g = r.block (o, bsz, 0x5c4);
    p.globalOffset = (int) (o + 8 + 4);
    for (int k = 0; k < 40; ++k)
        p.A[k] = wf (g, 4 + 4 * (std::size_t) k);
    p.f6a8 = wf (g, 164);
    p.f6ac = wf (g, 168);
    for (std::size_t i = 172; i < 172 + 1024 && g[i] != 0; ++i)
        p.noise.push_back ((char) g[i]);
    for (std::size_t i = 1200; i < 1200 + 256 && g[i] != 0; ++i)
        p.name.push_back ((char) g[i]);
    p.f6bc = wf (g, 1456), p.f6b0 = wf (g, 1460), p.f6b4 = wf (g, 1464), p.f6b8 = wf (g, 1468), p.f6c0 = wf (g, 1472);
    return p;
}

std::optional<Patch> parseState (const std::vector<std::uint8_t>& blob)
{
    try
    {
        return parseStateThrowing (blob);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// ------------------------------------------------------------------------------------------------------- noise sample

struct NoiseSample
{
    std::vector<float> data[2]; // per channel, length + 4 zeros
    int ch = 1, len = 0, ls = 0, le = 0;
    double sr = 44100.0;
};

std::shared_ptr<const NoiseSample> loadNoise (std::string path)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const NoiseSample>> cache;
    if (path.empty())
        return nullptr;
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (const auto it = cache.find (path); it != cache.end())
            return it->second;
    }
    std::shared_ptr<const NoiseSample> result;
    auto key = path;
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        std::replace (path.begin(), path.end(), '\\', '/');
        in.open (path, std::ios::binary);
    }
    if (in)
    {
        const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        auto u32 = [&] (std::size_t o) { std::uint32_t v; std::memcpy (&v, b.data() + o, 4); return v; };
        std::size_t i = 12;
        int tag = 0, ch = 0, bits = 0;
        std::uint32_t sr = 0;
        const std::uint8_t* data = nullptr;
        std::size_t dataSize = 0;
        bool hasLoop = false;
        std::uint32_t loop0 = 0, loop1 = 0;
        while (b.size() >= 8 && i < b.size() - 8)
        {
            const auto n = (std::size_t) u32 (i + 4);
            if (std::memcmp (b.data() + i, "fmt ", 4) == 0 && i + 24 <= b.size())
            {
                tag = b[i + 8] | b[i + 9] << 8;
                ch = b[i + 10] | b[i + 11] << 8;
                sr = u32 (i + 12);
                bits = b[i + 22] | b[i + 23] << 8;
            }
            if (std::memcmp (b.data() + i, "data", 4) == 0)
                data = b.data() + i + 8, dataSize = std::min (n, b.size() - (i + 8));
            if (std::memcmp (b.data() + i, "smpl", 4) == 0 && n >= 60 && i + 8 + 52 <= b.size())
                if (u32 (i + 8 + 28) != 0)
                    hasLoop = true, loop0 = u32 (i + 8 + 44), loop1 = u32 (i + 8 + 48);
            i += 8 + n + (n & 1);
        }
        if (data != nullptr && ch >= 1 && ch <= 2 && (tag == 3 || bits == 16))
        {
            auto s = std::make_shared<NoiseSample>();
            const auto width = tag == 3 ? 4u : 2u;
            const auto frames = dataSize / (width * (std::size_t) ch);
            s->ch = ch, s->len = (int) frames, s->sr = (double) sr;
            for (int c = 0; c < ch; ++c)
            {
                s->data[c].assign (frames + 4, 0.0f);
                for (std::size_t k = 0; k < frames; ++k)
                {
                    const auto* p = data + (k * (std::size_t) ch + (std::size_t) c) * width;
                    if (tag == 3)
                        std::memcpy (&s->data[c][k], p, 4);
                    else
                        s->data[c][k] = (float) ((double) (std::int16_t) (p[0] | p[1] << 8) / 32768.0);
                }
            }
            s->ls = hasLoop ? (int) loop0 : 0;
            s->le = hasLoop ? (int) loop1 : (int) frames;
            if (frames > 0)
                result = s;
        }
    }
    const std::lock_guard<std::mutex> lock (mutex);
    return cache.emplace (key, result).first->second;
}

// ------------------------------------------------------------------------------------------------------- helpers

double loopmap (double p, int S, int E, int mode) noexcept // FUN_1800eb970 / FUN_180101e20
{
    if (mode == 0)
        return p;
    if (E == S && p >= E)
        return (double) E;
    const long long L = (long long) E - S;
    if (mode == 1)
    {
        if (p > E)
            p = p - (double) ((long long) ((p - S) / (double) L) * L);
        return p;
    }
    if (mode == 2)
    {
        if (p > E)
        {
            const auto q = E - p;
            if (q >= S)
                return q;
            auto c = std::llabs ((long long) (q - E));
            c = c - (L != 0 ? pyMod (c, L) : 0);
            return q + (double) c;
        }
        return p;
    }
    if (mode == 3)
    {
        if (p < E)
            return p;
        auto d = p - E;
        const auto k = (long long) (d / (double) L);
        d = d - (double) (k * L);
        return (k & 1) ? (S + d) : (E - d);
    }
    return p;
}

// breakpoint list evaluated at pos: linear from value i to value i+1 over duration i; after the last point its value
// holds. vi: index of the value in a point, the duration is its last element
template <std::size_t W>
double bpEval (const std::vector<std::array<double, W>>& pts, int vi, double pos) noexcept // FUN_1800f3e20 / FUN_180101b60
{
    const auto n = pts.size();
    if (n == 0)
        return 0.0;
    std::size_t i = 0;
    constexpr auto di = W - 1;
    while (i < n && pos >= pts[i][di])
    {
        if (i + 1 >= n)
            break;
        pos -= pts[i][di];
        ++i;
    }
    if (i + 1 < n)
    {
        const auto f = pos / pts[i][di];
        return F (F (pts[i][(std::size_t) vi]) * (1.0 - f) + F (pts[i + 1][(std::size_t) vi]) * f);
    }
    return F (pts[i][(std::size_t) vi]);
}

double rate2 (double rate, double kt, double vt, int key, double vel) noexcept // FUN_1800fb9d0 & co.
{
    const auto kt2 = F (F (F (kt - 0.5) + kt) - 0.5), vt2 = F (F (F (vt - 0.5) + vt) - 0.5);
    const auto e = kt2 * F ((double) key - 48.0) * (1.0 / 12.0) + (rate + vt2 * (vel - 0.5)) * 8.0 - 4.0;
    return std::pow (2.0, e);
}

double copysignD (double m, double s) noexcept { return std::copysign (m, s); }

// ------------------------------------------------------------------------------------------------------- engine

struct Synth;

struct ModProc // CModProcessor: Eg1…Eg4 and the 12-slot matrix
{
    const Patch* p = nullptr;
    const Synth* syn = nullptr;
    float dst[48] {};
    struct Eg
    {
        double pos = 0.0, vs = 1.0;
        bool rel = false, rl = false;
    } eg[4];
    double ev[4] {};
    int key = 0;
    double vel = 0.0;

    double get (int k) const noexcept
    {
        const auto v = (double) dst[k];
        return v < -999.9000244140625 ? 0.0 : v;
    }
    double knob (double x, int k) const noexcept { return clampD (F (get (k) + x), 0.0, 1.0); }
    void noteOn (int k, double v) noexcept
    {
        key = k, vel = v;
        std::fill (std::begin (ev), std::end (ev), 0.0);
        const auto ne = std::min<std::size_t> (4, p->envs.size());
        for (std::size_t i = 0; i < ne; ++i)
        {
            eg[i].pos = 0.0, eg[i].rel = false, eg[i].rl = p->envs[i].relLoop;
            eg[i].vs = F (1.0 - F (1.0 - vel) * F (p->envs[i].velocity));
        }
        matrix();
    }
    void release() noexcept
    {
        for (auto& e : eg)
            e.rel = true;
    }
    void update (double B, double fs, double tempo) noexcept;
    void matrix() noexcept;
};

struct Adsr // FUN_1800da760 / FUN_1800da830 / FUN_1800daa00
{
    bool done = false;
    double last = 0.0, pos = 0.0, fs = 44100.0, from = 0.0, ra = 0, rd = 0, S = 0, rr = 0, relfrom = 0;
    int stage = 3;

    void start (double rate, double A, double D, double s) noexcept
    {
        fs = rate;
        from = done ? 0.0 : last;
        A = std::max (A, 0.001);
        D = std::max (D, 0.001);
        ra = 1.0 / (A * fs), rd = 1.0 / (D * fs), S = s;
        done = false, stage = 0, pos = 0.0, rr = 1.0 / (0.001 * fs);
    }
    void setRelease (double R) noexcept
    {
        if (stage != 2)
            rr = 1.0 / (std::max (R, 0.001) * fs);
    }
    void release() noexcept
    {
        if (stage != 2)
            relfrom = last, stage = 2, pos = 0.0;
    }
    static double curve (double t, double a, double b, double c) noexcept
    {
        if (c != 0.0)
            t = 1.0 - std::pow (10.0, t * c) * (1.0 - t);
        return (b - a) * t + a;
    }
    double step (double B) noexcept
    {
        double v = 0.0;
        if (stage == 0)
        {
            pos += B * ra;
            if (pos >= 1.0)
                v = 1.0, stage = 1, pos = 0.0;
            else
                v = curve (pos, from, 1.0, 0.0);
        }
        else if (stage == 1)
        {
            pos += B * rd;
            if (pos >= 1.0)
            {
                if (S == 0.0)
                    stage = 2, done = true;
                v = S, pos = 1.0;
            }
            else
                v = curve (pos, 1.0, S, -2.0);
        }
        else if (stage == 2)
        {
            pos += B * rr;
            if (pos >= 1.0)
                done = true, v = 0.0, pos = 1.0;
            else
                v = curve (pos, relfrom, 0.0, -2.0);
        }
        last = v;
        if (v < 0.0)
        {
            last = 0.0;
            return 0.0;
        }
        return F (v);
    }
};

struct GenOut
{
    float amp[partials], pan[partials], rat[partials];
};

struct GenPlayer // one generator (CZoneGen) of one voice
{
    const Gen* g = nullptr;
    double fs = 44100.0, tempo = 120.0;
    double pwmPh[partials] {};
    int key = 0, mode = 0;
    double vel = 0.0, L = 0.0, pos = 0.0;
    bool rel = false, started = false;
    const Zone* z = nullptr;

    void noteOn (int k, double v, bool first) noexcept
    {
        key = k, vel = v;
        z = nullptr;
        if (! g->zones.empty())
        {
            z = &g->zones.back();
            auto kk = k;
            for (const auto& zz : g->zones)
            {
                const auto k2 = kk - zz.key;
                if (k2 < 0)
                {
                    z = &zz;
                    break;
                }
                kk = k2;
            }
        }
        const auto lv = z != nullptr ? z->level : 0.0;
        L = lv != 0.0 ? F (copysignD (std::pow (std::fabs (lv), (double) 0.3333333432674408f), lv)) : 0.0;
        pos = 0.0, rel = false;
        mode = pyInt (F (F (g->pwmMode + g->pwmMode) + 0.5));
        if (first && ! g->pwmFree)
            pwmReset();
    }
    void pwmReset() noexcept // FUN_1800f83b0
    {
        const auto ph = clampD (g->pwmPhase, 0.0, 1.0);
        for (int k = 0; k < partials; ++k)
        {
            const auto x = (double) (k + 1) * ph;
            pwmPh[k] = x >= 1.0 ? x - std::trunc (x) : x;
        }
    }
    void voiceReset() noexcept
    {
        if (! g->pwmFree)
            pwmReset();
    }
    void render (double B, const ModProc& mod, int gi, GenOut& out) noexcept; // FUN_18012c850
};

struct Resynth // partial bank (CResynth): FUN_1801220c0, FUN_180122ae0, FUN_180115860
{
    int idx[partials];
    double ph[partials];
    int n = 0;
    float gL[partials], gR[partials], X[partials], Y[partials], w[partials], dgL[partials], dgR[partials], dw[partials];
    float q[4] {}, c[4] {}, t = 0.0f, dt = 0.0f;
    double gainL = 0.0, gainR = 0.0;

    Resynth() { reset(); }
    void reset() noexcept
    {
        std::fill (std::begin (idx), std::end (idx), -1);
        std::fill (std::begin (ph), std::end (ph), 0.0);
        n = 0;
        std::fill (std::begin (q), std::end (q), 0.0f);
        std::fill (std::begin (c), std::end (c), 0.0f);
        t = dt = 0.0f;
        gainL = gainR = 0.0;
    }
    void setAmp (double a, double B) noexcept
    {
        const auto q0 = q[1], q1 = q[2], q2 = q[3], q3 = (float) a;
        q[0] = q0, q[1] = q1, q[2] = q2, q[3] = q3;
        const auto c3 = ((q1 - q2) * 1.5f) + ((q3 - q0) * 0.5f);
        const auto c1 = (q2 - q0) * 0.5f;
        const auto c2 = ((q0 - q1) + c1) - c3;
        c[0] = q1, c[1] = c1, c[2] = c2, c[3] = c3;
        t = 0.0f;
        dt = 1.0f / (float) B;
    }
    void update (const float* amp, const float* pan, const float* rat, double B, double f0, double panmod, double fs) noexcept
    {
        const auto nyq = fs * 0.4535, inv = 1.0 / fs, invn = 1.0 / nyq;
        float aL[partials], aR[partials], wn[partials];
        double old[partials];
        for (int k = 0; k < partials; ++k)
        {
            auto f = std::max ((double) rat[k] * f0, 1.0);
            auto p = (((float) panmod + pan[k] + 1.0f) * 0.5f);
            p = std::min (std::max (p, 0.0f), 1.0f);
            auto r = (p * amp[k]) * (float) gainR;
            auto l = ((1.0f - p) * amp[k]) * (float) gainL;
            if (f > nyq)
                r = 0.0f, l = 0.0f;
            f = std::min (f, nyq);
            wn[k] = (float) std::sin (f * 3.141592653589793 * inv) * 2.0f;
            const auto tap = (float) ((1.0 - invn * f) * 0.6 + 0.4);
            aL[k] = l * tap;
            aR[k] = r * tap;
            old[k] = ph[k];
            auto np = inv * f * B + ph[k];
            ph[k] = np >= 1.0 ? np - std::trunc (np) : np;
        }
        const auto inB = 1.0f / (float) B;
        int newIdx[partials];
        float nL[partials], nR[partials], nX[partials], nY[partials], nW[partials], ndL[partials], ndR[partials], ndW[partials];
        int m = 0;
        for (int k = 0; k < partials; ++k)
        {
            const auto o = idx[k];
            const auto cL = o >= 0 ? gL[o] : 0.0f, cR = o >= 0 ? gR[o] : 0.0f;
            const auto keep = std::fabs (aL[k]) >= 1e-6f || std::fabs (aR[k]) >= 1e-6f || std::fabs (cL) >= 1e-6f || std::fabs (cR) >= 1e-6f;
            if (! keep)
            {
                newIdx[k] = -1;
                continue;
            }
            float x, y, w0;
            if (o < 0)
            {
                const auto a2 = old[k] * twoPi;
                x = (float) std::cos (a2), y = (float) std::sin (a2), w0 = wn[k];
            }
            else
                x = X[o], y = Y[o], w0 = w[o];
            auto dl = (aL[k] - cL) * inB, dr = (aR[k] - cR) * inB, dww = (wn[k] - w0) * inB;
            if (std::fabs (dl) < 1e-9f)
                dl = 0.0f;
            if (std::fabs (dr) < 1e-9f)
                dr = 0.0f;
            if (std::fabs (dww) < 1e-9f)
                dww = 0.0f;
            nL[m] = cL, nR[m] = cR, nX[m] = x, nY[m] = y, nW[m] = w0, ndL[m] = dl, ndR[m] = dr, ndW[m] = dww;
            newIdx[k] = m++;
        }
        n = m;
        std::copy (nL, nL + m, gL), std::copy (nR, nR + m, gR), std::copy (nX, nX + m, X), std::copy (nY, nY + m, Y);
        std::copy (nW, nW + m, w), std::copy (ndL, ndL + m, dgL), std::copy (ndR, ndR + m, dgR), std::copy (ndW, ndW + m, dw);
        std::copy (newIdx, newIdx + partials, idx);
    }
    void render (int count, float* outL, float* outR) noexcept
    {
        const auto c0 = c[0], c1 = c[1], c2 = c[2], c3 = c[3];
        for (int i = 0; i < count; ++i)
        {
            float sL = 0.0f, sR = 0.0f;
            for (int k = 0; k < n; ++k)
            {
                gL[k] = gL[k] + dgL[k];
                gR[k] = gR[k] + dgR[k];
                X[k] = X[k] - w[k] * Y[k];
                Y[k] = Y[k] + w[k] * X[k];
                w[k] = w[k] + dw[k];
                sL = sL + gL[k] * X[k];
                sR = sR + gR[k] * X[k];
            }
            const auto env = ((((t * c3) + c2) * t) + c1) * t + c0;
            t = t + dt;
            outL[i] = sL * env + outL[i];
            outR[i] = sR * env + outR[i];
        }
    }
};

struct Voice;

// ------------------------------------------------------------------------------------------------------- effects

struct TapeLine // CTapeDelay (FUN_1800e9550, FUN_1800e9810, FUN_1800e9750)
{
    int N = 4;
    double phase0 = 0.0, minspeed = 0.25, target = 0.0, wetT = 1.0, fb = 0.0, depth = 0.0, rate = 0.0;
    double pos = 0.0, base = 0.0, wet = 0.0, ph = 0.0;
    std::vector<double> rbT, rbV, wbT, wbV;
    double rpreT[3] {}, rpreV[3] {}, wpreT[3] {}, wpreV[3] {};
    int ridx = 0, rcnt = 0;

    void init (int size, double phase)
    {
        N = std::max (4, size);
        phase0 = phase;
        minspeed = 1.0 / N;
        target = 0.0;
        setTime (0.5);
        wetT = 1.0, fb = 0.0, depth = 0.0, rate = 0.0;
        for (auto* v : { &rbT, &rbV, &wbT, &wbV })
            v->reserve ((std::size_t) N + 16);
        reset();
    }
    void setTime (double t) noexcept
    {
        t = std::min (t, 0.99);
        t = std::max (t, 0.0);
        auto d = 1.0 / N;
        d = d + d;
        target = 1.0 / ((t * (1.0 - d) + d) * N);
    }
    void setRate (double hz, double fs) noexcept { rate = std::max (hz, 0.0) / fs; }
    void setDepth (double v) noexcept { depth = F (clampD (v, 0.0, 1.0)); }
    void reset() noexcept
    {
        pos = phase0, base = target, wet = 0.0, ph = 0.0;
        rbT.clear(), rbV.clear(), wbT.clear(), wbV.clear();
        for (int i = 0; i < 3; ++i)
            rpreT[i] = rpreV[i] = wpreT[i] = wpreV[i] = 0.0;
        ridx = 0, rcnt = 0;
    }
    double rt (int r) const noexcept
    {
        const auto j = r - 1;
        if (j < 0)
            return j >= -3 ? rpreT[3 + j] : 0.0;
        return j < (int) rbT.size() ? rbT[(std::size_t) j] : 0.0;
    }
    double rv (int r) const noexcept
    {
        const auto j = r - 1;
        if (j < 0)
            return j >= -3 ? rpreV[3 + j] : 0.0;
        return j < (int) rbV.size() ? rbV[(std::size_t) j] : 0.0;
    }
    double process (double x) noexcept
    {
        x = flush (x);
        auto p = ph;
        const auto tri = p >= 0.5 ? (1.0 - (((p - 0.5) + p) - 0.5)) : p + p;
        p = p + rate;
        auto s = ((3.0 - (tri + tri)) * tri) * (depth * tri) + 1.0;
        s = std::max (s, 1.0);
        ph = p >= 1.0 ? p - (double) (long long) p : p;
        double y = 0.0;
        const auto cnt = rcnt;
        if (cnt != 0)
            for (;;)
            {
                const auto r = ridx;
                if (r >= cnt + 1)
                {
                    y = 0.0;
                    break;
                }
                const auto t1 = rt (r + 1);
                if (pos > t1)
                {
                    ridx = r + 1;
                    continue;
                }
                const auto t0 = F (rt (r)), t1f = F (t1);
                double fr = 0.0;
                if (t0 < t1f)
                    fr = (pos - t0) / F (t1f - t0);
                const auto v0 = rv (r), v1 = rv (r + 1);
                y = F (F (v1 - v0) * fr + v0);
                y = flush (y);
                break;
            }
        auto sp = s * base;
        sp = std::min (sp, 0.2);
        sp = std::max (sp, minspeed);
        const auto w = F (F (y * fb) + x);
        const auto out = F (y * wet);
        pos = sp + pos;
        if (pos >= 1.0)
        {
            const auto n = rcnt;
            auto allT = [&] (int i) { return i < 3 ? rpreT[i] : (i - 3 < n && i - 3 < (int) rbT.size() ? rbT[(std::size_t) (i - 3)] : 0.0); };
            auto allV = [&] (int i) { return i < 3 ? rpreV[i] : (i - 3 < n && i - 3 < (int) rbV.size() ? rbV[(std::size_t) (i - 3)] : 0.0); };
            double nT[3], nV[3];
            for (int i = 0; i < 3; ++i)
                nT[i] = allT (n + i) - 1.0, nV[i] = allV (n + i);
            const auto wcnt = (int) wbT.size();
            wbT.push_back (pos), wbV.push_back (w);
            rcnt = wcnt;
            std::swap (rbT, wbT), std::swap (rbV, wbV);
            wbT.clear(), wbV.clear();
            for (int i = 0; i < 3; ++i)
                wpreT[i] = rpreT[i], wpreV[i] = rpreV[i], rpreT[i] = nT[i], rpreV[i] = nV[i];
            ridx = 0;
            pos = pos - 1.0;
        }
        if (wbT.size() < wbT.capacity())
            wbT.push_back (pos), wbV.push_back (w);
        wet = flush (F (F (wetT - wet) * 0.01 + wet));
        base = (target - base) * 0.005 + base;
        return out;
    }
};

struct Overdrive // FUN_1800ff800 / FUN_1800ff7e0
{
    double target = 0.0, d = 0.0;
    void set (double p) noexcept { target = F (F (F (p * p) * p) * 16.0); }
    static double shape (double x, double d) noexcept
    {
        if (x >= 0.0)
            return F (1.0 - F (F (std::pow (10.0, F (-F (d * x)))) * F (1.0 - x)));
        return F (F (F (std::pow (10.0, F (d * x))) * F (x + 1.0)) - 1.0);
    }
    void process (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const auto dd = F (F (target - d) * 0.01 + d);
            d = dd;
            if (-tinyD < dd && dd < tinyD)
            {
                d = 0.0;
                continue;
            }
            if (dd > 0.01)
            {
                const auto xl = flush ((double) L[i]), xr = flush ((double) R[i]);
                auto q = F (1.0 - F (dd * 0.0625));
                q = F (q * q), q = F (q * q), q = F (q * q);
                const auto g = F (q * 0.7 + 0.3);
                L[i] = (float) F (g * shape (xl, dd));
                R[i] = (float) F (g * shape (xr, dd));
            }
        }
    }
};

struct Chorus // FUN_1800f2a10, FUN_1800f2960, FUN_1800f29c0, FUN_1800f28e0, FUN_1800f2bc0
{
    bool on = false;
    double fs = 44100.0;
    TapeLine l18, l20, l28;
    void init (double rate)
    {
        fs = rate;
        const auto base = fs * 0.019999999552965164;
        l18.init ((int) (base * 1.1), 0.0);
        l20.init ((int) (base * 0.854), 0.467);
        l28.init ((int) base, 0.778);
    }
    void set (double depth, double wet, double rate, double onv, bool rateChange) noexcept
    {
        for (auto* l : { &l18, &l20, &l28 })
            l->setDepth (F (F (depth * depth) * depth));
        auto w = -F (wet * wet);
        if (rateChange)
            w = -F (w * w);
        for (auto* l : { &l18, &l20, &l28 })
            l->wetT = F (w);
        const auto hz = rate * 5.0 + 0.1;
        l18.setRate (hz * 0.896, fs);
        l20.setRate (hz, fs);
        l28.setRate (hz * 0.689, fs);
        on = onv != 0.0;
    }
    void process (float* L, float* R, int n) noexcept
    {
        if (! on)
            return;
        for (int i = 0; i < n; ++i)
        {
            const auto x = F ((double) L[i] + (double) R[i]);
            const auto a = l18.process (x), b = l28.process (x), c = l20.process (x);
            L[i] = (float) F (F (c + a) + (double) L[i]);
            R[i] = (float) F (F (c + b) + (double) R[i]);
        }
    }
};

struct Delay // FUN_1800f2e90, FUN_1800f3080, FUN_1800f2df0, FUN_1800f2da0, FUN_1800f2e50, FUN_1800f3000
{
    bool on = false;
    double fs = 44100.0;
    TapeLine lL, lR;
    void init (double rate)
    {
        fs = rate;
        const auto n = (int) (fs + fs);
        lL.init (n, 0.0);
        lR.init (n, 0.7);
        lL.setRate (1.0, fs);
        lR.setRate (0.876, fs);
    }
    void set (double time, double stereo, double mod, double fb, double wet, double onv) noexcept
    {
        auto t = F (time) * 0.95 + 0.05;
        t = t * t * 0.5;
        const auto d = (F (stereo) - 0.5) * t;
        const auto a = clampD (d + t, 0.05, 1.0), b = clampD (t - d, 0.05, 1.0);
        lL.setTime (a);
        lR.setTime (b);
        const auto m = F (F (mod * mod) * 0.02);
        lL.setDepth (m);
        lR.setDepth (m);
        const auto f = F (fb * 0.84);
        lL.fb = f, lR.fb = f;
        const auto w = F (wet * wet);
        lL.wetT = w, lR.wetT = w;
        on = onv != 0.0;
        lL.reset();
        lR.reset();
    }
    void process (float* L, float* R, int n) noexcept
    {
        if (! on)
            return;
        for (int i = 0; i < n; ++i)
        {
            L[i] = (float) F (lL.process ((double) L[i]) + (double) L[i]);
            R[i] = (float) F (lR.process ((double) R[i]) + (double) R[i]);
        }
    }
};

struct ReverbChannel // FUN_1801247d0
{
    std::vector<float> combs[8], aps[4];
    int ci[8] {}, ai[4] {};
    double cs[8] {};
    double c0 = 0, c1 = 0, c2 = 0, h[4] {};

    void init (double fs, int ch)
    {
        static constexpr int combN[2][8] = { { 1016, 1288, 1177, 1456, 1322, 1591, 1457, 1717 }, { 1216, 1088, 1377, 1256, 1522, 1391, 1657, 1517 } };
        static constexpr int apN[2][4] = { { 249, 210, 198, 145 }, { 244, 220, 178, 155 } };
        const auto k = fs * 2.4943310657596373e-05;
        for (int i = 0; i < 8; ++i)
            combs[i].assign ((std::size_t) std::max (1, (int) (k * combN[ch][i])), 0.0f), ci[i] = 0, cs[i] = 0.0;
        for (int i = 0; i < 4; ++i)
            aps[i].assign ((std::size_t) std::max (1, (int) (k * apN[ch][i])), 0.0f), ai[i] = 0;
        // 50 Hz high-pass (FUN_1800df680, Q = 1); the coefficients the DLL's vectorised sin/cos produced at the common
        // rates were read from the running DLL (§2.13) and are used when available
        const auto w = F (50.0 * twoPi / fs);
        const auto sn = F (std::sin (w)), cn = F (std::cos (w));
        const auto Q = 1.0;
        const auto inv = F (1.0 / F (sn + Q));
        c0 = flush (F (F (cn + 1.0) / F (F (sn / Q) + 2.0)));
        c1 = flush (F (F (F (cn * inv) * Q) * -2.0));
        c2 = flush (F (F (Q - sn) * inv));
        if (fs == 44100.0)
            c0 = F (0.996438086), c1 = F (-1.98580277), c2 = F (0.985853314);
        else if (fs == 48000.0)
            c0 = F (0.996727586), c1 = F (-1.98695254), c2 = F (0.98699522);
        else if (fs == 88200.0)
            c0 = F (0.998218954), c1 = F (-1.99288881), c2 = F (0.992901504);
        else if (fs == 96000.0)
            c0 = F (0.998363674), c1 = F (-1.99346542), c2 = F (0.993476331);
        h[0] = h[1] = h[2] = h[3] = 0.0;
    }
    double hp (double x) noexcept
    {
        x = flush (x);
        const auto x1 = h[1], x2 = h[0], y1 = h[3], y2 = h[2];
        auto y = F (F (F (F (F (F (x - F (x1 + x1)) + x2) * c0) - F (y1 * c1))) - F (c2 * y2));
        y = flush (y);
        h[0] = x1, h[1] = x, h[2] = y1, h[3] = y;
        return y;
    }
    void process (float* buf, int n, double mix, double fb, double damp) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const auto x = hp ((double) buf[i]);
            double acc = 0.0;
            for (int k = 0; k < 8; ++k)
            {
                auto& b = combs[k];
                const auto j = ci[k];
                auto s = F (F (F ((double) b[(std::size_t) j] - cs[k]) * damp) + cs[k]);
                if (-tinyD < s && s < tinyD)
                    s = 0.0;
                cs[k] = s;
                b[(std::size_t) j] = (float) F (F (s * fb) + x);
                ci[k] = j + 1 < (int) b.size() ? j + 1 : 0;
                acc = k == 0 ? s : F (acc + s);
            }
            auto y = acc;
            for (int k = 0; k < 4; ++k)
            {
                auto& b = aps[k];
                const auto j = ai[k];
                auto v = (double) b[(std::size_t) j];
                if (-tinyD < v && v < tinyD)
                    v = 0.0;
                b[(std::size_t) j] = (float) F (F (v * 0.5) + y);
                ai[k] = j + 1 < (int) b.size() ? j + 1 : 0;
                y = F (v - y);
            }
            buf[i] = (float) F (F (y * mix) + (double) buf[i]);
        }
    }
};

struct Reverb
{
    bool on = false;
    double fs = 44100.0, mix = 0.5, fb = F (-0.9), damp = 1.0;
    ReverbChannel L, R;
    void init (double rate)
    {
        fs = rate;
        L.init (fs, 0);
        R.init (fs, 1);
    }
    void set (double decay, double size, double mx, double onv)
    {
        fb = F (F (std::pow ((double) (float) decay, (double) 0.25f)) * 0.67 + 0.3);
        const auto s = F (F (std::pow ((double) (float) size, (double) 0.25f)) * 0.8 + 0.2);
        damp = s > 1.0 ? 1.0 : s;
        const auto m = clampD (mx, 0.0, 1.0);
        mix = F (F (F (m * m) * m) * 0.2);
        on = onv != 0.0;
        L.init (fs, 0);
        R.init (fs, 1);
    }
    void process (float* l, float* r, int n) noexcept
    {
        if (! on)
            return;
        L.process (l, n, mix, fb, damp);
        R.process (r, n, mix, fb, damp);
    }
};

struct EqBand // FUN_1800dfa60 / FUN_1800dfe00
{
    int typ = 1;
    double freq = 1000, bw = 1.0, fs = 44100, gain = 0;
    double c[5] {}, s[4] {};
    void calc() noexcept
    {
        const auto A = F (std::pow (10.0, F (F (gain) * F (0.05000000074505806))));
        auto w = F ((F (freq) * 6.283185) / F (fs));
        w = clampD (w, 0.0, 3.0);
        const auto sn = F (std::sin (w));
        const auto sh = sn != 0.0 ? F (std::sinh ((bw * 0.3465736 * w) / sn) * 2.0) : 0.0;
        const auto al = F (F (sn * sh) * 0.5), sA = F (F (std::sqrt (A)) * sh);
        const auto cs = F (std::cos (w));
        double b0, b1, b2, a0, a1, a2;
        if (typ == 1)
        {
            b0 = F (F (al * A) + 1.0), a0 = F (F (al / A) + 1.0);
            b1 = a1 = F (cs * -2.0);
            a2 = F (1.0 - F (al / A)), b2 = F (1.0 - F (al * A));
        }
        else if (typ == 0)
        {
            const auto t = F (sn * sA), Am = F (A - 1.0), Ap = F (A + 1.0), m = F (Am * cs);
            const auto x2 = F (Ap - m), x5 = F (m + Ap);
            a0 = F (t + x5);
            b0 = F (F (x2 + t) * A), b2 = F (F (x2 - t) * A);
            b1 = F (F (A + A) * F (Am - F (Ap * cs)));
            a1 = F (F (Am * -2.0) - F (F (Ap * cs) + F (Ap * cs)));
            a2 = F (x5 - t);
        }
        else
        {
            const auto t = F (sn * sA), Am = F (A - 1.0), Ap = F (A + 1.0);
            const auto x3 = F (F (Am * cs) + Ap), x5 = F (Ap - F (Am * cs));
            a0 = F (x5 + t);
            b0 = F (F (t + x3) * A), b2 = F (F (x3 - t) * A);
            b1 = F (F (F (F (Ap * cs) + Am) * A) * -2.0);
            a1 = F (F (Am - F (Ap * cs)) + F (Am - F (Ap * cs)));
            a2 = F (x5 - t);
        }
        const auto k = F (1.0 / a0);
        const double v[5] = { b0, b1, b2, a1, a2 };
        for (int i = 0; i < 5; ++i)
            c[i] = flush (F (k * v[i]));
    }
    void process (float* buf, int n) noexcept
    {
        const auto b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
        auto x1 = s[0], x2 = s[1], y1 = s[2], y2 = s[3];
        for (int i = 0; i < n; ++i)
        {
            const auto x = flush ((double) buf[i]);
            auto y = F (F (F (F (F (x1 * b1) + F (x * b0)) + F (b2 * x2)) - F (a1 * y1)) - F (a2 * y2));
            if (-tinyD < y && y < tinyD)
                y = 0.0;
            x2 = x1, y2 = y1, x1 = x, y1 = y;
            buf[i] = (float) y;
        }
        s[0] = x1, s[1] = x2, s[2] = y1, s[3] = y2;
    }
};

struct Eq
{
    bool on = false;
    EqBand bL[8], bR[8];
    void init (double fs) noexcept
    {
        for (int k = 0; k < 8; ++k)
            for (auto* b : { &bL[k], &bR[k] })
            {
                b->typ = k == 0 ? 0 : (k == 7 ? 2 : 1);
                b->freq = F (250.0 * std::pow (2.0, k - 2));
                b->bw = 1.0, b->fs = fs, b->gain = 0.0;
                b->s[0] = b->s[1] = b->s[2] = b->s[3] = 0.0;
                b->calc();
            }
    }
    void set (const double* bands, double onv) noexcept
    {
        for (int k = 0; k < 8; ++k)
        {
            const auto d = ((F (bands[k]) - 0.5) + F (bands[k])) - 0.5;
            const auto g = F (std::copysign (d * d, d) * 12.0);
            for (auto* b : { &bL[k], &bR[k] })
                b->gain = g, b->calc();
        }
        on = onv != 0.0;
    }
    void process (float* L, float* R, int n) noexcept
    {
        if (! on)
            return;
        for (int k = 0; k < 8; ++k)
            bL[k].process (L, n), bR[k].process (R, n);
    }
};

struct FxHost // FUN_1800ff630: overdrive, chorus, delay, reverb, EQ (the bypass never triggers)
{
    Overdrive od;
    Chorus ch;
    Delay dl;
    Reverb rv;
    Eq eq;
    void init (const double* A, double fs, bool rateChange)
    {
        ch.init (fs);
        dl.init (fs);
        rv.init (fs);
        eq.init (fs);
        od = Overdrive {};
        od.set (A[7]);
        if (rateChange)
            od.d = od.target; // a rate change resets the smoother (FUN_1800ff5d0)
        ch.set (A[13], A[14], A[15], A[16], rateChange);
        dl.set (A[17], A[18], A[19], A[20], A[21], A[22]);
        rv.set (A[23], A[24], A[25], A[26]);
        eq.set (A + 27, A[35]);
    }
    void process (float* L, float* R, int n) noexcept
    {
        od.process (L, R, n);
        ch.process (L, R, n);
        dl.process (L, R, n);
        rv.process (L, R, n);
        eq.process (L, R, n);
    }
};

// ------------------------------------------------------------------------------------------------------- voice / synth

struct Synth
{
    const Patch* p = nullptr;
    std::shared_ptr<const NoiseSample> noise;
    double fs = 44100.0, tempo = 120.0;
    double cc[128] {};
    double bend = 0.0, chanpress = 0.0, level = 0.0, finetr = 0.0, bendrange = 0.0, velcurve = 0.0, glidet = 0.0, rdetscale = 0.0;
    int transpose = 0, glidemode = 0, poly = 32, hostmax = 32;
    bool mono = false;
    double ptab[128] {};
    double lrL[128] {}, lrR[128] {};
    bool hasPtab[128] {}, hasLr[128] {};
    int held[128] {};
    double notePofs[128] {};  // each key's Final.Pitch offset at its note on (cents)
    HostSlide slides[128] {}; // a slide note moving a key's Final.Pitch (issue #14)
    std::uint32_t rng = 2222;
    int lastnote = -1;
    long long age = 0;
    double gL = 0.5, gR = 0.5;
    FxHost fx;
    std::vector<Voice> pool;

    void init (const Patch* patch, std::shared_ptr<const NoiseSample> ns, double rate, double bpm, bool rateChange);
    double rand() noexcept
    {
        rng = rng * 0xbb38435u + 0x3619636bu;
        return F (F ((double) rng) * F (2.3283064365386963e-10));
    }
    double target (int ch) const noexcept // FUN_1800e9160
    {
        const auto f2 = F (F (level * cc[7]) * 0.5);
        auto f1 = F (F (F (F (cc[10] - 0.5) + cc[10]) - 0.5) + F (0.5 + 0.5));
        f1 = clampD (f1, 0.0, 2.0);
        if (ch == 0)
            return f1 > 1.0 ? F (f2 * F (2.0 - f1)) : f2;
        return f1 < 1.0 ? F (f2 * f1) : f2;
    }
    int nmax() const noexcept { return std::min (std::min (poly, hostmax), 32); }
    void lr (int key, double& l, double& r) const noexcept
    {
        l = hasLr[key] ? lrL[key] : 0.0;
        r = hasLr[key] ? lrR[key] : 0.0;
    }
    void noteOn (int key, double ivol, double fvol, double fpan, double pofs) noexcept;
    void noteOff (int key) noexcept;
    void slideTo (double to, float samples) noexcept; // issue #14: `to` = Final.Pitch, cents from C5
    void setLevels (int key, double fvol, double fpan) noexcept;
    void render (float* L, float* R, int n) noexcept;
    bool idle() const noexcept;
};

struct Voice
{
    Synth* syn = nullptr;
    const Patch* p = nullptr;
    double fs = 44100.0;
    int B = 127;
    int state = -1, key = -1, pos = 0, donecnt = 0;
    bool alive = false, released = false, flag160 = false;
    long long age = 0;
    double glide = 0.0, vel = 0.0, rdet = 0.0, grate = 0.0, morphPos = 0.0;
    double panmod = 0.0, pfac = 1.0, env = 0.0;
    // noise
    double npos = 0.0, ndir = 1.0, ngL = 0.0, ngR = 0.0, ndL = 0.0, ndR = 0.0, ninc = 0.0;
    int nmode = 0, nS = 0, nE = 0, nL = 0, fmode = 0;
    bool nmono = false;
    double fc[3] {};
    float fst[2][4] {};
    Resynth res;
    Adsr adsr;
    ModProc mod;
    GenPlayer gens[4];
    GenOut outs[4];

    void init (Synth* s)
    {
        syn = s, p = s->p, fs = s->fs;
        B = (int) (fs * 0.0029);
        state = -1, alive = false, released = false, age = 0, key = -1, glide = 0.0, pos = 0;
        npos = 0.0, ndir = 1.0, ngL = ngR = ndL = ndR = 0.0;
        std::memset (fst, 0, sizeof (fst));
        res.reset();
        adsr = Adsr {};
        mod = ModProc {};
        mod.p = p, mod.syn = s;
        std::fill (std::begin (mod.dst), std::end (mod.dst), -1000.0f);
        for (int k = 0; k < 4; ++k)
        {
            gens[k] = GenPlayer {};
            gens[k].g = &p->gens[k];
            gens[k].fs = fs;
        }
    }

    void start (int k, double v) noexcept // FUN_1800fffa0
    {
        const auto* A = p->A;
        key = k, vel = v, alive = true, released = false;
        mod.noteOn (k, v);
        const auto a = mod.knob (A[9], 4), d = mod.knob (A[10], 5), s = mod.knob (A[11], 7);
        const auto D = std::max (F (F (d * d * d) * 30.0), F (0.003));
        const auto At = std::max (F (F (F (F (std::sqrt (a)) * a) * a * a) * 15.0), F (0.003));
        adsr.start (fs, At, D, s * s);
        for (auto& g : gens)
        {
            g.tempo = syn->tempo;
            const auto first = ! g.started;
            g.noteOn (k + syn->transpose, v, first);
            g.started = true;
        }
        morphPos = 0.0, npos = 0.0;
        flag160 = true, donecnt = 0;
    }
    void release() noexcept // FUN_180100620
    {
        released = true;
        mod.release();
        for (auto& g : gens)
            g.rel = true;
        const auto r = mod.knob (p->A[12], 6);
        adsr.setRelease (std::max (F (F (r * r * r) * 10.0), F (0.003)));
        adsr.release();
    }
    double pitchSemis() const noexcept // FUN_1800e9e90
    {
        const auto pt = key >= 0 && key < 128 && syn->hasPtab[key] ? syn->ptab[key] : 0.0;
        return F (syn->finetr + glide + 0.0 + pt + 0.0 + rdet + syn->bendrange * syn->bend + 0.0);
    }
    double freq() const noexcept // FUN_1800e9e50
    {
        return F (220.0 * std::pow (2.0, F (F (pitchSemis() - 57.0) * F (0.0833333358168602))));
    }
    void modUpdate (double b) noexcept // FUN_180100cd0
    {
        mod.update (b, fs, syn->tempo);
        panmod = mod.get (2);
        const auto m3 = mod.get (3);
        pfac = F (std::pow (2.0, F (std::copysign (m3 * m3, m3))));
    }
    bool ampUpdate (double b) noexcept // FUN_180100c20
    {
        if (adsr.done)
            ++donecnt;
        if (donecnt > 1)
        {
            alive = false;
            res.reset();
            state = -1, key = -1, pos = 0;
            ndir = 1.0, npos = 0.0;
            for (auto& g : gens)
                g.voiceReset();
            return false;
        }
        const auto e = adsr.step (b);
        const auto m1 = mod.get (1);
        const auto a = F (F (std::pow (10.0, F (m1))) * e);
        env = a;
        res.setAmp (a, b);
        return true;
    }
    void noiseUpdate (double b) noexcept; // FUN_180100300
    void noiseRender (int n, float* outL, float* outR) noexcept;
    float nfilt (int c, float x) noexcept;
    void resynthUpdate (double b) noexcept;
    void render (float* outL, float* outR, int n) noexcept // FUN_1801006a0
    {
        if (! alive)
            return;
        if (state < 0)
        {
            modUpdate (1);
            resynthUpdate (1);
            float dl = 0.0f, dr = 0.0f;
            res.render (1, &dl, &dr);
            state = 0;
        }
        while (n > 0)
        {
            if (pos == 0)
            {
                modUpdate (B);
                if (! ampUpdate (B))
                    return;
                noiseUpdate (B);
                const auto st = state;
                if (st == 3)
                {
                    state = 1;
                    resynthUpdate (flag160 ? B : 3 * B);
                }
                else
                {
                    state = st + 1;
                    if (st == 0)
                        resynthUpdate (flag160 ? B : 3 * B);
                }
                if (flag160)
                    state = 0;
                flag160 = false;
            }
            const auto k = std::min (n, B - pos);
            res.render (k, outL, outR);
            noiseRender (k, outL, outR);
            pos += k;
            if (pos >= B)
                pos = 0;
            outL += k, outR += k, n -= k;
        }
    }
};

void ModProc::update (double B, double fs, double tempo) noexcept // FUN_1800fb820
{
    const auto ne = std::min<std::size_t> (4, p->envs.size());
    for (std::size_t k = 0; k < ne; ++k)
    {
        const auto& e = p->envs[k];
        auto& st = eg[k];
        const auto rate = knob (e.rate, 0x29 + 2 * (int) k);
        const auto r = rate2 (rate, e.keytrk, e.veltrk, key, vel);
        const auto mul = e.tsync ? tempo * 0.4166666666666667 : 100.0;
        const auto d = (1.0 / fs) * mul * r * B;
        auto amt = (double) dst[0x28 + 2 * k];
        if (amt < -999.9000244140625)
            amt = 1.0;
        st.pos += d;
        auto pp = st.pos;
        if (! (st.rel && st.rl))
            pp = loopmap (pp, e.o40, e.o44, e.loopMode);
        auto v = bpEval (e.pts, 0, pp);
        if (e.o49)
            v = F (v * 2.0 - 1.0);
        ev[k] = F (F (v * st.vs) * F (amt));
    }
    matrix();
}

void ModProc::matrix() noexcept // FUN_1800fbbc0
{
    std::fill (std::begin (dst), std::end (dst), -1000.0f);
    for (const auto& s : p->matrix)
    {
        if (s.src == 0 || s.dst == 0)
            continue;
        auto d = s.dst < 0 ? s.dst + 48 : s.dst;
        if (d < 0 || d >= 48)
            continue;
        auto cur = (double) dst[d];
        if (cur < -999.9000244140625)
            cur = 0.0, dst[d] = 0.0f;
        double v;
        if (s.src == 1)
            v = F ((double) key * F (0.008333333767950535));
        else if (s.src == 2)
            v = vel;
        else if (s.src == 3)
            v = syn->bend;
        else if (s.src == 4)
            v = syn->chanpress;
        else if (s.src >= 5 && s.src <= 8)
            v = ev[s.src - 5];
        else
            v = syn->cc[(s.src - 9) & 0x7f];
        auto x = F (v * F (F (s.amt + s.amt) - 1.0));
        x = clampD (x, -1.0, 1.0);
        dst[d] = (float) F (x + cur);
    }
}

void GenPlayer::render (double B, const ModProc& mod, int gi, GenOut& out) noexcept
{
    static constexpr int rateIdx[4] = { 0xd, 0x11, 0x15, 0x19 }, detIdx[4] = { 0xf, 0x13, 0x17, 0x1b }, panIdx[4] = { 0x10, 0x14, 0x18, 0x1c },
                         pwmIdx[4] = { 0xe, 0x12, 0x16, 0x1a };
    if (z == nullptr || z->frames.empty())
    {
        std::fill (std::begin (out.amp), std::end (out.amp), 0.0f);
        std::fill (std::begin (out.pan), std::end (out.pan), 0.0f);
        for (int k = 0; k < partials; ++k)
            out.rat[k] = (float) (k + 1);
        return;
    }
    const auto& so = z->opt;
    const auto& fr = z->frames;
    const auto rate = mod.knob (so.rate, rateIdx[gi]);
    const auto r = rate2 (rate, so.keytrk, so.veltrk, key, vel);
    const auto step = so.tsync ? r * (tempo * 0.4166666666666667 / fs) : r * (100.0 / fs);
    auto pp = pos;
    if (! (rel && so.relLoop))
        pp = loopmap (pp, so.o40, so.o44, so.loopMode);
    std::size_t i = 0;
    while (i < fr.size() && pp >= fr[i].dur && i + 1 < fr.size())
        pp -= fr[i].dur, ++i;
    const auto& a = fr[i];
    const Frame* b = nullptr;
    double gA, gB, wA, wB;
    if (i + 1 < fr.size() && ! (pp >= a.dur))
    {
        b = &fr[i + 1];
        const auto f = pp / a.dur, w0 = 1.0 - f;
        gA = F (a.gain * w0), gB = F (b->gain * f), wA = F (w0), wB = F (f);
    }
    else
        gA = F (a.gain), gB = 0.0, wA = 1.0, wB = 0.0;
    pos = pos + B * step;
    const auto m8a0 = (float) F (L * gA), m8a4 = (float) F (L * gB), m898 = (float) F (L * wA), m89c = (float) F (L * wB);
    auto det = mod.knob (g->detuneRange, detIdx[gi]);
    det = F (det * det);
    auto pnr = mod.knob (g->panRange, panIdx[gi]);
    pnr = F (pnr * pnr);
    const auto vm1 = (float) F (vel - 1.0);
    const auto fdet = (float) det, fpnr = (float) pnr;
    for (int k = 0; k < partials; ++k)
    {
        const auto vs = so.arr60[(std::size_t) k], gk = so.arr68[(std::size_t) k], hk = (float) (k + 1);
        if (b != nullptr)
        {
            const auto m = m8a4 * b->amp[(std::size_t) k] + m8a0 * a.amp[(std::size_t) k];
            out.amp[k] = (vm1 * vs + 1.0f) * m * m * m * gk;
            out.pan[k] = fpnr * b->pan[(std::size_t) k] * m89c + fpnr * a.pan[(std::size_t) k] * m898;
            out.rat[k] = (m89c * b->det[(std::size_t) k] + m898 * a.det[(std::size_t) k]) * fdet + hk;
        }
        else
        {
            const auto m = m8a0 * a.amp[(std::size_t) k];
            out.amp[k] = (vm1 * vs + 1.0f) * m * m * m * gk;
            out.pan[k] = fpnr * a.pan[(std::size_t) k] * m898;
            out.rat[k] = m898 * a.det[(std::size_t) k] * fdet + hk;
        }
    }
    if (mode == 1 || mode == 2) // PWM (FUN_1800f8460)
    {
        const auto prate = mod.knob (g->pwmRate, pwmIdx[gi]);
        const auto inc = F (prate * 0.25) / fs;
        for (int k = 0; k < partials; ++k)
        {
            auto ph = (double) out.rat[k] * inc * B + pwmPh[k];
            if (ph > 1.0)
                ph = ph - std::trunc (ph);
            pwmPh[k] = ph;
            const auto c = std::cos (ph * twoPi);
            out.amp[k] = (float) (mode == 2 ? 1.0 - c : c) * out.amp[k];
        }
    }
}

void Voice::noiseUpdate (double b) noexcept
{
    const auto* w = syn->noise.get();
    if (w == nullptr)
        return;
    const auto* A = p->A;
    auto g = mod.knob (A[36], 8);
    g = F (F (F (g * g) * g) * g);
    g = F (g + g);
    if (A[38] != 0.0)
        g = 0.0;
    const auto n9 = mod.knob (A[37], 9);
    auto pn = F (F (F (F (F (n9 + n9) + panmod) - 1.0) + 1.0) * 0.5);
    pn = clampD (pn, 0.0, 1.0);
    double TL = 0.0, TR = 0.0;
    if (key >= 0 && key < 128)
        syn->lr (key, TL, TR);
    const auto hR = F (F (clampD (1.0 - 0.0, 0.0, 1.0) * TR) * 0.5);
    auto hL = F (F (clampD (1.0 + 0.0, 0.0, 1.0) * TL) * 0.5);
    const auto vv = F (F (1.0 - F (F (1.0 - vel) * A[39])) * g);
    hL = F (F (F (hL * F (1.0 - pn)) * env) * vv);
    const auto inv = F (1.0 / F ((double) b));
    ndR = F (F (F (F (F (F (env + env) * vv) * pn) * hR) - ngR) * inv);
    ndL = F (F (F (hL + hL) - ngL) * inv);
    const auto rr = mod.knob (p->f6a8, 10);
    ninc = std::pow (2.0, (rr - 0.5) * 4.0) * F (w->sr / fs);
    nmode = pyInt (p->f6ac);
    auto S = std::clamp (w->ls, 0, w->len - 1), E = std::clamp (w->le, 0, w->len - 1);
    if (S == E)
        S = 0, E = w->len - 1;
    nS = S, nE = E, nL = E - S;
    nmono = p->f6c0 != 0.0;
    fmode = pyInt (p->f6bc);
    if (fmode >= 1 && fmode <= 3) // noise filter coefficients (FUN_180113b70)
    {
        const auto resv = mod.knob (p->f6b4, 0xc), cut = mod.knob (p->f6b0, 0xb);
        const auto pitch = pitchSemis();
        auto wv = (std::pow (2.0, (pitch - 57.0) * 0.15789473684210525 * p->f6b8 + cut * 9.0) * 219.9114857512855) / fs;
        wv = clampD (wv, 0.0, 3.1);
        const auto Q = clampD (F (resv * resv) * 30.0 + 1.0, 1.0, 31.0);
        const auto sn = std::sin (wv), cs = std::cos (wv);
        double c0, c1, c2;
        if (fmode == 2)
        {
            const auto al = (0.5 / Q) * sn, iv = 1.0 / (al + 1.0);
            c0 = F (F (sn * 0.5) * iv), c2 = F (F (1.0 - al) * iv), c1 = F (F (cs + cs) * iv);
        }
        else
        {
            c0 = F (((fmode == 1) ? (1.0 - cs) : (cs + 1.0)) / (sn / Q + 2.0));
            const auto iv = 1.0 / (sn + Q);
            c1 = F (cs * iv * (Q * -2.0));
            c2 = F ((Q - sn) * iv);
        }
        auto fl = [] (double v) { return (-1e-9 < v && v < 9.999999717180685e-10) ? 0.0 : v; };
        fc[0] = fl (c0), fc[1] = fl (c1), fc[2] = fl (c2);
    }
}

float Voice::nfilt (int c, float x) noexcept // FUN_180113df0
{
    auto* st = fst[c];
    const auto c0 = (float) fc[0], c1 = (float) fc[1], c2 = (float) fc[2];
    const auto x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    float y;
    if (fmode == 1)
        y = (((((x1 + x1) + x) + x2) * c0) - (c1 * y1)) - (c2 * y2);
    else if (fmode == 3)
        y = ((((x - (x1 + x1)) + x2) * c0) - (c1 * y1)) - (c2 * y2);
    else
        y = (((x - x2) * c0) + (c1 * y1)) - (c2 * y2);
    auto fl = [] (float v) { return (-9.999999717180685e-10f < v && v < 9.999999717180685e-10f) ? 0.0f : v; };
    st[0] = fl (x), st[1] = fl (x1), st[2] = fl (y), st[3] = fl (y1);
    return y;
}

void Voice::noiseRender (int n, float* outL, float* outR) noexcept // FUN_180100880
{
    const auto* w = syn->noise.get();
    if (w == nullptr || n == 0)
        return;
    const auto L = w->len;
    auto ps = npos;
    const auto inc = ninc;
    auto gl = ngL, gr = ngR;
    auto interp = [&] (int c, int i, float fr) {
        const auto* x = w->data[c].data();
        const auto x0 = x[i], x1 = x[i + 1], x2 = x[i + 2], x3 = x[i + 3];
        const auto a = (x2 - x0) * 0.5f;
        const auto b = ((x3 - x0) * 0.5f) + ((x1 - x2) * 1.5f);
        return ((((((x0 - x1) + a) - b) + (b * fr)) * fr) + a) * fr + x1;
    };
    for (int k = 0; k < n; ++k)
    {
        float vL = 0.0f, vR = 0.0f;
        const auto i = (int) ps;
        if (0 <= i && i < L)
        {
            const auto fr = (float) F (ps - (double) i);
            vL = interp (0, i, fr);
            vR = vL;
            if (w->ch == 2)
                vR = interp (1, i, fr);
            if (nmono)
                vL = vR = (vL + vR) * 0.5f;
        }
        if (fmode >= 1 && fmode <= 3)
        {
            vL = nfilt (0, vL);
            vR = nfilt (1, vR);
        }
        if (nmode == 0)
            ps = ps + inc;
        else if (nmode == 1)
        {
            ps = ps + inc;
            if (nE <= ps)
                ps = ps - nL;
        }
        else if (nmode == 2)
        {
            if (ps < nE)
            {
                ps = ndir * inc + ps;
                if (ndir < 0.0 && ps <= nS)
                    ps = nL + ps;
            }
            else
            {
                ps = ps - inc;
                ndir = -1.0;
                if (ps <= nS)
                    ps = nL + ps;
            }
        }
        else if (nmode == 3)
        {
            double d;
            if (ps < nE)
            {
                d = ndir;
                if (d < 0.0 && ps <= nS)
                    ndir = 1.0, d = 1.0;
            }
            else
            {
                ndir = -1.0, d = -1.0;
                if (ps <= nS)
                    ndir = 1.0, d = 1.0;
            }
            ps = d * inc + ps;
        }
        outL[k] = vL * (float) gl + outL[k];
        outR[k] = vR * (float) gr + outR[k];
        gr = F (ndR + gr);
        gl = F (ndL + gl);
    }
    npos = ps;
    ngL = gl, ngR = gr;
}

void Voice::resynthUpdate (double b) noexcept // FUN_1801001c0 (+ FUN_1801054b0 morph line)
{
    const auto& ml = p->morph;
    // glide FUN_1800e9d90
    if (glide < key)
    {
        glide = F (F (b * grate) + glide);
        if (glide > key)
            glide = (double) key;
    }
    else if (glide > key)
    {
        glide = F (glide - F (b * grate));
        if (glide < key)
            glide = (double) key;
    }
    const auto mp = loopmap (morphPos, ml.o40, ml.o44, ml.loopMode);
    auto X = bpEval (ml.pts, 0, mp), Y = bpEval (ml.pts, 1, mp);
    X = mod.knob (X, 0x1e);
    Y = mod.knob (Y, 0x1f);
    const auto wB = F (Y * X), wC = F (F (1.0 - Y) * X), wA = F (F (1.0 - X) * Y), wD = F (F (1.0 - Y) * F (1.0 - X));
    double mix[4];
    for (int k = 0; k < 4; ++k)
    {
        const auto v = mod.knob (ml.mix[k], 0x20 + 2 * k);
        mix[k] = ml.mute[k] ? 0.0 : F (v * v);
    }
    double W[4] = { wA, wB, wC, wD };
    if (ml.o80 >= 1 && ml.o80 <= 4)
        for (int k = 0; k < 4; ++k)
            mix[k] = W[k] = k == ml.o80 - 1 ? 1.0 : 0.0;
    double gp[4];
    for (int k = 0; k < 4; ++k)
        gp[k] = F (F (mod.knob (ml.pan[k], 0x21 + 2 * k) * 2.0) - 1.0);
    for (int k = 0; k < 4; ++k)
        gens[k].render (b, mod, k, outs[k]);
    float G[4], fW[4], fgp[4];
    for (int k = 0; k < 4; ++k)
        G[k] = (float) F (mix[k] * W[k]), fW[k] = (float) W[k], fgp[k] = (float) gp[k];
    float amp[partials], pan[partials], rat[partials];
    for (int k = 0; k < partials; ++k)
    {
        amp[k] = G[0] * outs[0].amp[k] + G[1] * outs[1].amp[k] + G[2] * outs[2].amp[k] + G[3] * outs[3].amp[k];
        pan[k] = (fgp[0] + outs[0].pan[k]) * fW[0] + (fgp[1] + outs[1].pan[k]) * fW[1] + (fgp[2] + outs[2].pan[k]) * fW[2]
                 + (fgp[3] + outs[3].pan[k]) * fW[3];
        rat[k] = fW[0] * outs[0].rat[k] + fW[1] * outs[1].rat[k] + fW[2] * outs[2].rat[k] + fW[3] * outs[3].rat[k];
    }
    const auto mr = mod.knob (ml.rate, 0x1d);
    const auto r = rate2 (mr, ml.keytrk, ml.veltrk, key + syn->transpose, vel);
    const auto m208 = ml.tsync ? (syn->tempo * 0.4166666666666667 / fs) : (100.0 / fs);
    morphPos = r * m208 * b + morphPos;
    double TL = 0.0, TR = 0.0;
    if (key >= 0 && key < 128)
        syn->lr (key, TL, TR);
    res.gainL = F (F (clampD (1.0 + 0.0, 0.0, 1.0) * TL * 0.5) * 2.0);
    res.gainR = F (F (clampD (1.0 - 0.0, 0.0, 1.0) * TR * 0.5) * 2.0);
    const auto f0 = freq() * pfac;
    res.update (amp, pan, rat, b, f0, panmod, fs);
}

void Synth::init (const Patch* patch, std::shared_ptr<const NoiseSample> ns, double rate, double bpm, bool rateChange)
{
    p = patch;
    noise = std::move (ns);
    fs = rate, tempo = bpm;
    const auto* A = p->A;
    std::fill (std::begin (cc), std::end (cc), 0.0);
    cc[7] = 1.0, cc[10] = 0.5;
    bend = chanpress = 0.0;
    level = F (std::pow (10.0, F (F (A[0] * F (2.4000000953674316)) - F (1.2000000476837158))));
    transpose = pyInt (F (F (A[5] - 0.5) * 48.0));
    const auto fine = pyInt (F (F (A[6] - 0.5) * 200.0));
    finetr = F (fine * 0.01 + transpose);
    bendrange = (double) pyInt (F (A[8] * 12.0));
    velcurve = std::max (F (A[4] * 5.0), 0.0);
    const auto g = F (F (A[1] * A[1]) * A[1]);
    glidet = g >= 0.0001 ? g : 9.999999747378752e-05;
    glidemode = pyInt (A[2]);
    std::fill (std::begin (hasPtab), std::end (hasPtab), false);
    std::fill (std::begin (hasLr), std::end (hasLr), false);
    std::fill (std::begin (held), std::end (held), 0);
    std::fill (std::begin (slides), std::end (slides), HostSlide {});
    rng = 2222, lastnote = -1, age = 0;
    poly = pyInt (A[3]) + 1;
    if (poly < 1 || poly > 32)
        poly = 32;
    mono = pyInt (A[3]) == 0;
    hostmax = 32;
    rdetscale = 0.0;
    fx.init (A, fs, rateChange);
    pool.assign (voiceCount, Voice {});
    for (auto& v : pool)
        v.init (this);
    gL = rateChange ? target (0) : 0.5;
    gR = rateChange ? target (1) : 0.5;
}

void Synth::noteOn (int key, double ivol, double fvol, double fpan, double pofs) noexcept // FUN_1800e93d0
{
    key = std::clamp (key, 0, 127);
    const auto v7 = F (F (F (std::log (F (F (ivol * 10.0) + 1.0))) * F (0.3284587264060974)) * 127.0);
    const auto vel7 = std::clamp (pyInt (F (v7)), 0, 127);
    const auto velf = F (vel7 * F (0.007874015718698502));
    ptab[key] = F ((F (100.0 * key - 6000.0 + pofs) * 0.01 - key) + 60.0);
    hasPtab[key] = true;
    notePofs[key] = pofs, slides[key] = {};
    const auto t = (1.0 + clampD (fpan, -1.0, 1.0)) * 0.78539816339744831;
    lrL[key] = F (fvol * std::cos (t)), lrR[key] = F (fvol * std::sin (t)), hasLr[key] = true;
    held[key] = pyInt (F (velf * 127.0));
    if (velf == 0.0)
        return;
    const auto r = rand();
    const auto rd = F (F (F (r - 1.0) + F (r - 1.0)) * rdetscale);
    auto start = lastnote >= 0 ? (double) lastnote : (double) key;
    const auto vcur = velf > 0.0 ? F (std::pow (velf, velcurve)) : 0.0;
    if (mono)
    {
        auto& v = pool[0];
        if ((glidemode == 1 || glidemode == 3) && (! v.alive || v.released))
            start = (double) key;
        double rate = F (12.0 / (fs * glidet));
        if (glidemode == 2 || glidemode == 3)
        {
            const auto cur = v.alive ? v.glide : start;
            rate = F (F (std::fabs ((double) key - cur)) / (fs * glidet));
        }
        const auto wasFree = ! v.alive || v.released;
        v.key = key, v.rdet = rd, v.released = false, v.grate = rate, v.age = age++;
        if (wasFree)
        {
            v.vel = vcur;
            v.glide = start;
            if (glidet < 0.001)
                v.glide = (double) key;
            v.start (key, v.vel);
        }
        lastnote = key;
        return;
    }
    const auto n = nmax();
    Voice* pick = nullptr;
    for (int i = 0; i < n; ++i)
        if (! pool[(std::size_t) i].alive)
        {
            pick = &pool[(std::size_t) i];
            break;
        }
    if (pick == nullptr)
    {
        Voice *oAct = nullptr, *oRel = nullptr;
        for (int i = 0; i < n; ++i)
        {
            auto& v = pool[(std::size_t) i];
            auto*& o = v.released ? oRel : oAct;
            if (o == nullptr || v.age < o->age)
                o = &v;
        }
        if (oAct == nullptr)
            oAct = &pool[0];
        if (oRel == nullptr)
            oRel = &pool[0];
        pick = oRel->released ? oRel : oAct;
    }
    auto& v = *pick;
    v.age = age++;
    v.key = key, v.rdet = rd, v.vel = vcur, v.glide = start, v.released = false;
    if (glidet < 0.001)
        v.glide = (double) key;
    if (glidemode == 0 || glidemode == 1)
        v.grate = F (12.0 / (fs * glidet));
    else
        v.grate = F (F (std::fabs ((double) key - v.glide)) / (fs * glidet));
    v.start (key, v.vel);
    lastnote = key;
}

void Synth::noteOff (int key) noexcept // FUN_1800e9430
{
    if (key < 0 || key > 127)
        return;
    held[key] = 0;
    if (mono)
    {
        int best = -1, bd = 128;
        for (int k = 0; k < 128; ++k)
            if (held[k] != 0 && k != key && std::abs (key - k) < bd)
                bd = std::abs (key - k), best = k;
        for (auto& v : pool)
            if (v.alive && v.key == key)
            {
                if (best >= 0)
                {
                    v.age = age++;
                    v.key = best;
                    v.grate = F (12.0 / (fs * glidet));
                    lastnote = best;
                }
                else
                {
                    v.released = true;
                    v.release();
                }
            }
        return;
    }
    for (auto& v : pool)
        if (v.alive && v.key == key && ! v.released)
        {
            v.released = true;
            v.release();
        }
}

void Synth::setLevels (int key, double fvol, double fpan) noexcept
{
    if (key < 0 || key > 127 || ! hasLr[key])
        return;
    const auto t = (1.0 + clampD (fpan, -1.0, 1.0)) * 0.78539816339744831;
    lrL[key] = F (fvol * std::cos (t)), lrR[key] = F (fvol * std::sin (t));
}

void Synth::slideTo (double to, float samples) noexcept
{
    for (int k = 0; k < 128; ++k)
        if (held[k] != 0 && hasPtab[k])
            slides[k].start ((float) (to - (100.0 * k - 6000.0 + notePofs[k])), samples);
}

void Synth::render (float* L, float* R, int n) noexcept
{
    std::fill (L, L + n, 0.0f);
    std::fill (R, R + n, 0.0f);
    for (int k = 0; k < 128; ++k) // a slide note moves the key's Final.Pitch (issue #14)
        if (slides[k].moving())
        {
            const auto c = (double) slides[k].advance ((float) n);
            ptab[k] = F ((F (100.0 * k - 6000.0 + notePofs[k] + c) * 0.01 - k) + 60.0);
        }
    for (auto& v : pool)
        v.render (L, R, n);
    fx.process (L, R, n);
    const auto tl = target (0), tr = target (1);
    for (int i = 0; i < n; ++i)
    {
        L[i] = (float) gL * L[i];
        R[i] = R[i] * (float) gR;
        gL = F ((tl - gL) * 0.005 + gL);
        gR = F ((tr - gR) * 0.005 + gR);
    }
}

bool Synth::idle() const noexcept
{
    return std::none_of (pool.begin(), pool.end(), [] (const Voice& v) { return v.alive; });
}

// ------------------------------------------------------------------------------------------------------- LYNX front-end
//
// Levels (§2.3, REA_rodada28 §4.1): as GMS — Final.Vol = chan.Vol, Final.Pan = note pan + chan.Pan, Final.Pitch =
// 100·(key − 60) + channel pitch, law A per note; LYNX recovers V and pan from its channel gains (gl = √2·V·cos(…))
// and does not apply them again. Velocity → Init.Vol = (11^(1.28·vel) − 1)/10 (round 28 inference). The effects
// (chorus, delay, reverb, EQ) run after the voices whether or not a voice sounds (no bypass), so the instrument only
// reports idle once no voice is alive and the output has been silent for 30 s of effect tails.

struct Setup
{
    std::unique_ptr<Patch> patch;
    std::shared_ptr<const NoiseSample> noise;
    std::unique_ptr<Synth> synth; // built for `rate` (off the audio thread)
    double rate = 0.0;

    bool build (double sr, double bpm)
    {
        synth.reset();
        rate = sr;
        if (patch == nullptr)
            return false;
        try
        {
            auto s = std::make_unique<Synth>();
            s->init (patch.get(), noise, sr, bpm, false);
            synth = std::move (s);
        }
        catch (...)
        {
            synth.reset();
        }
        return synth != nullptr;
    }
};

class MorphineInstrument final : public Instrument
{
public:
    MorphineInstrument() = default;
    ~MorphineInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete setup;
    }

    void prepare (double sr) noexcept override
    {
        take();
        rate = sr;
        preparedRate.store (sr);
        if (setup != nullptr)
            setup->build (sr, tempo.load());
        synth = setup != nullptr ? setup->synth.get() : nullptr;
        quiet = 0;
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        tempo.store (bpm > 0.0 ? bpm : 120.0);
        if (channel.pluginState == state && channel.samplePath == noisePath)
            return;
        state = channel.pluginState;
        noisePath = channel.samplePath;
        try
        {
            auto s = std::make_unique<Setup>();
            if (auto p = parseState (state))
            {
                s->patch = std::make_unique<Patch> (std::move (*p));
                if (! s->patch->noise.empty())
                    s->noise = loadNoise (noisePath);
            }
            s->build (preparedRate.load(), tempo.load());
            delete incoming.exchange (s.release());
        }
        catch (...)
        {
        }
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        if (synth != nullptr)
            synth->slideTo (100.0 * std::clamp (key, 0, 127) - 6000.0 + (double) pitchCents.load() + (double) cents, samples);
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        if (synth == nullptr)
            return;
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto k = std::clamp (key, 0, 127);
        notePan[k] = nextPan;
        synth->noteOn (k, initVol, channelVol, std::clamp ((double) nextPan + channelPan, -1.0, 1.0), (double) pitchCents.load() + (double) nextCents);
        quiet = 0;
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (synth != nullptr)
            synth->noteOff (key);
    }

    void allNotesOff() noexcept override
    {
        if (synth == nullptr)
            return;
        for (int k = 0; k < 128; ++k)
            if (synth->held[k] != 0)
                synth->noteOff (k);
    }

    // the effects run after the voices whether or not a voice sounds (no bypass, §2.11): idle once no voice is alive
    // and the output has stayed silent longer than the longest delay line (2 s)
    bool idle() const noexcept override { return synth == nullptr || (synth->idle() && quiet > (long long) (4.0 * rate)); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (synth == nullptr || n <= 0)
            return;
        synth->tempo = tempo.load();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int k = 0; k < 128; ++k)
                synth->setLevels (k, vol, std::clamp ((double) notePan[k] + pan, -1.0, 1.0));
        }
        for (int done = 0; done < n; done += blockSize)
        {
            const auto m = std::min (blockSize, n - done);
            synth->render (bufL, bufR, m);
            bool loud = false;
            for (int i = 0; i < m; ++i)
            {
                left[done + i] += bufL[i];
                right[done + i] += bufR[i];
                loud = loud || std::fabs (bufL[i]) > 1e-7f || std::fabs (bufR[i]) > 1e-7f;
            }
            quiet = loud || ! synth->idle() ? 0 : quiet + m;
        }
    }

private:
    static constexpr int blockSize = 512;

    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            // a new state (load or an edit in the settings panel) plays on a synth built for it off the audio thread;
            // one built for another rate waits for prepare()
            retired.store (setup);
            setup = s;
            synth = setup->rate == rate ? setup->synth.get() : nullptr;
            quiet = 0;
        }
    }

    Setup* setup = nullptr;
    Synth* synth = nullptr;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::atomic<float> pitchCents { 0.0f };
    std::atomic<double> tempo { 120.0 }, preparedRate { 44100.0 };
    std::vector<std::uint8_t> state;
    std::string noisePath;
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0;
    long long quiet = 0;
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
    float bufL[blockSize] {}, bufR[blockSize] {};
};
} // namespace

bool morphineStateUsable (const std::vector<std::uint8_t>& state) { return parseState (state).has_value(); }

std::string morphineNoiseName (const std::vector<std::uint8_t>& state)
{
    const auto p = parseState (state);
    return p ? p->noise : std::string();
}

int morphineParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    const auto p = parseState (state);
    if (! p || index < 0 || index >= 40)
        return -1;
    return p->globalOffset + 4 * index;
}

std::unique_ptr<Instrument> makeMorphineInstrument() { return std::make_unique<MorphineInstrument>(); }

namespace morphine
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, const std::string& noisePath, int block, double bpm, bool rateChange)
{
    const auto p = parseState (state);
    if (! p || length <= 0)
        return {};
    const auto patch = std::make_unique<Patch> (*p);
    auto synth = std::make_unique<Synth>();
    synth->init (patch.get(), patch->noise.empty() ? nullptr : loadNoise (noisePath), sampleRate, bpm, rateChange);
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    block = std::max (1, block);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r (l.size());
    int t = 0;
    while (t < length)
    {
        // events at this sample in the model's order: (on, off) per note in list order, stable-sorted by sample
        for (const auto& nt : notes)
        {
            if (nt.start == t)
                synth->noteOn (nt.key, nt.initVol, nt.finalVol, nt.finalPan, nt.pitchCents);
            if (nt.release == t)
                synth->noteOff (std::clamp (nt.key, 0, 127));
        }
        auto m = std::min (block, length - t);
        for (auto e : evs)
            if (t < e && e < t + m)
                m = e - t;
        synth->render (l.data(), r.data(), m);
        for (int j = 0; j < m; ++j)
        {
            out[2 * (std::size_t) (t + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (t + j) + 1] = r[(std::size_t) j];
        }
        t += m;
    }
    return out;
}
} // namespace morphine
} // namespace lynx
