#include "core/Patcher.h"
#include "core/Effects.h"
#include "core/FormulaController.h"
#include "core/RhythmPan.h"
#include "core/MixSuite.h"
#include "core/VoiceMatch.h"
#include "core/EchoSuite.h"
#include "core/LynxTune.h"
#include "core/LynxVerbs.h"

#include "core/Effector.h"
#include "core/PitcherDsp.h"
#include "core/Reeverb2.h"
#include "core/LuxeVerb.h"
#include "core/ConvolverFx.h"
#include "core/VintagePhaser.h"
#include "core/EmphasizerFx.h"
#include "core/FreqShifterFl.h"
#include "core/LowLifterFl.h"
#include "core/Vocodex.h"
#include "core/FruityVocoder.h"
#include "core/MaximusFx.h"
#include "core/ParametricEqFl.h"
#include "core/EquoFx.h"
#include "core/DistructorFx.h"
#include "core/MultibandDelayFx.h"
#include "core/PitchShifterFl.h"
#include "core/FruityDynamics.h"
#include "core/FruityShapers.h"
#include "core/FruityBasicFx.h"
#include "core/FruityFilters.h"
#include "core/NewtoneFx.h"
#include "core/FruityDelay.h"
#include "core/FrequencySplitter.h"
#include "core/HardcoreFx.h"
#include "core/StereoFx.h"
#include "core/ChorusFx.h"
#include "core/GrossBeat.h"
#include "core/LovePhilter.h"
#include "core/FruityDelayBank.h"
#include "core/FruityMultiband.h"
#include "core/FlangusFastDist.h"
#include "core/FruityPhaser.h"

#include "core/Biquad.h"
#include "core/FacetEq.h"
#include "core/ApexLimiter.h"
#include "core/DynamicsSuite.h"
#include "core/VoiceTools.h"
#include "core/VoiceShift.h"
#include "core/VisualFx.h"
#include "core/XyController.h"
#include "core/ControlSurface.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>

namespace lynx
{
namespace
{
constexpr double pi = 3.141592653589793;
constexpr double maxSampleRate = 192000.0;

float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }

// approximate tanh (Padé), saturating at ±1: much cheaper than std::tanh per sample
inline float fastTanh (float x) noexcept
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;

    const auto x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// sine of a phase in turns (0..1) via refined parabola (error < 0.1%), for LFOs
inline float sinTurns (double phase) noexcept
{
    const auto x = (float) (phase < 0.5 ? phase : phase - 1.0) * 2.0f;
    const auto y = 4.0f * x * (1.0f - std::abs (x));
    return 0.225f * (y * std::abs (y) - y) + y;
}

float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1e-9f)); }

// 0..1 -> logarithmic range [lo, hi]
float logRange (float v, float lo, float hi) noexcept { return lo * std::pow (hi / lo, std::clamp (v, 0.0f, 1.0f)); }

// ---------------------------------------------------------------------------
// Parameter mapping (shared by the DSP and the UI text)

float eqGainDb (float v) { return (v - 0.5f) * 36.0f; } // -18..+18 dB
float eqMidHz (float v) { return logRange (v, 200.0f, 5000.0f); }
float compThresholdDb (float v) { return -60.0f + v * 60.0f; }
float compRatio (float v) { return 1.0f + v * 19.0f; }
float compAttackMs (float v) { return logRange (v, 0.1f, 100.0f); }
float compReleaseMs (float v) { return logRange (v, 10.0f, 1000.0f); }
float compMakeupDb (float v) { return v * 24.0f; }
// round 150 (REA_rodada150 §6): the Renaissance Compressor's own ranges, read black-box from the trial's parameter
// texts — ratio 50 … 1 … 0.5 (log-linear: 50 at 0, 1.00 at 0.625, 0.5 at 1; below 1 = expansion), gain −30 … +30 dB,
// attack 0.5 … 500 ms and release 5 … 5000 ms (0.5 · 1000^v, 5 · 1000^v)
float rcRatio (float v)
{
    const auto x = (double) std::clamp (v, 0.0f, 1.0f);
    return (float) (x <= 0.625 ? std::exp (std::log (50.0) * (1.0 - x / 0.625)) : std::exp (std::log (0.5) * (x - 0.625) / 0.375));
}
float rcRatioValue (double ratio)
{
    const auto r = std::clamp (ratio, 0.5, 50.0);
    return (float) (r >= 1.0 ? 0.625 * (1.0 - std::log (r) / std::log (50.0)) : 0.625 + 0.375 * std::log (r) / std::log (0.5));
}
float rcAttackMs (float v) { return logRange (v, 0.5f, 500.0f); }
float rcReleaseMs (float v) { return logRange (v, 5.0f, 5000.0f); }
float rcGainDb (float v) { return -30.0f + 60.0f * std::clamp (v, 0.0f, 1.0f); }

// BPM-synced Delay divisions, in beats (quarter notes).
constexpr std::array<double, 8> delayDivisions { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0 };
constexpr std::array<const char*, 8> delayNames { "1/16", "1/8", "1/8 dot.", "1/4", "1/4 dot.", "1/2", "1/2 dot.", "1 bar" };

int delayIndex (float v) { return std::clamp ((int) (v * (float) delayDivisions.size()), 0, (int) delayDivisions.size() - 1); }

float reverbLowCutHz (float v) { return logRange (v, 20.0f, 3000.0f); }    // 0 = off
float reverbHighCutHz (float v) { return logRange (v, 500.0f, 22000.0f); } // 1 = off

float limiterCeilingDb (float v) { return -12.0f + v * 12.0f; }
float limiterGainDb (float v) { return v * 24.0f; }
float limiterReleaseMs (float v) { return logRange (v, 10.0f, 1000.0f); }
int filterType (float v) { return std::clamp ((int) (v * 3.0f), 0, 2); } // low-pass, high-pass, band-pass
constexpr std::array<const char*, 3> filterNames { "Low-pass", "High-pass", "Band-pass" };
float filterHz (float v) { return logRange (v, 20.0f, 20000.0f); }
float filterQ (float v) { return logRange (v, 0.5f, 12.0f); }
float distortionDriveDb (float v) { return v * 36.0f; }
float distortionToneHz (float v) { return logRange (v, 800.0f, 20000.0f); }
float chorusRateHz (float v) { return logRange (v, 0.05f, 5.0f); }
float chorusDepthMs (float v) { return v * 8.0f; }

// ---------------------------------------------------------------------------

class Eq final : public Effect
{
public:
    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        lastParams.fill (-1.0f); // force recalculation
        reset();
    }

    void reset() noexcept override
    {
        for (auto& b : bands)
            b.reset();
    }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        if (params != lastParams)
        {
            lastParams = params;
            bands[0].shelf (sampleRate, 120.0, eqGainDb (params[0]), false);
            bands[1].peaking (sampleRate, eqMidHz (params[2]), 0.8, eqGainDb (params[1]));
            bands[2].shelf (sampleRate, 8000.0, eqGainDb (params[3]), true);
            output = dbToGain (eqGainDb (params[4]));
        }

        for (int i = 0; i < n; ++i)
        {
            auto a = l[i], b = r[i];

            for (auto& band : bands)
            {
                a = band.process (a, 0);
                b = band.process (b, 1);
            }

            l[i] = a * output;
            r[i] = b * output;
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<Biquad, 3> bands;
    std::array<float, maxEffectParams> lastParams {};
    float output = 1.0f;
};

// ---------------------------------------------------------------------------

class Compressor final : public Effect
{
public:
    void prepare (double sr) noexcept override { sampleRate = sr; vintage.prepare (sr); reset(); }
    void reset() noexcept override
    {
        envelopeDb = 0.0f;
        inPower = outPower = 0.0f;
        autoGain = 1.0f;
        vintage.reset();
    }

    // round 123: voicing 1 = the measured program-dependent compressor (core/VoiceMatch.h); it delays the audio
    int latencySamples() const noexcept override { return latency.load (std::memory_order_relaxed); }

    // round 129: the editor's meters (Effect::controlOutput): 0 = gain reduction (0..1 over 0..30 dB), 2 / 3 = input /
    // output peak — the level meters the compressors it stands in for show (Renaissance Compressor's three meters)
    float controlOutput (int k) const noexcept override
    {
        return k == 0 ? meterGr.load (std::memory_order_relaxed) : k == 2 ? meterIn.load (std::memory_order_relaxed)
             : k == 3 ? meterOut.load (std::memory_order_relaxed) : k == 4 ? meterLimit.load (std::memory_order_relaxed)
             : k == 5 ? meterChange.load (std::memory_order_relaxed) : 0.0f; // round 152: 5 = the signed gain change in dB
    }

    // round 135w: the reference's built-in 0 dBFS output limiter — the gain drops at once to keep every sample at or
    // under 0 dBFS and recovers over 50 ms; meter 4 = its reduction this block (0..1 over 0..12 dB), the activity pill
    void outputLimit (float* l, float* r, int n) noexcept
    {
        const auto rel = (float) (1.0 - std::exp (-1.0 / (0.05 * sampleRate)));
        float most = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const auto peak = std::max (std::abs (l[i]), std::abs (r[i]));
            limitGain += rel * (1.0f - limitGain);
            if (peak * limitGain > 1.0f)
                limitGain = 1.0f / peak;
            l[i] *= limitGain;
            r[i] *= limitGain;
            most = std::min (most, limitGain);
        }
        meterLimit.store (std::clamp (-20.0f * std::log10 (std::max (most, 1e-6f)) / 12.0f, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        if (params[8] >= 0.5f)
        {
            latency.store (vintage.latency(), std::memory_order_relaxed);
            float peakIn = 0.0f, peakOut = 0.0f;
            for (int i = 0; i < n; ++i)
                peakIn = std::max (peakIn, std::max (std::abs (l[i]), std::abs (r[i])));
            vmatch::progcomp::Settings v;
            const bool rc = params[13] >= 0.5f; // round 150: the RComp ranges
            v.thresholdDb = compThresholdDb (params[0]);
            v.ratio = rc ? rcRatio (params[14]) : compRatio (params[1]);
            v.attackMs = rc ? rcAttackMs (params[16]) : compAttackMs (params[2]);
            v.releaseMs = rc ? rcReleaseMs (params[17]) : compReleaseMs (params[3]);
            v.gainDb = rc ? rcGainDb (params[15]) : compMakeupDb (params[4]);
            v.mix = std::clamp ((double) params[6], 0.0, 1.0);
            v.trimDb = -18.0 + 36.0 * std::clamp ((double) params[7], 0.0, 1.0);
            v.manual = params[9] >= 0.5f;
            v.opto = params[10] >= 0.5f;
            v.smooth = params[11] >= 0.5f;
            v.measuredStatic = params[18] >= 0.5f; // round 152
            vintage.process (l, r, n, v);
            if (params[12] >= 0.5f)
                outputLimit (l, r, n);
            else
                meterLimit.store (0.0f, std::memory_order_relaxed);
            for (int i = 0; i < n; ++i)
                peakOut = std::max (peakOut, std::max (std::abs (l[i]), std::abs (r[i])));
            useSidechain = false;
            meterGr.store (std::clamp (vintage.reductionDb() / 30.0f, 0.0f, 1.0f), std::memory_order_relaxed);
            meterChange.store (vintage.changeDb(), std::memory_order_relaxed);
            meterIn.store (std::min (peakIn, 1.0f), std::memory_order_relaxed);
            meterOut.store (std::min (peakOut, 1.0f), std::memory_order_relaxed);
            return;
        }
        latency.store (0, std::memory_order_relaxed);
        // "Auto gain" (round 107): the long-term level stays where it was (slow in/out power match, ±12 dB), so the
        // compressor shapes the dynamics without making the track quieter. Used where its settings are a guess (a
        // substitute for a missing plugin, whose own makeup gain is unknown).
        const auto levelMatch = params[5] >= 0.5f;
        const auto powerCoeff = (float) std::exp (-1.0 / (1.5 * sampleRate));
        const auto gainCoeff = (float) std::exp (-1.0 / (0.05 * sampleRate));
        const bool rc = params[13] >= 0.5f; // round 150: the RComp ranges (ratio below 1 = upward expansion)
        const auto threshold = compThresholdDb (params[0]);
        const auto ratio = rc ? rcRatio (params[14]) : compRatio (params[1]);
        const auto attack = (float) std::exp (-1.0 / ((rc ? rcAttackMs (params[16]) : compAttackMs (params[2])) * 0.001 * sampleRate));
        const auto release = (float) std::exp (-1.0 / ((rc ? rcReleaseMs (params[17]) : compReleaseMs (params[3])) * 0.001 * sampleRate));
        const auto makeup = dbToGain (rc ? rcGainDb (params[15]) : compMakeupDb (params[4]));
        const bool expand = ratio < 1.0f;
        const auto slope = expand ? 1.0f / ratio - 1.0f : 1.0f - 1.0f / ratio; // dB of change per dB over the threshold
        const auto thresholdGain = dbToGain (threshold);
        const auto sidechain = useSidechain;
        useSidechain = false;
        float peakIn = 0.0f, peakOut = 0.0f, maxGr = 0.0f;
        // round 129: parallel mix (dry with the compressed signal) and a trim after it (±18 dB); older projects load
        // 100 % / 0 dB
        const auto wetMix = std::clamp (params[6], 0.0f, 1.0f);
        const auto trim = dbToGain (-18.0f + 36.0f * std::clamp (params[7], 0.0f, 1.0f));

        for (int i = 0; i < n; ++i)
        {
            peakIn = std::max (peakIn, std::max (std::abs (l[i]), std::abs (r[i])));
            // linked stereo peak detector, "hard knee" curve; below the threshold the log is not needed.
            // With sidechain, the detector listens to the other track (kick ducking)
            const auto peak = sidechain ? (sideL != nullptr ? std::max (std::abs (sideL[i]), std::abs (sideR[i])) : 0.0f)
                                        : std::max (std::abs (l[i]), std::abs (r[i]));
            const auto reductionDb = peak > thresholdGain ? (gainToDb (peak) - threshold) * slope : 0.0f; // the change (≥ 0)

            // the envelope rises with the attack and falls with the release
            const auto coeff = reductionDb > envelopeDb ? attack : release;
            envelopeDb = reductionDb + coeff * (envelopeDb - reductionDb);

            if (envelopeDb < 1.0e-4f)
                envelopeDb = 0.0f; // no audible reduction: skip the pow

            auto gain = envelopeDb > 0.0f ? dbToGain (expand ? envelopeDb : -envelopeDb) * makeup : makeup;

            if (levelMatch)
            {
                const auto in = 0.5f * (l[i] * l[i] + r[i] * r[i]);
                inPower = in + powerCoeff * (inPower - in);
                const auto out = in * gain * gain;
                outPower = out + powerCoeff * (outPower - out);
                // follow only while there is signal (silence keeps the last match)
                const auto target = inPower > 1.0e-8f && outPower > 1.0e-12f ? std::clamp (std::sqrt (inPower / outPower), 0.25f, 4.0f) : autoGain;
                autoGain = target + gainCoeff * (autoGain - target);
                gain *= autoGain;
            }

            l[i] = (l[i] * gain * wetMix + l[i] * (1.0f - wetMix)) * trim;
            r[i] = (r[i] * gain * wetMix + r[i] * (1.0f - wetMix)) * trim;
            maxGr = std::max (maxGr, envelopeDb);
        }
        if (params[12] >= 0.5f)
            outputLimit (l, r, n);
        else
            meterLimit.store (0.0f, std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            peakOut = std::max (peakOut, std::max (std::abs (l[i]), std::abs (r[i])));
        meterGr.store (std::clamp ((expand ? 0.0f : maxGr) / 30.0f, 0.0f, 1.0f), std::memory_order_relaxed);
        meterChange.store (expand ? maxGr : -maxGr, std::memory_order_relaxed); // round 152: signed (the expansion's boost > 0)
        meterIn.store (std::min (peakIn, 1.0f), std::memory_order_relaxed);
        meterOut.store (std::min (peakOut, 1.0f), std::memory_order_relaxed);
    }

    void setSidechain (const float* left, const float* right) noexcept override
    {
        sideL = left;
        sideR = right;
        useSidechain = true;
    }

private:
    std::atomic<float> meterGr { 0.0f }, meterIn { 0.0f }, meterOut { 0.0f }, meterLimit { 0.0f }, meterChange { 0.0f };
    float limitGain = 1.0f;
    std::atomic<int> latency { 0 };
    vmatch::progcomp::Compressor vintage;
    double sampleRate = 44100.0;
    float envelopeDb = 0.0f;
    float inPower = 0.0f, outPower = 0.0f, autoGain = 1.0f;
    const float* sideL = nullptr;
    const float* sideR = nullptr;
    bool useSidechain = false;
};

// ---------------------------------------------------------------------------

class Delay final : public Effect
{
public:
    static constexpr double maxSeconds = 3.0;

    Delay()
    {
        const auto size = (std::size_t) (maxSeconds * maxSampleRate) + 4;
        buffer[0].assign (size, 0.0f);
        buffer[1].assign (size, 0.0f);
    }

    void prepare (double sr) noexcept override { sampleRate = sr; reset(); }

    void reset() noexcept override
    {
        std::fill (buffer[0].begin(), buffer[0].end(), 0.0f);
        std::fill (buffer[1].begin(), buffer[1].end(), 0.0f);
        damp[0] = damp[1] = 0.0f;
        write = 0;
    }

    void process (float* l, float* r, int n, const EffectParams& params, double bpm) noexcept override
    {
        const auto beats = delayDivisions[(std::size_t) delayIndex (params[0])];
        const auto seconds = std::min (maxSeconds - 0.01, beats * 60.0 / std::max (bpm, 20.0));
        const auto delaySamples = std::max<std::size_t> (1, (std::size_t) (seconds * sampleRate));
        const auto feedback = params[1] * 0.95f;
        const auto mix = params[2];
        const auto pingPong = params[3] > 0.5f;
        const auto dampCoeff = 1.0f - params[4] * 0.9f; // 1 = no filter in the feedback path
        const auto size = buffer[0].size();

        for (int i = 0; i < n; ++i)
        {
            const auto readPos = (write + size - delaySamples) % size;
            auto echoL = buffer[0][readPos];
            auto echoR = buffer[1][readPos];

            // 1-pole low-pass in the feedback path: repeats get darker
            damp[0] += dampCoeff * (echoL - damp[0]);
            damp[1] += dampCoeff * (echoR - damp[1]);
            echoL = damp[0];
            echoR = damp[1];

            const auto inL = l[i], inR = r[i];

            if (pingPong) // mono input on the left side, repeats alternate
            {
                buffer[0][write] = 0.5f * (inL + inR) + echoR * feedback;
                buffer[1][write] = echoL * feedback;
            }
            else
            {
                buffer[0][write] = inL + echoL * feedback;
                buffer[1][write] = inR + echoR * feedback;
            }

            l[i] = inL + mix * echoL;
            r[i] = inR + mix * echoR;
            write = (write + 1) % size;
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<std::vector<float>, 2> buffer;
    std::size_t write = 0;
    float damp[2] {};
};

// ---------------------------------------------------------------------------
// Schroeder/Moorer-style reverb (the "Freeverb" structure, public domain):
// 8 comb filters with low-pass in the feedback + 4 all-passes per side.

class Reverb final : public Effect
{
public:
    Reverb()
    {
        const auto scale = maxSampleRate / 44100.0;

        for (int ch = 0; ch < 2; ++ch)
        {
            for (std::size_t i = 0; i < combTunings.size(); ++i)
                combs[ch][i].buffer.assign ((std::size_t) ((combTunings[i] + stereoSpread) * scale) + 2, 0.0f);

            for (std::size_t i = 0; i < allpassTunings.size(); ++i)
                allpasses[ch][i].buffer.assign ((std::size_t) ((allpassTunings[i] + stereoSpread) * scale) + 2, 0.0f);
        }

        predelayBuffer.assign ((std::size_t) (maxPredelaySeconds * maxSampleRate) + 4, 0.0f);
    }

    static constexpr double maxPredelaySeconds = 1.0;

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        lastLowCut = lastHighCut = -1.0f;
        const auto scale = sr / 44100.0;

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto spread = ch == 1 ? stereoSpread : 0;

            for (std::size_t i = 0; i < combTunings.size(); ++i)
                combs[ch][i].length = std::min (combs[ch][i].buffer.size(), (std::size_t) ((combTunings[i] + spread) * scale));

            for (std::size_t i = 0; i < allpassTunings.size(); ++i)
                allpasses[ch][i].length = std::min (allpasses[ch][i].buffer.size(), (std::size_t) ((allpassTunings[i] + spread) * scale));
        }

        reset();
    }

    void reset() noexcept override
    {
        for (auto& side : combs)
            for (auto& c : side)
            {
                std::fill (c.buffer.begin(), c.buffer.end(), 0.0f);
                c.index = 0;
                c.store = 0.0f;
            }

        for (auto& side : allpasses)
            for (auto& a : side)
            {
                std::fill (a.buffer.begin(), a.buffer.end(), 0.0f);
                a.index = 0;
            }

        std::fill (predelayBuffer.begin(), predelayBuffer.end(), 0.0f);
        predelayWrite = 0;
        lowCut.reset();
        highCut.reset();
    }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        const auto feedback = 0.7f + params[0] * 0.28f; // room size
        const auto damping = params[1] * 0.4f;
        const auto width = params[2];
        const auto mix = params[3];
        const auto dry = params[4]; // 0 = reverb only (reverb on a send)
        const auto wet1 = width / 2.0f + 0.5f;
        const auto wet2 = (1.0f - width) / 2.0f;
        constexpr float inputGain = 0.015f;

        // at the wet input: low/high cut (at the extremes = off) and predelay
        const auto lowCutHz = params[5], highCutHz = params[6];
        const auto useLowCut = lowCutHz > 0.001f, useHighCut = highCutHz < 0.999f;

        if (lowCutHz != lastLowCut || highCutHz != lastHighCut)
        {
            lastLowCut = lowCutHz;
            lastHighCut = highCutHz;
            lowCut.filter (sampleRate, 1, reverbLowCutHz (lowCutHz), 0.7071);
            highCut.filter (sampleRate, 0, reverbHighCutHz (highCutHz), 0.7071);
        }

        const auto predelay = std::min ((int) (params[7] * maxPredelaySeconds * sampleRate), (int) predelayBuffer.size() - 1);
        const auto predelayLength = (int) predelayBuffer.size();

        // filter by filter over a chunk (each comb/allpass fully in cache), with the same summing
        // order as sample-by-sample processing
        constexpr int span = 256;
        float input[span], out[2][span];

        for (int start = 0; start < n; start += span)
        {
            const auto m = std::min (span, n - start);
            auto* bl = l + start;
            auto* br = r + start;

            for (int i = 0; i < m; ++i)
            {
                auto x = (bl[i] + br[i]) * inputGain;

                if (useLowCut)
                    x = lowCut.process (x, 0);
                if (useHighCut)
                    x = highCut.process (x, 0);

                if (predelay > 0)
                {
                    predelayBuffer[(std::size_t) predelayWrite] = x;
                    auto read = predelayWrite - predelay;
                    if (read < 0)
                        read += predelayLength;
                    x = predelayBuffer[(std::size_t) read];

                    if (++predelayWrite == predelayLength)
                        predelayWrite = 0;
                }

                input[i] = x;
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                std::fill (out[ch], out[ch] + m, 0.0f);

                // 4 combs per loop: the feedback chains are independent and the CPU interleaves
                // them (a single comb is bound by the damping filter's latency)
                for (std::size_t c = 0; c + 4 <= combs[ch].size(); c += 4)
                    Comb::process4 (&combs[ch][c], input, out[ch], m, feedback, damping);

                for (auto& a : allpasses[ch])
                    a.process (out[ch], m);
            }

            for (int i = 0; i < m; ++i)
            {
                bl[i] = bl[i] * dry + mix * (out[0][i] * wet1 + out[1][i] * wet2);
                br[i] = br[i] * dry + mix * (out[1][i] * wet1 + out[0][i] * wet2);
            }
        }
    }

private:
    struct Comb
    {
        std::vector<float> buffer;
        std::size_t length = 1, index = 0;
        float store = 0.0f;

        // four combs at once; sums into `out` in the same order as one by one
        static void process4 (Comb* c, const float* input, float* out, int n, float feedback, float damping) noexcept
        {
            float* buf[4] = { c[0].buffer.data(), c[1].buffer.data(), c[2].buffer.data(), c[3].buffer.data() };
            std::size_t idx[4] = { c[0].index, c[1].index, c[2].index, c[3].index };
            const std::size_t len[4] = { c[0].length, c[1].length, c[2].length, c[3].length };
            float st[4] = { c[0].store, c[1].store, c[2].store, c[3].store };
            const auto keep = 1.0f - damping;

            for (int i = 0; i < n; ++i)
            {
                float o[4];

                for (int k = 0; k < 4; ++k)
                {
                    o[k] = buf[k][idx[k]];
                    st[k] = o[k] * keep + st[k] * damping;
                    buf[k][idx[k]] = input[i] + st[k] * feedback;

                    if (++idx[k] == len[k])
                        idx[k] = 0;
                }

                out[i] = out[i] + o[0] + o[1] + o[2] + o[3];
            }

            for (int k = 0; k < 4; ++k)
            {
                c[k].index = idx[k];
                c[k].store = st[k];
            }
        }

    };

    struct Allpass
    {
        std::vector<float> buffer;
        std::size_t length = 1, index = 0;

        void process (float* io, int n) noexcept
        {
            auto* buf = buffer.data();
            auto idx = index;
            const auto len = length;

            for (int i = 0; i < n; ++i)
            {
                const auto input = io[i];
                const auto delayed = buf[idx];
                io[i] = delayed - input;
                buf[idx] = input + delayed * 0.5f;

                if (++idx == len)
                    idx = 0;
            }

            index = idx;
        }
    };

    static constexpr std::array<int, 8> combTunings { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static constexpr std::array<int, 4> allpassTunings { 556, 441, 341, 225 };
    static constexpr int stereoSpread = 23;

    std::array<std::array<Comb, 8>, 2> combs;
    std::array<std::array<Allpass, 4>, 2> allpasses;
    double sampleRate = 44100.0;
    Biquad lowCut, highCut;
    float lastLowCut = -1.0f, lastHighCut = -1.0f;
    std::vector<float> predelayBuffer;
    int predelayWrite = 0;
};
// ---------------------------------------------------------------------------
// Lookahead limiter: gain starts dropping before the peak arrives (1.5 ms delay,
// reported as latency so PDC aligns it with the other tracks).

class Limiter final : public Effect
{
public:
    static constexpr double lookaheadSeconds = 0.0015;

    Limiter()
    {
        const auto size = (std::size_t) (lookaheadSeconds * maxSampleRate) + 4;
        delay[0].assign (size, 0.0f);
        delay[1].assign (size, 0.0f);
        maxValues.assign (size, 0.0f);
        maxTimes.assign (size, 0);
    }

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        lookahead = std::max (1, (int) std::lround (lookaheadSeconds * sr));
        attack = (float) (1.0 - std::exp (-1.0 / (lookahead * 0.3)));
        reset();
    }

    void reset() noexcept override
    {
        std::fill (delay[0].begin(), delay[0].end(), 0.0f);
        std::fill (delay[1].begin(), delay[1].end(), 0.0f);
        position = 0;
        gain = 1.0f;
        clock = 0;
        // the window starts full of zeros: a single 0 candidate covers all of them
        head = 0;
        count = 1;
        maxValues[0] = 0.0f;
        maxTimes[0] = -1;
    }

    int latencySamples() const noexcept override { return lookahead; }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        const auto ceiling = dbToGain (limiterCeilingDb (params[0]));
        const auto input = dbToGain (limiterGainDb (params[1]));
        const auto release = (float) (1.0 - std::exp (-1.0 / (limiterReleaseMs (params[2]) * 0.001 * sampleRate)));

        for (int i = 0; i < n; ++i)
        {
            const auto a = l[i] * input, b = r[i] * input;
            const auto peak = std::max (std::abs (a), std::abs (b));

            // largest peak among the last `lookahead` samples (sliding maximum with a monotonic queue)
            const auto capacity = (int) maxValues.size();

            while (count > 0 && maxValues[(std::size_t) ((head + count - 1) % capacity)] <= peak)
                --count;

            const auto slot = (std::size_t) ((head + count) % capacity);
            maxValues[slot] = peak;
            maxTimes[slot] = clock;
            ++count;

            if (maxTimes[(std::size_t) head] <= clock - lookahead)
            {
                head = (head + 1) % capacity;
                --count;
            }

            ++clock;
            const auto ahead = maxValues[(std::size_t) head];

            const auto target = ahead > ceiling ? ceiling / ahead : 1.0f;
            gain += (target - gain) * (target < gain ? attack : release);

            const auto outL = delay[0][(std::size_t) position] * gain;
            const auto outR = delay[1][(std::size_t) position] * gain;
            delay[0][(std::size_t) position] = a;
            delay[1][(std::size_t) position] = b;

            if (++position == lookahead)
                position = 0;

            // safety: never exceed the ceiling
            l[i] = std::clamp (outL, -ceiling, ceiling);
            r[i] = std::clamp (outR, -ceiling, ceiling);
        }
    }

private:
    double sampleRate = 44100.0;
    int lookahead = 64, position = 0;
    float attack = 0.1f, gain = 1.0f;
    std::array<std::vector<float>, 2> delay;
    std::vector<float> maxValues;      // monotonic (decreasing) queue for the sliding maximum
    std::vector<std::int64_t> maxTimes;
    int head = 0, count = 0;
    std::int64_t clock = 0;
};

