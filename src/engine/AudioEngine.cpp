#include "engine/AudioEngine.h"
#include "engine/AudioLogger.h"

#include "core/Patcher.h"
#include "core/ValueMath.h"
#include "platform/Paths.h"
#include "engine/FlStudio.h"

#include <limits>
#include <map>
#include <thread>

#include "engine/Text.h"
#include "ui/I18n.h"

#include "core/FlpImport.h"
#include "core/LoadTrace.h"
#include "core/Instruments.h"
#include "core/Denormals.h"
#include "core/Dither.h"
#include "core/ProjectSafety.h"
#include "core/SoftClip.h"
#include "core/Scheduler.h"
#include "engine/ChannelRackProcessor.h"
#include "engine/DllAsioDevice.h"
#include "engine/PipeWireDevice.h"
#include "platform/LinuxRealtime.h"
#include "engine/Metronome.h"
#include "core/NoteTools.h"
#include "engine/PreviewPlayer.h"
#include "flp/Flp.h"

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_MAC
 #include <pthread.h>
 #include <sys/qos.h>
#elif JUCE_LINUX || JUCE_BSD
 #include <pthread.h>
 #include <sched.h>
#endif

namespace lynx
{
using IONode = juce::AudioProcessorGraph::AudioGraphIOProcessor;

// The FL Studio installation (registry, else the usual folders) to resolve %FLStudio...%.
FlPathRoots findFlStudioRoots()
{
    FlPathRoots roots;
    const auto fl = findFlStudio();

    if (fl.installDir.isDirectory())
    {
        roots.factoryData = fl.installDir.getFullPathName().toStdString();
        roots.data = fl.installDir.getChildFile ("Data").getFullPathName().toStdString();
    }

    if (fl.userData.isDirectory())
        roots.userData = fl.userData.getFullPathName().toStdString();

    roots.home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName().toStdString();
    return roots;
}

const natives::FlAccess& flStudioAccess()
{
    static const natives::FlAccess cached = [] {
        natives::FlAccess access;
        access.roots = findFlStudioRoots();
        access.fileExists = [] (const std::string& path) { return platform::localFile (path).existsAsFile(); };
        access.readFile = [] (const std::string& path) {
            juce::MemoryBlock data;
            platform::localFile (path).loadFileAsData (data);
            const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
            return std::vector<std::uint8_t> (bytes, bytes + data.getSize());
        };
        access.listPresets = [] (const std::string& folder) {
            std::vector<std::string> out;
            auto files = platform::localFile (folder).findChildFiles (juce::File::findFiles, true, "*.fst;*.tbio;*.gmsynth;*.mrp");
            std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
                return a.getFullPathName().compareNatural (b.getFullPathName()) < 0;
            });
            for (const auto& f : files)
                out.push_back (f.getFullPathName().toStdString());
            return out;
        };
        // (FL Keys' raw wavetables become WAV files of LYNX's settings folder, like an .flp's embedded samples)
        access.saveEmbedded = [] (const std::vector<std::uint8_t>& wav, const std::string& name) -> std::string {
            const auto folder = PluginHost::settingsFolder().getChildFile ("flp-embedded");
            folder.createDirectory();
            const auto hash = juce::String::toHexString ((juce::int64) juce::DefaultHashFunctions::generateHash (
                juce::String::createStringFromData (wav.data(), (int) juce::jmin<std::size_t> (wav.size(), 65536)) + juce::String ((juce::int64) wav.size()),
                0x7FFFFFFF));
            const auto file = folder.getChildFile (juce::File::createLegalFileName (juce::String::fromUTF8 (name.c_str())) + "-" + hash + ".wav");
            if (! file.existsAsFile() && ! file.replaceWithData (wav.data(), wav.size()))
                return {};
            return file.getFullPathName().toStdString();
        };
        return access;
    }();
    return cached;
}

namespace
{
// Audio workers with real-time priority (MMCSS "Pro Audio" on Windows, like the driver; QoS class on macOS; SCHED_FIFO
// on Linux when allowed).
void raiseAudioThreadPriority()
{
   #if JUCE_WINDOWS
    using AvSet = void* (__stdcall*) (const wchar_t*, unsigned long*);
    static const auto avSet = [] () -> AvSet {
        if (auto* avrt = LoadLibraryW (L"avrt.dll"))
            return reinterpret_cast<AvSet> (reinterpret_cast<void*> (GetProcAddress (avrt, "AvSetMmThreadCharacteristicsW")));
        return nullptr;
    }();
    // FL's audio worker threads also raise their MMCSS priority to "high" (FLEngine_x64.dll, REA round 87)
    using AvPriority = int (__stdcall*) (void*, int);
    static const auto avPriority = [] () -> AvPriority {
        if (auto* avrt = LoadLibraryW (L"avrt.dll"))
            return reinterpret_cast<AvPriority> (reinterpret_cast<void*> (GetProcAddress (avrt, "AvSetMmThreadPriority")));
        return nullptr;
    }();
    unsigned long task = 0;

    if (auto* handle = avSet != nullptr ? avSet (L"Pro Audio", &task) : nullptr)
    {
        if (avPriority != nullptr)
            avPriority (handle, 1 /* AVRT_PRIORITY_HIGH */);
    }
    else
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
   #elif JUCE_MAC
    // the highest quality-of-service class: scheduled like the UI's own interactive work, on the performance cores
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
   #elif JUCE_LINUX || JUCE_BSD
    // real-time FIFO scheduling when the user may have it (audio group / rtkit limits, as JACK and PipeWire set up);
    // below the usual JACK/PipeWire audio thread priorities. Without the permission the thread stays as it is.
    sched_param param {};
    param.sched_priority = juce::jlimit (sched_get_priority_min (SCHED_FIFO), sched_get_priority_max (SCHED_FIFO), 70);
    if (pthread_setschedparam (pthread_self(), SCHED_FIFO, &param) != 0)
        rt::noteAudioThread (false); // RealtimeKit / the portal may grant it (round 161)
   #endif
}

int workerCount()
{
    // diagnostics: LYNX_WORKERS=n fixes the number of extra threads
    if (const auto forced = juce::SystemStats::getEnvironmentVariable ("LYNX_WORKERS", {}); forced.isNotEmpty())
        return juce::jlimit (0, 31, forced.getIntValue());

    // the audio thread works too; leave one core free for the UI
    const auto cores = (int) std::thread::hardware_concurrency();
    return juce::jlimit (0, 7, cores - 2);
}
juce::File midiSettingsFile() { return PluginHost::settingsFolder().getChildFile ("midi-settings.txt"); }
juce::File audioDeviceFile() { return PluginHost::settingsFolder().getChildFile ("audio-device.xml"); }
} // namespace

// Saves the audio device setup whenever it changes (Settings › Audio, the setup wizard, a driver's own panel), so
// the next start opens the same device.
struct AudioEngine::DeviceStateSaver final : public juce::ChangeListener
{
    explicit DeviceStateSaver (AudioEngine& e) : engine (e) { engine.deviceManager.addChangeListener (this); }
    ~DeviceStateSaver() override { engine.deviceManager.removeChangeListener (this); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { engine.saveAudioDeviceState(); }
    AudioEngine& engine;
};

void AudioEngine::saveAudioDeviceState()
{
    if (auto xml = deviceManager.createStateXml())
        xml->writeTo (audioDeviceFile());
}

void AudioEngine::restoreAudioDeviceState (const juce::XmlElement* state)
{
    deviceStateSaver = nullptr;
    if (state != nullptr)
        state->writeTo (audioDeviceFile());
    else
        audioDeviceFile().deleteFile();
}

namespace
{
AudioEngine::RecordingSettings loadRecordingSettings();
}

struct AudioEngine::Drainer final : juce::TimeSliceClient
{
    explicit Drainer (AudioEngine& e) : engine (e) {}
    int useTimeSlice() override
    {
        std::vector<std::shared_ptr<Take>> list;
        {
            const juce::ScopedLock lock (engine.drainLock);
            list = engine.draining;
        }
        for (auto& take : list)
        {
            const juce::ScopedLock lock (take->drainMutex);
            take->drain();
        }
        return list.empty() ? 50 : 5;
    }
    AudioEngine& engine;
};


AudioEngine::AudioEngine (bool openAudioDevice)
    : workers (std::make_unique<WorkerPool> (workerCount(), raiseAudioThreadPriority)),
      song (Song::makeDefault())
{
    inputSlot.fill (-1);
    outputSlot.fill (-1);
    publishSong();
    buildInitialGraph();

    if (openAudioDevice)
    {
        player.setProcessor (&graph);

        // LYNX ASIO without installation: lynx-asio.dll next to LYNX.exe, when it is not registered (REA round 106)
        if (auto lynxAsio = makeBuiltInLynxAsioType())
        {
            deviceManager.getAvailableDeviceTypes(); // the standard types first
            deviceManager.addAudioDeviceType (std::move (lynxAsio));
        }
        // LYNX's own PipeWire client on Linux (round 161), listed when a PipeWire daemon answers
        if (auto pipeWire = makePipeWireDeviceType())
        {
            deviceManager.getAvailableDeviceTypes();
            deviceManager.addAudioDeviceType (std::move (pipeWire));
        }

        // 0 inputs, 2 outputs; the device of the last session (Settings › Audio) if it is still there, else the default.
        const auto saved = juce::XmlDocument::parse (audioDeviceFile());
        auto fresh = saved == nullptr;
        if (deviceManager.initialise (0, 2, saved.get(), true).isNotEmpty() && saved != nullptr)
        {
            deviceManager.initialise (0, 2, nullptr, true);
            fresh = true;
        }
        if (fresh)
        {
            choosePreferredDriver(); // Linux: PipeWire, then JACK, then ALSA
            chooseSafeBufferSize(); // no saved setup: a buffer with room for heavy projects (Settings › Audio changes it)
        }
        deviceManager.addAudioCallback (this);
        if (rackProcessor != nullptr)
            rackProcessor->getRenderer().setProfiling (true); // per-unit CPU for the CPU meter and the glitch log
        deviceStateSaver = std::make_unique<DeviceStateSaver> (*this);
        midiPrefs = midiPrefsShared = prefs::MidiPrefs::fromText (midiSettingsFile().loadFileAsString().toStdString());
        recordingSettings = loadRecordingSettings();
        saveGlobalLinks = true;

        prefetcher = std::make_unique<StreamPrefetcher> (transport);
        prefetcher->update (song, samples);
        enableAllMidiInputs();
        deviceManager.addMidiInputDeviceCallback ({}, this); // {} = all inputs enabled
        midiDevicesChanged = juce::MidiDeviceListConnection::make ([this] { enableAllMidiInputs(); });
    }

    startTimerHz (30); // recording and collection of retired snapshots
}

AudioLogger& AudioEngine::getAudioLogger()
{
    if (audioLogger == nullptr)
        audioLogger = std::make_unique<AudioLogger> (meters.logTap);
    return *audioLogger;
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    audioLogger.reset(); // closes the log file

    // the takes in progress are closed as they are (complete WAVs); their journals stay, so the next start offers to
    // put them on the Playlist (round 163)
    {
        const juce::SpinLock::ScopedLockType lock (takeLock);
        takes.fill (nullptr);
        takeCount = 0;
    }
    {
        const juce::ScopedLock lock (drainLock);
        draining.clear();
    }
    if (drainer != nullptr)
        recordThread.removeTimeSliceClient (drainer.get());
    for (auto& take : openTakes)
    {
        take->stream->finish();
        const juce::ScopedLock lock (take->drainMutex);
        take->drain();
        take->writer.close();
    }
    openTakes.clear();

    recordThread.stopThread (2000);
    prefetcher = nullptr;
    midiDevicesChanged = {};
    deviceStateSaver = nullptr;
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeAudioCallback (this);
    player.setProcessor (nullptr);
    graph.clear(); // the nodes reference rackExchange and transport
}

void AudioEngine::buildInitialGraph()
{
    // The output node inherits the graph's channel count at the moment it is created.
    // Without this the graph still has 0 outputs and JUCE silently rejects the connections.
    graph.setPlayConfigDetails (0, 2, transport.sampleRate.load(), 512);

    auto output = graph.addNode (std::make_unique<IONode> (IONode::audioOutputNode));
    auto rack = graph.addNode (std::make_unique<ChannelRackProcessor> (transport, rackExchange, liveEvents, midiEvents, meters, workers.get()));
    rackProcessor = dynamic_cast<ChannelRackProcessor*> (rack->getProcessor());
    if (rackProcessor != nullptr)
    {
        rackProcessor->setLiveInput (&liveInput);
        rackProcessor->setHardwareOutput (&hwOutput);
    }
    auto metronome = graph.addNode (std::make_unique<Metronome> (transport));
    auto previewOwned = std::make_unique<PreviewPlayer>();
    preview = previewOwned.get();
    preview->setVolume (controls::previewGain (previewVolume));
    auto previewNode = graph.addNode (std::move (previewOwned));

    for (int channel = 0; channel < 2; ++channel)
    {
        graph.addConnection ({ { rack->nodeID, channel }, { output->nodeID, channel } });
        graph.addConnection ({ { metronome->nodeID, channel }, { output->nodeID, channel } });
        graph.addConnection ({ { previewNode->nodeID, channel }, { output->nodeID, channel } });
    }

    jassert (graph.getConnections().size() == 6);
}

float AudioEngine::renderOffline (const RenderOptions& options)
{
    const auto seconds = options.seconds;
    const auto sampleRate = options.sampleRate;
    auto* output = options.output;
    const auto& progress = options.progress;
    const auto tailSeconds = options.tailSeconds;
    const auto trimSamples = options.trimSamples;
    std::optional<Dither> dither;

    if (options.ditherBits > 0)
        dither.emplace (options.ditherBits, options.ditherShaping);

    constexpr int blockSize = 512;
    constexpr float silence = 3.0e-5f; // ~ -90 dB
    // "HQ for all plugins" (REA round 134): engines with FL render-quality settings use them (Sytrus) and HQ switches
    // are forced on; without it the export keeps the playback quality
    setOfflineRendering (options.hqAllPlugins);
    setHqForAllPlugins (options.hqAllPlugins);
    setPluginsRenderMode (true); // and hosted plugins are told an export is running (FL: "notify about rendering mode")
    transport.sampleRate.store (sampleRate);
    graph.setPlayConfigDetails (0, 2, sampleRate, blockSize);
    graph.prepareToPlay (sampleRate, blockSize);

    if (rackProcessor != nullptr)
        rackProcessor->setMetering (false); // nobody looks at the meters during the render

    lastRenderStats = {};
    // FL's init list first (play() would do it at the first block), so the plugins settle on those values too
    resetToInitialValues();
    int latency = getLatencySamples();
    if (options.pluginWarmUp)
        lastRenderStats.warmUp = warmUpPlugins (options.warmUp, sampleRate, blockSize, progress, latency);

    play();

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    float peak = 0.0f;
    // PDC trim with the latency the plugins report once they have settled
    const auto trim = (std::int64_t) juce::jmax (0, trimSamples + (options.trimPdc ? latency : 0));
    lastRenderStats.trimmedSamples = (int) trim;
    const auto musicSamples = (std::int64_t) (seconds * sampleRate) + trim;
    const auto totalSamples = musicSamples + (std::int64_t) (juce::jmax (0.0, tailSeconds) * sampleRate);
    std::int64_t quietSamples = 0;
    bool cancelled = lastRenderStats.warmUp.cancelled;

    for (std::int64_t done = 0; done < totalSamples && ! cancelled; done += blockSize)
    {
        const auto n = (int) juce::jmin<std::int64_t> (blockSize, totalSamples - done);
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, n);
        view.clear();
        const auto before = juce::Time::getMillisecondCounterHiRes();
        graph.processBlock (view, midi);
        const auto afterGraph = juce::Time::getMillisecondCounterHiRes();
        lastRenderStats.graphMs += afterGraph - before;

        if (! options.keepOvers) // float WAV and the buffered export (normalize) keep what goes above 0 dBFS, like FL
            for (int ch = 0; ch < view.getNumChannels(); ++ch)
                clipOvers (view.getWritePointer (ch), n);

        const auto blockPeak = view.getMagnitude (0, n);
        peak = juce::jmax (peak, blockPeak);

        if (dither)
            for (int ch = 0; ch < view.getNumChannels(); ++ch)
                dither->process (view.getWritePointer (ch), n, ch);

        // PDC: the start is the latency's silence; the song's end arrives `trim` samples later
        if ((output != nullptr || options.capture) && done + n > trim)
        {
            const auto skip = (int) juce::jmax<std::int64_t> (0, trim - done);
            const auto beforeWrite = juce::Time::getMillisecondCounterHiRes();
            if (output != nullptr)
                output->writeFromAudioSampleBuffer (view, skip, n - skip);
            if (options.capture)
                options.capture (view.getReadPointer (0, skip), view.getReadPointer (1, skip), n - skip);
            lastRenderStats.writeMs += juce::Time::getMillisecondCounterHiRes() - beforeWrite;
        }

        transport.positionTicks.store (transport.positionTicks.load() + n * transport.ticksPerSample());

        // in the tail: half a second of silence ends it
        if (done + n > musicSamples)
        {
            quietSamples = blockPeak < silence ? quietSamples + n : 0;

            if (quietSamples > (std::int64_t) (sampleRate * 0.5))
                break;
        }

        if (progress && (done / blockSize) % 32 == 0 && ! progress ((double) done / (double) juce::jmax<std::int64_t> (1, totalSamples)))
        {
            cancelled = true;
            break;
        }
    }

