#pragma once

// Matched voicings (REA round 123): engines that reproduce, as closely as black-box measurement allows, the
// plug-ins her vocal projects use when they are missing. They are selected by a "Voicing" parameter of the LYNX
// effect that substitutes the plug-in (Ridge Strip, Hush De-esser, Compressor, Glow Leveler); the effect's own
// engine stays untouched at voicing 0. Every law here was measured on the vendors' official trials hosted
// headless (tools/fxmeasure: impulses, sine stairs, tone bursts, pink noise, her own takes) and fitted; no
// plug-in code or data was read. Evidence: docs/analise/REA_rodada123.md.

#include <array>
#include <cstdint>

#include "core/Biquad.h"

namespace lynx::vmatch
{
// ---- E-type console channel strip (Ridge Strip, voicing "E") ----
namespace echannel
{
// Controls in the units the console's own dials show (the measured behaviour follows the displayed, rounded values)
struct Settings
{
    double inputDb = 0.0, outputDb = 0.0;
    bool phase = false;
    double hpHz = 0.0;  // 0 = out, else 17 .. 350
    double lpKHz = 0.0; // 0 = out, else 19.5 .. 3
    double lfGain = 0.0, lfHz = 60.0;
    bool lfBell = false;
    double lmfGain = 0.0, lmfKHz = 0.5, lmfQ = 2.5;
    double hmfGain = 0.0, hmfKHz = 3.5, hmfQ = 1.5;
    double hfGain = 0.0, hfKHz = 8.0;
    bool hfBell = false;
    bool eqOn = true;
    double compThreshold = 0.0, compRatio = 1.0, compRelease = 0.4; // dB on the console's scale (+10 .. -20), x:1, s
    bool compFast = false;
    double expThreshold = -30.0, expRange = 0.0, expRelease = 0.2; // console dB (-30 .. +10), dB, s
    bool expFast = false, gate = false;
    bool dynOn = true;
    bool filtersBeforeDynamics = true; // "split": the filters run before the dynamics (otherwise with the EQ)
    bool dynamicsAfterEq = false;      // "dynamics to channel out"
    // round 148: the console the settings drive — 0 the E-type channel of round 123, 1 the "4K" console measured on the
    // SSL 4K E trial (REA_rodada148 §2). The 4K's own controls (ignored by model 0):
    int model = 0;
    int colour = 0;               // EQ colour: 0 Brown, 1 Black, 2 Orange
    bool pre = false;             // the mic pre: drive `micDb` (0 … 50 dB, level-compensated)
    double micDb = 0.0;
    bool vca = false;             // the Analogue VCA (the dynamics' VCA colour)
    bool autoMakeup = true;       // the compressor's automatic make-up …
    double makeupOffsetDb = 0.0;  // … and its offset (−12 … +12 dB)
    double compMix = 1.0;         // the compressor's dry / wet mix 0 … 1
    // round 165 (model 0): "EQ to Dyn S.C." — [EXEC] the filters (unless split) and the EQ leave the audio and feed only
    // the dynamics' side chain; with the filter split on, the filters stay at the input of the audio path
    bool eqToSidechain = false;
};

// Q of the mid bands as the console's dial shows it, for a 0..1 dial position (3.5 at 0 .. 0.1 at 1)
double qFromDial (double v) noexcept;
double dialFromQ (double q) noexcept;

// One EQ band / filter as a digital biquad at `sampleRate` (band 0 LF, 1 LMF, 2 HMF, 3 HF). False = flat (skip it).
bool bandBiquad (int band, const Settings& s, double sampleRate, Biquad& out);
// The 3rd-order high-pass as a first-order section and a biquad
bool highPass (const Settings& s, double sampleRate, Biquad& first, Biquad& second);
bool lowPass (const Settings& s, double sampleRate, Biquad& out);

// Magnitude of the filters + EQ (dB) at `hz`
double responseDb (const Settings& s, double sampleRate, double hz);
// The compressor's static output level (dBFS peak of a sine) for a steady sine input level, from the model
double compStaticGainDb (const Settings& s, double sineDbfs);
// round 147b: the console's automatic make-up (dB) for its threshold and ratio (measured with the dynamics law; 0 at 1:1)
double compMakeupDb (const Settings& s) noexcept;

class Channel
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (float* l, float* r, int n, const Settings& s) noexcept;
    float compReductionDb() const noexcept { return lastComp; }
    float gateReductionDb() const noexcept { return lastGate; }

private:
    void design (const Settings& s) noexcept;
    double sr = 48000.0;
    Settings designed {};
    bool designedOnce = false;
    Biquad hp1, hp2, lp, bands[4], vcaHp;
    // round 165: the side chain's own copies of the filters / EQ (same coefficients, own state): [0] the gate's
    // detector (it hears the input), [1] the compressor's (it hears the compressor's output: a feedback design)
    struct ScChain { Biquad hp1, hp2, lp, bands[4]; } sc[2];
    bool hpOn = false, lpOn = false, bandOn[4] {};
    // dynamics state
    double env = 0.0, gr = 0.0, gr1 = 0.0, yl = 0.0, yr = 0.0;
    double genv = 0.0, ggr = 0.0;
    bool gateOpen = false;
    float lastComp = 0.0f, lastGate = 0.0f;
    std::uint32_t noise[2] { 0x9e3779b9u, 0x85ebca6bu }; // round 148: the 4K's VCA / mic pre hiss
};
} // namespace echannel

