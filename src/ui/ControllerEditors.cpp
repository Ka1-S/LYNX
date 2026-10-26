#include "ui/WindowOwner.h"
#include "ui/ControllerEditors.h"

#include <array>
#include <map>

#include "core/FlSynths.h"
#include "core/FormulaController.h"
#include "core/MidiControls.h"
#include "engine/Text.h"
#include "ui/AutomationUi.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"

namespace lynx
{
namespace
{
juce::String nameOf (const std::string& s)
{
    return juce::String::fromUTF8 (s.c_str());
}

// A small rotary control with LYNX's control contract (drag, Ctrl = fine, wheel, Alt / middle-click reset, right-click
// menu with automation and controller links when `spec.target` is set).
class Knob final : public juce::Component
{
public:
    Knob (AudioEngine* e, ui::ValueSpec s, std::function<double()> get, std::function<void (double)> set)
        : engine (e), spec (std::move (s)), getter (std::move (get)), setter (std::move (set))
    {
        setRepaintsOnMouseActivity (true);
        ui::setHint (*this, spec.name, true, spec.target.has_value());
    }

    void paint (juce::Graphics& g) override
    {
        const auto v = juce::jlimit (0.0, 1.0, (getter() - spec.minimum) / juce::jmax (1e-9, spec.maximum - spec.minimum));
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
        g.setColour (theme::colour::bg6);
        g.strokePath (track, juce::PathStrokeType (3.0f));
        arc.addCentredArc (box.getCentreX(), box.getCentreY(), radius, radius, 0.0f, -sweep * 0.5f, -sweep * 0.5f + sweep * (float) v, true);
        g.setColour (isMouseOverOrDragging() ? theme::colour::accent : theme::colour::knobArc);
        g.strokePath (arc, juce::PathStrokeType (3.0f));

        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (spec.text (getter()), valueArea, juce::Justification::centred);
        g.setColour (theme::colour::textSecondary);
        g.drawText (shortName.isNotEmpty() ? shortName : spec.name, label, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, engine, spec, getter(), setter);
            return;
        }
        if (ui::isResetClick (e))
        {
            setter (spec.defaultValue);
            repaint();
            return;
        }
        drag.begin (e, getter());
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isActive())
            return;
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
        setter (spec.defaultValue);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (const auto v = ui::wheelValue (getter(), e, wheel, spec))
        {
            setter (*v);
            ui::flashValueTip (*this, getLocalBounds(), spec.text (getter()));
            repaint();
        }
    }

    juce::String shortName;

private:
    AudioEngine* engine;
    ui::ValueSpec spec;
    std::function<double()> getter;
    std::function<void (double)> setter;
    ui::ValueDrag drag;
};

ui::ValueSpec unitSpec (const juce::String& name, double def, std::optional<AutomationTarget> target)
{
    ui::ValueSpec s;
    s.name = name;
    s.defaultValue = def;
    s.toText = [] (double v) { return juce::String (v, 3); };
    s.fromText = [] (const juce::String& t) -> std::optional<double> { return juce::jlimit (0.0, 1.0, t.getDoubleValue()); };
    s.target = target;
    return s;
}

// The history of a control output, drawn as FL's controller monitors do.
class Monitor final : public juce::Component
{
public:
    void push (float v)
    {
        history[(std::size_t) head] = v;
        head = (head + 1) % (int) history.size();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (theme::colour::border);
        for (auto y : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * y), area.getX(), area.getRight());
        juce::Path p;
        const auto n = (int) history.size();
        for (int i = 0; i < n; ++i)
        {
            const auto v = history[(std::size_t) ((head + i) % n)];
            const auto x = area.getX() + area.getWidth() * (float) i / (float) (n - 1);
            const auto y = area.getBottom() - 2.0f - (area.getHeight() - 4.0f) * juce::jlimit (0.0f, 1.0f, v);
            if (i == 0)
                p.startNewSubPath (x, y);
            else
                p.lineTo (x, y);
        }
        g.setColour (theme::colour::automation);
        g.strokePath (p, juce::PathStrokeType (1.6f));
        const auto last = history[(std::size_t) ((head + n - 1) % n)];
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (juce::String (last, 3), area.reduced (6.0f, 3.0f), juce::Justification::topRight);
    }

private:
    std::array<float, 240> history {};
    int head = 0;
};

// ===================================================================================================
// Formula Controller
// ===================================================================================================

struct FormulaExample
{
    const char* name;
    const char* formula;
};

