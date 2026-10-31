// FL Studio's Sawer (REA_rodada65 §3). See Sawer.h.
//
// The framework parts (wrapper, note allocation, arpeggiator, master gain, chorus, delay) follow LYNX's PoiZone port
// (core/PoiZone.cpp, REA_rodada44), whose DLL functions are instruction-for-instruction those of Sawer's engine; the
// rest is ported from Sawer's own machine code. Float/double types and the operation order of every expression are the
// DLL's, so contraction stays off. Clean-room: behaviour described in our own words; no Image-Line code.

#include "core/Sawer.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int voiceCount = 24;   // CSawerGenerator objects in the engine's list (§3.2)
constexpr int chunkMax = 64;     // engine buffers of 68 floats, used 64 at a time (as PoiZone)
constexpr int paramCount = 96;
constexpr int tableSize = 8192;
constexpr float tinyF = 9.99999972e-10f;

inline float fl (float x) noexcept { return (x < tinyF && x > -tinyF) ? 0.0f : x; }
inline std::uint32_t lcg (std::uint32_t s) noexcept { return s * 0x0bb38435u + 0x3619636bu; }
inline std::uint32_t lcgB (std::uint32_t s) noexcept { return s * 0x77a73631u + 0xf6ff3a94u; } // voice reset's draws
inline float powf32 (float a, float b) noexcept { return (float) std::pow ((double) a, (double) b); }
// The engine's CRT cosf on its FMA path (REA_rodada65 §3.9): |x| reduced by n = trunc(|x|·2/π + 0.5) with π/2 split in two
// parts, then the degree-8 cosine or degree-9 sine Taylor polynomial in double, the sign from n, rounded to float.
inline double cosPoly (double x) noexcept
{
    const auto x2 = x * x;
    const auto t = 1.0 - x2 * 0.5;
    auto p = std::fma (x2, -2.755731922398589e-07, 2.4801587301587298e-05);
    p = std::fma (p, x2, -0.0013888888888888887);
    p = std::fma (p, x2, 0.041666666666666664);
    return std::fma (p, x2 * x2, t);
}
inline float cosfCrt (float xf) noexcept
{
    const auto x = std::fabs ((double) xf);
    if (! std::isfinite (xf) || x >= 3373259520.0)
        return (float) std::cos ((double) xf);
    if (x <= 0.78539816339744828)
    {
        if (x < 0.0078125)
            return x < 0.0001220703125 ? 1.0f : (float) std::fma (-(x * 0.5), x, 1.0);
        return (float) cosPoly (x);
    }
    const auto n = (int) std::fma (x, 0.63661977236758138, 0.5);
    const auto nd = (double) n;
    const auto r1 = std::fma (-nd, 1.5707963267341256, x);
    const auto r = r1 - nd * 6.077100506506192e-11;
    double y;
    if ((n & 1) != 0)
    {
        const auto x2 = r * r;
        auto p = std::fma (x2, 2.7557319223985893e-06, -0.0001984126984126984);
        p = std::fma (p, x2, 0.008333333333333333);
        p = std::fma (p, x2, -0.16666666666666666);
        y = std::fma (p, r * x2, r);
    }
    else
        y = cosPoly (r);
    if ((((n & 3) + 1) & 2) != 0)
        y = -y;
    return (float) y;
}
inline float clampf (float x, float lo, float hi) noexcept { return x <= lo ? lo : (x >= hi ? hi : x); }
inline float u01 (std::uint32_t r) noexcept { return (float) r * 2.32830644e-10f; }
inline float minss (float a, float b) noexcept { return a < b ? a : b; } // SSE minss/maxss operand order
inline float maxss (float a, float b) noexcept { return a > b ? a : b; }

int i32 (double x) noexcept
{
    if (! (x == x) || x >= 2147483648.0 || x < -2147483648.0)
        return (int) 0x80000000u;
    return (int) x;
}
int i32f (float x) noexcept { return i32 ((double) x); }

// The engine's statically linked CRT sincosf (FUN_18005adc0; the same vector routine as BassDrum's, REA_rodada43 §2.9):
// x itself and 1 − x²/2 below 2⁻¹³, otherwise a float sine kernel (Cody-Waite reduction by π, odd polynomial) of x and
// of |x| + π/2
float vsinf (float x) noexcept
{
    auto u = [] (std::uint32_t b) { float f; std::memcpy (&f, &b, 4); return f; };
    const float invPi = u (0x3ea2f983), magic = u (0x4b400000), p1 = u (0x40490000), p2 = u (0x3a7da000), p3 = u (0x34222000),
                p4 = u (0x2cb4611a), c9 = u (0x362edef8), c7 = u (0xb94fb7ff), c5 = u (0x3c088766), c3 = u (0xbe2aaaa6);
    const float ax = std::fabs (x);
    const bool neg = std::signbit (x);
    const float k = ax * invPi + magic;
    std::uint32_t kb;
    std::memcpy (&kb, &k, 4);
    const float n = k - magic;
    float r = ax - p1 * n;
    r = r - p2 * n;
    r = r - p3 * n;
    r = r - p4 * n;
    const float r2 = r * r;
    if ((kb & 1) != 0)
        r = -r;
    float p = c9 * r2 + c7;
    p = p * r2 + c5;
    p = p * r2 + c3;
    const float t = (r2 * p) * r;
    const float y = r + t;
    return neg ? -y : y;
}
void sincosf32 (float x, float& sn, float& cs) noexcept
{
    const float ax = std::fabs (x);
    if (ax < 0.000122070312f)
    {
        sn = x;
        cs = 1.0f - (ax * ax) * 0.5f;
        return;
    }
    sn = vsinf (x);
    cs = vsinf (ax + 1.57079637f);
}

std::uint32_t le32 (const std::uint8_t* p) noexcept { return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24; }

// "SSLF" + u32 size + "1rwS" (or the demo tag "DrwS") + 96 floats [+ the modulation block + name] (§3.1)
bool stateHeader (const std::vector<std::uint8_t>& s, std::size_t* count = nullptr)
{
    if (s.size() < 12 + 4 * paramCount || std::memcmp (s.data(), "SSLF", 4) != 0)
        return false;
    const auto tag = le32 (s.data() + 8);
    if (tag != 0x53777231u && tag != 0x53777244u)
        return false;
    const auto size = (std::size_t) le32 (s.data() + 4);
    if (size < 4 + 4 * paramCount || size > s.size() - 8)
        return false;
    if (count != nullptr)
        *count = paramCount;
    return true;
}

// ------------------------------------------------------------------------------- shared effects (as PoiZone, §3.9)
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
        float sn, cs;
        sincosf32 (w, sn, cs);
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
        lp = Biq (false), hp = Biq (true);
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

struct Chorus
{
    bool on = false;
    float rate = 0.0f, depth = 0.0f, wet = -0.0f;
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
        setWet (-wet), setRateParam (rate);
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
        const auto d = (double) ((v * v) * v);
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

// Phaser (FUN_180026e20): six first-order all-passes per channel swept by a sine LFO, feedback through the last stage,
// the wet signal added to the dry one; centre and mix smoothed, the input faded in after a reset (§3.9.2)
struct Phaser
{
    float rate = 0, depth = 0, fb = 0, mix = 0, centre = 0;
    bool on = false;
    float zL[6] {}, zR[6] {}, fbL = 0, fbR = 0, phase = 0, centreS = -1.0f, mixS = -1.0f, ramp = 0;
    double fs = 44100.0;

    void reset() noexcept
    {
        ramp = 0;
        std::fill (std::begin (zL), std::end (zL), 0.0f);
        std::fill (std::begin (zR), std::end (zR), 0.0f);
        fbL = fbR = 0, phase = 0;
        centreS = -1.0f, mixS = -1.0f;
    }
    void run (float* L, float* R, int n) noexcept
    {
        if (! on)
            return;
        const auto inv = 1.0 / fs;
        const auto hi = (float) (inv * 20000.0), lo = (float) (inv * 50.0);
        const auto amt = (depth * depth) * (depth * 0.5f);
        const auto g = (float) ((double) powf32 (fb, 0.333333343f) * 0.97999999999999998);
        const auto mixT = maxss (mix, 9.99999975e-06f);
        const auto cenT = maxss (centre * centre, 9.99999975e-06f);
        const auto inc = (double) (float) ((double) ((rate * rate) * 44100.0f) * inv) * 0.00050000000000000001;
        if (0.0f > mixS)
            mixS = mixT;
        if (0.0f > centreS)
            centreS = cenT;
        const auto span = hi - lo;
        for (int i = 0; i < n; ++i)
        {
            const auto r = ramp;
            auto inL = r * L[i] - 9.99999997e-07f;
            auto inR = r * R[i] - 9.99999997e-07f;
            ramp = (float) ((double) (1.0f - r) * 0.01 + (double) r);
            const auto sn = cosfCrt (phase);
            const auto m = (sn + 1.0f) * amt;
            auto ph = (float) ((double) phase + inc);
            phase = ph;
            if ((double) ph > 6.2831853000000004)
                phase = ph - 6.28318548f;
            const auto c = centreS;
            auto x = minss ((c + m) * span + lo, 0.499000013f);
            const auto a = (1.0f - x) / (x + 1.0f);
            centreS = (float) ((double) (cenT - c) * 0.001 + (double) c);
            inL = inL + g * fbL;
            inR = inR + g * fbR;
            auto chain = [a] (float* z, float in) {
                for (int k = 0; k < 6; ++k)
                {
                    const auto t = z[k] - a * in;
                    z[k] = t * a + in;
                    in = t;
                }
                return in;
            };
            const auto oL = chain (zL, inL);
            const auto oR = chain (zR, inR);
            fbL = oL, fbR = oR;
            const auto ms = mixS;
            mixS = (mixT - ms) * 0.00100000005f + ms;
            L[i] = ms * oL + L[i];
            R[i] = ms * oR + R[i];
        }
    }
};

// Reverb (fxReverb, FUN_18002a9c0): the same plate as Transistor Bass's (REA_rodada57 §2.9, core/TransistorBass.cpp)
// with 18 controls (no predelay feedback) and the controls' integer laws computed as v·a + b then floor (§3.9.4)
struct Reverb
{
    static constexpr int lineN[12] { 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };
    static constexpr int tapN[14] { 266, 2974, 1913, 2496, 1690, 187, 1566, 353, 3627, 1728, 3173, 1811, 335, 621 };
    static constexpr int erLN[8] { 1102, 2904, 3784, 4873, 5878, 7470, 9062, 9263 };
    static constexpr int erRN[8] { 837, 1423, 2865, 3938, 5278, 5978, 7532, 9263 };

