// FL Studio's SimSynth: the engine of SimSynth_Eng_x64.dll and the state handling of its front-end SimSynth_x64.dll,
// rebuilt from docs/analise/REA_rodada64.md (clean-room: data layouts, formulas and behaviour in our own words; no
// Image-Line code or data). Section numbers (§) refer to that document.

#include "core/SimSynth.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "core/FlVoiceProcessor.h" // panLaw (FL's ComputeLRVol law)

namespace lynx
{
namespace
{
constexpr int numP = 45, numS = 19, numVoices = 12, stateBytes = 564, tabLen = 4096;
constexpr std::uint16_t filterOffMark = 0x2253;

// host parameter i → { kind (1 knob, 0 switch, 2 wave selector), engine index, knob scale } (§1.2)
struct HostParam { int kind, index, scale; };
constexpr HostParam hostParams[58] = {
    { 1, 3, 65535 }, { 1, 4, 65535 }, { 1, 5, 65535 }, { 1, 10, 65535 }, { 1, 11, 65535 }, { 1, 12, 65535 }, { 1, 17, 65535 },
    { 1, 18, 65535 }, { 1, 19, 65535 }, { 1, 21, 65535 }, { 1, 22, 65535 }, { 1, 26, 65535 }, { 1, 27, 65535 }, { 1, 28, 65535 },
    { 1, 29, 65535 }, { 1, 32, 65535 }, { 1, 33, 65535 }, { 1, 35, 65535 }, { 1, 36, 65535 }, { 1, 37, 65535 }, { 1, 38, 65535 },
    { 1, 39, 65535 }, { 1, 41, 65535 }, { 1, 42, 65535 }, { 1, 43, 65535 }, { 1, 44, 65535 }, { 1, 0, 65535 }, { 1, 1, 20160 },
    { 1, 2, 65535 }, { 1, 7, 65535 }, { 1, 8, 20160 }, { 1, 9, 65535 }, { 1, 14, 65535 }, { 1, 15, 20160 }, { 1, 16, 65535 },
    { 1, 30, 65535 }, { 0, 18, 0 }, { 0, 13, 0 }, { 0, 16, 0 }, { 0, 12, 0 }, { 0, 14, 0 }, { 0, 0, 0 }, { 0, 4, 0 }, { 0, 8, 0 },
    { 0, 17, 0 }, { 0, 1, 0 }, { 0, 2, 0 }, { 0, 3, 0 }, { 0, 5, 0 }, { 0, 6, 0 }, { 0, 7, 0 }, { 0, 9, 0 }, { 0, 10, 0 },
    { 0, 11, 0 }, { 2, 6, 65535 }, { 2, 13, 65535 }, { 2, 20, 65535 }, { 2, 24, 65535 } };
constexpr double waveSelector[5] = { 0.2, 0.1, 0.0, 0.4, 0.3 };
// the engine's built-in patch (§1.3)
constexpr double defaultP[numP] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5,
                                    0.5, 0.5, 0.0, 0.5, 0.5, 0.5, 0.0, 0.25, 1.0, 0.6, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0,
                                    0.0, 0.5, 1.0, 0.0, 0.5, 0.0, 0.0 };
constexpr int defaultS[numS] = { 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1 };

// --------------------------------------------------------------------------------------- process-wide data (§2.1)
// In the DLL these are globals shared by every instance; LYNX keeps one copy per instance.
struct Globals
{
    std::uint32_t crtRand = 1;           // the C runtime's rand() state of the calling thread
    float rnd[65536] {};                 // random table, rand()/32767
    std::uint16_t ridx = 0;              // its 16-bit read index
    std::int32_t inc[30000 + 20] {};     // phase increments per cent; the 20 entries after the table are the periods'
    std::int32_t period[30000] {};
    std::uint16_t pitchMap[0x10020] {};  // cent value → table index (clamped)
    float tableRate = 0.0f;              // rate the tables were built for
    float halfCos[tabLen] {}, sine[tabLen] {}, sine4[tabLen] {}, tri[tabLen] {};
    std::uint32_t blocks = 0;            // Gen_Render calls
    int chorusTurn = 0;                  // chorus period offset, (+5) mod 53 per chorus set-up
    float rate = 44100.0f, rateRatio = 1.0f, msRate = 44.0999985f, rateKilo = 44.0999985f; // rate, 44100/rate, rate/1000

    int crt() noexcept
    {
        crtRand = crtRand * 214013u + 2531011u;
        return (int) ((crtRand >> 16) & 0x7fff);
    }
    float draw() noexcept { return rnd[ridx++]; }

    void build (float r) // §2.1
    {
        if (tableRate == r)
            return;
        const float k = 4294967296.0f / r;
        for (int i = 0; i < 30000; ++i)
        {
            const auto f = (float) (std::pow (2.0, (double) i / 1200.0) * 4.088681697845459);
            period[i] = (std::int32_t) (std::int64_t) (r / f);
            inc[i] = (std::int32_t) (std::int64_t) (k * f);
        }
        for (int j = 0; j < 20; ++j)
            inc[30000 + j] = period[j];
        tableRate = r;
        for (int i = 0; i < 42; ++i)
            pitchMap[i] = 10;
        for (int i = 42; i < 74; ++i)
            pitchMap[i] = 0;
        for (int i = 74; i < 0x754c; ++i)
            pitchMap[i] = (std::uint16_t) (i - 32);
        for (int i = 0x754c; i < 0x10020; ++i)
            pitchMap[i] = 0x752c;
        for (auto& v : rnd)
            v = (float) crt() / 32767.0f;
        auto x = 1.0f;
        for (auto& v : halfCos)
        {
            v = (float) std::sin ((double) x * 1.5707963267948966);
            x += -0.00048828125f;
        }
        auto y = 0.0f;
        for (int i = 0; i < tabLen; ++i)
        {
            const auto s = (float) std::sin ((double) y);
            y += 0.0015339808305725455f;
            sine[i] = s;
            sine4[i] = s * s * s * s;
        }
        auto t = 0.0f;
        for (int i = 0; i < 1024; ++i)
            tri[i] = t, t += 0.0009765625f;
        for (int i = 1024; i < 3072; ++i)
            tri[i] = t, t += -0.0009765625f;
        for (int i = 3072; i < 4096; ++i)
            tri[i] = t, t += 0.0009765625f;
    }
};

// ------------------------------------------------------------------------------------------- shared patch data (§1.3)
struct OscData
{
    int lfoPitch = 0, shape = 0, coarse = 0, fine = 0, unused18 = 0, level = 0, envPitch = 0;
    int off = 0, octaveUp = 0, octaveUp2 = 0, unison = 0, kind = 0, ring = 0; // ring: only oscillator 1's
};

struct Data
{
    float tab[3][tabLen] {};
    std::uint8_t quality = 0, renderMode = 0;
    int mode4 = 0;
    OscData osc[3];
    int sync = 0;                                    // never set: the syncer oscillator stays unused (§3.2)
    int lfoRate = 0, lfoDelay = 0, lfoOff = 0, lfoRetrigger = 0, lfoShape = 0;
    int ampTime[5] {}, ampLevel[5] {};               // ms; 0…255
    int filtTime[5] {}, filtLevel[5] {};
    std::uint8_t filterEnvOwn = 0;
    int cutoff = 0, envAmt = 0, lfoAmt = 0, res = 0, mixA = 0, mixB = 0, unused144 = 0;
    std::uint8_t filterKind = 0;
    float unused160 = 0.0f, keyTrack = 0.0f;
    int unused168 = 0;
    int filterMark = 0;
    float unused194 = 0.0f, unused19c = 0.0f;

    Data()
    {
        // FUN_180007f10's defaults (only the fields no parameter sets matter)
        osc[0].kind = osc[1].kind = osc[2].kind = 1;
        osc[0].shape = osc[1].shape = osc[2].shape = 0x32;
        osc[0].level = osc[1].level = osc[2].level = 0xff;
        osc[1].off = osc[2].off = 1;
        lfoShape = 2, lfoRate = 0xc, lfoDelay = 0, lfoOff = 0;
        ampTime[0] = 0, ampTime[1] = 400, ampTime[2] = 0, ampTime[3] = 0, ampTime[4] = 2;
        ampLevel[0] = 0xff, ampLevel[1] = 0, ampLevel[2] = 0, ampLevel[3] = 0, ampLevel[4] = 0;
        filtTime[0] = 0, filtTime[1] = 0x40, filtTime[2] = 0, filtTime[3] = 0, filtTime[4] = 2;
        filtLevel[0] = 0xff, filtLevel[1] = 0, filtLevel[2] = 0, filtLevel[3] = 0, filtLevel[4] = 0;
        cutoff = 0x40, envAmt = 0x40, mixA = 0xff;
        filterMark = 0x2254;
    }
};

// ------------------------------------------------------------------------------------------------ voice parts (§3)
struct Env // §3.4
{
    std::uint32_t stage = 4;
    std::int32_t count = 0;
    float step = 0.0f;
    std::int32_t counts[5] {};
    float steps[5] {};
    float targets[5] {};
    float levels[5] {};
    std::uint32_t times[5] {};
    float level = 0.0f;

