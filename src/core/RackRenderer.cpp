#include "core/RackRenderer.h"

#include "core/ControlSurface.h"
#include "core/VoltageController.h"
#include "core/FlSynths.h"
#include "core/HardwareInsert.h"
#include "core/Layer.h"
#include "core/Vfx.h"
#include "core/FacetEq.h"
#include "core/MidiControls.h"
#include "core/Patcher.h"
#include "core/LoadTrace.h"
#include "core/DynamicsSuite.h"
#include "core/MixSuite.h"
#include "core/VoiceTools.h"

#include "core/WorkerPool.h"
#include "core/Denormals.h"
#include "core/FlKeys.h"
#include "core/DirectWave.h"
#include "core/SoundFont.h"
#include "core/NewtoneFx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace lynx
{
namespace
{
// a hosted plugin is created again when its processing options change (fixed blocks, own process)
bool samePluginProcessing (const PluginRef& a, const PluginRef& b) noexcept
{
    return a.fixedBuffers == b.fixedBuffers && a.separateProcess == b.separateProcess && a.maxSizeBuffers == b.maxSizeBuffers
           && a.hostMaxBuffer == b.hostMaxBuffer;
}

// Silent instrument: plugin channel that could not be loaded.
class SilentInstrument final : public Instrument
{
public:
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
};

// FL Keys: the data FL builds from a wavetable's files, shared by every channel that plays the same wavetable
// (Grand Piano holds 40 MB of 16-bit samples)
std::shared_ptr<const flkeys::Wavetable> flKeysWavetable (const ChannelDef& channel, const RackState::SampleProvider& samples)
{
    const auto table = flkeys::FlKeysTable::decode (channel.modelParams);
    if (! table || ! samples || table->files.size() != channel.zones.size())
        return nullptr;
    std::string key;
    for (const auto& z : channel.zones)
        key += z.path + '\n';
    for (auto v : channel.modelParams)
        key += std::to_string (v) + ' ';
    static std::mutex lock;
    static std::map<std::string, std::weak_ptr<const flkeys::Wavetable>> cache;
    {
        std::lock_guard<std::mutex> guard (lock);
        if (auto found = cache[key].lock())
            return found;
    }
    std::vector<flkeys::FileSamples> files;
    for (std::size_t i = 0; i < channel.zones.size(); ++i)
    {
        const auto sample = samples (channel.zones[i].path);
        if (sample == nullptr || sample->frames() == 0)
            return nullptr;
        files.push_back (flkeys::toFileSamples (*sample, table->files[i].scale));
    }
    auto built = flkeys::buildWavetable (*table, files);
    std::lock_guard<std::mutex> guard (lock);
    cache[key] = built;
    return built;
}

// Effect that does nothing: missing plugin slot.
class BypassEffect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float*, float*, int, const EffectParams&, double) noexcept override {}
};
} // namespace

std::unique_ptr<const RackState> RackState::build (Song song,
                                                   const RackState* previous,
                                                   double sampleRate,
                                                   const SampleProvider& samples)
{
    Factories factories;
    factories.samples = samples;
    return build (std::move (song), previous, sampleRate, factories);
}

namespace
{
// A channel's instrument, built new (UI thread)
// deferKeyShift (round 165i): the pitch shifters of a keep-pitch sampler are made later by the caller, all channels at
// once in parallel (Thief: 29 such channels × 8 shifters took most of the 860 ms of rack_instruments, one by one)
std::shared_ptr<Instrument> buildInstrument (const ChannelDef& channel, double sampleRate, double bpm, const RackState::Factories& factories,
                                             std::vector<Instrument*>* deferKeyShift = nullptr)
{
    const auto& samples = factories.samples;
    const auto kind = channel.instrument;

    if (kind == InstrumentKind::plugin)
    {
        std::shared_ptr<Instrument> instrument;

        if (factories.pluginInstruments && ! channel.plugin.id.empty())
            instrument = factories.pluginInstruments (channel.plugin, sampleRate);

        return instrument != nullptr ? instrument : std::make_shared<SilentInstrument>();
    }

    // FL Keys: its wavetable built from the channel's files
    if (kind == InstrumentKind::flkeys)
    {
        std::shared_ptr<Instrument> instrument = flkeys::makeFlKeysInstrument (flKeysWavetable (channel, samples));
        instrument->prepare (sampleRate);
        instrument->configure (channel, bpm);
        return instrument;
    }

    // DirectWave (REA_rodada72): the zones' samples as the plugin holds them
    if (kind == InstrumentKind::directwave)
    {
        const auto formats = dw::decodeFormats (channel.modelParams);
        std::vector<std::shared_ptr<const dw::ZoneSamples>> zs;
        for (std::size_t z = 0; z < channel.zones.size(); ++z)
        {
            std::shared_ptr<const dw::ZoneSamples> s;
            if (samples && ! channel.zones[z].path.empty())
                if (const auto data = samples (channel.zones[z].path))
                    s = dw::zoneSamples (*data, z < formats.size() && formats[z]);
            zs.push_back (std::move (s));
        }
        std::shared_ptr<Instrument> instrument = dw::makeDirectWaveInstrument (std::move (zs));
        instrument->prepare (sampleRate);
        instrument->configure (channel, bpm);
        return instrument;
    }

    // LYNX Soundfont (REA_rodada158): the font is loaded on a background thread and shared by the channels playing it
    if (kind == InstrumentKind::soundfont)
    {
        std::shared_ptr<Instrument> instrument = sf::makeInstrument (channel.samplePath.empty() ? nullptr : sf::FontLibrary::instance().get (channel.samplePath));
        instrument->prepare (sampleRate);
        instrument->configure (channel, bpm);
        return instrument;
    }

    // multi-sample sampler: one zone per sample (DirectWave, FPC layers...)
    if (kind == InstrumentKind::sampler && ! channel.zones.empty())
    {
        const loadtrace::Scope timing ("rack_zoned");
        std::vector<LoadedZone> zones;

        for (const auto& z : channel.zones)
            zones.push_back ({ z, samples ? samples (z.path) : nullptr });

        std::shared_ptr<Instrument> instrument = makeZonedSampler (std::move (zones));
        instrument->prepare (sampleRate);
        instrument->configure (channel, bpm);
        return instrument;
    }

    std::shared_ptr<const SampleData> sample;

    if ((kind == InstrumentKind::sampler || kind == InstrumentKind::audioClip || kind == InstrumentKind::granulizer) && samples
        && ! channel.samplePath.empty())
        sample = samples (channel.samplePath);

    // pitch-preserving stretch: the sample comes already stretched to the current tempo
    if (sample != nullptr && kind != InstrumentKind::granulizer && channel.sampler.keepPitch && channel.sampler.stretchBeats > 0.0f
        && factories.stretchedSamples
        && sample->frames() > 0)
    {
        const auto target = channel.sampler.stretchBeats * 60.0 / std::max (1.0, bpm);
        const auto ratio = target / ((double) sample->frames() / sample->sampleRate);

        if (auto stretched = factories.stretchedSamples (channel.samplePath, ratio, channel.sampler.stretchMethod))
            sample = std::move (stretched);
    }

    const loadtrace::Scope timing ("rack_instruments");
    std::shared_ptr<Instrument> instrument = makeInstrument (kind, std::move (sample));
    instrument->prepare (sampleRate);
    instrument->configure (channel, bpm);
    if ((kind == InstrumentKind::sampler || kind == InstrumentKind::audioClip) && channel.sampler.keepPitch)
    {
        if (deferKeyShift != nullptr)
            deferKeyShift->push_back (instrument.get());
        else
            instrument->prepareKeyShift (sampleRate); // transpositions keep the length (rounds 162, 163)
    }
    return instrument;
}

// A slot's effect, built new (UI thread)
std::shared_ptr<Effect> buildEffect (const EffectSlot& slot, double sampleRate, const RackState::Factories& factories)
{
    const auto kind = slot.kind;
    std::shared_ptr<Effect> effect;

    if (kind == EffectKind::plugin)
    {
        if (factories.pluginEffects && ! slot.plugin.id.empty())
            effect = factories.pluginEffects (slot.plugin, sampleRate);

        if (effect == nullptr)
            effect = std::make_shared<BypassEffect>();
    }
    else if ((kind == EffectKind::newtone || kind == EffectKind::newtime))
    {
        // Newtone / Newtime: their source sample (loaded off the audio thread), REA_rodada73
        // (Newtone: its note edits rendered by LYNX's engine, REA_rodada95)
        effect = newtonefx::make (factories.samples && ! slot.samplePath.empty() ? factories.samples (slot.samplePath) : nullptr,
                                  slot.state);
        effect->prepare (sampleRate);
    }
    else
    {
        const loadtrace::Scope timing ("rack_builtin_fx"); // (round 112: what the open spends on LYNX's own effects)
        effect = makeEffect (slot);
        effect->prepare (sampleRate);
    }

    return effect;
}

// What a Patcher builds its nodes with: the same builders as the rack's channels and slots
patcher::Factories patcherFactories (double sampleRate, double bpm, const RackState::Factories& factories)
{
    patcher::Factories f;
    f.effect = [sampleRate, bpm, factories] (const EffectSlot& slot) -> std::shared_ptr<Effect> {
        if (slot.kind == EffectKind::patcher)
        {
            std::shared_ptr<Effect> e = patcher::makeEffect (slot, sampleRate, patcherFactories (sampleRate, bpm, factories));
            e->prepare (sampleRate);
            return e;
        }
        return buildEffect (slot, sampleRate, factories);
    };
    f.instrument = [sampleRate, bpm, factories] (const ChannelDef& channel) -> std::shared_ptr<Instrument> {
        if (channel.instrument == InstrumentKind::patcher)
        {
            std::shared_ptr<Instrument> i = patcher::makeInstrument (channel, sampleRate, patcherFactories (sampleRate, bpm, factories));
            i->configure (channel, bpm);
            return i;
        }
        return buildInstrument (channel, sampleRate, bpm, factories);
    };
    return f;
}

// The keep-pitch samplers' shifters, in parallel: each instrument only touches its own pool, and each shifter's
// set-up is deterministic (a fixed seed and a silent priming run), so the result is the same as one by one
void prepareKeyShifts (const std::vector<Instrument*>& list, double sampleRate)
{
    if (list.empty())
        return;
    const loadtrace::Scope timing ("rack_keyshift");
    const auto threads = (std::size_t) std::clamp ((int) std::thread::hardware_concurrency() - 1, 1, 6);
    const char* off = std::getenv ("LYNX_PARALLEL_KEYSHIFT"); // "0": one by one (A/B and the null test)
    if (list.size() < 2 || threads < 2 || (off != nullptr && off[0] == '0'))
    {
        for (auto* i : list)
            i->prepareKeyShift (sampleRate);
        return;
    }
    std::atomic<std::size_t> next { 0 };
    const auto mode = FloatMode::current(); // the helpers round and flush like this thread
    auto work = [&] {
        mode.applyHere();
        for (auto k = next.fetch_add (1); k < list.size(); k = next.fetch_add (1))
            list[k]->prepareKeyShift (sampleRate);
    };
    std::vector<std::thread> helpers;
    for (std::size_t t = 1; t < std::min (threads, list.size()); ++t)
    {
        try
        {
            helpers.emplace_back (work);
        }
        catch (...) // no thread: the others (and this one) do the rest
        {
            break;
        }
    }
    work();
    for (auto& h : helpers)
        h.join();
}

} // namespace

