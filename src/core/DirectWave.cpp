#include "core/DirectWave.h"
#include "core/HostSlide.h"

#include "core/BassDrum.h"
#include "core/PitchShifterFl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Behaviour per docs/analise/REA_rodada72.md. Every float operation keeps the order and precision the plugin uses
// (single precision unless noted), so that the output matches the DLL's bit for bit on the same data.

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace lynx
{
namespace dw
{
namespace
{
// ------------------------------------------------------------------------------------------------- bits
float fbits (std::uint32_t b) { float f; std::memcpy (&f, &b, 4); return f; }

const float kTwoPi = fbits (0x40c90fdbu);     // 6.2831855f
const float kPi = fbits (0x40490fdbu);
const float kInvPi = fbits (0x3ea2f983u);
const float kTwoInvPi = fbits (0x3f22f983u);
const float kInt16 = fbits (0x38000100u);     // 1/32767
const float kTiny = fbits (0x283424dcu);      // 1e-14 (added against denormals)
const float kRand15 = fbits (0x38000000u);    // 1/32768
const float kRandLfo = fbits (0x38800000u);   // 1/16384
const float kQBase = fbits (0x3f3504f3u);     // 0.70710677f
const float kQRes = fbits (0x3f8147aeu);      // 1.01f
const double kFrac = 2.3283064365386963e-10;  // 2^-32
const double kOne32 = 4294967296.0;

// ------------------------------------------------------------------------------------------------- reading
struct Chunk { std::uint32_t id; const std::uint8_t* data; std::size_t size; };

template <typename Fn>
void forEachChunk (const std::uint8_t* p, std::size_t n, Fn&& fn)
{
    std::size_t o = 0;
    while (o + 12 <= n)
    {
        std::uint32_t id; std::uint64_t sz;
        std::memcpy (&id, p + o, 4);
        std::memcpy (&sz, p + o + 4, 8);
        if (sz > n - o - 12)
            return;
        fn (Chunk { id, p + o + 12, (std::size_t) sz });
        o += 12 + (std::size_t) sz;
    }
}

float rf (const std::uint8_t* p) { float f; std::memcpy (&f, p, 4); return f; }
std::int32_t ri (const std::uint8_t* p) { std::int32_t v; std::memcpy (&v, p, 4); return v; }
std::uint16_t ru16 (const std::uint8_t* p) { std::uint16_t v; std::memcpy (&v, p, 2); return v; }

void readZone (const Chunk& z, Zone& zone)
{
    int filters = 0, envs = 0, lfos = 0, mods = 0;
    forEachChunk (z.data, z.size, [&] (const Chunk& c) {
        const auto* b = c.data;
        switch (c.id)
        {
            case 0x1F4:
                if (c.size >= 25)
                {
                    zone.root = b[0]; zone.lowKey = b[1]; zone.highKey = b[2]; zone.lowVel = b[3]; zone.highVel = b[4];
                    zone.exclusiveGroup = b[5]; zone.output = b[6]; zone.parallelFilters = b[8] != 0;
                    zone.gain = rf (b + 9); zone.pan = rf (b + 13); zone.altGroup = ri (b + 17);
                }
                break;
            case 0x1F5: break;
            case 0x1F6: zone.path.assign ((const char*) b, c.size); break;
            case 0x1F7:
                if (c.size >= 40)
                {
                    std::memcpy (&zone.frames, b, 8);
                    zone.channels = ri (b + 8); zone.beats = ri (b + 12); zone.rate = rf (b + 16);
                    zone.loopMode = ri (b + 20); zone.loopA = ri (b + 24); zone.loopB = ri (b + 28);
                    zone.offset = zone.frames < (std::int64_t) (std::uint32_t) ri (b + 32) ? 0 : ri (b + 32);
                    zone.bits = ri (b + 36);
                }
                break;
            case 0x1F8:
                if (c.size >= 8)
                {
                    zone.pitch = rf (b); zone.coarse = (std::int8_t) b[4]; zone.fine = (std::int8_t) b[5];
                    zone.keyTrack = (std::int16_t) ru16 (b + 6);
                }
                break;
            case 0x1F9:
                if (c.size >= 14)
                {
                    zone.stretchMode = b[0]; zone.stretchSync = b[1];
                    zone.stretch[0] = rf (b + 2); zone.stretch[1] = rf (b + 6); zone.stretch[2] = rf (b + 10);
                }
                break;
            case 0x1FA:
                if (c.size >= 16)
                    for (int i = 0; i < 4; ++i)
                        zone.sends[i] = rf (b + 4 * i);
                break;
            case 0x1FB:
                if (filters < 2 && c.size >= 20)
                {
                    auto& f = zone.filter[filters++];
                    f.type = b[0]; f.cutoff = rf (b + 4); f.res = rf (b + 8); f.shape = rf (b + 12); f.extra = rf (b + 16);
                }
                break;
            case 0x1FC:
                if (c.size >= 2) { zone.syncTempo = b[0] != 0; zone.sliceKeys = b[1] != 0; }
                break;
            case 0x1FD:
                if (c.size >= 16)
                    for (int i = 0; i < 4; ++i)
                        zone.amp[i] = rf (b + 4 * i);
                break;
            case 0x1FE: case 0x1FF: case 0x200: case 0x201:
                if (c.size >= 9)
                {
                    const auto k = (int) c.id - 0x1FE;
                    zone.fxOn[k] = b[0]; zone.fx[k][0] = rf (b + 1); zone.fx[k][1] = rf (b + 5);
                }
                break;
            case 0x202:
                if (envs < 2 && c.size >= 16)
                {
                    for (int i = 0; i < 4; ++i)
                        zone.modEnv[envs][i] = rf (b + 4 * i);
                    ++envs;
                }
                break;
            case 0x203:
                if (lfos < 2 && c.size >= 20)
                {
                    auto& l = zone.lfo[lfos++];
                    l.type = b[0]; l.sync = b[1]; l.rate = rf (b + 4); l.phase = rf (b + 8); l.fade = rf (b + 12); l.extra = rf (b + 16);
                }
                break;
            case 0x204:
                if (mods < 16 && c.size >= 8)
                {
                    auto& m = zone.mods[(std::size_t) mods++];
                    m.src = ru16 (b); m.dst = ru16 (b + 2); m.amount = rf (b + 4);
                }
                break;
            case 0x205:
                zone.embedded.assign (b, b + c.size);
                break;
            default: break;
        }
    });
}

void readProgram (const Chunk& top, Program& p)
{
    int lfos = 0, values = 0;
    forEachChunk (top.data, top.size, [&] (const Chunk& c) {
        const auto* b = c.data;
        switch (c.id)
        {
            case 0x64:
                if (c.size >= 30)
                {
                    p.index = ri (b);
                    p.voiceMode = b[4];
                    p.glideLegatoOnly = b[5] != 0;
                    p.gain = rf (b + 6);
                    p.glide = rf (b + 10);
                }
                break;
            case 0x66: p.name.assign ((const char*) b, c.size); break;
            case 0x68:
                if (c.size >= 10) { p.shape1 = b[0] != 0; p.shape2 = b[1] != 0; p.shape1Amount = rf (b + 2); p.shape2Amount = rf (b + 6); }
                break;
            case 0x69:
                if (c.size >= 18)
                {
                    p.delayOn = b[0] != 0; p.delayCross = b[1] != 0;
                    for (int i = 0; i < 4; ++i) p.delay[i] = rf (b + 2 + 4 * i);
                }
                break;
            case 0x6A:
                if (c.size >= 17) { p.reverbOn = b[0] != 0; for (int i = 0; i < 4; ++i) p.reverb[i] = rf (b + 1 + 4 * i); }
                break;
            case 0x6B:
                if (c.size >= 17) { p.chorusOn = b[0] != 0; for (int i = 0; i < 4; ++i) p.chorus[i] = rf (b + 1 + 4 * i); }
                break;
            case 0x6C:
                if (lfos < 2 && c.size >= 20)
                {
                    auto& l = p.lfo[lfos++];
                    l.type = b[0]; l.sync = b[1]; l.rate = rf (b + 4); l.phase = rf (b + 8); l.fade = rf (b + 12); l.extra = rf (b + 16);
                }
                break;
            case 0x6D:
                if (values < 4 && c.size == 4)
                    p.modValue[values++] = rf (b);
                break;
            case 0x6E:
                if (c.size >= 13)
                {
                    const auto i = ri (b);
                    if (i >= 1 && i <= 99)
                    {
                        auto& g = p.groups[(std::size_t) i];
                        g.mode = b[4] > 3 ? 0 : b[4];
                        g.probability = rf (b + 5);
                        g.velTolerance = rf (b + 9);
                    }
                }
                break;
            case 3:
            {
                Zone z;
                readZone (c, z);
                p.zones.push_back (std::move (z));
                break;
            }
            default: break;
        }
    });
}
} // namespace

std::optional<State> parseState (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 16)
        return std::nullopt;
    std::uint32_t version = 0;
    std::memcpy (&version, d.data(), 4);
    if (version < 36 || version > 38)
        return std::nullopt;
    State s;
    forEachChunk (d.data() + 4, d.size() - 4, [&] (const Chunk& c) {
        if (c.id == 6 && c.size >= 4)
            s.flags = (std::uint32_t) ri (c.data);
        else if (c.id == 1)
        {
            Program p;
            readProgram (c, p);
            if (! p.zones.empty())
                s.programs.push_back (std::move (p));
        }
    });
    if (s.programs.empty())
        return std::nullopt;
    return s;
}

// ================================================================================================= CRT math
// The plugin's run-time library (AMD-derived, FMA3 code paths on the machines FL runs on): see REA_rodada72 §CRT.
namespace
{
double dbits (std::uint64_t b) { double v; std::memcpy (&v, &b, 8); return v; }

// polynomial coefficients of the run-time's single-precision sine and cosine (on |r| ≤ π/4, in double)
const double kS1 = -0.16666666666666666, kS2 = 0.0083333333333333332, kS3 = -0.00019841269841269841, kS4 = 2.7557319223985893e-06;
const double kC1 = 0.041666666666666664, kC2 = -0.0013888888888888887, kC3 = 2.4801587301587298e-05, kC4 = -2.7557319223985888e-07;
const double kPio2Hi = 1.5707963267341256, kPio2Lo = 6.0771005065061922e-11, kTwoOverPi = 0.63661977236758138;

double sinPoly (double r)
{
    const double x2 = r * r;
    double p = std::fma (x2, kS4, kS3);
    p = std::fma (p, x2, kS2);
    p = std::fma (p, x2, kS1);
    return std::fma (p, r * x2, r);
}

// cosine polynomial; `fusedHead`: 1 − x²/2 as one fused step (sine's reduced path) or rounded first (cosine's)
double cosPoly (double r, bool fusedHead)
{
    const double x2 = r * r;
    const double head = fusedHead ? std::fma (x2, -0.5, 1.0) : 1.0 - x2 * 0.5;
    double p = std::fma (x2, kC4, kC3);
    p = std::fma (p, x2, kC2);
    p = std::fma (p, x2, kC1);
    return std::fma (p, x2 * x2, head);
}

// Cody–Waite reduction by π/2 for |x| below 2^20·π/2: region and remainder
bool reduce (double ax, int& region, double& r)
{
    if (ax >= 1.0e6)
        return false;
    const int n = (int) std::fma (ax, kTwoOverPi, 0.5);
    const double dn = (double) n;
    const double hi = std::fma (-dn, kPio2Hi, ax);
    const double t = dn * kPio2Lo;
    r = hi - t;
    region = n & 3;
    return true;
}
} // namespace

float sinfCrt (float x)
{
    if (! std::isfinite (x))
        return (float) std::sin ((double) x);
    const double xd = (double) x;
    const double ax = std::fabs (xd);
    if (ax <= 0.78539816339744828)
    {
        if (ax < 0.0078125)
        {
            if (ax < 0.0001220703125)
                return x;
            return (float) std::fma (-(xd * xd * xd), 0.16666666666666666, xd);
        }
        return (float) sinPoly (xd);
    }
    int region; double r;
    if (! reduce (ax, region, r))
        return (float) std::sin (xd);
    double y = (region & 1) ? cosPoly (r, true) : sinPoly (r);
    if (region >= 2) y = -y;
    if (xd < 0.0) y = -y;
    return (float) y;
}

float cosfCrt (float x)
{
    if (! std::isfinite (x))
        return (float) std::cos ((double) x);
    const double xd = (double) x;
    const double ax = std::fabs (xd);
    if (ax <= 0.78539816339744828)
    {
        if (ax < 0.0078125)
        {
            if (ax < 0.0001220703125)
                return 1.0f;
            return (float) std::fma (-(xd * 0.5), xd, 1.0);
        }
        return (float) cosPoly (xd, false);
    }
    int region; double r;
    if (! reduce (ax, region, r))
        return (float) std::cos (xd);
    double y = (region & 1) ? sinPoly (r) : cosPoly (r, false);
    if (((region + 1) >> 1) & 1) y = -y;
    return (float) y;
}

float expfCrt (float x)
{
    static const auto table = [] {
        std::array<double, 64> t {};
        for (int i = 0; i < 64; ++i)
            t[(std::size_t) i] = std::exp2 (i / 64.0);
        return t;
    }();
    const double xd = (double) x;
    const double t = xd * 92.332482616893657;
    if (! std::isfinite (xd) || t >= 8192.0 || t < -9600.0)
        return (float) std::exp (xd);
    const auto k = (int) std::nearbyint (t);
    const double r = std::fma (-(double) k, 0.010830424696249145, xd);
    const double p = std::fma (std::fma (0.16666666666666666, r, 0.5), r * r, r);
    const double e = table[(std::size_t) (k & 63)];
    const double q = std::fma (p, e, e);
    return (float) (q * dbits ((std::uint64_t) ((k - (k & 63)) / 64 + 0x3ff) << 52));
}

float powfCrt (float x, float y) { return bassdrum::powfMsvc (x, y); }
double powCrt (double x, double y) { return std::pow (x, y); }

// ================================================================================================= engine
namespace
{
// the host gives Final.Pitch (float, 100·key − 6000 + offset); the voice takes (Final.Pitch + 6000) − 100·note
float hostCents (int key, int note, double cents)
{
    const auto fp = (float) (100.0 * key - 6000.0 + cents);
    return (fp + 6000.0f) - (float) note * 100.0f;
}

std::uint32_t crtRand (std::uint32_t& s)
{
    s = s * 0x343fdu + 0x269ec3u;
    return (s >> 16) & 0x7fffu;
}

// rate per step of a time value v (0…1): 1/(v⁴·r·10 + r·0.001), 1 when v ≤ 0
float rateOf (float v, float r)
{
    if (v <= 0.0f)
        return 1.0f;
    return 1.0f / (v * v * v * v * r * 10.0f + r * fbits (0x3a83126fu));
}

struct Env
{
    int state = 0;
    float level = 0.0f;

    void start() { state = 1; level = 0.0f; }
    void release() { if (state > 0) state = 4; }
    float tick (float a, float dcy, float s, float r)
    {
        if (state == 4)
        {
            level = level - r;
            if (! (level > 0.0f)) { state = 0; level = 0.0f; return level * level; }
        }
        else if (state == 2)
        {
            level = level - dcy;
            if (! (level > s)) { level = s; state = 3; return level * level; }
        }
        else if (state == 1)
        {
            level = a + level;
            if (! (level < 1.0f)) { level = 1.0f; state = 2; }
        }
        return level * level;
    }
};

struct LfoState
{
    float phase = 0.0f, fade = 0.0f, held = 0.0f, smooth = 0.0f;

    void reset (float startPhase)
    {
        fade = 0.0f;
        if (startPhase < 1.0f)
            phase = kTwoPi * startPhase;
    }

    float tick (int type, float inc, float fadeInc, std::uint32_t& rnd)
    {
        float v = 0.0f;
        switch (type)
        {
            case 0: v = sinfCrt (phase); break;
            case 1: v = std::fabs (sinfCrt (phase)); break;
            case 2: v = phase < kPi ? phase * kTwoInvPi - 1.0f : 3.0f - phase * kTwoInvPi; break;
            case 3: v = phase < kPi ? -1.0f : 1.0f; break;
            case 4: v = kInvPi * phase - 1.0f; break;
            case 5: v = 1.0f - kInvPi * phase; break;
            case 6: v = held; break;
            case 7: smooth = (held - smooth) * inc + smooth; v = smooth; break;
            default: v = 0.0f; break; // (an uninitialised register in the plugin)
        }
        phase = inc + phase;
        if (! (phase < kTwoPi))
        {
            const auto r = (int) crtRand (rnd);
            phase = phase - kTwoPi;
            held = (float) (r - 0x4000) * kRandLfo;
        }
        float f = fadeInc + fade;
        fade = f;
        if (f > 1.0f)
        {
            fade = 1.0f;
            f = 1.0f;
        }
        return f * v;
    }
};

void designBiquad (float* c, int type, float hz, float q, float sr)
{
    {
        if (type == 0)
        {
            c[0] = 1.0f; c[1] = c[2] = c[3] = c[4] = 0.0f;
            return;
        }
        const float w = (kTwoPi * hz) / sr;
        const float s = sinfCrt (w), cs = cosfCrt (w);
        const float alpha = s / (q + q);
        const float a0 = alpha + 1.0f;
        const float inv = 1.0f / a0;
        const float a1 = (cs * -2.0f) * inv;
        const float a2 = (1.0f - alpha) * inv;
        switch (type)
        {
            case 1:
                c[1] = (1.0f - cs) * inv;
                c[0] = c[2] = ((1.0f - cs) * 0.5f) * inv;
                c[3] = a1; c[4] = a2;
                break;
            case 2:
            {
                const float p = cs + 1.0f;
                c[0] = c[2] = (p * 0.5f) * inv;
                c[1] = (-p) * inv;
                c[3] = a1; c[4] = a2;
                break;
            }
            case 3:
                c[0] = (s * 0.5f) * inv;
                c[1] = 0.0f;
                c[2] = ((-s) * 0.5f) * inv;
                c[3] = a1; c[4] = a2;
                break;
            case 4:
                c[0] = c[2] = inv;
                c[1] = c[3] = a1;
                c[4] = a2;
                break;
            case 5:
                c[0] = c[4] = a2;
                c[2] = a0 * inv;
                c[1] = c[3] = a1;
                break;
            default: break;
        }
    }
}

struct Biquad
{
    float c[5] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }; // b0, b1, b2, a1, a2
    void design (int type, float hz, float q, float sr) { designBiquad (c, type, hz, q, sr); }
};


// filter type 6 (FUN_18003b860 / FUN_18003b4b0 / FUN_18003b680): a four-pole ladder in double precision whose
// one-sample transition is the 4096th power of a small step (12 squarings), a soft saturation of the first stage,
// and a DC blocker shared by both channels
struct Ladder
{
    double sr = 44100.0;
    double s[2][4] {};
    double m[20] {}; // m[4·i + j]: input i (state 0…3, then the input) → state j
    double dc = 0.0;

    void clear() { std::fill (&s[0][0], &s[0][0] + 8, 0.0); dc = 0.0; }

    void set (float hz, float res)
    {
        const float fc = hz <= 20000.0f ? hz : 20000.0f;
        double g = ((double) fc * 6.25) / (sr * 4096.0);
        g = 0.0 <= g ? g : 0.0;
        g = g <= 1.0 ? g : 1.0;
        const auto k = (double) -(res * fbits (0x4081999au));
        const double a = 1.0 - g;
        double* p = m - 9; // p[9…28] as the plugin numbers them
        p[0x19] = g;
        double kg = k * g;
        p[9] = a;
        const double ag = a * g;
        p[0x15] = kg;
        kg = kg * g;
        p[0x1a] = g * g;
        const double ag2 = ag * g;
        const double g3 = g * g * g;
        p[0x16] = kg;
        kg = kg * g;
        p[0x1b] = g3;
        p[0xc] = ag2 * g;
        p[10] = ag; p[0xe] = a; p[0xb] = ag2; p[0xf] = ag; p[0x13] = a; p[0x17] = kg; p[0x10] = ag2; p[0x14] = ag;
        p[0x18] = g * kg + a;
        p[0x1c] = g3 * g;
        p[0xd] = 0.0; p[0x11] = 0.0; p[0x12] = 0.0;
        for (int it = 0; it < 12; ++it)
        {
            double o[29];
            std::copy (p + 9, p + 29, o + 9);
            const double d32 = o[0xd], d20 = o[0x11], d28 = o[0x15], d27 = o[9], d29 = o[10], d30 = o[0xe], d1 = o[0xb], d2 = o[0xc];
            const double d3 = o[0x12], d4 = o[0x19], d5 = o[0x16], d6 = o[0xf], d7 = o[0x13], d8 = o[0x17], d11 = o[0x1a];
            const double d31 = o[0x14], d12 = o[0x10], d13 = o[0x18], d14 = o[0xc];
            const double d23 = d27 * d14 + d29 * d12 + d1 * d31 + d14 * d13;
            const double d24 = d32 * d14 + o[0xe] * d12 + o[0xf] * d31 + d12 * d13;
            const double d21 = d20 * d14 + o[0x12] * d12 + o[0x13] * d31 + d31 * d13;
            const double d22 = d28 * d14 + o[0x16] * d12 + o[0x17] * d31 + d13 * d13;
            const double d31n = d4 * d14 + o[0x1a] * d12 + o[0x1b] * d31 + o[0x1c] * d13 + o[0x1c];
            p[9] = d27 * d27 + d32 * d29 + d20 * d1 + d28 * d2;
            p[10] = d29 * d27 + d30 * d29 + d3 * d1 + d5 * d2;
            p[0xb] = d1 * d27 + d6 * d29 + d7 * d1 + d8 * d2;
            p[0xc] = d23;
            p[0xd] = d32 * d27 + d32 * d30 + d20 * o[0xf] + d28 * o[0x10];
            p[0xe] = d29 * d32 + d30 * d30 + d3 * o[0xf] + d5 * o[0x10];
            p[0xf] = d1 * d32 + d6 * d30 + d7 * d6 + d8 * o[0x10];
            p[0x10] = d24;
            p[0x11] = d20 * d27 + d32 * d3 + d20 * o[0x13] + d28 * o[0x14];
            p[0x12] = d29 * d20 + d30 * d3 + d3 * o[0x13] + d5 * o[0x14];
            p[0x13] = d1 * d20 + d6 * o[0x12] + d7 * d7 + d8 * o[0x14];
            p[0x14] = d21;
            p[0x15] = d28 * d27 + d32 * o[0x16] + d20 * o[0x17] + d28 * o[0x18];
            p[0x16] = d29 * d28 + d30 * d5 + d3 * o[0x17] + d5 * o[0x18];
            p[0x17] = d1 * d28 + d6 * o[0x16] + d7 * d8 + d8 * o[0x18];
            p[0x18] = d22;
            p[0x19] = d4 * d27 + d32 * o[0x1a] + d20 * o[0x1b] + d28 * o[0x1c] + d4;
            p[0x1a] = d29 * d4 + d30 * o[0x1a] + d3 * o[0x1b] + d5 * o[0x1c] + o[0x1a];
            p[0x1b] = d1 * d4 + d6 * d11 + d7 * o[0x1b] + d8 * o[0x1c] + o[0x1b];
            p[0x1c] = d31n;
        }
    }

    float run (int ch, float x)
    {
        double* st = s[ch];
        const double* p = m - 9;
        const double s0 = st[0], s1 = st[1], s2 = st[2], s3 = st[3];
        const auto in = (double) ((x + fbits (0x3eb33333u)) * fbits (0x3e4ccccdu));
        double y = s0 * p[0xc] + s1 * p[0x10] + s2 * p[0x14] + s3 * p[0x18] + in * p[0x1c];
        st[3] = y;
        y = y / 0.20000000298023224;
        const double v = s0 * p[9] + s1 * p[0xd] + s2 * p[0x11] + s3 * p[0x15] + in * p[0x19];
        const double den = v * v + 1.0;
        st[1] = s1 * p[0xe] + s0 * p[10] + s2 * p[0x12] + s3 * p[0x16] + in * p[0x1a];
        st[0] = v / den;
        st[2] = s1 * p[0xf] + s0 * p[0xb] + s2 * p[0x13] + s3 * p[0x17] + in * p[0x1b];
        dc = (y - dc) * 0.004999999888241291 + dc;
        return (float) (y - dc);
    }
};


// filter type 7 (FUN_180044f60, FUN_180044e50): a vowel filter — eight constant-skirt band-passes per channel at
// formant frequencies morphed between three vowel tables by the cutoff, scaled by 2^((res − ½)·1.7)
struct Vowel
{
    float sr = 44100.0f, cut = 0.0f, res = 0.0f;
    float band[2][8][10] {}; // per band: frequency, Q, rate, gain, 2cos/a0, (1 − α)/a0, x2, x1, y2, y1

    static void design (float* b, float f, float q, float rate)
    {
        if (b[0] == f && q == b[1] && rate == b[2])
            return;
        b[0] = f;
        b[1] = q;
        b[2] = rate;
        const double w = (double) f * 6.2831853071795898 / (double) rate;
        const double sn = std::sin (w), cs = std::cos (w);
        const double alpha = sn / ((double) q + (double) q);
        const double a0 = alpha + 1.0;
        b[3] = (float) ((double) (float) (sn * 0.5) / a0);
        b[4] = (float) ((double) (float) (cs + cs) / a0);
        b[5] = (float) ((double) (float) (1.0 - alpha) / a0);
    }

    void run (float& l, float& r)
    {
        static const float tA[8] { 700.0f, 1250.0f, 2500.0f, 3500.0f, 4200.0f, 8500.0f, 10000.0f, 12000.0f };
        static const float tB[8] { 250.0f, 700.0f, 2500.0f, 3500.0f, 4200.0f, 8500.0f, 10000.0f, 12000.0f };
        static const float tC[8] { 250.0f, 2500.0f, 2700.0f, 3500.0f, 4200.0f, 8500.0f, 10000.0f, 12000.0f };
        const auto gain = (float) powCrt (2.0, (double) ((res - 0.5f) * fbits (0x3fd9999au)));
        const float pos = cut * 3.0f;
        const int i = (int) pos;
        const int sel = i < 3 ? i : 0;
        const float fr = pos - (float) i;
        float wA = 0.0f, wB = 0.0f, wC = 0.0f;
        if (sel >= 0)
        {
            if (sel == 0) { wA = fr; wB = 1.0f - fr; }
            else if (sel == 1) { wA = 1.0f - fr; wC = fr; }
            else { wB = fr; wC = 1.0f - fr; }
        }
        const float scale = 1.0f - (float) sel * 0.125f;
        float sl = 0.0f, sr2 = 0.0f;
        for (int k = 0; k < 8; ++k)
        {
            const float f = (wA * tA[k] + wB * tB[k] + wC * tC[k]) * gain;
            design (band[0][k], f, 12.0f, sr);
            design (band[1][k], f, 12.0f, sr);
            float* b = band[0][k];
            const float xl = l;
            const float x2 = b[6];
            b[6] = b[7]; b[7] = xl;
            const float y2 = b[8];
            b[8] = b[9];
            float y = ((xl - x2) * b[3] + b[9] * b[4]) - b[5] * y2;
            b[9] = y;
            sl = sl + y * scale;
            b = band[1][k];
            const float xr = r;
            const float x1r = b[7], y2r = b[8];
            b[7] = xr; b[8] = b[9];
            const float x2r = b[6];
            b[6] = x1r;
            y = ((xr - x2r) * b[3] + b[9] * b[4]) - b[5] * y2r;
            b[9] = y;
            sr2 = sr2 + y * scale;
        }
        l = l * 0.0f + sl;
        r = r * 0.0f + sr2;
    }
};

// two passes of the same biquad, then an optional clip at ±(1 − shape)
struct BiquadState
{
    float s[8] {};

    void clear() { std::fill (std::begin (s), std::end (s), 0.0f); }

    float run (float x, float shape, const float* c)
    {
        const float t = ((x * c[0] + s[2] * c[1]) + c[2] * s[3] - s[0] * c[3]) - s[1] * c[4];
        s[3] = s[2]; s[1] = s[0]; s[2] = x; s[0] = t;
        const float u = ((t * c[0] + s[6] * c[1]) + c[2] * s[7] - s[4] * c[3]) - c[4] * s[5];
        s[7] = s[6]; s[6] = t; s[5] = s[4]; s[4] = u;
        if (shape > 0.0f)
        {
            const float lim = 1.0f - shape;
            float y = u <= lim ? u : lim;
            const float nl = -lim;
            return nl <= y ? y : nl;
        }
        return u;
    }
};

constexpr int kMaxBlock = 256;
constexpr int kMaxBlockFx = 256;

float cutoffHz (float v) { return powfCrt (2.0f, (v * 127.0f) / 12.0f - 5.0f) * 440.0f; }
void designBiquad (float* c, int type, float hz, float q, float sr);

// ------------------------------------------------------------------------------------------------- program effects
float tanfCrt (float x) { return (float) std::tan ((double) x); } // equal to the plugin's on the arguments used (EXEC)

struct SingleBiquad
{
    float s[4] {}; // y1, y2, x1, x2
    float run (float x, const float* c)
    {
        const float y = (((s[2] * c[1] + x * c[0]) + c[2] * s[3]) - s[0] * c[3]) - c[4] * s[1];
        s[3] = s[2]; s[2] = x; s[1] = s[0]; s[0] = y;
        return y;
    }
};

// FUN_180011f10: stereo delay, allpass-interpolated read, high- and low-pass in the feedback, optional cross feedback
struct DelayFx
{
    SingleBiquad hp[2], lp[2];
    std::vector<float> buf[2];
    float time = 32768.0f, fb = 0.5f, y[2] {}; // the delay's reset (FUN_180011a?0 family) start values
    std::uint32_t wp = 0;

    void run (const Program& P, float sr, float bpm, int n, float (*bus)[kMaxBlockFx], std::uint32_t& rnd)
    {
        if (buf[0].empty()) { buf[0].assign (65536, 0.0f); buf[1].assign (65536, 0.0f); }
        float target = sr * 60.0f / bpm * P.delay[0];
        target = std::max (target, sr * fbits (0x3a83126fu));
        const float k = 16.0f / sr;
        const float q = std::sqrt (2.0f) * 0.5f;
        float ch[5], cl[5];
        designBiquad (ch, 2, cutoffHz (P.delay[2]), q, sr);
        designBiquad (cl, 1, cutoffHz (P.delay[3]), q, sr);
        const float fbT = P.delay[1];
        for (int i = 0; i < n; ++i)
        {
            const float noise = (float) (int) crtRand (rnd) * fbits (0x283424dcu);
            const auto w16 = wp & 0xffffu;
            const int ti = (int) time;
            const float f = time - (float) ti;
            const float w = 1.0f - f;
            const auto r0 = (std::uint32_t) (ti + (int) wp) & 0xffffu, r1 = (std::uint32_t) (ti + (int) wp + 1) & 0xffffu;
            const float y1 = (buf[1][r0] * w + buf[1][r1]) - w * y[1];
            const float y0 = (w * buf[0][r0] + buf[0][r1]) - w * y[0];
            y[1] = y1; y[0] = y0;
            if (P.delayCross)
            {
                buf[0][w16] = lp[0].run (hp[0].run (y1 * fb + (noise + bus[2][i]), ch), cl);
                buf[1][w16] = lp[1].run (hp[1].run (y[0] * fb + noise, ch), cl);
            }
            else
            {
                buf[0][w16] = lp[0].run (hp[0].run (y0 * fb + (noise + bus[2][i]), ch), cl);
                buf[1][w16] = lp[1].run (hp[1].run (y[1] * fb + (noise + bus[3][i]), ch), cl);
            }
            --wp;
            bus[0][i] = y[0] + bus[0][i];
            bus[1][i] = bus[1][i] + y[1];
            time = ((target - time) * k + noise) + time;
            fb = ((fbT - fb) * k + noise) + fb;
        }
    }
};


// FUN_18002d0b0 (+ FUN_18002c9a0): plate reverb — input low-pass and DC blocker, four input allpasses, a one-sample
// pre-delay, a two-branch tank (damping one-pole + shelf, decay allpasses, delays) and fixed output taps
struct ReverbFx
{
    static constexpr int kLen[12] { 0x8e, 0x6b, 0x17b, 0x115, 0x2a0, 0x1165, 0x708, 0xe88, 0x38c, 0x1079, 0xa60, 0xc5b };
    static constexpr int kTap[14] { 0x10a, 0xb9e, 0x779, 0x9c0, 0x69a, 0xbb, 0x61e, 0x161, 0xe2b, 0x6c0, 0xc65, 0x713, 0x14f, 0x26d };
    int size = -1, rate = -1;
    int pos[12] {}, len[12] {}, tap[14] {};
    int prePos = 0, preLen = 1;
    std::vector<float> line[12], pre;
    float lp = 0.0f, hpY = 0.0f, hpX = 0.0f;  // +0xb4, +0xb8, +0xbc
    float shA = 0.0f, shAy = 0.0f, dampA = 0.0f; // +0xc0, +0xc4, +0xd0
    float shB = 0.0f, shBy = 0.0f, dampB = 0.0f; // +0xc8, +0xcc, +0xd4

    void allocate (int sr)
    {
        if (sr == rate)
            return;
        rate = sr;
        for (int k = 0; k < 12; ++k)
        {
            line[k].assign ((std::size_t) std::max (1, (sr * kLen[k]) / 20000) + 1, 0.0f);
            pos[k] = 0;
        }
        pre.assign ((std::size_t) std::max (1, (sr * 250) / 1000), fbits (0x0da24260u)); // starts at 1e-30 [EXEC]
        prePos = 0;
    }

    void run (const Program& P, float sr, int n, float (*bus)[kMaxBlockFx])
    {
        const float roomV = P.reverb[0], dampV = P.reverb[1], diffV = P.reverb[2], decayV = P.reverb[3];
        const int isr = (int) sr;
        allocate (isr);
        const float lnK = fbits (0xc0dd0c55u); // logf(0.001)
        float t = tanfCrt ((kPi * 94.5f) / sr);
        const float aHp = (t - 1.0f) / (t + 1.0f);
        float c = 2.0f - cosfCrt ((kTwoPi * 22000.0f) / sr);
        float sq = c * c - 1.0f;
        sq = std::sqrt (sq);
        const float lpK = 1.0f - (c - sq);
        preLen = std::max (1, (int) ((sr * 0.0f) * fbits (0x3a83126fu)));
        prePos = prePos % preLen;
        const int sizeI = (int) (roomV * 99.0f + 1.0f);
        const int base = ((sizeI + 15) * isr) / 115;
        for (int k = 0; k < 12; ++k)
        {
            len[k] = std::max (1, (base * kLen[k]) / 20000);
            len[k] = std::min (len[k], (int) line[k].size());
            pos[k] = pos[k] % len[k];
        }
        for (int j = 0; j < 14; ++j)
            tap[j] = (base * kTap[j]) / 20000;
        if (sizeI != size)
        {
            size = sizeI;
            for (int k = 0; k < 12; ++k)
                std::fill (line[k].begin(), line[k].begin() + len[k], 0.0f);
        }
        float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        if (diffV > 0.0f)
        {
            a1 = expfCrt (((((float) (len[0] + len[1]) * 0.5f) / sr) * lnK) / (diffV * fbits (0x3d8f5c29u)));
            a2 = expfCrt (((((float) (len[8] + len[4]) * 0.5f) / sr) * lnK) / (diffV * fbits (0x3eb33333u)));
            a3 = expfCrt (((((float) (len[6] + len[10]) * 0.5f) / sr) * lnK) / (diffV * fbits (0x3f028f5cu)));
        }
        float dd = decayV * 199.0f;
        dd = dd * decayV + 1.0f;
        dd = dd / 10.0f;
        const float g = powfCrt (expfCrt ((((float) (len[11] + len[9] + len[7] + len[5]) / sr) * lnK) / dd), 0.25f);
        const float one = g / g;
        const float oneMin = std::min (1.0f, one);
        const float tilt = (one - 1.0f) * 0.5f;
        t = tanfCrt ((kPi * 500.0f) / sr);
        const float aSh = (t - oneMin) / (t + oneMin);
        float c2 = 2.0f - cosfCrt ((((dampV * 216.0f + 5.0f) * 100.0f) * kTwoPi) / sr);
        float sq2 = c2 * c2 - 1.0f;
        sq2 = std::sqrt (sq2);
        const float dampK = 1.0f - (c2 - sq2);
        const float gm = 1.0f - g;
        const float k07 = gm * fbits (0x3f333333u) + g, k06 = gm * fbits (0x3f19999au) + g, k025 = gm * 0.25f + g;
        const float k02 = gm * fbits (0x3e4ccccdu) + g, k05 = gm * 0.5f + g;
        auto step = [&] (int k) { if (--pos[k] < 0) pos[k] = len[k] - 1; };
        auto at = [&] (int k, int i) -> float& { return line[k][(std::size_t) i]; };
        for (int i = 0; i < n; ++i)
        {
            const float tiny = fbits (0x0da24260u);
            float x = ((bus[7][i] + bus[6][i]) * 0.5f + tiny - lp) * lpK + lp;
            lp = x;
            const float h = (x - hpY) * aHp + hpX;
            hpX = x;
            hpY = h;
            const float in = (x - h) * 0.5f + tiny;
            // input allpasses
            float u0 = at (0, pos[0]) - in * a1;
            float t0 = u0 * a1;
            at (0, pos[0]) = t0 + in; step (0);
            float u1 = at (1, pos[1]) - t0;
            float t1 = u1 * a1;
            at (1, pos[1]) = t1 + u0; step (1);
            float u2 = at (2, pos[2]) - t1;
            float t2 = u2 * a1;
            at (2, pos[2]) = t2 + u1; step (2);
            float u3 = at (3, pos[3]) - t2;
            at (3, pos[3]) = u3 * a1 + u2; step (3);
            const float pd = pre[(std::size_t) prePos];
            pre[(std::size_t) prePos] = u3;
            if (--prePos < 0) prePos = preLen - 1;
            // branch A
            {
                const float r = g * at (11, pos[11]);
                const float sh = (r - shAy) * aSh + shA;
                shA = r;
                shAy = sh;
                const float v = ((((sh + r) * tilt + r) + pd) - dampA) * dampK + dampA;
                dampA = v;
                float w = at (4, pos[4]) - v * a2;
                at (4, pos[4]) = w * a2 + v; step (4);
                const float dly = at (5, pos[5]);
                at (5, pos[5]) = w; step (5);
                const float q = dly * g;
                float w2 = at (6, pos[6]) - q * a3;
                at (6, pos[6]) = w2 * a3 + q; step (6);
                at (7, pos[7]) = w2; step (7);
            }
            // branch B
            {
                const float r = g * at (7, pos[7]);
                const float sh = (r - shBy) * aSh + shB;
                shB = r;
                shBy = sh;
                const float v = ((((sh + r) * tilt + r) + pd) - dampB) * dampK + dampB;
                dampB = v;
                float w = at (8, pos[8]) - v * a2;
                at (8, pos[8]) = w * a2 + v; step (8);
                const float dly = at (9, pos[9]);
                at (9, pos[9]) = w; step (9);
                const float q = dly * g;
                float w2 = at (10, pos[10]) - q * a3;
                at (10, pos[10]) = w2 * a3 + q; step (10);
                at (11, pos[11]) = w2; step (11);
            }
            auto wrap = [&] (int v, int k) { return v < len[k] ? v : v - len[k]; };
            const float rR = ((((k025 * at (5, wrap (tap[8] + pos[5], 5)) + at (5, wrap (tap[7] + pos[5], 5)))
                                - k07 * at (6, wrap (tap[9] + pos[6], 6)))
                               - k02 * at (7, wrap (tap[10] + pos[7], 7)))
                              - k06 * at (9, wrap (tap[11] + pos[9], 9)))
                             + at (10, wrap (pos[10] + tap[12], 10)) + k05 * at (11, wrap (pos[11] + tap[13], 11));
            const float rL = (((at (9, wrap (tap[0] + pos[9], 9)) - k025 * at (9, wrap (tap[1] + pos[9], 9)))
                               - k07 * at (10, wrap (tap[2] + pos[10], 10)))
                              - k02 * at (11, wrap (pos[11] + tap[3], 11)))
                             + k06 * at (5, wrap (tap[4] + pos[5], 5)) + at (6, wrap (tap[5] + pos[6], 6))
                             + k05 * at (7, wrap (tap[6] + pos[7], 7));
            bus[1][i] = rR + bus[1][i];
            bus[0][i] = rL + bus[0][i];
        }
    }
};

// FUN_1800119c0: stereo chorus, two allpass-interpolated taps moved by quadrature sines, feedback
struct ChorusFx
{
    std::vector<float> buf[2];
    float d[2] { 8192.0f, 8192.0f }, fb = 0.5f, phase = 0.0f, y[2] {}; // FUN_180011970's start values
    std::int32_t wp = 0;

    void run (const Program& P, float sr, int n, float (*bus)[kMaxBlockFx], std::uint32_t& rnd)
    {
        if (buf[0].empty()) { buf[0].assign (16384, 0.0f); buf[1].assign (16384, 0.0f); }
        const float ms = sr * fbits (0x3a83126fu);
        const float k = 16.0f / sr;
        float depth = P.chorus[1] * P.chorus[1];
        depth = depth * 24.0f;
        depth = depth * ms;
        depth = depth * 0.5f;
        float centre = P.chorus[0] * P.chorus[0];
        centre = centre * fbits (0x423f999au) + fbits (0x3dcccccdu);
        centre = centre * ms;
        const float mid = depth + centre;
        float inc = P.chorus[2] * kTwoPi;
        inc = inc * P.chorus[2];
        inc = inc * 20.0f;
        inc = inc / sr;
        const float fbT = P.chorus[3];
        for (int i = 0; i < n; ++i)
        {
            const float noise = (float) (int) crtRand (rnd) * fbits (0x283424dcu);
            const float sl = sinfCrt (phase) * depth + mid;
            const float sr2 = sinfCrt (kTwoPi * 0.25f + phase) * depth + mid;
            const int il = (int) d[0], ir = (int) d[1];
            const float wl = 1.0f - (d[0] - (float) il);
            const float wr = 1.0f - (d[1] - (float) ir);
            const float yl = (wl * buf[0][(std::uint32_t) (wp + il) & 0x3fffu] + buf[0][(std::uint32_t) (wp + il + 1) & 0x3fffu]) - wl * y[0];
            y[0] = yl;
            const float yr = (wr * buf[1][(std::uint32_t) (wp + ir) & 0x3fffu] + buf[1][(std::uint32_t) (wp + ir + 1) & 0x3fffu]) - wr * y[1];
            y[1] = yr;
            bus[0][i] = yl + bus[0][i];
            bus[1][i] = y[1] + bus[1][i];
            const auto w = (std::uint32_t) wp & 0x3fffu;
            buf[0][w] = (y[0] * fb + bus[4][i]) + noise;
            buf[1][w] = (y[1] * fb + bus[5][i]) + noise;
            float ph = inc + phase;
            --wp;
            phase = ph;
            if (ph > kTwoPi) phase = ph - kTwoPi;
            d[0] = ((sl - d[0]) * k + noise) + d[0];
            d[1] = ((sr2 - d[1]) * k + noise) + d[1];
            fb = ((fbT - fb) * k + noise) + fb;
        }
    }
};
constexpr int kVoices = 256;

struct Voice
{
    // host values
    float pan = 0.0f, vel = 0.0f, cents = 0.0f;
    std::uint64_t id = 0;
    int group = 0, zone = 0, prevNote = 0, note = 0;
    bool start = false, ended = false, legato = false, released = false;
    float glide = 0.0f, random = 0.0f, follow = 0.0f;
    float cutA = 0.0f, cutB = 0.0f, gainL = 0.0f, gainR = 0.0f;
    float lim = 1.0f, lim2 = 1.0f;
    float ringPhase = 0.0f, shCount = 0.0f, shHold[2] {};      // +0x58…+0x64
    float fxA = 0.0f, fxD = 0.0f, ringMix = 0.0f, shMix = 0.0f, crushMix = 0.0f, phMix = 0.0f; // +0x68…+0x7c
    float chain[2][9] {};                                    // +0x89c, +0x8c0
    std::uint64_t pos = 0; // high 32 bits: frame, low 32 bits: fraction
    std::int64_t step = 0;
    double speed = 0.0;
    int bounceDir = 0, loopState = 0;
    int loopStart = 0, loopEnd = 0, loopLen = 0, end = 0;
    Env amp, menv[2];
    LfoState lfo[2];
    BiquadState fa[2], fb[2];
    Ladder la, lb;
    Vowel va, vb;

    std::uint32_t frame() const { return (std::uint32_t) (pos >> 32); }
    std::uint32_t frac() const { return (std::uint32_t) pos; }
    void setFrame (std::uint32_t f) { pos = ((std::uint64_t) f << 32) | (pos & 0xffffffffu); }
    void setFrac (std::uint32_t f) { pos = (pos & 0xffffffff00000000ull) | f; }
    bool playing() const { return amp.state > 0 || start; }
};
} // namespace

