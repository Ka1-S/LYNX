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
// --patchershot: every built-in node's editor (double-click / Open editor), as images by node index (round 165c)
void shootNodeEditors (AudioEngine& engine, bool channel, int index, int slot, std::function<void (const juce::Image&, int)> save);
// FL's Add plugin › Browse plugin database (round 162): the main window shows the Browser's Plugin database
// (set by the main window)...
extern std::function<void (bool generators)> onBrowsePluginDatabase;
// ...and the next generator / effect picked there goes into the Patcher that asked (true = taken). One pick only.
bool takeInstrumentFromBrowser (const ChannelDef& c);
bool takeEffectFromBrowser (const EffectSlot& fx);
} // namespace PatcherWindows
} // namespace lynx
