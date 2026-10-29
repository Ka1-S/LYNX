#pragma once

// FL Studio's Newtone (pitch editor) and Newtime (time warper) as LYNX mixer effects, described in
// docs/analise/REA_rodada73.md (clean-room: layouts and behaviour in our own words; no Image-Line or zplane code or data
// in this repository).
//
// Both plugins are editors: the user loads a sample into them, edits it, and exports the result. On a mixer insert their
// render passes the insert's input through unchanged, and only when the plugin's "slave to host playback" switch is on
// AND FL is playing does the plugin's engine add the edited sample, positioned at the song time (REA_rodada73 §3).
// LYNX reproduces the pass-through exactly and, in slave mode, adds the source sample as it is (the edits themselves —
// zplane's pitch/time engines — are not reproduced; see §5).
//
// Parameters (EffectKind::newtone / EffectKind::newtime):  [0] slave to host playback (0/1).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Effects.h"
#include "core/TimeStretch.h"

namespace lynx
{
struct SampleData;
}

namespace lynx::newtonefx
{
constexpr int paramCount = 1;

// One note ("pitch object", XML "Pitch_N") as Newtone's render uses it (REA_rodada95 §2). Times in source samples.
struct Note
{
    std::int64_t origStart = 0, origEnd = 0; // iOrigStartSamplePos / iOrigEndSamplePos: where it lies in the source
    std::int64_t curStart = 0, curEnd = 0;   // iCurStartSamplePos / iCurEndSamplePos: where it plays (it owns [curStart, curEnd))
    double origPitch = 0.0;                  // fOrigPitch (MIDI note; shown, not used by the render)
    double shift = 0.0;                      // fCurrentPitchOffset + fAdditionalPitchOffset, semitones (fCurPitch is not read)
    double stretch = 1.0;                    // fCurInvStretchFactor: source samples per output sample
    double volume = 1.0, formant = 1.0, drift = 1.0;   // fCurVolume, fCurFormantFactor, fDrift
    double smoothIn = 0.0, smoothOut = 0.0;            // fPitchSmoothInTime / fPitchSmoothOutTime (samples)
    double origSmoothIn = 0.0, origSmoothOut = 0.0;    // fOrigPitchSmooth*Time: the analysed ones (the Transition knob scales them)
    double fadeInTime = 0.0, fadeOutTime = 0.0, fadeInGain = 1.0, fadeOutGain = 1.0;
    double vibPeriod = 0.0, vibAmpStart = 0.0, vibAmpEnd = 0.0, tremAmp = 0.0; // fVibFreq is a period in seconds
    double rampIn = 0.0, rampOut = 0.0;      // fRampIn / fRampOut: semitones at the note's start / end, gone after smoothIn / before smoothOut
    bool enabled = true;

    // the render differs from the source over this note
    bool edited (double globalFormant) const noexcept;
};

// One track of the engine's project (XML "Trk_N")
struct Track
{
    int number = 0;
    bool timeWarp = false;        // "CzplJTimelineWarpSource" (Newtime); otherwise "CzplJPositionablePSOLASource" (Newtone)
    std::string filePath;         // the engine's working copy of the sample (a temporary file of the session that saved it)
    int notes = 0;                // pitch objects ("NumOfPitches")
    std::int64_t frames = 0;      // "OriginalFileLength"
    double selfBpm = 0.0;         // "SelfBPM"
    bool edited = false;          // a note or a global setting differs from the analysis (pitch, time, volume, formant...)
    std::vector<Note> pitches;    // the notes, in order
    double globalFormant = 1.0;   // GlobalFormantShift (the one global the render reads)
    // the editor's global knobs as last set (FL folds them into the notes when it saves; so does LYNX): Center 0..1
    // (pull toward the note's semitone), Variation 0..1 (the notes' drift), Transition 0..2 (× the analysed times)
    double centre = 0.0, variation = 1.0, transition = 1.0;
};

// The plugin's saved state (record 0xD5 of a mixer slot), REA_rodada73 §2
struct State
{
    int version = 0;                 // Newtone 0, Newtime 1
    int editorWidth = 0, editorHeight = 0;
    std::uint32_t flags = 0;         // options (bit 2: slave to host playback, bit 3: ignore host selection, bit 4: ignore own selection...)
    std::string sourcePath;          // record 3: the sample the user loaded, as the host stored it (ANSI); "" = none
    std::string projectXml;          // the engine's project (JUCE XML) from record 2
    std::vector<Track> tracks;