struct Engine::Impl
{
    std::uint32_t& randState;
    explicit Impl (std::uint32_t& r) : randState (r) {}
    Program prog;
    std::vector<std::shared_ptr<const ZoneSamples>> samples;
    EngineOptions opt;
    float sr = 44100.0f, invSr = 1.0f / 44100.0f, bpm = 120.0f;
    float smoothK = 0.0f, limUp = 0.0f, limDown = 0.0f, twoPiOverSr = 0.0f;
    std::array<Voice, kVoices> voices;
    LfoState progLfo[2];
    DelayFx delayFx;
    ChorusFx chorusFx;
    ReverbFx reverbFx;
    float progLfoOut[2] {};
    int lastNote = 0;
    int heldVel[128] {};
    std::uint64_t heldId[128] {};
    float heldFinal[128][3] {};
    int altLast[100];

    void releaseVoice (Voice& v, bool force)
    {
        if (v.amp.state >= 1 && v.amp.state <= 3 && v.zone >= 0 && v.zone < (int) prog.zones.size()
            && (prog.zones[(std::size_t) v.zone].loopMode != 1 || force))
        {
            v.amp.release();
            v.menv[0].release();
            v.menv[1].release();
        }
        if (v.start)
        {
            v.start = false;
            v.amp.state = 0;
        }
    }

