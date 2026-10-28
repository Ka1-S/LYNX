#include "core/PluginCatalog.h"

#include <algorithm>
#include <cctype>

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
            const auto text = lower (e.name + " " + e.vendor + " " + e.format + " " + category);
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
        const auto nx = lower (x.name), ny = lower (y.name);
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
} // namespace lynx::plugincatalog
