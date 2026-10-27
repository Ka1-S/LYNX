#include "ui/WindowOwner.h"
#include "ui/AutomationUi.h"
#include "core/LastTweaked.h"

#include <map>

#include "core/Effects.h"
#include "core/ControlSurface.h"
#include "core/XyController.h"
#include "core/MidiControls.h"
#include "core/FlSynths.h"
#include "engine/Text.h"
#include "core/EditorGestures.h"
#include "ui/EditorTools.h"
#include "ui/EventEditor.h"
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

juce::AudioPluginInstance* pluginFor (AudioEngine& engine, const AutomationTarget& t)
{
    if (t.kind == AutomationTargetKind::channelParam)
        if (auto* p = dynamic_cast<PluginInstrument*> (engine.getChannelInstrument (t.index).get()))
            return &p->getInstance();

    if (t.kind == AutomationTargetKind::effectParam)
        if (auto* p = dynamic_cast<PluginEffect*> (engine.getEffect (t.index, t.slot).get()))
            return &p->getInstance();

    return nullptr;
}

// The CC / aftertouch / pitch bend value the plugin was last sent (or the controller's rest value).
float midiControlValue (AudioEngine& engine, const AutomationTarget& t)
{
    if (t.kind == AutomationTargetKind::channelParam)
        if (auto* p = dynamic_cast<PluginInstrument*> (engine.getChannelInstrument (t.index).get()))
            return p->midiControlValue (t.param);
    if (t.kind == AutomationTargetKind::effectParam)
        if (auto* p = dynamic_cast<PluginEffect*> (engine.getEffect (t.index, t.slot).get()))
            return p->midiControlValue (t.param);
    return midictl::defaultValue (t.param);
}

