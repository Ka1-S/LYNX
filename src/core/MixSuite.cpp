#include "core/MixSuite.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "core/Biquad.h"
#include "core/DynamicsSuite.h"
#include "core/Effects.h"
#include "core/EmberClassic.h"
#include "core/LrCrossover.h"
#include "core/ParametricEqLinear.h"
#include "core/VoiceMatch.h"
#include "core/VoxStrip.h"

// REA_rodada96. Every formula below is textbook material, cited where it matters:
//  - biquads: R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients";
//  - half-band FIR oversampling with a Kaiser window: Oppenheim & Schafer, "Discrete-Time Signal Processing", 7.6
//    and 4.7 (multirate), Kaiser's design formula for beta;
//  - compressor / gate gain computer with a soft knee and the "smooth branching" peak detector in the log domain:
//    D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor Design — A Tutorial and Analysis",
//    JAES 60(6), 2012;
//  - the electro-optical cell (an electroluminescent panel lighting a photoresistor in a feedback leveler): the
//    published behaviour of such cells — about 10 ms attack, a release in two stages (roughly half the reduction
//    within ~60 ms, the rest over 0.5 to several seconds, longer after long or heavy reduction) — as described in
//    the public service literature and circuit analyses of opto levelers; the model (a fast and a slow conductance
//    and a memory that lengthens the slow release) is our own;
//  - waveshaping and its harmonics: U. Zölzer (ed.), "DAFX: Digital Audio Effects", 2nd ed., ch. 4 (nonlinear
//    processing: tube / tape / distortion curves, exciters and enhancers), and J. Pakarinen, D. T. Yeh, "A Review of
//    Digital Techniques for Modeling Vacuum-Tube Guitar Amplifiers", CMJ 33(2), 2009 (asymmetric curves → even
//    harmonics).

namespace lynx::mixsuite
{
namespace
{
constexpr double pi = 3.141592653589793;

inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g) noexcept { return 20.0 * std::log10 (std::max (g, 1e-12)); }
inline double logMap (float v, double lo, double hi) noexcept { return lo * std::pow (hi / lo, (double) std::clamp (v, 0.0f, 1.0f)); }
inline double lin (float v, double lo, double hi) noexcept { return lo + (hi - lo) * (double) std::clamp (v, 0.0f, 1.0f); }
inline bool on (float v) noexcept { return v >= 0.5f; }
// one-pole smoothing coefficient for a time constant in seconds
inline double coef (double seconds, double sr) noexcept { return seconds <= 0.0 ? 0.0 : std::exp (-1.0 / (seconds * sr)); }

// |H(e^jw)| of a biquad
double biquadMag (const Biquad& b, double w)
{
    const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
    const double nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = -(b.b1 * s1 + b.b2 * s2);
    const double dr = 1.0 + b.a1 * c1 + b.a2 * c2, di = -(b.a1 * s1 + b.a2 * s2);
    return std::sqrt ((nr * nr + ni * ni) / std::max (dr * dr + di * di, 1e-300));
}

// first-order high-pass (bilinear), stereo
struct OnePoleHp
{
    double b0 = 1, b1 = 0, a1 = 0;
    double x1[2] {}, y1[2] {};
    void set (double sr, double hz) noexcept
    {
        const auto k = std::tan (pi * std::min (hz, sr * 0.45) / sr);
        b0 = 1.0 / (1.0 + k);
        b1 = -b0;
        a1 = (k - 1.0) / (k + 1.0);
    }
    void reset() noexcept { x1[0] = x1[1] = y1[0] = y1[1] = 0.0; }
    float process (float x, int ch) noexcept
    {
        const auto y = b0 * x + b1 * x1[ch] - a1 * y1[ch];
        x1[ch] = x;
        y1[ch] = y;
        return (float) y;
    }
    double mag (double w) const
    {
        const double nr = b0 + b1 * std::cos (w), ni = -b1 * std::sin (w);
        const double dr = 1.0 + a1 * std::cos (w), di = -a1 * std::sin (w);
        return std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// Stereo delay line of fixed capacity (no allocation).
template <int N>
struct Delay2
{
    std::array<float, N> l {}, r {};
    int pos = 0, len = 0;
    void setLength (int n) noexcept { len = std::clamp (n, 0, N - 1); }
    void reset() noexcept { l.fill (0.0f); r.fill (0.0f); pos = 0; }
    void process (float& a, float& b) noexcept
    {
        if (len == 0)
            return;
        l[(std::size_t) pos] = a;
        r[(std::size_t) pos] = b;
        auto rd = pos - len;
        if (rd < 0)
            rd += N;
        a = l[(std::size_t) rd];
        b = r[(std::size_t) rd];
        if (++pos == N)
            pos = 0;
    }
};

struct Meters
{
    std::atomic<float> gr0 { 0.0f }, gr1 { 0.0f }, in { 0.0f }, out { 0.0f };
    float read (int k) const noexcept
    {
        switch (k)
        {
            case 0: return gr0.load (std::memory_order_relaxed);
            case 1: return gr1.load (std::memory_order_relaxed);
            case 2: return in.load (std::memory_order_relaxed);
            case 3: return out.load (std::memory_order_relaxed);
            default: return 0.0f;
        }
    }
    static float peak (const float* l, const float* r, int n) noexcept
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i)
            p = std::max (p, std::max (std::abs (l[i]), std::abs (r[i])));
        return std::min (p, 1.0f);
    }
    void set (float grDb0, float grDb1, float inPeak, float outPeak) noexcept
    {
        gr0.store (std::clamp (grDb0 / 30.0f, 0.0f, 1.0f), std::memory_order_relaxed);
        gr1.store (std::clamp (grDb1 / 30.0f, 0.0f, 1.0f), std::memory_order_relaxed);
        in.store (inPeak, std::memory_order_relaxed);
        out.store (outPeak, std::memory_order_relaxed);
    }
    // round 147: a level for the second meter slot when an engine has no second reduction (Hush: the detector's
    // energy, −60 … 0 dB as 0 … 1)
    void setDetector (float db) noexcept { gr1.store (std::clamp ((db + 60.0f) / 60.0f, 0.0f, 1.0f), std::memory_order_relaxed); }
};

// ---- oversampler design ----
constexpr int stageTaps[Oversampler::maxStages] = { 95, 23, 15 }; // N = 4k + 3: the centre index c = (N − 1) / 2 is odd
constexpr double stageBeta[Oversampler::maxStages] = { 10.0, 9.0, 8.5 };

double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

// latency of a factor's path in units of the highest rate, before the pad
int rawUnits (int stages)
{
    const int f = 1 << stages;
    int u = 0;
    for (int s = 0; s < stages; ++s)
        u += ((stageTaps[s] - 1) / 2) * f / (1 << s);
    return u;
}

int pathLatency (int stages)
{
    const int f = 1 << stages;
    const int u = rawUnits (stages);
    return (u + (f - u % f) % f) / f;
}
} // namespace

// ================================================================== Oversampler

Oversampler::Oversampler()
{
    for (int s = 0; s < maxStages; ++s)
    {
        auto& stage = st[(std::size_t) s];
        const int n = stageTaps[s];
        const int c = (n - 1) / 2;
        const auto i0b = besselI0 (stageBeta[s]);
        double sum = 0.0;
        std::array<double, 64> h {};

        for (int j = 0; 2 * j < n; ++j)
        {
            const int t = 2 * j - c; // odd offset from the centre
            const auto r = (double) (2 * j - c) / c;
            const auto w = besselI0 (stageBeta[s] * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            h[(std::size_t) j] = std::sin (0.5 * pi * t) / (0.5 * pi * t) * w; // sinc(t/2), scaled below
            sum += h[(std::size_t) j];
        }

        stage.taps = (n + 1) / 2;
        for (int j = 0; j < stage.taps; ++j)
            stage.coef[(std::size_t) j] = (float) (h[(std::size_t) j] * 0.5 / sum); // the side taps sum to 0.5
        stage.centreDelay = (c - 1) / 2;
    }
    setFactor (4);
}

int Oversampler::latency() noexcept { return pathLatency (maxStages); }

void Oversampler::setFactor (int factor) noexcept
{
    const int s = factor >= 8 ? 3 : (factor >= 4 ? 2 : 1);
    if (s == stages && (hiPadLen != 0 || loPadLen != 0 || s == maxStages))
        return;
    stages = s;
    const int f = 1 << s;
    const int u = rawUnits (s);
    hiPadLen = (f - u % f) % f;
    loPadLen = latency() - pathLatency (s);
    reset();
}

void Oversampler::reset() noexcept
{
    for (auto& s : st)
    {
        s.upHist.fill (0.0f);
        s.evenHist.fill (0.0f);
        s.oddHist.fill (0.0f);
        s.upPos = s.downPos = 0;
    }
    hiPad.fill (0.0f);
    loPad.fill (0.0f);
    hiPadPos = loPadPos = 0;
}

void Oversampler::up (Stage& s, float x, float& a, float& b) noexcept
{
    const int L = s.taps;
    s.upPos = s.upPos == 0 ? L - 1 : s.upPos - 1;
    s.upHist[(std::size_t) s.upPos] = s.upHist[(std::size_t) (s.upPos + L)] = x;
    const float* hist = s.upHist.data() + s.upPos; // hist[j] = x[m − j]
    float acc = 0.0f;
    for (int j = 0; j < L; ++j)
        acc += s.coef[(std::size_t) j] * hist[j];
    a = 2.0f * acc;              // y[2m]
    b = hist[s.centreDelay];     // y[2m + 1] = x[m − (c − 1) / 2]
}

float Oversampler::down (Stage& s, float a, float b) noexcept
{
    const int L = s.taps;
    s.downPos = s.downPos == 0 ? L - 1 : s.downPos - 1;
    s.evenHist[(std::size_t) s.downPos] = s.evenHist[(std::size_t) (s.downPos + L)] = a;
    s.oddHist[(std::size_t) s.downPos] = s.oddHist[(std::size_t) (s.downPos + L)] = b;
    const float* even = s.evenHist.data() + s.downPos;
    float acc = 0.0f;
    for (int j = 0; j < L; ++j)
        acc += s.coef[(std::size_t) j] * even[j];
    return acc + 0.5f * s.oddHist[(std::size_t) (s.downPos + s.centreDelay + 1)]; // v[2m − c]
}

float Oversampler::padIn (float x) noexcept
{
    if (hiPadLen == 0)
        return x;
    const auto y = hiPad[(std::size_t) hiPadPos];
    hiPad[(std::size_t) hiPadPos] = x;
    hiPadPos = (hiPadPos + 1) % hiPadLen;
    return y;
}

float Oversampler::padOut (float x) noexcept
{
    if (loPadLen == 0)
        return x;
    const auto y = loPad[(std::size_t) loPadPos];
    loPad[(std::size_t) loPadPos] = x;
    loPadPos = (loPadPos + 1) % loPadLen;
    return y;
}

// ================================================================== Ridge Strip

namespace ridge
{
double hpfHz (float v) noexcept { return v <= 0.0f ? 0.0 : logMap (v, 16.0, 350.0); }
double lpfHz (float v) noexcept { return v <= 0.0f ? 0.0 : logMap (v, 22000.0, 3000.0); }
double bandGainDb (float v, bool black) noexcept { return (2.0 * std::clamp (v, 0.0f, 1.0f) - 1.0) * (black ? 18.0 : 15.0); }
double lfHz (float v) noexcept { return logMap (v, 30.0, 450.0); }
double lmfHz (float v) noexcept { return logMap (v, 200.0, 2500.0); }
double hmfHz (float v) noexcept { return logMap (v, 600.0, 7000.0); }
double hfHz (float v) noexcept { return logMap (v, 1500.0, 16000.0); }
double bandQ (float v) noexcept { return logMap (v, 0.5, 2.5); }
double ratio (float v) noexcept { return v >= 0.999f ? 1e9 : 1.0 + 19.0 * std::pow ((double) std::clamp (v, 0.0f, 1.0f), 2.0); }
double compThresholdDb (float v) noexcept { return lin (v, -40.0, 0.0); }
double releaseSeconds (float v) noexcept { return logMap (v, 0.1, 4.0); }
double gateRangeDb (float v) noexcept { return lin (v, 0.0, 60.0); }
double gateThresholdDb (float v) noexcept { return lin (v, -80.0, 0.0); }
double trimDb (float v) noexcept { return lin (v, -20.0, 20.0); }
double eQ (float v) noexcept { return vmatch::echannel::qFromDial (1.0 - (double) std::clamp (v, 0.0f, 1.0f)); }
double eCompThresholdDb (float v) noexcept { return lin (v, -20.0, 10.0); }
double eGateRangeDb (float v) noexcept { return lin (v, 0.0, 40.0); }
double eGateThresholdDb (float v) noexcept { return lin (v, -30.0, 10.0); }
bool eVoicing (const EffectParams& p) noexcept { return on (p[voicing]); }
bool vocalVoicing (const EffectParams& p) noexcept { return on (p[vocal]); }
bool k4Voicing (const EffectParams& p) noexcept { return on (p[console4k]); }
double k4Q (float v) noexcept { return logMap (v, 0.5, 3.0); }
float k4QValue (double q) noexcept { return (float) std::clamp (std::log (std::clamp (q, 0.5, 3.0) / 0.5) / std::log (6.0), 0.0, 1.0); }
int k4ColourIndex (float v) noexcept { return std::clamp ((int) std::lround (v * 2.0f), 0, 2); }
float k4ColourValue (int colour) noexcept { return (float) std::clamp (colour, 0, 2) * 0.5f; }
double k4MicDb (float v) noexcept { return lin (v, 0.0, 50.0); }
double k4OffsetDb (float v) noexcept { return lin (v, -12.0, 12.0); }
double autoMakeupDb (const EffectParams& p) noexcept
{
    if (p[compRatio] <= 0.0005f || ! on (p[dynOn]))
        return 0.0;
    vmatch::echannel::Settings e;
    e.compRatio = ratio (p[compRatio]);
    e.compThreshold = std::clamp (compThresholdDb (p[compThreshold]) + 18.0, -20.0, 10.0);
    return vmatch::echannel::compMakeupDb (e);
}

// round 145: the Vocal strip voicing's settings (the reference's own laws, core/VoxStrip.h)
voxstrip::Settings vocalSettings (const EffectParams& p) noexcept
{
    namespace vs = voxstrip;
    vs::Settings s;
    s.inputDb = vs::gainDb (p[vInput]);
    s.outputDb = vs::gainDb (p[vOutput]);
    s.order = vs::orderOf (p[vOrder]);
    s.dsOn = on (p[vDsOn]); s.dsSplit = on (p[vDsSplit]); s.dsLookahead = on (p[vDsLookahead]); s.dsListen = on (p[vDsListen]);
    s.dsHz = vs::dsFreqHz (p[vDsFreq]); s.dsThresholdDb = vs::thresholdDb (p[vDsThreshold]);
    s.dpOn = on (p[vDpOn]); s.dpSplit = on (p[vDpSplit]); s.dpLookahead = on (p[vDpLookahead]); s.dpListen = on (p[vDpListen]);
    s.dpHz = vs::dpFreqHz (p[vDpFreq]); s.dpThresholdDb = vs::thresholdDb (p[vDpThreshold]);
    s.eqOn = on (p[vEqOn]); s.hpfOn = on (p[vHpfOn]); s.notchOn = on (p[vNotchOn]); s.highOn = on (p[vHighOn]);
    s.hpfHz = vs::hpfHz (p[vHpfFreq]); s.notchHz = vs::notchHz (p[vNotchFreq]); s.notchDb = vs::notchGainDb (p[vNotchGain]);
    s.highHz = vs::highHz (p[vHighFreq]); s.highDb = vs::highGainDb (p[vHighGain]);
    s.compOn = on (p[vCompOn]); s.softKnee = on (p[vKnee]); s.drive = on (p[vDrive]);
    s.expThresholdDb = vs::expThresholdDb (p[vExpThreshold]); s.compThresholdDb = vs::thresholdDb (p[vCompThreshold]);
    s.ratio = vs::ratio (p[vRatio]); s.attackMs = vs::attackMs (p[vAttack]); s.releaseMs = vs::releaseMs (p[vRelease]);
    s.makeupDb = vs::makeupDb (p[vMakeup]);
    return s;
}


namespace
{
constexpr double kneeDb = 6.0;

// Giannoulis et al. 2012, eq. (4): soft-knee gain computer, output level for input level x (dB)
double gainComputer (double x, double threshold, double r)
{
    const auto over = x - threshold;
    if (2.0 * over < -kneeDb)
        return x;
    if (2.0 * std::abs (over) <= kneeDb)
        return x + (1.0 / r - 1.0) * (over + kneeDb / 2.0) * (over + kneeDb / 2.0) / (2.0 * kneeDb);
    return threshold + over / r;
}

// the EQ section's filters (shared by the processor and responseDb)
struct EqDesign
{
    OnePoleHp hp1;
    Biquad hp2, lp, lf, lmf, hmf, hf;
    bool hpOn = false, lpOn = false;

    void design (const EffectParams& p, double sr)
    {
        const bool black = on (p[curve]);
        hpOn = p[hpf] > 0.0f;
        lpOn = p[lpf] > 0.0f;
        if (hpOn)
        {
            // 3rd-order Butterworth high-pass: a first-order section and a second-order one with Q = 1
            hp1.set (sr, hpfHz (p[hpf]));
            hp2.filter (sr, 1, hpfHz (p[hpf]), 1.0);
        }
        if (lpOn)
            lp.filter (sr, 0, std::min (lpfHz (p[lpf]), sr * 0.45), 0.70710678);

        const auto nyq = sr * 0.49;
        // shelves: "brown" curves are plain (S = 1, no overshoot); "black" ones have the resonant shelf with the small
        // dip / bump before the shelf that many console EQs show (S > 1)
        const auto shelfS = black ? 1.6 : 1.0;
        const auto lg = bandGainDb (p[lfGain], black), hg = bandGainDb (p[hfGain], black);
        if (on (p[lfBell]))
            lf.peaking (sr, lfHz (p[lfFreq]), black ? propQ (0.9, lg) : 0.9, lg);
        else
            lf.shelf (sr, lfHz (p[lfFreq]), lg, false, shelfS);
        if (on (p[hfBell]))
            hf.peaking (sr, std::min (hfHz (p[hfFreq]), nyq), black ? propQ (0.9, hg) : 0.9, hg);
        else
            hf.shelf (sr, std::min (hfHz (p[hfFreq]), nyq), hg, true, shelfS);

        const auto mg = bandGainDb (p[lmfGain], black), ng = bandGainDb (p[hmfGain], black);
        lmf.peaking (sr, lmfHz (p[lmfFreq]), black ? propQ (bandQ (p[lmfQ]), mg) : bandQ (p[lmfQ]), mg);
        hmf.peaking (sr, std::min (hmfHz (p[hmfFreq]), nyq), black ? propQ (bandQ (p[hmfQ]), ng) : bandQ (p[hmfQ]), ng);
    }

    // "black" bells: proportional Q — wide for small boosts / cuts, narrower as the gain grows
    static double propQ (double q, double gainDb) { return std::max (0.2, q * (0.5 + std::abs (gainDb) / 12.0)); }
};
} // namespace

// E voicing: Ridge's controls in the console's units (the shared ranges use Ridge's own laws)
vmatch::echannel::Settings consoleSettingsOf (const EffectParams& p) noexcept
{
    vmatch::echannel::Settings e;
    e.inputDb = trimDb (p[input]);
    e.outputDb = trimDb (p[output]);
    e.phase = on (p[phase]);
    e.hpHz = p[hpf] > 0.0f ? hpfHz (p[hpf]) : 0.0;
    e.lpKHz = p[lpf] > 0.0f ? lpfHz (p[lpf]) / 1000.0 : 0.0;
    e.lfGain = bandGainDb (p[lfGain], false);
    e.lfHz = lfHz (p[lfFreq]);
    e.lfBell = on (p[lfBell]);
    e.lmfGain = bandGainDb (p[lmfGain], false);
    e.lmfKHz = lmfHz (p[lmfFreq]) / 1000.0;
    e.lmfQ = eQ (p[lmfQ]);
    e.hmfGain = bandGainDb (p[hmfGain], false);
    e.hmfKHz = hmfHz (p[hmfFreq]) / 1000.0;
    e.hmfQ = eQ (p[hmfQ]);
    e.hfGain = bandGainDb (p[hfGain], false);
    e.hfKHz = hfHz (p[hfFreq]) / 1000.0;
    e.hfBell = on (p[hfBell]);
    e.eqOn = on (p[eqOn]);
    e.compRatio = p[compRatio] <= 0.0005f ? 1.0 : ratio (p[compRatio]);
    e.compThreshold = eCompThresholdDb (p[compThreshold]);
    e.compRelease = releaseSeconds (p[ridge::compRelease]);
    e.compFast = on (p[compFastAttack]);
    e.expRange = eGateRangeDb (p[gateRange]);
    e.expThreshold = eGateThresholdDb (p[gateThreshold]);
    e.expRelease = releaseSeconds (p[ridge::gateRelease]);
    e.expFast = on (p[gateFastAttack]);
    e.gate = ! on (p[expander]);
    e.dynOn = on (p[dynOn]);
    e.dynamicsAfterEq = ! on (p[order]);
    e.filtersBeforeDynamics = on (p[filterSplit]);
    e.eqToSidechain = on (p[eqToSidechain]); // round 165
    if (k4Voicing (p)) // round 148: the 4K console (its own Q scale, colour, mic pre, VCA, make-up, mix)
    {
        e.model = 1;
        e.colour = k4ColourIndex (p[k4Colour]);
        e.lmfQ = k4Q (p[lmfQ]);
        e.hmfQ = k4Q (p[hmfQ]);
        e.lpKHz = e.lpKHz >= 15.9 ? 0.0 : e.lpKHz;
        e.filtersBeforeDynamics = true;
        e.pre = on (p[k4Pre]);
        e.micDb = k4MicDb (p[k4Mic]);
        e.vca = on (p[analog]);
        e.autoMakeup = on (p[autoMakeup]);
        e.makeupOffsetDb = k4OffsetDb (p[k4MakeupOffset]);
        e.compMix = std::clamp ((double) p[compMix], 0.0, 1.0);
    }
    return e;
}


double responseDb (const EffectParams& p, double sampleRate, double hz)
{
    if (k4Voicing (p) || eVoicing (p)) // round 148: the console's own curve
        return vmatch::echannel::responseDb (consoleSettingsOf (p), sampleRate, hz);
    EqDesign d;
    d.design (p, sampleRate);
    const auto w = 2.0 * pi * hz / sampleRate;
    double m = 1.0;
    if (d.hpOn)
        m *= d.hp1.mag (w) * biquadMag (d.hp2, w);
    if (d.lpOn)
        m *= biquadMag (d.lp, w);
    if (on (p[eqOn]))
        m *= biquadMag (d.lf, w) * biquadMag (d.lmf, w) * biquadMag (d.hmf, w) * biquadMag (d.hf, w);
    return gainToDb (m);
}

double compressorCurveDb (const EffectParams& p, double inputDb)
{
    return gainComputer (inputDb, compThresholdDb (p[compThreshold]), ratio (p[compRatio]));
}

namespace
{
class Strip final : public Effect
{
public:
    void prepare (double rate) noexcept override
    {
        sr = rate;
        console.prepare (rate);
        vocalEngine.prepare (rate);
        reset();
    }

    // round 145: only the Vocal strip voicing's look-ahead adds latency (known from the last block's settings)
    int latencySamples() const noexcept override { return vocalLatency; }

    void reset() noexcept override
    {
        console.reset();
        vocalEngine.reset();
        for (auto* d : { &audio, &side })
        {
            d->hp1.reset(); d->hp2.reset(); d->lp.reset(); d->lf.reset(); d->lmf.reset(); d->hmf.reset(); d->hf.reset();
        }
        compGr = 0.0;
        gateGr = 0.0;
        levelEnv = 0.0;
        gateOpen = false;
    }

    float controlOutput (int k) const noexcept override { return meters.read (k); }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        if (vocalVoicing (p))
        {
            const auto inPeak = Meters::peak (l, r, n);
            vocalEngine.set (vocalSettings (p));
            vocalLatency = vocalEngine.latencySamples();
            vocalEngine.process (l, r, n);
            meters.set (vocalEngine.compReductionDb(), std::max (vocalEngine.deEssReductionDb(), vocalEngine.dePlosReductionDb()),
                        inPeak, Meters::peak (l, r, n));
            return;
        }
        vocalLatency = 0;
        if (k4Voicing (p)) // round 148
        {
            const auto inPeak = Meters::peak (l, r, n);
            console.process (l, r, n, consoleSettingsOf (p));
            meters.set (console.compReductionDb(), console.gateReductionDb(), inPeak, Meters::peak (l, r, n));
            return;
        }
        if (eVoicing (p))
        {
            const auto inPeak = Meters::peak (l, r, n);
            console.process (l, r, n, consoleSettingsOf (p));
            if (on (p[analog]))
                addAnalogNoise (l, r, n);
            meters.set (console.compReductionDb(), console.gateReductionDb(), inPeak, Meters::peak (l, r, n));
            return;
        }
        audio.design (p, sr);
        side.design (p, sr);
        const auto inPeak = Meters::peak (l, r, n);
        const auto inGain = (float) dbToGain (trimDb (p[input]));
        // round 147b: + the console's automatic make-up when switched on
        const auto outGain = (float) (dbToGain (trimDb (p[output]) + (on (p[autoMakeup]) ? autoMakeupDb (p) : 0.0)) * (on (p[phase]) ? -1.0 : 1.0));
        const bool eq = on (p[eqOn]), dyn = on (p[dynOn]), eqFirst = ! on (p[order]), fts = on (p[filtersToSidechain]);
        const bool eqSc = on (p[eqToSidechain]); // round 165: the filters and the EQ in the side chain only

        // compressor
        const auto thr = compThresholdDb (p[compThreshold]), rat = ratio (p[compRatio]);
        const auto compAttack = coef (on (p[compFastAttack]) ? 0.001 : 0.010, sr);
        const auto compRel = coef (releaseSeconds (p[ridge::compRelease]), sr);
        const auto cmix = (float) std::clamp (p[compMix], 0.0f, 1.0f);
        const bool compOn = p[compRatio] > 0.0005f;
        // gate / expander
        const auto range = gateRangeDb (p[gateRange]), gthr = gateThresholdDb (p[gateThreshold]);
        const bool gateOn = range > 0.05, expand = on (p[expander]);
        const auto gateAttack = coef (on (p[gateFastAttack]) ? 0.0001 : 0.0015, sr);
        const auto gateRel = coef (releaseSeconds (p[ridge::gateRelease]), sr);
        const auto levelRelease = coef (0.030, sr);
        float maxComp = 0.0f, maxGate = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            float a = l[i] * inGain, b = r[i] * inGain;
            if (! fts && ! eqSc)
                filters (audio, a, b);
            if (eq && eqFirst && ! eqSc)
                bands (audio, a, b);

            if (dyn && (compOn || gateOn))
            {
                float sa = a, sb = b;
                if (fts || eqSc)
                    filters (side, sa, sb);
                if (eqSc && eq)
                    bands (side, sa, sb);
                const auto rect = (double) std::max (std::abs (sa), std::abs (sb));
                // level detector: instant attack, 30 ms release (keeps the ripple of low notes out of the gain)
                levelEnv = rect > levelEnv ? rect : levelRelease * levelEnv + (1.0 - levelRelease) * rect;
                const auto peak = levelEnv;

                // gate / expander first (as in console channel dynamics), then the compressor
                if (gateOn)
                {
                    const auto lvl = gainToDb (peak);
                    double target;
                    if (expand) // 1:2 downward expansion below the threshold, limited to the range
                        target = lvl < gthr ? std::min (range, (gthr - lvl)) : 0.0;
                    else
                    {
                        // hysteresis: opens at the threshold, closes 4 dB below it
                        if (lvl >= gthr)
                            gateOpen = true;
                        else if (lvl < gthr - 4.0)
                            gateOpen = false;
                        target = gateOpen ? 0.0 : range;
                    }
                    gateGr = target < gateGr ? gateAttack * gateGr + (1.0 - gateAttack) * target
                                             : gateRel * gateGr + (1.0 - gateRel) * target;
                }
                else
                    gateGr = 0.0;

                if (compOn)
                {
                    const auto x = gainToDb (peak) - gateGr;
                    const auto want = x - gainComputer (x, thr, rat);
                    compGr = want > compGr ? compAttack * compGr + (1.0 - compAttack) * want
                                           : compRel * compGr + (1.0 - compRel) * want;
                }
                else
                    compGr = 0.0;

                const auto g = (float) dbToGain (-gateGr);
                const auto c = (float) dbToGain (-compGr);
                const auto wet = g * (cmix * c + (1.0f - cmix));
                a *= wet;
                b *= wet;
                maxComp = std::max (maxComp, (float) compGr);
                maxGate = std::max (maxGate, (float) gateGr);
            }

            if (eq && ! eqFirst && ! eqSc)
                bands (audio, a, b);
            l[i] = a * outGain;
            r[i] = b * outGain;
        }
        if (on (p[analog]))
            addAnalogNoise (l, r, n);
        meters.set (maxComp, maxGate, inPeak, Meters::peak (l, r, n));
    }

private:
    // round 147b: ANALOG — [EXEC] the reference console with Analog on adds, at its very output (whatever the input,
    // output or compressor settings), white noise of −110.7 dBFS RMS per channel, the two channels uncorrelated; its
    // harmonics are unchanged (H2 … H5 of a 1 kHz / −20 dBFS and a 100 Hz / −6 dBFS sine within 0.4 dB). Gaussian
    // (Irwin–Hall, four uniforms) from a small xorshift generator, one per channel.
    void addAnalogNoise (float* l, float* r, int n) noexcept
    {
        constexpr float sigma = 2.91e-6f; // 10^(−110.73 / 20)
        const auto gauss = [] (std::uint32_t& st) noexcept {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                st ^= st << 13; st ^= st >> 17; st ^= st << 5;
                sum += (float) (st >> 8) * (1.0f / 16777216.0f);
            }
            return (sum - 2.0f) * 1.7320508f; // variance 4/12 → 1
        };
        for (int i = 0; i < n; ++i)
        {
            l[i] += sigma * gauss (noiseL);
            r[i] += sigma * gauss (noiseR);
        }
    }
    std::uint32_t noiseL = 0x9e3779b9u, noiseR = 0x85ebca6bu;


    vmatch::echannel::Channel console;
    voxstrip::Engine vocalEngine;
    int vocalLatency = 0;

    static void filters (EqDesign& d, float& a, float& b) noexcept
    {
        if (d.hpOn)
        {
            a = d.hp2.process (d.hp1.process (a, 0), 0);
            b = d.hp2.process (d.hp1.process (b, 1), 1);
        }
        if (d.lpOn)
        {
            a = d.lp.process (a, 0);
            b = d.lp.process (b, 1);
        }
    }

    static void bands (EqDesign& d, float& a, float& b) noexcept
    {
        a = d.hf.process (d.hmf.process (d.lmf.process (d.lf.process (a, 0), 0), 0), 0);
        b = d.hf.process (d.hmf.process (d.lmf.process (d.lf.process (b, 1), 1), 1), 1);
    }

    double sr = 44100.0;
    EqDesign audio, side;
    double compGr = 0.0, gateGr = 0.0, levelEnv = 0.0;
    bool gateOpen = false;
    Meters meters;
};
} // namespace
} // namespace ridge

