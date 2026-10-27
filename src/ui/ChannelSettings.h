#pragma once

#include "engine/AudioEngine.h"

namespace lynx
{
// Sampler settings window for a channel (pitch, attack, release, reverse,
// "cut itself"), like FL's Channel settings. One window per channel.
void openChannelSettings (AudioEngine& engine, int channel);
void closeChannelSettings();

// Channel button toggle (Channel Rack): which kinds have a settings window, and whether it is open.
bool hasChannelSettings (InstrumentKind kind) noexcept;
bool isChannelSettingsOpen (int channel);
void closeChannelSettings (int channel);

// The channel window on its "Misc" tab (FL's wrench tab: levels, polyphony, time, cut groups, arpeggiator); for a channel
// without a settings page (a hosted plugin) a window with that tab alone.
void openChannelMisc (AudioEngine& engine, int channel);

// Channel modulation window (cutoff/pitch envelopes and LFOs), for the synth or the sampler.
void openChannelModulation (AudioEngine& engine, int channel);

// Window content only (for --snapshot mode).
std::unique_ptr<juce::Component> makeChannelSettingsPanel (AudioEngine& engine, int channel);
std::unique_ptr<juce::Component> makeChannelModulationPanel (AudioEngine& engine, int channel);
// The whole channel window (REA round 143b): FL's header strip (tabs; On, Pan, Vol, Track) over the instrument page,
// the "Envelope / instrument" tab (synth, sampler) and the "Misc" tab. `tab` < 0 = the Misc tab.
std::unique_ptr<juce::Component> makeChannelFrame (AudioEngine& engine, int channel, int tab = 0);

// A Patcher node's built-in generator (round 164f): the same pages as on a channel — the instrument's page and, for
// the synth and the sampler, the Envelope / instrument tab — editing the node `locate` finds. Null: no page for it.
bool hasNodeEditor (const ChannelDef& channel) noexcept;
std::unique_ptr<juce::Component> makeNodeChannelFrame (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate);
} // namespace lynx
