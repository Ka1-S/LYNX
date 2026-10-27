#include "core/FruityShapers.h"

// Fruity WaveShaper and Fruity Soft Clipper, exact engines (docs/analise/REA_rodada51.md). Clean-room: written from
// the behaviour, formulas and data layouts described there; nothing of Image-Line's code or data is copied.
//
// Arithmetic follows the plugins operation by operation: float/double steps in their order with one rounding each
// (no contraction), and the x87 kernels in 80-bit long double under the control word the plugins use (round to
// nearest, or toward zero in the interpolating WaveShaper kernels).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Instruments.h"
#include "core/StateHandoff.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#pragma STDC FENV_ACCESS ON
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off", "rounding-math")
#endif

namespace lynx::flshape
{
namespace
{
using ld = long double;

// ------------------------------------------------------------------------------------------- x87 control word (§1.3)
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Mode
{
    unsigned short saved = 0;
    explicit X87Mode (bool truncate) noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved) : : "memory");
        unsigned short cw = (unsigned short) ((saved & ~0x0F00u) | 0x0300u | (truncate ? 0x0C00u : 0u));
        asm volatile ("fldcw %0" : : "m"(cw) : "memory");
    }
    ~X87Mode() { asm volatile ("fldcw %0" : : "m"(saved) : "memory"); }
    X87Mode (const X87Mode&) = delete;
    X87Mode& operator= (const X87Mode&) = delete;
};
#else
// no x87 (ARM64): conversions follow the kernel's rounding direction (arithmetic stays round-to-nearest; precision
// is the platform's long double)
inline thread_local bool x87Truncate = false; // the conversions below follow it (arithmetic stays round-to-nearest)
struct X87Mode
{
    bool saved;
    explicit X87Mode (bool truncate) noexcept : saved (x87Truncate) { x87Truncate = truncate; }
    ~X87Mode() { x87Truncate = saved; }
    X87Mode (const X87Mode&) = delete;
    X87Mode& operator= (const X87Mode&) = delete;
};
#endif

// long double → integer in the current x87 rounding mode (what the plugins' fist/fistp do), without the control-word
// round trip a C++ conversion makes; |v| < 2^63
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
inline long long x87Int (long double v) noexcept
{
    long long r;
    asm ("fistpll %0" : "=m"(r) : "t"(v) : "st");
    return r;
}
// exact conversions straight between memory and the x87 stack (a C++ conversion goes through an SSE register and a
// temporary), and frndint: the value rounded to an integer in the current mode, as a long double
inline long double x87Load (const float* p) noexcept
{
    long double r;
    asm ("flds %1" : "=t"(r) : "m"(*p));
    return r;
}
inline long double x87Load (const std::int16_t* p) noexcept
{
    long double r;
    asm ("filds %1" : "=t"(r) : "m"(*p));
    return r;
}
inline long double x87Round (long double v) noexcept
{
    long double r;
    asm ("frndint" : "=t"(r) : "0"(v));
    return r;
}
// (float) v in the current rounding mode, stored
inline void x87Store (float* p, long double v) noexcept { asm ("fstps %0" : "=m"(*p) : "t"(v) : "st"); }
#else
inline long long x87Int (long double v) noexcept { return x87Truncate ? (long long) std::trunc (v) : (long long) std::nearbyint (v); }
inline long double x87Load (const float* p) noexcept { return *p; }
inline long double x87Load (const std::int16_t* p) noexcept { return *p; }
inline long double x87Round (long double v) noexcept { return x87Truncate ? std::trunc (v) : std::nearbyint (v); }
inline void x87Store (float* p, long double v) noexcept { *p = (float) v; }
#endif

inline float F (double v) noexcept { return (float) v; }
inline std::int32_t i32At (const std::vector<std::uint8_t>& d, std::size_t o) noexcept
{
    std::int32_t v = 0;
    std::memcpy (&v, d.data() + o, 4);
    return v;
}
inline double f64At (const std::vector<std::uint8_t>& d, std::size_t o) noexcept
{
    double v = 0;
    std::memcpy (&v, d.data() + o, 8);
    return v;
}
inline float f32At (const std::vector<std::uint8_t>& d, std::size_t o) noexcept
{
    float v = 0;
    std::memcpy (&v, d.data() + o, 4);
    return v;
}
int rawFrom (float v, int lo, int hi) noexcept
{
    return lo + (int) std::lround (std::clamp ((double) v, 0.0, 1.0) * (hi - lo));
}
float norm (int raw, int lo, int hi) noexcept { return (float) std::clamp ((double) (raw - lo) / (hi - lo), 0.0, 1.0); }

// FL's level law L11: (11^x − 1)/10 with 1 kept exact (both plugins' post gains, §1.1 / §2.1)
float postGain (int raw) noexcept
{
    const float n = (float) raw * 0.0078125f;
    if (n == 1.0f)
        return 1.0f;
    return F ((std::exp ((double) (n * 2.39789534f)) - 1.0) * (double) 0.1f);
}

// ----------------------------------------------------------------------------------------------- map evaluator (§2.4)
constexpr double kEps = 5.960464477539063e-08; // float 2^-24 (as a double)

struct Pt
{
    float dt = 0, y = 0, t = 0;
    int mode = 0, sign = 0;
    float k = 0, ilk = 0, L = 0; // modes 0/1: curvature; other modes reuse p4…p6
    float p4 = 0, p5 = 0, p6 = 0;
};

int stepCount (const Pt& p) noexcept
{
    if (p.mode < 3 || p.mode > 6)
        return -1;
    const auto a = F (1.0 - std::abs ((double) p.t) * 0.6);
    auto n = (int) std::nearbyint ((double) F (std::exp ((double) a * std::log (1000001.0)) - 1.0) * 0.001);
    if (p.mode == 3 && p.t >= 0.0f)
        ++n;
    return n;
}

