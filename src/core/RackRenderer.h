#pragma once

// Ties Song + instruments + scheduler together: given the transport state, renders
// a block of stereo audio with notes triggered at the exact sample.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Arpeggiator.h"
#include "core/ChannelMiscFx.h"
#include "core/VoiceLimit.h"
#include "core/Biquad.h"
#include "core/Effects.h"
#include "core/HardwareOutput.h"
#include "core/Instruments.h"
#include "core/LiveEventQueue.h"
#include "core/LiveInput.h"
#include "core/Meters.h"
#include "core/Pdc.h"
#include "core/Scheduler.h"
#include "core/Song.h"

namespace lynx
{
// Immutable snapshot published by the UI. Instruments are shared between
// consecutive snapshots so voices are not cut off when the song is edited.
struct RackState
{
    Song song;
    std::vector<std::shared_ptr<Instrument>> instruments; // one per channel
    std::vector<std::vector<std::shared_ptr<Effect>>> effects; // [mixer track][slot]
    double sampleRate = 44100.0; // rate the new instruments were prepared with

    // Automation, preprocessed so the audio thread only does table lookups.
    // Indices into song.automations, or -1 = no automation.
    static constexpr int maxAutomations = 1024;
    std::vector<std::vector<int>> automationClips; // per automation: clips in the Playlist
    std::vector<int> channelVolumeAuto, channelPanAuto;
    std::vector<int> mixerVolumeAuto, mixerPanAuto;
    std::vector<std::vector<std::array<int, maxEffectParams>>> effectParamAuto; // [track][slot][param] (built-in)
    std::vector<std::vector<int>> effectMixAuto;                                 // [track][slot]: dry/wet
    std::vector<int> liveAutomations;                                            // automations with a live source (controllers)

    // pattern events (event editor): each lane uses an automation slot after the song's automations
    struct EventLane
    {
        int pattern = 0, lane = 0, slot = 0;
    };
    std::vector<EventLane> eventLanes;
    std::vector<std::vector<int>> patternClips; // per pattern: its clips in the Playlist, by start

    struct PluginParamBinding
    {
        int automation = 0;
        int channel = -1;       // channel instrument, or
        int track = 0, slot = 0; // effect in the slot
        int param = 0;
    };
    std::vector<PluginParamBinding> pluginParams;
    bool hasAutomation = false;
    ScheduleIndex schedule; // index to schedule the Playlist without scanning every note
    int tempoAutomation = -1; // tempo automation, if any

    // PDC: per-edge delays (nullptr = no delay) and the latencies used to build the plan.
    PdcPlan pdc;
    std::vector<std::shared_ptr<DelayLine>> channelDelays;
    std::vector<std::vector<std::shared_ptr<DelayLine>>> sendDelays; // [track][k] like PdcPlan::sendDelay
    std::vector<int> latencies; // channels, then slots of all tracks

    // Inserts with a hardware output target (REA round 156), each delayed by what it lacks to line up with the
    // Master output (pdc.total − outLat[track]; nullptr = no delay). Built with the PDC plan.
    struct HwRoute
    {
        int track = 0;
        int channel = 0;
        OutputMode mode = OutputMode::stereo;
        std::shared_ptr<DelayLine> delay;
    };
    std::vector<HwRoute> hwRoutes;

    // Current latencies of instruments and effects, in the same layout as `latencies`.
    std::vector<int> currentLatencies() const;
    void buildPdc (const RackState* previous);

    // Arpeggiators of channels with arpeggio on (nullptr for the others); reused across snapshots.
    std::vector<std::shared_ptr<Arpeggiator>> arpeggiators;
    // Echo delays of channels with FL's Misc › Echo delay on (nullptr for the others; REA round 146b).
    std::vector<std::shared_ptr<channelmisc::NoteEcho>> echoes;

    // Cut groups: channels each channel cuts when it plays (cutBy == its cutGroup).
    std::vector<std::vector<int>> cutTargets;
    bool hasCutGroups = false;