// ---------------------------------------------------------------------------

class Filter final : public Effect
{
public:
    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        last.fill (-1.0f);
        reset();
    }

    void reset() noexcept override { biquad.reset(); }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        if (params[0] != last[0] || params[1] != last[1] || params[2] != last[2])
        {
            last = params;
            biquad.filter (sampleRate, filterType (params[0]), filterHz (params[1]), filterQ (params[2]));
        }

        const auto mix = params[3];

        for (int i = 0; i < n; ++i)
        {
            l[i] += (biquad.process (l[i], 0) - l[i]) * mix;
            r[i] += (biquad.process (r[i], 1) - r[i]) * mix;
        }
    }

private:
    double sampleRate = 44100.0;
    Biquad biquad;
    EffectParams last {};
};

// ---------------------------------------------------------------------------

class Distortion final : public Effect
{
public:
    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        lastTone = -1.0f;
        reset();
    }

    void reset() noexcept override { tone.reset(); }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        if (params[1] != lastTone)
        {
            lastTone = params[1];
            tone.filter (sampleRate, 0, distortionToneHz (params[1]), 0.707);
        }

        const auto drive = dbToGain (distortionDriveDb (params[0]));
        const auto mix = params[2];
        const auto output = dbToGain ((params[3] - 0.5f) * 24.0f);
        // compensate the drive gain so the volume doesn't blow up
        const auto compensate = 1.0f / fastTanh (std::max (drive, 1.0f));

        for (int i = 0; i < n; ++i)
        {
            const auto wl = tone.process (fastTanh (l[i] * drive) * compensate, 0);
            const auto wr = tone.process (fastTanh (r[i] * drive) * compensate, 1);
            l[i] = (l[i] + (wl - l[i]) * mix) * output;
            r[i] = (r[i] + (wr - r[i]) * mix) * output;
        }
    }

private:
    double sampleRate = 44100.0;
    Biquad tone;
    float lastTone = -1.0f;
};

// ---------------------------------------------------------------------------
// Chorus: an LFO-modulated delay line, with opposite phases on L and R.

class Chorus final : public Effect
{
public:
    static constexpr double maxSeconds = 0.05;

    Chorus()
    {
        const auto size = (std::size_t) (maxSeconds * maxSampleRate) + 4;
        buffer[0].assign (size, 0.0f);
        buffer[1].assign (size, 0.0f);
    }

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        length = (int) std::min<std::size_t> (buffer[0].size(), (std::size_t) (maxSeconds * sr) + 4);
        reset();
    }

    void reset() noexcept override
    {
        std::fill (buffer[0].begin(), buffer[0].end(), 0.0f);
        std::fill (buffer[1].begin(), buffer[1].end(), 0.0f);
        write = 0;
        phase = 0.0;
    }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        const auto rate = chorusRateHz (params[0]);
        const auto depth = chorusDepthMs (params[1]) * 0.001 * sampleRate;
        const auto mix = params[2];
        const auto feedback = params[3] * 0.7f;
        const auto base = 0.012 * sampleRate; // 12 ms
        const auto step = rate / sampleRate;

        for (int i = 0; i < n; ++i)
        {
            float* io[2] = { l + i, r + i };
            const auto s = sinTurns (phase); // L and R in opposite phases: the same sine with flipped sign

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto lfo = ch == 0 ? s : -s;
                const auto delaySamples = base + depth * 0.5 * (1.0 + lfo);
                auto readPos = (double) write - delaySamples;
                while (readPos < 0.0)
                    readPos += length;

                const auto i0 = (int) readPos;
                const auto frac = (float) (readPos - i0);
                const auto& buf = buffer[(std::size_t) ch];
                const auto next = i0 + 1 == length ? 0 : i0 + 1;
                const auto delayed = buf[(std::size_t) i0] + frac * (buf[(std::size_t) next] - buf[(std::size_t) i0]);
                buffer[(std::size_t) ch][(std::size_t) write] = *io[ch] + delayed * feedback;
                *io[ch] += (delayed - *io[ch]) * mix * 0.5f;
            }

            if (++write == length)
                write = 0;

            phase += step;
            if (phase >= 1.0)
                phase -= 1.0;
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<std::vector<float>, 2> buffer;
    int length = 1, write = 0;
    double phase = 0.0;
};

// ---------------------------------------------------------------------------
// 7-band parametric EQ: each band is a cookbook biquad (or up to 4 in series for the cuts,
// with the Qs of a Butterworth of the requested order).

constexpr std::array<std::array<double, 4>, 4> butterworthQ { { { 0.70710678, 0, 0, 0 },
                                                                { 0.54119610, 1.30656296, 0, 0 },
                                                                { 0.51763809, 0.70710678, 1.93185165, 0 },
                                                                { 0.50979558, 0.60134489, 0.89997622, 2.56291545 } } };

// Stage coefficients of a band; returns the number of stages (0 = band off/transparent).
int bandStages (const EffectParams& p, int band, double sr, std::array<Biquad, 4>& out) noexcept
{
    const auto t = peq::type (p[(std::size_t) peq::param (band, 0)]);
    const auto hz = std::min (peq::hz (p[(std::size_t) peq::param (band, 1)]), sr * 0.45);
    const auto db = peq::gainDb (p[(std::size_t) peq::param (band, 2)]);
    const auto q = peq::q (p[(std::size_t) peq::param (band, 3)]);

    switch (t)
    {
        case peq::BandType::off: return 0;
        case peq::BandType::peak:
            if (std::abs (db) < 0.01)
                return 0;
            out[0].peaking (sr, hz, q, db);
            return 1;
        case peq::BandType::lowShelf:
        case peq::BandType::highShelf:
            if (std::abs (db) < 0.01)
                return 0;
            out[0].shelf (sr, hz, db, t == peq::BandType::highShelf, std::clamp (q, 0.1, 4.0)); // Q = shelf slope S
            return 1;
        case peq::BandType::lowCut:
        case peq::BandType::highCut:
        {
            // cascaded: the Qs of a Butterworth of that order, scaled by the band Q (0.707 = Butterworth)
            const auto stages = peq::slopeStages (p[(std::size_t) (peq::slopeParam + band)]);
            const auto& qs = butterworthQ[(std::size_t) (stages - 1)];
            for (int s = 0; s < stages; ++s)
                out[(std::size_t) s].filter (sr, t == peq::BandType::lowCut ? 1 : 0, hz, stages == 1 ? q : qs[(std::size_t) s] * q / 0.70710678);
            return stages;
        }
        case peq::BandType::bandPass:
            out[0].filter (sr, 2, hz, q);
            return 1;
        case peq::BandType::notch:
        {
            const auto w = 2.0 * pi * hz / sr;
            const auto alpha = std::sin (w) / (2.0 * q);
            out[0].set (1, -2 * std::cos (w), 1, 1 + alpha, -2 * std::cos (w), 1 - alpha);
            return 1;
        }
    }

    return 0;
}

class ParametricEq final : public Effect
{
public:
    ParametricEq() = default;
    // a slot imported from FL: its engine is set up at once, so the latency is known before the first block
    explicit ParametricEq (const EffectParams& params)
    {
        if (peq::flLaw (params))
        {
            lastParams = params;
            flMode = true;
            fl.set (peqfl::fromParams (params));
        }
    }

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        if (! flMode)
            lastParams.fill (-1.0f);
        fl.prepare (sr);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& band : stages)
            for (auto& s : band)
                s.reset();
        fl.reset();
    }

    int latencySamples() const noexcept override { return flMode ? fl.latencySamples() : 0; }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        if (peq::flLaw (params))
        {
            // FL's Parametric EQ 2 / Parametric EQ, exact (core/ParametricEqFl.h)
            if (params != lastParams || ! flMode)
            {
                lastParams = params;
                flMode = true;
                fl.set (peqfl::fromParams (params));
            }
            fl.process (l, r, n);
            return;
        }
        flMode = false;

        if (params != lastParams)
        {
            lastParams = params;
            active = 0;

            for (int b = 0; b < peq::bands; ++b)
            {
                std::array<Biquad, 4> fresh;
                const auto count = bandStages (params, b, sampleRate, fresh);

                // keep the state of filters that remain (no click when turning a knob)
                for (int s = 0; s < count; ++s)
                {
                    auto& stage = stages[(std::size_t) b][(std::size_t) s];
                    stage.b0 = fresh[(std::size_t) s].b0; stage.b1 = fresh[(std::size_t) s].b1; stage.b2 = fresh[(std::size_t) s].b2;
                    stage.a1 = fresh[(std::size_t) s].a1; stage.a2 = fresh[(std::size_t) s].a2;
                }

                for (int s = count; s < 4; ++s)
                    stages[(std::size_t) b][(std::size_t) s].reset();

                for (int s = 0; s < count; ++s)
                    order[(std::size_t) active++] = &stages[(std::size_t) b][(std::size_t) s];
            }

            output = (float) std::pow (10.0, peq::gainDb (params[peq::outputParam]) / 20.0);
        }

        for (int k = 0; k < active; ++k)
            order[(std::size_t) k]->processBlock (l, r, n);

        if (output != 1.0f)
            for (int i = 0; i < n; ++i)
            {
                l[i] *= output;
                r[i] *= output;
            }
    }

private:
    double sampleRate = 44100.0;
    std::array<std::array<Biquad, 4>, peq::bands> stages;
    std::array<Biquad*, peq::bands * 4> order {};
    int active = 0;
    float output = 1.0f;
    EffectParams lastParams {};
    peqfl::Engine fl;
    bool flMode = false;
};

// ---------------------------------------------------------------------------
// Utility: stereo matrix (L->L, R->L, L->R, R->R), balance and gain.

float utilityGainDb (float v) { return (v - 0.5f) * 48.0f; }   // ±24 dB
float utilityMatrix (float v) { return v * 4.0f - 2.0f; }      // −2..2 (0.75 = 1.0)

// Volume gate: 32 steps per 4-beat bar (Gross Beat's "volume"). Per-step gain,
// mix with the original signal and attack/release smoothing.
namespace gate
{
constexpr int steps = 32;
constexpr int mixParam = 32, attackParam = 33, releaseParam = 34;
inline double attackMs (float v) { return std::pow (501.0, (double) v) - 1.0; }
inline double releaseMs (float v) { return 2.0 * (std::pow (501.0, (double) v) - 1.0); }
} // namespace gate

class VolumeGate final : public Effect
{
public:
    void prepare (double sr) noexcept override { sampleRate = sr; }
    void reset() noexcept override { gain = 1.0f; }
    void setSongPosition (double ticks) noexcept override { tick = ticks; }

    void process (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept override
    {
        const auto mix = std::clamp (p[gate::mixParam], 0.0f, 1.0f);
        const auto up = (float) (1.0 - std::exp (-1.0 / (std::max (0.05, gate::attackMs (p[gate::attackParam])) * 0.001 * sampleRate)));
        const auto down = (float) (1.0 - std::exp (-1.0 / (std::max (0.05, gate::releaseMs (p[gate::releaseParam])) * 0.001 * sampleRate)));
        const auto ticksPerSample = std::max (1.0, bpm) / 60.0 * ppq / sampleRate;
        const auto bar = 4.0 * ppq;
        auto position = std::fmod (std::max (0.0, tick), bar);

        for (int i = 0; i < n; ++i)
        {
            const auto step = std::clamp ((int) (position / bar * gate::steps), 0, gate::steps - 1);
            const auto target = 1.0f - mix + mix * std::clamp (p[(std::size_t) step], 0.0f, 1.0f);
            gain += (target - gain) * (target > gain ? up : down);
            l[i] *= gain;
            r[i] *= gain;
            position += ticksPerSample;
            if (position >= bar)
                position -= bar;
        }
        tick += ticksPerSample * n;
    }

private:
    double sampleRate = 44100.0, tick = 0.0;
    float gain = 1.0f;
};

// Real-time pitch shift: two read heads on a delay line whose delay varies as a sawtooth
// (the "rotating tape"); when one head wraps around, the other is mid-window and the
// sine/cosine crossfade hides the jump. Parameters: semitones (±24), cents (±100), mix, window.
namespace pitch
{
inline double semitones (float v) { return std::round ((std::clamp ((double) v, 0.0, 1.0) - 0.5) * 48.0); }
inline double cents (float v) { return (std::clamp ((double) v, 0.0, 1.0) - 0.5) * 200.0; }
inline double windowMs (float v) { return 20.0 + 100.0 * std::clamp ((double) v, 0.0, 1.0); }
} // namespace pitch

class PitchShift final : public Effect
{
public:
    static constexpr double maxSeconds = 0.13;

    PitchShift()
    {
        const auto size = (std::size_t) (maxSeconds * maxSampleRate) + 8;
        buffer[0].assign (size, 0.0f);
        buffer[1].assign (size, 0.0f);
    }

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        length = (int) std::min<std::size_t> (buffer[0].size(), (std::size_t) (maxSeconds * sr) + 8);
        reset();
    }

    void reset() noexcept override
    {
        std::fill (buffer[0].begin(), buffer[0].end(), 0.0f);
        std::fill (buffer[1].begin(), buffer[1].end(), 0.0f);
        write = 0;
        phase = 0.0;
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        const auto ratio = std::pow (2.0, (pitch::semitones (p[0]) + pitch::cents (p[1]) / 100.0) / 12.0);
        const auto mix = std::clamp (p[2], 0.0f, 1.0f);
        const auto window = std::min (pitch::windowMs (p[3]) * 0.001 * sampleRate, (double) length - 4.0);
        const auto step = (1.0 - ratio) / window; // the delay grows (pitch down) or shrinks (pitch up)

        for (int i = 0; i < n; ++i)
        {
            float* io[2] = { l + i, r + i };
            buffer[0][(std::size_t) write] = *io[0];
            buffer[1][(std::size_t) write] = *io[1];

            const auto phaseB = phase + 0.5 >= 1.0 ? phase - 0.5 : phase + 0.5;
            const auto gainA = (float) std::sin (3.141592653589793 * phase);
            const auto gainB = (float) std::sin (3.141592653589793 * phaseB);

            for (int c = 0; c < 2; ++c)
            {
                const auto shifted = gainA * read (c, 1.0 + phase * window) + gainB * read (c, 1.0 + phaseB * window);
                *io[c] += (shifted - *io[c]) * mix;
            }

            phase += step;
            phase -= std::floor (phase);
            if (++write >= length)
                write = 0;
        }
    }

private:
    float read (int c, double delay) const noexcept
    {
        auto position = (double) write - delay;
        while (position < 0.0)
            position += length;
        const auto i0 = (int) position;
        const auto frac = (float) (position - i0);
        const auto i1 = i0 + 1 >= length ? 0 : i0 + 1;
        const auto& b = buffer[(std::size_t) c];
        return b[(std::size_t) i0] + (b[(std::size_t) i1] - b[(std::size_t) i0]) * frac;
    }

