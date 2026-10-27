#include "core/FlLegacyFx.h"

// FL's legacy Fruity Phase Inverter, Center, Bass Boost, 7 Band EQ and Delay (v1), exact engines (docs/analise/REA_rodada165k.md).
// Clean-room: written from the behaviour, formulas and data layouts described there; nothing of Image-Line's code or
// data is copied. Arithmetic follows the plug-ins operation by operation (float / double steps in their order, one
// rounding each, no contraction); the x87 kernels run in 80-bit long double with 64-bit precision and round to
// nearest; the Delphi run-time's Exp / Ln / SinCos / Sinh come from core/DelphiMath.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/DelphiMath.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace lynx::fllegacy
{
namespace
{
using ld = long double;
constexpr int engineBlock = 512;
constexpr float tiny = 5.9604644775390625e-08f; // 2^-24

std::int32_t i32At (const std::vector<std::uint8_t>& d, std::size_t o) noexcept
{
    std::int32_t v = 0;
    std::memcpy (&v, d.data() + o, 4);
    return v;
}
int rawFrom (float v, int lo, int hi) noexcept
{
    return lo + (int) std::lround (std::clamp ((double) v, 0.0, 1.0) * (hi - lo));
}
float norm (int raw, int lo, int hi) noexcept { return (float) std::clamp ((double) (raw - lo) / (hi - lo), 0.0, 1.0); }
// a control's value as the plug-ins' ProcessParam computes it: (raw − min) · (1/range) in double, clamped to 0…1
float unit (int raw, int lo, int hi) noexcept
{
    return (float) std::clamp ((double) (raw - lo) * (1.0 / (double) (hi - lo)), 0.0, 1.0);
}

// x87 control word: 64-bit precision, round to nearest, for the extended-precision kernels
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Mode
{
    unsigned short saved = 0;
    X87Mode() noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved) : : "memory");
        unsigned short cw = (unsigned short) ((saved & ~0x0F00u) | 0x0300u);
        asm volatile ("fldcw %0" : : "m"(cw) : "memory");
    }
    ~X87Mode() { asm volatile ("fldcw %0" : : "m"(saved) : "memory"); }
    X87Mode (const X87Mode&) = delete;
    X87Mode& operator= (const X87Mode&) = delete;
};
#else
struct X87Mode
{
};
#endif

// the plug-ins' end-of-call guard on an x87 state: |s| ≤ 2^-24 → 0, stored as a double
double guard (ld s) noexcept { return (std::fabs (s) > (ld) tiny) ? (double) s : 0.0; }

// Delphi's Power (Single, Single) for the arguments these plug-ins pass (integral exponents by repeated squaring in
// double with a single result per step, otherwise Exp (e · Ln b))
float intPowerSingle (float base, int n) noexcept
{
    int k = std::abs (n);
    double b = base;
    float r = 1.0f;
    while (k > 0)
    {
        while ((k & 1) == 0)
        {
            k >>= 1;
            b = b * b;
        }
        --k;
        r = (float) ((double) r * b);
    }
    return n < 0 ? (float) (1.0 / (double) r) : r;
}
float powerSingle (float base, float e) noexcept
{
    if ((double) e == 0.0)
        return 1.0f;
    if ((double) base == 0.0 && (double) e > 0.0)
        return 0.0f;
    const double ed = e;
    if (ed - std::trunc (ed) == 0.0 && std::fabs (ed) <= 2147483647.0)
        return intPowerSingle (base, (int) std::trunc (ed));
    if (base < 0.0f)
        return std::nanf ("");
    return (float) dmath::exp ((double) e * dmath::ln ((double) base));
}
} // namespace

// ================================================================================================ state readers
std::optional<PhaseInverterState> parsePhaseInverter (const std::vector<std::uint8_t>& d)
{
    // i32 version (the plug-in reads version ≤ 1), then the control
    if (d.size() < 8 || i32At (d, 0) > 1)
        return std::nullopt;
    return PhaseInverterState { i32At (d, 4) };
}
std::optional<CenterState> parseCenter (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 8 || i32At (d, 0) > 1)
        return std::nullopt;
    return CenterState { i32At (d, 4) };
}
std::optional<BassBoostState> parseBassBoost (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 12 || i32At (d, 0) > 1)
        return std::nullopt;
    return BassBoostState { i32At (d, 4), i32At (d, 8) };
}
std::optional<Eq7State> parseEq7 (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 32 || i32At (d, 0) > 1)
        return std::nullopt;
    Eq7State s;
    for (int b = 0; b < eq7Bands; ++b)
        s.gain[(std::size_t) b] = i32At (d, 4 + 4 * (std::size_t) b);
    return s;
}

