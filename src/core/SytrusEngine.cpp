// Sytrus engine (docs/analise/REA_rodada31.md). Clean-room port of our reference model: every formula and every
// float32/double rounding step follows the document (DECOMP/EXEC facts) and the model that was checked against FL's
// own DLL. Section numbers (§) refer to REA_rodada31.md.
//
// Precision conventions: the DLL works in float32 with double (x87/SSE) intermediates in places; `F(x)` marks every
// point where a value is rounded to float32, the rest is double. Python's round()/Delphi's Round are round-half-even
// (rne), int() is truncation.

#include "core/SytrusEngine.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <future>
#include <map>

#include "core/Inflate.h"

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE2, or ARM64 through core/Simd.h (sse2neon)
 #include "core/Simd.h"
#endif

// the DLL rounds every float32 product and sum separately: no fused multiply-adds in this file
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace sytrus
{
namespace
{
using u32 = std::uint32_t;
using cd = std::complex<double>;

inline float F (double x) noexcept { return (float) x; }

// round half to even without a libm call: exact for |x| < 2^51 in the default rounding mode (the magic constant
// 1.5·2^52 pushes the fraction out of the mantissa); larger values are already integers
inline double rne (double x) noexcept
{
    if (! (std::abs (x) < 2251799813685248.0))
        return x;
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE2, or ARM64 through core/Simd.h (sse2neon)
    return (double) _mm_cvtsd_si64 (_mm_set_sd (x)); // cvtsd2si: the MXCSR mode, round-to-nearest-even in LYNX and FL
#else
    volatile double big = 6755399441055744.0; // not folded by the optimiser
    const double b = big;
    return (x + b) - b;
#endif
}
inline long long rnd (double x) noexcept { return (long long) rne (x); }

const double kLn2_1200 = std::log (2.0) / 1200.0;
const float kLn11 = (float) std::log (11.0);
const double kLn1000001 = std::log (1000001.0);
constexpr double kPi = 3.141592653589793;
constexpr float kTiny = 5.960464477539063e-08f; // 2^-24

// matrix / level curve sign(v)·(11^(|v|/full) − 1)/10, exactly ±1 at ±full (FUN_00864ff0)
float gCurve (int v, int full = 256) noexcept
{
    const auto av = std::abs (v);
    const float x = full == 256 ? F ((float) av * F (1.0 / 256)) : F ((double) av / full);
    float g;
    if (x == 1.0f)
        g = 1.0f;
    else
        g = F ((std::exp ((double) F (x * kLn11)) - 1.0) * (double) F (0.1));
    return v >= 0 ? g : -g;
}

// ------------------------------------------------------------------------------------------------------------------
// state (REA_rodada11 §1 layout, corrected in §0 of round 31)

struct EnvRaw
{
    struct Pt
    {
        double dt = 0.0, y = 0.0;
        float t = 0.0f;
        std::uint8_t fl[4] {};
    };
    int kind = 0, ver = 0;
    std::vector<Pt> pts;
    int flags = 0, on = 0, decay = -1, loop = -1, sus = -1;
};

struct ArtRaw
{
    int v = 0;
    std::int32_t fl = 0;
    float envdepth = 0.0f, lfodepth = 0.0f;
    EnvRaw env, lfo;
    int knobs[4] {};
    int lfoP[4] {}; // speed, tension, skew, pulse width
    int lfo5 = 0;
    std::array<EnvRaw, 6> maps;
};

struct UnitRaw
{
    int p[10] {};
    std::vector<std::uint8_t> op, f;
    EnvRaw shape;
    int fbits = -1;
    std::vector<ArtRaw> art;
};

struct Reader
{
    const std::uint8_t* b;
    std::size_t n, i = 0;
    bool ok = true;

    bool need (std::size_t k) noexcept
    {
        if (! ok || i + k > n)
            return ok = false;
        return true;
    }
    std::uint8_t u8() noexcept { return need (1) ? b[i++] : 0; }
    std::int32_t i32() noexcept
    {
        std::int32_t v = 0;
        if (need (4))
            std::memcpy (&v, b + i, 4), i += 4;
        return v;
    }
    float f32() noexcept
    {
        float v = 0;
        if (need (4))
            std::memcpy (&v, b + i, 4), i += 4;
        return v;
    }
    double f64() noexcept
    {
        double v = 0;
        if (need (8))
            std::memcpy (&v, b + i, 8), i += 8;
        return v;
    }
    std::vector<std::uint8_t> raw (std::size_t k)
    {
        if (! need (k))
            return {};
        std::vector<std::uint8_t> v (b + i, b + i + k);
        i += k;
        return v;
    }
};

bool readEnv (Reader& r, EnvRaw& e)
{
    e.kind = r.i32();
    e.ver = r.i32();
    const auto n = r.i32();
    if (! r.ok || n < 0 || n >= 5000 || e.ver < 0 || e.ver > 3)
        return r.ok = false;
    e.pts.resize ((std::size_t) n);
    for (auto& p : e.pts)
    {
        if (e.ver < 2)
        {
            p.dt = e.kind == 6 ? r.f64() : (double) r.f32();
            p.y = r.f32();
            r.raw (8 - (e.ver < 1 ? 4 : 0));
        }
        else
        {
            p.dt = r.f64();
            p.y = r.f64();
            p.t = r.f32();
            for (auto& f : p.fl)
                f = r.u8();
        }
    }
    e.flags = r.i32();
    e.on = r.i32();
    e.decay = r.i32();
    e.loop = r.i32();
    e.sus = r.i32();
    return r.ok;
}

bool readArt (Reader& r, ArtRaw& a)
{
    a.v = r.i32();
    a.fl = r.i32();
    a.envdepth = r.f32();
    a.lfodepth = r.f32();
    if (! readEnv (r, a.env))
        return false;
    for (auto& k : a.knobs)
        k = r.i32();
    if (! readEnv (r, a.lfo))
        return false;
    for (auto& k : a.lfoP)
        k = r.i32();
    a.lfo5 = a.lfo.ver >= 3 ? r.i32() : 0;
    for (auto& m : a.maps)
        if (! readEnv (r, m))
            return false;
    return r.ok;
}

constexpr int kArts[10] { 6, 6, 6, 6, 6, 6, 8, 8, 8, 2 };
constexpr int kArtBase[10] { 0, 6, 12, 18, 24, 30, 36, 44, 52, 60 };
constexpr int kNumArts = 62;

struct StateRaw
{
    std::uint32_t v0 = 0;
    int ver = 0;
    std::vector<std::uint8_t> main;
    int p4f0 = 0;
    int b4f4 = 0, b4f5 = 0, b4f6 = 0;
    std::vector<std::uint8_t> b4f7;
    std::array<std::vector<std::uint8_t>, 3> delay;
    std::vector<std::uint8_t> rev;
    int eqmode = 0;
    std::vector<std::array<int, 4>> eq;
    std::array<UnitRaw, 10> units;
    int b5f18 = 0;

    int mget (int i) const noexcept
    {
        std::int32_t v = 0;
        if ((std::size_t) (4 * i + 4) <= main.size())
            std::memcpy (&v, main.data() + 4 * i, 4);
        return v;
    }
    // matrix cell: layer 0 FM, 1 RM; row = destination, column = source; muted cells read as 0
    int cell (int layer, int row, int col) const noexcept
    {
        const auto i = layer * 81 + row * 9 + col;
        if (ver >= 2 && (std::size_t) (0x320 + i) < main.size() && main[(std::size_t) (0x320 + i)] != 0)
            return 0;
        return mget (3 + i);
    }
    int b4f7at (std::size_t i) const noexcept { return i < b4f7.size() ? b4f7[i] : 0; }
};

int opInt (const UnitRaw& u, int off) noexcept
{
    std::int32_t v = 0;
    if ((std::size_t) off + 4 <= u.op.size())
        std::memcpy (&v, u.op.data() + off, 4);
    return v;
}

bool parseState (const std::vector<std::uint8_t>& blob, StateRaw& S)
{
    if (blob.size() < 8)
        return false;
    std::memcpy (&S.v0, blob.data(), 4);
    const auto v = (int) (S.v0 > 0xffff ? S.v0 >> 16 : S.v0);
    S.ver = v;
    if (v < 0x11 || v > 64) // operator block layout of version ≥ 17 (all FL 20+ states)
        return false;
    auto body = inflateZlib (blob.data() + 4, blob.size() - 4);
    if (! body)
        return false;
    Reader r { body->data(), body->size() };
    S.main = r.raw (962);
    S.p4f0 = r.i32();
    S.b4f4 = r.u8();
    S.b4f5 = r.u8();
    S.b4f6 = v > 12 ? r.u8() : 0;
    S.b4f7 = r.raw (1 + (v > 14 ? 4 : 0) + (v > 17 ? 3 : 0) + (v > 18 ? 1 : 0));
    for (auto& d : S.delay)
        d = r.raw (23);
    S.rev = r.raw (0x25 + (v > 10 ? 1 : 0));
    S.eqmode = r.i32();
    for (int k = 0; k < 3; ++k)
    {
        std::array<int, 4> e { 0, 0, 0, k == 0 ? 0 : (k == 1 ? 1 : 2) };
        for (int j = 0; j < (v > 13 ? 4 : 3); ++j)
            e[(std::size_t) j] = r.i32();
        S.eq.push_back (e);
    }
    if (v < 0x17)
        r.raw ((std::size_t) std::max (0, r.i32()));
    else
    {
        std::size_t nn = r.u8();
        if (nn == 255)
            nn = (std::size_t) std::max (0, r.i32());
        r.raw (2 * nn);
    }
    if (! r.ok)
        return false;
    for (int u = 0; u < 10; ++u)
    {
        auto& U = S.units[(std::size_t) u];
        for (auto& p : U.p)
            p = r.i32();
        if (u < 6)
            U.op = r.raw (0x828);
        else if (u < 9)
        {
            U.f = r.raw (0x30);
            if (! readEnv (r, U.shape))
                return false;
            U.fbits = r.u8();
        }
        U.art.resize ((std::size_t) kArts[u]);
        for (auto& a : U.art)
            if (! readArt (r, a))
                return false;
    }
    if (v > 0x17)
        r.u8();
    S.b5f18 = v > 0x18 ? r.u8() : 0;
    return r.ok;
}

// ------------------------------------------------------------------------------------------------------------------
// articulator curves (§4.4, §4.6): a point list prepared once per patch

struct CPt
{
    double dt = 0, y = 0, t = 0;
    int mode = 0, arp = 0, sign = 0;
    double k = 0, ilk = 0, L = 0, p4 = 0, p5 = 0, p6 = 0, w = 0;
};

struct Curve
{
    int flags = 0;
    bool on = false;
    int decay = -1, loop = -1, sus = -1, endpt = -1;
    double offset = 0.0;
    bool clamp = true, zeroloop = false;
    std::vector<CPt> pts;

    void build (const EnvRaw& e, double off, bool clampOn)
    {
        flags = e.flags;
        on = e.on != 0;
        decay = e.decay, loop = e.loop, sus = e.sus;
        offset = off;
        clamp = clampOn;
        pts.clear();
        for (const auto& p : e.pts)
        {
            CPt c;
            c.dt = (double) F (p.dt > 0 ? std::max (p.dt, 0.0) : 0.0);
            c.y = (double) F (p.y);
            c.t = (double) F (p.t);
            c.mode = p.fl[0];
            c.arp = p.fl[1];
            pts.push_back (c);
        }
        prepare();
    }

    int count (const CPt& p) const
    {
        const auto a = (double) F (1.0 - std::abs (p.t) * 0.6);
        auto n = rnd ((double) F (std::exp (a * kLn1000001) - 1.0) * 0.001);
        if (p.mode == 3 && p.t >= 0.0)
            n += 1;
        return (int) n;
    }

    void prepare()
    {
        const auto n = (int) pts.size();
        endpt = sus;
        if (endpt < 0 && loop >= 0)
            endpt = n - 1;
        const auto D = std::max (decay, 0);
        const auto S = sus >= 0 ? sus : n - 1;
        const auto L = loop >= 0 ? loop : S;
        double A_sum = 0, M_sum = 0, R_sum = 0;
        for (int i = 0; i < n; ++i)
        {
            const auto dt = pts[(std::size_t) i].dt;
            if (i <= D)
                A_sum += dt;
            else if (i <= L)
                M_sum += dt;
            if (i > S)
                R_sum += dt;
        }
        for (int i = 0; i < n; ++i)
        {
            auto& p = pts[(std::size_t) i];
            const auto t = p.t;
            p.sign = t == 0 ? 0 : (t > 0 ? 1 : -1);
            if (i > 0 && (p.mode == 0 || p.mode == 1))
            {
                const auto e1 = F (std::exp ((double) F (std::abs (t)) * kLn1000001) - 1.0);
                const float sg = t > 0 ? 1.0f : (t < 0 ? -1.0f : 0.0f);
                p.k = (double) F (e1 * sg);
                if (p.k != 0.0)
                {
                    p.ilk = (double) F (1.0 / std::abs (p.k));
                    p.L = (double) F (std::log (std::abs (p.k) + 1.0));
                }
            }
            else if (i > 0 && p.mode >= 3 && p.mode <= 6)
            {
                const auto cnt = count (p);
                const int sg = t < 0 ? -1 : 1;
                if (p.mode == 3)
                {
                    p.p6 = (double) F (sg * 0.5);
                    p.p4 = (double) (cnt + 1);
                    p.p5 = (double) F (1.0 / (p.p4 - sg));
                }
                else if (p.mode == 4)
                {
                    p.p4 = (double) (cnt + 1);
                    p.p5 = (double) F (1.0 / p.p4);
                }
                else if (p.mode == 5)
                {
                    p.p6 = (double) F (sg * 0.5);
                    p.p4 = (double) ((cnt + 1) * 2);
                    p.p5 = (double) F (2 * kPi);
                }
                else
                {
                    p.p4 = (double) F (cnt + 0.5);
                    p.p5 = (double) F (2 * kPi);
                    if (cnt == 0 && p.sign > 0)
                        p.sign = 2;
                }
            }
            else if (i > 0 && (p.mode == 7 || p.mode == 8))
                p.p4 = t;
            else if (i > 0 && p.mode == 9)
                p.p4 = std::abs (t);
            if (i > 0 && (p.mode == 0 || p.mode == 1 || p.mode == 7 || p.mode == 10 || p.mode == 11 || p.mode == 12 || p.mode == 14)
                && t == 0.0)
                p.sign = 2;
        }
        // modulation weights
        double r = A_sum;
        for (int i = 0; i < n; ++i)
        {
            auto& p = pts[(std::size_t) i];
            double w = 0.0, rn = 0.0;
            if (i > D)
            {
                w = 1.0;
                if (i > S)
                {
                    rn = r - p.dt;
                    w = (flags & 0x80) == 0 ? rn / std::max (R_sum, 1.52587890625e-05) : 1.0;
                }
                else
                {
                    if (i <= L)
                        w = (r + p.dt) / std::max (M_sum, 1.52587890625e-05);
                    rn = r + p.dt;
                    if (i == S)
                        rn = R_sum;
                }
            }
            else
            {
                if (i == D)
                    rn = 0.0;
                else
                {
                    rn = r - p.dt;
                    w = rn / std::max (A_sum, 1.52587890625e-05);
                }
            }
            p.w = (double) F (w);
            r = rn;
        }
        zeroloop = false;
        if (endpt >= 0 && loop >= 0)
            zeroloop = (cum (endpt) - cum (loop)) <= 0.0;
    }

    double cum (int i) const noexcept
    {
        double s = 0.0;
        for (int k = 0; k <= i && k < (int) pts.size(); ++k)
            s += pts[(std::size_t) k].dt;
        return s;
    }

    double level (int i, double mod, bool clampOn) const noexcept
    {
        if (i < 0 || i >= (int) pts.size())
            return 0.0;
        auto v = pts[(std::size_t) i].y;
        if (mod != 0.0)
        {
            v = v + (double) (F (pts[(std::size_t) i].w) * F (mod));
            if (clampOn)
                v = std::min (std::max (v, 0.0), 1.0);
        }
        return v - offset;
    }
    double level (int i, double mod) const noexcept { return level (i, mod, clamp); }

    static double curveK (const CPt& p, double x) noexcept
    {
        if (p.k == 0.0)
            return x;
        if (p.k > 0)
            return 1.0 - (std::exp ((1.0 - x) * p.L) - 1.0) * p.ilk;
        return (std::exp (x * p.L) - 1.0) * p.ilk;
    }

    // fraction (0…1) of the segment ending at point i at phase ph (§4.4 curve shapes)
    double shape (int i, double ph) const noexcept
    {
        const auto& p = pts[(std::size_t) i];
        const auto m = p.mode;
        if (m == 2)
            return 0.0;
        if (m == 0)
            return curveK (p, ph);
        if (m == 1)
        {
            if (ph < 0.5)
                return curveK (p, ph * 2.0) * 0.5;
            return 1.0 - curveK (p, (1.0 - ph) * 2.0) * 0.5;
        }
        if (p.sign == 2)
            return ph;
        if (m == 3)
            return (double) rnd (ph * p.p4 - p.p6) * p.p5;
        if (m == 4)
        {
            const auto x = ph * p.p4 - 0.5;
            const auto i2 = (double) rnd (x - 0.5);
            auto y = (x - i2) * 2.0 - 1.0;
            if (p.sign < 0)
            {
                const auto z = 1.0 - std::abs (y);
                y = (1.0 - z * z * z) * (y < 0 ? -1.0 : 1.0);
            }
            else
                y = y * y * y;
            return (i2 + y * 0.5 + 1.0) * p.p5;
        }
        if (m == 5)
            return (double) (rnd (ph * p.p4 - p.p6) & 1);
        if (m == 6)
        {
            if (p.sign < 0)
                return (std::sin ((ph * p.p4 + 0.75) * p.p5) + 1.0) * 0.5;
            const auto x = ph * p.p4 - 0.5;
            const auto i2 = (double) rnd (x - 0.5);
            return std::abs ((x - i2) * 2.0 - 1.0);
        }
        if (m == 7)
        {
            const auto d = p.sign > 0 ? 1.0 - ph : ph;
            const auto d3 = d * d * d;
            return ph - (d3 * d3 - d) * p.p4;
        }
        if (m == 8)
        {
            if (ph < 0.5)
            {
                const auto x = ph * 2.0;
                const auto d = p.sign > 0 ? 1.0 - x : x;
                const auto d3 = d * d * d;
                return (x - (d3 * d3 - d) * p.p4) * 0.5;
            }
            const auto x = (1.0 - ph) * 2.0;
            const auto d = p.sign > 0 ? 1.0 - x : x;
            const auto d3 = d * d * d;
            return 1.0 - (x - (d3 * d3 - d) * p.p4) * 0.5;
        }
        if (m == 9)
        {
            const auto hp = (double) F (kPi / 2);
            const auto d = p.sign > 0 ? std::sin (ph * hp) : 1.0 - std::cos (ph * hp);
            return ph - (ph - d) * p.p4;
        }
        return ph; // modes 10…12 not modelled (absent from the demo data, §4.4)
    }

    double mapValue (double x) const noexcept
    {
        if (pts.empty())
            return 0.0 - offset;
        double t0 = 0.0;
        for (int i = 1; i < (int) pts.size(); ++i)
        {
            const auto dt = pts[(std::size_t) i].dt;
            if (x < t0 + dt && dt > 0)
            {
                const auto ph = (x - t0) / dt;
                const auto a = (double) F (level (i - 1, 0.0));
                const auto b = (double) F (level (i, 0.0) - a);
                return (double) (F (a) + F (shape (i, ph)) * F (b));
            }
            t0 += dt;
        }
        return (double) F (level ((int) pts.size() - 1, 0.0));
    }

    bool isDefault() const noexcept
    {
        if (pts.empty())
            return false;
        for (const auto& p : pts)
            if (p.y != pts[0].y)
                return false;
        return pts[0].y == (offset == 0.5 ? 0.5 : 1.0);
    }
};

// LFO oscillator (§4.5)
struct LfoWave
{
    double k = 0, ik = 0, L = 0;
    bool uni = false;
    u32 p5 = 0, p6 = 0, p7 = 0;
    double s0 = 0, s1 = 0;
    u32 startPhase = 0;

    void build (int tension, int skew, int pw, bool unipolar)
    {
        const auto a = (std::pow (21.0, std::abs (tension) / 128.0) - 1.0) * 0.05;
        const auto K = (double) F (std::exp ((double) F (a) * std::log (9.999999843067494e17)) - 1.0)
                       * (tension > 0 ? 1 : (tension < 0 ? -1 : 0));
        k = (double) F (K);
        if (k != 0.0)
        {
            ik = (double) F (1.0 / std::abs (k));
            L = (double) F (std::log (std::abs (k) + 1.0));
        }
        const auto sk = (double) F (skew / 256.0);
        const auto pwf = (double) F (pw / 128.0);
        uni = unipolar;
        if (0.0078125 >= pwf)
            p7 = 0xFFFFFFFFu;
        else
            p7 = (u32) rnd ((double) F (F (F (1.0f) - F (pwf)) * F (4294967296.0) - F (1.0)));
        auto d = ((double) F ((F (1.0f) - F (pwf)) / F (2.0)) - sk * (1.0 - std::abs (pwf))) * 4294967296.0;
        const auto hi = (u32) (p7 - 1u);
        if (d > (double) hi)
            d = (double) hi;
        else if (d < 1.0)
            d = 1.0;
        p5 = (u32) (std::uint64_t) rnd (d);
        if (pwf < 0.0078125)
        {
            p6 = (u32) (std::uint64_t) rnd (d + (double) F (pwf * 4294967296.0));
            if (p6 == 0)
                p6 = 1;
        }
        else
            p6 = p5;
        s0 = (double) F (2.0 / (double) p6);
        s1 = (double) F (2.0 / (double) (u32) (p7 - p5));
        startPhase = (u32) (p5 + (u32) (p7 - p5) / 2 + 1);
    }

    double curveK (double x) const noexcept
    {
        if (k == 0.0)
            return x;
        if (k > 0)
            return 1.0 - (std::exp ((1.0 - x) * L) - 1.0) * ik;
        return (std::exp (x * L) - 1.0) * ik;
    }

    double value (u32 u) const noexcept
    {
        double v;
        if (u > p7)
            return 1.0;
        if (u < p6)
            v = (double) F (1.0f - F ((double) u * s0));
        else if (u < p5)
            return -1.0;
        else
            v = (double) F (F ((double) (u32) (u - p5) * s1) - 1.0f);
        if (! uni)
        {
            const auto r = curveK (std::abs (v));
            return std::copysign ((double) F (r), v);
        }
        const auto x = (double) F ((v + 1.0) * 0.5);
        return (double) F (curveK (x) * 2.0 - 1.0);
    }
};

// ------------------------------------------------------------------------------------------------------------------
// FFT (radix 2, double): real forward / inverse with numpy's conventions (irfft ignores the imaginary parts of the DC
// and Nyquist bins and divides by N)

void fft (std::vector<cd>& a, bool inverse)
{
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    // twiddles cached per size and direction (FFT lengths are 4 096 and 16 384 only)
    thread_local std::map<std::pair<std::size_t, bool>, std::vector<cd>> cache;
    auto& w = cache[{ n, inverse }];
    if (w.size() != n / 2)
    {
        w.resize (n / 2);
        for (std::size_t k = 0; k < n / 2; ++k)
        {
            const auto ang = 2.0 * kPi * (double) k / (double) n * (inverse ? 1.0 : -1.0);
            w[k] = cd (std::cos (ang), std::sin (ang));
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const auto step = n / len;
        for (std::size_t i = 0; i < n; i += len)
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w[k * step];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

std::vector<cd> rfft (const double* x, std::size_t n)
{
    std::vector<cd> a (n);
    for (std::size_t i = 0; i < n; ++i)
        a[i] = cd (x[i], 0.0);
    fft (a, false);
    a.resize (n / 2 + 1);
    return a;
}

std::vector<double> irfft (const std::vector<cd>& X, std::size_t n)
{
    std::vector<cd> a (n);
    a[0] = cd (X[0].real(), 0.0);
    for (std::size_t k = 1; k < n / 2; ++k)
    {
        a[k] = X[k];
        a[n - k] = std::conj (X[k]);
    }
    a[n / 2] = cd (X[n / 2].real(), 0.0);
    fft (a, true);
    std::vector<double> y (n);
    for (std::size_t i = 0; i < n; ++i)
        y[i] = a[i].real() / (double) n;
    return y;
}

// ------------------------------------------------------------------------------------------------------------------
// operator wavetable (§2.3; round 23 §2): 16 384 points + guard

constexpr int kN = 16384;

struct ShapeParams
{
    double m, a0, t, p;
};

ShapeParams shapeParams (int S)
{
    const auto x = S / 128.0;
    ShapeParams r { x, 1.0, 0.0, 0.0 };
    if (x > 1.0)
    {
        r.m = 1.0;
        if (x <= 2.0)
            r.a0 = 2.0 - x;
        else
        {
            r.a0 = 0.0;
            if (x <= 3.0)
                r.t = x - 2.0;
            else
                r.t = 1.0, r.p = x - 3.0;
        }
    }
    return r;
}

double baseShape (double phi, const ShapeParams& s)
{
    if (s.p > 0)
        return (1 - 0.75 * s.p <= phi) ? -1.0 : 1.0;
    const auto sn = s.m < 1.0 ? std::sin (kPi * phi) : 0.0; // (× (1 − m) = 0 when m = 1: same result)
    if (s.m <= 0)
        return sn;
    auto u = phi + s.a0 / 2.0;
    if (u > 2.0)
        u -= 2.0;
    double wf = s.a0 < 2.0 ? 1.0 - (u - s.a0) * (2.0 / (2.0 - s.a0)) : 0.0;
    if (s.t != 0)
        wf = (u >= 1.0 ? -s.t : s.t) + wf * (1 - s.t);
    const auto wr = s.a0 > 0 ? (u / s.a0) * 2.0 - 1.0 : 0.0;
    const auto w = u >= s.a0 ? wf : wr;
    return sn * (1 - s.m) + w * s.m;
}

struct TensionCurve
{
    double k = 0, K = 0, L = 0;
    explicit TensionCurve (double kk) : k (kk), K (std::abs (kk)), L (std::log (std::abs (kk) + 1)) {}
    double operator() (double u) const
    {
        if (k == 0)
            return u;
        if (k > 0)
            return 1 - (std::exp ((1 - u) * L) - 1) / K;
        return (std::exp (u * L) - 1) / K;
    }
};

std::vector<float> opWave (const UnitRaw& U, bool gibbs)
{
    float amps[256], phs[256];
    std::memcpy (amps, U.op.data() + 0xc, sizeof amps);
    std::memcpy (phs, U.op.data() + 0x40c, sizeof phs);
    const auto S = opInt (U, 0x80c), T = opInt (U, 0x810), Sk = opInt (U, 0x814), Sine = opInt (U, 0x818), Pre = opInt (U, 0x81c);
    const bool half = U.op[0x820] != 0, even = U.op[0x821] != 0, absol = U.op[0x822] != 0;
    const auto sp = shapeParams (S);
    std::vector<float> out ((std::size_t) kN, 0.0f);
    const bool sk = Sk > 0;
    const TensionCurve skew (sk ? std::exp (std::log (1e9) * (std::exp (Sk / 128.0 * std::log (21.0)) - 1) * 0.05) - 1 : 0.0);
    for (int k = 0; k < 256; ++k)
    {
        const auto a = amps[k];
        if (a == 0.0f)
            continue;
        auto step = 2.0 * (k + 1) / kN;
        auto per = 2.0;
        if (half)
            step /= 2, per = 1.0;
        auto wrap = per;
        if (even)
            wrap = per * 2, step *= 2;
        auto ph = (double) F (F (phs[k]) * F (2.0));
        const auto af = (double) F (a);
        for (int i = 0; i < kN; ++i)
        {
            while (ph > wrap)
                ph -= wrap;
            auto q = ph;
            if (sk)
            {
                const auto c = skew (std::abs (q - 1.0));
                q = q <= 1.0 ? 1.0 - c : 1.0 + c;
            }
            auto v = ph <= per ? baseShape (q, sp) : 0.0;
            if (Sine > 0)
                v = std::sin (v * Sine * 0.03681553890925539);
            out[(std::size_t) i] = F ((double) out[(std::size_t) i] + v * af);
            ph += step;
        }
    }
    float pk = 0.0f;
    for (auto v : out)
        pk = std::max (pk, std::abs (v));
    std::vector<double> o ((std::size_t) kN);
    if (pk > kTiny)
    {
        const auto inv = F (1.0f / pk);
        for (std::size_t i = 0; i < o.size(); ++i)
            o[i] = (double) (out[i] * inv);
    }
    else
        for (std::size_t i = 0; i < o.size(); ++i)
            o[i] = out[i];
    if (T != 0)
    {
        const auto aa = (std::exp (std::abs (T) / 128.0 * std::log (21.0)) - 1) * 0.05;
        const TensionCurve tension (std::copysign (std::exp (aa * std::log (9.999999843067494e17)) - 1, (double) T));
        for (auto& v : o)
            v = std::copysign (tension (std::abs (v)), v);
    }
    if (absol)
        for (auto& v : o)
            v = std::abs (v);
    const auto M = (int) rnd ((std::exp ((0.35 * Pre / 128 + 0.65) * std::log (1e10)) - 1) * 1.6384e-6);
    if (M <= kN / 2)
    {
        auto X = rfft (o.data(), (std::size_t) kN);
        for (std::size_t i = (std::size_t) std::max (M, 0); i < X.size(); ++i)
            X[i] = 0.0;
        if (gibbs && M > 1)
        {
            const auto L = (double) (M - 1);
            for (int j = 1; j < M; ++j)
            {
                const auto x = j * kPi / L;
                X[(std::size_t) j] *= std::sin (x) / x;
            }
        }
        o = irfft (X, (std::size_t) kN);
    }
    std::vector<float> t ((std::size_t) kN + 1);
    for (int i = 0; i < kN; ++i)
    {
        auto v = F (o[(std::size_t) i]);
        if (std::abs (v) < kTiny)
            v = 0.0f;
        t[(std::size_t) i] = v;
    }
    t[(std::size_t) kN] = t[0];
    return t;
}

// the noise LCG s ← s·0x08088405 + 1 advanced n steps at once (affine map squared)
u32 lcgJump (u32 s, std::uint64_t n) noexcept
{
    u32 a = 0x08088405u, c = 1u, A = 1u, C = 0u;
    while (n > 0)
    {
        if (n & 1)
            A = A * a, C = C * a + c;
        c = c * a + c;
        a = a * a;
        n >>= 1;
    }
    return A * s + C;
}

// uniform generator of the pluck excitation (FL's IPP ippsRandUniform_Direct_32f, generic path; §2.6)
void ippRandUniform (float* out, int n, float low, float high, u32 seed)
{
    constexpr u32 A = 69069, C = 0x3c6ef373;
    const auto x0 = (double) F (F (high) - F (low)) * 2.3283064365387e-10;
    const auto x2 = (double) F (F (low) + F (high)) * 0.5;
    auto s32 = [] (u32 x) { return (double) (std::int32_t) x; };
    u32 r10 = seed, eax = 0x436cbae9u;
    u32 r14 = r10 * A + C;
    u32 r12 = r14 * A + C;
    u32 r11 = 0xffffffffu, r8 = 0;
    for (int k = 0; k < n / 2; ++k)
    {
        r12 = (u32) (0u - r12) + r14 + r11;
        r11 = (r12 & 0x80000000u) ? 0xffffffffu : 0u;
        const u32 edi = r12 - (r11 ? 18u : 0u);
        eax = eax * A;
        const u32 esi = eax + C;
        r8 = (u32) (0u - r14) + r10 + r11;
        r11 = (r8 & 0x80000000u) ? 0xffffffffu : 0u;
        r14 = edi;
        const u32 v1 = edi + eax + C;
        const u32 edi2 = esi * A;
        eax = edi2 + C;
        r8 = r8 - (r11 ? 18u : 0u);
        r12 = r10;
        r10 = r8;
        const u32 v2 = r8 + edi2 + C;
        out[2 * k] = F (s32 (v1) * x0 + x2);
        out[2 * k + 1] = F (s32 (v2) * x0 + x2);
    }
    if (n & 1)
    {
        const u32 r14b = r14 - r12 + r11;
        const u32 r8b = r14b - ((r14b & 0x80000000u) ? 18u : 0u);
        eax = eax * A;
        const u32 v = r8b + eax + C;
        out[n - 1] = F (s32 (v) * x0 + x2);
    }
}
} // namespace

// ------------------------------------------------------------------------------------------------------------------
// the immutable patch

struct ArtDef
{
    int unit = 0, index = 0, mode = 0;
    bool clampOn = true;
    double base = 0.0, fileEnvDepth = 0.0;
    Curve env, lfoCurve;
    std::array<Curve, 6> maps;
    std::array<bool, 6> mapDefault {};
    LfoWave wave;
    int knobs[4] {};
    int envFlags = 0, lfoFlags = 0, lfoSpeed = 0;
    u32 lfoStart = 0;
    bool lfoOn = false;
};

struct Patch
{
    StateRaw S;
    std::array<std::vector<float>, 6> wave;      // full 16 384 + 1 tables (empty: operator never sounds)
    std::array<std::vector<cd>, 6> spectrum;     // rfft of the table (band-limited operators)
    std::array<ArtDef, kNumArts> arts;
    std::array<std::vector<double>, 3> shaper;   // filter waveshaper tables (int16 values)
    std::array<int, 3> shaperN {};
    std::array<bool, 3> wsOn {}, wsUni {};
    std::vector<int> activeArts; // articulators evaluated per tick: envelope or LFO on, or a Mod X/Y map in use
    bool gibbs = false;
};

namespace
{

void buildArts (Patch& P)
{
    const auto& S = P.S;
    for (int u = 0; u < 10; ++u)
        for (int k = 0; k < kArts[u]; ++k)
        {
            const auto& a = S.units[(std::size_t) u].art[(std::size_t) k];
            auto& d = P.arts[(std::size_t) (kArtBase[u] + k)];
            d.unit = u;
            d.index = k;
            // constructor masks (§4.2): arts 0, 3 additive everywhere, filter arts 2, 3, operator art 4
            const bool add = k == 0 || k == 3 || (u >= 6 && u <= 8 && (k == 2 || k == 3)) || (u < 6 && k == 4);
            d.mode = add ? 1 : 0;
            const double off = add ? 0.5 : 0.0;
            d.clampOn = ! (u < 6 && k == 3);
            float base;
            std::memcpy (&base, &a.fl, 4);
            d.base = (double) base;
            d.fileEnvDepth = (double) a.envdepth;
            d.env.build (a.env, off, d.clampOn);
            d.lfoCurve.build (a.lfo, 0.5, true);
            d.lfoCurve.clamp = d.clampOn;
            for (int m = 0; m < 6; ++m)
            {
                d.maps[(std::size_t) m].build (a.maps[(std::size_t) m], off, true);
                d.mapDefault[(std::size_t) m] = d.maps[(std::size_t) m].isDefault();
            }
            d.wave.build (a.lfoP[1], a.lfoP[2], a.lfoP[3], (a.lfo.flags & 4) != 0);
            for (int j = 0; j < 4; ++j)
                d.knobs[j] = a.knobs[j];
            d.envFlags = a.env.flags;
            d.lfoFlags = a.lfo.flags;
            d.lfoSpeed = a.lfoP[0];
            d.lfoStart = (u32) (d.wave.startPhase + (u32) a.lfo5);
            d.lfoOn = a.lfo.on != 0 && ! a.lfo.pts.empty();
            if (d.env.pts.empty())
                d.env.on = false;
            if (d.env.on || d.lfoOn || ! d.mapDefault[2] || ! d.mapDefault[3])
                P.activeArts.push_back (kArtBase[u] + k);
        }
}

// filter waveshaper table (§5.5 step 4), rasterised with the map evaluator
void buildShaper (Patch& P, int k)
{
    const auto& U = P.S.units[(std::size_t) (6 + k)];
    std::int32_t on = 0;
    if (U.f.size() >= 0x2c)
        std::memcpy (&on, U.f.data() + 0x28, 4);
    P.wsOn[(std::size_t) k] = on != 0;
    P.wsUni[(std::size_t) k] = U.fbits > 0;
    if (! P.wsOn[(std::size_t) k])
        return;
    const bool uni = P.wsUni[(std::size_t) k];
    Curve c;
    c.build (U.shape, uni ? 0.5 : 0.0, true);
    const int N = (uni ? 2 : 1) * 0x4000 - 1;
    const int sc = (uni ? 2 : 1) * 0x8000 - 1;
    const auto dx = (double) F (1.0 / N);
    auto& t = P.shaper[(std::size_t) k];
    t.assign ((std::size_t) N + 2, 0.0);
    for (int i = 0; i <= N; ++i)
    {
        const auto v = c.mapValue (i * dx);
        t[(std::size_t) i] = (double) std::min<long long> (32767, std::max<long long> (-32768, rnd ((double) (F (v) * F (sc)))));
    }
    t[(std::size_t) N + 1] = t[(std::size_t) N];
    P.shaperN[(std::size_t) k] = N;
}

// ------------------------------------------------------------------------------------------------------------------
// per-voice articulator state (§4)

struct EnvSt
{
    const Curve* c = nullptr;
    double ttime = 0.0;
    double knobs[4] { 1, 1, 0, 1 };
    double out = 0.0;
    int stage = 9, idx = 0, sub = 9;
    double phase = 0.0, rate = 0.0, start = 0.0, delta = 0.0, mod = 0.0, susmod = 0.0;
    bool hold = false;

    void setup (const Curve* curve, double tt, double k0, double k1, double k2, double k3) noexcept
    {
        c = curve;
        ttime = tt;
        knobs[0] = (double) F (k0), knobs[1] = (double) F (k1), knobs[2] = (double) F (k2), knobs[3] = (double) F (k3);
        out = 0.0;
        stage = 9, idx = 0, sub = 9;
        phase = rate = start = delta = mod = susmod = 0.0;
        hold = false;
    }

    void init() noexcept
    {
        if (! c->on || c->pts.empty())
        {
            stage = sub = 9;
            start = delta = out = 0.0;
            return;
        }
        idx = 0;
        mod = 0.0;
        stage = c->decay < 0 ? 3 : 1;
        if (c->pts[0].dt == 0.0)
            rate = 1.0, phase = 1.0;
        else
            rate = ttime / c->pts[0].dt, phase = 0.0;
        sub = stage;
        start = (double) F (c->level (0, mod));
        delta = 0.0;
        out = start;
        hold = c->zeroloop;
    }

    void applyKnobs() noexcept
    {
        susmod = c->sus < 0 ? 0.0 : knobs[2];
        hold = hold || knobs[1] <= 0.0;
        if (stage >= 2)
            mod = susmod;
        if (c->on && ! c->pts.empty())
            out = (double) F (c->level (0, mod));
    }

    void process (double adv) noexcept
    {
        const auto n = (int) c->pts.size();
        if (! (idx >= 0 && idx <= n - 1))
            return;
        phase += adv * rate;
        int guard = 0;
        while (phase >= 1.0 && ++guard < 100000)
        {
            if (rate == 0.0)
                phase = adv;
            else
                phase = (phase - 1.0) / rate;
            start = (double) F (c->level (idx, mod));
            bool stop = false;
            if (idx < c->endpt || c->endpt < 0 || stage > 5)
            {
                if (c->decay >= 0 && idx >= c->decay && sub < 3)
                {
                    stage = std::max (stage, 3);
                    sub = 3;
                    mod = susmod;
                }
                if (idx < n - 1)
                    ++idx;
                else
                {
                    stage = 9;
                    stop = true;
                }
            }
            else if (c->loop < 0)
                stop = true;
            else
            {
                sub = 4;
                idx = c->loop + 1;
                stop = hold;
            }
            delta = (double) F (c->level (idx, mod) - start);
            if (! stop)
            {
                const auto ki = (stage > 2 ? 1 : 0) + ((stage >= 6 && c->sus >= 0) ? 2 : 0);
                const auto dur = (idx >= 0 && idx < n ? (double) F (c->pts[(std::size_t) idx].dt) : 0.0) * knobs[ki];
                if (dur <= 0.0)
                {
                    rate = 1.0;
                    phase += 1.0;
                }
                else
                {
                    rate = ttime / dur;
                    phase *= rate;
                }
            }
            else
                phase = rate = 0.0;
        }
        if (delta == 0.0)
            out = start;
        else
        {
            const auto fr = (idx >= 0 && idx < n) ? F (c->shape (idx, phase)) : 0.0f;
            out = (double) F (F (start) + fr * F (delta));
        }
    }

    void release() noexcept
    {
        if (stage >= 6)
            return;
        if (c->sus >= 0)
        {
            idx = c->sus;
            const auto lv = c->level (idx, mod, false);
            mod = (double) F (mod + (out - lv));
            phase = 1.0;
            sub = 6;
        }
        stage = 6;
        if (rate == 0.0)
            phase = 1.0;
    }
};

struct LfoSt
{
    EnvSt env;
    const LfoWave* wave = nullptr;
    u32 phase = 0;
    long long inc = 0;
    double out = 0.0;
    bool on = false;

    void process (double adv) noexcept
    {
        auto& e = env;
        auto adv2 = adv;
        if (e.idx == 0 && e.stage < 9 && e.phase < 1.0)
        {
            const auto x = e.rate != 0.0 ? adv + (e.phase - 1.0) / e.rate : adv;
            adv2 = x > 0.0 ? x : 0.0;
        }
        e.process (adv);
        if (e.idx != 0 || e.stage > 8 || e.phase >= 1.0)
        {
            phase = (u32) (phase + (u32) rnd (adv2 * (double) inc));
            out = (double) (F (e.out) * F (wave->value (phase)));
        }
    }
};

struct ArtSt
{
    const ArtDef* d = nullptr;
    int mode = 0;
    double base = 0.0, envdepth = 0.0, lfodepth = 0.0;
    EnvSt env;
    LfoSt lfo;
    double stat = 0.0;
    double tabs[2] {};
    int ntabs = 0;
    double out = 0.0;
    bool first = true;
    double relMult = 1.0;

    double combine (double a, double b) const noexcept
    {
        if (mode == 0)
            return (double) (F (a) * F (b));
        return (double) (F (a) + F (b));
    }
    bool active() const noexcept { return env.c->on || lfo.on || ntabs > 0; }
    bool finished() const noexcept { return std::min (env.stage, lfo.env.stage) >= 9; }

    void tick() noexcept
    {
        const auto adv = first ? 0.0 : 1.0;
        first = false;
        auto o = stat;
        for (int i = 0; i < ntabs; ++i)
            o = combine (o, tabs[i]);
        if (env.c->on)
        {
            env.process (adv);
            if (mode == 1)
                o = (double) (F (o) + F (env.out) * F (envdepth));
            else
                o = (double) (F (o) * F (env.out) * F (envdepth));
        }
        if (lfo.on)
        {
            lfo.process (adv);
            if (mode == 1)
                o = (double) (F (o) + F (lfo.out) * F (lfodepth));
            else
                o = (double) (F (o) * (F (lfo.out) * F (lfodepth) * 2.0f + 1.0f));
        }
        out = o;
    }

    void release() noexcept
    {
        const auto rm = F (relMult);
        env.knobs[3] = (double) (F (env.knobs[3]) * rm);
        lfo.env.knobs[3] = (double) (F (lfo.env.knobs[3]) * rm);
        env.release();
        lfo.env.release();
    }
};

// ------------------------------------------------------------------------------------------------------------------
// filter units 6…8 (§5)

struct FiltDef
{
    int type = 3, mode = 0, n = -1, nph = 0;
    bool hq = false, flat = false, os2 = false, alt = false;
    double k184 = 1.0, qexp = 1.0, fmode = 0.0, gL = 0, gB = 0, gH = 0;
    int p[10] {};
    std::vector<double> tab;

    static double g11 (double x) { return x == 1.0 ? 1.0 : (std::pow (11.0, x) - 1.0) / 10.0; }

    void build (const UnitRaw& U, double fs)
    {
        for (int i = 0; i < 10; ++i)
            p[i] = U.p[i];
        hq = U.f.size() > 0 && U.f[0] != 0;
        flat = U.f.size() > 1 && U.f[1] != 0;
        type = 3;
        if (U.f.size() >= 0x30)
            std::memcpy (&type, U.f.data() + 0x2c, 4);
        const auto ratio = 44100.0 / fs;
        mode = std::clamp (p[0], 0, 5);
        n = mode - 1;
        os2 = (hq && fs <= 0x2b10f) || (type == 11 && fs <= 0x15887);
        k184 = (double) F (ratio * (os2 ? 0.5 : 1.0));
        auto fm = (double) F ((double) mode);
        alt = false;
        if (type < 0 || type > 12)
            n = -1;
        if (type == 12)
            nph = mode + (flat ? 5 : 0);
        else
        {
            if (n > 2)
                alt = true, n -= 2;
            qexp = n >= 0 ? (type == 11 ? (double) F (1.0 / ((n / 4.0) + 1.0)) : (double) F (1.0 / (n + 1))) : 1.0;
            if (type == 5 && fm != 0)
                fm = (double) F (1.0 / fm);
        }
        fmode = (double) F (fm * 0.34657359027997264);
        gL = (double) F (g11 ((double) F (p[4] * 0.0078125)));
        gB = (double) F (g11 ((double) F (p[5] * 0.0078125)));
        gH = (double) F (g11 ((double) F (p[6] * 0.0078125)));
        tab.clear();
        if (type == 0)
        {
            const auto r = ratio / (os2 ? 2.0 : 1.0);
            for (int i = 0; i < 4096; ++i)
            {
                const auto x = (double) F (std::min (1.0, (double) F ((i * (1.0 / 2048.0) + 0.01) * 0.5)));
                const auto v = 2.0 * std::sin ((double) F (std::asin (x)) * r);
                if (v >= 1.175)
                {
                    tab.push_back (1.175);
                    break;
                }
                tab.push_back ((double) F (v));
            }
        }
    }
    double altExp() const noexcept { return n == 1 ? 0.5 : 0.75; }
};

struct Coefs
{
    double f[3] {}, q0 = 1.0;
    double lime[3][2] {};
    double biq[3][5] {};
    double mango[3][4] {};
    double phA = 0, phDep = 0;
};

void biquadCoefs (int code, double A, double alpha, double s, double c, double* o)
{
    (void) s;
    if (code >= 1 && code <= 5)
    {
        const auto k = 1.0 / (alpha + 1.0);
        const auto a1 = k * c * -2.0, a2 = k * (1.0 - alpha);
        double b0, b1, b2;
        if (code == 1)
            b1 = k * (1.0 - c), b0 = b1 * 0.5, b2 = b0;
        else if (code == 2)
            b0 = k * alpha, b1 = 0.0, b2 = -k * alpha;
        else if (code == 3)
            b0 = k, b1 = a1, b2 = k;
        else if (code == 4)
            b1 = -k * (c + 1.0), b0 = -b1 * 0.5, b2 = b0;
        else
            b0 = a2, b1 = a1, b2 = 1.0;
        o[0] = b0, o[1] = b1, o[2] = b2, o[3] = a1, o[4] = a2;
        return;
    }
    if (code == 6)
    {
        const auto d2 = (A - 1.0) * c, p3 = alpha * s, k = 1.0 / (A + 1.0 + d2 + p3);
        o[3] = k * ((A - 1.0) + (A + 1.0) * c) * -2.0;
        o[4] = k * ((A + 1.0 + d2) - p3);
        o[1] = k * A * 2.0 * ((A - 1.0) - (A + 1.0) * c);
        o[0] = k * A * (((A + 1.0) - d2) + p3);
        o[2] = k * A * (((A + 1.0) - d2) - p3);
        return;
    }
    if (code == 7)
    {
        const auto k = 1.0 / (alpha / A + 1.0);
        const auto a1 = k * c * -2.0, a2 = k * (1.0 - alpha / A);
        o[0] = k * (alpha * A + 1.0), o[1] = a1, o[2] = k * (1.0 - alpha * A), o[3] = a1, o[4] = a2;
        return;
    }
    const auto d2 = (A - 1.0) * c, p3 = alpha * s, k = 1.0 / (((A + 1.0) - d2) + p3);
    o[3] = k * ((A - 1.0) - (A + 1.0) * c) * 2.0;
    o[4] = k * (((A + 1.0) - d2) - p3);
    o[1] = k * A * -2.0 * ((A - 1.0) + (A + 1.0) * c);
    o[0] = k * A * (A + 1.0 + d2 + p3);
    o[2] = k * A * ((A + 1.0 + d2) - p3);
}

// coefficient laws per type (§5.4, FUN_0086ebe0)
void filterCoefs (const FiltDef& D, double c, double r, double gainL, Coefs& C)
{
    const auto t = D.type;
    const auto n = D.n;
    if (t >= 1 && t <= 10 && D.flat)
        r = r * (c + 1.0) * 0.5;
    if (t == 0)
    {
        auto d = c > 0 ? (double) F (std::pow (26.0, c) - 1.0) * 0.03959999978542328 : 0.0;
        double q = r <= 0 ? 1.0 : (r >= 1 ? 0.2 : 1.0 - r * 0.800000011920929);
        if (D.flat)
            q = q * q;
        for (int i = n; i >= 0; --i)
        {
            auto idx = (long long) rnd (d * 2048.0);
            idx = std::min<long long> (idx, (long long) D.tab.size() - 1);
            C.f[i] = D.tab[(std::size_t) std::max<long long> (idx, 0)];
            if (D.alt)
                d = std::pow (d, D.altExp());
        }
        C.q0 = (double) F (q);
        return;
    }
    if (t == 1 || t == 2)
    {
        double d = 0.0;
        if (c > 0)
            d = std::min ((double) F (std::pow (76.0, c) - 1.0) * 0.01324, 1.6);
        double q = 1.0;
        if (r > 0)
            q = r >= 1 ? 50.0 : (double) F (std::pow (1001.0, (double) F (r)) - 1.0) * 0.049 + 1.0;
        if (n > 0)
            q = std::pow (q, D.qexp);
        const auto sc = D.k184 * 1.5707963267948966;
        for (int i = n; i >= 0; --i)
        {
            const auto w = (d + 0.007) * sc;
            const auto cw = std::cos (std::min (w, 1.2566370614359172));
            const auto A = (double) F (2.0 - cw * 2.0);
            const auto tt = cw - 1.0;
            auto B = (double) F ((std::sqrt (-tt * tt * tt) * 1.41421354) / (q * tt) + 1.0);
            if (B < 0.1)
                B = (double) F (0.1);
            C.lime[i][0] = A, C.lime[i][1] = B;
            if (D.alt)
                d = std::pow (d, D.altExp());
        }
        return;
    }
    if (t >= 3 && t <= 10)
    {
        double d = 0.0, off;
        if (t == 3)
        {
            d = c > 0 ? (double) F (std::pow (1501.0, c) - 1.0) * 0.0006646666442975402 : 0.0;
            off = 0.003;
        }
        else
        {
            const auto lim = t == 6 ? 0.6001 : 1.0001;
            if (c > 0)
                d = std::min ((double) F (std::pow (1501.0, c) - 1.0) * 0.00039993334212340415, lim);
            off = 0.0001;
        }
        double A = 0.0, qq = 0.0, q = 0.0, sA = 0.0, kk = 0.0, bw = 0.0;
        if (t == 3 || t == 6 || t == 7)
        {
            q = 0.1;
            if (r > 0)
                q = r >= 1 ? 50.0 : (double) F (std::pow (2001.0, (double) F (r)) - 1.0) * 0.024950001388788223 + 0.1;
            if (n > 0)
                q = std::pow (q, D.qexp);
            qq = 0.5 / q;
        }
        else if (t == 4 || t == 5)
        {
            q = 0.01;
            if (r > 0)
                q = r >= 1 ? 12.0 : (double) F (std::pow (101.0, (double) F (r)) - 1.0) * 0.11989999562501907 + 0.01;
        }
        else if (t == 8 || t == 10)
        {
            const auto rr = 1.0 - r;
            double S = 0.1;
            if (rr > 0)
                S = rr >= 1 ? 1.0 : (double) F (std::pow (3.0, (double) F (rr)) - 1.0) * 0.44999998807907104 + 0.1;
            A = gainL;
            if (n > 0)
                A = std::pow (A, D.qexp);
            sA = std::sqrt (A);
            kk = std::sqrt ((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
        }
        else
        {
            const auto rr = 1.0 - r;
            bw = 0.1;
            if (rr > 0)
                bw = rr >= 1 ? 100.0 : (double) F (std::pow (2001.0, (double) F (rr)) - 1.0) * 0.049949999898672104 + 0.1;
            A = gainL;
            if (n > 0)
                A = std::pow (A, D.qexp);
        }
        static constexpr int BIQ[11] { 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
        const auto sc = D.k184 * 6.283185307179586;
        for (int i = n; i >= 0; --i)
        {
            auto w = (d + off) * sc;
            w = std::min (std::max (w, 0.0), 2.849517146113191);
            const auto s = std::sin (w), cw = std::cos (w);
            const auto code = BIQ[t];
            if (t == 3 || t == 6 || t == 7)
                biquadCoefs (code, 0.0, s * qq, s, cw, C.biq[i]);
            else if (t == 4 || t == 5)
                biquadCoefs (code, 0.0, s * std::sinh ((q * D.fmode * w) / s), s, cw, C.biq[i]);
            else if (t == 8 || t == 10)
                biquadCoefs (code, A, sA * kk, s, cw, C.biq[i]);
            else
                biquadCoefs (code, A, s / (bw * 2.0), s, cw, C.biq[i]);
            if (D.alt)
                d = std::pow (d, D.altExp());
        }
        return;
    }
    if (t == 11)
    {
        double d = 0.0;
        if (c > 0)
            d = std::min ((double) F (std::pow (201.0, c) - 1.0) * 0.014949999749660492, 3.0);
        const auto g = gainL;
        const auto drv = (double) F (std::pow (251.0, g) - 1.0) * 0.12;
        const auto inv = drv >= 0.001 ? 1.0 / drv : 0.0;
        double res = 0.0;
        if (r > 0)
        {
            res = (double) F (std::min ((double) F (drv), 1.0)) * 0.6 + 1.9;
            if (r < 1)
                res = r * res;
        }
        if (n > 0)
            res = std::pow (res + 1.0, D.qexp) - 1.0;
        const auto sc = D.k184 * 0.5;
        for (int i = n; i >= 0; --i)
        {
            const auto w = std::min ((d + 0.01) * sc, 0.65);
            const auto w2 = w * w;
            auto coef = 1.0 - std::exp (-kPi * (((w * 1.873 * w2 + w2 * 0.4955) - w * 0.649) + 0.9988) * w);
            auto fb = (double) F (-res * (w2 * -3.9364 + w * 1.8409 + 0.9968));
            double k4, k58;
            if (drv >= 0.001)
            {
                k58 = drv * 2.0;
                coef = coef * inv;
                k4 = (double) F (std::tanh (inv * 4.0));
                if (D.flat)
                    fb = (double) F (fb * k4);
                k4 = (double) F ((1.0 / k4) * 0.5 * (1.0 - fb));
            }
            else
            {
                k4 = (double) F ((1.0 - fb) * 0.5);
                k58 = 0.0;
            }
            C.mango[i][0] = coef, C.mango[i][1] = fb, C.mango[i][2] = k4, C.mango[i][3] = k58;
            if (D.alt)
                d = std::pow (d, D.altExp());
        }
        return;
    }
    // 12 Cherry phaser
    double d = 0.0;
    if (c > 0)
        d = (double) F (std::pow (151.0, c) - 1.0) * 0.006666000001132488;
    double dep = 0.0;
    if (r > 0)
        dep = r >= 1 ? 0.9 : (double) F (std::log ((double) F (r * 10.0) + 1.0) / std::log (11.0)) * 0.9;
    const auto w = std::min ((d + 0.0001) * D.k184, 1.0);
    auto a = (double) F ((1.0 - w) / (w + 1.0));
    if (std::abs (a) < 5.960464477539063e-08)
        a = 0.0;
    C.phA = a;
    C.phDep = (double) F (dep);
}

struct FiltState
{
    float svf[3][2] {};
    double st[3][4] {};
    float mg[3][5] {};
    float ph[32] {};
    float phfb = 0.0f;
    bool prevSet = false;
    double prev[8] {};
    bool gprevSet = false;
    float gprev[3] {};
    // coefficients of the last sub-block (recomputed only when cutoff, resonance or gain change)
    bool coefsSet = false;
    double lastC = 0, lastR = 0, lastGain = 0;
    Coefs coefs;

    void reset() noexcept { *this = FiltState {}; }
};

// scratch buffers for one filter call (2× oversampled length)
struct FiltScratch
{
    std::vector<float> xs, y, ga[3];
    void resize (int n)
    {
        xs.assign ((std::size_t) n, 0.0f);
        y.assign ((std::size_t) n, 0.0f);
        for (auto& g : ga)
            g.assign ((std::size_t) n, 0.0f);
    }
};

void laneRamp (float start, float inc, int n, float* g) noexcept
{
    const auto f4 = F (inc * 4.0f);
    float lanes[4] { start + 0.0f * f4, start + F (0.25f * f4), start + F (0.5f * f4), start + F (0.75f * f4) };
    const auto full = (n >> 4) << 4;
    for (int j = 0; j < full; j += 4)
        for (int c = 0; c < 4; ++c)
        {
            g[j + c] = lanes[c];
            lanes[c] = lanes[c] + f4;
        }
    auto v = lanes[0];
    for (int j = full; j < n; ++j)
    {
        g[j] = v;
        v = v + inc;
    }
}

// FUN_0062bb20: start value, per-sample increment, new "previous" value
void rampOut (float prev, float target, int n, float maxstep, float& start, float& inc, float& newprev) noexcept
{
    start = prev;
    inc = target - prev;
    newprev = prev;
    if (inc != 0.0f)
    {
        newprev = target;
        inc = inc / (float) n;
        if (std::abs (inc) > maxstep)
        {
            inc = std::copysign (maxstep, inc);
            newprev = prev + inc * (float) n;
            if (std::abs (newprev) < kTiny)
            {
                newprev = 0.0f;
                inc = (-prev) / (float) n;
            }
        }
    }
}

// one stage set over y[0..nn) in place, plain coefficients (FUN_00626230, biquad, Mango, Cherry kernels)
void filterRun (const FiltDef& D, FiltState& S, const Coefs& C, float* y, int nn, float gL, float gB, float gH, const float* const* GA)
{
    const auto t = D.type, n = D.n;
    if (t == 0)
    {
        const auto q0 = (float) C.q0;
        for (int i = 0; i <= n; ++i)
        {
            const auto f = (float) C.f[i];
            auto q = i == 0 ? q0 : 1.0f;
            if (D.flat)
                q = i == 0 ? (q * (float) D.k184) / f : (float) D.k184 / f;
            auto lo = S.svf[i][0], bd = S.svf[i][1];
            for (int k = 0; k < nn; ++k)
            {
                lo = bd * f + lo;
                const auto hi = y[k] - (bd * q + lo);
                bd = hi * f + bd;
                if (GA == nullptr)
                    y[k] = (lo * gL + bd * gB) + hi * gH;
                else
                    y[k] = (lo * GA[0][k] + bd * GA[1][k]) + hi * GA[2][k];
            }
            S.svf[i][0] = lo, S.svf[i][1] = bd;
        }
        return;
    }
    if (t >= 3 && t <= 10)
    {
        for (int i = 0; i <= n; ++i)
        {
            const auto b0 = C.biq[i][0], b1 = C.biq[i][1], b2 = C.biq[i][2], a1 = C.biq[i][3], a2 = C.biq[i][4];
            auto x1 = S.st[i][0], x2 = S.st[i][1], y1 = S.st[i][2], y2 = S.st[i][3];
            for (int k = 0; k < nn; ++k)
            {
                const auto xv = (double) y[k];
                const auto yv = xv * b0 + (((x1 * b1 - y2 * a2) + x2 * b2) - y1 * a1);
                y[k] = (float) yv;
                x2 = x1, x1 = xv, y2 = y1, y1 = yv;
            }
            S.st[i][0] = x1, S.st[i][1] = x2, S.st[i][2] = y1, S.st[i][3] = y2;
        }
        return;
    }
    if (t == 11)
    {
        for (int i = 0; i <= n; ++i)
        {
            const auto g = (float) C.mango[i][0], fb = (float) C.mango[i][1], k4 = (float) C.mango[i][2];
            const auto k58 = C.mango[i][3];
            auto s0 = S.mg[i][0], s2 = S.mg[i][1], s4 = S.mg[i][2], s6 = S.mg[i][3], s8 = S.mg[i][4];
            if (k58 == 0.0)
                for (int k = 0; k < nn; ++k)
                {
                    s0 = ((y[k] + s8 * fb) - s0) * g + s0;
                    s2 = (s0 - s2) * g + s2;
                    s4 = (s2 - s4) * g + s4;
                    const auto o6 = s6;
                    s6 = (s4 - s6) * g + s6;
                    s8 = s6 + o6;
                    y[k] = k4 * s8;
                }
            else
            {
                const auto dr = (float) k58;
                auto sat = [] (float v) {
                    const auto a = std::abs (v);
                    const auto m = (3.0f + a) * a + 6.0f;
                    return (v * m) / (m * a + 12.0f);
                };
                for (int k = 0; k < nn; ++k)
                {
                    const auto u = (y[k] + s8 * fb) * dr;
                    s0 = (sat (u) - sat (s0 * dr)) * g + s0;
                    s2 = (sat (s0 * dr) - sat (s2 * dr)) * g + s2;
                    s4 = (sat (s2 * dr) - sat (s4 * dr)) * g + s4;
                    const auto o6 = s6;
                    s6 = (sat (s4 * dr) - sat (o6 * dr)) * g + o6;
                    s8 = s6 + o6;
                    y[k] = k4 * s8;
                }
            }
            S.mg[i][0] = s0, S.mg[i][1] = s2, S.mg[i][2] = s4, S.mg[i][3] = s6, S.mg[i][4] = s8;
        }
        return;
    }
    if (t == 12)
    {
        const auto a = (float) C.phA, dep = (float) C.phDep;
        const auto np = std::min (2 * D.nph, 32);
        auto fbs = S.phfb;
        for (int k = 0; k < nn; ++k)
        {
            auto v = fbs * dep + y[k];
            for (int j = 0; j < np; ++j)
            {
                const auto w = v * -a + S.ph[j];
                S.ph[j] = w * a + v;
                v = w;
            }
            y[k] = v;
            fbs = v;
        }
        S.phfb = fbs;
    }
}

// one sub-block of a filter unit (SytFilterBlocks._seg): x[0..m) → out[0..m)
void filterSeg (const FiltDef& D, FiltState& S, FiltScratch& W, const float* x, int m, double cut, double res, const double* ga,
                float maxstep, float* out)
{
    if (D.n < 0)
    {
        std::memmove (out, x, sizeof (float) * (std::size_t) m);
        return;
    }
    const auto c = (double) F ((double) F (D.p[2] * 0.00390625) + cut);
    const auto r = (double) F ((double) F (D.p[3] * 0.00390625) + res);
    double gainA = 0.0;
    if (D.type >= 8 && D.type <= 10)
    {
        const auto x1 = F (D.p[4] * ga[0]);
        const auto x2 = F ((double) x1 * 0.015625);
        const auto x3 = F (std::pow (8.0, (double) x2));
        gainA = (double) F (((double) x3 - 1.0) * 0.125 + 0.125);
    }
    else if (D.type == 11)
        gainA = (double) F (D.p[4] * 0.0078125 * ga[0]);
    if (! S.coefsSet || c != S.lastC || r != S.lastR || gainA != S.lastGain)
    {
        S.coefs = Coefs {};
        filterCoefs (D, c, r, gainA, S.coefs);
        S.coefsSet = true, S.lastC = c, S.lastR = r, S.lastGain = gainA;
    }
    const auto& C = S.coefs;
    const auto os = D.os2 ? 2 : 1;
    const auto nn = m * os;
    float* y = W.y.data();
    for (int k = 0; k < m; ++k)
        for (int j = 0; j < os; ++j)
            y[k * os + j] = x[k];
    auto finish = [&] {
        if (D.os2)
            for (int k = 0; k < m; ++k)
                out[k] = (y[2 * k] + y[2 * k + 1]) * 0.5f;
        else
            std::memcpy (out, y, sizeof (float) * (std::size_t) m);
    };
    auto plainGains = [&] (float& gL, float& gB, float& gH) {
        gL = (float) D.gL, gB = (float) D.gB, gH = (float) D.gH;
        if (D.type == 0 && D.n > 0)
        {
            auto pw = [&] (float v) { return v <= 0 ? 0.0f : F (std::pow ((double) v, D.qexp)); };
            gL = pw (gL), gB = pw (gB), gH = pw (gH);
        }
    };
    if (D.type > 2)
    {
        float gL, gB, gH;
        plainGains (gL, gB, gH);
        filterRun (D, S, C, y, nn, gL, gB, gH, nullptr);
        finish();
        return;
    }
    const float* GA[3] { nullptr, nullptr, nullptr };
    bool useGA = false;
    if (D.type == 0)
    {
        // Low/Band/High = unit gain × articulators 4/5/6 (pow 1/(n+1)), ramped over the sub-block (§5.8)
        float gt[3];
        const double g0s[3] { D.gL, D.gB, D.gH };
        for (int j = 0; j < 3; ++j)
        {
            auto v = (float) g0s[j] * F (ga[j]);
            if (D.n > 0)
                v = v <= 0 ? 0.0f : F (std::pow ((double) v, D.qexp));
            gt[j] = v;
        }
        if (! S.gprevSet)
        {
            S.gprevSet = true;
            for (int j = 0; j < 3; ++j)
                S.gprev[j] = gt[j];
        }
        bool anyramp = false;
        for (int j = 0; j < 3; ++j)
        {
            const auto cur = S.gprev[j];
            auto s_ = gt[j] - cur;
            auto newp = cur;
            if (s_ != 0.0f)
            {
                newp = gt[j];
                s_ = s_ / (float) nn;
                if (std::abs ((double) s_) > (double) maxstep)
                {
                    s_ = std::copysign (maxstep, s_);
                    newp = cur + s_ * (float) nn;
                    if (std::abs (newp) < kTiny)
                        newp = 0.0f, s_ = (-cur) / (float) nn;
                }
            }
            S.gprev[j] = newp;
            if (s_ != 0.0f)
                anyramp = true;
            float* a_ = W.ga[j].data();
            if (j == 0)
                laneRamp (cur, s_, nn, a_);
            else
            {
                auto vv = cur;
                for (int q = 0; q < nn; ++q)
                {
                    a_[q] = vv;
                    vv = vv + s_;
                }
            }
            GA[j] = a_;
        }
        useGA = anyramp || gt[0] != (float) D.gL || gt[1] != (float) D.gB || gt[2] != (float) D.gH;
    }
    double tgt[8];
    int nt = 0;
    if (D.type == 0)
    {
        for (int i = 0; i <= D.n; ++i)
            tgt[nt++] = C.f[i];
        tgt[nt++] = C.q0;
    }
    else
        for (int i = 0; i <= D.n; ++i)
            tgt[nt++] = C.lime[i][0], tgt[nt++] = C.lime[i][1];
    if (! S.prevSet)
    {
        S.prevSet = true;
        for (int k = 0; k < nt; ++k)
            S.prev[k] = tgt[k];
    }
    double start[8], steps[8];
    bool allZero = true;
    for (int k = 0; k < nt; ++k)
    {
        const auto cur = S.prev[k];
        start[k] = cur;
        auto s = (double) F (tgt[k]) - cur;
        if (s != 0.0)
        {
            S.prev[k] = tgt[k];
            s = (double) F (s / nn);
            if (std::abs (s) > (double) maxstep)
            {
                s = std::copysign ((double) maxstep, s);
                S.prev[k] = (double) F (cur + s * nn);
            }
        }
        steps[k] = s;
        if (s != 0.0)
            allZero = false;
    }
    if (D.type == 0 && allZero)
    {
        float gL, gB, gH;
        plainGains (gL, gB, gH);
        filterRun (D, S, C, y, nn, gL, gB, gH, useGA ? GA : nullptr);
        finish();
        return;
    }
    // ramped kernels (FUN_006262d0, Lime LP/HP)
    if (D.type == 0)
    {
        float gL, gB, gH;
        plainGains (gL, gB, gH);
        for (int i = 0; i <= D.n; ++i)
        {
            auto f = (float) start[i];
            const auto df = (float) steps[i];
            auto q = i == 0 ? (float) start[nt - 1] : 1.0f;
            const auto dq = i == 0 ? (float) steps[nt - 1] : 0.0f;
            auto lo = S.svf[i][0], bd = S.svf[i][1];
            for (int k = 0; k < nn; ++k)
            {
                lo = bd * f + lo;
                const auto hi = y[k] - (bd * q + lo);
                bd = hi * f + bd;
                if (! useGA)
                    y[k] = (lo * gL + bd * gB) + hi * gH;
                else
                    y[k] = (lo * GA[0][k] + bd * GA[1][k]) + hi * GA[2][k];
                q = q + dq;
                f = f + df;
            }
            S.svf[i][0] = lo, S.svf[i][1] = bd;
        }
    }
    else
    {
        const bool lp = D.type == 1;
        for (int i = 0; i <= D.n; ++i)
        {
            auto A = start[2 * i], B = start[2 * i + 1];
            const auto dA = (double) F (steps[2 * i]), dB = (double) F (steps[2 * i + 1]);
            auto yy = S.st[i][0], v = S.st[i][1];
            for (int k = 0; k < nn; ++k)
            {
                const auto xv = (double) y[k];
                v = v + (xv - yy) * A;
                yy = yy + v;
                v = v * B;
                y[k] = lp ? (float) yy : (float) (xv - yy);
                A += dA;
                B += dB;
            }
            S.st[i][0] = yy, S.st[i][1] = v;
        }
    }
    finish();
}

// filter waveshaper kernels (FUN_0062af60/…afe0 bipolar, FUN_0062b130/…b1b0 unipolar); mix = 1.
// Nearest point (live): index round(x·pre·N) (odd-symmetric) or round(x·pre·N/2 + N/2) (unipolar).
// Interpolated (while FL renders, or state byte P+0x5F18): linear between the two table points around the exact
// position, for both modes. EXEC (this port): demo channels 89/116 −73/−72 dB → −131/−122 dB, 217 (unipolar)
// −63 → −113 dB; the reference model's round-to-nearest fraction (and its nearest-point unipolar kernel) was wrong.
void waveshape (const float* x, int m, const std::vector<double>& tab, int N, bool uni, float pre, bool interp, float* sh) noexcept
{
    const auto top = (std::size_t) N + 1;
    if (uni)
    {
        const auto a = F ((double) pre * N * 0.5), b = F (N * 0.5);
        for (int k = 0; k < m; ++k)
        {
            const auto v = x[k] * a + b;
            if (! interp)
            {
                const auto idx = (int) std::clamp ((double) rne (v), 0.0, (double) N);
                sh[k] = F (tab[(std::size_t) idx] * 3.0517578125e-05);
                continue;
            }
            const auto f = std::clamp ((double) v, 0.0, (double) N);
            const auto i = (std::size_t) f;
            const auto fr = f - (double) i;
            sh[k] = F ((tab[i] + (tab[std::min (i + 1, top)] - tab[i]) * fr) * 3.0517578125e-05);
        }
        return;
    }
    const auto pN = (float) N;
    for (int k = 0; k < m; ++k)
    {
        const auto f = x[k] * pre * pN;
        double v;
        if (interp)
        {
            const auto af = std::min (std::abs ((double) f), (double) N);
            const auto i = (std::size_t) af;
            const auto fr = af - (double) i;
            v = tab[i] + (tab[std::min (i + 1, top)] - tab[i]) * fr;
            if (f < 0)
                v = -v;
        }
        else
        {
            const auto r = rne (f);
            const auto a = (std::size_t) std::min (std::abs ((double) r), (double) N);
            v = r >= 0 ? tab[a] : -tab[a];
        }
        sh[k] = F (v * 3.0517578125e-05);
    }
}

// ------------------------------------------------------------------------------------------------------------------
// post-voice chain (§7): stereo blocks of doubles

struct Stereo
{
    std::vector<double> l, r;
    void resize (std::size_t n)
    {
        l.assign (n, 0.0);
        r.assign (n, 0.0);
    }
};

inline double flush32 (double v) noexcept
{
    v = (double) (float) v;
    return std::abs (v) < 5.960464477539063e-08 ? 0.0 : v;
}

// 2:1 polyphase IIR half-band (FUN_00841cd0, §7.2)
struct Decim2
{
    static constexpr double A[3] { 0.1473771136010466, 0.48246854276970014, 0.8830050257693731 };
    static constexpr double B[3] { 0.039151597734460045, 0.3026468483284934, 0.6746159185469639 };
    double ya[2][3] {}, yb[2][3] {}, xa[2][3] {}, xb[2][3] {};

    // in: 2n samples per channel → out: n (in place allowed: out[i] written after in[2i], in[2i+1] are read)
    void process (double* l, double* r, int n2) noexcept
    {
        processCh (0, l, n2);
        processCh (1, r, n2);
    }

    void processCh (int c, double* x, int n2) noexcept
    {
        const auto n = n2 / 2;
        if (ya[c][0] == 0 && ya[c][1] == 0 && ya[c][2] == 0 && yb[c][0] == 0 && yb[c][1] == 0 && yb[c][2] == 0 && xa[c][0] == 0
            && xa[c][1] == 0 && xa[c][2] == 0 && xb[c][0] == 0 && xb[c][1] == 0 && xb[c][2] == 0)
        {
            bool zero = true;
            for (int i = 0; i < n2 && zero; ++i)
                zero = x[i] == 0.0;
            if (zero) // zero in, zero states: zero out (exact)
            {
                std::fill (x, x + n, 0.0);
                return;
            }
        }
        double a0 = ya[c][0], a1 = ya[c][1], a2 = ya[c][2], b0 = yb[c][0], b1 = yb[c][1], b2 = yb[c][2];
        double xa0 = xa[c][0], xa1 = xa[c][1], xa2 = xa[c][2], xb0 = xb[c][0], xb1 = xb[c][1], xb2 = xb[c][2];
        for (int i = 0; i < n; ++i)
        {
            double a = (double) (float) x[2 * i], b = (double) (float) x[2 * i + 1];
            double na = xa0 + (a - a0) * A[0];
            xa0 = a, a0 = na, a = na;
            double nb = xb0 + (b - b0) * B[0];
            xb0 = b, b0 = nb, b = nb;
            na = xa1 + (a - a1) * A[1];
            xa1 = a, a1 = na, a = na;
            nb = xb1 + (b - b1) * B[1];
            xb1 = b, b1 = nb, b = nb;
            na = xa2 + (a - a2) * A[2];
            xa2 = a, a2 = na, a = na;
            nb = xb2 + (b - b2) * B[2];
            xb2 = b, b2 = nb, b = nb;
            x[i] = (double) (float) ((b + a) * 0.5);
        }
        ya[c][0] = flush32 (a0), ya[c][1] = flush32 (a1), ya[c][2] = flush32 (a2);
        yb[c][0] = flush32 (b0), yb[c][1] = flush32 (b1), yb[c][2] = flush32 (b2);
        xa[c][0] = xa0, xa[c][1] = xa1, xa[c][2] = xa2, xb[c][0] = xb0, xb[c][1] = xb1, xb[c][2] = xb2;
    }
};

// block-linear parameter ramp with a slope limit (FUN_0062bb20 as used by the delay feedback)
struct Ramp
{
    double cur = 0.0, maxslope = 0.0;
    // returns start and slope; sample i uses start + slope·i
    void block (double target, int n, double& start, double& slope) noexcept
    {
        start = cur;
        slope = (target - start) / n;
        if (slope != 0.0)
        {
            if (std::abs (slope) > maxslope)
                slope = std::copysign (maxslope, slope);
            cur = start + slope * n;
            if (slope != 0 && std::abs (target - start) / n <= maxslope)
                cur = target;
        }
    }
};

double gcurve (double x) { return x == 1.0 ? 1.0 : (std::pow (11.0, x) - 1.0) / 10.0; }

struct DelayRec
{
    int fb = 0, time = 0, off = 0, vol = 0, mode = 0, tempo = 0, serial = 0, on = 0;
};

// delay units (§7.4)
struct Delay
{
    int mode = 0, D = 1, oL = 0, oR = 0, size = 1, rpos = 0;
    long long zeroRun = 1 << 30; // consecutive zero ring reads (≥ size: the ring is empty)
    double fb = 0.0, gain = 0.0;
    bool toFx = false;
    std::vector<double> bl, br;
    Ramp ramp;

    void build (const DelayRec& rec, double fxrate, int block, double bpm, bool startOnTarget)
    {
        mode = rec.mode;
        long long d = rec.tempo ? rnd (rec.time * fxrate * 0.3125 / bpm) : rnd (rec.time * fxrate / 384.0);
        D = (int) std::max<long long> (1, d);
        const auto a = (std::pow (1001.0, std::abs (rec.off) / 512.0) - 1.0) * 50.0;
        const auto o = (int) rnd (a * fxrate / 100000.0);
        oL = rec.off < 0 ? o : 0;
        oR = rec.off > 0 ? o : 0;
        fb = rec.fb / 128.0;
        const auto v = rec.vol / 128.0;
        gain = v != 0 ? std::copysign (gcurve (std::abs (v)), v) : 0.0;
        size = block + o + D;
        bl.assign ((std::size_t) size, 0.0);
        br.assign ((std::size_t) size, 0.0);
        rpos = 0;
        // a fresh unit's feedback ramp starts from 0.5 (§7.4); after the render-mode rebuild (FPD_ProcessMode with
        // PM_IsRendering) it starts on its target (EXEC, this port: demo channel 44 −22 dB → −166 dB)
        ramp.cur = startOnTarget ? fb : 0.5;
        ramp.maxslope = 0.001 * 44100.0 / fxrate;
    }

    void write (const double* xl, const double* xr, int n) noexcept
    {
        const auto w = (rpos + D) % size;
        for (int i = 0; i < n; ++i)
        {
            if (xl[i] != 0.0 || xr[i] != 0.0)
                zeroRun = 0;
            bl[(std::size_t) ((w + oL + i) % size)] += xl[i];
            br[(std::size_t) ((w + oR + i) % size)] += xr[i];
        }
    }

    // output × gain added to (ol, or)
    void process (int n, double* ol, double* orr) noexcept
    {
        double s0, slope;
        ramp.block (fb, n, s0, slope);
        if (zeroRun >= size) // the whole ring is zero: nothing to read, feed back or output
        {
            rpos = (rpos + n) % size;
            return;
        }
        auto r = rpos;
        for (int i = 0; i < n; ++i)
        {
            const auto f = s0 + slope * i;
            const auto a = bl[(std::size_t) r], b = br[(std::size_t) r];
            zeroRun = (a == 0.0 && b == 0.0) ? zeroRun + 1 : 0;
            bl[(std::size_t) r] = 0.0, br[(std::size_t) r] = 0.0;
            const auto w = (r + D) % size;
            double yl, yr, fl, fr;
            if (mode == 0)
                yl = a, yr = b, fl = a, fr = b;
            else if (mode == 1)
                yl = b, yr = a, fl = a, fr = b;
            else
                yl = b, yr = a, fl = b, fr = a;
            bl[(std::size_t) w] += fl * f;
            br[(std::size_t) w] += fr * f;
            ol[i] += yl * gain;
            orr[i] += yr * gain;
            r = (r + 1) % size;
        }
        rpos = r;
    }
};

// effects unit / chorus (§7.3)
struct Chorus
{
    struct Tap
    {
        int ch = 0;
        float gl = 0, gr = 0;
        u32 phase = 0;
        u32 inc = 0;
        float p3 = 0, p8 = 0, p9 = 0, p10 = 0;
    };
    static constexpr double table[20] { 0.967, 1.039, 1.021, 0.1013, 0.977, 1.033, 0.997, 0.971, 1.061, 0.947,
                                        1.031, 0.1009, 1.063, 0.953, 0.991, 1.049, 0.983, 1.051, 1.019, 1.069 };
    std::vector<Tap> taps;
    int maxdelay = 8;
    std::vector<double> hl, hr; // ring (power of two)
    int mask = 0, wpos = 0;
    long long zeroRun = 1 << 30; // consecutive zero input samples

    void build (const int* p, double fxrate, int maxBlock)
    {
        const auto N = std::max (1, p[0]);
        const auto depth = F (p[1] * 0.0009765625), speed = F (p[2] * 0.0009765625);
        const auto dly = F (p[3] / 128.0), spread = F (p[4] / 128.0);
        const auto cross = F (p[5] / 128.0), vol = F (p[6] / 128.0);
        const auto A = F ((std::pow (11.0, (double) depth) - 1.0) * (1092.5 / 10.0));
        const auto B = (2147483648.0 / ((double) F (std::pow (101.0, (double) F (1.0f - speed)) - 1.0) * 75000.0 + 50000.0)) * 500.0;
        const auto Cc = F ((std::pow (11.0, (double) dly) - 1.0) * (4410.0 / 10.0));
        const auto s = F (F (std::pow (1001.0, (double) spread) - 1.0) / 1000.0f);
        auto mix = [&] (double a, double b) { return a * (double) s + b * (1.0 - (double) s); };
        const auto fsr = F (44100.0 / fxrate);
        taps.clear();
        float sumabs = 0.0f, mx = 0.0f;
        for (int i = 0; i < 2 * N; ++i)
        {
            Tap t;
            float f;
            u32 ph;
            if (i < N)
                f = (float) i, t.ch = 0, ph = 0;
            else
                f = (float) (2 * N - 1 - i), t.ch = 1, ph = 0x80000000u;
            const auto w = 1.0f - f / (float) N;
            const auto r = F (table[i % 20]);
            const auto dV = F (mix ((double) (r * r), 1.0));
            float gl = 1.0f, gr = cross * w;
            if (t.ch)
                std::swap (gl, gr);
            sumabs = F ((double) sumabs + std::abs ((double) gl) + std::abs ((double) gr));
            auto p0 = A * w + 10.0f;
            const auto d11 = mix (B * (double) dV / (double) p0, B / (double) (A + 10.0f));
            auto p3 = F (mix ((double) ((Cc * w) * dV), (double) Cc));
            ph = ph + (u32) ((std::uint64_t) rnd ((double) (F ((w * s) * 2147483648.0f) / 256.0f)) << 8);
            t.inc = (u32) (std::uint64_t) (long long) rnd (d11 * (double) fsr);
            p0 = p0 / fsr;
            auto t3 = p3 / fsr;
            if (! (t3 > 8.0f))
                t3 = 8.0f;
            p3 = p0 + t3;
            if (p3 + p0 > mx)
                mx = p3 + p0;
            const auto q = std::sqrt ((double) (p0 * 4.0f));
            t.gl = gl, t.gr = gr, t.phase = ph, t.p3 = p3;
            t.p8 = F (q / 2147483648.0), t.p9 = F (q), t.p10 = F (q / 2.0);
            taps.push_back (t);
        }
        const auto norm = (1.0f - F (F (std::pow (1001.0, (double) (1.0f - 1.0f / sumabs)) - 1.0) / 1000.0f)) * vol;
        for (auto& t : taps)
            t.gl = t.gl * norm, t.gr = t.gr * norm;
        maxdelay = (int) rnd ((double) mx) + 8;
        int size = 1;
        while (size < maxdelay + 8 + maxBlock)
            size <<= 1;
        hl.assign ((std::size_t) size, 0.0);
        hr.assign ((std::size_t) size, 0.0);
        mask = size - 1;
        wpos = 0;
    }

    // x: FX bus block; adds the chorus output to (yl, yr)
    void process (const double* xl, const double* xr, int n, double* yl, double* yr) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            hl[(std::size_t) ((wpos + i) & mask)] = xl[i];
            hr[(std::size_t) ((wpos + i) & mask)] = xr[i];
            zeroRun = (xl[i] == 0.0 && xr[i] == 0.0) ? zeroRun + 1 : 0;
        }
        // every sample this block reads is zero: the output is exactly 0, only the LFOs move
        if (zeroRun >= (long long) n + maxdelay + 2)
        {
            advance (n);
            wpos = (wpos + n) & mask;
            return;
        }
        for (auto& t : taps)
        {
            auto ph = t.phase;
            const auto& H = t.ch ? hr : hl;
            for (int i = 0; i < n; ++i)
            {
                const auto sp = (std::int32_t) ph;
                auto v = F (std::abs ((double) ((float) sp * t.p8)) - (double) t.p10);
                v = F ((double) v * ((double) t.p9 - std::abs ((double) v)) + (double) t.p3);
                ph = ph + t.inc;
                const auto k = (int) v; // (v ≥ P3 − P0 ≥ 8 > 0: truncation = floor)
                const auto a = H[(std::size_t) ((wpos + i - k) & mask)], b = H[(std::size_t) ((wpos + i - k - 1) & mask)];
                const auto smp = F (a) + (v - (float) k) * F (b - a);
                yl[i] += (double) (smp * t.gl);
                yr[i] += (double) (smp * t.gr);
            }
            t.phase = ph;
        }
        wpos = (wpos + n) & mask;
    }

    void advance (int n) noexcept
    {
        for (auto& t : taps)
            t.phase = t.phase + (u32) ((std::uint64_t) t.inc * (std::uint64_t) n);
    }
};

// 3-band EQ band (§7.6)
struct EQBand
{
    double c[5] {};
    double st[8] {};
    long long active = -1;

    void build (const std::array<int, 4>& b, double fxrate)
    {
        const auto level = b[0], freq = b[1], width = b[2], typ = b[3];
        const auto kind = typ + 6;
        const auto gainDb = (double) F (F ((float) level) * F (0.01));
        const auto fr = (double) F (freq * 1.52587890625e-05);
        auto hz = (double) F ((double) F (std::pow (101.0, fr) - 1.0) * 159.9 + 10.0);
        const auto w = (double) F (width * 1.52587890625e-05);
        const auto nyq = (double) F (fxrate * 0.4999);
        if (hz > nyq)
            hz = nyq;
        const auto om = hz * (6.283185307179586 / fxrate);
        const auto sn = std::sin (om), cs = std::cos (om);
        const auto A = std::pow (10.0, (double) F (gainDb) * 0.025);
        double b0, b1, b2, a1, a2;
        if (kind == 7)
        {
            const auto q = (double) F (std::pow (1001.0, w) - 1.0);
            const auto al = sn / ((q * 0.0999 + 0.1) * 2.0);
            const auto d = 1.0 / (al / A + 1.0);
            a1 = d * cs * -2.0, a2 = d * (1.0 - al / A), b1 = a1, b0 = d * (al * A + 1.0), b2 = d * (1.0 - al * A);
        }
        else
        {
            const auto beta = std::sqrt ((A * A + 1.0) / (w * 1.9 + 0.1) - (A - 1.0) * (A - 1.0)) * sn;
            const auto t = (A - 1.0) * cs;
            if (kind == 6)
            {
                const auto d = 1.0 / (A + 1.0 + t + beta);
                a1 = d * ((A - 1.0) + (A + 1.0) * cs) * -2.0, a2 = d * ((A + 1.0 + t) - beta);
                b1 = d * A * 2.0 * ((A - 1.0) - (A + 1.0) * cs);
                b0 = d * A * ((A + 1.0 - t) + beta), b2 = d * A * ((A + 1.0 - t) - beta);
            }
            else
            {
                const auto d = 1.0 / ((A + 1.0 - t) + beta);
                a1 = d * ((A - 1.0) - (A + 1.0) * cs) * 2.0, a2 = d * ((A + 1.0 - t) - beta);
                b1 = d * A * -2.0 * ((A - 1.0) + (A + 1.0) * cs);
                b0 = d * A * (A + 1.0 + t + beta), b2 = d * A * ((A + 1.0 + t) - beta);
            }
        }
        c[0] = b0, c[1] = b1, c[2] = b2, c[3] = a1, c[4] = a2;
        active = level != 0 ? -1 : (long long) fxrate / 5;
        std::fill (std::begin (st), std::end (st), 0.0);
    }

    void process (double* l, double* r, int n) noexcept
    {
        if (active == 0)
            return;
        const auto b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
        double* ch[2] { l, r };
        for (int k = 0; k < 2; ++k)
        {
            auto x1 = st[k], x2 = st[2 + k], y1 = st[4 + k], y2 = st[6 + k];
            auto* x = ch[k];
            for (int i = 0; i < n; ++i)
            {
                const auto xi = (double) (float) x[i];
                const auto yi = xi * b0 + ((x1 * b1 - y2 * a2) + x2 * b2) - y1 * a1;
                x[i] = (double) (float) yi;
                x2 = x1, x1 = xi, y2 = y1, y1 = yi;
            }
            st[k] = x1, st[2 + k] = x2, st[4 + k] = y1, st[6 + k] = y2;
        }
        for (auto& v : st)
            v = flush32 (v);
        if (active > 0)
        {
            active -= n;
            if (active < 1)
            {
                active = 0;
                std::fill (std::begin (st), std::end (st), 0.0);
            }
        }
    }
};

// shared plate reverb (Shared/Reverb_x64.dll behaviour, coefficients from Sytrus; §7.5)
struct Reverb
{
    static constexpr int lines[12] { 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };
    static constexpr int tapsL[7] { 266, 2974, 1913, 2496, 1690, 187, 1566 };
    static constexpr int tapsR[7] { 353, 3627, 1728, 3173, 1811, 335, 621 };
    static constexpr double colour[5] { 0.2, 0.6, 1.0, 1.5, 3.0 };

    double wet = 0, c_lc = 0, c_hc = 1, c_damp = 1, c_in = 0, c_ap1 = 0, c_ap2 = 0, g = 0, c48 = 0, c44 = 0;
    int L[12] {}, tl[7] {}, tr[7] {};
    std::array<std::vector<double>, 12> buf;
    int pos[12] {};
    std::vector<double> pre;
    int pp = 0;
    double s10 = 0, s14 = 0, s18 = 0, s3c = 0, s40 = 0, s4c = 0, s50 = 0, s54 = 0, s58 = 0;

    static long long muldiv (long long a, long long b, long long c)
    {
        const auto q = a * b;
        return q >= 0 ? q / c : -((-q) / c);
    }

    void build (const int* r, int tempo, double fxrate, double bpm)
    {
        const auto lcI = r[0], hcI = r[1], preI = r[2], room = r[3], dif = r[4], dec = r[5], dampI = r[6], col = r[7], wetI = r[8];
        const auto rate = (long long) fxrate;
        this->rate = (int) rate;
        quietIn = 0, lastPeak = 0.0, bypassed = false;
        const double lc = lcI == 19 ? 0.0 : (double) lcI;
        const double hc = hcI == 221 ? 0.0 : (double) hcI;
        const double damp = dampI == 221 ? 0.0 : (double) dampI;
        const long long pre_s = tempo ? rnd (preI * (double) rate * 0.3125 / bpm) : rnd (preI * (double) rate / 384.0);
        const auto decay = (double) F (dec * (double) rate / 10.0);
        wet = (double) F (wetI * 0.00390625);
        const auto f17 = (double) F (1.0 / (double) rate);
        const auto half = (double) F ((double) rate * 0.5);
        if (lc == 0.0)
            c_lc = 0.0;
        else
        {
            const auto t = (double) F (std::tan (std::min (lc, half) * kPi * f17));
            c_lc = (double) F ((t - 1.0) / (t + 1.0));
        }
        auto lp = [&] (double v) {
            if (v == 0.0)
                return 1.0;
            const auto x = (double) F (2.0 - std::cos (std::min (v, half) * 628.3185307179587 * f17));
            return (double) F (1.0 - (x - std::sqrt (x * x - 1.0)));
        };
        c_hc = lp (hc);
        c_damp = lp (damp);
        const auto sc = muldiv (rate, room + 15, 115);
        for (int i = 0; i < 12; ++i)
            L[i] = (int) std::max<long long> (1, muldiv (lines[i], sc, 20000));
        for (int i = 0; i < 7; ++i)
            tl[i] = (int) muldiv (tapsL[i], sc, 20000), tr[i] = (int) muldiv (tapsR[i], sc, 20000);
        const auto dv = (double) F (dif * 0.01);
        const auto f13 = (double) F (f17 * 0.5);
        if (dv == 0.0)
            c_in = c_ap1 = c_ap2 = 0.0;
        else
        {
            auto e = [&] (int a, int b, double k) {
                return (double) F (std::exp (std::log (0.001) * (double) F ((a + b) * f13) / (dv * k)));
            };
            c_in = e (L[0], L[1], 0.07);
            c_ap1 = e (L[4], L[8], 0.35);
            c_ap2 = e (L[6], L[10], 0.51);
        }
        const auto ssum = (double) (L[5] + L[7] + L[9] + L[11]);
        const auto ln001 = std::log (0.001);
        g = (double) F (std::pow (std::exp (ln001 * ssum / decay), 0.25));
        const auto fc = (col >= 0 && col <= 4) ? colour[col] : 1.0;
        auto r2 = (double) F ((double) F (std::pow (std::exp (ln001 * ssum / (double) F (decay * fc)), 0.25)) / g);
        c48 = (double) F ((r2 - 1.0) * 0.5);
        if (r2 > 1.0)
            r2 = 1.0;
        const auto t = (double) F (std::tan (500.0 * kPi * f17));
        c44 = (double) F ((t - r2) / (t + r2));
        for (int i = 0; i < 12; ++i)
            buf[(std::size_t) i].assign ((std::size_t) L[i], 0.0), pos[i] = 0;
        pre.assign ((std::size_t) std::max<long long> (1, pre_s), 0.0);
        pp = 0;
        s10 = s14 = s18 = s3c = s40 = s4c = s50 = s54 = s58 = 0.0;
    }

    // after 2 s of zero input with an output below 1e-9 the tank is cleared and the unit stops working until the
    // input comes back (the tail left is below −180 dB; the DLL keeps computing it)
    long long quietIn = 0;
    double lastPeak = 0.0;
    bool bypassed = false;
    int rate = 44100;

    void clear() noexcept
    {
        for (auto& b : buf)
            std::fill (b.begin(), b.end(), 0.0);
        std::fill (pre.begin(), pre.end(), 0.0);
        s10 = s14 = s18 = s3c = s40 = s4c = s50 = s54 = s58 = 0.0;
    }

    // x: mono input; adds wet·output to (ol, or)
    void process (const double* xl, const double* xr, int n, double* ol, double* orr) noexcept
    {
        bool inZero = true;
        for (int s = 0; s < n && inZero; ++s)
            inZero = xl[s] + xr[s] == 0.0;
        quietIn = inZero ? quietIn + n : 0;
        if (! inZero)
            bypassed = false;
        else if (! bypassed && quietIn > 2LL * rate && lastPeak < 1e-9)
        {
            bypassed = true;
            clear();
        }
        if (bypassed)
            return;
        double peak = 0.0;
        auto ap = [&] (int i, double v, double c) {
            const auto p = pos[i];
            auto& B = buf[(std::size_t) i];
            const auto w = B[(std::size_t) p] - c * v;
            B[(std::size_t) p] = c * w + v;
            pos[i] = p > 0 ? p - 1 : L[i] - 1;
            return w;
        };
        auto dl = [&] (int i, double v) {
            const auto p = pos[i];
            auto& B = buf[(std::size_t) i];
            const auto r = B[(std::size_t) p];
            B[(std::size_t) p] = v;
            pos[i] = p > 0 ? p - 1 : L[i] - 1;
            return r;
        };
        auto tap = [&] (int i, int off) {
            const auto k = off + pos[i];
            return buf[(std::size_t) i][(std::size_t) (k >= L[i] ? k - L[i] : k)];
        };
        const auto t1 = 1.0 - g;
        const auto w74 = t1 * 0.6 + g, w6c = t1 * 0.25 + g, w78 = t1 * 0.7 + g, w68 = t1 * 0.2 + g, w70 = t1 * 0.5 + g;
        const auto plen = (int) pre.size();
        for (int s = 0; s < n; ++s)
        {
            const auto x = xl[s] + xr[s];
            auto a = ((x + 1e-30) - s10) * c_hc + s10;
            s10 = a;
            const auto h = (a - s14) * c_lc + s18;
            s18 = a;
            s14 = h;
            if (c_lc != 0.0)
                a = (a - h) * 0.5 + 1e-30;
            for (int i = 0; i < 4; ++i)
                a = ap (i, a, c_in);
            const auto z = pre[(std::size_t) pp];
            pre[(std::size_t) pp] = a;
            pp = pp > 0 ? pp - 1 : plen - 1;
            // half 1
            auto u = buf[11][(std::size_t) pos[11]] * g;
            auto f = (u - s50) * c44 + s4c;
            s4c = u, s50 = f;
            auto v = (((f + u) * c48 + u + z) - s3c) * c_damp + s3c;
            s3c = v;
            auto w = ap (4, v, c_ap1);
            auto rr = dl (5, w) * g;
            auto w2 = ap (6, rr, c_ap2);
            {
                const auto p = pos[7];
                buf[7][(std::size_t) p] = w2;
                pos[7] = p > 0 ? p - 1 : L[7] - 1;
            }
            // half 2
            u = buf[7][(std::size_t) pos[7]] * g;
            f = (u - s58) * c44 + s54;
            s54 = u, s58 = f;
            v = (((f + u) * c48 + u + z) - s40) * c_damp + s40;
            s40 = v;
            w = ap (8, v, c_ap1);
            rr = dl (9, w) * g;
            w2 = ap (10, rr, c_ap2);
            {
                const auto p = pos[11];
                buf[11][(std::size_t) p] = w2;
                pos[11] = p > 0 ? p - 1 : L[11] - 1;
            }
            const auto outL = (((tap (9, tl[0]) - w6c * tap (9, tl[1])) - w78 * tap (10, tl[2])) - w68 * tap (11, tl[3]))
                              + w74 * tap (5, tl[4]) + tap (6, tl[5]) + w70 * tap (7, tl[6]);
            const auto outR = ((((w6c * tap (5, tr[1]) + tap (5, tr[0])) - w78 * tap (6, tr[2])) - w68 * tap (7, tr[3]))
                               - w74 * tap (9, tr[4]))
                              + tap (10, tr[5]) + w70 * tap (11, tr[6]);
            ol[s] += outL * wet;
            orr[s] += outR * wet;
            peak = std::max (peak, std::abs (outL * wet) + std::abs (outR * wet));
        }
        lastPeak = peak;
    }
};

struct DCBlock
{
    double a = 0, b = 0, st[4] {};
    void build (double fs)
    {
        a = (double) F (1.0070234999999998 / fs);
        b = (double) F (0.96);
        std::fill (std::begin (st), std::end (st), 0.0);
    }
    void process (double* l, double* r, int n) noexcept
    {
        double* ch[2] { l, r };
        for (int c = 0; c < 2; ++c)
        {
            auto s = st[2 * c], v = st[2 * c + 1];
            auto* x = ch[c];
            for (int i = 0; i < n; ++i)
            {
                const auto xi = (double) (float) x[i];
                v = v + (xi - s) * a;
                s = s + v;
                v = v * b;
                x[i] = (double) (float) (xi - s);
            }
            st[2 * c] = s, st[2 * c + 1] = v;
        }
    }
};

// Gen_Render's post-voice part (§7.1): M/F at fs·2^order → FX at fs·2^min(order,2) → fs
struct Post
{
    int order = 0, fo = 0;
    std::vector<Decim2> decM, decF;
    bool hasChorus = false, chorusToFx = false, hasReverb = false, hasEq = false;
    int eqmode = 0;
    Chorus chorus;
    std::vector<Delay> delays;
    Reverb reverb;
    std::array<EQBand, 3> eq;
    bool dcOn = false;
    DCBlock dc;
    Stereo T; // scratch

    void build (const StateRaw& S, double fs, int ord, int maxBlock, double bpm, bool renderMode)
    {
        order = ord;
        fo = std::min (ord, 2);
        const auto fxrate = fs * (1 << fo);
        const auto fxBlock = maxBlock * (1 << fo);
        decM.assign ((std::size_t) order, Decim2 {});
        decF.assign ((std::size_t) order, Decim2 {});
        int ch[7];
        for (int i = 0; i < 7; ++i)
            ch[i] = S.mget (181 + i);
        hasChorus = ch[0] >= 1;
        if (hasChorus)
            chorus.build (ch, fxrate, fxBlock);
        DelayRec recs[3];
        int lastOn = -1;
        for (int k = 0; k < 3; ++k)
        {
            const auto& d = S.delay[(std::size_t) k];
            if (d.size() < 23)
                continue;
            std::int32_t v[5];
            std::memcpy (v, d.data(), 20);
            recs[k] = { v[0], v[1], v[2], v[3], v[4], d[20], d[21], d[22] };
            if (recs[k].on)
                lastOn = k;
        }
        hasReverb = S.rev.size() >= 37 && S.rev[36] != 0;
        const auto lim = hasReverb ? 3 : lastOn;
        delays.clear();
        for (int k = 0; k < 3; ++k)
            if (recs[k].on)
            {
                Delay d;
                d.build (recs[k], fxrate, fxBlock, bpm, renderMode);
                d.toFx = recs[k].serial != 0 && k < lim;
                delays.push_back (std::move (d));
            }
        const auto sendTrack = S.mget (189);
        chorusToFx = lastOn >= 0 || hasReverb || sendTrack != -1;
        if (hasReverb)
        {
            int rv[9];
            std::memcpy (rv, S.rev.data(), 36);
            reverb.build (rv, S.rev.size() > 37 ? S.rev[37] : 0, fxrate, bpm);
        }
        eqmode = S.eqmode;
        hasEq = eqmode != 0;
        if (hasEq)
            for (int k = 0; k < 3 && k < (int) S.eq.size(); ++k)
                eq[(std::size_t) k].build (S.eq[(std::size_t) k], fxrate);
        dcOn = S.b4f5 != 0;
        dc.build (fs);
        T.resize ((std::size_t) fxBlock);
    }

    int chorusRing() const noexcept { return hasChorus ? chorus.maxdelay : 0; }

    void eqProcess (double* l, double* r, int n) noexcept
    {
        for (auto& b : eq)
            b.process (l, r, n);
    }

    // M, F: n·2^order samples (modified in place); output: the first n samples of M
    void process (double* Ml, double* Mr, double* Fl, double* Fr, int nv, bool active) noexcept
    {
        auto len = nv;
        for (int k = order - 1; k >= fo; --k)
        {
            decM[(std::size_t) k].process (Ml, Mr, len);
            decF[(std::size_t) k].process (Fl, Fr, len);
            len /= 2;
        }
        if (hasEq && eqmode >= 2)
        {
            if (eqmode == 3)
                eqProcess (Fl, Fr, len);
            else
                eqProcess (Ml, Mr, len);
        }
        if (hasChorus)
        {
            if (active)
            {
                std::fill (T.l.begin(), T.l.begin() + len, 0.0);
                std::fill (T.r.begin(), T.r.begin() + len, 0.0);
                chorus.process (Fl, Fr, len, T.l.data(), T.r.data());
                for (int i = 0; i < len; ++i)
                    Ml[i] += T.l[(std::size_t) i], Mr[i] += T.r[(std::size_t) i];
                if (chorusToFx)
                    for (int i = 0; i < len; ++i)
                        Fl[i] += T.l[(std::size_t) i], Fr[i] += T.r[(std::size_t) i];
            }
            else
                chorus.advance (len);
        }
        for (auto& d : delays)
        {
            if (active)
                d.write (Fl, Fr, len);
            if (d.toFx)
                d.process (len, Fl, Fr);
            else
                d.process (len, Ml, Mr);
        }
        if (hasReverb)
            reverb.process (Fl, Fr, len, Ml, Mr);
        if (hasEq && eqmode == 1)
            eqProcess (Ml, Mr, len);
        for (int k = fo - 1; k >= 0; --k)
        {
            decM[(std::size_t) k].process (Ml, Mr, len);
            len /= 2;
        }
        if (dcOn)
            dc.process (Ml, Mr, len);
    }
};

// ------------------------------------------------------------------------------------------------------------------
// voices (§2, §3, §6)

struct OpV
{
    bool active = false, persample = false, noiseOn = false, pluck = false, bandlimit = false, dc = false, globalPhase = false;
    float outGain = 0, fxGain = 0, panCell = 0, vol = 0, prange = 0, n10c = 0, n110 = 0, n108 = 0;
    double k8d8 = 0;
    float k8d0 = 0, startph = 0;
    int nfm = 0, fmT[6] {};
    float fmAmt[6] {};
    int nrm = 0, rmT[6] {};
    float rmA[6] {}, rmB[6] {};
    float art[6] {};
    float mod = 0, rmacc = 0, fmacc = 0, ptf = 1, phArt = 0, fmInc = 0, curVol = 0, volInc = 0, modInc = 0, freq = 0, fr = 0, inc = 0;
    double phi = 0;
    const float* table = nullptr;
    int N = 0, cacheSlot = -1;
    bool attached = false;
    double dcs = 0, dcv = 0, dck1 = 0, dck2 = 0;
    int dkN = -1;
    float dkC = 0, dkI = 0, dkY = 0;
    float L = 0, R = 0, pL = 0, pR = 0, FL = 0, FR = 0, pFL = 0, pFR = 0;
    double angO = -9.0, cosO = 0.0, sinO = 0.0, angF = -9.0, cosF = 0.0, sinF = 0.0;
    float ptfArt = 0.0f; // articulator 3 value of the cached ptf
    float damp = 0;
    int noiseAmt = 0;
    // plucked string (§2.6)
    std::vector<float> pbuf;
    int plen = 0, pptr = 0;
    float pa = 0, pb = 0, pbCache = 0, p12c = 0, p148 = 0, p16c = 0, p168 = 0;
    bool pbCacheSet = false;
    long long p15c = 0, p164 = 1;
    u32 p160 = 0, p154 = 0;
};

struct FiltV
{
    bool active = false;
    int nin = 0, inSrc[7] {};
    float inG[7] {};
    float outGain = 0, fxGain = 0, panCell = 0;
    float L = 0, R = 0, pL = 0, pR = 0, FL = 0, FR = 0, pFL = 0, pFR = 0;
    double angO = -9.0, cosO = 0.0, sinO = 0.0, angF = -9.0, cosF = 0.0, sinF = 0.0;
    float vprev = 0, wsPrev = 0;
    bool wsPrevSet = false;
    FiltState st;
};

struct Voice
{
    bool used = false;
    int state = 0;
    long long group = 0, id = 0, age = 0;
    int key = 0;
    bool isSlave = false;
    double fs = 44100;
    float vel = 0.78125f, relvel = 1.0f, fvol = 1, fpan = 0, notePan = 0, initVol = 1;
    float pitch = 0, master = 1, base = 0, rampMax = 0, panMax = 0;
    float uvol = 1, upan = 0.5f, uphase = 0, udet = 1, envvar = 1.0f / 128, fxpan = 0, w34 = 1;
    double unisonX = 0.5;
    bool pitchSub = false, gpitch = false, slot6 = false;
    bool hasPortaD = false, hasPortaKey = false;
    float portaD = 0, portaKey = 0;
    int tcnt = 0, tick = 96, gridTcnt = 0;
    std::array<ArtSt, kNumArts> arts;
    std::array<OpV, 6> ops;
    std::array<FiltV, 3> filts;
    Voice* slaves[18] {};
    int nslaves = 0;
    u32 lcg = 0;                // the noise generator's position for this voice in the current block
    Voice* lcgOwner = nullptr;  // grouped unison: slaves draw from their master's sequence
    int noiseOps = 0;           // noise operators rendered per sample
    float notePitch = 0;        // `pitch` at the note on
    double noteFinal = 0;       // the note's Final.Pitch (cents from C5) at the note on
    HostSlide slide;            // a slide note moving the pitch (issue #14)

    float out (int u, int k) const noexcept { return (float) arts[(std::size_t) (kArtBase[u] + k)].out; }
    bool artActive (int u, int k) const noexcept { return arts[(std::size_t) (kArtBase[u] + k)].active(); }
};

struct Uni
{
    float pan = 0.5f, vol = 1, det = 1, phase = 0, envvar = 1.0f / 128;
    double x = 0.5;
    bool sub = false;
};

const double kUniDet[4][9] { { 0.877, 1.069, 0.929, 1.181, 1.031, 1.129, 0.887, 1.117, 0.977 },
                             { 0.877, 1.069, 0.929, 1.181, 1.031, 1.129, 0.887, 1.117, 0.977 },
                             { 1, 1, 1, 1, 1, 1, 1, 1, 1 },
                             { 0.890472, 1.076426, 0.911577, 1.102536, 1.034126, 1.166861, 0.853971, 1.206273, 1.0 } };
const double kUniEnv[9] { 0.9877, 1.0069, 0.9929, 1.0181, 1.0031, 1.0129, 0.9887, 1.0117, 0.9977 };

// per-voice unison parameters in creation order (TriggerVoice, §6.3)
int unisonParams (const StateRaw& S, Uni* res)
{
    const auto n = std::clamp (S.mget (173), 1, 9);
    const auto pan = S.mget (174);
    const auto volk = (float) S.mget (175) * F (1.0 / 128);
    const auto pit = S.mget (176);
    const auto sub = S.mget (177);
    const auto phk = (float) S.mget (178) * F (1.0 / 128);
    const auto ev = S.mget (179);
    const auto alt = S.b4f7at (8);
    double dd;
    const auto px = (double) ((float) pit * F (1.0 / 256));
    if (alt == 0)
        dd = (double) F (std::exp (px * std::log (11.0)) - 1.0) * 5.0 * kLn2_1200;
    else
        dd = (double) F (std::exp (px * std::log (41.0)) - 1.0) * 1.875 * kLn2_1200;
    const auto d = F (dd);
    auto w = F (1.0 / n);
    if (n > 1)
        w = F (1.0 - (double) F (std::exp ((1.0 - (double) w) * std::log (3.0)) - 1.0) * (double) F (0.5));
    float ws[2] { w, 0 };
    int nw = 1;
    if (sub)
    {
        const auto sk = (float) sub * F (1.0 / 128);
        ws[0] = w * (1.0f - sk), ws[1] = w * sk, nw = 2;
    }
    int count = 0;
    for (int pas = 0; pas < nw; ++pas)
        for (int e = 0; e < n; ++e)
        {
            const auto wv = ws[pas];
            Uni u;
            if (n == 1)
            {
                u.pan = 0.5f, u.vol = wv, u.det = 1.0f, u.phase = 0.0f, u.x = 0.5, u.envvar = F (1.0 / 128);
            }
            else
            {
                u.phase = ((float) e / (float) n) * phk;
                const auto b3 = (float) e / (float) (n - 1);
                u.x = (double) b3;
                const auto x = b3 * 2.0f - 1.0f;
                const auto u50 = x * F (1.0 / 256);
                u.pan = F ((double) (u50 * (float) pan) + 0.5);
                const auto ax = std::abs (x);
                const auto u58 = (alt && n == 2) ? wv : (wv * 2.0f) * (1.0f - ax);
                u.vol = wv * volk + u58 * (1.0f - volk);
                auto sx = ax;
                if (e & 1)
                    sx = -sx;
                if (alt && (n & 1) && n < 2 * e)
                    sx = -sx;
                const auto& row = kUniDet[alt >= 0 && alt < 4 ? alt : 0];
                const auto x4c = sx * F (row[(e + pas) % 9]);
                u.det = F (std::exp ((double) (x4c * d)));
                if (alt == 3)
                {
                    const auto p2 = (double) (phk * phk);
                    auto dv = p2 * p2;
                    dv = dv * (double) (u.det * 100000.0f) * dv;
                    if (pas > 0)
                        dv *= 0.5;
                    u.phase = std::abs (dv) < 4503599627370496.0 ? F (dv - std::trunc (dv)) : 0.0f;
                }
                u.envvar = F ((((double) ax - 0.5) * ev * (double) F (kUniEnv[(e + pas) % 9]) * (double) F (1.0 / 128) + 1.0)
                              * (double) F (1.0 / 128));
            }
            if (pas > 0)
            {
                u.det = u.det * 0.5f;
                u.sub = true;
            }
            res[count++] = u;
        }
    return count;
}

// band-limited table cache entry (§2.4)
struct BandTable
{
    int op = -1, lo = 0, hi = 0, M = 0, users = 0;
    std::uint64_t stamp = 0;
    std::vector<float> t;
};
} // namespace

// ------------------------------------------------------------------------------------------------------------------

struct Engine::Impl
{
    std::shared_ptr<const Patch> keep;
    const Patch* P = nullptr;
    double fs = 44100.0;
    bool renderMode = true;
    int maxBlock = 512, order = 0, K = 1, tick = 96, maxSub = 96;
    double fsv = 44100.0, bpm = 120.0;
    std::array<FiltDef, 3> fdef;
    double ttEnv[kNumArts] {}, ttLfo[kNumArts] {};
    long long lfoInc[kNumArts] {};
    bool glfoOn[kNumArts] {};
    u32 glfoPhase[kNumArts] {};
    u32 noise = 0, randseed = 0;
    long long counter = 0, groupSerial = 0, ageSerial = 0;
    bool hasLastPitch = false;
    float lastPitch = 0;
    std::vector<Voice> pool;
    std::vector<Voice*> act; // top-level voices in creation order (rendered last to first)
    static constexpr int kBatch = 4; // voices rendered in lockstep (their per-sample chains interleave)
    std::array<std::array<std::vector<float>, 6>, kBatch> opBufs;
    std::array<std::vector<float>, 6>* ob = &opBufs[0]; // operator buffers of the voice being rendered
    std::array<std::vector<float>, 6> acc;
    std::array<std::vector<float>, 3> fBuf;
    std::vector<float> xin, sh, gains;
    FiltScratch fw;
    std::vector<float> Ml, Mr, Fl, Fr;
    std::vector<double> Mdl, Mdr, Fdl, Fdr;
    std::vector<BandTable> bands;
    std::uint64_t bandStamp = 0;
    Post post;
    long long fxTail = 0, tailLen = 0, silentOut = 0;
    bool runningTime = false;
    bool computeRunningTime() const noexcept;
    std::array<std::vector<float>, 6> overrides;
    std::array<std::vector<cd>, 6> overrideSpectrum;

    // ---------------------------------------------------------------- configuration
    double tickTime (bool tempo) const noexcept
    {
        const auto ratio = tick / (fsv * 96.0 / 44100.0);
        const auto sec = ratio * 96.0 / 44100.0;
        return tempo ? sec * bpm / 60.0 : sec * 2.0;
    }

    void computeTimes() noexcept
    {
        if (P == nullptr)
            return;
        for (int i = 0; i < kNumArts; ++i)
        {
            const auto& d = P->arts[(std::size_t) i];
            ttEnv[i] = tickTime ((d.envFlags & 1) != 0);
            ttLfo[i] = tickTime ((d.lfoFlags & 1) != 0);
            auto r = std::exp ((double) F (d.lfoSpeed / 65536.0) * std::log (10001.0)) - 1.0;
            r = (double) F (r) * 6871947.6736 * ttLfo[i];
            r = std::min (r, 805306368.0);
            lfoInc[i] = (d.lfoFlags & 8) ? 0 : rnd (r);
        }
    }

    void configure()
    {
        if (P == nullptr)
            return;
        const auto& S = P->S;
        order = std::clamp (S.mget (renderMode ? 196 : 195), 0, 4);
        K = 1 << order;
        const auto tix = S.mget (renderMode ? 194 : 193);
        const int T0 = tix == 1 ? 8 : 96;
        fsv = fs * K;
        tick = std::max (4, 4 * (int) rnd (T0 * fsv / 44100.0 / 4.0));
        maxSub = tick;
        for (int k = 0; k < 3; ++k)
            fdef[(std::size_t) k].build (S.units[(std::size_t) (6 + k)], fsv);
        computeTimes();
        for (int i = 0; i < kNumArts; ++i)
        {
            const auto& d = P->arts[(std::size_t) i];
            glfoOn[i] = d.lfoOn && (d.lfoFlags & 2) != 0;
            glfoPhase[i] = 0xC0000000u; // §4.5: the plugin-wide phase starts at ¾ cycle
        }
        for (auto& set : opBufs)
            for (auto& b : set)
                b.assign ((std::size_t) maxSub + 4, 0.0f);
        for (auto& b : acc)
            b.assign ((std::size_t) maxSub + 4, 0.0f);
        for (auto& b : fBuf)
            b.assign ((std::size_t) maxSub + 4, 0.0f);
        xin.assign ((std::size_t) maxSub + 4, 0.0f);
        sh.assign ((std::size_t) maxSub + 4, 0.0f);
        gains.assign ((std::size_t) maxSub + 4, 0.0f);
        fw.resize (2 * maxSub + 8);
        const auto nv = (std::size_t) (maxBlock * K);
        Ml.assign (nv, 0.0f), Mr.assign (nv, 0.0f), Fl.assign (nv, 0.0f), Fr.assign (nv, 0.0f);
        Mdl.assign (nv, 0.0), Mdr.assign (nv, 0.0), Fdl.assign (nv, 0.0), Fdr.assign (nv, 0.0);
        post.build (S, fs, order, maxBlock, bpm, renderMode);
        // FX tail (§7.1 step 2): 100 host-rate samples' worth + the chorus ring (its longest delay plus one block, taken
        // as the ring's length — the block term is inferred from EXEC: without it the chorus tail of demo channel 6 is
        // cut while the DLL's continues)
        silentOut = 0;
        runningTime = computeRunningTime();
        tailLen = (long long) (fs * K * 100 / 44100) + (long long) (post.hasChorus ? post.chorusRing() + maxBlock * (1 << post.fo) : 0) * K / (1 << post.fo);
        fxTail = 0;
        // voice pool: 16 note groups of the patch's unison size (×2 with the sub layer), at least 32 voices
        const auto per = std::clamp (S.mget (173), 1, 9) * (S.mget (177) > 0 ? 2 : 1);
        const auto poolSize = (std::size_t) std::clamp (per * 16, 32, 512);
        pool.assign (poolSize, Voice {});
        for (auto& v : pool)
            for (int u = 0; u < 6; ++u)
                if (P->S.units[(std::size_t) u].op[0x826] != 0)
                    v.ops[(std::size_t) u].pbuf.reserve (4096);
        act.clear();
        act.reserve (poolSize);
        bands.clear();
        bands.reserve (32);
        counter = 0;
        noise = 0;
        hasLastPitch = false;
    }

    // ---------------------------------------------------------------- tables
    const std::vector<float>& fullTable (int u) const noexcept
    {
        return overrides[(std::size_t) u].empty() ? P->wave[(std::size_t) u] : overrides[(std::size_t) u];
    }

    const std::vector<cd>& spectrum (int u)
    {
        auto& o = overrides[(std::size_t) u];
        if (! o.empty())
        {
            auto& s = overrideSpectrum[(std::size_t) u];
            if (s.empty())
            {
                std::vector<double> x (o.begin(), o.begin() + kN);
                s = rfft (x.data(), (std::size_t) kN);
            }
            return s;
        }
        return P->spectrum[(std::size_t) u];
    }

    void releaseTable (OpV& o) noexcept
    {
        if (o.cacheSlot >= 0)
            --bands[(std::size_t) o.cacheSlot].users;
        o.cacheSlot = -1;
    }

    // band-limited copy for an operator sounding at f_eff (FUN_0086ace0, §2.4); nullptr = use the full table
    const float* bandLimited (int u, float fr, double fsVoice, int& N, int& slot)
    {
        const auto fhz = std::max (0.1, (double) (fr * (float) fsVoice));
        const auto inv = (double) F (1.0 / fhz);
        const auto nyq = P->gibbs ? 22050.0 : (double) ((float) fsVoice * 0.5f);
        const auto d = (nyq - 18000.0) * (double) F (0.25);
        const auto lo = (long long) ((d + 18000.0) * inv) + 1;
        auto hi = (long long) ((d * 3.0 + 18000.0) * inv) + 1;
        const int Nb = renderMode ? 16384 : 4096;
        if (lo > Nb / 2)
            return nullptr;
        hi = std::min<long long> (hi, Nb / 2);
        for (std::size_t i = 0; i < bands.size(); ++i)
        {
            auto& b = bands[i];
            if (b.op == u && b.lo == lo && b.hi == hi && b.M == Nb && ! b.t.empty())
            {
                ++b.users;
                b.stamp = ++bandStamp;
                slot = (int) i;
                N = Nb;
                return b.t.data();
            }
        }
        // build: reuse an unused slot (oldest) or add one
        int pick = -1;
        for (std::size_t i = 0; i < bands.size(); ++i)
            if (bands[i].users == 0 && (pick < 0 || bands[i].stamp < bands[(std::size_t) pick].stamp))
                pick = (int) i;
        if (pick < 0 || bands.size() < 24)
        {
            bands.emplace_back();
            pick = (int) bands.size() - 1;
        }
        auto& b = bands[(std::size_t) pick];
        b.op = u, b.lo = (int) lo, b.hi = (int) hi, b.M = Nb, b.users = 1, b.stamp = ++bandStamp;
        const auto& full = spectrum (u);
        std::vector<cd> X ((std::size_t) Nb / 2 + 1);
        const auto scale = (double) Nb / 16384.0;
        for (int k = 0; k <= Nb / 2; ++k)
            X[(std::size_t) k] = full[(std::size_t) k] * scale;
        std::vector<double> g ((std::size_t) Nb / 2 + 1, 0.0);
        for (long long k = 0; k < lo && k <= Nb / 2; ++k)
            g[(std::size_t) k] = 1.0;
        if (lo <= hi)
        {
            const auto kk = hi - lo + 1;
            const auto step = F (-1.0 / (double) kk);
            for (long long i = 0; i < kk; ++i)
                g[(std::size_t) (lo + i)] = (double) ((1.0f + step) + F ((double) step * (double) i));
        }
        if (P->gibbs && hi > 0)
            for (long long k = 1; k <= hi; ++k)
                g[(std::size_t) k] *= ((double) hi * 0.3183098861837907) * std::sin ((double) k * (kPi / (double) hi)) / (double) k;
        for (int k = 0; k <= Nb / 2; ++k)
            X[(std::size_t) k] *= g[(std::size_t) k];
        const auto y = irfft (X, (std::size_t) Nb);
        b.t.resize ((std::size_t) Nb + 1);
        for (int i = 0; i < Nb; ++i)
        {
            auto v = F (y[(std::size_t) i]);
            if (std::abs (v) < kTiny)
                v = 0.0f;
            b.t[(std::size_t) i] = v;
        }
        b.t[(std::size_t) Nb] = b.t[0];
        slot = pick;
        N = Nb;
        return b.t.data();
    }

    // ---------------------------------------------------------------- voice set-up (Voice.__init__ / FullArts / _setup)
    struct NoteCtx
    {
        float key, cents, vel, relvel, fvol, fpan;
        bool porta, portaKey;
        float portaD, portaKeyPitch;
    };

    void artsInit (Voice& v)
    {
        const auto& S = P->S;
        const auto ml = gCurve (S.mget (1), 128);
        for (int i = 0; i < kNumArts; ++i)
        {
            const auto& d = P->arts[(std::size_t) i];
            auto& a = v.arts[(std::size_t) i];
            a.d = &d;
            a.mode = d.mode;
            a.base = d.base;
            a.envdepth = (double) F (d.fileEnvDepth);
            a.lfodepth = (double) ml;
            a.env.setup (&d.env, ttEnv[i], d.knobs[0] / 128.0, d.knobs[1] / 128.0, d.knobs[2] / 128.0, d.knobs[3] / 128.0);
            a.lfo.env.setup (&d.lfoCurve, ttLfo[i], 1.0, 1.0, 0.0, 1.0);
            a.lfo.wave = &d.wave;
            a.lfo.inc = lfoInc[i];
            a.lfo.on = d.lfoOn;
            a.lfo.out = 0.0;
            a.lfo.phase = glfoOn[i] ? glfoPhase[i] : d.lfoStart;
            if (d.unit >= 6 && d.unit <= 8 && d.index == 2) // FUN_00869e80: cutoff depth from p1
            {
                a.envdepth = (double) ((float) S.units[(std::size_t) d.unit].p[1] * F (1.0 / 128));
                a.lfodepth = (double) (F (a.envdepth) * ml);
            }
            a.ntabs = 0;
            a.first = true;
            a.relMult = 1.0;
        }
    }

    void artNoteOn (Voice& v, ArtSt& a, double key, double vel, double unisonX, double unisonVar, double relMult, int velflags,
                    bool hasRand, double rand)
    {
        const auto& d = *a.d;
        const auto& S = P->S;
        const auto& m = d.maps;
        a.stat = a.combine (m[0].mapValue (key / 120.0), m[1].mapValue (vel));
        if (hasRand && ! d.mapDefault[4])
            a.stat = a.combine (a.stat, m[4].mapValue (rand));
        a.stat = a.combine (a.stat, m[5].mapValue (unisonX));
        a.ntabs = 0;
        const int mxy[2] { S.mget (190), S.mget (191) };
        for (int j = 0; j < 2; ++j)
            if (! d.mapDefault[(std::size_t) (2 + j)])
            {
                const auto ix = std::clamp<long long> (rnd (mxy[j] + 0.0 * 256.0), 0, 512); // Final.FCut/FRes = 0 in LYNX
                a.tabs[a.ntabs++] = m[(std::size_t) (2 + j)].mapValue ((double) ix / 512.0);
            }
        auto& e = a.env;
        e.init();
        a.lfo.env.init();
        a.lfo.env.applyKnobs();
        a.lfo.out = 0.0;
        double pre = 0.0;
        const bool on = e.c->on && ! e.c->pts.empty();
        if (on)
            pre = e.out;
        const bool glob = d.index == 1 || (d.unit >= 6 && d.unit <= 8 && (d.index == 2 || d.index == 3));
        if (on && glob)
        {
            const int g0 = d.index == 1 ? 165 : 169;
            const auto gA = S.mget (g0), gD = S.mget (g0 + 1), gS = S.mget (g0 + 2), gR = S.mget (g0 + 3);
            const auto b0 = F (unisonVar / 128.0);
            e.knobs[0] = (double) (F (e.knobs[0]) * (float) gA * b0);
            e.knobs[1] = (double) F ((double) (F (e.knobs[1]) * (float) gD) * (0.015625 - (double) b0));
            e.knobs[2] = (double) (F (e.knobs[2]) + (float) gS * F (0.0078125));
            e.knobs[3] = (double) (F (e.knobs[3]) * (float) gR * F (0.0078125));
            e.applyKnobs();
        }
        if (on && velflags)
        {
            if (velflags & 1)
            {
                e.knobs[0] = (double) (F (e.knobs[0]) * (2.0f - F (vel) * 2.0f));
                e.applyKnobs();
            }
            if ((velflags & 2) && e.c->decay >= 0)
            {
                const auto adv = e.c->cum (e.c->decay) * e.knobs[0] * (double) F (vel) / e.ttime;
                e.process (adv);
            }
        }
        if (on && ! glob)
            e.applyKnobs();
        a.relMult = relMult;
        noteOnValue (a, on, pre);
        a.first = true;
        (void) v;
    }

    // the articulator's value at note-on: base + first envelope level (pre) × depth, LFO, maps
    static void noteOnValue (ArtSt& a, bool on, double pre) noexcept
    {
        auto val = a.base;
        if (on)
            val = (double) (F (val) + F (pre) * F (a.envdepth));
        if (a.lfo.on)
        {
            if (a.mode == 1)
                val = (double) (F (val) + F (a.lfo.out) * F (a.lfodepth));
            else
                val = (double) (F (val) * (F (a.lfo.out) * F (a.lfodepth) * 2.0f + 1.0f));
        }
        auto o = ! (on || a.lfo.on) ? a.stat : a.combine (val, a.stat);
        for (int i = 0; i < a.ntabs; ++i)
            o = a.combine (o, a.tabs[i]);
        a.out = o;
    }

    void porta (Voice& v, int u, ArtSt& a, float dIn)
    {
        const auto& S = P->S;
        const auto mode = S.b4f7at (4);
        auto d = dIn;
        if (mode)
        {
            const auto sg = d == 0 ? 0.0 : std::copysign (1.0, (double) d);
            if (mode < 2)
            {
                const auto step = F (sg * 1200.0);
                int guard = 0;
                while (std::abs ((double) d) > 1250.0 && ++guard < 10000)
                    d = d - step;
            }
            else
            {
                const auto lim = F ((6 - mode) * 100);
                d = F (sg * (double) std::min (std::abs (d), lim));
            }
        }
        if (d == 0)
            return;
        auto& e = a.env;
        if (e.stage < 2)
            e.mod = (double) F ((double) d * 0.0004166666666666667);
        else
            e.mod = e.susmod;
        e.out = (double) F (e.c->level (0, e.mod));
        if (S.b4f7at (2))
        {
            e.knobs[0] = (double) F ((double) F (e.knobs[0]) * std::abs ((double) d) * (double) S.units[(std::size_t) u].p[2]
                                     * 6.944444444444445e-07);
            e.applyKnobs();
            e.out = (double) F (e.c->level (0, e.mod));
        }
        a.out = a.combine ((double) (F (a.base) + F (e.out) * F (a.envdepth)), a.stat);
        (void) v;
    }

    void artsNoteOn (Voice& v, const NoteCtx& nt)
    {
        const auto& S = P->S;
        auto key = std::max (0.0, (double) (v.pitch * F (0.01) + 60.0f));
        if (v.pitchSub)
            key = std::max (0.0, key - 12.0);
        const auto fl = S.b4f7at (6);
        const auto rm = S.b4f7at (7) ? (double) v.relvel : 1.0;
        if (v.hasPortaKey)
        {
            key = std::max (0.0, (double) (v.portaKey * F (0.01) + 60.0f));
            if (v.pitchSub)
                key = std::max (0.0, key - 12.0);
        }
        const bool rndOn = ((S.mget (197) >> 8) & 0xff) != 0;
        for (int i = 0; i < kNumArts; ++i)
        {
            auto& a = v.arts[(std::size_t) i];
            bool hasRand = false;
            double rand = 0.0;
            if (rndOn && ! a.d->mapDefault[4])
            {
                randseed = randseed * 0x8088405u + 1u; // Delphi System.Random
                rand = randseed * 2.3283064365386963e-10;
                hasRand = true;
            }
            artNoteOn (v, a, key, (double) v.vel, v.unisonX, (double) v.envvar * 128.0, rm, fl, hasRand, rand);
            const auto& d = *a.d;
            if (v.hasPortaD && d.unit < 6 && d.index == 3 && a.env.c->on && ! a.env.c->pts.empty()
                && S.units[(std::size_t) d.unit].p[2] != 0 && ! (d.env.flags & 2))
                porta (v, d.unit, a, v.portaD);
        }
        (void) nt;
    }

    // GLOBAL envelopes (envelope flag bit 1, §4.7): a new voice copies the envelope state of the first still-held
    // voice and adopts its tick grid. EXEC (this port): exact for a held voice resting on its sustain point (demo patch
    // "Default", −110 dB with two overlapping notes); when the held voice is inside a segment FL hands the envelope
    // over differently (the new voice joins from a lower level and the old one stops at its next point) — not
    // decoded, so that case is approximate.
    void globalEnvelopes (Voice& v)
    {
        v.gridTcnt = 0;
        const Voice* held = nullptr;
        for (auto* a : act)
        {
            if (a->state < 5)
            {
                held = a;
                break;
            }
            for (int s = 0; s < a->nslaves && held == nullptr; ++s)
                if (a->slaves[s]->state < 5)
                    held = a->slaves[s];
            if (held != nullptr)
                break;
        }
        if (held == nullptr)
            return;
        bool copied = false;
        for (const auto i : P->activeArts)
        {
            auto& a = v.arts[(std::size_t) i];
            if (! a.env.c->on || (a.d->envFlags & 2) == 0)
                continue;
            a.env = held->arts[(std::size_t) i].env; // (the note-on value stays the voice's own: EXEC)
            copied = true;
        }
        if (copied)
            v.gridTcnt = std::max (0, held->tcnt);
    }

    float opFreq (const Voice& v, const OpV& o) const noexcept
    {
        const auto f = F (std::exp ((double) v.pitch * kLn2_1200) * o.k8d8 * (double) v.udet + (double) o.k8d0);
        return std::max (0.0f, f);
    }

    void pluckCoef (const Voice& v, OpV& o) noexcept
    {
        const auto a = o.damp * 0.99f + 0.01f;
        const auto b = 1.0f - a;
        o.pa = a, o.pb = b;
        if (! o.pbCacheSet || b != o.pbCache)
        {
            o.pbCacheSet = true;
            o.pbCache = b;
            const auto w = 1.0 / o.plen;
            const auto sn = std::sin (w), cs = std::cos (w);
            o.p12c = F (o.plen * (((double) b * sn) / (1.0 - (double) b * cs) + 1.0));
        }
        const auto fr = std::max ((double) o.fr, 0.0);
        const auto d = rnd ((double) (o.p12c * F (fr)) * 4294967296.0);
        o.p15c = d >> 32;
        o.p160 = (u32) (d & 0xffffffffLL);
        (void) v;
    }

    void pluckInit (const Voice& v, OpV& o, int u)
    {
        const auto L = rnd ((double) F (88200.0) / (double) (o.freq * (float) v.fs));
        o.plen = (int) std::max<long long> (2, std::min<long long> (L, 1 << 22));
        o.pbCacheSet = false;
        pluckCoef (v, o);
        const auto& full = fullTable (u);
        o.pbuf.assign ((std::size_t) o.plen, 0.0f);
        float keep = 0.0f;
        double nmix = 0.0;
        const auto seed = (u32) (std::uint64_t) rnd (o.phi * 4294967296.0);
        if (o.noiseAmt)
        {
            const auto x = (float) o.noiseAmt * F (1.0 / 128);
            const auto nn = x == 1.0f ? 1.0f : F ((std::exp ((double) (x * kLn11)) - 1.0) * (double) F (0.1));
            keep = 1.0f - nn;
            nmix = (double) nn * 1.5;
            ippRandUniform (o.pbuf.data(), o.plen, -1.0f, 1.0f, seed);
        }
        u32 uu = seed;
        const auto step = (u32) (std::uint64_t) rnd (4294967296.0 / (double) o.p12c);
        float accum = 0.0f;
        for (int i = 0; i < o.plen; ++i)
        {
            const auto t0 = full[(uu >> 18) & 0x3fff], t1 = full[(u32) (uu + 0x40000u) >> 18];
            auto val = F ((double) t0 + (double) ((t1 - t0) * (float) (uu & 0x3ffff)) * 3.814697265625e-06);
            if (o.noiseAmt)
                val = val * keep + o.pbuf[(std::size_t) i] * F (nmix);
            o.pbuf[(std::size_t) i] = val;
            accum = accum + val;
            uu = uu + step;
        }
        const auto mean = accum / (float) o.plen;
        for (auto& x : o.pbuf)
            x = x - mean;
        o.pptr = 0;
        o.p148 = o.pbuf.back();
        o.p154 = 0, o.p164 = 1, o.p168 = 0.0f, o.p16c = 0.0f;
    }

    // creates one voice (Voice.__init__); unison parameters u; returns false when the patch has nothing to play
    void setupVoice (Voice& v, const NoteCtx& nt, const Uni& u)
    {
        const auto& S = P->S;
        v.fs = fsv;
        v.vel = nt.vel, v.relvel = nt.relvel;
        v.fvol = nt.fvol, v.fpan = nt.fpan;
        v.pitch = F (100.0 * nt.key + nt.cents - 6000.0) + F (S.mget (2) * 100);
        v.notePitch = v.pitch, v.noteFinal = 100.0 * nt.key + nt.cents - 6000.0, v.slide = {};
        v.master = gCurve (S.mget (0), 128);
        v.tick = tick;
        v.base = F (523.251130601197 / fsv);
        v.rampMax = F (0.01 * 44100 / fsv);
        v.panMax = F (0.001 * 44100 / fsv);
        v.uvol = u.vol, v.upan = u.pan, v.uphase = u.phase, v.udet = u.det;
        v.pitchSub = u.sub;
        v.envvar = u.envvar;
        v.unisonX = u.x;
        v.fxpan = F (S.mget (180) / 256.0);
        v.nslaves = 0, v.slot6 = false, v.w34 = 1.0f;
        v.gpitch = ((S.mget (197) >> 16) & 0xff) != 0;
        v.hasPortaD = nt.porta;
        v.portaD = nt.portaD;
        v.hasPortaKey = nt.portaKey;
        v.portaKey = nt.portaKeyPitch;
        artsInit (v);
        artsNoteOn (v, nt);
        globalEnvelopes (v);
        // operators
        for (int k = 0; k < 6; ++k)
        {
            auto& o = v.ops[(std::size_t) k];
            releaseTable (o);
            auto pb = std::move (o.pbuf);
            o = OpV {};
            o.pbuf = std::move (pb);
            o.outGain = gCurve (S.cell (0, k, 8));
            o.fxGain = gCurve (S.cell (0, k, 7));
            o.panCell = F (S.cell (0, k, 6) / 512.0);
            o.active = o.outGain != 0 || o.fxGain != 0;
        }
        for (int j = 0; j < 6; ++j)
            for (int i = 0; i < 6; ++i)
            {
                auto& oj = v.ops[(std::size_t) j];
                if (const auto c = S.cell (0, i, j); c != 0)
                {
                    const auto g = gCurve (c);
                    const auto amt = i == j ? (g * 35.0f) / 128.0f : g * 4.0f;
                    oj.fmT[oj.nfm] = i, oj.fmAmt[oj.nfm] = amt, ++oj.nfm;
                    oj.active = true, oj.persample = true, v.ops[(std::size_t) i].persample = true;
                }
                if (const auto c = S.cell (1, i, j); c != 0)
                {
                    const auto a = gCurve (c);
                    oj.rmT[oj.nrm] = i, oj.rmA[oj.nrm] = a, oj.rmB[oj.nrm] = F (1.0 - std::abs ((double) a)), ++oj.nrm;
                    oj.active = true, oj.persample = true, v.ops[(std::size_t) i].persample = true;
                }
            }
        for (int k = 0; k < 6; ++k)
        {
            auto& o = v.ops[(std::size_t) k];
            const auto& U = S.units[(std::size_t) k];
            o.vol = (float) U.p[1] * F (1.0 / 256);
            o.prange = F ((double) (U.p[2] * 2) * kLn2_1200);
            const auto noiseV = opInt (U, 8);
            const auto* fl = U.op.data() + 0x820;
            if (noiseV && ! fl[6])
            {
                const auto x = (float) noiseV * F (1.0 / 128);
                auto nz = x == 1.0f ? 1.0f : F ((std::exp ((double) (x * kLn11)) - 1.0) * (double) F (0.1));
                o.n10c = 1.0f - nz;
                nz = F ((double) nz * std::sqrt (fsv / 44100.0));
                o.n110 = nz;
                o.n108 = F ((double) nz * 4.656612873077393e-10);
                o.noiseOn = true, o.persample = true;
            }
            const auto ratio = opInt (U, 0), off = opInt (U, 4);
            o.k8d8 = (double) (((float) ratio * v.base) / 20000.0f);
            o.k8d0 = off ? (float) off / F (fsv * 100) : 0.0f;
            o.startph = (float) U.p[0] * F (1.0 / 256);
            o.bandlimit = ! fl[6] && fl[7];
            o.pluck = fl[6] != 0;
            o.noiseAmt = noiseV;
            o.dc = fl[4] != 0;
            o.globalPhase = fl[3] != 0;
            o.dck1 = (double) F ((44100.0 / fsv) * 2.2835e-05);
            o.dck2 = (double) F (0.96);
            if (o.pluck)
                o.persample = true;
        }
        setupFilters (v);
        const auto p6330 = rnd ((double) F (std::exp ((double) ((float) S.p4f0 * F (1.0 / 128)) * std::log (101.0)) - 1.0) * 10.0);
        const auto p62d8 = F (96.0 / ((44100.0 / fsv) * (44100.0 / fsv)));
        for (int k = 0; k < 6; ++k)
        {
            auto& o = v.ops[(std::size_t) k];
            for (int j = 0; j < 6; ++j)
                o.art[j] = v.out (k, j);
            o.mod = F (std::min (1.0, std::max (0.0, (double) o.art[2])));
            o.rmacc = o.art[2];
            o.fmacc = 0.0f;
            o.ptf = F (std::exp ((double) (o.art[3] * o.prange)));
            o.ptfArt = o.art[3];
            o.phArt = o.art[4];
            o.fmInc = 0.0f;
            o.curVol = o.art[1] * o.vol, o.volInc = 0.0f, o.modInc = 0.0f;
            o.freq = opFreq (v, o);
            o.fr = o.freq * o.ptf;
            if (v.gpitch && k > 0)
                o.fr = o.fr * v.ops[0].ptf;
            auto ph = (double) ((o.startph + v.uphase) + o.phArt);
            if (o.globalPhase)
                ph = ph + (double) ((float) counter * o.freq);
            ph = ph + (double) (64 - rnd (ph));
            o.phi = ph;
            o.table = nullptr;
            o.attached = false;
            if (o.pluck)
            {
                o.damp = F (std::min (1.0, std::max (0.0, (double) o.art[5])));
                pluckInit (v, o, k);
            }
            auto b0 = 0.0f;
            if (p6330 >= 1)
            {
                b0 = F ((0.775 - (double) v.vel) * (double) p6330);
                b0 = p62d8 * b0;
                if (b0 <= 0)
                    b0 = 0.0f;
            }
            if (S.units[(std::size_t) k].op[0x825])
                b0 = b0 + p62d8;
            o.dkN = (int) rnd ((double) b0);
            if (o.dkN < 1)
                o.dkN = -1;
            else
                o.dkC = 0.0f, o.dkI = F (1.0 / o.dkN), o.dkY = 0.0f;
        }
        v.noiseOps = 0;
        for (const auto& o : v.ops)
            v.noiseOps += o.active && o.noiseOn ? 1 : 0;
        v.lcgOwner = nullptr;
        v.tcnt = -tick;
        v.state = 1;
    }

    void setupFilters (Voice& v)
    {
        const auto& S = P->S;
        for (int k = 0; k < 3; ++k)
        {
            const auto u = 6 + k;
            auto& f = v.filts[(std::size_t) k];
            f = FiltV {};
            for (int j = 0; j < 6; ++j)
                if (const auto c = S.cell (0, u, j); c != 0)
                    f.inSrc[f.nin] = j, f.inG[f.nin] = gCurve (c), ++f.nin;
            if (k > 0 && S.units[(std::size_t) (u - 1)].p[9] != 0 && v.filts[(std::size_t) (k - 1)].active)
                f.inSrc[f.nin] = u - 1, f.inG[f.nin] = gCurve (S.units[(std::size_t) (u - 1)].p[9]), ++f.nin;
            f.active = f.nin > 0;
            for (int i = 0; i < f.nin; ++i)
                if (f.inSrc[i] < 6)
                    v.ops[(std::size_t) f.inSrc[i]].active = true;
            f.outGain = gCurve (S.cell (0, u, 8));
            f.fxGain = gCurve (S.cell (0, u, 7));
            f.panCell = F (S.cell (0, u, 6) / 512.0);
            f.vprev = v.out (u, 1); // note-on value of the volume articulator (§5.5)
            f.wsPrevSet = false;
            f.st.reset();
        }
    }

    // ---------------------------------------------------------------- per tick (Voice._tick)
    float rampInc (const Voice& v, float tgt, float cur, int n) const noexcept
    {
        auto d = tgt - cur;
        if (d == 0.0f)
            return 0.0f;
        d = d / (float) n;
        if (std::abs (d) > v.rampMax)
            d = std::copysign (v.rampMax, d);
        return d;
    }

    template <typename U>
    void panGains (const Voice& v, U& o, float artpan, bool first) const noexcept
    {
        auto p = ((v.fpan * 0.5f + v.upan) + o.panCell) + artpan;
        const auto pp = std::min (1.0, std::max (-1.0, (double) (p * 2.0f - 1.0f)));
        const auto gain = ((v.fvol * v.master) * v.uvol) * o.outGain;
        const auto ang = (1.0 + pp) * 0.7853981633974483;
        if (ang != o.angO) // (cos/sin cached per unit: the pan rarely moves)
            o.angO = ang, o.cosO = std::cos (ang), o.sinO = std::sin (ang);
        if (v.state < 9)
            o.L = F (o.cosO * (double) gain), o.R = F (o.sinO * (double) gain);
        else
            o.L = o.R = 0.0f;
        if (first)
            o.pL = o.L, o.pR = o.R;
        if (o.fxGain == 0.0f)
            return;
        const auto a90 = v.out (9, 0), a91 = v.out (9, 1);
        p = ((((v.fpan * 0.5f + v.upan) + o.panCell) + v.fxpan) + artpan) + a90;
        const auto fp = std::min (1.0, std::max (-1.0, (double) (p * 2.0f - 1.0f)));
        const auto fg = (((v.fvol * v.master) * v.uvol) * o.fxGain) * a91;
        const auto fa = (1.0 + fp) * 0.7853981633974483;
        if (fa != o.angF)
            o.angF = fa, o.cosF = std::cos (fa), o.sinF = std::sin (fa);
        if (v.state < 9)
            o.FL = F (o.cosF * (double) fg), o.FR = F (o.sinF * (double) fg);
        else
            o.FL = o.FR = 0.0f;
        if (first)
            o.pFL = o.FL, o.pFR = o.FR;
    }

    void tickVoice (Voice& v, bool first)
    {
        for (const auto i : P->activeArts)
            v.arts[(std::size_t) i].tick();
        if (v.state > 4)
        {
            int ms = 9;
            for (const auto i : P->activeArts)
                if (! v.arts[(std::size_t) i].finished())
                {
                    ms = 5;
                    break;
                }
            v.state = std::min (9, ms);
        }
        for (int k = 0; k < 3; ++k)
        {
            auto& f = v.filts[(std::size_t) k];
            if (f.active)
                panGains (v, f, v.out (6 + k, 0), first);
        }
        for (int u = 0; u < 6; ++u)
        {
            auto& o = v.ops[(std::size_t) u];
            if (! o.active)
            {
                if (u == 0)
                    o.ptf = F (std::exp ((double) (v.out (0, 3) * o.prange)));
                continue; // never rendered (its state is not read)
            }
            float a[6];
            for (int k = 0; k < 6; ++k)
                a[k] = v.out (u, k);
            for (int k = 0; k < 6; ++k)
                o.art[k] = a[k];
            const auto tgt = F (std::min (1.0, std::max (0.0, (double) a[2])));
            o.modInc = rampInc (v, tgt, o.mod, v.tick);
            if (a[3] != o.ptfArt)
                o.ptf = F (std::exp ((double) (a[3] * o.prange))), o.ptfArt = a[3];
            o.fmInc = (a[4] - o.phArt) / (float) v.tick;
            o.phArt = a[4];
            if (o.pluck && v.artActive (u, 5))
            {
                o.damp = F (std::min (1.0, std::max (0.0, (double) a[5])));
                pluckCoef (v, o);
            }
            auto vt = a[1] * o.vol;
            if (std::abs (vt) < kTiny)
                vt = 0.0f;
            if (std::abs (o.curVol) < kTiny)
                o.curVol = 0.0f;
            o.volInc = rampInc (v, vt, o.curVol, v.tick);
            panGains (v, o, a[0], first);
        }
    }

    // ---------------------------------------------------------------- operators (_render_ops)
    void attach (Voice& v, int u)
    {
        auto& o = v.ops[(std::size_t) u];
        if (o.attached)
            return;
        o.attached = true;
        const float* t = nullptr;
        int N = kN, slot = -1;
        if (o.bandlimit)
            t = bandLimited (u, o.fr, v.fs, N, slot);
        if (t == nullptr)
        {
            t = fullTable (u).data();
            N = kN;
            slot = -1;
        }
        o.table = t;
        o.N = N;
        o.cacheSlot = slot;
        o.phi *= N;
        for (auto& p : v.ops)
            for (int i = 0; i < p.nfm; ++i)
                if (p.fmT[i] == u)
                    p.fmAmt[i] = p.fmAmt[i] * (float) N;
    }

    void renderOps (Voice& v, int m)
    {
        int ps[6];
        const auto nps = prepOps (v, ps);
        if (nps > 0)
        {
            Voice* vs[1] { &v };
            const int* pss[1] { ps };
            std::array<std::vector<float>, 6>* bufs[1] { ob };
            u32* lcgs[1] { v.lcgOwner != nullptr ? &v.lcgOwner->lcg : &v.lcg };
            perSample (vs, 1, pss, nps, m, bufs, lcgs);
        }
        finishOps (v, m);
    }

    // per sub-block operator set-up: frequency, table, increment, phase wrap; returns the per-sample list
    int prepOps (Voice& v, int* ps)
    {
        int nps = 0;
        for (int u = 0; u < 6; ++u)
        {
            auto& o = v.ops[(std::size_t) u];
            if (! o.active)
                continue;
            auto fr = o.freq * o.ptf;
            if (v.gpitch && u > 0)
                fr = fr * v.ops[0].ptf; // GLOBAL PITCH (§6.4)
            if (o.pluck && fr != o.fr)
            {
                o.fr = fr;
                pluckCoef (v, o);
            }
            o.fr = fr;
            attach (v, u);
            const auto N = o.N;
            auto inc = (fr + o.fmInc) * (float) N;
            if (inc >= (float) (N >> 1))
                inc = (float) (N >> 1);
            o.inc = inc;
            const auto invN = (double) F (1.0 / N);
            if (o.persample)
            {
                o.phi = o.phi + (double) (64 - rnd (o.phi * invN)) * N;
                ps[nps++] = u;
            }
            else
                o.phi = o.phi + (double) (1 - rnd (o.phi * invN)) * N;
        }
        return nps;
    }

    void finishOps (Voice& v, int m)
    {
        for (int u = 0; u < 6; ++u)
        {
            auto& o = v.ops[(std::size_t) u];
            if (o.active && ! o.persample)
                blockOp (o, (*ob)[(std::size_t) u].data(), m);
        }
    }

    // 4-lane block reader (FUN_00870760 + FUN_00860f10): lane c starts at φ + c/4·4·inc and adds 4·inc per group of 4
    // samples (float); index = truncation, fraction = x − trunc(x); then × the volume ramp (incremented before use)
    void blockOp (OpV& o, float* buf, int m) noexcept
    {
        if (o.curVol == 0.0f && o.volInc == 0.0f)
        {
            std::fill (buf, buf + m, 0.0f);
            o.phi = m * (double) o.inc + o.phi;
            return;
        }
        const auto fv = o.inc * 4.0f;
        const auto ph0 = (float) o.phi;
        float lanes[4] { ph0 + 0.0f * fv, ph0 + 0.25f * fv, ph0 + 0.5f * fv, ph0 + 0.75f * fv };
        float* vols = gains.data();
        auto vol = o.curVol;
        const auto vi = o.volInc;
        if (vi == 0.0f)
            std::fill (vols, vols + m, vol);
        else
            for (int k = 0; k < m; ++k)
            {
                vol = vol + vi;
                vols[k] = vol;
            }
        const auto* t = o.table;
        const auto mask = o.N - 1;
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE2, or ARM64 through core/Simd.h (sse2neon)
        // 4 lanes in one register: truncation, fraction, two table reads per lane, interpolation and volume — the
        // same float operations in the same order as the scalar form
        auto x4 = _mm_loadu_ps (lanes);
        const auto f4 = _mm_set1_ps (fv);
        const auto m4 = _mm_set1_epi32 (mask);
        alignas (16) std::int32_t idx[4];
        alignas (16) float r[4];
        for (int j = 0; j < m; j += 4)
        {
            const auto ii = _mm_cvttps_epi32 (x4); // positive and < 2^31 (φ ≤ 1.5·N + m·N/2)
            const auto fr = _mm_sub_ps (x4, _mm_cvtepi32_ps (ii));
            _mm_store_si128 ((__m128i*) idx, _mm_and_si128 (ii, m4));
            const auto t0 = _mm_setr_ps (t[idx[0]], t[idx[1]], t[idx[2]], t[idx[3]]);
            const auto t1 = _mm_setr_ps (t[idx[0] + 1], t[idx[1] + 1], t[idx[2] + 1], t[idx[3] + 1]);
            const auto y = _mm_add_ps (t0, _mm_mul_ps (_mm_sub_ps (t1, t0), fr));
            if (j + 4 <= m)
                _mm_storeu_ps (buf + j, _mm_mul_ps (y, _mm_loadu_ps (vols + j)));
            else
            {
                _mm_store_ps (r, y);
                for (int c = 0; j + c < m; ++c)
                    buf[j + c] = r[c] * vols[j + c];
            }
            x4 = _mm_add_ps (x4, f4);
        }
#else
        float* xs = xin.data();
        const auto groups = (m + 3) >> 2;
        for (int g = 0; g < groups; ++g)
            for (int c = 0; c < 4; ++c)
            {
                xs[4 * g + c] = lanes[c];
                lanes[c] = lanes[c] + fv;
            }
        for (int k = 0; k < m; ++k)
        {
            const auto x = xs[k];
            const auto ii = (int) x;
            const auto fr = x - (float) ii;
            const auto i = ii & mask;
            const auto t0 = t[i], t1 = t[i + 1];
            buf[k] = (t0 + (t1 - t0) * fr) * vols[k];
        }
#endif
        o.curVol = vol;
        o.phi = m * (double) o.inc + o.phi;
    }

    // nv voices of the same patch (same per-sample list) in lockstep: their dependency chains interleave
    void perSample (Voice* const* vs, int nv, const int* const* pss, int nps, int m, std::array<std::vector<float>, 6>* const* bufs,
                    u32* const* lcgs)
    {
        struct K
        {
            const float* t;
            float* out;
            OpV* op;
            int u, mask, nrm, nfm;
            bool pluck, noise;
            double n10c, n108, n110, modInc, volInc;
            int rmT[6], fmT[6];
            double rmA[6], rmB[6], fmAmt[6];
        };
        struct S
        {
            double phi[6], inc[6];
            float fmacc[6], rmacc[6], mod[6], cur[6];
            u32 lcg;
            K k[6];
        } st[kBatch];
        for (int j = 0; j < nv; ++j)
        {
            auto& ops = vs[j]->ops;
            auto& s = st[j];
            for (int u = 0; u < 6; ++u)
            {
                const auto& o = ops[(std::size_t) u];
                s.phi[u] = o.phi, s.inc[u] = (double) o.inc;
                s.fmacc[u] = o.fmacc, s.rmacc[u] = o.rmacc, s.mod[u] = o.mod, s.cur[u] = o.curVol;
            }
            s.lcg = *lcgs[j];
            for (int q = 0; q < nps; ++q)
            {
                const auto u = pss[j][q];
                auto& o = ops[(std::size_t) u];
                auto& c = s.k[q];
                c.t = o.table, c.out = (*bufs[j])[(std::size_t) u].data(), c.op = &o, c.u = u, c.mask = o.N - 1;
                c.nrm = o.nrm, c.nfm = o.nfm, c.pluck = o.pluck, c.noise = o.noiseOn;
                c.n10c = o.n10c, c.n108 = o.n108, c.n110 = o.n110, c.modInc = o.modInc, c.volInc = o.volInc;
                for (int r = 0; r < o.nrm; ++r)
                    c.rmT[r] = o.rmT[r], c.rmA[r] = o.rmA[r], c.rmB[r] = o.rmB[r];
                for (int f = 0; f < o.nfm; ++f)
                    c.fmT[f] = o.fmT[f], c.fmAmt[f] = o.fmAmt[f];
            }
        }
        for (int k = 0; k < m; ++k)
            for (int j = 0; j < nv; ++j)
            {
                auto& s = st[j];
                for (int q = 0; q < nps; ++q)
                {
                    const auto& c = s.k[q];
                    const auto u = c.u;
                    double w;
                    if (c.pluck)
                        w = pluckSampleD (*c.op);
                    else
                    {
                        const auto fmod = (double) s.fmacc[u] * (double) s.mod[u];
                        const auto ph = s.phi[u];
                        s.phi[u] = s.inc[u] + ph;
                        const auto x = ph + fmod;
                        s.fmacc[u] = 0.0f;
                        const auto fl = rne (x - 0.5);
                        const auto i = (int) ((long long) fl & c.mask);
                        const auto t0 = (double) c.t[i];
                        w = t0 + ((double) c.t[i + 1] - t0) * (x - fl);
                    }
                    if (c.noise)
                    {
                        s.lcg = s.lcg * 0x8088405u + 1u;
                        w = w * c.n10c + ((double) s.lcg * c.n108 - c.n110);
                    }
                    const auto prev = (double) s.rmacc[u];
                    const auto md = s.mod[u];
                    s.rmacc[u] = md;
                    s.mod[u] = F ((double) md + c.modInc);
                    w = ((1.0 + prev) - (double) md) * w;
                    for (int r = 0; r < c.nrm; ++r)
                    {
                        const auto t = c.rmT[r];
                        s.rmacc[t] = F ((c.rmA[r] * w + c.rmB[r]) * (double) s.rmacc[t]);
                    }
                    const auto vv = (double) s.cur[u] + c.volInc;
                    w = w * vv;
                    s.cur[u] = (float) vv;
                    c.out[k] = (float) w;
                    for (int f = 0; f < c.nfm; ++f)
                    {
                        const auto t = c.fmT[f];
                        s.fmacc[t] = F (c.fmAmt[f] * w + (double) s.fmacc[t]);
                    }
                }
            }
        for (int j = 0; j < nv; ++j)
        {
            auto& ops = vs[j]->ops;
            const auto& s = st[j];
            *lcgs[j] = s.lcg;
            for (int u = 0; u < 6; ++u)
            {
                auto& o = ops[(std::size_t) u];
                o.phi = s.phi[u];
                o.fmacc = s.fmacc[u], o.rmacc = s.rmacc[u], o.mod = s.mod[u], o.curVol = s.cur[u];
            }
        }
    }

    double pluckSampleD (OpV& o) noexcept
    {
        double st0 = o.p168;
        const auto s = (std::uint64_t) o.p154 + o.p160;
        const auto carry = (long long) (s >> 32);
        o.p154 = (u32) s;
        const auto cnt = o.p164 + o.p15c + carry;
        if (cnt)
        {
            auto* buf = o.pbuf.data();
            const auto a = (double) o.pa, b = (double) o.pb;
            for (long long c = 0; c < cnt; ++c)
            {
                o.p16c = (float) st0;
                const auto val = (double) buf[o.pptr];
                const auto y = val * a + (double) o.p148 * b;
                buf[o.pptr] = (float) y;
                o.p148 = (float) y;
                if (++o.pptr >= o.plen)
                    o.pptr = 0;
                st0 = val;
            }
            o.p168 = (float) st0;
            o.p164 = 0;
        }
        return (double) o.p16c + (st0 - (double) o.p16c) * ((double) o.p154 * 2.3283064365386963e-10);
    }

    // ---------------------------------------------------------------- CENTER / DECLICK, filters, output (_post_ops)
    void postOps (Voice& v, int m, int pos)
    {
        for (int u = 0; u < 6; ++u)
        {
            auto& o = v.ops[(std::size_t) u];
            if (! o.active)
                continue;
            auto* x = (*ob)[(std::size_t) u].data();
            if (o.dc)
            {
                auto s_ = o.dcs, v_ = o.dcv;
                for (int k = 0; k < m; ++k)
                {
                    const auto xi = (double) x[k];
                    v_ = v_ + (xi - s_) * o.dck1;
                    s_ = s_ + v_;
                    v_ = v_ * o.dck2;
                    x[k] = (float) (xi - s_);
                }
                o.dcs = s_, o.dcv = v_;
            }
            if (o.dkN > 0)
            {
                o.dkN -= m;
                const auto cnt = o.dkN >= 0 ? m : m + o.dkN;
                auto y = o.dkY, c = o.dkC;
                for (int k = 0; k < cnt; ++k)
                {
                    y = y + c * (x[k] - y);
                    x[k] = y;
                    c = c + o.dkI;
                }
                o.dkY = y;
                o.dkC = o.dkC + o.dkI * (float) cnt;
            }
        }
        filtersSub (v, m);
        for (int u = 0; u < 6; ++u)
        {
            auto& o = v.ops[(std::size_t) u];
            if (o.active && o.outGain != 0.0f)
                mix (v, (*ob)[(std::size_t) u].data(), Ml.data() + pos, Mr.data() + pos, m, o.pL, o.L, o.pR, o.R);
        }
        for (int k = 0; k < 3; ++k)
        {
            auto& f = v.filts[(std::size_t) k];
            if (f.active && f.outGain != 0.0f)
                mix (v, fBuf[(std::size_t) k].data(), Ml.data() + pos, Mr.data() + pos, m, f.pL, f.L, f.pR, f.R);
        }
        for (int u = 0; u < 6; ++u)
        {
            auto& o = v.ops[(std::size_t) u];
            if (o.active && o.fxGain != 0.0f)
                mix (v, (*ob)[(std::size_t) u].data(), Fl.data() + pos, Fr.data() + pos, m, o.pFL, o.FL, o.pFR, o.FR);
        }
        for (int k = 0; k < 3; ++k)
        {
            auto& f = v.filts[(std::size_t) k];
            if (f.active && f.fxGain != 0.0f)
                mix (v, fBuf[(std::size_t) k].data(), Fl.data() + pos, Fr.data() + pos, m, f.pFL, f.FL, f.pFR, f.FR);
        }
    }

    // gains ramped over the sub-block (FUN_0062bb20, §6.1) and added to the bus
    static void mix (const Voice& v, const float* buf, float* outL, float* outR, int m, float& pL, float L, float& pR, float R) noexcept
    {
        const auto mx = v.panMax * v.uvol;
        float gL, incL, gR, incR, nl, nr;
        rampOut (pL, L, m, mx, gL, incL, nl);
        rampOut (pR, R, m, mx, gR, incR, nr);
        pL = nl, pR = nr;
        if (incL == 0.0f && incR == 0.0f)
        {
            if (gL != 0.0f || gR != 0.0f)
                for (int k = 0; k < m; ++k)
                {
                    outL[k] = outL[k] + buf[k] * gL;
                    outR[k] = outR[k] + buf[k] * gR;
                }
            return;
        }
        for (int k = 0; k < m; ++k)
        {
            outL[k] = buf[k] * gL + outL[k];
            outR[k] = buf[k] * gR + outR[k];
            gR = gR + incR;
            gL = gL + incL;
        }
    }

    void filtersSub (Voice& v, int m)
    {
        for (int k = 0; k < 3; ++k)
        {
            auto& f = v.filts[(std::size_t) k];
            if (! f.active)
                continue;
            auto* x = xin.data();
            for (int i = 0; i < f.nin; ++i)
            {
                const auto j = f.inSrc[i];
                const float* src = j < 6 ? (*ob)[(std::size_t) j].data() : fBuf[(std::size_t) (j - 6)].data();
                const auto g = f.inG[i];
                if (i == 0)
                    for (int s = 0; s < m; ++s)
                        x[s] = src[s] * g;
                else
                    for (int s = 0; s < m; ++s)
                        x[s] = x[s] + src[s] * g;
            }
            // volume articulator ramped over the sub-block (lane ramp)
            const auto u = 6 + k;
            float start, inc, np;
            rampOut (f.vprev, v.out (u, 1), m, v.rampMax, start, inc, np);
            f.vprev = np;
            if (inc == 0.0f)
            {
                if (start != 1.0f)
                    for (int s = 0; s < m; ++s)
                        x[s] = x[s] * start;
            }
            else
            {
                laneRamp (start, inc, m, gains.data());
                for (int s = 0; s < m; ++s)
                    x[s] = x[s] * gains[(std::size_t) s];
            }
            const double ga[3] { (double) v.out (u, 4), (double) v.out (u, 5), (double) v.out (u, 6) };
            auto* y = fBuf[(std::size_t) k].data();
            filterSeg (fdef[(std::size_t) k], f.st, fw, x, m, (double) v.out (u, 2), (double) v.out (u, 3), ga, v.rampMax, y);
            if (P->wsOn[(std::size_t) k])
            {
                const auto ws_mix = (float) P->S.units[(std::size_t) u].p[8] * F (1.0 / 128);
                const auto ws_pre = (float) P->S.units[(std::size_t) u].p[7] * F (1.0 / 128);
                const auto tgt = F (std::min (1.0, std::max (0.0, (double) (v.out (u, 7) * ws_mix))));
                if (! f.wsPrevSet)
                    f.wsPrevSet = true, f.wsPrev = tgt;
                float cur, wi, nw;
                rampOut (f.wsPrev, tgt, m, v.rampMax, cur, wi, nw);
                f.wsPrev = nw;
                const bool interp = renderMode || P->S.b5f18 != 0;
                waveshape (y, m, P->shaper[(std::size_t) k], P->shaperN[(std::size_t) k], P->wsUni[(std::size_t) k], ws_pre, interp, sh.data());
                if (wi == 0.0f)
                {
                    if (cur == 1.0f)
                        std::memcpy (y, sh.data(), sizeof (float) * (std::size_t) m);
                    else if (cur != 0.0f)
                        for (int s = 0; s < m; ++s)
                            y[s] = y[s] * (1.0f - cur) + sh[(std::size_t) s] * cur;
                }
                else
                {
                    laneRamp (1.0f - cur, -wi, m, gains.data());
                    auto g = cur;
                    for (int s = 0; s < m; ++s)
                    {
                        const auto yy = y[s] * gains[(std::size_t) s];
                        y[s] = sh[(std::size_t) s] * g + yy;
                        g = g + wi;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- one voice over n voice-rate samples (Voice.render)
    void renderVoice (Voice& v, int n)
    {
        if (v.state >= 10)
            return;
        int q = 0;
        while (q < n)
        {
            if (v.tcnt <= 0)
            {
                const auto first = v.tcnt < 0;
                v.tcnt = v.tick;
                if (first && v.gridTcnt > 0)
                    v.tcnt = v.gridTcnt; // the held voice's tick grid (GLOBAL envelope)
                tickVoice (v, first);
                for (int s = 0; s < v.nslaves; ++s)
                    tickVoice (*v.slaves[s], first);
            }
            const auto m = std::min (v.tcnt, n - q);
            renderSub (v, m, q);
            q += m;
            v.tcnt -= m;
        }
        endCheck (v);
    }

    void endCheck (Voice& v) noexcept
    {
        if (v.state < 9)
            return;
        bool silent = true;
        for (const auto& o : v.ops)
            if (o.active && ((o.outGain != 0 && (o.pL != 0 || o.pR != 0)) || (o.fxGain != 0 && (o.pFL != 0 || o.pFR != 0))))
                silent = false;
        for (const auto& f : v.filts)
            if (f.active && ((f.outGain != 0 && (f.pL != 0 || f.pR != 0)) || (f.fxGain != 0 && (f.pFL != 0 || f.pFR != 0))))
                silent = false;
        if (silent)
            v.state = 10;
    }

    // voices on the same tick grid rendered sub-block by sub-block together (same results as one after the other:
    // per sample, each voice's contributions are still added to the buses in render order)
    void renderBatch (Voice* const* vs, int nb, int n)
    {
        int q = 0;
        while (q < n)
        {
            if (vs[0]->tcnt <= 0)
                for (int j = 0; j < nb; ++j)
                {
                    auto& v = *vs[j];
                    const auto first = v.tcnt < 0;
                    v.tcnt = v.tick;
                    tickVoice (v, first);
                }
            const auto m = std::min (vs[0]->tcnt, n - q);
            int ps[kBatch][6];
            const int* pss[kBatch];
            std::array<std::vector<float>, 6>* bufs[kBatch];
            u32* lcgs[kBatch];
            int nps = 0;
            for (int j = 0; j < nb; ++j)
            {
                ob = &opBufs[(std::size_t) j];
                nps = prepOps (*vs[j], ps[j]);
                pss[j] = ps[j], bufs[j] = ob, lcgs[j] = &vs[j]->lcg;
            }
            if (nps > 0)
                perSample (vs, nb, pss, nps, m, bufs, lcgs);
            for (int j = 0; j < nb; ++j)
            {
                ob = &opBufs[(std::size_t) j];
                finishOps (*vs[j], m);
                postOps (*vs[j], m, q);
            }
            ob = &opBufs[0];
            q += m;
            for (int j = 0; j < nb; ++j)
                vs[j]->tcnt -= m;
        }
        for (int j = 0; j < nb; ++j)
            endCheck (*vs[j]);
    }

    void renderSub (Voice& v, int m, int pos)
    {
        if (v.nslaves > 0)
        {
            // OSC grouping (§6.3): unison voices' operator outputs summed into the master's buffers
            for (int s = 0; s < v.nslaves; ++s)
            {
                auto& sv = *v.slaves[s];
                renderOps (sv, m);
                for (int u = 0; u < 6; ++u)
                {
                    if (! sv.ops[(std::size_t) u].active)
                        continue;
                    auto* a = acc[(std::size_t) u].data();
                    const auto* b = (*ob)[(std::size_t) u].data();
                    if (sv.slot6)
                        for (int k = 0; k < m; ++k)
                            a[k] = a[k] + b[k] * sv.w34;
                    else
                        for (int k = 0; k < m; ++k)
                            a[k] = b[k] * sv.w34;
                }
            }
            renderOps (v, m);
            for (int u = 0; u < 6; ++u)
            {
                if (! v.ops[(std::size_t) u].active)
                    continue;
                auto* a = acc[(std::size_t) u].data();
                auto* b = (*ob)[(std::size_t) u].data();
                for (int k = 0; k < m; ++k)
                    b[k] = a[k] + b[k] * v.w34;
            }
        }
        else
            renderOps (v, m);
        postOps (v, m, pos);
    }

    // ---------------------------------------------------------------- voice management
    Voice* freeVoice() noexcept
    {
        for (auto& v : pool)
            if (! v.used)
                return &v;
        return nullptr;
    }

    void freeVoiceObj (Voice& v) noexcept
    {
        for (auto& o : v.ops)
            releaseTable (o);
        for (int s = 0; s < v.nslaves; ++s)
            freeVoiceObj (*v.slaves[s]);
        v.nslaves = 0;
        v.used = false;
        v.state = 0;
    }

    // FL's kill (FUN_0086d770, MONO KEY and the polyphony limit): state 9 on the voice, all its articulators (so a later
    // tick cannot bring the voice back to "released") and its unison chain
    void killVoice (Voice& v) noexcept
    {
        v.state = 9;
        for (auto& a : v.arts)
            a.env.stage = 9, a.lfo.env.stage = 9;
        // the output gain targets drop to 0 at once: the ramp-out starts with the next sub-block, not at the next tick
        // (EXEC, this port: MONO KEY re-trigger of a releasing note, demo channel 141: −51 → −117 dB)
        for (auto& o : v.ops)
            o.L = o.R = o.FL = o.FR = 0.0f;
        for (auto& f : v.filts)
            f.L = f.R = f.FL = f.FR = 0.0f;
        for (int s = 0; s < v.nslaves; ++s)
            killVoice (*v.slaves[s]);
    }

    int countFree() const noexcept
    {
        int n = 0;
        for (const auto& v : pool)
            n += v.used ? 0 : 1;
        return n;
    }

    // hard-frees the oldest group (pool exhausted)
    void stealOldest() noexcept
    {
        long long g = -1;
        for (auto* v : act)
            if (g < 0 || v->group < g)
                g = v->group;
        if (g < 0)
            return;
        for (auto it = act.begin(); it != act.end();)
            if ((*it)->group == g)
            {
                freeVoiceObj (**it);
                it = act.erase (it);
            }
            else
                ++it;
    }

    void noteOn (const NoteParams& note, int maxPoly)
    {
        if (P == nullptr)
            return;
        const auto& S = P->S;
        // MONO KEY (P+0x443): kills the same key's voices that are not finishing
        if (((S.mget (197) >> 24) & 0xff) != 0)
            for (auto* v : act)
                if (v->key == (int) std::lround (note.key) && v->state < 9)
                    killVoice (*v);
        // polyphony (§6.4): oldest group (released ones count as oldest) killed when the limit is reached
        {
            long long groups[64];
            long long ages[64];
            int ng = 0;
            for (auto* v : act)
            {
                if (v->state >= 9)
                    continue;
                bool seen = false;
                for (int i = 0; i < ng; ++i)
                    if (groups[i] == v->group)
                        seen = true, ages[i] = std::max (ages[i], v->age);
                if (! seen && ng < 64)
                    groups[ng] = v->group, ages[ng] = v->age, ++ng;
            }
            if (ng >= maxPoly && ng > 0)
            {
                int pick = 0;
                for (int i = 1; i < ng; ++i)
                    if (ages[i] > ages[pick])
                        pick = i;
                for (auto* v : act)
                    if (v->group == groups[pick])
                        killVoice (*v);
            }
        }
        Uni ups[18];
        const auto nu = unisonParams (S, ups);
        while (countFree() < nu && ! act.empty())
            stealOldest();
        if (countFree() < nu)
            return;
        const bool grouped = S.b4f4 != 0 && (S.mget (173) >= 2 || S.mget (177) > 0);
        const auto curPitch = F (100.0 * note.key + note.cents - 6000.0) + F (S.mget (2) * 100);
        const auto pflag = S.b4f7at (1);
        const bool porta = hasLastPitch && ((pflag != 0) != (note.colour == 14));
        NoteCtx nt { note.key, note.cents, note.velocity, note.releaseVelocity, 1.0f, note.pan,
                     porta, porta && S.b4f7at (3) != 0, porta ? lastPitch - curPitch : 0.0f, lastPitch };
        const auto group = ++groupSerial;
        // ages: every live group gets older by one note
        for (auto* v : act)
            if (v->age < 0x4FFFFFFF)
                ++v->age;
        Voice* created[18];
        for (int j = 0; j < nu; ++j)
        {
            auto u = ups[j];
            float w34 = 1.0f;
            if (grouped)
                w34 = u.vol, u.pan = 0.5f;
            auto* v = freeVoice();
            v->used = true;
            v->group = group;
            v->id = note.id;
            v->age = 0;
            v->key = (int) std::lround (note.key);
            v->isSlave = false;
            v->notePan = note.pan;
            // FL velocity → Init.Vol = (11^(vel/100) − 1)/10 (REA_rodada28 §4.1, inference), used only with link velocity
            v->initVol = (float) ((std::pow (11.0, 1.28 * std::clamp ((double) note.velocity, 0.0, 1.0)) - 1.0) / 10.0);
            setupVoice (*v, nt, u);
            v->w34 = w34;
            created[j] = v;
        }
        lastPitch = curPitch;
        hasLastPitch = true;
        if (grouped)
        {
            auto* master = created[0];
            master->w34 = master->uvol;
            master->uvol = 1.0f;
            for (int j = 1; j < nu; ++j)
            {
                auto* sv = created[j];
                sv->w34 = sv->uvol;
                sv->slot6 = j > 1;
                sv->isSlave = true;
                sv->lcgOwner = master;
                master->slaves[master->nslaves++] = sv;
            }
            act.push_back (master);
        }
        else
            for (int j = 0; j < nu; ++j)
                act.push_back (created[j]);
    }

    // a slide note (issue #14): the voice's pitch (and its operators' frequencies) moved over `n` samples
    void slideVoice (Voice& v, int n) noexcept
    {
        if (! v.slide.moving())
            return;
        v.pitch = v.notePitch + v.slide.advance ((float) n);
        for (auto& o : v.ops)
            if (o.active)
                o.freq = opFreq (v, o);
    }

    void releaseVoice (Voice& v) noexcept
    {
        if (v.state < 5)
            v.state = 5;
        for (auto& a : v.arts)
            a.release();
        v.age = 0x4FFFFFFF;
        for (int s = 0; s < v.nslaves; ++s)
            releaseVoice (*v.slaves[s]);
    }

    // ---------------------------------------------------------------- one host block (Gen_Render)
    void renderBlock (float* left, float* right, int hb, float finalVol, float finalPan)
    {
        const auto nv = hb * K;
        // nothing sounding and the effects silent for 2 s: only the running time advances (sample counter, GLOBAL
        // LFO phases, chorus LFOs) — the post chain's states are (numerically) zero
        if (act.empty() && silentOut > (long long) (2.0 * fs))
        {
            counter += nv;
            for (int i = 0; i < kNumArts; ++i)
                if (glfoOn[i])
                    glfoPhase[i] = (u32) (glfoPhase[i] + (u32) (std::uint64_t) rnd ((double) nv / tick * (double) lfoInc[i]));
            fxTail = 0;
            if (post.hasChorus)
                post.chorus.advance (nv >> (order - post.fo));
            return;
        }
        std::fill (Ml.begin(), Ml.begin() + nv, 0.0f);
        std::fill (Mr.begin(), Mr.begin() + nv, 0.0f);
        std::fill (Fl.begin(), Fl.begin() + nv, 0.0f);
        std::fill (Fr.begin(), Fr.begin() + nv, 0.0f);
        const bool link = P->S.b4f7at (5) != 0; // the plugin does not send "link velocity off" (§1.3)
        // the voices draw from the noise generator one after the other (last to first, a whole block each): each
        // voice's start in the sequence is known in advance, so voices can be rendered in lockstep
        auto seq = noise;
        for (auto it = act.rbegin(); it != act.rend(); ++it)
        {
            auto& v = **it;
            const auto fv = link ? finalVol * v.initVol : finalVol;
            v.fvol = fv;
            v.fpan = finalPan + v.notePan;
            for (int s = 0; s < v.nslaves; ++s)
                v.slaves[s]->fvol = fv, v.slaves[s]->fpan = v.fpan;
            if (v.state >= 10)
                continue;
            v.lcg = seq;
            auto draws = v.noiseOps;
            for (int s = 0; s < v.nslaves; ++s)
                draws += v.slaves[s]->noiseOps;
            seq = lcgJump (seq, (std::uint64_t) nv * (std::uint64_t) draws);
        }
        noise = seq;
        for (std::size_t i = act.size(); i > 0;)
        {
            // a batch: consecutive voices (render order) on the same tick grid, without unison slaves
            Voice* batch[kBatch];
            int nb = 0;
            while (i > 0 && nb < kBatch)
            {
                auto* v = act[i - 1];
                const bool fits = v->nslaves == 0 && v->state < 10 && ! (v->tcnt < 0 && v->gridTcnt > 0)
                                  && (nb == 0 || (v->tcnt == batch[0]->tcnt && v->tick == batch[0]->tick));
                if (! fits)
                    break;
                batch[nb++] = v;
                --i;
            }
            if (nb >= 2)
                renderBatch (batch, nb, nv);
            else if (nb == 1)
                renderVoice (*batch[0], nv);
            else
                renderVoice (*act[--i], nv);
        }
        const bool anyVoice = ! act.empty();
        for (auto it = act.begin(); it != act.end();)
            if ((*it)->state >= 10)
            {
                freeVoiceObj (**it);
                it = act.erase (it);
            }
            else
                ++it;
        counter += nv;
        for (int i = 0; i < kNumArts; ++i)
            if (glfoOn[i])
                glfoPhase[i] = (u32) (glfoPhase[i] + (u32) (std::uint64_t) rnd ((double) nv / tick * (double) lfoInc[i]));
        if (anyVoice)
            fxTail = tailLen;
        else
            fxTail = std::max<long long> (0, fxTail - nv);
        const bool fxActive = fxTail > 0;
        for (int i = 0; i < nv; ++i)
        {
            Mdl[(std::size_t) i] = Ml[(std::size_t) i], Mdr[(std::size_t) i] = Mr[(std::size_t) i];
            Fdl[(std::size_t) i] = Fl[(std::size_t) i], Fdr[(std::size_t) i] = Fr[(std::size_t) i];
        }
        post.process (Mdl.data(), Mdr.data(), Fdl.data(), Fdr.data(), nv, fxActive);
        bool silent = true;
        for (int i = 0; i < hb; ++i)
        {
            left[i] += (float) Mdl[(std::size_t) i];
            right[i] += (float) Mdr[(std::size_t) i];
            silent = silent && std::abs (Mdl[(std::size_t) i]) < 1e-10 && std::abs (Mdr[(std::size_t) i]) < 1e-10;
        }
        silentOut = silent && act.empty() ? silentOut + hb : 0;
    }
};

// ------------------------------------------------------------------------------------------------------------------
// public API

std::shared_ptr<const Patch> parsePatch (const std::vector<std::uint8_t>& state)
{
    auto P = std::make_shared<Patch>();
    if (! parseState (state, P->S))
        return nullptr;
    auto& S = P->S;
    P->gibbs = S.b4f6 != 0;
    buildArts (*P);
    for (int k = 0; k < 3; ++k)
        buildShaper (*P, k);
    // wavetables only for operators that can sound (output, FX, filter input, FM/RM source); one thread per table
    std::vector<std::future<void>> jobs;
    for (int u = 0; u < 6; ++u)
    {
        bool used = S.cell (0, u, 8) != 0 || S.cell (0, u, 7) != 0;
        for (int i = 0; i < 6; ++i)
            used = used || S.cell (0, i, u) != 0 || S.cell (1, i, u) != 0;
        for (int f = 6; f < 9; ++f)
            used = used || S.cell (0, f, u) != 0;
        const auto& U = S.units[(std::size_t) u];
        if (! used && U.op[0x826] == 0) // (plucked operators are excited at every note-on, sounding or not)
            continue;
        jobs.push_back (std::async (std::launch::async, [&P, &U, u] {
            P->wave[(std::size_t) u] = opWave (U, P->gibbs);
            if (U.op[0x827] != 0 && U.op[0x826] == 0)
            {
                std::vector<double> x (P->wave[(std::size_t) u].begin(), P->wave[(std::size_t) u].begin() + kN);
                P->spectrum[(std::size_t) u] = rfft (x.data(), (std::size_t) kN);
            }
        }));
    }
    for (auto& j : jobs)
        j.get();
    return P;
}

bool stateUsable (const std::vector<std::uint8_t>& state)
{
    StateRaw S;
    return parseState (state, S);
}

// ------------------------------------------------------------------------------------------------------------------
// the main settings page (REA_rodada74 §3): controls of Sytrus's "Main settings" tab (names and ranges from the
// plugin's form), each an int of the main block (index = offset / 4) or, for PORTA, a byte of the flag block

namespace
{
constexpr int kPortaByte = -1; // byte 1 of the flag block after the main block (REA_rodada11 §1.2)

const std::vector<Setting>& settingTable()
{
    static const std::vector<Setting> t = {
        { "Master level", 0, 0, 160 }, { "Master LFO", 1, -128, 128 }, { "Master pitch", 2, -24, 24 },
        { "Vol attack", 165, 0, 256 }, { "Vol decay", 166, 0, 256 }, { "Vol sustain", 167, -128, 128 }, { "Vol release", 168, 0, 256 },
        { "Flt attack", 169, 0, 256 }, { "Flt decay", 170, 0, 256 }, { "Flt sustain", 171, -128, 128 }, { "Flt release", 172, 0, 256 },
        { "Unison", 173, 1, 9 }, { "Uni pan", 174, 0, 128 }, { "Uni volume", 175, 16, 128 }, { "Uni pitch", 176, 0, 256 },
        { "Uni sub", 177, 0, 96 }, { "Uni phase", 178, 0, 128 }, { "Uni env var", 179, 0, 192 },
        { "Mod X", 190, 0, 512 }, { "Mod Y", 191, 0, 512 },
        { "Porta", kPortaByte, 0, 1 },
        // quality: oversampling order (2^order) and extra-high envelope accuracy, live ("draft") and export
        { "Live oversampling", 195, 0, 4 }, { "Export oversampling", 196, 0, 4 },
        { "Live env accuracy", 193, 0, 1 }, { "Export env accuracy", 194, 0, 1 } };
    return t;
}

// the decompressed body of a state the engine can read, or nothing
std::optional<std::vector<std::uint8_t>> settingsBody (const std::vector<std::uint8_t>& state)
{
    StateRaw S;
    if (! parseState (state, S))
        return std::nullopt;
    auto body = inflateZlib (state.data() + 4, state.size() - 4);
    if (! body || body->size() < 962 + 4 + 3 + 2)
        return std::nullopt;
    return body;
}

std::size_t settingOffset (int index) noexcept
{
    return index == kPortaByte ? (std::size_t) (962 + 4 + 3 + 1) : (std::size_t) (4 * index);
}
} // namespace

const std::vector<Setting>& settings()
{
    return settingTable();
}

std::optional<int> settingValue (const std::vector<std::uint8_t>& state, int setting)
{
    const auto& t = settingTable();
    if (setting < 0 || setting >= (int) t.size())
        return std::nullopt;
    const auto body = settingsBody (state);
    if (! body)
        return std::nullopt;
    const auto at = settingOffset (t[(std::size_t) setting].index);
    if (t[(std::size_t) setting].index == kPortaByte)
        return (*body)[at] != 0 ? 1 : 0;
    std::int32_t v = 0;
    std::memcpy (&v, body->data() + at, 4);
    return (int) v;
}

bool setSetting (std::vector<std::uint8_t>& state, int setting, int value)
{
    const auto& t = settingTable();
    if (setting < 0 || setting >= (int) t.size())
        return false;
    auto body = settingsBody (state);
    if (! body)
        return false;
    const auto& d = t[(std::size_t) setting];
    const auto v = std::clamp (value, d.low, d.high);
    const auto at = settingOffset (d.index);
    if (d.index == kPortaByte)
        (*body)[at] = (std::uint8_t) (v != 0 ? 1 : 0);
    else
    {
        const std::int32_t x = v;
        std::memcpy (body->data() + at, &x, 4);
    }
    const auto z = zlibStored (*body);
    state.resize (4); // the version word stays
    state.insert (state.end(), z.begin(), z.end());
    return true;
}

Engine::Engine() : impl (std::make_unique<Impl>()) {}
Engine::~Engine() = default;

void Engine::prepare (double sampleRate, bool renderMode, int maxBlock)
{
    impl->fs = sampleRate > 0 ? sampleRate : 44100.0;
    impl->renderMode = renderMode;
    impl->maxBlock = std::clamp (maxBlock, 16, 8192);
    impl->configure();
}

void Engine::setPatch (std::shared_ptr<const Patch> patch)
{
    current = std::move (patch);
    impl->keep = current;
    impl->P = current.get();
    impl->act.clear();
    for (auto& o : impl->overrides)
        o.clear();
    for (auto& o : impl->overrideSpectrum)
        o.clear();
    impl->configure();
}

void Engine::setTempo (double bpm) noexcept
{
    if (bpm > 0 && bpm != impl->bpm)
    {
        impl->bpm = bpm;
        impl->computeTimes();
    }
}

void Engine::noteOn (const NoteParams& note) noexcept { impl->noteOn (note, maxPoly); }

void Engine::release (std::int64_t id) noexcept
{
    for (auto* v : impl->act)
        if (v->id == id)
            impl->releaseVoice (*v);
}

void Engine::releaseKey (int key) noexcept
{
    for (auto* v : impl->act)
        if (v->key == key && v->state < 5)
            impl->releaseVoice (*v);
}

void Engine::slideTo (double toCents, float samples) noexcept
{
    for (auto* v : impl->act)
        if (v->state < 5)
        {
            v->slide.start ((float) (toCents - v->noteFinal), samples);
            for (int s = 0; s < v->nslaves; ++s)
                v->slaves[s]->slide.start ((float) (toCents - v->slaves[s]->noteFinal), samples);
        }
}

void Engine::killAll() noexcept
{
    for (auto* v : impl->act)
        impl->killVoice (*v);
}

void Engine::reset() noexcept
{
    if (impl->P != nullptr)
    {
        impl->act.clear();
        impl->configure();
    }
}

void Engine::render (float* left, float* right, int n, float finalVol, float finalPan) noexcept
{
    if (impl->P == nullptr)
        return;
    while (n > 0)
    {
        const auto hb = std::min (n, impl->maxBlock);
        for (auto* v : impl->act)
        {
            impl->slideVoice (*v, hb);
            for (int s = 0; s < v->nslaves; ++s)
                impl->slideVoice (*v->slaves[s], hb);
        }
        impl->renderBlock (left, right, hb, finalVol, finalPan);
        left += hb;
        right += hb;
        n -= hb;
    }
}

bool Engine::idle() const noexcept { return impl->P == nullptr || (impl->act.empty() && impl->silentOut > (long long) (2.0 * impl->fs)); }

int Engine::activeVoices() const noexcept { return (int) impl->act.size(); }

bool Engine::followsRunningTime() const noexcept { return impl->runningTime; }

bool Engine::Impl::computeRunningTime() const noexcept
{
    if (P == nullptr)
        return false;
    for (int i = 0; i < kNumArts; ++i)
        if (glfoOn[i])
            return true;
    for (int u = 0; u < 6; ++u)
        if (! P->wave[(std::size_t) u].empty() && P->S.units[(std::size_t) u].op[0x823] != 0)
            return true;
    return false;
}

void Engine::overrideTable (int op, const std::vector<float>& table)
{
    if (op < 0 || op > 5 || table.size() != (std::size_t) kN + 1)
        return;
    impl->overrides[(std::size_t) op] = table;
    impl->overrideSpectrum[(std::size_t) op].clear();
}

void Engine::setRandSeed (std::uint32_t seed) noexcept { impl->randseed = seed; }
} // namespace sytrus
} // namespace lynx

// ------------------------------------------------------------------------------------------------------------------
// LYNX instrument

namespace lynx
{
namespace
{
class SytrusInstrument final : public Instrument
{
public:
    ~SytrusInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    // FL plays Sytrus with the state's live settings and exports with its render settings (oversampling order and
    // envelope tick, §1.2, §7.2); LYNX does the same: render settings while an offline render (export) is running
    void prepare (double sr) noexcept override
    {
        take();
        rate.store (sr);
        mode.store (offlineRendering());
        if (engine != nullptr)
        {
            engine->prepare (sr, mode.load(), block);
            engine->setTempo (tempo.load());
        }
    }

    // UI thread: a new state builds a new engine (wavetables, effect rings) that the audio thread picks up
    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        tempo.store (bpm > 0 ? bpm : 120.0);
        channelCents.store (channel.sampler.pitch * 100.0f);
        mono.store (channel.synth.mono);
        const bool render = offlineRendering();
        if (channel.pluginState == loadedState && loadedRate == rate.load() && loadedMode == render)
            return;
        if (channel.pluginState != loadedState)
            patch = sytrus::parsePatch (channel.pluginState);
        loadedState = channel.pluginState;
        loadedRate = rate.load();
        loadedMode = render;
        auto e = std::make_unique<sytrus::Engine>();
        e->setTempo (tempo.load());
        e->prepare (loadedRate, render, block);
        if (patch != nullptr)
            e->setPatch (patch);
        delete incoming.exchange (e.release());
    }

    void setNextNoteExpression (float pan, float cents, bool porta) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
        nextPorta = porta;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        if (engine == nullptr)
            return;
        sytrus::NoteParams p;
        p.key = (float) key;
        p.cents = channelCents.load() + nextCents;
        p.velocity = std::clamp (velocity, 0.0f, 1.0f);
        p.pan = nextPan;
        // LYNX's per-note porta flag inverts the patch's PORTA button, as FL's note colour 14 does (§6.4; inference)
        p.colour = nextPorta ? 14 : 0;
        p.id = ++serial;
        nextPan = 0.0f, nextCents = 0.0f, nextPorta = false;
        engine->setMaxPolyphony (mono.load() ? 1 : 1 << 20);
        engine->noteOn (p);
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        if (engine != nullptr)
            engine->slideTo (100.0 * std::clamp (key, 0, 127) - 6000.0 + (double) channelCents.load() + (double) cents, samples);
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (engine != nullptr)
            engine->releaseKey (key);
    }

    void allNotesOff() noexcept override
    {
        take();
        if (engine != nullptr)
            engine->killAll();
    }

    // FL hands Sytrus Final.Vol (= the channel volume, "link velocity" off) and Final.Pan (channel + note pan) and the
    // plugin applies them inside its own pan law (§6.1). LYNX's channel gains are gl = √2·V·cos((1 + p)π/4),
    // gr = √2·V·sin(…), from which V and p are recovered exactly; the output is then added at unit gain.
    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (engine == nullptr)
            return;
        engine->setTempo (tempo.load());
        const auto vol = std::sqrt ((gl * gl + gr * gr) * 0.5f);
        const auto pan = vol > 0.0f ? std::atan2 (gr, gl) * 1.2732395f - 1.0f : 0.0f;
        engine->render (left, right, n, vol, pan);
        if (engine->activeVoices() > 0)
            quiet = 0;
        else
        {
            // trailing silence of this call (effect tails)
            int k = n;
            while (k > 0 && std::abs (left[k - 1]) < 1e-7f && std::abs (right[k - 1]) < 1e-7f)
                --k;
            quiet = k == 0 ? quiet + n : n - k;
        }
    }

    // idle once no voice sounds and the effect tails have been silent for 2 s — unless the patch follows the plugin's
    // running time (GLOBAL LFO / operator phase), which advances with every block in FL
    bool idle() const noexcept override
    {
        return engine == nullptr
               || (engine->activeVoices() == 0 && quiet >= (long long) (2.0 * rate.load()) && ! engine->followsRunningTime());
    }

private:
    static constexpr int block = 512;

    void take() noexcept
    {
        if (auto* e = incoming.exchange (nullptr))
        {
            delete retired.exchange (engine.release());
            engine.reset (e);
            quiet = 1 << 30;
        }
    }

    std::unique_ptr<sytrus::Engine> engine;
    std::atomic<sytrus::Engine*> incoming { nullptr }, retired { nullptr };
    std::atomic<double> rate { 44100.0 }, tempo { 120.0 };
    std::atomic<float> channelCents { 0.0f };
    std::atomic<bool> mono { false };
    std::vector<std::uint8_t> loadedState;
    std::shared_ptr<const sytrus::Patch> patch;
    double loadedRate = 0.0;
    bool loadedMode = false;
    std::atomic<bool> mode { false };
    float nextPan = 0.0f, nextCents = 0.0f;
    bool nextPorta = false;
    std::int64_t serial = 0;
    long long quiet = 1 << 30;
};
} // namespace

std::unique_ptr<Instrument> makeSytrusInstrument() { return std::make_unique<SytrusInstrument>(); }
} // namespace lynx