    stop();

    if (rackProcessor != nullptr)
        rackProcessor->setMetering (true);

    graph.releaseResources();
    setPluginsRenderMode (false);
    setOfflineRendering (false);
    setHqForAllPlugins (false);
    return cancelled ? -1.0f : peak;
}

warmup::Result AudioEngine::warmUpPlugins (const warmup::Settings& settings, double sampleRate, int blockSize,
                                           const std::function<bool (double)>& progress, int& latency)
{
    // the hosted plugins, collected once: the timer may republish the song meanwhile (the instances stay the same)
    std::vector<juce::AudioPluginInstance*> hosted;
    forEachHostedPlugin ([&hosted] (juce::AudioPluginInstance& p, bool) { hosted.push_back (&p); });
    if (hosted.empty())
        return {}; // LYNX's own engines are ready as soon as they are created: nothing to wait for

    // keeps the units alive whatever the timer publishes during the warm-up
    std::vector<std::shared_ptr<Instrument>> keepInstruments;
    std::vector<std::shared_ptr<Effect>> keepEffects;
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        keepInstruments.push_back (getChannelInstrument ((int) i));
    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            keepEffects.push_back (getEffect ((int) t, (int) s));

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    int blocks = 0;

    // transport stopped at the start position: no note plays, and the automation / pattern events at that position
    // reach the plugins (the rack evaluates them every block), so they settle on the values the song starts with
    const auto result = warmup::run (
        settings, sampleRate,
        [&]() -> int {
            if (progress && ++blocks % 32 == 0 && ! progress (0.0))
                return -1;
            buffer.clear();
            midi.clear();
            graph.processBlock (buffer, midi);
            return blockSize;
        },
        [&hosted] {
            warmup::Hash h;
            for (auto* p : hosted)
            {
                h.add ((std::uint64_t) (std::int64_t) p->getLatencySamples());
                for (auto* param : p->getParameters())
                    h.addValue (param->getValue());
            }
            return h.value;
        },
        [] { return juce::Time::getMillisecondCounterHiRes() * 0.001; },
        [] (double s) { juce::Thread::sleep (juce::jlimit (1, 20, (int) std::lround (s * 1000.0))); });

    // a latency that changed (reported late) gets its PDC plan before the render, on the message thread as the
    // timer does it (renders from the Export window run on a thread of their own)
    auto republish = [this] {
        if (lastPublishedShape.currentLatencies() != lastPublishedShape.latencies)
        {
            ++latencyRepublishes;
            publishSong();
        }
        return getLatencySamples();
    };
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
        latency = republish();
    else
    {
        struct Reply
        {
            juce::WaitableEvent done;
            std::atomic<int> latency { 0 };
        };
        auto reply = std::make_shared<Reply>();
        reply->latency = getLatencySamples();
        juce::MessageManager::callAsync ([this, reply, republish] {
            reply->latency = republish();
            reply->done.signal();
        });
        reply->done.wait (3000);
        latency = reply->latency.load();
    }

    // LYNX's own units start the render from a clean state, as without the warm-up (free-running LFOs, the delay
    // compensation's lines); the hosted plugins are not reset: they keep what they settled on (a reset re-activates
    // them, which is what they had to settle from)
    if (rackProcessor != nullptr)
        rackProcessor->prepareToPlay (sampleRate, blockSize);
    return result;
}

void AudioEngine::forEachHostedPlugin (const std::function<void (juce::AudioPluginInstance&, bool notifyRender)>& visit) const
{
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument ((int) i).get()); p != nullptr && ! p->hasCrashed())
            visit (p->getInstance(), song.channels[i].plugin.notifyRender);

    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            if (auto* p = dynamic_cast<PluginEffect*> (getEffect ((int) t, (int) s).get()); p != nullptr && ! p->hasCrashed())
                visit (p->getInstance(), song.mixer[t].effects[s].plugin.notifyRender);

    std::function<void (patcher::Graph*)> inGraph = [&] (patcher::Graph* graph) {
        if (graph == nullptr)
            return;
        for (int n = 0; n < graph->nodeCount(); ++n)
        {
            auto effect = graph->effectAt (n);
            auto instrument = graph->instrumentAt (n);
            if (auto* p = dynamic_cast<PluginEffect*> (effect.get()); p != nullptr && ! p->hasCrashed())
                visit (p->getInstance(), true);
            if (auto* p = dynamic_cast<PluginInstrument*> (instrument.get()); p != nullptr && ! p->hasCrashed())
                visit (p->getInstance(), true);
            inGraph (patcher::graphOf (effect.get()));
            inGraph (patcher::graphOf (instrument.get()));
        }
    };
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        inGraph (patcher::graphOf (getChannelInstrument ((int) i).get()));
    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            inGraph (patcher::graphOf (getEffect ((int) t, (int) s).get()));
}

ProcessTimes* AudioEngine::getProcessTimes() noexcept
{
    return rackProcessor != nullptr ? &rackProcessor->processTimes() : nullptr;
}

void AudioEngine::play() noexcept
{
    if (! transport.playing.load())
        resetToInitialValues();
    transport.playing.store (true);
}

// FL resets its "initialized controls" whenever the song position is set in song mode (playing starts, a jump):
// FUN_011e7470 plays the song's init list (PTR_DAT_015dfc48) before the pattern events (REA round 146).
void AudioEngine::resetToInitialValues() noexcept
{
    if (song.initialValues.empty() || song.mode != PlayMode::song)
        return;
    try
    {
        auto differs = false;
        for (const auto& v : song.initialValues)
        {
            const auto now = currentTargetValue (v.target);
            differs = differs || (now >= 0.0f && std::abs (now - v.value) > 1.0e-6f);
        }
        if (! differs)
            return;
        std::vector<Song::InitialValue> plugins;
        editSong ([&plugins] (Song& s) { s.applyInitialValues ([&plugins] (const AutomationTarget& t, float v) { plugins.push_back ({ t, v }); }); },
                  false);
        for (const auto& v : plugins)
        {
            if (v.target.kind == AutomationTargetKind::channelParam)
                if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument (v.target.index).get()))
                    p->setParameterFromUi (v.target.param, v.value);
            if (v.target.kind == AutomationTargetKind::effectParam)
                if (auto* p = dynamic_cast<PluginEffect*> (getEffect (v.target.index, v.target.slot).get()))
                    p->setParameterFromUi (v.target.param, v.value);
        }
    }
    catch (...)
    {
    }
}

void AudioEngine::stop() noexcept
{
    waitingForInput.store (false);
    transport.countIn.store (0.0);
    transport.playing.store (false);
}

void AudioEngine::setRecordOptions (const RecordOptions& options)
{
    recordOptions = options;
    recordOptions.countInBars = juce::jlimit (0, 4, options.countInBars);
    recordOptions.preRollBars = juce::jlimit (0, 8, options.preRollBars);
    recordOptions.postRollBars = juce::jlimit (0, 8, options.postRollBars);
    if (takeCount > 0)
        liveInput.linearSong.store (linearRecording());
    sendChangeMessage();
}

void AudioEngine::startPlayback()
{
    if (! transport.recording.load())
    {
        play();
        return;
    }

    // pre-roll and punch (round 163): playback starts before the range the take keeps; post-roll stops it after
    activePunch.reset();
    punchStopAt = -1;
    auto startAt = (std::int64_t) transport.positionTicks.load();
    if (audioArmed && recordOptions.recordAudio)
    {
        const auto here = (std::int64_t) std::llround (transport.positionTicks.load());
        const auto punching = recordOptions.punchLoopRegion && song.mode == PlayMode::song && song.hasLoop();
        const auto in = punching ? song.loopStart : here;
        const auto out = punching ? song.loopEnd : std::numeric_limits<std::int64_t>::max() / 4;
        const auto pre = recordOptions.preRollBars > 0 ? countInLength (song, recordOptions.preRollBars, std::max<std::int64_t> (0, in - 1)) : 0;
        if (punching || pre > 0)
        {
            activePunch = PunchRange { in, out };
            if (punching)
                punchStopAt = out + (recordOptions.postRollBars > 0 ? countInLength (song, recordOptions.postRollBars, out) : 0);
            startAt = std::max<std::int64_t> (0, in - pre);
            transport.seekRequest.store ((double) startAt);
        }
        liveInput.linearSong.store (takeCount > 0 && linearRecording());
    }

    if (recordOptions.waitForInput)
    {
        waitingForInput.store (true);
        sendChangeMessage();
        return;
    }

    if (recordOptions.countInBars > 0)
    {
        transport.countIn.store ((double) countInLength (song, recordOptions.countInBars, startAt));
        sendChangeMessage();
        return;
    }

    play();
}

void AudioEngine::startOnInput() noexcept
{
    if (waitingForInput.exchange (false))
        transport.playing.store (true);
}

void AudioEngine::setInsertArmed (int track, bool armed)
{
    const auto it = std::find (armedInserts.begin(), armedInserts.end(), track);
    const auto wasArmed = it != armedInserts.end();

    if (armed == wasArmed || track <= 0)
        return;

    if (armed && isInsertRecordSafe (track)) // record safe (round 163): never armed by accident
    {
        notify (InputIssue::recordSafe, track);
        return;
    }

    if (armed)
        armedInserts.push_back (track);
    else
        armedInserts.erase (it);

    if (track < maxMixerTracks)
        liveInput.armed[(std::size_t) track].store (armed);

    if (armed && ! audioArmed)
        setAudioRecordingArmed (true);
    else if (! armed && armedInserts.empty() && audioArmed)
        setAudioRecordingArmed (false);

    sendChangeMessage();
}

bool AudioEngine::isInsertArmed (int track) const noexcept
{
    return std::find (armedInserts.begin(), armedInserts.end(), track) != armedInserts.end();
}
void AudioEngine::rewind() noexcept { transport.rewindRequested.store (true); }
void AudioEngine::seek (double ticks) noexcept
{
    if (transport.playing.load())
        resetToInitialValues(); // a jump while the song plays (FL sets the position the same way)
    transport.seekRequest.store (juce::jmax (0.0, ticks));
}

void AudioEngine::copyScope (float* destination, int count) const noexcept
{
    count = juce::jmin (count, scopeSize);
    auto r = (scopeWrite.load (std::memory_order_acquire) - count + scopeSize) % scopeSize;

    for (int i = 0; i < count; ++i)
    {
        destination[i] = scope[(std::size_t) r].load (std::memory_order_relaxed);
        r = (r + 1) % scopeSize;
    }
}

void AudioEngine::previewFile (const juce::File& file)
{
    if (preview != nullptr)
        preview->playBrowser (file.existsAsFile() ? samples.load (file.getFullPathName()) : nullptr);
}

void AudioEngine::setInputScale (int scale, int root) noexcept
{
    inputScaleRoot.store (root);
    inputScale.store (scale);
}

int AudioEngine::snapInputKey (int key) const noexcept
{
    const auto scale = inputScale.load();
    return scale > 0 ? notetools::snapToScale (key, inputScaleRoot.load(), scale) : key;
}

void AudioEngine::previewSample (std::shared_ptr<const SampleData> sample, bool loop, float gain)
{
    if (preview != nullptr)
        preview->play (std::move (sample), loop, gain);
}

void AudioEngine::setPreviewVolume (double volume) noexcept
{
    previewVolume = controls::clampUtilityVolume (volume);
    if (preview != nullptr)
        preview->setVolume (controls::previewGain (previewVolume));
}

double AudioEngine::getPreviewPosition() const noexcept
{
    return preview != nullptr ? preview->getPlayPosition() : -1.0;
}

void AudioEngine::markHeldKey (int key, bool on) noexcept
{
    if (key < 0 || key > 127)
        return;
    auto& count = heldKeyCount[(std::size_t) key];
    auto before = count.load();
    std::uint8_t after = 0;
    do
        after = on ? (std::uint8_t) std::min (255, before + 1) : (std::uint8_t) (before > 0 ? before - 1 : 0);
    while (! count.compare_exchange_weak (before, after));
    const auto bit = std::uint64_t { 1 } << (key & 63);
    if (after > 0)
        heldKeyBits[key >> 6].fetch_or (bit);
    else
        heldKeyBits[key >> 6].fetch_and (~bit);
}

std::vector<int> AudioEngine::heldInputKeys() const
{
    std::vector<int> keys;
    for (int word = 0; word < 2; ++word)
    {
        const auto bits = heldKeyBits[word].load();
        for (int b = 0; b < 64; ++b)
            if ((bits >> b & 1u) != 0)
                keys.push_back (word * 64 + b);
    }
    return keys;
}

void AudioEngine::sendLiveNote (int channel, int key, float velocity, bool on, bool record)
{
    if (record) // played notes (the PC keyboard) follow the scale snap; previews don't
    {
        markHeldKey (key, on);
        // the note-off stops the key its note-on started, even if the snap changed in between
        const auto requested = key;
        if (on)
        {
            key = snapInputKey (key);
            playedKeys.on (channel, requested, key);
            startOnInput();
        }
        else
            key = playedKeys.off (channel, requested, snapInputKey (key));
    }

    NoteEvent e;
    e.type = on ? NoteEvent::Type::on : NoteEvent::Type::off;
    e.channel = channel;
    e.key = key;
    e.velocity = velocity;
    liveEvents.push (e);

    if (record && transport.recording.load() && transport.playing.load())
        uiRecorded.push_back ({ estimatedPositionNow(), channel, key, velocity, on });
}

void AudioEngine::panic()
{
    NoteEvent e;
    e.type = NoteEvent::Type::choke;
    e.channel = -1; // every channel
    liveEvents.push (e);
    for (auto& c : heldKeyCount)
        c.store (0);
    for (auto& w : heldKeyBits)
        w.store (0);
}

double AudioEngine::estimatedPositionNow() const noexcept
{
    // the position only moves block by block; interpolate by the time since the last block
    auto tick = transport.positionTicks.load (std::memory_order_relaxed);

    if (transport.playing.load (std::memory_order_relaxed))
    {
        const auto elapsedMs = juce::jlimit (0.0, 100.0, juce::Time::getMillisecondCounterHiRes() - blockTimeMs.load());
        tick += elapsedMs * 0.001 * transport.sampleRate.load() * transport.ticksPerSample();
    }

    return tick;
}

void AudioEngine::updateAudioRecording()
{
    const auto wantTake = audioArmed && recordOptions.recordAudio && transport.recording.load();
    const auto open = takeCount > 0;

    // the files are ready before Play: the very first block gets recorded
    if (wantTake && ! open && recordingError.isEmpty())
        startAudioTake();

    // post-roll over: playback stops (the take is closed below on the next tick)
    if (open && punchStopAt >= 0 && transport.playing.load() && transport.positionTicks.load() >= (double) punchStopAt)
    {
        punchStopAt = -1;
        stop();
    }

    // the first samples arrived: the journals that let a crash be recovered (round 163)
    if (open && ! journalsWritten && takeStartTick.load() >= 0.0)
        writeJournals();

    // a file that could not be written (disk full, drive gone): say it once, the take stops there
    for (auto& take : openTakes)
        if (take->failed.load() && recordingError.isEmpty())
        {
            recordingError = take->writer.error().empty() ? juce::String ("could not write ") + take->file.getFullPathName()
                                                          : juce::String::fromUTF8 (take->writer.error().c_str()) + ": " + take->file.getFullPathName();
            notify (InputIssue::failed, take->track, recordingError);
        }

    // playback stopped (or recording turned off) with something recorded: close the takes
    if (open && (! wantTake || (! transport.playing.load() && takeFrames.load() > 0)))
        finishAudioTake();
}

