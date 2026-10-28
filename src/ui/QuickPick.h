#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"

namespace lynx::quickpick
{
// FL's renamed plugins (issue #11): users rename their most used plugins in the Plugin database ("1_EQ", "2_Limiter")
// and then, on an empty mixer slot (or the Channel Rack's add menu), press "1" to load that one. The names live in
// the plugin host (PluginHost::pluginNames); these helpers list LYNX's generators / effects with their keys, show the
// renamed ones first in the menus and pick them by their first character.

// One generator or effect of the Plugin database. `key` is what a user name is stored under: the plugin's identifier
// string, or "lynx:gen:<English name>" / "lynx:fx:<English name>" for LYNX's built-in ones and FL's engines.
struct GeneratorEntry
{
    std::string key;
    juce::String label, detail;
    int folder = 0; // 0 LYNX, 1 FL engines, 2 VFX, 3 installed
    std::function<std::optional<ChannelDef>()> make;
};

struct EffectEntry
{
    std::string key;
    juce::String label, detail;
    int folder = 0; // 0 LYNX, 1 FL engines, 3 installed
    std::function<std::optional<EffectSlot>()> make;
};

std::vector<GeneratorEntry> generatorEntries (AudioEngine& engine);
std::vector<EffectEntry> effectEntries (AudioEngine& engine);

// The user's name for `key`, else `original`.
juce::String shownName (AudioEngine& engine, const std::string& key, const juce::String& original);

// What a mixer slot shows (round 165c): a plugin renamed in the Plugin database ("1_EQ") shows that name, as in FL;
// otherwise the slot's own name (core/ProjectContents.h effectSlotName).
juce::String slotName (AudioEngine& engine, const EffectSlot& slot);

// Asks for a new name (empty: the original again); `done` runs after a change.
void rename (AudioEngine& engine, const std::string& key, const juce::String& original, std::function<void()> done);

// A menu item typing can pick.
struct Pick
{
    juce::String label;
    std::function<void()> action;
};

// The renamed effects / generators (sorted by name), each adding itself through `add`.
std::vector<Pick> renamedEffects (AudioEngine& engine, std::function<void (EffectSlot)> add);
std::vector<Pick> renamedGenerators (AudioEngine& engine, std::function<void (ChannelDef)> add);

// Adds `picks` at the top of `menu` under a header ("Renamed plugins: type the first character").
void addPicks (juce::PopupMenu& menu, const std::vector<Pick>& picks);

// Shows `menu`; while it is open, typing a character that starts exactly one of `picks` runs that one (FL's menus),
// and `onKey` sees the other keys first (e.g. Delete in a slot menu; true = used, the menu closes).
void showMenu (juce::PopupMenu& menu, const juce::PopupMenu::Options& options, std::vector<Pick> picks,
               std::function<bool (const juce::KeyPress&)> onKey = {});
} // namespace lynx::quickpick