std::unique_ptr<const RackState> RackState::build (Song song,
                                                   const RackState* previous,
                                                   double sampleRate,
                                                   const Factories& factories)
{
    const auto& samples = factories.samples;

    auto state = std::make_unique<RackState>();
    state->instruments.reserve (song.channels.size());
    std::vector<Instrument*> keyShiftLater; // new instruments only: none is visible to the audio thread yet

    for (std::size_t i = 0; i < song.channels.size(); ++i)
    {
        const auto& channel = song.channels[i];
        const auto kind = channel.instrument;

        if (previous != nullptr && i < previous->song.channels.size() && i < previous->instruments.size()
            && previous->song.channels[i].instrument == kind && kind != InstrumentKind::patcher
            && (kind != InstrumentKind::granulizer || previous->song.channels[i].samplePath == channel.samplePath)
            && (kind != InstrumentKind::soundfont
                || (previous->song.channels[i].samplePath == channel.samplePath && previous->sampleRate == sampleRate))
            && (kind != InstrumentKind::directwave
                || (previous->song.channels[i].zones == channel.zones && previous->song.channels[i].modelParams == channel.modelParams
                    && previous->song.channels[i].pluginState == channel.pluginState && previous->sampleRate == sampleRate))
            && ((kind != InstrumentKind::sampler && kind != InstrumentKind::audioClip && kind != InstrumentKind::flkeys)
                || (previous->song.channels[i].samplePath == channel.samplePath && previous->song.channels[i].zones == channel.zones
                    && previous->song.channels[i].modelParams == channel.modelParams
                    && previous->song.channels[i].sampler.keepPitch == channel.sampler.keepPitch
                    && (! channel.sampler.keepPitch
                        || (previous->song.channels[i].sampler.stretchBeats == channel.sampler.stretchBeats
                            && previous->song.channels[i].sampler.stretchMethod == channel.sampler.stretchMethod
                            && previous->song.bpm == song.bpm))))
            && (kind != InstrumentKind::plugin
                || (previous->song.channels[i].plugin.id == channel.plugin.id && previous->sampleRate == sampleRate
                    && samePluginProcessing (previous->song.channels[i].plugin, channel.plugin))))
        {
            state->instruments.push_back (previous->instruments[i]);
            state->instruments.back()->configure (channel, song.bpm);
            continue;
        }

        // FL's Patcher as a generator (REA_rodada141): same structure keeps the running graph (it takes the values)
        if (kind == InstrumentKind::patcher)
        {
            const auto* old = previous != nullptr && i < previous->instruments.size() ? previous->instruments[i].get() : nullptr;
            if (old != nullptr && previous->song.channels[i].instrument == kind && previous->sampleRate == sampleRate
                && patcher::sameStructure (previous->song.channels[i].patcher, channel.patcher))
            {
                state->instruments.push_back (previous->instruments[i]);
                state->instruments.back()->configure (channel, song.bpm);
                continue;
            }
            state->instruments.push_back (patcher::makeInstrument (channel, sampleRate, patcherFactories (sampleRate, song.bpm, factories), old));
            state->instruments.back()->configure (channel, song.bpm);
            continue;
        }

        state->instruments.push_back (buildInstrument (channel, sampleRate, song.bpm, factories, &keyShiftLater));
    }
    prepareKeyShifts (keyShiftLater, sampleRate);

    state->effects.resize (song.mixer.size());

    for (std::size_t t = 0; t < song.mixer.size(); ++t)
    {
        const auto& slots = song.mixer[t].effects;

        for (std::size_t i = 0; i < slots.size() && i < (std::size_t) maxEffectSlots; ++i)
        {
            const auto kind = slots[i].kind;

            if (previous != nullptr && t < previous->song.mixer.size() && t < previous->effects.size()
                && i < previous->song.mixer[t].effects.size() && i < previous->effects[t].size()
                && previous->song.mixer[t].effects[i].kind == kind && kind != EffectKind::patcher
                && (kind != EffectKind::parallel || sameContainer (previous->song.mixer[t].effects[i], slots[i])) // children are built in
                && previous->song.mixer[t].effects[i].samplePath == slots[i].samplePath
                // round 144: Facet EQ's buffers are sized by its linear-phase resolution: a new one, a new instance
                && (kind != EffectKind::facetEq
                    || facet::resolution (previous->song.mixer[t].effects[i].params[(std::size_t) facet::resolutionParam])
                           == facet::resolution (slots[i].params[(std::size_t) facet::resolutionParam]))
                && (kind != EffectKind::plugin
                    || (previous->song.mixer[t].effects[i].plugin.id == slots[i].plugin.id && previous->sampleRate == sampleRate
                        && samePluginProcessing (previous->song.mixer[t].effects[i].plugin, slots[i].plugin)))
                && (previous->song.mixer[t].effects[i].state == slots[i].state
                    || (previous->sampleRate == sampleRate && previous->effects[t][i] != nullptr && previous->effects[t][i]->acceptState (slots[i].state))))
            {
                state->effects[t].push_back (previous->effects[t][i]);
                continue;
            }

            // FL's Patcher (REA_rodada141): same structure keeps the running graph (it takes the values)
            if (kind == EffectKind::patcher)
            {
                const auto* old = previous != nullptr && t < previous->effects.size() && i < previous->effects[t].size()
                                          && i < previous->song.mixer[t].effects.size() && previous->song.mixer[t].effects[i].kind == kind
                                      ? previous->effects[t][i].get()
                                      : nullptr;
                if (old != nullptr && previous->sampleRate == sampleRate
                    && patcher::sameStructure (previous->song.mixer[t].effects[i].patcher, slots[i].patcher)
                    && patcher::acceptValues (previous->effects[t][i].get(), slots[i]))
                {
                    state->effects[t].push_back (previous->effects[t][i]);
                    continue;
                }
                std::shared_ptr<Effect> effect = patcher::makeEffect (slots[i], sampleRate, patcherFactories (sampleRate, song.bpm, factories), old);
                effect->prepare (sampleRate);
                state->effects[t].push_back (std::move (effect));
                continue;
            }

            state->effects[t].push_back (buildEffect (slots[i], sampleRate, factories));
        }
    }

    state->song = std::move (song);
    const auto tailStarted = std::chrono::steady_clock::now();

    // scheduling: notes sorted by start (order among notes on the same tick is preserved)
    for (auto& p : state->song.patterns)
        std::stable_sort (p.notes.begin(), p.notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    state->schedule = makeScheduleIndex (state->song);
    state->sampleRate = sampleRate;
    state->buildMidiRoutes();
    state->buildAutomationTables();
    {
        const loadtrace::Scope timing ("pdc");
        state->buildPdc (previous);
    }
    state->buildMixerGraph();

    const auto& channels = state->song.channels;
    state->cutTargets.assign (channels.size(), {});
    state->arpeggiators.assign (channels.size(), nullptr);

    for (std::size_t c = 0; c < channels.size(); ++c)
        if (channels[c].arp.enabled())
            state->arpeggiators[c] = previous != nullptr && c < previous->arpeggiators.size() && previous->arpeggiators[c] != nullptr
                                         ? previous->arpeggiators[c]
                                         : std::make_shared<Arpeggiator>();

    state->echoes.assign (channels.size(), nullptr);
    for (std::size_t c = 0; c < channels.size(); ++c)
        if (channels[c].misc.echo.enabled())
            state->echoes[c] = previous != nullptr && c < previous->echoes.size() && previous->echoes[c] != nullptr
                                   ? previous->echoes[c]
                                   : std::make_shared<channelmisc::NoteEcho>();

    for (std::size_t c = 0; c < channels.size(); ++c)
        if (channels[c].cutGroup > 0)
            for (std::size_t d = 0; d < channels.size(); ++d)
                if (channels[d].cutBy == channels[c].cutGroup)
                {
                    state->cutTargets[c].push_back ((int) d);
                    state->hasCutGroups = true;
                }
    state->hasLayers = std::any_of (channels.begin(), channels.end(), [] (const ChannelDef& c) {
        return c.instrument == InstrumentKind::layer && ! c.layer.children.empty();
    });
    const auto audible = state->song.soloAudible();
    state->mixerAudible.assign (audible.begin(), audible.end());
    loadtrace::add ("rack_schedule_graph", loadtrace::msSince (tailStarted));
    return state;
}

void RackState::buildMixerGraph()
{
    mixerOrder = song.mixerOrder();
    const auto audible = song.soloAudible();
    mixerAudible.assign (audible.begin(), audible.end());

    const auto tracks = std::max<int> (1, (int) song.mixer.size());
    mixerInputs.assign ((std::size_t) tracks, {});
    sendTaps.clear();
    slotTap.assign ((std::size_t) tracks, {});
    std::vector<int> depth ((std::size_t) tracks, 0);

    // who listens to each track via sidechain (the source goes in an earlier layer)
    std::vector<std::vector<int>> sidechainListeners ((std::size_t) tracks);
    for (int t = 1; t < tracks; ++t)
        for (const auto& fx : song.mixer[(std::size_t) t].effects)
            for (const auto source : { fx.sidechain, fx.sidechain2 })
                if (source > 0 && source < tracks && source != t)
                    sidechainListeners[(std::size_t) source].push_back (t);

    for (auto t : mixerOrder) // sources before destinations: the depth is already computed
    {
        const auto& track = song.mixer[(std::size_t) t];

        for (const auto listener : sidechainListeners[(std::size_t) t])
            depth[(std::size_t) listener] = std::max (depth[(std::size_t) listener], depth[(std::size_t) t] + 1);

        if (track.toMaster)
            mixerInputs[0].push_back ({ t, 0, track.masterLevel });

        for (std::size_t k = 0; k < track.sends.size(); ++k)
        {
            const auto& send = track.sends[k];

            if (send.target > 0 && send.target < tracks && send.target != t && send.level > 0.0f)
            {
                mixerInputs[(std::size_t) send.target].push_back ({ t, (int) k + 1, send.level });
                depth[(std::size_t) send.target] = std::max (depth[(std::size_t) send.target], depth[(std::size_t) t] + 1);
            }
        }

        // Fruity Send slots: their own signal, delay lines after the sends' (PdcPlan::sendDelay)
        const auto taps = fruitySendsOf (song, t);
        for (std::size_t j = 0; j < taps.size(); ++j)
        {
            const auto [slot, target] = taps[j];
            if ((int) sendTaps.size() >= maxSendTaps)
                break;
            if ((std::size_t) t >= slotTap.size())
                slotTap.resize ((std::size_t) t + 1);
            if ((std::size_t) slot >= slotTap[(std::size_t) t].size())
                slotTap[(std::size_t) t].resize ((std::size_t) slot + 1, -1);
            slotTap[(std::size_t) t][(std::size_t) slot] = (int) sendTaps.size();
            mixerInputs[(std::size_t) target].push_back ({ t, (int) (1 + track.sends.size() + j), 1.0f, (int) sendTaps.size() });
            sendTaps.push_back ({ t, slot, target });
            depth[(std::size_t) target] = std::max (depth[(std::size_t) target], depth[(std::size_t) t] + 1);
        }
    }

    mixerLayers.clear();

    for (auto t : mixerOrder)
    {
        const auto d = (std::size_t) depth[(std::size_t) t];

        if (mixerLayers.size() <= d)
            mixerLayers.resize (d + 1);

        mixerLayers[d].push_back (t);
    }

    // splitting across threads costs a few microseconds per block; only worth it with plugins or many channels
    const auto plugins = std::count_if (song.channels.begin(), song.channels.end(),
                                        [] (const ChannelDef& c) { return c.instrument == InstrumentKind::plugin; });
    auto pluginEffects = 0;
    for (const auto& t : song.mixer)
        pluginEffects += (int) std::count_if (t.effects.begin(), t.effects.end(), [] (const EffectSlot& e) { return e.kind == EffectKind::plugin; });

    parallel = plugins + pluginEffects >= 2 || song.channels.size() >= 24;

    // plugins whose "Allow threaded processing" is off pin their channel / insert to the audio thread
    threadedChannels.clear();
    audioThreadChannels.clear();
    for (int c = 0; c < (int) song.channels.size(); ++c)
    {
        const auto& ch = song.channels[(std::size_t) c];
        // (and a generator Patcher holding a Hardware Insert, round 165c: the device-output buses are not shared between threads)
        const auto pinned = (ch.instrument == InstrumentKind::plugin && ! ch.plugin.threaded)
                            || (ch.instrument == InstrumentKind::patcher && patcher::usesHardwareIo (ch.patcher));
        (pinned ? audioThreadChannels : threadedChannels).push_back (c);
    }
    threadedLayers.assign (mixerLayers.size(), {});
    audioThreadLayers.assign (mixerLayers.size(), {});
    for (std::size_t d = 0; d < mixerLayers.size(); ++d)
        for (const auto t : mixerLayers[d])
        {
            const auto& effects = song.mixer[(std::size_t) t].effects;
            // (and the hardware inserts, round 164d: the device-output buses they write are not shared between threads)
            const auto pinned = ! song.mixer[(std::size_t) t].threaded || std::any_of (effects.begin(), effects.end(),
                                             [] (const EffectSlot& e) {
                                                 return (e.kind == EffectKind::plugin && ! e.plugin.threaded) || e.kind == EffectKind::hardwareInsert
                                                        || (e.kind == EffectKind::patcher && patcher::usesHardwareIo (e.patcher)); // (round 165c)
                                             });
            (pinned ? audioThreadLayers : threadedLayers)[d].push_back (t);
        }

    buildTaskGraph (depth);
}

void RackState::buildTaskGraph (const std::vector<int>& depth)
{
    const auto tracks = std::max<int> (1, (int) song.mixer.size());
    graphChannels = std::min<int> ((int) song.channels.size(), RackRenderer::maxChannels);
    trackChannels.assign ((std::size_t) tracks, {});
    for (int c = 0; c < graphChannels; ++c)
        trackChannels[(std::size_t) std::clamp (song.channels[(std::size_t) c].mixerTrack, 0, tracks - 1)].push_back (c);

    graphOk = false;
    graphIndegree.clear();
    graphNextStart.clear();
    graphNext.clear();
    graphRoots.clear();

    const auto pinned = ! audioThreadChannels.empty()
                     || std::any_of (audioThreadLayers.begin(), audioThreadLayers.end(), [] (const std::vector<int>& l) { return ! l.empty(); });
    if (pinned)
        return;

    const auto tasks = graphChannels + tracks - 1;
    const auto trackTask = [this] (int t) { return graphChannels + t - 1; };
    std::vector<std::vector<int>> next ((std::size_t) tasks);
    auto ok = true;
    auto edge = [&] (int from, int to) {
        // the layers run a source in an earlier layer than its destination; a cycle breaks that (the layered path
        // then reads the source before it ran): keep the layers for such a mixer
        if (depth[(std::size_t) to] <= depth[(std::size_t) from])
            ok = false;
        next[(std::size_t) trackTask (from)].push_back (trackTask (to));
    };

    for (int c = 0; c < graphChannels; ++c)
        if (const auto t = std::clamp (song.channels[(std::size_t) c].mixerTrack, 0, tracks - 1); t > 0)
            next[(std::size_t) c].push_back (trackTask (t));
    for (int t = 1; t < tracks; ++t)
    {
        for (const auto& in : mixerInputs[(std::size_t) t])
            if (in.source > 0 && in.source < tracks && in.source != t)
                edge (in.source, t);
        for (const auto& fx : song.mixer[(std::size_t) t].effects)
            for (const auto source : { fx.sidechain, fx.sidechain2 })
                if (source > 0 && source < tracks && source != t)
                    edge (source, t);
    }
    if (! ok)
        return;

    graphIndegree.assign ((std::size_t) tasks, 0);
    graphNextStart.assign ((std::size_t) tasks + 1, 0);
    for (int i = 0; i < tasks; ++i)
    {
        auto& n = next[(std::size_t) i];
        std::sort (n.begin(), n.end());
        n.erase (std::unique (n.begin(), n.end()), n.end());
        graphNextStart[(std::size_t) i + 1] = graphNextStart[(std::size_t) i] + (int) n.size();
        for (const auto j : n)
        {
            graphNext.push_back (j);
            ++graphIndegree[(std::size_t) j];
        }
    }
    // the channels first (they are most of the work and every insert waits for some of them), then the inserts
    // nothing feeds
    for (int i = 0; i < tasks; ++i)
        if (graphIndegree[(std::size_t) i] == 0)
            graphRoots.push_back (i);
    graphOk = true;
}

std::vector<int> RackState::currentLatencies() const
{
    std::vector<int> out;

    // a hosted plugin's latency set by hand in its wrapper settings (round 149) replaces what it reports
    auto manual = [this] (const PluginRef& ref, int reported) {
        return ref.latencyOverrideMs >= 0.0f ? (int) std::lround (ref.latencyOverrideMs * 0.001 * sampleRate) : reported;
    };
    for (std::size_t c = 0; c < instruments.size(); ++c)
    {
        const auto reported = instruments[c]->latencySamples();
        out.push_back (c < song.channels.size() && song.channels[c].instrument == InstrumentKind::plugin ? manual (song.channels[c].plugin, reported) : reported);
    }

    for (std::size_t t = 0; t < effects.size(); ++t)
        for (std::size_t s = 0; s < effects[t].size(); ++s)
        {
            const auto reported = effects[t][s]->latencySamples();
            const auto* slot = t < song.mixer.size() && s < song.mixer[t].effects.size() ? &song.mixer[t].effects[s] : nullptr;
            out.push_back (slot != nullptr && slot->kind == EffectKind::plugin ? manual (slot->plugin, reported) : reported);
        }

    return out;
}

void RackState::buildPdc (const RackState* previous)
{
    latencies = currentLatencies();

    std::vector<int> channelLatency (latencies.begin(), latencies.begin() + (std::ptrdiff_t) instruments.size());
    std::vector<std::vector<int>> slotLatency (effects.size());
    auto next = latencies.begin() + (std::ptrdiff_t) instruments.size();

    for (std::size_t t = 0; t < effects.size(); ++t)
        for (std::size_t s = 0; s < effects[t].size(); ++s)
            slotLatency[t].push_back (*next++);

    pdc = computePdc (song, channelLatency, slotLatency);

    // reuses lines of the same length: editing the song must not click the audio
    auto line = [] (int length, const std::shared_ptr<DelayLine>* old) -> std::shared_ptr<DelayLine> {
        if (length <= 0)
            return nullptr;
        if (old != nullptr && *old != nullptr && (*old)->length() == length)
            return *old;
        return std::make_shared<DelayLine> (length);
    };

    channelDelays.resize (pdc.channelDelay.size());

    for (std::size_t c = 0; c < pdc.channelDelay.size(); ++c)
        channelDelays[c] = line (pdc.channelDelay[c],
                                 previous != nullptr && c < previous->channelDelays.size() ? &previous->channelDelays[c] : nullptr);

    sendDelays.resize (pdc.sendDelay.size());

    for (std::size_t t = 0; t < pdc.sendDelay.size(); ++t)
    {
        sendDelays[t].resize (pdc.sendDelay[t].size());

        for (std::size_t k = 0; k < pdc.sendDelay[t].size(); ++k)
        {
            const std::shared_ptr<DelayLine>* old = nullptr;

            if (previous != nullptr && t < previous->sendDelays.size() && k < previous->sendDelays[t].size())
                old = &previous->sendDelays[t][k];

            sendDelays[t][k] = line (pdc.sendDelay[t][k], old);
        }
    }

    // inserts with a hardware output target: aligned with the Master's output (which waits for the longest path)
    hwRoutes.clear();
    for (std::size_t t = 1; t < song.mixer.size() && t < (std::size_t) maxMixerTracks; ++t)
    {
        const auto& track = song.mixer[t];
        if (track.outputChannel < 0 || track.outputChannel >= HardwareOutput::maxChannels)
            continue;
        const auto length = t < pdc.outLat.size() ? std::max (0, pdc.total - pdc.outLat[t]) : 0;
        const std::shared_ptr<DelayLine>* old = nullptr;
        if (previous != nullptr)
            for (const auto& r : previous->hwRoutes)
                if (r.track == (int) t)
                    old = &r.delay;
        hwRoutes.push_back ({ (int) t, track.outputChannel, track.outputMode, line (length, old) });
    }
}

void RackState::buildMidiRoutes()
{
    const auto count = song.channels.size();
    midiTargets.assign (count, {});
    midiSources.assign (count, {});
    midiOutChannel.assign (count, -1);
    vfxChannels.clear();
    dashboardChannels.clear();
    vfxPort.assign (count, -1);
    hasMidiRoutes = false;
    midiOutTargets.assign (count, {});
    hasMidiOutputs = false;
    for (std::size_t c = 0; c < count; ++c)
        if (song.channels[c].instrument == InstrumentKind::plugin && song.channels[c].plugin.midiOutPort >= 0)
            for (std::size_t t = 0; t < count; ++t)
                if (t != c && song.channels[t].instrument == InstrumentKind::plugin
                    && song.channels[t].plugin.midiInPort == song.channels[c].plugin.midiOutPort)
                {
                    midiOutTargets[c].push_back ((int) t);
                    hasMidiOutputs = true;
                }
    midiRoutesOut.assign (count, {});
    midiRoutesIn.assign (count, {});
    effectMidiRoutes.assign (count, {});
    hasEffectMidiRoutes = false;

    // the Patchers that take MIDI ports through their "From LYNX" pins (round 162)
    std::vector<std::vector<int>> channelPorts (count);
    for (std::size_t t = 0; t < count; ++t)
        if (song.channels[t].instrument == InstrumentKind::patcher)
            channelPorts[t] = patcher::midiInputPorts (song.channels[t].patcher);
    struct SlotPorts
    {
        int track, slot;
        std::vector<int> ports;
    };
    std::vector<SlotPorts> slotPorts;
    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t k = 0; k < song.mixer[t].effects.size(); ++k)
            if (song.mixer[t].effects[k].kind == EffectKind::patcher)
                if (auto ports = patcher::midiInputPorts (song.mixer[t].effects[k].patcher); ! ports.empty())
                    slotPorts.push_back ({ (int) t, (int) k, std::move (ports) });

    // source c sends on `port` with MIDI channel `midiChannel`: the plugins whose input port it is, the Patchers that
    // take that port
    // (a Patcher that takes the port may pass it on to "To LYNX" MIDI ports: those are sent on too, a few levels deep)
    std::function<void (std::size_t, int, int, int)> send = [&] (std::size_t c, int port, int midiChannel, int depth) {
        auto passOn = [&] (const PatcherDoc& doc) {
            if (depth < 4)
                for (const auto& r : patcher::midiOutputsOf (doc, patchMidiPortPin (port)))
                    if (r.port != port)
                        send (c, r.port, r.midiChannel >= 0 ? r.midiChannel : midiChannel, depth + 1);
        };
        if (midiOutChannel[c] < 0)
            midiOutChannel[c] = midiChannel;
        for (std::size_t t = 0; t < count; ++t)
        {
            if (t == c)
                continue;
            const auto plugin = song.channels[t].instrument == InstrumentKind::plugin && song.channels[t].plugin.midiInPort == port;
            const auto patch = std::find (channelPorts[t].begin(), channelPorts[t].end(), port) != channelPorts[t].end();
            if (! plugin && ! patch)
                continue;
            if (std::any_of (midiRoutesOut[c].begin(), midiRoutesOut[c].end(), [&] (const MidiRoute& r) {
                    return r.other == (int) t && r.port == port && r.midiChannel == midiChannel;
                }))
                continue;
            midiRoutesOut[c].push_back ({ (int) t, port, midiChannel });
            midiRoutesIn[t].push_back ({ (int) c, port, midiChannel });
            if (std::find (midiSources[t].begin(), midiSources[t].end(), (int) c) == midiSources[t].end())
                midiSources[t].push_back ((int) c);
            if (plugin && std::find (midiTargets[c].begin(), midiTargets[c].end(), (int) t) == midiTargets[c].end())
                midiTargets[c].push_back ((int) t); // (controls and program changes: the plugins)
            hasMidiRoutes = true;
            if (patch)
                passOn (song.channels[t].patcher);
        }
        for (const auto& s : slotPorts)
            if (std::find (s.ports.begin(), s.ports.end(), port) != s.ports.end()
                && std::none_of (effectMidiRoutes[c].begin(), effectMidiRoutes[c].end(), [&] (const EffectMidiRoute& r) {
                       return r.track == s.track && r.slot == s.slot && r.port == port && r.midiChannel == midiChannel;
                   }))
            {
                effectMidiRoutes[c].push_back ({ s.track, s.slot, port, midiChannel });
                hasEffectMidiRoutes = true;
                passOn (song.mixer[(std::size_t) s.track].effects[(std::size_t) s.slot].patcher);
            }
    };

    for (std::size_t c = 0; c < count; ++c)
    {
        // a generator Patcher's "MIDI port N" outputs (REA_rodada141): its notes reach the plugins on those ports, as a
        // MIDI Out on MIDI channel 1 would send them (or on the channel of a MIDI Out node on the way, round 162)
        if (song.channels[c].instrument == InstrumentKind::patcher)
        {
            for (const auto& r : patcher::midiOutputsOf (song.channels[c].patcher, 0))
                send (c, r.port, std::max (0, r.midiChannel), 0);
            continue;
        }
        if (song.channels[c].instrument == InstrumentKind::vfx) // a VFX channel: output k on port + k (round 164b)
        {
            if (const auto vs = vfx::readState (song.channels[c].pluginState))
            {
                vfxChannels.push_back ((int) c);
                vfxPort[c] = vs->port;
                for (int k = 0; k < vfx::outputCount (vs->kind) && vs->port + k < patchMidiPorts; ++k)
                    send (c, vs->port + k, 0, 0);
            }
            continue;
        }
        if (song.channels[c].instrument == InstrumentKind::dashboard) // its notes and controls' MIDI (round 165)
        {
            const auto settings = surface::dashboardMidi (song.channels[c].pluginState);
            midiOutChannel[c] = settings.channel;
            dashboardChannels.push_back ((int) c);
            send (c, settings.port, settings.channel, 0);
            continue;
        }
        if (song.channels[c].instrument != InstrumentKind::midiOut)
            continue;
        const auto settings = midictl::parseMidiOut (song.channels[c].pluginState);
        midiOutChannel[c] = settings.channel;
        send (c, settings.port, settings.channel, 0);
    }
}

