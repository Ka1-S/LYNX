#include "core/PoiZone.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

// 1:1 port of our round-44 reference model (r44/py/pzmodel.py + pzfx.py; docs/analise/REA_rodada44.md). The model
// emulates the DLL's float32 arithmetic with numpy float32 scalars and uses Python floats (doubles) where the DLL
// computes in double; this file keeps those types — `float` where the model holds float32, `double` + an explicit
// (float) where it rounds a double — and the operation order of every expression (taken from the disassembly, which
// often differs from the decompiler's rendering). Fusing a·b + c into one FMA would change the results: contraction
// stays off. Clean-room: behaviour described in our own words; no Image-Line code.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int voiceCount = 1024;   // voices in the engine's list (§2.1)
constexpr int chunkMax = 64;       // engine buffers of 68 floats, used 64 at a time (§1.2)
constexpr int paramCount = 156;
constexpr float tinyF = 9.99999972e-10f;

inline float fl (float x) noexcept { return (x < tinyF && x > -tinyF) ? 0.0f : x; } // the DLL's denormal guard
inline std::uint32_t lcg (std::uint32_t s) noexcept { return s * 0x0bb38435u + 0x3619636bu; }
// the DLL's powf behaves as the correctly rounded power of the two floats (EXEC, §1.3)
inline float powf32 (float a, float b) noexcept { return (float) std::pow ((double) a, (double) b); }
inline float clampf (float x, float lo, float hi) noexcept { return x <= lo ? lo : (x >= hi ? hi : x); }
inline float u01 (std::uint32_t r) noexcept { return (float) r * 2.32830644e-10f; }

// cvttss2si / cvttsd2si: truncation toward zero; out of range or NaN → INT_MIN
int i32 (double x) noexcept
{
    if (! (x == x) || x >= 2147483648.0 || x < -2147483648.0)
        return std::numeric_limits<int>::min();
    return (int) x;
}

std::uint32_t le32 (const std::uint8_t* p) noexcept { return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24; }

// "SSLF" + u32 + tag + 156 × f32 (+ 64-byte name); tags "Ver4", "Ver3" (and the demo variants "EDV4"/"EDV3")
bool stateHeader (const std::vector<std::uint8_t>& s, std::size_t* count = nullptr)
{
    if (s.size() < 16 || std::memcmp (s.data(), "SSLF", 4) != 0)
        return false;
    const auto tag = le32 (s.data() + 8);
    if (tag != 0x56657234u && tag != 0x56657233u && tag != 0x56344445u && tag != 0x56334445u)
        return false;
    if (count != nullptr)
        *count = std::min<std::size_t> ((s.size() - 12) / 4, paramCount);
    return true;
}

// ------------------------------------------------------------------------------------------------- envelope (§3.2)
struct Env
{
    double rate = 0.0, peak = 0.0, relTarget = 0.0, start = 0.0, incA = 0.0, incD = 0.0, sus = 0.0, incR = 0.0;
    int stage = 0;
    double pos = 0.0, cur = 0.0, relFrom = 0.0, cA = 0.0, cD = 0.0, cR = 0.0;
    bool done = true;

    void init (double A, double D, double S, double ca, double cd, double cr, double r, double pk) noexcept
    {
        rate = r, peak = pk, relTarget = 0.0;
        start = done ? 0.0 : cur;
        if (A <= 0.001) A = 0.001;
        if (D <= 0.001) D = 0.001;
        cA = ca, cR = cr, done = false, stage = 0;
        sus = pk * S, pos = 0.0, cD = cd;
        incA = 1.0 / (A * rate), incD = 1.0 / (D * rate);
        setRelease (0.0);
    }
    void setRelease (double R) noexcept
    {
        if (stage != 2)
        {
            if (R <= 0.001) R = 0.001;
            incR = 1.0 / (R * rate);
        }
    }
    void release() noexcept
    {
        if (stage != 2)
            relFrom = cur, stage = 2, pos = 0.0;
    }
    static double curve (double t, double a, double b, double c) noexcept
    {
        if (c != 0.0)
        {
            const auto p = std::pow (10.0, t * c);
            t = 1.0 - p * (1.0 - t);
        }
        return (b - a) * t + a;
    }
    float tick (int n) noexcept
    {
        cur = 0.0;
        double v = 0.0;
        if (stage == 0)
        {
            const auto t = (double) n * incA + pos;
            pos = t;
            if (t >= 1.0)
                v = peak, cur = v, stage = 1, pos = 0.0;
            else
                v = curve (t, start, peak, cA), cur = v;
        }
        else if (stage == 1)
        {
            const auto t = (double) n * incD + pos;
            pos = t;
            if (t >= 1.0)
            {
                v = sus;
                if (v == 0.0 && relTarget == 0.0)
                    stage = 2, done = true, v = sus;
                cur = v, pos = 1.0;
            }
            else
                v = curve (t, peak, sus, cD), cur = v;
        }
        else if (stage == 2)
        {
            const auto t = (double) n * incR + pos;
            pos = t;
            if (t >= 1.0)
                done = true, v = relTarget, cur = v, pos = 1.0;
            else
                v = curve (t, relFrom, relTarget, cR), cur = v;
        }
        if (v < 0.0)
        {
            cur = 0.0;
            return 0.0f;
        }
        return (float) v;
    }
};

// ------------------------------------------------------------------------------------------------------ LFO (§3.3)
struct Lfo
{
    double rate = 1.0, phase = 0.0, rnd = 0.0, sync = -1.0;
    int shape = 2;
    bool retrig = false;

    float tick (double fs, double tempo, int n, std::uint32_t& rng3) noexcept
    {
        auto r = rate;
        if (sync > 0.0)
            r = (0.066666666666666666 / sync) * tempo;
        const auto old = phase;
        auto p = (r / fs) * (double) n + old;
        p = p - (double) i32 (p);
        phase = p;
        switch (shape)
        {
            case 0: return (float) p;
            case 1: return p < 0.5 ? 1.0f : 0.0f;
            case 2:
            {
                const auto d = p + p;
                return d > 1.0 ? (float) (2.0 - d) : (float) d;
            }
            case 3: return (float) std::cos (p * 6.2831853071795862);
            case 4:
                if (p < old)
                {
                    rng3 = lcg (rng3);
                    rnd = (double) rng3 * 2.3283064370807974e-10;
                }
                return (float) rnd;
            default: return 0.0f;
        }
    }
};

class Engine;

// ---------------------------------------------------------------------------------------------------- voice (§2)
struct Voice
{
    int note = -1, uniI = 0, uniN = 1;
    float drift = 0.0f, vel = 0.0f, x28 = 0.0f, grate = 0.0f, gpos = 0.0f;
    bool released = false;
    std::uint32_t counter = 0;
    float pA1 = 0, pA2 = 0, pB1 = 0, pB2 = 0, incA = 0, lvA = 0, dlvA = 0, incB = 0, lvB = 0, dlvB = 0;
    float xin[4] {}, y[4] {};
    float g = 0, k = 0, res = 0, m9c = 0, ma0 = 0, ma4 = 0, nlv = 0, dnlv = 0;
    Env amp, eg;
    float g130 = 0, d134 = 0, g138 = 0, d13c = 0, g140 = 0, g144 = 0, dc = 0;
    Lfo lfo;
};

// --------------------------------------------------------------------------------------------- 12 kHz shelf (§5.1)
struct Shelf
{
    float fs = 44100.0f, gain = 5.0f, f = 12000.0f, bw = 0.99f;
    float c[5] {}, st[4] {};
    bool dirty = true;

    void setRate (float r) noexcept
    {
        dirty = dirty || r != fs;
        fs = r;
        if (! dirty)
            return;
        dirty = false;
        const auto A = powf32 (10.0f, gain * 0.0500000007f);
        auto w = (float) (((double) f * 6.2831849999999996) / (double) fs);
        w = std::max (std::min (w, 3.0f), 0.0f);
        const auto sn = (float) std::sin ((double) w);
        const auto sh = std::sinh ((((double) bw * 0.34657359999999998) * (double) w) / (double) sn);
        const auto t = (float) (sh + sh);
        const auto sq = std::sqrt (A) * t;
        const auto cs = (float) std::cos ((double) w);
        const auto beta = sn * sq;
        const auto Am = A - 1.0f, Ap = A + 1.0f;
        const auto x4 = Am * cs, x2 = Ap * cs;
        const auto x3 = x4 + Ap, x5 = Ap - x4;
        const auto b0 = (beta + x3) * A, b2 = (x3 - beta) * A;
        const auto a0 = x5 + beta;
        const auto b1 = ((x2 + Am) * A) * -2.0f;
        auto a1 = Am - x2;
        a1 = a1 + a1;
        const auto a2 = x5 - beta;
        const auto inv = 1.0f / a0;
        c[0] = fl (inv * b0), c[1] = fl (inv * b1), c[2] = fl (inv * b2), c[3] = fl (inv * a1), c[4] = fl (inv * a2);
    }
    void reset() noexcept { st[0] = st[1] = st[2] = st[3] = 0.0f; }
    float tick (float x) noexcept
    {
        x = fl (x);
        const auto x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
        auto yv = (((x1 * c[1]) + (x * c[0])) + (c[2] * x2) - (y1 * c[3])) - (c[4] * y2);
        yv = fl (yv);
        st[0] = x, st[1] = x1, st[2] = yv, st[3] = y1;
        return yv;
    }
};

// ------------------------------------------------------------------------- tape-delay low/high-pass biquads (§5.3)
struct Biq
{
    bool hp = false;
    float f = 0, q = 0, fs = 0, b = 0, a1 = 0, a2 = 0, x2 = 0, x1 = 0, y2 = 0, y1 = 0;