    bool slave() const noexcept { return (flags & 4u) != 0; }
    bool edited() const noexcept;
};

// nullopt = not a Newtone/Newtime state (wrong version, broken records)
std::optional<State> parse (const std::vector<std::uint8_t>& state);

// Parameters for a slot built from FL's state
void toParams (const State& s, EffectParams& params) noexcept;
std::string paramText (int param, float value01);

// The effect: the input passes through; with [0] on, while the host plays, `source` is added from the song time on.
// `source` may be null (sample not found): the effect is then a pass-through.
std::unique_ptr<Effect> make (std::shared_ptr<const SampleData> source);
// The same for a Newtone slot with FL's saved state: with note edits, the edited render of the first track plays
// instead of the plain source (renderEdits; Newtime's warping is still not reproduced).
std::unique_ptr<Effect> make (std::shared_ptr<const SampleData> source, const std::vector<std::uint8_t>& state);

// LYNX's Newtone engine (REA_rodada95 §2-3): renders a track's note edits over its source sample — pitch offsets with
// the note transitions, drift, vibrato, formant factor, volume and fades, timing (moved / stretched notes),
// disabled notes — with LYNX's own pitch analysis (YIN) and TD-PSOLA (core/VocalPitch.h). Unedited notes are copied
// sample for sample. nullptr when nothing differs from the source.
std::shared_ptr<SampleData> renderEdits (const SampleData& source, const Track& track);

// ------------------------------------------------------------------------------------------------------------------
// LYNX's own Newtone / Newtime projects (round 142, issue #5): a slot made in a LYNX project keeps its edits in its
// own format in EffectSlot::state (FL's XML project is only read). The source sample is EffectSlot::samplePath.

// LYNX's note analysis of a sample (YIN pitch track, core/VocalPitch.h): voiced stretches of steady pitch become
// notes (Note.origPitch = their median pitch; nothing edited). Newtone: timeWarp = false.
Track analyseNotes (const SampleData& source);

// ------------------------------------------------------------------------------------------------------------------
// LYNX's Newtone editor tools (round 165g, FL's workflow interpreted: Cut mode / glue, the Center / Variation /
// Transition knobs, snap to scale, score export). `which` = the notes to change; empty = every note.

// Cut mode: note k becomes two at output sample `at` (strictly inside it); the halves keep every edit. `pitchOf`
// gives the source's median pitch over a source range (each half's origPitch); null keeps the note's.
bool splitNote (Track& track, std::size_t k, std::int64_t at, double (*pitchOf) (const void*, std::int64_t, std::int64_t) = nullptr,
                const void* context = nullptr);
// Glue: note k joins the note before it (adjacent in the source and in time, same stretch); the joined note keeps
// the earlier one's edits, its pitch = the length-weighted mean.
bool glueNote (Track& track, std::size_t k);
// Center: every chosen note lands on its semitone by `amount` (0 = as sung, 1 = exactly), keeping the semitone it
// was moved to. Variation: their drift (0 = flat, 1 = as sung). Transition: their smooth times = analysed × factor.
void applyCentre (Track& track, double amount, const std::vector<int>& which = {});
void applyVariation (Track& track, double amount, const std::vector<int>& which = {});
void applyTransition (Track& track, double factor, const std::vector<int>& which = {});
// The nearest allowed note to `pitch` (MIDI) in a scale: `mask` bit n = pitch class (key + n) % 12 allowed;
// 0xFFF = chromatic. Ties go down.
double nearestInScale (double pitch, int key, std::uint16_t mask) noexcept;
// Snap to scale: the chosen notes move to the nearest allowed note (whole semitones, the sung detune removed)
void snapToScale (Track& track, int key, std::uint16_t mask, const std::vector<int>& which = {});

// The track's notes as a score (FL's Create Score / Save as MIDI): enabled voiced notes, key = the semitone they
// sound at, times in seconds
struct ScoreNote
{
    double start = 0.0, length = 0.0;
    int key = 60;
    float velocity = 0.78f;
};
std::vector<ScoreNote> score (const Track& track, double sampleRate);

// Newtone: the track's notes. "LXNT" + version + frames + global formant + the notes.
std::vector<std::uint8_t> writeNative (const Track& track);
std::optional<Track> parseNative (const std::vector<std::uint8_t>& state);
// The track a slot's state describes: LYNX's own, or the first Newtone track of FL's state (nullopt: none)
std::optional<Track> trackOf (const std::vector<std::uint8_t>& state);

// Newtime: warp markers (source sample → output sample), in increasing order on both axes; between two markers the
// source plays stretched (pitch kept) so that it lasts the output distance. Implicit markers at 0 → 0 and at the
// end (the last marker's slope continued). "LXNW" + version + frames + the markers.
struct WarpMarker
{
    std::int64_t source = 0, output = 0;
    bool operator== (const WarpMarker&) const = default;
};
struct WarpMap
{
    std::int64_t frames = 0; // the source's length when the markers were placed
    std::vector<WarpMarker> markers;
    bool operator== (const WarpMap&) const = default;
};
std::vector<std::uint8_t> writeWarp (const WarpMap& map);
std::optional<WarpMap> parseWarp (const std::vector<std::uint8_t>& state);
// The output length of the warped source; and where a source sample lands (and back)
std::int64_t warpedLength (const WarpMap& map, std::int64_t frames);
double warpToOutput (const WarpMap& map, std::int64_t frames, double source);
double warpToSource (const WarpMap& map, std::int64_t frames, double output);
// The warped source (nullptr: no marker moves anything)
std::shared_ptr<SampleData> renderWarp (const SampleData& source, const WarpMap& map);

// What a slot plays in slave mode: the source with its edits (Newtone notes / Newtime warp; LYNX's or FL's state)
std::shared_ptr<const SampleData> renderedSource (std::shared_ptr<const SampleData> source, const std::vector<std::uint8_t>& state);
} // namespace lynx::newtonefx