// Point editor: time horizontal (bars), value vertical.
//  - click on empty space creates a point; drag moves; right-click deletes
//  - Alt+drag over a segment changes the curve (tension)
//  - Ctrl+wheel zooms at the mouse; wheel scrolls; middle-drag pans
//  - the cursor and the hint bar show what a click would do
class AutomationEditor final : public juce::Component,
                               public ui::HintSource,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    AutomationEditor (AudioEngine& e, int index)
        : engine (e),
          automation (index)
    {
        engine.addChangeListener (this);
        setSize (760, 300);
        startTimerHz (30);
    }

    ~AutomationEditor() override { engine.removeChangeListener (this); }

    int getIndex() const noexcept { return automation; }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg1);
        const auto* a = current();

        if (a == nullptr)
            return;

        const auto& song = engine.getSong();
        const auto area = plotArea();
        const auto bar = (std::int64_t) ppq * juce::jmax (1, song.beatsPerBar);

        g.setColour (colour::bg0);
        g.fillRect (area);

        // grid: beats and bars
        const auto beatPixels = ppq * pixelsPerTick;
        const auto lastTick = (std::int64_t) xToTick (area.getRight());

        for (auto tick = (std::int64_t) std::floor (scroll / ppq) * ppq; tick <= lastTick; tick += ppq)
        {
            const auto isBar = tick % bar == 0;

            if (! isBar && beatPixels < 8.0)
                continue;

            const auto x = (int) tickToX ((double) tick);
            g.setColour (isBar ? colour::gridBar : colour::gridBeat);
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());

            if (isBar)
            {
                g.setColour (colour::textSecondary);
                g.setFont (font (type::caption));
                g.drawText (juce::String (tick / bar + 1), x + 3, 2, 40, 16, juce::Justification::centredLeft);
            }
        }

        for (auto v : { 0.25f, 0.5f, 0.75f })
        {
            g.setColour (colour::gridStep.brighter (0.15f));
            g.drawHorizontalLine ((int) valueToY (v), (float) area.getX(), (float) area.getRight());
        }

        // curve
        juce::Path curve;
        const auto& points = a->points;

        for (auto x = area.getX(); x <= area.getRight(); x += 2)
        {
            const auto y = valueToY (a->valueAt (xToTick (x)));

            if (x == area.getX())
                curve.startNewSubPath ((float) x, y);
            else
                curve.lineTo ((float) x, y);
        }

        // soft fill under the curve, then the line
        auto fill = curve;
        fill.lineTo ((float) area.getRight(), (float) area.getBottom());
        fill.lineTo ((float) area.getX(), (float) area.getBottom());
        fill.closeSubPath();
        g.setColour (colour::automation.withAlpha (0.10f));
        g.fillPath (fill);
        g.setColour (colour::automation);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const auto p = pointPosition (points[i]);
            const auto active = (int) i == hoverPoint || (int) i == dragPoint;
            g.setColour (colour::bg0);
            g.fillEllipse (p.x - 6.0f, p.y - 6.0f, 12.0f, 12.0f);
            g.setColour (active ? colour::textPrimary : colour::automation);
            g.fillEllipse (p.x - 4.5f, p.y - 4.5f, 9.0f, 9.0f);
        }

        // playback cursor (at the position of the clip currently playing)
        if (playhead >= 0.0)
        {
            g.setColour (colour::playhead);
            g.drawVerticalLine ((int) tickToX (playhead), (float) area.getY(), (float) area.getBottom());
        }


        if (hoverPoint >= 0 && hoverPoint < (int) points.size())
        {
            const auto p = pointPosition (points[(std::size_t) hoverPoint]);
            g.setColour (colour::textPrimary);
            g.setFont (font (type::small));
            const auto value = points[(std::size_t) hoverPoint].value;
            g.drawText (a->target.kind == AutomationTargetKind::tempo ? juce::String (tempoFromValue (value), 1) + " BPM"
                                                                       : juce::String (juce::roundToInt (value * 100.0f)) + "%",
                        (int) p.x + 8, (int) p.y - 18, 60, 14, juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto hit = pointAt (e.position);

        if (hit != hoverPoint)
        {
            hoverPoint = hit;
            repaint();
        }

        updateCursor (e.mods);
    }

    void modifierKeysChanged (const juce::ModifierKeys& mods) override
    {
        if (dragPoint < 0 && tensionPoint < 0 && ! panning)
            updateCursor (mods);
    }

    void updateCursor (juce::ModifierKeys mods)
    {
        using C = gesture::Cursor;
        const auto inPlot = plotArea().contains (getMouseXYRelative());

        if (inPlot && mods.isAltDown())
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor); // drag up / down bends the segment
        else
            setMouseCursor (editorCursor (! inPlot ? C::normal : hoverPoint >= 0 ? C::move : C::draw));
    }

    ui::Hint hintAt (juce::Point<int> position) override
    {
        ui::Hint hint;

        if (! plotArea().contains (position))
            return hint;

        const auto* a = current();
        const auto point = pointAt (position.toFloat());
        hint.hasMenu = true;

        if (juce::ModifierKeys::currentModifiers.isAltDown())
            hint.text = tr ("[Alt][LMB] drag up / down bends this segment");
        else if (point >= 0)
            hint.text = tr ("Point: [LMB] drag moves, then [Shift] keeps time, [Ctrl] keeps value  [RMB] menu");
        else
            hint.text = tr ("[LMB] new point  [Shift][RMB] point on the curve  [Alt][LMB] bend  [Ctrl][Wheel] zoom  [MMB] scroll");

        if (a != nullptr)
            hint.value = valueText (point >= 0 ? a->points[(std::size_t) point].value : yToValue (position.y));
        return hint;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto* a = current();

        if (a == nullptr || ! plotArea().contains (e.getPosition()))
            return;

        const auto hit = pointAt (e.position);

        downMods = e.mods;

        if (e.mods.isMiddleButtonDown()) // middle-drag scrolls
        {
            panning = true;
            panStart = scroll;
            setMouseCursor (editorCursor (gesture::Cursor::pan));
            return;
        }

        if (e.mods.isPopupMenu())
        {
            if (e.mods.isShiftDown() && hit < 0) // a point on the curve, keeping its level
            {
                AutomationPoint p;
                p.tick = snap (xToTick (e.x), true);
                p.value = a->valueAt ((double) p.tick);
                edit ([p] (Automation& x) {
                    x.points.push_back (p);
                    std::stable_sort (x.points.begin(), x.points.end(), [] (auto& l, auto& r) { return l.tick < r.tick; });
                });
                return;
            }

            if (hit >= 0)
                showPointMenu (hit);
            else
                showCurveMenu();
            return;
        }

        if (e.mods.isAltDown())
        {
            // segment under the mouse: the point on the left holds the tension
            tensionPoint = -1;
            const auto tick = xToTick (e.x);

            for (int i = 0; i + 1 < (int) a->points.size(); ++i)
                if (tick >= (double) a->points[(std::size_t) i].tick && tick < (double) a->points[(std::size_t) i + 1].tick)
                    tensionPoint = i;

            if (tensionPoint >= 0)
                tensionStart = a->points[(std::size_t) tensionPoint].tension;
            return;
        }

        if (hit >= 0)
        {
            dragPoint = hit;
            return;
        }

        AutomationPoint p;
        p.tick = snap (xToTick (e.x), ! e.mods.isShiftDown());
        p.value = yToValue (e.y);
        edit ([p] (Automation& x) {
            x.points.push_back (p);
            std::stable_sort (x.points.begin(), x.points.end(), [] (auto& l, auto& r) { return l.tick < r.tick; });
        });

        for (int i = 0; i < (int) current()->points.size(); ++i)
            if (current()->points[(std::size_t) i].tick == p.tick && current()->points[(std::size_t) i].value == p.value)
                dragPoint = i;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto* a = current();

        if (a == nullptr)
            return;

        if (panning)
        {
            scroll = juce::jmax (0.0, panStart - e.getDistanceFromDragStartX() / pixelsPerTick);
            repaint();
            return;
        }

        if (tensionPoint >= 0 && tensionPoint < (int) a->points.size())
        {
            const auto tension = juce::jlimit (-1.0f, 1.0f, tensionStart + (float) e.getDistanceFromDragStartY() / 100.0f);
            edit ([i = tensionPoint, tension] (Automation& x) { x.points[(std::size_t) i].tension = tension; });
            return;
        }

        if (dragPoint < 0 || dragPoint >= (int) a->points.size())
            return;

        // the point can't pass its neighbours (order doesn't change while dragging)
        const auto& points = a->points;
        auto tick = snap (xToTick (e.x), ! e.mods.isShiftDown());
        const auto lo = dragPoint > 0 ? points[(std::size_t) dragPoint - 1].tick : 0;
        const auto hi = dragPoint + 1 < (int) points.size() ? points[(std::size_t) dragPoint + 1].tick : std::numeric_limits<std::int64_t>::max();
        tick = juce::jlimit (lo, hi, tick);
        auto value = yToValue (e.y);

        // locks: Shift pressed after the click keeps the time, Ctrl keeps the value
        if (e.mods.isShiftDown() && ! downMods.isShiftDown())
            tick = points[(std::size_t) dragPoint].tick;
        if (e.mods.isCtrlDown() && ! downMods.isCtrlDown())
            value = points[(std::size_t) dragPoint].value;

        edit ([i = dragPoint, tick, value] (Automation& x) {
            x.points[(std::size_t) i].tick = tick;
            x.points[(std::size_t) i].value = value;
        });
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        dragPoint = tensionPoint = -1;
        panning = false;
        updateCursor (e.mods);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (e.mods.isCtrlDown())
        {
            const auto anchor = xToTick (e.x);
            juce::ignoreUnused (anchor);
            const auto zoomed = juce::jlimit (4.0 / (4 * ppq), 600.0 / (4 * ppq), pixelsPerTick * gesture::wheelZoomFactor (wheel.deltaY, wheel.isSmooth));
            scroll = gesture::zoomAnchored (scroll, pixelsPerTick, zoomed, (double) juce::jmax (0, e.x - plotArea().getX()));
            pixelsPerTick = zoomed;
        }
        else
            scroll = juce::jmax (0.0, scroll - (wheel.deltaY + wheel.deltaX) * 300.0 / pixelsPerTick);

        repaint();
    }