bool AudioEngine::Take::drain()
{
    if (stream == nullptr || failed.load())
        return false;
    const auto n = 8192;
    if (scratch.size() < (std::size_t) (2 * n))
        scratch.resize ((std::size_t) (2 * n));
    auto any = false;
    for (;;)
    {
        float* out[] = { scratch.data(), scratch.data() + n };
        const auto got = stream->pull (out, n);
        if (got <= 0)
            break;
        any = true;
        const float* in[] = { out[0], out[1] };
        if (! writer.write (in, got))
        {
            failed.store (true);
            break;
        }
    }
    return any;
}

namespace
{
juce::File recordingSettingsFile() { return PluginHost::settingsFolder().getChildFile ("recording.txt"); }
juce::File inProgressFile() { return PluginHost::settingsFolder().getChildFile ("recordings-in-progress.txt"); }

AudioEngine::RecordingSettings loadRecordingSettings()
{
    AudioEngine::RecordingSettings s;
    for (const auto& raw : juce::StringArray::fromLines (recordingSettingsFile().loadFileAsString()))
    {
        const auto key = raw.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto value = raw.fromFirstOccurrenceOf ("=", false, false);
        if (key == "format")
            s.format = value.trim() == "float32" ? takes::SampleFormat::float32 : takes::SampleFormat::pcm24;
        else if (key == "trackTakeNames")
            s.trackTakeNames = value.getIntValue() != 0;
        else if (key == "manualOffset")
            s.manualOffset = juce::jlimit (-48000, 48000, value.getIntValue());
        else if (key == "useMeasured")
            s.useMeasured = value.getIntValue() != 0;
        else if (key == "pingOutput")
            s.pingOutput = juce::jlimit (0, 63, value.getIntValue());
        else if (key == "pingInput")
            s.pingInput = juce::jlimit (0, 63, value.getIntValue());
        else if (key == "measured") // measured=<samples>\t<setup key>
            s.measured[value.fromFirstOccurrenceOf ("\t", false, false).toStdString()] = value.upToFirstOccurrenceOf ("\t", false, false).getIntValue();
    }
    return s;
}
} // namespace

void AudioEngine::setRecordingSettings (const RecordingSettings& settings)
{
    recordingSettings = settings;
    juce::String text;
    text << "format=" << (settings.format == takes::SampleFormat::float32 ? "float32" : "pcm24") << "\n"
         << "trackTakeNames=" << (settings.trackTakeNames ? 1 : 0) << "\n"
         << "manualOffset=" << settings.manualOffset << "\n"
         << "useMeasured=" << (settings.useMeasured ? 1 : 0) << "\n"
         << "pingOutput=" << settings.pingOutput << "\n"
         << "pingInput=" << settings.pingInput << "\n";
    for (const auto& [key, samples] : settings.measured)
        text << "measured=" << samples << "\t" << juce::String::fromUTF8 (key.c_str()) << "\n";
    if (deviceStateSaver != nullptr) // (a headless engine — tests, export — keeps the user's file as it is)
        recordingSettingsFile().replaceWithText (text);
    sendChangeMessage();
}

std::string AudioEngine::currentSetupKey() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return {};
    const auto setup = deviceManager.getAudioDeviceSetup();
    return takes::setupKey (device->getTypeName().toStdString(), setup.outputDeviceName.toStdString(), setup.inputDeviceName.toStdString(),
                            device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

takes::Latency AudioEngine::currentLatency() const
{
    takes::Latency l;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        l.driverInput = device->getInputLatencyInSamples();
        l.driverOutput = device->getOutputLatencyInSamples();
    }
    if (recordingSettings.useMeasured)
        if (const auto it = recordingSettings.measured.find (currentSetupKey()); it != recordingSettings.measured.end())
            l.measured = it->second;
    l.manual = recordingSettings.manualOffset;
    l.pdc = getLatencySamples();
    return l;
}

juce::String AudioEngine::startLatencyPing()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return TR_NOOP ("no audio device is open");
    if (transport.playing.load() || isCountingIn())
        return TR_NOOP ("stop playback first");
    if (! hasAudioInput())
        if (const auto r = openAudioInput(); r.issue != InputIssue::none)
            return TR_NOOP ("no audio input is open");
    device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return TR_NOOP ("no audio device is open");
    const auto in = recordingSettings.pingInput, out = recordingSettings.pingOutput;
    if (! device->getActiveInputChannels()[in])
        return TR_NOOP ("the input channel is not open");
    if (! device->getActiveOutputChannels()[out])
        return TR_NOOP ("the output channel is not open");
    // device channel -> the callback's index (only open channels are passed)
    const auto slotOf = [] (const juce::BigInteger& active, int c) {
        if (! active[c])
            return -1;
        int slot = 0;
        for (int k = 0; k < c; ++k)
            slot += active[k] ? 1 : 0;
        return slot;
    };
    pingInput = slotOf (device->getActiveInputChannels(), in);
    pingOutputs = { slotOf (device->getActiveOutputChannels(), out), slotOf (device->getActiveOutputChannels(), out + 1) };
    const auto rate = device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : 48000.0;
    ping.start (8, juce::jmax (4096, (int) (rate / 4.0)), 0.5f);
    return {};
}

takes::LatencyPing::Result AudioEngine::finishLatencyPing()
{
    auto r = ping.result();
    if (r.ok)
    {
        auto s = recordingSettings;
        s.measured[currentSetupKey()] = r.samples;
        setRecordingSettings (s);
    }
    return r;
}

void AudioEngine::setInsertRecordSafe (int track, bool safe)
{
    if (track <= 0 || track >= (int) song.mixer.size() || song.mixer[(std::size_t) track].recordSafe == safe)
        return;
    if (safe && isInsertArmed (track))
        setInsertArmed (track, false);
    editSong ([track, safe] (Song& s) { s.mixer[(std::size_t) track].recordSafe = safe; });
}

void AudioEngine::startAudioTake()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return;

    const auto names = getInputChannelNames();
    const auto deviceChannels = (int) names.size();
    const auto active = device->getActiveInputChannels();

    if (deviceChannels == 0 || active.isZero())
    {
        recordingError = "no audio input is open";
        notify (InputIssue::noInputOpen);
        return;
    }

    // one take per armed insert that has an input source; without armed inserts (Settings' "Record input audio"),
    // the first two input channels into a new insert
    struct Wanted
    {
        int track, first, channels;
    };
    std::vector<Wanted> wanted;
    std::vector<int> withoutInput;

    for (const auto t : armedInserts)
    {
        if (t <= 0 || t >= (int) song.mixer.size() || (int) wanted.size() >= maxTakes)
            continue;
        const auto [first, count] = inputChannelsOf (song.mixer[(std::size_t) t], deviceChannels);
        if (count == 0)
            withoutInput.push_back (t);
        else
            wanted.push_back ({ t, first, count });
    }

    if (armedInserts.empty())
        wanted.push_back ({ 0, 0, juce::jmin (2, deviceChannels) });

    if (wanted.empty())
    {
        recordingError = "the armed inserts have no audio input";
        notify (InputIssue::armedWithoutInput, withoutInput.empty() ? 0 : withoutInput.front());
        return;
    }

    if (! withoutInput.empty())
        notify (InputIssue::insertWithoutInput, withoutInput.front());

    auto folder = recordingFolder;
    if (! folder.createDirectory()) // e.g. a read-only project folder: the global recordings folder
        folder = defaultRecordingFolder(), folder.createDirectory();
    takeSampleRate = device->getCurrentSampleRate();
    takeLatency = currentLatency().total();
    takeTicksPerSample = transport.ticksPerSample();
    const auto format = recordingSettings.format;

    // room on the drive (round 163): under 30 s refuse, under 10 minutes warn
    {
        int channels = 0;
        for (const auto& w : wanted)
            channels += w.channels;
        const auto minutes = takes::minutesLeft (folder.getBytesFreeOnVolume(), takeSampleRate, channels, format);
        if (minutes < 0.5)
        {
            recordingError = "the recordings drive is full";
            notify (InputIssue::lowDisk, 0, juce::String (minutes, 1));
            return;
        }
        if (minutes < 10.0)
            notify (InputIssue::lowDisk, 0, juce::String (minutes, 1));
    }

    const auto now = juce::Time::getCurrentTime();
    std::vector<std::shared_ptr<Take>> made;

    for (const auto& w : wanted)
    {
        juce::String insert;
        if (w.track > 0)
            insert = song.mixer[(std::size_t) w.track].name.empty() ? "Insert " + juce::String (w.track)
                                                                    : juce::String::fromUTF8 (song.mixer[(std::size_t) w.track].name.c_str());
        juce::File file;
        if (recordingSettings.trackTakeNames)
        {
            // "<Insert>_Take<N>.wav", N raised until the name is free (round 163)
            for (int n = 1; n < 100000; ++n)
            {
                file = folder.getChildFile (juce::File::createLegalFileName (juce::String::fromUTF8 (takes::takeName (insert.toStdString(), n).c_str())) + ".wav");
                if (! file.exists() && std::none_of (made.begin(), made.end(), [&file] (const std::shared_ptr<Take>& t) { return t->file == file; }))
                    break;
            }
        }
        else
        {
            // FL's name (REA round 134): "<project>_<yyyy-mm-dd hh-mm-ss>_<insert name>", "_2", "_3"... when taken
            const auto name = juce::File::createLegalFileName (juce::String::fromUTF8 (
                safety::recordingName (recordingStem.toStdString(), now.getYear(), now.getMonth() + 1, now.getDayOfMonth(), now.getHours(),
                                       now.getMinutes(), now.getSeconds(), insert.toStdString()).c_str()));
            const auto unique = safety::uniqueName (name.toStdString(), [&folder] (const std::string& n) {
                return folder.getChildFile (juce::String::fromUTF8 (n.c_str()) + ".wav").exists();
            });
            file = folder.getChildFile (juce::String::fromUTF8 (unique.c_str()) + ".wav");
        }

        auto take = std::make_shared<Take>();
        take->track = w.track;
        take->first = w.first;
        take->channels = w.channels;
        take->file = file;
        // ~22 s at 48 kHz of room between the audio thread and the disk
        take->stream = std::make_unique<takes::TakeStream> (w.channels, 1 << 20);
        if (! take->writer.open (file.getFullPathName().toStdString(), takeSampleRate, w.channels, format))
        {
            recordingError = utf8 ("could not create ") + file.getFullPathName();
            continue;
        }
        made.push_back (std::move (take));
    }

    if (recordingError.isNotEmpty())
        notify (InputIssue::failed, 0, recordingError);

    takeStartTick.store (-1.0);
    takeFrames.store (0);
    takeTicks.store (0.0);
    takeWrapCount.store (0);
    lostFrames = 0;
    journalsWritten = false;
    openTakes = made;
    {
        const juce::ScopedLock lock (drainLock);
        draining = made;
    }
    if (drainer == nullptr)
        drainer = std::make_unique<Drainer> (*this);
    if (! recordThread.isThreadRunning())
        recordThread.startThread (juce::Thread::Priority::high);
    recordThread.addTimeSliceClient (drainer.get());

    const juce::SpinLock::ScopedLockType lock (takeLock);
    takeCount = 0;
    for (auto& take : made)
        takes[(std::size_t) takeCount++] = take.get();
    liveInput.linearSong.store (takeCount > 0 && linearRecording());
}

void AudioEngine::writeJournals()
{
    journalsWritten = true;
    juce::StringArray list (juce::StringArray::fromLines (inProgressFile().loadFileAsString()));
    list.removeEmptyStrings();
    int k = 0;
    for (const auto& take : openTakes)
    {
        takes::Journal j;
        j.wav = take->file.getFullPathName().toStdString();
        j.firstTick = takeStartTick.load();
        j.ticksPerSample = takeTicksPerSample;
        j.sampleRate = takeSampleRate;
        j.latency = takeLatency;
        j.insert = take->track;
        j.playlistTrack = -1;
        j.project = recordingStem.toStdString();
        const auto journal = journalFor (take->file);
        journal.replaceWithText (juce::String::fromUTF8 (takes::journalText (j).c_str()));
        list.addIfNotAlreadyThere (journal.getFullPathName());
        ++k;
    }
    if (deviceStateSaver != nullptr)
        inProgressFile().replaceWithText (list.joinIntoString ("\n"));
}

void AudioEngine::finishAudioTake()
{
    {
        const juce::SpinLock::ScopedLockType lock (takeLock);
        takes.fill (nullptr);
        takeCount = 0;
    }
    liveInput.linearSong.store (false);
    std::vector<std::shared_ptr<Take>> done;
    done.swap (openTakes);
    {
        const juce::ScopedLock lock (drainLock);
        draining.clear();
    }

    std::vector<std::shared_ptr<Take>> kept;
    for (auto& take : done)
    {
        take->stream->finish(); // the audio thread let go of it (takeLock above)
        const juce::ScopedLock lock (take->drainMutex);
        take->drain();
        lostFrames += take->stream->lostFrames();
        take->writer.close();
        if (take->file.existsAsFile())
        {
            sessionRecordings.push_back (take->file);
            kept.push_back (take);
        }
    }

    if (recordOptions.disarmOnStop) // FL's "Disarm on stop"
        for (const auto t : std::vector<int> (armedInserts))
            setInsertArmed (t, false);

    // the journals go once the takes are on the Playlist (or thrown away)
    const auto dropJournals = [this, &done] {
        juce::StringArray list (juce::StringArray::fromLines (inProgressFile().loadFileAsString()));
        for (const auto& take : done)
        {
            journalFor (take->file).deleteFile();
            list.removeString (journalFor (take->file).getFullPathName());
        }
        list.removeEmptyStrings();
        if (deviceStateSaver != nullptr)
        {
            if (list.isEmpty())
                inProgressFile().deleteFile();
            else
                inProgressFile().replaceWithText (list.joinIntoString ("\n"));
        }
    };

    const auto frames = takeFrames.exchange (0);
    const auto startTick = takeStartTick.exchange (-1.0);
    const auto ticks = takeTicks.exchange (0.0);
    const auto keepPunch = activePunch;
    activePunch.reset();
    punchStopAt = -1;
    std::vector<double> wraps;
    for (int k = 0, count = takeWrapCount.exchange (0); k < count; ++k)
        wraps.push_back ((double) takeWraps[(std::size_t) k].load() * takeTicksPerSample);

    if (frames <= 0 || startTick < 0.0)
    {
        for (auto& take : done)
        {
            take->file.deleteFile();
            forgetRecordings ({ take->file });
        }
        dropJournals();
        return;
    }

    if (lostFrames > 0)
        notify (InputIssue::framesLost, 0, juce::String (lostFrames));

    // what came into the interface corresponds to what was heard one latency earlier; the clip lands on that
    // song position to the sample (round 163: sub-tick rest in the source offset)
    const auto placement = takes::place (startTick, ticks, takeTicksPerSample, takeLatency);
    std::vector<takes::Part> parts;
    if (keepPunch.has_value()) // pre-roll / punch in-out: only the range (the rest stays in the file)
        parts = takes::punch (placement, { *keepPunch });
    else if (song.mode == PlayMode::song && song.hasLoop())
        parts = takes::loopPasses (placement, song.loopStart, song.loopEnd, ppq / 4, wraps); // one part per pass of the loop
    else if (recordOptions.recordingMarkers && song.mode == PlayMode::song)
        parts = takes::punch (placement, punchRanges (song.markers())); // FL's "Enable recording markers"
    else
        parts = takes::punch (placement, {});

    if (parts.empty()) // the whole take was outside the ranges
    {
        for (auto& take : done)
        {
            take->file.deleteFile();
            forgetRecordings ({ take->file });
        }
        dropJournals();
        return;
    }

    std::vector<takes::TakeFile> made;
    for (const auto& take : kept)
        made.push_back ({ take->file.getFullPathName().toStdString(), take->file.getFileNameWithoutExtension().toStdString(), take->track });

    editSong ([made, parts] (Song& s) { takes::placeTakeClips (s, made, parts); });
    dropJournals();
}

std::vector<takes::Journal> AudioEngine::recoverInterruptedTakes (const std::vector<juce::File>& folders)
{
    // the journals listed as in progress, and any journal left in the given folders
    juce::StringArray journals (juce::StringArray::fromLines (inProgressFile().loadFileAsString()));
    auto where = folders;
    where.push_back (defaultRecordingFolder());
    for (const auto& folder : where)
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.lynxrec"))
            journals.addIfNotAlreadyThere (f.getFullPathName());
    journals.removeEmptyStrings();

    std::vector<takes::Journal> out;
    for (const auto& path : journals)
    {
        const juce::File journal (path);
        takes::Journal j;
        if (! journal.existsAsFile() || ! takes::parseJournal (journal.loadFileAsString().toStdString(), j))
            continue;
        // (a take being recorded by this very session is not "interrupted")
        if (std::any_of (openTakes.begin(), openTakes.end(), [&journal, this] (const std::shared_ptr<Take>& t) { return journalFor (t->file) == journal; }))
            continue;
        std::int64_t frames = 0;
        if (! takes::repairWav (j.wav, &frames) || frames <= 0)
            continue;
        out.push_back (j);
    }
    return out;
}

