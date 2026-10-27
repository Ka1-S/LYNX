#include "core/Harmor.h"
#include "core/HarmlessFx.h"
#include "core/Song.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

// The engine is a 1:1 port of our round-34 reference model (r34/py/hmodel.py + hcurve.py, henv.py, hart.py, tabs.py,
// hphaser.py, hrand.py), which reproduces the DLL as a sequence of float32 and double operations. The model is
// numpy code: float32 arrays combined with Python floats stay float32 (NEP 50), Python floats are doubles that are
// rounded to float32 with f32() at chosen places. This file keeps those types — `float` where the model holds
// float32, `double` + f32() where it holds Python floats — so fusing a·b + c into one FMA would change the rounding:
// contraction stays off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int bankSize = 520;          // positions: 0 self-oscillation, 1 Sub 1, 2 Sub 3, 3 Sub 4, 4…519 harmonics 1…516
constexpr double pi = 3.141592653589793;
const double ln2 = std::log (2.0);
const double ln001 = std::log (0.01);
const double log2Of440 = std::log2 (440.0);
const double x0Log = std::log2 (440.0 * std::pow (2.0, -57.0 / 12.0)); // log2 16.3516 Hz
constexpr double log2Of13081 = 7.031359713524659;
constexpr double zth = 5.960464477539063e-08;
constexpr std::uint32_t u32mask = 0xffffffffu;

inline double f32 (double x) noexcept { return (double) (float) x; }
inline double sgn (double x) noexcept { return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0; }
const double milli = f32 (0.001);    // f32(0.001) of the model (a Python float)

// Python's round() / numpy's rint: round half to even, inline: adding and removing 2^52 (2^23 for floats) rounds in
// the default round-to-nearest-even mode; larger values are already integers (std::nearbyint is a libm call on
// baseline x86-64, and the table reads use it per partial)
inline double rnd (double x) noexcept
{
    if (! (std::fabs (x) < 4503599627370496.0))
        return x;
    const auto big = std::copysign (4503599627370496.0, x);
    return (x + big) - big;
}
inline float rndf (float x) noexcept
{
    if (! (std::fabs (x) < 8388608.0f))
        return x;
    const auto big = std::copysign (8388608.0f, x);
    return (x + big) - big;
}
inline long long rndi (double x) noexcept { return (long long) rnd (x); }

// float → int64 as numpy's astype on x86 (non-finite or out-of-range values become INT64_MIN)
inline long long toInt64 (double x) noexcept
{
    if (! std::isfinite (x) || std::fabs (x) >= 9.2233720368547758e18)
        return std::numeric_limits<long long>::min();
    return (long long) x;
}

// numpy's pairwise summation (np.sum of float32 / float64 arrays)
template <typename T>
T pairwiseSum (const T* a, std::size_t n) noexcept
{
    if (n < 8)
    {
        T r = (T) 0;
        for (std::size_t i = 0; i < n; ++i)
            r += a[i];
        return r;
    }
    if (n <= 128)
    {
        T r[8];
        for (int j = 0; j < 8; ++j)
            r[j] = a[j];
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; ++j)
                r[j] += a[i + (std::size_t) j];
        T res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i)
            res += a[i];
        return res;
    }
    auto n2 = n / 2;
    n2 -= n2 % 8;
    return pairwiseSum (a, n2) + pairwiseSum (a + n2, n - n2);
}

// Per-partial transcendental functions of the control block (thousands per block): double exp/log within ~2 ulp,
// whose results the model rounds to float32 anyway (identical except at the rare rounding ties), and k^m by squaring.
inline double fastExp (double x) noexcept
{
    if (! (x > -700.0 && x < 700.0))
        return std::exp (x);
    const double ln2hi = 6.93147180369123816490e-01, ln2lo = 1.90821492927058770002e-10;
    const auto n = rnd (x * 1.4426950408889634);
    const auto r = (x - n * ln2hi) - n * ln2lo;
    // Taylor series of e^r on |r| ≤ 0.35 (terms to r^13)
    auto p = 1.0 / 6227020800.0;
    for (double c : { 1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0, 1.0 / 40320.0, 1.0 / 5040.0,
                      1.0 / 720.0, 1.0 / 120.0, 1.0 / 24.0, 1.0 / 6.0, 0.5, 1.0, 1.0 })
        p = p * r + c;
    const auto bits = (std::uint64_t) ((long long) n + 1023) << 52;
    double scale;
    std::memcpy (&scale, &bits, 8);
    return p * scale;
}