private:
    juce::ModifierKeys downMods;
    static inline float copiedValue = -1.0f;

    juce::String valueText (float v) const
    {
        const auto* a = current();
        return a != nullptr && a->target.kind == AutomationTargetKind::tempo ? juce::String (tempoFromValue (v), 2) + " BPM"
                                                                             : juce::String (v * 100.0f, 1) + "%";
    }

    void showPointMenu (int point)
    {
        const auto* a = current();
        const auto value = a->points[(std::size_t) point].value;
        juce::PopupMenu menu;
        menu.addSectionHeader (valueText (value));
        menu.addItem (tr ("Type value..."), [this, point] { typeValue (point); });
        menu.addItem (tr ("Copy value"), [value] { copiedValue = value; });
        menu.addItem (tr ("Paste value"), copiedValue >= 0.0f, false, [this, point] {
            edit ([point, v = copiedValue] (Automation& x) {
                if (point < (int) x.points.size())
                    x.points[(std::size_t) point].value = v;
            });
        });
        menu.addItem (tr ("Straight segment (reset the curve)"), [this, point] {
            edit ([point] (Automation& x) {
                if (point < (int) x.points.size())
                    x.points[(std::size_t) point].tension = 0.0f;
            });
        });
        menu.addSeparator();
        menu.addItem (tr ("Delete point"), a->points.size() > 1, false, [this, point] {
            edit ([point] (Automation& x) {
                if (point < (int) x.points.size() && x.points.size() > 1)
                    x.points.erase (x.points.begin() + point);
            });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void typeValue (int point)
    {
        const auto* a = current();
        const auto tempo = a->target.kind == AutomationTargetKind::tempo;
        const auto value = a->points[(std::size_t) point].value;
        auto* window = new juce::AlertWindow (tr ("Point value"), tempo ? tr ("BPM") : tr ("Percent (0 to 100)"), juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("value", tempo ? juce::String (tempoFromValue (value), 2) : juce::String (value * 100.0f, 1));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([this, point, tempo, window] (int result) {
                                     if (result != 1)
                                         return;
                                     const auto typed = window->getTextEditorContents ("value").retainCharacters ("0123456789.,-").replaceCharacter (',', '.').getDoubleValue();
                                     const auto v = juce::jlimit (0.0f, 1.0f, tempo ? valueFromTempo (typed) : (float) typed / 100.0f);
                                     edit ([point, v] (Automation& x) {
                                         if (point < (int) x.points.size())
                                             x.points[(std::size_t) point].value = v;
                                     });
                                 }),
                                 true);
    }

    void showCurveMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (tr ("Scale the values into a range..."), [this] { rerange(); });
        menu.addItem (tr ("Flip the values upside down"), [this] {
            edit ([] (Automation& x) {
                for (auto& p : x.points)
                    p.value = 1.0f - p.value;
            });
        });
        menu.addItem (tr ("All segments straight"), [this] {
            edit ([] (Automation& x) {
                for (auto& p : x.points)
                    p.tension = 0.0f;
            });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void rerange()
    {
        auto* window = new juce::AlertWindow (tr ("Scale the values into a range"), tr ("The lowest point goes to the minimum and the highest to the maximum (percent)."),
                                              juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("min", "0", tr ("Minimum"));
        window->addTextEditor ("max", "100", tr ("Maximum"));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result) {
                                     if (result != 1)
                                         return;
                                     const auto lo = juce::jlimit (0.0f, 1.0f, window->getTextEditorContents ("min").getFloatValue() / 100.0f);
                                     const auto hi = juce::jlimit (0.0f, 1.0f, window->getTextEditorContents ("max").getFloatValue() / 100.0f);
                                     edit ([lo, hi] (Automation& x) {
                                         if (x.points.empty())
                                             return;
                                         auto [mn, mx] = std::minmax_element (x.points.begin(), x.points.end(), [] (auto& a, auto& b) { return a.value < b.value; });
                                         const auto from = mn->value, span = mx->value - mn->value;
                                         for (auto& p : x.points)
                                             p.value = span > 1e-6f ? lo + (p.value - from) / span * (hi - lo) : lo;
                                     });
                                 }),
                                 true);
    }

    const Automation* current() const
    {
        const auto& song = engine.getSong();
        return automation >= 0 && automation < (int) song.automations.size() ? &song.automations[(std::size_t) automation] : nullptr;
    }

    void edit (const std::function<void (Automation&)>& fn)
    {
        engine.editSong ([this, &fn] (Song& s) {
            if (automation >= 0 && automation < (int) s.automations.size())
                fn (s.automations[(std::size_t) automation]);
        });
    }

    juce::Rectangle<int> plotArea() const { return getLocalBounds().withTrimmedTop (18).withTrimmedBottom (4).reduced (8, 4); }
    double tickToX (double tick) const { return plotArea().getX() + (tick - scroll) * pixelsPerTick; }
    double xToTick (int x) const { return juce::jmax (0.0, (x - plotArea().getX()) / pixelsPerTick + scroll); }
    float valueToY (float v) const { return (float) plotArea().getBottom() - v * (float) plotArea().getHeight(); }
    float yToValue (int y) const { return juce::jlimit (0.0f, 1.0f, (float) (plotArea().getBottom() - y) / (float) plotArea().getHeight()); }

    std::int64_t snap (double tick, bool on) const
    {
        const auto grid = on ? (double) ticksPerStep : 1.0;
        return (std::int64_t) std::round (tick / grid) * (std::int64_t) grid;
    }

    juce::Point<float> pointPosition (const AutomationPoint& p) const { return { (float) tickToX ((double) p.tick), valueToY (p.value) }; }

    int pointAt (juce::Point<float> pos) const
    {
        const auto* a = current();

        if (a == nullptr)
            return -1;

        for (int i = (int) a->points.size() - 1; i >= 0; --i)
            if (pointPosition (a->points[(std::size_t) i]).getDistanceFrom (pos) < 8.0f)
                return i;

        return -1;
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (current() == nullptr) // automation removed: close
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Component> (getParentComponent())] {
                if (auto* w = dynamic_cast<juce::DocumentWindow*> (safe.getComponent()))
                    w->closeButtonPressed();
            });
            return;
        }

        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (tr ("Automation - {0}", { juce::String::fromUTF8 (current()->name.c_str()) }));

        repaint();
    }

    void timerCallback() override
    {
        // position within the automation, if one of its clips is under the cursor
        auto tick = -1.0;
        const auto& song = engine.getSong();

        if (song.mode == PlayMode::song && engine.getTransport().playing.load())
        {
            const auto position = std::fmod (engine.getTransport().positionTicks.load(), (double) juce::jmax<std::int64_t> (1, song.songLength()));

            for (const auto& c : song.playlist)
                if (c.kind == ClipKind::automation && c.automation == automation && position >= (double) c.start && position < (double) c.end())
                    tick = position - (double) c.start + (double) c.offset;
        }

        if (tick != playhead)
        {
            playhead = tick;
            repaint();
        }
    }

    AudioEngine& engine;
    int automation;
    double pixelsPerTick = 120.0 / (4 * ppq);
    double scroll = 0.0;
    int dragPoint = -1, hoverPoint = -1, tensionPoint = -1;
    bool panning = false;
    double panStart = 0.0;
    float tensionStart = 0.0f;
    double playhead = -1.0;
};