    // FUN_1800045f0
    void setup (const std::uint32_t* t, const std::uint16_t* l, float msRate) noexcept
    {
        for (int i = 0; i < 5; ++i)
            levels[i] = (float) l[i] / 65280.0f, times[i] = t[i];
        auto n = (int) ((float) (std::int64_t) t[0] * msRate);
        steps[0] = n == 0 ? 0.0f : 1.0f / (float) n;
        targets[0] = 1.0f;
        counts[0] = n;
        const auto l2 = levels[2];
        n = (int) ((float) (std::int64_t) t[1] * msRate);
        steps[1] = n == 0 ? 0.0f : (l2 - 1.0f) / (float) n;
        targets[1] = l2;
        counts[1] = n;
        targets[2] = l2;
        counts[2] = 0x4fffffff;
        steps[2] = 0.0f;
        n = (int) ((float) (std::int64_t) t[3] * msRate);
        steps[3] = n == 0 ? 0.0f : (0.0f - l2) / (float) n;
        targets[3] = targets[4] = 0.0f;
        counts[3] = n;
        count = counts[0];
        step = steps[0];
        counts[4] = 0x4fffffff;
        steps[4] = 0.0f;
        stage = 0;
        level = 0.0f;
    }
    // FUN_180004900: new sustain level
    void setSustain (int v, float msRate) noexcept
    {
        const auto s = (float) v / 255.0f;
        levels[2] = levels[1] = s;
        auto n = (int) ((float) (std::int32_t) times[1] * msRate);
        steps[1] = n == 0 ? 0.0f : (s - levels[0]) / (float) n;
        targets[1] = s;
        counts[1] = n;
        n = (int) ((float) (std::int32_t) times[3] * msRate);
        steps[3] = n == 0 ? 0.0f : (levels[3] - s) / (float) n;
        targets[3] = levels[3];
        counts[3] = n;
        if (stage == 2)
            level = s;
    }
    // a segment's time: index 0 attack (from 0 to levels[0]), 1 decay (levels[0] → levels[1]), 3 release
    void setTime (int seg, int ms, float msRate) noexcept
    {
        times[seg] = (std::uint32_t) ms;
        const auto n = (int) ((float) ms * msRate);
        const auto to = seg == 0 ? levels[0] : seg == 1 ? levels[1] : levels[3];
        const auto from = seg == 0 ? 0.0f : seg == 1 ? levels[0] : levels[2];
        targets[seg] = to;
        counts[seg] = n;
        steps[seg] = n == 0 ? 0.0f : (to - from) / (float) n;
    }
    // FUN_180007980 (one half)
    void release() noexcept
    {
        if (stage < 3)
        {
            count = counts[3] == 0 ? 1 : counts[3];
            stage = 3;
            step = -(level / (float) (std::uint32_t) count);
        }
    }
    // one sample of the segment walk (§3.4); `square` only for the amplitude envelope
    float tick (bool square) noexcept
    {
        float v;
        for (;;)
        {
            if (count != 0)
            {
                v = step + level;
                --count;
                level = v;
                if (! square || stage == 0)
                    return v;
                return v * v;
            }
            v = targets[stage];
            const auto s = stage + 1;
            level = v;
            stage = s;
            count = counts[s] + count;
            step = steps[s];
            if (s == 2 && v == 0.0f)
                break;
        }
        stage = 4;
        count = -1;
        step = 0.0f;
        return square ? v * v : v;
    }
};

struct Lfo // §3.5
{
    std::uint32_t shape = 0;
    std::int32_t count = 0;
    std::uint32_t period = 64;
    float inv = 0.0f;
    std::uint32_t half = 0;
    float frac = 1.0f, random = 0.0f, gain = 0.0f, gainStep = 0.0f;
    std::int32_t delayCount = 0;
    std::uint32_t delay = 0, retrigger = 0;
    float out1 = 0.0f, out2 = 0.0f;

    // FUN_180004b80
    void setRate (std::uint16_t v, float rate) noexcept
    {
        const auto x = 1.0f - (float) v / 999.0f;
        auto p = (std::uint32_t) (std::int64_t) (x * x * x * rate * 4.0f);
        if (p < 0x40)
            p = 0x40;
        period = p;
        inv = 1.0f / (float) p;
        count = (std::int32_t) (std::int64_t) ((float) p * frac);
    }
    // FUN_180004c10
    void reset (std::uint32_t n, Globals& g) noexcept
    {
        if (n == 0)
        {
            delayCount = 0;
            if (delay != 0)
            {
                const auto c = (std::int32_t) (delay * 0xdc8u);
                gain = 0.0f;
                delayCount = c;
                gainStep = 1.0f / (float) c;
            }
        }
        const auto r = n % period;
        half = (n / period) & 1;
        frac = 1.0f - (float) r * inv;
        count = (std::int32_t) (std::int64_t) ((float) period * frac);
        if (r == 0)
            random = g.draw();
    }
    // FUN_180004a30
    void tick (Globals& g) noexcept
    {
        auto f = frac - inv;
        frac = f;
        if (--count == 0)
        {
            half = (half - 1) & 1;
            count = (std::int32_t) period;
            frac = 1.0f;
            random = g.draw();
            f = 1.0f;
        }
        switch (half * 4 | shape)
        {
            case 0: out1 = out2 = 0.0f; break;
            case 2:
            {
                auto a = 1.0f - f;
                out1 = a;
                if (! (f > 0.5f))
                    a = f;
                out2 = -(a + a);
                break;
            }
            case 3: case 7: out1 = out2 = random; break;
            case 1: case 5: out1 = out2 = f; break;
            case 4: out1 = out2 = 1.0f; break;
            case 6:
                out1 = f;
                if (f > 0.5f)
                    f = 1.0f - f;
                out2 = f + f;
                break;
            default: break;
        }
        if (delayCount != 0)
        {
            const auto gn = gain;
            --delayCount;
            out1 = gn * out1;
            out2 = gn * out2;
            gain = gn + gainStep;
        }
    }
};

struct Osc // §3.2: kind 2 = the two-reads-per-sample table oscillator, 4 = filtered noise
{
    int kind = 0;
    float out = 0.0f;
    std::uint16_t cur = 0, base = 0;
    float level = 0.0f;
    int tab = 0; // which Data table
    std::uint32_t inc = 0, ph = 0, inc2 = 0, ph2 = 0;
    int unison = 0;
    float prev1 = 0.0f, prev2 = 0.0f;
    int detune = 0;
    float n1 = 0.0f, n2 = 0.0f, ncoef = 0.3025f; // noise

    // FUN_1800039f0 (kind 2; the noise ignores pitch)
    void setPitch (int offset, const Globals& g) noexcept
    {
        if (kind != 2)
            return;
        const auto p = (std::uint16_t) (offset + base);
        if (p == cur)
            return;
        const auto at = std::min<std::uint32_t> ((std::uint32_t) (detune + 0x20 + (int) p), 0x1001f);
        const auto i = g.pitchMap[at];
        cur = i;
        inc = (std::uint32_t) (g.inc[i] + detune);
        inc2 = (std::uint32_t) (g.inc[i + 20] + detune);
    }
    void resetPhase2() noexcept
    {
        if (kind == 2)
            ph2 = ph;
    }
    float render (const Data& d, Globals& g) noexcept
    {
        if (kind == 4)
        {
            const auto c = ncoef;
            const auto r = g.draw();
            const auto s1 = n2 * c + n1;
            n1 = s1;
            const auto t = (r - s1) - n2;
            n2 = t * c + n2;
            return n2 * 16.0f;
        }
        const auto* t = d.tab[tab];
        auto a = t[ph >> 20] + prev1;
        ph += inc;
        a = (a + t[ph >> 20]) * 0.333332986f;
        ph += inc;
        prev1 = a;
        if (unison == 0)
            return a;
        auto b = t[ph2 >> 20] + prev2;
        ph2 += inc2;
        b = (b + t[ph2 >> 20]) * 0.333332986f;
        ph2 += inc2;
        prev2 = b;
        return b + a;
    }
};

struct Filter // §3.6, FUN_180004130 / FUN_180004020
{
    float f = 0.0f, base = 0.0f;
    std::uint16_t res = 0;
    double mixLow = 0.0, mixBand = 0.0, mixHigh = 0.0, angleA = 0.0, angleB = 0.0;
    double low1 = 0.0, band1 = 0.0, high1 = 0.0, f1 = 0.0, f2 = 0.0, damp = 0.0, x70 = 0.0, low2 = 0.0, band2 = 0.0, high2 = 0.0, q = 0.0;
    bool kindFlag = false;

