#pragma once

#include <array>
#include <vector>

#include <juce_core/juce_core.h>

#include "core/MetronomeVoice.h"
#include "engine/Transport.h"

namespace lynx
{
// The metronome (core/MetronomeVoice.h has FL's measured behaviour): one click per beat, the downbeat two semitones
// up and louder. It no longer plays straight to the output: the Channel Rack node renders it each block and the
// mixer adds it to the "Metronome track" (Transport::metronomeTrack; the Master by default), as FL does.
// Toggled by Transport::metronome; it also counts the count-in.
class Metronome final
{
public:
    // Loads the five sounds: FL's own files from `flDataFolder` (FL's "Data" folder), LYNX's stand-ins for any that
    // is missing. Off the audio thread.
    Metronome (const Transport& transport, const juce::File& flDataFolder);

    void prepare (double sampleRate, int maximumBlockSize);

    // Audio thread: the block's click into left/right (cleared first). False = silent block (nothing was written
    // beyond the clearing).
    bool render (float* left, float* right, int numSamples) noexcept;

    const metronome::Sample& sound (int index) const noexcept { return sounds[(std::size_t) metronome::clampSound (index)]; }
    // The block buffers the Channel Rack hands to the mixer (sized by prepare()).
    float* bufferLeft() noexcept { return left.data(); }
    float* bufferRight() noexcept { return right.data(); }
    int capacity() const noexcept { return (int) left.size(); }

    // The five sounds, FL's files where `flDataFolder`\System has them.
    static std::array<metronome::Sample, metronome::soundCount> loadSounds (const juce::File& flDataFolder);

private:
    const Transport& transport;
    std::array<metronome::Sample, metronome::soundCount> sounds;
    metronome::Voice voice;
    std::vector<float> left, right;
    std::int64_t lastBeat = -1;

    // local copy of the Transport's time-signature table (start, bar length in ticks)
    std::array<std::pair<std::int64_t, std::int64_t>, Transport::maxSignatures> signatures {};
    int signatureCount = 0;
    std::uint32_t signatureVersion = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Metronome)
};
} // namespace lynx