void prepareSmooth (std::vector<Pt>& pts, int i) noexcept
{
    auto& p = pts[(std::size_t) i];
    const int n = (int) pts.size();
    const double h = (double) p.dt > kEps ? (double) p.dt : kEps;
    const double y1 = p.y;
    const int j = i - 1;
    const double y0 = j >= 0 ? (double) pts[(std::size_t) j].y : y1;
    double rp = 1.0, ypp = y0;
    if (j >= 0 && pts[(std::size_t) j].mode == 10)
    {
        rp = (double) pts[(std::size_t) j].dt / h;
        if (i - 2 >= 0)
            ypp = pts[(std::size_t) (i - 2)].y;
    }
    double rn = 1.0, yn = y1;
    if (i + 1 < n && pts[(std::size_t) (i + 1)].mode == 10)
    {
        rn = (double) pts[(std::size_t) (i + 1)].dt / h;
        yn = pts[(std::size_t) (i + 1)].y;
    }
    const double d = y1 - y0;
    if (std::abs (d) < kEps)
    {
        p.sign = 2;
        return;
    }
    const double a = (ypp - y0) / d, b = (yn - y0) / d;
    rp = rp > kEps ? rp : kEps;
    rn = rn > kEps ? rn : kEps;
    const double sr = (b - 1.0) / rn, s = 1.0 / 1.0, sl = -a / rp;
    if (! (F (sr) * F (s) > 0.0f))
        p.p4 = 0.0f;
    else
    {
        const float w1 = F (rn) * 2.0f + F (1.0), w2 = F (rn) + F (1.0) * 2.0f;
        p.p4 = (F (sr) * F (s) * (w1 + w2)) / (w1 * F (sr) + w2 * F (s));
    }
    if (! (F (s) * F (sl) > 0.0f))
        p.p5 = 0.0f;
    else
    {
        const float w1 = F (1.0) * 2.0f + F (rp), w2 = F (1.0) + F (rp) * 2.0f;
        p.p5 = (F (s) * F (sl) * (w1 + w2)) / (w1 * F (s) + w2 * F (sl));
    }
    p.p6 = F (1.0 - std::abs ((double) p.t));
}

std::vector<Pt> preparePoints (const std::vector<WsPoint>& in)
{
    std::vector<Pt> pts;
    for (const auto& q : in)
    {
        Pt p;
        p.dt = q.dt;
        p.y = q.y;
        p.t = q.tension;
        p.mode = q.mode;
        pts.push_back (p);
    }
    for (int i = 0; i < (int) pts.size(); ++i)
    {
        auto& p = pts[(std::size_t) i];
        if (p.dt < 0.0f)
            p.dt = 0.0f;
        const float t = p.t;
        p.sign = t == 0.0f ? 0 : (t > 0.0f ? 1 : -1);
        if (i == 0)
            continue;
        const int m = p.mode;
        if (m == 0 || m == 1)
        {
            const auto e1 = F (std::exp ((double) std::abs (t) * std::log (1000001.0)) - 1.0);
            const float sg = t > 0.0f ? 1.0f : (t < 0.0f ? -1.0f : 0.0f);
            p.k = e1 * sg;
            if (p.k != 0.0f)
            {
                p.ilk = F (1.0 / std::abs ((double) p.k));
                p.L = F (std::log (std::abs ((double) p.k) + 1.0));
            }
        }
        else if (m == 3)
        {
            const int sg = t < 0.0f ? -1 : 1;
            const int c = stepCount (p);
            p.p6 = (float) sg * 0.5f;
            p.p4 = (float) (c + 1);
            p.p5 = 1.0f / (p.p4 - (float) sg);
        }
        else if (m == 4)
        {
            p.p4 = (float) (stepCount (p) + 1);
            p.p5 = 1.0f / p.p4;
        }
        else if (m == 5)
        {
            const int sg = t < 0.0f ? -1 : 1;
            p.p6 = (float) sg * 0.5f;
            p.p4 = (float) ((stepCount (p) + 1) * 2);
            p.p5 = 6.2831855f;
        }
        else if (m == 6)
        {
            const int c = stepCount (p);
            p.p4 = F ((double) c + 0.5);
            p.p5 = 6.2831855f;
            if (c == 0 && p.sign > 0)
                p.sign = 2;
        }
        else if (m == 7 || m == 8)
            p.p4 = t;
        else if (m == 9)
            p.p4 = F (std::abs ((double) t));
        else if (m == 10)
            prepareSmooth (pts, i);
        else if (m == 11 || m == 12)
        {
            auto d = std::abs ((double) t) * 0.9;
            if (d <= 1.52587890625e-05)
                d = 1.52587890625e-05;
            const auto u = 1.0 - d;
            p.p4 = F (1.0 - u * u * u);
            p.p5 = (1.0f - p.p4) / p.p4;
            p.p6 = p.p5;
            if (p.sign > 0)
            {
                p.p5 = -p.p5;
                p.p6 = p.p5 - 1.0f;
            }
            if (m == 12)
            {
                p.p5 = F ((double) p.p5 * 0.5);
                p.p6 = F ((double) p.p6 * 0.5);
            }
        }
        // linear when flat: modes 0, 1, 7, 8, 9, 11, 12 with no tension
        if (m < 16 && ((0x1b83u >> m) & 1u) != 0 && t == 0.0f)
            p.sign = 2;
    }
    return pts;
}

double curveK (const Pt& p, double x) noexcept
{
    if (p.k == 0.0f)
        return x;
    if (p.k > 0.0f)
        return 1.0 - (std::exp ((1.0 - x) * (double) p.L) - 1.0) * (double) p.ilk;
    return (std::exp (x * (double) p.L) - 1.0) * (double) p.ilk;
}