std::optional<Delay1State> parseDelay1 (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 4 + 4 * (std::size_t) delay1Params || i32At (d, 0) > 1)
        return std::nullopt;
    Delay1State s;
    for (int k = 0; k < delay1Params; ++k)
        s.raw[(std::size_t) k] = i32At (d, 4 + 4 * (std::size_t) k);
    return s;
}

namespace
{
constexpr int delay1Lo[delay1Params] = { 0, 0, 0, 0, 1, 0 };
constexpr int delay1Hi[delay1Params] = { 1024, 1024, 1024, 1024, 16, 1024 };
} // namespace

void toParams (const PhaseInverterState& s, float* p) noexcept { p[0] = norm (s.invert, 0, 1024); }
void toParams (const CenterState& s, float* p) noexcept { p[0] = s.on != 0 ? 1.0f : 0.0f; }
void toParams (const BassBoostState& s, float* p) noexcept
{
    p[0] = norm (s.freq, 0, 1024);
    p[1] = norm (s.amount, 0, 1024);
}
void toParams (const Eq7State& s, float* p) noexcept
{
    for (int b = 0; b < eq7Bands; ++b)
        p[b] = norm (s.gain[(std::size_t) b], -1800, 1800);
}
void toParams (const Delay1State& s, float* p) noexcept
{
    for (int k = 0; k < delay1Params; ++k)
        p[k] = norm (s.raw[(std::size_t) k], delay1Lo[k], delay1Hi[k]);
}
Delay1State delay1FromParams (const float* p) noexcept
{
    Delay1State s;
    for (int k = 0; k < delay1Params; ++k)
        s.raw[(std::size_t) k] = rawFrom (p[k], delay1Lo[k], delay1Hi[k]);
    return s;
}
PhaseInverterState phaseInverterFromParams (const float* p) noexcept { return { rawFrom (p[0], 0, 1024) }; }
CenterState centerFromParams (const float* p) noexcept { return { p[0] >= 0.5f ? 1 : 0 }; }
BassBoostState bassBoostFromParams (const float* p) noexcept { return { rawFrom (p[0], 0, 1024), rawFrom (p[1], 0, 1024) }; }
Eq7State eq7FromParams (const float* p) noexcept
{
    Eq7State s;
    for (int b = 0; b < eq7Bands; ++b)
        s.gain[(std::size_t) b] = rawFrom (p[b], -1800, 1800);
    return s;
}

const float eq7BandHz[eq7Bands] = { 63.0f, 250.0f, 500.0f, 1500.0f, 3000.0f, 5000.0f, 8000.0f };

// ================================================================================================ value texts
int phaseInverterMode (int raw) noexcept
{
    const float x = unit (raw, 0, 1024);
    return (int) std::nearbyint ((double) (x * 3.0f - 0.5f)); // round half to even, as the x87 / SSE conversion
}
std::string phaseInverterParamText (int param, float v01)
{
    if (param != 0)
        return {};
    static const char* const names[3] = { "None", "Left", "Right" };
    return names[std::clamp (phaseInverterMode (rawFrom (v01, 0, 1024)), 0, 2)];
}
std::string centerParamText (int param, float v01)
{
    return param == 0 ? std::string (v01 >= 0.5f ? "On" : "Off") : std::string();
}
namespace
{
int bassHz (int raw) noexcept { return (int) std::nearbyint ((double) (unit (raw, 0, 1024) * 80.0f + 20.0f)); }
float bassGain (int raw) noexcept
{
    const double v = unit (raw, 0, 1024);
    return (float) (dmath::exp (v * dmath::ln (11.0)) - 1.0) * 0.5f;
}
float eq7Db (int raw) noexcept { return unit (raw, -1800, 1800) * 36.0f + -18.0f; }
} // namespace
std::string bassBoostParamText (int param, float v01)
{
    char b[32];
    if (param == 0)
        std::snprintf (b, sizeof b, "%d Hz", bassHz (rawFrom (v01, 0, 1024)));
    else if (param == 1)
        std::snprintf (b, sizeof b, "%d%%", (int) std::lround (std::clamp (v01, 0.0f, 1.0f) * 100.0f));
    else
        return {};
    return b;
}
std::string eq7ParamText (int param, float v01)
{
    if (param < 0 || param >= eq7Bands)
        return {};
    char b[32];
    std::snprintf (b, sizeof b, "%+.1f dB", (double) eq7Db (rawFrom (v01, -1800, 1800)));
    return b;
}

