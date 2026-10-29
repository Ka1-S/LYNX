#pragma once

// Song data model. Does not depend on JUCE.
// The UI edits a copy and publishes an immutable snapshot to the audio thread
// (see SnapshotExchange.h), so nothing here needs to be thread-safe.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <map>
#include <vector>
#include <utility>

namespace lynx
{
constexpr int ppq = 960;              // ticks per quarter note
constexpr int stepsPerBeat = 4;       // step sequencer in sixteenth notes
constexpr int ticksPerStep = ppq / stepsPerBeat;
constexpr int maxMixerTracks = 128;   // 0 = Master, 1..127 = inserts (FL has 125 + master)
constexpr float unityFader = 0.8f;    // fader position that gives 0 dB, as in FL

// FL mixer fader curve: g = (11^v - 1) / 10 with v = position * 1.25 (0 dB at 0.8;
// +5.6 dB at maximum). docs/analise/REA_rodada2.md §2.2.
float faderGain (float position) noexcept;
// Its inverse: the fader position (0..1) that gives `gain` (0 for gain <= 0, 1 for maxFaderGain and above).
float faderPosition (float gain) noexcept;
// Gain at the top of the fader: +5.6 dB. FL's insert-to-insert and insert-to-Master route levels use the same curve
// and range (0..16000, 12800 = 0 dB; REA_rodada107 §3), so a send can reach it too.
constexpr float maxFaderGain = 1.9031f;

// Channel volume with the same curve, but 1.0 = 0 dB (FL's default, 0.78, gives -5.2 dB).
// docs/analise/REA_rodada3.md §2.
float channelGain (float volume) noexcept;

enum class InstrumentKind : std::uint8_t
{
    kick,
    snare,
    hat,
    clap,
    synth,
    sampler,
    audioClip, // sampler without transposition that stops at the end of the clip (audio clip channel, as in FL)
    plugin,    // external instrument (VST3), created by the app
    controller, // makes no sound: notes trigger envelopes that modulate other parameters (FL's Envelope Controller)
    sakura,     // physical string model (FL's Sakura engine, REA_rodada28 §1); parameters in ChannelDef::modelParams
    drumpad,    // physical drum model (FL's Drumpad engine, REA_rodada28 §2); parameters in ChannelDef::modelParams
    boobass,    // FL's BooBass engine (REA_rodada29 §4); multisample read from the user's DLL (samplePath), 3 knobs
    drumsynth,  // FL's Fruity DrumSynth Live (REA_rodada30 §1): 120 patches in modelParams, recorded waves from the user's DLL
    harmless,   // FL's Harmless additive engine (REA_rodada32): raw state in modelParams (layout in core/Harmless.h)
    sytrus,     // FL's Sytrus engine (REA_rodada31): the plugin's raw saved state in ChannelDef::pluginState
    harmor,     // FL's Harmor additive engine (REA_rodada34): the plugin's raw saved state in ChannelDef::pluginState
    gms,        // FL's GMS engine (REA_rodada39 §1): raw saved state in pluginState, wave folder in samplePath
    morphine,   // FL's Morphine engine (REA_rodada39 §2): raw saved state in pluginState, noise sample in samplePath
    ogun,       // FL's Ogun inverse-FFT engine (REA_rodada37): the plugin's raw saved state in ChannelDef::pluginState
    minisynth,  // FL's MiniSynth engine (REA_rodada41): raw saved state in pluginState, path of oscdata.dat in samplePath
    dx10,       // FL's Fruity DX10 FM engine (REA_rodada42): the plugin's raw saved state in ChannelDef::pluginState
    kepler,     // FL's Kepler engine (REA_rodada46): raw saved state in pluginState, the user's engine DLL in samplePath
    poizone,    // FL's PoiZone engine (REA_rodada44): the plugin's raw saved state in ChannelDef::pluginState
    fruitkick,  // FL's Fruit Kick (REA_rodada43 §1): raw saved state (with its envelope snapshot) in pluginState
    bassdrum,   // FL's BassDrum (REA_rodada43 §2): raw saved state in pluginState, the BassDrum folder in samplePath
    toxic,      // FL's Toxic Biohazard engine (REA_rodada45): raw saved state in pluginState, the user's DLL in samplePath
    threeosc,   // FL's 3x Osc engine (REA_rodada47): normalised state in pluginState, custom-wave sample in samplePath
    autogun,    // FL's Autogun (REA_rodada57 §1): Ogun's engine on the raw saved state (pluginState), SG.dat path in samplePath
    transistorbass, // FL's Transistor Bass (REA_rodada57 §2): raw saved state in pluginState, the user's engine_x64.dll in samplePath
    simsynth,   // FL's SimSynth engine (REA_rodada64 §1–4): the plugin's raw saved state (564 bytes) in ChannelDef::pluginState
    wasp,       // FL 2026's Wasp / Wasp XT (REA_rodada64 §5): a 64-bit placeholder that keeps its state and makes no sound
    plucked,    // FL's Plucked! (REA_rodada65 §1): the plugin's raw saved state (5 knobs) in ChannelDef::pluginState
    beepmap,    // FL's BeepMap (REA_rodada65 §2): the plugin's raw saved state (settings + bitmap) in ChannelDef::pluginState
    sawer,      // FL's Sawer (REA_rodada65 §3): raw saved state in pluginState, the user's Sawer\data.saw in samplePath
    flkeys,     // FL Keys (REA_rodada69 §8): raw saved state in pluginState, wavetable files in zones, .wti table in modelParams
    granulizer, // FL's Fruity Granulizer engine (REA_rodada67): normalised state in pluginState, the channel's sample in samplePath
    drumaxx,    // FL's Drumaxx engine (REA_rodada67 §6–§9): the plugin's raw saved state ("SSLF" stream) in ChannelDef::pluginState
    slayer,     // FL Slayer (REA_rodada68 §2–6): raw saved state in pluginState, the user's Slayer_Engine.dll in samplePath
    fruitydance, // Fruity Dance (REA_rodada68 §8): a visual plugin (dancing character) that keeps its state and makes no sound
    directwave, // DirectWave (REA_rodada72): raw saved state in pluginState, one zone per zone of the played program, sample formats in modelParams
    midiOut,    // FL's MIDI Out (REA_rodada16 §1, REA_rodada133 §4): no sound; its notes and MIDI controls go to the plugins whose
                // MIDI input port is its port (PluginRef::midiInPort). FL's state (channel @4, port @24) in pluginState
    patcher,    // FL's Patcher as a generator (REA_rodada141): a graph of instruments and effects in ChannelDef::patcher
    soundfont,  // LYNX Soundfont (REA_rodada158, core/SoundFont.h): the .sf2 / .sf3 in samplePath, settings in modelParams
    layer,      // FL's Layer channel (REA_rodada164a, core/Layer.h): no sound; its notes play its children (ChannelDef::layer)
    dashboard,  // FL's Dashboard (REA_rodada164a, core/ControlSurface.h): no sound; a panel of controls in patcher.surface
    voltage,    // FL's Voltage Controller (REA_rodada164a, core/VoltageController.h): its notes as control voltages; FL's
                // state in pluginState
    vfx,        // FL's VFX note processors (round 164b, core/Vfx.h): kind, output port and FL's state in pluginState; no sound
    videoplayer // FL's Fruity Video Player (round 165g, core/VideoPlayer.h): FL's state (with the video's path) in
                // pluginState; plays the video's sound in sync with the song, the video is shown by its editor
};

// External plugin: format identifier (the app knows how to find the file) + saved state.
// Third-party plugin coming from an FL project ("Fruity Wrapper"), before finding the same
// installed plugin (docs/analise/REA_rodada5.md). Not saved in .lynx: the app resolves it on import.
struct ForeignPlugin
{
    enum class Format : std::uint8_t { none, vst2, vst3, clap, other };

    Format format = Format::none;
    std::uint32_t vst2Id = 0;                 // AEffect::uniqueID
    std::array<std::uint8_t, 16> classId {};  // VST3 class TUID, as laid out in memory
    bool hasClassId = false;
    std::string clapId, name, path;           // path: binary on the machine that saved it
    std::vector<std::uint8_t> state;          // raw 0x35 record (format depends on `format`)
    int midiInPort = -1, midiOutPort = -1;     // wrapper MIDI ports (record 0x01; −1 = none)
    // wrapper options (record 0x02, REA round 134): processing/troubleshooting flags and the fixed-buffer options
    std::uint32_t wrapperVersion = 0, wrapperFlags = 0, fixedOptions = 0;
    bool hasWrapperOptions = false;
    // round 151 [DATA: FL 2026's options toggled one by one, saved and compared]: the second flags word of record 0x02
    // (+21) and the pitch bend range of record 0x01 (+8, semitones; −1 = not saved)
    std::uint32_t wrapperFlags2 = 0;
    bool hasWrapperFlags2 = false;
    int bendRange = -1;

    bool operator== (const ForeignPlugin&) const = default;
};

struct PluginRef
{
    std::string id;    // unique plugin identifier (JUCE's format-name-hash)
    std::string name;  // to display even when the plugin is not installed
    std::string state; // plugin state in base64 (what the plugin returns when saving)
    ForeignPlugin foreign; // FL import not yet resolved (format == none: nothing pending)
    // FL wrapper processing options (REA_rodada118 Part B), saved with the project:
    bool fixedBuffers = false;    // "Use fixed size buffers": constant 2 ms blocks (flFixedBlockSize), + that latency
    bool separateProcess = false; // "Make bridged": this plugin runs in its own process even with the global option off
    bool maxSizeBuffers = false;  // with fixedBuffers: "Process maximum size buffers" (the device buffer, not 2 ms)
    bool threaded = true;         // "Allow threaded processing" (REA round 134): off = always on the audio thread itself
    // "Allow smart disable" (REA round 139, wrapper flag 0x20000000): off = this plugin is never smart-disabled, whatever
    // its slot's "Smart disable" says (for plugins that misbehave when they are not called)
    bool allowSmartDisable = true;
    // with fixedBuffers: "Use maximum buffer size from host" (fixed-buffer options bit 1) — the plugin is prepared for
    // the host's maximum block (4096) instead of the fixed block it is always called with
    bool hostMaxBuffer = false;
    // The wrapper's "MIDI input port" (0…255; −1 = none): the notes / controls of the MIDI Out channels on that port
    // reach this plugin (FL: Plugin wrapper settings › MIDI › Input port)
    int midiInPort = -1;
    // FL's wrapper settings, round 149 (Fruity Wrapper's DFM, REA_rodada149 §C):
    // "Output port" (0…255; −1 = none): the notes the plugin sends out play the plugins whose input port it is
    int midiOutPort = -1;
    // "Notify about rendering mode" (wrapper flag 0x4, on by default): the plugin is told when an export renders
    bool notifyRender = true;
    // the Info section's Latency: the delay compensation uses this (ms) instead of what the plugin reports (−1)
    float latencyOverrideMs = -1.0f;
    // FL's wrapper settings with LYNX behaviour, round 151 (defaults as FL's page shows them [EXEC]):
    bool releaseVelocity = true;    // Settings › MIDI › "Send note release velocity": note offs carry the note's release
    bool sendBendRange = false;     // "Send pitch bend range (semitones)": a pitch-bend-sensitivity RPN when it starts
    int bendRange = 12;             //   the range sent, 1…48 semitones
    bool modXAftertouch = false;    // "Send MOD X as polyphonic aftertouch": each note's Mod X follows its note on
    bool allNotesOffOnStop = true;  // "Send All Notes Off when playback stops" (off: only the held notes are released)
    bool resetOnFlush = true;       // Troubleshooting › "Reset plugin when FL Studio resets": before an export, on panic
    bool sendLoopPosition = true;   // "Send loop position": the play head reports the loop's start and end