// waveforms built from the functions (the plugin's manual lists these shapes as examples of its language)
constexpr FormulaExample formulaExamples[] = {
    { "Sum of the knobs", "a+b+c" },
    { "Sine LFO (a = depth, b = speed, c = offset)", "a*Sin(SongTime*Pi*b*4)+c" },
    { "Triangle LFO", "a*ArcCos(Cos(SongTime*Pi*2*b))/Pi+c" },
    { "Saw LFO", "a*Frac(SongTime*b*2)+c" },
    { "Square LFO", "a*Round(0.5*Sin(SongTime*Pi*2*b)+0.5)+c" },
    { "Sample and hold (a = rate)", "b*SeededRand(Int(SongTime*(1+a*15)))+c" },
    { "Random", "a+(Rand-0.5)*b" },
    { "Range: a mapped into b..c", "b+a*(c-b)" },
    { "Invert a", "1-a" },
    { "Curve (b = tension)", "Tension(a,(b-0.5)*100)" },
    { "Steps (b = count)", "Round(a*(b*16+1))/(b*16+1)" },
    { "Switch: a when c is off, b when on", "Case(Round(1-c),a,b)" },
};

class FormulaEditor final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    FormulaEditor (AudioEngine& e, int t, int s) : engine (e), track (t), slot (s)
    {
        for (int k = 0; k < 3; ++k)
        {
            const juce::String name = juce::String::charToString ((juce::juce_wchar) ('a' + k));
            auto knob = std::make_unique<Knob> (
                &engine, unitSpec (name, 0.0, AutomationTarget { AutomationTargetKind::effectParam, track, slot, k }),
                [this, k] { const auto* fx = current(); return fx != nullptr ? (double) fx->params[(std::size_t) k] : 0.0; },
                [this, k] (double v) {
                    engine.editSong ([this, k, v] (Song& song) {
                        if (auto* fx = find (song))
                            fx->params[(std::size_t) k] = (float) juce::jlimit (0.0, 1.0, v);
                    });
                });
            knob->shortName = name;
            addAndMakeVisible (*knob);
            knobs[(std::size_t) k] = std::move (knob);
        }

        formula.setFont (juce::Font (theme::monoFont (14.0f)));
        formula.setColour (juce::TextEditor::backgroundColourId, theme::colour::bg0);
        formula.setColour (juce::TextEditor::textColourId, theme::colour::textPrimary);
        formula.setColour (juce::TextEditor::outlineColourId, theme::colour::borderStrong);
        formula.setColour (juce::TextEditor::focusedOutlineColourId, theme::colour::accent);
        formula.setInputRestrictions (255);
        formula.onReturnKey = [this] { compile(); };
        formula.onTextChange = [this] { setStatus (tr ("Not compiled yet (Enter or Compile)"), theme::colour::warning); };
        ui::setHint (formula, tr ("The formula: a, b, c (the knobs), SongTime (quarter notes), Pi, functions such as Sin, Round, Frac, If... Enter compiles"));
        addAndMakeVisible (formula);

        compileButton.setButtonText (tr ("Compile"));
        compileButton.onClick = [this] { compile(); };
        addAndMakeVisible (compileButton);

        examples.setButtonText (tr ("Examples"));
        examples.onClick = [this] {
            juce::PopupMenu menu;
            for (const auto& ex : formulaExamples)
                menu.addItem (tr (ex.name) + "   " + ex.formula, [this, f = juce::String (ex.formula)] {
                    formula.setText (f, false);
                    compile();
                });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&examples));
        };
        addAndMakeVisible (examples);

        help.setText (tr ("Operators: + - * / ^ ( )  = <> < > <= >=  and or xor not  div mod  %  !\n"
                          "Functions: Sin Cos Tan ArcTg ArcCos Sqrt Sqr Abs Exp Ln Log10 Log2 LogN Power Min Max Round Int Frac Ceil Floor Trunc\n"
                          "If(a,b,c) Case(a,b,c) IfE IfL IfG IfLE IfGE Inter Map Fold Warp Tension Rand SeededRand(x) SongTime GetTempo Time"),
                      juce::dontSendNotification);
        help.setFont (theme::font (theme::type::caption));
        help.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        help.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (help);

        status.setFont (theme::font (theme::type::small, true));
        addAndMakeVisible (status);
        addAndMakeVisible (monitor);

        if (const auto* fx = current())
            formula.setText (utf8 (formula::formulaOf (fx->state).c_str()), false);
        compile (false);
        engine.addChangeListener (this);
        startTimerHz (30);
        setSize (600, 330);
    }

    ~FormulaEditor() override { engine.removeChangeListener (this); }

    const EffectSlot* current() const
    {
        const auto& mixer = engine.getSong().mixer;
        if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
            return nullptr;
        const auto& fx = mixer[(std::size_t) track].effects[(std::size_t) slot];
        return fx.kind == EffectKind::formulaController ? &fx : nullptr;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::title, true));
        g.drawText (tr ("Formula Controller"), 14, 8, 300, 20, juce::Justification::centredLeft);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (tr ("Output 0..1 - right-click any knob > Link to internal controller"), 200, 8, getWidth() - 214, 20,
                    juce::Justification::centredRight);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14).withTrimmedTop (24);
        auto top = area.removeFromTop (92);
        for (auto& k : knobs)
            k->setBounds (top.removeFromLeft (72));
        top.removeFromLeft (10);
        monitor.setBounds (top);
        area.removeFromTop (10);
        auto row = area.removeFromTop (28);
        examples.setBounds (row.removeFromRight (90));
        row.removeFromRight (6);
        compileButton.setBounds (row.removeFromRight (80));
        row.removeFromRight (6);
        formula.setBounds (row);
        area.removeFromTop (4);
        status.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        help.setBounds (area);
    }

