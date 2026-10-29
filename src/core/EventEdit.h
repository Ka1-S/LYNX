#pragma once

// Event editor operations (round 83): edits a pattern's parameter events (Pattern::events, like FL's pattern
// events). Values are 0..1; each event holds until the next. Pure functions, no JUCE.

#include <cstdint>
#include <vector>

#include "core/PianoRollTools.h"
#include "core/Song.h"

namespace lynx::eventedit
{
PatternEvents* findLane (Pattern& pattern, const AutomationTarget& target) noexcept;
const PatternEvents* findLane (const Pattern& pattern, const AutomationTarget& target) noexcept;
// The lane of `target`, created when missing.
PatternEvents& laneFor (Pattern& pattern, const AutomationTarget& target);
// Drops lanes without events or without a target.
void removeEmptyLanes (Pattern& pattern);

// Sets the event at `tick` (replacing one at the same tick).
void setEvent (PatternEvents& lane, std::int64_t tick, float value);
// Removes the events in [from, to).
void erase (PatternEvents& lane, std::int64_t from, std::int64_t to);
// One event per `step` over [from, to), all at `value` (what the draw tool writes for the cells it crosses).
void fill (PatternEvents& lane, std::int64_t from, std::int64_t to, float value, std::int64_t step);
// A straight line from `v0` at `from` to `v1` at `to`: one event per `step` (the line tool).
void line (PatternEvents& lane, std::int64_t from, std::int64_t to, float v0, float v1, std::int64_t step);
// value -> centre + (value - centre) * factor + offset for the events in [from, to); centre = 0.5 for bipolar
// targets (pan), else 0.
void scale (PatternEvents& lane, std::int64_t from, std::int64_t to, float factor, float offset, bool bipolar);
// Moves each event in [from, to) by up to +-amount (seeded, deterministic).
void randomize (PatternEvents& lane, std::int64_t from, std::int64_t to, float amount, std::uint32_t seed);
// Writes FL's piano roll LFO (prtools::lfoCurve) over [from, to), one event per `step`.
void applyLfo (PatternEvents& lane, std::int64_t from, std::int64_t to, const prtools::LfoOptions& options, std::int64_t step);
// FL's "Convert to automation" (event editor, round 165l): the lane's events as automation points (ticks from the
// pattern start), holding each value until the next one (an event's step) and dropping the points a straight line
// explains. sensitivity 0.4..1 like FL's form: 1 keeps every change, lower values simplify more.
std::vector<AutomationPoint> toAutomationPoints (const PatternEvents& lane, float sensitivity);
} // namespace lynx::eventedit
