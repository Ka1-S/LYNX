#include "core/Gms.h"
#include "core/HostSlide.h"

#include <algorithm>
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
#include <vector>

// 1:1 port of our round-39 reference model (r39/py/gmsmodel.py + gmspatch.py, gmsfx.py, gmseq.py, gmsrev.py). The
// model emulates the DLL's float32 arithmetic with numpy float32 scalars (operations between float32 values and Python
// floats stay float32, NEP 50) and uses Python floats (doubles) where the DLL computes in double. This file keeps those
// types — `float` where the model holds float32, `double` + an explicit (float) where it rounds a Python float — and
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
constexpr double twoPi = 6.283185307179586;
constexpr int maxChunk = 512; // engine buffers of 0x200 floats (§1.1)
constexpr int voiceCount = 99;
constexpr int paramCount = 212;

// cvttss2si / cvttsd2si: truncation toward zero; out of range or NaN → INT_MIN
int i32 (double x) noexcept
{
    if (! (x == x) || x >= 2147483648.0 || x < -2147483648.0)
        return std::numeric_limits<int>::min();
    return (int) x;
}

// MSVC powf as the model computes it: the exact power of the two floats, rounded to float
float powf32 (float x, float y) noexcept { return (float) std::pow ((double) x, (double) y); }

float clamp01 (float x) noexcept { return std::min (std::max (x, 0.0f), 1.0f); }

// FUN_1800ef900: 0 below −80 dB, else powf(10, x·0.05)
float db2lin (float x) noexcept
{
    if (! (x >= -80.0f))
        return 0.0f;
    return powf32 (10.0f, x * 0.05f);
}

// ------------------------------------------------------------------------------------------------------- state

struct Patch
{
    std::array<float, paramCount> P {};
    std::array<std::string, 3> waves; // oscillator wave base names, as GMS resolves them
};

std::uint32_t le32 (const std::uint8_t* p) noexcept { return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24; }

// "SSLF" + u32 n + preset; preset = tag "Syn4"|"Syn3" + 7 × 512 path slots + (Syn4: 3 × i32 UI) + 212 × f32 + 512 name
std::optional<Patch> parseState (const std::vector<std::uint8_t>& s, int* paramOffset = nullptr)
{
    if (s.size() < 8 || std::memcmp (s.data(), "SSLF", 4) != 0)
        return std::nullopt;
    const auto n = (std::size_t) le32 (s.data() + 4);
    if (s.size() < 8 + n)
        return std::nullopt;
    const auto* b = s.data() + 8;
    const auto tag = n >= 4 ? le32 (b) : 0u;
    if (tag != 0x53796E34u && tag != 0x53796E33u)
        return std::nullopt;
    auto o = (std::size_t) 4 + 7 * 512;
    if (tag == 0x53796E34u)
        o += 12;
    if (n < o + 4 * paramCount)
        return std::nullopt;
    Patch p;
    for (int i = 0; i < paramCount; ++i)
        std::memcpy (&p.P[(std::size_t) i], b + o + 4 * (std::size_t) i, 4);
    if (paramOffset != nullptr)
        *paramOffset = (int) (8 + o);
    for (int k = 0; k < 3; ++k)
    {
        const auto* slot = b + 4 + 512 * (std::size_t) k;
        std::string path;
        for (int i = 0; i < 512 && slot[i] != 0; ++i)
            path.push_back ((char) slot[i]);
        std::replace (path.begin(), path.end(), '\\', '/');
        if (const auto cut = path.rfind ('/'); cut != std::string::npos)
            path = path.substr (cut + 1);
        if (const auto dot = path.rfind ('.'); dot != std::string::npos)
            path = path.substr (0, dot);
        if (path == "Devils Horn")
            path = "Double Spike";
        p.waves[(std::size_t) k] = path;
    }
    return p;
}

// ------------------------------------------------------------------------------------------------- wave tables

constexpr int tableCount = 512, tableSize = 1024;
using Tables = std::vector<float>; // tableCount × tableSize

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

// channel 0 of a 16-bit PCM or float WAV (others: empty)
std::vector<float> readWaveChannel0 (const std::vector<std::uint8_t>& b)
{
    if (b.size() < 12)
        return {};
    std::size_t i = 12;
    int tag = 0, ch = 0, bits = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    while (i + 8 <= b.size() && i < b.size() - 8)
    {
        const auto n = (std::size_t) le32 (b.data() + i + 4);
        if (std::memcmp (b.data() + i, "fmt ", 4) == 0 && i + 8 + 16 <= b.size())
        {
            tag = b[i + 8] | b[i + 9] << 8;
            ch = b[i + 10] | b[i + 11] << 8;
            bits = b[i + 22] | b[i + 23] << 8;
        }
        if (std::memcmp (b.data() + i, "data", 4) == 0)
        {
            data = b.data() + i + 8;
            dataSize = std::min (n, b.size() - (i + 8));
        }
        i += 8 + n + (n & 1);
    }
    if (data == nullptr || ch <= 0)
        return {};
    std::vector<float> x;
    if (tag == 3)
    {
        const auto frames = dataSize / (4 * (std::size_t) ch);
        x.resize (frames);
        for (std::size_t k = 0; k < frames; ++k)
            std::memcpy (&x[k], data + 4 * (std::size_t) ch * k, 4);
    }
    else if (bits == 16)
    {
        const auto frames = dataSize / (2 * (std::size_t) ch);
        x.resize (frames);
        for (std::size_t k = 0; k < frames; ++k)
        {
            const auto v = (std::int16_t) (data[2 * (std::size_t) ch * k] | data[2 * (std::size_t) ch * k + 1] << 8);
            x[k] = (float) ((double) v / 32768.0);
        }
    }
    return x;
}

// FUN_1800f1570 (§1.6): resample to 1024 points with the 4-point cubic of FUN_1800dfa30 (value between p[j+1] and
// p[j+2]), spectrum magnitude/1024 and phase of harmonics 1…512, table k = Σ_{h ≤ k+1} A_h·cos(2π·h·i/1024 + φ_h)
// accumulated in float with double products.
std::shared_ptr<const Tables> buildTables (const std::vector<float>& x)
{
    auto T = std::make_shared<Tables> ((std::size_t) tableCount * tableSize, 0.0f);
    const auto L = x.size();
    if (L == 0)
        return T;
    std::vector<float> p (L + 4, 0.0f);
    std::copy (x.begin(), x.end(), p.begin());
    std::vector<double> y (tableSize);
    for (int i = 0; i < tableSize; ++i)
    {
        const auto pos = (double) i * (double) L * 0.0009765625;
        const auto idx = (std::size_t) pos;
        const auto t = (float) (pos - (double) idx);
        const auto p0 = p[idx], p1 = p[idx + 1], p2 = p[idx + 2], p3 = p[idx + 3];
        const auto a = (p2 - p0) * 0.5f;
        const auto b = (p3 - p0) * 0.5f + (p1 - p2) * 1.5f;
        const auto v = (((((p0 - p1) + a) - b) + b * t) * t + a) * t + p1;
        y[(std::size_t) i] = idx < L ? (double) v : 0.0;
    }
    // DFT of harmonics 1…512
    std::vector<double> cw (tableSize), sw (tableSize);
    for (int k = 0; k < tableSize; ++k)
    {
        cw[(std::size_t) k] = std::cos (twoPi * k / tableSize);
        sw[(std::size_t) k] = std::sin (twoPi * k / tableSize);
    }
    std::vector<float> amp (tableCount), ph (tableCount);
    for (int h = 1; h <= tableCount; ++h)
    {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < tableSize; ++n)
        {
            const auto k = (std::size_t) ((h * n) & (tableSize - 1));
            re += y[(std::size_t) n] * cw[k];
            im -= y[(std::size_t) n] * sw[k];
        }
        amp[(std::size_t) h - 1] = (float) (std::hypot (re, im) / 1024.0);
        ph[(std::size_t) h - 1] = (float) std::atan2 (im, re);
    }
    std::vector<float> acc (tableSize, 0.0f);
    for (int kk = 0; kk < tableCount; ++kk)
    {
        const auto w = (double) ((float) kk + 1.0f) * 0.006135923151542565;
        auto* row = T->data() + (std::size_t) kk * tableSize;
        for (int n = 0; n < tableSize; ++n)
        {
            acc[(std::size_t) n] = (float) (std::cos ((double) n * w + (double) ph[(std::size_t) kk]) * (double) amp[(std::size_t) kk] + (double) acc[(std::size_t) n]);
            row[n] = acc[(std::size_t) n];
        }
    }
    return T;
}

std::shared_ptr<const Tables> zeroTables()
{
    static const auto z = std::make_shared<const Tables> ((std::size_t) tableCount * tableSize, 0.0f);
    return z;
}

// tables per wave file, built once per process (a missing or unreadable file gives silent tables, like the model)
std::shared_ptr<const Tables> waveTables (const std::string& dir, const std::string& name)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Tables>> cache;
    if (dir.empty() || name.empty())
        return zeroTables();
    const auto path = dir + "/" + name + ".wav";
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (const auto it = cache.find (path); it != cache.end())
            return it->second;
    }
    const auto x = readWaveChannel0 (readFileBytes (path));
    auto T = x.empty() ? zeroTables() : buildTables (x);
    const std::lock_guard<std::mutex> lock (mutex);
    return cache.emplace (path, T).first->second;
}

// FUN_1800f0ed2…: 65 536 (g, k) pairs, independent of the sample rate
struct CutoffTable
{
    std::vector<float> g, k;
    CutoffTable() : g (65536), k (65536)
    {
        for (int i = 0; i < 65536; ++i)
        {
            const auto e = (float) i * 0.00014913368795532733f;
            g[(std::size_t) i] = (float) std::pow (2.0, (double) e) * 0.0005f;
            k[(std::size_t) i] = (float) ((double) (g[(std::size_t) i] * 7.5f + 5.0f) * 0.756525);
        }
    }
};
const CutoffTable& cutoffTable()
{
    static const CutoffTable t;
    return t;
}

// ------------------------------------------------------------------------------------------------------- RNG

// the process-global LCG of GMS (FUN_1800ef8c0); LYNX keeps one per instance (§1.7)
struct Rng
{
    static constexpr std::uint32_t A = 0x0bb38435u, C = 0x3619636bu;
    std::uint32_t s = 0;
    std::uint32_t next() noexcept { return s = s * A + C; }
    void skip (std::uint64_t n) noexcept // jump ahead by n draws
    {
        std::uint32_t a = 1, c = 0, ba = A, bc = C;
        while (n != 0)
        {
            if (n & 1)
                a = a * ba, c = c * ba + bc;
            bc = bc * ba + bc;
            ba = ba * ba;
            n >>= 1;
        }
        s = s * a + c;
    }
};

std::uint32_t phaseOfs (std::uint32_t base, float x) noexcept // FUN_1800ef960
{
    auto d = (double) x * 4194303.0 + 268435392.0;
    d = std::clamp (d, -9.0e18, 9.0e18);
    return (std::uint32_t) ((std::uint32_t) (std::int64_t) d * 256u + base);
}

int tableFor (float freq, double fs) noexcept // FUN_1800ef980
{
    const auto fsf = (float) fs;
    if (fsf * 0.0009765625f < freq)
        return std::clamp (i32 ((double) ((0.5f / freq) * fsf)), 0, 511);
    return 511;
}

std::uint32_t phaseInc (float f, double fs) noexcept // FUN_1800ef930
{
    const auto fsf = (float) fs;
    if (f <= 0.0f)
        f = 0.0f;
    const auto lim = fsf * 0.4999000132083893f;
    if (lim <= f)
        f = lim;
    return (std::uint32_t) (std::int64_t) ((double) (f / fsf) * 4294967295.0);
}

