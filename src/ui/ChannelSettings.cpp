#include "ui/WindowOwner.h"
#include "ui/ChannelSettings.h"
#include "ui/LayerEditor.h"

#include "core/MixerOps.h"

#include <cstring>
#include <functional>

#include "core/DrumSynthLive.h"
#include "core/FlSynths.h"
#include "core/FlKeys.h"
#include "core/ChannelMiscFx.h"
#include "core/FlpImport.h"
#include "core/Harmor.h"
#include "core/Gms.h"
#include "core/MiniSynth.h"
#include "core/SimSynth.h"
#include "core/Drumaxx.h"
#include "core/Granulizer.h"
#include "core/Morphine.h"
#include "core/Ogun.h"
#include "core/Dx10.h"
#include "core/Kepler.h"
#include "core/PoiZone.h"
#include "core/BassDrum.h"
#include "core/FruitKick.h"
#include "core/Toxic.h"
#include "core/ThreeOsc.h"
#include "core/BeepMap.h"
#include "core/Sawer.h"
#include "core/Slayer.h"
#include "core/SytrusEngine.h"
#include "core/SynthPresets.h"
#include "core/PluginLayout.h"
#include "engine/Text.h"
#include "core/RackOps.h"
#include "ui/ControllerEditors.h"
#include "ui/HintBar.h"
#include "ui/ValueControl.h"
#include "ui/I18n.h"
#include "ui/PluginSkin.h"
#include "ui/Theme.h"
#include "ui/WaveformView.h"
#include "ui/SoundFontEditor.h"

namespace lynx
{
namespace
{
void showModulationFor (juce::Component& from, AudioEngine& engine, int channel);

// Cutoff and pitch envelopes (FL's channel envelopes), used by the synth and the sampler.
class ModEnvelopeEditor final : public juce::Component
{
public:
    std::function<void()> onChange;
    static constexpr int preferredHeight = 16 + 2 * 62;

    ModEnvelopeEditor()
    {
        const juce::String titles[] = { tr ("Cutoff env."), tr ("Pitch env.") };

        for (int e = 0; e < 2; ++e)
        {
            auto& row = rows[(std::size_t) e];
            row.title.setText (titles[e], juce::dontSendNotification);
            row.title.setFont (theme::font (theme::type::small));
            addAndMakeVisible (row.title);
            knob (row.amount, e == 0 ? -10.0 : -48.0, e == 0 ? 10.0 : 48.0, 0.01, e == 0 ? " oct" : " st");
            row.amount.setDoubleClickReturnValue (true, 0.0);
            row.amount.setTooltip (tr ("0 = off"));

            for (auto* s : { &row.attack, &row.hold, &row.decay, &row.release })
            {
                knob (*s, 0.0, 10.0, 0.001, " s");
                s->setSkewFactorFromMidPoint (0.5);
            }

            knob (row.sustain, 0.0, 1.0, 0.01, "");
        }

        const char* names[] = { TR_NOOP ("Amount"), TR_NOOP ("Attack"), TR_NOOP ("Hold"), TR_NOOP ("Decay"), TR_NOOP ("Sustain"), TR_NOOP ("Release") };

        for (int i = 0; i < 6; ++i)
        {
            labels[(std::size_t) i].setText (tr (names[i]), juce::dontSendNotification);
            labels[(std::size_t) i].setJustificationType (juce::Justification::centred);
            labels[(std::size_t) i].setFont (theme::font (theme::type::caption));
            addAndMakeVisible (labels[(std::size_t) i]);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (16);
        header.removeFromLeft (80);
        const auto w = header.getWidth() / 6;

        for (auto& l : labels)
            l.setBounds (header.removeFromLeft (w));

        for (auto& row : rows)
        {
            auto line = area.removeFromTop (62);
            row.title.setBounds (line.removeFromLeft (80).withSizeKeepingCentre (80, 24));
            for (auto* s : { &row.amount, &row.attack, &row.hold, &row.decay, &row.sustain, &row.release })
                s->setBounds (line.removeFromLeft (w));
        }
    }

    void show (const ModEnvelope& filter, const ModEnvelope& pitch)
    {
        rows[0].show (filter);
        rows[1].show (pitch);
    }

    ModEnvelope filter() const { return rows[0].read(); }
    ModEnvelope pitch() const { return rows[1].read(); }

private:
    struct Row
    {
        juce::Label title;
        juce::Slider amount, attack, hold, decay, sustain, release;

        void show (const ModEnvelope& e)
        {
            amount.setValue (e.amount, juce::dontSendNotification);
            attack.setValue (e.attack, juce::dontSendNotification);
            hold.setValue (e.hold, juce::dontSendNotification);
            decay.setValue (e.decay, juce::dontSendNotification);
            sustain.setValue (e.sustain, juce::dontSendNotification);
            release.setValue (e.release, juce::dontSendNotification);
        }

        ModEnvelope read() const
        {
            return { (float) amount.getValue(), (float) attack.getValue(), (float) hold.getValue(),
                     (float) decay.getValue(), (float) sustain.getValue(), (float) release.getValue() };
        }
    };

    void knob (juce::Slider& s, double lo, double hi, double step, const char* suffix)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.onValueChange = [this] {
            if (onChange)
                onChange();
        };
        addAndMakeVisible (s);
    }

    std::array<Row, 2> rows;
    std::array<juce::Label, 6> labels;
};

// Channel LFOs (pan, volume, cutoff, pitch), like FL's.
class LfoEditor final : public juce::Component
{
public:
    std::function<void()> onChange;
    static constexpr int preferredHeight = 18 + lfoTargets * 60;

    LfoEditor()
    {
        const juce::String titles[] = { tr ("Pan LFO"), tr ("Volume LFO"), tr ("Cutoff LFO"), tr ("Pitch LFO") };
        const double amountRange[] = { 1.0, 1.0, 8.0, 24.0 };
        const char* amountSuffix[] = { "", "", " oct", " st" };

        for (int i = 0; i < lfoTargets; ++i)
        {
            auto& row = rows[(std::size_t) i];
            row.title.setText (titles[i], juce::dontSendNotification);
            row.title.setFont (theme::font (theme::type::small));
            addAndMakeVisible (row.title);
            knob (row.amount, -amountRange[i], amountRange[i], 0.01, amountSuffix[i]);
            row.amount.setDoubleClickReturnValue (true, 0.0);
            row.amount.setTooltip (tr ("0 = off"));
            knob (row.period, 0.01, 16.0, 0.001, "");
            row.period.setSkewFactorFromMidPoint (0.5);
            knob (row.delay, 0.0, 8.0, 0.001, "");
            row.delay.setSkewFactorFromMidPoint (0.5);
            knob (row.attack, 0.0, 8.0, 0.001, "");
            row.attack.setSkewFactorFromMidPoint (0.5);
            row.shape.addItemList ({ tr ("Sine"), tr ("Triangle"), tr ("Pulse") }, 1);
            row.shape.onChange = [this] { changed(); };
            addAndMakeVisible (row.shape);
            row.sync.setButtonText (tr ("Beats"));
            row.sync.setTooltip (tr ("Period, delay and fade-in in beats (follows the BPM) instead of seconds"));
            row.sync.onClick = [this] { changed(); };
            addAndMakeVisible (row.sync);
            row.global.setButtonText (tr ("Global"));
            row.global.setTooltip (tr ("Phase follows the song position instead of restarting on each note"));
            row.global.onClick = [this] { changed(); };
            addAndMakeVisible (row.global);
        }

        const juce::String headers[] = { tr ("Amount"), tr ("Period"), tr ("Shape"), tr ("Delay"), tr ("Fade-in"), tr ("Unit") };

        for (int i = 0; i < 6; ++i)
        {
            labels[(std::size_t) i].setText (headers[i], juce::dontSendNotification);
            labels[(std::size_t) i].setJustificationType (juce::Justification::centred);
            labels[(std::size_t) i].setFont (theme::font (theme::type::caption));
            addAndMakeVisible (labels[(std::size_t) i]);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (18);
        header.removeFromLeft (80);
        const auto w = header.getWidth() / 6;

        for (auto& l : labels)
            l.setBounds (header.removeFromLeft (w));

        for (auto& row : rows)
        {
            auto line = area.removeFromTop (60);
            row.title.setBounds (line.removeFromLeft (80).withSizeKeepingCentre (80, 24));
            row.amount.setBounds (line.removeFromLeft (w));
            row.period.setBounds (line.removeFromLeft (w));
            row.shape.setBounds (line.removeFromLeft (w).withSizeKeepingCentre (w - 6, 24));
            row.delay.setBounds (line.removeFromLeft (w));
            row.attack.setBounds (line.removeFromLeft (w));
            auto toggles = line.removeFromLeft (w).withSizeKeepingCentre (w - 6, 50);
            row.sync.setBounds (toggles.removeFromTop (24));
            row.global.setBounds (toggles.removeFromBottom (24));
        }
    }

    void show (const std::array<ChannelLfo, lfoTargets>& lfo)
    {
        for (std::size_t i = 0; i < lfoTargets; ++i)
        {
            auto& row = rows[i];
            const auto& l = lfo[i];
            row.amount.setValue (l.amount, juce::dontSendNotification);
            row.period.setValue (l.period, juce::dontSendNotification);
            row.delay.setValue (l.delay, juce::dontSendNotification);
            row.attack.setValue (l.attack, juce::dontSendNotification);
            row.shape.setSelectedId (l.shape + 1, juce::dontSendNotification);
            row.sync.setToggleState (l.sync, juce::dontSendNotification);
            row.global.setToggleState (l.global, juce::dontSendNotification);
            const auto unit = l.sync ? " t" : " s";
            for (auto* s : { &row.period, &row.delay, &row.attack })
                s->setTextValueSuffix (unit);
        }
    }

    std::array<ChannelLfo, lfoTargets> read() const
    {
        std::array<ChannelLfo, lfoTargets> lfo {};

        for (std::size_t i = 0; i < lfoTargets; ++i)
        {
            const auto& row = rows[i];
            lfo[i] = { (float) row.amount.getValue(), (float) row.period.getValue(), row.sync.getToggleState(),
                       juce::jlimit (0, 2, row.shape.getSelectedId() - 1), (float) row.delay.getValue(), (float) row.attack.getValue(),
                       row.global.getToggleState() };
        }

        return lfo;
    }

private:
    struct Row
    {
        juce::Label title;
        juce::Slider amount, period, delay, attack;
        juce::ComboBox shape;
        juce::ToggleButton sync, global;
    };

    void knob (juce::Slider& s, double lo, double hi, double step, const char* suffix)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.onValueChange = [this] { changed(); };
        addAndMakeVisible (s);
    }

    void changed()
    {
        if (onChange)
            onChange();
    }