    Voice* freeVoice()
    {
        for (auto& v : voices)
            if (v.amp.state == 0 && ! v.start)
                return &v;
        return nullptr;
    }

    void initVoice (Voice& v, int zi, int note, std::uint64_t id, bool legato, float fv, float fp, float cents)
    {
        // the plugin reuses the voice record: what its start does not set (the LFOs' phase and held values) carries over
        const LfoState keep[2] { v.lfo[0], v.lfo[1] };
        const Ladder la = v.la, lb = v.lb;
        const Vowel va = v.va, vb = v.vb;
        v = Voice();
        v.lfo[0] = keep[0];
        v.lfo[1] = keep[1];
        v.la = la; v.lb = lb; v.va = va; v.vb = vb;
        v.start = true;
        v.zone = zi;
        v.prevNote = lastNote;
        v.note = note;
        v.legato = legato;
        v.group = prog.zones[(std::size_t) zi].exclusiveGroup;
        v.id = id;
        v.vel = fv;
        v.pan = fp;
        v.cents = cents;
    }

    // FUN_180024f90: zone choice (key, velocity with the group's tolerance, alternation groups), exclusive groups,
    // poly / mono / legato voices
    void startNote (std::uint64_t id, int note, int vel, float fv, float fp, float cents)
    {
        bool held = false;
        for (int k = 0; k < 128; ++k)
            if (heldVel[k] > 0)
                held = true;
        progLfo[0].reset (prog.lfo[0].phase);
        progLfo[1].reset (prog.lfo[1].phase);
        std::vector<int> cand[100];
        for (int zi = 0; zi < (int) prog.zones.size(); ++zi)
        {
            const auto& z = prog.zones[(std::size_t) zi];
            if (note < z.lowKey || note > z.highKey)
                continue;
            const auto g = (std::size_t) std::clamp (z.altGroup, 0, 99);
            const auto tol = (float) ((double) prog.groups[g].velTolerance * 127.0);
            if ((float) z.lowVel - tol <= (float) vel && (float) vel <= (float) z.highVel + tol)
                cand[g].push_back (zi);
        }
        for (int g = 0; g < 100; ++g)
        {
            auto& c = cand[g];
            if (c.empty())
                continue;
            const auto& G = prog.groups[(std::size_t) g];
            if (! (1.0f <= G.probability))
            {
                // the plugin draws from rand_s (operating-system randomness) here: not reproducible
                const auto u = (float) (std::uint32_t) (randState * 2654435761u) * fbits (0x2f800000u);
                if (! (u <= G.probability)) { c.clear(); continue; }
            }
            const int n = (int) c.size();
            if (n != 1)
            {
                int u = -1;
                auto pick = [&] (int m) { return m > 0 ? (int) crtRand (randState) / (0x7fff / m) : 0; };
                if (G.mode == 1)
                    u = altLast[g] + 1 < n ? altLast[g] + 1 : 0;
                else if (G.mode == 3 && altLast[g] > -1)
                {
                    u = pick (n - 1);
                    if (altLast[g] <= u) ++u;
                }
                else if (G.mode == 2 || G.mode == 3)
                    u = pick (n);
                if (u >= 0)
                {
                    u = std::min (u, n - 1);
                    const int z = c[(std::size_t) u];
                    c.assign (1, z);
                    altLast[g] = u;
                }
            }
            for (const int zi : c)
            {
                const auto& z = prog.zones[(std::size_t) zi];
                if (z.exclusiveGroup != 0)
                    for (auto& v : voices)
                        if (v.amp.state >= 1 && v.amp.state <= 3 && v.group == z.exclusiveGroup)
                            releaseVoice (v, true);
                if (prog.voiceMode == 1)
                    for (auto& v : voices)
                    {
                        v.ended = v.ended || v.playing();
                        releaseVoice (v, true);
                    }
                if (prog.voiceMode == 2)
                {
                    Voice* lv = nullptr;
                    for (auto& v : voices)
                        if (v.amp.state >= 1 && v.amp.state <= 3) { lv = &v; break; }
                    if (lv)
                    {
                        lv->prevNote = lastNote;
                        lv->note = note;
                        lv->id = id;
                        continue;
                    }
                }
                if (auto* v = freeVoice())
                    initVoice (*v, zi, note, id, held, fv, fp, cents);
            }
        }
        heldVel[note] = vel;
        heldId[note] = id;
        heldFinal[note][0] = fv; heldFinal[note][1] = fp; heldFinal[note][2] = cents;
        lastNote = note;
    }
    // slot (MIDI channel 0) controllers
    float bend = 0.0f, wheel = 0.0f, volume = 100.0f / 127.0f, slotPan = 0.5f, expression = 1.0f;
    int bendRange = 2;
    // per block
    float buf[2][kMaxBlock] {};
    float bus[8][kMaxBlockFx] {}; // dry L/R, delay L/R, chorus L/R, reverb L/R
    std::uint32_t* rnd = nullptr;

