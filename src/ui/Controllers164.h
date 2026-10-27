#pragma once

// Editors of the round 164 controllers (LYNX's own look, FL's workflow): the X-Y / X-Y-Z Controller and the Control
// Surface (mixer slots or Patcher nodes), the Dashboard and the Voltage Controller (Channel Rack channels).

#include "engine/AudioEngine.h"

namespace lynx
{
// An effect slot found in the song (a mixer slot or a Patcher node; null = gone), and its running control output k.
using SlotLocator = std::function<EffectSlot* (Song&)>;
using OutputReader = std::function<float (int)>;

// `target`: the automation target of the slot's parameters (its track / slot), none inside a Patcher.
std::unique_ptr<juce::Component> makeXyControllerEditor (AudioEngine& engine, SlotLocator locate, OutputReader output,
                                                         std::optional<AutomationTarget> target);
std::unique_ptr<juce::Component> makeControlSurfaceEditor (AudioEngine& engine, SlotLocator locate, std::optional<AutomationTarget> target);
// A Dashboard channel's panel (its controls in channel.patcher.surface).
std::unique_ptr<juce::Component> makeDashboardEditor (AudioEngine& engine, int channel);
std::unique_ptr<juce::Component> makeDashboardEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate); // (a Patcher node)

namespace Controller164Windows
{
bool handles (EffectKind kind) noexcept;
void open (AudioEngine& engine, int track, int slot);
void closeAll();
} // namespace Controller164Windows
} // namespace lynx