namespace
{
int delphiRound (double x) noexcept { return (int) std::nearbyint (x); } // Round: to nearest, ties to even
int delayTempo (float v) noexcept { return delphiRound ((double) (v * 220.0f + 60.0f)); }
int delayMode (float v) noexcept { return delphiRound ((double) (v * 3.0f - 0.5f)); }
} // namespace
std::string delay1ParamText (int param, float v01)
{
    if (param < 0 || param >= delay1Params)
        return {};
    const int raw = rawFrom (v01, delay1Lo[param], delay1Hi[param]);
    const float v = unit (raw, delay1Lo[param], delay1Hi[param]);
    char b[32];
    if (param == 3)
    {
        if (raw == 0)
            return "Host";
        std::snprintf (b, sizeof b, "%d BPM", delayTempo (v));
    }
    else if (param == 4)
        std::snprintf (b, sizeof b, "%d/16", raw);
    else if (param == 5)
    {
        static const char* const names[3] = { "Normal", "Inverted stereo", "Ping-pong" };
        return names[std::clamp (delayMode (v), 0, 2)];
    }
    else
        std::snprintf (b, sizeof b, "%d%%", (int) std::lround (v * 100.0f));
    return b;
}

// ================================================================================================ Fruity Center
void CenterEngine::prepare (int sampleRate) noexcept
{
    // the host's rate ratio 44100 / rate times the plug-in's constant
    c = (float) (4.567e-06 * (44100.0 / (double) std::max (1, sampleRate)));
    q = 0.96f;
    std::fill (std::begin (st), std::end (st), 0.0);
}

void CenterEngine::process (float* left, float* right, int n) noexcept
{
    if (! on || n <= 0)
        return; // off: the input is copied through
    X87Mode mode;
    ld l0 = st[0], l1 = st[1], v0 = st[2], v1 = st[3];
    const ld cc = c, qq = q;
    for (int i = 0; i < n; ++i)
    {
        const ld xl = left[i];
        v0 += (xl - l0) * cc;
        l0 += v0;
        left[i] = (float) (xl - l0);
        v0 *= qq;
        const ld xr = right[i];
        v1 += (xr - l1) * cc;
        l1 += v1;
        right[i] = (float) (xr - l1);
        v1 *= qq;
    }
    st[0] = guard (l0), st[1] = guard (l1), st[2] = guard (v0), st[3] = guard (v1);
}

// ================================================================================================ Fruity Bass Boost
void BassBoostEngine::set (const BassBoostState& s) noexcept
{
    // frequency: whole Hz 20…100 at the host's rate; the pole radius from the cosine of the angle
    const int hz = bassHz (s.freq);
    const double w = ((double) hz * 6.283185307179586) / (double) rate;
    const auto cs = (float) dmath::cos (w);
    c = 2.0f - cs * 2.0f;
    const double root2 = std::sqrt (2.0);
    const float m = cs - 1.0f;
    const auto p3 = powerSingle (m, 3.0f);
    const double num = root2 * std::sqrt ((double) -p3) + (double) (m * 0.5f);
    r = (float) (num / (double) (m * 0.5f));
    gain = bassGain (s.amount);
}

