#pragma once

// FL's "Last tweaked" helpers (round 162d, issue #8): the value text the toolbar's last-tweaked read-out shows, and
// the filter that turns a third-party plugin's own parameter changes (many plugins never send begin/end gestures)
// into "the user moved this control" while ignoring preset loads and the plugin's internal sweeps. No JUCE: tested
// in tests/Look162Tests.inc.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Effects.h"
#include "core/Song.h"

namespace lynx::lasttweak
{
// The value of `target` at `value01` (its 0..1 automation value) in the unit its control shows: channel volume and
// mixes in %, mixer volume in dB, pans as FL writes them (L50 / C / R50), tempo in BPM, LYNX effect parameters in their
// own unit. Hosted plugin parameters return "" (the plugin itself formats them).
inline std::string valueText (const Song& song, const AutomationTarget& t, float value01)
{
    char text[64] {};
    const auto v = std::clamp (value01, 0.0f, 1.0f);
    switch (t.kind)
    {
        case AutomationTargetKind::channelVolume:
        case AutomationTargetKind::effectMix: std::snprintf (text, sizeof (text), "%d%%", (int) std::lround (v * 100.0f)); break;
        case AutomationTargetKind::mixerVolume:
        {
            const auto gain = faderGain (v);
            if (gain <= 1.0e-5f)
                return "-inf dB";
            std::snprintf (text, sizeof (text), "%.1f dB", 20.0 * std::log10 ((double) gain));
            break;
        }
        case AutomationTargetKind::channelPan:
        case AutomationTargetKind::mixerPan:
        {
            const auto percent = (int) std::lround (std::abs (v * 2.0f - 1.0f) * 100.0f);
            if (percent == 0)
                return "C";
            std::snprintf (text, sizeof (text), "%c%d", v < 0.5f ? 'L' : 'R', percent);
            break;
        }
        case AutomationTargetKind::tempo: std::snprintf (text, sizeof (text), "%.2f BPM", tempoFromValue (v)); break;
        case AutomationTargetKind::effectParam:
        {
            if (t.index < 0 || t.index >= (int) song.mixer.size())
                return {};
            const auto& effects = song.mixer[(std::size_t) t.index].effects;
            if (t.slot < 0 || t.slot >= (int) effects.size())
                return {};
            const auto& slot = effects[(std::size_t) t.slot];
            if (slot.kind == EffectKind::plugin || slot.kind == EffectKind::patcher || t.param < 0 || t.param >= maxEffectParams)
                return {};
            return formatEffectParam (slot.kind, t.param, v, song.bpm, slot.params);
        }
        case AutomationTargetKind::channelParam:
        case AutomationTargetKind::none: return {};
    }
    return text;
}

// Collects a plugin's parameter changes made on the UI thread between two ticks of the plugin window's timer and
// decides which one (if any) the user moved: exactly one parameter changed (a knob drag also changes only one), and
// it is not inside a begin/end gesture (those already report themselves). Two or more parameters changing within
// the same tick are a preset load / randomise / the plugin's own macro: ignored.
class ChangeFilter
{
public:
    // a parameter changed (message thread)
    void changed (int param)
    {
        if (std::find (pending.begin(), pending.end(), param) == pending.end())
            pending.push_back (param);
        if (pending.size() > 64) // a burst this big between two ticks is never one knob
        {
            pending.clear();
            pendingOverflow = true;
        }
    }
    void gesture (int param, bool starting)
    {
        if (starting)
        {
            if (std::find (held.begin(), held.end(), param) == held.end())
                held.push_back (param);
        }
        else
            held.erase (std::remove (held.begin(), held.end(), param), held.end());
    }

    // the timer tick: the parameter to report as tweaked, or −1. `userActive` = the plugin's window is the active
    // window or under the mouse (otherwise the change came from automation, a link or the plugin itself).
    int tick (bool userActive)
    {
        auto moved = pending;
        pending.clear();
        moved.erase (std::remove_if (moved.begin(), moved.end(), [this] (int p) { return std::find (held.begin(), held.end(), p) != held.end(); }),
                     moved.end());
        if (! userActive || moved.size() != 1 || pendingOverflow)
        {
            pendingOverflow = false;
            return -1;
        }
        return moved.front();
    }

private:
    std::vector<int> pending, held;
    bool pendingOverflow = false;
};
} // namespace lynx::lasttweak