bool matrixHit (int param, int dest) noexcept // FUN_1800ef810
{
    switch (dest)
    {
        case 1: return param == 12;
        case 2: return param == 13;
        case 4: return param == 50;
        case 6: return param == 20;
        case 7: return param == 22;
        case 8:
        case 12: return param == 16;
        case 10: return param == 21;
        case 11: return param == 23;
        case 14: return param == 34;
        case 15: return param == 28;
        case 17: return param == 35;
        case 18: return param == 29;
        case 20: return param == 4;
        case 21: return param == 5;
        default: return false;
    }
}

constexpr float lfoSync[9] = { 0.125f, 0.25f, 0.375f, 0.5f, 0.75f, 1.0f, 2.0f, 4.0f, 8.0f };

float lfoSyncValue (float x) noexcept // FUN_1800efa80
{
    const auto k = i32 ((double) (x * 9.0f));
    if (k < 1)
        return 0.125f;
    if (k > 8)
        return 8.0f;
    return lfoSync[k];
}

float lfoWave (int shape, float ph) noexcept // FUN_1800ef9f0
{
    if (shape == 0)
        return (float) std::sin ((double) ph * twoPi);
    if (shape == 1)
    {
        auto v = ph + ph;
        if (1.0f < v)
            v = 2.0f - v;
        return v;
    }
    if (shape == 2)
        return 1.0f - ph;
    return ph < 0.5f ? 1.0f : 0.0f;
}