class AutomationWindow final : public juce::DocumentWindow
{
public:
    AutomationWindow (AudioEngine& engine, int index, const juce::String& title)
        : juce::DocumentWindow (title, theme::colour::bg1, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AutomationEditor (engine, index), true);
        setResizable (true, false);
        setResizeLimits (400, 200, 4000, 2000);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
    }

    int getIndex() const { return static_cast<AutomationEditor*> (getContentComponent())->getIndex(); }

    void closeButtonPressed() override;
};

std::vector<std::unique_ptr<AutomationWindow>>& windows()
{
    static std::vector<std::unique_ptr<AutomationWindow>> open;
    return open;
}

void AutomationWindow::closeButtonPressed()
{
    auto* self = this;
    juce::MessageManager::callAsync ([self] {
        auto& open = windows();
        open.erase (std::remove_if (open.begin(), open.end(), [self] (auto& w) { return w.get() == self; }), open.end());
    });
}
} // namespace

juce::String describeAutomationTarget (AudioEngine& engine, const AutomationTarget& t)
{
    const auto& song = engine.getSong();
    const auto channelName = t.index >= 0 && t.index < (int) song.channels.size() ? nameOf (song.channels[(std::size_t) t.index].name) : juce::String ("?");
    const auto trackName = t.index >= 0 && t.index < (int) song.mixer.size() ? nameOf (song.mixer[(std::size_t) t.index].name) : juce::String ("?");

    switch (t.kind)
    {
        case AutomationTargetKind::channelVolume: return channelName + " - " + tr ("Volume");
        case AutomationTargetKind::channelPan: return channelName + " - " + tr ("Pan");
        case AutomationTargetKind::mixerVolume: return trackName + " - " + tr ("Volume");
        case AutomationTargetKind::mixerPan: return trackName + " - " + tr ("Pan");

        case AutomationTargetKind::channelParam:
        case AutomationTargetKind::effectParam:
        {
            juce::String param = tr ("Param {0}", { juce::String (t.param + 1) });

            if (midictl::isMidiParam (t.param))
                param = midiParamLabel (t.param);
            else if (auto* plugin = pluginFor (engine, t))
            {
                if (t.param >= 0 && t.param < plugin->getParameters().size())
                    param = plugin->getParameters()[t.param]->getName (40);
            }
            else if (t.kind == AutomationTargetKind::effectParam && t.index < (int) song.mixer.size()
                     && t.slot < (int) song.mixer[(std::size_t) t.index].effects.size())
            {
                const auto& info = effectInfo (song.mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].kind);

                if (t.param >= 0 && t.param < (int) info.params.size())
                    param = utf8 (info.params[(std::size_t) t.param].name);
            }

            return (t.kind == AutomationTargetKind::channelParam ? channelName : trackName) + " - " + param;
        }

        case AutomationTargetKind::tempo: return tr ("Tempo");
        case AutomationTargetKind::effectMix:
        {
            juce::String effect = "FX " + juce::String (t.slot + 1);
            if (t.index < (int) song.mixer.size() && t.slot >= 0 && t.slot < (int) song.mixer[(std::size_t) t.index].effects.size())
                effect = utf8 (effectInfo (song.mixer[(std::size_t) t.index].effects[(std::size_t) t.slot].kind).name);
            return trackName + " - " + effect + " - " + tr ("Mix");
        }
        case AutomationTargetKind::none: break;
    }

    return tr ("(no target)");
}