// ================================================================== Glow Leveler

namespace glow
{
double gainDb (float v) noexcept { return lin (v, 0.0, 30.0); }
double sidechainDb (float v) noexcept { return lin (v, -20.0, 40.0); }
bool classic (const EffectParams& p) noexcept { return on (p[voicing]); }
double trimDb (float v) noexcept { return lin (v, -18.0, 18.0); }
std::vector<std::pair<int, float>> linkedEdit (const EffectParams& p, int param, float value)
{
    std::vector<std::pair<int, float>> edits { { param, value } };
    if (param != autoMakeup || ! classic (p) || ! on (p[optoLaw]) || on (value) == on (p[autoMakeup]))
        return edits;
    const double pr = 100.0 * std::clamp ((double) p[peakReduction], 0.0, 1.0);
    if (on (value)) // switched on: the dial as it is now is the reference
        edits.push_back ({ makeupRef, p[peakReduction] });
    else // switched off: the trial keeps the make-up (its Gain knob moved); LYNX moves the Gain dial by as much
    {
        const double ref = 100.0 * std::clamp ((double) p[makeupRef], 0.0, 1.0);
        const double db = vmatch::opto::makeupDb (100.0 * p[gain]) + vmatch::opto::autoMakeupDb (pr) - vmatch::opto::autoMakeupDb (ref);
        edits.push_back ({ gain, (float) (vmatch::opto::gainForMakeupDb (db) / 100.0) });
    }
    return edits;
}
int analogHz (float v) noexcept { return v < 0.25f ? 0 : v < 0.75f ? 50 : 60; }

namespace
{
class Leveler final : public Effect
{
public:
    void prepare (double rate) noexcept override
    {
        sr = rate;
        panelAttack = coef (0.001, sr);
        panelRelease = coef (0.030, sr);
        fastAttack = coef (0.010, sr);
        fastRelease = coef (0.050, sr);
        slowAttack = coef (0.400, sr);
        memoryUp = coef (2.0, sr);
        memoryDown = coef (8.0, sr);
        autoCoef = coef (0.5, sr);
        classicEngine.prepare (rate);
        reset();
    }

    void reset() noexcept override
    {
        emph.reset();
        classicEngine.reset();
        panel = cellFast = cellSlow = memory = autoGr = 0.0;
        lastL = lastR = 0.0f;
    }

    float controlOutput (int k) const noexcept override { return meters.read (k); }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        const auto inPeak = Meters::peak (l, r, n);
        if (classic (p))
        {
            vmatch::opto::Settings c;
            c.peakReduction = 100.0 * std::clamp ((double) p[peakReduction], 0.0, 1.0);
            c.gain = 100.0 * std::clamp ((double) p[gain], 0.0, 1.0);
            c.hiFreq = 100.0 * std::clamp ((double) p[emphasis], 0.0, 1.0);
            c.limit = on (p[mode]);
            c.measured = on (p[optoLaw]);
            c.autoMakeup = on (p[autoMakeup]);
            c.measuredRelease = on (p[releaseLaw]);
            c.makeupRef = 100.0 * std::clamp ((double) p[makeupRef], 0.0, 1.0);
            const auto mx = std::clamp (p[mix], 0.0f, 1.0f);
            const int hz = analogHz (p[analog]);
            const auto trimGain = (float) dbToGain (trimDb (p[trim]));
            for (int off = 0; off < n; off += maxBlock)
            {
                const int m = std::min (maxBlock, n - off);
                std::copy (l + off, l + off + m, dryL.begin());
                std::copy (r + off, r + off + m, dryR.begin());
                classicEngine.process (l + off, r + off, m, c);
                if (hz > 0)
                    addAnalog (l + off, r + off, m, hz);
                for (int i = 0; i < m; ++i)
                {
                    l[off + i] = (l[off + i] * mx + dryL[(std::size_t) i] * (1.0f - mx)) * trimGain;
                    r[off + i] = (r[off + i] * mx + dryR[(std::size_t) i] * (1.0f - mx)) * trimGain;
                }
            }
            meters.set (classicEngine.reductionDb(), 0.0f, inPeak, Meters::peak (l, r, n));
            return;
        }
        const auto sc = p[peakReduction] <= 0.0f ? 0.0 : dbToGain (sidechainDb (p[peakReduction]));
        const auto gamma = on (p[mode]) ? 9.0 : 2.0; // asymptotic ratio 1 + gamma: about 3:1 (compress), 10:1 (limit)
        emph.shelf (sr, 2500.0, 10.0 * std::clamp ((double) p[emphasis], 0.0, 1.0), true, 0.7);
        const auto makeup = (float) dbToGain (gainDb (p[gain]));
        const auto mx = std::clamp (p[mix], 0.0f, 1.0f);
        const auto trimGain = (float) dbToGain (trimDb (p[trim]));
        const auto autoUp = on (p[autoMakeup]);
        const int hz = analogHz (p[analog]);
        double maxGr = 0.0;

        for (int i = 0; i < n; ++i)
        {
            // feedback detection: the cell sees the leveler's own output (the previous sample)
            const auto s = std::max (std::abs (emph.process (lastL, 0)), std::abs (emph.process (lastR, 1)));
            panel = s > panel ? panelAttack * panel + (1.0 - panelAttack) * s : panelRelease * panel + (1.0 - panelRelease) * s;
            const auto light = std::pow (sc * panel, gamma);

            // the cell: a fast conductance (attack ~10 ms, release ~50 ms) and a slow one whose release lengthens
            // with the memory of past reduction (0.4 s fresh, up to 5 s after long, heavy leveling)
            cellFast = light > cellFast ? fastAttack * cellFast + (1.0 - fastAttack) * light
                                        : fastRelease * cellFast + (1.0 - fastRelease) * light;
            const auto slowRelease = coef (0.4 + 4.6 * memory, sr);
            cellSlow = light > cellSlow ? slowAttack * cellSlow + (1.0 - slowAttack) * light
                                        : slowRelease * cellSlow + (1.0 - slowRelease) * light;
            const auto busy = light > 0.1 ? 1.0 : 0.0;
            memory = busy > memory ? memoryUp * memory + (1.0 - memoryUp) * busy : memoryDown * memory + (1.0 - memoryDown) * busy;

            const auto conductance = 0.7 * cellFast + 0.3 * cellSlow;
            const auto g = (float) (1.0 / (1.0 + conductance));
            lastL = l[i] * g;
            lastR = r[i] * g;
            // auto make-up (round 129) [model]: gives back ~2/3 of the slow average reduction, so turning PEAK REDUCTION
            // up keeps the level roughly where it was (the reference's AUTO MAKEUP; its law is not measured)
            auto wet = makeup;
            if (autoUp)
            {
                const auto grDb = -gainToDb (g);
                autoGr = autoCoef * autoGr + (1.0 - autoCoef) * grDb;
                wet *= (float) dbToGain (0.66 * autoGr);
            }
            auto wl = lastL * wet, wr = lastR * wet;
            if (hz > 0)
                addAnalog (&wl, &wr, 1, hz);
            l[i] = (wl * mx + l[i] * (1.0f - mx)) * trimGain;
            r[i] = (wr * mx + r[i] * (1.0f - mx)) * trimGain;
            maxGr = std::max (maxGr, -gainToDb (g));
        }
        meters.set ((float) maxGr, 0.0f, inPeak, Meters::peak (l, r, n));
    }

private:
    // round 152: ANALOG [EXEC] (REA_rodada152a §2) — the reference adds, in its wet path after the make-up (Trim moves it,
    // Mix 0 % removes it, Gain and Peak reduction do not), pink-like noise of −89.5 dBFS RMS per channel (the two
    // uncorrelated, flat per third octave within 3 dB) and mains hum on both channels alike: harmonics of 50 or 60 Hz,
    // the 2nd strongest (−98.7 dBFS peak), as measured on silence. Pink: Paul Kellet's economy filter over Gaussian
    // (Irwin–Hall) noise from a small xorshift per channel.
    void addAnalog (float* l, float* r, int n, int hz) noexcept
    {
        static constexpr double hum50[12] { -117.2, -98.7, -118.2, -105.2, -122.5, -120.6, -126.6, -126.4, -127.7, -123.8, -125.5, -125.5 };
        static constexpr double hum60[12] { -117.8, -98.5, -118.7, -104.9, -121.8, -120.1, -121.8, -123.1, -128.7, -121.4, -127.7, -132.1 };
        constexpr float sigma = 1.1322e-5f; // 10^(−89.5 / 20) / the filter's RMS gain (2.9586)
        if (humHz != hz)
        {
            humHz = hz;
            for (int k = 0; k < 12; ++k)
                humAmp[k] = dbToGain ((hz == 50 ? hum50 : hum60)[k]);
        }
        const auto gauss = [] (std::uint32_t& st) noexcept {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                st ^= st << 13; st ^= st >> 17; st ^= st << 5;
                sum += (float) (st >> 8) * (1.0f / 16777216.0f);
            }
            return (sum - 2.0f) * 1.7320508f;
        };
        const double dphi = (double) hz / sr;
        for (int i = 0; i < n; ++i)
        {
            float* ch[2] { &l[i], &r[i] };
            for (int c = 0; c < 2; ++c)
            {
                const auto w = gauss (seed[c]);
                auto* b = pink[c];
                b[0] = 0.99765f * b[0] + w * 0.0990460f;
                b[1] = 0.96300f * b[1] + w * 0.2965164f;
                b[2] = 0.57000f * b[2] + w * 1.0526913f;
                *ch[c] += sigma * (b[0] + b[1] + b[2] + w * 0.1848f);
            }
            double hum = 0.0;
            for (int k = 0; k < 12; ++k)
                hum += humAmp[k] * std::sin (2.0 * pi * (double) (k + 1) * humPhase);
            humPhase += dphi;
            if (humPhase >= 1.0) humPhase -= 1.0;
            l[i] += (float) hum;
            r[i] += (float) hum;
        }
    }
    std::uint32_t seed[2] { 0x9e3779b9u, 0x85ebca6bu };
    float pink[2][3] {};
    double humPhase = 0.0, humAmp[12] {};
    int humHz = 0;

    static constexpr int maxBlock = 1024;
    double sr = 44100.0;
    vmatch::opto::Leveler classicEngine;
    std::array<float, maxBlock> dryL {}, dryR {};
    Biquad emph;
    double panel = 0, cellFast = 0, cellSlow = 0, memory = 0;
    double panelAttack = 0, panelRelease = 0, fastAttack = 0, fastRelease = 0, slowAttack = 0, memoryUp = 0, memoryDown = 0;
    double autoGr = 0, autoCoef = 0;
    float lastL = 0, lastR = 0;
    Meters meters;
};
} // namespace
} // namespace glow

// ================================================================== Hush