private:
    EffectSlot* find (Song& song) const
    {
        if (track < 0 || track >= (int) song.mixer.size() || slot < 0 || slot >= (int) song.mixer[(std::size_t) track].effects.size())
            return nullptr;
        auto& fx = song.mixer[(std::size_t) track].effects[(std::size_t) slot];
        return fx.kind == EffectKind::formulaController ? &fx : nullptr;
    }

    void setStatus (const juce::String& text, juce::Colour colour)
    {
        status.setText (text, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, colour);
    }

    void compile (bool store = true)
    {
        std::string error;
        const auto text = formula.getText().toStdString();
        if (formula::Program::compile (text, &error) == nullptr)
        {
            setStatus (tr ("Error: {0}", { utf8 (error.c_str()) }), theme::colour::meterClip);
            return;
        }
        setStatus (tr ("Compiled OK"), theme::colour::ledOn);
        if (! store)
            return;
        engine.editSong ([this, text] (Song& song) {
            if (auto* fx = find (song))
            {
                auto state = formula::parseState (fx->state).value_or (formula::State {});
                if (state.formula == text && ! fx->state.empty())
                    return;
                state.formula = text;
                state.a = fx->params[0];
                state.b = fx->params[1];
                state.c = fx->params[2];
                fx->state = formula::writeState (state);
            }
        });
    }

    void timerCallback() override
    {
        if (auto fx = engine.getEffect (track, slot))
            monitor.push (fx->controlOutput (0));
        const auto mouse = juce::Desktop::getMousePosition();
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            formula::setMousePosition ((mouse.x - display->totalArea.getX()) / (double) juce::jmax (1, display->totalArea.getWidth()),
                                       (mouse.y - display->totalArea.getY()) / (double) juce::jmax (1, display->totalArea.getHeight()));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        for (auto& k : knobs)
            k->repaint();
    }

    AudioEngine& engine;
    int track, slot;
    std::array<std::unique_ptr<Knob>, 3> knobs;
    juce::TextEditor formula;
    juce::TextButton compileButton, examples;
    juce::Label status, help;
    Monitor monitor;
};

class FormulaWindow final : public juce::DocumentWindow
{
public:
    FormulaWindow (AudioEngine& engine, int track, int slot, const juce::String& title)
        : DocumentWindow (title, theme::colour::bg2, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new FormulaEditor (engine, track, slot), true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
    }

    void closeButtonPressed() override;
};

std::map<std::pair<int, int>, std::unique_ptr<FormulaWindow>>& formulaWindows()
{
    static std::map<std::pair<int, int>, std::unique_ptr<FormulaWindow>> open;
    return open;
}

void FormulaWindow::closeButtonPressed()
{
    for (auto& [key, window] : formulaWindows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { formulaWindows().erase (k); });
            return;
        }
}

// ===================================================================================================
// Envelope Controller
// ===================================================================================================

// One articulator's envelope: drag points, double-click to add one, right-click a point for its menu (delete,
// sustain point, curve), Alt+drag a segment to bend it.
class EnvelopeGraph final : public juce::Component
{
public:
    std::function<ControllerArticulator()> get;
    std::function<void (const ControllerArticulator&)> set;
    std::function<float()> live; // the output now

