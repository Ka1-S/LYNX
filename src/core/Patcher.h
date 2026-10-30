#pragma once

// FL's Patcher as a real plugin graph (REA_rodada141): the runtime of a PatcherDoc (core/Song.h), used by a mixer
// slot (EffectKind::patcher) and by a Channel Rack generator (InstrumentKind::patcher).
//
// Behaviour rebuilt from FL's Patcher (REA_rodada14 §1, REA_rodada20 §3.1, REA_rodada141; clean-room, our own code):
//  - every audio input port sums its active cables: the first one writes source × gain, the next ones add
//    source × gain; an input with no active cable is silence;
//  - nodes run in the order of the audio cables (a cable that closes a loop reads the source's previous block);
//  - delay compensation: a cable whose source has less latency than the destination's latest input goes through a
//    delay line of the difference, so parallel branches stay aligned; the graph reports its longest path;
//  - notes: the "From LYNX SHARE" MIDI output reaches the instruments cabled to it (a generator Patcher's channel notes);
//  - control cables: a node's control output (Peak Controller, Formula Controller, an Envelope Controller…) sets a
//    parameter of another node each block, through the cable's value map.

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Effects.h"
#include "core/Instruments.h"
#include "core/Song.h"
#include "core/Vfx.h"

namespace lynx::patcher
{
// How the graph creates its nodes: the rack's own builders (plugins through the app's factories, nested Patchers…).
// Null results become pass-through / silent nodes.
struct Factories
{
    std::function<std::shared_ptr<Effect> (const EffectSlot&)> effect;
    std::function<std::shared_ptr<Instrument> (const ChannelDef&)> instrument;
};

// Default builders without plugins (tests, offline tools): built-in effects and instruments only.
Factories builtInFactories (double sampleRate, double bpm = 140.0);

// A node's audio output bus `bus` ≥ 1 that FL's splitting plugins provide: an imported BusHead, or LYNX's
// Frequency Splitter bands (1 low, 2 mid, 3 high). nullopt = the node has no such output (silence).
std::optional<PatchNode::BusHead> busHeadOf (const PatchNode& node, int bus);

// The audio inputs / outputs and control outputs a node shows in the editor.
struct NodePorts
{
    int audioIns = 1, audioOuts = 1, controlOuts = 0;
    bool midiIn = false;
    bool midiOut = false; // a MIDI Out node: its notes leave on its MIDI output pin (round 162)
    int midiOuts = 0;     // MIDI output pins (a MIDI Out: 1; a VFX node: its voice outputs, round 164b)
};
// The voice outputs a VFX node shows (FL activates the first few; LYNX shows one free pin past the last cabled one)
int shownMidiOutputs (const PatcherDoc& doc, int node);
NodePorts nodePorts (const PatchNode& node);

// MIDI through the I/O nodes (round 162, FL's "MIDI port N" pins).
// The MIDI ports the "From LYNX SHARE" / "To LYNX SHARE" nodes show: the activated ones and those with a cable, sorted.
std::vector<int> shownFromMidiPorts (const PatcherDoc& doc);
std::vector<int> shownToMidiPorts (const PatcherDoc& doc);
bool fromNotesShown (const PatcherDoc& doc, bool generator);
// The MIDI ports whose "From LYNX SHARE" pin has an active cable: the Patcher takes what LYNX sends on those ports.
std::vector<int> midiInputPorts (const PatcherDoc& doc);
// Where what enters on "From LYNX SHARE" pin `fromPin` (0 Notes, patchMidiPortPin (N)) leaves on "To LYNX SHARE" MIDI ports,
// directly or through MIDI Out nodes (which set the MIDI channel; −1 = unchanged).
struct MidiPortRoute
{
    int port = 0, midiChannel = -1;
    bool operator== (const MidiPortRoute&) const = default;
};
std::vector<MidiPortRoute> midiOutputsOf (const PatcherDoc& doc, int fromPin);
// The "To LYNX SHARE" MIDI ports a VFX node's outputs are cabled to (round 165c): what the VFX plays goes out on them
std::vector<int> vfxHostPorts (const PatcherDoc& doc);
// Pin menu helpers (FL's Events submenu): "Deactivate unused" keeps only the pins with a cable.
void deactivateUnusedMidiPins (PatcherDoc& doc, bool fromNode);

// Whether two documents have the same structure (nodes, their plugins / states and the cables): a running graph
// built for `a` can take `b`'s values (parameters, on/off, dry/wet, cable gains) without being built again.
bool sameStructure (const PatcherDoc& a, const PatcherDoc& b);

class Graph
{
public:
    static constexpr int capacity = 2048; // samples per piece (longer blocks are split)