    void setRate (float r)
    {
        sr = r;
        invSr = r != 0.0f ? 1.0f / r : 0.0f;
        smoothK = expfCrt (-1.0f / (r * fbits (0x3ba3d70au)));
        limUp = expfCrt (-1000.0f / r);
        limDown = expfCrt (-10.0f / r);
        twoPiOverSr = kTwoPi / r;
    }

    // ----------------------------------------------------------------------------------------- one voice, one block
    void renderVoice (Voice& v, int n, float blockRate, float lfoScale, float blockTempo)
    {
        if (v.zone < 0 || v.zone >= (int) prog.zones.size())
        {
            v.amp.state = 0; v.start = false;
            return;
        }
        const Zone& z = prog.zones[(std::size_t) v.zone];
        const ZoneSamples* zs = samples.size() > (std::size_t) v.zone ? samples[(std::size_t) v.zone].get() : nullptr;
        const std::int64_t zFrames = zs && zs->channels > 0 ? zs->frames : z.frames;
        const int zChannels = zs && zs->channels > 0 ? zs->channels : z.channels;
        if (zChannels == 0 || zFrames == 0)
        {
            v.amp.state = 0; v.start = false;
            return;
        }
        if (v.start)
        {
            v.pos = 0;
            v.follow = 0.0f;
            v.menv[0].start();
            v.menv[1].start();
            v.lfo[0].reset (z.lfo[0].phase);
            v.lfo[1].reset (z.lfo[1].phase);
            v.amp.start();
            v.random = (float) (int) crtRand (*rnd) * kRand15;
            if (! (0.0f < prog.glide) || (prog.glideLegatoOnly && ! v.legato))
                v.glide = (float) v.note;
            else
                v.glide = (float) v.prevNote;
        }
        if (v.amp.state < 1)
            return;
        const auto frames = (std::uint32_t) zFrames;
        if (frames <= v.frame())
            v.setFrame (frames);

        // modulation sources
        const float me0 = v.menv[0].tick (rateOf (z.modEnv[0][0], blockRate), rateOf (z.modEnv[0][1], blockRate), z.modEnv[0][2],
                                          rateOf (z.modEnv[0][3], blockRate));
        const float me1 = v.menv[1].tick (rateOf (z.modEnv[1][0], blockRate), rateOf (z.modEnv[1][1], blockRate), z.modEnv[1][2],
                                          rateOf (z.modEnv[1][3], blockRate));
        float lfoOut[2];
        for (int k = 0; k < 2; ++k)
        {
            const auto& L = z.lfo[k];
            const float fadeInc = rateOf (L.fade, blockRate);
            const float r2 = L.rate * L.rate;
            float inc = lfoScale * r2;
            if (L.sync)
            {
                const auto beats = (int) ((double) r2 * 127.5 + 1.0);
                inc = (float) ((double) kTwoPi / ((double) beats * (double) blockTempo));
            }
            lfoOut[k] = v.lfo[k].tick (L.type, inc, fadeInc, *rnd);
        }

        // destinations
        float D[48] {};
        D[1] = z.pitch;
        D[2] = opt.velocityAsInitVol ? z.gain : 1.0f;
        {
            const float p = v.pan * 0.5f + z.pan;
            D[3] = 0.0f > p ? 0.0f : std::min (1.0f, p);
        }
        D[7] = z.filter[0].cutoff; D[8] = z.filter[0].res; D[9] = z.filter[0].shape;
        D[10] = z.filter[1].cutoff; D[11] = z.filter[1].res; D[12] = z.filter[1].shape;
        for (int i = 0; i < 4; ++i) D[13 + i] = z.amp[i];
        D[17] = z.fx[0][0]; D[18] = z.fx[0][1]; D[19] = z.fx[1][0]; D[20] = z.fx[1][1];
        D[21] = z.fx[2][0]; D[22] = z.fx[2][1]; D[23] = z.fx[3][0]; D[24] = z.fx[3][1];
        for (int i = 0; i < 4; ++i) D[25 + i] = z.sends[i];
        D[29] = z.stretch[0]; D[30] = z.stretch[1]; D[31] = z.stretch[2];
        for (int k = 0; k < 16; ++k) D[32 + k] = z.mods[(std::size_t) k].amount;
        bool modStart = false, modLoopA = false, modLoopB = false, follows = false;
        for (const auto& m : z.mods)
        {
            if (m.dst == 4) modStart = true;
            else if (m.dst == 5) modLoopA = true;
            else if (m.dst == 6) modLoopB = true;
        }
        const auto fr = (int) frames;
        if (modStart) D[4] = (float) z.offset / (float) fr;
        if (modLoopA) D[5] = (float) z.loopA / (float) fr;
        if (modLoopB) D[6] = (float) z.loopB / (float) fr;

        auto source = [&] (int src, float a, float& dst, bool& added) {
            // returns the term to add in `a` (already multiplied), or handles additive forms itself
            added = false;
            switch (src)
            {
                case 0: case 7: case 8: dst = dst + 0.0f; added = true; return a;
                case 1: dst = dst + ((float) v.note / 127.0f) * a; added = true; return a;
                case 2: return a * v.vel;
                case 3: return a * wheel;
                case 4: return a * bend;
                case 5: return a * progLfoOut[0];
                case 6: return a * progLfoOut[1];
                case 9: return a * me0;
                case 10: return a * me1;
                case 11: return a * lfoOut[0];
                case 12: return a * lfoOut[1];
                case 13: follows = true; return a * v.follow;
                case 14: dst = dst + ((float) (std::int64_t) v.frame() / (float) fr) * a; added = true; return a;
                case 15: return a * v.random;
                case 16: case 17: case 18: case 19: return a * prog.modValue[src - 16];
                default: added = true; return a; // no change
            }
        };
        // pass 1: slots that modulate the amount of an earlier slot, from the last slot down
        for (int k = 15; k >= 0; --k)
        {
            const auto& m = z.mods[(std::size_t) k];
            const int dst = m.dst;
            if (dst <= 31 || dst - k >= 32)
                continue;
            if (dst >= 48)
                continue;
            float a = (D[32 + k] - 0.5f);
            a = a + a;
            a = a * a * a;
            float x = D[dst];
            bool added;
            const float t = source (m.src, a, x, added);
            if (! added)
                x = x + t;
            D[dst] = std::min (1.0f, std::max (0.0f, x));
        }
        // pass 2: the others, from the first slot up
        for (int k = 0; k < 16; ++k)
        {
            const auto& m = z.mods[(std::size_t) k];
            const int dst = m.dst;
            if (dst > 31 && dst - k < 32)
                continue;
            if (dst >= 48)
                continue;
            float a = (D[32 + k] - 0.5f);
            a = a + a;
            a = a * a * a;
            float x = D[dst];
            bool added = false;
            if (m.src == 2 && ! opt.velocityAsInitVol && dst == 2)
            {
                x = x * (a * v.vel);
                added = true;
            }
            else
            {
                const float t = source (m.src, a, x, added);
                if (! added)
                    x = x + t;
            }
            if (! (m.src == 2 && ! opt.noGainClamp && dst == 2))
                x = std::min (1.0f, std::max (0.0f, x));
            D[dst] = x;
        }

        // sample points
        int startFrame = z.offset, la = z.loopA, lb = z.loopB;
        if (modStart) startFrame = (int) ((double) D[4] * (double) fr);
        if (modLoopA) la = (int) ((double) D[5] * (double) fr);
        if (modLoopB) lb = (int) ((double) D[6] * (double) fr);
        v.loopEnd = std::max (la, lb);
        v.loopStart = std::min (la, lb);
        v.loopLen = v.loopEnd - v.loopStart;
        v.end = fr;

        // pitch, glide, speed
        {
            const float target = (float) v.note;
            const float dist = std::fabs ((float) (v.note - v.prevNote));
            const float g = rateOf (prog.glide, blockRate) * dist;
            if (v.glide > target)
            {
                v.glide = v.glide - g;
                if (! (target < v.glide)) v.glide = target;
            }
            if (target > v.glide)
            {
                const float ng = g + v.glide;
                v.glide = ng;
                if (! (ng < target)) v.glide = target;
            }
        }
        float cents = (v.glide - (float) z.root) * (float) z.keyTrack;
        cents = cents + (float) z.coarse * 100.0f;
        cents = cents + (float) z.fine;
        cents = cents + (D[1] * 2400.0f - 1200.0f);
        cents = cents + (float) bendRange * bend;
        cents = cents + v.cents;
        double speed = powCrt (2.0, (double) cents / 1200.0) * (double) z.rate / (double) sr;
        if (! (speed < 32768.0)) // (absurd pitches: keep the 32.32 step defined)
            speed = 32768.0;
        v.speed = speed;
        v.step = (std::int64_t) (speed * kOne32);

        const bool filters = z.filter[0].type != 0 || z.filter[1].type != 0;
        const float relTime = v.ended ? 0.25f : D[16];
        const float aRate = rateOf (D[13], sr), dRate = rateOf (D[14], sr), rRate = rateOf (relTime, sr);

        // gains
        float g = D[2];
        if (! opt.velocityAsInitVol)
            g = g * z.gain;
        g = g * prog.gain;
        g = g * volume;
        g = g * expression;
        float gl = std::sqrt ((1.0f - slotPan) * (1.0f - D[3]));
        float gr = std::sqrt (slotPan * D[3]);
        if (opt.velocityAsInitVol)
        {
            gl = gl * 2.0f * g;
            gr = gr * 2.0f * g;
        }
        const float targetL = gl * g, targetR = gr * g;

        if (v.start)
        {
            v.lim = 1.0f; v.lim2 = 1.0f;
            v.fa[0].clear(); v.fa[1].clear(); v.fb[0].clear(); v.fb[1].clear();
            v.cutA = D[7]; v.cutB = D[10];
            v.fxA = D[17]; v.ringMix = D[18]; v.shMix = D[20]; v.crushMix = D[22]; v.fxD = D[23]; v.phMix = D[24];
            v.ringPhase = v.shCount = v.shHold[0] = v.shHold[1] = 0.0f;
            std::fill (&v.chain[0][0], &v.chain[0][0] + 18, 0.0f);
            v.gainL = targetL; v.gainR = targetR;
            v.setFrame ((std::uint32_t) startFrame);
            v.bounceDir = 0; v.loopState = 0;
            if (z.sliceKeys && z.beats > 0 && v.note >= z.root)
                v.setFrame ((std::uint32_t) (int) (std::int64_t) ((double) ((v.note - z.root) % z.beats) * ((double) fr / (double) z.beats)));
        }
        v.start = false;

        // ------------------------------------------------------------------ sample reading
        float* L = buf[0];
        float* R = buf[1];
        const bool stereo = zChannels == 2;
        auto sampleAt = [&] (int ch, std::uint32_t i) -> float {
            if (! zs)
                return 0.0f;
            const auto idx = (std::size_t) i + 256;
            if (zs->bits == 32)
            {
                if ((std::size_t) ch >= zs->f32.size())
                    return 0.0f;
                const auto& a = zs->f32[(std::size_t) ch];
                return idx < a.size() ? a[idx] : 0.0f;
            }
            if ((std::size_t) ch >= zs->i16.size())
                return 0.0f;
            const auto& a = zs->i16[(std::size_t) ch];
            return idx < a.size() ? (float) a[idx] * kInt16 : 0.0f;
        };
        auto rawAt = [&] (int ch, std::uint32_t i) -> float { // 16-bit: the integer value as float; float: the value
            if (! zs)
                return 0.0f;
            const auto idx = (std::size_t) i + 256;
            if (zs->bits == 32)
            {
                if ((std::size_t) ch >= zs->f32.size())
                    return 0.0f;
                const auto& a = zs->f32[(std::size_t) ch];
                return idx < a.size() ? a[idx] : 0.0f;
            }
            if ((std::size_t) ch >= zs->i16.size())
                return 0.0f;
            const auto& a = zs->i16[(std::size_t) ch];
            return idx < a.size() ? (float) a[idx] : 0.0f;
        };
        auto readOneShot = [&] (int i) {
            const auto f = v.frame();
            const float x = (float) ((double) v.frac() * kFrac);
            if (! stereo)
            {
                const float s0 = sampleAt (0, f), s1 = sampleAt (0, f + 1);
                L[i] = R[i] = (s1 - s0) * x + s0;
                return;
            }
            const float w = 1.0f - x;
            if (zs && zs->bits != 32)
            {
                L[i] = (rawAt (0, f) * w + rawAt (0, f + 1) * x) * kInt16;
                R[i] = (rawAt (1, f) * w + rawAt (1, f + 1) * x) * kInt16;
            }
            else
            {
                L[i] = w * sampleAt (0, f) + x * sampleAt (0, f + 1);
                R[i] = w * sampleAt (1, f) + x * sampleAt (1, f + 1);
            }
        };
        auto readLoop = [&] (int i) {
            const auto f = v.frame();
            const float x = (float) ((double) v.frac() * kFrac);
            const bool wrap = f == (std::uint32_t) (v.loopEnd - 1) && v.loopState < 2;
            if (! stereo)
            {
                if (wrap)
                {
                    const float s0 = sampleAt (0, f), s1 = sampleAt (0, (std::uint32_t) v.loopStart);
                    L[i] = R[i] = (s1 - s0) * x + s0;
                }
                else
                {
                    const float s0 = sampleAt (0, f), s1 = sampleAt (0, f + 1);
                    L[i] = R[i] = (s1 - s0) * x + s0;
                }
                return;
            }
            if (wrap)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float s0 = sampleAt (ch, f), s1 = sampleAt (ch, (std::uint32_t) v.loopStart);
                    (ch == 0 ? L : R)[i] = (s1 - s0) * x + s0;
                }
                return;
            }
            const float w = 1.0f - x;
            if (zs && zs->bits != 32)
            {
                L[i] = (rawAt (0, f + 1) * x + rawAt (0, f) * w) * kInt16;
                R[i] = (rawAt (1, f + 1) * x + rawAt (1, f) * w) * kInt16;
            }
            else
            {
                L[i] = x * sampleAt (0, f + 1) + w * sampleAt (0, f);
                R[i] = x * sampleAt (1, f + 1) + w * sampleAt (1, f);
            }
        };
        auto endCheck = [&] {
            if ((std::uint32_t) v.end <= v.frame())
            {
                v.setFrame ((std::uint32_t) v.end);
                v.setFrac (0);
                v.ended = true;
                v.amp.release();
            }
        };
        auto loopWrap = [&] {
            const auto f = v.frame();
            if ((std::uint32_t) v.loopEnd <= f)
            {
                int nf = v.loopStart;
                if (v.loopLen > 0)
                {
                    int r = ((int) f - v.loopStart) % v.loopLen;
                    if (r < 0) r += v.loopLen;
                    nf = r + v.loopStart;
                }
                v.setFrame ((std::uint32_t) nf);
                v.loopState = 1;
            }
        };
        std::fill (L, L + n, 0.0f);
        std::fill (R, R + n, 0.0f);
        if (z.stretchMode == 0)
        {
            const int mode = z.loopMode;
            const bool releasedSustain = mode == 3 && v.amp.state > 3;
            if (mode < 2 || releasedSustain)
            {
                v.loopState = 2;
                for (int i = 0; i < n; ++i)
                {
                    readOneShot (i);
                    v.pos += (std::uint64_t) v.step;
                    endCheck();
                }
            }
            if (mode == 2 || (mode == 3 && ! releasedSustain))
            {
                for (int i = 0; i < n; ++i)
                {
                    readLoop (i);
                    v.pos += (std::uint64_t) v.step;
                    loopWrap();
                }
            }
            if (mode == 4)
            {
                for (int i = 0; i < n; ++i)
                {
                    readOneShot (i);
                    auto back = [&] {
                        const auto p = (std::int64_t) v.pos;
                        v.pos = (std::uint64_t) (p - v.step);
                        if (v.frame() <= (std::uint32_t) v.loopStart || p - v.step < 0)
                            v.bounceDir = 0;
                    };
                    if (v.bounceDir == 0)
                    {
                        v.pos += (std::uint64_t) v.step;
                        if ((std::uint32_t) v.loopEnd <= v.frame())
                        {
                            v.bounceDir = 1;
                            back();
                        }
                    }
                    else if (v.bounceDir == 1)
                        back();
                }
            }
        }

        // envelope follower (source 13)
        if (follows)
        {
            float f = v.follow;
            for (int i = 0; i < n; ++i)
            {
                const float p = std::max (std::fabs (L[i]), std::fabs (R[i]));
                if (f < p) f = p;
                f = f * smoothK;
            }
            v.follow = f;
        }

        // ------------------------------------------------------------------ filters
        if (filters)
        {
            const float k = expfCrt (-1.0f / (blockRate * fbits (0x3ba3d70au)));
            Biquad ca, cb;
            const float hzA = flCutoff (v.cutA);
            const int tA = z.filter[0].type, tB = z.filter[1].type;
            if (tA < 6)
                ca.design (tA, hzA, kQBase / (kQRes - D[8]), sr);
            else if (tA == 6)
            {
                v.la.sr = (double) sr;
                v.la.set (hzA, D[8]);
            }
            else if (tA == 7)
            {
                v.va.sr = sr;
                v.va.cut = v.cutA;
                v.va.res = D[8];
            }
            const float hzB = flCutoff (v.cutB);
            if (tB < 6)
                cb.design (tB, hzB, kQBase / (kQRes - D[11]), sr);
            else if (tB == 6)
            {
                v.lb.sr = (double) sr;
                v.lb.set (hzB, D[11]);
            }
            else if (tA == 7) // (the plugin tests filter A's type here)
            {
                v.vb.sr = sr;
                v.vb.cut = v.cutB;
                v.vb.res = D[11];
            }
            v.cutA = (v.cutA - D[7]) * k + D[7];
            v.cutB = (v.cutB - D[10]) * k + D[10];
            const float shA = D[9], shB = D[12];
            float pl = 0.0f, pr = 0.0f; // the previous outputs (what an unknown type leaves)
            for (int i = 0; i < n; ++i)
            {
                const float noise = (float) (int) crtRand (*rnd) * kTiny;
                const float xl = noise + L[i];
                const float xr = noise + R[i];
                float yl = pl, yr = pr;
                if (tA < 6) { yl = v.fa[0].run (xl, shA, ca.c); yr = v.fa[1].run (xr, shA, ca.c); }
                else if (tA == 6) { yl = v.la.run (0, xl); yr = v.la.run (1, xr); }
                else if (tA == 7) { yl = xl; yr = xr; v.va.run (yl, yr); }
                if (! z.parallelFilters)
                {
                    if (tB < 6) { yl = v.fb[0].run (yl, shB, cb.c); yr = v.fb[1].run (yr, shB, cb.c); }
                    else if (tB == 6) { yl = v.lb.run (0, yl); yr = v.lb.run (1, yr); }
                    else if (tB == 7) v.vb.run (yl, yr);
                }
                else
                {
                    if (tB < 6) { yl = yl + v.fb[0].run (xl, shB, cb.c); yr = yr + v.fb[1].run (xr, shB, cb.c); }
                    else if (tB == 6) { yl = yl + v.lb.run (0, xl); yr = yr + v.lb.run (0, xr); }
                    else if (tB == 7)
                    {
                        float bl = xl, br = xr;
                        v.vb.run (bl, br);
                        yl = yl + bl;
                        yr = yr + br;
                    }
                }
                pl = yl; pr = yr;
                L[i] = yl;
                R[i] = yr;
                limiter (L[i], R[i], v.lim);
            }
        }


        // ------------------------------------------------------------------ zone effects (0x1FE…0x201)
        const float smoothK2 = smoothK;
        if (z.fxOn[0]) // ring modulator: sine at the cutoff law of D17, mix D18
        {
            const float tgA = D[17], tgM = D[18];
            for (int i = 0; i < n; ++i)
            {
                const float inc = flCutoff (tgA) * twoPiOverSr;
                const float sn = sinfCrt (v.ringPhase);
                L[i] = (L[i] * sn) * v.ringMix + (1.0f - v.ringMix) * L[i];
                R[i] = (R[i] * sn) * v.ringMix + (1.0f - v.ringMix) * R[i];
                float ph = inc + v.ringPhase;
                v.ringPhase = ph;
                if (! (ph < kTwoPi)) v.ringPhase = ph - kTwoPi;
                v.fxA = (v.fxA - tgA) * smoothK2 + tgA + kTiny;
                v.ringMix = (v.ringMix - tgM) * smoothK2 + tgM + kTiny;
            }
        }
        if (z.fxOn[1]) // sample-and-hold: step 1 − 0.99·D19, mix D20
        {
            const float step = 1.0f - D[19] * fbits (0x3f7d70a4u), tgM = D[20];
            for (int i = 0; i < n; ++i)
            {
                if (! (v.shCount < 1.0f))
                {
                    v.shHold[0] = L[i];
                    v.shHold[1] = R[i];
                    v.shCount = v.shCount - 1.0f;
                }
                L[i] = (1.0f - v.shMix) * L[i] + v.shHold[0] * v.shMix;
                R[i] = (1.0f - v.shMix) * R[i] + v.shMix * v.shHold[1];
                v.shCount = step + v.shCount;
                v.shMix = (v.shMix - tgM) * smoothK2 + tgM + kTiny;
            }
        }
        if (z.fxOn[2]) // bit crusher: 3 + trunc(13·(1 − D21)) bits, mix D22
        {
            const int bits = 3 - (int) ((1.0f - D[21]) * -13.0f);
            const float q = (float) (1 << (bits & 31));
            const float tgM = D[22];
            for (int i = 0; i < n; ++i)
            {
                const float ql = (float) (int) (q * L[i]) / q;
                const float qr = (float) (int) (q * R[i]) / q;
                L[i] = ql * v.crushMix + (1.0f - v.crushMix) * L[i];
                R[i] = (1.0f - v.crushMix) * R[i] + v.crushMix * qr;
                v.crushMix = (v.crushMix - tgM) * smoothK2 + tgM + kTiny;
            }
        }
        if (z.fxOn[3]) // phaser: eight first-order allpasses, feedback ½, depth D23, mix D24, then the limiter
        {
            const float tgD = D[23], tgM = D[24];
            auto chainRun = [&] (float* st, float x, float p) {
                const float a = (1.0f - p) / (p + 1.0f);
                const float noise = (float) (int) crtRand (*rnd) * kTiny;
                const float in = 0.5f * st[8] + (noise + x);
                float prev = in, t = in * a;
                for (int k = 0; k < 8; ++k)
                {
                    const float u = st[k] - t;
                    const float tn = u * a;
                    st[k] = tn + prev;
                    prev = u;
                    t = tn;
                }
                st[8] = prev;
                return prev;
            };
            for (int i = 0; i < n; ++i)
            {
                (void) flCutoff (tgD);
                L[i] = chainRun (v.chain[0], L[i], v.fxD) * v.phMix + L[i];
                R[i] = chainRun (v.chain[1], R[i], v.fxD) * v.phMix + R[i];
                limiter (L[i], R[i], v.lim2);
                v.fxD = (v.fxD - tgD) * smoothK2 + tgD + kTiny;
                v.phMix = (v.phMix - tgM) * smoothK2 + tgM + kTiny;
            }
        }

        // ------------------------------------------------------------------ amplitude
        for (int i = 0; i < n; ++i)
        {
            const float e = v.amp.tick (aRate, dRate, D[15], rRate);
            L[i] = e * v.gainL * L[i];
            R[i] = e * v.gainR * R[i];
            v.gainL = (v.gainL - targetL) * smoothK + targetL + kTiny;
            v.gainR = (v.gainR - targetR) * smoothK + targetR + kTiny;
        }

        // ------------------------------------------------------------------ buses
        auto send = [&] (int b, float gain) {
            for (int i = 0; i < n; ++i)
            {
                bus[b][i] = L[i] * gain + bus[b][i];
                bus[b + 1][i] = R[i] * gain + bus[b + 1][i];
            }
        };
        if (prog.delayOn && D[25] > 0.0f) send (2, D[25] * D[25]);
        if (prog.chorusOn && D[26] > 0.0f) send (4, D[26] * D[26]);
        if (prog.reverbOn && D[27] > 0.0f) send (6, D[27] * D[27]);
        if (! (1.0f <= D[28]))
            send (0, D[28] * D[28]);
        else
            for (int i = 0; i < n; ++i)
            {
                bus[0][i] = L[i] + bus[0][i];
                bus[1][i] = R[i] + bus[1][i];
            }
    }

    static float flCutoff (float v) { return powfCrt (2.0f, (v * 127.0f) / 12.0f - 5.0f) * 440.0f; }

    void limiter (float& l, float& r, float& state) const
    {
        float p = std::fabs (l);
        const float q = std::fabs (r);
        if (! (q < p)) p = q;
        const float k = p <= state ? limDown : limUp;
        const float s = (state - p) * k + p;
        state = s;
        if (1.0f < s)
        {
            const float g = powfCrt (s, -1.0f);
            l = g * l;
            r = g * r;
        }
    }
};