    std::array<std::vector<float>, 2> buffer;
    double sampleRate = 44100.0, phase = 0.0;
    int length = 1, write = 0;
};

// Frequency shifter: the analytic signal (quadrature pair from two chains of 2nd-order allpasses
// in z², a ~90° "Hilbert transformer" over the useful range) multiplied by a complex oscillator.
// Parameters: shift (±2 kHz, quadratic curve around the center), mix, stereo spread
// (the right side shifts the opposite way).
namespace fshift
{
inline double hz (float v)
{
    const auto x = (std::clamp ((double) v, 0.0, 1.0) - 0.5) * 2.0;
    return 2000.0 * x * std::abs (x);
}
} // namespace fshift

// Lo-fi: holds each sample for 128·t³ + 1 samples and quantizes to 65536/(2048·b² + 1) levels
// (the laws of FL Effector's Lo-fi, REA_rodada18 §3.2); linear dry/wet mix.
namespace lofi
{
int holdSamples (float v) { return (int) (128.0f * v * v * v) + 1; }
double levels (float v) { return 65536.0 / (2048.0 * v * v + 1.0); }
} // namespace lofi

// Crossover (a band of FL's Frequency Splitter, REA_rodada17 §1.2): high-pass and/or low-pass with the
// structure of each slope — 6 dB: 1st order; 12: 1 biquad Q 0.5 (inverted HP); 18: Butterworth 3
// (in FL it is an undocumented all-pass structure: approximation); 24: 2× Q 0.707; 36: Q 0.5412/1.3066/
// 0.7070 (inverted HP); 48: Butterworth 4 squared; 72: Butterworth 6 squared; 96: Butterworth 8
// squared. Parameters: 0 HP on, 1 HP cutoff, 2 HP slope, 3 LP on, 4 LP cutoff, 5 LP slope.
namespace xover
{
double hz (float v) { return 30.0 * std::pow (2.0, 9.0589 * std::clamp (v, 0.0f, 1.0f)); }
int combo (float v) { return std::clamp ((int) std::lround (v * 7.0f), 0, 7); }
constexpr int dbPerOctave[8] { 6, 12, 18, 24, 36, 48, 72, 96 };

struct Design
{
    std::vector<double> qs; // 2nd-order biquads
    bool firstOrder = false;
    bool invertHighPass = false;
};

Design design (int c)
{
    switch (c)
    {
        case 0: return { {}, true, false };
        case 1: return { { 0.5 }, false, true };
        case 2: return { { 1.0 }, true, false };
        case 3: return { { 0.7071, 0.7071 }, false, false };
        case 4: return { { 0.5412, 1.3066, 0.7070 }, false, true };
        case 5: return { { 0.5412, 1.3066, 0.5412, 1.3066 }, false, false };
        case 6: return { { 0.5176, 0.7071, 1.9319, 0.5176, 0.7071, 1.9319 }, false, false };
        default: return { { 0.5098, 0.6013, 0.9000, 2.5629, 0.5098, 0.6013, 0.9000, 2.5629 }, false, false };
    }
}
} // namespace xover

class Crossover final : public Effect
{
public:
    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        last.fill (-1.0f);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& side : sides)
        {
            for (auto& b : side.biquads)
                b.reset();
            side.firstState[0] = side.firstState[1] = 0.0;
            side.firstIn[0] = side.firstIn[1] = 0.0;
        }
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        if (p[0] != last[0] || p[1] != last[1] || p[2] != last[2] || p[3] != last[3] || p[4] != last[4] || p[5] != last[5])
        {
            for (int i = 0; i < 6; ++i)
                last[(std::size_t) i] = p[(std::size_t) i];
            configure (sides[0], p[0] > 0.5f, true, xover::hz (p[1]), xover::combo (p[2]));
            configure (sides[1], p[3] > 0.5f, false, xover::hz (p[4]), xover::combo (p[5]));
        }

        for (auto& side : sides)
            if (side.active)
                for (int i = 0; i < n; ++i)
                {
                    l[i] = (float) side.process (l[i], 0);
                    r[i] = (float) side.process (r[i], 1);
                }
    }

private:
    struct Side
    {
        bool active = false, highPass = false, firstOrder = false;
        double sign = 1.0;
        std::array<Biquad, 8> biquads;
        int count = 0;
        double a = 0.0; // 1st order (bilinear transform)
        double firstState[2] {}, firstIn[2] {};

        double process (double x, int ch) noexcept
        {
            if (firstOrder)
            {
                // 1st-order low-pass: y = a·(x + x[n−1]) + (1 − 2a)·y[n−1]; the high-pass is the complement
                const auto lp = a * (x + firstIn[ch]) + (1.0 - 2.0 * a) * firstState[ch];
                firstIn[ch] = x;
                firstState[ch] = lp;
                x = highPass ? x - lp : lp;
            }
            for (int i = 0; i < count; ++i)
                x = biquads[(std::size_t) i].process (x, ch);
            return sign * x;
        }
    };

    void configure (Side& side, bool on, bool highPass, double hz, int combo)
    {
        side.active = on;
        if (! on)
            return;
        const auto d = xover::design (combo);
        side.highPass = highPass;
        side.firstOrder = d.firstOrder;
        side.sign = highPass && d.invertHighPass ? -1.0 : 1.0;
        side.count = (int) d.qs.size();
        const auto f = std::clamp (hz, 10.0, 0.49 * sampleRate);
        const auto k = std::tan (3.141592653589793 * f / sampleRate);
        side.a = k / (1.0 + k);
        for (int i = 0; i < side.count; ++i)
            side.biquads[(std::size_t) i].filter (sampleRate, highPass ? 1 : 0, f, d.qs[(std::size_t) i]);
    }

    double sampleRate = 44100.0;
    std::array<Side, 2> sides {};
    std::array<float, 6> last {};
};

// Rhythmic envelope (REA_rodada20 §1.2/§1.3): period in beats, up to 12 points (position 0..1 in the period, level,
// tension); the value between two points follows the curve of the destination point. Parameters: 0 period/64 beats,
// 1 point count/12, then 3 per point (position, level, (tension + 1)/2).
namespace rhythm
{
constexpr int maxPoints = 12;
double curve (double x, double t) noexcept
{
    if (std::abs (t) < 1e-6)
        return x;
    const auto q = std::pow (1000001.0, std::abs (t)) - 1.0;
    return t > 0.0 ? 1.0 - (std::pow (1.0 + q, 1.0 - x) - 1.0) / q : (std::pow (1.0 + q, x) - 1.0) / q;
}

double valueAt (const EffectParams& p, double u) noexcept
{
    const auto count = std::clamp ((int) std::lround (p[1] * maxPoints), 0, maxPoints);
    if (count == 0)
        return 1.0;
    auto prevX = 0.0, prevY = (double) p[3];
    for (int k = 0; k < count; ++k)
    {
        const auto x = (double) p[(std::size_t) (2 + 3 * k)], y = (double) p[(std::size_t) (3 + 3 * k)];
        if (u < x)
        {
            const auto f = x > prevX ? (u - prevX) / (x - prevX) : 1.0;
            return prevY + (y - prevY) * curve (f, 2.0 * p[(std::size_t) (4 + 3 * k)] - 1.0);
        }
        prevX = x;
        prevY = y;
    }
    return prevY;
}
} // namespace rhythm

class RhythmGain final : public Effect
{
public:
    void prepare (double sr) noexcept override { sampleRate = sr; }
    void reset() noexcept override {}
    void setSongPosition (double ticks) noexcept override { beats = ticks / 960.0; }

    void process (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept override
    {
        const auto period = std::max (1e-3, (double) p[0] * 64.0);
        const auto step = std::max (bpm, 1.0) / 60.0 / sampleRate;
        if (p[38] > 0.5f) // control only: the value becomes a live automation source, audio passes through untouched
        {
            const auto t = beats + step * n;
            current = (float) rhythm::valueAt (p, (t - std::floor (t / period) * period) / period);
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            const auto t = beats + step * i;
            const auto g = (float) rhythm::valueAt (p, (t - std::floor (t / period) * period) / period);
            l[i] *= g;
            r[i] *= g;
            current = g;
        }
    }

    float controlOutput (int) const noexcept override { return current; }

private:
    double sampleRate = 44100.0, beats = 0.0;
    float current = 1.0f;
};

// Auto-pan (REA_rodada20 §1.5): triangle (+1 at φ = 0, −1 at 0.5) with phase 0.75 + offset + beats/period.
// Parameters: 0 period/64 beats, 1 depth ((a + 1)/2, a = maximum pan excursion), 2 phase
// offset (cycles), 3 fixed pan ((p + 1)/2). Pan law: the utility's balance (FL's has not been decoded).
} // namespace

// FL's mode-0 pan law (REA_rodada27 §3.1): G_L = √2·cos((1 + pan)π/4), G_R = √2·sin((1 + pan)π/4) (unit volume),
// the near side keeps 1 − f/2 of its input and gets f/2 of the other, f = 1 − (far gain); no clamp of pan
void flPanMatrix (double pan, float& ll, float& rl, float& lr, float& rr) noexcept
{
    const auto angle = (1.0 + pan) * 3.141592653589793 / 4.0;
    const auto gl = std::sqrt (2.0) * std::cos (angle), gr = std::sqrt (2.0) * std::sin (angle);
    ll = (float) gl, rr = (float) gr, rl = lr = 0.0f;
    if (pan < 0.0)
    {
        const auto f = 1.0 - gr;
        ll = (float) (gl * (1.0 - f / 2.0));
        rl = (float) (gl * f / 2.0);
    }
    else if (pan > 0.0)
    {
        const auto f = 1.0 - gl;
        rr = (float) (gr * (1.0 - f / 2.0));
        lr = (float) (gr * f / 2.0);
    }
}

namespace
{
// Fruity Squeeze (REA_rodada29 §2), float32 like FL: silence bypass per block → (filter, pre) → quantiser
// Lq = 32768^(1 − Steps/32768) → periodic gate Impact/Preserve (samples at 44.1 kHz × fs/44100) → (filter, post)
// → (dry·(128 − Mix) + wet·Mix)/128 × Volume/127. The SVF (2 passes per sample, LP/HP/BP/notch/peak) is always
// designed for 44.1 kHz, whatever the rate (as FL does). Parameters are FL's raw values divided by their maximum.
} // namespace
const std::array<float, 12> squeezeMax { 160, 128, 32768, 1, 4, 65536, 65536, 1, 3, 44100, 44100, 65536 };
namespace
{
class Squeeze final : public Effect
{
public:
    void prepare (double sr) noexcept override
    {
        rate = (float) (sr / 44100.0);
        reset();
    }
    void reset() noexcept override
    {
        for (auto& f : filters)
            f.low = f.band = 0.0f;
        counterA = counterB = 0.0f;
        gateOn = false;
        last.fill (-1.0f);
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        bool changed = false;
        for (std::size_t i = 0; i < 12; ++i)
            if (p[i] != last[i])
                last[i] = p[i], changed = true;
        if (changed)
            configure (p);
        bool silent = true;
        for (int i = 0; i < n && silent; ++i)
            silent = std::abs (l[i]) <= 1e-6f && std::abs (r[i]) <= 1e-6f;
        if (silent)
            return; // FL leaves (near-)silent blocks untouched
        for (int i = 0; i < n; ++i)
        {
            const float dryL = l[i], dryR = r[i];
            float x[2] { l[i], r[i] };
            if (! post && filterOn)
                for (int c = 0; c < 2; ++c)
                    x[c] = filters[(std::size_t) c].tick (x[c], type, coefF, coefQ);
            if (steps < 32768.0f)
                for (auto& v : x)
                    v = (float) std::nearbyint (v * steps) / steps; // round half to even, like the DLL
            if (impact > 0.0f)
            {
                float w0, w1;
                if (! gateOn)
                {
                    if (counterA <= preserve)
                        counterA += 1.0f, w1 = 0.0f, w0 = 1.0f;
                    else
                    {
                        gateOn = true;
                        const auto fr = counterA - preserve;
                        w1 = fr, w0 = 1.0f - fr, counterB = fr, counterA = 0.0f;
                    }
                }
                else if (counterB <= impact)
                    counterB += 1.0f, w1 = 1.0f, w0 = 0.0f;
                else
                {
                    gateOn = false;
                    const auto fr = counterB - impact;
                    w1 = 1.0f - fr, w0 = fr, counterB = 0.0f, counterA = fr;
                }
                const auto g = (float) (amountRaw * 1.52587890625e-05 * w1);
                for (auto& v : x)
                    v = g * v + w0 * v;
            }
            if (post && filterOn)
                for (int c = 0; c < 2; ++c)
                    x[c] = filters[(std::size_t) c].tick (x[c], type, coefF, coefQ);
            l[i] = (dryL * dry + x[0] * wet) * gain;
            r[i] = (dryR * dry + x[1] * wet) * gain;
        }
    }

private:
    struct Svf
    {
        float low = 0.0f, band = 0.0f;
        float tick (float in, int mode, float f, float q) noexcept
        {
            constexpr float tiny = 1.1754943508222875e-38f;
            if (std::abs (in) < tiny)
                in = 0.0f;
            float acc[5] {};
            for (int k = 0; k < 2; ++k)
            {
                auto t = in - q * band;
                if (std::abs (t) < tiny)
                    t = 0.0f;
                low = low + f * band;
                const auto high = t - low;
                band = f * high + band;
                const float values[5] { low, band, high, t, low - high };
                for (int i = 0; i < 5; ++i)
                    acc[i] += values[i] * 0.5f;
            }
            static constexpr int pick[5] { 0, 2, 1, 3, 4 }; // FL's types: LP, HP, BP, notch, peak
            return acc[pick[std::clamp (mode, 0, 4)]];
        }
    };

    int raw (const EffectParams& p, int k) const noexcept { return (int) std::lround (std::clamp (p[(std::size_t) k], 0.0f, 1.0f) * squeezeMax[(std::size_t) k]); }

    void configure (const EffectParams& p) noexcept
    {
        gain = (float) raw (p, 0) * (1.0f / 127.0f);
        const auto mix = (float) raw (p, 1);
        dry = (128.0f - mix) / 128.0f;
        wet = mix / 128.0f;
        steps = std::pow (32768.0f, 1.0f - (float) raw (p, 2) / 32768.0f);
        filterOn = raw (p, 3) != 0;
        type = raw (p, 4);
        post = raw (p, 7) == 1;
        amountRaw = raw (p, 11);
        auto gateLength = [this] (int v) { return (std::pow (44101.0f, (float) v / 44100.0f) - 1.0f) * rate; };
        const auto preserveRaw = raw (p, 10);
        auto impactRaw = raw (p, 9);
        switch (raw (p, 8)) // Relation: Impact follows Preserve
        {
            case 1: impactRaw = preserveRaw; break;
            case 2: impactRaw = preserveRaw * 2; break;
            case 3: impactRaw = preserveRaw >> 1; break;
            default: break;
        }
        impact = gateLength (impactRaw);
        preserve = gateLength (preserveRaw);
        // SVF at 44.1 kHz: cutoff exp-mapped between 50 Hz and 0.75·44100, q = min(2, 2/f − f/2, 2(1 − res^0.25))
        const float fs = 44100.0f, lo = 50.0f, hi = fs * 0.75f;
        const auto x = (float) raw (p, 6) / 65536.0f, res = (float) raw (p, 5) / 65536.0f;
        auto hz = std::exp (x * (std::log (hi) - std::log (lo)) + std::log (lo));
        hz = std::clamp (hz, lo, hi);
        const auto ratio = std::min (hz / (fs * 2.0f), 0.25f);
        coefF = (float) (std::sin (ratio * 3.141592653589793) * 2.0);
        const auto a = (1.0f - std::pow (res, 0.25f)) * 2.0f;
        coefQ = std::min (std::min (2.0f / coefF - coefF * 0.5f, 2.0f), a);
    }

    float rate = 1.0f, gain = 1.0f, dry = 0.5f, wet = 0.5f, steps = 32768.0f, impact = 0.0f, preserve = 0.0f;
    float coefF = 0.5f, coefQ = 1.0f, counterA = 0.0f, counterB = 0.0f;
    int type = 0, amountRaw = 0;
    bool filterOn = false, post = false, gateOn = false;
    std::array<Svf, 2> filters {};
    std::array<float, 12> last {};
};

class AutoPan final : public Effect
{
public:
    void prepare (double sr) noexcept override { sampleRate = sr; rhythm.prepare (sr); dcA = (float) std::exp (-2.0 * 3.14159265358979323846 * 8.0 / sr); }
    void reset() noexcept override { rhythm.reset(); lfo = 0.0; smooth = 0.0; dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f; }
    void setSongPosition (double ticks) noexcept override { beats = ticks / 960.0; }

