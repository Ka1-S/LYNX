#include "core/MiniSynth.h"
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
#include <vector>

// 1:1 port of our round-41 reference model (r41/py/msmodel.py). The model emulates the DLL's float32 arithmetic by
// rounding every single-precision result and uses Python floats (doubles) where the DLL computes in double. This file
// keeps those types — `float` where the model holds float32, `double` + an explicit (float) where it rounds a double —
// and the operation order of every expression, so fusing a·b + c into one FMA would change the results: contraction
// stays off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int maxChunk = 512;  // engine buffer of 512 samples per call (§2.2)
constexpr int blockLen = 32;   // control block (§2.3)
constexpr int voiceCount = 99; // §3.1
constexpr int paramCount = 43;
constexpr int tableCount = 1024, tableLen = 2048;

// cvttss2si / cvttsd2si: truncation toward zero; out of range or NaN → INT_MIN
int i32 (double x) noexcept
{
    if (! (x == x) || x >= 2147483648.0 || x < -2147483648.0)
        return std::numeric_limits<int>::min();
    return (int) x;
}
// (int)(long long)x: truncation to 64 bits, low 32 bits kept
std::int32_t i32via64 (double x) noexcept
{
    if (! (x == x) || x >= 9.2e18 || x <= -9.2e18)
        return 0;
    return (std::int32_t) (std::uint32_t) (std::uint64_t) (long long) x;
}
std::int32_t add32 (std::int32_t a, std::int32_t b) noexcept { return (std::int32_t) ((std::uint32_t) a + (std::uint32_t) b); }

// the C runtime's powf as the model computes it: the exact power, rounded to float
float powf32 (double x, double y) noexcept { return (float) std::pow (x, y); }

// ------------------------------------------------------------------------------------------------------- state

constexpr float defaults[paramCount] = { 0.375f, 0.5f,  0.0f, 0.0f, 0.0f, 0.0f,  0.0f,  0.0f,         1.0f, 0.0f,       0.0f,
                                         0.0f,   0.0f,  1.0f, 1.0f, 1.0f, 0.5f,  0.0f,  0.0f,         0.25f, 0.5f,      0.0f,
                                         1.0f,   1.0f,  0.07f, 0.0f, 0.0f, 0.0f, 0.0f,  0.7f,         0.5f, 1.0f,       0.5f,
                                         0.0f,   0.25f, 0.5f, 0.5f, 0.0f, 0.11222222f, 0.4f, 0.1f,     1.0f, 0.5f };

std::uint32_t rd32 (const std::uint8_t* p) noexcept { return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24; }

// "SSLF" + u32 size, then the .flms body: signature (99 46 4C 53 0D 0A 1A 0A; one factory preset has "\x99FLM"),
// "HEAD" + size (u16 1, u16 1, u16 L, L UTF-16 characters of the preset name), "DATA" + size, "0rev" + floats. Like the
// plugin, only the name length and the DATA size are read (an optional 60-byte "CcnK…FPCh" header is skipped); parameter
// i is set while the DATA size minus 4·(i + 1) is still above 3 (§1.1); a value is clamped to 0…1 (NaN is read as 0
// here).
std::optional<std::array<float, paramCount>> parseState (const std::vector<std::uint8_t>& d, std::size_t* floatsAt = nullptr)
{
    if (d.size() < 8 || std::memcmp (d.data(), "SSLF", 4) != 0)
        return std::nullopt;
    std::size_t body = 8;
    if (d.size() > body + 60 && std::memcmp (d.data() + body, "CcnK", 4) == 0 && std::memcmp (d.data() + body + 8, "FPCh", 4) == 0)
        body += 60;
    if (d.size() < body + 0x16)
        return std::nullopt;
    const std::size_t nameLen = (std::size_t) (d[body + 0x14] | d[body + 0x15] << 8);
    const std::size_t at = body + 0x16 + 2 * nameLen; // "DATA"
    if (at + 12 > d.size())
        return std::nullopt;
    if (floatsAt != nullptr)
        *floatsAt = at + 12;
    auto size = (long long) (std::int32_t) rd32 (d.data() + at + 4);
    if (size < 4)
        return std::nullopt;
    std::array<float, paramCount> P {};
    std::copy (std::begin (defaults), std::end (defaults), P.begin());
    for (int i = 0; i < paramCount; ++i)
    {
        size -= 4;
        if (size <= 3)
            continue;
        const auto off = at + 12 + 4 * (std::size_t) i;
        if (off + 4 > d.size())
            break;
        float v;
        const auto bits = rd32 (d.data() + off);
        std::memcpy (&v, &bits, 4);
        P[(std::size_t) i] = v == v ? std::min (std::max (v, 0.0f), 1.0f) : 0.0f;
    }
    return P;
}

std::vector<std::uint8_t> readFileBytes (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        auto local = path;
        std::replace (local.begin(), local.end(), '\\', '/');
        in.open (local, std::ios::binary);
    }
    return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------------------------------------------- tables

// §3.3: oscdata.dat = 1024 amplitudes + 1024 phases (float). Table 1023 − h holds harmonics 1…h + 1: harmonic h + 1 adds
// amp[h]·cos(j·(h + 1)·2π/2048 + phase[h]) to the running float sum.
using Tables = std::vector<float>;
std::shared_ptr<const Tables> zeroTables()
{
    static const auto z = std::make_shared<const Tables> ((std::size_t) tableCount * tableLen, 0.0f);
    return z;
}
std::shared_ptr<const Tables> buildTables (const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 8192)
        return zeroTables();
    auto T = std::make_shared<Tables> ((std::size_t) tableCount * tableLen);
    std::vector<float> acc (tableLen, 0.0f);
    for (int h = 0; h < tableCount; ++h)
    {
        float amp, ph;
        const auto a = rd32 (bytes.data() + 4 * h), p = rd32 (bytes.data() + 4096 + 4 * h);
        std::memcpy (&amp, &a, 4);
        std::memcpy (&ph, &p, 4);
        const auto w = (double) ((float) h + 1.0f) * 0.0030679615757712823;
        for (int j = 0; j < tableLen; ++j)
            acc[(std::size_t) j] = (float) (std::cos ((double) (float) j * w + (double) ph) * (double) amp + (double) acc[(std::size_t) j]);
        std::copy (acc.begin(), acc.end(), T->begin() + (std::ptrdiff_t) (tableCount - 1 - h) * tableLen);
    }
    return T;
}
std::shared_ptr<const Tables> tablesFor (const std::string& path)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Tables>> cache;
    if (path.empty())
        return zeroTables();
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (const auto it = cache.find (path); it != cache.end())
            return it->second;
    }
    auto T = buildTables (readFileBytes (path));
    const std::lock_guard<std::mutex> lock (mutex);
    return cache.emplace (path, T).first->second;
}