// the shape of the segment ending at p, at phase ph (0…1)
float shape (const Pt& p, double ph) noexcept
{
    if (p.sign == 2)
        return F (ph);
    const double p4 = p.p4, p5 = p.p5, p6 = p.p6;
    switch (p.mode)
    {
        case 0: return F (curveK (p, ph));
        case 1:
            if (ph < 0.5)
                return F (curveK (p, ph * 2.0) * 0.5);
            return F (1.0 - curveK (p, (1.0 - ph) * 2.0) * 0.5);
        case 3: return (float) (int) std::nearbyint (ph * p4 - p6) * p.p5;
        case 4:
        {
            const auto v = ph * p4 - 0.5;
            const auto i2 = std::nearbyint (v - 0.5);
            auto d = (v - i2) * 2.0 - 1.0;
            if (p.sign < 0)
            {
                const auto z = 1.0 - std::abs (d);
                d = (1.0 - z * z * z) * (std::signbit (d) ? -1.0 : 1.0);
            }
            else
                d = d * d * d;
            return F ((i2 + d * 0.5 + 1.0) * p5);
        }
        case 5: return (float) (((long long) std::nearbyint (ph * p4 - p6)) & 1);
        case 6:
        {
            if (p.sign < 0)
                return F ((std::sin ((ph * p4 + 0.75) * p5) + 1.0) * 0.5);
            const auto v = ph * p4 - 0.5;
            const auto i2 = std::nearbyint (v - 0.5);
            return F (std::abs ((v - i2) * 2.0 - 1.0));
        }
        case 7:
        {
            const auto d = p.sign > 0 ? 1.0 - ph : ph;
            const auto d3 = d * d * d;
            return F (ph - (d3 * d3 - d) * p4);
        }
        case 8:
        {
            if (ph < 0.5)
            {
                const auto x = ph * 2.0;
                const auto d = p.sign > 0 ? 1.0 - x : x;
                const auto d3 = d * d * d;
                return F ((x - (d3 * d3 - d) * p4) * 0.5);
            }
            const auto x = (1.0 - ph) * 2.0;
            const auto d = p.sign > 0 ? 1.0 - x : x;
            const auto d3 = d * d * d;
            return F (1.0 - (x - (d3 * d3 - d) * p4) * 0.5);
        }
        case 9:
        {
            const auto hp = (double) 1.5707963705062866;
            const auto d = p.sign > 0 ? std::sin (ph * hp) : 1.0 - std::cos (ph * hp);
            return F (ph - (ph - d) * p4);
        }
        case 10:
        {
            const auto x2 = ph * ph, x1 = ph - 1.0;
            return F (ph + ((((x2 * 3.0 - x2 * 2.0 * ph) + x2 * x1 * p4) + ph * x1 * x1 * p5) - ph) * p6);
        }
        case 11:
            if (p.sign < 1)
                return F (p5 / (1.0 - p4 * ph) - p6);
            return F (p5 / (1.0 - p4 * (1.0 - ph)) - p6);
        case 12:
            if (ph < 0.5)
            {
                if (p.sign < 1)
                    return F (p5 / (1.0 - p4 * ph * 2.0) - p6);
                return F (p5 / (1.0 - p4 * (1.0 - ph * 2.0)) - p6);
            }
            else
            {
                const auto x = (1.0 - ph) * 2.0;
                if (p.sign < 1)
                    return F ((1.0 - p5 / (1.0 - p4 * x)) + p6);
                return F ((1.0 - p5 / (1.0 - p4 * (1.0 - x))) + p6);
            }
        default: return 0.0f; // 2 (hold) and unknown modes
    }
}

const std::vector<WsPoint>& defaultCurve()
{
    static const std::vector<WsPoint> c { { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 1.0f, 0.0f, 0 } };
    return c;
}

// --------------------------------------------------------------------------------------------- oversampling (§2.6)
// polyphase IIR half-band: two chains of three first-order all-passes (y = x[n−1] + (x − y[n−1])·a), in double
constexpr double kA[3] = { 0.1473771136010466, 0.48246854276970014, 0.8830050257693731 };
constexpr double kB[3] = { 0.039151597734460045, 0.3026468483284934, 0.6746159185469639 };

struct HalfBand
{
    // per channel: outputs A1 A2 A3 B1 B2 B3, then the previous inputs of each all-pass
    double s[12] {};
    void clear() noexcept { std::fill (std::begin (s), std::end (s), 0.0); }
    void flush() noexcept
    {
        for (int i = 0; i < 6; ++i)
            if (std::abs (s[i]) < kEps)
                s[i] = 0.0;
    }
    // Both channels in one pass, on two-lane double vectors (lane 0 = left, lane 1 = right): each lane gets the
    // channel's scalar steps (y = x[n−1] + (x − y[n−1])·a per all-pass), in the same order with one rounding each.
    static void up2 (HalfBand& h0, HalfBand& h1, const float* x0, const float* x1, float* y0, float* y1, int m) noexcept
    {
        V2 s[12];
        load (h0, h1, s);
        for (int n = 0; n < m; ++n)
        {
            const V2 v = { (double) x0[n], (double) x1[n] };
            const V2 a1 = s[6] + (v - s[0]) * kA[0];
            s[6] = v, s[0] = a1;
            const V2 b1 = s[9] + (v - s[3]) * kB[0];
            s[9] = v, s[3] = b1;
            const V2 a2 = s[7] + (s[0] - s[1]) * kA[1];
            s[7] = s[0], s[1] = a2;
            const V2 b2 = s[10] + (s[3] - s[4]) * kB[1];
            s[10] = s[3], s[4] = b2;
            const V2 a3 = s[8] + (s[1] - s[2]) * kA[2];
            s[2] = a3;
            const V2 b3 = s[11] + (s[4] - s[5]) * kB[2];
            s[5] = b3;
            s[8] = s[1], s[11] = s[4];
            y0[2 * n] = (float) b3[0];
            y0[2 * n + 1] = (float) a3[0];
            y1[2 * n] = (float) b3[1];
            y1[2 * n + 1] = (float) a3[1];
        }
        store (s, h0, h1);
    }
    static void down2 (HalfBand& h0, HalfBand& h1, const float* x0, const float* x1, float* y0, float* y1, int m) noexcept
    {
        V2 s[12];
        load (h0, h1, s);
        for (int n = 0; n < m; ++n)
        {
            const V2 e = { (double) x0[2 * n], (double) x1[2 * n] }, o = { (double) x0[2 * n + 1], (double) x1[2 * n + 1] };
            const V2 a1 = s[6] + (e - s[0]) * kA[0];
            s[6] = e, s[0] = a1;
            const V2 b1 = s[9] + (o - s[3]) * kB[0];
            s[9] = o, s[3] = b1;
            const V2 a2 = s[7] + (s[0] - s[1]) * kA[1];
            s[7] = s[0], s[1] = a2;
            const V2 b2 = s[10] + (s[3] - s[4]) * kB[1];
            s[10] = s[3], s[4] = b2;
            const V2 a3 = s[8] + (s[1] - s[2]) * kA[2];
            s[8] = s[1], s[2] = a3;
            const V2 b3 = s[11] + (s[4] - s[5]) * kB[2];
            s[11] = s[4], s[5] = b3;
            const V2 y = (b3 + a3) * 0.5;
            y0[n] = (float) y[0];
            y1[n] = (float) y[1];
        }
        store (s, h0, h1);
    }

private:
    using V2 = double __attribute__ ((vector_size (16)));
    static void load (const HalfBand& h0, const HalfBand& h1, V2* s) noexcept
    {
        for (int i = 0; i < 12; ++i)
            s[i] = V2 { h0.s[i], h1.s[i] };
    }
    static void store (const V2* s, HalfBand& h0, HalfBand& h1) noexcept
    {
        for (int i = 0; i < 12; ++i)
            h0.s[i] = s[i][0], h1.s[i] = s[i][1];
        h0.flush();
        h1.flush();
    }
};