    void process (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept override
    {
        constexpr int rb = autopan::count; // round 136's Rhythm voicing params follow round 135's panner params
        if (p.size() > rb + 11 && p[rb] > 0.5f) // round 136: the Rhythm voicing (core/RhythmPan.h)
        {
            rhythmpan::Settings s;
            s.rateHz = rhythmpan::rateHz (p[rb + 1]);
            s.sync = p[rb + 2] > 0.5f;
            s.beats = rhythmpan::beats (p[rb + 3]);
            s.width = 210.0 * std::clamp ((double) p[rb + 4], 0.0, 1.0);
            s.offset = -105.0 + 210.0 * std::clamp ((double) p[rb + 5], 0.0, 1.0);
            s.smoothing = std::clamp ((double) p[rb + 6], 0.0, 1.0);
            s.pattern = p[rb + 7] > 0.5f ? 1 : 0;
            s.amount = std::clamp ((double) p[rb + 8], 0.0, 1.0);
            s.phase = std::clamp ((double) p[rb + 9], 0.0, 1.0);
            s.analog = p[rb + 10] < 0.25f ? 0 : (p[rb + 10] < 0.75f ? 1 : 2); // off / analog / hot
            s.outputDb = -24.0 + 48.0 * std::clamp ((double) p[rb + 11], 0.0, 1.0);
            // round 150: the Tweak page (envelope → rate / offset / width), Feel / Groove, past the speakers (appended)
            s.rateMod = -3.0 + 6.0 * std::clamp ((double) p[rb + 12], 0.0, 1.0);
            s.offsetMod = -180.0 + 360.0 * std::clamp ((double) p[rb + 13], 0.0, 1.0);
            s.widthMod = -180.0 + 360.0 * std::clamp ((double) p[rb + 14], 0.0, 1.0);
            s.envThresholdDb = -60.0 + 60.0 * std::clamp ((double) p[rb + 15], 0.0, 1.0);
            s.envRangeDb = 1.0 + 39.0 * std::clamp ((double) p[rb + 16], 0.0, 1.0);
            s.feel = -0.25 + 0.5 * std::clamp ((double) p[rb + 17], 0.0, 1.0);
            s.groove = -0.25 + 0.5 * std::clamp ((double) p[rb + 18], 0.0, 1.0);
            s.beyond = p[rb + 19] >= 0.5f;
            if (p.size() > rb + 26) // round 152: the trigger sources and the Random source (appended)
            {
                s.trigger = p[rb + 20] < 0.25f ? 0 : (p[rb + 20] < 0.75f ? 1 : 2);
                s.trigAudio = p[rb + 21] < 0.999f; // fully up = no audio trigger (the manual one still works)
                s.trigThresholdDb = -60.0 + 60.0 * std::clamp ((double) p[rb + 21], 0.0, 1.0);
                s.divider = 1 + (int) std::lround (11.0 * std::clamp ((double) p[rb + 22], 0.0, 1.0));
                s.trigFilter = p[rb + 23] < 0.25f ? 0 : (p[rb + 23] < 0.75f ? 1 : 2);
                s.trigFreqHz = 20.0 * std::pow (1000.0, std::clamp ((double) p[rb + 24], 0.0, 1.0));
                s.manual = p[rb + 25];
                s.randomSteps = p[rb + 26] >= 0.5f;
            }
            if (! (s == rhythmSet))
            {
                rhythm.set (s);
                rhythmSet = s;
            }
            rhythm.process (l, r, n, beats, bpm);
            current = rhythm.currentPan();
            position.store (current, std::memory_order_relaxed);
            beats += n * std::max (bpm, 1.0) / 60.0 / sampleRate;
            return;
        }
        if (autopan::sourceOf (p[autopan::source]) != autopan::Source::fl)
        {
            processPanner (l, r, n, p, bpm);
            return;
        }
        const auto period = std::max (1e-3, (double) p[0] * 64.0);
        const auto depth = 2.0 * p[1] - 1.0, fixed = 2.0 * p[3] - 1.0;
        const auto step = std::max (bpm, 1.0) / 60.0 / sampleRate / period;
        auto phase = 0.75 + p[2] + beats / period;
        {
            const auto f = phase + step * n - std::floor (phase + step * n);
            current = (float) ((f < 0.5 ? 1.0 - 4.0 * f : -3.0 + 4.0 * f) + 1.0) * 0.5f; // wave in 0..1
        }
        position.store (0.5f * (float) (std::clamp (fixed + depth * (2.0 * current - 1.0), -1.0, 1.0) + 1.0), std::memory_order_relaxed);
        if (p[4] > 0.5f) // control only (live automation source)
            return;
        // LFO tension (Love Philter, REA_rodada27 §3.3): applied to |w| with the sign kept, T = 256·(p6 − 0.5)
        const auto tension = 256.0 * (p[6] - 0.5);
        const auto q = tension == 0.0 ? 0.0 : (tension > 0.0 ? 1.0 : -1.0) * (std::pow (1e18, 0.05 * (std::pow (21.0, std::abs (tension) / 128.0) - 1.0)) - 1.0);
        const auto flLaw = p[5] > 0.5f;
        for (int i = 0; i < n; ++i, phase += step)
        {
            const auto f = phase - std::floor (phase);
            auto w = f < 0.5 ? 1.0 - 4.0 * f : -3.0 + 4.0 * f;
            if (q != 0.0)
            {
                const auto y = std::abs (w);
                const auto shaped = q > 0.0 ? 1.0 - (std::pow (1.0 + q, 1.0 - y) - 1.0) / q : (std::pow (1.0 - q, y) - 1.0) / -q;
                w = w < 0.0 ? -shaped : shaped;
            }
            if (flLaw) // FL's mode-0 pan law with the far side folded into the near one (REA_rodada27 §3.1)
            {
                float ll, rl, lr, rr;
                flPanMatrix (fixed + depth * w, ll, rl, lr, rr);
                const auto a = l[i], b = r[i];
                l[i] = ll * a + rl * b;
                r[i] = lr * a + rr * b;
                continue;
            }
            const auto pan = std::clamp (fixed + depth * w, -1.0, 1.0);
            l[i] *= (float) (pan > 0.0 ? 1.0 - pan : 1.0);
            r[i] *= (float) (pan < 0.0 ? 1.0 + pan : 1.0);
        }
    }

    // 0: the wave (0..1), 1: the pan position (0 = left, 0.5 = centre, 1 = right; round 135's display)
    float controlOutput (int o) const noexcept override
    {
        switch (o)
        {
            case 1: return position.load (std::memory_order_relaxed);
            case 2: return peakIn.load (std::memory_order_relaxed);
            case 3: return peakOut.load (std::memory_order_relaxed);
            default: return current;
        }
    }

private:
    // round 135: the rhythmic panner's sources (the PanMan role), measured black-box on the official trial
    // (docs/analise/REA_rodada135s.md §3): an equal-power balance (centre −3 dB per side, the far side silent at the
    // extremes), a free LFO in Hz or steps on the beat grid swept back and forth or one way, Smoothing from a hard
    // square / jump to a soft sine / glide, input and output gains and an "analog" stage (asymmetric soft saturation:
    // H2 −36 dBc and H3 −54 dBc at −18 dBFS, H2 −25 / H3 −30 at −6 dBFS)
    void processPanner (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept
    {
        using namespace autopan;
        const auto src = sourceOf (p[source]);
        const auto dir = directionOf (p[direction]);
        const auto soft = std::clamp ((double) p[smoothing], 0.0, 1.0);
        const auto depth = 2.0 * p[1] - 1.0, fixed = 2.0 * p[3] - 1.0;
        const auto gIn = (float) std::pow (10.0, inputDb (p[inputGain]) / 20.0);
        const auto gOut = (float) std::pow (10.0, inputDb (p[outputGain]) / 20.0);
        const auto analog = p[analogStage] >= 0.5f;
        const auto k = 0.05 + 12.0 * (1.0 - soft) * (1.0 - soft); // the sine's hardness (soft = a sine, hard ≈ a square)
        const auto tk = std::tanh (k);
        const auto steps = stepCount (p[stepsParam]);
        // the period of one cycle: the free rate (Hz), or the steps on the beat grid
        double inc;
        if (src == Source::lfo)
            inc = rateHz (p[rate]) / sampleRate;
        else
            inc = std::max (bpm, 1.0) / 60.0 / sampleRate / (stepBeats (p[stepNote]) * (dir == Direction::both ? 2.0 * (steps - 1) : steps));
        // rhythm steps follow the song position (like the FL source); the LFO runs freely
        if (src == Source::steps && beats != syncedBeats) // a new song position: back on the grid
        {
            lfo = beats / (stepBeats (p[stepNote]) * (dir == Direction::both ? 2.0 * (steps - 1) : steps));
            syncedBeats = beats;
        }
        // steps glide over up to a third of a step (Smoothing 100 %), or jump (0 %)
        const auto stepSec = stepBeats (p[stepNote]) * 60.0 / std::max (bpm, 1.0);
        const auto glide = 1.0 - std::exp (-1.0 / (sampleRate * (0.0005 + 0.35 * soft * stepSec)));
        float pos = 0.0f, pin = 0.0f, pout = 0.0f;
        for (int i = 0; i < n; ++i, lfo += inc)
        {
            const auto f = lfo - std::floor (lfo);
            double w; // −1 … +1
            if (src == Source::lfo)
            {
                if (dir == Direction::both)
                    w = std::tanh (k * std::sin (2.0 * 3.14159265358979323846 * f)) / tk;
                else
                    w = dir == Direction::right ? 2.0 * f - 1.0 : 1.0 - 2.0 * f;
                smooth = w;
            }
            else
            {
                int s;
                if (dir == Direction::both)
                {
                    const auto span = 2 * (steps - 1);
                    const auto at = (int) std::floor (f * span);
                    s = at < steps ? at : span - at;
                }
                else
                {
                    s = std::min (steps - 1, (int) std::floor (f * steps));
                    if (dir == Direction::left) s = steps - 1 - s;
                }
                const auto target = steps > 1 ? -1.0 + 2.0 * s / (steps - 1) : 0.0;
                smooth += glide * (target - smooth);
                w = smooth;
            }
            const auto pan = std::clamp (fixed + depth * w, -1.0, 1.0);
            const auto th = (pan + 1.0) * 0.25 * 3.14159265358979323846;
            pin = std::max (pin, std::max (std::abs (l[i]), std::abs (r[i])));
            float a = l[i] * gIn, b = r[i] * gIn;
            if (analog)
            {
                a = analogCurve (a, 0);
                b = analogCurve (b, 1);
            }
            l[i] = (float) (a * std::cos (th)) * gOut;
            r[i] = (float) (b * std::sin (th)) * gOut;
            pout = std::max (pout, std::max (std::abs (l[i]), std::abs (r[i])));
            pos = (float) pan;
        }
        lfo -= std::floor (lfo);
        current = (float) (0.5 * (smooth + 1.0));
        position.store (0.5f * (pos + 1.0f), std::memory_order_relaxed);
        peakIn.store (pin, std::memory_order_relaxed);
        peakOut.store (pout, std::memory_order_relaxed);
    }

    float analogCurve (float x, int c) noexcept
    {
        const auto u = std::clamp (x, -1.2f, 1.2f);
        const auto y = u + 0.25f * u * u - 0.45f * u * u * u; // fitted on the trial's harmonics; DC removed after it
        dcY[c] = y - dcX[c] + dcA * dcY[c];
        dcX[c] = y;
        return dcY[c];
    }

    double sampleRate = 44100.0, beats = 0.0, lfo = 0.0, smooth = 0.0, syncedBeats = -1.0;
    float current = 0.5f, dcA = 0.999f, dcX[2] {}, dcY[2] {};
    std::atomic<float> position { 0.5f }, peakIn { 0.0f }, peakOut { 0.0f };
    rhythmpan::Engine rhythm; // round 136
    rhythmpan::Settings rhythmSet;
};

// Vocoder = FL's Fruity Vocoder, exact engine (core/FruityVocoder.h, REA_rodada70; laws first read in REA_rodada20
// §4.1). Parameters (0..1): 0 bands (4 << round(5v)), 1 order (1 + round(2v)), 2 listen (0 modulator, 1 carrier,
// 2 mix), 3 swap L/R, then FL's twelve controls as (raw − min) / (max − min): 4/5 FreqMin/Max (0…65536), 6 FreqScale
// (0…128), 7 invert, 8 Formant (−1200…1200 cents), 9 band width (5…200 %), 10/11 attack/release (10…1000 ms),
// 12 hold, 13/14/15 modulator/carrier/wet levels (0…160); 16 LYNX's side-input mode. The per-band levels of the
// analysis bank travel in EffectSlot::state (FL's saved state); they apply while the band count matches.
namespace vocoder
{
int bands (float v) { return 4 << std::clamp ((int) std::lround (v * 5.0f), 0, 5); }
int order (float v) { return 1 + std::clamp ((int) std::lround (v * 2.0f), 0, 2); }
int raw (const EffectParams& p, int control)
{
    const auto lo = fvocoder::controlMin (control), hi = fvocoder::controlMax (control);
    return lo + (int) std::lround (std::clamp (p[(std::size_t) (4 + control)], 0.0f, 1.0f) * (float) (hi - lo));
}
int raw (float v, int control)
{
    const auto lo = fvocoder::controlMin (control), hi = fvocoder::controlMax (control);
    return lo + (int) std::lround (std::clamp (v, 0.0f, 1.0f) * (float) (hi - lo));
}
} // namespace vocoder

class Vocoder final : public Effect
{
public:
    explicit Vocoder (const std::vector<std::uint8_t>& state)
    {
        if (auto s = fvocoder::parse (state))
            saved = *s;
        last.fill (-1.0f);
    }

    void prepare (double sr) noexcept override
    {
        engine.prepare (sr);
        reset();
    }

    void reset() noexcept override { engine.reset(); }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        bool changed = false;
        for (std::size_t i = 0; i < 16; ++i)
            if (p[i] != last[i])
            {
                last[i] = p[i];
                changed = true;
            }
        if (changed)
        {
            auto s = engine.state();
            s.bands = vocoder::bands (p[0]);
            s.order = vocoder::order (p[1]);
            s.listen = std::clamp ((int) std::lround (p[2] * 2.0f), 0, 2);
            s.swap = p[3] > 0.5f;
            for (int c = 0; c < fvocoder::controls; ++c)
                s.c[(std::size_t) c] = fvocoder::rawFromKnob (p[(std::size_t) (4 + c)], c, saved.c[(std::size_t) c]);
            if (s.bands == saved.bands)
                s.levels = saved.levels;
            else
                s.levels.fill (1.0f);
            engine.setState (s);
        }

        // "Side inputs" (Vocodex-style): modulator = side input 1 or the own input, carrier = side input 2 or the own
        // input, each in mono (L + R)/2; a configured side input that is silent gives silence
        const auto separate = p[16] > 0.5f;
        const bool modSide = useSide[0], carSide = useSide[1];
        useSide = { false, false };
        if (! separate)
        {
            engine.processStereo (l, r, n);
            return;
        }
        for (int done = 0; done < n;)
        {
            const auto len = std::min (n - done, fvocoder::blockFrames);
            for (int i = 0; i < len; ++i)
            {
                const auto k = (std::size_t) i, j = (std::size_t) (done + i);
                const auto own = 0.5f * (l[j] + r[j]);
                mod[k] = modSide ? (side[0][0] != nullptr ? 0.5f * (side[0][0][j] + side[0][1][j]) : 0.0f) : own;
                car[k] = carSide ? (side[1][0] != nullptr ? 0.5f * (side[1][0][j] + side[1][1][j]) : 0.0f) : own;
            }
            engine.process (mod.data(), car.data(), l + done, len);
            std::copy (l + done, l + done + len, r + done);
            done += len;
        }
    }

    void setSidechain (const float* left, const float* right) noexcept override
    {
        side[0] = { left, right };
        useSide[0] = true;
    }

    void setSidechain2 (const float* left, const float* right) noexcept override
    {
        side[1] = { left, right };
        useSide[1] = true;
    }

private:
    fvocoder::Engine engine;
    fvocoder::State saved;
    std::array<float, 16> last {};
    std::array<std::array<const float*, 2>, 2> side {};
    std::array<bool, 2> useSide { false, false };
    std::array<float, fvocoder::blockFrames> mod {}, car {};
};

// Pitch correction (FL's Pitcher; laws in REA_rodada15 §1, rodada19 §6 and rodada30 §2). Parameters: 0 Correction
// Speed, 1 Fine Tune, 2 Bypass All, 3 Note A Freq, 4 range (index/5: min f 220/170/110/80/55/25 Hz), 5…16 note
// mask C…B (1 = allowed), 17…28 Key Bypass C…B, 29 Formant, 30 Gender, 31 Pitch Reference (0 left, 0.5
// independent, 1 right), 32 Stereo, 33 Harmonize, 34 MIDI, 35 Octaves, 36 Replace/Mix, 37 Vel, 38…41 Voice 1…4 pan.
// The DSP is FL's (core/PitcherDsp.h). MIDI mode and Harmonize take held notes, but LYNX effects receive no MIDI:
// no note is ever held, so MIDI mode corrects nothing (only Fine applies) and Harmonize passes the voice-0 dry
// signal (Replace) or the dry input alone (Mix) — what FL does with nothing arriving on the Pitcher's MIDI port.
namespace pcorr
{
constexpr double minHz[6] { 220.0, 170.0, 110.0, 80.0, 55.0, 25.0 };
int range (float v) { return std::clamp ((int) std::lround (v * 5.0f), 0, 5); }
double noteA (float v) { return 55.0 * std::pow (2.0, (2.0 * v - 1.0) / 12.0); } // ×8 = A 440 at v = 0.5
} // namespace pcorr

class PitchCorrect final : public Effect
{
public:
    PitchCorrect() { dsp.reserve (maxSampleRate); }

    void prepare (double sr) noexcept override { dsp.prepare (sr); }
    void reset() noexcept override { dsp.reset(); }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        auto on = [&p] (int i) { return p[(std::size_t) i] > 0.5f; };
        pitcher::Params q;
        q.speed = std::clamp (p[0], 0.0f, 1.0f);
        q.fine = p[1];
        q.bypassAll = on (2);
        q.aRef = p[3];
        q.range = pcorr::range (p[4]);
        for (int k = 0; k < 12; ++k)
        {
            q.scale[(std::size_t) k] = on (5 + k);
            q.keyBypass[(std::size_t) k] = on (17 + k);
        }
        q.formant = on (29);
        q.gender = p[30];
        q.pitchRef = p[31];
        q.stereo = p[32];
        q.harmonize = on (33);
        q.midi = on (34);
        q.octaves = on (35);
        q.mix = on (36);
        q.vel = on (37);
        for (int j = 0; j < 4; ++j)
            q.voicePan[(std::size_t) j] = p[(std::size_t) (38 + j)];
        dsp.process (l, r, n, q);
        latency.store (dsp.latency(), std::memory_order_relaxed);
    }

    // The DSP's own delay: 9 samples on the plain path, 257 on the formant path (its pass-through delay; voiced
    // PSOLA grains are placed about one period + 70 samples back). FL's reported PDC latency is not confirmed.
    int latencySamples() const noexcept override { return latency.load (std::memory_order_relaxed); }

private:
    pitcher::Pitcher dsp;
    std::atomic<int> latency { 9 };
};

class LoFi final : public Effect
{
public:
    void prepare (double) noexcept override { reset(); }
    void reset() noexcept override
    {
        held[0] = held[1] = 0.0f;
        countdown = 0;
    }

    void process (float* l, float* r, int n, const EffectParams& params, double) noexcept override
    {
        const auto hold = lofi::holdSamples (params[0]);
        const auto step = (float) (2.0 / lofi::levels (params[1]));
        const auto mix = params[2];
        auto quantize = [step] (float x) { return step * std::floor (x / step + 0.5f); };

        for (int i = 0; i < n; ++i)
        {
            if (--countdown <= 0)
            {
                countdown = hold;
                held[0] = quantize (l[i]);
                held[1] = quantize (r[i]);
            }
            l[i] += mix * (held[0] - l[i]);
            r[i] += mix * (held[1] - r[i]);
        }
    }

private:
    float held[2] {};
    int countdown = 0;
};

class FreqShift final : public Effect
{
public:
    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        reset();
    }

    void reset() noexcept override
    {
        for (auto& c : chains)
            c = {};
        phase = 0.0;
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        const auto shift = fshift::hz (p[0]);
        const auto mix = std::clamp (p[1], 0.0f, 1.0f);
        const auto spread = std::clamp (p[2], 0.0f, 1.0f);
        const auto step = shift / sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const auto angle = 2.0 * 3.141592653589793 * phase;
            const auto cosA = (float) std::cos (angle), sinA = (float) std::sin (angle);
            float* io[2] = { l + i, r + i };

            for (int c = 0; c < 2; ++c)
            {
                float re = 0.0f, im = 0.0f;
                chains[(std::size_t) c].analytic (*io[c], re, im);
                // right side: between the same direction (0) and the opposite (1)
                const auto sign = c == 0 ? 1.0f : 1.0f - 2.0f * spread;
                const auto shifted = re * cosA + sign * im * sinA;
                *io[c] += (shifted - *io[c]) * mix;
            }

            phase += step;
            phase -= std::floor (phase);
        }
    }

private:
    // 4 2nd-order allpasses in z² per branch; the real branch has 1 extra sample of delay
    struct Chain
    {
        static constexpr float a[4] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
        static constexpr float b[4] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };
        std::array<float, 4> ax1 {}, ax2 {}, ay1 {}, ay2 {}, bx1 {}, bx2 {}, by1 {}, by2 {};
        float delayed = 0.0f;

        static float stage (float k, float x, float& x1, float& x2, float& y1, float& y2) noexcept
        {
            const auto y = k * k * (x + y2) - x2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }

        void analytic (float x, float& re, float& im) noexcept
        {
            auto u = x, v = x;
            for (std::size_t k = 0; k < 4; ++k)
            {
                u = stage (a[k], u, ax1[k], ax2[k], ay1[k], ay2[k]);
                v = stage (b[k], v, bx1[k], bx2[k], by1[k], by2[k]);
            }
            re = delayed;
            im = v;
            delayed = u;
        }
    };

    std::array<Chain, 2> chains {};
    double sampleRate = 44100.0, phase = 0.0;
};

// Parallel-branch container (FL's Patcher): each branch processes a copy of the input through its
// chain of built-in effects and the output is the gain-weighted sum of the branches.
class ParallelEffect final : public Effect
{
public:
    static constexpr int capacity = 4096; // samples per chunk (larger blocks are split)

    explicit ParallelEffect (const EffectSlot& slot)
    {
        for (const auto& b : slot.branches)
        {
            Branch branch;
            branch.gain = b.gain;
            for (const auto& child : b.chain)
                if (child.kind != EffectKind::plugin) // nested containers too (Patcher series-parallel graphs)
                    branch.chain.push_back ({ makeEffect (child), child });
            branches.push_back (std::move (branch));
        }
        for (const auto& link : slot.links)
            if (link.branch >= 0 && link.branch < (int) branches.size() && link.curve.size() >= 2)
            {
                const auto& chain = branches[(std::size_t) link.branch].chain;
                if (link.source >= 0 && link.source < (int) chain.size() && link.target >= 0 && link.target < (int) chain.size()
                    && link.param >= 0 && link.param < maxEffectParams)
                    links.push_back (link);
            }
        for (auto& buffer : scratch)
            buffer.assign ((std::size_t) capacity, 0.0f);
    }

    void prepare (double sr) noexcept override
    {
        for (auto& b : branches)
            for (auto& c : b.chain)
                c.effect->prepare (sr);
    }

    void reset() noexcept override
    {
        for (auto& b : branches)
            for (auto& c : b.chain)
                c.effect->reset();
    }

    void setSongPosition (double ticks) noexcept override
    {
        for (auto& b : branches)
            for (auto& c : b.chain)
                c.effect->setSongPosition (ticks);
    }

    void setPlaying (bool playing) noexcept override
    {
        for (auto& b : branches)
            for (auto& c : b.chain)
                c.effect->setPlaying (playing);
    }

    void setHeldNotes (const NoteMask* channels, int count) noexcept override
    {
        for (auto& b : branches)
            for (auto& c : b.chain)
                c.effect->setHeldNotes (channels, count);
    }

    void process (float* l, float* r, int n, const EffectParams&, double bpm) noexcept override
    {
        for (int done = 0; done < n; done += capacity)
            processPiece (l + done, r + done, std::min (capacity, n - done), bpm);
    }

    // the longest branch's latency (the branches themselves are not aligned; FL's Patcher PDC is not confirmed)
    int latencySamples() const noexcept override
    {
        int longest = 0;
        for (const auto& b : branches)
        {
            int sum = 0;
            for (const auto& c : b.chain)
                if (c.slot.enabled && c.slot.mix > 0.0f)
                    sum += c.effect->latencySamples();
            longest = std::max (longest, sum);
        }
        return longest;
    }

private:
    struct Child
    {
        std::unique_ptr<Effect> effect;
        EffectSlot slot;
    };
    struct Branch
    {
        std::vector<Child> chain;
        float gain = 1.0f;
    };

    void processPiece (float* l, float* r, int n, double bpm) noexcept
    {
        auto* inL = scratch[0].data();
        auto* inR = scratch[1].data();
        auto* sumL = scratch[2].data();
        auto* sumR = scratch[3].data();
        auto* wL = scratch[4].data();
        auto* wR = scratch[5].data();
        auto* dryL = scratch[6].data();
        auto* dryR = scratch[7].data();
        std::copy (l, l + n, inL);
        std::copy (r, r + n, inR);
        std::fill (sumL, sumL + n, 0.0f);
        std::fill (sumR, sumR + n, 0.0f);

        // inner links: the control child's latest output sets the target's parameter (one block of lag)
        for (const auto& link : links)
        {
            auto& chain = branches[(std::size_t) link.branch].chain;
            const auto x = std::clamp (chain[(std::size_t) link.source].effect->controlOutput (0), 0.0f, 1.0f) * (float) (link.curve.size() - 1);
            const auto i = std::min ((std::size_t) x, link.curve.size() - 2);
            const auto f = x - (float) i;
            chain[(std::size_t) link.target].slot.params[(std::size_t) link.param] = link.curve[i] + (link.curve[i + 1] - link.curve[i]) * f;
        }

        for (auto& b : branches)
        {
            std::copy (inL, inL + n, wL);
            std::copy (inR, inR + n, wR);
            for (auto& c : b.chain)
            {
                if (! c.slot.enabled || c.slot.mix <= 0.0f)
                    continue;
                const auto partial = c.slot.mix < 0.999f;
                if (partial)
                {
                    std::copy (wL, wL + n, dryL);
                    std::copy (wR, wR + n, dryR);
                }
                c.effect->process (wL, wR, n, c.slot.params, bpm);
                if (partial)
                    for (int i = 0; i < n; ++i)
                    {
                        wL[i] = dryL[i] + (wL[i] - dryL[i]) * c.slot.mix;
                        wR[i] = dryR[i] + (wR[i] - dryR[i]) * c.slot.mix;
                    }
            }
            for (int i = 0; i < n; ++i)
            {
                sumL[i] += wL[i] * b.gain;
                sumR[i] += wR[i] * b.gain;
            }
        }

        std::copy (sumL, sumL + n, l);
        std::copy (sumR, sumR + n, r);
    }