    bool operator== (const PluginRef& o) const
    {
        return id == o.id && state == o.state && foreign == o.foreign && fixedBuffers == o.fixedBuffers && separateProcess == o.separateProcess
               && threaded == o.threaded && maxSizeBuffers == o.maxSizeBuffers && allowSmartDisable == o.allowSmartDisable
               && hostMaxBuffer == o.hostMaxBuffer
               && midiInPort == o.midiInPort && midiOutPort == o.midiOutPort && notifyRender == o.notifyRender
               && latencyOverrideMs == o.latencyOverrideMs && releaseVelocity == o.releaseVelocity && sendBendRange == o.sendBendRange
               && bendRange == o.bendRange && modXAftertouch == o.modXAftertouch && allNotesOffOnStop == o.allNotesOffOnStop
               && resetOnFlush == o.resetOnFlush && sendLoopPosition == o.sendLoopPosition;
    }
};

// Channel sampler settings (FL's "Channel settings" window).
// Modulation envelope (cutoff or pitch), like the channel envelopes in FL.
struct ModEnvelope
{
    float amount = 0.0f;  // filter: octaves (±); pitch: semitones (±). 0 = off
    float attack = 0.0f;  // seconds
    float hold = 0.0f;
    float decay = 0.3f;
    float sustain = 0.0f; // 0..1
    float release = 0.2f;
    // 0 = LYNX's linear AHDSR; 1 = Toxic Biohazard's filter envelope (REA_rodada33 §2: exponential stages chosen per
    // 128-sample block, attack towards 5 clipped at 1, elapsed time counted in 1/44100 s)
    int shape = 0;

    bool operator== (const ModEnvelope&) const = default;
};

// Channel LFO (like FL's, REA_rodada6 §2): shape, period, delay and fade-in; the amount
// is in the target's unit. Restarts on every note.
struct ChannelLfo
{
    float amount = 0.0f;  // pan: −1..1; volume: depth 0..1; cutoff: octaves; pitch: semitones. 0 = off
    float period = 0.5f;  // seconds, or beats with `sync`
    bool sync = false;
    int shape = 0;        // 0 sine, 1 triangle, 2 pulse
    float delay = 0.0f;   // seconds (or beats with `sync`) before starting
    float attack = 0.0f;  // fade-in, in the same unit
    bool global = false;  // phase locked to the song position (FL's "Global" button) instead of restarting per note
    bool secondsTiming = false; // delay and fade-in stay in seconds even with `sync` (Kepler's LFO1)

    bool operator== (const ChannelLfo&) const = default;
};

enum LfoTarget { lfoPan, lfoVolume, lfoCutoff, lfoPitch, lfoTargets };

// How a pitch-preserving stretch is computed (core/TimeStretch, REA_rodada160): general = polyphonic material and full
// mixes (FL's élastique Pro / E3 generic / Auto), transients = drums and loops (FL's E2 transient, slice modes), mono =
// a single voice or instrument (FL's E3 mono, E2 mono / speech).
enum class StretchMethod : int { general = 0, transients = 1, mono = 2 };

struct SamplerSettings
{
    float pitch = 0.0f;   // semitones, -24..24
    float attack = 0.0f;  // seconds, 0..2
    float decay = 0.0f;   // seconds until sustain (0 = no decay)
    float sustain = 1.0f; // 0..1
    float release = 0.0f; // seconds, 0..4; 0 = plays the whole sample (one-shot)
    bool reverse = false;
    bool cut = false;     // a new note cuts the previous one ("cut itself")
    float stretchBeats = 0.0f; // > 0: the whole sample lasts this long (in beats), following the BPM
    bool keepPitch = false;    // with stretch: stretches without changing pitch (core/TimeStretch); otherwise resamples
    StretchMethod stretchMethod = StretchMethod::general; // with keepPitch: the stretcher's preset
    float regionStart = 0.0f;  // played region of the sample (fractions 0..1)
    float regionEnd = 1.0f;
    std::vector<float> slices; // slices (starts as fractions): key 60 + i plays slice i, at the original pitch
    bool porta = false;        // portamento (with "cut", the 808 glide): the note starts from the previous note's pitch
    float portaBeats = 0.125f; // glide time, in beats
    float cutoff = 20000.0f;   // channel filter (Hz); low-pass at 20 kHz with no resonance = off
    float resonance = 0.0f;    // 0..1
    int filterType = 0;        // 0 low-pass, 1 band-pass, 2 high-pass, 3 band-stop
    float gainDb = 0.0f;       // the Sampler's Gain knob (dB, -24..+24): a level boost/cut of the sample before the
                               // channel volume (FL's Sampler has none: its closest is the "Normalize" precomputed effect)
    ModEnvelope filterEnv;     // shifts the cutoff (octaves)
    ModEnvelope pitchEnv;      // shifts the pitch (semitones)
    std::array<ChannelLfo, lfoTargets> lfo {}; // per LfoTarget
    // FL's volume envelope as the channel stored it (REA_rodada69 §3): the 17 values of its 0xDA volume record followed
    // by the project's PPQ. When present it replaces attack/decay/sustain/release (delay, hold, tensions, FL's tick
    // stepping); editing the ADSR by hand clears it.
    std::vector<std::int32_t> flVolumeEnvelope;
    // FL's native sampler channel as the project stored it (REA_rodada76): the five 0xDA articulator records (pan,
    // volume, cutoff, resonance, pitch; 17 values each), Mod X, Mod Y and the filter type (0xDB), then the project's
    // PPQ, the project's pan law (FLP event 0x17; 2 = triangular) — 90 values (89: circular). When present the sampler runs FL's voice: the velocity and pan laws with the channel's volume and
    // pan inside the voice, the articulators stepped on the song's tick grid with FL's gain ramps, and the channel filter
    // (core/FlVoiceProcessor.h); ADSR, cutoff/resonance/type, the modulation envelopes and LFOs above are then unused.
    // Editing the envelope or the filter by hand clears it.
    std::vector<std::int32_t> flChannel;

    bool operator== (const SamplerSettings&) const = default;
};

// The Sampler's knobs as automation targets (issue #12): a sampler channel's AutomationTarget { channelParam, channel, 0, k }
// with k one of these. Values 0..1 follow the knobs' own curves (the Sampler settings page), so a clip drawn at 50% is
// the knob at mid travel.
namespace samplerparam
{
enum : int { attack, decay, sustain, release, cutoff, resonance, start, end, count };
const char* name (int param) noexcept;              // "Attack", "Cutoff"... (TR_NOOP-marked in Song.cpp)
float toValue (int param, float natural) noexcept;  // seconds / Hz / fraction -> 0..1
float toNatural (int param, float value) noexcept;  // 0..1 -> seconds / Hz / fraction
float get (const SamplerSettings& s, int param) noexcept;          // 0..1
void set (SamplerSettings& s, int param, float value) noexcept;     // 0..1
} // namespace samplerparam

// Channel arpeggiator (FL's Channel settings).
struct ArpSettings
{
    // 0 off, 1 up, 2 down, 3 up and down (bounce: the ends once), 4 random, 5 up and down "sticky" (the ends twice;
    // FL's 4th direction, REA round 146b [EXEC])
    int direction = 0;
    int range = 1;           // octaves, 1..4
    float stepBeats = 0.25f; // duration of each step in beats (0.25 = sixteenth note)
    // the note's length as a fraction of the step; above 1 (FL's gate past 100 %, up to 128/48) the next step waits
    // for the note to end (REA round 146b [EXEC]: FL's step = time · max(1, gate))
    float gate = 0.5f;
    int repeat = 1;          // LYNX: times each note plays before the next one
    // FL's Repeat (REA round 146b [EXEC]): the sequence plays this many times in an octave before the next octave of
    // the range (1 = once)
    int octaveRepeat = 1;
    // FL's Chord (REA round 146b [EXEC]): empty = "Auto" (the held notes); else the chord the arpeggio plays, as
    // semitones above the lowest held note (FL's index k = line k of its Chords.map: 1 Major, 5 minor, 27 Maj7…)
    std::vector<int> chord;
    bool chordSustain = true; // "Auto (sustain)": the arpeggio keeps the notes of the chord while one is held
    bool slide = false;       // FL's Slide: each arpeggio note glides from the previous one (no retrigger)

    bool enabled() const noexcept { return direction > 0; }
    bool operator== (const ArpSettings&) const = default;
};

// Built-in 3-oscillator synth (in the spirit of FL's 3xOsc).
enum class Waveform : std::uint8_t
{
    sine,
    triangle,
    saw,
    square,
    noise,
    spectral // channel harmonic table (SynthSettings::spectrum): imported additive timbres (Harmless)
};

struct SynthOscillator
{
    Waveform wave = Waveform::saw;
    int octave = 0;      // -3..3
    float detune = 0.0f; // cents, -100..100
    float level = 0.0f;  // 0..1
    int semitones = 0;   // -11..11, added to the octave (3x Osc's "coarse" goes up to ±24)
    float pan = 0.0f;    // -1..1, linear balance as in FL

