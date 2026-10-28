#pragma once

// The settings page of a Layer channel (round 164, FL's "Layer channel settings" interpreted in LYNX's style): its
// children, Set / Show children, Random, Crossfade + Fade, Sequential, the Layering menu (Split / Reset / Group /
// Delete children) and a keyboard map of the children's key regions.

#include "engine/AudioEngine.h"

namespace lynx
{
std::unique_ptr<juce::Component> makeLayerEditor (AudioEngine& engine, int channel);
// The same on a Layer node inside a Patcher (round 165c): `locate` finds the node's channel, `children` its children (the
// nodes on its Children pin, in order); Set / Show / Group / Delete children are left out, Level / Pan / Pitch knobs added.
std::unique_ptr<juce::Component> makeLayerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate,
                                                  std::function<std::vector<ChannelDef*> (Song&)> children);
} // namespace lynx
