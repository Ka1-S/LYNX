#include "core/Harmless.h"
#include "core/HostSlide.h"
#include "core/HarmlessFx.h"
#include "core/Song.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// The engine is specified as a sequence of float32 and double operations (REA_rodada32 §1; the reference model rounds
// at exactly the same places): fusing a·b + c into one FMA would change the rounding of the table indices and the
// pitch, so contraction stays off in this file.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int bankSize = 528;          // positions: 0 self-oscillation, 1 Sub 1, 2 Sub 3, 3 Sub 4, 4…527 harmonics 1…524
constexpr int harmonicCount = 524;
constexpr double pi = 3.141592653589793;
constexpr double twoPi = 2.0 * pi;
const double ln001 = std::log (0.01);
const double ln2 = std::log (2.0);
const double log2Of440 = std::log2 (440.0);
const double twoPiF = (double) (float) twoPi;

double f32 (double x) noexcept { return (double) (float) x; }

// round half to even (numpy's rint, the reference's rounding) without touching the FP environment: adding and
// removing 2^52 (2^23 for floats) rounds in the default round-to-nearest-even mode; larger values are already integers
double rint (double x) noexcept
{
    if (! (std::fabs (x) < 4503599627370496.0))
        return x;
    const auto big = std::copysign (4503599627370496.0, x);
    return (x + big) - big;
}

float rintf (float x) noexcept
{
    if (! (std::fabs (x) < 8388608.0f))
        return x;
    const auto big = std::copysign (8388608.0f, x);
    return (x + big) - big;
}
double sgn (double x) noexcept { return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0; }
const double milli = f32 (0.001); // np.float32(0.001)

// float → int64 as numpy's astype on x86 (non-finite or out-of-range values become INT64_MIN)
std::int64_t toInt64 (double x) noexcept
{
    if (! std::isfinite (x) || std::fabs (x) >= 9.2233720368547758e18)
        return std::numeric_limits<std::int64_t>::min();
    return (std::int64_t) x;
}

// numpy's pairwise summation (used by the reference's table normalisations)
double pairwiseSum (const double* a, std::size_t n) noexcept
{
    if (n < 8)
    {
        double r = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            r += a[i];
        return r;
    }
    if (n <= 128)
    {
        double r[8];
        for (int j = 0; j < 8; ++j)
            r[j] = a[j];
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; ++j)
                r[j] += a[i + (std::size_t) j];
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i)
            res += a[i];
        return res;
    }
    auto n2 = n / 2;
    n2 -= n2 % 8;
    return pairwiseSum (a, n2) + pairwiseSum (a + n2, n - n2);
}

// ---------------------------------------------------------------------------------------------------------------
// Tables (§1.3, §1.4, §1.6, §1.8, §1.9): regenerated from our closed forms, never copied

struct Shape
{
    int n = 0;
    double origin = 0.0;
};

Shape shapeOf (int code) noexcept
{
    switch (code)
    {
        case 0: return { 2048, 0.0 };     case 1: return { 4096, 2047.5 };  case 2: return { 4096, 2047.5 };
        case 3: return { 2048, 2047.0 };  case 4: return { 1024, 0.0 };     case 5: return { 2048, 1023.5 };
        case 6: return { 2048, 1023.5 };  case 7: return { 1024, 1023.0 };  case 8: return { 682, 0.0 };
        case 9: return { 1024, 0.0 };     case 10: return { 1365, 0.0 };    case 11: return { 1024, 0.0 };
        case 12: return { 4096, 2047.5 }; case 13: return { 1024, 0.0 };    case 14: return { 4096, 2047.5 };
        case 15: return { 4096, 2047.5 }; case 16: return { 2048, 1023.5 };
        case 100: return { 1024, 511.5 }; case 101: return { 682, 340.5 };  case 102: return { 341, 170.0 };
        case 103: return { 512, 255.5 };  case 104: return { 2048, 1023.5 }; case 105: return { 1536, 767.5 };
        case 106: return { 768, 383.5 };  case 107: return { 1024, 511.5 }; case 108: return { 1024, 511.5 };
        default: return { 2048, 0.0 };
    }
}

double l72 (double d) noexcept { return std::pow (10.0, -72.0 * d / 20.0) * (1.0 - std::pow (d, 16.0)); }

