#pragma once

// Plugin delay compensation (PDC), in the "per-edge" model FL uses
// (docs/analise/REA_rodada2.md §1): in routing order,
//   inAlign[T] = max(latency of the instruments feeding T, outLat[S] of each S -> T)
//   outLat[T]  = inAlign[T] + sum of the latencies of T's enabled effects
// and each edge (instrument -> track, track -> track) delays by what's missing to reach
// the destination's inAlign. The longest path is never delayed.
// FL's manual track latency (MixerTrack::latencyMs, REA round 165f-mixer: FUN_010ff3b0) adds to outLat[T] (it may be
// negative), and a track whose latency compensation is bypassed "Always" gets no delay on its outputs (FUN_010ffca0) —
// its outLat still counts at the destination. "When recording or not playing" is planned like "Never"; the renderer
// skips those delays while the transport is stopped or recording.

#include <memory>
#include <vector>

#include "core/Song.h"

namespace lynx
{
struct PdcPlan
{
    std::vector<int> channelDelay;             // per channel, in samples
    std::vector<std::vector<int>> sendDelay;   // [track][k]: k = 0 Master, k = 1 + i for sends[i], then one per
                                               // Fruity Send slot (fruitySendsOf order): k = 1 + sends.size() + j
    std::vector<int> inAlign, outLat;          // per track
    int total = 0;                              // latency at the Master output

    bool any() const noexcept;
};

// `channelLatency[c]` = latency of the channel's instrument; `slotLatency[t][s]` = of the slot's
// effect (0 if disabled). Vectors smaller than the Song count as 0.
// `sampleRate` converts the tracks' manual latencies (ms) to samples.
PdcPlan computePdc (const Song& song, const std::vector<int>& channelLatency, const std::vector<std::vector<int>>& slotLatency,
                    double sampleRate = 44100.0);

// A track's manual latency in samples (FL rounds ms × rate / 1000).
int manualLatencySamples (const MixerTrack& track, double sampleRate) noexcept;

// Stereo delay line with an integer delay, allocated on creation. process() does not allocate.
class DelayLine
{
public:
    explicit DelayLine (int delaySamples);

    int length() const noexcept { return delay; }
    void process (float* left, float* right, int n) noexcept; // in place

    // Delays `source * level` and adds it into `dest`, in a single pass. With a null source (silent
    // source) pushes silence; when the line is already all silence, does nothing.
    void mixInto (const float* sourceLeft, const float* sourceRight, float level, float* destLeft, float* destRight, int n) noexcept;

    // Empties the line (no allocation; nothing to do when it already holds only silence): used when a bypassed edge
    // goes back to being compensated, so it starts from silence instead of replaying what it held.
    void clear() noexcept;

private:
    int delay;
    std::vector<float> left, right; // planar ring
    int position = 0;
    int quiet = 0; // consecutive silent samples already written (up to delay)
};
} // namespace lynx