juce::String describeTargetValue (AudioEngine& engine, const AutomationTarget& target)
{
    const auto value = engine.currentTargetValue (target);
    if (value < 0.0f)
        return {};
    if (target.kind == AutomationTargetKind::channelParam || target.kind == AutomationTargetKind::effectParam)
    {
        if (midictl::isMidiParam (target.param))
            return juce::String (juce::roundToInt (value * 127.0f));
        if (auto* plugin = pluginFor (engine, target))
        {
            if (target.param < 0 || target.param >= plugin->getParameters().size())
                return {};
            auto* p = plugin->getParameters()[target.param];
            auto text = p->getCurrentValueAsText().trim();
            if (text.isEmpty())
                text = juce::String (value * 100.0f, 1) + "%";
            else if (const auto label = p->getLabel().trim(); label.isNotEmpty() && ! text.endsWith (label))
                text << " " << label;
            return text;
        }
    }
    const auto text = lasttweak::valueText (engine.getSong(), target, value);
    return text.empty() ? juce::String (juce::roundToInt (value * 100.0f)) + "%" : juce::String::fromUTF8 (text.c_str());
}

void createAutomationClip (AudioEngine& engine, const AutomationTarget& target)
{
    auto value = engine.getSong().targetValue (target);

    if (midictl::isMidiParam (target.param) && (target.kind == AutomationTargetKind::channelParam || target.kind == AutomationTargetKind::effectParam))
        value = midiControlValue (engine, target);
    else if (auto* plugin = pluginFor (engine, target); plugin != nullptr && target.param >= 0 && target.param < plugin->getParameters().size())
        value = plugin->getParameters()[target.param]->getValue();

    if (value < 0.0f)
        return;

    const auto name = describeAutomationTarget (engine, target).toStdString();

    engine.editSong ([&] (Song& s) {
        // the Playlist time selection gives the clip its place and length; otherwise the whole song
        const auto bar = (std::int64_t) ppq * std::max (1, s.beatsPerBar);
        const auto length = s.hasLoop() ? s.loopEnd - s.loopStart : s.playlist.empty() ? bar * 4 : s.songLength();

        Automation a;
        a.name = name;
        a.target = target;
        a.points = { { 0, value, 0.0f }, { length, value, 0.0f } };
        s.automations.push_back (std::move (a));

        int track = 0;
        for (const auto& c : s.playlist)
            track = std::max (track, c.track + 1);

        Clip clip;
        clip.kind = ClipKind::automation;
        clip.automation = (int) s.automations.size() - 1;
        clip.track = track;
        clip.start = s.hasLoop() ? s.loopStart : 0;
        clip.length = length;
        s.playlist.push_back (clip);
    });

    openAutomationEditor (engine, (int) engine.getSong().automations.size() - 1);
}

