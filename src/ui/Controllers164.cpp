#include "ui/Controllers164.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/ControlSurface.h"
#include "core/XyController.h"
#include "ui/ControllerKnob.h"
#include "ui/I18n.h"
#include "ui/SlotWatch.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
juce::String utf8 (const std::string& s)
{
    return juce::String::fromUTF8 (s.c_str());
}

const char* const axisNames[] = { "X", "Y", "Z" };

// ===================================================================================================
// X-Y / X-Y-Z Controller
// ===================================================================================================

class XyEditor final : public juce::Component,
                       private juce::Timer,
                       private juce::ChangeListener
{
public:
    XyEditor (AudioEngine& e, SlotLocator l, OutputReader o, std::optional<AutomationTarget> t)
        : engine (e), locate (std::move (l)), output (std::move (o)), target (t)
    {
        const auto* fx = current();
        kind = fx != nullptr ? fx->kind : EffectKind::xyController;
        axes = xy::axesOf (kind);
        for (int a = 0; a < axes; ++a)
        {
            auto& b = axisButtons[(std::size_t) a];
            b.setButtonText (axisNames[a]);
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
            b.onClick = [this, a] { selectAxis (a); };
            ui::setHint (b, tr ("Show the {0} axis' settings (level, min, max, tension)", { axisNames[a] }));
            addAndMakeVisible (b);
        }
        const auto makeKnob = [this] (int param, const juce::String& shortName, const juce::String& name, double def) {
            ui::ValueSpec s;
            s.name = name;
            s.defaultValue = def;
            s.toText = [this, param] (double v) { return utf8 (xy::paramText (kind, param, (float) v)); };
            if (target)
                s.target = AutomationTarget { target->kind, target->index, target->slot, param };
            auto k = std::make_unique<ctlui::Knob> (&engine, s, [this, param] { return value (param); }, [this, param] (double v) { set (param, (float) v); });
            k->shortName = shortName;
            addAndMakeVisible (*k);
            return k;
        };
        for (int a = 0; a < axes; ++a)
        {
            const juce::String axis = axisNames[a];
            auto& k = axisKnobs[(std::size_t) a];
            k[0] = makeKnob (xy::level (a), tr ("Level"), tr ("{0} level: where the target goes", { axis }), 0.5);
            k[1] = makeKnob (xy::minimum (a), tr ("Min"), tr ("{0} minimum: the output at the pad's low edge", { axis }), 0.0);
            k[2] = makeKnob (xy::maximum (a), tr ("Max"), tr ("{0} maximum: the output at the pad's high edge", { axis }), 1.0);
            k[3] = makeKnob (xy::tension (a), tr ("Tension"), tr ("{0} tension: bends the curve from the position to the output", { axis }), 0.5);
        }
        speedKnob = makeKnob (xy::speed (kind), tr ("Speed"), tr ("Speed: how fast the target follows the goal"), 0.5);
        accelKnob = makeKnob (xy::acceleration (kind), tr ("Accel"), tr ("Acceleration: how quickly the target reaches its speed"), 0.5);
        selectAxis (0);
        engine.addChangeListener (this);
        timerCallback(); // (the outputs before the first tick)
        startTimerHz (30);
        setSize (axes == 3 ? 640 : 600, 380);
    }

    ~XyEditor() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg2);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::title, true));
        g.drawText (axes == 3 ? "X-Y-Z Controller" : "X-Y Controller", 14, 8, 260, 22, juce::Justification::centredLeft);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        g.drawText (tr ("Link its outputs to any knob: right-click it > Link to internal controller"), 220, 8, getWidth() - 234, 22,
                    juce::Justification::centredRight);

        const auto* fx = current();
        if (fx == nullptr)
            return;
        const auto& p = fx->params;
        // the pad
        const auto pad = padArea().toFloat();
        fillRoundRect (g, pad, radius::lg, colour::bg0);
        g.setColour (colour::gridBeat);
        for (int i = 1; i < 8; ++i)
        {
            g.drawVerticalLine ((int) (pad.getX() + pad.getWidth() * (float) i / 8.0f), pad.getY() + 2.0f, pad.getBottom() - 2.0f);
            g.drawHorizontalLine ((int) (pad.getY() + pad.getHeight() * (float) i / 8.0f), pad.getX() + 2.0f, pad.getRight() - 2.0f);
        }
        g.setColour (colour::gridBar);
        g.drawVerticalLine ((int) pad.getCentreX(), pad.getY() + 2.0f, pad.getBottom() - 2.0f);
        g.drawHorizontalLine ((int) pad.getCentreY(), pad.getX() + 2.0f, pad.getRight() - 2.0f);
        const auto at = [&] (double x, double y) {
            return juce::Point<float> (pad.getX() + 8.0f + (pad.getWidth() - 16.0f) * (float) x, pad.getBottom() - 8.0f - (pad.getHeight() - 16.0f) * (float) y);
        };
        // goal: a ring with a cross; target: a filled dot (its size follows Z)
        const auto goal = at (p[(std::size_t) xy::level (0)], p[(std::size_t) xy::level (1)]);
        g.setColour (colour::textSecondary.withAlpha (0.8f));
        g.drawEllipse (juce::Rectangle<float> (22.0f, 22.0f).withCentre (goal), 1.4f);
        g.drawLine (goal.x - 15.0f, goal.y, goal.x + 15.0f, goal.y, 1.0f);
        g.drawLine (goal.x, goal.y - 15.0f, goal.x, goal.y + 15.0f, 1.0f);
        const auto z = axes == 3 ? shown[2] : 0.5f;
        const auto dot = at (shown[0], shown[1]);
        const auto size = 10.0f + 14.0f * z;
        g.setColour (colour::accent.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (size + 12.0f, size + 12.0f).withCentre (dot));
        g.setColour (colour::accent);
        g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (dot));

        // Z strip
        if (axes == 3)
        {
            const auto strip = zArea().toFloat();
            fillRoundRect (g, strip, radius::md, colour::bg0);
            const auto zGoal = (float) p[(std::size_t) xy::level (2)];
            const auto yOf = [&] (float v) { return strip.getBottom() - 6.0f - (strip.getHeight() - 12.0f) * v; };
            fillRoundRect (g, strip.withTop (yOf (shown[2])).reduced (5.0f, 0.0f).withTrimmedBottom (6.0f), 3.0f, colour::accent.withAlpha (0.7f));
            g.setColour (colour::textPrimary);
            g.fillRoundedRectangle (juce::Rectangle<float> (strip.getWidth() - 4.0f, 4.0f).withCentre ({ strip.getCentreX(), yOf (zGoal) }), 2.0f);
            g.setColour (colour::textSecondary);
            g.drawText ("Z", strip.withY (strip.getBottom() + 2.0f).withHeight (14.0f).toNearestInt(), juce::Justification::centred);
        }

        // the selected axis' curve (position → output)
        const auto curve = curveArea().toFloat();
        fillRoundRect (g, curve, radius::md, colour::bg0);
        juce::Path path;
        for (int i = 0; i <= 48; ++i)
        {
            const auto pos = (double) i / 48.0;
            const auto out = xy::output (p, axis, pos);
            const auto pt = juce::Point<float> (curve.getX() + 4.0f + (curve.getWidth() - 8.0f) * (float) pos, curve.getBottom() - 4.0f - (curve.getHeight() - 8.0f) * out);
            i == 0 ? path.startNewSubPath (pt) : path.lineTo (pt);
        }
        g.setColour (colour::automation);
        g.strokePath (path, juce::PathStrokeType (1.6f));
        const auto now = xy::output (p, axis, shown[(std::size_t) axis]);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ curve.getX() + 4.0f + (curve.getWidth() - 8.0f) * shown[(std::size_t) axis],
                                                                        curve.getBottom() - 4.0f - (curve.getHeight() - 8.0f) * now }));

        // readouts
        g.setFont (monoFont (type::small));
        auto line = readoutArea();
        for (int a = 0; a < axes; ++a)
        {
            g.setColour (a == axis ? colour::textPrimary : colour::textSecondary);
            g.drawText (juce::String (axisNames[a]) + juce::String (" ") + juce::String (outputs[(std::size_t) a], 3), line.removeFromLeft (line.getWidth() / (axes - a)),
                        juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto right = getLocalBounds().reduced (14).withTrimmedTop (32).withTrimmedLeft (padArea().getWidth() + (axes == 3 ? 44 : 14));
        auto tabs = right.removeFromTop (26);
        for (int a = 0; a < axes; ++a)
            axisButtons[(std::size_t) a].setBounds (tabs.removeFromLeft (40).reduced (2, 0));
        right.removeFromTop (8);
        auto row = right.removeFromTop (72);
        const auto knobWidth = juce::jmin (62, row.getWidth() / 4);
        for (int a = 0; a < axes; ++a)
        {
            auto r = row;
            for (auto& k : axisKnobs[(std::size_t) a])
                k->setBounds (r.removeFromLeft (knobWidth));
        }
        right.removeFromTop (curveArea().getHeight() + 30);
        auto bottom = right.removeFromTop (72);
        speedKnob->setBounds (bottom.removeFromLeft (62));
        accelKnob->setBounds (bottom.removeFromLeft (62));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingZ = axes == 3 && zArea().contains (e.getPosition());
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingZ)
        {
            const auto strip = zArea().toFloat();
            set (xy::level (2), juce::jlimit (0.0f, 1.0f, (strip.getBottom() - 6.0f - e.position.y) / (strip.getHeight() - 12.0f)));
            return;
        }
        const auto pad = padArea().toFloat();
        if (! pad.expanded (30.0f).contains (e.mouseDownPosition))
            return;
        const auto x = juce::jlimit (0.0f, 1.0f, (e.position.x - pad.getX() - 8.0f) / (pad.getWidth() - 16.0f));
        const auto y = juce::jlimit (0.0f, 1.0f, (pad.getBottom() - 8.0f - e.position.y) / (pad.getHeight() - 16.0f));
        edit ([x, y] (EffectSlot& fx) {
            fx.params[(std::size_t) xy::level (0)] = x;
            fx.params[(std::size_t) xy::level (1)] = y;
        });
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        // FL's X-Y-Z: the wheel moves Z
        if (axes == 3 && (padArea().contains (e.getPosition()) || zArea().contains (e.getPosition())))
            set (xy::level (2), juce::jlimit (0.0f, 1.0f, (float) value (xy::level (2)) + wheel.deltaY * 0.25f));
    }