    explicit Biq (bool highPass = false) noexcept : hp (highPass) { set (100.0f, 1.0f, 44100.0f); }
    void set (float nf, float nq, float nfs) noexcept
    {
        if (nf == f && nq == q && nfs == fs)
            return;
        f = nf, q = nq, fs = nfs;
        const auto w = (float) (((double) f * 6.2831853071795862) / (double) fs);
        // FL's DLL uses its CRT's low-accuracy vector sincosf here (§6): correctly rounded values are used instead
        const auto sn = (float) std::sin ((double) w), cs = (float) std::cos ((double) w);
        const auto r = 1.0f / (sn + q);
        const auto bb = hp ? (cs + 1.0f) / ((sn / q) + 2.0f) : (1.0f - cs) / ((sn / q) + 2.0f);
        const auto aa1 = (cs * r) * (q * -2.0f);
        const auto aa2 = (q - sn) * r;
        b = fl (bb), a1 = fl (aa1), a2 = fl (aa2);
    }
    void reset() noexcept { x2 = x1 = y2 = y1 = 0.0f; }
    float tick (float x) noexcept
    {
        x = fl (x);
        float yv;
        if (hp)
            yv = ((((x - (x1 + x1)) + x2) * b) - (y1 * a1)) - (a2 * y2);
        else
            yv = (((((x1 + x1) + x) + x2) * b) - (y1 * a1)) - (a2 * y2);
        yv = fl (yv);
        x2 = x1, y2 = y1, x1 = x, y1 = yv;
        return yv;
    }
};

// ----------------------------------------------------------------------------------------- tape line (§5.2)
struct TapeBuf
{
    std::vector<float> f;
    std::vector<double> d;
    int cnt = 0, ri = 0;
    void alloc (int n) { f.assign ((std::size_t) n + 256, 0.0f), d.assign ((std::size_t) n + 256, 0.0); }
    void clear() noexcept
    {
        std::fill (f.begin(), f.end(), 0.0f);
        std::fill (d.begin(), d.end(), 0.0);
        cnt = 0, ri = 0;
    }
};

struct Tape
{
    int size = 4;
    Biq lp { false }, hp { true };
    TapeBuf b1, b2;
    TapeBuf* W = &b1;
    TapeBuf* R = &b2;
    double phase0 = 0.0, minSpd = 0.25, ph = 0.0, inc = 0.0, tgt48 = 0.0, spd = 0.0, wpos = 0.0;
    float lvl = 0, fb = 0, tgtLvl = 0, depth = 0;
    bool lpOn = false, hpOn = false;

    void init (int sz, double p0)
    {
        size = std::max (sz, 4);
        b1.alloc (size), b2.alloc (size);
        W = &b1, R = &b2;
        lp = Biq (false), hp = Biq (true); // fresh filters, as the DLL's new line objects
        phase0 = p0, minSpd = 1.0 / (double) size;
        setTime (0.5);
        tgtLvl = 1.0f, fb = 0.0f;
        setLfo (0.0, 44100.0);
        lp.set (1000.0f, 1.0f, 44100.0f), hp.set (1000.0f, 1.0f, 44100.0f);
        depth = 0.0f, lpOn = hpOn = false;
        reset();
    }
    void setTime (double x) noexcept
    {
        x = std::max (std::min (x, 0.98999999999999999), 0.0);
        auto dd = 1.0 / (double) size;
        dd = dd + dd;
        tgt48 = 1.0 / ((x * (1.0 - dd) + dd) * (double) size);
    }
    void setLfo (double r, double fs) noexcept { inc = std::max (r, 0.0) / fs; }
    void setDepth (double v) noexcept { depth = (float) std::max (std::min (v, 1.0), 0.0); }
    void setLp (float fr, double fs) noexcept
    {
        if (fr <= 10.0f) fr = 10.0f;
        if (fs * 0.47619049781574513 < (double) fr) fr = (float) (fs * 0.47619049781574513);
        lp.set (fr, 1.0f, (float) fs);
    }
    void setHp (float fr, double fs) noexcept
    {
        if (fr <= 10.0f) fr = 10.0f;
        if (fs * 0.47619049781574513 < (double) fr) fr = (float) (fs * 0.47619049781574513);
        hp.set (fr, 1.0f, (float) fs);
    }
    void reset() noexcept
    {
        wpos = phase0, spd = tgt48, lvl = 0.0f, ph = 0.0;
        W->clear(), R->clear();
        lp.reset(), hp.reset();
    }
    float tick (float x) noexcept
    {
        x = fl (x);
        const auto t = ph;
        const auto tri = t >= 0.5 ? 1.0 - (((t - 0.5) + t) - 0.5) : t + t;
        const auto nph = t + inc;
        auto m = 3.0 - (tri + tri);
        m = m * tri;
        m = m * ((double) depth * tri);
        m = std::max (m + 1.0, 1.0);
        ph = nph;
        if (nph >= 1.0)
            ph = nph - (double) i32 (nph);
        float v = 0.0f;
        while (R->cnt != 0)
        {
            if (R->ri >= R->cnt + 1)
                break;
            const auto i = (std::size_t) R->ri;
            const auto pos = R->d[i + 10];
            if (wpos > pos)
            {
                R->ri += 1;
                continue;
            }
            const auto p0 = (float) R->d[i + 9], p1 = (float) pos;
            double fr = 0.0;
            if (p0 < p1)
                fr = (wpos - (double) p0) / (double) (p1 - p0);
            const auto df = R->f[i + 10] - R->f[i + 9];
            v = (float) ((double) df * fr + (double) R->f[i + 9]);
            if (! (v >= tinyF || v <= -tinyF))
                v = 0.0f;
            break;
        }
        if (lpOn) v = lp.tick (v);
        if (hpOn) v = hp.tick (v);
        auto sp = m * spd;
        const auto ret = v * lvl;
        const auto wv = (v * fb) + x;
        sp = std::min (sp, 0.20000000000000001);
        sp = std::max (sp, minSpd);
        wpos = sp + wpos;
        if (wpos >= 1.0)
        {
            const auto c = (std::size_t) R->cnt;
            W->f[9] = R->f[9 + c], W->d[9] = R->d[9 + c] - 1.0;
            W->f[8] = R->f[8 + c], W->d[8] = R->d[8 + c] - 1.0;
            W->f[7] = R->f[7 + c], W->d[7] = R->d[7 + c] - 1.0;
            W->f[10 + (std::size_t) W->cnt] = wv, W->d[10 + (std::size_t) W->cnt] = wpos;
            W->ri = 0;
            std::swap (W, R);
            wpos = wpos - 1.0;
            W->cnt = 0;
        }
        W->f[10 + (std::size_t) W->cnt] = wv, W->d[10 + (std::size_t) W->cnt] = wpos, W->cnt += 1;
        lvl = fl ((float) ((double) (tgtLvl - lvl) * 0.01 + (double) lvl));
        spd = (tgt48 - spd) * 0.0050000000000000001 + spd;
        return ret;
    }
};

// ------------------------------------------------------------------------------------------------- chorus (§5.4)
struct Chorus
{
    bool on = false;
    float rate = 0.0f, depth = 0.0f, wet = -0.0f; // wet kept negated, as the DLL does (§5.4)
    double fs = 0.0;
    bool hasLines = false;
    Tape lines[3];

    void setRate (double r)
    {
        fs = r;
        const auto d = r * 0.019999999552965164;
        lines[0].init ((int) (long long) (d * 1.1000000000000001), 0.0);
        lines[1].init ((int) (long long) (d * 0.85399999999999998), 0.46700000000000003);
        lines[2].init ((int) (long long) d, 0.77800000000000002);
        hasLines = true;
        setWet (wet), setRateParam (rate);
        for (auto& t : lines)
            t.setDepth (1.0);
        reset();
    }
    void reset() noexcept
    {
        if (hasLines)
            for (auto& t : lines)
                t.reset();
    }
    void setRateParam (float v) noexcept
    {
        rate = v;
        const auto r = (double) v * 5.0 + 0.10000000000000001;
        if (hasLines)
        {
            lines[0].setLfo (r * 0.89600000000000002, fs);
            lines[1].setLfo (r, fs);
            lines[2].setLfo (r * 0.68899999999999995, fs);
        }
    }
    void setDepth (float v) noexcept
    {
        depth = v;
        auto d = (double) v;
        d = d * d * d;
        if (hasLines)
            for (auto& t : lines)
                t.setDepth (d);
    }
    void setWet (float v) noexcept
    {
        wet = -v;
        if (hasLines)
            for (auto& t : lines)
                t.tgtLvl = (float) (double) v;
    }
    void run (float* L, float* R, int n) noexcept
    {
        if (! on)
            return;
        for (int i = 0; i < n; ++i)
        {
            const auto m = R[i] + L[i];
            const auto d1 = lines[0].tick (m);
            const auto d3 = lines[2].tick (m);
            const auto d2 = lines[1].tick (m);
            L[i] = (d2 + d1) + L[i];
            R[i] = (d2 + d3) + R[i];
        }
    }
};

// -------------------------------------------------------------------------------------------------- delay (§5.3)
struct Delay
{
    bool on = false, hasLines = false;
    float tL = 0.5f, tR = 0.5f, fbp = 0.0f, mod = 0.0f, wet = 0.0f, lpf = 1000.0f, hpf = 10.0f;
    double tempo = 120.0, fs = 0.0;
    Tape lines[2];

