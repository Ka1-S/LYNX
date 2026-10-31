#include "core/LynxTune.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "core/Effects.h"
#include "core/Song.h"
#include <mutex>
#include "core/VocalPitch.h"

namespace lynx::lynxtune
{
namespace
{
constexpr double maxRate = 192000.0;
// how far past the half-way point (semitones) a held target is kept: enough to stop a wobbly note between two
// neighbours from flipping, small enough that a glide is not pulled back toward the note it left (round 120: 0.1
// biased a glide's correction by ~10 c against its direction; Waves Tune Real-Time measured shows no such bias)
constexpr double hysteresisSt = 0.03;

struct RangeInfo
{
    const char* name;
    double fmin, fmax;
};
// voice ranges: the lowest pitch sets the detector window and the latency
constexpr RangeInfo ranges[rangeCount] { { "Soprano", 150.0, 1200.0 }, { "Alto / Tenor", 80.0, 1000.0 },
                                         { "Low male", 55.0, 700.0 }, { "Instrument", 40.0, 1200.0 } };
constexpr const char* keyNames[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

int stepped (float v, int steps) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * (float) (steps - 1)), 0, steps - 1); }
} // namespace

const char* rangeName (int r) noexcept { return ranges[std::clamp (r, 0, rangeCount - 1)].name; }
double rangeMinHz (int r) noexcept { return ranges[std::clamp (r, 0, rangeCount - 1)].fmin; }
double rangeMaxHz (int r) noexcept { return ranges[std::clamp (r, 0, rangeCount - 1)].fmax; }
int rangeIndex (float v) noexcept { return stepped (v, rangeCount); }
int scaleIndex (float v) noexcept { return stepped (v, vocal::scaleCount); }
int keyIndex (float v) noexcept { return stepped (v, 12); }
double retuneMs (float v) noexcept { return 400.0 * (double) v * (double) v; }
double formantSemitones (float v) noexcept { return 14.0 * ((double) std::clamp (v, 0.0f, 1.0f) - 0.5); }
double formantSemitones (float v, bool wide) noexcept { return (wide ? 24.0 : 14.0) * ((double) std::clamp (v, 0.0f, 1.0f) - 0.5); }
int transposeSemitones (float v) noexcept { return stepped (v, 25) - 12; }
double referenceHz (float v) noexcept { return 430.0 + 20.0 * (double) std::clamp (v, 0.0f, 1.0f); }
int midiChannelIndex (float v) noexcept
{
    const auto k = (int) std::lround (std::clamp (v, 0.0f, 1.0f) * 128.0f);
    return k <= 0 ? -1 : k - 1;
}

double naturalVibratoDb (float v) noexcept { return 24.0 * ((double) std::clamp (v, 0.0f, 1.0f) - 0.5); }
int vibratoShapeIndex (float v) noexcept { return stepped (v, vibratoShapes); }
double vibratoRateHz (float v) noexcept { return 0.1 + 9.9 * (double) std::clamp (v, 0.0f, 1.0f); }
double vibratoDepthCents (float v) noexcept { return 100.0 * (double) std::clamp (v, 0.0f, 1.0f); }
double vibratoOnsetMs (float v) noexcept { return 1500.0 * (double) std::clamp (v, 0.0f, 1.0f); }

// round 165hb: Throat length — 50 … 100 % on the knob's lower half, 100 … 180 % on the upper half (exponential halves)
double throatPercent (float v) noexcept
{
    const auto x = (double) std::clamp (v, 0.0f, 1.0f);
    return x < 0.5 ? 50.0 * std::exp2 (x / 0.5) : 100.0 * std::pow (1.8, (x - 0.5) / 0.5);
}
float throatValue (double percent) noexcept
{
    percent = std::clamp (percent, 50.0, 180.0);
    return (float) (percent < 100.0 ? 0.5 * std::log2 (percent / 50.0) : 0.5 + 0.5 * std::log (percent / 100.0) / std::log (1.8));
}
// [EXEC] Auto-Tune Pro 11.6 black-box (REA_rodada165hb §2): a single +24 dB resonance (400 … 4000 Hz) under a 147 Hz
// harmonic voice moves by −12·log2 (L / 100) semitones within ±1 st at 50 … 180 % — a little more below 100 % (+13.0 st
// at 50 %, +6.1 at 72 %) and a little less above it (−5.0 at 135 %, −9.4 at 180 %); with Formant Correction off the
// throat does nothing. (Round 138's half-size law came from a synthetic vowel whose formants were too flat to follow.)
double throatSemitones (double percent) noexcept
{
    percent = std::clamp (percent, 50.0, 180.0);
    const auto st = -12.0 * std::log2 (percent / 100.0);
    return st * (percent < 100.0 ? 1.08 : 0.93);
}
// round 166c: the created vibrato's amplitude (the reference's 0 … 30 units) and the steps of Scale transpose
double vibratoAmplitudeOf (float v) noexcept { return 30.0 * (double) std::clamp (v, 0.0f, 1.0f); }
// [model] (the reference trial could no longer be hosted in round 166c, REA_rodada166c §2): amount A swings the level
// by ±A % with the vibrato's pitch (30 = ±30 %: +2.3 / −3.1 dB), up when the pitch is up
double vibratoAmplitudeDepth (double amount) noexcept { return std::clamp (amount, 0.0, 30.0) / 100.0; }
int scaleTransposeSteps (float v) noexcept { return stepped (v, 15) - 7; }
float scaleTransposeValue (int steps) noexcept { return (float) (std::clamp (steps, -7, 7) + 7) / 14.0f; }
int transposeInScaleNote (int note, std::uint16_t mask, int steps) noexcept
{
    mask &= 0xfff;
    if (mask == 0 || steps == 0)
        return note;
    auto in = [mask] (int n) { return ((mask >> (((n % 12) + 12) % 12)) & 1u) != 0; };
    auto base = note;
    for (int k = 0; k < 12 && ! in (base); ++k)
        --base; // a note outside the scale moves from the nearest scale note below it, keeping its distance
    auto out = base;
    for (int k = 0; k < std::abs (steps); ++k)
        do
            out += steps > 0 ? 1 : -1;
        while (! in (out));
    return out + (note - base);
}
// [EXEC] the trial's taper (round 120, docs/specs/waves-tune-real-time.md): 0 … 0.5 → 0.1 … 50 ms, 0.5 … 2/3 → 50 …
// 120 ms, 2/3 … 1 → 120 … 800 ms
double speedMs (float v) noexcept
{
    const auto x = (double) std::clamp (v, 0.0f, 1.0f);
    if (x <= 0.5) return 0.1 + 49.9 * x / 0.5;
    if (x <= 2.0 / 3.0) return 50.0 + 70.0 * (x - 0.5) / (2.0 / 3.0 - 0.5);
    return 120.0 + 680.0 * (x - 2.0 / 3.0) / (1.0 / 3.0);
}
float speedValue (double ms) noexcept
{
    ms = std::clamp (ms, 0.1, 800.0);
    if (ms <= 50.0) return (float) (0.5 * (ms - 0.1) / 49.9);
    if (ms <= 120.0) return (float) (0.5 + (2.0 / 3.0 - 0.5) * (ms - 50.0) / 70.0);
    return (float) (2.0 / 3.0 + (ms - 120.0) / 680.0 / 3.0);
}
double toleranceCentsOf (float v) noexcept { return 40.0 * (double) std::clamp (v, 0.0f, 1.0f); }
double toleranceTimeMs (float v) noexcept { return 300.0 * (double) std::clamp (v, 0.0f, 1.0f); }
double vibratoAmountOf (float v) noexcept { return 2.0 * (double) std::clamp (v, 0.0f, 1.0f); }