    bool operator== (const SynthOscillator&) const = default;
};

struct SynthSettings
{
    std::array<SynthOscillator, 3> osc { SynthOscillator { Waveform::saw, 0, 0.0f, 1.0f }, SynthOscillator {}, SynthOscillator {} };
    float attack = 0.005f; // seconds
    float decay = 0.3f;
    float sustain = 1.0f;  // 0..1
    float release = 0.25f;
    float cutoff = 2500.0f;  // Hz
    float resonance = 0.0f;  // 0..1
    int filterType = 0;      // 0 low-pass, 1 band-pass, 2 high-pass, 3 band-stop
    bool ladder = false;     // 4-pole ladder (Toxic Biohazard's filter, REA_rodada26 §2) instead of the 2-pole SVF
    int chorus = 0;          // Kepler's chorus on the channel output: 0 off, 1 I, 2 II, 3 I+II (REA_rodada26 §3)
    // Harmless (REA_rodada27 §1.1/§1.2): stereo tremolo depth d (2.5 Hz, |1.25·cos|/|1.25·sin| per side) and vibrato depth
    // in cents (6 Hz), both on free-running global phases; a voice fades into them over 0.1 s / 1/3 s (r²)
    float stereoTremolo = 0.0f;
    float vibratoCents = 0.0f;
    float keyTrack = 0.0f;   // cutoff follows the note: octaves per octave from key 48 (C3, 130.81 Hz)
    // kick pitch sweep: f = f_note·(1 + sweep·s⁴), s = e^(−t/sweepTime) (BassDrum's "Peak"/"Slide")
    float sweep = 0.0f;      // 0 = off
    float sweepTime = 0.1f;  // seconds (time constant of s)
    // 3-band tone control after the filter (BooBass's EQ): low < ~178 Hz < mid < ~1.18 kHz < high,
    // split by 1-pole low-passes; linear gains (1, 1, 1 = off)
    std::array<float, 3> bandGain { 1.0f, 1.0f, 1.0f };
    // "spectral" shape: amplitude and phase (in cycles) of harmonics 1…256, as a sum of sines
    static constexpr int spectrumSize = 256;
    std::array<float, spectrumSize> spectrum {};
    std::array<float, spectrumSize> spectrumPhase {};
    // per-harmonic decay: seconds for harmonic h to fall 60 dB from the note start (0 = holds); lets the
    // spectrum evolve during the note (Ogun's per-partial decay)
    std::array<float, spectrumSize> spectrumDecay {};
    // level each harmonic decays towards (0 = silence): A(t) = target + (A₀ − target)·10^(−3t/decay) — Ogun's ALT DECAY
    std::array<float, spectrumSize> spectrumTarget {};
    ModEnvelope filterEnv;   // shifts the cutoff (octaves)
    ModEnvelope pitchEnv;    // shifts the pitch (semitones)
    std::array<ChannelLfo, lfoTargets> lfo {}; // per LfoTarget
    bool mono = false;       // single voice: the new note takes over the sounding voice (no click)
    bool porta = false;       // portamento: the new note glides from the previous one
    float portaBeats = 0.125f; // glide time, in beats (FL's default: half a step)
    bool am = false;
    bool ring = false;       // with `am`: Sytrus ring modulation, (osc 1 + 2)·(1 − g3 + g3·osc 3) instead of 1 − g3·osc 3         // osc 3 modulates the amplitude of osc 1 + osc 2 (3x Osc's "AM")
    float fm = 0.0f;         // FM index: osc 3 (muted) shifts the phase of osc 1 + 2 by fm·output cycles; 0 = off
    float fmDecay = 0.0f;    // seconds for the index to fall to fmSustain (0 = constant index)
    float fmSustain = 1.0f;  // fraction of the index that remains after the decay

    bool operator== (const SynthSettings&) const = default;
};

// Zone of a multi-sample sampler (like FL's DirectWave/FPC zones): the sample plays on the keys and
// velocities in range, tuned relative to the root note.
struct SampleZone
{
    std::string path;
    int rootKey = 60;
    int lowKey = 0, highKey = 127;
    int lowVelocity = 0, highVelocity = 127; // 0..127
    float tune = 0.0f;  // semitones
    float gain = 1.0f;
    std::int64_t loopStart = 0, loopEnd = 0; // in sample frames; loopEnd <= loopStart = no loop
    int loopMode = 0;   // 0 no loop, 1 continuous, 2 while the note is held

    bool operator== (const SampleZone&) const = default;
};

// FL's Patcher (REA_rodada141): a user-editable graph of plugins, in a mixer slot (EffectKind::patcher) or as a
// Channel Rack generator (InstrumentKind::patcher). Nodes are effects or instruments; wires carry audio (summed with
// a linear gain, FL's IPP multiply-add), notes (MIDI) or control values (a controller's output moving a parameter).
// The two host nodes are "From LYNX" (patchFromHost: the slot's audio input, its side-chain, the channel's notes)
// and "To LYNX" (patchToHost: the main audio output and the extra outputs to other mixer inserts).
struct PatchNode; // defined after EffectSlot
constexpr int patchFromHost = -1, patchToHost = -2;
// FL's Patcher "Surface" node (REA round 151): its control outputs are the Surface tab's controls
constexpr int patchSurface = -3;

// A control on the Patcher's Surface tab (FL's Patcher › Surface, REA round 151 [EXEC FL 2026: Add control › Bevel,
// Button, CheckBox, Digits, Label, Keyboard, Knob, Slider, XY; each one becomes a control output of the Map's
// "Surface" node, cabled to any node parameter]). LYNX's own drawing; knob, slider, button, check box and XY pad.
struct SurfaceControl
{
    enum class Kind : std::uint8_t { knob, slider, button, checkBox, xy };
    Kind kind = Kind::knob;
    std::string name;
    float value = 0.0f, valueY = 0.0f; // 0..1 (an XY pad has two)
    float x = 40.0f, y = 40.0f;        // place on the Surface tab
    float size = 1.0f;                 // a Control Surface / Dashboard control's scale (round 164; 0.5…3)
    // A Dashboard control that sends MIDI on the Dashboard's port (round 165, FL's record types): none = a controller
    // output only; cc, rpn, nrpn (parameter number midiNumber), channel pressure, note (key midiNumber). The value
    // 0..1 maps to midiLow…midiHigh (high > 127: 14-bit).
    enum class Midi : std::int8_t { none = -1, cc = 0, rpn = 1, nrpn = 2, pressure = 3, note = 5 };
    Midi midi = Midi::none;
    int midiNumber = 0;
    int midiLow = 0, midiHigh = 127;
    // control outputs: one each, an XY pad two (X then Y)
    int outputs() const noexcept { return kind == Kind::xy ? 2 : 1; }
    bool operator== (const SurfaceControl&) const = default;
};

struct PatchWire
{
    enum class Kind : std::uint8_t { audio, midi, control };
    Kind kind = Kind::audio;
    int from = patchFromHost; // source node index (or patchFromHost)
    int fromPort = 0;         // audio: output bus (host: 0 = input, 1 = side-chain); control: the source's control output
    int to = patchToHost;     // destination node index (or patchToHost)
    int toPort = 0;           // audio: input bus (0 = main, 1 = side-chain; host: 0 = main output, k = extra output k);
                              // control: the destination's parameter index
    float gain = 1.0f;        // audio: linear cable gain
    bool enabled = true;      // FL's disabled cable: ignored in the sum
    std::vector<float> curve; // control: value map over 0..1 (≥ 2 points; empty = straight)
    // MIDI (round 162): "From LYNX" output pins: fromPort 0 = Notes (the channel's notes), patchMidiPortPin (N) = what
    // reaches the Patcher on MIDI port N; a node's MIDI output (MIDI Out) = fromPort 0; a node's notes input = toPort 0;
    // "To LYNX": toPort N = MIDI port N.

    bool operator== (const PatchWire&) const = default;
};
// The "From LYNX" pin of MIDI port N (0…255), and back (−1 = the Notes pin)
constexpr int patchMidiPortPin (int port) noexcept { return 1 + port; }
constexpr int patchPinMidiPort (int pin) noexcept { return pin - 1; }
constexpr int patchMidiPorts = 256;

// A parameter the Patcher publishes (FL's record 1100): automating the Patcher's parameter k moves this node parameter
// (through `curve`, as a control cable). node −1 = an unused index.
struct PatchParam
{
    int node = -1, param = 0;
    std::vector<float> curve;
    std::string name;

    bool operator== (const PatchParam&) const = default;
};

struct PatcherDoc
{
    std::vector<PatchNode> nodes;
    std::vector<PatchWire> wires;
    std::vector<PatchParam> published; // [Patcher parameter]
    float inputX = 30.0f, inputY = 150.0f, outputX = 560.0f, outputY = 150.0f; // the host nodes in the editor
    // "To LYNX" extra audio outputs (bus 1…): the mixer insert each one feeds (−1 = the k-th insert after the
    // channel's own); generator Patcher only
    std::vector<int> outputTracks;
    // The I/O nodes' MIDI pins the user activated (FL's node menu › Events: Notes, MIDI port 0…255; round 162). A pin
    // with a cable is always shown. fromNotesHidden: "Deactivate" on the Notes pin of "From LYNX".
    std::vector<int> fromMidiPorts, toMidiPorts; // sorted port numbers
    bool fromNotesHidden = false;
    // the Surface tab's controls and the Surface node's place on the Map (round 151)
    std::vector<SurfaceControl> surface;
    float surfaceX = 30.0f, surfaceY = 30.0f;
    // the value of the Surface node's control output k (the controls' outputs in order); 0 past the end
    float surfaceOutput (int k) const noexcept
    {
        for (const auto& c : surface)
        {
            if (k < c.outputs())
                return k == 0 ? c.value : c.valueY;
            k -= c.outputs();
        }
        return 0.0f;
    }
    int surfaceOutputs() const noexcept
    {
        int n = 0;
        for (const auto& c : surface)
            n += c.outputs();
        return n;
    }