private:
    juce::Rectangle<int> padArea() const
    {
        const auto side = juce::jmin (getHeight() - 60, 300);
        return { 14, 40, side, side };
    }
    juce::Rectangle<int> zArea() const { return { padArea().getRight() + 8, padArea().getY(), 22, padArea().getHeight() - 16 }; }
    juce::Rectangle<int> curveArea() const
    {
        const auto x = padArea().getRight() + (axes == 3 ? 44 : 14);
        return { x, 40 + 26 + 8 + 72 + 8, juce::jmin (getWidth() - x - 14, 4 * 62), 70 };
    }
    juce::Rectangle<int> readoutArea() const { return curveArea().translated (0, curveArea().getHeight() + 4).withHeight (18); }

    const EffectSlot* current() const { return locate (const_cast<Song&> (engine.getSong())); }
    double value (int param) const
    {
        const auto* fx = current();
        return fx != nullptr && param >= 0 && param < maxEffectParams ? (double) fx->params[(std::size_t) param] : 0.0;
    }
    void edit (std::function<void (EffectSlot&)> change)
    {
        engine.editSong ([this, change] (Song& s) {
            if (auto* fx = locate (s); fx != nullptr && xy::isXyController (fx->kind))
                change (*fx);
        });
    }
    void set (int param, float v)
    {
        edit ([param, v] (EffectSlot& fx) { fx.params[(std::size_t) param] = juce::jlimit (0.0f, 1.0f, v); });
    }

    void selectAxis (int a)
    {
        axis = a;
        for (int i = 0; i < axes; ++i)
        {
            axisButtons[(std::size_t) i].setToggleState (i == a, juce::dontSendNotification);
            for (auto& k : axisKnobs[(std::size_t) i])
                k->setVisible (i == a);
        }
        repaint();
    }

    void timerCallback() override
    {
        // the running target (outputs 4…6) and the axes' outputs; repaint only when something moved
        // (not running — no audio, or the slot is off: the target rests on the goal)
        std::array<float, 3> pos {}, out {};
        const auto* fx = current();
        const auto running = output && output (7) > 0.5f;
        for (int a = 0; a < axes; ++a)
        {
            pos[(std::size_t) a] = running ? output (4 + a) : (fx != nullptr ? fx->params[(std::size_t) xy::level (a)] : 0.5f);
            out[(std::size_t) a] = running ? output (a) : (fx != nullptr ? xy::output (fx->params, a, pos[(std::size_t) a]) : 0.0f);
        }
        if (pos != shown || out != outputs)
        {
            shown = pos;
            outputs = out;
            repaint();
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    AudioEngine& engine;
    SlotLocator locate;
    OutputReader output;
    std::optional<AutomationTarget> target;
    EffectKind kind = EffectKind::xyController;
    int axes = 2, axis = 0;
    bool draggingZ = false;
    std::array<juce::TextButton, 3> axisButtons;
    std::array<std::array<std::unique_ptr<ctlui::Knob>, 4>, 3> axisKnobs;
    std::unique_ptr<ctlui::Knob> speedKnob, accelKnob;
    std::array<float, 3> shown { 0.5f, 0.5f, 0.5f }, outputs {};
};

// ===================================================================================================
// Control Surface / Dashboard: a panel of controls, each one a control output
// ===================================================================================================

class SurfacePanel final : public juce::Component,
                           private juce::ChangeListener
{
public:
    using Locate = std::function<const std::vector<SurfaceControl>* (const Song&)>;
    using Edit = std::function<void (std::function<void (std::vector<SurfaceControl>&)>)>;

    SurfacePanel (AudioEngine& e, juce::String title, Locate l, Edit ed, std::function<std::optional<AutomationTarget> (int)> targetOf)
        : engine (e), heading (std::move (title)), locate (std::move (l)), editControls (std::move (ed)), target (std::move (targetOf))
    {
        editButton.setButtonText (tr ("Edit"));
        editButton.setClickingTogglesState (true);
        editButton.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
        ui::setHint (editButton, tr ("Edit mode: drag a control to move it, right-click it to rename, resize or delete it"));
        editButton.onClick = [this] { repaint(); };
        addAndMakeVisible (editButton);
        addButton.setButtonText ("+");
        ui::setHint (addButton, tr ("Add a control (knob, slider, button, check box, XY pad): each one is an output to link to any knob"));
        addButton.onClick = [this] { showAddMenu ({ 40.0f, 60.0f }, &addButton); };
        addAndMakeVisible (addButton);
        engine.addChangeListener (this);
        setSize (560, 380);
    }

    ~SurfacePanel() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg1);
        g.setColour (colour::bg3);
        g.fillRect (getLocalBounds().removeFromTop (top));
        g.setColour (colour::textPrimary);
        g.setFont (font (type::title, true));
        g.drawText (heading, 14, 6, 240, 22, juce::Justification::centredLeft);
        g.setColour (colour::gridStep);
        for (int x = 10; x < getWidth(); x += 10)
            for (int y = top + 10; y < getHeight(); y += 10)
                g.fillRect ((float) x, (float) y, 1.0f, 1.0f);
        const auto* controls = locate (engine.getSong());
        if (controls == nullptr)
            return;
        if (controls->empty())
        {
            g.setColour (colour::textSecondary);
            g.setFont (font (type::body));
            g.drawFittedText (tr ("Empty panel. \"+\" adds a knob, a slider, a button, a check box or an XY pad; each one becomes an "
                                  "output you can link to any knob (right-click it > Link to internal controller)."),
                              getLocalBounds().withTrimmedTop (top).reduced (40), juce::Justification::centred, 4);
        }
        const auto editing = editButton.getToggleState();
        int output = 0;
        for (int i = 0; i < (int) controls->size(); ++i)
        {
            const auto& c = (*controls)[(std::size_t) i];
            const auto r = bounds (c);
            const auto name = utf8 (c.name);
            const auto active = dragging == i;
            const auto arc = active ? colour::accent : colour::knobArc;
            g.setFont (font (type::caption));
            switch (c.kind)
            {
                case SurfaceControl::Kind::knob:
                    drawKnob (g, r.withHeight (r.getWidth()).reduced (4.0f), c.value, false, arc, active);
                    g.setColour (colour::textSecondary);
                    g.drawText (name, r.withTrimmedTop (r.getWidth()), juce::Justification::centred, true);
                    break;
                case SurfaceControl::Kind::slider:
                {
                    const auto track = r.withTrimmedBottom (16.0f).withSizeKeepingCentre (6.0f, r.getHeight() - 26.0f);
                    fillRoundRect (g, track, 3.0f, colour::bg0);
                    const auto y = track.getBottom() - c.value * track.getHeight();
                    fillRoundRect (g, track.withTop (y), 3.0f, arc);
                    fillRoundRect (g, juce::Rectangle<float> (24.0f, 10.0f).withCentre ({ track.getCentreX(), y }), 3.0f, colour::textPrimary);
                    g.setColour (colour::textSecondary);
                    g.drawText (name, r.withTop (r.getBottom() - 16.0f), juce::Justification::centred, true);
                    break;
                }
                case SurfaceControl::Kind::button:
                    fillRoundRect (g, r, 4.0f, c.value > 0.5f ? colour::accent : colour::bg4);
                    g.setColour (c.value > 0.5f ? colour::onAccent : colour::textPrimary);
                    g.drawText (name, r, juce::Justification::centred, true);
                    break;
                case SurfaceControl::Kind::checkBox:
                {
                    const auto box = r.withWidth (20.0f).reduced (2.0f);
                    fillRoundRect (g, box, 3.0f, colour::bg0);
                    if (c.value > 0.5f)
                        fillRoundRect (g, box.reduced (3.0f), 2.0f, colour::accent);
                    g.setColour (colour::textPrimary);
                    g.drawText (name, r.withTrimmedLeft (26.0f), juce::Justification::centredLeft, true);
                    break;
                }
                case SurfaceControl::Kind::xy:
                {
                    const auto pad = r.withTrimmedBottom (18.0f);
                    fillRoundRect (g, pad, 4.0f, colour::bg0);
                    g.setColour (colour::border);
                    g.drawHorizontalLine ((int) pad.getCentreY(), pad.getX(), pad.getRight());
                    g.drawVerticalLine ((int) pad.getCentreX(), pad.getY(), pad.getBottom());
                    const auto dot = juce::Point<float> (pad.getX() + c.value * pad.getWidth(), pad.getBottom() - c.valueY * pad.getHeight());
                    g.setColour (arc);
                    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (dot));
                    g.setColour (colour::textSecondary);
                    g.drawText (name, r.withTop (pad.getBottom()), juce::Justification::centred, true);
                    break;
                }
            }
            if (editing)
            {
                g.setColour (colour::accent.withAlpha (0.7f));
                g.drawRect (r.expanded (2.0f), 1.0f);
                g.setColour (colour::textDisabled);
                g.setFont (font (9.0f));
                g.drawText (juce::String (output + 1), r.expanded (2.0f).withHeight (11.0f).translated (0.0f, -12.0f).toNearestInt(),
                            juce::Justification::centredLeft);
            }
            output += c.outputs();
        }
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop (top).reduced (8, 5);
        addButton.setBounds (bar.removeFromRight (30));
        bar.removeFromRight (6);
        editButton.setBounds (bar.removeFromRight (60));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto* controls = locate (engine.getSong());
        if (controls == nullptr)
            return;
        const auto i = controlAt (e.position);
        if (e.mods.isPopupMenu())
        {
            if (i >= 0)
                showControlMenu (i);
            else
                showAddMenu (e.position, nullptr);
            return;
        }
        if (i < 0)
            return;
        dragging = i;
        const auto& c = (*controls)[(std::size_t) i];
        grab = { c.value, c.valueY };
        moving = editButton.getToggleState();
        grabAt = moving ? e.position - juce::Point<float> (c.x, c.y + (float) top) : e.position;
        if (moving)
            return;
        if (ui::isResetClick (e) && (c.kind == SurfaceControl::Kind::knob || c.kind == SurfaceControl::Kind::slider))
        {
            setValue (i, 0.5f, c.valueY);
            dragging = -1;
            return;
        }
        switch (c.kind)
        {
            case SurfaceControl::Kind::button: setValue (i, 1.0f, c.valueY); break; // held while pressed
            case SurfaceControl::Kind::checkBox: setValue (i, c.value > 0.5f ? 0.0f : 1.0f, c.valueY); break;
            case SurfaceControl::Kind::xy: mouseDrag (e); break;
            default: break;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto* controls = locate (engine.getSong());
        if (controls == nullptr || dragging < 0 || dragging >= (int) controls->size())
            return;
        const auto i = dragging;
        const auto& c = (*controls)[(std::size_t) i];
        if (moving)
        {
            const auto at = e.position - grabAt;
            const auto x = juce::jmax (0.0f, std::round (at.x / 10.0f) * 10.0f), y = juce::jmax (0.0f, std::round ((at.y - (float) top) / 10.0f) * 10.0f);
            editControls ([i, x, y] (std::vector<SurfaceControl>& cs) {
                if (i < (int) cs.size())
                    cs[(std::size_t) i].x = x, cs[(std::size_t) i].y = y;
            });
            return;
        }
        const auto r = bounds (c);
        switch (c.kind)
        {
            case SurfaceControl::Kind::knob:
            case SurfaceControl::Kind::slider:
            {
                const auto range = e.mods.isCtrlDown() || e.mods.isShiftDown() ? 1000.0f : 200.0f; // (fine: Ctrl, as every LYNX knob)
                setValue (i, grab.x + (grabAt.y - e.position.y) / range, c.valueY);
                ui::showValueTip (*this, r.toNearestInt(), juce::String (juce::roundToInt ((*controls)[(std::size_t) i].value * 100.0f)) + "%");
                break;
            }
            case SurfaceControl::Kind::xy:
            {
                const auto pad = r.withTrimmedBottom (18.0f);
                setValue (i, (e.position.x - pad.getX()) / pad.getWidth(), (pad.getBottom() - e.position.y) / pad.getHeight());
                break;
            }
            default: break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        const auto* controls = locate (engine.getSong());
        if (controls != nullptr && dragging >= 0 && dragging < (int) controls->size() && ! moving
            && (*controls)[(std::size_t) dragging].kind == SurfaceControl::Kind::button)
            setValue (dragging, 0.0f, (*controls)[(std::size_t) dragging].valueY); // released
        dragging = -1;
        moving = false;
        ui::hideValueTip();
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const auto* controls = locate (engine.getSong());
        const auto i = controlAt (e.position);
        if (controls == nullptr || i < 0)
            return;
        const auto& c = (*controls)[(std::size_t) i];
        if (c.kind == SurfaceControl::Kind::knob || c.kind == SurfaceControl::Kind::slider)
            setValue (i, c.value + wheel.deltaY * (e.mods.isCtrlDown() ? 0.01f : 0.05f), c.valueY);
    }

private:
    static constexpr int top = 34;

    juce::Rectangle<float> bounds (const SurfaceControl& c) const
    {
        const auto x = c.x, y = c.y + (float) top;
        const auto s = std::max (0.5f, c.size);
        switch (c.kind)
        {
            case SurfaceControl::Kind::knob: return { x, y, 56.0f * s, 56.0f * s + 14.0f };
            case SurfaceControl::Kind::slider: return { x, y, 40.0f, 140.0f * s };
            case SurfaceControl::Kind::button: return { x, y, 80.0f * s, 30.0f };
            case SurfaceControl::Kind::checkBox: return { x, y, 110.0f * s, 24.0f };
            case SurfaceControl::Kind::xy: return { x, y, 140.0f * s, 140.0f * s + 18.0f };
        }
        return { x, y, 56.0f, 70.0f };
    }

    int controlAt (juce::Point<float> p) const
    {
        if (const auto* controls = locate (engine.getSong()))
            for (int i = (int) controls->size() - 1; i >= 0; --i)
                if (bounds ((*controls)[(std::size_t) i]).contains (p))
                    return i;
        return -1;
    }

    void setValue (int i, float v, float vy)
    {
        editControls ([i, v, vy] (std::vector<SurfaceControl>& cs) { surface::setValue (cs, i, v, vy); });
    }

    void showAddMenu (juce::Point<float> at, juce::Component* from)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Add control"));
        const std::pair<SurfaceControl::Kind, const char*> kinds[] = { { SurfaceControl::Kind::knob, TR_NOOP ("Knob") },
                                                                       { SurfaceControl::Kind::slider, TR_NOOP ("Slider") },
                                                                       { SurfaceControl::Kind::button, TR_NOOP ("Button") },
                                                                       { SurfaceControl::Kind::checkBox, TR_NOOP ("Check box") },
                                                                       { SurfaceControl::Kind::xy, TR_NOOP ("XY pad") } };
        // a free spot: right of the last control when added from the button
        if (from != nullptr)
            if (const auto* controls = locate (engine.getSong()); controls != nullptr && ! controls->empty())
            {
                const auto r = bounds (controls->back());
                at = r.getRight() + 20.0f < (float) getWidth() - 60.0f ? juce::Point<float> (r.getRight() + 20.0f, r.getY()) : juce::Point<float> (20.0f, r.getBottom() + 20.0f);
            }
        for (const auto& [kind, name] : kinds)
            menu.addItem (tr (name), [this, kind = kind, english = juce::String (name), at] {
                editControls ([kind, english, at] (std::vector<SurfaceControl>& cs) {
                    int same = 1;
                    for (const auto& c : cs)
                        same += c.kind == kind ? 1 : 0;
                    SurfaceControl c;
                    c.kind = kind;
                    c.name = (english + " " + juce::String (same)).toStdString();
                    c.value = kind == SurfaceControl::Kind::knob || kind == SurfaceControl::Kind::slider ? 0.5f : 0.0f;
                    c.valueY = 0.5f;
                    c.x = std::round (at.x / 10.0f) * 10.0f;
                    c.y = juce::jmax (0.0f, std::round ((at.y - (float) top) / 10.0f) * 10.0f);
                    if (surface::firstOutput (cs, (int) cs.size()) + c.outputs() <= surface::maxOutputs)
                        cs.push_back (c);
                });
            });
        menu.showMenuAsync (from != nullptr ? juce::PopupMenu::Options().withTargetComponent (from)
                                            : juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showControlMenu (int i)
    {
        juce::PopupMenu menu;
        const auto* controls = locate (engine.getSong());
        if (controls == nullptr || i >= (int) controls->size())
            return;
        const auto output = surface::firstOutput (*controls, i);
        if (const auto t = target (output))
        {
            addAutomationItem (menu, engine, *t);
            menu.addSeparator();
        }
        menu.addItem (tr ("Rename..."), [this, i] { rename (i); });
        juce::PopupMenu size;
        for (const auto s : { 0.75f, 1.0f, 1.5f, 2.0f })
            size.addItem (juce::String (juce::roundToInt (s * 100.0f)) + "%", true, std::abs ((*controls)[(std::size_t) i].size - s) < 0.01f,
                          [this, i, s] {
                              editControls ([i, s] (std::vector<SurfaceControl>& cs) {
                                  if (i < (int) cs.size())
                                      cs[(std::size_t) i].size = s;
                              });
                          });
        menu.addSubMenu (tr ("Size"), size);
        menu.addItem (tr ("Delete"), [this, i] {
            editControls ([i] (std::vector<SurfaceControl>& cs) {
                if (i < (int) cs.size())
                    cs.erase (cs.begin() + i);
            });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void rename (int i)
    {
        const auto* controls = locate (engine.getSong());
        if (controls == nullptr || i >= (int) controls->size())
            return;
        auto* window = new juce::AlertWindow (tr ("Rename"), {}, juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("name", utf8 ((*controls)[(std::size_t) i].name));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<SurfacePanel> safe (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window, i] (int result) {
                                     if (result != 1 || safe == nullptr)
                                         return;
                                     const auto name = window->getTextEditorContents ("name").trim().toStdString();
                                     if (! name.empty())
                                         safe->editControls ([i, name] (std::vector<SurfaceControl>& cs) {
                                             if (i < (int) cs.size())
                                                 cs[(std::size_t) i].name = name;
                                         });
                                 }),
                                 true);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    AudioEngine& engine;
    juce::String heading;
    Locate locate;
    Edit editControls;
    std::function<std::optional<AutomationTarget> (int)> target;
    juce::TextButton editButton, addButton;
    int dragging = -1;
    bool moving = false;
    juce::Point<float> grab, grabAt;
};

// ===================================================================================================
// windows (mixer slots)
// ===================================================================================================

class ControllerWindow final : public juce::DocumentWindow
{
public:
    ControllerWindow (AudioEngine& engine, int track, int slot, const juce::String& title, std::unique_ptr<juce::Component> content)
        : DocumentWindow (title, theme::colour::bg2, DocumentWindow::closeButton),
          gone (engine, track, slot, [this] { closeButtonPressed(); })
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content.release(), true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
    }

    void closeButtonPressed() override;

private:
    ui::SlotGoneWatcher gone;
};

std::map<std::pair<int, int>, std::unique_ptr<ControllerWindow>>& windows()
{
    static std::map<std::pair<int, int>, std::unique_ptr<ControllerWindow>> open;
    return open;
}

void ControllerWindow::closeButtonPressed()
{
    for (auto& [key, window] : windows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { windows().erase (k); });
            return;
        }
}

SlotLocator mixerSlot (int track, int slot)
{
    return [track, slot] (Song& s) -> EffectSlot* {
        if (track < 0 || track >= (int) s.mixer.size() || slot < 0 || slot >= (int) s.mixer[(std::size_t) track].effects.size())
            return nullptr;
        return &s.mixer[(std::size_t) track].effects[(std::size_t) slot];
    };
}
} // namespace

std::unique_ptr<juce::Component> makeXyControllerEditor (AudioEngine& engine, SlotLocator locate, OutputReader output, std::optional<AutomationTarget> target)
{
    return std::make_unique<XyEditor> (engine, std::move (locate), std::move (output), target);
}

std::unique_ptr<juce::Component> makeControlSurfaceEditor (AudioEngine& engine, SlotLocator locate, std::optional<AutomationTarget> target)
{
    return std::make_unique<SurfacePanel> (
        engine, "Control Surface",
        [locate] (const Song& s) -> const std::vector<SurfaceControl>* {
            const auto* fx = locate (const_cast<Song&> (s));
            return fx != nullptr && fx->kind == EffectKind::controlSurface ? &fx->patcher.surface : nullptr;
        },
        [&engine, locate] (std::function<void (std::vector<SurfaceControl>&)> change) {
            engine.editSong ([locate, change] (Song& s) {
                if (auto* fx = locate (s); fx != nullptr && fx->kind == EffectKind::controlSurface)
                {
                    change (fx->patcher.surface);
                    surface::syncParams (*fx);
                }
            });
        },
        [target] (int output) -> std::optional<AutomationTarget> {
            if (! target)
                return std::nullopt;
            return AutomationTarget { target->kind, target->index, target->slot, output };
        });
}

std::unique_ptr<juce::Component> makeDashboardEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate)
{
    return std::make_unique<SurfacePanel> (
        engine, "Dashboard",
        [locate] (const Song& s) -> const std::vector<SurfaceControl>* {
            const auto* c = locate (const_cast<Song&> (s));
            return c != nullptr && c->instrument == InstrumentKind::dashboard ? &c->patcher.surface : nullptr;
        },
        [&engine, locate] (std::function<void (std::vector<SurfaceControl>&)> change) {
            engine.editSong ([locate, change] (Song& s) {
                if (auto* c = locate (s); c != nullptr && c->instrument == InstrumentKind::dashboard)
                    change (c->patcher.surface);
            });
        },
        [] (int) -> std::optional<AutomationTarget> { return std::nullopt; });
}

