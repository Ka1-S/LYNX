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
        if (box) // (a value box: the caption above, the value in a field; dragged like the knob)
        {
            auto area = getLocalBounds().toFloat();
            g.setColour (theme::colour::textSecondary);
            g.setFont (theme::font (theme::type::caption));
            g.drawText (caption, area.removeFromTop (15.0f), juce::Justification::centredLeft);
            g.setColour (isMouseOverOrDragging() ? theme::colour::bg5 : theme::colour::bg4);
            g.fillRoundedRectangle (area, 3.0f);
            g.setColour (theme::colour::textPrimary);
            g.setFont (theme::font (theme::type::small));
            g.drawText (spec.text (value), area, juce::Justification::centred);
            return;
        }
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
    bool box = false;

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
    s.pixelsForRange = std::clamp ((double) (hi - lo) * 6.0, 120.0, 300.0);
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

// ---------------------------------------------------------------------------------------------------------------
// VFX Level Scaler: a tab per voice property, its curve's controls on the left, the curve on the right

class LevelScalerBody final : public Body
{
public:
    using L = vfx::LevelScalerSettings;

    LevelScalerBody()
    {
        static const char* names[] = { TR_NOOP ("Velocity"), TR_NOOP ("Release"), TR_NOOP ("Fine pitch"), TR_NOOP ("Pan"), TR_NOOP ("Mod X"), TR_NOOP ("Mod Y") };
        for (int t = 0; t < L::tabCount; ++t)
        {
            auto& b = tabs[(std::size_t) t];
            b.setButtonText (tr (names[t]));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b.onClick = [this, t] { change ([t] (L& st) { st.selectedTab = t; }); };
            addAndMakeVisible (b);
        }
        auto knob = [this] (Knob& k, const juce::String& caption) {
            k.caption = caption;
            addAndMakeVisible (k);
        };
        knob (center, tr ("Center"));
        knob (multiply, tr ("Multiply"));
        knob (offset, tr ("Offset"));
        knob (tension, tr ("Tension"));
        knob (humanize, tr ("Humanize"));
        mode.addItem (tr ("Subtract"), 1);
        mode.addItem (tr ("Spread"), 2);
        mode.addItem (tr ("Add"), 3);
        mode.onChange = [this] { set (L::humanizeMode, mode.getSelectedId() - 1); };
        ui::setHint (mode, tr ("Humanize mode: the random part goes below the value, around it or above it"));
        addAndMakeVisible (mode);
        reset.setButtonText (tr ("Reset"));
        ui::setHint (reset, tr ("Reset all controls of this property"));
        reset.onClick = [this] { change ([] (L& st) { vfx::resetLevelScalerTab (st, st.selectedTab); }); };
        addAndMakeVisible (reset);
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        settings = vfx::parseLevelScaler (fl).value_or (vfx::defaultLevelScaler());
        for (int t = 0; t < L::tabCount; ++t)
            tabs[(std::size_t) t].setToggleState (t == settings.selectedTab, juce::dontSendNotification);
        mode.setSelectedId (std::clamp (settings.at (settings.selectedTab, L::humanizeMode), 0, 2) + 1, juce::dontSendNotification);
        for (auto* k : { &center, &multiply, &offset, &tension, &humanize })
            k->repaint();
        repaint();
    }

    int preferredHeight() const override { return 360; }
    int preferredWidth() const override { return 700; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto top = area.removeFromTop (28);
        for (auto& b : tabs)
            b.setBounds (top.removeFromLeft (92).reduced (2, 0));
        area.removeFromTop (8);
        auto left = area.removeFromLeft (230);
        auto row1 = left.removeFromTop (92);
        for (auto* k : { &center, &multiply, &offset })
            k->setBounds (row1.removeFromLeft (76));
        auto row2 = left.removeFromTop (92);
        for (auto* k : { &tension, &humanize })
            k->setBounds (row2.removeFromLeft (76));
        auto side = row2.reduced (2, 14);
        mode.setBounds (side.removeFromTop (26));
        left.removeFromTop (8);
        reset.setBounds (left.removeFromTop (26).withWidth (100));
        graph = area.reduced (10, 0).toFloat();
    }

    void paint (juce::Graphics& g) override
    {
        if (graph.isEmpty())
            return;
        const auto tab = settings.selectedTab;
        const auto bipolar = tab >= L::finePitch;
        const auto lo = bipolar ? -1.0 : 0.0, hi = 1.0;
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (graph, 4.0f);
        g.setColour (theme::colour::border);
        for (int k = 1; k < 8; ++k)
        {
            g.drawVerticalLine ((int) (graph.getX() + graph.getWidth() * (float) k / 8.0f), graph.getY(), graph.getBottom());
            g.drawHorizontalLine ((int) (graph.getY() + graph.getHeight() * (float) k / 8.0f), graph.getX(), graph.getRight());
        }
        g.setColour (theme::colour::textDisabled);
        g.setFont (theme::font (theme::type::title, true));
        g.drawText (tabs[(std::size_t) tab].getButtonText(), graph.reduced (12.0f, 8.0f), juce::Justification::topLeft);
        auto point = [&] (double u, double y) {
            return juce::Point<float> (graph.getX() + (float) u * graph.getWidth(), graph.getBottom() - (float) ((y - lo) / (hi - lo)) * graph.getHeight());
        };
        g.setColour (theme::colour::borderStrong);
        g.drawLine (juce::Line<float> (point (0.0, lo), point (1.0, hi)), 1.0f);
        juce::Path curve, upper, lower;
        constexpr int steps = 160;
        for (int k = 0; k <= steps; ++k)
        {
            const auto u = (double) k / steps;
            const auto x = lo + u * (hi - lo);
            const auto p = point (u, vfx::levelScale (settings, tab, x, lo, hi, 0.5));
            const auto a = point (u, vfx::levelScale (settings, tab, x, lo, hi, 0.0));
            const auto b = point (u, vfx::levelScale (settings, tab, x, lo, hi, 1.0));
            if (k == 0)
            {
                curve.startNewSubPath (p);
                lower.startNewSubPath (a);
                upper.startNewSubPath (b);
            }
            else
            {
                curve.lineTo (p);
                lower.lineTo (a);
                upper.lineTo (b);
            }
        }
        if (settings.at (tab, L::humanize) > 0)
        {
            juce::Path band (upper);
            for (int k = steps; k >= 0; --k)
            {
                const auto u = (double) k / steps;
                band.lineTo (point (u, vfx::levelScale (settings, tab, lo + u * (hi - lo), lo, hi, 0.0)));
            }
            band.closeSubPath();
            g.setColour (theme::colour::accent.withAlpha (0.16f));
            g.fillPath (band);
            g.setColour (theme::colour::accent.withAlpha (0.45f));
            g.strokePath (upper, juce::PathStrokeType (1.0f));
            g.strokePath (lower, juce::PathStrokeType (1.0f));
        }
        g.setColour (theme::colour::accent);
        g.strokePath (curve, juce::PathStrokeType (2.2f));
        g.setColour (theme::colour::borderStrong);
        g.drawRoundedRectangle (graph, 4.0f, 1.0f);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (tr ("in"), graph.withTrimmedTop (graph.getHeight() - 16.0f).reduced (6.0f, 0.0f), juce::Justification::centredRight);
        g.drawText (tr ("out"), graph.reduced (6.0f, 4.0f).withHeight (14.0f), juce::Justification::centredRight);
    }

private:
    void change (std::function<void (L&)> f)
    {
        if (edit)
            edit ([f] (std::vector<std::uint8_t>& fl) {
                auto s = vfx::parseLevelScaler (fl).value_or (vfx::defaultLevelScaler());
                f (s);
                fl = vfx::writeLevelScaler (s);
            });
    }
    void set (int field, double v)
    {
        change ([field, v] (L& st) { st.at (st.selectedTab, field) = (int) std::lround (v); });
    }
    double get (int field) const { return settings.at (settings.selectedTab, field); }
    static juce::String percent (double v) { return juce::String (v * 100.0, 0) + "%"; }

    L settings = vfx::defaultLevelScaler();
    std::array<juce::TextButton, L::tabCount> tabs;
    Knob center { intSpec (tr ("Center: the input level the curve turns around"), 0, 8192, 0, [] (double v) { return percent (v / 8192.0); }),
                  [this] { return get (L::center); }, [this] (double v) { set (L::center, v); } };
    Knob multiply { intSpec (tr ("Multiply: the slope around the center (negative inverts)"), -16384, 16384, 8192,
                             [] (double v) { return juce::String::charToString (0xd7) + juce::String (v / 8192.0, 2); }),
                    [this] { return get (L::multiply); }, [this] (double v) { set (L::multiply, v); } };
    Knob offset { intSpec (tr ("Offset: moves every value up or down"), -8192, 8192, 0,
                           [] (double v) { return (v > 0 ? "+" : "") + percent (v / 8192.0); }),
                  [this] { return get (L::offset); }, [this] (double v) { set (L::offset, v); } };
    Knob tension { intSpec (tr ("Tension: bends the curve (positive rises fast, negative starts slow)"), -8192, 8192, 0,
                            [] (double v) { return (v > 0 ? "+" : "") + percent (v / 8192.0); }),
                   [this] { return get (L::tension); }, [this] (double v) { set (L::tension, v); } };
    Knob humanize { intSpec (tr ("Humanize: a random amount for each note"), 0, 16384, 0, [] (double v) { return percent (v / 16384.0); }),
                    [this] { return get (L::humanize); }, [this] (double v) { set (L::humanize, v); } };
    juce::ComboBox mode;
    juce::TextButton reset;
    juce::Rectangle<float> graph;
};

