#pragma once

// An effect slot as its editor draws it (issue #11 comment 4, after issue #12): while the song plays, the parameters an
// automation clip or event lane drives show the automation's value (core/AutomationDisplay, UI side), so Parametric
// EQ 2's band handles and curve, Fruity Filter's / Delay's / Reeverb's knobs... move with the sound, as in FL. While
// the hand holds a control (a mouse button down) the Song's own values are shown: the hand move wins, as on the knobs.
//
// Cost: nothing for a slot without automation; otherwise one copy of the slot per change of the shown values (the copy
// is reused while neither the automation's values nor the slot change), checked in O(automated targets).

#include "core/AutomationDisplay.h"

namespace lynx
{
class ShownEffectSlot
{
public:
    // `fx`: the slot in the Song (may be null), mixer track `track`, slot `slot`. Returns `fx` itself or a copy with the
    // automated parameters at their shown value (valid until the next call).
    const EffectSlot* get (const AutomationDisplay& shown, const EffectSlot* fx, int track, int slot, bool handHeld)
    {
        if (fx == nullptr || handHeld || ! shown.any() || ! automates (shown, track, slot))
            return fx;

        const auto upToDate = cachedFrom == fx && generation == shown.getGeneration() && base == fx->params
                              && copy.kind == fx->kind && copy.enabled == fx->enabled && copy.mix == fx->mix
                              && copy.state == fx->state;
        if (! upToDate)
        {
            copy = *fx;
            base = fx->params;
            cachedFrom = fx;
            generation = shown.getGeneration();

            for (int p = 0; p < maxEffectParams; ++p)
                if (const auto v = shown.valueOf ({ AutomationTargetKind::effectParam, track, slot, p }))
                    copy.params[(std::size_t) p] = *v;
        }
        return &copy;
    }

    // For an editor's timer: true when a value shown for this slot changed since the last call (then repaint).
    // (Stopping changes the generation too: the controls go back to the Song's values.)
    bool changed (const AutomationDisplay& shown, int track, int slot)
    {
        if (shown.getGeneration() == seenGeneration)
            return false;
        seenGeneration = shown.getGeneration();
        return automates (shown, track, slot);
    }

    static bool automates (const AutomationDisplay& shown, int track, int slot) noexcept
    {
        for (const auto& t : shown.automatedTargets())
            if (t.kind == AutomationTargetKind::effectParam && t.index == track && t.slot == slot)
                return true;
        return false;
    }

private:
    EffectSlot copy;
    EffectParams base {};
    const EffectSlot* cachedFrom = nullptr;
    std::uint32_t generation = 0, seenGeneration = 0;
};
} // namespace lynx