Engine::Engine() : d (std::make_unique<Impl> (randState)) { d->rnd = &randState; d->setRate (44100.0f); reset(); }
Engine::~Engine() = default;

void Engine::setProgram (const Program& program, std::vector<std::shared_ptr<const ZoneSamples>> samples, const EngineOptions& o)
{
    d->prog = program;
    d->samples = std::move (samples);
    d->opt = o;
    reset();
}

void Engine::setSampleRate (double sampleRate) { d->setRate ((float) sampleRate); }
void Engine::setTempo (double bpm) { d->bpm = (float) bpm; }

void Engine::reset()
{
    for (auto& v : d->voices)
        v = Voice();
    d->progLfo[0] = d->progLfo[1] = LfoState();
    std::fill (std::begin (d->altLast), std::end (d->altLast), -1);
    std::fill (std::begin (d->heldVel), std::end (d->heldVel), 0);
    std::fill (std::begin (d->heldId), std::end (d->heldId), 0);
}

bool Engine::active() const
{
    for (const auto& v : d->voices)
        if (v.playing())
            return true;
    return false;
}

void Engine::noteOn (std::uint64_t id, int key, double velocity, double finalVol, double finalPan, double cents)
{
    const int note = std::clamp (key, 0, 127);
    const int vel = std::clamp ((int) ((float) velocity * 127.0f), 1, 127);
    d->startNote (id, note, vel, (float) finalVol, (float) finalPan, hostCents (key, note, cents));
}