// ------------------------------------------------------------------------------------------------- kernels (§2.5)
struct KernelArgs
{
    const std::int16_t* T = nullptr; // entry k at T[k + 1]
    int N = 0;
    float dry = 0, wet = 0, pre = 0;
    bool unipolar = false;
};

constexpr ld kTwo31 = 2147483648.0L;

// The kernels' bodies run under a control word their caller set (X87Mode): the block entry points below set it once
// per call; the smoothed path sets it once per block and calls the bodies sample by sample.
[[gnu::always_inline]] inline void nearestBody (const float* x, float* y, int m, const KernelArgs& a) noexcept
{
    const auto* T = a.T + 1;
    const ld A = a.dry, bK = (ld) a.wet * 3.0517578125e-05L;
    if (! a.unipolar)
    {
        const ld preN = (ld) a.pre * (ld) a.N;
        for (int k = 0; k < m; ++k)
        {
            const ld X = x[k];
            const ld v = X * preN;
            const long long i = std::abs (v) >= kTwo31 ? -2147483648LL : x87Int (v);
            const long long ai = std::min<long long> (i < 0 ? -i : i, a.N);
            const ld t = i < 0 ? -(ld) T[ai] : (ld) T[ai];
            y[k] = (float) (X * A + t * bK);
        }
        return;
    }
    const ld hN = (ld) a.N * 0.5L, pHN = (ld) a.pre * (ld) a.N * 0.5L;
    for (int k = 0; k < m; ++k)
    {
        const ld X = x[k];
        const ld v = X * pHN + hN;
        long long i = std::abs (v) >= kTwo31 ? -2147483648LL : x87Int (v);
        i = std::clamp<long long> (i, 0, a.N);
        y[k] = (float) (X * A + (ld) T[i] * bK);
    }
}

// the interpolating kernels run with the x87 rounding set toward zero: every product, sum and the float store
[[gnu::always_inline]] inline void interpBody (const float* x, float* y, int m, const KernelArgs& a) noexcept
{
    const auto* T = a.T + 1;
    volatile float vdry = a.dry, vwet = a.wet, vpre = a.pre; // computed after the mode change
    const ld A = vdry, bK = (ld) vwet * 3.0517578125e-05L;
    if (! a.unipolar)
    {
        const ld preN = (ld) vpre * (ld) a.N;
        for (int k = 0; k < m; ++k)
        {
            const ld X = x87Load (x + k);
            const ld v = X * preN;
            ld t;
            if (std::abs (v) >= kTwo31)
                t = 0.0L;
            else
            {
                const auto i = x87Int (v); // toward zero in this mode
                const ld fr = v - x87Round (v); // (ld) i: the same integer, without the round trip through memory
                if (i >= 0)
                {
                    const auto j = std::min<long long> (i, a.N);
                    const ld t0 = x87Load (T + j), t1 = x87Load (T + j + 1);
                    t = t0 + (t1 - t0) * fr;
                }
                else
                {
                    const auto j = std::min<long long> (-i, a.N);
                    const ld t0 = x87Load (T + j), t1 = x87Load (T + j + 1);
                    t = -(t0 - (t1 - t0) * fr);
                }
            }
            x87Store (y + k, X * A + t * bK);
        }
        return;
    }
    const ld hN = (ld) a.N * 0.5L, pHN = (ld) vpre * (ld) a.N * 0.5L;
    for (int k = 0; k < m; ++k)
    {
        const ld X = x[k];
        const ld v = X * pHN + hN;
        long long i;
        ld fr;
        if (std::abs (v) >= kTwo31)
        {
            i = -2147483648LL;
            fr = v - (ld) i;
        }
        else
        {
            i = x87Int (v);
            fr = v - (ld) i;
        }
        i = std::clamp<long long> (i, -1, a.N);
        const ld t0 = T[i], t1 = T[i + 1];
        y[k] = (float) (X * A + (t0 + (t1 - t0) * fr) * bK);
    }
}

[[gnu::noinline]] void kernelNearest (const float* x, float* y, int m, const KernelArgs& a) noexcept
{
    const X87Mode mode (false);
    nearestBody (x, y, m, a);
}

[[gnu::noinline]] void kernelInterp (const float* x, float* y, int m, const KernelArgs& a) noexcept
{
    const X87Mode mode (true);
    interpBody (x, y, m, a);
}

inline bool sameBits (float a, float b) noexcept
{
    std::uint32_t x, y;
    std::memcpy (&x, &a, 4);
    std::memcpy (&y, &b, 4);
    return x == y;
}

// SMOOTH: the gains glide sample by sample (float steps, as in FL) and each sample of both channels goes through the
// kernel with that sample's gains. The control word is set once for the block instead of once per sample and channel.
template <bool interp>
[[gnu::noinline]] void kernelSmoothed (float* x0, float* x1, int m, KernelArgs a, float (&sm)[3], float preGain, float wet,
                                       float dry, float smoothC) noexcept
{
    const X87Mode mode (interp);
    auto body = [&a] (float* x, int len) {
        if constexpr (interp)
            interpBody (x, x, len, a);
        else
            nearestBody (x, x, len, a);
    };
    for (int i = 0; i < m; ++i)
    {
        const float p = sm[0] + (preGain - sm[0]) * smoothC;
        const float w = sm[1] + (wet - sm[1]) * smoothC;
        const float d = sm[2] + (dry - sm[2]) * smoothC;
        if (sameBits (p, sm[0]) && sameBits (w, sm[1]) && sameBits (d, sm[2]))
        {
            // the glide reached its fixed point (bit for bit): every later step gives these same gains, so the rest of
            // the block is one constant-gain run (each sample's result depends only on its input and the gains)
            a.pre = p, a.wet = w, a.dry = d;
            body (x0 + i, m - i);
            body (x1 + i, m - i);
            return;
        }
        sm[0] = p, sm[1] = w, sm[2] = d;
        a.pre = p, a.wet = w, a.dry = d;
        body (x0 + i, 1);
        body (x1 + i, 1);
    }
}
} // namespace