// FUN_1800efb10: LFO frequency (Hz) and, for a synced LFO while the host plays (pos4 = song beats × 4), the phase
struct LfoRate
{
    float f = 0.0f;
    bool hasPhase = false;
    float phase = 0.0f;
};
LfoRate lfoRate (float rate, float sync, float tempo, bool hasPos, float pos4) noexcept
{
    LfoRate r;
    const auto k = i32 ((double) (sync + sync));
    if (k == 0)
    {
        const auto v = clamp01 (1.0f - rate);
        r.f = (float) ((double) (v * v * 25.0f) + 0.01);
        return r;
    }
    auto v = lfoSyncValue (rate);
    if (k == 2)
        v = v * 4.0f;
    if (hasPos && 0.0f <= pos4)
        r.hasPhase = true, r.phase = (pos4 * 0.25f) / v;
    if (tempo > 0.0f)
    {
        const auto t = (v * 60.0f) / tempo;
        if (t != 0.0f)
            r.f = 1.0f / t;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------------- effects

constexpr float tiny9 = 1e-9f;
float tiny0 (float v) noexcept { return (v < tiny9 && v > -tiny9) ? 0.0f : v; }

// float sine of the DLL's packed CRT kernel (0x18012a460, SSE path)
float sinK (float x) noexcept
{
    const auto a = std::fabs (x);
    const auto t = a * 0.31830987334251404f + 12582912.0f;
    const auto n = t - 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy (&bits, &t, 4);
    const auto odd = (bits & 1u) != 0;
    auto r = a - 3.140625f * n;
    r = r - 0.0009675025939941406f * n;
    r = r - 1.5099067240953445e-07f * n;
    r = r - 5.126688136514179e-12f * n;
    const auto r2 = r * r;
    if (odd)
        r = -r;
    auto p = 2.605780537123792e-06f * r2 + -0.00019809602235909551f;
    p = p * r2 + 0.008333062753081322f;
    p = p * r2 + -0.16666659712791443f;
    const auto y = r + (r2 * p) * r;
    return x < 0.0f ? -y : y;
}

void sinCosK (float w, float& s, float& c) noexcept // FUN_18012a3f0
{
    const auto a = std::fabs (w);
    if (a < 0.0001220703125f)
    {
        s = w;
        c = 1.0f - (a * a) * 0.5f;
        return;
    }
    s = sinK (w);
    c = sinK (a + 1.5707963705062866f);
}

struct Biquad // FUN_1800e4eb0/e4fb0 (low-pass), FUN_1800e4cc0/e4dc0 (high-pass)
{
    bool hp = false;
    float f = 0, q = 0, fs = 0, a = 0, b1 = 0, b2 = 0, x2 = 0, x1 = 0, y2 = 0, y1 = 0;

    void init (bool highPass) noexcept
    {
        hp = highPass;
        f = q = fs = 0.0f;
        design (100.0f, 1.0f, 44100.0f);
        clear();
    }
    void clear() noexcept { x2 = x1 = y2 = y1 = 0.0f; }
    void design (float nf, float nq, float nfs) noexcept
    {
        if (nf == f && nq == q && nfs == fs)
            return;
        f = nf, q = nq, fs = nfs;
        const auto w = (float) ((double) f * twoPi / (double) fs);
        float s = 0, c = 0;
        sinCosK (w, s, c);
        const auto inv = 1.0f / (s + q);
        const auto den = s / q + 2.0f;
        const auto na = hp ? (c + 1.0f) / den : (1.0f - c) / den;
        const auto nb1 = (c * inv) * (q * -2.0f);
        const auto nb2 = (q - s) * inv;
        a = tiny0 (na), b1 = tiny0 (nb1), b2 = tiny0 (nb2);
    }
    float tick (float x) noexcept
    {
        x = tiny0 (x);
        const auto t = hp ? (x - (x1 + x1)) + x2 : ((x1 + x1) + x) + x2;
        auto y = ((t * a) - (y1 * b1)) - (b2 * y2);
        y = tiny0 (y);
        x2 = x1, y2 = y1, x1 = x, y1 = y;
        return y;
    }
};

// MFXTapeDelay (FUN_1800feae0 / FUN_1800fef90 / FUN_1800fed00 / reset FUN_1800fec70)
struct TapeDelay
{
    struct Worm
    {
        std::vector<float> v;
        std::vector<double> t;
        int count = 0, ridx = 0;
    };
    int len = 4;
    Worm worms[2];
    int w = 0, r = 1;
    float gain = 0.0f, fb = 0.0f, lp = 1.0f, hp = 0.0f, s1 = 0.0f, s2 = 0.0f;
    double rate = 0.0, pos = 0.0, minrate = 0.25;

    void init (int size)
    {
        len = std::max (4, size);
        for (auto& wm : worms)
        {
            wm.v.assign ((std::size_t) len + 0x100, 0.0f);
            wm.t.assign ((std::size_t) len + 0x100, 0.0);
            wm.count = wm.ridx = 0;
        }
        w = 0, r = 1;
        gain = 0.0f, fb = 0.0f, lp = 1.0f, hp = 0.0f, s1 = s2 = 0.0f;
        rate = 0.0, pos = 0.0, minrate = 1.0 / len;
    }
    void reset() noexcept
    {
        for (auto& wm : worms)
        {
            std::fill (wm.v.begin(), wm.v.end(), 0.0f);
            std::fill (wm.t.begin(), wm.t.end(), 0.0);
            wm.count = wm.ridx = 0;
        }
        s1 = s2 = 0.0f;
    }
    void setTime (double d) noexcept
    {
        d = std::min (d, 0.99);
        d = std::max (d, 0.0);
        const auto m2 = 2.0 * (1.0 / len);
        rate = 1.0 / ((d * (1.0 - m2) + m2) * len);
    }
    float tick (float x) noexcept
    {
        auto& rb = worms[r];
        float out = 0.0f;
        const auto p = pos;
        if (rb.count != 0)
            for (;;)
            {
                const auto i = rb.ridx;
                if (rb.count + 1 <= i)
                    break;
                const auto ti = rb.t[(std::size_t) (10 + i)];
                if (p <= ti)
                {
                    double fr = 0.0;
                    const auto t0 = (float) rb.t[(std::size_t) (9 + i)];
                    const auto t1 = (float) ti;
                    if (t0 < t1)
                        fr = (p - (double) t0) / (double) (t1 - t0);
                    const auto v = (float) ((double) (rb.v[(std::size_t) (10 + i)] - rb.v[(std::size_t) (9 + i)]) * fr + (double) rb.v[(std::size_t) (9 + i)]);
                    if (! (v < tiny9 && v > -tiny9))
                        out = v;
                    break;
                }
                rb.ridx = i + 1;
            }
        auto rt = std::min (rate, 0.2);
        rt = std::max (rt, minrate);
        const auto y = (((out * fb) + (x * gain)) - s1) * lp + s1;
        const auto newpos = rt + p;
        pos = newpos;
        s1 = y;
        s2 = ((y - s2) * hp) + s2;
        const auto wv = y - s2;
        if (newpos >= 1.0)
        {
            auto& wb = worms[w];
            const auto c = (std::size_t) rb.count;
            wb.v[9] = rb.v[9 + c], wb.t[9] = rb.t[9 + c] - 1.0;
            wb.v[8] = rb.v[8 + c], wb.t[8] = rb.t[8 + c] - 1.0;
            wb.v[7] = rb.v[7 + c], wb.t[7] = rb.t[7 + c] - 1.0;
            wb.v[(std::size_t) (10 + wb.count)] = wv;
            wb.t[(std::size_t) (10 + wb.count)] = pos;
            wb.ridx = 0;
            std::swap (w, r);
            pos -= 1.0;
            worms[w].count = 0;
        }
        auto& wb = worms[w];
        if ((std::size_t) (10 + wb.count) < wb.v.size())
        {
            wb.v[(std::size_t) (10 + wb.count)] = wv;
            wb.t[(std::size_t) (10 + wb.count)] = pos;
            ++wb.count;
        }
        return out;
    }
};

// output EQ band (FUN_1800e5050 / FUN_1800e50a0 / FUN_1800e5440); band 0 low shelf, 1–3 peaking, 4 high shelf
struct EqBand
{
    int idx = 0;
    float c[5] {}, s[4] {};

    void clear() noexcept { s[0] = s[1] = s[2] = s[3] = 0.0f; }
    void design (float gainDb, float fs) noexcept
    {
        static constexpr float freq[5] = { 80.0f, 220.0f, 1200.0f, 6000.0f, 12000.0f };
        static constexpr float bwt[5] = { 1.0f, 2.5f, 2.799999952316284f, 2.5f, 2.0f };
        const auto type = idx == 0 ? 0 : (idx == 4 ? 2 : 1);
        const auto f = std::max (freq[idx], 0.0f);
        const auto bw = std::max (bwt[idx], 9.99999975e-05f);
        const auto A = (float) std::pow (10.0, (double) (gainDb * 0.05000000074505806f));
        auto w = (float) ((double) f * 6.283185 / (double) fs);
        w = std::min (w, 3.0f);
        w = std::max (w, 0.0f);
        const auto sn = (float) std::sin ((double) w);
        const auto sh = std::sinh ((double) bw * 0.3465736 * (double) w / (double) sn);
        const auto al2 = (float) (sh + sh);
        const auto alpha = (sn * al2) * 0.5f;
        const auto beta = std::sqrt (A) * al2;
        const auto one = 1.0f;
        float b0, b1, b2, a0, a1, a2;
        if (type == 0)
        {
            const auto cs = (float) std::cos ((double) w);
            const auto k = sn * beta;
            const auto am1 = A - one, ap1 = A + one;
            const auto t = am1 * cs, u = ap1 - t, v = t + ap1;
            b0 = (u + k) * A;
            b2 = (u - k) * A;
            b1 = (A + A) * (am1 - (ap1 * cs));
            a1 = (am1 * -2.0f) - ((ap1 * cs) + (ap1 * cs));
            a0 = k + v;
            a2 = v - k;
        }
        else if (type == 2)
        {
            const auto cs = (float) std::cos ((double) w);
            const auto k = sn * beta;
            const auto am1 = A - one, ap1 = A + one;
            const auto t = am1 * cs, c2 = ap1 * cs;
            const auto p = t + ap1, q = ap1 - t;
            b0 = (k + p) * A;
            b2 = (p - k) * A;
            a0 = q + k;
            b1 = ((c2 + am1) * A) * -2.0f;
            const auto d = am1 - c2;
            a1 = d + d;
            a2 = q - k;
        }
        else
        {
            const auto aA = alpha * A, aD = alpha / A;
            b0 = aA + one;
            a0 = aD + one;
            const auto cc = (float) std::cos ((double) w) * -2.0f;
            a1 = cc, b1 = cc;
            a2 = one - aD;
            b2 = one - aA;
        }
        const auto inv = one / a0;
        const float cs[5] = { inv * b0, inv * b1, inv * b2, inv * a1, inv * a2 };
        for (int i = 0; i < 5; ++i)
            c[i] = tiny0 (cs[i]);
    }
    float tick (float x) noexcept
    {
        x = tiny0 (x);
        auto y = ((((s[0] * c[1]) + (x * c[0])) + (c[2] * s[1])) - (s[2] * c[3])) - (c[4] * s[3]);
        y = tiny0 (y);
        s[1] = s[0], s[3] = s[2], s[0] = x, s[2] = y;
        return y;
    }
};

// GMS reverb (class fxReverb, §1.13 slot 7). The early-reflection path is unreachable from GMS (switched off every
// block) and is not built.
struct Reverb
{
    static constexpr double qa[18][2] = { { 421.0, -360.5 }, { 2981.0, 19.5 }, { 216.0, 5.5 }, { 250.0, 0.5 }, { 99.0, 1.5 }, { 100.0, 0.5 },
                                          { 28.0, 2.5 }, { 1975.0, 25.5 }, { 199.0, 1.5 }, { 216.0, 5.5 }, { 421.0, -360.5 }, { 421.0, -360.5 },
                                          { 421.0, -360.5 }, { 200.0, 0.5 }, { 1.0, 0.5 }, { 1.0, 0.5 }, { 1.0, 0.5 }, { 1.0, 0.5 } };
    static constexpr int lines[12] = { 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };
    static constexpr int taps0[14] = { 266, 2974, 1913, 2496, 1690, 187, 1566, 353, 3627, 1728, 3173, 1811, 335, 621 };
    static constexpr float tiny = 1e-30f;

    float fs = 44100.0f;
    int pi[18] {};
    std::vector<float> buf[12], pre;
    int len[12] {}, pos[12] {}, prepos = 0, prelen = 1, lastsize = 0;
    int taps[14] {};
    bool dirty = true;
    float lp = 0, hpx = 0, hpy = 0, dL[2] {}, dR[2] {}, tL = 0, tR = 0, lev = -1000.0f;
    float gin = 0, hpk = 0, lpk = 1, ap1 = 0, ap2 = 0, tap1 = 0, tap2 = 0, decay = 0, shelf = 0, shk = 0, damp = 1, dry = 0, tank = 0, width = 0;

    static int floorI (float x) noexcept
    {
        auto i = (int) x;
        if ((double) i != (double) x && x < 0.0f)
            --i;
        return i;
    }
    static int cdiv (long long a, long long b) noexcept { return (int) (a / b); }
    static float db (int i) noexcept // FUN_180102ff0
    {
        if (i < -360)
            return 0.0f;
        return (float) std::pow (10.0, (double) ((float) i * 0.004999999888241291f));
    }

    void init (float rate)
    {
        static constexpr float preset[18] = { 0.8574821949005127f, 0.018785642459988594f, 0.16203702986240387f, 0.0f, 0.49494948983192444f, 1.0f,
                                              0.2857142984867096f, 0.2405063360929489f, 0.07035175710916519f, 0.16203702986240387f,
                                              0.8574821949005127f, 0.7743467688560486f, 0.5724465847015381f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f };
        fs = rate;
        const auto fsi = (int) fs;
        for (int i = 0; i < 18; ++i)
            setp (i, preset[i]);
        for (int k = 0; k < 12; ++k)
        {
            const auto n = std::max (1, cdiv ((long long) fsi * lines[k], 20000));
            buf[k].assign ((std::size_t) n, tiny);
            len[k] = n;
            pos[k] = 0;
        }
        const auto predmax = cdiv ((long long) fsi * 250, 1000);
        pre.assign ((std::size_t) std::max (1, predmax), tiny);
        prepos = 0, prelen = std::max (1, predmax), lastsize = 0, dirty = true;
        reset();
    }
    void setp (int i, float v) noexcept
    {
        pi[i] = floorI ((v * (float) qa[i][0]) + (float) qa[i][1]);
        dirty = true;
    }
    void reset() noexcept
    {
        for (auto& b : buf)
            std::fill (b.begin(), b.end(), tiny);
        std::fill (pre.begin(), pre.end(), tiny);
        lp = hpx = hpy = 0.0f;
        dL[0] = dL[1] = dR[0] = dR[1] = 0.0f;
        tL = tR = 0.0f;
        lev = -1000.0f;
    }
    static float onePoleLp (int p, float fs) noexcept
    {
        if ((float) p < 221.0f)
        {
            const auto c = (float) std::cos ((double) (((float) p * 628.3185424804688f) / fs));
            const auto t = 2.0f - c;
            const auto s = std::sqrt ((t * t) - 1.0f);
            return 1.0f - (t - s);
        }
        return 1.0f;
    }
    void recalc() noexcept // FUN_180103020
    {
        const auto fsi = (int) fs;
        const auto* p = pi;
        const auto one = 1.0f;
        gin = p[14] == 0 ? db (p[0]) : 0.0f;
        if ((float) p[1] > 19.0f)
        {
            const auto x = (std::min (fs * 0.5f, (float) p[1]) * 3.1415927410125732f) / fs;
            const auto e = (float) std::tan ((double) x);
            hpk = (e - one) / (e + one);
        }
        else
            hpk = 0.0f;
        lpk = onePoleLp (p[2], fs);
        prelen = std::max (1, cdiv ((long long) fsi * p[3], 1000));
        prepos %= prelen;
        const auto base = cdiv ((long long) (p[4] + 15) * fsi, 115);
        for (int k = 0; k < 12; ++k)
        {
            len[k] = std::max (1, cdiv ((long long) base * lines[k], 20000));
            len[k] = std::min (len[k], (int) buf[k].size());
            pos[k] %= len[k];
        }
        for (int t = 0; t < 14; ++t)
            taps[t] = cdiv ((long long) base * taps0[t], 20000);
        if (p[4] != lastsize)
        {
            lastsize = p[4];
            for (int k = 0; k < 12; ++k)
                std::fill (buf[k].begin(), buf[k].begin() + len[k], 0.0f);
        }
        const auto diff = (float) p[5] * 0.01f;
        const auto inv = one / fs;
        if (diff != 0.0f)
        {
            ap1 = (float) std::exp ((double) (((float) (len[0] + len[1]) * (-49.34111022949219f / diff)) * inv));
            ap2 = (float) std::exp ((double) (((-31.398887634277344f / diff) * (float) (len[2] + len[3])) * inv));
            auto x = (((float) (len[4] + len[8]) * -3.4538776874542236f) / fs) * (2.857142925262451f / diff);
            tap1 = (float) std::exp ((double) x);
            x = ((-3.4538776874542236f / fs) * (float) (len[6] + len[10])) * (1.9607843160629272f / diff);
            tap2 = (float) std::exp ((double) x);
        }
        else
            ap1 = ap2 = tap1 = tap2 = 0.0f;
        const auto tsum = ((float) (len[9] + len[7] + len[5] + len[11]) / fs) * -6.907755374908447f;
        const auto t60 = (float) p[8] * 0.10000000149011612f;
        decay = (float) std::pow ((double) (float) std::exp ((double) (tsum / t60)), 0.25);
        auto r = (float) std::pow ((double) (float) std::exp ((double) ((10.0f / ((float) p[6] * t60)) * tsum)), 0.25);
        r = r / decay;
        shelf = (r - one) * 0.5f;
        const auto rm = std::min (r, one);
        const auto e = (float) std::tan ((double) (((float) p[7] * 3.1415927410125732f) / fs));
        shk = (e - rm) / (rm + e);
        damp = onePoleLp (p[9], fs);
        dry = p[15] == 0 ? db (p[10]) : 0.0f;
        tank = p[17] == 0 ? db (p[12]) * 0.30000001192092896f : 0.0f;
        width = (float) p[13] * 0.01f;
        dirty = false;
    }
    float ap (int k, float x, float c1, float c2) noexcept
    {
        auto& b = buf[k];
        const auto i = pos[k];
        const auto t = b[(std::size_t) i] - (c1 * x);
        b[(std::size_t) i] = (c2 * t) + x;
        pos[k] = i > 0 ? i - 1 : len[k] - 1;
        return t;
    }
    static int wrap (int i, int n) noexcept { return i >= n ? i - n : i; }
    // FUN_180103f30: adds the result into outL/outR
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
    {
        if (dirty)
            recalc();
        const auto d = decay, one = 1.0f, half = 0.5f, dd = one - d;
        const auto g41 = (dd * 0.6000000238418579f) + d, g42 = (dd * 0.25f) + d, g43 = (dd * 0.699999988079071f) + d;
        const auto g56 = (dd * 0.20000000298023224f) + d, g44 = (dd * half) + d;
        auto* B = buf;
        auto* L = len;
        auto* P = pos;
        const auto* T = taps;
        for (int s = 0; s < n; ++s)
        {
            const auto xl = gin * inL[s], xr = gin * inR[s];
            const auto m = ((xr + xl) * half) + tiny;
            auto l = ((m - lp) * lpk) + lp;
            lp = l;
            const auto h = ((l - hpx) * hpk) + hpy;
            hpy = l;
            hpx = h;
            if (hpk != 0.0f)
                l = ((l - h) * half) + tiny;
            auto t = ap (0, l, ap1, ap1);
            auto u = ap (1, t, ap2, ap1);
            t = ap (2, u, ap1, ap1);
            u = ap (3, t, ap2, ap1);
            const auto pv = pre[(std::size_t) prepos];
            pre[(std::size_t) prepos] = u;
            prepos = prepos > 0 ? prepos - 1 : prelen - 1;
            const auto tin = pv + 9.99999968e-21f;
            // tank, left half
            auto v = B[11][(std::size_t) P[11]] * d;
            auto s2 = ((v - dL[1]) * shk) + dL[0];
            dL[0] = v, dL[1] = s2;
            auto w = (((((s2 + v) * shelf) + v) + tin) - tL) * damp;
            w = w + tL;
            tL = w;
            u = ap (4, w, tap1, tap1);
            auto i = P[5];
            auto o = B[5][(std::size_t) i];
            B[5][(std::size_t) i] = u;
            P[5] = i > 0 ? i - 1 : L[5] - 1;
            o = o * d;
            u = ap (6, o, tap2, tap2);
            i = P[7];
            B[7][(std::size_t) i] = u;
            i = i > 0 ? i - 1 : L[7] - 1;
            P[7] = i;
            v = B[7][(std::size_t) i] * d;
            s2 = ((v - dR[1]) * shk) + dR[0];
            dR[0] = v, dR[1] = s2;
            w = (((((s2 + v) * shelf) + v) + tin) - tR) * damp;
            w = w + tR;
            tR = w;
            u = ap (8, w, tap1, tap1);
            i = P[9];
            o = B[9][(std::size_t) i];
            B[9][(std::size_t) i] = u;
            P[9] = i > 0 ? i - 1 : L[9] - 1;
            o = o * d;
            u = ap (10, o, tap2, tap2);
            i = P[11];
            B[11][(std::size_t) i] = u;
            P[11] = i > 0 ? i - 1 : L[11] - 1;
            auto at = [&] (int line, int tap) { return B[line][(std::size_t) wrap (tap + P[line], L[line])]; };
            const auto fA = at (9, T[0]), fB = at (9, T[1]), fC = at (10, T[2]), fD = at (11, T[3]);
            const auto fE = at (5, T[4]), fF = at (6, T[5]), fG = at (7, T[6]);
            const auto f7 = at (5, T[8]), f8 = at (5, T[7]), f9 = at (6, T[9]), f10 = at (7, T[10]), f11 = at (9, T[11]);
            const auto f12 = at (10, T[12]), f13 = at (11, T[13]);
            auto tl = (((fA - (g42 * fB)) - (g43 * fC)) - (g56 * fD)) + (g41 * fE);
            tl = (tl + fF) + (g44 * fG);
            auto tr = ((((g42 * f7) + f8) - (g43 * f9)) - (g56 * f10)) - (g41 * f11);
            tr = (tr + f12) + (g44 * f13);
            if (lev < -99.0f)
                lev = tank;
            lev = ((tank - lev) * 0.00100000005f) + lev;
            const auto a = (lev * tl) + 0.0f, b = (lev * tr) + 0.0f;
            const auto sm = b + a, df = (a - b) * width;
            outL[s] = ((xl * dry) + (df + sm)) + outL[s];
            outR[s] = ((xr * dry) + (sm - df)) + outR[s];
        }
    }
};

constexpr float rateSteps[9] = { 0.125f, 0.25f, 0.375f, 0.5f, 0.75f, 1.0f, 2.0f, 4.0f, 8.0f };

// one effect object (FUN_1800fefe0 kind k; FUN_1800ff500 per block)
struct Fx
{
    int kind = 0;
    float fs = 44100.0f;
    float x = 0, y = 0, rate = 1.0f, amount = 0;
    int shape = 0;
    bool off = false;
    double phase = 0.0, inc = 0.0;
    float period = 0.0f, fade = 0.0f;
    int active = 0, tail = 0;
    Biquad flt[8], hpf[8];
    float hold = 0, holdL = 0, holdR = 0;
    float apS[12] {}, apfbL = 0, apfbR = 0, xs = -1.0f;
    bool eqOn = false;
    float eqGain[5] {};
    bool eqDirty[5] { true, true, true, true, true };
    EqBand eqL[5], eqR[5];
    std::vector<TapeDelay> dl;
    std::unique_ptr<Reverb> rev;
    std::vector<float> cl, cr; // reverb input copies

    void init (int k, double rateHz)
    {
        kind = k;
        fs = (float) rateHz;
        x = y = amount = 0.0f, rate = 1.0f, shape = 0, off = false;
        phase = inc = 0.0, period = 0.0f, fade = 0.0f, active = tail = 0;
        for (auto& b : flt)
            b.init (false);
        for (auto& b : hpf)
            b.init (true);
        hold = holdL = holdR = 0.0f;
        eqOn = false;
        for (int i = 0; i < 5; ++i)
            eqGain[i] = 0.0f, eqDirty[i] = true, eqL[i].idx = eqR[i].idx = i, eqL[i].clear(), eqR[i].clear();
        dl.clear();
        if (k == 6)
            dl.resize (2), std::for_each (dl.begin(), dl.end(), [&] (TapeDelay& d) { d.init ((int) (fs * 1.5f)); });
        else if (k == 8)
            dl.resize (2), std::for_each (dl.begin(), dl.end(), [&] (TapeDelay& d) { d.init ((int) ((double) fs * 0.002)); });
        else if (k == 2)
            dl.resize (6), std::for_each (dl.begin(), dl.end(), [&] (TapeDelay& d) { d.init ((int) ((double) fs * 0.002)); });
        rev.reset();
        if (k == 7)
        {
            rev = std::make_unique<Reverb>();
            rev->init (44100.0f); // re-created at its default rate (never told the host rate)
            cl.assign (maxChunk, 0.0f);
            cr.assign (maxChunk, 0.0f);
        }
        reset();
    }
    void reset() noexcept // FUN_1800ff2c0 with an unchanged rate
    {
        for (auto& d : dl)
            d.reset();
        std::fill (std::begin (apS), std::end (apS), 0.0f);
        apfbL = apfbR = 0.0f;
        xs = -1.0f;
        if (rev != nullptr)
            rev->reset();
        hold = 0.0f, holdL = 0.0f;
        for (int i = 0; i < 6; ++i)
            flt[i].clear(), hpf[i].clear();
        for (int i = 0; i < 5; ++i)
            eqL[i].clear(), eqR[i].clear(), eqDirty[i] = true;
        phase = 0.0, fade = 0.0f, tail = 0, active = 0;
    }
    float lfo() noexcept // FUN_1800fea00
    {
        const auto p = inc + phase;
        const auto pf = p - (double) (long long) p;
        if (shape == 0)
        {
            const auto v = (float) std::sin (phase * twoPi);
            phase = pf;
            return v * amount;
        }
        phase = pf;
        auto v = (float) pf;
        if (shape == 1)
        {
            v = v + v;
            if (1.0f < v)
                return (2.0f - v) * amount;
        }
        else if (shape != 2)
        {
            v = 0.5f < v ? 1.0f : 0.0f;
            return v * amount;
        }
        return v * amount;
    }
    void blockPhase (int n) noexcept
    {
        const auto d = (double) (n - 1) * inc + phase;
        phase = d - (double) (long long) d;
    }
    float fadeStep() noexcept
    {
        fade = (((off ? 0.0f : 1.0f) - fade) * 0.01f) + fade;
        return fade;
    }
    bool running() const noexcept { return ! off || 0.0001f <= fade; }

    void process (float* L, float* R, int n, float tempo) noexcept
    {
        if (! (1.0f <= fs && 1.0f <= tempo))
            return;
        const auto per = (rate * 60.0f) / tempo;
        active = 0;
        period = per;
        inc = per == 0.0f ? 0.0 : (double) (1.0f / (fs * per));
        switch (kind)
        {
            case 6: delay (L, R, n); break;
            case 4: lphp (L, R, n, false); break;
            case 5: lphp (L, R, n, true); break;
            case 7: reverb (L, R, n); break;
            case 0: dist (L, R, n); break;
            case 1: crush (L, R, n); break;
            case 2: flanger (L, R, n); break;
            case 9: trancer (L, R, n); break;
            case 3: phaser (L, R, n); break;
            case 8: panner (L, R, n); break;
            default: break;
        }
        if (eqOn)
            eq (L, R, n);
    }
    void dist (float* L, float* R, int n) noexcept
    {
        if (! running())
            return;
        active = 1;
        auto d = lfo() + x;
        d = clamp01 (d);
        blockPhase (n);
        const auto d2 = d * d;
        const auto kk = (d2 * d2) * 1024.0f;
        const auto g = 0.5f / (1.0f / (kk + 1.0f));
        const auto fc = powf32 (2.0f, (y * 4.28000020980835f) + 10.0f);
        flt[0].design (fc, 0.99f, fs);
        flt[1].design (fc, 0.99f, fs);
        for (int i = 0; i < n; ++i)
        {
            auto fd = fadeStep();
            auto v = L[i];
            auto s = (v / ((std::fabs (v) * kk) + 1.0f)) * g;
            L[i] = (flt[0].tick (s) * fd) + ((1.0f - fd) * v);
            v = R[i];
            fd = fade;
            s = (v / ((std::fabs (v) * kk) + 1.0f)) * g;
            R[i] = (flt[1].tick (s) * fd) + ((1.0f - fd) * v);
        }
    }
    void crush (float* L, float* R, int n) noexcept
    {
        if (! running())
            return;
        active = 1;
        auto c = (1.0f - x) + lfo();
        c = std::max (c, 0.0f);
        blockPhase (n);
        c = std::min (c, 1.0f);
        const auto hl = ((c * c) * 64.0f) + 3.0f;
        const auto fc = powf32 (2.0f, (y * 4.28000020980835f) + 10.0f);
        flt[0].design (fc, 0.99f, fs);
        flt[1].design (fc, 0.99f, fs);
        for (int i = 0; i < n; ++i)
        {
            auto h = hold;
            float vL;
            if (0.0f < h)
                vL = holdL;
            else
            {
                vL = L[i];
                holdL = vL;
                holdR = R[i];
                h = hl;
            }
            hold = h - 1.0f;
            auto fd = fadeStep();
            L[i] = ((1.0f - fd) * L[i]) + (fd * flt[0].tick (vL));
            fd = fade;
            R[i] = ((1.0f - fd) * R[i]) + (fd * flt[1].tick (holdR));
        }
    }
    void lphp (float* L, float* R, int n, bool high) noexcept
    {
        if (! running())
            return;
        active = 1;
        auto v = lfo() + x;
        const auto q = ((y * y) + (y * y)) + 1.0f;
        v = clamp01 (v);
        blockPhase (n);
        Biquad* f = high ? hpf : flt;
        const auto fc = high ? powf32 (2.0f, (v * 7.423999786376953f) + 5.0f) : powf32 (2.0f, (v * 8.279999732971191f) + 6.0f);
        for (int k = 0; k < 8; ++k)
            f[k].design (fc, q, fs);
        for (int i = 0; i < n; ++i)
        {
            const auto fd = fadeStep();
            auto t = f[0].tick (f[1].tick (f[2].tick (L[i])));
            L[i] = ((1.0f - fd) * L[i]) + (fade * t);
            t = f[4].tick (f[5].tick (f[6].tick (R[i])));
            R[i] = ((1.0f - fade) * R[i]) + (fade * t);
        }
    }
    void phaser (float* L, float* R, int n) noexcept
    {
        if (! running())
            return;
        active = 1;
        const auto h = 0.5f / fs;
        const auto lo = (float) ((double) h * 100.0), hi = (float) ((double) h * 40000.0);
        const auto rng = hi - lo;
        const auto fb = powf32 (y * 0.6000000238418579f, 0.3333333432674408f) * 0.9800000190734863f;
        auto tgt = x * x;
        tgt = std::max (tgt, 9.99999975e-06f);
        if (xs < 0.0f)
            xs = tgt;
        for (int i = 0; i < n; ++i)
        {
            fadeStep();
            const auto s = xs;
            const auto lv = lfo();
            auto w = ((s + ((lv * s) * 0.5f)) * rng) + lo;
            w = std::min (w, 0.49900001287460327f);
            if ((double) w < 0.001)
                w = 0.00100000005f;
            const auto a = (1.0f - w) / (w + 1.0f);
            xs = (float) ((double) (tgt - s) * 0.001 + (double) s);
            auto u = (L[i] - 9.99999997e-07f) + (fb * apfbL);
            auto pr = a * u;
            for (int k = 0; k < 6; ++k)
            {
                const auto t = apS[k] - pr;
                pr = t * a;
                apS[k] = pr + u;
                u = t;
            }
            apfbL = u;
            const auto outL = u;
            u = (R[i] - 9.99999997e-07f) + (fb * apfbR);
            pr = a * u;
            for (int k = 6; k < 12; ++k)
            {
                const auto t = apS[k] - pr;
                pr = t * a;
                apS[k] = pr + u;
                u = t;
            }
            apfbR = u;
            L[i] = (outL * fade) + L[i];
            R[i] = (u * fade) + R[i];
        }
    }
    void trancer (float* L, float* R, int n) noexcept
    {
        if (off)
            return;
        active = 1;
        if (! (0.0f < rate))
            return;
        const auto dep = 1.0f - (y * y);
        const auto sh = (((1.0f - x) * (1.0f - x)) * 50.0f) + 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const auto p = phase;
            float v;
            if (0.009999999776482582 <= p)
            {
                v = (float) ((p - 0.009999999776482582) * 1.0101010003706326);
                v = v * v;
                v = (v * v) * sh;
                v = std::min (v, 1.0f);
                v = 1.0f - v;
            }
            else
                v = (float) (p * 100.00000223517424);
            hold = ((v - hold) * 0.01f) + hold;
            const auto g = 1.0f - (hold * dep);
            const auto q = p + inc;
            phase = q - (double) (long long) q;
            L[i] = g * L[i];
            R[i] = g * R[i];
        }
    }
    void delay (float* L, float* R, int n) noexcept
    {
        if (off && tail < 0)
            return;
        active = 1;
        auto dt = period * 0.6666666865348816f;
        if ((double) dt < 0.01)
            dt = 0.00999999978f;
        const auto y2 = y + y;
        const auto lpv = std::min (y2, 1.0f);
        const auto hpv = std::max (y2 - 1.0f, 0.0f);
        const auto xx = x * x;
        for (auto& d : dl)
        {
            d.lp = lpv * lpv;
            d.hp = hpv * hpv;
            d.fb = xx;
            d.setTime ((double) dt);
        }
        const auto thr = 9.99999975e-05f;
        for (int i = 0; i < n; ++i)
        {
            const auto fd = fadeStep();
            const auto g = fd * xx;
            dl[0].gain = g;
            dl[1].gain = g;
            const auto a = dl[0].tick (L[i]);
            const auto b = dl[1].tick (R[i]);
            if (thr < std::fabs (a) || thr < std::fabs (b) || thr < std::fabs (L[i]) || thr < std::fabs (R[i]))
                tail = (int) (fs * 1.5f);
            else
                --tail;
            L[i] = a + L[i];
            R[i] = b + R[i];
        }
    }
    void flanger (float* L, float* R, int n) noexcept
    {
        if (! running())
            return;
        active = 1;
        const auto fbv = y * -0.5f, base = 1.0f - x;
        for (auto& d : dl)
            d.fb = fbv;
        for (int i = 0; i < n; ++i)
        {
            const auto xl = L[i], xr = R[i];
            const auto lv = lfo();
            fadeStep();
            const auto g = -fade;
            for (auto& d : dl)
            {
                d.setTime ((double) (base - lv));
                d.gain = g;
            }
            const auto a = dl[0].tick (dl[1].tick (dl[2].tick (xl)));
            const auto b = dl[3].tick (dl[4].tick (dl[5].tick (xr)));
            L[i] = a + L[i];
            R[i] = b + R[i];
        }
    }
    void panner (float* L, float* R, int n) noexcept
    {
        if (off && ! (0 < tail))
            return;
        active = 1;
        for (auto& d : dl)
            d.fb = 0.0f, d.gain = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            auto v = lfo() + x;
            v = v + v;
            auto a = v - 1.0f;
            dl[0].setTime ((double) clamp01 (a));
            dl[1].setTime ((double) clamp01 (1.0f - v));
            a = a * y;
            const auto gl = clamp01 ((1.0f - a) + (1.0f - a));
            L[i] = dl[0].tick (gl * L[i]);
            const auto gr = clamp01 ((a + a) + 2.0f);
            R[i] = dl[1].tick (gr * R[i]);
        }
    }
    void reverb (float* L, float* R, int n) noexcept // FUN_180100950
    {
        if (off && tail < 1)
            return;
        active = 1;
        const auto tgt = off ? 0.0f : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            fade = ((tgt - fade) * 0.01f) + fade;
            cl[(std::size_t) i] = fade * L[i];
            cr[(std::size_t) i] = fade * R[i];
        }
        const auto x3 = ((x * x) * x) * 0.75f;
        rev->setp (9, y * y);
        rev->setp (8, x3);
        rev->setp (12, (x3 * 0.30000001192092896f) + 0.5f);
        rev->setp (2, 1.0f);
        rev->setp (1, 0.10000000149011612f);
        rev->setp (5, 1.0f);
        rev->setp (4, 1.0f);
        rev->setp (3, 0.0f);
        rev->setp (10, 0.0f);
        rev->setp (16, 1.0f);
        rev->process (cl.data(), cr.data(), L, R, n);
        const auto thr = 9.99999975e-05f;
        const auto full = (int) (fs * 0.25f);
        for (int i = 0; i < n; ++i)
            if (thr < std::fabs (L[i]) || thr < std::fabs (R[i]))
                tail = full;
            else
                --tail;
    }
    void eq (float* L, float* R, int n) noexcept
    {
        active = 1;
        for (int b = 0; b < 5; ++b)
        {
            if (eqDirty[b])
            {
                eqDirty[b] = false;
                const auto g = (eqGain[b] - 0.5f) * 24.0f;
                eqL[b].design (g, fs);
                eqR[b].design (g, fs);
            }
            for (int i = 0; i < n; ++i)
            {
                L[i] = eqL[b].tick (L[i]);
                R[i] = eqR[b].tick (R[i]);
            }
        }
    }
    void setEq (const float* gains, bool on) noexcept
    {
        eqOn = on;
        if (! on)
            return;
        for (int b = 0; b < 5; ++b)
        {
            const auto g = clamp01 (gains[b]);
            if (g != eqGain[b])
                eqDirty[b] = true;
            eqGain[b] = g;
        }
    }
};