    double fs = 44100.0;
    std::vector<float> mem;
    std::array<std::size_t, 14> base {};
    std::array<int, 12> L {}, P {};
    int preLen = 1, preP = 0, erSize = 1, erP = 0, storedSize = 0;
    std::array<int, 14> taps {};
    std::array<int, 8> erL {}, erR {};
    std::array<float, 8> erGL {}, erGR {};
    int inLevel = 0, lowCut = 75, highCut = 40, predelayMs = 0, size = 50, diffusion = 100, bassMul = 10, xover = 500, decay = 15,
        damping = 40, dry = 0, er = 0, wet = 0, width = 100;
    bool muteIn = false, muteDry = false, muteEr = false, muteWet = false, dirty = true;
    float gIn = 1.0f, cLc = 0.0f, cHc = 1.0f, cD1 = 0.0f, cD2 = 0.0f, cT1 = 0.0f, cT2 = 0.0f, g = 0.0f, cShelf = 0.0f, cXover = 0.0f, cDamp = 1.0f;
    float gDry = 0.0f, gEr = 0.0f, gWet = 0.0f, wetNow = -1000.0f;
    float sHc = 0.0f, sLcY = 0.0f, sLcX = 0.0f, sAx = 0.0f, sAy = 0.0f, sBx = 0.0f, sBy = 0.0f, sDampA = 0.0f, sDampB = 0.0f;

    static float bitsF (std::uint32_t b) noexcept { float f; std::memcpy (&f, &b, 4); return f; }
    Reverb()
    {
        static const std::uint32_t def[18] = { 0x3f5b83f4, 0x3c99e459, 0x3e25ed09, 0, 0x3efd6a05, 0x3f800000, 0x3e924925, 0x3e76474b, 0x3d901495,
                                               0x3e25ed09, 0x3f5b83f4, 0x3f463b97, 0x3f128bdc, 0x3f000000, 0, 0, 0, 0 };
        for (int k = 0; k < 18; ++k)
            setParam (k, bitsF (def[k]));
        allocate (44100.0);
    }
    static int fl0 (float x) noexcept { return (int) std::floor ((double) x); }
    void setParam (int id, float v) noexcept
    {
        dirty = true;
        switch (id)
        {
            case 0: inLevel = fl0 (v * 421.0f - 360.5f); break;
            case 1: lowCut = fl0 (v * 2981.0f + 19.5f); break;
            case 2: highCut = fl0 (v * 216.0f + 5.5f); break;
            case 3: predelayMs = fl0 (v * 250.0f + 0.5f); break;
            case 4: size = fl0 (v * 99.0f + 1.5f); break;
            case 5: diffusion = fl0 (v * 100.0f + 0.5f); break;
            case 6: bassMul = fl0 (v * 28.0f + 2.5f); break;
            case 7: xover = fl0 (v * 1975.0f + 25.5f); break;
            case 8: decay = fl0 (v * 199.0f + 1.5f); break;
            case 9: damping = fl0 (v * 216.0f + 5.5f); break;
            case 10: dry = fl0 (v * 421.0f - 360.5f); break;
            case 11: er = fl0 (v * 421.0f - 360.5f); break;
            case 12: wet = fl0 (v * 421.0f - 360.5f); break;
            case 13: width = fl0 (v * 200.0f + 0.5f); break;
            case 14: muteIn = fl0 (v + 0.5f) != 0; break;
            case 15: muteDry = fl0 (v + 0.5f) != 0; break;
            case 16: muteEr = fl0 (v + 0.5f) != 0; break;
            case 17: muteWet = fl0 (v + 0.5f) != 0; break;
            default: break;
        }
    }
    void allocate (double rate)
    {
        fs = rate;
        const auto f = (int) rate;
        std::size_t total = 0;
        for (int k = 0; k < 12; ++k)
        {
            base[(std::size_t) k] = total;
            L[(std::size_t) k] = (f * lineN[k]) / 20000;
            total += (std::size_t) L[(std::size_t) k];
            P[(std::size_t) k] = 0;
        }
        base[12] = total;
        preLen = (f * 250) / 1000;
        total += (std::size_t) preLen;
        base[13] = total;
        auto u = (unsigned) ((double) f * 13950.0 / 44100.0);
        int bits = 0;
        if ((int) u > 0)
            while (u != 0)
                ++bits, u >>= 1;
        erSize = 1 << (bits + 1);
        total += (std::size_t) erSize;
        mem.assign (total, 0.0f);
        preP = erP = 0;
    }
    void reset() noexcept
    {
        std::fill (mem.begin(), mem.end(), bitsF (0x0da24260));
        sHc = sLcY = sLcX = 0.0f;
        sAx = sAy = sBx = sBy = 0.0f;
        sDampA = sDampB = 0.0f;
        wetNow = -1000.0f;
    }
    static float powF (float a, float b) noexcept { return (float) std::pow ((double) a, (double) b); }
    static float expF (float a) noexcept { return (float) std::exp ((double) a); }
    static float lnF (float a) noexcept { return (float) std::log ((double) a); }
    void coefficients() noexcept
    {
        const auto fsF = (float) fs;
        auto level = [] (int v) { return v < -360 ? 0.0f : powF (10.0f, (float) v / 200.0f); };
        gIn = level (inLevel);
        if (muteIn)
            gIn = 0.0f;
        if ((float) lowCut > 19.0f)
        {
            auto x = fsF * 0.5f;
            x = std::min (x, (float) lowCut);
            x = x * 3.1415927410125732f;
            x = x / fsF;
            const auto t = (float) std::tan ((double) x);
            cLc = (t - 1.0f) / (t + 1.0f);
        }
        else
            cLc = 0.0f;
        auto cutLaw = [&] (int v) {
            if (221.0f > (float) v)
            {
                const auto x = ((float) v * 628.318542f) / fsF; // (Transistor Bass: v·100, doubled, ·π)
                const auto c = cosfCrt (x);
                const auto a = 2.0f - c;
                auto r = a * a;
                r = r - 1.0f;
                const auto sq = std::sqrt (r);
                return 1.0f - (a - sq);
            }
            return 1.0f;
        };
        cHc = cutLaw (highCut);
        const auto fi = (int) fsF;
        int len = (fi * predelayMs) / 1000;
        if (len < 1)
            len = 1;
        preLen = len;
        preP = preP % len;
        const auto Rr = ((size + 15) * fi) / 115;
        for (int k = 0; k < 12; ++k)
        {
            auto n = (Rr * lineN[k]) / 20000;
            if (n < 1)
                n = 1;
            L[(std::size_t) k] = n;
            P[(std::size_t) k] = P[(std::size_t) k] % n;
        }
        for (int k = 0; k < 14; ++k)
            taps[(std::size_t) k] = (Rr * tapN[k]) / 20000;
        const auto E = (float) (((size + 3) * fi) / 103);
        for (int k = 0; k < 8; ++k)
        {
            auto a = (float) erLN[k];
            a = a * 1.5f;
            a = a * E;
            a = a / 44100.0f;
            auto b = (float) erRN[k];
            b = b * 1.5f;
            b = b * E;
            b = b / 44100.0f;
            erL[(std::size_t) k] = (int) a;
            erR[(std::size_t) k] = (int) b;
            float x = 0.0f;
            if (k == 0)
                x = erL[0] < erR[0] ? (float) erL[0] : (float) erR[0];
            auto gl = x * 0.5f;
            erGL[(std::size_t) k] = gl / (float) erL[(std::size_t) k];
            auto gr = x * 0.36000001430511475f;
            erGR[(std::size_t) k] = gr / (float) erR[(std::size_t) k];
        }
        if (size != storedSize)
        {
            storedSize = size;
            for (int k = 0; k < 12; ++k)
                std::fill_n (mem.begin() + (std::ptrdiff_t) base[(std::size_t) k], L[(std::size_t) k], 0.0f);
            std::fill_n (mem.begin() + (std::ptrdiff_t) base[13], erSize, 0.0f);
        }
        const auto diff = (float) diffusion / 100.0f;
        const auto logv = lnF (0.0010000000474974513f);
        auto half = [&] (int a, int b) {
            auto x = (float) (L[(std::size_t) a] + L[(std::size_t) b]);
            x = x * 0.5f;
            return x / fsF;
        };
        if (diff == 0.0f)
            cD1 = cD2 = cT1 = cT2 = 0.0f;
        else
        {
            auto x = half (0, 1) * logv;
            cD1 = expF (x / (diff * 0.07000000029802322f));
            x = logv * half (2, 3);
            cD2 = expF (x / (diff * 0.10999999940395355f));
            x = logv * half (4, 8);
            cT1 = expF (x / (diff * 0.3499999940395355f));
            x = logv * half (6, 10);
            cT2 = expF (x / (diff * 0.5099999904632568f));
        }
        const auto sum = (float) (L[9] + L[7] + L[5] + L[11]);
        const auto d10 = (float) decay / 10.0f;
        auto t = sum / fsF;
        t = t * logv;
        g = powF (expF (t / d10), 0.25f);
        auto b = (float) bassMul * d10;
        b = b / 10.0f;
        const auto ratio = powF (expF (t / b), 0.25f) / g;
        const auto m = std::min (1.0f, ratio);
        auto sh = ratio - 1.0f;
        cShelf = sh * 0.5f;
        auto xo = (float) xover * 3.1415927410125732f;
        xo = xo / fsF;
        const auto tx = (float) std::tan ((double) xo);
        cXover = (tx - m) / (m + tx);
        cDamp = cutLaw (damping);
        gDry = level (dry);
        if (muteDry)
            gDry = 0.0f;
        gEr = level (er) * 0.5f;
        if (muteEr)
            gEr = 0.0f;
        gWet = level (wet) * 0.300000012f; // (Transistor Bass: ·0.6, then ·0.5)
        if (muteWet)
            gWet = 0.0f;
        dirty = false;
    }
    float* line (int k) noexcept { return mem.data() + base[(std::size_t) k]; }
    float allpass (int k, float x, float cRead, float cWrite) noexcept
    {
        auto* b = line (k);
        auto& p = P[(std::size_t) k];
        auto v = b[p] - cRead * x;
        b[p] = cWrite * v + x;
        if (--p < 0)
            p = L[(std::size_t) k] - 1;
        return v;
    }
    float tap (int k, int at) noexcept
    {
        const auto n = L[(std::size_t) k];
        return line (k)[at < n ? at : at - n];
    }
    void process (float* left, float* right, int n) noexcept
    {
        if (dirty)
            coefficients();
        const auto t1 = 1.0f - g;
        auto wA = t1 * 0.6000000238418579f;
        wA = wA + g;
        auto wB = t1 * 0.25f;
        wB = wB + g;
        auto wC = t1 * 0.699999988079071f;
        wC = wC + g;
        auto wD = t1 * 0.20000000298023224f;
        wD = wD + g;
        auto wE = t1 * 0.5f;
        wE = wE + g;
        const auto wdt = (float) width * 0.009999999776482582f;
        float erLs = 0.0f, erRs = 0.0f;
        auto* erb = line (13);
        const auto mask = erSize - 1;
        for (int i = 0; i < n; ++i)
        {
            const auto a = gIn * left[i];
            const auto b = gIn * right[i];
            auto m = b + a;
            m = m * 0.5f;
            m = m + 1.0000000031710769e-30f;
            m = (m - sHc) * cHc + sHc;
            sHc = m;
            auto lc = (m - sLcY) * cLc + sLcX;
            sLcX = m;
            sLcY = lc;
            if (cLc != 0.0f)
            {
                m = (m - lc) * 0.5f;
                m = m + 1.0000000031710769e-30f;
            }
            if (gEr != 0.0f)
            {
                auto rd = [&] (int t) { return erb[(t + erP) & mask]; };
                auto x = rd (erL[0]) * erGL[0];
                x = x + rd (erL[1]) * erGL[1];
                x = x - rd (erL[2]) * erGL[2];
                erLs = rd (erL[3]) * erGL[3] + x;
                erLs = erLs + rd (erL[4]) * erGL[4];
                erLs = erLs + rd (erL[5]) * erGL[5];
                erLs = erLs + rd (erL[6]) * erGL[6];
                erRs = rd (erR[0]) * erGR[0];
                erRs = erRs - rd (erR[1]) * erGR[1];
                erRs = erRs + rd (erR[2]) * erGR[2];
                erRs = erRs + rd (erR[3]) * erGR[3];
                erRs = erRs - rd (erR[4]) * erGR[4];
                erRs = erRs + rd (erR[5]) * erGR[5];
                erRs = erRs + rd (erR[6]) * erGR[6];
                erRs = erRs - rd (erR[7]) * erGR[7];
            }
            const auto v1 = allpass (0, m, cD1, cD1);
            const auto v2 = allpass (1, v1, cD2, cD1);
            erb[erP] = v1;
            if (--erP < 0)
                erP = erSize - 1;
            const auto v3 = allpass (2, v2, cD1, cD1);
            const auto v4 = allpass (3, v3, cD2, cD1);
            auto* pre = line (12);
            auto x = pre[preP];
            pre[preP] = v4;
            if (--preP < 0)
                preP = preLen - 1;
            x = x + 9.999999682655225e-21f;
            auto s = line (11)[P[11]] * g;
            auto sh = (s - sAy) * cXover + sAx;
            sAx = s;
            sAy = sh;
            auto u = (sh + s) * cShelf + s;
            u = u + x;
            u = (u - sDampA) * cDamp + sDampA;
            sDampA = u;
            const auto a1 = allpass (4, u, cT1, cT1);
            auto& p5 = P[5];
            auto y = line (5)[p5];
            line (5)[p5] = a1;
            if (--p5 < 0)
                p5 = L[5] - 1;
            y = y * g;
            const auto a2 = allpass (6, y, cT2, cT2);
            auto& p7 = P[7];
            line (7)[p7] = a2;
            if (--p7 < 0)
                p7 = L[7] - 1;
            s = line (7)[p7] * g;
            sh = (s - sBy) * cXover + sBx;
            sBy = sh;
            sBx = s;
            u = (sh + s) * cShelf + s;
            u = u + x;
            u = (u - sDampB) * cDamp + sDampB;
            sDampB = u;
            const auto b1 = allpass (8, u, cT1, cT1);
            auto& p9 = P[9];
            y = line (9)[p9];
            line (9)[p9] = b1;
            if (--p9 < 0)
                p9 = L[9] - 1;
            y = y * g;
            const auto b2 = allpass (10, y, cT2, cT2);
            auto& p11 = P[11];
            line (11)[p11] = b2;
            if (--p11 < 0)
                p11 = L[11] - 1;
            auto oL = tap (9, taps[0] + P[9]);
            oL = oL - wB * tap (9, taps[1] + P[9]);
            oL = oL - wC * tap (10, taps[2] + P[10]);
            oL = oL - wD * tap (11, taps[3] + P[11]);
            oL = oL + wA * tap (5, taps[4] + P[5]);
            oL = oL + tap (6, taps[5] + P[6]);
            oL = oL + wE * tap (7, taps[6] + P[7]);
            auto oR = wB * tap (5, taps[8] + P[5]);
            oR = oR + tap (5, taps[7] + P[5]);
            oR = oR - wC * tap (6, taps[9] + P[6]);
            oR = oR - wD * tap (7, taps[10] + P[7]);
            oR = oR - wA * tap (9, taps[11] + P[9]);
            oR = oR + tap (10, taps[12] + P[10]);
            oR = oR + wE * tap (11, taps[13] + P[11]);
            float w;
            if (-99.0f > wetNow)
            {
                wetNow = gWet;
                w = (gWet - wetNow) * 0.0010000000474974513f + wetNow;
            }
            else
                w = (gWet - wetNow) * 0.0010000000474974513f + wetNow;
            wetNow = w;
            auto wl = w * oL;
            wl = wl + gEr * erLs;
            auto wr = w * oR;
            wr = wr + gEr * erRs;
            const auto sum2 = wr + wl;
            auto d = wl - wr;
            d = d * wdt;
            const auto r2 = sum2 - d;
            auto l2 = d + sum2;
            l2 = l2 + a * gDry;
            auto rr = b * gDry;
            rr = rr + r2;
            left[i] = l2;
            right[i] = rr;
        }
    }
};

// 2nd-order filter of the DC/denormal guard (FUN_18001a8e0 / FUN_18001a9e0): a high-pass at 4 Hz, Q 1
struct DcFilter
{
    float f = 0, q = 0, fs = 0, b = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set (float nf, float nq, float nfs) noexcept
    {
        if (nf == f && nq == q && nfs == fs)
            return;
        f = nf, q = nq, fs = nfs;
        const auto w = (float) (((double) nf * 6.2831853071795862) / (double) nfs);
        float sn, cs;
        sincosf32 (w, sn, cs);
        const auto r = 1.0f / (sn + nq);
        b = fl ((cs + 1.0f) / ((sn / nq) + 2.0f));
        a1 = fl ((cs * r) * (nq * -2.0f));
        a2 = fl ((nq - sn) * r);
    }
    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0f; }
    float tick (float x) noexcept
    {
        x = fl (x);
        auto yv = ((((x - (x1 + x1)) + x2) * b) - (y1 * a1)) - (a2 * y2);
        yv = fl (yv);
        x2 = x1, y2 = y1, x1 = x, y1 = yv;
        return yv;
    }
};