    void setCutoff (float v, float rateRatio) noexcept
    {
        auto x = v + base;
        f = x;
        if (x > 1.0f)
            f = x = 1.0f;
        else if (x < 9.99999975e-05f)
            f = x = 9.99999975e-05f;
        const auto a = x * rateRatio;
        f1 = (double) a;
        if (1.0 < (double) a)
            f1 = 1.0;
        f2 = (double) (x * x);
        f2 = (double) rateRatio * (double) (x * x);
        if (1.0 < f2)
            f2 = 1.0;
    }
    float process (float in) noexcept
    {
        const auto b2 = band2, b1 = band1;
        const auto l1 = b1 * f1 + low1;
        low1 = l1;
        const auto l2 = f2 * b2 + low2;
        low2 = l2;
        const auto h2 = ((double) in - l2) - damp * b2;
        high2 = h2;
        const auto nb2 = h2 * f2 + b2;
        band2 = nb2;
        const auto h1 = ((nb2 * mixBand + l2 * mixLow + h2 * mixHigh) - l1) - b1 * damp;
        high1 = h1;
        const auto nb1 = h1 * f1 + b1;
        band1 = nb1;
        return (float) (nb1 * mixBand + l1 * mixLow + h1 * mixHigh);
    }
    void setRes (int v) noexcept
    {
        const auto r = 1.0 - (double) v / 255.0;
        q = 1.0 - r * r;
        damp = 1.1 - q;
    }
    void clearState() noexcept { x70 = high2 = band2 = low2 = high1 = band1 = low1 = 0.0; }
};

struct Voice // the 0x340-byte voice object of the DLL (§3)
{
    std::uint8_t quality = 0, renderMode = 0;
    int mode4 = 0;
    float unused14 = 0.0f;
    Env amp, filt;
    std::uint8_t filterEnvOwn = 0;
    float unused160 = 0.0f, velocity = 0.0f;
    float smooth = 0.0f;
    bool ring = false;
    std::optional<Osc> osc[3];
    int oscKind[3] {};
    int lfoOn = 0;
    Lfo lfo;
    float lfoPitch[3] {}, envPitch[3] {};
    int pitch = 0;
    std::uint16_t oscPitch[3] {}, oscBase[3] {};
    float pitchOffset = 0.0f, cutoffParam = 0.0f;
    std::uint32_t counter = 0;
    std::uint8_t filterOn = 0;
    Filter filter;
    float envAmt2 = 0.0f, envAmt = 0.0f, lfoAmt = 0.0f;
    std::uint8_t envAmtOn = 0, lfoAmtOn = 0;
    int spread = 0;
};

// FUN_180007ae0: oscillator k's 4096-point table from its wave kind, shape and octave layers (§2.2)
void buildTable (Data& d, int k, Globals& g)
{
    static thread_local float temp[tabLen], temp2[tabLen];
    const auto& o = d.osc[k];
    const auto shape = (std::uint16_t) o.shape;
    if (o.kind == 0)
    {
        auto v = 0.0f;
        int i = 0;
        const auto w = (float) shape * 0.00999999978f * 4096.0f;
        if (0.0f < w)
        {
            do
            {
                ++i;
                v = v * 0.649999976f + 0.227499992f;
                temp[i - 1] = v;
            } while ((float) i < w);
        }
        for (; i < 0xff8; ++i)
            temp[i] = v = v * 0.649999976f - 0.227499992f;
        for (; i < tabLen; ++i)
            temp[i] = v = v * 0.649999976f;
    }
    else if (o.kind == 1)
        std::memcpy (temp, g.halfCos, sizeof temp);
    else if (o.kind == 2)
        std::memcpy (temp, g.tri, sizeof temp);
    else if (o.kind == 4)
    {
        const auto m = (float) (shape + 2) / 96.0f;
        const auto n = 1.0f - m;
        for (int i = 0; i < tabLen; ++i)
            temp[i] = g.sine[i] * n + g.sine4[i] * m;
    }
    // (kind 3, noise: the temporary keeps what the last build left in it)
    auto* t = d.tab[k];
    std::memcpy (t, temp, sizeof temp);
    if (o.octaveUp == 0)
    {
        if (o.octaveUp2 != 0)
            for (int j = 0; j < tabLen; ++j)
                t[j] = temp[j] + temp[4 * (j & 1023)];
    }
    else if (o.octaveUp2 == 0)
    {
        for (int j = 0; j < tabLen; ++j)
            t[j] = temp[j] + temp[2 * (j & 2047)];
    }
    else
    {
        for (int j = 0; j < tabLen; ++j)
            temp2[j] = temp[2 * (j & 2047)];
        for (int j = 0; j < tabLen; ++j)
            t[j] = temp2[j] + temp[4 * (j & 1023)] + temp[j];
    }
    if (d.renderMode == 0)
    {
        for (int i = 0; i < 4094; ++i)
            t[i] = (float) ((double) (t[i + 1] + t[i]) * 0.5);
        t[4094] = (float) ((double) (t[4095] + t[4094]) * 0.5);
    }
}

// ------------------------------------------------------------------------------------------------ the engine (§2–4)
// The front-end's own control values right after creation (they are sent to the engine once, §1.4)
constexpr double frontDefaults[numP] = {
    0.49999237048905165, 0.5, 0.52343022812237738, 0.49999237048905165, 0.49999237048905165, 0.49999237048905165, 0.1,
    0.49999237048905165, 0.25, 0.48825818265049209, 0.49999237048905165, 0.49999237048905165, 0.49999237048905165, 0.1,
    0.77343404287785156, 0.0, 0.49999237048905165, 0.63279163805600058, 0.49999237048905165, 0.49999237048905165, 0.2, 0.0,
    0.49999237048905165, 0.5, 0.0, 0.0, 0.20543221179522392, 0.19790951400015258, 0.68494697489890899, 0.49999237048905165,
    0.36719310292210272, 0.0, 0.0, 0.0, 0.0, 0.0, 0.65624475471122301, 0.82420080872816048, 0.12108033875028611,
    0.57419699397268631, 1.0, 0.093736171511406119, 0.22262912947280078, 0.3632715342946517, 0.11326771953917754 };
constexpr int frontDefaultS[numS] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

class Engine
{
public:
    Globals g;
    Data d;
    Voice voices[numVoices];
    double P[numP] {};
    int S[numS] {};
    double offsets[numP] {}; // modulation offsets (always 0 here)
    int pitchBase = 0;       // preset +0x48
    float gain = 0.5f;       // P39
    int chorusOn = 0;

    // host voices (TPlugVoice) in the 12 slots
    struct Slot
    {
        bool used = false;
        int pan = 0, pitch = 0;            // the host's Final levels in the old voice-parameter record: integer pan
        float vol = 1.0f, fcut = 0.0f, fres = 0.0f; // (−64…64) and pitch (cents); float volume, cutoff, resonance
        float l = 0.0f, r = 0.0f, dl = 0.0f, dr = 0.0f;
        int ramp = 0;
        float lastRes = 0.0f;
        std::uint32_t started = 0, released = 0;
        int id = -1;
        int notePitch = 0; // Final.Pitch at the trigger (cents); a slide note moves `pitch` away from it (issue #14)
        HostSlide slide;
    };
    Slot slots[numVoices];
    std::uint32_t samples = 0; // +0x40dd8

    // chorus (§4.2)
    std::vector<float> line = std::vector<float> (65536, 0.0f);
    double rpos = 0.0, speed = 1.0, speedStep = 0.0, speedMax = 1.0, speedMin = 1.0;
    std::uint32_t wpos = 0;
    int tail = 0, tailLen = 0;

    std::vector<float> mixL, mixR, mono, outL, outR;

    // ------------------------------------------------------------------ creation (CreatePlugInstance, §2.3)
    void create()
    {
        chorusSetup();
        // the 12 voices are linked one by one; each link sets the voice up from the fresh data at the plain rate
        for (auto& v : voices)
            initVoice (v, false);
        setSpread();
        // quality byte on (no voice set-up), render-mode byte off (every voice set up again: tables at 2× rate)
        d.quality = 1;
        d.renderMode = 0, d.mode4 = 0;
        for (auto& v : voices)
            initVoice (v, false);
        // the engine's built-in patch
        for (auto& o : offsets)
            o = 0.0;
        applyPatch (defaultP, defaultS);
        pitchBase = 0x960;
        for (int i = 0; i < numP; ++i)
            P[i] = defaultP[i];
        for (int i = 0; i < numS; ++i)
            S[i] = defaultS[i];
        chorusOn = 1;
        gain = 0.5f;
        // the front-end then sends its 58 control values
        for (int h = 0; h < 58; ++h)
        {
            const auto& hp = hostParams[h];
            int v;
            if (hp.kind == 1)
                v = (int) std::lround (frontDefaults[hp.index] * (double) hp.scale);
            else if (hp.kind == 0)
                v = frontDefaultS[hp.index];
            else
            {
                const auto x = frontDefaults[hp.index];
                v = x == 0.2 ? 0 : x == 0.1 ? 1 : x == 0.0 ? 2 : x == 0.4 ? 3 : 4;
            }
            hostSet (h, v);
        }
    }

    // ProcessParam, engine side: get (flag 2) and set (flag 1) (§1.2)
    int engineGet (int h) const
    {
        const auto& hp = hostParams[h];
        if (hp.kind == 1)
            return (int) std::floor ((double) hp.scale * P[hp.index]);
        if (hp.kind == 0)
            return S[hp.index];
        const auto x = P[hp.index];
        return x == 0.0 ? 2 : x == 0.4 ? 3 : x == 0.2 ? 0 : x == 0.3 ? 4 : 1;
    }
    void hostSet (int h, int v)
    {
        const auto& hp = hostParams[h];
        if (hp.kind == 1)
        {
            P[hp.index] = (double) v / (double) hp.scale;
            setParam (hp.index, P[hp.index], true);
        }
        else if (hp.kind == 0)
            setSwitch (hp.index, v != 0 ? 1 : 0, true);
        else
        {
            P[hp.index] = waveSelector[std::clamp (v, 0, 4)];
            setParam (hp.index, P[hp.index], true);
        }
    }
    // the front-end's read-back/write of host parameter h (the 48-step pitch knobs go through value/420)
    void frontReset (int h)
    {
        auto v = engineGet (h);
        const bool steps = h == 27 || h == 30 || h == 33;
        if (steps)
            v = (int) std::nearbyint ((double) v * 0.002380952380952381);
        hostSet (h, steps ? v * 420 : v);
    }

