#pragma once

// LYNX Tune: LYNX's own real-time vocal pitch corrector (docs/analise/REA_rodada95.md §4), in the role of the
// automatic pitch correctors used on almost every modern vocal (the "Auto-Tune" kind of effect). It is an original
// design written only from published DSP — YIN pitch detection and TD-PSOLA pitch shifting (core/VocalPitch.h) —
// and does not model or reproduce any product: no third-party plugin was opened, run or analysed to make it.
//
// Chain: the input's pitch is tracked (YIN on the mid signal); each voiced estimate picks a target note — the
// nearest note of the key/scale (with hysteresis), or the nearest pitch class held on a MIDI source channel — and
// the wanted correction (target − sung pitch, in semitones) is followed with the retune time constant (0 ms =
// instant, the hard "robot" effect; slower keeps the singer's own slides and vibrato). Humanize lengthens the retune
// time on sustained notes; flex-tune leaves notes far from their target as sung. TD-PSOLA shifts the voice by the correction (+ transpose) with the formants preserved, or
// moved with the pitch when formant preserve is off; formant shift moves them on its own.
//
// Latency: a constant number of samples set by the voice range and the low-latency switch (two longest periods, or
// 0.5 in low-latency mode), reported to PDC.

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <string>

namespace lynx
{
struct EffectInfo;
class Effect;
struct Song;

namespace lynxtune
{
enum Param : int
{
    key,             // 0..11 (C … B)
    scale,           // vocal::Scale
    retune,          // retune speed 0…400 ms (400·v²)
    humanize,        // 0…100 %
    formantPreserve, // on / off
    formantShift,    // −7…+7 semitones
    transpose,       // −12…+12 semitones
    reference,       // A4 = 430…450 Hz
    range,           // voice range: soprano, alto / tenor, low male, instrument
    lowLatency,      // on / off
    midiChannel,     // 0 = off; k = take the target notes from Channel Rack channel k (1-based), v = k / 128
    mix,             // dry / wet
    tracking,        // detector strictness (YIN threshold 0.05 … 0.30)
    noteC,           // 12 note switches (C … B): a note that is off is never a target
    flexTune = noteC + 12, // 0…100 %: notes far from their target are left as sung (round 120)
    // round 128 (parity with the auto-mode corrector it stands in for; all neutral at their defaults)
    naturalVibrato,          // −12…+12 dB: the vibrato already in the voice, scaled (0 dB = untouched)
    bypassC,                 // 12 note switches (C … B): a bypassed note is still a target, but passes uncorrected
    ignoreVibrato = bypassC + 12, // on / off: the target is chosen from the pitch with its vibrato smoothed away
    vibratoShape,            // created vibrato: none, sine, square, sawtooth
    vibratoRate,             // 0.1…10 Hz
    vibratoDepth,            // 0…100 cents (peak)
    vibratoDelay,            // onset delay 0…1500 ms after a note starts
    vibratoOnset,            // onset rate (fade-in) 0…1500 ms
    moreScales,              // 0 = the Scale parameter; else whole tone / diminished
    // round 135f (Auto-Tune Pro parity, appended; off by default)
    learnMidi,               // on / off: the notes held on the MIDI channel teach the scale (the editor turns them into
                             // the custom scale's note switches) instead of being the targets
    classicMode,             // on / off: the classic DSP — the same retune, the short-latency path (as low latency)
    // ---- round 135w: the real-time corrector's controls (the Waves Tune Real-Time role, docs/PARITY.md), appended;
    // every default is the old behaviour
    speedLink,               // on (default) = the in-note deviations follow the retune time, as before; off = Speed
    speed,                   // 0.1 … 800 ms (half travel = 50 ms): how fast the in-note deviations (drift, vibrato) go
    toleranceCents,          // 0 (off) … 40 c added to the half-way point before the target note changes
    toleranceTime,           // 0 (off) … 300 ms a new target note must last before the correction moves to it
    keepVibrato,             // on (default) = the vibrato is kept at Vibrato amount; off = flattened
    vibratoAmount,           // 0 … 200 % (default 100 %): the vibrato kept (multiplies Natural vibrato's gain)
    correction,              // 0 … 100 % (default 100 %): how much of the correction is applied (scales the shift)
    // round 136 (appended, off by default): the drive stage of the voice-shifter role it plays (measured black-box on
    // the reference, REA_rodada136 §3), a wider formant range and an output trim
    driveStage,              // on / off
    drive,                   // 0 … 10
    wideFormant,             // on: Formant shift reaches ±12 semitones
    outputTrim,              // −24 … +24 dB
    // round 147 (appended; the Waves Tune Real-Time role's keyboard, REA_rodada147b; defaults = the old behaviour):
    // what an illegal (removed) note does — 0 the nearest legal note (as before), 0.5 the next legal note up, 1 the next
    // one down — and the keyboard's range: sung notes outside [low, high] pass uncorrected
    noteDirC,                // 12 values (C … B)
    rangeLow = noteDirC + 12, // MIDI note = round (127 v); default 0
    rangeHigh,               // default 127
    // round 150 (appended): the Waves Tune Real-Time role's Receive — on (default) = this instance takes the scales
    // another instance sends (Send scale to all); off = its scale stays as it is
    receiveScale,
    // round 152 (appended): the Waves Tune Real-Time role's Group Octaves — on (default) = a key edits its note name in
    // every octave, as before; off = a key's own status (the key map below) overrides its note name's in that octave
    groupOctaves,
    keyMap1,                       // 12 packed values: MIDI notes 24 … 119, 8 keys per value (base 6), 0 = follow
    // round 152 (appended): the MIDI reference tone — a sine at each note held on the MIDI channel, mixed into the
    // output at −18 … +18 dB (0 dB = −12 dBFS peak per note [model]); off by default
    referenceTone = keyMap1 + 12,
    referenceToneLevel,
    // round 165hb / 166c (appended; neutral defaults = the old sound). The auto-mode corrector's remaining controls
    // (Auto-Tune Pro parity, REA_rodada165hb, REA_rodada166c):
    throatLength,            // 50 … 180 % (default 100 %): the modelled throat — longer lowers the formants (formant on only)
    vibratoAmplitude,        // created vibrato: 0 … 30 (the reference's units), the level swing that goes with it
    vibratoFormant,          // created vibrato: 0 … 100 %: how far the formants follow its pitch swing (formant on only)
    vibratoVariation,        // created vibrato: 0 … 100 %: random drift of its rate and depth
    transposeInScale,        // on: Scale transpose moves the target notes by steps of the scale (Transpose is ignored)
    scaleTranspose,          // −7 … +7 steps of the scale (default 0)
    count
};
// round 165hb: the throat length (%) of a value and back, and its formant shift in semitones (measured law)
double throatPercent (float v) noexcept;
float throatValue (double percent) noexcept;
double throatSemitones (double percent) noexcept;
double vibratoAmplitudeOf (float v) noexcept;   // 0 … 30
double vibratoAmplitudeDepth (double amount) noexcept; // the created vibrato's level swing (± fraction of the level)
int scaleTransposeSteps (float v) noexcept;      // −7 … +7
float scaleTransposeValue (int steps) noexcept;
// the target note moved by `steps` steps of the scale `mask` (the note itself counts as a step when it is in the scale;
// a note outside the scale moves from the nearest scale note below it)
int transposeInScaleNote (int note, std::uint16_t mask, int steps) noexcept;
double referenceToneDb (float v) noexcept; // −18 … +18 dB
// round 152: the key map of Group Octaves off
enum class KeyStatus { follow, legal, removed, removedUp, removedDown, bypass };
constexpr int keyStatuses = 6;
constexpr int keyMapFirst = 24, keyMapPerParam = 8, keyMapParams = 12, keyMapKeys = keyMapPerParam * keyMapParams;
bool groupOctavesOn (const float* params) noexcept;
KeyStatus keyOverride (const float* params, int note) noexcept;       // follow outside 24 … 119
void setKeyOverride (float* params, int note, KeyStatus s) noexcept;  // ignored outside 24 … 119
bool anyKeyOverride (const float* params) noexcept;
void clearKeyOverrides (float* params) noexcept;
// a note's status in effect: its key's own (Group Octaves off) or its note name's (legal = a target in the scale mask
// `mask`, the note switches, bypasses and directions) — never follow
KeyStatus noteStatus (const float* params, int note, std::uint16_t mask) noexcept;
// the target note with the key map in effect (Group Octaves off): the nearest allowed key within an octave with the
// hysteresis toward `previous`, then an illegal sung key's direction; −1 never (all keys illegal: the sung note)
int targetNoteKeys (double sung, const float* params, std::uint16_t mask, int previous, double hysteresis) noexcept;
// the next status of a REMOVE click on a key: legal → removed → removed up → removed down → legal (the reference's)
KeyStatus nextRemoveStatus (KeyStatus current) noexcept;
enum class NoteDir { nearest, up, down };
NoteDir noteDir (float v) noexcept;
int rangeNote (float v) noexcept;       // 0 … 127
float rangeNoteValue (int note) noexcept;
// the keyboard's range presets (LYNX's own spans, classical voice ranges with a margin): low / high MIDI notes
constexpr int rangePresets = 7;          // Bass, Baritone, Tenor, Alto, Mezzo, Soprano, Generic (the whole keyboard)
const char* rangePresetName (int k) noexcept;
std::pair<int, int> rangePresetNotes (int k) noexcept;
// The target note (a MIDI note number) for a sung pitch (notes): the nearest legal one with the hysteresis toward
// `previous`, then an illegal note's direction (noteDirs: the 12 directions); −1 = none legal
int targetNote (double sung, std::uint16_t legal, int previous, double hysteresis, const NoteDir* noteDirs) noexcept;

constexpr int vibratoShapes = 4;

constexpr int rangeCount = 4;
const char* rangeName (int r) noexcept;
double rangeMinHz (int r) noexcept;
double rangeMaxHz (int r) noexcept;
int rangeIndex (float v) noexcept;
int scaleIndex (float v) noexcept;
int keyIndex (float v) noexcept;
double retuneMs (float v) noexcept;
double formantSemitones (float v) noexcept;
double formantSemitones (float v, bool wide) noexcept; // round 136: ±12 when wide
int transposeSemitones (float v) noexcept;
double referenceHz (float v) noexcept;
int midiChannelIndex (float v) noexcept; // −1 = off, else the channel index (0-based)
double naturalVibratoDb (float v) noexcept;
int vibratoShapeIndex (float v) noexcept;
double vibratoRateHz (float v) noexcept;
double vibratoDepthCents (float v) noexcept;
double vibratoOnsetMs (float v) noexcept; // delay and onset rate share one law
double speedMs (float v) noexcept;          // round 135w: 0.1 … 800 ms, piecewise (0.5 = 50 ms)
float speedValue (double ms) noexcept;
double toleranceCentsOf (float v) noexcept; // 0 … 40 c
double toleranceTimeMs (float v) noexcept;  // 0 … 300 ms
double vibratoAmountOf (float v) noexcept;  // 0 … 2
// The scale in effect: the Scale parameter, or one of the extra scales chosen by More scales (round 128)
int effectiveScale (float scaleValue, float moreScalesValue) noexcept;

// controlOutput (5): the pitch classes held on the MIDI channel, a 12-bit mask / 4096 (round 135, LEARN)
// Latency (samples) for a sample rate and the range / low-latency parameters
int latencyFor (double sampleRate, int range, bool lowLatency) noexcept;

// round 150: Send / Receive between instances (the reference's "Send Scale to All" / "Receive"). The shared scale is
// the key, the scale, More scales, the twelve note switches, bypasses and directions, and the keyboard's range.
constexpr int sharedScaleParams = 3 + 12 * 3 + 2 + 1 + 12; // round 152: + Group Octaves and the key map
const std::array<int, sharedScaleParams>& sharedScaleList() noexcept;
struct SharedScale
{
    std::array<float, sharedScaleParams> values {};
    bool operator== (const SharedScale&) const = default;
};
SharedScale captureScale (const float* params) noexcept;        // a slot's params (maxEffectParams values)
void applyScale (float* params, const SharedScale& s) noexcept;
// the process-wide slot: the scale sent last (by any instance in this LYNX); false = nothing sent yet
void publishScale (const SharedScale& s) noexcept;
bool publishedScale (SharedScale& out) noexcept;
void clearPublishedScale() noexcept; // tests
// Send: publish the scale of the LYNX Tune at (track, slot) and copy it to every other LYNX Tune in the song whose
// Receive is on (mixer inserts, including effects inside containers). Returns the number of instances changed.
int sendScaleToAll (Song& song, int track, int slot);

const EffectInfo& info();
std::string paramText (int param, float value01);
std::unique_ptr<Effect> make();
} // namespace lynxtune
} // namespace lynx