// ------------------------------------------------------------------------------------------------------- engine

struct Voice
{
    bool active = false, released = false, rendered = false, flagRand = false;
    int note = -1, notep = 0;
    std::uint64_t counter = 0;
    float vel = 0, pitch = 0;
    float eAmp = 0, ampT = 0, ampC = 0, ampCd = 0, ampS = 0;
    float e1 = 0, e1t = 0, e1c = 0, e1cd = 0, e2 = 0, e2t = 0, e2c = 0, e2cd = 0;
    float lvl = 0, lvlT = 0;
    float y[2][4] {};
    float kt = 0, res = 0, amtE1 = 0, amtE2 = 0, amtL1 = 0, amtL2 = 0, lv1 = 0, lv2 = 0;
    std::uint32_t ph1[16] {}, phm[16] {}, ph2[16] {}, ph3[16] {};
    std::uint32_t inc1[16] {}, incm[16] {}, inc2[16] {}, inc3[16] {};
    float g[8][16] {}; // rows: 1L 1R 2L 2R 3L 3R nL nR
    int tab[3] { 511, 511, 511 };
};

class Engine
{
public:
    double fs = 44100.0;
    std::array<float, paramCount> P {};
    std::shared_ptr<const Tables> tabs[3];
    float velTab[128] {};
    float velsrc = 0.0f, lastNote = -1.0f, bend = 0.0f;
    std::uint64_t counter = 1;
    float lfoPh[2] {}, lfoInc[2] {};
    float cutS = -1.0f, cutRaw = 0.0f, modS = -1.0f;
    float mgL = -1.0f, mgR = -1.0f;
    int maxpoly = voiceCount, npoly2 = voiceCount;
    float tempo = 140.0f;
    bool playing = false;
    double songpos = 0.0;
    Rng rng;
    std::vector<Voice> voices;
    std::vector<Fx> fx;
    bool fxOn = true;
    bool lastActive = false;