    void paint (juce::Graphics& g) override
    {
        const auto a = get();
        const auto area = plot();
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (theme::colour::border);
        for (auto y : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * y), area.getX(), area.getRight());
        const auto span = timeSpan (a);
        const auto step = span <= 2.0f ? 0.25f : span <= 8.0f ? 1.0f : 4.0f;
        g.setFont (theme::font (theme::type::caption));
        for (float t = step; t < span - 1.0e-4f; t += step)
        {
            const auto x = toX (a, t);
            g.setColour (theme::colour::border);
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.setColour (theme::colour::textDisabled);
            g.drawText (juce::String (t, step < 1.0f ? 2 : 0) + (a.tempo ? tr (" beat") : " s"), (int) x + 2, (int) area.getBottom() - 14, 60, 12,
                        juce::Justification::centredLeft);
        }
        if (! a.envOn || a.count == 0)
        {
            g.setColour (theme::colour::textDisabled);
            g.drawText (tr ("Envelope off: the output rests at its base"), area, juce::Justification::centred);
        }
        juce::Path p;
        for (int i = 0; i < a.count; ++i)
        {
            const auto x = toX (a, a.time[(std::size_t) i]);
            const auto y = toY (a.level[(std::size_t) i]);
            if (i == 0)
            {
                p.startNewSubPath (x, y);
                continue;
            }
            // the segment with its curve
            const auto x0 = toX (a, a.time[(std::size_t) i - 1]);
            const auto y0 = toY (a.level[(std::size_t) i - 1]);
            const auto k = a.tension[(std::size_t) i - 1];
            for (int step = 1; step <= 24; ++step)
            {
                auto f = (float) step / 24.0f;
                const auto shaped = k != 0.0f ? std::pow (f, std::pow (2.0f, k * 3.0f)) : f;
                p.lineTo (x0 + (x - x0) * f, y0 + (y - y0) * shaped);
            }
        }
        g.setColour (a.envOn ? theme::colour::automation : theme::colour::textDisabled);
        g.strokePath (p, juce::PathStrokeType (2.0f));
        for (int i = 0; i < a.count; ++i)
        {
            const auto c = juce::Point<float> (toX (a, a.time[(std::size_t) i]), toY (a.level[(std::size_t) i]));
            g.setColour (i == a.sustain ? theme::colour::accent : theme::colour::textPrimary);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (c));
            if (i == a.sustain)
            {
                g.setColour (theme::colour::accent.withAlpha (0.6f));
                g.drawVerticalLine ((int) c.x, area.getY(), area.getBottom());
            }
        }
        if (live)
        {
            g.setColour (theme::colour::accent.withAlpha (0.7f));
            const auto y = toY (live());
            g.drawHorizontalLine ((int) y, area.getRight() - 14.0f, area.getRight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto a = get();
        dragged = hit (a, e.position);
        if (e.mods.isPopupMenu())
        {
            pointMenu (dragged, e.position);
            dragged = -1;
            return;
        }
        if (e.mods.isAltDown() && dragged < 0)
        {
            bendSegment = segmentAt (a, e.position.x);
            bendStartY = e.position.y;
            bendStart = bendSegment >= 0 ? a.tension[(std::size_t) bendSegment] : 0.0f;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto a = get();
        if (bendSegment >= 0 && bendSegment < a.count - 1)
        {
            a.tension[(std::size_t) bendSegment] = juce::jlimit (-1.0f, 1.0f, bendStart + (e.position.y - bendStartY) / 120.0f);
            set (a);
            return;
        }
        if (dragged < 0 || dragged >= a.count)
            return;
        const auto lo = dragged > 0 ? a.time[(std::size_t) dragged - 1] : 0.0f;
        const auto hi = dragged + 1 < a.count ? a.time[(std::size_t) dragged + 1] : 1.0e6f;
        a.time[(std::size_t) dragged] = dragged == 0 ? 0.0f : juce::jlimit (lo, hi, fromX (a, e.position.x));
        a.level[(std::size_t) dragged] = juce::jlimit (0.0f, 1.0f, fromY (e.position.y));
        a.envOn = true;
        set (a);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragged = -1;
        bendSegment = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto a = get();
        if (hit (a, e.position) >= 0 || a.count >= (int) a.time.size())
            return;
        const auto t = juce::jmax (0.0f, fromX (a, e.position.x));
        int at = a.count;
        for (int i = 0; i < a.count; ++i)
            if (t < a.time[(std::size_t) i])
            {
                at = i;
                break;
            }
        for (int i = a.count; i > at; --i)
        {
            a.time[(std::size_t) i] = a.time[(std::size_t) i - 1];
            a.level[(std::size_t) i] = a.level[(std::size_t) i - 1];
            a.tension[(std::size_t) i] = a.tension[(std::size_t) i - 1];
        }
        a.time[(std::size_t) at] = a.count == 0 ? 0.0f : t;
        a.level[(std::size_t) at] = juce::jlimit (0.0f, 1.0f, fromY (e.position.y));
        a.tension[(std::size_t) at] = 0.0f;
        ++a.count;
        if (a.sustain >= at)
            ++a.sustain;
        a.envOn = true;
        set (a);
    }

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (10.0f, 8.0f); }
    static float timeSpan (const ControllerArticulator& a)
    {
        const auto last = a.count > 0 ? a.time[(std::size_t) a.count - 1] : 1.0f;
        return juce::jmax (1.0f, std::ceil (last * 1.25f));
    }
    float toX (const ControllerArticulator& a, float t) const { return plot().getX() + plot().getWidth() * t / timeSpan (a); }
    float fromX (const ControllerArticulator& a, float x) const { return (x - plot().getX()) / plot().getWidth() * timeSpan (a); }
    float toY (float v) const { return plot().getBottom() - plot().getHeight() * v; }
    float fromY (float y) const { return (plot().getBottom() - y) / plot().getHeight(); }

    int hit (const ControllerArticulator& a, juce::Point<float> p) const
    {
        for (int i = 0; i < a.count; ++i)
            if (p.getDistanceFrom ({ toX (a, a.time[(std::size_t) i]), toY (a.level[(std::size_t) i]) }) < 7.0f)
                return i;
        return -1;
    }

    int segmentAt (const ControllerArticulator& a, float x) const
    {
        for (int i = 0; i + 1 < a.count; ++i)
            if (x >= toX (a, a.time[(std::size_t) i]) && x < toX (a, a.time[(std::size_t) i + 1]))
                return i;
        return -1;
    }

    void pointMenu (int index, juce::Point<float>)
    {
        juce::PopupMenu menu;
        const auto a = get();
        if (index >= 0)
        {
            menu.addItem (tr ("Delete point"), a.count > 1, false, [this, index] {
                auto v = get();
                for (int i = index; i + 1 < v.count; ++i)
                {
                    v.time[(std::size_t) i] = v.time[(std::size_t) i + 1];
                    v.level[(std::size_t) i] = v.level[(std::size_t) i + 1];
                    v.tension[(std::size_t) i] = v.tension[(std::size_t) i + 1];
                }
                --v.count;
                if (v.sustain == index)
                    v.sustain = -1;
                else if (v.sustain > index)
                    --v.sustain;
                if (v.count > 0)
                    v.time[0] = 0.0f;
                set (v);
            });
            menu.addItem (tr ("Sustain point (holds while the key is down)"), true, a.sustain == index, [this, index] {
                auto v = get();
                v.sustain = v.sustain == index ? -1 : index;
                set (v);
            });
            menu.addItem (tr ("Straight line to the next point"), index + 1 < a.count, false, [this, index] {
                auto v = get();
                v.tension[(std::size_t) index] = 0.0f;
                set (v);
            });
            menu.addSeparator();
        }
        menu.addItem (tr ("Double-click adds a point, drag moves it, Alt+drag bends a segment"), false, false, [] {});
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    int dragged = -1, bendSegment = -1;
    float bendStartY = 0.0f, bendStart = 0.0f;
};

class EnvelopeControllerEditor final : public juce::Component,
                                       private juce::Timer,
                                       private juce::ChangeListener
{
public:
    EnvelopeControllerEditor (AudioEngine& e, int c) : engine (e), channel (c)
    {
        for (int k = 0; k < 8; ++k)
        {
            auto& b = tabs[(std::size_t) k];
            b.setButtonText (juce::String (k + 1));
            b.setClickingTogglesState (false);
            b.onClick = [this, k] { select (k); };
            ui::setHint (b, tr ("Output {0}: link it from any knob (right-click > Link to internal controller)", { juce::String (k + 1) }));
            addAndMakeVisible (b);
        }

        graph.get = [this] { return articulator(); };
        graph.set = [this] (const ControllerArticulator& a) { store (a); };
        graph.live = [this] {
            const auto instrument = engine.getChannelInstrument (channel);
            return instrument != nullptr ? instrument->controlOutput (shown) : 0.0f;
        };
        addAndMakeVisible (graph);

        base = std::make_unique<Knob> (&engine, unitSpec (tr ("Base"), 0.0, std::nullopt), [this] { return (double) articulator().base; },
                                       [this] (double v) { auto a = articulator(); a.base = (float) v; store (a); });
        base->shortName = tr ("Base");
        ui::ValueSpec amountSpec;
        amountSpec.name = tr ("Envelope amount");
        amountSpec.minimum = -1.0;
        amountSpec.maximum = 1.0;
        amountSpec.defaultValue = 1.0;
        amountSpec.toText = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        amount = std::make_unique<Knob> (&engine, amountSpec, [this] { return (double) articulator().envLevel; },
                                         [this] (double v) { auto a = articulator(); a.envLevel = (float) v; store (a); });
        amount->shortName = tr ("Amount");
        addAndMakeVisible (*base);
        addAndMakeVisible (*amount);

        enabled.setButtonText (tr ("Envelope on"));
        enabled.onClick = [this] { auto a = articulator(); a.envOn = enabled.getToggleState(); store (a); };
        bipolar.setButtonText (tr ("Bipolar (centred on base)"));
        bipolar.onClick = [this] { auto a = articulator(); a.bipolar = bipolar.getToggleState(); store (a); };
        tempo.setButtonText (tr ("Time in beats"));
        tempo.onClick = [this] { auto a = articulator(); a.tempo = tempo.getToggleState(); store (a); };
        for (auto* b : { &enabled, &bipolar, &tempo })
            addAndMakeVisible (*b);

        for (auto* s : { &lowKey, &highKey })
        {
            s->setSliderStyle (juce::Slider::IncDecButtons);
            s->setRange (0, 127, 1);
            s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
            s->textFromValueFunction = [] (double v) { return juce::MidiMessage::getMidiNoteName ((int) v, true, true, 4); };
            s->updateText();
            addAndMakeVisible (*s);
        }
        highKey.textFromValueFunction = [] (double v) {
            return v >= 127.0 ? tr ("all above") : juce::MidiMessage::getMidiNoteName ((int) v, true, true, 4);
        };
        highKey.updateText();
        lowKey.onValueChange = [this] { auto a = articulator(); a.lowKey = (int) lowKey.getValue(); store (a); };
        // the top of the range means every key (FL saves 256)
        highKey.onValueChange = [this] { auto a = articulator(); a.highKey = highKey.getValue() >= 127.0 ? 256 : (int) highKey.getValue(); store (a); };
        keysLabel.setText (tr ("Keys"), juce::dontSendNotification);
        keysLabel.setFont (theme::font (theme::type::small));
        addAndMakeVisible (keysLabel);
        links.setFont (theme::font (theme::type::caption));
        links.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        links.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (links);

        engine.addChangeListener (this);
        refresh();
        startTimerHz (30);
        setSize (720, 420);
    }

    ~EnvelopeControllerEditor() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::title, true));
        g.drawText (tr ("Envelope Controller"), 14, 8, 220, 22, juce::Justification::centredLeft);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (tr ("Each note on this channel starts the envelopes; output k drives the knobs linked to it"), 230, 8,
                    getWidth() - 244, 22, juce::Justification::centredRight);
        // a meter per output
        for (int k = 0; k < 8; ++k)
        {
            const auto r = tabs[(std::size_t) k].getBounds().toFloat();
            const auto bar = juce::Rectangle<float> (r.getX() + 4.0f, r.getBottom() + 3.0f, r.getWidth() - 8.0f, 3.0f);
            g.setColour (theme::colour::bg0);
            g.fillRect (bar);
            g.setColour (theme::colour::automation);
            g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, outputs[(std::size_t) k])));
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14).withTrimmedTop (26);
        auto tabRow = area.removeFromTop (26);
        for (auto& b : tabs)
            b.setBounds (tabRow.removeFromLeft (44).reduced (2, 0));
        area.removeFromTop (10);
        auto side = area.removeFromRight (190);
        graph.setBounds (area);
        side.removeFromLeft (12);
        auto knobsRow = side.removeFromTop (88);
        base->setBounds (knobsRow.removeFromLeft (88));
        amount->setBounds (knobsRow.removeFromLeft (88));
        side.removeFromTop (6);
        enabled.setBounds (side.removeFromTop (24));
        bipolar.setBounds (side.removeFromTop (24));
        tempo.setBounds (side.removeFromTop (24));
        side.removeFromTop (8);
        keysLabel.setBounds (side.removeFromTop (18));
        lowKey.setBounds (side.removeFromTop (26));
        highKey.setBounds (side.removeFromTop (26));
        side.removeFromTop (8);
        links.setBounds (side);
    }