int AudioEngine::placeRecoveredTakes (const std::vector<takes::Journal>& recovered)
{
    struct One
    {
        takes::TakeFile file;
        std::vector<takes::Part> parts;
    };
    std::vector<One> placed;
    for (const auto& j : recovered)
    {
        std::int64_t frames = 0;
        if (! takes::repairWav (j.wav, &frames) || frames <= 0)
            continue;
        const auto placement = takes::place (j.firstTick, (double) frames * j.ticksPerSample, j.ticksPerSample, j.latency);
        if (! placement.valid())
            continue;
        const juce::File wav (juce::String::fromUTF8 (j.wav.c_str()));
        placed.push_back ({ { j.wav, wav.getFileNameWithoutExtension().toStdString(), j.insert }, takes::punch (placement, {}) });
    }
    if (! placed.empty())
        editSong ([placed] (Song& s) {
            for (const auto& p : placed)
                takes::placeTakeClips (s, { p.file }, p.parts);
        });
    forgetInterruptedTakes (recovered);
    return (int) placed.size();
}

void AudioEngine::forgetInterruptedTakes (const std::vector<takes::Journal>& recovered)
{
    juce::StringArray list (juce::StringArray::fromLines (inProgressFile().loadFileAsString()));
    for (const auto& j : recovered)
    {
        const auto journal = journalFor (juce::File (juce::String::fromUTF8 (j.wav.c_str())));
        journal.deleteFile();
        list.removeString (journal.getFullPathName());
    }
    list.removeEmptyStrings();
    if (deviceStateSaver != nullptr)
    {
        if (list.isEmpty())
            inProgressFile().deleteFile();
        else
            inProgressFile().replaceWithText (list.joinIntoString ("\n"));
    }
}

juce::File AudioEngine::defaultRecordingFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("LYNX/Recordings");
}

void AudioEngine::setRecordingTarget (const juce::File& folder, const juce::String& projectStem)
{
    recordingFolder = folder == juce::File() ? defaultRecordingFolder() : folder;
    recordingStem = projectStem;
}

void AudioEngine::forgetRecordings (const std::vector<juce::File>& files)
{
    sessionRecordings.erase (std::remove_if (sessionRecordings.begin(), sessionRecordings.end(),
                                             [&files] (const juce::File& f) { return std::find (files.begin(), files.end(), f) != files.end(); }),
                             sessionRecordings.end());
}

std::vector<std::string> AudioEngine::getInputChannelNames() const
{
    std::vector<std::string> names;

    if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && ! device->getActiveInputChannels().isZero())
        for (const auto& n : device->getInputChannelNames())
            if ((int) names.size() < LiveInput::maxChannels)
                names.push_back (n.toStdString());

    return names;
}

bool AudioEngine::hasAudioInput() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr && ! device->getActiveInputChannels().isZero();
}

void AudioEngine::notify (InputIssue issue, int track, const juce::String& detail)
{
    recordingNotice = { issue, track, detail };
    sendChangeMessage();
}

AudioEngine::RecordingNotice AudioEngine::openAudioInput (const juce::String& deviceName)
{
    auto* type = deviceManager.getCurrentDeviceTypeObject();

    if (type == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        return { InputIssue::noDevice, 0, {} };

    auto setup = deviceManager.getAudioDeviceSetup();
    int count = 0;

    if (type->hasSeparateInputsAndOutputs())
    {
        // Windows Audio / DirectSound: the input is a device of its own
        const auto inputs = type->getDeviceNames (true);
        auto name = deviceName.isNotEmpty() ? deviceName : setup.inputDeviceName;

        if (! inputs.contains (name))
        {
            const auto index = type->getDefaultDeviceIndex (true);
            name = juce::isPositiveAndBelow (index, inputs.size()) ? inputs[index] : juce::String();
        }

        if (name.isEmpty())
            return { InputIssue::noInputDevice, 0, {} };

        auto* current = deviceManager.getCurrentAudioDevice();
        if (name == setup.inputDeviceName && ! current->getActiveInputChannels().isZero())
            count = current->getInputChannelNames().size();
        else if (std::unique_ptr<juce::AudioIODevice> probe (type->createDevice ({}, name)); probe != nullptr)
            count = probe->getInputChannelNames().size();

        setup.inputDeviceName = name;
    }
    else
    {
        // ASIO: one device does both; its input channels come in pairs in the mixer
        count = deviceManager.getCurrentAudioDevice()->getInputChannelNames().size();
        setup.inputDeviceName = setup.outputDeviceName;
    }

    if (count <= 0)
        count = 2;

    count = juce::jmin (count, (int) LiveInput::maxChannels);
    juce::BigInteger wanted;
    wanted.setRange (0, count, true);

    if (! setup.useDefaultInputChannels && setup.inputChannels == wanted && ! deviceManager.getCurrentAudioDevice()->getActiveInputChannels().isZero())
        return {};

    setup.useDefaultInputChannels = false;
    setup.inputChannels = wanted;
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    sendChangeMessage();

    if (error.isNotEmpty())
        return { InputIssue::failed, 0, error };
    if (! hasAudioInput())
        return { InputIssue::noInputDevice, 0, {} };
    return {};
}

void AudioEngine::setInsertInput (int track, int channel, bool stereo)
{
    if (track < 0 || track >= (int) song.mixer.size()) // (the Master too: REA round 143)
        return;

    editSong ([=] (Song& s) {
        s.mixer[(std::size_t) track].inputChannel = channel;
        s.mixer[(std::size_t) track].inputStereo = stereo;
    });

    recordingError = {};

    if (channel >= 0 && ! hasAudioInput())
        if (const auto result = openAudioInput(); result.issue != InputIssue::none)
            notify (result.issue, track, result.detail);
}

void AudioEngine::setInsertMonitor (int track, InputMonitor mode)
{
    if (track >= 0 && track < (int) song.mixer.size())
        editSong ([=] (Song& s) { s.mixer[(std::size_t) track].monitor = mode; });
}

void AudioEngine::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m)
{
    // MIDI thread: queues and atomics only (and a short copy of the device settings)
    prefs::MidiInputPrefs device;
    auto velocityCurve = prefs::VelocityCurve::linear;
    {
        const juce::SpinLock::ScopedLockType lock (midiPrefsLock);
        device = midiPrefsShared.input (source != nullptr ? source->getName().toStdString() : std::string());
        velocityCurve = midiPrefsShared.velocity;
    }

    if (! device.enabled)
        return;

    midiActivity.fetch_add (1, std::memory_order_relaxed); // the toolbar's MIDI input light

    if (device.midiChannel != 0 && m.getChannel() != device.midiChannel)
        return;

    if (m.isController())
    {
        controllerQueue.push ({ m.getControllerNumber(), m.getChannel() - 1, m.getControllerValue() });
        return;
    }

    if (! m.isNoteOnOrOff())
        return;

    const auto on = m.isNoteOn();
    markHeldKey (m.getNoteNumber(), on);

    if (on)
        startOnInput();

    // a note-off goes to the channels and key its note-on reached (the selection or the scale snap may have
    // changed while the key was down); a repeated note-on first stops what the previous one started
    InputHeldNotes::Target targets[InputHeldNotes::maxTargets], stale[InputHeldNotes::maxTargets];
    auto count = 0;
    auto staleCount = 0;

    if (on)
    {
        const auto key = snapInputKey (m.getNoteNumber());
        const auto extra = juce::jlimit (0, maxLiveChannels, extraLiveCount.load());

        // the primary selected channel, then the other selected ones (or the device's own channel)
        if (device.target >= 0)
            targets[count++] = { device.target, key };
        else
            for (int i = -1; i < extra && count < InputHeldNotes::maxTargets; ++i)
                targets[count++] = { i < 0 ? liveChannel.load() : extraLiveChannels[(std::size_t) i].load(), key };

        staleCount = midiHeld.noteOn (m.getNoteNumber(), targets, count, stale);
    }
    else
        count = midiHeld.noteOff (m.getNoteNumber(), targets);

    auto push = [this] (const InputHeldNotes::Target& t, bool noteOn, float velocity) {
        NoteEvent e;
        e.type = noteOn ? NoteEvent::Type::on : NoteEvent::Type::off;
        e.channel = t.channel;
        e.key = t.key;
        e.velocity = velocity;
        midiEvents.push (e);

        if (transport.recording.load() && transport.playing.load())
            midiRecordQueue.push ({ estimatedPositionNow(), e.channel, e.key, e.velocity, noteOn });
    };

    for (int i = 0; i < staleCount; ++i)
        push (stale[i], false, 0.0f);

    for (int i = 0; i < count; ++i)
        push (targets[i], on, on ? prefs::mapVelocity (m.getFloatVelocity(), velocityCurve) : 0.0f);
}

void AudioEngine::enableAllMidiInputs()
{
    // (a device turned off in the MIDI settings is closed, so other programs can use it)
    std::vector<prefs::MidiPort> available;
    std::vector<std::string> open;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
    {
        available.push_back ({ device.identifier.toStdString(), device.name.toStdString() });
        if (deviceManager.isMidiInputDeviceEnabled (device.identifier))
            open.push_back (device.identifier.toStdString());
    }

    const auto plan = prefs::planMidiInputs (midiPrefs, available, lastMidiInputIds, open);
    for (const auto& id : plan.close)
        deviceManager.setMidiInputDeviceEnabled (juce::String (id), false);
    for (const auto& id : plan.reopen) // it went away and came back: the old handle may be dead
        deviceManager.setMidiInputDeviceEnabled (juce::String (id), false);
    for (const auto* list : { &plan.reopen, &plan.open })
        for (const auto& id : *list)
            deviceManager.setMidiInputDeviceEnabled (juce::String (id), true);

    lastMidiInputIds.clear();
    for (const auto& port : available)
        lastMidiInputIds.push_back (port.id);
}

void AudioEngine::reconnectMidiInputs()
{
    if (prefetcher == nullptr) // (only with an audio device: the real app)
        return;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        if (deviceManager.isMidiInputDeviceEnabled (device.identifier))
            deviceManager.setMidiInputDeviceEnabled (device.identifier, false);
    enableAllMidiInputs();
}

void AudioEngine::setMidiPrefs (const prefs::MidiPrefs& p, bool save)
{
    midiPrefs = p;
    {
        const juce::SpinLock::ScopedLockType lock (midiPrefsLock);
        midiPrefsShared = p;
    }
    if (! p.pickup)
        pickups.clear();
    if (prefetcher != nullptr) // (only with an audio device: the real app)
        enableAllMidiInputs();
    if (save)
        midiSettingsFile().replaceWithText (juce::String (p.toText()));
    sendChangeMessage();
}

void AudioEngine::setUndoLevels (int levels)
{
    maxUndoSteps = (std::size_t) prefs::clampUndoLevels (levels);
    while (undoStack.size() > maxUndoSteps)
        undoStack.pop_front();
}

juce::StringArray AudioEngine::getActiveMidiInputs() const
{
    juce::StringArray names;

    for (const auto& device : juce::MidiInput::getAvailableDevices())
        if (deviceManager.isMidiInputDeviceEnabled (device.identifier))
            names.add (device.name);

    return names;
}

void AudioEngine::setAudioRecordingArmed (bool armed)
{
    audioArmed = armed;
    recordingError = {};

    if (armed)
    {
        // opens the input of the current driver (every channel, so each insert can pick its own)
        if (! hasAudioInput())
            if (const auto result = openAudioInput(); result.issue != InputIssue::none)
            {
                recordingError = "no audio input";
                notify (result.issue, 0, result.detail);
            }

        if (! recordThread.isThreadRunning())
            recordThread.startThread();
    }

    sendChangeMessage();
}

void AudioEngine::setRecording (bool shouldRecord)
{
    if (shouldRecord && ! transport.recording.load())
        recordingError = {}; // a new attempt
    transport.recording.store (shouldRecord);
    liveInput.recording.store (shouldRecord);
    updateRecording();
    sendChangeMessage();
}

void AudioEngine::updateRecording()
{
    const auto playing = transport.playing.load();
    const auto active = transport.recording.load() && playing;

    if (playing)
        lastPlayingTick = estimatedPositionNow();

    // the take starts when playing with recording on, or at the first recorded event
    // (which is only queued in that situation, even if this timer hasn't seen it yet)
    auto beginTake = [this] {
        if (recorder.isActive())
            return;

        const auto& pattern = song.patterns[(std::size_t) song.currentPattern];
        recorder.begin (pattern.effectiveLength (song.beatsPerBar));
        takeHasUndoStep = false;
        takeClearedChannels.clear();
        takeStartPosition = lastPlayingTick;
    };

    if (active)
        beginTake();

    RecordedEvent e;

    // FL's "Enable recording markers" (song mode): a note that starts outside the Start / Stop recording ranges is
    // not recorded (its release finds no held note and is ignored too)
    const auto punch = activePunch.has_value() ? std::vector<PunchRange> { *activePunch } // pre-roll / punch in-out (round 163)
                       : recordOptions.recordingMarkers && song.mode == PlayMode::song ? punchRanges (song.markers()) : std::vector<PunchRange> {};
    auto outsidePunch = [this, &punch] (const RecordedEvent& ev) {
        if (punch.empty() || ! ev.on)
            return false;
        auto tick = ev.tick;
        if (! song.hasLoop())
            tick = std::fmod (std::max (0.0, tick), (double) std::max<std::int64_t> (1, song.songLength()));
        return ! inPunchRanges (punch, tick);
    };

    while (midiRecordQueue.pop (e))
    {
        if (! recordOptions.recordNotes || outsidePunch (e)) // recording filter: notes off
            continue;
        beginTake();
        recorder.add (song, e);
    }

    for (const auto& ui : uiRecorded)
    {
        if (! recordOptions.recordNotes || outsidePunch (ui))
            continue;
        beginTake();
        recorder.add (song, ui);
    }

    uiRecorded.clear();

    updateAudioRecording();
    updateAutomationRecording (active);

    if (! active && recorder.isActive())
        recorder.end (song, lastPlayingTick);

    auto notes = recorder.takeFinished();

    if (notes.empty())
        return;

    // the whole take is a single undo step: overdub or replace, quantised to the record grid; in song mode a
    // clip of the pattern is placed where recording started if none plays there
    const auto options = recordOptions;
    auto placeClipAt = (std::int64_t) takeStartPosition;
    if (song.mode == PlayMode::song && ! song.hasLoop()) // (the position loops over the song)
        placeClipAt %= std::max<std::int64_t> (1, song.songLength());
    editSong ([&] (Song& s) {
        TakeQuantize q;
        q.grid = options.quantise;
        q.starts = options.quantiseStarts;
        q.end = options.noteEnd;
        q.stepSequencer = options.toStepSequencer;
        commitTake (s.patterns[(std::size_t) s.currentPattern], notes, options.mode, q, takeClearedChannels);

        if (options.placeSongClip && s.mode == PlayMode::song && ! takeHasUndoStep)
            placePatternClipAt (s, s.currentPattern, placeClipAt);
    }, ! takeHasUndoStep);
    takeHasUndoStep = true;
}

void AudioEngine::setSong (Song newSong)
{
    undoStack.clear();
    redoStack.clear();
    song = std::move (newSong);
    selectedChannel = 0;
    selectedChannels = { 0 };
    modified = false;

    // arming isn't saved: a new project starts disarmed; its inserts' audio inputs open the device input
    for (const auto t : std::vector<int> (armedInserts))
        setInsertArmed (t, false);
    if (prefetcher != nullptr && ! hasAudioInput()
        && std::any_of (song.mixer.begin(), song.mixer.end(), [] (const MixerTrack& t) { return t.inputChannel >= 0 || t.flInput >= 0; }))
        openAudioInput();
    // an FL project's insert inputs (REA round 157c): FL's list index on the device open now
    if (prefetcher != nullptr && hasAudioInput())
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        const auto channels = device != nullptr ? device->getInputChannelNames().size() : 0;
        for (auto& t : song.mixer)
        {
            if (t.flInput >= 0 && t.inputChannel < 0)
            {
                const auto source = flInputSource (t.flInput, channels);
                t.inputChannel = source.channel;
                t.inputStereo = source.stereo;
            }
            t.flInput = -1;
        }
    }
    // hardware output targets: their channels open with the project (the ones the device has)
    if (prefetcher != nullptr)
        openTargetedOutputs();

    publishSong();
}

