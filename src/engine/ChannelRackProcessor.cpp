#include "engine/ChannelRackProcessor.h"

namespace lynx
{
ChannelRackProcessor::ChannelRackProcessor (Transport& t, SnapshotExchange<RackState>& x, LiveEventQueue& l,
                                            LiveEventQueue& midi, MeterBank& m, WorkerPool* workers)
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      transport (t),
      exchange (x),
      live (l),
      midiLive (midi),
      meters (m)
{
    renderer.setWorkerPool (workers);
}

void ChannelRackProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    renderer.prepare (sampleRate, maximumExpectedSamplesPerBlock);
    scratch.setSize (2, maximumExpectedSamplesPerBlock);
}

void ChannelRackProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto numSamples = buffer.getNumSamples();
    const auto* state = exchange.acquire();
    const auto playing = transport.playing.load (std::memory_order_relaxed);
    const auto startTick = transport.positionTicks.load (std::memory_order_relaxed);
    renderer.setTempoOverride (transport.externalBpm.load (std::memory_order_relaxed));

    if (buffer.getNumChannels() >= 2)
    {
        renderer.process (state, playing, startTick, numSamples, buffer.getWritePointer (0), buffer.getWritePointer (1), &live, metering.load (std::memory_order_relaxed) ? &meters : nullptr, &midiLive);
    }
    else if (numSamples <= scratch.getNumSamples())
    {
        renderer.process (state, playing, startTick, numSamples, scratch.getWritePointer (0), scratch.getWritePointer (1), &live, metering.load (std::memory_order_relaxed) ? &meters : nullptr, &midiLive);

        if (buffer.getNumChannels() == 1)
        {
            buffer.copyFrom (0, 0, scratch, 0, 0, numSamples);
            buffer.addFrom (0, 0, scratch, 1, 0, numSamples);
            buffer.applyGain (0.5f);
        }
    }
    else
    {
        buffer.clear();
    }

    // tempo automation: the transport advances at the tempo the renderer used
    if (state != nullptr && state->tempoAutomation >= 0 && transport.externalBpm.load (std::memory_order_relaxed) <= 0.0)
        transport.bpm.store (renderer.currentBpm(), std::memory_order_relaxed);
}
} // namespace lynx
