#include "core/ProjectContents.h"

#include <algorithm>

#include "core/Effects.h"
#include "core/PluginSubstitutes.h"
#include "core/FlEffects.h"

namespace lynx::projectcontents
{
bool playsSampleFile (InstrumentKind kind) noexcept
{
    return kind == InstrumentKind::sampler || kind == InstrumentKind::audioClip || kind == InstrumentKind::granulizer;
}

std::string insertName (const Song& song, int track)
{
    if (track >= 0 && track < (int) song.mixer.size() && ! song.mixer[(std::size_t) track].name.empty())
        return song.mixer[(std::size_t) track].name;
    return track == 0 ? std::string ("Master") : "Insert " + std::to_string (track);
}

std::string effectSlotName (const EffectSlot& slot)
{
    if (slot.kind == EffectKind::plugin)
        return slot.plugin.name;
    if (slot.isSubstitute())
        return substituteOriginalName (slot);
    if (isSoundgoodizerApprox (slot)) // (approximated without FL's preset files)
        return "Soundgoodizer";
    return effectInfo (slot.kind).name;
}

std::vector<Item> patterns (const Song& song)
{
    std::vector<Item> list;
    for (int p = 0; p < (int) song.patterns.size(); ++p)
    {
        Item item;
        item.kind = Kind::pattern;
        item.index = p;
        item.name = song.patterns[(std::size_t) p].name;
        if (item.name.empty())
            item.name = "Pattern " + std::to_string (p + 1);
        item.colour = song.patterns[(std::size_t) p].colour;
        for (const auto& c : song.playlist)
            if (c.kind == ClipKind::pattern && c.pattern == p)
                ++item.uses;
        list.push_back (std::move (item));
    }
    return list;
}

std::vector<Item> samples (const Song& song)
{
    std::vector<Item> list;

    auto add = [&list] (const std::string& path, int channel, std::uint32_t colour) {
        if (path.empty())
            return;
        auto it = std::find_if (list.begin(), list.end(), [&path] (const Item& i) { return i.path == path; });
        if (it == list.end())
        {
            Item item;
            item.kind = Kind::sample;
            item.index = channel;
            item.path = path;
            const auto slash = path.find_last_of ("/\\");
            item.name = slash == std::string::npos ? path : path.substr (slash + 1);
            if (const auto dot = item.name.find_last_of ('.'); dot != std::string::npos && dot > 0)
                item.name.erase (dot);
            item.colour = colour;
            list.push_back (std::move (item));
            it = list.end() - 1;
        }
        else if (it->colour == 0)
            it->colour = colour;
        ++it->uses;
    };

    for (int c = 0; c < (int) song.channels.size(); ++c)
    {
        const auto& ch = song.channels[(std::size_t) c];
        if (! playsSampleFile (ch.instrument))
            continue;
        if (ch.zones.empty())
            add (ch.samplePath, c, ch.colour);
        else
            for (const auto& z : ch.zones)
                add (z.path, c, ch.colour);
    }

    for (int t = 0; t < (int) song.mixer.size(); ++t)
        for (const auto& slot : song.mixer[(std::size_t) t].effects)
            add (slot.samplePath, -1, song.mixer[(std::size_t) t].colour);

    // the detail: how many places play it
    for (auto& item : list)
        if (item.uses > 1)
            item.detail = std::to_string (item.uses) + "x";
    return list;
}

std::vector<Item> generators (const Song& song)
{
    std::vector<Item> list;
    for (int c = 0; c < (int) song.channels.size(); ++c)
    {
        const auto& ch = song.channels[(std::size_t) c];
        Item item;
        item.kind = Kind::generator;
        item.index = c;
        item.name = ch.name.empty() ? "Channel " + std::to_string (c + 1) : ch.name;
        item.plugin = ch.instrument == InstrumentKind::plugin;
        if (ch.mixerTrack > 0 && ch.mixerTrack < (int) song.mixer.size())
            item.detail = insertName (song, ch.mixerTrack);
        item.colour = ch.colour;
        if (playsSampleFile (ch.instrument))
            item.path = ch.samplePath;
        list.push_back (std::move (item));
    }
    return list;
}

std::vector<Item> effects (const Song& song)
{
    std::vector<Item> list;
    for (int t = 0; t < (int) song.mixer.size(); ++t)
    {
        const auto& track = song.mixer[(std::size_t) t];
        for (int s = 0; s < (int) track.effects.size(); ++s)
        {
            Item item;
            item.kind = Kind::effect;
            item.index = t;
            item.slot = s;
            item.name = effectSlotName (track.effects[(std::size_t) s]);
            item.detail = insertName (song, t);
            item.colour = track.colour;
            item.plugin = track.effects[(std::size_t) s].kind == EffectKind::plugin;
            list.push_back (std::move (item));
        }
    }
    return list;
}

std::vector<Item> automations (const Song& song)
{
    std::vector<Item> list;
    for (int a = 0; a < (int) song.automations.size(); ++a)
    {
        const auto& automation = song.automations[(std::size_t) a];
        Item item;
        item.kind = Kind::automation;
        item.index = a;
        item.name = automation.name.empty() ? "Automation " + std::to_string (a + 1) : automation.name;
        for (const auto& c : song.playlist)
            if (c.kind == ClipKind::automation && c.automation == a)
                ++item.uses;
        // the colour of what it moves: a channel's or an insert's
        const auto& t = automation.target;
        if ((t.kind == AutomationTargetKind::channelVolume || t.kind == AutomationTargetKind::channelPan
             || t.kind == AutomationTargetKind::channelParam)
            && t.index >= 0 && t.index < (int) song.channels.size())
            item.colour = song.channels[(std::size_t) t.index].colour;
        else if ((t.kind == AutomationTargetKind::mixerVolume || t.kind == AutomationTargetKind::mixerPan
                  || t.kind == AutomationTargetKind::effectParam || t.kind == AutomationTargetKind::effectMix
                  || t.kind == AutomationTargetKind::mixerSend)
                 && t.index >= 0 && t.index < (int) song.mixer.size())
            item.colour = song.mixer[(std::size_t) t.index].colour;
        if (automation.isLive())
            item.detail = "live";
        list.push_back (std::move (item));
    }
    return list;
}
} // namespace lynx::projectcontents