void AudioEngine::editSong (const std::function<void (Song&)>& edit, bool undoable)
{
    if (undoable)
    {
        // Dozens of edits arrive during a drag; only the first one of the click saves the state.
        bool samePress = false;

        if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        {
            const auto press = juce::Desktop::getInstance().getMainMouseSource().getLastMouseDownTime();
            samePress = press == lastUndoPress;
            lastUndoPress = press;
        }
        else
        {
            lastUndoPress = {};
        }

        if (! samePress)
        {
            undoStack.push_back (song);
            redoStack.clear();

            if (undoStack.size() > maxUndoSteps)
                undoStack.pop_front();
        }
    }

    if (undoable)
        modified = true;

    const auto lengthsBefore = patternLengths (song);
    edit (song);
    followPatternLengths (lengthsBefore, song);
    publishSong();
}

void AudioEngine::undo()
{
    if (undoStack.empty())
        return;

    redoStack.push_back (std::move (song));
    song = std::move (undoStack.back());
    undoStack.pop_back();
    modified = true;
    lastUndoPress = {};
    publishSong();
}

void AudioEngine::redo()
{
    if (redoStack.empty())
        return;

    undoStack.push_back (std::move (song));
    song = std::move (redoStack.back());
    redoStack.pop_back();
    modified = true;
    lastUndoPress = {};
    publishSong();
}

namespace
{
const Song& historyState (const std::deque<Song>& undo, const Song& current, const std::deque<Song>& redo, int index)
{
    if (index < (int) undo.size())
        return undo[(std::size_t) index];
    if (index == (int) undo.size())
        return current;
    return redo[redo.size() - 1 - (std::size_t) (index - (int) undo.size() - 1)];
}
} // namespace

ChangeLabel AudioEngine::getHistoryLabel (int index) const
{
    if (index <= 0 || index >= getHistorySize())
        return { TR_NOOP ("Start"), {} };

    return describeChange (historyState (undoStack, song, redoStack, index - 1), historyState (undoStack, song, redoStack, index));
}

void AudioEngine::jumpToHistory (int index)
{
    index = juce::jlimit (0, getHistorySize() - 1, index);

    if (index == getHistoryPosition())
        return;

    // move through the stacks, then publish once
    while (getHistoryPosition() > index && ! undoStack.empty())
    {
        redoStack.push_back (std::move (song));
        song = std::move (undoStack.back());
        undoStack.pop_back();
    }

    while (getHistoryPosition() < index && ! redoStack.empty())
    {
        undoStack.push_back (std::move (song));
        song = std::move (redoStack.back());
        redoStack.pop_back();
    }

    modified = true;
    lastUndoPress = {};
    publishSong();
}

void AudioEngine::setSelectedChannel (int channel)
{
    channel = juce::jlimit (0, juce::jmax (0, (int) song.channels.size() - 1), channel);
    setChannelSelection ({ channel }, channel);
}

void AudioEngine::setChannelSelection (std::vector<int> channels, int primary)
{
    const auto count = (int) song.channels.size();
    std::sort (channels.begin(), channels.end());
    channels.erase (std::unique (channels.begin(), channels.end()), channels.end());
    channels.erase (std::remove_if (channels.begin(), channels.end(), [count] (int c) { return c < 0 || c >= count; }), channels.end());
    primary = juce::jlimit (0, juce::jmax (0, count - 1), primary);

    if (std::find (channels.begin(), channels.end(), primary) == channels.end())
        channels.insert (std::lower_bound (channels.begin(), channels.end(), primary), primary);

    if (channels == selectedChannels && primary == selectedChannel)
    {
        publishSelection();
        return;
    }

    selectedChannels = std::move (channels);
    selectedChannel = primary;
    publishSelection();
    sendChangeMessage();
}

void AudioEngine::publishSelection()
{
    liveChannel.store (selectedChannel);
    int extra = 0;

    for (auto c : selectedChannels)
        if (c != selectedChannel && extra < maxLiveChannels)
            extraLiveChannels[(std::size_t) extra++].store (c);

    extraLiveCount.store (extra);
}

namespace
{
// Whether a drive / network share is there, within a time limit: a disconnected network drive can block a file
// query for half a minute, and an open probes many paths under the same root (round 112). One probe per root per open.
bool rootReachable (const std::string& root)
{
    auto result = std::make_shared<std::atomic<int>> (-1);
    std::thread ([result, root] {
        result->store (platform::localFile (root).isDirectory() ? 1 : 0); // FL's "C:\" spelling on macOS/Linux too
    }).detach();
    const auto started = juce::Time::getMillisecondCounter();
    while (result->load() < 0 && juce::Time::getMillisecondCounter() - started < 2000)
        juce::Thread::sleep (2);
    return result->load() == 1; // still unknown after 2 s: treated as missing (the probe thread finishes on its own)
}

std::string userOf (const Song& song, std::size_t track)
{
    return song.mixer[track].name.empty() ? (track == 0 ? std::string ("Master") : "Insert " + std::to_string (track)) : song.mixer[track].name;
}
} // namespace

void AudioEngine::prepareFlpImport (const juce::File& file, const std::map<std::string, std::string>& relocations, SampleLibrary& library,
                                    PreparedFlp& out)
{
    juce::MemoryBlock data;
    {
        const loadtrace::Scope timing ("read_project");
        if (! file.loadFileAsData (data))
        {
            out.error = utf8 ("could not read the file");
            return;
        }
    }

    try
    {
        loadtrace::stage (0.20, 0.30, TR_NOOP ("Reading the project..."));
        const auto started = std::chrono::steady_clock::now();
        const auto project = flp::parseProject ({ static_cast<const std::uint8_t*> (data.getData()), data.getSize() });
        loadtrace::add ("parse", loadtrace::msSince (started));

        FlpImportOptions options;
        options.roots = findFlStudioRoots();
        options.projectFolder = file.getParentDirectory().getFullPathName().toStdString();
        // samples on a drive that is not there (unplugged external drive, dead network share): one probe per root
        auto roots = std::make_shared<loadtrace::RootProbe> ([] (const std::string& root) {
            const auto t = std::chrono::steady_clock::now();
            const auto ok = rootReachable (root);
            loadtrace::add ("probe_root", loadtrace::msSince (t));
            return ok;
        });
        options.fileExists = [roots] (const std::string& path) {
            if (! roots->rootAvailable (path))
                return false;
            const auto t = std::chrono::steady_clock::now();
            const auto exists = platform::localFile (path).existsAsFile();
            loadtrace::add ("probe_file", loadtrace::msSince (t));
            return exists;
        };
        options.readFile = [roots] (const std::string& path) {
            if (! roots->rootAvailable (path))
                return std::vector<std::uint8_t> {};
            const auto t = std::chrono::steady_clock::now();
            juce::MemoryBlock data;
            platform::localFile (path).loadFileAsData (data);
            const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
            loadtrace::add ("read_aux_file", loadtrace::msSince (t));
            return std::vector<std::uint8_t> (bytes, bytes + data.getSize());
        };
        auto decoded = std::make_shared<std::vector<std::shared_ptr<const SampleData>>>();
        options.sampleFrames = [&library, decoded] (const std::string& path) -> std::int64_t {
            const auto sample = library.load (juce::String::fromUTF8 (path.c_str()));
            if (sample != nullptr)
                decoded->push_back (sample); // kept: the rack uses it next (it was decoded twice before)
            return sample != nullptr ? (std::int64_t) sample->frames() : 0;
        };
        // WAV that only exists inside the .flp (Slicex): goes to %APPDATA%\LYNX\flp-embedded, keyed by content
        options.saveEmbedded = [] (const std::vector<std::uint8_t>& wav, const std::string& name) -> std::string {
            const auto folder = PluginHost::settingsFolder().getChildFile ("flp-embedded");
            folder.createDirectory();
            const auto hash = juce::String::toHexString ((juce::int64) juce::DefaultHashFunctions::generateHash (
                juce::String::createStringFromData (wav.data(), (int) juce::jmin<std::size_t> (wav.size(), 65536)) + juce::String ((juce::int64) wav.size()),
                0x7FFFFFFF));
            const auto file = folder.getChildFile (juce::File::createLegalFileName (juce::String::fromUTF8 (name.c_str())) + "-" + hash + ".wav");

            if (! file.existsAsFile() && ! file.replaceWithData (wav.data(), wav.size()))
                return {};

            return file.getFullPathName().toStdString();
        };

        out.report = {};
        options.report = &out.report;
        options.relocations = relocations;
        {
            const auto t = std::chrono::steady_clock::now();
            out.song = importFlp (project, options);
            loadtrace::add ("import", loadtrace::msSince (t));
        }
        loadtrace::add ("missing_roots", 0.0, (int) roots->missingRoots().size());
        loadtrace::add ("probe_skipped", 0.0, roots->skipped());

        // the project's third-party plugins, to say which channel/insert used each one that is missing
        for (const auto& c : out.song.channels)
            if (c.plugin.foreign.format != ForeignPlugin::Format::none)
                out.uses.push_back ({ c.plugin.foreign.name, c.name, true });
        for (std::size_t t = 0; t < out.song.mixer.size(); ++t)
            for (const auto& fx : out.song.mixer[t].effects)
                if (fx.kind == EffectKind::plugin && fx.plugin.foreign.format != ForeignPlugin::Format::none)
                    out.uses.push_back ({ fx.plugin.foreign.name, userOf (out.song, t), false });
        // (and the ones inside Patchers, REA_rodada141)
        std::function<void (const PatcherDoc&, const std::string&)> patcherUses = [&] (const PatcherDoc& doc, const std::string& user) {
            for (const auto& n : doc.nodes)
            {
                const auto& ref = n.generator ? n.channel.plugin : n.effect.plugin;
                if (ref.foreign.format != ForeignPlugin::Format::none)
                    out.uses.push_back ({ ref.foreign.name, user + " (Patcher)", n.generator });
                if (n.generator && n.channel.instrument == InstrumentKind::patcher)
                    patcherUses (n.channel.patcher, user);
                if (! n.generator && n.effect.kind == EffectKind::patcher)
                    patcherUses (n.effect.patcher, user);
            }
        };
        for (const auto& c : out.song.channels)
            if (c.instrument == InstrumentKind::patcher)
                patcherUses (c.patcher, c.name);
        for (std::size_t t = 0; t < out.song.mixer.size(); ++t)
            for (const auto& fx : out.song.mixer[t].effects)
                if (fx.kind == EffectKind::patcher)
                    patcherUses (fx.patcher, userOf (out.song, t));

        // the samples, decoded now (in parallel, off the message thread) so building the rack only finds them ready
        std::vector<std::string> paths;
        auto addPath = [&paths] (const std::string& p) {
            if (! p.empty() && std::find (paths.begin(), paths.end(), p) == paths.end())
                paths.push_back (p);
        };
        for (const auto& c : out.song.channels)
        {
            if (c.instrument == InstrumentKind::sampler || c.instrument == InstrumentKind::audioClip || c.instrument == InstrumentKind::granulizer
                || c.instrument == InstrumentKind::flkeys || c.instrument == InstrumentKind::directwave)
                addPath (c.samplePath);
            for (const auto& z : c.zones)
                addPath (z.path);
        }
        for (const auto& track : out.song.mixer)
            for (const auto& fx : track.effects)
                addPath (fx.samplePath);

        loadtrace::stage (0.30, 0.55, TR_NOOP ("Reading the samples..."), TR_NOOP ("Reading {0}... {1}/{2}"));
        const auto decodeStarted = std::chrono::steady_clock::now();
        std::atomic<std::size_t> next { 0 }, done { 0 };
        std::mutex keep;
        auto worker = [&] {
            for (auto i = next++; i < paths.size(); i = next++)
            {
                const auto t = std::chrono::steady_clock::now();
                auto sample = library.load (juce::String::fromUTF8 (paths[i].c_str()));
                loadtrace::add ("sample_decode", loadtrace::msSince (t));
                {
                    const std::scoped_lock l (keep);
                    if (sample != nullptr)
                        out.decoded.push_back (std::move (sample));
                }
                const auto n = ++done;
                loadtrace::step ((int) n, (int) paths.size(),
                                 juce::File::createFileWithoutCheckingPath (juce::String::fromUTF8 (paths[i].c_str())).getFileName().toStdString());
            }
        };
        const auto threads = (std::size_t) juce::jlimit (1, 6, juce::SystemStats::getNumCpus() - 1);
        std::vector<std::thread> pool;
        for (std::size_t i = 1; i < juce::jmin (threads, paths.size()); ++i)
            pool.emplace_back (worker);
        worker();
        for (auto& t : pool)
            t.join();
        for (auto& d : *decoded)
            out.decoded.push_back (std::move (d));

        // pitch-preserving stretch (audio clips that follow the tempo): the stretched copy too, in parallel, with the
        // same ratio the rack asks for (RackState::build) — it was computed on the message thread while building
        struct Stretch
        {
            std::string path;
            double ratio;
            StretchMethod method;
        };
        std::vector<Stretch> stretches;
        for (const auto& c : out.song.channels)
            if ((c.instrument == InstrumentKind::sampler || c.instrument == InstrumentKind::audioClip) && c.zones.empty() && ! c.samplePath.empty()
                && c.sampler.keepPitch && c.sampler.stretchBeats > 0.0f)
                if (const auto data = library.load (juce::String::fromUTF8 (c.samplePath.c_str())); data != nullptr && data->frames() > 0)
                {
                    const auto target = c.sampler.stretchBeats * 60.0 / std::max (1.0, out.song.bpm);
                    const auto ratio = target / ((double) data->frames() / data->sampleRate);
                    const auto method = c.sampler.stretchMethod;
                    if (std::none_of (stretches.begin(), stretches.end(), [&] (const Stretch& x) { return x.path == c.samplePath && x.ratio == ratio && x.method == method; }))
                        stretches.push_back ({ c.samplePath, ratio, method });
                }
        if (! stretches.empty())
        {
            const auto stretchStarted = std::chrono::steady_clock::now();
            std::atomic<std::size_t> nextStretch { 0 };
            auto stretchWorker = [&] {
                for (auto i = nextStretch++; i < stretches.size(); i = nextStretch++)
                    library.loadStretched (juce::String::fromUTF8 (stretches[i].path.c_str()), stretches[i].ratio, stretches[i].method);
            };
            std::vector<std::thread> helpers;
            for (std::size_t i = 1; i < juce::jmin (threads, stretches.size()); ++i)
                helpers.emplace_back (stretchWorker);
            stretchWorker();
            for (auto& t : helpers)
                t.join();
            loadtrace::add ("stretch_wall", loadtrace::msSince (stretchStarted), (int) stretches.size());
        }
        loadtrace::add ("samples_wall", loadtrace::msSince (decodeStarted), (int) paths.size());
        out.ok = true;
    }
    catch (const flp::Error& e)
    {
        out.error = juce::String::fromUTF8 (e.what());
    }
}

void AudioEngine::adoptPrepared (PreparedFlp& prepared)
{
    if (prepared.adopted)
        return;
    prepared.adopted = true;
    const auto t = std::chrono::steady_clock::now();
    plugins.clearPreloaded();
    plugins.setPreloadRate (transport.sampleRate.load());
    plugins.adoptFlPlugins (prepared.song, &prepared.missing, &prepared.matched, &prepared.withoutState, &prepared.substituted);
    loadtrace::add ("adopt_plugins", loadtrace::msSince (t));

    // the hosted plugins the rack will create, in its order (channels, then inserts)
    for (const auto& c : prepared.song.channels)
        if (c.instrument == InstrumentKind::plugin && ! c.plugin.id.empty())
            prepared.pluginQueue.push_back (c.plugin);
    for (const auto& track : prepared.song.mixer)
        for (std::size_t i = 0; i < track.effects.size() && i < (std::size_t) maxEffectSlots; ++i)
            if (track.effects[i].kind == EffectKind::plugin && ! track.effects[i].plugin.id.empty())
                prepared.pluginQueue.push_back (track.effects[i].plugin);
}

bool AudioEngine::preloadNextPlugin (PreparedFlp& prepared)
{
    adoptPrepared (prepared);
    if (prepared.pluginsDone >= prepared.pluginQueue.size())
        return false;
    const auto total = (int) prepared.pluginQueue.size();
    const auto& ref = prepared.pluginQueue[prepared.pluginsDone];
    auto wanted = 0;
    for (std::size_t i = 0; i <= prepared.pluginsDone; ++i)
        wanted += prepared.pluginQueue[i].id == ref.id && prepared.pluginQueue[i].state == ref.state ? 1 : 0;
    plugins.preload (ref, wanted);
    ++prepared.pluginsDone;
    loadtrace::step ((int) prepared.pluginsDone, total, {});
    return prepared.pluginsDone < prepared.pluginQueue.size();
}