    void setRate (double r)
    {
        fs = r;
        lines[0].init ((int) (long long) (r + r), 0.0);
        lines[1].init ((int) (long long) (r + r), 0.69999999999999996);
        hasLines = true;
        lines[0].setLfo (1.0, r), lines[1].setLfo (0.876, r);
        setWet (wet), setMod (mod), setFb (fbp), setTime (0, tL), setTime (1, tR);
        for (auto& t : lines)
        {
            t.lpOn = true, t.lp.reset();
            t.hpOn = true, t.hp.reset();
        }
        for (auto& t : lines)
            t.setLp (lpf, r), t.setHp (hpf, r);
        reset();
    }
    void reset() noexcept
    {
        if (hasLines)
            for (auto& t : lines)
                t.reset();
    }
    void setTime (int ch, float v) noexcept
    {
        (ch == 0 ? tL : tR) = v;
        if (hasLines)
        {
            auto x = 2.0 / ((0.26666666666666666 / ((double) i32 ((double) v * 7.0) + 1.0)) * tempo);
            x = std::min (std::max (x, 0.0), 1.0);
            lines[ch].setTime (x);
        }
    }
    void setFb (float v) noexcept
    {
        fbp = v;
        if (hasLines)
            for (auto& t : lines)
                t.fb = (float) ((double) v * 0.83999999999999997);
    }
    void setMod (float v) noexcept
    {
        mod = v;
        if (hasLines)
            for (auto& t : lines)
                t.setDepth ((double) (float) ((double) (v * v) * 0.01));
    }
    void setWet (float v) noexcept
    {
        wet = v;
        if (hasLines)
            for (auto& t : lines)
                t.tgtLvl = v * v;
    }
    void setHpf (float v) noexcept
    {
        hpf = powf32 (2.0f, v * 10.0f) * 10.0f;
        if (hasLines)
            for (auto& t : lines)
                t.setHp (hpf, fs);
    }
    void setLpf (float v) noexcept
    {
        lpf = powf32 (2.0f, v * 6.5999999f) * 200.0f;
        if (hasLines)
            for (auto& t : lines)
                t.setLp (lpf, fs);
    }
    void run (float* L, float* R, int n) noexcept
    {
        if (! on)
            return;
        for (int i = 0; i < n; ++i)
        {
            L[i] = lines[0].tick (L[i]) + L[i];
            R[i] = lines[1].tick (R[i]) + R[i];
        }
    }
};

// --------------------------------------------------------------------------------------------------- gate (§4.2)
struct Gate
{
    float mix = 0.0f, smooth = 0.0f, steps[16], g = 0.0f;
    double div = 0.5, ph = 0.0;
    int step = 0;

    Gate() noexcept
    {
        std::fill (std::begin (steps), std::end (steps), 1.0f);
        setSmooth (0.0f);
        reset();
    }
    void setSmooth (float v) noexcept
    {
        v = clampf (v, 0.0f, 1.0f);
        smooth = (((1.0f - v) * (1.0f - v)) * 0.00999999978f) + 9.99999975e-05f;
    }
    void reset() noexcept
    {
        ph = 0.0, step = 0;
        g = 1.0f - steps[0] * mix;
    }
};

// ------------------------------------------------------------------------------------------------- engine (§1–§4)
class Engine
{
public:
    // three process-global LCGs of the DLL, started as in a freshly loaded DLL (§2.2)
    std::uint32_t rng1 = 22222, rng2 = 2222, rng3 = 2222;
    std::vector<Voice> voices; // [0, 1024) the engine's list in order; [1024] the padding voice
    std::vector<int> active, inactive;
    bool listDirty = true;
    double fs = 44100.0, fs8 = 352800.0, tempo = 120.0;
    float ampc = 0.0f;
    float hostTL[128] {}, hostTR[128] {}, hostT[128] {}, hostFC[128] {}, hostFR[128] {};
    // synthesizer section
    float pitchOfs = 0.0f, glideT = 1e-4f, velCurve = 1.0f, pbRange = 0.0f, driftAmt = 0.01f, uniDetune = 0.1f, uniPan = 0.0f;
    float gain = 1.0f, cc7 = 1.0f, cc10 = 0.5f, x3904 = 0.5f, mw = 0.0f, pb = 0.0f, sustain = 0.0f;
    int glideMode = 1, poly = 0, maxVoices = 64, npoly = 0, unison = 1, lastNote = -1;
    bool octaver = false, mono = false;
    std::uint32_t counter = 0;
    int held[128] {};
    float velTab[128] {};
    float mgL = 0.0f, mgR = 0.0f;
    // PoiZone section
    float shapeA = 0, pwA = 0, shapeB = 0, pwB = 0, detB = 0, bal = 0, noise = 0, egamt = 0, egA = 0, egD = 0, egS = 0, egR = 0;
    float cut = 0, reso = 0, kbd = 0, veltrk = 0, ampA = 0, ampD = 0, ampS = 0, ampR = 0, lfoamt = 0, mwamt = 0, fine = 0, transp = 0;
    int semB = 0, egDest = 0, fmode = 0, lfoDest = 0, mwDest = 0;
    bool syncOn = false, ring = false;
    Lfo lfo;
    float sPwA = 0, sPwB = 0, sBal = 0, sNoise = 0, sLfoAmt = 0, sEgAmt = 0, sMwAmt = 0, sCut = 0;
    float dec[3] {}, dstL[4] {}, dstR[4] {};
    float P[160] {};
    // effects
    Shelf shL, shR;
    Chorus chorus;
    Delay delay;
    Gate gate;
    bool playing = false;
    double songpos = 0.0;
    // arpeggiator (§4.3)
    bool arpOn = false, arpStopped = true, arpToggle = false, arpPending = true;
    int arpMode = 0, arpRange = 1, arpCount = 0, arpIdx = -2, arpDir = 1;
    double arpDiv = 1.0, arpSwing = 0.0, arpPhase = 0.0;
    int arpCur[8] {};
    int arpNote[128];
    float arpVel[128];
    float bufL[chunkMax * 8] {}, bufR[chunkMax * 8] {};

    Engine()
    {
        voices.resize (voiceCount + 1);
        for (auto& v : voices) // the constructor gives every voice two random start phases (§2.2)
        {
            rng1 = lcg (rng1);
            const auto a = clampf (u01 (rng1), 0.0f, 1.0f);
            rng1 = lcg (rng1);
            const auto b = clampf (u01 (rng1), 0.0f, 1.0f);
            v.pA1 = v.pA2 = a, v.pB1 = v.pB2 = b;
            resetVoice (v, 0.0f);
        }
        std::fill (std::begin (arpNote), std::end (arpNote), -1);
        std::fill (std::begin (arpVel), std::end (arpVel), 1.0f);
        arpReset();
        active.reserve (voiceCount + 1), inactive.reserve (voiceCount + 1);
    }

    // constructor tail and the wrapper's first rate set (§1.1)
    void construct()
    {
        for (int i = 0x51; i < 0x9b; ++i)
            setParam (i, -1.0f);
        for (int i = 0; i < 0x51; ++i)
            setParam (i, defaultParam (i));
        setRate (44100.0);
    }

    static float defaultParam (int i) noexcept
    {
        switch (i)
        {
            case 1: case 5: case 12: case 13: case 14: case 18: case 23: case 24: case 37: case 49: case 51: case 53: case 54:
            case 55: case 57: case 59: case 61: case 62: case 63: case 77:
                return 1.0f;
            case 6: case 10: case 15: case 16: case 17: case 26: case 34: case 38: case 40: case 41: case 43: case 68:
                return 0.5f;
            case 28: return 0.20000000298023224f;
            case 29: return 0.10000000149011612f;
            case 32: case 33: return 0.4285714328289032f;
            case 35: return 0.30000001192092896f;
            case 42: return 0.4000000059604645f;
            case 76: return 12.0f;
            default: return 0.0f;
        }
    }

    // ---- parameters (§1.3)
    static float x4curve (float v, float lo, float hi) noexcept
    {
        auto t = (((v * v) * (v * v)) * (hi - lo)) + lo;
        t = t <= lo ? lo : t;
        t = t >= hi ? hi : t;
        return t * 0.00100000005f;
    }
    static int sel (float v, float scale, float bias, int hi) noexcept { return std::min (std::max (i32 ((double) ((v * scale) + bias)), 0), hi); }