    // per-chunk state
    int dstE1 = 0, dstE2 = 0, dstL1 = 0, dstL2 = 0, shpL1 = 0, shpL2 = 0, U = 1, ftype = 0, kind = 0;
    std::uint32_t ph2ofs = 0;
    float bufCut[maxChunk] {}, bufMod[maxChunk] {}, bufL1[maxChunk] {}, bufL2[maxChunk] {};
    float ampb[maxChunk] {}, cutv[maxChunk] {}, lvlb[maxChunk] {}, modb[maxChunk] {}, vbL[maxChunk] {}, vbR[maxChunk] {};

    void init (double rate, bool withFx)
    {
        fs = rate;
        voices.assign (voiceCount, Voice {});
        rng.s = 0;
        // the constructor and the sample-rate set each reset all 99 voices (16 × 93 draws each); a rate other than the
        // constructor's 44 100 Hz causes one more reset (§1.7)
        const int nres = fs == 44100.0 ? 2 : 3;
        rng.skip ((std::uint64_t) (nres - 1) * voiceCount * 16 * 93);
        for (auto& v : voices)
            resetVoice (v);
        std::fill (std::begin (velTab), std::end (velTab), 0.0f);
        velsrc = 0.0f, counter = 1, lastNote = -1.0f;
        lfoPh[0] = lfoPh[1] = lfoInc[0] = lfoInc[1] = 0.0f;
        cutS = -1.0f, cutRaw = 0.0f, modS = -1.0f, bend = 0.0f, mgL = mgR = -1.0f;
        maxpoly = npoly2 = voiceCount;
        playing = false, songpos = 0.0;
        fxOn = withFx;
        fx.clear();
        if (withFx)
        {
            fx.resize (10);
            for (int k = 0; k < 10; ++k)
                fx[(std::size_t) k].init (k, fs);
        }
        lastActive = false;
    }

    float raw (int i) const noexcept { return i >= 0 && i < paramCount ? P[(std::size_t) i] : 0.0f; }

    float par (int i) const noexcept // FUN_1800f50c0
    {
        auto v = raw (i);
        const int rows[3][2] = { { 199, 198 }, { 202, 201 }, { 204, 203 } };
        const float src[3] = { velsrc, 0.0f, 0.0f };
        for (int r = 0; r < 3; ++r)
        {
            const auto d = raw (rows[r][0]);
            if (d != 0.0f && matrixHit (i, i32 ((double) d)))
            {
                const auto a = raw (rows[r][1]);
                v = (((a - 0.5f) + a) - 0.5f) * src[r] + v;
                if (v <= 0.0f)
                    v = 0.0f;
                if (1.0f <= v)
                    v = 1.0f;
            }
        }
        return v;
    }

    int transposed (int note) const noexcept
    {
        auto n2 = note - i32 ((double) (par (2) * -48.0f + 24.0f));
        n2 = n2 + (i32 ((double) (par (44) * -4.0f)) + 2) * -12;
        return n2;
    }

    void noteOn (int note, int vel7) noexcept // FUN_1800ccd70 → FUN_1800f5250 / FUN_1800f53e0
    {
        velTab[note] = (float) vel7 * 0.007874015718698502f;
        if (vel7 == 0)
        {
            noteOff (note);
            return;
        }
        velsrc = velTab[note];
        Voice* v = nullptr;
        if (par (40) == 0.0f)
        {
            const auto lim = std::min (npoly2, maxpoly);
            for (int k = 0; k < lim; ++k)
                if (! voices[(std::size_t) k].active)
                    v = &voices[(std::size_t) k];
            if (v == nullptr)
            {
                std::uint64_t best = 0xefffffffu;
                for (int k = 0; k < lim; ++k)
                {
                    const auto c = voices[(std::size_t) k].counter;
                    if (c < best)
                        v = &voices[(std::size_t) k];
                    if (best > c)
                        best = c;
                }
            }
            if (v == nullptr)
                return;
        }
        else
            v = &voices[0];
        v->note = note;
        v->notep = transposed (note);
        v->vel = velsrc;
        v->counter = counter++;
        start (*v);
        lastNote = (float) v->notep;
    }