    std::array<Row, lfoTargets> rows;
    std::array<juce::Label, 6> labels;
};

// Channel modulation window: cutoff/pitch envelopes and LFOs of the synth or the sampler.
class ModulationPanel final : public juce::Component,
                              private juce::ChangeListener
{
public:
    ModulationPanel (AudioEngine& e, int c)
        : engine (e),
          channel (c)
    {
        envelopes.onChange = [this] { apply(); };
        lfos.onChange = [this] { apply(); };
        addAndMakeVisible (envelopes);
        addAndMakeVisible (lfos);
        refresh();
        engine.addChangeListener (this);
        setSize (660, 20 + ModEnvelopeEditor::preferredHeight + 16 + LfoEditor::preferredHeight);
    }

    ~ModulationPanel() override { engine.removeChangeListener (this); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        envelopes.setBounds (area.removeFromTop (ModEnvelopeEditor::preferredHeight));
        area.removeFromTop (16);
        lfos.setBounds (area.removeFromTop (LfoEditor::preferredHeight));
    }

    int getChannel() const noexcept { return channel; }

private:
    const ChannelDef* current() const
    {
        const auto& channels = engine.getSong().channels;
        return channel >= 0 && channel < (int) channels.size() ? &channels[(std::size_t) channel] : nullptr;
    }

    bool isSynth() const { return current() != nullptr && current()->instrument == InstrumentKind::synth; }

    void refresh()
    {
        const auto* c = current();

        if (c == nullptr)
            return;

        if (isSynth())
        {
            envelopes.show (c->synth.filterEnv, c->synth.pitchEnv);
            lfos.show (c->synth.lfo);
        }
        else
        {
            envelopes.show (c->sampler.filterEnv, c->sampler.pitchEnv);
            lfos.show (c->sampler.lfo);
        }

        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (tr ("Modulation - {0}", { juce::String::fromUTF8 (c->name.c_str()) }));
    }

    void apply()
    {
        if (current() == nullptr)
            return;

        const auto filter = envelopes.filter(), pitch = envelopes.pitch();
        const auto lfo = lfos.read();
        const auto synth = isSynth();

        engine.editSong ([c = channel, synth, filter, pitch, lfo] (Song& song) {
            auto& ch = song.channels[(std::size_t) c];

            if (synth)
            {
                ch.synth.filterEnv = filter;
                ch.synth.pitchEnv = pitch;
                ch.synth.lfo = lfo;
            }
            else
            {
                ch.sampler.filterEnv = filter;
                ch.sampler.pitchEnv = pitch;
                ch.sampler.lfo = lfo;
            }
        });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    int channel;
    ModEnvelopeEditor envelopes;
    LfoEditor lfos;
};

// "Controller" channel (FL's Envelope Controller): shows the 8 articulators (envelope, base, mode and
// key range) and which automations each output drives. Read-only.
class ControllerPanel final : public juce::Component
{
public:
    ControllerPanel (AudioEngine& e, int c)
        : engine (e),
          channel (c)
    {
        setSize (720, 470);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        const auto& song = engine.getSong();
        if (channel >= (int) song.channels.size())
            return;
        const auto parsed = parseEnvelopeController (song.channels[(std::size_t) channel].controllerState);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::title));
        g.drawText (tr ("Envelope controller: each note on this channel triggers the envelopes; output k drives the linked targets"),
                    10, 6, getWidth() - 20, 20, juce::Justification::centredLeft);
        if (! parsed)
            return;

        for (int k = 0; k < 8; ++k)
        {
            const auto& a = (*parsed)[(std::size_t) k];
            const auto area = juce::Rectangle<float> (10.0f + (float) (k % 4) * 177.0f, 34.0f + (float) (k / 4) * 215.0f, 170.0f, 205.0f);
            g.setColour (theme::colour::bg3);
            g.fillRoundedRectangle (area, theme::radius::lg);

            // targets of this output
            juce::StringArray targets;
            for (const auto& automation : song.automations)
                if (automation.source.channel == channel && automation.source.output == k)
                    targets.add (juce::String::fromUTF8 (automation.name.c_str()));

            g.setColour (targets.isEmpty() ? theme::colour::textSecondary : theme::colour::accent);
            g.setFont (theme::font (theme::type::body, true));
            g.drawText (tr ("Output {0}", { juce::String (k) }) + (targets.isEmpty() ? juce::String() : tr (" (in use)")), area.withHeight (20.0f).reduced (6.0f, 0.0f),
                        juce::Justification::centredLeft);

            const auto graph = area.withTrimmedTop (22.0f).withHeight (110.0f).reduced (6.0f, 2.0f);
            g.setColour (theme::colour::bg0);
            g.fillRect (graph);
            if (a.envOn && a.count > 0)
            {
                const auto total = std::max (0.001f, a.time[(std::size_t) a.count - 1]);
                juce::Path path;
                for (int i = 0; i < a.count; ++i)
                {
                    const auto x = graph.getX() + graph.getWidth() * a.time[(std::size_t) i] / total;
                    const auto y = graph.getBottom() - graph.getHeight() * a.level[(std::size_t) i];
                    if (i == 0)
                        path.startNewSubPath (x, y);
                    else
                        path.lineTo (x, y);
                }
                g.setColour (theme::colour::info);
                g.strokePath (path, juce::PathStrokeType (2.0f));
                if (a.sustain >= 0)
                {
                    g.setColour (theme::colour::textDisabled);
                    g.drawVerticalLine ((int) (graph.getX() + graph.getWidth() * a.time[(std::size_t) a.sustain] / total), graph.getY(), graph.getBottom());
                }
            }
            else
            {
                g.setColour (theme::colour::textDisabled);
                g.drawText (tr ("envelope off"), graph, juce::Justification::centred);
            }

            g.setColour (theme::colour::textSecondary);
            g.setFont (theme::font (theme::type::caption));
            auto line = graph.getBottom() + 4.0f;
            auto text = [&] (const juce::String& t) {
                g.drawText (t, (int) area.getX() + 6, (int) line, (int) area.getWidth() - 12, 14, juce::Justification::centredLeft, true);
                line += 14.0f;
            };
            text (tr ("base {0}, env {1}", { juce::String::formatted ("%.2f", a.base), juce::String::formatted ("%+.2f", a.envLevel) }) + (a.bipolar ? tr (", bipolar") : tr (", unipolar")));
            if (a.envOn && a.count > 0)
                text (tr ("length {0} ", { juce::String::formatted ("%.2f", a.time[(std::size_t) a.count - 1]) }) + (a.tempo ? tr ("beats") : "s")
                      + (a.sustain >= 0 ? tr (", with sustain") : tr (", no sustain")));
            if (a.lowKey > 0 || a.highKey < 127)
                text (tr ("keys {0} to {1}", { juce::String (a.lowKey), juce::String (std::min (a.highKey, 127)) }));
            for (const auto& t : targets)
                text (utf8 ("-> ") + t);
        }
    }

private:
    AudioEngine& engine;
    int channel;
};

// Physical-model channels (Sakura, Drumpad; REA_rodada28): knobs for the main raw FL parameters (0..1)
class ModelPanel final : public juce::Component
{
public:
    ModelPanel (AudioEngine& e, int c)
        : engine (e),
          channel (c)
    {
        const auto& def = engine.getSong().channels[(std::size_t) channel];
        sakura = def.instrument == InstrumentKind::sakura;
        boobass = def.instrument == InstrumentKind::boobass;
        drumsynth = def.instrument == InstrumentKind::drumsynth;
        harmless = def.instrument == InstrumentKind::harmless;
        harmor = def.instrument == InstrumentKind::harmor;
        gms = def.instrument == InstrumentKind::gms;
        morphine = def.instrument == InstrumentKind::morphine;
        autogun = def.instrument == InstrumentKind::autogun;
        transistorBass = def.instrument == InstrumentKind::transistorbass;
        ogun = def.instrument == InstrumentKind::ogun || autogun; // Autogun: Ogun's parameter layout (REA_rodada57)
        miniSynth = def.instrument == InstrumentKind::minisynth;
        dx10 = def.instrument == InstrumentKind::dx10;
        kepler = def.instrument == InstrumentKind::kepler;
        poizone = def.instrument == InstrumentKind::poizone;
        fruitkick = def.instrument == InstrumentKind::fruitkick;
        bassdrum = def.instrument == InstrumentKind::bassdrum;
        toxic = def.instrument == InstrumentKind::toxic;
        threeOsc = def.instrument == InstrumentKind::threeosc;
        simSynth = def.instrument == InstrumentKind::simsynth;
        wasp = def.instrument == InstrumentKind::wasp;
        plucked = def.instrument == InstrumentKind::plucked;
        beepMap = def.instrument == InstrumentKind::beepmap;
        sawer = def.instrument == InstrumentKind::sawer;
        flKeys = def.instrument == InstrumentKind::flkeys;
        granulizer = def.instrument == InstrumentKind::granulizer;
        drumaxx = def.instrument == InstrumentKind::drumaxx;
        slayer = def.instrument == InstrumentKind::slayer;
        fruityDance = def.instrument == InstrumentKind::fruitydance;
        sytrus = def.instrument == InstrumentKind::sytrus;
        // DrumSynth Live: the patch on the key selected in FL's editor (raw FL values)
        if (drumsynth)
        {
            if (lynx::drumSynthEditorKeyIndex < (int) def.modelParams.size())
                editorKey = (int) def.modelParams[(std::size_t) lynx::drumSynthEditorKeyIndex];
            if (drumSynthKeyToSlot (editorKey) < 0)
                editorKey = 60;
            wavesFound = ! def.samplePath.empty();
        }
        const auto offset = drumsynth ? drumSynthPatchParams * drumSynthKeyToSlot (editorKey) : 0;
        static const std::pair<int, const char*> sakuraKnobs[] = {
            { 0, "Master" }, { 1, "Tune" }, { 2, "Attack" }, { 3, "Decay" }, { 4, "Sustain" }, { 5, "Release" },
            { 6, "Str1 decay" }, { 9, "Str1 damp" }, { 11, "Str2 decay" }, { 14, "Str2 damp" }, { 16, "Saturation" },
            { 17, "String mix" }, { 18, "Stereo sep" }, { 19, "Comb ratio" }, { 22, "Comb level" }, { 24, "Lo-cut" },
            { 29, "Hi-cut" }, { 30, "Hi-cut res" }, { 31, "Noise level" }, { 32, "Noise rate" } };
        static const std::pair<int, const char*> drumpadKnobs[] = {
            { 2, "Mallet amp" }, { 3, "Mallet decay" }, { 4, "Mallet noise" }, { 6, "Decay" }, { 7, "Cutoff" },
            { 8, "Tension" }, { 9, "Phase" }, { 10, "Material" }, { 11, "Size" }, { 12, "Shape" }, { 13, "Low freq" },
            { 18, "Mid freq" }, { 19, "Mid level" }, { 24, "High freq" }, { 25, "High level" }, { 27, "Lo-fi" },
            { 28, "Hold" }, { 29, "Pitch" } };
        auto add = [&] (int index, const char* name, double low = 0.0, double high = -1.0) {
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            index += offset;
            if (high > low)
                knob->setRange (low, high, 1.0);
            else
                knob->setRange (0.0, boobass ? 65535.0 : 1.0, boobass ? 1.0 : 0.001);
            knob->setValue (index < (int) def.modelParams.size() ? def.modelParams[(std::size_t) index] : 0.0, juce::dontSendNotification);
            knob->setName (name);
            knob->onValueChange = [this, index, k = knob.get()] {
                engine.editSong ([c = channel, index, v = (float) k->getValue()] (Song& song) {
                    auto& params = song.channels[(std::size_t) c].modelParams;
                    if (index < (int) params.size())
                        params[(std::size_t) index] = v;
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        static const std::pair<int, const char*> booBassKnobs[] = { { 0, "Bass" }, { 1, "Mid" }, { 2, "Treble" } };
        struct Ranged { int index; const char* name; double low, high; };
        static const Ranged drumSynthKnobs[] = {
            { 0, "Level", 0, 128 }, { 1, "Pan", -64, 64 }, { 2, "Tone", -50, 50 }, { 3, "Harmonics", -50, 50 },
            { 4, "Osc1 freq", 0, 1000 }, { 7, "Osc1 decay", 0, 127 }, { 8, "Osc1 level", 0, 127 }, { 10, "Osc2 freq", 0, 1000 },
            { 13, "Osc2 decay", 0, 127 }, { 14, "Osc2 level", 0, 127 }, { 17, "Wave mix", 0, 127 }, { 21, "Release", 0, 127 },
            { 23, "Click", 0, 100 }, { 33, "Ring mod", 0, 127 } };
        // Harmless: raw FL values (REA_rodada21/32 ranges)
        static const Ranged harmlessKnobs[] = {
            { 36, "Volume", 0, 2000 }, { 26, "Attack", 0, 1000 }, { 27, "Decay", 0, 1000 }, { 28, "Release", 0, 1000 },
            { 54, "Cutoff", 0, 8192 }, { 59, "Resonance", 0, 1000 }, { 52, "Filter env", -1000, 1000 },
            { 31, "Pluck", 0, 1000 }, { 23, "Mask mix", 0, 1000 }, { 65, "Unison", 1, 9 }, { 67, "Uni detune", 0, 256 },
            { 25, "Tremolo", 0, 1000 }, { 71, "Phaser mix", 0, 1000 }, { 79, "Harmonizer", 0, 1000 } };
        // Harmor: raw FL integers inside the saved state (part A, and the global master / A–B mix; REA_rodada34)
        struct HarmorKnob { int part, index; const char* name; double low, high; };
        static const HarmorKnob harmorKnobs[] = {
            { -1, 12, "Master vol", 0, 2000 }, { -1, 10, "A/B mix", 0, 1000 }, { 0, 11, "A volume", -1000, 1000 },
            { 0, 4, "Timbre mix", 0, 1000 }, { 0, 52, "Cutoff", 0, 8192 }, { 0, 57, "Resonance", 0, 1000 },
            { 0, 75, "Pluck", 0, 1000 }, { 0, 34, "Unison", 1, 9 }, { 0, 38, "Uni detune", 0, 256 },
            { 0, 27, "Harmonizer", 0, 1000 }, { 0, 76, "Phaser mix", 0, 1000 }, { 0, 23, "Harm. blur", 0, 10000 },
            { 0, 45, "Vibrato", 0, 1000 }, { 0, 17, "Tremolo", 0, 1000 } };
        auto addHarmor = [&] (const HarmorKnob& k) {
            const auto at = harmorParamOffset (def.pluginState, k.part, k.index);
            if (at < 0)
                return;
            std::int32_t value = 0;
            std::memcpy (&value, def.pluginState.data() + at, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (k.low, k.high, 1.0);
            knob->setValue (value, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, k, s = knob.get()] {
                engine.editSong ([c = channel, k, v = (std::int32_t) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (const auto o = harmorParamOffset (st, k.part, k.index); o >= 0)
                        std::memcpy (st.data() + o, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // GMS / Morphine: the plugin's own 0…1 floats inside the saved state (GMS P[i], Morphine global A[i]; REA_rodada39)
        struct FloatKnob { int index; const char* name; };
        static const FloatKnob gmsKnobs[] = {
            { 0, "Master" }, { 1, "Pan" }, { 8, "Level" }, { 4, "Attack" }, { 5, "Decay" }, { 6, "Sustain" }, { 7, "Release" },
            { 12, "Cutoff" }, { 13, "Resonance" }, { 16, "Env1 amount" }, { 50, "Modulation" }, { 48, "Osc2 mix" }, { 49, "Osc3 mix" },
            { 45, "Noise" }, { 41, "Unison" }, { 42, "Uni detune" }, { 43, "Stereo" }, { 10, "Slide time" } };
        static const FloatKnob morphineKnobs[] = {
            { 0, "Master" }, { 9, "Attack" }, { 10, "Decay" }, { 11, "Sustain" }, { 12, "Release" }, { 1, "Glide" }, { 4, "Vel curve" },
            { 7, "Overdrive" }, { 14, "Chorus mix" }, { 21, "Delay mix" }, { 25, "Reverb mix" }, { 36, "Noise mix" } };
        // BassDrum: its 23 floats 0…1 inside the saved state (REA_rodada15 §12.3 / rodada43 §2.2)
        static const FloatKnob bassDrumKnobs[] = {
            { 0, "Output" }, { 6, "Base" }, { 7, "Peak" }, { 8, "Slide" }, { 2, "Duration" }, { 5, "Attack" }, { 4, "Drive" },
            { 3, "Butt" }, { 15, "Click level" }, { 14, "Click freq" }, { 13, "Click cut" }, { 21, "Noise mix" }, { 22, "Noise filter" },
            { 20, "Noise pitch" }, { 19, "Noise start" }, { 1, "Sampling" } };
        // Toxic Biohazard: the plugin's 0…1 parameters P[i] inside the saved state (REA_rodada45)
        static const FloatKnob toxicKnobs[] = {
            { 0, "Gain" }, { 1, "Distortion" }, { 3, "Attack" }, { 4, "Decay" }, { 5, "Sustain" }, { 6, "Release" },
            { 15, "Cutoff" }, { 16, "Resonance" }, { 23, "Filter env" }, { 8, "Uni detune" }, { 9, "Uni pan" },
            { 10, "Glide" }, { 7, "Vel curve" }, { 2, "Transpose" } };
        // MiniSynth: its 43 floats 0…1 inside the saved state (REA_rodada41 §1.2)
        static const FloatKnob miniSynthKnobs[] = {
            { 5, "Osc kind" }, { 6, "Osc mod" }, { 7, "Noise" }, { 1, "Transpose" }, { 8, "Cutoff" }, { 9, "Resonance" },
            { 16, "Filter env" }, { 21, "Attack" }, { 22, "Decay" }, { 23, "Sustain" }, { 24, "Release" }, { 20, "LFO amount" },
            { 19, "LFO rate" }, { 2, "Slide time" } };
        // PoiZone: the plugin's own floats P[i] inside the saved state (REA_rodada44 §1.3)
        static const FloatKnob poizoneKnobs[] = {
            { 15, "Gain" }, { 8, "Osc balance" }, { 9, "Noise" }, { 18, "Cutoff" }, { 19, "Resonance" }, { 10, "EG amount" },
            { 22, "Attack" }, { 23, "Decay" }, { 24, "Sustain" }, { 25, "Release" }, { 26, "LFO amount" }, { 28, "LFO rate" },
            { 46, "Glide" }, { 38, "Delay wet" }, { 42, "Chorus wet" }, { 47, "Gate mix" } };
        auto paramOffset = [this] (const std::vector<std::uint8_t>& st, int index) {
            return gms ? gmsParamOffset (st, index)
                       : miniSynth ? miniSynthParamOffset (st, index)
                       : poizone ? poizoneParamOffset (st, index)
                       : bassdrum ? bassDrumParamOffset (st, index)
                       : toxic ? toxicParamOffset (st, index) : morphineParamOffset (st, index);
        };
        auto addFloat = [&] (const FloatKnob& k) {
            const auto at = paramOffset (def.pluginState, k.index);
            if (at < 0)
                return;
            float value = 0.0f;
            std::memcpy (&value, def.pluginState.data() + at, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (0.0, 1.0, 0.001);
            knob->setValue (std::isfinite (value) ? std::clamp (value, 0.0f, 1.0f) : 0.0f, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, k, paramOffset, s = knob.get()] {
                engine.editSong ([c = channel, k, paramOffset, v = (float) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (const auto o = paramOffset (st, k.index); o >= 0)
                        std::memcpy (st.data() + o, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // SimSynth: its 45 doubles 0…1 at the start of the saved state (REA_rodada64 §1.1)
        static const FloatKnob simSynthKnobs[] = {
            { 3, "Osc 1 level" }, { 10, "Osc 2 level" }, { 17, "Osc 3 level" }, { 26, "Cutoff" }, { 27, "Resonance" },
            { 28, "Filter env" }, { 30, "Key track" }, { 35, "Attack" }, { 36, "Decay" }, { 37, "Sustain" }, { 38, "Release" },
            { 22, "LFO rate" }, { 29, "LFO cutoff" }, { 39, "Volume" } };
        auto addDouble = [&] (const FloatKnob& k) {
            const auto at = 8 * (std::size_t) k.index;
            if (! simSynthStateUsable (def.pluginState))
                return;
            double value = 0.0;
            std::memcpy (&value, def.pluginState.data() + at, 8);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (0.0, 1.0, 0.001);
            knob->setValue (std::isfinite (value) ? std::clamp (value, 0.0, 1.0) : 0.0, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, at, s = knob.get()] {
                engine.editSong ([c = channel, at, v = s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (simSynthStateUsable (st))
                        std::memcpy (st.data() + at, &v, 8);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // FL Slayer: the 28 parameter integers of the saved state (REA_rodada68 §6.1), ranges of the plugin's controls
        static const Ranged slayerKnobs[] = {
            { 0, "Strings", 0, 6 }, { 1, "Coils", 0, 2 }, { 2, "Pickup", 0, 65535 }, { 3, "Tone", 0, 65535 }, { 4, "Slap", 0, 65535 },
            { 5, "Fret", 0, 65535 }, { 6, "Harmonic", 0, 65535 }, { 7, "Damp", 0, 65535 }, { 8, "Vel volume", 0, 65535 },
            { 9, "Vel damp", 0, 65535 }, { 12, "Amp type", 0, 3 }, { 13, "Drive", 0, 65535 }, { 14, "Presence", 0, 65535 },
            { 15, "Feedback", 0, 65535 }, { 16, "EQ low", 0, 65535 }, { 17, "EQ mid", 0, 65535 }, { 18, "EQ high", 0, 65535 },
            { 19, "Cabinet", 0, 3 }, { 20, "Effect", 0, 15 }, { 21, "FX par 1", 0, 65535 }, { 22, "FX par 2", 0, 65535 },
            { 24, "Mode", 0, 5 }, { 25, "Timing", 0, 65535 }, { 26, "Latch", 0, 1 }, { 27, "Glissando", 0, 1 } };
        auto addSlayer = [&] (const Ranged& k) {
            const auto at = 4 + 4 * (std::size_t) k.index;
            if (! slayerStateUsable (def.pluginState) || at + 4 > def.pluginState.size())
                return;
            std::int32_t value = 0;
            std::memcpy (&value, def.pluginState.data() + at, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange ((double) k.low, (double) k.high, 1.0);
            knob->setValue ((double) std::clamp ((int) value, (int) k.low, (int) k.high), juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, at, s = knob.get()] {
                engine.editSong ([c = channel, at, v = (std::int32_t) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (slayerStateUsable (st) && at + 4 <= st.size())
                        std::memcpy (st.data() + at, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Sytrus: its "Main settings" page, raw FL integers inside the saved (zlib) state (REA_rodada74 §3)
        auto addSytrus = [&] (int setting) {
            const auto value = sytrus::settingValue (def.pluginState, setting);
            if (! value)
                return;
            const auto& d = sytrus::settings()[(std::size_t) setting];
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (d.low, d.high, 1.0);
            knob->setValue (std::clamp (*value, d.low, d.high), juce::dontSendNotification);
            knob->setName (d.name);
            knob->onValueChange = [this, setting, s = knob.get()] {
                engine.editSong ([c = channel, setting, v = (int) s->getValue()] (Song& song) {
                    sytrus::setSetting (song.channels[(std::size_t) c].pluginState, setting, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Ogun: raw FL integers P0…P63 inside the saved (zlib) state (REA_rodada37 §1.3)
        static const Ranged ogunKnobs[] = {
            { 0, "Master vol", 0, 256 }, { 5, "Pre-decay", 0, 1000 }, { 6, "Decay", 0, 10000 }, { 7, "Release", 0, 10000 },
            { 8, "Fullness", -1000, 1000 }, { 9, "Morph", 0, 1000 }, { 10, "Seed 1", 0, 1000 }, { 11, "Seed 2", 0, 1000 },
            { 52, "Richness", 6, 16 }, { 12, "Unison", 1, 9 }, { 15, "Uni pitch", 0, 256 }, { 17, "Cutoff", 0, 256 },
            { 18, "Resonance", 0, 256 }, { 3, "Mod X", 0, 512 }, { 4, "Mod Y", 0, 512 }, { 54, "Quality", 0, 4 } };
        static const Ranged autogunKnobs[] = { { 63, "Magic dust", 0, 1000 } };
        auto addOgun = [&] (const Ranged& k) {
            const auto value = ogunParam (def.pluginState, k.index);
            if (! value)
                return;
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (k.low, k.high, 1.0);
            knob->setValue (*value, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, index = k.index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (int) s->getValue()] (Song& song) {
                    ogunSetParam (song.channels[(std::size_t) c].pluginState, index, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Fruit Kick: its six integers (REA_rodada10 §4.2 ranges)
        static const Ranged fruitKickKnobs[] = {
            { 0, "Max freq", -2400, 3600 }, { 1, "Min freq", -1200, 1200 }, { 2, "Freq decay", 0, 256 }, { 3, "Amp decay", 0, 256 },
            { 4, "Click", 0, 64 }, { 5, "Distortion", 0, 128 } };
        auto addFruitKick = [&] (const Ranged& k) {
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (k.low, k.high, 1.0);
            knob->setValue (fruitKickParam (def.pluginState, k.index), juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, index = k.index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (int) s->getValue()] (Song& song) {
                    fruitKickSetParam (song.channels[(std::size_t) c].pluginState, index, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Fruity DX10: the 22 raw FL integers of the saved state (REA_rodada42 §1.2)
        static const Ranged dx10Knobs[] = {
            { 0, "Attack", 0, 65536 }, { 1, "Decay", 0, 65536 }, { 2, "Release", 0, 65536 }, { 21, "Coarse", -3, 3 },
            { 11, "Waveform", 0, 65536 }, { 10, "Vibrato", 0, 65536 }, { 13, "LFO rate", 0, 65536 },
            { 3, "Mod 1 coarse", 0, 65536 }, { 4, "Mod 1 fine", 0, 65536 }, { 5, "Mod 1 init", 0, 65536 }, { 6, "Mod 1 time", 0, 65536 },
            { 7, "Mod 1 sustain", 0, 65536 }, { 8, "Mod 1 release", 0, 65536 }, { 9, "Mod 1 velocity", 0, 65536 },
            { 14, "Mod 2 coarse", 0, 65536 }, { 15, "Mod 2 fine", 0, 65536 }, { 16, "Mod 2 init", 0, 65536 }, { 17, "Mod 2 time", 0, 65536 },
            { 18, "Mod 2 sustain", 0, 65536 }, { 19, "Mod 2 release", 0, 65536 }, { 20, "Mod 2 velocity", 0, 65536 }, { 12, "Mod thru", 0, 65536 } };
        auto addDx10 = [&] (const Ranged& k) {
            const auto value = dx10Param (def.pluginState, k.index);
            if (! value)
                return;
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (k.low, k.high, 1.0);
            knob->setValue (*value, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, index = k.index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (int) s->getValue()] (Song& song) {
                    dx10SetParam (song.channels[(std::size_t) c].pluginState, index, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // 3x Osc: the 21 parameters of the saved state, then invert ×3, AM and HQ (REA_rodada47 §1.1)
        static const char* const threeOscNames[] = {
            "Osc 1 pan", "Osc 1 shape", "Osc 1 coarse", "Osc 1 fine", "Osc 1 phase", "Osc 1 detune", "Osc 2 mix", "Osc 2 pan",
            "Osc 2 shape", "Osc 2 coarse", "Osc 2 fine", "Osc 2 phase", "Osc 2 detune", "Osc 3 mix", "Osc 3 pan", "Osc 3 shape",
            "Osc 3 coarse", "Osc 3 fine", "Osc 3 phase", "Osc 3 detune", "Phase rand", "Invert 1", "Invert 2", "Invert 3",
            "Osc 3 AM", "HQ" };
        auto addThreeOsc = [&] (int index) {
            const auto value = threeOscParam (def.pluginState, index);
            if (! value)
                return;
            const auto [low, high] = threeOscParamRange (index);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (low, high, 1.0);
            knob->setValue (*value, juce::dontSendNotification);
            knob->setName (threeOscNames[index]);
            knob->onValueChange = [this, index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (int) s->getValue()] (Song& song) {
                    threeOscSetParam (song.channels[(std::size_t) c].pluginState, index, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Fruity Granulizer: the 12 parameters of the saved state (REA_rodada67 §1)
        static const char* const granulizerNames[] = {
            "Grain attack", "Grain hold", "Grain spacing", "Wave spacing", "Effect depth", "Effect speed", "Randomness",
            "Stereo", "Loop", "Freeze", "Sample start", "Transient hold" };
        auto addGranulizer = [&] (int index) {
            const auto value = granulizerParam (def.pluginState, index);
            if (! value)
                return;
            const auto [low, high] = granulizerParamRange (index);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (low, high, 1.0);
            knob->setValue (*value, juce::dontSendNotification);
            knob->setName (granulizerNames[index]);
            knob->onValueChange = [this, index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (int) s->getValue()] (Song& song) {
                    granulizerSetParam (song.channels[(std::size_t) c].pluginState, index, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Drumaxx: the master level and the 16 pad volumes, 0…1 floats of the saved state (REA_rodada67 §6)
        auto addDrumaxx = [&] (int index, const juce::String& name) {
            const auto value = drumaxxParam (def.pluginState, index);
            if (! std::isfinite (value))
                return;
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (0.0, 1.0, 0.001);
            knob->setValue (std::clamp ((double) value, 0.0, 1.0), juce::dontSendNotification);
            knob->setName (name);
            knob->onValueChange = [this, index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (float) s->getValue()] (Song& song) {
                    drumaxxSetParam (song.channels[(std::size_t) c].pluginState, index, v);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Kepler: the panel's 0…1 floats inside the saved state ("Kepler-1" order, REA_rodada46 §1.1)
        static const FloatKnob keplerKnobs[] = {
            { 33, "Volume" }, { 19, "Cutoff" }, { 20, "Resonance" }, { 22, "Env amount" }, { 26, "Attack" }, { 27, "Decay" },
            { 28, "Sustain" }, { 29, "Release" }, { 11, "PW" }, { 16, "Sub level" }, { 17, "Noise" }, { 5, "LFO rate" },
            { 10, "Vibrato" }, { 18, "High-pass" }, { 30, "Chorus" } };
        auto addKepler = [&] (const FloatKnob& k) {
            const auto at = keplerPanelOffset (def.pluginState, k.index);
            if (at < 0)
                return;
            float value = 0.0f;
            std::memcpy (&value, def.pluginState.data() + at, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (0.0, 1.0, 0.001);
            knob->setValue (std::isfinite (value) ? std::clamp (value, 0.0f, 1.0f) : 0.0f, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, k, s = knob.get()] {
                engine.editSong ([c = channel, k, v = (float) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (const auto o = keplerPanelOffset (st, k.index); o >= 0)
                        std::memcpy (st.data() + o, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Plucked!: the five int32 knobs of the saved state (REA_rodada65 §1.1)
        static const Ranged pluckedKnobs[] = { { 0, "Decay", 0, 256 }, { 1, "Color", 0, 128 }, { 2, "Normalize", 0, 1 },
                                               { 3, "Gate", 0, 1 }, { 4, "Widen", 0, 1 } };
        auto addPlucked = [&] (const Ranged& k) {
            if (def.pluginState.size() < 20)
                return;
            std::int32_t value = 0;
            std::memcpy (&value, def.pluginState.data() + 4 * k.index, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (k.low, k.high, 1.0);
            knob->setValue (std::clamp ((double) value, k.low, k.high), juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, k, s = knob.get()] {
                engine.editSong ([c = channel, k, v = (std::int32_t) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (st.size() >= 20)
                        std::memcpy (st.data() + 4 * k.index, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // BeepMap: the int32 settings of the saved state (REA_rodada65 §2.1; Frequency's range not confirmed)
        static const Ranged beepMapKnobs[] = { { 0, "Frequency", 0, 9600 }, { 1, "Length", 0, 4096 }, { 2, "Scale", 0, 2 },
                                               { 3, "Use blue", 0, 1 }, { 4, "Grainy", 0, 1 }, { 5, "Loop", 0, 1 }, { 6, "Widen", 0, 1 } };
        auto addBeepMap = [&] (const Ranged& k) {
            const auto at = beepMapParamOffset (def.pluginState, k.index);
            if (at < 0)
                return;
            std::int32_t value = 0;
            std::memcpy (&value, def.pluginState.data() + at, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (k.low, k.high, 1.0);
            knob->setValue (std::clamp ((double) value, k.low, k.high), juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, k, s = knob.get()] {
                engine.editSong ([c = channel, k, v = (std::int32_t) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (const auto o = beepMapParamOffset (st, k.index); o >= 0)
                        std::memcpy (st.data() + o, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // Sawer: 0…1 floats of the saved state (REA_rodada65 §3.3)
        static const FloatKnob sawerKnobs[] = {
            { 0, "Level" }, { 27, "Cutoff" }, { 28, "Resonance" }, { 36, "EGF amount" }, { 2, "Attack" }, { 3, "Decay" },
            { 4, "Sustain" }, { 5, "Release" }, { 19, "Sub level" }, { 18, "Sub harmonic" }, { 26, "Noise" }, { 14, "Unison" },
            { 15, "Uni detune" }, { 45, "LFO amount" } };
        auto addSawer = [&] (const FloatKnob& k) {
            const auto at = sawerParamOffset (def.pluginState, k.index);
            if (at < 0)
                return;
            float value = 0.0f;
            std::memcpy (&value, def.pluginState.data() + at, 4);
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (0.0, 1.0, 0.001);
            knob->setValue (std::isfinite (value) ? std::clamp (value, 0.0f, 1.0f) : 0.0f, juce::dontSendNotification);
            knob->setName (k.name);
            knob->onValueChange = [this, k, s = knob.get()] {
                engine.editSong ([c = channel, k, v = (float) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (const auto o = sawerParamOffset (st, k.index); o >= 0)
                        std::memcpy (st.data() + o, &v, 4);
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        // FL Keys: the 15 int32 knobs of the saved state (REA_rodada69 §8.1)
        auto addFlKeys = [&] (int index) {
            const auto state = flkeys::parseState (def.pluginState);
            if (! state)
                return;
            const auto& r = flkeys::knobRange[index];
            auto knob = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
            knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            knob->setRange (r.min, r.max, 1.0);
            knob->setValue (std::clamp ((double) state->knobs[(std::size_t) index], (double) r.min, (double) r.max), juce::dontSendNotification);
            knob->setName (r.name);
            knob->onValueChange = [this, index, s = knob.get()] {
                engine.editSong ([c = channel, index, v = (int) s->getValue()] (Song& song) {
                    auto& st = song.channels[(std::size_t) c].pluginState;
                    if (auto parsed = flkeys::parseState (st))
                    {
                        parsed->knobs[(std::size_t) index] = v;
                        st = flkeys::writeState (*parsed);
                    }
                });
            };
            addAndMakeVisible (*knob);
            knobs.push_back (std::move (knob));
        };
        if (flKeys)
            for (int i = 0; i < 15; ++i)
                addFlKeys (i);
        else if (plucked)
            for (const auto& k : pluckedKnobs)
                addPlucked (k);
        else if (beepMap)
            for (const auto& k : beepMapKnobs)
                addBeepMap (k);
        else if (sawer)
            for (const auto& k : sawerKnobs)
                addSawer (k);
        else if (threeOsc)
            for (int i = 0; i < threeOscParamCount(); ++i)
                addThreeOsc (i);
        else if (granulizer)
            for (int i = 0; i < granulizerParamCount(); ++i)
                addGranulizer (i);
        else if (drumaxx)
        {
            addDrumaxx (0x2c1, "Master");
            for (int pad = 0; pad < 16; ++pad)
                addDrumaxx (pad * 44, "Pad " + juce::String (pad + 1));
        }
        else if (dx10)
            for (const auto& k : dx10Knobs)
                addDx10 (k);
        else if (kepler)
            for (const auto& k : keplerKnobs)
                addKepler (k);
        else if (gms)
            for (const auto& k : gmsKnobs)
                addFloat (k);
        else if (bassdrum)
            for (const auto& k : bassDrumKnobs)
                addFloat (k);
        else if (fruitkick)
            for (const auto& k : fruitKickKnobs)
                addFruitKick (k);
        else if (morphine)
            for (const auto& k : morphineKnobs)
                addFloat (k);
        else if (poizone)
            for (const auto& k : poizoneKnobs)
                addFloat (k);
        else if (miniSynth)
            for (const auto& k : miniSynthKnobs)
                addFloat (k);
        else if (simSynth)
            for (const auto& k : simSynthKnobs)
                addDouble (k);
        else if (wasp || fruityDance)
            ;
        else if (slayer)
            for (const auto& k : slayerKnobs)
                addSlayer (k);
        else if (sytrus)
            for (int i = 0; i < (int) sytrus::settings().size(); ++i)
                addSytrus (i);
        else if (toxic)
            for (const auto& k : toxicKnobs)
                addFloat (k);
        else if (ogun)
        {
            for (const auto& k : ogunKnobs)
                addOgun (k);
            if (autogun)
                for (const auto& k : autogunKnobs)
                    addOgun (k);
        }
        else if (harmor)
            for (const auto& k : harmorKnobs)
                addHarmor (k);
        else if (harmless)
            for (const auto& k : harmlessKnobs)
                add (k.index, k.name, k.low, k.high);
        else if (drumsynth)
            for (const auto& k : drumSynthKnobs)
                add (k.index, k.name, k.low, k.high);
        else if (boobass)
            for (const auto& [index, name] : booBassKnobs)
                add (index, name);
        else if (sakura)
            for (const auto& [index, name] : sakuraKnobs)
                add (index, name);
        else
            for (const auto& [index, name] : drumpadKnobs)
                add (index, name);
        const auto rows = ((int) knobs.size() + 6) / 7; // the grid of resized()
        setSize (720, std::max (dx10 || threeOsc ? 500 : 384, 46 + rows * 110 + 8));

        // FL's arrangement of the plugin's page (core/PluginLayout.cpp): the knobs at FL's places, faders and
        // switches drawn by the panel
        layout = plugui::instrumentLayout (def.instrument);
        // a state the engine cannot read gives no knobs: keep the plain page then
        if (layout != nullptr && std::none_of (knobs.begin(), knobs.end(), [this] (const auto& k) { return itemFor (k->getName()) != nullptr; }))
            layout = nullptr;
        if (layout != nullptr)
        {
            accent = flKeys ? skin::colour::azure : boobass || transistorBass ? skin::colour::ember : harmor || sytrus || harmless || morphine ? skin::colour::violet
                                                                                                                                           : skin::colour::crimson;
            // round 126: the page's own style (FL Keys hardware, BooBass vivid, the synths neon)
            pageStyle = flKeys ? skin::Style::hardware : boobass || transistorBass ? skin::Style::vivid : skin::Style::neon;
            for (auto& k : knobs)
            {
                k->getProperties().set (skin::sliderProperty, (juce::int64) accent.getARGB());
                k->getProperties().set (skin::styleProperty, (int) pageStyle);
                const auto* item = itemFor (k->getName());
                if (item == nullptr || (item->type != plugui::Item::knob && item->type != plugui::Item::vslider && item->type != plugui::Item::hslider))
                {
                    k->setVisible (false); // edited by a switch of the panel (or not shown by FL)
                    continue;
                }
                k->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                if (item->type == plugui::Item::vslider)
                    k->setSliderStyle (juce::Slider::LinearVertical);
                if (item->type == plugui::Item::hslider)
                    k->setSliderStyle (juce::Slider::LinearHorizontal);
                k->setPopupDisplayEnabled (true, true, this);
                k->setTooltip (k->getName());
            }
            // FL's size × 1.25, at most about 1200 × 760 (big pages such as Harmor's fit a laptop screen)
            const auto f = juce::jmin (layoutScale, 1200.0f / (float) layout->width, 720.0f / (float) layout->height);
            setSize ((int) std::ceil (f * (float) layout->width) + 16, (int) std::ceil (f * (float) layout->height) + layoutTop + 8);
        }
    }

    // --- FL's arrangement ------------------------------------------------------------------------------

    static constexpr float layoutScale = 1.25f;
    static constexpr int layoutTop = 34;

    const plugui::LayoutItem* itemFor (const juce::String& name) const
    {
        if (layout == nullptr)
            return nullptr;
        const plugui::LayoutItem* any = nullptr;
        for (const auto& item : layout->items) // the knob / fader of the parameter first (a pad may share it)
            if (item.param != nullptr && name == item.param)
            {
                if (item.type == plugui::Item::knob || item.type == plugui::Item::vslider)
                    return &item;
                if (any == nullptr && item.type != plugui::Item::display)
                    any = &item;
            }
        return any;
    }

    juce::Slider* knobFor (const char* name) const
    {
        for (const auto& k : knobs)
            if (name != nullptr && k->getName() == name)
                return k.get();
        return nullptr;
    }

    float layoutFactor() const
    {
        if (layout == nullptr)
            return 1.0f;
        return juce::jmax (0.4f, juce::jmin ((float) (getWidth() - 16) / (float) layout->width, (float) (getHeight() - layoutTop - 8) / (float) layout->height));
    }

    juce::Rectangle<float> layoutRect (const plugui::LayoutItem& item) const
    {
        const auto f = layoutFactor();
        return { 8.0f + item.x * f, (float) layoutTop + item.y * f, item.w * f, item.h * f };
    }

    static float normalised (const juce::Slider& k)
    {
        const auto range = k.getMaximum() - k.getMinimum();
        return range > 0.0 ? (float) ((k.getValue() - k.getMinimum()) / range) : 0.0f;
    }

    static void setNormalised (juce::Slider& k, float v)
    {
        k.setValue (k.getMinimum() + (double) v * (k.getMaximum() - k.getMinimum()), juce::sendNotificationSync);
    }

    // the radio's option cell (grids fill row by row)
    juce::Rectangle<float> optionCell (const plugui::LayoutItem& item, int o, int count) const
    {
        const auto r = layoutRect (item);
        const auto columns = juce::jmax (1, item.columns);
        const auto rows = (count + columns - 1) / columns;
        const auto w = r.getWidth() / (float) columns, h = r.getHeight() / (float) juce::jmax (1, rows);
        return { r.getX() + w * (float) (o % columns), r.getY() + h * (float) (o / columns), w, h };
    }

    // a small drawing of an oscillator shape (3x Osc's shape buttons)
    static void drawShape (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& name)
    {
        juce::Path p;
        const auto x0 = r.getX(), w = r.getWidth(), yc = r.getCentreY(), a = r.getHeight() * 0.5f;
        const auto n = 48;
        for (int i = 0; i <= n; ++i)
        {
            const auto t = (float) i / (float) n;
            float y = 0.0f;
            if (name == "Sine") y = std::sin (t * juce::MathConstants<float>::twoPi);
            else if (name == "Square") y = t < 0.5f ? 1.0f : -1.0f;
            else if (name == "Rounded") y = t < 0.46f ? 1.0f - 2.0f * std::pow (0.997f, t * 16384.0f) : (t < 0.83f ? 1.0f : -1.0f);
            else if (name == "Triangle") y = t < 0.25f ? 4.0f * t : (t < 0.75f ? 2.0f - 4.0f * t : 4.0f * t - 4.0f);
            else if (name == "Saw") y = 2.0f * t - 1.0f;
            else if (name == "Noise") y = std::sin ((float) i * 12.9898f) * std::cos ((float) i * 4.1414f) * 1.6f;
            else /* custom */ y = std::sin (t * juce::MathConstants<float>::twoPi * 3.0f) * std::exp (-std::abs (t - 0.5f) * 5.0f);
            y = juce::jlimit (-1.0f, 1.0f, y);
            const juce::Point<float> pt { x0 + w * t, yc - a * y };
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void paintLayout (juce::Graphics& g)
    {
        using namespace theme;
        const skin::StyleScope styleScope (skin::resolve (pageStyle));
        const auto f = layoutFactor();
        const auto& def = engine.getSong().channels[(std::size_t) channel];
        const auto caption = [&] (const char* text, juce::Rectangle<float> r, bool dimmed, juce::Justification j) {
            if (text == nullptr || *text == 0)
                return;
            g.setColour (dimmed ? colour::textDisabled : colour::textSecondary);
            g.setFont (font (juce::jmax (8.0f, 9.5f * f), true));
            g.drawFittedText (caps (tr (text)), r.toNearestInt(), j, 1, 0.7f);
        };

        for (const auto& item : layout->items)
        {
            const auto r = layoutRect (item);
            const auto dimmed = (item.style & plugui::dim) != 0;
            auto* k = knobFor (item.param);
            switch (item.type)
            {
                case plugui::Item::section:
                    skin::section (g, r, radius::lg * f, (item.style & plugui::raised) != 0, accent);
                    if (item.label != nullptr && (item.style & plugui::boxed) == 0)
                        skin::sectionTitle (g, r.withHeight (16.0f * f).translated (0.0f, 1.0f * f), caps (tr (item.label)), accent, juce::jmax (8.0f, 10.0f * f));
                    break;
                case plugui::Item::label:
                    if (item.label != nullptr && juce::String (item.label).length() <= 2 && dimmed) // big dim numbers (3x Osc rows)
                    {
                        g.setColour (colour::bg4);
                        g.setFont (font (r.getHeight() * 0.9f, true));
                        g.drawText (item.label, r, juce::Justification::centred, false);
                    }
                    else if (dimmed && r.getHeight() > 30.0f * f) // the plugin's name, big and quiet
                    {
                        g.setColour (colour::bg4);
                        g.setFont (font (r.getHeight() * 0.6f, true));
                        g.drawFittedText (item.label, r.toNearestInt(),
                                          (item.style & plugui::alignLeft) != 0 ? juce::Justification::centredLeft : juce::Justification::centredRight, 1, 0.8f);
                    }
                    else
                        caption (item.label, r, dimmed, (item.style & plugui::alignLeft) != 0 ? juce::Justification::centredLeft : juce::Justification::centred);
                    break;
                case plugui::Item::knob:
                    if ((item.style & plugui::noCaption) == 0)
                        caption (item.label, r.withTop (r.getBottom() - 12.0f * f), dimmed, juce::Justification::centred);
                    break;
                case plugui::Item::vslider:
                case plugui::Item::hslider:
                    if ((item.style & plugui::noCaption) == 0)
                        caption (item.label, r.withTop (r.getBottom() - 12.0f * f).expanded (16.0f * f, 0.0f), dimmed, juce::Justification::centred);
                    break;
                case plugui::Item::radio:
                    if (k != nullptr)
                    {
                        const auto labels = plugui::optionLabels (item);
                        const auto chosen = plugui::optionAt (item, normalised (*k));
                        for (int o = 0; o < (int) labels.size(); ++o)
                        {
                            const auto cell = optionCell (item, o, (int) labels.size()).reduced (3.0f * f);
                            skin::button (g, cell, o == chosen, false, accent, radius::md * f);
                            g.setColour (o == chosen ? accent.brighter (0.5f) : colour::textSecondary);
                            if (item.columns > 0)
                                drawShape (g, cell.reduced (cell.getWidth() * 0.22f, cell.getHeight() * 0.3f), labels[(std::size_t) o]);
                            else
                                g.drawFittedText (tr (labels[(std::size_t) o]), cell.toNearestInt(), juce::Justification::centred, 1, 0.7f);
                        }
                    }
                    break;
                case plugui::Item::toggle:
                    if (k != nullptr)
                    {
                        const auto on = normalised (*k) >= 0.5f;
                        if ((item.style & plugui::boxed) != 0)
                        {
                            const auto box = r.reduced (3.0f * f);
                            skin::button (g, box, on, false, accent, radius::md * f);
                            g.setColour (on ? colour::onAccent : colour::textSecondary);
                            g.setFont (font (juce::jmax (8.0f, 11.0f * f), true));
                            g.drawText (item.label != nullptr ? item.label : "", box, juce::Justification::centred, false);
                        }
                        else
                        {
                            const auto dot = juce::Rectangle<float> (10.0f * f, 10.0f * f).withCentre ({ r.getX() + 6.0f * f, r.getCentreY() });
                            if (on)
                            {
                                g.setColour (accent.withAlpha (0.3f));
                                g.fillEllipse (dot.expanded (3.0f * f));
                            }
                            g.setColour (on ? accent : juce::Colour (0xff0b0c0f));
                            g.fillEllipse (dot);
                            g.setColour (juce::Colours::white.withAlpha (on ? 0.4f : 0.08f));
                            g.drawEllipse (dot.reduced (0.5f), 1.0f);
                            caption (item.label, r.withTrimmedLeft (14.0f * f), false, juce::Justification::centredLeft);
                        }
                    }
                    break;
                case plugui::Item::stepper:
                    if (k != nullptr)
                    {
                        auto box = r;
                        if (item.label != nullptr)
                            caption (item.label, box.removeFromTop (12.0f * f), false, juce::Justification::centred);
                        skin::valuePlate (g, box, radius::sm * f);
                        g.setColour (colour::textPrimary);
                        g.setFont (font (juce::jmax (9.0f, 12.0f * f), true));
                        // oversampling orders read as FL shows them (0 → 1x … 3 → 8x)
                        const auto text = juce::String (item.param).containsIgnoreCase ("oversampling") ? juce::String (1 << juce::jlimit (0, 8, (int) k->getValue())) + "x"
                                                                                                       : juce::String ((int) k->getValue());
                        g.drawText (text, box.withTrimmedRight (8.0f * f), juce::Justification::centred, false);
                        g.setColour (colour::textSecondary);
                        g.drawText (juce::String::fromUTF8 ("\xe2\x96\xb4"), box.withTrimmedLeft (box.getWidth() - 10.0f * f).withHeight (box.getHeight() * 0.5f), juce::Justification::centred, false);
                        g.drawText (juce::String::fromUTF8 ("\xe2\x96\xbe"), box.withTrimmedLeft (box.getWidth() - 10.0f * f).withTrimmedTop (box.getHeight() * 0.5f), juce::Justification::centred, false);
                    }
                    break;
                case plugui::Item::display:
                    skin::well (g, r, radius::md * f);
                    if (juce::String (item.display) == "xy")
                        if (auto* kx = knobFor (item.param); kx != nullptr)
                            if (auto* ky = knobFor (item.param2); ky != nullptr)
                            {
                                g.setColour (colour::gridBeat);
                                g.drawVerticalLine ((int) r.getCentreX(), r.getY(), r.getBottom());
                                g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
                                const juce::Point<float> pt { r.getX() + r.getWidth() * normalised (*kx), r.getBottom() - r.getHeight() * normalised (*ky) };
                                g.setColour (accent.withAlpha (0.3f));
                                g.fillEllipse (juce::Rectangle<float> (16.0f * f, 16.0f * f).withCentre (pt));
                                g.setColour (accent);
                                g.drawEllipse (juce::Rectangle<float> (10.0f * f, 10.0f * f).withCentre (pt), 2.0f);
                            }
                    if (juce::String (item.display) == "flkeysname")
                        if (const auto st = flkeys::parseState (def.pluginState))
                        {
                            g.setColour (colour::textPrimary);
                            g.setFont (font (juce::jmax (9.0f, 11.0f * f)));
                            g.drawFittedText (juce::String::fromUTF8 (st->wavetable.c_str()), r.reduced (6.0f * f, 0.0f).toNearestInt(),
                                              juce::Justification::centredLeft, 1, 0.8f);
                        }
                    break;
                default:
                    break;
            }
        }
    }

    void resizeLayout()
    {
        for (auto& k : knobs)
        {
            const auto* item = itemFor (k->getName());
            if (item == nullptr || ! k->isVisible())
                continue;
            auto r = layoutRect (*item);
            if ((item->type == plugui::Item::vslider || item->type == plugui::Item::hslider) && (item->style & plugui::noCaption) == 0 && item->label != nullptr)
                r.removeFromBottom (13.0f * layoutFactor());
            if (item->type == plugui::Item::knob)
            {
                if ((item->style & plugui::noCaption) == 0)
                    r.removeFromBottom (14.0f * layoutFactor());
                const auto d = juce::jmin (r.getWidth(), r.getHeight());
                r = r.withSizeKeepingCentre (d, d).withY (r.getY());
            }
            k->setBounds (r.toNearestInt());
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (xyItem == nullptr)
            return;
        const auto r = layoutRect (*xyItem);
        if (auto* kx = knobFor (xyItem->param))
            setNormalised (*kx, juce::jlimit (0.0f, 1.0f, (e.position.x - r.getX()) / r.getWidth()));
        if (auto* ky = knobFor (xyItem->param2))
            setNormalised (*ky, juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight()));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { xyItem = nullptr; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (layout == nullptr)
            return;
        for (const auto& item : layout->items)
        {
            auto* k = knobFor (item.param);
            if (k == nullptr || ! layoutRect (item).contains (e.position))
                continue;
            if (item.type == plugui::Item::display && juce::String (item.display) == "xy")
            {
                xyItem = &item;
                mouseDrag (e);
                return;
            }
            if (item.type == plugui::Item::stepper)
            {
                const auto r = layoutRect (item);
                const auto up = e.position.y < r.getY() + (item.label != nullptr ? 12.0f * layoutFactor() : 0.0f) + (r.getHeight() - (item.label != nullptr ? 12.0f * layoutFactor() : 0.0f)) * 0.5f;
                k->setValue (k->getValue() + (up ? 1.0 : -1.0), juce::sendNotificationSync);
            }
            else if (item.type == plugui::Item::toggle)
                setNormalised (*k, normalised (*k) >= 0.5f ? 0.0f : 1.0f);
            else if (item.type == plugui::Item::radio)
            {
                const auto count = (int) plugui::optionLabels (item).size();
                for (int o = 0; o < count; ++o)
                    if (optionCell (item, o, count).contains (e.position))
                        setNormalised (*k, plugui::optionValue (item, o));
            }
            repaint();
            return;
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        if (layout != nullptr) // a plugin page in FL's arrangement: the plugin skin's canvas
            skin::canvas (g, getLocalBounds().toFloat(), accent);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (layout != nullptr ? theme::type::body : theme::type::title));
        g.drawText (drumsynth ? tr ("DrumSynth Live — FL's engine, patch on key {0}", { juce::String (editorKey) })
                                    + (wavesFound ? tr (" (next hit; recorded waves from your FL installation)")
                                                  : tr (" (next hit; recorded waves not found: silent)"))
                    : harmless ? tr ("Harmless — FL's additive engine (chorus/delay/reverb/compressor rack off)")
                    : harmor ? tr ("Harmor — FL's additive engine (effects rack off; image synthesis not decoded)")
                    : gms ? tr ("GMS — FL's engine (waves read from your FL installation)")
                    : morphine ? tr ("Morphine — FL's additive engine (new settings restart the voices)")
                    : transistorBass ? tr ("Transistor Bass — FL's engine with its sequencer and effects (oscillator cycles read from your FL installation)")
                    : autogun ? tr ("Autogun — FL's engine with its effects and Magic Dust (SG.dat read from your FL installation)")
                    : ogun ? tr ("Ogun — FL's engine with its effects (Seed 2 Random is a fixed draw per channel)")
                    : poizone ? tr ("PoiZone — FL's engine with its gate, chorus, delay and arpeggiator (new settings restart the voices)")
                    : miniSynth ? tr ("MiniSynth — FL's engine with its effects (wave tables read from your FL installation)")
                    : dx10 ? tr ("Fruity DX10 — FL's FM engine")
                    : kepler ? tr ("Kepler — FL's engine (LFO shape and noise read from your FL installation; new settings restart the voices)")
                    : fruitkick ? tr ("Fruit Kick — FL's engine (channel envelopes stepped per tick)")
                    : bassdrum ? tr ("BassDrum — FL's engine (table and noise samples read from your FL installation)")
                    : toxic ? tr ("Toxic Biohazard — FL's engine with its effects (waves read from your FL installation)")
                    : threeOsc ? tr ("3x Osc — FL's engine (custom wave from the channel's sample)")
                    : simSynth ? tr ("SimSynth — FL's engine (new settings restart the voices)")
                    : wasp ? tr ("Wasp — FL Studio 2026 has no 64-bit Wasp: the channel keeps its settings and stays silent, as in FL")
                    : flKeys ? tr ("FL Keys — FL's engine (wavetable read from your FL installation)")
                    : plucked ? tr ("Plucked! — FL's plucked-string engine")
                    : beepMap ? tr ("BeepMap — FL's engine (plays the bitmap saved with the channel)")
                    : sawer ? tr ("Sawer — FL's engine with its effects (oscillator cycle read from your FL installation)")
                    : granulizer ? tr ("Fruity Granulizer — FL's engine on the channel's sample")
                    : drumaxx ? tr ("Drumaxx — FL's physical drum engine (pads on their keys; patterns not played)")
                    : slayer ? tr ("FL Slayer — FL's engine with its effects, amp and cabinet (picks read from your FL installation; new settings restart the voices)")
                    : sytrus ? tr ("Sytrus — FL's engine, main settings (operators, filters and effects keep the saved patch)")
                    : fruityDance ? tr ("Fruity Dance — a visual plugin: it makes no sound, the channel keeps its settings")
                    : boobass ? tr ("BooBass — FL's engine, its sound read from your FL installation (knobs: 3-band EQ)")
                    : sakura ? tr ("Sakura — physical strings (FL's engine; new parameters apply to the next note)")
                             : tr ("Drumpad — physical drum (FL's engine; new parameters apply to the next hit)"),
                    10, 6, getWidth() - 20, 20, juce::Justification::centredLeft);
        if (layout != nullptr)
        {
            paintLayout (g);
            return;
        }
        g.setFont (theme::font (theme::type::small));
        for (const auto& k : knobs)
            g.drawText (k->getName(), k->getBounds().withHeight (14).translated (0, -14), juce::Justification::centred);
    }

    void resized() override
    {
        if (layout != nullptr)
        {
            resizeLayout();
            return;
        }
        constexpr int columns = 7, w = 96, h = 92;
        for (std::size_t i = 0; i < knobs.size(); ++i)
            knobs[i]->setBounds (12 + (int) (i % columns) * (w + 4), 46 + (int) (i / columns) * (h + 18), w, h);
    }

private:
    AudioEngine& engine;
    int channel;
    juce::Colour accent { skin::colour::crimson }; // the plugin page's accent (ui/PluginSkin.h)
    skin::Style pageStyle = skin::Style::glow;     // the plugin page's automatic style (round 126)
    bool sakura = true, boobass = false, drumsynth = false, harmless = false, harmor = false, gms = false, morphine = false, ogun = false, autogun = false, transistorBass = false, miniSynth = false, dx10 = false, kepler = false, poizone = false,
         fruitkick = false, bassdrum = false, toxic = false, threeOsc = false, simSynth = false, wasp = false, plucked = false, beepMap = false,
         sawer = false, flKeys = false, granulizer = false, drumaxx = false, slayer = false, fruityDance = false, sytrus = false, wavesFound = false;
    int editorKey = 60;
    std::vector<std::unique_ptr<juce::Slider>> knobs;
    const plugui::Layout* layout = nullptr;
    const plugui::LayoutItem* xyItem = nullptr; // the pad being dragged
};

class SettingsPanel final : public juce::Component,
                            private juce::ChangeListener
{
public:
    SettingsPanel (AudioEngine& e, int c)
        : engine (e),
          channel (c)
    {
        setup (pitch, pitchLabel, TR_NOOP ("Pitch"), -24.0, 24.0, 0.01, " st");
        pitch.setTooltip (tr ("The sample's pitch in semitones (0.5 = 50 cents). The Piano Roll's keys play it chromatically around its root note (Misc › Root note, C5 by default); right-click the channel › Pitch... for semitones + cents"));
        setup (gain, gainLabel, TR_NOOP ("Gain"), -24.0, 24.0, 0.1, " dB");
        gain.textFromValueFunction = [] (double v) { return (v > 0.04 ? "+" : "") + juce::String (std::abs (v) < 0.05 ? 0.0 : v, 1) + " dB"; };
        gain.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("+-0123456789.,").replaceCharacter (',', '.').getDoubleValue(); };
        gain.setTooltip (tr ("Sample gain: boosts or cuts the sample's level before the channel volume (-24 to +24 dB, double-click = 0 dB)"));
        gain.setValue (0.0, juce::dontSendNotification);
        setup (attack, attackLabel, TR_NOOP ("Attack"), 0.0, 2.0, 0.001, " s");
        setup (release, releaseLabel, TR_NOOP ("Release"), 0.0, 4.0, 0.001, " s");
        setup (decay, decayLabel, TR_NOOP ("Decay"), 0.0, 10.0, 0.001, " s");
        setup (sustain, sustainLabel, TR_NOOP ("Sustain"), 0.0, 1.0, 0.01, "");
        setup (stretch, stretchLabel, TR_NOOP ("Stretch"), 0.0, 64.0, 0.25, " beats");
        setup (regionStart, regionStartLabel, TR_NOOP ("Start"), 0.0, 1.0, 0.001, "");
        setup (regionEnd, regionEndLabel, TR_NOOP ("End"), 0.0, 1.0, 0.001, "");
        setup (cutoff, cutoffLabel, TR_NOOP ("Cutoff"), 20.0, 20000.0, 1.0, " Hz");
        setup (resonance, resonanceLabel, TR_NOOP ("Resonance"), 0.0, 1.0, 0.01, "");
        cutoff.setSkewFactorFromMidPoint (1000.0);
        cutoff.setDoubleClickReturnValue (true, 20000.0);
        cutoff.setTooltip (tr ("Channel filter cutoff (low-pass at maximum with no resonance = off)"));
        filterType.addItemList ({ tr ("Low-pass"), tr ("Band-pass"), tr ("High-pass"), tr ("Notch") }, 1);
        filterType.onChange = [this] { apply(); };
        addAndMakeVisible (filterType);
        modulationButton.setButtonText (tr ("Modulation (envelopes and LFOs)..."));
        modulationButton.onClick = [this] { showModulationFor (*this, engine, channel); };
        addAndMakeVisible (modulationButton);
        regionEnd.setDoubleClickReturnValue (true, 1.0);
        regionStart.setTooltip (tr ("Where the sample starts playing (fraction of the file)"));
        sustain.setDoubleClickReturnValue (true, 1.0);
        decay.setSkewFactorFromMidPoint (0.5);
        stretch.setTooltip (tr ("0 = off. Otherwise the whole sample lasts this many beats and follows the BPM (by resampling, like FL's Resample mode)"));
        attack.setSkewFactorFromMidPoint (0.2);
        release.setSkewFactorFromMidPoint (0.4);
        release.setTooltip (tr ("0 = the whole sample plays (one-shot), like FL's steps"));

        keepPitch.setButtonText (tr ("Keep length (stretch engine)"));
        keepPitch.setTooltip (tr ("On: the pitch, the fine tune and the keys change the pitch without changing the length, and \"Stretch\" changes the length without changing the pitch (like FL's Stretch modes). Off: resample, pitch and speed change together"));
        keepPitch.onClick = [this] { apply(); };
        addAndMakeVisible (keepPitch);

        stretchMethod.addItem (tr ("General"), 1);
        stretchMethod.addItem (tr ("Drums / transients"), 2);
        stretchMethod.addItem (tr ("Vocal / mono"), 3);
        stretchMethod.setTooltip (tr ("How the stretch keeping pitch is computed: General for full mixes and chords, Drums for loops and percussive sounds (attacks kept sharp), Vocal for a single voice or instrument"));
        stretchMethod.onChange = [this] { apply(); };
        addAndMakeVisible (stretchMethod);

        reverse.setButtonText (tr ("Reverse"));
        cut.setButtonText (tr ("Cut itself"));
        porta.setButtonText ("Porta");
        porta.setTooltip (tr ("Portamento: the note starts from the previous note's pitch (with Cut itself, the 808 glide)"));
        cut.setTooltip (tr ("A new note cuts the previous one (good for hi-hats and 808s)"));

        for (auto* b : { &reverse, &cut, &porta })
        {
            addAndMakeVisible (b);
            b->onClick = [this] { apply(); };
        }

        wave.setEmptyText (tr ("No sample loaded"));
        wave.setTooltip (tr ("The sample: colours show where it is bass-heavy (warm) or bright (cool); outside Start/End is dimmed"));
        addAndMakeVisible (wave);

        refresh();
        engine.addChangeListener (this);
        setSize (420, 676);
    }

    ~SettingsPanel() override { engine.removeChangeListener (this); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        wave.setBounds (area.removeFromTop (92));
        area.removeFromTop (8);
        auto place = [] (juce::Rectangle<int> row, std::initializer_list<std::pair<juce::Slider*, juce::Label*>> items) {
            const auto w = row.getWidth() / 3;

            for (auto [slider, label] : items)
            {
                auto column = row.removeFromLeft (w);
                label->setBounds (column.removeFromTop (18));
                slider->setBounds (column);
            }
        };

        auto top = area.removeFromTop (110);
        {
            // Gain, Pitch, Stretch and the keep-pitch switch in four columns
            const auto w4 = top.getWidth() / 4;
            for (auto [slider, label] : { std::pair { &gain, &gainLabel }, { &pitch, &pitchLabel }, { &stretch, &stretchLabel } })
            {
                auto column = top.removeFromLeft (w4);
                label->setBounds (column.removeFromTop (18));
                slider->setBounds (column);
            }
            auto column = top.withSizeKeepingCentre (juce::jmin (top.getWidth(), 140), juce::jmin (top.getHeight(), 64));
            keepPitch.setBounds (column.removeFromTop (column.getHeight() / 2));
            stretchMethod.setBounds (column.withSizeKeepingCentre (column.getWidth(), juce::jmin (column.getHeight(), 24)));
        }
        area.removeFromTop (6);
        auto envelope = area.removeFromTop (110);
        const auto w = envelope.getWidth() / 4;

        for (auto [slider, label] : { std::pair { &attack, &attackLabel }, { &decay, &decayLabel }, { &sustain, &sustainLabel }, { &release, &releaseLabel } })
        {
            auto column = envelope.removeFromLeft (w);
            label->setBounds (column.removeFromTop (18));
            slider->setBounds (column);
        }

        area.removeFromTop (8);
        area.removeFromTop (6);
        place (area.removeFromTop (110), { { &regionStart, &regionStartLabel }, { &regionEnd, &regionEndLabel } });
        area.removeFromTop (6);
        {
            auto filterRow = area.removeFromTop (110);
            filterType.setBounds (filterRow.removeFromRight (filterRow.getWidth() / 3).withSizeKeepingCentre (124, 24));
            place (filterRow.withWidth (filterRow.getWidth() * 3 / 2), { { &cutoff, &cutoffLabel }, { &resonance, &resonanceLabel } });
        }
        modulationButton.setBounds (area.removeFromBottom (26).withSizeKeepingCentre (260, 26));
        area.removeFromTop (6);
        reverse.setBounds (area.removeFromLeft (150).removeFromTop (24));
        cut.setBounds (area.removeFromLeft (130).removeFromTop (24));
        porta.setBounds (area.removeFromLeft (100).removeFromTop (24));
    }

    int getChannel() const noexcept { return channel; }

private:
    void setup (juce::Slider& s, juce::Label& label, const char* name, double lo, double hi, double step, const char* suffix)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.setDoubleClickReturnValue (true, 0.0);
        s.onValueChange = [this] { apply(); };
        label.setText (tr (name), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (s);
        addAndMakeVisible (label);
    }

    const ChannelDef* current() const
    {
        const auto& channels = engine.getSong().channels;
        return channel >= 0 && channel < (int) channels.size() ? &channels[(std::size_t) channel] : nullptr;
    }

    void refresh()
    {
        if (const auto* c = current())
        {
            updating = true;
            pitch.setValue (c->sampler.pitch, juce::dontSendNotification);
            gain.setValue (c->sampler.gainDb, juce::dontSendNotification);
            attack.setValue (c->sampler.attack, juce::dontSendNotification);
            decay.setValue (c->sampler.decay, juce::dontSendNotification);
            sustain.setValue (c->sampler.sustain, juce::dontSendNotification);
            stretch.setValue (c->sampler.stretchBeats, juce::dontSendNotification);
            keepPitch.setToggleState (c->sampler.keepPitch, juce::dontSendNotification);
            stretchMethod.setSelectedId ((int) c->sampler.stretchMethod + 1, juce::dontSendNotification);
            stretchMethod.setEnabled (c->sampler.keepPitch);
            regionStart.setValue (c->sampler.regionStart, juce::dontSendNotification);
            regionEnd.setValue (c->sampler.regionEnd, juce::dontSendNotification);
            cutoff.setValue (c->sampler.cutoff, juce::dontSendNotification);
            resonance.setValue (c->sampler.resonance, juce::dontSendNotification);
            filterType.setSelectedId (c->sampler.filterType + 1, juce::dontSendNotification);
            release.setValue (c->sampler.release, juce::dontSendNotification);
            reverse.setToggleState (c->sampler.reverse, juce::dontSendNotification);
            cut.setToggleState (c->sampler.cut, juce::dontSendNotification);
            porta.setToggleState (c->sampler.porta, juce::dontSendNotification);
            updating = false;

            const auto path = juce::String::fromUTF8 ((c->samplePath.empty() && ! c->zones.empty() ? c->zones.front().path : c->samplePath).c_str());
            wave.setFile (path, [&e = engine, path] { return e.getSampleLibrary().load (path); });
            wave.setRegion (c->sampler.regionStart, c->sampler.regionEnd);
            wave.setReversed (c->sampler.reverse);

            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setName (tr ("Sampler settings - {0}", { juce::String::fromUTF8 (c->name.c_str()) })
                            + (c->zones.empty() ? juce::String() : tr (" (multi-sample: {0} zones)", { juce::String ((int) c->zones.size()) })));
        }
    }

    void apply()
    {
        if (updating || current() == nullptr)
            return;

        auto s = current()->sampler; // keeps what the panel does not show (slices)
        s.pitch = (float) pitch.getValue();
        s.gainDb = std::abs (gain.getValue()) < 0.05 ? 0.0f : (float) gain.getValue();
        s.attack = (float) attack.getValue();
        s.decay = (float) decay.getValue();
        s.sustain = (float) sustain.getValue();
        s.stretchBeats = (float) stretch.getValue();
        s.keepPitch = keepPitch.getToggleState();
        s.stretchMethod = (StretchMethod) juce::jlimit (0, 2, stretchMethod.getSelectedId() - 1);
        stretchMethod.setEnabled (s.keepPitch);
        s.regionStart = (float) regionStart.getValue();
        s.regionEnd = (float) regionEnd.getValue();
        s.release = (float) release.getValue();
        s.reverse = reverse.getToggleState();
        s.cut = cut.getToggleState();
        s.porta = porta.getToggleState();
        s.cutoff = (float) cutoff.getValue();
        s.resonance = (float) resonance.getValue();
        s.filterType = juce::jlimit (0, 3, filterType.getSelectedId() - 1);
        {
            // an ADSR edited by hand replaces FL's imported volume envelope
            const auto& old = current()->sampler;
            auto moved = [] (float a, float b) { return std::abs (a - b) > 1.0e-4f; };
            const auto adsr = moved (s.attack, old.attack) || moved (s.decay, old.decay) || moved (s.sustain, old.sustain) || moved (s.release, old.release);
            if (adsr)
                s.flVolumeEnvelope.clear();
            // so does an edited envelope or filter for FL's whole sampler voice (REA_rodada76)
            if (adsr || moved (s.cutoff, old.cutoff) || moved (s.resonance, old.resonance) || s.filterType != old.filterType)
                s.flChannel.clear();
        }
        if (s == current()->sampler)
            return;

        engine.editSong ([c = channel, s] (Song& song) { song.channels[(std::size_t) c].sampler = s; });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (current() == nullptr) // channel removed
        {
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::DocumentWindow> (w)] {
                    if (safe != nullptr)
                        safe->closeButtonPressed();
                });
            return;
        }

        refresh();
    }

    AudioEngine& engine;
    int channel;
    bool updating = false;
    juce::Slider pitch, gain, attack, release, decay, sustain, stretch, regionStart, regionEnd, cutoff, resonance;
    juce::Label pitchLabel, gainLabel, attackLabel, releaseLabel, decayLabel, sustainLabel, stretchLabel, regionStartLabel, regionEndLabel,
        cutoffLabel, resonanceLabel;
    juce::ToggleButton reverse, cut, keepPitch, porta;
    juce::ComboBox filterType, stretchMethod;
    juce::TextButton modulationButton;
    ui::WaveformDisplay wave;
};

// Built-in synth page: 3 oscillators, envelope and filter.
class SynthPanel final : public juce::Component,
                         private juce::ChangeListener
{
public:
    SynthPanel (AudioEngine& e, int c)
        : engine (e),
          channel (c)
    {
        for (int o = 0; o < 3; ++o)
        {
            auto& row = rows[(std::size_t) o];
            row.title.setText (tr ("Osc {0}", { juce::String (o + 1) }), juce::dontSendNotification);
            row.wave.addItemList ({ tr ("Sine"), tr ("Triangle"), tr ("Saw"), tr ("Square"), tr ("Noise"), tr ("Harmonics") }, 1);
            row.wave.onChange = [this] { apply(); };
            knob (row.octave, -3.0, 3.0, 1.0, " oct");
            knob (row.semitones, -11.0, 11.0, 1.0, " st");
            knob (row.detune, -100.0, 100.0, 1.0, " ct");
            knob (row.pan, -1.0, 1.0, 0.01, "");
            knob (row.level, 0.0, 4.0, 0.01, "");
            row.level.setSkewFactorFromMidPoint (1.0); // above 1: gain (Harmor goes up to +12 dB)
            addAndMakeVisible (row.title);
            addAndMakeVisible (row.wave);
        }

        knob (attack, 0.0, 5.0, 0.001, " s");
        knob (decay, 0.0, 5.0, 0.001, " s");
        knob (sustain, 0.0, 1.0, 0.01, "");
        knob (release, 0.0, 5.0, 0.001, " s");
        knob (cutoff, 20.0, 20000.0, 1.0, " Hz");
        knob (resonance, 0.0, 1.0, 0.01, "");
        knob (fm, 0.0, 8.0, 0.01, "");
        fm.setSkewFactorFromMidPoint (1.0);
        fm.setTooltip (tr ("FM: osc 3 (which stops sounding) modulates the phase of osc 1 and 2; 0 = off"));
        knob (fmDecay, 0.0, 5.0, 0.001, " s");
        fmDecay.setSkewFactorFromMidPoint (0.5);
        fmDecay.setTooltip (tr ("Time for the FM index to decay (0 = constant); the brightness fades like an electric piano"));
        knob (keyTrack, -1.0, 2.0, 0.01, "");
        keyTrack.setTooltip (tr ("Cutoff follows the note: cutoff octaves per note octave, relative to C3 (130.8 Hz)"));
        knob (sweep, 0.0, 100.0, 0.01, "");
        sweep.setSkewFactorFromMidPoint (2.0);
        sweep.setTooltip (tr ("Kick pitch sweep: the note starts (1 + value) times higher and falls"));
        knob (sweepTime, 0.001, 2.0, 0.001, " s");
        sweepTime.setSkewFactorFromMidPoint (0.1);
        sweepTime.setTooltip (tr ("Sweep time constant"));
        for (auto* b : { &bandLow, &bandMid, &bandHigh })
        {
            knob (*b, 0.0, 2.0, 0.01, "");
            b->setTooltip (tr ("3-band tone after the filter (low < 178 Hz < mid < 1.18 kHz < high); 1 = unchanged"));
        }
        ring.setButtonText (tr ("Ring"));
        ring.setTooltip (tr ("With AM on: ring modulation, (osc 1 + 2) × (1 − level + osc 3), like Sytrus's RM"));
        ring.onClick = [this] { apply(); };
        addAndMakeVisible (ring);

        modulationButton.setButtonText (tr ("Modulation (envelopes and LFOs)..."));
        modulationButton.onClick = [this] { showModulationFor (*this, engine, channel); };
        addAndMakeVisible (modulationButton);

        for (auto* s : { &attack, &decay, &release })
            s->setSkewFactorFromMidPoint (0.5);
        cutoff.setSkewFactorFromMidPoint (1000.0);

        const char* names[] = { TR_NOOP ("Octave"), TR_NOOP ("Semitone"), TR_NOOP ("Fine"), TR_NOOP ("Pan"), TR_NOOP ("Level"), TR_NOOP ("Attack"), TR_NOOP ("Decay"), TR_NOOP ("Sustain"), TR_NOOP ("Release"), TR_NOOP ("Cutoff"), TR_NOOP ("Resonance"), TR_NOOP ("Porta time"), TR_NOOP ("FM"), TR_NOOP ("FM decay"),
                                TR_NOOP ("Keytrack"), TR_NOOP ("Sweep"), TR_NOOP ("Sweep time"), TR_NOOP ("Low"), TR_NOOP ("Mid"), TR_NOOP ("High") };

        for (int i = 0; i < (int) labels.size(); ++i)
        {
            labels[(std::size_t) i].setText (tr (names[i]), juce::dontSendNotification);
            labels[(std::size_t) i].setJustificationType (juce::Justification::centred);
            labels[(std::size_t) i].setFont (theme::font (theme::type::caption));
            addAndMakeVisible (labels[(std::size_t) i]);
        }

        presetBox.setTextWhenNothingSelected (tr ("Presets..."));
        const auto presets = synthPresets();
        for (int i = 0; i < (int) presets.size(); ++i)
            presetBox.addItem (juce::String::fromUTF8 (presets[(std::size_t) i].first.c_str()), i + 1);
        presetBox.onChange = [this] {
            const auto presets = synthPresets();
            const auto index = presetBox.getSelectedId() - 1;

            if (juce::isPositiveAndBelow (index, (int) presets.size()) && current() != nullptr)
                engine.editSong ([c = channel, s = presets[(std::size_t) index].second] (Song& song) { song.channels[(std::size_t) c].synth = s; });

            presetBox.setSelectedId (0, juce::dontSendNotification);
        };
        addAndMakeVisible (presetBox);

        flPresets.setButtonText (tr ("FL presets..."));
        flPresets.setTooltip (tr ("The 3x Osc channel presets that ship with FL Studio (Patches\\Channel presets\\3x Osc)"));
        flPresets.onClick = [this] { showFlPresets(); };
        addAndMakeVisible (flPresets);

        filterTypeBox.addItemList ({ tr ("Low-pass"), tr ("Band-pass"), tr ("High-pass"), tr ("Notch") }, 1);
        filterTypeBox.setTooltip (tr ("Filter type"));
        filterTypeBox.onChange = [this] { apply(); };
        addAndMakeVisible (filterTypeBox);

        am.setButtonText ("AM");
        am.setTooltip (tr ("Osc 3 modulates the amplitude of osc 1 and 2 (like 3x Osc's AM); its level is the depth"));
        am.onClick = [this] { apply(); };
        addAndMakeVisible (am);

        knob (porta, 0.0, 2.0, 0.005, " t");
        porta.setSkewFactorFromMidPoint (0.25);
        porta.setTooltip (tr ("Portamento time, in beats (applies to the Porta button and to notes marked porta)"));
        portaOn.setButtonText ("Porta");
        portaOn.setTooltip (tr ("Portamento: the new note glides from the previous one (notes marked porta invert this)"));
        portaOn.onClick = [this] { apply(); };
        addAndMakeVisible (portaOn);

        mono.setButtonText ("Mono");
        mono.setTooltip (tr ("Single voice: the new note takes over the one currently sounding (basses and leads)"));
        mono.onClick = [this] { apply(); };
        addAndMakeVisible (mono);

        refresh();
        engine.addChangeListener (this);
        setSize (720, 510);
    }

    ~SynthPanel() override { engine.removeChangeListener (this); }

    int getChannel() const noexcept { return channel; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto topRow = area.removeFromTop (24);
        presetBox.setBounds (topRow.removeFromLeft (200));
        mono.setBounds (topRow.removeFromLeft (90).withTrimmedLeft (16));
        am.setBounds (topRow.removeFromLeft (52));
        ring.setBounds (topRow.removeFromLeft (60));
        flPresets.setBounds (topRow.removeFromLeft (130).reduced (4, 0));
        portaOn.setBounds (topRow.removeFromLeft (70));
        filterTypeBox.setBounds (topRow.removeFromRight (140));
        area.removeFromTop (6);
        auto header = area.removeFromTop (16);
        header.removeFromLeft (150);

        for (int i = 0; i < oscKnobs; ++i)
            labels[(std::size_t) i].setBounds (header.removeFromLeft (88));

        for (auto& row : rows)
        {
            auto line = area.removeFromTop (64);
            row.title.setBounds (line.removeFromLeft (50).withSizeKeepingCentre (50, 24));
            row.wave.setBounds (line.removeFromLeft (100).withSizeKeepingCentre (96, 24));
            for (auto* s : { &row.octave, &row.semitones, &row.detune, &row.pan, &row.level })
                s->setBounds (line.removeFromLeft (88));
        }

        area.removeFromTop (10);
        auto names = area.removeFromTop (16);
        const auto w = area.getWidth() / 9;

        for (int i = oscKnobs; i < oscKnobs + 9; ++i)
            labels[(std::size_t) i].setBounds (names.removeFromLeft (w));

        auto knobsRow = area.removeFromTop (80);
        for (auto* s : { &attack, &decay, &sustain, &release, &cutoff, &resonance, &porta, &fm, &fmDecay })
            s->setBounds (knobsRow.removeFromLeft (w));

        area.removeFromTop (6);
        auto names2 = area.removeFromTop (16);
        for (int i = oscKnobs + 9; i < (int) labels.size(); ++i)
            labels[(std::size_t) i].setBounds (names2.removeFromLeft (w));
        auto knobsRow2 = area.removeFromTop (80);
        for (auto* s : { &keyTrack, &sweep, &sweepTime, &bandLow, &bandMid, &bandHigh })
            s->setBounds (knobsRow2.removeFromLeft (w));

        area.removeFromTop (12);
        modulationButton.setBounds (area.removeFromTop (26).withSizeKeepingCentre (260, 26));
    }

private:
    struct Row
    {
        juce::Label title;
        juce::ComboBox wave;
        juce::Slider octave, semitones, detune, pan, level;
    };

    static constexpr int oscKnobs = 5; // oscillator knob labels come first

    juce::TextButton modulationButton;
    juce::ToggleButton mono, am, portaOn;
    juce::TextButton flPresets;

    // menu with the presets from the FL install for the synths LYNX converts: the 3x Osc channel .fst
    // files (by letter) and, per generator, the "Plugin presets" folders (Sytrus, Harmor, GMS...)
    void showFlPresets()
    {
        const auto roots = findFlStudioRoots();
        const auto patches = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches");
        auto sorted = [] (juce::Array<juce::File> files) {
            std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
                return a.getFileName().compareNatural (b.getFileName()) < 0;
            });
            return files;
        };

        juce::PopupMenu menu;

        // 3x Osc: ~170 presets grouped by first letter
        {
            juce::PopupMenu threeOsc, sub;
            juce::juce_wchar letter = 0;
            for (const auto& f : sorted (patches.getChildFile ("Channel presets/3x Osc").findChildFiles (juce::File::findFiles, false, "*.fst")))
            {
                const auto first = juce::CharacterFunctions::toUpperCase (f.getFileName()[0]);
                if (first != letter && sub.getNumItems() > 0)
                {
                    threeOsc.addSubMenu (juce::String::charToString (letter), sub);
                    sub = {};
                }
                letter = first;
                sub.addItem (f.getFileNameWithoutExtension(), [this, f] { loadFlPreset (f); });
            }
            if (sub.getNumItems() > 0)
                threeOsc.addSubMenu (juce::String::charToString (letter), sub);
            if (threeOsc.getNumItems() > 0)
                menu.addSubMenu ("3x Osc", threeOsc);
        }

        // a preset folder as a menu: subfolders become submenus
        std::function<juce::PopupMenu (const juce::File&, int)> folderMenu = [&] (const juce::File& folder, int depth) {
            juce::PopupMenu m;
            if (depth < 3)
                for (const auto& dir : sorted (folder.findChildFiles (juce::File::findDirectories, false)))
                    if (auto sub = folderMenu (dir, depth + 1); sub.getNumItems() > 0)
                        m.addSubMenu (dir.getFileName(), sub);
            for (const auto& f : sorted (folder.findChildFiles (juce::File::findFiles, false, "*.fst;*.gmsynth")))
                m.addItem (f.getFileNameWithoutExtension(), [this, f] { loadFlPreset (f); });
            return m;
        };

        const auto generators = patches.getChildFile ("Plugin presets/Generators");
        for (const auto* name : { "Sytrus", "Harmor", "GMS", "Harmless", "MiniSynth", "Fruity DX10", "Fruit Kick", "Ogun", "Sakura", "Kepler",
                                 "PoiZone", "Toxic Biohazard", "BassDrum" })
            if (auto sub = folderMenu (generators.getChildFile (name), 0); sub.getNumItems() > 0)
                menu.addSubMenu (name, sub);

        if (menu.getNumItems() == 0)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("FL presets"),
                                                    tr ("FL presets not found (install FL Studio or check the Data\\Patches folder)."));
            return;
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&flPresets));
    }

    void loadFlPreset (const juce::File& file)
    {
        juce::MemoryBlock data;

        if (! file.loadFileAsData (data) || current() == nullptr)
            return;

        const auto* bytes = static_cast<const std::uint8_t*> (data.getData());

        if (auto synth = synthFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize())))
            engine.editSong ([c = channel, s = *synth] (Song& song) { song.channels[(std::size_t) c].synth = s; });
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("FL presets"),
                                                    tr ("This preset is not for a synth LYNX converts, or it could not be read."));
    }
    juce::Slider porta;
    juce::ComboBox filterTypeBox;

    void knob (juce::Slider& s, double lo, double hi, double step, const char* suffix)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.onValueChange = [this] { apply(); };
        addAndMakeVisible (s);
    }

    const ChannelDef* current() const
    {
        const auto& channels = engine.getSong().channels;
        return channel >= 0 && channel < (int) channels.size() ? &channels[(std::size_t) channel] : nullptr;
    }

    void refresh()
    {
        const auto* c = current();

        if (c == nullptr)
            return;

        updating = true;
        const auto& s = c->synth;

        for (std::size_t o = 0; o < 3; ++o)
        {
            rows[o].wave.setSelectedId ((int) s.osc[o].wave + 1, juce::dontSendNotification);
            rows[o].octave.setValue (s.osc[o].octave, juce::dontSendNotification);
            rows[o].detune.setValue (s.osc[o].detune, juce::dontSendNotification);
            rows[o].semitones.setValue (s.osc[o].semitones, juce::dontSendNotification);
            rows[o].pan.setValue (s.osc[o].pan, juce::dontSendNotification);
            rows[o].level.setValue (s.osc[o].level, juce::dontSendNotification);
        }

        attack.setValue (s.attack, juce::dontSendNotification);
        decay.setValue (s.decay, juce::dontSendNotification);
        sustain.setValue (s.sustain, juce::dontSendNotification);
        release.setValue (s.release, juce::dontSendNotification);
        cutoff.setValue (s.cutoff, juce::dontSendNotification);
        resonance.setValue (s.resonance, juce::dontSendNotification);
        mono.setToggleState (s.mono, juce::dontSendNotification);
        am.setToggleState (s.am, juce::dontSendNotification);
        porta.setValue (s.portaBeats, juce::dontSendNotification);
        portaOn.setToggleState (s.porta, juce::dontSendNotification);
        filterTypeBox.setSelectedId (s.filterType + 1, juce::dontSendNotification);
        fm.setValue (s.fm, juce::dontSendNotification);
        fmDecay.setValue (s.fmDecay, juce::dontSendNotification);
        keyTrack.setValue (s.keyTrack, juce::dontSendNotification);
        sweep.setValue (s.sweep, juce::dontSendNotification);
        sweepTime.setValue (s.sweepTime, juce::dontSendNotification);
        bandLow.setValue (s.bandGain[0], juce::dontSendNotification);
        bandMid.setValue (s.bandGain[1], juce::dontSendNotification);
        bandHigh.setValue (s.bandGain[2], juce::dontSendNotification);
        ring.setToggleState (s.ring, juce::dontSendNotification);

        updating = false;

        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (tr ("Synth settings - {0}", { juce::String::fromUTF8 (c->name.c_str()) }));
    }

    void apply()
    {
        if (updating || current() == nullptr)
            return;

        auto s = current()->synth; // keeps what this window does not show (envelopes and LFOs)

        for (std::size_t o = 0; o < 3; ++o)
            s.osc[o] = { (Waveform) juce::jlimit (0, 5, rows[o].wave.getSelectedId() - 1), (int) rows[o].octave.getValue(),
                         (float) rows[o].detune.getValue(), (float) rows[o].level.getValue(), (int) rows[o].semitones.getValue(),
                         (float) rows[o].pan.getValue() };

        s.attack = (float) attack.getValue();
        s.decay = (float) decay.getValue();
        s.sustain = (float) sustain.getValue();
        s.release = (float) release.getValue();
        s.cutoff = (float) cutoff.getValue();
        s.resonance = (float) resonance.getValue();
        s.mono = mono.getToggleState();
        s.am = am.getToggleState();
        s.portaBeats = (float) porta.getValue();
        s.porta = portaOn.getToggleState();
        s.filterType = juce::jlimit (0, 3, filterTypeBox.getSelectedId() - 1);
        s.fm = (float) fm.getValue();
        s.fmDecay = (float) fmDecay.getValue();
        if (s.fmDecay <= 0.0f)
            s.fmSustain = 1.0f;
        s.keyTrack = (float) keyTrack.getValue();
        s.sweep = (float) sweep.getValue();
        s.sweepTime = (float) sweepTime.getValue();
        s.bandGain = { (float) bandLow.getValue(), (float) bandMid.getValue(), (float) bandHigh.getValue() };
        s.ring = ring.getToggleState();

        if (s == current()->synth)
            return;

        engine.editSong ([c = channel, s] (Song& song) { song.channels[(std::size_t) c].synth = s; });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (current() == nullptr)
        {
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::DocumentWindow> (w)] {
                    if (safe != nullptr)
                        safe->closeButtonPressed();
                });
            return;
        }

        refresh();
    }

    AudioEngine& engine;
    int channel;
    bool updating = false;
    std::array<Row, 3> rows;
    juce::Slider attack, decay, sustain, release, cutoff, resonance, fm, fmDecay, keyTrack, sweep, sweepTime, bandLow, bandMid, bandHigh;
    juce::ToggleButton ring;
    std::array<juce::Label, 20> labels;
    juce::ComboBox presetBox;
};

// --- FL's channel window frame (REA round 143b) --------------------------------------------------------------
// FL 2026 draws every channel's settings window the same way: a header strip with the channel's tabs on the left
// (the instrument / plugin page, the wrench "Miscellaneous functions" page) and ON, PAN, VOL and TRACK on the right
// [EXEC: FL 2026 build 5654, "Detailed settings" of a generator and of a sampler]. LYNX does the same around its own
// instrument pages, in its own style; the envelopes and LFOs of the synth and the sampler are the "Envelope /
// instrument" tab instead of a second window.

// A small rotary control with LYNX's control contract (drag, Ctrl = fine, wheel, Alt / middle-click / double-click
// reset, right-click menu with automation items when the spec has a target); the hint bar shows its name and value.
class FrameKnob final : public juce::Component,
                        public ui::HintSource
{
public:
    FrameKnob (AudioEngine& e, std::function<ui::ValueSpec()> makeSpec, std::function<double()> get, std::function<void (double)> set,
               juce::String caption)
        : engine (e), specOf (std::move (makeSpec)), getter (std::move (get)), setter (std::move (set)), label (std::move (caption))
    {
        setRepaintsOnMouseActivity (true);
    }

    ui::Hint hintAt (juce::Point<int>) override { return ui::valueHint (specOf(), getter(), ui::valueGestures()); }

    void paint (juce::Graphics& g) override
    {
        const auto spec = specOf();
        auto area = getLocalBounds().toFloat();
        const auto caption = area.removeFromBottom (13.0f);
        const auto valueArea = area.removeFromBottom (13.0f);
        const auto size = juce::jmin (area.getWidth(), area.getHeight());
        const auto v = (float) juce::jlimit (0.0, 1.0, (getter() - spec.minimum) / juce::jmax (1e-9, spec.maximum - spec.minimum));
        theme::drawKnob (g, juce::Rectangle<float> (size, size).withCentre (area.getCentre()), v, spec.minimum < 0.0,
                         isMouseOverOrDragging() ? theme::colour::accent : theme::colour::knobArc, isMouseOver());
        g.setFont (theme::font (theme::type::caption));
        g.setColour (theme::colour::textPrimary);
        g.drawText (spec.text (getter()), valueArea, juce::Justification::centred);
        g.setColour (theme::colour::textSecondary);
        g.drawText (theme::caps (label), caption, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, specOf(), getter(), setter);
            return;
        }
        if (ui::isResetClick (e))
        {
            setter (specOf().defaultValue);
            repaint();
            return;
        }
        drag.begin (e, getter());
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isActive())
            return;
        const auto spec = specOf();
        setter (drag.update (e, spec));
        ui::showValueTip (*this, getLocalBounds(), spec.text (getter()));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag.end();
        ui::hideValueTip();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        setter (specOf().defaultValue);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (const auto v = ui::wheelValue (getter(), e, wheel, specOf()))
        {
            setter (*v);
            ui::flashValueTip (*this, getLocalBounds(), specOf().text (getter()));
            repaint();
        }
    }

private:
    AudioEngine& engine;
    std::function<ui::ValueSpec()> specOf;
    std::function<double()> getter;
    std::function<void (double)> setter;
    juce::String label;
    ui::ValueDrag drag;
};

juce::String channelNameOf (const Song& song, int channel)
{
    if (channel < 0 || channel >= (int) song.channels.size())
        return "?";
    const auto& n = song.channels[(std::size_t) channel].name;
    return n.empty() ? tr ("Channel {0}", { juce::String (channel + 1) }) : juce::String::fromUTF8 (n.c_str());
}

// An integer spec shown as a number, 0 as "--" (FL's empty number boxes).
ui::ValueSpec countSpec (const juce::String& name, int maximum, int defaultValue, std::function<juce::String (int)> text = {})
{
    ui::ValueSpec spec;
    spec.name = name;
    spec.maximum = maximum;
    spec.defaultValue = defaultValue;
    spec.interval = 1.0;
    spec.pixelsForRange = juce::jlimit (60.0, 400.0, 5.0 * maximum);
    spec.toText = [text] (double v) {
        const auto i = juce::roundToInt (v);
        if (text)
            return text (i);
        return i == 0 ? juce::String ("--") : juce::String (i);
    };
    spec.fromText = [] (const juce::String& t) -> std::optional<double> {
        const auto digits = t.retainCharacters ("0123456789");
        return digits.isEmpty() ? std::optional<double> (0.0) : std::optional<double> (digits.getDoubleValue());
    };
    return spec;
}

// The wrench tab: FL's "Miscellaneous functions" with what LYNX's channels have (Levels, Polyphony, Time, Group,
// Arpeggiator).
// FL's Root note keyboard (Misc tab, REA round 149): every key 0…127 (C0…G10, C5 = 60), the channel's root marked;
// a click sets it. LYNX's own drawing.
class RootKeyboard final : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void (int)> onPick;
    // FL's key region ruler (round 164): a drag on the strip above the keys sets the notes the channel plays,
    // a double-click there gives it the whole keyboard back
    std::function<void (int, int)> onRegion;
    void setRoot (int key)
    {
        root = key;
        repaint();
    }
    void setRegion (int low, int high)
    {
        if (low != regionLow || high != regionHigh)
        {
            regionLow = low;
            regionHigh = high;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto w = whiteWidth();
        const auto top = (float) rulerHeight();
        const auto keysH = (float) getHeight() - top;
        if (onRegion)
        {
            const auto ruler = juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), top - 2.0f);
            g.setColour (colour::bg0);
            g.fillRect (ruler);
            const auto full = regionLow <= 0 && regionHigh >= 127;
            const auto x0 = keyLeft (regionLow), x1 = keyRight (regionHigh);
            g.setColour (full ? colour::bg5 : colour::accent);
            g.fillRect (juce::Rectangle<float> (x0, 1.0f, juce::jmax (2.0f, x1 - x0), ruler.getHeight() - 2.0f));
        }
        for (int k = 0, white = 0; k < 128; ++k)
            if (! isBlack (k))
            {
                const auto r = juce::Rectangle<float> ((float) (white++) * w, top, w - 1.0f, keysH);
                const auto outside = onRegion && (k < regionLow || k > regionHigh);
                g.setColour (k == root ? colour::accent : k == hover ? juce::Colour (0xffc8ccd4) : outside ? juce::Colour (0xff9a9ea8) : juce::Colour (0xffe6e8ec));
                g.fillRect (r);
                if (k % 12 == 0 && w * 7.0f >= 14.0f) // the octave's C named (FL's C5 = 60)
                {
                    g.setColour (k == root ? colour::onAccent : juce::Colour (0xff5a5f6a));
                    g.setFont (font (8.5f));
                    g.drawText ("C" + juce::String (k / 12), r.withTrimmedTop (r.getHeight() - 11.0f).expanded (6.0f, 0.0f).toNearestInt(),
                                juce::Justification::centred);
                }
            }
        for (int k = 0; k < 128; ++k)
            if (isBlack (k))
            {
                const auto x = (float) whitesBefore (k) * w - w * 0.32f;
                g.setColour (k == root ? colour::accent : k == hover ? colour::bg5 : colour::bg0);
                g.fillRect (juce::Rectangle<float> (x, top, w * 0.64f, keysH * 0.6f));
            }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto onRuler = onRegion && e.position.y < (float) rulerHeight();
        if (const auto k = keyAt (e.position); k != hover || onRuler != hoverRuler)
        {
            hover = k;
            hoverRuler = onRuler;
            if (onRuler)
                setTooltip (tr ("Key region: {0} to {1} (drag to set, double-click for the whole keyboard)",
                                { keyName (regionLow), keyName (regionHigh) }));
            else
                setTooltip (k >= 0 ? tr ("Root note: {0}", { keyName (k) }) : juce::String());
            repaint();
        }
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hover = -1;
        repaint();
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragFrom = -1;
        if (onRegion && e.position.y < (float) rulerHeight())
        {
            dragFrom = keyAtX (e.position.x);
            if (dragFrom >= 0)
                onRegion (dragFrom, dragFrom);
            return;
        }
        if (const auto k = keyAt (e.position); k >= 0 && onPick)
            onPick (k);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragFrom < 0 || ! onRegion)
            return;
        const auto k = keyAtX (e.position.x);
        if (k >= 0)
            onRegion (juce::jmin (dragFrom, k), juce::jmax (dragFrom, k));
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (onRegion && e.position.y < (float) rulerHeight())
            onRegion (0, 127);
    }

    static juce::String keyName (int key)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[key % 12]) + juce::String (key / 12);
    }

private:
    static bool isBlack (int k) noexcept
    {
        const auto n = k % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }
    static int whitesBefore (int k) noexcept
    {
        int count = 0;
        for (int i = 0; i < k; ++i)
            count += isBlack (i) ? 0 : 1;
        return count;
    }
    int rulerHeight() const noexcept { return onRegion ? 9 : 0; }
    float whiteWidth() const noexcept { return (float) getWidth() / 75.0f; } // 75 white keys in 0…127
    float keyLeft (int k) const noexcept
    {
        const auto w = whiteWidth();
        return isBlack (k) ? (float) whitesBefore (k) * w - w * 0.32f : (float) whitesBefore (k) * w;
    }
    float keyRight (int k) const noexcept
    {
        const auto w = whiteWidth();
        return isBlack (k) ? keyLeft (k) + w * 0.64f : keyLeft (k) + w;
    }
    // the key under x on the ruler (each key owns its own horizontal span; black keys win where they overlap)
    int keyAtX (float x) const noexcept
    {
        for (int k = 0; k < 128; ++k)
            if (isBlack (k) && x >= keyLeft (k) && x < keyRight (k))
                return k;
        const auto white = (int) std::floor (x / whiteWidth());
        for (int k = 0, n = 0; k < 128; ++k)
            if (! isBlack (k) && n++ == white)
                return k;
        return x < 0.0f ? 0 : 127;
    }
    int keyAt (juce::Point<float> p) const noexcept
    {
        const auto w = whiteWidth();
        const auto top = (float) rulerHeight();
        if (p.y < top)
            return keyAtX (p.x);
        if (p.y < top + ((float) getHeight() - top) * 0.6f)
            for (int k = 0; k < 128; ++k)
                if (isBlack (k))
                {
                    const auto x = (float) whitesBefore (k) * w - w * 0.32f;
                    if (p.x >= x && p.x < x + w * 0.64f)
                        return k;
                }
        const auto white = (int) std::floor (p.x / w);
        for (int k = 0, n = 0; k < 128; ++k)
            if (! isBlack (k) && n++ == white)
                return k;
        return -1;
    }

    int root = 60, hover = -1, dragFrom = -1;
    int regionLow = 0, regionHigh = 127;
    bool hoverRuler = false;
};

class MiscPanel final : public juce::Component,
                        private juce::ChangeListener
{
public:
    static constexpr int preferredWidth = 800, preferredHeight = 360;

    MiscPanel (AudioEngine& e, int c) : engine (e), channel (c)
    {
        const auto name = [this] { return channelNameOf (engine.getSong(), channel); };
        auto knob = [this] (std::function<ui::ValueSpec()> spec, std::function<double (const ChannelDef&)> get,
                            std::function<void (ChannelDef&, double)> set, const char* caption) {
            auto k = std::make_unique<FrameKnob> (
                engine, std::move (spec), [this, get] { return current() != nullptr ? get (*current()) : 0.0; },
                [this, set] (double v) {
                    if (current() == nullptr)
                        return;
                    engine.editSong ([ch = channel, set, v] (Song& s) {
                        if (ch < (int) s.channels.size())
                            set (s.channels[(std::size_t) ch], v);
                    });
                },
                tr (caption));
            addAndMakeVisible (*k);
            knobs.push_back (std::move (k));
            return knobs.back().get();
        };

        // Levels adjustment
        pan = knob ([this, name] {
                        auto s = ui::panSpec (name() + " - " + tr ("Pan"));
                        s.target = AutomationTarget { AutomationTargetKind::channelPan, channel };
                        return s;
                    },
                    [] (const ChannelDef& d) { return (double) d.pan; }, [] (ChannelDef& d, double v) { d.pan = (float) v; }, TR_NOOP ("Pan"));
        volume = knob ([this, name] {
                           auto s = ui::percentSpec (name() + " - " + tr ("Volume"), 0.78);
                           s.target = AutomationTarget { AutomationTargetKind::channelVolume, channel };
                           return s;
                       },
                       [] (const ChannelDef& d) { return (double) d.volume; }, [] (ChannelDef& d, double v) { d.volume = (float) v; },
                       TR_NOOP ("Vol"));
        // Polyphony
        maxPoly = knob ([name] { return countSpec (name() + " - " + tr ("Maximum polyphony (-- = no limit)"), 64, 0); },
                        [] (const ChannelDef& d) { return (double) d.maxPolyphony; },
                        [] (ChannelDef& d, double v) { d.maxPolyphony = juce::jlimit (0, 64, juce::roundToInt (v)); }, TR_NOOP ("Max"));
        slide = knob (
            [name] {
                ui::ValueSpec s;
                s.name = name() + " - " + tr ("Portamento time");
                s.maximum = 2.0;
                s.defaultValue = 0.125;
                s.toText = [] (double v) { return tr ("{0} beats", { juce::String (v, 3) }); };
                s.fromText = [] (const juce::String& t) -> std::optional<double> { return t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getDoubleValue(); };
                return s;
            },
            [] (const ChannelDef& d) { return (double) channelPortamentoBeats (d); },
            [] (ChannelDef& d, double v) { setChannelPortamento (d, channelPortamento (d), (float) v); }, TR_NOOP ("Slide"));
        porta.setButtonText (tr ("Porta"));
        porta.setTooltip (tr ("Portamento: each note glides from the previous one's pitch (the Slide knob sets the time)"));
        porta.onClick = [this] { edit ([on = porta.getToggleState()] (ChannelDef& d) { setChannelPortamento (d, on, channelPortamentoBeats (d)); }); };
        mono.setButtonText (tr ("Mono"));
        mono.setTooltip (tr ("Mono: one voice; a new note takes over the sounding one"));
        mono.onClick = [this] { edit ([on = mono.getToggleState()] (ChannelDef& d) { setChannelMono (d, on); }); };
        // Time
        swing = knob ([name] { return ui::percentSpec (name() + " - " + tr ("Swing mix"), 1.0); },
                      [] (const ChannelDef& d) { return (double) d.swingMix; }, [] (ChannelDef& d, double v) { d.swingMix = (float) v; },
                      TR_NOOP ("Swing"));
        // Levels adjustment: Mod X / Mod Y = the channel filter's cutoff / resonance (sampler, synth)
        modX = knob ([name] {
                         ui::ValueSpec s;
                         s.name = name() + " - " + tr ("Mod X (filter cutoff)");
                         s.defaultValue = 1.0;
                         s.toText = [] (double v) { const auto hz = 20.0 * std::pow (1000.0, v); return hz >= 1000.0 ? juce::String (hz / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz"; };
                         return s;
                     },
                     [] (const ChannelDef& d) {
                         const auto hz = d.instrument == InstrumentKind::synth ? d.synth.cutoff : d.sampler.cutoff;
                         return juce::jlimit (0.0, 1.0, std::log ((double) hz / 20.0) / std::log (1000.0));
                     },
                     [] (ChannelDef& d, double v) {
                         const auto hz = (float) (20.0 * std::pow (1000.0, v));
                         if (d.instrument == InstrumentKind::synth)
                             d.synth.cutoff = hz;
                         else if (d.instrument == InstrumentKind::sampler)
                         {
                             d.sampler.cutoff = hz;
                             d.sampler.flChannel.clear(); // an edited filter replaces FL's whole sampler voice (REA_rodada76)
                         }
                     },
                     TR_NOOP ("Mod X"));
        modY = knob ([name] { return ui::percentSpec (name() + " - " + tr ("Mod Y (filter resonance)"), 0.0); },
                     [] (const ChannelDef& d) { return (double) (d.instrument == InstrumentKind::synth ? d.synth.resonance : d.sampler.resonance); },
                     [] (ChannelDef& d, double v) {
                         if (d.instrument == InstrumentKind::synth)
                             d.synth.resonance = (float) v;
                         else if (d.instrument == InstrumentKind::sampler)
                         {
                             d.sampler.resonance = (float) v;
                             d.sampler.flChannel.clear();
                         }
                     },
                     TR_NOOP ("Mod Y"));
        // Time: Gate and Shift on FL's time-knob scale (REA round 146b)
        auto timeSpec = [name] (const char* what, int maximum, int defaultValue, bool offAtMax) {
            ui::ValueSpec s;
            s.name = name() + " - " + tr (what);
            s.maximum = maximum;
            s.defaultValue = defaultValue;
            s.interval = 1.0;
            s.pixelsForRange = 300.0;
            s.toText = [maximum, offAtMax] (double v) {
                const auto i = juce::roundToInt (v);
                if (offAtMax && i >= maximum)
                    return tr ("Off");
                // steps:ticks, as FL shows them (24 ticks per step)
                const auto ticks = juce::roundToInt (channelmisc::flTimeKnobBeats (i) * 96.0);
                return juce::String (ticks / 24) + ":" + juce::String (ticks % 24).paddedLeft ('0', 2);
            };
            return s;
        };
        timeGate = knob ([timeSpec] { return timeSpec (TR_NOOP ("Gate: long notes are cut to this length (Off = no limit)"), 1446, 1446, true); },
                         [] (const ChannelDef& d) { return d.misc.gateBeats > 0.0f ? (double) channelmisc::flTimeKnobValue (d.misc.gateBeats) : 1446.0; },
                         [] (ChannelDef& d, double v) {
                             const auto i = juce::roundToInt (v);
                             d.misc.gateBeats = i >= 1446 ? 0.0f : (float) std::max (1.0 / 96.0, channelmisc::flTimeKnobBeats (i));
                         },
                         TR_NOOP ("Gate"));
        timeShift = knob ([timeSpec] { return timeSpec (TR_NOOP ("Fine time shift: delays the notes, up to one step"), 1024, 0, false); },
                          [] (const ChannelDef& d) { return (double) channelmisc::flTimeKnobValue (d.misc.shiftBeats); },
                          [] (ChannelDef& d, double v) { d.misc.shiftBeats = (float) channelmisc::flTimeKnobBeats (juce::roundToInt (v)); },
                          TR_NOOP ("Shift"));
        fullPorta.setButtonText (tr ("Full porta"));
        fullPorta.setTooltip (tr ("Full porta: portamento notes ignore the Gate and play their whole length"));
        fullPorta.onClick = [this] { edit ([on = fullPorta.getToggleState()] (ChannelDef& d) { d.misc.fullPorta = on; }); };
        // Vol / Key tracking
        trackVol.setButtonText (tr ("Vol"));
        trackKey.setButtonText (tr ("Key"));
        for (auto* b : { &trackVol, &trackKey })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (146);
            b->onClick = [this] { repaint(); for (auto& k : knobs) k->repaint(); };
            addAndMakeVisible (b);
        }
        trackVol.setTooltip (tr ("Velocity tracking: the note's velocity moves its pan"));
        trackKey.setTooltip (tr ("Keyboard tracking: the note's key moves its pan"));
        trackKey.setToggleState (true, juce::dontSendNotification);
        auto tracking = [this] (ChannelDef& d) -> NoteTracking& { return trackKey.getToggleState() ? d.misc.key : d.misc.velocity; };
        auto trackingOf = [this] (const ChannelDef& d) -> const NoteTracking& { return trackKey.getToggleState() ? d.misc.key : d.misc.velocity; };
        trackPan = knob ([name] { return ui::panSpec (name() + " - " + tr ("Tracking: pan")); },
                         [trackingOf] (const ChannelDef& d) { return (double) trackingOf (d).pan; },
                         [tracking] (ChannelDef& d, double v) { tracking (d).pan = (float) v; }, TR_NOOP ("Pan"));
        trackMid = knob ([this, name] {
                             const auto key = trackKey.getToggleState();
                             auto s = countSpec (name() + " - " + tr ("Tracking: the middle value (no offset)"), key ? 131 : 128, key ? 60 : 100,
                                                 [key] (int v) { return key ? juce::MidiMessage::getMidiNoteName (120 - v, true, true, 5) : juce::String (v); }); // FL: C5 = 60
                             return s;
                         },
                         [trackingOf] (const ChannelDef& d) { return (double) trackingOf (d).mid; },
                         [tracking] (ChannelDef& d, double v) { tracking (d).mid = juce::roundToInt (v); }, TR_NOOP ("Mid"));
        // FL's tracking Mod X / Y (round 151): the notes' Mod X / Y follow the velocity / key like their pan
        trackModX = knob ([name] { return ui::panSpec (name() + " - " + tr ("Tracking: Mod X (the note's filter cutoff / a plugin's MOD X)")); },
                          [trackingOf] (const ChannelDef& d) { return (double) trackingOf (d).modX; },
                          [tracking] (ChannelDef& d, double v) { tracking (d).modX = (float) v; }, TR_NOOP ("Mod X"));
        trackModY = knob ([name] { return ui::panSpec (name() + " - " + tr ("Tracking: Mod Y (the note's filter resonance)")); },
                          [trackingOf] (const ChannelDef& d) { return (double) trackingOf (d).modY; },
                          [tracking] (ChannelDef& d, double v) { tracking (d).modY = (float) v; }, TR_NOOP ("Mod Y"));
        // Echo delay / fat mode
        echoFeed = knob ([name] { return ui::percentSpec (name() + " - " + tr ("Delay feedback (100% = the echoes as loud as the note)"), 0.0); },
                         [] (const ChannelDef& d) { return (double) d.misc.echo.feedback / 2.0; },
                         [] (ChannelDef& d, double v) { d.misc.echo.feedback = (float) (2.0 * v); }, TR_NOOP ("Feed"));
        echoPan = knob ([name] { return ui::panSpec (name() + " - " + tr ("Delay pan: each echo moves this much")); },
                        [] (const ChannelDef& d) { return (double) d.misc.echo.pan; }, [] (ChannelDef& d, double v) { d.misc.echo.pan = (float) v; },
                        TR_NOOP ("Pan"));
        echoModX = knob ([name] { return ui::panSpec (name() + " - " + tr ("Delay Mod X: each echo moves the note's Mod X this much")); },
                         [] (const ChannelDef& d) { return (double) d.misc.echo.modX; }, [] (ChannelDef& d, double v) { d.misc.echo.modX = (float) v; },
                         TR_NOOP ("Mod X"));
        echoModY = knob ([name] { return ui::panSpec (name() + " - " + tr ("Delay Mod Y: each echo moves the note's Mod Y this much")); },
                         [] (const ChannelDef& d) { return (double) d.misc.echo.modY; }, [] (ChannelDef& d, double v) { d.misc.echo.modY = (float) v; },
                         TR_NOOP ("Mod Y"));
        echoPitch = knob ([name] {
                              ui::ValueSpec s;
                              s.name = name() + " - " + tr ("Delay pitch: each echo moves this many cents");
                              s.minimum = -1200.0;
                              s.maximum = 1200.0;
                              s.interval = 1.0;
                              s.toText = [] (double v) { return tr ("{0} cents", { juce::String (juce::roundToInt (v)) }); };
                              return s;
                          },
                          [] (const ChannelDef& d) { return (double) d.misc.echo.pitch; }, [] (ChannelDef& d, double v) { d.misc.echo.pitch = (float) v; },
                          TR_NOOP ("Pitch"));
        echoTime = knob ([name] {
                             ui::ValueSpec s;
                             s.name = name() + " - " + tr ("Delay time (steps:ticks)");
                             s.minimum = 1.0;
                             s.maximum = 192.0 * 4.0;
                             s.defaultValue = 144.0;
                             s.interval = 1.0;
                             s.pixelsForRange = 400.0;
                             s.toText = [] (double v) { const auto t = juce::roundToInt (v); return juce::String (t / 48) + ":" + juce::String ((t % 48) / 2).paddedLeft ('0', 2); };
                             return s;
                         },
                         [] (const ChannelDef& d) { return (double) d.misc.echo.timeBeats * 192.0; },
                         [] (ChannelDef& d, double v) { d.misc.echo.timeBeats = (float) (juce::roundToInt (v) / 192.0); }, TR_NOOP ("Time"));
        echoCount = knob ([name] { return countSpec (name() + " - " + tr ("Delay number of echoes"), 10, 4); },
                          [] (const ChannelDef& d) { return (double) d.misc.echo.echoes; },
                          [] (ChannelDef& d, double v) { d.misc.echo.echoes = juce::jlimit (1, 10, juce::roundToInt (v)); }, TR_NOOP ("Echoes"));
        pingPong.setButtonText (tr ("Ping pong"));
        pingPong.setTooltip (tr ("Ping pong: the echoes' pan bounces off the left and right ends"));
        pingPong.onClick = [this] { edit ([on = pingPong.getToggleState()] (ChannelDef& d) { d.misc.echo.pingPong = on; }); };
        fatMode.setButtonText (tr ("Fat mode"));
        fatMode.setTooltip (tr ("Fat mode: a second, mirrored series of echoes (opposite pan and pitch steps), all at the feedback level"));
        fatMode.onClick = [this] { edit ([on = fatMode.getToggleState()] (ChannelDef& d) { d.misc.echo.fat = on; }); };
        // Root note row: main pitch
        mainPitch.setButtonText (tr ("Enable main pitch"));
        mainPitch.setTooltip (tr ("The channel follows the main pitch (the toolbar's master pitch)"));
        mainPitch.onClick = [this] { edit ([on = mainPitch.getToggleState()] (ChannelDef& d) { d.misc.mainPitch = on; }); };
        // FL's Add to key and Root note keyboard (round 149): a sampler's root moves its pitch, other instruments are
        // transposed by (C5 − root) with Add to key
        addToKey.setButtonText (tr ("Add to key"));
        addToKey.setTooltip (tr ("Add to key: the notes are played the root note's distance from C5 away (instruments that are not samplers)"));
        addToKey.onClick = [this] { edit ([on = addToKey.getToggleState()] (ChannelDef& d) { d.misc.addToKey = on; }); };
        // FL's Root note › Fine tune (round 151 [DATA: .flp event 0x8E, cents]): every note of the channel this many
        // cents away
        fineTune = knob ([name] {
                             ui::ValueSpec s;
                             s.name = name() + " - " + tr ("Fine tuning");
                             s.minimum = -100.0;
                             s.maximum = 100.0;
                             s.interval = 1.0;
                             s.toText = [] (double v) { return tr ("{0} cents", { (v > 0.0 ? "+" : "") + juce::String (juce::roundToInt (v)) }); };
                             return s;
                         },
                         [] (const ChannelDef& d) { return (double) d.misc.fineTune; }, [] (ChannelDef& d, double v) { d.misc.fineTune = (float) v; },
                         TR_NOOP ("Fine tune"));
        rootReset.setButtonText (tr ("Reset"));
        rootReset.setTooltip (tr ("Reset the root note to C5"));
        rootReset.onClick = [this] { if (rootKeys.onPick) rootKeys.onPick (60); };
        addAndMakeVisible (rootReset);
        rootKeys.onPick = [this] (int key) {
            edit ([key] (ChannelDef& d) {
                if (d.instrument == InstrumentKind::sampler)
                {
                    // the sample plays at its own pitch on the root: the pitch moves by the root's change (within ±24)
                    const auto pitch = std::clamp (d.sampler.pitch + (float) (d.misc.rootNote - key), -24.0f, 24.0f);
                    const auto moved = (int) std::lround (pitch - d.sampler.pitch);
                    d.sampler.pitch = pitch;
                    d.misc.rootNote = std::clamp (d.misc.rootNote - moved, 0, 127);
                }
                else
                    d.misc.rootNote = key;
            });
        };
        rootKeys.onRegion = [this] (int low, int high) {
            edit ([low, high] (ChannelDef& d) {
                d.keyLow = juce::jlimit (0, 127, low);
                d.keyHigh = juce::jlimit (d.keyLow, 127, high);
            });
        };
        addAndMakeVisible (addToKey);
        addAndMakeVisible (rootKeys);
        // Arpeggiator: FL's Slide and Chord
        arpSlide.setButtonText (tr ("Slide"));
        arpSlide.setTooltip (tr ("Slide: the arpeggio glides from note to note (instruments that slide)"));
        arpSlide.onClick = [this] { edit ([on = arpSlide.getToggleState()] (ChannelDef& d) { d.arp.slide = on; }); };
        arpChord.addItem (tr ("Auto (sustain)"), 1);
        arpChord.addItem (tr ("None (octaves)"), 2);
        for (const auto& [chordName, intervals] : arpChords())
            arpChord.addItem (tr (chordName), arpChord.getNumItems() + 1);
        arpChord.setTooltip (tr ("Chord: Auto plays the held notes; a chord plays it on the lowest held note"));
        arpChord.onChange = [this] {
            const auto id = arpChord.getSelectedId();
            if (id <= 0)
                return;
            edit ([id] (ChannelDef& d) {
                if (id == 1)
                    d.arp.chord.clear();
                else if (id == 2)
                    d.arp.chord = { 0 };
                else if (id - 3 < (int) arpChords().size())
                    d.arp.chord = arpChords()[(std::size_t) (id - 3)].second;
            });
        };
        for (auto* b : { &fullPorta, &pingPong, &fatMode, &mainPitch, &arpSlide })
            addAndMakeVisible (b);
        addAndMakeVisible (arpChord);
        // Group
        cut = knob ([this, name] { return countSpec (name() + " - " + tr ("Cut: notes of this channel stop the channels cut by this group"), 
                                                     juce::jmax (99, current() != nullptr ? current()->cutGroup : 0), 0,
                                                     [this] (int g) { return juce::String (cutGroupText (g, channel)); }); },
                    [] (const ChannelDef& d) { return (double) d.cutGroup; }, [] (ChannelDef& d, double v) { d.cutGroup = juce::jmax (0, juce::roundToInt (v)); },
                    TR_NOOP ("Cut group"));
        cutBy = knob ([this, name] { return countSpec (name() + " - " + tr ("By: this channel is cut by the channels of this group"),
                                                       juce::jmax (99, current() != nullptr ? current()->cutBy : 0), 0,
                                                       [this] (int g) { return juce::String (cutGroupText (g, channel)); }); },
                      [] (const ChannelDef& d) { return (double) d.cutBy; }, [] (ChannelDef& d, double v) { d.cutBy = juce::jmax (0, juce::roundToInt (v)); },
                      TR_NOOP ("Cut by"));
        cutSelf.setButtonText (tr ("Cut self"));
        cutSelf.setClickingTogglesState (true);
        cutSelf.setTooltip (tr ("Cut self: a new note of this channel stops its own sounding note (hi-hats, 808s)"));
        cutSelf.onClick = [this] { edit ([on = cutSelf.getToggleState(), ch = channel] (ChannelDef& d) { setChannelCutsItself (d, ch, on); }); };
        // Arpeggiator
        arpDirection.addItemList ({ tr ("Off"), tr ("Up"), tr ("Down"), tr ("Up and down"), tr ("Random"), tr ("Up and down (sticky)") }, 1);
        arpDirection.setTooltip (tr ("Arpeggiator direction (Off = the notes play as written)"));
        arpDirection.onChange = [this] { edit ([d = arpDirection.getSelectedId() - 1] (ChannelDef& c) { c.arp.direction = juce::jlimit (0, 5, d); }); };
        for (const auto& [text, beats] : arpSpeeds())
            arpTime.addItem (tr (text), arpTime.getNumItems() + 1);
        arpTime.setTooltip (tr ("Arpeggiator time: the length of each step"));
        arpTime.onChange = [this] {
            const auto i = arpTime.getSelectedId() - 1;
            if (i >= 0 && i < (int) arpSpeeds().size())
                edit ([b = arpSpeeds()[(std::size_t) i].second] (ChannelDef& c) { c.arp.stepBeats = b; });
        };
        arpRange = knob ([name] { return countSpec (name() + " - " + tr ("Arpeggiator range (octaves)"), 4, 1); },
                         [] (const ChannelDef& d) { return (double) d.arp.range; },
                         [] (ChannelDef& d, double v) { d.arp.range = juce::jlimit (1, 4, juce::roundToInt (v)); }, TR_NOOP ("Range"));
        arpGate = knob ([name] {
                            auto s = ui::percentSpec (name() + " - " + tr ("Arpeggio gate (past 100% the next note waits)"), 0.5);
                            s.minimum = 0.05;
                            s.maximum = 128.0 / 48.0;
                            return s;
                        },
                        [] (const ChannelDef& d) { return (double) d.arp.gate; },
                        [] (ChannelDef& d, double v) { d.arp.gate = juce::jlimit (0.05f, 128.0f / 48.0f, (float) v); }, TR_NOOP ("Gate"));
        // FL's Repeat 0 plays the whole chord at once on each step (round 149)
        arpRepeat = knob ([name] { return countSpec (name() + " - " + tr ("Arpeggiator repeat: times the arpeggio plays in an octave before the next one (0: the chord at once)"), 16, 1,
                                                     [] (int i) { return i == 0 ? tr ("Chord") : juce::String (i); }); },
                          [] (const ChannelDef& d) { return (double) d.arp.octaveRepeat; },
                          [] (ChannelDef& d, double v) { d.arp.octaveRepeat = juce::jlimit (0, 16, juce::roundToInt (v)); }, TR_NOOP ("Repeat"));

        for (auto* b : { &porta, &mono })
            addAndMakeVisible (b);
        addAndMakeVisible (cutSelf);
        addAndMakeVisible (arpDirection);
        addAndMakeVisible (arpTime);
        refresh();
        engine.addChangeListener (this);
        setSize (preferredWidth, preferredHeight);
    }

    ~MiscPanel() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        for (const auto& [area, title] : sections)
        {
            g.setColour (theme::colour::bg3);
            g.fillRoundedRectangle (area.toFloat(), 4.0f);
            g.setColour (theme::colour::textSecondary);
            g.setFont (theme::font (theme::type::caption, true));
            auto text = theme::caps (title);
            if (&title == &sections[(std::size_t) juce::jmax (0, rootSection)].second && rootSection >= 0 && current() != nullptr)
                text << ": " << RootKeyboard::keyName (current()->misc.rootNote); // FL's "Root note: C5"
            g.drawText (text, area.withHeight (20).reduced (8, 0), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        sections.clear();
        auto area = getLocalBounds().reduced (8);
        auto section = [this] (juce::Rectangle<int> r, const juce::String& title) {
            sections.push_back ({ r, title });
            return r.withTrimmedTop (20).reduced (6, 4);
        };
        constexpr int knobW = 50, knobH = 64;
        auto row = [] (juce::Rectangle<int>& r, std::initializer_list<FrameKnob*> ks) {
            auto line = r.removeFromTop (knobH);
            for (auto* k : ks)
                k->setBounds (line.removeFromLeft (knobW));
        };
        auto top = area.removeFromTop (118);
        {
            auto levels = section (top.removeFromLeft (4 * knobW + 12), tr ("Levels adjustment"));
            row (levels, { pan, volume, modX, modY });
        }
        top.removeFromLeft (6);
        {
            auto poly = section (top.removeFromLeft (2 * knobW + 30), tr ("Polyphony"));
            row (poly, { maxPoly, slide });
            auto toggles = poly.removeFromTop (22);
            porta.setBounds (toggles.removeFromLeft (64));
            mono.setBounds (toggles);
        }
        top.removeFromLeft (6);
        {
            auto time = section (top.removeFromLeft (3 * knobW + 12), tr ("Time"));
            row (time, { timeGate, timeShift, swing });
            fullPorta.setBounds (time.removeFromTop (22));
        }
        top.removeFromLeft (6);
        {
            auto track = section (top, tr ("Tracking"));
            auto tabs = track.removeFromTop (0);
            juce::ignoreUnused (tabs);
            const auto title = sections.back().first;
            trackVol.setBounds (title.getRight() - 84, title.getY() + 2, 38, 16);
            trackKey.setBounds (title.getRight() - 44, title.getY() + 2, 38, 16);
            row (track, { trackPan, trackModX, trackModY, trackMid });
        }
        area.removeFromTop (6);
        auto bottom = area.removeFromTop (130);
        {
            auto group = section (bottom.removeFromLeft (2 * knobW + 16), tr ("Group"));
            row (group, { cut, cutBy });
            cutSelf.setBounds (group.removeFromTop (24).reduced (4, 0));
        }
        bottom.removeFromLeft (6);
        {
            auto arp = section (bottom.removeFromLeft (330), tr ("Arpeggiator"));
            auto boxes = arp.removeFromLeft (118);
            arpDirection.setBounds (boxes.removeFromTop (22));
            boxes.removeFromTop (4);
            arpTime.setBounds (boxes.removeFromTop (22));
            boxes.removeFromTop (4);
            arpChord.setBounds (boxes.removeFromTop (22));
            boxes.removeFromTop (4);
            arpSlide.setBounds (boxes.removeFromTop (22));
            arp.removeFromLeft (6);
            row (arp, { arpRange, arpGate, arpRepeat });
        }
        bottom.removeFromLeft (6);
        {
            auto echo = section (bottom, tr ("Echo delay / fat mode"));
            {
                // seven knobs (round 151 added Mod X / Y): narrower when the window is
                auto line = echo.removeFromTop (knobH);
                const auto w = juce::jmin (knobW, line.getWidth() / 7);
                for (auto* k : { echoFeed, echoPan, echoModX, echoModY, echoPitch, echoTime, echoCount })
                    k->setBounds (line.removeFromLeft (w));
            }
            auto toggles = echo.removeFromTop (22);
            pingPong.setBounds (toggles.removeFromLeft (100));
            fatMode.setBounds (toggles.removeFromLeft (100));
        }
        area.removeFromTop (6);
        {
            auto rootRow = section (area.removeFromTop (74), tr ("Root note"));
            rootSection = (int) sections.size() - 1;
            auto toggles = rootRow.removeFromLeft (150);
            mainPitch.setBounds (toggles.removeFromTop (22));
            auto line = toggles.removeFromTop (22);
            rootReset.setBounds (line.removeFromRight (50).reduced (0, 1));
            addToKey.setBounds (line);
            auto fine = rootRow.removeFromLeft (58);
            row (fine, { fineTune });
            rootKeys.setBounds (rootRow.reduced (4, 2));
        }
    }

private:
    // LYNX's chord choices for the arpeggiator (the piano roll's chord stamps)
    static const std::vector<std::pair<const char*, std::vector<int>>>& arpChords()
    {
        static const std::vector<std::pair<const char*, std::vector<int>>> chords {
            { TR_NOOP ("Major"), { 0, 4, 7 } }, { TR_NOOP ("Minor"), { 0, 3, 7 } }, { "Dim", { 0, 3, 6 } }, { "Aug", { 0, 4, 8 } },
            { "Sus2", { 0, 2, 7 } }, { "Sus4", { 0, 5, 7 } }, { "7", { 0, 4, 7, 10 } }, { "Maj7", { 0, 4, 7, 11 } },
            { "m7", { 0, 3, 7, 10 } }, { "m9", { 0, 3, 7, 10, 14 } }, { TR_NOOP ("Octave"), { 0, 12 } },
        };
        return chords;
    }

    static const std::vector<std::pair<const char*, float>>& arpSpeeds()
    {
        static const std::vector<std::pair<const char*, float>> speeds { { "1/4", 1.0f },  { "1/8", 0.5f },
                                                                         { TR_NOOP ("1/8 triplet"), 1.0f / 3.0f }, { "1/16", 0.25f },
                                                                         { TR_NOOP ("1/16 triplet"), 1.0f / 6.0f }, { "1/32", 0.125f } };
        return speeds;
    }

    const ChannelDef* current() const
    {
        const auto& channels = engine.getSong().channels;
        return channel >= 0 && channel < (int) channels.size() ? &channels[(std::size_t) channel] : nullptr;
    }

    void edit (std::function<void (ChannelDef&)> change)
    {
        if (updating || current() == nullptr)
            return;
        engine.editSong ([ch = channel, change] (Song& s) {
            if (ch < (int) s.channels.size())
                change (s.channels[(std::size_t) ch]);
        });
    }

    void refresh()
    {
        const auto* c = current();
        if (c == nullptr)
            return;
        updating = true;
        porta.setToggleState (channelPortamento (*c), juce::dontSendNotification);
        porta.setEnabled (channelHasPortamento (*c));
        slide->setEnabled (channelHasPortamento (*c));
        mono.setToggleState (channelMono (*c), juce::dontSendNotification);
        mono.setEnabled (channelHasMono (*c));
        cutSelf.setToggleState (channelCutsItself (*c), juce::dontSendNotification);
        arpDirection.setSelectedId (juce::jlimit (0, 5, c->arp.direction) + 1, juce::dontSendNotification);
        fullPorta.setToggleState (c->misc.fullPorta, juce::dontSendNotification);
        pingPong.setToggleState (c->misc.echo.pingPong, juce::dontSendNotification);
        fatMode.setToggleState (c->misc.echo.fat, juce::dontSendNotification);
        mainPitch.setToggleState (c->misc.mainPitch, juce::dontSendNotification);
        addToKey.setToggleState (c->misc.addToKey, juce::dontSendNotification);
        addToKey.setEnabled (c->instrument != InstrumentKind::sampler);
        rootKeys.setRoot (c->misc.rootNote);
        rootKeys.setRegion (c->keyLow, c->keyHigh);
        arpSlide.setToggleState (c->arp.slide, juce::dontSendNotification);
        const auto filter = c->instrument == InstrumentKind::synth || c->instrument == InstrumentKind::sampler;
        modX->setEnabled (filter);
        modY->setEnabled (filter);
        {
            auto chordId = c->arp.chord.empty() ? 1 : (c->arp.chord == std::vector<int> { 0 } ? 2 : 0);
            for (int i = 0; chordId == 0 && i < (int) arpChords().size(); ++i)
                if (arpChords()[(std::size_t) i].second == c->arp.chord)
                    chordId = i + 3;
            arpChord.setSelectedId (chordId, juce::dontSendNotification);
            if (chordId == 0)
                arpChord.setText (tr ("Chord from FL"), juce::dontSendNotification);
        }
        int speed = 0;
        for (int i = 0; i < (int) arpSpeeds().size(); ++i)
            if (std::abs (arpSpeeds()[(std::size_t) i].second - c->arp.stepBeats) < 1e-4f)
                speed = i + 1;
        arpTime.setSelectedId (speed, juce::dontSendNotification);
        if (speed == 0)
            arpTime.setText (tr ("{0} beats", { juce::String (c->arp.stepBeats, 3) }), juce::dontSendNotification);
        updating = false;
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    int channel;
    bool updating = false;
    std::vector<std::unique_ptr<FrameKnob>> knobs;
    FrameKnob *pan = nullptr, *volume = nullptr, *maxPoly = nullptr, *slide = nullptr, *swing = nullptr, *cut = nullptr, *cutBy = nullptr,
              *arpRange = nullptr, *arpGate = nullptr, *arpRepeat = nullptr, *modX = nullptr, *modY = nullptr, *timeGate = nullptr,
              *timeShift = nullptr, *trackPan = nullptr, *trackMid = nullptr, *echoFeed = nullptr, *echoPan = nullptr, *echoPitch = nullptr,
              *echoTime = nullptr, *echoCount = nullptr, *trackModX = nullptr, *trackModY = nullptr, *echoModX = nullptr, *echoModY = nullptr,
              *fineTune = nullptr;
    juce::ToggleButton porta, mono, fullPorta, pingPong, fatMode, mainPitch, arpSlide, addToKey;
    RootKeyboard rootKeys;
    juce::TextButton rootReset;
    int rootSection = -1;
    juce::TextButton trackVol, trackKey;
    juce::ComboBox arpChord;
    juce::TextButton cutSelf;
    juce::ComboBox arpDirection, arpTime;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> sections;
};

// The frame: header strip (tabs on the left; ON, PAN, VOL, TRACK on the right) over the page of the selected tab.
class ChannelFrame final : public juce::Component,
                           private juce::ChangeListener
{
public:
    static constexpr int headerHeight = 62;

    ChannelFrame (AudioEngine& e, int c, std::unique_ptr<juce::Component> instrumentPage, std::unique_ptr<juce::Component> envelopePage)
        : engine (e), channel (c)
    {
        auto addPage = [this] (std::unique_ptr<juce::Component> page, const juce::String& title, const juce::String& tip) {
            auto button = std::make_unique<juce::TextButton> (title);
            button->setTooltip (tip);
            button->setClickingTogglesState (true);
            button->setRadioGroupId (1);
            const auto index = (int) pages.size();
            button->onClick = [this, index] { showTab (index); };
            addAndMakeVisible (*button);
            addChildComponent (*page);
            sizes.push_back ({ juce::jmax (1, page->getWidth()), juce::jmax (1, page->getHeight()) });
            tabs.push_back (std::move (button));
            pages.push_back (std::move (page));
        };
        if (instrumentPage != nullptr) // (none: a channel whose instrument has its own window, e.g. a hosted plugin)
            addPage (std::move (instrumentPage), tr ("Plugin"), tr ("The instrument's own settings"));
        if (envelopePage != nullptr)
            addPage (std::move (envelopePage), tr ("Envelope / instrument"), tr ("Envelope / instrument settings: the filter and pitch envelopes and the LFOs"));
        addPage (std::make_unique<MiscPanel> (engine, channel), tr ("Misc"), tr ("Miscellaneous functions: levels, polyphony, time, tracking, cut groups, arpeggiator, echo delay"));

        on.setButtonText (tr ("On"));
        on.setTooltip (tr ("Channel on / off (mute)"));
        on.onClick = [this] {
            const auto muted = ! on.getToggleState();
            engine.editSong ([ch = channel, muted] (Song& s) {
                if (ch < (int) s.channels.size())
                    s.channels[(std::size_t) ch].muted = muted;
            });
        };
        addAndMakeVisible (on);
        const auto name = [this] { return channelNameOf (engine.getSong(), channel); };
        auto headerKnob = [this] (std::function<ui::ValueSpec()> spec, std::function<double (const ChannelDef&)> get, std::function<void (Song&, int, double)> set,
                                  const char* caption) {
            auto k = std::make_unique<FrameKnob> (
                engine, std::move (spec),
                [this, get] { return channel < (int) engine.getSong().channels.size() ? get (engine.getSong().channels[(std::size_t) channel]) : 0.0; },
                [this, set] (double v) {
                    engine.editSong ([ch = channel, set, v] (Song& s) {
                        if (ch < (int) s.channels.size())
                            set (s, ch, v);
                    });
                },
                tr (caption));
            addAndMakeVisible (*k);
            header.push_back (std::move (k));
        };
        headerKnob ([this, name] {
                        auto s = ui::panSpec (name() + " - " + tr ("Pan"));
                        s.target = AutomationTarget { AutomationTargetKind::channelPan, channel };
                        return s;
                    },
                    [] (const ChannelDef& d) { return (double) d.pan; }, [] (Song& s, int ch, double v) { s.channels[(std::size_t) ch].pan = (float) v; },
                    TR_NOOP ("Pan"));
        headerKnob ([this, name] {
                        auto s = ui::percentSpec (name() + " - " + tr ("Volume"), 0.78);
                        s.target = AutomationTarget { AutomationTargetKind::channelVolume, channel };
                        return s;
                    },
                    [] (const ChannelDef& d) { return (double) d.volume; },
                    [] (Song& s, int ch, double v) { s.channels[(std::size_t) ch].volume = (float) v; }, TR_NOOP ("Vol"));
        // FL's PITCH knob and its RANGE (REA round 146b): the channel's pitch, within ± range semitones
        headerKnob ([this, name] {
                        ui::ValueSpec s;
                        s.name = name() + " - " + tr ("Pitch (semitones, within the range)");
                        const auto range = channel < (int) engine.getSong().channels.size() ? engine.getSong().channels[(std::size_t) channel].misc.pitchRange : 2;
                        s.minimum = -juce::jmax (1, range);
                        s.maximum = juce::jmax (1, range);
                        s.toText = [] (double v) { return tr ("{0} cents", { juce::String (juce::roundToInt (v * 100.0)) }); };
                        return s;
                    },
                    [] (const ChannelDef& d) { return (double) d.sampler.pitch; },
                    [] (Song& s, int ch, double v) { s.channels[(std::size_t) ch].sampler.pitch = (float) juce::jlimit (-24.0, 24.0, v); }, TR_NOOP ("Pitch"));
        headerKnob ([name] { return countSpec (name() + " - " + tr ("Pitch knob range (semitones)"), 48, 2,
                                               [] (int v) { return juce::String (v); }); },
                    [] (const ChannelDef& d) { return (double) d.misc.pitchRange; },
                    [] (Song& s, int ch, double v) { s.channels[(std::size_t) ch].misc.pitchRange = juce::jlimit (0, 48, juce::roundToInt (v)); },
                    TR_NOOP ("Range"));
        headerKnob ([this, name] {
                        return countSpec (name() + " - " + tr ("Target mixer track"), maxMixerTracks - 1, 0,
                                          [] (int t) { return t == 0 ? tr ("Master") : juce::String (t); });
                    },
                    [] (const ChannelDef& d) { return (double) d.mixerTrack; },
                    [] (Song& s, int ch, double v) {
                        mixerops::routeChannel (s, ch, juce::roundToInt (v)); // any insert (round 162): missing ones are added
                    },
                    TR_NOOP ("Track"));

        int width = MiscPanel::preferredWidth, height = MiscPanel::preferredHeight;
        for (auto& p : pages)
        {
            width = juce::jmax (width, p->getWidth());
            height = juce::jmax (height, p->getHeight());
        }
        refresh();
        engine.addChangeListener (this);
        setSize (width, headerHeight + height);
        showTab (0);
    }

    ~ChannelFrame() override { engine.removeChangeListener (this); }

    void showTab (int index)
    {
        index = juce::jlimit (0, (int) pages.size() - 1, index);
        current = index;
        for (int i = 0; i < (int) pages.size(); ++i)
        {
            pages[(std::size_t) i]->setVisible (i == index);
            tabs[(std::size_t) i]->setToggleState (i == index, juce::dontSendNotification);
        }
    }

    int getTab() const noexcept { return current; }
    int tabCount() const noexcept { return (int) pages.size(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::bg3);
        g.fillRect (getLocalBounds().removeFromTop (headerHeight));
        g.setColour (theme::colour::border);
        g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto tabsArea = getLocalBounds().removeFromTop (headerHeight).reduced (8, 6);
        for (auto& t : tabs)
        {
            const auto w = juce::jmax (64, (int) juce::GlyphArrangement::getStringWidth (juce::Font (theme::font (theme::type::body)), t->getButtonText()) + 24);
            t->setBounds (tabsArea.removeFromLeft (w).withSizeKeepingCentre (w, 26));
            tabsArea.removeFromLeft (4);
        }
        auto right = getLocalBounds().removeFromTop (headerHeight).reduced (6, 2);
        for (auto it = header.rbegin(); it != header.rend(); ++it)
            (*it)->setBounds (right.removeFromRight (48));
        on.setBounds (right.removeFromRight (52).withSizeKeepingCentre (52, 24));
        // each page keeps its own size (some scale their drawing with it), centred under the header
        const auto body = getLocalBounds().withTrimmedTop (headerHeight);
        for (std::size_t i = 0; i < pages.size(); ++i)
        {
            const auto w = juce::jmin (body.getWidth(), sizes[i].x), h = juce::jmin (body.getHeight(), sizes[i].y);
            pages[i]->setBounds (body.getX() + (body.getWidth() - w) / 2, body.getY(), w, h);
        }
    }

private:
    void refresh()
    {
        if (channel < (int) engine.getSong().channels.size())
            on.setToggleState (! engine.getSong().channels[(std::size_t) channel].muted, juce::dontSendNotification);
        for (auto& k : header)
            k->repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    int channel;
    int current = 0;
    std::vector<std::unique_ptr<juce::TextButton>> tabs;
    std::vector<std::unique_ptr<juce::Component>> pages;
    std::vector<juce::Point<int>> sizes;
    std::vector<std::unique_ptr<FrameKnob>> header;
    juce::ToggleButton on;
};

bool hasEnvelopeTab (InstrumentKind kind) noexcept
{
    return kind == InstrumentKind::synth || kind == InstrumentKind::sampler;
}

void showModulationFor (juce::Component& from, AudioEngine& engine, int channel)
{
    if (auto* frame = from.findParentComponentOfClass<ChannelFrame>(); frame != nullptr && frame->tabCount() > 2)
        frame->showTab (1);
    else
        openChannelModulation (engine, channel);
}

class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow (AudioEngine& engine, int c, bool modulationWindow)
        : juce::DocumentWindow (tr ("Channel settings"), theme::colour::bg2, DocumentWindow::closeButton),
          channel (c),
          modulation (modulationWindow)
    {
        setUsingNativeTitleBar (true);
        // FL names the window after the channel and its mixer track (REA round 149 [EXEC]: "FL Studio VSTi (Master)",
        // "Synth Bass (Synth Bass)")
        if (const auto& song = engine.getSong(); channel >= 0 && channel < (int) song.channels.size())
        {
            const auto& def = song.channels[(std::size_t) channel];
            const auto t = def.mixerTrack;
            const auto track = t <= 0 || t >= (int) song.mixer.size() ? tr ("Master")
                               : song.mixer[(std::size_t) t].name.empty() ? tr ("Insert {0}", { juce::String (t) })
                                                                          : juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str());
            setName (juce::String::fromUTF8 (def.name.c_str()) + " (" + track + ")"
                     + (modulation ? " - " + tr ("Envelope / instrument") : juce::String()));
        }

        if (modulation)
            setContentOwned (new ModulationPanel (engine, channel), true);
        else if (! hasChannelSettings (engine.getSong().channels[(std::size_t) channel].instrument))
            setContentOwned (new ChannelFrame (engine, channel, nullptr, nullptr), true); // the Misc tab alone
        else
            setContentOwned (makeChannelFrame (engine, channel).release(), true);

        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
    }

    int getChannel() const { return channel; }
    bool isModulation() const { return modulation; }
    void closeButtonPressed() override;

private:
    int channel;
    bool modulation;
};

std::vector<std::unique_ptr<SettingsWindow>>& windows()
{
    static std::vector<std::unique_ptr<SettingsWindow>> open;
    return open;
}

void SettingsWindow::closeButtonPressed()
{
    auto* self = this;
    juce::MessageManager::callAsync ([self] {
        auto& open = windows();
        open.erase (std::remove_if (open.begin(), open.end(), [self] (auto& w) { return w.get() == self; }), open.end());
    });
}
} // namespace

namespace
{
void openWindow (AudioEngine& engine, int channel, bool modulation)
{
    for (auto& w : windows())
        if (w->getChannel() == channel && w->isModulation() == modulation)
        {
            w->toFront (true);
            return;
        }

    windows().push_back (std::make_unique<SettingsWindow> (engine, channel, modulation));
}
} // namespace

void openChannelSettings (AudioEngine& engine, int channel)
{
    openWindow (engine, channel, false);
}

void openChannelMisc (AudioEngine& engine, int channel)
{
    openWindow (engine, channel, false);
    for (auto& w : windows())
        if (w->getChannel() == channel && ! w->isModulation())
            if (auto* frame = dynamic_cast<ChannelFrame*> (w->getContentComponent()))
                frame->showTab (frame->tabCount() - 1);
}

void openChannelModulation (AudioEngine& engine, int channel)
{
    openWindow (engine, channel, true);
}

void closeChannelSettings()
{
    windows().clear();
}

bool hasChannelSettings (InstrumentKind kind) noexcept
{
    switch (kind)
    {
        case InstrumentKind::synth:
        case InstrumentKind::sampler:
        case InstrumentKind::controller:
        case InstrumentKind::midiOut:
        case InstrumentKind::sakura:
        case InstrumentKind::drumpad:
        case InstrumentKind::boobass:
        case InstrumentKind::drumsynth:
        case InstrumentKind::harmless:
        case InstrumentKind::harmor:
        case InstrumentKind::gms:
        case InstrumentKind::morphine:
        case InstrumentKind::ogun:
        case InstrumentKind::minisynth:
        case InstrumentKind::dx10:
        case InstrumentKind::kepler:
        case InstrumentKind::poizone:
        case InstrumentKind::fruitkick:
        case InstrumentKind::bassdrum:
        case InstrumentKind::toxic:
        case InstrumentKind::threeosc:
        case InstrumentKind::autogun:
        case InstrumentKind::transistorbass:
        case InstrumentKind::simsynth:
        case InstrumentKind::wasp:
        case InstrumentKind::plucked:
        case InstrumentKind::beepmap:
        case InstrumentKind::sawer:
        case InstrumentKind::flkeys:
        case InstrumentKind::granulizer:
        case InstrumentKind::drumaxx:
        case InstrumentKind::slayer:
        case InstrumentKind::fruitydance:
        case InstrumentKind::sytrus:
        case InstrumentKind::soundfont:
        case InstrumentKind::layer:
            return true;
        default:
            return false;
    }
}

bool isChannelSettingsOpen (int channel)
{
    return std::any_of (windows().begin(), windows().end(), [channel] (auto& w) { return w->getChannel() == channel && ! w->isModulation(); });
}

void closeChannelSettings (int channel)
{
    auto& open = windows();
    open.erase (std::remove_if (open.begin(), open.end(), [channel] (auto& w) { return w->getChannel() == channel && ! w->isModulation(); }),
                open.end());
}

std::unique_ptr<juce::Component> makeChannelModulationPanel (AudioEngine& engine, int channel)
{
    return std::make_unique<ModulationPanel> (engine, channel);
}

std::unique_ptr<juce::Component> makeChannelFrame (AudioEngine& engine, int channel, int tab)
{
    const auto kind = engine.getSong().channels[(std::size_t) channel].instrument;
    auto frame = std::make_unique<ChannelFrame> (engine, channel, makeChannelSettingsPanel (engine, channel),
                                                 hasEnvelopeTab (kind) ? std::make_unique<ModulationPanel> (engine, channel) : nullptr);
    frame->showTab (tab < 0 ? frame->tabCount() - 1 : tab);
    return frame;
}

std::unique_ptr<juce::Component> makeChannelSettingsPanel (AudioEngine& engine, int channel)
{
    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::synth)
        return std::make_unique<SynthPanel> (engine, channel);
    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::controller)
    {
        if (parseKeyboardController (engine.getSong().channels[(std::size_t) channel].controllerState))
            return std::make_unique<ControllerPanel> (engine, channel);
        return makeEnvelopeControllerEditor (engine, channel);
    }
    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::midiOut)
        return makeMidiOutEditor (engine, channel);
    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::soundfont)
        return makeSoundFontEditor (engine, channel);
    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::layer)
        return makeLayerEditor (engine, channel);
    if (const auto kind = engine.getSong().channels[(std::size_t) channel].instrument;
        kind == InstrumentKind::sakura || kind == InstrumentKind::drumpad || kind == InstrumentKind::boobass
                 || kind == InstrumentKind::drumsynth || kind == InstrumentKind::harmless || kind == InstrumentKind::harmor
                 || kind == InstrumentKind::gms || kind == InstrumentKind::morphine
                 || kind == InstrumentKind::ogun || kind == InstrumentKind::minisynth
                 || kind == InstrumentKind::dx10 || kind == InstrumentKind::kepler || kind == InstrumentKind::poizone
                 || kind == InstrumentKind::fruitkick || kind == InstrumentKind::bassdrum || kind == InstrumentKind::toxic
                 || kind == InstrumentKind::threeosc
                 || kind == InstrumentKind::autogun || kind == InstrumentKind::transistorbass
                 || kind == InstrumentKind::simsynth || kind == InstrumentKind::wasp || kind == InstrumentKind::plucked
                 || kind == InstrumentKind::beepmap || kind == InstrumentKind::sawer || kind == InstrumentKind::flkeys
                 || kind == InstrumentKind::granulizer || kind == InstrumentKind::drumaxx
                 || kind == InstrumentKind::slayer || kind == InstrumentKind::fruitydance || kind == InstrumentKind::sytrus)
        return std::make_unique<ModelPanel> (engine, channel);

    return std::make_unique<SettingsPanel> (engine, channel);
}
} // namespace lynx