// ------------------------------------------------------------------------------------------------------- engine

// §3.6: the generator; per-sample/per-block draws use one rule, the voice reset another (128 steps per value)
struct Lcg
{
    std::uint32_t s = 0;
    std::uint32_t step() noexcept
    {
        const auto old = s;
        s = s * 0xbb38435u + 0x3619636bu;
        return old;
    }
    std::uint32_t resetValue() noexcept
    {
        for (int i = 0; i < 128; ++i)
            s = s * 0xB464B2F9u + 0x9D6F2492u;
        return s;
    }
};

// §4.4: per-sample envelope coefficient
float envRate (double p, bool attack) noexcept
{
    if (p >= 0.0010000000474974513)
    {
        const auto k = p * 0.95 + 0.05;
        const auto d = k * k * k * k * (attack ? 500.0 : 50.0);
        if (d >= 0.001)
            return (float) (0.0002 / d);
    }
    return 1.0f;
}

constexpr double lfoBeats[12] = { 0.25, 0.375, 0.5, 0.75, 1.0, 2.0, 4.0, 8.0, 16.0, 64.0, 128.0, 256.0 };

class Engine;

struct Voice
{
    Engine* e = nullptr;
    double rate = 44100.0;
    std::int32_t ph[8] {}, inc[8] {};
    float gain[8] {};
    int n = 0;
    const float* table = nullptr;
    int key = -1;
    float vel = 0.0f;
    std::uint32_t age = 0;
    float glide = 0.0f;
    double y[4] {};
    double A[4][5] {};
    float fe = 0.0f, feT = 0.0f, feR = 0.0f, ae = 0.0f, aeT = 0.0f, aeR = 0.0f;
    float dc = 0.0f, drift = 0.0f;
    double note = 0.0, freq = 0.0;
    float tL = 0.0f, tR = 0.0f, gL = -1.0f, gR = -1.0f;
    bool released = true;

    bool active() const noexcept { return key >= 0; }
    void reset (double r) noexcept;
    void start (int k, float v, bool legato) noexcept;
    void release() noexcept;
    bool control() noexcept;
    void oscSetup() noexcept;
    void filterSetup() noexcept;
    void render (float* outL, float* outR, int m) noexcept;
};

class Engine
{
public:
    Lcg lcg;
    std::shared_ptr<const Tables> tabs = zeroTables();
    double rate = 0.0, bpm = 120.0, songpos = 0.0;
    bool playing = false;
    float P[paramCount] {};
    float keyvel[128] {};
    int last = 0;
    std::uint32_t counter = 1;
    float keyL[128] {}, keyR[128] {}, keyPitch[128] {}, keyFcut[128] {}, keyFres[128] {};
    float bend = 0.0f, mw = 0.0f;
    bool sustainPedal = false;
    float lfo = 0.0f;
    double lfoPhase = 0.0;
    std::uint32_t silence = 0;
    int polyMax = voiceCount, polyLimit = 0;
    std::array<Voice, voiceCount> voices;

    // derived parameters (§1.2)
    bool poly = true, lead = false;
    float transpose = 0.0f, slide = 0.0f, mwAmt = 0.0f, oscmod = 0.0f, noise = 0.0f, fltFreq = 1.0f, peak = 0.0f, fltKbd = 0.0f,
          fltVel = 0.0f;
    int mwDest = 0, kind = 0, lfoShape = 0, lfoDest = 0;
    float feAtk = 1.0f, feDec = 1.0f, feSus = 1.0f, feRel = 1.0f, egfAmt = 0.0f, lfoAmt = 0.0f;
    float aeAtk = 1.0f, aeDec = 1.0f, aeSus = 1.0f, aeRel = 1.0f;
    double lfoBeat = 1.0, dlyTime = 0.25;
    float phaSpeed = 0.25f;
    bool dlyOn = false, choOn = false, phaOn = false, dlyReset = true, choReset = true, phaReset = true;

    // effect state (§6)
    std::int32_t holdCnt = 0;
    float holdL = 0.0f, holdR = 0.0f;
    std::vector<float> dlyBuf, choBuf;
    int dlyN = 0, choN = 0, dlyPos = 0, choPos = 0;
    float dlyLp = 0.0f, choP1 = 0.62f, choP2 = 0.1f;
    float phaS[14] {}, phaF = -1.0f, phaPhase = 0.0f;

    void init() noexcept
    {
        lcg = {};
        rate = 0.0;
        bpm = 120.0;
        songpos = 0.0;
        playing = false;
        std::fill (std::begin (keyvel), std::end (keyvel), 0.0f);
        last = 0;
        counter = 1;
        std::fill (std::begin (keyL), std::end (keyL), 1.0f);
        std::fill (std::begin (keyR), std::end (keyR), 1.0f);
        std::fill (std::begin (keyPitch), std::end (keyPitch), 0.0f);
        std::fill (std::begin (keyFcut), std::end (keyFcut), 0.0f);
        std::fill (std::begin (keyFres), std::end (keyFres), 0.0f);
        bend = mw = 0.0f;
        lfo = 0.0f;
        lfoPhase = 0.0;
        silence = 0;
        polyLimit = 0;
        dlyOn = choOn = phaOn = false;
        // the 99 voices are constructed (one reset each), then the defaults are applied (§3.6)
        for (auto& v : voices)
        {
            v = Voice {};
            v.e = this;
            v.reset (0.0);
        }
        for (int i = 0; i < paramCount; ++i)
            setParam (i, defaults[i]);
        holdCnt = 0;
        holdL = holdR = 0.0f;
        dlyReset = choReset = phaReset = true;
        dlyPos = choPos = 0;
        dlyLp = 0.0f;
        choP1 = 0.62f, choP2 = 0.1f;
        std::fill (std::begin (phaS), std::end (phaS), 0.0f);
        phaF = -1.0f, phaPhase = 0.0f;
    }

