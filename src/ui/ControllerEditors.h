#pragma once

// Editors of LYNX's built-in controllers (round 133, issue #3): the Formula Controller (a mixer slot), the Envelope
// Controller and MIDI Out (Channel Rack channels). Their outputs are linked to knobs through "Link to internal
// controller" (ui/AutomationUi.h).

#include "engine/AudioEngine.h"

namespace lynx
{
namespace FormulaControllerWindows
{
void open (AudioEngine& engine, int track, int slot);
void closeAll();
} // namespace FormulaControllerWindows

// The editor shown inside the channel settings window of an Envelope Controller channel.
std::unique_ptr<juce::Component> makeEnvelopeControllerEditor (AudioEngine& engine, int channel);
// The editor shown inside the channel settings window of a MIDI Out channel.
std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, int channel);
// The same for a MIDI Out that is not a channel (a node inside a Patcher, round 162): `locate` finds it in a song.
std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate);
} // namespace lynx