std::unique_ptr<juce::Component> makeDashboardEditor (AudioEngine& engine, int channel)
{
    return makeDashboardEditor (engine, [channel] (Song& s) -> ChannelDef* {
        return channel >= 0 && channel < (int) s.channels.size() ? &s.channels[(std::size_t) channel] : nullptr;
    });
}

namespace Controller164Windows
{
bool handles (EffectKind kind) noexcept
{
    return xy::isXyController (kind) || kind == EffectKind::controlSurface;
}

void open (AudioEngine& engine, int track, int slot)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;
    const auto kind = mixer[(std::size_t) track].effects[(std::size_t) slot].kind;
    auto& open = windows();
    const auto key = std::make_pair (track, slot);
    if (auto it = open.find (key); it != open.end())
    {
        it->second->toFront (true);
        return;
    }
    const auto name = track == 0 ? tr ("Master") : utf8 (mixer[(std::size_t) track].name);
    const auto where = (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) })) + " (" + tr ("slot {0}", { juce::String (slot + 1) }) + ")";
    const AutomationTarget target { AutomationTargetKind::effectParam, track, slot, 0 };
    std::unique_ptr<juce::Component> content;
    if (kind == EffectKind::controlSurface)
        content = makeControlSurfaceEditor (engine, mixerSlot (track, slot), target);
    else
        content = makeXyControllerEditor (engine, mixerSlot (track, slot),
                                          [&engine, track, slot] (int k) {
                                              // the live meters the audio thread publishes (core/RackRenderer.cpp hasLiveMeters)
                                              const auto* times = engine.getProcessTimes();
                                              const auto key = (std::size_t) (track * maxEffectSlots + slot) * ProcessTimes::meterOutputs
                                                               + (std::size_t) juce::jlimit (0, ProcessTimes::meterOutputs - 1, k);
                                              return times != nullptr && key < times->effectMeters.size()
                                                         ? times->effectMeters[key].load (std::memory_order_relaxed)
                                                         : 0.0f;
                                          },
                                          target);
    open[key] = std::make_unique<ControllerWindow> (engine, track, slot, utf8 (effectInfo (kind).name) + " - " + where, std::move (content));
}

void closeAll()
{
    windows().clear();
}
} // namespace Controller164Windows
} // namespace lynx