    void setParam (int i, float v) noexcept
    {
        if (i < 0 || i >= paramCount)
            return;
        v = std::min (std::max (v, 0.0f), 1.0f);
        P[i] = v;
        switch (i)
        {
            case 0:
            {
                const auto mode = (int) (v + v);
                if (mode == 0)
                    poly = true, lead = false;
                else if (mode == 1)
                    poly = false, lead = false;
                else if (mode == 2)
                    poly = false, lead = true;
                break;
            }
            case 1: transpose = (float) i32 ((v - 0.5f) * 24.0f); break;
            case 2:
            {
                const auto t = (1.0f - v) * (1.0f - v);
                slide = (float) ((double) (t * t) * 0.9 + 0.01);
                break;
            }
            case 3: mwDest = (int) v; break;
            case 4: mwAmt = v; break;
            case 5: kind = (int) (v * 9.0f); break;
            case 6: oscmod = v; break;
            case 7: noise = v * v; break;
            case 8: fltFreq = v; break;
            case 9: peak = v * 0.99f; break;
            case 10: fltKbd = v; break;
            case 11: fltVel = v; break;
            case 12: feAtk = envRate (v, true); break;
            case 13: feDec = envRate (v, false); break;
            case 14: feSus = v; break;
            case 15: feRel = envRate (v, false); break;
            case 16: egfAmt = (v + v) - 1.0f; break;
            case 17: lfoShape = (int) (v + v); break;
            case 18: lfoDest = (int) (v + v); break;
            case 19:
                if (const auto j = (int) (v * 11.0f); j >= 0 && j <= 11)
                    lfoBeat = lfoBeats[j];
                break;
            case 20: lfoAmt = (v + v) - 1.0f; break;
            case 21: aeAtk = envRate (v, true); break;
            case 22: aeDec = envRate (v, false); break;
            case 23: aeSus = v; break;
            case 24: aeRel = envRate ((double) v * 0.95 + 0.05, false) * 10.0f; break;
            case 28:
                if (dlyOn != (v != 0.0f))
                    dlyReset = true;
                dlyOn = v != 0.0f;
                break;
            case 29: dlyTime = ((double) i32 (v * 3.0f) + 1.0) * 0.25; break;
            case 33:
                if (choOn != (v != 0.0f))
                    choReset = true;
                choOn = v != 0.0f;
                break;
            case 37:
                if (phaOn != (v != 0.0f))
                    phaReset = true;
                phaOn = v != 0.0f;
                break;
            case 38:
                if (const auto j = (int) (v * 11.0f); j >= 0 && j <= 11)
                    phaSpeed = (float) lfoBeats[j];
                break;
            default: break;
        }
    }

    // §2.4: the sample-rate set; the buffers are (re)allocated off the audio thread (prepare)
    void setRate (double r)
    {
        if (r != 0.0 && r != rate)
        {
            rate = r;
            dlyN = (int) (rate * 1.5);
            choN = (int) (rate * 0.01);
            dlyBuf.assign ((std::size_t) std::max (dlyN, 1), 0.0f);
            choBuf.assign ((std::size_t) std::max (choN, 1), 0.0f);
        }
        std::fill (std::begin (keyvel), std::end (keyvel), 0.0f);
        lfoPhase = 0.0;
        for (auto& v : voices)
            v.reset (rate);
        holdCnt = 0;
        holdL = holdR = 0.0f;
        silence = 0xffffffffu;
        dlyReset = choReset = phaReset = true;
    }

    void setMaxPoly (int n) noexcept { polyLimit = n == 0 ? polyMax : n; }

    // §3.2: note on (vel > 0) / off (vel = 0), vel = MIDI velocity / 127
    void note (int key, float vel) noexcept
    {
        if (key < 0 || key > 127)
            return;
        vel = std::min (std::max (vel, 0.0f), 1.0f);
        keyvel[key] = vel;
        if (vel == 0.0f)
        {
            if (sustainPedal)
                return;
            for (auto& v : voices)
            {
                if (v.key != key)
                    continue;
                if (! poly && lead)
                {
                    int below = -1, above = -1;
                    float vb = 0.0f, va = 0.0f;
                    for (int k = 0; k < key; ++k)
                        if (keyvel[k] != 0.0f)
                            below = k, vb = keyvel[k];
                    for (int k = 127; k > key; --k)
                        if (keyvel[k] != 0.0f)
                            above = k, va = keyvel[k];
                    if (vb != 0.0f)
                    {
                        v.key = below, v.vel = vb, last = below;
                        continue;
                    }
                    if (va != 0.0f)
                    {
                        v.key = above, v.vel = va, last = above;
                        continue;
                    }
                }
                v.release();
            }
            return;
        }
        Voice* v = nullptr;
        if (poly)
        {
            const auto n = std::min (polyMax, polyLimit);
            for (int i = 0; i < n; ++i)
                if (! voices[(std::size_t) i].active())
                    v = &voices[(std::size_t) i];
            if (v == nullptr)
            {
                std::uint32_t best = 0xffffffffu;
                for (int i = 0; i < n; ++i)
                {
                    const auto a = voices[(std::size_t) i].age;
                    if (a <= best)
                        v = &voices[(std::size_t) i];
                    if (a < best)
                        best = a;
                }
            }
            if (v == nullptr)
                return;
        }
        else
            v = &voices[0];
        counter = counter + 1;
        v->age = counter;
        v->glide = (float) last;
        if (lead)
        {
            bool alone = true;
            for (int k = 0; k < 128; ++k)
                alone = alone && (keyvel[k] == 0.0f || k == key);
            if (alone)
                v->glide = (float) key;
        }
        v->start (key, vel, lead);
        last = key;
    }

    void lfoStep (int m, double pos) noexcept // §5
    {
        float w = lfo;
        if (lfoShape == 0)
            w = (float) std::sin (lfoPhase * 6.283185307179586);
        else if (lfoShape == 1)
            w = (float) (((1.0 - lfoPhase) + (1.0 - lfoPhase)) - 1.0);
        else if (lfoShape == 2)
        {
            const auto b = 0.5 > lfoPhase ? 1.0f : 0.0f;
            w = (b + b) - 1.0f;
        }
        const auto a = lfoAmt;
        auto a2 = a;
        if (mwDest == 1)
            a2 = a + mwAmt * mw;
        if (a > 1.0f)
            lfo = w;
        else if (a < -1.0f)
            lfo = -w;
        else
        {
            w = (a2 * a2) * w;
            lfo = a >= 0.0f ? w : -w;
        }
        double x;
        if (playing)
            x = pos / lfoBeat;
        else
            x = (((0.016666666666666666 / lfoBeat) * bpm) / rate) * (double) m + lfoPhase;
        lfoPhase = x - (double) i32 (x);
    }

