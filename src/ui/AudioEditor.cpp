#include "ui/AudioEditor.h"
#include "ui/BrowserComponent.h"
#include "ui/EditorTools.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/AudioCapture.h"
#include "core/WaveAnalysis.h"
#include "core/ValueMath.h"
#include "engine/Text.h"
#include "ui/HintBar.h"
#include "ui/ValueControl.h"
#include "ui/I18n.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
namespace ae = audioedit;

// one clipboard for every editor window (copy in one, paste in another)
ae::Audio& clipboard()
{
    static ae::Audio audio;
    return audio;
}

// the clean clip look (docs/analise/UI_STUDY_playlist_clips.md): one light filled shape in a tint of the audio colour
juce::Colour waveColour() { return theme::colour::audioClip.interpolatedWith (juce::Colours::white, 0.7f); }

enum class TimeFormat
{
    automatic,
    samples,
    seconds,
    bars
};

enum class RecordMode
{
    now,     // starts at once
    onInput, // starts when the input goes above the threshold
    onPlay   // starts with the song playback, stops with it
};

constexpr float inputThreshold = 0.01f; // -40 dBFS

juce::String formatSeconds (double s)
{
    const auto negative = s < 0.0;
    s = std::abs (s);
    const auto minutes = (int) (s / 60.0);
    const auto rest = s - minutes * 60.0;
    return juce::String (negative ? "-" : "") + juce::String (minutes) + ":" + juce::String (rest, 3).paddedLeft ('0', 6);
}

// Spectrogram colours (LYNX's own map): the canvas, a deep red, the accent, amber, then white for the loudest.
juce::Colour spectrumColour (float db)
{
    const auto t = juce::jlimit (0.0f, 1.0f, (db + 96.0f) / 96.0f);
    static const juce::Colour stops[] = { theme::colour::bg0, juce::Colour (0xff3a1016), theme::colour::accent, theme::colour::warning,
                                          juce::Colour (0xfffff4dc) };
    static const float at[] = { 0.0f, 0.35f, 0.62f, 0.85f, 1.0f };
    for (int i = 1; i < 5; ++i)
        if (t <= at[i])
            return stops[i - 1].interpolatedWith (stops[i], (t - at[i - 1]) / (at[i] - at[i - 1]));
    return stops[4];
}

// --- a small parameter window for the tools (sliders + an optional choice; Preview / Accept / Cancel) ------------
class ToolPanel final : public juce::Component
{
public:
    struct Param
    {
        juce::String name;
        double min = 0.0, max = 1.0, value = 0.0, step = 0.0;
        juce::String suffix;
    };
    using Action = std::function<void (const std::vector<double>&, int)>;

    ToolPanel (std::vector<Param> ps, juce::StringArray choices, const juce::String& choiceName, int choice, Action applyAction, Action previewAction)
        : params (std::move (ps)), apply (std::move (applyAction)), preview (std::move (previewAction))
    {
        for (const auto& p : params)
        {
            auto* label = labels.add (new juce::Label ({}, p.name));
            label->setFont (theme::font (theme::type::body));
            label->setColour (juce::Label::textColourId, theme::colour::textSecondary);
            addAndMakeVisible (label);

            auto* s = sliders.add (new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight));
            s->setRange (p.min, p.max, p.step);
            s->setValue (p.value, juce::dontSendNotification);
            s->setDoubleClickReturnValue (true, p.value);
            s->setTextValueSuffix (p.suffix);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 84, 22);
            addAndMakeVisible (s);
        }

        if (! choices.isEmpty())
        {
            choiceLabel.setText (choiceName, juce::dontSendNotification);
            choiceLabel.setFont (theme::font (theme::type::body));
            choiceLabel.setColour (juce::Label::textColourId, theme::colour::textSecondary);
            addAndMakeVisible (choiceLabel);
            choiceBox.addItemList (choices, 1);
            choiceBox.setSelectedItemIndex (choice, juce::dontSendNotification);
            addAndMakeVisible (choiceBox);
        }

        previewButton.setButtonText (tr ("Preview"));
        acceptButton.setButtonText (tr ("Accept"));
        cancelButton.setButtonText (tr ("Cancel"));
        acceptButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        previewButton.onClick = [this] {
            if (preview)
                preview (values(), choiceBox.getSelectedItemIndex());
        };
        acceptButton.onClick = [this] {
            if (apply)
                apply (values(), choiceBox.getSelectedItemIndex());
            closeToolWindowOf (*this);
        };
        cancelButton.onClick = [this] { closeToolWindowOf (*this); };
        previewButton.setVisible (preview != nullptr);
        for (auto* b : { &previewButton, &acceptButton, &cancelButton })
            addAndMakeVisible (*b);

        setSize (420, 24 + (int) params.size() * 36 + (choices.isEmpty() ? 0 : 36) + 52);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (theme::space::lg);
        if (choiceBox.getNumItems() > 0)
        {
            auto row = area.removeFromTop (28);
            choiceLabel.setBounds (row.removeFromLeft (120));
            choiceBox.setBounds (row);
            area.removeFromTop (8);
        }
        for (int i = 0; i < sliders.size(); ++i)
        {
            auto row = area.removeFromTop (28);
            labels[i]->setBounds (row.removeFromLeft (120));
            sliders[i]->setBounds (row);
            area.removeFromTop (8);
        }
        auto buttons = area.removeFromBottom (30);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        acceptButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        previewButton.setBounds (buttons.removeFromRight (90));
    }

private:
    std::vector<double> values() const
    {
        std::vector<double> v;
        for (auto* s : sliders)
            v.push_back (s->getValue());
        return v;
    }

    std::vector<Param> params;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Slider> sliders;
    juce::Label choiceLabel;
    juce::ComboBox choiceBox;
    juce::TextButton previewButton, acceptButton, cancelButton;
    Action apply, preview;
};

// --- the editor --------------------------------------------------------------------------------------------------
struct Source
{
    enum class Kind
    {
        none,
        file,
        channel,
        insert
    };
    Kind kind = Kind::none;
    int index = -1; // channel or insert
    juce::String path;
};

