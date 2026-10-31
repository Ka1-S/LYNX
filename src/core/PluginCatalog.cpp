#include "core/PluginCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <sstream>

namespace lynx::plugincatalog
{
namespace
{
std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

bool has (const std::string& haystack, std::initializer_list<const char*> words)
{
    return std::any_of (words.begin(), words.end(), [&haystack] (const char* w) { return haystack.find (w) != std::string::npos; });
}

// a short word as a whole token, or a token's start / end ("eq" in "Pro-EQ", "EQ8", not in "Frequency")
bool hasToken (const std::string& text, std::initializer_list<const char*> words)
{
    std::vector<std::string> tokens;
    std::string token;
    for (const auto c : text + " ")
        if (std::isalnum ((unsigned char) c))
            token += c;
        else if (! token.empty())
        {
            tokens.push_back (token);
            token.clear();
        }
    for (const auto* w : words)
    {
        const std::string word (w);
        for (const auto& t : tokens)
            if (t.size() >= word.size() && (t.compare (0, word.size(), word) == 0 || t.compare (t.size() - word.size(), word.size(), word) == 0))
            {
                // "eq8" / "proeq" yes; "equalis..." is matched by its own word, "sequencer" never (neither start nor end)
                return true;
            }
    }
    return false;
}

// LYNX's category names (the picker shows them translated)
constexpr const char* synth = "Synth";
constexpr const char* sampler = "Sampler";
constexpr const char* drum = "Drum";
constexpr const char* keys = "Piano & keys";
constexpr const char* other = "Other";
constexpr const char* delayReverb = "Delay & reverb";
constexpr const char* distortion = "Distortion";
constexpr const char* dynamics = "Dynamics";
constexpr const char* eqFilter = "EQ & filter";
constexpr const char* modulation = "Modulation";
constexpr const char* pitch = "Pitch & vocal";
constexpr const char* analyzer = "Analyzer";
constexpr const char* visual = "Visual"; // FL's picker has a Visual group (Wave Candy, ZGameEditor Visualizer; round 164c)
constexpr const char* utility = "Utility";

std::string effectCategory (const std::string& declared, const std::string& name)
{
    // what the plugin declares first (VST3 "Fx|Reverb", CLAP "audio-effect reverb"), then its name
    for (const auto* text : { &declared, &name })
    {
        const auto& t = *text;
        if (has (t, { "visualiz", "visualis", "video", "milkdrop", "projectm", "plugin dance", "big clock", "notebook" }) || hasToken (t, { "visual" }))
            return visual; // (round 165g: FL's Big Clock and HTML NoteBook sit in FL's Visual group)
        if (has (t, { "analy", "meter", "scope", "spectrum", "loudness", "tuner" }))
            return analyzer;
        if (has (t, { "reverb", "verb", "delay", "echo", "plate", "room", "hall", "spring" }))
            return delayReverb;
        if (has (t, { "pitch", "tune", "tuning", "vocal", "voice", "formant", "alterboy", "harmon" }))
            return pitch;
        if (has (t, { "compress", "limit", "dynamic", "gate", "expander", "de-ess", "deess", "transient", "clipper", "maxim", "leveler", "cla-2a",
                      "la-2a", "1176" }) || hasToken (t, { "comp" }))
            return dynamics;
        if (has (t, { "filter", "equal", "channel strip", "channelstrip", "sslchannel", "pro-q" }) || hasToken (t, { "eq" }))
            return eqFilter;
        if (has (t, { "chorus", "flang", "phase", "modulat", "tremolo", "vibrato", "doubler", "ensemble", "rotary", "panman", "autopan", "wider", "stereo" }))
            return modulation;
        if (has (t, { "distort", "drive", "satur", "decapitator", "tube", "tape", "crush", "fuzz", "warm", "exciter", "fresh air" }) || hasToken (t, { "amp" }))
            return distortion;
        if (has (t, { "tools", "utility", "gain", "trim", "mono", "routing", "restoration", "noise", "spatial", "mute" }))
            return utility;
    }
    return other;
}

std::string instrumentCategory (const std::string& declared, const std::string& name)
{
    for (const auto* text : { &declared, &name })
    {
        const auto& t = *text;
        if (has (t, { "drum", "beat", "kick", "808", "percussion" }))
            return drum;
        if (has (t, { "sampler", "sample", "rompler", "slicer" }))
            return sampler;
        if (has (t, { "piano", "keys", "organ", "rhodes", "e-piano" }))
            return keys;
        if (has (t, { "synth", "osc", "wavetable", "analog", "bass" }))
            return synth;
    }
    return other;
}
} // namespace

std::string categoryOf (const Entry& entry)
{
    auto declared = lower (entry.category);
    // the bare kind words say nothing about the category
    for (const auto* generic : { "instrument", "fx", "effect", "audio-effect", "synth|", "generator" })
        if (declared == generic)
            declared.clear();
    if (entry.instrument && (declared == "synth" || declared == "instrument|synth"))
        declared.clear(); // VST2's "Synth": all instruments say it; the name decides (a drum machine, a sampler)
    const auto name = lower (entry.name);
    return entry.instrument ? instrumentCategory (declared, name) : effectCategory (declared, name);
}

const std::vector<std::string>& categories (bool instruments)
{
    static const std::vector<std::string> generators { synth, sampler, drum, keys, other };
    static const std::vector<std::string> effects { delayReverb, distortion, dynamics, eqFilter, modulation, pitch, analyzer, visual, utility, other };
    return instruments ? generators : effects;
}

std::vector<int> select (const std::vector<Entry>& entries, const std::set<std::string>& favourites, const Filter& filter)
{
    std::vector<std::string> words;
    {
        std::string word;
        for (const auto c : lower (filter.query) + " ")
            if (c == ' ')
            {
                if (! word.empty())
                    words.push_back (word);
                word.clear();
            }
            else
                word += c;
    }

    std::vector<int> shown;
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const auto& e = entries[(std::size_t) i];
        if (e.instrument != filter.instruments)
            continue;
        if (filter.favouritesOnly && favourites.count (e.id) == 0)
            continue;
        if (! filter.vendor.empty() && e.vendor != filter.vendor)
            continue;
        const auto category = categoryOf (e);
        if (! filter.category.empty() && category != filter.category)
            continue;
        if (! words.empty())
        {
            const auto text = lower (e.name + " " + e.renamed + " " + e.vendor + " " + e.format + " " + category);
            if (! std::all_of (words.begin(), words.end(), [&text] (const std::string& w) { return text.find (w) != std::string::npos; }))
                continue;
        }
        shown.push_back (i);
    }