    // one engine call of n ≤ 512 samples, summed into L/R (§2.3)
    void process (float* L, float* R, int n) noexcept
    {
        auto pos = songpos;
        const auto limit = (std::uint32_t) i32 (rate * 0.5);
        bool any = false;
        while (n > 0)
        {
            const auto m = std::min (n, blockLen);
            n -= m;
            lfoStep (m, pos);
            pos = pos + (((double) m / rate) * bpm) * 0.016666666666666666;
            for (auto& v : voices)
                if (v.active())
                {
                    v.render (L, R, m);
                    any = true;
                }
            std::uint32_t cnt;
            if (any)
                silence = 0, cnt = 0;
            else
            {
                if (silence != 0xffffffffu)
                    silence += (std::uint32_t) m;
                cnt = silence;
            }
            if (cnt < limit)
            {
                fxDist (L, R, m);
                fxDelay (L, R, m);
                fxPhaser (L, R, m, pos);
                fxChorus (L, R, m);
                any = true;
            }
            L += m;
            R += m;
        }
    }

    bool anyActive() const noexcept
    {
        return std::any_of (voices.begin(), voices.end(), [] (const Voice& v) { return v.active(); });
    }

private:
    void fxDist (float* L, float* R, int m) noexcept // §6.1
    {
        if (P[25] == 0.0f)
            return;
        const auto D = (P[26] * P[26]) * 20.0f;
        auto t = 1.0f - D * 0.0625f;
        t = t * t, t = t * t, t = t * t;
        const auto g = (float) ((double) t * 0.7 + 0.3);
        auto h = P[27] * P[27];
        h = std::min (std::max (h, 0.0f), 1.0f);
        const auto H = i32 (h * 64.0f);
        auto shape = [&] (float x) {
            float y;
            if (x >= 0.0f)
                y = 1.0f - powf32 (10.0, (double) -(x * D)) * (1.0f - x);
            else
                y = powf32 (10.0, (double) (x * D)) * (x + 1.0f) - 1.0f;
            return g * y;
        };
        for (int i = 0; i < m; ++i)
        {
            auto yl = shape (L[i]), yr = shape (R[i]);
            if (H != 0)
            {
                auto c = holdCnt;
                if ((float) c > 0.0f)
                    yl = holdL, yr = holdR;
                else
                {
                    holdL = yl, holdR = yr;
                    c = add32 (c, H);
                }
                holdCnt = i32 ((float) c - 1.0f);
            }
            L[i] = yl;
            R[i] = yr;
        }
    }

    void fxDelay (float* L, float* R, int m) noexcept // §6.2
    {
        if (! dlyOn || dlyBuf.empty() || dlyN <= 0)
            return;
        if (dlyReset)
        {
            dlyReset = false;
            std::fill (dlyBuf.begin(), dlyBuf.end(), 0.0f);
            dlyPos = 0;
            dlyLp = 0.0f;
        }
        const auto c = std::min (std::max (P[31], 0.0f), 1.0f) * 0.99f + 0.01f;
        const auto fb = P[30], mix = P[32] * P[32];
        auto q = (dlyTime * 40.0) / bpm;
        while (q > 1.0)
            q = q * 0.5;
        const auto ln = std::min (std::max (i32 ((double) (dlyN - 1) * q), 1), dlyN);
        for (int i = 0; i < m; ++i)
        {
            if (dlyPos >= ln)
                dlyPos = 0;
            auto& b = dlyBuf[(std::size_t) dlyPos];
            const auto s = (float) ((((double) (fb * b) + 1e-30) - (double) dlyLp) * (double) c + (double) dlyLp);
            dlyLp = s;
            b = (R[i] + L[i]) + s;
            const auto wet = s * mix;
            if (std::fabs ((double) wet) > 1e-05)
                silence = 0;
            L[i] = wet + L[i];
            R[i] = wet + R[i];
            ++dlyPos;
        }
    }

    void fxPhaser (float* L, float* R, int m, double pos) noexcept // §6.3
    {
        if (! phaOn)
            return;
        if (phaReset)
        {
            phaReset = false;
            std::fill (std::begin (phaS), std::end (phaS), 0.0f);
            phaF = -1.0f;
            phaPhase = 0.0f;
        }
        const auto hi = (float) ((0.5 / rate) * 40000.0), lo = (float) ((0.5 / rate) * 100.0);
        const auto w = powf32 ((double) P[39], 3.0) * 0.5f;
        const auto fb = (float) ((double) powf32 ((double) P[40], (double) 0.33333334f) * 0.98);
        const auto T = std::max (P[42] * P[42], 1e-5f);
        const auto mo = std::max (P[41], 1e-5f);
        if (phaF < 0.0f)
            phaF = T;
        double x;
        if (playing)
            x = pos / (double) phaSpeed;
        else
            x = (bpm / ((double) (phaSpeed * 60.0f) * rate)) * (double) m + (double) phaPhase;
        const auto xf = (float) x;
        phaPhase = xf - (float) i32 (xf);
        const auto s = std::cos ((double) phaPhase * 6.283185307179586);
        const auto cA = (float) ((s + 1.0) * (double) w), cB = (float) ((1.0 - s) * (double) w);
        const auto span = hi - lo;
        for (int i = 0; i < m; ++i)
        {
            const auto F = phaF;
            const auto xl = (float) ((double) L[i] + 1e-30), xr = (float) ((double) R[i] + 1e-30);
            const auto gA = std::max (std::min ((F + cA) * span + lo, 0.499f), 1e-5f);
            const auto gB = std::max (std::min ((F + cB) * span + lo, 0.499f), 1e-5f);
            phaF = (float) ((double) (T - F) * 0.001 + (double) F);
            const auto aA = (1.0f - gA) / (gA + 1.0f), aB = (1.0f - gB) / (gB + 1.0f);
            auto chain = [&] (float a, int base, int fbi, float in, float& out) {
                auto prev = in + fb * phaS[fbi];
                for (int k = 0; k < 6; ++k)
                {
                    const auto v = phaS[base + k] - a * prev;
                    phaS[base + k] = v * a + prev;
                    prev = v;
                }
                phaS[fbi] = prev;
                out = prev * mo + out;
            };
            chain (aA, 0, 6, xl, L[i]);
            chain (aB, 7, 13, xr, R[i]);
        }
    }

