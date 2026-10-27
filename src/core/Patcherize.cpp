#include "core/Patcherize.h"

#include <algorithm>
#include <map>

namespace lynx::patcherize
{
namespace
{
PatchWire wire (PatchWire::Kind kind, int from, int fromPort, int to, int toPort)
{
    PatchWire w;
    w.kind = kind;
    w.from = from;
    w.fromPort = fromPort;
    w.to = to;
    w.toPort = toPort;
    return w;
}

// The Patcher's published parameter for node `node`'s parameter `param` (added when new).
int publish (PatcherDoc& doc, int node, int param, const std::string& name)
{
    for (std::size_t k = 0; k < doc.published.size(); ++k)
        if (doc.published[k].node == node && doc.published[k].param == param)
            return (int) k;
    PatchParam p;
    p.node = node;
    p.param = param;
    p.name = name;
    doc.published.push_back (p);
    return (int) doc.published.size() - 1;
}
} // namespace

bool canPatcherizeChannel (const Song& song, int channel)
{
    if (channel < 0 || channel >= (int) song.channels.size())
        return false;
    switch (song.channels[(std::size_t) channel].instrument)
    {
        case InstrumentKind::audioClip:
        case InstrumentKind::controller:
        case InstrumentKind::midiOut:
        case InstrumentKind::patcher:
        case InstrumentKind::layer:
        case InstrumentKind::dashboard:
        case InstrumentKind::voltage:
        case InstrumentKind::vfx:
            return false;
        default:
            return true;
    }
}

bool patcherizeChannel (Song& song, int channel)
{
    if (! canPatcherizeChannel (song, channel))
        return false;
    auto& ch = song.channels[(std::size_t) channel];

    PatchNode node;
    node.generator = true;
    node.channel = ch;
    node.x = 260.0f;
    node.y = 150.0f;
    // what stays on the channel itself plays outside the Patcher: the copy inside has neutral channel settings
    node.channel.volume = 0.78f;
    node.channel.pan = 0.0f;
    node.channel.mixerTrack = 0;
    node.channel.muted = false;
    node.channel.arp = {};
    node.channel.misc = {};
    node.channel.patcher = {};
    node.channel.layer = {};
    node.channel.keyLow = 0;
    node.channel.keyHigh = 127;
    node.channel.group = -1;

    PatcherDoc doc;
    doc.nodes.push_back (std::move (node));
    doc.wires.push_back (wire (PatchWire::Kind::midi, patchFromHost, 0, 0, 0));
    doc.wires.push_back (wire (PatchWire::Kind::audio, 0, 0, patchToHost, 0));

    // the plugin's automated / linked parameters become the Patcher's published ones
    song.forEachTarget ([&doc, channel] (AutomationTarget& t) {
        if (t.kind == AutomationTargetKind::channelParam && t.index == channel)
            t.param = publish (doc, 0, t.param, {});
    });

    ch.instrument = InstrumentKind::patcher;
    ch.patcher = std::move (doc);
    // the instrument now lives in the node (the channel would otherwise load the plugin a second time)
    ch.plugin = {};
    ch.samplePath.clear();
    ch.zones.clear();
    ch.pluginState.clear();
    ch.modelParams.clear();
    ch.controllerState.clear();
    return true;
}

bool canPatcherizeSlot (const Song& song, int track, int slot)
{
    if (track < 0 || track >= (int) song.mixer.size())
        return false;
    const auto& fx = song.mixer[(std::size_t) track].effects;
    if (slot < 0 || slot >= (int) fx.size())
        return false;
    const auto& s = fx[(std::size_t) slot];
    // a Fruity Send is routing (its insert sends mid-chain), a second side input has no Patcher pin
    return fruitySendTarget (s) < 0 && s.sidechain2 < 0;
}

std::vector<int> slotsForAll (const Song& song, int track)
{
    std::vector<int> out;
    if (track < 0 || track >= (int) song.mixer.size())
        return out;
    int sidechain = -1;
    for (int s = 0; s < (int) song.mixer[(std::size_t) track].effects.size(); ++s)
    {
        if (! canPatcherizeSlot (song, track, s))
            continue;
        const auto sc = song.mixer[(std::size_t) track].effects[(std::size_t) s].sidechain;
        if (sc >= 0 && sidechain >= 0 && sc != sidechain)
            continue; // one side-chain source per Patcher (its From LYNX side-chain pin)
        if (sc >= 0)
            sidechain = sc;
        out.push_back (s);
    }
    return out;
}

int patcherizeSlots (Song& song, int track, int slot, bool all)
{
    const auto moved = all ? slotsForAll (song, track) : (canPatcherizeSlot (song, track, slot) ? std::vector<int> { slot } : std::vector<int> {});
    if (moved.empty())
        return -1;
    auto& fx = song.mixer[(std::size_t) track].effects;

    EffectSlot patch;
    patch.kind = EffectKind::patcher;
    auto& doc = patch.patcher;
    int sidechain = -1;
    for (std::size_t k = 0; k < moved.size(); ++k)
    {
        PatchNode node;
        node.effect = fx[(std::size_t) moved[k]];
        node.x = 160.0f + 150.0f * (float) k;
        node.y = 150.0f;
        if (node.effect.sidechain >= 0)
        {
            sidechain = node.effect.sidechain;
            node.effect.sidechain = -1; // it comes through the Patcher's side-chain pin now
        }
        doc.nodes.push_back (std::move (node));
        const auto n = (int) k;
        doc.wires.push_back (wire (PatchWire::Kind::audio, k == 0 ? patchFromHost : n - 1, 0, n, 0));
        if (fx[(std::size_t) moved[k]].sidechain >= 0) // its detector input: the Patcher's side-chain pin
            doc.wires.push_back (wire (PatchWire::Kind::audio, patchFromHost, 1, n, 1));
    }
    doc.wires.push_back (wire (PatchWire::Kind::audio, (int) moved.size() - 1, 0, patchToHost, 0));
    doc.outputX = 160.0f + 150.0f * (float) moved.size() + 40.0f;
    patch.sidechain = sidechain;

    // the new chain: the Patcher where the first moved slot was, the others in their order
    const auto first = moved.front();
    std::vector<EffectSlot> chain;
    std::map<int, int> newIndex; // old slot → new slot (moved ones → the Patcher)
    std::map<int, int> nodeOf;   // old slot → its node
    for (int s = 0; s < (int) fx.size(); ++s)
    {
        const auto it = std::find (moved.begin(), moved.end(), s);
        if (it != moved.end())
        {
            nodeOf[s] = (int) (it - moved.begin());
            if (s == first)
            {
                newIndex[s] = (int) chain.size();
                chain.push_back (patch);
            }
            else
                newIndex[s] = newIndex[first];
            continue;
        }
        newIndex[s] = (int) chain.size();
        chain.push_back (fx[(std::size_t) s]);
    }
    const auto patchIndex = newIndex[first];
    auto& placed = chain[(std::size_t) patchIndex].patcher;

    // automation and links: a moved slot's parameters → the Patcher's published parameters
    song.forEachTarget ([&] (AutomationTarget& t) {
        if ((t.kind != AutomationTargetKind::effectParam && t.kind != AutomationTargetKind::effectMix) || t.index != track)
            return;
        const auto node = nodeOf.find (t.slot);
        if (node == nodeOf.end())
        {
            if (const auto ni = newIndex.find (t.slot); ni != newIndex.end())
                t.slot = ni->second;
            return;
        }
        if (t.kind == AutomationTargetKind::effectMix)
        {
            t.kind = AutomationTargetKind::none; // (a node's dry/wet is not a parameter of the Patcher)
            return;
        }
        t.slot = patchIndex;
        t.param = publish (placed, node->second, t.param, {});
    });
    // live sources (controllers) in moved slots now run inside the Patcher: their automations can't read them
    for (auto& a : song.automations)
        if (a.source.track == track && a.source.slot >= 0)
        {
            if (nodeOf.count (a.source.slot) != 0)
            {
                a.source.track = -1;
                a.target.kind = AutomationTargetKind::none;
            }
            else if (const auto ni = newIndex.find (a.source.slot); ni != newIndex.end())
                a.source.slot = ni->second;
        }
    // side-chains that other inserts' slots take from this insert are unchanged (inserts, not slots)
    fx = std::move (chain);
    song.removeDeadMidiLinks();
    return patchIndex;
}
} // namespace lynx::patcherize
