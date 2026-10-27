#pragma once

// What a Browser shows (round 163, user feedback "the browser always has a ton of useless folders I don't care about,
// I just need my samples/audio/soundfonts"): which of its top-level entries (roots) are visible and which kinds of
// files it lists. The main Browser and the separate Sample browser window each keep their own view, saved as
// "key=value" lines in the settings folder. No JUCE, so it can be tested in isolation.

#include <set>
#include <string>

namespace lynx::browserview
{
// What the Browser lists in its folders and search results (folders are always listed).
enum class ContentFilter
{
    all,      // every file LYNX can use (audio, MIDI, projects, presets, SoundFonts)
    audio,    // audio files only
    samples,  // audio files and SoundFonts
    presets,  // channel / plugin presets and SoundFonts
    projects, // .lynx / .flp projects and MIDI files
    count
};

constexpr int contentFilterCount = (int) ContentFilter::count;
const char* contentFilterId (ContentFilter filter);    // "all", "audio", "samples", "presets", "projects"
const char* contentFilterLabel (ContentFilter filter); // English menu text (the UI translates it)
ContentFilter contentFilterFromId (const std::string& id); // unknown: all

// Whether a file named `fileName` (only its extension matters, case-insensitive) is listed under `filter`.
// Files LYNX doesn't use are never listed.
bool listed (const std::string& fileName, ContentFilter filter);

// The kinds of top-level entries a Browser has. Folder roots are identified by their path; the two entries without a
// folder have fixed ids (projectRootId, pluginsRootId).
enum class RootKind
{
    project,        // "Current project"
    pluginDatabase, // the Plugin database
    flSamples,      // FL's sample packs
    flDemos,        // FL's demo projects
    flMidi,         // FL's MIDI scores
    flPresets,      // FL's channel / plugin presets (3x Osc, synth presets)
    pluginPresets,  // LYNX's own plugin presets folder
    documents,
    music,
    desktop,
    downloads,
    user            // a folder the user added
};

inline constexpr const char* projectRootId = "@project";
inline constexpr const char* pluginsRootId = "@plugins";

// Whether a root is shown in the Sample browser before the user chose (only sample / audio places).
bool sampleBrowserShowsByDefault (RootKind kind);

struct View
{
    std::set<std::string> hidden; // ids of the hidden roots (a folder's path, or projectRootId / pluginsRootId)
    ContentFilter filter = ContentFilter::all;
    bool open = false;            // the Sample browser window was open (unused for the main Browser)
    int x = 0, y = 0, width = 0, height = 0; // the window's last bounds (0 = default)

    bool shows (const std::string& id) const { return hidden.count (id) == 0; }
    void setShown (const std::string& id, bool shown);

    std::string toText() const;
    static View fromText (const std::string& text);
    bool operator== (const View&) const = default;
};
} // namespace lynx::browserview
