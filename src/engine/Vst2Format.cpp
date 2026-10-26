#include "engine/Vst2Format.h"

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include <juce_gui_extra/juce_gui_extra.h>

#include "core/FlWrapper.h"
#include "platform/NativeEmbed.h"

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_LINUX || JUCE_BSD
 #include <dlfcn.h>
#endif

namespace lynx
{
namespace
{
// --- the VST 2.4 binary interface (our own declarations of the public ABI) ---------------------------
namespace abi
{
struct Effect;
using HostCallback = std::intptr_t (*) (Effect*, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt);
using Dispatcher = std::intptr_t (*) (Effect*, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt);
using Process = void (*) (Effect*, float** inputs, float** outputs, std::int32_t frames);
using SetParameter = void (*) (Effect*, std::int32_t index, float value);
using GetParameter = float (*) (Effect*, std::int32_t index);
using Entry = Effect* (*) (HostCallback);

struct Effect
{
    std::int32_t magic;
    Dispatcher dispatcher;
    Process process; // accumulating (deprecated)
    SetParameter setParameter;
    GetParameter getParameter;
    std::int32_t numPrograms, numParams, numInputs, numOutputs;
    std::int32_t flags;
    std::intptr_t hostReserved1, hostReserved2;
    std::int32_t initialDelay;
    std::int32_t realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;
    std::int32_t uniqueId, version;
    Process processReplacing;
    void* processDoubleReplacing;
    char future[56];
};

constexpr std::int32_t magic = 0x56737450; // 'VstP'

// Effect::flags
constexpr std::int32_t hasEditor = 1 << 0, canReplacing = 1 << 4, programChunks = 1 << 5, isSynth = 1 << 8;

// dispatcher opcodes
enum : std::int32_t
{
    open = 0, close = 1, setProgram = 2, getProgram = 3, getProgramName = 5, getParamLabel = 6, getParamDisplay = 7,
    getParamName = 8, setSampleRate = 10, setBlockSize = 11, mainsChanged = 12, editGetRect = 13, editOpen = 14,
    editClose = 15, editIdle = 19, getChunk = 23, setChunk = 24, processEvents = 25, getProgramNameIndexed = 29,
    getPlugCategory = 35, getEffectName = 45, getVendorString = 47, getProductString = 48, getVendorVersion = 49,
    canDo = 51, getTailSize = 52, getVstVersion = 58, beginSetProgram = 67, endSetProgram = 68,
    shellGetNextPlugin = 70, startProcess = 71, stopProcess = 72
};

// host callback opcodes
enum : std::int32_t
{
    automate = 0, version = 1, currentId = 2, idle = 3, wantMidi = 6, getTime = 7, hostProcessEvents = 8,
    ioChanged = 13, sizeWindow = 15, getSampleRate = 16, getBlockSize = 17, getInputLatency = 18,
    getOutputLatency = 19, getCurrentProcessLevel = 23, getAutomationState = 24, hostGetVendorString = 32,
    hostGetProductString = 33, hostGetVendorVersion = 34, hostCanDo = 37, getLanguage = 38, getDirectory = 41,
    updateDisplay = 42, beginEdit = 43, endEdit = 44
};

constexpr std::int32_t categorySynth = 2, categoryShell = 10;

struct MidiEvent // 32 bytes
{
    std::int32_t type = 1; // MIDI
    std::int32_t byteSize = 32;
    std::int32_t deltaFrames = 0;
    std::int32_t flags = 1; // real-time
    std::int32_t noteLength = 0, noteOffset = 0;
    char data[4] {};
    char detune = 0, noteOffVelocity = 0, reserved1 = 0, reserved2 = 0;
};
static_assert (sizeof (MidiEvent) == 32);

struct Rect
{
    std::int16_t top, left, bottom, right;
};

struct TimeInfo
{
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    std::int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
constexpr std::int32_t transportChanged = 1, transportPlaying = 2, ppqValid = 1 << 9, tempoValid = 1 << 10,
                       barsValid = 1 << 11, timeSigValid = 1 << 13;
} // namespace abi

// --- module loading ------------------------------------------------------------------------------------
// A DLL is loaded once and stays loaded (some plugins crash when unloaded and loaded again).
abi::Entry entryOf (const juce::String& path)
{
   #if JUCE_WINDOWS
    static std::mutex mutex;
    static std::map<juce::String, HMODULE> modules;
    const std::lock_guard<std::mutex> lock (mutex);
    auto& module = modules[path];
    if (module == nullptr)
        module = LoadLibraryW (path.toWideCharPointer());
    if (module == nullptr)
        return nullptr;
    auto entry = (abi::Entry) (void*) GetProcAddress (module, "VSTPluginMain");
    if (entry == nullptr)
        entry = (abi::Entry) (void*) GetProcAddress (module, "main");
    return entry;
   #elif JUCE_LINUX || JUCE_BSD
    // a Linux VST2 is a shared object with the same entry points (yabridge's bridges too)
    static std::mutex mutex;
    static std::map<juce::String, void*> modules;
    const std::lock_guard<std::mutex> lock (mutex);
    auto& module = modules[path];
    if (module == nullptr)
        module = dlopen (path.toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr)
        return nullptr;
    auto entry = (abi::Entry) dlsym (module, "VSTPluginMain");
    if (entry == nullptr)
        entry = (abi::Entry) dlsym (module, "main");
    return entry;
   #else
    juce::ignoreUnused (path);
    return nullptr;
   #endif
}

class Vst2Instance;

// Which instance an Effect belongs to (hostReserved1), and the shell sub-plugin being created (the plugin
// asks for it through the callback before its Effect exists).
std::mutex creationLock;
std::atomic<std::int32_t> shellIdToCreate { 0 };

std::intptr_t hostCallback (abi::Effect* effect, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt);

juce::String dispatcherText (abi::Effect* e, std::int32_t opcode, std::int32_t index = 0)
{
    char text[512] {};
    e->dispatcher (e, opcode, index, 0, text, 0.0f);
    text[sizeof (text) - 1] = 0;
    return juce::String::fromUTF8 (text).trim();
}

// Opens one plugin of the file (the shell sub-plugin `shellId`, or the plain plugin with 0); nullptr if it isn't a VST2.
abi::Effect* openEffect (const juce::String& path, std::int32_t shellId)
{
    const auto entry = entryOf (path);
    if (entry == nullptr)
        return nullptr;
    const std::lock_guard<std::mutex> lock (creationLock);
    shellIdToCreate = shellId;
    abi::Effect* effect = nullptr;
    effect = entry (hostCallback);
    shellIdToCreate = 0;
    if (effect == nullptr || effect->magic != abi::magic || effect->dispatcher == nullptr)
        return nullptr;
    effect->hostReserved1 = 0;
    effect->dispatcher (effect, abi::open, 0, 0, nullptr, 0.0f);
    return effect;
}

void closeEffect (abi::Effect* effect)
{
    if (effect != nullptr)
        effect->dispatcher (effect, abi::close, 0, 0, nullptr, 0.0f);
}

bool effectIsInstrument (abi::Effect* e)
{
    return (e->flags & abi::isSynth) != 0 || e->dispatcher (e, abi::getPlugCategory, 0, 0, nullptr, 0.0f) == abi::categorySynth;
}

juce::PluginDescription describe (abi::Effect* e, const juce::File& file, const juce::String& shellName, bool shared)
{
    juce::PluginDescription d;
    auto name = shellName.isNotEmpty() ? shellName : dispatcherText (e, abi::getEffectName);
    if (name.isEmpty())
        name = dispatcherText (e, abi::getProductString);
    if (name.isEmpty())
        name = file.getFileNameWithoutExtension();
    d.name = name;
    d.descriptiveName = name;
    d.pluginFormatName = Vst2PluginFormat::formatName;
    d.manufacturerName = dispatcherText (e, abi::getVendorString);
    d.version = juce::String ((int) e->dispatcher (e, abi::getVendorVersion, 0, 0, nullptr, 0.0f));
    d.fileOrIdentifier = file.getFullPathName();
    d.lastFileModTime = file.getLastModificationTime();
    d.lastInfoUpdateTime = juce::Time::getCurrentTime();
    d.uniqueId = d.deprecatedUid = e->uniqueId;
    d.isInstrument = effectIsInstrument (e);
    d.category = d.isInstrument ? "Synth" : "Effect";
    d.numInputChannels = e->numInputs;
    d.numOutputChannels = e->numOutputs;
    d.hasSharedContainer = shared;
    return d;
}

// --- parameter -------------------------------------------------------------------------------------------
class Vst2Parameter final : public juce::HostedAudioProcessorParameter
{
public:
    Vst2Parameter (abi::Effect* e, int i, const juce::String& n, const juce::String& l)
        : effect (e), index (i), name (n), label (l)
    {
        value.store (e->getParameter != nullptr ? e->getParameter (e, i) : 0.0f);
        defaultValue = value.load();
    }

