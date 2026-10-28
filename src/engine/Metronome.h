#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine/Transport.h"

namespace lynx
{
// Metronome: one click per beat, higher-pitched on the first beat
// of the bar. Toggled by Transport::metronome.
class Metronome final : public juce::AudioProcessor
{
public:
    explicit Metronome (const Transport& transport);

    const juce::String getName() const override { return "Metronome"; }

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    const Transport& transport;
    double currentSampleRate = 44100.0;
    std::int64_t lastBeat = -1;
    double clickPhase = 0.0, clickPhase2 = 0.0;
    double clickPhaseDelta = 0.0, clickPhaseDelta2 = 0.0;
    int clickSamplesLeft = 0, clickLength = 1, clickSound = 0;
    float clickAccent = 1.0f;
    std::uint32_t noiseState = 0x9e3779b9u;
    float hp1 = 0.0f, hp2 = 0.0f, lastNoise = 0.0f, lastHp = 0.0f;
    float voiceSample() noexcept;

    // local copy of the Transport's time-signature table (start, bar length in ticks)
    std::array<std::pair<std::int64_t, std::int64_t>, Transport::maxSignatures> signatures {};
    int signatureCount = 0;
    std::uint32_t signatureVersion = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Metronome)
};
} // namespace lynx
