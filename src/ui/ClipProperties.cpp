#include "ui/ClipProperties.h"

#include "core/PlaylistOps.h"
#include "core/RackOps.h"
#include "engine/AudioEngine.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
using namespace settingsui;

bool sameClip (const Clip& a, const Clip& b)
{
    return a.kind == b.kind && a.pattern == b.pattern && a.channel == b.channel && a.automation == b.automation && a.track == b.track
           && a.start == b.start && a.length == b.length && a.offset == b.offset;
}

// LYNX's palette as a row of swatches; the first is "automatic".
class ColourRow final : public juce::Component
{
public:
    std::function<void()> onChange;
    std::uint32_t colour = 0;

    void paint (juce::Graphics& g) override
    {
        const auto& palette = lynxPalette();
        const auto n = (int) palette.size() + 1;
        const auto w = (float) getWidth() / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const auto value = i == 0 ? 0u : palette[(std::size_t) i - 1];
            auto r = juce::Rectangle<float> (w * (float) i, 0.0f, w, (float) getHeight()).reduced (2.0f);
            if (i == 0)
            {
                g.setColour (theme::colour::bg4);
                g.fillRoundedRectangle (r, theme::radius::sm);
                g.setColour (theme::colour::textSecondary);
                g.drawLine (r.getX() + 4, r.getBottom() - 4, r.getRight() - 4, r.getY() + 4, 1.2f);
            }
            else
            {
                g.setColour (juce::Colour (value));
                g.fillRoundedRectangle (r, theme::radius::sm);
            }
            if (value == colour)
            {
                g.setColour (theme::colour::textPrimary);
                g.drawRoundedRectangle (r.expanded (1.0f), theme::radius::sm, 2.0f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto& palette = lynxPalette();
        const auto n = (int) palette.size() + 1;
        const auto i = juce::jlimit (0, n - 1, (int) ((float) e.x / ((float) getWidth() / (float) n)));
        colour = i == 0 ? 0u : palette[(std::size_t) i - 1];
        repaint();
        if (onChange)
            onChange();
    }
};

class ClipPropertiesPanel final : public juce::Component,
                                  private juce::ChangeListener
{
public:
    ClipPropertiesPanel (AudioEngine& e, int clipIndex, std::vector<int> selected, std::function<void()> after)
        : engine (e),
          index (clipIndex),
          selection (std::move (selected)),
          afterEdit (std::move (after))
    {
        if (index >= 0 && index < (int) engine.getSong().playlist.size())
            original = engine.getSong().playlist[(std::size_t) index];
        if (std::find (selection.begin(), selection.end(), index) == selection.end())
            selection = { index };

        for (auto* t : { &sourceTitle, &timeTitle, &levelsTitle, &groupTitle })
            addAndMakeVisible (t);
        sourceTitle.setText (tr ("Source"));
        timeTitle.setText (tr ("Time"));
        levelsTitle.setText (tr ("Audio"));
        groupTitle.setText (tr ("All {0} selected clips", { juce::String ((int) selection.size()) }));

        auto label = [this] (juce::Label& l, const juce::String& text) {
            styleLabel (l, text);
            addAndMakeVisible (l);
        };
        auto editor = [this] (juce::TextEditor& ed) {
            styleEditor (ed);
            ed.onReturnKey = [this] { apply(); };
            ed.onFocusLost = [this] { apply(); };
            addAndMakeVisible (ed);
        };
        auto slider = [this] (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix) {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
            s.setRange (lo, hi, step);
            s.setTextValueSuffix (suffix);
            s.onDragEnd = [this] { apply(); };
            s.onValueChange = [this, &s] { if (! s.isMouseButtonDown()) apply(); };
            addAndMakeVisible (s);
        };

        label (nameLabel, tr ("Name"));
        editor (name);
        label (colourLabel, tr ("Color"));
        colours.onChange = [this] { apply(); };
        addAndMakeVisible (colours);
        label (sourceLabel, tr ("Uses"));
        source.onChange = [this] { apply(); };
        addAndMakeVisible (source);
        label (trackLabel, tr ("Track"));
        slider (track, 1, 500, 1, {});
        track.setSliderStyle (juce::Slider::IncDecButtons);
        track.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 60, 22);
        track.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);

        label (startLabel, tr ("Start"));
        editor (start);
        start.setTooltip (tr ("bar:beat:tick (1:1:000 is the start of the song)"));
        label (lengthLabel, tr ("Length"));
        editor (length);
        length.setTooltip (tr ("bars:beats:ticks"));
        label (offsetLabel, tr ("Offset"));
        editor (offset);
        offset.setTooltip (tr ("How much of the source's start is skipped (bars:beats:ticks)"));
        muted.setButtonText (tr ("Muted"));
        muted.onClick = [this] { apply(); };
        addAndMakeVisible (muted);

        label (gainLabel, tr ("Gain"));
        slider (gain, -24.0, 12.0, 0.1, " dB");
        gain.setDoubleClickReturnValue (true, 0.0);
        label (panLabel, tr ("Pan"));
        slider (pan, -100.0, 100.0, 1.0, "%");
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setTooltip (tr ("The clip's pan (left -100% ... right 100%), on top of the channel's"));
        label (pitchLabel, tr ("Pitch"));
        slider (pitch, -24.0, 24.0, 0.01, " st");
        pitch.setDoubleClickReturnValue (true, 0.0);
        pitch.setTooltip (tr ("The audio channel's pitch in semitones (0.5 = 50 cents): every clip of this file follows it. With \"Keep length\" the length stays, else the speed changes like a tape"));
        label (fadeInLabel, tr ("Fade in"));
        slider (fadeIn, 0.0, 16.0, 0.01, " " + tr ("beats"));
        label (fadeOutLabel, tr ("Fade out"));
        slider (fadeOut, 0.0, 16.0, 0.01, " " + tr ("beats"));
        label (stretchLabel, tr ("Stretch to"));
        slider (stretch, 0.0, 256.0, 0.25, " " + tr ("beats"));
        stretch.setTooltip (tr ("0: the file plays at its own speed; otherwise the whole file lasts this many beats at the project tempo"));
        keepPitch.setButtonText (tr ("Keep length (stretch engine)"));
        keepPitch.setTooltip (tr ("On: the stretch and the pitch change separately (the stretch keeps the pitch, the pitch keeps the length). Off: resample, pitch and speed change together"));
        keepPitch.onClick = [this] { apply(); };
        addAndMakeVisible (keepPitch);

        label (groupMuteLabel, tr ("Mute"));
        groupMute.addItemList ({ tr ("Don't change"), tr ("Mute all"), tr ("Unmute all"), tr ("Flip state") }, 1);
        groupMute.setSelectedItemIndex (0, juce::dontSendNotification);
        addAndMakeVisible (groupMute);
        label (groupGainLabel, tr ("Gain"));
        groupGainMode.addItemList ({ tr ("Don't change"), tr ("Set to"), tr ("Change by") }, 1);
        groupGainMode.setSelectedItemIndex (0, juce::dontSendNotification);
        addAndMakeVisible (groupGainMode);
        groupGain.setSliderStyle (juce::Slider::LinearHorizontal);
        groupGain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
        groupGain.setRange (-24.0, 12.0, 0.1);
        groupGain.setTextValueSuffix (" dB");
        addAndMakeVisible (groupGain);
        groupApply.onClick = [this] { applyGroup(); };
        addAndMakeVisible (groupApply);

        reset.setTooltip (tr ("Back to how the clip was when this window opened"));
        reset.onClick = [this] { resetClip(); };
        close.onClick = [this] { closeToolWindowOf (*this); };
        addAndMakeVisible (reset);
        addAndMakeVisible (close);

        engine.addChangeListener (this);
        load();
        setSize (460, height());
    }