// ---------------------------------------------------------------------------------------------------- voice (§3.4)
struct Voice
{
    int note = -1, uniI = 0, uniN = 1;              // +0x10 key, +0x14/+0x15 unison index/count
    float drift = 0.0f, vel = 0.0f, x28 = 0.0f;     // +0x18 note-on detune, +0x20 velocity (curved), +0x28
    float grate = 0.0f, gpos = 0.0f;                // +0x2c glide rate, +0x30 glide position
    bool released = false;                          // +0x24
    std::uint32_t counter = 0;                      // +0x34 age
    float rate = 44100.0f;                          // +0x1c
    double relT = -1.0, t = 0.0;                    // +0x48 time since release (−1: held), +0x50 time since start
    std::uint32_t ph1 = 0, inc1 = 0, ph2 = 0, inc2 = 0, ph3 = 0, inc3 = 0, subOfs = 0; // +0x58 … +0x70
    float gL = 0, gR = 0, tgL = 0, tgR = 0;         // +0x74/+0x78 levels, +0x7c/+0x80 their targets
    float s1 = 0, s2 = 0, s3 = 0, y = 0, yp = 0, fb = 0; // +0x84 … +0x98 filter stages and feedback memory
    float cut = 0, cutT = 0, res = 0;               // +0x9c, +0xa0, +0xa4
    float aA = 0, aD = 0, aR = 0, aS = 0;           // +0xa8 … +0xb4 amplitude envelope times / sustain
    float aT = 0, aC = 0, aK = 0;                   // +0xb8 target, +0xbc current, +0xc0 coefficient
    float fA = 0, fD = 0, fR = 0, fS = 0;           // +0xc4 … +0xd0 filter envelope
    float fT = 0, fC = 0, fK = 0;                   // +0xd4 target, +0xd8 current, +0xdc coefficient
    float fAmt = 0;                                 // +0xe0 filter envelope → cutoff
    float dr = 0, drS = 0;                          // +0xe4 drift draw, +0xe8 its smoothed value
    int drCnt = 0;                                  // +0xec
    float noise = 0;                                // +0xf0
};

// ------------------------------------------------------------------------------------------------- engine (§3)
class Engine
{
public:
    // process-global generators of the DLL, started as in a freshly loaded DLL
    std::uint32_t rng1 = 2545672, rng2 = 2222;
    std::array<Voice, voiceCount + 1> voices {}; // [0, 24) the list in order; [24] the padding voice
    std::array<int, voiceCount + 3> list {};
    int listCount = 0;
    double fs = 44100.0, tempo = 120.0;
    float hostTL[128] {}, hostTR[128] {}, hostT[128] {};
    std::vector<float> saw = std::vector<float> (tableSize, 0.0f), sq = std::vector<float> (tableSize, 0.0f);