void RackState::buildAutomationTables()
{
    const auto& automations = song.automations;
    const auto count = std::min<int> ((int) automations.size(), maxAutomations);

    automationClips.assign ((std::size_t) count, {});

    for (int i = 0; i < (int) song.playlist.size(); ++i)
    {
        const auto& c = song.playlist[(std::size_t) i];

        if (c.kind == ClipKind::automation && c.automation >= 0 && c.automation < count && c.length > 0 && ! c.muted && ! song.isTrackMuted (c.track))
            automationClips[(std::size_t) c.automation].push_back (i);
    }

    for (auto& clips : automationClips)
        std::sort (clips.begin(), clips.end(),
                   [this] (int a, int b) { return song.playlist[(std::size_t) a].start < song.playlist[(std::size_t) b].start; });

    liveAutomations.clear();
    for (int a = 0; a < count; ++a)
        if (automations[(std::size_t) a].isLive())
            liveAutomations.push_back (a);

    channelVolumeAuto.assign (song.channels.size(), -1);
    channelPanAuto.assign (song.channels.size(), -1);
    mixerVolumeAuto.assign (song.mixer.size(), -1);
    mixerPanAuto.assign (song.mixer.size(), -1);
    effectParamAuto.assign (song.mixer.size(), {});
    effectMixAuto.assign (song.mixer.size(), {});

    for (std::size_t t = 0; t < song.mixer.size(); ++t)
    {
        std::array<int, maxEffectParams> none;
        none.fill (-1);
        effectParamAuto[t].assign (song.mixer[t].effects.size(), none);
        effectMixAuto[t].assign (song.mixer[t].effects.size(), -1);
    }

    pluginParams.clear();
    hasAutomation = false;
    tempoAutomation = -1;

    // binds automation slot `a` (an automation, or a pattern event lane after them) to its target
    const auto bind = [this] (int a, const AutomationTarget& t) {
        const auto channelOk = t.index >= 0 && t.index < (int) song.channels.size();
        const auto trackOk = t.index >= 0 && t.index < (int) song.mixer.size();
        auto used = true;

        switch (t.kind)
        {
            case AutomationTargetKind::channelVolume: if (channelOk) channelVolumeAuto[(std::size_t) t.index] = a; else used = false; break;
            case AutomationTargetKind::channelPan: if (channelOk) channelPanAuto[(std::size_t) t.index] = a; else used = false; break;
            case AutomationTargetKind::mixerVolume: if (trackOk) mixerVolumeAuto[(std::size_t) t.index] = a; else used = false; break;
            case AutomationTargetKind::mixerPan: if (trackOk) mixerPanAuto[(std::size_t) t.index] = a; else used = false; break;

            case AutomationTargetKind::channelParam:
                // VST plugins and the exact engines that take FL's parameter automation (FL Slayer: REA_rodada79)
                if (channelOk && (song.channels[(std::size_t) t.index].instrument == InstrumentKind::plugin
                                  || song.channels[(std::size_t) t.index].instrument == InstrumentKind::patcher // (its published parameters)
                                  || song.channels[(std::size_t) t.index].instrument == InstrumentKind::slayer
                                  || song.channels[(std::size_t) t.index].instrument == InstrumentKind::soundfont // (LYNX Soundfont's knobs)
                                  || (song.channels[(std::size_t) t.index].instrument == InstrumentKind::midiOut && midictl::isMidiParam (t.param))
                                  // an Envelope Controller's global Mod X / Y (round 164f)
                                  || (song.channels[(std::size_t) t.index].instrument == InstrumentKind::controller
                                      && (t.param == envelopeControllerModXParam || t.param == envelopeControllerModYParam))
                                  // a Dashboard's outputs, the Voltage Controller's offsets / scales (round 165)
                                  || (song.channels[(std::size_t) t.index].instrument == InstrumentKind::dashboard && t.param >= 0
                                      && t.param < surface::maxOutputs)
                                  || (song.channels[(std::size_t) t.index].instrument == InstrumentKind::voltage && t.param >= 0
                                      && t.param < voltage::paramCount)))
                    pluginParams.push_back ({ a, t.index, 0, 0, t.param });
                else
                    used = false;
                break;

            case AutomationTargetKind::effectParam:
            {
                const auto slotOk = trackOk && t.slot >= 0 && t.slot < (int) song.mixer[(std::size_t) t.index].effects.size();

                if (! slotOk)
                    used = false;
                else if (song.mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].kind == EffectKind::plugin
                         || song.mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].kind == EffectKind::patcher) // (its published parameters)
                    pluginParams.push_back ({ a, -1, t.index, t.slot, t.param });
                else if (song.mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].isSubstitute())
                    used = false; // the missing plugin's parameters: kept for when it is back, they don't drive the substitute
                else if (t.param >= 0 && t.param < maxEffectParams)
                    effectParamAuto[(std::size_t) t.index][(std::size_t) t.slot][(std::size_t) t.param] = a;
                else
                    used = false;
                break;
            }

            case AutomationTargetKind::effectMix:
                if (trackOk && t.slot >= 0 && t.slot < (int) song.mixer[(std::size_t) t.index].effects.size())
                    effectMixAuto[(std::size_t) t.index][(std::size_t) t.slot] = a;
                else
                    used = false;
                break;

            case AutomationTargetKind::tempo: tempoAutomation = a; break;
            case AutomationTargetKind::none: used = false; break;
        }

        hasAutomation = hasAutomation || used;
    };

    // the last automation of the same target wins
    for (int a = 0; a < count; ++a)
    {
        if (automationClips[(std::size_t) a].empty() && ! automations[(std::size_t) a].isLive())
            continue;

        bind (a, automations[(std::size_t) a].target);
    }

    // pattern events (event editor): slots after the automations; they win over automation clips of the same
    // target. The tempo is not driven by pattern events.
    eventLanes.clear();
    patternClips.assign (song.patterns.size(), {});

    for (int i = 0; i < (int) song.playlist.size(); ++i)
    {
        const auto& c = song.playlist[(std::size_t) i];
        if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) song.patterns.size() && c.length > 0 && ! c.muted
            && ! song.isTrackMuted (c.track))
            patternClips[(std::size_t) c.pattern].push_back (i);
    }

    for (auto& clips : patternClips)
        std::sort (clips.begin(), clips.end(),
                   [this] (int a, int b) { return song.playlist[(std::size_t) a].start < song.playlist[(std::size_t) b].start; });

    auto slot = count;

    for (int p = 0; p < (int) song.patterns.size(); ++p)
        for (int l = 0; l < (int) song.patterns[(std::size_t) p].events.size(); ++l)
        {
            const auto& lane = song.patterns[(std::size_t) p].events[(std::size_t) l];

            if (lane.events.empty() || lane.target.kind == AutomationTargetKind::tempo || slot >= maxAutomations)
                continue;

            eventLanes.push_back ({ p, l, slot });
            bind (slot++, lane.target);
        }
}

void RackState::slotsOfTarget (const Song& song, const AutomationTarget& target, std::vector<int>& out)
{
    out.clear();
    if (target.kind == AutomationTargetKind::none)
        return;
    const auto count = std::min<int> ((int) song.automations.size(), maxAutomations);
    for (int a = 0; a < count; ++a)
        if (song.automations[(std::size_t) a].target == target)
            out.push_back (a);

    auto slot = count;
    for (const auto& pattern : song.patterns)
        for (const auto& lane : pattern.events)
        {
            if (lane.events.empty() || lane.target.kind == AutomationTargetKind::tempo || slot >= maxAutomations)
                continue;
            if (lane.target == target)
                out.push_back (slot);
            ++slot;
        }
}

namespace
{
// Constant-power pan: at center both sides are 1 (0 dB).
void panGains (float pan, float gain, float& left, float& right) noexcept
{
    const auto angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    left = gain * std::cos (angle) * 1.41421356f;
    right = gain * std::sin (angle) * 1.41421356f;
}

// FL's mixer fader and pan: each side's gain moves toward its target by at most 0.001 per sample (a linear glide;
// measured on FL's renders of pattern events on a track's volume and pan, REA_rodada85 §3). Returns the gain reached.
float glideGain (const float* in, float* out, int n, float from, float to) noexcept
{
    constexpr float step = 0.001f;
    auto g = from;
    int i = 0;

    for (; i < n && g != to; ++i)
    {
        g = to > g ? std::min (to, g + step) : std::max (to, g - step);
        out[i] = in[i] * g;
    }

    for (; i < n; ++i)
        out[i] = in[i] * g;

    return g;
}

float peakOf (const float* data, int n) noexcept
{
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const auto a = std::abs (data[i]);
        peak = a > peak ? a : peak; // form the compiler vectorizes
    }

    return peak;
}

// No NaN / inf in the block? (x · 0 is 0 for every finite x: one vectorized pass)
bool finiteBlock (const float* l, const float* r, int n) noexcept
{
    float acc = 0.0f;
    for (int k = 0; k < n; ++k)
        acc += l[k] * 0.0f + r[k] * 0.0f;
    return acc == 0.0f;
}

std::atomic<std::uint64_t> nonFiniteBlocks { 0 };

// Any sample above `level`? Stops at the first: with sound (the common case) it costs almost nothing.
bool anyAbove (const float* l, const float* r, int n, float level) noexcept
{
    // in blocks of 32: the inner loop vectorizes; silent chunks (the ones that scan everything) stay cheap
    int i = 0;

    for (; i + 32 <= n; i += 32)
    {
        float m = 0.0f;

        for (int k = i; k < i + 32; ++k)
        {
            const auto a = std::abs (l[k]), b = std::abs (r[k]);
            m = a > m ? a : m;
            m = b > m ? b : m;
        }

        if (m > level)
            return true;
    }

    for (; i < n; ++i)
        if (std::abs (l[i]) > level || std::abs (r[i]) > level)
            return true;

    return false;
}
} // namespace

namespace
{
// A Patcher-style container bakes its children (kinds, states, parameters, routing) in when it is built, so it can be
// reused only if all of that is unchanged. (It used to be rebuilt on every edit: its children's tails and delay lines
// restarted, and a fresh linear-phase child reporting its latency only after the first block made the engine re-plan
// PDC and rebuild it again, over and over — REA round 87.)
bool sameSlotSetup (const EffectSlot& a, const EffectSlot& b);

bool sameBranches (const EffectSlot& a, const EffectSlot& b)
{
    if (a.branches.size() != b.branches.size() || a.links.size() != b.links.size())
        return false;
    for (std::size_t i = 0; i < a.branches.size(); ++i)
    {
        const auto& x = a.branches[i];
        const auto& y = b.branches[i];
        if (x.gain != y.gain || x.chain.size() != y.chain.size())
            return false;
        for (std::size_t k = 0; k < x.chain.size(); ++k)
            if (! sameSlotSetup (x.chain[k], y.chain[k]))
                return false;
    }
    for (std::size_t i = 0; i < a.links.size(); ++i)
    {
        const auto& x = a.links[i];
        const auto& y = b.links[i];
        if (x.branch != y.branch || x.source != y.source || x.target != y.target || x.param != y.param || x.curve != y.curve)
            return false;
    }
    return true;
}

bool sameSlotSetup (const EffectSlot& a, const EffectSlot& b)
{
    return a.kind == b.kind && a.enabled == b.enabled && a.params == b.params && a.mix == b.mix && a.plugin.id == b.plugin.id
           && a.sidechain == b.sidechain && a.sidechain2 == b.sidechain2 && a.state == b.state && a.samplePath == b.samplePath
           && sameBranches (a, b);
}
} // namespace