    std::vector<Branch> branches;
    std::vector<EffectLink> links;
    std::array<std::vector<float>, 8> scratch;
};

// Peak Controller (REA_rodada17 §5): audio passes straight through. Detector with instant attack (block peak)
// and LINEAR decay in musical time: (10001^(D/128) − 1)·0.0192 per quarter note. Peak output =
// base + a·shape(p) with a = ±0.4·(6^(|A|/256) − 1) and FL's tension; output 0 = Peak + LFO, 1 = Peak,
// 2 = LFO (its LFO is not generated yet: the LFO base is used). Parameters = FL raw values, normalized.
class PeakController final : public Effect
{
public:
    void prepare (double sr) noexcept override { sampleRate = sr; }
    void reset() noexcept override { detector = 0.0; }

    void process (float* l, float* r, int n, const EffectParams& p, double bpm) noexcept override
    {
        const auto amountRaw = (std::clamp ((double) p[1], 0.0, 1.0) * 2.0 - 1.0) * 256.0;
        const auto tensionRaw = (std::clamp ((double) p[2], 0.0, 1.0) * 2.0 - 1.0) * 128.0;
        const auto decayRaw = 5.0 + 123.0 * std::clamp ((double) p[3], 0.0, 1.0);
        const auto perBeat = (std::pow (10001.0, decayRaw / 128.0) - 1.0) * 0.0192;

        float blockPeak = 0.0f;
        for (int i = 0; i < n; ++i)
            blockPeak = std::max ({ blockPeak, std::abs (l[i]), std::abs (r[i]) });
        detector = std::max (0.0, detector - perBeat * std::max (1.0, bpm) / 60.0 * n / sampleRate);
        detector = std::max (detector, (double) blockPeak);

        const auto amount = (amountRaw < 0.0 ? -1.0 : 1.0) * (std::pow (6.0, std::abs (amountRaw) / 256.0) - 1.0) * 0.4;
        double shaped = detector;
        if (std::abs (tensionRaw) > 1e-9)
        {
            const auto k = (std::pow (1001.0, std::abs (tensionRaw) / 128.0) - 1.0) * 0.1;
            const auto y = tensionRaw > 0.0 ? k - (std::pow (k + 1.0, 1.0 - detector) - 1.0) : std::pow (k + 1.0, detector) - 1.0;
            shaped = y / k;
        }
        peakOut = (float) std::clamp (p[0] + amount * shaped, 0.0, 1.0);
        lfoOut = std::clamp (p[4], 0.0f, 1.0f);
    }

    float controlOutput (int output) const noexcept override
    {
        return output == 1 ? peakOut : output == 2 ? lfoOut : std::clamp (peakOut + lfoOut, 0.0f, 1.0f);
    }

private:
    double sampleRate = 44100.0, detector = 0.0;
    float peakOut = 0.0f, lfoOut = 0.0f;
};

class Utility final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        if (p[0] != lastGainParam) // pow only when the parameter changes (Patchers have dozens of utilities)
        {
            lastGainParam = p[0];
            cachedGain = p[0] <= 0.0f ? 0.0f : std::pow (10.0f, utilityGainDb (p[0]) / 20.0f); // at the bottom = mute
        }
        const auto gain = cachedGain;
        const auto pan = std::clamp (p[1] * 2.0f - 1.0f, -1.0f, 1.0f);
        const auto gl = gain * (pan > 0.0f ? 1.0f - pan : 1.0f); // balance: lowers the opposite side
        const auto gr = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
        const auto ll = utilityMatrix (p[2]), rl = utilityMatrix (p[3]), lr = utilityMatrix (p[4]), rr = utilityMatrix (p[5]);

        // shortcuts: identity (common in Patchers' PanOMatic/Stereo Shaper) and mute
        if (gl == 0.0f && gr == 0.0f)
        {
            std::fill (l, l + n, 0.0f);
            std::fill (r, r + n, 0.0f);
            return;
        }
        if (rl == 0.0f && lr == 0.0f)
        {
            const auto fl = ll * gl, fr = rr * gr;
            if (fl == 1.0f && fr == 1.0f)
                return;
            for (int i = 0; i < n; ++i)
            {
                l[i] *= fl;
                r[i] *= fr;
            }
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            const auto a = l[i], b = r[i];
            l[i] = (ll * a + rl * b) * gl;
            r[i] = (lr * a + rr * b) * gr;
        }
    }

private:
    float lastGainParam = -1.0f, cachedGain = 1.0f;
};
} // namespace

namespace peq
{
BandType type (float v) noexcept { return (BandType) std::clamp ((int) std::lround (v * (bandTypes - 1)), 0, bandTypes - 1); }
float typeValue (BandType t) noexcept { return (float) (int) t / (float) (bandTypes - 1); }
double hz (float v) noexcept { return 20.0 * std::pow (1000.0, std::clamp ((double) v, 0.0, 1.0)); }
float hzValue (double f) noexcept { return (float) std::clamp (std::log (std::max (f, 1.0) / 20.0) / std::log (1000.0), 0.0, 1.0); }
double gainDb (float v) noexcept { return (std::clamp ((double) v, 0.0, 1.0) - 0.5) * 36.0; }
float gainValue (double db) noexcept { return (float) std::clamp (db / 36.0 + 0.5, 0.0, 1.0); }
double q (float v) noexcept { return 0.1 * std::pow (180.0, std::clamp ((double) v, 0.0, 1.0)); }
float qValue (double value) noexcept { return (float) std::clamp (std::log (std::max (value, 0.01) / 0.1) / std::log (180.0), 0.0, 1.0); }
int slopeStages (float v) noexcept { return 1 + (int) std::lround (std::clamp (v, 0.0f, 1.0f) * 3.0f); }
float slopeValue (int stages) noexcept { return (float) (std::clamp (stages, 1, 4) - 1) / 3.0f; }

const char* typeName (BandType t) noexcept
{
    static constexpr const char* names[] = { "Off", "Peak", "Low shelf", "High shelf", "Low cut",
                                             "High cut", "Band-pass", "Notch" };
    return names[(int) t];
}

double responseDb (const EffectParams& params, double sr, double f)
{
    if (flLaw (params))
        return peqfl::responseDb (peqfl::fromParams (params), sr, f);

    // |H(e^jw)| of each stage, summed in dB
    const auto w = 2.0 * pi * std::clamp (f, 1.0, sr * 0.499) / sr;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    auto total = gainDb (params[outputParam]);

    for (int b = 0; b < bands; ++b)
    {
        std::array<Biquad, 4> st;
        const auto count = bandStages (params, b, sr, st);

        for (int s = 0; s < count; ++s)
        {
            const auto& c = st[(std::size_t) s];
            const auto h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
            const auto magnitude = std::abs (h);
            if (std::isfinite (magnitude)) // (the editor draws this: never NaN)
                total += 20.0 * std::log10 (std::max (magnitude, 1.0e-9));
        }
    }

    return total;
}
bool flLaw (const EffectParams& params) noexcept { return peqfl::lawOf (params) != peqfl::Law::lynx; }

std::string orderText (const EffectParams& params, int band)
{
    const auto v = params[(std::size_t) (slopeParam + band)];
    if (! flLaw (params))
        return std::to_string (12 * slopeStages (v)) + " dB/oct";
    if (peqfl::lawOf (params) == peqfl::Law::peq1)
        return "12 dB/oct";
    // FL's order texts, −3…+3 (REA_rodada7 §1.4)
    static constexpr const char* names[7] = { "Steep 8", "Steep 6", "Steep 4", "2", "Gentle 4", "Gentle 6", "Gentle 8" };
    return names[std::clamp ((int) std::lround (v * 6.0f), 0, 6)];
}

std::string widthText (const EffectParams& params, int band)
{
    const auto v = params[(std::size_t) param (band, 3)];
    char text[32];
    if (flLaw (params))
        std::snprintf (text, sizeof (text), "BW %.0f%%", (double) v * 100.0);
    else
        std::snprintf (text, sizeof (text), "Q %.2f", q (v));
    return text;
}
} // namespace peq