bool AudioEngine::finishFlpImport (PreparedFlp&& prepared, bool asEdit)
{
    if (! prepared.ok)
        return false;
    adoptPrepared (prepared);
    auto& imported = prepared.song;
    importReport = std::move (prepared.report);
    const auto& uses = prepared.uses;

    missingPlugins = prepared.missing;
    matchedPlugins = prepared.matched;
    pluginsWithoutState = prepared.withoutState;
    missingPluginDetails.clear();
    for (const auto& name : prepared.missing)
    {
        MissingPlugin m;
        m.name = name.toStdString();
        for (const auto& u : uses)
            if (u.name == m.name)
            {
                if (std::find (m.usedBy.begin(), m.usedBy.end(), u.user) == m.usedBy.end())
                    m.usedBy.push_back (u.user);
                m.replacement = u.instrument ? "Built-in synth (a preset chosen by the channel name)" : "Nothing: the slot was left out of the mixer";
            }
        missingPluginDetails.push_back (std::move (m));
    }
    // missing, played by a LYNX substitute that keeps the original for when it is installed
    for (std::size_t t = 0; t < imported.mixer.size(); ++t)
        for (const auto& fx : imported.mixer[t].effects)
            if (fx.isSubstitute())
            {
                const auto name = substituteOriginalName (fx);
                auto it = std::find_if (missingPluginDetails.begin(), missingPluginDetails.end(),
                                        [&] (const MissingPlugin& m) { return m.name == name && ! m.replacementArg.empty(); });
                if (it == missingPluginDetails.end())
                    it = missingPluginDetails.insert (missingPluginDetails.end(),
                                                      MissingPlugin { name, {}, TR_NOOP ("LYNX substitute: {0} (the original plugin is used again once it is installed). FL Studio plays a missing plugin's slot unprocessed: a substitute slot's menu › Bypass all LYNX substitutes of this project does the same"),
                                                                      effectInfo (fx.kind).name });
                const auto user = userOf (imported, t);
                if (std::find (it->usedBy.begin(), it->usedBy.end(), user) == it->usedBy.end())
                    it->usedBy.push_back (user);
            }
    // found, but the project's settings are in the other format (VST2 <-> VST3): they play their default sound
    for (const auto& line : prepared.withoutState)
        missingPluginDetails.push_back ({ line.toStdString(), {}, TR_NOOP ("Its default settings: the project saved them for the other format (VST2/VST3)") });

    plugins.clearLoadFailures();
    loadtrace::stage (0.90, 0.99, TR_NOOP ("Building the mixer..."));
    {
        const loadtrace::Scope timing ("set_song");
        if (asEdit)
            editSong ([&imported] (Song& s) { s = std::move (imported); });
        else
        {
            stop();
            rewind();
            setSong (std::move (imported));
        }
    }
    plugins.clearPreloaded(); // (any left over: a slot the rack reused, nothing to keep)
    prepared.decoded.clear();

    // installed but failed to load (the rack was just built): silence / bypass, said as loudly as a missing one
    for (const auto& line : plugins.getLoadFailures())
        missingPluginDetails.push_back ({ line.toStdString(), {}, TR_NOOP ("Silence: the plugin is installed but did not load") });
    return true;
}

bool AudioEngine::importFlpFile (const juce::File& file, juce::String& error, const std::map<std::string, std::string>& relocations, bool asEdit)
{
    // a slow plugin module (a Waves shell) starts on its own thread while the samples decode (round 121)
    if (juce::MemoryBlock data; file.loadFileAsData (data))
        try
        {
            PluginHost::newOpen();
            plugins.startModulesFor (flpForeignPlugins (flp::parseProject ({ static_cast<const std::uint8_t*> (data.getData()), data.getSize() })));
        }
        catch (const flp::Error&)
        {
        }
    PreparedFlp prepared;
    prepareFlpImport (file, relocations, samples, prepared);
    if (! prepared.ok)
    {
        error = prepared.error;
        return false;
    }
    if (asEdit)
        keepRunningPlugins (prepared);
    loadtrace::stage (0.55, 0.90, TR_NOOP ("Loading plugins..."), TR_NOOP ("Loading {0}... {1}/{2}"));
    while (preloadNextPlugin (prepared))
    {
    }
    return finishFlpImport (std::move (prepared), asEdit);
}

void AudioEngine::keepRunningPlugins (PreparedFlp& prepared)
{
    // The open project imported again (Missing files › Apply): its plugins are already running with these settings,
    // and the rack keeps a slot whose plugin did not change. Creating them all again first (round 151: 71 plugins,
    // 25 s with the window frozen, then a crash inside a plugin as the second copies replaced the first) is skipped
    // for every plugin the song already has with the same settings.
    adoptPrepared (prepared);
    std::vector<const PluginRef*> running;
    for (const auto& c : song.channels)
        if (c.instrument == InstrumentKind::plugin && ! c.plugin.id.empty())
            running.push_back (&c.plugin);
    for (const auto& track : song.mixer)
        for (const auto& fx : track.effects)
            if (fx.kind == EffectKind::plugin && ! fx.plugin.id.empty())
                running.push_back (&fx.plugin);
    auto& queue = prepared.pluginQueue;
    queue.erase (std::remove_if (queue.begin(), queue.end(),
                                 [&running] (const PluginRef& ref) {
                                     const auto it = std::find_if (running.begin(), running.end(), [&ref] (const PluginRef* r) { return r != nullptr && *r == ref; });
                                     if (it == running.end())
                                         return false;
                                     *it = nullptr; // each running plugin stands for one queued copy
                                     return true;
                                 }),
                 queue.end());
}

void AudioEngine::publishSong()
{
    // diagnostics (round 149): with LYNX_TRACE_EDITS set, each publish's time goes to %APPDATA%\LYNX\edits.log
    static const bool traceEdits = std::getenv ("LYNX_TRACE_EDITS") != nullptr;
    const auto publishStart = juce::Time::getMillisecondCounterHiRes();
    const juce::ScopeGuard traceEnd { [&] {
        if (traceEdits)
            platform::settingsFile ("edits.log").appendText (juce::String (juce::Time::getMillisecondCounterHiRes() - publishStart, 1) + " ms publish\n");
    } };
    // the Master's hardware output target, read by the device callback
    masterOutChannel.store (song.mixer.empty() ? outputDefault : song.mixer[0].outputChannel, std::memory_order_relaxed);
    masterOutMode.store (song.mixer.empty() ? 0 : (int) song.mixer[0].outputMode, std::memory_order_relaxed);
    song.currentPattern = juce::jlimit (0, juce::jmax (0, (int) song.patterns.size() - 1), song.currentPattern);
    selectedChannel = juce::jlimit (0, juce::jmax (0, (int) song.channels.size() - 1), selectedChannel);
    {
        const auto count = (int) song.channels.size();
        selectedChannels.erase (std::remove_if (selectedChannels.begin(), selectedChannels.end(), [count] (int c) { return c >= count; }),
                                selectedChannels.end());
        if (std::find (selectedChannels.begin(), selectedChannels.end(), selectedChannel) == selectedChannels.end())
            selectedChannels.insert (std::lower_bound (selectedChannels.begin(), selectedChannels.end(), selectedChannel), selectedChannel);
    }
    publishSelection();
    transport.bpm.store (song.bpm);
    transport.beatsPerBar.store (song.beatsPerBar);

    {
        // time-signature table for the metronome: the start (beatsPerBar/4) and each signature change
        transport.signatureVersion.fetch_add (1, std::memory_order_acq_rel); // odd: writing
        int count = 0;
        transport.signatureTick[0].store (0, std::memory_order_relaxed);
        transport.signatureLength[0].store ((std::int64_t) ppq * juce::jmax (1, song.beatsPerBar), std::memory_order_relaxed);
        ++count;

        for (const auto& m : song.markers())
            if (m.isSignature() && count < Transport::maxSignatures)
            {
                const auto length = juce::jmax<std::int64_t> (1, (std::int64_t) ppq * 4 * m.numerator / juce::jmax (1, m.denominator));
                const auto slot = m.tick <= 0 ? 0 : count++; // at position 0 it replaces the default
                transport.signatureTick[(std::size_t) slot].store (juce::jmax<std::int64_t> (0, m.tick), std::memory_order_relaxed);
                transport.signatureLength[(std::size_t) slot].store (length, std::memory_order_relaxed);
            }

        transport.signatureCount.store (count, std::memory_order_relaxed);
        transport.signatureVersion.fetch_add (1, std::memory_order_acq_rel); // even: ready
    }
    transport.songMode.store (song.mode == PlayMode::song);
    transport.loopStart.store ((double) song.loopStart);
    transport.loopEnd.store ((double) song.loopEnd);
    transport.songEnd.store ((double) song.songLength());

    const loadtrace::Scope buildTiming ("rack_build");
    RackState::Factories factories;
    factories.samples = [this] (const std::string& path) {
        const loadtrace::Scope timing ("rack_samples");
        return samples.load (juce::String::fromUTF8 (path.c_str()));
    };
    factories.stretchedSamples = [this] (const std::string& path, double ratio, StretchMethod method) {
        const loadtrace::Scope timing ("rack_stretch");
        return samples.loadStretched (juce::String::fromUTF8 (path.c_str()), ratio, method);
    };
    factories.pluginInstruments = [this] (const PluginRef& ref, double sr) { return plugins.makeInstrument (ref, sr); };
    factories.pluginEffects = [this] (const PluginRef& ref, double sr) { return plugins.makeEffect (ref, sr); };

    const auto buildStart = juce::Time::getMillisecondCounterHiRes();
    const auto tracePhases = traceEdits && ! loadtrace::active(); // the build's phases (an open traces its own)
    if (tracePhases)
        loadtrace::begin ("edit");
    auto state = RackState::build (song, &lastPublishedShape, transport.sampleRate.load(), factories);
    if (traceEdits)
        platform::settingsFile ("edits.log").appendText (juce::String (juce::Time::getMillisecondCounterHiRes() - buildStart, 1) + " ms rack build\n"
                                                         + (tracePhases ? juce::String (loadtrace::reportLines()) : juce::String()));
    if (tracePhases)
        loadtrace::end();

    // what the next build needs to reuse instruments and effects
    lastPublishedShape.song.channels = song.channels;
    lastPublishedShape.song.mixer = song.mixer;
    // the tempo too (round 149): a channel that plays a time-stretched copy is kept when its stretch and the tempo
    // did not change; without it every edit rebuilt them (sample reloaded and stretch looked up, ~0.5 s on Thief)
    lastPublishedShape.song.bpm = song.bpm;
    lastPublishedShape.instruments = state->instruments;
    lastPublishedShape.effects = state->effects;
    lastPublishedShape.sampleRate = state->sampleRate;
    lastPublishedShape.latencies = state->latencies;
    lastPublishedShape.pdc = state->pdc;
    lastPublishedShape.channelDelays = state->channelDelays;
    lastPublishedShape.sendDelays = state->sendDelays;

    const auto mark = [&] (const char* what) {
        if (traceEdits)
            platform::settingsFile ("edits.log").appendText (juce::String (juce::Time::getMillisecondCounterHiRes() - publishStart, 1) + " ms " + what + "\n");
    };
    mark ("built");
    rackExchange.publish (std::move (state));
    mark ("exchanged");

    if (prefetcher != nullptr)
        prefetcher->update (song, samples);
    mark ("prefetch updated");

    sendChangeMessage();
}

std::vector<MidiLink> AudioEngine::getGlobalLinks() const
{
    std::vector<MidiLink> out;
    for (const auto& l : midiPrefs.globalLinks)
        out.push_back ({ l.cc, l.channel, { (AutomationTargetKind) l.kind, l.index, l.slot, l.param } });
    return out;
}

std::optional<MidiLink> AudioEngine::globalLinkOf (const AutomationTarget& target) const
{
    for (const auto& l : getGlobalLinks())
        if (l.target == target)
            return l;
    return std::nullopt;
}

void AudioEngine::learnGlobalLink (const AutomationTarget& target)
{
    learning = target;
    learningGlobal = true;
    sendChangeMessage();
}

void AudioEngine::removeGlobalLink (const AutomationTarget& target)
{
    auto p = midiPrefs;
    p.globalLinks.erase (std::remove_if (p.globalLinks.begin(), p.globalLinks.end(),
                                         [&] (const prefs::GlobalLink& l) {
                                             return l.kind == (int) target.kind && l.index == target.index && l.slot == target.slot
                                                 && l.param == target.param;
                                         }),
                         p.globalLinks.end());
    setMidiPrefs (p, saveGlobalLinks);
}

void AudioEngine::learnMidiController (const AutomationTarget& target)
{
    learning = target;
    learningGlobal = false;
    sendChangeMessage();
}

// --- last tweaked parameters and automation recording (REA round 143) --------------------------------------

void AudioEngine::noteTweaked (const AutomationTarget& target, bool held)
{
    if (target.kind == AutomationTargetKind::none)
        return;

    if (! (lastTweaked[0] == target))
    {
        lastTweaked[1] = lastTweaked[0];
        lastTweaked[0] = target;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    auto it = std::find_if (touches.begin(), touches.end(), [&] (const Touch& t) { return t.target == target; });
    if (it == touches.end())
    {
        touches.push_back ({ target, now, held, {} });
        return;
    }
    it->lastMs = now;
    it->held = it->held || held;
}

void AudioEngine::tweakReleased (const AutomationTarget& target)
{
    for (auto& t : touches)
        if (t.target == target)
        {
            t.held = false;
            t.lastMs = juce::Time::getMillisecondCounterHiRes();
        }
}

std::optional<AutomationTarget> AudioEngine::getLastTweaked (int which) const
{
    if (which < 0 || which > 1 || lastTweaked[(std::size_t) which].kind == AutomationTargetKind::none
        || currentTargetValue (lastTweaked[(std::size_t) which]) < 0.0f)
        return std::nullopt;
    return lastTweaked[(std::size_t) which];
}

float AudioEngine::currentTargetValue (const AutomationTarget& t) const
{
    const auto base = song.targetValue (t);
    if (base < 0.0f || (t.kind != AutomationTargetKind::channelParam && t.kind != AutomationTargetKind::effectParam))
        return base;

    juce::AudioPluginInstance* plugin = nullptr;
    float midiValue = -1.0f;
    if (t.kind == AutomationTargetKind::channelParam)
        if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument (t.index).get()))
        {
            plugin = &p->getInstance();
            midiValue = midictl::isMidiParam (t.param) ? p->midiControlValue (t.param) : -1.0f;
        }
    if (t.kind == AutomationTargetKind::effectParam)
        if (auto* p = dynamic_cast<PluginEffect*> (getEffect (t.index, t.slot).get()))
        {
            plugin = &p->getInstance();
            midiValue = midictl::isMidiParam (t.param) ? p->midiControlValue (t.param) : -1.0f;
        }

    if (midiValue >= 0.0f)
        return midiValue;
    if (plugin != nullptr && t.param >= 0 && t.param < plugin->getParameters().size())
        return plugin->getParameters()[t.param]->getValue();
    return base;
}

std::optional<AutomationTarget> AudioEngine::targetOfPlugin (const juce::AudioPluginInstance& plugin, int param) const
{
    for (int c = 0; c < (int) song.channels.size(); ++c)
        if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument (c).get()); p != nullptr && &p->getInstance() == &plugin)
            return AutomationTarget { AutomationTargetKind::channelParam, c, 0, param };

    for (int t = 0; t < (int) song.mixer.size(); ++t)
        for (int s = 0; s < (int) song.mixer[(std::size_t) t].effects.size(); ++s)
            if (auto* p = dynamic_cast<PluginEffect*> (getEffect (t, s).get()); p != nullptr && &p->getInstance() == &plugin)
                return AutomationTarget { AutomationTargetKind::effectParam, t, s, param };

    return std::nullopt;
}

