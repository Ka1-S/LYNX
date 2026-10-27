#include "ui/AudioEditorTools.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <thread>

#include <juce_audio_formats/juce_audio_formats.h>

#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx::audiotools
{
namespace ae = audioedit;

const char* toolTitle (Tool tool)
{
    switch (tool)
    {
        case Tool::amp: return TR_NOOP ("Amp");
        case Tool::equalize: return TR_NOOP ("Equalizer");
        case Tool::matchSpectrum: return TR_NOOP ("Match spectrum");
        case Tool::blur: return TR_NOOP ("Blur");
        case Tool::reverb: return TR_NOOP ("Convolution reverb");
        case Tool::denoise: return TR_NOOP ("Clean up (denoise)");
        case Tool::vocalDenoise: return TR_NOOP ("Vocal denoiser");
        case Tool::claw: return TR_NOOP ("Claw machine");
        case Tool::drumStretch: return TR_NOOP ("Drum(loop) stretcher");
        case Tool::loopTuner: return TR_NOOP ("Loop tuner");
        case Tool::scaleLevels: return TR_NOOP ("Level scaling");
        case Tool::decimate: return TR_NOOP ("Decimate points");
        case Tool::envFilter: return TR_NOOP ("Envelope filter");
        case Tool::smoothUp: return TR_NOOP ("Smooth up");
        case Tool::createSequence: return TR_NOOP ("Create sequence");
        case Tool::pasteSpectral: return TR_NOOP ("Paste mix (spectral)");
        case Tool::stretch: return TR_NOOP ("Time stretcher / pitch shifter");
    }
    return "";
}

// --- curve editor ----------------------------------------------------------------------------------------------
CurveEditor::CurveEditor() { setRepaintsOnMouseActivity (false); }

juce::Rectangle<float> CurveEditor::plotArea() const { return getLocalBounds().toFloat().reduced (6.0f, 6.0f).withTrimmedBottom (axis == Axis::frequency ? 12.0f : 0.0f); }

juce::Point<float> CurveEditor::toScreen (double x, double y) const
{
    const auto a = plotArea();
    return { a.getX() + (float) x * a.getWidth(), a.getBottom() - (float) y * a.getHeight() };
}

std::pair<double, double> CurveEditor::fromScreen (juce::Point<float> p) const
{
    const auto a = plotArea();
    return { juce::jlimit (0.0, 1.0, (double) ((p.x - a.getX()) / a.getWidth())), juce::jlimit (0.0, 1.0, (double) ((a.getBottom() - p.y) / a.getHeight())) };
}

int CurveEditor::hitPoint (juce::Point<float> p) const
{
    int best = -1;
    float bestD = 7.0f;
    for (int i = 0; i < (int) envelope.points.size(); ++i)
    {
        const auto d = toScreen (envelope.points[(std::size_t) i].x, envelope.points[(std::size_t) i].y).getDistanceFrom (p);
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void CurveEditor::paint (juce::Graphics& g)
{
    const auto a = plotArea();
    g.setColour (theme::colour::bg0);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::radius::md);

    // grid
    g.setColour (theme::colour::gridBeat);
    g.setFont (theme::font (theme::type::caption));
    if (axis == Axis::frequency)
    {
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            if (hz >= sampleRate * 0.5)
                continue;
            const auto x = a.getX() + (float) ae::curvePosition (hz, freqScale, sampleRate) * a.getWidth();
            g.setColour (theme::colour::gridBeat);
            g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            g.setColour (theme::colour::textDisabled);
            const auto label = hz >= 1000.0 ? juce::String (hz / 1000.0, 0) + "k" : juce::String ((int) hz);
            g.drawText (label, juce::Rectangle<float> (x - 16.0f, a.getBottom() + 1.0f, 32.0f, 11.0f), juce::Justification::centred);
        }
    }
    else
        for (int i = 1; i < 8; ++i)
            g.drawVerticalLine ((int) (a.getX() + a.getWidth() * (float) i / 8.0f), a.getY(), a.getBottom());
    for (int i = 1; i < 4; ++i)
    {
        g.setColour (i == 2 && centreLine ? theme::colour::borderStrong : theme::colour::gridBeat);
        g.drawHorizontalLine ((int) (a.getY() + a.getHeight() * (float) i / 4.0f), a.getX(), a.getRight());
    }

    // the background (a spectrum): a soft filled shape
    if (! background.empty())
    {
        juce::Path p;
        p.startNewSubPath (a.getBottomLeft());
        const auto n = background.size();
        for (std::size_t i = 0; i < n; ++i)
            p.lineTo (a.getX() + a.getWidth() * (float) i / (float) std::max<std::size_t> (1, n - 1), a.getBottom() - juce::jlimit (0.0f, 1.0f, background[i]) * a.getHeight());
        p.lineTo (a.getBottomRight());
        p.closeSubPath();
        g.setColour (theme::colour::audioClip.withAlpha (0.28f));
        g.fillPath (p);
    }

    // the curve
    if (! envelope.points.empty())
    {
        juce::Path curve, fill;
        const int steps = juce::jmax (2, (int) a.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const auto x = (double) i / steps;
            const auto pt = toScreen (x, envelope.at (x));
            if (i == 0)
            {
                curve.startNewSubPath (pt);
                fill.startNewSubPath (a.getX(), centreLine ? toScreen (0, 0.5).y : a.getBottom());
                fill.lineTo (pt);
            }
            else
            {
                curve.lineTo (pt);
                fill.lineTo (pt);
            }
        }
        fill.lineTo (a.getRight(), centreLine ? toScreen (0, 0.5).y : a.getBottom());
        fill.closeSubPath();
        g.setColour (theme::colour::accent.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (theme::colour::accent);
        g.strokePath (curve, juce::PathStrokeType (1.6f));
        for (int i = 1; i < (int) envelope.points.size(); ++i)
        {
            const auto& a0 = envelope.points[(std::size_t) i - 1];
            const auto& b0 = envelope.points[(std::size_t) i];
            if (toScreen (b0.x, b0.y).x - toScreen (a0.x, a0.y).x < 14.0f)
                continue;
            const auto mx = 0.5 * (a0.x + b0.x);
            const auto h = toScreen (mx, envelope.at (mx));
            g.setColour (i == bending || h.getDistanceFrom (hover) < 6.0f ? theme::colour::textPrimary : theme::colour::accent.withAlpha (0.8f));
            g.drawEllipse (h.x - 3.0f, h.y - 3.0f, 6.0f, 6.0f, 1.2f);
        }
        for (int i = 0; i < (int) envelope.points.size(); ++i)
        {
            const auto& p = envelope.points[(std::size_t) i];
            const auto pt = toScreen (p.x, p.y);
            const auto hot = i == dragging || pt.getDistanceFrom (hover) < 7.0f;
            g.setColour (hot ? theme::colour::textPrimary : theme::colour::accentHover);
            g.fillEllipse (pt.x - 3.5f, pt.y - 3.5f, 7.0f, 7.0f);
            g.setColour (theme::colour::bg0);
            g.drawEllipse (pt.x - 3.5f, pt.y - 3.5f, 7.0f, 7.0f, 1.0f);
        }
    }

    g.setColour (theme::colour::textSecondary);
    g.setFont (theme::font (theme::type::small));
    if (caption.isNotEmpty())
        g.drawText (caption, a.reduced (6.0f, 4.0f), juce::Justification::topLeft);
    if (readout.isNotEmpty())
        g.drawText (readout, a.reduced (6.0f, 4.0f), juce::Justification::topRight);
}

void CurveEditor::mouseMove (const juce::MouseEvent& e)
{
    hover = e.position;
    if (describe)
    {
        const auto [x, y] = fromScreen (e.position);
        readout = describe (x, y);
    }
    repaint();
}

int CurveEditor::hitHandle (juce::Point<float> p) const
{
    for (int i = 1; i < (int) envelope.points.size(); ++i)
    {
        const auto& a = envelope.points[(std::size_t) i - 1];
        const auto& b = envelope.points[(std::size_t) i];
        if (toScreen (b.x, b.y).x - toScreen (a.x, a.y).x < 14.0f)
            continue;
        const auto mx = 0.5 * (a.x + b.x);
        if (toScreen (mx, envelope.at (mx)).getDistanceFrom (p) < 6.0f)
            return i;
    }
    return -1;
}

void CurveEditor::mouseDown (const juce::MouseEvent& e)
{
    changed = false;
    const auto hit = hitPoint (e.position);
    // FL's point editing: right-click (or a click) on an empty spot adds a point and drags it, right-click or
    // Alt+click on a point deletes it, the hollow handle between two points bends the segment (right-click: straight)
    if (hit >= 0 && (e.mods.isPopupMenu() || e.mods.isAltDown()))
    {
        if (envelope.points.size() > 1)
        {
            envelope.points.erase (envelope.points.begin() + hit);
            changed = true;
            if (onChange)
                onChange();
        }
        repaint();
        return;
    }
    if (const auto h = hitHandle (e.position); h > 0 && hit < 0)
    {
        if (e.mods.isPopupMenu())
        {
            envelope.points[(std::size_t) h].tension = 0.0f;
            changed = true;
            if (onChange)
                onChange();
            repaint();
            return;
        }
        bending = h;
        bendStartY = e.position.y;
        bendStartTension = envelope.points[(std::size_t) h].tension;
        return;
    }
    if (hit >= 0)
        dragging = hit;
    else
    {
        const auto [x, y] = fromScreen (e.position);
        dragging = envelope.insert (x, y);
        changed = true;
        if (onChange)
            onChange();
    }
    repaint();
}

void CurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    hover = e.position;
    if (bending > 0 && bending < (int) envelope.points.size())
    {
        auto& p = envelope.points[(std::size_t) bending];
        const auto rising = p.y >= envelope.points[(std::size_t) bending - 1].y;
        const auto delta = (e.position.y - bendStartY) / 120.0f * (rising ? 1.0f : -1.0f);
        p.tension = juce::jlimit (-1.0f, 1.0f, bendStartTension + delta);
        changed = true;
        if (onChange)
            onChange();
        repaint();
        return;
    }
    if (dragging < 0 || dragging >= (int) envelope.points.size())
        return;
    auto [x, y] = fromScreen (e.position);
    // a point stays between its neighbours (the first and last keep their x when the curve spans the whole axis)
    const auto lo = dragging > 0 ? envelope.points[(std::size_t) dragging - 1].x : 0.0;
    const auto hi = dragging + 1 < (int) envelope.points.size() ? envelope.points[(std::size_t) dragging + 1].x : 1.0;
    x = juce::jlimit (lo, hi, x);
    if (e.mods.isShiftDown()) // fine vertical only
        x = envelope.points[(std::size_t) dragging].x;
    envelope.points[(std::size_t) dragging].x = x;
    envelope.points[(std::size_t) dragging].y = y;
    if (describe)
        readout = describe (x, y);
    changed = true;
    if (onChange)
        onChange();
    repaint();
}

void CurveEditor::mouseUp (const juce::MouseEvent&)
{
    dragging = bending = -1;
    if (changed && onEditEnd)
        onEditEnd();
    changed = false;
    repaint();
}

void CurveEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = hitPoint (e.position);
    if (hit >= 0)
    {
        envelope.points[(std::size_t) hit].tension = 0.0f;
        if (onChange)
            onChange();
        if (onEditEnd)
            onEditEnd();
        repaint();
    }
}