private:
    EnvelopeControllerData data() const
    {
        const auto& song = engine.getSong();
        if (channel < 0 || channel >= (int) song.channels.size())
            return defaultEnvelopeController();
        return parseEnvelopeController (song.channels[(std::size_t) channel].controllerState).value_or (defaultEnvelopeController());
    }

    ControllerArticulator articulator() const { return data()[(std::size_t) shown]; }

    void store (const ControllerArticulator& a)
    {
        auto d = data();
        d[(std::size_t) shown] = a;
        const auto bytes = writeEnvelopeController (d);
        engine.editSong ([this, bytes] (Song& song) {
            if (channel >= 0 && channel < (int) song.channels.size() && song.channels[(std::size_t) channel].instrument == InstrumentKind::controller)
                song.channels[(std::size_t) channel].controllerState = bytes;
        });
    }

    void select (int k)
    {
        shown = k;
        refresh();
    }

    void refresh()
    {
        const auto a = articulator();
        for (int k = 0; k < 8; ++k)
            tabs[(std::size_t) k].setColour (juce::TextButton::buttonColourId, k == shown ? theme::colour::accentMuted : theme::colour::bg4);
        enabled.setToggleState (a.envOn, juce::dontSendNotification);
        bipolar.setToggleState (a.bipolar, juce::dontSendNotification);
        tempo.setToggleState (a.tempo, juce::dontSendNotification);
        lowKey.setValue (juce::jlimit (0, 127, a.lowKey), juce::dontSendNotification);
        highKey.setValue (juce::jlimit (0, 127, a.highKey), juce::dontSendNotification);

        juce::StringArray targets;
        for (const auto& automation : engine.getSong().automations)
            if (automation.source.channel == channel && automation.source.output == shown && automation.target.kind != AutomationTargetKind::none)
                targets.add (describeAutomationTarget (engine, automation.target));
        links.setText (targets.isEmpty() ? tr ("Not linked yet. Right-click a knob > Link to internal controller > this output.")
                                         : tr ("Drives:") + "\n" + targets.joinIntoString ("\n"),
                       juce::dontSendNotification);
        graph.repaint();
        base->repaint();
        amount->repaint();
    }

    void timerCallback() override
    {
        const auto instrument = engine.getChannelInstrument (channel);
        for (int k = 0; k < 8; ++k)
            outputs[(std::size_t) k] = instrument != nullptr ? instrument->controlOutput (k) : 0.0f;
        repaint (getLocalBounds().withHeight (70));
        graph.repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    int channel, shown = 0;
    std::array<juce::TextButton, 8> tabs;
    EnvelopeGraph graph;
    std::unique_ptr<Knob> base, amount;
    juce::ToggleButton enabled, bipolar, tempo;
    juce::Slider lowKey, highKey;
    juce::Label keysLabel, links;
    std::array<float, 8> outputs {};
};

