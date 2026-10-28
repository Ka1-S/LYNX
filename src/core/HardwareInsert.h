#pragma once

// Hardware insert (round 164d, LYNX extra; the role of Pro Tools' H/W Insert and Nuendo's External FX): a mixer effect
// that sends the insert's signal at its place in the chain out to chosen device outputs (an outboard compressor, a
// tape machine, a guitar amp…) and reads it back from chosen device inputs, inside the mixer.
//
// Timing: what the effect writes into the device-output buses in a callback piece leaves on that piece; what it reads
// from the device inputs in that piece is what came back `roundTrip` samples after it was sent (measured by the
// effect's own ping: core/TakeRecording.h LatencyPing on exactly this output → input path). The effect reports the round
// trip as its latency (PDC delays every other path by as much) and delays its own dry signal by it, so dry and return
// line up sample-accurately.
//
// Offline (an export, or no device): hardware can't run faster than real time, so the return is replaced by the dry
// signal, still delayed by the round trip (the export keeps every other track aligned). The Export dialog warns.
//
// Realtime: nothing allocates after construction; the buses and inputs are handed in per block by the renderer
// (setHardwareIo), which pins inserts with a hardware insert to the audio thread (the output buses are not shared
// between threads).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Effects.h"
#include "core/HardwareOutput.h"
#include "core/LiveInput.h"
#include "core/TakeRecording.h"

namespace lynx::hwinsert
{
enum Param : int
{
    send = 0,     // send gain, ±24 dB (0.5 = 0 dB)
    returnGain,   // return gain, ±24 dB
    dryWet,       // 0 = dry only, 1 = return only (default: 1, a plain insert)
    invert,       // return phase invert (0 / 1)
    output,       // device output channel: (channel + 1) / 128, 0 = none
    outputMono,   // 0 = stereo pair, 1 = mono (L+R)/2 on one channel
    input,        // device input channel: (channel + 1) / 128, 0 = none
    inputStereo,  // 1 = stereo pair, 0 = one channel (to both sides)
    roundTrip,    // samples / 65536 (set by the ping, editable)
    ping,         // momentary: any change of the value starts a ping
    count
};

constexpr int maxChannel = 126;
constexpr int maxRoundTrip = 65535;

// meter outputs (controlOutput)
enum Meter : int
{
    sendPeak = 2,   // the "iometer" display's pair (send, return)
    returnPeak = 3,
    pingState = 4,  // 0 idle, 0.25 running, 0.5 done ok; failed: 0.625 no signal, 0.75 clicks missing, 0.875 unstable
    pingSamples = 5, // last ping result / 65536
    pingSerial = 6,  // finished pings / 1024 (the editor applies a new result once)
    status = 7       // flags / 16: 1 offline (no device plays live), 2 output channel not open, 4 input channel not open
};

inline float dbOf (float v) noexcept { return -24.0f + 48.0f * std::clamp (v, 0.0f, 1.0f); }
inline float gainOf (float v) noexcept { return std::pow (10.0f, dbOf (v) / 20.0f); }
inline float dbValue (float db) noexcept { return std::clamp ((db + 24.0f) / 48.0f, 0.0f, 1.0f); }

inline int channelOf (float v) noexcept { return std::clamp ((int) std::lround (v * 128.0f), 0, maxChannel + 1) - 1; }
inline float channelValue (int channel) noexcept { return channel < 0 ? 0.0f : (float) (std::min (channel, maxChannel) + 1) / 128.0f; }

inline int roundTripOf (float v) noexcept { return std::clamp ((int) std::lround (v * 65536.0f), 0, maxRoundTrip); }
inline float roundTripValue (int samples) noexcept { return (float) std::clamp (samples, 0, maxRoundTrip) / 65536.0f; }

// Routed both ways (an output and an input chosen)
inline bool isRouted (const EffectParams& p) noexcept { return channelOf (p[output]) >= 0 && channelOf (p[input]) >= 0; }

// The device setup (AudioEngine::currentSetupKey: driver, devices, rate, buffer size) its round trip was measured for,
// kept in the slot's state (round 165c; empty: not known, e.g. a 0.1.19 project). Another setup has another round trip.
inline std::string measuredSetup (const EffectSlot& slot)
{
    static constexpr char prefix[] = "setup=";
    const std::string text (slot.state.begin(), slot.state.end());
    return text.rfind (prefix, 0) == 0 ? text.substr (sizeof (prefix) - 1) : std::string();
}
inline void setMeasuredSetup (EffectSlot& slot, const std::string& key)
{
    const auto text = "setup=" + key;
    slot.state.assign (text.begin(), text.end());
}

inline OutputMode outputModeOf (const EffectParams& p) noexcept { return p[outputMono] >= 0.5f ? OutputMode::monoSum : OutputMode::stereo; }

inline std::string paramText (int param, float v)
{
    char text[64];
    switch (param)
    {
        case send:
        case returnGain:
            std::snprintf (text, sizeof (text), "%+.1f dB", (double) dbOf (v));
            return text;
        case dryWet:
            std::snprintf (text, sizeof (text), "%d%%", (int) std::lround (v * 100.0f));
            return text;
        case invert: return v >= 0.5f ? "Inverted" : "Normal";
        case output:
        case input:
            if (const auto c = channelOf (v); c >= 0)
                return (param == output ? "Out " : "In ") + std::to_string (c + 1);
            return "None";
        case outputMono: return v >= 0.5f ? "Mono" : "Stereo";
        case inputStereo: return v >= 0.5f ? "Stereo" : "Mono";
        case roundTrip: return std::to_string (roundTripOf (v)) + " smp";
        default: return {};
    }
}

// Every hardware insert slot of the song (round 165c): the mixer's, and those inside Patchers (mixer-slot and Channel Rack
// generator Patchers, nested ones too). `active` = on (the slot, and the Patcher slot that holds it).
template <typename F>
void forEachHardwareInsert (const PatcherDoc& doc, bool active, F&& f)
{
    for (const auto& n : doc.nodes)
    {
        if (n.generator)
        {
            if (n.channel.instrument == InstrumentKind::patcher)
                forEachHardwareInsert (n.channel.patcher, active, f);
            continue;
        }
        if (n.effect.kind == EffectKind::hardwareInsert)
            f (n.effect, active && n.effect.enabled);
        else if (n.effect.kind == EffectKind::patcher)
            forEachHardwareInsert (n.effect.patcher, active && n.effect.enabled, f);
    }
}

template <typename F>
void forEachHardwareInsert (const Song& song, F&& f)
{
    for (const auto& t : song.mixer)
        for (const auto& fx : t.effects)
        {
            if (fx.kind == EffectKind::hardwareInsert)
                f (fx, fx.enabled);
            else if (fx.kind == EffectKind::patcher)
                forEachHardwareInsert (fx.patcher, fx.enabled, f);
        }
    for (const auto& c : song.channels)
        if (c.instrument == InstrumentKind::patcher)
            forEachHardwareInsert (c.patcher, true, f);
}

// The song's hardware inserts that are on, and whether one of them returns from a device input.
inline int countActive (const Song& song)
{
    auto n = 0;
    forEachHardwareInsert (song, [&n] (const EffectSlot&, bool active) { n += active ? 1 : 0; });
    return n;
}

inline bool returnsFromInput (const Song& song)
{
    auto any = false;
    forEachHardwareInsert (song, [&any] (const EffectSlot& fx, bool) { any = any || channelOf (fx.params[input]) >= 0; });
    return any;
}

class HardwareInsertFx final : public Effect
{
public:
    explicit HardwareInsertFx (const EffectParams* initial = nullptr)
        : dryL ((std::size_t) maxRoundTrip + 1, 0.0f), dryR ((std::size_t) maxRoundTrip + 1, 0.0f)
    {
        if (initial != nullptr)
        {
            latency.store (roundTripOf ((*initial)[roundTrip]));
            lastPing = (*initial)[ping];
        }
    }