namespace autopan
{
Source sourceOf (float v) noexcept { return (Source) std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 2.0f), 0, 2); }
Direction directionOf (float v) noexcept { return (Direction) std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 2.0f), 0, 2); }
double rateHz (float v) noexcept { return 0.01 * std::pow (1000.0, (double) std::clamp (v, 0.0f, 1.0f)); }
float rateValue (double hz) noexcept { return (float) std::clamp (std::log (std::max (hz, 1e-6) / 0.01) / std::log (1000.0), 0.0, 1.0); }
int stepCount (float v) noexcept { return 2 + std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 6.0f), 0, 6); }
double stepBeats (float v) noexcept { return 4.0 / (double) (1 << std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 5.0f), 0, 5)); }
double inputDb (float v) noexcept { return -24.0 + 48.0 * (double) std::clamp (v, 0.0f, 1.0f); }
std::string paramText (int param, float v)
{
    char t[32];
    switch (param)
    {
        case source: { static const char* const n[3] { "FL LFO", "LFO", "Rhythm steps" }; return n[(int) sourceOf (v)]; }
        case rate:
        {
            const auto hz = rateHz (v);
            if (hz < 1.0) std::snprintf (t, sizeof t, "%.1f s", 1.0 / hz);
            else std::snprintf (t, sizeof t, "%.2f Hz", hz);
            return t;
        }
        case direction: { static const char* const n[3] { "Back and forth", "Left to right", "Right to left" }; return n[(int) directionOf (v)]; }
        case smoothing: std::snprintf (t, sizeof t, "%.0f%%", 100.0 * std::clamp (v, 0.0f, 1.0f)); return t;
        case stepsParam: return std::to_string (stepCount (v));
        case stepNote: { static const char* const n[6] { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" }; return n[std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 5.0f), 0, 5)]; }
        case inputGain:
        case outputGain: std::snprintf (t, sizeof t, "%+.1f dB", inputDb (v)); return t;
        case analogStage: return v >= 0.5f ? "Analog" : "Digital";
        default: std::snprintf (t, sizeof t, "%.2f", (double) v); return t;
    }
}
} // namespace autopan

const EffectInfo& effectInfo (EffectKind kind)
{
    static const EffectInfo eq { "EQ", { { "Low", 0.5f }, { "Mid", 0.5f }, { "Mid freq.", 0.5f }, { "High", 0.5f }, { "Output", 0.5f } } };
    static const EffectInfo comp { "Compressor", { { "Threshold", 0.7f }, { "Ratio", 3.0f / 19.0f }, { "Attack", 0.67f }, { "Release", 0.54f }, { "Makeup", 0.0f }, { "Auto gain", 0.0f },
                                                  { "Mix", 1.0f }, { "Trim", 0.5f }, { "Voicing", 0.0f },
                                                  // round 135w: the RComp role's switches (the vintage voicing's ARC / Electro / Warm
                                                  // by default, as fitted) and its 0 dBFS output limiter (off: older projects unchanged)
                                                  { "Release mode", 0.0f }, { "Release curve", 0.0f }, { "Character", 0.0f }, { "Limiter", 0.0f },
                                                  // round 150: the RComp role's own ranges (off: LYNX's, older projects unchanged) —
                                                  // ratio 1.00 : 1, gain 0 dB, attack 16 ms, release 160 ms (the reference's defaults)
                                                  { "Ranges", 0.0f }, { "RC ratio", 0.625f }, { "RC gain", 0.5f }, { "RC attack", 0.5f }, { "RC release", 0.5f },
                                                  // round 152: the vintage voicing's static curve measured at every ratio (on for new
                                                  // instances; projects saved before keep the round-150 curve: upgradeLoadedEffectParams)
                                                  { "Vintage curve", 1.0f } } };
    static const EffectInfo delay { "Delay", { { "Time", 0.3f }, { "Feedback", 0.42f }, { "Mix", 0.3f }, { "Ping-pong", 0.0f }, { "Damping", 0.3f } } };
    static const EffectInfo reverb { "Reverb", { { "Room", 0.6f }, { "Damping", 0.5f }, { "Width", 1.0f }, { "Mix", 0.25f }, { "Dry", 1.0f },
                                                 { "Low cut", 0.0f }, { "High cut", 1.0f }, { "Predelay", 0.0f } } };
    static const EffectInfo plugin { "Plugin", {} };
    static const EffectInfo limiter { "Limiter", { { "Ceiling", 0.975f }, { "Gain", 0.0f }, { "Release", 0.5f } } };
    static const EffectInfo filter { "Filter", { { "Type", 0.0f }, { "Cutoff", 0.75f }, { "Reso.", 0.2f }, { "Mix", 1.0f } } };
    static const EffectInfo distortion { "Distortion", { { "Drive", 0.4f }, { "Tone", 0.7f }, { "Mix", 1.0f }, { "Output", 0.5f } } };
    static const EffectInfo chorus { "Chorus", { { "Rate", 0.45f }, { "Depth", 0.4f }, { "Mix", 0.5f }, { "Feedback", 0.0f } } };
    static const EffectInfo utility { "Utility", { { "Gain", 0.5f }, { "Balance", 0.5f }, { "L -> L", 0.75f }, { "R -> L", 0.5f },
                                                      { "L -> R", 0.5f }, { "R -> R", 0.75f } } };
    static const EffectInfo volumeGate = [] {
        static const char* names[] = { "Step 1", "Step 2", "Step 3", "Step 4", "Step 5", "Step 6", "Step 7", "Step 8",
                                       "Step 9", "Step 10", "Step 11", "Step 12", "Step 13", "Step 14", "Step 15", "Step 16",
                                       "Step 17", "Step 18", "Step 19", "Step 20", "Step 21", "Step 22", "Step 23", "Step 24",
                                       "Step 25", "Step 26", "Step 27", "Step 28", "Step 29", "Step 30", "Step 31", "Step 32" };
        EffectInfo info { "Volume gate", {} };
        for (int i = 0; i < gate::steps; ++i)
            info.params.push_back ({ names[i], (i % 8) < 4 ? 1.0f : 0.0f }); // default: half a beat on, half off
        info.params.push_back ({ "Mix", 1.0f });
        info.params.push_back ({ "Attack", 0.1765f });  // ~2 ms
        info.params.push_back ({ "Release", 0.5f });    // ~43 ms
        return info;
    }();
    static const EffectInfo pitchShift { "Pitch shift", { { "Semitones", 0.5f }, { "Cents", 0.5f }, { "Mix", 1.0f }, { "Window", 0.4f } } };
    static const EffectInfo freqShift { "Frequency shifter", { { "Shift", 0.5f }, { "Mix", 1.0f }, { "Stereo", 0.0f } } };
    static const EffectInfo parallelInfo { "Patcher (parallel)", {} };
    static const EffectInfo patcherInfo { "Patcher", {} };
    static const EffectInfo crossoverInfo { "Crossover", { { "High-pass", 0.0f }, { "HP cutoff", 0.3f }, { "HP slope", 3.0f / 7.0f },
                                                           { "Low-pass", 0.0f }, { "LP cutoff", 0.7f }, { "LP slope", 3.0f / 7.0f } } };
    static const EffectInfo rhythmInfo = [] {
        EffectInfo info { "Rhythmic envelope", { { "Period", 1.0f / 64.0f }, { "Points", 0.0f } } };
        static const char* names[] = { "P1 pos", "P1 level", "P1 tension", "P2 pos", "P2 level", "P2 tension", "P3 pos", "P3 level", "P3 tension",
                                       "P4 pos", "P4 level", "P4 tension", "P5 pos", "P5 level", "P5 tension", "P6 pos", "P6 level", "P6 tension",
                                       "P7 pos", "P7 level", "P7 tension", "P8 pos", "P8 level", "P8 tension", "P9 pos", "P9 level", "P9 tension",
                                       "P10 pos", "P10 level", "P10 tension", "P11 pos", "P11 level", "P11 tension", "P12 pos", "P12 level", "P12 tension" };
        for (int i = 0; i < 36; ++i)
            info.params.push_back ({ names[i], i % 3 == 2 ? 0.5f : 1.0f });
        info.params.push_back ({ "Control only", 0.0f });
        return info;
    }();
    static const EffectInfo autoPanInfo { "Auto-pan", { { "Period", 1.0f / 64.0f }, { "Depth", 0.75f }, { "Phase", 0.0f }, { "Pan", 0.5f }, { "Control only", 0.0f },
                                                        { "FL pan law", 0.0f }, { "Tension", 0.5f },
                                                        // round 135 (the PanMan role; the FL source stays the default)
                                                        { "Source", 0.0f }, { "Rate", 0.46598f }, { "Direction", 0.0f }, { "Smoothing", 1.0f },
                                                        { "Steps", 1.0f / 6.0f }, { "Step note", 0.4f }, { "Input", 0.5f }, { "Output", 0.5f },
                                                        { "Analog", 0.0f },
                                                        // round 136: the Rhythm voicing (core/RhythmPan.h, measured), off = the engines above
                                                        { "Voicing", 0.0f }, { "Rhythm rate", 0.4659f /* 0.25 Hz */ }, { "Sync", 0.0f },
                                                        { "Beats", 0.4f /* 1 beat */ }, { "Width", 180.0f / 210.0f }, { "Offset", 0.5f },
                                                        { "Rhythm smoothing", 1.0f }, { "Pattern", 0.0f }, { "Amount", 1.0f }, { "Start phase", 0.0f },
                                                        { "Rhythm analog", 0.5f }, { "Rhythm output", 0.5f },
                                                        // round 150: the Tweak page, Feel / Groove, past the speakers (appended;
                                                        // a project saved before round 150 keeps the clipped angle)
                                                        { "Rate mod", 0.5f }, { "Offset mod", 0.5f }, { "Width mod", 0.5f },
                                                        { "Env threshold", 40.0f / 60.0f }, { "Env range", 10.0f / 39.0f },
                                                        { "Feel", 0.5f }, { "Groove", 0.5f }, { "Beyond speakers", 1.0f },
                                                        // round 152: the trigger sources (Ping-pong / Random step), their
                                                        // threshold, divider, filter and manual trigger; the Random source
                                                        { "Trigger", 0.0f }, { "Trig threshold", 40.0f / 60.0f }, { "Divider", 0.0f },
                                                        { "Trig filter", 0.0f }, { "Trig freq", 0.5f }, { "Manual trigger", 0.0f },
                                                        { "Random steps", 0.0f } } };
    static const EffectInfo vocoderInfo { "Vocoder", { { "Bands", 0.4f }, { "Order", 0.5f }, { "Listen", 1.0f }, { "Swap L/R", 0.0f },
                                                       { "Min freq.", 0.0f }, { "Max freq.", 1.0f }, { "Scale", 0.5f }, { "Invert", 0.0f },
                                                       { "Formant", 0.5f }, { "Width", 45.0f / 195.0f }, { "Attack", 10.0f / 990.0f },
                                                       { "Release", 90.0f / 990.0f }, { "Hold", 0.0f }, { "Modulator", 0.0f },
                                                       { "Carrier", 0.0f }, { "Wet", 0.8f }, { "Side inputs", 0.0f } } };
    static const EffectInfo vocodexInfo { "Vocodex", {} }; // everything comes from FL's raw state (EffectSlot::state)
    static const EffectInfo maximusInfo { "Maximus", {} }; // the engine's state travels in EffectSlot::state (core/MaximusFx.h)
    // Distructor: each module's input volume, mix and output volume (FL's 0…10000 / 10000), the final clipper; the
    // modules themselves come from FL's state (core/DistructorFx.h)
    static const EffectInfo distructorInfo { "Distructor", { { "M1 In", 0.5f }, { "M1 Mix", 1.0f }, { "M1 Out", 0.5f },
                                                             { "M2 In", 0.5f }, { "M2 Mix", 1.0f }, { "M2 Out", 0.5f },
                                                             { "M3 In", 0.5f }, { "M3 Mix", 1.0f }, { "M3 Out", 0.5f },
                                                             { "M4 In", 0.5f }, { "M4 Mix", 1.0f }, { "M4 Out", 0.5f },
                                                             { "Clipper", 1.0f } } };
    // Multiband Delay: FL's twelve global controls (raw − min) / range; the banks come from FL's state (core/MultibandDelayFx.h)
    static const EffectInfo multibandDelayInfo = [] {
        EffectParams p {};
        mbdelay::toParams (mbdelay::State {}, p);
        EffectInfo info { "Multiband Delay", {} };
        const char* const names[mbdelay::numGlobals] = { "Delay scale", "Delay x0.1", "Feedback", "Smoothing", "Filter mode", "Keep pitch",
                                                         "Morph", "Mix", "Harmonic scale", "Weighting", "High pass", "Saturation" };
        for (int i = 0; i < mbdelay::numGlobals; ++i)
            info.params.push_back ({ names[i], p[(std::size_t) i] });
        return info;
    }();
    // EQUO: FL's knobs (raw − min) / range and options; the bands come from FL's state (core/EquoFx.h)
    static const EffectInfo equoInfo { "EQUO", { { "Band shift", 0.5f }, { "Band mix", 1.0f }, { "Morph", 0.0f },
                                                 { "Smoothness", 170.0f / 1024.0f }, { "Volume", 0.5f },
                                                 { "Bandwidth", 70.0f / 420.0f }, { "Oversample", 0.0f }, { "Add mode", 0.0f },
                                                 { "Send only", 0.0f } } };
    // FL's Pitch Shifter: its 13 knobs (raw − min) / range, then engine, detector, tempo sync; defaults = a new FL instance
    static const EffectInfo flPitchShifterInfo = [] {
        EffectParams p {};
        psfl::toParams (psfl::State {}, p);
        EffectInfo info { "Pitch Shifter", {} };
        const char* const names[psfl::numParams] = { "Pitch", "Formant", "Channel", "Mix", "Duration", "Density", "Jitter",
                                                     "Random", "Delay", "Feedback", "Delay mode", "Formant correction",
                                                     "Pitch mode", "Granular", "Accurate detector", "Tempo sync" };
        for (int i = 0; i < psfl::numParams; ++i)
            info.params.push_back ({ names[i], p[(std::size_t) i] });
        return info;
    }();
    // FL's raw controls normalised over their ranges (core/FruityDynamics.h); defaults = a new FL instance
    static const EffectInfo fruityLimiterInfo = [] {
        EffectInfo info { "Fruity Limiter", {} };
        for (int i = 0; i < fldyn::limiterControls; ++i)
            info.params.push_back ({ fldyn::limiterParamName (i), fldyn::normalised (fldyn::limiterDefault[(std::size_t) i], fldyn::limiterMin[(std::size_t) i], fldyn::limiterMax[(std::size_t) i]) });
        info.params.push_back ({ fldyn::limiterParamName (fldyn::limiterControls), 0.0f });
        return info;
    }();
    static const EffectInfo fruityCompressorInfo = [] {
        EffectInfo info { "Fruity Compressor", {} };
        for (int i = 0; i < fldyn::compressorControls; ++i)
            info.params.push_back ({ fldyn::compressorParamName (i), fldyn::normalised (fldyn::compressorDefault[(std::size_t) i], fldyn::compressorMin[(std::size_t) i], fldyn::compressorMax[(std::size_t) i]) });
        info.params.push_back ({ fldyn::compressorParamName (fldyn::compressorControls), 1.0f });
        return info;
    }();
    // FL's controls over their ranges (core/FruityShapers.h); defaults = a new FL instance; the curve is in EffectSlot::state
    static const EffectInfo waveShaperInfo { "Fruity WaveShaper", { { "Pre amp", 0.5f }, { "Mix", 1.0f }, { "Post gain", 1.0f },
                                                                    { "Oversampling", 0.0f }, { "HQ", 0.0f }, { "Smooth", 0.0f },
                                                                    { "Center", 0.0f }, { "Unipolar", 0.0f } } };
    static const EffectInfo softClipperInfo { "Fruity Soft Clipper", { { "Threshold", 99.0f / 126.0f }, { "Post", 128.0f / 160.0f } } };
    // FL's controls over their ranges (core/FruityBasicFx.h); defaults = a new FL instance
    static const EffectInfo panOMaticInfo { "Fruity PanOMatic", { { "Pan", 0.5f }, { "Volume", 0.8f }, { "LFO shape", 0.0f },
                                                                  { "LFO target", 0.0f }, { "LFO amount", 0.5f }, { "LFO speed", 0.5f } } };
    static const EffectInfo balanceInfo { "Fruity Balance", { { "Balance", 0.5f }, { "Volume", 0.8f } } };
    static const EffectInfo bloodOverdriveInfo { "Fruity Blood Overdrive", { { "Pre band", 0.0f }, { "Color", 0.5f }, { "Pre amp", 0.0f },
                                                                              { "x100", 0.0f }, { "Post filter", 0.0f }, { "Post gain", 1.0f } } };
    // FL's controls as (raw − min) / range (core/Reeverb2.h); defaults = FL's
    static const EffectInfo reeverb2Info = [] {
        EffectInfo info { "Reeverb 2", { { "Low cut", 0 }, { "High cut", 0 }, { "Predelay", 0 }, { "Room size", 0 }, { "Diffusion", 0 },
                                         { "Decay", 0 }, { "High damping", 0 }, { "Bass mult.", 0 }, { "Crossover", 0 }, { "Stereo sep.", 0 },
                                         { "Dry", 0 }, { "Early ref.", 0 }, { "Wet", 0 }, { "Mod speed", 0 }, { "Mod depth", 0 },
                                         { "Tempo predelay", 0 }, { "Mid/side input", 0 } } };
        EffectParams p {};
        reeverb2::toParams (reeverb2::Settings {}, p);
        for (std::size_t i = 0; i < info.params.size(); ++i)
            info.params[i].defaultValue = p[i];
        return info;
    }();
    // FL's 49 controls as (raw − min) / range + the 4 sync switches (core/LuxeVerb.h); defaults = a new FL instance
    static const EffectInfo luxeVerbInfo = [] {
        EffectInfo info { "LuxeVerb", {} };
        EffectParams p {};
        luxeverb::toParams (luxeverb::Settings {}, p);
        for (std::size_t i = 0; i < (std::size_t) luxeVerbParamCount; ++i)
            info.params.push_back ({ luxeverb::names[i], p[i] });
        return info;
    }();
    // FL's 4 automatable controls as (raw − min) / range (core/ConvolverFx.h); the impulse response is in the slot's state
    static const EffectInfo convolverInfo { "Fruity Convolver", { { "Dry", 1.0f }, { "Wet", 1.0f }, { "Input separation", 0.5f },
                                                                  { "Wet separation", 0.5f } } };
    // FL's controls as raw / max (core/Reeverb2.h); defaults = the plugin's own (1250, 10800, 0, 26000, 65536, 20, 4700,
    // 10800, 65536, 13120)
    static const EffectInfo reeverb1Info { "Reeverb", { { "Low cut", 1250.0f / 65536 }, { "High cut", 10800.0f / 65536 }, { "Predelay", 0.0f },
                                                        { "Room size", 26000.0f / 65536 }, { "Diffusion", 1.0f }, { "Color", 0.5f },
                                                        { "Decay", 4700.0f / 65536 }, { "High damping", 10800.0f / 65536 }, { "Dry", 1.0f },
                                                        { "Wet", 13120.0f / 65536 } } };
    // FL's raw controls normalised over their ranges (core/FruityDelay.h); defaults = FL's "Default" presets
    static const EffectInfo fruityDelay2Info = [] {
        EffectInfo info { "Fruity Delay 2", {} };
        for (int i = 0; i < fldelay::delay2Controls; ++i)
            info.params.push_back ({ fldelay::delay2ParamName (i), (float) (fldelay::delay2Default[(std::size_t) i] - fldelay::delay2Min[(std::size_t) i])
                                                                      / (float) (fldelay::delay2Max[(std::size_t) i] - fldelay::delay2Min[(std::size_t) i]) });
        return info;
    }();
    static const EffectInfo fruityDelay3Info = [] {
        EffectInfo info { "Fruity Delay 3", {} };
        for (int i = 0; i < fldelay::delay3Controls; ++i)
            info.params.push_back ({ fldelay::delay3ParamName (i), (float) (fldelay::delay3Default[(std::size_t) i] - fldelay::delay3Min[(std::size_t) i])
                                                                      / (float) (fldelay::delay3Max[(std::size_t) i] - fldelay::delay3Min[(std::size_t) i]) });
        return info;
    }();
    // FL's controls over their ranges (core/StereoFx.h); defaults = a new FL instance
    static const EffectInfo stereoShaperInfo { "Stereo Shaper", { { "R > L", 0.5f }, { "Left", 0.75f }, { "Right", 0.75f }, { "L > R", 0.5f },
                                                                 { "Delay", 0.5f }, { "Phase", 0.5f }, { "Effects post", 0.0f },
                                                                 { "Difference", 0.0f } } };
    static const EffectInfo stereoEnhancerInfo { "Stereo Enhancer", { { "Pan", 0.5f }, { "Volume", 0.8f }, { "Separation", 0.5f },
                                                                     { "Phase offset", 0.5f }, { "Effects post", 1.0f }, { "Invert", 0.0f } } };
    static const EffectInfo transientProcessorInfo { "Transient Processor", { { "Attack", 0.5f }, { "Drive", 0.0f }, { "Release", 0.5f },
                                                                             { "Gain", 0.5f }, { "Attack shape", 0.5f }, { "Release shape", 0.5f },
                                                                             { "Split freq.", 0.5f }, { "Split balance", 0.5f }, { "On", 1.0f } } };
    static const EffectInfo spreaderInfo { "Spreader", { { "Width", 0.5f }, { "Separation", 0.5f }, { "Low bypass", 9990.0f / 999000.0f },
                                                         { "Mono bass", 0.0f }, { "Enable", 1.0f } } };
    // FL's controls over their ranges (core/FruityFilters.h); defaults = a new FL instance
    static const EffectInfo fastLPInfo { "Fruity Fast LP", { { "Cutoff", 1.0f }, { "Resonance", 0.0f } } };
    static const EffectInfo freeFilterInfo { "Fruity Free Filter", { { "Type", 0.0f }, { "Frequency", 0.5f }, { "Q", 169.0f / 1024.0f },
                                                                    { "Gain", 676.0f / 1024.0f }, { "Center", 0.0f } } };
    // FL's Newtone / Newtime: the sample and its edits travel in EffectSlot::state / samplePath (core/NewtoneFx.h)
    static const EffectInfo newtoneInfo { "Newtone", { { "Slave to host playback", 0.0f } } };
    static const EffectInfo newtimeInfo { "Newtime", { { "Slave to host playback", 0.0f } } };
    static const EffectInfo fruityFilterInfo { "Fruity Filter", { { "Cutoff", 1.0f }, { "Resonance", 205.0f / 1024.0f }, { "Low pass", 1.0f },
                                                                 { "Band pass", 0.0f }, { "High pass", 0.5f }, { "x2", 0.0f }, { "Center", 0.0f } } };
    // FL's eight Gross Beat parameters (core/GrossBeat.h); the slots' envelopes travel in EffectSlot::state
    static const EffectInfo grossBeatInfo { "Gross Beat", { { "Time slot", 0.0f }, { "Volume slot", 0.0f }, { "Time mix", 1.0f },
                                                            { "Volume mix", 1.0f }, { "Attack", 0.1765f }, { "Release", 0.5f },
                                                            { "Tension", 0.5f }, { "Hold", 0.0f } } };
    // FL's state travels in EffectSlot::state; the four global controls are the editable parameters (raw / 128)
    static const EffectInfo fruityDelayBankInfo { "Fruity Delay Bank", { { "Dry", 1.0f }, { "Output", 1.0f }, { "Input", 1.0f }, { "Feedback", 1.0f } } };
    // FL's raw controls normalised over their ranges (core/FruityMultiband.h); defaults = a new FL instance
    static const EffectInfo fruityMultibandInfo = [] {
        EffectInfo info { "Fruity Multiband Compressor", {} };
        for (int i = 0; i < flmb::controls; ++i)
            info.params.push_back ({ flmb::paramName (i), flmb::normalised (flmb::controlDefault[(std::size_t) i], flmb::controlMin[(std::size_t) i], flmb::controlMax[(std::size_t) i]) });
        return info;
    }();
    // FL's controls over their ranges (core/FlangusFastDist.h); defaults = a new FL instance
    static const EffectInfo fastDistInfo { "Fruity Fast Dist", { { "Pre amp", 0.5f }, { "Threshold", 1.0f }, { "Type", 0.0f },
                                                                 { "Mix", 1.0f }, { "Post gain", 1.0f } } };
    static const EffectInfo flangusInfo { "Fruity Flangus", { { "Order", 3.0f / 8.0f }, { "Depth", 0.5f }, { "Speed", 0.75f },
                                                              { "Delay", 8.0f / 128.0f }, { "Spread", 1.0f }, { "Cross", 96.0f / 256.0f },
                                                              { "Dry", 224.0f / 256.0f }, { "Wet", 224.0f / 256.0f } } };
    static const EffectInfo flangerInfo { "Fruity Flanger", { { "Delay", 0.0f }, { "Depth", 0.4f }, { "Rate", 0.4f }, { "Phase", 118.0f / 360.0f },
                                                              { "Damp", 0.0f }, { "Shape", 0.0f }, { "Feed", 0.23f }, { "Invert feed", 0.0f },
                                                              { "Invert wet", 1.0f }, { "Dry", 0.7f }, { "Wet", 0.6f }, { "Cross", 0.5f } } };
    // raw FL values / max (the names are FL's controls, REA_rodada29 §2.1)
    static const EffectInfo squeezeInfo { "Squeeze", { { "Volume", 127.0f / 160.0f }, { "Mix", 0.5f }, { "Steps", 1.0f / 32768.0f },
                                                       { "Filter", 0.0f }, { "Filter type", 0.0f }, { "Resonance", 0.5f },
                                                       { "Cutoff", 0.5f }, { "Filter post", 0.0f }, { "Relation", 0.0f },
                                                       { "Impact", 0.0f }, { "Preserve", 7500.0f / 44100.0f }, { "Amount", 0.0f } } };
    // the engine's normalised values in core order (VST 22/23 swapped); defaults = the front-end's new instance
    static const EffectInfo vintagePhaserInfo { "Vintage Phaser", {
        { "Min freq.", 0.4375f }, { "Max freq.", 0.7758789f }, { "Feedback", 0.5f }, { "Manual", 0.0f }, { "Manual freq.", 0.0f },
        { "HQ", 0.0f }, { "LFO speed", 0.0f }, { "Tempo sync", 0.0f }, { "Sine", 0.0f }, { "Tension", 0.0f }, { "Start phase", 0.0f },
        { "L/R phase", 0.0f }, { "Delay", 0.0f }, { "Keep pitch", 0.0f }, { "Delay mix", 0.0f }, { "Delay stereo", 0.5f },
        { "Delay time", 0.0f }, { "Delay sync", 0.0f }, { "Delay fdbk", 0.0f }, { "Delay LP", 0.0f }, { "Delay HP", 0.0f },
        { "Mix", 0.5f }, { "Noise", 0.0f }, { "Mono input", 0.0f }, { "Gate", 0.0f }, { "Colour gain", 0.0f }, { "Colour freq.", 0.0f },
        { "Input HP", 0.0f }, { "Fdbk HP", 0.0f }, { "Rate 1", 0.0f }, { "Rate 2", 1.0f }, { "Depth scale", 1.0f },
        { "Pull-down", 0.0f }, { "Engine V2", 1.0f }, { "Drive", 0.0f }, { "Invert fdbk", 0.0f } } };
    static const EffectInfo pitchCorrectInfo = [] {
        EffectInfo info { "Pitch correction", { { "Speed", 1.0f }, { "Fine", 0.5f }, { "Bypass all", 0.0f }, { "A freq.", 0.5f },
                                                     { "Range", 0.4f } } };
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        for (const auto* name : names)
            info.params.push_back ({ name, 1.0f });
        static const char* bypass[] = { "C off", "C# off", "D off", "D# off", "E off", "F off", "F# off", "G off", "G# off", "A off", "A# off", "B off" };
        for (const auto* name : bypass)
            info.params.push_back ({ name, 0.0f });
        // added later (REA round 30); the defaults keep older projects as they were: plain shifter, no harmony
        info.params.insert (info.params.end(), { { "Formant", 0.0f }, { "Gender", 0.5f }, { "Pitch ref.", 0.0f }, { "Stereo", 1.0f },
                                                 { "Harmonize", 0.0f }, { "MIDI", 0.0f }, { "Octaves", 0.0f }, { "Mix", 0.0f },
                                                 { "Vel", 0.0f }, { "Voice 1 pan", 0.5f }, { "Voice 2 pan", 0.5f },
                                                 { "Voice 3 pan", 0.5f }, { "Voice 4 pan", 0.5f } });
        return info;
    }();
    static const EffectInfo lofiInfo { "Lo-fi", { { "Rate", 0.0f }, { "Bits", 0.0f }, { "Mix", 1.0f } } };
    // FL's Fruity Phaser: its nine raw controls / max (core/FruityPhaser.h); defaults = a new FL instance
    static const EffectInfo fruityPhaserInfo = [] {
        EffectInfo info { "Fruity Phaser", {} };
        float p[phaserfx::params] {};
        phaserfx::toParams (phaserfx::defaults, p);
        for (int i = 0; i < phaserfx::params; ++i)
            info.params.push_back ({ phaserfx::paramName (i), p[i] });
        return info;
    }();
    // FL's Fruity Send: pan, volume, dry over their ranges and the insert it sends to (core/FruityBasicFx.h)
    static const EffectInfo fruitySendInfo { "Fruity Send", { { "Pan", 0.5f }, { "Volume", 0.8f }, { "Dry", 1.0f }, { "Send to", 0.0f } } };
    static const EffectInfo effectorInfo { "Effector", { { "Type", 0.0f }, { "Bypass", 0.0f }, { "Wet", 1.0f }, { "X", 0.5f }, { "Y", 0.5f },
                                                         { "Mod rate", 0.5f }, { "Tempo red.", 0.0f }, { "Mod X", 0.5f }, { "Mod Y", 0.5f },
                                                         { "LFO shape", 0.0f }, { "Input", 1.0f }, { "Output", 0.5f } } };
    static const EffectInfo formulaControllerInfo { "Formula Controller", { { "a", 0.0f }, { "b", 0.0f }, { "c", 0.0f } } };
    static const EffectInfo peakController { "Peak Controller", { { "Base", 0.0f }, { "Amount", 0.8496f }, { "Tension", 0.5f },
                                                                  { "Decay", 0.4959f }, { "LFO base", 0.0f } } };
    static const EffectInfo parametric = [] {
        // bands as in a mixing EQ: low shelf, 5 peaks and high shelf, all at 0 dB
        constexpr peq::BandType types[] = { peq::BandType::lowShelf, peq::BandType::peak, peq::BandType::peak, peq::BandType::peak,
                                            peq::BandType::peak, peq::BandType::peak, peq::BandType::highShelf };
        constexpr double freqs[] = { 80.0, 200.0, 500.0, 1200.0, 3000.0, 6000.0, 12000.0 };
        static const char* names[peq::bands][4] = {
            { "B1 type", "B1 freq.", "B1 gain", "B1 Q" }, { "B2 type", "B2 freq.", "B2 gain", "B2 Q" },
            { "B3 type", "B3 freq.", "B3 gain", "B3 Q" }, { "B4 type", "B4 freq.", "B4 gain", "B4 Q" },
            { "B5 type", "B5 freq.", "B5 gain", "B5 Q" }, { "B6 type", "B6 freq.", "B6 gain", "B6 Q" },
            { "B7 type", "B7 freq.", "B7 gain", "B7 Q" } };
        static const char* slopes[peq::bands] = { "B1 slope", "B2 slope", "B3 slope", "B4 slope", "B5 slope", "B6 slope", "B7 slope" };
        EffectInfo info { "Parametric EQ", {} };

        for (int b = 0; b < peq::bands; ++b)
        {
            info.params.push_back ({ names[b][0], peq::typeValue (types[b]) });
            info.params.push_back ({ names[b][1], peq::hzValue (freqs[b]) });
            info.params.push_back ({ names[b][2], 0.5f });
            // Q 1.0 on shelves = shelf slope S 1; on cuts, 0.707 = Butterworth
            info.params.push_back ({ names[b][3], peq::qValue (types[b] == peq::BandType::lowCut || types[b] == peq::BandType::highCut ? 0.70710678 : 1.0) });
        }

        info.params.push_back ({ "Output", 0.5f });

        for (int b = 0; b < peq::bands; ++b)
            info.params.push_back ({ slopes[b], 0.0f });

        return info;
    }();

    switch (kind)
    {
        case EffectKind::eq: return eq;
        case EffectKind::compressor: return comp;
        case EffectKind::delay: return delay;
        case EffectKind::reverb: return reverb;
        case EffectKind::plugin: return plugin;
        case EffectKind::limiter: return limiter;
        case EffectKind::filter: return filter;
        case EffectKind::distortion: return distortion;
        case EffectKind::chorus: return chorus;
        case EffectKind::parametricEq: return parametric;
        case EffectKind::utility: return utility;
        case EffectKind::volumeGate: return volumeGate;
        case EffectKind::peakController: return peakController;
        case EffectKind::pitchShift: return pitchShift;
        case EffectKind::freqShift: return freqShift;
        case EffectKind::parallel: return parallelInfo;
        case EffectKind::patcher: return patcherInfo;
        case EffectKind::lofi: return lofiInfo;
        case EffectKind::effector: return effectorInfo;
        case EffectKind::crossover: return crossoverInfo;
        case EffectKind::rhythmGain: return rhythmInfo;
        case EffectKind::autoPan: return autoPanInfo;
        case EffectKind::vocoder: return vocoderInfo;
        case EffectKind::pitchCorrect: return pitchCorrectInfo;
        case EffectKind::squeeze: return squeezeInfo;
        case EffectKind::vintagePhaser: return vintagePhaserInfo;
        case EffectKind::vocodex: return vocodexInfo;
        case EffectKind::maximus: return maximusInfo;
        case EffectKind::fruityLimiter: return fruityLimiterInfo;
        case EffectKind::fruityCompressor: return fruityCompressorInfo;
        case EffectKind::fruityWaveShaper: return waveShaperInfo;
        case EffectKind::fruitySoftClipper: return softClipperInfo;
        case EffectKind::fruityPanOMatic: return panOMaticInfo;
        case EffectKind::fruityBalance: return balanceInfo;
        case EffectKind::fruityBloodOverdrive: return bloodOverdriveInfo;
        case EffectKind::reeverb2: return reeverb2Info;
        case EffectKind::reeverb1: return reeverb1Info;
        case EffectKind::fruityDelay2: return fruityDelay2Info;
        case EffectKind::fruityDelay3: return fruityDelay3Info;
        case EffectKind::stereoShaper: return stereoShaperInfo;
        case EffectKind::stereoEnhancer: return stereoEnhancerInfo;
        case EffectKind::transientProcessor: return transientProcessorInfo;
        case EffectKind::spreader: return spreaderInfo;
        case EffectKind::fruityChorus: return fruitychorusfx::info();
        case EffectKind::hyperChorus: return hyperchorusfx::info();
        case EffectKind::vintageChorus: return vintagechorusfx::info();
        case EffectKind::grossBeat: return grossBeatInfo;
        case EffectKind::lovePhilter: return philter::info();
        case EffectKind::luxeVerb: return luxeVerbInfo;
        case EffectKind::convolver: return convolverInfo;
        case EffectKind::fruityDelayBank: return fruityDelayBankInfo;
        case EffectKind::fruityMultiband: return fruityMultibandInfo;
        case EffectKind::fruityFastDist: return fastDistInfo;
        case EffectKind::fruityFlangus: return flangusInfo;
        case EffectKind::fruityFlanger: return flangerInfo;
        case EffectKind::equo: return equoInfo;
        case EffectKind::flPitchShifter: return flPitchShifterInfo;
        case EffectKind::multibandDelay: return multibandDelayInfo;
        case EffectKind::frequencySplitter: return fsplit::info();
        case EffectKind::fruityFastLP: return fastLPInfo;
        case EffectKind::fruityFreeFilter: return freeFilterInfo;
        case EffectKind::fruityFilter: return fruityFilterInfo;
        case EffectKind::hardcore: return hardcore::info();
        case EffectKind::distructor: return distructorInfo;
        case EffectKind::newtone: return newtoneInfo;
        case EffectKind::newtime: return newtimeInfo;
        case EffectKind::facetEq: return facet::info();
        case EffectKind::apexLimiter: return apex::info();
        case EffectKind::ridgeStrip: return mixsuite::ridgeInfo();
        case EffectKind::glowLeveler: return mixsuite::glowInfo();
        case EffectKind::hushDeEsser: return mixsuite::hushInfo();
        case EffectKind::aether: return mixsuite::aetherInfo();
        case EffectKind::ember: return mixsuite::emberInfo();
        case EffectKind::lynxTune: return lynxtune::info();
        case EffectKind::fruityPhaser: return fruityPhaserInfo;
        case EffectKind::epochVerb:
        case EffectKind::atrium:
        case EffectKind::foilPlate:
        case EffectKind::nebula:
        case EffectKind::halo:
        case EffectKind::horizon: return verbs::info (kind);
        case EffectKind::echoform: return echosuite::echoformInfo();
        case EffectKind::spiralEcho: return echosuite::spiralInfo();
        case EffectKind::orbit: return echosuite::orbitInfo();
        case EffectKind::latticeMod: return echosuite::latticeInfo();
        case EffectKind::fruitySend: return fruitySendInfo;
        case EffectKind::crestCompressor: return dyn::crestInfo();
        case EffectKind::prismMultiband: return dyn::prismInfo();
        case EffectKind::sentryGate: return dyn::sentryInfo();
        case EffectKind::quellDenoiser: return voicetools::quellInfo();
        case EffectKind::geminiDoubler: return voicetools::geminiInfo();
        case EffectKind::formulaController: return formulaControllerInfo;
        case EffectKind::voiceShift: return voiceshift::info();
        case EffectKind::emphasizer:
        {
            static const EffectInfo info { "Emphasizer", { { "Input gain", 0.0f }, { "Emphasis", 0.5013f }, { "Soft clipping", 0.0f },
                                                           { "Level matching", 0.0f }, { "Bypass", 0.0f }, { "Output", 1.0f } } };
            return info;
        }
        case EffectKind::flFrequencyShifter:
        {
            // FL's controls and defaults (a new instance: Mix 100 %, Frequency shifter, +200 Hz in the 200 Hz range)
            static const EffectInfo info { "Frequency Shifter", { { "Mix", 1.0f }, { "Type", 0.5f }, { "Frequency", 1.0f },
                                                                  { "L/R phase offset", 0.0f }, { "L shape", 1.0f }, { "R shape", 1.0f },
                                                                  { "Feedback", 0.5f }, { "Mono/stereo", 1.0f }, { "Range", 0.5f },
                                                                  { "Left phase offset", 0.5f } } };
            return info;
        }
        case EffectKind::flLowLifter:
        {
            // FL's controls; defaults = the plug-in's Default preset (as a new instance in FL)
            static const EffectInfo info { "Low Lifter", { { "On", 1.0f }, { "Dry/Wet", 1.0f }, { "Low band gain", 0.4999f },
                                                           { "Slope", 0.6993f }, { "Look-ahead", 1.0f }, { "Invert polarity", 0.0f },
                                                           { "Pre gain", 0.4998f }, { "Diffuser mix", 0.0f }, { "Diffuser depth", 0.5f },
                                                           { "High band gain", 1.0f }, { "Crossover", 0.892f }, { "Low cut", 0.0f },
                                                           { "Multiband", 0.0f }, { "High band", 1.0f }, { "Low band", 1.0f } } };
            return info;
        }
        case EffectKind::waveCandy:
        {
            // round 164c (core/VisualFx.h wavecandy::Param): a new one shows the spectrum, like FL's Default preset's analyser
            static const EffectInfo info { "Wave Candy", { { "Mode", visualfx::wavecandy::modeValue (visualfx::wavecandy::Mode::spectrum) },
                                                           { "Gain", 0.5f }, { "Hold", 0.0f }, { "Scope window", 0.5f }, { "Separation", 0.0f },
                                                           { "Sync", 1.0f }, { "Speed", 0.35f }, { "Range", 0.5f }, { "Scale", 1.0f },
                                                           { "Weighting", 1.0f }, { "Resolution", 0.5f }, { "Meter mode", 0.0f },
                                                           { "Peak hold", 0.3f }, { "Decay", 0.5f }, { "Meter scale", 0.5f },
                                                           { "Persistence", 0.5f }, { "Thickness", 0.3f }, { "Colour", 0.97f } } };
            return info;
        }
        case EffectKind::visualizer:
        {
            static const EffectInfo info { "Visualizer", { { "Sensitivity", 0.5f }, { "Beat threshold", 0.4f }, { "Hue shift", 0.0f },
                                                           { "Brightness", 1.0f }, { "Zoom", 0.5f }, { "Speed", 0.5f } } };
            return info;
        }
        case EffectKind::xyController:
        case EffectKind::xyzController:
        {
            // round 164: per axis level / min / max / tension, then speed and acceleration (core/XyController.h)
            static const auto make = [] (EffectKind k) {
                EffectInfo info { k == EffectKind::xyController ? "X-Y Controller" : "X-Y-Z Controller", {} };
                static const char* names[3][4] = { { "X level", "X min", "X max", "X tension" },
                                                   { "Y level", "Y min", "Y max", "Y tension" },
                                                   { "Z level", "Z min", "Z max", "Z tension" } };
                const auto d = xy::defaults (k);
                for (int a = 0; a < xy::axesOf (k); ++a)
                    for (int i = 0; i < 4; ++i)
                        info.params.push_back ({ names[a][i], d[(std::size_t) (4 * a + i)] });
                info.params.push_back ({ "Speed", d[(std::size_t) xy::speed (k)] });
                info.params.push_back ({ "Acceleration", d[(std::size_t) xy::acceleration (k)] });
                return info;
            };
            static const EffectInfo xyInfo = make (EffectKind::xyController), xyzInfo = make (EffectKind::xyzController);
            return kind == EffectKind::xyController ? xyInfo : xyzInfo;
        }
        case EffectKind::controlSurface:
        {
            // round 164: one parameter per control output (the panel's controls name them, core/ControlSurface.h)
            static const EffectInfo info = [] {
                EffectInfo i { "Control Surface", {} };
                static std::vector<std::string> names;
                for (int k = 0; k < surface::maxOutputs; ++k)
                    names.push_back ("Control " + std::to_string (k + 1));
                for (const auto& n : names)
                    i.params.push_back ({ n.c_str(), 0.0f });
                return i;
            }();
            return info;
        }
    }
    return eq;
}

std::string formatEffectParam (EffectKind kind, int param, float v, double bpm, const EffectParams& context)
{
    if (kind == EffectKind::ridgeStrip && mixsuite::ridge::k4Voicing (context)) // round 148: the 4K console's scales
        return mixsuite::ridge::k4ParamText (param, v);
    if (kind == EffectKind::ridgeStrip && mixsuite::ridge::eVoicing (context)) // round 123: the console's own scales
        return mixsuite::ridge::eParamText (param, v);
    if (kind == EffectKind::glowLeveler && mixsuite::glow::classic (context))
        return mixsuite::glow::classicParamText (param, v);
    if (kind == EffectKind::hushDeEsser && mixsuite::hush::classic (context))
        return mixsuite::hush::classicParamText (param, v);
    if (kind == EffectKind::fruityPhaser && param == 0) // the sweep rate follows the frequency range switch
        return phaserfx::sweepText (v, context[3] > 0.5f);
    if (kind == EffectKind::parametricEq && peq::flLaw (context) && param >= 0 && param < peq::slopeParam + peq::bands)
    {
        auto p = context;
        p[(std::size_t) param] = v;
        if (param > peq::outputParam)
            return peq::orderText (p, param - peq::slopeParam);
        if (param < peq::outputParam && param % 4 == 3)
            return peq::widthText (p, param / 4);
    }
    return formatEffectParam (kind, param, v, bpm);
}

std::string formatEffectParam (EffectKind kind, int param, float v, double bpm)
{
    char text[48];

    switch (kind)
    {
        case EffectKind::eq:
            if (param == 2)
                std::snprintf (text, sizeof (text), "%.0f Hz", eqMidHz (v));
            else
                std::snprintf (text, sizeof (text), "%+.1f dB", eqGainDb (v));
            return text;

        case EffectKind::compressor:
            switch (param)
            {
                case 0: std::snprintf (text, sizeof (text), "%.1f dB", std::abs (compThresholdDb (v)) < 0.05f ? 0.0f : compThresholdDb (v)); break;
                case 1: std::snprintf (text, sizeof (text), "%.1f:1", compRatio (v)); break;
                case 2: std::snprintf (text, sizeof (text), "%.1f ms", compAttackMs (v)); break;
                case 3: std::snprintf (text, sizeof (text), "%.0f ms", compReleaseMs (v)); break;
                case 5: return v >= 0.5f ? "on" : "off";
                case 6: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
                case 7: std::snprintf (text, sizeof (text), "%+.1f dB", -18.0f + 36.0f * v); break;
                case 8: return v >= 0.5f ? "vintage program" : "classic";
                case 9: return v >= 0.5f ? "manual" : "ARC";
                case 10: return v >= 0.5f ? "opto" : "electro";
                case 11: return v >= 0.5f ? "smooth" : "warm";
                case 12: return v >= 0.5f ? "on" : "off";
                case 13: return v >= 0.5f ? "RComp" : "LYNX";
                case 14: std::snprintf (text, sizeof (text), "%.2f:1", rcRatio (v)); break;
                case 15: std::snprintf (text, sizeof (text), "%+.1f dB", rcGainDb (v)); break;
                case 16: std::snprintf (text, sizeof (text), rcAttackMs (v) < 10.0f ? "%.2f ms" : "%.1f ms", rcAttackMs (v)); break;
                case 17: std::snprintf (text, sizeof (text), rcReleaseMs (v) < 100.0f ? "%.1f ms" : "%.0f ms", rcReleaseMs (v)); break;
                case 18: return v >= 0.5f ? "measured" : "round 150";
                default: std::snprintf (text, sizeof (text), "+%.1f dB", compMakeupDb (v)); break;
            }
            return text;

        case EffectKind::delay:
            if (param == 0)
            {
                const auto index = delayIndex (v);
                std::snprintf (text, sizeof (text), "%s (%.0f ms)", delayNames[(std::size_t) index],
                               delayDivisions[(std::size_t) index] * 60000.0 / std::max (bpm, 20.0));
                return text;
            }
            if (param == 3)
                return v > 0.5f ? "on" : "off";
            std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::reverb:
            if (param == 5)
                return v <= 0.001f ? std::string ("off") : (std::snprintf (text, sizeof (text), "%.0f Hz", reverbLowCutHz (v)), std::string (text));
            if (param == 6)
                return v >= 0.999f ? std::string ("off") : (std::snprintf (text, sizeof (text), "%.1f kHz", reverbHighCutHz (v) / 1000.0f), std::string (text));
            if (param == 7)
                std::snprintf (text, sizeof (text), "%.0f ms", v * 1000.0f);
            else
                std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::limiter:
            switch (param)
            {
                case 0: std::snprintf (text, sizeof (text), "%.1f dB", limiterCeilingDb (v)); break;
                case 1: std::snprintf (text, sizeof (text), "+%.1f dB", limiterGainDb (v)); break;
                default: std::snprintf (text, sizeof (text), "%.0f ms", limiterReleaseMs (v)); break;
            }
            return text;

        case EffectKind::filter:
            switch (param)
            {
                case 0: return filterNames[(std::size_t) filterType (v)];
                case 1: std::snprintf (text, sizeof (text), "%.0f Hz", filterHz (v)); break;
                case 2: std::snprintf (text, sizeof (text), "Q %.2f", filterQ (v)); break;
                default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
            }
            return text;

        case EffectKind::distortion:
            switch (param)
            {
                case 0: std::snprintf (text, sizeof (text), "+%.1f dB", distortionDriveDb (v)); break;
                case 1: std::snprintf (text, sizeof (text), "%.0f Hz", distortionToneHz (v)); break;
                case 2: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
                default: std::snprintf (text, sizeof (text), "%+.1f dB", (v - 0.5f) * 24.0f); break;
            }
            return text;

        case EffectKind::chorus:
            switch (param)
            {
                case 0: std::snprintf (text, sizeof (text), "%.2f Hz", chorusRateHz (v)); break;
                case 1: std::snprintf (text, sizeof (text), "%.1f ms", chorusDepthMs (v)); break;
                default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
            }
            return text;

        case EffectKind::parametricEq:
        {
            if (param == peq::outputParam)
                std::snprintf (text, sizeof (text), "%+.1f dB", peq::gainDb (v));
            else if (param > peq::outputParam)
                std::snprintf (text, sizeof (text), "%d dB/oct", 12 * peq::slopeStages (v));
            else
                switch (param % 4)
                {
                    case 0: return peq::typeName (peq::type (v));
                    case 1:
                    {
                        const auto f = peq::hz (v);
                        if (f >= 1000.0)
                            std::snprintf (text, sizeof (text), "%.2f kHz", f / 1000.0);
                        else
                            std::snprintf (text, sizeof (text), "%.0f Hz", f);
                        break;
                    }
                    case 2: std::snprintf (text, sizeof (text), "%+.1f dB", peq::gainDb (v)); break;
                    default: std::snprintf (text, sizeof (text), "Q %.2f", peq::q (v)); break;
                }
            return text;
        }

        case EffectKind::utility:
            if (param == 0)
            {
                if (v <= 0.0f)
                    return "mute";
                std::snprintf (text, sizeof (text), "%+.1f dB", utilityGainDb (v));
            }
            else if (param == 1)
            {
                const auto pan = v * 2.0f - 1.0f;
                if (std::abs (pan) < 0.005f)
                    return "center";
                std::snprintf (text, sizeof (text), "%.0f%% %s", std::abs (pan) * 100.0f, pan < 0.0f ? "L" : "R");
            }
            else
                std::snprintf (text, sizeof (text), "%.2f", utilityMatrix (v));
            return text;

        case EffectKind::volumeGate:
            if (param < gate::steps || param == gate::mixParam)
                std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            else
                std::snprintf (text, sizeof (text), "%.1f ms", param == gate::attackParam ? gate::attackMs (v) : gate::releaseMs (v));
            return text;

        case EffectKind::pitchShift:
            switch (param)
            {
                case 0: std::snprintf (text, sizeof (text), "%+.0f st", pitch::semitones (v)); break;
                case 1: std::snprintf (text, sizeof (text), "%+.0f ct", pitch::cents (v)); break;
                case 3: std::snprintf (text, sizeof (text), "%.0f ms", pitch::windowMs (v)); break;
                default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
            }
            return text;

        case EffectKind::freqShift:
            if (param == 0)
                std::snprintf (text, sizeof (text), "%+.1f Hz", fshift::hz (v));
            else
                std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::peakController:
            if (param == 1)
            {
                const auto raw = (v * 2.0 - 1.0) * 256.0;
                std::snprintf (text, sizeof (text), "%+.0f%%", (raw < 0.0 ? -1.0 : 1.0) * (std::pow (6.0, std::abs (raw) / 256.0) - 1.0) * 40.0);
            }
            else if (param == 3)
                std::snprintf (text, sizeof (text), "%.2f / beat", (std::pow (10001.0, (5.0 + 123.0 * v) / 128.0) - 1.0) * 0.0192);
            else if (param == 2)
                std::snprintf (text, sizeof (text), "%+.0f", (v * 2.0f - 1.0f) * 128.0f);
            else
                std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::lofi:
            if (param == 0)
                std::snprintf (text, sizeof (text), "%.0f Hz", 44100.0 / lofi::holdSamples (v));
            else if (param == 1)
                std::snprintf (text, sizeof (text), "%.1f bits", std::log2 (lofi::levels (v)));
            else
                std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::effector:
            return effector::formatParam (param, v, bpm);

        case EffectKind::pitchCorrect:
            switch (param)
            {
                case 0:
                {
                    const auto u = 1.0 - v;
                    if (u < 0.01)
                        return "instant";
                    std::snprintf (text, sizeof (text), "%.1f ms", (3.0 * u * u * u + 0.001) / 8.82 * 1000.0);
                    break;
                }
                case 1: std::snprintf (text, sizeof (text), "%+.0f ct", (2.0f * v - 1.0f) * 100.0f); break;
                case 3: std::snprintf (text, sizeof (text), "%.1f Hz", pcorr::noteA (v) * 8.0); break;
                case 4: std::snprintf (text, sizeof (text), "%.0f Hz min.", pcorr::minHz[pcorr::range (v)]); break;
                case 30: std::snprintf (text, sizeof (text), "x%.2f", std::pow (2.0, 2.0 * v - 1.0)); break;
                case 31: return (int) (v + v) == 0 ? "left" : ((int) (v + v) == 1 ? "independent" : "right");
                case 32: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
                case 36: return v > 0.5f ? "mix" : "replace";
                case 38: case 39: case 40: case 41:
                    std::snprintf (text, sizeof (text), "%+.0f%%", (2.0f * v - 1.0f) * 100.0f);
                    break;
                default:
                    if (param >= 29)
                        return v > 0.5f ? "on" : "off";
                    return v > 0.5f ? (param >= 17 ? "bypassed" : "yes") : (param >= 17 ? "-" : "no");
            }
            return text;

        case EffectKind::squeeze:
            std::snprintf (text, sizeof (text), "%d", (int) std::lround (v * squeezeMax[(std::size_t) std::clamp (param, 0, 11)]));
            return text;

        case EffectKind::reeverb2: return reeverb2ParamText (param, v);
        case EffectKind::luxeVerb: return luxeVerbParamText (param, v);
        case EffectKind::convolver: return convolverParamText (param, v);
        case EffectKind::reeverb1: return reeverb1ParamText (param, v);
        case EffectKind::fruityChorus: return fruitychorusfx::paramText (param, v);
        case EffectKind::hyperChorus: return hyperchorusfx::paramText (param, v);
        case EffectKind::vintageChorus: return vintagechorusfx::paramText (param, v);

        case EffectKind::vintagePhaser:
            switch (param)
            {
                case 0: case 1: case 4: std::snprintf (text, sizeof (text), "%.0f Hz", 1.25 * std::pow (2.0, 14.0 * v)); break;
                case 3: case 5: case 7: case 12: case 13: case 17: case 23: case 24: case 33: case 35: return v >= 0.5f ? "on" : "off";
                case 8: return v >= 0.5f ? "sine" : "triangle";
                case 25: std::snprintf (text, sizeof (text), "%.1f dB", 30.0f * v); break;
                case 26: std::snprintf (text, sizeof (text), "%.0f Hz", 50.0f + 600.0f * v); break;
                case 27: case 28: std::snprintf (text, sizeof (text), "%.0f Hz", 195.0f * v * v * v + 5.0f); break;
                case 34: std::snprintf (text, sizeof (text), "%+.1f dB", 30.0f * v - 15.0f); break;
                default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
            }
            return text;

        case EffectKind::vocoder:
            switch (param)
            {
                case 0: std::snprintf (text, sizeof (text), "%d", vocoder::bands (v)); break;
                case 1: std::snprintf (text, sizeof (text), "%d", vocoder::order (v)); break;
                case 2: return std::array<const char*, 3> { "modulator", "carrier", "mix" }[(std::size_t) std::clamp ((int) std::lround (v * 2.0f), 0, 2)];
                case 3: case 7: case 12: case 16: return v > 0.5f ? "on" : "off";
                case 4: case 5: std::snprintf (text, sizeof (text), "%.0f Hz", (double) fvocoder::edgeHz (vocoder::raw (v, param - 4))); break;
                case 8: std::snprintf (text, sizeof (text), "%+.0f ct", v * 2400.0f - 1200.0f); break;
                case 9: std::snprintf (text, sizeof (text), "%.0f%%", 5.0f + 195.0f * v); break;
                case 10: case 11: std::snprintf (text, sizeof (text), "%d ms", vocoder::raw (v, param - 4)); break;
                case 13: case 14: case 15: std::snprintf (text, sizeof (text), "%.2f", (double) fvocoder::mixLevel (vocoder::raw (v, param - 4))); break;
                default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
            }
            return text;

        case EffectKind::autoPan:
            if (param >= autopan::source && param < autopan::count)
                return autopan::paramText (param, v);
            [[fallthrough]];
        case EffectKind::rhythmGain:
            if (param == 0)
                std::snprintf (text, sizeof (text), "%.3g beats", v * 64.0f);
            else if (kind == EffectKind::rhythmGain && param == 1)
                std::snprintf (text, sizeof (text), "%d", (int) std::lround (v * rhythm::maxPoints));
            else if (kind == EffectKind::autoPan && (param == 1 || param == 3))
                std::snprintf (text, sizeof (text), "%+.0f%%", (2.0f * v - 1.0f) * 100.0f);
            else if (kind == EffectKind::autoPan && param >= autopan::count) // round 136: the Rhythm voicing (after round 135's panner)
            {
                switch (param - autopan::count + 7)
                {
                    case 7: return v > 0.5f ? "Rhythm" : "Classic";
                    case 8: std::snprintf (text, sizeof (text), "%.2f Hz", rhythmpan::rateHz (v)); break;
                    case 9: return v > 0.5f ? "beats" : "Hz";
                    case 10: std::snprintf (text, sizeof (text), "%.3g beats", rhythmpan::beats (v)); break;
                    case 11: std::snprintf (text, sizeof (text), "%.1f deg", 210.0f * v); break;
                    case 12: std::snprintf (text, sizeof (text), "%+.1f deg", -105.0f + 210.0f * v); break;
                    case 14: return v > 0.5f ? "8 steps" : "left / right";
                    case 17: return v < 0.25f ? "digital" : (v < 0.75f ? "analog" : "analog (hot)");
                    case 18: std::snprintf (text, sizeof (text), "%+.1f dB", -24.0f + 48.0f * v); break;
                    case 19: std::snprintf (text, sizeof (text), "%+.2f oct", -3.0f + 6.0f * v); break;
                    case 20: case 21: std::snprintf (text, sizeof (text), "%+.0f deg", -180.0f + 360.0f * v); break;
                    case 22: std::snprintf (text, sizeof (text), "%.1f dB", -60.0f + 60.0f * v); break;
                    case 23: std::snprintf (text, sizeof (text), "%.1f dB", 1.0f + 39.0f * v); break;
                    case 24: case 25: std::snprintf (text, sizeof (text), "%+.3f", -0.25f + 0.5f * v); break;
                    case 26: return v >= 0.5f ? "fold" : "clip";
                    case 27: return v < 0.25f ? "off (clock)" : (v < 0.75f ? "ping-pong" : "random step");
                    case 28: if (v >= 0.999f) return "off (manual only)";
                             std::snprintf (text, sizeof (text), "%.1f dB", -60.0f + 60.0f * v); break;
                    case 29: std::snprintf (text, sizeof (text), "%d", 1 + (int) std::lround (11.0f * v)); break;
                    case 30: return v < 0.25f ? "off" : (v < 0.75f ? "high pass" : "low pass");
                    case 31: std::snprintf (text, sizeof (text), "%.0f Hz", 20.0 * std::pow (1000.0, (double) v)); break;
                    case 32: return "Trigger";
                    case 33: return v >= 0.5f ? "on" : "off";
                    default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); break;
                }
            }
            else
                std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::fruityFastDist:
            return fastdistfx::paramText (param, v);

        case EffectKind::fruityFlangus:
            return flangusfx::paramText (param, v);

        case EffectKind::fruityFlanger:
            return flangerfx::paramText (param, v);

        case EffectKind::crossover:
            if (param == 0 || param == 3)
                return v > 0.5f ? "on" : "off";
            if (param == 1 || param == 4)
                std::snprintf (text, sizeof (text), "%.0f Hz", xover::hz (v));
            else
                std::snprintf (text, sizeof (text), "%d dB/oct", xover::dbPerOctave[xover::combo (v)]);
            return text;

        case EffectKind::fruityLimiter:
            return fldyn::limiterParamText (param, v);

        case EffectKind::fruityCompressor:
            return fldyn::compressorParamText (param, v);
        case EffectKind::fruityWaveShaper:
            return flshape::waveShaperParamText (param, v);

        case EffectKind::fruitySoftClipper:
            return flshape::softClipperParamText (param, v);
        case EffectKind::stereoShaper:
            return stereofx::stereoShaperParamText (param, v);

        case EffectKind::stereoEnhancer:
            return stereofx::stereoEnhancerParamText (param, v);

        case EffectKind::transientProcessor:
            return stereofx::transientProcessorParamText (param, v);

        case EffectKind::spreader:
            return stereofx::spreaderParamText (param, v);
        case EffectKind::fruityDelayBank:
            return fldbank::globalText (param, v);

        case EffectKind::fruityMultiband:
            return flmb::paramText (param, v);

        case EffectKind::frequencySplitter:
            return fsplit::paramText (param, v);

        case EffectKind::hardcore:
            return hardcore::paramText (param, v);

        case EffectKind::fruityPanOMatic:
            return flbasic::panOMaticParamText (param, v);

        case EffectKind::fruityBalance:
            return flbasic::balanceParamText (param, v);

        case EffectKind::fruitySend:
            return flbasic::sendParamText (param, v);

        case EffectKind::fruityBloodOverdrive:
            return flbasic::bloodOverdriveParamText (param, v);
        case EffectKind::equo:
            return equofx::paramText (param, v);

        case EffectKind::distructor:
            return distructor::paramText (param, v);

        case EffectKind::flPitchShifter:
            return psfl::paramText (param, v);

        case EffectKind::fruityFastLP:
            return flfilter::fastLPParamText (param, v);

        case EffectKind::fruityFreeFilter:
            return flfilter::freeFilterParamText (param, v);

        case EffectKind::fruityFilter:
            return flfilter::filterParamText (param, v);
        case EffectKind::multibandDelay:
            return mbdelay::paramText (param, v);

        case EffectKind::newtone:
        case EffectKind::newtime:
            return newtonefx::paramText (param, v);

        case EffectKind::facetEq:
            return facet::paramText (param, v);

        case EffectKind::apexLimiter:
            return apex::paramText (param, v);
        case EffectKind::ridgeStrip:
        case EffectKind::glowLeveler:
        case EffectKind::hushDeEsser:
        case EffectKind::aether:
        case EffectKind::ember:
            return mixsuite::paramText (kind, param, v);
        case EffectKind::lynxTune:
            return lynxtune::paramText (param, v);
        case EffectKind::fruityPhaser:
            return phaserfx::paramText (param, v);
        case EffectKind::epochVerb:
        case EffectKind::atrium:
        case EffectKind::foilPlate:
        case EffectKind::nebula:
        case EffectKind::halo:
        case EffectKind::horizon:
            return verbs::paramText (kind, param, v, bpm);
        case EffectKind::echoform:
        case EffectKind::spiralEcho:
        case EffectKind::orbit:
        case EffectKind::latticeMod:
            return echosuite::paramText (kind, param, v);
        case EffectKind::crestCompressor:
        case EffectKind::prismMultiband:
        case EffectKind::sentryGate:
            return dyn::paramText (kind, param, v);
        case EffectKind::quellDenoiser:
        case EffectKind::geminiDoubler:
            return voicetools::paramText (kind, param, v);
        case EffectKind::voiceShift:
            return voiceshift::paramText (param, v);
        case EffectKind::emphasizer:
            return emphasizer::paramText (param, v);
        case EffectKind::flFrequencyShifter:
            return fsfl::paramText (param, v);
        case EffectKind::flLowLifter:
            return lowlifter::paramText (param, v);
        case EffectKind::waveCandy:
            return visualfx::wavecandy::paramText (param, v);
        case EffectKind::visualizer:
            return visualfx::visualizer::paramText (param, v);
        case EffectKind::xyController:
        case EffectKind::xyzController:
            return xy::paramText (kind, param, v);
        case EffectKind::controlSurface:
            std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
            return text;

        case EffectKind::fruityDelay2:
            return fldelay::delay2ParamText (param, v);

        case EffectKind::fruityDelay3:
            return fldelay::delay3ParamText (param, v, bpm);

        case EffectKind::lovePhilter:
            return philter::paramText (param, v);

        case EffectKind::grossBeat:
        {
            const auto raw = grossbeat::rawFromParam (param, v);
            if (param < 2)
                std::snprintf (text, sizeof (text), "slot %d", raw + 1);
            else if (param == 2 || param == 3)
                std::snprintf (text, sizeof (text), "%.0f%%", raw / 2.56);
            else if (param == 4 || param == 5) // 0…500 ms attack, 0…1000 ms release (REA_rodada14 §2.2)
                std::snprintf (text, sizeof (text), "%.1f ms", (std::pow (501.0, raw / 10000.0) - 1.0) * (param == 5 ? 2.0 : 1.0));
            else if (param == 6)
                std::snprintf (text, sizeof (text), "%.0f%%", raw / 2.56);
            else
                return raw != 0 ? "on" : "off";
            return text;
        }

        case EffectKind::formulaController: // the knobs a, b, c as the formula sees them
            std::snprintf (text, sizeof (text), "%.3f", v);
            return text;

        case EffectKind::plugin:
        case EffectKind::parallel:
        case EffectKind::patcher:
        case EffectKind::vocodex:
        case EffectKind::maximus:
            break;
    }

    return {};
}