    bool empty() const noexcept { return nodes.empty() && wires.empty(); }
    bool operator== (const PatcherDoc& o) const; // (Song.cpp: needs PatchNode)
};

// FL's "Echo delay / fat mode" (REA round 146b [EXEC: FL renders of the Sampler render test with the 0xD1 record
// changed]): each note repeats `echoes` times, `timeBeats` apart; echo k plays at feedback^k of the note's volume,
// with k·pan added to the note's pan (clamped) and k·pitch cents.
struct EchoSettings
{
    float feedback = 0.0f;   // 0..1 (FL 0..12800): the volume factor from one echo to the next; 0 = off
    float pan = 0.0f;        // −1..1 per echo (FL 0..12800, 6400 = centre)
    float pitch = 0.0f;      // cents per echo (FL ±1200)
    float modX = 0.0f, modY = 0.0f; // −1..1 per echo (FL 0..256, 128 = none): stored; LYNX's notes do not route mod X / Y
    int echoes = 4;          // FL 1..10
    float timeBeats = 0.75f; // the delay between echoes (FL: 1/192 beats; 144 = 3 steps)
    bool pingPong = false;   // echoes alternate their pan side
    bool fat = false;        // FL's fat mode (REA round 149): a mirrored second series, every echo at feedback × the level

    bool enabled() const noexcept { return feedback > 0.0f && echoes > 0; }
    bool operator== (const EchoSettings&) const = default;
};

// FL's Vol / Key tracking (record 0xE4, REA round 146b): the note's velocity (or key) away from `mid` moves its pan
// (and FL's mod X / Y, stored only).
struct NoteTracking
{
    int mid = 0;            // velocity 0..128 (FL default 100) or key 0..131 (FL default 60 = C5)
    float pan = 0.0f;       // −1..1: the pan at the far end of the range
    float modX = 0.0f, modY = 0.0f;

    bool operator== (const NoteTracking&) const = default;
};

// The rest of FL's Misc tab (REA round 146b)
struct ChannelMisc
{
    float shiftBeats = 0.0f; // Time › Shift: every note starts (and ends) this much later
    float gateBeats = 0.0f;  // Time › Gate: notes last at most this long (0 = off)
    bool fullPorta = true;   // Time › Full porta: a porta note glides the whole way (off: FL's partial slide)
    EchoSettings echo;
    NoteTracking velocity { 100 }, key { 60 };
    int pitchRange = 2;      // the header's PITCH knob range, semitones (FL default 2)
    bool mainPitch = true;   // follows the song's main (master) pitch
    // FL's Root note keyboard (REA round 149): the note at which the channel plays at its own pitch (C5 = 60). A
    // sampler's root is in its pitch (SamplerSettings::pitch moves with it); other instruments only follow it with
    // "Add to key" (0xD7 byte 63): their notes are played (60 − root) semitones away
    int rootNote = 60;
    bool addToKey = false;
    // FL's Root note › Fine tune (REA round 151 [DATA: .flp event 0x8E, signed cents]): every note this many cents away
    float fineTune = 0.0f;

    bool operator== (const ChannelMisc&) const = default;
};

// FL's Layer channel (REA_rodada164a §1, FL's manual "Layer channel settings"): it makes no sound; every note played on
// it (Piano roll, steps, keyboard) plays its children channels. A child played from its own notes is not affected.
struct LayerSettings
{
    std::vector<int> children; // channel indices ("Set children": the selected channels, in Channel Rack order)
    bool random = false;     // one child at random per note (else every child)
    bool crossfade = false;  // the Fade knob crossfades the children, top to bottom
    bool sequential = false; // the children in turn (round robin), one per note
    float fade = 0.0f;       // the Fade knob 0..1 (with crossfade)
    // (the Layer's VOL / PAN are the channel's volume / pan, its PITCH the header's pitch knob: sampler.pitch)

    bool operator== (const LayerSettings&) const = default;
};

struct ChannelDef
{
    std::string name;
    InstrumentKind instrument = InstrumentKind::synth;
    float volume = 0.78f;  // 0..1 (channelGain curve; 0.78 = FL's default)
    float pan = 0.0f;      // -1..1
    bool muted = false;
    std::string samplePath; // used when instrument == sampler
    std::vector<SampleZone> zones; // multi-sample sampler (empty: samplePath only)
    int mixerTrack = 0;     // 0 = straight to Master
    // FL's Polyphony › Max (REA round 139): at most this many notes sound at once, a new one releases the oldest
    // (0 = unlimited). The Export window's "Disable maximum polyphony" lifts it for the render.
    int maxPolyphony = 0;
    PluginRef plugin;       // used when instrument == plugin
    std::vector<std::uint8_t> controllerState; // instrument == controller: FL Envelope Controller state
    std::vector<float> modelParams; // instrument == sakura / drumpad / boobass / drumsynth / harmless: FL's raw state parameters
    std::vector<std::uint8_t> pluginState; // instrument == sytrus / harmor / ogun / gms / morphine / fruitkick / bassdrum: the FL plugin's saved state (record 0xD5)
    SamplerSettings sampler; // used when instrument == sampler
    SynthSettings synth;     // used when instrument == synth
    float swingMix = 1.0f;   // how much of the global swing applies to this channel (0..1), as in FL
    ArpSettings arp;
    int cutGroup = 0;        // playing this channel cuts channels with an equal cutBy (0 = none)
    int cutBy = 0;
    std::uint32_t colour = 0; // 0xFFRRGGBB; 0 = theme default
    int group = -1;           // the Channel Rack filter group (index into Song::channelGroups); -1 = none
    PatcherDoc patcher;       // instrument == patcher: its graph (REA_rodada141)
    ChannelMisc misc;         // FL's Misc tab: time shift / gate, echo delay, velocity / key tracking (REA round 146b)
    // Piano Roll key labels (FL 2026's custom key labels, REA round 153): MIDI key -> the name shown on that key
    std::map<int, std::string> keyLabels;
    // FL's key region (the Channel settings keyboard's ruler, REA_rodada164a §2): the channel plays only the notes
    // from keyLow to keyHigh (MIDI keys); 0…127 = every note. Layer's "Split children" sets one key per child.
    int keyLow = 0, keyHigh = 127;
    LayerSettings layer; // instrument == layer: its children and options