void BassBoostEngine::process (float* left, float* right, int n) noexcept
{
    if (n <= 0)
        return;
    X87Mode mode;
    ld l0 = st[0], v0 = st[1], l1 = st[2], v1 = st[3];
    const ld cc = c, g = gain, rr = r;
    for (int i = 0; i < n; ++i)
    {
        const float xl = left[i];
        v0 += ((ld) xl - l0) * cc;
        l0 += v0;
        left[i] = (float) (l0 * g + (ld) xl);
        v0 *= rr;
        const float xr = right[i];
        v1 += ((ld) xr - l1) * cc;
        l1 += v1;
        right[i] = (float) (l1 * g + (ld) xr);
        v1 *= rr;
    }
    st[0] = guard (l0), st[1] = guard (v0), st[2] = guard (l1), st[3] = guard (v1);
}

// ================================================================================================ Fruity 7 Band EQ
namespace
{
constexpr float bandBw[eq7Bands] = { 0.5f, 2.0f, 2.0f, 2.0f, 1.0f, 1.0f, 0.5f };
constexpr int bandType[eq7Bands] = { 5, 6, 6, 6, 6, 6, 7 }; // low shelf, peaks, high shelf
constexpr int tailCalls = 48;
} // namespace

Eq7Engine::Eq7Engine() noexcept
{
    for (int k = 0; k < eq7Bands; ++k)
    {
        auto& b = bands[(std::size_t) k];
        b.type = bandType[k], b.freq = eq7BandHz[k], b.bw = bandBw[k], b.gain = 0.0f;
        design (b);
        b.counter = 0;
    }
}

void Eq7Engine::setBand (int band, int raw) noexcept
{
    if (band < 0 || band >= eq7Bands)
        return;
    auto& b = bands[(std::size_t) band];
    b.gain = eq7Db (raw);
    b.counter = b.gain == 0.0f ? tailCalls : -1;
    design (b);
}

void Eq7Engine::design (Band& b) noexcept
{
    // the design rate is fixed: the plug-ins' 44.1 kHz
    const float w = (6.28318548f * b.freq) / 44100.0f;
    double sd = 0, cd = 0;
    dmath::sinCos ((double) w, sd, cd);
    const auto sn = (float) sd, cs = (float) cd;
    const float A = powerSingle (10.0f, b.gain / 40.0f);
    float alpha;
    if (b.type == 6)
    {
        const double arg = ((0.34657359027997264 * (double) b.bw) * (double) w) / (double) sn;
        alpha = (float) ((double) sn * dmath::sinh (arg));
    }
    else
    {
        float t = A * A;
        t = t + 1.0f;
        t = t / b.bw;
        const float am = A - 1.0f;
        t = t - am * am;
        alpha = (float) std::sqrt ((double) t);
    }
    const double a = A, al = alpha, s = sn, c = cs;
    auto& co = b.co; // b0 b1 b2 a1 a2
    if (b.type == 6)
    {
        const double q = al / a;
        const double inv = 1.0 / (q + 1.0);
        co[3] = (float) ((inv * c) * -2.0);
        co[4] = (float) (inv * (1.0 - q));
        co[1] = co[3];
        const double t = al * a;
        co[0] = (float) (inv * (t + 1.0));
        co[2] = (float) (inv * (1.0 - t));
    }
    else if (b.type == 5) // low shelf
    {
        const double x0 = (a - 1.0) * c;
        const double x2 = al * s;
        const double inv = 1.0 / (((a + 1.0) + x0) + x2);
        co[3] = (float) ((inv * ((a - 1.0) + (a + 1.0) * c)) * -2.0);
        co[4] = (float) (inv * (((a + 1.0) + x0) - x2));
        co[1] = (float) (inv * ((a * 2.0) * ((a - 1.0) - (a + 1.0) * c)));
        co[0] = (float) (inv * (a * (((a + 1.0) - x0) + x2)));
        co[2] = (float) (inv * (a * (((a + 1.0) - x0) - x2)));
    }
    else // 7: high shelf
    {
        const double x0 = (a - 1.0) * c;
        const double x2 = al * s;
        const double inv = 1.0 / (((a + 1.0) - x0) + x2);
        co[3] = (float) ((inv * ((a - 1.0) - (a + 1.0) * c)) * 2.0);
        co[4] = (float) (inv * (((a + 1.0) - x0) - x2));
        co[1] = (float) (inv * ((a * -2.0) * ((a - 1.0) + (a + 1.0) * c)));
        co[0] = (float) (inv * (a * (((a + 1.0) + x0) + x2)));
        co[2] = (float) (inv * (a * (((a + 1.0) + x0) - x2)));
    }
}