    void fxChorus (float* L, float* R, int m) noexcept // §6.4
    {
        if (! choOn || choBuf.empty() || choN <= 0)
            return;
        const auto N = choN;
        if (choReset)
        {
            choReset = false;
            std::fill (choBuf.begin(), choBuf.end(), 0.0f);
            choPos = 0;
            choP1 = 0.62f, choP2 = 0.1f;
        }
        const auto D = (P[35] * 0.9f) * 0.4f + 0.04f;
        const auto M = P[36] * P[36];
        const auto r = (P[34] * P[34]) * 4.0f + 0.2f;
        const auto inv = 1.0 / rate;
        const auto i1 = (float) ((double) r * inv), i2 = (float) ((double) (r + 0.05f) * inv);
        const auto ND = (float) N * D;
        auto tap = [&] (int p, float posf) {
            const auto ip = i32 (posf);
            const auto fr = posf - (float) ip;
            auto a = p + ip;
            if (a >= N)
                a -= N;
            auto b = a + 1;
            if (b >= N)
                b -= N;
            return (1.0f - fr) * choBuf[(std::size_t) a] + fr * choBuf[(std::size_t) b];
        };
        for (int i = 0; i < m; ++i)
        {
            if (choPos >= N)
                choPos = 0;
            const auto p = choPos;
            choBuf[(std::size_t) p] = L[i] + R[i];
            const auto v = (float) (std::sin ((double) choP1 * 6.283185307179586) * (double) D + 0.5);
            const auto tapL = tap (p, ND * (v * v));
            const auto tapR = tap (p, (float) (std::sin ((double) choP2 * 6.283185307179586) * (double) D + 0.5));
            const auto p2 = choP2 + i2, p1 = choP1 + i1;
            choP1 = p1 - (float) i32 (p1);
            choP2 = p2 - (float) i32 (p2);
            L[i] = tapL * M + L[i];
            R[i] = tapR * M + R[i];
            ++choPos;
        }
    }
};

void Voice::reset (double r) noexcept
{
    if (r != 0.0)
        rate = (double) (float) r;
    for (auto& p : ph)
        p = (std::int32_t) e->lcg.resetValue();
    key = -1;
    vel = 0.0f;
    y[0] = y[1] = y[2] = y[3] = 0.0;
    fe = 0.0f;
    ae = 0.0f;
    released = true;
    gL = gR = -1.0f;
}

void Voice::start (int k, float v, bool legato) noexcept
{
    vel = v;
    key = k;
    if (legato && ! released)
        return;
    fe = 0.0f, feT = 4.0f, feR = e->feAtk;
    aeT = 4.0f, aeR = e->aeAtk;
    released = false;
}

void Voice::release() noexcept
{
    released = true;
    feT = 0.0f, feR = e->feRel;
    aeT = 0.0f, aeR = e->aeRel;
}

// §4.1: once per 32-sample block; false when the voice ended (reset) instead
bool Voice::control() noexcept
{
    auto v = vel;
    if (ae < 1e-5f && aeT == 0.0f)
        v = 0.0f;
    tL = v * e->keyL[key];
    tR = v * e->keyR[key];
    if (! (gL >= 0.0f))
        gL = tL;
    if (! (gR >= 0.0f))
        gR = tR;
    if (released && v == 0.0f && gL < 1e-5f && gR < 1e-5f)
    {
        reset (0.0);
        return false;
    }
    const auto r = e->lcg.step();
    const auto rnd = (float) ((double) r * 4.656612874161595e-10 - 1.0);
    drift = (rnd - drift) * 0.01f + drift;
    const auto ds = drift * 0.08f;
    glide = ((float) key - glide) * e->slide + glide;
    auto nt = (double) ((glide + e->transpose) + ds);
    nt = (double) e->bend + nt;
    if (e->lfoDest == 1)
        nt = (double) (e->lfo * 12.0f) + nt;
    nt = (double) e->keyPitch[key] + nt;
    note = nt;
    freq = std::pow (2.0, (nt - 57.0) * 0.08333333333333333) * 220.0;
    oscSetup();
    filterSetup();
    return true;
}

// §3.4: oscillator set-up per kind (all oscillators read the table chosen for the first one)
void Voice::oscSetup() noexcept
{
    const auto f = freq;
    auto m = e->oscmod;
    if (e->lfoDest == 2)
        m = m + e->lfo * 12.0f;
    const auto expo = (float) i32 (m * 24.0f - 12.0f) * 0.0833333358168602f + 0.006250000558793545f;
    const auto r = (double) powf32 (2.0, (double) expo);
    const auto am = std::fabs (m);
    float g[8] = { 1.0f, -1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f };
    double fr[8] = { f, f, f, f, f, f, f, f };
    auto offset = [] (float x) { return i32via64 ((double) x * 4294967295.0); };
    switch (e->kind)
    {
        case 0:
        {
            n = 3;
            const auto off = offset (1.0f - am);
            ph[2] = ph[0];
            ph[1] = add32 (ph[0], off);
            if (off == 0)
                n = 1;
            break;
        }
        case 1:
            n = 2, fr[1] = f * r, g[1] = 1.0f;
            break;
        case 2:
            n = 2;
            ph[1] = add32 (offset (1.0f - am), ph[0]);
            break;
        case 3:
            n = 3, fr[2] = f * r;
            ph[1] = add32 (ph[0], 0x7fffffff);
            break;
        case 4:
            n = 4, fr[2] = fr[3] = f * r, g[3] = -1.0f;
            ph[1] = add32 (ph[0], -0x70000001);
            ph[3] = add32 (ph[2], -0x70000001);
            break;
        case 5:
            n = 8;
            for (int k = 0; k < 8; ++k)
                g[k] = 0.5f, fr[k] = (((double) k * 0.0125 - 0.05) * (double) m + 1.0) * f;
            break;
        case 6:
        {
            n = 6;
            for (auto& x : g)
                x = 0.5f;
            const auto d = offset (std::fabs (m * 0.5f));
            const auto p0 = ph[0];
            std::int32_t acc = 0;
            for (int k = 0; k < 6; ++k)
                ph[k] = add32 (p0, acc), acc = add32 (acc, d);
            break;
        }
        case 7:
            n = 2, g[1] = 1.0f;
            ph[1] = add32 (offset (1.0f - am), ph[0]);
            break;
        case 8:
        case 9:
        {
            n = 3, g[1] = 1.0f, g[2] = 1.0f;
            const auto q = (double) m * 0.01;
            fr[0] = (1.0 - q) * f * (e->kind == 8 ? 0.5 : 1.5);
            fr[2] = (f + f) * (q + 1.0);
            break;
        }
        default: n = 0; break;
    }
    const auto ratef = (float) rate;
    for (int k = 0; k < n; ++k)
    {
        gain[k] = g[k];
        inc[k] = i32via64 ((fr[k] / rate) * 4294967295.0);
        if (k == 0)
        {
            const auto frf = (float) fr[k];
            int lvl = 0;
            if (ratef * 0.00048828125f < frf)
                lvl = std::min (std::max (0x3ff - i32 ((0.5f / frf) * ratef), 0), 0x3ff);
            table = e->tabs->data() + (std::size_t) lvl * tableLen;
        }
    }
}

