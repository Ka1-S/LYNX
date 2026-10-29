#include "ui/SlotWatch.h"
#include "ui/NewtoneEditor.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/NewtoneFx.h"
#include "core/VocalPitch.h"
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

class NewtoneEditor final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    NewtoneEditor (AudioEngine& e, int t, int s) : engine (e), track (t), slot (s)
    {
        for (auto* b : { &load, &analyse, &snapAll, &reset, &preview, &save, &stretch })
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
        snapAll.setButtonText (tr ("Snap all to notes"));
        reset.setButtonText (tr ("Reset edits"));
        preview.setButtonText (tr ("Preview"));
        save.setButtonText (tr ("Save as WAV..."));
        stretch.setButtonText (tr ("Length..."));
        slave.setButtonText (tr ("Play with the song"));
        slave.setTooltip (tr ("Slave to host playback: while the song plays, the edited sample plays from the song's start"));
        load.setTooltip (tr ("Choose the audio file to edit (WAV, AIFF, FLAC, OGG, MP3)"));
        analyse.setTooltip (tr ("Find the notes of the sample again (discards the note edits)"));

        load.onClick = [this] { chooseSample(); };
        analyse.onClick = [this] { reanalyse(); };
        snapAll.onClick = [this] {
            for (auto& p : notes.pitches)
                p.shift = std::round (p.origPitch + p.shift) - p.origPitch;
            commit();
        };
        reset.onClick = [this] {
            if (warpMode())
                warp.markers.clear();
            else
                for (auto& p : notes.pitches)
                {
                    const auto origPitch = p.origPitch;
                    const auto a = p.origStart, b = p.origEnd;
                    const auto si = p.smoothIn, so = p.smoothOut;
                    p = {};
                    p.origPitch = origPitch;
                    p.origStart = p.curStart = a;
                    p.origEnd = p.curEnd = b;
                    p.smoothIn = si;
                    p.smoothOut = so;
                }
            commit();
        };
        preview.onClick = [this] { togglePreview(); };
        save.onClick = [this] { saveWav(); };
        stretch.onClick = [this] { showLengthMenu(); };
        slave.onClick = [this] {
            const auto on = slave.getToggleState();
            engine.editSong ([this, on] (Song& song) {
                if (auto* fx = slotIn (song))
                    fx->params[0] = on ? 1.0f : 0.0f;
            });
        };

        setWantsKeyboardFocus (true);
        engine.addChangeListener (this);
        reloadFromSong();
        startTimerHz (30);
        setSize (900, 460);
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
            paintNotes (g, area);

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
        for (auto* b : { &load, &analyse, &snapAll, &stretch, &reset, &preview, &save })
            if (b->isVisible())
            {
                b->setBounds (top.removeFromLeft (juce::jmax (70, b->getBestWidthForHeight (26) + 16)));
                top.removeFromLeft (4);
            }
        slave.setBounds (top.removeFromRight (170));
        r.removeFromTop (4);
        status.setBounds (r.removeFromBottom (18));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (warpMode() || selected < 0 || selected >= (int) notes.pitches.size())
            return false;
        auto& p = notes.pitches[(std::size_t) selected];
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            p.shift += key == juce::KeyPress::upKey ? 1.0 : -1.0;
            commit();
            return true;
        }
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            p.enabled = ! p.enabled;
            commit();
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (sample == nullptr || ! canvasArea().contains (e.getPosition()))
            return;
        drag = {};
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

        selected = noteAt (e.position);
        repaint();
        if (selected < 0)
            return;
        if (e.mods.isPopupMenu())
        {
            showNoteMenu (selected);
            return;
        }
        const auto& p = notes.pitches[(std::size_t) selected];
        drag.note = selected;
        drag.shift = p.shift;
        drag.start = p.curStart;
        drag.end = p.curEnd;
        drag.time = e.mods.isCtrlDown() || e.mods.isCommandDown();
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
        if (drag.note < 0 || drag.note >= (int) notes.pitches.size())
            return;
        auto& p = notes.pitches[(std::size_t) drag.note];
        if (drag.time)
        {
            const auto dx = (std::int64_t) std::llround ((double) e.getDistanceFromDragStartX() / pixelsPerSample());
            const auto len = drag.end - drag.start;
            p.curStart = std::clamp<std::int64_t> (drag.start + dx, 0, std::max<std::int64_t> (0, frames() - len));
            p.curEnd = p.curStart + len;
        }
        else
        {
            auto s = drag.shift - (double) e.getDistanceFromDragStartY() / rowHeight();
            if (! e.mods.isAltDown()) // snapped: the note lands on a semitone
                s = std::round (p.origPitch + s) - p.origPitch;
            p.shift = std::clamp (s, -24.0, 24.0);
        }
        drag.moved = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag.moved)
            commit();
        drag = {};
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
        if (const auto k = noteAt (e.position); k >= 0)
        {
            auto& p = notes.pitches[(std::size_t) k];
            p.shift = std::round (p.origPitch + p.shift) - p.origPitch;
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
        int note = -1, marker = -1;
        double shift = 0.0;
        std::int64_t start = 0, end = 0;
        bool time = false, moved = false;
    };

    AudioEngine& engine;
    int track, slot;
    juce::TextButton load, analyse, snapAll, reset, preview, save, stretch;
    juce::ToggleButton slave;
    juce::Label status;
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
    int selected = -1;
    Drag drag;
    double zoom = 1.0, scroll = 0.0;       // horizontal view: zoom over the whole length, first visible sample
    bool previewing = false;

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
        selected = std::min (selected, (int) notes.pitches.size() - 1);
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
        repaint();
    }

    void reanalyse()
    {
        if (sample == nullptr || warpMode())
            return;
        notes = newtonefx::analyseNotes (*sample);
        selected = -1;
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
        menu.addItem (tr ("Snap to the nearest note"), [this, k] {
            auto& q = notes.pitches[(std::size_t) k];
            q.shift = std::round (q.origPitch + q.shift) - q.origPitch;
            commit();
        });
        menu.addItem (tr ("Reset this note"), [this, k] {
            auto& q = notes.pitches[(std::size_t) k];
            q.shift = 0.0;
            q.curStart = q.origStart;
            q.curEnd = q.origEnd;
            q.enabled = true;
            q.volume = 1.0;
            commit();
        });
        menu.addItem (p.enabled ? tr ("Mute") : tr ("Unmute"), [this, k] {
            notes.pitches[(std::size_t) k].enabled = ! notes.pitches[(std::size_t) k].enabled;
            commit();
        });
        juce::PopupMenu volume;
        for (const auto db : { 6.0, 3.0, 0.0, -3.0, -6.0, -12.0 })
            volume.addItem ((db > 0 ? "+" : "") + juce::String (db, 0) + " dB", true, std::abs (20.0 * std::log10 (std::max (1e-6, p.volume)) - db) < 0.1, [this, k, db] {
                notes.pitches[(std::size_t) k].volume = std::pow (10.0, db / 20.0);
                commit();
            });
        menu.addSubMenu (tr ("Volume"), volume);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void updateButtons()
    {
        const auto has = sample != nullptr && validSlot();
        analyse.setVisible (! warpMode());
        snapAll.setVisible (! warpMode());
        stretch.setVisible (warpMode());
        for (auto* b : { &analyse, &snapAll, &reset, &preview, &save, &stretch })
            b->setEnabled (has);
        load.setEnabled (validSlot());
        slave.setEnabled (validSlot());
        preview.setButtonText (previewing ? tr ("Stop") : tr ("Preview"));
        juce::String text = tr ("(beta) LYNX SHARE's own analysis and pitch / time engines; FL's own edits are not reproduced exactly");
        if (sample != nullptr)
        {
            const juce::File f (juce::String::fromUTF8 (samplePath.c_str()));
            text = f.getFileName() + "  ·  " + juce::String ((double) frames() / sample->sampleRate, 2) + " s  ·  ";
            text += warpMode() ? tr ("{0} warp markers. Double-click: add a marker, drag it to move that point, right-click: remove", { juce::String ((int) warp.markers.size()) })
                               : tr ("{0} notes. Drag: pitch (Alt: free), Ctrl+drag: time, double-click: snap, right-click: menu", { juce::String ((int) notes.pitches.size()) });
        }
        status.setText (text, juce::dontSendNotification);
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
    juce::Rectangle<int> canvasArea() const { return getLocalBounds().reduced (8).withTrimmedTop (30).withTrimmedBottom (22); }

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

    int noteAt (juce::Point<float> pos) const
    {
        const auto h = (float) rowHeight();
        for (int k = (int) notes.pitches.size() - 1; k >= 0; --k)
        {
            const auto& p = notes.pitches[(std::size_t) k];
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

    void paintNotes (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        const auto [lo, hi] = pitchRange();
        // semitone rows (black keys darker) with C labels
        g.setFont (theme::font (theme::type::caption));
        for (auto m = (int) lo; m < (int) hi; ++m)
        {
            const auto y0 = yOfPitch (m + 0.5), y1 = yOfPitch (m - 0.5);
            const auto black = juce::String ("010100101010").substring (((m % 12) + 12) % 12, ((m % 12) + 12) % 12 + 1) == "1";
            g.setColour (black ? theme::colour::bg0.darker (0.2f) : theme::colour::bg0.brighter (0.03f));
            g.fillRect ((float) area.getX(), y0, (float) area.getWidth(), y1 - y0);
            if (((m % 12) + 12) % 12 == 0)
            {
                g.setColour (theme::colour::textDisabled);
                g.drawText (noteName (m), area.getX() + 2, (int) y0, 30, (int) (y1 - y0), juce::Justification::centredLeft);
            }
        }
        // the waveform, faint, at the bottom
        paintWave (g, area.withTrimmedTop (area.getHeight() * 3 / 4), [] (double s) { return s; }, theme::colour::textDisabled.withAlpha (0.35f));

        const auto h = (float) std::max (6.0, rowHeight() * 0.8);
        for (int k = 0; k < (int) notes.pitches.size(); ++k)
        {
            const auto& p = notes.pitches[(std::size_t) k];
            const auto x0 = xOfOutput ((double) p.curStart), x1 = xOfOutput ((double) p.curEnd);
            if (x1 < (float) area.getX() || x0 > (float) area.getRight())
                continue;
            const auto target = p.origPitch + p.shift;
            const auto y = yOfPitch (target);
            const auto edited = p.edited (notes.globalFormant);
            auto colour = ! p.enabled ? theme::colour::bg5 : edited ? theme::colour::accent : theme::colour::info;
            g.setColour (colour.withAlpha (k == selected ? 0.75f : 0.45f));
            g.fillRoundedRectangle (x0, y - 0.5f * h, std::max (2.0f, x1 - x0), h, 3.0f);
            if (k == selected)
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
                    const auto px = xOfOutput ((double) (p.curStart + f * pitchHop - p.origStart));
                    const auto py = yOfPitch (v + p.shift);
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
        setResizeLimits (560, 300, 4000, 2000);
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
