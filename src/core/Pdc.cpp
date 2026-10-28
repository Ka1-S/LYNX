#include "core/Pdc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lynx
{
bool PdcPlan::any() const noexcept
{
    if (std::any_of (channelDelay.begin(), channelDelay.end(), [] (int d) { return d > 0; }))
        return true;

    for (const auto& t : sendDelay)
        if (std::any_of (t.begin(), t.end(), [] (int d) { return d > 0; }))
            return true;

    return false;
}

int manualLatencySamples (const MixerTrack& track, double sampleRate) noexcept
{
    const auto ms = std::clamp (track.latencyMs, -maxTrackLatencyMs, maxTrackLatencyMs);
    return ms == 0.0f || ! (sampleRate > 0.0) ? 0 : (int) std::lround ((double) ms * sampleRate * 0.001);
}

PdcPlan computePdc (const Song& song, const std::vector<int>& channelLatency, const std::vector<std::vector<int>>& slotLatency,
                    double sampleRate)
{
    const auto tracks = std::max<int> (1, (int) song.mixer.size());
    PdcPlan plan;
    plan.inAlign.assign ((std::size_t) tracks, 0);
    plan.outLat.assign ((std::size_t) tracks, 0);
    plan.channelDelay.assign (song.channels.size(), 0);
    plan.sendDelay.assign ((std::size_t) tracks, {});

    auto channelLat = [&] (std::size_t c) { return c < channelLatency.size() ? std::max (0, channelLatency[c]) : 0; };
    auto trackOf = [&] (const ChannelDef& c) { return std::clamp (c.mixerTrack, 0, tracks - 1); };

    // own input latency: the slowest instrument feeding the track
    for (std::size_t c = 0; c < song.channels.size(); ++c)
    {
        auto& in = plan.inAlign[(std::size_t) trackOf (song.channels[c])];
        in = std::max (in, channelLat (c));
    }

    auto effectsLatency = [&] (int t) {
        int sum = 0;

        if (t < (int) song.mixer.size() && t < (int) slotLatency.size())
        {
            const auto& slots = song.mixer[(std::size_t) t].effects;

            for (std::size_t s = 0; s < slots.size() && s < slotLatency[(std::size_t) t].size(); ++s)
                if (song.slotActive (slots[s]))
                    sum += std::max (0, slotLatency[(std::size_t) t][s]);
        }

        return sum;
    };

    // the latency of a Fruity Send slot's signal: the insert's input plus the enabled slots before it
    auto tapLatency = [&] (int t, int slot) {
        int sum = plan.inAlign[(std::size_t) t];
        if (t < (int) slotLatency.size())
        {
            const auto& slots = song.mixer[(std::size_t) t].effects;
            for (int s = 0; s < slot && s < (int) slots.size() && s < (int) slotLatency[(std::size_t) t].size(); ++s)
                if (song.slotActive (slots[(std::size_t) s]))
                    sum += std::max (0, slotLatency[(std::size_t) t][(std::size_t) s]);
        }
        return sum;
    };

    // inserts in order: when T is processed, all its sources already have been
    const auto order = song.mixerOrder();

    auto manual = [&] (int t) { return t < (int) song.mixer.size() ? manualLatencySamples (song.mixer[(std::size_t) t], sampleRate) : 0; };

    for (auto t : order)
    {
        plan.outLat[(std::size_t) t] = plan.inAlign[(std::size_t) t] + effectsLatency (t) + manual (t);
        const auto& track = song.mixer[(std::size_t) t];

        if (track.toMaster)
            plan.inAlign[0] = std::max (plan.inAlign[0], plan.outLat[(std::size_t) t]);

        for (const auto& s : track.sends)
            if (s.target > 0 && s.target < tracks)
                plan.inAlign[(std::size_t) s.target] = std::max (plan.inAlign[(std::size_t) s.target], plan.outLat[(std::size_t) t]);

        // Fruity Send slots leave the chain after the slots before them
        for (const auto& [slot, target] : fruitySendsOf (song, t))
            plan.inAlign[(std::size_t) target] = std::max (plan.inAlign[(std::size_t) target], tapLatency (t, slot));
    }

    plan.outLat[0] = plan.inAlign[0] + effectsLatency (0) + manual (0);
    plan.total = std::max (0, plan.outLat[0]);

    // per-edge delays (what's missing to reach the destination's alignment)
    for (std::size_t c = 0; c < song.channels.size(); ++c)
        plan.channelDelay[c] = plan.inAlign[(std::size_t) trackOf (song.channels[c])] - channelLat (c);

    for (int t = 1; t < (int) song.mixer.size(); ++t)
    {
        const auto& track = song.mixer[(std::size_t) t];
        auto& delays = plan.sendDelay[(std::size_t) t];
        const auto taps = fruitySendsOf (song, t);
        delays.assign (1 + track.sends.size() + taps.size(), 0);
        // bypassed "Always": its routes are not delayed (FL's Fruity Send slots still are)
        const auto bypassed = track.latencyBypass == LatencyBypass::always;
        delays[0] = track.toMaster && ! bypassed ? std::max (0, plan.inAlign[0] - plan.outLat[(std::size_t) t]) : 0;

        for (std::size_t i = 0; i < track.sends.size(); ++i)
        {
            const auto target = track.sends[i].target;

            if (target > 0 && target < tracks && ! bypassed)
                delays[1 + i] = std::max (0, plan.inAlign[(std::size_t) target] - plan.outLat[(std::size_t) t]);
        }

        for (std::size_t j = 0; j < taps.size(); ++j)
            delays[1 + track.sends.size() + j] = std::max (0, plan.inAlign[(std::size_t) taps[j].second] - tapLatency (t, taps[j].first));
    }

    return plan;
}

DelayLine::DelayLine (int delaySamples)
    : delay (std::max (1, delaySamples)),
      left ((std::size_t) delay, 0.0f),
      right ((std::size_t) delay, 0.0f),
      quiet (delay)
{
}

void DelayLine::process (float* l, float* r, int n) noexcept
{
    // in contiguous ring segments: swap what comes in for what goes out
    for (int done = 0; done < n;)
    {
        const auto run = std::min (n - done, delay - position);
        auto* bl = left.data() + position;
        auto* br = right.data() + position;

        for (int i = 0; i < run; ++i)
        {
            std::swap (bl[i], l[done + i]);
            std::swap (br[i], r[done + i]);
        }

        done += run;
        position = (position + run) % delay;
    }

    quiet = 0;
}

void DelayLine::clear() noexcept
{
    if (quiet >= delay)
        return;
    std::memset (left.data(), 0, sizeof (float) * (std::size_t) delay);
    std::memset (right.data(), 0, sizeof (float) * (std::size_t) delay);
    quiet = delay;
}

void DelayLine::mixInto (const float* sl, const float* sr, float level, float* dl, float* dr, int n) noexcept
{
    if (sl == nullptr)
    {
        if (quiet >= delay)
            return; // all zeros inside: the output would be silence and the position doesn't matter

        quiet += n;
    }
    else
    {
        quiet = 0;
    }

    for (int done = 0; done < n;)
    {
        const auto run = std::min (n - done, delay - position);
        auto* bl = left.data() + position;
        auto* br = right.data() + position;
        auto* ol = dl + done;
        auto* orr = dr + done;

        if (sl != nullptr)
        {
            const auto* il = sl + done;
            const auto* ir = sr + done;

            for (int i = 0; i < run; ++i)
            {
                ol[i] += bl[i];
                orr[i] += br[i];
                bl[i] = il[i] * level;
                br[i] = ir[i] * level;
            }
        }
        else
        {
            for (int i = 0; i < run; ++i)
            {
                ol[i] += bl[i];
                orr[i] += br[i];
                bl[i] = 0.0f;
                br[i] = 0.0f;
            }
        }

        done += run;
        position = (position + run) % delay;
    }
}
} // namespace lynx