// §4.2: cutoff/resonance and the per-sample transition matrix (4096 Euler sub-steps by 12 squarings)
void Voice::filterSetup() noexcept
{
    auto a = (((double) vel - 0.5) * (double) e->fltVel) * 14.0;
    const auto b = ((note - 48.0) * (double) e->fltKbd) * 0.08333333333333333;
    const auto c = (double) e->fltFreq * 10.3;
    a = a + b;
    a = a + 4.0;
    a = a + ((double) drift * 0.10300000000000001 + c);
    if (e->lfoDest == 0)
        a = a + (double) e->lfo * 4.0;
    if (e->mwDest == 0)
        a = a + (double) (e->mwAmt * e->mw) * 4.0;
    const auto fc = std::pow (2.0, (double) e->keyFcut[key] + a);
    const auto g0 = 6.25 / (4096.0 * rate);
    const auto ex = std::pow (2.0, (double) e->egfAmt * 10.0);
    const auto fc2 = std::min ((ex * fc - fc) * (double) fe + fc, 20000.0);
    const auto g = std::min (std::max (g0 * fc2, 0.0), 1.0);
    const auto k = std::min (std::max ((double) e->keyFres[key] + (double) e->peak, 0.0), 1.0) * -4.050000190734863;
    const auto om = 1.0 - g;
    const auto g2 = g * g, ag = om * g, kg = k * g;
    const auto ag2 = ag * g, g3 = g2 * g, kg2 = kg * g, kg3 = kg2 * g;
    const auto ag3 = ag2 * g, g4 = g3 * g, kg4a = kg3 * g + om;
    double M[4][5] = { { om, 0.0, 0.0, kg, g }, { ag, om, 0.0, kg2, g2 }, { ag2, ag, om, kg3, g3 }, { ag3, ag2, ag, kg4a, g4 } };
    for (int it = 0; it < 12; ++it)
    {
        const auto A00 = M[0][0], A01 = M[0][1], A02 = M[0][2], A03 = M[0][3], A04 = M[0][4];
        const auto A10 = M[1][0], A11 = M[1][1], A12 = M[1][2], A13 = M[1][3], A14 = M[1][4];
        const auto A20 = M[2][0], A21 = M[2][1], A22 = M[2][2], A23 = M[2][3], A24 = M[2][4];
        const auto A30 = M[3][0], A31 = M[3][1], A32 = M[3][2], A33 = M[3][3], A34 = M[3][4];
        M[0][0] = ((A10 * A01) + (A00 * A00)) + ((A20 * A02) + (A30 * A03));
        M[0][1] = (((A11 + A00) * A01) + (A21 * A02)) + (A31 * A03);
        M[0][2] = (((A00 + A22) * A02) + (A12 * A01)) + (A32 * A03);
        M[0][3] = (((A33 + A00) * A03) + (A13 * A01)) + (A23 * A02);
        M[0][4] = (((A34 * A03) + A04) + (A24 * A02)) + ((A14 * A01) + (A04 * A00));
        M[1][0] = ((A00 * A10) + (A10 * A11)) + ((A20 * A12) + (A30 * A13));
        M[1][1] = ((A01 * A10) + (A11 * A11)) + ((A21 * A12) + (A13 * A31));
        M[1][2] = ((A11 * A12) + (A10 * A02)) + ((A12 * A22) + (A13 * A32));
        M[1][3] = ((A33 * A13) + (A23 * A12)) + ((A13 * A11) + (A03 * A10));
        M[1][4] = (((A34 * A13) + A14) + (A24 * A12)) + ((A04 * A10) + (A14 * A11));
        M[2][0] = ((A30 * A23) + (A20 * A22)) + ((A00 * A20) + (A10 * A21));
        M[2][1] = ((A01 * A20) + (A11 * A21)) + ((A23 * A31) + (A21 * A22));
        M[2][2] = ((A22 * A22) + (A23 * A32)) + ((A21 * A12) + (A20 * A02));
        M[2][3] = ((A03 * A20) + (A13 * A21)) + ((A33 * A23) + (A23 * A22));
        M[2][4] = (((A34 * A23) + A24) + (A24 * A22)) + ((A04 * A20) + (A14 * A21));
        M[3][0] = ((A33 * A30) + (A32 * A20)) + ((A00 * A30) + (A10 * A31));
        M[3][1] = ((A33 * A31) + (A32 * A21)) + ((A30 * A01) + (A11 * A31));
        M[3][2] = ((A31 * A12) + (A30 * A02)) + ((A32 * A22) + (A33 * A32));
        M[3][3] = ((A33 * A33) + (A23 * A32)) + ((A03 * A30) + (A13 * A31));
        M[3][4] = (((A34 * A33) + A34) + (A24 * A32)) + ((A04 * A30) + (A14 * A31));
    }
    std::memcpy (A, M, sizeof (A));
}

// §4.3: the per-sample loop
void Voice::render (float* outL, float* outR, int m) noexcept
{
    if (! control())
        return;
    auto& lcg = e->lcg;
    const auto noise = e->noise;
    const auto A00 = A[0][0], A01 = A[0][1], A02 = A[0][2], A03 = A[0][3], A04 = A[0][4];
    const auto A10 = A[1][0], A11 = A[1][1], A12 = A[1][2], A13 = A[1][3], A14 = A[1][4];
    const auto A20 = A[2][0], A21 = A[2][1], A22 = A[2][2], A23 = A[2][3], A24 = A[2][4];
    const auto A30 = A[3][0], A31 = A[3][1], A32 = A[3][2], A33 = A[3][3], A34 = A[3][4];
    auto y1 = y[0], y2 = y[1], y3 = y[2], y4 = y[3];
    const auto* T = table;
    for (int i = 0; i < m; ++i)
    {
        const auto r = lcg.s;
        lcg.s = r * 0xbb38435u + 0x3619636bu;
        const auto d = lcg.s & 0x3fffffu;
        double s = 0.0;
        for (int k = 0; k < n; ++k)
        {
            const auto p = (std::uint32_t) ph[k];
            s += (double) (T[(p + d) >> 21] * gain[k]);
            ph[k] = add32 (ph[k], inc[k]);
        }
        const auto nz = (float) (((double) r * 2.3283064370807974e-10) * 2.0 - 1.0) * noise;
        const auto x = (((double) nz + s) * 0.20000000298023224 + 0.06999999985098837) * (double) ae;
        const auto n4 = (((x * A34) + (y4 * A33)) + (y3 * A32)) + ((y1 * A30) + (y2 * A31));
        const auto n1 = (((y4 * A03) + (x * A04)) + (y3 * A02)) + ((y1 * A00) + (y2 * A01));
        const auto n2 = (((y4 * A13) + (x * A14)) + (y3 * A12)) + ((y1 * A10) + (y2 * A11));
        const auto n3 = (((y4 * A23) + (x * A24)) + (y3 * A22)) + ((y1 * A20) + (y2 * A21));
        y4 = n4, y2 = n2, y3 = n3;
        y1 = n1 / (n1 * n1 + 1.0);
        auto out = n4 * 4.999999925494195;
        dc = (float) ((out - (double) dc) * 0.004999999888241291 + (double) dc);
        out = out + (double) -dc;
        fe = (feT - fe) * feR + fe;
        ae = (aeT - ae) * aeR + ae;
        gL = (tL - gL) * 0.005f + gL;
        gR = (tR - gR) * 0.005f + gR;
        if (fe > 1.0f)
            fe = 1.0f, feT = e->feSus, feR = e->feDec;
        if (ae > 1.0f)
            ae = 1.0f, aeT = e->aeSus, aeR = e->aeDec;
        outL[i] = (float) ((double) gL * out + (double) outL[i]);
        outR[i] = (float) ((double) gR * out + (double) outR[i]);
    }
    y[0] = y1, y[1] = y2, y[2] = y3, y[3] = y4;
}