void openAutomationEditor (AudioEngine& engine, int index)
{
    if (index < 0 || index >= (int) engine.getSong().automations.size())
        return;

    for (auto& w : windows())
        if (w->getIndex() == index)
        {
            w->toFront (true);
            return;
        }

    windows().push_back (std::make_unique<AutomationWindow> (
        engine, index, tr ("Automation - {0}", { nameOf (engine.getSong().automations[(std::size_t) index].name) })));
}

void closeAutomationEditors()
{
    windows().clear();
}

std::unique_ptr<juce::Component> makeAutomationEditor (AudioEngine& engine, int automation)
{
    return std::make_unique<AutomationEditor> (engine, automation);
}

std::vector<ControllerOutput> controllerOutputs (const Song& song)
{
    std::vector<ControllerOutput> out;
    for (int c = 0; c < (int) song.channels.size(); ++c)
    {
        const auto& ch = song.channels[(std::size_t) c];
        if (ch.instrument == InstrumentKind::dashboard) // round 164: one output per control
        {
            const auto outputs = surface::firstOutput (ch.patcher.surface, (int) ch.patcher.surface.size());
            for (int k = 0; k < outputs; ++k)
                out.push_back ({ -1, 0, k, c, nameOf (ch.name) + " - " + nameOf (surface::outputName (ch.patcher.surface, k)) });
            continue;
        }
        if (ch.instrument != InstrumentKind::controller)
            continue;
        const auto name = nameOf (ch.name);
        if (parseKeyboardController (ch.controllerState))
        {
            out.push_back ({ -1, 0, 0, c, name + " - " + tr ("Key value") });
            continue;
        }
        for (int k = 0; k < 8; ++k)
            out.push_back ({ -1, 0, k, c, name + " - " + tr ("Output {0}", { juce::String (k + 1) }) });
    }
    for (int t = 0; t < (int) song.mixer.size(); ++t)
        for (int s = 0; s < (int) song.mixer[(std::size_t) t].effects.size(); ++s)
        {
            const auto kind = song.mixer[(std::size_t) t].effects[(std::size_t) s].kind;
            const auto track = t == 0 ? tr ("Master") : nameOf (song.mixer[(std::size_t) t].name);
            const auto where = (track.isNotEmpty() ? track : tr ("Insert {0}", { juce::String (t) })) + " (" + juce::String (s + 1) + ")";
            const auto& fx = song.mixer[(std::size_t) t].effects[(std::size_t) s];
            if (kind == EffectKind::formulaController)
                out.push_back ({ t, s, 0, -1, tr ("Formula Controller") + " " + where });
            else if (xy::isXyController (kind)) // round 164: X, Y (, Z)
            {
                static const char* axes[] = { "X", "Y", "Z" };
                for (int a = 0; a < xy::axesOf (kind); ++a)
                    out.push_back ({ t, s, a, -1, juce::String (effectInfo (kind).name) + " " + where + " - " + axes[a] });
            }
            else if (kind == EffectKind::controlSurface) // round 164: one output per control
            {
                const auto outputs = surface::firstOutput (fx.patcher.surface, (int) fx.patcher.surface.size());
                for (int k = 0; k < outputs; ++k)
                    out.push_back ({ t, s, k, -1, "Control Surface " + where + " - " + nameOf (surface::outputName (fx.patcher.surface, k)) });
            }
            else if (kind == EffectKind::peakController)
            {
                out.push_back ({ t, s, 1, -1, tr ("Peak Controller") + " " + where + " - " + tr ("Peak") });
                out.push_back ({ t, s, 2, -1, tr ("Peak Controller") + " " + where + " - " + tr ("LFO") });
                out.push_back ({ t, s, 0, -1, tr ("Peak Controller") + " " + where + " - " + tr ("Peak + LFO") });
            }
        }
    return out;
}

