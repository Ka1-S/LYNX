#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"

// Editors of FL's VFX note processors (round 164b, core/Vfx.h), in LYNX's look: a header with the plugin's name,
// its presets (LYNX's own and, when FL Studio is installed, FL's factory presets) and — on a Channel Rack channel —
// the MIDI port its outputs go out on; then the plugin's own controls.
namespace lynx
{
// `locate` finds the VFX's ChannelDef in a song (a Channel Rack channel, or a node inside a Patcher); `channel` = its
// channel index, −1 inside a Patcher
std::unique_ptr<juce::Component> makeVfxEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, int channel);
std::unique_ptr<juce::Component> makeVfxEditor (AudioEngine& engine, int channel);
} // namespace lynx