void AudioEngine::updateAutomationRecording (bool active)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto recording = active && recordOptions.recordAutomation;
    const auto grid = recordOptions.quantiseAutomation ? recordOptions.quantise : std::int64_t { 0 };

    auto writeFor = [this] (const AutomationTarget& target) -> AutomationWrite& {
        auto it = std::find_if (takeWrites.begin(), takeWrites.end(), [&] (const AutomationWrite& w) { return w.target == target; });
        if (it != takeWrites.end())
            return *it;
        AutomationWrite w;
        w.target = target;
        w.name = nameAutomation ? nameAutomation (target) : std::string ("Automation");
        takeWrites.push_back (w);
        return takeWrites.back();
    };

    // writes what was sampled (one undo step for the whole take); `ended` = the gestures that are over
    auto commit = [&] (const std::vector<AutomationTarget>& ended) {
        if (std::none_of (touches.begin(), touches.end(), [] (const Touch& t) { return ! t.pending.empty(); }))
            return;
        const auto songMode = song.mode == PlayMode::song;
        editSong ([&] (Song& s) {
            for (auto& t : touches)
                if (! t.pending.empty())
                {
                    auto& w = writeFor (t.target);
                    recordedAutomationPoints += (int) t.pending.size();
                    if (songMode)
                        commitAutomationPoints (s, w, std::move (t.pending), grid);
                    else
                        commitEventPoints (s, automationPattern, w, std::move (t.pending), automationLoopLength, grid);
                    t.pending.clear();
                }
        }, ! automationTakeHasUndoStep);
        automationTakeHasUndoStep = true;
        for (const auto& target : ended)
            writeFor (target).lastLocal = -1; // the next gesture on it starts a new stretch
    };

    // FL: while recording, the moved control wins over its playing automation at once (REA round 146); the
    // automation of a touched target does not drive it until the gesture ends and its points are written.
    auto overrideTouched = [this] {
        if (rackProcessor == nullptr)
            return;
        std::vector<int> slots, one;
        for (const auto& t : touches)
        {
            RackState::slotsOfTarget (song, t.target, one);
            slots.insert (slots.end(), one.begin(), one.end());
        }
        if (slots.empty() && ! automationOverrideActive)
            return;
        rackProcessor->getRenderer().setOverriddenSlots (slots);
        automationOverrideActive = ! slots.empty();
    };

    if (! recording)
    {
        commit ({});
        touches.clear();
        overrideTouched();
        if (! active)
        {
            takeWrites.clear();
            automationTakeHasUndoStep = false;
            automationLoopLength = 0;
        }
        return;
    }

    if (automationLoopLength <= 0)
    {
        automationPattern = song.currentPattern;
        automationLoopLength = song.patterns.empty() ? ppq * 4
                                                     : song.patterns[(std::size_t) juce::jlimit (0, (int) song.patterns.size() - 1, song.currentPattern)]
                                                           .effectiveLength (song.beatsPerBar);
    }

    const auto tick = estimatedPositionNow();
    std::vector<AutomationTarget> ended;
    for (auto& t : touches)
    {
        const auto alive = t.held || now - t.lastMs < 350.0;
        const auto value = currentTargetValue (t.target);
        if (value < 0.0f)
        {
            ended.push_back (t.target);
            continue;
        }
        if (alive)
            t.pending.push_back ({ tick, value });
        else
            ended.push_back (t.target);
    }

    if (! ended.empty() || --automationCommitCountdown <= 0)
    {
        commit (ended);
        automationCommitCountdown = 6; // about 5 writes per second while a control keeps moving
    }

    touches.erase (std::remove_if (touches.begin(), touches.end(),
                                   [&] (const Touch& t) { return std::find (ended.begin(), ended.end(), t.target) != ended.end(); }),
                   touches.end());
    overrideTouched();
}

void AudioEngine::applyControllers()
{
    ControllerEvent e;
    std::map<std::pair<int, int>, int> latest; // (cc, channel) -> latest value this frame

    while (controllerQueue.pop (e))
    {
        if (learning.kind != AutomationTargetKind::none && learningGlobal)
        {
            // "Override global link...": the link goes to the settings, for every project (REA round 146)
            auto p = midiPrefs;
            const prefs::GlobalLink g { e.cc, e.channel, (int) learning.kind, learning.index, learning.slot, learning.param };
            p.globalLinks.erase (std::remove_if (p.globalLinks.begin(), p.globalLinks.end(),
                                                 [&] (const prefs::GlobalLink& l) {
                                                     return l.kind == g.kind && l.index == g.index && l.slot == g.slot && l.param == g.param;
                                                 }),
                                 p.globalLinks.end());
            p.globalLinks.push_back (g);
            learning = {};
            learningGlobal = false;
            setMidiPrefs (p, saveGlobalLinks);
        }
        else if (learning.kind != AutomationTargetKind::none)
        {
            // MIDI learn: replace the old link for the same target with the new one
            editSong ([t = learning, e] (Song& s) {
                s.midiLinks.erase (std::remove_if (s.midiLinks.begin(), s.midiLinks.end(), [&] (const MidiLink& l) { return l.target == t; }),
                                   s.midiLinks.end());
                s.midiLinks.push_back ({ e.cc, e.channel, t });
            });
            learning = {};
        }

        latest[{ e.cc, e.channel }] = e.value;
    }

    const auto links = linksWithGlobal (song.midiLinks, getGlobalLinks());
    if (latest.empty() || links.empty())
        return;

    // one snapshot per frame, outside the undo history (like turning the knob)
    std::vector<std::pair<AutomationTarget, float>> changes;

    for (const auto& [key, value] : latest)
        for (const auto& link : links)
            if (link.cc == key.first && (link.channel < 0 || link.channel == key.second))
            {
                const auto v = value / 127.0f;
                const auto& t = link.target;
                // takeover: only parameters the song holds (a plugin's own parameters have no value to compare here)
                const auto songHeld = t.kind != AutomationTargetKind::channelParam && t.kind != AutomationTargetKind::effectParam;
                if (midiPrefs.pickup && songHeld)
                {
                    auto& state = pickups[{ key.first, key.second, (int) t.kind, t.index, t.slot, t.param }];
                    if (! prefs::pickupUpdate (state, song.targetValue (t), v))
                        continue;
                }
                changes.push_back ({ t, v });
                noteTweaked (t); // a linked controller's move is a tweak (Last tweaked, automation recording)
            }

    std::vector<std::pair<AutomationTarget, float>> pluginChanges;

    editSong ([&] (Song& s) {
        for (const auto& [target, v] : changes)
            if (! s.setTargetValue (target, v))
                pluginChanges.push_back ({ target, v });
    }, false);

    // plugin parameters live in the plugin
    for (const auto& [target, v] : pluginChanges)
    {
        if (target.kind == AutomationTargetKind::channelParam)
        {
            if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument (target.index).get()))
                p->setParameterFromUi (target.param, v);
            // a MIDI Out's controls: the plugins on its port (REA_rodada133 §4)
            if (target.index >= 0 && target.index < (int) song.channels.size()
                && song.channels[(std::size_t) target.index].instrument == InstrumentKind::midiOut)
            {
                const auto port = midictl::parseMidiOut (song.channels[(std::size_t) target.index].pluginState).port;
                for (int c = 0; c < (int) song.channels.size(); ++c)
                    if (song.channels[(std::size_t) c].instrument == InstrumentKind::plugin && song.channels[(std::size_t) c].plugin.midiInPort == port)
                        if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument (c).get()))
                            p->setParameterFromUi (target.param, v);
            }
        }

        if (target.kind == AutomationTargetKind::effectParam)
            if (auto* p = dynamic_cast<PluginEffect*> (getEffect (target.index, target.slot).get()))
                p->setParameterFromUi (target.param, v);
    }
}

void AudioEngine::timerCallback()
{
    // MIDI inputs: open the enabled devices that appeared (or were not ready) and reopen the ones that came back
    if (prefetcher != nullptr && ++midiCheckTicks >= 60) // every 2 s
    {
        midiCheckTicks = 0;
        enableAllMidiInputs();
    }

    updateRealtimeStats();
    rt::promotePending(); // Linux: real-time priority for the audio threads seen since the last tick (round 161)
    applyControllers();
    updateRecording();
    rackExchange.collectGarbage();

    // Plugins are prepared at a fixed rate; if the device changed rate, recreate them.
    // A plugin whose latency changed (preset change, lookahead) requests a new PDC plan.
    const auto rateChanged = std::abs (transport.sampleRate.load() - lastPublishedShape.sampleRate) > 0.5;
    const auto latencies = lastPublishedShape.currentLatencies();
    if (rateChanged || latencies != lastPublishedShape.latencies)
    {
        // (diagnostics: which unit's latency moved)
        if (! rateChanged)
            for (std::size_t i = 0; i < latencies.size() && i < lastPublishedShape.latencies.size(); ++i)
                if (latencies[i] != lastPublishedShape.latencies[i])
                {
                    lastLatencyChange = juce::String ((int) i) + ": " + juce::String (lastPublishedShape.latencies[i]) + " -> " + juce::String (latencies[i]);
                    break;
                }
        ++latencyRepublishes;
        publishSong();
    }
}

void AudioEngine::choosePreferredDriver()
{
   #if JUCE_LINUX || JUCE_BSD
    std::vector<std::pair<std::string, bool>> types;
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        types.emplace_back (type->getTypeName().toStdString(), ! type->getDeviceNames (false).isEmpty());
    }
    const auto best = juce::String (prefs::preferredLinuxDriver (types));
    if (best.isNotEmpty() && best != deviceManager.getCurrentAudioDeviceType())
    {
        deviceManager.setCurrentAudioDeviceType (best, true);
        if (deviceManager.getCurrentAudioDevice() == nullptr) // it would not open: back to what worked
            deviceManager.initialise (0, 2, nullptr, true);
    }
   #endif
}

void AudioEngine::chooseSafeBufferSize()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;
    std::vector<int> sizes;
    for (auto b : device->getAvailableBufferSizes())
        sizes.push_back (b);
    const auto current = device->getCurrentBufferSizeSamples();
    const auto target = prefs::safeBufferSize (sizes, current, device->getCurrentSampleRate());
    if (target > 0 && target != current)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.bufferSize = target;
        deviceManager.setAudioDeviceSetup (setup, true);
    }
}

juce::File AudioEngine::glitchLogFile() { return PluginHost::settingsFolder().getChildFile ("audio-glitches.log"); }

void AudioEngine::updateRealtimeStats()
{
    // the message thread's pulse: this 30 Hz timer arriving late means the UI is saturated
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (lastUiPulseMs > 0.0)
            uiPulse.maxGapMs = juce::jmax (uiPulse.maxGapMs, now - lastUiPulseMs);
        lastUiPulseMs = now;
        ++uiPulse.calls;
    }

    // every ~2 s: the share of real time each channel / insert effect took in that window
    if (rackProcessor != nullptr && juce::Time::getMillisecondCounterHiRes() - lastUnitsWindowMs >= 2000.0)
    {
        lastUnitsWindowMs = juce::Time::getMillisecondCounterHiRes();
        auto& renderer = rackProcessor->getRenderer();
        const auto& units = renderer.unitTimes();
        const auto processed = units.processedSamples.load (std::memory_order_relaxed);
        const auto rate = juce::jmax (1.0, transport.sampleRate.load());
        const auto audioNs = (double) (processed - std::min (processed, lastProcessedSamples)) / rate * 1.0e9;
        lastProcessedSamples = processed;
        lastChannelNs.resize (units.channelNs.size(), 0);
        lastEffectNs.resize (units.effectNs.size(), 0);
        std::vector<UnitLoad> loads;

        auto take = [&] (const std::atomic<std::uint64_t>& total, std::uint64_t& last, std::atomic<std::uint64_t>& longest, auto&& name) {
            const auto now = total.load (std::memory_order_relaxed);
            const auto delta = now - std::min (now, last);
            last = now;
            const auto longestNs = const_cast<std::atomic<std::uint64_t>&> (longest).exchange (0, std::memory_order_relaxed);
            if (delta > 0 && audioNs > 0.0)
                loads.push_back ({ name(), (double) delta / audioNs, (double) longestNs * 1.0e-6 });
        };
        auto& mutableUnits = const_cast<RackRenderer::UnitTimes&> (units);
        for (std::size_t c = 0; c < units.channelNs.size(); ++c)
            take (units.channelNs[c], lastChannelNs[c], mutableUnits.channelMaxNs[c], [&] {
                return c < song.channels.size() ? juce::String::fromUTF8 (song.channels[c].name.c_str()) : "Channel " + juce::String ((int) c + 1);
            });
        for (std::size_t k = 0; k < units.effectNs.size(); ++k)
            take (units.effectNs[k], lastEffectNs[k], mutableUnits.effectMaxNs[k], [&] {
                const auto t = k / (std::size_t) maxEffectSlots, slot = k % (std::size_t) maxEffectSlots;
                juce::String effect;
                if (t < song.mixer.size() && slot < song.mixer[t].effects.size())
                {
                    const auto& fx = song.mixer[t].effects[slot];
                    effect = juce::String::fromUTF8 (fx.plugin.name.empty() ? effectInfo (fx.kind).name : fx.plugin.name.c_str());
                }
                const auto track = t == 0 ? juce::String ("Master") : (t < song.mixer.size() && ! song.mixer[t].name.empty() ? juce::String::fromUTF8 (song.mixer[t].name.c_str())
                                                                                                               : "Insert " + juce::String ((int) t));
                return track + " / " + juce::String ((int) slot + 1) + " " + effect;
            });
        std::sort (loads.begin(), loads.end(), [] (const UnitLoad& a, const UnitLoad& b) { return a.load > b.load; });
        if (loads.size() > 8)
            loads.resize (8);
        if (audioNs > 0.0)
            heaviestUnits = std::move (loads);
    }

    // a line in the glitch log when callbacks ran over their deadline or the device called late
    const auto stats = rtStats.snapshot();
    if ((stats.overBudget > loggedOver || stats.lateCalls > loggedLate) && juce::Time::getMillisecondCounterHiRes() - lastLogMs > 2000.0)
    {
        const auto file = glitchLogFile();
        if (file.getSize() > 1024 * 1024)
            file.moveFileTo (file.withFileExtension ("old.log"));
        const auto bar = transport.positionTicks.load() / (double) (ppq * juce::jmax (1, song.beatsPerBar)) + 1.0;
        file.appendText (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") + "  over deadline +" + juce::String ((juce::int64) (stats.overBudget - loggedOver))
                             + ", late calls +" + juce::String ((juce::int64) (stats.lateCalls - loggedLate)) + ", at bar " + juce::String (bar, 2)
                             + (transport.playing.load() ? "" : " (stopped)") + "\n" + describeRealtime() + "\n",
                         false, false, "\r\n");
        loggedOver = stats.overBudget;
        loggedLate = stats.lateCalls;
        lastLogMs = juce::Time::getMillisecondCounterHiRes();
    }
}

juce::String AudioEngine::describeRealtime (bool withUnits) const
{
    juce::String text;
    if (auto* device = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentAudioDevice())
    {
        const auto rate = device->getCurrentSampleRate();
        const auto buffer = device->getCurrentBufferSizeSamples();
        text << "  device: " << device->getTypeName() << " / " << device->getName() << ", " << (int) rate << " Hz, buffer " << buffer
             << " samples (deadline " << juce::String (prefs::bufferLatencyMs (buffer, rate), 2) << " ms), output latency "
             << device->getOutputLatencyInSamples() << " samples, driver dropouts " << device->getXRunCount() << "\n";
       #if JUCE_LINUX
        text << "  audio thread: " << (rt::status() == rt::Status::unknown ? juce::String ("not seen yet") : rt::describe()) << "\n";
       #endif
    }
    else
        text << "  device: none\n";
    const auto s = rtStats.snapshot();
    text << "  callbacks " << (juce::int64) s.callbacks << ": over deadline " << (juce::int64) s.overBudget << ", 80-100% " << (juce::int64) s.nearBudget
         << ", late device calls " << (juce::int64) s.lateCalls << ", average load " << juce::String (s.averageLoad * 100.0, 1) << "%, max "
         << juce::String (s.maxLoad * 100.0, 0) << "%, workers " << (workers != nullptr ? workers->workers() : 0) << "\n";
    if (latencyRepublishes > 0)
        text << "  latency changes re-planned PDC " << latencyRepublishes << " times (last: unit " << lastLatencyChange << ")\n";
    if (withUnits && heaviestUnits.empty())
        text << "  heaviest: not measured yet\n";
    if (withUnits && ! heaviestUnits.empty())
    {
        text << "  heaviest:";
        for (std::size_t i = 0; i < heaviestUnits.size() && i < 5; ++i)
            text << (i > 0 ? ";" : "") << " " << heaviestUnits[i].name << " " << juce::String (heaviestUnits[i].load * 100.0, 1) << "% (peak "
                 << juce::String (heaviestUnits[i].longestMs, 1) << " ms)";
        text << "\n";
    }
    return text;
}

std::shared_ptr<Instrument> AudioEngine::getChannelInstrument (int channel) const
{
    if (channel < 0 || channel >= (int) lastPublishedShape.instruments.size())
        return nullptr;

    return lastPublishedShape.instruments[(std::size_t) channel];
}

std::shared_ptr<Effect> AudioEngine::getEffect (int track, int slot) const
{
    const auto& effects = lastPublishedShape.effects;

    if (track < 0 || track >= (int) effects.size() || slot < 0 || slot >= (int) effects[(std::size_t) track].size())
        return nullptr;

    return effects[(std::size_t) track][(std::size_t) slot];
}