int effectiveScale (float scaleValue, float moreScalesValue) noexcept
{
    switch (stepped (moreScalesValue, 3))
    {
        case 1: return (int) vocal::Scale::wholeTone;
        case 2: return (int) vocal::Scale::diminished;
        default: return scaleIndex (scaleValue);
    }
}

int latencyFor (double sampleRate, int range, bool lowLatency) noexcept
{
    const auto pmax = sampleRate / rangeMinHz (range);
    // Round 157c (REA_rodada157c §5, live singing): low latency was 1.25 periods (15.7 ms for Alto / Tenor); half a
    // period (6.4 ms) measured on her takes in 128-sample blocks keeps the pitch (4.1 c), the harmonics-to-noise and
    // the HF clicks of Auto-Tune Pro's Classic mode (5 ms) — the grains then come from the newest complete period
    return (int) std::ceil ((lowLatency ? 0.5 : 2.0) * pmax) + 4;
}

const EffectInfo& info()
{
    static const EffectInfo i = [] {
        EffectInfo e { "LYNX Tune", {} };
        e.params = { { "Key", 0.0f },
                     { "Scale", 1.0f / (float) (vocal::scaleCount - 1) }, // Major
                     { "Retune", 0.2236068f },                           // 20 ms
                     { "Humanize", 0.0f },
                     { "Formant preserve", 1.0f },
                     { "Formant shift", 0.5f },
                     { "Transpose", 0.5f },
                     { "Reference", 0.5f }, // A4 = 440 Hz
                     { "Range", 1.0f / 3.0f },  // alto / tenor
                     { "Low latency", 0.0f },
                     { "MIDI", 0.0f },
                     { "Mix", 1.0f },
                     { "Tracking", 0.4f } };
        for (const auto* n : keyNames)
            e.params.push_back ({ n, 1.0f });
        e.params.push_back ({ "Flex-Tune", 0.0f });
        // round 128
        e.params.push_back ({ "Natural vibrato", 0.5f });
        static const char* const bypassNames[12] { "C bypass", "C# bypass", "D bypass", "D# bypass", "E bypass", "F bypass",
                                                   "F# bypass", "G bypass", "G# bypass", "A bypass", "A# bypass", "B bypass" };
        for (const auto* n : bypassNames)
            e.params.push_back ({ n, 0.0f });
        e.params.push_back ({ "Ignore vibrato", 0.0f });
        e.params.push_back ({ "Vibrato shape", 0.0f });
        e.params.push_back ({ "Vibrato rate", (float) ((5.5 - 0.1) / 9.9) }); // 5.5 Hz
        e.params.push_back ({ "Vibrato depth", 0.18f });                        // 18 c
        e.params.push_back ({ "Onset delay", 1.0f / 3.0f });                    // 500 ms
        e.params.push_back ({ "Onset rate", 1.0f / 3.0f });                     // 500 ms
        e.params.push_back ({ "More scales", 0.0f });
        // round 135f
        e.params.push_back ({ "Learn MIDI", 0.0f });
        e.params.push_back ({ "Classic", 0.0f });
        // round 135w (the real-time corrector's controls)
        e.params.push_back ({ "Speed link", 1.0f });
        e.params.push_back ({ "Speed", speedValue (15.0) });
        e.params.push_back ({ "Tolerance cents", 0.0f });
        e.params.push_back ({ "Tolerance time", 0.0f });
        e.params.push_back ({ "Keep vibrato", 1.0f });
        e.params.push_back ({ "Vibrato amount", 0.5f });
        e.params.push_back ({ "Correction", 1.0f });
        e.params.push_back ({ "Drive stage", 0.0f }); // round 136
        e.params.push_back ({ "Drive", 0.0f });
        e.params.push_back ({ "Wide formant", 0.0f });
        e.params.push_back ({ "Output", 0.5f });
        // round 147: an illegal note's direction (C … B) and the keyboard's range
        static const char* const dirNames[12] { "C dir", "C# dir", "D dir", "D# dir", "E dir", "F dir", "F# dir", "G dir", "G# dir", "A dir", "A# dir", "B dir" };
        for (const auto* nm : dirNames)
            e.params.push_back ({ nm, 0.0f });
        e.params.push_back ({ "Range low", 0.0f });
        e.params.push_back ({ "Range high", 1.0f });
        e.params.push_back ({ "Receive", 1.0f }); // round 150
        e.params.push_back ({ "Group octaves", 1.0f }); // round 152
        static const char* const mapNames[keyMapParams] { "Key map 1", "Key map 2", "Key map 3", "Key map 4", "Key map 5", "Key map 6",
                                                          "Key map 7", "Key map 8", "Key map 9", "Key map 10", "Key map 11", "Key map 12" };
        for (const auto* nm : mapNames)
            e.params.push_back ({ nm, 0.0f });
        e.params.push_back ({ "Reference tone", 0.0f });
        e.params.push_back ({ "Tone level", 0.5f });
        // round 165hb / 166c: throat, the created vibrato's amplitude / formant / variation, transpose in scale
        e.params.push_back ({ "Throat length", 0.5f }); // 100 %
        e.params.push_back ({ "Vibrato amplitude", 0.0f });
        e.params.push_back ({ "Vibrato formant", 0.0f });
        e.params.push_back ({ "Vibrato variation", 0.0f });
        e.params.push_back ({ "Transpose in scale", 0.0f });
        e.params.push_back ({ "Scale transpose", 0.5f }); // 0 steps
        return e;
    }();
    return i;
}

NoteDir noteDir (float v) noexcept { return (NoteDir) stepped (v, 3); }

