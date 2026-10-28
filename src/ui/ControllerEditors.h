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
// The same editor for any Envelope Controller: `locate` finds its ChannelDef in the song (a Channel Rack channel or a
// Patcher node; null = gone; reads get the current song, writes go through editSong), `channel` = its Channel Rack
// index when it has one (−1: the links list is left out), `output` reads its running output k (may be empty).
std::unique_ptr<juce::Component> makeEnvelopeControllerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, int channel = -1,
                                                               std::function<float (int)> output = {});
// The Keyboard Controller's editor (round 165c): each key's value, the note-off value and auto release; on a channel or a
// Patcher node (`locate`), `output` reads the running output 0 (may be empty).
std::unique_ptr<juce::Component> makeKeyboardControllerEditor (AudioEngine& engine, int channel);
std::unique_ptr<juce::Component> makeKeyboardControllerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, std::function<float()> output = {});
// The editor shown inside the channel settings window of a MIDI Out channel.
std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, int channel);
// The same for a MIDI Out that is not a channel (a node inside a Patcher, round 162): `locate` finds it in a song.
std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate);
} // namespace lynx
