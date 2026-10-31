#pragma once

// The plugin picker's catalog (REA round 151): the scanned plugins grouped like FL Studio's Plugin picker / plugin
// database (categories along the picker, a search, favourites), instead of one menu of every plugin. FL groups its
// generators as Controller, Drum, MIDI, Misc, Patcher, Sampler, Synth classic, Synth special, Visual and its effects
// as Controller, Delay reverb, Distortion, Dynamics, Filter, Flanger, Gain, Misc, Patcher, Visual [EXEC: FL Studio
// 2026's Plugin picker, F8]; for third-party plugins it goes by what the plugin reports. LYNX does the same from the
// category a plugin declares (VST3 sub-categories such as "Fx|Dynamics", CLAP features) and, when it declares none
// useful (most VST2 plugins say only "Effect" / "Synth"), from words in its name, in LYNX's own category names.
// No JUCE here (tests/PluginCatalogTests151.inc).

#include <map>
#include <set>
#include <string>
#include <utility>
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

// --- Round 166f (issue #16): the user's plugin folders ---------------------------------------------------------------
// FL Studio lets users arrange its Plugin database in folders and subfolders (e.g. "Stock/EQ", "Third party/Reverb");
// the mixer slot's and the Channel Rack's plugin menus then list the plugins in groups, one heading per folder. LYNX
// keeps the same in its own Plugin database (the Browser): a folder path per plugin key ("lynx:fx:<name>", a plugin's
// identifier — the keys of ui/QuickPick.h) plus the folders the user created (empty ones too), per kind. Folder
// paths use "/" between levels, at most `maxFolderDepth` levels; they are saved as text (plugin-folders.txt):
//   "fx<TAB>Stock/EQ"                 a folder of effects        "gen<TAB>Synths"   a folder of generators
//   "key<TAB><plugin key><TAB>Stock/EQ"   where that plugin sits
constexpr int maxFolderDepth = 4;
struct Folders
{
    std::map<std::string, std::string> of; // plugin key → folder path
    std::set<std::string> effects, generators; // the folders (each parent of a folder is a folder too)
    int view = 0; // how the menus show them: MenuView (saved as "view<TAB>n")

    std::set<std::string>& kind (bool instruments) noexcept { return instruments ? generators : effects; }
    const std::set<std::string>& kind (bool instruments) const noexcept { return instruments ? generators : effects; }
};
// "  Stock // EQ / " → "Stock/EQ" (trimmed parts, no empty ones, no tabs / line breaks, at most maxFolderDepth levels)
std::string cleanFolder (const std::string& path);
std::string parentFolder (const std::string& path); // "" at the top
std::string folderLeaf (const std::string& path);    // "EQ" of "Stock/EQ"
Folders parseFolders (const std::string& text);
std::string formatFolders (const Folders& folders);
// Adds a folder (and its parents); returns the cleaned path ("" = not a valid name)
std::string addFolder (Folders& folders, bool instruments, const std::string& path);
// Renames / moves a folder and everything in it (its subfolders, the plugins in them); false when `to` is taken or invalid
bool renameFolder (Folders& folders, bool instruments, const std::string& from, const std::string& to);
// Removes a folder and its subfolders; their plugins go back to no folder (`keys`: the plugins of this kind)
void removeFolder (Folders& folders, bool instruments, const std::string& path, const std::vector<std::string>& keys);
// Puts a plugin in a folder ("" = no folder); the folder is created when missing
void moveToFolder (Folders& folders, bool instruments, const std::string& key, const std::string& path);

// The plugin menus' folders (FL Studio 2026's mixer slot / Add menus, measured in REA_rodada166f §1): the user
// folders holding some of the plugins `keys` (directly or deeper), each with its subfolders (natural A-Z) and the
// indexes into `keys` of the plugins directly in it; empty folders are left out. FL shows them three ways (the menu's
// own radio items): Categories — a heading per top folder, its subfolders as submenus first, then its plugins;
// Tree — every folder a submenu; Simple — one A-Z list without folders.
struct MenuFolder
{
    std::string name, path; // "EQ", "Stock/EQ"
    std::vector<MenuFolder> sub;
    std::vector<int> items;
};
std::vector<MenuFolder> menuTree (const Folders& folders, bool instruments, const std::vector<std::string>& keys);
enum class MenuView { categories, simple, tree };
// every plugin index of a folder tree, depth first (the Simple view's list before sorting)
std::vector<int> allItems (const std::vector<MenuFolder>& tree);
std::string groupHeading (const std::string& path); // "Stock › EQ" (a path shown in one line)

// Folders from FL Studio's own Plugin database: `files` = the .fst presets under its Effects (or Generators) folder as
// paths relative to it ("Stock/EQ/Fruity Parametric EQ 2.fst"; FL's own category folders included), `names` = the
// shown name of each LYNX plugin of that kind (its key's label) and their keys. A preset whose name (the file name) is
// the name of a LYNX plugin (same letters and digits, any case) puts that plugin in the preset's folder. Returns how
// many plugins were placed.
int importFlFolders (Folders& folders, bool instruments, const std::vector<std::string>& files,
                     const std::vector<std::pair<std::string, std::string>>& keyAndName);
} // namespace lynx::plugincatalog
