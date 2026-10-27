#pragma once

// FL's Control Surface (an effect slot) and Dashboard (a Channel Rack generator), REA_rodada164a §5: a panel the user
// builds from knobs, sliders, buttons, check boxes and XY pads; each control is a control output any parameter can be
// linked to ("Link to internal controller", Patcher cables). LYNX keeps both on the Patcher's Surface model
// (SurfaceControl, round 151): the controls live in the slot's / channel's `patcher.surface`.
//
//  - Control Surface (EffectKind::controlSurface): output k's value is params[k] (automatable like any effect
//    parameter); the controls' own value fields mirror it.
//  - Dashboard (InstrumentKind::dashboard): output k's value is the controls' value (the instrument copies them on every
//    snapshot).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx
{
class Effect;
class Instrument;
}

namespace lynx::surface
{
// A panel publishes at most this many outputs (an XY pad counts two).
constexpr int maxOutputs = 32;

// The output index of control `index`'s first value (an XY pad has two: X, then Y).
int firstOutput (const std::vector<SurfaceControl>& controls, int index) noexcept;
// The name of output k ("Knob 1", "XY 1 X").
std::string outputName (const std::vector<SurfaceControl>& controls, int k);

// Sets a control's value(s) (0..1) in a Control Surface slot (its params too) or any surface.
void setValue (EffectSlot& slot, int index, float value, float valueY);
void setValue (std::vector<SurfaceControl>& controls, int index, float value, float valueY);
// Adds / removes a control of a Control Surface slot keeping each other control's output values (params) in place.
int add (EffectSlot& slot, SurfaceControl control);
void remove (EffectSlot& slot, int index);
// The params of a Control Surface from its controls' values.
void syncParams (EffectSlot& slot) noexcept;

std::unique_ptr<Effect> makeEffect();
std::unique_ptr<Instrument> makeDashboard();

// A new panel with a few controls to start from (FL's default Control Surface is empty; LYNX's new one has two knobs).
std::vector<SurfaceControl> starterControls();

// FL's Control Surface state (0xD5, REA_rodada15 §11.2 / REA_rodada164a §5): the controls with their kind, name, place
// and value(s), in output order; nullopt when the state does not parse.
std::optional<std::vector<SurfaceControl>> parseFlControlSurface (const std::vector<std::uint8_t>& state);
// The LYNX output of FL's published output `flOutput` (FL numbers them freely; LYNX in panel order).
std::optional<int> flOutputToLynx (const std::vector<std::uint8_t>& state, int flOutput);

// FL's Dashboard state (0xD5, REA_rodada164f-flimport): a list of "records" (one per value control, in the panel's
// order of value controls: record k = parameter k + 1 = the k-th control that holds a value) and the skinned panel.
// A record of type 4 publishes its value on FL's controller output `number`; other types send MIDI (CC…).
struct FlDashboard
{
    std::vector<SurfaceControl> controls; // the value controls (knob, slider, check box) with FL's names and values
    std::vector<int> flOutputs;           // [LYNX output] FL's output number (−1: the record sends MIDI, no output)
};
std::optional<FlDashboard> parseFlDashboard (const std::vector<std::uint8_t>& state);
// The LYNX output (Dashboard channel) of FL's Dashboard output `flOutput`
std::optional<int> flDashboardOutputToLynx (const std::vector<std::uint8_t>& state, int flOutput);
} // namespace lynx::surface