// ===================================================================================================
// MIDI Out
// ===================================================================================================

class MidiOutEditor final : public juce::Component,
                            private juce::ChangeListener
{
public:
    // `locate` finds the MIDI Out's ChannelDef in a song (a Channel Rack channel, or a node inside a Patcher: round
    // 162); `c` = its channel index, −1 inside a Patcher (no channel automation there)
    MidiOutEditor (AudioEngine& e, std::function<ChannelDef* (Song&)> l, int c) : engine (e), locate (std::move (l)), channel (c)
    {
        for (int m = 1; m <= 16; ++m)
            midiChannel.addItem (tr ("Channel {0}", { juce::String (m) }), m);
        midiChannel.onChange = [this] { edit ([v = midiChannel.getSelectedId() - 1] (midictl::MidiOutSettings& s) { s.channel = v; }); };
        port.setSliderStyle (juce::Slider::IncDecButtons);
        port.setRange (0, 255, 1);
        port.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 54, 22);
        port.onValueChange = [this] { edit ([v = (int) port.getValue()] (midictl::MidiOutSettings& s) { s.port = v; }); };
        program.setSliderStyle (juce::Slider::IncDecButtons);
        program.setRange (0, 128, 1);
        program.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 54, 22);
        program.textFromValueFunction = [] (double v) { return v < 0.5 ? tr ("none") : juce::String ((int) v); };
        program.updateText();
        program.onValueChange = [this] { edit ([v = (int) program.getValue()] (midictl::MidiOutSettings& s) { s.program = v; }); };
        for (auto* l : { &channelLabel, &portLabel, &programLabel })
        {
            l->setFont (theme::font (theme::type::small));
            addAndMakeVisible (*l);
        }
        channelLabel.setText (tr ("MIDI channel"), juce::dontSendNotification);
        portLabel.setText (tr ("Port"), juce::dontSendNotification);
        programLabel.setText (tr ("Program (sent on play)"), juce::dontSendNotification);
        addAndMakeVisible (midiChannel);
        addAndMakeVisible (port);
        addAndMakeVisible (program);

        connect.setButtonText (tr ("Send to a plugin..."));
        connect.onClick = [this] { showConnectMenu(); };
        ui::setHint (connect, tr ("Sets a plugin channel's MIDI input port to this port, so it plays this channel's notes"));
        addAndMakeVisible (connect);
        automate.setButtonText (tr ("Automate MIDI controller..."));
        automate.onClick = [this] {
            juce::PopupMenu menu;
            addMidiParameterItems (menu, engine, { AutomationTargetKind::channelParam, channel, 0, 0 });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&automate));
        };
        ui::setHint (automate, tr ("CCs, aftertouch and pitch bend this channel sends to the plugins on its port"));
        addAndMakeVisible (automate);
        automate.setVisible (channel >= 0);
        connect.setVisible (channel >= 0);
        listeners.setFont (theme::font (theme::type::small));
        listeners.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        listeners.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (listeners);

        engine.addChangeListener (this);
        refresh();
        setSize (520, 300);
    }

    ~MidiOutEditor() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::title, true));
        g.drawText (tr ("MIDI Out"), 14, 8, 200, 22, juce::Justification::centredLeft);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (tr ("Its notes play the plugins whose MIDI input port is this port"), 120, 8, getWidth() - 134, 22,
                    juce::Justification::centredRight);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14).withTrimmedTop (30);
        auto row = [&] (juce::Label& label, juce::Component& control) {
            auto r = area.removeFromTop (28);
            label.setBounds (r.removeFromLeft (170));
            control.setBounds (r.removeFromLeft (160));
            area.removeFromTop (6);
        };
        row (channelLabel, midiChannel);
        row (portLabel, port);
        row (programLabel, program);
        auto buttons = area.removeFromTop (28);
        connect.setBounds (buttons.removeFromLeft (180));
        buttons.removeFromLeft (8);
        automate.setBounds (buttons.removeFromLeft (220));
        area.removeFromTop (10);
        listeners.setBounds (area);
    }