    ~ClipPropertiesPanel() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        auto bottom = area.removeFromBottom (30);
        close.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (8);
        reset.setBounds (bottom.removeFromRight (90));

        constexpr int labelWidth = 86, rowHeight = 26, gap = 6;
        auto row = [&] (juce::Label& l, juce::Component& c) {
            auto r = area.removeFromTop (rowHeight);
            l.setBounds (r.removeFromLeft (labelWidth));
            c.setBounds (r);
            area.removeFromTop (gap);
        };
        auto title = [&] (juce::Component& t) {
            t.setBounds (area.removeFromTop (24));
            area.removeFromTop (6);
        };

        title (sourceTitle);
        row (nameLabel, name);
        row (colourLabel, colours);
        row (sourceLabel, source);
        {
            auto r = area.removeFromTop (rowHeight);
            trackLabel.setBounds (r.removeFromLeft (labelWidth));
            track.setBounds (r.removeFromLeft (130));
            area.removeFromTop (gap);
        }
        title (timeTitle);
        {
            auto r = area.removeFromTop (rowHeight);
            startLabel.setBounds (r.removeFromLeft (labelWidth));
            start.setBounds (r.removeFromLeft (110));
            r.removeFromLeft (14);
            lengthLabel.setBounds (r.removeFromLeft (60));
            length.setBounds (r);
            area.removeFromTop (gap);
        }
        {
            auto r = area.removeFromTop (rowHeight);
            offsetLabel.setBounds (r.removeFromLeft (labelWidth));
            offset.setBounds (r.removeFromLeft (110));
            r.removeFromLeft (14);
            muted.setBounds (r);
            area.removeFromTop (gap);
        }
        if (isAudio())
        {
            title (levelsTitle);
            row (gainLabel, gain);
            row (panLabel, pan);
            row (pitchLabel, pitch);
            row (fadeInLabel, fadeIn);
            row (fadeOutLabel, fadeOut);
            row (stretchLabel, stretch);
            keepPitch.setBounds (area.removeFromTop (22).withTrimmedLeft (labelWidth));
            area.removeFromTop (gap);
        }
        if (selection.size() > 1)
        {
            title (groupTitle);
            row (groupMuteLabel, groupMute);
            auto r = area.removeFromTop (rowHeight);
            groupGainLabel.setBounds (r.removeFromLeft (labelWidth));
            groupGainMode.setBounds (r.removeFromLeft (120));
            r.removeFromLeft (6);
            groupApply.setBounds (r.removeFromRight (70));
            r.removeFromRight (6);
            groupGain.setBounds (r);
        }
    }

