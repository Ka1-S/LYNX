#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "core/MissingFiles.h"
#include "core/Song.h"
#include "flp/Flp.h"

namespace lynx
{
// Folders that the variables FL writes into sample paths point to.
struct FlPathRoots
{
    std::string factoryData; // %FLStudioFactoryData%  e.g. C:\Program Files\Image-Line\FL Studio 2026
    std::string data;        // %FLStudioData%         e.g. ...\FL Studio 2026\Data
    std::string userData;    // %FLStudioUserData%     e.g. Documents\Image-Line\FL Studio
    std::string home;        // this user's profile (C:\Users\name): re-roots paths saved on another account/PC
};

std::string resolveFlPath (const std::string& flPath, const FlPathRoots& roots);
// `path` with its letter case as on disk (POSIX file systems), or unchanged; used for FL's Windows-spelled paths.
std::string existingPathIgnoringCase (const std::string& path);

// What the import did with each FL thing that isn't "notes and samples": so the user knows why
// something sounds different.
struct FlpImportReport
{
    std::map<std::string, int> converted;    // generators truly converted (3x Osc, FPC, DirectWave...)
    std::map<std::string, int> approximated; // approximated by the internal synth (MiniSynth, DX10...)
    std::map<std::string, int> unsupported;  // no equivalent: play with a synth preset chosen by name
    std::map<std::string, int> droppedEffects; // effects with no equivalent (removed from the mixer)
    std::map<std::string, int> partialEffects; // effects kept but approximated or partly reproduced (still in the mixer)
    int missingSamples = 0;
    int relocatedSamples = 0; // found on this PC at another place than the project saved (core/FlPluginDb.h)
    std::vector<MissingFile> missingFiles; // the samples not found, with the channels/effects that use them (core/MissingFiles.h)
    std::vector<std::pair<std::string, std::string>> relocatedFiles; // saved path → the copy found elsewhere on this PC
};

struct FlpImportOptions
{
    FlPathRoots roots;
    std::string projectFolder; // the .flp's folder: samples saved elsewhere are also looked for next to it
    std::function<bool (const std::string& path)> fileExists; // without this, no sample is used

    // Optional (Fruity Slicer/Slicex slices): bytes of a file, a sample's length in
    // frames (0 = unknown) and saving of a WAV embedded in the project (returns the path).
    std::function<std::vector<std::uint8_t> (const std::string& path)> readFile;
    std::function<std::int64_t (const std::string& path)> sampleFrames;
    // Optional: a sample's length in seconds (0 = unknown) — FL's time-stretch MUL without a TIME stretches it by a factor
    std::function<double (const std::string& path)> sampleSeconds;
    std::function<std::string (const std::vector<std::uint8_t>& wav, const std::string& name)> saveEmbedded;

    FlpImportReport* report = nullptr; // optional

    // Samples the user located (the "Missing files" window): saved path (FL's variables resolved) → file to use instead.
    std::map<std::string, std::string> relocations;

    bool threeOscAsSynth = false; // 3x Osc → the built-in synth conversion instead of FL's engine (the synth's preset menu)
};

// Converts an FL Studio project to our model. Sampler channels whose
// file exists become samplers; the rest use an internal instrument chosen
// by name. Patterns are rescaled to our PPQ. Plugins not yet.
Song importFlp (const flp::Project& project, const FlpImportOptions& options = {});

// FL Layer channel options (.flp 0x90, REA_rodada164a §1) into LYNX's Layer settings
void applyFlLayerFlags (int flags, LayerSettings& layer);
InstrumentKind guessInstrument (const std::string& channelName, flp::ChannelType type);

// Internal synth oscillators from the 3x Osc state (0xD5), versions 10 to 14
// (docs/analise/REA_rodada4.md §1-2). Keeps ADSR and filter from `base`. False if the blob is unusable.
bool synthFrom3xOsc (const std::vector<std::uint8_t>& state, SynthSettings& base);

// FL channel filter (Mod X/Y 0..256, type 0..7 from 0xDB) as cutoff in Hz and resonance 0..1
// of the synth/sampler filter (REA_rodada4 §3); `mode` 0 low-pass, 1 band-pass, 2 high-pass, 3 band-reject.
void synthFilterFromChannel (int modX, int modY, int type, float& cutoffHz, float& resonance, int& mode);

// Octaves the FL cutoff envelope (amount ±128 in Mod X units) shifts from the channel's Mod X.
float filterEnvelopeOctaves (int modX, int type, int amount);

// A pad of an FL drum sampler (FPC and similar) that becomes its own channel.
struct DrumPad
{
    std::string name;
    int key = 60;                   // key that triggers the pad in the original channel
    std::vector<SampleZone> layers; // velocity layers (play at the sample's pitch)
    float gain = 1.0f;              // pad linear gain (multiplies the channel's)
    float pan = 0.0f, tune = 0.0f;  // pan −1..1; tuning in semitones
    int cutGroup = 0, cutBy = 0;    // playing this pad cuts pads with cutBy == cutGroup (0 = none)
    int mixerOffset = 0;            // mixer track relative to the channel's
};

// Replaces channel `channel` with one sampler channel per pad (right after it, on the same mixer insert)
// and moves notes by key: a pad's note goes to its channel, at key 60. Notes on keys without a
// pad are dropped. Channel indices of patterns, automations and clips are adjusted.
void splitDrumChannel (Song& song, int channel, const std::vector<DrumPad>& pads);

// FL's Time stretching panel (0xD7) on a sample channel or an audio clip, as FL plays it (REA_rodada164e-clippitch):
// every mode but Resample (0) keeps the length and adds the PITCH knob (cents); MUL without a TIME stretches the
// sample (sampleSeconds long) by 2^(mul/10000), turned into beats at `bpm`.
void applyFlStretchPanel (SamplerSettings& s, int mode, int pitchCents, int mul, float timeTicks, double sampleSeconds, double bpm);

// FL channel/plugin preset (.fst) of a synthesizer the import converts (3x Osc, Sytrus,
// Harmor, GMS, Harmless...) or native GMS preset (.gmsynth), as internal synth settings.
// nullopt if the generator isn't one of these or the file couldn't be read.
std::optional<SynthSettings> synthFromFlPreset (const std::vector<std::uint8_t>& fileBytes);
bool isFlSynthPresetPlugin (const std::string& pluginName);

// Internal effect similar to a native FL effect (by the plugin's internal name).
std::optional<EffectKind> effectForFlPlugin (const std::string& name);

// Our target for an FL controlID (channel/mixer). `channelMap` = FL channel -> ours.
AutomationTarget automationTargetFor (flp::ControlId id, const std::map<int, int>& channelMap, int mixerTracks);
} // namespace lynx