    // SaveRestoreState (load) + the front-end's re-set of its 58 parameters (§1.1)
    void load (const double* p, const int* s, int version)
    {
        double q[numP];
        std::copy (p, p + numP, q);
        if (version == 0)
            q[39] = q[39] * 0.75;
        if (version < 5)
            for (int k : { 1, 8, 15 })
            {
                auto x = (int) (q[k] * 24.0 + -12.0);
                if (q[k + 5] == 0.3)
                    x += 12;
                q[k] = (double) (x + 24) / 48.0;
            }
        if (version < 13)
            for (int k : { 1, 8, 15 })
            {
                const auto x = (int) (q[k] * 48.0 - 24.0);
                q[k] = (double) (x + 24) / 48.0;
            }
        std::copy (q, q + numP, P);
        std::copy (s, s + numS, S);
        for (auto& o : offsets)
            o = 0.0;
        pitchBase = 0x18 * 100;
        for (auto& v : voices)
            v.pitchOffset = 0.0f;
        chorusOn = S[18];
        gain = (float) P[39];
        applyPatch (P, S);
        for (int h = 0; h < 58; ++h)
            frontReset (h);
    }

    void setRate (double fs)
    {
        g.rate = (float) fs;
        g.rateRatio = 44100.0f / g.rate;
        g.rateKilo = g.rate / 1000.0f;
        g.msRate = g.rate / 1000.0f;
        chorusSetup();
        // Dispatcher 4 ends with Dispatcher 1 (ProcessMode): render-mode byte off (no voice set-up), quality byte on
        // with every voice set up again
        d.renderMode = 0, d.mode4 = 0;
        d.quality = 1;
        for (auto& v : voices)
            initVoice (v, false);
    }

    // ------------------------------------------------------------------ patch → data (FUN_180002a30 + FUN_180001820)
    void applyPatch (const double* p, const int* s)
    {
        for (int i = 0; i < numS; ++i)
            setSwitch (i, s[i], false);
        for (int i = 0; i < numP; ++i)
            setParam (i, p[i], false);
        for (auto& v : voices)
            initVoice (v, false);
    }

    // FUN_180002340
    void setSwitch (int i, int v, bool propagate)
    {
        S[i] = v;
        int rebuild = -1;
        switch (i)
        {
            case 0: d.osc[0].off = v == 0; break;
            case 1: d.osc[0].octaveUp = v; rebuild = 0; break;
            case 2: d.osc[0].octaveUp2 = v; rebuild = 0; break;
            case 3: d.osc[0].unison = v; break;
            case 4: d.osc[1].off = v == 0; break;
            case 5: d.osc[1].octaveUp = v; rebuild = 1; break;
            case 6: d.osc[1].octaveUp2 = v; rebuild = 1; break;
            case 7: d.osc[1].unison = v; break;
            case 8: d.osc[2].off = v == 0; break;
            case 9: d.osc[2].octaveUp = v; rebuild = 2; break;
            case 10: d.osc[2].octaveUp2 = v; rebuild = 2; break;
            case 11: d.osc[2].unison = v; break;
            case 12: d.lfoOff = v == 0; break;
            case 13: d.lfoRetrigger = v; break;
            case 14: d.filterMark = v != 0 ? 0 : filterOffMark; break;
            case 15: d.filterKind = v == 1; break;
            case 16: d.filterEnvOwn = v == 0; break;
            case 17: d.osc[0].ring = v; break;
            case 18: chorusOn = v == 1; return;
            default: break;
        }
        if (rebuild >= 0)
            buildTable (d, rebuild, g);
        if (! propagate)
            return;
        for (auto& vc : voices)
            switchVoice (vc, i);
        if (i == 0 || i == 4 || i == 8)
        {
            const auto b = i / 4 * 7;
            setParam (b + 1, P[b + 1], true);
            setParam (b + 3, P[b + 3], true);
            setParam (b, P[b], true);
        }
    }

    // FUN_180001440: the voice side of a switch change
    void switchVoice (Voice& v, int i)
    {
        switch (i)
        {
            case 0: case 4: case 8: setupOscs (v); break;
            case 1: case 2: case 3: checkOsc (v, 0); break;
            case 5: case 6: case 7: checkOsc (v, 1); break;
            case 9: case 10: case 11: checkOsc (v, 2); break;
            case 12: case 13: lfoSetup (v); break;
            case 14: filterSetup (v); break;
            case 16: v.filterEnvOwn = d.filterEnvOwn; break;
            case 17: v.ring = d.osc[0].ring != 0; break;
            default: break;
        }
    }

    // FUN_180006c70: an oscillator re-initialised after a switch (a new object if its kind changes)
    void checkOsc (Voice& v, int k)
    {
        const auto kind = d.osc[k].off != 0 ? -1 : oscKindFor (v, k);
        if (d.osc[k].off != 0 || kind != v.oscKind[k])
        {
            setupOscs (v);
            return;
        }
        if (! v.osc[k])
            return;
        v.lfoPitch[k] = (float) d.osc[k].lfoPitch * 12.0f;
        v.envPitch[k] = (float) d.osc[k].envPitch * 12.0f;
        auto& o = *v.osc[k];
        if (o.kind == 2)
        {
            o.tab = k;
            o.out = 0.0f;
            o.unison = d.osc[k].unison;
            o.ph2 = o.ph;
            o.cur = o.base;
            o.setPitch (1, g);
            o.setPitch (0x12c0, g);
            o.ph = o.ph2 = phaseFromDraw (g.draw());
        }
        oscPitchChanged (v, k);
    }

    static std::uint32_t phaseFromDraw (float r) noexcept
    {
        const auto x = r * 2147483648.0f; // (r = 1 overflows the conversion: the integer-indefinite value)
        return x >= 2147483648.0f ? 0x80000000u : (std::uint32_t) (std::int32_t) x;
    }

    // FUN_180001820
    void setParam (int idx, double value, bool propagate)
    {
        for (;;)
        {
            P[idx] = value;
            auto x = value + offsets[idx];
            x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
            int chain = -1;
            const auto k = std::min (idx / 7, 2);
            auto& o = d.osc[k];
            auto wave = [&] (int kk)
            {
                auto& ok = d.osc[kk];
                if (x == 0.0) { ok.kind = 2; buildTable (d, kk, g); }
                else if (x == 0.1) { ok.kind = 1; buildTable (d, kk, g); }
                else if (x == 0.2) { ok.kind = 0; buildTable (d, kk, g); }
                else if (x == 0.3) { ok.kind = 4; buildTable (d, kk, g); }
                else if (x == 0.4) ok.kind = 3;
                chain = kk * 7 + 1;
            };
            switch (idx)
            {
                case 0: case 7: case 14: o.shape = 2 - (int) (x * -96.0); buildTable (d, k, g); break;
                case 1: case 8: case 15: o.coarse = ((int) (std::floor (x * 48.0) - 24.0)) * 100; break;
                case 2: case 9: case 16: o.fine = (int) (x * 200.0 - 100.0); break;
                case 3: case 10: case 17: o.level = (int) (x * 100.0); break;
                case 4: case 11: case 18: o.lfoPitch = (int) (x * 200.0 - 100.0); break;
                case 5: case 12: case 19: o.envPitch = (int) (x * 200.0 - 100.0); break;
                case 6: case 13: case 20: wave (k); break;
                case 21: d.lfoDelay = (int) (x * 100.0); break;
                case 22: d.lfoRate = (int) (x * 999.0); break;
                case 24:
                    if (x == 0.0) d.lfoShape = 2;
                    else if (x == 0.1) d.lfoShape = 1;
                    else if (x == 0.2) d.lfoShape = 0;
                    else if (x == 0.4) d.lfoShape = 3;
                    break;
                case 26: d.cutoff = (int) (x * 255.0); break;
                case 27: d.res = (int) (x * 255.0); break;
                case 28: d.envAmt = (int) ((x - 0.5) * 255.0); break;
                case 29: d.lfoAmt = (int) ((x - 0.5) * 255.0); break;
                case 30: d.keyTrack = (float) x; break;
                case 31: d.unused160 = (float) x; break;
                case 32: d.mixA = (int) (x * 255.0); break;
                case 33: d.mixB = (int) (x * 255.0); break;
                case 34: d.unused144 = (int) (x * 255.0); break;
                case 35: d.ampTime[0] = (int) (x * x * x * 8000.0); d.ampLevel[0] = 0xff; break;
                case 36: d.ampTime[1] = (int) (x * x * 8000.0); break;
                case 37: d.ampLevel[1] = (int) (x * 255.0); d.ampLevel[2] = d.ampLevel[1]; d.ampTime[2] = -1; break;
                case 38: d.ampLevel[3] = 0; d.ampTime[3] = (int) (x * x * 8000.0); break;
                case 39: d.unused19c = (float) x; gain = (float) x; return;
                case 40: d.unused194 = (float) (x * x); break;
                case 41: d.filtTime[0] = (int) (x * x * x * 8000.0); d.filtLevel[0] = 0xff; break;
                case 42: d.filtTime[1] = (int) (x * x * 8000.0); break;
                case 43: d.filtLevel[1] = (int) (x * 255.0); d.filtLevel[2] = d.filtLevel[1]; d.filtTime[2] = -1; break;
                case 44: d.filtLevel[3] = 0; d.filtTime[3] = (int) (x * x * 8000.0); break;
                default: break;
            }
            if (! propagate)
                return;
            for (auto& v : voices)
                updateVoice (v, idx, x);
            if (chain < 0)
                return;
            idx = chain;
            value = P[idx];
        }
    }