EffectSlot makeEffectSlot (EffectKind kind)
{
    EffectSlot slot;
    slot.kind = kind;
    const auto& info = effectInfo (kind);

    for (std::size_t i = 0; i < info.params.size() && i < slot.params.size(); ++i)
        slot.params[i] = info.params[i].defaultValue;
    if (kind == EffectKind::patcher) // a new Patcher passes its input through, as FL's
        slot.patcher.wires.push_back ({});
    if (kind == EffectKind::visualizer) // round 164c: LYNX's default scene
        slot.state = visualfx::scene::toState (visualfx::scene::defaultScene());

    return slot;
}

void upgradeLoadedEffectParams (EffectSlot& slot, std::size_t savedCount) noexcept
{
    // round 147: Prism Multiband saved before its measured Dynamic phase engine keeps the round-108 construction
    if (slot.kind == EffectKind::prismMultiband && savedCount <= (std::size_t) dyn::prism::dynamicEngine)
        slot.params[(std::size_t) dyn::prism::dynamicEngine] = 0.0f;
    // round 150: … and before its measured detector keeps the round-108 detector
    if (slot.kind == EffectKind::prismMultiband && savedCount <= (std::size_t) dyn::prism::detectorLaw)
        slot.params[(std::size_t) dyn::prism::detectorLaw] = 0.0f;
    // round 152: … and before the gain-stage attack keeps round 150's level-follower attack
    if (slot.kind == EffectKind::prismMultiband && savedCount <= (std::size_t) dyn::prism::rev152)
        slot.params[(std::size_t) dyn::prism::rev152] = 0.0f;
    // round 150: Auto-pan saved before the Tweak block keeps the Rhythm voicing's clipped angle
    if (slot.kind == EffectKind::autoPan && savedCount <= (std::size_t) autopan::count + 19)
        slot.params[(std::size_t) autopan::count + 19] = 0.0f;
    // round 150: Echoform saved before the voicings' measured stereo behaviour keeps the round-147 voicings
    // round 152: Ember saved before the effect characters' measured law keeps the static stand-ins
    if (slot.kind == EffectKind::ember && savedCount <= (std::size_t) mixsuite::ember::effectLaw)
        slot.params[(std::size_t) mixsuite::ember::effectLaw] = 0.0f;
    if (slot.kind == EffectKind::echoform && savedCount <= (std::size_t) echosuite::echoform::voicingStereo
        && slot.params.size() > (std::size_t) echosuite::echoform::voicingStereo)
        slot.params[(std::size_t) echosuite::echoform::voicingStereo] = 0.0f;
    // round 152: a Glow Leveler saved before the measured opto law keeps the round-123 Limit and no classic auto make-up
    if (slot.kind == EffectKind::glowLeveler && savedCount <= (std::size_t) mixsuite::glow::optoLaw)
        slot.params[(std::size_t) mixsuite::glow::optoLaw] = 0.0f;
    // round 152: a Compressor saved before the measured vintage curve keeps the round-150 curve
    if (slot.kind == EffectKind::compressor && savedCount <= 18 && slot.params.size() > 18)
        slot.params[18] = 0.0f;
}