void AudioEngine::setPluginsRenderMode (bool offline)
{
    // FL's "Notify about rendering mode" off: that plugin is not told (round 149); "Reset plugin when FL Studio
    // resets": reset as the export starts (round 151)
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument ((int) i).get()))
        {
            if (song.channels[i].plugin.notifyRender)
                p->setRenderMode (offline);
            if (offline)
                p->resetForExport();
        }

    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            if (auto* p = dynamic_cast<PluginEffect*> (getEffect ((int) t, (int) s).get()))
            {
                if (song.mixer[t].effects[s].plugin.notifyRender)
                    p->setRenderMode (offline);
                if (offline)
                    p->resetForExport();
            }

    // the plugins inside Patchers (REA_rodada141)
    std::function<void (patcher::Graph*)> graphMode = [&] (patcher::Graph* graph) {
        if (graph == nullptr)
            return;
        for (int n = 0; n < graph->nodeCount(); ++n)
        {
            auto effect = graph->effectAt (n);
            auto instrument = graph->instrumentAt (n);
            if (auto* p = dynamic_cast<PluginEffect*> (effect.get()))
                p->setRenderMode (offline);
            if (auto* p = dynamic_cast<PluginInstrument*> (instrument.get()))
                p->setRenderMode (offline);
            graphMode (patcher::graphOf (effect.get()));
            graphMode (patcher::graphOf (instrument.get()));
        }
    };
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        graphMode (patcher::graphOf (getChannelInstrument ((int) i).get()));
    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            graphMode (patcher::graphOf (getEffect ((int) t, (int) s).get()));
}

void AudioEngine::capturePluginStates()
{
    // Changes only the saved state (recreates nothing and doesn't enter the undo history).
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        if (auto* p = dynamic_cast<PluginInstrument*> (getChannelInstrument ((int) i).get()))
            if (auto state = p->captureState()) // (a plugin that crashed keeps the state saved before)
                song.channels[i].plugin.state = std::move (*state);

    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            if (auto* p = dynamic_cast<PluginEffect*> (getEffect ((int) t, (int) s).get()))
                if (auto state = p->captureState())
                    song.mixer[t].effects[s].plugin.state = std::move (*state);

    // the plugins inside Patchers (REA_rodada141), nested ones too
    std::function<void (PatcherDoc&, patcher::Graph*)> captureGraph = [&] (PatcherDoc& doc, patcher::Graph* graph) {
        if (graph == nullptr)
            return;
        for (int n = 0; n < (int) doc.nodes.size() && n < graph->nodeCount(); ++n)
        {
            auto& node = doc.nodes[(std::size_t) n];
            if (node.generator)
            {
                auto instrument = graph->instrumentAt (n);
                if (auto* p = dynamic_cast<PluginInstrument*> (instrument.get()))
                    if (auto state = p->captureState())
                        node.channel.plugin.state = std::move (*state);
                if (node.channel.instrument == InstrumentKind::patcher)
                    captureGraph (node.channel.patcher, patcher::graphOf (instrument.get()));
            }
            else
            {
                auto effect = graph->effectAt (n);
                if (auto* p = dynamic_cast<PluginEffect*> (effect.get()))
                    if (auto state = p->captureState())
                        node.effect.plugin.state = std::move (*state);
                if (node.effect.kind == EffectKind::patcher)
                    captureGraph (node.effect.patcher, patcher::graphOf (effect.get()));
            }
        }
    };
    for (std::size_t i = 0; i < song.channels.size(); ++i)
        if (song.channels[i].instrument == InstrumentKind::patcher)
            captureGraph (song.channels[i].patcher, patcher::graphOf (getChannelInstrument ((int) i).get()));
    for (std::size_t t = 0; t < song.mixer.size(); ++t)
        for (std::size_t s = 0; s < song.mixer[t].effects.size(); ++s)
            if (song.mixer[t].effects[s].kind == EffectKind::patcher)
                captureGraph (song.mixer[t].effects[s].patcher, patcher::graphOf (getEffect ((int) t, (int) s).get()));

    lastPublishedShape.song.channels = song.channels;
    lastPublishedShape.song.mixer = song.mixer;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext& context)
{
    // FTZ/DAZ for the whole callback (round 160): the rack had it, but the metronome, the preview player and the
    // hosted plugins' graph nodes around it ran without
    const ScopedFlushDenormals noDenormals;
    rt::noteAudioThread(); // Linux: the message thread asks for real-time priority for this thread once
    // real-time health: how long this callback took against its deadline (the buffer's duration)
    const auto callbackStart = juce::Time::getMillisecondCounterHiRes();
    struct Measure
    {
        ~Measure()
        {
            const auto rate = engine.transport.sampleRate.load (std::memory_order_relaxed);
            if (rate > 0.0)
                engine.rtStats.record (start, juce::Time::getMillisecondCounterHiRes() - start, 1000.0 * samples / rate);
        }
        AudioEngine& engine;
        double start;
        int samples;
    } measure { *this, callbackStart, numSamples };

    if (transport.rewindRequested.exchange (false))
        transport.positionTicks.store (0.0);

    if (const auto target = transport.seekRequest.exchange (-1.0); target >= 0.0)
        transport.positionTicks.store (target);

    // With the loop region on, the block is split at the loop end and the position jumps back to
    // its start (the nodes see a jump and release their notes, as in FL).
    // the audio editor's input recording (nothing happens unless it chose the input)
    meters.capture.push (AudioCapture::input, numInputChannels > 0 ? inputChannelData[0] : nullptr, numInputChannels > 1 ? inputChannelData[1] : nullptr,
                         numSamples);

    std::array<const float*, 64> inputs {};
    std::array<float*, 64> outputs {};
    numInputChannels = juce::jmin (numInputChannels, (int) inputs.size());
    numOutputChannels = juce::jmin (numOutputChannels, (int) outputs.size());

    // count-in: the metronome counts the pre-roll down, then playback starts
    if (const auto countIn = transport.countIn.load (std::memory_order_relaxed); countIn > 0.0 && ! transport.playing.load (std::memory_order_relaxed))
    {
        const auto left = countIn - numSamples * transport.ticksPerSample();
        transport.countIn.store (juce::jmax (0.0, left), std::memory_order_relaxed);

        if (left <= 0.0)
            transport.playing.store (true, std::memory_order_relaxed);
    }

    for (int done = 0; done < numSamples;)
    {
        auto n = numSamples - done;
        const auto playing = transport.playing.load (std::memory_order_relaxed);
        const auto tps = transport.ticksPerSample();
        // Song mode: the loop region, or else the whole song (back to the start at its end, like FL)
        // (while audio takes record in Song mode without a loop region, the song plays on past its end instead of
        // starting over: the take stays one piece, as in Pro Tools / Nuendo; round 163)
        const auto linear = liveInput.linearSong.load (std::memory_order_relaxed);
        const auto loop = playbackLoop (transport.songMode.load (std::memory_order_relaxed), transport.loopStart.load (std::memory_order_relaxed),
                                        transport.loopEnd.load (std::memory_order_relaxed), linear ? 0.0 : transport.songEnd.load (std::memory_order_relaxed));
        const auto loopStart = loop.start, loopEnd = loop.end;
        const auto looping = playing && loop.active() && tps > 0.0;

        if (looping)
        {
            // before the loop, play normally until reaching it; past the end, jump back to the start
            auto position = transport.positionTicks.load (std::memory_order_relaxed);

            if (position >= loopEnd)
            {
                position = loopStart;
                transport.positionTicks.store (position, std::memory_order_relaxed);
            }

            n = juce::jlimit (1, n, (int) std::ceil ((loopEnd - position) / tps));
        }

        for (int ch = 0; ch < numInputChannels; ++ch)
            inputs[(std::size_t) ch] = inputChannelData[ch] != nullptr ? inputChannelData[ch] + done : nullptr;

        // the block's input by device channel, for the inserts (monitoring, meters) and the takes
        liveInput.numChannels = 0;
        for (int c = 0; c < LiveInput::maxChannels; ++c)
        {
            const auto slot = inputSlot[(std::size_t) c];
            liveInput.channels[(std::size_t) c] = slot >= 0 && slot < numInputChannels ? inputs[(std::size_t) slot] : nullptr;
            if (liveInput.channels[(std::size_t) c] != nullptr)
                liveInput.numChannels = c + 1;
        }

        // audio recording: this block's input goes to the takes' streams (without waiting on a lock). Every take gets
        // every block (silence for an input channel missing now), so all of them keep the same timeline (round 163).
        if (playing && transport.recording.load (std::memory_order_relaxed))
        {
            const juce::SpinLock::ScopedTryLockType lock (takeLock);

            if (lock.isLocked() && takeCount > 0)
            {
                for (int k = 0; k < takeCount; ++k)
                {
                    auto* take = takes[(std::size_t) k];
                    const std::array<const float*, 2> data { liveInput.channel (take->first, 0), liveInput.channel (take->first + take->channels - 1, 0) };
                    if (data[0] == nullptr || data[1] == nullptr)
                        take->stream->pushSilence (n);
                    else
                        take->stream->push (data.data(), n);
                }

                if (takeStartTick.load (std::memory_order_relaxed) < 0.0)
                    takeStartTick.store (transport.positionTicks.load (std::memory_order_relaxed));
                takeFrames.fetch_add (n, std::memory_order_relaxed);
                takeTicks.store (takeTicks.load (std::memory_order_relaxed) + n * tps, std::memory_order_relaxed);
            }
        }

        for (int ch = 0; ch < numOutputChannels; ++ch)
            outputs[(std::size_t) ch] = outputChannelData[ch] != nullptr ? outputChannelData[ch] + done : nullptr;

        player.audioDeviceIOCallbackWithContext (inputs.data(), numInputChannels, outputs.data(), numOutputChannels, n, context);
        routeOutputs (outputs.data(), numOutputChannels, n);

        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputs[(std::size_t) ch] != nullptr)
                clipOvers (outputs[(std::size_t) ch], n);

        // oscilloscope: average of both sides into the ring buffer
        if (numOutputChannels > 0 && outputs[0] != nullptr)
        {
            auto w = scopeWrite.load (std::memory_order_relaxed);
            const auto* right = numOutputChannels > 1 && outputs[1] != nullptr ? outputs[1] : outputs[0];

            for (int i = 0; i < n; ++i)
            {
                scope[(std::size_t) w].store (0.5f * (outputs[0][i] + right[i]), std::memory_order_relaxed);
                w = (w + 1) % scopeSize;
            }

            scopeWrite.store (w, std::memory_order_release);
        }

        if (playing)
        {
            auto position = transport.positionTicks.load (std::memory_order_relaxed) + n * tps;

            if (looping && position >= loopEnd - 1e-6)
            {
                position = loopStart;
                // loop recording: where this pass ends in the takes (round 163)
                if (const auto frames = takeFrames.load (std::memory_order_relaxed); frames > 0 && takeStartTick.load (std::memory_order_relaxed) >= 0.0)
                    if (const auto k = takeWrapCount.load (std::memory_order_relaxed); k < (int) takeWraps.size())
                    {
                        takeWraps[(std::size_t) k].store (frames, std::memory_order_relaxed);
                        takeWrapCount.store (k + 1, std::memory_order_release);
                    }
            }

            transport.positionTicks.store (position, std::memory_order_relaxed);
        }

        done += n;
    }

    // the latency ping (round 163): only its clicks go out (no feedback through a monitored input), listened for on
    // its input
    if (ping.isRunning())
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
        std::array<float*, 2> clickOut {};
        for (std::size_t k = 0; k < 2; ++k)
            if (pingOutputs[k] >= 0 && pingOutputs[k] < numOutputChannels)
                clickOut[k] = outputChannelData[pingOutputs[k]];
        const auto* in = pingInput >= 0 && pingInput < numInputChannels ? inputChannelData[pingInput] : nullptr;
        ping.process (in, clickOut.data(), 2, numSamples);
    }

    // the block is done: the workers sleep until the next one instead of spinning (REA round 116)
    if (workers != nullptr)
        workers->park();

    blockTimeMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    rtStats.deviceRestarted();
    rt::resetForNewDevice();
    transport.sampleRate.store (device->getCurrentSampleRate());
    // FL's fixed size buffers: 2 ms, or the device buffer with "Process maximum size buffers" (REA round 134)
    setFixedPluginBlock (device->getCurrentBufferSizeSamples(), device->getCurrentSampleRate());

    // the callback gets the open input channels only, in order: map device channel -> callback index
    const auto active = device->getActiveInputChannels();
    int slot = 0;
    for (int c = 0; c < LiveInput::maxChannels; ++c)
        inputSlot[(std::size_t) c] = active[c] ? slot++ : -1;
    // the same for the outputs (hardware output targets, REA round 156)
    const auto activeOut = device->getActiveOutputChannels();
    slot = 0;
    for (int c = 0; c < HardwareOutput::maxChannels; ++c)
        outputSlot[(std::size_t) c] = activeOut[c] ? slot++ : -1;
    hwOutput.clear();
    hwOutput.active.store (true);
    player.audioDeviceAboutToStart (device);
}

void AudioEngine::audioDeviceStopped()
{
    hwOutput.active.store (false);
    player.audioDeviceStopped();
}

// The Master's own target (its default: the graph's output as it is, on the first open pair) and the inserts'
// hardware outputs, added into the device channels they chose. A channel that is not open drops its sound.
void AudioEngine::routeOutputs (float* const* outputs, int numOutputs, int n) noexcept
{
    const auto slotOf = [this] (int c) { return c >= 0 && c < HardwareOutput::maxChannels ? outputSlot[(std::size_t) c] : -1; };

    if (const auto channel = masterOutChannel.load (std::memory_order_relaxed); channel != outputDefault && numOutputs > 0 && n <= HardwareOutput::capacity)
    {
        auto* l = masterScratch.data();
        auto* r = masterScratch.data() + HardwareOutput::capacity;
        const auto* srcR = numOutputs > 1 && outputs[1] != nullptr ? outputs[1] : outputs[0];
        if (outputs[0] == nullptr)
            return;
        std::copy (outputs[0], outputs[0] + n, l);
        std::copy (srcR, srcR + n, r);
        for (int k = 0; k < std::min (numOutputs, 2); ++k)
            if (outputs[k] != nullptr)
                std::fill (outputs[k], outputs[k] + n, 0.0f);

        const auto mode = (OutputMode) masterOutMode.load (std::memory_order_relaxed);
        const auto [first, count] = outputChannelsOf (channel, mode);
        if (count > 0)
        {
            const auto a = slotOf (first), b = count > 1 ? slotOf (first + 1) : -1;
            if (a >= 0 && a < numOutputs)
                addToOutput (l, r, mode, outputs[a], b >= 0 && b < numOutputs ? outputs[b] : nullptr, n);
        }
    }

    hwOutput.drainInto (outputs, numOutputs, slotOf, n);
}

std::vector<std::string> AudioEngine::getOutputChannelNames() const
{
    std::vector<std::string> names;

    if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr)
        for (const auto& n : device->getOutputChannelNames())
            if ((int) names.size() < HardwareOutput::maxChannels)
                names.push_back (n.toStdString());

    return names;
}

bool AudioEngine::isOutputChannelOpen (int channel) const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr && channel >= 0 && device->getActiveOutputChannels()[channel];
}

void AudioEngine::setTrackOutput (int track, int channel, OutputMode mode)
{
    if (track < 0 || track >= (int) song.mixer.size())
        return;

    if (track > 0 && channel < 0) // an insert without a target: the default (the Master only), nothing saved
        channel = outputDefault;

    editSong ([=] (Song& s) {
        s.mixer[(std::size_t) track].outputChannel = channel;
        s.mixer[(std::size_t) track].outputMode = mode;
    });

    openTargetedOutputs();
}

void AudioEngine::openTargetedOutputs()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    const auto available = juce::jmin ((int) HardwareOutput::maxChannels, device->getOutputChannelNames().size());
    auto setup = deviceManager.getAudioDeviceSetup();
    auto wanted = device->getActiveOutputChannels();
    const auto before = wanted;

    for (const auto& t : song.mixer)
    {
        const auto [first, count] = outputChannelsOf (t.outputChannel, t.outputMode);
        for (int c = first; c < first + count; ++c)
            if (c < available)
                wanted.setBit (c);
    }

    if (wanted == before)
        return;

    setup.useDefaultOutputChannels = false;
    setup.outputChannels = wanted;
    deviceManager.setAudioDeviceSetup (setup, true);
    sendChangeMessage();
}
} // namespace lynx
