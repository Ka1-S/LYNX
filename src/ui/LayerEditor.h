#pragma once

// The settings page of a Layer channel (round 164, FL's "Layer channel settings" interpreted in LYNX's style): its
// children, Set / Show children, Random, Crossfade + Fade, Sequential, the Layering menu (Split / Reset / Group /
// Delete children) and a keyboard map of the children's key regions.

#include "engine/AudioEngine.h"

namespace lynx
{
std::unique_ptr<juce::Component> makeLayerEditor (AudioEngine& engine, int channel);
} // namespace lynx
