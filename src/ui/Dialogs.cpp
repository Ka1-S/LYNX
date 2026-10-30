#include "ui/Dialogs.h"

#include <functional>

#include <set>

#include "core/ClipOps.h"
#include "core/RackOps.h"
#include "engine/AudioEngine.h"
#include "engine/MidiFileIo.h"
#include "ui/ColourPicker.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
// a row of swatches: "automatic" first, then the palette
class Swatches final : public juce::Component
{
public:
    explicit Swatches (std::uint32_t c) : colour (c) { setSize (13 * (size + gap), size + 4); }

    std::uint32_t colour;
    std::function<void()> onPick; // a swatch was clicked (or F2)

    void paint (juce::Graphics& g) override
    {
        const auto& palette = lynxPalette();

        for (int i = 0; i <= (int) palette.size(); ++i)
        {
            const auto r = cell (i).toFloat();
            const auto value = i == 0 ? 0u : palette[(std::size_t) i - 1];

            if (i == 0)
            {
                g.setColour (theme::colour::bg4);
                g.fillRoundedRectangle (r, theme::radius::sm);
                g.setColour (theme::colour::textSecondary);
                g.drawLine (r.getX() + 5.0f, r.getBottom() - 5.0f, r.getRight() - 5.0f, r.getY() + 5.0f, 1.2f);
            }
            else
            {
                g.setColour (juce::Colour (value));
                g.fillRoundedRectangle (r, theme::radius::sm);
            }

            if (value == colour)
            {
                g.setColour (theme::colour::textPrimary);
                g.drawRoundedRectangle (r.expanded (1.5f), theme::radius::md, 2.0f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto& palette = lynxPalette();

        for (int i = 0; i <= (int) palette.size(); ++i)
            if (cell (i).expanded (1).contains (e.getPosition()))
            {
                colour = i == 0 ? 0u : palette[(std::size_t) i - 1];
                repaint();
                if (onPick)
                    onPick();
            }
    }

    void pickRandom()
    {
        colour = randomPaletteColour (colour, (std::uint32_t) juce::Time::getMillisecondCounter());
        repaint();
        if (onPick)
            onPick();
    }

private:
    static constexpr int size = 22, gap = 6;
    juce::Rectangle<int> cell (int i) const { return { 2 + i * (size + gap), 2, size, size }; }
};

class NameAndColourWindow final : public juce::AlertWindow
{
public:
    NameAndColourWindow (const juce::String& title, const juce::String& name, std::uint32_t colour)
        : juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon), swatches (colour)
    {
        addTextEditor ("name", name, tr ("Name"));
        addCustomComponent (&swatches);
        hint.setText (tr ("Click a color, pick any color below, or press F2 for a random one"), juce::dontSendNotification);
        hint.setFont (theme::font (theme::type::small));
        hint.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        hint.setSize (swatches.getWidth(), 18);
        addCustomComponent (&hint);
        // any colour: wheel, H / S / V and hex (round 162d, issue #9)
        picker.setSize (swatches.getWidth(), picker.preferredHeight());
        picker.setOriginal (colour != 0 ? juce::Colour (colour) : theme::colour::bg4);
        if (colour != 0)
            picker.setColour (juce::Colour (colour), false);
        picker.onChange = [this] (juce::Colour c) {
            swatches.colour = c.getARGB();
            swatches.repaint();
        };
        swatches.onPick = [this] {
            if (swatches.colour != 0)
                picker.setColour (juce::Colour (swatches.colour), false);
        };
        addCustomComponent (&picker);
        addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

        if (auto* editor = getTextEditor ("name"))
            editor->selectAll();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::F2Key)
        {
            swatches.pickRandom();
            return true;
        }

        return juce::AlertWindow::keyPressed (key);
    }

    std::uint32_t colour() const { return swatches.colour; }

private:
    Swatches swatches;
    juce::Label hint;
    ui::ColourPicker picker { juce::Colours::grey, true };
};

// One big choice of the record dialog: a title line and a note below it.
class RecordChoiceButton final : public juce::Button
{
public:
    RecordChoiceButton (const juce::String& title, const juce::String& note)
        : juce::Button ({}), noteText (note) // (no component name: the alert window would print it above)
    {
        setButtonText (title);
        setSize (380, 46);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        using namespace theme;
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (down ? colour::bg5 : over ? colour::bg4 : colour::bg3);
        g.fillRoundedRectangle (r, radius::md);
        g.setColour (over ? colour::accent : colour::border);
        g.drawRoundedRectangle (r.reduced (0.5f), radius::md, 1.0f);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::body, true));
        g.drawText (getButtonText(), getLocalBounds().reduced (12, 0).withTrimmedTop (5).withHeight (18), juce::Justification::centredLeft, true);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::small));
        g.drawText (noteText, getLocalBounds().reduced (12, 0).withTrimmedTop (24).withHeight (16), juce::Justification::centredLeft, true);
    }

