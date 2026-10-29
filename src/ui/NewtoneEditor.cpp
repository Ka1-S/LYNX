#include "ui/SlotWatch.h"
#include "ui/NewtoneEditor.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "core/NewtoneFx.h"
#include "core/VocalPitch.h"
#include "engine/MidiFileIo.h"
#include "engine/Text.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
bool isNewtoneKind (EffectKind k)
{
    return k == EffectKind::newtone || k == EffectKind::newtime;
}

const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// FL's naming, as LYNX's piano roll (round 149: the editor said D4 where the piano roll says D5): key 60 = C5
juce::String noteName (double midi)
{
    const auto k = (int) std::lround (midi);
    return juce::String (noteNames[((k % 12) + 12) % 12]) + juce::String (k / 12);
}

// Snap to scale (FL's Edit › Snap to scale / Scale): pitch classes from the key, bit n = key + n
struct ScaleDef
{
    const char* name;
    std::uint16_t mask;
};
const ScaleDef scales[] = {
    { TR_NOOP ("Chromatic"), 0xFFF },
    { TR_NOOP ("Major"), 0b101010110101 },
    { TR_NOOP ("Minor"), 0b010110101101 },
    { TR_NOOP ("Harmonic minor"), 0b100110101101 },
    { TR_NOOP ("Melodic minor"), 0b101010101101 },
    { TR_NOOP ("Dorian"), 0b011010101101 },
    { TR_NOOP ("Mixolydian"), 0b011010110101 },
    { TR_NOOP ("Major pentatonic"), 0b001010010101 },
    { TR_NOOP ("Minor pentatonic"), 0b010010101001 },
    { TR_NOOP ("Blues"), 0b010011101001 },
};

// A labelled bar slider of the editor's knob rows / note panel
struct BarSlider
{
    juce::Label label;
    juce::Slider slider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };

    void setUp (juce::Component& owner, const juce::String& caption, double lo, double hi, double step, double def, const juce::String& tip)
    {
        label.setText (caption, juce::dontSendNotification);
        label.setFont (theme::font (theme::type::small));
        label.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        label.setJustificationType (juce::Justification::centredLeft);
        slider.setRange (lo, hi, step);
        slider.setDoubleClickReturnValue (true, def);
        slider.setValue (def, juce::dontSendNotification);
        slider.setTooltip (tip);
        label.setTooltip (tip);
        slider.setWantsKeyboardFocus (false);
        slider.setColour (juce::Slider::trackColourId, theme::colour::accentMuted);
        slider.setColour (juce::Slider::textBoxOutlineColourId, theme::colour::borderStrong);
        owner.addAndMakeVisible (label);
        owner.addAndMakeVisible (slider);
    }
    void setVisible (bool v)
    {
        label.setVisible (v);
        slider.setVisible (v);
    }
    void place (juce::Rectangle<int> r, int labelWidth)
    {
        label.setBounds (r.removeFromLeft (labelWidth));
        slider.setBounds (r);
    }
};

