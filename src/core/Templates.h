#pragma once

#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx
{
// New project templates (File > New from template).
std::vector<std::string> templateNames();
Song makeTemplate (int index);

// The New project window (FL's "New project", interpreted; REA_rodada84): name, folder, time settings and credits.
struct NewProjectSetup
{
    std::string name;         // the project's name (file name)
    std::string folder;       // where it is saved; empty = not saved yet
    bool ownFolder = true;    // create "<folder>/<name>/" for it
    double bpm = 130.0;
    int beatsPerBar = 4;
    ProjectInfo info;
};

// A name usable as a file name: characters Windows refuses become '_', spaces/dots trimmed at the ends; empty → "Untitled".
std::string legalProjectName (const std::string& name);

// Where the project file goes: "<folder>/<name>/<name><ext>" with ownFolder, else "<folder>/<name><ext>"; empty
// when there is no folder. Uses '/' (the caller converts).
std::string newProjectPath (const NewProjectSetup& setup, const char* extension);

// Applies the time settings (tempo 10..522, beats per bar 1..16) and the credits to a song made from a template.
void applySetup (Song& song, const NewProjectSetup& setup);

// What a new project starts with in the mixer (Options › Mixer, round 163): how many inserts, the Master's chain and
// whether the inserts have no built-in processing ("clean mixer": the per-track EQ is bypassed and hidden).
enum class MasterChain
{
    empty,      // nothing on the Master
    clipper,    // a soft clipper only (transparent below 0 dB)
    lynxDefault // LYNX's template: a limiter (transparent up to -0.3 dB)
};

struct NewProjectDefaults
{
    static constexpr int defaultInserts = 16;
    int inserts = defaultInserts;               // 0..maxMixerTracks - 1
    MasterChain master = MasterChain::lynxDefault;
    bool cleanMixer = false;
    int playlistTracks = 16;                    // tracks the Playlist shows at least (1..500)

    bool operator== (const NewProjectDefaults&) const = default;

    // "key=value" lines for ui-options.txt (keys newInserts, newMasterChain, newCleanMixer, playlistTracks) and back;
    // unknown or missing keys keep the defaults, values are clamped.
    std::string toText() const;
    void readKey (const std::string& key, const std::string& value);
};

// Applies the defaults to a song made from a built-in template (or Song::makeDefault): the mixer gets exactly
// `inserts` inserts (channels routed past them go to the Master), the Master's effects become the chosen chain and the
// clean-mixer switch is set. Effects already on inserts are kept.
void applyNewProjectDefaults (Song& song, const NewProjectDefaults& defaults);

// The app's current choice (Options › Mixer; the main window loads it from ui-options.txt).
NewProjectDefaults& newProjectDefaults();
// A new project from a built-in template (-1 = LYNX's default song) with the current defaults applied.
Song makeNewProject (int templateIndex = -1);

// A user template's name from its file name ("Trap kit.lynx" → "Trap kit").
std::string templateDisplayName (const std::string& fileName);
} // namespace lynx