// --- the tool panel ------------------------------------------------------------------------------------------------
namespace
{
juce::String percent (double v) { return juce::String (juce::roundToInt (v)) + "%"; }

struct Control
{
    enum Kind
    {
        slider,
        knob,
        choice,
        toggle,
        header
    };
    Kind kind = slider;
    juce::String name;
    double minimum = 0.0, maximum = 1.0, interval = 0.0, initial = 0.0;
    std::function<juce::String (double)> format;
    juce::StringArray items;
    int row = -1; // controls sharing a row id sit side by side
};

class ToolPanel final : public juce::Component,
                        private juce::Timer
{
public:
    ToolPanel (Host& h, Tool t) : host (h), tool (t), hostComponent (&h.toolComponent())
    {
        title.setText (tr (toolTitle (tool)), juce::dontSendNotification);
        title.setFont (theme::font (theme::type::title, true));
        addAndMakeVisible (title);
        info.setFont (theme::font (theme::type::small));
        info.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        info.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (info);

        build();

        for (auto* b : { &resetButton, &previewButton, &acceptButton, &cancelButton })
            addAndMakeVisible (b);
        resetButton.setButtonText (tr ("Reset"));
        previewButton.setButtonText (tr ("Preview"));
        acceptButton.setButtonText (tr ("Accept"));
        cancelButton.setButtonText (tr ("Cancel"));
        acceptButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        previewButton.setTooltip (tr ("Play the result (Space)"));
        resetButton.onClick = [this] { reset(); };
        previewButton.onClick = [this] { run (true); };
        acceptButton.onClick = [this] { run (false); };
        cancelButton.onClick = [this] {
            if (busy)
            {
                progress->cancel = true;
                return;
            }
            close();
        };
        previewButton.setVisible (! isEnvelopeTool());
        addChildComponent (progressBar);

        setWantsKeyboardFocus (true);
        setSize (width, computeHeight());
        updateInfo();
        if (isEnvelopeTool())
            snapshotEnvelope();
    }

    ~ToolPanel() override
    {
        stopWorker();
        if (isEnvelopeTool() && ! accepted && originalEnvelope && hostComponent != nullptr)
            if (auto* env = host.toolEnvelope())
            {
                *env = *originalEnvelope; // Cancel / close puts the envelope back
                host.toolEnvelopeChanged (false, {});
            }
        if (hostComponent != nullptr)
            host.toolStopPreview();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey && previewButton.isVisible())
        {
            run (true);
            return true;
        }
        if (k == juce::KeyPress::returnKey)
        {
            run (false);
            return true;
        }
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::border);
        for (auto& s : sectionLines)
            g.fillRect (s);
    }

    void resized() override
    {
        sectionLines.clear();
        auto area = getLocalBounds().reduced (14, 10);
        title.setBounds (area.removeFromTop (22));
        info.setBounds (area.removeFromTop (34));
        area.removeFromTop (4);

        for (std::size_t i = 0; i < controls.size();)
        {
            const auto& c = controls[i];
            if (c.kind == Control::header)
            {
                auto r = area.removeFromTop (headerHeight).withTrimmedTop (6);
                labels[i]->setBounds (r);
                sectionLines.push_back (r.removeFromBottom (1));
                if (curveAfter.count ((int) i) != 0)
                    curveAfter[(int) i]->setBounds (area.removeFromTop (curveHeight).reduced (0, 4));
                ++i;
                continue;
            }
            // a row: this control and the following ones with the same row id
            auto j = i + 1;
            while (c.row >= 0 && j < controls.size() && controls[j].row == c.row && controls[j].kind != Control::header)
                ++j;
            const auto knobs = c.kind == Control::knob;
            auto row = area.removeFromTop (knobs ? knobHeight : rowHeight).reduced (0, 3);
            const auto count = (int) (j - i);
            const auto cellW = row.getWidth() / count;
            for (auto k = i; k < j; ++k)
            {
                auto cell = row.removeFromLeft (cellW).reduced (k > i ? 4 : 0, 0);
                if (controls[k].kind == Control::knob)
                {
                    labels[k]->setBounds (cell.removeFromTop (16));
                    editors[k]->setBounds (cell);
                }
                else if (controls[k].kind == Control::toggle)
                    editors[k]->setBounds (cell);
                else
                {
                    labels[k]->setBounds (cell.removeFromLeft (count > 1 ? 70 : 130));
                    editors[k]->setBounds (cell);
                }
            }
            i = j;
        }
        if (! extraButtons.isEmpty())
        {
            auto row = area.removeFromTop (rowHeight).reduced (0, 3);
            for (auto* b : extraButtons)
            {
                b->setBounds (row.removeFromLeft (juce::jmax (120, b->getBestWidthForHeight (24) + 16)));
                row.removeFromLeft (8);
            }
        }
        auto buttons = area.removeFromBottom (30);
        progressBar.setBounds (area.removeFromBottom (22).reduced (0, 2));
        cancelButton.setBounds (buttons.removeFromRight (84));
        buttons.removeFromRight (6);
        acceptButton.setBounds (buttons.removeFromRight (84));
        buttons.removeFromRight (6);
        previewButton.setBounds (buttons.removeFromRight (84));
        resetButton.setBounds (buttons.removeFromLeft (84));
    }