std::vector<double> buildShape (int code)
{
    const auto [N, origin] = shapeOf (code);
    (void) origin;
    std::vector<double> t ((std::size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const auto x = (double) i / (double) (N - 1);
        double v = 0.0;
        switch (code)
        {
            case 0: v = l72 (x); break;
            case 1: case 2: case 103:
            {
                const auto d = 1.0 - std::fabs (1.0 - (double) ((i + N / 2) % N) * 2.0 / (double) N);
                v = code != 2 ? l72 (d) : 1.0 - l72 (d);
                break;
            }
            case 3: v = l72 (1.0 - x); break;
            case 4: v = std::pow (std::cos (pi * x / 2.0), 4.0); break;
            case 5: v = std::pow (std::sin (pi * x), 4.0); break;
            case 6: v = std::pow (std::cos (pi * x), 4.0); break;
            case 7: v = std::pow (std::sin (pi * x / 2.0), 4.0); break;
            case 8: v = std::cos (pi * x / 2.0); break;
            case 9: v = 0.00075 * (std::pow (1001.0, 1.0 - x) - 1.0) + 0.25 * (1.0 - x); break;
            case 10: v = l72 (1.0 - std::cos (pi * x / 2.0)); break;
            case 11: v = std::pow (std::sin (1.5 * pi * (1.0 - x)), 2.0) * std::pow (std::cos (pi * x / 2.0), 2.0); break;
            case 12:
                v = i < N / 2 ? std::pow (1.0 - (1.0 + std::cos (2.0 * pi * x)) / 4.0, 2.0)
                              : l72 ((double) (i - N / 2) / (double) (N - 1) * 2.0);
                break;
            case 13: v = std::pow (10.0, -0.9 * std::pow (std::sin (pi * x / 2.0), 2.0)); break;
            case 14: v = std::pow (std::cos (4.0 * pi * x), 4.0); break;
            case 15:
            {
                const auto b = std::pow (std::sin (pi * x), 2.0);
                v = std::pow (std::cos (6.0 * pi * x), 4.0) * b * b + 0.125 * b;
                break;
            }
            case 100: case 108: v = std::pow (std::sin (pi * x), 6.0); break;
            case 105: v = std::pow (std::sin (2.0 * pi * x), 8.0); break;
            case 16: case 102: case 104:
            {
                const auto M = N / 3;
                const auto j = std::min (i, N - 1 - i);
                v = j < M ? std::pow (std::sin (pi * j * 0.5 / M), 4.0) : 1.0;
                break;
            }
            case 101: case 107:
            {
                const auto c = N / 2;
                const auto z = (double) (2 * (i - c)) / (double) (N - 1) * 2.0 * pi;
                const auto sinc = i != c ? std::sin (z) / z : 1.0;
                v = code == 101 ? sinc * std::pow (std::sin (pi * x), 2.0) : -sinc * std::pow (std::pow (std::sin (pi * x), 2.0), 0.125);
                break;
            }
            case 106: v = -std::pow (1.0 - std::fabs ((double) (i - N / 2)) / ((double) N / 2.0), 2.0); break;
            default: break;
        }
        t[(std::size_t) i] = v;
    }
    int rms = 0;
    switch (code)
    {
        case 100: rms = 7; break; case 101: rms = 9; break; case 102: rms = 10; break; case 103: rms = 18; break;
        case 104: rms = 5; break; case 105: rms = 5; break; case 107: rms = 10; break; case 108: rms = 7; break;
        default: break;
    }
    if (rms != 0)
    {
        std::vector<double> sq (t.size());
        for (std::size_t i = 0; i < t.size(); ++i)
            sq[i] = t[i] * t[i];
        const auto norm = std::sqrt (pairwiseSum (sq.data(), sq.size()) / (double) N);
        for (auto& v : t)
            v = v * (double) rms / norm;
    }
    for (auto& v : t)
        v = f32 (v);
    return t;
}

constexpr int phaserSize[10] { 1024, 1024, 1024, 1024, 1024, 1024, 2048, 2048, 2048, 2048 };
constexpr double phaserF18[10] { 1024, 1024, 1024, 1024, 1024, 1024, 2048, 2048, 2048, 2048 };
constexpr double phaserF1c[10] { 1024, 1024, 1024, 1024, 1024, 1024, 1024, 512, 2048, 2048 };

double l72f (double d) noexcept
{
    const auto e = (float) ((float) (d * -72.0) * (float) (std::log (10.0) / 20.0));
    return (double) (float) std::exp ((double) e) * (1.0 - std::pow (d, 16.0));
}

std::vector<double> buildPhaser (int type)
{
    const auto N = phaserSize[type];
    std::vector<double> T ((std::size_t) N, 0.0);
    auto th = [N] (int i, int shift) { return 2.0 * pi * (double) ((i + shift) % N) / (double) N; };
    for (int i = 0; i < N; ++i)
    {
        auto& out = T[(std::size_t) i];
        switch (type)
        {
            case 0: out = std::sin (th (i, 3 * N / 4)) + 1.0; break;
            case 1: { const auto v = std::fabs (1.0 - (double) ((i + N / 2) % N) * (2.0 / N)); out = v * v; break; }
            case 2: out = std::sin ((double) i * pi / (double) N); break;
            case 3: { const auto s = std::sin (th (i, 3 * N / 4)) * 0.5 + 0.5; out = std::pow (std::pow (s, 3.0), 2.0); break; }
            case 4: { const auto a = 1.0 - std::fabs (1.0 - (double) ((i + N / 2) % N) * (2.0 / N)); out = l72f (a); break; }
            case 5:
            {
                const auto s = std::pow (std::sin (th (i, 3 * N / 4)) * 0.5 + 0.5, 2.0);
                out = l72f (1.0 - s * s);
                break;
            }
            case 6:
            {
                const auto a = 1.0 - (std::sin (th (i, N / 4)) * 0.5 + 0.5);
                const auto t1 = l72f (a);
                const auto b = std::sin (th (i, 3 * N / 4)) * 0.5 + 0.5;
                const auto t2 = l72f (1.0 - b * b);
                out = f32 (t1) + t2 * 4.0;
                break;
            }
            case 9: out = std::sin (th (i, 3 * N / 4)); break;
            default: break;
        }
    }
    if (type == 7)
    {
        const auto l50 = 1.0 / (double) (N - 1);
        for (int i = 0; i < N; ++i)
            T[(std::size_t) ((i + N / 8) % N)] = (std::sin ((4.0 * i * l50 + 0.75) * 2.0 * pi) + 1.0) * (1.0 - i * l50);
    }
    if (type == 8)
        for (int i = 0; i < N / 2; ++i)
        {
            const auto s = std::sin ((double) ((i + 3 * N / 8) % N) * (4.0 * pi / N));
            const auto e = std::exp (f32 (1.0 - std::fabs (s)) * std::log (1000001.0));
            const auto f = (1.0 - f32 (e - 1.0) / 1000000.0) * sgn (s) + 1.0;
            T[(std::size_t) (i + N / 4)] = std::pow (f, 3.0);
        }
    for (auto& v : T)
        v = f32 (v);
    std::vector<double> sq (T.size());
    for (std::size_t i = 0; i < T.size(); ++i)
        sq[i] = T[i] * T[i];
    const auto r = std::sqrt (pairwiseSum (sq.data(), sq.size()) / (double) N);
    if (r > 0.0)
    {
        const auto k = f32 (1.0 / r);
        for (auto& v : T)
            v = f32 (v * k);
    }
    T.push_back (T[0]); // periodic: the interpolated read uses entry N = entry 0
    return T;
}

struct Tables
{
    std::array<std::vector<double>, 17> filter;
    std::array<std::vector<double>, 9> resonance;
    std::array<std::vector<double>, 10> phaser;
    std::array<double, bankSize> baseRatio {};
    std::array<std::array<double, bankSize>, 6> eq {};
    std::array<double, 8> lhp {};
    std::array<float, 12> smoothSaw {};

    Tables()
    {
        for (int c = 0; c < 17; ++c)
            filter[(std::size_t) c] = buildShape (c);
        for (int c = 0; c < 9; ++c)
            resonance[(std::size_t) c] = buildShape (100 + c);
        for (int c = 0; c < 10; ++c)
            phaser[(std::size_t) c] = buildPhaser (c);
        baseRatio[0] = 1.0, baseRatio[1] = 0.5, baseRatio[2] = 1.5, baseRatio[3] = 2.5;
        for (int k = 1; k <= harmonicCount; ++k)
            baseRatio[(std::size_t) k + 3] = (double) k;
        // six Hann windows over log2 of the base ratios (band 0 = low shelf, band 5 = high shelf)
        for (int k = 0; k < 6; ++k)
            for (int i = 0; i < bankSize; ++i)
            {
                const auto lr = std::log2 (baseRatio[(std::size_t) i]);
                auto w = (std::cos (pi * std::clamp (lr - k, -1.0, 1.0)) + 1.0) / 2.0;
                if ((k == 0 && lr - k < 0.0) || (k == 5 && lr - k > 0.0))
                    w = 1.0;
                eq[(std::size_t) k][(std::size_t) i] = w;
            }
        for (int i = 0; i < 8; ++i)
        {
            const auto r = std::max (baseRatio[(std::size_t) i], 1.0);
            lhp[(std::size_t) i] = std::pow ((std::cos (pi * std::log2 (r) / std::log2 (5.0)) + 1.0) / 2.0, 5.0);
        }
        for (int k = 1; k <= 12; ++k)
            smoothSaw[(std::size_t) k - 1] = (float) ((-11.148862085948322 / k) * std::pow (std::sin (k * 0.2617993877991494) / k, 2.0));
    }
};

const Tables& tables()
{
    static const Tables t;
    return t;
}

// nearest-entry read (quality ≤ 1) and linear interpolation (quality 2) of a filter/resonance shape (§1.6)
double shapeRead (const std::vector<double>& t, float x, bool interpolate) noexcept
{
    const auto N = (int) t.size();
    if (std::isnan (x))
        x = 0.0f;
    else if (std::isinf (x))
        x = x > 0.0f ? (float) (N - 1) : -1.0f;
    if (! interpolate)
        return t[(std::size_t) rintf (std::clamp (x, 0.0f, (float) (N - 1)))];
    const auto i = toInt64 (rintf (x - 0.5f));
    const auto fr = (double) (float) (x - (float) i);
    if (i < 0)
        return t[0];
    if (i >= N - 1)
        return t[(std::size_t) N - 1];
    return f32 (t[(std::size_t) i] * (1.0 - fr) + t[(std::size_t) i + 1] * fr);
}

double g3 (double v) noexcept { return v == 1.0 ? 1.0 : f32 ((std::exp (f32 (v) * std::log (3.0)) - 1.0) * 0.5); }
double g11 (double v) noexcept { return v == 1.0 ? 1.0 : f32 ((std::exp (f32 (v) * std::log (11.0)) - 1.0) * 0.1); }

// colour amplitude and phase (cycles) at the 0-based harmonic index x (0.5 / 1.5 for Sub 3 / Sub 4) — §1.4
void colourEntry (int colour, double x, const float* custom, double& amplitude, double& phase) noexcept
{
    const auto t = (long long) x;
    double a = 0.0, ph = 0.0;
    switch (colour)
    {
        case 7: a = (std::exp ((1.0 / (x + 1.0)) * std::log (7.0)) - 1.0) * 0.2916666666666667; ph = (double) (t & 1) * 0.75; break;
        case 1: a = std::sqrt (0.5) / (x * 0.5 + 1.0); break;
        case 2: a = 0.5 / (x * 0.25 + 1.0); break;
        case 5: case 6:
            if ((t & 1) == 0)
                a = 1.1 / (x + 1.0);
            else
            {
                a = colour == 5 ? 1.1 / ((x + 1.0) * x) : (1.1 / (x + 1.0)) * (1.0 / f32 (std::pow (x, 0.2)));
                ph = 0.25;
            }
            break;
        case 8:
            a = f32 (0.5 / (x + 1.0));
            if (t & 1)
                a = a + 0.5 / (x * 0.5 + 1.0);
            break;
        case 9:
            a = f32 (1.0 / ((x + 1.0) * 3.0));
            if (t & 1)
                a = f32 (a + 1.0 / ((x * 0.5 + 1.0) * 3.0));
            if ((t & 3) == 3)
                a = a + 1.0 / ((x * 0.25 + 1.0) * 3.0);
            break;
        case 10:
            a = f32 (0.5 / (x + 1.0));
            if (t & 1)
                a = f32 (a + 0.25 / (x * 0.5 + 1.0));
            if ((t & 3) == 3)
                a = a + 0.125 / (x * 0.25 + 1.0);
            break;
        case 11:
        {
            const auto i = (int) rint (x); // round half to even: Sub 3 → entry 0, Sub 4 → entry 2
            amplitude = custom != nullptr ? custom[i] : 0.0;
            phase = custom != nullptr ? custom[i + harmonicCount] : 0.0;
            return;
        }
        default:
            a = 1.0 / (x + 1.0);
            if (colour == 3)
                ph = (double) (t & 1) * 0.25;
            if (colour == 4)
                ph = (t & 3) == 1 ? 0.5 : 0.0;
            break;
    }
    amplitude = f32 (a);
    phase = f32 (ph);
}

// IPP's uniform generators (REA round 24 §1.3), as the reference model restates them: integer recurrence, double / float output
struct UniformGenerator
{
    std::int32_t e0 = 0, e1 = 0, e2 = 0, borrow = -1;
    std::uint32_t l = 0x436cbae9u;

    explicit UniformGenerator (std::uint32_t seed) noexcept
    {
        constexpr std::uint32_t A = 69069u, C = 0x3c6ef373u;
        const auto s1 = seed * A + C;
        e0 = (std::int32_t) (((std::uint32_t) (std::int32_t) s1) * A + C); // oldest
        e1 = (std::int32_t) s1;
        e2 = (std::int32_t) seed;                                            // newest
    }

    std::int32_t next() noexcept
    {
        auto v = (std::int32_t) ((std::uint32_t) e1 - (std::uint32_t) e0 + (std::uint32_t) borrow);
        borrow = v < 0 ? -1 : 0;
        if (v < 0)
            v = (std::int32_t) ((std::uint32_t) v - 18u);
        e0 = e1, e1 = e2, e2 = v;
        l = l * 69069u + 0x3c6ef373u;
        return (std::int32_t) ((std::uint32_t) v + l);
    }

    std::uint32_t state() const noexcept { return (std::uint32_t) e2; }
};

// ---------------------------------------------------------------------------------------------------------------
// Patch: everything derived from the raw state, the rate, the quality and the tempo (§1.2…§1.19)

struct Patch
{
    std::array<int, 100> p {};
    std::array<int, 7> t {};
    double fs = 44100.0;
    int quality = 0, block = 88;
    std::array<double, bankSize> a570 {}, p598 {}, ratio {}, gritTable {};
    std::array<double, 12> mask {};
    double mix = 0, mixLfo = 0, lhp = 0, D = 1, grit = 0, masterVol = 0;
    std::array<double, 4> gain580 {};
    int pitchOffset = 0;
    double A = 0, Dn = -1, R = 0;
    int ftype = 0, rtype = 100, phType = 0;
    double W = 1024, lo = 0, hi = 0, knob = 0, kt = 0, fcutScale = 0, envAmount = 0, lfoCut = 0;
    bool adaptive = false;
    double feAttack = 0, feDecay = 0;
    std::array<double, 3> curveAttack {}, curveDecay {};
    double knobC = 0, motionC = 0;
    double noiseLength = 0, noiseStep = 0, rOffset = 0, rMul = 1, rK = 1024, rC = -0.125;
    int lfoShape = 0;
    bool lfoGlobal = false;
    double lfoAttack = 0, lfoInc = 0, lfoP13 = 0, lfoPitch = 0, vib = 0, vibInc = 0, vibR = 0;
    int unison = 1;
    double uniGain = 1, uniS = 0, uniPhase = 0, phRand = 0, kamp = 0;
    double hostEv5 = 1.0, hostEv4 = 0.0; // host answers to voice events 5 / 4 (FL's not observed; the reference's 1 and 0)
    double pluckT = -1, pluckNum = 0;
    int pluckStart = 0;
    double porta = 0, trem = 0, tremInc = 0, tremS = 0, maxStep = 0;
    long long tremM = 0;
    double hzMix2 = 0, hzW = 0, hzWl = 0, hzStrength = 0;
    std::array<int, 4> hzType {};
    double phMix = 0, phW = 0, phWl = 0, phOl = 0, phInc = 0;

    static std::array<double, 3> curve (double x) noexcept
    {
        if (x == 0.0)
            return { 0.0, 0.0, 0.0 };
        const auto q = f32 (f32 (std::exp (std::fabs (x) * std::log (1000001.0)) - 1.0) * std::copysign (1.0, x));
        return { q, f32 (1.0 / std::fabs (q)), f32 (std::log (std::fabs (q) + 1.0)) };
    }

    static double curveValue (const std::array<double, 3>& rec, double t) noexcept
    {
        const auto [q, a, b] = rec;
        if (q == 0.0)
            return t;
        if (q > 0.0)
            return f32 (1.0 - (std::exp ((1.0 - t) * b) - 1.0) * a);
        return f32 ((std::exp (t * b) - 1.0) * a);
    }

    double X (int i) const noexcept { return f32 (p[(std::size_t) i] * milli); }

    void build (const float* raw, double rate, int q, double bpm) noexcept
    {
        const auto& tab = tables();
        for (int i = 0; i < 100; ++i)
            p[(std::size_t) i] = std::isfinite (raw[i]) ? (int) std::lround (raw[i]) : 0;
        for (int i = 0; i < 7; ++i)
            t[(std::size_t) i] = std::isfinite (raw[harmlessComboIndex + i]) ? (int) std::lround (raw[harmlessComboIndex + i]) : 0;
        const float* custom = raw[harmlessCustomFlagIndex] > 0.5f ? raw + harmlessCustomIndex : nullptr;
        fs = rate;
        quality = std::clamp (q, 0, 2);
        const auto fsf = fs;
        static constexpr double controlBlocks[3] { 88.0, 44.0, 8.0 };
        block = std::max (4, (int) rint (controlBlocks[quality] / 4.0 * (44100.0 / fs)) * 4);

        // colour × timbre EQ (table A) and colour phases
        const auto colour = t[4];
        std::array<double, bankSize> A {};
        for (int i = 0; i < harmonicCount; ++i)
            colourEntry (colour, (double) i, custom, A[(std::size_t) i + 4], p598[(std::size_t) i + 4]);
        A[0] = 1.0, p598[0] = 0.0;
        A[1] = A[4], p598[1] = p598[4];
        colourEntry (colour, 0.5, custom, A[2], p598[2]);
        colourEntry (colour, 1.5, custom, A[3], p598[3]);
        std::array<double, 6> g {};
        for (int k = 0; k < 6; ++k)
            g[(std::size_t) k] = g3 (f32 ((p[(std::size_t) k + 5] + 1024) * 0.0009765625));
        for (int i = 0; i < bankSize; ++i)
        {
            auto w = tab.eq[0][(std::size_t) i] * g[0];
            for (int k = 1; k < 6; ++k)
                w = w + tab.eq[(std::size_t) k][(std::size_t) i] * g[(std::size_t) k];
            a570[(std::size_t) i] = f32 (A[(std::size_t) i] * w);
        }
        for (int j = 0; j < 12; ++j)
        {
            const auto v = p[(std::size_t) j + 11];
            mask[(std::size_t) j] = v != 0 ? std::copysign (g11 (std::fabs (X (11 + j))), (double) v) : 0.0;
        }
        mix = X (23), mixLfo = X (24);
        lhp = g11 (f32 (p[1] * 0.0009765625));
        gain580 = { f32 (p[62] * 0.00390625), f32 (p[2] * 0.0009765625), f32 (p[3] * 0.0009765625), f32 (p[4] * 0.0009765625) };
        D = std::exp (p[39] * 0.0008333333333333334 * ln2);
        ratio = tab.baseRatio;
        for (int k = 0; k < harmonicCount; ++k)
            ratio[(std::size_t) k + 4] = 1.0 + (double) k * D;
        ratio[2] = D * 0.5 + 1.0;
        ratio[3] = D * 1.5 + 1.0;
        for (int i = 0; i < bankSize; ++i)
            gritTable[(std::size_t) i] = std::sin ((ratio[(std::size_t) i] - 1.0) * 0.41887902047863906);
        gritTable[0] = 0.0;
        const auto xg = f32 (p[40] * milli);
        grit = f32 (f32 (xg != 0.0 ? (std::exp (std::fabs (xg) * std::log (65.0)) - 1.0) * std::copysign (1.0, xg) : 0.0) * -0.0125);
        const auto x36 = X (36);
        masterVol = f32 ((x36 == 1.0 ? 1.0 : f32 ((std::exp (x36 * std::log (3.0)) - 1.0) * 0.5)) * 0.75);
        pitchOffset = p[38] - 1200;
        this->A = f32 (f32 (fsf) * f32 (std::exp (X (26) * std::log (101.0)) - 1.0)) * 0.05;
        Dn = p[27] >= 1000 ? -1.0 : fsf * (f32 (std::exp (X (27) * std::log (1001.0)) - 1.0) * 0.11995 + 0.05);
        R = fsf * (f32 (std::exp (X (28) * std::log (101.0)) - 1.0) * 0.024900000000000002 + 0.01);

        // filter (§1.6)
        ftype = std::clamp (t[0], 0, 16);
        W = f32 (std::exp ((1.0 - p[56] * 0.004) * ln2) * 1024.0);
        const auto M = f32 (682.6666666666666 / W);
        double eL = 1.0, eR = 0.0;
        switch (ftype)
        {
            case 1: case 2: case 5: case 6: case 14: case 15: case 16: eL = 1.0, eR = 1.0; break;
            case 3: case 7: eL = 0.0, eR = 1.0; break;
            default: break;
        }
        const auto m1 = f32 (M * eL), m2 = f32 (M * eR);
        lo = f32 (std::log (f32 (440.0 * std::pow (2.0, -31.0 / 12.0))) / ln2 - (m1 - std::min (m1, 1.0)));
        hi = f32 (std::log (17000.0) / ln2 + (m2 - std::min (m2, 1.0)));
        knob = f32 (lo + p[54] * 0.0001220703125 * f32 (hi - lo));
        kt = X (57);
        fcutScale = f32 (f32 (hi - lo) * 0.5);
        adaptive = p[47] > 0;
        envAmount = adaptive ? X (52) : f32 (X (52) * f32 (hi - lo));
        lfoCut = f32 (X (58) * 6.0);
        const auto x48 = X (48), x49 = X (49);
        feAttack = p[48] >= 1000 ? 0.0 : f32 (1.0 / (fsf * f32 (std::exp (x48 * std::log (1001.0)) - 1.0) * 0.12 + 1.0));
        feDecay = p[49] >= 1000 ? 0.0 : f32 (-1.0 / (fsf * f32 (std::exp (x49 * std::log (1001.0)) - 1.0) * 0.12 + 1.0));
        curveAttack = curve (X (50));
        curveDecay = curve (X (51));
        knobC = f32 (std::exp (ln001 / (fsf * 0.1 / block)));
        motionC = f32 (std::exp (ln001 / (fsf * 1.25 / block)));

        // resonance (§1.8)
        rtype = 100 + std::clamp (t[1], 0, 8);
        noiseLength = (double) f32 (std::exp ((double) f32 ((p[61] + 2400) * 0.00020833333333333335) * std::log (201.0)) - 1.0)
                          * 2.4900226757369612 + 1.9954648526077097;
        noiseStep = block / (noiseLength * 0.001 * fsf);
        rOffset = f32 (p[61] * 0.0008333333333333334);
        rMul = f32 (std::exp (rOffset * ln2));
        const auto hzScale = p[60] != 0, adaptiveWidth = p[63] > 0;
        rK = hzScale ? (adaptiveWidth ? 71.0389328 : 0.384) : (adaptiveWidth ? 94718.5703 : 1024.0);
        rC = hzScale ? (adaptiveWidth ? 0.5 : -1.5) : (adaptiveWidth ? 0.875 : -0.125);

        // LFO, vibrato (§1.12)
        lfoShape = p[86];
        lfoGlobal = p[87] > 0;
        lfoAttack = f32 (1.0 / (fsf * f32 (std::exp (X (88) * std::log (101.0)) - 1.0) * 0.05 + 1.0));
        const auto tempo = (double) f32 ((double) (float) bpm / (double) (float) (fs * 0xf00));
        const auto v89 = p[89];
        lfoInc = sgn (v89) * std::exp (f32 (std::abs (v89) * milli) * ln2) * tempo;
        lfoP13 = f32 (v89 * 0.6 + 1200.0);
        const auto x42 = X (42);
        lfoPitch = f32 (f32 (x42 != 0.0 ? (std::exp (std::fabs (x42) * std::log (101.0)) - 1.0) * std::copysign (1.0, x42) : 0.0) * 12.0);
        vib = f32 (p[41] * 0.075);
        vibInc = f32 (6.0 / fsf);
        vibR = f32 (3.0 / fsf);

        // unison (§1.14)
        unison = std::clamp (p[65], 1, 9);
        const auto n = unison;
        uniGain = n == 1 ? 1.0 : f32 (1.0 - f32 (std::exp (f32 (1.0 - 1.0 / n) * std::log (3.0)) - 1.0) * 0.5);
        uniS = f32 (f32 (std::exp (f32 (p[67] * 0.00390625) * std::log (41.0)) - 1.0) * 1.875);
        uniPhase = f32 (p[68] * 0.0078125);
        const auto x0 = f32 (p[0] * milli);
        phRand = f32 (f32 (x0 * x0) * sgn (p[0]));
        kamp = f32 (std::exp (ln001 / (0.01 * fsf)));

        // pluck (§1.11)
        pluckT = p[31] >= 1000 ? -1.0 : (double) f32 (std::exp (X (31) * std::log (1001.0)) - 1.0) * 0.11975;
        pluckNum = ln001 * block * (1.0 / fsf);
        pluckStart = 1 - (int) rint (fsf * 0.002 / block);

        // portamento (§1.15): blocks; the sign selects exponential (> 0) or linear (< 0)
        const auto x45 = X (45);
        porta = f32 (f32 (f32 (sgn (x45) * fs) * f32 (std::exp (std::fabs (x45) * std::log (41.0)) - 1.0)) / (block * 26.666666666666668));

        // stereo tremolo (§1.17)
        trem = X (25);
        tremInc = f32 (2.5 / fsf);
        tremM = (long long) rint (fsf * 0.1);
        tremS = f32 (1.0 / (double) (tremM + 1));
        maxStep = f32 (0.001 * (44100.0 / fsf));

        // harmonizer (§1.10)
        const auto hm = f32 (p[79] * milli);
        hzMix2 = f32 (hm * hm);
        hzW = f32 (p[80] * milli);
        hzWl = f32 (p[81] * milli);
        hzStrength = f32 (std::exp (p[83] * 0.00390625 * ln2) * 1.3310003559094952);
        static constexpr std::array<int, 4> hzTypes[5] { { -1, 0, 2, 5 }, { 0, 0, 2, 5 }, { 1, 0, 2, 5 }, { -1, 1, 1, 10 }, { 0, 2, 1, 10 } };
        hzType = hzTypes[std::clamp (p[84], 0, 4)];

        // phaser (§1.9)
        phType = std::clamp (t[2], 0, 9);
        phMix = f32 (p[71] * milli);
        phW = f32 (p[73] * 0.0001);
        phWl = f32 (p[74] * 0.0005);
        phOl = f32 (p[76] * 0.0005);
        const auto v77 = p[77];
        phInc = sgn (v77) * std::exp (f32 (std::abs (v77) * milli) * ln2) * tempo;
    }
};

// one-pole smoother of the library (the double-precision variant; output stored as float) — §1.13
struct Smooth
{
    double c = 0.0, s = 0.0;

    void setN (double n) noexcept { c = n > 0.0 ? std::exp (ln001 / std::max (n, 1.0)) : 0.0; }

    // y[i] = smoothed x[i]; x and y may alias
    void run (const double* x, double* y, int n) noexcept
    {
        auto st = s;
        for (int j = 0; j < n; ++j)
        {
            const auto v = f32 (x[j]);
            st = v + (st - v) * c;
            y[j] = f32 (st);
        }
        s = std::fabs (st) >= 5.960464477539063e-08 ? st : 0.0;
    }

    void runZero (double* y, int n) noexcept
    {
        auto st = s;
        for (int j = 0; j < n; ++j)
        {
            st = 0.0 + (st - 0.0) * c;
            y[j] = f32 (st);
        }
        s = std::fabs (st) >= 5.960464477539063e-08 ? st : 0.0;
    }
};

// volume envelope (§1.13)
struct AmpEnv
{
    Smooth out, dec;
    const Patch* P = nullptr;
    double fs = 44100.0, slope = 0.0, last = 0.0, lastOut = 1.0;
    long long n30 = 0, cnt = 1, end = 0;
    int state = 1;

    void start (const Patch& patch, double vel, double variation) noexcept
    {
        P = &patch;
        fs = patch.fs;
        out = {}, dec = {};
        auto A = patch.A * variation;
        if (patch.p[29] > 0)
            A = A * (1.0 - std::min (1.0, f32 (vel * 1.125)));
        const auto zero = A <= 0.0;
        n30 = (long long) rint (fs * 0.03);
        const auto nA = std::min (A, (double) n30);
        out.setN (nA);
        state = 1;
        cnt = 1;
        const auto a02 = fs * 0.2;
        if (A < a02)
        {
            const auto d = 1.0 - A / a02;
            cnt = (long long) rint (a02 * d * d) + 1;
            A = a02;
        }
        end = (long long) rint (A + nA) + 1;
        slope = 1.0 / (A + 1.0);
        out.s = zero ? 1.0 : 0.0;
        last = f32 ((double) cnt * slope);
        lastOut = 1.0;
    }

    void release() noexcept
    {
        if (state < 6)
            state = 6, end = -1;
    }

    void kill() noexcept // the 30 ms declick fade, after which the record is removed
    {
        state = 9, cnt = 0, end = n30;
        out.setN ((double) n30);
        dec.c = 0.0;
    }

    void render (double* res, int n) noexcept
    {
        int k = 0;
        while (k < n)
        {
            if (end <= cnt)
            {
                if (end == -1)
                {
                    out.setN ((double) n30);
                    dec.setN (P->R * (P->p[30] > 0 ? P->hostEv5 : 1.0));
                }
                else if (state < 9)
                {
                    out.setN (fs * 0.08);
                    if (P->Dn >= 0.0)
                        dec.setN (P->Dn);
                    else
                        dec.c = 1.0;
                    state = std::max (state, 3);
                }
                else
                    state = 10;
                dec.s = last;
                end = 0x7fffffff;
            }
            const auto m = (int) std::min<long long> (end - cnt, n - k);
            auto* raw = res + k;
            if (state < 3)
            {
                const auto off = (float) ((double) cnt * slope), sl = (float) slope;
                for (int j = 0; j < m; ++j)
                    raw[j] = (double) std::min (off + (float) j * sl, 1.0f);
            }
            else
                dec.runZero (raw, m);
            last = raw[m - 1];
            out.run (raw, raw, m);
            cnt += m;
            k += m;
        }
        lastOut = res[n - 1];
    }
};

// plugin-wide running state shared by the voices (all 0 when the plugin is created / flushed)
struct Globals
{
    double vibPhase = 0.0, lfoPhase = 0.0, phPhase = 0.0;
    long long counter = 0;

    void advance (const Patch& P, int n) noexcept
    {
        lfoPhase = lfoPhase + P.lfoInc * n;
        double whole = 0.0;
        phPhase = std::modf ((phPhase + P.phInc * n) * 0.25, &whole) * 4.0;
        vibPhase = f32 (std::modf (f32 (vibPhase + f32 (P.vibInc * f32 (n))), &whole));
        counter += n;
    }
};

const float unisonTable[3][9] {
    { 0.877f, 1.069f, 0.929f, 1.181f, 1.031f, 1.129f, 0.887f, 1.117f, 0.977f },
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 0.8904719352722168f, 1.0764262676239014f, 0.911577045917511f, 1.1025358438491821f, 1.0341261625289917f,
      1.1668611764907837f, 0.8539709448814392f, 1.2062726020812988f, 1.0f } };