    void setParam (int i, float v)
    {
        if (i >= 0 && i < 160)
            P[i] = v;
        if (i >= 0x51 && i < 0x51 + 0x4a)
            return;
        switch (i)
        {
            case 0: shapeA = v != 0.0f ? 1.0f : 0.0f; break;
            case 1: pwA = v; break;
            case 2: syncOn = v != 0.0f; break;
            case 3: ring = v != 0.0f; break;
            case 4: shapeB = v != 0.0f ? 1.0f : 0.0f; break;
            case 5: pwB = v; break;
            case 6: semB = -24 - i32 ((double) (v * -48.0f)); break;
            case 7: detB = v * 0.5f; break;
            case 8: bal = v; break;
            case 9: noise = v; break;
            case 10: egamt = (v + v) - 1.0f; break;
            case 11: egA = x4curve (v, 5.0f, 50000.0f); break;
            case 12: egD = x4curve (v, 5.0f, 50000.0f); break;
            case 13: egS = v; break;
            case 14: egR = x4curve (v, 5.0f, 50000.0f); break;
            case 15: gain = powf32 (10.0f, (v * 1.5f) - 1.0f); break;
            case 16: fine = (v + v) - 1.0f, pitchOfs = fine + transp; break;
            case 17: transp = (float) (-24 - i32 ((double) (v * -48.0f))), pitchOfs = transp + fine; break;
            case 18: cut = v; break;
            case 19: reso = v; break;
            case 20: kbd = v; break;
            case 21: veltrk = v; break;
            case 22: ampA = x4curve (v, 1.0f, 5000.0f); break;
            case 23: ampD = x4curve (v, 5.0f, 20000.0f); break;
            case 24: ampS = clampf (v * v, 0.0f, 1.0f); break;
            case 25: ampR = x4curve (v, 15.0f, 10000.0f); break;
            case 26: lfoamt = (v + v) - 1.0f; break;
            case 27: lfo.retrig = v != 0.0f; break;
            case 28:
            {
                auto t = (((v * v) * v) * (v * v)) * 30.0f;
                t = std::min (std::max (t, 0.0f), 30.0f);
                lfo.rate = std::max ((double) t, 0.0);
                break;
            }
            case 29: uniDetune = v * 1.5f; break;
            case 30: uniPan = clampf (v, 0.0f, 1.0f); break;
            case 31: delay.reset(), delay.on = v != 0.0f; break;
            case 32: delay.setTime (0, v); break;
            case 33: delay.setTime (1, v); break;
            case 34: delay.setFb (v); break;
            case 35: delay.setMod (v); break;
            case 36: delay.setHpf (v); break;
            case 37: delay.setLpf (v); break;
            case 38: delay.setWet (v); break;
            case 39: chorus.on = v != 0.0f; break;
            case 40: chorus.setRateParam (v); break;
            case 41: chorus.setDepth (v); break;
            case 42: chorus.setWet (v); break;
            case 43: mwamt = (v + v) - 1.0f; break;
            case 44: setMono (v != 0.0f); break;
            case 45: octaver = v != 0.0f; break;
            case 46:
            {
                auto t = clampf ((v * v) * v, 0.0f, 1.0f);
                t = t + t;
                glideT = (double) t < 0.0001 ? 9.99999975e-05f : t;
                break;
            }
            case 47: gate.mix = clampf (v, 0.0f, 1.0f); break;
            case 48: gate.setSmooth (v); break;
            case 65: egDest = sel (v, 4.0f, 0.00039999999f, 4); break;
            case 66: fmode = sel (v, 2.0f, 0.000199999995f, 2); break; // (v + v) + bias
            case 67: lfoDest = sel (v, 5.0f, 0.000499999966f, 5); break;
            case 68: lfo.shape = sel (v, 4.0f, 0.00039999999f, 4); break;
            case 69:
            {
                const auto m = std::min (std::max (i32 ((double) ((v * 4.0f) + 0.00039999999f)), 0), 4);
                setArp (m != 0, m - 1);
                break;
            }
            case 70: arpRange = std::min (std::max (sel (v, 3.0f, 0.000299999985f, 3) + 1, 1), 4); break;
            case 71: mwDest = sel (v, 4.0f, 0.00039999999f, 4); break;
            case 72:
            case 73:
            {
                const auto a = P[72], b = P[73];
                glideMode = a == 0.0f ? (b == 0.0f ? 0 : 2) : (b == 0.0f ? 1 : 3);
                break;
            }
            case 75:
            {
                static const int u[] = { 1, 2, 3, 4, 8, 12, 16 };
                const auto k = i32 ((double) v);
                if (k >= 0 && k <= 6)
                    unison = std::min (u[k], 16);
                break;
            }
            case 76:
            {
                const auto p = i32 ((double) v) + 1;
                poly = (p >= 1 && p <= maxVoices) ? p : maxVoices;
                break;
            }
            case 77:
            case 79:
            {
                static const double d[] = { 0.5, 1.0, 2.0, 3.0, 4.0, 8.0, 16.0 };
                const auto k = i32 ((double) v);
                if (k >= 0 && k <= 6)
                {
                    if (i == 77)
                        arpDiv = std::max (d[k], 0.25);
                    else
                        gate.div = d[k];
                }
                break;
            }
            case 78: pbRange = (float) (i32 ((double) v) + 1); break;
            case 80:
            {
                static const double d[] = { -1.0, 1.0, 2.0, 3.0, 4.0, 8.0, 12.0, 16.0, 32.0, 64.0, 128.0 };
                lfo.sync = d[std::min (std::max (i32 ((double) v), 0), 10)];
                break;
            }
            case 157: mw = v; break;
            case 158: pb = (v + v) - 1.0f; break;
            default:
                if (i >= 49 && i <= 64)
                    gate.steps[i - 49] = 1.0f - (float) i32 ((double) v);
                break;
        }
    }

    void allNotesOffRaw()
    {
        const auto sus = sustain;
        sustain = 0.0f;
        for (int k = 0; k < 128; ++k)
            noteOff (k);
        sustain = sus;
    }
    void setMono (bool m)
    {
        if (mono != m)
        {
            mono = m;
            allNotesOffRaw();
        }
    }
    void setArp (bool on, int mode)
    {
        if (arpOn != on)
        {
            allNotesOffRaw();
            arpOn = on;
        }
        if (on)
            arpMode = std::min (std::max (mode, 0), 3);
    }

    void loadState (const std::vector<std::uint8_t>& s)
    {
        std::size_t n = 0;
        if (! stateHeader (s, &n))
            return;
        for (std::size_t i = 0; i < n; ++i)
        {
            float v;
            std::memcpy (&v, s.data() + 12 + 4 * i, 4);
            setParam ((int) i, v);
        }
    }

    // ---- voices (§2)
    void resetVoice (Voice& v, float) noexcept
    {
        std::fill (std::begin (v.xin), std::end (v.xin), 0.0f);
        std::fill (std::begin (v.y), std::end (v.y), 0.0f);
        v.g = v.k = v.res = v.m9c = v.ma0 = v.ma4 = v.nlv = 0.0f;
        v.incA = v.lvA = v.incB = v.lvB = 0.0f;
        killVoice (v);
    }
    void killVoice (Voice& v) noexcept
    {
        v.g130 = v.d134 = v.g138 = v.d13c = v.g140 = v.g144 = 0.0f;
        v.dlvA = v.dlvB = v.dnlv = 0.0f;
        v.amp.done = true, v.eg.done = true;
        v.note = -1;
        listDirty = true;
    }

    // ---- sample rate (§1.1)
    void setRate (double r)
    {
        fs = r;
        allNotesOffRaw();
        for (auto& v : voices)
            resetVoice (v, (float) fs), v.gpos = 48.0f;
        mgL = masterGain (false), mgR = masterGain (true);
        lastNote = -1;
        arpReset();
        if (! arpStopped)
            arpStopped = true, arpAllNotesOff(), arpReset();
        delay.setRate (fs);
        chorus.setRate (fs);
        shL.setRate ((float) fs), shR.setRate ((float) fs);
        shL.reset(), shR.reset(), delay.reset(), chorus.reset();
        fs8 = fs * 8.0;
        ampc = (float) (fs * 6.8027210884353747e-08);
        const auto w = (fs * 0.26179938779914941) / fs;
        const auto sn = std::sin (w), cs = std::cos (w);
        const auto r1 = 1.0 / (sn + 2.0);
        dec[0] = fl ((float) ((1.0 - cs) / (sn * 0.5 + 2.0)));
        dec[1] = fl ((float) ((cs * -4.0) * r1));
        dec[2] = fl ((float) ((2.0 - sn) * r1));
        lfo.phase = 0.0;
        sPwA = pwA, sPwB = pwB, sBal = bal, sNoise = noise, sLfoAmt = lfoamt, sEgAmt = egamt, sMwAmt = mwamt, sCut = cut;
        std::fill (std::begin (dstL), std::end (dstL), 0.0f);
        std::fill (std::begin (dstR), std::end (dstR), 0.0f);
        shL.reset(), shR.reset(), delay.reset(), chorus.reset();
    }

    float masterGain (bool right) const noexcept
    {
        auto g = (gain * cc7) * 0.5f;
        auto f = (((cc10 - 0.5f) + cc10) - 0.5f) + (x3904 + x3904);
        f = clampf (f, 0.0f, 2.0f);
        if (g <= 0.0f)
            return 0.0f;
        if (! right)
        {
            if (1.0f < f)
                return g * (2.0f - f);
        }
        else if (f < 1.0f)
            g = g * f;
        return g;
    }

    // ---- notes (§2.3)
    int maxAlloc() const noexcept
    {
        auto n = npoly <= poly ? npoly : poly;
        return std::min (n * unison, voiceCount);
    }

    void noteOn (int note, int vel7)
    {
        const auto v = (float) vel7 * 0.007874015718698502f;
        velTab[note] = v;
        if (vel7 == 0)
        {
            noteOffRaw (note);
            return;
        }
        held[note] = i32 ((double) (v * 127.0f));
        if (arpOn)
        {
            arpEvent();
            return;
        }
        voiceEvent (note, v);
    }
    void noteOff (int note)
    {
        velTab[note] = 0.0f;
        noteOffRaw (note);
    }
    void noteOffRaw (int note)
    {
        if (sustain != 0.0f)
            return;
        held[note] = 0;
        if (arpOn)
        {
            arpEvent();
            return;
        }
        voiceEvent (note, 0.0f);
    }
    void voiceEvent (int note, float vel)
    {
        held[note] = i32 ((double) (vel * 127.0f));
        if (poly == 0)
            return;
        if (mono)
            monoEvent (note, vel);
        else
            polyEvent (note, vel);
    }

    void startVoice (Voice& v)
    {
        v.amp.init ((double) ampA, (double) ampD, (double) ampS, 0.0, -2.5, -2.5, fs8, (double) v.vel);
        v.eg.done = true;
        v.eg.init ((double) egA, (double) egD, (double) egS, -2.0, -2.0, -2.0, fs8, 1.0);
        v.lfo = lfo;
        if (v.lfo.retrig)
            v.lfo.phase = 0.0;
        listDirty = true;
    }
    void releaseVoice (Voice& v)
    {
        v.amp.setRelease ((double) ampR), v.eg.setRelease ((double) egR);
        v.amp.release(), v.eg.release();
    }
    float driftDraw()
    {
        rng2 = lcg (rng2);
        return u01 (rng2) - 1.0f;
    }

