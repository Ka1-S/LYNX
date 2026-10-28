#pragma once

#include <array>
#include <atomic>

#include "core/AudioCapture.h"
#include "core/Song.h"

namespace lynx
{
// Peaks per mixer track, written by the audio thread and read by the UI.
// Audio accumulates the maximum; the UI reads and resets (takePeak), so no peak is lost
// between two screen frames.
class MeterBank
{
public:
    void accumulate (int track, float left, float right) noexcept
    {
        if (track < 0 || track >= maxMixerTracks)
            return;

        raise (peaks[(std::size_t) track * 2], left);
        raise (peaks[(std::size_t) track * 2 + 1], right);
    }

    // UI: peak since the last read. channel 0 = left, 1 = right.
    float takePeak (int track, int channel) noexcept
    {
        if (track < 0 || track >= maxMixerTracks)
            return 0.0f;

        return peaks[(std::size_t) track * 2 + (std::size_t) channel].exchange (0.0f, std::memory_order_relaxed);
    }

    // the audio editor's recording tap (round 86): input, Master (0) or a mixer insert
    AudioCapture capture;
    // the audio logger's tap (REA round 153): the Master, while Tools > Audio logger is on
    AudioCapture logTap;
    // the real-time bounce's tap (round 165c): the Master while the song plays, during Export › "Render through the
    // hardware in real time"
    AudioCapture bounceTap;

private:
    static void raise (std::atomic<float>& slot, float value) noexcept
    {
        auto current = slot.load (std::memory_order_relaxed);

        while (value > current && ! slot.compare_exchange_weak (current, value, std::memory_order_relaxed))
        {
        }
    }

    std::array<std::atomic<float>, maxMixerTracks * 2> peaks {};
};
} // namespace lynx
