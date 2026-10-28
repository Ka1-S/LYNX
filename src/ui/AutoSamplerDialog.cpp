#include "ui/AutoSamplerDialog.h"

#include <limits>

#include "core/AutoSampler.h"
#include "core/ExportOptions.h"
#include "core/RackOps.h"
#include "engine/Exporter.h"
#include "ui/I18n.h"

namespace lynx
{
namespace
{
autosample::Options lastOptions; // remembered for the session, as FL's form

juce::String keyName (int key)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[key % 12]) + juce::String (key / 12); // FL's names: C5 = 60
}

class AutoSamplerThread final : public juce::ThreadWithProgressWindow
{
public:
    AutoSamplerThread (AudioEngine& e, int c, autosample::Options o, autosample::Plan p, std::unique_ptr<ExportJob> j, juce::File render,
                       juce::File out, double rate)
        : juce::ThreadWithProgressWindow (tr ("Sampling the instrument..."), true, true),
          engine (e), channel (c), options (o), plan (std::move (p)), job (std::move (j)), renderFile (std::move (render)),
          folder (std::move (out)), sampleRate (rate), baseName (e.getSong().channels[(std::size_t) c].name)
    {
    }

    void run() override
    {
        ok = job->run ([this] (double p) {
            setProgress (0.9 * p);
            return ! threadShouldExit();
        }, error);
        if (! ok || threadShouldExit())
            return;
        setStatusMessage (tr ("Writing the samples..."));
        ok = writeTakes();
        renderFile.deleteFile();
    }

    void threadComplete (bool cancelled) override
    {
        if (ok && ! cancelled)
        {
            const auto zones = autosample::zones (plan, paths);
            const auto source = channel;
            const auto o = options;
            engine.editSong ([&] (Song& s) {
                if (source < 0 || source >= (int) s.channels.size())
                    return;
                insertChannel (s, source + 1, autosample::makeSamplerChannel (s.channels[(std::size_t) source], zones, o));
            });
        }
        else if (! cancelled)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Could not sample the instrument"), error);
        if (! ok || cancelled)
            folder.deleteRecursively();
        renderFile.deleteFile();
        // the export engine is torn down on the UI thread (plugins require this)
        juce::MessageManager::callAsync ([this] { delete this; });
    }

private:
    bool writeTakes()
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (renderFile));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            error = tr ("could not read {0}", { renderFile.getFullPathName() });
            return false;
        }
        const auto frames = (int) std::min<juce::int64> (reader->lengthInSamples, std::numeric_limits<int>::max() / 2);
        juce::AudioBuffer<float> buffer ((int) std::max (2u, reader->numChannels), frames);
        reader->read (&buffer, 0, frames, 0, true, true);
        const auto* right = reader->numChannels > 1 ? buffer.getReadPointer (1) : nullptr;
        const auto cuts = autosample::cutTakes (buffer.getReadPointer (0), right, frames, reader->sampleRate, plan, options);
        reader.reset();

        juce::WavAudioFormat wav;
        for (std::size_t i = 0; i < cuts.size() && ! threadShouldExit(); ++i)
        {
            const auto file = folder.getChildFile (juce::String::fromUTF8 (autosample::takeFileName (baseName, plan.takes[i]).c_str()));
            auto stream = file.createOutputStream();
            const auto chans = cuts[i].right.empty() ? 1 : 2;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                stream == nullptr ? nullptr : wav.createWriterFor (stream.get(), sampleRate, (unsigned int) chans, options.float32 ? 32 : 16, {}, 0));
            if (writer == nullptr)
            {
                error = tr ("could not write {0}", { file.getFullPathName() });
                return false;
            }
            stream.release();
            float* data[2] = { const_cast<float*> (cuts[i].left.data()), const_cast<float*> (cuts[i].right.data()) };
            juce::AudioBuffer<float> view (data, chans, (int) cuts[i].left.size());
            if (! writer->writeFromAudioSampleBuffer (view, 0, view.getNumSamples()))
            {
                error = tr ("could not write {0}", { file.getFullPathName() });
                return false;
            }
            paths.push_back (file.getFullPathName().toStdString());
            setProgress (0.9 + 0.1 * (double) (i + 1) / (double) cuts.size());
        }
        return ! threadShouldExit() && paths.size() == cuts.size();
    }

    AudioEngine& engine;
    int channel;
    autosample::Options options;
    autosample::Plan plan;
    std::unique_ptr<ExportJob> job;
    juce::File renderFile, folder;
    double sampleRate;
    std::string baseName; // read on the UI thread
    std::vector<std::string> paths;
    bool ok = false;
    juce::String error;
};

void start (AudioEngine& engine, int channel, const autosample::Options& options)
{
    engine.capturePluginStates();
    const auto& song = engine.getSong();
    if (channel < 0 || channel >= (int) song.channels.size())
        return;
    const auto plan = autosample::plan (options, song.bpm);
    if (plan.takes.empty())
        return;

    const auto name = juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str());
    const auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("LYNX/Samples");
    root.createDirectory();
    const auto folder = root.getNonexistentChildFile (juce::File::createLegalFileName (name + " (sampled)"), {});
    folder.createDirectory();
    const auto render = folder.getChildFile ("render.tmp.wav");

    exportopts::Options o;
    o.format = exportopts::Format::wav;
    o.wavBits = 32;
    o.dither = exportopts::DitherMode::none;
    o.range = exportopts::Range::pattern;
    o.tail = exportopts::TailMode::cut;
    o.loops = 1;
    o.normalize = false;
    o.masterEffects = options.masterEffects;
    o.trimPdc = true;
    o.sampleRate = engine.getTransport().sampleRate.load() > 0 ? engine.getTransport().sampleRate.load() : 44100.0;
    o.disableMaxPolyphony = true;
    auto job = std::make_unique<ExportJob> (autosample::makeRenderSong (song, channel, plan, options), o, render);
    (new AutoSamplerThread (engine, channel, options, plan, std::move (job), render, folder, o.sampleRate))->launchThread();
}