class AudioEditor final : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          public juce::DragAndDropTarget,
                          private juce::Timer
{
public:
    AudioEditor (AudioEngine& e, Source s) : engine (e), source (std::move (s))
    {
        auto setupIcon = [this] (IconButton& b, const juce::String& tip) {
            b.setFlat (true);
            b.setTooltip (tip);
            addAndMakeVisible (b);
        };
        setupIcon (menuButton, tr ("Menu: file, edit, tools, regions, view"));
        setupIcon (undoButton, tr ("Undo") + " (Ctrl+Z)");
        setupIcon (redoButton, tr ("Redo") + " (Ctrl+Y)");
        setupIcon (historyButton, tr ("Undo history"));
        setupIcon (recordButton, tr ("Record") + " (R)");
        setupIcon (recordMenuButton, tr ("What and when to record"));
        setupIcon (playButton, tr ("Play / stop the selection") + " (Space)");
        setupIcon (stopButton, tr ("Stop"));
        setupIcon (snapButton, tr ("Snap the selection to zero crossings"));
        setupIcon (fitButton, tr ("Zoom out full"));
        setupIcon (saveButton, tr ("Save as WAV..."));
        // Follow (issue #12): the view pages along with the playback position; off, it stays where you look
        setupIcon (followButton, tr ("Follow: the view pages along with the playback position; off, it stays where you scrolled"));
        followButton.setClickingTogglesState (false);
        followButton.setOnColour (theme::colour::bg6);
        followButton.setIconColourWhenOn (theme::colour::accent);
        followButton.setToggleState (EditorGlobals::get().isFollowingView (EditorGlobals::FollowView::audioEditor), juce::dontSendNotification);
        followButton.onClick = [this] {
            auto& g = EditorGlobals::get();
            g.setFollowingView (EditorGlobals::FollowView::audioEditor, ! g.isFollowingView (EditorGlobals::FollowView::audioEditor));
            followButton.setToggleState (g.isFollowingView (EditorGlobals::FollowView::audioEditor), juce::dontSendNotification);
        };
        recordButton.setOnColour (theme::colour::record);
        recordButton.setClickingTogglesState (false);
        snapButton.setClickingTogglesState (true);

        menuButton.onClick = [this] { mainMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton)); };
        undoButton.onClick = [this] { undo(); };
        redoButton.onClick = [this] { redo(); };
        historyButton.onClick = [this] { historyMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&historyButton)); };
        recordButton.onClick = [this] { toggleRecording(); };
        recordMenuButton.onClick = [this] { recordMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&recordMenuButton)); };
        playButton.onClick = [this] { playing ? stopPlayback() : startPlayback(); };
        stopButton.onClick = [this] {
            stopPlayback();
            if (recState != RecState::idle)
                stopRecording();
        };
        snapButton.onClick = [this] { snapZero = snapButton.getToggleState(); };
        fitButton.onClick = [this] { zoomToFit(); };
        saveButton.onClick = [this] { saveAs (bits); };

        auto setupText = [this] (juce::TextButton& b, const juce::String& text, const juce::String& tip) {
            b.setButtonText (text);
            b.setTooltip (tip);
            addAndMakeVisible (b);
        };
        setupText (loopButton, tr ("Loop"), tr ("Loop the playback") + " (L)");
        loopButton.setClickingTogglesState (true);
        loopButton.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
        loopButton.onClick = [this] {
            loopPlay = loopButton.getToggleState();
            if (playing)
                startPlayback();
        };
        // the playback volume (round 162, issue #9): how loud the editor plays, without changing the audio (the
        // Amp... tool changes the audio itself); remembered with the settings
        volumeKnob.setRange (0.0, controls::maxUtilityVolume, 0.01);
        volumeKnob.setDoubleClickReturnValue (true, 1.0);
        volumeKnob.textFromValueFunction = [] (double v) { return ui::utilityVolumeText (v); };
        volumeKnob.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue() / 100.0; };
        volumeKnob.setMouseClickGrabsKeyboardFocus (false);
        volumeKnob.setWantsKeyboardFocus (false);
        volumeKnob.setValue (engine.getEditorVolume(), juce::dontSendNotification);
        volumeKnob.onValueChange = [this] {
            engine.setEditorVolume (volumeKnob.getValue());
            refreshVolumeKnob();
        };
        addAndMakeVisible (volumeKnob);
        refreshVolumeKnob();
        setupText (viewButton, tr ("Spectrum"), tr ("Show the waveform or the spectrogram") + " (S)");
        viewButton.setClickingTogglesState (true);
        viewButton.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
        viewButton.onClick = [this] {
            spectrumView = viewButton.getToggleState();
            repaint();
        };
        setupText (sendButton, tr ("Send"), tr ("Send to the Playlist as an audio clip, to a new Sampler channel or back to the channel"));
        sendButton.setColour (juce::TextButton::buttonColourId, theme::colour::accentMuted);
        sendButton.onClick = [this] { sendMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sendButton)); };

        // quick tools (the second row)
        struct Quick
        {
            const char* text;
            const char* tip;
            std::function<void()> action;
        };
        const Quick quick[] = {
            { TR_NOOP ("Cut"), TR_NOOP ("Cut the selection (Ctrl+X)"), [this] { cutSelection(); } },
            { TR_NOOP ("Copy"), TR_NOOP ("Copy the selection (Ctrl+C)"), [this] { copySelection(); } },
            { TR_NOOP ("Paste"), TR_NOOP ("Paste at the cursor, or over the selection (Ctrl+V)"), [this] { paste (false); } },
            { TR_NOOP ("Delete"), TR_NOOP ("Delete the selection (Del)"), [this] { deleteSelection(); } },
            { TR_NOOP ("Trim"), TR_NOOP ("Keep only the selection"), [this] { trimSelection(); } },
            { TR_NOOP ("Silence"), TR_NOOP ("Silence the selection (Clear)"), [this] { edit (tr ("Silence"), [] (ae::Document& d, ae::Range r) { ae::clear (d, r); }, true); } },
            { TR_NOOP ("Fade in"), TR_NOOP ("Fade in over the selection (curve: the arrow next to Fade out)"), [this] { fade (true); } },
            { TR_NOOP ("Fade out"), TR_NOOP ("Fade out over the selection"), [this] { fade (false); } },
            { "~", TR_NOOP ("Fade curve"), [this] { curveMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (quickButtons[8])); } },
            { TR_NOOP ("Normalize"), TR_NOOP ("Normalize the selection (or everything) to 0 dB"), [this] { edit (tr ("Normalize"), [] (ae::Document& d, ae::Range r) { ae::normalize (d, r); }); } },
            { TR_NOOP ("Amp..."), TR_NOOP ("Volume and panning"), [this] { showAmp(); } },
            { TR_NOOP ("Reverse"), TR_NOOP ("Reverse the selection (or everything)"), [this] { edit (tr ("Reverse"), [] (ae::Document& d, ae::Range r) { ae::reverse (d, r); }); } },
            { TR_NOOP ("Invert"), TR_NOOP ("Reverse the polarity"), [this] { edit (tr ("Reverse polarity"), [] (ae::Document& d, ae::Range r) { ae::invert (d, r); }); } },
            { TR_NOOP ("Center"), TR_NOOP ("Remove the DC offset"), [this] { edit (tr ("Remove DC offset"), [] (ae::Document& d, ae::Range r) { ae::removeDc (d, r); }); } },
            { TR_NOOP ("Stretch..."), TR_NOOP ("Time stretch / pitch shift"), [this] { showStretch(); } },
            { TR_NOOP ("Resample..."), TR_NOOP ("Change the sample rate"), [this] { showResample(); } },
            { TR_NOOP ("Marker"), TR_NOOP ("Add a marker at the cursor (M)"), [this] { addMarkerAtCursor(); } },
            { TR_NOOP ("Region"), TR_NOOP ("Make the selection a region"), [this] { addRegionFromSelection(); } },
            { TR_NOOP ("Set loop"), TR_NOOP ("Make the selection the loop"), [this] { setLoopFromSelection(); } },
        };
        for (const auto& q : quick)
        {
            auto* b = quickButtons.add (new juce::TextButton (q.text[0] == '~' ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe")) : tr (q.text)));
            b->setTooltip (tr (q.tip));
            b->onClick = q.action;
            addAndMakeVisible (b);
        }

        setWantsKeyboardFocus (true);
        setSize (1120, 600);
        history.reset (doc, tr ("Empty").toStdString());
        updateButtons();
        startTimerHz (30);
    }

    ~AudioEditor() override
    {
        if (playing)
            engine.previewSample (nullptr);
        if (recState != RecState::idle)
            engine.getMeters().capture.setSource (AudioCapture::off);
        releaseInput();
    }

    // --- opening ---------------------------------------------------------------------------------------------
    bool loadFile (const juce::File& file)
    {
        auto data = engine.getSampleLibrary().load (file.getFullPathName());
        if (data == nullptr)
        {
            setStatus (tr ("Could not read {0}", { file.getFileName() }), true);
            return false;
        }

        ae::Document d;
        d.audio.sampleRate = data->sampleRate;
        d.audio.channels.emplace_back (data->leftData(), data->leftData() + data->frames());
        if (data->isStereo())
            d.audio.channels.emplace_back (data->rightData(), data->rightData() + data->frames());
        if (data->loopStart >= 0 && data->loopEnd > data->loopStart)
            ae::setLoop (d, { data->loopStart, data->loopEnd });

        if (file.hasFileExtension ("wav"))
        {
            juce::MemoryBlock bytes;
            if (file.loadFileAsData (bytes))
            {
                ae::Document marks = d;
                if (ae::decodeWavMarkers (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize(), marks))
                {
                    d.markers = marks.markers;
                    if (marks.hasLoop())
                        ae::setLoop (d, { marks.loopStart, marks.loopEnd });
                }
            }
        }

        d.title = file.getFileNameWithoutExtension().toStdString();
        setDocument (std::move (d), tr ("Open"));
        lastFile = file;
        return true;
    }

    void setDocument (ae::Document d, const juce::String& label)
    {
        doc = std::move (d);
        history.reset (doc, label.toStdString());
        selection = {};
        cursor = 0;
        modified = false;
        documentChanged();
        zoomToFit();
        updateTitle();
    }

    void selectForSnapshot (ae::Range r, bool spectrum)
    {
        selection = r;
        cursor = r.start;
        spectrumView = spectrum;
        viewButton.setToggleState (spectrum, juce::dontSendNotification);
        repaint();
    }

    juce::PopupMenu mainMenu();

    // --- layout and painting ---------------------------------------------------------------------------------
    void resized() override
    {
        auto area = getLocalBounds();
        auto bar = area.removeFromTop (36).reduced (6, 4);
        auto place = [&bar] (juce::Component& c, int w, int gap = 2) {
            c.setBounds (bar.removeFromLeft (w));
            bar.removeFromLeft (gap);
        };
        place (menuButton, 28, 8);
        place (undoButton, 28);
        place (redoButton, 28);
        place (historyButton, 18, 12);
        place (recordButton, 30);
        place (recordMenuButton, 18, 6);
        place (playButton, 30);
        place (stopButton, 30, 6);
        place (loopButton, 54, 4);
        place (volumeKnob, 26, 12);
        place (viewButton, 76, 6);
        place (snapButton, 28);
        place (fitButton, 28);
        place (followButton, 28);
        sendButton.setBounds (bar.removeFromRight (70));
        bar.removeFromRight (6);
        saveButton.setBounds (bar.removeFromRight (28));

        // each button gets the width its text needs, plus an equal share of what is left (round 165o: translated
        // labels such as "Normalisieren" or "Ausschneiden" are never cut). When one row cannot hold them all (German,
        // Russian, Polish… at the default width) the buttons go on two rows instead; English keeps its single row.
        const auto n = quickButtons.size();
        const auto curveWidth = 18;
        const juce::Font font (theme::font (theme::type::body));
        std::vector<float> natural;
        float total = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            natural.push_back (i == 8 ? (float) curveWidth : juce::GlyphArrangement::getStringWidth (font, quickButtons[i]->getButtonText()) + 18.0f);
            total += natural.back() + (i == 7 ? 0.0f : 2.0f);
        }
        const auto rowWidth = (float) (area.getWidth() - 12);
        const auto twoRows = total - 14.0f * (float) (n - 1) > rowWidth && n > 9; // not even with 4 px padding
        auto tools = area.removeFromTop (twoRows ? 56 : 30).reduced (6, 2);
        // lays out buttons [first, last) in one row: natural widths plus an equal share of the free space; still too
        // narrow -> the padding goes first (down to 4 px a side), then the text squeezes (drawFittedText)
        auto layoutRow = [&] (juce::Rectangle<int> row, int first, int last)
        {
            float need = 0.0f;
            int stretchable = 0;
            for (int i = first; i < last; ++i)
            {
                need += natural[(std::size_t) i] + (i == 7 || i == last - 1 ? 0.0f : 2.0f);
                stretchable += i == 8 ? 0 : 1;
            }
            const auto avail = (float) row.getWidth();
            const auto extra = juce::jmax (0.0f, avail - need) / (float) juce::jmax (1, stretchable);
            auto shrink = 0.0f, scale = 1.0f;
            if (need > avail)
            {
                const auto padding = 14.0f * (float) stretchable;
                shrink = 14.0f * juce::jlimit (0.0f, 1.0f, (need - avail) / padding);
                const auto after = need - shrink * (float) stretchable;
                scale = after > avail ? avail / after : 1.0f;
            }
            float x = (float) row.getX();
            for (int i = first; i < last; ++i)
            {
                const auto w = i == 8 ? (float) curveWidth : (natural[(std::size_t) i] - shrink) * scale + extra;
                quickButtons[i]->setBounds (juce::roundToInt (x), row.getY(), juce::roundToInt (x + w) - juce::roundToInt (x), row.getHeight());
                x += w + (i == 7 ? 0.0f : 2.0f);
            }
        };
        if (! twoRows)
            layoutRow (tools, 0, n);
        else
        {
            // split near half the total width, never between Fade out (7) and its curve menu (8)
            int split = 1;
            float run = 0.0f;
            while (split < n - 1 && run + natural[(std::size_t) split - 1] < total * 0.5f)
                run += natural[(std::size_t) split++ - 1] + 2.0f;
            if (split == 8)
                ++split;
            const auto top = tools.removeFromTop (tools.getHeight() / 2);
            layoutRow (top.withTrimmedBottom (1), 0, split);
            layoutRow (tools.withTrimmedTop (1), split, n);
        }

        infoArea = area.removeFromBottom (24);
        overviewArea = area.removeFromBottom (18).reduced (6, 2);
        area.removeFromBottom (2);
        area = area.reduced (6, 0);
        levelArea = area.removeFromLeft (34);
        rulerArea = area.removeFromTop (24);
        rulerArea.setX (area.getX());
        waveArea = area;
        clampView();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg2);
        g.setColour (colour::bg3);
        g.fillRect (getLocalBounds().removeFromTop (36));
        g.setColour (colour::border);
        g.drawHorizontalLine (36, 0.0f, (float) getWidth());

        paintRuler (g);
        paintLevels (g);
        if (spectrumView)
            paintSpectrum (g);
        else
            paintWave (g);
        paintOverlays (g);
        paintOverview (g);
        paintInfo (g);
    }

    // --- mouse ----------------------------------------------------------------------------------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        drag = Drag::none;
        if (doc.audio.empty())
        {
            if (e.mods.isPopupMenu())
                mainMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }

        const auto pos = e.getPosition();

        if (overviewArea.contains (pos))
        {
            drag = Drag::overview;
            centreViewOnOverview (e.x);
            return;
        }

        if (rulerArea.contains (pos))
        {
            // the loop handles live in the ruler's bottom strip
            if (doc.hasLoop() && pos.y >= rulerArea.getBottom() - 7)
            {
                if (std::abs (frameToX ((double) doc.loopStart) - (float) e.x) < 6.0f)
                {
                    beginPointDrag (Drag::loopStart);
                    return;
                }
                if (std::abs (frameToX ((double) doc.loopEnd) - (float) e.x) < 6.0f)
                {
                    beginPointDrag (Drag::loopEnd);
                    return;
                }
            }
            if (const auto m = markerAt (e.x); m >= 0)
            {
                if (e.mods.isPopupMenu())
                    markerMenu (m).showMenuAsync (juce::PopupMenu::Options().withMousePosition());
                else
                {
                    dragMarkerIndex = m;
                    beginPointDrag (Drag::marker);
                }
                return;
            }
        }

        if (e.mods.isPopupMenu())
        {
            editMenu (true).showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }

        if (waveArea.contains (pos) || rulerArea.contains (pos))
        {
            const auto f = snapFrame (xToFrame (e.x));
            if (e.mods.isShiftDown() && ! selection.empty())
                anchor = std::abs (f - selection.start) < std::abs (f - selection.end) ? selection.end : selection.start;
            else if (e.mods.isShiftDown())
                anchor = cursor;
            else
                anchor = f;
            selection = { std::min (anchor, f), std::max (anchor, f) };
            cursor = f;
            drag = Drag::select;
            if (playing && ! e.mods.isShiftDown())
                startPlayback();
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag == Drag::overview)
        {
            centreViewOnOverview (e.x);
            return;
        }
        if (drag == Drag::none)
            return;

        // auto-scroll past the edges
        if (e.x < waveArea.getX())
            viewStart -= (waveArea.getX() - e.x) * fpp * 0.25;
        else if (e.x > waveArea.getRight())
            viewStart += (e.x - waveArea.getRight()) * fpp * 0.25;
        clampView();

        const auto f = snapFrame (xToFrame (e.x));
        if (drag == Drag::select)
        {
            selection = { std::min (anchor, f), std::max (anchor, f) };
            cursor = f;
        }
        else if (drag == Drag::marker && juce::isPositiveAndBelow (dragMarkerIndex, (int) doc.markers.size()))
        {
            auto& m = doc.markers[(std::size_t) dragMarkerIndex];
            const auto length = m.end - m.start;
            m.start = juce::jlimit<std::int64_t> (0, doc.audio.frames() - length, f);
            m.end = m.start + length;
        }
        else if (drag == Drag::loopStart)
            doc.loopStart = juce::jlimit<std::int64_t> (0, doc.loopEnd - 1, f);
        else if (drag == Drag::loopEnd)
            doc.loopEnd = juce::jlimit<std::int64_t> (doc.loopStart + 1, doc.audio.frames(), f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag == Drag::marker || drag == Drag::loopStart || drag == Drag::loopEnd)
        {
            const auto& before = history.current();
            const auto moved = before.loopStart != doc.loopStart || before.loopEnd != doc.loopEnd || before.markers.size() != doc.markers.size()
                               || ! std::equal (before.markers.begin(), before.markers.end(), doc.markers.begin(),
                                                [] (const ae::Marker& a, const ae::Marker& b) { return a.start == b.start && a.end == b.end; });
            if (moved)
            {
                ae::sortMarkers (doc);
                commit (drag == Drag::marker ? tr ("Move marker") : tr ("Move loop"));
            }
        }
        drag = Drag::none;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (doc.audio.empty() || ! waveArea.contains (e.getPosition()))
            return;
        // the region under the mouse, else the stretch between the markers around it, else everything
        const auto f = xToFrame (e.x);
        std::int64_t from = 0, to = doc.audio.frames();
        for (const auto& m : doc.markers)
            if (m.isRegion() && f >= m.start && f < m.end)
            {
                selection = { m.start, m.end };
                repaint();
                return;
            }
        for (const auto& m : doc.markers)
        {
            if (m.start <= f)
                from = std::max (from, m.start);
            if (m.start > f)
                to = std::min (to, m.start);
        }
        selection = { from, to };
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (doc.audio.empty())
            return;
        const auto delta = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
            zoomAround ((float) e.x, delta > 0 ? 0.8 : 1.25);
        else
        {
            viewStart -= delta * (double) waveArea.getWidth() * fpp * 0.5;
            clampView();
            repaint();
        }
    }

    // --- keys -----------------------------------------------------------------------------------------------
    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto ctrl = k.getModifiers().isCtrlDown() || k.getModifiers().isCommandDown();
        const auto shift = k.getModifiers().isShiftDown();
        const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
        const auto code = k.getKeyCode();

        if (ctrl && (code == 'Z' || c == 'z'))
            shift ? redo() : undo();
        else if (ctrl && (code == 'Y' || c == 'y'))
            redo();
        else if (ctrl && (code == 'X' || c == 'x'))
            cutSelection();
        else if (ctrl && (code == 'C' || c == 'c'))
            copySelection();
        else if (ctrl && (code == 'V' || c == 'v'))
            paste (false);
        else if (ctrl && (code == 'A' || c == 'a'))
        {
            selection = { 0, doc.audio.frames() };
            repaint();
        }
        else if (ctrl && (code == 'S' || c == 's'))
            saveAs (bits);
        else if (code == juce::KeyPress::spaceKey)
            playing ? stopPlayback() : startPlayback();
        else if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
            deleteSelection();
        else if (code == juce::KeyPress::escapeKey)
        {
            if (recState != RecState::idle)
                stopRecording();
            selection = {};
            repaint();
        }
        else if (code == juce::KeyPress::homeKey)
            setCursor (0);
        else if (code == juce::KeyPress::endKey)
            setCursor (doc.audio.frames());
        else if (! ctrl && c == 'r')
            toggleRecording();
        else if (! ctrl && c == 'l')
            loopButton.triggerClick();
        else if (! ctrl && c == 's')
            viewButton.triggerClick();
        else if (! ctrl && c == 'm')
            addMarkerAtCursor();
        else if (! ctrl && c == 'f')
            zoomToFit();
        else if (c == '+' || c == '=')
            zoomAround ((float) waveArea.getCentreX(), 0.5);
        else if (c == '-')
            zoomAround ((float) waveArea.getCentreX(), 2.0);
        else
            return false;
        return true;
    }

    // --- drag and drop of files -----------------------------------------------------------------------------
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && juce::File (files[0]).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
    }

    // a sample dragged from LYNX's Browser too (issue #12)
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return BrowserComponent::isBrowserDrag (details) && BrowserComponent::draggedFile (details).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
    }

    void itemDropped (const SourceDetails& details) override
    {
        const auto file = BrowserComponent::draggedFile (details);
        if (file.existsAsFile())
            filesDropped ({ file.getFullPathName() }, 0, 0);
    }

    // FL's Edison: a dropped file replaces what the editor shows (issue #12); with Ctrl held it is pasted at the
    // cursor instead (LYNX's earlier behaviour)
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        if (files.isEmpty() || recState != RecState::idle)
            return;
        if (doc.audio.empty() || ! juce::ModifierKeys::getCurrentModifiersRealtime().isCommandDown())
        {
            stopPlayback();
            const auto previous = source;
            source = { Source::Kind::file, -1, files[0] };
            if (! loadFile (juce::File (files[0])))
                source = previous;
            return;
        }
        // into a document: pasted at the cursor
        auto data = engine.getSampleLibrary().load (files[0]);
        if (data == nullptr)
            return;
        ae::Audio clip;
        clip.sampleRate = data->sampleRate;
        clip.channels.emplace_back (data->leftData(), data->leftData() + data->frames());
        if (data->isStereo())
            clip.channels.emplace_back (data->rightData(), data->rightData() + data->frames());
        const auto at = cursor;
        ae::insert (doc, at, clip);
        selection = { at, at + ae::conform (clip, doc.audio.numChannels(), doc.audio.sampleRate).frames() };
        commit (tr ("Paste"));
    }

    // menus shared with the snapshot check
    juce::PopupMenu editMenu (bool contextual);

