#pragma once

// Audio editor tools (round 165l): LYNX's versions of FL Edison's tool forms — Amp (left / right / panning law /
// stereo separation), Equalize, Blur, Convolution reverb, Clean up (denoise with a noise profile, declipper,
// declicker), Vocal denoise, Match spectrum, Claw machine, Drum(loop) stretch, Loop tuner, the noise gating and
// region batch tools, lossy normalize, noise generation and the point-envelope tools (Scale levels, Decimate,
// Filter, Smooth up, Create sequence) with the amp envelopes drawn over the audio. The tool set and the parameters
// follow FL's forms (docs/analise/REA_rodada165l.md); every algorithm here is LYNX's own. JUCE-free; the tools run
// off the audio thread (the editor runs them on a worker with `Progress`); tests in tests/AudioEditToolsTests.inc.

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/AudioEdit.h"

namespace lynx::audioedit
{
// Progress and cancellation of a long tool (written by the tool, read by the UI; `cancel` set by the UI).
struct Progress
{
    std::atomic<float> done { 0.0f };
    std::atomic<bool> cancel { false };
    bool cancelled() const noexcept { return cancel.load (std::memory_order_relaxed); }
    void set (float f) noexcept { done.store (f, std::memory_order_relaxed); }
};

// --- point envelopes (the curves drawn in the tool forms and over the audio) ------------------------------------
// x 0..1 (time or frequency, depending on the tool), y 0..1; `tension` bends the segment that ends at the point
// (-1..1, 0 = straight; positive = slow start). Points are kept sorted by x.
struct EnvPoint
{
    double x = 0.0, y = 0.0;
    float tension = 0.0f;
};

struct Envelope
{
    std::vector<EnvPoint> points;

    bool empty() const noexcept { return points.empty(); }
    // The value at x (the first / last point's value outside them; 0 when there are no points).
    double at (double x) const noexcept;
    void sort();
    // Index of the point inserted at (x, y).
    int insert (double x, double y, float tension = 0.0f);