// ================================================================================================== Soft Clipper
std::optional<SoftClipperState> parseSoftClipper (const std::vector<std::uint8_t>& state)
{
    if (state.size() < 8)
        return std::nullopt;
    return SoftClipperState { i32At (state, 0), i32At (state, 4) };
}

void softClipperToParams (const SoftClipperState& s, float* params) noexcept
{
    params[0] = norm (s.threshold, 1, 127);
    params[1] = norm (s.post, 0, 160);
}

SoftClipperState softClipperFromParams (const float* params) noexcept
{
    return { rawFrom (params[0], 1, 127), rawFrom (params[1], 0, 160) };
}

void softClip (float* x, int n, int threshold, int post) noexcept
{
    // threshold t = v/128 (ceiling 1); range 1 − t (2^-16 when ≤ 0); its inverse — all floats (§1.1)
    const float t = (float) threshold * 0.0078125f;
    float r = 1.0f - t;
    if (r <= 0.0f)
        r = 1.52587890625e-05f;
    const float inv = 1.0f / r;
    std::int32_t tb = 0;
    std::memcpy (&tb, &t, 4);
    {
        const X87Mode mode (false);
        for (int k = 0; k < n; ++k)
        {
            std::uint32_t xb = 0;
            std::memcpy (&xb, x + k, 4);
            const auto ab = (std::int32_t) (xb & 0x7fffffffu);
            if (ab <= tb) // compared as integers (|x| > t for every |x| when t < 0)
                continue;
            float a = 0;
            std::memcpy (&a, &ab, 4);
            const ld e = ((ld) t - (ld) a) * (ld) inv;
            const auto yv = (float) ((1.0L - std::exp (e)) * (ld) r + (ld) t);
            std::uint32_t yb = 0;
            std::memcpy (&yb, &yv, 4);
            yb |= xb & 0x80000000u;
            std::memcpy (x + k, &yb, 4);
        }
    }
    const float g = postGain (post);
    if (g != 1.0f)
        for (int k = 0; k < n; ++k)
            x[k] = x[k] * g;
}

const char* softClipperParamName (int param) noexcept
{
    static const char* names[softClipperParams] = { "Threshold", "Post" };
    return param >= 0 && param < softClipperParams ? names[param] : "";
}

std::string softClipperParamText (int param, float value01)
{
    char text[32];
    if (param == 0)
    {
        const auto v = rawFrom (value01, 1, 127);
        std::snprintf (text, sizeof (text), "%.1f dB", 20.0 * std::log10 (v / 128.0));
    }
    else
    {
        const auto g = postGain (rawFrom (value01, 0, 160));
        if (g <= 0.0f)
            return "-inf dB";
        std::snprintf (text, sizeof (text), "%+.1f dB", 20.0 * std::log10 ((double) g));
    }
    return text;
}

namespace
{
class SoftClipperEffect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        const auto s = softClipperFromParams (params.data());
        softClip (left, n, s.threshold, s.post);
        softClip (right, n, s.threshold, s.post);
    }
};
} // namespace

std::unique_ptr<Effect> makeSoftClipper() { return std::make_unique<SoftClipperEffect>(); }

// ==================================================================================================== WaveShaper
std::optional<WaveShaperState> parseWaveShaper (const std::vector<std::uint8_t>& d)
{
    // version (low word 3; high word = layout 2…4), pre / mix / post, unipolar, CENTER, oversampling, the map
    // (kind, version 3, points of 24 bytes, 20-byte header), then the options of later layouts (§2.2)
    if (d.size() < 34)
        return std::nullopt;
    auto v = (std::uint32_t) i32At (d, 0);
    if ((std::int32_t) v > 0x40003)
        return std::nullopt;
    if (v > 0xffff)
        v >>= 16;
    WaveShaperState s;
    s.version = (int) v;
    s.pre = i32At (d, 4);
    s.mix = i32At (d, 8);
    s.post = i32At (d, 12);
    s.unipolar = d[16] != 0;
    s.center = d[17] != 0;
    s.oversampling = i32At (d, 18);
    std::size_t o = 22;
    const auto mapVersion = i32At (d, o + 4);
    const auto n = i32At (d, o + 8);
    o += 12;
    if (mapVersion != 3 || n < 0 || n > 100000 || d.size() < o + 24 * (std::size_t) n + 20)
        return std::nullopt;
    float prev = 0.0f;
    for (int i = 0; i < n; ++i, o += 24)
    {
        WsPoint p;
        p.dt = (float) f64At (d, o);
        p.y = (float) f64At (d, o + 8);
        if (std::isnan (p.y))
            p.y = prev;
        prev = p.y;
        p.tension = f32At (d, o + 16);
        p.mode = d[o + 20];
        s.points.push_back (p);
    }
    o += 20;
    if (v > 1 && d.size() >= o + 4)
        o += 4; // step-editing flag (editor only)
    if (v > 2 && d.size() > o)
        s.hq = d[o++] != 0;
    if (v > 3 && d.size() > o)
        s.smooth = d[o++] != 0;

    // the map spans exactly 1: a point at 0 is added if the first starts later; every Δt is scaled by 1/total
    auto& pts = s.points;
    if (! pts.empty() && pts[0].dt > 0.0f)
        pts.insert (pts.begin(), WsPoint { 0.0f, pts[0].y, 0.0f, 0 });
    double total = 0.0;
    for (const auto& p : pts)
        total += (double) p.dt;
    if (total > 0.0)
    {
        const double f = 1.0 / total;
        if (f != 1.0)
            for (auto& p : pts)
                p.dt = (float) ((double) p.dt * f);
    }
    else
        pts.push_back (WsPoint { 1.0f, pts.empty() ? 0.0f : pts.back().y, 0.0f, 0 });
    return s;
}

void waveShaperToParams (const WaveShaperState& s, float* params) noexcept
{
    params[0] = norm (s.pre, 64, 192);
    params[1] = norm (s.mix, 0, 128);
    params[2] = norm (s.post, 0, 128);
    params[3] = norm (s.oversampling, 0, 4);
    params[4] = s.hq ? 1.0f : 0.0f;
    params[5] = s.smooth ? 1.0f : 0.0f;
    params[6] = s.center ? 1.0f : 0.0f;
    params[7] = s.unipolar ? 1.0f : 0.0f;
}