private:
    enum class Drag
    {
        none,
        select,
        marker,
        loopStart,
        loopEnd,
        overview
    };
    enum class RecState
    {
        idle,
        waiting,
        recording
    };

    // --- coordinates ----------------------------------------------------------------------------------------
    float frameToX (double frame) const noexcept { return (float) waveArea.getX() + (float) ((frame - viewStart) / fpp); }
    std::int64_t xToFrame (int x) const noexcept
    {
        return juce::jlimit<std::int64_t> (0, doc.audio.frames(), (std::int64_t) std::llround (viewStart + (double) (x - waveArea.getX()) * fpp));
    }
    double maxFpp() const noexcept { return juce::jmax (1.0, (double) doc.audio.frames() / juce::jmax (1, waveArea.getWidth())); }

    void clampView()
    {
        fpp = juce::jlimit (1.0 / 32.0, maxFpp(), fpp);
        const auto visible = (double) waveArea.getWidth() * fpp;
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, (double) doc.audio.frames() - visible), viewStart);
    }

    void zoomToFit()
    {
        fpp = maxFpp();
        viewStart = 0.0;
        clampView();
        repaint();
    }

    void zoomAround (float x, double factor)
    {
        const auto frame = viewStart + (x - (float) waveArea.getX()) * fpp;
        fpp *= factor;
        clampView();
        viewStart = frame - (x - (float) waveArea.getX()) * fpp;
        clampView();
        repaint();
    }

    void zoomTo (ae::Range r)
    {
        if (r.empty())
            return;
        fpp = (double) r.length() / juce::jmax (1, waveArea.getWidth());
        clampView();
        viewStart = (double) r.start;
        clampView();
        repaint();
    }

    void centreViewOnOverview (int x)
    {
        const auto frames = (double) doc.audio.frames();
        const auto f = (double) (x - overviewArea.getX()) / juce::jmax (1, overviewArea.getWidth()) * frames;
        viewStart = f - (double) waveArea.getWidth() * fpp * 0.5;
        clampView();
        repaint();
    }

    std::int64_t snapFrame (std::int64_t f) const
    {
        if (! snapZero)
            return f;
        return ae::nearestZeroCrossing (doc.audio, f, (std::int64_t) juce::jmax (8.0, fpp * 6.0));
    }

    int markerAt (int x) const
    {
        for (int i = (int) doc.markers.size() - 1; i >= 0; --i)
            if (std::abs (frameToX ((double) doc.markers[(std::size_t) i].start) - (float) x) < 5.0f)
                return i;
        return -1;
    }

    // markers and loop ends move live; mouseUp commits the move against the history's current state
    void beginPointDrag (Drag d) { drag = d; }

    void setCursor (std::int64_t f)
    {
        cursor = juce::jlimit<std::int64_t> (0, doc.audio.frames(), f);
        selection = {};
        if (frameToX ((double) cursor) < (float) waveArea.getX() || frameToX ((double) cursor) > (float) waveArea.getRight())
        {
            viewStart = (double) cursor - (double) waveArea.getWidth() * fpp * 0.5;
            clampView();
        }
        repaint();
    }

    // --- painting -------------------------------------------------------------------------------------------
    // per pixel column x of a lane: the min/max of the samples it covers (exact when zoomed in, from the
    // summary when zoomed out)
    bool columnRange (int channel, double from, double to, float& lo, float& hi) const
    {
        const auto frames = doc.audio.frames();
        if (to <= 0.0 || from >= (double) frames)
            return false;
        if (fpp > 256.0 && summary != nullptr)
        {
            const auto p = summary->query (from, to, doc.audio.numChannels() > 1 ? channel : -1);
            if (! p.valid)
                return false;
            lo = p.lo;
            hi = p.hi;
            return true;
        }
        const auto& data = doc.audio.channels[(std::size_t) channel];
        auto a = (std::int64_t) std::floor (from), b = (std::int64_t) std::ceil (to);
        a = juce::jlimit<std::int64_t> (0, frames - 1, a);
        b = juce::jlimit<std::int64_t> (a + 1, frames, b);
        lo = hi = data[(std::size_t) a];
        for (auto i = a + 1; i < b; ++i)
        {
            lo = std::min (lo, data[(std::size_t) i]);
            hi = std::max (hi, data[(std::size_t) i]);
        }
        return true;
    }

    juce::Rectangle<float> laneBounds (int lane) const
    {
        const auto lanes = juce::jmax (1, doc.audio.numChannels());
        const auto h = (float) waveArea.getHeight() / (float) lanes;
        return { (float) waveArea.getX(), (float) waveArea.getY() + h * (float) lane, (float) waveArea.getWidth(), h };
    }

    void paintWave (juce::Graphics& g)
    {
        using namespace theme;
        g.setColour (colour::bg0);
        g.fillRect (waveArea);

        if (doc.audio.empty())
        {
            g.setColour (colour::textSecondary);
            g.setFont (font (type::body));
            g.drawFittedText (recState != RecState::idle ? tr ("Waiting for input...")
                                                         : tr ("Empty: press Record, drop an audio file here or open one from the menu"),
                              waveArea.reduced (20), juce::Justification::centred, 2);
            return;
        }

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (waveArea);
        paintSelectionAndRegions (g);

        const auto lanes = doc.audio.numChannels();
        for (int lane = 0; lane < lanes; ++lane)
        {
            const auto r = laneBounds (lane);
            const auto centre = r.getCentreY();
            const auto half = r.getHeight() * 0.5f - 2.0f;

            // level guides: centre line and -6 dB
            g.setColour (colour::gridBeat);
            g.drawHorizontalLine ((int) (centre - half * 0.5f), r.getX(), r.getRight());
            g.drawHorizontalLine ((int) (centre + half * 0.5f), r.getX(), r.getRight());
            g.setColour (colour::gridBar);
            g.drawHorizontalLine ((int) centre, r.getX(), r.getRight());
            if (lane > 0)
            {
                g.setColour (colour::border);
                g.drawHorizontalLine ((int) r.getY(), r.getX(), r.getRight());
            }

            const auto width = waveArea.getWidth();
            if (fpp < 1.0)
            {
                // zoomed in past one sample per pixel: the samples joined by a line, dots when far apart
                const auto& data = doc.audio.channels[(std::size_t) lane];
                const auto first = juce::jmax<std::int64_t> (0, (std::int64_t) std::floor (viewStart) - 1);
                const auto last = juce::jmin<std::int64_t> (doc.audio.frames() - 1, (std::int64_t) std::ceil (viewStart + width * fpp) + 1);
                juce::Path line;
                for (auto i = first; i <= last; ++i)
                {
                    const auto p = juce::Point<float> (frameToX ((double) i), centre - juce::jlimit (-1.0f, 1.0f, data[(std::size_t) i]) * half);
                    i == first ? line.startNewSubPath (p) : line.lineTo (p);
                }
                g.setColour (waveColour());
                g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                if (fpp < 1.0 / 6.0)
                    for (auto i = first; i <= last; ++i)
                        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ frameToX ((double) i), centre - juce::jlimit (-1.0f, 1.0f, data[(std::size_t) i]) * half }));
                continue;
            }

            // one filled shape: the top edge (maxima) left to right, the bottom edge (minima) back
            std::vector<float> tops ((std::size_t) width), bottoms ((std::size_t) width);
            std::vector<char> valid ((std::size_t) width);
            for (int x = 0; x < width; ++x)
            {
                const auto from = viewStart + x * fpp;
                float lo = 0.0f, hi = 0.0f;
                valid[(std::size_t) x] = columnRange (lane, from, from + fpp, lo, hi) ? 1 : 0;
                tops[(std::size_t) x] = centre - juce::jlimit (-1.0f, 1.0f, hi) * half;
                bottoms[(std::size_t) x] = centre - juce::jlimit (-1.0f, 1.0f, lo) * half;
                if (bottoms[(std::size_t) x] - tops[(std::size_t) x] < 1.0f) // silence still shows as a hairline
                {
                    const auto mid = (bottoms[(std::size_t) x] + tops[(std::size_t) x]) * 0.5f;
                    tops[(std::size_t) x] = mid - 0.5f;
                    bottoms[(std::size_t) x] = mid + 0.5f;
                }
            }
            juce::Path wave;
            for (int x = 0; x < width;)
            {
                while (x < width && ! valid[(std::size_t) x])
                    ++x;
                if (x >= width)
                    break;
                const auto start = x;
                while (x < width && valid[(std::size_t) x])
                    ++x;
                const auto px = [this] (int i) { return (float) (waveArea.getX() + i); };
                wave.startNewSubPath (px (start), tops[(std::size_t) start]);
                for (int i = start + 1; i < x; ++i)
                    wave.lineTo (px (i), tops[(std::size_t) i]);
                wave.lineTo (px (x), tops[(std::size_t) (x - 1)]);
                wave.lineTo (px (x), bottoms[(std::size_t) (x - 1)]);
                for (int i = x - 1; i >= start; --i)
                    wave.lineTo (px (i), bottoms[(std::size_t) i]);
                wave.closeSubPath();
            }
            g.setColour (waveColour());
            g.fillPath (wave);
        }
    }

    void paintSelectionAndRegions (juce::Graphics& g)
    {
        using namespace theme;
        for (const auto& m : doc.markers)
            if (m.isRegion())
            {
                g.setColour (colour::warning.withAlpha (0.07f));
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (frameToX ((double) m.start), (float) waveArea.getY(), frameToX ((double) m.end),
                                                                        (float) waveArea.getBottom()));
            }
        if (! selection.empty())
        {
            g.setColour (colour::selectionFill);
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (frameToX ((double) selection.start), (float) waveArea.getY(),
                                                                    frameToX ((double) selection.end), (float) waveArea.getBottom()));
        }
    }

    void paintSpectrum (juce::Graphics& g)
    {
        using namespace theme;
        g.setColour (colour::bg0);
        g.fillRect (waveArea);
        if (doc.audio.empty())
        {
            paintWave (g);
            return;
        }

        const auto columns = juce::jmax (1, waveArea.getWidth() / 2);
        const auto height = juce::jmax (1, waveArea.getHeight());
        const auto key = juce::String (docVersion) + "|" + juce::String (viewStart) + "|" + juce::String (fpp) + "|" + juce::String (columns) + "x"
                         + juce::String (height) + "|" + juce::String (fftSize) + (logFrequency ? "l" : "n");
        if (key != spectrumKey)
        {
            const auto from = (std::int64_t) viewStart;
            const auto to = juce::jmin (doc.audio.frames(), (std::int64_t) std::ceil (viewStart + waveArea.getWidth() * fpp));
            const auto spec = ae::spectrogram (doc.audio, { from, to }, columns, fftSize);
            spectrumImage = juce::Image (juce::Image::RGB, columns, height, false);
            const auto nyquist = doc.audio.sampleRate * 0.5;
            const auto binHz = doc.audio.sampleRate / fftSize;
            for (int y = 0; y < height; ++y)
            {
                const auto t = 1.0 - (double) y / juce::jmax (1, height - 1);
                const auto hz = logFrequency ? 20.0 * std::pow (nyquist / 20.0, t) : t * nyquist;
                const auto bin = juce::jlimit (0, spec.bins - 1, (int) std::lround (hz / binHz));
                // the bins between this row and the next (log scale, high frequencies) are summarised by their max
                const auto nextT = 1.0 - (double) (y + 1) / juce::jmax (1, height - 1);
                const auto nextHz = logFrequency ? 20.0 * std::pow (nyquist / 20.0, nextT) : nextT * nyquist;
                const auto binLow = juce::jlimit (0, spec.bins - 1, (int) std::lround (nextHz / binHz));
                for (int c = 0; c < columns; ++c)
                {
                    auto db = spec.at (c, bin);
                    for (int b = binLow; b < bin; ++b)
                        db = std::max (db, spec.at (c, b));
                    spectrumImage.setPixelAt (c, y, spectrumColour (db));
                }
            }
            spectrumKey = key;
        }
        g.drawImage (spectrumImage, waveArea.toFloat());

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (waveArea);
        if (! selection.empty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (frameToX ((double) selection.start), (float) waveArea.getY(),
                                                                    frameToX ((double) selection.end), (float) waveArea.getBottom()));
        }
    }

    void paintLevels (juce::Graphics& g)
    {
        using namespace theme;
        g.setColour (colour::bg2);
        g.fillRect (levelArea);
        g.setFont (font (type::caption));
        g.setColour (colour::textSecondary);
        if (doc.audio.empty())
            return;
        if (spectrumView)
        {
            const auto nyquist = doc.audio.sampleRate * 0.5;
            for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
            {
                if (hz >= nyquist)
                    continue;
                const auto t = logFrequency ? std::log (hz / 20.0) / std::log (nyquist / 20.0) : hz / nyquist;
                const auto y = (float) waveArea.getBottom() - (float) t * (float) waveArea.getHeight();
                g.drawText (hz >= 1000.0 ? juce::String (hz / 1000.0, 0) + "k" : juce::String ((int) hz), juce::Rectangle<float> (0.0f, y - 6.0f, 30.0f, 12.0f).withX ((float) levelArea.getX()),
                            juce::Justification::centredRight);
            }
            return;
        }
        for (int lane = 0; lane < doc.audio.numChannels(); ++lane)
        {
            const auto r = laneBounds (lane);
            const auto half = r.getHeight() * 0.5f - 2.0f;
            auto label = [&] (float level, const juce::String& text) {
                const auto y = r.getCentreY() - level * half;
                g.drawText (text, juce::Rectangle<float> ((float) levelArea.getX(), y - 6.0f, 30.0f, 12.0f), juce::Justification::centredRight);
            };
            if (r.getHeight() > 70.0f)
            {
                label (0.5f, "-6");
                label (-0.5f, "-6");
            }
            label (0.0f, doc.audio.numChannels() > 1 ? (lane == 0 ? "L" : "R") : "0");
        }
    }

    void paintRuler (juce::Graphics& g)
    {
        using namespace theme;
        g.setColour (colour::bg3);
        g.fillRect (rulerArea);
        if (doc.audio.empty())
            return;

        // a time step that leaves ~90 px between labels
        const auto rate = doc.audio.sampleRate;
        const auto secondsPerPixel = fpp / rate;
        double step = 0.001;
        for (double s : { 0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
            if (s / secondsPerPixel >= 90.0)
            {
                step = s;
                break;
            }
        const auto bars = timeFormat == TimeFormat::bars;
        const auto beatSeconds = 60.0 / juce::jmax (1.0, engine.getSong().bpm);
        if (bars)
        {
            step = beatSeconds;
            while (step / secondsPerPixel < 90.0)
                step *= 2.0;
        }

        g.setFont (font (type::caption));
        const auto firstSecond = viewStart / rate;
        const auto lastSecond = (viewStart + waveArea.getWidth() * fpp) / rate;
        for (auto t = std::floor (firstSecond / step) * step; t <= lastSecond; t += step)
        {
            const auto x = frameToX (t * rate);
            if (x < (float) rulerArea.getX())
                continue;
            g.setColour (colour::gridBar);
            g.drawVerticalLine ((int) x, (float) rulerArea.getBottom() - 7.0f, (float) rulerArea.getBottom());
            g.setColour (colour::textSecondary);
            g.drawText (formatTime ((std::int64_t) std::llround (t * rate)), juce::Rectangle<float> (x + 3.0f, (float) rulerArea.getY(), 90.0f, 13.0f),
                        juce::Justification::centredLeft);
        }

        // regions and markers: flags with their names; the loop: a bar along the bottom
        for (const auto& m : doc.markers)
        {
            const auto x = frameToX ((double) m.start);
            if (m.isRegion())
            {
                g.setColour (colour::warning.withAlpha (0.22f));
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x, (float) rulerArea.getY() + 12.0f, frameToX ((double) m.end), (float) rulerArea.getBottom() - 5.0f));
            }
            juce::Path flag;
            flag.addTriangle (x, (float) rulerArea.getY() + 12.0f, x + 7.0f, (float) rulerArea.getY() + 15.5f, x, (float) rulerArea.getY() + 19.0f);
            g.setColour (colour::warning);
            g.fillPath (flag);
            g.drawVerticalLine ((int) x, (float) rulerArea.getY() + 12.0f, (float) rulerArea.getBottom());
            if (! m.name.empty())
                g.drawText (juce::String::fromUTF8 (m.name.c_str()), juce::Rectangle<float> (x + 8.0f, (float) rulerArea.getY() + 12.0f, 120.0f, 8.0f),
                            juce::Justification::centredLeft);
        }
        if (doc.hasLoop())
        {
            const auto a = frameToX ((double) doc.loopStart), b = frameToX ((double) doc.loopEnd);
            g.setColour (colour::accent);
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (a, (float) rulerArea.getBottom() - 4.0f, b, (float) rulerArea.getBottom()));
            g.fillRect (juce::Rectangle<float> (a, (float) rulerArea.getBottom() - 8.0f, 2.0f, 8.0f));
            g.fillRect (juce::Rectangle<float> (b - 2.0f, (float) rulerArea.getBottom() - 8.0f, 2.0f, 8.0f));
        }
    }

    void paintOverlays (juce::Graphics& g)
    {
        using namespace theme;
        if (doc.audio.empty())
            return;
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (waveArea);

        for (const auto& m : doc.markers)
        {
            g.setColour (colour::warning.withAlpha (0.55f));
            g.drawVerticalLine ((int) frameToX ((double) m.start), (float) waveArea.getY(), (float) waveArea.getBottom());
            if (m.isRegion())
            {
                g.setColour (colour::warning.withAlpha (0.3f));
                g.drawVerticalLine ((int) frameToX ((double) m.end), (float) waveArea.getY(), (float) waveArea.getBottom());
            }
        }
        if (doc.hasLoop())
        {
            g.setColour (colour::accent.withAlpha (0.6f));
            const float dash[] = { 4.0f, 3.0f };
            for (auto f : { doc.loopStart, doc.loopEnd })
            {
                const auto x = frameToX ((double) f);
                g.drawDashedLine ({ x, (float) waveArea.getY(), x, (float) waveArea.getBottom() }, dash, 2);
            }
        }
        if (! selection.empty())
        {
            g.setColour (colour::selection.withAlpha (0.8f));
            g.drawVerticalLine ((int) frameToX ((double) selection.start), (float) waveArea.getY(), (float) waveArea.getBottom());
            g.drawVerticalLine ((int) frameToX ((double) selection.end), (float) waveArea.getY(), (float) waveArea.getBottom());
        }
        else
        {
            g.setColour (colour::textPrimary.withAlpha (0.7f));
            g.drawVerticalLine ((int) frameToX ((double) cursor), (float) waveArea.getY(), (float) waveArea.getBottom());
        }
        if (playing && playhead >= 0)
        {
            g.setColour (colour::playhead);
            g.fillRect (juce::Rectangle<float> (frameToX ((double) playhead) - 0.5f, (float) waveArea.getY(), 2.0f, (float) waveArea.getHeight()));
        }
        if (recState != RecState::idle)
        {
            auto badge = juce::Rectangle<float> ((float) waveArea.getX() + 8.0f, (float) waveArea.getY() + 8.0f, recState == RecState::recording ? 70.0f : 150.0f, 20.0f);
            g.setColour (colour::bg1.withAlpha (0.85f));
            g.fillRoundedRectangle (badge, radius::md);
            g.setColour (colour::record);
            g.fillEllipse (badge.removeFromLeft (20.0f).reduced (5.0f));
            g.setFont (font (type::small, true));
            g.drawText (recState == RecState::recording ? juce::String ("REC") : tr ("Waiting..."), badge, juce::Justification::centredLeft);
        }
    }

    void paintOverview (juce::Graphics& g)
    {
        using namespace theme;
        g.setColour (colour::bg0);
        g.fillRoundedRectangle (overviewArea.toFloat(), radius::sm);
        if (doc.audio.empty() || summary == nullptr)
            return;
        const auto frames = (double) doc.audio.frames();
        const auto perPixel = frames / juce::jmax (1, overviewArea.getWidth());
        const auto centre = (float) overviewArea.getCentreY();
        const auto half = overviewArea.getHeight() * 0.5f - 1.0f;
        g.setColour (waveColour().withAlpha (0.55f));
        for (int x = 0; x < overviewArea.getWidth(); ++x)
        {
            const auto p = summary->query (x * perPixel, (x + 1) * perPixel, -1);
            if (! p.valid)
                break;
            const auto a = juce::jmin (1.0f, std::max (std::abs (p.lo), std::abs (p.hi)));
            g.drawVerticalLine (overviewArea.getX() + x, centre - a * half, centre + a * half + 1.0f);
        }
        const auto x0 = (float) overviewArea.getX() + (float) (viewStart / frames) * overviewArea.getWidth();
        const auto w = (float) ((double) waveArea.getWidth() * fpp / frames) * overviewArea.getWidth();
        g.setColour (colour::accent.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (x0, (float) overviewArea.getY(), juce::jmax (3.0f, w), (float) overviewArea.getHeight()));
        g.setColour (colour::accent);
        g.drawRect (juce::Rectangle<float> (x0, (float) overviewArea.getY(), juce::jmax (3.0f, w), (float) overviewArea.getHeight()), 1.0f);
    }

    void paintInfo (juce::Graphics& g)
    {
        using namespace theme;
        g.setColour (colour::bg1);
        g.fillRect (infoArea);
        g.setColour (colour::border);
        g.drawHorizontalLine (infoArea.getY(), 0.0f, (float) getWidth());

        auto area = infoArea.reduced (10, 0);
        g.setFont (font (type::small));
        if (status.isNotEmpty())
        {
            g.setColour (statusIsError ? colour::meterClip : colour::textSecondary);
            g.drawText (status, area.removeFromRight (juce::jmin (420, area.getWidth() / 2)), juce::Justification::centredRight);
        }
        if (doc.audio.empty())
            return;

        juce::StringArray parts;
        parts.add (juce::String (juce::roundToInt (doc.audio.sampleRate)) + " Hz");
        parts.add (doc.audio.numChannels() > 1 ? tr ("Stereo") : tr ("Mono"));
        parts.add (tr ("Length") + " " + formatTime (doc.audio.frames()));
        const auto r = ae::orAll (doc, selection);
        if (! selection.empty())
            parts.add (tr ("Selection") + " " + formatTime (selection.start) + " - " + formatTime (selection.end) + " (" + formatTime (selection.length()) + ")");
        else
            parts.add (tr ("Cursor") + " " + formatTime (cursor));
        const auto p = ae::peak (doc.audio, r);
        parts.add (tr ("Peak") + " " + (p > 0.0f ? juce::String (20.0f * std::log10 (p), 1) + " dB" : juce::String ("-inf dB")));
        g.setColour (colour::textPrimary);
        g.drawText (parts.joinIntoString ("   |   "), area, juce::Justification::centredLeft);
    }

    juce::String formatTime (std::int64_t frames) const
    {
        const auto rate = doc.audio.sampleRate;
        switch (timeFormat)
        {
            case TimeFormat::samples: return juce::String (frames);
            case TimeFormat::bars:
            {
                const auto beats = (double) frames / rate * engine.getSong().bpm / 60.0;
                const auto perBar = juce::jmax (1, engine.getSong().beatsPerBar);
                const auto bar = (int) (beats / perBar);
                const auto beat = (int) beats - bar * perBar;
                const auto tick = (int) ((beats - std::floor (beats)) * 96.0);
                return juce::String (bar + 1) + ":" + juce::String (beat + 1) + ":" + juce::String (tick).paddedLeft ('0', 2);
            }
            case TimeFormat::automatic:
                if (fpp < 1.0)
                    return juce::String (frames);
                [[fallthrough]];
            case TimeFormat::seconds: return formatSeconds ((double) frames / rate);
        }
        return {};
    }

    // --- edits ----------------------------------------------------------------------------------------------
    void commit (const juce::String& label)
    {
        history.commit (doc, label.toStdString());
        modified = true;
        documentChanged();
    }

    // runs `fn` on the selection (an empty selection: the whole audio, unless `needsSelection`)
    void edit (const juce::String& label, const std::function<void (ae::Document&, ae::Range)>& fn, bool needsSelection = false)
    {
        if (doc.audio.empty() || recState != RecState::idle)
            return;
        if (needsSelection && selection.empty())
        {
            setStatus (tr ("Select a part first (drag over the waveform)"), false);
            return;
        }
        stopPlayback();
        const auto frames = doc.audio.frames();
        fn (doc, selection);
        // a length-changing edit of the selection keeps the edited part selected
        if (! selection.empty())
            selection.end = juce::jmax (selection.start, selection.end + (doc.audio.frames() - frames));
        commit (label);
    }

    void documentChanged()
    {
        ++docVersion;
        selection = ae::clamp (doc, selection);
        cursor = juce::jlimit<std::int64_t> (0, doc.audio.frames(), cursor);
        rebuildSummary();
        clampView();
        updateButtons();
        updateTitle();
        repaint();
    }

    void rebuildSummary()
    {
        if (doc.audio.empty())
        {
            summary.reset();
            return;
        }
        const auto* l = doc.audio.channels[0].data();
        const auto* r = doc.audio.numChannels() > 1 ? doc.audio.channels[1].data() : nullptr;
        summary = std::make_shared<wave::Summary> (wave::analyse (l, r, (std::size_t) doc.audio.frames(), doc.audio.sampleRate));
    }

    void undo()
    {
        if (! history.canUndo() || recState != RecState::idle)
            return;
        stopPlayback();
        doc = history.undo();
        modified = true;
        documentChanged();
    }

    void redo()
    {
        if (! history.canRedo() || recState != RecState::idle)
            return;
        stopPlayback();
        doc = history.redo();
        modified = true;
        documentChanged();
    }

    void copySelection()
    {
        if (selection.empty())
            return;
        clipboard() = ae::copy (doc.audio, selection);
        setStatus (tr ("Copied {0}", { formatTime (selection.length()) }), false);
    }

    void cutSelection()
    {
        if (selection.empty())
            return;
        clipboard() = ae::copy (doc.audio, selection);
        const auto at = selection.start;
        edit (tr ("Cut"), [] (ae::Document& d, ae::Range r) { ae::erase (d, r); }, true);
        selection = {};
        cursor = at;
        repaint();
    }

    void trimSelection()
    {
        if (selection.empty())
            return;
        edit (tr ("Trim"), [] (ae::Document& d, ae::Range r) { ae::trim (d, r); }, true);
        selection = {};
        cursor = 0;
        zoomToFit();
    }

    void deleteSelection()
    {
        if (selection.empty())
            return;
        const auto at = selection.start;
        edit (tr ("Delete"), [] (ae::Document& d, ae::Range r) { ae::erase (d, r); }, true);
        selection = {};
        cursor = at;
        repaint();
    }

    // insert at the cursor, replace the selection, or (mix) sum at the cursor / selection start
    void paste (bool mix)
    {
        const auto& clip = clipboard();
        if (clip.empty() || recState != RecState::idle)
            return;
        stopPlayback();
        const auto wasEmpty = doc.audio.empty();
        const auto at = selection.empty() ? cursor : selection.start;
        const auto length = wasEmpty ? clip.frames() : ae::conform (clip, doc.audio.numChannels(), doc.audio.sampleRate).frames();
        if (mix)
            ae::pasteMix (doc, at, clip);
        else if (! selection.empty())
            ae::pasteReplace (doc, selection, clip);
        else
            ae::insert (doc, at, clip);
        selection = { at, at + length };
        commit (mix ? tr ("Paste mix") : tr ("Paste"));
        if (wasEmpty)
            zoomToFit();
    }

    void fade (bool in)
    {
        const auto curve = fadeCurve;
        edit (in ? tr ("Fade in") : tr ("Fade out"), [in, curve] (ae::Document& d, ae::Range r) { in ? ae::fadeIn (d, r, curve) : ae::fadeOut (d, r, curve); });
    }

    void addMarkerAtCursor()
    {
        if (doc.audio.empty())
            return;
        ae::addMarker (doc, selection.empty() ? cursor : selection.start, {});
        commit (tr ("Add marker"));
    }

    void addRegionFromSelection()
    {
        if (selection.empty())
        {
            setStatus (tr ("Select a part first (drag over the waveform)"), false);
            return;
        }
        ae::addRegion (doc, selection, tr ("Region").toStdString() + " " + std::to_string (std::count_if (doc.markers.begin(), doc.markers.end(), [] (const ae::Marker& m) { return m.isRegion(); }) + 1));
        commit (tr ("Add region"));
    }

    void setLoopFromSelection()
    {
        if (selection.empty())
        {
            setStatus (tr ("Select a part first (drag over the waveform)"), false);
            return;
        }
        ae::setLoop (doc, selection);
        commit (tr ("Set loop"));
    }

    // --- tool windows ---------------------------------------------------------------------------------------
    // plays `fn` applied to a copy of the selection (Preview)
    void previewProcessed (const std::function<void (ae::Document&)>& fn)
    {
        ae::Document part;
        part.audio = ae::copy (doc.audio, ae::orAll (doc, selection));
        fn (part);
        playAudio (part.audio, 0, false);
    }

    void openTool (std::unique_ptr<ToolPanel> panel, const juce::String& title)
    {
        openToolWindow (std::move (panel), title, "audioeditor-tool");
    }

    void showAmp()
    {
        if (doc.audio.empty())
            return;
        juce::Component::SafePointer<AudioEditor> self (this);
        auto run = [] (ae::Document& d, ae::Range r, const std::vector<double>& v) { ae::amp (d, r, juce::Decibels::decibelsToGain ((float) v[0], -100.0f), (float) v[1] / 100.0f); };
        openTool (std::make_unique<ToolPanel> (std::vector<ToolPanel::Param> { { tr ("Volume"), -36.0, 24.0, 0.0, 0.1, " dB" }, { tr ("Pan"), -100.0, 100.0, 0.0, 1.0, "%" } },
                                               juce::StringArray {}, juce::String {}, 0,
                                               [self, run] (const std::vector<double>& v, int) {
                                                   if (self != nullptr)
                                                       self->edit (tr ("Amp"), [run, v] (ae::Document& d, ae::Range r) { run (d, r, v); });
                                               },
                                               [self, run] (const std::vector<double>& v, int) {
                                                   if (self != nullptr)
                                                       self->previewProcessed ([run, v] (ae::Document& d) { run (d, {}, v); });
                                               }),
                  tr ("Amp"));
    }

    void showStretch()
    {
        if (doc.audio.empty())
            return;
        juce::Component::SafePointer<AudioEditor> self (this);
        // v: time (%), pitch (semitones), fine (cents); choice 0 = stretch (pitch kept), 1 = resample (speed)
        auto run = [] (ae::Document& d, ae::Range r, const std::vector<double>& v, int method) {
            const auto ratio = v[0] / 100.0;
            const auto semitones = v[1] + v[2] / 100.0;
            if (method == 1)
                ae::changeSpeed (d, r, ratio * std::pow (2.0, -semitones / 12.0));
            else
            {
                const auto start = ae::orAll (d, r).start;
                const auto length = ae::orAll (d, r).length();
                ae::timeStretch (d, r, ratio);
                ae::pitchShift (d, { start, start + (std::int64_t) std::llround ((double) length * ratio) }, semitones);
            }
        };
        openTool (std::make_unique<ToolPanel> (
                      std::vector<ToolPanel::Param> { { tr ("Length"), 25.0, 400.0, 100.0, 0.1, "%" },
                                                      { tr ("Pitch"), -24.0, 24.0, 0.0, 1.0, " st" },
                                                      { tr ("Fine"), -100.0, 100.0, 0.0, 1.0, " ct" } },
                      juce::StringArray { tr ("Stretch (keep pitch and length apart)"), tr ("Resample (pitch follows the speed)") }, tr ("Method"), 0,
                      [self, run] (const std::vector<double>& v, int m) {
                          if (self != nullptr)
                              self->edit (tr ("Time stretch / pitch shift"), [run, v, m] (ae::Document& d, ae::Range r) { run (d, r, v, m); });
                      },
                      [self, run] (const std::vector<double>& v, int m) {
                          if (self != nullptr)
                              self->previewProcessed ([run, v, m] (ae::Document& d) { run (d, {}, v, m); });
                      }),
                  tr ("Time stretch / pitch shift"));
    }

    void showResample()
    {
        if (doc.audio.empty())
            return;
        static const int rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000 };
        juce::StringArray names;
        int current = 5;
        for (int i = 0; i < 10; ++i)
        {
            names.add (juce::String (rates[i]) + " Hz");
            if (std::abs (rates[i] - doc.audio.sampleRate) < 1.0)
                current = i;
        }
        juce::Component::SafePointer<AudioEditor> self (this);
        openTool (std::make_unique<ToolPanel> (std::vector<ToolPanel::Param> {}, names, tr ("Sample rate"), current,
                                               [self] (const std::vector<double>&, int i) {
                                                   if (self != nullptr && i >= 0)
                                                   {
                                                       self->selection = {};
                                                       self->edit (tr ("Resample"), [i] (ae::Document& d, ae::Range) { ae::resample (d, rates[i]); });
                                                       self->zoomToFit();
                                                   }
                                               },
                                               nullptr),
                  tr ("Resample"));
    }

    void showInsertSilence()
    {
        if (doc.audio.empty())
            return;
        juce::Component::SafePointer<AudioEditor> self (this);
        openTool (std::make_unique<ToolPanel> (std::vector<ToolPanel::Param> { { tr ("Length"), 1.0, 10000.0, 500.0, 1.0, " ms" } }, juce::StringArray {},
                                               juce::String {}, 0,
                                               [self] (const std::vector<double>& v, int) {
                                                   if (self == nullptr)
                                                       return;
                                                   const auto frames = (std::int64_t) std::llround (v[0] * 0.001 * self->doc.audio.sampleRate);
                                                   const auto at = self->cursor;
                                                   ae::insertSilence (self->doc, at, frames);
                                                   self->selection = { at, at + frames };
                                                   self->commit (tr ("Insert silence"));
                                               },
                                               nullptr),
                  tr ("Insert silence"));
    }

    void showNormalizeTo()
    {
        if (doc.audio.empty())
            return;
        juce::Component::SafePointer<AudioEditor> self (this);
        openTool (std::make_unique<ToolPanel> (std::vector<ToolPanel::Param> { { tr ("Peak"), -24.0, 0.0, -0.3, 0.1, " dB" } }, juce::StringArray {}, juce::String {}, 0,
                                               [self] (const std::vector<double>& v, int) {
                                                   if (self != nullptr)
                                                       self->edit (tr ("Normalize"), [v] (ae::Document& d, ae::Range r) { ae::normalize (d, r, (float) v[0]); });
                                               },
                                               nullptr),
                  tr ("Normalize"));
    }

    void autoSlice (float sensitivity)
    {
        if (doc.audio.empty())
            return;
        const auto r = ae::orAll (doc, selection);
        auto part = ae::copy (doc.audio, r);
        int added = 0;
        for (auto f : ae::detectOnsets (part, sensitivity))
            if (f > 0 || r.start > 0)
            {
                ae::addMarker (doc, r.start + f, {});
                ++added;
            }
        if (added > 0)
            commit (tr ("Auto-slice"));
        setStatus (tr ("{0} markers added", { juce::String (added) }), false);
    }

    void gridSlice (double beats)
    {
        if (doc.audio.empty())
            return;
        const auto step = beats * 60.0 / juce::jmax (1.0, engine.getSong().bpm) * doc.audio.sampleRate;
        const auto r = ae::orAll (doc, selection);
        int added = 0;
        for (auto f = (double) r.start + step; f < (double) r.end - 1.0; f += step, ++added)
            ae::addMarker (doc, (std::int64_t) std::llround (f), {});
        if (added > 0)
            commit (tr ("Grid slice"));
    }

    // --- playback -------------------------------------------------------------------------------------------
    void playAudio (const ae::Audio& audio, std::int64_t from, bool loop)
    {
        if (audio.empty())
            return;
        auto data = std::make_shared<SampleData>();
        data->sampleRate = audio.sampleRate;
        data->left = audio.channels[0];
        if (audio.numChannels() > 1)
            data->right = audio.channels[1];
        engine.previewSample (data, loop, 1.0f, true); // (at the volume knob's level)
        playing = true;
        playFrom = from;
        playStartedMs = juce::Time::getMillisecondCounterHiRes();
        playhead = from;
        playButton.setIcon (Icon::pause);
    }

    void startPlayback()
    {
        if (doc.audio.empty() || recState != RecState::idle)
            return;
        auto r = selection;
        if (r.empty() && loopPlay && doc.hasLoop())
            r = { doc.loopStart, doc.loopEnd };
        if (r.empty())
            r = { cursor, doc.audio.frames() };
        if (r.length() < 2)
            r = { 0, doc.audio.frames() };
        playAudio (ae::copy (doc.audio, r), r.start, loopPlay);
    }

    void stopPlayback()
    {
        if (! playing)
            return;
        engine.previewSample (nullptr);
        playing = false;
        playhead = -1;
        playButton.setIcon (Icon::play);
        repaint();
    }

    // --- recording ------------------------------------------------------------------------------------------
    void toggleRecording()
    {
        if (recState == RecState::idle)
            startRecording();
        else
            stopRecording();
    }

    void startRecording()
    {
        stopPlayback();
        auto* device = engine.getDeviceManager().getCurrentAudioDevice();
        if (device == nullptr)
        {
            setStatus (tr ("No audio device is open (Settings > Audio)"), true);
            return;
        }
        if (recordSource == AudioCapture::input)
        {
            if (! engine.hasAudioInput())
            {
                if (const auto result = engine.openAudioInput(); result.issue != AudioEngine::InputIssue::none)
                {
                    setStatus (result.detail.isNotEmpty() ? result.detail : tr ("No active audio input (Settings > Audio)"), true);
                    return;
                }
                openedInput = true; // closed again when the recording stops (round 165q)
            }
            device = engine.getDeviceManager().getCurrentAudioDevice();
            if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
            {
                setStatus (tr ("No active audio input (Settings > Audio)"), true);
                return;
            }
        }
        recordRate = device->getCurrentSampleRate();
        recordStereo = recordSource != AudioCapture::input || device->getActiveInputChannels().countNumberOfSetBits() > 1;
        recL.clear();
        recR.clear();
        beforeRecording = doc;
        recordAt = selection.empty() ? cursor : selection.start;
        if (doc.audio.empty())
            recordAt = 0;
        engine.getMeters().capture.setSource (recordSource);
        recState = recordMode == RecordMode::now ? RecState::recording : RecState::waiting;
        recordButton.setToggleState (true, juce::dontSendNotification);
        if (recordMode == RecordMode::onPlay && ! engine.getTransport().playing.load())
            setStatus (tr ("Recording starts when the song plays"), false);
        else if (recordMode == RecordMode::onInput)
            setStatus (tr ("Recording starts when the input is heard"), false);
        else
            setStatus (tr ("Recording from {0}", { sourceName (recordSource) }), false);
        repaint();
    }

    void drainRecording()
    {
        std::vector<float> l, r;
        engine.getMeters().capture.pull (l, r);
        if (l.empty())
            return;
        std::size_t from = 0;
        if (recState == RecState::waiting)
        {
            if (recordMode == RecordMode::onPlay)
            {
                if (! engine.getTransport().playing.load())
                    return;
            }
            else
            {
                while (from < l.size() && std::abs (l[from]) < inputThreshold && std::abs (r[from]) < inputThreshold)
                    ++from;
                if (from == l.size())
                    return;
            }
            recState = RecState::recording;
            setStatus (tr ("Recording from {0}", { sourceName (recordSource) }), false);
        }
        recL.insert (recL.end(), l.begin() + (std::ptrdiff_t) from, l.end());
        recR.insert (recR.end(), r.begin() + (std::ptrdiff_t) from, r.end());
        if (recordMode == RecordMode::onPlay && ! engine.getTransport().playing.load())
            stopRecording();
    }

    ae::Audio recordedAudio() const
    {
        ae::Audio a;
        a.sampleRate = recordRate;
        a.channels.push_back (recL);
        if (recordStereo)
            a.channels.push_back (recR);
        return a;
    }

    // the document = what it was, with the recording inserted at the cursor
    void showRecording()
    {
        doc = beforeRecording;
        const auto rec = recordedAudio();
        if (doc.audio.empty())
        {
            doc.audio = rec;
            doc.title = tr ("Recording").toStdString();
        }
        else
            ae::insert (doc, recordAt, rec);
        const auto length = doc.audio.frames() - beforeRecording.audio.frames();
        selection = { recordAt, recordAt + length };
        cursor = recordAt + length;
        rebuildSummary();
        ++docVersion;
        // follow the end
        const auto visible = (double) waveArea.getWidth() * fpp;
        if (beforeRecording.audio.empty() || (double) cursor > viewStart + visible)
        {
            if (beforeRecording.audio.empty() && (double) cursor > visible)
                fpp = juce::jmax (fpp, (double) cursor / juce::jmax (1, waveArea.getWidth()) * 1.25);
            viewStart = juce::jmax (0.0, (double) cursor - visible * 0.8);
        }
        clampView();
        repaint();
    }

    // the input this editor opened for its recording: closed again (FL does not keep the microphone open; Windows
    // showed its microphone-in-use icon until LYNX quit — round 165q)
    void releaseInput()
    {
        if (! openedInput)
            return;
        openedInput = false;
        engine.releaseAudioInputIfUnused();
    }

    void stopRecording()
    {
        if (recState == RecState::idle)
            return;
        drainRecording();
        engine.getMeters().capture.setSource (AudioCapture::off);
        releaseInput();
        const auto got = ! recL.empty();
        const auto dropped = engine.getMeters().capture.droppedFrames();
        recState = RecState::idle;
        recordButton.setToggleState (false, juce::dontSendNotification);
        if (got)
        {
            showRecording();
            commit (tr ("Record"));
            setStatus (tr ("Recorded {0}", { formatTime ((std::int64_t) recL.size()) }) + (dropped > 0 ? "  (" + tr ("{0} frames lost", { juce::String (dropped) }) + ")" : ""), dropped > 0);
        }
        else
        {
            doc = beforeRecording;
            documentChanged();
            setStatus (tr ("Nothing was recorded"), false);
        }
        recL.clear();
        recR.clear();
    }

    juce::String sourceName (int s) const
    {
        if (s == AudioCapture::input)
            return tr ("the audio input");
        if (s == 0)
            return tr ("the Master");
        const auto& mixer = engine.getSong().mixer;
        if (juce::isPositiveAndBelow (s, (int) mixer.size()) && ! mixer[(std::size_t) s].name.empty())
            return tr ("insert {0}", { juce::String (s) }) + " (" + juce::String::fromUTF8 (mixer[(std::size_t) s].name.c_str()) + ")";
        return tr ("insert {0}", { juce::String (s) });
    }