// ---------------------------------------------------------------------------------------------------------------
// An FL mapping / envelope curve, edited with the mouse: drag a point, double-click to add one, right-click a point to
// remove it, drag a segment up or down to bend it; Alt+click a segment straightens it

class CurveEditor final : public juce::Component
{
public:
    std::function<void (const vfx::MapCurve&)> onChange;
    std::function<void (juce::Graphics&, juce::Rectangle<float>, double xMax)> drawGrid; // under the curves
    std::vector<std::pair<vfx::MapCurve, juce::Colour>> behind;                          // other curves, faint
    double fixedXMax = 0.0;  // > 0: the x range (maps: 1); else it follows the curve (envelopes)
    bool bipolar = false;    // a centre line
    int sustainPoint = -1, loopPoint = -1;
    juce::Colour colour = theme::colour::accent;
    float marker = -1.0f;    // a vertical line (x), e.g. the last note

    void setCurve (const vfx::MapCurve& c)
    {
        if (dragging < 0 && bending < 0)
            curve = c;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = plot();
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (area, 4.0f);
        const auto xMax = range();
        if (drawGrid)
            drawGrid (g, area, xMax);
        if (bipolar)
        {
            g.setColour (theme::colour::borderStrong);
            g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());
        }
        for (const auto& [c, col] : behind)
            stroke (g, c, col.withAlpha (0.35f), 1.2f, false);
        stroke (g, curve, colour, 2.2f, true);
        for (int i = 0; i < (int) curve.points.size(); ++i)
        {
            const auto p = toScreen (curve.points[(std::size_t) i]);
            const auto r = i == hover || i == dragging ? 5.5f : 4.0f;
            g.setColour (theme::colour::bg0);
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f + 2.0f, r * 2.0f + 2.0f).withCentre (p));
            g.setColour (i == sustainPoint ? theme::colour::warning : colour);
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p));
        }
        if (sustainPoint >= 0 && sustainPoint < (int) curve.points.size())
        {
            const auto x = toScreen (curve.points[(std::size_t) sustainPoint]).x;
            g.setColour (theme::colour::warning.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.setFont (theme::font (theme::type::caption));
            g.drawText (tr ("sustain"), juce::Rectangle<float> (x + 3.0f, area.getY() + 2.0f, 60.0f, 14.0f), juce::Justification::centredLeft);
        }
        if (marker >= 0.0f && marker <= xMax)
        {
            g.setColour (theme::colour::textPrimary.withAlpha (0.7f));
            const auto x = area.getX() + (float) (marker / xMax) * area.getWidth();
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }
        g.setColour (theme::colour::borderStrong);
        g.drawRoundedRectangle (area, 4.0f, 1.0f);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto h = pointAt (e.position);
        if (h != hover)
        {
            hover = h;
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
        const auto at = pointAt (e.position);
        if (e.mods.isPopupMenu())
        {
            if (at > 0 && at < (int) curve.points.size() - 1 && curve.points.size() > 2)
            {
                curve.points.erase (curve.points.begin() + at);
                hover = -1;
                commit();
            }
            return;
        }
        if (at >= 0)
        {
            dragging = at;
            return;
        }
        const auto seg = segmentAt (e.position.x);
        if (seg > 0 && e.mods.isAltDown())
        {
            curve.points[(std::size_t) seg].tension = 0.0f;
            curve.points[(std::size_t) seg].b15 = 2;
            commit();
            return;
        }
        if (seg > 0)
        {
            bending = seg;
            bendStart = curve.points[(std::size_t) seg].tension;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto area = plot();
        if (dragging >= 0)
        {
            auto& p = curve.points[(std::size_t) dragging];
            const auto xMax = range();
            auto x = (double) ((e.position.x - area.getX()) / area.getWidth()) * xMax;
            const auto lo = dragging == 0 ? 0.0 : curve.points[(std::size_t) dragging - 1].x;
            const auto hi = dragging + 1 < (int) curve.points.size() ? curve.points[(std::size_t) dragging + 1].x : (fixedXMax > 0.0 ? fixedXMax : 1.0e4);
            p.x = dragging == 0 ? 0.0 : std::clamp (x, lo, hi);
            p.y = std::clamp ((double) ((area.getBottom() - e.position.y) / area.getHeight()), 0.0, 1.0);
            if (e.mods.isShiftDown()) // (snap the level to 1/8)
                p.y = std::round (p.y * 8.0) / 8.0;
            commit();
        }
        else if (bending > 0)
        {
            auto& p = curve.points[(std::size_t) bending];
            const auto rising = p.y >= curve.points[(std::size_t) bending - 1].y;
            const auto dy = (float) -e.getDistanceFromDragStartY() / 120.0f;
            p.tension = std::clamp (bendStart + (rising ? -dy : dy), -1.0f, 1.0f);
            p.b15 = p.tension == 0.0f ? 2 : p.tension > 0.0f ? 1 : 0xff;
            commit();
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = bending = -1;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (pointAt (e.position) >= 0)
            return;
        const auto area = plot();
        const auto x = (double) ((e.position.x - area.getX()) / area.getWidth()) * range();
        const auto y = std::clamp ((double) ((area.getBottom() - e.position.y) / area.getHeight()), 0.0, 1.0);
        auto at = curve.points.begin();
        while (at != curve.points.end() && at->x <= x)
            ++at;
        if (at == curve.points.begin())
            return;
        curve.points.insert (at, vfx::MapCurve::Point { x, y });
        commit();
    }

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (2.0f, 2.0f); }
    double range() const
    {
        if (fixedXMax > 0.0)
            return fixedXMax;
        const auto last = curve.points.empty() ? 1.0 : curve.points.back().x;
        return std::max (1.0, std::ceil (last * 1.25 * 4.0) / 4.0);
    }
    juce::Point<float> toScreen (const vfx::MapCurve::Point& p) const
    {
        const auto area = plot();
        return { area.getX() + (float) (p.x / range()) * area.getWidth(), area.getBottom() - (float) p.y * area.getHeight() };
    }
    int pointAt (juce::Point<float> at) const
    {
        for (int i = (int) curve.points.size() - 1; i >= 0; --i)
            if (toScreen (curve.points[(std::size_t) i]).getDistanceFrom (at) < 8.0f)
                return i;
        return -1;
    }
    int segmentAt (float sx) const
    {
        for (int i = 1; i < (int) curve.points.size(); ++i)
            if (sx >= toScreen (curve.points[(std::size_t) i - 1]).x && sx < toScreen (curve.points[(std::size_t) i]).x)
                return i;
        return -1;
    }
    void stroke (juce::Graphics& g, const vfx::MapCurve& c, juce::Colour col, float width, bool fill) const
    {
        if (c.points.empty())
            return;
        const auto area = plot();
        const auto xMax = range();
        juce::Path path;
        const auto steps = (int) area.getWidth() / 2;
        for (int k = 0; k <= steps; ++k)
        {
            const auto x = xMax * k / steps;
            const juce::Point<float> p (area.getX() + area.getWidth() * (float) k / (float) steps, area.getBottom() - (float) c.at (x) * area.getHeight());
            if (k == 0)
                path.startNewSubPath (p);
            else
                path.lineTo (p);
        }
        if (fill)
        {
            juce::Path under (path);
            under.lineTo (area.getBottomRight());
            under.lineTo (area.getBottomLeft());
            under.closeSubPath();
            g.setColour (col.withAlpha (0.12f));
            g.fillPath (under);
        }
        g.setColour (col);
        g.strokePath (path, juce::PathStrokeType (width));
    }
    void commit()
    {
        repaint();
        if (onChange)
            onChange (curve);
    }

    vfx::MapCurve curve;
    int hover = -1, dragging = -1, bending = -1;
    float bendStart = 0.0f;
};

// ---------------------------------------------------------------------------------------------------------------
// VFX Keyboard Splitter: the zones, a page per map (keyboard, velocity), the current zone's map over the others

juce::Colour zoneColour (int z)
{
    return juce::Colour::fromHSV (std::fmod ((float) z * 2.5f / 16.0f, 1.0f), 0.62f, 0.95f, 1.0f);
}

class KeyboardSplitterBody final : public Body
{
public:
    using S = vfx::KeyboardSplitterSettings;

    KeyboardSplitterBody()
    {
        for (int z = 0; z < 16; ++z)
        {
            auto& b = zoneButtons[(std::size_t) z];
            b.setButtonText (juce::String (z + 1));
            b.setClickingTogglesState (false);
            b.onClick = [this, z] {
                zone = z;
                refreshControls();
            };
            addAndMakeVisible (b);
        }
        for (int p = 0; p < 2; ++p)
        {
            auto& b = pages[(std::size_t) p];
            b.setButtonText (p == 0 ? tr ("Keyboard") : tr ("Velocity"));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b.onClick = [this, p] { change ([p] (S& st) { st.page = p; }); };
            addAndMakeVisible (b);
        }
        name.setTextToShowWhenEmpty (tr ("Zone {0}", { "1" }), theme::colour::textDisabled);
        name.onReturnKey = name.onFocusLost = [this] {
            change ([z = zone, text = name.getText().trim().toStdString()] (S& st) { st.zones[(std::size_t) z].name = text; });
        };
        ui::setHint (name, tr ("The zone's name (its output's name in the Patcher)"));
        addAndMakeVisible (name);
        flag.onClick = [this] {
            change ([z = zone, on = flag.getToggleState()] (S& st) {
                auto& zn = st.zones[(std::size_t) z];
                (st.page == 0 ? zn.random : zn.absolute) = on;
            });
        };
        addAndMakeVisible (flag);
        transpose.caption = tr ("Transpose");
        addAndMakeVisible (transpose);
        for (auto [label, make] : std::initializer_list<std::pair<const char*, int>> { { TR_NOOP ("Whole keyboard"), 0 }, { TR_NOOP ("Clear"), 1 } })
        {
            auto* b = make == 0 ? &whole : &clear;
            b->setButtonText (tr (label));
            b->onClick = [this, make = make] {
                change ([z = zone, make] (S& st) {
                    auto& curve = st.page == 0 ? st.zones[(std::size_t) z].keyboard : st.zones[(std::size_t) z].velocity;
                    curve = vfx::MapCurve::flat (make == 0 ? 1.0 : (st.page == 0 ? 0.0 : 1.0));
                });
            };
            addAndMakeVisible (b);
        }
        ui::setHint (whole, tr ("The whole range at full level"));
        ui::setHint (clear, tr ("Back to the map's default (the zone is off when both maps are)"));
        editor.fixedXMax = 1.0;
        editor.onChange = [this] (const vfx::MapCurve& c) {
            change ([z = zone, c] (S& st) { (st.page == 0 ? st.zones[(std::size_t) z].keyboard : st.zones[(std::size_t) z].velocity) = c; });
        };
        editor.drawGrid = [this] (juce::Graphics& g, juce::Rectangle<float> area, double) {
            g.setFont (theme::font (theme::type::caption));
            if (settings.page == 0) // octaves: C0 … C10 (x = key / 132)
                for (int key = 0; key <= 132; key += 12)
                {
                    const auto x = area.getX() + area.getWidth() * (float) key / 132.0f;
                    g.setColour (theme::colour::border);
                    g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                    g.setColour (theme::colour::textSecondary);
                    if (key < 132)
                        g.drawText ("C" + juce::String (key / 12), juce::Rectangle<float> (x + 3.0f, area.getBottom() - 16.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
                }
            else
                for (int q = 1; q < 4; ++q)
                {
                    g.setColour (theme::colour::border);
                    g.drawVerticalLine ((int) (area.getX() + area.getWidth() * (float) q / 4.0f), area.getY(), area.getBottom());
                }
            for (int q = 1; q < 4; ++q)
            {
                g.setColour (theme::colour::border.withAlpha (0.5f));
                g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * (float) q / 4.0f), area.getX(), area.getRight());
            }
        };
        addAndMakeVisible (editor);
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        settings = vfx::parseKeyboardSplitter (fl).value_or (vfx::defaultKeyboardSplitter());
        refreshControls();
    }

    int preferredHeight() const override { return 400; }
    int preferredWidth() const override { return 860; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto top = area.removeFromTop (30);
        for (auto& b : zoneButtons)
            b.setBounds (top.removeFromLeft (30).reduced (1, 0));
        top.removeFromLeft (12);
        name.setBounds (top.removeFromLeft (150).reduced (0, 2));
        top.removeFromLeft (12);
        for (auto& b : pages)
            b.setBounds (top.removeFromLeft (84).reduced (1, 0));
        area.removeFromTop (6);
        auto row = area.removeFromTop (64);
        transpose.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (10);
        flag.setBounds (row.removeFromLeft (220).withSizeKeepingCentre (220, 24));
        clear.setBounds (row.removeFromRight (90).withSizeKeepingCentre (90, 26));
        row.removeFromRight (6);
        whole.setBounds (row.removeFromRight (130).withSizeKeepingCentre (130, 26));
        area.removeFromTop (6);
        editor.setBounds (area);
    }

private:
    void refreshControls()
    {
        const auto& z = settings.zones[(std::size_t) zone];
        for (int k = 0; k < 16; ++k)
        {
            auto& b = zoneButtons[(std::size_t) k];
            const auto active = settings.zones[(std::size_t) k].active();
            b.setColour (juce::TextButton::buttonColourId, active ? zoneColour (k).withAlpha (0.55f) : theme::colour::bg3);
            b.setColour (juce::TextButton::buttonOnColourId, zoneColour (k));
            b.setColour (juce::TextButton::textColourOffId, active ? theme::colour::textPrimary : theme::colour::textDisabled);
            b.setToggleState (k == zone, juce::dontSendNotification);
        }
        for (int p = 0; p < 2; ++p)
            pages[(std::size_t) p].setToggleState (p == settings.page, juce::dontSendNotification);
        if (! name.hasKeyboardFocus (true))
            name.setText (juce::String::fromUTF8 (z.name.c_str()), juce::dontSendNotification);
        name.setTextToShowWhenEmpty (tr ("Zone {0}", { juce::String (zone + 1) }), theme::colour::textDisabled);
        const auto keyboard = settings.page == 0;
        flag.setButtonText (keyboard ? tr ("Random (the map is a probability)") : tr ("Absolute (the map sets the volume)"));
        ui::setHint (flag, keyboard ? tr ("Random: each note goes to one of the random zones, picked by their keyboard maps")
                                    : tr ("Absolute: the velocity map gives the volume instead of scaling the note's"));
        flag.setToggleState (keyboard ? z.random : z.absolute, juce::dontSendNotification);
        transpose.setVisible (keyboard);
        editor.colour = zoneColour (zone);
        editor.behind.clear();
        for (int k = 0; k < 16; ++k)
            if (k != zone && settings.zones[(std::size_t) k].active())
                editor.behind.emplace_back (keyboard ? settings.zones[(std::size_t) k].keyboard : settings.zones[(std::size_t) k].velocity, zoneColour (k));
        editor.setCurve (keyboard ? z.keyboard : z.velocity);
        transpose.repaint();
        repaint();
    }

    void change (std::function<void (S&)> f)
    {
        if (edit)
            edit ([f] (std::vector<std::uint8_t>& fl) {
                auto s = vfx::parseKeyboardSplitter (fl).value_or (vfx::defaultKeyboardSplitter());
                f (s);
                fl = vfx::writeKeyboardSplitter (s);
            });
    }

    S settings = vfx::defaultKeyboardSplitter();
    int zone = 0;
    std::array<juce::TextButton, 16> zoneButtons;
    std::array<juce::TextButton, 2> pages;
    juce::TextEditor name;
    juce::ToggleButton flag;
    juce::TextButton whole, clear;
    Knob transpose { intSpec (tr ("Transpose (semitones)"), -48, 48, 0, [] (double v) { return (v > 0 ? "+" : "") + juce::String ((int) std::lround (v)); }),
                     [this] { return (double) settings.zones[(std::size_t) zone].transpose; },
                     [this] (double v) { change ([z = zone, v] (S& st) { st.zones[(std::size_t) z].transpose = (int) std::lround (v); }); } };
    CurveEditor editor;
};

// ---------------------------------------------------------------------------------------------------------------
// VFX Envelope: 16 articulators (each for a note colour and key range), 5 targets, an envelope and an LFO each

class EnvelopeBody final : public Body
{
public:
    using E = vfx::EnvelopeSettings;

    EnvelopeBody()
    {
        for (int g = 0; g < 16; ++g)
        {
            auto& b = articulatorButtons[(std::size_t) g];
            b.setButtonText (juce::String (g + 1));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
            b.onClick = [this, g] { change ([g] (E& st) { st.shownArticulator = g; }); };
            ui::setHint (b, tr ("Articulator {0}", { juce::String (g + 1) }));
            addAndMakeVisible (b);
        }
        static const char* targetNames[] = { TR_NOOP ("Velocity"), TR_NOOP ("Pitch"), TR_NOOP ("Pan"), TR_NOOP ("Mod X"), TR_NOOP ("Mod Y") };
        for (int t = 0; t < 5; ++t)
        {
            auto& b = targets[(std::size_t) t];
            b.setButtonText (tr (targetNames[t]));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b.onClick = [this, t] { change ([t] (E& st) { st.shownTarget = t; }); };
            addAndMakeVisible (b);
        }
        for (int p = 0; p < 2; ++p)
        {
            auto& b = partTabs[(std::size_t) p];
            b.setButtonText (p == 0 ? tr ("Envelope") : tr ("LFO"));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b.onClick = [this, p] { change ([p] (E& st) { st.shownPart = p; }); };
            addAndMakeVisible (b);
        }
        for (int c = 0; c < 16; ++c)
            colour.addItem (tr ("Colour {0}", { juce::String (c + 1) }), c + 1);
        colour.onChange = [this] { changeArticulator ([c = colour.getSelectedId() - 1] (E::Articulator& a) { a.colour = std::max (0, c); }); };
        ui::setHint (colour, tr ("The note colour this articulator works on (the MIDI channel)"));
        addAndMakeVisible (colour);
        enable.setButtonText (tr ("Enable"));
        enable.onClick = [this] { change ([on = enable.getToggleState()] (E& st) { st.enabled = on; }); };
        ui::setHint (enable, tr ("Off: the notes pass unchanged"));
        addAndMakeVisible (enable);
        partOn.onClick = [this] {
            changePart ([lfo = settings.shownPart == 1, on = partOn.getToggleState()] (E::Part& p) { (lfo ? p.lfoAmp : p.envelope).footer[1] = on ? -1 : 0; });
        };
        addAndMakeVisible (partOn);
        tempo.setButtonText (tr ("Tempo"));
        tempo.onClick = [this] {
            changePart ([lfo = settings.shownPart == 1, on = tempo.getToggleState()] (E::Part& p) {
                auto& f = (lfo ? p.lfoAmp : p.envelope).footer[0];
                f = on ? (f | 1) : (f & ~1);
            });
        };
        ui::setHint (tempo, tr ("Tempo: the time in beats (else in seconds)"));
        addAndMakeVisible (tempo);
        for (auto* k : { &low, &high, &base, &envLevel, &lfoLevel, &attack, &decay, &sustain, &release, &speed, &lfoTension, &skew, &pulseWidth })
            addAndMakeVisible (k);
        low.caption = tr ("Low");
        high.caption = tr ("High");
        base.caption = tr ("Base");
        envLevel.caption = tr ("Env");
        lfoLevel.caption = tr ("LFO");
        attack.caption = tr ("Attack");
        decay.caption = tr ("Decay");
        sustain.caption = tr ("Sustain");
        release.caption = tr ("Release");
        speed.caption = tr ("Speed");
        lfoTension.caption = tr ("Tension");
        skew.caption = tr ("Skew");
        pulseWidth.caption = tr ("Pulse width");
        editor.onChange = [this] (const vfx::MapCurve& c) {
            changePart ([lfo = settings.shownPart == 1, c] (E::Part& p) {
                auto& curve = lfo ? p.lfoAmp : p.envelope;
                const auto footer = curve.footer;
                curve = c;
                curve.footer = footer;
                // the sustain / decay / loop points stay inside the curve
                for (const auto k : { 2, 3, 4 })
                    if (curve.footer[(std::size_t) k] >= (int) curve.points.size())
                        curve.footer[(std::size_t) k] = (int) curve.points.size() - 1;
            });
        };
        addAndMakeVisible (editor);
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        settings = vfx::parseEnvelope (fl).value_or (vfx::defaultEnvelope());
        const auto& a = articulator();
        const auto& p = part();
        for (int g = 0; g < 16; ++g)
        {
            const auto& ag = settings.articulators[(std::size_t) g];
            const auto used = std::any_of (ag.parts.begin(), ag.parts.end(), [] (const E::Part& q) { return q.lfoOn() || (q.envOn() && ! neutral (q)); });
            articulatorButtons[(std::size_t) g].setColour (juce::TextButton::textColourOffId, used ? theme::colour::textPrimary : theme::colour::textDisabled);
            articulatorButtons[(std::size_t) g].setToggleState (g == settings.shownArticulator, juce::dontSendNotification);
        }
        for (int t = 0; t < 5; ++t)
        {
            const auto& q = a.parts[(std::size_t) t];
            targets[(std::size_t) t].setToggleState (t == settings.shownTarget, juce::dontSendNotification);
            targets[(std::size_t) t].setColour (juce::TextButton::textColourOffId,
                                                q.lfoOn() || (q.envOn() && ! neutral (q)) ? theme::colour::accent : theme::colour::textPrimary);
        }
        for (int k = 0; k < 2; ++k)
            partTabs[(std::size_t) k].setToggleState (k == settings.shownPart, juce::dontSendNotification);
        colour.setSelectedId (a.colour + 1, juce::dontSendNotification);
        enable.setToggleState (settings.enabled, juce::dontSendNotification);
        const auto lfo = settings.shownPart == 1;
        const auto& curve = lfo ? p.lfoAmp : p.envelope;
        partOn.setButtonText (lfo ? tr ("LFO on") : tr ("Envelope on"));
        partOn.setToggleState (lfo ? p.lfoOn() : p.envOn(), juce::dontSendNotification);
        tempo.setToggleState ((curve.footer[0] & 1) != 0, juce::dontSendNotification);
        for (auto* k : { &attack, &decay, &sustain, &release })
            k->setVisible (! lfo);
        for (auto* k : { &speed, &lfoTension, &skew, &pulseWidth })
            k->setVisible (lfo);
        editor.bipolar = lfo || settings.shownTarget != E::velocity;
        editor.sustainPoint = curve.footer[4];
        editor.setCurve (curve);
        for (auto* c : getChildren())
            if (dynamic_cast<Knob*> (c) != nullptr)
                c->repaint();
        repaint();
    }

    int preferredHeight() const override { return 470; }
    int preferredWidth() const override { return 860; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto top = area.removeFromTop (30);
        for (auto& b : articulatorButtons)
            b.setBounds (top.removeFromLeft (28).reduced (1, 0));
        top.removeFromLeft (14);
        for (auto& b : targets)
            b.setBounds (top.removeFromLeft (72).reduced (1, 0));
        enable.setBounds (top.removeFromRight (90));
        area.removeFromTop (8);
        auto row = area.removeFromTop (70);
        auto left = row.removeFromLeft (130);
        colour.setBounds (left.removeFromTop (26));
        for (auto* k : { &low, &high })
            k->setBounds (row.removeFromLeft (62));
        row.removeFromLeft (14);
        for (auto* k : { &base, &envLevel, &lfoLevel })
            k->setBounds (row.removeFromLeft (62));
        area.removeFromTop (8);
        auto parts = area.removeFromTop (28);
        for (auto& b : partTabs)
            b.setBounds (parts.removeFromLeft (100).reduced (1, 0));
        parts.removeFromLeft (14);
        partOn.setBounds (parts.removeFromLeft (140));
        tempo.setBounds (parts.removeFromLeft (90));
        area.removeFromTop (6);
        auto knobs = area.removeFromBottom (70);
        for (auto* k : { &attack, &decay, &sustain, &release })
            k->setBounds (knobs.withWidth (62).translated ((int) (k == &attack ? 0 : k == &decay ? 1 : k == &sustain ? 2 : 3) * 66, 0));
        for (auto* k : { &speed, &lfoTension, &skew, &pulseWidth })
            k->setBounds (knobs.withWidth (62).translated ((int) (k == &speed ? 0 : k == &lfoTension ? 1 : k == &skew ? 2 : 3) * 66, 0));
        area.removeFromBottom (6);
        editor.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        const auto& p = part();
        const auto lfo = settings.shownPart == 1;
        const auto unit = ((lfo ? p.lfoAmp : p.envelope).footer[0] & 1) != 0 ? tr ("beats") : tr ("seconds");
        g.drawText (tr ("Time in {0}. Drag a point, double-click to add one, right-click to remove it, drag a segment to bend it.", { unit }),
                    editor.getBounds().translated (0, editor.getHeight() + 2).withHeight (14), juce::Justification::centredLeft);
    }

private:
    static bool neutral (const E::Part& p)
    {
        return std::all_of (p.envelope.points.begin(), p.envelope.points.end(), [] (const vfx::MapCurve::Point& q) { return std::abs (q.y - 0.5) < 1e-6; })
               && std::abs (p.base - 0.5f) < 1e-6f;
    }
    const E::Articulator& articulator() const { return settings.articulators[(std::size_t) settings.shownArticulator]; }
    const E::Part& part() const { return articulator().parts[(std::size_t) settings.shownTarget]; }

    void change (std::function<void (E&)> f)
    {
        if (edit)
            edit ([f] (std::vector<std::uint8_t>& fl) {
                auto s = vfx::parseEnvelope (fl).value_or (vfx::defaultEnvelope());
                f (s);
                fl = vfx::writeEnvelope (s);
            });
    }
    void changeArticulator (std::function<void (E::Articulator&)> f)
    {
        change ([f] (E& st) { f (st.articulators[(std::size_t) st.shownArticulator]); });
    }
    void changePart (std::function<void (E::Part&)> f)
    {
        change ([f] (E& st) { f (st.articulators[(std::size_t) st.shownArticulator].parts[(std::size_t) st.shownTarget]); });
    }
    Knob partKnob (const juce::String& hint, int lo, int hi, int def, std::function<int& (E::Part&)> field, std::function<juce::String (double)> text = nullptr)
    {
        return Knob (intSpec (hint, lo, hi, def, std::move (text)),
                     [this, field] { auto p = part(); return (double) field (p); },
                     [this, field] (double v) { changePart ([field, v] (E::Part& p) { field (p) = (int) std::lround (v); }); });
    }
    Knob levelKnob (const juce::String& hint, int lo, int hi, int def, float E::Part::*field)
    {
        return Knob (intSpec (hint, lo, hi, def, [lo] (double v) { return lo < 0 ? juce::String (v / 2.56, 0) + "%" : juce::String (v / 2.56, 0) + "%"; }),
                     [this, field] { return (double) std::lround (part().*field * 256.0f); },
                     [this, field] (double v) { changePart ([field, v] (E::Part& p) { p.*field = (float) (v / 256.0); }); });
    }
    static juce::String keyText (double v)
    {
        const auto k = (int) std::lround (v) - 1; // (FL stores the key + 1)
        return k < 0 ? juce::String ("-") : juce::MidiMessage::getMidiNoteName (std::clamp (k, 0, 131), true, true, 0);
    }

    E settings = vfx::defaultEnvelope();
    std::array<juce::TextButton, 16> articulatorButtons;
    std::array<juce::TextButton, 5> targets;
    std::array<juce::TextButton, 2> partTabs;
    juce::ComboBox colour;
    juce::ToggleButton enable, partOn, tempo;
    Knob low { intSpec (tr ("Low key: the lowest note this articulator works on"), 0, 132, 0, keyText),
               [this] { return (double) articulator().low; },
               [this] (double v) { changeArticulator ([v] (E::Articulator& a) { a.low = (int) std::lround (v); a.high = std::max (a.high, a.low); }); } };
    Knob high { intSpec (tr ("High key: the highest note this articulator works on"), 0, 132, 132, keyText),
                [this] { return (double) articulator().high; },
                [this] (double v) { changeArticulator ([v] (E::Articulator& a) { a.high = (int) std::lround (v); a.low = std::min (a.low, a.high); }); } };
    Knob base = levelKnob (tr ("Base level"), 0, 256, 128, &E::Part::base);
    Knob envLevel = levelKnob (tr ("Envelope level"), -256, 256, 256, &E::Part::env);
    Knob lfoLevel = levelKnob (tr ("LFO level"), -256, 256, 256, &E::Part::lfo);
    Knob attack = partKnob (tr ("Attack: scales the time up to the decay point"), 0, 256, 128, [] (E::Part& p) -> int& { return p.adsr[0]; });
    Knob decay = partKnob (tr ("Decay: scales the time up to the sustain point"), 0, 256, 128, [] (E::Part& p) -> int& { return p.adsr[1]; });
    Knob sustain = partKnob (tr ("Sustain: raises or lowers the sustain level"), -256, 256, 0, [] (E::Part& p) -> int& { return p.adsr[2]; });
    Knob release = partKnob (tr ("Release: scales the time after the sustain point"), 0, 256, 128, [] (E::Part& p) -> int& { return p.adsr[3]; });
    Knob speed = partKnob (tr ("LFO speed"), 0, 65536, 40898, [] (E::Part& p) -> int& { return p.speed; }, [] (double v) {
        const auto rate = (std::pow (10001.0, v / 65536.0) - 1.0) / 625.0;
        return juce::String (rate, rate < 1.0 ? 2 : 1);
    });
    Knob lfoTension = partKnob (tr ("LFO tension"), -128, 128, 0, [] (E::Part& p) -> int& { return p.tension; });
    Knob skew = partKnob (tr ("LFO skew"), -128, 128, 0, [] (E::Part& p) -> int& { return p.skew; });
    Knob pulseWidth = partKnob (tr ("LFO pulse width"), -128, 128, 0, [] (E::Part& p) -> int& { return p.pulseWidth; });
    CurveEditor editor;
};

// ---------------------------------------------------------------------------------------------------------------
// VFX Sequencer (FL's layout, LYNX's look): the sequence on top, a big grid that edits the chosen lane, the six lanes
// below (click one to edit it), the parameters on the right; 16 steps per page

class SequencerBody final : public Body
{
public:
    using S = vfx::SequencerSettings;
    enum Layer { noteSelect, velocity, octave, gate, stepType, scaleStep, layerCount };

    SequencerBody()
    {
        program.onClick = [this] { showProgramMenu(); };
        ui::setHint (program, tr ("The sequence (program) of the bank that plays; the bank keeps 128"));
        addAndMakeVisible (program);
        for (auto* b : { &prevProgram, &nextProgram, &dice, &shiftLeft, &shiftRight })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        prevProgram.onClick = [this] { stepProgram (-1); };
        nextProgram.onClick = [this] { stepProgram (1); };
        dice.onClick = [this] {
            // a random sequence of those that differ from the first (the bank's empty slots are copies of it)
            std::vector<int> candidates;
            for (int p = 1; p < (int) settings.programs.size(); ++p)
                if (settings.programs[(std::size_t) p].lanes != settings.programs[0].lanes || settings.programs[(std::size_t) p].params != settings.programs[0].params)
                    candidates.push_back (p);
            if (! candidates.empty())
                change ([n = candidates[(std::size_t) juce::Random::getSystemRandom().nextInt ((int) candidates.size())]] (S& s) { s.set (S::currentProgram, n); });
        };
        ui::setHint (dice, tr ("A random sequence"));
        shiftLeft.onClick = [this] { change ([] (S& s) { vfx::shiftPattern (s, -1); }); };
        shiftRight.onClick = [this] { change ([] (S& s) { vfx::shiftPattern (s, 1); }); };
        ui::setHint (shiftLeft, tr ("Shift the pattern one step earlier"));
        ui::setHint (shiftRight, tr ("Shift the pattern one step later"));
        for (int p = 0; p < 4; ++p)
        {
            auto& b = pages[(std::size_t) p];
            b.setButtonText (juce::String (p + 1));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b.onClick = [this, p] { change ([p] (S& s) { s.set (S::page, p); }); };
            ui::setHint (b, tr ("Steps {0} to {1}", { juce::String (p * 16 + 1), juce::String (p * 16 + 16) }));
            addAndMakeVisible (b);
        }
        toggle (pageLock, tr ("Page lock"), S::pageLock, tr ("Plays only the page shown"));
        toggle (latch, tr ("Latch"), S::latch, tr ("Latch: the notes keep playing after the keys are released, until a new chord"));
        toggle (polyKeys, tr ("Poly"), S::polyKeys, tr ("Several keys per step (else one)"));
        toggle (polyOctave, tr ("Poly"), S::polyOctave, tr ("Several octaves per step (else one)"));
        toggle (gateLaneMode, tr ("Channels"), S::gateLaneMode, tr ("The gate lane sets each step's output channel instead of its length"));

        combo (arpMode, tr ("Mode"), S::arpMode, { tr ("Off"), tr ("On"), tr ("Thru"), tr ("On from 2 keys, else off"), tr ("On from 2 keys, else thru"),
                                                    tr ("On from 3 keys, else off"), tr ("On from 3 keys, else thru") });
        combo (programMode, tr ("Input"), S::programMode, { tr ("The held keys"), tr ("The fixed key") });
        combo (order, tr ("Order"), S::order, { tr ("By pitch"), tr ("By pitch, descending"), tr ("As played"), tr ("As played, reversed"),
                                                 tr ("By velocity"), tr ("By velocity, descending"), tr ("Chord, normalized"), tr ("Chord, as played") });
        combo (missing, tr ("Missing keys"), S::missingKeys, { tr ("Not played"), tr ("Cyclic"), tr ("First key"), tr ("Last key"), tr ("Fixed key") });
        combo (missingOctave, tr ("Missing octave"), S::missingOctave, { tr ("Same"), "-1", "+1" });
        combo (quantize, tr ("Input quantize"), S::inputQuantize, { tr ("None"), "1/16", "1/12", "1/8", "1/6", "1/4", "1/2", tr ("1 bar"), tr ("2 bars") });
        combo (sync, tr ("Step"), S::sync, { "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "3/64", "3/32", "3/16", "3/8" });
        combo (restart, tr ("Restart on"), S::restartOn, { tr ("Beat"), tr ("Key"), tr ("First key"), tr ("Play") });
        combo (velocityMode, tr ("Velocity"), S::velocityMode, { tr ("From the lane"), tr ("From the keys"), tr ("Keys + lane") });
        juce::StringArray keysList { tr ("Off"), tr ("Detect") };
        for (int k = 0; k < 12; ++k)
            keysList.add (pitchClassName (k));
        combo (forceKey, tr ("Scale key"), S::forceKey, keysList);
        combo (forceScale, tr ("Scale"), S::forceScale, { tr ("Chromatic"), tr ("Detect"), tr ("Major"), tr ("Minor"), tr ("Harmonic minor"), tr ("Melodic minor"),
                                                          tr ("Pentatonic major"), tr ("Pentatonic minor"), tr ("Pentatonic neutral"), tr ("Blues"),
                                                          tr ("Dorian"), tr ("Phrygian"), tr ("Lydian"), tr ("Mixolydian"), tr ("Locrian") });
        combo (forceMode, tr ("Scale mode"), S::forceMode, { tr ("All notes"), tr ("Scale steps only") });
        ui::setHint (arpMode, tr ("Mode: Thru passes the notes (transposed); the auto modes play only while enough keys are held"));
        ui::setHint (order, tr ("Order: how the held keys are numbered N1, N2…"));
        ui::setHint (missing, tr ("What the steps of a key that is not held play"));
        ui::setHint (restart, tr ("Where the sequence starts: on the song's beats, at each new key, at the first key, at play"));
        ui::setHint (forceKey, tr ("Scale key: Detect takes the held chord's root"));
        ui::setHint (forceScale, tr ("Scale: the notes snap to it and the scale-step lane moves along it"));
        ui::setHint (quantize, tr ("Input quantize: chord changes wait for this grid"));

        auto percent = [] (double v) { return juce::String ((int) v) + " %"; };
        auto key = [] (double v) { return keyName ((int) v); };
        auto signedValue = [] (double v) { return v > 0 ? "+" + juce::String ((int) v) : juce::String ((int) v); };
        box (steps, tr ("Steps"), S::steps);
        box (gatePercent, tr ("Gate"), S::gate, percent);
        box (swing, tr ("Swing"), S::swing, signedValue);
        box (fixedKey, tr ("Fixed key"), S::fixedKey, key);
        box (smartBend, tr ("Smart bend"), S::smartBend, signedValue);
        box (transpose, tr ("Transpose"), S::transposeSemitones, signedValue);
        box (octaves, tr ("Octave"), S::transposeOctaves, signedValue);
        box (randomVelocity, tr ("Rand. vel."), S::randomVelocity, percent);
        box (randomGate, tr ("Rand. gate"), S::randomGate, percent);
        box (randomStart, tr ("Rand. start"), S::randomStart, percent);
        box (outLow, tr ("Out low"), S::outputLow, key);
        box (outHigh, tr ("Out high"), S::outputHigh, key);

        static const char* laneNames[] = { TR_NOOP ("Note select"), TR_NOOP ("Velocity"), TR_NOOP ("Octave"), TR_NOOP ("Gate"), TR_NOOP ("Step type"), TR_NOOP ("Scale step") };
        for (int l = 0; l < layerCount; ++l)
        {
            auto& b = laneButtons[(std::size_t) l];
            b.setButtonText (tr (laneNames[l]));
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b.onClick = [this, l] { showLayer (l); };
            ui::setHint (b, tr ("Edit this lane in the grid above"));
            addAndMakeVisible (b);
        }
        grid.owner = this;
        addAndMakeVisible (grid);
        for (int l = 0; l < layerCount; ++l)
        {
            auto& r = rows[(std::size_t) l];
            r.owner = this;
            r.layer = l;
            addAndMakeVisible (r);
        }
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        settings = vfx::parseSequencer (fl).value_or (vfx::defaultSequencer());
        program.setButtonText (juce::String (settings.program() + 1).paddedLeft ('0', 3) + "  " + juce::String (settings.current().nameText()));
        for (int k = 0; k < 4; ++k)
            pages[(std::size_t) k].setToggleState (k == settings.get (S::page), juce::dontSendNotification);
        for (int k = 0; k < layerCount; ++k)
            laneButtons[(std::size_t) k].setToggleState (k == shown, juce::dontSendNotification);
        for (auto& [b, param] : toggles)
            b->setToggleState (settings.get (param) != 0, juce::dontSendNotification);
        for (auto& [c, param] : combos)
            c->setSelectedItemIndex (settings.get (param) - S::minimum (param), juce::dontSendNotification);
        for (auto& b : boxes)
            b->repaint();
        grid.repaint();
        for (auto& r : rows)
            r.repaint();
        repaint();
    }

    int preferredWidth() const override { return 1080; }
    int preferredHeight() const override { return 470; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 4);
        auto right = area.removeFromRight (336);
        area.removeFromRight (14);

        auto top = area.removeFromTop (28);
        prevProgram.setBounds (top.removeFromLeft (24));
        program.setBounds (top.removeFromLeft (250));
        nextProgram.setBounds (top.removeFromLeft (24));
        top.removeFromLeft (4);
        dice.setBounds (top.removeFromLeft (28));
        top.removeFromLeft (10);
        latch.setBounds (top.removeFromLeft (70));
        shiftRight.setBounds (top.removeFromRight (28));
        shiftLeft.setBounds (top.removeFromRight (28));
        top.removeFromRight (8);
        pageLock.setBounds (top.removeFromRight (96));
        for (int k = 3; k >= 0; --k)
            pages[(std::size_t) k].setBounds (top.removeFromRight (28).reduced (1, 0));
        area.removeFromTop (8);

        auto lanes = area.removeFromBottom (layerCount * 24);
        for (int l = 0; l < layerCount; ++l)
        {
            auto row = lanes.removeFromTop (24).reduced (0, 1);
            auto label = row.removeFromLeft (Grid::labelWidth + 50);
            laneButtons[(std::size_t) l].setBounds (label.removeFromLeft (Grid::labelWidth + 6));
            if (l == noteSelect)
                polyKeys.setBounds (label.reduced (2, 0));
            if (l == octave)
                polyOctave.setBounds (label.reduced (2, 0));
            if (l == gate)
                gateLaneMode.setBounds (label.reduced (2, 0).withWidth (60));
            rows[(std::size_t) l].setBounds (row.withX (area.getX() + Grid::labelWidth).withWidth (area.getWidth() - Grid::labelWidth));
        }
        area.removeFromBottom (8);
        grid.setBounds (area);

        // the parameters: four sections, three controls a row
        int y = right.getY();
        auto section = [&] (int index, std::initializer_list<juce::Component*> items) {
            sectionTops[(std::size_t) index] = y;
            y += 20;
            int column = 0;
            for (auto* c : items)
            {
                const auto x = right.getX() + column * 112;
                c->setBounds (x + 2, y + 15, 106, 22);
                if (dynamic_cast<Knob*> (c) != nullptr)
                    c->setBounds (x + 2, y, 106, 37);
                if (++column == 3)
                {
                    column = 0;
                    y += 40;
                }
            }
            if (column != 0)
                y += 40;
            y += 4;
        };
        section (0, { &arpMode, &programMode, &velocityMode });
        section (1, { &order, &missing, &missingOctave, &quantize, smartBend.get(), fixedKey.get() });
        section (2, { steps.get(), &sync, gatePercent.get(), swing.get(), &restart, &forceMode, &forceKey, &forceScale });
        section (3, { transpose.get(), octaves.get(), randomVelocity.get(), randomGate.get(), randomStart.get(), outLow.get(), outHigh.get() });
        sectionTops[4] = y;
    }

    void paint (juce::Graphics& g) override
    {
        static const char* titles[] = { TR_NOOP ("Mode"), TR_NOOP ("Input"), TR_NOOP ("Arpeggiator"), TR_NOOP ("Output") };
        const auto x = getLocalBounds().reduced (12, 4).getRight() - 336;
        for (int k = 0; k < 4; ++k)
        {
            const auto y = sectionTops[(std::size_t) k];
            g.setColour (theme::colour::bg3);
            g.fillRoundedRectangle (juce::Rectangle<float> ((float) x - 6.0f, (float) y, 342.0f, (float) (sectionTops[(std::size_t) k + 1] - y - 4)), 4.0f);
            g.setColour (theme::colour::accent);
            g.setFont (theme::font (theme::type::caption, true));
            g.drawText (theme::caps (tr (titles[k])), x, y + 2, 200, 16, juce::Justification::centredLeft);
        }
        g.setFont (theme::font (theme::type::caption));
        g.setColour (theme::colour::textSecondary);
        for (auto& [c, param] : combos)
            g.drawText (c->getName(), c->getBounds().translated (0, -15).withHeight (14), juce::Justification::centredLeft);
    }

    // lane values as text and colour (the big grid's labels and the lane rows)
    juce::String text (int layer, int step) const
    {
        const auto& p = settings.current();
        const auto v = (int) p.lanes[(std::size_t) laneOf (layer)][(std::size_t) step];
        switch (layer)
        {
            case noteSelect:
            {
                static const char* names[] = { "F", "R", "N1", "N2", "N3", "N4", "N5" };
                juce::String s;
                for (int bit = 0; bit < 7; ++bit)
                    if (v & (1 << bit))
                        s << (s.isEmpty() ? "" : "+") << names[bit];
                return s.isEmpty() ? "-" : s;
            }
            case octave:
            {
                juce::String s;
                for (int bit = 0; bit < 7; ++bit)
                    if (v & (1 << bit))
                        s << (s.isEmpty() ? "" : ",") << (bit == 3 ? juce::String ("-") : bit > 3 ? "+" + juce::String (bit - 3) : juce::String (bit - 3));
                return s.isEmpty() ? juce::String ("x") : s;
            }
            case gate:
            {
                if (settings.get (S::gateLaneMode) != 0)
                    return v == 0 ? juce::String ("-") : juce::String (v);
                static const char* names[] = { "1/8", "1/4", "1/2", "-", "2x", "4x", "8x", "16x", "32x" };
                return names[juce::jlimit (0, 8, v + 3)];
            }
            case stepType:
            {
                static const char* names[] = { "Off", "Nrm", "Rst", "Tie", "Chr", "Rnd" };
                return names[juce::jlimit (0, 5, v)];
            }
            case scaleStep: return v == 0 ? juce::String ("-") : (v > 0 ? "+" : "") + juce::String (v);
            default: return juce::String (v);
        }
    }

    static juce::Colour laneColour (int layer)
    {
        static const juce::uint32 colours[] = { 0xffe5383b, 0xff59c27a, 0xffe8894a, 0xff4cb8d8, 0xff3fbf86, 0xffd9b443 };
        return juce::Colour (colours[juce::jlimit (0, 5, layer)]);
    }

    static int laneOf (int layer)
    {
        static const int lanes[] = { S::keySelect, S::velocity, S::octave, S::gateStep, S::stepType, S::scaleStep };
        return lanes[juce::jlimit (0, 5, layer)];
    }

    int firstStep() const { return settings.get (S::page) * 16; }

    // the big grid: rows for the bit / choice lanes, bars for velocity, scale step and channels
    struct Grid final : juce::Component
    {
        SequencerBody* owner = nullptr;
        static constexpr int labelWidth = 70;

        int layer() const { return owner->shown; }
        bool bars() const
        {
            return layer() == velocity || layer() == scaleStep || (layer() == gate && owner->settings.get (S::gateLaneMode) != 0);
        }
        int rows() const
        {
            switch (layer())
            {
                case noteSelect: case octave: return 7;
                case gate: return 9;
                case stepType: return 6;
                default: return 0;
            }
        }
        juce::String rowName (int row) const
        {
            switch (layer())
            {
                case noteSelect:
                {
                    static const char* names[] = { "N5", "N4", "N3", "N2", "N1", TR_NOOP ("Root"), TR_NOOP ("Fixed") };
                    return row >= 5 ? tr (names[row]) : names[row];
                }
                case octave: return row == 3 ? "0" : (row < 3 ? "+" : "") + juce::String (3 - row);
                case gate:
                {
                    static const char* names[] = { "32x", "16x", "8x", "4x", "2x", "1x", "1/2", "1/4", "1/8" };
                    return names[row];
                }
                case stepType:
                {
                    static const char* names[] = { TR_NOOP ("Random"), TR_NOOP ("Chord"), TR_NOOP ("Tie"), TR_NOOP ("Rest"), TR_NOOP ("Note"), TR_NOOP ("Off") };
                    return tr (names[row]);
                }
                default: return {};
            }
        }
        // row ↔ value: the bit lanes have a bit per row (top = highest), the others a value per row
        bool rowSet (int value, int row) const
        {
            if (layer() == noteSelect || layer() == octave)
                return (value & (1 << (6 - row))) != 0;
            return value == (layer() == gate ? 5 - row : 5 - row);
        }
        int barMin() const { return layer() == scaleStep ? (owner->settings.get (14) == 2 || owner->settings.get (14) == 0 ? -12 : 0) : 0; }
        int barMax() const { return layer() == scaleStep ? 12 : layer() == gate ? 16 : 127; }

        juce::Rectangle<int> cells() const { return getLocalBounds().withTrimmedLeft (labelWidth).withTrimmedBottom (18); }
        int stepAt (int x) const
        {
            const auto c = cells();
            return owner->firstStep() + juce::jlimit (0, 15, (x - c.getX()) * 16 / std::max (1, c.getWidth()));
        }
        int rowAt (int y) const
        {
            const auto c = cells();
            return juce::jlimit (0, rows() - 1, (y - c.getY()) * rows() / std::max (1, c.getHeight()));
        }

        void paint (juce::Graphics& g) override
        {
            const auto& s = owner->settings;
            const auto c = cells();
            const auto steps = s.get (S::steps);
            const auto lane = laneOf (layer());
            const auto colour = laneColour (layer());
            const auto w = c.getWidth() / 16.0f;
            g.setColour (theme::colour::bg0);
            g.fillRoundedRectangle (c.toFloat().expanded (2.0f), 3.0f);
            g.setFont (theme::font (theme::type::caption));
            for (int k = 0; k < 16; ++k)
            {
                const auto step = owner->firstStep() + k;
                const auto x = c.getX() + k * w;
                const auto used = step < steps;
                const auto value = (int) s.current().lanes[(std::size_t) lane][(std::size_t) step];
                const auto type = (int) s.current().lanes[S::stepType][(std::size_t) step];
                const auto silent = type == S::off || type == S::rest;
                g.setColour (k % 4 == 0 ? theme::colour::textPrimary : theme::colour::textSecondary);
                g.drawText (juce::String (step + 1), juce::Rectangle<float> (x, (float) c.getBottom() + 3.0f, w, 14.0f), juce::Justification::centred);
                const auto alpha = used ? 1.0f : 0.35f;
                if (! bars())
                {
                    const auto h = c.getHeight() / (float) rows();
                    for (int r = 0; r < rows(); ++r)
                    {
                        const auto cell = juce::Rectangle<float> (x, c.getY() + r * h, w, h).reduced (1.0f);
                        g.setColour ((k / 4 % 2 == 0 ? theme::colour::bg4 : theme::colour::bg3).withMultipliedAlpha (alpha));
                        g.fillRect (cell);
                        if (rowSet (value, r))
                        {
                            g.setColour (colour.withMultipliedAlpha (alpha * (layer() == noteSelect && (silent || type == S::chord || type == S::random) ? 0.4f : 1.0f)));
                            g.fillRect (cell);
                            g.setColour (juce::Colours::white.withAlpha (alpha));
                            const auto label = layer() == noteSelect ? rowName (r) : owner->text (noteSelect, step);
                            if (cell.getWidth() > 24.0f)
                                g.drawText (label, cell, juce::Justification::centred);
                        }
                    }
                }
                else
                {
                    const auto lo = barMin(), hi = barMax();
                    const auto zero = lo < 0 ? c.getY() + c.getHeight() * (float) hi / (float) (hi - lo) : (float) c.getBottom();
                    const auto y = c.getY() + c.getHeight() * (float) (hi - juce::jlimit (lo, hi, value)) / (float) (hi - lo);
                    const auto column = juce::Rectangle<float> (x, (float) c.getY(), w, (float) c.getHeight()).reduced (1.0f, 0.0f);
                    g.setColour ((k / 4 % 2 == 0 ? theme::colour::bg4 : theme::colour::bg3).withMultipliedAlpha (alpha * 0.6f));
                    g.fillRect (column);
                    auto bar = juce::Rectangle<float>::leftTopRightBottom (column.getX(), std::min (y, zero), column.getRight(), std::max (y, zero));
                    g.setColour (colour.withMultipliedAlpha (alpha * (silent ? 0.4f : 1.0f)));
                    g.fillRect (bar.getHeight() < 2.0f ? bar.withHeight (2.0f).withY (zero - 1.0f) : bar);
                    g.setColour (juce::Colours::white.withAlpha (alpha));
                    g.drawText (owner->text (layer(), step), column.withHeight (16.0f).withY (std::min (y, zero) - (std::min (y, zero) > c.getY() + 16 ? 16.0f : 0.0f)),
                                juce::Justification::centred);
                }
            }
            // row labels / scale
            g.setColour (theme::colour::textSecondary);
            if (! bars())
            {
                const auto h = c.getHeight() / (float) rows();
                for (int r = 0; r < rows(); ++r)
                    g.drawText (rowName (r), juce::Rectangle<float> (0.0f, c.getY() + r * h, labelWidth - 8.0f, h), juce::Justification::centredRight);
            }
            else
                for (const auto v : { barMax(), (barMin() + barMax()) / 2, barMin() })
                {
                    const auto y = c.getY() + c.getHeight() * (float) (barMax() - v) / (float) (barMax() - barMin());
                    g.drawText (juce::String (v), juce::Rectangle<float> (0.0f, y - 7.0f, labelWidth - 8.0f, 14.0f), juce::Justification::centredRight);
                }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            lastStep = -1;
            if (! bars())
            {
                const auto step = stepAt (e.x);
                const auto row = rowAt (e.y);
                const auto value = (int) owner->settings.current().lanes[(std::size_t) laneOf (layer())][(std::size_t) step];
                painting = ! rowSet (value, row); // (a poly lane: the click toggles that row, a drag paints the same)
                paintRow = row;
            }
            mouseDrag (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const auto step = stepAt (e.x);
            const auto lane = laneOf (layer());
            const auto right = e.mods.isPopupMenu();
            if (! bars())
            {
                if (step == lastStep)
                    return;
                lastStep = step;
                const auto l = layer();
                const auto row = paintRow;
                const auto set = painting;
                const auto poly = (l == noteSelect && owner->settings.get (S::polyKeys) != 0) || (l == octave && owner->settings.get (S::polyOctave) != 0);
                owner->change ([=] (S& s) {
                    auto& v = s.current().lanes[(std::size_t) lane][(std::size_t) step];
                    if (l == noteSelect || l == octave)
                    {
                        const auto bit = 6 - row;
                        if (right)
                            v = (std::int8_t) (l == octave ? 0x08 : 0); // (right-click: no key / octave 0)
                        else if (poly)
                            v = (std::int8_t) (set ? (v | (1 << bit)) : (v & ~(1 << bit)));
                        else
                            v = (std::int8_t) (1 << bit);
                    }
                    else
                        v = (std::int8_t) (right ? (l == stepType ? S::normal : 0) : 5 - row);
                });
                return;
            }
            const auto c = cells();
            const auto lo = barMin(), hi = barMax();
            const auto value = right ? (layer() == velocity ? 96 : 0)
                                     : juce::jlimit (lo, hi, (int) std::lround (hi - (e.y - c.getY()) * (double) (hi - lo) / std::max (1, c.getHeight())));
            owner->change ([=] (S& s) { s.current().lanes[(std::size_t) lane][(std::size_t) step] = (std::int8_t) value; });
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            // the wheel moves a step's value (FL: moves the note up / down)
            const auto step = stepAt (e.x);
            const auto lane = laneOf (layer());
            const auto delta = w.deltaY > 0 ? 1 : w.deltaY < 0 ? -1 : 0;
            if (delta == 0)
                return;
            const auto l = layer();
            owner->change ([=] (S& s) {
                auto& v = s.current().lanes[(std::size_t) lane][(std::size_t) step];
                if (l == noteSelect || l == octave) // (shift the bits one row)
                {
                    const auto shifted = delta > 0 ? (v << 1) : ((std::uint8_t) v >> 1);
                    if ((shifted & 0x7f) != 0)
                        v = (std::int8_t) (shifted & 0x7f);
                }
                else
                    s.set (0x80 + lane * 64 + step, v + delta);
            });
        }

        int lastStep = -1, paintRow = 0;
        bool painting = true;
    };

    // one lane in brief: its values on the page; a click shows it in the big grid
    struct LaneRow final : juce::Component
    {
        SequencerBody* owner = nullptr;
        int layer = 0;
        void paint (juce::Graphics& g) override
        {
            const auto steps = owner->settings.get (S::steps);
            const auto w = getWidth() / 16.0f;
            const auto colour = laneColour (layer);
            g.setFont (theme::font (theme::type::caption));
            for (int k = 0; k < 16; ++k)
            {
                const auto step = owner->firstStep() + k;
                const auto cell = juce::Rectangle<float> (k * w, 0.0f, w, (float) getHeight()).reduced (1.0f, 1.0f);
                const auto alpha = step < steps ? 1.0f : 0.3f;
                g.setColour (colour.withMultipliedAlpha (alpha * (layer == owner->shown ? 1.0f : 0.75f)));
                g.fillRect (cell);
                g.setColour (juce::Colours::black.withAlpha (0.8f * alpha));
                g.drawText (owner->text (layer, step), cell, juce::Justification::centred);
            }
        }
        void mouseDown (const juce::MouseEvent&) override { owner->showLayer (layer); }
    };

private:
    void showLayer (int l)
    {
        shown = l;
        for (int k = 0; k < layerCount; ++k)
            laneButtons[(std::size_t) k].setToggleState (k == shown, juce::dontSendNotification);
        grid.repaint();
        for (auto& r : rows)
            r.repaint();
    }

    void change (std::function<void (S&)> f)
    {
        if (edit)
            edit ([f] (std::vector<std::uint8_t>& fl) {
                auto s = vfx::parseSequencer (fl).value_or (vfx::defaultSequencer());
                f (s);
                fl = vfx::writeSequencer (s);
            });
    }

    void toggle (juce::ToggleButton& b, const juce::String& text, int param, const juce::String& hint)
    {
        b.setButtonText (text);
        b.onClick = [this, &b, param] { change ([param, on = b.getToggleState()] (S& s) { s.set (param, on ? 1 : 0); }); };
        ui::setHint (b, hint);
        addAndMakeVisible (b);
        toggles.push_back ({ &b, param });
    }

    void combo (juce::ComboBox& c, const juce::String& name, int param, const juce::StringArray& items)
    {
        c.setName (name);
        c.addItemList (items, 1);
        c.onChange = [this, &c, param] { change ([param, v = c.getSelectedItemIndex() + S::minimum (param)] (S& s) { s.set (param, v); }); };
        ui::setHint (c, name);
        addAndMakeVisible (c);
        combos.push_back ({ &c, param });
    }

    void box (std::unique_ptr<Knob>& k, const juce::String& name, int param, std::function<juce::String (double)> text = nullptr)
    {
        k = std::make_unique<Knob> (intSpec (name, S::minimum (param), S::maximum (param), defaultOf (param), std::move (text)),
                                    [this, param] { return (double) settings.get (param); },
                                    [this, param] (double v) { change ([param, v] (S& s) { s.set (param, (int) std::lround (v)); }); });
        k->caption = name;
        k->box = true;
        addAndMakeVisible (*k);
        boxes.push_back (k.get());
    }

    static int defaultOf (int param)
    {
        static const auto blank = [] {
            auto s = vfx::defaultSequencer();
            s.set (S::currentProgram, 0);
            return s;
        }();
        return blank.get (param);
    }

    void showProgramMenu()
    {
        juce::PopupMenu menu;
        for (int group = 0; group < (int) settings.programs.size(); group += 16)
        {
            juce::PopupMenu sub;
            for (int p = group; p < std::min (group + 16, (int) settings.programs.size()); ++p)
                sub.addItem (juce::String (p + 1).paddedLeft ('0', 3) + "  " + juce::String (settings.programs[(std::size_t) p].nameText()), true,
                             p == settings.program(), [this, p] { change ([p] (S& s) { s.set (S::currentProgram, p); }); });
            menu.addSubMenu (juce::String (group + 1) + "-" + juce::String (group + 16), sub, true, nullptr, settings.program() / 16 == group / 16);
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&program));
    }

    void stepProgram (int delta)
    {
        const auto n = std::max (1, (int) settings.programs.size());
        change ([delta, n] (S& s) { s.set (S::currentProgram, (s.program() + delta + n) % n); });
    }

    S settings = vfx::defaultSequencer();
    int shown = noteSelect;
    juce::TextButton program, prevProgram { "<" }, nextProgram { ">" }, dice { "?" }, shiftLeft { "<<" }, shiftRight { ">>" };
    std::array<juce::TextButton, 4> pages;
    std::array<juce::TextButton, layerCount> laneButtons;
    juce::ToggleButton pageLock, latch, polyOctave, polyKeys, gateLaneMode;
    juce::ComboBox arpMode, programMode, order, missing, missingOctave, quantize, sync, restart, velocityMode, forceKey, forceScale, forceMode;
    std::unique_ptr<Knob> steps, gatePercent, swing, fixedKey, smartBend, transpose, octaves, randomVelocity, randomGate, randomStart, outLow, outHigh;
    std::vector<std::pair<juce::ToggleButton*, int>> toggles;
    std::vector<std::pair<juce::ComboBox*, int>> combos;
    std::vector<Knob*> boxes;
    std::array<int, 5> sectionTops {};
    Grid grid;
    std::array<LaneRow, layerCount> rows;
};

// ---------------------------------------------------------------------------------------------------------------
// VFX Script: the script and the values of its controls, kept as FL saved them (LYNX does not run Python)

class ScriptBody final : public Body
{
public:
    ScriptBody()
    {
        for (auto* e : { &source, &values })
        {
            e->setMultiLine (true, false);
            e->setReturnKeyStartsNewLine (true);
            e->setTabKeyUsedAsCharacter (true);
            e->setScrollbarsShown (true);
            e->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
            addAndMakeVisible (e);
        }
        values.setReadOnly (true);
        source.onTextChange = [this] { save.setEnabled (source.getText().replace ("\r\n", "\n").toStdString() != loaded); };
        save.setButtonText (tr ("Save the script"));
        save.setEnabled (false);
        save.onClick = [this] {
            const auto text = source.getText().replace ("\r\n", "\n").toStdString();
            if (edit)
                edit ([text] (std::vector<std::uint8_t>& fl) {
                    auto s = vfx::parseScript (fl).value_or (vfx::defaultScript());
                    s.setSource (text);
                    fl = vfx::writeScript (s);
                });
        };
        ui::setHint (save, tr ("Keeps the edited script in the project (FL Studio runs it when the project opens there)"));
        addAndMakeVisible (save);
    }

    void refresh (const std::vector<std::uint8_t>& fl) override
    {
        const auto s = vfx::parseScript (fl).value_or (vfx::defaultScript());
        const auto text = s.source();
        if (text != loaded) // (a pending edit stays while other things change)
        {
            loaded = text;
            source.setText (juce::String::fromUTF8 (text.c_str()), juce::dontSendNotification);
        }
        save.setEnabled (source.getText().replace ("\r\n", "\n").toStdString() != loaded);
        juce::String list;
        for (const auto& [name, value] : s.parameters())
            list << juce::String::fromUTF8 (name.c_str()) << " = " << juce::String::fromUTF8 (value.c_str()) << "\n";
        if (s.hasSurface())
            list << "\n" << tr ("(with an embedded Control Surface)");
        values.setText (list.isEmpty() ? tr ("(no saved control values)") : list, juce::dontSendNotification);
    }

    int preferredWidth() const override { return 900; }
    int preferredHeight() const override { return 520; }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        area.removeFromTop (40);
        auto bottom = area.removeFromBottom (30);
        save.setBounds (bottom.removeFromRight (160).withTrimmedTop (4));
        auto right = area.removeFromRight (260);
        right.removeFromTop (18);
        values.setBounds (right);
        area.removeFromRight (10);
        area.removeFromTop (18);
        source.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().reduced (12, 6);
        g.setColour (theme::colour::warning);
        g.setFont (theme::font (theme::type::body));
        g.drawFittedText (tr ("LYNX keeps the script and its settings but does not run Python scripts yet: the notes pass through unchanged."),
                          area.removeFromTop (36), juce::Justification::centredLeft, 2);
        g.setFont (theme::font (theme::type::caption));
        g.setColour (theme::colour::textSecondary);
        const auto top = area.removeFromTop (18);
        g.drawText (theme::caps (tr ("Script")), top, juce::Justification::centredLeft);
        g.drawText (theme::caps (tr ("Control values")), top.withTrimmedLeft (top.getWidth() - 260), juce::Justification::centredLeft);
    }

private:
    juce::TextEditor source, values;
    juce::TextButton save;
    std::string loaded;
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
        case vfx::Kind::levelScaler: return std::make_unique<LevelScalerBody>();
        case vfx::Kind::keyboardSplitter: return std::make_unique<KeyboardSplitterBody>();
        case vfx::Kind::envelope: return std::make_unique<EnvelopeBody>();
        case vfx::Kind::sequencer: return std::make_unique<SequencerBody>();
        case vfx::Kind::script: return std::make_unique<ScriptBody>();
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
            // the Sequencer's factory programs are one bank: the state of FL's plugin database entry
            const auto bank = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin database/Effects/Patcher/VFX Sequencer.fst");
            if (kind == vfx::Kind::sequencer && bank.existsAsFile())
                out.push_back ({ "FL Studio", tr ("Factory bank"), {}, bank });
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