    void polyEvent (int note, float vel)
    {
        if (vel == 0.0f)
        {
            for (int j = 0; j < voiceCount; ++j)
                if (voices[(std::size_t) j].note == note)
                    voices[(std::size_t) j].released = true, releaseVoice (voices[(std::size_t) j]);
            return;
        }
        const auto r = driftDraw();
        auto frm = note;
        if (0.0 <= (double) (float) lastNote)
            frm = lastNote;
        auto from = (float) frm;
        if (glideMode == 1 || glideMode == 3)
        {
            from = (float) note;
            int best = -1, bd = 0x80;
            for (int k = 0; k < 128; ++k)
                if (held[k] != 0 && k != note && std::abs (note - k) < bd)
                    bd = std::abs (note - k), best = k;
            if (best >= 0)
                from = (float) best;
        }
        const auto lim = maxAlloc();
        for (int u = 0; u < unison; ++u)
        {
            Voice* chosen = nullptr;
            Voice* oldHeld = &voices[0];
            Voice* oldRel = &voices[0];
            for (int j = 0; j < lim; ++j)
            {
                auto& v = voices[(std::size_t) j];
                if (v.note < 0)
                {
                    chosen = &v;
                    break;
                }
                if (! v.released)
                {
                    if (v.counter < oldHeld->counter)
                        oldHeld = &v;
                }
                else if (v.counter < oldRel->counter)
                    oldRel = &v;
            }
            if (chosen == nullptr)
                chosen = oldRel->released ? oldRel : oldHeld;
            auto& v = *chosen;
            v.counter = counter++;
            v.note = note;
            v.uniI = u, v.uniN = unison;
            v.drift = (r + r) * driftAmt;
            v.vel = powf32 (vel, velCurve);
            v.gpos = from;
            v.released = false;
            if ((double) glideT < 0.001)
                v.gpos = (float) v.note;
            if (glideMode == 0 || glideMode == 1)
                v.grate = (float) (12.0 / (fs * (double) glideT));
            else
                v.grate = (float) ((double) std::fabs ((float) v.note - v.gpos) / (fs * (double) glideT));
            startVoice (v);
        }
        lastNote = note;
    }

    void monoEvent (int note, float vel)
    {
        if (vel == 0.0f)
        {
            int best = -1, bd = 0x80;
            for (int k = 0; k < 128; ++k)
                if (held[k] != 0 && std::abs (note - k) < bd)
                    bd = std::abs (note - k), best = k;
            for (int j = 0; j < voiceCount; ++j)
            {
                auto& v = voices[(std::size_t) j];
                if (v.note != note)
                    continue;
                if (best >= 0)
                {
                    v.counter = counter++;
                    v.note = best;
                    v.grate = (float) (12.0 / (fs * (double) glideT));
                    lastNote = best;
                }
                else
                    v.released = true, releaseVoice (v);
            }
            return;
        }
        const auto r = driftDraw();
        auto frm = note;
        if (0.0 <= (double) (float) lastNote)
            frm = lastNote;
        auto from = (float) frm;
        const auto& head = voices[0];
        if ((glideMode == 1 || glideMode == 3) && (head.note < 0 || head.released))
            from = (float) note;
        for (int u = 0; u < unison; ++u)
        {
            auto& v = voices[(std::size_t) u];
            const bool wasOff = v.note < 0 || v.released;
            v.counter = counter++;
            v.note = note;
            v.drift = (r + r) * driftAmt;
            v.released = false;
            v.uniI = u, v.uniN = unison;
            if (glideMode == 0 || glideMode == 1)
                v.grate = (float) (12.0 / (fs * (double) glideT));
            else
                v.grate = (float) ((double) std::fabs ((float) v.note - v.gpos) / (fs * (double) glideT));
            if (wasOff)
            {
                v.vel = powf32 (vel, velCurve);
                v.gpos = from;
                if ((double) glideT < 0.001)
                    v.gpos = (float) v.note;
                startVoice (v);
            }
        }
        lastNote = note;
    }

    // ---- arpeggiator (§4.3)
    void arpReset() noexcept
    {
        arpPhase = 0.0, arpToggle = false, arpPending = true, arpIdx = -2, arpDir = 1;
        std::fill (std::begin (arpCur), std::end (arpCur), -1);
    }
    void arpAllNotesOff()
    {
        for (auto& c : arpCur)
            if (c >= 0)
            {
                voiceEvent (c, 0.0f);
                c = -1;
            }
    }
    void arpEvent()
    {
        int keys[128];
        int n = 0;
        for (int k = 0; k < 128; ++k)
            if (velTab[k] != 0.0f)
                keys[n++] = k;
        if (n == 0)
        {
            if (! arpStopped)
                arpStopped = true, arpAllNotesOff(), arpReset();
            return;
        }
        if (arpStopped)
            arpStopped = false, arpAllNotesOff(), arpReset();
        for (int i = 0; i < n; ++i)
            for (int o = 0; o < arpRange; ++o)
            {
                const auto idx = std::min (std::max (i + o * n, 0), 127);
                arpNote[idx] = keys[i] + 12 * o;
                arpVel[idx] = velTab[keys[i]];
            }
        arpCount = std::min (std::max (arpRange * n, 1), 128);
    }
    void arpTick (int n)
    {
        if (arpStopped)
            return;
        if (arpPending)
        {
            arpPending = false;
            const auto c = arpCount;
            switch (arpMode)
            {
                case 0:
                    if (c <= arpIdx || arpIdx < 0)
                        arpIdx = 0;
                    break;
                case 1:
                    if (arpIdx < 0)
                        arpIdx = c - 1;
                    break;
                case 2:
                {
                    auto i1 = arpIdx;
                    if (i1 < -1)
                        arpIdx = 0, arpDir = 1, i1 = 0;
                    else if (i1 < 0)
                        i1 = 1, arpIdx = 1, arpDir = 1;
                    const auto i2 = c - 1;
                    if (i2 < i1)
                        i1 = c - 2, arpDir = 0, arpIdx = i1;
                    if (i1 < 0)
                        arpIdx = 0, i1 = 0;
                    if (i2 < i1)
                        arpIdx = i2;
                    break;
                }
                default:
                {
                    rng2 = lcg (rng2);
                    auto i = i32 ((double) (u01 (rng2) * (float) c));
                    arpIdx = i;
                    if (i < 0)
                        arpIdx = 0, i = 0;
                    if (c <= i)
                        arpIdx = c - 1;
                    break;
                }
            }
            int note = -1;
            float vl = 1.0f;
            if (arpIdx >= 0 && arpIdx < 128)
                note = arpNote[arpIdx], vl = arpVel[arpIdx];
            for (int j = 0; j < 8; ++j)
            {
                const auto nn = j == 0 ? std::min (note, 0x7f) : -1;
                if (arpCur[j] >= 0)
                    voiceEvent (arpCur[j], 0.0f);
                arpCur[j] = nn;
                if (nn >= 0)
                    voiceEvent (nn, vl * 1.0f);
            }
            if (arpMode == 0)
                ++arpIdx;
            else if (arpMode == 1)
                --arpIdx;
            else if (arpMode == 2)
                arpIdx = arpDir != 0 ? arpIdx + 1 : arpIdx - 1;
        }
        const auto per = ! arpToggle ? arpSwing + 1.0 : 1.0 - arpSwing;
        auto ph = (double) n * (((0.066666666666666666 / arpDiv) * tempo) / fs) + arpPhase;
        arpPhase = ph;
        if (per <= ph)
        {
            auto t = arpToggle;
            while (per <= ph)
                t = ! t, ph = ph - per;
            arpPhase = ph, arpPending = true, arpToggle = t;
        }
    }

    // ---- per-voice control values (§3)
    float pitch (const Voice& v) const noexcept
    {
        const auto oct = (octaver && (v.uniI & 1) != 0) ? 12.0f : 0.0f;
        auto spread = 0.0f;
        if (v.uniN > 1)
            spread = (((float) v.uniI * uniDetune) / (float) (v.uniN - 1)) - uniDetune * 0.5f;
        auto t = pitchOfs + v.gpos;
        t = t + (spread + hostT[v.note]);
        t = t + oct;
        t = t + v.drift;
        t = t + pbRange * pb;
        t = t + v.x28;
        return t;
    }
    float freq (const Voice& v) const noexcept
    {
        const auto p = (pitch (v) - 57.0f) * 0.0833333358f;
        return (float) (std::pow (2.0, (double) p) * 220.0);
    }
    float panGain (const Voice& v, bool right) const noexcept
    {
        auto f = 0.0f;
        if (v.uniN > 1)
        {
            f = (float) v.uniI / (float) (v.uniN - 1);
            f = (f + f) - 1.0f;
        }
        f = f * uniPan;
        if (right)
            return (clampf (1.0f - f, 0.0f, 1.0f) * hostTR[v.note]) * 0.5f;
        return (clampf (f + 1.0f, 0.0f, 1.0f) * hostTL[v.note]) * 0.5f;
    }
    static void glide (Voice& v, int n) noexcept
    {
        auto g = v.gpos;
        const auto t = (float) v.note;
        if (g < t)
        {
            g = ((float) n * v.grate) + g;
            v.gpos = g;
            if (t < g)
            {
                v.gpos = t;
                return;
            }
        }
        if (g <= t)
            return;
        g = g - (float) n * v.grate;
        v.gpos = g;
        if (t <= g)
            return;
        v.gpos = t;
    }
    static float phase2 (float p1, float pw) noexcept
    {
        auto p2 = p1 - (float) (0.98999999999999999 - (double) pw * 0.48999999999999999);
        if (p2 < 0.0f)
            p2 = p2 + 1.0f;
        return p2;
    }

    void block (Voice& v, int n8)
    {
        glide (v, n8);
        if (v.amp.done)
        {
            killVoice (v);
            return;
        }
        const auto egv = v.eg.tick (n8);
        v.lfo.rate = lfo.rate, v.lfo.shape = lfo.shape, v.lfo.sync = lfo.sync;
        auto lv = v.lfo.tick (fs8, (double) (float) tempo, n8, rng3);
        if (mwDest == 1)
            lv = lv * (sMwAmt * mw);
        const auto av = v.amp.tick (n8) * 10.0f;
        auto pan = 0.5f;
        if (lfoDest == 5)
            pan = ((lv * sLfoAmt) + 1.0f) * 0.5f;
        pan = clampf (pan, 0.0f, 1.0f);
        const auto inv = 1.0f / (float) n8;
        const auto gl = panGain (v, false);
        const auto d = (((gl * av) * pan) - v.g130) * inv;
        v.d134 = d;
        const auto gr = panGain (v, true);
        const auto d2 = (((gr * av) * (1.0f - pan)) - v.g138) * inv;
        v.d13c = d2;
        v.g130 = fl (v.g130), v.d134 = fl (d), v.g138 = fl (v.g138), v.d13c = fl (d2), v.g140 = fl (v.g140), v.g144 = fl (v.g144);
        const auto nl = v.nlv;
        v.dnlv = fl ((sNoise - nl) * inv);
        v.nlv = fl (nl);
        oscA (v, n8, egv, lv);
        oscB (v, n8, egv, lv);
        filt (v, egv, lv);
    }