private:
    juce::String noteText;
};

class RecordChoiceWindow final : public juce::AlertWindow
{
public:
    RecordChoiceWindow()
        : juce::AlertWindow (tr ("Recording"), tr ("What would you like to record?"), juce::MessageBoxIconType::NoIcon)
    {
        int result = recordChoiceAudio;
        for (auto* b : { &audio, &notes, &everything, &editor })
        {
            b->onClick = [this, result] { exitModalState (result); };
            addCustomComponent (b);
            ++result;
        }
        dontAsk.setButtonText (tr ("Don't ask again (right-click the record button for the recording filter)"));
        dontAsk.setSize (380, 24);
        addCustomComponent (&dontAsk);
        addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    }

    bool dontAskAgain() const { return dontAsk.getToggleState(); }

private:
    RecordChoiceButton audio { tr ("Audio, into the Playlist as an audio clip"),
                               tr ("Arms the selected mixer insert; choose its audio input (microphone) if it has none") },
        notes { tr ("Notes and automation"), tr ("Notes played on a MIDI keyboard or the PC keyboard, and the controls you move") },
        everything { tr ("Everything"), tr ("Notes, automation, and audio from the armed inserts") },
        editor { tr ("Audio, into the audio editor"), tr ("Records the audio input into a new audio editor while the song plays") };
    juce::ToggleButton dontAsk;
};
} // namespace

std::unique_ptr<juce::AlertWindow> makeRecordChoiceDialog()
{
    return std::make_unique<RecordChoiceWindow>();
}

namespace
{
// FL's "Import MIDI data" window (REA rounds 143 / 146b, TMIDIImportForm): its options as LYNX can apply them
class MidiChannelFilter final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    std::uint16_t mask = 0xFFFF;

    MidiChannelFilter() { setSize (240, 44); setTooltip (tr ("Deselect MIDI channels to filter out (right-click: only this one)")); }

    // FL's grid: two rows of eight (REA round 149 [EXEC])
    juce::Rectangle<float> cell (int c) const
    {
        const auto w = getWidth() / 8.0f, h = getHeight() / 2.0f;
        return { (float) (c % 8) * w, (float) (c / 8) * h, w, h };
    }