std::unique_ptr<Effect> makeParallelEffect (const EffectSlot& slot)
{
    return std::make_unique<ParallelEffect> (slot);
}

std::unique_ptr<Effect> makeEffect (const EffectSlot& slot)
{
    if (slot.kind == EffectKind::parallel)
        return makeParallelEffect (slot);
    if (slot.kind == EffectKind::patcher) // built-in nodes only; the rack builds it with its plugins (core/Patcher.h)
    {
        return patcher::makeEffect (slot, 44100.0, patcher::builtInFactories (44100.0));
    }
    if (slot.kind == EffectKind::formulaController)
        return formula::makeEffect (slot.state);
    if (slot.kind == EffectKind::vocodex)
        return vocodex::make (slot.state);
    if (slot.kind == EffectKind::maximus)
        return maximusfx::make (slot.state);
    if (slot.kind == EffectKind::multibandDelay)
        return mbdelay::make (slot.state);
    if (slot.kind == EffectKind::vocoder)
        return std::make_unique<Vocoder> (slot.state);
    if (slot.kind == EffectKind::equo)
        return equofx::make (slot.state);
    if (slot.kind == EffectKind::hardcore)
        return hardcore::make (slot);
    if (slot.kind == EffectKind::distructor)
        return distructor::make (slot.state);
    if (slot.kind == EffectKind::fruityWaveShaper)
        return flshape::makeWaveShaper (slot.state);
    if (slot.kind == EffectKind::parametricEq && peq::flLaw (slot.params))
        return std::make_unique<ParametricEq> (slot.params);
    if (slot.kind == EffectKind::frequencySplitter)
        return fsplit::make (slot.params);
    if (slot.kind == EffectKind::flLowLifter) // its look-ahead (latency) known before the first block (round 154)
        return lowlifter::make (slot.params[4] >= 0.5f);
    if (slot.kind == EffectKind::hushDeEsser) // its look-ahead (latency) known before the first block (round 128)
        return mixsuite::makeHush (&slot.params);
    if (slot.kind == EffectKind::facetEq) // its phase mode (latency) known before the first block
        return facet::make (&slot.params);
    if (slot.kind == EffectKind::ember) // round 152: its processing mode (linear phase: latency) known before the first block
        return mixsuite::makeEmber (&slot.params);
    if (dyn::isDynKind (slot.kind)) // their look-ahead / oversampling latency known before the first block
        return dyn::make (slot.kind, &slot.params);
    if (slot.kind == EffectKind::apexLimiter) // round 135: the oversampling latency known before the first block
        return apex::make (&slot.params);
    if (slot.kind == EffectKind::hyperChorus)
        return hyperchorusfx::make (hyperchorusfx::instanceOf (slot.state));
    if (slot.kind == EffectKind::vintageChorus)
        return vintagechorusfx::make (hyperchorusfx::instanceOf (slot.state));
    if (slot.kind == EffectKind::grossBeat)
        return grossbeat::make (slot.state);
    if (slot.kind == EffectKind::lovePhilter)
        return philter::make (slot);
    if (slot.kind == EffectKind::convolver)
        return makeConvolver (slot.state);
    if (slot.kind == EffectKind::fruityDelayBank)
        return fldbank::make (slot.state);
    if (slot.kind == EffectKind::fruityMultiband)
        return slot.state.empty() ? flmb::make() : flmb::make (slot.state);
    return makeEffect (slot.kind);
}

std::unique_ptr<Effect> makeEffect (EffectKind kind)
{
    switch (kind)
    {
        case EffectKind::eq: return std::make_unique<Eq>();
        case EffectKind::compressor: return std::make_unique<Compressor>();
        case EffectKind::delay: return std::make_unique<Delay>();
        case EffectKind::reverb: return std::make_unique<Reverb>();
        case EffectKind::limiter: return std::make_unique<Limiter>();
        case EffectKind::filter: return std::make_unique<Filter>();
        case EffectKind::distortion: return std::make_unique<Distortion>();
        case EffectKind::chorus: return std::make_unique<Chorus>();
        case EffectKind::parametricEq: return std::make_unique<ParametricEq>();
        case EffectKind::utility: return std::make_unique<Utility>();
        case EffectKind::volumeGate: return std::make_unique<VolumeGate>();
        case EffectKind::peakController: return std::make_unique<PeakController>();
        case EffectKind::pitchShift: return std::make_unique<PitchShift>();
        case EffectKind::freqShift: return std::make_unique<FreqShift>();
        case EffectKind::parallel: return std::make_unique<ParallelEffect> (EffectSlot {}); // empty: use makeParallelEffect
        case EffectKind::patcher: return makeEffect (makeEffectSlot (EffectKind::patcher)); // empty graph: silence
        case EffectKind::lofi: return std::make_unique<LoFi>();
        case EffectKind::effector: return effector::make();
        case EffectKind::crossover: return std::make_unique<Crossover>();
        case EffectKind::rhythmGain: return std::make_unique<RhythmGain>();
        case EffectKind::autoPan: return std::make_unique<AutoPan>();
        case EffectKind::vocoder: return std::make_unique<Vocoder> (std::vector<std::uint8_t> {}); // use makeEffect (slot) for the band levels
        case EffectKind::squeeze: return std::make_unique<Squeeze>();
        case EffectKind::vintagePhaser: return makeVintagePhaser();
        case EffectKind::vocodex: return vocodex::make ({}); // without a state: passes the signal; use makeEffect (slot)
        case EffectKind::maximus: return maximusfx::make ({}); // without a state: passes the signal; use makeEffect (slot)
        case EffectKind::fruityLimiter: return fldyn::makeLimiter();
        case EffectKind::fruityCompressor: return fldyn::makeCompressor();
        case EffectKind::fruityWaveShaper: return flshape::makeWaveShaper ({}); // FL's default curve; use makeEffect (slot)
        case EffectKind::fruitySoftClipper: return flshape::makeSoftClipper();
        case EffectKind::fruityPanOMatic: return flbasic::makePanOMatic();
        case EffectKind::fruityBalance: return flbasic::makeBalance();
        case EffectKind::fruitySend: return flbasic::makeSend(); // FL's Fruity Send (REA_rodada105_send)
        case EffectKind::fruityBloodOverdrive: return flbasic::makeBloodOverdrive();
        case EffectKind::fruityFastLP: return flfilter::makeFastLP();
        case EffectKind::fruityFreeFilter: return flfilter::makeFreeFilter();
        case EffectKind::fruityFilter: return flfilter::makeFilter();
        case EffectKind::reeverb2: return makeReeverb2();
        case EffectKind::luxeVerb: return makeLuxeVerb();
        case EffectKind::convolver: return makeConvolver(); // no impulse response: dry only; use makeEffect (slot)
        case EffectKind::reeverb1: return makeReeverb1();
        case EffectKind::fruityDelay2: return fldelay::makeDelay2();
        case EffectKind::fruityDelay3: return fldelay::makeDelay3();
        case EffectKind::stereoShaper: return stereofx::makeStereoShaper();
        case EffectKind::stereoEnhancer: return stereofx::makeStereoEnhancer();
        case EffectKind::transientProcessor: return stereofx::makeTransientProcessor();
        case EffectKind::spreader: return stereofx::makeSpreader();
        case EffectKind::fruityChorus: return fruitychorusfx::make();
        case EffectKind::hyperChorus: return hyperchorusfx::make();
        case EffectKind::vintageChorus: return vintagechorusfx::make();
        case EffectKind::grossBeat: return grossbeat::make ({}); // without a state: passes the signal; use makeEffect (slot)
        case EffectKind::lovePhilter: return philter::make (EffectSlot {}); // without a state: passes the signal
        case EffectKind::fruityDelayBank: return fldbank::make ({}); // a new FL instance; use makeEffect (slot) for a state
        case EffectKind::fruityMultiband: return flmb::make();
        case EffectKind::equo: return equofx::make ({}); // FL's default EQUO; use makeEffect (slot)
        case EffectKind::flPitchShifter: return psfl::make();
        case EffectKind::frequencySplitter: return fsplit::make();
        case EffectKind::hardcore: return hardcore::make (makeEffectSlot (EffectKind::hardcore)); // no cabinet impulses; use makeEffect (slot)
        case EffectKind::distructor: return distructor::make ({}); // without a state: passes the signal; use makeEffect (slot)
        case EffectKind::multibandDelay: return mbdelay::make ({}); // FL's default Multiband Delay; use makeEffect (slot)
        case EffectKind::pitchCorrect: return std::make_unique<PitchCorrect>();
        case EffectKind::fruityFastDist: return fastdistfx::make();
        case EffectKind::fruityFlangus: return flangusfx::make();
        case EffectKind::fruityFlanger: return flangerfx::make();
        case EffectKind::newtone:
        case EffectKind::newtime: return newtonefx::make (nullptr); // without its sample: a pass-through
        case EffectKind::facetEq: return facet::make();
        case EffectKind::apexLimiter: return apex::make();
        case EffectKind::ridgeStrip:
        case EffectKind::glowLeveler:
        case EffectKind::hushDeEsser:
        case EffectKind::aether:
        case EffectKind::ember: return mixsuite::make (kind); // LYNX's own mixing suite (REA_rodada96)
        case EffectKind::lynxTune: return lynxtune::make(); // LYNX's own pitch corrector (REA_rodada95)
        case EffectKind::fruityPhaser: return phaserfx::make(); // FL's Fruity Phaser, exact (REA_rodada105_phaser)
        case EffectKind::epochVerb:
        case EffectKind::atrium:
        case EffectKind::foilPlate:
        case EffectKind::nebula:
        case EffectKind::halo:
        case EffectKind::horizon: return verbs::make (kind); // LYNX's own reverbs (REA_rodada100)
        case EffectKind::echoform:
        case EffectKind::spiralEcho:
        case EffectKind::orbit:
        case EffectKind::latticeMod: return echosuite::make (kind); // LYNX's delay / modulation suite (REA_rodada101)
        case EffectKind::crestCompressor:
        case EffectKind::prismMultiband:
        case EffectKind::sentryGate: return dyn::make (kind); // LYNX's dynamics suite (REA_rodada108)
        case EffectKind::quellDenoiser:
        case EffectKind::geminiDoubler: return voicetools::make (kind); // LYNX's voice tools (REA_rodada119)
        case EffectKind::formulaController: return formula::makeEffect ({});
        case EffectKind::voiceShift: return voiceshift::make(); // round 135
        case EffectKind::emphasizer: return emphasizer::make(); // round 154
        case EffectKind::flFrequencyShifter: return fsfl::make(); // round 154
        case EffectKind::flLowLifter: return lowlifter::make(); // round 154
        case EffectKind::waveCandy:
        case EffectKind::visualizer: return visualfx::make(); // round 164c: passes the sound, feeds the editor's display
        case EffectKind::xyController:
        case EffectKind::xyzController: return xy::makeEffect (kind); // round 164
        case EffectKind::controlSurface: return surface::makeEffect(); // round 164
        case EffectKind::plugin: break; // created by the app (RackState::Factories)
    }
    return std::make_unique<Eq>();
}
} // namespace lynx