void Eq7Engine::process (float* left, float* right, int n) noexcept
{
    if (n <= 0)
        return;
    X87Mode mode;
    for (auto& b : bands)
    {
        if (b.counter == 0)
            continue;
        if (b.counter > 0 && --b.counter == 0)
            std::fill (std::begin (b.h), std::end (b.h), 0.0f); // the tail is over: cleared, this call still runs
        const ld b0 = b.co[0], b1 = b.co[1], b2 = b.co[2], a1 = b.co[3], a2 = b.co[4];
        float* io[2] = { left, right };
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                float* h = b.h + ch; // h[0] x1, h[2] x2, h[4] y1, h[6] y2
                const float x = io[ch][i];
                const ld acc = (((ld) h[0] * b1 + (ld) h[2] * b2) - (ld) h[4] * a1) - (ld) h[6] * a2;
                const auto y = (float) (acc + (ld) x * b0);
                h[2] = h[0], h[0] = x;
                h[6] = h[4], h[4] = y;
                io[ch][i] = y;
            }
        for (auto& v : b.h) // |v| < 2^-24 → 0
            if (std::fabs (v) < tiny)
                v = 0.0f;
    }
}

// ================================================================================================ Fruity Delay (v1)
namespace
{
std::uint32_t bitsOf (float f) noexcept
{
    std::uint32_t u;
    std::memcpy (&u, &f, 4);
    return u;
}
// the plug-ins' per-call parameter glide: from the current value towards the target over the call, at most `limit`
// per sample; a glide ending within 2^-24 of zero ends at zero
void glide (float& slope, float& cur, float& start, float target, float limit, int n) noexcept
{
    start = cur;
    slope = target - start;
    if ((bitsOf (slope) & 0xFFFFFFFFu) == 0)
        return;
    cur = target;
    slope = slope / (float) n;
    if (std::fabs (slope) > limit)
    {
        slope = std::copysign (limit, slope);
        cur = start + slope * (float) n;
        if ((bitsOf (cur) & 0x7FFFFFFFu) < 0x33800000u)
        {
            cur = 0.0f;
            slope = -start / (float) n;
        }
    }
}

// dst += src · g over m frames, the gain gliding by d per frame as the plug-ins' SSE routine steps it (two frames per
// register: even frames from g, odd ones from g + d, each advancing by 2d; the last m mod 8 frames by d)
void depositGlide (const float* l, const float* r, float* dst, int m, float g, float d) noexcept
{
    float a = g, b = g + d;
    const float d2 = d + d;
    const int groups = m >> 3;
    int i = 0;
    for (int k = 0; k < groups * 4; ++k)
    {
        dst[2 * i] = l[i] * a + dst[2 * i], dst[2 * i + 1] = r[i] * a + dst[2 * i + 1];
        ++i;
        dst[2 * i] = l[i] * b + dst[2 * i], dst[2 * i + 1] = r[i] * b + dst[2 * i + 1];
        ++i;
        a = a + d2, b = b + d2;
    }
    for (; i < m; ++i)
    {
        dst[2 * i] = l[i] * a + dst[2 * i], dst[2 * i + 1] = r[i] * a + dst[2 * i + 1];
        a = a + d;
    }
}
} // namespace

void Delay1Engine::prepare (int sampleRate, int maxBlock) noexcept
{
    *this = Delay1Engine();
    rate = std::max (1, sampleRate);
    extra = std::max (1, maxBlock);
    limit = (float) (44.1 / (double) rate); // the host's glide limit per sample: 0.001 at 44.1 kHz
    const Delay1State defaults;
    for (int k = 0; k < delay1Params; ++k)
        setParam (k, defaults.raw[(std::size_t) k]);
    inCur = inTarget;
    fbCur = fbTarget;
}

void Delay1Engine::setHostTempo (float bpm) noexcept
{
    hostTempo = bpm;
    if (tempoBpm == 0)
    {
        sixteenth = delphiRound ((double) ((float) (rate * 15) / hostTempo));
        setLength();
    }
}