    juce::String getParameterID() const override { return juce::String (index); }
    float getValue() const override { return value.load(); }
    void setValue (float v) override
    {
        value.store (v);
        if (effect->setParameter != nullptr)
            effect->setParameter (effect, index, v);
    }
    float getDefaultValue() const override { return defaultValue; }
    juce::String getName (int maximumLength) const override { return name.substring (0, maximumLength); }
    juce::String getLabel() const override { return label; }
    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, text.getFloatValue()); }
    juce::String getText (float v, int length) const override
    {
        // the plugin formats its current value only: ask it on the UI thread when it's the current one
        if (juce::MessageManager::existsAndIsCurrentThread() && std::abs (v - value.load()) < 1.0e-6f)
        {
            const auto text = dispatcherText (effect, abi::getParamDisplay, index);
            const auto unit = dispatcherText (effect, abi::getParamLabel, index);
            if (text.isNotEmpty())
                return (unit.isNotEmpty() ? text + " " + unit : text).substring (0, length);
        }
        return juce::String (v, 3).substring (0, length);
    }

    void setFromPlugin (float v) noexcept { value.store (v); }
    void refresh() noexcept
    {
        if (effect->getParameter != nullptr)
            value.store (effect->getParameter (effect, index));
    }

private:
    abi::Effect* effect;
    const int index;
    const juce::String name, label;
    std::atomic<float> value { 0.0f };
    float defaultValue = 0.0f;
};

// --- instance ----------------------------------------------------------------------------------------------
class Vst2Instance final : public juce::AudioPluginInstance
{
public:
    Vst2Instance (abi::Effect* e, const juce::PluginDescription& d)
        : AudioPluginInstance (busesFor (*e, d.isInstrument)),
          effect (e),
          description (d)
    {
        effect->hostReserved1 = (std::intptr_t) this;
        for (int i = 0; i < effect->numParams; ++i)
        {
            auto parameter = std::make_unique<Vst2Parameter> (effect, i, dispatcherText (effect, abi::getParamName, i),
                                                              dispatcherText (effect, abi::getParamLabel, i));
            parameters.push_back (parameter.get());
            addHostedParameter (std::move (parameter));
        }
        inputs.resize ((std::size_t) juce::jmax (0, effect->numInputs));
        outputs.resize ((std::size_t) juce::jmax (0, effect->numOutputs));
        events.resize (sizeof (std::int32_t) * 2 + sizeof (void*) * (maxEvents + 2));
        midiEvents.resize (maxEvents);
        timeInfo = {};
    }