    void paint (juce::Graphics& g) override
    {
        for (int c = 0; c < 16; ++c)
        {
            const auto r = cell (c).reduced (1.5f);
            const auto on = (mask >> c & 1) != 0;
            g.setColour (on ? theme::colour::accent : theme::colour::bg3);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (on ? theme::colour::textPrimary : theme::colour::textSecondary);
            g.setFont (theme::font (theme::type::caption));
            g.drawText (juce::String (c + 1), r, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto c = juce::jlimit (0, 15, juce::jlimit (0, 7, (int) (e.x / (getWidth() / 8.0f))) + (e.y >= getHeight() / 2 ? 8 : 0));
        if (e.mods.isPopupMenu() || e.mods.isCtrlDown())
            mask = mask == (std::uint16_t) (1u << c) ? (std::uint16_t) 0xFFFF : (std::uint16_t) (1u << c); // solo / all
        else
            mask ^= (std::uint16_t) (1u << c);
        repaint();
    }
};

class MidiImportWindow final : public juce::AlertWindow
{
public:
    MidiImportWindow (const juce::String& fileName, const midiimport::ParsedMidi& midi, const midiimport::Options& o, bool pianoRoll)
        : juce::AlertWindow (tr ("Import MIDI data"), fileName, juce::MessageBoxIconType::NoIcon),
          roll (pianoRoll),
          kept (o)
    {
        auto add = [this] (juce::ToggleButton& b, const juce::String& text, const juce::String& hint, bool on) {
            b.setButtonText (text);
            b.setTooltip (hint);
            b.setToggleState (on, juce::dontSendNotification);
            content.addAndMakeVisible (b);
            optionBoxes.push_back (&b);
        };
        // FL's two columns of options (REA round 149 [EXEC]): new project / realign / time signatures, then one channel
        // per track / mixer tracks / zero velocity notes
        if (pianoRoll)
        {
            add (blend, tr ("Blend with existing data"), tr ("Don't delete the channel's notes before importing"), o.blend);
            add (realign, tr ("Realign events"), tr ("Removes any silent part at the beginning"), o.realign);
            add (zeroVelocity, tr ("Import zero velocity notes"), tr ("Don't treat note on messages with zero velocity as note off"), o.zeroVelocityNotes);
        }
        else
        {
            add (newProject, tr ("Start new project"), tr ("Start a blank project, or add to the open one"), o.newProject);
            add (realign, tr ("Realign events"), tr ("Removes any silent part at the beginning"), o.realign);
            add (timeSignatures, tr ("Import time signatures"), tr ("The file's time signature becomes the new project's"), o.timeSignatures);
            add (perTrack, tr ("Create one channel per track"), tr ("One channel per track, or per MIDI channel"), o.perTrack);
            add (mixerTracks, tr ("Set mixer tracks for new channels"), tr ("Route new channels to consecutive mixer tracks"), o.mixerTracks);
            add (zeroVelocity, tr ("Import zero velocity notes"), tr ("Don't treat note on messages with zero velocity as note off"), o.zeroVelocityNotes);
        }

        // Which tracks to import?
        tracks.addItem (tr ("All tracks"), 1);
        trackIds.push_back (-1);
        for (const auto t : midiimport::tracksWithContent (midi))
        {
            const auto& name = t < (int) midi.trackNames.size() ? midi.trackNames[(std::size_t) t] : std::string();
            tracks.addItem (juce::String (t + 1) + ". " + (name.empty() ? tr ("Track {0}", { juce::String (t + 1) }) : juce::String::fromUTF8 (name.c_str())),
                            (int) trackIds.size() + 1);
            trackIds.push_back (t);
        }
        tracks.setSelectedId (1, juce::dontSendNotification);
        for (std::size_t i = 0; i < trackIds.size(); ++i)
            if (trackIds[i] == o.track)
                tracks.setSelectedId ((int) i + 1, juce::dontSendNotification);

        // Which channels to import?
        channels.mask = o.channels;

        // Channel type (FL: FLEX / MIDI Out…; LYNX: its synth or a MIDI Out on the note's MIDI channel)
        channelType.addItem (tr ("LYNX SHARE synth"), 1);
        channelType.addItem (tr ("MIDI Out"), 2);
        channelType.setSelectedId (o.midiOut ? 2 : 1, juce::dontSendNotification);
        channelType.setTooltip (tr ("MIDI Out channels play the plugins on their MIDI port, on the note's MIDI channel"));

        // Which controller to import? (the ones LYNX applies: 7 / 11 volume, 10 pan, when the file has them)
        controller.addItem (tr ("None"), 1);
        controllerIds.push_back (-1);
        std::set<int> present;
        for (const auto& e : midi.events)
            if ((e.status & 0xF0) == 0xB0)
                present.insert (e.data1);
        for (const auto& [cc, label] : { std::pair<int, const char*> { 7, TR_NOOP ("CC 7 (volume) → channel volume") },
                                         { 11, TR_NOOP ("CC 11 (expression) → channel volume") }, { 10, TR_NOOP ("CC 10 (pan) → channel pan") } })
            if (present.count (cc) > 0 || cc == o.controller)
            {
                controller.addItem (tr (label), (int) controllerIds.size() + 1);
                controllerIds.push_back (cc);
            }
        controller.setSelectedId (1, juce::dontSendNotification);
        for (std::size_t i = 0; i < controllerIds.size(); ++i)
            if (controllerIds[i] == o.controller)
                controller.setSelectedId ((int) i + 1, juce::dontSendNotification);
        controller.setTooltip (tr ("Which controller to import? (into the pattern's events)"));

        // FL's order: the selections on top (label on the left, the control on the right), the options below
        auto label = [this] (const juce::String& text) {
            auto l = std::make_unique<juce::Label> (juce::String(), text);
            l->setFont (theme::font (theme::type::body));
            l->setJustificationType (juce::Justification::centredRight);
            content.addAndMakeVisible (*l);
            labels.push_back (std::move (l));
        };
        label (tr ("Which tracks to import?"));
        content.addAndMakeVisible (tracks);
        label (tr ("Which channels to import?"));
        content.addAndMakeVisible (channels);
        if (! pianoRoll)
        {
            label (tr ("Channel type"));
            content.addAndMakeVisible (channelType);
        }
        label (tr ("Which controller?"));
        content.addAndMakeVisible (controller);

        content.onLayout = [this] { layout(); };
        const auto rows = pianoRoll ? 3 : 4;
        content.setSize (440, rows * 30 + 22 + ((int) optionBoxes.size() + 1) / 2 * 26 + 12);
        addCustomComponent (&content);

        addButton (tr ("Accept"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    }

    midiimport::Options options() const
    {
        auto o = kept;
        if (roll)
            o.blend = blend.getToggleState();
        else
        {
            o.newProject = newProject.getToggleState();
            o.perTrack = perTrack.getToggleState();
            o.timeSignatures = timeSignatures.getToggleState();
            o.mixerTracks = mixerTracks.getToggleState();
            o.midiOut = channelType.getSelectedId() == 2;
        }
        o.realign = realign.getToggleState();
        o.zeroVelocityNotes = zeroVelocity.getToggleState();
        const auto t = tracks.getSelectedId() - 1;
        o.track = t >= 0 && t < (int) trackIds.size() ? trackIds[(std::size_t) t] : -1;
        o.channels = channels.mask;
        const auto c = controller.getSelectedId() - 1;
        o.controller = c >= 0 && c < (int) controllerIds.size() ? controllerIds[(std::size_t) c] : -1;
        return o;
    }

private:
    struct Content final : public juce::Component
    {
        std::function<void()> onLayout;
        void resized() override
        {
            if (onLayout)
                onLayout();
        }
    };

    void layout()
    {
        auto area = content.getLocalBounds();
        std::size_t l = 0;
        auto row = [&] (juce::Component& c, int height) {
            auto r = area.removeFromTop (height);
            area.removeFromTop (6);
            if (l < labels.size())
                labels[l++]->setBounds (r.removeFromLeft (190));
            r.removeFromLeft (8);
            c.setBounds (r);
        };
        row (tracks, 24);
        row (channels, 44);
        if (! roll)
            row (channelType, 24);
        row (controller, 24);
        area.removeFromTop (6);
        const auto half = (int) (optionBoxes.size() + 1) / 2;
        auto left = area.removeFromLeft (area.getWidth() / 2);
        for (std::size_t i = 0; i < optionBoxes.size(); ++i)
            optionBoxes[i]->setBounds (((int) i < half ? left : area).removeFromTop (26));
    }

    bool roll = false;
    midiimport::Options kept;
    Content content;
    juce::ToggleButton newProject, perTrack, realign, timeSignatures, mixerTracks, zeroVelocity, blend;
    std::vector<juce::ToggleButton*> optionBoxes;
    juce::ComboBox tracks, controller, channelType;
    std::vector<int> trackIds, controllerIds;
    MidiChannelFilter channels;
    std::vector<std::unique_ptr<juce::Label>> labels;
};
} // namespace

void showMidiImportDialog (const juce::String& fileName, const midiimport::ParsedMidi& midi, const midiimport::Options& current, bool pianoRoll,
                           std::function<void (const midiimport::Options&)> onAccept)
{
    auto* window = new MidiImportWindow (fileName, midi, current, pianoRoll);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, onAccept] (int result) {
                                 if (result == 1)
                                     onAccept (window->options());
                             }),
                             true);
    keepAboveMainWindow (*window);
}