const char* waveShaperParamName (int param) noexcept
{
    static const char* names[waveShaperParams] = { "Pre amp", "Mix", "Post gain", "Oversampling", "HQ", "Smooth", "Center", "Unipolar" };
    return param >= 0 && param < waveShaperParams ? names[param] : "";
}

std::string waveShaperParamText (int param, float v)
{
    char text[32];
    switch (param)
    {
        case 0:
        {
            const float s0 = (float) rawFrom (v, 64, 192) * 0.0078125f;
            const auto g = (std::exp ((double) s0 * std::log (4.0)) - 1.0) / 3.0;
            std::snprintf (text, sizeof (text), "%+.1f dB", 20.0 * std::log10 (g));
            return text;
        }
        case 1: std::snprintf (text, sizeof (text), "%d%%", (int) std::lround (rawFrom (v, 0, 128) * 100.0 / 128.0)); return text;
        case 2:
        {
            const auto g = postGain (rawFrom (v, 0, 128));
            if (g <= 0.0f)
                return "-inf dB";
            std::snprintf (text, sizeof (text), "%+.1f dB", 20.0 * std::log10 ((double) g));
            return text;
        }
        case 3:
        {
            const auto k = rawFrom (v, 0, 4);
            if (k == 0)
                return "Off";
            std::snprintf (text, sizeof (text), "x%d", 1 << k);
            return text;
        }
        default: return v >= 0.5f ? "On" : "Off";
    }
}

std::vector<std::int16_t> shaperTable (const std::vector<WsPoint>& points, bool unipolar)
{
    // the map read by FL's incremental evaluator in steps of 1/N, each value × (32767 or 65535) rounded (§2.3)
    auto pts = preparePoints (points.empty() ? defaultCurve() : points);
    const int n = (int) pts.size();
    const double off = unipolar ? 0.5 : 0.0;
    auto level = [&] (int i) { return (double) pts[(std::size_t) i].y - off; };
    const int N = (unipolar ? 2 : 1) * 0x4000 - 1;
    const double sc = (double) ((unipolar ? 2 : 1) * 0x8000 - 1);
    const double dx = 1.0 / N;
    std::vector<std::int16_t> T ((std::size_t) N + 3, 0);

    int idx = 0;
    double rate, ph;
    if (pts[0].dt == 0.0f)
        rate = 1.0, ph = 1.0;
    else
        rate = 1.0 / (double) pts[0].dt, ph = 0.0;
    float start = F (level (0)), delta = 0.0f, val = start;
    for (int i = 0; i <= N; ++i)
    {
        const auto r = std::nearbyint ((double) val * sc);
        T[(std::size_t) i + 1] = (std::int16_t) std::clamp (r, -32768.0, 32767.0);
        ph += dx * rate;
        while (ph >= 1.0)
        {
            const int old = idx;
            ph = rate == 0.0 ? dx : (ph - 1.0) / rate;
            start = F (level (idx));
            if (idx < n - 1)
                ++idx;
            delta = F (level (idx) - (double) start);
            if (old == idx)
                ph = 0.0, rate = 0.0;
            else
            {
                const double dt = pts[(std::size_t) idx].dt;
                if (dt <= 0.0)
                    rate = 1.0, ph += 1.0;
                else
                    rate = 1.0 / dt, ph *= rate;
            }
        }
        val = start + shape (pts[(std::size_t) idx], ph) * delta;
    }
    T[0] = T[1];
    T[(std::size_t) N + 2] = T[(std::size_t) N + 1];
    return T;
}

// ------------------------------------------------------------------------------------------------ curve editor
WaveShaperState waveShaperForEditing (const std::vector<std::uint8_t>& state, const float* params)
{
    if (auto s = parseWaveShaper (state))
    {
        if (s->points.empty())
            s->points = defaultCurve();
        return *s;
    }
    WaveShaperState s;
    s.points = defaultCurve();
    if (params != nullptr)
    {
        s.pre = rawFrom (params[0], 64, 192);
        s.mix = rawFrom (params[1], 0, 128);
        s.post = rawFrom (params[2], 0, 128);
        s.oversampling = rawFrom (params[3], 0, 4);
        s.hq = params[4] >= 0.5f;
        s.smooth = params[5] >= 0.5f;
        s.center = params[6] >= 0.5f;
        s.unipolar = params[7] >= 0.5f;
    }
    return s;
}

std::vector<std::uint8_t> writeWaveShaper (const WaveShaperState& s, const std::vector<std::uint8_t>& base)
{
    // what is kept from an FL state: the map's kind, its 20-byte header and the editor flag (§2.1)
    std::int32_t mapKind = 3, editorFlag = 1;
    std::array<std::uint8_t, 20> header {};
    {
        const std::int32_t defaults[5] = { 0, -1, -1, -1, -1 };
        std::memcpy (header.data(), defaults, sizeof (defaults));
    }
    if (const auto old = parseWaveShaper (base))
    {
        const std::size_t o = 22;
        const auto n = (std::size_t) i32At (base, o + 8);
        const auto end = o + 12 + 24 * n;
        mapKind = i32At (base, o);
        std::memcpy (header.data(), base.data() + end, header.size());
        if (old->version > 1 && base.size() >= end + 24)
            editorFlag = i32At (base, end + 20);
    }

    std::vector<std::uint8_t> b;
    b.reserve (60 + 24 * s.points.size());
    auto i32 = [&b] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    auto f64 = [&b] (double v) { std::uint64_t u; std::memcpy (&u, &v, 8); for (int k = 0; k < 8; ++k) b.push_back ((std::uint8_t) (u >> (8 * k))); };
    auto f32 = [&b] (float v) { std::uint32_t u; std::memcpy (&u, &v, 4); for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) (u >> (8 * k))); };
    i32 (0x40003);
    i32 (s.pre), i32 (s.mix), i32 (s.post);
    b.push_back (s.unipolar ? 1 : 0), b.push_back (s.center ? 1 : 0);
    i32 (s.oversampling);
    const auto& pts = s.points.empty() ? defaultCurve() : s.points;
    i32 (mapKind), i32 (3), i32 ((std::int32_t) pts.size());
    for (const auto& p : pts)
    {
        f64 ((double) std::max (0.0f, p.dt)), f64 ((double) p.y), f32 (p.tension);
        b.push_back (p.mode), b.push_back (0), b.push_back (0), b.push_back (0);
    }
    b.insert (b.end(), header.begin(), header.end());
    i32 (editorFlag);
    b.push_back (s.hq ? 1 : 0), b.push_back (s.smooth ? 1 : 0);
    return b;
}

