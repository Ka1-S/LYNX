#include "core/NativeEngines.h"

#include <algorithm>

#include "core/Effects.h"
#include "core/FlEffects.h"
#include "core/GrossBeat.h"
#include "core/PhysicalModels.h"
#include "flp/Flp.h"

namespace lynx::natives
{
namespace
{
constexpr auto fl = Needs::flStudio;
constexpr auto own = Needs::nothing;

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

bool endsWith (const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && lower (s.substr (s.size() - tail.size())) == tail;
}

// The plugin's name as FL's import knows it (event 0xC9 of a channel / slot)
const char* flPluginName (InstrumentKind kind)
{
    switch (kind)
    {
        case InstrumentKind::toxic: return "Toxic Biohazard";
        case InstrumentKind::gms: return "GMS";
        case InstrumentKind::morphine: return "Morphine";
        default: return "";
    }
}

std::vector<std::uint8_t> readPreset (const std::string& path, const FlAccess& access)
{
    if (path.empty() || ! access.readFile)
        return {};
    return access.readFile (path);
}

FlpImportOptions importOptions (const FlAccess& access)
{
    FlpImportOptions options;
    options.roots = access.roots;
    options.fileExists = access.fileExists;
    options.readFile = access.readFile;
    options.saveEmbedded = access.saveEmbedded;
    return options;
}

std::optional<ChannelDef> channelFromPreset (InstrumentKind kind, const InstrumentEngine& engine, const std::string& path, const FlAccess& access)
{
    const auto bytes = readPreset (path, access);
    if (bytes.empty())
        return std::nullopt;
    try
    {
        flp::Project project;
        if (! endsWith (path, ".fst")) // Toxic Biohazard's .tbio, GMS' .gmsynth, Morphine's .mrp
        {
            flp::Channel c;
            c.pluginName = flPluginName (kind);
            // the state FL saves is "SSLF" + size + the preset file
            c.pluginState = { 'S', 'S', 'L', 'F' };
            for (int i = 0; i < 4; ++i)
                c.pluginState.push_back ((std::uint8_t) (bytes.size() >> (8 * i)));
            c.pluginState.insert (c.pluginState.end(), bytes.begin(), bytes.end());
            project.channels.push_back (c);
        }
        else
            project = flp::parseProject (bytes, true);

        if (project.channels.empty())
            return std::nullopt;
        auto& c = project.channels.front();
        c.name = engine.name;
        if (c.type == flp::ChannelType::unknown)
            c.type = flp::ChannelType::instrument;
        c.mixerTrack = -1;
        const auto song = importFlp (project, importOptions (access));
        if (song.channels.empty() || song.channels.front().instrument != kind)
            return std::nullopt;
        auto def = song.channels.front();
        def.name = engine.name;
        def.mixerTrack = 0;
        return def;
    }
    catch (const flp::Error&)
    {
        return std::nullopt;
    }
}
} // namespace

const std::vector<InstrumentEngine>& instrumentEngines()
{
    // name, preset folder (Generators), needs, beta. Engines that play from an empty state (LYNX's own defaults) need
    // nothing; the rest start from the user's FL "Default" preset (with its data files: DLL tables, wave folders...).
    static const std::vector<InstrumentEngine> list = {
        { InstrumentKind::threeosc, "3x Osc", "3x Osc", own, false },
        { InstrumentKind::sytrus, "Sytrus", "Sytrus", fl, false },
        { InstrumentKind::harmor, "Harmor", "Harmor", fl, true },
        { InstrumentKind::harmless, "Harmless", "Harmless", own, false },
        { InstrumentKind::gms, "GMS", "GMS", fl, false },
        { InstrumentKind::morphine, "Morphine", "Morphine", fl, false },
        { InstrumentKind::ogun, "Ogun", "Ogun", fl, false },
        { InstrumentKind::toxic, "Toxic Biohazard", "Toxic Biohazard", fl, false },
        { InstrumentKind::poizone, "PoiZone", "PoiZone", fl, false },
        { InstrumentKind::sakura, "Sakura", "Sakura", fl, false },
        { InstrumentKind::kepler, "Kepler", "Kepler", fl, false },
        { InstrumentKind::transistorbass, "Transistor Bass", "Transistor Bass", fl, false },
        { InstrumentKind::boobass, "BooBass", "", fl, false }, // (its multisample: the user's BooBass_Eng_x64.dll)
        { InstrumentKind::simsynth, "SimSynth", "SimSynth", own, false },
        { InstrumentKind::minisynth, "MiniSynth", "MiniSynth", fl, false },
        { InstrumentKind::dx10, "Fruity DX10", "Fruity DX10", own, false },
        { InstrumentKind::plucked, "Plucked!", "Plucked!", own, false },
        { InstrumentKind::sawer, "Sawer", "Sawer", fl, false },
        { InstrumentKind::beepmap, "BeepMap", "BeepMap", fl, false },
        { InstrumentKind::flkeys, "FL Keys", "FL Keys", fl, false },
        { InstrumentKind::slayer, "FL Slayer", "FL Slayer", fl, false },
        { InstrumentKind::granulizer, "Fruity Granulizer", "Fruity Granulizer", fl, true },
        { InstrumentKind::drumaxx, "Drumaxx", "Drumaxx", fl, false },
        { InstrumentKind::drumpad, "Drumpad", "Drumpad", fl, false },
        { InstrumentKind::fruitkick, "Fruit Kick", "Fruit Kick", fl, false },
        { InstrumentKind::bassdrum, "BassDrum", "BassDrum", fl, false },
    };
    return list;
}

const std::vector<EffectEngine>& effectEngines()
{
    static const std::vector<EffectEngine> list = {
        { EffectKind::maximus, "Maximus", fl, false },
        { EffectKind::vocodex, "Vocodex", fl, false },
        { EffectKind::equo, "EQUO", fl, false },
        { EffectKind::distructor, "Distructor", fl, false },
        { EffectKind::lovePhilter, "Fruity Love Philter", fl, false },
        { EffectKind::grossBeat, "Gross Beat", fl, false },
        { EffectKind::convolver, "Fruity Convolver", fl, false },
        { EffectKind::flPitchShifter, "Pitch Shifter", own, false },
        { EffectKind::vintagePhaser, "Vintage Phaser", own, false },
        { EffectKind::squeeze, "Fruity Squeeze", own, false },
        { EffectKind::rhythmGain, "", own, false },
        { EffectKind::newtone, "", own, true },
        { EffectKind::newtime, "", own, true },
    };
    return list;
}

const InstrumentEngine* instrumentEngine (InstrumentKind kind)
{
    for (const auto& e : instrumentEngines())
        if (e.kind == kind)
            return &e;
    return nullptr;
}

const EffectEngine* effectEngine (EffectKind kind)
{
    for (const auto& e : effectEngines())
        if (e.kind == kind)
            return &e;
    return nullptr;
}

std::vector<std::string> presetCandidates (const std::string& presetFolder, bool generator, const FlAccess& access)
{
    std::vector<std::string> out;
    if (presetFolder.empty() || access.roots.data.empty())
        return out;
    const auto folder = std::string ("%FLStudioData%\\Patches\\Plugin presets\\") + (generator ? "Generators\\" : "Effects\\") + presetFolder;
    const auto exists = [&access] (const std::string& p) { return ! p.empty() && access.fileExists && access.fileExists (p); };

    for (const auto* name : { "Default.fst", "Default.tbio", "Default\\Default.tbio" })
        if (const auto path = resolveFlPath (folder + "\\" + name, access.roots); exists (path))
            out.push_back (path);

    if (access.listPresets)
        for (const auto& path : access.listPresets (resolveFlPath (folder, access.roots)))
            if ((endsWith (path, ".fst") || endsWith (path, ".tbio") || endsWith (path, ".gmsynth") || endsWith (path, ".mrp"))
                && std::find (out.begin(), out.end(), path) == out.end())
                out.push_back (path);
    return out;
}

std::string defaultPresetPath (const std::string& presetFolder, bool generator, const FlAccess& access)
{
    const auto c = presetCandidates (presetFolder, generator, access);
    return c.empty() ? std::string() : c.front();
}

namespace
{
// BooBass has no preset folder: a new one is its three knobs at their defaults (0.5, REA_rodada29 §4) over the
// multisample of the user's own engine DLL
std::string booBassDll (const FlAccess& access)
{
    const auto dll = resolveFlPath ("%FLStudioFactoryData%\\Plugins\\Fruity\\Generators\\BooBass\\BooBass_Eng_x64.dll", access.roots);
    return ! dll.empty() && access.fileExists && access.fileExists (dll) ? dll : std::string();
}
} // namespace

bool available (const InstrumentEngine& e, const FlAccess& access)
{
    if (e.kind == InstrumentKind::boobass)
        return ! booBassDll (access).empty();
    return e.needs == Needs::nothing || ! defaultPresetPath (e.presetFolder, true, access).empty();
}

bool available (const EffectEngine& e, const FlAccess& access)
{
    return e.needs == Needs::nothing || ! defaultPresetPath (e.presetFolder, false, access).empty();
}

std::optional<ChannelDef> makeChannel (InstrumentKind kind, const FlAccess& access)
{
    const auto* engine = instrumentEngine (kind);
    if (engine == nullptr)
        return std::nullopt;

    if (kind == InstrumentKind::boobass)
    {
        const auto dll = booBassDll (access);
        if (dll.empty() || ! booBassDllUsable (dll))
            return std::nullopt;
        ChannelDef def;
        def.name = engine->name;
        def.instrument = kind;
        def.samplePath = dll;
        def.modelParams = { 32768.0f, 32768.0f, 32768.0f };
        return def;
    }

    // the user's FL default preset, imported like an .flp channel (the exact engine with its data files)
    int tries = 0;
    for (const auto& path : presetCandidates (engine->presetFolder, true, access))
    {
        if (++tries > 60)
            break;
        if (auto def = channelFromPreset (kind, *engine, path, access))
            return def;
    }

    if (engine->needs != Needs::nothing)
        return std::nullopt;

    ChannelDef def; // LYNX's own default: the engine plays from an empty state
    def.name = engine->name;
    def.instrument = kind;
    return def;
}

std::optional<EffectSlot> makeSlot (EffectKind kind, const FlAccess& access, double bpm)
{
    const auto* engine = effectEngine (kind);
    if (engine == nullptr)
        return std::nullopt;

    if (const auto path = defaultPresetPath (engine->presetFolder, false, access); ! path.empty())
    {
        const auto bytes = readPreset (path, access);
        try
        {
            const auto project = flp::parseProject (bytes, true);
            if (! project.channels.empty() && ! project.channels.front().pluginState.empty())
            {
                const FlDataReader data = [&access] (const std::string& relative) -> std::vector<std::uint8_t> {
                    const auto p = resolveFlPath ("%FLStudioFactoryData%\\" + relative, access.roots);
                    if (p.empty() || ! access.readFile || (access.fileExists && ! access.fileExists (p)))
                        return {};
                    return access.readFile (p);
                };
                auto slot = effectFromFl (project.channels.front().pluginName.empty() ? engine->presetFolder : project.channels.front().pluginName,
                                          project.channels.front().pluginState, bpm, data);
                if (slot && slot->kind == kind)
                {
                    if (kind == EffectKind::grossBeat)
                        grossbeat::setPpq (slot->state, 96);
                    return slot;
                }
            }
        }
        catch (const flp::Error&)
        {
        }
    }

    if (engine->needs != Needs::nothing)
        return std::nullopt;
    return makeEffectSlot (kind);
}

std::optional<EffectSlot> newEffect (EffectKind kind, const FlAccess& access, double bpm)
{
    if (effectEngine (kind) != nullptr)
        return makeSlot (kind, access, bpm);
    return makeEffectSlot (kind);
}

std::string effectMenuName (EffectKind kind)
{
    std::string name = effectInfo (kind).name;
    if (const auto* e = effectEngine (kind); e != nullptr && e->beta)
        name += " (beta)";
    return name;
}

std::string instrumentMenuName (const InstrumentEngine& e)
{
    return std::string (e.name) + (e.beta ? " (beta)" : "");
}
} // namespace lynx::natives