    ~Vst2Instance() override
    {
        *alive = false;
        releaseResources();
        effect->dispatcher (effect, abi::close, 0, 0, nullptr, 0.0f);
    }

    // --- AudioProcessor ---------------------------------------------------------------------------------------
    const juce::String getName() const override { return description.name; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }

    void prepareToPlay (double sampleRate, int blockSize) override
    {
        releaseResources();
        maxBlock = juce::jmax (1, blockSize);
        rate = sampleRate;
        effect->dispatcher (effect, abi::setSampleRate, 0, 0, nullptr, (float) sampleRate);
        effect->dispatcher (effect, abi::setBlockSize, 0, maxBlock, nullptr, 0.0f);
        scratch.setSize (juce::jmax (1, effect->numOutputs + effect->numInputs), maxBlock);
        effect->dispatcher (effect, abi::mainsChanged, 0, 1, nullptr, 0.0f);
        effect->dispatcher (effect, abi::startProcess, 0, 0, nullptr, 0.0f);
        active = true;
        setLatencySamples (effect->initialDelay);
    }

    void releaseResources() override
    {
        if (! active)
            return;
        effect->dispatcher (effect, abi::stopProcess, 0, 0, nullptr, 0.0f);
        effect->dispatcher (effect, abi::mainsChanged, 0, 0, nullptr, 0.0f);
        active = false;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const auto n = buffer.getNumSamples();
        audioThread.store (std::this_thread::get_id());

        if (! active || n > maxBlock)
        {
            buffer.clear();
            return;
        }

        updateTimeInfo (n);

        // MIDI → VstEvents (preallocated; extra events are dropped)
        int count = 0;
        for (const auto m : midi)
        {
            if (count >= maxEvents)
                break;
            const auto& message = m.getMessage();
            if (message.getRawDataSize() > 3 || message.isSysEx())
                continue;
            auto& e = midiEvents[(std::size_t) count];
            e = {};
            e.deltaFrames = juce::jlimit (0, n - 1, m.samplePosition);
            std::memcpy (e.data, message.getRawData(), (std::size_t) message.getRawDataSize());
            ++count;
        }
        if (count > 0)
        {
            auto* header = events.data();
            const std::int32_t numEvents = count;
            std::memcpy (header, &numEvents, 4);
            std::memset (header + 4, 0, sizeof (void*) == 8 ? 12 : 4);
            auto** list = reinterpret_cast<abi::MidiEvent**> (header + (sizeof (void*) == 8 ? 16 : 8));
            for (int i = 0; i < count; ++i)
                list[i] = &midiEvents[(std::size_t) i];
            effect->dispatcher (effect, abi::processEvents, 0, 0, header, 0.0f);
        }

        // inputs: the buffer's channels (extra ones, e.g. a sidechain pair, get silence); outputs: scratch
        const auto numIn = effect->numInputs, numOut = effect->numOutputs;
        for (int ch = 0; ch < numIn; ++ch)
        {
            auto* in = scratch.getWritePointer (numOut + ch);
            if (ch < buffer.getNumChannels() && ch < getTotalNumInputChannels())
                juce::FloatVectorOperations::copy (in, buffer.getReadPointer (ch), n);
            else
                juce::FloatVectorOperations::clear (in, n);
            inputs[(std::size_t) ch] = in;
        }
        for (int ch = 0; ch < numOut; ++ch)
        {
            outputs[(std::size_t) ch] = scratch.getWritePointer (ch);
            juce::FloatVectorOperations::clear (outputs[(std::size_t) ch], n);
        }

        if ((effect->flags & abi::canReplacing) != 0 && effect->processReplacing != nullptr)
            effect->processReplacing (effect, inputs.data(), outputs.data(), n);
        else if (effect->process != nullptr)
            effect->process (effect, inputs.data(), outputs.data(), n);

        // main output: the first one or two outputs (a mono plugin feeds both sides)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            if (numOut > 0)
                buffer.copyFrom (ch, 0, scratch, juce::jmin (ch, juce::jmin (numOut, 2) - 1), 0, n);
            else
                buffer.clear (ch, 0, n);
        }
    }

