#pragma once

// Vocal pitch toolkit shared by LYNX Tune (the real-time pitch corrector, core/LynxTune.h) and the Newtone engine
// (core/NewtoneFx.h). Written only from published DSP (docs/analise/REA_rodada95.md §4):
//
// - Pitch detection: YIN (A. de Cheveigné, H. Kawahara, "YIN, a fundamental frequency estimator for speech and
//   music", JASA 111(4), 2002): difference function, cumulative mean normalised difference, absolute threshold,
//   parabolic interpolation. It runs on a low-passed, decimated copy of the signal (~11 kHz) and the period is then
//   refined on the full-rate signal around the coarse estimate (a plain difference function + parabola).
// - Pitch shifting: TD-PSOLA (E. Moulines, F. Charpentier, "Pitch-synchronous waveform processing techniques for
//   text-to-speech synthesis using diphones", Speech Communication 9, 1990; U. Zölzer (ed.), DAFX, ch. 7). Two-period
//   Hann grains are taken at analysis marks one period apart and overlap-added at synthesis marks one target period
//   apart, so the spectral envelope (formants) stays where it was. Resampling each grain moves the formants
//   (formant shift, or "formant preserve" off).
//
// Nothing here allocates after construction / prepare().

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace lynx::vocal
{
extern int experiment;          // EXPERIMENT (round 165j tuning, removed before landing)
extern float experimentRelaxed;
extern float experimentBridgeMs;
extern float experimentLock;
extern float experimentNormLo;

// ---------------------------------------------------------------------------------------------------------------------
// Scales

enum class Scale : int
{
    chromatic, major, minor, harmonicMinor, melodicMinor, majorPentatonic, minorPentatonic, blues, dorian, mixolydian,
    phrygian, lydian, locrian, custom,
    wholeTone, diminished // round 128 (Auto-Tune's modern list): chosen through LYNX Tune's "More scales" parameter
};
constexpr int scaleCount = 14;    // the scales a "Scale" parameter steps through (chromatic … custom; saved projects)
constexpr int scaleCountAll = 16; // with the two extra scales
const char* scaleName (Scale s) noexcept;
// Allowed pitch classes (bit k = pitch class (key + k) mod 12 relative to C) of a scale on a key (0 = C … 11 = B).
// `custom` returns `customMask` (bit k = pitch class k).
std::uint16_t scaleMask (Scale s, int key, std::uint16_t customMask = 0xfff) noexcept;

// Key detection (round 128, LYNX Tune's AUTO KEY): the key and mode (Scale::major / Scale::minor) whose
// Krumhansl–Kessler profile correlates best with a pitch-class histogram (bin k = time spent on pitch class k).
// Returns {-1, major} when the histogram is empty.
std::pair<int, Scale> detectKey (const std::array<double, 12>& histogram) noexcept;

// The allowed note (MIDI number) closest to `note` (fractional MIDI number). `mask` bit k = pitch class k allowed;
// an empty mask returns the nearest semitone. Hysteresis: while `current` is allowed and `note` is within
// 0.5 + `hysteresis` semitones of it, `current` is kept (no flipping between neighbours on a wobbly note).
int nearestNote (double note, std::uint16_t mask, int current = -1, double hysteresis = 0.0) noexcept;

// MIDI note number of a frequency for a reference A4 (Hz), and back
double hzToNote (double hz, double a4 = 440.0) noexcept;
double noteToHz (double note, double a4 = 440.0) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// Pitch detector (streaming)

struct PitchEstimate
{
    double period = 0.0;  // full-rate samples (fractional); 0 = none yet
    double hz = 0.0;
    float clarity = 0.0f; // 1 − the YIN dip (1 = perfectly periodic)
    float level = 0.0f;   // RMS of the analysed window
    bool voiced = false;
};

class PitchDetector
{
public:
    static constexpr int maxDecimated = 4096; // decimated history (power of two)
    static constexpr int maxFull = 16384;     // full-rate history (power of two): ≥ the refine window + the longest period at 192 kHz

    PitchDetector();
    // fmin / fmax in Hz; `hop` = full-rate samples between two estimates (rounded to whole decimated samples).
    void prepare (double sampleRate, double fminHz, double fmaxHz, int hop) noexcept;
    void reset() noexcept;
    // YIN's absolute threshold (0.05 strict … 0.3 relaxed) and the RMS level under which the signal counts as silent
    void setThreshold (float t) noexcept { threshold = t; }
    void setGate (float rms) noexcept { gate = rms; }
    // Round 157: prefer a clearly deeper dip at 1.5 / 2 / 3 / 4 times the first one when it continues the last voiced
    // period (harmonic errors on strong formants). Off by default (LYNX Tune turns it on).
    void setHarmonicGuard (bool on) noexcept { harmonicGuard = on; }

    // Feeds one sample; returns true when a new estimate is ready (then read estimate()).
    bool push (float x) noexcept;
    const PitchEstimate& estimate() const noexcept { return est; }
    // How far back (full-rate samples) the centre of the analysed window lies from the newest sample
    int windowCentre() const noexcept { return centre; }
    int decimation() const noexcept { return dec; }
    double maxPeriod() const noexcept { return sr / fmin; }

private:
    void analyse() noexcept;

    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float run (float x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    double sr = 44100.0, fmin = 70.0, fmax = 1000.0;
    int dec = 4, hopDec = 32, tauMin = 10, tauMax = 160, window = 160, centre = 0;
    int phase = 0, hopCount = 0;
    std::int64_t written = 0, writtenDec = 0;
    float threshold = 0.15f, gate = 0.0005f;
    bool harmonicGuard = false;
    double lastTau = 0.0;               // the last voiced estimate's lag (decimated samples)
    int sinceVoiced = 1 << 20, guardMemory = 16; // estimates since then; the guard remembers it this long
    std::array<Biquad, 2> lp {};
    std::vector<float> full, deci, diff;
    PitchEstimate est;
};

// Offline analysis of a whole signal (the Newtone engine): one estimate every `hop` samples; frame k describes the
// signal around sample k·hop.
struct PitchFrame
{
    double hz = 0.0; // 0 = unvoiced
    float clarity = 0.0f, level = 0.0f;
};
std::vector<PitchFrame> analysePitch (const float* x, int n, double sampleRate, int hop, double fminHz = 60.0,
                                      double fmaxHz = 1000.0, float threshold = 0.15f);

// ---------------------------------------------------------------------------------------------------------------------
// TD-PSOLA shifter (streaming, up to two channels sharing the pitch marks)

class Psola
{
public:
    static constexpr int ringSize = 32768; // per channel (power of two): ≥ latency + 3 longest periods at 192 kHz
    static constexpr int maxMarks = 256;

    Psola();
    // `maxPeriod` = the longest period handled (samples); `latency` = the fixed delay between input and output
    // (≥ 2·maxPeriod recommended; less trades quality for latency: grains then come from the newest available mark).
    void prepare (double sampleRate, double maxPeriod, int latency) noexcept;
    void reset() noexcept;
    int latency() const noexcept { return delay; }

    // Control for the samples that follow: the input's current period (samples; ≤ 0 or !voiced = unvoiced), the pitch
    // ratio (output / input frequency) and the formant factor (> 1 moves the formants up; 1 = preserved).
    void setPitch (double period, bool voiced) noexcept;
    void setShift (double ratio, double formant) noexcept;

    // One frame: `in` / `out` hold `channels` samples (1 or 2). The output is the input delayed by latency(), shifted.
    void process (const float* in, float* out, int channels) noexcept;

    // Marks align to the waveform near the predicted position (pitch-synchronous): where it best repeats the period at
    // the previous mark (round 148), the strongest peak for a phrase's first mark. Default on.
    void setPeakAlign (bool on) noexcept { peakAlign = on; }
    // Marks are placed this many samples behind the newest input (the centre of the pitch detector's window, so the
    // period and shift given with setPitch / setShift describe the signal where the mark lies). Clamped so that the
    // grains stay within the latency.
    void setMarkLag (int samples) noexcept;

private:
    struct Mark
    {
        double pos = 0.0;   // absolute input sample
        float period = 0.0f;
        bool voiced = false;
        float ratio = 1.0f, formant = 1.0f; // the shift wanted when the mark was placed (it goes with that period)
    };

    void placeMarks() noexcept;
    float readIn (int ch, double pos) const noexcept;

    double sr = 44100.0, maxP = 600.0;
    int delay = 1200, unvoicedHop = 256, lag = 0;
    std::int64_t now = 0;        // samples written so far
    double nextMark = 0.0;       // next analysis mark (absolute)
    double nextSynth = 0.0;      // next synthesis mark (absolute, output timeline = input timeline)
    double lastMark = 0.0;       // the last voiced mark placed (absolute; 0 = none): the next one correlates with it
    double period = 0.0, ratio = 1.0, formant = 1.0;
    std::int64_t voicedAt = -(1 << 30); // EXPERIMENT E2
    bool voiced = false, peakAlign = true, lastVoiced = false;
    std::array<Mark, maxMarks> marks {};
    int markHead = 0, markCount = 0;
    std::vector<float> in[2], acc[2], env; // input history, output accumulators, a low-passed copy for peak search
    float envZ = 0.0f;
    std::vector<float> hfEnv, wsum; // EXPERIMENT E3 / E6
    float hpZ = 0.0f, hfZ = 0.0f;
    int hfLead = 44;
};
} // namespace lynx::vocal