    bool hasKeyRegion() const noexcept { return keyLow > 0 || keyHigh < 127; }
    bool inKeyRegion (int key) const noexcept { return key >= keyLow && key <= keyHigh; }
    bool operator== (const ChannelDef&) const = default;
};

enum class EffectKind : std::uint8_t
{
    eq,
    compressor,
    delay,
    reverb,
    plugin, // external effect (VST3/CLAP), created by the app
    limiter,
    filter,
    distortion,
    chorus,
    parametricEq, // 7 bands (like FL's Parametric EQ 2)
    utility,      // gain, pan and stereo matrix (FL's Balance, PanOMatic, Stereo Shaper)
    volumeGate,   // per-step gain synced to the bar (Gross Beat's volume)
    peakController, // does not change the sound: measures the peak and serves as a source for live automation (FL's Peak Controller)
    pitchShift,   // changes pitch without changing duration (two read heads crossfaded)
    freqShift,    // shifts all frequencies by Hz (single sideband, does not preserve harmonics)
    parallel,     // container: parallel effect branches summed (FL's Patcher); see EffectSlot::branches
    lofi,         // sample-rate reduction (sample and hold) and bit reduction (Lo-fi from FL's Effector)
    effector,     // FL's Effector: 12 algorithms with X/Y, LFO and Mod Rate (core/Effector.h)
    crossover,    // one band of a frequency splitter (FL's Frequency Splitter): Linkwitz-Riley high-pass and/or low-pass
    rhythmGain,   // gain following a curve of points in beats, looped, locked to the song (Love Philter envelopes)
    autoPan,      // pan moved by a triangle LFO locked to the song (Love Philter pan LFO)
    vocoder,      // Fruity Vocoder channel vocoder: L modulates, R is the carrier (mono output)
    pitchCorrect, // pitch correction (FL's Pitcher): detects the pitch and pulls it to the nearest allowed note
    squeeze,      // FL's Fruity Squeeze, exact (REA_rodada29 §2): quantiser, periodic gate, filter; raw values / max
    vintagePhaser, // FL's Vintage Phaser (REA_rodada29 §3, core/VintagePhaser.h): 36 normalised engine values
    vocodex,      // FL's Vocodex (REA_rodada35, core/Vocodex.h): built from FL's raw state (EffectSlot::state)
    maximus,      // FL's Maximus / Soundgoodizer engine (REA_rodada36/38, core/MaximusFx.h): built from EffectSlot::state
    fruityLimiter,   // FL's Fruity Limiter, exact (REA_rodada48, core/FruityDynamics.h): FL's raw controls / range
    fruityCompressor, // FL's Fruity Compressor, exact (REA_rodada48, core/FruityDynamics.h): FL's raw controls / range
    fruityWaveShaper, // FL's Fruity WaveShaper, exact (REA_rodada51, core/FruityShapers.h): curve in EffectSlot::state, controls / range
    fruitySoftClipper, // FL's Fruity Soft Clipper, exact (REA_rodada51, core/FruityShapers.h): threshold, post / range
    reeverb2,     // FL's Fruity Reeverb 2, exact (REA_rodada49, core/Reeverb2.h): FL's 15 controls + 2 options, (raw − min) / range
    reeverb1,     // FL's Fruity Reeverb (first version), exact (REA_rodada49 §6, core/Reeverb2.h): FL's 10 controls, raw / max
    fruityDelay2, // FL's Fruity Delay 2, exact (REA_rodada52, core/FruityDelay.h): FL's 8 raw controls / range
    fruityDelay3, // FL's Fruity Delay 3, exact (REA_rodada52, core/FruityDelay.h): FL's 26 raw controls / range
    stereoShaper, // FL's Fruity Stereo Shaper, exact (REA_rodada54, core/StereoFx.h): FL's 8 controls / range + difference flag
    stereoEnhancer, // FL's Fruity Stereo Enhancer, exact (REA_rodada54, core/StereoFx.h): FL's 6 controls / range
    transientProcessor, // FL's Transient Processor, exact (REA_rodada54, core/StereoFx.h): FL's 9 controls / range
    spreader,     // FL's Spreader, exact (REA_rodada54, core/StereoFx.h): FL's 5 controls / range
    fruityChorus, // FL's Fruity Chorus, exact (REA_rodada59 §1, core/ChorusFx.h): controls / range
    hyperChorus,  // FL's Hyper Chorus, exact (REA_rodada59 §2, core/ChorusFx.h): (raw − min) / range; instance in EffectSlot::state
    vintageChorus, // FL's Vintage Chorus, exact (REA_rodada59 §3, core/ChorusFx.h): (raw − min) / range; instance in EffectSlot::state
    fruityPanOMatic, // FL's Fruity PanOMatic, exact (REA_rodada58, core/FruityBasicFx.h): pan, volume, LFO; controls / range
    fruityBalance,   // FL's Fruity Balance, exact (REA_rodada58, core/FruityBasicFx.h): balance, volume; controls / range
    fruityBloodOverdrive, // FL's Fruity Blood Overdrive, exact (REA_rodada58, core/FruityBasicFx.h): 6 controls / range
    grossBeat,    // FL's Gross Beat engine (REA_rodada53, core/GrossBeat.h): slots in EffectSlot::state, parameters in params
    fruityDelayBank, // FL's Fruity Delay Bank, exact (REA_rodada56, core/FruityDelayBank.h): FL's state in EffectSlot::state, params = its 4 global controls / 128
    fruityMultiband, // FL's Fruity Multiband Compressor, exact (REA_rodada56, core/FruityMultiband.h): FL's raw controls / range (+ FL's state)
    fruityFastDist, // FL's Fruity Fast Dist, exact (REA_rodada55, core/FlangusFastDist.h): controls / range
    fruityFlangus,  // FL's Fruity Flangus, exact (REA_rodada55, core/FlangusFastDist.h): controls / range
    fruityFlanger,  // FL's Fruity Flanger, exact (REA_rodada55, core/FlangusFastDist.h): controls / range
    luxeVerb,     // FL's LuxeVerb, exact (REA_rodada60, core/LuxeVerb.h): FL's 49 controls (raw − min) / range + 4 sync switches
    convolver,    // FL's Fruity Convolver, exact (REA_rodada60, core/ConvolverFx.h): FL's 4 controls as (raw − min) / range; impulse response, knobs and envelopes in EffectSlot::state
    equo,         // FL's EQUO, exact (REA_rodada61, core/EquoFx.h): FL's state in EffectSlot::state, knobs (raw − min) / range
    flPitchShifter, // FL's Pitch Shifter, exact (REA_rodada61, core/PitchShifterFl.h): FL's 13 knobs (raw − min) / range + 3 options
    frequencySplitter, // FL's Frequency Splitter, exact (REA_rodada62, core/FrequencySplitter.h): 26 engine values + output bands
    fruityFastLP, // FL's Fruity Fast LP, exact (REA_rodada63 §1, core/FruityFilters.h): cutoff, resonance; controls / range
    fruityFreeFilter, // FL's Fruity Free Filter, exact (REA_rodada63 §2, core/FruityFilters.h): 4 controls / range + CENTER
    fruityFilter, // FL's Fruity Filter, exact (REA_rodada63 §3, core/FruityFilters.h): 6 controls / range + CENTER
    hardcore,     // FL's Hardcore, exact (REA_rodada71, core/HardcoreFx.h): FL's 50 parameters (cabinet / 20, types / 11); impulses in EffectSlot::state
    distructor,   // FL's Distructor, exact (REA_rodada71, core/DistructorFx.h): FL's state (+ cabinet impulses) in EffectSlot::state, module gains / 10000 + clipper
    multibandDelay, // FL's Multiband Delay, exact (REA_rodada71, core/MultibandDelayFx.h): FL's state in EffectSlot::state, 12 globals (raw − min) / range
    lovePhilter,   // FL's Fruity Love Philter, exact (REA_rodada66, core/LovePhilter.h): FL's state in EffectSlot::state, 48 controls
    newtone,      // FL's Newtone (REA_rodada73, core/NewtoneFx.h): pass-through; in slave mode adds its source sample at the song time
    newtime,      // FL's Newtime (REA_rodada73, core/NewtoneFx.h): the same contract (its time warping is not reproduced)
    facetEq,      // LYNX's Facet EQ (MEASURE_fabfilter §9, core/FacetEq.h): 8 matched analog-prototype bands, 42 controls
    apexLimiter,  // LYNX's Apex Limiter (MEASURE_fabfilter §9, core/ApexLimiter.h): look-ahead true-peak limiter, 7 controls
    ridgeStrip,   // LYNX's channel strip (REA_rodada96, core/MixSuite.h): filters, 4-band EQ, compressor, gate / expander
    glowLeveler,  // LYNX's optical leveler (REA_rodada96, core/MixSuite.h)
    hushDeEsser,  // LYNX's de-esser (REA_rodada96, core/MixSuite.h)
    aether,       // LYNX's air band + exciter (REA_rodada96, core/MixSuite.h)
    ember,        // LYNX's saturator (REA_rodada96, core/MixSuite.h)
    lynxTune,     // LYNX Tune, LYNX's own real-time vocal pitch corrector (REA_rodada95, core/LynxTune.h)
    fruityPhaser, // FL's Fruity Phaser, exact (REA_rodada105_phaser, core/FruityPhaser.h): FL's 9 controls / max
    epochVerb,    // LYNX's Epoch Verb (REA_rodada100, core/LynxVerbs.h): multi-mode vintage-digital reverb, 17 controls
    atrium,       // LYNX's Atrium (REA_rodada100, core/LynxVerbs.h): room reverb, early + late sections, 22 controls
    foilPlate,    // LYNX's Foil Plate (REA_rodada100, core/LynxVerbs.h): plate reverb, 12 controls
    nebula,       // LYNX's Nebula (REA_rodada100, core/LynxVerbs.h): delay-reverb network, 13 controls
    halo,         // LYNX's Halo (REA_rodada100, core/LynxVerbs.h): shimmer reverb, 12 controls
    horizon,      // LYNX's Horizon (REA_rodada100, core/LynxVerbs.h): echo + reverb, 24 controls
    echoform,     // LYNX's multi-mode echo (REA_rodada101, core/EchoSuite.h), 29 controls
    spiralEcho,   // LYNX's frequency-shifting echo (REA_rodada101, core/EchoSuite.h), 8 controls
    orbit,        // LYNX's modulation machine: flangers / choruses / doubler (REA_rodada101, core/EchoSuite.h), 6 controls
    latticeMod,   // LYNX's multitap modulated delay (REA_rodada101, core/EchoSuite.h), 34 controls
    fruitySend,   // FL's Fruity Send, exact (REA_rodada105_send, core/FruityBasicFx.h): pan, volume, dry / range + the insert it sends to
    crestCompressor, // LYNX's Crest Compressor (REA_rodada108, core/DynamicsSuite.h): styles, side-chain EQ, character, oversampling
    prismMultiband,  // LYNX's Prism Multiband (REA_rodada108, core/DynamicsSuite.h): 4-band dynamics
    sentryGate,      // LYNX's Sentry Gate (REA_rodada108, core/DynamicsSuite.h): gate / expander
    quellDenoiser,   // LYNX's Quell Denoiser (REA_rodada119, core/VoiceTools.h): one-knob multiband noise suppressor
    geminiDoubler,   // LYNX's Gemini Doubler (REA_rodada119, core/VoiceTools.h): two pitch-shifted delay voices
    formulaController, // FL's Formula Controller (REA_rodada133, core/FormulaController.h): no sound; output = formula of a, b, c
    patcher,         // FL's Patcher (REA_rodada141, core/Patcher.h): a graph of effects / instruments in EffectSlot::patcher
    voiceShift,      // LYNX's Voice Shift (REA_rodada135s, core/VoiceShift.h): pitch / formant / quantize / robot + drive
    emphasizer,      // FL's Emphasizer, exact (REA_rodada154, core/EmphasizerFx.h): FL's 6 controls / max
    flFrequencyShifter, // FL's Frequency Shifter, exact (REA_rodada154, core/FreqShifterFl.h): FL's 10 controls (raw − min) / range
    flLowLifter,     // FL's Low Lifter, exact (REA_rodada154, core/LowLifterFl.h): FL's 15 controls / max
    flPhaseInverter, // FL's Fruity Phase Inverter, exact (REA_rodada165k, core/FlLegacyFx.h): its control / 1024
    flCenter,        // FL's Fruity Center, exact (REA_rodada165k, core/FlLegacyFx.h): on / off
    flBassBoost,     // FL's Fruity Bass Boost, exact (REA_rodada165k, core/FlLegacyFx.h): frequency, amount / 1024
    fl7BandEq,       // FL's Fruity 7 Band EQ, exact (REA_rodada165k, core/FlLegacyFx.h): 7 gains (raw + 1800) / 3600
    flDelay1,        // FL's Fruity Delay (v1), exact (REA_rodada165k, core/FlLegacyFx.h): 6 controls (raw − min) / range
    waveCandy,       // LYNX's analyser in FL's Wave Candy role (REA_rodada164c, core/VisualFx.h): no sound; scope, spectrum, meters
    visualizer,      // LYNX's Visualizer in FL's ZGameEditor Visualizer role (REA_rodada164c): no sound; scene in EffectSlot::state
    xyController,    // FL's X-Y Controller (REA_rodada164a, core/XyController.h): no sound; outputs X, Y follow a pad
    xyzController,   // FL's X-Y-Z Controller (REA_rodada164a, core/XyController.h): the same with a Z axis (mouse wheel)
    controlSurface,  // FL's Control Surface (REA_rodada164a, core/ControlSurface.h): a user panel; output k = params[k]
    hardwareInsert,  // LYNX's hardware insert (REA_rodada164d, core/HardwareInsert.h): send to device outputs, return from inputs, pinged round trip
    fruityMute2,     // FL's Fruity Mute 2, exact (REA_rodada165g §1, core/FlExtraFx.h): mute, channel (L, L + R, R)
    bigClock,        // FL's Fruity Big Clock (REA_rodada165g §2, core/FlExtraFx.h): no sound; the song position in big digits
    tuner,           // FL's Tuner (REA_rodada165g §3, core/FlExtraFx.h): no sound; chromatic tuner fed by the slot's tap
    htmlNotebook,    // FL's Fruity HTML NoteBook (REA_rodada165g §4, core/FlExtraFx.h): no sound; its page's address in EffectSlot::state
    scratcher        // FL's Fruity Scratcher (REA_rodada165g-flscratch165, core/Scratcher.h): turntable over EffectSlot::samplePath, added to the input
};
// the last EffectKind (loops over every kind; update when one is appended)
constexpr EffectKind lastEffectKind = EffectKind::scratcher;

// round 165x: 160 → 192 (Ember had reached 160; a slot's parameters are 768 bytes, project files store only an engine's
// own count, so older projects load unchanged). Round 150: 128 → 160 (Prism Multiband 155, Ember 133).
constexpr int maxEffectParams = 192; // LuxeVerb uses 53 (FL's 49 controls + 4 sync switches); Facet EQ 88 (round 128); Echoform 102 (round 144)
constexpr int maxEffectSlots = 10; // as in FL
using EffectParams = std::array<float, maxEffectParams>;

struct EffectSlot;

// A branch of the `parallel` container: a chain of inner effects and the gain it enters the sum with
// (empty chain = the dry signal).
struct EffectBranch
{
    std::vector<EffectSlot> chain;
    float gain = 1.0f;
};

// Live modulation inside a `parallel` container: the control output of child `source` of branch `branch`
// drives parameter `param` of child `target` of the same branch, through `curve` (33 points over 0..1).
struct EffectLink
{
    int branch = 0, source = 0, target = 0, param = 0;
    std::vector<float> curve;
};

struct EffectSlot
{
    EffectKind kind = EffectKind::eq;
    bool enabled = true;
    EffectParams params {}; // 0..1; meaning depends on the effect
    float mix = 1.0f;       // slot dry/wet (as in FL): 0 = dry signal only
    // FL's per-plugin "Smart disable" (REA round 139): on = the slot is skipped while its input and output are silent
    // (built-ins after 1 s, plugins after 4 s); off = always processed. FL's "Switch smart disable for all plugins"
    // turns it on everywhere.
    bool smartDisable = true;
    PluginRef plugin;                             // used when kind == plugin
    int sidechain = -1;   // mixer track whose (post-fader) output feeds the effect's detector (compressor); −1 = none
    int sidechain2 = -1;  // second side input (vocoder carrier); −1 = none
    std::vector<EffectBranch> branches;           // used when kind == parallel
    std::vector<EffectLink> links;                // used when kind == parallel
    std::vector<std::uint8_t> state;              // FL's raw plugin state, for exact effects built from it (vocodex, maximus)
    std::string samplePath;                       // audio file the effect plays (Newtone/Newtime source sample); "" = none
    // When set (id or foreign), this built-in effect is a LYNX substitute for that missing third-party plugin
    // (core/PluginSubstitutes.h): the original reference is kept byte for byte, saved with the project, and the slot
    // goes back to it once the plugin is installed.
    PluginRef substituteFor;
    // The editor's header (REA round 129, the originals' preset bar and A/B compare): the name of the preset loaded
    // into the slot ("" = none), and the other one of the two settings A / B (its parameters; empty = B was never
    // used) with which of them is playing. Saved with the project; the audio only ever sees `params`.
    std::string presetName;
    std::vector<float> abOther;
    bool abShowingB = false;
    PatcherDoc patcher; // kind == patcher: its graph (REA_rodada141)