private:
    midictl::MidiOutSettings settings() const
    {
        const auto* c = locate (const_cast<Song&> (engine.getSong()));
        return c != nullptr ? midictl::parseMidiOut (c->pluginState) : midictl::MidiOutSettings {};
    }

    void edit (std::function<void (midictl::MidiOutSettings&)> change)
    {
        engine.editSong ([this, change] (Song& song) {
            auto* c = locate (song);
            if (c == nullptr || c->instrument != InstrumentKind::midiOut)
                return;
            auto& state = c->pluginState;
            auto s = midictl::parseMidiOut (state);
            change (s);
            midictl::writeMidiOut (state, s);
        });
    }

    void showConnectMenu()
    {
        juce::PopupMenu menu;
        const auto& song = engine.getSong();
        const auto p = settings().port;
        for (int c = 0; c < (int) song.channels.size(); ++c)
            if (song.channels[(std::size_t) c].instrument == InstrumentKind::plugin)
                menu.addItem (nameOf (song.channels[(std::size_t) c].name), true, song.channels[(std::size_t) c].plugin.midiInPort == p, [this, c, p] {
                    engine.editSong ([c, p] (Song& s) {
                        auto& port = s.channels[(std::size_t) c].plugin.midiInPort;
                        port = port == p ? -1 : p;
                    });
                });
        if (menu.getNumItems() == 0)
            menu.addItem (tr ("(no plugin channel in the Channel Rack)"), false, false, [] {});
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&connect));
    }

    void refresh()
    {
        const auto s = settings();
        midiChannel.setSelectedId (s.channel + 1, juce::dontSendNotification);
        port.setValue (s.port, juce::dontSendNotification);
        program.setValue (s.program, juce::dontSendNotification);
        if (channel < 0) // inside a Patcher (round 162)
        {
            listeners.setText (tr ("Inside a Patcher: cable its \"MIDI out\" pin to an instrument or to a \"MIDI port\" pin of \"To LYNX\"; "
                                   "its notes go out on this MIDI channel."),
                               juce::dontSendNotification);
            return;
        }
        juce::StringArray names;
        const auto& song = engine.getSong();
        for (const auto& c : song.channels)
            if (c.instrument == InstrumentKind::plugin && c.plugin.midiInPort == s.port)
                names.add (nameOf (c.name));
        listeners.setText (names.isEmpty() ? tr ("No plugin listens on port {0} yet: use \"Send to a plugin...\" or the plugin's Processing > MIDI input port.",
                                                 { juce::String (s.port) })
                                           : tr ("Plays: {0}", { names.joinIntoString (", ") }),
                           juce::dontSendNotification);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    std::function<ChannelDef* (Song&)> locate;
    int channel;
    juce::ComboBox midiChannel;
    juce::Slider port, program;
    juce::Label channelLabel, portLabel, programLabel, listeners;
    juce::TextButton connect, automate;
};
} // namespace