    // CSynthesizer section (the PoiZone layout)
    float pitchOfs = 0.0f, glideT = 1e-4f, velCurve = 1.0f, pbRange = 0.0f, driftAmt = 0.0f, uniDetune = 0.1f, uniPan = 0.0f;
    float gain = 1.0f, cc7 = 1.0f, cc10 = 0.5f, mpan = 0.5f, pb = 0.0f, sustain = 0.0f;
    int glideMode = 0, poly = 0, maxVoices = voiceCount, npoly = 0, unison = 1, lastNote = -1;
    bool octaver = false, mono = false;
    std::uint32_t counter = 0;
    int held[128] {};
    float velTab[128] {};
    float mgL = 0.0f, mgR = 0.0f;
    float lastVel = 0.0f; // +0x3b24

    // Sawer section (§3.3)
    float P[paramCount] {};
    float transp = -12.0f, fine = 0.0f, uniShift = 0.0f;
    float aAtt = 0, aDec = 0, aSus = 0, aRel = 0, fAtt = 0, fDec = 0, fSus = 0, fRel = 0, fAmt = 0;
    float subPh = 0, subHarm = 0.25f, subLvl = 0, subDet = 0, subInv = 1.0f, syncFrq = 0, noiseLvl = 0;
    float cutP = 0, resP = 0, keyTrk = 0, velTrk = 0;
    int fMode = 0, egDest = 0, lfoDest = 0, lfoShape = 0;
    bool subRetrig = false, subSquare = false, syncOn = false, uniRetrig = false;
    float mixW[4] {}, mixT[4] {};       // filter output weights (low, band, high, notch): current, target
    float subMix = 0.0f;                // +0x3acc
    // global LFO (§3.6)
    double lfoPh = 0.0, lfoSpeed = 0.0;
    float lfoSH = 0.0f, lfoVal = 0.0f, lfoDiv = 1.0f, lfoAmt = 0.0f, lfoAtt = 0.0f, lfoRel = 0.0f;
    bool lfoInv = false, lfoRetrig = false, lfoSync = false;
    // modulation matrix (§3.8): amount, destination, source per slot
    float modAmt[6] {};
    int modDst[6] { -1, -1, -1, -1, -1, -1 }, modSrc[6] {};
    // rate-dependent
    int chunk = 46;                      // control chunk (+0x3a58)
    float maxCut = 0.0f;                 // +0x3a5c
    float dec[3] {}, dstL[4] {}, dstR[4] {};
    int stepCnt = 0;                     // +0x3b20 (6-sample step counter, shared by every voice group)
    DcFilter dc;                          // +0x3ad0 (one state for both channels, §3.7)
    float bufL[chunkMax * 6] {}, bufR[chunkMax * 6] {};
    // effects (§3.9)
    Chorus chorus;
    Phaser phaser;
    Delay delay;
    Reverb reverb;
    bool reverbOn = false;
    int fxSilent = 0;
    bool fxIdle = false;
    bool playing = false;
    double songpos = 0.0;
    // arpeggiator (as PoiZone)
    bool arpOn = false, arpStopped = true, arpToggle = false, arpPending = true;
    int arpMode = 0, arpRange = 1, arpCount = 0, arpIdx = -2, arpDir = 1;
    double arpDiv = 1.0, arpSwing = 0.0, arpPhase = 0.0;
    int arpCur[8] {};
    int arpNote[128];
    float arpVel[128];

    Engine()
    {
        std::fill (std::begin (arpNote), std::end (arpNote), -1);
        std::fill (std::begin (arpVel), std::end (arpVel), 1.0f);
        arpReset();
    }

    void setTable (const std::vector<float>& t)
    {
        if (t.size() != (std::size_t) tableSize)
            return;
        saw = t;
        for (int k = 0; k < tableSize; ++k) // the pulse form: the cycle minus itself half a period later
            sq[(std::size_t) k] = saw[(std::size_t) k] - saw[(std::size_t) ((k + 0x1000) & 0x1fff)];
    }

    // ---- parameters (§3.3)
    static double timeCurveD (float v) noexcept // FUN_180031ff0: v⁵·50, at least 0.00101 (seconds)
    {
        auto d = (double) v;
        d = d * d * d * d * 50.0 * d;
        return d <= 0.00101 ? 0.00101 : d;
    }
    static float timeCurve (float v) noexcept { return (float) timeCurveD (v); }
    static int sel (float v, int lo, int hi) noexcept // FUN_180031fc0
    {
        auto i = i32f (((float) (hi - lo) * v) + (float) lo);
        return std::min (std::max (i, lo), hi);
    }
    void retune() noexcept { pitchOfs = (fine + transp) + uniShift; }

    // FUN_180037910: the parameter plus the modulation matrix (velocity is the only source LYNX feeds)
    float modulated (int i, float v) const noexcept
    {
        for (int s = 0; s < 6; ++s)
        {
            if (modSrc[s] == 0 || modDst[s] != i)
                continue;
            const auto src = modSrc[s] == 1 ? lastVel : 0.0f; // 2 aftertouch, 3 pitch bend, ≥ 4 controllers: 0
            v = s == 0 ? src * modAmt[s] + v : v + src * modAmt[s];
        }
        return clampf (v, 0.0f, 1.0f);
    }