    bool operator== (const EffectSlot& o) const; // every field (Song.cpp)

    bool isSubstitute() const noexcept { return ! substituteFor.id.empty() || substituteFor.foreign.format != ForeignPlugin::Format::none; }
};

// A Patcher node (REA_rodada141): an effect or an instrument, with its place in the editor.
struct PatchNode
{
    bool generator = false;
    EffectSlot effect;  // !generator: the effect (any kind: built-in, plugin, another Patcher)
    ChannelDef channel; // generator: the instrument and its settings (name, plugin, state…)
    std::string label;  // the user's name for the node ("" = the plugin's)
    float x = 0.0f, y = 0.0f;
    // Extra audio outputs of a splitting node (FL's Frequency Splitter bands, Stereo Shaper difference): output bus
    // `bus` is the node's input through `head` (none = direct) × gain. Built on import from FL's state.
    struct BusHead
    {
        int bus = 1;
        std::optional<EffectSlot> head;
        float gain = 1.0f;
        bool operator== (const BusHead& o) const;
    };
    std::vector<BusHead> busHeads;

    bool operator== (const PatchNode& o) const;
};

// The insert a Fruity Send slot sends its mid-chain signal to (1…127), or −1 (none, or not a Fruity Send).
// Such a slot is a route too: the destination is processed after the slot's insert (Song::mixerOrder).
int fruitySendTarget (const EffectSlot& slot) noexcept;

// Send from one track to another (post-fader), like FL's routing between inserts.
struct MixerSend
{
    int target = 0;      // destination track (> 0; Master is `toMaster`)
    float level = 1.0f;  // linear gain, 0..maxFaderGain (FL's send knob: the fader curve, 0 dB at 80%, +5.6 dB at 100%)

    bool operator== (const MixerSend& o) const noexcept { return target == o.target && level == o.level; }
};

// Built-in EQ of each mixer track (FL's 3 bands: low shelf, peak, high shelf),
// after the effect slots and before the fader (REA_rodada6 §1).
struct TrackEq
{
    std::array<float, 3> gainDb { 0.0f, 0.0f, 0.0f };          // ±18
    std::array<float, 3> hz { 90.0f, 1500.0f, 8000.0f };       // 10 Hz .. 16 kHz
    std::array<float, 3> width { 0.267f, 0.267f, 0.267f };     // 0..1: peak Q / shelf slope

    bool active() const noexcept { return gainDb[0] != 0.0f || gainDb[1] != 0.0f || gainDb[2] != 0.0f; }
    bool operator== (const TrackEq&) const = default;

    // FL conversions: peak Q = 0.0999·(1001^w − 1) + 0.1; shelves S = 1.9·w + 0.1
    static double peakQ (float w) noexcept;
    static double shelfSlope (float w) noexcept;
};

// When an insert's external audio input is heard through it (FL's "Monitor external input": Off / When armed /
// Auto / On, REA_rodada88 §1). Auto [model]: like "When armed", except during playback without recording, so a
// take plays back without the live input on top.
enum class InputMonitor : std::uint8_t
{
    off,
    whenArmed,
    automatic,
    on
};

enum class MixerDock : std::uint8_t { left, middle, right };

// FL's mixer track latency menu "Bypass track latency compensation" (REA round 165f-mixer; .flp event 0x31): a bypassed
// track's outputs get no PDC delay (its own latency still makes the other paths wait), so a live input monitored
// through it is not delayed to line up with slow plugins elsewhere (FL 24.2's per-track low-latency monitoring).
enum class LatencyBypass : std::uint8_t
{
    never,
    always,
    whenRecordingOrStopped // FL's "When recording or not playing"
};

// How an insert's stereo output reaches its hardware output target (LYNX extra, REA round 156): a stereo pair of
// device channels, or one channel alone fed with the sum (L+R)/2, the left side only or the right side only.
enum class OutputMode : std::uint8_t
{
    stereo,
    monoSum,
    monoLeft,
    monoRight
};

// MixerTrack::outputChannel values besides a device channel (0-based)
constexpr int outputDefault = -2; // Master: the device's first output pair; inserts: none (they reach it via the Master)
constexpr int outputNone = -1;    // no hardware output (the Master too)

struct MixerTrack
{
    std::string name;
    float volume = unityFader; // 0..1; gain = volume / unityFader
    float pan = 0.0f;          // -1..1
    bool muted = false;
    bool solo = false;
    std::vector<EffectSlot> effects; // up to maxEffectSlots, processed in order
    bool toMaster = true;            // inserts: output to Master (besides the sends)
    float masterLevel = 1.0f;        // level of that route to Master (linear gain, 0..maxFaderGain; FL's route knob)
    float stereo = 0.0f;             // separation: -1 = widest, 0 = normal, +1 = mono (as in FL)
    std::uint32_t colour = 0;        // 0xFFRRGGBB; 0 = theme default
    bool invertPhase = false;
    bool swapChannels = false;
    std::vector<MixerSend> sends;    // to other inserts
    TrackEq eq;
    // external audio input (FL's "Audio input source"): the device's input channel `inputChannel` (0-based;
    // -1 = none) and, when `inputStereo`, the next one too
    int inputChannel = -1;
    bool inputStereo = true;
    InputMonitor monitor = InputMonitor::automatic;
    // record safe (round 163, as Pro Tools / Nuendo): the insert can't be armed by accident (saved)
    bool recordSafe = false;
    // an FL project's input (.flp 0x9A, REA round 157c): the index in FL's list of its own device's inputs, which only
    // the open device can turn into a channel — AudioEngine::setSong resolves it (flInputSource) when the project opens,
    // if inputChannel is still -1. -1 = none. Not saved.
    int flInput = -1;
    // hardware output target (FL's "Audio output target" .flp 0x93; mono channels are a LYNX extra, REA round 156):
    // outputDefault, outputNone or the first device output channel; an insert with a target still feeds the
    // Master (and its sends) as in FL — the hardware output is a parallel path
    int outputChannel = outputDefault;
    OutputMode outputMode = OutputMode::stereo;
    // FL's mixer track menu "CPU › Allow threaded processing" (REA round 134): off = this insert always runs on the
    // audio thread itself (as do inserts holding a plugin whose own option is off)
    bool threaded = true;
    // FL's mixer track menu "Group › Separator" (REA round 146a; .flp insert flag 0x400): a gap is drawn on the left of
    // this track; a run of tracks between two separators is a group ("Create group", "Select › Group", "Auto color group")
    bool separator = false;
    // FL's "Layout › Dock to" (.flp insert flags bits 6-7 = dock 0 left / 1 middle / 2 right): the strip is shown in the
    // left block, the middle or the right block of the mixer
    MixerDock dock = MixerDock::middle;
    // FL's track latency menu (REA round 165f-mixer, .flp 0xEC float at +0): a manual latency in ms (−5000..5000) added
    // to the track's output latency in the PDC plan, as if its effects had it: positive = the other paths wait for this
    // one (it plays earlier), negative = this track is delayed
    float latencyMs = 0.0f;
    LatencyBypass latencyBypass = LatencyBypass::never;
    // "Compensate for master latency" (.flp 0xEC flag 0x4000 = off): with a hardware output target, that output is
    // delayed to line up with the Master's (PDC total)
    bool compensateMasterLatency = true;
};

constexpr float maxTrackLatencyMs = 5000.0f; // FL's clamp (FUN_012a7230)

struct Note
{
    std::int64_t start = 0;   // ticks
    std::int64_t length = ticksPerStep;
    int channel = 0;
    int key = 60;             // MIDI; 60 = C4 (FL's C5)
    float velocity = 0.78f;   // 0..1
    float pan = 0.0f;         // -1..1 (per note, as in FL)
    float finePitch = 0.0f;   // cents of a semitone, -120..120
    bool muted = false;       // disabled note (does not play, stays in the pattern)
    bool slide = false;       // glides the sounding note to this pitch (without retriggering), as in FL
    bool porta = false;       // inverts the channel's porta on this note (the note glides from the previous one; as in FL)
    float release = 0.5f;     // release velocity 0..1 (Note inspector; stored, not yet used by the instruments)
    float modX = 0.5f, modY = 0.5f; // two free per-note modulation values 0..1 (stored, not yet routed)
    int colour = 0;           // note colour group 0..15 (FL's note colours; .flp note byte 'extra' bits 0-3)
    int group = 0;            // note group (FL's Group, Shift+G: clicking one note selects the group); 0 = none
};

constexpr int noteColourCount = 16;

// Swing delay (FL's formula, docs/analise/REA_rodada3.md §3): only notes whose nearest step
// is odd move, with triangular weight by distance to the grid; half a step at swing 1.
std::int64_t swingDelay (std::int64_t start, float swing, float channelMix = 1.0f) noexcept;

// What an automation clip controls. Values always 0..1 (like the model's knobs).
enum class AutomationTargetKind : std::uint8_t
{
    none,          // target removed (the clip goes silent)
    channelVolume, // index = channel
    channelPan,    // index = channel (0..1 -> -1..1)
    channelParam,  // index = channel with plugin; param = plugin parameter index
    mixerVolume,   // index = mixer track (0 = Master)
    mixerPan,      // index = track
    effectParam,   // index = track, slot, param (built-in effect or plugin)
    tempo,         // song tempo: 0..1 = minTempo..maxTempo BPM
    effectMix,     // index = track, slot: slot dry/wet (0..1)
    mixerSend      // index = source insert, slot = destination (0 = Master): the route's level knob (fader curve; issue #12)
};

constexpr double minTempo = 10.0, maxTempo = 522.0; // FL's range
inline double tempoFromValue (float v) noexcept { return minTempo + (maxTempo - minTempo) * (double) v; }
inline float valueFromTempo (double bpm) noexcept { return (float) ((bpm - minTempo) / (maxTempo - minTempo)); }

struct AutomationTarget
{
    AutomationTargetKind kind = AutomationTargetKind::none;
    int index = 0;
    int slot = 0;
    int param = 0;

