#pragma once

#include <atomic>

// Built-in instruments: synthesized drums and synth, and a sampler. They let the
// DAW make sound before plugin hosting exists and play imported projects.
// Everything has fixed capacity: noteOn/render never allocate.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/Song.h"

namespace lynx
{
namespace vfx
{
class Processor;
}
class Instrument
{
public:
    virtual ~Instrument() = default;

    virtual void prepare (double sampleRate) noexcept = 0;
    virtual void noteOn (int key, float velocity) noexcept = 0;
    // Starts the note already `startSeconds` in (only meaningful for samples; others ignore it).
    virtual void noteOnAt (int key, float velocity, double /*startSeconds*/) noexcept { noteOn (key, velocity); }
    virtual void noteOff (int key) noexcept = 0;
    virtual void allNotesOff() noexcept = 0;
    // A cut group's "cut" (another note of the group starts): the sound stops; a Patcher's VFX nodes keep the notes
    // they hold, as FL's do (round 165d: FL's Arpeggiator script on a "cut itself" Patcher keeps arpeggiating a chord
    // whose notes start apart [EXEC: FL render, REA_rodada165d §4])
    virtual void cut() noexcept { allNotesOff(); }
    // A note on a given MIDI channel (0…15), from a MIDI Out channel routed to this one's MIDI input port. Only
    // hosted plugins see the channel; the others play it as a plain note.
    virtual void noteOnMidi (int key, float velocity, int /*midiChannel*/) noexcept { noteOn (key, velocity); }
    virtual void noteOffMidi (int key, int /*midiChannel*/) noexcept { noteOff (key); }
    // what arrives on MIDI port `port` (a MIDI Out on that port): a plugin takes it as noteOnMidi; a Patcher sends it
    // to the nodes cabled to its "MIDI port N" pin (round 162)
    virtual void noteOnPort (int /*port*/, int key, float velocity, int midiChannel) noexcept { noteOnMidi (key, velocity, midiChannel); }
    virtual void noteOffPort (int /*port*/, int key, int midiChannel) noexcept { noteOffMidi (key, midiChannel); }
    // A short MIDI message (program change, bank select…) for the next block. Only hosted plugins take it.
    virtual void midiMessage (const std::uint8_t* /*bytes*/, int /*size*/) noexcept {}
    // A MIDI controller's pitch wheel on this channel (round 162, issue #9), already scaled by the channel's pitch
    // range (FL's Misc › Pitch range, ±2 semitones by default): every sounding and new note is bent by `semitones`
    // until the wheel moves again. Audio thread. Hosted plugins get the raw MIDI pitch bend instead (midiMessage).
    virtual void pitchBend (float /*semitones*/) noexcept {}
    // A controller (CC 0…127, `value` 0..1) played live into a native instrument (the mod wheel is CC 1). Ignored by
    // engines without MIDI controllers; hosted plugins get the raw message (midiMessage).
    virtual void controlChange (int /*cc*/, float /*value*/) noexcept {}
    // UI thread, before the instrument reaches the audio thread: a sampler whose sample is stretched keeping its pitch
    // allocates its per-voice pitch shifters (keys transpose it without changing its length, like FL's Stretch modes).
    virtual void prepareKeyShift (double /*sampleRate*/) {}