namespace hush
{
double hz (float v) noexcept { return logMap (v, 2000.0, 16000.0); }
double lowPassHz (float v) noexcept { return logMap (v, 2000.0, 20000.0); }
double thresholdDb (float v) noexcept { return lin (v, -60.0, 0.0); }
double rangeDb (float v) noexcept { return lin (v, 0.0, 24.0); }
double releaseMs (float v) noexcept { return logMap (v, 10.0, 250.0); }
double lookaheadMs() noexcept { return 2.0; }
double lookaheadTimeMs (float v) noexcept { return std::round (lin (v, 0.0, 15.0) * 100.0) / 100.0; } // (0.01 ms steps: the default is exactly 2 ms)
float lookaheadTimeValue (double ms) noexcept { return (float) std::clamp (ms / 15.0, 0.0, 1.0); }
double classicThresholdDb (float v) noexcept { return lin (v, -80.0, 0.0); }
double panValue (float v) noexcept { return lin (v, -1.0, 1.0); }
// round 165x [EXEC] (REA_rodada165x §2, the trial's value texts at 1000 steps): the reference's Input / Output Level reads
// 72 dB per unit above 0.2 (0.5 = 0 dB, 1 = +36 dB) and 40 log10 (v) + 6.36 dB below (−21.6 dB at 0.2, −∞ at 0); its
// high-pass 2000 · 10^v Hz (2 … 20 kHz)
double levelDb (float v, bool ext) noexcept
{
    if (! ext)
        return lin (v, -12.0, 12.0);
    const double x = std::clamp ((double) v, 0.0, 1.0);
    return x >= 0.2 ? 72.0 * x - 36.0 : x <= 0.0 ? -200.0 : 40.0 * std::log10 (x) + 6.3588;
}
float levelValue (double db, bool ext) noexcept
{
    if (! ext)
        return (float) std::clamp ((db + 12.0) / 24.0, 0.0, 1.0);
    if (db >= -21.6)
        return (float) std::clamp ((db + 36.0) / 72.0, 0.0, 1.0);
    return db <= -199.0 ? 0.0f : (float) std::clamp (std::pow (10.0, (db - 6.3588) / 40.0), 0.0, 0.2);
}
double hpHz (float v, bool ext) noexcept { return ext ? 2000.0 * std::pow (10.0, std::clamp ((double) v, 0.0, 1.0)) : hz (v); }
float hpValue (double f, bool ext) noexcept
{
    const double x = std::max (f, 1.0) / 2000.0;
    return (float) std::clamp (ext ? std::log10 (x) : std::log (x) / std::log (8.0), 0.0, 1.0);
}
bool extended (const EffectParams& p) noexcept { return on (p[rangeLaw]) && ! on (p[voicing]); }
// round 165x [EXEC] (REA_rodada165x §2; L / R test tones, sibilance in one channel, both in phase, in anti-phase):
// the reference's Stereo Link k (0 … 1 = 0 … 100 %) detects each channel on (1 − k/2) · own + k/2 · other — 100 % is
// the mid signal, so side-only sibilance does not trigger; above 100 % a share m of the output is processed on the mid
// signal only (Mid-only: the side passes) with the same gain. Side-only (the same test material, 125 / 150 / 200 %):
// each channel detects on (1 − m) · mid ∓ m · side (L −, R +) and processes (1 − m) · itself ± m · side (L +, R −).
struct ProLink
{
    double k = 1.0, m = 0.0;
    bool side = false;
    bool twoLanes() const noexcept { return (m <= 0.0 && k < 1.0) || (m > 0.0 && side); }
    // the detector's input(s) for the channels' samples a / b
    void lanes (double a, double b, double& d0, double& d1) const noexcept
    {
        if (m <= 0.0 && k < 1.0) { d0 = (1.0 - 0.5 * k) * a + 0.5 * k * b; d1 = 0.5 * k * a + (1.0 - 0.5 * k) * b; }
        else if (! side) d0 = d1 = 0.5 * (a + b);
        else { const double mid = 0.5 * (a + b), sd = 0.5 * (a - b); d0 = (1.0 - m) * mid - m * sd; d1 = (1.0 - m) * mid + m * sd; }
    }
    // the change to add to the channels given their band signals ba / bb and the lanes' gains
    void delta (double g0, double g1, double ba, double bb, double& oa, double& ob) const noexcept
    {
        if (m <= 0.0) { oa = (g0 - 1.0) * ba; ob = ((k < 1.0 ? g1 : g0) - 1.0) * bb; return; }
        if (! side)
        {
            const double mid = 0.5 * (ba + bb);
            oa = (g0 - 1.0) * ((1.0 - m) * ba + m * mid);
            ob = (g0 - 1.0) * ((1.0 - m) * bb + m * mid);
            return;
        }
        const double sd = 0.5 * (ba - bb);
        oa = (g0 - 1.0) * ((1.0 - m) * ba + m * sd);
        ob = (g1 - 1.0) * ((1.0 - m) * bb - m * sd);
    }
};
static ProLink proLinkOf (const EffectParams& p) noexcept
{
    ProLink l;
    l.k = std::clamp ((double) p[stereoLink], 0.0, 1.0);
    l.m = l.k >= 1.0 ? std::clamp ((double) p[linkOnly], 0.0, 1.0) : 0.0;
    l.side = on (p[linkSide]);
    return l;
}
// round 165w [EXEC] (REA_rodada165w §5): the reference's input / output pan is a balance — the far channel × (1 − |pan|),
// the near one untouched (both 1 kHz / 1.5 kHz test tones, every quarter step)
static void balance (float* l, float* r, int n, double pan) noexcept
{
    if (pan == 0.0)
        return;
    const auto g = (float) (1.0 - std::min (1.0, std::abs (pan)));
    auto* far = pan > 0.0 ? l : r;
    for (int i = 0; i < n; ++i)
        far[i] *= g;
}
bool classic (const EffectParams& p) noexcept { return on (p[voicing]); }
bool vocal (const EffectParams& p) noexcept { return on (p[vocalVoicing]) && ! on (p[voicing]); }

namespace
{
class DeEsser final : public Effect
{
public:
    explicit DeEsser (const EffectParams* p = nullptr)
        : primedMs (p != nullptr ? lookaheadTimeMs ((*p)[lookaheadTime]) : lookaheadMs()), vocalOn (p != nullptr && vocal (*p)) {}

    void prepare (double rate) noexcept override
    {
        sr = rate;
        setDelay (primedMs);
        classicEngine.prepare (rate);
        vocalEngine.prepare (rate);
        reset();
    }

    void reset() noexcept override
    {
        delay.reset();
        scDelay.reset();
        classicEngine.reset();
        vocalEngine.reset();
        detect.reset();
        detectLp.reset();
        listenF.reset();
        listenLp.reset();
        split.reset();
        bandLp.reset();
        bandLp2.reset();
        topHp.reset();
        topHp2.reset();
        env = 0.0;
        gr = 0.0;
        envCh[0] = envCh[1] = grCh[0] = grCh[1] = 0.0;
        fullEnv = 0.0;
        for (auto& o : os) o.reset();
        for (auto& o : osBand) o.reset();
        for (auto& o : osGain) o.reset();
    }

    int latencySamples() const noexcept override
    {
        if (vocalOn)
            return vocalEngine.latencySamples();
        if (classicOn) // round 165 [EXEC]: the reference reports 0 samples, so the classic voicing has no latency
            return 0;
        return delay.len + (osFactor > 1 ? Oversampler::latency() : 0);
    }
    float controlOutput (int k) const noexcept override { return meters.read (k); }
    void setSidechain (const float* l, const float* r) noexcept override { scL = l; scR = r; }

    void process (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept override
    {
        if (n > maxBlock) // the oversampled path works on blocks of at most maxBlock samples
        {
            const auto* sl = scL;
            const auto* sr2 = scR;
            for (int i = 0; i < n; i += maxBlock)
            {
                if (sl != nullptr && sr2 != nullptr) { scL = sl + i; scR = sr2 + i; }
                processBlock (l + i, r + i, std::min (maxBlock, n - i), p, bpm);
            }
            scL = scR = nullptr;
            return;
        }
        processBlock (l, r, n, p, bpm);
    }

private:
    void processBlock (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept
    {
        processCore (l, r, n, p, bpm);
        balance (l, r, n, panValue (p[outputPan])); // round 165w: after the output level, every voicing
    }

    void processCore (float* l, float* r, int n, const EffectParams& p, double) noexcept
    {
        // round 128: the look-ahead time is the delay (and the reported latency; the engine re-plans the delay
        // compensation when it moves). The default 2 ms is the old fixed value.
        if (const auto ms = lookaheadTimeMs (p[lookaheadTime]); std::abs (ms - delayMs) > 1e-9)
            setDelay (ms);
        const bool extLaw = extended (p); // round 165x: the reference's ranges and stereo link
        const auto inGainDb = classic (p) ? lin (p[input], -12.0, 12.0) : levelDb (p[input], extLaw);
        if (inGainDb != 0.0)
        {
            const auto gIn = (float) dbToGain (inGainDb);
            for (int i = 0; i < n; ++i) { l[i] *= gIn; r[i] *= gIn; }
        }
        balance (l, r, n, panValue (p[inputPan])); // round 165w: after the input level, before the detector
        const auto inPeak = Meters::peak (l, r, n);
        // round 135: oversampling (Hush's own voicing): the gain and the band it acts on are upsampled with the audio
        const auto factor = classic (p) ? 1 : oversamplingOf (p[oversampling]);
        if (factor != osFactor)
        {
            osFactor = factor;
            for (auto* bank : { &os, &osBand, &osGain })
                for (auto& o : *bank) { o.setFactor (std::max (2, factor)); o.reset(); }
        }
        const bool over = osFactor > 1;
        vocalOn = vocal (p);
        if (vocalOn) // round 148: the measured single-vocal de-esser
        {
            vmatch::vocalds::Settings v;
            v.thresholdDb = thresholdDb (p[threshold]);
            v.rangeDb = rangeDb (p[hush::range]);
            v.hpHz = hpHz (p[frequency], extLaw);
            v.lpHz = p[lowPass] < 0.999f ? lowPassHz (p[lowPass]) : 20000.0;
            if (extLaw)
            {
                const auto pl = proLinkOf (p);
                v.proLink = true;
                v.link = pl.k;
                v.linkOnly = pl.m;
                v.linkSide = pl.side;
            }
            v.lookahead = on (p[lookahead]);
            v.lookaheadMs = lookaheadTimeMs (p[lookaheadTime]);
            v.wideband = on (p[mode]);
            v.listen = on (p[listen]);
            vocalEngine.process (l, r, n, v);
            const auto out = (float) dbToGain (levelDb (p[output], extLaw));
            for (int i = 0; i < n; ++i) { l[i] *= out; r[i] *= out; }
            meters.set (vocalEngine.reductionDb(), 0.0f, inPeak, Meters::peak (l, r, n));
            meters.setDetector (vocalEngine.detectorDb());
            scL = scR = nullptr;
            return;
        }
        classicOn = classic (p);
        if (classicOn)
        {
            // the measured classic de-esser has no look-ahead and (round 165) no latency, as the reference
            vmatch::deesser::Settings c;
            c.hz = hz (p[frequency]);
            c.thresholdDb = classicThresholdDb (p[threshold]);
            c.bandPass = on (p[detector]);
            c.wideband = on (p[mode]);
            c.listen = on (p[listen]);
            c.outputDb = lin (p[output], -12.0, 12.0);
            classicEngine.process (l, r, n, c);
            meters.set (classicEngine.reductionDb(), 0.0f, inPeak, Meters::peak (l, r, n));
            meters.setDetector (classicEngine.detectorDb());
            return;
        }
        const auto f = std::min (hpHz (p[frequency], extLaw), sr * 0.45);
        const bool band = on (p[detector]);
        if (band)
            detect.filter (sr, 2, f, 1.4);
        else
            detect.filter (sr, 1, f, 0.70710678);
        listenF = detect;
        split.filter (sr, 0, f, 0.70710678);
        const bool lpOn = p[lowPass] < 0.999f;
        if (lpOn)
        {
            const auto lpHz = std::min (std::max (lowPassHz (p[lowPass]), f * 1.05), sr * 0.45);
            detectLp.filter (sr, 0, lpHz, 0.70710678);
            listenLp.filter (sr, 0, lpHz, 0.70710678);
            bandLp.filter (sr, 0, lpHz, 0.70710678); // Linkwitz-Riley 4th order at the low-pass: band = LP², above = HP²
            bandLp2 = bandLp;
            topHp.filter (sr, 1, lpHz, 0.70710678);
            topHp2 = topHp;
        }
        const auto thr = thresholdDb (p[threshold]), range = rangeDb (p[hush::range]);
        const bool wide = on (p[mode]), listening = on (p[listen]), ahead = on (p[lookahead]);
        const bool allroundMode = on (p[allround]), mid = on (p[linkMid]);
        const bool ext = on (p[external]) && scL != nullptr && scR != nullptr;
        const auto link = std::clamp ((double) p[stereoLink], 0.0, 1.0);
        const bool linked = link >= 1.0 || mid;
        const auto release = coef (releaseMs (p[hush::release]) * 0.001, sr);
        const auto attack = coef (ahead ? std::max (delayMs, 0.03) * 0.001 / 3.0 : 0.0005, sr);
        const auto out = (float) dbToGain (levelDb (p[output], extLaw));
        const auto pl = proLinkOf (p);
        const bool pro = extLaw; // round 165x: the reference's link (detection on the blended signals)
        constexpr double knee = 3.0;
        double maxGr = 0.0, maxDet = 0.0;

        // gain reduction (dB) wanted for a detected level `s` (and the full-band level in allround mode)
        auto wanted = [&] (double s, double full) {
            auto over = gainToDb (s) - thr;
            if (allroundMode) // the sibilance must also stand out from the whole signal (steady highs in a mix trigger less)
                over -= 0.5 * std::max (0.0, gainToDb (full) - gainToDb (s) - 6.0);
            double want = 0.0;
            if (over > knee)
                want = over;
            else if (over > -knee)
                want = (over + knee) * (over + knee) / (4.0 * knee);
            return std::min (want, range);
        };

        for (int i = 0; i < n; ++i)
        {
            float a = l[i], b = r[i];
            float da = a, db = b;
            delay.process (da, db);
            // with lookahead the detector hears the signal before it is played; without, both see the delayed one
            auto xa = ahead ? a : da, xb = ahead ? b : db;
            if (ext)
            {
                float sa = scL[i], sb = scR[i];
                float dsa = sa, dsb = sb;
                scDelay.process (dsa, dsb);
                xa = ahead ? sa : dsa;
                xb = ahead ? sb : dsb;
            }
            double sA, sB;
            if (pro)
            {
                double d0, d1;
                pl.lanes (xa, xb, d0, d1);
                auto ya = detect.process ((float) d0, 0), yb = detect.process ((float) d1, 1);
                if (lpOn) { ya = detectLp.process (ya, 0); yb = detectLp.process (yb, 1); }
                sA = std::abs (ya);
                sB = std::abs (yb);
            }
            else if (mid)
            {
                auto m = detect.process (0.5f * (xa + xb), 0);
                if (lpOn) m = detectLp.process (m, 0);
                sA = sB = std::abs (m);
            }
            else
            {
                auto ya = detect.process (xa, 0), yb = detect.process (xb, 1);
                if (lpOn) { ya = detectLp.process (ya, 0); yb = detectLp.process (yb, 1); }
                sA = std::abs (ya);
                sB = std::abs (yb);
            }
            double full = 0.0;
            if (allroundMode)
            {
                const double fa = std::max (std::abs (xa), std::abs (xb));
                fullEnv = fa > fullEnv ? fa : release * fullEnv + (1.0 - release) * fa;
                full = fullEnv;
            }
            float gA, gB;
            if (pro)
            {
                // each lane its own envelope and gain (one lane when both hear the same signal)
                const int lanes = pl.twoLanes() ? 2 : 1;
                const double ss[2] { sA, sB };
                for (int c = 0; c < lanes; ++c)
                {
                    envCh[c] = ss[c] > envCh[c] ? ss[c] : release * envCh[c] + (1.0 - release) * ss[c];
                    maxDet = std::max (maxDet, envCh[c]);
                    const auto want = wanted (envCh[c], full);
                    grCh[c] = want > grCh[c] ? attack * grCh[c] + (1.0 - attack) * want : release * grCh[c] + (1.0 - release) * want;
                    maxGr = std::max (maxGr, grCh[c]);
                }
                gA = (float) dbToGain (-grCh[0]);
                gB = (float) dbToGain (-grCh[lanes - 1]);
            }
            else if (linked)
            {
                const auto s = std::max (sA, sB);
                env = s > env ? s : release * env + (1.0 - release) * s;
                maxDet = std::max (maxDet, env);
                const auto want = wanted (env, full);
                gr = want > gr ? attack * gr + (1.0 - attack) * want : release * gr + (1.0 - release) * want;
                gA = gB = (float) dbToGain (-gr);
                maxGr = std::max (maxGr, gr);
            }
            else
            {
                const auto s = std::max (sA, sB);
                const double ss[2] { link * s + (1.0 - link) * sA, link * s + (1.0 - link) * sB };
                for (int c = 0; c < 2; ++c)
                {
                    envCh[c] = ss[c] > envCh[c] ? ss[c] : release * envCh[c] + (1.0 - release) * ss[c];
                    maxDet = std::max (maxDet, envCh[c]);
                    const auto want = wanted (envCh[c], full);
                    grCh[c] = want > grCh[c] ? attack * grCh[c] + (1.0 - attack) * want : release * grCh[c] + (1.0 - release) * want;
                    maxGr = std::max (maxGr, grCh[c]);
                }
                gA = (float) dbToGain (-grCh[0]);
                gB = (float) dbToGain (-grCh[1]);
            }

            if (listening)
            {
                a = listenF.process (da, 0);
                b = listenF.process (db, 1);
                if (lpOn) { a = listenLp.process (a, 0); b = listenLp.process (b, 1); }
            }
            else if (pro)
            {
                // round 165x: output = the pass-through + the link's change of the band (whole signal, above the
                // split, or between the two handles)
                double pa = da, pb = db, ba = da, bb = db;
                if (! wide)
                {
                    const auto la = split.process (da, 0), lb = split.process (db, 1);
                    ba = da - la;
                    bb = db - lb;
                    if (lpOn)
                    {
                        const auto ma = bandLp2.process (bandLp.process ((float) ba, 0), 0), mb = bandLp2.process (bandLp.process ((float) bb, 1), 1);
                        const auto ta = topHp2.process (topHp.process ((float) ba, 0), 0), tb = topHp2.process (topHp.process ((float) bb, 1), 1);
                        pa = la + ma + ta;
                        pb = lb + mb + tb;
                        ba = ma;
                        bb = mb;
                    }
                }
                double oa, ob;
                pl.delta (gA, gB, ba, bb, oa, ob);
                if (over)
                {
                    // the change goes to the high rate as a band with a unit gain (the gain moves at the base rate)
                    bandBuf[0][(std::size_t) i] = (float) oa;
                    bandBuf[1][(std::size_t) i] = (float) ob;
                    gainBuf[0][(std::size_t) i] = gainBuf[1][(std::size_t) i] = 1.0f;
                    a = (float) pa; // (the pass-through; the oversampler adds the change)
                    b = (float) pb;
                }
                else
                {
                    a = (float) (pa + oa);
                    b = (float) (pb + ob);
                }
            }
            else if (over)
            {
                // output = input + band × (gain − 1): the band (whole signal, above the split, or between the two
                // handles) and the gain go to the high rate below
                float ba = da, bb = db;
                if (! wide)
                {
                    const auto la = split.process (da, 0), lb = split.process (db, 1);
                    ba = da - la;
                    bb = db - lb;
                    if (lpOn)
                    {
                        ba = bandLp2.process (bandLp.process (ba, 0), 0);
                        bb = bandLp2.process (bandLp.process (bb, 1), 1);
                        topHp2.process (topHp.process (da - la, 0), 0); // (kept running: switching back stays smooth)
                        topHp2.process (topHp.process (db - lb, 1), 1);
                    }
                }
                bandBuf[0][(std::size_t) i] = ba;
                bandBuf[1][(std::size_t) i] = bb;
                gainBuf[0][(std::size_t) i] = gA - 1.0f;
                gainBuf[1][(std::size_t) i] = gB - 1.0f;
                a = da;
                b = db;
            }
            else if (wide)
            {
                a = da * gA;
                b = db * gB;
            }
            else if (! lpOn)
            {
                // complementary split: low = 2nd-order low-pass, high = input − low (sums back exactly)
                const auto la = split.process (da, 0), lb = split.process (db, 1);
                a = la + (da - la) * gA;
                b = lb + (db - lb) * gB;
            }
            else
            {
                // round 128: only the band between the two handles is turned down; above the low-pass it passes
                // (the high part is split again at the low-pass by a Linkwitz-Riley pair: flat sum, no notch)
                const auto la = split.process (da, 0), lb = split.process (db, 1);
                const auto ha = da - la, hb = db - lb;
                const auto ma = bandLp2.process (bandLp.process (ha, 0), 0), mb = bandLp2.process (bandLp.process (hb, 1), 1);
                const auto ta = topHp2.process (topHp.process (ha, 0), 0), tb = topHp2.process (topHp.process (hb, 1), 1);
                a = la + ma * gA + ta;
                b = lb + mb * gB + tb;
            }
            l[i] = a * out;
            r[i] = b * out;
        }
        if (over)
        {
            const auto m = n * osFactor;
            for (int c = 0; c < 2; ++c)
            {
                auto* x = c == 0 ? l : r;
                if (listening)
                {
                    os[(std::size_t) c].process (x, n, [] (float v) noexcept { return v; });
                    continue;
                }
                auto& ub = upBand[(std::size_t) c];
                auto& ug = upGain[(std::size_t) c];
                int k = 0, j = 0, q = 0;
                osBand[(std::size_t) c].process (bandBuf[(std::size_t) c].data(), n, [&] (float v) noexcept { ub[(std::size_t) std::min (k++, m - 1)] = v; return 0.0f; });
                osGain[(std::size_t) c].process (gainBuf[(std::size_t) c].data(), n, [&] (float v) noexcept { ug[(std::size_t) std::min (j++, m - 1)] = v; return 0.0f; });
                os[(std::size_t) c].process (x, n, [&] (float v) noexcept {
                    const auto at = (std::size_t) std::min (q++, m - 1);
                    return v + ub[at] * ug[at] * out;
                });
            }
        }
        meters.set ((float) maxGr, 0.0f, inPeak, Meters::peak (l, r, n));
        meters.setDetector ((float) gainToDb (maxDet)); // round 147: the energy the threshold is compared with
        scL = scR = nullptr;
    }

    void setDelay (double ms) noexcept
    {
        delayMs = ms;
        const auto len = (int) std::lround (ms * 0.001 * sr);
        delay.setLength (len);
        scDelay.setLength (len);
    }

    static constexpr int maxBlock = 1024, maxFactor = 8;
    int osFactor = 1;
    std::array<Oversampler, 2> os, osBand, osGain;
    std::array<std::array<float, maxBlock>, 2> bandBuf {}, gainBuf {};
    std::array<std::array<float, maxBlock * maxFactor>, 2> upBand {}, upGain {};
    double sr = 44100.0;
    double primedMs = 2.0, delayMs = 2.0;
    Delay2<4096> delay, scDelay;
    vmatch::deesser::DeEsser classicEngine;
    vmatch::vocalds::DeEsser vocalEngine;
    bool vocalOn = false;
    bool classicOn = false; // round 165: the classic voicing has no latency
    Biquad detect, detectLp, listenF, listenLp, split, bandLp, bandLp2, topHp, topHp2;
    double env = 0.0, gr = 0.0, fullEnv = 0.0;
    double envCh[2] {}, grCh[2] {};
    const float* scL = nullptr;
    const float* scR = nullptr;
    Meters meters;
};
} // namespace
} // namespace hush

// ================================================================== Aether

namespace aether
{
double airDb (float v) noexcept { return lin (v, 0.0, 15.0); }
double airHz (float v) noexcept { return logMap (v, 5000.0, 20000.0); }
double exciterHz (float v) noexcept { return logMap (v, 1000.0, 12000.0); }

namespace
{
// ---- the dynamic air model (round 124) ------------------------------------------------------------------------------
// Black-box measurements of the official Fresh Air trial (docs/analise/REA_rodada124.md §3), our own code:
//   y = s(mid, high) * x + kMid(mid) * gMid(t) * HPmid(x) + kHigh(high) * gHigh(t) * HPhigh(x)
// HPmid / HPhigh: RBJ 2nd-order high-passes (2993 Hz, Q 0.629 / 9000 Hz, Q 0.707; the same analog corners at 44.1
// and 48 kHz); s, kMid, kHigh: measured on a 0.025 grid (small-signal transfer functions, residual < 0.4 %).
// gMid: a linked peak detector (instant attack, 5 ms hold, 96 ms release) of the band, 4:1 above -30.1 dBFS,
// released through a 7.7 ms one-pole. gHigh: two 4:1 stages — a peak detector with 2-pole gain smoothing, then a
// feedback stage on the band after the first one, whose threshold rises as High air goes down (T + c (1 - high)^2).
namespace dyn
{
constexpr double gridMidS[41] { 1.00237, 1.02960, 1.04368, 1.05483, 1.06431, 1.07263, 1.08006, 1.08676, 1.09284, 1.09838, 1.10343,
    1.10804, 1.11224, 1.11606, 1.11952, 1.12264, 1.12542, 1.12788, 1.13004, 1.13189, 1.13345, 1.13472, 1.13571, 1.13642, 1.13685,
    1.13700, 1.13689, 1.13650, 1.13584, 1.13492, 1.13373, 1.13227, 1.13055, 1.12855, 1.12629, 1.12376, 1.12097, 1.11790, 1.11456,
    1.11095, 1.10707 };
constexpr double gridMidK[41] { 0.00000, 0.13781, 0.22533, 0.30473, 0.38070, 0.45506, 0.52874, 0.60230, 0.67608, 0.75035, 0.82529,
    0.90102, 0.97767, 1.05531, 1.13403, 1.21388, 1.29491, 1.37717, 1.46071, 1.54556, 1.63175, 1.71932, 1.80829, 1.89870, 1.99056,
    2.08391, 2.17877, 2.27515, 2.37309, 2.47260, 2.57371, 2.67643, 2.78079, 2.88681, 2.99451, 3.10390, 3.21501, 3.32786, 3.44246,
    3.55884, 3.67701 };
constexpr double gridHighS[41] { 1.00237, 1.03339, 1.04999, 1.06349, 1.07527, 1.08587, 1.09559, 1.10462, 1.11306, 1.12100, 1.12851,
    1.13562, 1.14238, 1.14881, 1.15493, 1.16077, 1.16634, 1.17166, 1.17673, 1.18156, 1.18616, 1.19055, 1.19472, 1.19867, 1.20242,
    1.20597, 1.20932, 1.21247, 1.21543, 1.21820, 1.22077, 1.22316, 1.22535, 1.22736, 1.22918, 1.23082, 1.23227, 1.23353, 1.23461,
    1.23550, 1.23620 };
constexpr double gridHighK[41] { 0.00000, 0.26995, 0.45609, 0.63358, 0.81042, 0.98969, 1.17298, 1.36128, 1.55524, 1.75537, 1.96206,
    2.17564, 2.39639, 2.62458, 2.86045, 3.10422, 3.35611, 3.61634, 3.88510, 4.16260, 4.44906, 4.74466, 5.04961, 5.36411, 5.68837,
    6.02260, 6.36699, 6.72176, 7.08712, 7.46329, 7.85048, 8.24890, 8.65879, 9.08036, 9.51384, 9.95946, 10.41747, 10.88807, 11.37154,
    11.86810, 12.37801 };
inline double grid (const double (&t)[41], double v) noexcept
{
    const auto x = std::clamp (v, 0.0, 1.0) * 40.0;
    const auto i = std::min (39, (int) x);
    return t[i] + (t[i + 1] - t[i]) * (x - i);
}
// the dry gain with both bands at 0
constexpr double baseGain = 1.00237;

// the fitted dynamics (REA_rodada124 §3)
struct Tuning
{
    double midThreshold = -30.13, midHoldMs = 5.0, midReleaseMs = 96.03, midGainReleaseMs = 7.706;
    double highThreshold = -38.03, highReleaseMs = 0.5354, highGainAttackMs = 2.1428, highGainReleaseMs = 55.52;
    double high2ReleaseMs = 0.0190, high2GainAttackMs = 56.73, high2GainReleaseMs = 53.45, high2Slope = 8.257;
    double high2Threshold = -41.13, high2Curve = 15.585;
};

class Engine
{
public:
    void prepare (double rate) noexcept
    {
        sr = rate;
        mid.hp (rate, 2993.0, 0.6291);
        high.hp (rate, 9000.0, 0.7069);
        reset();
    }
    void reset() noexcept
    {
        mid.reset();
        high.reset();
        envM = holdM = gM = envH = g1a = g1b = env2 = g2a = g2b = 0.0;
    }
    // processes in place; `out` is the output gain (linear)
    void process (float* l, float* r, int n, double midAmount, double highAmount, double out) noexcept
    {
        const Tuning t;
        const auto c = [this] (double ms) { return ms <= 0.0 ? 0.0 : std::exp (-1000.0 / (ms * sr)); };
        const auto s = (grid (gridMidS, midAmount) + grid (gridHighS, highAmount) - baseGain) * out;
        const auto km = grid (gridMidK, midAmount) * out, kh = grid (gridHighK, highAmount) * out;
        const auto mRel = c (t.midReleaseMs), mGr = c (t.midGainReleaseMs), mHold = t.midHoldMs * sr / 1000.0;
        const auto hRel = c (t.highReleaseMs), h1a = c (t.highGainAttackMs), h1r = c (t.highGainReleaseMs);
        const auto h2Rel = c (t.high2ReleaseMs), h2a = c (t.high2GainAttackMs), h2r = c (t.high2GainReleaseMs);
        const auto t2 = t.high2Threshold + t.high2Curve * (1.0 - highAmount) * (1.0 - highAmount);
        for (int i = 0; i < n; ++i)
        {
            const double xl = l[i], xr = r[i];
            const double ml = mid.process ((float) xl, 0), mr = mid.process ((float) xr, 1);
            const double hl = high.process ((float) xl, 0), hr = high.process ((float) xr, 1);
            // mid: peak with hold, 4:1
            const auto pm = std::max (std::fabs (ml), std::fabs (mr));
            if (pm >= envM) { envM = pm; holdM = mHold; }
            else if (holdM > 0.0) holdM -= 1.0;
            else envM = pm + (envM - pm) * mRel;
            const auto gmTarget = -0.75 * std::max (0.0, 20.0 * std::log10 (envM + 1e-30) - t.midThreshold);
            gM = gmTarget < gM ? gmTarget : gmTarget + (gM - gmTarget) * mGr;
            // high, stage 1: peak, 4:1, 2-pole gain smoothing
            const auto ph = std::max (std::fabs (hl), std::fabs (hr));
            envH = ph > envH ? ph : ph + (envH - ph) * hRel;
            const auto g1Target = -0.75 * std::max (0.0, 20.0 * std::log10 (envH + 1e-30) - t.highThreshold);
            const auto k1 = g1Target < g1b ? h1a : h1r;
            g1a = g1Target + (g1a - g1Target) * k1;
            g1b = g1a + (g1b - g1a) * k1;
            const auto g1 = std::pow (10.0, g1b / 20.0);
            // high, stage 2: feedback (detector after both stages), peak, 2-pole gain smoothing
            const auto g2 = std::pow (10.0, g2b / 20.0);
            const auto p2 = ph * g1 * g2;
            env2 = p2 >= env2 ? p2 : p2 + (env2 - p2) * h2Rel;
            const auto g2Target = -t.high2Slope * std::max (0.0, 20.0 * std::log10 (env2 + 1e-30) - t2);
            const auto k2 = g2Target < g2b ? h2a : h2r;
            g2a = g2Target + (g2a - g2Target) * k2;
            g2b = g2a + (g2b - g2a) * k2;
            const auto gm = km * std::pow (10.0, gM / 20.0), gh = kh * g1 * g2;
            l[i] = (float) (s * xl + gm * ml + gh * hl);
            r[i] = (float) (s * xr + gm * mr + gh * hr);
        }
    }
    float reductionDb() const noexcept { return (float) std::min (gM, g1b + g2b); }

private:
    struct Hp2 : Biquad
    {
        void hp (double rate, double fc, double q) noexcept
        {
            const auto w = 2.0 * 3.14159265358979323846 * fc / rate, a = std::sin (w) / (2.0 * q), cw = std::cos (w);
            set ((1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0, 1.0 + a, -2.0 * cw, 1.0 - a);
        }
        double process (float x, int ch) noexcept
        {
            const auto y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
    };
    double sr = 48000.0;
    Hp2 mid, high;
    double envM = 0, holdM = 0, gM = 0, envH = 0, g1a = 0, g1b = 0, env2 = 0, g2a = 0, g2b = 0;
};
} // namespace dyn

class Air final : public Effect
{
public:
    Air()
    {
        for (auto& o : os)
            o.setFactor (4);
    }

