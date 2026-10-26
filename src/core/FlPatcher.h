#pragma once

// FL's Patcher (REA_rodada14 §1): the plugin graph stored in the state (0xD5). LYNX handles
// the simple cases: a serial chain of effects (becomes consecutive mixer slots) and the main generator
// of a Patcher used as an instrument.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lynx
{
struct PatcherNode
{
    std::string plugin;               // internal name (same as 0xC9)
    std::vector<std::uint8_t> state;  // same format as the plugin's 0xD5 in a slot/channel
    bool generator = false;
    // the full graph (REA_rodada141)
    std::uint64_t id = 0;   // node id (record 5000 @4)
    std::uint32_t flags = 0; // record 5000 @12 (bit 2 = disabled)
    std::string label;      // displayed name (5010)
    float x = 0.0f, y = 0.0f; // editor position (group 1600)
    bool placed = false;
};

// Every port, pin and cable of the graph, as FL saved them (REA_rodada14 §1.3–1.5, REA_rodada141)
struct PatcherPort
{
    int node = -1;      // −1 = a Patcher I/O pin
    bool input = false; // node: an input port; pin: one that takes the signal out of the Patcher ("To FL Studio")
    int index = 0;      // audio: bus; control input: parameter; control output: 0x10000000 + output; pin: bus
    int kind = 0;       // 0 audio, 1 MIDI, 2 control
    std::string name;   // pins: "Audio input", "Notes", "Insert 3"…
};
struct PatcherCable
{
    std::uint64_t destination = 0, source = 0;
    float gain = 1.0f;
    int kind = 0;          // resolved: the class byte if valid, else the destination port's
    bool disabled = false;
};

// Series-parallel expression of the IN ⇒ OUT audio path (REA_rodada20 §3.3): node (with the output bus
// used, for splitters), serial chain or parallel sum; `gain` = product of the cable gains.
struct PatcherExpr
{
    enum class Kind { identity, node, series, parallel };
    Kind kind = Kind::identity;
    int node = -1;
    int bus = -1; // splitter node: output bus of this branch (−1 = ordinary node)
    float gain = 1.0f;
    std::vector<PatcherExpr> children;
};

struct PatcherGraph
{
    std::vector<PatcherNode> nodes;
    std::vector<int> serialChain; // nodes from input to output, if the audio is a simple chain

    // Parallel branches from input to output (all on bus 0, each a simple chain; empty
    // branch = dry signal), with the cable gains multiplied. Empty if the graph is anything else.
    struct Branch
    {
        std::vector<int> nodes;
        float gain = 1.0f;
        int band = 0; // output bus of the `splitter` node the branch comes out of
    };
    std::vector<Branch> branches;
    std::vector<int> preChain; // serial nodes before the split into branches
    int splitter = -1;         // Frequency Splitter / Stereo Shaper that splits (the branches start at its outputs)
    int mainGenerator = -1;       // generator connected to the output with the most gain
    std::optional<PatcherExpr> audio; // the whole reduced audio graph (empty if it does not reduce or has a generator)

    std::vector<std::pair<std::uint64_t, PatcherPort>> ports; // id → port / pin
    std::vector<PatcherCable> cables;                          // all of them (disabled ones too)
    std::vector<std::pair<std::uint64_t, int>> exposed;        // record 1100: control port → Patcher parameter
    float inputX = 0.0f, inputY = 0.0f, outputX = 0.0f, outputY = 0.0f; // the I/O nodes' positions (first two 1600s)
    bool ioPlaced = false;
};

std::optional<PatcherGraph> parsePatcher (const std::vector<std::uint8_t>& state);
// A MIDI pin's port (round 162, Patcher_x64 FUN_00952560 / FUN_0094fb20): FL names its pins "Notes" (−1) and
// "MIDI port N" (or the port's own name, with N in the pin's bus field); −2 = not a MIDI port pin.
int flPatcherMidiPinPort (const PatcherPort& pin);
} // namespace lynx