// ---- round 152: Group Octaves off — the key map (8 keys per value, base 6, an exact integer / 2^24 in the float) ----
namespace
{
constexpr double keyMapScale = 16777216.0; // 2^24: every packed value (< 6^8 = 1 679 616) is exact in a float
std::uint32_t keyMapWord (const float* p, int i) noexcept
{
    return (std::uint32_t) std::clamp (std::lround ((double) p[keyMap1 + i] * keyMapScale), 0L, 1679615L);
}
} // namespace
bool groupOctavesOn (const float* p) noexcept { return p[groupOctaves] >= 0.5f; }
double referenceToneDb (float v) noexcept { return -18.0 + 36.0 * (double) std::clamp (v, 0.0f, 1.0f); }
KeyStatus keyOverride (const float* p, int note) noexcept
{
    const auto k = note - keyMapFirst;
    if (k < 0 || k >= keyMapKeys) return KeyStatus::follow;
    auto w = keyMapWord (p, k / keyMapPerParam);
    for (int j = 0; j < k % keyMapPerParam; ++j) w /= keyStatuses;
    return (KeyStatus) (w % keyStatuses);
}
void setKeyOverride (float* p, int note, KeyStatus st) noexcept
{
    const auto k = note - keyMapFirst;
    if (k < 0 || k >= keyMapKeys) return;
    const auto i = k / keyMapPerParam;
    std::uint32_t digits[keyMapPerParam] {};
    auto w = keyMapWord (p, i);
    for (auto& d : digits) { d = w % keyStatuses; w /= keyStatuses; }
    digits[k % keyMapPerParam] = (std::uint32_t) st;
    std::uint32_t out = 0;
    for (int j = keyMapPerParam - 1; j >= 0; --j) out = out * keyStatuses + digits[j];
    p[keyMap1 + i] = (float) ((double) out / keyMapScale);
}
bool anyKeyOverride (const float* p) noexcept
{
    for (int i = 0; i < keyMapParams; ++i)
        if (keyMapWord (p, i) != 0) return true;
    return false;
}
void clearKeyOverrides (float* p) noexcept
{
    for (int i = 0; i < keyMapParams; ++i) p[keyMap1 + i] = 0.0f;
}
KeyStatus noteStatus (const float* p, int note, std::uint16_t mask) noexcept
{
    if (! groupOctavesOn (p))
        if (const auto o = keyOverride (p, note); o != KeyStatus::follow) return o;
    const auto pc = ((note % 12) + 12) % 12;
    if (((mask >> pc) & 1u) == 0)
    {
        const auto d = noteDir (p[noteDirC + pc]);
        return d == NoteDir::up ? KeyStatus::removedUp : (d == NoteDir::down ? KeyStatus::removedDown : KeyStatus::removed);
    }
    return p[bypassC + pc] >= 0.5f ? KeyStatus::bypass : KeyStatus::legal;
}
int targetNoteKeys (double sung, const float* p, std::uint16_t mask, int previous, double hysteresis) noexcept
{
    auto allowed = [&] (int n) { const auto st = noteStatus (p, n, mask); return st == KeyStatus::legal || st == KeyStatus::bypass; };
    if (previous >= 0 && allowed (previous) && std::abs (sung - previous) < 0.5 + hysteresis)
        return previous;
    const auto base = (int) std::lround (sung);
    // an illegal sung key with a direction goes that way to the next allowed key
    const auto own = noteStatus (p, base, mask);
    if (own == KeyStatus::removedUp || own == KeyStatus::removedDown)
        for (int k = 1; k <= 24; ++k)
            if (const auto n = base + (own == KeyStatus::removedUp ? k : -k); allowed (n)) return n;
    int best = -1;
    double bestD = 1e9;
    for (int d = -12; d <= 12; ++d)
        if (const auto n = base + d; allowed (n) && std::abs (sung - n) < bestD - 1e-12)
        {
            bestD = std::abs (sung - n);
            best = n;
        }
    return best >= 0 ? best : base;
}
KeyStatus nextRemoveStatus (KeyStatus c) noexcept
{
    switch (c)
    {
        case KeyStatus::legal: case KeyStatus::bypass: case KeyStatus::follow: return KeyStatus::removed;
        case KeyStatus::removed: return KeyStatus::removedUp;
        case KeyStatus::removedUp: return KeyStatus::removedDown;
        default: return KeyStatus::legal;
    }
}
int rangeNote (float v) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 127.0f), 0, 127); }
float rangeNoteValue (int note) noexcept { return (float) std::clamp (note, 0, 127) / 127.0f; }
const char* rangePresetName (int k) noexcept
{
    static const char* const n[rangePresets] { "Bass", "Baritone", "Tenor", "Alto", "Mezzo", "Soprano", "Generic" };
    return n[std::clamp (k, 0, rangePresets - 1)];
}
std::pair<int, int> rangePresetNotes (int k) noexcept
{
    // [model] the classical voice ranges (bass E2–E4, baritone A2–A4, tenor C3–C5, alto F3–F5, mezzo A3–A5, soprano
    // C4–C6) widened by a third on each side; Generic = the whole keyboard (no note bypassed)
    static constexpr std::pair<int, int> r[rangePresets] { { 36, 68 }, { 41, 73 }, { 44, 76 }, { 49, 81 }, { 53, 85 }, { 56, 88 }, { 0, 127 } };
    return r[std::clamp (k, 0, rangePresets - 1)];
}
int targetNote (double sung, std::uint16_t legal, int previous, double hysteresis, const NoteDir* dirs) noexcept
{
    const auto t = vocal::nearestNote (sung, legal, previous, hysteresis);
    legal &= 0xfff;
    if (dirs == nullptr || legal == 0 || t == previous)
        return t;
    const auto base = (int) std::lround (sung);
    const auto pc = ((base % 12) + 12) % 12;
    if ((legal >> pc) & 1u)
        return t;
    const auto d = dirs[pc];
    if (d == NoteDir::nearest)
        return t;
    const int step = d == NoteDir::up ? 1 : -1;
    for (int k = 1; k <= 12; ++k)
    {
        const auto n = base + step * k;
        if ((legal >> (((n % 12) + 12) % 12)) & 1u)
            return n;
    }
    return t;
}

