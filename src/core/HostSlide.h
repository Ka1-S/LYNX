#pragma once

// FL's slide notes on a native generator (issue #14). A slide note does not start a voice: FL's host moves the pitch
// of the channel's sounding voices to the slide note's pitch, linearly in cents, over the slide note's length; each
// voice keeps its key (its own note off still ends it) and stays at the new pitch afterwards. One HostSlide per voice:
// the engine starts it from Instrument::slideTo and adds `cents` to the voice's pitch while it renders.
//
// [INFERENCE, REA_rodada166a §4] FL's voice model (the host owns each voice's Final pitch, the generator reads it every
// tick) and FL 2026's what's new (16250 "MiniSynth: piano roll slides don't work", 18004 / 17811 "Repeated notes don't
// slide correctly"): slides are a host feature every native generator follows. LYNX's sampler and synth glide the same way.

#include <algorithm>

namespace lynx
{
struct HostSlide
{
    float cents = 0.0f;  // how far the voice has moved from its note's pitch
    float target = 0.0f; // where it is going (cents from its note's pitch)
    float left = 0.0f;   // samples still to go
    float step = 0.0f;   // cents per sample

    // glide from where the voice is now to `toCents` (from its note's pitch) over `samples`
    void start (float toCents, float samples) noexcept
    {
        target = toCents;
        left = std::max (1.0f, samples);
        step = (target - cents) / left;
    }
    // `n` samples went by: the new offset (cents)
    float advance (float n) noexcept
    {
        if (left <= 0.0f)
            return cents;
        if (n >= left)
        {
            cents = target;
            left = 0.0f;
        }
        else
        {
            cents += step * n;
            left -= n;
        }
        return cents;
    }
    bool moving() const noexcept { return left > 0.0f; }
};
} // namespace lynx
