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

// One curve of an articulator (FL's Envelope Controller editor: the envelope, the LFO's amplitude, or a map): drag
// points, double-click to add one, right-click a point for its menu (delete, sustain point, curve), Alt+drag a
// segment to bend it. Time curves run in seconds / beats; maps run over their input 0..1.
class CurveGraph final : public juce::Component
{
public:
    enum class Axis { time, keys, unit };
    std::function<ControllerCurve()> get;
    std::function<void (const ControllerCurve&)> set;
    std::function<float()> live;           // the output now
    std::function<bool()> active;          // drawn in colour (an envelope / LFO that is on, a map that is used)
    std::function<juce::String()> emptyText; // shown over an inactive curve
    Axis axis = Axis::time;
    bool tempo = false;      // time axis in beats
    bool sustainable = true; // the point menu offers the sustain point
    float neutral = -1.0f;   // a map's neutral level (dashed line), < 0 = none
    std::function<float (float)> preview; // a faint wave behind the curve (the LFO page: -1 … 1 at time t)
    float minSpan = 1.0f;                 // the time axis shows at least this much (one LFO cycle)
    bool showPreview = false;

    void paint (juce::Graphics& g) override
    {
        const auto c = get();
        const auto area = plot();
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (theme::colour::border);
        for (auto y : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * y), area.getX(), area.getRight());
        g.setFont (theme::font (theme::type::caption));
        if (axis == Axis::time)
        {
            const auto span = timeSpan (c);
            const auto step = span <= 2.0f ? 0.25f : span <= 8.0f ? 1.0f : 4.0f;
            for (float t = step; t < span - 1.0e-4f; t += step)
                gridLine (g, toX (c, t), juce::String (t, step < 1.0f ? 2 : 0) + (tempo ? tr (" beat") : " s"));
        }
        else if (axis == Axis::keys)
        {
            for (int key = 12; key < 120; key += 12) // the C of each octave (x = key / 120)
                gridLine (g, toX (c, key / 120.0f), juce::MidiMessage::getMidiNoteName (key, true, true, 4));
        }
        else
            for (int i = 1; i < 4; ++i)
                gridLine (g, toX (c, i * 0.25f), juce::String (i * 25) + "%");
        if (neutral >= 0.0f)
        {
            const auto y = toY (neutral);
            juce::Path dash;
            dash.startNewSubPath (area.getX(), y);
            dash.lineTo (area.getRight(), y);
            const float pattern[] { 4.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dash, dash, pattern, 2);
            g.setColour (theme::colour::textDisabled);
            g.fillPath (dash);
        }
        const auto on = ! active || active();
        if (preview && showPreview && axis == Axis::time)
        {
            const auto span = timeSpan (c);
            juce::Path wave;
            for (int i = 0; i <= 400; ++i)
            {
                const auto t = span * (float) i / 400.0f;
                const auto y = toY (0.5f + 0.5f * juce::jlimit (-1.0f, 1.0f, preview (t)));
                if (i == 0)
                    wave.startNewSubPath (toX (c, t), y);
                else
                    wave.lineTo (toX (c, t), y);
            }
            g.setColour (theme::colour::accent.withAlpha (on ? 0.45f : 0.2f));
            g.strokePath (wave, juce::PathStrokeType (1.2f));
        }
        if (! on && emptyText)
        {
            g.setColour (theme::colour::textDisabled);
            g.drawText (emptyText(), area.reduced (8.0f), juce::Justification::centredTop);
        }
        juce::Path p;
        for (int i = 0; i < c.count; ++i)
        {
            const auto x = toX (c, c.time[(std::size_t) i]);
            const auto y = toY (c.level[(std::size_t) i]);
            if (i == 0)
            {
                if (axis != Axis::time && x > area.getX())
                {
                    p.startNewSubPath (area.getX(), y); // a map is flat before its first point
                    p.lineTo (x, y);
                }
                else
                    p.startNewSubPath (x, y);
                continue;
            }
            const auto x0 = toX (c, c.time[(std::size_t) i - 1]);
            const auto y0 = toY (c.level[(std::size_t) i - 1]);
            const auto k = c.tension[(std::size_t) i - 1];
            for (int step = 1; step <= 24; ++step)
            {
                auto f = (float) step / 24.0f;
                const auto shaped = k != 0.0f ? std::pow (f, std::pow (2.0f, k * 3.0f)) : f;
                p.lineTo (x0 + (x - x0) * f, y0 + (y - y0) * shaped);
            }
        }
        if (axis != Axis::time && c.count > 0)
            p.lineTo (area.getRight(), toY (c.level[(std::size_t) c.count - 1]));
        g.setColour (on ? theme::colour::automation : theme::colour::textDisabled);
        g.strokePath (p, juce::PathStrokeType (2.0f));
        for (int i = 0; i < c.count; ++i)
        {
            const auto pt = juce::Point<float> (toX (c, c.time[(std::size_t) i]), toY (c.level[(std::size_t) i]));
            g.setColour (i == c.sustain ? theme::colour::accent : theme::colour::textPrimary);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (pt));
            if (i == c.sustain)
            {
                g.setColour (theme::colour::accent.withAlpha (0.6f));
                g.drawVerticalLine ((int) pt.x, area.getY(), area.getBottom());
            }
        }
        if (live)
        {
            g.setColour (theme::colour::accent.withAlpha (0.7f));
            g.drawHorizontalLine ((int) toY (live()), area.getRight() - 14.0f, area.getRight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto c = get();
        dragged = hit (c, e.position);
        if (e.mods.isPopupMenu())
        {
            pointMenu (dragged);
            dragged = -1;
            return;
        }
        if (e.mods.isAltDown() && dragged < 0)
        {
            bendSegment = segmentAt (c, e.position.x);
            bendStartY = e.position.y;
            bendStart = bendSegment >= 0 ? c.tension[(std::size_t) bendSegment] : 0.0f;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto c = get();
        if (bendSegment >= 0 && bendSegment < c.count - 1)
        {
            c.tension[(std::size_t) bendSegment] = juce::jlimit (-1.0f, 1.0f, bendStart + (e.position.y - bendStartY) / 120.0f);
            set (c);
            return;
        }
        if (dragged < 0 || dragged >= c.count)
            return;
        const auto lo = dragged > 0 ? c.time[(std::size_t) dragged - 1] : 0.0f;
        const auto hi = dragged + 1 < c.count ? c.time[(std::size_t) dragged + 1] : (axis == Axis::time ? 1.0e6f : 1.0f);
        // the first point of a time curve stays at 0; a map's points move freely within 0..1
        c.time[(std::size_t) dragged] = dragged == 0 && axis == Axis::time ? 0.0f : juce::jlimit (lo, hi, fromX (c, e.position.x));
        c.level[(std::size_t) dragged] = juce::jlimit (0.0f, 1.0f, fromY (e.position.y));
        set (c);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragged = -1;
        bendSegment = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto c = get();
        if (hit (c, e.position) >= 0 || c.count >= (int) c.time.size())
            return;
        const auto limit = axis == Axis::time ? 1.0e6f : 1.0f;
        const auto t = juce::jlimit (0.0f, limit, fromX (c, e.position.x));
        int at = c.count;
        for (int i = 0; i < c.count; ++i)
            if (t < c.time[(std::size_t) i])
            {
                at = i;
                break;
            }
        for (int i = c.count; i > at; --i)
        {
            c.time[(std::size_t) i] = c.time[(std::size_t) i - 1];
            c.level[(std::size_t) i] = c.level[(std::size_t) i - 1];
            c.tension[(std::size_t) i] = c.tension[(std::size_t) i - 1];
        }
        c.time[(std::size_t) at] = c.count == 0 && axis == Axis::time ? 0.0f : t;
        c.level[(std::size_t) at] = juce::jlimit (0.0f, 1.0f, fromY (e.position.y));
        c.tension[(std::size_t) at] = 0.0f;
        ++c.count;
        if (c.sustain >= at)
            ++c.sustain;
        set (c);
    }

private:
    void gridLine (juce::Graphics& g, float x, const juce::String& label) const
    {
        const auto area = plot();
        g.setColour (theme::colour::border);
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (theme::colour::textDisabled);
        g.drawText (label, (int) x + 2, (int) area.getBottom() - 14, 60, 12, juce::Justification::centredLeft);
    }

    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (10.0f, 8.0f); }
    float timeSpan (const ControllerCurve& c) const
    {
        if (axis != Axis::time)
            return 1.0f;
        const auto last = c.count > 0 ? c.time[(std::size_t) c.count - 1] : 1.0f;
        return juce::jmax (minSpan, std::ceil (last * 1.25f));
    }
    float toX (const ControllerCurve& c, float t) const { return plot().getX() + plot().getWidth() * t / timeSpan (c); }
    float fromX (const ControllerCurve& c, float x) const { return (x - plot().getX()) / plot().getWidth() * timeSpan (c); }
    float toY (float v) const { return plot().getBottom() - plot().getHeight() * v; }
    float fromY (float y) const { return (plot().getBottom() - y) / plot().getHeight(); }

    int hit (const ControllerCurve& c, juce::Point<float> p) const
    {
        for (int i = 0; i < c.count; ++i)
            if (p.getDistanceFrom ({ toX (c, c.time[(std::size_t) i]), toY (c.level[(std::size_t) i]) }) < 7.0f)
                return i;
        return -1;
    }

    int segmentAt (const ControllerCurve& c, float x) const
    {
        for (int i = 0; i + 1 < c.count; ++i)
            if (x >= toX (c, c.time[(std::size_t) i]) && x < toX (c, c.time[(std::size_t) i + 1]))
                return i;
        return -1;
    }

    void pointMenu (int index)
    {
        juce::PopupMenu menu;
        const auto c = get();
        if (index >= 0)
        {
            menu.addItem (tr ("Delete point"), c.count > 1, false, [this, index] {
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
                if (v.count > 0 && axis == Axis::time)
                    v.time[0] = 0.0f;
                set (v);
            });
            if (sustainable)
                menu.addItem (tr ("Sustain point (holds while the key is down)"), true, c.sustain == index, [this, index] {
                    auto v = get();
                    v.sustain = v.sustain == index ? -1 : index;
                    set (v);
                });
            menu.addItem (tr ("Straight line to the next point"), index + 1 < c.count, false, [this, index] {
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

// FL's X/Y modulation pad: drag the dot to set the global Mod X / Mod Y.
class ModPad final : public juce::Component
{
public:
    std::function<juce::Point<float>()> get;
    std::function<void (juce::Point<float>)> set;

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (theme::colour::border);
        g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());
        g.drawVerticalLine ((int) area.getCentreX(), area.getY(), area.getBottom());
        const auto v = get();
        const auto pt = juce::Point<float> (area.getX() + 6.0f + (area.getWidth() - 12.0f) * v.x, area.getBottom() - 6.0f - (area.getHeight() - 12.0f) * v.y);
        g.setColour (theme::colour::accent);
        g.drawEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (pt), 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { move (e); }
    void mouseDrag (const juce::MouseEvent& e) override { move (e); }
    void mouseDoubleClick (const juce::MouseEvent&) override { set ({ 0.5f, 0.5f }); }

private:
    void move (const juce::MouseEvent& e)
    {
        const auto area = getLocalBounds().toFloat().reduced (6.0f);
        set ({ juce::jlimit (0.0f, 1.0f, (e.position.x - area.getX()) / area.getWidth()),
               juce::jlimit (0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight()) });
        repaint();
    }
};

// A one-row keyboard showing an articulator's key range (FL's keyboard strip): click sets the lowest key,
// right-click / Shift+click the highest.
class KeyStrip final : public juce::Component
{
public:
    std::function<std::pair<int, int>()> get;
    std::function<void (int low, int high)> set;
    static constexpr int firstKey = 24, lastKey = 108; // C1 … C8

    void paint (juce::Graphics& g) override
    {
        const auto [low, high] = get();
        const auto area = getLocalBounds().toFloat();
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (area, 3.0f);
        const auto w = area.getWidth() / (float) (lastKey - firstKey + 1);
        for (int key = firstKey; key <= lastKey; ++key)
        {
            const auto black = juce::MidiMessage::isMidiNoteBlack (key);
            const auto r = juce::Rectangle<float> (area.getX() + (float) (key - firstKey) * w, area.getY(), w, area.getHeight());
            const auto inRange = key >= low && key <= std::min (high, 127);
            auto colour = black ? theme::colour::bg4 : theme::colour::textSecondary;
            if (inRange)
                colour = black ? theme::colour::accentMuted : theme::colour::accent.withAlpha (0.75f);
            g.setColour (colour);
            g.fillRect (r.reduced (0.5f, black ? 0.0f : 0.0f).withTrimmedBottom (black ? area.getHeight() * 0.35f : 0.0f));
            if (key % 12 == 0)
            {
                g.setColour (theme::colour::textPrimary);
                g.setFont (theme::font (theme::type::caption));
                g.drawText (juce::MidiMessage::getMidiNoteName (key, true, true, 4), r.withWidth (30.0f).withTrimmedTop (area.getHeight() - 12.0f),
                            juce::Justification::centredLeft);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto w = (float) getWidth() / (float) (lastKey - firstKey + 1);
        const auto key = juce::jlimit (0, 127, firstKey + (int) (e.position.x / w));
        auto [low, high] = get();
        if (e.mods.isPopupMenu() || e.mods.isShiftDown())
            high = juce::jmax (key, low);
        else
        {
            low = key;
            high = juce::jmax (high, key);
        }
        set (low, high);
        repaint();
    }
};

class EnvelopeControllerEditor final : public juce::Component,
                                       private juce::Timer,
                                       private juce::ChangeListener
{
public:
    // `locate` finds the controller's channel (a Channel Rack channel or a Patcher node; null = gone); `channel` is
    // the Channel Rack index when there is one (links listing), `output` reads the running outputs.
    EnvelopeControllerEditor (AudioEngine& e, std::function<ChannelDef* (Song&)> where, int c, std::function<float (int)> output)
        : engine (e), locate (std::move (where)), channel (c), outputOf (std::move (output))
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

        part.addItemList ({ tr ("Envelope"), tr ("LFO"), tr ("Keyboard mapping"), tr ("Velocity mapping"), tr ("Modulation X mapping"),
                            tr ("Modulation Y mapping"), tr ("Random") },
                          1);
        part.setSelectedItemIndex (0, juce::dontSendNotification);
        part.onChange = [this] { showPart (part.getSelectedItemIndex()); };
        ui::setHint (part, tr ("Which part of this output to edit: its envelope, its LFO or how the key, velocity, Mod X / Y and a random value per note shape it"));
        addAndMakeVisible (part);

        graph.get = [this] { return shownCurve(); };
        graph.set = [this] (const ControllerCurve& c) { storeCurve (c); };
        graph.live = [this] { return outputs[(std::size_t) shown]; };
        graph.active = [this] { return partActive(); };
        graph.emptyText = [this] { return emptyText(); };
        graph.preview = [this] (float t) -> float {
            const auto a = data()[(std::size_t) shown];
            const auto amp = controllerCurveAt (a.lfoAmp, t, 1.0f);
            return amp * controllerLfoWave (a, controllerLfoStartPhase (a) + (double) t * controllerLfoRate (a));
        };
        addAndMakeVisible (graph);

        pad.get = [this] { const auto d = data(); return juce::Point<float> (d.modX, d.modY); };
        pad.set = [this] (juce::Point<float> v) { editAll ([v] (EnvelopeControllerData& d) { d.modX = v.x; d.modY = v.y; }); };
        ui::setHint (pad, tr ("Mod X / Mod Y: the inputs of the Modulation X / Y maps (each note adds its own Mod X / Y)"));
        addAndMakeVisible (pad);
        modX = makeKnob (unitSpec (tr ("Mod X"), 0.5, std::nullopt), [this] { return (double) data().modX; },
                         [this] (double v) { editAll ([v] (EnvelopeControllerData& d) { d.modX = (float) v; }); }, "X");
        modY = makeKnob (unitSpec (tr ("Mod Y"), 0.5, std::nullopt), [this] { return (double) data().modY; },
                         [this] (double v) { editAll ([v] (EnvelopeControllerData& d) { d.modY = (float) v; }); }, "Y");

        base = makeKnob (unitSpec (tr ("Base"), 0.5, std::nullopt), [this] { return (double) articulator().base; },
                         [this] (double v) { edit ([v] (ControllerArticulator& a) { a.base = (float) v; }); }, tr ("Base"));
        envAmount = makeKnob (levelSpec (tr ("Envelope amount"), 1.0), [this] { return (double) articulator().envLevel; },
                              [this] (double v) { edit ([v] (ControllerArticulator& a) { a.envLevel = (float) v; }); }, tr ("Env"));
        lfoAmount = makeKnob (levelSpec (tr ("LFO amount"), 0.5), [this] { return (double) articulator().lfoLevel; },
                              [this] (double v) { edit ([v] (ControllerArticulator& a) { a.lfoLevel = (float) v; }); }, tr ("LFO"));

        // envelope page: FL's ATT / DEC / SUS / REL scale the drawn envelope
        const juce::String adsrNames[] { tr ("Attack"), tr ("Decay"), tr ("Sustain"), tr ("Release") };
        const juce::String adsrShort[] { tr ("Att"), tr ("Dec"), tr ("Sus"), tr ("Rel") };
        for (int i = 0; i < 4; ++i)
        {
            ui::ValueSpec s;
            s.name = adsrNames[i];
            s.minimum = i == 2 ? -128.0 : 0.0;
            s.maximum = i == 2 ? 128.0 : 256.0;
            s.defaultValue = i == 2 ? 0.0 : 128.0;
            s.interval = 1.0;
            s.toText = [i] (double v) { return i == 2 ? juce::String (juce::roundToInt (v / 1.28)) + "%" : juce::String (v / 128.0, 2) + "x"; };
            adsr[(std::size_t) i] = makeKnob (s, [this, i] { return (double) articulator().adsr[(std::size_t) i]; },
                                              [this, i] (double v) { edit ([i, v] (ControllerArticulator& a) { a.adsr[(std::size_t) i] = juce::roundToInt (v); }); },
                                              adsrShort[i]);
        }
        // LFO page
        {
            ui::ValueSpec s;
            s.name = tr ("LFO speed");
            s.maximum = 65536.0;
            s.defaultValue = 40000.0;
            s.interval = 1.0;
            s.toText = [this] (double v) {
                auto a = articulator();
                a.lfoSpeed = (int) v;
                const auto period = 1.0 / juce::jmax (1.0e-9, controllerLfoRate (a));
                return juce::String (period, period < 10.0 ? 2 : 0) + (a.lfoTempo ? tr (" beat") : " s");
            };
            lfoSpeed = makeKnob (s, [this] { return (double) articulator().lfoSpeed; },
                                 [this] (double v) { edit ([v] (ControllerArticulator& a) { a.lfoSpeed = juce::roundToInt (v); }); }, tr ("Speed"));
        }
        const juce::String lfoNames[] { tr ("LFO tension"), tr ("LFO skew"), tr ("LFO pulse width") };
        const juce::String lfoShort[] { tr ("Tension"), tr ("Skew"), tr ("PW") };
        for (int i = 0; i < 3; ++i)
        {
            ui::ValueSpec s;
            s.name = lfoNames[i];
            s.minimum = -128.0;
            s.maximum = 128.0;
            s.interval = 1.0;
            s.toText = [] (double v) { return juce::String (juce::roundToInt (v / 1.28)) + "%"; };
            lfoShape[(std::size_t) i] = makeKnob (s,
                                                  [this, i] {
                                                      const auto a = articulator();
                                                      return (double) (i == 0 ? a.lfoTension : i == 1 ? a.lfoSkew : a.lfoPulseWidth);
                                                  },
                                                  [this, i] (double v) {
                                                      edit ([i, v] (ControllerArticulator& a) {
                                                          (i == 0 ? a.lfoTension : i == 1 ? a.lfoSkew : a.lfoPulseWidth) = juce::roundToInt (v);
                                                      });
                                                  },
                                                  lfoShort[i]);
        }

        enabled.onClick = [this] {
            const auto on = enabled.getToggleState();
            edit ([this, on] (ControllerArticulator& a) { (shownPart == 1 ? a.lfoOn : a.envOn) = on; });
        };
        bipolar.setButtonText (tr ("Bipolar (centred on base)"));
        bipolar.onClick = [this] {
            const auto on = bipolar.getToggleState();
            edit ([on] (ControllerArticulator& a) { a.bipolar = on; }); // (empty maps stay neutral in either mode)
        };
        ui::setHint (bipolar, tr ("FL's MODE: bipolar adds the envelope, LFO and maps around the base; unipolar multiplies them"));
        tempo.setButtonText (tr ("Time in beats"));
        tempo.onClick = [this] {
            const auto on = tempo.getToggleState();
            edit ([this, on] (ControllerArticulator& a) { (shownPart == 1 ? a.lfoTempo : a.tempo) = on; });
        };
        global.setButtonText (tr ("Global (follows the song, not each note)"));
        global.onClick = [this] { const auto on = global.getToggleState(); edit ([on] (ControllerArticulator& a) { a.lfoGlobal = on; }); };
        continuous.setButtonText (tr ("Continuous output"));
        continuous.onClick = [this] { const auto on = continuous.getToggleState(); editAll ([on] (EnvelopeControllerData& d) { d.continuous = on; }); };
        ui::setHint (continuous, tr ("With no note playing, the outputs keep following the base and the Mod X / Y maps (FL's \"Smart\" presets)"));
        resetMap.setButtonText (tr ("Reset map"));
        resetMap.onClick = [this] {
            const auto m = shownPart - 2;
            edit ([m] (ControllerArticulator& a) { a.maps[(std::size_t) m] = {}; });
        };
        for (juce::Component* b : { (juce::Component*) &enabled, (juce::Component*) &bipolar, (juce::Component*) &tempo, (juce::Component*) &global,
                                    (juce::Component*) &continuous, (juce::Component*) &resetMap })
            addAndMakeVisible (*b);

        keys.get = [this] { const auto a = articulator(); return std::pair<int, int> (a.lowKey, a.highKey); };
        keys.set = [this] (int low, int high) { edit ([low, high] (ControllerArticulator& a) { a.lowKey = low; a.highKey = high >= 127 ? 256 : high; }); };
        ui::setHint (keys, tr ("The keys this output follows: click sets the lowest, right-click (or Shift+click) the highest"));
        addAndMakeVisible (keys);
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
        lowKey.onValueChange = [this] { const auto v = (int) lowKey.getValue(); edit ([v] (ControllerArticulator& a) { a.lowKey = v; }); };
        // the top of the range means every key (FL saves 256)
        highKey.onValueChange = [this] { const auto v = highKey.getValue(); edit ([v] (ControllerArticulator& a) { a.highKey = v >= 127.0 ? 256 : (int) v; }); };
        keysLabel.setText (tr ("Keys"), juce::dontSendNotification);
        keysLabel.setFont (theme::font (theme::type::small));
        addAndMakeVisible (keysLabel);
        for (auto* l : { &links, &partInfo })
        {
            l->setFont (theme::font (theme::type::caption));
            l->setColour (juce::Label::textColourId, theme::colour::textSecondary);
            l->setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (*l);
        }

        engine.addChangeListener (this);
        refresh();
        startTimerHz (30);
        setSize (760, 520);
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
        auto top = area.removeFromTop (96);
        // X/Y pad with its knobs (FL: left of the outputs)
        auto padArea = top.removeFromLeft (96);
        pad.setBounds (padArea.reduced (2));
        auto xy = top.removeFromLeft (52);
        modX->setBounds (xy.removeFromTop (48));
        modY->setBounds (xy);
        top.removeFromLeft (8);
        // outputs 1–8 in two rows of four (as FL)
        auto outs = top.removeFromLeft (4 * 34).withTrimmedBottom (14);
        for (int k = 0; k < 8; ++k)
            tabs[(std::size_t) k].setBounds (outs.getX() + (k % 4) * 34, outs.getY() + (k / 4) * 42, 32, 30);
        top.removeFromLeft (10);
        auto knobs = top.removeFromLeft (3 * 58);
        base->setBounds (knobs.removeFromLeft (58));
        envAmount->setBounds (knobs.removeFromLeft (58));
        lfoAmount->setBounds (knobs.removeFromLeft (58));
        top.removeFromLeft (10);
        auto right = top;
        bipolar.setBounds (right.removeFromTop (24));
        continuous.setBounds (right.removeFromTop (24));
        right.removeFromTop (6);
        part.setBounds (right.removeFromTop (26).withWidth (juce::jmin (right.getWidth(), 220)));

        area.removeFromTop (6);
        keys.setBounds (area.removeFromTop (30));
        area.removeFromTop (6);

        auto bottom = area.removeFromBottom (78);
        auto side = area.removeFromRight (170);
        graph.setBounds (area);
        side.removeFromLeft (12);
        keysLabel.setBounds (side.removeFromTop (18));
        lowKey.setBounds (side.removeFromTop (26));
        highKey.setBounds (side.removeFromTop (26));
        side.removeFromTop (8);
        links.setBounds (side);

        bottom.removeFromTop (6);
        auto toggles = bottom.removeFromLeft (190);
        enabled.setBounds (toggles.removeFromTop (24));
        tempo.setBounds (toggles.removeFromTop (24));
        global.setBounds (toggles.removeFromTop (24));
        resetMap.setBounds (juce::Rectangle<int> (toggles.getX(), enabled.getY(), 120, 26));
        auto pageKnobs = bottom.removeFromLeft (4 * 64);
        for (auto& k : adsr)
            k->setBounds (pageKnobs.removeFromLeft (64));
        pageKnobs = adsr[0]->getBounds().getUnion (adsr[3]->getBounds());
        lfoSpeed->setBounds (pageKnobs.removeFromLeft (64));
        for (auto& k : lfoShape)
            k->setBounds (pageKnobs.removeFromLeft (64));
        bottom.removeFromLeft (10);
        partInfo.setBounds (bottom);
    }

private:
    std::unique_ptr<Knob> makeKnob (ui::ValueSpec spec, std::function<double()> get, std::function<void (double)> set, const juce::String& label)
    {
        auto k = std::make_unique<Knob> (&engine, std::move (spec), std::move (get), std::move (set));
        k->shortName = label;
        addAndMakeVisible (*k);
        return k;
    }

    static ui::ValueSpec levelSpec (const juce::String& name, double def)
    {
        ui::ValueSpec s;
        s.name = name;
        s.minimum = -1.0;
        s.maximum = 1.0;
        s.defaultValue = def;
        s.toText = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        return s;
    }

    EnvelopeControllerData data() const
    {
        auto& song = const_cast<Song&> (engine.getSong());
        const auto* c = locate ? locate (song) : nullptr;
        if (c == nullptr)
            return defaultEnvelopeController();
        return parseEnvelopeController (c->controllerState).value_or (defaultEnvelopeController());
    }

    ControllerArticulator articulator() const { return data()[(std::size_t) shown]; }

    void editAll (std::function<void (EnvelopeControllerData&)> change)
    {
        auto d = data();
        change (d);
        const auto bytes = writeEnvelopeController (d);
        engine.editSong ([this, bytes] (Song& song) {
            if (auto* c = locate ? locate (song) : nullptr; c != nullptr && c->instrument == InstrumentKind::controller)
                c->controllerState = bytes;
        });
    }

    void edit (std::function<void (ControllerArticulator&)> change)
    {
        const auto k = (std::size_t) shown;
        editAll ([k, change] (EnvelopeControllerData& d) { change (d[k]); });
    }

    // the curve the graph shows: the envelope, the LFO's amplitude or a map (an empty map as its neutral line)
    ControllerCurve shownCurve() const
    {
        const auto a = articulator();
        if (shownPart == 0)
        {
            ControllerCurve c;
            c.count = a.count;
            c.time = a.time;
            c.level = a.level;
            c.tension = a.tension;
            c.sustain = a.sustain;
            return c;
        }
        if (shownPart == 1)
        {
            if (a.lfoAmp.count > 0)
                return a.lfoAmp;
            ControllerCurve c;
            c.count = 1;
            c.level[0] = 1.0f;
            return c;
        }
        const auto& m = a.maps[(std::size_t) shownPart - 2];
        if (m.count > 0)
            return m;
        ControllerCurve c;
        c.count = 2;
        c.time = { 0.0f, 1.0f };
        c.level = { neutralOf (a), neutralOf (a) };
        return c;
    }

    static float neutralOf (const ControllerArticulator& a) { return a.bipolar ? 0.5f : 1.0f; }

    void storeCurve (const ControllerCurve& c)
    {
        const auto partShown = shownPart;
        edit ([partShown, c] (ControllerArticulator& a) {
            if (partShown == 0)
            {
                a.count = c.count;
                a.time = c.time;
                a.level = c.level;
                a.tension = c.tension;
                a.sustain = c.sustain;
                a.envOn = true;
                if (a.decayPoint >= a.count)
                    a.decayPoint = -1;
            }
            else if (partShown == 1)
            {
                a.lfoAmp = c;
                a.lfoOn = true;
            }
            else
            {
                auto m = c;
                m.sustain = -1;
                bool flat = true;
                for (int i = 0; i < m.count; ++i)
                    flat = flat && std::abs (m.level[(std::size_t) i] - neutralOf (a)) < 1.0e-4f;
                a.maps[(std::size_t) partShown - 2] = flat ? ControllerCurve {} : m;
            }
        });
    }

    bool partActive() const
    {
        const auto a = articulator();
        if (shownPart == 0)
            return a.envOn && a.count > 0;
        if (shownPart == 1)
            return a.lfoOn;
        return a.maps[(std::size_t) shownPart - 2].count > 0;
    }

    juce::String emptyText() const
    {
        if (shownPart == 0)
            return tr ("Envelope off: the output rests at its base");
        if (shownPart == 1)
            return tr ("LFO off: turn it on below");
        return tr ("Neutral: drag the line or double-click to shape this map");
    }

    void showPart (int index)
    {
        shownPart = juce::jlimit (0, 6, index);
        graph.axis = shownPart == 2 ? CurveGraph::Axis::keys : shownPart >= 3 ? CurveGraph::Axis::unit : CurveGraph::Axis::time;
        graph.sustainable = shownPart <= 1;
        refresh();
        resized();
    }

    void select (int k)
    {
        shown = k;
        refresh();
    }

    void refresh()
    {
        const auto d = data();
        const auto& a = d[(std::size_t) shown];
        for (int k = 0; k < 8; ++k)
            tabs[(std::size_t) k].setColour (juce::TextButton::buttonColourId, k == shown ? theme::colour::accentMuted : theme::colour::bg4);
        const auto envelopePage = shownPart == 0, lfoPage = shownPart == 1, mapPage = shownPart >= 2;
        enabled.setButtonText (lfoPage ? tr ("LFO on") : tr ("Envelope on"));
        enabled.setToggleState (lfoPage ? a.lfoOn : a.envOn, juce::dontSendNotification);
        enabled.setVisible (! mapPage);
        tempo.setToggleState (lfoPage ? a.lfoTempo : a.tempo, juce::dontSendNotification);
        tempo.setVisible (! mapPage);
        global.setToggleState (a.lfoGlobal, juce::dontSendNotification);
        global.setVisible (lfoPage);
        resetMap.setVisible (mapPage);
        for (auto& k : adsr)
            k->setVisible (envelopePage);
        lfoSpeed->setVisible (lfoPage);
        for (auto& k : lfoShape)
            k->setVisible (lfoPage);
        bipolar.setToggleState (a.bipolar, juce::dontSendNotification);
        continuous.setToggleState (d.continuous, juce::dontSendNotification);
        lowKey.setValue (juce::jlimit (0, 127, a.lowKey), juce::dontSendNotification);
        highKey.setValue (juce::jlimit (0, 127, a.highKey), juce::dontSendNotification);
        graph.tempo = lfoPage ? a.lfoTempo : a.tempo;
        graph.neutral = mapPage ? neutralOf (a) : -1.0f;
        graph.showPreview = lfoPage;
        graph.minSpan = lfoPage ? (float) juce::jlimit (1.0, 16.0, std::ceil (1.0 / juce::jmax (1.0e-3, controllerLfoRate (a)))) : 1.0f;

        const juce::String info[] {
            tr ("The envelope starts on each note in the key range; ATT / DEC / REL stretch its parts, SUS moves the sustain level."),
            tr ("The LFO restarts on each note (unless Global); the curve sets its amplitude over time. Speed, Tension, Skew and PW shape the wave."),
            tr ("x = the note's key (C0 to B9): how each key moves the output."),
            tr ("x = the note's velocity: how hard notes move the output."),
            tr ("x = the Mod X knob plus the note's own Mod X."),
            tr ("x = the Mod Y knob plus the note's own Mod Y."),
            tr ("x = a random value drawn on each note."),
        };
        partInfo.setText (info[shownPart], juce::dontSendNotification);

        juce::StringArray targets;
        if (channel >= 0)
            for (const auto& automation : engine.getSong().automations)
                if (automation.source.channel == channel && automation.source.output == shown && automation.target.kind != AutomationTargetKind::none)
                    targets.add (describeAutomationTarget (engine, automation.target));
        links.setText (channel < 0          ? tr ("Cable this output to a parameter in the Patcher.")
                       : targets.isEmpty() ? tr ("Not linked yet. Right-click a knob > Link to internal controller > this output.")
                                           : tr ("Drives:") + "\n" + targets.joinIntoString ("\n"),
                       juce::dontSendNotification);
        repaint();
        graph.repaint();
        pad.repaint();
        keys.repaint();
        for (auto* k : { base.get(), envAmount.get(), lfoAmount.get(), modX.get(), modY.get(), lfoSpeed.get() })
            k->repaint();
        for (auto& k : adsr)
            k->repaint();
        for (auto& k : lfoShape)
            k->repaint();
    }

    void timerCallback() override
    {
        for (int k = 0; k < 8; ++k)
            outputs[(std::size_t) k] = outputOf ? outputOf (k) : 0.0f;
        repaint (getLocalBounds().withHeight (130));
        graph.repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    std::function<ChannelDef* (Song&)> locate;
    int channel, shown = 0, shownPart = 0;
    std::function<float (int)> outputOf;
    std::array<juce::TextButton, 8> tabs;
    juce::ComboBox part;
    CurveGraph graph;
    ModPad pad;
    KeyStrip keys;
    std::unique_ptr<Knob> base, envAmount, lfoAmount, modX, modY, lfoSpeed;
    std::array<std::unique_ptr<Knob>, 4> adsr;
    std::array<std::unique_ptr<Knob>, 3> lfoShape;
    juce::ToggleButton enabled, bipolar, tempo, global, continuous;
    juce::TextButton resetMap;
    juce::Slider lowKey, highKey;
    juce::Label keysLabel, links, partInfo;
    std::array<float, 8> outputs {};
};

// ===================================================================================================
// MIDI Out
// ===================================================================================================

class MidiOutEditor final : public juce::Component,
                            private juce::ChangeListener
{
public:
    MidiOutEditor (AudioEngine& e, int c) : engine (e), channel (c)
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
        const auto& song = engine.getSong();
        return channel >= 0 && channel < (int) song.channels.size() ? midictl::parseMidiOut (song.channels[(std::size_t) channel].pluginState)
                                                                    : midictl::MidiOutSettings {};
    }

    void edit (std::function<void (midictl::MidiOutSettings&)> change)
    {
        engine.editSong ([this, change] (Song& song) {
            if (channel < 0 || channel >= (int) song.channels.size() || song.channels[(std::size_t) channel].instrument != InstrumentKind::midiOut)
                return;
            auto& state = song.channels[(std::size_t) channel].pluginState;
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

std::unique_ptr<juce::Component> makeEnvelopeControllerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, int channel,
                                                               std::function<float (int)> output)
{
    return std::make_unique<EnvelopeControllerEditor> (engine, std::move (locate), channel, std::move (output));
}

std::unique_ptr<juce::Component> makeEnvelopeControllerEditor (AudioEngine& engine, int channel)
{
    return makeEnvelopeControllerEditor (
        engine,
        [channel] (Song& song) -> ChannelDef* {
            return channel >= 0 && channel < (int) song.channels.size() ? &song.channels[(std::size_t) channel] : nullptr;
        },
        channel,
        [&engine, channel] (int k) {
            const auto instrument = engine.getChannelInstrument (channel);
            return instrument != nullptr ? instrument->controlOutput (k) : 0.0f;
        });
}

std::unique_ptr<juce::Component> makeMidiOutEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<MidiOutEditor> (engine, channel);
}
} // namespace lynx
