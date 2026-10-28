#include "ui/VoltageEditor.h"

#include "core/VoltageController.h"
#include "ui/ControllerKnob.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace lynx
{
namespace
{
// The Voltage Controller's settings page (LYNX's own layout of FL's controls, REA_rodada164a §6): the global switches
// on top, then one row per output — its source, offset, scale and polarity, and for each extra pair the mixer insert
// it feeds.
class VoltageEditor final : public juce::Component,
                            private juce::ChangeListener
{
public:
    // `channel`: the Channel Rack channel (its offsets / scales are automation and link targets, round 165); −1 in a Patcher
    VoltageEditor (AudioEngine& e, std::function<ChannelDef* (Song&)> l, int channel = -1) : engine (e), locate (std::move (l))
    {
        enable.setButtonText (tr ("On"));
        enable.setClickingTogglesState (true);
        enable.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
        enable.onClick = [this] { edit ([on = enable.getToggleState()] (voltage::Settings& s) { s.enabled = on; }); };
        ui::setHint (enable, tr ("Sends the voltages (off: silence, FL's default for a new one)"));
        addAndMakeVisible (enable);

        range.addItem (juce::String::fromUTF8 ("\xc2\xb1" "10 V"), 1);
        range.addItem (juce::String::fromUTF8 ("\xc2\xb1" "5 V"), 2);
        range.addItem (juce::String::fromUTF8 ("0\xe2\x80\xa6" "10 V"), 3);
        range.onChange = [this] { edit ([i = range.getSelectedItemIndex()] (voltage::Settings& s) { s.range = juce::jlimit (0, 2, i); }); };
        ui::setHint (range, tr ("Voltage range of the interface: full scale (1.0) is 10 V"));
        addAndMakeVisible (range);

        pitch.addItemList ({ "1 V/oct", "1.2 V/oct" }, 1);
        pitch.onChange = [this] { edit ([i = pitch.getSelectedItemIndex()] (voltage::Settings& s) { s.pitchStandard = juce::jlimit (0, 1, i); }); };
        ui::setHint (pitch, tr ("Pitch standard of the modules: volts per octave (0 V = C5)"));
        addAndMakeVisible (pitch);

        for (const auto p : clockChoices)
            clock.addItem (tr ("Clock {0} PPQ", { juce::String (p) }), p);
        clock.onChange = [this] { edit ([p = clock.getSelectedId()] (voltage::Settings& s) { s.clockPpq = juce::jmax (1, p); }); };
        ui::setHint (clock, tr ("Clock pulses per quarter note (while the song plays)"));
        addAndMakeVisible (clock);

        ui::ValueSpec pulseSpec;
        pulseSpec.name = tr ("Pulse length");
        pulseSpec.minimum = 1.0;
        pulseSpec.maximum = 500.0;
        pulseSpec.interval = 1.0;
        pulseSpec.defaultValue = 15.0;
        pulseSpec.toText = [] (double v) { return juce::String (juce::roundToInt (v)) + " smp"; };
        pulse = std::make_unique<ctlui::Knob> (&engine, pulseSpec, [this] { return (double) current().pulseLength; },
                                               [this] (double v) { edit ([v] (voltage::Settings& s) { s.pulseLength = juce::jlimit (1, 500, juce::roundToInt (v)); }); });
        pulse->shortName = tr ("Pulse");
        addAndMakeVisible (*pulse);

        for (int o = 0; o < voltage::outputs; ++o)
        {
            auto& row = rows[(std::size_t) o];
            for (int k = 0; k < voltage::sourceCount; ++k)
                row.source.addItem (tr (voltage::sourceName ((voltage::Source) k)), k + 1);
            row.source.onChange = [this, o, &row] {
                edit ([o, k = row.source.getSelectedId() - 1] (voltage::Settings& s) {
                    s.source[(std::size_t) o] = (voltage::Source) juce::jlimit (0, voltage::sourceCount - 1, k);
                });
            };
            addAndMakeVisible (row.source);

            ui::ValueSpec off;
            off.name = tr ("{0} offset", { voltage::outputName (o) });
            off.defaultValue = 0.5;
            if (channel >= 0)
                off.target = AutomationTarget { AutomationTargetKind::channelParam, channel, 0, o };
            off.toText = [this, o] (double v) {
                auto s = current();
                s.params[(std::size_t) o] = juce::roundToInt (v * 65536.0);
                return juce::String (voltage::offsetOf (s, o) * 10.0f, 2) + " V";
            };
            row.offset = std::make_unique<ctlui::Knob> (&engine, off, [this, o] { return current().params[(std::size_t) o] / 65536.0; },
                                                        [this, o] (double v) {
                                                            edit ([o, v] (voltage::Settings& s) { s.params[(std::size_t) o] = juce::jlimit (0, 65536, juce::roundToInt (v * 65536.0)); });
                                                        });
            row.offset->shortName = tr ("Offset");
            addAndMakeVisible (*row.offset);

            ui::ValueSpec sc;
            sc.name = tr ("{0} scale", { voltage::outputName (o) });
            sc.defaultValue = 1.0;
            if (channel >= 0)
                sc.target = AutomationTarget { AutomationTargetKind::channelParam, channel, 0, 8 + o };
            sc.toText = [] (double v) { return juce::String::charToString (0xd7) + juce::String (v * 2.0 - 1.0, 2); };
            row.scale = std::make_unique<ctlui::Knob> (&engine, sc, [this, o] { return current().params[(std::size_t) (8 + o)] / 65536.0; },
                                                       [this, o] (double v) {
                                                           edit ([o, v] (voltage::Settings& s) {
                                                               s.params[(std::size_t) (8 + o)] = juce::jlimit (0, 65536, juce::roundToInt (v * 65536.0));
                                                           });
                                                       });
            row.scale->shortName = tr ("Scale");
            addAndMakeVisible (*row.scale);

            row.polarity.setClickingTogglesState (true);
            row.polarity.onClick = [this, o, &row] { edit ([o, uni = row.polarity.getToggleState()] (voltage::Settings& s) { s.unipolar[(std::size_t) o] = uni; }); };
            ui::setHint (row.polarity, tr ("Polarity of the offset: bipolar (−10…10 V) or unipolar (0…10 V)"));
            addAndMakeVisible (row.polarity);

            if (o >= 2 && o % 2 == 0)
            {
                auto& t = tracks[(std::size_t) (o / 2 - 1)];
                t.addItem (tr ("Off"), 1);
                for (int k = 1; k < 126; ++k)
                    t.addItem (tr ("Insert {0}", { juce::String (k) }), k + 1);
                t.onChange = [this, pair = o / 2 - 1, &t] { edit ([pair, k = t.getSelectedId() - 1] (voltage::Settings& s) { s.tracks[(std::size_t) pair] = juce::jmax (0, k); }); };
                ui::setHint (t, tr ("The mixer insert this pair of outputs is added to"));
                addAndMakeVisible (t);
            }
        }
        engine.addChangeListener (this);
        refresh();
        setSize (620, rowTop + voltage::outputs * rowHeight + 8);
    }

    ~VoltageEditor() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg2);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::title, true));
        g.drawText ("Voltage Controller", 14, 8, 240, 22, juce::Justification::centredLeft);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        g.drawText (tr ("Its notes as control voltages for modular gear (a DC-coupled audio interface)"), 230, 8, getWidth() - 244, 22,
                    juce::Justification::centredRight);
        // the pairs' bands
        for (int pair = 0; pair < 4; ++pair)
        {
            const auto band = juce::Rectangle<int> (10, rowTop + pair * 2 * rowHeight, getWidth() - 20, 2 * rowHeight - 4).toFloat();
            fillRoundRect (g, band, radius::md, pair % 2 == 0 ? colour::bg1 : colour::bg3);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::small, true));
            g.drawText (pair == 0 ? tr ("Main") : tr ("Out {0}", { juce::String (pair + 1) }), band.withWidth (60.0f).toNearestInt(),
                        juce::Justification::centred);
        }
        g.setFont (font (type::caption));
        for (int o = 0; o < voltage::outputs; ++o)
        {
            g.setColour (colour::textSecondary);
            g.drawText (o % 2 == 0 ? "L" : "R", 70, rowTop + o * rowHeight, 20, rowHeight - 4, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto top = getLocalBounds().reduced (14).withTrimmedTop (30).removeFromTop (60);
        enable.setBounds (top.removeFromLeft (60).withSizeKeepingCentre (60, 26));
        top.removeFromLeft (10);
        range.setBounds (top.removeFromLeft (100).withSizeKeepingCentre (100, 26));
        top.removeFromLeft (8);
        pitch.setBounds (top.removeFromLeft (100).withSizeKeepingCentre (100, 26));
        top.removeFromLeft (8);
        clock.setBounds (top.removeFromLeft (130).withSizeKeepingCentre (130, 26));
        top.removeFromLeft (8);
        pulse->setBounds (top.removeFromLeft (60));
        for (int o = 0; o < voltage::outputs; ++o)
        {
            auto r = juce::Rectangle<int> (96, rowTop + o * rowHeight, getWidth() - 106, rowHeight - 4);
            auto& row = rows[(std::size_t) o];
            row.source.setBounds (r.removeFromLeft (110).withSizeKeepingCentre (110, 22));
            r.removeFromLeft (10);
            row.offset->setBounds (r.removeFromLeft (70));
            row.scale->setBounds (r.removeFromLeft (70));
            r.removeFromLeft (10);
            row.polarity.setBounds (r.removeFromLeft (70).withSizeKeepingCentre (70, 22));
            r.removeFromLeft (10);
            if (o >= 2 && o % 2 == 0)
                tracks[(std::size_t) (o / 2 - 1)].setBounds (r.removeFromLeft (120).withSizeKeepingCentre (120, 22).translated (0, rowHeight / 2));
        }
    }

private:
    static constexpr int rowTop = 104, rowHeight = 64;
    static constexpr int clockChoices[] = { 2, 4, 8, 12, 16, 24, 32, 48, 64, 96 };

    struct Row
    {
        juce::ComboBox source;
        std::unique_ptr<ctlui::Knob> offset, scale;
        juce::TextButton polarity;
    };

    voltage::Settings current() const
    {
        if (const auto* c = locate (const_cast<Song&> (engine.getSong())))
            if (auto s = voltage::parse (c->pluginState))
                return *s;
        return {};
    }

    void edit (std::function<void (voltage::Settings&)> change)
    {
        engine.editSong ([this, change] (Song& song) {
            if (auto* c = locate (song))
            {
                auto s = voltage::parse (c->pluginState).value_or (voltage::Settings {});
                change (s);
                c->pluginState = voltage::write (s);
            }
        });
    }

    void refresh()
    {
        const auto s = current();
        enable.setToggleState (s.enabled, juce::dontSendNotification);
        range.setSelectedItemIndex (s.range, juce::dontSendNotification);
        pitch.setSelectedItemIndex (s.pitchStandard, juce::dontSendNotification);
        clock.setSelectedId (s.clockPpq, juce::dontSendNotification);
        if (clock.getSelectedId() == 0)
            clock.setText (tr ("Clock {0} PPQ", { juce::String (s.clockPpq) }), juce::dontSendNotification);
        for (int o = 0; o < voltage::outputs; ++o)
        {
            auto& row = rows[(std::size_t) o];
            row.source.setSelectedId ((int) s.source[(std::size_t) o] + 1, juce::dontSendNotification);
            row.polarity.setToggleState (s.unipolar[(std::size_t) o], juce::dontSendNotification);
            row.polarity.setButtonText (s.unipolar[(std::size_t) o] ? tr ("Unipolar") : tr ("Bipolar"));
            row.offset->repaint();
            row.scale->repaint();
        }
        for (int k = 0; k < 3; ++k)
            tracks[(std::size_t) k].setSelectedId (s.tracks[(std::size_t) k] + 1, juce::dontSendNotification);
        pulse->repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    std::function<ChannelDef* (Song&)> locate;
    juce::TextButton enable;
    juce::ComboBox range, pitch, clock;
    std::unique_ptr<ctlui::Knob> pulse;
    std::array<Row, voltage::outputs> rows;
    std::array<juce::ComboBox, 3> tracks;
};
} // namespace

std::unique_ptr<juce::Component> makeVoltageControllerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate)
{
    return std::make_unique<VoltageEditor> (engine, std::move (locate));
}

std::unique_ptr<juce::Component> makeVoltageControllerEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<VoltageEditor> (engine, [channel] (Song& s) -> ChannelDef* {
        return channel >= 0 && channel < (int) s.channels.size() && s.channels[(std::size_t) channel].instrument == InstrumentKind::voltage
                   ? &s.channels[(std::size_t) channel]
                   : nullptr;
    }, channel);
}
} // namespace lynx