    // FUN_180001000: the voice side of a parameter change
    void updateVoice (Voice& v, int idx, double x)
    {
        switch (idx)
        {
            case 0: case 7: case 14: case 23: case 25: break;
            case 1: case 8: case 15:
            {
                auto& o = v.osc[idx / 7];
                if (o && o->kind == 4)
                {
                    const auto c = (float) x * 0.899999976f + 0.100000001f;
                    o->ncoef = c * c;
                }
                oscPitchChanged (v, idx / 7);
                break;
            }
            case 2: case 9: case 16: oscPitchChanged (v, idx / 7); break;
            case 3: case 10: case 17: setLevels (v); break;
            case 4: case 11: case 18: v.lfoPitch[idx / 7] = (float) d.osc[idx / 7].lfoPitch * 12.0f; break;
            case 5: case 12: case 19: v.envPitch[idx / 7] = (float) d.osc[idx / 7].envPitch * 12.0f; break;
            case 6: case 13: case 20: setupOscs (v); break;
            case 21: case 22:
                if (d.lfoOff != 0)
                {
                    v.lfoOn = 0;
                    v.lfo.out1 = 0.0f;
                }
                else
                {
                    v.lfoOn = 1;
                    if (idx == 22)
                        v.lfo.setRate ((std::uint16_t) d.lfoRate, g.rate);
                    else
                        v.lfo.delay = (std::uint16_t) d.lfoDelay;
                }
                break;
            case 24: lfoSetup (v); break;
            case 26: v.filter.base = (float) d.cutoff / 255.0f; break;
            case 27: v.filter.res = (std::uint16_t) d.res; v.filter.setRes (v.filter.res); break;
            case 28:
                if (v.envAmtOn) { v.envAmt = v.envAmt2 = (float) d.envAmt * 0.0078125f; break; }
                filterSetup (v); break;
            case 29:
                if (v.lfoAmtOn) { v.lfoAmt = (float) d.lfoAmt * 0.0078125f; break; }
                filterSetup (v); break;
            case 35: v.amp.setTime (0, d.ampTime[0], g.msRate); break;
            case 36: v.amp.setTime (1, d.ampTime[1], g.msRate); break;
            case 37: v.amp.setSustain (d.ampLevel[2], g.msRate); break;
            case 38: v.amp.setTime (3, std::max (2, d.ampTime[3]), g.msRate); break;
            case 39: v.unused160 = d.unused19c; break;
            case 40: v.unused14 = d.unused194; break;
            case 41: v.filt.setTime (0, d.filtTime[0], g.msRate); break;
            case 42: v.filt.setTime (1, d.filtTime[1], g.msRate); break;
            case 43: v.filt.setSustain (d.filtLevel[2], g.msRate); break;
            case 44: v.filt.setTime (3, std::max (1, d.filtTime[3]), g.msRate); break;
            default: filterSetup (v); break;
        }
    }

    // ------------------------------------------------------------------ voice set-up (FUN_180008180 and parts)
    void lfoSetup (Voice& v)
    {
        v.lfoOn = 0;
        v.lfo.out1 = 0.0f;
        if (d.lfoOff == 0)
        {
            v.lfoOn = 1;
            v.lfo.shape = (std::uint32_t) d.lfoShape;
            v.lfo.setRate ((std::uint16_t) d.lfoRate, g.rate);
            v.lfo.retrigger = (std::uint32_t) d.lfoRetrigger;
            v.lfo.half = 0;
            v.lfo.delay = (std::uint16_t) d.lfoDelay;
            v.lfo.reset (0, g);
        }
    }

    void filterSetup (Voice& v)
    {
        v.filterOn = 0;
        if ((std::uint16_t) d.filterMark == filterOffMark)
            return;
        auto& f = v.filter;
        f.angleB = ((double) (std::uint16_t) d.mixB / 255.0) * 1.5707963267948966;
        f.angleA = ((double) (std::uint16_t) d.mixA / 255.0) * 1.5707963267948966;
        f.base = (float) (std::uint16_t) d.cutoff / 255.0f;
        const auto cb = std::cos (f.angleB);
        f.mixHigh = std::sin (f.angleA) * cb;
        f.mixLow = std::cos (f.angleA) * cb;
        f.mixBand = std::sin (f.angleB);
        f.kindFlag = d.filterKind == 1;
        f.res = (std::uint16_t) d.res;
        f.setRes (f.res);
        v.filterOn = 1;
        v.envAmtOn = v.lfoAmtOn = 0;
        if (d.envAmt != 0)
        {
            v.envAmtOn = 1;
            v.envAmt = v.envAmt2 = (float) d.envAmt * 0.0078125f;
        }
        if (d.lfoAmt != 0)
        {
            v.lfoAmtOn = 1;
            v.lfoAmt = (float) d.lfoAmt * 0.0078125f;
        }
    }

    int oscKindFor (const Voice& v, int k) const
    {
        if (d.osc[k].kind == 3)
            return 4;
        if (k == 2 && d.osc[1].off == 0 && d.osc[1].kind != 3 && d.sync != 0)
            return 6;
        if (v.renderMode != 0)
            return 7;
        return v.quality != 0 ? 2 : 1;
    }

    // FUN_180006660
    void setupOscs (Voice& v)
    {
        for (int k = 0; k < 3; ++k)
            v.oscPitch[k] = (std::uint16_t) (d.osc[k].coarse + d.osc[k].fine);
        for (int k = 0; k < 3; ++k)
        {
            v.envPitch[k] = 0.0f;
            v.lfoPitch[k] = 0.0f;
            const auto& od = d.osc[k];
            if (od.off != 0)
            {
                v.osc[k].reset();
                v.oscKind[k] = 0;
                continue;
            }
            const auto kind = oscKindFor (v, k);
            if (kind != v.oscKind[k])
            {
                std::uint32_t keep = 0;
                if (v.oscKind[k] != 0 && v.osc[k] && (v.oscKind[k] == 1 || v.oscKind[k] == 2))
                    keep = v.osc[k]->ph;
                v.oscKind[k] = kind;
                Osc o;
                o.kind = kind;
                o.ph = o.ph2 = keep;
                v.osc[k] = o;
            }
            auto& o = *v.osc[k];
            const auto p = (std::uint16_t) (od.coarse + v.pitch + od.fine);
            if (kind == 4)
            {
                o.cur = o.base = p;
                o.level = 1.0f;
                o.out = 0.0f;
            }
            else if (kind == 2)
            {
                o.cur = o.base = p;
                o.out = 0.0f;
                o.unison = od.unison;
                o.level = 0.5f;
                o.ph2 = o.ph;
                o.tab = k;
                o.setPitch (1, g);
                o.setPitch (0x12c0, g);
                const auto ph = phaseFromDraw (g.draw());
                o.ph = o.ph2 = ph;
                o.detune = v.spread + k;
            }
            else if (kind == 1 || kind == 7)
            {
                // the plain and render-mode table oscillators (only alive before the quality byte is set at creation):
                // only their random start-phase draw matters here
                o.cur = o.base = p;
                g.draw();
            }
            if (od.lfoPitch != 0)
                v.lfoPitch[k] = (float) od.lfoPitch * 12.0f;
            if (od.envPitch != 0)
                v.envPitch[k] = (float) od.envPitch * 12.0f;
        }
        for (int k = 0; k < 3; ++k)
            if (v.osc[k])
                v.oscBase[k] = v.osc[k]->base;
        setLevels (v);
    }

    void setLevels (Voice& v)
    {
        for (int k = 0; k < 3; ++k)
            if (v.osc[k])
            {
                const auto a = (float) d.osc[k].level / 100.0f;
                v.osc[k]->level = a * a;
            }
    }

    void oscPitchChanged (Voice& v, int k)
    {
        if (! v.osc[k])
            return;
        const auto s = (std::uint16_t) (d.osc[k].coarse + d.osc[k].fine);
        v.oscPitch[k] = s;
        v.osc[k]->base = (std::uint16_t) (s + v.pitch);
        v.oscBase[k] = v.osc[k]->base;
    }

