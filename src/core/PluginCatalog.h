#pragma once

// The plugin picker's catalog (REA round 151): the scanned plugins grouped like FL Studio's Plugin picker / plugin
// database (categories along the picker, a search, favourites), instead of one menu of every plugin. FL groups its
// generators as Controller, Drum, MIDI, Misc, Patcher, Sampler, Synth classic, Synth special, Visual and its effects
// as Controller, Delay reverb, Distortion, Dynamics, Filter, Flanger, Gain, Misc, Patcher, Visual [EXEC: FL Studio
// 2026's Plugin picker, F8]; for third-party plugins it goes by what the plugin reports. LYNX does the same from the
// category a plugin declares (VST3 sub-categories such as "Fx|Dynamics", CLAP features) and, when it declares none
// useful (most VST2 plugins say only "Effect" / "Synth"), from words in its name, in LYNX's own category names.
// No JUCE here (tests/PluginCatalogTests151.inc).

#include <set>
#include <string>
#include <vector>

namespace lynx::plugincatalog
{
struct Entry
{
    std::string id;       // the plugin's identifier (PluginRef::id)
    std::string name;
    std::string vendor;
    std::string format;   // VST3, VST, CLAP, AU, LV2
    std::string category; // what the plugin declares (may be empty)
    bool instrument = false;
    std::string renamed = {}; // its name in the Plugin database when the user renamed it ("1_EQ"; empty = not renamed)

    const std::string& shownName() const noexcept { return renamed.empty() ? name : renamed; }
};

// The picker's category of one plugin: one of categories (entry.instrument).
std::string categoryOf (const Entry& entry);

// The categories in the picker's order (instruments: generators; else effects). "Other" last.
const std::vector<std::string>& categories (bool instruments);

// The picker's filters: what to show, in the order to show it (names A-Z, favourites first when `favouritesFirst`).
struct Filter
{
    bool instruments = false;
    std::string category;     // empty = all
    std::string vendor;       // empty = all
    bool favouritesOnly = false;
    std::string query;        // words that must all appear in the name, vendor, format or category (any case)
    bool favouritesFirst = true;
};
std::vector<int> select (const std::vector<Entry>& entries, const std::set<std::string>& favourites, const Filter& filter);

// The vendors of the plugins of one kind, A-Z (empty vendors left out).
std::vector<std::string> vendors (const std::vector<Entry>& entries, bool instruments);

// How many plugins of one kind fall in `category` ("" = all).
int countIn (const std::vector<Entry>& entries, bool instruments, const std::string& category);
} // namespace lynx::plugincatalog