    // Adds (does not overwrite) n samples into left/right, with the gains already applied.
    virtual void render (float* left, float* right, int n, float gainLeft, float gainRight) noexcept = 0;
    // Automated parameter (plugins). 0..1. Audio thread.
    virtual void setParameter (int /*index*/, float /*value*/) noexcept {}
    // Latency the instrument introduces (plugins), in samples. For PDC.
    virtual int latencySamples() const noexcept { return 0; }
    // "Slide" note: sounding notes glide to `key` over `samples`, without retriggering. Ignored if unsupported.
    virtual void slideTo (int /*key*/, float /*cents*/, float /*samples*/) noexcept {}
    // Audio clip: fades and in-clip position of the next note (before noteOn), in samples.
    virtual void setNextClipFades (float /*elapsed*/, float /*remaining*/, float /*fadeIn*/, float /*fadeOut*/) noexcept {}
    // Pan, fine tuning and "porta" (inverts the channel's porta) of the next note (called right before
    // noteOn). Ignored if unsupported.
    virtual void setNextNoteExpression (float /*pan*/, float /*cents*/, bool /*porta*/ = false) noexcept {}
    // The next note's volume factor (FL's echo delay, REA round 146b); false when the instrument does not scale its
    // voices (the caller then scales the velocity).
    virtual bool setNextNoteGain (float /*gain*/) noexcept { return false; }
    // The next note's Mod X / Y (0..1, 0.5 = none; the note's own, moved by the echo delay and the key / velocity
    // tracking, REA round 151), called right before noteOn; and the release velocity of the next noteOff (0..1),
    // right before it. Ignored if unsupported (hosted plugins: FL's wrapper MIDI options).
    virtual void setNextNoteModulation (float /*modX*/, float /*modY*/) noexcept {}
    virtual void setNextNoteRelease (float /*velocity*/) noexcept {}
    // Playback stopped: the held notes stop (hosted plugins may send "All Notes Off" or only those notes' note offs:
    // FL's wrapper option). The panic button: everything stops (a hosted plugin may also be reset).
    virtual void transportStopped() noexcept { allNotesOff(); }
    virtual void panic() noexcept { allNotesOff(); }
    // Channel settings (sampler, synth). UI thread, on every snapshot; the instrument stores them
    // so the audio thread can read without waiting.
    virtual void configure (const ChannelDef&, double /*bpm*/) noexcept {}
    // Control output ("controller" instruments), 0..1. Audio thread.
    virtual float controlOutput (int /*output*/) const noexcept { return 0.0f; }
    // Song position (ticks) at the start of the block, for the global LFOs. Audio thread.
    virtual void setSongPosition (double /*ticks*/) noexcept {}
    // No voice sounding: render would only produce silence (the mixer skips the channel until a note arrives).
    virtual bool idle() const noexcept { return false; }
    // The notes it sent out in its renders since the last call (a hosted plugin's MIDI output, FL's wrapper "Output
    // port", round 149): fills `out` with up to `max` and returns how many.
    struct MidiOutNote
    {
        int key = 60;
        float velocity = 0.0f; // 0 = note off
    };
    virtual int takeMidiOutput (MidiOutNote* /*out*/, int /*max*/) noexcept { return 0; }
    // Extra outputs to other mixer inserts (a generator Patcher's extra "To LYNX" outputs, REA_rodada141): the rack
    // calls beginChunk (n) before a chunk's render() calls, which then also add each extra output's audio (gains
    // applied) into auxOutputData (k, side) from the chunk's start; auxOutputTrack (k) = the insert it feeds.
    virtual void beginChunk (int /*n*/) noexcept {}
    virtual int auxOutputCount() const noexcept { return 0; }
    virtual int auxOutputTrack (int /*k*/) const noexcept { return -1; }
    virtual const float* auxOutputData (int /*k*/, int /*side*/) const noexcept { return nullptr; }
    // A VFX channel / node (round 164b, core/Vfx.h): its note processor (null for every other instrument)
    virtual vfx::Processor* noteProcessor() noexcept { return nullptr; }
    // The next note's colour group (0…15, FL's note colours; called right before noteOn): a Patcher passes it to its
    // VFX nodes (VFX Color Mapper, VFX Envelope). Ignored by the others.
    virtual void setNextNoteColour (int /*colour*/) noexcept {}
    // A property of the sounding voices of `key` changes while they sound (VFX Envelope, round 164b): `prop` is a
    // vfx::Prop (0 velocity 0..1 — the voice's volume relative to its note-on velocity is value / velocity at
    // note-on —, 1 pitch in cents from the key, 2 pan −1..1, 3 Mod X, 4 Mod Y). Ignored if unsupported.
    virtual void voiceProperty (int /*key*/, int /*prop*/, float /*value*/) noexcept {}
};

// Audio of an already-decoded sample (loaded off the audio thread). Short samples
// live in the vectors; long ones live in a memory-mapped cache file (the OS reads
// from disk on demand) and use the `mapped*` pointers.
struct SampleData
{
    std::vector<float> left, right; // empty right = mono
    double sampleRate = 44100.0;
    // the file's own loop (WAV smpl chunk, first loop): frames [loopStart, loopEnd), −1 = none
    std::int64_t loopStart = -1, loopEnd = -1;

    const float* mappedLeft = nullptr;
    const float* mappedRight = nullptr; // nullptr = mono
    std::size_t mappedFrames = 0;
    std::shared_ptr<const void> mapping; // owner of the mapping

    bool isMapped() const noexcept { return mappedLeft != nullptr; }
    std::size_t frames() const noexcept { return isMapped() ? mappedFrames : left.size(); }
    const float* leftData() const noexcept { return isMapped() ? mappedLeft : left.data(); }
    const float* rightData() const noexcept
    {
        if (isMapped())
            return mappedRight != nullptr ? mappedRight : mappedLeft;
        return right.empty() ? left.data() : right.data();
    }
    bool isStereo() const noexcept { return isMapped() ? mappedRight != nullptr : ! right.empty(); }
};

// Offline rendering (export) in progress. Instruments that, like FL's plugins, have separate render-quality settings
// switch to them while it is set (Sytrus: oversampling and envelope accuracy, REA_rodada31 §1.2); they read it in
// prepare()/configure(), so set it before preparing the graph for the render and clear it afterwards.
void setOfflineRendering (bool on) noexcept;
bool offlineRendering() noexcept;
// FL's "HQ for all plugins" during an export (REA round 134): effects with an HQ switch use HQ whatever it is set to.
void setHqForAllPlugins (bool on) noexcept;
bool hqForAllPlugins() noexcept;

// Where the song is, for hosted plugins (tempo-synced effects, LFO/volume shapers, arpeggiators read the host's
// transport): the renderer publishes it at the start of each chunk it renders.
struct HostTimeline
{
    std::atomic<double> tick { 0.0 };          // song position in LYNX ticks (ppq per quarter note)
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 }; // the playback loop in LYNX ticks (round 151: "Send loop position")
    std::atomic<bool> looping { false };
    std::atomic<double> bpm { 120.0 };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool> playing { false };
    std::atomic<int> beatsPerBar { 4 };
};
HostTimeline& hostTimeline() noexcept;

// `sample` is only used by the sampler; without a sample the sampler is silent.
// The Sampler's Gain knob (SamplerSettings::gainDb) as a factor: dB clamped to -24..+24, exactly 1 at 0 dB.
float samplerGain (float db) noexcept;

std::unique_ptr<Instrument> makeInstrument (InstrumentKind kind, std::shared_ptr<const SampleData> sample = nullptr);

// Multi-sample sampler: each zone with its sample already loaded (nullptr = missing file).
struct LoadedZone
{
    SampleZone zone;
    std::shared_ptr<const SampleData> sample;
};
std::unique_ptr<Instrument> makeZonedSampler (std::vector<LoadedZone> zones);

// Cheap, deterministic white noise (xorshift), identical on every platform.
struct Noise
{
    std::uint32_t state = 0x12345678u;

    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (std::int32_t) state * (1.0f / 2147483648.0f);
    }
};
} // namespace lynx