bool sameContainer (const EffectSlot& a, const EffectSlot& b) { return sameBranches (a, b); }

namespace
{
std::uint64_t nowNs() noexcept
{
#if defined(__linux__)
    // diagnostics on a busy machine: LYNX_PROFILE_CPU=1 measures each unit in its thread's CPU time (renderwav)
    static const bool threadCpu = std::getenv ("LYNX_PROFILE_CPU") != nullptr;
    if (threadCpu)
    {
        timespec ts {};
        clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
        return (std::uint64_t) ts.tv_sec * 1000000000ull + (std::uint64_t) ts.tv_nsec;
    }
#endif
    return (std::uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Adds the time since `start` to `slot` when profiling (start = 0: profiling was off when the unit began)
struct UnitTimer
{
    UnitTimer (bool on, std::atomic<std::uint64_t>& s, std::atomic<std::uint64_t>* m = nullptr) noexcept
        : slot (s), longest (m), start (on ? nowNs() : 0) {}
    ~UnitTimer()
    {
        if (start == 0)
            return;
        const auto used = nowNs() - start;
        slot.fetch_add (used, std::memory_order_relaxed);
        if (longest != nullptr)
        {
            auto current = longest->load (std::memory_order_relaxed);
            while (used > current && ! longest->compare_exchange_weak (current, used, std::memory_order_relaxed))
            {
            }
        }
    }
    std::atomic<std::uint64_t>& slot;
    std::atomic<std::uint64_t>* longest;
    std::uint64_t start;
};
} // namespace

void RackRenderer::resetUnitTimes() noexcept
{
    for (auto& t : units->channelNs)
        t.store (0, std::memory_order_relaxed);
    for (auto& t : units->effectNs)
        t.store (0, std::memory_order_relaxed);
    for (auto& t : units->channelMaxNs)
        t.store (0, std::memory_order_relaxed);
    for (auto& t : units->effectMaxNs)
        t.store (0, std::memory_order_relaxed);
    units->processNs.store (0, std::memory_order_relaxed);
    units->processedSamples.store (0, std::memory_order_relaxed);
}

void RackRenderer::prepare (double sr, int maxBlockSize)
{
    automationValues.fill (std::numeric_limits<float>::quiet_NaN());
    lastTrackGain.fill (std::numeric_limits<float>::quiet_NaN());
    channelBuffers.assign ((std::size_t) maxChannels * 2 * chunkCapacity, 0.0f);
    smartDisable.assign ((std::size_t) maxMixerTracks * maxEffectSlots, {});
    trackEqs.assign ((std::size_t) maxMixerTracks, {});
    trackScratchBuffers.assign ((std::size_t) maxMixerTracks * 2 * chunkCapacity, 0.0f);
    tapBuffers.assign ((std::size_t) RackState::maxSendTaps * 2 * chunkCapacity, 0.0f);
    sampleRate = sr;
    blockCapacity = std::max (64, maxBlockSize);
    mixBuffers.assign ((std::size_t) maxMixerTracks * 2 * (std::size_t) blockCapacity, 0.0f);
    hwScratch.assign (2 * (std::size_t) blockCapacity, 0.0f);
    layerPick.assign ((std::size_t) maxChannels * 128, (std::int16_t) -1);

    if (lastState != nullptr)
    {
        for (auto& instrument : lastState->instruments)
            instrument->prepare (sr);

        for (auto& track : lastState->effects)
            for (auto& effect : track)
                effect->prepare (sr);
    }
}

float* RackRenderer::trackBuffer (int track, int channel) noexcept
{
    return mixBuffers.data() + ((std::size_t) track * 2 + (std::size_t) channel) * (std::size_t) blockCapacity;
}

float* RackRenderer::tapBuffer (int tap, int side) noexcept
{
    return tapBuffers.data() + ((std::size_t) tap * 2 + (std::size_t) side) * chunkCapacity;
}

float RackRenderer::automated (int automation, float base) const noexcept
{
    if (automation < 0)
        return base;

    const auto v = automationValues[(std::size_t) automation];
    return std::isnan (v) ? base : v;
}

namespace
{
// Value of an automation at a song position: inside a clip, the curve; between clips,
// the last value of the previous clip; before the first, its initial value. Depends only on
// position, so playing from any point (or exporting) gives the same result.
float automationValueAt (const Song& song, const Automation& automation, const std::vector<int>& clips, double tick) noexcept
{
    const Clip* before = nullptr;

    for (auto i : clips) // sorted by start
    {
        const auto& c = song.playlist[(std::size_t) i];

        if (tick < (double) c.start)
            break;

        if (tick < (double) c.end())
            return automation.valueAt (tick - (double) c.start + (double) c.offset);

        before = &c;
    }

    if (before != nullptr)
        return automation.valueAt ((double) (before->length + before->offset));

    const auto& first = song.playlist[(std::size_t) clips.front()];
    return automation.valueAt ((double) first.offset);
}
} // namespace

void RackRenderer::setOverriddenSlots (const std::vector<int>& slots) noexcept
{
    std::array<std::uint32_t, RackState::maxAutomations / 32> bits {};
    for (const auto s : slots)
        if (s >= 0 && s < RackState::maxAutomations)
            bits[(std::size_t) s / 32] |= 1u << ((unsigned) s % 32);
    for (std::size_t i = 0; i < bits.size(); ++i)
        overridden[i].store (bits[i], std::memory_order_relaxed);
    anyOverridden.store (! slots.empty(), std::memory_order_release);
}

void RackRenderer::clearOverriddenValues() noexcept
{
    if (! anyOverridden.load (std::memory_order_acquire))
        return;
    for (std::size_t w = 0; w < overridden.size(); ++w)
        if (auto bits = overridden[w].load (std::memory_order_relaxed); bits != 0)
            for (unsigned b = 0; b < 32; ++b)
                if ((bits & (1u << b)) != 0)
                    automationValues[w * 32 + b] = std::numeric_limits<float>::quiet_NaN(); // the control's own value
}

void RackRenderer::updateAutomation (const RackState& state, double startTick) noexcept
{
    const auto& song = state.song;

    // controllers (live sources): the effect's output at the end of the previous block, in any mode
    for (const auto a : state.liveAutomations)
    {
        const auto& automation = song.automations[(std::size_t) a];
        const auto& src = automation.source;
        if (src.channel >= 0)
        {
            if (src.channel < (int) state.instruments.size() && state.instruments[(std::size_t) src.channel] != nullptr)
                automationValues[(std::size_t) a] = automation.mapLive (state.instruments[(std::size_t) src.channel]->controlOutput (src.output));
        }
        else if (src.track < (int) state.effects.size() && src.slot >= 0 && src.slot < (int) state.effects[(std::size_t) src.track].size()
            && state.effects[(std::size_t) src.track][(std::size_t) src.slot] != nullptr)
            automationValues[(std::size_t) a] = automation.mapLive (state.effects[(std::size_t) src.track][(std::size_t) src.slot]->controlOutput (src.output));
    }

    // pattern events: in pattern mode the current pattern's (looped); in song mode those of the pattern clip
    // that covers the position. Elsewhere the value holds.
    if (! state.eventLanes.empty())
    {
        if (song.mode != PlayMode::song)
        {
            const auto current = song.currentPattern;

            if (current >= 0 && current < (int) song.patterns.size())
            {
                const auto& pattern = song.patterns[(std::size_t) current];
                const auto length = (double) std::max<std::int64_t> (1, pattern.effectiveLength (song.beatsPerBar));
                const auto t = std::fmod (std::max (0.0, startTick), length);

                for (const auto& lane : state.eventLanes)
                    if (lane.pattern == current)
                        automationValues[(std::size_t) lane.slot] = pattern.events[(std::size_t) lane.lane].valueAt (t);
            }
        }
        else
        {
            const auto length = (double) std::max<std::int64_t> (1, state.schedule.songLength);
            const auto tick = std::fmod (std::max (0.0, startTick), length);

            // inside a clip of the pattern: its events; after one: the value it ended with; before the first:
            // no value (the target's own). Depends only on the position, like automation clips.
            for (const auto& lane : state.eventLanes)
            {
                const auto& events = song.patterns[(std::size_t) lane.pattern].events[(std::size_t) lane.lane];
                auto value = std::numeric_limits<float>::quiet_NaN();

                for (auto i : state.patternClips[(std::size_t) lane.pattern]) // sorted by start
                {
                    const auto& c = song.playlist[(std::size_t) i];

                    if (tick < (double) c.start)
                        break;

                    value = tick < (double) c.end() ? events.valueAt (tick - (double) c.start + (double) c.offset)
                                                    : events.valueAt ((double) (c.offset + c.length) - 1.0);
                }

                automationValues[(std::size_t) lane.slot] = value;
            }
        }
    }

    // in pattern mode the Playlist does not play: values stay as they were
    if (song.mode != PlayMode::song)
    {
        clearOverriddenValues();
        // only the plugin parameters driven by pattern events and by live controllers (links: FL's work in any mode,
        // round 165)
        if (! state.eventLanes.empty() || ! state.liveAutomations.empty())
            applyPluginParams (state, std::min<int> ((int) song.automations.size(), RackState::maxAutomations), true);
        return;
    }

    const auto length = (double) std::max<std::int64_t> (1, state.schedule.songLength);
    const auto tick = std::fmod (std::max (0.0, startTick), length);

    for (std::size_t a = 0; a < state.automationClips.size(); ++a)
        if (! state.automationClips[a].empty())
            automationValues[a] = automationValueAt (song, song.automations[a], state.automationClips[a], tick);

    clearOverriddenValues();
    applyPluginParams (state, 0);
}

void RackRenderer::applyPluginParams (const RackState& state, int firstSlot, bool andLive) noexcept
{
    for (const auto& b : state.pluginParams)
    {
        if (b.automation < firstSlot
            && ! (andLive && (std::size_t) b.automation < state.song.automations.size() && state.song.automations[(std::size_t) b.automation].isLive()))
            continue;

        const auto v = automationValues[(std::size_t) b.automation];

        if (std::isnan (v))
            continue;

        if (b.channel >= 0)
        {
            if (b.channel < (int) state.instruments.size())
                state.instruments[(std::size_t) b.channel]->setParameter (b.param, v);
            // a MIDI Out's controls reach the plugins on its port
            if ((std::size_t) b.channel < state.midiTargets.size() && midictl::isMidiParam (b.param)) // (a Dashboard's outputs stay its own)
                for (const auto t : state.midiTargets[(std::size_t) b.channel])
                    if (t < (int) state.instruments.size())
                        state.instruments[(std::size_t) t]->setParameter (b.param, v);
        }
        else if (b.track < (int) state.effects.size() && b.slot < (int) state.effects[(std::size_t) b.track].size())
            state.effects[(std::size_t) b.track][(std::size_t) b.slot]->setParameter (b.param, v);
    }
}

void RackRenderer::deliverToEffects (const RackState& state, const NoteEvent& e) noexcept
{
    if (e.channel < 0 || (std::size_t) e.channel >= state.effectMidiRoutes.size()
        || (e.type != NoteEvent::Type::on && e.type != NoteEvent::Type::off) || e.port == -2)
        return;
    if (e.type == NoteEvent::Type::on && state.song.channels[(std::size_t) e.channel].muted)
        return;
    for (const auto& route : state.effectMidiRoutes[(std::size_t) e.channel])
    {
        if ((std::size_t) route.track >= state.effects.size() || (std::size_t) route.slot >= state.effects[(std::size_t) route.track].size())
            continue;
        if (e.port >= 0 && route.port != e.port)
            continue;
        if (auto* graph = patcher::graphOf (state.effects[(std::size_t) route.track][(std::size_t) route.slot].get()))
        {
            // at the note's sample in the chunk the slot renders next (round 164f; it was the chunk's start); a slot
            // that is off renders nothing and gets nothing
            const auto& slot = state.song.mixer[(std::size_t) route.track].effects[(std::size_t) route.slot];
            if (! state.song.slotActive (slot))
                continue;
            if (e.type == NoteEvent::Type::on)
            {
                graph->queueNoteOnPort (route.port, e.key, e.velocity, route.midiChannel, e.sampleOffset);
                // a note wakes a smart-disabled slot: the instruments inside play it (a silent insert is their usual place)
                if (const auto key = (std::size_t) route.track * maxEffectSlots + (std::size_t) route.slot; key < smartDisable.size())
                    smartDisable[key].silentSamples = 0;
            }
            else
                graph->queueNoteOffPort (route.port, e.key, route.midiChannel, e.sampleOffset);
        }
    }
}

float* RackRenderer::channelBuffer (int channel, int side) noexcept
{
    return channelBuffers.data() + ((std::size_t) channel * 2 + (std::size_t) side) * chunkCapacity;
}

float* RackRenderer::trackScratch (int track, int side) noexcept
{
    return trackScratchBuffers.data() + ((std::size_t) track * 2 + (std::size_t) side) * chunkCapacity;
}

// A whole channel over the chunk: notes at the exact sample (only its own events), volume/pan and the
// PDC delay. Each channel only touches its own instrument and buffer: it can run in parallel.
void RackRenderer::renderChannel (const RackState& state, int i, int n) noexcept
{
    const auto& c = state.song.channels[(std::size_t) i];
    const auto automation = state.hasAutomation;
    const auto volume = channelGain (automation ? automated (state.channelVolumeAuto[(std::size_t) i], c.volume) : c.volume);
    const auto pan = automation ? automated (state.channelPanAuto[(std::size_t) i], (c.pan + 1.0f) * 0.5f) * 2.0f - 1.0f : c.pan;
    float gl = 0.0f, gr = 0.0f;
    panGains (pan, volume, gl, gr);

    auto* l = channelBuffer (i, 0);
    auto* r = channelBuffer (i, 1);

    if ((std::size_t) i < state.instruments.size() && state.instruments[(std::size_t) i] != nullptr)
        state.instruments[(std::size_t) i]->setSongPosition (chunkTick);

    auto& instrument = *state.instruments[(std::size_t) i];
    instrument.beginChunk (n); // (extra outputs: a generator Patcher's, REA_rodada141)
    if (c.instrument == InstrumentKind::patcher) // its Hardware Insert nodes: this chunk's device buses (round 165c)
        instrument.setHardwareIo (liveInput, hwOut != nullptr && hwOut->active.load (std::memory_order_relaxed) ? hwOut : nullptr, liveInputOffset);

    // idle channel, no notes in this chunk and no PDC: nothing to do (neither clear nor sum into the mixer)
    const auto hasDelay = (std::size_t) i < state.channelDelays.size() && state.channelDelays[(std::size_t) i] != nullptr;
    const auto hasArp = ((std::size_t) i < state.arpeggiators.size() && state.arpeggiators[(std::size_t) i] != nullptr)
                        || ((std::size_t) i < state.echoes.size() && state.echoes[(std::size_t) i] != nullptr);
    const auto* sources = state.hasMidiRoutes && (std::size_t) i < state.midiSources.size() && ! state.midiSources[(std::size_t) i].empty()
                              ? &state.midiSources[(std::size_t) i] : nullptr;
    const auto routed = [sources] (int channel) {
        return sources != nullptr && std::find (sources->begin(), sources->end(), channel) != sources->end();
    };
    if (! hasDelay && ! hasArp && instrument.idle()
        && std::none_of (events.begin(), events.end(), [&] (const NoteEvent& e) { return e.channel == i || routed (e.channel); }))
    {
        channelSilent[(std::size_t) i] = 1;
        return;
    }
    channelSilent[(std::size_t) i] = 0;
    const UnitTimer timer (profiling.load (std::memory_order_relaxed), units->channelNs[(std::size_t) i], &units->channelMaxNs[(std::size_t) i]);
    std::memset (l, 0, sizeof (float) * (std::size_t) n);
    std::memset (r, 0, sizeof (float) * (std::size_t) n);
    int position = 0;

    // arpeggiator: the channel's notes become the arpeggio sequence
    const NoteEvent* list = events.begin();
    int listCount = events.size();

    if ((std::size_t) i < state.arpeggiators.size() && state.arpeggiators[(std::size_t) i] != nullptr)
    {
        auto& arp = *state.arpeggiators[(std::size_t) i];
        int inCount = 0;

        for (const auto& e : events)
            if (e.channel == i && inCount < Arpeggiator::maxEvents)
                arp.input[(std::size_t) inCount++] = e;

        int outCount = 0;
        arp.process (i, arp.input.data(), inCount, chunkStartTick, chunkTps, n, c.arp, arp.output.data(), outCount);
        // FL cuts an arpeggio note at its gate even on a sample that plays out (REA round 146b [EXEC: FL's renders of
        // its Sampler render test]): the arpeggio is one voice, so its "off" stops the channel's sound (not with the
        // echo delay, whose echoes keep ringing)
        if (! c.misc.echo.enabled())
            for (int k = 0; k < outCount; ++k)
                if (arp.output[(std::size_t) k].type == NoteEvent::Type::off)
                    arp.output[(std::size_t) k].type = NoteEvent::Type::choke;
        list = arp.output.data();
        listCount = outCount;
    }

    // FL's echo delay (Misc tab): the channel's notes (after the arpeggio) and their echoes
    const float* gains = nullptr;
    if ((std::size_t) i < state.echoes.size() && state.echoes[(std::size_t) i] != nullptr)
    {
        auto& echo = *state.echoes[(std::size_t) i];
        int inCount = 0;
        for (int k = 0; k < listCount; ++k)
            if (list[k].channel == i && inCount < channelmisc::NoteEcho::maxEvents)
                echo.input[(std::size_t) inCount++] = list[k];
        int outCount = 0;
        echo.process (i, echo.input.data(), inCount, chunkStartTick, chunkTps, n, c.misc.echo, echo.output.data(), echo.outputGains.data(), outCount);
        list = echo.output.data();
        listCount = outCount;
        gains = echo.outputGains.data();
    }
    const auto tracking = c.misc.key.pan != 0.0f || c.misc.velocity.pan != 0.0f;
    const auto modTracking = c.misc.key.modX != 0.0f || c.misc.key.modY != 0.0f || c.misc.velocity.modX != 0.0f || c.misc.velocity.modY != 0.0f;
    // FL's Add to key (round 149): the root note moves the notes of instruments that are not samplers
    const auto keyShift = c.misc.addToKey && c.instrument != InstrumentKind::sampler ? 60 - c.misc.rootNote : 0;

    for (int index = 0; index < listCount; ++index)
    {
        auto shifted = list[index];
        if (keyShift != 0 && shifted.channel == i)
            shifted.key = std::clamp (shifted.key + keyShift, 0, 127);
        const auto& e = shifted;

        if (e.channel != i)
        {
            // a note of a MIDI Out channel on this plugin's MIDI input port, with that MIDI Out's MIDI channel
            if (sources == nullptr || ! routed (e.channel) || (e.type != NoteEvent::Type::on && e.type != NoteEvent::Type::off))
                continue;
            if (e.port == -2) // (a VFX channel's own note: its processor's output goes out instead)
                continue;
            if (e.sampleOffset > position)
            {
                instrument.render (l + position, r + position, e.sampleOffset - position, gl, gr);
                position = e.sampleOffset;
            }
            // (each route from that channel: its port and MIDI channel, round 162)
            for (const auto& route : state.midiRoutesIn[(std::size_t) i])
            {
                if (route.other != e.channel || (e.port >= 0 && route.port != e.port))
                    continue;
                // (a VFX channel's voice keeps its colour as the MIDI channel)
                const auto midiChannel = e.port >= 0 ? (int) e.colour : route.midiChannel;
                if (e.type == NoteEvent::Type::on)
                {
                    if (! c.muted && ! state.song.channels[(std::size_t) e.channel].muted)
                    {
                        if (e.port >= 0)
                        {
                            instrument.setNextNoteExpression (e.pan, e.cents, e.porta);
                            instrument.setNextNoteModulation (e.modX, e.modY);
                            instrument.setNextNoteColour (e.colour);
                        }
                        instrument.noteOnPort (route.port, e.key, e.velocity, midiChannel);
                    }
                }
                else
                {
                    if (e.port >= 0)
                        instrument.setNextNoteRelease (e.release);
                    instrument.noteOffPort (route.port, e.key, midiChannel);
                }
            }
            continue;
        }

        if (e.sampleOffset > position)
        {
            instrument.render (l + position, r + position, e.sampleOffset - position, gl, gr);
            position = e.sampleOffset;
        }

        auto& held = heldNotes[(std::size_t) i];
        const auto bit = std::uint64_t { 1 } << (e.key & 63);
        if (e.type == NoteEvent::Type::on)
        {
            if (e.key >= 0 && e.key < 128)
                held[(std::size_t) (e.key >> 6)] |= bit;
            // FL's key region (round 164): the channel plays only the notes inside it (audio clips: the key is the clip)
            const auto inRegion = c.instrument == InstrumentKind::audioClip || c.inKeyRegion (list[index].key);
            if (! c.muted && inRegion)
            {
                // Max polyphony: the oldest note is released first
                if (const auto limit = voiceLimitFor (c.maxPolyphony, c.synth.mono); limit > 0)
                    if (const auto victim = voiceOrders[(std::size_t) i].noteOn (e.key, limit); victim >= 0)
                        instrument.noteOff (victim);
                const auto pan = tracking ? channelmisc::trackedPan (c.misc, e.key, e.velocity, e.pan) : e.pan;
                // (an audio clip follows its fine tune, not the main pitch: LYNX never moved clips with it)
                instrument.setNextNoteExpression (pan, e.cents + c.misc.fineTune
                                                           + (c.misc.mainPitch && c.instrument != InstrumentKind::audioClip ? state.song.masterPitch : 0.0f),
                                                  e.porta);
                {
                    const auto [modX, modY] = modTracking ? channelmisc::trackedModulation (c.misc, e.key, e.velocity, e.modX, e.modY)
                                                          : std::pair<float, float> { e.modX, e.modY };
                    instrument.setNextNoteModulation (modX, modY);
                }
                instrument.setNextNoteColour (e.colour);

                if (e.fadeIn > 0.0f || e.fadeOut > 0.0f)
                    instrument.setNextClipFades (e.clipElapsed, e.clipRemaining, e.fadeIn, e.fadeOut);

                // an echo's volume: the instrument scales the voice, else (engines without it) the velocity
                auto velocity = e.velocity;
                const auto gain = e.gain * (gains != nullptr ? gains[index] : 1.0f);
                if (gain != 1.0f && ! instrument.setNextNoteGain (gain))
                    velocity *= gain;
                instrument.noteOnAt (e.key, velocity, e.startSeconds);
            }
        }
        else if (e.type == NoteEvent::Type::slide)
        {
            instrument.slideTo (e.key, e.cents + c.misc.fineTune + (c.misc.mainPitch ? state.song.masterPitch : 0.0f), e.slideSamples);
        }
        else if (e.type == NoteEvent::Type::choke)
        {
            instrument.allNotesOff(); // short fade, like FL's "cut"
            held = {};
            voiceOrders[(std::size_t) i].clear();
        }
        else
        {
            instrument.setNextNoteRelease (e.release);
            instrument.noteOff (e.key);
            voiceOrders[(std::size_t) i].noteOff (e.key);
            if (e.key >= 0 && e.key < 128)
                held[(std::size_t) (e.key >> 6)] &= ~bit;
        }
    }

    if (position < n)
        instrument.render (l + position, r + position, n - position, gl, gr);

    // PDC: this channel arrives before the track's others; delay until aligned
    if ((std::size_t) i < state.channelDelays.size() && state.channelDelays[(std::size_t) i] != nullptr)
        state.channelDelays[(std::size_t) i]->process (l, r, n);
}

void RackRenderer::applyTrackEq (const MixerTrack& track, int t, bool bypassed, float* l, float* r, int n) noexcept
{
    if ((std::size_t) t >= trackEqs.size())
        return;

    auto& eq = trackEqs[(std::size_t) t];

    if (bypassed || ! track.eq.active()) // clean mixer (round 163): no built-in processing, the samples pass untouched
    {
        if (eq.running)
            eq = {}; // off: reset the filters so they don't come back with a stale tail
        return;
    }

    if (! eq.running || ! (eq.settings == track.eq))
    {
        // new coefficients, keeping the filter state (no click when adjusting)
        const auto& s = track.eq;
        eq.bands[0].shelf (sampleRate, std::min ((double) s.hz[0], sampleRate * 0.45), s.gainDb[0], false, TrackEq::shelfSlope (s.width[0]));
        eq.bands[1].peaking (sampleRate, std::min ((double) s.hz[1], sampleRate * 0.45), TrackEq::peakQ (s.width[1]), s.gainDb[1]);
        eq.bands[2].shelf (sampleRate, std::min ((double) s.hz[2], sampleRate * 0.45), s.gainDb[2], true, TrackEq::shelfSlope (s.width[2]));

        for (std::size_t b = 0; b < 3; ++b)
            if (s.gainDb[b] == 0.0f)
                eq.bands[b].reset();

        eq.settings = s;
        eq.running = true;
    }

    for (std::size_t b = 0; b < 3; ++b)
    {
        if (eq.settings.gainDb[b] == 0.0f)
            continue; // band at 0 dB: not processed (like FL)

        eq.bands[b].processBlock (l, r, n);
    }
}

// effects whose controlOutput() an editor shows live
static bool hasLiveMeters (EffectKind kind) noexcept
{
    // round 129: the voice tools (Quell's reduction meter was never published) and the basic compressor's meters
    return mixsuite::isSuiteKind (kind) || dyn::isDynKind (kind) || kind == EffectKind::lynxTune || kind == EffectKind::facetEq
           || kind == EffectKind::apexLimiter || voicetools::isVoiceKind (kind) || kind == EffectKind::compressor
           || kind == EffectKind::echoform // round 135w: Echoform's output meter
           || kind == EffectKind::autoPan || kind == EffectKind::voiceShift // round 135: the pan position, the shifter's notes
           || kind == EffectKind::hardwareInsert // round 164d: send / return levels, the ping
           || kind == EffectKind::xyController || kind == EffectKind::xyzController; // round 164: the pad's target and outputs
}

void RackRenderer::setSidechain (const RackState& state, const EffectSlot& slot, Effect& effect, int track) noexcept
{
    // the source was already processed in this pass (earlier layer); silent = no signal in the detector
    for (const auto second : { false, true })
    {
        const auto source = second ? slot.sidechain2 : slot.sidechain;
        if (source < 0 || source == track || source >= (int) state.song.mixer.size())
            continue;
        const auto active = (std::size_t) source < trackActive.size() && trackActive[(std::size_t) source] != 0;
        const auto* left = active ? trackBuffer (source, 0) : nullptr;
        const auto* right = active ? trackBuffer (source, 1) : nullptr;
        if (second)
            effect.setSidechain2 (left, right);
        else
            effect.setSidechain (left, right);
    }
}

void RackRenderer::runEffects (const RackState& state, int track, int n) noexcept
{
    if (track >= (int) state.effects.size())
        return;

    const auto& slots = state.song.mixer[(std::size_t) track].effects;
    const auto& effects = state.effects[(std::size_t) track];

    auto* l = trackBuffer (track, 0);
    auto* r = trackBuffer (track, 1);
    int audible = -1; // -1 = not measured yet; an effect's output is the next one's input
    const auto timing = times->enabled.load (std::memory_order_relaxed);
    auto timed = [this, timing] (std::size_t key, auto&& run) {
        if (! timing || key >= times->effectNs.size())
        {
            run();
            return;
        }
        const auto before = ProcessTimes::now();
        run();
        times->effectNs[key].fetch_add (ProcessTimes::now() - before, std::memory_order_relaxed);
    };

    for (std::size_t i = 0; i < effects.size() && i < slots.size(); ++i)
    {
        // slot dry/wet (automatable)
        const auto wet = state.hasAutomation && i < state.effectMixAuto[(std::size_t) track].size()
                             ? std::clamp (automated (state.effectMixAuto[(std::size_t) track][i], slots[i].mix), 0.0f, 1.0f)
                             : slots[i].mix;

        auto clearMeters = [this, track, i] {
            const auto at = ((std::size_t) track * maxEffectSlots + i) * ProcessTimes::meterOutputs;
            if (at + ProcessTimes::meterOutputs <= times->effectMeters.size())
                for (int o = 0; o < ProcessTimes::meterOutputs; ++o)
                    times->effectMeters[at + (std::size_t) o].store (0.0f, std::memory_order_relaxed);
        };

        if (! state.song.slotActive (slots[i]) || wet <= 0.0f)
        {
            if (hasLiveMeters (slots[i].kind))
                clearMeters();
            continue;
        }

        // smart disable (like FL): an effect with silent input and output is skipped (plugins
        // after 4 s; built-ins after 1 s, which is well beyond their tail)
        const auto key = (std::size_t) track * maxEffectSlots + i;
        const auto isPlugin = slots[i].kind == EffectKind::plugin;
        const auto hold = (int) (sampleRate * (isPlugin || slots[i].kind == EffectKind::patcher ? 4.0 : 1.0)); // (a Patcher may hold plugins)

        // (not Gross Beat: its buffer must keep recording the silence it may play back later, as in FL)
        // (nor Newtone / Newtime: in slave mode they play their sample whatever the input)
        // (nor a slot whose "Smart disable" is off, or a plugin that does not allow it — FL's per-plugin options, REA round 139)
        if (key < smartDisable.size() && slots[i].smartDisable && ! (isPlugin && ! slots[i].plugin.allowSmartDisable)
            && slots[i].kind != EffectKind::grossBeat && slots[i].kind != EffectKind::newtone
            && slots[i].kind != EffectKind::newtime
            && slots[i].kind != EffectKind::hardwareInsert // (nor the hardware insert: outboard gear has its own tails and noise)
            && ! (slots[i].kind == EffectKind::patcher && patcher::usesHardwareIo (slots[i].patcher)) // (nor a Patcher holding one, round 165c)
            // (nor a controller: its outputs move whatever the sound — a silent insert is their usual place; round 164)
            && slots[i].kind != EffectKind::formulaController
            && slots[i].kind != EffectKind::xyController && slots[i].kind != EffectKind::xyzController
            && slots[i].kind != EffectKind::controlSurface
            // round 164c: a visual plugin keeps recording silence (its display falls, the video export stays in time)
            && slots[i].kind != EffectKind::waveCandy && slots[i].kind != EffectKind::visualizer
            && slots[i].kind != EffectKind::tuner) // (round 165g: the tuner's display follows the signal into silence)
        {
            auto& sd = smartDisable[key];

            if (sd.effect != effects[i].get())
                sd = { effects[i].get(), 0 };

            if (audible < 0)
                audible = anyAbove (l, r, n, silenceLevel) ? 1 : 0;

            if (audible == 1)
                sd.silentSamples = 0;
            else if (sd.silentSamples >= hold)
            {
                if (hasLiveMeters (slots[i].kind))
                    clearMeters();
                continue;
            }
            else
                sd.silentSamples += n;
        }

        // the editor's spectrum analyser listens to this slot: its input
        const auto tapped = times->analyzer.key.load (std::memory_order_relaxed) == (int) key;
        if (tapped)
        {
            auto& tap = times->analyzer;
            const auto w = tap.written.load (std::memory_order_relaxed);
            for (int k = 0; k < n; ++k)
                tap.pre[(std::size_t) ((w + (std::uint32_t) k) & (ProcessTimes::AnalyzerTap::size - 1))].store (0.5f * (l[k] + r[k]), std::memory_order_relaxed);
        }

        // dry/wet: keep the dry signal to mix later
        // (not the hardware insert: its dry path is delayed by its round trip inside it, so the slot's mix scales its own
        // dry/wet instead — round 164d)
        const auto partial = wet < 0.999f && slots[i].kind != EffectKind::hardwareInsert;

        if (partial)
        {
            std::memcpy (trackScratch (track, 0), l, sizeof (float) * (std::size_t) n);
            std::memcpy (trackScratch (track, 1), r, sizeof (float) * (std::size_t) n);
        }

        const UnitTimer timer (profiling.load (std::memory_order_relaxed), units->effectNs[std::min (key, units->effectNs.size() - 1)],
                               &units->effectMaxNs[std::min (key, units->effectNs.size() - 1)]);

        // the hardware insert (round 164d): this piece's device input and output buses (none during an export)
        if (slots[i].kind == EffectKind::hardwareInsert || slots[i].kind == EffectKind::patcher) // (a Patcher hands them to its nodes, round 165c)
            effects[i]->setHardwareIo (liveInput, hwOut != nullptr && hwOut->active.load (std::memory_order_relaxed) ? hwOut : nullptr, liveInputOffset);

        // Fruity Send: what it sends goes to its own buffer, pulled by the destination insert (processed later)
        if (slots[i].kind == EffectKind::fruitySend || slots[i].kind == EffectKind::patcher)
        {
            const auto tap = (std::size_t) track < state.slotTap.size() && i < state.slotTap[(std::size_t) track].size()
                                 ? state.slotTap[(std::size_t) track][i]
                                 : -1;
            if (tap >= 0 && tap < RackState::maxSendTaps)
            {
                std::memset (tapBuffer (tap, 0), 0, sizeof (float) * (std::size_t) n);
                std::memset (tapBuffer (tap, 1), 0, sizeof (float) * (std::size_t) n);
                effects[i]->setSendOutput (tapBuffer (tap, 0), tapBuffer (tap, 1));
                tapActive[(std::size_t) tap] = 1;
            }
            else
                effects[i]->setSendOutput (nullptr, nullptr);
        }

        if (slots[i].kind == EffectKind::hardwareInsert && wet < 0.999f)
        {
            auto params = slots[i].params; // copy without allocation
            if (state.hasAutomation)
            {
                const auto& autos = state.effectParamAuto[(std::size_t) track][i];
                const auto used = std::min (params.size(), effectInfo (slots[i].kind).params.size());
                for (std::size_t p = 0; p < used; ++p)
                    params[p] = automated (autos[p], params[p]);
            }
            params[(std::size_t) hwinsert::dryWet] *= wet;
            timed (key, [&] { effects[i]->process (l, r, n, params, bpm); });
        }
        else if (state.hasAutomation && ! isPlugin)
        {
            auto params = slots[i].params; // copy without allocation
            const auto& autos = state.effectParamAuto[(std::size_t) track][i];
            const auto used = std::min (params.size(), effectInfo (slots[i].kind).params.size());

            for (std::size_t p = 0; p < used; ++p)
                params[p] = automated (autos[p], params[p]);

            effects[i]->setSongPosition (chunkTick);
            effects[i]->setPlaying (chunkPlaying);
            effects[i]->setHeldNotes (heldNotes.data(), maxChannels);
            setSidechain (state, slots[i], *effects[i], track);
            timed (key, [&] { effects[i]->process (l, r, n, params, bpm); });
        }
        else
        {
            effects[i]->setSongPosition (chunkTick);
            effects[i]->setPlaying (chunkPlaying);
            effects[i]->setHeldNotes (heldNotes.data(), maxChannels);
            setSidechain (state, slots[i], *effects[i], track);
            timed (key, [&] { effects[i]->process (l, r, n, slots[i].params, bpm); });
        }

        // an effect that output NaN / inf (an unstable setting): this block of it is muted and its state reset, so the
        // broken values never reach the rest of the mix (they would stay in every filter and feedback path after it)
        if (! finiteBlock (l, r, n))
        {
            std::memset (l, 0, sizeof (float) * (std::size_t) n);
            std::memset (r, 0, sizeof (float) * (std::size_t) n);
            effects[i]->reset();
            nonFiniteBlocks.fetch_add (1, std::memory_order_relaxed);
        }

        if (partial)
        {
            const auto* dl = trackScratch (track, 0);
            const auto* dr = trackScratch (track, 1);

            for (int k = 0; k < n; ++k)
            {
                l[k] = dl[k] + (l[k] - dl[k]) * wet;
                r[k] = dr[k] + (r[k] - dr[k]) * wet;
            }
        }

        // ... and its output
        if (tapped)
        {
            auto& tap = times->analyzer;
            const auto w = tap.written.load (std::memory_order_relaxed);
            for (int k = 0; k < n; ++k)
                tap.post[(std::size_t) ((w + (std::uint32_t) k) & (ProcessTimes::AnalyzerTap::size - 1))].store (0.5f * (l[k] + r[k]), std::memory_order_relaxed);
            tap.sampleRate.store (sampleRate, std::memory_order_relaxed);
            tap.written.store (w + (std::uint32_t) n, std::memory_order_release);
        }

        // live meters for the editor (the mixing suite's gain reduction, LYNX Tune's pitch, Facet EQ's dynamic gains)
        if (hasLiveMeters (slots[i].kind) && key < (std::size_t) maxMixerTracks * maxEffectSlots)
            for (int o = 0; o < ProcessTimes::meterOutputs; ++o)
                times->effectMeters[key * ProcessTimes::meterOutputs + (std::size_t) o].store (effects[i]->controlOutput (o), std::memory_order_relaxed);

        // the effect changed the signal: measure again (it is the next slot's input)
        audible = anyAbove (l, r, n, silenceLevel) ? 1 : 0;

        if (audible == 1 && key < smartDisable.size())
            smartDisable[key].silentSamples = 0; // tail (reverb, delay): keep processing
    }
}

std::uint64_t RackRenderer::nonFiniteEffectBlocks() noexcept
{
    return nonFiniteBlocks.load (std::memory_order_relaxed);
}

void RackRenderer::process (const RackState* state,
                            bool playing,
                            double startTick,
                            int numSamples,
                            float* left,
                            float* right,
                            LiveEventQueue* live,
                            MeterBank* meters,
                            LiveEventQueue* moreLive) noexcept
{
    const ScopedFlushDenormals noDenormals;
    const UnitTimer timer (profiling.load (std::memory_order_relaxed), units->processNs);
    if (profiling.load (std::memory_order_relaxed))
        units->processedSamples.fetch_add ((std::uint64_t) std::max (0, numSamples), std::memory_order_relaxed);
    std::memset (left, 0, sizeof (float) * (std::size_t) numSamples);
    std::memset (right, 0, sizeof (float) * (std::size_t) numSamples);
    events.clear();

    if (state == nullptr || blockCapacity == 0)
        return;

    const auto timing = times->enabled.load (std::memory_order_relaxed);
    const auto wallStart = timing ? ProcessTimes::now() : 0;

    // A snapshot built before the device opened may have another rate. The instruments'
    // prepare() does not allocate, so it can run here.
    if (state != lastState && state->sampleRate != sampleRate)
    {
        for (auto& instrument : state->instruments)
            instrument->prepare (sampleRate);

        for (auto& track : state->effects)
            for (auto& effect : track)
                effect->prepare (sampleRate);
    }

    lastState = state; // so prepare() knows which instruments to reconfigure

    // Stopping, or jumping to another position (seek/rewind), releases held notes:
    // their "off" events will no longer happen.
    const auto jumped = playing && wasPlaying && std::abs (startTick - expectedTick) > 1.0;

    if ((wasPlaying && ! playing) || (playing && ! wasPlaying))
    {
        // the arpeggiators / echoes ran on the live clock while stopped (round 149): they restart on the song's
        liveRoutedCount = 0;
        for (auto& arp : state->arpeggiators)
            if (arp != nullptr)
                arp->reset();
        for (auto& echo : state->echoes)
            if (echo != nullptr)
                echo->reset();
    }

    if ((wasPlaying && ! playing) || jumped)
    {
        for (auto& instrument : state->instruments)
            if (jumped)
                instrument->allNotesOff();
            else
                instrument->transportStopped(); // (a hosted plugin: FL's "Send All Notes Off when playback stops")
        heldNotes.fill ({});

        for (auto& arp : state->arpeggiators)
            if (arp != nullptr)
                arp->reset();
        for (auto& echo : state->echoes)
            if (echo != nullptr)
                echo->reset();
    }

    // FL's MIDI Out sends its bank and program when playback starts (REA_rodada16 §1.3)
    if (playing && ! wasPlaying && state->hasMidiRoutes)
        for (std::size_t c = 0; c < state->midiTargets.size(); ++c)
        {
            if (state->midiTargets[c].empty() || state->song.channels[c].instrument != InstrumentKind::midiOut) // (a Dashboard sends its own)
                continue;
            const auto s = midictl::parseMidiOut (state->song.channels[c].pluginState);
            const auto status = (std::uint8_t) (s.channel & 15);
            for (const auto t : state->midiTargets[c])
            {
                auto& target = *state->instruments[(std::size_t) t];
                if (s.bankMsb >= 0)
                {
                    const std::uint8_t m[3] { (std::uint8_t) (0xB0 | status), 0, (std::uint8_t) s.bankMsb };
                    target.midiMessage (m, 3);
                }
                if (s.bankLsb >= 0)
                {
                    const std::uint8_t m[3] { (std::uint8_t) (0xB0 | status), 32, (std::uint8_t) s.bankLsb };
                    target.midiMessage (m, 3);
                }
                if (s.program > 0)
                {
                    const std::uint8_t m[2] { (std::uint8_t) (0xC0 | status), (std::uint8_t) (s.program - 1) };
                    target.midiMessage (m, 2);
                }
            }
        }

    // started playing or jumped: audio clips in progress must start mid-way
    resumeAudio = playing && (! wasPlaying || jumped);
    wasPlaying = playing;
    // tempo automation: the tempo holds for the whole block (evaluated at its start)
    bpm = tempoOverride > 0.0 ? tempoOverride : state->song.bpm;

    if (tempoOverride <= 0.0 && state->tempoAutomation >= 0 && state->song.mode == PlayMode::song && ! isSlotOverridden (state->tempoAutomation))
    {
        const auto length = (double) std::max<std::int64_t> (1, state->schedule.songLength);
        const auto& clips = state->automationClips[(std::size_t) state->tempoAutomation];

        if (! clips.empty())
            bpm = tempoFromValue (automationValueAt (state->song, state->song.automations[(std::size_t) state->tempoAutomation], clips,
                                                     std::fmod (std::max (0.0, startTick), length)));
    }

    const auto tps = ticksPerSample (bpm, sampleRate);
    expectedTick = startTick + numSamples * tps;

    for (auto* queue : { live, moreLive })
    {
        if (queue == nullptr)
            continue;

        NoteEvent e;

        while (queue->pop (e))
        {
            if (e.type == NoteEvent::Type::choke && e.channel < 0) // panic: every channel stops sounding
            {
                for (auto& instrument : state->instruments)
                    if (instrument != nullptr)
                        instrument->panic();
                heldNotes.fill ({});
                for (auto& order : voiceOrders)
                    order.clear();
                continue;
            }

            if (e.channel < 0 || e.channel >= (int) state->instruments.size())
                continue;

            if (e.type == NoteEvent::Type::control)
            {
                applyLiveControl (*state, e);
                continue;
            }

            auto& instrument = *state->instruments[(std::size_t) e.channel];

            auto& held = heldNotes[(std::size_t) std::min (e.channel, maxChannels - 1)];
            const auto bit = std::uint64_t { 1 } << (e.key & 63);
            const auto inRange = e.key >= 0 && e.key < 128;
            // played on a MIDI Out channel: the plugins on its port get the note with its MIDI channel
            if (state->hasMidiRoutes && (std::size_t) e.channel < state->midiRoutesOut.size()
                && ((std::size_t) e.channel >= state->vfxPort.size() || state->vfxPort[(std::size_t) e.channel] < 0))
                for (const auto& route : state->midiRoutesOut[(std::size_t) e.channel])
                    if (route.other < (int) state->instruments.size() && state->instruments[(std::size_t) route.other] != nullptr)
                    {
                        auto& target = *state->instruments[(std::size_t) route.other];
                        if (e.type == NoteEvent::Type::on)
                            target.noteOnPort (route.port, e.key, e.velocity, route.midiChannel);
                        else if (e.type == NoteEvent::Type::off)
                            target.noteOffPort (route.port, e.key, route.midiChannel);
                    }
            // and the mixer-slot Patchers on its port (round 162)
            // (notes that go through the channel's note path reach them in processChunk)
            if (state->hasEffectMidiRoutes && ! routesLiveNotes (*state, e.channel))
                deliverToEffects (*state, e);
            // the channel's arpeggiator / echo delay / tracking play it (in processChunk's first chunk)
            if ((e.type == NoteEvent::Type::on || e.type == NoteEvent::Type::off) && routesLiveNotes (*state, e.channel))
            {
                if (liveRoutedCount < (int) liveRouted.size())
                {
                    e.sampleOffset = 0;
                    liveRouted[(std::size_t) liveRoutedCount++] = e;
                }
                continue;
            }
            auto& order = voiceOrders[(std::size_t) std::min (e.channel, maxChannels - 1)];
            if (e.type == NoteEvent::Type::on && (std::size_t) e.channel < state->song.channels.size()
                && ! state->song.channels[(std::size_t) e.channel].inKeyRegion (e.key))
                continue; // outside the channel's key region (round 164)
            if (e.type == NoteEvent::Type::on)
            {
                const auto& def = state->song.channels;
                if ((std::size_t) e.channel < def.size())
                    if (const auto limit = voiceLimitFor (def[(std::size_t) e.channel].maxPolyphony, def[(std::size_t) e.channel].synth.mono); limit > 0)
                        if (const auto victim = order.noteOn (e.key, limit); victim >= 0)
                            instrument.noteOff (victim);
                // a played note is tuned like the song's notes: Misc › Fine tune and the main pitch (round 162, issue
                // #9: the fine tune did nothing to the keys played live)
                if ((std::size_t) e.channel < def.size())
                {
                    const auto& misc = def[(std::size_t) e.channel].misc;
                    const auto cents = misc.fineTune + (misc.mainPitch ? state->song.masterPitch : 0.0f);
                    if (cents != 0.0f)
                        instrument.setNextNoteExpression (0.0f, cents, false);
                }
                instrument.noteOn (e.key, e.velocity);
                if (inRange)
                    held[(std::size_t) (e.key >> 6)] |= bit;
            }
            else
            {
                order.noteOff (e.key);
                instrument.noteOff (e.key);
                if (inRange)
                    held[(std::size_t) (e.key >> 6)] &= ~bit;
            }
        }
    }

    // Blocks larger than the mixer buffers are done in pieces. With automation, small
    // pieces so values move smoothly.
    const auto chunk = std::min (blockCapacity, state->hasAutomation ? 128 : chunkCapacity);

    for (int done = 0; done < numSamples;)
    {
        auto n = std::min (chunk, numSamples - done);

        // pattern events take effect at their own sample, as in FL (REA_rodada85 §3): a chunk ends where one falls
        if (playing && ! state->eventLanes.empty() && tps > 0.0)
        {
            const auto ahead = ticksToNextEvent (*state, startTick + done * tps);
            if (ahead < (double) n * tps)
                n = std::max (1, (int) std::ceil (ahead / tps - 1.0e-9));
        }
        liveInputOffset = done;
        processChunk (*state, playing, startTick + done * tps, n, left + done, right + done, meters);
        resumeAudio = false;
        done += n;
    }

    if (timing)
    {
        times->wallNs.fetch_add (ProcessTimes::now() - wallStart, std::memory_order_relaxed);
        times->audioNs.fetch_add ((std::uint64_t) ((double) numSamples / sampleRate * 1.0e9), std::memory_order_relaxed);
    }
}

double RackRenderer::ticksToNextEvent (const RackState& state, double tick) noexcept
{
    const auto& song = state.song;
    auto best = std::numeric_limits<double>::infinity();
    // first event tick of `events` strictly after `t` (events sorted by tick)
    auto after = [] (const PatternEvents& events, double t) {
        auto it = std::upper_bound (events.events.begin(), events.events.end(), t,
                                    [] (double x, const PatternEvent& e) { return x < (double) e.tick; });
        return it == events.events.end() ? std::numeric_limits<double>::infinity() : (double) it->tick;
    };

    if (song.mode != PlayMode::song)
    {
        const auto current = song.currentPattern;
        if (current < 0 || current >= (int) song.patterns.size())
            return best;
        const auto& pattern = song.patterns[(std::size_t) current];
        const auto length = (double) std::max<std::int64_t> (1, pattern.effectiveLength (song.beatsPerBar));
        const auto t = std::fmod (std::max (0.0, tick), length);
        for (const auto& lane : state.eventLanes)
            if (lane.pattern == current)
            {
                const auto& events = pattern.events[(std::size_t) lane.lane];
                best = std::min (best, after (events, t) - t);
                if (! events.events.empty()) // the loop restarts at the first event's value
                    best = std::min (best, length - t);
            }
        return best;
    }

    const auto length = (double) std::max<std::int64_t> (1, state.schedule.songLength);
    const auto t = std::fmod (std::max (0.0, tick), length);

    for (const auto& lane : state.eventLanes)
    {
        const auto& events = song.patterns[(std::size_t) lane.pattern].events[(std::size_t) lane.lane];
        for (auto i : state.patternClips[(std::size_t) lane.pattern])
        {
            const auto& c = song.playlist[(std::size_t) i];
            if ((double) c.end() <= t)
                continue;
            if ((double) c.start > t)
            {
                best = std::min (best, (double) c.start - t);
                break;
            }
            const auto next = after (events, t - (double) c.start + (double) c.offset) - (double) c.offset + (double) c.start;
            best = std::min (best, std::min (next, (double) c.end()) - t);
        }
    }
    return best;
}

void RackRenderer::applyLiveControl (const RackState& state, const NoteEvent& e) noexcept
{
    // a controller's pitch wheel, mod wheel (CC 1), other CCs and channel pressure played into a channel (round 162,
    // issue #9): hosted plugins (and the SoundFont player) get the MIDI message; LYNX's own instruments bend by the
    // channel's pitch range (FL's Misc › Pitch range, default ±2 semitones) and take the CCs they know
    auto* instrument = state.instruments[(std::size_t) e.channel].get();
    if (instrument == nullptr)
        return;

    std::uint8_t m[3] {};
    auto size = 3;
    const auto value = std::isfinite (e.velocity) ? e.velocity : 0.0f;
    if (e.key == NoteEvent::controlPitchBend)
    {
        const auto v = (int) std::clamp (std::lround ((double) (value + 1.0f) * 8192.0), 0L, 16383L);
        m[0] = 0xE0, m[1] = (std::uint8_t) (v & 127), m[2] = (std::uint8_t) (v >> 7);
    }
    else if (e.key == NoteEvent::controlPressure)
    {
        m[0] = 0xD0, m[1] = (std::uint8_t) std::clamp (std::lround (value * 127.0f), 0L, 127L);
        size = 2;
    }
    else if (e.key >= 0 && e.key < 128)
        m[0] = 0xB0, m[1] = (std::uint8_t) e.key, m[2] = (std::uint8_t) std::clamp (std::lround (value * 127.0f), 0L, 127L);
    else
        return;

    instrument->midiMessage (m, size);
    if (e.key == NoteEvent::controlPitchBend)
    {
        const auto range = (std::size_t) e.channel < state.song.channels.size() ? state.song.channels[(std::size_t) e.channel].misc.pitchRange : 2;
        instrument->pitchBend (std::clamp (value, -1.0f, 1.0f) * (float) std::max (0, range));
    }
    else if (e.key < 128)
        instrument->controlChange (e.key, std::clamp (value, 0.0f, 1.0f));

    // a MIDI Out channel: the plugins on its port get it on its MIDI channel
    if (state.hasMidiRoutes && (std::size_t) e.channel < state.midiTargets.size())
        for (const auto t : state.midiTargets[(std::size_t) e.channel])
            if (t >= 0 && t < (int) state.instruments.size() && state.instruments[(std::size_t) t] != nullptr)
            {
                std::uint8_t routed[3] { (std::uint8_t) ((m[0] & 0xF0) | (state.midiOutChannel[(std::size_t) e.channel] & 15)), m[1], m[2] };
                state.instruments[(std::size_t) t]->midiMessage (routed, size);
            }
}

void RackRenderer::expandLayers (const RackState& state) noexcept
{
    const auto& channels = state.song.channels;
    const auto count = events.size();
    auto added = false;
    for (int i = 0; i < count; ++i)
    {
        const auto e = events[i];
        if (e.channel < 0 || e.channel >= (int) channels.size() || e.channel >= maxChannels)
            continue;
        const auto& l = channels[(std::size_t) e.channel];
        if (l.instrument != InstrumentKind::layer || l.layer.children.empty())
            continue;
        const auto key = std::clamp (e.key, 0, 127);
        auto* picked = layerPick.empty() ? nullptr : &layerPick[(std::size_t) e.channel * 128 + (std::size_t) key];
        auto child = e;
        child.pan = std::clamp (e.pan + l.pan, -1.0f, 1.0f);
        child.cents = e.cents + layer::pitchCents (l);
        const auto one = l.layer.random || l.layer.sequential;

        if (e.type == NoteEvent::Type::on)
        {
            if (l.muted)
                continue;
            std::array<layer::Pick, 64> picks;
            const auto n = layer::pick (l, channels, key, layerRandom, layerTurn[(std::size_t) e.channel], picks.data(), (int) picks.size());
            if (one && picked != nullptr)
                *picked = (std::int16_t) (n > 0 ? picks[0].child : -1);
            const auto level = layer::levelGain (l);
            for (int k = 0; k < n; ++k)
            {
                child.channel = picks[(std::size_t) k].child;
                child.gain = e.gain * level * picks[(std::size_t) k].gain;
                events.add (child);
                added = true;
            }
        }
        else if (e.type == NoteEvent::Type::off && one)
        {
            if (picked != nullptr && *picked >= 0)
            {
                child.channel = *picked;
                *picked = -1;
                events.add (child);
                added = true;
            }
        }
        else if (e.type == NoteEvent::Type::off || e.type == NoteEvent::Type::slide || e.type == NoteEvent::Type::choke)
            for (const auto c : l.layer.children)
                if (c >= 0 && c < (int) channels.size() && c != e.channel && channels[(std::size_t) c].instrument != InstrumentKind::layer)
                {
                    child.channel = c;
                    events.add (child);
                    added = true;
                }
    }
    if (added)
        events.sortByOffset();
}

bool RackRenderer::routesLiveNotes (const RackState& state, int channel) const noexcept
{
    const auto c = (std::size_t) channel;
    if (channel < 0 || c >= state.song.channels.size())
        return false;
    if (state.song.channels[c].instrument == InstrumentKind::layer) // (its children play the note: round 164)
        return true;
    const auto& misc = state.song.channels[c].misc;
    return (c < state.arpeggiators.size() && state.arpeggiators[c] != nullptr) || (c < state.echoes.size() && state.echoes[c] != nullptr)
           || misc.key.pan != 0.0f || misc.velocity.pan != 0.0f
           || (misc.addToKey && misc.rootNote != 60 && state.song.channels[c].instrument != InstrumentKind::sampler)
           || (c < state.vfxPort.size() && state.vfxPort[c] >= 0); // (a VFX channel's processor takes them in the chunk)
}

void RackRenderer::runVfxChannels (const RackState& state, bool playing, int numSamples) noexcept
{
    vfx::Transport transport;
    transport.sampleRate = sampleRate;
    transport.bpm = chunkTps * sampleRate * 60.0 / (double) ppq; // (the tempo at this chunk)
    transport.ticks = chunkStartTick; // (stopped: the live clock, as the arpeggiator)
    transport.playing = playing;
    for (const auto c : state.vfxChannels)
    {
        if ((std::size_t) c >= state.instruments.size() || state.instruments[(std::size_t) c] == nullptr)
            continue;
        auto* processor = state.instruments[(std::size_t) c]->noteProcessor();
        if (processor == nullptr)
            continue;
        vfxIn->clear();
        vfxOut->clear();
        const auto count = events.size();
        for (int k = 0; k < count; ++k)
        {
            auto& e = events.at (k);
            if (e.channel != c || e.port != -1)
                continue;
            if (e.type == NoteEvent::Type::on || e.type == NoteEvent::Type::off)
            {
                vfx::Event in;
                in.type = e.type == NoteEvent::Type::on ? vfx::Event::Type::on : vfx::Event::Type::off;
                in.offset = e.sampleOffset;
                in.voice.key = e.key;
                in.voice.velocity = e.velocity;
                in.voice.pan = e.pan;
                in.voice.cents = e.cents;
                in.voice.modX = e.modX;
                in.voice.modY = e.modY;
                in.voice.release = e.release;
                in.voice.colour = e.colour & 15;
                in.voice.porta = e.porta;
                in.id = vfx::inputId (e.key, in.voice.colour);
                vfxIn->add (in);
            }
            e.port = -2; // (taken by the processor)
        }
        processor->process (vfxIn->size() > 0 ? &(*vfxIn)[0] : nullptr, vfxIn->size(), numSamples, transport, *vfxOut);
        const auto port = state.vfxPort[(std::size_t) c];
        for (int k = 0; k < vfxOut->size(); ++k)
        {
            const auto& o = (*vfxOut)[k];
            if (o.type == vfx::Event::Type::prop || port + o.output >= patchMidiPorts)
                continue; // (voice properties over time reach the instruments inside a Patcher only)
            NoteEvent e;
            e.type = o.type == vfx::Event::Type::on ? NoteEvent::Type::on : NoteEvent::Type::off;
            e.channel = c;
            e.sampleOffset = std::clamp (o.offset, 0, std::max (0, numSamples - 1));
            e.key = o.voice.key;
            e.velocity = o.voice.velocity;
            e.pan = o.voice.pan;
            e.cents = o.voice.cents;
            e.modX = o.voice.modX;
            e.modY = o.voice.modY;
            e.release = o.voice.release;
            e.colour = (std::int8_t) (o.voice.colour & 15);
            e.porta = o.voice.porta;
            e.port = (std::int16_t) (port + o.output);
            events.add (e);
        }
    }
    events.sortByOffset();
}

void RackRenderer::processChunk (const RackState& state, bool playing, double startTick, int numSamples,
                                 float* left, float* right, MeterBank* meters) noexcept
{
    chunkTick = startTick;
    chunkPlaying = playing;
    const auto& song = state.song;
    const auto tracks = std::max<int> (1, (int) song.mixer.size());

    if (state.hasAutomation) // (a hair later: a chunk that starts on a pattern event's sample takes its value)
        updateAutomation (state, startTick + 1.0e-6);

    chunkTps = ticksPerSample (bpm, sampleRate);
    chunkStartTick = playing ? startTick : liveTick; // stopped: live notes' arpeggios run on their own clock
    if (! playing)
        liveTick += numSamples * chunkTps;

    // hosted plugins read the transport from here (PluginHost's play head)
    {
        auto& timeline = hostTimeline();
        timeline.tick.store (hostPositionTick (song, startTick, state.schedule.songLength), std::memory_order_relaxed);
        timeline.bpm.store (bpm, std::memory_order_relaxed);
        timeline.sampleRate.store (sampleRate, std::memory_order_relaxed);
        timeline.beatsPerBar.store (std::max (1, song.beatsPerBar), std::memory_order_relaxed);
        timeline.playing.store (playing, std::memory_order_relaxed);
        timeline.looping.store (song.hasLoop(), std::memory_order_relaxed);
        timeline.loopStart.store ((double) song.loopStart, std::memory_order_relaxed);
        timeline.loopEnd.store ((double) song.loopEnd, std::memory_order_relaxed);
    }

    for (int t = 0; t < tracks; ++t)
    {
        std::memset (trackBuffer (t, 0), 0, sizeof (float) * (std::size_t) numSamples);
        std::memset (trackBuffer (t, 1), 0, sizeof (float) * (std::size_t) numSamples);
    }

    events.clear();

    if (playing && song.mode == PlayMode::song)
    {
        scheduleSong (song, startTick, numSamples, ticksPerSample (bpm, sampleRate), events, resumeAudio, &state.schedule,
                      liveInput == nullptr || ! liveInput->linearSong.load (std::memory_order_relaxed));
    }
    else if (playing && ! song.patterns.empty())
    {
        const auto patternIndex = std::clamp (song.currentPattern, 0, (int) song.patterns.size() - 1);
        const auto& pattern = song.patterns[(std::size_t) patternIndex];
        schedulePatternLoop (pattern, pattern.effectiveLength (song.beatsPerBar), startTick, numSamples,
                             ticksPerSample (bpm, sampleRate), events, song.swing, &song.channels);
    }

    // FL's wrapper "Output port" (round 149): the notes a plugin sent out in the previous chunk play the plugins on
    // that port, at this chunk's start
    if (state.hasMidiOutputs)
    {
        std::array<Instrument::MidiOutNote, 128> notes;
        for (std::size_t c = 0; c < state.midiOutTargets.size() && c < state.instruments.size(); ++c)
        {
            if (state.midiOutTargets[c].empty())
                continue;
            const auto count = state.instruments[c]->takeMidiOutput (notes.data(), (int) notes.size());
            for (int k = 0; k < count; ++k)
                for (const auto t : state.midiOutTargets[c])
                {
                    NoteEvent e;
                    e.type = notes[(std::size_t) k].velocity > 0.0f ? NoteEvent::Type::on : NoteEvent::Type::off;
                    e.channel = t;
                    e.key = notes[(std::size_t) k].key;
                    e.velocity = notes[(std::size_t) k].velocity;
                    e.sampleOffset = 0;
                    events.add (e);
                }
        }
        events.sortByOffset();
    }

    // Dashboards (round 165, FL's Dashboard): the MIDI their controls and patch send reaches the plugins on their port
    for (const auto c : state.dashboardChannels)
    {
        if ((std::size_t) c >= state.instruments.size() || state.instruments[(std::size_t) c] == nullptr)
            continue;
        std::array<std::uint32_t, 192> sent;
        const auto count = state.instruments[(std::size_t) c]->takeMidiMessages (sent.data(), (int) sent.size());
        if (count <= 0 || (std::size_t) c >= state.midiTargets.size())
            continue;
        for (int k = 0; k < count; ++k)
        {
            const auto m = sent[(std::size_t) k];
            const std::uint8_t bytes[3] { (std::uint8_t) (m & 0xff), (std::uint8_t) ((m >> 8) & 127), (std::uint8_t) ((m >> 16) & 127) };
            const auto size = (bytes[0] & 0xE0) == 0xC0 ? 2 : 3; // (program change, channel pressure: one data byte)
            for (const auto t : state.midiTargets[(std::size_t) c])
                if (t >= 0 && t < (int) state.instruments.size() && state.instruments[(std::size_t) t] != nullptr)
                    state.instruments[(std::size_t) t]->midiMessage (bytes, size);
        }
    }

    // live-played notes routed through their channel's note path (round 149)
    if (liveRoutedCount > 0)
    {
        for (int k = 0; k < liveRoutedCount; ++k)
            events.add (liveRouted[(std::size_t) k]);
        liveRoutedCount = 0;
        events.sortByOffset();
    }

    // Layer channels play their children (round 164)
    if (state.hasLayers)
        expandLayers (state);
    // VFX channels (round 164b): their processors turn their notes into what they send on their ports
    if (! state.vfxChannels.empty())
        runVfxChannels (state, playing, numSamples);

    // cut groups: each note of a channel "cuts" the channels linked to it, at the same sample
    if (state.hasCutGroups)
    {
        const auto count = events.size();

        for (int i = 0; i < count; ++i)
        {
            const auto e = events[i];

            if (e.type != NoteEvent::Type::on || e.channel < 0 || e.channel >= (int) state.cutTargets.size())
                continue;

            for (auto target : state.cutTargets[(std::size_t) e.channel])
            {
                NoteEvent choke;
                choke.type = NoteEvent::Type::choke;
                choke.channel = target;
                choke.sampleOffset = e.sampleOffset;
                events.add (choke);
            }
        }

        events.sortByOffset();
    }

    // MIDI to the mixer-slot Patchers that take a MIDI port (round 162): at its sample (round 164f)
    if (state.hasEffectMidiRoutes)
        for (int k = 0; k < events.size(); ++k)
            deliverToEffects (state, events[k]);

    // 1) channels, each with its own notes at the exact sample (in parallel when worth it)
    const auto channels = std::min<int> ({ (int) state.instruments.size(), (int) song.channels.size(), maxChannels });
    auto channel = [this, &state, numSamples] (int c) {
        if (! times->enabled.load (std::memory_order_relaxed) || c >= ProcessTimes::channels)
        {
            renderChannel (state, c, numSamples);
            return;
        }
        const auto before = ProcessTimes::now();
        renderChannel (state, c, numSamples);
        times->channelNs[(std::size_t) c].fetch_add (ProcessTimes::now() - before, std::memory_order_relaxed);
    };

    // the task graph (round 165i): channels and inserts by readiness; else the channel pass, then the inserts by layers
    if (! runTaskGraph (state, playing, channels, numSamples, meters))
    {
        if (workers != nullptr && state.parallel && state.audioThreadChannels.empty())
            workers->parallelFor (channels, channel);
        else if (workers != nullptr && state.parallel)
        {
            // "Allow threaded processing" off: those channels on this thread, the rest in parallel
            auto threaded = [&state, &channel, channels] (int i) {
                if (const auto c = state.threadedChannels[(std::size_t) i]; c < channels)
                    channel (c);
            };
            workers->parallelFor ((int) state.threadedChannels.size(), threaded);
            for (const auto c : state.audioThreadChannels)
                if (c < channels)
                    channel (c);
        }
        else
            for (int c = 0; c < channels; ++c)
                channel (c);

        for (int c = 0; c < channels; ++c)
        {
            if (channelSilent[(std::size_t) c])
                continue;
            const auto t = std::clamp (song.channels[(std::size_t) c].mixerTrack, 0, tracks - 1);
            auto* dl = trackBuffer (t, 0);
            auto* dr = trackBuffer (t, 1);
            const auto* sl = channelBuffer (c, 0);
            const auto* sr = channelBuffer (c, 1);

            for (int k = 0; k < numSamples; ++k)
            {
                dl[k] += sl[k];
                dr[k] += sr[k];
            }

            // extra outputs to other inserts (a generator Patcher's, REA_rodada141)
            if ((std::size_t) c < state.instruments.size() && state.instruments[(std::size_t) c] != nullptr)
            {
                const auto& instrument = *state.instruments[(std::size_t) c];
                for (int k = 0; k < instrument.auxOutputCount(); ++k)
                {
                    const auto target = instrument.auxOutputTrack (k);
                    const auto* al = instrument.auxOutputData (k, 0);
                    const auto* ar = instrument.auxOutputData (k, 1);
                    if (target < 0 || target >= tracks || al == nullptr || ar == nullptr)
                        continue;
                    auto* tl = trackBuffer (target, 0);
                    auto* tr = trackBuffer (target, 1);
                    for (int x = 0; x < numSamples; ++x)
                    {
                        tl[x] += al[x];
                        tr[x] += ar[x];
                    }
                }
            }
        }

        if (liveInput != nullptr && liveInput->numChannels > 0)
            mixLiveInput (state, playing, numSamples);

        // 2) inserts by layers: within a layer nothing depends on anything else; each track pulls the
        //    outputs of the tracks that send to it (without writing into others' buffers)
        trackActive.fill (0);
        tapActive.fill (0);

        for (std::size_t d = 0; d < state.mixerLayers.size(); ++d)
        {
            const auto split = d < state.audioThreadLayers.size() && ! state.audioThreadLayers[d].empty();
            const auto& layer = split ? state.threadedLayers[d] : state.mixerLayers[d];
            auto track = [this, &state, &layer, numSamples, meters] (int i) { processTrack (state, layer[(std::size_t) i], numSamples, meters); };

            if (workers != nullptr && state.parallel && layer.size() > 1)
                workers->parallelFor ((int) layer.size(), track);
            else
                for (int i = 0; i < (int) layer.size(); ++i)
                    track (i);

            if (split) // inserts with a plugin whose "Allow threaded processing" is off
                for (const auto t : state.audioThreadLayers[d])
                    processTrack (state, t, numSamples, meters);
        }
    }

    // 3) Master: inputs, effects, fader. Without a configured mixer, it stays at 0 dB.
    pullInputs (state, 0, numSamples);
    runEffects (state, 0, numSamples);

    if (! song.mixer.empty())
    {
        applyTrackEq (song.mixer[0], 0, song.cleanMixer, trackBuffer (0, 0), trackBuffer (0, 1), numSamples);
        applyStereo (song.mixer[0], trackBuffer (0, 0), trackBuffer (0, 1), numSamples);
    }

    float gl = 1.0f, gr = 1.0f;

    if (! song.mixer.empty())
    {
        const auto& master = song.mixer[0];
        const auto volume = state.hasAutomation ? automated (state.mixerVolumeAuto[0], master.volume) : master.volume;
        const auto pan = state.hasAutomation ? automated (state.mixerPanAuto[0], (master.pan + 1.0f) * 0.5f) * 2.0f - 1.0f : master.pan;
        panGains (pan, master.muted ? 0.0f : faderGain (volume), gl, gr);
    }

    auto& lastL = lastTrackGain[0];
    auto& lastR = lastTrackGain[1];
    const auto fromL = std::isnan (lastL) ? gl : lastL;
    const auto fromR = std::isnan (lastR) ? gr : lastR;
    const auto* masterL = trackBuffer (0, 0);
    const auto* masterR = trackBuffer (0, 1);
    lastL = glideGain (masterL, left, numSamples, fromL, gl);
    lastR = glideGain (masterR, right, numSamples, fromR, gr);

    if (meters != nullptr)
    {
        meters->accumulate (0, peakOf (left, numSamples), peakOf (right, numSamples));
        meters->capture.push (0, left, right, numSamples);
        meters->logTap.push (0, left, right, numSamples);
        if (chunkPlaying)
            meters->bounceTap.push (0, left, right, numSamples);
    }

    if (hwOut != nullptr && ! state.hwRoutes.empty() && hwOut->active.load (std::memory_order_relaxed))
        mixHardwareOutputs (state, numSamples);
}

// 4) inserts with a hardware output target: their post-fader output (the same signal the Master gets) into the
//    device-channel buses, delayed like the Master's path
void RackRenderer::mixHardwareOutputs (const RackState& state, int n) noexcept
{
    auto* sl = hwScratch.data();
    auto* sr = hwScratch.data() + blockCapacity;

    for (const auto& route : state.hwRoutes)
    {
        const auto t = route.track;
        if (t <= 0 || t >= maxMixerTracks)
            continue;

        const float* l = trackActive[(std::size_t) t] ? trackBuffer (t, 0) : nullptr;
        const float* r = trackActive[(std::size_t) t] ? trackBuffer (t, 1) : nullptr;

        if (route.delay != nullptr)
        {
            if (l != nullptr)
            {
                std::copy (l, l + n, sl);
                std::copy (r, r + n, sr);
            }
            else
            {
                std::fill (sl, sl + n, 0.0f);
                std::fill (sr, sr + n, 0.0f);
            }
            route.delay->process (sl, sr, n);
            l = sl;
            r = sr;
        }

        if (l == nullptr)
            continue;

        const auto [first, count] = outputChannelsOf (route.channel, route.mode);
        auto* a = hwOut->bus (first, liveInputOffset, n);
        auto* b = count > 1 ? hwOut->bus (first + 1, liveInputOffset, n) : nullptr;
        addToOutput (l, r, route.mode, a, b, n);
    }
}

// Track stereo separation, phase and channel swap (before the fader, like FL).
// Narrowing uses FL's exponential curve, widening is linear (docs/analise/REA_rodada2.md §2.2).
void RackRenderer::applyStereo (const MixerTrack& track, float* l, float* r, int n) noexcept
{
    if (track.stereo != 0.0f)
    {
        const auto s = track.stereo > 0.0f ? (std::pow (5.0f, std::min (track.stereo, 1.0f)) - 1.0f) / 4.0f
                                           : std::max (track.stereo, -1.0f);
        // s = 1: mono; s = -1: side doubled
        const auto keep = 1.0f - 0.5f * s, cross = 0.5f * s;

        for (int i = 0; i < n; ++i)
        {
            const auto a = l[i], b = r[i];
            l[i] = keep * a + cross * b;
            r[i] = keep * b + cross * a;
        }
    }

    if (track.swapChannels)
        for (int i = 0; i < n; ++i)
            std::swap (l[i], r[i]);

    if (track.invertPhase)
        for (int i = 0; i < n; ++i)
        {
            l[i] = -l[i];
            r[i] = -r[i];
        }
}

void RackRenderer::mixLiveInput (const RackState& state, bool playing, int n) noexcept
{
    const auto recording = liveInput->recording.load (std::memory_order_relaxed);

    // the Master too (FL's slot panel has its audio input selector; REA round 143): it plays whenever its
    // monitoring is not off (it has no record arm)
    for (int t = 0; t < (int) state.song.mixer.size() && t < maxMixerTracks; ++t)
        mixLiveInputTrack (state, t, recording, playing, n);
}

void RackRenderer::mixLiveInputTrack (const RackState& state, int t, bool recording, bool playing, int n) noexcept
{
    if (t >= (int) state.song.mixer.size() || t >= maxMixerTracks)
        return;
    const auto& track = state.song.mixer[(std::size_t) t];
    const auto [first, count] = inputChannelsOf (track, liveInput->numChannels);

    if (count == 0)
        return;

    const auto* a = liveInput->channel (first, liveInputOffset);
    const auto* b = count > 1 ? liveInput->channel (first + 1, liveInputOffset) : a;

    if (a == nullptr || b == nullptr)
        return;

    liveInput->meters.accumulate (t, peakOf (a, n), peakOf (b, n));

    if (t == 0 ? track.monitor == InputMonitor::off
               : ! inputMonitored (track.monitor, liveInput->armed[(std::size_t) t].load (std::memory_order_relaxed), playing, recording))
        return;

    auto* dl = trackBuffer (t, 0);
    auto* dr = trackBuffer (t, 1);

    for (int i = 0; i < n; ++i)
    {
        dl[i] += a[i];
        dr[i] += b[i];
    }
}

void RackRenderer::sumChannelsInto (const RackState& state, int t, int channels, int n) noexcept
{
    if ((std::size_t) t >= state.trackChannels.size())
        return;
    auto* dl = trackBuffer (t, 0);
    auto* dr = trackBuffer (t, 1);
    for (const auto c : state.trackChannels[(std::size_t) t])
    {
        if (c >= channels || channelSilent[(std::size_t) c])
            continue;
        const auto* sl = channelBuffer (c, 0);
        const auto* sr = channelBuffer (c, 1);
        for (int k = 0; k < n; ++k)
        {
            dl[k] += sl[k];
            dr[k] += sr[k];
        }
    }
}

bool RackRenderer::runTaskGraph (const RackState& state, bool playing, int channels, int n, MeterBank* meters) noexcept
{
    // LYNX_TASKGRAPH=0: the layers only (A/B measurements)
    static const bool enabled = [] { const char* e = std::getenv ("LYNX_TASKGRAPH"); return e == nullptr || std::atoi (e) != 0; }();
    if (! enabled || ! state.graphOk || workers == nullptr || ! state.parallel || state.graphChannels != channels)
        return false;
    const auto tracks = std::max<int> (1, (int) state.song.mixer.size());
    const auto tasks = channels + tracks - 1;
    if (tasks <= 0 || tasks > graphCapacity || (int) state.graphIndegree.size() != tasks)
        return false;
    // extra outputs of an instrument (a generator Patcher's) go to inserts known only now: the layers then
    for (int c = 0; c < channels; ++c)
        if (state.instruments[(std::size_t) c] != nullptr && state.instruments[(std::size_t) c]->auxOutputCount() > 0)
            return false;

    trackActive.fill (0);
    tapActive.fill (0);
    const auto live = liveInput != nullptr && liveInput->numChannels > 0;
    const auto recording = live && liveInput->recording.load (std::memory_order_relaxed);

    for (int i = 0; i < tasks; ++i)
    {
        graphPending[(std::size_t) i].store (state.graphIndegree[(std::size_t) i], std::memory_order_relaxed);
        graphSlots[(std::size_t) i].store (0, std::memory_order_relaxed);
    }
    auto pushed = 0;
    for (const auto r : state.graphRoots)
        graphSlots[(std::size_t) pushed++].store (r + 1, std::memory_order_relaxed);
    graphPushed.store (pushed, std::memory_order_release); // (the pool publishes its work after this: seq_cst)

    const auto timing = times->enabled.load (std::memory_order_relaxed);
    auto task = [&] (int position) {
        int id;
        for (int spins = 0; (id = graphSlots[(std::size_t) position].load (std::memory_order_acquire)) == 0; ++spins)
        {
            WorkerPool::relax();
            if ((spins & 1023) == 1023)
                std::this_thread::yield();
        }
        --id;
        if (id < channels)
        {
            if (! timing || id >= ProcessTimes::channels)
                renderChannel (state, id, n);
            else
            {
                const auto before = ProcessTimes::now();
                renderChannel (state, id, n);
                times->channelNs[(std::size_t) id].fetch_add (ProcessTimes::now() - before, std::memory_order_relaxed);
            }
        }
        else
        {
            const auto t = id - channels + 1;
            sumChannelsInto (state, t, channels, n);
            if (live)
                mixLiveInputTrack (state, t, recording, playing, n);
            processTrack (state, t, n, meters);
        }
        for (auto k = state.graphNextStart[(std::size_t) id]; k < state.graphNextStart[(std::size_t) id + 1]; ++k)
        {
            const auto j = state.graphNext[(std::size_t) k];
            if (graphPending[(std::size_t) j].fetch_sub (1, std::memory_order_acq_rel) == 1)
                graphSlots[(std::size_t) graphPushed.fetch_add (1, std::memory_order_acq_rel)].store (j + 1, std::memory_order_release);
        }
    };
    workers->parallelFor (tasks, task);

    // the Master's own channels and input (its effects run after, as before)
    sumChannelsInto (state, 0, channels, n);
    if (live)
        mixLiveInputTrack (state, 0, recording, playing, n);
    return true;
}

void RackRenderer::pullInputs (const RackState& state, int t, int n) noexcept
{
    if ((std::size_t) t >= state.mixerInputs.size())
        return;

    auto* dl = trackBuffer (t, 0);
    auto* dr = trackBuffer (t, 1);

    for (const auto& in : state.mixerInputs[(std::size_t) t])
    {
        const auto tap = in.tap >= 0 && in.tap < RackState::maxSendTaps ? in.tap : -1;
        const auto active = tap >= 0 ? tapActive[(std::size_t) tap] : trackActive[(std::size_t) in.source];
        const auto& delays = state.sendDelays[(std::size_t) in.source];
        auto* delay = (std::size_t) in.delay < delays.size() ? delays[(std::size_t) in.delay].get() : nullptr;
        auto* sl = tap >= 0 ? tapBuffer (tap, 0) : trackBuffer (in.source, 0);
        auto* sr = tap >= 0 ? tapBuffer (tap, 1) : trackBuffer (in.source, 1);

        if (delay != nullptr)
        {
            // PDC: this edge is shorter than the slowest one arriving here. A silent source
            // still drains the tail that was in the line.
            delay->mixInto (active ? sl : nullptr, active ? sr : nullptr, in.level, dl, dr, n);
            continue;
        }

        if (! active)
            continue;

        for (int i = 0; i < n; ++i)
        {
            dl[i] += sl[i] * in.level;
            dr[i] += sr[i] * in.level;
        }
    }
}

void RackRenderer::processTrack (const RackState& state, int t, int n, MeterBank* meters) noexcept
{
    const auto& track = state.song.mixer[(std::size_t) t];
    pullInputs (state, t, n);

    const auto silent = track.muted || ! state.mixerAudible[(std::size_t) t];

    // target gain; the ramp from the previous block avoids clicks on mute and fader
    float gl = 0.0f, gr = 0.0f;

    if (! silent)
    {
        const auto volume = state.hasAutomation ? automated (state.mixerVolumeAuto[(std::size_t) t], track.volume) : track.volume;
        const auto pan = state.hasAutomation ? automated (state.mixerPanAuto[(std::size_t) t], (track.pan + 1.0f) * 0.5f) * 2.0f - 1.0f
                                             : track.pan;
        panGains (pan, faderGain (volume), gl, gr);
    }

    auto& lastL = lastTrackGain[(std::size_t) t * 2];
    auto& lastR = lastTrackGain[(std::size_t) t * 2 + 1];
    const auto fromL = std::isnan (lastL) ? gl : lastL;
    const auto fromR = std::isnan (lastR) ? gr : lastR;

    if (silent && fromL == 0.0f && fromR == 0.0f)
    {
        if (meters != nullptr)
            meters->capture.pushSilence (t, n);
        return; // stays inactive: nobody pulls anything from it
    }

    runEffects (state, t, n);

    auto* l = trackBuffer (t, 0);
    auto* r = trackBuffer (t, 1);
    applyTrackEq (track, t, state.song.cleanMixer, l, r, n);
    applyStereo (track, l, r, n);
    lastL = glideGain (l, l, n, fromL, gl);
    lastR = glideGain (r, r, n, fromR, gr);

    trackActive[(std::size_t) t] = 1;

    if (meters != nullptr)
    {
        meters->accumulate (t, peakOf (l, n), peakOf (r, n));
        meters->capture.push (t, l, r, n);
    }
}
} // namespace lynx
