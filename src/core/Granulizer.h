#pragma once

// FL Studio's Fruity Granulizer rebuilt as the engine decoded in docs/analise/REA_rodada67.md (clean-room; a port of our
// own round-67 reference model r67/py/granmodel.py, checked against the installation's DLL run unmodified).
//
// Per voice (§2): grains are started on a grid of "grain spacing" samples; each grain reads the channel's sample from
// the voice's position with the library's sample reader (24-point windowed sinc, FL's default quality; a plain copy at
// unity speed), at the speed given by Final.Pitch and the sample's rate, under an attack / hold / release envelope
// (the very first grain starts at full level), alternate grains panned left/right by "stereo separation" through the
// x87 pan law. After each grain the voice's position advances by "wave spacing" (negative: backwards), with optional
// randomness (Delphi's Random), position wobble (effect depth/speed), freeze, sample start and loop. Then Image-Line's
// plugin voice processor (core/FlVoiceProcessor.h), as 3x Osc keeps it — including the 10 ms end fade on release.
//
// The channel keeps the plugin state in ChannelDef::pluginState normalised to version 7 (12 parameters, slice mode,
// key mode, sync byte, the 364-byte channel-settings snapshot); ChannelDef::samplePath is the channel's sample (FL hands
// it to the plugin through the host's GetSampleInfo); ChannelDef::modelParams[0] (optional) is the project's PPQ.
//
// Slices (REA_rodada165g-flgran165): with a transients source (the WAV's cue points / regions, or FL's auto slicing,
// core/FlAutoSlicer) grains line up with the slice starts and the first grain of each slice holds for "transient hold";
// tempo lock plays the sample once per its beat count (the WAV's ACID beats, else the channel's stretch TIME in beats,
// as FL hands the plugin with Dispatcher 0x13); the keys can move the start in 16th notes or to the k-th slice. The
// sample reader uses FL's render quality (16 points per side) while LYNX exports and the plugin's 12 live.
//
// Not modelled: rcpps (a CPU-specific approximate reciprocal in the sinc kernel) is replaced by the exact reciprocal;
// the sines of the kernel use the C library instead of IPP's ippsSin_64f_A50 (both: about −130 dB, isolated samples
// on the reader's edge path near −90 dB); one random seed per instrument (FL: one per process); the sample's own loop
// points (the whole sample, as FL hands it).

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/Instruments.h"

namespace lynx
{
// true when the exact engine can read this Fruity Granulizer state (any version the plugin loads: 1 … 7)
bool granulizerStateUsable (const std::vector<std::uint8_t>& state);

// The state normalised to version 7 (428 bytes) with `snapshot` (364 bytes) as its channel settings; when `snapshot` is
// empty a version-7 state keeps its own and older ones get the plugin's defaults. Empty if the state is unusable.
std::vector<std::uint8_t> granulizerNormalisedState (const std::vector<std::uint8_t>& state, const std::vector<std::uint8_t>& snapshot);

// Parameters of a normalised state: 0 grain attack, 1 grain hold, 2 grain spacing, 3 wave spacing, 4 effect depth,
// 5 effect speed, 6 randomness, 7 stereo separation, 8 loop, 9 freeze (hold), 10 sample start, 11 transient hold
std::optional<int> granulizerParam (const std::vector<std::uint8_t>& state, int index);
bool granulizerSetParam (std::vector<std::uint8_t>& state, int index, int value); // clamped to the knob's range
int granulizerParamCount() noexcept;

// The editor's modes of a normalised state: 0 = transients source (slice mode: 0 off, 1 the sample's regions / cue
// points, 2 FL's auto slicing; "Transient" keys turn 0 into 2 as FL's editor does), 1 = key to (0 pitch, 1 percent:
// the start across the sample, 2 step: 16th notes, 3 transient: the slices), 2 = tempo locked (0/1)
std::optional<int> granulizerMode (const std::vector<std::uint8_t>& state, int which);
bool granulizerSetMode (std::vector<std::uint8_t>& state, int which, int value); // clamped to the mode's range
std::pair<int, int> granulizerParamRange (int index) noexcept; // the editor knobs' ranges (EXEC)

// The instrument; `sample` is the channel's sample (silent without one, as FL: TriggerVoice refuses the note)
std::unique_ptr<Instrument> makeGranulizerInstrument (std::shared_ptr<const SampleData> sample);

namespace granulizer
{
struct RefNote
{
    int start = 0, key = 60;
    double finalVol = 1.0, finalPan = 0.0, pitchOffset = 0.0; // Final.Vol/Pan, Final.Pitch − 100·(key − 60)
    int release = -1;                                         // sample of the note-off (−1: held)
};

struct RefOptions
{
    int block = 512;        // host block (split at every event and tick)
    int maxPoly = 0;        // 0 = unlimited
    int tick = 0;           // > 0: NewTick at every multiple of `tick` samples
    int ppq = 96;
    double tempo = 140.0;
    std::uint32_t seed = 0; // the DLL's RandSeed when the first note starts
    float beats = 0.0f;     // the host's beat count (Dispatcher 0x13): tempo sync and key mode 2
    std::vector<std::int64_t> slices; // slice starts (ascending) for slice modes 1 and 2
    int points = 16;        // interpolation points per side (FL renders with 16; the plugin's own default is 12)
};

// Renders `notes` from a normalised `state` and `sample` like granmodel.run(). Interleaved stereo float samples
// (2·length); empty if the state is unusable.
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const SampleData& sample, const std::vector<RefNote>& notes,
                                    double sampleRate, int length, const RefOptions& options = {});
} // namespace granulizer
} // namespace lynx