void Engine::noteOff (std::uint64_t id)
{
    auto& I = *d;
    int note = -1;
    for (const auto& v : I.voices)
        if (v.id == id && (v.playing() || v.start))
            note = v.note;
    for (int k = 0; k < 128; ++k)
        if (I.heldId[k] == id && I.heldVel[k] > 0)
            note = k;
    if (note < 0)
        return;
    I.heldVel[note] = 0;
    I.heldId[note] = 0;
    for (auto& v : I.voices)
        if (v.note == note && v.id == id)
            I.releaseVoice (v, false);
    if (I.prog.voiceMode > 0 && note == I.lastNote)
        for (int k = 0; k < 128; ++k)
            if (I.heldVel[k] > 0)
            {
                I.startNote (I.heldId[k], k, I.heldVel[k], I.heldFinal[k][0], I.heldFinal[k][1], I.heldFinal[k][2]);
                break;
            }
}

void Engine::setLevels (std::uint64_t id, double finalVol, double finalPan, double cents)
{
    for (auto& v : d->voices)
        if (v.id == id && v.playing())
        {
            v.vel = (float) finalVol;
            v.pan = (float) finalPan;
            v.cents = hostCents (v.note, v.note, cents);
        }
}

void Engine::render (float* out, int n)
{
    auto& I = *d;
    const int maxBlock = I.opt.fineBlocks ? 8 : kMaxBlock;
    int done = 0;
    while (done < n)
    {
        const int m = std::min (n - done, maxBlock);
        const float blockRate = I.sr / (float) m;
        const float blockTempo = (float) ((double) blockRate * 15.0 / (double) I.bpm);
        const float lfoScale = (kTwoPi * 20.0f) / blockRate;
        // program LFOs
        for (int k = 0; k < 2; ++k)
        {
            const auto& L = I.prog.lfo[k];
            const float fadeInc = rateOf (L.fade, blockRate);
            const float r2 = L.rate * L.rate;
            float inc = lfoScale * r2;
            if (L.sync)
            {
                const auto beats = (int) ((double) r2 * 127.5 + 1.0);
                inc = (float) ((double) kTwoPi / ((double) beats * (double) blockTempo));
            }
            I.progLfoOut[k] = I.progLfo[k].tick (L.type, inc, fadeInc, randState);
        }
        for (auto& b : I.bus)
            std::fill (b, b + m, 0.0f);
        for (auto& v : I.voices)
            if (v.amp.state > 0 || v.start)
                I.renderVoice (v, m, blockRate, lfoScale, blockTempo);
        if (I.prog.delayOn) I.delayFx.run (I.prog, I.sr, I.bpm, m, I.bus, randState);
        if (I.prog.chorusOn) I.chorusFx.run (I.prog, I.sr, m, I.bus, randState);
        if (I.prog.reverbOn) I.reverbFx.run (I.prog, I.sr, m, I.bus);
        if (I.prog.shape1)
        {
            const float g = 1.0f - I.prog.shape1Amount * 0.75f;
            const float s = 1.0f / (g * g * g);
            for (int i = 0; i < m; ++i)
            {
                I.bus[0][i] = psfl::tanhfCrt (s * I.bus[0][i]) * g;
                I.bus[1][i] = psfl::tanhfCrt (s * I.bus[1][i]) * g;
            }
        }
        if (I.prog.shape2)
        {
            const float g = 1.0f - I.prog.shape2Amount * 0.75f;
            const float s = 1.0f / (g * g * g);
            for (int i = 0; i < m; ++i)
            {
                I.bus[0][i] = sinfCrt (s * I.bus[0][i]) * g;
                I.bus[1][i] = sinfCrt (s * I.bus[1][i]) * g;
            }
        }
        for (int i = 0; i < m; ++i)
        {
            out[2 * (done + i)] = I.bus[0][i] + out[2 * (done + i)];
            out[2 * (done + i) + 1] = I.bus[1][i] + out[2 * (done + i) + 1];
        }
        done += m;
    }
}