    void start (Voice& v) noexcept
    {
        v.active = true, v.released = false;
        v.lv1 = v.lv2 = 0.0f;
        const auto p8 = par (8);
        v.lvlT = p8 * p8;
        if (v.lvl < 0.0f)
            v.lvl = v.lvlT;
        const auto p5 = par (5), p4 = par (4);
        const auto p5s = p5 * p5;
        const auto dec = (double) (p5s * p5s * 50.0f) + 0.001;
        const auto att = (float) (8.82 / (((double) (p4 * p4 * p4 * 50.0f) + 0.001) * fs));
        v.ampCd = (float) (8.82 / (dec * fs));
        v.ampS = par (6);
        v.ampC = att;
        v.ampT = 5.0f;
        for (int e = 1; e <= 2; ++e)
        {
            const auto x = par (21 + e) * 0.800000011920929f + 0.20000000298023224f;
            const auto a = par (19 + e);
            const auto ca = (float) (8.82 / (((double) (a * a * a * a * a * 300.0f) + 0.001) * fs));
            const auto cd = (float) (8.82 / (((double) (x * x * x * x * (x * 70.0f)) + 0.001) * fs));
            if (e == 1)
                v.e1t = 5.0f, v.e1c = ca, v.e1 = 0.0f, v.e1cd = cd;
            else
                v.e2t = 5.0f, v.e2c = ca, v.e2 = 0.0f, v.e2cd = cd;
        }
        if (par (11) != 0.0f && lastNote != -1.0f)
            v.pitch = lastNote;
        if (par (47) != 0.0f)
        {
            const auto m = par (46) * 4.0f;
            for (int k = 0; k < 16; ++k)
            {
                const auto ph = phaseOfs (0, (float) k * m);
                v.ph1[k] = v.phm[k] = v.ph2[k] = v.ph3[k] = ph;
            }
            v.flagRand = false;
        }
        else
            v.flagRand = par (53) != 0.0f;
        if (par (26) != 0.0f)
            lfoPh[0] = 0.0f;
        if (par (27) != 0.0f)
            lfoPh[1] = 0.0f;
    }

    void noteOff (int note) noexcept // FUN_1800ccdb0 + FUN_1800f58e0
    {
        velTab[note] = 0.0f;
        const auto x = (float) ((double) par (7) * 0.55 + 0.1);
        const auto c = (float) (8.82 / (((double) (x * x * x * 25.0f) + 0.001) * fs));
        for (auto& v : voices)
            if (v.note == note && v.active)
                v.ampC = c, v.released = true, v.ampT = 0.0f;
    }

    void resetVoice (Voice& v) noexcept // FUN_1800fe890
    {
        v.active = false, v.released = false, v.note = -1;
        std::memset (v.y, 0, sizeof (v.y));
        v.lvl = -1.0f, v.lvlT = 1.0f, v.eAmp = 0.0f, v.e1 = 0.0f, v.e2 = 0.0f;
        for (int k = 0; k < 16; ++k)
        {
            const auto r = rng.next();
            v.ph1[k] = r, v.phm[k] = r;
            rng.skip (30);
            v.ph2[k] = rng.next();
            rng.skip (30);
            v.ph3[k] = rng.next();
            rng.skip (30);
        }
    }

    bool anyActive() const noexcept
    {
        return std::any_of (voices.begin(), voices.end(), [] (const Voice& v) { return v.active; });
    }

    // FUN_1800f32f0 for one chunk of n samples (adds nothing: writes outL/outR)
    void process (int n, const float (*LR)[2], const float* ptab, float* outL, float* outR) noexcept
    {
        std::fill (outL, outL + n, 0.0f);
        std::fill (outR, outR + n, 0.0f);
        const auto anyVoice = f5f00 (n, LR, ptab, outL, outR);
        auto active = anyVoice;
        if (fxOn)
        {
            auto act = anyVoice;
            for (auto& f : fx)
            {
                if (! act)
                    f.off = true;
                f.process (outL, outR, n, tempo);
                act = act || f.active != 0;
            }
            active = act || active;
        }
        lastActive = active;
        const auto vol = db2lin (raw (0) * 80.0f - 60.0f);
        const auto pan = raw (1);
        auto Lt = clamp01 (1.0f - pan) * vol;
        auto Rt = clamp01 (pan) * vol;
        Lt = std::max (Lt, 9.999999747378752e-05f);
        Rt = std::max (Rt, 9.999999747378752e-05f);
        if (active)
        {
            auto cl = mgL;
            if (! (cl >= 0.0f))
                mgL = Lt, cl = Lt;
            const auto cr = mgR;
            if (! (cr >= 0.0f))
                mgL = Rt, cl = Rt;
            if (! (cl >= 9.999999747378752e-05f))
                mgL = 0.0f;
            if (! (cr >= 9.999999747378752e-05f))
                mgR = 0.0f;
            auto gl = mgL, gr = mgR;
            const auto c = 0.009999999776482582f;
            for (int i = 0; i < n; ++i)
            {
                gl = (Lt - gl) * c + gl;
                gr = (Rt - gr) * c + gr;
                outL[i] = gl * outL[i];
                outR[i] = gr * outR[i];
            }
            mgL = gl, mgR = gr;
        }
        else
            mgL = Lt, mgR = Rt;
    }

    void pushFx() noexcept // effect-slot parameters (FUN_1800f5f00)
    {
        if (! fxOn)
            return;
        for (int k = 0; k < 10; ++k)
        {
            auto& f = fx[(std::size_t) k];
            const auto on = P[(std::size_t) (147 + k)] != 0.0f;
            f.off = ! on;
            if (! on)
                continue;
            f.x = clamp01 (P[(std::size_t) (117 + k)]);
            f.y = clamp01 (P[(std::size_t) (127 + k)]);
            const auto a = (P[(std::size_t) (177 + k)] + P[(std::size_t) (177 + k)]) - 1.0f;
            auto amt = a * a;
            if (a < 0.0f)
                amt = -amt;
            f.amount = amt;
            const auto shv = (double) (P[(std::size_t) (157 + k)] * 3.0f);
            f.shape = shv != shv ? 0 : (int) std::clamp (shv, 0.0, 4.0);
            const auto riv = (double) (P[(std::size_t) (137 + k)] * 9.0f);
            auto r = rateSteps[riv != riv ? 0 : (int) std::clamp (riv, 0.0, 8.0)];
            if (P[(std::size_t) (167 + k)] != 0.0f)
                r = r * 4.0f;
            f.rate = std::max (r, 0.0f);
            if (playing) // song-time LFO phase (engine flag +0x4470)
            {
                const auto d = f.rate <= 0.0f ? f.phase : (0.25 / (double) f.rate) * songpos * 4.0;
                f.phase = d - (double) (long long) d;
            }
        }
        fx[9].setEq (P.data() + 207, P[206] != 0.0f);
    }

    bool f5f00 (int n, const float (*LR)[2], const float* ptab, float* outL, float* outR) noexcept
    {
        for (int l = 0; l < 2; ++l)
        {
            const auto lock = playing && par (26 + l) == 0.0f;
            const auto r = lfoRate (par (34 + l), par (36 + l), tempo, lock, lock ? (float) (songpos * 4.0) : 0.0f);
            if (r.hasPhase)
                lfoPh[l] = r.phase;
            lfoInc[l] = (float) ((double) r.f / fs);
        }
        pushFx();
        bool any = false;
        for (auto& v : voices)
        {
            v.rendered = false;
            any = any || v.active;
        }
        if (! any)
            return false;
        dstE1 = i32 ((double) (par (24) * 3.0f));
        dstE2 = i32 ((double) (par (25) * 3.0f));
        dstL1 = i32 ((double) (par (38) * 3.0f));
        dstL2 = i32 ((double) (par (39) * 3.0f));
        shpL1 = i32 ((double) (par (32) * 3.0f));
        shpL2 = i32 ((double) (par (33) * 3.0f));
        f68f0 (LR, ptab);
        return f7530 (n, outL, outR);
    }

    void f68f0 (const float (*LR)[2], const float* ptab) noexcept
    {
        U = std::clamp (1 - i32 ((double) (par (41) * -15.0f)), 1, 16);
        auto spread = (float) ((double) par (42) * 0.4);
        spread = spread * spread;
        auto a = par (48);
        a = a * a;
        auto b = par (49);
        b = b * b;
        auto nz = par (45);
        nz = nz * nz;
        const auto one = 1.0f;
        const auto x1 = (one - a) * (one - b) * (one - nz);
        const auto x2 = (one - b) * a * (one - nz);
        const auto x3 = (one - nz) * b;
        cutRaw = raw (12);
        if (cutS < 0.0f)
            cutS = cutRaw;
        ftype = i32 ((double) (par (15) + par (15)));
        ph2ofs = phaseOfs (0, par (52) + par (52));
        kind = i32 ((double) (par (51) + par (51)));
        for (auto& v : voices)
        {
            if (! v.active)
                continue;
            velsrc = v.vel;
            auto amt = par (16);
            amt = amt * amt;
            v.amtE1 = par (18) != 0.0f ? -amt : amt;
            amt = par (17);
            amt = amt * amt;
            v.amtE2 = par (19) != 0.0f ? -amt : amt;
            amt = par (28);
            amt = amt * amt;
            v.amtL1 = par (30) != 0.0f ? -amt : amt;
            amt = par (29);
            amt = amt * amt;
            v.amtL2 = par (31) != 0.0f ? -amt : amt;
            if (! (v.ampT != 0.0f || 9.999999747378752e-05f <= v.eAmp))
            {
                resetVoice (v);
                continue;
            }
            const auto p14 = par (14);
            v.kt = p14 == 0.0f ? 0.0f : p14 * (v.pitch - 57.0f) * 0.0078125f;
            auto lv = db2lin (par (8) * 50.0f - 25.0f) * 200.0f;
            if (par (200) != 0.0f)
                lv = lv * v.vel;
            v.lvlT = lv;
            v.kt = (par (12) - cutRaw) + v.kt;
            v.res = par (13);
            v.notep = transposed (v.note);
            if (par (11) == 0.0f)
                v.pitch = (float) v.notep;
            const auto e1o = v.e1, e2o = v.e2, l1o = v.lv1, l2o = v.lv2;
            auto pv = (float) i32 ((double) (par (205) * 12.0f)) * bend + v.pitch;
            const auto de = dstE1, d2 = dstE2, dl1 = dstL1, dl2 = dstL2;
            auto fb = [] (bool c) { return c ? 1.0f : 0.0f; };
            auto t = fb (d2 == 2) * v.amtE2 * e2o + fb (de == 2) * v.amtE1 * e1o;
            t = t * 24.0f;
            auto u = fb (dl2 == 2) * l2o + fb (dl1 == 2) * l1o;
            u = u * 12.0f;
            pv = pv + (t + u);
            pv = pv - 57.0f;
            pv = pv + ptab[v.note];
            auto ofs = [&] (int cents, int semi) {
                return (float) ((double) i32 ((double) (par (cents) * 100.0f - 50.0f)) * 0.01 + (double) (float) i32 ((double) (par (semi) * 72.0f - 36.0f)));
            };
            const auto o1 = ofs (56, 55), o2 = ofs (58, 57);
            float m;
            if (kind == 0)
            {
                m = par (50) + fb (de == 1) * v.amtE1 * e1o;
                m = m + ((fb (dl2 == 1) * l2o + fb (dl1 == 1) * l1o) + fb (d2 == 1) * v.amtE2 * e2o);
                m = clamp01 (m) * 3.0f + 1.0f;
            }
            else
                m = 1.0f;
            const auto k12 = 0.0833333358168602f;
            const auto base = powf32 (2.0f, pv * k12) * 220.0f;
            const auto f1 = powf32 (2.0f, o1 * k12) * base;
            const auto f2 = powf32 (2.0f, o2 * k12) * base;
            const auto o3 = ofs (60, 59);
            const auto f3 = powf32 (2.0f, o3 * k12) * base;
            const auto fsync = m * f1;
            v.tab[0] = tableFor (fsync, fs), v.tab[1] = tableFor (f2, fs), v.tab[2] = tableFor (f3, fs);
            const auto st = par (43);
            const auto norm = 0.004999999888241291f / ((float) (U - 1) * 0.15000000596046448f + one);
            const auto Lg = LR[v.note][0], Rg = LR[v.note][1];
            for (int k = 0; k < 16; ++k)
            {
                auto det = one;
                if (U > 1)
                    det = powf32 (2.0f, ((float) k / (float) (U - 1) - 0.5f) * spread);
                v.inc1[k] = phaseInc (fsync * det, fs);
                v.incm[k] = phaseInc (det * f1, fs);
                v.inc2[k] = phaseInc (det * f2, fs);
                v.inc3[k] = phaseInc (det * f3, fs);
                auto s = (k & 1) == 0 ? one : -1.0f;
                if ((U & 1) && k == 0)
                    s = 0.0f;
                s = s * st;
                const auto gl = clamp01 (one - s) * Lg;
                const auto gr = clamp01 (s + one) * Rg;
                v.g[0][k] = gl * x1 * norm, v.g[1][k] = gr * x1 * norm;
                v.g[2][k] = gl * x2 * norm, v.g[3][k] = gr * x2 * norm;
                v.g[4][k] = gl * x3 * norm, v.g[5][k] = gr * x3 * norm;
                v.g[6][k] = gl * nz * norm, v.g[7][k] = gr * nz * norm;
                if (v.flagRand)
                {
                    const auto r = (std::uint32_t) (((std::uint64_t) rng.next() + 1) & 0x1ff);
                    v.ph1[k] = r * v.inc1[k];
                    v.ph2[k] = r * v.inc2[k];
                    v.phm[k] = v.ph1[k];
                }
            }
            v.flagRand = false;
        }
    }