// ---- split-band de-esser (Hush De-esser, voicing "Classic split") ----
namespace deesser
{
struct Settings
{
    double hz = 5506.0, thresholdDb = -24.2; // the dial's frequency and threshold (-80 .. 0 dB)
    bool bandPass = false, wideband = false, listen = false;
    double outputDb = 0.0;
};

class DeEsser
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (float* l, float* r, int n, const Settings& s) noexcept;
    float reductionDb() const noexcept { return last; }
    // round 147: the block's highest detector energy in the threshold's own scale (DeEsser's Energy Detector meter)
    float detectorDb() const noexcept { return lastDet; }

private:
    void design (const Settings& s) noexcept;
    double sr = 48000.0, designedHz = -1.0;
    float lastDet = -120.0f;
    bool designedBp = false;
    Biquad sc1, sc2, bp1, bp2; // round 165: bp1 / bp2 = BandPass's low-pass after the high-pass
    double lpB = 0.0, lpA = 0.0, lpX[2] {}, lpY[2] {};
    double env = 0.0, gr = 0.0;
    float last = 0.0f;
};
} // namespace deesser

// ---- round 148: a single-vocal de-esser (Hush De-esser, "Vocal voicing"), fitted black-box on the official FabFilter
// Pro-DS trial at her two states (REA_rodada124_extras §5, REA_rodada148 §3): the band between the high-pass and the
// low-pass (4th-order each) feeds a power detector, a second detector compares that band with the signal below it, and
// the reduction only switches on for a band that is loud AND stands out (a vocal sibilant, not a steady bright tone);
// the gain follows range x (level - threshold + 4.1) / (0.1 - threshold), looked ahead; out = in (delayed) x gain
namespace vocalds
{
struct Settings
{
    double thresholdDb = -20.0, rangeDb = 6.0; // threshold -60 .. 0 dB, range 0 .. 24 dB
    double hpHz = 7000.0, lpHz = 14000.0;      // the detection band
    double lookaheadMs = 12.0;                 // 0 .. 15 ms (within the fixed 15 ms latency)
    bool lookahead = true, wideband = true, listen = false;
};

class DeEsser
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (float* l, float* r, int n, const Settings& s) noexcept;
    int latencySamples() const noexcept { return latency; }
    float reductionDb() const noexcept { return last; }
    float detectorDb() const noexcept { return lastDet; }

private:
    void design (const Settings& s) noexcept;
    static constexpr int ring = 4096;
    double sr = 48000.0, designedHp = -1.0, designedLp = -1.0;
    int latency = 720;
    Biquad hp1, hp2, lp1, lp2, low1, low2, split1, split2;
    double eh = 0.0, qh = 0.0, ql = 0.0, sm = 0.0;
    bool open = false;
    std::array<float, ring> dl {}, dr {}, level {}, ratio {};
    std::int64_t pos = 0;
    float last = 0.0f, lastDet = -120.0f;
};
} // namespace vocalds