    double getTailLengthSeconds() const override
    {
        const auto samples = effect->dispatcher (effect, abi::getTailSize, 0, 0, nullptr, 0.0f);
        return samples > 1 && rate > 0.0 ? (double) samples / rate : 0.0;
    }
    bool acceptsMidi() const override { return description.isInstrument; }
    bool producesMidi() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return (effect->flags & abi::hasEditor) != 0; }

    int getNumPrograms() override { return juce::jmax (1, effect->numPrograms); }
    int getCurrentProgram() override { return (int) effect->dispatcher (effect, abi::getProgram, 0, 0, nullptr, 0.0f); }
    void setCurrentProgram (int index) override
    {
        if (index >= 0 && index < effect->numPrograms)
        {
            effect->dispatcher (effect, abi::setProgram, 0, index, nullptr, 0.0f);
            refreshParameters();
        }
    }
    const juce::String getProgramName (int index) override
    {
        char text[256] {};
        if (effect->dispatcher (effect, abi::getProgramNameIndexed, index, -1, text, 0.0f) != 0)
            return juce::String::fromUTF8 (text);
        return index == getCurrentProgram() ? dispatcherText (effect, abi::getProgramName) : juce::String();
    }
    void changeProgramName (int, const juce::String&) override {}

    // State: FL Studio's wrapper layout (REA_rodada89 §2), so FL's own data restores as is.
    void getStateInformation (juce::MemoryBlock& data) override
    {
        FlVst2State state;
        state.currentProgram = getCurrentProgram();
        if ((effect->flags & abi::programChunks) != 0)
        {
            state.flags = 8;
            void* chunk = nullptr;
            const auto size = effect->dispatcher (effect, abi::getChunk, 0, 0, &chunk, 0.0f);
            if (chunk != nullptr && size > 0)
                state.chunk.assign ((const std::uint8_t*) chunk, (const std::uint8_t*) chunk + size);
        }
        if (state.chunk.empty())
            for (int i = 0; i < effect->numParams; ++i)
                state.params.push_back (effect->getParameter != nullptr ? effect->getParameter (effect, i) : 0.0f);
        const auto bytes = writeFlVst2State (state);
        data.replaceAll (bytes.data(), bytes.size());
    }

    void setStateInformation (const void* data, int size) override
    {
        FlVst2State state;
        if (! parseFlVst2State (std::vector<std::uint8_t> ((const std::uint8_t*) data, (const std::uint8_t*) data + size), state))
            return;

        const auto programs = effect->numPrograms;
        if (! state.chunk.empty())
        {
            // FL: effSetChunk as a bank (index 0), as a program only for old version −6 states
            effect->dispatcher (effect, abi::setChunk, state.chunkIsProgram() ? 1 : 0, (std::intptr_t) state.chunk.size(),
                                state.chunk.data(), 0.0f);
        }
        else if (! state.params.empty() && effect->setParameter != nullptr)
        {
            const auto params = juce::jmax (1, effect->numParams);
            const auto perProgram = state.allPrograms() && programs > 1 && (int) state.params.size() == params * programs;
            for (int p = 0; p < (perProgram ? programs : 1); ++p)
            {
                if (perProgram)
                    effect->dispatcher (effect, abi::setProgram, 0, p, nullptr, 0.0f);
                effect->dispatcher (effect, abi::beginSetProgram, 0, 0, nullptr, 0.0f);
                for (int i = 0; i < juce::jmin (effect->numParams, (int) state.params.size() - p * params); ++i)
                    effect->setParameter (effect, i, state.params[(std::size_t) (p * params + i)]);
                effect->dispatcher (effect, abi::endSetProgram, 0, 0, nullptr, 0.0f);
            }
        }
        if (state.currentProgram >= 0 && state.currentProgram < programs && state.currentProgram != getCurrentProgram())
            effect->dispatcher (effect, abi::setProgram, 0, state.currentProgram, nullptr, 0.0f);
        refreshParameters();
    }

    // --- host callback -----------------------------------------------------------------------------------------
    std::intptr_t callback (std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt)
    {
        juce::ignoreUnused (ptr);
        switch (opcode)
        {
            case abi::automate:
                if (index >= 0 && index < (int) parameters.size())
                {
                    parameters[(std::size_t) index]->setFromPlugin (opt);
                    if (juce::MessageManager::existsAndIsCurrentThread())
                        parameters[(std::size_t) index]->sendValueChangedMessageToListeners (opt);
                }
                return 0;
            case abi::getTime: return (std::intptr_t) &timeInfo;
            case abi::ioChanged:
                onMainThread ([this] { setLatencySamples (effect->initialDelay); });
                return 1;
            case abi::sizeWindow:
                onMainThread ([this, w = index, h = (int) value] {
                    if (auto* e = editor.getComponent())
                    {
                        const auto scale = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale;
                        e->setSize (juce::roundToInt (w / scale), juce::roundToInt (h / scale));
                    }
                });
                return 1;
            case abi::getSampleRate: return (std::intptr_t) rate;
            case abi::getBlockSize: return maxBlock;
            case abi::getCurrentProcessLevel:
                if (audioThread.load() == std::this_thread::get_id())
                    return isNonRealtime() ? 4 : 2;
                return 1;
            case abi::updateDisplay:
                onMainThread ([this] { refreshParameters(); updateHostDisplay(); });
                return 1;
            default: return 0;
        }
    }

    void refreshParameters()
    {
        for (auto* p : parameters)
            p->refresh();
    }

    abi::Effect* const effect;
    juce::Component::SafePointer<juce::Component> editor;

