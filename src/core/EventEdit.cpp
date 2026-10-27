#include "core/EventEdit.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace lynx::eventedit
{
namespace
{
void sortLane (PatternEvents& lane)
{
    std::stable_sort (lane.events.begin(), lane.events.end(), [] (const PatternEvent& a, const PatternEvent& b) { return a.tick < b.tick; });
}

float clamp01 (float v) noexcept
{
    return std::clamp (v, 0.0f, 1.0f);
}
} // namespace

PatternEvents* findLane (Pattern& pattern, const AutomationTarget& target) noexcept
{
    for (auto& l : pattern.events)
        if (l.target == target)
            return &l;
    return nullptr;
}

const PatternEvents* findLane (const Pattern& pattern, const AutomationTarget& target) noexcept
{
    for (const auto& l : pattern.events)
        if (l.target == target)
            return &l;
    return nullptr;
}

PatternEvents& laneFor (Pattern& pattern, const AutomationTarget& target)
{
    if (auto* l = findLane (pattern, target))
        return *l;

    pattern.events.push_back ({ target, {} });
    return pattern.events.back();
}

void removeEmptyLanes (Pattern& pattern)
{
    pattern.events.erase (std::remove_if (pattern.events.begin(), pattern.events.end(),
                                          [] (const PatternEvents& l) { return l.events.empty() || l.target.kind == AutomationTargetKind::none; }),
                          pattern.events.end());
}

void setEvent (PatternEvents& lane, std::int64_t tick, float value)
{
    tick = std::max<std::int64_t> (0, tick);
    auto it = std::lower_bound (lane.events.begin(), lane.events.end(), tick, [] (const PatternEvent& e, std::int64_t t) { return e.tick < t; });

    if (it != lane.events.end() && it->tick == tick)
        it->value = clamp01 (value);
    else
        lane.events.insert (it, { tick, clamp01 (value) });
}

void erase (PatternEvents& lane, std::int64_t from, std::int64_t to)
{
    lane.events.erase (std::remove_if (lane.events.begin(), lane.events.end(), [from, to] (const PatternEvent& e) { return e.tick >= from && e.tick < to; }),
                       lane.events.end());
}

void fill (PatternEvents& lane, std::int64_t from, std::int64_t to, float value, std::int64_t step)
{
    line (lane, from, to, value, value, step);
}

void line (PatternEvents& lane, std::int64_t from, std::int64_t to, float v0, float v1, std::int64_t step)
{
    step = std::max<std::int64_t> (1, step);
    from = std::max<std::int64_t> (0, from);

    if (to <= from)
        to = from + step;

    erase (lane, from, to);
    const auto span = (double) std::max<std::int64_t> (1, to - from - step);

    for (auto t = from; t < to; t += step)
    {
        const auto f = to - from > step ? (double) (t - from) / span : 0.0;
        lane.events.push_back ({ t, clamp01 ((float) (v0 + (v1 - v0) * std::min (1.0, f))) });
    }

    sortLane (lane);
}

void scale (PatternEvents& lane, std::int64_t from, std::int64_t to, float factor, float offset, bool bipolar)
{
    const auto centre = bipolar ? 0.5f : 0.0f;

    for (auto& e : lane.events)
        if (e.tick >= from && e.tick < to)
            e.value = clamp01 (centre + (e.value - centre) * factor + offset);
}

void randomize (PatternEvents& lane, std::int64_t from, std::int64_t to, float amount, std::uint32_t seed)
{
    std::mt19937 random (seed);
    std::uniform_real_distribution<float> spread (-1.0f, 1.0f);

    for (auto& e : lane.events)
        if (e.tick >= from && e.tick < to)
            e.value = clamp01 (e.value + spread (random) * amount);
}

void applyLfo (PatternEvents& lane, std::int64_t from, std::int64_t to, const prtools::LfoOptions& options, std::int64_t step)
{
    step = std::max<std::int64_t> (1, step);
    from = std::max<std::int64_t> (0, from);

    if (to <= from)
        return;

    const auto curve = prtools::lfoCurve (options, to - from);
    erase (lane, from, to);

    for (auto t = from; t < to; t += step)
        lane.events.push_back ({ t, curve[(std::size_t) (t - from)] });

    sortLane (lane);
}
std::vector<AutomationPoint> toAutomationPoints (const PatternEvents& lane, float sensitivity)
{
    std::vector<AutomationPoint> pts;
    const auto& ev = lane.events;
    if (ev.empty())
        return pts;
    // a step per event: the value jumps at the event (two points at the same tick), then holds
    for (std::size_t i = 0; i < ev.size(); ++i)
    {
        if (i == 0)
            pts.push_back ({ ev[i].tick, ev[i].value, 0.0f });
        else if (std::abs (ev[i].value - ev[i - 1].value) > 1.0e-6f)
        {
            pts.push_back ({ ev[i].tick, ev[i - 1].value, 0.0f });
            pts.push_back ({ ev[i].tick, ev[i].value, 0.0f });
        }
    }
    if (pts.back().tick != ev.back().tick)
        pts.push_back ({ ev.back().tick, ev.back().value, 0.0f });
    // simplification: points within `eps` of the line through their neighbours go (Ramer-Douglas-Peucker on value)
    const auto eps = (1.0 - std::clamp ((double) sensitivity, 0.4, 1.0)) / 0.6 * 0.08; // 1 -> exact, 0.4 -> 8 %
    if (eps <= 0.0 || pts.size() < 3)
        return pts;
    std::vector<bool> keep (pts.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> stack { { 0, pts.size() - 1 } };
    while (! stack.empty())
    {
        const auto [a, b] = stack.back();
        stack.pop_back();
        double worst = -1.0;
        std::size_t at = a;
        for (auto k = a + 1; k < b; ++k)
        {
            const auto span = (double) (pts[b].tick - pts[a].tick);
            const auto t = span > 0.0 ? (double) (pts[k].tick - pts[a].tick) / span : 0.5;
            const auto d = std::abs ((double) pts[k].value - ((double) pts[a].value + ((double) pts[b].value - pts[a].value) * t));
            if (d > worst)
            {
                worst = d;
                at = k;
            }
        }
        if (worst > eps)
        {
            keep[at] = true;
            stack.push_back ({ a, at });
            stack.push_back ({ at, b });
        }
    }
    std::vector<AutomationPoint> out;
    for (std::size_t k = 0; k < pts.size(); ++k)
        if (keep[k])
            out.push_back (pts[k]);
    return out;
}
} // namespace lynx::eventedit