    // `previous` (may be null): nodes with the same effect / instrument setup keep their running instance.
    Graph (const PatcherDoc& doc, double sampleRate, const Factories& factories, const Graph* previous = nullptr);
    ~Graph();

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setSongPosition (double ticks) noexcept;
    void setPlaying (bool playing) noexcept;
    void setHeldNotes (const NoteMask* channels, int count) noexcept;

    // One block: the host's input (and side-chain, may be null) in, the main output out (may alias the input);
    // the extra outputs land in auxOutput(). Any n (split into pieces).
    void process (const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, int n, double bpm) noexcept;

    // the instruments cabled to "From LYNX SHARE" notes
    void noteOn (int key, float velocity) noexcept;
    void noteOff (int key) noexcept;
    void noteOnMidi (int key, float velocity, int midiChannel) noexcept;
    void noteOffMidi (int key, int midiChannel) noexcept;
    // what LYNX sends to the Patcher on MIDI port `port` (a MIDI Out on that port…): the nodes cabled to that pin
    void noteOnPort (int port, int key, float velocity, int midiChannel) noexcept;
    void noteOffPort (int port, int key, int midiChannel) noexcept;
    // The same at a sample of the next block(s) (round 164f): `offset` counts from the next sample process() renders,
    // so the notes the rack sends a mixer-slot Patcher land where they are in the chunk. Audio thread; past the
    // queue's capacity notes are dropped.
    void queueNoteOnPort (int port, int key, float velocity, int midiChannel, int offset) noexcept;
    void queueNoteOffPort (int port, int key, int midiChannel, int offset) noexcept;
    bool takesMidiPort (int port) const noexcept;
    // a voice property of a note that came in on MIDI port `port` (a Channel Rack VFX's output, round 165c): the
    // instruments on that pin take it (VFX nodes as a property event of that voice)
    void voicePropertyPort (int port, int key, int prop, float value, int midiChannel) noexcept;
    // what the VFX nodes sent to "To LYNX SHARE" MIDI ports since the last call (round 165c); the rest wait
    int takeHostEvents (Instrument::PortEvent* out, int max) noexcept;
    // a controller's mod wheel, other CCs, channel pressure and pitch wheel played into the Patcher's channel (round
    // 164e): like FL's Patcher (it takes MIDI input and passes it on), the instruments cabled to "From LYNX SHARE" notes get
    // them as if they were the channel (hosted plugins the MIDI message, LYNX's samplers the bend)
    void midiMessage (const std::uint8_t* bytes, int size) noexcept;
    void pitchBend (float semitones) noexcept;
    void controlChange (int cc, float value) noexcept;
    void allNotesOff() noexcept;
    void cut() noexcept; // (a cut group's cut: the instruments stop, the VFX nodes keep their voices)
    void slideTo (int key, float cents, float samples) noexcept;
    void setNextNoteExpression (float pan, float cents, bool porta) noexcept;
    // the next note's other properties, for the VFX nodes (round 164b)
    void setNextNoteModulation (float modX, float modY) noexcept;
    void setNextNoteRelease (float velocity) noexcept;
    void setNextNoteColour (int colour) noexcept;
    bool hasInstruments() const noexcept;
    bool instrumentsIdle() const noexcept;