// ------------------------------------------------------------------------------------------------------- wrapper

void lawA (double pan, double vol, float& l, float& r) noexcept
{
    const auto t = (1.0 + std::clamp (pan, -1.0, 1.0)) * 0.78539816339744831;
    l = (float) (vol * std::cos (t));
    r = (float) (vol * std::sin (t));
}

int velFromInitVol (double iv) noexcept // §2.1
{
    const auto x = (float) iv * 10.0f + 1.0f;
    const auto v = ((float) std::log ((double) x) * 0.3284587264060974f) * 127.0f;
    return std::clamp (i32 ((double) v), 0, 127);
}

// The FL wrapper (CFLWrapper): held notes with their FinalLevels, per-note tables, Gen_Render chunking (§2)
class Plugin
{
public:
    struct Held
    {
        bool on = false;
        double fvol = 1.0, fpan = 0.0, pitchCents = 0.0; // Final.Vol, Final.Pan, Final.Pitch − 100·(key − 60)
        bool released = false;
        HostSlide slide; // a slide note moving Final.Pitch (issue #14)
    };
    bool sliding = false;
    Engine eng;
    Held held[128];
    bool panLawA = true;
    bool playing = false, changed = false;

    // a freshly created plugin (its own 44.1 kHz set-up), then the host's sample rate when it differs (§2.4)
    void init (double fs, const float* params)
    {
        eng.init();
        if (params != nullptr)
            for (int i = 0; i < paramCount; ++i)
                eng.setParam (i, params[i]);
        eng.setRate (44100.0);
        if (fs != 44100.0)
            eng.setRate (fs);
        eng.setMaxPoly (0);
        for (auto& h : held)
            h = Held {};
        playing = changed = false;
    }

    void noteOn (int key, double initVol, double fvol, double fpan, double pitchCents) noexcept
    {
        key = std::clamp (key, 0, 127);
        if (held[key].on)
            eng.note (key, 0.0f);
        held[key] = { true, fvol, fpan, pitchCents, false, {} };
        eng.note (key, (float) velFromInitVol (initVol) * 0.007874016f);
    }
    void noteOff (int key) noexcept
    {
        if (key >= 0 && key < 128)
        {
            held[key].released = true;
            eng.note (key, 0.0f);
        }
    }
    // a slide note: every held, unreleased key glides to `toCents` (Final.Pitch, cents from C5) over `samples`
    void slideTo (double toCents, float samples) noexcept
    {
        for (int i = 0; i < 128; ++i)
            if (held[i].on && ! held[i].released)
            {
                held[i].slide.start ((float) (toCents - (100.0 * i - 6000.0 + held[i].pitchCents)), samples);
                sliding = true;
            }
    }
    void setLevels (int key, double fvol, double fpan) noexcept
    {
        if (key >= 0 && key < 128 && held[key].on)
            held[key].fvol = fvol, held[key].fpan = fpan;
    }
    void setPlaying (bool on) noexcept
    {
        if (on != playing)
            playing = on, changed = true;
    }

    void tables() noexcept
    {
        float cl = 0.0f, cr = 0.0f, cp = 0.0f;
        bool first = true;
        for (int i = 0; i < 128; ++i)
        {
            if (held[i].on)
            {
                const auto& h = held[i];
                if (panLawA)
                    lawA (h.fpan, h.fvol, cl, cr);
                else
                    cl = cr = (float) h.fvol;
                const auto fp = (float) (float) ((float) (100.0 * i - 6000.0)
                                                 + (h.slide.cents != 0.0f ? (float) (h.pitchCents + (double) h.slide.cents) : (float) h.pitchCents));
                cp = (float) ((double) fp * 0.01 - (double) i + 60.0);
                if (first)
                {
                    for (int j = 0; j < i; ++j)
                        eng.keyL[j] = cl, eng.keyR[j] = cr, eng.keyPitch[j] = cp, eng.keyFcut[j] = 0.0f, eng.keyFres[j] = 0.0f;
                    first = false;
                }
            }
            eng.keyL[i] = cl, eng.keyR[i] = cr, eng.keyPitch[i] = cp, eng.keyFcut[i] = 0.0f, eng.keyFres[i] = 0.0f;
        }
    }

    bool idle() const noexcept
    {
        if (eng.anyActive())
            return false;
        return std::none_of (std::begin (held), std::end (held), [] (const Held& h) { return h.on; });
    }

    // one Gen_Render of n samples summed into outL/outR; songBeats = the host's song position (used while playing)
    void render (float* outL, float* outR, int n, double songBeats) noexcept
    {
        tables();
        auto sp = playing ? songBeats : 0.0;
        for (int c0 = 0; c0 < n; c0 += maxChunk)
        {
            const auto cn = std::min (maxChunk, n - c0);
            eng.songpos = sp;
            if (changed)
                eng.playing = playing, changed = false;
            eng.process (outL + c0, outR + c0, cn);
            if (playing)
                sp = sp + ((eng.bpm * 0.016666666666666666) * (double) cn) * (1.0 / eng.rate);
            if (sliding) // (issue #14) the slide moves the pitch table chunk by chunk
            {
                sliding = false;
                for (auto& h : held)
                    if (h.slide.moving())
                    {
                        h.slide.advance ((float) cn);
                        sliding = sliding || h.slide.moving();
                    }
                tables();
            }
        }
        // host voices whose key is released and has no engine voice any more are killed
        for (int key = 0; key < 128; ++key)
            if (held[key].on && eng.keyvel[key] == 0.0f
                && std::none_of (eng.voices.begin(), eng.voices.end(), [key] (const Voice& v) { return v.active() && v.key == key; }))
                held[key].on = false;
    }
};