    // FUN_1800f7530 (envelopes, glide, level, modulation, oscillators) + the ladder of FUN_1800f32f0 per voice
    bool f7530 (int n, float* outL, float* outR) noexcept
    {
        const auto inv2 = par (54) != 0.0f;
        auto sl = par (10);
        sl = sl * sl * sl;
        const auto gc = (float) (8.82 / (((double) (sl + sl) + 0.001) * fs));
        auto m = par (50);
        m = m * m;
        if (modS < 0.0f)
            modS = m;
        const auto c01 = 0.009999999776482582f;
        bool first = true, rendered = false;
        for (auto& v : voices)
        {
            if (! v.active)
                continue;
            if (first)
            {
                auto cs = cutS, ms = modS, p1 = lfoPh[0], p2 = lfoPh[1];
                for (int i = 0; i < n; ++i)
                {
                    cs = (cutRaw - cs) * c01 + cs;
                    bufCut[i] = cs;
                    ms = (m - ms) * c01 + ms;
                    bufMod[i] = ms;
                    bufL1[i] = lfoWave (shpL1, p1);
                    auto t = lfoInc[0] + p1;
                    p1 = t - (float) i32 ((double) t);
                    bufL2[i] = lfoWave (shpL2, p2);
                    t = lfoInc[1] + p2;
                    p2 = t - (float) i32 ((double) t);
                }
                cutS = cs, modS = ms, lfoPh[0] = p1, lfoPh[1] = p2;
                first = false;
            }
            v.rendered = true;
            rendered = true;
            renderVoice (v, n, inv2, gc, c01);
            filter (v, n, outL, outR);
        }
        return rendered;
    }

    void renderVoice (Voice& v, int n, bool inv2, float gc, float c01) noexcept
    {
        auto e1 = v.e1, e1t = v.e1t, e1c = v.e1c, e2 = v.e2, e2t = v.e2t, e2c = v.e2c;
        auto ea = v.eAmp, eat = v.ampT, eac = v.ampC;
        auto lv = v.lvl;
        const auto lvt = v.lvlT;
        const auto de = dstE1, d2 = dstE2, dl1 = dstL1, dl2 = dstL2;
        const auto a1 = v.amtE1, a2 = v.amtE2, al1 = v.amtL1, al2 = v.amtL2;
        auto pitch = v.pitch;
        const auto tgt = (float) v.notep;
        float x1 = 0.0f, x2 = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            e1 = e1 + (e1t - e1) * e1c;
            e2 = e2 + (e2t - e2) * e2c;
            ea = ea + (eat - ea) * eac;
            lv = lv + (lvt - lv) * c01;
            pitch = (tgt - pitch) * gc + pitch;
            if (i32 ((double) e1) != 0)
                e1c = v.e1cd, e1 = 1.0f, e1t = 0.0f;
            if (i32 ((double) e2) != 0)
                e2c = v.e2cd, e2 = 1.0f, e2t = 0.0f;
            if (i32 ((double) ea) != 0)
                eac = v.ampCd, eat = v.ampS, ea = 1.0f;
            lvlb[i] = lv;
            x1 = bufL1[i] * al1;
            if (de == 3)
                x1 = x1 * e1;
            x2 = bufL2[i] * al2;
            if (d2 == 3)
                x2 = x2 * e2;
            if (kind == 1 || kind == 2)
            {
                auto mm = bufMod[i];
                if (de == 1)
                    mm = mm + e1 * a1;
                if (d2 == 1)
                    mm = mm + e2 * a2;
                if (dl1 == 1)
                    mm = mm + x1;
                if (dl2 == 1)
                    mm = mm + x2;
                if (mm <= 0.0f)
                    mm = 0.0f;
                modb[i] = mm * (kind == 1 ? 15.0f : 3.0f);
            }
            else
                modb[i] = 0.0f;
            auto aa = ea * ea;
            if (dl1 == 3)
                aa = aa + x1;
            if (dl2 == 3)
                aa = aa + x2;
            if (aa <= 0.0f)
                aa = 0.0f;
            ampb[i] = aa;
            auto cc = bufCut[i] + v.kt;
            if (de == 0)
                cc = cc + e1 * a1;
            if (d2 == 0)
                cc = cc + e2 * a2;
            if (dl1 == 0)
                cc = cc + x1;
            if (dl2 == 0)
                cc = cc + x2;
            cutv[i] = cc;
        }
        v.lv1 = x1, v.lv2 = x2;
        v.e1 = e1, v.e1t = e1t, v.e1c = e1c;
        v.e2 = e2, v.e2t = e2t, v.e2c = e2c;
        v.eAmp = ea, v.ampT = eat, v.ampC = eac;
        v.lvl = lv, v.pitch = pitch;

        // oscillators: unison slot outer, samples inner; four draws per sample (osc2, osc1, osc3, noise)
        const auto* T1 = tabs[0]->data() + (std::size_t) v.tab[0] * tableSize;
        const auto* T2 = tabs[1]->data() + (std::size_t) v.tab[1] * tableSize;
        const auto* T3 = tabs[2]->data() + (std::size_t) v.tab[2] * tableSize;
        std::fill (vbL, vbL + n, 0.0f);
        std::fill (vbR, vbR + n, 0.0f);
        for (int u = 0; u < U; ++u)
        {
            auto ph1 = v.ph1[u], phm = v.phm[u], ph2 = v.ph2[u], ph3 = v.ph3[u];
            const auto inc1 = v.inc1[u], incm = v.incm[u], inc2 = v.inc2[u], inc3 = v.inc3[u];
            const auto g0 = v.g[0][u], g1 = v.g[1][u], g2 = v.g[2][u], g3 = v.g[3][u];
            const auto g4 = v.g[4][u], g5 = v.g[5][u], g6 = v.g[6][u], g7 = v.g[7][u];
            for (int i = 0; i < n; ++i)
            {
                const auto r0 = rng.next(), r1 = rng.next(), r2 = rng.next(), r3 = rng.next();
                auto o2 = T2[(((r0 & 0x3fffffu) + (ph2 + ph2ofs)) >> 22)];
                if (inv2)
                    o2 = -o2;
                float o1;
                if (kind == 1)
                {
                    const auto xx = (double) (o2 * modb[i]) * 4194303.0 + 268435392.0;
                    const auto idx = (std::uint32_t) (std::int64_t) std::clamp (xx, -9.0e18, 9.0e18) * 256u + ph1;
                    o1 = T1[((r1 & 0x3fffffu) + idx) >> 22];
                    ph1 += inc1;
                }
                else if (kind == 0)
                {
                    const auto pmNew = phm + incm;
                    if (pmNew < phm)
                        ph1 = pmNew;
                    phm = pmNew;
                    o1 = T1[((r1 & 0x3fffffu) + ph1) >> 22];
                    ph1 += inc1;
                }
                else
                {
                    o1 = T1[((r1 & 0x3fffffu) + ph1) >> 22];
                    if (kind == 2)
                        o1 = o1 * (1.0f - o2 * modb[i]);
                    ph1 += inc1;
                }
                const auto o3 = T3[((r2 & 0x3fffffu) + ph3) >> 22];
                const auto nzv = ((float) r3 * 2.3283064365386963e-10f) - 0.5f;
                auto xl = nzv * g6 + vbL[i];
                xl = o3 * g4 + xl;
                vbL[i] = xl + (o1 * g0 + o2 * g2);
                auto xr = nzv * g7 + vbR[i];
                xr = xr + o3 * g5;
                vbR[i] = xr + (o1 * g1 + o2 * g3);
                ph2 += inc2;
                ph3 += inc3;
            }
            v.ph1[u] = ph1, v.phm[u] = phm, v.ph2[u] = ph2, v.ph3[u] = ph3;
        }
        for (int i = 0; i < n; ++i)
        {
            vbL[i] = vbL[i] * ampb[i];
            vbR[i] = vbR[i] * ampb[i];
        }
    }

    void filter (Voice& v, int n, float* outL, float* outR) noexcept
    {
        const auto& ct = cutoffTable();
        const auto ft = ftype & 0xff;
        const auto res = v.res;
        const auto c004 = 0.03999999910593033f, one = 1.0f;
        for (int lane = 0; lane < 2; ++lane)
        {
            const auto* buf = lane == 0 ? vbL : vbR;
            auto* out = lane == 0 ? outL : outR;
            auto y1 = v.y[lane][0], y2 = v.y[lane][1], y3 = v.y[lane][2], y4 = v.y[lane][3];
            for (int i = 0; i < n; ++i)
            {
                const auto cx = (double) (cutv[i] * 65536.0f);
                const auto idx = cx != cx ? 0 : (std::size_t) std::clamp (std::trunc (cx), 0.0, 65535.0);
                const auto g = ct.g[idx];
                const auto kr = ct.k[idx] * res;
                const auto x = buf[i];
                float s = 0.0f;
                for (int it = 0; it < 4; ++it)
                {
                    s = x - kr * y4;
                    s = s / (s * s * c004 + one);
                    y1 = y1 + (s - y1) * g;
                    y2 = y2 + (y1 - y2) * g;
                    y3 = y3 + (y2 - y3) * g;
                    y4 = y4 + (y3 - y4) * g;
                }
                if (ft == 0)
                    out[i] = y4 * lvlb[i] + out[i];
                else if (ft == 1)
                    out[i] = (y3 - y4) * lvlb[i] + out[i];
                else if (ft == 2)
                    out[i] = (s - y4) * lvlb[i] + out[i];
            }
            v.y[lane][0] = y1, v.y[lane][1] = y2, v.y[lane][2] = y3, v.y[lane][3] = y4;
        }
    }
};