    static Envelope flat (double y); // two points (0, y) and (1, y)
};

// Bends a straight 0..1 segment position by a tension (-1..1).
double tensionCurve (double t, float tension) noexcept;

// --- point-envelope tools (Edison's Tools › Envelope; also usable on any Envelope) -------------------------------
// FL's "Level scaling" form: y -> centre + (y - centre) * multiply + offset, then the tension bends the result
// (centre 0..1, multiply -2..2, offset -1..1, tension -1..1); results are clamped to 0..1.
void scaleLevels (Envelope& env, double centre, double multiply, double offset, double tension);
void flipLevels (Envelope& env);       // y -> 1 - y
void normalizeLevels (Envelope& env);  // the lowest point to 0, the highest to 1
void makePointsSmooth (Envelope& env); // every segment an S-curve (tension 0 with the smooth flag: here +-0.5 pairs)
// Removes points that a straight line through their neighbours explains within `threshold` (0..1: 0 keeps all,
// 1 keeps only the turning points). The first and the last point are always kept.
void decimatePoints (Envelope& env, double threshold);
// Smooth up: limits how fast the envelope may rise (attack) and fall (release) — times 0..1 of the envelope's
// length, slopes 0..1 (0.5 = linear) — then decimates; `offset` (-1..1) moves the result in time.
void smoothUp (Envelope& env, double attack, double release, double attackSlope, double releaseSlope, double offset, double decimation);
// Filter: convolves the envelope (sampled finely) with `impulse` (its shape, x over -width..+width), then
// decimates; width 0..1 of the length, offset -1..1.
void filterEnvelope (Envelope& env, const Envelope& impulse, double width, double offset, double decimation);
// Create sequence (FL's "Create sequence" form): writes a stepped gate pattern with an ADSR shape per step.
struct SequenceSettings
{
    std::vector<float> levels;     // per step 0..1 (the pattern's height)
    std::vector<bool> breaks;      // per step: true = the step is silent
    int stepsPerUnit = 2;          // "Time mul": steps per beat (FL 0..10, default 2)
    bool pingPong = false;
    double swing = 0.0;            // 0..1
    double attack = 0.0;           // 0..0.9 of a step
    double decay = 0.5;            // 0..0.9 of a step
    double sustain = 0.8;          // 0..1
    double gate = 0.55;            // 0.1..1 of a step
};
// `beats` = the envelope's length in beats.
Envelope createSequence (const SequenceSettings& s, double beats);

// --- amp envelopes over the audio ------------------------------------------------------------------------------
// Edison draws volume, panning and stereo separation envelopes over the audio (x = position in the whole audio,
// y 0..1; volume 0.5 = unity like FL's amp envelope, panning / separation 0.5 = centre) and mixes them down.
enum class EnvTarget : std::uint8_t
{
    volume,
    panning,
    stereoSeparation
};
// volume y -> gain: 0 .. 1 (0.5) .. 2 (1.0) in amplitude, the curve FL's envelope uses for "volume".
float envelopeGain (double y) noexcept;
// Applies the envelopes (the empty ones are skipped) to the whole audio.
void mixdownEnvelopes (Document& doc, const Envelope& volume, const Envelope& panning, const Envelope& separation);
// Points at the selection's edges (keeping the current values there) — "Add points at selection".
void addPointsAt (Envelope& env, const std::vector<double>& xs);

// --- amp ---------------------------------------------------------------------------------------------------------
struct AmpSettings
{
    double volume = 1.0;          // FL: -2..2 (the wheel is -512..512, 256 = 1); negative inverts
    double left = 1.0, right = 1.0;
    double pan = 0.0;             // -1..1
    bool circularLaw = true;      // constant power (circular) or linear (triangular)
    double stereoSeparation = 0.0; // -1 (mono) .. 0 .. 1 (wider)
};
void ampTool (Document& doc, Range r, const AmpSettings& s);

// Lossy normalize: normalizes so that `clipFraction` of the samples (the loudest few) go over full scale and are
// soft-clipped, giving a louder result than a plain normalize. Returns the gain applied.
float lossyNormalize (Document& doc, Range r, double clipFraction = 0.0005);
// Stereo audio whose channels are the same (within `tolerance`) becomes mono. True if it was converted.
bool monoIfMono (Document& doc, float tolerance = 1.0e-6f);
// White noise at `level` (peak, linear) over the range (an empty range: inserted at r.start, `frames` long).
void generateNoise (Document& doc, Range r, float level, std::uint32_t seed, std::int64_t framesIfEmpty = 0);

// --- noise gating ------------------------------------------------------------------------------------------------
// The threshold heard in the range (its peak level, linear): FL's "Acquire noise threshold" on a quiet part.
float acquireNoiseThreshold (const Audio& audio, Range r) noexcept;
// Parts (of at least `minSilence` seconds) below the threshold. Frames [start, end) per part.
std::vector<Range> quietParts (const Audio& audio, Range r, float threshold, double minSilence = 0.05);
// Gate noise: silences the quiet parts (short fades at their edges).
int gateNoise (Document& doc, Range r, float threshold);
// Trim side noise: removes the quiet start and end of the range.
void trimSideNoise (Document& doc, Range r, float threshold);
// Trim all noise & slice up: removes every quiet part and marks each remaining piece as a region.
int trimNoiseAndSlice (Document& doc, Range r, float threshold);

// --- regions ------------------------------------------------------------------------------------------------------
int normalizeRegions (Document& doc);
int declickRegions (Document& doc, bool in, bool out);
// "Perfect all regions": each region's edges snap to zero crossings, then declick in and out.
int perfectRegions (Document& doc);

// --- spectral -------------------------------------------------------------------------------------------------
// Frequency axis of the curve editors: x 0..1 -> Hz. `freqScale` 0..1 blends linear (0) and logarithmic (1).
double curveFrequency (double x, double freqScale, double sampleRate) noexcept;
double curvePosition (double hz, double freqScale, double sampleRate) noexcept;

struct EqSettings
{
    Envelope gain;          // y 0..1: 0.5 = 0 dB, 1 = +18 dB, 0 = -inf (FL's envelope centre line is unity)
    double freqScale = 1.0; // 0..1
    bool smooth = true;     // smooths the final response (less ringing)
    double mix = 1.0;       // 0..1
};
// gain y -> linear gain of the EQ curve.
double eqGain (double y) noexcept;
void equalize (Document& doc, Range r, const EqSettings& s, Progress* progress = nullptr);
// "Filter unharmonics": an EQ curve that keeps only the harmonics of `fundamentalHz` (width 0 narrow .. 2 wide).
Envelope unharmonicsCurve (double fundamentalHz, int width, double freqScale, double sampleRate);

struct BlurSettings
{
    double amount = 0.5;          // 0..1: the blur's length (up to ~2 s)
    double offset = 0.0;          // -1..1: moves the blurred result in time
    bool useOutside = true;       // the audio around the selection feeds the blur
    double mix = 1.0;             // 0..1 (dry/wet)
    Envelope impulse;             // the blur's time shape (empty: a decaying default)
};
void blur (Document& doc, Range r, const BlurSettings& s, Progress* progress = nullptr);

struct ReverbSettings
{
    Audio impulse;                // a loaded impulse; empty: one is made from `shape`
    Envelope shape;               // the drawn impulse's level over its length (empty: a 2 s exponential decay)
    double length = 2.0;          // seconds of the drawn impulse
    double offset = 0.0;          // -1..1 (pre-delay up to +-1 s, negative = the wet starts earlier)
    bool addTail = true;          // the reverb's tail extends the audio
    bool useOutside = true;
    double dry = 1.0, wet = 0.25; // 0..1 (FL: 256 = 1, wet default 64)
};
// The impulse made from the drawn shape (stereo noise, LYNX's own generator, normalised).
Audio makeReverbImpulse (const Envelope& shape, double seconds, double sampleRate, std::uint32_t seed = 1);
void convolutionReverb (Document& doc, Range r, const ReverbSettings& s, Progress* progress = nullptr);

// The noise profile: the average magnitude per bin of a noise-only part (FFT 4096).
struct NoiseProfile
{
    std::vector<float> magnitude; // per bin (fftSize / 2 + 1)
    int fftSize = 0;
    double sampleRate = 0.0;
    bool valid() const noexcept { return fftSize > 0 && ! magnitude.empty(); }
};
NoiseProfile acquireNoiseProfile (const Audio& audio, Range r, int fftSize = 4096);

struct DenoiseSettings
{
    bool denoise = true;
    double freqScale = 0.0;   // 0..1 of the threshold curve's axis
    double threshold = 0.5;   // 0..1: the noise floor's scale (0.5 = the profile as measured, +-18 dB)
    double amount = 0.5;      // 0..1: how much of the noise goes (0.5 ~ -18 dB, 1 = all)
    bool noiseOnly = false;   // outputs what would be removed
    Envelope thresholdCurve;  // per frequency, y 0.5 = no change (empty: flat)
    bool declip = false;
    double clipThreshold = 0.9; // 0..1 of full scale: samples above it are rebuilt
    bool declick = false;
    double clickSensitivity = 0.25; // 0..1
};
// Without a valid profile the noise floor is estimated from the quietest frames of the range.
void denoise (Document& doc, Range r, const NoiseProfile& profile, const DenoiseSettings& s, Progress* progress = nullptr);
// Declipper / declicker on their own (also used by denoise).
int declip (Audio& audio, Range r, double threshold);
int declick (Audio& audio, Range r, double sensitivity);

// Vocal denoise / isolator: mix -1 (only the noise) .. 0 .. 1 (only the voice).
void vocalDenoise (Document& doc, Range r, double mix, Progress* progress = nullptr);
// Match spectrum: makes the range's long-term spectrum match `reference`'s (amount 0..1, smoothing in octaves).
void matchSpectrum (Document& doc, Range r, const Audio& reference, double amount = 1.0, double smoothingOctaves = 1.0 / 3.0,
                    Progress* progress = nullptr);

// --- time ---------------------------------------------------------------------------------------------------------
struct ClawSettings
{
    double periodBeats = 1.0;    // 1/4 .. 16 beats
    int trashPart = 4, trashOf = 4; // "trash every" n/m: the n-th of m parts of each period is removed
    bool snapToMarkers = true;   // periods start at the markers when there are any
    double compensation = 0.0;   // 0..1: how much the rest is stretched back to the original length
    bool resample = false;       // compensation by resampling (pitch follows) instead of a stretch
};
void clawMachine (Document& doc, Range r, const ClawSettings& s, double bpm, Progress* progress = nullptr);

struct DrumStretchSettings
{
    double ratio = 1.0;          // new length / old length
    double pitchSemitones = 0.0; // pitch of each slice (resampled within the slice)
    bool insertTails = true;     // the stretched range replaces the original (else pasted over, same length)
    bool autoSlice = false;      // slices from onsets (else markers, else onsets)
    enum class Fill : std::uint8_t { none, echoGrains, stretch, pogo } fill = Fill::echoGrains;
    bool blur = true, randomPresence = false, loop = false;
    enum class Tail : std::uint8_t { none, smoothFade, append } tail = Tail::smoothFade;
    double transient = 0.7;      // 0..1: onset sensitivity
    double tailSlope = 1.0;      // 0..2
    double tailSmoothing = 0.25; // 0.05..2
};
void drumloopStretch (Document& doc, Range r, const DrumStretchSettings& s, Progress* progress = nullptr);

struct LoopTuneSettings
{
    double crossfade = 0.5;      // 0..1 of the longest possible crossfade (FL 0..5000, default 2500)
    double tension = 0.25;       // 0..1
    int periodSnap = 5;          // 1..7: how strongly the loop points snap to the pitch period (1 = off)
    int blurOrder = 5;           // 1..25
    double blurScale = 0.0;      // -1..1
    double blurGain = 1.0;       // 0.5..1.5
};
// Makes the loop (or the selection, or the whole audio) loop seamlessly: the loop points move to matching zero
// crossings / periods and the part before the loop start is crossfaded into the loop end.
void tuneLoop (Document& doc, Range r, const LoopTuneSettings& s, Progress* progress = nullptr);

// --- shared DSP --------------------------------------------------------------------------------------------------
// In-place complex FFT of a power-of-two length (inverse without the 1/n scale).
void fftInPlace (std::vector<double>& re, std::vector<double>& im, bool inverse);
} // namespace lynx::audioedit
