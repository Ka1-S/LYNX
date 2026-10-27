#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"

namespace lynx
{
// The plugin picker (REA round 151): FL Studio's "More plugins..." list and Plugin picker categories, in LYNX's
// look, for the scanned plugins (VST3 / VST / CLAP). Instruments and effects are two tabs; categories down the left
// (All, Favourites, the categories of core/PluginCatalog.h, then the vendors) with their counts; the list shows the
// favourite star (click it), name, vendor, format and category; typing filters (every word must match). Double-click
// or Enter adds the highlighted plugin; "Manage plugins..." opens the Plugin manager, "Scan" looks for new ones.
// One picker at a time (a tool window); `onChosen` runs once, then the picker closes.
// `onBuiltIn` (instruments only): LYNX's built-in note effects (FL's VFX, round 164b) are listed too, as "LYNX"
// entries; choosing one hands over a new channel of it.
void showPluginPicker (AudioEngine& engine, bool instruments, std::function<void (PluginRef)> onChosen,
                       std::function<void (ChannelDef)> onBuiltIn = nullptr);
} // namespace lynx