    void initVoice (Voice& v, bool keepPlaying)
    {
        g.build (d.quality != 0 && d.renderMode == 0 ? g.rate * 2.0f : g.rate);
        v.quality = d.quality;
        v.mode4 = d.mode4;
        v.renderMode = d.renderMode;
        auto envFrom = [] (Env& e, const int* times, const int* levels, int minRelease, float msRate, int* liveTimes)
        {
            std::uint32_t t[5];
            std::uint16_t l[5];
            if (liveTimes[3] < minRelease)
                liveTimes[3] = minRelease;
            for (int i = 0; i < 5; ++i)
            {
                t[i] = (std::uint32_t) times[i];
                auto s = (std::uint16_t) (levels[i] << 8);
                l[i] = s == 0x100 ? 1 : s;
            }
            e.setup (t, l, msRate);
        };
        envFrom (v.amp, d.ampTime, d.ampLevel, 2, g.msRate, d.ampTime);
        v.filterEnvOwn = d.filterEnvOwn;
        envFrom (v.filt, d.filtTime, d.filtLevel, 1, g.msRate, d.filtTime);
        lfoSetup (v);
        setupOscs (v);
        for (int k = 0; k < 3; ++k)
            if (d.osc[k].off == 0)
                buildTable (d, k, g);
        v.ring = d.osc[0].ring != 0;
        v.filterOn = 0;
        if ((std::uint16_t) d.filterMark != filterOffMark)
        {
            filterSetup (v);
            v.filter.clearState();
        }
        v.unused14 = d.unused194;
        v.unused160 = d.unused19c;
        v.counter = 0;
        g.crtRand = 666;
        for (int k = 0; k < 3; ++k)
            if (v.osc[k])
                v.oscBase[k] = v.osc[k]->base;
        if (v.osc[0])
            v.osc[0]->base = v.oscBase[0];
        if (v.osc[1])
            v.osc[1]->base = (std::uint16_t) (v.oscBase[1] - 4);
        if (v.osc[2])
            v.osc[2]->base = (std::uint16_t) (v.oscBase[2] + 4);
        for (auto& o : v.osc)
            if (o)
                o->setPitch (0, g);
        v.lfo.reset (0, g);
        v.smooth = 0.0f;
        v.pitchOffset = 0.0f;
        if (! keepPlaying)
            v.amp.stage = v.filt.stage = 4;
    }

    void setSpread()
    {
        double up = -8.0, down = 8.0, cur = 0.0;
        const auto step = 8.0 / (double) std::max (1, numVoices / 2);
        for (auto& v : voices)
        {
            v.spread = (int) cur;
            if (0.0 <= cur)
                cur = up, up += step;
            else
                cur = down, down -= step;
        }
    }

    void chorusSetup()
    {
        const int len = 333 + g.chorusTurn;
        g.chorusTurn = (g.chorusTurn + 5) % 0x35;
        rpos = (double) (65536.0f - (g.rate / 1000.0f) * 20.0f);
        const auto dd = 0.12 * 0.12 * 0.5;
        speedMax = dd + 1.0;
        speedMin = 1.0 - dd;
        const auto f = (float) len * g.msRate;
        speed = 1.0;
        const auto s = (speedMax - speedMin) / (double) f;
        speedStep = -(s + s);
        tail = 0;
        tailLen = (int) ((g.rate / 1000.0f) * 20.0f);
    }

    // ------------------------------------------------------------------ host calls
    // FUN_180001700 + FUN_180006510: the note's pitch (cents, Final.Pitch + 0x960)
    void voicePitch (Voice& v, int p)
    {
        auto x = p + pitchBase;
        if (x < 100)
            x = 100;
        v.pitch = x + 0x960;
        for (int k = 0; k < 3; ++k)
            if (v.osc[k])
            {
                const auto s = (std::uint16_t) (d.osc[k].coarse + d.osc[k].fine);
                v.oscPitch[k] = s;
                v.osc[k]->base = (std::uint16_t) (s + v.pitch);
                v.oscBase[k] = v.osc[k]->base;
            }
    }

    void noteOnVoice (Voice& v, int p, int vel, std::uint32_t counter, float fcut, float fres)
    {
        auto x = (int) (std::uint32_t) ((std::uint32_t) p + (std::uint32_t) pitchBase);
        if (x < 100)
            x = 100;
        v.pitch = x + 0x960;
        for (int k = 0; k < 3; ++k)
            if (v.osc[k])
            {
                const auto s = (std::uint16_t) (d.osc[k].coarse + d.osc[k].fine);
                v.oscPitch[k] = s;
                v.osc[k]->base = (std::uint16_t) (s + v.pitch);
                v.oscBase[k] = v.osc[k]->base;
            }
        auto r = (int) v.filter.res - (int) (fres * -255.0f);
        r = r < 0 ? 0 : (r > 0xff ? 0xff : r);
        v.velocity = (float) vel / 255.0f;
        v.filter.setRes (r);
        v.cutoffParam = (float) x / 12700.0f * d.keyTrack + fcut;
        v.counter = 0;
        if (v.amp.level == 0.0f && v.filterOn != 0)
            v.filter.clearState();
        v.amp.count = v.amp.counts[0];
        v.amp.step = v.amp.steps[0];
        v.filt.count = v.filt.counts[0];
        v.filt.step = v.filt.steps[0];
        v.amp.stage = 0;
        v.amp.level = 0.0f;
        v.filt.stage = 0;
        v.filt.level = 0.0f;
        if (v.osc[0])
            v.osc[0]->base = v.oscBase[0];
        if (v.osc[1])
            v.osc[1]->base = (std::uint16_t) (v.oscBase[1] - 4);
        if (v.osc[2])
            v.osc[2]->base = (std::uint16_t) (v.oscBase[2] + 4);
        for (auto& o : v.osc)
            if (o)
                o->resetPhase2();
        for (auto& o : v.osc)
            if (o)
                o->setPitch (0, g);
        if (v.lfoOn == 1)
            v.lfo.reset (v.lfo.retrigger != 0 ? 0u : counter, g);
        v.smooth = 0.0f;
    }

    // the host's integer-pan ComputeLRVol (host slot 3, §2.4)
    static void hostLR (int pan, float vol, float& l, float& r) { flvp::panLaw ((float) pan * 0.015625f, vol, l, r); }

    // TriggerVoice converts Final.Pitch as if it were a float although the old record holds an integer (§2.4):
    // the pitch it sets up lasts until the next Gen_Render, which reads the integer
    static int pitchAsFloat (int p) noexcept
    {
        float f;
        std::memcpy (&f, &p, 4);
        if (! (f > -2147483648.0f && f < 2147483648.0f))
            return (int) 0x80000000;
        return (int) f;
    }

    int trigger (int id, int pan, float vol, int pitch, float fcut, float fres) // TriggerVoice (§2.4)
    {
        int slot = -1;
        for (int i = 0; i < numVoices; ++i)
            if (! slots[i].used)
            {
                slot = i;
                break;
            }
        if (slot < 0)
        {
            // stealing: the voice released longest ago, else the one with the highest start stamp
            int best = -1;
            std::uint32_t bestAge = 0;
            for (int i = 0; i < numVoices; ++i)
            {
                const auto age = g.blocks - slots[i].released;
                if (age != g.blocks && bestAge < age)
                    best = i, bestAge = age;
            }
            if (best < 0)
            {
                // none released: the one with the highest start stamp (the newest note); ties keep the lowest slot
                best = 0;
                std::uint32_t top = 0;
                for (int i = 0; i < numVoices; ++i)
                    if (top < slots[i].started)
                        top = slots[i].started, best = i;
            }
            slot = best;
            voices[slot].amp.stage = voices[slot].filt.stage = 4;
        }
        auto& s = slots[slot];
        s = Slot {};
        s.used = true;
        s.id = id;
        s.pan = pan, s.vol = vol, s.pitch = s.notePitch = pitch, s.fcut = fcut, s.fres = fres;
        hostLR (pan, gain * vol, s.l, s.r);
        s.ramp = 0;
        noteOnVoice (voices[slot], (int) ((std::uint32_t) pitchAsFloat (pitch) + 0x960u), (int) (vol * 127.0f), samples, fcut, fres);
        s.lastRes = fres;
        s.released = 0;
        s.started = g.blocks;
        return slot;
    }

    void release (int slot)
    {
        if (slot < 0 || ! slots[slot].used)
            return;
        slots[slot].released = g.blocks;
        voices[slot].amp.release();
        voices[slot].filt.release();
    }

    void kill (int slot)
    {
        if (slot < 0 || ! slots[slot].used)
            return;
        voices[slot].amp.stage = voices[slot].filt.stage = 4;
        slots[slot].used = false;
    }