private:
    static constexpr int maxEvents = 1024;

    static BusesProperties busesFor (const abi::Effect& e, bool instrument)
    {
        BusesProperties buses;
        auto set = [] (int channels) { return channels >= 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono(); };
        if (! instrument && e.numInputs > 0)
            buses = buses.withInput ("Input", set (e.numInputs), true);
        if (e.numOutputs > 0)
            buses = buses.withOutput ("Output", set (e.numOutputs), true);
        return buses;
    }

    void updateTimeInfo (int)
    {
        timeInfo = {};
        timeInfo.sampleRate = rate;
        timeInfo.tempo = 120.0;
        timeInfo.timeSigNumerator = timeInfo.timeSigDenominator = 4;
        timeInfo.flags = abi::tempoValid | abi::timeSigValid;
        if (auto* head = getPlayHead())
            if (const auto position = head->getPosition())
            {
                if (const auto bpm = position->getBpm())
                    timeInfo.tempo = *bpm;
                if (const auto samples = position->getTimeInSamples())
                    timeInfo.samplePos = (double) *samples;
                if (const auto ppq = position->getPpqPosition())
                {
                    timeInfo.ppqPos = *ppq;
                    timeInfo.flags |= abi::ppqValid;
                }
                if (const auto bar = position->getPpqPositionOfLastBarStart())
                {
                    timeInfo.barStartPos = *bar;
                    timeInfo.flags |= abi::barsValid;
                }
                if (const auto signature = position->getTimeSignature())
                {
                    timeInfo.timeSigNumerator = signature->numerator;
                    timeInfo.timeSigDenominator = signature->denominator;
                }
                if (position->getIsPlaying())
                    timeInfo.flags |= abi::transportPlaying;
                if (position->getIsPlaying() != wasPlaying)
                    timeInfo.flags |= abi::transportChanged;
                wasPlaying = position->getIsPlaying();
            }
    }

    void onMainThread (std::function<void()> fn)
    {
        juce::MessageManager::callAsync ([flag = alive, fn = std::move (fn)] {
            if (*flag)
                fn();
        });
    }

    juce::PluginDescription description;
    std::vector<Vst2Parameter*> parameters;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    bool active = false, wasPlaying = false;
    int maxBlock = 512;
    double rate = 44100.0;
    juce::AudioBuffer<float> scratch;
    std::vector<float*> inputs, outputs;
    std::vector<std::uint8_t> events; // VstEvents header + pointer list
    std::vector<abi::MidiEvent> midiEvents;
    abi::TimeInfo timeInfo {};
    std::atomic<std::thread::id> audioThread {};
};