    bool operator== (const AutomationTarget& o) const noexcept
    {
        return kind == o.kind && index == o.index && slot == o.slot && param == o.param;
    }
};

// Event editor (round 83): a pattern's own parameter events, like FL's pattern events (.flp 0xDF). Each event
// holds its value (0..1, like automation) until the next one; while the pattern plays they drive the target.
struct PatternEvent
{
    std::int64_t tick = 0; // from the start of the pattern
    float value = 0.5f;    // 0..1
};

struct PatternEvents
{
    AutomationTarget target;
    std::vector<PatternEvent> events; // sorted by tick, one per tick

    // Value at `tick` (held from the previous event; before the first = the first value).
    float valueAt (double tick) const noexcept;
};

struct Pattern
{
    std::string name;
    std::vector<Note> notes;  // sorted by start (kept by sortNotes)
    std::int64_t lengthTicks = 0; // 0 = automatic (rounds up to bars)
    std::uint32_t colour = 0;     // 0xFFRRGGBB; 0 = automatic (by index)
    std::vector<PatternEvents> events; // event editor lanes (one per target)

    void sortNotes();
    std::int64_t effectiveLength (int beatsPerBar) const noexcept;

    bool hasStep (int channel, int step) const noexcept;
    void toggleStep (int channel, int step);
};

enum class ClipKind : std::uint8_t
{
    pattern,
    audio,
    automation
};


struct AutomationPoint
{
    std::int64_t tick = 0; // from the start of the automation
    float value = 0.5f;    // 0..1
    float tension = 0.0f;  // curve to the next point: -1..1 (0 = straight)
};

struct Automation
{
    std::string name;
    AutomationTarget target;
    std::vector<AutomationPoint> points; // sorted by tick (kept by the editor)

    // Live source (FL controller link, REA_rodada14 §7): the value comes from output `output` of the
    // controller effect in slot (track, slot), passed through the curve (points evenly spaced over 0..1; empty =
    // direct). Uses neither clips nor points.
    struct LiveSource
    {
        int track = -1, slot = 0, output = 0;
        int channel = -1; // instead of an effect: this channel's "controller" instrument
    } source;
    std::vector<float> curve;
    bool isLive() const noexcept { return source.track >= 0 || source.channel >= 0; }
    float mapLive (float v) const noexcept; // applies the curve

    // Value at `tick` (before the first point = first value; after the last = last).
    float valueAt (double tick) const noexcept;
    std::int64_t length() const noexcept; // tick of the last point (minimum 1)
};

// A block in the Playlist: a section of a pattern, or a section of an audioClip channel's audio.
struct Clip
{
    int pattern = 0;
    int track = 0;
    std::int64_t start = 0;   // ticks in the song
    std::int64_t length = 0;  // ticks
    std::int64_t offset = 0;  // how much of the start of the pattern/audio is skipped (ticks)
    float offsetFraction = 0.0f; // audio clips: sub-tick part of `offset` (-0.5..0.5 ticks; imported FL offsets in ms)
    ClipKind kind = ClipKind::pattern;
    int channel = -1;         // audio clips: channel (audioClip) that holds the sample
    int automation = -1;      // automation clips: index into Song::automations
    std::int64_t fadeIn = 0, fadeOut = 0; // audio clips: fades (ticks)
    float gain = 1.0f;        // audio clips: clip volume
    float pan = 0.0f;         // audio clips: clip pan -1..1 (FL 2026's clip pan; REA round 153)
    bool muted = false;       // clip-level mute (the Mute tool); independent of the track mute

    std::int64_t end() const noexcept { return start + length; }
};

enum class PlayMode : std::uint8_t
{
    pattern, // loops the current pattern (like FL's "PAT")
    song     // plays the Playlist (like FL's "SONG")
};

// MIDI controller (CC) linked to a parameter (FL's "Link to controller").
struct MidiLink
{
    int cc = 0;       // 0..127
    int channel = -1; // MIDI channel 0..15, -1 = any
    AutomationTarget target;
};

// The links a controller move follows: the project's, then the global ones (settings, every project) whose target
// and controller the project does not link itself (REA round 146: a project link overrides a global one).
std::vector<MidiLink> linksWithGlobal (const std::vector<MidiLink>& project, const std::vector<MidiLink>& global);

enum class PlaylistTrackMode : std::uint8_t { unassigned, audio, instrument };

// FL's Performance mode track settings (round 165m: the track menu's "Performance settings" [DFM], stored in the
// .flp Playlist track record 0xEE at bytes 22…45 [DATA]; the numbers are FL's menu Tags = the stored values).
// How a clip launched in the performance zone (the Playlist before the "Start" marker) plays on its track.
struct TrackPerformance
{
    enum class Press : std::uint8_t { retrigger, holdStop, holdMotion, latch };
    enum class Motion : std::uint8_t { stay, oneShot, marchWrap, marchStay, marchStop, random, exclusiveRandom };
    // Trigger / position sync: off, 1/4, 1/2, 1, 2 or 4 beats, or auto (the clip's own length)
    enum class Sync : std::uint8_t { off, quarterBeat, halfBeat, beat, twoBeats, fourBeats, autoLength };

    Press press = Press::retrigger;
    Motion motion = Motion::stay;
    Sync triggerSync = Sync::fourBeats; // a launch waits for the next multiple of this (FL's default 4 beats)
    bool queue = false;                 // a launch waits for the playing clip to end
    bool tolerant = true;               // a launch a little after the grid point still starts on it (FL's default on)
    Sync positionSync = Sync::off;      // the launched clip starts at the song position modulo this, not at its start

    bool isDefault() const noexcept { return *this == TrackPerformance {}; }
    bool operator== (const TrackPerformance&) const = default;
};

// Header of a Playlist track.
struct PlaylistTrack
{
    std::string name;  // empty = "Track N"
    bool muted = false; // clips on this track don't play
    int height = 0;     // Playlist row height in px (0 = the default)
    std::uint32_t colour = 0; // header colour 0xFFRRGGBB (from the .flp or "Rename and color"); 0 = default
    // Track groups (FL's "Group with above track"): a grouped track belongs to the group of the nearest
    // ungrouped track above it (the group's header). `collapsed` on a header folds its members into thin rows.
    bool grouped = false;
    bool collapsed = false;
    // FL's "Track mode" (REA round 146a; .flp record 0xEE: u32 mode at byte 49 = 0 / 1 audio / 3 instrument, i32 mixer
    // track at 53, i32 channel at 57). An audio track is linked to a mixer track: audio clips placed on it play through
    // that track (their channel is routed there). An instrument track belongs to a channel (and its mixer track).
    PlaylistTrackMode mode = PlaylistTrackMode::unassigned;
    int mixerTrack = -1; // audio / instrument tracks: the linked mixer track (-1 = none)
    int channel = -1;    // instrument tracks: the channel
    TrackPerformance performance {}; // FL's Performance settings (round 165m)
};

// Splits a clip at `tick` (absolute): the original keeps the start and the return value is the rest.
// `tick` must be inside the clip.
Clip splitClip (Clip& clip, std::int64_t tick);

// A Playlist arrangement (FL allows several per project).
// Playlist marker (like FL's "time markers"). With numerator > 0 it is a time signature
// change: a new bar starts there, with numerator/denominator.
struct Marker
{
    std::int64_t tick = 0;
    std::string name;
    int numerator = 0, denominator = 4;
    // FL's marker action (the .flp's marker type, REA_rodada6 §5 / round 139): 0 = plain marker, 9 "Start recording"
    // (punch in), 10 "Stop recording" (punch out); other FL actions are kept as their number (no effect in LYNX)
    int action = 0;
    static constexpr int punchIn = 9, punchOut = 10;
    static constexpr int songStart = 5; // FL's "Start": the song starts here; Performance mode's zone ends here