    void oscA (Voice& v, int n8, float egv, float lv) noexcept
    {
        auto f = freq (v);
        auto pw = sPwA;
        if (egDest == 1 || egDest == 2)
        {
            const auto x = (((float) i32 ((double) (sEgAmt * 24.0f))) * egv) * 0.0833333358f;
            f = (float) (std::pow (2.0, (double) x) * (double) f);
        }
        if (egDest == 3 || egDest == 4)
            pw = pw + egv * sEgAmt;
        if (lfoDest == 1 || lfoDest == 2)
        {
            const auto l = sLfoAmt;
            auto x = ((l * l) * (l * l)) * lv;
            x = l > 0.0f ? x * 2.0f : x * -2.0f;
            f = f * powf32 (2.0f, x);
        }
        if (lfoDest == 3 || lfoDest == 4)
            pw = pw + lv * sLfoAmt;
        if (mwDest == 2)
        {
            const auto m = sMwAmt;
            f = f * powf32 (2.0f, (((m * m) * m) * mw) * 2.0f);
        }
        else if (mwDest == 3 || mwDest == 4)
            pw = pw + sMwAmt * mw;
        f = f > 0.0f ? f : 0.0f;
        pw = clampf (pw, 0.0f, 1.0f);
        if ((double) f > fs8 * 0.5)
            f = (float) (fs8 * 0.5);
        const auto inc = (float) ((double) f / fs8);
        v.incA = inc;
        const auto lv0 = v.lvA;
        v.dlvA = fl ((((1.0f - sBal) * (1.0f - sNoise)) - lv0) / (float) n8);
        v.lvA = fl (lv0);
        const auto p1 = v.pA1;
        const auto p2 = phase2 (p1, pw);
        v.incA = fl (inc), v.pA1 = fl (p1), v.pA2 = fl (p2);
    }

    void oscB (Voice& v, int n8, float egv, float lv) noexcept
    {
        const auto x = (((float) semB) + detB) * 0.0833333358f;
        auto f = (float) ((double) freq (v) * std::pow (2.0, (double) x));
        auto pw = sPwB;
        if (egDest == 2)
        {
            const auto yv = (((float) i32 ((double) (sEgAmt * 24.0f))) * egv) * 0.0833333358f;
            f = (float) (std::pow (2.0, (double) yv) * (double) f);
        }
        else if (egDest == 4)
            pw = pw + egv * sEgAmt;
        if (lfoDest == 2)
        {
            const auto l = sLfoAmt;
            auto yv = ((l * l) * (l * l)) * lv;
            yv = l > 0.0f ? yv * 2.0f : yv * -2.0f;
            f = f * powf32 (2.0f, yv);
        }
        else if (lfoDest == 4)
            pw = pw + lv * sLfoAmt;
        if (mwDest == 4)
            pw = pw + sMwAmt * mw;
        f = f > 0.0f ? f : 0.0f;
        pw = clampf (pw, 0.0f, 1.0f);
        if ((double) f > fs8 * 0.5)
            f = (float) (fs8 * 0.5);
        const auto inc = (float) ((double) f / fs8);
        v.incB = inc;
        const auto lv0 = v.lvB;
        v.dlvB = fl ((((1.0f - sNoise) * sBal) - lv0) / (float) n8);
        v.lvB = fl (lv0);
        const auto p1 = v.pB1;
        const auto p2 = phase2 (p1, pw);
        v.incB = fl (inc), v.pB1 = fl (p1), v.pB2 = fl (p2);
    }

    void filt (Voice& v, float egv, float lv) noexcept
    {
        const auto p = pitch (v);
        auto x = (double) ((p - 56.0f) * kbd) * 0.0083999999999999995;
        x = x + (((double) v.vel - 0.5) * (double) veltrk) * 0.5;
        x = x + (double) hostFC[v.note];
        if (egDest == 0)
        {
            const auto a = sEgAmt;
            const auto t = (double) ((a * a) * egv);
            x = a > 0.0f ? x + t : x - t;
        }
        if (lfoDest == 0)
        {
            const auto a = sLfoAmt;
            const auto t = (double) ((a * a) * lv);
            x = a > 0.0f ? x + t : x - t;
        }
        if (mwDest == 0)
        {
            const auto a = sMwAmt;
            const auto t = (double) ((a * a) * mw);
            x = a > 0.0f ? x + t : x - t;
        }
        x = (double) sCut * 1.1000000000000001 + x;
        x = std::min (std::max (x, 0.0), 0.91000000000000003);
        const auto w = std::pow (2.0, x * 10.0 + 6.0) / fs8;
        const auto kk = (float) ((w * 3.6000000000000001 - (w * 1.6000000000000001) * w) - 1.0);
        v.k = kk;
        v.g = (kk + 1.0f) * 0.5f;
        const auto ex = std::exp (1.3862490000000001 - x * 1.5233505494505493) + 1.0;
        auto r = (double) (hostFR[v.note] + reso);
        r = std::min (std::max (r, 0.0), 1.0);
        v.res = (float) ((ex * r) * 2.0534400000000002);
        float a11 = 0.0f, a0 = 0.0f, a1 = 0.0f;
        if (fmode == 0)
            a11 = 1.0f;
        else if (fmode == 1)
            a0 = 1.0f;
        else if (fmode == 2)
            a1 = 1.0f;
        a0 = a0 * 3.0f;
        v.ma4 = a1;
        v.ma0 = -a0;
        v.m9c = (a11 - a1) + a0;
    }

    // ---- one engine chunk (§1.2)
    void process (float* outL, float* outR, int n)
    {
        const auto c = 0.0399999991f;
        sPwA = ((pwA - sPwA) * c) + sPwA;
        sPwB = ((pwB - sPwB) * c) + sPwB;
        sBal = ((bal - sBal) * c) + sBal;
        sNoise = ((noise - sNoise) * c) + sNoise;
        sLfoAmt = ((lfoamt - sLfoAmt) * c) + sLfoAmt;
        sEgAmt = ((egamt - sEgAmt) * c) + sEgAmt;
        sMwAmt = ((mwamt - sMwAmt) * c) + sMwAmt;
        sCut = ((cut - sCut) * c) + sCut;
        if (listDirty)
        {
            active.clear(), inactive.clear();
            for (int j = 0; j < voiceCount; ++j)
                (voices[(std::size_t) j].note >= 0 ? active : inactive).push_back (j);
            inactive.push_back (voiceCount);
            listDirty = false;
        }
        const auto n8 = n * 8;
        std::fill (outL, outL + n, 0.0f);
        std::fill (outR, outR + n, 0.0f);
        const auto cnt = (int) active.size();
        if (cnt > 0)
        {
            auto at = [this] (int k) -> Voice& {
                const auto na = (int) active.size();
                return voices[(std::size_t) (k < na ? active[(std::size_t) k] : inactive[(std::size_t) (k - na)])];
            };
            int k = 0, rem = cnt;
            bool first = true;
            while (rem > 0)
            {
                if (rem < 2)
                {
                    auto& v = at (k);
                    block (v, n8);
                    render1 (v, n8, first);
                    first = false, ++k, --rem;
                }
                else
                {
                    Voice* g4[4] = { &at (k), &at (k + 1), &at (k + 2), &at (k + 3) };
                    for (auto* v : g4)
                        if (v->note >= 0)
                            block (*v, n8);
                    render4 (g4, n8, first);
                    first = false, k += 4, rem -= 4;
                }
            }
            decimate (outL, outR, n);
        }
        runGate (outL, outR, n, cnt);
        for (int i = 0; i < n; ++i) // FX (§5): shelves, chorus, delay
            outL[i] = shL.tick (outL[i]), outR[i] = shR.tick (outR[i]);
        delay.tempo = tempo;
        delay.setTime (0, delay.tL), delay.setTime (1, delay.tR);
        chorus.run (outL, outR, n);
        delay.run (outL, outR, n);
        if (arpOn)
            arpTick (n);
        const auto tl = masterGain (false), tr = masterGain (true);
        const auto kk = 0.00499999989f;
        for (int i = 0; i < n; ++i)
        {
            outL[i] = mgL * outL[i], outR[i] = outR[i] * mgR;
            mgL = ((tl - mgL) * kk) + mgL, mgR = ((tr - mgR) * kk) + mgR;
        }
        lfo.tick (fs, tempo, n, rng3);
    }

    void runGate (float* L, float* R, int n, int nvoices) noexcept
    {
        auto& gt = gate;
        if (nvoices == 0)
        {
            gt.reset();
            return;
        }
        if (playing)
        {
            const auto d = (songpos * 4.0) / gt.div;
            const auto u = (std::uint32_t) (long long) d;
            gt.step = (int) (u & 15u), gt.ph = d - (double) (int) u;
        }
        if (0.00100000005f > gt.mix)
            return;
        auto inc = 1.0;
        if (gt.div > 0.0)
            inc = (0.066666666666666666 / gt.div) * tempo;
        inc = inc / fs;
        for (int i = 0; i < n; ++i)
        {
            const auto p = inc + gt.ph;
            gt.ph = p;
            if (p > 1.0)
            {
                const auto st = gt.step + 1;
                gt.step = st > 15 ? 0 : st;
                gt.ph = p - 1.0;
            }
            auto g = (((1.0f - gt.steps[gt.step] * gt.mix) - gt.g) * gt.smooth) + gt.g;
            gt.g = g;
            if (g < 9.99999997e-07f && g > -9.99999997e-07f)
                gt.g = 0.0f, g = 0.0f;
            L[i] = g * L[i];
            R[i] = R[i] * gt.g;
        }
    }