    void prepare (double rate) noexcept override
    {
        sr = rate;
        dry.setLength (Oversampler::latency());
        dynEngine.prepare (rate);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& o : os)
            o.reset();
        dry.reset();
        shelf.reset();
        pre.reset();
        post.reset();
        dynEngine.reset();
    }

    // the dynamic model has no latency (as the measured reference); the exciter path is oversampled
    int latencySamples() const noexcept override { return dynamicMode ? 0 : Oversampler::latency(); }
    float controlOutput (int k) const noexcept override { return meters.read (k); }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        const bool dynamicNow = p[dynamic] >= 0.5f;
        if (dynamicNow != dynamicMode)
        {
            dynamicMode = dynamicNow;
            reset();
        }
        for (int off = 0; off < n; off += maxBlock)
            if (dynamicMode)
                dynamicChunk (l + off, r + off, std::min (maxBlock, n - off), p);
            else
                chunk (l + off, r + off, std::min (maxBlock, n - off), p);
    }

private:
    void dynamicChunk (float* l, float* r, int n, const EffectParams& p) noexcept
    {
        const auto inPeak = Meters::peak (l, r, n);
        const auto mx = std::clamp (p[mix], 0.0f, 1.0f);
        if (mx < 1.0f)
            for (int i = 0; i < n; ++i)
                hl[(std::size_t) i] = l[i], hr[(std::size_t) i] = r[i];
        dynEngine.process (l, r, n, std::clamp (p[midAir], 0.0f, 1.0f), std::clamp (p[highAir], 0.0f, 1.0f), dbToGain (lin (p[output], -12.0, 12.0)));
        if (mx < 1.0f)
            for (int i = 0; i < n; ++i)
            {
                l[i] = hl[(std::size_t) i] + (l[i] - hl[(std::size_t) i]) * mx;
                r[i] = hr[(std::size_t) i] + (r[i] - hr[(std::size_t) i]) * mx;
            }
        meters.set (0.0f, -dynEngine.reductionDb(), inPeak, Meters::peak (l, r, n));
    }

    void chunk (float* l, float* r, int n, const EffectParams& p) noexcept
    {
        const auto inPeak = Meters::peak (l, r, n);
        shelf.shelf (sr, std::min (airHz (p[airFreq]), sr * 0.45), airDb (p[air]), true, 0.6);
        const auto ex = std::min (exciterHz (p[exciterFreq]), sr * 0.45);
        pre.filter (sr, 1, ex, 0.70710678);
        post.filter (sr, 1, ex, 0.70710678);
        const auto amount = 2.0f * std::clamp (p[harmonics], 0.0f, 1.0f);
        const auto odd = std::clamp (p[character], 0.0f, 1.0f);
        const auto mx = std::clamp (p[mix], 0.0f, 1.0f);
        const auto out = (float) dbToGain (lin (p[output], -12.0, 12.0));

        // harmonic generator (DAFX ch. 4, exciters): only the band above the exciter frequency is shaped, at 4x;
        // an even part (x² -like, 2nd harmonic) and an odd part (tanh − x, 3rd harmonic), blended by `character`
        auto gen = [odd] (float x) noexcept
        {
            const auto k = 4.0f * x;
            const auto even = (std::sqrt (1.0f + k * k) - 1.0f) * 0.25f;
            const auto oddPart = std::tanh (3.0f * x) / 3.0f - x;
            return (1.0f - odd) * even - odd * oddPart * 3.0f;
        };

        for (int i = 0; i < n; ++i)
        {
            hl[(std::size_t) i] = pre.process (l[i], 0);
            hr[(std::size_t) i] = pre.process (r[i], 1);
        }
        if (amount > 0.0f)
        {
            os[0].process (hl.data(), n, gen);
            os[1].process (hr.data(), n, gen);
        }
        else
        {
            // keep the oversamplers' state moving so turning the harmonics up does not click with stale history
            auto zero = [] (float) noexcept { return 0.0f; };
            os[0].process (hl.data(), n, zero);
            os[1].process (hr.data(), n, zero);
        }

        for (int i = 0; i < n; ++i)
        {
            float a = l[i], b = r[i];
            dry.process (a, b);
            const auto wa = shelf.process (a, 0) + amount * post.process (hl[(std::size_t) i], 0);
            const auto wb = shelf.process (b, 1) + amount * post.process (hr[(std::size_t) i], 1);
            l[i] = (a + (wa - a) * mx) * out;
            r[i] = (b + (wb - b) * mx) * out;
        }
        meters.set (0.0f, 0.0f, inPeak, Meters::peak (l, r, n));
    }

    static constexpr int maxBlock = 2048;
    double sr = 44100.0;
    std::array<Oversampler, 2> os;
    Delay2<256> dry;
    Biquad shelf, pre, post;
    dyn::Engine dynEngine;
    bool dynamicMode = false;
    std::array<float, maxBlock> hl {}, hr {};
    Meters meters;
};
} // namespace
} // namespace aether

// ================================================================== Ember

namespace ember
{
Style styleOf (float v) noexcept { return (Style) std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * (styles - 1)), 0, styles - 1); }
double driveDb (float v) noexcept { return lin (v, 0.0, 36.0); }
double toneTilt (float v) noexcept { return lin (v, -1.0, 1.0); }
double lowCutHz (float v) noexcept { return v <= 0.0f ? 0.0 : logMap (v, 20.0, 500.0); }
double highCutHz (float v) noexcept { return v >= 1.0f ? 0.0 : logMap (v, 2000.0, 20000.0); }
int oversampling (float v) noexcept { return v < 0.25f ? 2 : (v < 0.75f ? 4 : 8); }
int voiceIndex (float v) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * voices), 0, voices); }
const char* voiceName (int k) noexcept
{
    static const char* const names[voices + 1] { "Off", "Tape pre", "Console", "Grit", "Triode", "Pentode" };
    return names[std::clamp (k, 0, voices)];
}
Style voiceStyle (int k) noexcept
{
    static const Style st[voices + 1] { Style::tube, Style::tube, Style::console, Style::transistor, Style::tube, Style::tape };
    return st[std::clamp (k, 0, voices)];
}
double lowCutWideHz (float v) noexcept { return v <= 0.0f ? 0.0 : logMap (v, 20.0, 1000.0); }
double highCutWideHz (float v) noexcept { return v >= 1.0f ? 0.0 : logMap (v, 1000.0, 20000.0); }
double driveSteps (float v) noexcept { return driveDb (v) / 3.6; }
int bandCount (float v) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 3.0f), 0, 3) + 1; }
double crossoverHz (float v) noexcept { return logMap (v, 30.0, 18000.0); }
float crossoverValue (double hz) noexcept { return (float) std::clamp (std::log (hz / 30.0) / std::log (600.0), 0.0, 1.0); }
int slopeDbOct (float v) noexcept { return v < 0.25f ? 12 : (v < 0.75f ? 24 : 48); }
double bandLevelDb (float v) noexcept { return lin (v, -18.0, 18.0); }
BandState bandState (float v) noexcept { return (BandState) std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 2.0f), 0, 2); }
double inputDb (float v) noexcept { return lin (v, -24.0, 24.0); }
double toneDb (float v) noexcept { return lin (v, -18.0, 18.0); }
double feedbackAmount (float v) noexcept { return lin (v, 0.0, 1.0); }
double feedbackHz (float v) noexcept { return logMap (v, 10.0, 1000.0); }
double dynamicsAmount (float v) noexcept { return lin (v, -1.0, 1.0); }
double panOf (float v) noexcept { return std::clamp ((double) v * 2.0 - 1.0, -1.0, 1.0); }
std::pair<float, float> panScales (double pan) noexcept
{
    pan = std::clamp (pan, -1.0, 1.0);
    return { (float) (pan > 0.0 ? 1.0 - pan : 1.0), (float) (pan < 0.0 ? 1.0 + pan : 1.0) };
}
bool midSide (const EffectParams& p) noexcept { return on (p[channelMode]); }
int bandParam (int band, int field) noexcept
{
    if (band <= 0)
    {
        static constexpr int first[] { style, drive, band1Mix, band1Level };
        return first[std::clamp (field, 0, 3)];
    }
    if (band < firstBands)
        return band2Style + (band - 1) * 4 + std::clamp (field, 0, 3);
    return band5Style + (std::min (band, maxBands - 1) - firstBands) * 4 + std::clamp (field, 0, 3); // round 150
}
int activeBands (const EffectParams& p) noexcept
{
    const auto more = std::clamp ((int) std::lround (std::clamp (p[bandsMore], 0.0f, 1.0f) * 2.0f), 0, 2);
    return more > 0 ? firstBands + more : bandCount (p[bands]);
}
std::pair<float, float> bandCountValues (int n) noexcept
{
    n = std::clamp (n, 1, maxBands);
    return { (float) (std::min (n, firstBands) - 1) / 3.0f, n <= firstBands ? 0.0f : (float) (n - firstBands) / 2.0f };
}
int characterIndex (float v) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * characters), 0, characters); }

// ---- round 152 (REA_rodada152b): own crossover slopes, linear phase, modulation, the effect characters' law ----
namespace
{
// a knob law given as a table at equal steps of the value (the reference's value texts, read black-box)
template <std::size_t N>
double tableLaw (const double (&t)[N], float v) noexcept
{
    const auto x = std::clamp ((double) v, 0.0, 1.0) * (double) (N - 1);
    const auto i = std::min ((std::size_t) x, N - 2);
    return t[i] + (t[i + 1] - t[i]) * (x - (double) i);
}
template <std::size_t N>
float tableInverse (const double (&t)[N], double y) noexcept
{
    if (y <= t[0]) return 0.0f;
    for (std::size_t i = 0; i + 1 < N; ++i)
        if (y <= t[i + 1])
            return (float) (((double) i + (y - t[i]) / std::max (t[i + 1] - t[i], 1e-12)) / (double) (N - 1));
    return 1.0f;
}
// XLFO Frequency (21 texts, 0.05 apart) [EXEC]
constexpr double rateTable[21] { 0.02, 0.145, 0.27, 0.395, 0.52, 1.02, 1.52, 2.02, 2.52, 3.02, 3.52, 6.395, 9.27, 12.145,
                                 15.019, 32.519, 50.018, 100.02, 150.01, 250.01, 500.0 };
// Slot Level (21 texts) [EXEC]
constexpr double levelTable[21] { 0.0, 0.001, 0.002, 0.018, 0.034, 0.05, 0.072, 0.094, 0.116, 0.138, 0.16, 0.21, 0.26, 0.31,
                                  0.36, 0.498, 0.635, 0.773, 0.91, 0.955, 1.0 };
// EF Attack / Release (41 texts, 0.025 apart) [EXEC]
constexpr double attackTable[41] { 1, 4.33, 7.66, 10.99, 14.32, 17.65, 20.98, 25.97, 30.97, 34.81, 38.65, 42.5, 46.34, 50.18,
                                   56.66, 63.79, 70.93, 78.42, 85.92, 93.41, 100.9, 108.4, 115.9, 123.4, 130.9, 143.4, 155.8,
                                   168.3, 185.1, 202.9, 220.8, 253.2, 285.7, 318.2, 350.6, 405.6, 460.5, 560.4, 660.3, 793.5, 1000 };
constexpr double releaseTable[41] { 50, 66.5, 83, 99.5, 116, 132.5, 149, 173.7, 198.5, 217.5, 236.6, 255.6, 274.7, 293.7,
                                    325.8, 361.1, 396.5, 433.6, 470.7, 507.9, 545, 582.1, 619.2, 656.4, 693.5, 755.4, 817.2,
                                    879.1, 962.2, 1051, 1139, 1300, 1461, 1622, 1782, 2055, 2327, 2822, 3317, 3977, 5000 };
} // namespace