std::intptr_t hostCallback (abi::Effect* effect, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt)
{
    switch (opcode)
    {
        case abi::version: return 2400;
        case abi::currentId: return shellIdToCreate.load();
        case abi::idle: return 0;
        case abi::wantMidi: return 1;
        case abi::getInputLatency:
        case abi::getOutputLatency: return 0;
        case abi::getAutomationState: return 1; // off
        case abi::hostGetVendorString:
        case abi::hostGetProductString:
            if (ptr != nullptr)
                std::strcpy ((char*) ptr, "LYNX");
            return 1;
        case abi::hostGetVendorVersion: return 1;
        case abi::getLanguage: return 1; // English
        case abi::hostCanDo:
        {
            static const char* yes[] = { "sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents",
                                         "receiveVstMidiEvent", "sizeWindow", "supplyIdle", "startStopProcess" };
            if (ptr != nullptr)
                for (auto* s : yes)
                    if (std::strcmp ((const char*) ptr, s) == 0)
                        return 1;
            return 0;
        }
        case abi::beginEdit:
        case abi::endEdit: return 1;
        case abi::getDirectory: return 0;
        default: break;
    }

    if (effect != nullptr && effect->hostReserved1 != 0)
        return reinterpret_cast<Vst2Instance*> (effect->hostReserved1)->callback (opcode, index, value, ptr, opt);

    if (opcode == abi::getSampleRate)
        return 44100;
    if (opcode == abi::getBlockSize)
        return 512;
    return 0;
}