    // Mixer: insert processing order and which ones sound with the current solo.
    std::vector<int> mixerOrder;
    std::vector<char> mixerAudible;

    // "Pull" model: each track sums the (post-fader) outputs of the tracks that send to it.
    struct MixerInput
    {
        int source = 0;
        int delay = 0; // index into sendDelays[source]
        float level = 1.0f;
        int tap = -1;  // ≥ 0: a Fruity Send slot's signal (sendTaps[tap]) instead of the source's output
    };
    std::vector<std::vector<MixerInput>> mixerInputs; // [track]; [0] = Master

    // Fruity Send slots (FL's mid-chain sends to another insert): what each one sends is collected in its own
    // buffer while its insert runs, and pulled by the destination like a route.
    static constexpr int maxSendTaps = 64;
    struct SendTap
    {
        int track = 0, slot = 0, target = 0;
    };
    std::vector<SendTap> sendTaps;
    std::vector<std::vector<int>> slotTap; // [track][slot]: index into sendTaps, or −1
    std::vector<std::vector<int>> mixerLayers;        // inserts with no dependencies among them, in order
    // "Allow threaded processing" off (PluginRef::threaded, REA round 134): those channels / inserts always run on the
    // calling (audio) thread, the others may go to the workers. Same layers as mixerLayers.
    std::vector<int> threadedChannels, audioThreadChannels;
    std::vector<std::vector<int>> threadedLayers, audioThreadLayers;
    bool parallel = false;                            // worth splitting across threads
    void buildMixerGraph();

    void buildAutomationTables();
    // The automation slots that drive `target` in a snapshot of `song` (its automations, then its pattern event
    // lanes, numbered as buildAutomationTables numbers them). REA round 146: a touched control overrides them.
    static void slotsOfTarget (const Song& song, const AutomationTarget& target, std::vector<int>& out);

    // MIDI Out channels (REA_rodada133 §4): which channels each one's notes and controls reach (the plugin channels
    // whose MIDI input port is its port), and the MIDI channel it sends on. Indexed by channel; empty / −1 otherwise.
    std::vector<std::vector<int>> midiTargets;  // [MIDI Out channel] → plugin channels
    std::vector<std::vector<int>> midiSources;  // [plugin channel] → MIDI Out channels
    std::vector<int> midiOutChannel;            // [channel] MIDI channel 0…15 of a MIDI Out, −1 = not one
    bool hasMidiRoutes = false;
    // The same routes with the port and MIDI channel of each (round 162: Patchers take MIDI ports through their "From
    // LYNX" MIDI port pins, and a generator Patcher sends on several ports / channels through MIDI Out nodes)
    struct MidiRoute
    {
        int other = 0;        // [source] → the target channel; [target] → the source channel
        int port = 0, midiChannel = 0;
    };
    std::vector<std::vector<MidiRoute>> midiRoutesOut; // [source channel] → targets
    std::vector<std::vector<MidiRoute>> midiRoutesIn;  // [target channel] → sources
    struct EffectMidiRoute
    {
        int track = 0, slot = 0, port = 0, midiChannel = 0;
    };
    std::vector<std::vector<EffectMidiRoute>> effectMidiRoutes; // [source channel] → mixer-slot Patchers on its port
    bool hasEffectMidiRoutes = false;
    // FL's wrapper "Output port" (round 149): [plugin channel] → the plugin channels whose input port is its output port
    std::vector<std::vector<int>> midiOutTargets;
    bool hasMidiOutputs = false;
    void buildMidiRoutes();

    // Loads (or fetches from cache) a file's audio; nullptr on failure.
    using SampleProvider = std::function<std::shared_ptr<const SampleData> (const std::string& path)>;

    // The app creates plugin instruments/effects (the core does not know JUCE). nullptr = failed
    // (missing plugin): the channel goes silent / the slot becomes bypass.
    using PluginInstrumentFactory = std::function<std::shared_ptr<Instrument> (const PluginRef&, double sampleRate)>;
    using PluginEffectFactory = std::function<std::shared_ptr<Effect> (const PluginRef&, double sampleRate)>;