int ownSlopeDbOct (float v) noexcept
{
    static constexpr int s[ownSlopes + 1] { 0, 6, 12, 24, 36, 48 };
    return s[std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * ownSlopes), 0, ownSlopes)];
}
float ownSlopeValue (int dbOct) noexcept
{
    static constexpr int s[ownSlopes] { 6, 12, 24, 36, 48 };
    for (int k = 0; k < ownSlopes; ++k)
        if (dbOct <= s[k]) return (float) (k + 1) / (float) ownSlopes;
    return 1.0f;
}
int crossoverSlopeDbOct (const EffectParams& p, int k) noexcept
{
    const auto own = ownSlopeDbOct (p[(std::size_t) (crossSlope1 + std::clamp (k, 0, maxBands - 2))]);
    return own > 0 ? own : slopeDbOct (p[xoverSlope]);
}
bool linearPhase (const EffectParams& p) noexcept { return on (p[processing]); }
int linearLatency (double sampleRate) noexcept { return dyn::prism::linearLatency (sampleRate); }
double linearLow (double hz, double crossoverHz, int slopeDbOct) noexcept
{
    const auto x = std::max (hz, 0.0) / std::max (crossoverHz, 1.0);
    return 1.0 / (1.0 + std::pow (x, (double) std::max (slopeDbOct, 6) / 6.0));
}
ModSource modSourceOf (float v) noexcept { return (ModSource) std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * (modSources - 1)), 0, modSources - 1); }
int modTargetOf (float v) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * (modTargets - 1)), 0, modTargets - 1); }
const char* modTargetName (int t) noexcept
{
    static const char* const names[modTargets] { "None", "Input gain", "Output gain", "Mix",
                                                 "B1 drive", "B2 drive", "B3 drive", "B4 drive", "B5 drive", "B6 drive",
                                                 "B1 level", "B2 level", "B3 level", "B4 level", "B5 level", "B6 level",
                                                 "B1 mix", "B2 mix", "B3 mix", "B4 mix", "B5 mix", "B6 mix",
                                                 "Crossover 1", "Crossover 2", "Crossover 3", "Crossover 4", "Crossover 5" };
    return names[std::clamp (t, 0, modTargets - 1)];
}
int modTargetParam (int t) noexcept
{
    if (t <= 0 || t >= modTargets) return -1;
    if (t == 1) return inputGain;
    if (t == 2) return output;
    if (t == 3) return mix;
    if (t < 10) return bandParam (t - 4, 1);
    if (t < 16) return bandParam (t - 10, 3);
    if (t < 22) return bandParam (t - 16, 2);
    return crossoverParam (t - 22);
}
double modDelta (int t, double units) noexcept
{
    // the reference's target ranges (its parameters' value texts) in Ember's parameter units [EXEC: a slot of level L
    // moves its target by L × the source in the target's own 0 … 1 range — e.g. B1 Output Level: 72 dB × L]
    if (t <= 0 || t >= modTargets) return 0.0;
    if (t == 1) return units * 72.0 / 48.0;            // ±36 dB → Ember's input ±24 dB
    if (t == 2) return units * 72.0 / 36.0;            // ±36 dB → Ember's output ±18 dB
    if (t == 3 || (t >= 16 && t < 22)) return units;   // 0 … 100 %
    if (t < 10) return units * 30.0 / 36.0;            // 0 … 100 % = 0 … 30 dB of Ember's 0 … 36 dB drive
    if (t < 16) return units * 72.0 / 36.0;            // −∞ … +36 dB (0.5 = 0 dB) → Ember's ±18 dB
    return units * std::log (450.0) / std::log (600.0); // 40 Hz … 18 kHz → Ember's 30 Hz … 18 kHz (log)
}
double modAmount (float v) noexcept
{
    const auto d = 2.0 * (double) std::clamp (v, 0.0f, 1.0f) - 1.0;
    const auto m = tableLaw (levelTable, (float) std::abs (d));
    return d < 0.0 ? -m : m;
}
float modAmountValue (double level) noexcept
{
    const auto m = tableInverse (levelTable, std::min (std::abs (level), 1.0));
    return (float) (0.5 + (level < 0.0 ? -0.5 : 0.5) * (double) m);
}
double lfoRateHz (float v) noexcept { return tableLaw (rateTable, v); }
float lfoRateValue (double hz) noexcept { return tableInverse (rateTable, hz); }
LfoShape lfoShapeOf (float v) noexcept { return (LfoShape) std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * (lfoShapes - 1)), 0, lfoShapes - 1); }
double lfoValue (LfoShape s, double phase) noexcept
{
    // the reference's step LFO with two steps (+1, −1): each step glides from the other value over the whole step with
    // its glide function (linear, squared, square root, sine), or jumps (square) [EXEC: the four functions measured]
    phase -= std::floor (phase);
    const bool first = phase < 0.5;
    const auto u = (first ? phase : phase - 0.5) * 2.0;
    if (s == LfoShape::square) return first ? 1.0 : -1.0;
    double f = u;
    switch (s)
    {
        case LfoShape::sine: f = 0.5 - 0.5 * std::cos (pi * u); break;
        case LfoShape::easeIn: f = u * u; break;
        case LfoShape::easeOut: f = std::sqrt (u); break;
        default: break;
    }
    const auto from = first ? -1.0 : 1.0;
    return from + (-2.0 * from) * f;
}
double envAttackMs (float v) noexcept { return tableLaw (attackTable, v); }
double envReleaseMs (float v) noexcept { return tableLaw (releaseTable, v); }
double envSource (double followerDb) noexcept { return std::max (0.0, (followerDb + 60.7) / 30.0); }
bool modulated (const EffectParams& p) noexcept
{
    for (int k = 0; k < modSlots; ++k)
        if (modSourceOf (p[(std::size_t) (mod1Source + 3 * k)]) != ModSource::off
            && modTargetOf (p[(std::size_t) (mod1Target + 3 * k)]) != 0 && std::abs (modAmount (p[(std::size_t) (mod1Amount + 3 * k)])) > 1e-9)
            return true;
    return false;
}
bool measuredEffects (const EffectParams& p) noexcept { return on (p[effectLaw]); }

std::vector<std::pair<int, float>> linkedEdit (const EffectParams& p, int param, float value)
{
    std::vector<std::pair<int, float>> edits { { param, value } };
    if (param == classicDrive && on (p[classicAuto]))
        edits.push_back ({ classicOutput, emberclassic::autoOutput (p[classicOutput], p[classicDrive], value) });
    return edits;
}
double collapseRatio (double referenceDrive) noexcept { return std::exp2 (-2.0 * std::clamp (referenceDrive, 0.0, 1.0)); }

float shape (Style s, float x) noexcept
{
    switch (s)
    {
        case Style::tube:
        {
            // asymmetric: a biased tanh (even and odd harmonics, like a single-ended triode stage), unit slope at 0
            constexpr float bias = 0.35f;
            const float tb = std::tanh (bias);
            return (std::tanh (x + bias) - tb) / (1.0f - tb * tb);
        }
        case Style::tape: return x / std::sqrt (1.0f + x * x); // gentle symmetric (odd) saturation
        case Style::transistor:
        {
            const auto a = std::abs (x);
            const auto a2 = a * a;
            return x / std::sqrt (std::sqrt (1.0f + a2 * a2)); // harder knee: x / (1 + x⁴)^¼
        }
        case Style::console:
        {
            // round 135: a softer, mostly symmetric knee with a little even content (x / (1 + |x|^2.5)^0.4 + a small bias)
            constexpr float bias = 0.12f;
            auto f = [] (float u) { const auto a = std::abs (u); return u / std::pow (1.0f + std::pow (a, 2.5f), 0.4f); };
            const auto fb = f (bias);
            return f (x + bias) - fb;
        }
        case Style::fuzz:
        default:
            if (x >= 0.0f)
            {
                const auto y = x / 0.6f, y2 = y * y, y4 = y2 * y2;
                return 0.6f * y / std::sqrt (std::sqrt (std::sqrt (1.0f + y4 * y4))); // near-hard positive clip at 0.6
            }
            return std::tanh (1.5f * x) / 1.5f;
    }
}

// ---- round 150: the measured characters (REA_rodada150b) ----
namespace
{
struct CharacterData
{
    const char* name;
    int family;              // 0 soft knee u / (1 + |u|^p)^(1/p) around a bias, 1 sine fold, 2 full-wave rectifier
    float p, bias;
    float driveDb[6], gainDb[6]; // at the reference's drive 0, 20, 40, 60, 80, 100 %: the curve's input / output gain
    int highPasses, lowPasses;   // identical cookbook sections (n × the same filter)
    float hpHz, hpQ, lpHz, lpQ;
    float bellHz[3], bellDb[3], bellQ[3];
    float eqDb;                  // the EQ's gain (0 dB at 1 kHz overall)
};
constexpr CharacterData characterTable[characters] {
#include "core/EmberCharacters.inc"
};

float softKnee (float u, float p) noexcept
{
    const auto a = std::abs (u);
    if (p == 2.0f) return u / std::sqrt (1.0f + a * a);
    if (p == 1.0f) return u / (1.0f + a);
    return u / std::pow (1.0f + std::pow (a, p), 1.0f / p);
}

double interpDrive (const float* table, double drive) noexcept
{
    const auto x = std::clamp (drive, 0.0, 1.0) * 5.0;
    const auto i = std::min ((int) x, 4);
    return table[i] + (table[i + 1] - table[i]) * (x - i);
}
} // namespace

const char* characterName (int k) noexcept
{
    if (k <= 0 || k > characters) return "Off";
    return characterTable[k - 1].name;
}

float characterShape (int k, double drive, float x) noexcept
{
    if (k <= 0 || k > characters) return x;
    const auto& c = characterTable[k - 1];
    const auto d = (float) dbToGain (interpDrive (c.driveDb, drive));
    const auto a = (float) dbToGain (interpDrive (c.gainDb, drive)) / d; // A is the small-signal gain: A · g (D x) / D
    const auto u = d * x;
    if (c.family == 1) return a * std::sin (u);
    if (c.family == 2) return a * std::abs (u);
    const auto f0 = softKnee (c.bias, c.p);
    const auto slope = std::pow (1.0f + std::pow (std::abs (c.bias), c.p), -1.0f / c.p - 1.0f);
    return a * (softKnee (u + c.bias, c.p) - f0) / slope;
}

namespace
{
// a band's character: the curve (run oversampled by the caller), its input / output gain from the drive and the EQ after
// it (at the base rate, after the band's DC blocker)
struct CharacterStage
{
    int k = 0, lastK = -1, sections = 0;
    double lastSr = 0.0;
    int family = 0;
    float p = 2.0f, bias = 0.0f, f0 = 0.0f, slope = 1.0f, eqGain = 1.0f;
    std::array<Biquad, 9> eq;

    void reset() noexcept { for (auto& q : eq) q.reset(); }

    void configure (int kk, double sr) noexcept
    {
        k = std::clamp (kk, 0, characters);
        if (k == 0 || (k == lastK && sr == lastSr)) return;
        const auto& c = characterTable[k - 1];
        family = c.family;
        p = c.p;
        bias = c.bias;
        f0 = softKnee (bias, p);
        slope = std::pow (1.0f + std::pow (std::abs (bias), p), -1.0f / p - 1.0f);
        sections = 0;
        for (int i = 0; i < c.highPasses && sections < 9; ++i) eq[(std::size_t) sections++].filter (sr, 1, c.hpHz, c.hpQ);
        for (int i = 0; i < c.lowPasses && sections < 9; ++i) eq[(std::size_t) sections++].filter (sr, 0, c.lpHz, c.lpQ);
        for (int i = 0; i < 3 && sections < 9; ++i)
            if (std::abs (c.bellDb[i]) > 0.01f)
                eq[(std::size_t) sections++].peaking (sr, std::min ((double) c.bellHz[i], sr * 0.45), c.bellQ[i], c.bellDb[i]);
        eqGain = (float) dbToGain (c.eqDb);
        if (k != lastK) reset();
        lastK = k;
        lastSr = sr;
    }

    // the input (drive) and output gains for Ember's drive in dB on one channel (+ PUNISH's extra input gain)
    std::pair<float, float> gains (double driveDb, double extraDb) const noexcept
    {
        const auto& c = characterTable[std::max (k, 1) - 1];
        const auto d = referenceDrive (driveDb);
        // round 152: Collapse with the measured law — the pitch drop's own level curve (the output's rms at seven input
        // levels per drive step, fitted with the same knee: 0.02 … 0.37 dB rms) [EXEC]
        static constexpr float collapseD[6] { -1.5f, 1.5f, 11.0f, 22.0f, 35.25f, 47.0f };
        static constexpr float collapseA[6] { 0.82f, 1.5f, 1.25f, 3.21f, 12.76f, 24.96f };
        const bool measured = law && k == collapseCharacter;
        const auto dDb = interpDrive (measured ? collapseD : c.driveDb, d);
        return { (float) dbToGain (dDb + extraDb), (float) dbToGain (interpDrive (measured ? collapseA : c.gainDb, d) - dDb) }; // A · g (D x) / D
    }
    bool law = false; // round 152: the effect characters' measured law (set by the engine per block)

    float curve (float u) const noexcept
    {
        if (family == 1) return std::sin (u);
        if (family == 2) return std::abs (u);
        return (softKnee (u + bias, p) - f0) / slope;
    }

    void post (float& a, float& b) noexcept
    {
        for (int i = 0; i < sections; ++i)
        {
            a = eq[(std::size_t) i].process (a, 0);
            b = eq[(std::size_t) i].process (b, 1);
        }
        a *= eqGain;
        b *= eqGain;
    }
};
} // namespace

namespace
{
// A band's round-128 stages (Saturn parity): tone after the shaper, a feedback loop around it, dynamics on its level.
// Each is skipped while it is at its neutral setting, so the old sound is untouched.
struct BandExtras
{
    std::array<Biquad, 4> tone;   // bass, mid, treble, presence
    bool toneOn = false;
    Biquad fbBand;                // the feedback band (at the oversampled rate)
    double fbGain = 0.0;
    float fbLast[2] {};
    double dyn = 0.0, env = 0.0, envAtk = 0.0, envRel = 0.0;
    std::array<float, 4> lastTone { -1.0f, -1.0f, -1.0f, -1.0f };
    float lastFbHz = -1.0f;
    int lastFactor = 0;

    void reset() noexcept
    {
        for (auto& t : tone) t.reset();
        fbBand.reset();
        fbLast[0] = fbLast[1] = 0.0f;
        env = 0.0;
    }

    void configure (const EffectParams& p, int band, double sr, int factor) noexcept
    {
        std::array<float, 4> tv;
        for (int k = 0; k < 4; ++k) tv[(std::size_t) k] = p[(std::size_t) bandExtra (band, 2 + k)];
        if (tv != lastTone)
        {
            tone[0].shelf (sr, 120.0, toneDb (tv[0]), false, 0.7);
            tone[1].peaking (sr, 800.0, 0.8, toneDb (tv[1]));
            tone[2].shelf (sr, 3500.0, toneDb (tv[2]), true, 0.7);
            tone[3].peaking (sr, 6000.0, 1.0, toneDb (tv[3]));
            lastTone = tv;
        }
        toneOn = false;
        for (const auto v : tv) toneOn = toneOn || std::abs (toneDb (v)) > 1e-6;
        fbGain = 0.9 * feedbackAmount (p[(std::size_t) bandExtra (band, 6)]);
        const auto fhz = p[(std::size_t) bandExtra (band, 7)];
        if (fbGain > 0.0 && (fhz != lastFbHz || factor != lastFactor))
        {
            fbBand.filter (sr * factor, 2, feedbackHz (fhz), 1.4);
            lastFbHz = fhz;
            lastFactor = factor;
        }
        dyn = dynamicsAmount (p[(std::size_t) bandExtra (band, 8)]);
        envAtk = coef (0.005, sr);
        envRel = coef (0.080, sr);
    }

    // the shaper with the feedback loop: the band-passed last output added to the input (channel ch)
    float shapeWithFeedback (Style s, float x, int ch) noexcept
    {
        return feedbackShape ([s] (float u) noexcept { return shape (s, u); }, x, ch);
    }

    template <typename Curve>
    float feedbackShape (Curve&& curve, float x, int ch) noexcept
    {
        const auto y = curve (x + (float) fbGain * fbLast[ch]);
        fbLast[ch] = fbBand.process (y, ch);
        return y;
    }

    // after the shaper: the level follows the band's input level (dynamics), then the tone
    void post (float& a, float& b, float inA, float inB) noexcept
    {
        if (dyn != 0.0)
        {
            const double x = std::max (std::abs (inA), std::abs (inB));
            env = x > env ? envAtk * env + (1.0 - envAtk) * x : envRel * env + (1.0 - envRel) * x;
            const double over = gainToDb (env + 1e-9) + 18.0; // dB above −18 dBFS
            const auto g = (float) dbToGain (std::clamp (dyn * 0.5 * over, -18.0, 18.0));
            a *= g;
            b *= g;
        }
        if (toneOn)
            for (auto& t : tone)
            {
                a = t.process (a, 0);
                b = t.process (b, 1);
            }
    }
};

// round 152: the measured "breakdown" effect's pitch drop (Collapse with the measured law): a two-tap delay-line pitch
// shifter (taps half a window apart, sin² cross-fade) whose delay grows at 1 − ratio per sample [EXEC: the output
// frequency is the input's × 2^(−2 · drive) at every level and input frequency; the window length is LYNX's]
struct PitchDown
{
    std::vector<float> buf[2];
    int size = 0, w = 0;
    double phase = 0.0, window = 2400.0;

    void prepare (double sr)
    {
        window = std::max (64.0, 0.05 * sr);
        size = 1;
        while (size < (int) window + 8) size <<= 1;
        for (auto& b : buf) b.assign ((std::size_t) size, 0.0f);
        reset();
    }
    void reset() noexcept
    {
        for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
        w = 0;
        phase = 0.0;
    }
    float tap (int c, double d) const noexcept
    {
        const auto pos = (double) w - d;
        const auto i0 = (int) std::floor (pos);
        const auto f = (float) (pos - (double) i0);
        const auto& b = buf[c];
        const auto a = b[(std::size_t) (i0 & (size - 1))], bb = b[(std::size_t) ((i0 + 1) & (size - 1))];
        return a + (bb - a) * f;
    }
    void run (float& l, float& r, double ratio) noexcept
    {
        if (size == 0) return;
        buf[0][(std::size_t) w] = l;
        buf[1][(std::size_t) w] = r;
        if (ratio < 0.9999)
        {
            const auto ph2 = phase + 0.5 - std::floor (phase + 0.5);
            const auto g1 = std::sin (pi * phase), g2 = std::sin (pi * ph2);
            const auto d1 = phase * window, d2 = ph2 * window;
            l = (float) (g1 * g1) * tap (0, d1) + (float) (g2 * g2) * tap (0, d2);
            r = (float) (g1 * g1) * tap (1, d1) + (float) (g2 * g2) * tap (1, d2);
            phase += (1.0 - ratio) / window;
            phase -= std::floor (phase);
        }
        else
            phase = 0.0;
        w = (w + 1) & (size - 1);
    }
};

// round 152: a plain stereo delay of a fixed length (the linear-phase mode's dry paths), allocated in prepare()
struct LongDelay
{
    std::vector<float> buf[2];
    int n = 0, pos = 0;
    void allocate (int length)
    {
        n = std::max (length, 0);
        for (auto& b : buf) b.assign ((std::size_t) std::max (n, 1), 0.0f);
        pos = 0;
    }
    void reset() noexcept
    {
        for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
        pos = 0;
    }
    void process (float& l, float& r) noexcept
    {
        if (n <= 0) return;
        std::swap (buf[0][(std::size_t) pos], l);
        std::swap (buf[1][(std::size_t) pos], r);
        pos = pos + 1 == n ? 0 : pos + 1;
    }
};

class Saturator final : public Effect
{
public:
    explicit Saturator (const EffectParams* initial = nullptr) noexcept
    {
        if (initial != nullptr) linOn = linearPhase (*initial);
    }

    void prepare (double rate) noexcept override
    {
        sr = rate;
        // round 152: the linear-phase crossovers (FFT convolvers) and their dry delays, the pitch drop, allocated here
        linLatency = linearLatency (rate);
        const auto size = dyn::prism::linearFftSize (rate);
        if (size != linSize)
        {
            linSize = size;
            for (auto& c : linConv)
                c.init (size);
            linTable.assign ((std::size_t) (size / 2 + 1), 0.0f);
        }
        linDry.allocate (linLatency);
        linTop.allocate (linLatency);
        linDesigned = false;
        for (auto& pd : pitch)
            pd.prepare (rate);
        dry.setLength (Oversampler::latency());
        dc.set (sr, 8.0);
        classic.prepare (sr); // round 136: the Classic voicing (allocates here, never in process)
        classicOn = false;
        for (auto& d : bandDry)
            d.setLength (Oversampler::latency());
        for (auto& d : bandDc)
            d.set (sr, 8.0);
        splitBands = 0; // redesign the crossovers at the new rate
        lastVoice = -1;
        reset();
    }

    void reset() noexcept override
    {
        classic.reset();
        for (auto& o : os)
            o.reset();
        dry.reset();
        lowcut.reset();
        emphasis.reset();
        deemphasis.reset();
        tiltLo.reset();
        tiltHi.reset();
        highcut.reset();
        highcut2.reset();
        highcut1.reset();
        voicePre.reset();
        voicePost.reset();
        dc.reset();
        for (auto& b : bandOs)
            for (auto& o : b)
                o.reset();
        for (auto& d : bandDry)
            d.reset();
        for (auto& d : bandDc)
            d.reset();
        for (auto& x : split) x.reset();
        for (auto& row : ap)
            for (auto& x : row) x.reset();
        for (auto& x : extras)
            x.reset();
        for (auto& x : chars)
            x.reset();
        // round 152
        for (auto& c : linConv)
            if (c.n > 0) c.clear();
        linDry.reset();
        linTop.reset();
        for (auto& pd : pitch)
            pd.reset();
        lfoPhase = { 0.0, 0.0 };
        envPower = 0.0;
    }