    std::sort (shown.begin(), shown.end(), [&] (int a, int b) {
        const auto& x = entries[(std::size_t) a];
        const auto& y = entries[(std::size_t) b];
        if (filter.favouritesFirst)
        {
            const auto fx = favourites.count (x.id) > 0, fy = favourites.count (y.id) > 0;
            if (fx != fy)
                return fx;
        }
        const auto nx = lower (x.shownName()), ny = lower (y.shownName());
        if (nx != ny)
            return nx < ny;
        return x.format < y.format;
    });
    return shown;
}

std::vector<std::string> vendors (const std::vector<Entry>& entries, bool instruments)
{
    std::vector<std::string> list;
    for (const auto& e : entries)
        if (e.instrument == instruments && ! e.vendor.empty() && std::find (list.begin(), list.end(), e.vendor) == list.end())
            list.push_back (e.vendor);
    std::sort (list.begin(), list.end(), [] (const std::string& a, const std::string& b) { return lower (a) < lower (b); });
    return list;
}

int countIn (const std::vector<Entry>& entries, bool instruments, const std::string& category)
{
    return (int) std::count_if (entries.begin(), entries.end(),
                                [&] (const Entry& e) { return e.instrument == instruments && (category.empty() || categoryOf (e) == category); });
}
// --- round 166f: the user's plugin folders ------------------------------------------------------------------------