std::string paramText (int param, float v)
{
    char t[48];
    auto onOff = [v] { return std::string (v >= 0.5f ? "On" : "Off"); };
    switch (param)
    {
        case key: return keyNames[keyIndex (v)];
        case scale: return vocal::scaleName ((vocal::Scale) scaleIndex (v));
        case naturalVibrato: std::snprintf (t, sizeof t, "%+.1f dB", naturalVibratoDb (v)); return t;
        case ignoreVibrato: return onOff();
        case vibratoShape: { static const char* n[vibratoShapes] { "No vibrato", "Sine", "Square", "Sawtooth" }; return n[vibratoShapeIndex (v)]; }
        case vibratoRate: std::snprintf (t, sizeof t, "%.1f Hz", vibratoRateHz (v)); return t;
        case vibratoDepth: std::snprintf (t, sizeof t, "%.0f c", vibratoDepthCents (v)); return t;
        case vibratoDelay:
        case vibratoOnset: std::snprintf (t, sizeof t, "%.0f ms", vibratoOnsetMs (v)); return t;
        case moreScales: { static const char* n[3] { "Off", "Whole tone", "Diminished" }; return n[stepped (v, 3)]; }
        case speedLink: case keepVibrato: return onOff();
        case speed: std::snprintf (t, sizeof t, speedMs (v) < 10.0 ? "%.1f ms" : "%.0f ms", speedMs (v)); return t;
        case toleranceCents: if (v <= 0.0f) return "Off"; std::snprintf (t, sizeof t, "%.0f c", toleranceCentsOf (v)); return t;
        case toleranceTime: if (v <= 0.0f) return "Off"; std::snprintf (t, sizeof t, "%.0f ms", toleranceTimeMs (v)); return t;
        case vibratoAmount: case correction: std::snprintf (t, sizeof t, "%.0f%%", 100.0 * (param == vibratoAmount ? vibratoAmountOf (v) : (double) v)); return t;
        case retune: std::snprintf (t, sizeof t, "%.0f ms", retuneMs (v)); return t;
        case humanize:
        case flexTune:
        case mix: std::snprintf (t, sizeof t, "%.0f%%", 100.0 * v); return t;
        case tracking: std::snprintf (t, sizeof t, "%.0f%%", 100.0 * v); return t;
        case formantPreserve:
        case learnMidi:
        case classicMode:
        case lowLatency: return onOff();
        case formantShift: std::snprintf (t, sizeof t, "%+.1f st", formantSemitones (v)); return t;
        case driveStage: case wideFormant: return onOff();
        case drive: std::snprintf (t, sizeof t, "%.1f", 10.0 * v); return t;
        case outputTrim: std::snprintf (t, sizeof t, "%+.1f dB", -24.0 + 48.0 * v); return t;
        case transpose: std::snprintf (t, sizeof t, "%+d st", transposeSemitones (v)); return t;
        case reference: std::snprintf (t, sizeof t, "%.1f Hz", referenceHz (v)); return t;
        case range: return rangeName (rangeIndex (v));
        case midiChannel:
        {
            const auto c = midiChannelIndex (v);
            if (c < 0)
                return "Off";
            std::snprintf (t, sizeof t, "Channel %d", c + 1);
            return t;
        }
        default: break;
    }
    if ((param >= noteC && param < noteC + 12) || (param >= bypassC && param < bypassC + 12))
        return onOff();
    if (param >= noteDirC && param < noteDirC + 12)
    {
        static const char* const n[3] { "Nearest", "Up", "Down" };
        return n[(int) noteDir (v)];
    }
    if (param == receiveScale || param == groupOctaves || param == referenceTone) // round 152: Group Octaves, reference tone
        return onOff();
    if (param == throatLength) // round 165hb / 166c
    {
        std::snprintf (t, sizeof t, "%.0f%%", throatPercent (v));
        return t;
    }
    if (param == vibratoAmplitude)
    {
        std::snprintf (t, sizeof t, "%.1f", vibratoAmplitudeOf (v));
        return t;
    }
    if (param == vibratoFormant || param == vibratoVariation)
    {
        std::snprintf (t, sizeof t, "%.0f%%", 100.0 * (double) std::clamp (v, 0.0f, 1.0f));
        return t;
    }
    if (param == transposeInScale)
        return v >= 0.5f ? "In scale" : "Ignoring scale";
    if (param == scaleTranspose)
    {
        std::snprintf (t, sizeof t, "%+d steps", scaleTransposeSteps (v));
        return t;
    }
    if (param == referenceToneLevel)
    {
        std::snprintf (t, sizeof t, "%+.1f dB", referenceToneDb (v));
        return t;
    }
    if (param >= keyMap1 && param < keyMap1 + keyMapParams) // round 152: the key map (packed, edited on the keyboard)
    {
        std::snprintf (t, sizeof t, "keys %d-%d", keyMapFirst + keyMapPerParam * (param - keyMap1), keyMapFirst + keyMapPerParam * (param - keyMap1 + 1) - 1);
        return t;
    }
    if (param == rangeLow || param == rangeHigh)
    {
        static const char* const names[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto k = rangeNote (v);
        return std::string (names[k % 12]) + std::to_string (k / 12 - 1);
    }
    std::snprintf (t, sizeof t, "%.2f", (double) v);
    return t;
}

namespace
{
class Tune final : public Effect
{
public:
    Tune() : dry ((std::size_t) (2 * dryRing), 0.0f) {}

    void prepare (double sampleRate) noexcept override
    {
        sr = std::clamp (sampleRate > 0.0 ? sampleRate : 44100.0, 8000.0, maxRate);
        configure (curRange, curLow);
    }

    void reset() noexcept override
    {
        det.reset();
        psola.reset();
        activity = 0.0;
        hpX[0] = hpX[1] = hpY[0] = hpY[1] = 0.0;
        std::fill (dry.begin(), dry.end(), 0.0f);
        dryPos = 0;
        shift = 0.0;
        wanted = 0.0;
        target = -1;
        noteAge = 0.0;
        unvoicedFor = 1e9;
        sung = -1.0;
        center1 = center2 = 0.0;
        extraWanted = extra = 0.0;
        noteTime = 0.0;
        lfoPhase = 0.0;
        candidateTime = 0.0;
        candidate = -1;
        scaleOffset = 0;
        varRate = varDepth = varRateAim = varDepthAim = 1.0;
    }

    void setHeldNotes (const NoteMask* channels, int count) noexcept override
    {
        held = channels;
        heldCount = count;
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        const auto rg = rangeIndex (p[range]);
        // round 135: Classic runs the short-latency path. [EXEC] Auto-Tune Pro hosted black-box: "Use Classic Mode DSP"
        // keeps the retune trajectory (a 452 Hz tone at 150 ms: the same pitch at every point to 0.1 Hz) and drops the
        // reported latency from 2670 to 37 samples
        const auto low = p[lowLatency] >= 0.5f || p[classicMode] >= 0.5f;
        if (rg != curRange || low != curLow)
            configure (rg, low);

        det.setThreshold ((float) (0.05 + 0.25 * std::clamp (p[tracking], 0.0f, 1.0f)));

        // the target notes: the scale on the key, minus the notes switched off
        std::uint16_t notes = 0;
        for (int k = 0; k < 12; ++k)
            if (p[(std::size_t) (noteC + k)] >= 0.5f)
                notes = (std::uint16_t) (notes | (1u << k));
        const auto sc = (vocal::Scale) effectiveScale (p[scale], p[moreScales]);
        std::uint16_t bypass = 0;
        for (int k = 0; k < 12; ++k)
            if (p[(std::size_t) (bypassC + k)] >= 0.5f)
                bypass = (std::uint16_t) (bypass | (1u << k));
        // round 128: natural vibrato (gain on the vibrato already sung), ignore vibrato, created vibrato
        // round 135w: Keep vibrato / Vibrato amount multiply the natural-vibrato gain; Speed (unlinked) corrects the
        // in-note deviations on its own time; the tolerances widen and delay the note changes; Correction scales it all
        // round 138 ([EXEC] REA_rodada138 §3.6): on the reference the vibrato that survives the correction grows by about
        // 10^(dB/34) (+6 dB: x1.5, +12 dB: x2.1) and shrinks to no less than about 2/3 (-6 ... -12 dB), not by the full
        // 10^(dB/20) amplitude gain (was x3.9 / x0.27 at +-12 dB)
        const auto natDb = naturalVibratoDb (p[naturalVibrato]);
        const auto natGain = std::max (natDb < 0.0 ? 0.66 : 0.0, std::pow (10.0, natDb / 34.0))
                             * (p[keepVibrato] >= 0.5f ? vibratoAmountOf (p[vibratoAmount]) : 0.0);
        const auto linked = p[speedLink] >= 0.5f;
        const auto speedTau = speedMs (p[speed]) * 0.001;
        const auto hysteresis = hysteresisSt + toleranceCentsOf (p[toleranceCents]) / 100.0;
        const auto holdTime = toleranceTimeMs (p[toleranceTime]) * 0.001;
        const auto amount = (double) std::clamp (p[correction], 0.0f, 1.0f);
        const auto natural = std::abs (natGain - 1.0) > 1e-9 || ! linked;
        const auto ignoreVib = p[ignoreVibrato] >= 0.5f;
        const auto vibShape = vibratoShapeIndex (p[vibratoShape]);
        const auto vibInc = vibratoRateHz (p[vibratoRate]) / sr;
        const auto vibDepth = vibratoDepthCents (p[vibratoDepth]) / 100.0;
        const auto vibDelay = vibratoOnsetMs (p[vibratoDelay]) * 0.001;
        const auto vibOnset = vibratoOnsetMs (p[vibratoOnset]) * 0.001;
        // round 166c: the created vibrato's level swing, formant following and random variation [model]
        const auto vibAmp = vibratoAmplitudeDepth (vibratoAmplitudeOf (p[vibratoAmplitude]));
        const auto vibFormant = (double) std::clamp (p[vibratoFormant], 0.0f, 1.0f);
        const auto vibVar = (double) std::clamp (p[vibratoVariation], 0.0f, 1.0f);
        // round 165hb: the throat (measured law, formant correction on only); round 166c: transpose in scale
        const auto throatRatio = std::exp2 (throatSemitones (throatPercent (p[throatLength])) / 12.0);
        const auto inScale = p[transposeInScale] >= 0.5f;
        const auto scaleSteps = scaleTransposeSteps (p[scaleTranspose]);
        auto mask = (std::uint16_t) (vocal::scaleMask (sc, keyIndex (p[key]), notes) & notes);
        // round 147: the illegal notes' directions and the keyboard range (outside it: uncorrected)
        std::array<NoteDir, 12> dirs {};
        bool anyDir = false;
        for (int k = 0; k < 12; ++k)
        {
            dirs[(std::size_t) k] = noteDir (p[(std::size_t) (noteDirC + k)]);
            anyDir = anyDir || dirs[(std::size_t) k] != NoteDir::nearest;
        }
        const auto lowNote = rangeNote (p[rangeLow]), highNote = rangeNote (p[rangeHigh]);
        const bool ranged = lowNote > 0 || highNote < 127;
        // round 152: Group Octaves off with keys of their own — the per-key selection
        const bool keyed = ! groupOctavesOn (p.data()) && anyKeyOverride (p.data());

        // MIDI targets: the pitch classes held on the source channel (none held: the scale)
        const auto ch = midiChannelIndex (p[midiChannel]);
        std::uint16_t heldMask = 0;
        if (ch >= 0 && held != nullptr && ch < heldCount)
        {
            for (int k = 0; k < 128; ++k)
                if ((held[ch][(std::size_t) (k >> 6)] >> (k & 63)) & 1u)
                    heldMask = (std::uint16_t) (heldMask | (1u << (k % 12)));
            if (heldMask != 0 && p[learnMidi] < 0.5f) // learning (round 135): the notes teach the scale instead
                mask = heldMask;
        }
        meterHeld.store ((float) heldMask / 4096.0f, std::memory_order_relaxed);

        const auto aRef = referenceHz (p[reference]);
        const auto tau = retuneMs (p[retune]) * 0.001;
        const auto human = std::clamp ((double) p[humanize], 0.0, 1.0);
        const auto flex = std::clamp ((double) p[flexTune], 0.0, 1.0);
        const auto knobTrans = (double) transposeSemitones (p[transpose]);
        const auto fShift = std::exp2 (formantSemitones (p[formantShift], p[wideFormant] >= 0.5f) / 12.0);
        // round 136: the drive stage — [EXEC] the voice shifter's Drive on 200 Hz stairs (−40 … 0 dBFS, drive 0 … 10):
        // y = K·(s(G·x + b) − s(b)), s(v) = v / (1 + |v / c|^p)^(1/p), G = −2.646 + 3.653·drive dB, K = 0.751 − 1.988·drive
        // dB, c 1.2707, p 2.0545, b 0.341 (levels within 0.5 dB, H2 / H3 within a few dB, REA_rodada136 §3)
        const bool driveOn = p[driveStage] >= 0.5f;
        const double dv = 10.0 * std::clamp ((double) p[drive], 0.0, 1.0);
        const double dG = std::pow (10.0, (-2.646 + 3.6529 * dv) / 20.0), dK = std::pow (10.0, (0.751 - 1.9875 * dv) / 20.0);
        const double trimGain = std::pow (10.0, (-24.0 + 48.0 * std::clamp ((double) p[outputTrim], 0.0, 1.0)) / 20.0);
        auto side = [] (double v) { return v / std::pow (1.0 + std::pow (std::abs (v / 1.2707), 2.0545), 1.0 / 2.0545); };
        const double sideB = side (0.341);
        const auto preserve = p[formantPreserve] >= 0.5f;
        const auto wet = std::clamp (p[mix], 0.0f, 1.0f);
        const auto dt = 1.0 / sr;
        const auto stereo = r != nullptr && r != l;
        float peakIn = 0.0f, peakOut = 0.0f; // round 135w: the input / output meters

        for (int i = 0; i < n; ++i)
        {
            auto clean = [] (float v) { return std::isfinite (v) ? v : 0.0f; };
            const float xin[2] { clean (l[i]), clean (stereo ? r[i] : l[i]) };
            const auto mid = stereo ? 0.5f * (xin[0] + xin[1]) : xin[0];

            // round 166c: In scale, the transposition is the target note's own (steps of the scale), else the knob's
            const auto trans = inScale ? (double) scaleOffset : knobTrans;
            if (det.push (mid))
            {
                const auto& e = det.estimate();
                const auto hop = (double) detHop / sr;
                if (e.voiced && e.hz > 0.0)
                {
                    psola.setPitch (e.period, true);
                    sung = vocal::hzToNote (e.hz, aRef);
                    const auto newPhrase = unvoicedFor > 0.06;
                    // steadiness (humanize): how long the sung pitch has stayed within a few cents of its own recent
                    // average. [EXEC] Auto-Tune Pro (round 120 §5): even a ±5 c vibrato is not a sustained note
                    // A single move (the singer settles on another pitch) keeps the note sustained; another move 30 …
                    // 150 ms later (a vibrato, a wobble) makes it unsteady.
                    sinceJump += hop;
                    // the pitch with its vibrato smoothed away (two one-poles, 80 ms): ignore vibrato / natural vibrato
                    if (newPhrase)
                        center1 = center2 = sung;
                    else
                    {
                        const auto a = 1.0 - std::exp (-hop / 0.08);
                        center1 += a * (sung - center1);
                        center2 += a * (center1 - center2);
                    }
                    // round 135w: Speed (unlinked) flattens a share of the in-note deviation (drift, vibrato) on top of
                    // the note transition: all of it at ≤ 1 ms, 20 % at 5 ms, 7 % at the 15 ms default, ~0 above 100 ms
                    // ([EXEC] round 120 §4.1: on the trial Speed only shows below ~5 ms, slightly less vibrato)
                    const auto resid = sung - center2;
                    const auto flatten = linked ? 0.0 : std::min (1.0, 0.001 / std::max (1e-4, speedTau));
                    extraWanted = natural ? (natGain - 1.0) * (1.0 - flatten) * resid - flatten * resid : 0.0;
                    const auto heard = sung + extraWanted; // the sung pitch with the natural-vibrato gain
                    if (newPhrase)
                    {
                        steadyRef = sung;
                        noteAge = 0.0;
                        sinceJump = 1.0;
                    }
                    else if (std::abs (sung - steadyRef) > 0.04)
                    {
                        steadyRef = sung;
                        if (sinceJump >= 0.03 && sinceJump < 0.15) // (estimates < 30 ms apart: the same move)
                            noteAge = 0.0;
                        sinceJump = 0.0;
                    }
                    else
                    {
                        steadyRef += (1.0 - std::exp (-hop / 0.05)) * (sung - steadyRef);
                        noteAge += hop;
                    }
                    if (newPhrase)
                    {
                        target = -1; // a new phrase: no hysteresis toward the last note
                        // ... and no correction left over from the last one: the phrase starts at the sung pitch and
                        // the retune pulls it from there (round 120: the previous phrase's correction used to bend
                        // the first notes of the next one, by up to half a semitone the wrong way)
                        shift = 0.0;
                    }
                    const auto t = keyed ? targetNoteKeys (ignoreVib ? center2 : sung, p.data(), mask, target, hysteresis) // round 152
                                   : anyDir ? targetNote (ignoreVib ? center2 : sung, mask, target, hysteresis, dirs.data())
                                          : vocal::nearestNote (ignoreVib ? center2 : sung, mask, target, hysteresis);
                    // round 135w: Tolerance time — a new note must last that long before the target moves to it
                    if (t != target && holdTime > 0.0 && target >= 0)
                    {
                        if (t == candidate)
                            candidateTime += hop;
                        else
                        {
                            candidate = t;
                            candidateTime = hop;
                        }
                    }
                    if (t != target && (holdTime <= 0.0 || target < 0 || candidateTime >= holdTime))
                    {
                        target = t;
                        noteAge = 0.0;
                        noteTime = 0.0;
                        candidate = -1;
                        candidateTime = 0.0;
                    }
                    else
                    {
                        noteTime += hop;
                        if (t == target)
                            candidate = -1;
                    }
                    if (newPhrase)
                        noteTime = 0.0;
                    wanted = (double) target - heard;
                    if (target >= 0) // round 166c: the target note moved by Scale transpose's steps of the scale
                        scaleOffset = transposeInScaleNote (target, mask, scaleSteps) - target;
                    if (keyed ? noteStatus (p.data(), target, mask) == KeyStatus::bypass : ((bypass >> (((target % 12) + 12) % 12)) & 1u) != 0)
                        wanted = 0.0; // a bypassed note passes as sung
                    if (ranged && (sung < (double) lowNote - 0.5 || sung > (double) highNote + 0.5))
                        wanted = 0.0; // round 147: outside the keyboard's range: passes as sung
                    // flex-tune: a note far from its target is left as sung. [EXEC] Auto-Tune Pro (round 120 §5): the
                    // correction of a note d cents from its target is min (d, (1 − F) / F · (50 − d)) cents, d measured
                    // in hundredths of the gap to the next target on the sung side (a semitone in chromatic)
                    if (flex > 0.0)
                    {
                        const auto up = sung > (double) target;
                        int gap = 1;
                        while (gap < 12 && mask != 0 && ((mask >> (((target + (up ? gap : -gap)) % 12 + 12) % 12)) & 1u) == 0)
                            ++gap;
                        const auto d = std::abs (wanted) * 100.0 / (double) gap;
                        const auto keep = d > 1e-6 ? std::clamp ((1.0 - flex) / flex * (50.0 - d) / d, 0.0, 1.0) : 1.0;
                        wanted *= keep;
                    }
                    if (unvoicedFor > 0.06 && tau <= 0.0)
                        shift = wanted;
                    unvoicedFor = 0.0;
                }
                else
                {
                    psola.setPitch (0.0, false);
                    extraWanted = 0.0;
                    unvoicedFor += hop;
                    if (unvoicedFor > 0.06)
                        sung = -1.0;
                }
            }

            // retune: the applied correction follows the wanted one; humanize slows it on sustained (steady) notes.
            // [EXEC] Auto-Tune Pro (round 120 §5): a note held steady for ~0.6 s is retuned as if the retune speed were
            // longer by the humanize amount (100 % = +100 ms of its speed = +135 ms of LYNX's time constant).
            // Round 138 (REA_rodada138 §3.2): the slow-down sets in between 0.275 and 0.375 s of steadiness (fitted on
            // notes that move after 0.3 s and after 0.8 s, retune 0 … 50 ms, humanize 25 … 100 %; was 0.2 … 0.6 s)
            auto t = tau;
            if (human > 0.0)
            {
                const auto x = std::clamp ((noteAge - 0.275) / 0.1, 0.0, 1.0);
                t += human * 0.135 * x * x * (3.0 - 2.0 * x);
            }
            shift += (t > 0.0 ? 1.0 - std::exp (-dt / t) : 1.0) * (wanted - shift);

            auto semis = shift + trans;
            if (natural || extra != 0.0)
            {
                extra += (1.0 - std::exp (-dt / 0.003)) * (extraWanted - extra);
                semis += extra;
            }
            if (amount < 1.0)
                semis = (semis - trans) * amount + trans; // round 135w: Correction % (the transpose stays whole)
            double vibSemis = 0.0, vibLevel = 0.0; // round 166c: the created vibrato's pitch (st) and level swing
            if (vibShape > 0)
            {
                // round 166c: Variation drifts the rate and the depth at random, a new aim every cycle [model]: up to
                // ±25 % of the rate and ±50 % of the depth at 100 %, glided over about a cycle
                lfoPhase += vibInc * varRate;
                if (lfoPhase >= 1.0)
                {
                    lfoPhase -= std::floor (lfoPhase);
                    varRateAim = 1.0 + 0.25 * vibVar * nextRandom();
                    varDepthAim = 1.0 + 0.5 * vibVar * nextRandom();
                }
                if (vibVar <= 0.0)
                    varRateAim = varDepthAim = 1.0;
                const auto glide = 1.0 - std::exp (-vibInc * 2.0);
                varRate += glide * (varRateAim - varRate);
                varDepth += glide * (varDepthAim - varDepth);
                if (unvoicedFor <= 0.0 && noteTime > vibDelay)
                {
                    const auto env = vibOnset > 0.0 ? std::min (1.0, (noteTime - vibDelay) / vibOnset) : 1.0;
                    const auto w = vibShape == 1 ? std::sin (2.0 * 3.14159265358979323846 * lfoPhase)
                                 : vibShape == 2 ? (lfoPhase < 0.5 ? 1.0 : -1.0)
                                                 : 2.0 * lfoPhase - 1.0;
                    vibSemis = vibDepth * varDepth * env * w;
                    vibLevel = vibAmp * varDepth * env * w;
                    semis += vibSemis;
                }
            }
            semis = std::clamp (semis, -24.0, 24.0);
            const auto ratio = std::exp2 (semis / 12.0);
            // round 165hb: the throat moves the corrected formants; round 166c: Vibrato formant lets them follow the
            // created vibrato's pitch swing by that share (both only while the formants are preserved)
            psola.setShift (ratio, preserve ? fShift * throatRatio * (vibFormant > 0.0 ? std::exp2 (vibFormant * vibSemis / 12.0) : 1.0) : ratio * fShift);
            // round 165j: the normalised overlap-add follows the high shelf's activity (below): only while a voiced
            // sound is being corrected — a voice already on its note and unvoiced sounds keep the plain overlap-add
            // (bit-transparent consonants, no level modulation from marks that jump between noise peaks)
            psola.setNormalise ((float) activity);

            float y[2];
            psola.process (xin, y, stereo ? 2 : 1);

            // the dry signal, delayed like the wet one
            const auto w = (std::size_t) (dryPos & (dryRing - 1));
            const auto rd = (std::size_t) ((dryPos - latency) & (dryRing - 1));
            dry[w] = xin[0];
            dry[w + dryRing] = xin[1];
            const auto d0 = dry[rd], d1 = dry[rd + dryRing];
            ++dryPos;

            // round 148: the overlap-add of shifted grains loses the highs' incoherent part (two-period Hann grains at
            // half overlap keep 0.75 of their power: -1.25 dB) — [EXEC] her takes at retune 0 / 20: 0.5–1.5 dB darker than
            // Auto-Tune Pro above 4 kHz. A first-order shelf (round 148: +1 dB from 5 kHz; round 165j: +1.5 dB from 3 kHz,
            // below) is faded in while a correction of 5 cents or more is being applied to a voiced sound (round 148: about
            // halves the difference: 10 kHz -1.0 -> -0.5 dB, 16 kHz -1.3 -> -0.8 dB) [model]
            {
                // Round 165j ([RUN] REA_rodada165j §4, her 24 live-set takes against Auto-Tune Pro with her states): the
                // corrected voice was still 0.52 dB darker at 2.5-6.3 kHz relative to 300-1000 Hz (Auto-Tune: 0.07); the
                // shelf now starts at 3 kHz with +1.5 dB (tilt -0.13 dB; pitch, bursts and formant distance not worse)
                const double a = std::exp (-2.0 * 3.141592653589793 * 3000.0 / sr);
                // (voiced only: unvoiced sounds pass as plain overlap-add, untouched)
                const double want = unvoicedFor > 0.0 ? 0.0 : std::min (1.0, std::abs (semis - trans) / 0.05);
                activity += (1.0 - std::exp (-1.0 / (0.01 * sr))) * (want - activity);
                constexpr double lift = 0.18850222743702228; // 10^(1.5/20) - 1
                for (int c = 0; c < 2; ++c)
                {
                    hpY[c] = a * (hpY[c] + y[c] - hpX[c]);
                    hpX[c] = y[c];
                    y[c] = (float) (y[c] + activity * lift * hpY[c]);
                }
            }
            if (vibLevel != 0.0) // round 166c: the created vibrato's level swing
                for (auto& v : y)
                    v = (float) (v * (1.0 + vibLevel));
            if (driveOn)
                for (auto& v : y)
                    v = (float) (dK * (side (dG * v + 0.341) - sideB));
            l[i] = (float) (trimGain * (d0 + wet * (y[0] - d0)));
            if (stereo)
                r[i] = (float) (trimGain * (d1 + wet * (y[1] - d1)));
            peakIn = std::max (peakIn, std::max (std::abs (xin[0]), std::abs (xin[1])));
            peakOut = std::max (peakOut, std::max (std::abs (l[i]), stereo ? std::abs (r[i]) : 0.0f));
        }
        // round 152: the MIDI reference tone (the reference's tone generator): the notes held on the MIDI channel as
        // sines at the reference pitch, 5 ms fades, mixed in after the correction
        if (p[referenceTone] >= 0.5f || toneActive)
        {
            const auto amp = p[referenceTone] >= 0.5f ? 0.25 * std::pow (10.0, referenceToneDb (p[referenceToneLevel]) / 20.0) : 0.0;
            const auto fade = 1.0 / (0.005 * sr);
            toneActive = false;
            for (int k = 0; k < 128; ++k)
            {
                const bool on = amp > 0.0 && ch >= 0 && held != nullptr && ch < heldCount && ((held[ch][(std::size_t) (k >> 6)] >> (k & 63)) & 1u);
                auto& g = toneGain[(std::size_t) k];
                if (! on && g <= 0.0) continue;
                const auto inc = aRef * std::exp2 ((k - 69) / 12.0) / sr;
                auto& ph = tonePhase[(std::size_t) k];
                for (int i = 0; i < n; ++i)
                {
                    g = on ? std::min (1.0, g + fade) : std::max (0.0, g - fade);
                    const auto v = (float) (amp * g * std::sin (2.0 * 3.14159265358979323846 * ph));
                    ph += inc;
                    ph -= std::floor (ph);
                    l[i] += v;
                    if (stereo) r[i] += v;
                }
                toneActive = toneActive || g > 0.0;
            }
        }
        meterIn.store (std::min (peakIn, 1.0f), std::memory_order_relaxed);
        meterOut.store (std::min (peakOut, 1.0f), std::memory_order_relaxed);

        meterSung.store ((float) sung, std::memory_order_relaxed);
        meterTarget.store ((float) target, std::memory_order_relaxed);
        meterShift.store ((float) shift, std::memory_order_relaxed);
    }

    // 0: sung note / 127 (0 = none), 1: target note / 127, 2: applied correction (0.5 ± semitones / 4)
    float controlOutput (int o) const noexcept override
    {
        switch (o)
        {
            case 0: return std::max (0.0f, meterSung.load (std::memory_order_relaxed)) / 127.0f;
            case 1: return std::max (0.0f, meterTarget.load (std::memory_order_relaxed)) / 127.0f;
            case 2: return std::clamp (0.5f + meterShift.load (std::memory_order_relaxed) / 4.0f, 0.0f, 1.0f);
            case 3: return meterIn.load (std::memory_order_relaxed);  // round 135w: input peak
            case 4: return meterOut.load (std::memory_order_relaxed); // round 135w: output peak
            case 5: return meterHeld.load (std::memory_order_relaxed); // round 135f: held MIDI pitch classes
            default: return 0.0f;
        }
    }

    int latencySamples() const noexcept override { return latencyNow.load (std::memory_order_relaxed); }

private:
    static constexpr int dryRing = 16384;

    void configure (int rg, bool low) noexcept
    {
        curRange = rg;
        curLow = low;
        detHop = std::max (16, (int) std::lround (128.0 * sr / 44100.0));
        det.prepare (sr, rangeMinHz (rg), rangeMaxHz (rg), detHop);
        det.setHarmonicGuard (true); // round 157: no harmonic errors on strong formants (VocalPitch.cpp, REA_rodada157c)
        latency = std::min (latencyFor (sr, rg, low), dryRing - 1);
        psola.prepare (sr, sr / rangeMinHz (rg), latency);
        psola.setMarkLag (det.windowCentre());
        latencyNow.store (latency, std::memory_order_relaxed);
        reset();
    }

    double sr = 44100.0;
    int curRange = 1, detHop = 128, latency = 1200;
    bool curLow = false;
    vocal::PitchDetector det;
    vocal::Psola psola;
    std::vector<float> dry;
    std::int64_t dryPos = 0;
    double shift = 0.0, wanted = 0.0, noteAge = 0.0, steadyRef = 0.0, sinceJump = 1.0, unvoicedFor = 1e9, sung = -1.0;
    double center1 = 0.0, center2 = 0.0, extraWanted = 0.0, extra = 0.0, noteTime = 0.0, lfoPhase = 0.0;
    int target = -1;
    const NoteMask* held = nullptr;
    int heldCount = 0;
    std::array<double, 128> tonePhase {}, toneGain {}; // round 152: the reference tone
    bool toneActive = false;
    std::atomic<int> latencyNow { 1200 };
    std::atomic<float> meterSung { -1.0f }, meterTarget { -1.0f }, meterShift { 0.0f }, meterHeld { 0.0f };
    std::atomic<float> meterIn { 0.0f }, meterOut { 0.0f };
    double activity = 0.0, hpX[2] {}, hpY[2] {}; // round 148: the high-frequency make-up
    double candidateTime = 0.0; // round 135w: Tolerance time
    int candidate = -1;
    int scaleOffset = 0; // round 166c: transpose in scale — the current target's offset (semitones)
    double varRate = 1.0, varDepth = 1.0, varRateAim = 1.0, varDepthAim = 1.0; // round 166c: vibrato variation
    std::uint32_t rng = 0x2545F491u;
    double nextRandom() noexcept // −1 … 1 (xorshift; real-time safe)
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (double) rng / 2147483647.5 - 1.0;
    }
};
} // namespace