    bool isSignature() const noexcept { return numerator > 0; }
    bool operator== (const Marker&) const = default;
};

// A bar on the ruler: where it starts, how many beats (quarter notes) it has and its number (1, 2, ...).
struct Bar
{
    std::int64_t tick = 0, length = 0;
    int number = 1;
};

struct Arrangement
{
    std::string name;
    std::vector<Clip> clips;               // empty for the current arrangement: its clips live in Song::playlist
    std::int64_t loopStart = 0, loopEnd = 0;
    std::vector<Marker> markers;           // sorted by tick (stay here even for the current arrangement)
};

// The project's credits (FL's Settings › Info page): shown and edited in Settings › Info, kept in .lynx, imported from .flp.
struct ProjectInfo
{
    std::string title, author, genre, url, comments;
    bool showOnOpen = false; // open Settings › Info when the project is opened

    bool empty() const noexcept { return title.empty() && author.empty() && genre.empty() && url.empty() && comments.empty() && ! showOnOpen; }
    bool operator== (const ProjectInfo&) const = default;
};

struct Song
{
    double bpm = 130.0;
    int beatsPerBar = 4;
    float swing = 0.0f; // 0..1: delays off-beat sixteenth notes (like the swing of FL's Channel Rack)
    std::vector<std::string> channelGroups; // the Channel Rack's filter groups (FL's "All" dropdown), by name
    float masterPitch = 0.0f; // cents, -1200..1200: shifts every note of the built-in instruments (the toolbar's master pitch)
    ProjectInfo info;
    std::vector<ChannelDef> channels;
    std::vector<Pattern> patterns;
    int currentPattern = 0;

    std::vector<Clip> playlist;
    PlayMode mode = PlayMode::pattern;
    std::int64_t loopStart = 0, loopEnd = 0; // looped section in the Playlist (ticks); loopEnd <= loopStart = off

    // Arrangements: `playlist` and the loop above belong to the current one; the others are stored here.
    std::vector<Arrangement> arrangements { Arrangement { "Arrangement 1", {}, 0, 0, {} } };
    int currentArrangement = 0;

    void selectArrangement (int index);
    int addArrangement (const std::string& name, bool copyCurrent = false); // returns the index (already selected)
    void removeArrangement (int index);
    // Applies `fn` to all targets (automations and MIDI controllers).
    template <typename Fn>
    void forEachTarget (Fn&& fn)
    {
        for (auto& a : automations)
            fn (a.target);
        for (auto& l : midiLinks)
            fn (l.target);
        for (auto& v : initialValues)
            fn (v.target);
        for (auto& p : patterns)
            for (auto& lane : p.events)
                fn (lane.target);
    }
    void removeDeadMidiLinks();

    // Applies `fn` to all clip lists (the current one and the stored ones).
    template <typename Fn>
    void forEachClipList (Fn&& fn)
    {
        fn (playlist);
        for (std::size_t i = 0; i < arrangements.size(); ++i)
            if ((int) i != currentArrangement)
                fn (arrangements[i].clips);
    }

    bool hasLoop() const noexcept { return loopEnd > loopStart; }

    // FL's Performance mode (Playlist › Options › Performance mode, Ctrl+P; round 165m): the clips before the first
    // "Start" marker (the performance zone) don't play with the song; they are launched live (click, controller pads)
    // and loop on their track by its Performance settings, while the song plays from the Start marker on.
    bool performanceMode = false;
    // The end of the performance zone: the first Start marker's tick, or 0 (no zone) without one / when off.
    std::int64_t performanceZoneEnd() const noexcept;
    bool inPerformanceZone (const Clip& clip) const noexcept { return performanceMode && clip.start < performanceZoneEnd(); }

    // Markers of the current arrangement.
    std::vector<Marker>& markers();
    const std::vector<Marker>& markers() const;
    void addMarker (Marker marker); // keeps the order

    // Bars that play in [from, to), taking the markers' time signature changes into account
    // (before the first one, beatsPerBar/4 applies).
    std::vector<Bar> barsBetween (std::int64_t from, std::int64_t to) const;
    Bar barAt (std::int64_t tick) const;

    std::vector<MixerTrack> mixer; // [0] = Master; empty = everything straight to the output
    // A/B of the LYNX substitutes (round 113): when set, every substitute slot plays as bypassed (the project's own
    // effects and the hosted plugins still play). Saved with the project.
    bool substitutesBypassed = false;
    // Clean mixer (round 163, Options › Mixer): the inserts have no built-in processing — the per-track EQ is bypassed
    // (its settings are kept, nothing is computed) and hidden from the mixer. Saved with the project.
    bool cleanMixer = false;
    // whether an effect slot processes: switched on, and not a substitute while the substitutes are bypassed
    bool slotActive (const EffectSlot& slot) const noexcept { return slot.enabled && ! (substitutesBypassed && slot.isSubstitute()); }
    std::vector<Automation> automations;
    std::vector<PlaylistTrack> playlistTracks; // index = track; tracks past the end use the default
    std::vector<MidiLink> midiLinks;
    // FL's "initialized controls" (.flp 0xD8; the parameter menu's "Init song with this position", REA round 146):
    // values the targets are reset to whenever the song starts playing in song mode (from any position), before
    // the automation clips take over at their own position.
    struct InitialValue
    {
        AutomationTarget target;
        float value = 0.0f; // 0..1
    };
    std::vector<InitialValue> initialValues;
    // Sets (replaces) / removes the target's initial value; the index into initialValues or -1.
    void setInitialValue (const AutomationTarget& target, float value);
    void removeInitialValue (const AutomationTarget& target);
    int findInitialValue (const AutomationTarget& target) const noexcept;
    // Resets the model's targets to their initial values; the plugin parameters among them (which don't live in
    // the model) are passed to `plugin` when given.
    void applyInitialValues (const std::function<void (const AutomationTarget&, float)>& plugin = {});

    bool isTrackMuted (int track) const noexcept
    {
        return track >= 0 && track < (int) playlistTracks.size() && playlistTracks[(std::size_t) track].muted;
    }

    static std::vector<MixerTrack> defaultMixer (int inserts = 16);
    // Whether anything uses insert `t`: a channel, a send to or from it, effects, an input, a Playlist link, a custom
    // name, its fader / pan / mute / colour / EQ / output moved from the defaults or automation on it. Master: true.
    bool insertInUse (int t) const;
    // First insert nothing uses (insertInUse false); 0 (Master) if none.
    int nextFreeInsert() const;
    // nextFreeInsert, but when every insert is in use a new one is added (up to maxMixerTracks), like FL where a new
    // channel takes the next of its 125 inserts (round 162); 0 (Master) only when the mixer is full.
    int claimFreeInsert();

    // End of the last clip, rounded up to a bar (minimum 1 bar; with no clip, the current pattern's length).
    std::int64_t songLength() const noexcept;

    // Removes a pattern and fixes the clips and the current index.
    void removePattern (int index);

    // Removes a channel along with its notes and audio clips, and reindexes the others.
    // Automations of that channel lose their target.
    void removeChannel (int index);

    // Routing between inserts. canRoute = valid destination that doesn't create a cycle.
    bool canRoute (int from, int to) const;
    // Enables/disables the send from -> to (to == 0 = Master). Does nothing if it would create a cycle.
    void setRoute (int from, int to, bool enabled, float level = 1.0f);
    bool routes (int from, int to) const noexcept;
    // Level of the route from -> to (to == 0 = Master: `masterLevel`), a linear gain 0..maxFaderGain; 0 without a route.
    float routeLevel (int from, int to) const noexcept;
    // Sets the level of an existing route (FL's route knob); does nothing if there is no such route.
    void setRouteLevel (int from, int to, float level);
    // Inserts (1..N) in processing order: senders come before receivers.
    std::vector<int> mixerOrder() const;
    // Tracks audible with the current solo: the soloed ones, those that feed them and the path to Master.
    // Without solo, all of them. Index = track.
    std::vector<bool> soloAudible() const;

    // Swaps/removes effect slots keeping automations pointing at the right effect.
    void swapEffectSlots (int track, int a, int b);
    void removeEffectSlot (int track, int slot);

    // Removes an automation and its clips.
    void removeAutomation (int index);

    // Current value (from the model, without automation) of the target, 0..1; -1 if the target doesn't exist.
    float targetValue (const AutomationTarget& target) const noexcept;
    // Changes the target's value in the model (0..1). Plugin parameters don't live here: returns false.
    bool setTargetValue (const AutomationTarget& target, float value) noexcept;

    // Value of an automation at a song position (the same one playback uses); NaN without clips.
    float automationValueAt (int automation, double tick) const;
    // Tempo at `tick`, taking tempo automation into account (in song mode).
    double tempoAt (double tick) const;
    // Duration in seconds of the first `ticks` of the song, integrating the tempo automation.
    double secondsForTicks (std::int64_t ticks) const;

    static Song makeDefault();
};

// The Fruity Send slots of insert `track` that send to another existing insert, in slot order (only the first
// maxEffectSlots slots run): {slot index, destination insert}. The renderer, the PDC plan and the routing order
// all enumerate them this way.
std::vector<std::pair<int, int>> fruitySendsOf (const Song& song, int track);

// Pattern clips follow their pattern's length, like FL's: after an edit, the clips of a pattern whose length
// changed get the new length when they had the old one and start at the pattern's start (an auto-sized clip;
// a clip resized or cut by hand keeps its length). `before` = patternLengths (song) before the edit.
// (Issue #4: a pattern set to 2 bars still looped 1 bar in Song mode.)
std::vector<std::int64_t> patternLengths (const Song& song);
void followPatternLengths (const std::vector<std::int64_t>& before, Song& song);

// Copy of the song ready for export: always in song mode (the current pattern becomes a clip
// if `wholeSong` is false or the Playlist is empty), with `tailTicks` of silence at the end
// so releases and reverbs ring out without the song restarting. `musicLength` receives the end of the
// musical part (without the tail), in ticks.
Song makeExportSong (const Song& song, bool wholeSong, std::int64_t tailTicks, std::int64_t& musicLength);

// Copy of the song in which only the (post-fader) output of insert `track` is heard, straight to Master
// without effects or fader: this is what gets rendered to "turn the insert into audio".
Song makeBounceSong (const Song& song, int track);

// After rendering the insert to `samplePath` (`length` ticks from the start): creates an
// audio clip channel on a new insert with the same routing, puts the clip in the Playlist and mutes
// the original insert. Returns the new insert.
int applyBounce (Song& song, int track, const std::string& samplePath, std::int64_t length);
} // namespace lynx