// ------------------------------------------------------------------------------------------------------- LYNX front-end
//
// Levels (§2.1, REA_rodada28 §4.1, as GMS in REA_rodada39 §3.1): Final.Vol = channel volume, Final.Pan = note pan +
// channel pan, Final.Pitch = 100·(key − 60) + channel pitch + note cents; the wrapper turns them into the per-note
// tables with the host's law A. LYNX's channel gains gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…) give V and p back, so
// the channel volume/pan are not applied twice. Velocity: Init.Vol = (11^(1.28·vel) − 1)/10 from LYNX's velocity, then
// the wrapper's MIDI velocity. The tempo is the song tempo; the max-polyphony event is taken as value 0 (99 voices).
// While the transport plays, the song position locks the LFO and the phaser LFO (§5, §6.3).

struct Setup
{
    std::array<float, paramCount> P {};
    std::shared_ptr<const Tables> tabs;
};

class MiniSynthInstrument final : public Instrument
{
public:
    MiniSynthInstrument()
    {
        setup = new Setup();
        std::copy (std::begin (defaults), std::end (defaults), setup->P.begin());
        setup->tabs = zeroTables();
        applySetup (44100.0);
    }
    ~MiniSynthInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete setup;
    }

    void prepare (double sr) noexcept override
    {
        take();
        idleKnown = false;
        rate = sr;
        applySetup (sr);
        lastTicks = -1.0;
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        tempo.store (bpm > 0.0 ? bpm : 120.0);
        if (channel.pluginState == state && channel.samplePath == oscdata)
            return;
        state = channel.pluginState;
        oscdata = channel.samplePath;
        auto s = std::make_unique<Setup>();
        if (auto p = parseState (state))
            s->P = *p;
        else
            std::copy (std::begin (defaults), std::end (defaults), s->P.begin());
        s->tabs = tablesFor (oscdata);
        delete incoming.exchange (s.release());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        idleKnown = false;
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto k = std::clamp (key, 0, 127);
        notePan[k] = nextPan;
        plugin.noteOn (k, initVol, channelVol, std::clamp ((double) nextPan + channelPan, -1.0, 1.0),
                       (double) pitchCents.load() + (double) nextCents);
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        idleKnown = false;
        plugin.noteOff (key);
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        plugin.slideTo (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents, samples);
    }

    void allNotesOff() noexcept override
    {
        idleKnown = false;
        for (int k = 0; k < 128; ++k)
            if (plugin.held[k].on)
                plugin.noteOff (k);
    }

    bool idle() const noexcept override
    {
        // the cheap test first; the plugin's scan of its 99 voices and 128 keys can only change through the calls
        // that touch the plugin (they clear idleKnown), so its answer is kept until then
        if (quiet < (int) rate)
            return false;
        if (! idleKnown)
            idleCached = plugin.idle(), idleKnown = true;
        return idleCached;
    }

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
        idleKnown = false;
        plugin.eng.bpm = tempo.load();
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
        plugin.setPlaying (playing);
        // render into the scratch buffers (the engine sums) and keep track of how long the output has been silent
        for (int c0 = 0; c0 < n; c0 += maxChunk)
        {
            const auto cn = std::min (maxChunk, n - c0);
            std::fill (bufL, bufL + cn, 0.0f);
            std::fill (bufR, bufR + cn, 0.0f);
            plugin.render (bufL, bufR, cn, songBeats + (double) c0 * plugin.eng.bpm / (60.0 * rate));
            bool loud = false;
            for (int i = 0; i < cn; ++i)
            {
                left[c0 + i] += bufL[i];
                right[c0 + i] += bufR[i];
                loud = loud || std::fabs (bufL[i]) > 1e-7f || std::fabs (bufR[i]) > 1e-7f;
            }
            quiet = loud ? 0 : std::min (quiet + cn, 1 << 30);
        }
    }

private:
    void applySetup (double sr) noexcept
    {
        plugin.eng.tabs = setup->tabs;
        plugin.init (sr, setup->P.data());
        plugin.eng.bpm = tempo.load();
        quiet = 0;
    }
    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            retired.store (setup);
            setup = s;
            // a new patch: the plugin restores the state (its parameters) without resetting its voices
            plugin.eng.tabs = setup->tabs;
            for (int i = 0; i < paramCount; ++i)
                plugin.eng.setParam (i, setup->P[(std::size_t) i]);
        }
    }

    Plugin plugin;
    Setup* setup = nullptr;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::atomic<float> pitchCents { 0.0f };
    std::atomic<double> tempo { 120.0 };
    std::vector<std::uint8_t> state;
    std::string oscdata;
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0, lastTicks = -1.0, songBeats = 0.0;
    bool playing = false;
    int quiet = 0;
    mutable bool idleKnown = false, idleCached = false;
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
    float bufL[maxChunk] {}, bufR[maxChunk] {};
};
} // namespace

bool miniSynthStateUsable (const std::vector<std::uint8_t>& state) { return parseState (state).has_value(); }

std::vector<float> miniSynthParams (const std::vector<std::uint8_t>& state)
{
    const auto p = parseState (state);
    if (! p)
        return {};
    return std::vector<float> (p->begin(), p->end());
}

int miniSynthParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    std::size_t at = 0;
    if (index < 0 || index >= paramCount || ! parseState (state, &at) || at + 4 * (std::size_t) index + 4 > state.size())
        return -1;
    return (int) (at + 4 * (std::size_t) index);
}

std::unique_ptr<Instrument> makeMiniSynthInstrument() { return std::make_unique<MiniSynthInstrument>(); }

namespace minisynth
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, const std::string& oscdataPath, int block, double bpm, int maxPoly, double songPos,
                                    bool panLawA)
{
    const auto p = parseState (state);
    if (! p || length <= 0)
        return {};
    auto plugin = std::make_unique<Plugin>();
    plugin->eng.tabs = tablesFor (oscdataPath);
    plugin->init (sampleRate, p->data());
    plugin->eng.setMaxPoly (maxPoly);
    plugin->eng.bpm = (double) (float) bpm;
    plugin->panLawA = panLawA;
    if (songPos >= 0.0)
        plugin->setPlaying (true);
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) std::max (block, 1)), r (l.size());
    block = std::max (1, block);
    int n0 = 0;
    while (n0 < length)
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
        plugin->render (l.data(), r.data(), ln, songPos >= 0.0 ? songPos + (double) n0 * bpm / (60.0 * sampleRate) : 0.0);
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace minisynth
} // namespace lynx