std::unique_ptr<juce::AlertWindow> makeMidiImportDialog()
{
    midiimport::ParsedMidi midi;
    midi.trackNames = { "", "Piano", "Strings" };
    midi.events = { { 1, 0, 0x90, 60, 100 }, { 1, 0, 0xB0, 7, 100 }, { 2, 0, 0x91, 64, 90 } };
    return std::make_unique<MidiImportWindow> ("song.mid", midi, midiimport::Options {}, false);
}

void showRecordChoiceDialog (std::function<void (int, bool)> onChoice)
{
    auto* window = new RecordChoiceWindow();
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, onChoice] (int result) {
                                 if (result > 0)
                                     onChoice (result, window->dontAskAgain());
                             }),
                             true);
    keepAboveMainWindow (*window);
}

std::unique_ptr<juce::AlertWindow> makeNameAndColourDialog (const juce::String& title, const juce::String& name, std::uint32_t colour)
{
    return std::make_unique<NameAndColourWindow> (title, name, colour);
}

void showNameAndColourDialog (const juce::String& title, const juce::String& name, std::uint32_t colour,
                              std::function<void (const juce::String&, std::uint32_t)> onDone)
{
    auto* window = new NameAndColourWindow (title, name, colour);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, onDone] (int result) {
                                 const auto typed = window->getTextEditorContents ("name").trim();

                                 if (result == 1 && typed.isNotEmpty())
                                     onDone (typed, window->colour());
                             }),
                             true);
    keepAboveMainWindow (*window);
}

