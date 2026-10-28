#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "core/LiveEventQueue.h"
#include "core/RackRenderer.h"
#include "core/SnapshotExchange.h"
#include "engine/Transport.h"

namespace lynx
{
// Graph node that plays the Channel Rack: takes the latest song snapshot
// and renders the built-in instruments with the sequencer.
class ChannelRackProcessor final : public juce::AudioProcessor
{
public:
    ChannelRackProcessor (Transport& transport, SnapshotExchange<RackState>& exchange, LiveEventQueue& live,
                          LiveEventQueue& midiLive, MeterBank& meters, WorkerPool* workers = nullptr);

    const juce::String getName() const override { return "Channel Rack"; }

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

    // Meters (for the UI); export turns them off to avoid wasting CPU.
    void setMetering (bool on) noexcept { metering.store (on); }

    // External audio input of the inserts (set before the device starts).
    void setLiveInput (LiveInput* input) noexcept { renderer.setLiveInput (input); }
    // Hardware output buses of the inserts (REA round 156).
    void setHardwareOutput (HardwareOutput* out) noexcept { renderer.setHardwareOutput (out); }
    void setPerformance (perf::Live* live) noexcept { renderer.setPerformance (live); }
    // The renderer (its per-unit CPU profile, REA round 87).
    RackRenderer& getRenderer() noexcept { return renderer; }
    // Per instrument / effect processing time (Plugin performance monitor).
    ProcessTimes& processTimes() noexcept { return renderer.processTimes(); }

private:
    std::atomic<bool> metering { true };
    Transport& transport;
    SnapshotExchange<RackState>& exchange;
    LiveEventQueue& live;
    LiveEventQueue& midiLive;
    MeterBank& meters;
    RackRenderer renderer;
    juce::AudioBuffer<float> scratch; // for mono output

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRackProcessor)
};
} // namespace lynx
