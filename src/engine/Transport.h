#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "core/Song.h"

namespace lynx
{
// Transport state shared between the UI and the audio thread.
// Atomics only: the audio thread never blocks waiting for the UI.
// The position (in ticks, core PPQ) is written only by the audio thread.
struct Transport
{
    std::atomic<bool> playing { false };
    std::atomic<bool> metronome { false };
    // the metronome's sound (FL's Metronome sound menu, REA round 165f): 0 click, 1 hi-hat, 2 tick, 3 beep, 4 cowbell, 5 pulse
    std::atomic<int> metronomeSound { 0 };
    std::atomic<float> metronomeVolume { 1.0f }; // the metronome's volume (0..1.25, 1 = the default click level)
    std::atomic<bool> recording { false };
    std::atomic<double> bpm { 130.0 };
    std::atomic<int> beatsPerBar { 4 };

    // Playlist time-signature changes for the metronome (song mode): the bar restarts at each
    // `signatureTick[i]` with `signatureLength[i]` ticks. The UI writes under a "seqlock": odd version
    // while writing; readers check the version before and after.
    static constexpr int maxSignatures = 64;
    std::array<std::atomic<std::int64_t>, maxSignatures> signatureTick {}, signatureLength {};
    std::atomic<int> signatureCount { 0 };
    std::atomic<std::uint32_t> signatureVersion { 0 };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<double> positionTicks { 0.0 };
    // Count-in before recording: ticks of pre-roll left. While > 0 playback has not started (`playing` is
    // false), the metronome clicks the count and the audio thread counts it down, then starts playing.
    std::atomic<double> countIn { 0.0 };
    std::atomic<bool> rewindRequested { false };
    std::atomic<double> seekRequest { -1.0 }; // < 0 = none; the audio thread applies it on the next block

    // Playlist loop region (song mode only). end <= start = disabled.
    std::atomic<bool> songMode { false };
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 };
    std::atomic<double> songEnd { 0.0 }; // Song::songLength(): without a loop region Song mode loops there

    double ticksPerSample() const noexcept
    {
        return bpm.load (std::memory_order_relaxed) * ppq / (60.0 * sampleRate.load (std::memory_order_relaxed));
    }

    double positionInBeats() const noexcept
    {
        return positionTicks.load (std::memory_order_relaxed) / ppq;
    }
};
} // namespace lynx