    void render1 (Voice& v, int n8, bool first) noexcept // one voice (§2.5)
    {
        const auto ac = ampc, shA = shapeA, shB = shapeB;
        auto r = rng1;
        for (int i = 0; i < n8; ++i)
        {
            auto a1 = v.pA1 - v.incA, a2 = v.pA2 - v.incA, b1 = v.pB1 - v.incB, b2 = v.pB2 - v.incB;
            const auto nl = v.nlv;
            r = lcg (r);
            const auto nz = ((float) ((double) r * 2.3283064370807974e-10) - 0.125f) * nl;
            if (a1 < 0.0f) a1 = a1 + 1.0f;
            if (a2 < 0.0f) a2 = a2 + 1.0f;
            auto w = 0.0f;
            if (b1 < 0.0f) b1 = b1 + 1.0f, w = 1.0f;
            if (b2 < 0.0f) b2 = b2 + 1.0f;
            if (syncOn)
            {
                const auto t = a1 * w;
                a2 = a2 - t, a1 = a1 - t;
            }
            v.pA1 = a1, v.pA2 = a2, v.pB1 = b1, v.pB2 = b2;
            const auto B = b1 - b2 * shB;
            auto A = a1 - a2 * shA;
            const auto xb = v.lvB * B;
            if (ring)
                A = B * A;
            auto x = v.lvA * A;
            x = x + xb;
            x = x + nz;
            x = x - 0.419999987f;
            x = x - v.y[3] * v.res;
            if (x < 0.0f)
                x = x / (((x * x) * 3.0f) + 1.0f);
            const auto kk = v.k, gg = v.g;
            const auto y0 = ((x + v.xin[0]) * gg) - kk * v.y[0];
            v.xin[0] = x, v.y[0] = y0;
            const auto y1 = ((y0 + v.xin[1]) * gg) - kk * v.y[1];
            v.xin[1] = y0, v.y[1] = y1;
            const auto y2 = ((y1 + v.xin[2]) * gg) - kk * v.y[2];
            v.xin[2] = y1, v.y[2] = y2;
            const auto y3 = ((y2 + v.xin[3]) * gg) - kk * v.y[3];
            v.xin[3] = y2, v.y[3] = y3;
            auto o = ((y2 * v.ma0) + (y3 * v.m9c)) + (x * v.ma4);
            const auto dcn = ((o - v.dc) * 9.99999975e-05f) + v.dc;
            o = o - dcn;
            v.dc = fl (dcn);
            v.g130 = v.g130 + v.d134, v.g138 = v.g138 + v.d13c;
            v.nlv = nl + v.dnlv, v.lvA = v.lvA + v.dlvA, v.lvB = v.lvB + v.dlvB;
            v.g140 = ((v.g130 - v.g140) * ac) + v.g140;
            v.g144 = ((v.g138 - v.g144) * ac) + v.g144;
            const auto L = o * v.g140, R = o * v.g144;
            if (first)
                bufL[i] = L, bufR[i] = R;
            else
                bufL[i] = L + bufL[i], bufR[i] = R + bufR[i];
        }
        rng1 = r;
    }

    void render4 (Voice* const vs[4], int n8, bool first) noexcept // four lanes as the SSE code (§2.6)
    {
        const auto ac = ampc, shA = shapeA, shB = shapeB;
        auto r = rng1;
        for (int i = 0; i < n8; ++i)
        {
            float nzs[4], Ls[4], Rs[4];
            for (auto& z : nzs)
            {
                r = lcg (r);
                z = (float) ((double) r * 2.3283064370807974e-10) - 0.125f;
            }
            for (int j = 0; j < 4; ++j)
            {
                auto& v = *vs[j];
                auto a1 = v.pA1 - v.incA, a2 = v.pA2 - v.incA, b1 = v.pB1 - v.incB, b2 = v.pB2 - v.incB;
                const auto nz = nzs[j] * v.nlv;
                if (a1 <= 0.0f) a1 = a1 + 1.0f;
                if (a2 <= 0.0f) a2 = a2 + 1.0f;
                auto w = 0.0f;
                if (b2 <= 0.0f) b2 = b2 + 1.0f;
                if (b1 <= 0.0f) b1 = b1 + 1.0f, w = 1.0f;
                if (syncOn)
                {
                    const auto t = w * a1;
                    a2 = a2 - t, a1 = a1 - t;
                }
                v.pA1 = a1, v.pA2 = a2, v.pB1 = b1, v.pB2 = b2;
                const auto A = a1 - shA * a2;
                const auto B = b1 - shB * b2;
                float x;
                if (ring)
                    x = ((B * v.lvA) * A) + (B * v.lvB);
                else
                    x = (B * v.lvB) + (A * v.lvA);
                x = nz + x;
                x = x - 0.419999987f;
                x = x - v.res * v.y[3];
                v.nlv = v.nlv + v.dnlv, v.lvA = v.lvA + v.dlvA, v.lvB = v.lvB + v.dlvB;
                const auto m = x <= 0.0f ? 1.0f : 0.0f;
                x = x / ((((m * 3.0f) * x) * x) + 1.0f);
                const auto kk = v.k, gg = v.g;
                const auto y0 = ((x + v.xin[0]) * gg) - kk * v.y[0];
                v.xin[0] = x, v.y[0] = y0;
                const auto y1 = ((y0 + v.xin[1]) * gg) - kk * v.y[1];
                v.xin[1] = y0, v.y[1] = y1;
                const auto y2 = ((y1 + v.xin[2]) * gg) - kk * v.y[2];
                v.xin[2] = y1, v.y[2] = y2;
                const auto y3 = ((y2 + v.xin[3]) * gg) - kk * v.y[3];
                v.xin[3] = y2, v.y[3] = y3;
                auto o = ((v.ma4 * x) + (v.ma0 * y2)) + (v.m9c * y3);
                v.dc = v.dc + (o - v.dc) * 9.99999975e-05f;
                o = o - v.dc;
                v.g130 = v.g130 + v.d134, v.g138 = v.g138 + v.d13c;
                Rs[j] = o * v.g144, Ls[j] = o * v.g140;
                v.g140 = v.g140 + (v.g130 - v.g140) * ac;
                v.g144 = v.g144 + (v.g138 - v.g144) * ac;
            }
            const auto L = ((Ls[1] + Ls[0]) + Ls[2]) + Ls[3];
            const auto R = ((Rs[1] + Rs[0]) + Rs[2]) + Rs[3];
            if (first)
                bufL[i] = L, bufR[i] = R;
            else
                bufL[i] = L + bufL[i], bufR[i] = R + bufR[i];
        }
        rng1 = r;
        // the SSE code writes the oscillator level lanes back in reverse voice order (§2.6)
        const float la[4] = { vs[0]->lvA, vs[1]->lvA, vs[2]->lvA, vs[3]->lvA }, lb[4] = { vs[0]->lvB, vs[1]->lvB, vs[2]->lvB, vs[3]->lvB };
        for (int j = 0; j < 4; ++j)
            vs[j]->lvA = la[3 - j], vs[j]->lvB = lb[3 - j];
    }

    void decimate (float* outL, float* outR, int n) noexcept // 8:1 (§2.7)
    {
        const auto g = dec[0], a1 = dec[1], a2 = dec[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* st = ch == 0 ? dstL : dstR;
            const auto* buf = ch == 0 ? bufL : bufR;
            auto* out = ch == 0 ? outL : outR;
            auto xo = st[0], xm = st[1], yo = st[2], yn = st[3];
            for (int i = 0; i < n; ++i)
            {
                for (int j = 0; j < 8; ++j)
                {
                    const auto x = buf[i * 8 + j];
                    const auto yv = ((((x + xo) + (xm + xm)) * g) - yn * a1) - yo * a2;
                    xo = xm, xm = x, yo = yn, yn = yv;
                }
                out[i] = yn;
                xo = fl (xo), xm = fl (xm), yo = fl (yo), yn = fl (yn);
            }
            st[0] = xo, st[1] = xm, st[2] = yo, st[3] = yn;
        }
    }

    bool anyActive() const noexcept
    {
        for (int j = 0; j < voiceCount; ++j)
            if (voices[(std::size_t) j].note >= 0)
                return true;
        return false;
    }
};

// ------------------------------------------------------------------------------------------------ host side (§1.4)
void lawA (double pan, double vol, float& l, float& r) noexcept
{
    const auto t = (1.0 + std::clamp (pan, -1.0, 1.0)) * 0.78539816339744831;
    l = (float) (vol * std::cos (t));
    r = (float) (vol * std::sin (t));
}

int velFromInitVol (double iv) noexcept // CFLWrapper TriggerVoice
{
    const auto x = (float) iv * 10.0f + 1.0f;
    const auto v = ((float) std::log ((double) x) * 0.328458726f) * 127.0f;
    return std::clamp (i32 ((double) v), 0, 127);
}

// The FL wrapper (CFLWrapper): note slots with their FinalLevels, per-note tables, Gen_Render chunking, slot removal.
class Plugin
{
public:
    struct Slot
    {
        bool on = false;
        double fvol = 1.0, fpan = 0.0, pitchCents = 0.0;
        bool released = false;
        HostSlide slide; // a slide note moving Final.Pitch (issue #14)
    };
    Engine eng;
    Slot slots[128];
    bool sliding = false;
    bool panLaw = true;
    double lastLevelsVol = -1.0;