// The plugin's own window inside a native child view (platform/NativeEmbed, like the CLAP editor).
class Vst2Editor final : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit Vst2Editor (Vst2Instance& p)
        : AudioProcessorEditor (p),
          owner (p)
    {
        const auto scale = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale;
        setSize (480, 320);
        abi::Rect* rect = nullptr;
        owner.effect->dispatcher (owner.effect, abi::editGetRect, 0, 0, &rect, 0.0f);
        if (rect != nullptr && rect->right > rect->left && rect->bottom > rect->top)
            setSize (juce::roundToInt ((rect->right - rect->left) / scale), juce::roundToInt ((rect->bottom - rect->top) / scale));
        addAndMakeVisible (embed);
        owner.editor = this;
        startTimer (20);
    }

    ~Vst2Editor() override
    {
        stopTimer();
        if (opened)
            owner.effect->dispatcher (owner.effect, abi::editClose, 0, 0, nullptr, 0.0f);
    }

    void resized() override { embed.setBounds (getLocalBounds()); }

private:
    void timerCallback() override
    {
        if (opened)
        {
            owner.effect->dispatcher (owner.effect, abi::editIdle, 0, 0, nullptr, 0.0f);
            return;
        }
        if (! embed.create())
            return; // not on screen yet
        const auto scale = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale;
        owner.effect->dispatcher (owner.effect, abi::editOpen, 0, 0, embed.nativeHandle(), 0.0f);
        opened = true;
        // some plugins only know their size once open
        abi::Rect* rect = nullptr;
        owner.effect->dispatcher (owner.effect, abi::editGetRect, 0, 0, &rect, 0.0f);
        if (rect != nullptr && rect->right > rect->left && rect->bottom > rect->top)
            setSize (juce::roundToInt ((rect->right - rect->left) / scale), juce::roundToInt ((rect->bottom - rect->top) / scale));
    }

    Vst2Instance& owner;
    platform::NativeEmbed embed;
    bool opened = false;
};

juce::AudioProcessorEditor* Vst2Instance::createEditor()
{
    return hasEditor() ? new Vst2Editor (*this) : nullptr;
}
} // namespace

// --- format ---------------------------------------------------------------------------------------------------

void Vst2PluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& path)
{
    if (! fileMightContainThisPluginType (path))
        return;

    auto* effect = openEffect (path, 0);
    if (effect == nullptr)
        return;

    const juce::File file (path);
    if (effect->dispatcher (effect, abi::getPlugCategory, 0, 0, nullptr, 0.0f) == abi::categoryShell)
    {
        // a shell (one DLL, several plugins): list each one by its ID and open it to describe it
        std::vector<std::pair<std::int32_t, juce::String>> ids;
        for (int guard = 0; guard < 4096; ++guard)
        {
            char name[256] {};
            const auto id = (std::int32_t) effect->dispatcher (effect, abi::shellGetNextPlugin, 0, 0, name, 0.0f);
            if (id == 0)
                break;
            ids.push_back ({ id, juce::String::fromUTF8 (name).trim() });
        }
        closeEffect (effect);
        for (const auto& [id, name] : ids)
            if (auto* sub = openEffect (path, id))
            {
                auto d = describe (sub, file, name, true);
                d.uniqueId = d.deprecatedUid = id;
                results.add (new juce::PluginDescription (d));
                closeEffect (sub);
            }
        return;
    }

    results.add (new juce::PluginDescription (describe (effect, file, {}, false)));
    closeEffect (effect);
}

bool Vst2PluginFormat::fileMightContainThisPluginType (const juce::String& path)
{
   #if JUCE_WINDOWS
    const juce::File f (path);
    // not the DLLs inside a VST3 bundle or a CLAP folder
    return f.hasFileExtension (".dll") && f.existsAsFile() && ! path.containsIgnoreCase (".vst3") && ! path.containsIgnoreCase (".clap");
   #elif JUCE_LINUX || JUCE_BSD
    const juce::File f (path);
    // not the shared objects inside VST3, CLAP or LV2 bundles
    return f.hasFileExtension (".so") && f.existsAsFile() && ! path.contains (".vst3/") && ! path.contains (".clap/")
        && ! path.contains (".lv2/");
   #else
    juce::ignoreUnused (path);
    return false;
   #endif
}

juce::String Vst2PluginFormat::getNameOfPluginFromIdentifier (const juce::String& path)
{
    return juce::File (path).getFileNameWithoutExtension();
}

bool Vst2PluginFormat::pluginNeedsRescanning (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).getLastModificationTime() != d.lastFileModTime;
}

bool Vst2PluginFormat::doesPluginStillExist (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).exists();
}