// ---------------------------------------------------------------------------------------------------------------
// The sine-bank kernel: groups of 8 partials kept in registers over a segment of m samples; each partial runs the
// two-term recurrence x ← c·x − y and adds (T + (a − T)·kamp^j)·x into its lane sum acc[8·j + lane]. The same IEEE
// operations in the same order whatever the vector width, so the AVX build gives bit-identical results.

#if defined(__GNUC__)
#define HARMLESS_INLINE inline __attribute__ ((always_inline))
#else
#define HARMLESS_INLINE inline
#endif

HARMLESS_INLINE void bankKernelBody (double* osc0, double* osc1, const double* oscC, const double* T, const double* A, int top,
                                     int m, const double* kp, double* acc) noexcept
{
    for (int g = 0; g < top; g += 8)
    {
        double x[8], y[8], c[8], t[8], d[8];
#pragma GCC unroll 8
        for (int l = 0; l < 8; ++l)
        {
            const auto i = (std::size_t) (g + l);
            x[l] = osc0[i], y[l] = osc1[i], c[l] = oscC[i];
            t[l] = T[i];
            d[l] = A[i] - T[i];
        }
        for (int j = 0; j < m; ++j)
        {
            const auto k = kp[j];
            auto* a = acc + 8 * j;
#pragma GCC unroll 8
            for (int l = 0; l < 8; ++l)
            {
                const auto s = x[l];
                a[l] += (t[l] + d[l] * k) * s;
                x[l] = c[l] * s - y[l];
                y[l] = s;
            }
        }
#pragma GCC unroll 8
        for (int l = 0; l < 8; ++l)
            osc0[(std::size_t) (g + l)] = x[l], osc1[(std::size_t) (g + l)] = y[l];
    }
}

