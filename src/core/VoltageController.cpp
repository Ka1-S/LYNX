#include "core/VoltageController.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/Instruments.h"
#include "core/Song.h"

namespace lynx::voltage
{
namespace
{
template <typename T>
T readAt (const std::vector<std::uint8_t>& d, std::size_t at) noexcept
{
    T v {};
    if (at + sizeof (T) <= d.size())
        std::memcpy (&v, d.data() + at, sizeof (T));
    return v;
}

template <typename T>
void put (std::vector<std::uint8_t>& d, T v)
{
    const auto* p = reinterpret_cast<const std::uint8_t*> (&v);
    d.insert (d.end(), p, p + sizeof (T));
}
} // namespace

std::optional<Settings> parse (const std::vector<std::uint8_t>& d)
{
    // [REA_rodada164a §6, DECOMP 0x702BD0]: version, 16 × i32, 8 sources, enable, range, pitch standard, 8 polarities,
    // 3 × i32 mixer tracks, i32 clock PPQ (103 bytes); v2 + i32 pulse length, f64 pitch factor (115); v3 + 8 voices
    const auto version = readAt<std::int32_t> (d, 0);
    if (version < 1 || version > 3 || d.size() < 103 || (version >= 2 && d.size() < 115))
        return std::nullopt;
    Settings s;
    std::size_t at = 4;
    for (auto& p : s.params)
        p = std::clamp (readAt<std::int32_t> (d, at), 0, 65536), at += 4;
    for (auto& src : s.source)
        src = (Source) std::min<int> (d[at++], sourceCount - 1);
    s.enabled = d[at++] != 0;
    s.range = std::min<int> (d[at++], 2);
    s.pitchStandard = std::min<int> (d[at++], 1);
    for (auto& u : s.unipolar)
        u = d[at++] != 0;
    for (auto& t : s.tracks)
        t = std::clamp (readAt<std::int32_t> (d, at), 0, 125), at += 4;
    s.clockPpq = std::clamp (readAt<std::int32_t> (d, at), 1, 960);
    at += 4;
    if (version >= 2)
    {
        s.pulseLength = std::clamp (readAt<std::int32_t> (d, at), 1, 500);
        const auto factor = readAt<double> (d, at + 4);
        s.pitchFactor = std::isfinite (factor) && factor > 0.0 && factor < 10.0 ? factor : 1.0;
        at += 12;
    }
    if (version >= 3 && at + 8 <= d.size())
        for (auto& v : s.voice)
            v = d[at++];
    return s;
}

std::vector<std::uint8_t> write (const Settings& s)
{
    std::vector<std::uint8_t> d;
    put<std::int32_t> (d, 3);
    for (const auto p : s.params)
        put<std::int32_t> (d, p);
    for (const auto src : s.source)
        d.push_back ((std::uint8_t) src);
    d.push_back (s.enabled ? 1 : 0);
    d.push_back ((std::uint8_t) s.range);
    d.push_back ((std::uint8_t) s.pitchStandard);
    for (const auto u : s.unipolar)
        d.push_back (u ? 1 : 0);
    for (const auto t : s.tracks)
        put<std::int32_t> (d, t);
    put<std::int32_t> (d, s.clockPpq);
    put<std::int32_t> (d, s.pulseLength);
    put<double> (d, s.pitchFactor);
    for (const auto v : s.voice)
        d.push_back (v);
    return d;
}

float offsetOf (const Settings& s, int output) noexcept
{
    const auto p = (float) s.params[(std::size_t) std::clamp (output, 0, outputs - 1)] / 65536.0f;
    return s.unipolar[(std::size_t) std::clamp (output, 0, outputs - 1)] ? p : p * 2.0f - 1.0f;
}

float scaleOf (const Settings& s, int output) noexcept
{
    return (float) s.params[(std::size_t) (8 + std::clamp (output, 0, outputs - 1))] / 65536.0f * 2.0f - 1.0f;
}

float applyRange (int range, float v) noexcept
{
    switch (range)
    {
        case 1: return std::clamp (0.5f * v, -1.0f, 1.0f);       // ±5 V
        case 2: return std::clamp (0.5f * v + 0.5f, 0.0f, 1.0f); // 0…10 V
        default: return std::clamp (v, -1.0f, 1.0f);             // ±10 V
    }
}

float pitchValue (const Settings& s, double key) noexcept
{
    // full scale 1.0 = 10 V, 0 at C5: 1 V / octave = 0.1 per octave. "1.2 V / octave" is what FL renders
    // (REA_rodada164f-flimport [FL render]: C5 0, C#5 +0.1, D5 +0.2, B4 −0.1, D#5 +0.3; an octave away clips at ±1):
    // 0.1 full scale (1 V) per semitone — 10 × the 0.01 the name says, as FL's code computes it (REA_rodada164a §6)
    const auto semitones = key - 60.0;
    return (float) (s.pitchFactor * (s.pitchStandard == 1 ? semitones * 0.1 : semitones / 120.0));
}

std::string paramName (int param)
{
    if (param < 0 || param >= paramCount)
        return {};
    return std::string (outputName (param % outputs)) + (param < outputs ? " offset" : " scale");
}

float paramValue (const Settings& s, int param) noexcept
{
    return param >= 0 && param < paramCount ? (float) s.params[(std::size_t) param] / 65536.0f : 0.0f;
}

void setParamValue (Settings& s, int param, float value) noexcept
{
    if (param >= 0 && param < paramCount && std::isfinite (value))
        s.params[(std::size_t) param] = (std::int32_t) std::lround (std::clamp (value, 0.0f, 1.0f) * 65536.0f);
}

const char* sourceName (Source source) noexcept
{
    static const char* names[] = { "Pitch", "Gate", "Trigger", "Velocity", "Volume", "Pan", "Mod X", "Mod Y", "Clock", "None" };
    return names[(int) source < sourceCount ? (int) source : sourceCount - 1];
}

const char* outputName (int output) noexcept
{
    static const char* names[] = { "Main L", "Main R", "Out 2 L", "Out 2 R", "Out 3 L", "Out 3 R", "Out 4 L", "Out 4 R" };
    return names[std::clamp (output, 0, outputs - 1)];
}

namespace
{
class VoltageInstrument final : public Instrument
{
public:
    VoltageInstrument()
    {
        for (auto& v : overrides)
            v.store (-1.0f, std::memory_order_relaxed);
    }

