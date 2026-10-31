#include "core/Templates.h"

#include "core/Effects.h"
#include "core/SynthPresets.h"

#include <algorithm>

namespace lynx
{
namespace
{
SynthSettings preset (const char* name)
{
    for (const auto& [n, s] : synthPresets())
        if (n == name)
            return s;
    return {};
}

ChannelDef synth (const char* name, const char* presetName, int track, float volume = 0.78f)
{
    ChannelDef c;
    c.name = name;
    c.instrument = InstrumentKind::synth;
    c.synth = preset (presetName);
    c.mixerTrack = track;
    c.volume = volume;
    return c;
}

void steps (Pattern& p, int channel, std::initializer_list<int> list, float velocity = 0.78f)
{
    for (auto s : list)
    {
        Note n;
        n.start = (std::int64_t) s * ticksPerStep;
        n.channel = channel;
        n.velocity = velocity;
        p.notes.push_back (n);
    }
}

void notes (Pattern& p, int channel, std::initializer_list<std::array<int, 3>> list)
{
    // {step, length in steps, key}
    for (const auto& [step, length, key] : list)
    {
        Note n;
        n.start = (std::int64_t) step * ticksPerStep;
        n.length = (std::int64_t) length * ticksPerStep;
        n.key = key;
        n.channel = channel;
        p.notes.push_back (n);
    }
}
} // namespace

std::vector<std::string> templateNames()
{
    return { "Empty", "Basic beat", "Trap with 808", "House" };
}

Song makeTemplate (int index)
{
    if (index == 1)
        return Song::makeDefault();

    if (index == 0)
    {
        auto s = Song::makeDefault();
        s.patterns[0].notes.clear();
        return s;
    }

    auto s = Song::makeDefault();
    s.patterns[0].notes.clear();
    auto& p = s.patterns[0];

    if (index == 2) // trap: 140 BPM, fast hats, 808 with slide
    {
        s.bpm = 140.0;
        p.name = "Trap";
        p.lengthTicks = (std::int64_t) ppq * 4 * 2;
        steps (p, 0, { 0, 10, 16, 26 }, 0.9f);
        steps (p, 3, { 8, 24 });
        for (int i = 0; i < 32; i += 2)
            steps (p, 2, { i }, i % 4 == 0 ? 0.7f : 0.45f);
        steps (p, 2, { 13, 29 }, 0.5f);
        s.channels[4] = synth ("808", "808 Bass", 5, 0.85f);
        notes (p, 4, { { { 0, 6, 36 } }, { { 10, 4, 36 } }, { { 16, 6, 39 } }, { { 26, 6, 34 } } });
        Note glide;
        glide.start = 28 * ticksPerStep;
        glide.length = 2 * ticksPerStep;
        glide.key = 41;
        glide.channel = 4;
        glide.slide = true;
        p.notes.push_back (glide);
        s.swing = 0.0f;
    }
    else // house: 124 BPM, four on the floor, clap, off-beat hats, bass and chord
    {
        s.bpm = 124.0;
        p.name = "House";
        steps (p, 0, { 0, 4, 8, 12 }, 0.9f);
        steps (p, 1, { 4, 12 });
        steps (p, 2, { 2, 6, 10, 14 }, 0.6f);
        s.channels[4] = synth ("Bass", "Reese Bass", 5);
        notes (p, 4, { { { 2, 2, 33 } }, { { 6, 2, 33 } }, { { 10, 2, 36 } }, { { 14, 2, 31 } } });
        s.channels.push_back (synth ("Chord", "Pluck", 6, 0.6f));
        for (int key : { 57, 60, 64 })
            notes (p, 5, { { { 3, 1, key } }, { { 7, 1, key } }, { { 11, 1, key } } });
        s.swing = 0.15f;
    }

    p.sortNotes();
    return s;
}

std::string legalProjectName (const std::string& name)
{
    std::string out;
    for (auto c : name)
        out += std::string ("<>:\"/\\|?*").find (c) != std::string::npos || (unsigned char) c < 32 ? '_' : c;
    const auto first = out.find_first_not_of (" .");
    if (first == std::string::npos)
        return "Untitled";
    out.erase (0, first);
    while (! out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out.empty() ? "Untitled" : out;
}

std::string newProjectPath (const NewProjectSetup& setup, const char* extension)
{
    if (setup.folder.empty())
        return {};
    auto folder = setup.folder;
    while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\'))
        folder.pop_back();
    const auto name = legalProjectName (setup.name);
    return folder + "/" + (setup.ownFolder ? name + "/" : std::string()) + name + extension;
}

void applySetup (Song& song, const NewProjectSetup& setup)
{
    song.bpm = std::clamp (setup.bpm, 10.0, 522.0);
    song.beatsPerBar = std::clamp (setup.beatsPerBar, 1, 16);
    song.info = setup.info;
    if (song.info.title.empty() && ! setup.name.empty())
        song.info.title = setup.name;
}

std::string templateDisplayName (const std::string& fileName)
{
    auto name = fileName;
    const auto slash = name.find_last_of ("/\\");
    if (slash != std::string::npos)
        name.erase (0, slash + 1);
    const auto dot = name.find_last_of ('.');
    if (dot != std::string::npos && dot > 0)
        name.erase (dot);
    return name;
}

std::string NewProjectDefaults::toText() const
{
    const char* chain = master == MasterChain::empty ? "empty" : master == MasterChain::clipper ? "clipper" : "default";
    return "newInserts=" + std::to_string (inserts) + "\nnewMasterChain=" + chain + "\nnewCleanMixer=" + (cleanMixer ? "1" : "0")
           + "\nplaylistTracks=" + std::to_string (playlistTracks) + "\nnewAutoCrossfades=" + (autoCrossfades ? "1" : "0") + "\n";
}

void NewProjectDefaults::readKey (const std::string& key, const std::string& value)
{
    auto number = [&value] (int fallback) {
        try
        {
            return std::stoi (value);
        }
        catch (...)
        {
            return fallback;
        }
    };

    if (key == "newInserts")
        inserts = std::clamp (number (defaultInserts), 0, maxMixerTracks - 1);
    else if (key == "newMasterChain")
        master = value == "empty" ? MasterChain::empty : value == "clipper" ? MasterChain::clipper : MasterChain::lynxDefault;
    else if (key == "newCleanMixer")
        cleanMixer = value == "1";
    else if (key == "playlistTracks")
        playlistTracks = std::clamp (number (16), 1, 500);
    else if (key == "newAutoCrossfades")
        autoCrossfades = value != "0";
}

void applyNewProjectDefaults (Song& song, const NewProjectDefaults& defaults)
{
    const auto inserts = std::clamp (defaults.inserts, 0, maxMixerTracks - 1);
    if (song.mixer.empty())
        song.mixer = Song::defaultMixer (inserts);
    else if ((int) song.mixer.size() > inserts + 1)
        song.mixer.resize ((std::size_t) inserts + 1);
    else
        while ((int) song.mixer.size() < inserts + 1)
        {
            MixerTrack track;
            track.name = "Insert " + std::to_string (song.mixer.size());
            song.mixer.push_back (std::move (track));
        }

    // nothing may point past the last insert
    for (auto& c : song.channels)
        if (c.mixerTrack > inserts)
            c.mixerTrack = 0;
    for (auto& m : song.mixer)
        m.sends.erase (std::remove_if (m.sends.begin(), m.sends.end(), [inserts] (const MixerSend& s) { return s.target > inserts; }), m.sends.end());
    for (auto& p : song.playlistTracks)
        if (p.mixerTrack > inserts)
            p.mixerTrack = -1;

    auto& master = song.mixer[0].effects;
    switch (defaults.master)
    {
        case MasterChain::empty: master.clear(); break;
        case MasterChain::clipper:
            master.clear();
            master.push_back (makeEffectSlot (EffectKind::fruitySoftClipper));
            break;
        case MasterChain::lynxDefault: break; // the template's own chain
    }

    song.cleanMixer = defaults.cleanMixer;
    song.autoCrossfades = defaults.autoCrossfades;
}

NewProjectDefaults& newProjectDefaults()
{
    static NewProjectDefaults defaults;
    return defaults;
}

Song makeNewProject (int templateIndex)
{
    auto song = templateIndex < 0 ? Song::makeDefault() : makeTemplate (templateIndex);
    applyNewProjectDefaults (song, newProjectDefaults());
    return song;
}
} // namespace lynx