    // FUN_1800050c0: one voice, n samples into `out` (§3.7)
    void renderVoice (Voice& v, float* out, int n)
    {
        if (v.amp.stage == 4)
            return;
        auto sm = v.smooth;
        auto* o1 = v.osc[0] ? &*v.osc[0] : nullptr;
        auto* o2 = v.osc[1] ? &*v.osc[1] : nullptr;
        auto* o3 = v.osc[2] ? &*v.osc[2] : nullptr;
        const bool ring = v.ring && o1 != nullptr && o2 != nullptr;
        const bool l1 = o1 && v.lfoPitch[0] != 0.0f, e1 = o1 && v.envPitch[0] != 0.0f;
        const bool l2 = o2 && v.lfoPitch[1] != 0.0f, e2 = o2 && v.envPitch[1] != 0.0f;
        const bool l3 = o3 && v.lfoPitch[2] != 0.0f, e3 = o3 && v.envPitch[2] != 0.0f;
        auto c = v.counter;
        float lfoA = 0.0f, lfoB = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const auto env = v.amp.tick (true);
            const auto fe = v.filt.tick (false);
            const auto fenv = v.filterEnvOwn ? fe : env;
            if (v.lfoOn == 1)
            {
                v.lfo.tick (g);
                lfoA = v.lfo.out1;
                lfoB = v.lfo.out2;
            }
            float s = 0.0f;
            if ((c & 0x1f) == 0)
            {
                auto pitchOf = [&] (Osc* o, bool la, bool ea, int k)
                {
                    auto p = v.pitchOffset;
                    if (la)
                        p = p + lfoB * v.lfoPitch[k];
                    if (ea)
                        p = p + env * v.envPitch[k];
                    o->setPitch ((int) p, g);
                };
                if (o1)
                {
                    pitchOf (o1, l1, e1, 0);
                    auto x = o1->render (d, g) * o1->level;
                    o1->out = x;
                    s = x + 0.0f;
                }
                if (o2)
                {
                    pitchOf (o2, l2, e2, 1);
                    auto x = o2->render (d, g) * o2->level;
                    o2->out = x;
                    s = ring ? s * (x + x) : s + x;
                }
                if (o3)
                {
                    pitchOf (o3, l3, e3, 2);
                    auto x = o3->render (d, g) * o3->level;
                    o3->out = x;
                    s = s + x;
                }
                if (v.filterOn)
                {
                    auto f = v.cutoffParam;
                    if (v.envAmtOn)
                        f = f + fenv * v.envAmt;
                    if (v.lfoAmtOn)
                        f = f + lfoA * v.lfoAmt;
                    v.filter.setCutoff (f, g.rateRatio);
                    s = v.filter.process (s);
                }
            }
            else
            {
                if (o1)
                {
                    auto x = o1->render (d, g) * o1->level;
                    o1->out = x;
                    s = x + 0.0f;
                }
                if (o2)
                {
                    auto x = o2->render (d, g) * o2->level;
                    o2->out = x;
                    s = ring ? s * (x + x) : s + x;
                }
                if (o3)
                {
                    auto x = o3->render (d, g) * o3->level;
                    o3->out = x;
                    s = s + x;
                }
                if (v.filterOn)
                    s = v.filter.process (s);
            }
            sm = sm + (env * env - sm) * 0.0299999993f;
            if (sm < 5.96046519e-08f && -5.96046519e-08f < sm)
                sm = 0.0f;
            out[i] = s * sm;
            ++c;
        }
        v.smooth = sm;
        v.counter = c;
    }

    // Gen_Render (§2.5) as the front-end calls it: the engine writes into a zeroed scratch buffer and reports how many
    // samples it wrote; only those are added to outL/outR
    int render (float* dstL, float* dstR, int n, std::vector<int>* finished)
    {
        if ((int) mixL.size() < n)
        {
            mixL.resize ((std::size_t) n);
            mixR.resize ((std::size_t) n);
            mono.resize ((std::size_t) n);
            outL.resize ((std::size_t) n);
            outR.resize ((std::size_t) n);
        }
        std::fill (outL.begin(), outL.begin() + n, 0.0f);
        std::fill (outR.begin(), outR.begin() + n, 0.0f);
        const auto written = engineRender (outL.data(), outR.data(), n, finished);
        for (int j = 0; j < written; ++j)
        {
            dstL[j] += outL[(std::size_t) j];
            dstR[j] += outR[(std::size_t) j];
        }
        return written;
    }

    int engineRender (float* oL, float* oR, int n, std::vector<int>* finished)
    {
        bool any = false;
        for (int i = 0; i < numVoices; ++i)
        {
            auto& s = slots[i];
            auto& v = voices[i];
            if (! s.used || v.amp.stage == 4)
                continue;
            if (! any)
            {
                std::fill (mixL.begin(), mixL.begin() + n, 0.0f);
                std::fill (mixR.begin(), mixR.begin() + n, 0.0f);
                any = true;
            }
            if (s.ramp == 0)
            {
                float l, r;
                hostLR (s.pan, s.vol * gain, l, r);
                if (l != s.l || r != s.r)
                {
                    s.ramp = 0x80;
                    s.dl = (l - s.l) * 0.0078125f;
                    s.dr = (r - s.r) * 0.0078125f;
                }
            }
            auto l = s.l, r = s.r;
            auto ramp = s.ramp;
            voicePitch (v, s.pitch + 0x960);
            {
                auto f = (float) pitchBase + (float) (s.pitch + 0x960);
                if (! (100.0f <= f))
                    f = 100.0f;
                v.cutoffParam = (f / 12700.0f) * d.keyTrack + s.fcut;
            }
            if (s.fres != s.lastRes)
            {
                s.lastRes = s.fres;
                auto x = (int) v.filter.res + (int) (s.fres * 255.0f);
                x = x < 0 ? 0 : (x > 0xff ? 0xff : x);
                v.filter.setRes (x);
            }
            renderVoice (v, mono.data(), n);
            if (ramp == 0)
                for (int j = 0; j < n; ++j)
                {
                    mixL[(std::size_t) j] = l * mono[(std::size_t) j] + mixL[(std::size_t) j];
                    mixR[(std::size_t) j] = r * mono[(std::size_t) j] + mixR[(std::size_t) j];
                }
            else
                for (int j = 0; j < n; ++j)
                {
                    const auto x = mono[(std::size_t) j];
                    if (ramp != 0)
                    {
                        l = l + s.dl;
                        r = r + s.dr;
                        --ramp;
                    }
                    mixL[(std::size_t) j] = x * l + mixL[(std::size_t) j];
                    mixR[(std::size_t) j] = x * r + mixR[(std::size_t) j];
                }
            s.l = l;
            s.ramp = ramp;
            s.r = r;
        }
        int written = n;
        if (any)
        {
            if (chorusOn == 0)
            {
                for (int j = 0; j < n; ++j)
                    oL[j] = mixL[(std::size_t) j] + oL[j], oR[j] = mixR[(std::size_t) j] + oR[j];
                for (int j = 0; j < n && tail != 0; ++j)
                {
                    oL[j] = chorusTap (0.0f) + oL[j];
                    if (--tail == 0)
                        break;
                }
            }
            else
            {
                for (int j = 0; j < n; ++j)
                {
                    oL[j] = chorusTap (mixL[(std::size_t) j]) + oL[j];
                    oR[j] = mixR[(std::size_t) j] + oR[j];
                }
                tail = tailLen;
            }
        }
        else if (chorusOn == 0 || tail == 0)
            written = 0;
        else
        {
            int left = n;
            for (int j = 0; left != 0; ++j)
            {
                oL[j] = chorusTap (0.0f) + oL[j];
                if (--tail == 0)
                    break;
                --left;
            }
            written = n - left;
        }
        for (int i = 0; i < numVoices; ++i)
            if (slots[i].used && voices[i].amp.stage == 4 && finished != nullptr)
                finished->push_back (i);
        samples += (std::uint32_t) written;
        ++g.blocks;
        return written;
    }

    void debugDump (int i) const
    {
        const auto& v = voices[i];
        for (int k = 0; k < 3; ++k)
            if (v.osc[k])
                std::fprintf (stderr, "osc%d kind %d lvl %.9g inc %d ph %d inc2 %d ph2 %d uni %d det %d cur %u base %u\n", k, v.osc[k]->kind,
                              (double) v.osc[k]->level, (int) v.osc[k]->inc, (int) v.osc[k]->ph, (int) v.osc[k]->inc2, (int) v.osc[k]->ph2,
                              v.osc[k]->unison, v.osc[k]->detune, v.osc[k]->cur, v.osc[k]->base);
        std::fprintf (stderr, "fil %.9g fbase %.9g mix %.17g %.17g %.17g damp %.17g q %.17g famt %.9g %.9g %.9g flags %d %d on %d f12 %.17g %.17g\n",
                      (double) v.cutoffParam, (double) v.filter.base, v.filter.mixLow, v.filter.mixBand, v.filter.mixHigh, v.filter.damp,
                      v.filter.q, (double) v.envAmt2, (double) v.envAmt, (double) v.lfoAmt, v.envAmtOn, v.lfoAmtOn, v.filterOn, v.filter.f1, v.filter.f2);
        auto env = [] (const char* n, const Env& e)
        {
            std::fprintf (stderr, "%s stage %u count %d step %.9g counts %d %d %d %d %d steps %.9g %.9g %.9g %.9g %.9g targets %.9g %.9g %.9g %.9g %.9g lvl %.9g\n", n,
                          e.stage, e.count, (double) e.step, e.counts[0], e.counts[1], e.counts[2], e.counts[3], e.counts[4], (double) e.steps[0],
                          (double) e.steps[1], (double) e.steps[2], (double) e.steps[3], (double) e.steps[4], (double) e.targets[0], (double) e.targets[1],
                          (double) e.targets[2], (double) e.targets[3], (double) e.targets[4], (double) e.level);
        };
        env ("amp", v.amp);
        env ("filt", v.filt);
        std::fprintf (stderr, "own %d ring %d lfoOn %d pitch %d gain %.9g chorus %d\n", v.filterEnvOwn, (int) v.ring, v.lfoOn, v.pitch, (double) gain, chorusOn);
    }