// ------------------------------------------------------------------------------------------------------- wrapper

void lawA (double pan, double vol, float& l, float& r) noexcept
{
    const auto t = (1.0 + std::clamp (pan, -1.0, 1.0)) * 0.78539816339744831;
    l = (float) (vol * std::cos (t));
    r = (float) (vol * std::sin (t));
}

int velFromInitVol (double iv) noexcept // CFLWrapper TriggerVoice (round 28 §4.2)
{
    const auto x = (float) iv * 10.0f + 1.0f;
    const auto v = (float) std::log ((double) x) * 0.3284587264060974f * 127.0f;
    return std::clamp (i32 ((double) v), 0, 127);
}

// The FL wrapper (CFLWrapper): held notes with their FinalLevels, per-note tables, Gen_Render chunking, host-voice kill.
class Plugin
{
public:
    struct Held
    {
        bool on = false;
        double fvol = 1.0, fpan = 0.0, pitchCents = 0.0; // Final.Vol, Final.Pan, Final.Pitch − 100·(key − 60)
        bool down = false; // the key's note off has not come yet
        HostSlide slide;   // a slide note moving Final.Pitch (issue #14)
    };
    Engine eng;
    Held held[128];
    float LR[128][2] {};
    float ptab[128] {};
    float chunkL[maxChunk] {}, chunkR[maxChunk] {};

    void init (double fs, bool withFx)
    {
        eng.init (fs, withFx);
        for (auto& h : held)
            h = Held {};
    }

    void noteOn (int key, double initVol, double fvol, double fpan, double pitchCents) noexcept
    {
        key = std::clamp (key, 0, 127);
        if (held[key].on)
            eng.noteOff (key);
        held[key] = { true, fvol, fpan, pitchCents, true, HostSlide {} };
        eng.noteOn (key, velFromInitVol (initVol));
    }
    void noteOff (int key) noexcept
    {
        if (key >= 0 && key < 128)
        {
            held[key].down = false;
            eng.noteOff (key);
        }
    }
    // a slide note (issue #14): the held keys glide to `to` (Final.Pitch, cents from C5) over `samples`
    void slideTo (double to, float samples) noexcept
    {
        for (int k = 0; k < 128; ++k)
            if (auto& h = held[k]; h.on && h.down)
                h.slide.start ((float) (to - (100.0 * k - 6000.0 + h.pitchCents - (double) h.slide.cents)), samples);
    }
    void setLevels (int key, double fvol, double fpan) noexcept
    {
        if (key >= 0 && key < 128 && held[key].on)
            held[key].fvol = fvol, held[key].fpan = fpan;
    }

    void tables() noexcept // FUN_1800c91b0
    {
        float cl = 0.0f, cr = 0.0f, cp = 0.0f;
        bool first = true;
        for (int i = 0; i < 128; ++i)
        {
            if (held[i].on)
            {
                const auto& h = held[i];
                lawA (h.fpan, h.fvol, cl, cr);
                const auto fp = 100.0 * i - 6000.0 + h.pitchCents;
                cp = (float) ((double) (float) fp * 0.01 - (double) i + 60.0);
                if (first)
                {
                    for (int j = 0; j < i; ++j)
                        LR[j][0] = cl, LR[j][1] = cr, ptab[j] = cp;
                    first = false;
                }
            }
            LR[i][0] = cl, LR[i][1] = cr, ptab[i] = cp;
        }
    }

    bool idle() const noexcept
    {
        if (eng.lastActive || eng.anyActive())
            return false;
        return std::none_of (std::begin (held), std::end (held), [] (const Held& h) { return h.on; });
    }

    // one Gen_Render of n samples; songBeats ≥ 0 while the transport plays (song position at the first sample)
    void render (float* outL, float* outR, int n, double songBeats) noexcept
    {
        for (auto& h : held) // a slide note moves Final.Pitch (issue #14)
            if (h.on && h.slide.moving())
            {
                const auto was = h.slide.cents;
                h.pitchCents += (double) (h.slide.advance ((float) n) - was);
            }
        tables();
        auto tpos = songBeats;
        eng.playing = songBeats >= 0.0;
        for (int c0 = 0; c0 < n; c0 += maxChunk)
        {
            const auto cn = std::min (maxChunk, n - c0);
            if (eng.playing)
                eng.songpos = tpos;
            eng.process (cn, LR, ptab, chunkL, chunkR);
            if (eng.playing)
                tpos = tpos + (double) eng.tempo * 0.016666666666666666 * (double) cn * (1.0 / eng.fs);
            for (int i = 0; i < cn; ++i)
                outL[c0 + i] += chunkL[i], outR[c0 + i] += chunkR[i];
        }
        // kill host voices whose engine voices ended and whose key is no longer held
        for (int key = 0; key < 128; ++key)
            if (held[key].on && eng.velTab[key] == 0.0f
                && std::none_of (eng.voices.begin(), eng.voices.end(), [key] (const Voice& v) { return v.active && v.note == key; }))
                held[key].on = false;
    }
};

// ------------------------------------------------------------------------------------------------------- LYNX front-end
//
// Levels (§1.11, REA_rodada28 §4.1). FL hands GMS Final.Vol = chan.Vol (the wrapper switches "link velocity" off),
// Final.Pan = note pan + chan.Pan and Final.Pitch = 100·(key − 60) + channel pitch; the wrapper turns them into the
// per-note tables T_L = Vol·cos((1 + pan)π/4), T_R = Vol·sin(…) and T = Final.Pitch/100 − key + 60. LYNX's channel
// gains are gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…), so V and p are recovered exactly and the channel volume/pan
// are not applied a second time. Velocity: Init.Vol = (11^(1.28·vel) − 1)/10 from LYNX's velocity (FL velocity/128,
// round 28 inference), then the wrapper's MIDI velocity. The tempo is the song tempo; the max-polyphony event is
// taken as value 0 (99 voices, FL's value is not confirmed). While the transport plays, the song position (beats)
// locks the synced LFOs and the effect LFOs (§1.8, §1.13).
// Random draws: one LCG per instance, started like a freshly created plugin; FL's GMS shares one process-global
// generator between all its instances, so noise, dither and the random phases of fresh voices differ from any given
// FL render by design.

struct Setup
{
    std::array<float, paramCount> P {};
    std::shared_ptr<const Tables> tabs[3];
};

class GmsInstrument final : public Instrument
{
public:
    GmsInstrument()
    {
        setup = new Setup();
        for (auto& t : setup->tabs)
            t = zeroTables();
        plugin.init (44100.0, true);
        applySetup();
    }
    ~GmsInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete setup;
    }

    void prepare (double sr) noexcept override
    {
        take();
        rate = sr;
        plugin.init (sr, true);
        applySetup();
        lastTicks = -1.0;
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        tempo.store (bpm > 0.0 ? (float) bpm : 140.0f);
        if (channel.pluginState == state && channel.samplePath == wavesDir)
            return;
        state = channel.pluginState;
        wavesDir = channel.samplePath;
        auto s = std::make_unique<Setup>();
        if (auto p = parseState (state))
        {
            s->P = p->P;
            for (int k = 0; k < 3; ++k)
                s->tabs[k] = waveTables (wavesDir, p->waves[(std::size_t) k]);
        }
        else
            for (auto& t : s->tabs)
                t = zeroTables();
        delete incoming.exchange (s.release());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        plugin.slideTo (100.0 * std::clamp (key, 0, 127) - 6000.0 + (double) pitchCents.load() + (double) cents, samples);
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto k = std::clamp (key, 0, 127);
        notePan[k] = nextPan;
        plugin.noteOn (k, initVol, channelVol, std::clamp ((double) nextPan + channelPan, -1.0, 1.0), (double) pitchCents.load() + (double) nextCents);
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        plugin.noteOff (key);
    }

    void allNotesOff() noexcept override
    {
        for (int k = 0; k < 128; ++k)
            if (plugin.held[k].on)
                plugin.noteOff (k);
    }

    bool idle() const noexcept override { return plugin.idle(); }

    void setSongPosition (double ticks) noexcept override
    {
        // the transport plays when the position moves between blocks
        playing = lastTicks >= 0.0 && ticks != lastTicks;
        lastTicks = ticks;
        songBeats = ticks / (double) ppq;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        plugin.eng.tempo = tempo.load();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int k = 0; k < 128; ++k)
                plugin.setLevels (k, vol, std::clamp ((double) notePan[k] + pan, -1.0, 1.0));
        }
        if (n <= 0)
            return;
        plugin.render (left, right, n, playing ? songBeats : -1.0);
    }

private:
    void applySetup() noexcept
    {
        plugin.eng.P = setup->P;
        for (int k = 0; k < 3; ++k)
            plugin.eng.tabs[k] = setup->tabs[k];
    }
    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            retired.store (setup);
            setup = s;
            applySetup();
        }
    }

    Plugin plugin;
    Setup* setup = nullptr;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::atomic<float> pitchCents { 0.0f }, tempo { 140.0f };
    std::vector<std::uint8_t> state;
    std::string wavesDir;
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0, lastTicks = -1.0, songBeats = 0.0;
    bool playing = false;
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

bool gmsStateUsable (const std::vector<std::uint8_t>& state) { return parseState (state).has_value(); }

std::vector<std::string> gmsWaveNames (const std::vector<std::uint8_t>& state)
{
    const auto p = parseState (state);
    if (! p)
        return {};
    return { p->waves[0], p->waves[1], p->waves[2] };
}

int gmsParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    int at = -1;
    if (index < 0 || index >= paramCount || ! parseState (state, &at))
        return -1;
    return at + 4 * index;
}

std::unique_ptr<Instrument> makeGmsInstrument() { return std::make_unique<GmsInstrument>(); }

namespace gms
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, const std::string& wavesDir, int block, double bpm, int maxPoly, double songPos, bool effects)
{
    const auto p = parseState (state);
    if (! p || length <= 0)
        return {};
    auto plugin = std::make_unique<Plugin>();
    plugin->init (sampleRate, effects);
    plugin->eng.P = p->P;
    for (int k = 0; k < 3; ++k)
        plugin->eng.tabs[k] = waveTables (wavesDir, p->waves[(std::size_t) k]);
    plugin->eng.tempo = (float) bpm;
    plugin->eng.npoly2 = maxPoly == 0 ? voiceCount : std::clamp (maxPoly, 1, voiceCount); // ProcessEvent 1 (FUN_1800ed930)
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    std::sort (evs.begin(), evs.end());
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) std::max (block, 1)), r (l.size());
    block = std::max (1, block);
    int n0 = 0;
    while (n0 < length)
    {
        // events at this sample in the model's order: (on, off) per note in list order, stable-sorted by sample
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            if (notes[i].start == n0)
                plugin->noteOn (notes[i].key, notes[i].initVol, notes[i].finalVol, notes[i].finalPan, notes[i].pitchCents);
            if (notes[i].release == n0)
                plugin->noteOff (std::clamp (notes[i].key, 0, 127));
        }
        auto ln = std::min (block, length - n0);
        for (auto e : evs)
            if (n0 < e && e < n0 + ln)
                ln = e - n0;
        std::fill (l.begin(), l.begin() + ln, 0.0f);
        std::fill (r.begin(), r.begin() + ln, 0.0f);
        plugin->render (l.data(), r.data(), ln, songPos >= 0.0 ? songPos + (double) n0 * bpm / (60.0 * sampleRate) : -1.0);
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace gms
} // namespace lynx
