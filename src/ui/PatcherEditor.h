#pragma once

#include "engine/AudioEngine.h"

namespace lynx
{
// FL's Patcher editor (REA_rodada141): the node graph of a mixer slot (EffectKind::patcher) or of a Channel Rack
// generator (InstrumentKind::patcher). Add effects, instruments and plugins, cable audio, notes and control values
// between them and the "From LYNX" / "To LYNX" nodes, open each plugin's own editor.
namespace PatcherWindows
{
void openSlot (AudioEngine& engine, int track, int slot);
void openChannel (AudioEngine& engine, int channel);
bool isChannelOpen (int channel);
void closeChannel (int channel);
// --patchershot: the Add plugin menu and the I/O nodes' menus (Events) of a Patcher, as images (round 162)
void shootMenus (AudioEngine& engine, bool channel, int index, int slot, std::function<void (const juce::Image&, const juce::String&)> save);
} // namespace PatcherWindows
} // namespace lynx