class NewtoneEditor final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    NewtoneEditor (AudioEngine& e, int t, int s) : engine (e), track (t), slot (s)
    {
        for (auto* b : { &load, &analyse, &cutMode, &snapAll, &reset, &preview, &save, &scoreButton, &stretch })
        {
            addAndMakeVisible (*b);
            b->setWantsKeyboardFocus (false);
        }
        addAndMakeVisible (slave);
        addAndMakeVisible (status);
        status.setFont (theme::font (theme::type::small));
        status.setColour (juce::Label::textColourId, theme::colour::textSecondary);

        load.setButtonText (tr ("Load sample..."));
        analyse.setButtonText (tr ("Analyse"));
        cutMode.setButtonText (tr ("Cut"));
        cutMode.setClickingTogglesState (true);
        cutMode.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
        cutMode.setTooltip (tr ("Cut mode (C): click a note to slice it there; Ctrl+click a note to glue it to the one before"));
        snapAll.setButtonText (tr ("Snap all to notes"));
        reset.setButtonText (tr ("Reset edits"));
        preview.setButtonText (tr ("Preview"));
        save.setButtonText (tr ("Save as WAV..."));
        scoreButton.setButtonText (tr ("Score"));
        scoreButton.setTooltip (tr ("The notes as a score: send them to a channel's piano roll, or save them as a MIDI file"));
        stretch.setButtonText (tr ("Length..."));
        slave.setButtonText (tr ("Play with the song"));
        slave.setTooltip (tr ("Slave to host playback: while the song plays, the edited sample plays from the song's start"));
        load.setTooltip (tr ("Choose the audio file to edit (WAV, AIFF, FLAC, OGG, MP3)"));
        analyse.setTooltip (tr ("Find the notes of the sample again (discards the note edits)"));

        load.onClick = [this] { chooseSample(); };
        analyse.onClick = [this] { reanalyse(); };
        cutMode.onClick = [this] { updateButtons(); };
        snapAll.onClick = [this] {
            for (int k = 0; k < (int) notes.pitches.size(); ++k)
                snapNote (k);
            commit();
        };
        reset.onClick = [this] {
            if (warpMode())
                warp.markers.clear();
            else
            {
                for (auto& p : notes.pitches)
                {
                    const auto origPitch = p.origPitch;
                    const auto a = p.origStart, b = p.origEnd;
                    const auto si = p.origSmoothIn, so = p.origSmoothOut;
                    p = {};
                    p.origPitch = origPitch;
                    p.origStart = p.curStart = a;
                    p.origEnd = p.curEnd = b;
                    p.smoothIn = p.origSmoothIn = si;
                    p.smoothOut = p.origSmoothOut = so;
                }
                notes.centre = 0.0;
                notes.variation = notes.transition = 1.0;
                notes.globalFormant = 1.0;
            }
            commit();
        };
        preview.onClick = [this] { togglePreview(); };
        save.onClick = [this] { saveWav(); };
        scoreButton.onClick = [this] { showScoreMenu(); };
        stretch.onClick = [this] { showLengthMenu(); };
        slave.onClick = [this] {
            const auto on = slave.getToggleState();
            engine.editSong ([this, on] (Song& song) {
                if (auto* fx = slotIn (song))
                    fx->params[0] = on ? 1.0f : 0.0f;
            });
        };

        // FL's global knobs: they act on the selected notes, or on every note when none is selected
        centreKnob.setUp (*this, tr ("Center"), 0.0, 100.0, 1.0, 0.0, tr ("Pulls the notes toward their semitone: 0 % as sung, 100 % exactly on it"));
        variationKnob.setUp (*this, tr ("Variation"), 0.0, 100.0, 1.0, 100.0, tr ("The notes' vibrato and pitch drift: 100 % as sung, 0 % flat"));
        transitionKnob.setUp (*this, tr ("Transition"), 0.0, 200.0, 1.0, 100.0, tr ("How long the pitch takes to move from one note to the next (100 % = as analysed)"));
        centreKnob.slider.setTextValueSuffix (" %");
        variationKnob.slider.setTextValueSuffix (" %");
        transitionKnob.slider.setTextValueSuffix (" %");
        hookSlider (centreKnob, [this] (double v) { newtonefx::applyCentre (notes, v / 100.0, selection()); });
        hookSlider (variationKnob, [this] (double v) { newtonefx::applyVariation (notes, v / 100.0, selection()); });
        hookSlider (transitionKnob, [this] (double v) { newtonefx::applyTransition (notes, v / 100.0, selection()); });

        for (int k = 0; k < 12; ++k)
            keyBox.addItem (noteNames[k], k + 1);
        for (int k = 0; k < (int) std::size (scales); ++k)
            scaleBox.addItem (tr (scales[k].name), k + 1);
        keyBox.setSelectedId (1, juce::dontSendNotification);
        scaleBox.setSelectedId (1, juce::dontSendNotification);
        keyBox.setTooltip (tr ("The key of the scale the notes snap to"));
        scaleBox.setTooltip (tr ("Snap to scale: notes move only to the notes of this scale (Chromatic = every semitone)"));
        for (auto* c : { &keyBox, &scaleBox })
        {
            addAndMakeVisible (*c);
            c->setWantsKeyboardFocus (false);
        }

        // the note panel (FL's Advanced Edit): the selected notes' own settings
        addAndMakeVisible (panelTitle);
        panelTitle.setFont (theme::font (theme::type::small, true));
        panelTitle.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        pitchKnob.setUp (*this, tr ("Pitch"), -24.0, 24.0, 0.01, 0.0, tr ("The note's pitch change in semitones (Alt+drag a note does the same)"));
        formantKnob.setUp (*this, tr ("Formant"), -12.0, 12.0, 0.1, 0.0, tr ("Moves the voice's formants (its apparent size, age or sex) without changing the pitch"));
        volumeKnob.setUp (*this, tr ("Volume"), -24.0, 12.0, 0.1, 0.0, tr ("The note's level in dB"));
        driftKnob.setUp (*this, tr ("Variation"), 0.0, 100.0, 1.0, 100.0, tr ("This note's vibrato and pitch drift: 100 % as sung, 0 % flat"));
        rampInKnob.setUp (*this, tr ("Ramp in"), -12.0, 12.0, 0.05, 0.0, tr ("Starts the note this many semitones off and glides to it over the transition time"));
        rampOutKnob.setUp (*this, tr ("Ramp out"), -12.0, 12.0, 0.05, 0.0, tr ("Glides the note's end this many semitones away over the transition time"));
        vibRateKnob.setUp (*this, tr ("Vibrato rate"), 0.0, 12.0, 0.05, 0.0, tr ("An added vibrato's speed in Hz (0 = none)"));
        vibStartKnob.setUp (*this, tr ("Vibrato start"), 0.0, 2.0, 0.01, 0.0, tr ("The added vibrato's depth at the note's start, in semitones"));
        vibEndKnob.setUp (*this, tr ("Vibrato end"), 0.0, 2.0, 0.01, 0.0, tr ("The added vibrato's depth at the note's end, in semitones"));
        tremoloKnob.setUp (*this, tr ("Tremolo"), 0.0, 100.0, 1.0, 0.0, tr ("A volume wobble in step with the added vibrato"));
        pitchKnob.slider.setTextValueSuffix (" st");
        formantKnob.slider.setTextValueSuffix (" st");
        volumeKnob.slider.setTextValueSuffix (" dB");
        driftKnob.slider.setTextValueSuffix (" %");
        rampInKnob.slider.setTextValueSuffix (" st");
        rampOutKnob.slider.setTextValueSuffix (" st");
        vibRateKnob.slider.setTextValueSuffix (" Hz");
        vibStartKnob.slider.setTextValueSuffix (" st");
        vibEndKnob.slider.setTextValueSuffix (" st");
        tremoloKnob.slider.setTextValueSuffix (" %");
        auto perNote = [this] (BarSlider& b, std::function<void (newtonefx::Note&, double)> set) {
            hookSlider (b, [this, set] (double v) {
                for (const auto k : selection())
                    set (notes.pitches[(std::size_t) k], v);
            });
        };
        perNote (pitchKnob, [] (newtonefx::Note& p, double v) { p.shift = v; });
        perNote (formantKnob, [] (newtonefx::Note& p, double v) { p.formant = std::exp2 (v / 12.0); });
        perNote (volumeKnob, [] (newtonefx::Note& p, double v) { p.volume = std::pow (10.0, v / 20.0); });
        perNote (driftKnob, [] (newtonefx::Note& p, double v) { p.drift = v / 100.0; });
        perNote (rampInKnob, [] (newtonefx::Note& p, double v) { p.rampIn = v; });
        perNote (rampOutKnob, [] (newtonefx::Note& p, double v) { p.rampOut = v; });
        perNote (vibRateKnob, [] (newtonefx::Note& p, double v) { p.vibPeriod = v > 0.0 ? 1.0 / v : 0.0; });
        perNote (vibStartKnob, [] (newtonefx::Note& p, double v) { p.vibAmpStart = v; });
        perNote (vibEndKnob, [] (newtonefx::Note& p, double v) { p.vibAmpEnd = v; });
        perNote (tremoloKnob, [] (newtonefx::Note& p, double v) { p.tremAmp = v / 100.0; });

        setWantsKeyboardFocus (true);
        engine.addChangeListener (this);
        reloadFromSong();
        startTimerHz (30);
        setSize (1040, 520);
    }

    ~NewtoneEditor() override
    {
        engine.removeChangeListener (this);
        if (previewing)
            engine.previewSample (nullptr);
    }

    // ------------------------------------------------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        const auto area = canvasArea();
        g.setColour (theme::colour::bg0);
        g.fillRect (area);
        if (! warpMode() && sample != nullptr)
        {
            g.setColour (theme::colour::bg1);
            g.fillRect (panelArea());
            g.fillRect (rulerArea());
        }

        if (sample == nullptr)
        {
            g.setColour (theme::colour::textSecondary);
            g.setFont (theme::font (theme::type::body));
            g.drawText (validSlot() ? tr ("Load a sample to edit it (Load sample... or drop an audio file here)") : tr ("This slot no longer holds this plugin"),
                        area, juce::Justification::centred);
            return;
        }

        if (warpMode())
            paintWarp (g, area);
        else
        {
            paintNotes (g, area);
            paintRuler (g, rulerArea());
        }

        // the rubber band of a selection drag
        if (! band.isEmpty())
        {
            g.setColour (theme::colour::selectionFill);
            g.fillRect (band);
            g.setColour (theme::colour::selection);
            g.drawRect (band, 1.0f);
        }

        // playhead of the preview
        if (previewing)
            if (const auto pos = engine.getPreviewPosition(); pos >= 0.0)
            {
                const auto x = xOfOutput (pos);
                g.setColour (theme::colour::playhead);
                g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
            }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (26);
        for (auto* b : { &load, &analyse, &cutMode, &snapAll, &stretch, &reset, &preview, &save, &scoreButton })
            if (b->isVisible())
            {
                b->setBounds (top.removeFromLeft (juce::jmax (60, b->getBestWidthForHeight (26) + 14)));
                top.removeFromLeft (4);
            }
        slave.setBounds (top.removeFromRight (170));
        r.removeFromTop (4);
        if (! warpMode())
        {
            auto knobs = r.removeFromTop (24);
            const auto w = juce::jmin (230, (knobs.getWidth() - 260) / 3);
            const auto lw = theme::labelColumnWidth ({ &centreKnob.label, &variationKnob.label, &transitionKnob.label }, 64, 120);
            for (auto* k : { &centreKnob, &variationKnob, &transitionKnob })
            {
                k->place (knobs.removeFromLeft (w), lw);
                knobs.removeFromLeft (8);
            }
            knobs.removeFromLeft (8);
            keyBox.setBounds (knobs.removeFromLeft (64));
            knobs.removeFromLeft (4);
            scaleBox.setBounds (knobs.removeFromLeft (160));
            r.removeFromTop (4);
        }
        status.setBounds (r.removeFromBottom (18));
        if (! warpMode())
        {
            auto panel = panelArea().reduced (6);
            panelTitle.setBounds (panel.removeFromTop (20));
            const auto lw = theme::labelColumnWidth ({ &pitchKnob.label, &formantKnob.label, &volumeKnob.label, &driftKnob.label, &rampInKnob.label, &rampOutKnob.label,
                                                       &vibRateKnob.label, &vibStartKnob.label, &vibEndKnob.label, &tremoloKnob.label },
                                                     92, 130);
            for (auto* k : { &pitchKnob, &formantKnob, &volumeKnob, &driftKnob, &rampInKnob, &rampOutKnob, &vibRateKnob, &vibStartKnob, &vibEndKnob, &tremoloKnob })
            {
                k->place (panel.removeFromTop (22), lw);
                panel.removeFromTop (3);
            }
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto cmd = key.getModifiers().isCommandDown();
        if (cmd && key.getKeyCode() == 'Z')
        {
            if (key.getModifiers().isShiftDown())
                engine.redo();
            else
                engine.undo();
            return true;
        }
        if (cmd && key.getKeyCode() == 'Y')
        {
            engine.redo();
            return true;
        }
        if (warpMode() || sample == nullptr)
            return false;
        if (cmd && key.getKeyCode() == 'A')
        {
            selected.clear();
            for (int k = 0; k < (int) notes.pitches.size(); ++k)
                selected.insert (k);
            selectionChanged();
            return true;
        }
        if (! cmd && (key.getKeyCode() == 'C' || key.getKeyCode() == 'c'))
        {
            cutMode.setToggleState (! cutMode.getToggleState(), juce::dontSendNotification);
            updateButtons();
            return true;
        }
        if (key == juce::KeyPress::escapeKey)
        {
            selected.clear();
            selectionChanged();
            return true;
        }
        if (selected.empty())
            return false;
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            for (const auto k : selected)
                notes.pitches[(std::size_t) k].shift += key == juce::KeyPress::upKey ? 1.0 : -1.0;
            commit();
            return true;
        }
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            const auto mute = std::any_of (selected.begin(), selected.end(), [this] (int k) { return notes.pitches[(std::size_t) k].enabled; });
            for (const auto k : selected)
                notes.pitches[(std::size_t) k].enabled = ! mute;
            commit();
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        drag = {};
        band = {};
        if (sample == nullptr)
            return;
        if (! warpMode() && rulerArea().contains (e.getPosition()))
        {
            // FL's ruler click: the selected notes all move to that pitch (auto-flatten)
            const auto key = std::round (pitchOfY (e.position.y));
            for (const auto k : selection())
            {
                auto& p = notes.pitches[(std::size_t) k];
                p.shift = key - p.origPitch;
            }
            commit();
            return;
        }
        if (! canvasArea().contains (e.getPosition()))
            return;
        if (warpMode())
        {
            const auto m = markerAt (e.position.x);
            if (e.mods.isPopupMenu())
            {
                if (m >= 0)
                {
                    warp.markers.erase (warp.markers.begin() + m);
                    commit();
                }
                return;
            }
            drag.marker = m;
            return;
        }

        const auto hit = noteAt (e.position);
        if (cutMode.getToggleState() && hit >= 0 && ! e.mods.isPopupMenu())
        {
            if (e.mods.isCtrlDown() || e.mods.isCommandDown())
            {
                if (newtonefx::glueNote (notes, (std::size_t) hit))
                {
                    selected = { hit - 1 };
                    commit();
                }
            }
            else
                cutAt (hit, e.position.x);
            return;
        }
        if (hit < 0)
        {
            if (! e.mods.isShiftDown())
                selected.clear();
            drag.banding = ! e.mods.isPopupMenu();
            drag.bandStart = e.position;
            selectionChanged();
            return;
        }
        if (e.mods.isShiftDown())
        {
            if (! selected.erase (hit))
                selected.insert (hit);
            selectionChanged();
            return;
        }
        if (selected.count (hit) == 0)
        {
            selected = { hit };
            selectionChanged();
        }
        if (e.mods.isPopupMenu())
        {
            showNoteMenu (hit);
            return;
        }
        // a drag moves every selected note (pitch, or time with Ctrl)
        drag.note = hit;
        drag.time = e.mods.isCtrlDown() || e.mods.isCommandDown();
        for (const auto k : selected)
        {
            const auto& p = notes.pitches[(std::size_t) k];
            drag.from.push_back ({ k, p.shift, p.curStart, p.curEnd });
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (warpMode())
        {
            if (drag.marker < 0 || drag.marker >= (int) warp.markers.size())
                return;
            auto& m = warp.markers[(std::size_t) drag.marker];
            const auto lo = drag.marker > 0 ? warp.markers[(std::size_t) drag.marker - 1].output + 1 : 1;
            const auto hi = drag.marker + 1 < (int) warp.markers.size() ? warp.markers[(std::size_t) drag.marker + 1].output - 1
                                                                        : (std::int64_t) (4 * frames());
            m.output = std::clamp<std::int64_t> ((std::int64_t) std::llround (outputOfX (e.position.x)), lo, std::max (lo, hi));
            drag.moved = true;
            repaint();
            return;
        }
        if (drag.banding)
        {
            band = juce::Rectangle<float> (drag.bandStart, e.position);
            auto picked = e.mods.isShiftDown() ? selectedBefore : std::set<int> {};
            for (int k = 0; k < (int) notes.pitches.size(); ++k)
                if (band.intersects (noteBounds (k)))
                    picked.insert (k);
            selected = picked;
            selectionChanged();
            return;
        }
        if (drag.note < 0)
            return;
        auto anchor = std::find_if (drag.from.begin(), drag.from.end(), [this] (const auto& f) { return f.note == drag.note; });
        if (anchor == drag.from.end())
            return;
        for (const auto& f : drag.from)
        {
            if (f.note < 0 || f.note >= (int) notes.pitches.size())
                continue;
            auto& p = notes.pitches[(std::size_t) f.note];
            if (drag.time)
            {
                const auto dx = (std::int64_t) std::llround ((double) e.getDistanceFromDragStartX() / pixelsPerSample());
                const auto len = f.end - f.start;
                p.curStart = std::clamp<std::int64_t> (f.start + dx, 0, std::max<std::int64_t> (0, frames() - len));
                p.curEnd = p.curStart + len;
            }
            else
            {
                // the dragged note snaps (to the scale; Alt = free) and the others follow it by the same amount
                auto s = anchor->shift - (double) e.getDistanceFromDragStartY() / rowHeight();
                const auto& a = notes.pitches[(std::size_t) anchor->note];
                if (! e.mods.isAltDown())
                    s = snapped (a.origPitch + s) - a.origPitch;
                p.shift = std::clamp (f.shift + (s - anchor->shift), -24.0, 24.0);
            }
        }
        drag.moved = true;
        updatePanel();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag.moved)
            commit();
        drag = {};
        band = {};
        selectedBefore = selected;
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto cutting = ! warpMode() && cutMode.getToggleState() && noteAt (e.position) >= 0;
        setMouseCursor (cutting ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (sample == nullptr || ! canvasArea().contains (e.getPosition()))
            return;
        if (warpMode())
        {
            // a new marker where the sample plays now (it does not move anything until dragged)
            const auto out = outputOfX (e.position.x);
            const auto src = (std::int64_t) std::llround (newtonefx::warpToSource (warp, frames(), out));
            newtonefx::WarpMarker m { src, (std::int64_t) std::llround (out) };
            if (m.source <= 0 || m.source >= frames())
                return;
            auto at = std::lower_bound (warp.markers.begin(), warp.markers.end(), m, [] (const auto& a, const auto& b) { return a.source < b.source; });
            if (at != warp.markers.end() && at->source == m.source)
                return;
            warp.markers.insert (at, m);
            // the first marker also pins the sample's end where it plays now, so moving a marker stretches one side
            // and squeezes the other instead of changing the whole length
            if (warp.markers.back().source < frames() - 1)
                warp.markers.push_back ({ frames() - 1, (std::int64_t) std::llround (newtonefx::warpToOutput (warp, frames(), (double) (frames() - 1))) });
            commit();
            return;
        }
        if (cutMode.getToggleState())
            return;
        if (const auto k = noteAt (e.position); k >= 0)
        {
            snapNote (k);
            commit();
        }
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return validSlot() && files.size() == 1 && SampleLibrary::isSampleFile (juce::File (files[0]));
    }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        if (isInterestedInFileDrag (files))
            useSample (juce::File (files[0]));
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (sample == nullptr)
            return;
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            const auto anchor = outputOfX (e.position.x);
            zoom = std::clamp (zoom * (w.deltaY > 0 ? 1.25 : 0.8), 1.0, 64.0);
            scroll = anchor - (e.position.x - (float) canvasArea().getX()) / pixelsPerSample();
        }
        else
            scroll -= (w.deltaY != 0.0f ? w.deltaY : w.deltaX) * 0.25 * visibleLength();
        clampScroll();
        repaint();
    }

