#pragma once

// VFX Script (round 165d): FL Studio's Python note scripts, run by LYNX's embedded Python (core/Python.h).
//
// LYNX's own `flvfx` module is written from FL's published documentation of the VFX Script API (the plugin's manual
// page and its "API reference" tab: names, signatures and behaviour; clean-room, our own code):
//   vfx.context (ticks, PPQ, isPlaying, tempo, voices, form), vfx.Voice (note, finePitch, velocity, releaseVelocity,
//   pan, fcut, fres, color, length, output; trigger(), release(), copyFrom()), vfx.ScriptDialog (addInputKnob,
//   addInputKnobInt, addInputCheckbox, addInputCombo, addInputText, addInput, addInputSurface, addGroup, endGroup,
//   getInputValue, setNormalizedValue), vfx.addOutputController / setOutputController, and the callbacks
//   onTriggerVoice, onReleaseVoice, onTick, createDialog.
//
// Real time: the audio thread never runs Python. Each block it hands the notes and the block's ticks to the Python
// thread (lock-free), waits a bounded time for the answer and plays what the script sent at its sample; an answer
// that comes later plays at the next block's start. While LYNX exports (offline) it always waits, so a render is
// exact and repeatable. A script that runs too long is stopped and the editor says why.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Vfx.h"

namespace lynx::vfx::script
{
// The script's PPQ (vfx.context.PPQ)
constexpr int ppq = 96;

// A control the script's createDialog() made
struct Control
{
    enum class Type : std::uint8_t
    {
        knob,     // addInputKnob: a float in min..max
        knobInt,  // addInputKnobInt: an integer in min..max
        checkbox, // addInputCheckbox: 0 / 1
        combo,    // addInputCombo: the option's index
        text,     // addInputText
        value,    // addInput: a float 0..1
        surface   // a control of the embedded Control Surface (0..1, saved as float bits)
    };
    Type type = Type::knob;
    std::string name;  // what getInputValue takes: "Group: Label" inside a group, else "Label"
    std::string group, label, hint;
    double defaultValue = 0.0, minimum = 0.0, maximum = 1.0;
    std::vector<std::string> options;
    std::string defaultText;

    double fromNormalized (double v01) const noexcept;
    double toNormalized (double value) const noexcept;
    double clampValue (double value) const noexcept;
    bool numeric() const noexcept { return type != Type::text; }
};

// What a script declares (compiled once per source text)
struct Info
{
    bool compiled = false;
    std::string error;                // the compile error (Python's traceback); empty when it compiled
    std::string title, description;   // the ScriptDialog's
    std::vector<Control> controls;    // in creation order (= FL's automation inputs)
    std::vector<std::string> outputs; // addOutputController names (= control outputs)
    std::vector<double> outputDefaults;
    std::string log;                  // what print() wrote while it compiled
    bool hasTick = false, hasTrigger = false, hasRelease = false, hasDialog = false;
    int index (const std::string& controlName) const noexcept; // −1: none
};

// The script's declarations (UI / non-audio threads; cached by source text; waits for the Python thread)
std::shared_ptr<const Info> describe (const std::string& source);
std::shared_ptr<const Info> describe (const ScriptSettings& settings);

// Control values as the state keeps them ([Parameters], FL's encodings: a knob as (v − min) / (max − min) · 2^18, an
// integer knob / checkbox / combo as the number, a text as it is, a surface control as its float's bits)
struct Values
{
    std::vector<double> numbers; // [control]
    std::vector<std::string> texts;
    bool operator== (const Values&) const = default;
};
Values defaults (const Info& info);
Values valuesOf (const Info& info, const ScriptSettings& settings);
// The surface controls a state carries that the script does not declare (BinaryBorn scripts' embedded Control
// Surface): added to `info` as Type::surface controls, so they keep their saved values and can be edited
Info withSurfaceControls (const Info& info, const ScriptSettings& settings);
std::string encodeValue (const Control& c, double value, const std::string& text);
double decodeValue (const Control& c, const std::string& stored);
// Rewrites [Parameters] with these values (FL's order: the controls in creation order; other lines kept)
void writeValues (ScriptSettings& settings, const Info& info, const Values& values);

// A running instance, as its editor sees it: found through the VFX state's tag (State::tag)
struct Monitor
{
    virtual ~Monitor() = default;
    virtual std::string log() const = 0;             // print() output and runtime errors (capped)
    virtual std::uint64_t logVersion() const = 0;
    virtual void clearLog() = 0;
    virtual std::string status() const = 0;         // empty: running; else why it is not (compile error, too slow…)
    virtual Values liveValues() const = 0;           // the controls as the script sees them now
    virtual bool takeScriptChanges (Values& out) = 0; // values the script set itself (setNormalizedValue) since the last call
    virtual std::vector<float> outputValues() const = 0;
    virtual void restart() = 0;                      // runs the script again (its globals start over, the controls keep their values)
};
std::shared_ptr<Monitor> monitorOf (std::uint32_t tag);

// The processor of a VFX Script channel / node
std::unique_ptr<Processor> makeProcessor (const std::vector<std::uint8_t>& flState);

// Real time (the audio device runs) or offline (export, tests: the processor waits for every answer). Default offline.
void setRealtime (bool on) noexcept;

// The API reference text the editor shows (LYNX's own words)
const char* apiReference() noexcept;
} // namespace lynx::vfx::script