    // the plugin is created at 44 100 Hz, the state is loaded, then the host's rate (§1.1)
    void init (const std::vector<std::uint8_t>& state, double fs, int maxPoly)
    {
        eng.construct();
        eng.loadState (state);
        if (i32 (eng.fs) != i32 (fs))
            eng.setRate (fs);
        eng.npoly = maxPoly == 0 ? eng.maxVoices : maxPoly; // ProcessEvent 1
    }

    void noteOn (int key, double initVol, double fvol, double fpan, double pitchCents)
    {
        key = std::clamp (key, 0, 127);
        if (slots[key].on)
            eng.noteOff (key);
        slots[key] = { true, fvol, fpan, pitchCents, false, {} };
        eng.noteOn (key, velFromInitVol (initVol));
    }
    void noteOff (int key)
    {
        if (key >= 0 && key < 128 && slots[key].on)
        {
            slots[key].released = true;
            eng.noteOff (key);
        }
    }
    // a slide note: every held, unreleased key glides to `toCents` (Final.Pitch, cents from C5) over `samples`
    void slideTo (double toCents, float samples) noexcept
    {
        for (int i = 0; i < 128; ++i)
            if (slots[i].on && ! slots[i].released)
            {
                slots[i].slide.start ((float) (toCents - (100.0 * i - 6000.0 + slots[i].pitchCents)), samples);
                sliding = true;
            }
    }
    void setLevels (int key, double fvol, double fpan) noexcept
    {
        if (key >= 0 && key < 128 && slots[key].on)
            slots[key].fvol = fvol, slots[key].fpan = fpan;
    }

    void tables() noexcept // FUN_180003e60
    {
        float cl = 0.0f, cr = 0.0f, cp = 0.0f;
        bool first = true;
        for (int i = 0; i < 128; ++i)
        {
            if (slots[i].on)
            {
                const auto& s = slots[i];
                if (panLaw)
                    lawA (s.fpan, s.fvol, cl, cr);
                else
                    cl = cr = (float) s.fvol;
                const auto fp = (float) (100.0 * i - 6000.0 + (s.slide.cents != 0.0f ? s.pitchCents + (double) s.slide.cents : s.pitchCents));
                cp = (float) ((double) fp * 0.01 - (double) i + 60.0);
                if (first)
                {
                    for (int j = 0; j < i; ++j)
                        eng.hostTL[j] = cl, eng.hostTR[j] = cr, eng.hostT[j] = cp, eng.hostFC[j] = 0.0f, eng.hostFR[j] = 0.0f;
                    first = false;
                }
            }
            eng.hostTL[i] = cl, eng.hostTR[i] = cr, eng.hostT[i] = cp, eng.hostFC[i] = 0.0f, eng.hostFR[i] = 0.0f;
        }
    }

    bool idle() const noexcept
    {
        if (eng.anyActive() || ! eng.arpStopped)
            return false;
        return std::none_of (std::begin (slots), std::end (slots), [] (const Slot& s) { return s.on; });
    }

    // one Gen_Render of n samples; songBeats ≥ 0 while the transport plays (song position at the first sample)
    void render (float* outL, float* outR, int n, double songBeats) noexcept
    {
        tables();
        auto tpos = songBeats;
        eng.playing = songBeats >= 0.0;
        float l[chunkMax], r[chunkMax];
        for (int c0 = 0; c0 < n; c0 += chunkMax)
        {
            const auto cn = std::min (chunkMax, n - c0);
            if (eng.playing)
                eng.songpos = tpos;
            eng.process (l, r, cn);
            if (eng.playing)
                tpos = tpos + eng.tempo * 0.016666666666666666 * (double) cn * (1.0 / eng.fs);
            for (int i = 0; i < cn; ++i)
                outL[c0 + i] += l[i], outR[c0 + i] += r[i];
            if (sliding) // (issue #14) the slide moves the pitch table chunk by chunk
            {
                sliding = false;
                for (auto& h : slots)
                    if (h.slide.moving())
                    {
                        h.slide.advance ((float) cn);
                        sliding = sliding || h.slide.moving();
                    }
                tables();
            }
        }
        for (int key = 0; key < 128; ++key) // FUN_1800048d0: slots whose note ended and is no longer held
            if (slots[key].on && eng.velTab[key] == 0.0f)
            {
                bool sounding = false;
                for (int j = 0; j < voiceCount && ! sounding; ++j)
                    sounding = eng.voices[(std::size_t) j].note == key;
                if (! sounding)
                    slots[key].on = false;
            }
    }
};

// --------------------------------------------------------------------------------------------- LYNX front-end
//
// Levels as for GMS (REA_rodada39 §1.11): FL hands the plugin Final.Vol = channel volume, Final.Pan = note pan + channel
// pan and Final.Pitch = 100·(key − 60) + channel pitch; the wrapper turns them into the per-note tables. LYNX's channel
// gains gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…) give V and p back, so they are not applied a second time.
// Velocity: Init.Vol = (11^(1.28·vel) − 1)/10 (as GMS), then the wrapper's MIDI velocity. Max polyphony: 1 when the
// channel is mono, else the event value 0 (→ 64 voices); FL's value for other settings is not confirmed. While the
// transport plays, the song position (beats) locks the gate's step (§4.2).
class PoiZoneInstrument final : public Instrument
{
public:
    ~PoiZoneInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        take();
        rate.store (sr);
        {
            const std::lock_guard<std::mutex> lock (buildMutex);
            if (! loadedState.empty() && sr != loadedRate)
                rebuild (sr);
        }
        take();
        lastTicks = -1.0;
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        tempo.store (bpm > 0.0 ? bpm : 120.0);
        const int mp = channel.synth.mono ? 1 : 0;
        const std::lock_guard<std::mutex> lock (buildMutex);
        if (channel.pluginState == loadedState && loadedRate == rate.load() && mp == loadedPoly)
            return;
        loadedState = channel.pluginState;
        loadedPoly = mp;
        rebuild (rate.load());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        if (plugin == nullptr)
            return;
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto k = std::clamp (key, 0, 127);
        notePan[k] = nextPan;
        plugin->noteOn (k, initVol, channelVol, std::clamp ((double) nextPan + channelPan, -1.0, 1.0), (double) pitchCents.load() + (double) nextCents);
        nextPan = 0.0f, nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (plugin != nullptr)
            plugin->noteOff (key);
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        if (plugin != nullptr)
            plugin->slideTo (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents, samples);
    }

    void allNotesOff() noexcept override
    {
        take();
        if (plugin != nullptr)
            for (int k = 0; k < 128; ++k)
                plugin->noteOff (k);
    }

    bool idle() const noexcept override { return plugin == nullptr || plugin->idle(); }

    void setSongPosition (double ticks) noexcept override
    {
        playing = lastTicks >= 0.0 && ticks != lastTicks; // the transport plays when the position moves between blocks
        lastTicks = ticks;
        songBeats = ticks / (double) ppq;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (plugin == nullptr)
            return;
        plugin->eng.tempo = tempo.load();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int k = 0; k < 128; ++k)
                plugin->setLevels (k, vol, std::clamp ((double) notePan[k] + pan, -1.0, 1.0));
        }
        if (n <= 0)
            return;
        plugin->render (left, right, n, playing ? songBeats : -1.0);
    }

private:
    void rebuild (double sr) noexcept
    {
        loadedRate = sr;
        if (! stateHeader (loadedState))
        {
            clearPlugin.store (true);
            return;
        }
        try
        {
            auto p = std::make_unique<Plugin>();
            p->init (loadedState, sr, loadedPoly);
            delete incoming.exchange (p.release());
        }
        catch (...)
        {
        }
    }

    void take() noexcept
    {
        if (clearPlugin.exchange (false))
            delete retired.exchange (plugin.release());
        if (auto* p = incoming.exchange (nullptr))
        {
            delete retired.exchange (plugin.release());
            plugin.reset (p);
            for (int k = 0; k < 128; ++k)
                plugin->setLevels (k, channelVol, channelPan);
        }
    }

    std::unique_ptr<Plugin> plugin;
    std::atomic<Plugin*> incoming { nullptr }, retired { nullptr };
    std::atomic<bool> clearPlugin { false };
    std::mutex buildMutex; // configure (UI thread) and prepare build plugins; the audio thread only takes them
    std::atomic<double> rate { 44100.0 }, tempo { 120.0 };
    std::atomic<float> pitchCents { 0.0f };
    std::vector<std::uint8_t> loadedState;
    double loadedRate = 0.0;
    int loadedPoly = 0;
    double channelVol = 1.0, channelPan = 0.0, lastTicks = -1.0, songBeats = 0.0;
    bool playing = false;
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

bool poizoneStateUsable (const std::vector<std::uint8_t>& state) { return stateHeader (state); }

int poizoneParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    std::size_t n = 0;
    if (index < 0 || ! stateHeader (state, &n) || (std::size_t) index >= n)
        return -1;
    return 12 + 4 * index;
}

std::optional<float> poizoneParam (const std::vector<std::uint8_t>& state, int index)
{
    const auto o = poizoneParamOffset (state, index);
    if (o < 0)
        return std::nullopt;
    float v;
    std::memcpy (&v, state.data() + o, 4);
    return v;
}

std::unique_ptr<Instrument> makePoiZoneInstrument() { return std::make_unique<PoiZoneInstrument>(); }

namespace poizone
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, int block, double bpm, int maxPoly, bool panLaw)
{
    if (! stateHeader (state) || length <= 0)
        return {};
    auto plugin = std::make_unique<Plugin>();
    plugin->panLaw = panLaw;
    plugin->init (state, sampleRate, maxPoly);
    plugin->eng.tempo = bpm;
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    block = std::max (1, block);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r (l.size());
    int n0 = 0;
    while (n0 < length)
    {
        // events at this sample in the model's order: sorted by sample, (on, off) per note in list order
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
        plugin->render (l.data(), r.data(), ln, -1.0);
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace poizone
} // namespace lynx