juce::StringArray Vst2PluginFormat::searchPathsForPlugins (const juce::FileSearchPath& directories, bool recursive, bool)
{
    juce::StringArray found;
   #if JUCE_WINDOWS
    for (int i = 0; i < directories.getNumPaths(); ++i)
        for (const auto& f : directories[i].findChildFiles (juce::File::findFiles, recursive, "*.dll"))
            if (fileMightContainThisPluginType (f.getFullPathName()))
                found.addIfNotAlreadyThere (f.getFullPathName());
   #elif JUCE_LINUX || JUCE_BSD
    for (int i = 0; i < directories.getNumPaths(); ++i)
        for (const auto& f : directories[i].findChildFiles (juce::File::findFiles, recursive, "*.so"))
            if (fileMightContainThisPluginType (f.getFullPathName()))
                found.addIfNotAlreadyThere (f.getFullPathName());
   #else
    juce::ignoreUnused (directories, recursive);
   #endif
    return found;
}

juce::FileSearchPath Vst2PluginFormat::getDefaultLocationsToSearch()
{
    juce::FileSearchPath paths;
   #if JUCE_WINDOWS
    const auto programs = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
    for (const auto* sub : { "VSTPlugins", "Steinberg/VSTPlugins", "Common Files/VST2", "Common Files/Steinberg/VST2" })
        paths.add (programs.getChildFile (sub));
   #elif JUCE_LINUX || JUCE_BSD
    paths.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".vst"));
    for (const auto* folder : { "/usr/lib/vst", "/usr/local/lib/vst", "/usr/lib64/vst", "/usr/local/lib64/vst" })
        paths.add (juce::File (folder));
   #endif
    return paths;
}

void Vst2PluginFormat::createPluginInstance (const juce::PluginDescription& d, double sampleRate, int blockSize,
                                             PluginCreationCallback callback)
{
    // a shell's sub-plugin: the ID the plugin asks the host for while being created
    auto* effect = openEffect (d.fileOrIdentifier, d.hasSharedContainer ? d.uniqueId : 0);
    if (effect == nullptr)
    {
        callback (nullptr, juce::String::fromUTF8 ("could not load ") + d.fileOrIdentifier);
        return;
    }
    if (effect->uniqueId != d.uniqueId && ! d.hasSharedContainer)
    {
        closeEffect (effect);
        callback (nullptr, juce::String::fromUTF8 ("a different plugin is now in ") + d.fileOrIdentifier);
        return;
    }
    auto instance = std::make_unique<Vst2Instance> (effect, d);
    instance->setRateAndBufferSizeDetails (sampleRate, blockSize);
    callback (std::move (instance), {});
}
namespace vst2
{
// (round 162d: the wrapper's .fxp / .fxb presets)
std::optional<Info> info (juce::AudioPluginInstance& plugin)
{
    auto* vst = dynamic_cast<Vst2Instance*> (&plugin);
    if (vst == nullptr)
        return std::nullopt;
    const auto* e = vst->effect;
    return Info { e->uniqueId, e->version, e->numPrograms, e->numParams, (e->flags & abi::programChunks) != 0 };
}

bool getChunk (juce::AudioPluginInstance& plugin, std::vector<std::uint8_t>& out, bool program)
{
    auto* vst = dynamic_cast<Vst2Instance*> (&plugin);
    if (vst == nullptr || (vst->effect->flags & abi::programChunks) == 0)
        return false;
    void* chunk = nullptr;
    const auto size = vst->effect->dispatcher (vst->effect, abi::getChunk, program ? 1 : 0, 0, &chunk, 0.0f);
    if (chunk == nullptr || size <= 0)
        return false;
    out.assign ((const std::uint8_t*) chunk, (const std::uint8_t*) chunk + size);
    return true;
}

bool setChunk (juce::AudioPluginInstance& plugin, const std::vector<std::uint8_t>& data, bool program)
{
    auto* vst = dynamic_cast<Vst2Instance*> (&plugin);
    if (vst == nullptr || (vst->effect->flags & abi::programChunks) == 0 || data.empty())
        return false;
    auto copy = data; // (plugins may keep the pointer until the call returns, never const)
    vst->effect->dispatcher (vst->effect, abi::setChunk, program ? 1 : 0, (std::intptr_t) copy.size(), copy.data(), 0.0f);
    vst->refreshParameters();
    vst->updateHostDisplay();
    return true;
}
} // namespace vst2
} // namespace lynx
