#include "engine/Metronome.h"

#include "core/ValueMath.h"

namespace lynx
{
namespace
{
constexpr double clickLengthSeconds = 0.03;
constexpr double downbeatHz = 1760.0;
constexpr double beatHz = 880.0;
constexpr float clickGain = 0.4f;

// The sounds of FL's Metronome sound menu, as LYNX's own synthesis [DFM main form: Hi-hat, Tick, Beep, Cowbell,
// Pulse]: {length in seconds, beat / downbeat frequencies}
struct SoundShape
{
    double seconds, beatHz, downbeatHz;
};
constexpr SoundShape shapes[] = {
    { clickLengthSeconds, beatHz, downbeatHz }, // LYNX's click: a sine with a falling ramp
    { 0.045, 0.0, 0.0 },                        // hi-hat: high-passed noise, a fast decay
    { 0.012, 2500.0, 3600.0 },                  // tick: a very short, bright sine
    { 0.08, 1000.0, 1500.0 },                   // beep: a flat sine with soft edges
    { 0.14, 587.0, 734.0 },                     // cowbell: two square partials (×1.44), ringing decay
    { 0.06, 330.0, 495.0 },                     // pulse: a low raised-cosine bump
};
constexpr int soundCount = (int) (sizeof (shapes) / sizeof (shapes[0]));
} // namespace

float Metronome::voiceSample() noexcept
{
    const auto t = 1.0f - (float) clickSamplesLeft / (float) clickLength; // 0 → 1 over the sound
    const auto twoPi = juce::MathConstants<double>::twoPi;
    float out = 0.0f;
    switch (clickSound)
    {
        case 1: // hi-hat
        {
            noiseState ^= noiseState << 13;
            noiseState ^= noiseState >> 17;
            noiseState ^= noiseState << 5;
            const auto noise = (float) (int) noiseState * (1.0f / 2147483648.0f);
            // two first-order high-passes (about 7 kHz at 44.1 kHz)
            hp1 = 0.35f * (hp1 + noise - lastNoise);
            lastNoise = noise;
            hp2 = 0.35f * (hp2 + hp1 - lastHp);
            lastHp = hp1;
            out = 2.4f * hp2 * std::exp (-6.0f * t);
            break;
        }
        case 2: // tick
            out = (float) std::sin (clickPhase) * std::exp (-5.0f * t);
            break;
        case 3: // beep: 5 % fade in and out
            out = (float) std::sin (clickPhase) * 0.6f * juce::jmin (1.0f, t / 0.05f, (1.0f - t) / 0.05f);
            break;
        case 4: // cowbell
        {
            const auto a = std::sin (clickPhase) >= 0.0 ? 1.0f : -1.0f;
            const auto b = std::sin (clickPhase2) >= 0.0 ? 1.0f : -1.0f;
            out = 0.28f * (a + b) * std::exp (-4.0f * t);
            clickPhase2 += clickPhaseDelta2;
            break;
        }
        case 5: // pulse
            out = (float) std::sin (clickPhase) * 0.5f * (1.0f - (float) std::cos (twoPi * t));
            break;
        default: // LYNX's click
            out = (float) std::sin (clickPhase) * (1.0f - t);
            break;
    }
    clickPhase += clickPhaseDelta;
    return out;
}

Metronome::Metronome (const Transport& t)
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      transport (t)
{
}

void Metronome::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    lastBeat = -1;
    clickSamplesLeft = 0;
}

void Metronome::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();

    const auto countIn = transport.countIn.load (std::memory_order_relaxed);
    const auto counting = countIn > 0.0 && ! transport.playing.load (std::memory_order_relaxed);

    if (! counting && (! transport.playing.load (std::memory_order_relaxed) || ! transport.metronome.load (std::memory_order_relaxed)))
    {
        lastBeat = -1;
        clickSamplesLeft = 0;
        return;
    }

    const auto beatsPerBar = juce::jmax (1, transport.beatsPerBar.load (std::memory_order_relaxed));
    const auto songMode = transport.songMode.load (std::memory_order_relaxed);

    // in song mode, bars follow the Playlist time-signature changes (consistent copy
    // of the table; if the UI is writing, the previous block's copy is kept)
    if (songMode)
    {
        const auto before = transport.signatureVersion.load (std::memory_order_acquire);

        if ((before & 1u) == 0 && before != signatureVersion)
        {
            const auto count = juce::jlimit (0, Transport::maxSignatures, transport.signatureCount.load (std::memory_order_relaxed));
            decltype (signatures) copy {};

            for (int s = 0; s < count; ++s)
                copy[(std::size_t) s] = { transport.signatureTick[(std::size_t) s].load (std::memory_order_relaxed),
                                          transport.signatureLength[(std::size_t) s].load (std::memory_order_relaxed) };

            if (transport.signatureVersion.load (std::memory_order_acquire) == before)
            {
                signatures = copy;
                signatureCount = count;
                signatureVersion = before;
            }
        }
    }

    auto isDownbeat = [&] (std::int64_t beat) {
        if (beat < 0 || ! songMode || signatureCount == 0)
            return ((beat % beatsPerBar) + beatsPerBar) % beatsPerBar == 0;

        const auto tick = beat * ppq;
        auto index = 0;
        for (int s = 1; s < signatureCount; ++s)
            if (signatures[(std::size_t) s].first <= tick)
                index = s;

        const auto [start, length] = signatures[(std::size_t) index];
        return (tick - start) % juce::jmax<std::int64_t> (1, length) == 0;
    };
    // during the count-in the clicks run on a position before the start (whole bars, so the beats line up)
    const auto startTick = transport.positionTicks.load (std::memory_order_relaxed) - (counting ? countIn : 0.0);
    const auto tps = transport.ticksPerSample();
    const auto level = clickGain * controls::metronomeGain (transport.metronomeVolume.load (std::memory_order_relaxed));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto beat = (std::int64_t) std::floor ((startTick + i * tps) / ppq);

        if (beat != lastBeat)
        {
            lastBeat = beat;
            const auto down = isDownbeat (beat);
            clickSound = juce::jlimit (0, soundCount - 1, transport.metronomeSound.load (std::memory_order_relaxed));
            const auto& shape = shapes[(std::size_t) clickSound];
            const auto hz = down ? shape.downbeatHz : shape.beatHz;
            clickPhase = clickPhase2 = 0.0;
            clickPhaseDelta = juce::MathConstants<double>::twoPi * hz / currentSampleRate;
            clickPhaseDelta2 = clickPhaseDelta * 1.44;
            clickLength = juce::jmax (1, (int) (shape.seconds * currentSampleRate));
            clickSamplesLeft = clickLength;
            clickAccent = down || clickSound == 0 ? 1.0f : 0.7f; // (the click marks the downbeat by its pitch)
        }

        if (clickSamplesLeft > 0)
        {
            const auto sample = level * clickAccent * voiceSample();
            --clickSamplesLeft;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                buffer.setSample (channel, i, sample);
        }
    }
}
} // namespace lynx