private:
    struct Drag
    {
        struct From
        {
            int note = -1;
            double shift = 0.0;
            std::int64_t start = 0, end = 0;
        };
        int note = -1, marker = -1;
        std::vector<From> from;
        bool time = false, moved = false, banding = false;
        juce::Point<float> bandStart;
    };

    AudioEngine& engine;
    int track, slot;
    juce::TextButton load, analyse, cutMode, snapAll, reset, preview, save, scoreButton, stretch;
    juce::ToggleButton slave;
    juce::Label status, panelTitle;
    BarSlider centreKnob, variationKnob, transitionKnob;
    BarSlider pitchKnob, formantKnob, volumeKnob, driftKnob, rampInKnob, rampOutKnob, vibRateKnob, vibStartKnob, vibEndKnob, tremoloKnob;
    juce::ComboBox keyBox, scaleBox;
    std::unique_ptr<juce::FileChooser> chooser;

    EffectKind kind = EffectKind::newtone;
    std::string samplePath;
    std::vector<std::uint8_t> savedState; // the state shown (to tell our own edits from outside changes)
    std::shared_ptr<const SampleData> sample;
    std::vector<float> peaks;              // |sample| maxima per 256 frames
    std::vector<double> pitchTrack;        // the sample's pitch (MIDI, < 0 unvoiced) per pitchHop frames
    int pitchHop = 256;
    newtonefx::Track notes;
    newtonefx::WarpMap warp;
    std::set<int> selected, selectedBefore;
    Drag drag;
    juce::Rectangle<float> band;
    double zoom = 1.0, scroll = 0.0;       // horizontal view: zoom over the whole length, first visible sample
    bool previewing = false;

    // ------------------------------------------------------------------------------------------------ selection
    // the notes an edit applies to: the selection, or every note
    std::vector<int> selection() const
    {
        if (! selected.empty())
            return { selected.begin(), selected.end() };
        std::vector<int> all ((std::size_t) notes.pitches.size());
        for (int k = 0; k < (int) all.size(); ++k)
            all[(std::size_t) k] = k;
        return all;
    }

    void selectionChanged()
    {
        for (auto it = selected.begin(); it != selected.end();)
            it = *it < 0 || *it >= (int) notes.pitches.size() ? selected.erase (it) : std::next (it);
        updatePanel();
        repaint();
    }

    void hookSlider (BarSlider& b, std::function<void (double)> apply)
    {
        b.slider.onValueChange = [this, &b, apply] {
            if (updatingPanel || sample == nullptr || warpMode())
                return;
            apply (b.slider.getValue());
            if (! b.slider.isMouseButtonDown())
                commit();
            else
                repaint();
        };
        b.slider.onDragEnd = [this] {
            if (! updatingPanel)
                commit();
        };
    }
    bool updatingPanel = false;

    void updatePanel()
    {
        const juce::ScopedValueSetter<bool> guard (updatingPanel, true);
        const auto noteMode = ! warpMode() && sample != nullptr;
        for (auto* k : { &centreKnob, &variationKnob, &transitionKnob })
            k->setVisible (! warpMode());
        keyBox.setVisible (! warpMode());
        scaleBox.setVisible (! warpMode());
        panelTitle.setVisible (noteMode);
        const auto one = noteMode && ! selected.empty();
        for (auto* k : { &pitchKnob, &formantKnob, &volumeKnob, &driftKnob, &rampInKnob, &rampOutKnob, &vibRateKnob, &vibStartKnob, &vibEndKnob, &tremoloKnob })
        {
            k->setVisible (noteMode);
            k->slider.setEnabled (one);
        }
        centreKnob.slider.setValue (notes.centre * 100.0, juce::dontSendNotification);
        variationKnob.slider.setValue (notes.variation * 100.0, juce::dontSendNotification);
        transitionKnob.slider.setValue (notes.transition * 100.0, juce::dontSendNotification);
        panelTitle.setText (selected.empty() ? tr ("Note: select one or more notes")
                                             : selected.size() == 1 ? tr ("Note {0}", { juce::String (*selected.begin() + 1) })
                                                                    : tr ("{0} notes selected", { juce::String ((int) selected.size()) }),
                            juce::dontSendNotification);
        if (! one)
            return;
        const auto& p = notes.pitches[(std::size_t) *selected.begin()];
        pitchKnob.slider.setValue (p.shift);
        formantKnob.slider.setValue (12.0 * std::log2 (std::max (1e-6, p.formant)));
        volumeKnob.slider.setValue (20.0 * std::log10 (std::max (1e-6, p.volume)));
        driftKnob.slider.setValue (p.drift * 100.0);
        rampInKnob.slider.setValue (p.rampIn);
        rampOutKnob.slider.setValue (p.rampOut);
        vibRateKnob.slider.setValue (p.vibPeriod > 0.0 ? 1.0 / p.vibPeriod : 0.0);
        vibStartKnob.slider.setValue (p.vibAmpStart);
        vibEndKnob.slider.setValue (p.vibAmpEnd);
        tremoloKnob.slider.setValue (p.tremAmp * 100.0);
    }

    std::uint16_t scaleMask() const
    {
        const auto i = std::clamp (scaleBox.getSelectedId() - 1, 0, (int) std::size (scales) - 1);
        return scales[i].mask;
    }
    int scaleKey() const { return std::max (0, keyBox.getSelectedId() - 1); }
    double snapped (double pitch) const { return newtonefx::nearestInScale (pitch, scaleKey(), scaleMask()); }
    void snapNote (int k)
    {
        auto& p = notes.pitches[(std::size_t) k];
        if (p.origPitch > 0.0)
            p.shift = snapped (p.origPitch + p.shift) - p.origPitch;
    }

    void cutAt (int k, float x)
    {
        const auto at = (std::int64_t) std::llround (outputOfX (x));
        struct Ctx
        {
            const std::vector<double>* track;
            int hop;
        } ctx { &pitchTrack, pitchHop };
        auto pitchOf = [] (const void* c, std::int64_t a, std::int64_t b) {
            const auto& q = *static_cast<const Ctx*> (c);
            std::vector<double> v;
            for (auto f = std::max<std::int64_t> (0, a / q.hop); f < std::min<std::int64_t> ((std::int64_t) q.track->size(), b / q.hop + 1); ++f)
                if ((*q.track)[(std::size_t) f] > 0.0)
                    v.push_back ((*q.track)[(std::size_t) f]);
            if (v.empty())
                return 0.0;
            std::nth_element (v.begin(), v.begin() + (std::ptrdiff_t) (v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        if (newtonefx::splitNote (notes, (std::size_t) k, at, pitchOf, &ctx))
        {
            selected = { k + 1 };
            commit();
        }
    }

    // --------------------------------------------------------------------------------------------------- the slot
    EffectSlot* slotIn (Song& song) const
    {
        if (track < 0 || track >= (int) song.mixer.size() || slot < 0 || slot >= (int) song.mixer[(std::size_t) track].effects.size())
            return nullptr;
        auto& fx = song.mixer[(std::size_t) track].effects[(std::size_t) slot];
        return isNewtoneKind (fx.kind) ? &fx : nullptr;
    }

    const EffectSlot* currentSlot() const
    {
        const auto& song = engine.getSong();
        if (track < 0 || track >= (int) song.mixer.size() || slot < 0 || slot >= (int) song.mixer[(std::size_t) track].effects.size())
            return nullptr;
        const auto& fx = song.mixer[(std::size_t) track].effects[(std::size_t) slot];
        return isNewtoneKind (fx.kind) ? &fx : nullptr;
    }

    bool validSlot() const { return currentSlot() != nullptr; }
    bool warpMode() const { return kind == EffectKind::newtime; }
    std::int64_t frames() const { return sample != nullptr ? (std::int64_t) sample->frames() : 0; }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        const auto* fx = currentSlot();
        if (fx == nullptr || fx->samplePath != samplePath || fx->state != savedState || fx->kind != kind)
            reloadFromSong();
        else
            slave.setToggleState (fx->params[0] >= 0.5f, juce::dontSendNotification);
    }

    void reloadFromSong()
    {
        const auto* fx = currentSlot();
        if (fx == nullptr)
        {
            sample = nullptr;
            updateButtons();
            repaint();
            return;
        }
        kind = fx->kind;
        slave.setToggleState (fx->params[0] >= 0.5f, juce::dontSendNotification);
        savedState = fx->state;
        if (fx->samplePath != samplePath || sample == nullptr)
        {
            samplePath = fx->samplePath;
            sample = samplePath.empty() ? nullptr : engine.getSampleLibrary().load (juce::String::fromUTF8 (samplePath.c_str()));
            analyseView();
        }
        if (warpMode())
        {
            warp = newtonefx::parseWarp (fx->state).value_or (newtonefx::WarpMap {});
            warp.frames = frames();
        }
        else if (auto t = newtonefx::trackOf (fx->state); t && sample != nullptr)
            notes = *t;
        else if (sample == nullptr)
            notes = {};
        else
        {
            // a sample without notes yet (a fresh slot, or a sample chosen from the slot menu): analyse it now
            notes = newtonefx::analyseNotes (*sample);
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NewtoneEditor> (this)] {
                if (safe != nullptr)
                    safe->commit();
            });
        }
        selectionChanged();
        updateButtons();
        repaint();
    }

    // the waveform and pitch curve shown (not saved)
    void analyseView()
    {
        peaks.clear();
        pitchTrack.clear();
        zoom = 1.0;
        scroll = 0.0;
        if (sample == nullptr)
            return;
        const auto n = (std::int64_t) sample->frames();
        for (std::int64_t i = 0; i < n; i += 256)
        {
            float m = 0.0f;
            for (auto j = i; j < std::min (n, i + 256); ++j)
                m = std::max ({ m, std::abs (sample->leftData()[j]), sample->isStereo() ? std::abs (sample->rightData()[j]) : 0.0f });
            peaks.push_back (m);
        }
        if (warpMode())
            return;
        std::vector<float> mono (sample->leftData(), sample->leftData() + n);
        if (sample->isStereo())
            for (std::int64_t i = 0; i < n; ++i)
                mono[(std::size_t) i] = 0.5f * (mono[(std::size_t) i] + sample->rightData()[i]);
        pitchHop = std::max (64, (int) std::lround (256.0 * sample->sampleRate / 44100.0));
        for (const auto& f : vocal::analysePitch (mono.data(), (int) n, sample->sampleRate, pitchHop))
            pitchTrack.push_back (f.hz > 0.0 ? vocal::hzToNote (f.hz) : -1.0);
    }

    void commit()
    {
        auto state = warpMode() ? newtonefx::writeWarp (warp) : newtonefx::writeNative (notes);
        savedState = state;
        engine.editSong ([this, state] (Song& song) {
            if (auto* fx = slotIn (song))
                fx->state = state;
        });
        if (previewing)
            startPreview();
        updateButtons();
        updatePanel();
        repaint();
    }

    void reanalyse()
    {
        if (sample == nullptr || warpMode())
            return;
        notes = newtonefx::analyseNotes (*sample);
        selected.clear();
        commit();
    }

    void chooseSample()
    {
        const juce::File current (juce::String::fromUTF8 (samplePath.c_str()));
        chooser = std::make_unique<juce::FileChooser> (tr ("Source sample"), current.existsAsFile() ? current : juce::File(), engine.getSampleLibrary().wildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<NewtoneEditor> (this)] (const juce::FileChooser& fc) {
                                  if (safe != nullptr && fc.getResult().existsAsFile())
                                      safe->useSample (fc.getResult());
                              });
    }

    void useSample (const juce::File& file)
    {
        if (engine.getSampleLibrary().load (file.getFullPathName()) == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Sample"), tr ("Could not read {0}", { file.getFileName() }));
            return;
        }
        const auto path = file.getFullPathName().toStdString();
        sample = nullptr;
        samplePath.clear();
        selected.clear();
        engine.editSong ([this, path] (Song& song) {
            if (auto* fx = slotIn (song))
            {
                fx->samplePath = path;
                fx->state.clear(); // (a new sample: its own notes / no markers)
            }
        });
        reloadFromSong();
    }

    void togglePreview()
    {
        if (previewing)
        {
            previewing = false;
            engine.previewSample (nullptr);
        }
        else
            startPreview();
        updateButtons();
    }

    void startPreview()
    {
        const auto* fx = currentSlot();
        if (fx == nullptr || sample == nullptr)
            return;
        previewing = true;
        engine.previewSample (newtonefx::renderedSource (sample, savedState), false, 0.8f);
    }

    void saveWav()
    {
        if (sample == nullptr)
            return;
        const juce::File src (juce::String::fromUTF8 (samplePath.c_str()));
        chooser = std::make_unique<juce::FileChooser> (tr ("Save the edited audio"), src.getSiblingFile (src.getFileNameWithoutExtension() + " (edited).wav"), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe = juce::Component::SafePointer<NewtoneEditor> (this)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  const auto out = newtonefx::renderedSource (safe->sample, safe->savedState);
                                  const auto file = fc.getResult().withFileExtension ("wav");
                                  file.deleteFile();
                                  std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
                                  juce::WavAudioFormat wav;
                                  const auto channels = out->isStereo() ? 2 : 1;
                                  std::unique_ptr<juce::AudioFormatWriter> writer (
                                      stream != nullptr ? wav.createWriterFor (stream.get(), out->sampleRate, (unsigned int) channels, 24, {}, 0) : nullptr);
                                  if (writer == nullptr)
                                  {
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Save"), tr ("Could not write {0}", { file.getFullPathName() }));
                                      return;
                                  }
                                  stream.release();
                                  const float* data[2] { out->leftData(), out->isStereo() ? out->rightData() : out->leftData() };
                                  writer->writeFromFloatArrays (data, channels, (int) out->frames());
                              });
    }

    // FL's Create Score / Save as MIDI: the notes as they sound now
    std::vector<Note> scoreNotes (int channel) const
    {
        std::vector<Note> out;
        const auto bpm = std::max (1.0, engine.getSong().bpm);
        for (const auto& n : newtonefx::score (notes, sample->sampleRate))
        {
            Note note;
            note.channel = channel;
            note.start = (std::int64_t) std::llround (n.start * bpm / 60.0 * ppq);
            note.length = std::max<std::int64_t> (1, (std::int64_t) std::llround (n.length * bpm / 60.0 * ppq));
            note.key = n.key;
            note.velocity = n.velocity;
            out.push_back (note);
        }
        return out;
    }

    void showScoreMenu()
    {
        if (sample == nullptr || warpMode())
            return;
        juce::PopupMenu menu, channels;
        const auto& song = engine.getSong();
        for (int c = 0; c < (int) song.channels.size(); ++c)
            channels.addItem (juce::String (c + 1) + ". " + juce::String::fromUTF8 (song.channels[(std::size_t) c].name.c_str()), [this, c] {
                const auto add = scoreNotes (c);
                engine.editSong ([add] (Song& s) {
                    if (s.patterns.empty())
                        return;
                    auto& pattern = s.patterns[(std::size_t) std::clamp (s.currentPattern, 0, (int) s.patterns.size() - 1)];
                    pattern.notes.insert (pattern.notes.end(), add.begin(), add.end());
                    pattern.sortNotes();
                });
            });
        menu.addSubMenu (tr ("Send to the piano roll of"), channels, ! song.channels.empty() && ! song.patterns.empty());
        menu.addItem (tr ("Save as MIDI..."), [this] { saveMidi(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scoreButton));
    }

    void saveMidi()
    {
        const juce::File src (juce::String::fromUTF8 (samplePath.c_str()));
        chooser = std::make_unique<juce::FileChooser> (tr ("Save the notes as MIDI"), src.getSiblingFile (src.getFileNameWithoutExtension() + ".mid"), "*.mid");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe = juce::Component::SafePointer<NewtoneEditor> (this)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  Song song;
                                  song.bpm = safe->engine.getSong().bpm;
                                  song.channels.resize (1);
                                  song.channels[0].name = "Newtone";
                                  song.patterns.resize (1);
                                  song.patterns[0].notes = safe->scoreNotes (0);
                                  juce::String error;
                                  if (! MidiFileIo::savePattern (song, 0, fc.getResult().withFileExtension ("mid"), error))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Save"), error);
                              });
    }

    void showLengthMenu()
    {
        if (sample == nullptr)
            return;
        juce::PopupMenu menu;
        const auto bpm = engine.getSong().bpm;
        const auto seconds = (double) frames() / sample->sampleRate;
        // the whole sample stretched to a length (one marker at its end)
        auto setLength = [this] (double ratio) {
            warp.markers.clear();
            if (std::abs (ratio - 1.0) > 1e-6)
                warp.markers.push_back ({ frames() - 1, (std::int64_t) std::llround ((double) (frames() - 1) * ratio) });
            commit();
        };
        for (const auto ratio : { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 })
            menu.addItem (juce::String (ratio * 100.0, 0) + " %", [setLength, ratio] { setLength (ratio); });
        menu.addSeparator();
        // to whole bars of the song's tempo
        const auto bar = 4.0 * 60.0 / std::max (1.0, bpm);
        for (const auto bars : { 1, 2, 4, 8, 16 })
            menu.addItem (tr ("{0} bar(s) at {1} BPM", { juce::String (bars), juce::String (bpm, 2) }),
                          [setLength, ratio = bars * bar / std::max (1e-6, seconds)] { setLength (ratio); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&stretch));
    }

    void showNoteMenu (int k)
    {
        juce::PopupMenu menu;
        auto& p = notes.pitches[(std::size_t) k];
        menu.addItem (tr ("Snap to the nearest note"), [this] {
            for (const auto q : selection())
                snapNote (q);
            commit();
        });
        menu.addItem (tr ("Reset this note"), [this] {
            for (const auto q : selection())
            {
                auto& n = notes.pitches[(std::size_t) q];
                n.shift = 0.0;
                n.curStart = n.origStart;
                n.curEnd = n.origEnd;
                n.enabled = true;
                n.volume = n.formant = n.drift = 1.0;
                n.vibPeriod = n.vibAmpStart = n.vibAmpEnd = n.tremAmp = n.rampIn = n.rampOut = 0.0;
                n.smoothIn = n.origSmoothIn;
                n.smoothOut = n.origSmoothOut;
            }
            commit();
        });
        menu.addItem (p.enabled ? tr ("Mute") : tr ("Unmute"), [this, on = ! p.enabled] {
            for (const auto q : selection())
                notes.pitches[(std::size_t) q].enabled = on;
            commit();
        });
        menu.addItem (tr ("Slice here"), ! warpMode(), false, [this, k, x = getMouseXYRelative().toFloat().x] { cutAt (k, x); });
        menu.addItem (tr ("Glue to the note before"), k > 0, false, [this, k] {
            if (newtonefx::glueNote (notes, (std::size_t) k))
            {
                selected = { k - 1 };
                commit();
            }
        });
        juce::PopupMenu volume;
        for (const auto db : { 6.0, 3.0, 0.0, -3.0, -6.0, -12.0 })
            volume.addItem ((db > 0 ? "+" : "") + juce::String (db, 0) + " dB", true, std::abs (20.0 * std::log10 (std::max (1e-6, p.volume)) - db) < 0.1, [this, db] {
                for (const auto q : selection())
                    notes.pitches[(std::size_t) q].volume = std::pow (10.0, db / 20.0);
                commit();
            });
        menu.addSubMenu (tr ("Volume"), volume);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void updateButtons()
    {
        const auto has = sample != nullptr && validSlot();
        analyse.setVisible (! warpMode());
        cutMode.setVisible (! warpMode());
        snapAll.setVisible (! warpMode());
        scoreButton.setVisible (! warpMode());
        stretch.setVisible (warpMode());
        for (auto* b : { &analyse, &cutMode, &snapAll, &reset, &preview, &save, &scoreButton, &stretch })
            b->setEnabled (has);
        load.setEnabled (validSlot());
        slave.setEnabled (validSlot());
        preview.setButtonText (previewing ? tr ("Stop") : tr ("Preview"));
        juce::String text = warpMode() ? tr ("(beta) LYNX's own time engine; FL's own warping is not reproduced exactly") : juce::String();
        if (sample != nullptr)
        {
            const juce::File f (juce::String::fromUTF8 (samplePath.c_str()));
            text = (warpMode() ? text + "  ·  " : juce::String()) + f.getFileName() + "  ·  " + juce::String ((double) frames() / sample->sampleRate, 2) + " s  ·  ";
            text += warpMode() ? tr ("{0} warp markers. Double-click: add a marker, drag it to move that point, right-click: remove", { juce::String ((int) warp.markers.size()) })
                    : cutMode.getToggleState()
                        ? tr ("{0} notes. Cut mode: click a note to slice it, Ctrl+click to glue it to the one before (C: leave)", { juce::String ((int) notes.pitches.size()) })
                        : tr ("{0} notes. Drag: pitch (Alt: free), Ctrl+drag: time, Shift+click / drag on empty space: select, ruler: set pitch, right-click: menu",
                              { juce::String ((int) notes.pitches.size()) });
        }
        status.setText (text, juce::dontSendNotification);
        updatePanel();
        resized();
    }

    void timerCallback() override
    {
        if (previewing)
        {
            if (engine.getPreviewPosition() < 0.0 && ++silentTicks > 15) // the preview ended
            {
                previewing = false;
                updateButtons();
            }
            else if (engine.getPreviewPosition() >= 0.0)
                silentTicks = 0;
            repaint (canvasArea());
        }
    }
    int silentTicks = 0;

    // -------------------------------------------------------------------------------------------------- geometry
    juce::Rectangle<int> contentArea() const
    {
        return getLocalBounds().reduced (8).withTrimmedTop (warpMode() ? 30 : 58).withTrimmedBottom (22);
    }
    juce::Rectangle<int> panelArea() const
    {
        auto r = contentArea();
        return warpMode() ? juce::Rectangle<int>() : r.removeFromRight (250);
    }
    juce::Rectangle<int> rulerArea() const
    {
        auto r = contentArea();
        if (warpMode())
            return {};
        r.removeFromRight (256);
        return r.removeFromLeft (36);
    }
    juce::Rectangle<int> canvasArea() const
    {
        auto r = contentArea();
        if (! warpMode())
        {
            r.removeFromRight (256);
            r.removeFromLeft (38);
        }
        return r;
    }

    double totalLength() const
    {
        if (warpMode())
            return (double) std::max<std::int64_t> (1, std::max (newtonefx::warpedLength (warp, frames()), frames()));
        std::int64_t end = frames();
        for (const auto& p : notes.pitches)
            end = std::max (end, p.curEnd);
        return (double) std::max<std::int64_t> (1, end);
    }
    double visibleLength() const { return totalLength() / zoom; }
    double pixelsPerSample() const { return (double) canvasArea().getWidth() / visibleLength(); }
    void clampScroll() { scroll = std::clamp (scroll, 0.0, std::max (0.0, totalLength() - visibleLength())); }
    float xOfOutput (double s) const { return (float) canvasArea().getX() + (float) ((s - scroll) * pixelsPerSample()); }
    double outputOfX (float x) const { return scroll + (x - (float) canvasArea().getX()) / pixelsPerSample(); }

    std::pair<double, double> pitchRange() const
    {
        double lo = 1e9, hi = -1e9;
        for (const auto& p : notes.pitches)
            if (p.origPitch > 0.0)
            {
                lo = std::min (lo, p.origPitch + p.shift);
                hi = std::max (hi, p.origPitch + p.shift);
            }
        if (lo > hi)
            lo = 55.0, hi = 70.0;
        return { std::floor (lo) - 4.0, std::ceil (hi) + 4.0 };
    }
    double rowHeight() const
    {
        const auto [lo, hi] = pitchRange();
        return (double) canvasArea().getHeight() / std::max (1.0, hi - lo);
    }
    float yOfPitch (double midi) const
    {
        const auto [lo, hi] = pitchRange();
        const auto area = canvasArea();
        return (float) area.getBottom() - (float) ((midi - lo) * (double) area.getHeight() / std::max (1.0, hi - lo));
    }
    double pitchOfY (float y) const
    {
        const auto [lo, hi] = pitchRange();
        const auto area = canvasArea();
        return lo + (double) ((float) area.getBottom() - y) * std::max (1.0, hi - lo) / (double) std::max (1, area.getHeight());
    }

    juce::Rectangle<float> noteBounds (int k) const
    {
        const auto& p = notes.pitches[(std::size_t) k];
        const auto h = (float) std::max (6.0, rowHeight() * 0.8);
        const auto x0 = xOfOutput ((double) p.curStart), x1 = xOfOutput ((double) p.curEnd);
        return { x0, yOfPitch (p.origPitch + p.shift) - 0.5f * h, std::max (2.0f, x1 - x0), h };
    }

    int noteAt (juce::Point<float> pos) const
    {
        const auto h = (float) rowHeight();
        for (int k = (int) notes.pitches.size() - 1; k >= 0; --k)
        {
            const auto& p = notes.pitches[(std::size_t) k];
            if (p.origPitch <= 0.0)
                continue;
            const auto y = yOfPitch (p.origPitch + p.shift);
            if (pos.x >= xOfOutput ((double) p.curStart) && pos.x <= xOfOutput ((double) p.curEnd) && std::abs (pos.y - y) <= std::max (5.0f, 0.5f * h))
                return k;
        }
        return -1;
    }

    int markerAt (float x) const
    {
        for (int k = 0; k < (int) warp.markers.size(); ++k)
            if (std::abs (xOfOutput ((double) warp.markers[(std::size_t) k].output) - x) <= 5.0f)
                return k;
        return -1;
    }

    // -------------------------------------------------------------------------------------------------- painting
    void paintWave (juce::Graphics& g, juce::Rectangle<int> area, std::function<double (double)> sourceOfOutput, juce::Colour colour) const
    {
        if (peaks.empty())
            return;
        g.setColour (colour);
        const auto mid = (float) area.getCentreY(), half = 0.48f * (float) area.getHeight();
        for (int x = area.getX(); x < area.getRight(); ++x)
        {
            const auto a = sourceOfOutput (outputOfX ((float) x)), b = sourceOfOutput (outputOfX ((float) x + 1.0f));
            if (b < 0.0 || a >= (double) frames())
                continue;
            float m = 0.0f;
            for (auto i = (std::int64_t) std::max (0.0, std::min (a, b)) / 256; i <= (std::int64_t) std::max (a, b) / 256 && i < (std::int64_t) peaks.size(); ++i)
                m = std::max (m, peaks[(std::size_t) i]);
            g.drawVerticalLine (x, mid - m * half, mid + m * half);
        }
    }

    // the keyboard ruler: click a key to move the selected notes to it
    void paintRuler (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area);
        const auto [lo, hi] = pitchRange();
        g.setFont (theme::font (theme::type::caption));
        for (auto m = (int) lo; m < (int) hi; ++m)
        {
            const auto y0 = yOfPitch (m + 0.5), y1 = yOfPitch (m - 0.5);
            const auto cls = ((m % 12) + 12) % 12;
            const auto black = juce::String ("010100101010").substring (cls, cls + 1) == "1";
            const auto inScale = (scaleMask() >> (((m - scaleKey()) % 12 + 12) % 12) & 1u) != 0;
            g.setColour (black ? theme::colour::bg3 : theme::colour::textSecondary.withAlpha (inScale ? 0.85f : 0.4f));
            g.fillRect ((float) area.getX(), y0 + 0.5f, (float) area.getWidth() * (black ? 0.62f : 1.0f), y1 - y0 - 1.0f);
            if (cls == 0 && y1 - y0 >= 8.0f)
            {
                g.setColour (theme::colour::bg0);
                g.drawText (noteName (m), area.getX(), (int) y0, area.getWidth() - 2, (int) (y1 - y0), juce::Justification::centredRight);
            }
        }
    }

    void paintNotes (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area);
        const auto [lo, hi] = pitchRange();
        // semitone rows (black keys darker; notes outside the scale dimmer) with C labels
        g.setFont (theme::font (theme::type::caption));
        for (auto m = (int) lo; m < (int) hi; ++m)
        {
            const auto y0 = yOfPitch (m + 0.5), y1 = yOfPitch (m - 0.5);
            const auto black = juce::String ("010100101010").substring (((m % 12) + 12) % 12, ((m % 12) + 12) % 12 + 1) == "1";
            const auto inScale = (scaleMask() >> (((m - scaleKey()) % 12 + 12) % 12) & 1u) != 0;
            g.setColour ((black ? theme::colour::bg0.darker (0.2f) : theme::colour::bg0.brighter (0.03f)).darker (inScale ? 0.0f : 0.25f));
            g.fillRect ((float) area.getX(), y0, (float) area.getWidth(), y1 - y0);
        }
        // the waveform, faint, at the bottom
        paintWave (g, area.withTrimmedTop (area.getHeight() * 3 / 4), [] (double s) { return s; }, theme::colour::textDisabled.withAlpha (0.35f));

        const auto h = (float) std::max (6.0, rowHeight() * 0.8);
        for (int k = 0; k < (int) notes.pitches.size(); ++k)
        {
            const auto& p = notes.pitches[(std::size_t) k];
            if (p.origPitch <= 0.0)
                continue; // (FL's unvoiced stretches between the notes)
            const auto x0 = xOfOutput ((double) p.curStart), x1 = xOfOutput ((double) p.curEnd);
            if (x1 < (float) area.getX() || x0 > (float) area.getRight())
                continue;
            const auto target = p.origPitch + p.shift;
            const auto y = yOfPitch (target);
            const auto edited = p.edited (notes.globalFormant);
            const auto isSelected = selected.count (k) > 0;
            auto colour = ! p.enabled ? theme::colour::bg5 : edited ? theme::colour::accent : theme::colour::info;
            g.setColour (colour.withAlpha (isSelected ? 0.75f : 0.45f));
            g.fillRoundedRectangle (x0, y - 0.5f * h, std::max (2.0f, x1 - x0), h, 3.0f);
            if (isSelected)
            {
                g.setColour (theme::colour::textPrimary);
                g.drawRoundedRectangle (x0, y - 0.5f * h, std::max (2.0f, x1 - x0), h, 3.0f, 1.2f);
            }
            // the sung pitch inside the note, moved with it
            if (! pitchTrack.empty())
            {
                juce::Path curve;
                bool started = false;
                for (auto f = std::max<std::int64_t> (0, p.origStart / pitchHop); f < std::min<std::int64_t> ((std::int64_t) pitchTrack.size(), p.origEnd / pitchHop + 1); ++f)
                {
                    const auto v = pitchTrack[(std::size_t) f];
                    if (v < 0.0 || std::abs (v - p.origPitch) > 1.5 || f * pitchHop < p.origStart || f * pitchHop >= p.origEnd) // (the note's own pitch only)
                    {
                        started = false;
                        continue;
                    }
                    const auto px = xOfOutput ((double) p.curStart + (double) (f * pitchHop - p.origStart) / std::max (1e-6, p.stretch));
                    const auto py = yOfPitch (p.origPitch + p.shift + p.drift * (v - p.origPitch));
                    if (! started)
                        curve.startNewSubPath (px, py);
                    else
                        curve.lineTo (px, py);
                    started = true;
                }
                g.setColour (theme::colour::textPrimary.withAlpha (p.enabled ? 0.8f : 0.3f));
                g.strokePath (curve, juce::PathStrokeType (1.2f));
            }
            if (x1 - x0 > 34.0f)
            {
                g.setColour (theme::colour::textPrimary);
                g.drawText (noteName (target) + (std::abs (p.shift) > 0.01 ? juce::String::formatted (" %+.1f", p.shift) : juce::String()),
                            juce::Rectangle<float> (x0 + 3.0f, y - 0.5f * h - 14.0f, std::max (40.0f, x1 - x0), 13.0f), juce::Justification::left);
            }
        }
    }

    void paintWarp (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        // the warped waveform (where each part of the sample plays now)
        paintWave (g, area, [this] (double out) { return newtonefx::warpToSource (warp, frames(), out); }, theme::colour::info.withAlpha (0.7f));
        // bars of the song's tempo
        const auto bpm = engine.getSong().bpm;
        const auto beat = sample->sampleRate * 60.0 / std::max (1.0, bpm);
        g.setColour (theme::colour::border);
        for (double s = 0.0; s < totalLength(); s += beat)
        {
            const auto x = xOfOutput (s);
            if (x >= (float) area.getX() && x <= (float) area.getRight())
                g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getY() + 8.0f);
        }
        // the end of the original length, and the markers
        g.setColour (theme::colour::textDisabled);
        g.drawVerticalLine ((int) xOfOutput ((double) frames()), (float) area.getY(), (float) area.getBottom());
        for (const auto& m : warp.markers)
        {
            const auto x = xOfOutput ((double) m.output);
            g.setColour (theme::colour::warning);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
            juce::Path tri;
            tri.addTriangle (x - 5.0f, (float) area.getY(), x + 5.0f, (float) area.getY(), x, (float) area.getY() + 8.0f);
            g.fillPath (tri);
        }
    }
};