    int latencySamples() const noexcept override { return Oversampler::latency() + (linOn ? linLatency : 0); }
    float controlOutput (int k) const noexcept override { return meters.read (k); }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        linOn = linearPhase (p); // round 152 (its latency follows)
        if (! modulated (p))
        {
            processBlock (l, r, n, p);
            return;
        }
        // round 152: the modulation, at a control rate of 32 samples — the sources at the start of each block, the
        // targets moved from the slot's own values
        constexpr int step = 32;
        for (int off = 0; off < n; off += step)
        {
            const int m = std::min (step, n - off);
            modulate (l + off, r + off, m, p);
            processBlock (l + off, r + off, m, pm);
        }
    }

private:
    void modulate (const float* l, const float* r, int n, const EffectParams& p) noexcept
    {
        // the follower: a mean-square one-pole (attack while rising, release / 10 while falling) [EXEC fit: the
        // trial's default 1 ms / 50 ms within 0.025 source units rms, REA_rodada152b §4]
        const auto ta = envAttackMs (p[envAttack]) * 1e-3 * sr, tr = envReleaseMs (p[envRelease]) * 1e-4 * sr;
        const auto aa = 1.0 - std::exp (-1.0 / std::max (ta, 1e-3)), ar = 1.0 - std::exp (-1.0 / std::max (tr, 1e-3));
        const auto envNow = envSource (10.0 * std::log10 (envPower + 1e-30));
        for (int i = 0; i < n; ++i)
        {
            const double v = 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
            envPower += (v > envPower ? aa : ar) * (v - envPower);
        }
        std::array<double, 2> lfo {};
        for (int k = 0; k < 2; ++k)
        {
            lfo[(std::size_t) k] = lfoValue (lfoShapeOf (p[(std::size_t) (k == 0 ? lfo1Shape : lfo2Shape)]), lfoPhase[(std::size_t) k]);
            lfoPhase[(std::size_t) k] += lfoRateHz (p[(std::size_t) (k == 0 ? lfo1Rate : lfo2Rate)]) * (double) n / sr;
            lfoPhase[(std::size_t) k] -= std::floor (lfoPhase[(std::size_t) k]);
        }
        pm = p;
        const bool lin = linearPhase (p);
        for (int k = 0; k < modSlots; ++k)
        {
            const auto src = modSourceOf (p[(std::size_t) (mod1Source + 3 * k)]);
            const auto t = modTargetOf (p[(std::size_t) (mod1Target + 3 * k)]);
            const auto prm = modTargetParam (t);
            if (src == ModSource::off || prm < 0 || (lin && t >= 22)) continue; // linear phase: crossovers not modulated
            const auto value = src == ModSource::lfo1 ? lfo[0] : (src == ModSource::lfo2 ? lfo[1] : envNow);
            pm[(std::size_t) prm] += (float) modDelta (t, modAmount (p[(std::size_t) (mod1Amount + 3 * k)]) * value);
        }
        for (int k = 0; k < modSlots; ++k)
            if (const auto prm = modTargetParam (modTargetOf (p[(std::size_t) (mod1Target + 3 * k)])); prm >= 0)
                pm[(std::size_t) prm] = std::clamp (pm[(std::size_t) prm], 0.0f, 1.0f);
    }

    void processBlock (float* l, float* r, int n, const EffectParams& p) noexcept
    {
        // round 147: mid / side — M = (L + R) / 2 on channel 0, S = (R − L) / 2 on channel 1 (the trial's sign: a
        // left-only tone's even harmonics land on the right), decoded L = M − S, R = M + S
        const bool ms = midSide (p);
        if (ms)
            for (int i = 0; i < n; ++i)
            {
                const auto m = 0.5f * (l[i] + r[i]), sd = 0.5f * (r[i] - l[i]);
                l[i] = m;
                r[i] = sd;
            }
        // round 150 (measured): the global input pan before everything and the output pan after the dry / wet mix,
        // the far channel × (1 − |pan|), on left / right or (in mid / side) on mid / side like the band pan
        const auto [inA, inB] = panScales (panOf (p[inputPan]));
        if (inA != 1.0f || inB != 1.0f)
            for (int i = 0; i < n; ++i) { l[i] *= inA; r[i] *= inB; }
        for (int off = 0; off < n; off += maxBlock)
            chunk (l + off, r + off, std::min (maxBlock, n - off), p);
        const auto [outA, outB] = panScales (panOf (p[outputPan]));
        if (outA != 1.0f || outB != 1.0f)
            for (int i = 0; i < n; ++i) { l[i] *= outA; r[i] *= outB; }
        if (ms)
            for (int i = 0; i < n; ++i)
            {
                const auto m = l[i], sd = r[i];
                l[i] = m - sd;
                r[i] = m + sd;
            }
    }

    void chunk (float* l, float* r, int n, const EffectParams& p) noexcept
    {
        // round 152: linear phase — the single band and the Classic voicing are delayed like the FIR bands
        if (linearPhase (p) && (on (p[voicing]) || activeBands (p) <= 1))
            for (int i = 0; i < n; ++i)
                linDry.process (l[i], r[i]);
        if (on (p[voicing])) // round 136: the Classic voicing (core/EmberClassic.h) replaces the whole engine
        {
            chunkClassic (l, r, n, p);
            return;
        }
        classicOn = false;
        if (const auto inDb = inputDb (p[inputGain]); std::abs (inDb) > 1e-9) // round 128: the input gain
        {
            const auto g = (float) dbToGain (inDb);
            for (int i = 0; i < n; ++i) { l[i] *= g; r[i] *= g; }
        }
        if (activeBands (p) > 1)
        {
            chunkBands (l, r, n, p);
            return;
        }
        const auto inPeak = Meters::peak (l, r, n);
        const auto factor = oversampling (p[quality]);
        for (auto& o : os)
            o.setFactor (factor);
        // round 135: a voicing replaces the Style menu's curve and adds its own EQ; PUNISH adds 20 dB before the drive
        // round 150: band 1's character (when set) replaces the Style menu and the voicing
        auto& chr = chars[0];
        chr.configure (characterIndex (p[characterParam (0)]), sr);
        chr.law = measuredEffects (p); // round 152
        const auto vk = chr.k > 0 ? 0 : voiceIndex (p[voice]);
        const auto s = vk > 0 ? voiceStyle (vk) : styleOf (p[style]);
        const auto drv = (float) dbToGain (driveDb (p[drive]) + (on (p[punish]) ? punishDb : 0.0));
        // round 147: the drive pan — the far channel's drive × (1 − |pan|) (in dB, the trial's 0 … 100 % scale)
        const auto [dsA, dsB] = panScales (panOf (p[bandDrivePanParam (0)]));
        const bool drivePanned = dsA != 1.0f || dsB != 1.0f;
        const auto drvA = drivePanned ? (float) dbToGain (driveDb (p[drive]) * dsA + (on (p[punish]) ? punishDb : 0.0)) : drv;
        const auto drvB = drivePanned ? (float) dbToGain (driveDb (p[drive]) * dsB + (on (p[punish]) ? punishDb : 0.0)) : drv;
        const auto [panA, panB] = panScales (panOf (p[bandPanParam (0)]));
        const bool tape = s == Style::tape && vk == 0 && chr.k == 0;
        const auto [lc, hc] = configureCuts (p);
        configureVoice (vk);
        // tape: pre-emphasis before the drive and the inverse after it, so the highs saturate first (DAFX 4.3)
        emphasis.shelf (sr, 3000.0, 6.0, true, 0.7);
        deemphasis.shelf (sr, 3000.0, -6.0, true, 0.7);
        const auto tilt = toneTilt (p[tone]);
        tiltLo.shelf (sr, 1000.0, -6.0 * tilt, false, 0.5);
        tiltHi.shelf (sr, 1000.0, 6.0 * tilt, true, 0.5);
        const auto mx = std::clamp (p[mix], 0.0f, 1.0f);
        const auto out = (float) dbToGain (lin (p[output], -18.0, 18.0));
        if (on (p[autoGain]) && (drv != lastDrive || s != lastStyle))
        {
            makeup = autoMakeup (s, drv);
            lastDrive = drv;
            lastStyle = s;
        }
        const auto comp = on (p[autoGain]) ? makeup : 1.0f;
        const auto compA = drivePanned && on (p[autoGain]) ? autoMakeup (s, drvA) : comp;
        auto compB = drivePanned && on (p[autoGain]) ? autoMakeup (s, drvB) : comp;
        auto drvA2 = drvA, drvB2 = drvB, compA2 = compA;
        if (chr.k > 0)
        {
            const auto extra = on (p[punish]) ? punishDb : 0.0;
            const auto ga = chr.gains (driveDb (p[drive]) * dsA, extra), gb = chr.gains (driveDb (p[drive]) * dsB, extra);
            drvA2 = ga.first;
            compA2 = ga.second;
            drvB2 = gb.first;
            compB = gb.second;
        }

        for (int i = 0; i < n; ++i)
        {
            float a = l[i], b = r[i];
            if (lc > 0.0)
            {
                a = lowcut.process (a, 0);
                b = lowcut.process (b, 1);
            }
            if (vk > 0)
            {
                a = voicePre.process (a, 0);
                b = voicePre.process (b, 1);
            }
            if (tape)
            {
                a = emphasis.process (a, 0);
                b = emphasis.process (b, 1);
            }
            wl[(std::size_t) i] = a * drvA2;
            wr[(std::size_t) i] = b * drvB2;
        }

        // round 128: band 1's power, mute, feedback, dynamics and tone (single-band mode)
        auto& ex = extras[0];
        ex.configure (p, 0, sr, factor);
        const bool powered = on (p[bandExtra (0, 0)]);
        const bool muted = bandState (p[bandExtra (0, 1)]) == BandState::mute;
        if (chr.k > 0)
        {
            if (ex.fbGain > 0.0)
            {
                auto left = [&ex, &chr] (float x) noexcept { return ex.feedbackShape ([&chr] (float u) noexcept { return chr.curve (u); }, x, 0); };
                auto right = [&ex, &chr] (float x) noexcept { return ex.feedbackShape ([&chr] (float u) noexcept { return chr.curve (u); }, x, 1); };
                os[0].process (wl.data(), n, left);
                os[1].process (wr.data(), n, right);
            }
            else
            {
                auto curve = [&chr] (float x) noexcept { return chr.curve (x); };
                os[0].process (wl.data(), n, curve);
                os[1].process (wr.data(), n, curve);
            }
        }
        else if (ex.fbGain > 0.0)
        {
            auto left = [&ex, s] (float x) noexcept { return ex.shapeWithFeedback (s, x, 0); };
            auto right = [&ex, s] (float x) noexcept { return ex.shapeWithFeedback (s, x, 1); };
            os[0].process (wl.data(), n, left);
            os[1].process (wr.data(), n, right);
        }
        else
        {
            auto curve = [s] (float x) noexcept { return shape (s, x); };
            os[0].process (wl.data(), n, curve);
            os[1].process (wr.data(), n, curve);
        }
        const bool post = ex.toneOn || ex.dyn != 0.0;
        const bool collapse = chr.k == collapseCharacter && measuredEffects (p); // round 152
        const auto cRatio = collapse ? collapseRatio (referenceDrive (driveDb (p[drive]))) : 1.0;
        // round 152 (with the measured law): the single band applies band 1's mix and level like the reference (before,
        // they only acted in the multiband mode)
        const bool bandLaw = measuredEffects (p);
        const auto bandMix = std::clamp (p[band1Mix], 0.0f, 1.0f);
        const auto bandGain = (float) dbToGain (bandLevelDb (p[band1Level]));

        for (int i = 0; i < n; ++i)
        {
            auto a = dc.process (wl[(std::size_t) i], 0) * compA2, b = dc.process (wr[(std::size_t) i], 1) * compB;
            if (chr.k > 0) chr.post (a, b);
            if (collapse) pitch[0].run (a, b, cRatio); // round 152
            if (tape)
            {
                a = deemphasis.process (a, 0);
                b = deemphasis.process (b, 1);
            }
            if (post) ex.post (a, b, l[i], r[i]);
            if (vk > 0)
            {
                a = voicePost.process (a, 0);
                b = voicePost.process (b, 1);
            }
            a = tiltHi.process (tiltLo.process (a, 0), 0);
            b = tiltHi.process (tiltLo.process (b, 1), 1);
            if (hc > 0.0)
            {
                a = applyHighCut (a, 0);
                b = applyHighCut (b, 1);
            }
            float da = l[i], db = r[i];
            dry.process (da, db);
            if (bandLaw) // round 152: band 1's own mix and level, as the reference's single band
            {
                a = (da + (a - da) * bandMix) * bandGain;
                b = (db + (b - db) * bandMix) * bandGain;
            }
            if (! powered) { a = da; b = db; }  // switched off: the band passes clean
            if (muted) { a = 0.0f; b = 0.0f; }
            a *= panA; // round 147: the band's pan
            b *= panB;
            l[i] = (da + (a - da) * mx) * out;
            r[i] = (db + (b - db) * mx) * out;
        }
        meters.set (0.0f, 0.0f, inPeak, Meters::peak (l, r, n));
    }

    // Multiband mode (REA_rodada108): Linkwitz-Riley bands (the lower ones through the higher splits' all-passes, so
    // the bands sum flat), each with its own style, drive, mix and level, oversampled separately; the cuts, tone,
    // global mix and output as in the single-band mode.
    void chunkBands (float* l, float* r, int n, const EffectParams& p) noexcept
    {
        const auto inPeak = Meters::peak (l, r, n);
        const int nb = activeBands (p);
        const auto factor = oversampling (p[quality]);
        // round 150: up to five crossovers (each at least 5 % above the one below)
        std::array<double, maxBands - 1> f {};
        for (int k = 0; k < maxBands - 1; ++k)
        {
            f[(std::size_t) k] = crossoverHz (p[(std::size_t) crossoverParam (k)]);
            if (k > 0) f[(std::size_t) k] = std::max (f[(std::size_t) k], f[(std::size_t) k - 1] * 1.05);
        }
        // round 152: each crossover its own slope (or the global one)
        std::array<int, maxBands - 1> slope {};
        for (int k = 0; k < maxBands - 1; ++k)
            slope[(std::size_t) k] = crossoverSlopeDbOct (p, k);
        const bool linear = linearPhase (p);
        if (linear && (f != linHz || slope != linSlopes || nb != linBands || ! linDesigned))
            designLinear (f, slope, nb);
        if (f != splitHz || slope != splitSlope || nb != splitBands)
        {
            for (int k = 0; k < maxBands - 1; ++k)
            {
                split[(std::size_t) k].design (sr, f[(std::size_t) k], slope[(std::size_t) k]);
                for (int j = 0; j < k; ++j) // the bands below split k pass its all-pass copy, so the sum stays flat
                    ap[(std::size_t) k][(std::size_t) j].design (sr, f[(std::size_t) k], slope[(std::size_t) k]);
            }
            if (nb != splitBands)
            {
                for (auto& x : split) x.reset();
                for (auto& row : ap)
                    for (auto& x : row) x.reset();
                for (auto& d : bandDry)
                    d.reset();
            }
            splitHz = f;
            splitSlope = slope;
            splitBands = nb;
        }
        const auto [lc, hc] = configureCuts (p);
        const auto tilt = toneTilt (p[tone]);
        tiltLo.shelf (sr, 1000.0, -6.0 * tilt, false, 0.5);
        tiltHi.shelf (sr, 1000.0, 6.0 * tilt, true, 0.5);
        const auto mx = std::clamp (p[mix], 0.0f, 1.0f);
        const auto out = (float) dbToGain (lin (p[output], -18.0, 18.0));
        const bool autoOn = on (p[autoGain]);
        const auto punishGain = (float) (on (p[punish]) ? dbToGain (punishDb) : 1.0); // round 135

        std::array<Style, maxBands> st {};
        std::array<float, maxBands> drv {}, bmix {}, lvl {}, comp {};
        std::array<float, maxBands> drvB {}, compB {}, panA {}, panB {}; // round 147: channel 1's drive, the band pan
        bool anySolo = false;
        for (int b = 0; b < nb; ++b) anySolo = anySolo || bandState (p[(std::size_t) bandExtra (b, 1)]) == BandState::solo;
        for (int b = 0; b < nb; ++b)
        {
            st[(std::size_t) b] = styleOf (p[(std::size_t) bandParam (b, 0)]);
            drv[(std::size_t) b] = (float) dbToGain (driveDb (p[(std::size_t) bandParam (b, 1)])) * punishGain;
            bmix[(std::size_t) b] = std::clamp (p[(std::size_t) bandParam (b, 2)], 0.0f, 1.0f);
            lvl[(std::size_t) b] = (float) dbToGain (bandLevelDb (p[(std::size_t) bandParam (b, 3)]));
            comp[(std::size_t) b] = autoOn ? autoMakeup (st[(std::size_t) b], drv[(std::size_t) b]) : 1.0f;
            // round 147: drive pan (channel 0's drive stays in drv) and band pan
            const auto [dsA, dsB] = panScales (panOf (p[(std::size_t) bandDrivePanParam (b)]));
            const auto dDb = driveDb (p[(std::size_t) bandParam (b, 1)]);
            drvB[(std::size_t) b] = drv[(std::size_t) b];
            compB[(std::size_t) b] = comp[(std::size_t) b];
            if (dsA != 1.0f || dsB != 1.0f)
            {
                drv[(std::size_t) b] = (float) dbToGain (dDb * dsA) * punishGain;
                drvB[(std::size_t) b] = (float) dbToGain (dDb * dsB) * punishGain;
                comp[(std::size_t) b] = autoOn ? autoMakeup (st[(std::size_t) b], drv[(std::size_t) b]) : 1.0f;
                compB[(std::size_t) b] = autoOn ? autoMakeup (st[(std::size_t) b], drvB[(std::size_t) b]) : 1.0f;
            }
            // round 150: a character replaces the style's curve, its drive law and the level (no auto gain on top)
            chars[(std::size_t) b].configure (characterIndex (p[(std::size_t) characterParam (b)]), sr);
            chars[(std::size_t) b].law = measuredEffects (p); // round 152
            if (chars[(std::size_t) b].k > 0)
            {
                const auto extra = on (p[punish]) ? punishDb : 0.0;
                const auto ga = chars[(std::size_t) b].gains (dDb * dsA, extra), gb = chars[(std::size_t) b].gains (dDb * dsB, extra);
                drv[(std::size_t) b] = ga.first;
                comp[(std::size_t) b] = ga.second;
                drvB[(std::size_t) b] = gb.first;
                compB[(std::size_t) b] = gb.second;
            }
            const auto [pa, pb] = panScales (panOf (p[(std::size_t) bandPanParam (b)]));
            panA[(std::size_t) b] = pa;
            panB[(std::size_t) b] = pb;
            bandOs[(std::size_t) b][0].setFactor (factor);
            bandOs[(std::size_t) b][1].setFactor (factor);
        }

        for (int i = 0; i < n && linear; ++i)
        {
            // round 152: linear phase — band k < top = the zero-phase FIR of its share, the top band = the delayed
            // input minus the others (so the bands sum to the delayed input exactly); the dry input is delayed alike
            float a = l[i], b = r[i];
            if (lc > 0.0)
            {
                a = lowcut.process (a, 0);
                b = lowcut.process (b, 1);
            }
            float ta = a, tb = b;
            linTop.process (ta, tb);
            for (int k = 0; k < nb - 1; ++k)
            {
                float ca = a, cb = b;
                linConv[(std::size_t) k].step (ca, cb);
                band (k, 0, i) = ca;
                band (k, 1, i) = cb;
                ta -= ca;
                tb -= cb;
            }
            band (nb - 1, 0, i) = ta;
            band (nb - 1, 1, i) = tb;
            linDry.process (l[i], r[i]);
        }
        for (int i = 0; i < n && ! linear; ++i)
        {
            float x[2] { l[i], r[i] };
            for (int c = 0; c < 2; ++c)
            {
                auto v = x[c];
                if (lc > 0.0)
                    v = lowcut.process (v, c);
                // the bands from the bottom up: split k cuts the rest in two, the bands already cut pass its all-pass
                // (the same filters in the same order as the four-band version before round 150)
                auto rest = v;
                for (int k = 0; k < nb - 1; ++k)
                {
                    float lo, hi;
                    split[(std::size_t) k].run (rest, c, lo, hi);
                    for (int j = 0; j < k; ++j)
                        band (j, c, i) = ap[(std::size_t) k][(std::size_t) j].allpass (band (j, c, i), c);
                    band (k, c, i) = lo;
                    rest = hi;
                }
                band (nb - 1, c, i) = rest;
            }
        }

        // each band: keep its dry copy (delayed like the oversampled path), drive and shape it
        for (int b = 0; b < nb; ++b)
        {
            auto* bl = bandBuf[(std::size_t) b][0].data();
            auto* br = bandBuf[(std::size_t) b][1].data();
            for (int i = 0; i < n; ++i)
            {
                float da = bl[i], db = br[i];
                bandDry[(std::size_t) b].process (da, db);
                dryBand[0][(std::size_t) i] = da;
                dryBand[1][(std::size_t) i] = db;
                bl[i] *= drv[(std::size_t) b];
                br[i] *= drvB[(std::size_t) b];
            }
            const auto s = st[(std::size_t) b];
            // round 128: the band's feedback, dynamics, tone, power and solo / mute
            auto& ex = extras[(std::size_t) b];
            ex.configure (p, b, sr, factor);
            auto& chr = chars[(std::size_t) b];
            const bool collapse = chr.k == collapseCharacter && measuredEffects (p);
            const auto ratio = collapse ? collapseRatio (referenceDrive (driveDb (p[(std::size_t) bandParam (b, 1)]))) : 1.0;
            if (chr.k > 0)
            {
                if (ex.fbGain > 0.0)
                {
                    auto left = [&ex, &chr] (float v) noexcept { return ex.feedbackShape ([&chr] (float u) noexcept { return chr.curve (u); }, v, 0); };
                    auto right = [&ex, &chr] (float v) noexcept { return ex.feedbackShape ([&chr] (float u) noexcept { return chr.curve (u); }, v, 1); };
                    bandOs[(std::size_t) b][0].process (bl, n, left);
                    bandOs[(std::size_t) b][1].process (br, n, right);
                }
                else
                {
                    auto curve = [&chr] (float v) noexcept { return chr.curve (v); };
                    bandOs[(std::size_t) b][0].process (bl, n, curve);
                    bandOs[(std::size_t) b][1].process (br, n, curve);
                }
            }
            else if (ex.fbGain > 0.0)
            {
                auto left = [&ex, s] (float v) noexcept { return ex.shapeWithFeedback (s, v, 0); };
                auto right = [&ex, s] (float v) noexcept { return ex.shapeWithFeedback (s, v, 1); };
                bandOs[(std::size_t) b][0].process (bl, n, left);
                bandOs[(std::size_t) b][1].process (br, n, right);
            }
            else
            {
                auto curve = [s] (float v) noexcept { return shape (s, v); };
                bandOs[(std::size_t) b][0].process (bl, n, curve);
                bandOs[(std::size_t) b][1].process (br, n, curve);
            }
            const auto m = bmix[(std::size_t) b], g = lvl[(std::size_t) b], k = comp[(std::size_t) b], kB = compB[(std::size_t) b];
            const auto gA = g * panA[(std::size_t) b], gB = g * panB[(std::size_t) b];
            const bool post = ex.toneOn || ex.dyn != 0.0;
            const bool powered = on (p[(std::size_t) bandExtra (b, 0)]);
            const auto state = bandState (p[(std::size_t) bandExtra (b, 1)]);
            const bool silent = state == BandState::mute || (anySolo && state != BandState::solo);
            for (int i = 0; i < n; ++i)
            {
                auto wa = bandDc[(std::size_t) b].process (bl[i], 0) * k, wb = bandDc[(std::size_t) b].process (br[i], 1) * kB;
                if (chr.k > 0) chr.post (wa, wb);
                if (collapse) pitch[(std::size_t) b].run (wa, wb, ratio); // round 152
                if (post) ex.post (wa, wb, dryBand[0][(std::size_t) i], dryBand[1][(std::size_t) i]);
                auto ya = (dryBand[0][(std::size_t) i] + (wa - dryBand[0][(std::size_t) i]) * m) * gA;
                auto yb = (dryBand[1][(std::size_t) i] + (wb - dryBand[1][(std::size_t) i]) * m) * gB;
                if (! powered) { ya = dryBand[0][(std::size_t) i]; yb = dryBand[1][(std::size_t) i]; }
                if (silent) { ya = 0.0f; yb = 0.0f; }
                if (b == 0)
                {
                    wl[(std::size_t) i] = ya;
                    wr[(std::size_t) i] = yb;
                }
                else
                {
                    wl[(std::size_t) i] += ya;
                    wr[(std::size_t) i] += yb;
                }
            }
        }

        for (int i = 0; i < n; ++i)
        {
            auto a = tiltHi.process (tiltLo.process (wl[(std::size_t) i], 0), 0);
            auto b = tiltHi.process (tiltLo.process (wr[(std::size_t) i], 1), 1);
            if (hc > 0.0)
            {
                a = applyHighCut (a, 0);
                b = applyHighCut (b, 1);
            }
            float da = l[i], db = r[i];
            dry.process (da, db);
            l[i] = (da + (a - da) * mx) * out;
            r[i] = (db + (b - db) * mx) * out;
        }
        meters.set (0.0f, 0.0f, inPeak, Meters::peak (l, r, n));
    }