private:
    static constexpr int width = 500, rowHeight = 30, knobHeight = 80, headerHeight = 26, curveHeight = 170;

    bool isEnvelopeTool() const noexcept
    {
        return tool == Tool::scaleLevels || tool == Tool::decimate || tool == Tool::envFilter || tool == Tool::smoothUp || tool == Tool::createSequence;
    }

    int computeHeight() const
    {
        int h = 10 + 22 + 34 + 4;
        for (std::size_t i = 0; i < controls.size();)
        {
            if (controls[i].kind == Control::header)
            {
                h += headerHeight + (curveAfter.count ((int) i) != 0 ? curveHeight : 0);
                ++i;
                continue;
            }
            auto j = i + 1;
            while (controls[i].row >= 0 && j < controls.size() && controls[j].row == controls[i].row && controls[j].kind != Control::header)
                ++j;
            h += controls[i].kind == Control::knob ? knobHeight : rowHeight;
            i = j;
        }
        if (! extraButtons.isEmpty())
            h += rowHeight;
        return h + 22 + 30 + 10 + 8;
    }

    // --- building ---------------------------------------------------------------------------------------
    std::size_t add (Control c)
    {
        auto label = std::make_unique<juce::Label> (juce::String(), c.name);
        label->setFont (c.kind == Control::header ? theme::font (theme::type::small, true) : theme::font (theme::type::body));
        label->setColour (juce::Label::textColourId, c.kind == Control::header ? theme::colour::textSecondary : theme::colour::textPrimary);
        if (c.kind == Control::knob)
            label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*label);
        const auto index = controls.size();
        std::unique_ptr<juce::Component> editor;
        const auto changed = [this] { controlChanged(); };
        if (c.kind == Control::slider || c.kind == Control::knob)
        {
            auto s = std::make_unique<juce::Slider> (c.kind == Control::knob ? juce::Slider::RotaryHorizontalVerticalDrag : juce::Slider::LinearHorizontal,
                                                     c.kind == Control::knob ? juce::Slider::TextBoxBelow : juce::Slider::TextBoxRight);
            s->setTextBoxStyle (c.kind == Control::knob ? juce::Slider::TextBoxBelow : juce::Slider::TextBoxRight, false, 64, 18);
            s->setRange (c.minimum, c.maximum, c.interval);
            s->setValue (c.initial, juce::dontSendNotification);
            s->setDoubleClickReturnValue (true, c.initial);
            if (c.format)
                s->textFromValueFunction = c.format;
            s->updateText();
            s->onValueChange = changed;
            editor = std::move (s);
        }
        else if (c.kind == Control::choice)
        {
            auto box = std::make_unique<juce::ComboBox>();
            box->addItemList (c.items, 1);
            box->setSelectedItemIndex ((int) c.initial, juce::dontSendNotification);
            box->onChange = changed;
            editor = std::move (box);
        }
        else if (c.kind == Control::toggle)
        {
            auto t = std::make_unique<juce::ToggleButton> (c.name);
            t->setToggleState (c.initial > 0.5, juce::dontSendNotification);
            t->onClick = changed;
            label->setVisible (false);
            editor = std::move (t);
        }
        if (editor != nullptr)
            addAndMakeVisible (*editor);
        labels.push_back (std::move (label));
        editors.push_back (std::move (editor));
        controls.push_back (std::move (c));
        return index;
    }

    std::size_t header (const juce::String& name) { return add ({ Control::header, name }); }
    std::size_t slider (const juce::String& name, double mn, double mx, double step, double init, std::function<juce::String (double)> f = {}, int row = -1)
    {
        Control c { Control::slider, name, mn, mx, step, init, std::move (f) };
        c.row = row;
        return add (std::move (c));
    }
    std::size_t knob (const juce::String& name, double mn, double mx, double step, double init, std::function<juce::String (double)> f, int row)
    {
        Control c { Control::knob, name, mn, mx, step, init, std::move (f) };
        c.row = row;
        return add (std::move (c));
    }
    std::size_t choice (const juce::String& name, juce::StringArray items, int init, int row = -1)
    {
        Control c { Control::choice, name };
        c.items = std::move (items);
        c.initial = init;
        c.row = row;
        return add (std::move (c));
    }
    std::size_t toggle (const juce::String& name, bool init, int row = -1)
    {
        Control c { Control::toggle, name };
        c.initial = init ? 1.0 : 0.0;
        c.row = row;
        return add (std::move (c));
    }
    CurveEditor& curve (std::size_t afterHeader)
    {
        auto e = std::make_unique<CurveEditor>();
        e->sampleRate = host.toolDocument().audio.sampleRate;
        e->onChange = [this] { curveChanged(); };
        e->onEditEnd = [this] { controlChanged(); };
        addAndMakeVisible (*e);
        auto& ref = *e;
        curveAfter[(int) afterHeader] = e.get();
        curves.push_back (std::move (e));
        return ref;
    }
    juce::TextButton& extra (const juce::String& text, std::function<void()> fn)
    {
        auto* b = extraButtons.add (new juce::TextButton (text));
        b->onClick = std::move (fn);
        addAndMakeVisible (b);
        return *b;
    }

    double value (std::size_t i) const { return static_cast<juce::Slider*> (editors[i].get())->getValue(); }
    int chosen (std::size_t i) const { return static_cast<juce::ComboBox*> (editors[i].get())->getSelectedItemIndex(); }
    bool on (std::size_t i) const { return static_cast<juce::ToggleButton*> (editors[i].get())->getToggleState(); }
    void setValue (std::size_t i, double v) { static_cast<juce::Slider*> (editors[i].get())->setValue (v, juce::dontSendNotification); }

    void reset()
    {
        for (std::size_t i = 0; i < controls.size(); ++i)
        {
            const auto& c = controls[i];
            if (c.kind == Control::slider || c.kind == Control::knob)
                setValue (i, c.initial);
            else if (c.kind == Control::choice)
                static_cast<juce::ComboBox*> (editors[i].get())->setSelectedItemIndex ((int) c.initial, juce::dontSendNotification);
            else if (c.kind == Control::toggle)
                static_cast<juce::ToggleButton*> (editors[i].get())->setToggleState (c.initial > 0.5, juce::dontSendNotification);
        }
        for (std::size_t k = 0; k < curves.size() && k < initialCurves.size(); ++k)
            curves[k]->envelope = initialCurves[k];
        if (onReset)
            onReset();
        for (auto& c : curves)
            c->repaint();
        controlChanged();
    }

    void controlChanged()
    {
        updateInfo();
        if (isEnvelopeTool())
            applyEnvelopeTool (false);
    }

    void curveChanged()
    {
        if (isEnvelopeTool())
            applyEnvelopeTool (false);
    }

    void updateInfo()
    {
        if (describe)
            info.setText (describe(), juce::dontSendNotification);
    }

    // --- the tools ------------------------------------------------------------------------------------------
    using Process = std::function<void (ae::Document&, ae::Range, ae::Progress&)>;
    std::function<Process()> makeProcess; // reads the controls now, returns the work for the worker
    std::function<juce::String()> describe;
    std::function<void()> onReset;
    bool selectionOnly = false; // the result's selection: the processed part

    void build()
    {
        const auto& doc = host.toolDocument();
        const auto sr = doc.audio.sampleRate;
        const auto sel = host.toolSelection();
        const auto range = ae::orAll (doc, sel);
        const auto where = sel.empty() ? tr ("The whole audio") : tr ("The selection ({0} s)", { juce::String ((double) sel.length() / sr, 2) });

        switch (tool)
        {
            case Tool::amp:
            {
                header (tr ("Panning"));
                const auto pan = knob (tr ("Panning"), -100, 100, 1, 0, [] (double v) { return v == 0 ? juce::String ("C") : juce::String (std::abs ((int) v)) + (v < 0 ? "L" : "R"); }, 1);
                const auto law = choice (tr ("Law"), { tr ("Circular"), tr ("Triangular") }, 0, 2);
                header (tr ("Volume"));
                const auto left = knob (tr ("Left"), -200, 200, 1, 100, percent, 3);
                const auto vol = knob (tr ("Left / Right"), -200, 200, 1, 100, percent, 3);
                const auto right = knob (tr ("Right"), -200, 200, 1, 100, percent, 3);
                const auto sep = slider (tr ("Stereo separation"), -100, 100, 1, 0, [] (double v) { return v < 0 ? tr ("{0}% merged", { juce::String (-(int) v) }) : v > 0 ? tr ("{0}% wider", { juce::String ((int) v) }) : tr ("unchanged"); });
                describe = [where] { return where + "\n" + tr ("Volume per channel, panning (circular = constant power) and stereo separation."); };
                makeProcess = [=, this] {
                    ae::AmpSettings s;
                    s.pan = value (pan) / 100.0;
                    s.circularLaw = chosen (law) == 0;
                    s.volume = value (vol) / 100.0;
                    s.left = value (left) / 100.0;
                    s.right = value (right) / 100.0;
                    s.stereoSeparation = value (sep) / 100.0;
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress&) { ae::ampTool (d, r, s); });
                };
                break;
            }
            case Tool::equalize:
            case Tool::matchSpectrum:
            {
                const auto matching = tool == Tool::matchSpectrum;
                const auto h = header (tr ("Envelope"));
                auto& c = curve (h);
                c.axis = CurveEditor::Axis::frequency;
                c.centreLine = true;
                c.envelope = ae::Envelope::flat (0.5);
                c.background = spectrumBackground (doc.audio, range, 1.0);
                c.describe = [this, &c] (double x, double y) {
                    const auto hz = ae::curveFrequency (x, c.freqScale, c.sampleRate);
                    const auto g = ae::eqGain (y);
                    return juce::String (hz < 1000 ? juce::String ((int) hz) + " Hz" : juce::String (hz / 1000.0, 2) + " kHz") + "  " + (g <= 1e-6 ? juce::String ("-inf") : juce::String (20.0 * std::log10 (g), 1)) + " dB";
                };
                const auto scale = slider (tr ("Freq scale"), 0, 100, 1, 100, percent, 1);
                const auto smooth = toggle (tr ("Smooth"), true, 1);
                const auto mix = slider (tr ("Mix"), 0, 100, 1, 100, percent);
                auto setScale = [this, &c, scale, doc = &doc, range] {
                    c.freqScale = value (scale) / 100.0;
                    c.background = spectrumBackground (doc->audio, range, c.freqScale);
                    c.repaint();
                };
                static_cast<juce::Slider*> (editors[scale].get())->onValueChange = [this, setScale] { setScale(); controlChanged(); };
                extra (tr ("Filter unharmonics..."), [this, &c, range] {
                    juce::PopupMenu m;
                    const auto f0 = detectFundamental (host.toolDocument().audio, range);
                    const auto label = f0 > 0 ? tr ("(fundamental {0} Hz)", { juce::String (f0, 1) }) : tr ("(no clear pitch found)");
                    m.addSectionHeader (tr ("Filter unharmonics") + " " + label);
                    const char* names[] = { TR_NOOP ("Narrow"), TR_NOOP ("Normal"), TR_NOOP ("Wide") };
                    for (int w = 0; w < 3; ++w)
                        m.addItem (tr (names[w]), f0 > 0, false, [this, &c, w, f0] {
                            c.envelope = ae::unharmonicsCurve (f0, w, c.freqScale, c.sampleRate);
                            c.repaint();
                        });
                    m.addSeparator();
                    m.addItem (tr ("Flat (reset the curve)"), [&c] {
                        c.envelope = ae::Envelope::flat (0.5);
                        c.repaint();
                    });
                    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (extraButtons[0]));
                });
                if (matching)
                {
                    const auto& ref = host.toolClipboard();
                    if (! ref.empty())
                    {
                        c.envelope = matchCurve (doc.audio, range, ref, c.freqScale);
                        describe = [where] { return where + "\n" + tr ("The curve makes it sound like the copied audio (the clipboard); edit it, then Accept."); };
                    }
                    else
                        describe = [where] { return where + "\n" + tr ("Copy the reference audio first (Ctrl+C in any audio editor), then open Match spectrum."); };
                }
                else
                    describe = [where] { return where + "\n" + tr ("Draw the gain over frequency (centre line = unchanged). Right-click adds a point, right-click or Alt+click on a point deletes it, the hollow handles bend."); };
                makeProcess = [=, this, &c] {
                    ae::EqSettings s;
                    s.gain = c.envelope;
                    s.freqScale = value (scale) / 100.0;
                    s.smooth = on (smooth);
                    s.mix = value (mix) / 100.0;
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::equalize (d, r, s, &p); });
                };
                break;
            }
            case Tool::pasteSpectral:
            {
                // FL's spectral paste mix: the clipboard and the audio under it cross-faded per frequency
                const auto h = header (tr ("Spectral mask"));
                auto& c = curve (h);
                c.axis = CurveEditor::Axis::frequency;
                c.envelope = ae::Envelope::flat (1.0); // FL's default mask: all clipboard
                const auto& clip = host.toolClipboard();
                c.background = spectrumBackground (clip, { 0, clip.frames() }, 1.0);
                c.describe = [&c] (double x, double y) {
                    const auto hz = ae::curveFrequency (x, c.freqScale, c.sampleRate);
                    return juce::String (hz < 1000 ? juce::String ((int) hz) + " Hz" : juce::String (hz / 1000.0, 2) + " kHz") + "  " + tr ("clipboard {0}%", { juce::String (juce::roundToInt (y * 100.0)) });
                };
                const auto scale = slider (tr ("Freq scale"), 0, 100, 1, 100, percent, 1);
                static_cast<juce::Slider*> (editors[scale].get())->onValueChange = [this, &c, scale] {
                    c.freqScale = value (scale) / 100.0;
                    c.repaint();
                    controlChanged();
                };
                describe = [where] { return where + "\n" + tr ("Per frequency: top = the clipboard, bottom = the original audio, between = a mix of both. The clipboard goes at the selection's start."); };
                selectionOnly = true;
                makeProcess = [=, this, &c] {
                    const auto mask = c.envelope;
                    const auto fs = value (scale) / 100.0;
                    const auto clipCopy = host.toolClipboard();
                    return Process ([mask, fs, clipCopy] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::pasteMixSpectral (d, r.start, clipCopy, mask, fs, &p); });
                };
                break;
            }
            case Tool::blur:
            {
                header (tr ("Options"));
                const auto amount = slider (tr ("Amount"), 0, 100, 0.1, 50, percent);
                const auto offset = slider (tr ("Offset"), -100, 100, 0.1, 0, percent);
                const auto outside = toggle (tr ("Use data outside selection"), true);
                const auto mix = slider (tr ("Mix"), 0, 100, 1, 100, percent);
                const auto h = header (tr ("Impulse"));
                auto& c = curve (h);
                // FL's default map: a bell centred on the sound
                c.envelope.points = { { 0.0, 0.0, 0.0f }, { 0.5, 1.0, -0.6f }, { 1.0, 0.0, 0.6f } };
                c.caption = tr ("level over the blur's length (the middle = the sound itself)");
                describe = [this, amount, where] { return where + "\n" + tr ("Smears the sound over {0} s with the impulse's shape.", { juce::String (0.02 + 1.98 * value (amount) / 100.0, 2) }); };
                makeProcess = [=, this, &c] {
                    ae::BlurSettings s;
                    s.amount = value (amount) / 100.0;
                    s.offset = value (offset) / 100.0;
                    s.useOutside = on (outside);
                    s.mix = value (mix) / 100.0;
                    s.impulse = c.envelope;
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::blur (d, r, s, &p); });
                };
                break;
            }
            case Tool::reverb:
            {
                header (tr ("Options"));
                const auto offset = slider (tr ("Offset"), -1000, 1000, 1, 0, [] (double v) { return juce::String ((int) v) + " ms"; });
                const auto tail = toggle (tr ("Add tail"), true, 1);
                const auto outside = toggle (tr ("Use data outside selection"), true, 1);
                const auto dry = slider (tr ("Dry"), 0, 100, 1, 100, percent, 2);
                const auto wet = slider (tr ("Wet"), 0, 100, 1, 25, percent, 2);
                const auto h = header (tr ("Impulse"));
                auto& c = curve (h);
                c.envelope.points = { { 0.0, 1.0, 0.0f }, { 0.08, 0.7, 0.0f }, { 1.0, 0.0, 0.7f } };
                c.caption = tr ("drawn impulse: level over its length");
                const auto length = slider (tr ("Length"), 0.1, 10.0, 0.01, 2.0, [] (double v) { return juce::String (v, 2) + " s"; });
                extra (tr ("Load impulse..."), [this, &c] { loadImpulse (c); });
                extra (tr ("Use the drawn impulse"), [this, &c] {
                    impulse = {};
                    impulseName.clear();
                    c.caption = tr ("drawn impulse: level over its length");
                    c.repaint();
                    updateInfo();
                });
                describe = [this, where] {
                    return where + "\n" + (impulse.empty() ? tr ("The impulse is made from the drawn curve (noise shaped by it).") : tr ("Impulse: {0} ({1} s)", { impulseName, juce::String (impulse.seconds(), 2) }));
                };
                makeProcess = [=, this, &c] {
                    ae::ReverbSettings s;
                    s.impulse = impulse;
                    s.shape = c.envelope;
                    s.length = value (length);
                    s.offset = value (offset) / 1000.0;
                    s.addTail = on (tail);
                    s.useOutside = on (outside);
                    s.dry = value (dry) / 100.0;
                    s.wet = value (wet) / 100.0;
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::convolutionReverb (d, r, s, &p); });
                };
                break;
            }
            case Tool::denoise:
            {
                const auto h = header (tr ("Denoiser"));
                auto& c = curve (h);
                c.axis = CurveEditor::Axis::frequency;
                c.centreLine = true;
                c.freqScale = 0.0;
                c.envelope = ae::Envelope::flat (0.5);
                c.caption = tr ("threshold over frequency");
                refreshProfileBackground (c);
                const auto use = toggle (tr ("Enable denoiser"), true, 1);
                const auto noiseOnly = toggle (tr ("Output noise only"), false, 1);
                const auto scale = slider (tr ("Freq scale"), 0, 100, 1, 0, percent);
                static_cast<juce::Slider*> (editors[scale].get())->onValueChange = [this, &c, scale] {
                    c.freqScale = value (scale) / 100.0;
                    refreshProfileBackground (c);
                    controlChanged();
                };
                const auto threshold = slider (tr ("Threshold"), 0, 100, 0.1, 50, percent, 2);
                const auto amount = slider (tr ("Amount"), 0, 100, 0.1, 50, percent, 2);
                header (tr ("Declipper"));
                const auto clip = toggle (tr ("Enable declipper"), false, 3);
                const auto clipThr = slider (tr ("Threshold"), 0, 100, 0.1, 90, percent, 3);
                const auto clipModel = choice (tr ("Model"), { tr ("General"), tr ("Vocal") }, 0, 3);
                const auto clipSafe = toggle (tr ("Safe mode"), false, 3);
                header (tr ("Declicker"));
                const auto click = toggle (tr ("Enable declicker"), false, 4);
                const auto clickSens = slider (tr ("Sensitivity"), 0, 100, 0.1, 25, percent, 4);
                extra (tr ("Acquire noise profile"), [this, &c] {
                    const auto sel = host.toolSelection();
                    if (sel.empty())
                    {
                        info.setText (tr ("Select a part with only noise first, then acquire the profile."), juce::dontSendNotification);
                        return;
                    }
                    host.toolNoiseProfile() = ae::acquireNoiseProfile (host.toolDocument().audio, sel);
                    refreshProfileBackground (c);
                    updateInfo();
                });
                describe = [this, where] {
                    return where + "\n" + (host.toolNoiseProfile().valid() ? tr ("Noise profile: acquired (its spectrum is drawn behind the curve).")
                                                                         : tr ("No noise profile: the noise floor is estimated from the quietest moments. Select noise only and press Acquire for better results."));
                };
                makeProcess = [=, this, &c] {
                    ae::DenoiseSettings s;
                    s.denoise = on (use);
                    s.noiseOnly = on (noiseOnly);
                    s.freqScale = value (scale) / 100.0;
                    s.threshold = value (threshold) / 100.0;
                    s.amount = value (amount) / 100.0;
                    s.thresholdCurve = c.envelope;
                    s.declip = on (clip);
                    s.clipThreshold = value (clipThr) / 100.0;
                    s.vocalModel = chosen (clipModel) == 1;
                    s.safeMode = on (clipSafe);
                    s.declick = on (click);
                    s.clickSensitivity = value (clickSens) / 100.0;
                    const auto profile = host.toolNoiseProfile();
                    return Process ([s, profile] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::denoise (d, r, profile, s, &p); });
                };
                break;
            }
            case Tool::vocalDenoise:
            {
                header (tr ("Denoiser"));
                const auto mix = knob (tr ("Mix"), -100, 100, 1, 100, [] (double v) {
                    return v > 0 ? tr ("{0}% vocal", { juce::String ((int) v) }) : v < 0 ? tr ("{0}% noise", { juce::String (-(int) v) }) : tr ("unchanged");
                }, 1);
                describe = [where] { return where + "\n" + tr ("Turn right to keep the voice and remove the background noise, left to keep only the noise."); };
                makeProcess = [=, this] {
                    const auto m = value (mix) / 100.0;
                    return Process ([m] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::vocalDenoise (d, r, m, &p); });
                };
                break;
            }
            case Tool::claw:
            {
                header (tr ("Block extraction"));
                static const double periods[] = { 0.25, 0.5, 1, 2, 4, 8, 16 };
                const auto period = choice (tr ("Period"), { tr ("1/4 beat"), tr ("1/2 beat"), tr ("Beat"), tr ("2 beats"), tr ("4 beats"), tr ("8 beats"), tr ("16 beats") }, 2);
                struct Part
                {
                    int n, of;
                };
                static const Part parts[] = { { 1, 4 }, { 2, 4 }, { 3, 4 }, { 4, 4 }, { 1, 3 }, { 2, 3 }, { 3, 3 }, { 1, 2 }, { 2, 2 } };
                juce::StringArray partNames;
                for (const auto& p : parts)
                    partNames.add (juce::String (p.n) + "/" + juce::String (p.of));
                const auto trash = choice (tr ("Trash every"), partNames, 3);
                const auto snap = toggle (tr ("Snap to markers"), true);
                header (tr ("Compensation stretching"));
                const auto amount = slider (tr ("Amount"), 0, 100, 1, 0, percent);
                const auto method = choice (tr ("Method"), { tr ("Resample"), tr ("Stretch (keep the pitch)") }, 1);
                describe = [this, where, period, trash] {
                    const auto p = parts[(std::size_t) juce::jlimit (0, 8, chosen (trash))];
                    return where + "\n" + tr ("Every {0}: the part {1} of {2} is removed ({3} BPM).", { tr (periods[(std::size_t) chosen (period)] == 1 ? "beat" : "period"), juce::String (p.n), juce::String (p.of), juce::String (host.toolBpm(), 1) });
                };
                makeProcess = [=, this] {
                    ae::ClawSettings s;
                    s.periodBeats = periods[(std::size_t) juce::jlimit (0, 6, chosen (period))];
                    const auto p = parts[(std::size_t) juce::jlimit (0, 8, chosen (trash))];
                    s.trashPart = p.n;
                    s.trashOf = p.of;
                    s.snapToMarkers = on (snap);
                    s.compensation = value (amount) / 100.0;
                    s.resample = chosen (method) == 0;
                    const auto bpm = host.toolBpm();
                    return Process ([s, bpm] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::clawMachine (d, r, s, bpm, &p); });
                };
                break;
            }
            case Tool::drumStretch:
            {
                header (tr ("Amount"));
                const auto coarse = slider (tr ("Pitch coarse"), -12, 12, 1, 0, [] (double v) { return juce::String ((int) v) + " st"; }, 1);
                const auto fine = slider (tr ("fine"), -100, 100, 1, 0, [] (double v) { return juce::String ((int) v) + " ct"; }, 1);
                const auto bpm = host.toolBpm();
                const auto time = slider (tr ("Time mul"), 25, 400, 0.1, 100, [] (double v) { return juce::String (v, 1) + "%"; });
                const auto tempo = slider (tr ("Target tempo"), 20, 400, 0.01, bpm, [] (double v) { return juce::String (v, 2) + " BPM"; });
                auto* timeSlider = static_cast<juce::Slider*> (editors[time].get());
                auto* tempoSlider = static_cast<juce::Slider*> (editors[tempo].get());
                // the target tempo and the time multiplier are the same setting seen two ways (the audio at the project tempo)
                timeSlider->onValueChange = [this, tempoSlider, timeSlider, bpm] {
                    tempoSlider->setValue (bpm * 100.0 / timeSlider->getValue(), juce::dontSendNotification);
                    controlChanged();
                };
                tempoSlider->onValueChange = [this, tempoSlider, timeSlider, bpm] {
                    timeSlider->setValue (bpm * 100.0 / tempoSlider->getValue(), juce::dontSendNotification);
                    controlChanged();
                };
                const auto insertTails = toggle (tr ("Insert tails"), true, 2);
                const auto autoSlice = toggle (tr ("Auto-slice"), false, 2);
                header (tr ("Filling"));
                const auto fill = choice (tr ("Method"), { tr ("None"), tr ("Echo grains"), tr ("Stretch"), tr ("Pogo") }, 1);
                const auto blurIt = toggle (tr ("Blur"), true, 3);
                const auto random = toggle (tr ("Random presence"), false, 3);
                const auto loop = toggle (tr ("Loop"), false, 3);
                header (tr ("Tail envelope"));
                const auto tailMode = choice (tr ("Method"), { tr ("None"), tr ("Smooth fade"), tr ("Append") }, 1);
                const auto transient = knob (tr ("Transient"), 0, 100, 1, 70, percent, 4);
                const auto slope = knob (tr ("Tail slope"), 0, 200, 1, 100, percent, 4);
                const auto smoothing = knob (tr ("smoothing"), 5, 200, 1, 25, percent, 4);
                describe = [this, where, time, range, sr] {
                    return where + "\n" + tr ("Slices at the markers (or the attacks) move to the new tempo, each keeping its attack. New length: {0} s.",
                                              { juce::String ((double) range.length() / sr * value (time) / 100.0, 3) });
                };
                makeProcess = [=, this] {
                    ae::DrumStretchSettings s;
                    s.ratio = value (time) / 100.0;
                    s.pitchSemitones = value (coarse) + value (fine) / 100.0;
                    s.insertTails = on (insertTails);
                    s.autoSlice = on (autoSlice);
                    s.fill = (ae::DrumStretchSettings::Fill) juce::jlimit (0, 3, chosen (fill));
                    s.blur = on (blurIt);
                    s.randomPresence = on (random);
                    s.loop = on (loop);
                    s.tail = (ae::DrumStretchSettings::Tail) juce::jlimit (0, 2, chosen (tailMode));
                    s.transient = value (transient) / 100.0;
                    s.tailSlope = value (slope) / 100.0;
                    s.tailSmoothing = value (smoothing) / 100.0;
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::drumloopStretch (d, r, s, &p); });
                };
                break;
            }
            case Tool::stretch:
            {
                // FL's Time stretcher / pitch shifter form: pitch, time, method, insert, formant preservation
                header (tr ("Pitch"));
                const auto coarse = slider (tr ("Coarse"), -24, 24, 1, 0, [] (double v) { return juce::String ((int) v) + " st"; }, 1);
                const auto fine = slider (tr ("fine"), -100, 100, 1, 0, [] (double v) { return juce::String ((int) v) + " ct"; }, 1);
                header (tr ("Time"));
                const auto bpm = host.toolBpm();
                const auto time = slider (tr ("Time mul"), 25, 400, 0.1, 100, [] (double v) { return juce::String (v, 1) + "%"; });
                const auto seconds = slider (tr ("Length"), 0.01, std::max (0.02, (double) range.length() / sr * 4.0), 0.001, (double) range.length() / sr,
                                             [] (double v) { return juce::String (v, 3) + " s"; });
                const auto tempo = slider (tr ("Target tempo"), 20, 400, 0.01, bpm, [] (double v) { return juce::String (v, 2) + " BPM"; });
                auto* timeSlider = static_cast<juce::Slider*> (editors[time].get());
                auto* secondsSlider = static_cast<juce::Slider*> (editors[seconds].get());
                auto* tempoSlider = static_cast<juce::Slider*> (editors[tempo].get());
                const auto baseSeconds = std::max (1.0e-6, (double) range.length() / sr);
                // the multiplier, the new length and the target tempo are one setting seen three ways
                auto sync = [=] (double mul) {
                    timeSlider->setValue (mul, juce::dontSendNotification);
                    secondsSlider->setValue (baseSeconds * mul / 100.0, juce::dontSendNotification);
                    tempoSlider->setValue (bpm * 100.0 / mul, juce::dontSendNotification);
                };
                timeSlider->onValueChange = [this, sync, timeSlider] { sync (timeSlider->getValue()); controlChanged(); };
                secondsSlider->onValueChange = [this, sync, secondsSlider, baseSeconds] { sync (juce::jlimit (25.0, 400.0, secondsSlider->getValue() / baseSeconds * 100.0)); controlChanged(); };
                tempoSlider->onValueChange = [this, sync, tempoSlider, bpm] { sync (juce::jlimit (25.0, 400.0, bpm * 100.0 / tempoSlider->getValue())); controlChanged(); };
                const auto method = choice (tr ("Method"), { tr ("Stretch (keep the pitch)"), tr ("Resample (pitch follows the speed)") }, 0, 2);
                const auto insertIt = toggle (tr ("Insert"), true, 2);
                header (tr ("Formant preservation"));
                const auto formants = toggle (tr ("Preserve formants"), false, 3);
                const auto fCoarse = slider (tr ("Formant shift"), -24, 24, 1, 0, [] (double v) { return juce::String ((int) v) + " st"; }, 4);
                const auto fFine = slider (tr ("fine"), -100, 100, 1, 0, [] (double v) { return juce::String ((int) v) + " ct"; }, 4);
                const auto order = slider (tr ("Order"), 8, 512, 1, 128, [] (double v) { return juce::String ((int) v); });
                extra (tr ("Copy from pitch"), [this, coarse, fine, fCoarse, fFine] {
                    // the formants move with the pitch (as if not preserved, but with the other options)
                    static_cast<juce::Slider*> (editors[fCoarse].get())->setValue (value (coarse), juce::dontSendNotification);
                    static_cast<juce::Slider*> (editors[fFine].get())->setValue (value (fine), juce::dontSendNotification);
                    controlChanged();
                });
                describe = [this, where, time, range, sr] {
                    return where + "\n" + tr ("New length: {0} s. With formant preservation a voice keeps its character when the pitch moves.",
                                              { juce::String ((double) range.length() / sr * value (time) / 100.0, 3) });
                };
                makeProcess = [=, this] {
                    ae::StretchSettings s;
                    s.semitones = value (coarse) + value (fine) / 100.0;
                    s.lengthRatio = value (time) / 100.0;
                    s.resample = chosen (method) == 1;
                    s.insert = on (insertIt);
                    s.preserveFormants = on (formants);
                    s.formantSemitones = value (fCoarse) + value (fFine) / 100.0;
                    s.formantOrder = (int) value (order);
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::stretchTool (d, r, s, &p); });
                };
                break;
            }
            case Tool::loopTuner:
            {
                header (tr ("Crossfade"));
                const auto length = slider (tr ("Length"), 0, 100, 0.1, 50, percent, 1);
                const auto tension = slider (tr ("tension"), 0, 100, 0.1, 25, percent, 1);
                const auto snap = slider (tr ("Period snap"), 1, 7, 1, 5, [] (double v) { return v <= 1 ? tr ("off") : juce::String ((int) v); });
                header (tr ("Blur"));
                const auto order = slider (tr ("Order"), 1, 25, 1, 1, {}, 2);
                const auto scale = slider (tr ("scale"), -100, 100, 1, -25, percent, 2);
                const auto amp = slider (tr ("amp"), 50, 150, 1, 100, percent);
                describe = [this, where] {
                    const auto& d = host.toolDocument();
                    return (d.hasLoop() ? tr ("The loop ({0} s)", { juce::String ((double) (d.loopEnd - d.loopStart) / d.audio.sampleRate, 3) }) : where)
                           + "\n" + tr ("Makes the loop seamless: the loop points snap to the pitch period and the end crossfades into the start.");
                };
                makeProcess = [=, this] {
                    ae::LoopTuneSettings s;
                    s.crossfade = value (length) / 100.0;
                    s.tension = value (tension) / 100.0;
                    s.periodSnap = (int) value (snap);
                    s.blurOrder = (int) value (order);
                    s.blurScale = value (scale) / 100.0;
                    s.blurGain = value (amp) / 100.0;
                    return Process ([s] (ae::Document& d, ae::Range r, ae::Progress& p) { ae::tuneLoop (d, r, s, &p); });
                };
                previewWholeLoop = true;
                break;
            }
            case Tool::scaleLevels:
            {
                header (tr ("Levels"));
                centreI = knob (tr ("Center"), 0, 100, 0.1, 0, percent, 1);
                mulI = knob (tr ("Multiply"), -200, 200, 0.1, 100, percent, 1);
                ofsI = knob (tr ("Offset"), -100, 100, 0.1, 0, percent, 1);
                tensionI = knob (tr ("Tension"), -100, 100, 0.1, 0, percent, 1);
                describe = [] { return tr ("Scales the envelope's levels around the center, then adds the offset; tension bends them."); };
                break;
            }
            case Tool::decimate:
            {
                header (tr ("Decimate"));
                thresholdI = slider (tr ("Threshold"), 0, 100, 0.1, 50, percent);
                describe = [this] { return tr ("Removes the points a straight line explains ({0} points now).", { juce::String (envelopePointCount()) }); };
                break;
            }
            case Tool::envFilter:
            {
                header (tr ("Options"));
                widthI = slider (tr ("Width"), 0, 100, 0.1, 50, percent);
                ofsI = slider (tr ("Offset"), -100, 100, 0.1, 0, percent);
                thresholdI = slider (tr ("Decimation"), 0, 100, 0.1, 50, percent);
                const auto h = header (tr ("Impulse"));
                auto& c = curve (h);
                c.envelope.points = { { 0.0, 0.0, 0.0f }, { 0.5, 1.0, 0.0f }, { 1.0, 0.0, 0.0f } }; // FL's default: a triangle
                describe = [] { return tr ("Filters the envelope with the impulse's shape (a smoother envelope)."); };
                break;
            }
            case Tool::smoothUp:
            {
                header (tr ("Smoothing"));
                attI = knob (tr ("Attack"), 0, 100, 0.1, 50, percent, 1);
                relI = knob (tr ("Release"), 0, 100, 0.1, 50, percent, 1);
                attSlopeI = knob (tr ("Attack slope"), 0, 100, 0.1, 50, percent, 2);
                relSlopeI = knob (tr ("Release slope"), 0, 100, 0.1, 50, percent, 2);
                ofsI = slider (tr ("Offset"), -100, 100, 0.1, 0, percent);
                thresholdI = slider (tr ("Decimation"), 0, 100, 0.1, 50, percent);
                describe = [] { return tr ("Limits how fast the envelope may rise (attack) and fall (release)."); };
                break;
            }
            case Tool::createSequence:
            {
                header (tr ("Pattern"));
                const auto h = header (tr ("Step levels (each point = a step; drag to 0 for a break)"));
                auto& c = curve (h);
                c.envelope.points.clear();
                for (int i = 0; i < 8; ++i)
                    c.envelope.points.push_back ({ (i + 0.5) / 8.0, i % 4 == 0 ? 1.0 : 0.6, 0.0f });
                c.caption = tr ("8 steps");
                seqCurve = &c;
                modeI = choice (tr ("Mode"), { tr ("Normal"), tr ("Ping-pong") }, 0);
                timeMulI = knob (tr ("Time mul"), 1, 10, 1, 2, [] (double v) { return juce::String ((int) v) + "/" + tr ("beat"); }, 1);
                swingI = knob (tr ("Swing"), 0, 100, 1, 0, percent, 1);
                gateI = knob (tr ("Gate"), 10, 100, 1, 55, percent, 1);
                attI = knob (tr ("Attack"), 0, 90, 1, 0, percent, 2);
                relI = knob (tr ("Decay"), 0, 90, 1, 50, percent, 2);
                susI = knob (tr ("Sustain"), 0, 100, 1, 80, percent, 2);
                extra (tr ("Randomize"), [this] {
                    juce::Random rnd;
                    for (auto& p : seqCurve->envelope.points)
                        p.y = rnd.nextFloat() < 0.2f ? 0.0 : 0.3 + 0.7 * rnd.nextDouble();
                    seqCurve->repaint();
                    controlChanged();
                });
                describe = [this] { return tr ("Writes a gated step pattern into the envelope ({0} beats).", { juce::String (envelopeBeats(), 2) }); };
                break;
            }
        }
        initialCurves.clear();
        for (auto& c : curves)
            initialCurves.push_back (c->envelope);
    }

    // --- envelope tools (live on the envelope drawn over the audio) -----------------------------------------
    std::size_t centreI = 0, mulI = 0, ofsI = 0, tensionI = 0, thresholdI = 0, widthI = 0, attI = 0, relI = 0, attSlopeI = 0, relSlopeI = 0;
    std::size_t modeI = 0, timeMulI = 0, swingI = 0, gateI = 0, susI = 0;
    CurveEditor* seqCurve = nullptr;
    std::optional<ae::Envelope> originalEnvelope;
    bool accepted = false;

    int envelopePointCount() { return host.toolEnvelope() != nullptr ? (int) host.toolEnvelope()->points.size() : 0; }
    double envelopeBeats() const
    {
        const auto& d = host.toolDocument();
        return d.audio.seconds() * host.toolBpm() / 60.0;
    }

    void snapshotEnvelope()
    {
        if (auto* env = host.toolEnvelope())
            originalEnvelope = *env;
        applyEnvelopeTool (false);
    }

    void applyEnvelopeTool (bool commit)
    {
        auto* env = host.toolEnvelope();
        if (env == nullptr || ! originalEnvelope)
            return;
        auto e = *originalEnvelope;
        switch (tool)
        {
            case Tool::scaleLevels: ae::scaleLevels (e, value (centreI) / 100.0, value (mulI) / 100.0, value (ofsI) / 100.0, value (tensionI) / 100.0); break;
            case Tool::decimate: ae::decimatePoints (e, value (thresholdI) / 100.0); break;
            case Tool::envFilter: ae::filterEnvelope (e, curves.empty() ? ae::Envelope {} : curves[0]->envelope, value (widthI) / 100.0, value (ofsI) / 100.0, value (thresholdI) / 100.0); break;
            case Tool::smoothUp:
                ae::smoothUp (e, value (attI) / 100.0, value (relI) / 100.0, value (attSlopeI) / 100.0, value (relSlopeI) / 100.0, value (ofsI) / 100.0, value (thresholdI) / 100.0);
                break;
            case Tool::createSequence:
            {
                ae::SequenceSettings s;
                for (const auto& p : seqCurve->envelope.points)
                {
                    s.levels.push_back ((float) p.y);
                    s.breaks.push_back (p.y < 0.02);
                }
                s.pingPong = chosen (modeI) == 1;
                s.stepsPerUnit = (int) value (timeMulI);
                s.swing = value (swingI) / 100.0;
                s.gate = value (gateI) / 100.0;
                s.attack = value (attI) / 100.0;
                s.decay = value (relI) / 100.0;
                s.sustain = value (susI) / 100.0;
                e = ae::createSequence (s, envelopeBeats());
                // the sequence is a volume shape around unity: level 1 = the envelope's 0 dB line
                for (auto& p : e.points)
                    p.y *= 0.5;
                break;
            }
            default: break;
        }
        *env = e;
        host.toolEnvelopeChanged (commit, tr (toolTitle (tool)));
        updateInfo();
    }

    // --- running the audio tools --------------------------------------------------------------------------
    struct Result
    {
        ae::Document doc;
        ae::Range selection;
        bool preview = false;
        bool cancelled = false;
    };

    std::unique_ptr<ae::Progress> progress = std::make_unique<ae::Progress>();
    std::thread worker;
    std::atomic<bool> finished { false };
    std::shared_ptr<Result> result;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    bool busy = false;
    bool previewWholeLoop = false;

    void run (bool preview)
    {
        if (busy)
            return;
        if (isEnvelopeTool())
        {
            applyEnvelopeTool (true);
            accepted = true;
            close();
            return;
        }
        if (! makeProcess)
            return;
        auto process = makeProcess();
        const auto& doc = host.toolDocument();
        if (doc.audio.empty())
            return;
        host.toolStopPreview();
        auto job = std::make_shared<Result>();
        job->preview = preview;
        const auto sel = host.toolSelection();
        if (preview)
        {
            // the preview works on the selection (at most 20 s of it) with a little context around it, then plays it
            auto r = ae::orAll (doc, sel);
            if (previewWholeLoop && doc.hasLoop() && sel.empty())
                r = { doc.loopStart, doc.loopEnd };
            const auto maxFrames = (std::int64_t) (20.0 * doc.audio.sampleRate);
            r.end = std::min (r.end, r.start + maxFrames);
            const auto context = std::min<std::int64_t> (r.start, (std::int64_t) (2.0 * doc.audio.sampleRate));
            ae::Document part;
            part.audio = ae::copy (doc.audio, { r.start - context, std::min (doc.audio.frames(), r.end + (std::int64_t) doc.audio.sampleRate) });
            for (const auto& m : doc.markers)
                if (m.start >= r.start - context && m.start < r.end)
                    part.markers.push_back ({ m.start - (r.start - context), m.end - (r.start - context), m.name });
            if (doc.hasLoop() && doc.loopStart >= r.start - context)
            {
                part.loopStart = doc.loopStart - (r.start - context);
                part.loopEnd = doc.loopEnd - (r.start - context);
            }
            job->doc = std::move (part);
            job->selection = { context, context + r.length() };
        }
        else
        {
            job->doc = doc;
            job->selection = sel;
        }
        result = job;
        progress = std::make_unique<ae::Progress>();
        finished = false;
        busy = true;
        progressValue = 0.0;
        progressBar.setVisible (true);
        setButtonsEnabled (false);
        auto* p = progress.get();
        worker = std::thread ([job, process, p, this] {
            const auto before = job->doc.audio.frames();
            process (job->doc, job->selection, *p);
            job->cancelled = p->cancelled();
            // a length-changing tool keeps the edited part selected
            if (! job->selection.empty())
                job->selection.end = std::max (job->selection.start, job->selection.end + (job->doc.audio.frames() - before));
            finished = true;
        });
        startTimerHz (20);
    }

    void timerCallback() override
    {
        progressValue = (double) progress->done.load();
        if (! finished)
            return;
        stopTimer();
        stopWorker();
        busy = false;
        progressBar.setVisible (false);
        setButtonsEnabled (true);
        auto job = result;
        result.reset();
        if (job == nullptr || job->cancelled || hostComponent == nullptr)
            return;
        if (job->preview)
        {
            // play the processed part (with what a tail tool added after it)
            auto r = job->selection;
            auto end = std::min (job->doc.audio.frames(), r.empty() ? job->doc.audio.frames() : r.end + (tool == Tool::reverb ? (std::int64_t) (job->doc.audio.sampleRate * 12.0) : 0));
            if (previewWholeLoop && job->doc.hasLoop())
            {
                // the loop seam: the last second of the loop, then its first second, twice
                const auto& d = job->doc;
                const auto s = (std::int64_t) d.audio.sampleRate;
                ae::Audio seam = ae::copy (d.audio, { std::max (d.loopStart, d.loopEnd - s), d.loopEnd });
                const auto head = ae::copy (d.audio, { d.loopStart, std::min (d.loopEnd, d.loopStart + s) });
                for (int k = 0; k < 2; ++k)
                    for (std::size_t c = 0; c < seam.channels.size(); ++c)
                        seam.channels[c].insert (seam.channels[c].end(), head.channels[c].begin(), head.channels[c].end());
                host.toolPreview (seam);
                return;
            }
            host.toolPreview (ae::copy (job->doc.audio, { r.empty() ? 0 : r.start, end }));
            return;
        }
        host.toolApply (tr (toolTitle (tool)), std::move (job->doc), job->selection);
        accepted = true;
        close();
    }

    void stopWorker()
    {
        if (worker.joinable())
        {
            progress->cancel = true;
            worker.join();
        }
    }

    void setButtonsEnabled (bool e)
    {
        for (auto* b : { &resetButton, &previewButton, &acceptButton })
            b->setEnabled (e);
        cancelButton.setButtonText (e ? tr ("Cancel") : tr ("Stop"));
    }

    void close()
    {
        juce::Component::SafePointer<ToolPanel> self (this);
        juce::MessageManager::callAsync ([self] {
            if (self != nullptr)
                closeToolWindowOf (*self);
        });
    }

    // --- helpers ----------------------------------------------------------------------------------------
    // the selection's average spectrum per curve column (0..1 = -90..0 dB), for the EQ's background
    static std::vector<float> spectrumBackground (const ae::Audio& audio, ae::Range r, double freqScale)
    {
        std::vector<float> out (256, 0.0f);
        if (audio.empty() || r.empty())
            return out;
        // at most ~4 s of the range, evenly
        const auto s = ae::spectrogram (audio, r, 48, 4096);
        if (s.columns == 0)
            return out;
        std::vector<double> mean ((std::size_t) s.bins, 0.0);
        for (int c = 0; c < s.columns; ++c)
            for (int b = 0; b < s.bins; ++b)
                mean[(std::size_t) b] += std::pow (10.0, s.at (c, b) / 10.0);
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const auto hz = ae::curveFrequency ((double) i / (double) (out.size() - 1), freqScale, audio.sampleRate);
            const auto bin = juce::jlimit (0, s.bins - 1, (int) std::lround (hz / (audio.sampleRate * 0.5) * s.bins));
            const auto db = 10.0 * std::log10 (mean[(std::size_t) bin] / s.columns + 1e-12);
            out[i] = (float) juce::jlimit (0.0, 1.0, (db + 90.0) / 90.0);
        }
        return out;
    }

    void refreshProfileBackground (CurveEditor& c)
    {
        const auto& p = host.toolNoiseProfile();
        if (! p.valid())
        {
            c.background = spectrumBackground (host.toolDocument().audio, ae::orAll (host.toolDocument(), host.toolSelection()), c.freqScale);
            c.repaint();
            return;
        }
        std::vector<float> out (256);
        const auto bins = (int) p.magnitude.size();
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const auto hz = ae::curveFrequency ((double) i / (double) (out.size() - 1), c.freqScale, p.sampleRate);
            const auto bin = juce::jlimit (0, bins - 1, (int) std::lround (hz / p.sampleRate * p.fftSize));
            const auto db = 20.0 * std::log10 (p.magnitude[(std::size_t) bin] * 2.0 / (p.fftSize * 0.5) + 1e-12);
            out[i] = (float) juce::jlimit (0.0, 1.0, (db + 120.0) / 120.0);
        }
        c.background = std::move (out);
        c.repaint();
    }

    static double detectFundamental (const ae::Audio& audio, ae::Range r)
    {
        // autocorrelation over the middle 100 ms of the range, 40..1000 Hz
        if (audio.empty())
            return 0.0;
        const auto sr = audio.sampleRate;
        const auto mid = (r.start + r.end) / 2;
        const auto half = (std::int64_t) (0.05 * sr);
        const auto from = std::max<std::int64_t> (0, mid - half), to = std::min (audio.frames(), mid + half);
        const auto lo = (int) (sr / 1000.0), hi = (int) (sr / 40.0);
        if (to - from < 2 * hi)
            return 0.0;
        std::vector<double> x ((std::size_t) (to - from));
        for (auto i = from; i < to; ++i)
            for (const auto& ch : audio.channels)
                x[(std::size_t) (i - from)] += ch[(std::size_t) i];
        double e0 = 0.0;
        for (std::size_t i = 0; i + (std::size_t) hi < x.size(); ++i)
            e0 += x[i] * x[i];
        if (e0 <= 1e-12)
            return 0.0;
        std::vector<double> nsdf ((std::size_t) hi + 1, 0.0);
        for (int l = lo; l <= hi; ++l)
        {
            double acc = 0.0, e = 0.0;
            for (std::size_t i = 0; i + (std::size_t) hi < x.size(); ++i)
            {
                acc += x[i] * x[i + (std::size_t) l];
                e += x[i] * x[i] + x[i + (std::size_t) l] * x[i + (std::size_t) l];
            }
            nsdf[(std::size_t) l] = e > 0 ? 2.0 * acc / e : 0.0;
        }
        // the first peak above 0.8 of the best (avoids octave errors)
        const auto best = *std::max_element (nsdf.begin(), nsdf.end());
        if (best < 0.5)
            return 0.0;
        for (int l = lo + 1; l < hi; ++l)
            if (nsdf[(std::size_t) l] >= 0.8 * best && nsdf[(std::size_t) l] >= nsdf[(std::size_t) l - 1] && nsdf[(std::size_t) l] >= nsdf[(std::size_t) l + 1])
            {
                // parabolic refinement
                const auto a = nsdf[(std::size_t) l - 1], b = nsdf[(std::size_t) l], c = nsdf[(std::size_t) l + 1];
                const auto d = a - 2 * b + c;
                const auto shift = std::abs (d) > 1e-12 ? 0.5 * (a - c) / d : 0.0;
                return sr / ((double) l + shift);
            }
        return 0.0;
    }

    // Match spectrum: the EQ curve that moves the range's long-term spectrum to the reference's (1/3 octave)
    static ae::Envelope matchCurve (const ae::Audio& audio, ae::Range r, const ae::Audio& reference, double freqScale)
    {
        const auto ref = ae::conform (reference, audio.numChannels(), audio.sampleRate);
        auto spectrum = [] (const ae::Audio& a, ae::Range rr) {
            const auto s = ae::spectrogram (a, rr, 64, 8192);
            std::vector<double> m ((std::size_t) s.bins, 0.0);
            for (int c = 0; c < s.columns; ++c)
                for (int b = 0; b < s.bins; ++b)
                    m[(std::size_t) b] += std::pow (10.0, s.at (c, b) / 10.0);
            return m;
        };
        const auto mine = spectrum (audio, r);
        const auto theirs = spectrum (ref, { 0, ref.frames() });
        const auto bins = (int) mine.size();
        ae::Envelope e;
        constexpr int points = 48;
        std::vector<double> db (points);
        for (int i = 0; i < points; ++i)
        {
            const auto x = (double) i / (points - 1);
            const auto hz = ae::curveFrequency (x, freqScale, audio.sampleRate);
            // a third of an octave around hz
            const auto b0 = juce::jlimit (1, bins - 1, (int) (hz * std::pow (2.0, -1.0 / 6.0) / (audio.sampleRate * 0.5) * bins));
            const auto b1 = juce::jlimit (b0, bins - 1, (int) (hz * std::pow (2.0, 1.0 / 6.0) / (audio.sampleRate * 0.5) * bins));
            double a = 0.0, b = 0.0;
            for (int k = b0; k <= b1; ++k)
            {
                a += mine[(std::size_t) k];
                b += theirs[(std::size_t) k];
            }
            db[(std::size_t) i] = juce::jlimit (-18.0, 18.0, 10.0 * std::log10 ((b + 1e-20) / (a + 1e-20)));
        }
        auto sorted = db;
        std::nth_element (sorted.begin(), sorted.begin() + points / 2, sorted.end());
        const auto median = sorted[(std::size_t) points / 2];
        for (int i = 0; i < points; ++i)
        {
            const auto d = juce::jlimit (-18.0, 18.0, db[(std::size_t) i] - median);
            // eqGain's inverse: above 0.5 the dB are linear in y; below, 4 y^2 = gain
            const auto y = d >= 0 ? 0.5 + d / 36.0 : 0.5 * std::sqrt (std::pow (10.0, d / 20.0));
            e.points.push_back ({ (double) i / (points - 1), y, 0.0f });
        }
        return e;
    }

    void loadImpulse (CurveEditor& c)
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Load an impulse response"), juce::File(), "*.wav;*.aif;*.aiff;*.flac");
        juce::Component::SafePointer<ToolPanel> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [self, &c] (const juce::FileChooser& fc) {
            if (self == nullptr || fc.getResults().isEmpty())
                return;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (fc.getResult()));
            if (reader == nullptr || reader->lengthInSamples <= 0)
                return;
            const auto frames = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 30.0));
            juce::AudioBuffer<float> buffer ((int) std::min<unsigned int> (2, reader->numChannels), frames);
            reader->read (&buffer, 0, frames, 0, true, buffer.getNumChannels() > 1);
            ae::Audio a;
            a.sampleRate = reader->sampleRate;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                a.channels.emplace_back (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + frames);
            // unit energy like the drawn impulse (the wet level reads the same)
            for (auto& v : a.channels)
            {
                double e = 0.0;
                for (auto s : v)
                    e += (double) s * s;
                const auto g = e > 0 ? 1.0 / std::sqrt (e) : 0.0;
                for (auto& s : v)
                    s = (float) (s * g);
            }
            self->impulse = std::move (a);
            self->impulseName = fc.getResult().getFileNameWithoutExtension();
            c.caption = tr ("loaded: {0} (the curve is not used)", { self->impulseName });
            c.repaint();
            self->updateInfo();
        });
    }

    Host& host;
    Tool tool;
    juce::Component::SafePointer<juce::Component> hostComponent;
    juce::Label title, info;
    std::vector<Control> controls;
    std::vector<std::unique_ptr<juce::Label>> labels;
    std::vector<std::unique_ptr<juce::Component>> editors;
    std::vector<std::unique_ptr<CurveEditor>> curves;
    std::vector<ae::Envelope> initialCurves;
    std::map<int, CurveEditor*> curveAfter;
    juce::OwnedArray<juce::TextButton> extraButtons;
    juce::TextButton resetButton, previewButton, acceptButton, cancelButton;
    std::vector<juce::Rectangle<int>> sectionLines;
    std::unique_ptr<juce::FileChooser> chooser;
    ae::Audio impulse;
    juce::String impulseName;
};
} // namespace

std::unique_ptr<juce::Component> makeToolPanel (Host& host, Tool tool) { return std::make_unique<ToolPanel> (host, tool); }

void showTool (Host& host, Tool tool)
{
    openToolWindow (makeToolPanel (host, tool), tr (toolTitle (tool)), "audioeditor-tool");
}
} // namespace lynx::audiotools