namespace
{
std::vector<std::string> splitPath (const std::string& path)
{
    std::vector<std::string> parts;
    std::string part;
    auto flush = [&] {
        const auto a = part.find_first_not_of (" \t");
        if (a != std::string::npos) parts.push_back (part.substr (a, part.find_last_not_of (" \t") - a + 1));
        part.clear();
    };
    for (const auto c : path)
        if (c == '/' || c == '\\') flush();
        else if (c != '\t' && c != '\n' && c != '\r') part += c;
    flush();
    return parts;
}

bool within (const std::string& path, const std::string& folder) // path == folder or inside it
{
    return path == folder || (path.size() > folder.size() && path.compare (0, folder.size(), folder) == 0 && path[folder.size()] == '/');
}

std::string nameKey (const std::string& s)
{
    std::string k;
    for (const auto c : s)
        if (std::isalnum ((unsigned char) c)) k += (char) std::tolower ((unsigned char) c);
    return k;
}

// natural order ("EQ 2" before "EQ 10"), case-insensitive, by path parts
int naturalCompare (const std::string& a, const std::string& b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        if (std::isdigit ((unsigned char) a[i]) && std::isdigit ((unsigned char) b[j]))
        {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit ((unsigned char) a[i2])) ++i2;
            while (j2 < b.size() && std::isdigit ((unsigned char) b[j2])) ++j2;
            const auto na = std::stoull (a.substr (i, std::min<std::size_t> (18, i2 - i))), nb = std::stoull (b.substr (j, std::min<std::size_t> (18, j2 - j)));
            if (na != nb) return na < nb ? -1 : 1;
            i = i2; j = j2;
            continue;
        }
        // '/' sorts first so a parent comes right before its children
        const auto ca = a[i] == '/' ? (char) 1 : (char) std::tolower ((unsigned char) a[i]);
        const auto cb = b[j] == '/' ? (char) 1 : (char) std::tolower ((unsigned char) b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i; ++j;
    }
    return a.size() - i == b.size() - j ? 0 : (a.size() - i < b.size() - j ? -1 : 1);
}
} // namespace

std::string cleanFolder (const std::string& path)
{
    auto parts = splitPath (path);
    if ((int) parts.size() > maxFolderDepth) parts.resize ((std::size_t) maxFolderDepth);
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::string parentFolder (const std::string& path)
{
    const auto slash = path.rfind ('/');
    return slash == std::string::npos ? std::string() : path.substr (0, slash);
}

std::string folderLeaf (const std::string& path)
{
    const auto slash = path.rfind ('/');
    return slash == std::string::npos ? path : path.substr (slash + 1);
}

std::string addFolder (Folders& folders, bool instruments, const std::string& path)
{
    const auto clean = cleanFolder (path);
    for (auto p = clean; ! p.empty(); p = parentFolder (p))
        folders.kind (instruments).insert (p);
    return clean;
}

Folders parseFolders (const std::string& text)
{
    Folders f;
    std::istringstream in (text);
    std::string line;
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        const auto t1 = line.find ('\t');
        if (t1 == std::string::npos) continue;
        const auto tag = line.substr (0, t1);
        const auto rest = line.substr (t1 + 1);
        if (tag == "fx" || tag == "gen")
            addFolder (f, tag == "gen", rest);
        else if (tag == "view")
            f.view = std::clamp (std::atoi (rest.c_str()), 0, 2);
        else if (tag == "key")
        {
            const auto t2 = rest.rfind ('\t');
            if (t2 == std::string::npos || t2 == 0) continue;
            const auto key = rest.substr (0, t2);
            if (const auto path = cleanFolder (rest.substr (t2 + 1)); ! path.empty()) f.of[key] = path;
        }
    }
    return f;
}

std::string formatFolders (const Folders& folders)
{
    std::string out;
    if (folders.view != 0) out += "view\t" + std::to_string (folders.view) + "\n";
    for (const auto* kind : { "fx", "gen" })
        for (const auto& path : folders.kind (std::string (kind) == "gen"))
            out += std::string (kind) + "\t" + path + "\n";
    for (const auto& [key, path] : folders.of)
        if (! key.empty() && key.find_first_of ("\t\n") == std::string::npos && ! path.empty())
            out += "key\t" + key + "\t" + path + "\n";
    return out;
}