public:
    void recordInputWithSong()
    {
        recordSource = AudioCapture::input;
        recordMode = RecordMode::onPlay;
        startRecording();
    }

    void setRecordSource (int s)
    {
        recordSource = s;
        setStatus (tr ("Press Record to capture {0}", { sourceName (s) }), false);
    }

private:
    // --- output ---------------------------------------------------------------------------------------------
    juce::String safeTitle() const
    {
        auto t = juce::File::createLegalFileName (juce::String::fromUTF8 (doc.title.c_str())).trim();
        return t.isEmpty() ? juce::String ("Audio") : t;
    }

    // the edited audio as a new WAV in Music\LYNX\Edits (a new name each time: the Sampler and the waveform caches
    // key on the path, so nothing stale is ever shown)
    juce::File writeEditFile()
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("LYNX/Edits");
        folder.createDirectory();
        const auto file = folder.getNonexistentChildFile (safeTitle() + " (edit)", ".wav");
        if (! writeWav (file, bits))
            return {};
        return file;
    }

    bool writeWav (const juce::File& file, ae::WavFormat format)
    {
        const auto bytes = ae::encodeWav (doc, format);
        if (! file.replaceWithData (bytes.data(), bytes.size()))
        {
            setStatus (tr ("Could not write {0}", { file.getFullPathName() }), true);
            return false;
        }
        engine.getSampleLibrary().forget (file.getFullPathName());
        return true;
    }

    void saveAs (ae::WavFormat format)
    {
        if (doc.audio.empty())
            return;
        auto start = lastFile.existsAsFile() ? lastFile.getSiblingFile (safeTitle() + ".wav")
                                             : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (safeTitle() + ".wav");
        chooser = std::make_unique<juce::FileChooser> (tr ("Save as WAV"), start, "*.wav");
        juce::Component::SafePointer<AudioEditor> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [self, format] (const juce::FileChooser& fc) {
                                  if (self == nullptr || fc.getResult() == juce::File {})
                                      return;
                                  const auto file = fc.getResult().withFileExtension ("wav");
                                  if (self->writeWav (file, format))
                                  {
                                      self->bits = format;
                                      self->lastFile = file;
                                      self->modified = false;
                                      self->setStatus (tr ("Saved {0}", { file.getFileName() }), false);
                                      self->updateTitle();
                                  }
                              });
    }

    void openFileDialog()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Open audio file"), lastFile, engine.getSampleLibrary().wildcard());
        juce::Component::SafePointer<AudioEditor> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [self] (const juce::FileChooser& fc) {
            if (self == nullptr || ! fc.getResult().existsAsFile())
                return;
            self->stopPlayback();
            self->source = { Source::Kind::file, -1, fc.getResult().getFullPathName() };
            self->loadFile (fc.getResult());
        });
    }

    // each region (or each stretch between markers when there are no regions) as its own WAV in a folder
    void exportRegions()
    {
        std::vector<ae::Range> parts;
        std::vector<std::string> names;
        for (const auto& m : doc.markers)
            if (m.isRegion())
            {
                parts.push_back ({ m.start, m.end });
                names.push_back (m.name);
            }
        if (parts.empty())
        {
            std::int64_t from = 0;
            for (const auto& m : doc.markers)
                if (m.start > from)
                {
                    parts.push_back ({ from, m.start });
                    names.push_back ({});
                    from = m.start;
                }
            if (from > 0 && from < doc.audio.frames())
            {
                parts.push_back ({ from, doc.audio.frames() });
                names.push_back ({});
            }
        }
        if (parts.empty())
        {
            setStatus (tr ("No regions or markers to export"), true);
            return;
        }
        chooser = std::make_unique<juce::FileChooser> (tr ("Export regions to a folder"), juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        juce::Component::SafePointer<AudioEditor> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [self, parts, names] (const juce::FileChooser& fc) {
                                  if (self == nullptr || ! fc.getResult().isDirectory())
                                      return;
                                  int written = 0;
                                  for (std::size_t i = 0; i < parts.size(); ++i)
                                  {
                                      ae::Document part;
                                      part.audio = ae::copy (self->doc.audio, parts[i]);
                                      const auto name = names[i].empty() ? self->safeTitle() + " " + juce::String (i + 1).paddedLeft ('0', 2)
                                                                         : juce::File::createLegalFileName (juce::String::fromUTF8 (names[i].c_str()));
                                      const auto bytes = ae::encodeWav (part, self->bits);
                                      written += fc.getResult().getNonexistentChildFile (name, ".wav").replaceWithData (bytes.data(), bytes.size()) ? 1 : 0;
                                  }
                                  self->setStatus (tr ("{0} files written", { juce::String (written) }), false);
                              });
    }

    void sendToPlaylist()
    {
        if (doc.audio.empty())
            return;
        const auto file = writeEditFile();
        if (file == juce::File {})
            return;
        const auto& song = engine.getSong();
        const auto length = juce::jmax<std::int64_t> (ppq / 4, (std::int64_t) (doc.audio.seconds() * song.bpm / 60.0 * ppq));
        const auto start = (std::int64_t) (engine.getTransport().positionTicks.load() / ppq) * ppq; // at the beat of the song position
        // the first track with room for it
        int track = 0;
        for (;; ++track)
        {
            const auto busy = std::any_of (song.playlist.begin(), song.playlist.end(),
                                           [&] (const Clip& c) { return c.track == track && c.start < start + length && c.end() > start; });
            if (! busy)
                break;
        }
        const auto path = file.getFullPathName().toStdString();
        const auto name = safeTitle().toStdString();
        engine.editSong ([=] (Song& s) {
            ChannelDef c;
            c.name = name;
            c.instrument = InstrumentKind::audioClip;
            c.samplePath = path;
            c.mixerTrack = s.claimFreeInsert();
            s.channels.push_back (c);
            Clip clip;
            clip.kind = ClipKind::audio;
            clip.channel = (int) s.channels.size() - 1;
            clip.track = track;
            clip.start = start;
            clip.length = length;
            s.playlist.push_back (clip);
            s.mode = PlayMode::song;
        });
        modified = false;
        setStatus (tr ("Sent to the Playlist (track {0})", { juce::String (track + 1) }), false);
        updateTitle();
    }

    // FL's Edison "Send to channel" of a part (issue #12): the selection (or a region) as a new Sampler channel's
    // sample; the editor keeps the whole audio (nothing cut or cropped)
    void sendRangeToNewSampler (ae::Range range)
    {
        range = ae::clamp (doc, range);
        if (doc.audio.empty() || range.empty())
            return;
        ae::Document part;
        part.audio = ae::copy (doc.audio, range);
        part.title = doc.title;
        if (doc.hasLoop() && doc.loopStart >= range.start && doc.loopEnd <= range.end)
            ae::setLoop (part, { doc.loopStart - range.start, doc.loopEnd - range.start });
        const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("LYNX/Edits");
        folder.createDirectory();
        const auto file = folder.getNonexistentChildFile (safeTitle() + " (part)", ".wav");
        const auto bytes = ae::encodeWav (part, bits);
        if (! file.replaceWithData (bytes.data(), bytes.size()))
        {
            setStatus (tr ("Could not write {0}", { file.getFullPathName() }), true);
            return;
        }
        engine.getSampleLibrary().forget (file.getFullPathName());
        const auto path = file.getFullPathName().toStdString();
        const auto name = (safeTitle() + " (part)").toStdString();
        int index = 0;
        engine.editSong ([&] (Song& s) {
            ChannelDef c;
            c.name = name;
            c.instrument = InstrumentKind::sampler;
            c.samplePath = path;
            c.mixerTrack = s.claimFreeInsert();
            s.channels.push_back (c);
            index = (int) s.channels.size() - 1;
        });
        engine.setSelectedChannel (index);
        setStatus (tr ("The selection went to a new Sampler channel ({0})", { juce::String::fromUTF8 (name.c_str()) }), false);
    }

    void sendToNewSampler()
    {
        if (doc.audio.empty())
            return;
        const auto file = writeEditFile();
        if (file == juce::File {})
            return;
        const auto path = file.getFullPathName().toStdString();
        const auto name = safeTitle().toStdString();
        int index = 0;
        engine.editSong ([&] (Song& s) {
            ChannelDef c;
            c.name = name;
            c.instrument = InstrumentKind::sampler;
            c.samplePath = path;
            c.mixerTrack = s.claimFreeInsert();
            s.channels.push_back (c);
            index = (int) s.channels.size() - 1;
        });
        engine.setSelectedChannel (index);
        modified = false;
        setStatus (tr ("Sent to a new Sampler channel"), false);
        updateTitle();
    }

    bool sourceChannelValid() const
    {
        const auto& channels = engine.getSong().channels;
        return source.kind == Source::Kind::channel && juce::isPositiveAndBelow (source.index, (int) channels.size())
               && juce::String::fromUTF8 (channels[(std::size_t) source.index].samplePath.c_str()) == source.path;
    }

    void sendBackToChannel()
    {
        if (doc.audio.empty() || ! sourceChannelValid())
            return;
        const auto file = writeEditFile();
        if (file == juce::File {})
            return;
        const auto path = file.getFullPathName().toStdString();
        const auto ch = source.index;
        engine.editSong ([=] (Song& s) { s.channels[(std::size_t) ch].samplePath = path; });
        source.path = file.getFullPathName();
        modified = false;
        setStatus (tr ("The channel now plays the edited audio"), false);
        updateTitle();
    }

    // --- menus ----------------------------------------------------------------------------------------------
    juce::PopupMenu historyMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Undo history"));
        for (int i = history.size() - 1; i >= 0; --i)
            menu.addItem (juce::String::fromUTF8 (history.label (i).c_str()), true, i == history.getPosition(), [this, i] {
                if (recState != RecState::idle)
                    return;
                stopPlayback();
                doc = history.jumpTo (i);
                modified = true;
                documentChanged();
            });
        return menu;
    }

    juce::PopupMenu curveMenu()
    {
        juce::PopupMenu menu;
        const std::pair<ae::Curve, const char*> curves[] = { { ae::Curve::smooth, TR_NOOP ("Smooth (S-curve)") },
                                                             { ae::Curve::linear, TR_NOOP ("Linear") },
                                                             { ae::Curve::exponential, TR_NOOP ("Slow start") },
                                                             { ae::Curve::logarithmic, TR_NOOP ("Fast start") } };
        menu.addSectionHeader (tr ("Fade curve"));
        for (const auto& [curve, name] : curves)
            menu.addItem (tr (name), true, fadeCurve == curve, [this, c = curve] { fadeCurve = c; });
        return menu;
    }

    juce::PopupMenu recordMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Record from"));
        menu.addItem (tr ("Audio input"), true, recordSource == AudioCapture::input, [this] { recordSource = AudioCapture::input; });
        menu.addItem (tr ("Master output"), true, recordSource == 0, [this] { recordSource = 0; });
        juce::PopupMenu inserts;
        const auto& mixer = engine.getSong().mixer;
        for (int t = 1; t < (int) mixer.size(); ++t)
            inserts.addItem (sourceName (t), true, recordSource == t, [this, t] { recordSource = t; });
        menu.addSubMenu (tr ("Mixer insert"), inserts, inserts.getNumItems() > 0, nullptr, recordSource > 0);
        menu.addSectionHeader (tr ("Start"));
        menu.addItem (tr ("Immediately"), true, recordMode == RecordMode::now, [this] { recordMode = RecordMode::now; });
        menu.addItem (tr ("When the input is heard"), true, recordMode == RecordMode::onInput, [this] { recordMode = RecordMode::onInput; });
        menu.addItem (tr ("When the song plays (stops with it)"), true, recordMode == RecordMode::onPlay, [this] { recordMode = RecordMode::onPlay; });
        return menu;
    }

    juce::PopupMenu sendMenu()
    {
        juce::PopupMenu menu;
        const auto has = ! doc.audio.empty();
        menu.addItem (tr ("Send to the Playlist as an audio clip"), has, false, [this] { sendToPlaylist(); });
        menu.addItem (tr ("Send to a new Sampler channel"), has, false, [this] { sendToNewSampler(); });
        menu.addItem (tr ("Send the selection to a new Sampler channel"), has && ! selection.empty(), false,
                      [this] { sendRangeToNewSampler (selection); });
        if (source.kind == Source::Kind::channel)
        {
            const auto& channels = engine.getSong().channels;
            const auto name = juce::isPositiveAndBelow (source.index, (int) channels.size())
                                  ? juce::String::fromUTF8 (channels[(std::size_t) source.index].name.c_str())
                                  : juce::String();
            menu.addItem (tr ("Replace the sample of \"{0}\"", { name }), has && sourceChannelValid(), false, [this] { sendBackToChannel(); });
        }
        return menu;
    }

    juce::PopupMenu markerMenu (int index)
    {
        juce::PopupMenu menu;
        const auto m = doc.markers[(std::size_t) index];
        menu.addSectionHeader (m.name.empty() ? (m.isRegion() ? tr ("Region") : tr ("Marker")) : juce::String::fromUTF8 (m.name.c_str()));
        menu.addItem (tr ("Rename..."), [this, index] { renameMarker (index); });
        if (m.isRegion())
        {
            menu.addItem (tr ("Select"), [this, m] {
                selection = { m.start, m.end };
                repaint();
            });
            menu.addItem (tr ("Zoom on"), [this, m] { zoomTo ({ m.start, m.end }); });
            menu.addItem (tr ("Send to a new Sampler channel"), [this, m] { sendRangeToNewSampler ({ m.start, m.end }); });
            menu.addItem (tr ("Set loop"), [this, m] {
                ae::setLoop (doc, { m.start, m.end });
                commit (tr ("Set loop"));
            });
            menu.addItem (tr ("Turn into marker"), [this, index] {
                doc.markers[(std::size_t) index].end = doc.markers[(std::size_t) index].start;
                commit (tr ("Turn into marker"));
            });
        }
        menu.addItem (tr ("Delete"), [this, index] {
            doc.markers.erase (doc.markers.begin() + index);
            commit (tr ("Delete marker"));
        });
        return menu;
    }

    void renameMarker (int index)
    {
        auto* box = new juce::AlertWindow (tr ("Rename"), {}, juce::MessageBoxIconType::NoIcon, this);
        box->addTextEditor ("name", juce::String::fromUTF8 (doc.markers[(std::size_t) index].name.c_str()));
        box->addButton (tr ("Accept"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<AudioEditor> self (this);
        box->enterModalState (true, juce::ModalCallbackFunction::create ([self, box, index] (int result) {
                                  if (self != nullptr && result == 1 && juce::isPositiveAndBelow (index, (int) self->doc.markers.size()))
                                  {
                                      self->doc.markers[(std::size_t) index].name = box->getTextEditorContents ("name").toStdString();
                                      self->commit (tr ("Rename marker"));
                                  }
                              }),
                              true);
    }

    void setStatus (const juce::String& text, bool error)
    {
        status = text;
        statusIsError = error;
        repaint (infoArea);
    }

    void updateButtons()
    {
        undoButton.setEnabled (history.canUndo());
        redoButton.setEnabled (history.canRedo());
        const auto has = ! doc.audio.empty();
        for (auto* b : quickButtons)
            b->setEnabled (has);
        playButton.setEnabled (has);
        saveButton.setEnabled (has);
    }

    void updateTitle()
    {
        if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
            window->setName (tr ("Audio editor") + (doc.title.empty() ? juce::String() : " - " + juce::String::fromUTF8 (doc.title.c_str())) + (modified ? " *" : ""));
    }

    void parentHierarchyChanged() override { updateTitle(); }

    void timerCallback() override
    {
        if (recState != RecState::idle)
        {
            drainRecording();
            if (recState == RecState::recording && ++recordTicks % 6 == 0) // the view follows ~5 times a second
                showRecording();
            if (recState == RecState::idle)
                return;
            repaint();
        }
        if (playing)
        {
            const auto pos = engine.getPreviewPosition();
            if (pos < 0.0)
            {
                if (juce::Time::getMillisecondCounterHiRes() - playStartedMs > 250.0)
                    stopPlayback();
                return;
            }
            const auto before = playhead;
            playhead = playFrom + (std::int64_t) pos;
            // page along with the playhead (Follow on, issue #12)
            if (EditorGlobals::get().isFollowingView (EditorGlobals::FollowView::audioEditor)
                && ((double) playhead > viewStart + (double) waveArea.getWidth() * fpp || (double) playhead < viewStart) && fpp < maxFpp())
            {
                viewStart = (double) playhead;
                clampView();
                repaint();
            }
            else if (before != playhead)
                repaint (waveArea.withTrimmedTop (-2));
        }
    }

    AudioEngine& engine;
    Source source;
    ae::Document doc;
    ae::History history;
    std::shared_ptr<wave::Summary> summary;
    int docVersion = 0;
    bool modified = false;
    juce::File lastFile;
    ae::WavFormat bits = ae::WavFormat::int24;

    ae::Range selection;
    std::int64_t cursor = 0, anchor = 0;
    double viewStart = 0.0, fpp = 1.0;
    Drag drag = Drag::none;
    int dragMarkerIndex = -1;

    bool spectrumView = false, logFrequency = true;
    int fftSize = 2048;
    juce::Image spectrumImage;
    juce::String spectrumKey;
    TimeFormat timeFormat = TimeFormat::automatic;
    bool snapZero = false;
    ae::Curve fadeCurve = ae::Curve::smooth;

    bool playing = false, loopPlay = false;
    std::int64_t playFrom = 0, playhead = -1;
    double playStartedMs = 0.0;

    RecState recState = RecState::idle;
    bool openedInput = false; // startRecording opened the device input itself (released when it stops)
    RecordMode recordMode = RecordMode::now;
    int recordSource = AudioCapture::input;
    std::vector<float> recL, recR;
    ae::Document beforeRecording;
    std::int64_t recordAt = 0;
    double recordRate = 44100.0;
    bool recordStereo = true;
    int recordTicks = 0;

    juce::String status;
    bool statusIsError = false;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> rulerArea, waveArea, levelArea, overviewArea, infoArea;
    IconButton menuButton { "menu", Icon::panelMenu }, undoButton { "undo", Icon::undo }, redoButton { "redo", Icon::redo },
        historyButton { "history", Icon::chevronDown }, recordButton { "record", Icon::record }, recordMenuButton { "recordMenu", Icon::chevronDown },
        playButton { "play", Icon::play }, stopButton { "stop", Icon::stop }, snapButton { "snap", Icon::magnet }, fitButton { "fit", Icon::zoom },
        saveButton { "save", Icon::save }, followButton { "follow", Icon::follow };
    juce::TextButton loopButton, viewButton, sendButton;
    juce::Slider volumeKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::OwnedArray<juce::TextButton> quickButtons;

    void refreshVolumeKnob()
    {
        volumeKnob.setValue (engine.getEditorVolume(), juce::dontSendNotification);
        const auto text = tr ("Playback volume: {0} (the audio is not changed; Tools > Amp... changes it)", { ui::utilityVolumeText (engine.getEditorVolume()) });
        volumeKnob.setTooltip (text);
        ui::setHint (volumeKnob, text + "  " + tr ("[LMB] drag  [Wheel] step  [2xLMB] 100%  [RMB] type a value"), true);
    }

};

juce::PopupMenu AudioEditor::editMenu (bool contextual)
{
    juce::PopupMenu menu;
    const auto has = ! doc.audio.empty();
    const auto sel = ! selection.empty();
    const auto clip = ! clipboard().empty();
    if (! contextual)
    {
        menu.addItem (tr ("Undo") + "\tCtrl+Z", history.canUndo(), false, [this] { undo(); });
        menu.addItem (tr ("Redo") + "\tCtrl+Y", history.canRedo(), false, [this] { redo(); });
        menu.addSubMenu (tr ("Undo history"), historyMenu());
        menu.addSeparator();
    }
    menu.addItem (tr ("Cut") + "\tCtrl+X", sel, false, [this] { cutSelection(); });
    menu.addItem (tr ("Copy") + "\tCtrl+C", sel, false, [this] { copySelection(); });
    menu.addItem (tr ("Paste insert") + "\tCtrl+V", clip && selection.empty(), false, [this] { paste (false); });
    menu.addItem (tr ("Paste replace"), clip && sel, false, [this] { paste (false); });
    menu.addItem (tr ("Paste mix"), clip && has, false, [this] { paste (true); });
    menu.addSeparator();
    menu.addItem (tr ("Delete") + "\tDel", sel, false, [this] { deleteSelection(); });
    menu.addItem (tr ("Clear (silence)"), sel, false, [this] { edit (tr ("Silence"), [] (ae::Document& d, ae::Range r) { ae::clear (d, r); }, true); });
    menu.addItem (tr ("Trim (keep the selection)"), sel, false, [this] { trimSelection(); });
    // (right-click on the selection: issue #12)
    menu.addItem (tr ("Send the selection to a new Sampler channel"), sel, false, [this] { sendRangeToNewSampler (selection); });
    menu.addItem (tr ("Insert silence..."), has, false, [this] { showInsertSilence(); });
    menu.addItem (tr ("Delete the part after the loop"), has && doc.hasLoop(), false,
                  [this] { edit (tr ("Delete after loop"), [] (ae::Document& d, ae::Range) { ae::deleteAfterLoop (d); }); });
    menu.addSeparator();
    menu.addItem (tr ("Select all") + "\tCtrl+A", has, false, [this] {
        selection = { 0, doc.audio.frames() };
        repaint();
    });
    menu.addItem (tr ("Deselect") + "\tEsc", sel, false, [this] {
        selection = {};
        repaint();
    });
    if (contextual)
    {
        menu.addSeparator();
        menu.addItem (tr ("Add marker") + "\tM", has, false, [this] { addMarkerAtCursor(); });
        menu.addItem (tr ("Add region"), sel, false, [this] { addRegionFromSelection(); });
        menu.addItem (tr ("Set loop"), sel, false, [this] { setLoopFromSelection(); });
        menu.addItem (tr ("Zoom on the selection"), sel, false, [this] { zoomTo (selection); });
    }
    return menu;
}

juce::PopupMenu AudioEditor::mainMenu()
{
    const auto has = ! doc.audio.empty();
    juce::PopupMenu file;
    file.addItem (tr ("New (empty)"), [this] {
        stopPlayback();
        source = {};
        setDocument ({}, tr ("Empty"));
    });
    file.addItem (tr ("Open audio file..."), [this] { openFileDialog(); });
    juce::PopupMenu save;
    save.addItem (tr ("16-bit"), has, bits == ae::WavFormat::int16, [this] { saveAs (ae::WavFormat::int16); });
    save.addItem (tr ("24-bit"), has, bits == ae::WavFormat::int24, [this] { saveAs (ae::WavFormat::int24); });
    save.addItem (tr ("32-bit float"), has, bits == ae::WavFormat::float32, [this] { saveAs (ae::WavFormat::float32); });
    file.addSubMenu (tr ("Save as WAV"), save, has);
    file.addItem (tr ("Export regions as files..."), has && ! doc.markers.empty(), false, [this] { exportRegions(); });
    file.addSeparator();
    file.addItem (tr ("Send to the Playlist as an audio clip"), has, false, [this] { sendToPlaylist(); });
    file.addItem (tr ("Send to a new Sampler channel"), has, false, [this] { sendToNewSampler(); });

    juce::PopupMenu tools;
    tools.addSectionHeader (tr ("Level"));
    tools.addItem (tr ("Amp..."), has, false, [this] { showAmp(); });
    tools.addItem (tr ("Normalize"), has, false, [this] { edit (tr ("Normalize"), [] (ae::Document& d, ae::Range r) { ae::normalize (d, r); }); });
    tools.addItem (tr ("Normalize to..."), has, false, [this] { showNormalizeTo(); });
    tools.addItem (tr ("Reverse polarity"), has, false, [this] { edit (tr ("Reverse polarity"), [] (ae::Document& d, ae::Range r) { ae::invert (d, r); }); });
    tools.addItem (tr ("Remove DC offset (center)"), has, false, [this] { edit (tr ("Remove DC offset"), [] (ae::Document& d, ae::Range r) { ae::removeDc (d, r); }); });
    tools.addItem (tr ("Fade in"), has, false, [this] { fade (true); });
    tools.addItem (tr ("Fade out"), has, false, [this] { fade (false); });
    tools.addSubMenu (tr ("Fade curve"), curveMenu());
    tools.addItem (tr ("Declick in"), has, false, [this] { edit (tr ("Declick in"), [] (ae::Document& d, ae::Range r) { ae::declickIn (d, r); }); });
    tools.addItem (tr ("Declick out"), has, false, [this] { edit (tr ("Declick out"), [] (ae::Document& d, ae::Range r) { ae::declickOut (d, r); }); });
    tools.addSectionHeader (tr ("Time"));
    tools.addItem (tr ("Reverse"), has, false, [this] { edit (tr ("Reverse"), [] (ae::Document& d, ae::Range r) { ae::reverse (d, r); }); });
    tools.addItem (tr ("Time stretch / pitch shift..."), has, false, [this] { showStretch(); });
    tools.addItem (tr ("Resample..."), has, false, [this] { showResample(); });
    tools.addSectionHeader (tr ("Channels"));
    const auto stereo = doc.audio.numChannels() > 1;
    tools.addItem (tr ("Swap channels"), has && stereo, false, [this] { edit (tr ("Swap channels"), [] (ae::Document& d, ae::Range) { ae::swapChannels (d); }); });
    tools.addItem (tr ("Convert to mono (mix)"), has && stereo, false, [this] { edit (tr ("Convert to mono"), [] (ae::Document& d, ae::Range) { ae::toMono (d, ae::MonoSource::merged); }); });
    tools.addItem (tr ("Convert the left channel to mono"), has && stereo, false, [this] { edit (tr ("Convert to mono"), [] (ae::Document& d, ae::Range) { ae::toMono (d, ae::MonoSource::left); }); });
    tools.addItem (tr ("Convert the right channel to mono"), has && stereo, false, [this] { edit (tr ("Convert to mono"), [] (ae::Document& d, ae::Range) { ae::toMono (d, ae::MonoSource::right); }); });
    tools.addItem (tr ("Convert to stereo"), has && ! stereo, false, [this] { edit (tr ("Convert to stereo"), [] (ae::Document& d, ae::Range) { ae::toStereo (d); }); });

    juce::PopupMenu regions;
    regions.addItem (tr ("Add marker") + "\tM", has, false, [this] { addMarkerAtCursor(); });
    regions.addItem (tr ("Add region"), has && ! selection.empty(), false, [this] { addRegionFromSelection(); });
    regions.addItem (tr ("Set loop"), has && ! selection.empty(), false, [this] { setLoopFromSelection(); });
    regions.addItem (tr ("Remove loop"), doc.hasLoop(), false, [this] {
        ae::setLoop (doc, {});
        commit (tr ("Remove loop"));
    });
    regions.addItem (tr ("Delete markers in the selection"), has && ! doc.markers.empty(), false, [this] {
        if (ae::removeMarkers (doc, ae::orAll (doc, selection)) > 0)
            commit (tr ("Delete markers"));
    });
    regions.addSeparator();
    juce::PopupMenu slice;
    slice.addItem (tr ("Dull auto-slicing"), has, false, [this] { autoSlice (0.2f); });
    slice.addItem (tr ("Medium auto-slicing"), has, false, [this] { autoSlice (0.5f); });
    slice.addItem (tr ("Sharp auto-slicing"), has, false, [this] { autoSlice (0.85f); });
    slice.addSeparator();
    slice.addItem (tr ("Every beat (project tempo)"), has, false, [this] { gridSlice (1.0); });
    slice.addItem (tr ("Every 1/2 beat"), has, false, [this] { gridSlice (0.5); });
    slice.addItem (tr ("Every 1/4 beat"), has, false, [this] { gridSlice (0.25); });
    regions.addSubMenu (tr ("Auto-slice"), slice, has);

    juce::PopupMenu view;
    view.addItem (tr ("Waveform"), true, ! spectrumView, [this] { viewButton.setToggleState (false, juce::sendNotification); });
    view.addItem (tr ("Spectrogram") + "\tS", true, spectrumView, [this] { viewButton.setToggleState (true, juce::sendNotification); });
    juce::PopupMenu precision;
    for (int size : { 512, 1024, 2048, 4096, 8192 })
        precision.addItem (tr ("{0} bands", { juce::String (size / 2) }), true, fftSize == size, [this, size] {
            fftSize = size;
            repaint();
        });
    view.addSubMenu (tr ("Spectrum precision"), precision);
    view.addItem (tr ("Logarithmic frequency scale"), true, logFrequency, [this] {
        logFrequency = ! logFrequency;
        repaint();
    });
    juce::PopupMenu time;
    const std::pair<TimeFormat, const char*> formats[] = { { TimeFormat::automatic, TR_NOOP ("Auto") },
                                                           { TimeFormat::seconds, TR_NOOP ("Minutes : seconds") },
                                                           { TimeFormat::samples, TR_NOOP ("Samples") },
                                                           { TimeFormat::bars, TR_NOOP ("Bar : beat : tick") } };
    for (const auto& [f, name] : formats)
        time.addItem (tr (name), true, timeFormat == f, [this, f = f] {
            timeFormat = f;
            repaint();
        });
    view.addSubMenu (tr ("Time format"), time);
    view.addSeparator();
    view.addItem (tr ("Zoom in") + "\t+", has, false, [this] { zoomAround ((float) waveArea.getCentreX(), 0.5); });
    view.addItem (tr ("Zoom out") + "\t-", has, false, [this] { zoomAround ((float) waveArea.getCentreX(), 2.0); });
    view.addItem (tr ("Zoom out full") + "\tF", has, false, [this] { zoomToFit(); });
    view.addItem (tr ("Zoom on the selection"), has && ! selection.empty(), false, [this] { zoomTo (selection); });
    view.addItem (tr ("Snap to zero crossings"), true, snapZero, [this] { snapButton.setToggleState (! snapZero, juce::sendNotification); });

    juce::PopupMenu menu;
    menu.addSubMenu (tr ("File"), file);
    menu.addSubMenu (tr ("Edit"), editMenu (false));
    menu.addSubMenu (tr ("Tools"), tools, has);
    menu.addSubMenu (tr ("Regions"), regions, has);
    menu.addSubMenu (tr ("View"), view);
    menu.addSubMenu (tr ("Record"), recordMenu());
    return menu;
}

juce::String windowKey (const Source& s)
{
    switch (s.kind)
    {
        case Source::Kind::file: return "audioeditor:" + s.path;
        case Source::Kind::channel: return "audioeditor:channel:" + juce::String (s.index);
        case Source::Kind::insert: return "audioeditor:insert:" + juce::String (s.index);
        case Source::Kind::none: break;
    }
    return "audioeditor";
}

AudioEditor* openWindow (AudioEngine& engine, Source source)
{
    auto editor = std::make_unique<AudioEditor> (engine, source);
    auto* raw = editor.get();
    if (auto* window = openToolWindow (std::move (editor), tr ("Audio editor"), windowKey (source)))
    {
        window->setResizable (true, false);
        window->setResizeLimits (720, 380, 4000, 3000);
    }
    return raw;
}
} // namespace