    void prepare (double sr) noexcept override
    {
        rate = sr > 0.0 ? sr : 44100.0;
        for (auto& pair : aux)
            for (auto& side : pair)
                side.assign (auxCapacity, 0.0f);
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        Settings s;
        if (auto parsed = parse (channel.pluginState))
            s = *parsed;
        // a seqlock: the audio thread copies the settings whole or keeps its last copy (no lock, no allocation)
        const auto seq = sequence.load (std::memory_order_relaxed);
        sequence.store (seq + 1, std::memory_order_release);
        std::atomic_thread_fence (std::memory_order_release);
        shared = s;
        std::atomic_thread_fence (std::memory_order_release);
        sequence.store (seq + 2, std::memory_order_release);
        tempo.store (bpm > 0.0 ? bpm : 120.0, std::memory_order_relaxed);
        channelVolume.store (channel.volume, std::memory_order_relaxed);
    }

    void noteOn (int key, float velocity) noexcept override
    {
        held[(std::size_t) std::clamp (key, 0, 127)] = true;
        if (std::find (order.begin(), order.begin() + orderCount, key) == order.begin() + orderCount && orderCount < (int) order.size())
            order[(std::size_t) orderCount++] = key;
        lastKey = key;
        lastCents = nextCents;
        lastVelocity = std::clamp (velocity, 0.0f, 1.0f);
        lastPan = nextPan;
        lastModX = nextModX;
        lastModY = nextModY;
        triggerLeft = settings.pulseLength;
    }