// ================================================================================================= LYNX channel
const Program* playedProgram (const State& state)
{
    for (const auto& p : state.programs)
        if (! p.zones.empty())
            return &p;
    return nullptr;
}

std::vector<float> encodeFormats (const std::vector<bool>& int16)
{
    std::vector<float> v { 1.0f, (float) int16.size() };
    for (const bool b : int16)
        v.push_back (b ? 1.0f : 0.0f);
    return v;
}

std::vector<bool> decodeFormats (const std::vector<float>& p)
{
    std::vector<bool> v;
    if (p.size() < 2 || p[0] != 1.0f || p[1] < 0.0f || (std::size_t) p[1] + 2 != p.size())
        return v;
    for (std::size_t i = 2; i < p.size(); ++i)
        v.push_back (p[i] != 0.0f);
    return v;
}

std::shared_ptr<const ZoneSamples> zoneSamples (const SampleData& sample, bool int16)
{
    auto z = std::make_shared<ZoneSamples>();
    const auto n = sample.frames();
    if (n == 0)
        return nullptr;
    z->frames = (std::int64_t) n;
    z->channels = sample.isStereo() ? 2 : 1;
    z->bits = int16 ? 16 : 32;
    for (int c = 0; c < z->channels; ++c)
    {
        const float* src = c == 0 ? sample.leftData() : sample.rightData();
        if (int16)
        {
            std::vector<std::int16_t> v (n + 512, 0);
            for (std::size_t i = 0; i < n; ++i)
                v[i + 256] = (std::int16_t) std::clamp (std::nearbyint ((double) src[i] * 32768.0), -32768.0, 32767.0);
            z->i16.push_back (std::move (v));
        }
        else
        {
            std::vector<float> v (n + 512, 0.0f);
            std::copy (src, src + n, v.begin() + 256);
            z->f32.push_back (std::move (v));
        }
    }
    return z;
}

