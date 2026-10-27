#pragma once

// The small rotary control of the controller editors (round 133; shared with the round 164 editors: Layer, X-Y / X-Y-Z
// Controller, Control Surface, Voltage Controller).

#include "engine/AudioEngine.h"
#include "ui/AutomationUi.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"

namespace lynx::ctlui
{
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

inline ui::ValueSpec unitSpec (const juce::String& name, double def, std::optional<AutomationTarget> target)
{
    ui::ValueSpec s;
    s.name = name;
    s.defaultValue = def;
    s.toText = [] (double v) { return juce::String (v, 3); };
    s.fromText = [] (const juce::String& t) -> std::optional<double> { return juce::jlimit (0.0, 1.0, t.getDoubleValue()); };
    s.target = target;
    return s;
}

} // namespace lynx::ctlui