inline double fastLog (double x) noexcept
{
    if (! (x >= 2.2250738585072014e-308 && x < 1e300))
        return std::log (x);
    std::uint64_t bits;
    std::memcpy (&bits, &x, 8);
    auto e = (int) ((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m;
    std::memcpy (&m, &bits, 8);
    if (m > 1.4142135623730951)
        m *= 0.5, ++e;
    // log m = 2·atanh(f), f = (m − 1)/(m + 1), |f| ≤ 0.172
    const auto f = (m - 1.0) / (m + 1.0), s = f * f;
    auto p = 1.0 / 23.0;
    for (double c : { 1.0 / 21.0, 1.0 / 19.0, 1.0 / 17.0, 1.0 / 15.0, 1.0 / 13.0, 1.0 / 11.0, 1.0 / 9.0, 1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0 })
        p = p * s + c;
    const double ln2hi = 6.93147180369123816490e-01, ln2lo = 1.90821492927058770002e-10;
    return e * ln2hi + (2.0 * f + (2.0 * f * s * p + e * ln2lo));
}

inline double powInt (double k, int m) noexcept
{
    double r = 1.0;
    while (m > 0)
    {
        if (m & 1)
            r *= k;
        k *= k;
        m >>= 1;
    }
    return r;
}

// (3^x − 1)/2, exactly 1 at x = 1 (float32 as the DLL)
double g3 (double x) noexcept
{
    x = f32 (x);
    return x == 1.0 ? 1.0 : f32 (f32 (std::exp (x * f32 (std::log (3.0))) - 1.0) * 0.5);
}

// ---------------------------------------------------------------------------------------------------------------
// The 0xD5 state (REA_rodada11 §4.1): two parts (386 integers, 12-integer module order, 0x4B-byte combo block, 43
// articulators, 26 shaping curves, optional image tail) and the global block (53 integers, 6 integers, 0x3D bytes)

struct Point
{
    double dx = 0.0, y = 0.0, tension = 0.0; // float32 values
    int mode = 0, arp = 0; // point mode byte, arpeggio byte (point +0xD)
};

struct CurveData
{
    int kind = 0, ver = 0;
    std::vector<Point> pts;
    std::array<std::int32_t, 5> hdr {}; // flags, enabled, decay point, loop point, sustain point
    std::array<std::int32_t, 4> adsr { 128, 128, 0, 128 };
    std::array<std::int32_t, 4> lfo {};
    std::int32_t lfo2 = 0;
};

struct ArtData
{
    std::array<std::int32_t, 3> a {};
    std::array<CurveData, 11> parts; // envelope, LFO, 9 maps
};

struct PartData
{
    std::array<std::int32_t, 386> p {};
    std::array<std::int32_t, 12> order {};
    std::array<std::uint8_t, 0x4b> misc {};
    std::vector<ArtData> arts;     // 43
    std::vector<CurveData> shp;    // 26
    std::size_t paramOffset = 0, tail = 0;
    std::vector<std::uint8_t> tailBytes; // what follows the 26 shaping curves (image object + file name)
    bool present = false;

    std::int32_t I (int o) const noexcept { std::int32_t v; std::memcpy (&v, misc.data() + o, 4); return v; }
    float Fl (int o) const noexcept { float v; std::memcpy (&v, misc.data() + o, 4); return v; }
};

struct StateData
{
    int version = 0;
    std::array<PartData, 2> parts;
    bool hasGlobal = false;
    std::array<std::int32_t, 53> g {};
    std::array<std::int32_t, 6> order { 0, 1, 2, 3, 4, 5 }; // effects-rack unit order
    std::array<std::uint8_t, 0x3d> gmisc {};
    std::size_t globalOffset = 0;

    std::int32_t GI (int o) const noexcept { std::int32_t v; std::memcpy (&v, gmisc.data() + o, 4); return v; }
};

struct Reader
{
    const std::uint8_t* b;
    std::size_t n, i = 0;
    bool ok = true;

    bool need (std::size_t k) noexcept
    {
        if (! ok || i + k > n)
            ok = false;
        return ok;
    }
    std::int32_t i32() noexcept
    {
        if (! need (4))
            return 0;
        std::int32_t v;
        std::memcpy (&v, b + i, 4);
        i += 4;
        return v;
    }
    float f32v() noexcept
    {
        if (! need (4))
            return 0.0f;
        float v;
        std::memcpy (&v, b + i, 4);
        i += 4;
        return v;
    }
    double f64v() noexcept
    {
        if (! need (8))
            return 0.0;
        double v;
        std::memcpy (&v, b + i, 8);
        i += 8;
        return v;
    }
};

bool readCurve (Reader& r, CurveData& c)
{
    c.kind = r.i32();
    c.ver = r.i32();
    const auto n = r.i32();
    if (! r.ok || n < 0 || n >= 5000 || c.ver < 0 || c.ver > 3)
        return false;
    c.pts.resize ((std::size_t) n);
    for (auto& p : c.pts)
    {
        double x = 0.0, y = 0.0;
        std::uint8_t rest[8] {};
        int restLen = 0;
        if (c.ver < 2)
        {
            x = c.kind == 6 ? r.f64v() : (double) r.f32v();
            y = (double) r.f32v();
            restLen = c.ver < 1 ? 4 : 8;
        }
        else
        {
            x = r.f64v();
            y = r.f64v();
            restLen = 8;
        }
        if (! r.need ((std::size_t) restLen))
            return false;
        std::memcpy (rest, r.b + r.i, (std::size_t) restLen);
        r.i += (std::size_t) restLen;
        float t = 0.0f;
        std::memcpy (&t, rest, 4);
        p.dx = f32 (x);
        p.y = f32 (y);
        p.tension = (double) t;
        p.mode = restLen > 4 ? rest[4] : 0;
        p.arp = restLen > 5 ? rest[5] : 0;
    }
    for (auto& h : c.hdr)
        h = r.i32();
    return r.ok;
}

bool readArt (Reader& r, ArtData& a)
{
    r.i32();
    for (auto& v : a.a)
        v = r.i32();
    if (! readCurve (r, a.parts[0]))
        return false;
    for (auto& v : a.parts[0].adsr)
        v = r.i32();
    if (! readCurve (r, a.parts[1]))
        return false;
    for (auto& v : a.parts[1].lfo)
        v = r.i32();
    a.parts[1].lfo2 = a.parts[1].ver >= 3 ? r.i32() : 0;
    for (int k = 2; k < 11; ++k)
        if (! readCurve (r, a.parts[(std::size_t) k]))
            return false;
    return r.ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Old states (version 11…15, the factory presets of Harmor's first releases, Default.fst included) as the DLL's loader
// brings them into the current layout (REA_rodada165g-flbeta165s §1, [DECOMP FUN_0086ed30] + [EXEC] DLL re-saves): a part
// stores 385 integers — the articulator-33 smoothing (index 376) is missing, so a 0 is inserted there; version 11 also
// stores the 43 smoothing times (343…385) on an older scale, converted as round(log2(1 + (41^(v/1000) − 1)·37.5·0.0511)·
// 1000/9); articulator 33 is not stored and gets its defaults. The global block's text becomes an empty string plus
// the option byte 1 (what the DLL writes for these states). Bytes = the DLL's own re-save of the 88 old factory
// presets, except shaping-curve point positions the DLL renormalises at load (sub-ulp of the curve length).

std::optional<std::vector<std::uint8_t>> upgradeOldState (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 8)
        return std::nullopt;
    std::uint32_t v = 0, mask = 0;
    std::memcpy (&v, d.data(), 4);
    std::memcpy (&mask, d.data() + 4, 4);
    if (v > 0xffff)
        v >>= 16;
    if (v < 11 || v >= 16 || (mask & 3u) == 0 || (mask & 4u) == 0)
        return std::nullopt;
    std::vector<std::uint8_t> out;
    auto put = [&out] (std::int32_t x) {
        std::uint8_t b[4];
        std::memcpy (b, &x, 4);
        out.insert (out.end(), b, b + 4);
    };
    auto putD = [&out] (double x) {
        std::uint8_t b[8];
        std::memcpy (b, &x, 8);
        out.insert (out.end(), b, b + 8);
    };
    put (0x130003);
    put ((std::int32_t) mask);
    std::size_t o = 8;
    for (int part = 0; part < 2; ++part)
    {
        if (((mask >> part) & 1u) == 0)
            continue;
        if (o + 4 > d.size())
            return std::nullopt;
        std::uint32_t sz = 0;
        std::memcpy (&sz, d.data() + o, 4);
        const auto end = o + 4 + (std::size_t) sz;
        if (end > d.size() || sz < 4 * 385 + 0x30 + 0x4b)
            return std::nullopt;
        std::vector<std::int32_t> p (385);
        std::memcpy (p.data(), d.data() + o + 4, 4 * 385);
        p.insert (p.begin() + 376, 0);
        if (v < 12)
            for (std::size_t i = 343; i < 386; ++i)
            {
                const auto x = (double) (float) ((float) p[i] * 0.001f);
                const auto t = std::exp (x * std::log (41.0));
                const auto y = std::log ((double) (float) (t - 1.0) * 37.5 * 0.0511 + 1.0);
                p[i] = (std::int32_t) std::nearbyint (y * (1.0 / std::log (2.0)) * 111.11111111111111);
            }
        const auto sizeAt = out.size();
        put (0);
        for (const auto x : p)
            put (x);
        Reader r { d.data(), end, o + 4 + 4 * 385 };
        r.i += 0x30 + 0x4b;
        out.insert (out.end(), d.begin() + (std::ptrdiff_t) (o + 4 + 4 * 385), d.begin() + (std::ptrdiff_t) r.i);
        for (int k = 0; k < 43; ++k)
        {
            if (k == 33)
            {
                // the defaults of an articulator: off, amounts 1, envelope at 0.5, LFO at 0.75, flat maps at 0.5
                auto curve = [&] (int kind, std::initializer_list<std::pair<double, double>> pts, std::array<std::int32_t, 5> hdr) {
                    put (kind);
                    put (3);
                    put ((std::int32_t) pts.size());
                    for (const auto& [x, y] : pts)
                    {
                        putD (x);
                        putD (y);
                        put (0);
                        put (0);
                    }
                    for (const auto h : hdr)
                        put (h);
                };
                put (3);
                put (0);
                put (0x3f800000);
                put (0x3f800000);
                curve (1, { { 0.0, 0.5 } }, { 0, 0, -1, -1, -1 });
                for (const auto a : { 128, 128, 0, 128 })
                    put (a);
                curve (2, { { 0.0, 0.75 } }, { 0, 0, -1, -1, -1 });
                for (const auto a : { 40898, 0, 0, 0, 0 })
                    put (a);
                for (int m = 0; m < 9; ++m)
                    curve (3, { { 0.0, 0.5 }, { 1.0, 0.5 } }, { 0, -1, -1, -1, -1 });
                continue;
            }
            const auto from = r.i;
            ArtData a;
            if (! readArt (r, a))
                return std::nullopt;
            out.insert (out.end(), d.begin() + (std::ptrdiff_t) from, d.begin() + (std::ptrdiff_t) r.i);
        }
        out.insert (out.end(), d.begin() + (std::ptrdiff_t) r.i, d.begin() + (std::ptrdiff_t) end);
        const auto blockSize = (std::int32_t) (out.size() - sizeAt - 4);
        std::memcpy (out.data() + sizeAt, &blockSize, 4);
        o = end;
    }
    if (o + 0xd4 + 0x18 + 0x3d > d.size())
        return std::nullopt;
    out.insert (out.end(), d.begin() + (std::ptrdiff_t) o, d.begin() + (std::ptrdiff_t) (o + 0xd4 + 0x18 + 0x3d));
    out.push_back (0); // the preset text (empty)
    out.push_back (1); // option byte
    return out;
}

std::optional<StateData> parseCurrent (const std::vector<std::uint8_t>& d);

std::optional<StateData> parseState (const std::vector<std::uint8_t>& d)
{
    if (auto up = upgradeOldState (d))
        return parseCurrent (*up);
    return parseCurrent (d);
}

std::optional<StateData> parseCurrent (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 8)
        return std::nullopt;
    StateData S;
    std::uint32_t v = 0, mask = 0;
    std::memcpy (&v, d.data(), 4);
    std::memcpy (&mask, d.data() + 4, 4);
    if (v > 0xffff)
        v >>= 16;
    S.version = (int) v;
    if (S.version < 16 || (mask & 3u) == 0)
        return std::nullopt; // the articulators and curves are stored from version 16
    std::size_t o = 8;
    for (int part = 0; part < 2; ++part)
    {
        if (((mask >> part) & 1u) == 0)
            continue;
        if (o + 4 > d.size())
            return std::nullopt;
        std::uint32_t sz = 0;
        std::memcpy (&sz, d.data() + o, 4);
        const auto end = o + 4 + (std::size_t) sz;
        if (end > d.size() || sz < 4 * 386 + 0x30 + 0x4b)
            return std::nullopt;
        auto& P = S.parts[(std::size_t) part];
        P.paramOffset = o + 4;
        std::memcpy (P.p.data(), d.data() + o + 4, 4 * 386);
        auto q = o + 4 + 4 * 386;
        std::memcpy (P.order.data(), d.data() + q, 0x30);
        q += 0x30;
        std::memcpy (P.misc.data(), d.data() + q, 0x4b);
        q += 0x4b;
        Reader r { d.data(), end, q };
        P.arts.resize (43);
        for (auto& a : P.arts)
            if (! readArt (r, a))
                return std::nullopt;
        P.shp.resize (26);
        for (auto& c : P.shp)
            if (! readCurve (r, c))
                return std::nullopt;
        P.tail = end - r.i;
        P.tailBytes.assign (d.begin() + (std::ptrdiff_t) r.i, d.begin() + (std::ptrdiff_t) end);
        P.present = true;
        o = end;
    }
    if ((mask & 4u) != 0 && o + 0xd4 + 0x18 + 0x3d <= d.size())
    {
        S.hasGlobal = true;
        S.globalOffset = o;
        std::memcpy (S.g.data(), d.data() + o, 0xd4);
        std::memcpy (S.order.data(), d.data() + o + 0xd4, 0x18);
        std::memcpy (S.gmisc.data(), d.data() + o + 0xd4 + 0x18, 0x3d);
    }
    if (! S.hasGlobal)
        return std::nullopt;
    return S;
}

// ---------------------------------------------------------------------------------------------------------------
// The generic point curve (hcurve.py): shaping curves, envelopes, LFO amounts and maps. Points hold float32 dx, y,
// tension; segment values are computed in double and stored as float32.

struct Curve
{
    std::vector<double> dx, y, T;
    std::vector<int> mode, sign;
    std::vector<std::array<double, 3>> k;
    double c = 0.0, length = 0.0;

    struct St
    {
        int idx = 0;
        double rate = 0.0, t = 0.0, ys = 0.0, dl = 0.0, v = 0.0;
    };

    Curve() = default;
    Curve (const std::vector<Point>& pts, double centre = 0.0) { build (pts, centre); }

    void build (const std::vector<Point>& ptsIn, double centre)
    {
        auto pts = ptsIn;
        if (pts.empty())
            pts.push_back ({}); // (the model needs at least one point; FL's curves always have one)
        const auto n = pts.size();
        dx.resize (n), y.resize (n), T.resize (n), mode.resize (n), sign.assign (n, 0), k.assign (n, { 0.0, 0.0, 0.0 });
        length = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            dx[i] = f32 (pts[i].dx), y[i] = f32 (pts[i].y), T[i] = f32 (pts[i].tension), mode[i] = pts[i].mode;
            length += dx[i];
        }
        c = centre;
        const auto LN1M = std::log (1000001.0);
        for (std::size_t i = 1; i < n; ++i)
        {
            const auto t = T[i];
            const auto m = mode[i];
            int s = t == 0.0 ? 0 : (t > 0.0 ? 1 : -1);
            long long N = 0;
            if (m >= 3 && m <= 6)
            {
                const auto v = f32 (1.0 - std::fabs (t) * 0.6);
                N = rndi (f32 (std::exp (v * LN1M) - 1.0) * 0.001);
                if (m == 3 && t >= 0.0)
                    N += 1;
            }
            double k10 = 0.0, k14 = 0.0, k18 = 0.0;
            if (m == 6)
            {
                k10 = f32 ((double) N + 0.5);
                k14 = f32 (2.0 * pi);
                if (N == 0 && s > 0)
                    s = 2;
            }
            else if (m < 2)
            {
                if (t != 0.0)
                {
                    auto q = f32 (std::exp (f32 (std::fabs (t)) * LN1M) - 1.0);
                    q = t > 0.0 ? q : -q;
                    k10 = q;
                    k14 = f32 (1.0 / std::fabs (q));
                    k18 = f32 (std::log (std::fabs (q) + 1.0));
                }
            }
            else if (m == 3)
            {
                const double sg = t < 0.0 ? -1.0 : 1.0;
                k18 = f32 (sg * 0.5);
                k10 = f32 ((double) (N + 1));
                k14 = f32 (1.0 / f32 (k10 - sg));
            }
            else if (m == 4)
            {
                k10 = f32 ((double) (N + 1));
                k14 = f32 (1.0 / k10);
            }
            else if (m == 5)
            {
                const double sg = t < 0.0 ? -1.0 : 1.0;
                k18 = f32 (sg * 0.5);
                k10 = f32 ((double) ((N + 1) * 2));
                k14 = f32 (2.0 * pi);
            }
            else if (m == 7 || m == 8)
                k10 = t;
            else if (m == 9)
                k10 = f32 (std::fabs (t));
            else if (m == 10)
            {
                // monotone cubic through the neighbouring points (neighbours without loop wrapping)
                const double TINY = 5.960464477539063e-08;
                const auto X6 = std::max (dx[i], TINY);
                const auto yi = y[i];
                const auto j = (long long) i - 1;
                const auto y9 = j >= 0 ? y[(std::size_t) j] : yi;
                auto X8 = y9;
                double X7 = 1.0;
                if (j < 0 || mode[(std::size_t) j] != 10)
                    X7 = 1.0;
                else
                {
                    X7 = dx[(std::size_t) j] / X6;
                    if (j - 1 >= 0)
                        X8 = y[(std::size_t) j - 1];
                }
                const auto kk = i + 1;
                double in1 = yi, d19 = 1.0;
                if (! (kk > n - 1 || mode[kk] != 10))
                    d19 = dx[kk] / X6, in1 = y[kk];
                const auto d = yi - y9;
                if (std::fabs (d) >= TINY)
                {
                    X8 = (X8 - y9) / d;
                    const auto d24 = std::max (X7, TINY);
                    d19 = std::max (d19, TINY);
                    const auto d26 = ((in1 - y9) / d - 1.0) / d19;
                    const auto d21 = (-X8) / d24;
                    float a10 = 0.0f, a14 = 0.0f;
                    const auto fd26 = (float) d26, fd19 = (float) d19, fd21 = (float) d21, fd24 = (float) d24;
                    if (fd26 * 1.0f > 0.0f)
                    {
                        const float f25 = fd19 * 2.0f + 1.0f, f18 = fd19 + 2.0f;
                        a10 = ((fd26 * 1.0f) * (f25 + f18)) / ((f25 * fd26) + (f18 * 1.0f));
                    }
                    if (1.0f * fd21 > 0.0f)
                    {
                        const float f18 = 2.0f + fd24, f25 = 1.0f + fd24 * 2.0f;
                        a14 = ((1.0f * fd21) * (f18 + f25)) / ((f18 * 1.0f) + (f25 * fd21));
                    }
                    k10 = a10, k14 = a14, k18 = f32 (1.0 - std::fabs (t));
                }
                else
                    s = 2;
            }
            else if (m == 11 || m == 12)
            {
                const auto d = std::max (std::fabs (t) * 0.9, 1.52587890625e-05);
                const auto u = 1.0 - d;
                k10 = f32 (1.0 - u * u * u);
                k14 = f32 ((1.0 - k10) / k10);
                k18 = k14;
                if (s > 0)
                    k14 = -k14, k18 = f32 (k14 - 1.0);
                if (m == 12)
                    k14 = f32 (k14 * 0.5), k18 = f32 (k18 * 0.5);
            }
            if (m < 16 && ((0x1b83 >> m) & 1) != 0 && t == 0.0)
                s = 2;
            sign[i] = s;
            k[i] = { k10, k14, k18 };
        }
    }

    int size() const noexcept { return (int) dx.size(); }
    double val (int i) const noexcept { return y[(std::size_t) i] - c; }

    // segment shape (0…1) at position t of segment i (point modes 0–12; unknown modes are linear)
    double shape (int i, double t) const noexcept
    {
        const auto u = (std::size_t) i;
        const auto m = mode[u];
        const auto s = sign[u];
        const auto k10 = k[u][0], k14 = k[u][1], k18 = k[u][2];
        if (s == 2)
            return f32 (t);
        switch (m)
        {
            case 7:
            {
                const auto w = s > 0 ? 1.0 - t : t;
                const auto w3 = w * w * w;
                return f32 (t - (w3 * w3 - w) * k10);
            }
            case 0:
                if (k10 != 0.0)
                    return f32 (k10 > 0.0 ? 1.0 - (std::exp ((1.0 - t) * k18) - 1.0) * k14 : (std::exp (t * k18) - 1.0) * k14);
                return f32 (t);
            case 2: return 0.0; // hold
            case 3: return (double) (float) (double) rndi (t * k10 - k18) * k14;
            case 5: return (double) (rndi (t * k10 - k18) & 1);
            case 6:
            {
                if (s < 0)
                    return f32 ((std::sin ((t * k10 + 0.75) * k14) + 1.0) * 0.5);
                const auto v = t * k10 - 0.5;
                const auto r = rnd (v - 0.5);
                return f32 (std::fabs ((v - r) * 2.0 - 1.0));
            }
            case 9:
            {
                const auto g = s < 1 ? (1.0 - std::cos (t * 1.5707963705062866)) : std::sin (t * 1.5707963705062866);
                return f32 (t - (t - g) * k10);
            }
            case 4:
            {
                const auto v = t * k10 - 0.5;
                const auto r = rnd (v - 0.5);
                auto d = (v - r) * 2.0 - 1.0;
                if (s < 0)
                {
                    const auto a = 1.0 - std::fabs (d);
                    d = (1.0 - a * a * a) * (d >= 0.0 ? 1.0 : -1.0);
                }
                else
                    d = d * d * d;
                return f32 ((r + d * 0.5 + 1.0) * k14);
            }
            case 8:
            {
                if (t < 0.5)
                {
                    const auto x = t * 2.0;
                    const auto w = s > 0 ? 1.0 - x : x;
                    const auto w3 = w * w * w;
                    return f32 ((x - (w3 * w3 - w) * k10) * 0.5);
                }
                const auto x = (1.0 - t) * 2.0;
                const auto w = s > 0 ? 1.0 - x : x;
                const auto w3 = w * w * w;
                return f32 (1.0 - (x - (w3 * w3 - w) * k10) * 0.5);
            }
            case 1:
            {
                auto f = [&] (double x) {
                    if (k10 == 0.0)
                        return x;
                    return k10 > 0.0 ? 1.0 - (std::exp ((1.0 - x) * k18) - 1.0) * k14 : (std::exp (x * k18) - 1.0) * k14;
                };
                if (t < 0.5)
                    return f32 (f (t * 2.0) * 0.5);
                return f32 (1.0 - f ((1.0 - t) * 2.0) * 0.5);
            }
            case 10:
            {
                const auto t2 = t * t, t1 = t - 1.0;
                return f32 (t + (((t2 * 3.0 - t2 * 2.0 * t) + t2 * t1 * k10 + t * t1 * t1 * k14) - t) * k18);
            }
            case 11:
                if (s < 1)
                    return f32 (k14 / (1.0 - k10 * t) - k18);
                return f32 (k14 / (1.0 - k10 * (1.0 - t)) - k18);
            case 12:
                if (t < 0.5)
                {
                    if (s < 1)
                        return f32 (k14 / (1.0 - k10 * t * 2.0) - k18);
                    return f32 (k14 / (1.0 - k10 * (1.0 - t * 2.0)) - k18);
                }
                else
                {
                    const auto d = (1.0 - t) * 2.0;
                    if (s < 1)
                        return f32 ((1.0 - k14 / (1.0 - k10 * d)) + k18);
                    return f32 ((1.0 - k14 / (1.0 - k10 * (1.0 - d))) + k18);
                }
            default: return f32 (t); // (modes 13+ are not decoded: linear)
        }
    }

    St start() const noexcept
    {
        St st;
        if (dx[0] == 0.0)
            st.rate = 1.0, st.t = 1.0;
        else
            st.rate = 1.0 / dx[0], st.t = 0.0;
        st.ys = f32 (val (0));
        st.dl = 0.0;
        st.v = st.ys;
        return st;
    }

    double step (St& st, double step) const noexcept
    {
        const auto n = size();
        if (! (0 <= st.idx && st.idx <= n - 1))
            return st.v;
        auto t = st.t + step * st.rate;
        auto rate = st.rate;
        auto idx = st.idx;
        while (t >= 1.0)
        {
            const auto old = idx;
            t = rate == 0.0 ? step : (t - 1.0) / rate;
            st.ys = f32 (val (idx));
            if (idx < n - 1)
                ++idx;
            st.dl = f32 (val (idx) - st.ys);
            if (old == idx)
                t = 0.0, rate = 0.0;
            else
            {
                const auto d = dx[(std::size_t) idx];
                if (d <= 0.0)
                    rate = 1.0, t = t + 1.0;
                else
                    rate = 1.0 / d, t = t * rate;
            }
        }
        st.t = t, st.rate = rate, st.idx = idx;
        st.v = f32 (st.ys + f32 (shape (idx, t)) * st.dl);
        return st.v;
    }

    double valueAt (double x) const noexcept
    {
        auto st = start();
        return step (st, x);
    }

    // values after stepping to the absolute positions xs (xs[0] may be 0 = start value)
    void renderAt (const double* xs, int count, float* out) const noexcept
    {
        auto st = start();
        double prev = 0.0;
        for (int i = 0; i < count; ++i)
        {
            if (xs[i] == 0.0 && i == 0)
            {
                out[i] = (float) st.v;
                continue;
            }
            out[i] = (float) step (st, xs[i] - prev);
            prev = xs[i];
        }
    }

    // table render: N values, step = length/(N − 1) unless given (> 0)
    void render (int N, double stepIn, float* out) const noexcept
    {
        auto stp = stepIn;
        if (! (stp > 0.0))
            stp = length / (N - 1);
        auto st = start();
        for (int j = 0; j < N; ++j)
        {
            out[j] = (float) st.v;
            step (st, stp);
        }
    }

    std::vector<float> render (int N, double stepIn = -1.0) const
    {
        std::vector<float> out ((std::size_t) N);
        render (N, stepIn, out.data());
        return out;
    }

    bool constant() const noexcept
    {
        for (auto v : y)
            if ((float) v != (float) y[0])
                return false;
        return true;
    }
};

// log2(i + 1)·length/9 for i = 0…515: the per-partial sampling positions of the shaping curves (FUN_00888ed0)
std::vector<double> harmonicPositions (double length)
{
    std::vector<double> xs (516);
    const auto L = length / 9.0;
    for (int i = 0; i < 516; ++i)
        xs[(std::size_t) i] = std::log2 ((double) (i + 1)) * L;
    return xs;
}

// ---------------------------------------------------------------------------------------------------------------
// Time envelope of the articulators (henv.py)

struct EnvState
{
    int idx = 0, stage = 1, sub = 1;
    double mod = 0.0, rate = 0.0, t = 0.0, ys = 0.0, dl = 0.0, out = 0.0;
    // arpeggio (REA_rodada75): chord position played (+0x3c), this note's place (+0x40) and the chord size (+0x44),
    // level held while another note plays (+0x48), muted (+0x39)
    int aidx = -1, own = -1, cnt = -1;
    double hold = 0.0;
    bool muted = false;
    // per-state time scales (+0x50…+0x5c, from the knobs; the global envelope scales act on them), the sustain offset
    // used on entering the sustain (+0x34) and the loop stop (+0x38)
    std::array<double, 4> sc { 1.0, 1.0, 0.0, 1.0 };
    double susmod = 0.0;
    bool loopstop = false;
};

struct EnvDef
{
    Curve c;
    int flags = 0, enabled = 0, decay = -1, loop = -1, sus = -1, rel = -1;
    std::array<double, 4> scale { 1.0, 1.0, 0.0, 1.0 };
    double rate = 1.0;
    std::vector<double> w;
    std::vector<int> arp;           // point arpeggio markers (point byte +0xD: 1 below, 2 same, 3 above)
    bool arpOn = false, zeroLoop = false;

    void build (const CurveData& part, double centre, double r, bool knobs)
    {
        c.build (part.pts, centre);
        arp.clear();
        for (const auto& pt : part.pts)
            arp.push_back (pt.arp);
        flags = part.hdr[0], enabled = part.hdr[1], decay = part.hdr[2], loop = part.hdr[3], sus = part.hdr[4];
        const std::array<std::int32_t, 4> kn = knobs ? part.adsr : std::array<std::int32_t, 4> { 128, 128, 0, 128 };
        for (int i = 0; i < 4; ++i)
            scale[(std::size_t) i] = f32 (kn[(std::size_t) i] / 128.0);
        rate = r;
        const auto n = c.size();
        rel = sus >= 0 ? sus : (loop >= 0 ? n - 1 : -1);
        weights();
        // arpeggio markers act when one lies at or before the sustain point and the loop has a length (+0x8d, +0x8c)
        auto tm = [&] (int i) {
            double t = 0.0;
            for (int k = 0; k <= i && k < n; ++k)
                t += c.dx[(std::size_t) k];
            return t;
        };
        zeroLoop = rel >= 0 && loop >= 0 && tm (rel) - tm (loop) <= 0.0;
        arpOn = false;
        for (int i = 0; i < (int) arp.size(); ++i)
            arpOn = arpOn || (arp[(std::size_t) i] != 0 && i <= sus);
        arpOn = arpOn && ! zeroLoop;
    }

    // FUN_007a24b0: this note's place in the held chord and the chord size
    void setChord (EnvState& s, int own, int cnt) const noexcept
    {
        s.own = own, s.cnt = cnt;
        if (s.stage == 5 && cnt < 1)
            release (s);
    }

    void weights()
    {
        const auto& d = c.dx;
        const int n = c.size();
        const int i15 = std::max (decay, 0);
        const int l80 = sus >= 0 ? sus : n - 1;
        const int l7c = loop >= 0 ? loop : l80;
        double L98 = 0.0, L90 = 0.0, L88 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (i <= i15)
                L98 += d[(std::size_t) i];
            else if (i <= l7c)
                L90 += d[(std::size_t) i];
            if (i > l80)
                L88 += d[(std::size_t) i];
        }
        w.assign ((std::size_t) n, 0.0);
        double d21 = L98, d20 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto di = d[(std::size_t) i];
            if (i > i15)
            {
                w[(std::size_t) i] = 1.0;
                if (i > l80)
                {
                    d20 = d21 - di;
                    w[(std::size_t) i] = (flags & 0x80) == 0 ? f32 (d20 / std::max (L88, 1.52587890625e-05)) : 1.0;
                }
                else
                {
                    if (i <= l7c)
                        w[(std::size_t) i] = f32 ((d21 + di) / std::max (L90, 1.52587890625e-05));
                    d20 = d21 + di;
                    if (i == l80)
                        d20 = L88;
                }
            }
            else
            {
                d20 = 0.0;
                if (i != i15)
                {
                    d20 = d21 - di;
                    w[(std::size_t) i] = f32 (d20 / std::max (L98, 1.52587890625e-05));
                }
            }
            d21 = d20;
        }
    }

    // point value with the sustain-level offset (clamped to [0, 1]) minus the centre
    double val (int i, double mod) const noexcept
    {
        auto v = c.y[(std::size_t) i];
        if (mod != 0.0)
            v = std::min (std::max (v + f32 (w[(std::size_t) i] * mod), 0.0), 1.0);
        return v - c.c;
    }

    EnvState start() const noexcept
    {
        EnvState s;
        s.idx = 0, s.mod = 0.0;
        s.stage = decay < 0 ? 3 : 1;
        if (c.dx[0] == 0.0)
            s.rate = 1.0, s.t = 1.0;
        else
            s.rate = rate / c.dx[0], s.t = 0.0;
        s.sub = s.stage;
        s.ys = f32 (val (0, 0.0));
        s.dl = 0.0;
        s.out = s.ys;
        for (std::size_t i = 0; i < 4; ++i)
            s.sc[i] = f32 (scale[i]);
        s.susmod = sus >= 0 ? s.sc[2] : 0.0;
        s.loopstop = zeroLoop || s.sc[1] <= 0.0;
        return s;
    }

    // global envelope attack/decay scale, sustain offset, release scale (first block, FUN_007a2c80 / FUN_007a2d60)
    void globalScales (EnvState& s, const double* g) const noexcept
    {
        s.sc = { f32 (s.sc[0] * g[0]), f32 (s.sc[1] * g[1]), f32 (s.sc[2] + g[2]), f32 (s.sc[3] * g[3]) };
        s.susmod = sus >= 0 ? s.sc[2] : 0.0;
        s.loopstop = s.loopstop || s.sc[1] <= 0.0;
        if (s.stage >= 2)
            s.mod = s.susmod;
        s.out = f32 (val (0, s.mod));
    }

    double step (EnvState& s, double dt) const noexcept
    {
        const int n = c.size();
        if (! (0 <= s.idx && s.idx <= n - 1))
            return s.out;
        s.t = s.t + dt * s.rate;
        while (s.t >= 1.0)
        {
            s.t = s.rate == 0.0 ? dt : (s.t - 1.0) / s.rate;
            s.ys = f32 (val (s.idx, s.mod));
            if (arpOn && s.stage < 6 && arp[(std::size_t) s.idx] != 0)
            {
                if (s.stage == 5)
                {
                    s.aidx = -1;
                    release (s);
                }
                else if (s.cnt > 0)
                {
                    const int a = (std::max (s.aidx, 0) - 2 + arp[(std::size_t) s.idx] + s.cnt) % s.cnt;
                    s.aidx = a, s.muted = a != s.own, s.hold = s.ys;
                }
            }
            bool stop = false;
            if (s.idx < rel || rel < 0 || s.stage > 5)
            {
                if (decay >= 0 && s.idx >= decay && s.sub < 3)
                    s.stage = std::max (s.stage, 3), s.sub = 3, s.mod = s.susmod;
                if (s.idx < n - 1)
                    ++s.idx;
                else
                    s.stage = 9, stop = true;
            }
            else if (loop < 0)
                stop = true;
            else
            {
                s.sub = 4;
                s.idx = loop + 1;
                stop = s.loopstop;
                if (s.idx > n - 1)
                    s.idx = n - 1;
            }
            if (! s.muted)
                s.dl = f32 (val (s.idx, s.mod) - s.ys);
            else
                s.ys = s.hold, s.dl = 0.0;
            if (! stop)
            {
                const int kx = (s.stage > 2 ? 1 : 0) + ((s.stage >= 6 && sus >= 0) ? 2 : 0);
                const auto d = c.dx[(std::size_t) s.idx] * s.sc[(std::size_t) kx];
                if (d <= 0.0)
                    s.rate = 1.0, s.t = s.t + 1.0;
                else
                    s.rate = rate / d, s.t = s.t * s.rate;
            }
            else
                s.t = 0.0, s.rate = 0.0;
        }
        s.out = s.dl == 0.0 ? s.ys : f32 (s.ys + f32 (c.shape (s.idx, s.t)) * s.dl);
        return s.out;
    }

    void release (EnvState& s) const noexcept
    {
        if (s.stage < 6)
        {
            // the note playing an arpeggio carries on to the next marker (stage 5)
            bool wait = false;
            const int n = c.size();
            if (s.aidx >= 0 && ! s.muted && s.cnt >= 2 && s.idx >= 0 && s.idx <= n - 1)
            {
                int u = s.idx, end = s.idx;
                while (true)
                {
                    if (arp[(std::size_t) u] != 0)
                    {
                        wait = true;
                        break;
                    }
                    if (u == sus && sus >= 0 && loop >= 0)
                        u = loop + 1, end = std::max (end, u);
                    else if (++u > n - 1)
                        break;
                    if (u == end || u > n - 1)
                        break;
                }
            }
            if (wait)
                s.stage = 5;
            else
            {
                if (sus >= 0)
                {
                    s.idx = std::min (sus, c.size() - 1);
                    s.mod = f32 (s.mod + (s.out - val (s.idx, s.mod)));
                    s.t = 1.0;
                    s.muted = false;
                    s.sub = 6;
                }
                s.stage = 6;
            }
            if (s.rate == 0.0)
                s.t = 1.0;
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Parametric LFO waveform (hart.LFOGen)

struct LfoGen
{
    double T = 0.0, k3 = 0.0, k4 = 0.0, k0 = 0.0, k1 = 0.0;
    bool uni = false;
    std::uint32_t e5 = 0, e6 = 0, e7 = 0, start0 = 0, start = 0;

    void build (const std::array<std::int32_t, 4>& knobs, std::int32_t extra, int flags)
    {
        const auto ten = knobs[1], skew = knobs[2], pw = knobs[3];
        const auto x = f32 (std::abs (ten) * f32 (0.0078125));
        auto d5 = f32 (std::exp (x * std::log (21.0)) - 1.0) * 0.05;
        d5 = std::exp (f32 (d5) * std::log (999999984306749440.0));
        T = f32 (f32 (d5 - 1.0) * sgn ((double) ten));
        if (T != 0.0)
            k3 = f32 (1.0 / std::fabs (T)), k4 = f32 (std::log (std::fabs (T) + 1.0));
        const auto sk = f32 (skew * f32 (0.00390625));
        const auto p = f32 (pw * f32 (0.0078125));
        uni = (flags & 4) != 0;
        if (p > 0.0)
            e7 = (std::uint32_t) (rndi ((double) ((float) f32 (1.0 - p) * 4294967296.0f - 1.0f)) & u32mask);
        else
            e7 = u32mask;
        auto d2 = ((f32 (1.0 - p) / 2.0) - sk * (1.0 - std::fabs (p))) * 4294967296.0;
        if (d2 > (double) e7 - 1.0)
            d2 = (double) e7 - 1.0;
        else if (d2 < 1.0)
            d2 = 1.0;
        e5 = (std::uint32_t) (rndi (d2) & u32mask);
        if (p < 0.0)
        {
            e6 = (std::uint32_t) (rndi (d2 + (double) ((float) p * 4294967296.0f)) & u32mask);
            if (e6 == 0)
                e6 = 1;
        }
        else
            e6 = e5;
        k0 = f32 (2.0 / (double) e6);
        const auto span = (std::uint32_t) (e7 - e5);
        k1 = span != 0 ? f32 (2.0 / (double) span) : 0.0;
        start0 = (std::uint32_t) (e5 + (span >> 1) + 1u);
        start = (std::uint32_t) (start0 + (std::uint32_t) extra);
    }

    double value (std::uint32_t ph) const noexcept
    {
        double x = 0.0;
        if (ph > e7)
            return 1.0;
        if (ph < e6)
            x = f32 (1.0 - (double) ((float) ph * (float) k0));
        else if (ph < e5)
            return -1.0;
        else
            x = f32 ((double) ((float) (std::uint32_t) (ph - e5) * (float) k1) - 1.0);
        auto curve = [&] (double a) {
            if (T == 0.0)
                return a;
            if (T > 0.0)
                return 1.0 - (std::exp ((1.0 - a) * k4) - 1.0) * k3;
            return (std::exp (a * k4) - 1.0) * k3;
        };
        if (! uni)
        {
            const auto v = std::fabs (f32 (curve (std::fabs (x))));
            return (x < 0.0 || (x == 0.0 && std::signbit (x))) ? -v : v;
        }
        const auto u = f32 (f32 (x + 1.0) * 0.5);
        return f32 (curve (u) * 2.0 - 1.0);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Articulators (hart.py): combination modes from the DLL's bit sets, envelope, LFO and the 9 maps

enum MapInput { inKey = 2, inVel = 3, inX = 4, inY = 5, inZ = 6, inRandom = 7, inUnison = 8, in9 = 9, in10 = 10 };

struct Art
{
    int k = 0, mode = 0;
    double centre = 0.0, envDepth = 0.0, lfoDepth = 0.0, smooth = 0.0, base = 0.0;
    bool hasEnv = false, envGlobal = false, lfoOn = false, lfoGlobal = false;
    EnvDef env, lfoEnv;
    LfoGen gen;
    double lfoInc98 = 0.0, lfoInc = 0.0;
    bool hasMapRand = false, hasMapUni = false;
    Curve mapRand, mapUni;
    std::array<bool, 11> hasMap {};
    std::array<Curve, 11> maps;
    std::array<int, 12> order {}; // −1 env, −2 LFO, map indices
    int orderCount = 0;

    static int modeOf (int k)
    {
        static const int m1[] = { 0, 2, 4, 5, 12, 13, 14, 15, 17, 18, 23, 24, 27, 28, 29, 30, 33, 36, 38, 39, 40, 41, 42 };
        static const int m2[] = { 2, 14, 23, 28, 30, 38, 39, 41, 42 };
        if (std::find (std::begin (m2), std::end (m2), k) != std::end (m2))
            return 2;
        if (std::find (std::begin (m1), std::end (m1), k) != std::end (m1))
            return 1;
        return 0;
    }

    double comb (double acc, double v) const noexcept
    {
        if (mode == 0)
            return f32 (acc * v);
        if (mode == 1)
            return f32 (acc + v);
        return f32 (f32 (acc * v) * 2.0);
    }

    void build (int index, const ArtData& a, double envRate, double tempoRate)
    {
        k = index;
        mode = modeOf (k);
        centre = mode >= 1 ? 0.5 : 0.0;
        float ed = 0.0f, ld = 0.0f;
        std::memcpy (&ed, &a.a[1], 4);
        std::memcpy (&ld, &a.a[2], 4);
        envDepth = ed, lfoDepth = ld;
        auto en = [] (const CurveData& c) { return c.hdr[1] != 0; };
        const auto ef = a.parts[0].hdr[0];
        hasEnv = en (a.parts[0]);
        if (hasEnv)
            env.build (a.parts[0], centre, (ef & 1) != 0 ? tempoRate : envRate, true);
        envGlobal = hasEnv && (ef & 2) != 0;
        lfoOn = en (a.parts[1]);
        if (lfoOn)
        {
            const auto& lp = a.parts[1];
            const auto flags = lp.hdr[0];
            lfoGlobal = (flags & 2) != 0;
            const auto lrate = (flags & 1) == 0 ? envRate : tempoRate;
            lfoEnv.build (lp, 0.5, lrate, false);
            gen.build (lp.lfo, lp.lfo2, flags);
            const auto v = f32 (lp.lfo[0] * 1.52587890625e-05);
            const auto inc = f32 (std::exp (f32 (v) * std::log (10001.0)) - 1.0) * 6871947.6736 * lrate;
            lfoInc98 = (double) rndi (std::min (inc, 805306368.0));
            lfoInc = (flags & 8) != 0 ? 0.0 : lfoInc98;
        }
        base = mode == 1 ? 0.0 : 1.0;
        for (int i = 2; i < 11; ++i)
        {
            const auto& mp = a.parts[(std::size_t) i];
            bool cst = ! mp.pts.empty();
            for (const auto& p : mp.pts)
                if ((float) p.y != (float) mp.pts[0].y)
                    cst = false;
            if (i == 7)
            {
                if (! cst && en (mp))
                    hasMapRand = true, mapRand.build (mp.pts, centre);
                if (! en (mp))
                    cst = false;
            }
            else if (i == 8)
            {
                if (! cst)
                    hasMapUni = true, mapUni.build (mp.pts, centre);
            }
            else if (! cst)
                hasMap[(std::size_t) i] = true, maps[(std::size_t) i].build (mp.pts, centre);
            if (cst)
            {
                const auto v = f32 (f32 (mp.pts[0].y) - centre);
                base = mode == 0 ? f32 (base * v) : mode == 1 ? f32 (base + v) : f32 (f32 (base * v) * 2.0);
            }
        }
        orderCount = 0;
        if (hasEnv)
            order[(std::size_t) orderCount++] = -1;
        if (lfoOn)
            order[(std::size_t) orderCount++] = -2;
        for (int i = 2; i < 11; ++i)
            if (hasMap[(std::size_t) i])
                order[(std::size_t) orderCount++] = i;
    }
};

struct ArtInputs
{
    std::array<double, 11> in {};
    double lspeed = 1.0, lamt = 1.0;
};

struct ArtState
{
    const Art* a = nullptr;
    double base = 0.0, out = 0.0, acc = 0.0, depth = 1.0, esScale0 = 1.0, lout = 0.0;
    EnvState es, ls;
    std::uint32_t lph = 0;
    std::array<double, 11> cacheX {}, cacheV {};
    std::array<bool, 11> cached {};

    void start (const Art& art, double rand, double uni, const EnvState* share, std::uint32_t gphase)
    {
        a = &art;
        auto b = art.base;
        if (art.hasMapRand)
            b = art.comb (b, f32 (art.mapRand.valueAt (rand)));
        if (art.hasMapUni)
            b = art.comb (b, f32 (art.mapUni.valueAt (uni)));
        base = b;
        cached.fill (false);
        if (art.hasEnv)
        {
            es = share != nullptr ? *share : art.env.start();
            if (share != nullptr && es.aidx >= 0) // inside an arpeggio: join muted at the last marker's level (FUN_007a2cc0)
                es.muted = true, es.ys = es.hold, es.dl = 0.0, es.out = es.ys;
        }
        esScale0 = art.hasEnv ? art.env.scale[0] : 1.0;
        depth = art.mode == 2 ? 2.0 : 1.0;
        if (art.k == 4 || art.k == 5)
            depth = 0.0; // TriggerVoice zeroes the filter-frequency envelope depth
        auto ac = b;
        if (art.hasEnv)
            ac = f32 (ac + f32 (es.out * art.envDepth));
        if (art.lfoOn)
        {
            ls = art.lfoEnv.start();
            lph = art.lfoGlobal ? gphase : art.gen.start;
            lout = 0.0;
        }
        acc = art.orderCount > 0 ? ac : b;
        out = acc;
    }

    double step (double dt, const ArtInputs& inputs) noexcept
    {
        const auto& A = *a;
        auto ac = base;
        for (int o = 0; o < A.orderCount; ++o)
        {
            const auto c = A.order[(std::size_t) o];
            if (c == -1)
            {
                const auto v = A.env.step (es, dt);
                ac = A.mode == 1 ? f32 (ac + f32 (v * depth)) : f32 (f32 (ac * v) * depth);
            }
            else if (c == -2)
            {
                auto& s = ls;
                auto d8 = dt;
                if (s.idx == 0 && s.stage < 9 && s.t < 1.0)
                {
                    d8 = s.rate != 0.0 ? dt + (s.t - 1.0) / s.rate : dt;
                    if (d8 <= 0.0)
                        d8 = 0.0;
                }
                A.lfoEnv.step (s, dt);
                if (s.idx != 0 || s.stage > 8 || s.t >= 1.0)
                {
                    lph = (std::uint32_t) ((long long) lph + rndi (inputs.lspeed * d8 * A.lfoInc));
                    lout = f32 (s.out * A.gen.value (lph));
                }
                const auto amt = inputs.lamt;
                ac = A.mode == 1 ? f32 (ac + f32 (lout * amt)) : f32 (ac * f32 (f32 (f32 (lout * amt) * 2.0) + 1.0));
            }
            else
            {
                const auto x = inputs.in[(std::size_t) c];
                if (! cached[(std::size_t) c] || cacheX[(std::size_t) c] != x)
                {
                    cached[(std::size_t) c] = true;
                    cacheX[(std::size_t) c] = x;
                    cacheV[(std::size_t) c] = f32 (A.maps[(std::size_t) c].valueAt (x));
                }
                ac = A.comb (ac, cacheV[(std::size_t) c]);
            }
        }
        acc = ac;
        out = f32 (ac + f32 ((out - ac) * A.smooth));
        return out;
    }

    void first() noexcept { out = acc; }

    void release() noexcept
    {
        if (a->hasEnv)
            a->env.release (es);
        if (a->lfoOn)
            a->lfoEnv.release (ls);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Tables (tabs.py): filter/resonance shapes, masks, global EQ — regenerated from our closed forms, never copied

struct Table
{
    std::vector<float> t;
    double o = 0.0, eL = 1.0, eR = 1.0, S = 1024.0, S2 = 1024.0; // float32 values
    bool flag = false;

    Table() = default;
    Table (std::vector<float> v, double origin, double el, double er, double s = 1024.0, double s2 = 1024.0)
        : t (std::move (v)), o (f32 (origin)), eL (f32 (el)), eR (f32 (er)), S (f32 (s)), S2 (f32 (s2)), flag (true)
    {
    }
};

void zeroCut (std::vector<float>& t, double th)
{
    for (auto& v : t)
        if (std::fabs ((double) v) < th)
            v = 0.0f;
}

// drop the leading run equal to entry 0 (keeping one) and the trailing run equal to the last (keeping one)
int trimTable (std::vector<float>& t)
{
    auto bits = [&] (std::size_t i) { std::uint32_t b; std::memcpy (&b, &t[i], 4); return b; };
    const auto n = t.size();
    std::size_t k = 1;
    while (k < n && bits (k) == bits (0))
        ++k;
    k -= 1;
    t.erase (t.begin(), t.begin() + (std::ptrdiff_t) k);
    auto m = t.size();
    while (m > 1 && bits (m - 2) == bits (m - 1))
        --m;
    t.resize (m);
    return (int) k;
}

Table& finish (Table& T, int code)
{
    zeroCut (T.t, zth);
    if (code < 200 || code > 299)
    {
        const auto k = trimTable (T.t);
        T.o = f32 (T.o - k);
    }
    if (T.t.size() < 2)
    {
        const float neutral = (code < 100 || code > 199) ? 1.0f : 0.0f;
        T.flag = T.t.empty() ? false : T.t[0] != neutral;
    }
    return T;
}

const double ln10over20 = 0.115129255; // float32 runtime constant (0x92bff8)

double dbOf2 (double x) noexcept // float(exp(float(f32(x·−72)·f32(ln10/20))))
{
    const auto a = (float) (x * -72.0);
    return f32 (std::exp ((double) (a * (float) ln10over20)));
}
double p16 (double x) noexcept
{
    const auto x4 = x * x * x * x;
    return x4 * x4 * x4 * x4;
}

// filter-shape / resonance-shape table (FUN_00874ed0) for the codes known so far; nullopt for the others
std::optional<Table> shapeTable (int code, const Curve* curve)
{
    auto mk = [] (int N, double& s) { s = 1.0 / (N - 1); return std::vector<float> ((std::size_t) N, 0.0f); };
    double s = 0.0;
    const auto twoPi = 2.0 * pi;
    std::optional<Table> T;
    if (code == 0)
    {
        auto t = mk (2048, s);
        for (int i = 0; i < 2048; ++i)
        {
            const auto x = i * s;
            t[(std::size_t) i] = (float) (dbOf2 (x) * (1.0 - p16 (x)));
        }
        T = Table (std::move (t), 0, 1, 0);
    }
    else if (code == 1 || code == 2)
    {
        const int N = 4096;
        std::vector<float> t ((std::size_t) N);
        s = 2.0 / N;
        for (int i = 0; i < N; ++i)
        {
            const auto u = 1.0 - std::fabs (1.0 - ((i + N / 2) % N) * s);
            const auto v = dbOf2 (u) * (1.0 - p16 (u));
            t[(std::size_t) i] = (float) (code == 1 ? v : 1.0 - v);
        }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 1);
    }
    else if (code == 3)
    {
        auto t = mk (2048, s);
        for (int i = 0; i < 2048; ++i)
        {
            const auto x = 1.0 - i * s;
            t[(std::size_t) i] = (float) (dbOf2 (x) * (1.0 - p16 (x)));
        }
        T = Table (std::move (t), 2047, 0, 1);
    }
    else if (code == 4 || code == 7)
    {
        auto t = mk (1024, s);
        for (int i = 0; i < 1024; ++i)
        {
            const auto c = std::cos ((1.0 - i * s) * pi) + 1.0;
            const auto v = code == 4 ? 1.0 - c * 0.5 : c * 0.5;
            t[(std::size_t) i] = (float) (v * v);
        }
        T = code == 4 ? Table (std::move (t), 0, 1, 0) : Table (std::move (t), 1023, 0, 1);
    }
    else if (code == 5 || code == 6)
    {
        auto t = mk (2048, s);
        for (int i = 0; i < 2048; ++i)
        {
            const auto c = std::cos ((1.0 - i * s) * twoPi) + 1.0;
            const auto v = code == 5 ? 1.0 - c * 0.5 : c * 0.5;
            t[(std::size_t) i] = (float) (v * v);
        }
        T = Table (std::move (t), 0.5 * 2047, 1, 1);
    }
    else if (code == 9)
    {
        auto t = mk (1024, s);
        for (int i = 0; i < 1024; ++i)
        {
            const auto x = 1.0 - i * s;
            t[(std::size_t) i] = (float) (f32 (std::exp (f32 (x) * std::log (1001.0))) - 1.0);
            t[(std::size_t) i] = (float) ((double) t[(std::size_t) i] * 0.00075 + x * 0.25);
        }
        T = Table (std::move (t), 0, 1, 0);
    }
    else if (code == 14)
    {
        auto t = mk (4096, s);
        for (int i = 0; i < 4096; ++i)
        {
            const auto c = (std::cos ((1.0 - i * s) * 25.132741228718345) + 1.0) * 0.5;
            t[(std::size_t) i] = (float) (c * c);
        }
        T = Table (std::move (t), 0.5 * 4095, 1, 1);
    }
    // the filter shapes 8, 10…13, 15, 16 (REA_rodada165g-flbeta165s §4, [DECOMP FUN_00874ed0] + DLL constants)
    else if (code == 8 || code == 10)
    {
        const int N = code == 8 ? 682 : 1365;
        auto t = mk (N, s);
        for (int i = 0; i < N; ++i)
        {
            const auto c = std::cos ((1.0 - i * s * 0.5) * pi);
            if (code == 8)
                t[(std::size_t) i] = (float) (1.0 - (c + 1.0));
            else
            {
                const auto u = 1.0 - (1.0 - (c + 1.0));
                t[(std::size_t) i] = (float) (dbOf2 (u) * (1.0 - p16 (u)));
            }
        }
        T = Table (std::move (t), 0, 1, 0);
    }
    else if (code == 11)
    {
        auto t = mk (1024, s);
        for (int i = 0; i < 1024; ++i)
        {
            const auto a = 1.0 - (std::cos ((1.0 - i * s) * 9.42477796076938) + 1.0) * 0.5;
            t[(std::size_t) i] = (float) (a * (1.0 - (std::cos ((1.0 - i * s) * pi) + 1.0) * 0.5));
        }
        T = Table (std::move (t), 0, 1, 0);
    }
    else if (code == 12)
    {
        const int N = 4096, half = N / 2;
        auto t = mk (N, s);
        for (int i = 0; i < N; ++i)
            if (i < half)
            {
                const auto v = 1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.25;
                t[(std::size_t) i] = (float) (v * v);
            }
            else
            {
                const auto x = (double) (i - half) * s * 2.0;
                t[(std::size_t) i] = (float) (dbOf2 (x) * (1.0 - p16 (x)));
            }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if (code == 13)
    {
        auto t = mk (1024, s);
        for (int i = 0; i < 1024; ++i)
        {
            const auto c = std::cos ((1.0 - i * s) * pi);
            const auto a = (float) ((1.0 - (c + 1.0) * 0.5) * 18.0 + -18.0);
            t[(std::size_t) i] = (float) std::exp ((double) (a * (float) ln10over20));
        }
        T = Table (std::move (t), 0, 1, 0);
    }
    else if (code == 15)
    {
        auto t = mk (4096, s);
        for (int i = 0; i < 4096; ++i)
        {
            const auto a = (std::cos ((1.0 - i * s) * 37.69911184307752) + 1.0) * 0.5;
            const auto b = 1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.5;
            t[(std::size_t) i] = (float) (a * a * b * b + b * 0.125);
        }
        T = Table (std::move (t), 0.5 * 4095, 1, 1);
    }
    else if (code == 16)
    {
        const int N = 2048, M = N / 3;
        std::vector<float> t ((std::size_t) N, 1.0f);
        s = 0.5 / M;
        for (int i = 0; i < M; ++i)
        {
            auto v = 1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.5;
            v = v * v;
            t[(std::size_t) i] = t[(std::size_t) (N - 1 - i)] = (float) v;
        }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 1);
    }
    else if (code == 100 || code == 108)
    {
        auto t = mk (1024, s);
        for (int i = 0; i < 1024; ++i)
        {
            const auto c = std::cos ((1.0 - i * s) * twoPi);
            if (code == 100)
                t[(std::size_t) i] = (float) ((1.0 - (c + 1.0) * 0.5) * 7.0 - 1.0);
            else
            {
                auto v = (1.0 - (c + 1.0) * 0.5) * 6.8 - 0.8;
                if (i > 512)
                    v = v - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 3.0;
                t[(std::size_t) i] = (float) v;
            }
        }
        T = Table (std::move (t), 0.5 * 1023, 1, 0);
    }
    else if (code == 101)
    {
        const int N = 1536;
        auto t = mk (N, s);
        const auto d16 = s * 9.42477796076938;
        for (int i = 0; i < N; ++i)
        {
            const auto d30 = 1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.5;
            t[(std::size_t) i] = (float) (std::cos ((i - N / 2) * d16) * d30 * 6.25);
        }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if (code == 102 || code == 104)
    {
        const int N = code == 102 ? 512 : 1024;
        std::vector<float> t ((std::size_t) N, code == 102 ? 5.8f : 5.4f);
        const int M = N / 3;
        s = 0.5 / M;
        for (int i = 0; i < M; ++i)
        {
            auto v = 1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.5;
            v = code == 102 ? v * 5.8 : v * v * 5.4;
            t[(std::size_t) i] = (float) v;
            t[(std::size_t) (N - 1 - i)] = (float) v;
        }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if (code == 103)
    {
        const int N = 512;
        std::vector<float> t ((std::size_t) N);
        s = 2.0 / N;
        for (int i = 0; i < N; ++i)
            t[(std::size_t) i] = (float) (std::fabs (1.0 - ((i + N / 2) % N) * s) * 6.9);
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if (code == 105)
    {
        const int N = 1536;
        auto t = mk (N, s);
        for (int i = 0; i < N; ++i)
        {
            const auto v = 1.0 - (std::cos ((1.0 - i * s) * 12.566370614359172) + 1.0) * 0.5;
            t[(std::size_t) i] = (float) (v * v * 6.2);
        }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if (code == 106)
    {
        const int N = 682;
        auto t = mk (N, s);
        for (int i = 0; i < N; ++i)
            t[(std::size_t) i] = (float) (-(1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.5) * 8.0);
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if (code == 107)
    {
        const int N = 1024, h = N / 2;
        auto t = mk (N, s);
        for (int i = 0; i < N; ++i)
        {
            const auto d31 = 1.0 - (std::cos ((1.0 - i * s) * twoPi) + 1.0) * 0.5;
            const auto d30 = (i - h) * 2 * s * 2 * pi;
            const auto sc = i == h ? 1.0 : std::sin (d30) / d30;
            const auto p = d31 == 0.0 ? 0.0 : std::pow (d31, 0.00125);
            t[(std::size_t) i] = (float) (-sc * p * 10.0);
        }
        T = Table (std::move (t), 0.5 * (N - 1), 1, 0);
    }
    else if ((code == 17 || code == 18 || code == 110 || code == 111) && curve != nullptr)
    {
        const int N = 1024;
        auto t = curve->render (N);
        if (code >= 110)
        {
            for (auto& v : t)
                v = v * 24.0f;
            T = Table (std::move (t), 0.5 * (N - 1), 1, 1, N * 0.25, N);
        }
        else
        {
            for (auto& v : t)
                v = v * v;
            int k = 0;
            while (k < N / 2 && t[(std::size_t) k] == 1.0f)
                ++k;
            const auto eR = f32 (1.0 - (k * 2.0) / N); // leading run of 1.0 → upper range extension
            k = 0;
            while (k < N / 2 && t[(std::size_t) (N - k - 1)] == 1.0f)
                ++k;
            const auto eL = f32 (1.0 - (k * 2.0) / N); // trailing run of 1.0 → lower range extension
            T = Table (std::move (t), 0.5 * (N - 1), 1, 1, N * 0.25, N);
            T->eR = eR, T->eL = eL;
        }
    }
    if (T)
        finish (*T, code);
    return T;
}

// mask tables (codes 400…800): 2160 entries, 240 per octave from log2 f = X0 + 1
Table maskTable (const Curve& curve)
{
    const int N = 2160;
    auto t = curve.render (2161);
    t.resize ((std::size_t) N);
    for (auto& v : t)
        v = v * v;
    const auto o = f32 (((-1.0 - x0Log) + 0.0020833333333333333) / 9.0 * (N - 1));
    Table T (std::move (t), o, 1, 1, f32 (N / 9.0), N);
    finish (T, 400);
    return T;
}

// global EQ table (code 300): 2421 entries, 240 per octave from log2 16.35 − 1, cosine skirts, (4v)², with the mix
struct GlobalEq
{
    Table table;
    double fmax = 0.0;
};

GlobalEq globalEq (const Curve& curve, float mix)
{
    const int N = (int) rnd (10.083333333333334 * 240.0);
    std::vector<float> tab ((std::size_t) N + 1, 0.0f);
    curve.render (2161, 0.0, tab.data() + 240);
    if (mix != 1.0f)
    {
        const auto add = (1.0f - mix) * 0.25f;
        for (int i = 240; i < 240 + 2161; ++i)
            tab[(std::size_t) i] = tab[(std::size_t) i] * mix + add;
    }
    auto d = (double) tab[240] * 0.5;
    auto st = 1.0 / 240.0;
    for (int i = 0; i < 240; ++i)
        tab[(std::size_t) i] = (float) ((std::cos ((1.0 - i * st) * pi) + 1.0) * d);
    d = (double) tab[2400] * 0.5;
    const int M = N - 0x961;
    st = 1.0 / M;
    for (int i = 1; i <= M; ++i)
        tab[(std::size_t) (2400 + i)] = (float) ((std::cos (i * st * pi) + 1.0) * d);
    tab.resize ((std::size_t) N);
    for (auto& v : tab)
    {
        const auto w = v * 4.0f;
        v = w * w;
    }
    auto o = f32 (((-x0Log) + 0.0020833333333333333) / 10.083333333333334 * (N - 1));
    const auto S = f32 (N / 10.083333333333334);
    int i = N - 1;
    while (i >= 1 && tab[(std::size_t) i] <= 0.0f)
        --i;
    GlobalEq g;
    g.fmax = f32 (std::pow (2.0, x0Log + i / 240.0));
    zeroCut (tab, zth);
    const auto k = trimTable (tab);
    o = f32 (o - k);
    g.table = Table (std::move (tab), o, 1, 1, S);
    return g;
}

// table read (FUN_00874930): precision 0 → nearest entry; ≥ 1 → linear interpolation (index = round(x − 0.5))
inline float tableRead (const Table& T, float x, int prec) noexcept
{
    const auto N = (long long) T.t.size();
    if (prec >= 1)
    {
        const auto i = toInt64 ((double) rndf (x - 0.5f));
        if (i < 0)
            return T.t[0];
        if (i >= N - 1)
            return T.t[(std::size_t) (N - 1)];
        const auto fr = x - (float) i;
        return T.t[(std::size_t) i] * (1.0f - fr) + T.t[(std::size_t) i + 1] * fr;
    }
    float y = x;
    if (std::isnan (y))
        y = 0.0f;
    else if (std::isinf (y))
        y = y > 0.0f ? (float) (N - 1) : 0.0f;
    y = std::min (std::max (y, 0.0f), (float) (N - 1));
    return T.t[(std::size_t) rndf (y)];
}

// ---------------------------------------------------------------------------------------------------------------
// Phaser tables (hphaser.py): closed forms of round 32, RMS-normalised

struct PhaserInfo
{
    int n = 0;
    double f18 = 1024.0, f1c = 1024.0;
};

PhaserInfo phaserInfo (int code) noexcept
{
    switch (code)
    {
        case 200: case 201: case 202: case 203: case 204: case 205: return { 1024, 1024, 1024 };
        case 206: return { 2048, 2048, 1024 };
        case 207: return { 2048, 2048, 512 };
        case 208: return { 2048, 2048, 2048 };
        case 209: return { 2048, 2048, 2048 };
        default: return { 0, 1024, 1024 };
    }
}

double l72f (double d) noexcept
{
    const auto a = (float) (d * -72.0);
    return f32 (std::exp ((double) (a * (float) (std::log (10.0) / 20.0)))) * (1.0 - std::pow (d, 16.0));
}

std::vector<float> phaserTable (int code)
{
    const auto N = phaserInfo (code).n;
    std::vector<double> T ((std::size_t) N, 0.0);
    auto th = [N] (int i, int sh) { return 2.0 * pi * ((i + sh) % N) / N; };
    for (int i = 0; i < N; ++i)
    {
        auto& v = T[(std::size_t) i];
        if (code == 200)
            v = std::sin (th (i, 3 * N / 4)) + 1.0;
        else if (code == 201)
        {
            const auto a = std::fabs (1.0 - ((i + N / 2) % N) * (2.0 / N));
            v = a * a;
        }
        else if (code == 202)
            v = std::sin (i * pi / N);
        else if (code == 203)
        {
            const auto s = std::sin (th (i, 3 * N / 4)) * 0.5 + 0.5;
            v = std::pow (std::pow (s, 3.0), 2.0);
        }
        else if (code == 204)
            v = l72f (1.0 - std::fabs (1.0 - ((i + N / 2) % N) * (2.0 / N)));
        else if (code == 205)
        {
            const auto s = std::pow (std::sin (th (i, 3 * N / 4)) * 0.5 + 0.5, 2.0);
            v = l72f (1.0 - s * s);
        }
        else if (code == 206)
        {
            const auto a = 1.0 - (std::sin (th (i, N / 4)) * 0.5 + 0.5);
            const auto t1 = l72f (a);
            const auto b = std::sin (th (i, 3 * N / 4)) * 0.5 + 0.5;
            const auto t2 = l72f (1.0 - b * b);
            v = f32 (t1) + t2 * 4.0;
        }
        else if (code == 209)
            v = std::sin (th (i, 3 * N / 4));
    }
    if (code == 207)
    {
        const auto l50 = 1.0 / (N - 1);
        for (int i = 0; i < N; ++i)
            T[(std::size_t) ((i + N / 8) % N)] = (std::sin ((4 * i * l50 + 0.75) * 2.0 * pi) + 1.0) * (1.0 - i * l50);
    }
    if (code == 208)
        for (int i = 0; i < N / 2; ++i)
        {
            const auto s = std::sin (((i + 3 * N / 8) % N) * (4.0 * pi / N));
            const auto e = std::exp (f32 (1.0 - std::fabs (s)) * std::log (1000001.0));
            const auto f = (1.0 - f32 (e - 1.0) / 1000000.0) * sgn (s) + 1.0;
            T[(std::size_t) (i + N / 4)] = std::pow (f, 3.0);
        }
    std::vector<double> sq ((std::size_t) N);
    for (int i = 0; i < N; ++i)
    {
        T[(std::size_t) i] = f32 (T[(std::size_t) i]);
        sq[(std::size_t) i] = T[(std::size_t) i] * T[(std::size_t) i];
    }
    const auto r = std::sqrt (pairwiseSum (sq.data(), sq.size()) / N);
    std::vector<float> out ((std::size_t) N);
    for (int i = 0; i < N; ++i)
        out[(std::size_t) i] = (float) (r > 0.0 ? T[(std::size_t) i] * (double) (float) (1.0 / r) : T[(std::size_t) i]);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Random sources (hrand.py): MT19937 per plugin and the uniform generator of the installation's IPP

struct Mt19937
{
    std::array<std::uint32_t, 624> mt {};
    int i = 624;

    void seed (std::uint32_t s) noexcept
    {
        mt[0] = s;
        for (int k = 1; k < 624; ++k)
            mt[(std::size_t) k] = 1812433253u * (mt[(std::size_t) k - 1] ^ (mt[(std::size_t) k - 1] >> 30)) + (std::uint32_t) k;
        i = 624;
    }

    std::uint32_t next() noexcept
    {
        if (i >= 624)
        {
            for (int k = 0; k < 624; ++k)
            {
                const auto y = (mt[(std::size_t) k] & 0x80000000u) | (mt[(std::size_t) ((k + 1) % 624)] & 0x7fffffffu);
                mt[(std::size_t) k] = mt[(std::size_t) ((k + 397) % 624)] ^ (y >> 1) ^ ((y & 1u) != 0 ? 0x9908b0dfu : 0u);
            }
            i = 0;
        }
        auto y = mt[(std::size_t) i++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }
};

// the integer recurrence of IPP's RandUniform_Direct (REA round 24 §1.3, as our model restates it)
struct IppUniform
{
    std::int32_t e0 = 0, e1 = 0, e2 = 0;
    std::uint32_t L = 0x436cbae9u;
    std::int32_t borrow = -1;

    explicit IppUniform (std::uint32_t seed) noexcept
    {
        const std::uint32_t A = 69069u, C = 0x3c6ef373u;
        const auto s1 = seed * A + C;
        e0 = (std::int32_t) (s1 * A + C);
        e1 = (std::int32_t) s1;
        e2 = (std::int32_t) seed;
    }

    std::int32_t next() noexcept
    {
        auto v = (std::int32_t) ((std::uint32_t) e1 - (std::uint32_t) e0 + (std::uint32_t) borrow);
        borrow = v < 0 ? -1 : 0;
        if (v < 0)
            v = (std::int32_t) ((std::uint32_t) v - 18u);
        e0 = e1, e1 = e2, e2 = v;
        L = L * 69069u + 0x3c6ef373u;
        return (std::int32_t) ((std::uint32_t) v + L);
    }

    std::uint32_t seedOut() const noexcept { return (std::uint32_t) e2; }
};

// n float32 uniform draws in [lo, hi] (ippsRandUniform_Direct_32f); returns the next seed
std::uint32_t rand32f (float* out, int n, float lo, float hi, std::uint32_t seed) noexcept
{
    IppUniform g (seed);
    const float sc = (hi - lo) * 2.3283064365386963e-10f, of = (lo + hi) * 0.5f;
    for (int k = 0; k < n; ++k)
        out[k] = (float) g.next() * sc + of;
    return g.seedOut();
}

std::uint32_t rand64f (double* out, int n, double lo, double hi, std::uint32_t seed) noexcept
{
    IppUniform g (seed);
    const auto sc = (hi - lo) * 2.3283064365386963e-10, of = (lo + hi) * 0.5;
    for (int k = 0; k < n; ++k)
        out[k] = (double) g.next() * sc + of;
    return g.seedOut();
}

// ---------------------------------------------------------------------------------------------------------------
// Per-part patch (hmodel.Part / Filt): everything derived from the state, the rate and the tempo

enum Module : std::uint8_t { mLeq, mPrism, mHarmonizer, mFilter, mPhaser, mPluck, mBlur, mProt, mClip, mVolume, mHrand };

// per-type unison detune factors (DLL data 0x928290, DATA — 3×9 constants, as round 32's)
const double unisonDetuneD[3][9] {
    { 0.3069306930693069, -0.2897196261682243, 0.3924050632911392, -0.22302158273381295, 0.5081967213114754,
      -0.2366412213740458, 0.43661971830985913, -0.34831460674157305, 0.2743362831858407 },
    { 0.8904719501335708, 1.0764262648008611, 0.9115770282588879, 1.1025358324145536, 1.0341261633919339,
      1.1668611435239207, 0.8539709649871904, 1.2062726176115801, 1.0 },
    { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 } };

struct Filt
{
    int code = 0, rcode = 0;
    Table tab, rtab, mask, rmask;
    bool hasRtab = false, noise = false, adaptEnv = false, hz = false, rhz = false, radapt = false;
    double w = 0, lo = 0, hi = 0, span = 0, span2 = 0, knob = 0, kt = 0, envamt = 0, rw = 0, rofsOct = 0, rofsF = 0, k0 = 0;
    int r = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Image synthesis (REA_rodada75; our model r75/py/imgm.py): the part's image object (two planes of columns × rows
// pixels: gain and frequency offset) read along a time position, applied to the timbre mix and to the partial
// frequencies; formant shift with its per-column pitch track. Resynthesis-mode images (a sample analysed at load
// time) are not modelled: such parts keep playing their timbres.

struct ImageData
{
    bool usable = false;
    std::string why;            // why an image tail is not used (fallback report)
    int cols = 0, rows = 0, stride = 0, loopMode = 0;
    float f100 = 0.0f;
    double e8 = -1.0, f0 = -1.0, f8 = 0.0;
    std::vector<float> gain, freq; // planes 0 and 1 (cols × stride, the padding is 0)
};

// The image object stored after the part's 26 shaping curves (FUN_00793300, read path)
ImageData parseImage (const std::vector<std::uint8_t>& t)
{
    ImageData im;
    if (t.size() < 4 + 0x2c)
        return im;
    Reader r { t.data(), t.size(), 0 };
    if (r.i32() >= 1)
    {
        im.why = "image object version not supported";
        return im;
    }
    const auto mode = r.i32();
    im.loopMode = r.i32();
    im.f100 = r.f32v();
    im.e8 = r.f64v(), im.f0 = r.f64v(), im.f8 = r.f64v();
    im.cols = r.i32(), im.rows = r.i32();
    if (mode != 0)
    {
        im.why = "resynthesis-mode image (not modelled: the part plays its timbres)";
        return im;
    }
    if (r.ok && (im.cols == 0 || im.rows == 0))
        return im; // (an empty image object: every part without an image stores one)
    if (! r.ok || im.cols < 0 || im.rows < 0 || im.rows > 4096 || (long long) im.cols * im.rows > 64LL * 1024 * 1024)
    {
        im.why = "image data unreadable";
        return im;
    }
    im.stride = (im.rows + 7) & ~7;
    for (int pl = 0; pl < 3; ++pl)
    {
        const auto n = r.i32();
        if (n <= 0)
            continue;
        if (! r.need ((std::size_t) im.cols * (std::size_t) im.rows * 4))
            break;
        std::vector<float> a ((std::size_t) im.cols * (std::size_t) im.stride, 0.0f);
        for (int c = 0; c < im.cols; ++c)
            std::memcpy (a.data() + (std::size_t) c * (std::size_t) im.stride, t.data() + r.i + (std::size_t) c * (std::size_t) im.rows * 4,
                         (std::size_t) im.rows * 4);
        r.i += (std::size_t) im.cols * (std::size_t) im.rows * 4;
        if (pl == 0)
            im.gain = std::move (a);
        else if (pl == 1)
            im.freq = std::move (a);
    }
    if (! r.ok)
    {
        im.why = "image data unreadable";
        return im;
    }
    if (im.gain.empty() && im.freq.empty())
        return im; // (no planes: nothing to play, like a part without image)
    im.usable = true;
    return im;
}

inline float expF (float x) noexcept { return (float) std::exp ((double) x); }     // IPP Exp_32f (taken as correctly rounded)
inline float expm1F (float x) noexcept { return (float) std::expm1 ((double) x); } // IPP Expm1_32f
inline float divF (float a, float b) noexcept { return (float) ((double) a / (double) b); } // IPP Div_32f

struct Image
{
    int cols = 0, rows = 0, stride = 0, lm = 0, k5c = 0, k90 = 0, k94 = 0;
    std::vector<float> gain, freq;
    bool hasGain = false, hasFreq = false;
    double s60 = 0.0, s68 = 0.0, s70 = 0.0, s88 = 0.0, s78 = std::numeric_limits<double>::max();
    float a0 = 0.0f;
    float k30 = 0.0f, k34 = 0.0f, k38 = 1.0f, k174 = 0.0f, smThr = 0.0f, smCoef = 0.0f, gmix = 1.0f, fmix = 0.0f;
    double toff = 0.0, speed = 0.0;
    int interpGain = 0, interpFreq = 0, fmode = 0;
    std::vector<float> t48;                 // per-column fundamental position (pitch track), empty = all 0
    bool hasT78 = false;
    std::array<float, bankSize> t78 {};     // formant mutation positions (part +0x78)
    std::array<float, bankSize> fmask {};   // image freq local mask (part +0x180)
    int fmaskSt = 0, fmaskN = 0;

    static float pix2ampK() noexcept { return (float) (std::log (256.0) / 255.0); }

    // FUN_007981f0: (256^(pix/255) − 1) / 255 for one column
    void pix2amp (const float* col, float* out) const noexcept
    {
        const auto k = pix2ampK();
        for (int i = 0; i < stride; ++i)
            out[i] = expm1F (col[i] * k) * 0.003921568859368563f;
    }

    // FUN_00798400
    int colidx (long long i) const noexcept
    {
        if (k5c < 1)
            return (int) std::min (std::max (i, 0LL), (long long) cols - 1);
        if (i < 0)
        {
            if (k5c < 2)
                return 0;
            while (i < 0)
                i += k94;
            return (int) i;
        }
        while (i >= k90)
            i -= k94;
        return (int) i;
    }

    // FUN_00797bd0: plane at position pos, cnt entries, with the loop crossfade (FUN_00793020)
    void column (const std::vector<float>& A, double pos, int interp, int cnt, float* out) const noexcept
    {
        column1 (A, pos, interp, cnt, out);
        if (pos <= s78)
            return;
        const auto t = (float) ((pos - s78) * (double) a0);
        float sec[4096 + 8];
        column1 (A, pos - s70, interp, cnt, sec);
        for (int j = 0; j < cnt; ++j)
            out[j] = (sec[j] - out[j]) * t + out[j];
    }

    void column1 (const std::vector<float>& A, double pos, int interp, int cnt, float* out) const noexcept
    {
        const auto fi = std::floor (pos);
        const auto i = (long long) fi;
        auto t = (float) (pos - fi);
        const float* c1 = A.data() + (std::size_t) colidx (i) * (std::size_t) stride;
        if (interp < 1)
        {
            std::copy (c1, c1 + cnt, out);
            return;
        }
        if (interp == 4)
        {
            const float* p0 = A.data() + (std::size_t) colidx (i - 1) * (std::size_t) stride;
            const float* p2 = A.data() + (std::size_t) colidx (i + 1) * (std::size_t) stride;
            const float* p3 = A.data() + (std::size_t) colidx (i + 2) * (std::size_t) stride;
            for (int j = 0; j < cnt; ++j)
            {
                auto a = (c1[j] - p2[j]) * 1.5f + (p3[j] - p0[j]) * 0.5f;
                a = a * t + p0[j];
                a = a - p3[j] * 0.5f;
                a = a + p2[j];
                a = a + p2[j];
                a = a - c1[j] * 2.5f;
                a = a * t + (p2[j] - p0[j]) * 0.5f;
                out[j] = a * t + c1[j];
            }
            return;
        }
        if (interp == 1)
        {
            if (t > 0.5f)
            {
                auto u = 2.0f - t * 2.0f;
                u = (u * u) * u;
                t = (float) (1.0 - (double) ((u * u) * u) * 0.5);
            }
            else
            {
                auto u = t * 2.0f;
                u = (u * u) * u;
                t = (float) ((double) ((u * u) * u) * 0.5);
            }
        }
        else if (interp == 2)
        {
            if (t > 0.5f)
            {
                const auto u = 2.0f - t * 2.0f;
                t = (float) (1.0 - (double) ((u * u) * u) * 0.5);
            }
            else
            {
                const auto u = t * 2.0f;
                t = (float) ((double) ((u * u) * u) * 0.5);
            }
        }
        else if (interp == 5)
        {
            auto u = 1.0f - t;
            u = (u * u) * u;
            t = 1.0f - u * u;
        }
        else if (interp != 3)
        {
            const auto u = (t * t) * t;
            t = u * u;
        }
        const float* c2 = A.data() + (std::size_t) colidx (i + 1) * (std::size_t) stride;
        const auto w1 = 1.0f - t;
        for (int j = 0; j < cnt; ++j)
            out[j] = c1[j] * w1 + c2[j] * t;
    }

    // gain plane at pos → partial levels (FUN_00798110 + FUN_00797570 + FUN_00798130; the +0x50 table is all 1)
    void amp (double pos, int cnt, float* out) const noexcept
    {
        column (gain, pos, interpGain, cnt, out);
        const float g = 1.0f;
        if (k34 == 0.0f)
        {
            const auto k = k30 * g;
            for (int j = 0; j < cnt; ++j)
                out[j] = std::max (out[j] * k, 0.0f);
            return;
        }
        const auto k = k34 * g;
        for (int j = 0; j < cnt; ++j)
            out[j] = expm1F (std::max (out[j] * k30, 0.0f)) * k;
    }

    void setGainScale (double x) noexcept
    {
        // FUN_007975f0 / FUN_007976e0
        if (x == 0.0)
        {
            k30 = (float) ((double) k38 * 0.0011300795218523825);
            k34 = 0.0f;
        }
        else
        {
            k30 = (float) (std::log (x + 1.0) * 0.00392156862745098);
            k34 = (float) (1.0 / x);
            const auto fv = (float) ((std::exp ((double) (170.0f * k30)) - 1.0) * (double) k34);
            k34 = (float) (((double) k38 * 0.192113518714905) / (x * (double) fv));
        }
    }

    // FUN_00798450 (loop points; the +0x100 per-column gain table is not modelled)
    bool loopSetup (const ImageData& d, std::string& why)
    {
        auto e8 = d.e8, f0 = d.f0, f8 = d.f8;
        double a60 = 0.0, a68 = 0.0;
        if (e8 >= 0.0)
        {
            e8 = std::min (std::max (e8, 0.0), cols - 1.0);
            a60 = e8;
        }
        else
            a60 = 0.0, f8 = 0.0;
        if (f0 >= 0.0)
        {
            f0 = std::min (std::max (e8 + 1.0, f0), (double) cols);
            a68 = f0;
        }
        else
            a68 = (double) cols;
        auto a70 = a68 - a60;
        k5c = 0;
        lm = d.loopMode;
        if (lm == 0)
            a60 = 0.0, a68 = (double) cols;
        else if (lm == 1)
        {
            if (f8 > 0.0)
            {
                if (e8 >= 0.0)
                {
                    // loop crossfade (+0xf8): the loop shortened by the fade, a second read one loop length back faded
                    // in over [+0x78, +0x68)
                    a70 = std::max (a70 * (1.0 - f8), 5.960464477539063e-08);
                    s78 = a60 + a70;
                    a0 = (float) (1.0 / (a68 - s78));
                    a60 = a68 - a70;
                }
            }
            else
            {
                const auto i5 = (int) std::min (std::max (rndi (a60), 0LL), (long long) cols - 1);
                const auto l2 = (int) std::min (std::max (rndi (a68), (long long) i5 + 1), (long long) cols);
                a60 = i5, a68 = l2, a70 = l2 - i5;
                k5c = (a70 == (double) cols ? 1 : 0) + 1;
            }
        }
        else if (lm == 2)
        {
            s88 = a68;
            a70 = a70 * 2.0;
            a68 = a60 + a70;
            k5c = a70 == (double) cols ? 2 : 0;
        }
        const auto i5 = (int) std::min (std::max (rndi (a60), 0LL), (long long) cols - 1);
        const auto l2 = (int) std::max (rndi (a68), (long long) i5 + 1);
        k90 = l2, k94 = l2 - i5;
        s60 = a60, s68 = a68, s70 = a70;
        if (d.f100 > 0.0f)
        {
            why = "image per-column gain table (not modelled: the part plays its timbres)";
            return false;
        }
        return true;
    }

    static double hpos (float p, int k) noexcept { return (double) ((((p + 1.0f) * (float) k)) - 1.0f); }

    // per-column fundamental position (+0x48): FUN_00795090 per column, then the clean-up and the 9-tap Hann
    // smoothing of FUN_00795720
    void pitchTrack()
    {
        const int R = rows;
        const float w6[6] { 1.0f, 0.5f, 0.3333333432674408f, 0.25f, 0.20000000298023224f, 0.1666666716337204f };
        const auto k40 = (float) ((R + 1) / 512.0);
        std::vector<float> t ((std::size_t) cols, 0.0f), run ((std::size_t) R, 0.0f), amp ((std::size_t) stride), X ((std::size_t) R),
            q ((std::size_t) R), q2 ((std::size_t) R);
        for (int c = 0; c < cols; ++c)
        {
            pix2amp (gain.data() + (std::size_t) c * (std::size_t) stride, amp.data());
            const float* fp = freq.data() + (std::size_t) c * (std::size_t) stride;
            for (int i = 0; i < R; ++i)
            {
                X[(std::size_t) i] = std::max ((float) i + fp[i], 0.0f) + 1.0f;
                auto v = divF (amp[(std::size_t) i], X[(std::size_t) i] * X[(std::size_t) i]);
                v = v * v;
                v = v * 0.25f;
                v = v + run[(std::size_t) i] * 0.75f;
                q[(std::size_t) i] = v;
            }
            run = q;
            const auto L = (int) std::min ((long long) rndi ((double) (k40 * 64.0f)), (long long) R - 3);
            int best = 2, j = 2;
            float bs = -2147483648.0f;
            if (L > 1)
            {
                for (j = 2; j <= L; ++j)
                {
                    float sc = 0.0f;
                    for (int k = 1; k <= 6; ++k)
                    {
                        auto idx = rndi (hpos ((float) j + fp[j], k));
                        idx = std::min (std::max (idx, 0LL), (long long) R - 1);
                        sc = sc + (float) std::sqrt ((double) (q[(std::size_t) idx] * w6[k - 1]));
                    }
                    if (bs < sc)
                        bs = sc, best = j;
                }
                j = L + 1;
            }
            const auto fa = amp[(std::size_t) best];
            if (best < 3 || amp[(std::size_t) best - 1] * (float) (j + 1) <= fa * (float) j)
            {
                if (best < R && fa * (float) (j + 2) < amp[(std::size_t) best + 1] * (float) (j + 1))
                    ++best;
            }
            else
                --best;
            for (int i = 0; i < R; ++i)
            {
                const auto v = divF (amp[(std::size_t) i], X[(std::size_t) i]);
                q2[(std::size_t) i] = v * v;
            }
            float s1 = 0.0f, s2 = 0.0f;
            const auto K = (int) rndi ((double) (k40 * 50.0f));
            if (K > 1)
                for (int k = 2; k <= K; ++k)
                {
                    auto i1 = rndi (hpos ((float) best + fp[best], k));
                    if (i1 < 0)
                        i1 = 0;
                    if (i1 >= (R >> 1))
                        break;
                    s1 = s1 + q2[(std::size_t) i1];
                    const auto f15 = (float) (best + 2) + fp[best + 2];
                    auto i2 = rndi (hpos (f15, k));
                    if (i2 < 0)
                        i2 = 0;
                    if (i2 >= (R >> 1))
                        break;
                    s2 = s2 + q2[(std::size_t) i2 + 2];
                }
            t[(std::size_t) c] = s2 * 3.0f <= s1 ? (float) best + fp[best] : 10000.0f;
        }
        for (auto& v : t)
            v = std::max (v, 0.0f);
        for (int c = 2; c < cols - 2; ++c)
        {
            double sm = 0.0;
            for (int u = c - 2; u <= c + 2; ++u)
                sm += (double) t[(std::size_t) u];
            const auto s = (float) sm;
            if (s < 10000.0f)
            {
                const auto mean = (float) ((double) s * 0.2);
                if (std::fabs ((double) t[(std::size_t) c] - (double) mean) >= 2.0)
                    t[(std::size_t) c] = mean, c += 2;
            }
        }
        for (int c = 0; c < cols; ++c)
            if (t[(std::size_t) c] >= 10000.0f)
            {
                int j = c;
                while (j < cols - 1 && t[(std::size_t) j + 1] >= 10000.0f)
                    ++j;
                auto lft = c > 0 ? t[(std::size_t) c - 1] : -1.0f;
                auto rgt = j < cols - 1 ? t[(std::size_t) j + 1] : -1.0f;
                if (lft >= 0.0f)
                {
                    if (rgt < 0.0f)
                        rgt = lft;
                }
                else
                {
                    if (rgt < 0.0f)
                        rgt = 0.0f;
                    lft = rgt;
                }
                const int h = (j - c + 1) >> 1;
                for (int u = c; u < c + h; ++u)
                    t[(std::size_t) u] = lft;
                for (int u = c + h; u <= j; ++u)
                    t[(std::size_t) u] = rgt;
                c = j;
            }
        std::array<float, 9> win {};
        std::array<double, 9> wd {};
        for (int i = 0; i < 9; ++i)
            win[(std::size_t) i] = (float) (0.5 - std::cos (6.283185307179586 / 8.0 * i) * 0.5), wd[(std::size_t) i] = win[(std::size_t) i];
        const auto g = (float) (1.0 / (double) (float) pairwiseSum (wd.data(), 9));
        std::vector<float> pad ((std::size_t) cols + 8);
        for (int i = 0; i < cols + 8; ++i)
            pad[(std::size_t) i] = t[(std::size_t) std::min (std::max (i - 4, 0), cols - 1)];
        t48.assign ((std::size_t) cols, 0.0f);
        for (int c = 0; c < cols; ++c)
        {
            double acc = 0.0;
            for (int u = 0; u < 9; ++u)
                acc += (double) pad[(std::size_t) (c + u)] * (double) win[(std::size_t) (8 - u)];
            t48[(std::size_t) c] = (float) acc * g;
        }
    }

    // FUN_00888720: formant shift of the gain column a (stride entries) by x cents; tmp scratch of 4·(stride + 1)
    void formant (float* a, double pos, float x, float* tmp) const noexcept
    {
        const int n = stride;
        float f0 = 0.0f;
        if (! t48.empty())
        {
            const auto fi = std::floor (pos);
            const auto tt = (float) (pos - fi);
            const auto v0 = t48[(std::size_t) colidx ((long long) fi)], v1 = t48[(std::size_t) colidx ((long long) fi + 1)];
            f0 = v0 + tt * (v1 - v0);
        }
        if (! (f0 > 2.0f))
            f0 = 2.0f;
        const auto fac = (float) std::exp ((double) x * -0.0008333333333333334 * ln2);
        const auto c = (float) (1.0 - std::exp (-4.6051701859880918 / (double) (f0 * 3.0f)));
        float* env = tmp;            // n + 1
        float* fine = tmp + n + 1;   // n
        float* outp = fine + n;      // n
        float y = 0.0f;
        for (int k = n - 1; k >= 0; --k)
        {
            const auto v = a[k];
            if (y <= v)
                y = v;
            y = y + (v - y) * c;
            env[k] = y;
        }
        y = 0.0f;
        for (int k = 0; k < n; ++k)
        {
            const auto v = env[k];
            if (y <= v)
                y = v;
            y = y + (v - y) * c;
            env[k] = y;
        }
        for (int k = 0; k < n; ++k)
            env[k] = std::max (env[k], 1.52587890625e-05f);
        env[n] = env[n - 1];
        const auto r = (int) std::min (rndi ((double) f0), (long long) n - 1);
        for (int k = 0; k < r; ++k)
            env[k] = env[r];
        for (int k = 0; k < n; ++k)
            fine[k] = divF (a[k], env[k]);
        const auto hi = (float) (n - 1);
        for (int k = 0; k < n; ++k)
        {
            auto p = hasT78 ? t78[(std::size_t) k] * fac + -1.0f : (float) (((double) fac - 1.0) + (double) fac * k);
            p = std::min (std::max (p, 0.0f), hi);
            const auto ki = (int) p;
            const auto fr = p - (float) ki;
            const auto e0 = env[ki], e1 = env[ki + 1];
            outp[k] = (e0 + (e1 - e0) * fr) * fine[k];
        }
        if (fmix == 1.0f)
            std::copy (outp, outp + n, a);
        else
            for (int k = 0; k < n; ++k)
                a[k] = (outp[k] - a[k]) * fmix + a[k];
    }
};

// position state of a record (+0x150 accumulator, +0x158 position, +0x168 smoothed time offset)
struct ImagePos
{
    double acc = 0.0, pos = 0.0, off = 0.0;
    bool ended = false;

    void step (const Image& im, double A27, double A28) noexcept
    {
        auto diff = (float) (off - im.toff);
        if (diff != 0.0f)
        {
            if (std::fabs (diff) <= 0.5f || im.lm < 1)
                off = std::fabs (diff) > im.smThr ? im.toff : im.toff + (double) (diff * im.smCoef);
            else
            {
                off = off - (diff > 0.0f ? 1.0 : -1.0);
                diff = (float) (off - im.toff);
                if (std::fabs (diff) > im.smThr)
                    off = im.toff;
                else
                {
                    const auto v = im.toff + (double) (diff * im.smCoef);
                    off = v - std::trunc (v); // Delphi Frac
                }
            }
        }
        const auto d = (off + A27 * 2.0) * (double) im.cols + acc;
        ended = im.lm == 0 && im.s68 <= d;
        double p = d;
        if (im.lm == 0)
            p = d < 0.0 ? 0.0 : std::min (d, (double) im.cols);
        else if (im.lm == 1)
        {
            if (d >= 0.0)
            {
                if (d > im.s60)
                {
                    if (im.k5c < 1)
                    {
                        const auto x = d - im.s60;
                        p = im.s60 + (x - std::trunc (x / im.s70) * im.s70);
                    }
                    else
                    {
                        const auto e = im.s60 + im.s70;
                        if (e < d)
                            p = e + ((d - e) - std::trunc ((d - e) / im.s70) * im.s70);
                    }
                }
            }
            else
            {
                p = 0.0;
                if (im.k5c > 1)
                    p = d - std::floor (d / im.s70) * im.s70;
            }
        }
        else
        {
            if (d >= 0.0)
            {
                if (d > im.s88)
                {
                    const auto x = d - im.s60;
                    p = im.s60 + (x - std::trunc (x / im.s70) * im.s70);
                    if (p > im.s88)
                        p = im.s88 * 2.0 - p;
                }
            }
            else
            {
                p = 0.0;
                if (im.k5c > 1)
                    p = d - std::floor (d / im.s70) * im.s70;
            }
        }
        pos = p;
        acc = acc + im.speed * A28;
    }
};

struct Part
{
    int k = 0;
    double fs = 44100.0;
    int B = 88, prec = 0;
    std::array<std::int32_t, 386> p {};
    bool enabled = false;
    int blend = 0, glideV = 0, submode = 0, pmode = 1, clipMode = 0, phCode = 200, prismMul = 0, prismLvl = 0;
    std::array<int, 12> order {};
    std::vector<Art> arts;     // 43
    double d = 1.0;
    std::array<double, bankSize> ratio {};
    std::array<float, bankSize> T1 {}, T2 {}, leq {};
    std::array<double, bankSize> phtab {};
    Table geqT;
    double l2fmax = 0, k19c = 0, fmax = 0;
    double main = 1, gain = 0.5, mainLfo = 0, modx = 0, mody = 0, modz = 0, glide = 0, glideHz = 0, mainpitch = 0, pitch = 0;
    int abmix = 0;
    double phase0 = 0, phrand = 0, gain12 = 1, tmix = 0, fx = 0, mix12 = 0, pitchamt = 0;
    std::array<float, 4> subg {};
    bool blurOn = false;
    float tbA = 0, tbD = 0, protAmt = 1, lvl248 = 0, blurMix = 0, autog = 0;
    std::array<std::array<float, 2>, 2> blurShape {};
    bool hasBlurMask = false, hasHmMask = false;
    std::array<float, bankSize> blurMask {}, hmMask {};
    int hrSt = 0, hrCnt = 0;           // harmonic randomness table (part +0xa0/+0xa8/+0xac)
    std::array<float, bankSize> hrTab {};
    std::array<float, bankSize> protTab {}, clipTab {};
    int protEnd = 0, protSt = 0, clipSt = 0, clipCnt = 0;
    int clipC = 0; // 0: per-partial table, 1: none, 2: constant
    float clipConst = 0.0f;
    bool hasUniTab = false;
    std::array<float, bankSize> uniTab {};
    std::array<Filt, 2> filt;
    int nuni = 1, unitype = 0;
    double uniGain = 1, uniPan = 0, uniS = 0, uniPh = 0;
    bool uniInv = false;
    std::array<double, 9> uniD {};
    double portaT = 0;
    bool portaVar = false;
    int portaLim = 0;
    double tremD = 0, tremRate = 0, tremGap = 0, vibDepth = 0, vibRate = 0;
    std::array<Module, 12> chain {};
    int chainLen = 0, firstF = 0;
    bool b69 = false;
    double hzS = 0, hzW = 0;
    bool phOn = false;
    std::vector<float> phTab;
    Table phMask;
    double phF18 = 1024, phF1c = 1024, phW = 0, phRate = 0;
    double prismK = 0, prismConst = -1;
    std::array<double, bankSize> prismTab {};
    std::array<float, bankSize> pkTab {}, pkTabDll {}, T2raw {};
    double pkBase = 0;
    std::array<double, 9> hzDist {};
    int maxMapRandArt = -1;
    bool anyEnvGlobal = false;
    double part30 = 0.0;                 // part +0x30: the part's pitch knob in cents (formant reference)
    int misc47 = 0;                      // part +0x238 (combo +0x47): auto gain measured after the image
    float xyzC = 0.0f;                   // X/Y/Z smoothing coefficient (plugin +0x1118)
    std::shared_ptr<const Image> img;    // image synthesis (null: no usable image)
    std::vector<std::string> fallbacks;

    // FUN_00888ed0: curve sampled per partial position (index 4+i at log2(i+1)·length/9), [1] = [4], [2]/[3] at the
    // sub positions, [0] = 1; a constant curve fills every entry with its value
    std::array<float, bankSize> harmCurve (const CurveData& cd, bool& cst, double& y0) const
    {
        std::array<float, bankSize> v {};
        const Curve c (cd.pts);
        y0 = (double) (float) c.y[0];
        cst = c.constant();
        if (cst)
            v.fill ((float) c.y[0]);
        else
        {
            const auto xs = harmonicPositions (c.length);
            c.renderAt (xs.data(), 516, v.data() + 4);
            v[1] = v[4];
            const auto L = c.length / 9.0;
            const double sub0 = submode == 0 ? 0.5 : 0.0, sub1 = submode == 0 ? 1.5 : 0.0;
            v[2] = (float) c.valueAt (std::log2 (sub0 + 1.0) * L);
            v[3] = (float) c.valueAt (std::log2 (sub1 + 1.0) * L);
        }
        v[0] = 1.0f;
        return v;
    }

    // FUN_00874640: [start, end) after removing leading/trailing runs equal to val (bitwise), rounded to 8
    static void trim8 (const std::array<float, bankSize>& v, float val, int& st, int& e)
    {
        const int N = bankSize;
        int k2 = 0;
        while (k2 < N && std::memcmp (&v[(std::size_t) k2], &val, 4) == 0)
            ++k2;
        st = (std::max (k2 - 7, 0) + 7) & ~7;
        e = N;
        while (e > st && std::memcmp (&v[(std::size_t) e - 1], &val, 4) == 0)
            --e;
        e = std::min ((e + 7) & ~7, N);
    }

    void build (const PartData& pd, const StateData& S, int index, double rate, int block, double bpm, int precision)
    {
        k = index, fs = rate, B = block, prec = precision;
        p = pd.p;
        enabled = pd.misc[0x44] != 0;
        blend = pd.I (0x00);
        glideV = pd.I (0x08);
        submode = pd.I (0x18);
        pmode = pd.I (0x1c);
        order = pd.order;
        const auto& shp = pd.shp;
        const auto envRate = 2.0 * B / fs;
        const auto tempoRate = B * (1.0 / fs) * bpm * 0.016666666666666666;
        arts.resize (43);
        for (int i = 0; i < 43; ++i)
        {
            arts[(std::size_t) i].build (i, pd.arts[(std::size_t) i], envRate, tempoRate);
            const auto v = p[(std::size_t) (343 + i)];
            auto& a = arts[(std::size_t) i];
            if (v < 1001)
            {
                const auto Nn = (fs * (std::exp (v * 0.009 * ln2) - 1.0)) / (B * 51.1);
                a.smooth = Nn > 0.0 ? f32 (std::exp (ln001 / Nn)) : 0.0;
            }
            else
                a.smooth = 1.0;
            if (a.hasMapRand)
                maxMapRandArt = std::max (maxMapRandArt, i);
            anyEnvGlobal = anyEnvGlobal || a.envGlobal;
        }
        const auto p43 = p[43] != 0 ? p[43] : 1;
        d = p[42] / (p43 * 10000.0);
        if (! (d > 0.0))
            d = 1.0;
        ratio.fill (0.0);
        if (pmode >= 1)
            for (int i = 0; i < 516; ++i)
                ratio[(std::size_t) (4 + i)] = 1.0 + (double) i * d;
        else
        {
            const auto qd = std::pow (2.0, d * 0.125);
            double v = 1.0;
            for (int i = 0; i < 516; ++i)
            {
                ratio[(std::size_t) (4 + i)] = v;
                v = v * qd;
            }
        }
        if (submode == 0)
            ratio[2] = d * 0.5 + 1.0, ratio[3] = d * 1.5 + 1.0;
        else
            ratio[2] = 0.25, ratio[3] = 0.125;
        ratio[1] = 0.5, ratio[0] = 0.0;
        buildTimbre (pd);
        auto geq = globalEq (Curve (shp[17].pts), pd.Fl (0x30));
        geqT = std::move (geq.table);
        const auto neglog2d = f32 (-(std::log (d) * 1.4426950408889634));
        if (pmode == 0)
        {
            l2fmax = f32 (std::log (geq.fmax) * 1.4426950408889634);
            k19c = f32 (std::exp (neglog2d * ln2) * 8.0);
        }
        fmax = f32 (geq.fmax * std::exp ((double) (0.0625f + (float) neglog2d) * ln2));
        const auto& g = S.g;
        main = g3 (g[11] * 0.001);
        abmix = g[10];
        mainLfo = f32 (g[15] * milli);
        modx = f32 (g[0] * 0.001953125), mody = f32 (g[1] * 0.001953125), modz = f32 (g[2] * 0.001953125);
        // X/Y/Z smoothed per control block (FUN_008826b0), 100 ms; off when global misc +0x39 is 0
        xyzC = S.gmisc[0x39] != 0 ? (float) std::exp (-4.6051701859880918 / ((fs * 0.1) / B)) : 0.0f;
        misc47 = pd.I (0x47);
        {
            const auto x = f32 (std::abs (glideV) * milli);
            const auto gv = x == 1.0 ? 1.0 : f32 ((std::exp (x * f32 (std::log (7.0))) - 1.0) * f32 (1.0 / 6.0));
            const auto N = std::max (1.0, f32 (gv * 25.0) * 0.001 * fs);
            glide = f32 (std::exp (ln001 / N));
            glideHz = glideV < 1 ? 0.0 : f32 (ln001 / (N * std::exp (0.057762265046662105 * 3.0) * 308.0));
        }
        mainpitch = (double) g[14];
        const auto p41 = p[41] != 0 ? p[41] : 1;
        const auto pr = p[40] > 0 ? p[40] / (p41 * 10000.0) : 1.0;
        pitch = f32 (mainpitch + (std::log (pr) * 1.4426950408889634 * 1200.0 - 1200.0));
        part30 = std::log (pr) * 1.4426950408889634 * 1200.0 - 1200.0;
        phase0 = f32 (p[0] * milli);
        {
            const auto x1 = f32 (p[1] * milli);
            phrand = f32 (f32 (x1 * x1) * sgn (p[1]));
        }
        {
            const auto a = (float) f32 (p[62] * 0.0009765625);
            subg = { a * a, (float) f32 (p[5] * f32 (1.0 / 1024.0)), (float) f32 (p[6] * f32 (1.0 / 1024.0)),
                     (float) f32 (p[7] * f32 (1.0 / 1024.0)) };
        }
        gain12 = g3 (p[12] * 0.001);
        tmix = f32 (p[4] * milli);
        fx = f32 (p[10] * milli);
        mix12 = f32 (p[74] * milli);
        pitchamt = f32 (p[44] * 2.0);
        blurOn = pd.misc[0x46] != 0;
        auto tbcoef = [&] (int v) {
            const auto Nn = ((std::exp ((double) ((float) v * 0.01f) * ln2) - 1.0) * 0.001 * fs) / B;
            return ! (Nn > 0.0) ? 0.0f : (float) std::exp (-4.6051701859880918 / Nn);
        };
        tbA = tbcoef (p[21]), tbD = tbcoef (p[22]);
        {
            const auto x8 = (float) p[8] * 0.0009765625f;
            protAmt = x8 == 1.0f ? 1.0f : (float) ((std::exp ((double) (x8 * 2.39789534f)) - 1.0) * (double) 0.1f);
        }
        lvl248 = (float) p[9] * 0.0009765625f;
        blurMix = (float) ((double) ((long long) p[20] * p[20]) * 1e-06);
        for (int side = 0; side < 2; ++side)
        {
            const auto q = p[(std::size_t) (24 + side)];
            if (q < 1)
                blurShape[(std::size_t) side] = { 0.0f, 0.0f };
            else
            {
                const auto e = std::exp ((double) ((float) q * 0.001f) * std::log (1000001.0)) - 1.0;
                blurShape[(std::size_t) side] = { (float) std::log ((double) (float) e + 1.0), (float) (1.0 / (double) (float) e) };
            }
        }
        {
            // blur mask (FUN_0088a1f0): curve shp 24 sampled per partial index, squared; all-1.0 → no mask
            const Curve c (shp[24].pts);
            std::array<float, 516> mv {};
            if (c.constant())
                mv.fill ((float) c.y[0]);
            else
            {
                const auto xs = harmonicPositions (c.length);
                c.renderAt (xs.data(), 516, mv.data());
            }
            hasBlurMask = std::any_of (mv.begin(), mv.end(), [] (float v) { return v != 1.0f; });
            blurMask.fill (0.0f);
            for (int i = 0; i < 516; ++i)
                blurMask[(std::size_t) i] = mv[(std::size_t) i] != 1.0f ? mv[(std::size_t) i] * mv[(std::size_t) i] : mv[(std::size_t) i];
        }
        clipMode = pd.I (0x14);
        autog = (((float) p[13] * 0.001f) * (float) p[13]) * 0.001f;
        buildClipProt (pd);
        {
            const Curve c (shp[8].pts);
            bool flat = true;
            for (auto y : c.y)
                flat = flat && (float) y == 0.5f;
            hasUniTab = ! flat;
            if (hasUniTab)
            {
                bool cst = false;
                double y0 = 0.0;
                auto v = harmCurve (shp[8], cst, y0);
                for (std::size_t i = 0; i < bankSize; ++i)
                {
                    const auto w2 = v[i] * 2.0f;
                    uniTab[i] = w2 * w2;
                }
            }
        }
        for (int f = 0; f < 2; ++f)
            buildFilter (pd, f);
        nuni = std::min (std::max (1, p[34]), 9);
        unitype = p[35];
        uniGain = nuni < 2 ? 1.0
                           : f32 (1.0 - (double) ((float) f32 (std::exp (f32 (1.0 - f32 (1.0 / nuni)) * std::log (3.0)) - 1.0) * 0.5f));
        uniPh = f32 (p[39] * milli);
        uniInv = p[36] > 0;
        uniPan = p[37] < 0xc1 ? f32 (p[37] * 0.005208333333333333) : f32 ((p[37] - 0xc0) * 0.046875 + 1.0);
        uniS = f32 (f32 (std::exp ((double) ((float) p[38] * 0.00390625f) * std::log (41.0)) - 1.0) * 1.875);
        uniTable();
        {
            double pt = 0.0;
            if (p[49] < 0)
                pt = (fs * (std::exp (p[49] * -0.015625 * ln2) - 1.0)) / (B * 42.0);
            else
                pt = ((fs * p[49]) * 0.3125) / (bpm * B);
            portaT = p[47] != 0 ? f32 (pt) : f32 (-pt);
            portaVar = p[48] != 0;
            portaLim = p[50];
        }
        tremD = f32 (p[17] * milli);
        tremRate = f32 (std::exp ((6.0 - p[18] * 0.001) * ln2) * (1.0 / fs));
        {
            auto g19 = p[19] * 0.001;
            if (std::fabs (g19) >= 1.0)
                g19 = g19 - std::trunc (g19);
            tremGap = f32 (f32 (g19) * pi);
        }
        vibDepth = f32 (p[45] * 0.1);
        vibRate = f32 (std::exp ((3.0 - f32 (p[46] * milli)) * ln2) * (1.0 / fs));
        {
            // harmonic randomness (FUN_008895a0): curve 0x2f rendered like a timbre, centre 0.5, trimmed on 0, × 8 ln 2
            const Curve c (shp[4].pts, 0.5);
            const double sub0 = submode == 0 ? 0.5 : 0.0, sub1 = submode == 0 ? 1.5 : 0.0;
            std::array<float, bankSize> hv {};
            c.render (516, 0.0, hv.data() + 4);
            hv[2] = sub0 != 0.0 ? c.render (2, sub0 / 515.0 * c.length)[1] : hv[4];
            hv[3] = sub1 != 0.0 ? c.render (2, sub1 / 515.0 * c.length)[1] : hv[4];
            hv[1] = hv[4], hv[0] = 0.0f;
            int st = 0, e = 0;
            trim8 (hv, 0.0f, st, e);
            hrSt = st, hrCnt = std::max (e - st, 0);
            const auto k8 = (float) (ln2 * 8.0);
            for (int i = 0; i < hrCnt; ++i)
                hv[(std::size_t) i] = hv[(std::size_t) i] * k8;
            hrTab = hv;
        }
        // module chain (FUN_008737e0)
        chainLen = 0;
        for (auto c : order)
        {
            if (c == 0 && order[0] != 0)
                chain[(std::size_t) chainLen++] = mLeq;
            else if (c == 1)
                chain[(std::size_t) chainLen++] = mPrism;
            else if (c == 2 && hrCnt > 0)
                chain[(std::size_t) chainLen++] = mHrand;
            else if (c == 3)
                chain[(std::size_t) chainLen++] = mHarmonizer;
            else if (c == 4 || c == 5)
                chain[(std::size_t) chainLen++] = mFilter;
            else if (c == 6)
                chain[(std::size_t) chainLen++] = mPhaser;
            else if (c == 7 && pd.misc[0x46] == 0)
                chain[(std::size_t) chainLen++] = mPluck;
            else if (c == 8)
                chain[(std::size_t) chainLen++] = mBlur;
            else if (c == 9 && protEnd - protSt > 0)
                chain[(std::size_t) chainLen++] = mProt;
            else if (c == 10 && clipCnt > 0)
                chain[(std::size_t) chainLen++] = mClip;
            else if (c == 11)
                chain[(std::size_t) chainLen++] = mVolume;
        }
        if (chainLen > 0 && chain[(std::size_t) chainLen - 1] == mVolume)
            --chainLen;
        const auto i4 = std::find (order.begin(), order.end(), 4) - order.begin();
        const auto i5 = std::find (order.begin(), order.end(), 5) - order.begin();
        firstF = i4 < i5 ? 0 : 1;
        b69 = false;
        for (auto c : order)
        {
            if (c == 0 && order[0] != 0)
                b69 = true;
            if (c == 3)
                b69 = false;
        }
        hzS = f32 (std::exp ((double) ((float) p[29] * 0.00390625f) * ln2) * 1.3310003559094952);
        hzW = f32 (p[28] * milli);
        phCode = pd.I (0x28);
        if (p[76] > 0)
        {
            phOn = true;
            if (phCode == 209 || phCode == 210)
            {
                // custom phaser curve (shp 15): 209 "frequency" bipolar ×2, 210 squared and RMS-normalised
                if (phCode == 209)
                {
                    phTab = Curve (shp[15].pts, 0.5).render (1024);
                    for (auto& v : phTab)
                        v = v * 2.0f;
                }
                else
                {
                    phTab = Curve (shp[15].pts).render (1024);
                    std::vector<double> sq (1024);
                    for (std::size_t i = 0; i < 1024; ++i)
                    {
                        phTab[i] = phTab[i] * phTab[i];
                        sq[i] = (double) phTab[i] * (double) phTab[i];
                    }
                    const auto rms = std::sqrt (pairwiseSum (sq.data(), sq.size()) / 1024.0);
                    if (rms > 0.0)
                    {
                        const auto kk = (float) (1.0 / rms);
                        for (auto& v : phTab)
                            v = v * kk;
                    }
                }
                phF18 = phF1c = 1024.0;
            }
            else if (phaserInfo (phCode).n > 0)
            {
                phTab = phaserTable (phCode);
                phF18 = phaserInfo (phCode).f18, phF1c = phaserInfo (phCode).f1c;
            }
            else
            {
                phOn = false;
                fallbacks.push_back ("phaser type " + std::to_string (phCode) + " (off)");
            }
            phMask = maskTable (Curve (shp[22].pts)); // (part +0x3b0, flag +0x3e4)
        }
        phW = f32 (p[78] * 0.0001);
        {
            const auto tpo = (double) ((float) bpm / (float) (fs * 3840.0));
            phRate = sgn (p[80]) * std::exp ((double) ((float) std::abs (p[80]) * 0.001f) * ln2) * tpo;
        }
        prismK = f32 (p[26] * milli);
        prismMul = pd.I (0x0c);
        prismLvl = pd.I (0x10);
        {
            const Curve c (shp[7].pts, 0.5);
            const auto sub0 = submode == 0 ? 0.5 : 0.0;
            prismTab.fill (0.0);
            if (c.constant())
            {
                prismConst = f32 (f32 (c.y[0] - 0.5) * 2.0);
                prismTab.fill ((double) (float) c.y[0] - 0.5);
            }
            else
            {
                prismConst = -1.0;
                const auto xs = harmonicPositions (c.length);
                std::array<float, 516> v {};
                c.renderAt (xs.data(), 516, v.data());
                for (int i = 0; i < 516; ++i)
                    prismTab[(std::size_t) (4 + i)] = v[(std::size_t) i];
                prismTab[1] = prismTab[4];
                const auto L = c.length / 9.0;
                prismTab[2] = c.valueAt (std::log2 (sub0 + 1.0) * L);
                prismTab[3] = c.valueAt (std::log2 (sub0 + 1.0) * L); // FUN_008890d0 reads the Sub 3 position for both
            }
            prismTab[0] = 0.0;
            for (auto& v : prismTab)
                v = v * 2.0;
            if (prismMul > 0 && prismLvl == 0)
                for (auto& v : prismTab)
                    v = v + 1.0;
        }
        {
            const Curve c (shp[14].pts);
            std::array<float, bankSize> pv {};
            if (c.constant())
                pv.fill ((float) c.y[0]);
            else
            {
                const auto xs = harmonicPositions (c.length);
                c.renderAt (xs.data(), 516, pv.data() + 4);
                pv[1] = pv[4];
                const auto L = c.length / 9.0;
                const double sub0 = submode == 0 ? 0.5 : 0.0, sub1 = submode == 0 ? 1.5 : 0.0;
                pv[2] = (float) c.valueAt (std::log2 (sub0 + 1.0) * L);
                pv[3] = (float) c.valueAt (std::log2 (sub1 + 1.0) * L);
            }
            pv[0] = 1.0f;
            pkTab[0] = 1.0f;
            for (int i = 1; i < bankSize; ++i)
                pkTab[(std::size_t) i] = (float) (1.0 / std::exp (f32 (f32 (pv[(std::size_t) i] - 1.0f) * 16.0) * ln2));
            pkBase = ln001 * B / fs;
            // the DLL's own table (part +0x258) holds the reciprocal 2^((v − 1)·16); the pluck blend (mode 5) multiplies
            // timbre 2 by it (at least 2^-16, FUN_0088f310)
            pkTabDll[0] = 1.0f;
            for (int i = 1; i < bankSize; ++i)
                pkTabDll[(std::size_t) i] = (float) std::exp (f32 (f32 (pv[(std::size_t) i] - 1.0f) * 16.0) * ln2);
            if (blend == 5)
            {
                const auto lo = (float) std::exp (ln2 * -16.0);
                for (std::size_t i = 0; i < bankSize; ++i)
                    T2[i] = std::max (T2raw[i] * pkTabDll[i], lo);
            }
        }
        {
            const Curve dc (shp[16].pts);
            const auto dv = dc.render (9, dc.length / 8.0);
            for (int i = 0; i < 9; ++i)
            {
                const auto w2 = dv[(std::size_t) i] * 2.0f;
                hzDist[(std::size_t) i] = w2 == 1.0f ? 1.0 : (double) ((float) (std::exp ((double) w2 * 1.0986123085021973) - 1.0) * 0.5f);
            }
        }
        {
            // harmonizer local mask (FUN_0088a0c0, part +0x610): curve per partial, squared where not 1; all 1 → none
            const Curve c (shp[23].pts);
            std::array<float, 516> mv {};
            if (c.constant())
                mv.fill ((float) c.y[0]);
            else
            {
                const auto xs = harmonicPositions (c.length);
                c.renderAt (xs.data(), 516, mv.data());
            }
            hasHmMask = std::any_of (mv.begin(), mv.end(), [] (float x) { return x != 1.0f; });
            hmMask.fill (0.0f);
            for (int i = 0; i < 516; ++i)
                hmMask[(std::size_t) i] = mv[(std::size_t) i] != 1.0f ? mv[(std::size_t) i] * mv[(std::size_t) i] : mv[(std::size_t) i];
        }
        if (blend > 5)
            fallbacks.push_back ("timbre blend mode " + std::to_string (blend) + " (played as mode 0)");
        if (pd.tail >= 4 + 0x2c) // (any image object: Labyrinth's 16 × 12 one is 829 bytes)
            buildImage (pd);
    }

    // the image object (imgm.Image): planes, loop points, normalisation, gain/frequency scales, masks, pitch track
    void buildImage (const PartData& pd)
    {
        const auto d = parseImage (pd.tailBytes);
        if (! d.usable)
        {
            if (! d.why.empty())
                fallbacks.push_back (d.why);
            return;
        }
        auto im = std::make_shared<Image>();
        im->cols = d.cols, im->rows = d.rows, im->stride = d.stride;
        im->gain = d.gain, im->freq = d.freq;
        im->hasGain = ! d.gain.empty(), im->hasFreq = ! d.freq.empty();
        std::string why;
        if (! im->loopSetup (d, why))
        {
            fallbacks.push_back (why);
            return;
        }
        // normalisation (FUN_00795720): 1 / sqrt(max over columns of Σ (amp_i/(i + 1))²), at least 2^-12
        if (im->hasGain)
        {
            std::vector<float> a ((std::size_t) im->stride);
            std::vector<double> a2 ((std::size_t) im->rows);
            float best = 0.0f;
            for (int c = 0; c < im->cols; ++c)
            {
                im->pix2amp (im->gain.data() + (std::size_t) c * (std::size_t) im->stride, a.data());
                for (int i = 0; i < im->rows; ++i)
                {
                    const auto w = (float) (1.0 / (i + 1));
                    const auto x = a[(std::size_t) i] * w;
                    a2[(std::size_t) i] = (double) (x * x);
                }
                const auto sm = (float) pairwiseSum (a2.data(), a2.size());
                if (sm > best)
                    best = sm;
            }
            auto x = (float) std::sqrt ((double) best);
            if (x <= 0.000244140625f)
                x = 0.000244140625f;
            im->k38 = (float) (1.0 / (double) x);
        }
        im->setGainScale (std::exp (p[88] * 0.016 * ln2) - 1.0);
        im->toff = p[83] * 9.5367431640625e-07;
        {
            const auto v = (float) std::abs (p[82]) * 0.001f;
            const auto d16 = (double) v * (double) v;
            const auto d13 = (d16 * 2.0 * fs) / B;
            im->smCoef = d13 > 0.0 ? (float) std::exp (-4.6051701859880918 / d13) : 0.0f;
            if (p[82] == 0)
                im->smThr = 0.0f;
            else if (p[82] < 0)
                im->smThr = 2147483648.0f;
            else
                im->smThr = (float) ((d16 * 250.0) / std::max ((double) im->cols, 5.960464477539063e-08));
        }
        // speed: columns per control block (+0x138 = B / (100 ms · fs) at fine speed 1)
        im->speed = p[84] * 0.0001 * std::exp (p[85] * 0.0001 * ln2) * (B / (100.0 * 0.001 * fs));
        im->interpGain = p[87], im->interpFreq = p[91], im->fmode = p[90];
        im->gmix = (float) p[89] * 0.001f;
        im->fmix = (float) p[94] * 0.001f;
        {
            auto sc = (double) ((float) p[92] * 0.001f);
            if (im->fmode == 0)
                sc = sc * ln2;
            else if (im->fmode == 2)
                sc = (double) std::max (0.0f, (float) ((sc + 2.0) * 32.0));
            else if (im->fmode == 3)
                sc = (sc + 2.0) * 0.125;
            im->k174 = (float) sc;
        }
        const auto& shp = pd.shp;
        {
            // FUN_00889c50: "Image formant mutation" curve per partial → positions (i + 1)·2^(−2v)
            const Curve c (shp[9].pts, 0.5);
            bool flat = true;
            for (auto y : c.y)
                flat = flat && (float) y == 0.5f;
            if (! flat)
            {
                const auto xs = harmonicPositions (c.length);
                std::array<float, bankSize> v {};
                c.renderAt (xs.data(), 516, v.data());
                for (int i = 0; i < bankSize; ++i)
                    im->t78[(std::size_t) i] = (float) ((i + 1) * std::exp ((double) (-v[(std::size_t) i] * 2.0f) * ln2));
                im->hasT78 = true;
            }
        }
        {
            // FUN_0088a320: image freq local mask (curve 25) per partial, trimmed on 1.0
            const Curve c (shp[25].pts);
            std::array<float, bankSize> mv {};
            if (c.constant())
                std::fill (mv.begin(), mv.begin() + 516, (float) c.y[0]);
            else
            {
                const auto xs = harmonicPositions (c.length);
                c.renderAt (xs.data(), 516, mv.data());
            }
            const float one = 1.0f;
            int k2 = 0;
            while (k2 < 516 && std::memcmp (&mv[(std::size_t) k2], &one, 4) == 0)
                ++k2;
            const int st = (std::max (k2 - 7, 0) + 7) & ~7;
            int e = 516;
            while (e > st && std::memcmp (&mv[(std::size_t) e - 1], &one, 4) == 0)
                --e;
            e = std::min ((e + 7) & ~7, 516);
            im->fmask = mv, im->fmaskSt = st, im->fmaskN = e - st;
        }
        if (im->hasFreq && im->hasGain && im->rows >= 16)
            im->pitchTrack();
        img = std::move (im);
    }

    void uniTable()
    {
        const int n = nuni, t = unitype;
        uniD.fill (0.0);
        if (n < 2)
            return;
        const auto invN1 = f32 (1.0 / (n - 1)), invN = f32 (1.0 / n);
        const int tt = std::min (t, 2);
        double acc = 0.0;
        for (int kk = 0; kk < n; ++kk)
        {
            double b = 0.0;
            if (t < 3)
                b = std::fabs ((double) (float) ((float) ((float) (kk * invN1) * 2.0f) - 1.0f));
            else
            {
                b = t == 3 ? (kk + 0.25) / (n - 0.5) : (kk + 0.5) * invN;
                b = std::fabs (b * 2.0 - 1.0);
            }
            const bool flip = (n & 1) == 1 && n < 2 * kk;
            if (((kk & 1) == 1) != flip)
                b = -b;
            if (t < 3)
            {
                uniD[(std::size_t) kk] = tt < 1 ? b + invN1 * unisonDetuneD[0][kk] : b * unisonDetuneD[tt][kk];
                acc += uniD[(std::size_t) kk];
            }
            else
                uniD[(std::size_t) kk] = b;
        }
        if (t < 2)
            for (int kk = 0; kk < n; ++kk)
                uniD[(std::size_t) kk] = uniD[(std::size_t) kk] - acc / n;
    }

    void buildTimbre (const PartData& pd)
    {
        const double sub0 = submode == 0 ? 0.5 : 0.0, sub1 = submode == 0 ? 1.5 : 0.0;
        for (int t = 0; t < 2; ++t)
        {
            const Curve c (pd.shp[(std::size_t) (2 + t)].pts);
            auto& v = t == 0 ? T1 : T2;
            v.fill (0.0f);
            c.render (516, 0.0, v.data() + 4);
            v[0] = 1.0f;
            v[2] = sub0 != 0.0 ? c.render (2, sub0 / 515.0 * c.length)[1] : v[4];
            v[3] = sub1 != 0.0 ? c.render (2, sub1 / 515.0 * c.length)[1] : v[4];
            for (std::size_t i = 1; i < bankSize; ++i)
                v[i] = (v[i] * 4.0f) * (v[i] * 4.0f);
            v[1] = v[4];
        }
        // timbre blending (FUN_0088f310, combo +0x00): timbre 2 combined with the raw timbre 1 before the local EQ
        for (std::size_t i = 0; i < bankSize; ++i)
            switch (blend)
            {
                case 1: T2[i] = std::max (T1[i] - T2[i], 0.0f); break;
                case 2: T2[i] = T2[i] * T1[i]; break;
                case 3: T2[i] = T1[i] > T2[i] ? T1[i] : T2[i]; break;
                case 4: T2[i] = T1[i] < T2[i] ? T1[i] : T2[i]; break;
                default: break;
            }
        T2raw = T2; // (pluck blend: finished once the pluck table exists, without the local EQ)
        leq = localEq (pd);
        if (order[0] == 0)
            for (std::size_t i = 0; i < bankSize; ++i)
                T1[i] = T1[i] * leq[i], T2[i] = blend == 5 ? T2[i] : T2[i] * leq[i];
        const Curve c (pd.shp[1].pts, 0.5);
        std::array<float, 516> ph {};
        c.render (516, 0.0, ph.data());
        phtab.fill (0.0);
        for (int i = 0; i < 516; ++i)
            phtab[(std::size_t) (4 + i)] = ph[(std::size_t) i];
        phtab[1] = phtab[4];
        phtab[2] = sub0 != 0.0 ? (double) c.render (2, sub0 / 515.0 * c.length)[1] : phtab[4];
        phtab[3] = sub1 != 0.0 ? (double) c.render (2, sub1 / 515.0 * c.length)[1] : phtab[4];
        phtab[0] = 0.0;
        for (auto& v : phtab)
            v = v + 1.0;
    }

    std::array<float, bankSize> localEq (const PartData& pd) const
    {
        const Curve c (pd.shp[0].pts);
        std::array<float, bankSize> v {};
        const double sub0 = submode == 0 ? 0.5 : 0.0, sub1 = submode == 0 ? 1.5 : 0.0;
        if (c.constant())
            v.fill ((float) c.y[0]);
        else
        {
            const auto xs = harmonicPositions (c.length);
            c.renderAt (xs.data(), 516, v.data() + 4);
            v[1] = v[4];
            const auto L = c.length / 9.0;
            double x2 = std::log2 (sub0 + 1.0) * L, x3 = std::log2 (sub1 + 1.0) * L;
            float r2 = 0.0f, r3 = 0.0f;
            c.renderAt (&x2, 1, &r2);
            c.renderAt (&x3, 1, &r3);
            v[2] = r2, v[3] = r3;
        }
        v[0] = 1.0f;
        auto out = v;
        if (pmode >= 1)
        {
            for (int i = 0; i < 516; ++i)
            {
                const auto w = v[(std::size_t) (4 + i)] * 4.0f;
                out[(std::size_t) (4 + i)] = (w * w) / (float) (i + 1);
            }
            auto w = v[1] * 4.0f;
            out[1] = w * w;
            w = v[2] * 4.0f;
            out[2] = (w * w) / (float) (sub0 + 1.0);
            w = v[3] * 4.0f;
            out[3] = (w * w) / (float) (sub1 + 1.0);
        }
        else
            for (std::size_t i = 0; i < bankSize; ++i)
                out[i] = v[i] * v[i];
        out[0] = 1.0f;
        return out;
    }

    void buildClipProt (const PartData& pd)
    {
        bool cst = false;
        double y0 = 0.0;
        // protection table (FUN_00889470): curve shp 5, trimmed on 0.0, squared from index max(start, 1)
        auto v = harmCurve (pd.shp[5], cst, y0);
        int st = 0, e = 0;
        trim8 (v, 0.0f, st, e);
        protTab = v;
        for (int i = std::max (st, 1); i < e; ++i)
            protTab[(std::size_t) i] = protTab[(std::size_t) i] * protTab[(std::size_t) i];
        protEnd = e, protSt = st;
        // clip table (FUN_00889750): curve shp 6, trimmed on float(mode == 0)
        const auto m = clipMode;
        if (m < 0 || m > 4)
        {
            clipCnt = 0;
            fallbacks.push_back ("clip mode " + std::to_string (m) + " (off)");
            return;
        }
        const float fac = std::array<float, 5> { 4.0f, 128.0f, 32.0f, 2.0f, 2.0f }[(std::size_t) m];
        const bool recip = m == 1 || m == 2;
        v = harmCurve (pd.shp[6], cst, y0);
        trim8 (v, m == 0 ? 1.0f : 0.0f, st, e);
        clipTab = v;
        for (int i = st; i < e; ++i)
        {
            auto& t = clipTab[(std::size_t) i];
            if (m == 0 && t == 1.0f)
            {
                t = 3.4e38f;
                continue;
            }
            auto dd = (double) (t * 2.0f);
            dd = dd * dd * (double) fac;
            if (recip)
            {
                if (dd <= 5.960464477539063e-08)
                    dd = 5.960464477539063e-08;
                t = (float) (1.0 / dd);
            }
            else
                t = (float) dd;
        }
        clipSt = st, clipCnt = e - st;
        if (! cst)
            clipC = 0;
        else if (m == 0 && y0 == 1.0)
            clipC = 1;
        else
        {
            const auto dd = (double) ((float) y0 * 2.0f);
            clipC = 2;
            clipConst = (float) (dd * dd * (double) fac);
        }
    }

    void buildFilter (const PartData& pd, int f)
    {
        auto& fl = filt[(std::size_t) f];
        const int o = 12 * f;
        fl.code = pd.I (0x34 + 4 * f);
        fl.rcode = pd.I (0x3c + 4 * f);
        std::optional<Curve> cur, rc;
        if (fl.code == 17 || fl.code == 18)
            cur.emplace (pd.shp[(std::size_t) (10 + fl.code - 17)].pts);
        if (auto t = shapeTable (fl.code, cur ? &*cur : nullptr))
            fl.tab = std::move (*t);
        else
        {
            fl.tab = Table();
            fl.tab.t = { 1.0f };
            fl.tab.flag = false;
            fallbacks.push_back ("filter shape " + std::to_string (fl.code) + " (off)");
        }
        if (fl.rcode == 110 || fl.rcode == 111)
            rc.emplace (pd.shp[(std::size_t) (12 + fl.rcode - 110)].pts, 0.5);
        fl.noise = fl.rcode == 109;
        fl.hasRtab = false;
        if (! fl.noise)
        {
            if (auto t = shapeTable (fl.rcode, rc ? &*rc : nullptr))
                fl.rtab = std::move (*t), fl.hasRtab = true;
            else
                fallbacks.push_back ("resonance shape " + std::to_string (fl.rcode) + " (off)");
        }
        fl.mask = maskTable (Curve (pd.shp[(std::size_t) (18 + f)].pts));
        fl.rmask = maskTable (Curve (pd.shp[(std::size_t) (20 + f)].pts));
        fl.adaptEnv = p[(std::size_t) (51 + o)] > 0;
        fl.hz = p[(std::size_t) (53 + o)] != 0;
        fl.w = f32 (p[(std::size_t) (54 + o)] * 0.004);
        const auto W = f32 (0.6666666666666666 / std::exp ((1.0 - fl.w) * ln2));
        const auto a = f32 (W * fl.tab.eL), b = f32 (W * fl.tab.eR);
        const double lo0 = 6.19802618, hi0 = 14.1146927;
        fl.lo = f32 (lo0 - f32 (a - sgn (a) * std::min (a, 1.0)));
        fl.hi = f32 (hi0 + f32 (b - sgn (b) * std::min (b, 1.0)));
        fl.span = f32 (fl.hi - fl.lo);
        fl.span2 = f32 (fl.span * 2.0);
        fl.knob = f32 (fl.lo + f32 (f32 (p[(std::size_t) (52 + o)] * 0.0001220703125) * fl.span));
        fl.kt = f32 (p[(std::size_t) (55 + o)] * milli);
        fl.envamt = f32 (p[(std::size_t) (56 + o)] * milli);
        fl.r = p[(std::size_t) (57 + o)];
        fl.rhz = p[(std::size_t) (58 + o)] != 0;
        fl.rw = f32 (p[(std::size_t) (59 + o)] * 0.004);
        fl.rofsOct = f32 (p[(std::size_t) (60 + o)] * 0.0008333333333333334);
        fl.rofsF = f32 (std::exp (fl.rofsOct * ln2));
        fl.radapt = p[(std::size_t) (61 + o)] >= 1;
        const auto S = fl.hasRtab ? fl.rtab.S : 1024.0;
        if (! fl.rhz)
            fl.k0 = ! fl.radapt ? S : f32 (S * std::exp (0.057762265046662105 * -27.0) * 440.0);
        else
            fl.k0 = ! fl.radapt ? f32 (S * 0.000375) : f32 (S * 0.000375 * std::exp (0.057762265046662105 * -15.0) * 440.0);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Plugin-level patch: both parts plus the global settings (Gen_Render, master volume, voice control)

struct Patch
{
    double fs = 44100.0, bpm = 120.0, fclamp = 0.0, coef = 0.0, masterTarget = 1.0;
    int B = 88, prec = 0, monoMode = 0, vrmode = 0, maxrk = 0, e8 = 0;
    bool legato = false, porta = false, portaVel = false; // portaVel: global 6, the glide time follows the velocity
    bool arpAny = false, arpDesc = false; // some envelope has arpeggio markers; chord order (global 16: descending)
    double legLim = 2147483647.0;
    float kfade = 0.0f;
    std::array<Part, 2> parts;
    std::array<bool, 2> present {};
    std::array<int, 2> act {};
    int actCount = 0;
    std::vector<float> declick;
    std::vector<std::string> fallbacks;
    std::shared_ptr<harmor::FxHook> rack; // the effects rack (this patch's own instance), null when every unit is off
};

// Harmor's effects rack (REA_rodada38) through the engine's FxHook slot
class RackHook final : public harmor::FxHook
{
public:
    explicit RackHook (const hlfx::HarmorRackSettings& s) { rack.prepare (s); }
    bool enabled() const noexcept override { return rack.enabled(); }
    bool quiet() const noexcept override { return rack.quiet(); }
    void process (float* l, float* r, const float* fl, const float* fr, int n, bool voices) noexcept override
    {
        rack.process (l, r, fl, fr, n, voices);
    }
    void advance (int n) noexcept override { rack.advance (n); }

private:
    hlfx::HarmorRack rack;
};

std::shared_ptr<Patch> buildPatch (const StateData& S, double fs, double bpm, const std::vector<std::uint8_t>& compState = {})
{
    auto P = std::make_shared<Patch>();
    P->fs = fs, P->bpm = bpm;
    const auto cr = S.GI (24);
    P->B = std::max (4, (int) rndi (cr * 0.01 * 0.001 * fs * 0.25) * 4);
    P->prec = S.GI (28);
    P->fclamp = f32 (1048576.0 / fs); // plugin +0x10d8 (23.777 at 44.1 kHz; 2^20/fs is an inference)
    for (int k = 0; k < 2; ++k)
        if (S.parts[(std::size_t) k].present)
        {
            P->present[(std::size_t) k] = true;
            P->parts[(std::size_t) k].build (S.parts[(std::size_t) k], S, k, fs, P->B, bpm, P->prec);
        }
    const bool both = P->present[0] && P->present[1] && P->parts[0].enabled && P->parts[1].enabled;
    for (int k = 0; k < 2; ++k)
    {
        if (! P->present[(std::size_t) k])
            continue;
        auto& p = P->parts[(std::size_t) k];
        double mk = 0.5;
        if (both)
        {
            auto m = f32 (p.abmix * milli);
            m = p.k == 0 ? m : f32 (1.0 - m);
            mk = f32 (f32 (std::min (std::max (f32 (std::cos (m * 1.5707963267948966)), 0.0), 1.0)) * 0.5625);
        }
        const auto x = f32 (std::abs (p.p[11]) * milli);
        const auto gp = x == 1.0 ? 1.0 : f32 ((std::exp (x * f32 (std::log (3.0))) - 1.0) * 0.5);
        p.gain = f32 (f32 (f32 (mk * p.main) * gp) * sgn (p.p[11]));
        if (p.enabled)
        {
            P->act[(std::size_t) P->actCount++] = k;
            for (const auto& f : p.fallbacks)
                P->fallbacks.push_back ((k == 0 ? "part A: " : "part B: ") + f);
        }
    }
    int maxrk = -1;
    for (int k = 0; k < 2; ++k)
        if (P->present[(std::size_t) k])
            maxrk = std::max (maxrk, P->parts[(std::size_t) k].maxMapRandArt);
    P->maxrk = std::max (maxrk, 0);
    P->coef = f32 (std::exp (ln001 / (0.015 * fs)));
    {
        const auto x = (double) ((float) S.g[12] * 0.001f);
        P->masterTarget = x == 1.0 ? 1.0 : f32 ((std::exp ((double) ((float) x * 1.09861231f)) - 1.0) * (double) 0.5f);
    }
    const auto nr = std::max (1, (int) rndi (10.0 * 0.001 * fs));
    P->declick.resize ((std::size_t) nr);
    for (int i = 1; i <= nr; ++i)
        P->declick[(std::size_t) i - 1] = (float) ((1.0 + std::cos (f32 (i * f32 (pi / (nr + 1))))) * 0.5);
    P->e8 = (int) rndi (fs * 0.1);
    P->kfade = (float) (1.0 / (P->e8 + 1));
    P->legato = S.g[4] > 0;
    P->porta = S.g[5] > 0;
    P->portaVel = S.g[6] >= 1;
    P->monoMode = S.GI (8);
    P->vrmode = S.GI (12);
    P->legLim = S.g[3] > 0 ? (double) S.g[3] : 2147483647.0;
    P->arpDesc = S.g[16] != 0;
    for (int k = 0; k < 2; ++k)
        if (P->present[(std::size_t) k])
            for (int i = 0; i < 31; ++i)
            {
                const auto& a = P->parts[(std::size_t) k].arts[(std::size_t) i];
                P->arpAny = P->arpAny || (a.hasEnv && a.env.arpOn) || (a.lfoOn && a.lfoEnv.arpOn);
            }
    // the effects rack (REA_rodada38): distortion, chorus, delay, reverb, compressor in the state's order
    {
        hlfx::HarmorRackSettings rs;
        std::copy (S.g.begin(), S.g.end(), rs.g.begin());
        rs.order = { S.order[0], S.order[1], S.order[2], S.order[3], S.order[4], S.order[5] };
        rs.distortion = S.GI (48);
        rs.precision = P->prec;
        rs.fs = fs, rs.bpm = bpm;
        if (S.GI (52) > 0)
        {
            if (! compState.empty())
                rs.comp = hlfx::makeMaximusPreset (hlfx::withMaximusParams (
                    compState, { { 5, S.g[49] }, { 8, S.g[50] }, { 21, S.g[51] }, { 34, S.g[52] } }));
            rs.compMix = S.g[49];
            if (rs.comp == nullptr)
                P->fallbacks.push_back ("compressor preset " + std::to_string (S.GI (52)) + " not available (off)");
        }
        auto hook = std::make_shared<RackHook> (rs);
        if (hook->enabled())
            P->rack = std::move (hook);
    }
    return P;
}

// ---------------------------------------------------------------------------------------------------------------
// The sine-bank kernel: groups of 8 partials kept in registers (4-wide double vectors) over a segment of m samples;
// each partial runs the two-term recurrence x ← c·x − y and adds (T + e)·x, e = (a − T)·k^j kept as a running product
// (Harmor's glide coefficient can differ per partial), into the 4-lane sum of the sample. The same IEEE operations in
// the same order whatever the instruction set, so the AVX build gives bit-identical results.

#if defined(__GNUC__)
#define HARMOR_INLINE inline __attribute__ ((always_inline))
#else
#define HARMOR_INLINE inline
#endif

typedef double HarmorV4 __attribute__ ((vector_size (32)));

// G vectors of 4 partials from partial g on: sines x (recurrence x ← c·x − y), glide term e = (a − T)·k^j (e ← e·k),
// per-sample sums of (T + e)·x into acc[4·j … 4·j + 3]
template <int G>
HARMOR_INLINE void bankKernelGroup (double* osc0, double* osc1, const double* oscC, const float* T, const float* A, const float* K,
                                    int g, int m, double* acc) noexcept
{
    HarmorV4 x[G], y[G], c[G], t[G], e[G], k[G];
    for (int v = 0; v < G; ++v)
        for (int l = 0; l < 4; ++l)
        {
            const auto i = (std::size_t) (g + 4 * v + l);
            x[v][l] = osc0[i], y[v][l] = osc1[i], c[v][l] = oscC[i];
            t[v][l] = (double) T[i];
            e[v][l] = (double) A[i] - (double) T[i];
            k[v][l] = (double) K[i];
        }
    for (int j = 0; j < m; ++j)
    {
        HarmorV4 sum = (t[0] + e[0]) * x[0];
        for (int v = 1; v < G; ++v)
            sum += (t[v] + e[v]) * x[v];
        HarmorV4 a;
        std::memcpy (&a, acc + 4 * j, sizeof a);
        a += sum;
        std::memcpy (acc + 4 * j, &a, sizeof a);
        for (int v = 0; v < G; ++v)
        {
            const auto s = x[v];
            x[v] = c[v] * s - y[v];
            y[v] = s;
            e[v] = e[v] * k[v];
        }
    }
    for (int v = 0; v < G; ++v)
        for (int l = 0; l < 4; ++l)
        {
            const auto i = (std::size_t) (g + 4 * v + l);
            osc0[i] = x[v][l], osc1[i] = y[v][l];
        }
}

HARMOR_INLINE void bankKernelBody (double* osc0, double* osc1, const double* oscC, const float* T, const float* A,
                                   const float* K, int top, int m, double* acc) noexcept
{
    // 8 partials per pass: the 12 vectors of state stay in the 16 AVX registers (16 partials would spill)
    for (int g = 0; g < top; g += 8) // (top is a multiple of 8)
        bankKernelGroup<2> (osc0, osc1, oscC, T, A, K, g, m, acc);
}

void bankKernelPlain (double* osc0, double* osc1, const double* oscC, const float* T, const float* A, const float* K, int top,
                      int m, double* acc) noexcept
{
    bankKernelBody (osc0, osc1, oscC, T, A, K, top, m, acc);
}

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__)) && ! defined(HARMOR_NO_AVX)
#define HARMOR_AVX 1
__attribute__ ((target ("avx"))) void bankKernelAvx (double* osc0, double* osc1, const double* oscC, const float* T,
                                                     const float* A, const float* K, int top, int m, double* acc) noexcept
{
    bankKernelBody (osc0, osc1, oscC, T, A, K, top, m, acc);
}

bool cpuHasAvx() noexcept
{
    unsigned a = 1, b = 0, c = 0, d = 0;
    __asm__ __volatile__ ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d) : "c"(0));
    if ((c & (1u << 27)) == 0 || (c & (1u << 28)) == 0) // OSXSAVE, AVX
        return false;
    unsigned lo = 0, hi = 0;
    __asm__ __volatile__ ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6u) == 6u; // the OS saves the XMM and YMM state
}
#endif

void bankKernel (double* osc0, double* osc1, const double* oscC, const float* T, const float* A, const float* K, int top, int m,
                 double* acc) noexcept
{
#ifdef HARMOR_AVX
    static const bool avx = cpuHasAvx();
    if (avx)
    {
        bankKernelAvx (osc0, osc1, oscC, T, A, K, top, m, acc);
        return;
    }
#endif
    bankKernelPlain (osc0, osc1, oscC, T, A, K, top, m, acc);
}

// harmonic-blur convolution: out[t] = Σ_u buf[t + u]·kr[u] for t < count, summed in u order (4 outputs per pass)
HARMOR_INLINE void blurConvBody (const double* buf, const double* kr, int taps, int count, double* out) noexcept
{
    int t = 0;
    for (; t + 4 <= count; t += 4)
    {
        HarmorV4 acc = { 0.0, 0.0, 0.0, 0.0 };
        for (int u = 0; u < taps; ++u)
        {
            HarmorV4 b;
            std::memcpy (&b, buf + t + u, sizeof b);
            acc += b * kr[u];
        }
        std::memcpy (out + t, &acc, sizeof acc);
    }
    for (; t < count; ++t)
    {
        double acc = 0.0;
        for (int u = 0; u < taps; ++u)
            acc += buf[t + u] * kr[u];
        out[t] = acc;
    }
}

void blurConvPlain (const double* buf, const double* kr, int taps, int count, double* out) noexcept
{
    blurConvBody (buf, kr, taps, count, out);
}

#ifdef HARMOR_AVX
__attribute__ ((target ("avx"))) void blurConvAvx (const double* buf, const double* kr, int taps, int count, double* out) noexcept
{
    blurConvBody (buf, kr, taps, count, out);
}
#endif

void blurConv (const double* buf, const double* kr, int taps, int count, double* out) noexcept
{
#ifdef HARMOR_AVX
    static const bool avx = cpuHasAvx();
    if (avx)
    {
        blurConvAvx (buf, kr, taps, count, out);
        return;
    }
#endif
    blurConvPlain (buf, kr, taps, count, out);
}

// ---------------------------------------------------------------------------------------------------------------
// One voice record (hmodel.Voice): part × unison voice, with its own bank, articulators and module state

struct TopFade
{
    std::array<float, 136> v {};
    TopFade()
    {
        for (int j = 0; j < 66; ++j)
            v[(std::size_t) j] = (float) ((std::cos ((j - 1) * pi / 65.0) + 1.0) * 0.5);
    }
};
const TopFade& topFade()
{
    static const TopFade t;
    return t;
}

struct Record
{
    // bank
    std::array<float, bankSize> a {}, T {}, kg {}, hz {}, l2 {}, tbBuf {}, pkLev {}, pkRate {}, hzu {}, fbuf {};
    std::array<double, bankSize> ph {}, f {}, osc0 {}, osc1 {}, oscC {}, oscS {}, oscF {}, km {};
    std::array<float, bankSize> kmK {};
    std::array<int, bankSize> kmM {};
    std::array<ArtState, 43> arts;
    std::array<float, 260> hbK {};

    bool used = false, firstblk = true, released = false, shared = false, hzdirty = true, freephase = false, forceChanged = false;
    bool centsValid = false, prismValid = false, hbValid = false, tbFresh = true, pluckOn = false, curValid = false;
    bool knValid[2] { false, false }, lsValid[2] { false, false };
    int part = 0, ku = 0, n = 0, note = -1, cnt = 0, firstlen = -1, hbM = 0, pkC = 0, pkIc = 0, dpos = -1, pendOldn = -1;
    int oscTop = 0, oscAge = 0, gcnt = 0, e8 = 0;
    long long stamp = 0, t0 = 0, scount = 0;
    double uPan = 0, uPh = -1, uDet = 0, uGain = 1, uDk = 0;
    double fvol = 1, fpan = 0, fpitch = 0, vel = 1;
    double centsPrev = 0, a4Prev = 0, vibph = 0, vph = 0, dtPrev = 0, phph = 0, phi70 = 0;
    double l2f0 = 0, f0 = 0, f0fs32 = 0, prism = 0, prismV = 0, hbW = 0, pkPrev = -1, pkT = -1;
    double kn[2] {}, Ls[2] {}, fc[2] {}, r1 = 0, fmix = 0, fcarry = 1, artpan = 0, fxv = 0;
    int fsign = 0, fwhich = 0;
    float hbG = 1.0f, volB4 = 1.0f, volB8 = 1.0f, gc = 1.0f, vfac = 1.0f;
    std::array<float, 8> gx {}, gy {};
    std::array<double, 4> cur {};
    std::uint32_t seed148 = 0, s148 = 0, s14c = 0;
    std::array<float, 3> xyz {}; // smoothed X/Y/Z (voice +0x3334)
    std::array<float, bankSize> bpDec {}; // blur-mode pluck decay per partial
    std::array<float, bankSize> hr {};    // harmonic randomness factors (voice +0xd8)
    bool bpValid = false, pbValid = false;
    float bpV = 0.0f;
    double pbMix = 0.0;
    int bpN = 0;
    std::uint32_t nzSeed = 0;    // noise resonance state
    float nzW = -1.0f;
    double nzPos = 0.0, nzLen = 0.0, nzRate = 0.0;
    ImagePos ipos;            // image time position
    bool imgEnd = false;      // a one-shot image ran out (record state 9)
    double v1b0 = 0.0;        // voice +0x1b0: pitch in cents incl. the unison detune (formant reference)
    float agref = 1.6428422f; // auto-gain reference energy
};

struct NoteRec
{
    bool used = false, held = false, pressed = false, inHeads = false, inHeld = false;
    int key = 60, group = 0, nv = 0;
    double vel = 1, fv = 1, fp = 0, fpitch = 0, notePan = 0;
    long long st = 0, seq = 0;
    std::array<int, 18> voices {};
};

using Levels = std::array<float, 6>; // pan, vol, pitch, fcut, fres, velocity of a note

struct PartRun
{
    std::array<std::uint32_t, 43> gl {};
    double phat = 0.0, tremCur = 0.0;
    float vibat = 0.0f, tremPh = 0.0f;
    std::array<float, 512> TL {}, TR {};
    bool tremActive = false;
};

class Engine
{
public:
    static constexpr int maxRecords = 96;  // ≈ 76 KB each; the pool is sized in prepare() (setPoolSize)
    static constexpr int maxNotes = 128;
    static constexpr int maxHostBlock = 512;

    harmor::FxHook* fx = nullptr;

    Engine()
    {
        records.resize (16);
        reset();
    }

    // record pool for this patch (not on the audio thread): 6 notes' worth of records, 16…96
    void setPoolSize (const Patch* p)
    {
        int need = 0;
        if (p != nullptr)
            for (int i = 0; i < p->actCount; ++i)
                need += p->parts[(std::size_t) p->act[(std::size_t) i]].nuni;
        const auto size = std::clamp (need * 6, 16, maxRecords);
        if ((int) records.size() != size)
        {
            records.clear();
            records.shrink_to_fit();
            records.resize ((std::size_t) size);
        }
        reset();
    }

    const Patch* patch() const noexcept { return P; }

    // a new patch (same layout or not): voices stop, the plugin-wide state restarts as at state load
    void setPatch (const Patch* p) noexcept
    {
        P = p;
        fx = p != nullptr ? p->rack.get() : nullptr;
        reset();
    }

    bool rackQuiet() const noexcept { return fx == nullptr || fx->quiet(); }

    void seedRandom (std::uint32_t s) noexcept { mt.seed (s); }
    void setMtState (const std::vector<std::uint32_t>& st) noexcept
    {
        if (st.size() < 625)
            return;
        mt.i = (int) st[0];
        for (int k = 0; k < 624; ++k)
            mt.mt[(std::size_t) k] = st[(std::size_t) k + 1];
    }

    void reset() noexcept
    {
        for (auto& r : records)
            r.used = false;
        const auto pool = (int) records.size();
        freeCount = pool;
        for (int i = 0; i < pool; ++i)
            freeList[(std::size_t) i] = pool - 1 - i;
        liveCount = 0;
        for (auto& nt : notes)
            nt = NoteRec();
        headCount = heldCount = 0;
        hasLast = false;
        gcount = 0;
        stampCounter = 0;
        noteSeq = 0;
        now = 0;
        masterCur = 1.0;
        lastMaps.fill (0.0f);
        if (P == nullptr)
            return;
        for (int k = 0; k < 2; ++k)
        {
            auto& R = run[(std::size_t) k];
            R = PartRun();
            if (! P->present[(std::size_t) k])
                continue;
            for (const auto& a : P->parts[(std::size_t) k].arts)
                if (a.lfoOn && a.lfoGlobal)
                    R.gl[(std::size_t) a.k] = a.gen.start0; // plugin-level phase, initialised before the phase offset is loaded
        }
    }

    bool idle() const noexcept { return liveCount == 0; }
    long long time() const noexcept { return now; }

    // ---- notes (hmodel.render's note-on / note-off handling) ----

    int noteOn (int key, double vel, double fv, double fp, double fpitch, double notePan = 0.0) noexcept
    {
        if (P == nullptr)
            return -1;
        const int slot = allocNote();
        if (slot < 0)
            return -1;
        auto& nt = notes[(std::size_t) slot];
        nt.used = true, nt.pressed = true, nt.held = false, nt.inHeads = false, nt.inHeld = false;
        nt.key = key, nt.vel = vel, nt.fv = fv, nt.fp = fp, nt.fpitch = fpitch, nt.notePan = notePan;
        nt.st = now, nt.seq = noteSeq++, nt.nv = 0, nt.group = 0;
        arpDirty = true;
        if (P->legato)
        {
            int best = -1;
            double bestD = 0.0;
            for (int i = 0; i < headCount; ++i)
            {
                const auto hi = heads[(std::size_t) i];
                const auto& h = notes[(std::size_t) hi];
                if (h.held || h.nv == 0)
                    continue;
                bool ok = true;
                for (int v = 0; v < h.nv; ++v)
                    ok = ok && ! records[(std::size_t) h.voices[(std::size_t) v]].released;
                const auto dist = std::fabs (100.0 * key - 100.0 * h.key);
                if (! ok || ! (dist <= P->legLim))
                    continue;
                if (best < 0 || dist < bestD)
                    best = hi, bestD = dist;
            }
            if (best >= 0)
            {
                auto& h = notes[(std::size_t) best];
                const auto L = levels (h);
                nt.voices = h.voices, nt.nv = h.nv, h.nv = 0;
                nt.group = h.group;
                removeHead (best);
                pushHead (slot);
                h.held = true;
                pushHeld (best);
                for (int v = 0; v < nt.nv; ++v)
                {
                    auto& r = records[(std::size_t) nt.voices[(std::size_t) v]];
                    setNote (r, slot);
                    glideTo (r, L, false);
                }
                last = levels (nt), hasLast = true;
                return slot;
            }
        }
        if (P->monoMode != 0)
        {
            int list[maxNotes];
            const int count = headCount;
            std::copy (heads.begin(), heads.begin() + count, list);
            for (int i = 0; i < count; ++i)
            {
                const auto hi = list[i];
                if (! notes[(std::size_t) hi].used || ! notes[(std::size_t) hi].inHeads)
                    continue;
                if (P->monoMode > 1 || notes[(std::size_t) hi].key == key)
                {
                    if (P->monoMode == 3)
                        releaseNote (hi);
                    else
                        for (int v = 0; v < notes[(std::size_t) hi].nv; ++v)
                        {
                            auto& r = records[(std::size_t) notes[(std::size_t) hi].voices[(std::size_t) v]];
                            if (r.dpos < 0)
                                r.dpos = 0;
                        }
                }
            }
        }
        nt.group = ++gcount;
        // records needed: steal the oldest ones when the pool is short (LYNX's voice limit)
        int need = 0;
        for (int i = 0; i < P->actCount; ++i)
            need += P->parts[(std::size_t) P->act[(std::size_t) i]].nuni;
        while (freeCount < need && liveCount > 0)
            killRecord (live[0]);
        std::uint32_t seed60 = mt.next() >> 1;
        bool bd = true;
        std::uint32_t s108 = 0;
        for (int ai = 0; ai < P->actCount; ++ai)
        {
            const auto pk = P->act[(std::size_t) ai];
            const auto& p = P->parts[(std::size_t) pk];
            int gsearch = -1;
            if (p.anyEnvGlobal)
            {
                long long bestStamp = 0;
                for (int i = 0; i < liveCount; ++i)
                {
                    const auto& w = records[(std::size_t) live[(std::size_t) i]];
                    if (w.part != pk || w.note < 0 || notes[(std::size_t) w.note].held || w.released || w.dpos >= 0)
                        continue;
                    if (gsearch < 0 || w.stamp < bestStamp)
                        gsearch = live[(std::size_t) i], bestStamp = w.stamp;
                }
            }
            std::array<float, 9> udk {};
            const bool hasUdk = p.nuni > 1 && p.unitype == 3;
            if (p.unitype == 3) // (drawn for any unison count: TriggerVoice)
            {
                const auto w = (float) f32 (0.5 / (p.nuni - 0.5));
                seed60 = rand32f (udk.data(), p.nuni, -w, w, seed60);
            }
            for (int ku = 0; ku < p.nuni; ++ku)
            {
                if (bd)
                    seed60 = rand32f (lastMaps.data(), P->maxrk + 1, 0.0f, 1.0f, seed60);
                std::array<float, bankSize> hzu {};
                if (p.nuni > 1 && p.unitype == 4)
                {
                    const auto w = (float) f32 (1.0 / p.nuni);
                    seed60 = rand32f (hzu.data(), 0x208, -w, w, seed60);
                    const auto dk = (float) p.uniD[(std::size_t) ku];
                    for (auto& v : hzu)
                        v = v + dk;
                }
                const auto s14c = mt.next() >> 1;
                if (bd)
                    s108 = s14c;
                bd = P->vrmode == 3;
                std::array<float, bankSize> hrr {};
                if (p.hrCnt > 0) // harmonic randomness draws (after the MT draw)
                    seed60 = rand32f (hrr.data(), p.hrCnt, 0.0f, 1.0f, seed60);
                if (freeCount == 0)
                    continue;
                const auto ri = allocRecord();
                auto& v = records[(std::size_t) ri];
                startRecord (v, pk, ku, nt, gsearch, hasUdk ? udk[(std::size_t) ku] : 0.0f, hzu, s14c, s108);
                v.hr.fill (1.0f);
                for (int i = 0; i < p.hrCnt; ++i)
                    v.hr[(std::size_t) (p.hrSt + i)] = (float) std::exp ((double) (hrr[(std::size_t) i] * p.hrTab[(std::size_t) (p.hrSt + i)]));
                v.note = slot;
                if (nt.nv < (int) nt.voices.size())
                    nt.voices[(std::size_t) nt.nv++] = ri;
                live[(std::size_t) liveCount++] = ri;
            }
            bd = P->vrmode > 1;
        }
        if (P->porta && hasLast)
            for (int v = 0; v < nt.nv; ++v)
                glideTo (records[(std::size_t) nt.voices[(std::size_t) v]], last, true);
        pushHead (slot);
        last = levels (nt), hasLast = true;
        return slot;
    }

    void noteOff (int slot) noexcept
    {
        if (slot < 0 || slot >= maxNotes || ! notes[(std::size_t) slot].used)
            return;
        notes[(std::size_t) slot].pressed = false;
        arpDirty = true;
        releaseNote (slot);
        maybeFree (slot);
    }

    int findPressed (int key) const noexcept
    {
        int best = -1;
        for (int i = 0; i < maxNotes; ++i)
        {
            const auto& nt = notes[(std::size_t) i];
            if (nt.used && nt.pressed && nt.key == key && (best < 0 || nt.seq > notes[(std::size_t) best].seq))
                best = i;
        }
        return best;
    }

    void allNotesOff() noexcept
    {
        for (int i = 0; i < maxNotes; ++i)
            if (notes[(std::size_t) i].used && notes[(std::size_t) i].pressed)
                noteOff (i);
    }

    // FL's channel level changes reach the sounding notes through FinalLevels (chan.Vol, chan.Pan)
    void setChannelLevels (double vol, double pan) noexcept
    {
        for (int i = 0; i < maxNotes; ++i)
        {
            auto& nt = notes[(std::size_t) i];
            if (nt.used)
                nt.fv = vol, nt.fp = nt.notePan + pan;
        }
        for (int i = 0; i < liveCount; ++i)
        {
            auto& r = records[(std::size_t) live[(std::size_t) i]];
            if (r.note >= 0)
                r.fvol = notes[(std::size_t) r.note].fv, r.fpan = notes[(std::size_t) r.note].fp;
        }
    }

    // arpeggio chord (Gen_Render after a note event, plugin +0xe90): the distinct pitches of the records still
    // playing, ascending (descending with global 16 set); every such record learns its place and the chord size
    // (FUN_007a24b0 on the envelopes and LFO envelopes of articulators 0…30)
    void arpChord() noexcept
    {
        int keys[maxNotes];
        int nk = 0;
        for (int i = 0; i < liveCount; ++i)
        {
            const auto& r = records[(std::size_t) live[(std::size_t) i]];
            if (r.released || r.dpos >= 0 || r.note < 0)
                continue;
            const auto key = notes[(std::size_t) r.note].key;
            bool have = false;
            for (int j = 0; j < nk; ++j)
                have = have || keys[j] == key;
            if (! have && nk < maxNotes)
                keys[nk++] = key;
        }
        std::sort (keys, keys + nk);
        if (P->arpDesc)
            std::reverse (keys, keys + nk);
        for (int i = 0; i < liveCount; ++i)
        {
            auto& r = records[(std::size_t) live[(std::size_t) i]];
            if (r.released || r.dpos >= 0 || r.note < 0)
                continue;
            const auto key = notes[(std::size_t) r.note].key;
            const int own = (int) (std::find (keys, keys + nk, key) - keys);
            const auto& p = P->parts[(std::size_t) r.part];
            for (int k = 0; k < 31; ++k)
            {
                const auto& a = p.arts[(std::size_t) k];
                auto& st = r.arts[(std::size_t) k];
                if (a.hasEnv)
                    a.env.setChord (st.es, own, nk);
                if (a.lfoOn)
                    a.lfoEnv.setChord (st.ls, own, nk);
            }
        }
    }

    // ---- one host block of n ≤ 512 samples (no events inside); adds into left/right ----

    void render (float* left, float* right, int n) noexcept
    {
        if (P == nullptr || n <= 0)
            return;
        if (arpDirty)
        {
            arpDirty = false;
            if (P->arpAny)
                arpChord();
        }
        std::fill (mixL.begin(), mixL.begin() + n, 0.0);
        std::fill (mixR.begin(), mixR.begin() + n, 0.0);
        const bool voices = liveCount > 0;
        split = fx != nullptr && fx->enabled();
        if (split)
        {
            std::fill (fxL.begin(), fxL.begin() + n, 0.0);
            std::fill (fxR.begin(), fxR.begin() + n, 0.0);
        }
        // part tremolo (Gen_Render): depth ramp reset when no record of the part sounds
        for (int ai = 0; ai < P->actCount; ++ai)
            tremolo (P->act[(std::size_t) ai], n);
        for (int i = 0; i < liveCount; ++i)
            renderRecord (records[(std::size_t) live[(std::size_t) i]], n);
        // records that finished their declick go back to the pool; ended ones start the declick next block
        int kept = 0;
        for (int i = 0; i < liveCount; ++i)
        {
            const auto ri = live[(std::size_t) i];
            auto& v = records[(std::size_t) ri];
            if (v.dpos >= 0 && v.dpos >= (int) P->declick.size())
            {
                finishRecord (ri);
                continue;
            }
            if (v.dpos < 0 && ended (v))
                v.dpos = 0;
            live[(std::size_t) kept++] = ri;
        }
        liveCount = kept;
        if (split)
        {
            // the effects rack on the float mix (Gen_Render: the voices are mixed into float buffers)
            for (int j = 0; j < n; ++j)
            {
                rackL[(std::size_t) j] = (float) mixL[(std::size_t) j], rackR[(std::size_t) j] = (float) mixR[(std::size_t) j];
                rackFxL[(std::size_t) j] = (float) fxL[(std::size_t) j], rackFxR[(std::size_t) j] = (float) fxR[(std::size_t) j];
            }
            fx->process (rackL.data(), rackR.data(), rackFxL.data(), rackFxR.data(), n, voices);
            for (int j = 0; j < n; ++j)
                mixL[(std::size_t) j] = (double) rackL[(std::size_t) j], mixR[(std::size_t) j] = (double) rackR[(std::size_t) j];
        }
        masterVolume (n);
        for (int j = 0; j < n; ++j)
        {
            left[j] += (float) mixL[(std::size_t) j];
            right[j] += (float) mixR[(std::size_t) j];
        }
        advance (n);
    }

    // nothing sounds: only the plugin-wide phases and the master-volume ramp run
    void skip (int n) noexcept
    {
        if (P == nullptr || n <= 0)
            return;
        for (int ai = 0; ai < P->actCount; ++ai)
            tremolo (P->act[(std::size_t) ai], n, false);
        if (fx != nullptr)
            fx->advance (n);
        rampMaster (n);
        advance (n);
    }

private:
    const Patch* P = nullptr;
    std::vector<Record> records;
    std::array<int, maxRecords> freeList {}, live {};
    int freeCount = 0, liveCount = 0;
    std::array<NoteRec, maxNotes> notes {};
    std::array<int, maxNotes> heads {}, held {};
    int headCount = 0, heldCount = 0, gcount = 0;
    bool arpDirty = false;
    Levels last {};
    bool hasLast = false;
    long long stampCounter = 0, noteSeq = 0, now = 0;
    Mt19937 mt;
    std::array<float, 64> lastMaps {};
    std::array<PartRun, 2> run {};
    double masterCur = 1.0;
    std::array<double, maxHostBlock> mixL {}, mixR {}, mono {}, fxL {}, fxR {};
    std::array<float, maxHostBlock> rackL {}, rackR {}, rackFxL {}, rackFxR {};
    bool split = false;
    std::array<double, 4 * maxHostBlock> gains {};
    std::array<double, 8 * maxHostBlock> laneSums {};
    std::array<float, bankSize> tmpT {}, tmpT0 {}, tmpA {}, tmpB {};
    std::array<float, 4096 + 8> imgAmp {};        // image column (≤ stride entries)
    std::array<float, 3 * (4096 + 8) + 8> imgTmp {}; // formant scratch (envelope, fine structure, output)
    std::array<double, bankSize + 600> convBuf {};

    // ---- bookkeeping ----

    int allocNote() noexcept
    {
        for (int i = 0; i < maxNotes; ++i)
            if (! notes[(std::size_t) i].used)
                return i;
        // full: forget the oldest released note without voices, else the oldest note
        int best = -1;
        for (int i = 0; i < maxNotes; ++i)
            if (best < 0 || notes[(std::size_t) i].seq < notes[(std::size_t) best].seq)
                best = i;
        if (best >= 0)
            dropNote (best);
        return best;
    }

    void dropNote (int slot) noexcept
    {
        auto& nt = notes[(std::size_t) slot];
        for (int v = 0; v < nt.nv; ++v)
            records[(std::size_t) nt.voices[(std::size_t) v]].note = -1;
        nt.nv = 0;
        if (nt.inHeads)
            removeHead (slot);
        if (nt.inHeld)
            removeHeld (slot);
        nt = NoteRec();
    }

    void maybeFree (int slot) noexcept
    {
        auto& nt = notes[(std::size_t) slot];
        if (nt.used && ! nt.pressed && ! nt.inHeld && (! nt.inHeads || nt.nv == 0))
            dropNote (slot);
    }

    void pushHead (int s) noexcept
    {
        if (headCount < maxNotes)
            heads[(std::size_t) headCount++] = s, notes[(std::size_t) s].inHeads = true;
    }
    void removeHead (int s) noexcept
    {
        for (int i = 0; i < headCount; ++i)
            if (heads[(std::size_t) i] == s)
            {
                std::copy (heads.begin() + i + 1, heads.begin() + headCount, heads.begin() + i);
                --headCount;
                break;
            }
        notes[(std::size_t) s].inHeads = false;
    }
    void pushHeld (int s) noexcept
    {
        if (heldCount < maxNotes)
            held[(std::size_t) heldCount++] = s, notes[(std::size_t) s].inHeld = true;
    }
    void removeHeld (int s) noexcept
    {
        for (int i = 0; i < heldCount; ++i)
            if (held[(std::size_t) i] == s)
            {
                std::copy (held.begin() + i + 1, held.begin() + heldCount, held.begin() + i);
                --heldCount;
                break;
            }
        notes[(std::size_t) s].inHeld = false;
    }

    Levels levels (const NoteRec& nt) const noexcept
    {
        return { (float) nt.fp, (float) nt.fv, (float) nt.fpitch, 0.0f, 0.0f, (float) nt.vel };
    }

    void setNote (Record& v, int slot) noexcept
    {
        const auto& nt = notes[(std::size_t) slot];
        v.fpan = nt.fp, v.fvol = nt.fv, v.fpitch = nt.fpitch, v.vel = nt.vel, v.note = slot;
    }

    void releaseNote (int slot) noexcept
    {
        auto& nt = notes[(std::size_t) slot];
        if (nt.inHeld)
        {
            removeHeld (slot);
            return;
        }
        if (! nt.inHeads)
            return;
        int h = -1;
        for (int i = 0; i < heldCount; ++i)
        {
            const auto c = held[(std::size_t) i];
            const auto& hn = notes[(std::size_t) c];
            if (hn.group != nt.group)
                continue;
            if (h < 0 || hn.st > notes[(std::size_t) h].st || (hn.st == notes[(std::size_t) h].st && hn.seq > notes[(std::size_t) h].seq))
                h = c;
        }
        if (h >= 0)
        {
            removeHeld (h);
            removeHead (slot);
            pushHead (h);
            const auto L = levels (nt);
            auto& hn = notes[(std::size_t) h];
            hn.voices = nt.voices, hn.nv = nt.nv, hn.group = nt.group;
            nt.nv = 0;
            for (int v = 0; v < hn.nv; ++v)
            {
                auto& r = records[(std::size_t) hn.voices[(std::size_t) v]];
                setNote (r, h);
                glideTo (r, L, false);
            }
            return;
        }
        for (int v = 0; v < nt.nv; ++v)
        {
            auto& r = records[(std::size_t) nt.voices[(std::size_t) v]];
            r.released = true;
            for (auto& s : r.arts)
                s.release();
        }
    }

    int allocRecord() noexcept
    {
        const auto r = freeList[(std::size_t) --freeCount];
        records[(std::size_t) r].used = true;
        return r;
    }

    // a record leaves: back to the pool, out of its note
    void finishRecord (int ri) noexcept
    {
        auto& v = records[(std::size_t) ri];
        if (v.note >= 0)
        {
            auto& nt = notes[(std::size_t) v.note];
            int kept = 0;
            for (int k = 0; k < nt.nv; ++k)
                if (nt.voices[(std::size_t) k] != ri)
                    nt.voices[(std::size_t) kept++] = nt.voices[(std::size_t) k];
            nt.nv = kept;
            const auto slot = v.note;
            v.note = -1;
            maybeFree (slot);
        }
        v.used = false;
        freeList[(std::size_t) freeCount++] = ri;
    }

    void killRecord (int ri) noexcept
    {
        for (int i = 0; i < liveCount; ++i)
            if (live[(std::size_t) i] == ri)
            {
                std::copy (live.begin() + i + 1, live.begin() + liveCount, live.begin() + i);
                --liveCount;
                break;
            }
        finishRecord (ri);
    }

    bool ended (const Record& v) const noexcept
    {
        const auto& p = P->parts[(std::size_t) v.part];
        if (v.imgEnd)
            return true;
        if (p.arts[1].hasEnv)
            return v.arts[1].es.stage > 8;
        return v.released;
    }

    // ---- record start (Voice.__init__) ----

    void startRecord (Record& v, int pk, int ku, const NoteRec& nt, int gvoice, float udk, const std::array<float, bankSize>& hzu,
                      std::uint32_t s14c, std::uint32_t s148) noexcept
    {
        const auto& p = P->parts[(std::size_t) pk];
        v.part = pk, v.ku = ku;
        v.stamp = stampCounter++;
        v.t0 = now, v.scount = now;
        const int n = p.nuni;
        if (n == 1)
            v.uPan = 0.0, v.uPh = -1.0, v.uDet = 0.0, v.uGain = 1.0;
        else
        {
            const auto fv = f32 (ku * f32 (1.0 / (n - 1)));
            const auto uk = f32 (f32 (fv * 2.0) - 1.0);
            v.uPan = f32 (std::min (std::max (f32 (uk * p.uniPan), -1.0), 1.0));
            v.uPh = f32 (ku * f32 (1.0 / n));
            v.uDk = f32 (p.uniD[(std::size_t) ku]);
            if (p.unitype == 4)
            {
                v.hzu = hzu;
                v.uDk = 1.0;
            }
            if (p.unitype == 3)
                v.uDk = (double) ((float) p.uniD[(std::size_t) ku] + udk);
            v.uDet = f32 (v.uDk * p.uniS);
            v.uGain = p.uniGain;
            if (p.uniInv && ((ku + p.k) & 1) == 1)
                v.uGain = -v.uGain;
        }
        v.fvol = nt.fv, v.fpan = nt.fp, v.fpitch = nt.fpitch, v.vel = nt.vel;
        v.n = 0, v.firstblk = true;
        v.a.fill (0.0f), v.T.fill (0.0f), v.kg.fill ((float) p.glide), v.ph.fill (0.0), v.f.fill (0.0);
        v.centsValid = false, v.prismValid = false, v.hbValid = false;
        v.vibph = 0.0, v.dtPrev = 0.0, v.phph = 0.0, v.phi70 = 0.0;
        v.gx.fill (0.0f), v.gy.fill (0.0f), v.gcnt = 0, v.gc = 1.0f, v.vfac = 1.0f;
        const auto uin = p.nuni == 1 ? 0.5 : f32 (ku * f32 (1.0 / (p.nuni - 1)));
        const auto* gv = gvoice >= 0 ? &records[(std::size_t) gvoice] : nullptr;
        for (int k = 0; k < 43; ++k)
        {
            const auto& art = p.arts[(std::size_t) k];
            const auto rand = (double) lastMaps[(std::size_t) std::min (k, P->maxrk)];
            const EnvState* share = (gv != nullptr && art.envGlobal) ? &gv->arts[(std::size_t) k].es : nullptr;
            v.arts[(std::size_t) k].start (art, rand, uin, share, run[(std::size_t) pk].gl[(std::size_t) k]);
        }
        v.shared = gv != nullptr && p.anyEnvGlobal;
        v.released = false;
        v.knValid[0] = v.knValid[1] = false, v.lsValid[0] = v.lsValid[1] = false;
        v.prism = 0.0, v.pluckOn = false;
        v.hbM = 0, v.hbG = 1.0f;
        v.tbBuf.fill (0.0f), v.tbFresh = true;
        v.pkC = 1 - (int) rndi (0.002 * p.fs / p.B), v.pkPrev = -1.0, v.pkT = -1.0, v.pkIc = 0;
        v.pkLev.fill (0.0f), v.pkLev[1] = 1.0f, v.pkRate.fill (1.0f);
        v.dpos = -1, v.pendOldn = -1, v.forceChanged = false, v.freephase = false;
        v.hz.fill (0.0f), v.l2.fill (0.0f), v.hzdirty = true;
        v.seed148 = s14c, v.s14c = s14c, v.s148 = s148;
        v.xyz = { (float) p.modx, (float) p.mody, (float) p.modz };
        v.nzSeed = s148, v.nzW = -1.0f, v.nzPos = 0.0, v.nzLen = 0.0, v.nzRate = 0.0;
        v.bpValid = false, v.pbValid = false;
        v.ipos = ImagePos {};
        if (p.img != nullptr)
            v.ipos.off = p.img->toff;
        v.imgEnd = false, v.v1b0 = 0.0;
        v.cnt = 0;
        v.firstlen = v.shared ? gv->cnt : -1;
        v.curValid = false;
        v.e8 = P->e8;
        v.oscTop = 0, v.oscAge = 0;
        v.oscF.fill (0.0);
        v.kmM.fill (-1);
    }

    // ---- portamento / legato glide of the level offsets (FUN_00888a50) ----

    void glideTo (Record& v, const Levels& L, bool porta) noexcept
    {
        const auto& p = P->parts[(std::size_t) v.part];
        // the note's glide factor ([DECOMP FUN_00883ae0, note set-up], REA_rodada165g-flbeta165s §2): 1, or (1 − 0.85·velocity)² in
        // float when global 6 is on (a harder note glides faster)
        if (P->portaVel && v.note >= 0 && v.note < maxNotes)
        {
            const auto x = (float) (1.0 - (double) (float) notes[(std::size_t) v.note].vel * 0.85);
            v.vfac = x * x;
        }
        auto lc = f32 (p.portaT * v.vfac);
        bool toX = false;
        if (lc > 0.0)
        {
            for (int i = 0; i < 8; ++i)
                v.gy[(std::size_t) i] = v.gy[(std::size_t) i] - v.gx[(std::size_t) i];
            toX = true;
        }
        auto a = toX ? v.gx : v.gy;
        const double cur[6] { v.fpan, v.fvol, v.fpitch, 0.0, 0.0, v.vel };
        for (int i : { 2, 3, 4 })
            a[(std::size_t) i] = (a[(std::size_t) i] + L[(std::size_t) i]) - (float) cur[i];
        for (int i : { 0, 1, 5 })
            a[(std::size_t) i] = porta ? 0.0f : (a[(std::size_t) i] + L[(std::size_t) i]) - (float) cur[i];
        if (porta && p.portaLim > 0)
        {
            if (p.portaLim < 1200)
            {
                const auto vv = std::min (std::fabs ((double) a[2]), (double) p.portaLim);
                a[2] = (float) (sgn (a[2]) * vv);
            }
            else
            {
                const auto sg = (float) sgn (a[2]);
                int guard = 0;
                while (std::fabs ((double) a[2]) > 1250.0 && guard++ < 100000)
                    a[2] = a[2] - sg * 1200.0f;
            }
        }
        if (p.portaVar)
            lc = f32 (lc * std::fabs ((double) a[2]) * 0.0008333333333333334);
        if (lc <= 0.0)
        {
            const auto Nn = 1 - (int) rndi (lc);
            v.gcnt = Nn;
            const auto kk = (float) (-1.0 / Nn);
            v.gy = a;
            for (int i = 0; i < 8; ++i)
                v.gx[(std::size_t) i] = a[(std::size_t) i] * kk;
        }
        else
        {
            v.gcnt = -1;
            v.gc = (float) std::exp (ln001 / (lc * 0.70710678118654746));
            v.gx = a;
            for (int i = 0; i < 8; ++i)
                v.gy[(std::size_t) i] = v.gy[(std::size_t) i] + a[(std::size_t) i];
        }
    }

    // ---- one control block of a record (FUN_0088ab10 / hmodel.Voice.control) ----

    void hzarr (Record& v) noexcept
    {
        if (! v.hzdirty)
            return;
        const auto fsF = (float) P->fs;
        for (int i = 0; i < v.n; ++i)
        {
            auto h = (float) v.f[(std::size_t) i] * fsF;
            h = std::max (h, 16.3515987f);
            v.hz[(std::size_t) i] = h;
            v.l2[(std::size_t) i] = (float) fastLog ((double) h) * 1.44269502f;
        }
        v.hzdirty = false;
    }

    void control (Record& v) noexcept
    {
        const auto& p = P->parts[(std::size_t) v.part];
        const auto fs = P->fs;
        if (v.gcnt != 0)
        {
            if (v.gcnt < 1)
            {
                const auto c = v.gc;
                for (std::size_t i = 0; i < 8; ++i)
                {
                    const auto xn = v.gx[i] * c;
                    v.gx[i] = xn;
                    v.gy[i] = ((v.gy[i] - xn) * c) + xn;
                }
            }
            else
            {
                for (std::size_t i = 0; i < 8; ++i)
                    v.gy[i] = v.gy[i] + v.gx[i];
                v.gcnt -= 1;
            }
        }
        const auto dt = v.firstblk ? 0.0 : 1.0;
        {
            // X/Y/Z smoothing state (voice +0x3334…): ((t + c) + (y − t)·c) − c in float, every block
            const float tg[3] { (float) p.modx, (float) p.mody, (float) p.modz };
            const auto c = p.xyzC;
            for (int i = 0; i < 3; ++i)
                v.xyz[(std::size_t) i] = ((tg[i] + c) + (v.xyz[(std::size_t) i] - tg[i]) * c) - c;
        }
        ArtInputs in;
        in.in[inKey] = f32 ((v.fpitch + p.mainpitch + (double) v.gy[2] + 6000.0) * 8.333333333333333e-05);
        in.in[inVel] = f32 (v.vel + (double) v.gy[5]); // (the model's offsets are doubles once a glide has set them)
        in.in[inX] = (double) (v.xyz[0] + (0.0f + v.gy[3]) * 0.5f);
        in.in[inY] = (double) (v.xyz[1] + (0.0f + v.gy[4]) * 0.5f);
        in.in[inZ] = (double) v.xyz[2];
        for (int k = 31; k < 43; ++k)
        {
            v.arts[(std::size_t) k].step (v.dtPrev, in);
            if (v.firstblk)
                v.arts[(std::size_t) k].first();
        }
        if (v.firstblk)
        {
            const auto g34 = v.arts[34].out, g35 = v.arts[35].out, g36 = v.arts[36].out, g37 = v.arts[37].out;
            const bool b29 = ! (g34 == 1.0 && g35 == 1.0 && g36 == 0.0 && g37 == 1.0);
            const auto ph40 = v.arts[40].out;
            const auto b4 = f32 (v.arts[33].out * 2.0);
            const auto b0 = p.p[14] != 0 ? (double) ((float) b4 + (float) v.vel) : b4;
            if (b29 || ph40 != 0.0 || b4 > 0.0 || b0 > 0.0)
                for (int k = 0; k < 31; ++k)
                {
                    auto& s = v.arts[(std::size_t) k];
                    const auto& a = *s.a;
                    if (a.hasEnv)
                    {
                        if (b29)
                        {
                            const double g[4] { g34, g35, g36, g37 };
                            a.env.globalScales (s.es, g);
                        }
                        const auto ax = a.k == 1 ? b0 : b4;
                        if (ax > 0.0 && a.env.decay >= 0)
                        {
                            double tot = 0.0;
                            for (int i = 0; i <= a.env.decay && i < a.env.c.size(); ++i)
                                tot += a.env.c.dx[(std::size_t) i];
                            a.env.step (s.es, (tot * s.es.sc[0] * std::min (1.0, ax)) / a.env.rate);
                        }
                    }
                    if (ph40 != 0.0 && a.lfoOn)
                        s.lph = (std::uint32_t) ((long long) s.lph + rndi (ph40 * 4294967296.0));
                }
        }
        in.lspeed = v.arts[39].out;
        in.lamt = f32 (v.arts[38].out * p.mainLfo);
        for (int k = 0; k < 31; ++k)
        {
            v.arts[(std::size_t) k].step (dt, in);
            if (v.firstblk)
                v.arts[(std::size_t) k].first();
        }
        v.dtPrev = dt;
        double A[43];
        for (int k = 0; k < 43; ++k)
            A[k] = v.arts[(std::size_t) k].out;
        const Image* im = p.img.get();
        if (im != nullptr)
        {
            v.ipos.step (*im, A[27], A[28]);
            // a one-shot image that ran out ends the record when the time-offset articulator (27, or 28 with the
            // filters in reverse order: part +0xb64) has no envelope (FUN_008856b0, record state 9 inside the block)
            if (v.ipos.ended && ! p.arts[(std::size_t) (27 + p.firstF)].hasEnv)
            {
                v.imgEnd = true;
                if (v.dpos < 0)
                    v.dpos = 0;
            }
        }
        const bool imf = im != nullptr && im->hasFreq;
        const bool imu = imf && im->fmode == 3;

        auto cents = (double) (((float) v.fpitch + (float) p.pitch) + v.gy[2]);
        if (p.vibDepth != 0.0)
            cents = f32 (cents + std::sin ((double) ((float) v.vibph * 6.28318548f)) * p.vibDepth * A[25]);
        cents = f32 (cents + f32 (p.pitchamt * A[24]));
        const auto a4 = f32 (f32 (v.uDet * A[26]) * A[26]);
        const auto pv = f32 (p.prismK * A[23]);
        bool prismChanged = false;
        if (! v.prismValid || pv != v.prismV)
        {
            v.prismValid = true;
            v.prismV = pv;
            prismChanged = true;
            if (pv == 0.0)
                v.prism = 0.0;
            else if (p.prismMul == 0)
                v.prism = f32 (f32 ((pv > 0.0 ? 1.0 : -1.0) * f32 (pv * pv)) * 4.0);
            else
                v.prism = f32 (std::min (std::max (pv, -1.0), 1.0));
        }
        const bool hbChanged = hblurUpdate (v, p, A);
        bool changed = ! v.centsValid || cents != v.centsPrev || a4 != v.a4Prev || prismChanged || hbChanged;
        if (imf)
            changed = true; // image frequency plane: frequencies rebuilt every block (voice +0x7c forced)
        int n = v.n;
        if (changed)
        {
            v.centsValid = true;
            v.centsPrev = cents, v.a4Prev = a4;
            v.v1b0 = f32 (cents + a4);
            const auto l2 = f32 (log2Of440 + (f32 (f32 (cents * f32 (0.01)) + 60.0) - 57.0) * 0.08333333333333333);
            v.l2f0 = l2;
            const auto f0 = f32 (std::exp (l2 * ln2));
            auto f0fs = f32 (f0 * (1.0 / fs));
            v.f0 = f0, v.f0fs32 = f0fs;
            if (p.unitype < 4 && ! p.hasUniTab && ! imu)
                f0fs = f32 (f0fs * std::exp (a4 * 0.0008333333333333334 * ln2));
            double q = p.pmode == 0 ? f32 ((p.l2fmax - std::log (f0) * 1.4426950408889634) * p.k19c) : f32 (p.fmax / f0);
            if (p.prismMul > 0 && v.prism != 0.0)
            {
                auto fv = f32 (1.0 - std::fabs (v.prism));
                if (fv <= 0.009999999776482582)
                    fv = 0.009999999776482582;
                q = f32 (q / fv);
            }
            long long cnt = (std::isfinite (q) ? std::min (std::max (rndi (q), -1000000LL), 1000000LL) : 1000000LL) + 1 + v.hbM;
            cnt = std::max (cnt, 1LL) + 4;
            cnt = std::min (cnt, (long long) bankSize);
            const auto nb = (int) ((cnt + 7) & ~7LL);
            const auto oldn = v.n;
            if (nb > v.n)
            {
                v.n = nb;
                v.pkPrev = std::numeric_limits<double>::quiet_NaN(), v.bpValid = false; // (+0x1cc = −1)
            }
            n = v.n;
            for (int i = 0; i < n; ++i)
                v.f[(std::size_t) i] = p.ratio[(std::size_t) i] * f0fs;
            if (p.pmode == 0)
                for (int i = 0; i < n; ++i)
                    v.f[(std::size_t) i] = std::min (v.f[(std::size_t) i], P->fclamp);
            if (p.unitype == 4 && p.nuni > 1 && ! imu)
            {
                const auto val = (float) (a4 * 0.006666666666666667 * v.f0fs32);
                for (int i = 0; i < n; ++i)
                {
                    auto off = v.hzu[(std::size_t) i] * val;
                    if (p.hasUniTab)
                        off = off * p.uniTab[(std::size_t) i];
                    v.f[(std::size_t) i] = v.f[(std::size_t) i] + (double) off;
                }
            }
            if (p.unitype < 4 && p.hasUniTab && a4 != 0.0 && ! imu)
            {
                const auto val = (float) (a4 * 0.0008333333333333334 * ln2);
                for (int i = 0; i < n; ++i)
                    v.f[(std::size_t) i] = v.f[(std::size_t) i] * (double) std::exp (p.uniTab[(std::size_t) i] * val);
            }
            if (v.firstblk)
            {
                double d32 = 0.0;
                if (v.uPh == -1.0)
                    d32 = p.phase0;
                else
                {
                    auto x = f32 (p.uniPh * A[41]);
                    if (p.unitype == 4)
                        x = -std::fabs (x);
                    if (x <= 0.0)
                        d32 = f32 (p.phase0 + f32 (v.uPh * x));
                    else if (x == 1.0)
                    {
                        v.seed148 = v.s14c;
                        d32 = 0.0;
                        v.freephase = true;
                    }
                    else
                    {
                        const auto d = (std::exp (f32 (x * 16.0) * ln2) * 2.0 - 2.0) * std::exp (v.uDet * 0.00057762265046662107);
                        d32 = (p.phase0 + d) - rnd (d);
                    }
                }
                const auto la8 = v.freephase ? -1.0 : p.phrand;
                if (la8 >= 0.0 && p.phrand > 0.0)
                {
                    auto x = v.f[4] * 277.0 * (double) v.scount;
                    if (std::fabs (x) >= 1.0)
                        x = x - std::trunc (x);
                    d32 = d32 + p.phrand * x;
                }
                if (la8 < 0.0)
                {
                    const auto sd = v.freephase ? v.seed148 : v.s148;
                    double r[bankSize];
                    rand64f (r, bankSize, 0.0, f32 (-la8), sd);
                    for (int i = 0; i < bankSize; ++i)
                        v.ph[(std::size_t) i] = r[i] + p.phtab[(std::size_t) i];
                }
                else
                    v.ph = p.phtab;
                v.phi70 = v.ph[(std::size_t) n - 1];
                if (d32 != 0.0)
                {
                    const auto r = d32 / v.f[4];
                    for (int i = 0; i < n; ++i)
                        v.ph[(std::size_t) i] = v.ph[(std::size_t) i] + v.f[(std::size_t) i] * r;
                }
                v.ph[0] = v.ph[1] = 0.0;
                if (p.submode > 0)
                    v.ph[2] = v.ph[3] = 0.0;
            }
            else if (n > oldn)
                v.pendOldn = oldn; // entering partials get their phase offset at the end of the block
            if (imf)
                imageFreq (v, p, *im, A, a4);
            v.hzdirty = true;
        }
        // timbre mix + sub gains (voice +0x1a0 is also the source of the protection module)
        auto mt = f32 (p.tmix * A[3]);
        if (mt >= 1.0)
            mt = 1.0;
        if (p.blend == 5)
        {
            // pluck blend (FUN_00885b70): timbre 1 plays; the mix steers the pluck decay (a change invalidates it)
            if (! v.pbValid || mt != v.pbMix)
                v.pbValid = true, v.pbMix = mt, v.pkPrev = std::numeric_limits<double>::quiet_NaN(), v.bpValid = false;
            mt = 0.0;
        }
        auto& T = tmpT;
        if (mt <= 0.0)
            std::copy (p.T1.begin(), p.T1.begin() + n, T.begin());
        else if (mt == 1.0)
            std::copy (p.T2.begin(), p.T2.begin() + n, T.begin());
        else
        {
            const auto w1 = (float) f32 (1.0 - mt), w2 = (float) mt;
            for (int i = 0; i < n; ++i)
                T[(std::size_t) i] = p.T1[(std::size_t) i] * w1 + p.T2[(std::size_t) i] * w2;
        }
        for (std::size_t i = 0; i < 4 && (int) i < n; ++i)
            T[i] = T[i] * p.subg[i];
        auto& T0 = tmpT0;
        std::copy (T.begin(), T.begin() + n, T0.begin());
        v.agref = 1.6428422f;
        if (im != nullptr && im->hasGain)
            imageGain (v, p, *im, A, T.data(), T0.data());
        v.volB4 = (float) f32 (f32 (A[1] * A[1]) * p.gain12);
        v.volB8 = 1.0f;
        const auto mixv = f32 (std::min (std::max (f32 (p.mix12 * A[14]), -1.0), 1.0));
        v.fmix = std::fabs (mixv);
        v.fsign = mixv == 1.0 ? 0 : (mixv > 0.0 ? 1 : -1);
        v.fwhich = p.firstF, v.fcarry = 1.0;
        v.fc[0] = v.fc[1] = 0.0, v.r1 = 0.0;
        for (int ci = 0; ci < p.chainLen; ++ci)
            switch (p.chain[(std::size_t) ci])
            {
                case mFilter: filter (v, p, A, T.data()); break;
                case mLeq:
                    for (int i = 0; i < n; ++i)
                        T[(std::size_t) i] = T[(std::size_t) i] * p.leq[(std::size_t) i];
                    break;
                case mPrism:
                    if (v.prism != 0.0 && (changed || p.prismLvl > 0))
                        prismModule (v, p, T.data());
                    break;
                case mHarmonizer: harmonizer (v, p, A, T.data()); break;
                case mHrand:
                {
                    // FUN_00886e30: T ·= the record's random factors over [start, start + count)
                    const int mm = std::min (n - p.hrSt, p.hrCnt);
                    for (int i = 0; i < mm; ++i)
                        T[(std::size_t) (p.hrSt + i)] = T[(std::size_t) (p.hrSt + i)] * v.hr[(std::size_t) (p.hrSt + i)];
                    break;
                }
                case mPhaser: phaser (v, p, A, T.data()); break;
                case mPluck: pluck (v, p, A, T.data()); break;
                case mBlur: hblur (v, p, A, T.data()); break;
                case mProt:
                {
                    const auto e = std::min (p.protEnd, n);
                    for (int i = 0; i < e; ++i)
                        T[(std::size_t) i] = T[(std::size_t) i] + ((T0[(std::size_t) i] - T[(std::size_t) i]) * p.protTab[(std::size_t) i]) * p.protAmt;
                    break;
                }
                case mClip: clip (v, p, A, T.data()); break;
                case mVolume:
                    if (v.volB4 != 1.0f)
                    {
                        for (int i = 0; i < n; ++i)
                            T[(std::size_t) i] = T[(std::size_t) i] * v.volB4;
                        v.volB8 = v.volB4;
                        v.volB4 = 1.0f;
                    }
                    break;
            }
        hzarr (v);
        if (p.submode > 0)
            for (std::size_t i = 0; i < 4; ++i)
            {
                // subs below the fundamental fade out under ~114 Hz (runtime constant 0x94e120)
                const float subk = 0.0087365862f;
                auto k = std::min (v.hz[i] * subk, 1.0f);
                k = k * k;
                T[i] = (k * k) * T[i];
            }
        // self-oscillation partial (position 0): filter-1 cutoff × 2^(p60/1200)
        const auto hz0 = (float) f32 (v.fc[0] * p.filt[0].rofsF);
        v.hz[0] = hz0;
        v.l2[0] = hz0 > 0.0f ? (float) f32 (std::log ((double) hz0)) * 1.44269502f : 0.0f;
        const auto f0n = (double) hz0 * (1.0 / fs);
        if (f0n != v.f[0])
            v.f[0] = f0n;
        T[0] = ((p.subg[0] * (float) v.r1) * v.pkLev[1]) * v.volB8;
        if (n > 456)
        {
            const auto& tf = topFade().v;
            for (int i = 456; i < n; ++i)
                T[(std::size_t) i] = T[(std::size_t) i] * tf[(std::size_t) (i - 456)];
        }
        {
            const auto S = (float) p.geqT.S, o = (float) p.geqT.o;
            for (int i = 0; i < n; ++i)
                if (T[(std::size_t) i] != 0.0f)
                    T[(std::size_t) i] = T[(std::size_t) i] * tableRead (p.geqT, v.l2[(std::size_t) i] * S + o, P->prec);
        }
        if (p.p[13] != 0)
        {
            // auto gain: energy of the final partial levels vs the reference 1.6428422 (8 float lanes)
            float acc[8] {};
            for (int r = 0; r < n; r += 8)
                for (int j = 0; j < 8; ++j)
                    acc[j] = acc[j] + T[(std::size_t) (r + j)] * T[(std::size_t) (r + j)];
            float E = 0.0f;
            for (int j : { 4, 0, 6, 2, 5, 1, 7, 3 })
                E = E + acc[j];
            float g = 4096.0f;
            if (E > 0.0f)
            {
                g = (float) std::sqrt ((double) (v.agref / E));
                g = g < 0.000244140625f ? 0.000244140625f : (g > 4096.0f ? 4096.0f : g);
            }
            v.volB4 = v.volB4 * (((p.autog * g) + 1.0f) - p.autog);
        }
        if (v.volB4 != 1.0f)
            for (int i = 0; i < n; ++i)
                T[(std::size_t) i] = T[(std::size_t) i] * v.volB4;
        std::copy (T.begin(), T.begin() + n, v.T.begin());
        if (v.forceChanged)
            changed = true, v.forceChanged = false;
        if (changed)
            for (int i = 0; i < n; ++i)
                v.f[(std::size_t) i] = std::min (v.f[(std::size_t) i], P->fclamp);
        if (v.pendOldn >= 0)
        {
            const auto po = v.pendOldn;
            v.pendOldn = -1;
            if (po > 0 && v.f[(std::size_t) po - 1] > 0.0)
            {
                const auto kk = (v.ph[(std::size_t) po - 1] - v.phi70) / v.f[(std::size_t) po - 1];
                v.phi70 = v.ph[(std::size_t) n - 1];
                for (int i = po; i < n; ++i)
                    v.ph[(std::size_t) i] = v.ph[(std::size_t) i] + v.f[(std::size_t) i] * kk;
            }
        }
        if (changed)
        {
            if (p.glideHz != 0.0)
            {
                const auto gh = (float) p.glideHz;
                for (int i = 0; i < n; ++i)
                    v.kg[(std::size_t) i] = (float) fastExp ((double) (v.hz[(std::size_t) i] * gh));
            }
            else
                std::fill (v.kg.begin(), v.kg.begin() + n, (float) p.glide);
        }
        v.fxv = f32 (f32 (p.fx) * A[2]);
        v.artpan = A[0];
        v.firstblk = false;
    }

    // ---- image synthesis (REA_rodada75) ----

    // gain plane × timbre mix (voice FUN_0088ab10, image branch), with the formant shift and the gain mix
    void imageGain (Record& v, const Part& p, const Image& im, const double* A, float* T, const float* T0) noexcept
    {
        const int n = v.n;
        const auto x = (float) ((double) (float) (((float) p.p[93] + (float) A[29] * 12000.0f) - (float) v.v1b0) + p.part30);
        const bool noform = im.fmix == 0.0f || (std::fabs ((double) x) < 5.960464477539063e-08 && ! im.hasT78);
        const int cnt = noform ? std::min ((n - 4 + 7) & ~7, im.stride) : im.stride;
        auto& a = imgAmp;
        im.amp (v.ipos.pos, cnt, a.data());
        if (! noform)
            im.formant (a.data(), v.ipos.pos, x, imgTmp.data());
        const int m = std::min (cnt, n - 4);
        const int m8 = (m & 7) != 0 ? (m & ~7) + 4 : m;
        for (int i = 0; i < m8; ++i)
            T[4 + i] = a[(std::size_t) i] * T0[4 + i];
        T[0] = 0.0f;
        T[1] = T0[1] * a[0];
        if (p.submode == 0)
        {
            T[2] = (T0[2] * (a[0] + a[1])) * 0.5f;
            T[3] = (T0[3] * (a[1] + a[2])) * 0.5f;
        }
        else
            T[2] = T0[2] * a[0], T[3] = T0[3] * a[0];
        for (int i = m + 4; i < n; ++i)
            T[i] = 0.0f;
        if (im.gmix != 1.0f)
        {
            const auto w = 1.0f - im.gmix;
            for (int i = 0; i < n; ++i)
                T[i] = (T0[i] - T[i]) * w + T[i];
        }
        if (p.p[13] != 0 && p.misc47 == 1)
        {
            float acc[8] {};
            for (int r = 0; r < n; r += 8)
                for (int j = 0; j < 8; ++j)
                    acc[j] = acc[j] + T[r + j] * T[r + j];
            float E = 0.0f;
            for (int j : { 4, 0, 6, 2, 5, 1, 7, 3 })
                E = E + acc[j];
            v.agref = E;
        }
    }

    // frequency plane applied to the partial frequencies (modes Oct, Hz, Wide Hz, Unison)
    void imageFreq (Record& v, const Part& p, const Image& im, const double* A, double a4) noexcept
    {
        const int n = v.n;
        const int e0 = std::min ((n - 4 + 7) & ~7, im.stride);
        const int m = std::min (e0, n - 4);
        const auto a0 = (float) (im.k174 * A[30]);
        if (a0 == 0.0f)
            return;
        auto& buf = imgAmp;
        im.column (im.freq, v.ipos.pos, im.interpFreq, e0, buf.data());
        auto mask = [&] {
            const int c = std::min (e0 - im.fmaskSt, im.fmaskN);
            for (int i = im.fmaskSt; i < im.fmaskSt + c; ++i)
                buf[(std::size_t) i] = buf[(std::size_t) i] * im.fmask[(std::size_t) i];
        };
        switch (im.fmode)
        {
            case 0:
                mask();
                for (int i = 0; i < m; ++i)
                    v.f[(std::size_t) (4 + i)] = v.f[(std::size_t) (4 + i)] * (double) expF (buf[(std::size_t) i] * a0);
                break;
            case 1:
            case 2:
            {
                if (im.fmode == 2)
                    for (int i = 0; i < e0; ++i)
                        buf[(std::size_t) i] = buf[(std::size_t) i] + 1.0f;
                mask();
                const auto sc = (float) (v.f0fs32 * p.d * (im.fmode == 2 ? std::fabs ((double) a0) : (double) a0));
                for (int i = 0; i < m; ++i)
                    v.f[(std::size_t) (4 + i)] = v.f[(std::size_t) (4 + i)] + (double) (buf[(std::size_t) i] * sc);
                break;
            }
            default:
            {
                auto& t = imgTmp;
                std::fill (t.begin(), t.begin() + std::max (n, 4 + m), 0.0f);
                for (int i = 0; i < m; ++i)
                    t[(std::size_t) (4 + i)] = buf[(std::size_t) i] + 1.0f;
                t[0] = 0.0f, t[1] = t[4];
                if (p.submode == 0)
                    t[2] = (t[4] + t[5]) * 0.5f, t[3] = (t[5] + t[6]) * 0.5f;
                else
                    t[2] = t[4], t[3] = t[4];
                const int c = m + 4;
                for (int i = 0; i < c; ++i)
                    t[(std::size_t) i] = t[(std::size_t) i] * t[(std::size_t) i];
                for (int i = c; i < n; ++i)
                    t[(std::size_t) i] = 1.0f;
                if (p.unitype == 4 && p.nuni > 1)
                {
                    const auto s = (float) (a4 * 0.006666666666666667 * v.f0fs32 * std::fabs ((double) a0));
                    for (int i = 0; i < n; ++i)
                    {
                        auto w = (v.hzu[(std::size_t) i] * t[(std::size_t) i]) * s;
                        if (p.hasUniTab)
                            w = p.uniTab[(std::size_t) i] * w;
                        v.f[(std::size_t) i] = v.f[(std::size_t) i] + (double) w;
                    }
                }
                else
                {
                    const auto s = (float) (a4 * 0.0008333333333333334 * ln2 * std::fabs ((double) a0));
                    for (int i = 0; i < n; ++i)
                    {
                        const auto w = p.hasUniTab ? (p.uniTab[(std::size_t) i] * t[(std::size_t) i]) * s : t[(std::size_t) i] * s;
                        v.f[(std::size_t) i] = v.f[(std::size_t) i] * (double) expF (w);
                    }
                }
            }
        }
    }

    // ---- modules (FUN_00886xxx) ----

    // harmonic blur kernel: width, half size M, kernel taps and gain (voice +0x1fc, +0x204, +0x1f0, +0x200)
    bool hblurUpdate (Record& v, const Part& p, const double* A) noexcept
    {
        float W = 1.0f;
        if (p.p[23] != 0)
        {
            const auto x8 = (float) ((double) ((float) p.p[23] * (float) A[22]) * 0.0001);
            W = (float) ((double) (float) (std::exp ((double) x8 * std::log (17.0)) - 1.0) * 15.9375 + 1.0);
        }
        if (v.hbValid && (double) W == v.hbW)
            return false;
        v.hbValid = true;
        v.hbW = W;
        const auto M = std::min ((int) std::ceil ((double) W) | 1, 0x101) >> 1;
        const bool ch = M != v.hbM;
        v.hbM = M;
        if (M > 0)
        {
            auto sl = 1.0 / ((double) W * 0.5 + 1.0);
            const auto off = (float) (1.0 - sl * M);
            const auto slF = (float) sl;
            auto& k = v.hbK;
            const auto& sh = p.blurShape;
            std::uint32_t b0 = 0, b1 = 0;
            std::memcpy (&b0, &sh[0][0], 4);
            std::memcpy (&b1, &sh[1][0], 4);
            if (b0 == b1)
            {
                for (int j = 0; j <= M; ++j)
                    k[(std::size_t) j] = (float) ((double) off + (double) slF * j);
                if (sh[0][0] != 0.0f)
                    for (int j = 0; j < M; ++j)
                        k[(std::size_t) j] = std::expm1 (k[(std::size_t) j] * sh[0][0]) * sh[0][1];
                for (int j = 0; j < M; ++j)
                    k[(std::size_t) (M + 1 + j)] = k[(std::size_t) (M - 1 - j)];
            }
            else
            {
                for (int j = 0; j <= 2 * M; ++j)
                    k[(std::size_t) j] = (float) ((double) off + (double) slF * j);
                for (int j = M + 1; j <= 2 * M; ++j)
                    k[(std::size_t) j] = 2.0f - k[(std::size_t) j];
                for (int side = 0; side < 2; ++side)
                {
                    const auto c = sh[(std::size_t) side][0], c2 = sh[(std::size_t) side][1];
                    if (c != 0.0f)
                    {
                        const auto o = side * (M + 1);
                        for (int j = o; j < o + M; ++j)
                            k[(std::size_t) j] = std::expm1 (k[(std::size_t) j] * c) * c2;
                    }
                }
            }
            const auto g = pairwiseSum (k.data(), (std::size_t) (2 * M + 1));
            v.hbG = g > 5.960464477539063e-08f ? 1.0f / g : 1.0f;
        }
        return ch;
    }

    // FUN_00886610: time blur (per-partial one-pole towards T) and harmonic blur (partials 4… convolved), mask/mix
    void hblur (Record& v, const Part& p, const double* A, float* T) noexcept
    {
        const int M = v.hbM, n = v.n;
        const float* src = T;
        bool srcIsT = true;
        if (p.blurOn)
        {
            // "pluck replaces blur decay" (combo +0x46, REA_rodada75): the time blur decays each partial at the pluck
            // rate (voice +0x1c8/+0x1cc/+0x1e0, FUN_00888da0), recomputed when the pluck value or the partial count changes
            const auto v2 = (float) ((double) ((float) p.p[75] * 0.001f) + A[15]);
            if (! v.bpValid || v2 != v.bpV || n != v.bpN)
            {
                v.bpValid = true, v.bpV = v2, v.bpN = n;
                const auto t1c8 = (float) (std::exp ((double) ((v2 - 1.0f) * 17.0f) * ln2) * 20000.0);
                const auto f5 = (float) (p.pkBase / ((double) t1c8 + 0.25));
                pluckRates (p, A, n, f5, v.bpDec.data());
            }
            if (v.tbFresh)
            {
                v.tbBuf.fill (0.0f);
                if (! v.firstblk)
                    std::copy (T, T + n, v.tbBuf.begin());
                v.tbFresh = false;
            }
            for (int i = 0; i < n; ++i)
            {
                const auto bb = v.tbBuf[(std::size_t) i];
                const auto mn = std::min (T[i], bb);
                v.tbBuf[(std::size_t) i] = (((bb - mn) * v.bpDec[(std::size_t) i]) + T[i]) + ((mn - T[i]) * p.tbA);
            }
            std::copy (v.tbBuf.begin(), v.tbBuf.begin() + n, tmpA.begin());
            src = tmpA.data();
            srcIsT = false;
        }
        else if (p.tbA != 0.0f || p.tbD != 0.0f)
        {
            if (v.tbFresh)
            {
                v.tbBuf.fill (0.0f);
                if (! v.firstblk)
                    std::copy (T, T + n, v.tbBuf.begin());
                v.tbFresh = false;
            }
            for (int i = 0; i < n; ++i)
            {
                const auto bb = v.tbBuf[(std::size_t) i];
                const auto mn = std::min (T[i], bb);
                v.tbBuf[(std::size_t) i] = (((bb - mn) * p.tbD) + T[i]) + ((mn - T[i]) * p.tbA);
            }
            std::copy (v.tbBuf.begin(), v.tbBuf.begin() + n, tmpA.begin());
            src = tmpA.data();
            srcIsT = false;
        }
        else
            v.tbFresh = true;
        if (M <= 0 && srcIsT)
            return;
        auto& nw = tmpB;
        std::copy (src, src + n, nw.begin());
        if (M > 0 && n > 4)
        {
            auto* buf = convBuf.data();
            int L = 0;
            for (int i = 0; i < M; ++i)
                buf[L++] = (double) src[4];
            for (int i = 4; i < n; ++i)
                buf[L++] = (double) src[i];
            for (int i = 0; i < M; ++i)
                buf[L++] = 0.0;
            double kr[260];
            for (int u = 0; u <= 2 * M; ++u)
                kr[u] = (double) v.hbK[(std::size_t) (2 * M - u)];
            double res[bankSize];
            blurConv (buf, kr, 2 * M + 1, n - 4, res);
            for (int t = 0; t < n - 4; ++t)
                nw[(std::size_t) (4 + t)] = (float) res[t] * v.hbG;
        }
        if (p.hasBlurMask)
        {
            for (int i = 0; i < n; ++i)
                T[i] = ((nw[(std::size_t) i] - T[i]) * p.blurMask[(std::size_t) i]) * p.blurMix + T[i];
        }
        else if (p.blurMix == 1.0f)
            std::copy (nw.begin(), nw.begin() + n, T);
        else
            for (int i = 0; i < n; ++i)
                T[i] = (nw[(std::size_t) i] - T[i]) * p.blurMix + T[i];
    }

    void prismModule (Record& v, const Part& p, const float* T) noexcept
    {
        const auto f0fs = v.f0fs32;
        const int n = v.n;
        double tbl[bankSize];
        const double* tb = p.prismTab.data();
        if (p.prismLvl > 0)
        {
            // prism from level: table × min(T, 1) (mode 2: 1 − min(T, 1)); everything is recomputed every block
            for (int i = 0; i < n; ++i)
            {
                auto g = std::min (T[i], 1.0f);
                if (p.prismLvl == 2)
                    g = 1.0f - g;
                tbl[i] = (double) g * p.prismTab[(std::size_t) i];
                if (p.prismMul != 0)
                    tbl[i] = tbl[i] + 1.0;
            }
            tb = tbl;
            v.centsValid = false;
        }
        if (p.prismMul == 0)
        {
            const auto kk = f0fs * p.d * v.prism;
            for (int i = 0; i < n; ++i)
                v.f[(std::size_t) i] = v.f[(std::size_t) i] + tb[i] * kk;
        }
        else if (p.prismConst < 0.0 || p.prismLvl > 0)
        {
            const auto c = f32 (1.0 - v.prism);
            for (int i = 0; i < n; ++i)
                v.f[(std::size_t) i] = (v.f[(std::size_t) i] - f0fs) * (tb[i] * v.prism + c) + f0fs;
        }
        else
        {
            const auto kk = f32 (f32 (p.prismConst * v.prism) + f32 (1.0 - v.prism));
            for (int i = 0; i < n; ++i)
                v.f[(std::size_t) i] = v.f[(std::size_t) i] * kk + (f0fs - f0fs * kk);
        }
        v.hzdirty = true;
    }

    // FUN_00886eb0 (one call per filter module): shape, mask, resonance and the serial/parallel mix
    void filter (Record& v, const Part& p, const double* A, float* T) noexcept
    {
        const int f = v.fwhich;
        const auto& fl = p.filt[(std::size_t) f];
        const int n = v.n;
        const bool first = f == p.firstF;
        if (v.fsign < 0 && first)
        {
            std::copy (T, T + n, v.fbuf.begin());
            v.fcarry = f32 (1.0 - v.fmix);
        }
        const auto kn = fl.knob;
        if (! v.knValid[f])
            v.kn[f] = kn, v.knValid[f] = true;
        v.kn[f] = f32 (kn + f32 ((v.kn[f] - kn) * f32 (0.912201345)));
        const auto ktv = f32 ((v.l2f0 + -log2Of13081) * fl.kt);
        const auto lokt = f32 (fl.lo + ktv);
        auto L = f32 (f32 (v.kn[f] + ktv) + f32 (A[4 + f] * fl.span2));
        const auto& art = p.arts[(std::size_t) (4 + f)];
        if (art.hasEnv)
        {
            const auto ev = v.arts[(std::size_t) (4 + f)].es.out;
            const auto amt = f32 (fl.envamt * A[31 + f]);
            if (! fl.adaptEnv)
                L = f32 (L + f32 (f32 (ev * amt) * fl.span2));
            else
            {
                const auto a4 = f32 (f32 (amt * ev) * 4.0);
                L = a4 <= 0.0 ? f32 (L + f32 (a4 * f32 (fl.hi - lokt))) : f32 (L + f32 (a4 * f32 (fl.hi - L)));
            }
        }
        if (! v.lsValid[f] || v.firstblk)
            v.Ls[f] = L, v.lsValid[f] = true;
        else
            v.Ls[f] = f32 (L + f32 ((v.Ls[f] - L) * art.smooth));
        L = v.Ls[f];
        const auto r = f32 (f32 (fl.r * milli) * A[8 + f]);
        if (f == 0)
            v.r1 = r;
        const auto fc = f32 (std::exp (L * ln2));
        v.fc[f] = fc;
        const int prec = P->prec;
        if (fl.tab.flag && v.fcarry != 0.0)
        {
            auto sc = f32 (std::exp ((1.0 - f32 (fl.w * A[6 + f])) * ln2) * fl.tab.S);
            hzarr (v);
            const bool masked = fl.mask.flag;
            const auto mS = (float) fl.mask.S, mO = (float) fl.mask.o;
            float k1 = 0.0f, k2 = 0.0f;
            const float* x = nullptr;
            if (! fl.hz)
            {
                k1 = (float) sc, k2 = (float) f32 (fl.tab.o - f32 (L * sc));
                x = v.l2.data();
            }
            else
            {
                sc = f32 (sc * 0.9);
                k1 = (float) f32 (sc / fc), k2 = (float) f32 (fl.tab.o - sc);
                x = v.hz.data();
            }
            for (int i = 0; i < n; ++i)
            {
                if (T[i] == 0.0f)
                    continue; // (0 stays 0: the tables are finite)
                const auto g = tableRead (fl.tab, x[i] * k1 + k2, prec);
                if (masked)
                {
                    const auto Mv = tableRead (fl.mask, v.l2[(std::size_t) i] * mS + mO, prec);
                    T[i] = T[i] + (g * Mv - Mv) * T[i];
                }
                else
                    T[i] = T[i] * g;
            }
        }
        if (r > 0.0 && fl.hasRtab && fl.rtab.flag && v.fcarry != 0.0)
        {
            const auto k0 = ! fl.radapt ? fl.k0 : f32 (fl.k0 / v.f0);
            const auto wr = f32 (k0 * std::exp ((1.0 - f32 (fl.rw * A[10 + f])) * ln2));
            hzarr (v);
            float k1 = (float) wr, k2 = 0.0f;
            const float* x = nullptr;
            if (! fl.rhz)
            {
                k2 = (float) f32 (fl.rtab.o - f32 (f32 (f32 (L + fl.rofsOct) + f32 (A[12 + f] * 8.0)) * wr));
                x = v.l2.data();
            }
            else
            {
                const auto d13 = f32 (fc * fl.rofsF) * std::exp (f32 (A[12 + f] * 8.0) * ln2) * wr;
                k2 = (float) f32 (fl.rtab.o - d13);
                x = v.hz.data();
            }
            const auto kk = (float) (r * ln2);
            const bool masked = fl.rmask.flag;
            const auto mS = (float) fl.rmask.S, mO = (float) fl.rmask.o;
            for (int i = 0; i < n; ++i)
            {
                if (T[i] == 0.0f)
                    continue;
                auto e = tableRead (fl.rtab, x[i] * k1 + k2, prec);
                if (masked)
                    e = (tableRead (fl.rmask, v.l2[(std::size_t) i] * mS + mO, prec) * e) * kk;
                else
                    e = e * kk;
                T[i] = T[i] * (float) fastExp ((double) e);
            }
        }
        else if (r > 0.0 && fl.noise && v.fcarry != 0.0)
            noiseResonance (v, p, fl, A, f, r, T);
        if (v.fsign < 0)
        {
            if (first)
            {
                for (int i = 0; i < n; ++i)
                    std::swap (T[i], v.fbuf[(std::size_t) i]);
            }
            else
            {
                const auto kk = (float) v.fcarry;
                for (int i = 0; i < n; ++i)
                    T[i] = v.fbuf[(std::size_t) i] + (T[i] - v.fbuf[(std::size_t) i]) * kk;
            }
        }
        else if (v.fsign > 0)
        {
            if (first)
                std::copy (T, T + n, v.fbuf.begin());
            else
            {
                const auto kk = (float) f32 (1.0 - v.fcarry);
                for (int i = 0; i < n; ++i)
                    T[i] = T[i] + (v.fbuf[(std::size_t) i] - T[i]) * kk;
            }
        }
        v.fcarry = v.fmix;
        v.fwhich ^= 1;
    }

    // noise resonance (type 109, REA_rodada75): partials switched on at random, the pattern held for a time set by the
    // resonance width (voice +0x27c seed, +0x280 width cache, +0x288 position, +0x290 rate, +0x298 length)
    void noiseResonance (Record& v, const Part& p, const Filt& fl, const double* A, int f, double r, float* T) noexcept
    {
        const int n = v.n;
        const auto lw = (float) f32 (fl.rw * A[10 + f]);
        if (lw != v.nzW)
        {
            v.nzW = lw;
            const auto d = std::exp ((double) (lw * 0.25f) * std::log (801.0));
            v.nzLen = 9999.0 / ((d - 1.0) * 1.2475056689342403 + 1.9954648526077097);
            v.nzRate = p.B * 0.1000100010001 * (1.0 / p.fs) * v.nzLen;
        }
        const auto rr = f32 (r * r);
        auto a21 = (float) (rr * rr * rr * v.nzLen * 2.0);
        if (fl.rhz)
            a21 = a21 * 2.0f;
        auto a20 = a21 + 1.0f;
        auto& buf = tmpA;
        rand32f (buf.data(), n, 0.0f, a20, v.nzSeed);
        a20 = (float) std::sqrt ((double) a20);
        hzarr (v);
        const float kHz = -5.63707763e-05f; // runtime constant 0x94e11c
        if (! fl.rhz)
            for (int i = 0; i < n; ++i)
                buf[(std::size_t) i] = v.hz[(std::size_t) i] * kHz + buf[(std::size_t) i];
        if (! fl.radapt)
            for (int i = 0; i < n; ++i)
            {
                auto b = buf[(std::size_t) i];
                if (b < a21)
                    b = 0.0f;
                if (b > 0.0f)
                    b = a20;
                buf[(std::size_t) i] = b;
            }
        else
        {
            const auto k = a20 * 2.0f;
            for (int i = 0; i < n; ++i)
            {
                auto b = std::max (buf[(std::size_t) i] + -a21, 0.0f);
                if (! fl.rhz)
                {
                    const auto q = v.hz[(std::size_t) i] * kHz + 1.0f;
                    if (b > 0.0f && q > 0.0f)
                        b = b / q;
                }
                buf[(std::size_t) i] = (b * b) * k;
            }
        }
        if (fl.rmask.flag)
        {
            const auto mS = (float) fl.rmask.S, mO = (float) fl.rmask.o;
            for (int i = 0; i < n; ++i)
            {
                const auto Mv = tableRead (fl.rmask, v.l2[(std::size_t) i] * mS + mO, P->prec);
                T[i] = (buf[(std::size_t) i] * Mv - Mv) * T[i] + T[i];
            }
        }
        else
            for (int i = 0; i < n; ++i)
                T[i] = T[i] * buf[(std::size_t) i];
        v.nzPos = v.nzPos + v.nzRate;
        if (v.nzPos >= 1.0)
        {
            v.nzPos = v.nzPos - std::trunc (v.nzPos);
            std::uint32_t u8 = (v.nzSeed - 0x2036au) ^ 0xcu;
            std::uint32_t u10 = (0u - u8 - 0x12c58u) ^ (u8 << 8);
            std::uint32_t u12 = (0x197e1u - u8 - u10) ^ (u10 >> 13);
            u8 = (u8 - u10 - u12) ^ (u12 >> 12);
            u10 = (u10 - u12 - u8) ^ (u8 << 16);
            u12 = (u12 - u8 - u10) ^ (u10 >> 5);
            u8 = (u8 - u10 - u12) ^ (u12 >> 3);
            u10 = (u10 - u12 - u8) ^ (u8 << 10);
            v.nzSeed = (u12 - u8 - u10) ^ (u10 >> 15);
        }
    }

    void harmonizer (Record& v, const Part& p, const double* A, float* T) noexcept
    {
        const auto& pp = p.p;
        if (pp[27] <= 0)
            return;
        auto mx = f32 (f32 (pp[27] * milli) * A[19]);
        if (mx >= 1.0)
            mx = 1.0;
        if (mx <= 0.0)
            return;
        auto w = f32 (p.hzW * A[20]);
        if (w >= 1.0)
            w = 1.0;
        if (w <= 0.0)
            return;
        const auto sig = f32 (1.0 / f32 (f32 (w * 8.0) + 1.0));
        auto c = f32 (1.0 - sig);
        const long long so = pp[30], ss = pp[31], go = pp[32], gs = pp[33];
        const long long ms = ss < 0 ? 1 - ss : 1, as = ss < 0 ? 0 : ss;
        const long long mg = gs < 0 ? 1 - gs : 1, ag = gs < 0 ? 0 : gs;
        long long qs = 2, qg = 2;
        const int n = v.n - 4;
        if (n <= 0)
            return;
        float* base = T + 4;
        auto& snap = tmpA;
        std::copy (base, base + n, snap.begin());
        float tot = 1.0f;
        int k = 0;
        while (c > 0.0 && k < 9)
        {
            auto t = ! p.b69 ? f32 (f32 (f32 (c * c) * c) * p.hzS) : f32 (f32 (c * c) * p.hzS);
            if (t >= 1.0)
                t = 1.0;
            const auto g = f32 (f32 (f32 (t * mx) * mx) * p.hzDist[(std::size_t) k]);
            tot = tot + (float) g;
            const auto st = qs + so, step = qg + go;
            const long long cnt = step != 0 ? (long long) std::trunc ((double) (n - st - 1) / (double) step) + 1 : 0;
            if (cnt > 0 && st < n && g >= 5.960464477539063e-08)
            {
                const auto gf = (float) g;
                for (long long j = 0; j < cnt && j < n; ++j)
                {
                    auto idx = st + step * j;
                    if (idx < 0)
                        idx += n;
                    if (idx < 0 || idx >= n)
                        continue;
                    base[idx] = snap[(std::size_t) j] * gf + base[idx];
                }
            }
            qs = qs * ms + as;
            qg = qg * mg + ag;
            ++k;
            c = f32 (c - sig);
        }
        if (p.b69)
            tot = (float) std::sqrt ((double) tot);
        const auto inv = (float) f32 (1.0 / tot);
        if (! p.hasHmMask)
            for (int i = 0; i < n; ++i)
                base[i] = base[i] * inv;
        else
            for (int i = 0; i < n; ++i) // FUN_00882980
                base[i] = (base[i] * inv - snap[(std::size_t) i]) * p.hmMask[(std::size_t) i] + snap[(std::size_t) i];
    }

    // FUN_00888da0: per-partial pluck decay exp(table·f5); with the pluck blend exp(f5 / mix of the DLL's pluck table and
    // timbre 2), the mix being the timbre-mix value (at most 1)
    static void pluckRates (const Part& p, const double* A, int n, float f5, float* out) noexcept
    {
        if (p.blend != 5)
        {
            for (int i = 0; i < n; ++i)
                out[i] = (float) fastExp ((double) (p.pkTab[(std::size_t) i] * f5));
            return;
        }
        auto m = (float) f32 (p.tmix * A[3]);
        if (m >= 1.0f)
            m = 1.0f;
        const auto w = 1.0f - m;
        for (int i = 0; i < n; ++i)
        {
            const auto t = (p.pkTabDll[(std::size_t) i] - p.T2[(std::size_t) i]) * w + p.T2[(std::size_t) i];
            out[i] = (float) std::exp ((double) (f5 / t));
        }
    }

    void pluck (Record& v, const Part& p, const double* A, float* T) noexcept
    {
        const int n = v.n;
        const auto vv = f32 ((double) p.p[75] * 0.001 + A[15]);
        if (vv < 1.0)
            v.pluckOn = true;
        if (! v.pluckOn)
            return;
        if (v.pkC >= 0)
        {
            if (vv != v.pkPrev)
            {
                v.pkPrev = vv;
                v.pkT = vv < 1.0 ? f32 (std::exp (f32 (f32 (vv - 1.0) * 17.0) * ln2) * 20000.0) : -1.0;
                if (v.pkT < 0.0)
                    v.pkRate.fill (1.0f);
                else
                {
                    const auto f5 = (float) f32 (p.pkBase / (v.pkT + 0.25));
                    pluckRates (p, A, n, f5, v.pkRate.data());
                }
                if (v.pkIc < n)
                {
                    for (int i = v.pkIc; i < n; ++i)
                        v.pkLev[(std::size_t) i] = v.pkC < 1 ? 1.0f : (float) std::pow ((double) v.pkRate[(std::size_t) i], v.pkC);
                    v.pkIc = n;
                }
            }
            for (int i = 0; i < n; ++i)
            {
                T[i] = T[i] * v.pkLev[(std::size_t) i];
                v.pkLev[(std::size_t) i] = v.pkLev[(std::size_t) i] * v.pkRate[(std::size_t) i];
            }
        }
        v.pkC += 1;
    }

    void phaser (Record& v, const Part& p, const double* A, float* T) noexcept
    {
        const auto& pp = p.p;
        if (pp[76] <= 0 || ! p.phOn)
            return;
        auto mx = f32 (f32 (pp[76] * milli) * A[16]);
        if (mx >= 1.0)
            mx = 1.0;
        if (mx <= 0.0)
            return;
        const int n = v.n;
        const auto key = (double) (((float) v.l2f0 * (float) pp[81]) * 0.001f);
        const auto f18 = (float) p.phF18, f1c = (float) p.phF1c;
        auto base = f32 ((double) ((float) f32 (f32 ((double) ((float) -pp[79] * 0.001f)) - A[18]) * f18) - (double) ((float) v.phph * f1c));
        const auto sc = pp[77];
        double k = 0.0;
        hzarr (v);
        const float* x = nullptr;
        if (sc == 0)
        {
            x = v.l2.data();
            k = std::exp (f32 (f32 (f32 (p.phW - 1.0) * 8.0) + f32 (A[17] * 2.0)) * ln2) * 12.0 * (double) f1c;
            base = base - key * k;
        }
        else if (sc == 1)
        {
            x = v.hz.data();
            k = std::exp (f32 (f32 (f32 (p.phW - 1.0) + A[17]) * 8.0) * ln2) * 0.005 * (double) f1c;
            base = base - (double) ((float) key * f18);
        }
        else
        {
            x = v.hz.data();
            k = (double) ((float) f32 (2.0 - f32 (p.phW + A[17])) * f1c) / v.f0;
            base = base - (double) ((float) key * f18);
        }
        const auto N = (long long) p.phTab.size();
        const auto kf = (float) k, bf = (float) base;
        const auto* tab = p.phTab.data();
        const bool freq = p.phCode == 209;
        const auto e3 = (float) f32 (f32 (mx * mx) * mx);
        const auto mxF = (float) mx, one = (float) f32 (1.0 - mx);
        for (int i = 0; i < n; ++i)
        {
            const auto idx = x[i] * kf + bf;
            float Pv = 0.0f;
            if (P->prec >= 1)
            {
                auto y = idx - 0.5f;
                if (std::isnan (y))
                    y = 0.0f;
                else if (std::isinf (y))
                    y = y > 0.0f ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
                const auto ii = toInt64 ((double) rndf (y));
                const auto fr = idx - (float) ii;
                const auto i0 = ii & (N - 1), i1 = (i0 + 1) % N;
                Pv = tab[i0] * (1.0f - fr) + tab[i1] * fr;
            }
            else
            {
                auto y = idx;
                if (std::isnan (y))
                    y = 0.0f;
                else if (std::isinf (y))
                    y = y > 0.0f ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
                Pv = tab[toInt64 ((double) rndf (y)) & (N - 1)];
            }
            float Mv = 1.0f;
            const bool masked = p.phMask.flag;
            if (masked)
                Mv = tableRead (p.phMask, v.l2[(std::size_t) i] * (float) p.phMask.S + (float) p.phMask.o, P->prec);
            if (freq)
            {
                const auto e = masked ? (Mv * Pv) * e3 : Pv * e3;
                const auto g = P->prec >= 1 ? std::exp ((double) e * ln2) : (double) std::exp (e * (float) ln2);
                v.f[(std::size_t) i] = v.f[(std::size_t) i] * g;
            }
            else if (masked)
                T[i] = ((Pv * Mv) * mxF - Mv * mxF) * T[i] + T[i]; // FUN_00882900
            else
                T[i] = (Pv * mxF + one) * T[i];
        }
        if (freq)
        {
            // frequency phaser: f ·= 2^(table·mix³); everything is recomputed in the next block
            v.hzdirty = true;
            v.centsValid = false;
            v.forceChanged = true;
        }
    }

    void clip (Record& v, const Part& p, const double* A, float* T) noexcept
    {
        const int n = v.n, m = p.clipMode;
        const auto x = (float) f32 (p.lvl248 * A[21]);
        if (m == 0)
        {
            if (! (x < 1.0f))
                return;
        }
        else if (! (x > 0.0f))
            return;
        const auto x2 = x * x, q = x2 * x2;
        auto curve = [&] (float* Y, int count, auto tbAt) {
            if (m == 0)
            {
                const auto qq = std::max (q, 2.3283064365386963e-10f);
                for (int i = 0; i < count; ++i)
                    Y[i] = std::min (tbAt (i) * qq, Y[i]);
            }
            else if (m == 1 || m == 2)
            {
                const auto pinv = 1.0f / std::max (q, 5.960464477539063e-08f);
                for (int i = 0; i < count; ++i)
                {
                    const auto xx = std::min ((Y[i] * pinv) * tbAt (i), 1.0f);
                    auto y = 1.0f - xx;
                    y = y * y;
                    if (m == 1)
                        y = y * y, y = y * y, y = y * y;
                    Y[i] = Y[i] - y * Y[i];
                }
            }
            else if (m == 3)
            {
                for (int i = 0; i < count; ++i)
                    Y[i] = (tbAt (i) * q) < Y[i] ? Y[i] : 0.0f;
            }
            else
                for (int i = 0; i < count; ++i)
                    Y[i] = std::max (Y[i] - tbAt (i) * q, 0.0f);
        };
        if (p.clipC == 0)
        {
            const int s = p.clipSt, c = std::min (n - s, p.clipCnt);
            if (c <= 0)
                return;
            curve (T + s, c, [&] (int i) { return p.clipTab[(std::size_t) (s + i)]; });
        }
        else if (p.clipC == 1)
            return;
        else
        {
            auto cc = q * p.clipConst;
            if (m == 0)
                for (int i = 0; i < n; ++i)
                    T[i] = std::min (T[i], cc);
            else if (m == 1 || m == 2)
            {
                if (cc <= 5.960464477539063e-08f)
                    cc = 5.960464477539063e-08f;
                const auto pinv = 1.0f / cc;
                for (int i = 0; i < n; ++i)
                {
                    const auto xx = std::min (T[i] * pinv, 1.0f);
                    auto y = 1.0f - xx;
                    y = y * y;
                    if (m == 1)
                        y = y * y, y = y * y, y = y * y;
                    T[i] = T[i] - y * T[i];
                }
            }
            else if (m == 3)
            {
                for (int i = 0; i < n; ++i)
                    T[i] = cc < T[i] ? T[i] : 0.0f;
            }
            else
                for (int i = 0; i < n; ++i)
                    T[i] = std::max (T[i] - cc, 0.0f);
        }
    }

    // ---- the bank and the host-block render of a record ----

    // Σ a_i[j]·sin(2π(φ_i + j·f_i)) over one segment of m samples, amplitudes gliding to the targets
    // (a_i[j] = T + (a − T)·k_i^j). Each sine runs as a double-precision two-term recurrence: a new frequency keeps the
    // running phase (only the step changes) and every 8192 samples it is restarted from the partial's phase
    // accumulator (as the DLL resyncs its own), so it equals the direct sine of the model to ~1e-12.
    void renderBank (Record& v, int m, double* out) noexcept
    {
        const int n = v.n;
        int top = n;
        while (top > 0 && v.a[(std::size_t) top - 1] == 0.0f && v.T[(std::size_t) top - 1] == 0.0f)
            --top;
        top = std::min ((top + 7) & ~7, bankSize);
        const bool resync = v.oscAge + m > 8192;
        if (resync)
            v.oscAge = 0;
        constexpr double twoPi = 2.0 * pi;
        // new steps e^{2πi·f}: along an arithmetic series of frequencies (the harmonics without per-partial
        // modulation) by complex rotation of the previous partial's step, otherwise directly
        double rc = 1.0, rs = 0.0, rf = 0.0;
        bool rot = false;
        for (int i = 0; i < top; ++i)
        {
            const auto u = (std::size_t) i;
            const auto f = v.f[u];
            if (! (resync || i >= v.oscTop || f != v.oscF[u]))
                continue;
            double cw = 0.0, sw = 0.0;
            bool have = false;
            if (i >= 6 && v.oscF[u - 1] == v.f[u - 1])
            {
                const auto d = f - v.f[u - 1];
                if (std::fabs (d - (v.f[u - 1] - v.f[u - 2])) <= 1e-13 * std::fabs (f))
                {
                    if (! rot || std::fabs (d - rf) > 1e-13 * std::fabs (d))
                        rc = std::cos (twoPi * d), rs = std::sin (twoPi * d), rf = d, rot = true;
                    const auto pc = 0.5 * v.oscC[u - 1], ps = v.oscS[u - 1];
                    cw = pc * rc - ps * rs, sw = pc * rs + ps * rc;
                    have = true;
                }
            }
            if (! have)
                cw = std::cos (twoPi * f), sw = std::sin (twoPi * f);
            if (! resync && i < v.oscTop && std::fabs (v.oscS[u]) > 1.0e-3)
            {
                // new frequency, same phase: cos θ from the running pair, then the pair for the new step
                const auto x = v.osc0[u];
                const auto cosTheta = (x * (0.5 * v.oscC[u]) - v.osc1[u]) / v.oscS[u];
                v.osc1[u] = x * cw - cosTheta * sw;
            }
            else
            {
                const auto ph = v.ph[u];
                v.osc0[u] = std::sin (twoPi * ph);
                v.osc1[u] = std::sin (twoPi * (ph - f));
            }
            v.oscC[u] = 2.0 * cw;
            v.oscS[u] = sw;
            v.oscF[u] = f;
        }
        v.oscTop = top;
        v.oscAge += m;
        if (top > 0)
        {
            std::fill (laneSums.begin(), laneSums.begin() + 4 * m, 0.0);
            auto* acc = laneSums.data();
            bankKernel (v.osc0.data(), v.osc1.data(), v.oscC.data(), v.T.data(), v.a.data(), v.kg.data(), top, m, acc);
            for (int j = 0; j < m; ++j)
            {
                const auto* a = acc + 4 * j;
                out[j] = (a[0] + a[2]) + (a[1] + a[3]);
            }
        }
        else
            std::fill (out, out + m, 0.0);
        for (int i = 0; i < n; ++i)
        {
            const auto u = (std::size_t) i;
            const auto t = (double) v.T[u];
            if (v.a[u] != 0.0f || t != 0.0)
            {
                if (v.kmK[u] != v.kg[u] || v.kmM[u] != m) // k^m per partial, cached
                    v.kmK[u] = v.kg[u], v.kmM[u] = m, v.km[u] = powInt ((double) v.kg[u], m);
                v.a[u] = (float) (t + ((double) v.a[u] - t) * v.km[u]);
            }
            v.ph[u] = v.ph[u] + v.f[u] * m;
        }
    }

    static void panlaw (double pan, double g, double& l, double& r) noexcept
    {
        if (pan == 0.0)
        {
            l = r = g;
            return;
        }
        if (g == 0.0)
        {
            l = r = 0.0;
            return;
        }
        const auto gg = f32 (g * f32 (1.4142135381698608));
        const auto t = (1.0 + pan) * 0.7853981633974483;
        l = f32 (std::cos (t) * gg), r = f32 (std::sin (t) * gg);
    }

    void renderRecord (Record& v, int n) noexcept
    {
        const auto& p = P->parts[(std::size_t) v.part];
        const auto& R = run[(std::size_t) v.part];
        v.vph = R.vibat;
        v.phph = f32 (R.phat);
        const auto gbase = f32 (f32 (p.gain * (double) ((float) v.fvol + v.gy[1])) * v.uGain);
        const auto panbase = f32 ((double) ((float) v.fpan + v.gy[0]) + v.uPan);
        const auto& dc = P->declick;
        const bool trem = p.tremD != 0.0;
        int t = 0;
        while (t < n)
        {
            if (v.cnt <= 0)
            {
                v.vibph = v.vph;
                control (v);
                v.cnt = P->B;
                if (v.firstlen >= 0)
                {
                    v.cnt = v.firstlen != 0 ? v.firstlen : P->B;
                    v.firstlen = -1;
                }
            }
            const auto m = std::min (v.cnt, n - t);
            v.cnt -= m;
            auto* y = mono.data();
            renderBank (v, m, y);
            v.vph = (double) ((float) v.vph + (float) p.vibRate * (float) m);
            v.phph = f32 (v.phph + p.phRate * m);
            if (v.dpos >= 0)
            {
                for (int j = 0; j < m; ++j)
                {
                    const auto idx = v.dpos + j;
                    y[j] = y[j] * (idx < (int) dc.size() ? (double) dc[(std::size_t) idx] : 0.0);
                }
                v.dpos += m;
            }
            // output gains: pan and FX send from the articulators, 15 ms one-pole ramps per sample
            const auto pan = f32 (std::min (std::max (f32 (panbase + f32 (v.artpan * 4.0)), -1.0), 1.0));
            const auto gdry = f32 (gbase * f32 (1.0 - std::fabs (v.fxv))), gfx = f32 (gbase * v.fxv);
            double tgt[4];
            panlaw (pan, gdry, tgt[0], tgt[1]);
            panlaw (pan, gfx, tgt[2], tgt[3]);
            if (! v.curValid)
            {
                for (int c = 0; c < 4; ++c)
                    v.cur[(std::size_t) c] = tgt[c];
                v.curValid = true;
            }
            bool near = true;
            for (int c = 0; c < 4; ++c)
                near = near && std::fabs (v.cur[(std::size_t) c] - tgt[c]) < 5.960464477539063e-08;
            if (near)
                for (int c = 0; c < 4; ++c)
                    v.cur[(std::size_t) c] = tgt[c];
            for (int c = 0; c < 4; ++c)
            {
                auto* gg = gains.data() + c * maxHostBlock;
                if (v.cur[(std::size_t) c] == tgt[c])
                    std::fill (gg, gg + m, tgt[c]);
                else
                {
                    auto d = v.cur[(std::size_t) c] - tgt[c];
                    for (int j = 0; j < m; ++j)
                    {
                        d = f32 (d * P->coef);
                        gg[j] = f32 (d + tgt[c]);
                    }
                    v.cur[(std::size_t) c] = f32 (d + tgt[c]);
                }
            }
            const auto* g0 = gains.data();
            const auto* g1 = g0 + maxHostBlock;
            const auto* g2 = g1 + maxHostBlock;
            const auto* g3p = g2 + maxHostBlock;
            // dry and FX-send outputs: summed (no rack) or kept apart for the rack, where the FX send joins at unit 4
            auto add = [&] (int j, double yl, double yr) {
                const auto at = (std::size_t) (t + j);
                if (split)
                {
                    mixL[at] += yl * g0[j], mixR[at] += yr * g1[j];
                    fxL[at] += yl * g2[j], fxR[at] += yr * g3p[j];
                }
                else
                {
                    mixL[at] += yl * (g0[j] + g2[j]);
                    mixR[at] += yr * (g1[j] + g3p[j]);
                }
            };
            if (trem)
            {
                // the record fades the part tremolo in over its first 0.1 s
                if (v.e8 > 0)
                {
                    const auto r0 = 1.0f - (float) v.e8 * P->kfade;
                    for (int j = 0; j < m; ++j)
                    {
                        const auto r = std::min (r0 + (float) j * P->kfade, 1.0f);
                        add (j, y[j] * (double) (r * R.TL[(std::size_t) (t + j)] + (1.0f - r)),
                             y[j] * (double) (r * R.TR[(std::size_t) (t + j)] + (1.0f - r)));
                    }
                    v.e8 -= m;
                }
                else
                    for (int j = 0; j < m; ++j)
                        add (j, y[j] * (double) R.TL[(std::size_t) (t + j)], y[j] * (double) R.TR[(std::size_t) (t + j)]);
            }
            else
                for (int j = 0; j < m; ++j)
                    add (j, y[j], y[j]);
            t += m;
        }
    }

    // part tremolo for this host block: g = 0.21 − (0.55·cos(2π·r·i + θ) + 0.55)², gain 1 + g·depth (depth ramped)
    void tremolo (int pk, int n, bool rendering = true) noexcept
    {
        const auto& p = P->parts[(std::size_t) pk];
        auto& R = run[(std::size_t) pk];
        if (p.tremD == 0.0)
            return;
        bool alive = false;
        if (rendering)
            for (int i = 0; i < liveCount && ! alive; ++i)
                alive = records[(std::size_t) live[(std::size_t) i]].part == pk;
        if (! alive)
            R.tremCur = p.tremD;
        if (rendering)
        {
            const float tp = 6.28318548f;
            auto th0 = R.tremPh * tp;
            if (th0 >= tp)
                th0 = th0 - tp;
            auto th1 = (R.tremPh * tp) + (float) f32 (p.tremGap * 2.0);
            if (th1 >= tp)
                th1 = th1 - tp;
            double d = 0.0;
            for (int side = 0; side < 2; ++side)
            {
                auto& arr = side == 0 ? R.TL : R.TR;
                const auto th = (double) (side == 0 ? th0 : th1);
                d = R.tremCur - p.tremD;
                const auto w = 2.0 * pi * p.tremRate;
                for (int i = 0; i < n; ++i)
                {
                    const auto c = (float) (0.55 * std::cos (w * i + th));
                    auto g = c + 0.55f;
                    g = g * g;
                    g = 0.21f - g;
                    d = f32 (d * P->coef);
                    const auto dd = (float) f32 (d + p.tremD);
                    arr[(std::size_t) i] = 1.0f + g * dd;
                }
            }
            R.tremCur = f32 (d + p.tremD);
        }
        R.tremPh = R.tremPh + (float) p.tremRate * (float) n;
        if (std::fabs (R.tremPh) >= 1.0f)
            R.tremPh = (float) std::fmod ((double) R.tremPh, 1.0);
    }

    // master volume (global 12, post FX): stereo gain with a 15 ms ramp from 1.0 (the value at state load)
    void masterVolume (int n) noexcept
    {
        const auto tg = P->masterTarget;
        if (std::fabs (f32 (masterCur - tg)) < 5.960464477539063e-08)
            masterCur = tg;
        if (masterCur == tg)
        {
            if (tg != 1.0)
                for (int j = 0; j < n; ++j)
                {
                    mixL[(std::size_t) j] = (double) ((float) mixL[(std::size_t) j] * (float) tg);
                    mixR[(std::size_t) j] = (double) ((float) mixR[(std::size_t) j] * (float) tg);
                }
            return;
        }
        auto d = f32 (masterCur - tg);
        double g = masterCur;
        for (int j = 0; j < n; ++j)
        {
            d = f32 (d * P->coef);
            g = f32 (d + tg);
            mixL[(std::size_t) j] = (double) ((float) mixL[(std::size_t) j] * (float) g);
            mixR[(std::size_t) j] = (double) ((float) mixR[(std::size_t) j] * (float) g);
        }
        masterCur = g;
    }

    void rampMaster (int n) noexcept
    {
        const auto tg = P->masterTarget;
        if (std::fabs (f32 (masterCur - tg)) < 5.960464477539063e-08)
            masterCur = tg;
        if (masterCur == tg)
            return;
        auto d = f32 (masterCur - tg);
        for (int j = 0; j < n; ++j)
            d = f32 (d * P->coef);
        masterCur = f32 (d + tg);
    }

    // plugin-wide phases at the next host-block start: global LFOs, phaser, vibrato
    void advance (int n) noexcept
    {
        for (int k = 0; k < 2; ++k)
        {
            if (! P->present[(std::size_t) k])
                continue;
            const auto& p = P->parts[(std::size_t) k];
            auto& R = run[(std::size_t) k];
            for (const auto& a : p.arts)
                if (a.lfoOn && a.lfoGlobal)
                    R.gl[(std::size_t) a.k] = (std::uint32_t) ((long long) R.gl[(std::size_t) a.k] + rndi (((double) n / P->B) * a.lfoInc98));
            auto ph = (R.phat + p.phRate * n) * 0.25;
            if (std::fabs (ph) >= 1.0)
                ph = ph - std::trunc (ph);
            R.phat = ph * 4.0;
            R.vibat = R.vibat + (float) p.vibRate * (float) n;
            R.vibat = R.vibat - (float) rnd ((double) R.vibat);
        }
        now += n;
    }
};

constexpr std::uint32_t fixedSeed = 0x6A09E667u; // LYNX's per-instance Mersenne-Twister seed (FL seeds from the clock)

// ---------------------------------------------------------------------------------------------------------------
// LYNX front-end
//
// Levels (§1.17, REA_rodada28 §4.1). FL hands the plugin Final.Vol = chan.Vol (Harmor's velocity reaches the sound
// through its articulators' velocity maps), Final.Pan = note pan + chan.Pan and Final.Pitch = 100·(key − 60) + channel
// pitch; Harmor applies its own record gain g_part·(Final.Vol + offset)·g_unison and pan law √2·g·cos/sin((1 + pan)π/4).
// LYNX's channel gains are gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…), so V = √((gl² + gr²)/2) and p are recovered
// exactly and Harmor's law is applied once (the channel volume and pan are not applied a second time). Velocity = LYNX
// velocity (FL velocity / 128); X/Y/Z (Final.FCut/FRes, global mod knobs) as in the state.
// Random sources: the plugin's Mersenne Twister is seeded with a fixed value per instance, so every render is
// deterministic; FL's is clock-seeded, so random unison, free/spectral phases and random maps differ from any given
// FL render by design. Tempo-synced times (articulator tempo envelopes/LFOs, phaser rate, portamento) use the song
// tempo (the reference was measured at 120 BPM).

class HarmorInstrument final : public Instrument
{
public:
    HarmorInstrument() { engine.seedRandom (fixedSeed); }

    ~HarmorInstrument() override = default;

    void prepare (double sr) noexcept override
    {
        rate = sr;
        preparedRate.store (sr);
        std::shared_ptr<const Patch> next;
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (hasPending.exchange (false))
                current = std::move (pending), pending.reset();
            if (current != nullptr && current->fs != sr && ! state.empty())
                if (auto S = parseState (state))
                    current = buildPatch (*S, sr, bpm, compState);
            retired.reset();
        }
        engine.setPoolSize (current.get());
        engine.setPatch (current.get());
        engine.seedRandom (fixedSeed);
    }

    void configure (const ChannelDef& channel, double songBpm) noexcept override
    {
        const auto b = songBpm > 0.0 ? songBpm : 120.0;
        pitchCents.store (channel.sampler.pitch * 100.0f);
        const std::lock_guard<std::mutex> lock (mutex);
        retired.reset();
        // the rack compressor's Maximus state (FlpImport: modelParams = size, bytes…)
        std::vector<std::uint8_t> comp;
        const auto& mp = channel.modelParams;
        if (! mp.empty())
        {
            const auto count = (std::size_t) std::max (0.0f, mp[0]);
            if (count > 0 && mp.size() >= count + 1)
            {
                comp.resize (count);
                for (std::size_t k = 0; k < count; ++k)
                    comp[k] = (std::uint8_t) std::clamp ((int) mp[k + 1], 0, 255);
            }
        }
        if (channel.pluginState == state && b == bpm && comp == compState)
            return;
        state = channel.pluginState;
        compState = std::move (comp);
        bpm = b;
        std::shared_ptr<const Patch> built;
        try
        {
            if (auto S = parseState (state))
                built = buildPatch (*S, preparedRate.load(), bpm, compState);
        }
        catch (...)
        {
            built.reset();
        }
        pending = std::move (built);
        hasPending.store (true);
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
        const auto fpitch = 100.0 * key - 6000.0 + (double) pitchCents.load() + (double) nextCents;
        engine.noteOn (key, vel, channelVol, (double) nextPan + channelPan, fpitch, (double) nextPan);
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (const auto i = engine.findPressed (key); i >= 0)
            engine.noteOff (i);
    }

    void allNotesOff() noexcept override { engine.allNotesOff(); }

    bool idle() const noexcept override { return engine.idle() && engine.rackQuiet(); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / pi) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0; // (equal gains: exactly centre, as FL's Final.Pan)
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            engine.setChannelLevels (vol, pan);
        }
        for (int done = 0; done < n; done += Engine::maxHostBlock)
        {
            const auto m = std::min (Engine::maxHostBlock, n - done);
            if (engine.idle() && engine.rackQuiet())
                engine.skip (m); // nothing sounds: only the plugin-wide phases and ramps run
            else
                engine.render (left + done, right + done, m);
        }
    }

private:
    // a patch built by configure() replaces the playing one (the old one is released on the UI thread)
    void take() noexcept
    {
        if (! hasPending.load())
            return;
        std::unique_lock<std::mutex> lock (mutex, std::try_to_lock);
        if (! lock.owns_lock() || retired != nullptr)
            return;
        if (pending != nullptr && pending->fs != rate)
            return; // built for another rate: prepare() rebuilds it
        hasPending.store (false);
        retired = std::move (current);
        current = std::move (pending);
        pending.reset();
        engine.setPatch (current.get());
    }

    Engine engine;
    std::mutex mutex;
    std::shared_ptr<const Patch> current, pending, retired;
    std::atomic<bool> hasPending { false };
    std::atomic<double> preparedRate { 44100.0 };
    std::atomic<float> pitchCents { 0.0f };
    std::vector<std::uint8_t> state, compState;
    double bpm = 120.0, rate = 44100.0, channelVol = 1.0, channelPan = 0.0;
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

bool harmorStateUsable (const std::vector<std::uint8_t>& state)
{
    try
    {
        return parseState (state).has_value();
    }
    catch (...)
    {
        return false;
    }
}

std::vector<std::string> harmorFallbacks (const std::vector<std::uint8_t>& state)
{
    const auto S = parseState (state);
    if (! S)
        return { "state not usable" };
    return buildPatch (*S, 44100.0, 120.0)->fallbacks;
}

std::optional<std::vector<std::uint8_t>> harmorUpgradeState (const std::vector<std::uint8_t>& state)
{
    try
    {
        auto up = upgradeOldState (state);
        if (up && parseCurrent (*up))
            return up;
    }
    catch (...)
    {
    }
    return std::nullopt;
}

int harmorParamOffset (const std::vector<std::uint8_t>& state, int part, int index)
{
    const auto S = parseCurrent (state); // (offsets into this very state: an old layout has to be upgraded first)
    if (! S)
        return -1;
    if (part < 0)
        return index >= 0 && index < 59 ? (int) (S->globalOffset + 4 * (std::size_t) index) : -1; // (53…58: the rack order)
    if (part > 1 || ! S->parts[(std::size_t) part].present || index < 0 || index >= 386)
        return -1;
    return (int) (S->parts[(std::size_t) part].paramOffset + 4 * (std::size_t) index);
}

int harmorCompressorType (const std::vector<std::uint8_t>& state)
{
    const auto S = parseState (state);
    return S ? std::max (0, S->GI (52)) : 0;
}

std::unique_ptr<Instrument> makeHarmorInstrument() { return std::make_unique<HarmorInstrument>(); }

namespace harmor
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, int block, const std::vector<std::uint32_t>& mt, double bpm,
                                    const std::vector<std::uint8_t>& compressorState)
{
    const auto S = parseState (state);
    if (! S || length <= 0)
        return {};
    const auto patch = buildPatch (*S, sampleRate, bpm, compressorState);
    auto engine = std::make_unique<Engine>();
    engine->setPoolSize (patch.get());
    engine->setPatch (patch.get());
    if (mt.size() >= 625)
        engine->setMtState (mt);
    else
        engine->seedRandom (fixedSeed);
    // host blocks of `block` samples split at every event (the harness and the model do the same)
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    std::sort (evs.begin(), evs.end());
    std::vector<int> handle (notes.size(), -1);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) Engine::maxHostBlock), r (l.size());
    block = std::max (1, std::min (block, Engine::maxHostBlock));
    int n0 = 0;
    while (n0 < length)
    {
        auto ln = std::min (block, length - n0);
        for (auto e : evs)
            if (n0 < e && e < n0 + ln)
                ln = e - n0;
        for (std::size_t i = 0; i < notes.size(); ++i)
            if (notes[i].start == n0)
                handle[i] = engine->noteOn (notes[i].key, notes[i].velocity, notes[i].finalVol, notes[i].finalPan,
                                            100.0 * notes[i].key - 6000.0, notes[i].finalPan);
        for (std::size_t i = 0; i < notes.size(); ++i)
            if (notes[i].release == n0 && handle[i] >= 0)
                engine->noteOff (handle[i]);
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
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
} // namespace harmor
} // namespace lynx