class OptionsWindow final : public juce::AlertWindow
{
public:
    OptionsWindow (const juce::String& channelName, const autosample::Options& o)
        : juce::AlertWindow (tr ("Create sampled instrument"), tr ("Plays {0} key by key and makes a Sampler channel of the recordings.", { channelName }),
                             juce::MessageBoxIconType::NoIcon)
    {
        juce::StringArray keys;
        for (int k = 0; k < 128; ++k)
            keys.add (keyName (k));
        addComboBox ("low", keys, tr ("Low note"));
        addComboBox ("high", keys, tr ("High note"));
        getComboBoxComponent ("low")->setSelectedItemIndex (o.lowKey, juce::dontSendNotification);
        getComboBoxComponent ("high")->setSelectedItemIndex (o.highKey, juce::dontSendNotification);
        addTextEditor ("zone", juce::String (o.keysPerZone), tr ("Keys per zone"));
        addTextEditor ("root", juce::String (o.rootOffset), tr ("Root offset"));
        addTextEditor ("layers", juce::String (o.velocityLayers), tr ("Velocity layers"));
        addTextEditor ("maxvel", juce::String (o.maxVelocity), tr ("Max velocity"));
        addTextEditor ("hold", juce::String (o.holdSeconds, 2), tr ("Release after (seconds)"));
        addTextEditor ("max", juce::String (o.maxSeconds, 2), tr ("Max length after release (seconds)"));
        addComboBox ("stop", { tr ("Silence"), tr ("Max length") }, tr ("Stop on"));
        getComboBoxComponent ("stop")->setSelectedItemIndex (o.stopOnSilence ? 0 : 1, juce::dontSendNotification);
        addComboBox ("bits", { tr ("16-bit integer"), tr ("32-bit float") }, tr ("Bit depth"));
        getComboBoxComponent ("bits")->setSelectedItemIndex (o.float32 ? 1 : 0, juce::dontSendNotification);
        addComboBox ("chans", { tr ("Stereo"), tr ("Mono (merged)"), tr ("Mono (left only)"), tr ("Mono (right only)") }, tr ("Channels"));
        getComboBoxComponent ("chans")->setSelectedItemIndex (o.channels, juce::dontSendNotification);
        const std::pair<const char*, bool> items[] = { { TR_NOOP ("Declick in"), o.declickIn },     { TR_NOOP ("Declick out"), o.declickOut },
                                                       { TR_NOOP ("Normalize"), o.normalize },       { TR_NOOP ("Enable insert effects"), o.insertEffects },
                                                       { TR_NOOP ("Enable master effects"), o.masterEffects } };
        for (std::size_t i = 0; i < toggles.size(); ++i)
        {
            toggles[i].setButtonText (tr (items[i].first));
            toggles[i].setToggleState (items[i].second, juce::dontSendNotification);
            toggles[i].setSize (320, 22);
            addCustomComponent (&toggles[i]);
        }
        addButton (tr ("Start"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    }

    autosample::Options options() const
    {
        autosample::Options o;
        auto* self = const_cast<OptionsWindow*> (this);
        o.lowKey = self->getComboBoxComponent ("low")->getSelectedItemIndex();
        o.highKey = self->getComboBoxComponent ("high")->getSelectedItemIndex();
        o.keysPerZone = getTextEditorContents ("zone").getIntValue();
        o.rootOffset = getTextEditorContents ("root").getIntValue();
        o.velocityLayers = getTextEditorContents ("layers").getIntValue();
        o.maxVelocity = getTextEditorContents ("maxvel").getIntValue();
        o.holdSeconds = getTextEditorContents ("hold").getDoubleValue();
        o.maxSeconds = getTextEditorContents ("max").getDoubleValue();
        o.stopOnSilence = self->getComboBoxComponent ("stop")->getSelectedItemIndex() == 0;
        o.float32 = self->getComboBoxComponent ("bits")->getSelectedItemIndex() == 1;
        o.channels = self->getComboBoxComponent ("chans")->getSelectedItemIndex();
        o.declickIn = toggles[0].getToggleState();
        o.declickOut = toggles[1].getToggleState();
        o.normalize = toggles[2].getToggleState();
        o.insertEffects = toggles[3].getToggleState();
        o.masterEffects = toggles[4].getToggleState();
        return autosample::clamped (o);
    }

private:
    std::array<juce::ToggleButton, 5> toggles;
};
} // namespace

void showAutoSamplerDialog (AudioEngine& engine, int channel)
{
    const auto& song = engine.getSong();
    if (channel < 0 || channel >= (int) song.channels.size())
        return;
    auto* w = new OptionsWindow (juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str()), lastOptions);
    auto* engineRef = &engine;
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, engineRef, channel] (int result) {
        if (result != 1)
            return;
        lastOptions = w->options();
        start (*engineRef, channel, lastOptions);
    }), true);
}
} // namespace lynx