    int latencySamples() const noexcept { return latency; }
    // The device's input and output buses for the hardware insert nodes (round 165c; null: offline / no device), and
    // where the next process() call's first sample sits in the callback piece. Audio thread, before process().
    void setHardwareIo (const LiveInput* in, HardwareOutput* out, int offset) noexcept
    {
        hwIn = in;
        hwOut = out;
        hwOffset = offset;
    }
    bool hasHardwareNodes() const noexcept { return hardwareNodes; }
    // The Patcher's published parameter k (automation, FL's record 1100) set to v (0..1). Audio thread.
    void setPublished (int k, float v) noexcept;
    float controlOutput (int output) const noexcept;

    // "To LYNX SHARE" extra outputs (bus 1…): how many and their last block (valid until the next process)
    int auxOutputs() const noexcept { return (int) aux.size(); }
    const float* auxOutput (int index, int side) const noexcept;

    // the running nodes (UI: plugin editors, state capture); null where the node failed or is the other type
    int nodeCount() const noexcept { return (int) nodes.size(); }
    std::shared_ptr<Effect> effectAt (int node) const;
    std::shared_ptr<Instrument> instrumentAt (int node) const;

    // Takes the values of a document with the same structure (sameStructure): applied at the next block.
    // UI thread; the graph keeps the document copy.
    void takeValues (const PatcherDoc& doc, double bpm = 140.0);

private:
    struct Node;
    template <typename F>
    void forNoteTargets (F&& f) noexcept;
    struct AudioWire;
    struct ControlWire;
    struct Values;

    void processPiece (const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, int n, double bpm) noexcept;
    void applyPending() noexcept;

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<int> order;
    std::vector<AudioWire> wires;
    std::vector<ControlWire> controls;
    std::vector<int> noteTargets; // nodes cabled to the host's notes (directly)
    struct MidiTarget
    {
        int node = 0, midiChannel = -1; // −1 = the incoming channel (a MIDI Out node on the way sets its own)
        bool vfx = false;               // a VFX node: the voice goes into its processor
        bool layer = false;             // a Layer node (round 165c): the note plays the nodes on its Children pin
    };
    // A Layer node (round 165c, FL's Layer inside a Patcher): the generator nodes cabled to its Children pin are its
    // children, in node order; Random / Sequential / Crossfade and its level, pan and pitch as a Layer channel's.
    struct LayerNode;
    std::vector<std::unique_ptr<LayerNode>> layers; // [node] (null: not a Layer)
    // Calls f (target, gain, pan, cents, viaLayer) for each target a note reaches, a Layer's picked children in its place.
    // `type`: 1 note on, 0 note off, 2 anything else (every child).
    template <typename F>
    void forLayeredTargets (const std::vector<MidiTarget>& targets, int type, int key, F&& f) noexcept;
    // what a node's MIDI output pin `pin` (or a "From LYNX SHARE" pin) reaches, through MIDI Out nodes
    std::vector<MidiTarget> midiTargetsOf (const PatcherDoc& doc, int from, int pin) const;
    std::vector<std::vector<MidiTarget>> pinTargets; // ["From LYNX SHARE" pin: 0 Notes, 1 + port] the instruments it reaches
    void deliver (int pin, bool on, int key, float velocity, int midiChannel) noexcept;

