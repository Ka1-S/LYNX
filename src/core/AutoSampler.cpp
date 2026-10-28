#include "core/AutoSampler.h"

#include <algorithm>
#include <cmath>

namespace lynx::autosample
{
namespace
{
std::int64_t secondsToTicks (double seconds, double bpm) noexcept
{
    return (std::int64_t) std::ceil (seconds * bpm / 60.0 * ppq);
}

double ticksToSeconds (std::int64_t ticks, double bpm) noexcept
{
    return (double) ticks / ppq * 60.0 / std::max (1.0, bpm);
}
} // namespace

Options clamped (Options o)
{
    o.lowKey = std::clamp (o.lowKey, 0, 127);
    o.highKey = std::clamp (o.highKey, o.lowKey, 127);
    o.keysPerZone = std::clamp (o.keysPerZone, 1, 128);
    o.rootOffset = std::clamp (o.rootOffset, 0, o.keysPerZone - 1);
    o.velocityLayers = std::clamp (o.velocityLayers, 1, 16);
    o.maxVelocity = std::clamp (o.maxVelocity, 1, 127);
    o.holdSeconds = std::clamp (o.holdSeconds, 0.05, 60.0);
    o.maxSeconds = std::clamp (o.maxSeconds, 0.05, 60.0);
    o.channels = std::clamp (o.channels, 0, 3);
    return o;
}

Plan plan (const Options& options, double bpm)
{
    const auto o = clamped (options);
    Plan p;
    p.bpm = std::max (10.0, bpm);
    p.holdTicks = std::max<std::int64_t> (1, secondsToTicks (o.holdSeconds, p.bpm));
    // every take gets hold + max length, rounded up to a step (FL lets the release ring up to "Max length")
    const auto slot = secondsToTicks (o.holdSeconds + o.maxSeconds, p.bpm);
    p.slotTicks = std::max<std::int64_t> (ticksPerStep, (slot + ticksPerStep - 1) / ticksPerStep * ticksPerStep);

    std::int64_t at = 0;
    for (int low = o.lowKey; low <= o.highKey; low += o.keysPerZone)
    {
        const auto high = std::min (low + o.keysPerZone - 1, o.highKey);
        const auto root = std::min (low + o.rootOffset, high);
        int previous = 0;
        for (int layer = 0; layer < o.velocityLayers; ++layer)
        {
            Take t;
            t.key = root;
            t.lowKey = low;
            t.highKey = high;
            t.velocity = std::max (1, (int) std::lround ((double) o.maxVelocity * (layer + 1) / o.velocityLayers));
            t.lowVelocity = layer == 0 ? 0 : previous + 1;
            t.highVelocity = layer == o.velocityLayers - 1 ? 127 : t.velocity;
            previous = t.highVelocity;
            t.startTick = at;
            at += p.slotTicks;
            p.takes.push_back (t);
        }
    }
    return p;
}

double renderSeconds (const Plan& plan) noexcept
{
    return ticksToSeconds (plan.slotTicks * (std::int64_t) plan.takes.size(), plan.bpm);
}

Song makeRenderSong (const Song& song, int channel, const Plan& plan, const Options& options)
{
    Song out = song;
    if (channel < 0 || channel >= (int) out.channels.size())
        return out;
    out.bpm = plan.bpm;
    for (int c = 0; c < (int) out.channels.size(); ++c)
        if (c != channel)
            out.channels[(std::size_t) c].muted = true;
    auto& source = out.channels[(std::size_t) channel];
    source.muted = false;
    source.volume = 1.0f; // channelGain (1) = 0 dB: the takes keep the instrument's own level
    source.pan = 0.0f;
    if (! options.insertEffects)
        source.mixerTrack = 0;
    // the automation and the controllers would move the instrument while it is sampled
    out.automations.clear();
    out.midiLinks.clear();
    out.initialValues.clear();
    for (auto& p : out.patterns)
        p.events.clear();

    Pattern p;
    p.name = "Auto-sampler";
    for (const auto& t : plan.takes)
    {
        Note n;
        n.start = t.startTick;
        n.length = plan.holdTicks;
        n.channel = channel;
        n.key = t.key;
        n.velocity = (float) t.velocity / 127.0f;
        p.notes.push_back (n);
    }
    p.lengthTicks = std::max<std::int64_t> (ticksPerStep, plan.slotTicks * (std::int64_t) plan.takes.size());
    out.patterns.push_back (std::move (p));
    out.currentPattern = (int) out.patterns.size() - 1;
    out.mode = PlayMode::pattern;
    out.swing = 0.0f;
    return out;
}

std::vector<Cut> cutTakes (const float* left, const float* right, int frames, double sampleRate, const Plan& plan, const Options& options)
{
    const auto o = clamped (options);
    std::vector<Cut> cuts;
    if (left == nullptr || frames <= 0 || sampleRate <= 0.0)
        return cuts;
    if (right == nullptr)
        right = left;
    const auto toFrame = [&] (std::int64_t tick) { return (int) std::clamp<double> (std::llround (ticksToSeconds (tick, plan.bpm) * sampleRate), 0.0, frames); };
    constexpr float silence = 3.1623e-5f; // −90 dBFS
    const auto tail = (int) std::lround (0.05 * sampleRate);
    const auto fadeIn = std::max (1, (int) std::lround (0.002 * sampleRate));
    const auto fadeOut = std::max (1, (int) std::lround (0.010 * sampleRate));

    float peak = 0.0f;
    for (const auto& t : plan.takes)
    {
        const auto start = toFrame (t.startTick);
        const auto slotEnd = toFrame (t.startTick + plan.slotTicks);
        auto end = slotEnd;
        if (o.stopOnSilence)
        {
            auto last = start;
            for (int i = start; i < slotEnd; ++i)
                if (std::abs (left[i]) > silence || std::abs (right[i]) > silence)
                    last = i + 1;
            end = std::min (slotEnd, last + tail);
        }
        else
            end = std::min (slotEnd, toFrame (t.startTick + secondsToTicks (o.holdSeconds + o.maxSeconds, plan.bpm)));
        const auto n = std::max (1, end - start);

        Cut c;
        c.left.resize ((std::size_t) n, 0.0f);
        if (o.channels == 0)
            c.right.resize ((std::size_t) n, 0.0f);
        for (int i = 0; i < n && start + i < frames; ++i)
        {
            const auto l = left[start + i], r = right[start + i];
            switch (o.channels)
            {
                case 0: c.left[(std::size_t) i] = l; c.right[(std::size_t) i] = r; break;
                case 1: c.left[(std::size_t) i] = 0.5f * (l + r); break;
                case 2: c.left[(std::size_t) i] = l; break;
                default: c.left[(std::size_t) i] = r; break;
            }
        }
        auto ramp = [&] (std::vector<float>& v) {
            if (o.declickIn)
                for (int i = 0; i < std::min (fadeIn, n); ++i)
                    v[(std::size_t) i] *= (float) i / (float) fadeIn;
            if (o.declickOut)
                for (int i = 0; i < std::min (fadeOut, n); ++i)
                    v[(std::size_t) (n - 1 - i)] *= (float) i / (float) fadeOut;
            for (auto x : v)
                peak = std::max (peak, std::abs (x));
        };
        ramp (c.left);
        if (! c.right.empty())
            ramp (c.right);
        cuts.push_back (std::move (c));
    }

    if (o.normalize && peak > silence)
    {
        const auto gain = 1.0f / peak; // one gain for every take: the velocity layers keep their levels
        for (auto& c : cuts)
        {
            for (auto& x : c.left)
                x *= gain;
            for (auto& x : c.right)
                x *= gain;
        }
    }
    return cuts;
}

std::vector<SampleZone> zones (const Plan& plan, const std::vector<std::string>& paths)
{
    std::vector<SampleZone> out;
    for (std::size_t i = 0; i < plan.takes.size() && i < paths.size(); ++i)
    {
        const auto& t = plan.takes[i];
        SampleZone z;
        z.path = paths[i];
        z.rootKey = t.key;
        z.lowKey = t.lowKey;
        z.highKey = t.highKey;
        z.lowVelocity = t.lowVelocity;
        z.highVelocity = t.highVelocity;
        out.push_back (z);
    }
    return out;
}

ChannelDef makeSamplerChannel (const ChannelDef& source, std::vector<SampleZone> zoneList, const Options& options)
{
    ChannelDef c;
    c.name = source.name + " (sampled)";
    c.instrument = InstrumentKind::sampler;
    c.volume = source.volume;
    c.pan = source.pan;
    c.colour = source.colour;
    c.group = source.group;
    c.mixerTrack = options.insertEffects ? 0 : source.mixerTrack;
    c.samplePath = zoneList.empty() ? std::string() : zoneList.front().path;
    c.zones = std::move (zoneList);
    c.sampler.release = 0.25f; // the take holds the note and its release; a shorter note fades out
    return c;
}

std::string takeFileName (const std::string& base, const Take& take)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    std::string clean;
    for (auto ch : base)
        clean += std::string ("<>:\"/\\|?*").find (ch) != std::string::npos || (unsigned char) ch < 32 ? '_' : ch;
    while (! clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
        clean.pop_back();
    if (clean.empty())
        clean = "Sample";
    const auto key = std::clamp (take.key, 0, 127);
    return clean + " " + names[key % 12] + std::to_string (key / 12) + " v" + std::to_string (take.velocity) + ".wav";
}
} // namespace lynx::autosample