    float& band (int b, int c, int i) noexcept { return bandBuf[(std::size_t) b][(std::size_t) c][(std::size_t) i]; }

    // round 135: the low / high cut of either range. Old range: 12 dB/oct both; THUMP gives the low cut a resonance,
    // STEEP makes the high cut 30 dB/oct (5th-order Butterworth), the wide range's gentle high cut is 6 dB/oct
    // [EXEC trial: gentle ≈ 6 dB/oct, steep ≈ 30 dB/oct, low cut 12 dB/oct, Thump +3.5 dB at the cutoff]
    std::pair<double, double> configureCuts (const EffectParams& p) noexcept
    {
        const bool wide = on (p[wideCuts]);
        const auto lc = wide ? lowCutWideHz (p[lowCutWide]) : lowCutHz (p[lowCut]);
        const auto hc = wide ? highCutWideHz (p[highCutWide]) : highCutHz (p[highCut]);
        if (lc > 0.0)
            lowcut.filter (sr, 1, lc, on (p[thump]) ? thumpQ : 0.70710678);
        hcMode = on (p[steep]) ? 2 : (wide ? 1 : 0);
        if (hc > 0.0)
        {
            const auto f = std::min (hc, sr * 0.45);
            if (hcMode == 2)
            {
                highcut.filter (sr, 0, f, 1.0 / (2.0 * std::cos (3.14159265358979323846 / 5.0)));        // Q 0.618
                highcut2.filter (sr, 0, f, 1.0 / (2.0 * std::cos (2.0 * 3.14159265358979323846 / 5.0))); // Q 1.618
            }
            else if (hcMode == 0)
                highcut.filter (sr, 0, f, 0.70710678);
            const auto k = std::tan (3.14159265358979323846 * f / sr); // first-order section (bilinear)
            highcut1.set (k, k, 0.0, 1.0 + k, k - 1.0, 0.0);
        }
        return { lc, hc };
    }

    float applyHighCut (float x, int c) noexcept
    {
        if (hcMode == 1) return highcut1.process (x, c);
        x = highcut.process (x, c);
        if (hcMode == 2) x = highcut1.process (highcut2.process (x, c), c);
        return x;
    }

    // a voicing's EQ before and after its curve (small, broad shelves: the five references differ mostly there)
    void configureVoice (int k) noexcept
    {
        if (k <= 0 || k == lastVoice) return;
        lastVoice = k;
        switch (k)
        {
            case 1: voicePre.shelf (sr, 8000.0, 1.0, true, 0.7); voicePost.shelf (sr, 60.0, 0.0, false, 0.7); break;  // tape pre: open top
            case 2: voicePre.shelf (sr, 7000.0, 2.5, true, 0.7); voicePost.shelf (sr, 50.0, 1.0, false, 0.7); break;  // console: airy, firm bass
            case 3: voicePre.shelf (sr, 6000.0, -2.5, true, 0.7); voicePost.shelf (sr, 80.0, 0.0, false, 0.7); break; // grit: darker top
            case 4: voicePre.shelf (sr, 60.0, -3.0, false, 0.7); voicePost.shelf (sr, 10000.0, 0.5, true, 0.7); break; // triode: thinner bass
            default: voicePre.peaking (sr, 100.0, 0.8, 2.5); voicePost.shelf (sr, 1000.0, 0.0, true, 0.7); break;     // pentode: bass bump
        }
    }

    static constexpr int maxBlock = 2048;
    // level compensation: the RMS gain of the curve on a −12 dBFS sine at this drive, inverted
    static float autoMakeup (Style s, float drv) noexcept
    {
        double in = 0.0, out = 0.0, mean = 0.0;
        constexpr int points = 64;
        std::array<double, points> ys {};
        for (int k = 0; k < points; ++k)
        {
            const auto x = 0.25 * std::sin (2.0 * pi * (k + 0.5) / points);
            ys[(std::size_t) k] = shape (s, (float) (x * drv));
            mean += ys[(std::size_t) k];
            in += x * x;
        }
        mean /= points;
        for (auto y : ys)
            out += (y - mean) * (y - mean);
        return out > 1e-12 ? (float) std::sqrt (in / out) : 1.0f;
    }

    double sr = 44100.0;
    std::array<Oversampler, 2> os;
    Delay2<256> dry;
    Biquad lowcut, emphasis, deemphasis, tiltLo, tiltHi, highcut;
    Biquad highcut2, highcut1, voicePre, voicePost; // round 135
    int hcMode = 0, lastVoice = -1;
    OnePoleHp dc;
    std::array<float, maxBlock> wl {}, wr {};
    float makeup = 1.0f, lastDrive = -1.0f;
    Style lastStyle = Style::tube;
    // round 136: the Classic voicing
    emberclassic::Engine classic;
    emberclassic::Settings classicSet;
    bool classicOn = false;

    void chunkClassic (float* l, float* r, int n, const EffectParams& p) noexcept
    {
        emberclassic::Settings c;
        c.style = emberclassic::styleOf (p[classicStyle]);
        c.drive = emberclassic::driveUnits (p[classicDrive]);
        c.punish = on (p[classicPunish]);
        c.lowCutHz = emberclassic::lowCutHz (p[classicLowCut]);
        c.thump = on (p[classicThump]);
        c.toneDb = emberclassic::toneDb (p[classicTone]);
        c.highCutHz = emberclassic::highCutHz (p[classicHighCut]);
        c.steep = on (p[classicSteep]);
        c.mix = std::clamp ((double) p[classicMix], 0.0, 1.0);
        c.outputDb = emberclassic::outputDb (p[classicOutput]);
        c.measuredPunish = on (p[classicLaw]);
        if (! classicOn || ! (c == classicSet))
        {
            if (! classicOn)
                classic.reset();
            classic.set (c);
            classicSet = c;
            classicOn = true;
        }
        const auto inPeak = Meters::peak (l, r, n);
        classic.process (l, r, n);
        meters.set (0.0f, 0.0f, inPeak, Meters::peak (l, r, n));
    }
    // multiband mode
    std::array<LrSplit, maxBands - 1> split;
    std::array<std::array<LrSplit, maxBands - 2>, maxBands - 1> ap; // ap[k][j]: split k's all-pass for band j < k
    std::array<double, maxBands - 1> splitHz {};
    std::array<int, maxBands - 1> splitSlope {};
    int splitBands = 0;
    std::array<std::array<Oversampler, 2>, maxBands> bandOs;
    std::array<Delay2<256>, maxBands> bandDry;
    std::array<OnePoleHp, maxBands> bandDc;
    std::array<std::array<std::array<float, maxBlock>, 2>, maxBands> bandBuf {};
    std::array<std::array<float, maxBlock>, 2> dryBand {};
    std::array<BandExtras, maxBands> extras;
    std::array<CharacterStage, maxBands> chars; // round 150
    // round 152: linear phase, the modulation, the pitch drop
    std::array<lynx::peqfl::FftConvolver, maxBands - 1> linConv;
    std::vector<float> linTable;
    LongDelay linDry, linTop;
    int linSize = 0, linLatency = 0, linBands = 0;
    std::array<double, maxBands - 1> linHz {};
    std::array<int, maxBands - 1> linSlopes {};
    bool linDesigned = false;
    std::atomic<bool> linOn { false };
    std::array<PitchDown, maxBands> pitch;
    std::array<double, 2> lfoPhase {};
    double envPower = 0.0;
    EffectParams pm {};

    // the FIR of band k < top: lo_k · Π_{j<k} (1 − lo_j), lo = 1 / (1 + (f / fc)^(slope / 6)) on the analog axis
    // [EXEC: the trial's linear-phase bands, every slope, 0.00 dB]
    void designLinear (const std::array<double, maxBands - 1>& f, const std::array<int, maxBands - 1>& slope, int nb) noexcept
    {
        linHz = f;
        linSlopes = slope;
        linBands = nb;
        linDesigned = true;
        if (linSize <= 0) return;
        const auto bins = linSize / 2 + 1;
        for (int k = 0; k < nb - 1; ++k)
        {
            for (int i = 0; i < bins; ++i)
            {
                const auto hz = (double) i * sr / (double) linSize;
                double m = linearLow (hz, f[(std::size_t) k], slope[(std::size_t) k]);
                for (int j = 0; j < k; ++j)
                    m *= 1.0 - linearLow (hz, f[(std::size_t) j], slope[(std::size_t) j]);
                linTable[(std::size_t) i] = (float) m;
            }
            linConv[(std::size_t) k].setKernel (linTable);
        }
    }
    Meters meters;
};
} // namespace
} // namespace ember

// ================================================================== registry

const EffectInfo& ridgeInfo()
{
    static const EffectInfo info { "Ridge Strip",
                                   { { "Input", 0.5f }, { "HPF", 0.0f }, { "LPF", 0.0f },
                                     { "LF gain", 0.5f }, { "LF freq.", 0.40f }, { "LF bell", 0.0f },
                                     { "LMF gain", 0.5f }, { "LMF freq.", 0.43f }, { "LMF Q", 0.43f },
                                     { "HMF gain", 0.5f }, { "HMF freq.", 0.49f }, { "HMF Q", 0.43f },
                                     { "HF gain", 0.5f }, { "HF freq.", 0.70f }, { "HF bell", 0.0f },
                                     { "EQ curve", 0.0f }, { "EQ on", 1.0f },
                                     { "Ratio", 0.0f }, { "Comp. threshold", 1.0f }, { "Comp. release", 0.29f }, { "Fast attack", 0.0f },
                                     { "Gate range", 0.0f }, { "Gate threshold", 0.25f }, { "Gate release", 0.29f }, { "Gate fast", 0.0f },
                                     { "Expander", 0.0f }, { "Dynamics on", 1.0f },
                                     { "Order", 1.0f }, { "Filters to SC", 0.0f }, { "Output", 0.5f }, { "Phase", 0.0f }, { "Comp. mix", 1.0f },
                                     { "Voicing", 0.0f }, { "Filter split", 0.0f },
                                     // round 145: the Vocal strip voicing (core/VoxStrip.h); defaults = the reference's
                                     { "Vocal strip", 0.0f }, { "Vocal input", 0.75f }, { "Vocal output", 0.75f }, { "Process order", 0.0f },
                                     { "De-esser", 0.0f }, { "De-ess freq.", 0.672591f }, { "De-ess threshold", 0.5f },
                                     { "De-ess split", 1.0f }, { "De-ess lookahead", 0.0f }, { "De-ess listen", 0.0f },
                                     { "De-ploser", 0.0f }, { "De-plos freq.", 0.709403f }, { "De-plos threshold", 0.5f },
                                     { "De-plos split", 1.0f }, { "De-plos lookahead", 0.0f }, { "De-plos listen", 0.0f },
                                     { "Vocal EQ", 0.0f }, { "Vocal HPF", 1.0f }, { "Vocal HPF freq.", 0.0f },
                                     { "Notch", 1.0f }, { "Notch freq.", 0.0f }, { "Notch gain", 0.75f },
                                     { "High band", 1.0f }, { "High freq.", 0.0f }, { "High gain", 0.5f },
                                     { "Compander", 0.0f }, { "Expander threshold", 0.0f }, { "Compander threshold", 1.0f },
                                     { "Compander ratio", 0.504899f }, { "Compander attack", 0.0f }, { "Compander release", 0.049762f },
                                     { "Make-up", 0.0f }, { "Soft knee", 0.0f }, { "Drive", 0.0f },
                                     // round 147b
                                     { "Auto make-up", 0.0f }, { "Analog", 0.0f },
                                     // round 148: the 4K voicing
                                     { "4K voicing", 0.0f }, { "EQ colour", 0.0f }, { "Mic pre", 0.0f }, { "Mic gain", 0.0f },
                                     { "Make-up offset", 0.5f },
                                     // round 165
                                     { "EQ to SC", 0.0f } } };
    return info;
}

const EffectInfo& glowInfo()
{
    static const EffectInfo info { "Glow Leveler",
                                   // round 152: a new instance starts as the reference does (the measured Opto classic
                                   // voicing, Gain 40, Peak reduction 46, Hi Freq 50)
                                   { { "Peak reduction", 0.46f }, { "Gain", 0.40f }, { "Limit", 0.0f }, { "Emphasis", 0.5f }, { "Mix", 1.0f },
                                     { "Trim", 0.5f }, { "Auto makeup", 0.0f }, { "Voicing", 1.0f },
                                     { "Opto law", 1.0f }, { "Analog", 0.0f },
                                     // round 165w: the measured release (a project saved before keeps round 123's)
                                     { "Opto release", 1.0f },
                                     // round 165x: the dial where Auto makeup was switched on (0.46 = round 152's)
                                     { "Makeup ref", 0.46f },
                                     // round 165x: what the VU shows (0 gain reduction, 0.5 input, 1 output), saved
                                     { "VU display", 0.0f } } };
    return info;
}

const EffectInfo& hushInfo()
{
    static const EffectInfo info { "Hush De-esser",
                                   // round 165x: 6.02 kHz on the extended law's 2 … 20 kHz scale (0.53 on the old 2 … 16 kHz)
                                   { { "Frequency", 0.4786f }, { "Threshold", 0.5f }, { "Range", 0.5f }, { "Wideband", 0.0f },
                                     { "Band detector", 0.0f }, { "Listen", 0.0f }, { "Lookahead", 1.0f }, { "Release", 0.56f },
                                     { "Output", 0.5f }, { "Voicing", 0.0f }, // round 123 (vmatch123)
                                     // round 128 (parity with the reference de-esser)
                                     { "Allround", 0.0f }, { "Low-pass freq.", 1.0f }, { "Lookahead time", 2.0f / 15.0f },
                                     { "Stereo link", 1.0f }, { "Link mid", 0.0f }, { "External SC", 0.0f }, { "Input", 0.5f },
                                     { "Display", 1.0f },
                                     { "Oversampling", 0.0f }, // round 135
                                     { "Vocal voicing", 0.0f }, // round 148
                                     { "Input pan", 0.5f }, { "Output pan", 0.5f }, // round 165w
                                     // round 165x: the reference's ranges and link (a project saved before: the old law)
                                     { "Range law", 1.0f }, { "Link only", 0.0f }, { "Link only side", 0.0f } } };
    return info;
}

const EffectInfo& aetherInfo()
{
    static const EffectInfo info { "Aether",
                                   { { "Air", 0.0f }, { "Air freq.", 0.62f }, { "Harmonics", 0.0f }, { "Exciter freq.", 0.56f },
                                     { "Character", 0.5f }, { "Mix", 1.0f }, { "Output", 0.5f },
                                     { "Dynamic", 0.0f }, { "Mid air", 0.0f }, { "High air", 0.0f } } };
    return info;
}