void Delay1Engine::setParam (int k, int raw) noexcept
{
    if (k < 0 || k >= delay1Params)
        return;
    const float x = unit (raw, delay1Lo[k], delay1Hi[k]);
    v[k] = x;
    switch (k)
    {
        case 0: inTarget = x; break;
        case 1: fbTarget = (float) ((double) x * 0.99); break;
        case 2:
        {
            const auto e = (float) (dmath::exp ((double) x * dmath::ln (21.0)) - 1.0);
            cutoff = (float) ((double) e * 0.05);
            break;
        }
        case 3:
            if (x == 0.0f)
            {
                tempoBpm = 0;
                sixteenth = delphiRound ((double) ((float) (rate * 15) / hostTempo));
            }
            else
            {
                tempoBpm = delayTempo (x);
                sixteenth = delphiRound ((double) ((float) (rate * 15) / (float) tempoBpm));
            }
            setLength();
            break;
        case 4:
            steps = delphiRound ((double) (x * 15.0f + 1.0f));
            setLength();
            break;
        default: mode = delayMode (x); break;
    }
}

void Delay1Engine::setLength() noexcept
{
    // the delay line holds the delay plus one host block; a new size reallocates it cleared, the read position at its
    // start and the write position one delay ahead, and the feedback glide jumps to its target
    const int len = std::max (1, sixteenth * steps);
    const int total = extra + len;
    if ((int) buf.size() == 2 * total)
        return;
    buf.assign (2 * (std::size_t) total, 0.0f);
    fbCur = fbTarget;
    length = len;
    rd = 0;
    wr = len;
}

void Delay1Engine::process (float* left, float* right, int n) noexcept
{
    if (n <= 0 || buf.empty())
        return;
    const int total = (int) buf.size() / 2;
    // 1. the input, at the Input level (gliding), added into the line at the write position
    {
        float slope = 0, start = 0;
        glide (slope, inCur, start, inTarget, limit, n);
        const int first = std::min (n, total - wr);
        if (bitsOf (slope) == 0)
        {
            if ((std::int32_t) bitsOf (start) > 0)
            {
                auto put = [&] (int from, int at, int m) {
                    for (int i = 0; i < m; ++i)
                    {
                        float* d = buf.data() + 2 * (at + i);
                        d[0] = left[from + i] * start + d[0];
                        d[1] = right[from + i] * start + d[1];
                    }
                };
                put (0, wr, first);
                if (n > first)
                    put (first, 0, n - first);
            }
        }
        else
        {
            depositGlide (left, right, buf.data() + 2 * wr, first, start, slope);
            if (n > first)
                depositGlide (left + first, right + first, buf.data(), n - first, start + slope * (float) first, slope);
        }
    }
    if (mode < 0 || mode > 2)
        return;
    // 2. the delayed signal out (added to the dry input) and fed back through the low-pass into the write position
    float slope = 0, g = 0;
    glide (slope, fbCur, g, fbTarget, limit, n);
    const float k = cutoff;
    // the plug-in's x87 kernels (80-bit): normal keeps the sides; inverted stereo swaps the delayed signals at the
    // output; ping-pong also feeds each side's delayed signal into the other side's line
    X87Mode fpu;
    ld z0 = z28, z1 = z30, gg = g;
    const ld ss = slope, kk = k, thr = (ld) tiny;
    const bool swapOut = mode != 0, cross = mode == 2;
    auto lp = [&] (ld& z, float d, float w) noexcept {
        z = (((ld) d * gg + (ld) w) - z) * kk + z;
        return ! (std::fabs (z) > thr) ? 0.0f : (float) z; // ≤ 2^-24 (or NaN) stores 0
    };
    for (int i = 0; i < n; ++i)
    {
        float* r = buf.data() + 2 * rd;
        float* w = buf.data() + 2 * wr;
        const float dl = r[0], dr = r[1];
        const float xl = left[i], xr = right[i];
        left[i] = (float) ((ld) (swapOut ? dr : dl) + (ld) xl);
        right[i] = (float) ((ld) (swapOut ? dl : dr) + (ld) xr);
        r[0] = r[1] = 0.0f;
        w[0] = lp (z0, cross ? dr : dl, w[0]);
        w[1] = lp (z1, cross ? dl : dr, w[1]);
        gg += ss;
        if (++rd >= total)
            rd = 0;
        if (++wr >= total)
            wr = 0;
    }
    z28 = (double) z0, z30 = (double) z1;
}