std::vector<float> waveShaperTimes (const std::vector<WsPoint>& points)
{
    std::vector<float> t;
    t.reserve (points.size());
    double at = 0.0;
    for (const auto& p : points)
    {
        at += (double) std::max (0.0f, p.dt);
        t.push_back ((float) at);
    }
    return t;
}

namespace
{
void setTimes (std::vector<WsPoint>& points, const std::vector<float>& t)
{
    for (std::size_t i = 0; i < points.size(); ++i)
        points[i].dt = i == 0 ? 0.0f : std::max (0.0f, t[i] - t[i - 1]);
}
} // namespace

int addWaveShaperPoint (std::vector<WsPoint>& points, float x, float y)
{
    if (points.size() < 2)
        points = defaultCurve();
    auto t = waveShaperTimes (points);
    x = std::clamp (x, 0.0f, 1.0f);
    y = std::clamp (y, 0.0f, 1.0f);
    // after every point at or before x, never before the first nor after the last
    std::size_t at = 1;
    while (at < points.size() - 1 && t[at] <= x)
        ++at;
    x = std::clamp (x, t[at - 1], t[at]);
    WsPoint p;
    p.y = y;
    p.mode = points[at].mode; // the segment it splits keeps its shape on both sides
    points.insert (points.begin() + (std::ptrdiff_t) at, p);
    t.insert (t.begin() + (std::ptrdiff_t) at, x);
    setTimes (points, t);
    return (int) at;
}

int moveWaveShaperPoint (std::vector<WsPoint>& points, int index, float x, float y)
{
    if (index < 0 || index >= (int) points.size())
        return -1;
    auto t = waveShaperTimes (points);
    const auto i = (std::size_t) index;
    if (i == 0)
        x = 0.0f;
    else if (i == points.size() - 1)
        x = t.back();
    else
        x = std::clamp (x, t[i - 1], t[i + 1]);
    t[i] = x;
    points[i].y = std::clamp (y, 0.0f, 1.0f);
    setTimes (points, t);
    return index;
}

bool removeWaveShaperPoint (std::vector<WsPoint>& points, int index)
{
    if (index <= 0 || index >= (int) points.size() - 1)
        return false; // the ends stay
    auto t = waveShaperTimes (points);
    points.erase (points.begin() + index);
    t.erase (t.begin() + index);
    setTimes (points, t);
    return true;
}

float waveShaperLevelAt (const std::vector<std::int16_t>& table, bool unipolar, float x) noexcept
{
    if (table.size() < 4)
        return std::clamp (x, 0.0f, 1.0f);
    const auto n = (double) (table.size() - 3);
    const auto i = (std::size_t) std::clamp ((long) std::lround ((double) std::clamp (x, 0.0f, 1.0f) * n), 0L, (long) n) + 1;
    return unipolar ? (float) table[i] / 65535.0f + 0.5f : (float) table[i] / 32767.0f;
}

const std::vector<WsModeName>& waveShaperModes()
{
    // FL's point menu, in its order
    static const std::vector<WsModeName> m { { 0, "Single curve" }, { 2, "Hold" },          { 10, "Smooth" },        { 7, "Single curve 2" },
                                             { 11, "Single curve 3" }, { 1, "Double curve" }, { 8, "Double curve 2" }, { 12, "Double curve 3" },
                                             { 9, "Half sine" },      { 3, "Stairs" },       { 4, "Smooth stairs" },  { 5, "Pulse" },
                                             { 6, "Wave" } };
    return m;
}

// ----------------------------------------------------------------------------------------------------- engine
struct WaveShaperEngine::Impl
{
    static constexpr int maxBlock = 512, maxOrder = 4;
    std::vector<std::int16_t> tables[2]; // bipolar, unipolar
    double rate = 44100.0;
    float smoothC = 0.0f, dcA = 0.0f, dcB = 0.96f;
    // controls
    int pre = -1, mix = -1, post = -1, order = 0;
    bool hq = false, smooth = false, center = false, unipolar = false, render = false;
    float dry = 0.0f, wet = 1.0f, preGain = 1.0f;
    float sm[3] {}; // smoothed pre, wet, dry (start at 0, as in FL)
    int stages = -1;
    HalfBand up[maxOrder][2], down[maxOrder][2];
    double dc[2][2] {}; // per channel: position, velocity
    std::vector<float> a[2], b[2];

    Impl()
    {
        for (int c = 0; c < 2; ++c)
        {
            a[c].assign ((std::size_t) maxBlock << maxOrder, 0.0f);
            b[c].assign ((std::size_t) maxBlock << maxOrder, 0.0f);
        }
    }

    void computeGains() noexcept
    {
        const float k = 0.0078125f;
        const float s0 = (float) pre * k;
        preGain = F (std::exp ((double) s0 * std::log (4.0)) - 1.0) / 3.0f;
        const float g = postGain (post);
        const float m = (float) mix * k;
        wet = m * g;
        dry = ((g - wet) * preGain) * g;
    }

    void resetStages() noexcept
    {
        for (auto& s : up)
            for (auto& h : s)
                h.clear();
        for (auto& s : down)
            for (auto& h : s)
                h.clear();
    }

    void dcBlock (float* x, int m, int c) noexcept
    {
        // CENTER: a two-pole tracker subtracted from the signal, x87 state kept in long double within the block
        const X87Mode mode (false);
        ld pos = dc[c][0], vel = dc[c][1];
        const ld A = dcA, B = dcB;
        for (int k = 0; k < m; ++k)
        {
            const ld X = x[k];
            vel = vel + (X - pos) * A;
            pos = pos + vel;
            vel = vel * B;
            x[k] = (float) (X - pos);
        }
        auto fl = [] (ld v) { return std::abs (v) <= (ld) kEps ? 0.0 : (double) v; };
        dc[c][0] = fl (pos);
        dc[c][1] = fl (vel);
    }