namespace
{
class SamplePitchWindow final : public juce::AlertWindow
{
public:
    SamplePitchWindow (float pitch, bool keepLength, bool audioClip)
        : juce::AlertWindow (tr ("Pitch"),
                             audioClip ? tr ("Transposes this audio clip's file (every clip of it follows).")
                                       : tr ("Transposes the sample. The Piano Roll's keys still play it chromatically around its root note."),
                             juce::MessageBoxIconType::NoIcon)
    {
        const auto semitones = (int) std::lround (pitch);
        const auto cents = (int) std::lround ((pitch - (float) semitones) * 100.0f);
        addTextEditor ("semitones", juce::String (semitones), tr ("Semitones (-24 to +24)"));
        addTextEditor ("cents", juce::String (cents), tr ("Cents (-100 to +100)"));
        keep.setButtonText (tr ("Keep length (stretch engine)"));
        keep.setToggleState (keepLength, juce::dontSendNotification);
        keep.setSize (340, 24);
        addCustomComponent (&keep);
        note.setText (tr ("Off: resample, like a tape: higher is shorter, lower is longer."), juce::dontSendNotification);
        note.setFont (theme::font (theme::type::small));
        note.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        note.setSize (340, 18);
        addCustomComponent (&note);
        addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        addButton (tr ("Reset"), 2);
        addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (auto* editor = getTextEditor ("semitones"))
            editor->selectAll();
    }

    // semitones + cents/100, clamped like the pitch knob
    double pitch()
    {
        const auto number = [this] (const char* id) {
            return getTextEditorContents (id).trim().replaceCharacter (',', '.').retainCharacters ("+-0123456789.").getDoubleValue();
        };
        return juce::jlimit (-24.0, 24.0, juce::jlimit (-24.0, 24.0, number ("semitones")) + juce::jlimit (-100.0, 100.0, number ("cents")) / 100.0);
    }

    bool keepLength() const { return keep.getToggleState(); }

private:
    juce::ToggleButton keep;
    juce::Label note;
};
} // namespace

std::unique_ptr<juce::AlertWindow> makeSamplePitchDialog (float pitch, bool keepLength, bool audioClip)
{
    return std::make_unique<SamplePitchWindow> (pitch, keepLength, audioClip);
}

void showSamplePitchDialog (AudioEngine& engine, int channel)
{
    const auto& channels = engine.getSong().channels;
    if (channel < 0 || channel >= (int) channels.size())
        return;
    const auto& c = channels[(std::size_t) channel];
    auto* window = new SamplePitchWindow (c.sampler.pitch, c.sampler.keepPitch, c.instrument == InstrumentKind::audioClip);
    const auto path = juce::String::fromUTF8 (c.samplePath.c_str());
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, &engine, channel, path] (int result) {
                                 if (result == 0)
                                     return;
                                 const auto pitch = result == 2 ? 0.0 : window->pitch();
                                 const auto keep = window->keepLength();
                                 const auto sample = path.isNotEmpty() ? engine.getSampleLibrary().load (path) : nullptr;
                                 const auto frames = sample != nullptr ? (double) sample->frames() : 0.0;
                                 const auto rate = sample != nullptr ? sample->sampleRate : 0.0;
                                 engine.editSong ([channel, pitch, keep, frames, rate] (Song& s) {
                                     clipops::setChannelPitch (s, channel, pitch, 0.0, keep, frames, rate);
                                 });
                             }),
                             true);
    keepAboveMainWindow (*window);
}
} // namespace lynx