private:
    bool isAudio() const noexcept { return props.kind == ClipKind::audio; }

    int height() const
    {
        // margins + buttons, then 30 per section title and 32 per row
        auto h = 24 + 30 + 14 + 2 * 30 + 6 * 32;
        if (isAudio())
            h += 30 + 6 * 32 + 28;
        if (selection.size() > 1)
            h += 30 + 2 * 32;
        return h;
    }

    // the clip may have moved in the list (sorting, other edits): find it again
    int locate() const
    {
        const auto& list = engine.getSong().playlist;
        if (index >= 0 && index < (int) list.size() && sameClip (list[(std::size_t) index], current))
            return index;
        for (int i = 0; i < (int) list.size(); ++i)
            if (sameClip (list[(std::size_t) i], current))
                return i;
        // moved or resized in the Playlist: the same slot still holding a clip of the same source
        if (index >= 0 && index < (int) list.size())
        {
            const auto& c = list[(std::size_t) index];
            if (c.kind == current.kind && c.pattern == current.pattern && c.channel == current.channel && c.automation == current.automation)
                return index;
        }
        return -1;
    }

    void load()
    {
        const auto& song = engine.getSong();
        const auto i = locate() >= 0 ? locate() : index;
        if (i < 0 || ! clipprops::read (song, i, props))
            return;
        index = i;
        current = song.playlist[(std::size_t) i];
        loading = true;
        const auto n = juce::dontSendNotification;
        name.setText (juce::String::fromUTF8 (props.name.c_str()), false);
        colours.colour = props.colour;
        colours.repaint();
        colourLabel.setVisible (props.kind != ClipKind::automation);
        colours.setVisible (props.kind != ClipKind::automation);
        source.clear (n);
        for (const auto& [id, text] : clipprops::sourcesFor (song, props.kind))
            source.addItem (juce::String::fromUTF8 (text.c_str()), id + 1);
        source.setSelectedId (props.source + 1, n);
        track.setValue (props.track + 1, n);
        start.setText (clipprops::formatPosition (props.start, song.beatsPerBar), false);
        length.setText (clipprops::formatLength (props.length, song.beatsPerBar), false);
        offset.setText (clipprops::formatLength (props.offset, song.beatsPerBar), false);
        muted.setToggleState (props.muted, n);
        gain.setValue (juce::Decibels::gainToDecibels (props.gain, -24.0f), n);
        pan.setValue (props.pan * 100.0, n);
        pitch.setValue (props.pitch, n);
        fadeIn.setValue ((double) props.fadeIn / ppq, n);
        fadeOut.setValue ((double) props.fadeOut / ppq, n);
        stretch.setValue (props.stretchBeats, n);
        keepPitch.setToggleState (props.keepPitch, n);
        for (auto* c : std::initializer_list<juce::Component*> { &levelsTitle, &gainLabel, &gain, &panLabel, &pan, &pitchLabel, &pitch, &fadeInLabel, &fadeIn,
                                                                  &fadeOutLabel, &fadeOut, &stretchLabel, &stretch, &keepPitch })
            c->setVisible (isAudio());
        for (auto* c : std::initializer_list<juce::Component*> { &groupTitle, &groupMuteLabel, &groupMute, &groupGainLabel, &groupGainMode, &groupGain, &groupApply })
            c->setVisible (selection.size() > 1);
        loading = false;
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (tr ("Clip properties") + " - " + name.getText());
    }

    void apply()
    {
        if (loading)
            return;
        const auto i = locate();
        if (i < 0)
            return;
        const auto& song = engine.getSong();
        auto p = props;
        p.name = name.getText().trim().toStdString();
        p.colour = colours.colour;
        p.source = source.getSelectedId() - 1;
        p.track = (int) track.getValue() - 1;
        if (const auto t = clipprops::parsePosition (start.getText().toStdString(), song.beatsPerBar); t >= 0)
            p.start = t;
        if (const auto t = clipprops::parseLength (length.getText().toStdString(), song.beatsPerBar); t > 0)
            p.length = t;
        if (const auto t = clipprops::parseLength (offset.getText().toStdString(), song.beatsPerBar); t >= 0)
            p.offset = t;
        p.muted = muted.getToggleState();
        p.gain = gain.getValue() <= -24.0 ? 0.0f : juce::Decibels::decibelsToGain ((float) gain.getValue());
        p.pan = (float) (pan.getValue() / 100.0);
        p.pitch = (float) pitch.getValue();
        p.fadeIn = (std::int64_t) std::llround (fadeIn.getValue() * ppq);
        p.fadeOut = (std::int64_t) std::llround (fadeOut.getValue() * ppq);
        p.stretchBeats = (float) stretch.getValue();
        p.keepPitch = keepPitch.getToggleState();
        if (p == props)
        {
            load(); // puts back text that did not parse
            return;
        }

        engine.editSong ([i, p] (Song& s) { clipprops::apply (s, i, p); });
        current = engine.getSong().playlist[(std::size_t) i];
        index = i;
        if (afterEdit)
            afterEdit();
        load();
    }

    void applyGroup()
    {
        const auto mute = (clipprops::MuteChange) juce::jmax (0, groupMute.getSelectedItemIndex());
        const auto gainMode = groupGainMode.getSelectedItemIndex();
        const auto db = (float) groupGain.getValue();
        auto targets = selection;
        engine.editSong ([targets, mute, gainMode, db] (Song& s) {
            clipprops::changeMute (s, targets, mute);
            if (gainMode == 1)
                clipprops::changeGain (s, targets, juce::Decibels::decibelsToGain (db), false);
            else if (gainMode == 2)
                clipprops::changeGain (s, targets, juce::Decibels::decibelsToGain (db), true);
        });
        if (afterEdit)
            afterEdit();
        load();
    }

    void resetClip()
    {
        const auto i = locate();
        if (i < 0)
            return;
        const auto back = original;
        engine.editSong ([i, back] (Song& s) { s.playlist[(std::size_t) i] = back; });
        current = original;
        if (afterEdit)
            afterEdit();
        load();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (locate() < 0)
        {
            closeToolWindowOf (*this); // the clip is gone (deleted, undone)
            return;
        }
        clipprops::Props now;
        if (clipprops::read (engine.getSong(), locate(), now) && ! (now == props))
            load(); // changed elsewhere (undo, the Playlist): show it; otherwise leave what is being typed alone
    }

    AudioEngine& engine;
    int index = -1;
    std::vector<int> selection;
    std::function<void()> afterEdit;
    Clip original, current;
    clipprops::Props props;
    bool loading = false;

    SectionTitle sourceTitle, timeTitle, levelsTitle, groupTitle;
    juce::Label nameLabel, colourLabel, sourceLabel, trackLabel, startLabel, lengthLabel, offsetLabel;
    juce::Label gainLabel, panLabel, pitchLabel, fadeInLabel, fadeOutLabel, stretchLabel, groupMuteLabel, groupGainLabel;
    juce::TextEditor name, start, length, offset;
    ColourRow colours;
    juce::ComboBox source, groupMute, groupGainMode;
    juce::Slider track, gain, pan, pitch, fadeIn, fadeOut, stretch, groupGain;
    juce::ToggleButton muted, keepPitch;
    juce::TextButton groupApply { tr ("Apply") }, reset { tr ("Reset") }, close { tr ("Close") };
};