    void block (float* L, float* R, int n) noexcept
    {
        const int k = order > 0 ? (render ? 4 : order) : 0;
        if (k != stages)
        {
            resetStages();
            stages = k;
        }
        float* io[2] = { L, R };
        float* buf[2] = { L, R };
        int m = n;
        if (k > 0)
        {
            const float* src[2] = { L, R };
            int len = n;
            for (int s = k - 1; s >= 0; --s)
            {
                const bool toA = ((k - 1 - s) & 1) == 0;
                float* dst0 = toA ? a[0].data() : b[0].data();
                float* dst1 = toA ? a[1].data() : b[1].data();
                HalfBand::up2 (up[s][0], up[s][1], src[0], src[1], dst0, dst1, len);
                src[0] = dst0, src[1] = dst1;
                len *= 2;
            }
            buf[0] = (float*) src[0], buf[1] = (float*) src[1];
            m = len;
        }
        KernelArgs args;
        args.unipolar = unipolar;
        const auto& T = tables[unipolar ? 1 : 0];
        args.T = T.data();
        args.N = (int) T.size() - 3;
        const bool interp = render || hq;
        auto run = [&] (const float* x, float* y, int len) {
            if (interp)
                kernelInterp (x, y, len, args);
            else
                kernelNearest (x, y, len, args);
        };
        if (! smooth)
        {
            args.dry = dry, args.wet = wet, args.pre = preGain;
            for (int c = 0; c < 2; ++c)
                run (buf[c], buf[c], m);
        }
        else if (interp)
            kernelSmoothed<true> (buf[0], buf[1], m, args, sm, preGain, wet, dry, smoothC);
        else
            kernelSmoothed<false> (buf[0], buf[1], m, args, sm, preGain, wet, dry, smoothC);
        if (k > 0)
        {
            const float* src[2] = { buf[0], buf[1] };
            int len = m;
            for (int s = k - 1; s >= 0; --s)
            {
                float* dst[2];
                for (int c = 0; c < 2; ++c)
                    dst[c] = s == 0 ? io[c] : (src[c] == a[c].data() ? b[c].data() : a[c].data());
                len /= 2;
                HalfBand::down2 (down[s][0], down[s][1], src[0], src[1], dst[0], dst[1], len);
                src[0] = dst[0], src[1] = dst[1];
            }
        }
        if (center)
        {
            dcBlock (L, n, 0);
            dcBlock (R, n, 1);
        }
    }
};

WaveShaperEngine::WaveShaperEngine() : d (std::make_unique<Impl>())
{
    setCurve ({});
    prepare (44100.0);
}
WaveShaperEngine::~WaveShaperEngine() = default;

void WaveShaperEngine::setCurve (const std::vector<WsPoint>& points)
{
    d->tables[0] = shaperTable (points, false);
    d->tables[1] = shaperTable (points, true);
}

void WaveShaperEngine::swapCurve (std::vector<std::int16_t>& bipolar, std::vector<std::int16_t>& unipolar) noexcept
{
    d->tables[0].swap (bipolar);
    d->tables[1].swap (unipolar);
}

void WaveShaperEngine::prepare (double sampleRate) noexcept
{
    d->rate = sampleRate > 0 ? sampleRate : 44100.0;
    // parameter glide: 1 − 0.1^(1/(fs·0.005)); CENTER: 2.2835e-5·44100/fs and 0.96 (§2.7, §2.8)
    const double den = (double) ((float) d->rate * 0.005f);
    d->smoothC = F (1.0 - std::exp ((1.0 / den) * std::log (0.1)));
    d->dcA = F (44100.0 / d->rate * 2.2835e-05);
    d->dcB = 0.96f;
    reset();
}

void WaveShaperEngine::reset() noexcept
{
    d->resetStages();
    d->stages = -1;
    for (auto& c : d->dc)
        c[0] = c[1] = 0.0;
}

void WaveShaperEngine::setControls (int pre, int mix, int post, int oversampling, bool hq, bool smooth, bool center,
                                    bool unipolar, bool renderMode) noexcept
{
    if (pre != d->pre || mix != d->mix || post != d->post)
    {
        d->pre = pre, d->mix = mix, d->post = post;
        d->computeGains();
    }
    d->order = std::clamp (oversampling, 0, Impl::maxOrder);
    d->hq = hq, d->smooth = smooth, d->center = center, d->unipolar = unipolar, d->render = renderMode;
}

void WaveShaperEngine::process (float* left, float* right, int n) noexcept
{
    for (int i = 0; i < n; i += Impl::maxBlock)
        d->block (left + i, right + i, std::min (Impl::maxBlock, n - i));
}

namespace
{
class WaveShaperEffect final : public Effect
{
public:
    explicit WaveShaperEffect (const std::vector<WsPoint>& points)
    {
        engine.setCurve (points);
        next.t[0].assign (16386, 0); // the sizes of the tables they will be swapped with (no allocation in process)
        next.t[1].assign (32770, 0);
    }
    void prepare (double sampleRate) noexcept override { engine.prepare (sampleRate); }
    void reset() noexcept override { engine.reset(); }
    // the editor moved a point (round 165): the new tables are built here, off the audio thread, and swapped in at
    // the start of a later block, so the oversampling filters and the DC tracker keep running
    bool acceptState (const std::vector<std::uint8_t>& bytes) noexcept override
    {
        try
        {
            const auto s = parseWaveShaper (bytes);
            if (! s)
                return false;
            Tables x;
            x.t[0] = shaperTable (s->points, false);
            x.t[1] = shaperTable (s->points, true);
            return x.t[0].size() == next.t[0].size() && x.t[1].size() == next.t[1].size() && incoming.offer (x);
        }
        catch (...)
        {
            return false;
        }
    }
    void process (float* left, float* right, int n, const EffectParams& p, double) noexcept override
    {
        if (incoming.take (next))
            engine.swapCurve (next.t[0], next.t[1]);
        engine.setControls (rawFrom (p[0], 64, 192), rawFrom (p[1], 0, 128), rawFrom (p[2], 0, 128), rawFrom (p[3], 0, 4),
                            p[4] >= 0.5f || hqForAllPlugins(), p[5] >= 0.5f, p[6] >= 0.5f, p[7] >= 0.5f, offlineRendering());
        engine.process (left, right, n);
    }

private:
    struct Tables
    {
        std::vector<std::int16_t> t[2];
    };
    WaveShaperEngine engine;
    StateHandoff<Tables> incoming;
    Tables next;
};
} // namespace

std::unique_ptr<Effect> makeWaveShaper (const std::vector<std::uint8_t>& flState)
{
    std::vector<WsPoint> points;
    if (const auto s = parseWaveShaper (flState))
        points = s->points;
    return std::make_unique<WaveShaperEffect> (points);
}
} // namespace lynx::flshape