    float chorusTap (float in) noexcept
    {
        line[wpos] = in;
        wpos = (wpos + 1) & 0xffff;
        const auto p = rpos;
        const auto i = (int) p;
        const auto a = line[(std::size_t) (i & 0xffff)];
        const auto b = line[(std::size_t) (std::uint16_t) ((std::int16_t) i + 1)];
        auto np = speed + p;
        rpos = np;
        if (65536.0 <= np)
            rpos = np - 65536.0;
        const auto st = speedStep;
        const auto ns = st + speed;
        speed = ns;
        if (ns <= 0.9928 || 1.0072 <= ns)
        {
            speedStep = -st;
            if (speedMin <= ns)
            {
                if (speedMax < ns)
                    speed = speedMax;
            }
            else
                speed = speedMin;
        }
        return (b - a) * ((float) p - (float) i) + a;
    }
};

// ------------------------------------------------------------------------------------------------ state (§1.1)
struct State
{
    double p[numP] {};
    int s[numS] {};
    int version = 0;
};

std::optional<State> parseState (const std::vector<std::uint8_t>& b)
{
    if (b.size() < (std::size_t) stateBytes)
        return std::nullopt;
    State st;
    std::memcpy (st.p, b.data(), sizeof st.p);
    std::memcpy (st.s, b.data() + 0x168 + 19 * 4, sizeof st.s);
    std::memcpy (&st.version, b.data() + 0x168 + 19 * 4 + 0x4c + 12 * 4, 4);
    for (auto x : st.p)
        if (! std::isfinite (x))
            return std::nullopt;
    return st;
}

// --------------------------------------------------------------------------------------------- LYNX front-end
class SimSynthInstrument final : public Instrument
{
public:
    SimSynthInstrument()
    {
        eng = build (std::nullopt, 44100.0);
        for (auto& h : held)
            h = -1;
    }
    ~SimSynthInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    // a plugin as FL creates it: created, the state restored, block size and sample rate set (§2.3)
    static std::unique_ptr<Engine> build (const std::optional<State>& st, double sr)
    {
        auto e = std::make_unique<Engine>();
        e->create();
        if (st)
            e->load (st->p, st->s, st->version);
        e->setRate (sr);
        return e;
    }

    void prepare (double sr) noexcept override
    {
        delete incoming.exchange (nullptr);
        rate.store (sr);
        eng = build (current, sr);
        for (auto& h : held)
            h = -1;
    }

    void configure (const ChannelDef& channel, double) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        if (channel.pluginState == state)
            return;
        state = channel.pluginState;
        current = parseState (state);
        // a new state restarts the plugin (its voices go silent, as when FL restores a state); built here, swapped in by
        // the audio thread
        delete incoming.exchange (build (current, rate.load()).release());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float) noexcept override
    {
        take();
        const auto k = std::clamp (key, 0, 127);
        if (held[k] >= 0)
            eng->release (held[k]);
        notePan[k] = nextPan;
        const auto pan = std::clamp (nextPan + (float) channelPan, -1.0f, 1.0f);
        const auto pitch = (float) (100.0 * (k - 60)) + pitchCents.load() + nextCents;
        // FL hands SimSynth the old voice record: integer pan (−64…64) and pitch (cents)
        const auto sl = eng->trigger (k, (int) std::lround (pan * 64.0f), (float) channelVol, (int) std::lround (pitch), 0.0f, 0.0f);
        for (auto& h : held)
            if (h == sl)
                h = -1;
        held[k] = sl;
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    // a slide note (issue #14): the held keys' voices glide their Final.Pitch to the slide's pitch
    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        const auto to = (float) (100.0 * (std::clamp (key, 0, 127) - 60)) + pitchCents.load() + cents;
        for (const auto h : held)
            if (h >= 0 && eng->slots[h].used)
                eng->slots[h].slide.start (to - (float) eng->slots[h].notePitch, samples);
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (key >= 0 && key < 128 && held[key] >= 0)
        {
            eng->release (held[key]);
            held[key] = -1;
        }
    }

    void allNotesOff() noexcept override
    {
        for (int k = 0; k < 128; ++k)
            noteOff (k);
    }

    bool idle() const noexcept override
    {
        for (const auto& s : eng->slots)
            if (s.used)
                return false;
        return eng->tail == 0 && incoming.load() == nullptr;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        // FL's channel volume/pan reach SimSynth only through Final.Vol/Final.Pan: recover them from LYNX's gains
        // (√2·V·cos((1 + p)π/4), √2·V·sin(…)) so they are not applied twice
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (auto& s : eng->slots)
                if (s.used && s.id >= 0 && s.id < 128)
                    s.vol = (float) vol, s.pan = (int) std::lround (std::clamp (notePan[s.id] + (float) pan, -1.0f, 1.0f) * 64.0f);
        }
        constexpr int chunk = 512;
        for (int c0 = 0; c0 < n; c0 += chunk)
        {
            const auto cn = std::min (chunk, n - c0);
            finished.clear();
            eng->render (left + c0, right + c0, cn, &finished);
            for (auto& s : eng->slots) // (issue #14) a slide moves the voices' pitch chunk by chunk
                if (s.used && s.slide.moving())
                    s.pitch = s.notePitch + (int) std::lround (s.slide.advance ((float) cn));
            for (auto i : finished)
            {
                for (auto& h : held)
                    if (h == i)
                        h = -1;
                eng->kill (i);
            }
        }
    }

private:
    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* e = incoming.exchange (nullptr))
        {
            retired.store (eng.release());
            eng.reset (e);
            for (auto& h : held)
                h = -1;
        }
    }

    std::unique_ptr<Engine> eng;
    std::atomic<Engine*> incoming { nullptr }, retired { nullptr };
    std::optional<State> current;
    std::atomic<double> rate { 44100.0 };
    std::atomic<float> pitchCents { 0.0f };
    std::vector<std::uint8_t> state;
    double channelVol = 1.0, channelPan = 0.0;
    int held[128] {};
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
    std::vector<int> finished = std::vector<int> (numVoices);
};
} // namespace

bool simSynthStateUsable (const std::vector<std::uint8_t>& state) { return parseState (state).has_value(); }

std::vector<int> simSynthHostParams (const std::vector<std::uint8_t>& state)
{
    const auto st = parseState (state);
    if (! st)
        return {};
    auto e = std::make_unique<Engine>();
    e->create();
    e->load (st->p, st->s, st->version);
    std::vector<int> out;
    for (const auto& h : hostParams)
    {
        if (h.kind == 1)
            out.push_back ((int) std::floor ((double) h.scale * e->P[h.index]));
        else if (h.kind == 0)
            out.push_back (e->S[h.index]);
        else
        {
            const auto x = e->P[h.index];
            out.push_back (x == 0.0 ? 2 : x == 0.4 ? 3 : x == 0.2 ? 0 : x == 0.3 ? 4 : 1);
        }
    }
    return out;
}

std::unique_ptr<Instrument> makeSimSynthInstrument() { return std::make_unique<SimSynthInstrument>(); }

namespace simsynth
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, int block, std::vector<std::int64_t>* counters)
{
    const auto st = parseState (state);
    if (! st || length <= 0)
        return {};
    auto e = std::make_unique<Engine>();
    e->create();
    const auto c0 = (std::int64_t) e->g.ridx;
    e->load (st->p, st->s, st->version);
    const auto c1 = (std::int64_t) e->g.ridx;
    e->setRate (sampleRate);
    if (counters != nullptr)
        *counters = { (std::int64_t) e->g.ridx, (std::int64_t) e->g.chorusTurn, c0, c1 };
    block = std::max (1, block);
    std::vector<int> evs, slotOf (notes.size(), -1);
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r ((std::size_t) block);
    std::vector<int> fin;
    int n0 = 0;
    while (n0 < length)
    {
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            const auto& nt = notes[i];
            if (nt.start == n0)
            {
                const auto sl = e->trigger ((int) i, nt.finalPan, nt.finalVol, 100 * nt.key - 6000 + nt.pitchCents, nt.fcut, nt.fres);
                for (auto& o : slotOf)
                    if (o == sl)
                        o = -1; // a stolen voice: the host forgets the old note (Voice_Kill without the handle)
                slotOf[i] = sl;
            }
            if (nt.release == n0 && slotOf[i] >= 0)
                e->release (slotOf[i]);
        }
        auto ln = std::min (block, length - n0);
        for (auto ev : evs)
            if (n0 < ev && ev < n0 + ln)
                ln = ev - n0;
        std::fill (l.begin(), l.begin() + ln, 0.0f);
        std::fill (r.begin(), r.begin() + ln, 0.0f);
        fin.clear();
        e->render (l.data(), r.data(), ln, &fin);
        if (n0 == 0 && std::getenv ("SIMSYNTH_DEBUG") != nullptr)
            e->debugDump (0);
        for (auto i : fin)
        {
            e->kill (i);
            for (auto& s : slotOf)
                if (s == i)
                    s = -1;
        }
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace simsynth
} // namespace lynx