namespace FormulaControllerWindows
{
void open (AudioEngine& engine, int track, int slot)
{
    auto& open = formulaWindows();
    const auto key = std::make_pair (track, slot);
    if (auto it = open.find (key); it != open.end())
    {
        it->second->toFront (true);
        return;
    }
    const auto& mixer = engine.getSong().mixer;
    const auto name = track == 0 ? tr ("Master")
                                 : (track < (int) mixer.size() ? nameOf (mixer[(std::size_t) track].name) : juce::String());
    open[key] = std::make_unique<FormulaWindow> (engine, track, slot,
                                                 tr ("Formula Controller") + " - " + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) }))
                                                     + " (" + tr ("slot {0}", { juce::String (slot + 1) }) + ")");
}

void closeAll()
{
    formulaWindows().clear();
}
} // namespace FormulaControllerWindows

std::unique_ptr<juce::Component> makeEnvelopeControllerEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<EnvelopeControllerEditor> (engine, channel);
}

std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<MidiOutEditor> (engine, [channel] (Song& song) -> ChannelDef* {
        return channel >= 0 && channel < (int) song.channels.size() ? &song.channels[(std::size_t) channel] : nullptr;
    }, channel);
}

std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate)
{
    return std::make_unique<MidiOutEditor> (engine, std::move (locate), -1);
}
} // namespace lynx