// --- Arrangements ---------------------------------------------------------------------------------------------------
class ArrangementsPanel final : public juce::Component,
                                private juce::ListBoxModel,
                                private juce::ChangeListener
{
public:
    explicit ArrangementsPanel (AudioEngine& e) : engine (e)
    {
        listTitle.setText (tr ("Arrangements"));
        mergeTitle.setText (tr ("Merge into the current arrangement"));
        addAndMakeVisible (listTitle);
        addAndMakeVisible (mergeTitle);

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        list.setColour (juce::ListBox::outlineColourId, theme::colour::border);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        auto button = [this] (juce::TextButton& b, const juce::String& tip, std::function<void()> fn) {
            b.setTooltip (tip);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };
        button (selectButton, tr ("Make it the current arrangement (the Playlist shows it)"), [this] {
            const auto row = list.getSelectedRow();
            if (row >= 0)
                engine.editSong ([row] (Song& s) { s.selectArrangement (row); }, false);
        });
        button (addButton, tr ("A new empty arrangement"), [this] {
            engine.editSong ([] (Song& s) { s.addArrangement ("Arrangement " + std::to_string (s.arrangements.size() + 1)); });
            list.selectRow (engine.getSong().currentArrangement);
        });
        button (duplicateButton, tr ("A copy of the selected arrangement, right after it"), [this] {
            const auto row = list.getSelectedRow();
            int made = -1;
            engine.editSong ([row, &made] (Song& s) { made = arrangeops::duplicate (s, row); });
            if (made >= 0)
                list.selectRow (made);
        });
        button (renameButton, {}, [this] { rename(); });
        button (removeButton, {}, [this] {
            const auto row = list.getSelectedRow();
            if (row >= 0 && engine.getSong().arrangements.size() > 1)
                engine.editSong ([row] (Song& s) { s.removeArrangement (row); });
        });
        button (upButton, tr ("Move up"), [this] { moveBy (-1); });
        button (downButton, tr ("Move down"), [this] { moveBy (1); });

        auto label = [this] (juce::Label& l, const juce::String& text) {
            styleLabel (l, text);
            addAndMakeVisible (l);
        };
        label (sourceLabel, tr ("Source arrangement"));
        addAndMakeVisible (sourceBox);
        label (modeLabel, tr ("Mode"));
        modeBox.addItemList ({ tr ("Insert (moves what follows)"), tr ("Merge (over what is there)"), tr ("Replace (clears that length first)") }, 1);
        modeBox.setSelectedItemIndex (0, juce::dontSendNotification);
        addAndMakeVisible (modeBox);
        label (positionLabel, tr ("Position"));
        positionBox.addItemList ({ tr ("Start"), tr ("Playback position"), tr ("End") }, 1);
        positionBox.setSelectedItemIndex (2, juce::dontSendNotification);
        addAndMakeVisible (positionBox);
        mergeButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        mergeButton.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        mergeButton.onClick = [this] { mergeNow(); };
        addAndMakeVisible (mergeButton);
        closeButton.onClick = [this] { closeToolWindowOf (*this); };
        addAndMakeVisible (closeButton);

        engine.addChangeListener (this);
        refresh();
        list.selectRow (engine.getSong().currentArrangement);
        setSize (520, 480);
    }

    ~ArrangementsPanel() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        closeButton.setBounds (area.removeFromBottom (30).removeFromRight (90));
        area.removeFromBottom (10);

        auto merge = area.removeFromBottom (24 + 6 + 3 * 32 + 30);
        mergeTitle.setBounds (merge.removeFromTop (24));
        merge.removeFromTop (6);
        auto row = [&merge] (juce::Label& l, juce::Component& c) {
            auto r = merge.removeFromTop (26);
            l.setBounds (r.removeFromLeft (132));
            c.setBounds (r);
            merge.removeFromTop (6);
        };
        row (sourceLabel, sourceBox);
        row (modeLabel, modeBox);
        row (positionLabel, positionBox);
        mergeButton.setBounds (merge.removeFromTop (28).removeFromRight (128));
        area.removeFromBottom (12);

        listTitle.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
        auto buttons = area.removeFromRight (110);
        area.removeFromRight (10);
        list.setBounds (area);
        for (auto* b : { &selectButton, &addButton, &duplicateButton, &renameButton, &removeButton, &upButton, &downButton })
        {
            b->setBounds (buttons.removeFromTop (26));
            buttons.removeFromTop (4);
        }
    }