std::unique_ptr<Effect> make()
{
    auto e = std::make_unique<Tune>();
    e->prepare (44100.0);
    return e;
}

// ---- round 150: Send / Receive -------------------------------------------------------------------------------------
const std::array<int, sharedScaleParams>& sharedScaleList() noexcept
{
    static const auto list = [] {
        std::array<int, sharedScaleParams> l {};
        int n = 0;
        l[(std::size_t) n++] = key;
        l[(std::size_t) n++] = scale;
        l[(std::size_t) n++] = moreScales;
        for (int k = 0; k < 12; ++k) l[(std::size_t) n++] = noteC + k;
        for (int k = 0; k < 12; ++k) l[(std::size_t) n++] = bypassC + k;
        for (int k = 0; k < 12; ++k) l[(std::size_t) n++] = noteDirC + k;
        l[(std::size_t) n++] = rangeLow;
        l[(std::size_t) n++] = rangeHigh;
        l[(std::size_t) n++] = groupOctaves; // round 152
        for (int k = 0; k < keyMapParams; ++k) l[(std::size_t) n++] = keyMap1 + k;
        return l;
    }();
    return list;
}

SharedScale captureScale (const float* params) noexcept
{
    SharedScale s;
    const auto& l = sharedScaleList();
    for (std::size_t i = 0; i < l.size(); ++i) s.values[i] = params[l[i]];
    return s;
}

