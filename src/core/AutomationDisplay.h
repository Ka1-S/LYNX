#pragma once

// Knobs that move with their automation (issue #12, as in FL). The audio thread applies automation clips and pattern
// event lanes to the instruments, effects and mixer without writing them into the Song, so the controls on screen kept
// showing the Song's own value while the automation played. This evaluates, on the UI thread, the value each automated
// target has at the playhead — the same rules RackRenderer::updateAutomation uses — so an editor can show it. The
// audio thread does no extra work for it.
//
// Cost: the tables are rebuilt only when the Song changes (`songStamp`); an update evaluates each automated target once
// (a few binary-search-free clip walks), and controls look their target up in a small sorted vector.

#include "core/Song.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace lynx
{
class AutomationDisplay
{
public:
    // UI thread, from a timer. `tick` = the transport position; `playing` = the transport runs. While stopped no
    // automated value is shown (the controls show the Song's value, which a hand move edits, as in FL). Returns true
    // when a shown value changed (the editors then repaint their automated controls).
    bool update (const Song& song, std::uint64_t songStamp, double tick, bool playing)
    {
        if (songStamp != builtStamp || ! built)
        {
            build (song);
            builtStamp = songStamp;
            built = true;
        }

        std::vector<Entry> next;
        if (playing && ! sources.empty())
        {
            next.reserve (targets.size());
            for (const auto& s : sources)
            {
                const auto v = evaluate (song, s, tick);
                if (std::isnan (v))
                    continue;
                // the last source of a target wins (automations, then event lanes: the renderer's order)
                auto it = std::find_if (next.begin(), next.end(), [&s] (const Entry& e) { return e.target == s.target; });
                if (it == next.end())
                    next.push_back ({ s.target, v });
                else
                    it->value = v;
            }
            std::sort (next.begin(), next.end(), [] (const Entry& a, const Entry& b) { return less (a.target, b.target); });
        }

        const auto changed = next.size() != values.size()
                             || ! std::equal (next.begin(), next.end(), values.begin(), [] (const Entry& a, const Entry& b) {
                                    return a.target == b.target && std::abs (a.value - b.value) < 1.0e-5f;
                                });
        values.swap (next);
        if (changed)
            ++generation;
        return changed;
    }

    // The value `target` is automated to now (0..1, the Song's target scale), if it is.
    std::optional<float> valueOf (const AutomationTarget& target) const noexcept
    {
        if (values.empty())
            return std::nullopt;
        const auto it = std::lower_bound (values.begin(), values.end(), target,
                                          [] (const Entry& e, const AutomationTarget& t) { return less (e.target, t); });
        if (it == values.end() || ! (it->target == target))
            return std::nullopt;
        return it->value;
    }

    // `base` (the Song's value), or the automated one while it plays
    float shown (const AutomationTarget& target, float base) const noexcept
    {
        const auto v = valueOf (target);
        return v ? *v : base;
    }

    bool any() const noexcept { return ! values.empty(); }
    // changes whenever a shown value does (an editor compares it with the one it last drew)
    std::uint32_t getGeneration() const noexcept { return generation; }
    // the targets automated in the Song (shown or not), for tests and editors
    const std::vector<AutomationTarget>& automatedTargets() const noexcept { return targets; }

private:
    struct Entry
    {
        AutomationTarget target;
        float value = 0.0f;
    };
    struct Source
    {
        AutomationTarget target;
        int automation = -1;       // song.automations index, or -1 for a pattern event lane
        std::vector<int> clips;    // automation: its clips, sorted by start
        int pattern = -1, lane = -1;
    };

    static bool less (const AutomationTarget& a, const AutomationTarget& b) noexcept
    {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.index != b.index) return a.index < b.index;
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.param < b.param;
    }

    void build (const Song& song)
    {
        sources.clear();
        targets.clear();
        patternClips.assign (song.patterns.size(), {});

        std::vector<std::vector<int>> clips (song.automations.size());
        for (int i = 0; i < (int) song.playlist.size(); ++i)
        {
            const auto& c = song.playlist[(std::size_t) i];
            if (c.length <= 0 || c.muted || song.isTrackMuted (c.track))
                continue;
            if (c.kind == ClipKind::automation && c.automation >= 0 && c.automation < (int) clips.size())
                clips[(std::size_t) c.automation].push_back (i);
            else if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) song.patterns.size())
                patternClips[(std::size_t) c.pattern].push_back (i);
        }
        const auto byStart = [&song] (int a, int b) { return song.playlist[(std::size_t) a].start < song.playlist[(std::size_t) b].start; };

        for (int a = 0; a < (int) song.automations.size(); ++a)
        {
            const auto& automation = song.automations[(std::size_t) a];
            auto& list = clips[(std::size_t) a];
            if (list.empty() || automation.isLive() || automation.target.kind == AutomationTargetKind::none
                || automation.target.kind == AutomationTargetKind::tempo)
                continue;
            std::sort (list.begin(), list.end(), byStart);
            Source s;
            s.target = automation.target;
            s.automation = a;
            s.clips = std::move (list);
            sources.push_back (std::move (s));
        }
        for (auto& list : patternClips)
            std::sort (list.begin(), list.end(), byStart);
        for (int p = 0; p < (int) song.patterns.size(); ++p)
            for (int l = 0; l < (int) song.patterns[(std::size_t) p].events.size(); ++l)
            {
                const auto& lane = song.patterns[(std::size_t) p].events[(std::size_t) l];
                if (lane.events.empty() || lane.target.kind == AutomationTargetKind::none || lane.target.kind == AutomationTargetKind::tempo)
                    continue;
                Source s;
                s.target = lane.target;
                s.pattern = p;
                s.lane = l;
                sources.push_back (std::move (s));
            }

        for (const auto& s : sources)
            if (std::find (targets.begin(), targets.end(), s.target) == targets.end())
                targets.push_back (s.target);
    }

    float evaluate (const Song& song, const Source& s, double tick) const
    {
        constexpr auto none = std::numeric_limits<float>::quiet_NaN();
        if (s.automation >= 0)
        {
            if (song.mode != PlayMode::song || s.automation >= (int) song.automations.size())
                return none; // in pattern mode the Playlist does not play (the renderer keeps the last value)
            const auto& automation = song.automations[(std::size_t) s.automation];
            const Clip* before = nullptr;
            for (const auto i : s.clips)
            {
                if (i >= (int) song.playlist.size())
                    return none; // (the Song changed without a new stamp)
                const auto& c = song.playlist[(std::size_t) i];
                if (tick < (double) c.start)
                    break;
                if (tick < (double) c.end())
                    return automation.valueAt (tick - (double) c.start + (double) c.offset);
                before = &c;
            }
            if (before != nullptr)
                return automation.valueAt ((double) (before->length + before->offset));
            return s.clips.front() < (int) song.playlist.size() ? automation.valueAt ((double) song.playlist[(std::size_t) s.clips.front()].offset) : none;
        }

        if (s.pattern < 0 || s.pattern >= (int) song.patterns.size())
            return none;
        const auto& pattern = song.patterns[(std::size_t) s.pattern];
        if (s.lane < 0 || s.lane >= (int) pattern.events.size())
            return none;
        const auto& events = pattern.events[(std::size_t) s.lane];

        if (song.mode != PlayMode::song)
        {
            if (s.pattern != song.currentPattern)
                return none;
            const auto length = (double) std::max<std::int64_t> (1, pattern.effectiveLength (song.beatsPerBar));
            return events.valueAt (std::fmod (std::max (0.0, tick), length));
        }

        auto value = none;
        if (s.pattern >= (int) patternClips.size())
            return none;
        for (const auto i : patternClips[(std::size_t) s.pattern])
        {
            if (i >= (int) song.playlist.size())
                return none;
            const auto& c = song.playlist[(std::size_t) i];
            if (tick < (double) c.start)
                break;
            value = tick < (double) c.end() ? events.valueAt (tick - (double) c.start + (double) c.offset)
                                            : events.valueAt ((double) (c.offset + c.length) - 1.0);
        }
        return value;
    }

    std::vector<Source> sources;
    std::vector<AutomationTarget> targets;
    std::vector<std::vector<int>> patternClips;
    std::vector<Entry> values;
    std::uint64_t builtStamp = 0;
    bool built = false;
    std::uint32_t generation = 0;
};
} // namespace lynx