private:
    int getNumRows() override { return (int) engine.getSong().arrangements.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        const auto& song = engine.getSong();
        if (row < 0 || row >= (int) song.arrangements.size())
            return;
        if (selected)
        {
            g.setColour (theme::colour::bg5);
            g.fillRect (0, 0, width, height);
        }
        const auto current = row == song.currentArrangement;
        if (current)
        {
            g.setColour (theme::colour::accent);
            g.fillRect (0, 0, 3, height);
        }
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::body, current));
        g.drawText (juce::String::fromUTF8 (song.arrangements[(std::size_t) row].name.c_str()), 12, 0, width - 160, height, juce::Justification::centredLeft, true);
        const auto bar = (std::int64_t) ppq * juce::jmax (1, song.beatsPerBar);
        const auto bars = (arrangeops::lengthOf (song, row) + bar - 1) / bar;
        const auto clips = (int) arrangeops::clipsOf (song, row).size();
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::small));
        g.drawText (tr ("{0} clips, {1} bars", { juce::String (clips), juce::String ((int) bars) }), width - 150, 0, 140, height,
                    juce::Justification::centredRight);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        engine.editSong ([row] (Song& s) { s.selectArrangement (row); }, false);
    }

    void selectedRowsChanged (int) override { refreshButtons(); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        list.updateContent();
        list.repaint();
        const auto& song = engine.getSong();
        const auto previous = sourceBox.getSelectedId();
        sourceBox.clear (juce::dontSendNotification);
        for (int i = 0; i < (int) song.arrangements.size(); ++i)
            if (i != song.currentArrangement)
                sourceBox.addItem (juce::String::fromUTF8 (song.arrangements[(std::size_t) i].name.c_str()), i + 1);
        if (previous > 0 && previous - 1 != song.currentArrangement && previous <= (int) song.arrangements.size())
            sourceBox.setSelectedId (previous, juce::dontSendNotification);
        else if (sourceBox.getNumItems() > 0)
            sourceBox.setSelectedItemIndex (0, juce::dontSendNotification);
        mergeButton.setEnabled (sourceBox.getNumItems() > 0);
        mergeTitle.setText (tr ("Merge into \"{0}\"", { juce::String::fromUTF8 (song.arrangements[(std::size_t) juce::jlimit (0, (int) song.arrangements.size() - 1, song.currentArrangement)].name.c_str()) }));
        refreshButtons();
    }

    void refreshButtons()
    {
        const auto row = list.getSelectedRow();
        const auto count = (int) engine.getSong().arrangements.size();
        selectButton.setEnabled (row >= 0 && row != engine.getSong().currentArrangement);
        duplicateButton.setEnabled (row >= 0);
        renameButton.setEnabled (row >= 0);
        removeButton.setEnabled (row >= 0 && count > 1);
        upButton.setEnabled (row > 0);
        downButton.setEnabled (row >= 0 && row < count - 1);
    }

    void moveBy (int delta)
    {
        const auto row = list.getSelectedRow();
        const auto to = row + delta;
        if (row < 0 || to < 0 || to >= (int) engine.getSong().arrangements.size())
            return;
        engine.editSong ([row, to] (Song& s) { arrangeops::move (s, row, to); });
        list.selectRow (to);
    }

    void rename()
    {
        const auto row = list.getSelectedRow();
        if (row < 0)
            return;
        auto* w = new juce::AlertWindow (tr ("Rename arrangement"), {}, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", juce::String::fromUTF8 (engine.getSong().arrangements[(std::size_t) row].name.c_str()));
        w->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* engineRef = &engine;
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w, row, engineRef] (int result) {
            if (result == 1)
            {
                const auto text = w->getTextEditorContents ("name").trim().toStdString();
                engineRef->editSong ([row, text] (Song& s) { arrangeops::rename (s, row, text); });
            }
        }), true);
    }

    void mergeNow()
    {
        const auto source = sourceBox.getSelectedId() - 1;
        if (source < 0)
            return;
        const auto mode = (arrangeops::MergeMode) juce::jmax (0, modeBox.getSelectedItemIndex());
        const auto position = (arrangeops::MergePosition) juce::jmax (0, positionBox.getSelectedItemIndex());
        const auto at = arrangeops::positionTick (engine.getSong(), position, (std::int64_t) engine.getTransport().positionTicks.load());
        engine.editSong ([source, mode, at] (Song& s) { arrangeops::merge (s, source, mode, at); });
    }

    AudioEngine& engine;
    SectionTitle listTitle, mergeTitle;
    juce::ListBox list;
    juce::TextButton selectButton { tr ("Show") }, addButton { tr ("New") }, duplicateButton { tr ("Duplicate") }, renameButton { tr ("Rename...") },
        removeButton { tr ("Remove") }, upButton { tr ("Move up") }, downButton { tr ("Move down") };
    juce::Label sourceLabel, modeLabel, positionLabel;
    juce::ComboBox sourceBox, modeBox, positionBox;
    juce::TextButton mergeButton { tr ("Merge") }, closeButton { tr ("Close") };
};
} // namespace

void showClipProperties (AudioEngine& engine, int clip, std::vector<int> selection, std::function<void()> afterEdit)
{
    if (clip < 0 || clip >= (int) engine.getSong().playlist.size())
        return;
    auto panel = std::make_unique<ClipPropertiesPanel> (engine, clip, std::move (selection), std::move (afterEdit));
    auto* raw = panel.get();
    openToolWindow (std::move (panel), tr ("Clip properties"), "clip-properties");
    raw->resized();
}

void showArrangements (AudioEngine& engine, bool)
{
    openToolWindow (std::make_unique<ArrangementsPanel> (engine), tr ("Arrangements"), "arrangements");
}

std::unique_ptr<juce::Component> makeClipPropertiesPanel (AudioEngine& engine, int clip, std::vector<int> selection)
{
    return std::make_unique<ClipPropertiesPanel> (engine, clip, std::move (selection), std::function<void()> {});
}

std::unique_ptr<juce::Component> makeArrangementsPanel (AudioEngine& engine)
{
    return std::make_unique<ArrangementsPanel> (engine);
}
} // namespace lynx