void bankKernelPlain (double* osc0, double* osc1, const double* oscC, const double* T, const double* A, int top, int m,
                      const double* kp, double* acc) noexcept
{
    bankKernelBody (osc0, osc1, oscC, T, A, top, m, kp, acc);
}

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__)) && ! defined(HARMLESS_NO_AVX)
#define HARMLESS_AVX 1
// 4-wide doubles where the CPU and the OS support AVX (no FMA: contraction is off, so the rounding is unchanged)
__attribute__ ((target ("avx"))) void bankKernelAvx (double* osc0, double* osc1, const double* oscC, const double* T,
                                                     const double* A, int top, int m, const double* kp, double* acc) noexcept
{
    bankKernelBody (osc0, osc1, oscC, T, A, top, m, kp, acc);
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

void bankKernel (double* osc0, double* osc1, const double* oscC, const double* T, const double* A, int top, int m,
                 const double* kp, double* acc) noexcept
{
#ifdef HARMLESS_AVX
    static const bool avx = cpuHasAvx();
    if (avx)
    {
        bankKernelAvx (osc0, osc1, oscC, T, A, top, m, kp, acc);
        return;
    }
#endif
    bankKernelPlain (osc0, osc1, oscC, T, A, top, m, kp, acc);
}

// ---------------------------------------------------------------------------------------------------------------
// One voice record: a full independent copy of the voice (bank, envelopes, filter state, LFO) per unison sub-voice

struct Record
{
    // per-position state of the bank
    std::array<double, bankSize> a {}, T {}, bl {}, phase {}, freq {};
    std::array<double, bankSize> osc0 {}, osc1 {}, oscC {}, oscS {}, oscF {}; // running sine recurrences, their steps
    std::array<float, bankSize> hz {}, l2 {}, pluckLevel {}, pluckDecay {};

    bool used = false, dead = false, first = true, knobInit = false, motionInit = false, pluckOn = false, pluckDirty = true;
    bool lrInit = false;
    int note = -1, cnt = 8, ctrlLeft = 0, feCurve = 0, glideN = 0, pluckInit = 0, oscTop = 0, oscAge = 0;
    long long pluckCnt = 0, trem = 0;
    double vel = 0, finalPitch = 0, finalVol = 1, finalPan = 0, volFactor = 1, notePan = 0;
    double uPhase = 0.5, uPan = 0, uDet = 0, uVar = 1, uLfo = 0;
    AmpEnv env;
    double prevAc = 0, feT = 0, feInc = 0, knobS = 0, motion = 0, lfoPhase = 0, lfoR = 0, vibR = 0, L = 0, U = 0.5, E = 0;
    std::array<float, 6> lvl {}, glideSlope {}; // pan, vol, pitch, fcut, fres, velocity offsets (§1.15)
    double glideC = 0, portaVel = 1, finalFcut = 0, finalFres = 0, nacc = 0, rnd = 0.5;
    std::uint32_t seed = 0, nseed = 0;
    double lf0 = 0, f0 = 0, pos = 0, ph70 = 0, phPh = 0;
    std::array<double, 2> lrPrev {};
};

struct Note
{
    bool used = false, ghost = false, released = false, stamped = false;
    int key = 0;
    double vel = 0, fvol = 0, fpan = 0, finalPitch = 0, volFactor = 1, notePan = 0;
    long long start = 0, stamp = 0;
    std::array<int, 9> records {};
    int count = 0;
    HostSlide slide; // a slide note moving the note's Final.Pitch (issue #14)
};

// pseudo-random draws for the per-record seeds and the LFO "voice random" (FL seeds its generator from the clock, so
// its draws can't be reproduced; LYNX uses a fixed-seed PCG-style generator, so every render is deterministic)
struct Random
{
    std::uint32_t state = 0x6A09E667u;

    std::uint32_t next() noexcept
    {
        state = state * 747796405u + 2891336453u;
        const auto w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
        return (w >> 22) ^ w;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The engine: Gen_Render's voice mix (§1.1), notes, mono/porta/legato (§1.15), stealing (§1.18)

class Engine
{
public:
    static constexpr int maxRecords = 96;  // ≈ 50 KB each, allocated with the instrument
    static constexpr int maxNotes = 256;
    static constexpr int maxHostBlock = 512;
    static constexpr int maxControlBlock = 1024;

    Engine()
    {
        (void) tables();
        records.resize (maxRecords);
        notes.resize (maxNotes);
        order.reserve (maxNotes);
        reset();
    }

    Patch P;
    Globals G;
    Random random;
    hlfx::Rack rack;          // the effects rack on the mix (REA_rodada36)
    long long N = 0;          // plugin sample counter
    double tremPrev = 0.0;
    int slot = -1;            // current note of the channel
    bool slotLevels = false;
    std::array<float, 6> slotLv {};
    int maxPoly = 0;          // 0 = derived from the record capacity

    void reset() noexcept
    {
        for (auto& r : records)
            r.used = false;
        freeCount = maxRecords;
        for (int i = 0; i < maxRecords; ++i)
            freeList[(std::size_t) i] = maxRecords - 1 - i;
        for (auto& n : notes)
            n.used = false;
        order.clear();
        G = {};
        random = {};
        N = 0;
        tremPrev = 0.0;
        slot = -1;
        slotLevels = false;
    }

    bool idle() const noexcept
    {
        return freeCount == maxRecords; // (a scan of the 50 KB records would miss the cache on every call)
    }

    // velocity 0…1 (FL velocity / 128); volFactor: Final.Vol / channel volume; pan: note pan; pitchOffset: cents
    int noteOn (int key, double velocity, double volFactor, double channelVol, double notePan, double channelPan,
                double pitchOffset) noexcept
    {
        const auto vel = std::min (velocity, 1.0);
        const auto fvol = channelVol * volFactor, fpan = notePan + channelPan;
        const auto finalPitch = f32 (100.0 * key - 6000.0 + pitchOffset);

        // polyphony: steal the record with the smallest stamp once the limit is reached (released first) — §1.18
        const auto limit = maxPoly > 0 ? maxPoly : std::max (1, maxRecords / P.unison - 1);
        {
            int candidates = 0, victim = -1;
            for (auto i : order)
            {
                const auto& nt = notes[(std::size_t) i];
                if (nt.ghost || ! nt.stamped || nt.count == 0)
                    continue;
                ++candidates;
                if (victim < 0 || nt.stamp < notes[(std::size_t) victim].stamp)
                    victim = i;
            }
            if (candidates >= limit && victim >= 0)
                steal (victim);
        }

        const auto index = allocateNote();
        auto& nt = notes[(std::size_t) index];
        nt = {};
        nt.used = true;
        nt.key = key, nt.vel = vel, nt.fvol = fvol, nt.fpan = fpan, nt.finalPitch = finalPitch;
        nt.volFactor = volFactor, nt.notePan = notePan;
        nt.start = N;

        if (P.p[44] > 0 && slot >= 0)
        {
            // legato: the sounding records move to the new note, the old note becomes a held ghost
            auto& old = notes[(std::size_t) slot];
            nt.records = old.records, nt.count = old.count;
            old.count = 0, old.ghost = true;
            for (int k = 0; k < nt.count; ++k)
            {
                auto& v = records[(std::size_t) nt.records[(std::size_t) k]];
                v.note = index;
                v.finalPitch = nt.finalPitch, v.volFactor = nt.volFactor, v.notePan = nt.notePan, v.vel = nt.vel;
                v.finalVol = nt.fvol, v.finalPan = nt.fpan;
                v.portaVel = P.p[46] <= 0 ? 1.0 : f32 (std::pow (f32 (1.0 - nt.vel * 0.85), 2.0));
            }
            order.push_back (index);
            const auto oldLv = levels (old);
            slot = index;
            porta (nt, oldLv, false, false);
            slotLv = levels (nt), slotLevels = true;
            return index;
        }

        // the records of a new note (one per unison sub-voice)
        ensureFreeRecords (P.unison);
        double sharedRnd = 0.0;
        for (int k = 0; k < P.unison; ++k)
        {
            const auto r = allocateRecord();
            if (r < 0)
                break;
            const auto seed = random.next();
            auto rnd = (double) random.next() / 4294967296.0;
            if (k == 0)
                sharedRnd = rnd;
            if (P.lfoGlobal)
                rnd = sharedRnd; // one shared draw when the LFO is global
            startRecord (records[(std::size_t) r], index, k, seed, rnd);
            nt.records[(std::size_t) nt.count++] = r;
        }
        nt.stamped = true, nt.stamp = N;
        order.push_back (index);
        if (P.p[43] > 0 && slotLevels)
            porta (nt, slotLv, true, true);
        slot = index;
        slotLv = levels (nt), slotLevels = true;
        return index;
    }

    void noteOff (int index) noexcept
    {
        auto& nt = notes[(std::size_t) index];
        nt.released = true;
        if (nt.ghost)
        {
            removeNote (index);
            return;
        }
        if (slot == index)
        {
            slot = -1;
            int g = -1;
            for (auto i : order)
                if (notes[(std::size_t) i].ghost && (g < 0 || notes[(std::size_t) i].start > notes[(std::size_t) g].start))
                    g = i;
            if (g >= 0)
            {
                // the sound goes back to the most recent held note
                auto& gh = notes[(std::size_t) g];
                gh.records = nt.records, gh.count = nt.count;
                nt.count = 0;
                gh.ghost = false;
                for (int k = 0; k < gh.count; ++k)
                {
                    auto& v = records[(std::size_t) gh.records[(std::size_t) k]];
                    v.note = g;
                    v.finalPitch = gh.finalPitch, v.volFactor = gh.volFactor, v.notePan = gh.notePan, v.vel = gh.vel;
                    v.finalVol = gh.fvol, v.finalPan = gh.fpan;
                    // the released record's factor (release velocity, host event 4) is handed over
                    v.portaVel = P.p[46] <= 0 ? 1.0 : f32 (std::pow (f32 (1.0 - std::min (1.0, P.hostEv4) * 0.85), 2.0));
                }
                slot = g;
                porta (gh, levels (nt), false, false);
                slotLv = levels (gh), slotLevels = true;
                removeNote (index);
                return;
            }
        }
        for (int k = 0; k < nt.count; ++k)
            releaseRecord (records[(std::size_t) nt.records[(std::size_t) k]]);
        if (nt.stamped)
            nt.stamp = N + std::numeric_limits<long long>::min();
        if (nt.count == 0)
            removeNote (index);
    }

    // every note fades out (30 ms) and is forgotten (choke / stop)
    void allNotesOff() noexcept
    {
        for (std::size_t i = 0; i < order.size();)
        {
            auto& nt = notes[(std::size_t) order[i]];
            for (int k = 0; k < nt.count; ++k)
                if (auto& v = records[(std::size_t) nt.records[(std::size_t) k]]; v.env.state < 9)
                    v.env.kill();
            nt.released = true, nt.stamped = false;
            if (nt.ghost || nt.count == 0)
            {
                removeNote (order[i]);
                continue;
            }
            ++i;
        }
        slot = -1;
    }

    // first note of `key` still held by the host (ghosts included), or −1
    int findHeld (int key) const noexcept
    {
        for (auto i : order)
            if (const auto& nt = notes[(std::size_t) i]; nt.key == key && ! nt.released)
                return i;
        return -1;
    }

    // adds n (≤ maxHostBlock) samples; channelVol / channelPan: FL's chan.Vol and chan.Pan this block
    // a slide note (issue #14): the sounding, unreleased notes glide to `to` (Final.Pitch, cents) over `samples`
    void slideTo (double to, float samples) noexcept
    {
        for (const auto ni : order)
            if (auto& nt = notes[(std::size_t) ni]; ! nt.released && ! nt.ghost && nt.count > 0)
                nt.slide.start ((float) (to - (nt.finalPitch - (double) nt.slide.cents)), samples);
    }

    void render (float* left, float* right, int n, double channelVol, double channelPan) noexcept
    {
        std::fill (mixL.begin(), mixL.begin() + n, 0.0);
        std::fill (mixR.begin(), mixR.begin() + n, 0.0);
        const auto live = freeCount < maxRecords; // dead records were returned at the end of the previous block
        if (! live)
            tremPrev = P.trem;
        bool trem = false;
        if (P.trem != 0.0 && live)
        {
            double whole = 0.0;
            auto ph = f32 (std::modf ((double) (float) (double) N * P.tremInc, &whole) * twoPiF);
            if (ph >= twoPiF)
                ph = f32 (ph - twoPiF);
            const auto depth = ramp (tremPrev, P.trem, n);
            tremPrev = depth.end;
            const auto w = twoPi * P.tremInc;
            for (int j = 0; j < n; ++j)
            {
                const auto ang = w * j + ph;
                const auto dep = depth.start + depth.step * j;
                tremL[(std::size_t) j] = f32 (1.0 + dep * (std::fabs (1.25 * std::cos (ang)) - 1.0));
                tremR[(std::size_t) j] = f32 (1.0 + dep * (std::fabs (1.25 * std::sin (ang)) - 1.0));
            }
            trem = true;
        }
        for (std::size_t oi = 0; oi < order.size(); ++oi)
        {
            const auto ni = order[oi];
            auto& nt = notes[(std::size_t) ni];
            if (nt.slide.moving()) // a slide note moves Final.Pitch (issue #14)
            {
                const auto was = nt.slide.cents;
                const auto d = (double) (nt.slide.advance ((float) n) - was);
                nt.finalPitch += d;
                for (int k = 0; k < nt.count; ++k)
                    records[(std::size_t) nt.records[(std::size_t) k]].finalPitch += d;
            }
            for (int k = 0; k < nt.count; ++k)
            {
                auto& v = records[(std::size_t) nt.records[(std::size_t) k]];
                if (v.dead)
                    continue;
                v.finalVol = channelVol * v.volFactor; // FL recomputes FinalLevels when the channel level changes
                v.finalPan = v.notePan + channelPan;
                renderRecord (v, mono.data(), n);
                double gl = 0.0, gr = 0.0;
                lrGains (v, gl, gr);
                if (! v.lrInit)
                    v.lrPrev = { gl, gr }, v.lrInit = true;
                const auto rl = ramp (v.lrPrev[0], gl, n), rr = ramp (v.lrPrev[1], gr, n);
                v.lrPrev = { rl.end, rr.end };
                for (int j = 0; j < n; ++j)
                {
                    auto l = mono[(std::size_t) j] * (rl.start + rl.step * j), r = mono[(std::size_t) j] * (rr.start + rr.step * j);
                    if (trem)
                    {
                        if (v.trem < 1)
                            l = l * tremL[(std::size_t) j], r = r * tremR[(std::size_t) j];
                        else
                        {
                            const auto fade = std::min (1.0 - (double) v.trem * P.tremS + j * P.tremS, 1.0);
                            l = l * ((1.0 - fade) + fade * tremL[(std::size_t) j]);
                            r = r * ((1.0 - fade) + fade * tremR[(std::size_t) j]);
                        }
                    }
                    mixL[(std::size_t) j] += l;
                    mixR[(std::size_t) j] += r;
                }
                if (trem && v.trem >= 1)
                    v.trem -= n;
                // end of a voice: below 1e-4 after the attack and not the current note → 30 ms fade (§1.13)
                if (v.env.state == 10)
                    v.dead = true;
                else if (v.env.state > 2 && v.env.state < 9 && ni != slot && v.env.lastOut < 1e-4)
                    v.env.kill();
            }
        }
        if (rack.enabled())
        {
            // the DLL mixes the voices into a float buffer and runs the rack on it in place
            for (int j = 0; j < n; ++j)
                rackL[(std::size_t) j] = (float) mixL[(std::size_t) j], rackR[(std::size_t) j] = (float) mixR[(std::size_t) j];
            rack.process (rackL.data(), rackR.data(), n, live);
            for (int j = 0; j < n; ++j)
            {
                left[j] += rackL[(std::size_t) j];
                right[j] += rackR[(std::size_t) j];
            }
        }
        else
            for (int j = 0; j < n; ++j)
            {
                left[j] += (float) mixL[(std::size_t) j];
                right[j] += (float) mixR[(std::size_t) j];
            }
        G.advance (P, n);
        N += n;
        collect();
    }

private:
    std::vector<Record> records;
    std::array<int, maxRecords> freeList {}; // free record indices (a stack)
    int freeCount = 0;
    std::vector<Note> notes;
    std::vector<int> order; // notes in creation order (the voice mix order)
    std::array<double, maxHostBlock> mono {}, mixL {}, mixR {}, tremL {}, tremR {}, envBuf {};
    std::array<float, maxHostBlock> rackL {}, rackR {};
    std::array<double, maxControlBlock + 1> kpow {};
    std::array<double, 8 * maxControlBlock> laneSums {};
    std::array<double, bankSize> amps {}; // the target amplitudes being built in a control block
    double kpowFor = -1.0;

    static std::array<float, 6> levels (const Note& nt) noexcept
    {
        return { (float) nt.fpan, (float) nt.fvol, (float) nt.finalPitch, 0.0f, 0.0f, (float) nt.vel };
    }

    // FUN_00612db0 + the ramped multiply: value start + step·j, the step limited to maxStep per sample
    struct Ramp
    {
        double start, step, end;
    };

    Ramp ramp (double prev, double target, int n) const noexcept
    {
        const auto d = f32 (target - prev);
        if (d == 0.0)
            return { prev, 0.0, prev };
        auto step = f32 (d / n);
        auto end = target;
        if (std::fabs (step) > P.maxStep)
        {
            step = std::copysign (P.maxStep, step);
            end = f32 (prev + step * n);
        }
        return { prev, step, end };
    }

    void lrGains (const Record& v, double& gl, double& gr) const noexcept
    {
        const auto vol = f32 (P.masterVol * f32 (f32 (v.finalVol + (double) v.lvl[1]) * P.uniGain));
        const auto pan = std::clamp (f32 (f32 (v.finalPan + (double) v.lvl[0]) + v.uPan), -1.0, 1.0);
        const auto t = (1.0 + pan) * 0.7853981633974483;
        gl = f32 (vol * std::cos (t));
        gr = f32 (vol * std::sin (t));
    }

    int allocateNote() noexcept
    {
        for (int i = 0; i < maxNotes; ++i)
            if (! notes[(std::size_t) i].used)
                return i;
        // full: forget the oldest note (its records stop)
        const auto oldest = order.front();
        auto& nt = notes[(std::size_t) oldest];
        for (int k = 0; k < nt.count; ++k)
            freeRecord (nt.records[(std::size_t) k]);
        nt.count = 0;
        if (slot == oldest)
            slot = -1;
        removeNote (oldest);
        return oldest;
    }

    void removeNote (int index) noexcept
    {
        notes[(std::size_t) index].used = false;
        order.erase (std::remove (order.begin(), order.end(), index), order.end());
    }

    int allocateRecord() noexcept
    {
        if (freeCount == 0)
            return -1;
        const auto r = freeList[(std::size_t) --freeCount];
        records[(std::size_t) r].used = true;
        return r;
    }

    void freeRecord (int r) noexcept
    {
        if (! records[(std::size_t) r].used)
            return;
        records[(std::size_t) r].used = false;
        freeList[(std::size_t) freeCount++] = r;
    }

    // capacity fallback (LYNX's own, outside FL's behaviour): stop the oldest records at once
    void ensureFreeRecords (int needed) noexcept
    {
        while (freeCount < needed)
        {
            int victim = -1;
            long long best = std::numeric_limits<long long>::max();
            for (auto i : order)
            {
                const auto& nt = notes[(std::size_t) i];
                if (nt.count == 0)
                    continue;
                const auto key = nt.stamped ? nt.stamp : std::numeric_limits<long long>::min() + nt.start; // fading first
                if (victim < 0 || key < best)
                    victim = i, best = key;
            }
            if (victim < 0)
                return;
            auto& nt = notes[(std::size_t) victim];
            for (int k = 0; k < nt.count; ++k)
                freeRecord (nt.records[(std::size_t) k]);
            nt.count = 0;
            nt.stamped = false;
            if (nt.released && victim != slot)
                removeNote (victim);
        }
    }

    // FUN_00783ac0: legato hand-over if it is the current note with held notes, then a 30 ms fade of every sub-voice
    void steal (int index) noexcept
    {
        auto& nt = notes[(std::size_t) index];
        if (slot == index && std::any_of (order.begin(), order.end(), [this] (int i) { return notes[(std::size_t) i].ghost; }))
        {
            noteOff (index);
            return;
        }
        for (int k = 0; k < nt.count; ++k)
            if (auto& v = records[(std::size_t) nt.records[(std::size_t) k]]; v.env.state < 9)
                v.env.kill();
        nt.stamped = false;
    }

    // dead records go back to the pool; notes without records the host has released are forgotten
    void collect() noexcept
    {
        for (std::size_t oi = 0; oi < order.size();)
        {
            const auto ni = order[oi];
            auto& nt = notes[(std::size_t) ni];
            int kept = 0;
            for (int k = 0; k < nt.count; ++k)
            {
                const auto r = nt.records[(std::size_t) k];
                if (records[(std::size_t) r].dead)
                    freeRecord (r);
                else
                    nt.records[(std::size_t) kept++] = r;
            }
            nt.count = kept;
            if (kept == 0 && ! nt.ghost && nt.released && ni != slot)
            {
                removeNote (ni);
                continue;
            }
            ++oi;
        }
    }

    void porta (Note& nt, const std::array<float, 6>& oldLv, bool wrap, bool zeroPanVolVel) noexcept
    {
        const auto newLv = levels (nt);
        for (int k = 0; k < nt.count; ++k)
        {
            auto& v = records[(std::size_t) nt.records[(std::size_t) k]];
            auto shift = [&] (int j) { v.lvl[(std::size_t) j] = (float) ((float) (v.lvl[(std::size_t) j] + oldLv[(std::size_t) j]) - newLv[(std::size_t) j]); };
            if (zeroPanVolVel)
                v.lvl[0] = 0.0f, v.lvl[1] = 0.0f, v.lvl[5] = 0.0f;
            else
                shift (0), shift (1), shift (5);
            shift (2), shift (3), shift (4);
            if (wrap)
            {
                const auto sg = v.lvl[2] > 0.0f ? 1.0 : v.lvl[2] < 0.0f ? -1.0 : 0.0;
                while (std::fabs ((double) v.lvl[2]) > 1250.0)
                    v.lvl[2] = (float) ((double) v.lvl[2] - sg * 1200.0);
            }
            const auto t = f32 (P.porta * v.portaVel);
            if (t > 0.0)
            {
                v.glideN = -1;
                v.glideC = f32 (std::exp (ln001 / t));
            }
            else
            {
                const auto nb = 1 - (int) rint (t);
                v.glideN = nb;
                const auto k2 = (float) f32 (-1.0 / nb);
                for (int j = 0; j < 6; ++j)
                    v.glideSlope[(std::size_t) j] = v.lvl[(std::size_t) j] * k2;
            }
        }
    }

    void startRecord (Record& v, int note, int k, std::uint32_t seed, double rnd) noexcept
    {
        const auto& nt = notes[(std::size_t) note];
        v.used = true, v.dead = false;
        v.note = note;
        v.vel = nt.vel;
        v.finalPitch = nt.finalPitch, v.finalVol = nt.fvol, v.finalPan = nt.fpan;
        v.volFactor = nt.volFactor, v.notePan = nt.notePan;
        const auto n = P.unison;
        if (n == 1)
            v.uPhase = 0.5, v.uPan = 0.0, v.uDet = 0.0, v.uVar = 1.0, v.uLfo = 0.0;
        else
        {
            auto ph = f32 (f32 ((double) k / n) * P.uniPhase);
            const auto u = f32 (f32 ((double) k / (n - 1)) * 2.0 - 1.0);
            v.uPan = f32 (f32 (u * 0.0078125) * (double) P.p[66]);
            auto s = std::fabs (u);
            if (k & 1)
                s = -s;
            if ((n & 1) && n < 2 * k)
                s = -s;
            const auto d = f32 (s * (double) unisonTable[std::clamp (P.p[70], 0, 2)][k]);
            v.uDet = f32 (d * P.uniS);
            if (P.p[70] == 2)
            {
                const auto x = P.uniPhase, x4 = (x * x) * (x * x);
                ph = f32 (std::exp (v.uDet * 0.00057762265046662107) * 100000.0 * x4 * x4);
            }
            v.uPhase = f32 (ph + 0.5 - rint (ph));
            v.uVar = f32 (std::exp ((std::fabs (s) - 1.0) * P.p[69] * (double) 0.0078125f * 3.0 * ln2));
            v.uLfo = f32 (f32 (s * (double) P.p[69]) * (double) 0.00390625f);
        }
        v.env.start (P, v.vel, v.uVar);
        v.cnt = 8;
        v.a.fill (0.0), v.T.fill (0.0), v.bl.fill (1.0), v.phase.fill (0.0), v.freq.fill (0.0);
        v.hz.fill (0.0f), v.l2.fill (0.0f);
        v.oscTop = 0, v.oscAge = 0;
        v.ctrlLeft = 0, v.first = true, v.prevAc = f32 (2147483648.0);
        v.feT = 0.0, v.feInc = f32 (P.feAttack * P.block), v.feCurve = 0;
        v.knobInit = false, v.motionInit = false;
        v.lfoPhase = 0.0, v.lfoR = P.lfoAttack, v.vibR = P.vibR;
        v.L = 0.0, v.U = 0.5, v.E = 0.0;
        v.lvl.fill (0.0f), v.glideSlope.fill (0.0f);
        v.glideN = 0, v.glideC = 0.0;
        v.portaVel = P.p[46] <= 0 ? 1.0 : f32 (std::pow (f32 (1.0 - v.vel * 0.85), 2.0));
        v.pluckOn = P.pluckT >= 0.0 && P.p[35] == 0;
        v.pluckCnt = std::min<long long> (P.pluckStart, -(long long) rint (P.A / P.block));
        v.pluckLevel.fill (0.0f), v.pluckLevel[1] = 1.0f;
        v.pluckDecay.fill (1.0f), v.pluckInit = 0, v.pluckDirty = true;
        v.trem = P.tremM;
        v.lrInit = false;
        v.finalFcut = 0.0, v.finalFres = 0.0; // LYNX notes carry no Mod X / Mod Y
        v.nacc = 0.0;
        v.seed = seed, v.nseed = seed, v.rnd = rnd;
        v.lf0 = v.f0 = v.pos = v.ph70 = v.phPh = 0.0;
    }

    void releaseRecord (Record& v) noexcept
    {
        v.env.release();
        if (P.pluckT >= 0.0 && (P.p[35] == 0 || v.env.state > 5))
            v.pluckOn = true, v.pluckCnt = std::max<long long> (v.pluckCnt, 0);
        if (v.feInc > 0.0)
        {
            // a rising filter envelope jumps to the decay branch keeping its value (§1.7)
            const auto E = Patch::curveValue (v.feCurve == 0 ? P.curveAttack : P.curveDecay, v.feT);
            const auto [q, a, b] = P.curveDecay;
            (void) a;
            double t = E;
            if (q > 0.0)
                t = 1.0 - std::log ((1.0 - E) * std::fabs (q) + 1.0) / b;
            else if (q < 0.0)
                t = std::log (E * std::fabs (q) + 1.0) / b;
            v.feT = f32 (t), v.feInc = f32 (P.feDecay * P.block), v.feCurve = 1;
        }
    }

    // LFO value L (bipolar) and U (unipolar) — §1.12
    void lfoValue (Record& v, double localAc) const noexcept
    {
        const auto sh = P.lfoShape;
        const auto r2 = f32 (v.lfoR * v.lfoR);
        double L0 = 0.0;
        switch (sh)
        {
            case 9: v.L = f32 (v.E * r2), v.U = v.L; return;
            case 10: v.L = f32 (f32 (f32 (v.knobS - P.lo) * f32 (1.0 / f32 (P.hi - P.lo))) * r2), v.U = v.L; return;
            case 11:
            {
                const auto U = f32 (f32 (f32 (v.rnd) * v.lfoR) * v.lfoR);
                v.L = f32 (U * 2.0 - 1.0), v.U = U;
                return;
            }
            case 12: v.L = f32 (f32 (v.vel + (double) v.lvl[5]) * r2), v.U = v.L; return;
            case 13:
            {
                auto x = f32 ((localAc + P.lfoP13) * 0.0002777777777777778);
                x = std::clamp (x, -1.0, 1.0);
                v.L = f32 (x * r2), v.U = f32 ((v.L + 1.0) * 0.5);
                return;
            }
            default: break;
        }
        const auto d = v.lfoPhase + v.uLfo;
        switch (sh)
        {
            case 5:
            {
                const auto q = (long long) rint (d * 3.0 - 0.5);
                L0 = (double) (q % 3) - 1.0; // C remainder: −3…−1 for negative phases
                break;
            }
            case 1:
            {
                const auto x = (float) f32 ((d - rint (d)) * twoPiF);
                float sum = 0.0f;
                const auto& w = tables().smoothSaw;
                for (int k = 0; k < 12; ++k)
                    sum = sum + w[(std::size_t) k] * (float) std::sin ((double) ((float) (k + 1) * x));
                L0 = (double) sum;
                break;
            }
            case 2:
            {
                const auto s = f32 (std::sin (((d - rint (d)) + 0.5) * twoPiF));
                auto q = f32 (1.0 - std::fabs (s));
                q = q * q * q;
                L0 = f32 (sgn (s) * (1.0 - q * q));
                break;
            }
            case 3: { const auto e = d - 0.5; L0 = f32 ((e - rint (e)) * 2.0); break; }
            case 4: L0 = (double) ((toInt64 (rint (d * 2.0 - 0.5)) & 1) * 2 - 1); break;
            case 6: { const auto e = d + 0.25; L0 = f32 (std::fabs ((e - rint (e - 0.5)) * 4.0 - 2.0) - 1.0); break; }
            case 7:
            {
                const auto e = d - 0.5;
                const auto x = f32 ((e - rint (e)) + 0.5);
                L0 = f32 (x * x * x * 2.0 - 1.0);
                break;
            }
            case 8:
            {
                const auto z = (d + 0.5) * twoPiF;
                L0 = f32 ((std::sin (z) + std::sin ((z + 7.0) * 0.2)) * 0.5);
                break;
            }
            default: L0 = f32 (std::sin (((d - rint (d)) + 0.5) * twoPiF)); break;
        }
        v.L = f32 (L0 * r2);
        v.U = f32 ((v.L + 1.0) * 0.5);
    }

    void harmonizer (Record& v, int N) noexcept
    {
        auto w = P.p[82] <= 0 ? P.hzW : f32 (P.hzW * f32 (v.vel + (double) v.lvl[5]));
        if (P.hzWl > 0.0)
            w = f32 (w + f32 (v.U * std::min (P.hzWl, f32 (1.0 - w))));
        else if (P.hzWl < 0.0)
            w = f32 (w + f32 (v.U * std::max (P.hzWl, -w)));
        if (w <= 0.0)
            return;
        const auto [st0, add, mul, Dd] = P.hzType;
        const auto s = (float) f32 (1.0 / f32 (f32 (w * (double) Dd) + 1.0));
        auto c = (float) (1.0f - s);
        float total = 1.0f;
        int step = 2;
        const auto n = N - 4;
        std::array<float, bankSize> a {}, src {};
        for (int i = 0; i < n; ++i)
            a[(std::size_t) i] = src[(std::size_t) i] = (float) amps[(std::size_t) i + 4];
        while (c > 0.0f)
        {
            const auto g = (float) f32 (std::min (1.0, f32 ((double) ((c * c) * c) * P.hzStrength)) * P.hzMix2);
            total = total + g;
            const auto start = step + st0;
            const auto count = (n - start - 1) / step + 1;
            if (count > 0 && start < n)
                for (int j = 0; j < count; ++j)
                    a[(std::size_t) (start + j * step)] = a[(std::size_t) (start + j * step)] + src[(std::size_t) j] * g;
            step = step * mul + add;
            c = c - s;
        }
        const auto inv = (float) f32 (1.0 / (double) total);
        for (int i = 0; i < n; ++i)
            amps[(std::size_t) i + 4] = (double) (a[(std::size_t) i] * inv);
    }

    // band-limit gains of positions [from, to) from their frequencies (§1.3)
    void bandLimit (Record& v, int from, int to) const noexcept
    {
        const auto fs = P.fs;
        const auto lo1 = 32.0 / fs, lo0 = 16.0 / fs, hi0 = 16000.0 / fs, hi1 = 17000.0 / fs;
        for (int i = from; i < to; ++i)
        {
            const auto f = f32 (v.freq[(std::size_t) i]);
            double b = 1.0;
            if (f > hi0)
            {
                const auto r = 1.0 - (f - hi0) * (1.0 / (hi1 - hi0));
                b = r * r;
            }
            if (f > hi1)
                b = 0.0;
            if (f < lo1)
            {
                const auto r = 1.0 - (lo1 - f) * (1.0 / (lo1 - lo0));
                b = r * r;
            }
            if (f < lo0)
                b = 0.0;
            v.bl[(std::size_t) i] = b;
        }
    }

    // positions [0, limit) can have a non-zero target: at quality 0/1 the band limit zeroes the rest (the first 8
    // always count: position 0's gain is set later and the low-harmonics protection reads 0…7)
    int activeLimit (const Record& v, int N, bool freqPhaser) const noexcept
    {
        if (P.quality >= 2 || freqPhaser)
            return N;
        int last = N;
        while (last > 8 && v.bl[(std::size_t) last - 1] == 0.0)
            --last;
        return last;
    }

    // the target amplitudes of one control block, in the order of §1.4
    void controlBlock (Record& v, double vibPh, double knobTarget) noexcept
    {
        const auto& tab = tables();
        const auto fs = P.fs;
        if (v.glideN != 0)
        {
            if (v.glideN < 0)
            {
                const auto c = (float) v.glideC;
                for (auto& x : v.lvl)
                    x = x * c;
            }
            else
            {
                for (int j = 0; j < 6; ++j)
                    v.lvl[(std::size_t) j] = v.lvl[(std::size_t) j] + v.glideSlope[(std::size_t) j];
                --v.glideN;
            }
        }
        v.feT = f32 (v.feT + v.feInc);
        if (v.feT >= 1.0 && v.feInc > 0.0)
        {
            v.feT = f32 (1.0 - ((1.0 - v.feT) / v.feInc) * P.feDecay * P.block);
            v.feInc = f32 (P.feDecay * P.block);
            v.feCurve = 1;
        }
        if (v.feT < 0.0)
            v.feT = 0.0;
        const auto E = v.E = Patch::curveValue (v.feCurve == 0 ? P.curveAttack : P.curveDecay, v.feT);
        v.knobS = f32 (knobTarget + (v.knobS - knobTarget) * P.knobC);

        // pitch (§1.5)
        auto localAc = f32 (f32 (v.finalPitch + (double) P.pitchOffset) + (double) v.lvl[2]);
        if (P.p[41] != 0)
            localAc = f32 (localAc + std::sin (f32 (vibPh * twoPiF)) * P.vib * v.vibR * v.vibR);
        lfoValue (v, localAc);
        const auto det = P.p[99] <= 0 ? v.uDet : f32 (v.uDet * f32 (v.vel + (double) v.lvl[5]));
        localAc = f32 (f32 (localAc + f32 (P.lfoPitch * v.L)) + det);
        const auto changed = localAc != v.prevAc;
        const auto oldCnt = v.cnt;
        // with the band limit at quality 0/1, positions whose gain is 0 get a target of exactly 0 whatever the stages
        // compute: only the positions below `lim` go through the pipeline (the pluck and Noise generator states still
        // advance for all N)
        const auto freqPhaser = P.p[71] != 0 && P.phType == 9;
        int lim = activeLimit (v, v.cnt, freqPhaser);
        if (changed)
        {
            const auto lf0 = f32 (log2Of440 + (f32 (f32 (localAc * (double) 0.01f) + 60.0) - 57.0) * 0.08333333333333333);
            const auto f0 = f32 (std::exp (lf0 * ln2));
            v.lf0 = lf0, v.f0 = f0;
            auto x = f32 (f32 (17000.0 / f0) / P.D);
            if (P.quality > 0)
                x = f32 (x * 1.5);
            auto nh = P.quality < 2 ? std::max ((int) std::min (rint (x), 1.0e9), 1) + 4 : bankSize;
            nh = std::min (nh, bankSize);
            const auto c = (nh + 7) & ~7;
            if (c > v.cnt)
                v.cnt = c;
            const auto N = v.cnt;
            const auto scale = f0 * (1.0 / fs);
            for (int i = 0; i < N; ++i)
                v.freq[(std::size_t) i] = P.ratio[(std::size_t) i] * scale;
            if (P.grit != 0.0)
            {
                const auto gs = f32 (f0 * P.grit) * (1.0 / fs);
                for (int i = 0; i < N; ++i)
                    v.freq[(std::size_t) i] += P.gritTable[(std::size_t) i] * gs;
            }
            if (! freqPhaser)
                bandLimit (v, 1, N);
            lim = activeLimit (v, N, freqPhaser);
            const auto fsF = (float) fs, invLn2 = (float) (1.0 / ln2);
            for (int i = 0; i < lim; ++i)
            {
                v.hz[(std::size_t) i] = (float) v.freq[(std::size_t) i] * fsF;
                // ln in float: computed as the correctly rounded float of the double log (§1.3: FL uses IPP Ln_32f)
                v.l2[(std::size_t) i] = (float) std::log ((double) v.hz[(std::size_t) i]) * invLn2;
            }
        }
        const auto N = v.cnt;

        // 1. colour × EQ, 2. mask (skipped when the pluck uses the mask)
        auto m = P.mix;
        if (P.mixLfo > 0.0)
            m = f32 (m + f32 (v.U * std::min (P.mixLfo, f32 (1.0 - m))));
        else if (P.mixLfo < 0.0)
            m = f32 (m + f32 (v.U * std::max (P.mixLfo, -m)));
        for (int i = 0; i < lim; ++i)
            amps[(std::size_t) i] = P.a570[(std::size_t) i];
        const auto oneMinusM = f32 (1.0 - m);
        if (m > 0.0 && P.p[34] == 0)
            for (int i = 4; i < lim; ++i)
                amps[(std::size_t) i] = f32 (P.a570[(std::size_t) i] * (P.mask[(std::size_t) ((i - 4) % 12)] * m + oneMinusM));
        const auto harmonize = P.p[79] != 0;
        if (P.p[85] == 0 && harmonize)
            harmonizer (v, lim);

        // 4. filter (§1.6)
        const auto kt = f32 (f32 (v.lf0 + -7.0) * P.kt);
        const auto loKt = f32 (P.lo + kt);
        const auto p0 = f32 (f32 (v.knobS + kt) + f32 (f32 (v.finalFcut + (double) v.lvl[3]) * P.fcutScale));
        auto pos = p0 < loKt ? loKt : (p0 > P.hi ? P.hi : p0);
        if (P.lfoCut > 0.0)
            pos = f32 (pos + f32 (v.U * std::min (P.lfoCut, f32 (P.hi - pos))));
        else if (P.lfoCut < 0.0)
            pos = f32 (pos + f32 (v.U * std::max (P.lfoCut, f32 (loKt - pos))));
        const auto amount = P.p[53] <= 0 ? P.envAmount : f32 (P.envAmount * f32 (v.vel + (double) v.lvl[5]));
        if (! P.adaptive)
            pos = f32 (pos + (E - 0.5) * amount);
        else if (amount > 0.0)
            pos = f32 (pos + f32 (f32 (P.hi - pos) * E) * amount);
        else
            pos = f32 (pos - f32 (f32 (loKt - pos) * E) * amount);
        v.pos = pos;
        const auto fc = std::exp (pos * ln2);
        const auto interp = P.quality == 2;
        {
            const auto& table = tab.filter[(std::size_t) P.ftype];
            const auto origin = shapeOf (P.ftype).origin;
            if (P.p[55] == 0)
            {
                const auto k = (float) P.W, off = (float) f32 (origin - f32 (pos * P.W));
                for (int i = 0; i < lim; ++i)
                    amps[(std::size_t) i] = f32 (amps[(std::size_t) i] * shapeRead (table, v.l2[(std::size_t) i] * k + off, interp));
            }
            else
            {
                const auto b = f32 (P.W * 0.9);
                const auto k = (float) f32 (b / fc), off = (float) f32 (origin - b);
                for (int i = 0; i < lim; ++i)
                    amps[(std::size_t) i] = f32 (amps[(std::size_t) i] * shapeRead (table, v.hz[(std::size_t) i] * k + off, interp));
            }
        }

        // 5. resonance (§1.8)
        auto xr = f32 (f32 (P.p[59] * milli) + (double) f32 (v.finalFres + (double) v.lvl[4]) * 0.5);
        xr = std::clamp (xr, 0.0, 1.0);
        auto ra = f32 ((xr == 1.0 ? 1.0 : f32 ((std::exp (xr * std::log (11.0)) - 1.0) * (double) 0.1f)) * 0.25);
        if (P.p[64] != 0)
        {
            v.motion = ! v.motionInit ? pos : f32 (pos + (v.motion - pos) * P.motionC);
            v.motionInit = true;
            const auto dd = std::fabs (pos - v.motion);
            ra = f32 (ra * std::min (1.0, f32 (dd * dd)));
        }
        if (ra > 0.0 && P.rtype == 108)
        {
            // Noise: energy-preserving random gates on the partials, held for the grain length
            const auto q = f32 (std::pow ((double) f32 (ra * 4.0), 3.0) * 9999.0 / P.noiseLength);
            const auto hiV = (float) f32 (q + 1.0);
            const auto qf = (float) q;
            UniformGenerator gen (v.nseed);
            const auto sc = (double) hiV * 0x1p-32, of = (double) (float) (0.0f + hiV) * 0.5;
            const auto keep = (float) std::sqrt ((double) hiV);
            for (int i = 0; i < N; ++i)
            {
                auto x = (float) ((double) gen.next() * sc + of);
                if (x < qf)
                    x = 0.0f;
                if (x > 0.0f)
                    x = keep;
                if (i < lim)
                    amps[(std::size_t) i] = f32 (amps[(std::size_t) i] * (double) x);
            }
            v.nacc += P.noiseStep;
            if (v.nacc >= 1.0)
            {
                double whole = 0.0;
                v.nacc = std::modf (v.nacc, &whole);
                v.nseed = gen.state();
            }
        }
        if (ra > 0.0 && P.rtype != 108)
        {
            const auto k = P.p[63] <= 0 ? P.rK : f32 (P.rK / v.f0);
            const auto& table = tab.resonance[(std::size_t) P.rtype - 100];
            const auto ro = shapeOf (P.rtype).origin;
            const auto kf = (float) k;
            const auto off = P.p[60] == 0 ? (float) f32 (ro - f32 (f32 (pos + P.rOffset) * k)) : (float) f32 (ro - fc * P.rMul * k);
            const auto gc = (float) f32 (P.rC - v.lf0), raf = (float) ra;
            for (int i = 0; i < lim; ++i)
            {
                const auto x = (P.p[60] == 0 ? v.l2[(std::size_t) i] : v.hz[(std::size_t) i]) * kf + off;
                const auto R = (float) shapeRead (table, x, interp);
                auto g1 = (v.l2[(std::size_t) i] + gc) * raf;
                g1 = std::max (g1, 0.0f);
                auto G = g1 * R + 1.0f;
                G = std::max (G, 0.0f);
                amps[(std::size_t) i] = f32 (amps[(std::size_t) i] * (double) G);
            }
        }
        if (P.p[85] == 1 && harmonize)
            harmonizer (v, lim);

        // 6. phaser (§1.9)
        bool freqMod = false;
        if (P.p[71] != 0)
        {
            const auto f18 = phaserF18[P.phType], f1c = phaserF1c[P.phType];
            const auto key = f32 (f32 (v.lf0 * (double) P.p[78]) * milli);
            const auto base = f32 (f32 (f32 (f32 (f32 (f32 (-P.p[75]) * milli) - f32 (v.L * P.phOl)) - v.uLfo) * f18) - f32 (v.phPh * f1c));
            double k = 0.0, off = 0.0;
            const bool octave = P.p[72] == 0;
            if (octave)
            {
                k = std::exp (((P.phW - 1.0) * 8.0 + v.L * P.phWl * 2.0) * ln2) * 12.0 * f1c;
                off = base - key * k;
            }
            else if (P.p[72] == 1)
            {
                k = std::exp (((P.phW - 1.0) * 8.0 + v.L * P.phWl * 2.0) * ln2) * 0.005 * f1c;
                off = base - key * f18;
            }
            else
            {
                k = ((2.0 - f32 (P.phW + f32 (v.L * P.phWl))) * f1c) / v.f0;
                off = base - key * f18;
            }
            const auto& Tp = tab.phaser[(std::size_t) P.phType];
            const auto Np = phaserSize[P.phType];
            const auto kf = (float) k, offf = (float) off;
            const auto mixv = P.phMix;
            const auto oneMinus = f32 (1.0 - mixv);
            const auto amt = P.phType == 9 ? (double) f32 ((double) f32 (mixv * mixv * mixv) * ln2 * 0.25) : 0.0;
            for (int i = 0; i < lim; ++i)
            {
                const auto x = (octave ? v.l2[(std::size_t) i] : v.hz[(std::size_t) i]) * kf + offf;
                double Pv = 0.0;
                if (interp)
                {
                    const auto ii = toInt64 (rintf (x - 0.5f));
                    const auto fr = (double) (float) (x - (float) ii);
                    const auto j = (std::size_t) (ii & (Np - 1));
                    Pv = f32 (Tp[j] * (1.0 - fr) + Tp[j + 1] * fr);
                }
                else
                    Pv = Tp[(std::size_t) (toInt64 (rintf (x)) & (Np - 1))];
                if (P.phType == 9)
                {
                    const auto mulv = (float) std::exp ((double) (float) (Pv * amt));
                    v.freq[(std::size_t) i] = f32 ((double) (mulv * v.hz[(std::size_t) i]) * (double) (float) (1.0 / fs));
                }
                else
                    amps[(std::size_t) i] = f32 (amps[(std::size_t) i] * f32 (Pv * mixv + oneMinus));
            }
            freqMod = P.phType == 9;
        }
        if (P.p[85] == 2 && harmonize)
            harmonizer (v, lim);

        // 7. pluck (§1.11)
        if (v.pluckOn)
        {
            if (v.pluckCnt >= 0)
            {
                if (v.pluckDirty || v.pluckInit < N || P.p[34] > 0)
                {
                    auto T = P.pluckT;
                    if (P.p[32] > 0)
                        T = T * v.vel * v.vel;
                    const auto f3 = (float) f32 (P.pluckNum / (T + 0.25));
                    for (int i = 0; i < N; ++i)
                    {
                        float rate = 0.0f;
                        if (P.p[33] == 0)
                            rate = (float) P.ratio[(std::size_t) i] * f3;
                        else
                            rate = f3 / std::max (std::fabs ((float) P.a570[(std::size_t) i]), 0.001f);
                        if (P.p[34] > 0 && m > 0.0 && i >= 4)
                            rate = (float) ((double) rate * (P.mask[(std::size_t) ((i - 4) % 12)] * m + oneMinusM));
                        v.pluckDecay[(std::size_t) i] = (float) std::exp ((double) rate);
                    }
                    v.pluckDirty = false;
                }
                if (v.pluckInit < N)
                {
                    for (int i = v.pluckInit; i < N; ++i)
                        v.pluckLevel[(std::size_t) i] = v.pluckCnt < 1 ? 1.0f : (float) std::pow ((double) v.pluckDecay[(std::size_t) i], (double) v.pluckCnt);
                    v.pluckInit = N;
                }
                for (int i = 0; i < N; ++i)
                {
                    if (i < lim)
                        amps[(std::size_t) i] = f32 (amps[(std::size_t) i] * (double) v.pluckLevel[(std::size_t) i]);
                    v.pluckLevel[(std::size_t) i] = v.pluckLevel[(std::size_t) i] * v.pluckDecay[(std::size_t) i];
                }
            }
            ++v.pluckCnt;
        }
        if (P.p[85] == 3 && harmonize)
            harmonizer (v, lim);

        // 8. low-harmonics protection (toward colour × EQ), 9. special gains
        for (int i = 0; i < 8; ++i)
            amps[(std::size_t) i] = amps[(std::size_t) i] + (P.a570[(std::size_t) i] - amps[(std::size_t) i]) * tab.lhp[(std::size_t) i] * P.lhp;
        for (int i = 0; i < 4; ++i)
            amps[(std::size_t) i] = amps[(std::size_t) i] * P.gain580[(std::size_t) i];

        // 10. band limit (§1.3), partials entering the bank, initial phases
        if (changed || freqMod)
        {
            if (freqMod)
                bandLimit (v, 1, N); // (otherwise already done with the new frequencies)
            if (oldCnt < N && ! v.first)
            {
                // FUN_00746080: partials entering the bank get their phase + f_j · (elapsed time estimate)
                const auto fl = v.freq[(std::size_t) oldCnt - 1];
                if (fl > 0.0)
                {
                    const auto r = (v.phase[(std::size_t) oldCnt - 1] - v.ph70) / fl;
                    v.ph70 = v.phase[(std::size_t) N - 1];
                    for (int i = oldCnt; i < N; ++i)
                        v.phase[(std::size_t) i] += v.freq[(std::size_t) i] * r;
                }
            }
        }
        if (v.first)
        {
            const auto f1 = v.freq[4];
            double whole = 0.0;
            const auto rr = std::modf (f1 * 277.0 * (double) G.counter, &whole);
            const auto dv = P.phRand > 0.0 ? P.phRand * rr : P.phRand < 0.0 ? P.phRand * P.phRand * rr : 0.0;
            std::array<double, bankSize> base = P.p598;
            if (P.phRand < 0.0)
            {
                // spectral random: a uniform draw in [0, x²] for every position, from the record's seed
                const auto hi = -P.phRand;
                const auto sc = hi * 0x1p-32, of = (0.0 + hi) * 0.5;
                UniformGenerator gen (v.seed);
                for (int i = 0; i < bankSize; ++i)
                    base[(std::size_t) i] = base[(std::size_t) i] + ((double) gen.next() * sc + of);
            }
            v.ph70 = base[(std::size_t) N - 1];
            const auto r = (v.uPhase + dv) / f1;
            v.phase = base;
            for (int i = 0; i < N; ++i)
                v.phase[(std::size_t) i] = base[(std::size_t) i] + v.freq[(std::size_t) i] * r;
            v.phase[0] = v.phase[1] = v.phase[2] = v.phase[3] = 0.0;
        }
        amps[0] = f32 (f32 (P.gain580[0] * ra) * (double) v.pluckLevel[1]);
        const auto fNew = fc * P.rMul * (1.0 / fs);
        if (v.freq[0] != fNew)
        {
            v.freq[0] = fNew;
            const auto f = f32 (fNew);
            const auto lo1 = 32.0 / fs, lo0 = 16.0 / fs, hi0 = 16000.0 / fs, hi1 = 17000.0 / fs;
            double b = 1.0;
            if (f > hi1 || f < lo0)
                b = 0.0;
            else if (f > hi0)
                b = std::pow (1.0 - (f - hi0) / (hi1 - hi0), 2.0);
            else if (f < lo1)
                b = std::pow (1.0 - (lo1 - f) / (lo1 - lo0), 2.0);
            v.bl[0] = b;
        }
        // quality 2 (per-sample bank path): the targets are stored before the band-limit gains are applied
        for (int i = 0; i < lim; ++i)
            v.T[(std::size_t) i] = f32 (P.quality < 2 ? amps[(std::size_t) i] * v.bl[(std::size_t) i] : amps[(std::size_t) i]);
        for (int i = lim; i < N; ++i)
            v.T[(std::size_t) i] = 0.0;
        v.prevAc = freqMod ? f32 (2147483648.0) : localAc;
        v.first = false;
    }

    // the sine bank over one segment of m samples: Σ a_i[k]·sin(2π(φ_i + k·f_i)), amplitudes gliding to the targets
    // (a_i[k] = T + (a − T)·kamp^k). Each sine runs as a double-precision two-term recurrence: a new frequency keeps
    // the running phase (only the step changes) and every 8192 samples it is restarted from the partial's phase
    // accumulator (as the DLL resyncs its own), so it equals the direct sine to ~1e-12. The partials are processed in
    // groups of 8 kept in registers.
    void renderBank (Record& v, int m, double* out) noexcept
    {
        if (kpowFor != P.kamp)
        {
            for (int j = 0; j <= maxControlBlock; ++j)
                kpow[(std::size_t) j] = std::pow (P.kamp, (double) j);
            kpowFor = P.kamp;
        }
        const auto N = v.cnt;
        // skip the silent top of the bank (exactly zero amplitude and target: identical output)
        int top = N;
        while (top > 0 && v.a[(std::size_t) top - 1] == 0.0 && v.T[(std::size_t) top - 1] == 0.0)
            --top;
        top = (top + 7) & ~7;
        const auto resync = v.oscAge + m > 8192;
        if (resync)
            v.oscAge = 0;
        // harmonics without grittiness or frequency phaser lie on f_h = (1 + (h − 1)·D)·f₀/fs: their steps e^{2πi·f_h}
        // follow from two sincos by complex rotation (the rounding differs from a direct sincos by ~h·1e-16)
        const auto geometric = P.grit == 0.0 && ! (P.p[71] != 0 && P.phType == 9) && top > 4;
        double gr = 0.0, gi = 0.0, rr = 1.0, ri = 0.0;
        if (geometric)
        {
            const auto s = v.freq[4]; // ratio 1
            gr = std::cos (twoPi * s), gi = std::sin (twoPi * s);
            rr = std::cos (twoPi * P.D * s), ri = std::sin (twoPi * P.D * s);
        }
        for (int i = 0; i < top; ++i)
        {
            const auto u = (std::size_t) i;
            const auto f = v.freq[u];
            double sw = 0.0, cw = 0.0;
            const auto changed = resync || i >= v.oscTop || f != v.oscF[u];
            if (geometric && i >= 4)
            {
                cw = gr, sw = gi;
                const auto nr = gr * rr - gi * ri;
                gi = gr * ri + gi * rr;
                gr = nr;
            }
            if (! changed)
                continue;
            if (! geometric || i < 4)
                sw = std::sin (twoPi * f), cw = std::cos (twoPi * f);
            if (! resync && i < v.oscTop && std::fabs (v.oscS[u]) > 1.0e-3)
            {
                // new frequency, same phase: cos θ from the running pair, then the pair for the new step
                const auto x = v.osc0[u];
                const auto cosTheta = (x * (0.5 * v.oscC[u]) - v.osc1[u]) / v.oscS[u];
                v.osc1[u] = x * cw - cosTheta * sw;
            }
            else
            {
                const auto ph = v.phase[u];
                v.osc0[u] = std::sin (twoPi * ph);
                v.osc1[u] = std::sin (twoPi * (ph - f));
            }
            v.oscC[u] = 2.0 * cw;
            v.oscS[u] = sw;
            v.oscF[u] = f;
        }
        v.oscTop = top;
        v.oscAge += m;
        // per-lane sums over all groups (lane l holds the partials ≡ l mod 8), combined once per sample at the end
        std::fill (laneSums.begin(), laneSums.begin() + 8 * m, 0.0);
        auto* acc = laneSums.data();
        bankKernel (v.osc0.data(), v.osc1.data(), v.oscC.data(), v.T.data(), v.a.data(), top, m, kpow.data(), acc);
        for (int j = 0; j < m; ++j)
        {
            const auto* a = acc + 8 * j;
            out[j] = ((a[0] + a[4]) + (a[2] + a[6])) + ((a[1] + a[5]) + (a[3] + a[7]));
        }
        const auto km = kpow[(std::size_t) m];
        for (int i = 0; i < N; ++i)
        {
            auto& a = v.a[(std::size_t) i];
            a = v.T[(std::size_t) i] + (a - v.T[(std::size_t) i]) * km;
            if (v.T[(std::size_t) i] == 0.0 && std::fabs (a) < 1e-30)
                a = 0.0; // a released partial reaches exactly 0 (below −600 dB) instead of running into denormals
            v.phase[(std::size_t) i] = v.phase[(std::size_t) i] + v.freq[(std::size_t) i] * m;
        }
    }

    void renderRecord (Record& v, double* out, int n) noexcept
    {
        auto vibPh = G.vibPhase;
        v.phPh = f32 (G.phPhase);
        if (P.lfoGlobal)
            v.lfoPhase = G.lfoPhase;
        const auto knobT = P.knob;
        if (! v.knobInit)
            v.knobS = knobT, v.knobInit = true;
        int k = 0;
        while (k < n)
        {
            if (v.ctrlLeft <= 0)
            {
                v.ctrlLeft = P.block;
                controlBlock (v, vibPh, knobT);
            }
            const auto m = std::min ({ v.ctrlLeft, n - k, maxControlBlock });
            renderBank (v, m, out + k);
            v.lfoPhase = v.lfoPhase + P.lfoInc * m;
            vibPh = f32 (vibPh + f32 (P.vibInc * f32 (m)));
            v.phPh = f32 (v.phPh + P.phInc * m);
            v.lfoR = std::min (1.0, f32 (v.lfoR + f32 (P.lfoAttack * f32 (m))));
            v.vibR = std::min (1.0, f32 (v.vibR + f32 (P.vibR * f32 (m))));
            v.ctrlLeft -= m;
            k += m;
        }
        v.env.render (envBuf.data(), n);
        for (int j = 0; j < n; ++j)
            out[j] = f32 (out[j] * envBuf[(std::size_t) j]);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Settings handed from configure() (UI thread) to the audio thread without blocking

template <typename T>
class Mailbox
{
public:
    void write (const T& value) noexcept
    {
        bool expected = false;
        while (! busy.compare_exchange_weak (expected, true))
            expected = false;
        pending = value;
        fresh = true;
        busy.store (false);
    }

    bool read (T& out) noexcept
    {
        bool expected = false;
        if (! busy.compare_exchange_strong (expected, true))
            return false;
        const auto had = fresh;
        if (had)
            out = pending;
        fresh = false;
        busy.store (false);
        return had;
    }

private:
    std::atomic<bool> busy { false };
    T pending {};
    bool fresh = false;
};

struct Settings
{
    std::array<float, harmlessParamCount> raw {};
    std::shared_ptr<const hlfx::MaximusPreset> comp; // the rack compressor's preset (built off the audio thread)
    double bpm = 120.0;
    float pitchCents = 0.0f; // channel pitch/fine/root shift, cents relative to C5
};

// FL's Harmless with an untouched preset: Saw, open filter, no modulation (the reference's clean patch)
Settings defaultSettings() noexcept
{
    Settings s;
    auto& p = s.raw;
    for (int i = 11; i < 23; ++i)
        p[(std::size_t) i] = 1000.0f;
    p[27] = 1000, p[28] = 250, p[31] = 1000, p[36] = 1000, p[37] = 1, p[47] = 1, p[49] = 500, p[51] = -300, p[54] = 8192;
    p[56] = 500, p[65] = 1, p[66] = 128, p[67] = 128, p[68] = 64, p[89] = 3000;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// LYNX front-end
//
// Levels (§1.16, REA_rodada28 §4.1). FL hands Harmless Final.Vol = chan.Vol (× Init.Vol when VEL → volume is on,
// Init.Vol = (11^(vel/100) − 1)/10, inference), Final.Pan = note pan + chan.Pan and Final.Pitch = 100·(key − 60) +
// channel pitch; each record asks the host for L = vol·cos((1 + pan)π/4), R = vol·sin(…) with
// vol = 0.75·(3^x36 − 1)/2·(Final.Vol + offset)·unison gain. LYNX's channel gains are gl = √2·V·cos((1 + p)π/4),
// gr = √2·V·sin(…), so V = √((gl² + gr²)/2) and p are recovered exactly and FL's law is applied once (the channel
// volume and pan are not applied a second time). Velocity = LYNX velocity (FL velocity / 128). Mod X/Y
// (Final.FCut/FRes) are 0: LYNX notes have none. Host events 4/5 (release velocity, release-time factor) are
// answered with 0 and 1, as in the reference.
//
// Quality: the editor's "quality" combo (state integer, 0 or 1) is used for both playback and export — LYNX's engine
// does not tell an instrument when it renders offline, so FL's quality 2 for rendering (§1.2) is not used.
// Random sources: deterministic per instrument (see Random); FL's are clock-seeded, so spectral-random phases, Noise
// resonance and the LFO "voice random" differ from any given FL render by design.
// Effects rack (REA_rodada36): chorus, delay and reverb from the plugin's own preset tables; the compressor is FL's
// Maximus engine with Harmless's preset <n>.fst, read by the importer from the user's FL installation (without it the
// compressor stays off and the import report says so). The delay's time follows the project tempo at load.

class HarmlessInstrument final : public Instrument
{
public:
    HarmlessInstrument()
    {
        settings = defaultSettings();
        engine.P.build (settings.raw.data(), 44100.0, 0, settings.bpm);
    }

    void prepare (double sr) noexcept override
    {
        mailbox.read (settings);
        rate = sr;
        rebuild();
        engine.reset();
        songTicks = -1.0;
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        Settings s = defaultSettings();
        if (channel.modelParams.size() >= (std::size_t) harmlessParamCount)
            for (std::size_t i = 0; i < s.raw.size(); ++i)
                s.raw[i] = std::isfinite (channel.modelParams[i]) ? channel.modelParams[i] : 0.0f;
        s.bpm = bpm > 0.0 ? bpm : 120.0;
        s.pitchCents = channel.sampler.pitch * 100.0f;
        const auto& mp = channel.modelParams;
        if (mp.size() > (std::size_t) harmlessCompIndex)
        {
            const auto count = (std::size_t) std::max (0.0f, mp[(std::size_t) harmlessCompIndex]);
            if (count > 0 && mp.size() >= (std::size_t) harmlessCompIndex + 1 + count)
            {
                std::vector<std::uint8_t> state (count);
                for (std::size_t k = 0; k < count; ++k)
                    state[k] = (std::uint8_t) std::clamp ((int) mp[(std::size_t) harmlessCompIndex + 1 + k], 0, 255);
                s.comp = hlfx::makeMaximusPreset (state);
            }
        }
        mailbox.write (s);
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        engine.slideTo (100.0 * std::clamp (key, 0, 127) - 6000.0 + (double) settings.pitchCents + (double) cents, samples);
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        // VEL → volume (p37): Final.Vol = chan.Vol·Init.Vol when on, chan.Vol when off
        const auto volFactor = engine.P.p[37] > 0 ? (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0 : 1.0;
        engine.noteOn (key, vel, volFactor, channelVol, (double) nextPan, channelPan, (double) settings.pitchCents + (double) nextCents);
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (const auto i = engine.findHeld (key); i >= 0)
            engine.noteOff (i);
    }

    void allNotesOff() noexcept override { engine.allNotesOff(); }

    bool idle() const noexcept override { return engine.idle() && engine.rack.quiet(); }

    // the LFO (global mode) and phaser phases follow song-position jumps as FL's song-position event sets them
    // (§1.9, §1.12); continuous playback and a stopped transport leave them running
    void setSongPosition (double ticks) noexcept override
    {
        const auto ticksPerSample = (double) ppq * settings.bpm / (60.0 * rate);
        const auto expected = songTicks + (double) sinceTicks * ticksPerSample;
        if (songTicks >= 0.0 && ticks != songTicks && std::fabs (ticks - expected) > 1.0)
        {
            const auto nPos = std::floor (ticks / ticksPerSample);
            double whole = 0.0;
            engine.G.lfoPhase = std::modf (nPos * engine.P.lfoInc, &whole);
            engine.G.phPhase = 4.0 * std::modf (nPos * engine.P.phInc / 4.0, &whole);
        }
        songTicks = ticks;
        sinceTicks = 0;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        channelVol = vol;
        channelPan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / pi) - 1.0 : 0.0;
        sinceTicks += n;
        if (n <= 0)
            return;
        if (engine.idle() && engine.rack.quiet())
        {
            // nothing sounds: only the plugin-wide phases and counters run (the rack's included)
            for (int done = 0; done < n; done += Engine::maxHostBlock)
            {
                const auto m = std::min (Engine::maxHostBlock, n - done);
                engine.G.advance (engine.P, m);
                engine.N += m;
                engine.rack.advance (m);
            }
            engine.tremPrev = engine.P.trem;
            return;
        }
        for (int done = 0; done < n; done += Engine::maxHostBlock)
        {
            const auto m = std::min (Engine::maxHostBlock, n - done);
            engine.render (left + done, right + done, m, channelVol, channelPan);
        }
    }

private:
    void take() noexcept
    {
        if (mailbox.read (incoming))
        {
            const auto changed = std::memcmp (incoming.raw.data(), settings.raw.data(), sizeof (float) * settings.raw.size()) != 0
                                 || incoming.bpm != settings.bpm || incoming.comp != settings.comp;
            settings = incoming;
            if (changed)
                rebuild();
        }
    }

    void rebuild() noexcept
    {
        const auto& raw = settings.raw;
        engine.P.build (raw.data(), rate, (int) std::lround (raw[(std::size_t) harmlessComboIndex + 6]), settings.bpm);
        engine.rack.prepare (engine.P.p, engine.P.t, rate, settings.bpm, settings.comp);
    }

    Engine engine;
    Settings settings, incoming;
    Mailbox<Settings> mailbox;
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0, songTicks = -1.0;
    long long sinceTicks = 0;
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

std::optional<std::vector<float>> harmlessParams (const std::vector<std::uint8_t>& state)
{
    auto i32 = [&] (std::size_t at) { std::int32_t v = 0; std::memcpy (&v, state.data() + at, 4); return v; };
    if (state.size() < 4)
        return std::nullopt;
    const auto version = i32 (0);
    if (version < 4 || version > 12)
        return std::nullopt;
    const auto count = version < 9 ? 99 : 100;
    const auto tail = 4 + 4 * (std::size_t) count;
    if (state.size() < tail + 28)
        return std::nullopt;
    std::vector<float> out ((std::size_t) harmlessParamCount, 0.0f);
    for (int i = 0; i < count; ++i)
        out[(std::size_t) i] = (float) i32 (4 + 4 * (std::size_t) i);
    for (int i = 0; i < 7; ++i)
        out[(std::size_t) harmlessComboIndex + (std::size_t) i] = (float) i32 (tail + 4 * (std::size_t) i);
    if (version >= 8 && state.size() >= tail + 32)
    {
        const auto entries = i32 (tail + 28);
        const auto at = tail + 32;
        if (entries > 0 && state.size() >= at + 8 * (std::size_t) entries)
        {
            out[(std::size_t) harmlessCustomFlagIndex] = 1.0f;
            const auto floats = std::min<std::size_t> (2 * (std::size_t) entries, (std::size_t) harmlessCustomSize);
            for (std::size_t k = 0; k < floats; ++k)
            {
                float v = 0.0f;
                std::memcpy (&v, state.data() + at + 4 * k, 4);
                out[(std::size_t) harmlessCustomIndex + k] = std::isfinite (v) ? v : 0.0f;
            }
        }
    }
    return out;
}

std::unique_ptr<Instrument> makeHarmlessInstrument() { return std::make_unique<HarmlessInstrument>(); }
} // namespace lynx