void linkToController (AudioEngine& engine, const AutomationTarget& target, const ControllerOutput& source)
{
    const auto name = describeAutomationTarget (engine, target).toStdString() + " <- " + source.label.toStdString();
    engine.editSong ([&] (Song& s) {
        // one controller per target (FL's Link dialog replaces the old link)
        for (int i = (int) s.automations.size() - 1; i >= 0; --i)
            if (s.automations[(std::size_t) i].isLive() && s.automations[(std::size_t) i].target == target)
                s.removeAutomation (i);
        Automation a;
        a.name = name;
        a.target = target;
        a.source.track = source.track;
        a.source.slot = source.slot;
        a.source.output = source.output;
        a.source.channel = source.channel;
        s.automations.push_back (std::move (a));
    });
}

void addControllerLinkItems (juce::PopupMenu& menu, AudioEngine& engine, const AutomationTarget& target)
{
    const auto& song = engine.getSong();
    const auto outputs = controllerOutputs (song);
    const auto current = std::find_if (song.automations.begin(), song.automations.end(),
                                       [&] (const Automation& a) { return a.isLive() && a.target == target; });
    juce::PopupMenu sub;
    for (const auto& o : outputs)
    {
        const auto ticked = current != song.automations.end() && current->source.track == o.track && current->source.slot == o.slot
                            && current->source.output == o.output && current->source.channel == o.channel;
        sub.addItem (o.label, true, ticked, [&engine, target, o] { linkToController (engine, target, o); });
    }
    if (outputs.empty())
        sub.addItem (tr ("(add an Envelope, Formula or Peak Controller first)"), false, false, [] {});
    if (current != song.automations.end())
    {
        sub.addSeparator();
        sub.addItem (tr ("Remove controller link"), [&engine, target] {
            engine.editSong ([target] (Song& s) {
                for (int i = (int) s.automations.size() - 1; i >= 0; --i)
                    if (s.automations[(std::size_t) i].isLive() && s.automations[(std::size_t) i].target == target)
                        s.removeAutomation (i);
            });
        });
    }
    menu.addSubMenu (tr ("Link to internal controller"), sub);
}

std::function<void (const AutomationTarget&)> onEditEventsInPianoRoll;

void addAutomationItem (juce::PopupMenu& menu, AudioEngine& engine, const AutomationTarget& target, bool headers)
{
    if (headers)
        menu.addSectionHeader (tr ("Automation"));
    if (target.kind != AutomationTargetKind::tempo)
    {
        menu.addItem (tr ("Edit events in the pattern"), [&engine, target] { openEventEditor (engine, target); });
        if (onEditEventsInPianoRoll)
            menu.addItem (tr ("Edit events in piano roll"), [target] {
                if (onEditEventsInPianoRoll)
                    onEditEventsInPianoRoll (target);
            });
    }
    // FL's initialized controls (REA round 146): the value the song resets this control to whenever it starts
    const auto initial = engine.getSong().findInitialValue (target);
    menu.addItem (tr ("Init song with this position"), engine.currentTargetValue (target) >= 0.0f, initial >= 0, [&engine, target] {
        const auto value = engine.currentTargetValue (target);
        if (value >= 0.0f)
            engine.editSong ([target, value] (Song& s) { s.setInitialValue (target, value); });
    });
    menu.addItem (tr ("Delete initial value"), initial >= 0, false, [&engine, target] {
        engine.editSong ([target] (Song& s) { s.removeInitialValue (target); });
    });
    menu.addItem (tr ("Create automation clip"), [&engine, target] { createAutomationClip (engine, target); });
    {
        // FL's "Edit automation clip..." (the clip that automates this control)
        const auto& automations = engine.getSong().automations;
        int clip = -1;
        for (int a = 0; a < (int) automations.size(); ++a)
            if (automations[(std::size_t) a].target == target && ! automations[(std::size_t) a].isLive())
                clip = a;
        menu.addItem (tr ("Edit automation clip..."), clip >= 0, false, [&engine, clip] { openAutomationEditor (engine, clip); });
    }
    if (headers)
        menu.addSectionHeader (tr ("Remote control"));
    menu.addItem (tr ("Link to MIDI controller (move a knob on the controller)"), [&engine, target] {
        engine.learnMidiController (target);
    });
    // FL's "Override global link..." (form "Global link settings"): a link for every project, kept in the settings
    menu.addItem (tr ("Override global link... (for all projects: move a knob on the controller)"), [&engine, target] {
        engine.learnGlobalLink (target);
    });

    if (target.kind != AutomationTargetKind::tempo)
        addControllerLinkItems (menu, engine, target);

    const auto& links = engine.getSong().midiLinks;
    const auto linked = std::find_if (links.begin(), links.end(), [&] (const MidiLink& l) { return l.target == target; });

    if (linked != links.end())
        menu.addItem (tr ("Unlink controller (CC {0})", { juce::String (linked->cc) }), [&engine, target] {
            engine.editSong ([target] (Song& s) {
                s.midiLinks.erase (std::remove_if (s.midiLinks.begin(), s.midiLinks.end(), [&] (const MidiLink& l) { return l.target == target; }),
                                   s.midiLinks.end());
            });
        });
    if (const auto global = engine.globalLinkOf (target))
        menu.addItem (tr ("Remove global link (CC {0})", { juce::String (global->cc) }), [&engine, target] { engine.removeGlobalLink (target); });
}