// ---- electro-optical leveler (Glow Leveler, voicing "Opto classic") ----
namespace opto
{
struct Settings
{
    double gain = 40.0, peakReduction = 46.0, hiFreq = 50.0; // the dials, 0 .. 100
    bool limit = false;
    // round 152 (REA_rodada152a §2) [EXEC]: the measured Limit law (a steeper cell, a little of the input in the side
    // chain, the cell's deepest reduction) and AUTO MAKEUP (a make-up that follows the peak reduction dial); off = the
    // round-123 Limit [model] and no auto make-up
    bool measured = false, autoMakeup = false;
};
double autoMakeupDb (double peakReduction) noexcept; // round 152: AUTO MAKEUP's gain (dB) at a peak reduction dial
double makeupDb (double gainKnob) noexcept;              // the gain dial's level (dB) below the threshold
double sensitivityDb (double peakReduction, double hiFreq) noexcept; // side-chain level shift vs her state (dB)

class Leveler
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (float* l, float* r, int n, const Settings& s) noexcept;
    float reductionDb() const noexcept { return last; }

private:
    double sr = 48000.0;
    double cf = 0.0, cs = 0.0, el = 0.0, mem = 0.0, yc[2] {}, fx1[2] {}, fy1[2] {}, tx[2][4] {}, ty[2][4] {}, xp[2] {};
    Biquad sc;
    double scDesignedRate = 0.0;
    float last = 0.0f;
};
} // namespace opto

// ---- program-dependent peak compressor (Compressor, voicing "Vintage program") ----
namespace progcomp
{
struct Settings
{
    double thresholdDb = -15.0, ratio = 3.0, attackMs = 0.5, releaseMs = 160.0, gainDb = 0.0;
    double mix = 1.0, trimDb = 0.0; // parallel mix (the dry path is delayed with the wet one) and the trim after it
    // round 135w (the reference's switches; the fit above is ARC + Electro + Warm, her state): Manual = one release
    // time (no program-dependent slow path), Opto = the slow path's release curve inverted (slower near 0 dB of
    // reduction), Smooth = without Warm's extra reduction (ratio as shown) [model: laws not measured yet]
    bool manual = false, opto = false, smooth = false;
    // round 152 (REA_rodada152a §1) [EXEC]: the static curve measured on the trial at every ratio 1.5 … 50 (a table of
    // corrections on top of the fit above; Smooth has Warm's static curve there). Off = the round-123 / 150 curve.
    bool measuredStatic = false;
};
constexpr int lookahead = 64; // samples at 48 kHz (the original's measured delay, scaled with the rate)

class Compressor
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void process (float* l, float* r, int n, const Settings& s) noexcept;
    int latency() const noexcept { return delayLen; }
    float reductionDb() const noexcept { return last; }
    // round 152: the block's largest gain change, signed (negative = reduction, positive = the expansion's boost)
    float changeDb() const noexcept { return change; }

private:
    double sr = 48000.0;
    int delayLen = lookahead, pos = 0;
    std::array<float, 1024> dl {}, dr {};
    double env = 0.0, grf = 0.0, grs = 0.0;
    float last = 0.0f, change = 0.0f;
};
// round 152: the measured static correction (dB of gain added) at a ratio ≥ 1 and a level `over` dB above the threshold
double measuredStaticDelta (double ratio, double over) noexcept;
} // namespace progcomp
} // namespace lynx::vmatch