    void prepare (double rate) noexcept override { sampleRate = rate > 0.0 ? rate : 48000.0; reset(); }

    void reset() noexcept override
    {
        std::fill (dryL.begin(), dryL.end(), 0.0f);
        std::fill (dryR.begin(), dryR.end(), 0.0f);
        write = 0;
    }

    // renderer, every block before process(): the device's input of this piece, the output buses (null when no device
    // plays live: an export) and where this chunk starts within the callback piece
    void setHardwareIo (const LiveInput* in, HardwareOutput* out, int offset) noexcept override
    {
        liveIn = in;
        hwOut = out;
        ioOffset = offset;
    }

    int latencySamples() const noexcept override { return latency.load (std::memory_order_relaxed); } // (read by the UI thread)

    float controlOutput (int o) const noexcept override
    {
        return o >= 0 && o < (int) meters.size() ? meters[(std::size_t) o] : 0.0f;
    }

    void process (float* left, float* right, int n, const EffectParams& p, double) noexcept override
    {
        const auto delay = roundTripOf (p[roundTrip]);
        latency.store (delay, std::memory_order_relaxed);

        // the ping button (a momentary value: any change)
        if (p[ping] != lastPing)
        {
            if (lastPing >= 0.0f)
                startPing = true;
            lastPing = p[ping];
        }

        const auto live = hwOut != nullptr && hwOut->active.load (std::memory_order_relaxed);
        const auto outChannel = channelOf (p[output]);
        const auto mode = outputModeOf (p);
        const auto inChannel = channelOf (p[input]);
        const auto inStereo = p[inputStereo] >= 0.5f;

        float* busA = nullptr;
        float* busB = nullptr;
        const float* retL = nullptr;
        const float* retR = nullptr;
        if (live)
        {
            const auto [first, count] = outputChannelsOf (outChannel, mode);
            if (count > 0)
            {
                busA = hwOut->bus (first, ioOffset, n);
                busB = count > 1 ? hwOut->bus (first + 1, ioOffset, n) : nullptr;
            }
            if (liveIn != nullptr && inChannel >= 0)
            {
                retL = liveIn->channel (inChannel, ioOffset);
                retR = inStereo ? liveIn->channel (inChannel + 1, ioOffset) : retL;
            }
        }
        const auto ioOk = busA != nullptr && retL != nullptr && retR != nullptr;

        auto flags = 0;
        if (! live)
            flags |= 1;
        else
        {
            if (busA == nullptr && outChannel >= 0)
                flags |= 2;
            if ((retL == nullptr || retR == nullptr) && inChannel >= 0)
                flags |= 4;
        }

        // the ping: only its clicks go out on the send (the signal is held back meanwhile), listened for on the return
        if (startPing && live && busA != nullptr && retL != nullptr)
        {
            pinger.start (8, std::max (4096, (int) (sampleRate / 4.0)), 0.5f);
            pinging = true;
            startPing = false;
            meters[pingState] = 0.25f;
        }
        else if (startPing && ! live)
            startPing = false; // (pressed during an export / without a device: nothing to measure)

        const auto sendGain = gainOf (p[send]);
        auto retGain = gainOf (p[returnGain]);
        if (p[invert] >= 0.5f)
            retGain = -retGain;
        const auto wet = std::clamp (p[dryWet], 0.0f, 1.0f);

        // the send (before this block's output replaces the buffers)
        float sendPeakNow = 0.0f;
        if (pinging)
        {
            std::array<float*, 2> outs { busA, busB };
            pinger.process (retL, outs.data(), 2, n);
            if (! pinger.isRunning())
            {
                pinging = false;
                const auto r = pinger.result();
                meters[pingState] = r.ok ? 0.5f : r.problem == "no signal" ? 0.625f : r.problem == "clicks missing" ? 0.75f : 0.875f;
                meters[pingSamples] = roundTripValue (r.samples);
                ++serial;
                meters[pingSerial] = (float) (serial % 1024) / 1024.0f;
            }
        }
        else if (busA != nullptr)
        {
            for (int i = 0; i < n; ++i)
                sendPeakNow = std::max ({ sendPeakNow, std::abs (left[i]), std::abs (right[i]) });
            if (sendGain != 1.0f)
            {
                // scaled into the bus without touching the chain's buffers
                for (int i = 0; i < n; ++i)
                {
                    const auto l = left[i] * sendGain, r = right[i] * sendGain;
                    switch (mode)
                    {
                        case OutputMode::stereo: busA[i] += l; if (busB != nullptr) busB[i] += r; break;
                        default: busA[i] += 0.5f * (l + r); break;
                    }
                }
            }
            else
                addToOutput (left, right, mode, busA, busB, n);
            sendPeakNow *= sendGain;
        }

        // dry delayed by the round trip; out = dry · (1 − wet) + return · wet
        const auto size = (int) dryL.size();
        float returnPeakNow = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            dryL[(std::size_t) write] = left[i];
            dryR[(std::size_t) write] = right[i];
            auto read = write - delay;
            if (read < 0)
                read += size;
            const auto dl = dryL[(std::size_t) read], dr = dryR[(std::size_t) read];
            if (++write == size)
                write = 0;

            if (ioOk && ! pinging)
            {
                const auto rl = retL[i] * retGain, rr = retR[i] * retGain;
                returnPeakNow = std::max ({ returnPeakNow, std::abs (rl), std::abs (rr) });
                left[i] = dl + (rl - dl) * wet;
                right[i] = dr + (rr - dr) * wet;
            }
            else if (live && ! pinging && outChannel >= 0 && inChannel >= 0)
            {
                // routed but a channel is not open: the return is silent (heard as a drop, shown in the editor)
                left[i] = dl * (1.0f - wet);
                right[i] = dr * (1.0f - wet);
            }
            else
            {
                // offline (export), no routing chosen, or pinging: the dry signal at the insert's latency
                left[i] = dl;
                right[i] = dr;
            }
        }

        // peak meters with a 300 ms fall: the editor reads them 30 times a second, a short peak between two reads still shows
        const auto fall = (float) std::exp (-(double) n / (0.3 * sampleRate));
        meters[sendPeak] = std::max (sendPeakNow, meters[sendPeak] * fall);
        meters[returnPeak] = std::max (returnPeakNow, meters[returnPeak] * fall);
        meters[status] = (float) flags / 16.0f;
    }

private:
    const LiveInput* liveIn = nullptr;
    HardwareOutput* hwOut = nullptr;
    int ioOffset = 0;
    double sampleRate = 48000.0;
    std::atomic<int> latency { 0 };
    std::vector<float> dryL, dryR;
    int write = 0;
    float lastPing = -1.0f;
    bool startPing = false, pinging = false;
    int serial = 0;
    takes::LatencyPing pinger;
    std::array<float, 8> meters {};
};
} // namespace lynx::hwinsert