juce::String midiParamLabel (int param)
{
    if (midictl::isCc (param))
    {
        const auto cc = param - midictl::ccBase;
        const juce::String standard = midictl::ccName (cc);
        return tr ("MIDI CC #{0}", { juce::String (cc) }) + (standard.isEmpty() ? juce::String() : " (" + tr (standard) + ")");
    }
    if (midictl::isPressure (param))
        return tr ("MIDI Channel {0} Aftertouch", { juce::String (param - midictl::pressureBase + 1) });
    if (midictl::isPitchBend (param))
        return tr ("MIDI Channel {0} Pitch Bend", { juce::String (param - midictl::pitchBendBase + 1) });
    return {};
}

void addMidiParameterItems (juce::PopupMenu& menu, AudioEngine& engine, const AutomationTarget& base)
{
    const auto item = [&] (juce::PopupMenu& into, int param) {
        auto target = base;
        target.param = param;
        juce::PopupMenu sub;
        addAutomationItem (sub, engine, target);
        into.addSubMenu (midiParamLabel (param), sub);
    };

    juce::PopupMenu midi;
    for (int group = 0; group < 4; ++group) // CC 0-31, 32-63, 64-95, 96-127
    {
        juce::PopupMenu ccs;
        for (int cc = group * 32; cc < group * 32 + 32; ++cc)
            item (ccs, midictl::ccBase + cc);
        midi.addSubMenu (tr ("MIDI CC {0}-{1}", { juce::String (group * 32), juce::String (group * 32 + 31) }), ccs);
    }
    // the ones people reach for first, without digging
    juce::PopupMenu common;
    for (int cc : { 1, 7, 10, 11, 64, 66, 67, 74 })
        item (common, midictl::ccBase + cc);
    midi.addSubMenu (tr ("Common controllers"), common);

    juce::PopupMenu pressure, bend;
    for (int c = 0; c < 16; ++c)
    {
        item (pressure, midictl::pressureBase + c);
        item (bend, midictl::pitchBendBase + c);
    }
    midi.addSubMenu (tr ("Channel aftertouch"), pressure);
    midi.addSubMenu (tr ("Pitch bend"), bend);
    menu.addSubMenu (tr ("Automate MIDI controller (CC, aftertouch, pitch bend)"), midi);
}

void addPluginParameterItems (juce::PopupMenu& menu, AudioEngine& engine, juce::AudioPluginInstance& plugin,
                              const AutomationTarget& base)
{
    juce::PopupMenu params;
    const auto& list = plugin.getParameters();
    const auto count = juce::jmin (list.size(), midictl::ccBase);

    // a long list (VSL's pianos expose ~950) goes into pages of 64, so the menu fits on the screen
    constexpr int page = 64;
    for (int first = 0; first < count; first += page)
    {
        juce::PopupMenu pageMenu;
        auto& into = count > page ? pageMenu : params;
        for (int i = first; i < juce::jmin (count, first + page); ++i)
        {
            auto target = base;
            target.param = i;
            juce::PopupMenu param;
            addAutomationItem (param, engine, target);
            into.addSubMenu (juce::String (i + 1) + ". " + list[i]->getName (60), param);
        }
        if (count > page)
            params.addSubMenu (tr ("Parameters {0}-{1}", { juce::String (first + 1), juce::String (juce::jmin (count, first + page)) }), pageMenu);
    }

    if (list.isEmpty())
        params.addItem (tr ("(the plugin exposes no parameters)"), false, false, [] {});

    menu.addSubMenu (tr ("Automate plugin parameter"), params);
    addMidiParameterItems (menu, engine, base);
}
} // namespace lynx
