#include "core/Layer.h"

#include <algorithm>
#include <cmath>

namespace lynx::layer
{
namespace
{
bool isChild (const std::vector<ChannelDef>& channels, int layer, int c) noexcept
{
    return c >= 0 && c < (int) channels.size() && c != layer && channels[(std::size_t) c].instrument != InstrumentKind::layer;
}
} // namespace

std::vector<int> validChildren (const Song& song, int layer)
{
    std::vector<int> out;
    if (layer < 0 || layer >= (int) song.channels.size())
        return out;
    for (const auto c : song.channels[(std::size_t) layer].layer.children)
        if (isChild (song.channels, layer, c) && std::find (out.begin(), out.end(), c) == out.end())
            out.push_back (c);
    return out;
}

float levelGain (const ChannelDef& layer) noexcept
{
    // FL multiplies a child voice's volume by its Layer's channel gain [REA_rodada164a DECOMP 0xD265E0]
    return channelGain (layer.volume);
}

float crossfadeGain (int index, int count, float fade) noexcept
{
    // FL's triangle per child [DECOMP 0xFEA3D0]: max (0, 1 − |f·(N − 1) − i|); one child: f
    const auto f = (double) std::clamp (fade, 0.0f, 1.0f);
    if (count <= 1)
        return (float) f;
    return (float) std::max (0.0, 1.0 - std::abs (f * (double) (count - 1) - (double) index));
}

float pitchCents (const ChannelDef& layer) noexcept
{
    // the Layer's pitch knob, minus its root note's distance from C5 [DECOMP 0xD265E0]
    return 100.0f * layer.sampler.pitch - 100.0f * (float) (layer.misc.rootNote - 60);
}

int pick (const ChannelDef& layer, const std::vector<ChannelDef>& channels, int key, std::uint32_t& random, int& turn, Pick* out, int max) noexcept
{
    const auto& kids = layer.layer.children;
    const auto count = (int) kids.size();
    if (count == 0 || max <= 0)
        return 0;
    const auto self = (int) (&layer - channels.data()); // (the Layer lives in `channels`)
    const auto takes = [&] (int k) {
        const auto c = kids[(std::size_t) k];
        return isChild (channels, self, c) && channels[(std::size_t) c].inKeyRegion (key);
    };
    const auto gainOf = [&] (int k) { return layer.layer.crossfade ? crossfadeGain (k, count, layer.layer.fade) : 1.0f; };

    if (layer.layer.random || layer.layer.sequential)
    {
        int candidates = 0;
        for (int k = 0; k < count; ++k)
            candidates += takes (k) ? 1 : 0;
        if (candidates == 0)
            return 0;
        int chosen = 0;
        if (layer.layer.random)
        {
            random = random * 1664525u + 1013904223u;
            chosen = (int) ((std::uint64_t) (random >> 8) * (std::uint64_t) candidates >> 24);
        }
        else
        {
            chosen = ((turn % candidates) + candidates) % candidates;
            turn = (chosen + 1) % candidates;
        }
        for (int k = 0; k < count; ++k)
            if (takes (k) && chosen-- == 0)
            {
                out[0] = { kids[(std::size_t) k], gainOf (k) };
                return out[0].gain > 0.0f ? 1 : 0;
            }
        return 0;
    }

    int n = 0;
    for (int k = 0; k < count && n < max; ++k)
        if (takes (k))
            if (const auto g = gainOf (k); g > 0.0f)
                out[n++] = { kids[(std::size_t) k], g };
    return n;
}

void setChildren (Song& song, int layer, std::vector<int> selected)
{
    if (layer < 0 || layer >= (int) song.channels.size())
        return;
    std::sort (selected.begin(), selected.end());
    selected.erase (std::unique (selected.begin(), selected.end()), selected.end());
    selected.erase (std::remove_if (selected.begin(), selected.end(), [&] (int c) { return ! isChild (song.channels, layer, c); }), selected.end());
    song.channels[(std::size_t) layer].layer.children = std::move (selected);
}

void setRootNote (ChannelDef& d, int key)
{
    key = std::clamp (key, 0, 127);
    if (d.instrument == InstrumentKind::sampler)
    {
        // the sample plays at its own pitch on the root: the pitch moves by the root's change (within ±24)
        const auto pitch = std::clamp (d.sampler.pitch + (float) (d.misc.rootNote - key), -24.0f, 24.0f);
        const auto moved = (int) std::lround (pitch - d.sampler.pitch);
        d.sampler.pitch = pitch;
        d.misc.rootNote = std::clamp (d.misc.rootNote - moved, 0, 127);
    }
    else
        d.misc.rootNote = key;
}

void splitChildren (Song& song, int layer)
{
    if (layer < 0 || layer >= (int) song.channels.size())
        return;
    const auto root = song.channels[(std::size_t) layer].misc.rootNote;
    const auto kids = validChildren (song, layer);
    for (std::size_t k = 0; k < kids.size(); ++k)
    {
        auto& c = song.channels[(std::size_t) kids[k]];
        const auto key = std::clamp (root + (int) k, 0, 127);
        c.keyLow = c.keyHigh = key;
        setRootNote (c, key);
        // instruments that are not samplers sound their own pitch on their key through Add to key
        if (c.instrument != InstrumentKind::sampler && c.instrument != InstrumentKind::audioClip)
            c.misc.addToKey = true;
    }
}

void resetChildren (Song& song, int layer)
{
    for (const auto k : validChildren (song, layer))
    {
        auto& c = song.channels[(std::size_t) k];
        c.keyLow = 0;
        c.keyHigh = 127;
        setRootNote (c, 60);
        if (c.instrument != InstrumentKind::sampler && c.instrument != InstrumentKind::audioClip)
            c.misc.addToKey = false;
    }
}

std::vector<int> parentsOf (const Song& song, int channel)
{
    std::vector<int> out;
    for (int l = 0; l < (int) song.channels.size(); ++l)
        if (song.channels[(std::size_t) l].instrument == InstrumentKind::layer)
        {
            const auto& kids = song.channels[(std::size_t) l].layer.children;
            if (std::find (kids.begin(), kids.end(), channel) != kids.end())
                out.push_back (l);
        }
    return out;
}
} // namespace lynx::layer