// ================================================================================================ effects
namespace
{
template <typename Fn>
void inBlocks (float* left, float* right, int n, Fn&& fn)
{
    for (int done = 0; done < n;)
    {
        const int m = std::min (engineBlock, n - done);
        fn (left + done, right + done, m);
        done += m;
    }
}

class PhaseInverterEffect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        const int mode = phaseInverterMode (phaseInverterFromParams (params.data()).invert);
        float* side = mode == 1 ? left : mode == 2 ? right : nullptr;
        if (side == nullptr)
            return;
        for (int i = 0; i < n; ++i)
        {
            std::uint32_t bits;
            std::memcpy (&bits, side + i, 4);
            bits ^= 0x80000000u; // the plug-in flips the sign bit
            std::memcpy (side + i, &bits, 4);
        }
    }
};

class CenterEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        if (! ready)
        {
            engine = CenterEngine();
            engine.prepare (rate);
            ready = true;
        }
        engine.set (centerFromParams (params.data()));
        inBlocks (left, right, n, [this] (float* l, float* r, int m) { engine.process (l, r, m); });
    }

private:
    CenterEngine engine;
    int rate = 44100;
    bool ready = false;
};

class BassBoostEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        const auto s = bassBoostFromParams (params.data());
        if (! ready || s.freq != current.freq || s.amount != current.amount)
        {
            if (! ready)
            {
                engine = BassBoostEngine();
                engine.prepare (rate);
                ready = true;
            }
            engine.set (s);
            current = s;
        }
        inBlocks (left, right, n, [this] (float* l, float* r, int m) { engine.process (l, r, m); });
    }

private:
    BassBoostEngine engine;
    BassBoostState current {};
    int rate = 44100;
    bool ready = false;
};

class Eq7Effect final : public Effect
{
public:
    void prepare (double) noexcept override { ready = false; }
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        const auto s = eq7FromParams (params.data());
        if (! ready)
            engine = Eq7Engine(); // a fresh plug-in, then the state's controls
        for (int b = 0; b < eq7Bands; ++b)
            if (! ready || s.gain[(std::size_t) b] != current.gain[(std::size_t) b])
                engine.setBand (b, s.gain[(std::size_t) b]);
        if (! ready)
            ready = true;
        current = s;
        inBlocks (left, right, n, [this] (float* l, float* r, int m) { engine.process (l, r, m); });
    }

private:
    Eq7Engine engine;
    Eq7State current {};
    bool ready = false;
};
class Delay1Effect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double bpm) noexcept override
    {
        const auto s = delay1FromParams (params.data());
        const auto tempo = (float) (bpm > 0.0 ? bpm : 140.0);
        if (! ready)
        {
            engine.prepare (rate, engineBlock); // a fresh plug-in, the host's tempo, then the state's controls
            engine.setHostTempo (tempo);
            for (int k = 0; k < delay1Params; ++k)
                engine.setParam (k, s.raw[(std::size_t) k]);
            current = s, hostTempo = tempo, ready = true;
        }
        if (tempo != hostTempo)
        {
            engine.setHostTempo (tempo);
            hostTempo = tempo;
        }
        for (int k = 0; k < delay1Params; ++k)
            if (s.raw[(std::size_t) k] != current.raw[(std::size_t) k])
                engine.setParam (k, s.raw[(std::size_t) k]);
        current = s;
        inBlocks (left, right, n, [this] (float* l, float* r, int m) { engine.process (l, r, m); });
    }

private:
    Delay1Engine engine;
    Delay1State current {};
    float hostTempo = 140.0f;
    int rate = 44100;
    bool ready = false;
};
} // namespace

std::unique_ptr<Effect> makePhaseInverter() { return std::make_unique<PhaseInverterEffect>(); }
std::unique_ptr<Effect> makeCenter() { return std::make_unique<CenterEffect>(); }
std::unique_ptr<Effect> makeBassBoost() { return std::make_unique<BassBoostEffect>(); }
std::unique_ptr<Effect> makeEq7() { return std::make_unique<Eq7Effect>(); }
std::unique_ptr<Effect> makeDelay1() { return std::make_unique<Delay1Effect>(); }
} // namespace lynx::fllegacy
