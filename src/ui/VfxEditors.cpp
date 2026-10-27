#include "ui/VfxEditors.h"

#include <algorithm>
#include <array>

#include "core/Vfx.h"
#include "engine/Text.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"
#include "ui/ValueControl.h"

namespace lynx
{
namespace
{
juce::String keyName (int key)
{
    return juce::MidiMessage::getMidiNoteName (std::clamp (key, 0, 127), true, true, 4);
}

juce::String pitchClassName (int pc)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((pc % 12) + 12) % 12];
}

// A small knob with LYNX's control contract (drag, Ctrl = fine, wheel, Alt / middle-click / double-click reset,
// right-click: type a value / reset)
class Knob final : public juce::Component
{
public:
    Knob (ui::ValueSpec s, std::function<double()> get, std::function<void (double)> set)
        : spec (std::move (s)), getter (std::move (get)), setter (std::move (set))
    {
        setRepaintsOnMouseActivity (true);
        ui::setHint (*this, spec.name, true, false);
    }

    void paint (juce::Graphics& g) override
    {
        const auto value = getter();
        const auto v = juce::jlimit (0.0, 1.0, (value - spec.minimum) / juce::jmax (1e-9, spec.maximum - spec.minimum));
        auto area = getLocalBounds().toFloat();
        const auto label = area.removeFromBottom (14.0f);
        const auto valueArea = area.removeFromBottom (13.0f);
        const auto size = juce::jmin (area.getWidth(), area.getHeight()) - 4.0f;
        const auto box = juce::Rectangle<float> (size, size).withCentre (area.getCentre());
        constexpr float sweep = juce::MathConstants<float>::pi * 1.5f;
        g.setColour (theme::colour::bg4);
        g.fillEllipse (box.reduced (4.0f));
        juce::Path track, arc;
        const auto radius = size * 0.5f - 1.5f;
        track.addCentredArc (box.getCentreX(), box.getCentreY(), radius, radius, 0.0f, -sweep * 0.5f, sweep * 0.5f, true);
        g.setColour (theme::colour::knobTrack);
        g.strokePath (track, juce::PathStrokeType (3.0f));
        // bipolar knobs light from the centre
        const auto bipolar = spec.minimum < 0.0 && spec.maximum > 0.0;
        const auto from = bipolar ? 0.5 : 0.0;
        arc.addCentredArc (box.getCentreX(), box.getCentreY(), radius, radius, 0.0f, -sweep * 0.5f + sweep * (float) std::min (from, v),
                           -sweep * 0.5f + sweep * (float) std::max (from, v), true);
        g.setColour (isMouseOverOrDragging() ? theme::colour::accent : theme::colour::knobArc);
        g.strokePath (arc, juce::PathStrokeType (3.0f));
        const auto angle = -sweep * 0.5f + sweep * (float) v;
        const auto tip = box.getCentre() + juce::Point<float> (std::sin (angle), -std::cos (angle)) * (radius - 5.0f);
        g.drawLine (box.getCentreX(), box.getCentreY(), tip.x, tip.y, 2.0f);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (spec.text (value), valueArea, juce::Justification::centred);
        g.setColour (theme::colour::textSecondary);
        g.drawText (caption, label, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, nullptr, spec, getter(), [this] (double v) { change (v); });
            return;
        }
        if (ui::isResetClick (e))
        {
            change (spec.defaultValue);
            return;
        }
        drag.begin (e, getter());
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag.isActive())
            change (drag.update (e, spec));
    }
    void mouseUp (const juce::MouseEvent&) override { drag.end(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { change (spec.defaultValue); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (const auto v = ui::wheelValue (getter(), e, w, spec))
            change (*v);
    }

    juce::String caption;

private:
    void change (double v)
    {
        v = juce::jlimit (spec.minimum, spec.maximum, v);
        if (spec.interval > 0.0)
            v = spec.minimum + std::round ((v - spec.minimum) / spec.interval) * spec.interval;
        setter (v);
        repaint();
    }

    ui::ValueSpec spec;
    std::function<double()> getter;
    std::function<void (double)> setter;
    ui::ValueDrag drag;
};

ui::ValueSpec intSpec (const juce::String& name, int lo, int hi, int def, std::function<juce::String (double)> text = nullptr)
{
    ui::ValueSpec s;
    s.name = name;
    s.minimum = lo;
    s.maximum = hi;
    s.defaultValue = def;
    s.interval = 1.0;
    s.pixelsForRange = std::max (120.0, (double) (hi - lo) * 6.0);
    if (text)
    {
        s.toText = text;
        s.fromText = ui::inverseOf (text, lo, hi);
    }
    else
        s.toText = [] (double v) { return juce::String ((int) std::lround (v)); };
    return s;
}

// What every body gets: the plugin's FL state and a way to change it
class Body : public juce::Component
{
public:
    using Edit = std::function<void (std::vector<std::uint8_t>&)>;
    std::function<void (Edit)> edit;
    virtual void refresh (const std::vector<std::uint8_t>& fl) = 0;
    virtual int preferredHeight() const { return 300; }
    virtual int preferredWidth() const { return 700; }
};

// ---------------------------------------------------------------------------------------------------------------
// VFX Key Mapper: inputs (one octave) down, outputs across; the controls above

class KeyMapperBody final : public Body
{
public:
    static constexpr int lowest = -12, columns = 36; // outputs: an octave below the row's octave to an octave above (FL's range)

    KeyMapperBody()
    {
        enable.setButtonText (tr ("Enable"));
        enable.onClick = [this] { change ([on = enable.getToggleState()] (vfx::KeyMapperSettings& s) { s.enabled = on; }); };
        addAndMakeVisible (enable);
        for (const auto& [id, text] : std::vector<std::pair<int, juce::String>> {
                 { 1, tr ("All") }, { 2, tr ("All (gain-compensated)") }, { 3, tr ("Random") }, { 4, tr ("Cycle up") }, { 5, tr ("Cycle down") } })
            multimode.addItem (text, id);
        multimode.onChange = [this] { change ([m = multimode.getSelectedId() - 1] (vfx::KeyMapperSettings& s) { s.multimode = std::max (0, m); }); };
        ui::setHint (multimode, tr ("Multimode: how an input mapped to several notes plays them"));
        addAndMakeVisible (multimode);
        multimodeLabel.setText (tr ("Multimode"), juce::dontSendNotification);
        multimodeLabel.setFont (theme::font (theme::type::small));
        addAndMakeVisible (multimodeLabel);

        offset.caption = tr ("Offset");
        baseKey.caption = tr ("Base key");
        slide.caption = tr ("Pitch slide");
        addAndMakeVisible (offset);
        addAndMakeVisible (baseKey);
        addAndMakeVisible (slide);
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        settings = vfx::parseKeyMapper (fl).value_or (vfx::defaultKeyMapper());
        enable.setToggleState (settings.enabled, juce::dontSendNotification);
        multimode.setSelectedId (std::clamp (settings.multimode, 0, 4) + 1, juce::dontSendNotification);
        repaint();
        offset.repaint();
        baseKey.repaint();
        slide.repaint();
    }

    int preferredHeight() const override { return 64 + 12 * 22 + 30; }
    int preferredWidth() const override { return 64 + columns * 15 + 16; }

    void resized() override
    {
        auto top = getLocalBounds().reduced (8, 4).removeFromTop (60);
        enable.setBounds (top.removeFromLeft (90).withSizeKeepingCentre (90, 24));
        top.removeFromLeft (8);
        offset.setBounds (top.removeFromLeft (64));
        baseKey.setBounds (top.removeFromLeft (64));
        slide.setBounds (top.removeFromLeft (64));
        top.removeFromLeft (16);
        auto mm = top.removeFromLeft (200);
        multimodeLabel.setBounds (mm.removeFromTop (22));
        multimode.setBounds (mm.removeFromTop (24));
    }

    void paint (juce::Graphics& g) override
    {
        const auto grid = gridArea();
        g.setColour (theme::colour::bg0);
        g.fillRect (grid);
        const auto cw = grid.getWidth() / (float) columns, rh = grid.getHeight() / 12.0f;
        // output columns: the octaves and the black keys, labelled every C
        for (int c = 0; c < columns; ++c)
        {
            const auto value = lowest + c;
            const auto pc = ((value % 12) + 12) % 12;
            const auto black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            const juce::Rectangle<float> col (grid.getX() + (float) c * cw, grid.getY(), cw, grid.getHeight());
            if (value >= 0 && value < 12) // the row's own octave, brighter (as FL draws it)
            {
                g.setColour (theme::colour::bg2);
                g.fillRect (col);
            }
            if (black)
            {
                g.setColour (theme::colour::bg0.withAlpha (0.55f));
                g.fillRect (col);
            }
            g.setColour (pc == 0 ? theme::colour::borderStrong : theme::colour::border);
            g.drawVerticalLine ((int) col.getX(), grid.getY(), grid.getBottom());
            if (pc == 0)
            {
                g.setColour (theme::colour::textSecondary);
                g.setFont (theme::font (theme::type::caption));
                const auto octave = value / 12;
                const auto name = pitchClassName (settings.baseKey);
                g.drawText (octave == 0 ? name : (octave > 0 ? name + " +" + juce::String (octave) : name + " " + juce::String (octave)),
                            juce::Rectangle<float> (col.getX() + 2.0f, grid.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
            }
        }
        // input rows (top = B, bottom = C, as a keyboard on its side)
        for (int r = 0; r < 12; ++r)
        {
            const auto pc = 11 - r;
            const juce::Rectangle<float> row (grid.getX(), grid.getY() + (float) r * rh, grid.getWidth(), rh);
            g.setColour (theme::colour::border);
            g.drawHorizontalLine ((int) row.getY(), grid.getX(), grid.getRight());
            const auto black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            const juce::Rectangle<float> label (4.0f, row.getY(), grid.getX() - 8.0f, rh);
            g.setColour (black ? theme::colour::bg4 : theme::colour::bg6);
            g.fillRoundedRectangle (label.reduced (1.0f, 1.5f), 2.0f);
            g.setColour (theme::colour::textPrimary);
            g.setFont (theme::font (theme::type::small));
            g.drawText (pitchClassName (pc + settings.baseKey), label.reduced (6.0f, 0.0f), juce::Justification::centredLeft);
            // the unchanged note (a faint diagonal)
            g.setColour (theme::colour::bg5.withAlpha (0.55f));
            g.fillRect (cell (pc, pc).reduced (cw * 0.35f, rh * 0.35f));
            for (const auto value : settings.rows[(std::size_t) pc])
            {
                const auto c = cell (pc, value);
                if (c.isEmpty())
                    continue;
                g.setColour (settings.enabled ? theme::colour::accent : theme::colour::textDisabled);
                g.fillRoundedRectangle (c.reduced (1.5f), 2.5f);
            }
        }
        g.setColour (theme::colour::borderStrong);
        g.drawRect (grid);
        if (hover.has_value())
        {
            g.setColour (theme::colour::textPrimary.withAlpha (0.5f));
            g.drawRect (cell (hover->first, hover->second), 1.0f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto now = cellAt (e.position);
        if (now != hover)
        {
            hover = now;
            if (hover)
                ui::setHint (*this, tr ("{0} plays {1} - left-click: add / remove, right-click: only this note, middle-click or Alt+click: reset the row",
                                        { pitchClassName (hover->first + settings.baseKey), outputName (hover->first, hover->second) }));
            repaint();
        }
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hover.reset();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (const auto grid = gridArea(); e.position.x < grid.getX() && e.position.y >= grid.getY() && e.position.y < grid.getBottom())
        {
            // a click on a row's label: back to the note itself
            const auto pc = 11 - std::clamp ((int) ((e.position.y - grid.getY()) / (grid.getHeight() / 12.0f)), 0, 11);
            change ([pc] (vfx::KeyMapperSettings& s) { s.rows[(std::size_t) pc] = { pc }; });
            return;
        }
        const auto at = cellAt (e.position);
        if (! at)
            return;
        const auto [pc, value] = *at;
        if (e.mods.isMiddleButtonDown() || e.mods.isAltDown())
            change ([pc] (vfx::KeyMapperSettings& s) { s.rows[(std::size_t) pc] = { pc }; });
        else if (e.mods.isPopupMenu())
            change ([pc, value] (vfx::KeyMapperSettings& s) { s.rows[(std::size_t) pc] = { value }; });
        else
            change ([pc, value] (vfx::KeyMapperSettings& s) {
                auto& row = s.rows[(std::size_t) pc];
                if (auto it = std::find (row.begin(), row.end(), value); it != row.end())
                    row.erase (it);
                else
                    row.push_back (value); // (FL keeps the order: Cycle and Random follow it)
            });
    }

private:
    juce::Rectangle<float> gridArea() const
    {
        return getLocalBounds().toFloat().withTrimmedTop (68.0f).withTrimmedLeft (56.0f).withTrimmedRight (8.0f).withTrimmedBottom (20.0f);
    }
    juce::Rectangle<float> cell (int pc, int value) const
    {
        const auto grid = gridArea();
        const auto c = value - lowest;
        if (c < 0 || c >= columns)
            return {};
        const auto cw = grid.getWidth() / (float) columns, rh = grid.getHeight() / 12.0f;
        return { grid.getX() + (float) c * cw, grid.getY() + (float) (11 - pc) * rh, cw, rh };
    }
    std::optional<std::pair<int, int>> cellAt (juce::Point<float> p) const
    {
        const auto grid = gridArea();
        if (! grid.contains (p))
            return std::nullopt;
        const auto c = (int) ((p.x - grid.getX()) / (grid.getWidth() / (float) columns));
        const auto r = (int) ((p.y - grid.getY()) / (grid.getHeight() / 12.0f));
        return std::make_pair (11 - std::clamp (r, 0, 11), lowest + std::clamp (c, 0, columns - 1));
    }
    juce::String outputName (int pc, int value) const
    {
        const auto shift = value - pc;
        return pitchClassName (value + settings.baseKey) + " (" + (shift > 0 ? "+" : "") + juce::String (shift) + ")";
    }

    void change (std::function<void (vfx::KeyMapperSettings&)> f)
    {
        if (edit)
            edit ([f] (std::vector<std::uint8_t>& fl) {
                auto s = vfx::parseKeyMapper (fl).value_or (vfx::defaultKeyMapper());
                f (s);
                fl = vfx::writeKeyMapper (s);
            });
    }

    vfx::KeyMapperSettings settings = vfx::defaultKeyMapper();
    juce::ToggleButton enable;
    juce::ComboBox multimode;
    juce::Label multimodeLabel;
    Knob offset { intSpec (tr ("Offset (transpose)"), -12, 12, 0, [] (double v) { return (v > 0 ? "+" : "") + juce::String ((int) std::lround (v)); }),
                  [this] { return (double) settings.offset; },
                  [this] (double v) { change ([v] (vfx::KeyMapperSettings& s) { s.offset = (int) std::lround (v); }); } };
    Knob baseKey { intSpec (tr ("Base key: the note the rows and their octaves start on"), 0, 11, 0, [] (double v) { return pitchClassName ((int) std::lround (v)); }),
                   [this] { return (double) settings.baseKey; },
                   [this] (double v) { change ([v] (vfx::KeyMapperSettings& s) { s.baseKey = (int) std::lround (v); }); } };
    Knob slide { intSpec (tr ("Pitch slide: the new notes start at the played pitch and glide to their own (in steps)"), 0, 192, 0,
                          [] (double v) { const auto u = (int) std::lround (v); return juce::String (u / 48) + ":" + juce::String (u % 48).paddedLeft ('0', 2); }),
                 [this] { return (double) settings.slide; },
                 [this] (double v) { change ([v] (vfx::KeyMapperSettings& s) { s.slide = (int) std::lround (v); }); } };
    std::optional<std::pair<int, int>> hover;
};

// ---------------------------------------------------------------------------------------------------------------
// VFX Color Mapper: per note colour, the voice output it goes to and the colour it leaves with

class ColorMapperBody final : public Body
{
public:
    ColorMapperBody()
    {
        for (int c = 0; c < 16; ++c)
        {
            auto& port = ports[(std::size_t) c];
            auto& colour = colours[(std::size_t) c];
            for (int k = 0; k < 16; ++k)
            {
                port.addItem (tr ("Voice {0}", { juce::String (k + 1) }), k + 1);
                colour.addItem (tr ("Colour {0}", { juce::String (k + 1) }), k + 1);
            }
            port.onChange = [this, c] { change ([c, v = ports[(std::size_t) c].getSelectedId() - 1] (vfx::ColorMapperSettings& s) { s.port[(std::size_t) c] = std::max (0, v); }); };
            colour.onChange = [this, c] { change ([c, v = colours[(std::size_t) c].getSelectedId() - 1] (vfx::ColorMapperSettings& s) { s.colour[(std::size_t) c] = std::max (0, v); }); };
            ui::setHint (port, tr ("The voice output the notes of colour {0} go to", { juce::String (c + 1) }));
            ui::setHint (colour, tr ("The colour the notes of colour {0} leave with", { juce::String (c + 1) }));
            addAndMakeVisible (port);
            addAndMakeVisible (colour);
        }
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        settings = vfx::parseColorMapper (fl).value_or (vfx::defaultColorMapper());
        for (int c = 0; c < 16; ++c)
        {
            ports[(std::size_t) c].setSelectedId (settings.port[(std::size_t) c] + 1, juce::dontSendNotification);
            colours[(std::size_t) c].setSelectedId (settings.colour[(std::size_t) c] + 1, juce::dontSendNotification);
        }
        repaint();
    }

    int preferredHeight() const override { return 28 + 8 * 28 + 8; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10, 4).withTrimmedTop (24);
        const auto half = area.getWidth() / 2;
        for (int c = 0; c < 16; ++c)
        {
            auto column = c < 8 ? area.withWidth (half - 10) : area.withTrimmedLeft (half + 10);
            auto row = column.withHeight (26).translated (0, (c % 8) * 28);
            row.removeFromLeft (70);
            ports[(std::size_t) c].setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
            row.removeFromLeft (8);
            colours[(std::size_t) c].setBounds (row);
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().reduced (10, 4);
        const auto half = area.getWidth() / 2;
        g.setFont (theme::font (theme::type::caption));
        g.setColour (theme::colour::textSecondary);
        for (const auto x : { area.getX(), area.getX() + half + 10 })
        {
            const auto w = half - 10 - 70;
            g.drawText (theme::caps (tr ("Note colour")), x, area.getY(), 70, 18, juce::Justification::centredLeft);
            g.drawText (theme::caps (tr ("Voice output")), x + 70, area.getY(), (w - 8) / 2, 18, juce::Justification::centredLeft);
            g.drawText (theme::caps (tr ("Colour")), x + 70 + (w - 8) / 2 + 8, area.getY(), (w - 8) / 2, 18, juce::Justification::centredLeft);
        }
        area.removeFromTop (24);
        for (int c = 0; c < 16; ++c)
        {
            auto column = c < 8 ? area.withWidth (half - 10) : area.withTrimmedLeft (half + 10);
            auto row = column.withHeight (26).translated (0, (c % 8) * 28).removeFromLeft (64).toFloat();
            g.setColour (theme::noteGroupHue (c));
            g.fillRoundedRectangle (row.removeFromLeft (26.0f).reduced (3.0f), 3.0f);
            g.setColour (theme::colour::textPrimary);
            g.setFont (theme::font (theme::type::small));
            g.drawText (juce::String (c + 1), row.reduced (4.0f, 0.0f), juce::Justification::centredLeft);
        }
    }

private:
    void change (std::function<void (vfx::ColorMapperSettings&)> f)
    {
        if (edit)
            edit ([f] (std::vector<std::uint8_t>& fl) {
                auto s = vfx::parseColorMapper (fl).value_or (vfx::defaultColorMapper());
                f (s);
                fl = vfx::writeColorMapper (s);
            });
    }

    vfx::ColorMapperSettings settings = vfx::defaultColorMapper();
    std::array<juce::ComboBox, 16> ports, colours;
};

// A kind LYNX does not edit yet: says so (its notes pass through)
class PendingBody final : public Body
{
public:
    void refresh (const std::vector<std::uint8_t>&) override {}
    int preferredHeight() const override { return 80; }
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::body));
        g.drawFittedText (tr ("LYNX keeps this plugin's settings (from FL Studio) and passes its notes through unchanged."),
                          getLocalBounds().reduced (14, 8), juce::Justification::topLeft, 3);
    }
};

std::unique_ptr<Body> makeBody (vfx::Kind kind)
{
    switch (kind)
    {
        case vfx::Kind::keyMapper: return std::make_unique<KeyMapperBody>();
        case vfx::Kind::colorMapper: return std::make_unique<ColorMapperBody>();
        default: break;
    }
    return std::make_unique<PendingBody>();
}

juce::String description (vfx::Kind kind)
{
    switch (kind)
    {
        case vfx::Kind::keyMapper: return tr ("Maps each played note to other notes: transpose, scales, chords");
        case vfx::Kind::colorMapper: return tr ("Sends each note colour to its own voice output");
        case vfx::Kind::envelope: return tr ("Envelopes and LFOs on the notes' velocity, pitch, pan and Mod X / Y");
        case vfx::Kind::keyboardSplitter: return tr ("Splits the keyboard and the velocity into zones, one output each");
        case vfx::Kind::levelScaler: return tr ("Reshapes the notes' velocity, release, fine pitch, pan and Mod X / Y");
        case vfx::Kind::sequencer: return tr ("Plays step patterns and arpeggios from the held notes");
        case vfx::Kind::script: return tr ("FL's Python note scripts");
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------

class VfxEditor final : public juce::Component, private juce::ChangeListener
{
public:
    VfxEditor (AudioEngine& e, std::function<ChannelDef* (Song&)> l, int c) : engine (e), locate (std::move (l)), channel (c)
    {
        const auto s = state();
        kind = s ? s->kind : vfx::Kind::keyMapper;
        body = makeBody (kind);
        body->edit = [this] (Body::Edit change) { editState ([change] (vfx::State& st) { change (st.fl); }); };
        addAndMakeVisible (*body);

        presetButton.onClick = [this] { showPresetMenu(); };
        ui::setHint (presetButton, tr ("Presets: LYNX's own and, with FL Studio installed, FL's factory presets"));
        addAndMakeVisible (presetButton);
        for (auto* b : { &prev, &next })
        {
            addAndMakeVisible (b);
            b->setWantsKeyboardFocus (false);
        }
        prev.onClick = [this] { stepPreset (-1); };
        next.onClick = [this] { stepPreset (1); };

        // on a Channel Rack channel: the MIDI port its outputs go out on
        port.setSliderStyle (juce::Slider::IncDecButtons);
        port.setRange (0, 255, 1);
        port.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 48, 22);
        port.onValueChange = [this] { editState ([v = (int) port.getValue()] (vfx::State& st) { st.port = v; }); };
        ui::setHint (port, tr ("Output port: output 1 goes out on this MIDI port, output 2 on the next one…; the plugins and Patchers "
                               "that take that port play it"));
        portLabel.setText (tr ("Output port"), juce::dontSendNotification);
        portLabel.setFont (theme::font (theme::type::small));
        connect.setButtonText (tr ("Send to a plugin..."));
        connect.onClick = [this] { showConnectMenu(); };
        ui::setHint (connect, tr ("Sets a plugin channel's MIDI input port to this port, so it plays what this channel sends"));
        for (juce::Component* comp : { (juce::Component*) &port, (juce::Component*) &portLabel, (juce::Component*) &connect })
        {
            addAndMakeVisible (comp);
            comp->setVisible (channel >= 0);
        }

        engine.addChangeListener (this);
        refresh();
        setSize (std::max (620, body->preferredWidth()), 44 + (channel >= 0 ? 34 : 0) + body->preferredHeight());
    }

    ~VfxEditor() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        auto top = getLocalBounds().removeFromTop (40).reduced (14, 0);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::title, true));
        g.drawText (vfx::flName (kind), top.removeFromLeft (220), juce::Justification::centredLeft);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (description (kind), top.withTrimmedRight (300), juce::Justification::centredLeft);
        g.setColour (theme::colour::border);
        g.drawHorizontalLine (40, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (40).reduced (10, 7);
        next.setBounds (top.removeFromRight (26));
        presetButton.setBounds (top.removeFromRight (210));
        prev.setBounds (top.removeFromRight (26));
        area.removeFromTop (4);
        if (channel >= 0)
        {
            auto row = area.removeFromTop (30).reduced (14, 2);
            portLabel.setBounds (row.removeFromLeft (90));
            port.setBounds (row.removeFromLeft (130));
            row.removeFromLeft (10);
            connect.setBounds (row.removeFromLeft (170));
        }
        body->setBounds (area);
    }

private:
    std::optional<vfx::State> state() const
    {
        const auto* c = locate (const_cast<Song&> (engine.getSong()));
        return c != nullptr ? vfx::readState (c->pluginState) : std::nullopt;
    }

    void editState (std::function<void (vfx::State&)> change)
    {
        engine.editSong ([this, change] (Song& song) {
            auto* c = locate (song);
            if (c == nullptr || c->instrument != InstrumentKind::vfx)
                return;
            auto s = vfx::readState (c->pluginState);
            if (! s)
                return;
            change (*s);
            c->pluginState = vfx::writeState (*s);
        });
    }

    struct PresetEntry
    {
        juce::String folder, name;
        std::vector<std::uint8_t> fl; // LYNX's own
        juce::File file;              // FL's
    };

    std::vector<PresetEntry> presets() const
    {
        std::vector<PresetEntry> out;
        for (auto& p : vfx::builtInPresets (kind))
            out.push_back ({ juce::String::fromUTF8 (p.folder.c_str()), juce::String::fromUTF8 (p.name.c_str()), std::move (p.fl), {} });
        const auto roots = findFlStudioRoots();
        if (! roots.data.empty())
        {
            const auto base = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin presets/Effects").getChildFile (vfx::flName (kind));
            auto files = base.findChildFiles (juce::File::findFiles, true, "*.fst");
            std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName().compareNatural (b.getFullPathName()) < 0; });
            for (const auto& f : files)
            {
                const auto folder = f.getParentDirectory() == base ? juce::String() : f.getParentDirectory().getFileName();
                out.push_back ({ "FL Studio" + (folder.isEmpty() ? juce::String() : " / " + folder), f.getFileNameWithoutExtension(), {}, f });
            }
        }
        return out;
    }

    void apply (const PresetEntry& p)
    {
        auto fl = p.fl;
        if (p.file != juce::File())
        {
            juce::MemoryBlock data;
            if (! p.file.loadFileAsData (data))
                return;
            const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
            const auto state = vfx::stateFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize()), kind);
            if (! state)
                return;
            fl = *state;
        }
        presetName = p.name;
        editState ([fl] (vfx::State& st) { st.fl = fl; });
        presetButton.setButtonText (presetName);
    }

    void showPresetMenu()
    {
        const auto all = presets();
        juce::PopupMenu menu;
        menu.addItem (tr ("Default"), [this] {
            presetName = tr ("Default");
            editState ([k = kind] (vfx::State& st) { st.fl = vfx::defaultState (k); });
            presetButton.setButtonText (presetName);
        });
        std::map<juce::String, juce::PopupMenu> folders;
        std::vector<juce::String> order;
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            const auto folder = all[i].folder.isEmpty() ? tr ("Presets") : all[i].folder;
            if (! folders.count (folder))
                order.push_back (folder);
            folders[folder].addItem (all[i].name, [this, entry = all[i]] { apply (entry); });
        }
        for (const auto& f : order)
            menu.addSubMenu (f, folders[f]);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton));
    }

    void stepPreset (int delta)
    {
        const auto all = presets();
        if (all.empty())
            return;
        int at = -1;
        for (int i = 0; i < (int) all.size(); ++i)
            if (all[(std::size_t) i].name == presetName)
                at = i;
        at = at < 0 ? (delta > 0 ? 0 : (int) all.size() - 1) : (at + delta + (int) all.size()) % (int) all.size();
        apply (all[(std::size_t) at]);
    }

    void showConnectMenu()
    {
        juce::PopupMenu menu;
        const auto& song = engine.getSong();
        const auto s = state();
        const auto p = s ? s->port : 0;
        for (int c = 0; c < (int) song.channels.size(); ++c)
            if (song.channels[(std::size_t) c].instrument == InstrumentKind::plugin)
                menu.addItem (juce::String::fromUTF8 (song.channels[(std::size_t) c].name.c_str()), true,
                              song.channels[(std::size_t) c].plugin.midiInPort == p, [this, c, p] {
                                  engine.editSong ([c, p] (Song& song) {
                                      auto& in = song.channels[(std::size_t) c].plugin.midiInPort;
                                      in = in == p ? -1 : p;
                                  });
                              });
        if (menu.getNumItems() == 0)
            menu.addItem (tr ("(no plugin channel in the Channel Rack)"), false, false, [] {});
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&connect));
    }

    void refresh()
    {
        const auto s = state();
        if (! s)
            return;
        body->refresh (s->fl);
        port.setValue (s->port, juce::dontSendNotification);
        if (presetName.isEmpty())
            presetButton.setButtonText (tr ("Presets"));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    std::function<ChannelDef* (Song&)> locate;
    int channel;
    vfx::Kind kind;
    std::unique_ptr<Body> body;
    juce::TextButton presetButton, prev { juce::String::fromUTF8 ("\xe2\x97\x80") }, next { juce::String::fromUTF8 ("\xe2\x96\xb6") };
    juce::String presetName;
    juce::Slider port;
    juce::Label portLabel;
    juce::TextButton connect;
};
} // namespace

std::unique_ptr<juce::Component> makeVfxEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, int channel)
{
    return std::make_unique<VfxEditor> (engine, std::move (locate), channel);
}

std::unique_ptr<juce::Component> makeVfxEditor (AudioEngine& engine, int channel)
{
    return makeVfxEditor (
        engine,
        [channel] (Song& song) -> ChannelDef* {
            return channel >= 0 && channel < (int) song.channels.size() ? &song.channels[(std::size_t) channel] : nullptr;
        },
        channel);
}
} // namespace lynx