    // Sample stretched without changing pitch (ratio = new / original duration); cached in the app.
    using StretchedSampleProvider = std::function<std::shared_ptr<const SampleData> (const std::string& path, double ratio, StretchMethod method)>;

    struct Factories
    {
        SampleProvider samples;
        StretchedSampleProvider stretchedSamples;
        PluginInstrumentFactory pluginInstruments;
        PluginEffectFactory pluginEffects;
    };

    // Builds the next state reusing the instruments from `previous` when the
    // channel changed neither instrument nor sample, and the effects when the slot has the
    // same type. Runs on the UI thread (allocates).
    static std::unique_ptr<const RackState> build (Song song,
                                                   const RackState* previous,
                                                   double sampleRate,
                                                   const SampleProvider& samples = {});
    static std::unique_ptr<const RackState> build (Song song,
                                                   const RackState* previous,
                                                   double sampleRate,
                                                   const Factories& factories);
};

class WorkerPool;

// Whether a parallel container slot (FL's Patcher) can keep its built effect: same branches, children and links.
bool sameContainer (const EffectSlot& a, const EffectSlot& b);
// Time spent in each instrument and effect slot (the Plugin performance monitor, round 84). Counted only while
// `enabled` is on; nanoseconds of processing and of audio rendered, accumulated (the reader takes differences).
struct ProcessTimes
{
    static constexpr int channels = 1024;
    std::atomic<bool> enabled { false };
    std::array<std::atomic<std::uint64_t>, channels> channelNs {};
    std::array<std::atomic<std::uint64_t>, maxMixerTracks * maxEffectSlots> effectNs {}; // [track * maxEffectSlots + slot]
    std::atomic<std::uint64_t> audioNs { 0 };  // duration of the audio rendered while enabled
    std::atomic<std::uint64_t> wallNs { 0 };   // time the whole rack took for it
    // Live meters of the effects that have them (the mixing suite's gain reduction, REA round 96; LYNX Tune's pitch and
    // Facet EQ's dynamic gains, round 103; the dynamics suite's band gains and levels, round 108), always published:
    // [(track * maxEffectSlots + slot) * meterOutputs + output] = Effect::controlOutput (output) after the last block.
    static constexpr int meterOutputs = 8;
    std::array<std::atomic<float>, (std::size_t) maxMixerTracks * maxEffectSlots * meterOutputs> effectMeters {};

    // Audio of one effect slot for an editor's spectrum analyser (Facet EQ, round 103): the editor sets `key`
    // (track * maxEffectSlots + slot, −1 = none); the audio thread then writes the slot's input (pre) and output
    // (post), as mono mid samples, into the rings and advances `written`. Reads may tear; the analyser tolerates it.
    struct AnalyzerTap
    {
        static constexpr int size = 16384; // power of two
        std::atomic<int> key { -1 };
        std::atomic<std::uint32_t> written { 0 };
        std::atomic<double> sampleRate { 48000.0 };
        std::array<std::atomic<float>, size> pre {}, post {};
    };
    AnalyzerTap analyzer;