void applyScale (float* params, const SharedScale& s) noexcept
{
    const auto& l = sharedScaleList();
    for (std::size_t i = 0; i < l.size(); ++i) params[l[i]] = std::clamp (s.values[i], 0.0f, 1.0f);
}

namespace
{
std::mutex sharedMutex;
SharedScale sharedScale;
bool sharedValid = false;

template <typename Fn>
void forEachTune (std::vector<EffectSlot>& chain, Fn&& fn)
{
    for (auto& fx : chain)
    {
        if (fx.kind == EffectKind::lynxTune) fn (fx);
        for (auto& br : fx.branches) forEachTune (br.chain, fn);
    }
}
} // namespace

void publishScale (const SharedScale& s) noexcept
{
    std::lock_guard<std::mutex> g (sharedMutex);
    sharedScale = s;
    sharedValid = true;
}

bool publishedScale (SharedScale& out) noexcept
{
    std::lock_guard<std::mutex> g (sharedMutex);
    if (sharedValid) out = sharedScale;
    return sharedValid;
}

void clearPublishedScale() noexcept
{
    std::lock_guard<std::mutex> g (sharedMutex);
    sharedValid = false;
}

int sendScaleToAll (Song& song, int track, int slot)
{
    if (track < 0 || track >= (int) song.mixer.size()) return 0;
    auto& fx = song.mixer[(std::size_t) track].effects;
    if (slot < 0 || slot >= (int) fx.size() || fx[(std::size_t) slot].kind != EffectKind::lynxTune) return 0;
    const auto* source = &fx[(std::size_t) slot];
    const auto s = captureScale (source->params.data());
    publishScale (s);
    int n = 0;
    for (auto& t : song.mixer)
        forEachTune (t.effects, [&] (EffectSlot& e) {
            if (&e == source || e.params[receiveScale] < 0.5f) return;
            applyScale (e.params.data(), s);
            ++n;
        });
    return n;
}
} // namespace lynx::lynxtune