void openAudioEditor (AudioEngine& engine)
{
    openWindow (engine, {});
}

void openAudioEditor (AudioEngine& engine, const juce::File& file)
{
    Source s { Source::Kind::file, -1, file.getFullPathName() };
    openWindow (engine, s)->loadFile (file);
}

void openAudioEditorForChannel (AudioEngine& engine, int channel)
{
    const auto& song = engine.getSong();
    if (! juce::isPositiveAndBelow (channel, (int) song.channels.size()))
        return;
    const auto& c = song.channels[(std::size_t) channel];
    const auto path = juce::String::fromUTF8 (c.samplePath.c_str());
    auto* editor = openWindow (engine, { Source::Kind::channel, channel, path });
    if (path.isNotEmpty())
        editor->loadFile (juce::File (path));
}

void openAudioEditorRecording (AudioEngine& engine)
{
    openWindow (engine, {})->recordInputWithSong();
}

void openAudioEditorForInsert (AudioEngine& engine, int insert)
{
    auto* editor = openWindow (engine, { Source::Kind::insert, insert, {} });
    editor->setRecordSource (insert);
}

std::unique_ptr<juce::Component> makeAudioEditorForSnapshot (AudioEngine& engine, audioedit::Document doc, bool spectrum)
{
    auto editor = std::make_unique<AudioEditor> (engine, Source {});
    const auto frames = doc.audio.frames();
    editor->setDocument (std::move (doc), tr ("Open"));
    editor->selectForSnapshot ({ frames / 4, frames / 2 }, spectrum);
    return editor;
}

juce::StringArray audioEditorMenuForSnapshot (juce::Component& component)
{
    juce::StringArray lines;
    auto* editor = dynamic_cast<AudioEditor*> (&component);
    if (editor == nullptr)
        return lines;
    std::function<void (const juce::PopupMenu&, const juce::String&)> walk = [&] (const juce::PopupMenu& menu, const juce::String& prefix) {
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        {
            const auto& item = it.getItem();
            if (item.isSeparator || item.text.isEmpty())
                continue;
            const auto text = prefix + item.text.upToFirstOccurrenceOf ("\t", false, false);
            if (item.subMenu != nullptr)
                walk (*item.subMenu, text + " > ");
            else if (! item.isSectionHeader)
                lines.add (text + (item.isEnabled ? "" : "  (disabled)"));
        }
    };
    walk (editor->mainMenu(), {});
    return lines;
}
} // namespace lynx
