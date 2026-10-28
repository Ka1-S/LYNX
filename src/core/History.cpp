#include "core/History.h"

#include <algorithm>

namespace lynx
{
namespace
{
bool sameNote (const Note& a, const Note& b) noexcept
{
    return a.start == b.start && a.length == b.length && a.channel == b.channel && a.key == b.key && a.velocity == b.velocity
           && a.pan == b.pan && a.finePitch == b.finePitch && a.muted == b.muted && a.slide == b.slide && a.porta == b.porta
           && a.release == b.release && a.modX == b.modX && a.modY == b.modY && a.colour == b.colour && a.group == b.group;
}

bool sameNotes (const std::vector<Note>& a, const std::vector<Note>& b) noexcept
{
    return a.size() == b.size() && std::equal (a.begin(), a.end(), b.begin(), sameNote);
}

bool sameChannelLevels (const ChannelDef& a, const ChannelDef& b) noexcept
{
    return a.volume == b.volume && a.pan == b.pan;
}

bool sameChannel (const ChannelDef& a, const ChannelDef& b) noexcept
{
    return a.name == b.name && a.instrument == b.instrument && sameChannelLevels (a, b) && a.muted == b.muted && a.samplePath == b.samplePath
           && a.zones == b.zones && a.mixerTrack == b.mixerTrack && a.plugin == b.plugin && a.controllerState == b.controllerState
           && a.modelParams == b.modelParams && a.pluginState == b.pluginState && a.sampler == b.sampler && a.synth == b.synth
           && a.swingMix == b.swingMix && a.arp == b.arp && a.cutGroup == b.cutGroup && a.cutBy == b.cutBy && a.colour == b.colour;
}

bool sameSlot (const EffectSlot& a, const EffectSlot& b) noexcept
{
    return a.kind == b.kind && a.enabled == b.enabled && a.params == b.params && a.mix == b.mix && a.plugin == b.plugin
           && a.sidechain == b.sidechain && a.sidechain2 == b.sidechain2 && a.branches.size() == b.branches.size() && a.state == b.state
           && a.presetName == b.presetName && a.abOther == b.abOther && a.abShowingB == b.abShowingB;
}

bool sameEffects (const MixerTrack& a, const MixerTrack& b) noexcept
{
    return a.effects.size() == b.effects.size() && std::equal (a.effects.begin(), a.effects.end(), b.effects.begin(), sameSlot);
}

bool sameRouting (const MixerTrack& a, const MixerTrack& b) noexcept
{
    return a.sends == b.sends && a.toMaster == b.toMaster && a.masterLevel == b.masterLevel;
}

bool sameLatency (const MixerTrack& a, const MixerTrack& b) noexcept
{
    return a.latencyMs == b.latencyMs && a.latencyBypass == b.latencyBypass && a.compensateMasterLatency == b.compensateMasterLatency;
}

bool sameTrack (const MixerTrack& a, const MixerTrack& b) noexcept
{
    return a.name == b.name && a.volume == b.volume && a.pan == b.pan && a.muted == b.muted && a.solo == b.solo && sameEffects (a, b)
           && sameRouting (a, b) && a.stereo == b.stereo && a.colour == b.colour && a.invertPhase == b.invertPhase
           && a.swapChannels == b.swapChannels && a.eq == b.eq
           && a.inputChannel == b.inputChannel && a.inputStereo == b.inputStereo && a.monitor == b.monitor && a.recordSafe == b.recordSafe
           && a.outputChannel == b.outputChannel && a.outputMode == b.outputMode && sameLatency (a, b);
}

bool sameClip (const Clip& a, const Clip& b) noexcept
{
    return a.pattern == b.pattern && a.track == b.track && a.start == b.start && a.length == b.length && a.offset == b.offset
           && a.kind == b.kind && a.channel == b.channel && a.automation == b.automation && a.fadeIn == b.fadeIn && a.fadeOut == b.fadeOut
           && a.gain == b.gain && a.pan == b.pan && a.muted == b.muted && a.offsetFraction == b.offsetFraction;
}

bool sameClips (const std::vector<Clip>& a, const std::vector<Clip>& b) noexcept
{
    return a.size() == b.size() && std::equal (a.begin(), a.end(), b.begin(), sameClip);
}

bool samePoints (const Automation& a, const Automation& b) noexcept
{
    if (a.points.size() != b.points.size() || a.name != b.name || ! (a.target == b.target))
        return false;

    for (std::size_t i = 0; i < a.points.size(); ++i)
        if (a.points[i].tick != b.points[i].tick || a.points[i].value != b.points[i].value || a.points[i].tension != b.points[i].tension)
            return false;

    return true;
}

ChangeLabel label (const char* text, std::string arg = {})
{
    return { text, std::move (arg) };
}
} // namespace

ChangeLabel describeChange (const Song& before, const Song& after)
{
    // patterns and notes
    if (before.patterns.size() != after.patterns.size())
        return label (after.patterns.size() > before.patterns.size() ? TR_NOOP ("Add pattern") : TR_NOOP ("Delete pattern"));

    for (std::size_t p = 0; p < after.patterns.size(); ++p)
    {
        const auto& a = before.patterns[p];
        const auto& b = after.patterns[p];

        if (! sameNotes (a.notes, b.notes))
        {
            if (b.notes.size() > a.notes.size())
                return label (TR_NOOP ("Add notes: {0}"), b.name);
            if (b.notes.size() < a.notes.size())
                return label (TR_NOOP ("Delete notes: {0}"), b.name);
            return label (TR_NOOP ("Edit notes: {0}"), b.name);
        }

        if (a.name != b.name || a.colour != b.colour)
            return label (TR_NOOP ("Rename pattern: {0}"), b.name);
        if (a.lengthTicks != b.lengthTicks)
            return label (TR_NOOP ("Pattern length: {0}"), b.name);

        const auto sameEvents = a.events.size() == b.events.size()
                                && std::equal (a.events.begin(), a.events.end(), b.events.begin(), [] (const PatternEvents& x, const PatternEvents& y) {
                                       return x.target == y.target && x.events.size() == y.events.size()
                                              && std::equal (x.events.begin(), x.events.end(), y.events.begin(),
                                                             [] (const PatternEvent& e, const PatternEvent& f) { return e.tick == f.tick && e.value == f.value; });
                                   });
        if (! sameEvents)
            return label (TR_NOOP ("Edit events: {0}"), b.name);
    }

    // channels
    if (before.channels.size() != after.channels.size())
        return label (after.channels.size() > before.channels.size() ? TR_NOOP ("Add channel") : TR_NOOP ("Delete channel"));

    for (std::size_t c = 0; c < after.channels.size(); ++c)
    {
        const auto& a = before.channels[c];
        const auto& b = after.channels[c];

        if (sameChannel (a, b))
            continue;

        if (a.name != b.name && std::any_of (before.channels.begin(), before.channels.end(), [&b] (const ChannelDef& o) { return o.name == b.name; }))
            return label (TR_NOOP ("Move channels"));

        auto onlyMute = a;
        onlyMute.muted = b.muted;
        if (sameChannel (onlyMute, b))
            return label (TR_NOOP ("Mute: {0}"), b.name);

        auto onlyLevels = a;
        onlyLevels.volume = b.volume;
        onlyLevels.pan = b.pan;
        if (sameChannel (onlyLevels, b))
            return label (TR_NOOP ("Channel level: {0}"), b.name);

        return label (TR_NOOP ("Channel: {0}"), b.name);
    }

    // mixer
    if (before.mixer.size() != after.mixer.size())
        return label (after.mixer.size() > before.mixer.size() ? TR_NOOP ("Add insert") : TR_NOOP ("Remove insert"));

    for (std::size_t t = 0; t < after.mixer.size(); ++t)
    {
        const auto& a = before.mixer[t];
        const auto& b = after.mixer[t];

        if (sameTrack (a, b))
            continue;

        if (a.name != b.name && std::any_of (before.mixer.begin(), before.mixer.end(), [&b] (const MixerTrack& o) { return o.name == b.name; }))
            return label (TR_NOOP ("Move inserts"));
        if (! sameEffects (a, b))
            return label (TR_NOOP ("Effects: {0}"), b.name);
        if (! sameRouting (a, b))
            return label (TR_NOOP ("Routing: {0}"), b.name);
        if (a.volume != b.volume || a.pan != b.pan)
            return label (TR_NOOP ("Mixer level: {0}"), b.name);
        if (! sameLatency (a, b))
            return label (TR_NOOP ("Track latency: {0}"), b.name);
        return label (TR_NOOP ("Mixer: {0}"), b.name);
    }

    // playlist
    if (! sameClips (before.playlist, after.playlist))
    {
        if (after.playlist.size() > before.playlist.size())
            return label (TR_NOOP ("Add clips"));
        if (after.playlist.size() < before.playlist.size())
            return label (TR_NOOP ("Delete clips"));
        return label (TR_NOOP ("Edit clips"));
    }

    if (before.automations.size() != after.automations.size()
        || ! std::equal (before.automations.begin(), before.automations.end(), after.automations.begin(), samePoints))
        return label (TR_NOOP ("Automation"));

    if (before.bpm != after.bpm)
        return label (TR_NOOP ("Tempo"));
    if (before.swing != after.swing)
        return label (TR_NOOP ("Swing"));
    if (before.beatsPerBar != after.beatsPerBar)
        return label (TR_NOOP ("Time signature"));
    if (before.info != after.info)
        return label (TR_NOOP ("Project info"));
    if (before.masterPitch != after.masterPitch)
        return label (TR_NOOP ("Master pitch"));
    if (before.loopStart != after.loopStart || before.loopEnd != after.loopEnd)
        return label (TR_NOOP ("Time selection"));
    if (before.markers() != after.markers())
        return label (TR_NOOP ("Markers"));
    if (before.arrangements.size() != after.arrangements.size() || before.currentArrangement != after.currentArrangement)
        return label (TR_NOOP ("Arrangements"));
    if (before.playlistTracks.size() != after.playlistTracks.size())
        return label (TR_NOOP ("Playlist tracks"));

    for (std::size_t i = 0; i < after.playlistTracks.size(); ++i)
        if (before.playlistTracks[i].name != after.playlistTracks[i].name || before.playlistTracks[i].muted != after.playlistTracks[i].muted
            || before.playlistTracks[i].colour != after.playlistTracks[i].colour || before.playlistTracks[i].grouped != after.playlistTracks[i].grouped
            || before.playlistTracks[i].collapsed != after.playlistTracks[i].collapsed)
            return label (TR_NOOP ("Playlist tracks"));

    for (std::size_t i = 0; i < after.playlistTracks.size(); ++i)
        if (before.playlistTracks[i].height != after.playlistTracks[i].height)
            return label (TR_NOOP ("Track height"));

    if (before.mode != after.mode)
        return label (TR_NOOP ("Pattern / song mode"));
    if (before.currentPattern != after.currentPattern)
        return label (TR_NOOP ("Select pattern"));

    return label (TR_NOOP ("Edit"));
}

std::string describeChangeText (const Song& before, const Song& after)
{
    const auto l = describeChange (before, after);
    std::string text = l.text;

    if (const auto at = text.find ("{0}"); at != std::string::npos)
        text.replace (at, 3, l.arg);

    return text;
}
} // namespace lynx