    // VFX nodes (round 164b): their processors run at the start of each piece on the voices that reached them, and
    // what they send out plays the instruments on their outputs at the exact sample (the piece is split there)
    struct VfxQueue;
    struct NoteOut;
    struct TimedNote
    {
        int pin = 0, key = 60, midiChannel = -1, offset = 0;
        float velocity = 0.0f; // 0 = note off
    };
    static constexpr int timedCapacity = 1024;
    std::array<TimedNote, timedCapacity> timed {}; // queued notes, offsets from the next sample rendered
    int timedCount = 0;
    void queueTimed (int pin, int key, float velocity, int midiChannel, int offset) noexcept;
    // hosted plugin nodes whose notes leave on their MIDI output pin (round 164f): what they sent in the last piece
    // plays the nodes cabled to it at the next piece's start
    std::vector<int> midiSources;
    void routeNote (const std::vector<MidiTarget>& targets, int sourceNode, bool on, int key, float velocity, int midiChannel, int offset) noexcept;
    void sortNoteOuts() noexcept;
    std::vector<int> vfxOrder;                // VFX nodes, sources before the nodes they feed
    std::vector<std::unique_ptr<VfxQueue>> vfxQueues; // [node] (null: not a VFX node)
    std::vector<NoteOut> noteOuts;            // what the VFX nodes play in the current piece (capacity reserved)
    bool hasVfx = false;
    struct PendingVoice
    {
        float pan = 0.0f, cents = 0.0f, modX = 0.5f, modY = 0.5f, release = 0.5f;
        int colour = 0;
        bool porta = false;
    } nextVoice;
    double ticks = 0.0;
    bool playing = false;
    void runVfx (int n, double bpm) noexcept;
    void routeVfxOutput (int node, const vfx::Event& e) noexcept;
    static constexpr int hostEventCapacity = 512;
    std::array<Instrument::PortEvent, hostEventCapacity> hostEvents {};
    int hostEventCount = 0;
    void playNoteOut (const NoteOut& o) noexcept;
    void renderNodes (const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, int start, int n, double bpm) noexcept;
    std::vector<PatchParam> published;
    std::vector<float> publishedValues; // built-in targets: the latest automation value (NaN = none), applied each block
    std::vector<std::vector<int>> outputs; // [bus of "To LYNX SHARE"] its cables
    std::vector<std::array<std::vector<float>, 2>> aux;
    std::array<std::vector<float>, 6> scratch; // gathered input, gathered side-chain, wire delay
    std::vector<float> zeros;
    std::unique_ptr<Values> active;
    std::atomic<Values*> pending { nullptr };
    std::atomic<Values*> retired { nullptr };
    int latency = 0;
    double rate = 44100.0;
    const LiveInput* hwIn = nullptr;
    HardwareOutput* hwOut = nullptr;
    int hwOffset = 0, pieceOffset = 0; // (pieceOffset: the current piece's first sample in the callback piece)
    bool hardwareNodes = false;
};

// A Layer node's children (round 165c): the generator nodes its Children pin reaches (through MIDI Out nodes), Layers
// left out, in node order — the order its Crossfade and Sequential go through.
std::vector<int> layerChildren (const PatcherDoc& doc, int node);

// Whether a Patcher holds a Hardware Insert (round 165c), also inside nested Patchers: its insert / channel then runs on
// the audio thread and hands the graph the device's buses.
bool usesHardwareIo (const PatcherDoc& doc) noexcept;

// A mixer slot of kind `patcher`
std::unique_ptr<Effect> makeEffect (const EffectSlot& slot, double sampleRate, const Factories& factories, const Effect* previous = nullptr);
// A Channel Rack generator Patcher (aux outputs go to doc.outputTracks)
std::unique_ptr<Instrument> makeInstrument (const ChannelDef& channel, double sampleRate, const Factories& factories,
                                            const Instrument* previous = nullptr);
// The graph inside a Patcher effect / instrument (null for anything else)
Graph* graphOf (Effect* effect) noexcept;
Graph* graphOf (Instrument* instrument) noexcept;
// Takes the new values of a slot / channel whose Patcher kept its structure: true = done (keep the running instance)
bool acceptValues (Effect* effect, const EffectSlot& slot);
bool acceptValues (Instrument* instrument, const ChannelDef& channel);

// The mixer insert that extra output k (1…) of a generator Patcher feeds
int auxOutputTrack (const ChannelDef& channel, int k) noexcept;

// Editing helpers (UI and import): a new node / wire, removing a node (its cables go, later indices shift down)
int addNode (PatcherDoc& doc, PatchNode node);
void removeNode (PatcherDoc& doc, int node);
bool connect (PatcherDoc& doc, const PatchWire& wire); // false if the same cable exists or the ends are invalid
// The Surface tab (round 151): a new control (its outputs follow the others'), removing one (its cables go)
int addSurfaceControl (PatcherDoc& doc, SurfaceControl control);
void removeSurfaceControl (PatcherDoc& doc, int index);
// Name shown for a node: its label, else the plugin / effect / instrument name
std::string nodeName (const PatchNode& node);
} // namespace lynx::patcher