    void setParam (int i, float raw)
    {
        if (i >= 0 && i < paramCount)
            P[i] = raw;
        apply (i, raw);
    }
    void apply (int i, float raw)
    {
        const auto v = i < 73 ? modulated (i, raw) : raw; // (the first 73 parameters are clamped to 0…1, §3.3)
        switch (i)
        {
            case 0: gain = powf32 (10.0f, v * 2.4000001f - 0.90000004f); break;
            case 1: mpan = clampf (v, 0.0f, 1.0f); break;
            case 2: aAtt = timeCurve (v); break;
            case 3: aDec = timeCurve (v); break;
            case 4: aSus = v; break;
            case 5: aRel = (float) (timeCurveD (v) * 0.35 + 0.02); break;
            case 6: transp = (float) sel (v, -24, 24) - 12.0f, pitchOfs = (transp + fine) + uniShift; break;
            case 7: fine = (v + v) - 1.0f, pitchOfs = (fine + transp) + uniShift; break;
            case 8: setMono (v != 0.0f); break;
            case 9:
            case 10:
            {
                const auto a = P[9], b = P[10];
                glideMode = a == 0.0f ? (b == 0.0f ? 1 : 0) : (b == 0.0f ? 3 : 2);
                break;
            }
            case 11:
            {
                const auto g = (v * v) * v;
                glideT = (double) g < 0.0001 ? 9.99999975e-05f : g;
                break;
            }
            case 12: octaver = v != 0.0f; break;
            case 13: uniRetrig = v != 0.0f; break;
            case 14:
            case 15:
            {
                if (i == 14)
                {
                    static const int u[] = { 1, 2, 4, 8 };
                    unison = std::min (u[sel (v, 0, 3)], 8);
                }
                else
                    uniDetune = v * 0.5f;
                uniShift = P[15] * -0.25f;
                if (! (P[14] >= 0.25f))
                    uniShift = 0.0f;
                retune();
                break;
            }
            case 16: uniPan = clampf (v, 0.0f, 1.0f); break;
            case 17: subPh = v; break;
            case 18:
            {
                static const float h[] = { 0.25f, 0.333333343f, 0.5f, 0.666666687f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
                const auto k = i32f (v * 8.0f);
                subHarm = h[std::min (std::max (k, 0), 8)];
                break;
            }
            case 19: subLvl = v * v; break;
            case 20: subDet = v * v; break;
            case 21: subRetrig = v != 0.0f; break;
            case 22: subInv = v != 0.0f ? -1.0f : 1.0f; break;
            case 23: subSquare = v != 0.0f; break;
            case 24: syncFrq = v * 4.0f; break;
            case 25: syncOn = v != 0.0f; break;
            case 26: noiseLvl = v; break;
            case 27: cutP = v; break;
            case 28: resP = v; break;
            case 29: keyTrk = v; break;
            case 30: velTrk = v; break;
            case 31:
            {
                fMode = sel (v, 0, 3);
                for (auto& t : mixT)
                    t = 9.99999997e-07f;
                mixT[fMode] = fMode == 3 ? 0.600000024f : 1.0f;
                break;
            }
            case 32: fAtt = timeCurve (v); break;
            case 33: fDec = timeCurve (v); break;
            case 34: fSus = v * v; break;
            case 35: fRel = timeCurve (v); break;
            case 36: fAmt = v; break;
            case 37: egDest = sel (v, 0, 3); break;
            case 38: lfoShape = sel (v, 0, 3); break;
            case 39:
            {
                static const float d[] = { 1.0f, 2.0f, 3.0f, 4.0f, 8.0f, 12.0f, 16.0f, 32.0f, 64.0f, 128.0f };
                lfoSpeed = (double) ((v * v) * (v * 10.0f));
                lfoDiv = d[std::min (std::max (sel (v, 0, 9), 0), 9)];
                break;
            }
            case 40: lfoSync = v != 0.0f; break;
            case 41: lfoAtt = (v * v) * (v * 20.0f) + 0.00999999978f; break;
            case 42: lfoRel = (v * v) * (v * 20.0f) + 0.00999999978f; break;
            case 43: lfoRetrig = v != 0.0f; break;
            case 44: lfoInv = v != 0.0f; break;
            case 45: lfoAmt = v; break;
            case 46: lfoDest = std::max (std::min (i32f (v * 6.0f), 6), 0); break;
            case 47: chorus.on = v != 0.0f; break;
            case 48: chorus.setDepth (v); break;
            case 49: chorus.setRateParam (v); break;
            case 50: chorus.setWet (v); break;
            case 51: phaser.on = v != 0.0f; break;
            case 52: phaser.centre = v; break;
            case 53: phaser.rate = v; break;
            case 54: phaser.depth = v; break;
            case 55: phaser.fb = v; break;
            case 56: phaser.mix = v; break;
            case 57: delay.reset(), delay.on = v != 0.0f; break;
            case 58: delay.setTime (0, v); break;
            case 59: delay.setTime (1, v); break;
            case 60: delay.setFb (v); break;
            case 61: delay.setMod (v); break;
            case 62: delay.setHpf (v); break;
            case 63: delay.setLpf (v); break;
            case 64: delay.setWet (v); break;
            case 65:
            {
                const bool on = v != 0.0f;
                if (reverbOn != on && (reverbOn = on))
                    reverb.reset();
                break;
            }
            case 66: reverb.setParam (8, (float) ((double) ((v * v) * v) * 0.59999999999999998)); break;
            case 67: reverb.setParam (2, (v * v) * v); break;
            case 68: reverb.setParam (9, std::fabs (1.0f - v)); break;
            case 69: reverb.setParam (12, v * 0.999000013f + 0.00100000005f); break;
            case 70: setArp (v != 0.0f); break;
            case 71:
                arpMode = std::min (std::max (i32 ((double) (v * 3.0f) + 0.00030000000000000003), 0), 3);
                break;
            case 72: arpRange = std::min (std::max (i32 ((double) (v * 3.0f) + 0.00030000000000000003), 0), 3) + 1; break;
            case 74:
            {
                static const double d[] = { 0.5, 1.0, 2.0, 3.0, 4.0, 8.0, 16.0 };
                const auto k = i32f (v);
                if (k >= 0 && k <= 6)
                    arpDiv = std::max (d[k], 0.25);
                break;
            }
            case 75:
            {
                auto p = i32f ((v + 1.0f) * 4.0f);
                poly = (p < 1 || maxVoices < p) ? maxVoices : p;
                break;
            }
            case 76: pbRange = (float) i32f (v); break;
            default:
                if (i >= 0x4e && i <= 0x53)
                {
                    const auto x = (v + v) - 1.0f;
                    auto s = x * x;
                    if (x < 0.0f)
                        s = -s;
                    modAmt[i - 0x4e] = s;
                }
                else if (i >= 0x54 && i <= 0x59)
                    modDst[i - 0x54] = (int) (short) i32f (v);
                else if (i >= 0x5a && i <= 0x5f)
                    modSrc[i - 0x5a] = (int) (short) i32f (v);
                break;
        }
    }

    void allNotesOffRaw()
    {
        const auto sus = sustain;
        sustain = 0.0f;
        for (int k = 0; k < 128; ++k)
            noteOffRaw (k);
        sustain = sus;
    }
    void setMono (bool m)
    {
        if (mono != m)
        {
            allNotesOffRaw();
            mono = m;
        }
    }
    void setArp (bool on)
    {
        if (arpOn != on)
        {
            allNotesOffRaw();
            arpOn = on;
        }
    }

    // ---- voices (§3.4)
    void resetVoice (Voice& v) noexcept // FUN_180031070
    {
        v.note = -1;
        v.drCnt = 0, v.t = 0.0;
        v.s1 = v.s2 = v.s3 = v.y = v.yp = v.fb = v.cut = v.cutT = 0.0f;
        rng1 = lcgB (rng1);
        v.ph1 = rng1 & 0xffffffu;
        v.res = 0.0f;
        v.aT = v.aC = v.aK = 0.0f;
        rng1 = lcgB (rng1);
        v.fT = v.fC = v.fK = 0.0f;
        v.gL = v.gR = v.tgL = v.tgR = 0.0f;
        v.ph2 = rng1 & 0xffffffu;
        v.inc1 = v.inc2 = v.inc3 = 0;
        rng1 = lcgB (rng1);
        v.ph3 = rng1 & 0xffffffu;
    }
    void startVoice (Voice& v) // FUN_180030ea0
    {
        if (lfoRetrig)
            lfoPh = 0.0;
        v.t = 0.0, v.relT = -1.0;
        v.aA = aAtt < 0.001f ? 0.001f : aAtt;
        v.aD = aDec < 0.001f ? 0.001f : aDec;
        v.aS = aSus;
        v.fA = fAtt < 0.001f ? 0.001f : fAtt;
        v.fD = fDec < 0.001f ? 0.001f : fDec;
        v.fS = fSus;
        if (uniRetrig)
            v.ph3 = 0, v.ph1 = 0;
        if (subRetrig)
            v.ph2 = (std::uint32_t) (long long) ((float) v.ph1 * subHarm);
        v.fC = 0.0f;
    }
    void releaseVoice (Voice& v) // FUN_180030ff0
    {
        v.aR = (double) aRel < 0.01 ? 0.00999999978f : aRel;
        v.fR = (double) fRel < 0.01 ? 0.00999999978f : fRel;
        v.relT = 0.0;
        v.released = true;
    }

    // ---- sample rate (§3.5)
    void setRate (double r)
    {
        fs = r;
        allNotesOffRaw();
        for (int j = 0; j < voiceCount; ++j) // FUN_180023140: every voice gets the rate and is reset
        {
            auto& v = voices[(std::size_t) j];
            v.rate = (float) r;
            resetVoice (v); // FUN_180031130: FUN_180024db0 resets the voice, then it is reset once more
            resetVoice (v);
            v.gpos = 48.0f;
        }
        mgL = masterGain (false), mgR = masterGain (true);
        lastNote = -1;
        arpReset();
        if (! arpStopped)
            arpStopped = true, arpAllNotesOff(), arpReset();
        // FUN_180037480
        dc.set (4.0f, 1.0f, (float) r);
        chunk = 2 - (int) (r * -0.001);
        maxCut = (float) (121026.0 / r) * 0.166666672f;
        const auto w = 16755.160819145562 / r;
        const auto sn = std::sin (w), cs = std::cos (w);
        dec[0] = fl ((float) ((1.0 - cs) / (sn * 0.125 + 2.0)));
        const auto inv = 1.0 / (sn + 8.0);
        dec[2] = fl ((float) ((8.0 - sn) * inv));
        dec[1] = fl ((float) ((cs * -16.0) * inv));
        chorus.setRate (r);
        phaser.fs = r;
        delay.setRate (r);
        reverb.allocate (r), reverb.dirty = true; // (FUN_1800273f0 → FUN_180027400)
        reverb.setParam (5, 1.0f);
        for (int k = 0; k < 4; ++k)
            mixW[k] = mixT[k];
        lfoPh = 0.0;
        std::fill (std::begin (dstL), std::end (dstL), 0.0f);
        std::fill (std::begin (dstR), std::end (dstR), 0.0f);
        dc.reset();
        chorus.reset(), phaser.reset(), delay.reset(), reverb.reset();
    }

    float masterGain (bool right) const noexcept // FUN_180023fa0 (= PoiZone's)
    {
        auto g = (gain * cc7) * 0.5f;
        auto f = (((cc10 - 0.5f) + cc10) - 0.5f) + (mpan + mpan);
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

    // ---- notes (the PoiZone framework, §3.2)
    int allocLimit() const noexcept { return npoly < poly ? npoly : poly; } // FUN_180031660

    void noteOn (int note, int vel7) // FUN_180007a20 → FUN_180037c50
    {
        const auto v = (float) vel7 * 0.007874015718698502f;
        velTab[note] = v;
        if (vel7 == 0)
        {
            noteOffRaw (note);
            return;
        }
        lastVel = v;
        for (int s = 0; s < 6; ++s) // the modulation matrix re-applies the destinations of its sources
            if (modSrc[s] != 0 && modDst[s] >= 0)
                apply (modDst[s], P[std::min (modDst[s], paramCount - 1)]);
        held[note] = i32f (v * 127.0f);
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
        held[note] = i32f (vel * 127.0f);
        if (poly == 0)
            return;
        if (mono)
            monoEvent (note, vel);
        else
            polyEvent (note, vel);
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
        const auto lim = allocLimit();
        for (int u = 0; u < unison; ++u)
        {
            Voice* chosen = nullptr;
            Voice* oldHeld = &voices[0];
            Voice* oldRel = &voices[0];
            for (int j = 0; j < lim && j < voiceCount; ++j)
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

    // ---- arpeggiator (the PoiZone framework)
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

    // ---- per-voice control (§3.4)
    float pitch (const Voice& v) const noexcept // FUN_180024e10
    {
        const auto oct = (octaver && (v.uniI & 1) != 0) ? 12.0f : 0.0f;
        auto spread = 0.0f;
        if (v.uniN > 1)
            spread = (((float) v.uniI * uniDetune) / (float) (v.uniN - 1)) - uniDetune * 0.5f;
        auto t = pitchOfs + v.gpos;
        t = t + (spread + hostT[v.note & 0x7f]);
        t = t + oct;
        t = t + v.drift;
        t = t + pbRange * pb;
        t = t + v.x28;
        return t;
    }
    float freq (const Voice& v) const noexcept // FUN_180024dd0
    {
        const auto p = (pitch (v) - 57.0f) * 0.0833333358f;
        return (float) (std::pow (2.0, (double) p) * 220.0);
    }
    float panGain (const Voice& v, bool right) const noexcept // FUN_180024eb0
    {
        auto f = 0.0f;
        if (v.uniN > 1)
        {
            f = (float) v.uniI / (float) (v.uniN - 1);
            f = (f + f) - 1.0f;
        }
        f = f * uniPan;
        if (right)
            return (clampf (1.0f - f, 0.0f, 1.0f) * hostTR[v.note & 0x7f]) * 0.5f;
        return (clampf (f + 1.0f, 0.0f, 1.0f) * hostTL[v.note & 0x7f]) * 0.5f;
    }
    static void glide (Voice& v, int n) noexcept // FUN_180024d10
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

    // FUN_1800305d0: once per control chunk of n host samples
    void control (Voice& v, int n)
    {
        glide (v, n);
        auto lfo = minss ((float) (v.t / (double) lfoAtt), 1.0f) * lfoVal;
        if (v.relT >= 0.0)
            lfo = lfo * (1.0f - minss ((float) (v.relT / (double) lfoRel), 1.0f));
        const auto amt = lfoAmt;
        const auto la = lfo * amt;
        float cutLfo = 0.0f, x8;
        if (lfoDest == 0)
        {
            x8 = la * amt;
            cutLfo = x8 + x8;
        }
        else
            x8 = amt * la;
        rng1 = lcg (rng1);
        if (v.drCnt == 0)
        {
            auto u = (float) rng1 * 2.32830644e-10f;
            u = u + u;
            v.dr = (u - 1.0f) * 0.00999999978f;
            v.drCnt = 1;
        }
        else
        {
            auto c = v.drCnt + 1;
            v.drCnt = c > 0x400 ? 0 : c;
        }
        const auto prev = v.t == 0.0 ? 0.0f : v.drS;
        const auto sm = (float) ((double) (v.dr - prev) * 0.0001 + (double) prev);
        v.drS = sm;
        const auto env = v.fC;
        auto gl = panGain (v, false), gr = panGain (v, true);
        v.tgL = gl, v.tgR = gr;
        auto x6 = lfo * 0.5f;
        if (lfoDest == 5)
        {
            const auto g = powf32 (10.0f, -((x6 + 0.5f) * amt));
            gr = gr * g, gl = gl * g;
            v.tgR = gr, v.tgL = gl;
        }
        if (lfoDest == 6)
        {
            x6 = x6 * amt;
            v.tgL = (0.5f - x6) * gl;
            v.tgR = (x6 + 0.5f) * gr;
        }
        auto nz = noiseLvl;
        if (lfoDest == 4)
            nz = nz + x8;
        nz = minss (maxss (nz, 0.0f), 1.0f);
        v.noise = (nz * nz) * 0.699999988f;
        auto sp = subPh;
        if (egDest == 2)
            sp = sp + fAmt * env;
        if (lfoDest == 2)
            sp = sp + la;
        sp = minss (maxss (sp, 0.0f), 1.0f);
        v.subOfs = (std::uint32_t) (long long) ((double) sp * 8371830.285 + 16777.215);
        double so = 0.0;
        if (syncOn)
        {
            so = (double) syncFrq;
            if (egDest == 1)
                so = std::min (std::max (so + (double) (fAmt * env) * 4.0, 0.0), 4.0);
            if (lfoDest == 1)
                so = std::min (std::max (so + (double) ((amt * amt) * lfo) * 4.0, 0.0), 4.0);
        }
        auto f = (double) freq (v) * std::pow (2.0, (double) sm + so);
        if (egDest == 3)
            f = f * (double) powf32 (2.0f, ((float) i32f (fAmt * 24.0f) * v.fC) * 0.0833333358f);
        if (lfoDest == 3)
        {
            x8 = (x8 * amt) * amt;
            f = f * (double) powf32 (2.0f, x8);
        }
        const auto R = 1.0 / ((double) (float) fs * 6.0);
        v.inc1 = (std::uint32_t) (long long) (std::min (std::max (R * f, 0.0), 0.49) * 16777215.0);
        v.inc2 = (std::uint32_t) (long long) (std::min (std::max ((((double) (subDet * 3.0f)) + (double) subHarm * f) * R, 0.0), 0.49) * 16777215.0);
        if (syncOn)
            v.inc3 = (std::uint32_t) (long long) (std::min (std::max ((std::pow (2.0, -so) * f) * R, 0.0), 0.49) * 16777215.0);
        else
            v.ph3 = 0, v.inc3 = 0;
        // cutoff (§3.4.4)
        const auto kt = (double) (((pitch (v) - 36.0f) * keyTrk) * 0.0833333358f);
        const auto base = (double) cutP * 9.3000000000000007 + 5.0;
        const auto vt = (((v.vel * 8.0f - 4.0f) * velTrk) + cutLfo) + v.drS;
        const auto c = (float) std::pow (2.0, (kt + base) + (double) vt);
        v.cutT = (float) ((0.33333333333333331 / ((double) (float) fs * 6.0)) * (double) (c * 6.0f));
        if (v.t == 0.0)
            v.cut = v.cutT;
        auto rr = (float) ((double) resP * 3.9900000000000002);
        if ((double) rr > 3.9900000000000002)
            rr = 3.98999977f;
        v.res = rr;
        v.fAmt = egDest == 0 ? ((fAmt * 320.0f) * fAmt) * (fAmt * fAmt) : 0.0f;
        // envelopes (§3.4.5)
        const auto dt = (double) n * 2.2675736961451248e-05;
        const auto vr = (double) v.rate;
        if (v.relT > -1.0)
        {
            const auto rel = (double) v.aR;
            if (v.relT > rel)
            {
                v.tgL = v.tgR = 0.0f;
                if (v.relT > rel + 0.10000000000000001)
                {
                    resetVoice (v); // vt+0x20 (FUN_180031130): the voice is reset twice (its rate unchanged)
                    resetVoice (v);
                    return;
                }
            }
            v.relT = v.relT + dt;
            v.aT = 0.0f;
            v.aK = (float) (8.8200000000000003 / (rel * vr));
        }
        else
        {
            double tt;
            if ((double) v.aA > v.t)
                v.aT = v.vel * 5.0f, tt = (double) v.aA * 39.441975;
            else
                v.aT = v.vel * v.aS, tt = (double) v.aD;
            v.aK = (float) (8.8200000000000003 / (tt * vr));
        }
        if (1.00000001e-07f > v.aT)
            v.aT = 1.00000001e-07f;
        if (v.relT > -1.0)
        {
            v.fT = 0.0f;
            if (v.relT > (double) v.fR)
            {
                v.fK = 0.0f;
                return;
            }
            v.fK = (float) (8.8200000000000003 / ((double) v.fR * vr));
        }
        else
        {
            double tt;
            if ((double) v.fA > v.t)
                v.fT = 5.0f, tt = (double) v.fA * 39.441975;
            else
                v.fT = v.fS, tt = (double) v.fD;
            v.fK = (float) (8.8200000000000003 / (tt * vr));
        }
        if (1.00000001e-07f > v.fT)
            v.fT = 1.00000001e-07f;
        v.t = dt + v.t;
    }

    // ---- the global LFO (§3.6), at the start of every process call
    void lfoStep (int n) noexcept
    {
        const auto p = lfoPh;
        lfoVal = 0.0f;
        if (lfoShape == 0)
        {
            const auto x = (float) (p + p);
            const auto y = 1.0f < x ? 2.0f - x : x;
            lfoVal = (y + y) - 1.0f;
        }
        else if (lfoShape == 1)
            lfoVal = (float) ((p + p) - 1.0);
        else if (lfoShape == 2)
            lfoVal = p >= 0.5 ? -1.0f : 1.0f;
        else if (lfoShape == 3)
            lfoVal = lfoSH;
        double inc;
        if (! lfoSync)
            inc = (1.0 / fs) * lfoSpeed;
        else
            inc = (tempo * (0.066666666666666666 / (double) lfoDiv)) / fs;
        auto np = inc * (double) n + lfoPh;
        lfoPh = np;
        if (np > 1.0)
        {
            lfoPh = np - 1.0;
            auto u = (float) rng1 * 2.32830644e-10f;
            u = u + u;
            lfoSH = u - 1.0f;
        }
        if (lfoInv)
            lfoVal = -lfoVal;
    }

    // ---- the voices at 6× (§3.4.6–§3.4.8)
    void render4 (Voice* const vs[4], float* outL, float* outR, int n6, bool first) noexcept // FUN_180035160
    {
        subMix = subInv * subLvl;
        float s1[4], s2[4], s3[4], y[4], yp[4], fbm[4], g[4], fenv[4], cut[4], aenv[4], gl[4], gr[4], res[4], fe0[4], nl[4];
        float fT[4], fK[4], aT[4], aK[4], cutT[4], tgL[4], tgR[4], u[4], fm[4], nz[4];
        std::uint32_t p1[4], p2[4], p3[4], i1[4], i2[4], i3[4], ofs[4];
        for (int l = 0; l < 4; ++l)
        {
            const auto& v = *vs[l];
            s1[l] = v.s1, s2[l] = v.s2, s3[l] = v.s3, y[l] = v.y, yp[l] = v.yp, fbm[l] = v.fb;
            fenv[l] = v.fC, cut[l] = v.cut, aenv[l] = v.aC, gl[l] = v.gL, gr[l] = v.gR, res[l] = v.res, fe0[l] = v.fAmt, nl[l] = v.noise;
            fT[l] = v.fT, fK[l] = v.fK, aT[l] = v.aT, aK[l] = v.aK, cutT[l] = v.cutT, tgL[l] = v.tgL, tgR[l] = v.tgR;
            p1[l] = v.ph1, p2[l] = v.ph2, p3[l] = v.ph3, i1[l] = v.inc1, i2[l] = v.inc2, i3[l] = v.inc3, ofs[l] = v.subOfs;
        }
        const auto k = (float) (176.40000837855041 / fs);
        const auto mc = maxCut;
        for (int l = 0; l < 4; ++l)
        {
            g[l] = ((fe0[l] * fenv[l]) + 1.0f) * cut[l];
            if (mc < g[l])
                g[l] = mc;
        }
        auto draw4 = [&] {
            for (int l = 0; l < 4; ++l)
            {
                u[l] = u01 (rng1) - 0.5f;
                rng1 = lcg (rng1);
            }
            for (int l = 0; l < 4; ++l)
                fm[l] = 0.0599999987f * u[l], nz[l] = u[l] * nl[l];
        };
        draw4();
        float w[4] = { mixW[0], mixW[1], mixW[2], mixW[3] }; // the weights the outputs use (one step late, §3.4.8)
        const auto& ST = subSquare ? sq : saw;
        for (int i = 0; i < n6; ++i)
        {
            if (stepCnt == 0)
            {
                for (int q = 0; q < 4; ++q)
                {
                    w[q] = mixW[q];
                    mixW[q] = (mixT[q] - mixW[q]) * k + mixW[q];
                }
                draw4();
                for (int l = 0; l < 4; ++l)
                {
                    g[l] = ((fe0[l] * fenv[l]) + 1.0f) * cut[l];
                    if (mc < g[l])
                        g[l] = mc;
                    cut[l] = cut[l] + (cutT[l] - cut[l]) * k;
                    fenv[l] = fenv[l] + (fT[l] - fenv[l]) * fK[l];
                    if (fenv[l] > 1.0f)
                        fenv[l] = 1.0f;
                    aenv[l] = aenv[l] + (aT[l] - aenv[l]) * aK[l];
                    if (aenv[l] > 1.0f)
                        aenv[l] = 1.0f;
                    gl[l] = gl[l] + (tgL[l] - gl[l]) * k;
                    gr[l] = gr[l] + (tgR[l] - gr[l]) * k;
                }
                stepCnt = 1;
            }
            else
            {
                ++stepCnt;
                if (stepCnt == 6)
                    stepCnt = 0;
            }
            float L = 0.0f, Rr = 0.0f, ol[4], orr[4];
            for (int l = 0; l < 4; ++l)
            {
                const auto sub = ST[((p2[l] + ofs[l]) >> 11) & 0x1fff];
                auto osc = sub * subMix + saw[(p1[l] >> 11) & 0x1fff];
                const auto sNew = p3[l] + i3[l];
                const auto ov = sNew & 0xff000000u;
                p3[l] = ov == 0 ? sNew : 0;
                const auto a = (ov == 0 ? p1[l] : 0) + i1[l];
                p1[l] = (a & 0xff000000u) == 0 ? a : 0;
                const auto b = (ov == 0 ? p2[l] : 0) + i2[l];
                p2[l] = (b & 0xff000000u) == 0 ? b : 0;
                auto x = (((osc * fm[l]) + osc) + -0.349999994f + nz[l]) * aenv[l];
                auto& S1 = s1[l];
                auto& S2 = s2[l];
                auto& S3 = s3[l];
                auto& Y = y[l];
                const auto G = g[l], Q = res[l];
                auto stage = [&] (float in) {
                    S1 = S1 + (in - S1) * G;
                    S2 = S2 + (S1 - S2) * G;
                    S3 = S3 + (S2 - S3) * G;
                    Y = Y + (S3 - Y) * G;
                    auto m = Y > 0.0f ? Y : 0.0f;
                    m = m * m;
                    m = m * m;
                    Y = Y / (m * 5.0f + 1.0f);
                };
                stage ((x - fbm[l] * Q));
                const auto y1 = Y;
                stage (x - ((yp[l] + y1) * 0.5f) * Q);
                const auto y2 = Y;
                x = x - ((y1 + y2) * 0.5f) * Q;
                stage (x);
                yp[l] = Y;
                fbm[l] = (y2 + Y) * 0.5f;
                auto o = Y * w[0] + S2 * w[1];
                o = o + (Y - S3) * w[2];
                o = o + (x - Y) * w[3];
                ol[l] = o * gl[l], orr[l] = o * gr[l];
            }
            L = ((ol[1] + ol[0]) + ol[2]) + ol[3];
            Rr = ((orr[1] + orr[0]) + orr[2]) + orr[3];
            if (first)
                outL[i] = L, outR[i] = Rr;
            else
                outL[i] = L + outL[i], outR[i] = Rr + outR[i];
        }
        for (int l = 0; l < 4; ++l)
        {
            auto& v = *vs[l];
            v.s1 = s1[l], v.s2 = s2[l], v.s3 = s3[l], v.y = y[l], v.yp = yp[l], v.fb = fbm[l];
            v.fC = fenv[l], v.cut = cut[l], v.aC = aenv[l], v.gL = gl[l], v.gR = gr[l];
            v.ph1 = p1[l], v.ph2 = p2[l], v.ph3 = p3[l];
        }
    }

    // FUN_180034a40: voices sounding within the polyphony, then the other sounding ones, then the free ones, then the
    // padding voice
    void buildList() noexcept
    {
        int a[voiceCount], b[voiceCount], c[voiceCount];
        int na = 0, nb = 0, nc = 0;
        const auto lim = allocLimit();
        for (int j = 0; j < voiceCount; ++j)
        {
            if (voices[(std::size_t) j].note < 0)
                c[nc++] = j;
            else if (na < lim)
                a[na++] = j;
            else
                b[nb++] = j;
        }
        int k = 0;
        for (int j = 0; j < na; ++j)
            list[(std::size_t) k++] = a[j];
        for (int j = 0; j < nb; ++j)
            list[(std::size_t) k++] = b[j];
        for (int j = 0; j < nc; ++j)
            list[(std::size_t) k++] = c[j];
        while (k < (int) list.size())
            list[(std::size_t) k++] = voiceCount;
        listCount = na;
    }

    void process (float* outL, float* outR, int n)
    {
        lfoStep (n);
        buildList();
        std::fill (outL, outL + n, 0.0f);
        std::fill (outR, outR + n, 0.0f);
        if (listCount <= 0)
            stepCnt = 0;
        else
        {
            bool first = true;
            for (int rem = listCount, k = 0; rem > 0; rem -= 4, k += 4)
            {
                if (rem == 1)
                {
                    auto& v = voices[(std::size_t) list[(std::size_t) k]];
                    int off = 0;
                    for (int left = n; left > 0;)
                    {
                        const auto m = std::min (chunk, left);
                        left -= m;
                        control (v, m);
                        renderOne (v, bufL + off, bufR + off, m * 6, first);
                        off += m * 6;
                    }
                    break;
                }
                Voice* g4[4] = { &voices[(std::size_t) list[(std::size_t) k]], &voices[(std::size_t) list[(std::size_t) k + 1]],
                                 &voices[(std::size_t) list[(std::size_t) k + 2]], &voices[(std::size_t) list[(std::size_t) k + 3]] };
                int off = 0;
                for (int left = n; left > 0;)
                {
                    const auto m = std::min (chunk, left);
                    left -= m;
                    for (auto* v : g4)
                        if (v->note >= 0)
                            control (*v, m);
                    render4 (g4, bufL + off, bufR + off, m * 6, first);
                    off += m * 6;
                }
                first = false;
            }
            decimate (outL, outR, n);
        }
        for (int i = 0; i < n; ++i) // the DC/denormal guard, one state for both channels (§3.7)
        {
            outL[i] = dc.tick (outL[i]);
            outR[i] = dc.tick (outR[i]);
        }
        delay.tempo = tempo;
        delay.setTime (0, delay.tL), delay.setTime (1, delay.tR);
        effects (outL, outR, n);
        if (arpOn)
            arpTick (n);
        const auto tl = masterGain (false), tr = masterGain (true);
        const auto kk = 0.00499999989f;
        for (int i = 0; i < n; ++i)
        {
            outL[i] = mgL * outL[i], outR[i] = outR[i] * mgR;
            mgL = ((tl - mgL) * kk) + mgL, mgR = ((tr - mgR) * kk) + mgR;
        }
    }

    void renderOne (Voice& v, float* oL, float* oR, int n6, bool first) noexcept // FUN_180036710
    {
        subMix = subInv * subLvl;
        auto s1 = v.s1, s2 = v.s2, s3 = v.s3, y = v.y, yp = v.yp, fbm = v.fb;
        auto p1 = v.ph1, p2 = v.ph2, p3 = v.ph3;
        const auto i1 = v.inc1, i2 = v.inc2, i3 = v.inc3, ofs = v.subOfs;
        const auto k = 176.400009f / (float) fs;
        const auto fe0 = v.fAmt, nl = v.noise;
        auto fenv = v.fC, cut = v.cut, aenv = v.aC, gl = v.gL, gr = v.gR;
        const auto res = v.res;
        auto g = minss (((fe0 * fenv) + 1.0f) * cut, maxCut);
        auto nz = (u01 (rng1) - 0.5f) * nl;
        rng1 = lcg (rng1);
        auto fm = 0.0f;
        const auto& ST = subSquare ? sq : saw;
        for (int i = 0; i < n6; ++i)
        {
            if (stepCnt == 0)
            {
                for (int q = 0; q < 4; ++q)
                    mixW[q] = (mixT[q] - mixW[q]) * k + mixW[q];
                g = minss (((fe0 * fenv) + 1.0f) * cut, maxCut);
                const auto u = u01 (rng1) - 0.5f;
                rng1 = lcg (rng1);
                fm = u * 0.0599999987f;
                nz = u * nl;
                cut = cut + (v.cutT - cut) * k;
                fenv = minss ((v.fT - fenv) * v.fK + fenv, 1.0f);
                aenv = minss ((v.aT - aenv) * v.aK + aenv, 1.0f);
                gr = gr + (v.tgR - gr) * k;
                gl = gl + (v.tgL - gl) * k;
                stepCnt = 1;
            }
            else
            {
                ++stepCnt;
                if (stepCnt == 6)
                    stepCnt = 0;
            }
            auto osc = ST[((p2 + ofs) >> 11) & 0x1fff] * subMix;
            const auto sNew = p3 + i3;
            const auto ov = sNew & 0xff000000u;
            p3 = ov == 0 ? sNew : 0;
            osc = osc + saw[(p1 >> 11) & 0x1fff];
            {
                const auto a = (ov == 0 ? p1 : 0) + i1;
                p1 = (a & 0xff000000u) == 0 ? a : 0;
                const auto b = (ov == 0 ? p2 : 0) + i2;
                p2 = (b & 0xff000000u) == 0 ? b : 0;
            }
            osc = osc * (fm + 1.0f);
            auto x = ((float) ((double) osc - 0.34999999999999998) + nz) * aenv;
            auto stage = [&] (float in) {
                s1 = s1 + (in - s1) * g;
                s2 = s2 + (s1 - s2) * g;
                s3 = s3 + (s2 - s3) * g;
                auto t4 = (s3 - y) * g + y;
                auto m = maxss (t4, 0.0f);
                m = m * m;
                m = m * m;
                y = t4 / (m * 5.0f + 1.0f);
            };
            stage (x - fbm * res);
            const auto h1 = y * 0.5f;
            stage (x - ((yp * 0.5f) + h1) * res);
            const auto h2 = y * 0.5f;
            x = x - (h1 + h2) * res;
            stage (x);
            yp = y;
            fbm = y * 0.5f + h2;
            auto o = (x - y) * mixW[3];
            o = o + (y - s3) * mixW[2];
            o = o + (y * mixW[0] + s2 * mixW[1]);
            const auto L = o * gl, Rr = o * gr;
            if (first)
                oL[i] = L, oR[i] = Rr;
            else
                oL[i] = L + oL[i], oR[i] = Rr + oR[i];
        }
        v.s1 = s1, v.s2 = s2, v.s3 = s3, v.gL = gl, v.gR = gr, v.aC = aenv, v.fC = fenv, v.cut = cut;
        v.y = y, v.yp = yp, v.fb = fbm;
        v.ph1 = p1, v.ph2 = p2, v.ph3 = p3;
    }

    void decimate (float* outL, float* outR, int n) noexcept // FUN_180036f50: 6:1
    {
        const auto b0 = dec[0], a1 = dec[1], a2 = dec[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* st = ch == 0 ? dstL : dstR;
            const auto* buf = ch == 0 ? bufL : bufR;
            auto* out = ch == 0 ? outL : outR;
            auto x2 = st[0], x1 = st[1], y2 = st[2], y1 = st[3];
            for (int i = 0; i < n; ++i)
            {
                for (int j = 0; j < 6; ++j)
                {
                    const auto x = buf[i * 6 + j];
                    const auto yv = (((x1 + x1) + (x + x2)) * b0 - y1 * a1) - y2 * a2;
                    x2 = x1, x1 = x, y2 = y1, y1 = yv;
                }
                out[i] = y1;
            }
            st[0] = x2, st[1] = x1, st[2] = y2, st[3] = y1;
        }
    }

    // FX (FUN_180030300): chorus, phaser, delay, reverb while sound came out in the last 2 s (§3.9)
    void effects (float* L, float* R, int n) noexcept
    {
        if ((int) (fs + fs) < fxSilent)
        {
            if (! fxIdle)
                chorus.reset(), phaser.reset(), delay.reset(), reverb.reset();
            fxIdle = true;
        }
        else if (! fxIdle)
        {
            chorus.run (L, R, n);
            phaser.run (L, R, n);
            delay.run (L, R, n);
            if (reverbOn)
                reverb.process (L, R, n);
        }
        for (int i = 0; i < n; ++i)
        {
            if (std::fabs (L[i]) > 9.99999975e-06f || std::fabs (R[i]) > 9.99999975e-06f)
                fxSilent = 0, fxIdle = false;
            else
                ++fxSilent;
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

// ------------------------------------------------------------------------------------------------ host side
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

// The FL wrapper (CFLWrapper, as PoiZone's)
class Plugin
{
public:
    struct Slot
    {
        bool on = false;
        double fvol = 1.0, fpan = 0.0, pitchCents = 0.0;
        HostSlide slide; // a slide note moving the key's Final.Pitch (issue #14)
    };
    Engine eng;
    Slot slots[128];
    bool panLaw = true;

    void loadState (const std::vector<std::uint8_t>& s)
    {
        if (! stateHeader (s))
            return;
        const auto size = (std::size_t) le32 (s.data() + 4);
        for (int i = 0; i < paramCount; ++i)
        {
            float v;
            std::memcpy (&v, s.data() + 12 + 4 * (std::size_t) i, 4);
            eng.setParam (i, v);
        }
        // the 96 parameters are followed by the modulation block; FL re-applies parameters 0x4e…0x5f after it
        (void) size;
        for (int i = 0x4e; i < 0x60; ++i)
            eng.apply (i, eng.P[i]);
    }

    void init (const std::vector<std::uint8_t>& state, const std::vector<float>& table, double fs, int maxPoly)
    {
        eng.setTable (table);
        construct();
        loadState (state);
        if (i32 (eng.fs) != i32 (fs))
            eng.setRate (fs);
        eng.npoly = maxPoly == 0 ? eng.maxVoices : std::min (maxPoly, eng.maxVoices); // ProcessEvent 1
    }

    // the engine's constructor tail (every parameter set to its default, FUN_180031e10 → FUN_180031eb0) and the
    // rate sets of a fresh instance (§3.5)
    static float defaultParam (int i) noexcept
    {
        switch (i)
        {
            case 0: case 1: case 6: case 7: case 0x12: case 0x27: case 0x30: case 0x40: case 0x42: case 0x43: case 0x44:
            case 0x45: case 0x48: case 0x4e: case 0x4f: case 0x50: case 0x51: case 0x52: case 0x53:
                return 0.5f;
            case 3: case 4: case 0x11: case 0x1b: case 0x21: case 0x22: case 0x23: case 0x2a: case 0x3f: case 0x4a:
                return 1.0f;
            case 0xf: case 0x31: case 0x3d: return 0.25f;
            case 0x32: case 0x34: case 0x3c: return 0.300000012f;
            case 0x35: return 0.200000003f;
            case 0x36: return 0.699999988f;
            case 0x38: return 0.600000024f;
            case 0x3a: case 0x3b: return 0.400000006f;
            case 0x4b: return 8.0f;
            case 0x4c: return 2.0f;
            default: return 0.0f;
        }
    }
    void construct()
    {
        eng.setParam (75, (float) ((double) 24 * 0.25 - 1.0));
        for (int i = 0; i < paramCount; ++i)
            eng.setParam (i, defaultParam (i));
        eng.setRate (44100.0); // a fresh instance has been through one rate set (EXEC: 144 reset draws of the generator)
    }

    void noteOn (int key, double initVol, double fvol, double fpan, double pitchCents)
    {
        key = std::clamp (key, 0, 127);
        if (slots[key].on)
            eng.noteOff (key);
        slots[key] = { true, fvol, fpan, pitchCents, {} };
        eng.noteOn (key, velFromInitVol (initVol));
    }
    // FL's slide note: the held keys' Final.Pitch glides to `to` (cents from C5); the wrapper's per-key pitch table
    // carries it to the engine every block
    void slide (double to, float samples) noexcept
    {
        for (int i = 0; i < 128; ++i)
            if (slots[i].on && eng.velTab[i] != 0.0f)
                slots[i].slide.start ((float) (to - (100.0 * i - 6000.0 + slots[i].pitchCents)), samples);
    }
    void noteOff (int key)
    {
        if (key >= 0 && key < 128 && slots[key].on)
            eng.noteOff (key);
    }
    void setLevels (int key, double fvol, double fpan) noexcept
    {
        if (key >= 0 && key < 128 && slots[key].on)
            slots[key].fvol = fvol, slots[key].fpan = fpan;
    }
    void tables() noexcept
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
                const auto fp = s.slide.cents != 0.0f ? (float) (100.0 * i - 6000.0 + s.pitchCents + (double) s.slide.cents)
                                                      : (float) (100.0 * i - 6000.0 + s.pitchCents);
                cp = (float) ((double) fp * 0.01 - (double) i + 60.0);
                if (first)
                {
                    for (int j = 0; j < i; ++j)
                        eng.hostTL[j] = cl, eng.hostTR[j] = cr, eng.hostT[j] = cp;
                    first = false;
                }
            }
            eng.hostTL[i] = cl, eng.hostTR[i] = cr, eng.hostT[i] = cp;
        }
    }
    bool idle() const noexcept
    {
        if (eng.anyActive() || ! eng.arpStopped)
            return false;
        // the effects' tails: until the engine's own 2-second silence gate has stopped them
        if (! eng.fxIdle && (eng.chorus.on || eng.phaser.on || eng.delay.on || eng.reverbOn))
            return false;
        return std::none_of (std::begin (slots), std::end (slots), [] (const Slot& s) { return s.on; });
    }
    void render (float* outL, float* outR, int n, double songBeats) noexcept
    {
        for (auto& s : slots)
            if (s.slide.moving())
                s.slide.advance ((float) n);
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
        }
        for (int key = 0; key < 128; ++key)
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
// As PoiZone (REA_rodada44 §1.4): FL hands the plugin Final.Vol = channel volume, Final.Pan = note pan + channel pan and
// Final.Pitch = 100·(key − 60) + channel pitch; the wrapper turns them into the per-note tables, so LYNX's channel
// gains are not applied a second time. Velocity: Init.Vol = (11^(1.28·vel) − 1)/10, then the wrapper's MIDI velocity.
// Max polyphony: 1 when the channel is mono, else the event value 0 (→ 24 voices). The song tempo drives the
// arpeggiator and the tempo-synced LFO and delay.
class SawerInstrument final : public Instrument
{
public:
    ~SawerInstrument() override
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
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        tempo.store (bpm > 0.0 ? bpm : 120.0);
        const int mp = channel.synth.mono ? 1 : 0;
        const std::lock_guard<std::mutex> lock (buildMutex);
        if (channel.pluginState == loadedState && loadedRate == rate.load() && mp == loadedPoly && channel.samplePath == tablePath)
            return;
        if (channel.samplePath != tablePath)
        {
            tablePath = channel.samplePath;
            table = tablePath.empty() ? std::vector<float> {} : sawerReadTable (tablePath);
        }
        loadedState = channel.pluginState;
        loadedPoly = mp;
        rebuild (rate.load());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        if (plugin != nullptr)
            plugin->slide (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents, samples);
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

    void allNotesOff() noexcept override
    {
        take();
        if (plugin != nullptr)
            for (int k = 0; k < 128; ++k)
                plugin->noteOff (k);
    }

    bool idle() const noexcept override { return plugin == nullptr || plugin->idle(); }

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
        plugin->render (left, right, n, -1.0);
    }

private:
    void rebuild (double sr) noexcept
    {
        loadedRate = sr;
        if (! stateHeader (loadedState) || table.size() != (std::size_t) tableSize)
        {
            clearPlugin.store (true);
            return;
        }
        try
        {
            auto p = std::make_unique<Plugin>();
            p->eng.tempo = tempo.load();
            p->init (loadedState, table, sr, loadedPoly);
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
    std::mutex buildMutex;
    std::atomic<double> rate { 44100.0 }, tempo { 120.0 };
    std::atomic<float> pitchCents { 0.0f };
    std::vector<std::uint8_t> loadedState;
    std::vector<float> table;
    std::string tablePath;
    double loadedRate = 0.0;
    int loadedPoly = 0;
    double channelVol = 1.0, channelPan = 0.0;
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

bool sawerStateUsable (const std::vector<std::uint8_t>& state) { return stateHeader (state); }

int sawerParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    if (index < 0 || index >= paramCount || ! stateHeader (state))
        return -1;
    return 12 + 4 * index;
}

std::optional<float> sawerParam (const std::vector<std::uint8_t>& state, int index)
{
    const auto o = sawerParamOffset (state, index);
    if (o < 0)
        return std::nullopt;
    float v;
    std::memcpy (&v, state.data() + o, 4);
    return v;
}

std::unique_ptr<Instrument> makeSawerInstrument() { return std::make_unique<SawerInstrument>(); }

std::vector<float> sawerReadTable (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        auto p = path;
        std::replace (p.begin(), p.end(), '\\', '/');
        in.open (p, std::ios::binary);
    }
    if (! in)
        return {};
    std::vector<char> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (b.size() < (std::size_t) tableSize * 4)
        return {};
    std::vector<float> t ((std::size_t) tableSize);
    std::memcpy (t.data(), b.data(), (std::size_t) tableSize * 4);
    for (auto x : t)
        if (! std::isfinite (x))
            return {};
    return t;
}

namespace sawer
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<float>& table, const std::vector<RefNote>& notes,
                                    double sampleRate, int length, int block, double bpm, int maxPoly, bool panLaw)
{
    if (! stateHeader (state) || length <= 0)
        return {};
    auto plugin = std::make_unique<Plugin>();
    plugin->panLaw = panLaw;
    plugin->eng.tempo = bpm;
    plugin->init (state, table, sampleRate, maxPoly);
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    block = std::max (1, block);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r (l.size());
    for (int n0 = 0; n0 < length;)
    {
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
} // namespace sawer
} // namespace lynx