bool renameFolder (Folders& folders, bool instruments, const std::string& from, const std::string& to)
{
    const auto target = cleanFolder (to);
    auto& set = folders.kind (instruments);
    if (target.empty() || ! set.count (from) || within (target, from) || (target != from && set.count (target))) return target == from && ! target.empty();
    std::set<std::string> moved;
    for (auto it = set.begin(); it != set.end();)
        if (within (*it, from)) { moved.insert (target + it->substr (from.size())); it = set.erase (it); }
        else ++it;
    for (const auto& m : moved) addFolder (folders, instruments, m);
    for (auto& [key, path] : folders.of)
        if (within (path, from) && set.count (target + path.substr (from.size())))
            path = target + path.substr (from.size());
    return true;
}

void removeFolder (Folders& folders, bool instruments, const std::string& path, const std::vector<std::string>& keys)
{
    auto& set = folders.kind (instruments);
    for (auto it = set.begin(); it != set.end();)
        it = within (*it, path) ? set.erase (it) : std::next (it);
    for (const auto& key : keys)
        if (const auto it = folders.of.find (key); it != folders.of.end() && within (it->second, path))
            folders.of.erase (it);
}

void moveToFolder (Folders& folders, bool instruments, const std::string& key, const std::string& path)
{
    const auto clean = addFolder (folders, instruments, path);
    if (clean.empty()) folders.of.erase (key);
    else folders.of[key] = clean;
}

std::vector<MenuFolder> menuTree (const Folders& folders, bool instruments, const std::vector<std::string>& keys)
{
    const auto& set = folders.kind (instruments);
    std::map<std::string, std::vector<int>> direct;
    for (int i = 0; i < (int) keys.size(); ++i)
        if (const auto it = folders.of.find (keys[(std::size_t) i]); it != folders.of.end() && set.count (it->second))
            direct[it->second].push_back (i);
    std::function<std::vector<MenuFolder> (const std::string&)> build = [&] (const std::string& parent) {
        std::vector<MenuFolder> out;
        for (const auto& path : set)
            if (parentFolder (path) == parent)
            {
                MenuFolder f;
                f.path = path;
                f.name = folderLeaf (path);
                f.sub = build (path);
                if (const auto it = direct.find (path); it != direct.end()) f.items = it->second;
                if (! f.items.empty() || ! f.sub.empty()) out.push_back (std::move (f));
            }
        std::sort (out.begin(), out.end(), [] (const MenuFolder& a, const MenuFolder& b) { return naturalCompare (a.name, b.name) < 0; });
        return out;
    };
    return build ({});
}

std::vector<int> allItems (const std::vector<MenuFolder>& tree)
{
    std::vector<int> out;
    for (const auto& f : tree)
    {
        const auto inner = allItems (f.sub);
        out.insert (out.end(), inner.begin(), inner.end());
        out.insert (out.end(), f.items.begin(), f.items.end());
    }
    return out;
}

std::string groupHeading (const std::string& path)
{
    std::string out;
    for (const auto c : path)
        out += c == '/' ? std::string (" \xe2\x80\xba ") : std::string (1, c); // " › "
    return out;
}

int importFlFolders (Folders& folders, bool instruments, const std::vector<std::string>& files,
                     const std::vector<std::pair<std::string, std::string>>& keyAndName)
{
    std::map<std::string, std::string> keyOfName;
    for (const auto& [key, name] : keyAndName)
        keyOfName.emplace (nameKey (name), key);
    int placed = 0;
    for (const auto& file : files)
    {
        auto parts = splitPath (file);
        if (parts.size() < 2) continue; // a preset at the top of the folder: no folder
        auto stem = parts.back();
        if (const auto dot = stem.rfind ('.'); dot != std::string::npos) stem = stem.substr (0, dot);
        parts.pop_back();
        std::string folder;
        for (const auto& p : parts) folder += (folder.empty() ? "" : "/") + p;
        const auto it = keyOfName.find (nameKey (stem));
        if (it == keyOfName.end() || cleanFolder (folder).empty()) continue;
        moveToFolder (folders, instruments, it->second, folder);
        ++placed;
    }
    return placed;
}
} // namespace lynx::plugincatalog
