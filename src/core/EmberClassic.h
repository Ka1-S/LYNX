#pragma once

// Ember Saturator's "Classic" voicing (round 136, vox136): a five-style saturator whose structure and constants were
// measured black-box on the official Soundtoys Decapitator trial (hosted through the VST3 API, fed signals, outputs
// measured; nothing decompiled or copied) so that the projects that used it sound the same through LYNX
// (docs/analise/REA_rodada136.md). LYNX's own code: a pre-emphasised, oversampled asymmetric soft clipper with a
// signal-dependent bias, a de-emphasis and a DC blocker, between the original's measured control laws.
//
//   in ─► Low cut (2nd-order high-pass; Thump = a resonant one) ─► Tone (2nd-order tilt) ─► pre-emphasis ─► drive gain
//      (+ Punish) ─► ×4 oversampled curve (its bias follows the level) ─► de-emphasis ─► DC blocker ─► style FIR
//      ─► High cut (31-tap linear-phase FIR, gentle / steep) ─► mix with the aligned dry input ─► output

#include <array>
#include <vector>

#include "core/Biquad.h"
#include "core/MixSuite.h"

namespace lynx::emberclassic
{
constexpr int styleFirTaps = 16;

// one style's fitted constants (the tables live in EmberClassic.cpp)
struct StyleModel
{
    double gain0Db = -2.0, gainPerUnitDb = 3.3; // drive 0 … 10 → curve input gain in dB
    double punishDb = 9.7;
    double outDb = 0.0;                         // output scale after the curve
    // pre-emphasis (undone after the curve): a low shelf and a high shelf (first order)
    double preLowHz = 200.0, preLowDb = 0.0, preHighHz = 3000.0, preHighDb = 0.0;
    // curve: s(v) = v / (1 + |v/c|^p)^(1/p) with its own c, p on each side; y = s(v + b) − s(b)
    double cPos = 1.0, pPos = 2.0, cNeg = 1.0, pNeg = 2.0;
    double bias = 0.0;                          // static bias (even harmonics)
    double biasDyn = 0.0;                       // bias added per unit of the low-passed curve output
    double envAttackMs = 5.0, envReleaseMs = 50.0;
    double dcHz = 13.0;                         // DC blocker after the curve
    std::array<double, styleFirTaps> fir { 1.0 }; // the style's own linear colour (base rate, causal)
    // round 165 [EXEC trial, REA_rodada165ha §1]: Punish's measured law — it adds punishLawDb + punishLawPerUnitDb × drive
    // before the curve (round 136's constant punishDb was fitted on a reading the trial gets wrong right after a toggle)
    double punishLawDb = 14.0, punishLawPerUnitDb = 0.4;
};

constexpr int styleCount = 5;                   // A, E, N, T, P
const StyleModel& styleModel (int style) noexcept;

// control laws of the original (normalised 0 … 1 values as its parameters use them)
double driveUnits (float v) noexcept;           // 0 … 10
double toneDb (float v) noexcept;               // −12 … +12
double lowCutHz (float v) noexcept;             // 20 Hz … 1 kHz (log)
double highCutHz (float v) noexcept;            // 1 … 20 kHz (log)
double outputDb (float v) noexcept;             // −24 … 0 dB
int styleOf (float v) noexcept;

// round 165 [EXEC: her 63 saved states]: the reference's Auto moves the Output knob opposite to Drive in its editor
// (−1.6 dB per Drive unit; the audio path itself is unchanged). The Output value (0 … 1) after Drive moves from
// `driveFrom` to `driveTo` (0 … 1), clamped to the knob's range.
constexpr double autoDbPerUnit = -1.6;
float autoOutput (float output, float driveFrom, float driveTo) noexcept;

struct Settings
{
    int style = 0;
    double drive = 0.0;                         // 0 … 10
    bool punish = false, thump = false, steep = false;
    double lowCutHz = 20.0, toneDb = 0.0, highCutHz = 20000.0;
    double mix = 1.0;                           // 0 … 1, linear
    double outputDb = 0.0;
    bool measuredPunish = true;                 // round 165: Punish's measured law (false = round 136's +9.75 dB)
    bool operator== (const Settings&) const = default;
};

class Engine
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    // `model` overrides the style's table (the fitting lab); null = the table
    void set (const Settings&, const StyleModel* model = nullptr);
    void process (float* left, float* right, int n) noexcept;
    static int latency() noexcept;              // base-rate samples: Ember's own (mixsuite::Oversampler::latency())

private:
    struct OnePole
    {
        double b0 = 1.0, b1 = 0.0, a1 = 0.0, z[2] {};
        float process (float x, int c) noexcept { const auto y = b0 * x + z[c]; z[c] = b1 * x - a1 * y; return (float) y; }
        void reset() noexcept { z[0] = z[1] = 0.0; }
    };
    // ×4 half-band oversampler (two stages, 59 and 19 taps) — shorter than mixsuite::Oversampler so that it and the
    // High cut's 31-tap FIR fit in Ember's fixed latency
    class Os4
    {
    public:
        Os4();
        void reset() noexcept;
        static int latency() noexcept;      // base-rate samples (34)
        template <typename F>
        float run (float x, F&& f) noexcept
        {
            float a, b, c, d;
            up (st[0], x, a, b);
            up (st[1], a, c, d);
            const float y0 = down (st[1], f (c), f (d));
            up (st[1], b, c, d);
            const float y1 = down (st[1], f (c), f (d));
            return down (st[0], y0, y1);
        }

    private:
        struct Stage
        {
            int taps = 0, centreDelay = 0, upPos = 0, downPos = 0;
            std::array<float, 32> coef {};
            std::array<float, 64> upHist {}, evenHist {}, oddHist {};
        };
        static void up (Stage&, float x, float& a, float& b) noexcept;
        static float down (Stage&, float a, float b) noexcept;
        std::array<Stage, 2> st;
        std::array<float, 8> pad {};
        int padPos = 0;
    };
    struct Channel
    {
        double dcX = 0.0, dcY = 0.0, env = 0.0;
        Os4 os;
        std::vector<float> dry;                 // delay line of the dry input (latency samples)
        std::vector<float> wetPad;              // the wet path's pad up to the latency
        std::array<float, 64> styleHist {}, cutHist {};
        int dryPos = 0, stylePos = 0, cutPos = 0, padPos = 0;
    };
    static constexpr int maxBlock = 2048;
    void design();
    void block (float* left, float* right, int n) noexcept;
    float curve (float v, double& env) const noexcept;

    double sr = 48000.0;
    Settings s;
    StyleModel m;
    Biquad lowCut, tone;
    OnePole preLow, preHigh, postLow, postHigh;
    std::array<float, 31> cut {};               // High cut taps (linear phase, centre at 15)
    double gain = 1.0, out = 1.0, dcA = 0.0, envA = 0.0, envR = 0.0, dryGain = 1.0;
    std::array<Channel, 2> ch;
    std::vector<float> wet;
};
} // namespace lynx::emberclassic