namespace
{
// a WAV whose audio is 16-bit PCM (format 1, or extensible with the PCM sub-format)
bool isPcm16Wav (const std::vector<std::uint8_t>& w)
{
    if (w.size() < 12 || std::memcmp (w.data(), "RIFF", 4) != 0 || std::memcmp (w.data() + 8, "WAVE", 4) != 0)
        return false;
    std::size_t o = 12;
    while (o + 8 <= w.size())
    {
        std::uint32_t sz = 0;
        std::memcpy (&sz, w.data() + o + 4, 4);
        if (std::memcmp (w.data() + o, "fmt ", 4) == 0 && o + 8 + 16 <= w.size())
        {
            const auto tag = ru16 (w.data() + o + 8), bits = ru16 (w.data() + o + 22);
            bool pcm = tag == 1;
            if (tag == 0xFFFE && sz >= 26 && o + 8 + 26 <= w.size())
                pcm = ru16 (w.data() + o + 8 + 24) == 1;
            return pcm && (bits == 16 || bits == 8); // the plugin keeps 8- and 16-bit PCM as 16-bit integers (EXEC)
        }
        o += 8 + sz + (sz & 1);
    }
    return false;
}

std::vector<std::uint8_t> wavOf (const Zone& z)
{
    const auto bytes = (std::size_t) (z.bits == 16 ? 2 : 4);
    const auto frames = (std::size_t) std::max<std::int64_t> (0, z.frames);
    const auto block = (frames + 512) * bytes;
    if (z.embedded.empty() || z.channels < 1 || z.channels > 2 || (z.bits != 16 && z.bits != 32)
        || block * (std::size_t) z.channels != z.embedded.size() || ! (z.rate >= 1000.0f && z.rate <= 384000.0f))
        return {};
    const int ch = z.channels;
    const auto data = frames * (std::size_t) ch * bytes;
    std::vector<std::uint8_t> w (44 + data);
    auto put32 = [&] (std::size_t o, std::uint32_t v) { std::memcpy (w.data() + o, &v, 4); };
    auto put16 = [&] (std::size_t o, std::uint16_t v) { std::memcpy (w.data() + o, &v, 2); };
    std::memcpy (w.data(), "RIFF", 4); put32 (4, (std::uint32_t) (36 + data)); std::memcpy (w.data() + 8, "WAVEfmt ", 8);
    put32 (16, 16); put16 (20, z.bits == 16 ? 1 : 3); put16 (22, (std::uint16_t) ch);
    const auto rate = (std::uint32_t) std::lround (z.rate);
    put32 (24, rate); put32 (28, rate * (std::uint32_t) (ch * (int) bytes)); put16 (32, (std::uint16_t) (ch * (int) bytes));
    put16 (34, (std::uint16_t) (bytes * 8)); std::memcpy (w.data() + 36, "data", 4); put32 (40, (std::uint32_t) data);
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            std::memcpy (w.data() + 44 + (i * (std::size_t) ch + (std::size_t) c) * bytes,
                         z.embedded.data() + (std::size_t) c * block + (i + 256) * bytes, bytes);
    return w;
}
} // namespace

bool importChannel (ChannelDef& def, const std::vector<std::uint8_t>& state,
                    const std::function<std::string (const std::string&)>& existing,
                    const std::function<std::vector<std::uint8_t> (const std::string&)>& readFile,
                    const std::function<std::string (const std::vector<std::uint8_t>&, const std::string&)>& saveEmbedded)
{
    const auto st = parseState (state);
    if (! st || ! existing)
        return false;
    const auto* prog = playedProgram (*st);
    if (prog == nullptr)
        return false;
    std::vector<SampleZone> zones;
    std::vector<bool> int16;
    int found = 0;
    for (std::size_t i = 0; i < prog->zones.size(); ++i)
    {
        const auto& z = prog->zones[i];
        SampleZone sz;
        sz.rootKey = std::clamp (z.root, 0, 127);
        sz.lowKey = std::clamp (z.lowKey, 0, 127);
        sz.highKey = std::clamp (z.highKey, sz.lowKey, 127);
        sz.lowVelocity = std::clamp (z.lowVel, 0, 127);
        sz.highVelocity = std::clamp (z.highVel, sz.lowVelocity, 127);
        bool i16 = false;
        sz.path = z.path.empty() ? std::string() : existing (z.path);
        if (! sz.path.empty())
            i16 = readFile && isPcm16Wav (readFile (sz.path));
        else if (saveEmbedded)
        {
            const auto wav = wavOf (z);
            if (! wav.empty())
            {
                sz.path = saveEmbedded (wav, def.name + " " + std::to_string (i + 1));
                i16 = z.bits == 16;
            }
        }
        found += sz.path.empty() ? 0 : 1;
        zones.push_back (std::move (sz));
        int16.push_back (i16);
    }
    if (found == 0)
        return false;
    def.instrument = InstrumentKind::directwave;
    def.zones = std::move (zones);
    def.modelParams = encodeFormats (int16);
    def.pluginState = state;
    return true;
}

namespace
{
class DirectWaveInstrument final : public Instrument
{
public:
    explicit DirectWaveInstrument (std::vector<std::shared_ptr<const ZoneSamples>> s) : samples (std::move (s)) { notes.reserve (1024); }

    void prepare (double sr) noexcept override
    {
        rate = sr;
        engine.setSampleRate (sr);
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        tempo.store (bpm);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        if (! loaded)
        {
            loaded = true;
            if (const auto st = parseState (channel.pluginState))
                if (const auto* p = playedProgram (*st))
                {
                    EngineOptions o;
                    o.velocityAsInitVol = (st->flags & 2) != 0;
                    o.noGainClamp = (st->flags & 1) != 0;
                    engine.setProgram (*p, samples, o);
                    usable = true;
                }
        }
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        const auto to = 100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents;
        for (auto& nt : notes)
            nt.slide.start ((float) (to - (100.0 * (nt.key - 60) + nt.cents)), samples);
    }

    void noteOn (int key, float velocity) noexcept override
    {
        if (! usable)
            return;
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        Note n;
        n.id = nextId++;
        n.key = std::clamp (key, 0, 127);
        n.initVol = initVol;
        n.pan = nextPan;
        n.cents = (double) pitchCents.load() + (double) nextCents;
        engine.setTempo (tempo.load());
        engine.noteOn (n.id, n.key, vel, channelVol * initVol, std::clamp ((double) n.pan + channelPan, -1.0, 1.0), n.cents);
        if (notes.size() < 1024)
            notes.push_back (n);
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        for (std::size_t i = 0; i < notes.size();)
            if (notes[i].key == key)
            {
                engine.noteOff (notes[i].id);
                notes.erase (notes.begin() + (std::ptrdiff_t) i);
            }
            else
                ++i;
    }

    void allNotesOff() noexcept override
    {
        for (const auto& n : notes)
            engine.noteOff (n.id);
        notes.clear();
    }

    bool idle() const noexcept override { return ! engine.active(); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (const auto& nt : notes)
                engine.setLevels (nt.id, channelVol * nt.initVol, std::clamp ((double) nt.pan + pan, -1.0, 1.0),
                                  nt.cents + (double) nt.slide.cents);
        }
        if (! usable)
            return;
        float tmp[2 * 256];
        for (int done = 0; done < n;)
        {
            const int len = std::min (256, n - done);
            for (auto& nt : notes) // a slide note moves the voices' Final.Pitch (issue #14)
                if (nt.slide.moving())
                {
                    nt.slide.advance ((float) len);
                    engine.setLevels (nt.id, channelVol * nt.initVol, std::clamp ((double) nt.pan + channelPan, -1.0, 1.0),
                                      nt.cents + (double) nt.slide.cents);
                }
            std::fill (tmp, tmp + 2 * len, 0.0f);
            engine.render (tmp, len);
            for (int i = 0; i < len; ++i)
            {
                left[done + i] += tmp[2 * i];
                right[done + i] += tmp[2 * i + 1];
            }
            done += len;
        }
    }

private:
    struct Note
    {
        std::uint64_t id = 0;
        int key = 0;
        double initVol = 1.0, pan = 0.0, cents = 0.0;
        HostSlide slide {}; // a slide note moving its Final.Pitch (issue #14; the zone stays the key's)
    };
    Engine engine;
    std::vector<std::shared_ptr<const ZoneSamples>> samples;
    std::vector<Note> notes;
    std::uint64_t nextId = 1;
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0;
    std::atomic<double> tempo { 120.0 };
    std::atomic<float> pitchCents { 0.0f };
    float nextPan = 0.0f, nextCents = 0.0f;
    bool loaded = false, usable = false;
};
} // namespace

std::unique_ptr<Instrument> makeDirectWaveInstrument (std::vector<std::shared_ptr<const ZoneSamples>> samples)
{
    return std::make_unique<DirectWaveInstrument> (std::move (samples));
}

std::vector<float> renderReference (const Program& program, const std::vector<std::shared_ptr<const ZoneSamples>>& samples,
                                    const EngineOptions& options, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, int block, std::uint32_t randSeed)
{
    Engine e;
    e.setSampleRate (sampleRate);
    e.setProgram (program, samples, options);
    e.randState = randSeed;
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    block = std::max (1, block);
    std::vector<float> out (2 * (std::size_t) std::max (0, length), 0.0f);
    int t = 0;
    while (t < length)
    {
        for (std::size_t k = 0; k < notes.size(); ++k)
        {
            if (notes[k].start == t)
                e.noteOn (k + 1, notes[k].key, notes[k].velocity, notes[k].finalVol, notes[k].finalPan, notes[k].cents);
            if (notes[k].release == t)
                e.noteOff (k + 1);
        }
        int len = std::min (block, length - t);
        for (const auto ev : evs)
            if (ev > t && ev < t + len)
                len = ev - t;
        e.render (out.data() + 2 * (std::size_t) t, len);
        t += len;
    }
    return out;
}
} // namespace dw
} // namespace lynx