const EffectInfo& emberInfo()
{
    static const EffectInfo info { "Ember Saturator",
                                   { { "Drive", 0.0f }, { "Style", 0.0f }, { "Tone", 0.5f }, { "Low cut", 0.0f }, { "High cut", 1.0f },
                                     { "Mix", 1.0f }, { "Output", 0.5f }, { "Auto gain", 1.0f }, { "Oversampling", 0.5f },
                                     { "Bands", 0.0f }, { "Crossover 1", 0.3266f }, { "Crossover 2", 0.6574f }, { "Crossover 3", 0.9375f },
                                     { "Crossover slope", 0.5f }, { "B1 mix", 1.0f }, { "B1 level", 0.5f },
                                     { "B2 style", 0.0f }, { "B2 drive", 0.0f }, { "B2 mix", 1.0f }, { "B2 level", 0.5f },
                                     { "B3 style", 0.0f }, { "B3 drive", 0.0f }, { "B3 mix", 1.0f }, { "B3 level", 0.5f },
                                     { "B4 style", 0.0f }, { "B4 drive", 0.0f }, { "B4 mix", 1.0f }, { "B4 level", 0.5f },
                                     // round 128 (Saturn parity)
                                     { "Input", 0.5f },
                                     { "B1 on", 1.0f }, { "B1 state", 0.0f }, { "B1 bass", 0.5f }, { "B1 mid", 0.5f }, { "B1 treble", 0.5f }, { "B1 presence", 0.5f },
                                     { "B1 feedback", 0.0f }, { "B1 fb freq.", 0.6990f }, { "B1 dynamics", 0.5f },
                                     { "B2 on", 1.0f }, { "B2 state", 0.0f }, { "B2 bass", 0.5f }, { "B2 mid", 0.5f }, { "B2 treble", 0.5f }, { "B2 presence", 0.5f },
                                     { "B2 feedback", 0.0f }, { "B2 fb freq.", 0.6990f }, { "B2 dynamics", 0.5f },
                                     { "B3 on", 1.0f }, { "B3 state", 0.0f }, { "B3 bass", 0.5f }, { "B3 mid", 0.5f }, { "B3 treble", 0.5f }, { "B3 presence", 0.5f },
                                     { "B3 feedback", 0.0f }, { "B3 fb freq.", 0.6990f }, { "B3 dynamics", 0.5f },
                                     { "B4 on", 1.0f }, { "B4 state", 0.0f }, { "B4 bass", 0.5f }, { "B4 mid", 0.5f }, { "B4 treble", 0.5f }, { "B4 presence", 0.5f },
                                     { "B4 feedback", 0.0f }, { "B4 fb freq.", 0.6990f }, { "B4 dynamics", 0.5f },
                                     // round 135 (the Decapitator role)
                                     { "Punish", 0.0f }, { "Thump", 0.0f }, { "Steep", 0.0f }, { "Voice", 0.0f }, { "Wide cuts", 0.0f },
                                     { "Low cut (wide)", 0.0f }, { "High cut (wide)", 1.0f },
                                     // round 136: the Classic voicing (EmberClassic.h)
                                     { "Voicing", 0.0f }, { "Classic style", 0.0f }, { "Classic drive", 0.0f }, { "Classic punish", 0.0f },
                                     { "Classic low cut", 0.0f }, { "Classic thump", 0.0f }, { "Classic tone", 0.5f }, { "Classic high cut", 1.0f },
                                     { "Classic steep", 0.0f }, { "Classic mix", 1.0f }, { "Classic output", 1.0f },
                                     // round 147 (Saturn 2's channel mode, drive pan and band pan)
                                     { "Channel mode", 0.0f },
                                     { "B1 drive pan", 0.5f }, { "B2 drive pan", 0.5f }, { "B3 drive pan", 0.5f }, { "B4 drive pan", 0.5f },
                                     { "B1 pan", 0.5f }, { "B2 pan", 0.5f }, { "B3 pan", 0.5f }, { "B4 pan", 0.5f },
                                     // round 150 (Saturn 2: bands 5 / 6, input / output pan, the measured characters)
                                     { "More bands", 0.0f }, { "Crossover 4", 0.97f }, { "Crossover 5", 0.99f },
                                     { "B5 style", 0.0f }, { "B5 drive", 0.0f }, { "B5 mix", 1.0f }, { "B5 level", 0.5f },
                                     { "B6 style", 0.0f }, { "B6 drive", 0.0f }, { "B6 mix", 1.0f }, { "B6 level", 0.5f },
                                     { "B5 on", 1.0f }, { "B5 state", 0.0f }, { "B5 bass", 0.5f }, { "B5 mid", 0.5f }, { "B5 treble", 0.5f }, { "B5 presence", 0.5f },
                                     { "B5 feedback", 0.0f }, { "B5 fb freq.", 0.6990f }, { "B5 dynamics", 0.5f },
                                     { "B6 on", 1.0f }, { "B6 state", 0.0f }, { "B6 bass", 0.5f }, { "B6 mid", 0.5f }, { "B6 treble", 0.5f }, { "B6 presence", 0.5f },
                                     { "B6 feedback", 0.0f }, { "B6 fb freq.", 0.6990f }, { "B6 dynamics", 0.5f },
                                     { "B5 drive pan", 0.5f }, { "B6 drive pan", 0.5f }, { "B5 pan", 0.5f }, { "B6 pan", 0.5f },
                                     { "Input pan", 0.5f }, { "Output pan", 0.5f },
                                     { "B1 character", 0.0f }, { "B2 character", 0.0f }, { "B3 character", 0.0f },
                                     { "B4 character", 0.0f }, { "B5 character", 0.0f }, { "B6 character", 0.0f },
                                     // round 152 (Saturn 2: own crossover slopes, processing, modulation, effect law)
                                     { "Slope 1", 0.0f }, { "Slope 2", 0.0f }, { "Slope 3", 0.0f }, { "Slope 4", 0.0f }, { "Slope 5", 0.0f },
                                     { "Processing", 0.0f },
                                     { "LFO 1 rate", 0.248f }, { "LFO 1 shape", 0.0f }, { "LFO 2 rate", 0.248f }, { "LFO 2 shape", 0.0f },
                                     { "Env attack", 0.0f }, { "Env release", 0.0f },
                                     { "Mod 1 source", 0.0f }, { "Mod 1 target", 0.0f }, { "Mod 1 amount", 0.5f },
                                     { "Mod 2 source", 0.0f }, { "Mod 2 target", 0.0f }, { "Mod 2 amount", 0.5f },
                                     { "Mod 3 source", 0.0f }, { "Mod 3 target", 0.0f }, { "Mod 3 amount", 0.5f },
                                     { "Mod 4 source", 0.0f }, { "Mod 4 target", 0.0f }, { "Mod 4 amount", 0.5f },
                                     { "Effect law", 1.0f },
                                     // round 165 (Decapitator: Auto, the Classic law)
                                     { "Classic auto", 1.0f }, { "Classic law", 1.0f } } };
    return info;
}

std::unique_ptr<Effect> makeHush (const EffectParams* params) { return std::make_unique<hush::DeEsser> (params); }

bool isSuiteKind (EffectKind kind) noexcept
{
    return kind == EffectKind::ridgeStrip || kind == EffectKind::glowLeveler || kind == EffectKind::hushDeEsser
           || kind == EffectKind::aether || kind == EffectKind::ember;
}

std::unique_ptr<Effect> make (EffectKind kind)
{
    switch (kind)
    {
        case EffectKind::ridgeStrip: return std::make_unique<ridge::Strip>();
        case EffectKind::glowLeveler: return std::make_unique<glow::Leveler>();
        case EffectKind::hushDeEsser: return std::make_unique<hush::DeEsser>();
        case EffectKind::aether: return std::make_unique<aether::Air>();
        case EffectKind::ember: return std::make_unique<ember::Saturator>();
        default: return nullptr;
    }
}

std::unique_ptr<Effect> makeEmber (const EffectParams* params) { return std::make_unique<ember::Saturator> (params); }

namespace
{
std::string hzText (double f)
{
    char t[32];
    if (f >= 1000.0)
        std::snprintf (t, sizeof (t), "%.2f kHz", f / 1000.0);
    else
        std::snprintf (t, sizeof (t), "%.0f Hz", f);
    return t;
}

std::string fmt (const char* f, double v)
{
    char t[32];
    std::snprintf (t, sizeof (t), f, v);
    return t;
}

std::string onOff (float v) { return on (v) ? "on" : "off"; }
} // namespace

namespace glow
{

int vuModeOf (float v) noexcept { return v < 0.25f ? 0 : v < 0.75f ? 1 : 2; }

std::string classicParamText (int param, float v)
{
    switch (param)
    {
        case peakReduction: case gain: case emphasis: return fmt ("%.1f", v * 100.0);
        default: return paramText (EffectKind::glowLeveler, param, v);
    }
}
} // namespace glow

namespace hush
{
int oversamplingOf (float v) noexcept { return 1 << std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 3.0f), 0, 3); }

std::string oldLawText (int param, float v)
{
    switch (param)
    {
        case frequency: return hzText (hz (v));
        case input: case output: return fmt ("%+.1f dB", lin (v, -12.0, 12.0));
        default: return paramText (EffectKind::hushDeEsser, param, v);
    }
}

std::string classicParamText (int param, float v)
{
    switch (param)
    {
        case threshold: return fmt ("%.1f dB", classicThresholdDb (v));
        case range: case lookahead: case release: return "-";
        default: return oldLawText (param, v); // (the classic voicing keeps its ranges)
    }
}
} // namespace hush

namespace ridge
{
std::string eParamText (int param, float v)
{
    switch (param)
    {
        case lmfQ: case hmfQ: return fmt ("Q %.2f", eQ (v));
        case lfGain: case lmfGain: case hmfGain: case hfGain: return fmt ("%+.1f dB", bandGainDb (v, false));
        case compThreshold: return fmt ("%+.1f dB", eCompThresholdDb (v));
        case gateRange: return v <= 0.0f ? "off" : fmt ("%.1f dB", eGateRangeDb (v));
        case gateThreshold: return fmt ("%+.1f dB", eGateThresholdDb (v));
        case curve: return "-";
        default: return paramText (EffectKind::ridgeStrip, param, v);
    }
}

std::string k4ParamText (int param, float v)
{
    switch (param)
    {
        case lmfQ: case hmfQ: return fmt ("Q %.2f", k4Q (v));
        case lpf: return v <= 0.0f || lpfHz (v) >= 15900.0 ? "off" : hzText (lpfHz (v));
        case k4Colour: { static const char* names[3] { "brown", "black", "orange" }; return names[k4ColourIndex (v)]; }
        case k4Mic: return fmt ("%.1f dB", k4MicDb (v));
        case k4MakeupOffset: return fmt ("%+.1f dB", k4OffsetDb (v));
        case analog: return on (v) ? "VCA on" : "VCA off";
        default: return eParamText (param, v);
    }
}

std::string vocalParamText (int param, float v)
{
    namespace vs = voxstrip;
    switch (param)
    {
        case vocal: return on (v) ? "vocal strip" : "off";
        case vInput: case vOutput: return fmt ("%+.1f dB", vs::gainDb (v));
        case vOrder: return vs::orderName (vs::orderOf (v));
        case vDsFreq: return hzText (vs::dsFreqHz (v));
        case vDpFreq: return hzText (vs::dpFreqHz (v));
        case vDsThreshold: case vDpThreshold: case vCompThreshold: return fmt ("%.1f dB", vs::thresholdDb (v));
        case vDsSplit: case vDpSplit: return on (v) ? "split" : "broadband";
        case vHpfFreq: return hzText (vs::hpfHz (v));
        case vNotchFreq: return hzText (vs::notchHz (v));
        case vNotchGain: return fmt ("%+.1f dB", vs::notchGainDb (v));
        case vHighFreq: return hzText (vs::highHz (v));
        case vHighGain: return fmt ("%+.1f dB", vs::highGainDb (v));
        case vExpThreshold: return v <= 0.0f ? "off" : fmt ("%.1f dB", vs::expThresholdDb (v));
        case vRatio: return fmt ("%.1f : 1", vs::ratio (v));
        case vAttack: return fmt ("%.1f ms", vs::attackMs (v));
        case vRelease: return fmt ("%.0f ms", vs::releaseMs (v));
        case vMakeup: return fmt ("%+.1f dB", vs::makeupDb (v));
        case vKnee: return on (v) ? "soft" : "hard";
        default: return onOff (v);
    }
}
} // namespace ridge

std::string paramText (EffectKind kind, int param, float v)
{
    if (kind == EffectKind::ridgeStrip)
    {
        using namespace ridge;
        switch (param)
        {
            case input: case output: return fmt ("%+.1f dB", trimDb (v));
            case hpf: return v <= 0.0f ? "off" : hzText (hpfHz (v));
            case lpf: return v <= 0.0f ? "off" : hzText (lpfHz (v));
            case lfGain: case lmfGain: case hmfGain: case hfGain: return fmt ("%+.1f dB", bandGainDb (v, false)) + " / " + fmt ("%+.1f", bandGainDb (v, true));
            case lfFreq: return hzText (lfHz (v));
            case lmfFreq: return hzText (lmfHz (v));
            case hmfFreq: return hzText (hmfHz (v));
            case hfFreq: return hzText (hfHz (v));
            case lmfQ: case hmfQ: return fmt ("Q %.2f", bandQ (v));
            case lfBell: case hfBell: return on (v) ? "bell" : "shelf";
            case curve: return on (v) ? "black" : "brown";
            case compRatio: return v >= 0.999f ? "inf : 1" : fmt ("%.1f : 1", ratio (v));
            case compThreshold: return fmt ("%.1f dB", compThresholdDb (v));
            case compRelease: case gateRelease: return fmt ("%.2f s", releaseSeconds (v));
            case gateRange: return v <= 0.0f ? "off" : fmt ("%.0f dB", gateRangeDb (v));
            case gateThreshold: return fmt ("%.1f dB", gateThresholdDb (v));
            case expander: return on (v) ? "expander" : "gate";
            case order: return on (v) ? "dyn > EQ" : "EQ > dyn";
            case voicing: return on (v) ? "E console" : "classic";
            case console4k: return on (v) ? "4K console" : "off";
            case k4Colour: case k4Mic: case k4MakeupOffset: return k4ParamText (param, v);
            case filterSplit: return on (v) ? "split" : "with EQ";
            case vocal: case vInput: case vOutput: case vOrder: case vDsFreq: case vDsThreshold: case vDsSplit:
            case vDpFreq: case vDpThreshold: case vDpSplit: case vHpfFreq: case vNotchFreq: case vNotchGain: case vHighFreq:
            case vHighGain: case vExpThreshold: case vCompThreshold: case vRatio: case vAttack: case vRelease: case vMakeup:
            case vKnee:
                return vocalParamText (param, v);
            case phase: return on (v) ? "inverted" : "normal";
            case compMix: return fmt ("%.0f%%", v * 100.0);
            default: return onOff (v);
        }
    }
    if (kind == EffectKind::glowLeveler)
    {
        using namespace glow;
        switch (param)
        {
            case peakReduction: return fmt ("%.0f", v * 100.0);
            case gain: return fmt ("%+.1f dB", gainDb (v));
            case mode: return on (v) ? "limit" : "compress";
            case emphasis: case mix: return fmt ("%.0f%%", v * 100.0);
            case trim: return fmt ("%+.1f dB", trimDb (v));
            case autoMakeup: return on (v) ? "on" : "off";
            case optoLaw: return on (v) ? "measured" : "round 123";
            case releaseLaw: return on (v) ? "measured" : "round 123";
            case makeupRef: return fmt ("%.0f", v * 100.0);
            case vuDisplay: return vuModeOf (v) == 1 ? "input" : vuModeOf (v) == 2 ? "output" : "gain reduction";
            case analog: return analogHz (v) == 0 ? "off" : analogHz (v) == 50 ? "50 Hz" : "60 Hz";
            default: return fmt ("%.2f", v);
        }
    }
    if (kind == EffectKind::hushDeEsser)
    {
        using namespace hush;
        switch (param)
        {
            // round 165x: new instances use the reference's ranges (the old law's texts: oldLawText)
            case frequency: return hzText (hpHz (v, true));
            case threshold: return fmt ("%.1f dB", thresholdDb (v));
            case range: return fmt ("%.1f dB", rangeDb (v));
            case mode: return on (v) ? "wideband" : "split";
            case detector: return on (v) ? "band" : "high-pass";
            case allround: return on (v) ? "allround" : "single vocal";
            case lowPass: return v >= 0.999f ? "off" : hzText (lowPassHz (v));
            case lookaheadTime: return fmt ("%.1f ms", lookaheadTimeMs (v));
            case stereoLink: return fmt ("%.0f%%", v * 100.0);
            case linkMid: return on (v) ? "mid" : "stereo";
            case external: return on (v) ? "external" : "internal";
            case input: case output: return v <= 0.0f ? std::string ("-inf dB") : fmt ("%+.1f dB", levelDb (v, true));
            case display: return on (v) ? "shown" : "hidden";
            case release: return fmt ("%.0f ms", releaseMs (v));
            case rangeLaw: return on (v) ? "extended" : "round 165w";
            case linkOnly: return fmt ("%.0f%%", v * 100.0);
            case linkSide: return on (v) ? "side" : "mid";
            case inputPan: case outputPan:
            {
                const auto pan = panValue (v);
                return std::abs (pan) < 0.0005 ? std::string ("centre") : fmt (pan < 0.0 ? "%.0f%% L" : "%.0f%% R", std::abs (pan) * 100.0);
            }
            case voicing: return on (v) ? "classic split" : "modern";
            case oversampling: return oversamplingOf (v) == 1 ? "off" : std::to_string (oversamplingOf (v)) + "x";
            default: return onOff (v);
        }
    }
    if (kind == EffectKind::aether)
    {
        using namespace aether;
        switch (param)
        {
            case air: return fmt ("%+.1f dB", airDb (v));
            case airFreq: return hzText (airHz (v));
            case exciterFreq: return hzText (exciterHz (v));
            case character: return v < 0.05f ? "even" : (v > 0.95f ? "odd" : fmt ("%.0f%% odd", v * 100.0));
            case output: return fmt ("%+.1f dB", lin (v, -12.0, 12.0));
            case dynamic: return onOff (v);
            default: return fmt ("%.0f%%", v * 100.0);
        }
    }
    if (kind == EffectKind::ember)
    {
        using namespace ember;
        switch (param)
        {
            case drive: return fmt ("%+.1f dB", driveDb (v)) + fmt (" (%.1f)", driveSteps (v)); // round 135: + the 0 … 10 scale
            case punish: case thump: case wideCuts: return onOff (v);
            case steep: return on (v) ? "30 dB/oct" : "gentle";
            case voice: return voiceName (voiceIndex (v));
            case lowCutWide: return v <= 0.0f ? "off" : hzText (lowCutWideHz (v));
            case highCutWide: return v >= 1.0f ? "off" : hzText (highCutWideHz (v));
            case style:
            {
                static const char* names[] = { "tube", "tape", "transistor", "fuzz" };
                return names[(int) styleOf (v)];
            }
            case tone: return fmt ("%+.0f%%", toneTilt (v) * 100.0);
            case lowCut: return v <= 0.0f ? "off" : hzText (lowCutHz (v));
            case highCut: return v >= 1.0f ? "off" : hzText (highCutHz (v));
            case mix: return fmt ("%.0f%%", v * 100.0);
            case output: return fmt ("%+.1f dB", lin (v, -18.0, 18.0));
            case autoGain: return onOff (v);
            case quality: return std::to_string (oversampling (v)) + "x";
            case bands: return bandCount (v) == 1 ? "1 band" : std::to_string (bandCount (v)) + " bands";
            case xover1: case xover2: case xover3: case xover4: case xover5: return hzText (crossoverHz (v));
            case bandsMore: { const auto m = (int) std::lround (std::clamp (v, 0.0f, 1.0f) * 2.0f); return m == 0 ? std::string ("off") : std::to_string (firstBands + m) + " bands"; }
            case band1Character: case band2Character: case band3Character: case band4Character: case band5Character: case band6Character:
                return characterName (characterIndex (v));
            case xoverSlope: return std::to_string (slopeDbOct (v)) + " dB/oct";
            // round 152
            case crossSlope1: case crossSlope2: case crossSlope3: case crossSlope4: case crossSlope5:
                return ownSlopeDbOct (v) == 0 ? std::string ("global") : std::to_string (ownSlopeDbOct (v)) + " dB/oct";
            case processing: return on (v) ? "Linear phase" : "Minimum phase";
            case lfo1Rate: case lfo2Rate: return fmt (lfoRateHz (v) < 10.0 ? "%.2f Hz" : "%.1f Hz", lfoRateHz (v));
            case lfo1Shape: case lfo2Shape:
            {
                static const char* names[lfoShapes] { "Sine", "Triangle", "Square", "Ease in", "Ease out" };
                return names[(int) lfoShapeOf (v)];
            }
            case envAttack: return fmt ("%.1f ms", envAttackMs (v));
            case envRelease: return envReleaseMs (v) < 1000.0 ? fmt ("%.0f ms", envReleaseMs (v)) : fmt ("%.2f s", envReleaseMs (v) / 1000.0);
            case mod1Source: case mod2Source: case mod3Source: case mod4Source:
            {
                static const char* names[modSources] { "Off", "LFO 1", "LFO 2", "Envelope" };
                return names[(int) modSourceOf (v)];
            }
            case mod1Target: case mod2Target: case mod3Target: case mod4Target: return modTargetName (modTargetOf (v));
            case mod1Amount: case mod2Amount: case mod3Amount: case mod4Amount: return fmt ("%+.3f", modAmount (v));
            case effectLaw: return on (v) ? "measured" : "static";
            case classicAuto: return onOff (v);
            case classicLaw: return on (v) ? "measured" : "round 136";
            case band2Style: case band3Style: case band4Style: case band5Style: case band6Style:
            {
                static const char* names[] = { "tube", "tape", "transistor", "fuzz" };
                return names[(int) styleOf (v)];
            }
            case band2Drive: case band3Drive: case band4Drive: case band5Drive: case band6Drive: return fmt ("%+.1f dB", driveDb (v));
            case band1Mix: case band2Mix: case band3Mix: case band4Mix: case band5Mix: case band6Mix: return fmt ("%.0f%%", v * 100.0);
            case band1Level: case band2Level: case band3Level: case band4Level: case band5Level: case band6Level: return fmt ("%+.1f dB", bandLevelDb (v));
            case inputGain: return fmt ("%+.1f dB", inputDb (v));
            // round 136: the Classic voicing, in the reference's own units
            case voicing: return on (v) ? "Classic" : "Ember";
            case classicStyle: { static const char* names[] = { "A", "E", "N", "T", "P" }; return names[emberclassic::styleOf (v)]; }
            case classicDrive: return fmt ("%.1f", emberclassic::driveUnits (v));
            case classicPunish: case classicThump: return onOff (v);
            case classicSteep: return on (v) ? "steep" : "gentle";
            case classicLowCut: return hzText (emberclassic::lowCutHz (v));
            case classicTone: return fmt ("%+.1f dB", emberclassic::toneDb (v));
            case classicHighCut: return hzText (emberclassic::highCutHz (v));
            case classicMix: return fmt ("%.0f%%", v * 100.0);
            case classicOutput: return fmt ("%+.1f dB", emberclassic::outputDb (v));
            // round 147
            case channelMode: return on (v) ? "Mid/Side" : "Left/Right";
            case band1DrivePan: case band2DrivePan: case band3DrivePan: case band4DrivePan: case band5DrivePan: case band6DrivePan:
            {
                const auto pan = panOf (v);
                return std::abs (pan) < 0.005 ? std::string ("centre") : fmt ("%.0f%% ", std::abs (pan) * 100.0) + (pan < 0.0 ? "L/M" : "R/S");
            }
            case band1Pan: case band2Pan: case band3Pan: case band4Pan: case band5Pan: case band6Pan: case inputPan: case outputPan:
            {
                const auto [a, b] = panScales (panOf (v));
                const auto db = [] (float g) { return g <= 0.0f ? std::string ("-inf") : fmt ("%.1f", gainToDb (g)); };
                return std::abs (panOf (v)) < 0.005 ? std::string ("centre") : db (a) + " / " + db (b) + " dB";
            }
            default:
                if ((param >= band1On && param < band1On + firstBands * extraFields) || (param >= band5On && param < band5On + (maxBands - firstBands) * extraFields))
                    switch ((param - (param >= band5On ? (int) band5On : (int) band1On)) % extraFields)
                    {
                        case 0: return onOff (v);
                        case 1: { static const char* names[] = { "normal", "solo", "mute" }; return names[(int) bandState (v)]; }
                        case 2: case 3: case 4: case 5: return fmt ("%+.1f dB", toneDb (v));
                        case 6: return fmt ("%.0f%%", feedbackAmount (v) * 100.0);
                        case 7: return hzText (feedbackHz (v));
                        default: return fmt ("%+.2f", dynamicsAmount (v));
                    }
                return fmt ("%.2f", v);
        }
    }
    return fmt ("%.2f", v);
}
} // namespace lynx::mixsuite