    static std::uint64_t now() noexcept
    {
        return (std::uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

class RackRenderer
{
public:
    ProcessTimes& processTimes() noexcept { return *times; }

    // Extra threads for channels and mixer layers (nullptr = everything on the audio thread).
    void setWorkerPool (WorkerPool* pool) noexcept { workers = pool; }

    // External audio input (REA_rodada88): inserts with an input source hear it (per their monitor mode) and
    // meter it. The caller fills `input` before each process() call on the same thread. Set before playback.
    void setLiveInput (LiveInput* input) noexcept { liveInput = input; }
    // Hardware output targets of the inserts (REA round 156): their post-fader output is added into `out`'s buses
    // while `out->active`. Set before playback.
    void setHardwareOutput (HardwareOutput* out) noexcept { hwOut = out; }

    // Allocates the mixer buffers: call off the audio thread.
    // Blocks larger than maxBlockSize are processed in pieces.
    void prepare (double sampleRate, int maxBlockSize = 2048);

    // Audio thread. left/right are overwritten. `live` (optional) carries notes
    // played live, applied at the start of the block.
    void process (const RackState* state,
                  bool playing,
                  double startTick,
                  int numSamples,
                  float* left,
                  float* right,
                  LiveEventQueue* live = nullptr,
                  MeterBank* meters = nullptr,
                  LiveEventQueue* moreLive = nullptr) noexcept;

    static void applyStereoForTest (const MixerTrack& t, float* l, float* r, int n) noexcept { applyStereo (t, l, r, n); }

    // Per-unit CPU profile (REA round 87): the time spent in each channel (instrument, notes, PDC) and in each insert
    // effect slot, accumulated while profiling is on. Off by default (it costs two clock reads per unit and chunk).
    // Written by the audio thread and the workers, read by any thread (relaxed counters, nanoseconds).
    static constexpr int maxChannels = 1024;
    struct UnitTimes
    {
        std::array<std::atomic<std::uint64_t>, maxChannels> channelNs {};
        std::array<std::atomic<std::uint64_t>, (std::size_t) maxMixerTracks * maxEffectSlots> effectNs {}; // [track * maxEffectSlots + slot]
        std::array<std::atomic<std::uint64_t>, maxChannels> channelMaxNs {}; // longest single call (spikes)
        std::array<std::atomic<std::uint64_t>, (std::size_t) maxMixerTracks * maxEffectSlots> effectMaxNs {};
        std::atomic<std::uint64_t> processNs { 0 }, processedSamples { 0 };
    };
    void setProfiling (bool on) noexcept { profiling.store (on, std::memory_order_relaxed); }
    bool isProfiling() const noexcept { return profiling.load (std::memory_order_relaxed); }
    const UnitTimes& unitTimes() const noexcept { return *units; }
    // Blocks in which an insert effect output NaN / inf and was muted and reset (all renderers, since start; round 111)
    static std::uint64_t nonFiniteEffectBlocks() noexcept;
    void resetUnitTimes() noexcept;

    // FL's touched control wins over its playing automation while recording (REA round 146): the slots set here
    // (RackState::slotsOfTarget) are not driven by automation clips / pattern events; the control's own value
    // plays. UI thread writes, audio thread reads (relaxed: a block late at worst).
    void setOverriddenSlots (const std::vector<int>& slots) noexcept;
    bool isSlotOverridden (int slot) const noexcept
    {
        return slot >= 0 && slot < RackState::maxAutomations
            && (overridden[(std::size_t) slot / 32].load (std::memory_order_relaxed) & (1u << ((unsigned) slot % 32))) != 0;
    }

    // Tempo used in the last block (changes with tempo automation). The transport advances with it.
    double currentBpm() const noexcept { return bpm; }

    // Events of the last block (for tests and diagnostics).
    const EventBuffer& lastEvents() const noexcept { return events; }

private:
    void processChunk (const RackState& state, bool playing, double startTick, int numSamples,
                       float* left, float* right, MeterBank* meters) noexcept;
    void renderChannel (const RackState& state, int channel, int n) noexcept;
    void processTrack (const RackState& state, int track, int n, MeterBank* meters) noexcept;
    void pullInputs (const RackState& state, int track, int n) noexcept;
    static void applyStereo (const MixerTrack& track, float* l, float* r, int n) noexcept;
    void deliverToEffects (const RackState& state, const NoteEvent& e) noexcept; // (round 162)
    float* channelBuffer (int channel, int side) noexcept;
    float* trackScratch (int track, int side) noexcept;
    void runEffects (const RackState& state, int track, int n) noexcept;
    void setSidechain (const RackState& state, const EffectSlot& slot, Effect& effect, int track) noexcept;
    void updateAutomation (const RackState& state, double startTick) noexcept;
    // ticks from `tick` to the next pattern event (or pattern clip edge) that changes a lane's value; +inf if none
    static double ticksToNextEvent (const RackState& state, double tick) noexcept;
    void applyPluginParams (const RackState& state, int firstSlot) noexcept; // bindings from this automation slot on
    float automated (int automation, float base) const noexcept;
    float* trackBuffer (int track, int channel) noexcept;

    double sampleRate = 44100.0;
    double bpm = 120.0;
    bool wasPlaying = false;
    double expectedTick = -1.0; // where the next block should start, to detect jumps
    bool resumeAudio = false;
    const RackState* lastState = nullptr;
    EventBuffer events;
    std::vector<float> mixBuffers; // maxMixerTracks * 2 * blockCapacity
    std::array<float, RackState::maxAutomations> automationValues {}; // NaN = not evaluated yet
    std::array<std::atomic<std::uint32_t>, RackState::maxAutomations / 32> overridden {}; // touched while recording
    std::atomic<bool> anyOverridden { false };
    void clearOverriddenValues() noexcept;
    std::array<float, maxMixerTracks * 2> lastTrackGain {}; // L/R gain of the previous block, for the ramp
    std::array<char, maxMixerTracks> trackActive {};        // the track output has sound in this block
    std::vector<float> tapBuffers;                          // RackState::maxSendTaps * 2 * chunkCapacity
    std::array<char, RackState::maxSendTaps> tapActive {};  // the Fruity Send slot sent something this block
    float* tapBuffer (int tap, int side) noexcept;
    std::array<char, maxChannels> channelSilent {};          // channel skipped in this chunk (idle, no notes)
    std::array<NoteMask, maxChannels> heldNotes {};          // keys held on each channel (effects that follow notes: LYNX Tune)
    std::array<VoiceOrder, maxChannels> voiceOrders {};      // sounding notes in order, for the channels' Max polyphony
    static constexpr int chunkCapacity = 512;               // chunks processed at once
    std::vector<float> channelBuffers;                      // maxChannels * 2 * chunkCapacity
    std::vector<float> trackScratchBuffers;                 // maxMixerTracks * 2 * chunkCapacity
    double chunkStartTick = 0.0, chunkTps = 1.0;            // of the chunk being rendered (for the arpeggiator)
    // live-played notes of channels with an arpeggiator, echo delay or key / velocity tracking go through the
    // channel's note path like the scheduled ones (FL, round 149); while stopped the arpeggiator runs on its own clock
    std::array<NoteEvent, 256> liveRouted {};
    int liveRoutedCount = 0;
    double liveTick = 0.0;
    bool routesLiveNotes (const RackState& state, int channel) const noexcept;
    void applyLiveControl (const RackState& state, const NoteEvent& e) noexcept;
    WorkerPool* workers = nullptr;
    LiveInput* liveInput = nullptr;
    HardwareOutput* hwOut = nullptr;
    std::vector<float> hwScratch; // 2 × blockCapacity
    void mixHardwareOutputs (const RackState& state, int n) noexcept;
    int liveInputOffset = 0; // of the chunk within the process() block
    void mixLiveInput (const RackState& state, bool playing, int n) noexcept;
    std::atomic<bool> profiling { false };
    std::unique_ptr<UnitTimes> units = std::make_unique<UnitTimes>();
    std::unique_ptr<ProcessTimes> times = std::make_unique<ProcessTimes>();

    struct SmartDisableState
    {
        const Effect* effect = nullptr;
        int silentSamples = 0;
    };
    std::vector<SmartDisableState> smartDisable; // [track * maxEffectSlots + slot]

    struct TrackEqState
    {
        TrackEq settings;
        std::array<Biquad, 3> bands;
        bool running = false;
    };
    std::vector<TrackEqState> trackEqs; // built-in EQ of each track
    double chunkTick = 0.0;             // start of the block (for the global LFOs)
    bool chunkPlaying = false;          // the song plays during the block (effects that follow the transport)
    void applyTrackEq (const MixerTrack& track, int t, bool bypassed, float* l, float* r, int n) noexcept;
    static constexpr float silenceLevel = 1.5849e-5f;  // -96 dBFS
    int blockCapacity = 0;
};
} // namespace lynx