class NewtoneWindow final : public juce::DocumentWindow
{
public:
    NewtoneWindow (AudioEngine& engine, int track, int slot, const juce::String& title)
        : DocumentWindow (title, theme::colour::bg2, DocumentWindow::closeButton),
          gone (engine, track, slot, [this] { closeButtonPressed(); })
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new NewtoneEditor (engine, track, slot), true);
        setResizable (true, false);
        setResizeLimits (760, 420, 4000, 2000);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
    }

    void closeButtonPressed() override;

private:
    ui::SlotGoneWatcher gone;
};

std::map<std::pair<int, int>, std::unique_ptr<NewtoneWindow>>& newtoneWindows()
{
    static std::map<std::pair<int, int>, std::unique_ptr<NewtoneWindow>> open;
    return open;
}

void NewtoneWindow::closeButtonPressed()
{
    for (auto& [key, window] : newtoneWindows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { newtoneWindows().erase (k); });
            return;
        }
}
} // namespace

namespace NewtoneWindows
{
bool handles (EffectKind kind)
{
    return isNewtoneKind (kind);
}

void open (AudioEngine& engine, int track, int slot)
{
    auto& open = newtoneWindows();
    const auto key = std::make_pair (track, slot);
    if (auto it = open.find (key); it != open.end())
    {
        it->second->toFront (true);
        return;
    }
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;
    const auto kind = mixer[(std::size_t) track].effects[(std::size_t) slot].kind;
    const auto name = track == 0 ? tr ("Master") : juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str());
    open[key] = std::make_unique<NewtoneWindow> (engine, track, slot,
                                                 utf8 (effectInfo (kind).name) + " - " + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) }))
                                                     + " (" + tr ("slot {0}", { juce::String (slot + 1) }) + ")");
}

void closeAll()
{
    newtoneWindows().clear();
}
} // namespace NewtoneWindows

std::unique_ptr<juce::Component> makeNewtoneEditor (AudioEngine& engine, int track, int slot)
{
    return std::make_unique<NewtoneEditor> (engine, track, slot);
}
} // namespace lynx