    void noteOff (int key) noexcept override
    {
        held[(std::size_t) std::clamp (key, 0, 127)] = false;
        const auto end = order.begin() + orderCount;
        if (auto it = std::find (order.begin(), end, key); it != end)
        {
            std::copy (it + 1, end, it);
            --orderCount;
        }
        if (orderCount > 0) // (the pitch goes back to the note still held, as a monophonic keyboard)
            lastKey = order[(std::size_t) orderCount - 1];
    }

    void allNotesOff() noexcept override
    {
        held = {};
        orderCount = 0;
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }
    void setNextNoteModulation (float modX, float modY) noexcept override
    {
        nextModX = modX;
        nextModY = modY;
    }
    void pitchBend (float semitones) noexcept override { bend = semitones; }
    // automation / a link on an offset or scale (round 165): overrides the setting until the next one
    void setParameter (int index, float value) noexcept override
    {
        if (index >= 0 && index < paramCount && std::isfinite (value))
            overrides[(std::size_t) index].store (std::clamp (value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    void setSongPosition (double ticks) noexcept override { songTicks = ticks; }
    bool idle() const noexcept override { return ! settings.enabled; }

    void beginChunk (int n) noexcept override
    {
        refresh();
        cursor = 0;
        for (auto& pair : aux)
            for (auto& side : pair)
                if (! side.empty())
                    std::fill (side.begin(), side.begin() + std::min<std::ptrdiff_t> (n, (std::ptrdiff_t) side.size()), 0.0f);
        // the song plays when its position moved forward since the last chunk (stopped: no clock)
        playing = songTicks > lastChunkTicks && songTicks - lastChunkTicks < (double) ppq * 16.0;
        lastChunkTicks = songTicks;
        chunkStartTicks = songTicks;
    }

    int auxOutputCount() const noexcept override { return settings.enabled ? 3 : 0; }
    int auxOutputTrack (int k) const noexcept override
    {
        return k >= 0 && k < 3 && settings.tracks[(std::size_t) k] > 0 ? settings.tracks[(std::size_t) k] : -1;
    }
    const float* auxOutputData (int k, int side) const noexcept override
    {
        return k >= 0 && k < 3 && ! aux[(std::size_t) k][(std::size_t) (side & 1)].empty() ? aux[(std::size_t) k][(std::size_t) (side & 1)].data() : nullptr;
    }

    void render (float* left, float* right, int n, float gainLeft, float gainRight) noexcept override
    {
        if (! settings.enabled || n <= 0)
        {
            cursor += std::max (0, n);
            return;
        }
        const auto& s = settings;
        const auto audible = gainLeft != 0.0f || gainRight != 0.0f;
        // the clock: pulses at every 960 / PPQ ticks of the song while it plays
        const auto ticksPerSample = (double) ppq * tempo.load (std::memory_order_relaxed) / (60.0 * rate);
        const auto clockStep = (double) ppq / (double) std::max (1, s.clockPpq);

        std::array<float, outputs> steady {};
        std::array<bool, outputs> pulsed {};
        const auto key = (double) lastKey + (double) lastCents / 100.0 + (double) bend;
        for (int o = 0; o < outputs; ++o)
        {
            float v = 0.0f;
            switch (s.source[(std::size_t) o])
            {
                case Source::pitch: v = pitchValue (s, key); break;
                case Source::gate: v = orderCount > 0 ? 1.0f : 0.0f; break;
                case Source::velocity: v = lastVelocity; break;
                case Source::volume: v = std::clamp (channelVolume.load (std::memory_order_relaxed), 0.0f, 1.0f); break;
                case Source::pan: v = std::clamp (lastPan, -1.0f, 1.0f); break;
                case Source::modX: v = std::clamp (lastModX * 2.0f - 1.0f, -1.0f, 1.0f); break;
                case Source::modY: v = std::clamp (lastModY * 2.0f - 1.0f, -1.0f, 1.0f); break;
                case Source::trigger:
                case Source::clock: pulsed[(std::size_t) o] = true; break;
                case Source::none: break;
            }
            steady[(std::size_t) o] = applyRange (s.range, scaleOf (s, o) * (v + offsetOf (s, o)));
        }

        for (int i = 0; i < n; ++i)
        {
            // the clock's pulse: a new one when the song crosses a clock step
            if (playing)
            {
                const auto t0 = chunkStartTicks + (double) (cursor + i) * ticksPerSample;
                if (std::floor ((t0 + ticksPerSample) / clockStep) > std::floor (t0 / clockStep)) // (a step in (t0, t0 + 1 sample])
                    clockLeft = s.pulseLength;
            }
            float out[outputs];
            for (int o = 0; o < outputs; ++o)
            {
                out[o] = steady[(std::size_t) o];
                if (pulsed[(std::size_t) o])
                {
                    const auto on = s.source[(std::size_t) o] == Source::trigger ? triggerLeft > 0 : clockLeft > 0;
                    // a pulse is scale · 1 + offset, past the range (FL)
                    out[o] = on ? scaleOf (s, o) + offsetOf (s, o) : 0.0f;
                }
            }
            // the channel's volume and pan do not scale the voltages (FL's render at 78 %: exact values, REA_rodada164a
            // §5b); a muted channel (both gains 0) stays silent
            left[i] += audible ? out[0] : 0.0f;
            right[i] += audible ? out[1] : 0.0f;
            const auto at = cursor + i;
            if (at < auxCapacity && audible)
                for (int k = 0; k < 3; ++k)
                {
                    aux[(std::size_t) k][0][(std::size_t) at] += out[2 + 2 * k];
                    aux[(std::size_t) k][1][(std::size_t) at] += out[3 + 2 * k];
                }
            if (triggerLeft > 0)
                --triggerLeft;
            if (clockLeft > 0)
                --clockLeft;
        }
        cursor += n;
    }

private:
    static constexpr int auxCapacity = 4096;

    void refresh() noexcept
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = sequence.load (std::memory_order_acquire);
            if ((before & 1u) != 0)
                continue;
            std::atomic_thread_fence (std::memory_order_acquire);
            Settings copy = shared;
            std::atomic_thread_fence (std::memory_order_acquire);
            if (sequence.load (std::memory_order_acquire) == before)
            {
                settings = copy;
                break;
            }
        }
        for (int p = 0; p < paramCount; ++p)
            if (const auto v = overrides[(std::size_t) p].load (std::memory_order_relaxed); v >= 0.0f)
                setParamValue (settings, p, v);
    }

    double rate = 44100.0;
    std::atomic<std::uint32_t> sequence { 0 };
    Settings shared;   // written by configure (UI thread)
    Settings settings; // the audio thread's copy
    std::array<std::atomic<float>, paramCount> overrides; // (−1: none)
    std::atomic<double> tempo { 120.0 };
    std::atomic<float> channelVolume { 0.78f };

    std::array<bool, 128> held {};
    std::array<int, 16> order {};
    int orderCount = 0;
    int lastKey = 60;
    float lastCents = 0.0f, lastVelocity = 0.0f, lastPan = 0.0f, lastModX = 0.5f, lastModY = 0.5f;
    float nextCents = 0.0f, nextPan = 0.0f, nextModX = 0.5f, nextModY = 0.5f, bend = 0.0f;
    int triggerLeft = 0, clockLeft = 0;

    double songTicks = 0.0, lastChunkTicks = 0.0, chunkStartTicks = 0.0;
    bool playing = false;
    int cursor = 0;
    std::array<std::array<std::vector<float>, 2>, 3> aux;
};
} // namespace

std::unique_ptr<Instrument> makeInstrument()
{
    return std::make_unique<VoltageInstrument>();
}
} // namespace lynx::voltage
