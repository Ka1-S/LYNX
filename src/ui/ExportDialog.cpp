#include "ui/ExportDialog.h"

#include "core/PlaylistOps.h"
#include "core/HardwareInsert.h"
#include "platform/Paths.h"
#include "ui/WindowOwner.h"

#include "engine/Exporter.h"
#include "engine/Text.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"

namespace lynx
{
namespace
{
class ExportThread final : public juce::ThreadWithProgressWindow
{
public:
    // onDone (on the UI thread): if empty, shows the result in a message box
    ExportThread (std::unique_ptr<ExportJob> j, juce::File f, std::function<void (bool)> done = {},
                  const juce::String& title = tr ("Exporting audio..."))
        : juce::ThreadWithProgressWindow (title, true, true),
          job (std::move (j)),
          file (std::move (f)),
          onDone (std::move (done))
    {
    }

    void run() override
    {
        ok = job->run ([this] (double p) {
            setProgress (p);
            return ! threadShouldExit();
        }, error);
    }

    void threadComplete (bool userPressedCancel) override
    {
        if (onDone)
        {
            if (! ok && ! userPressedCancel)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Could not render"), error);

            onDone (ok);
        }
        else if (ok && reveal)
            file.revealToUser(); // "Show the file(s) when complete"
        else if (ok)
            juce::AlertWindow::showMessageBoxAsync (job->getNotice().isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                    tr ("Export complete"),
                                                    file.getFullPathName() + "\n\n"
                                                        + tr ("Peak: {0} dBFS", { juce::String (juce::Decibels::gainToDecibels (job->getPeak()), 1) })
                                                        + (job->getNotice().isEmpty() ? juce::String() : "\n\n" + job->getNotice()));
        else if (! userPressedCancel)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Could not export"), error);

        // the export engine is torn down on the UI thread (plugins require this)
        juce::MessageManager::callAsync ([this] { delete this; });
    }

private:
    std::unique_ptr<ExportJob> job;
    juce::File file;
    std::function<void (bool)> onDone;
public:
    bool reveal = false;
private:
    juce::String error;
    bool ok = false;
};

// Stems: one file per insert that produces sound (FL's "split mixer tracks") or per channel that plays (each heard
// through its mixer path), plus the full mix when asked. Each render creates its own engine; plugins are created and
// destroyed on the UI thread, so the background thread asks it to do that.
class StemsThread final : public juce::ThreadWithProgressWindow
{
public:
    StemsThread (const AudioEngine& e, exportopts::Options o, juce::File base, const std::vector<int>& onlyTracks = {})
        : juce::ThreadWithProgressWindow (tr ("Exporting stems..."), true, true),
          options (o),
          baseFile (std::move (base)),
          song (e.getSong())
    {
        const auto ext = exportopts::extension (options.format);
        const auto baseName = baseFile.getFileNameWithoutExtension().toStdString();
        auto file = [&] (int number, const std::string& name) {
            return baseFile.getSiblingFile (juce::String::fromUTF8 (exportopts::stemFileName (baseName, number, name, ext).c_str()));
        };

        if (options.stemsIncludeMaster)
            items.push_back ({ juce::String ("Master"), baseFile, song });

        if (options.stems == exportopts::Stems::channels)
            for (auto c : exportopts::stemChannels (song, options.range))
                items.push_back ({ juce::String::fromUTF8 (song.channels[(std::size_t) c].name.c_str()), file (c + 1, song.channels[(std::size_t) c].name),
                                   exportopts::makeChannelStemSong (song, c) });
        else
            for (auto t : exportopts::stemTracks (song, onlyTracks))
                items.push_back ({ juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str()), file (t, song.mixer[(std::size_t) t].name),
                                   makeBounceSong (song, t) });
    }

    void run() override
    {
        for (std::size_t i = 0; i < items.size() && ! threadShouldExit(); ++i)
        {
            auto& item = items[i];
            setStatusMessage (item.name);

            std::unique_ptr<ExportJob> job;
            juce::WaitableEvent ready;
            juce::MessageManager::callAsync ([&] {
                job = std::make_unique<ExportJob> (item.song, options, item.file);
                ready.signal();
            });
            ready.wait();

            juce::String error;
            const auto base = (double) i / (double) items.size();
            const auto ok = job->run ([this, base] (double p) {
                setProgress (base + p / (double) items.size());
                return ! threadShouldExit();
            }, error);

            if (ok)
                ++written;

            juce::WaitableEvent gone;
            juce::MessageManager::callAsync ([&] {
                job.reset();
                gone.signal();
            });
            gone.wait();
        }
    }

    void threadComplete (bool cancelled) override
    {
        if (! cancelled && options.showWhenDone && written > 0)
            baseFile.getParentDirectory().startAsProcess();
        else if (! cancelled)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Stems exported"),
                                                    tr ("{0} file(s) in {1}", { juce::String (written), baseFile.getParentDirectory().getFullPathName() }));

        juce::MessageManager::callAsync ([this] { delete this; });
    }

private:
    struct Item
    {
        juce::String name;
        juce::File file;
        Song song;
    };
    exportopts::Options options;
    juce::File baseFile;
    Song song;
    std::vector<Item> items;
    int written = 0;
};

juce::File exportSettingsFile()
{
    return lynx::platform::settingsFile ("export-settings.txt");
}

// A row of buttons where exactly one is on (the output format).
class Segmented final : public juce::Component
{
public:
    std::function<void (int)> onChange;

    explicit Segmented (const juce::StringArray& names)
    {
        for (int i = 0; i < names.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (names[i]));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (8401);
            b->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < names.size() - 1 ? juce::Button::ConnectedOnRight : 0));
            b->setColour (juce::TextButton::buttonColourId, theme::colour::bg4);
            b->setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
            b->setColour (juce::TextButton::textColourOnId, theme::colour::onAccent);
            b->onClick = [this, i] {
                if (buttons[i]->getToggleState() && onChange)
                    onChange (i);
            };
            addAndMakeVisible (b);
        }
    }

    void setSelected (int index) { if (auto* b = buttons[index]) b->setToggleState (true, juce::dontSendNotification); }
    int getSelected() const
    {
        for (int i = 0; i < buttons.size(); ++i)
            if (buttons[i]->getToggleState())
                return i;
        return 0;
    }

    void resized() override
    {
        const auto w = (float) getWidth() / (float) juce::jmax (1, buttons.size());
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setBounds (juce::roundToInt (w * (float) i), 0, juce::roundToInt (w * (float) (i + 1)) - juce::roundToInt (w * (float) i), getHeight());
    }

private:
    juce::OwnedArray<juce::TextButton> buttons;
};

// The Export window (FL's "WAV rendering", interpreted; REA_rodada84): what to render on the left, the file format on
// the right, mix options below; the choices are remembered (%APPDATA%\LYNX\export-settings.txt).
class ExportPanel final : public juce::Component
{
public:
    // initialFormat: −1 = the formats of the last export (FL's "Last exported format(s)"), else that one format only
    // onlyTracks (FL's "Render armed / selected tracks to wave file(s)", REA round 165f-mixer): one file per insert of the
    // list, the stem choice locked (and not remembered); the Master in the list = also the full mix
    ExportPanel (AudioEngine& e, juce::File folder, juce::String name, bool remember, int initialFormat = -1, std::vector<int> tracks = {})
        : engine (e),
          suggestedFolder (std::move (folder)),
          suggestedName (std::move (name)),
          persistent (remember),
          onlyTracks (std::move (tracks))
    {
        using namespace exportopts;
        if (persistent)
            options = fromText (exportSettingsFile().loadFileAsString().toStdString());
        rememberedStems = options.stems;
        rememberedStemsMaster = options.stemsIncludeMaster;
        if (! onlyTracks.empty())
        {
            options.stems = Stems::mixerTracks;
            options.stemsIncludeMaster = std::find (onlyTracks.begin(), onlyTracks.end(), 0) != onlyTracks.end();
        }
        if (initialFormat >= 0 && initialFormat <= 3)
            options = withSingleFormat (options, (Format) initialFormat);
        const auto& song = engine.getSong();
        // the range follows what is playing unless a selection was the last choice and still exists
        if (options.range != Range::selection || ! hasSelection (song))
            options.range = song.mode == PlayMode::song && ! song.playlist.empty() ? Range::song : Range::pattern;

        for (auto* t : { &whatTitle, &formatTitle, &optionsTitle })
            addAndMakeVisible (t);
        whatTitle.setText (tr ("What to render"));
        formatTitle.setText (tr ("Output format"));
        optionsTitle.setText (tr ("Mix"));

        auto combo = [this] (juce::Label& label, const juce::String& text, juce::ComboBox& box, const juce::StringArray& items, const juce::String& tip = {}) {
            settingsui::styleLabel (label, text);
            addAndMakeVisible (label);
            box.addItemList (items, 1);
            box.setTooltip (tip);
            box.onChange = [this] { readControls(); };
            addAndMakeVisible (box);
        };

        combo (rangeLabel, tr ("Range"), rangeBox, { tr ("Song (Playlist)"), tr ("Current pattern"), tr ("Playlist selection") },
               tr ("What is rendered: the arrangement, the current pattern, or the Playlist's time selection"));
        rangeBox.setItemEnabled (3, hasSelection (song));
        juce::StringArray loopItems;
        for (int i = 1; i <= 16; ++i)
            loopItems.add (i == 1 ? tr ("Once") : tr ("{0} times", { juce::String (i) }));
        combo (loopsLabel, tr ("Play"), loopsBox, loopItems, tr ("How many times the range plays in the file"));
        combo (tailLabel, tr ("Tail"), tailBox, { tr ("Cut remainder"), tr ("Leave remainder"), tr ("Wrap remainder") },
               tr ("Cut: stop exactly at the end (loops). Leave: let reverbs and releases ring out (songs). Wrap: mix the tail into the start (seamless loops)"));
        combo (tailLengthLabel, tr ("Longest tail"), tailLengthBox, { "3 s", "6 s", "12 s", "30 s" }, tr ("The tail stops earlier when the sound has faded out"));
        combo (stemsLabel, tr ("Files"), stemsBox, { tr ("One file (the mix)"), tr ("One per mixer insert (stems)"), tr ("One per channel (stems)") },
               tr ("Stems: a file for each mixer insert that gets sound, or for each channel that plays"));
        stemsMaster.setButtonText (tr ("Also the full mix"));
        stemsMaster.onClick = [this] { readControls(); };
        addAndMakeVisible (stemsMaster);
        if (! onlyTracks.empty())
        {
            stemsBox.setEnabled (false);
            settingsui::styleLabel (tracksLabel, tr ("Tracks"));
            addAndMakeVisible (tracksLabel);
            juce::StringArray names;
            for (auto t : stemTracks (song, onlyTracks))
                names.add (juce::String (t) + " " + juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str()));
            tracksText.setText (names.joinIntoString (", "), juce::dontSendNotification);
            tracksText.setTooltip (names.joinIntoString ("\n"));
            tracksText.setMinimumHorizontalScale (0.7f);
            tracksText.setFont (theme::font (theme::type::small));
            tracksText.setColour (juce::Label::textColourId, theme::colour::textPrimary);
            addAndMakeVisible (tracksText);
        }

        formatButtons.onChange = [this] (int) { readControls(); };
        addAndMakeVisible (formatButtons);
        // FL ticks several format boxes at once (REA round 139): the other formats written from the same render
        settingsui::styleLabel (alsoLabel, tr ("Also save"));
        addAndMakeVisible (alsoLabel);
        for (int i = 0; i < 4; ++i)
        {
            static const char* names[] = { "WAV", "MP3", "OGG", "FLAC" };
            alsoToggles[(std::size_t) i].setButtonText (names[i]);
            alsoToggles[(std::size_t) i].setTooltip (tr ("Also write the {0} file from the same render (same name, its own extension)", { juce::String (names[i]) }));
            alsoToggles[(std::size_t) i].onClick = [this] { readControls(); };
            addAndMakeVisible (alsoToggles[(std::size_t) i]);
        }
        combo (depthLabel, tr ("Bit depth"), depthBox, { tr ("16 bits (integer)"), tr ("24 bits (integer)"), tr ("32 bits (float)") });
        juce::StringArray mp3Items, oggItems;
        for (auto k : mp3Bitrates())
            mp3Items.add (juce::String (k) + " kbps");
        for (auto k : oggBitrates())
            oggItems.add (juce::String (k) + " kbps");
        combo (bitrateLabel, tr ("Bitrate"), mp3Box, mp3Items, tr ("Constant bitrate (higher: better sound, bigger file)"));
        addAndMakeVisible (oggBox);
        oggBox.addItemList (oggItems, 1);
        oggBox.onChange = [this] { readControls(); };
        combo (compressionLabel, tr ("Compression"), flacLevelBox,
               { tr ("0 (fastest)"), "1", "2", "3", "4", tr ("5 (default)"), "6", "7", tr ("8 (smallest)") },
               tr ("FLAC is lossless at every level; higher levels make smaller files and take longer"));
        juce::StringArray rateItems;
        for (auto r : sampleRates())
            rateItems.add (juce::String ((int) r) + " Hz");
        combo (rateLabel, tr ("Sample rate"), rateBox, rateItems);
        combo (channelsLabel, tr ("Channels"), channelsBox, { tr ("Stereo"), tr ("Mono (both sides mixed)"), tr ("Mono (left only)"), tr ("Mono (right only)") });
        combo (ditherLabel, tr ("Dither"), ditherBox, { tr ("Off"), tr ("TPDF"), tr ("TPDF + noise shaping") },
               tr ("Hides the rounding noise of 16 and 24 bit files (noise shaping moves it where the ear is least sensitive)"));

        for (auto* t : { &normalizeToggle, &masterFxToggle, &trimToggle, &hqToggle, &showToggle, &polyToggle, &warmUpToggle, &realtimeToggle })
        {
            t->onClick = [this] { readControls(); };
            addAndMakeVisible (t);
        }
        normalizeToggle.setButtonText (tr ("Normalize (peak to 0 dB)"));
        masterFxToggle.setButtonText (tr ("Master effects"));
        trimToggle.setButtonText (tr ("Trim the delay compensation's silence"));
        normalizeToggle.setTooltip (tr ("Raises or lowers the whole file so its loudest peak reaches full scale"));
        masterFxToggle.setTooltip (tr ("Off: the Master insert's effects are bypassed for the export"));
        trimToggle.setTooltip (tr ("Removes the silence plugin latency compensation adds at the start"));
        hqToggle.setButtonText (tr ("HQ for all plugins"));
        showToggle.setButtonText (tr ("Show the file(s) when complete"));
        polyToggle.setButtonText (tr ("Disable maximum polyphony"));
        warmUpToggle.setButtonText (tr ("Plugin warm-up"));
        warmUpToggle.setTooltip (tr ("Before the export starts, third-party plugins process a moment of silence until they have "
                                     "finished loading their settings (bridged plugins, samplers that load in the background), "
                                     "so the file does not start with half-loaded effects"));
        // round 165c: only offered when the song has an active hardware insert (outboard gear can't run faster than real time)
        realtimeToggle.setButtonText (tr ("Render through the hardware in real time"));
        realtimeToggle.setTooltip (tr ("The song plays through the audio device while it is recorded, so the hardware inserts' gear is "
                                       "in the file (the export takes as long as the song). At the device's sample rate, played once, "
                                       "with the playback quality: what you hear"));
        // (not for a list of tracks: the real-time pass records the full mix only)
        realtimeToggle.setVisible (onlyTracks.empty() && hwinsert::countActive (engine.getSong()) > 0);
        polyToggle.setTooltip (tr ("The channels' Max polyphony is lifted for the export, so no note is cut by the voice limit"));
        showToggle.setTooltip (tr ("Opens the file manager on the exported file when the export is done"));
        hqToggle.setTooltip (tr ("Plugins with a separate render quality use it, and HQ switches are turned on for the export. "
                                 "Off: the export uses the playback quality, so it sounds exactly like what you heard"));

        infoLabel.setFont (theme::font (theme::type::small));
        infoLabel.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible (infoLabel);
        warningLabel.setFont (theme::font (theme::type::small));
        warningLabel.setColour (juce::Label::textColourId, theme::colour::warning);
        infoLabel.setMinimumHorizontalScale (0.7f); // (round 165c: the real-time line is longer)
        warningLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (warningLabel);

        exportButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        exportButton.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        addAndMakeVisible (exportButton);
        addAndMakeVisible (cancelButton);
        exportButton.onClick = [this] { chooseFile(); };
        cancelButton.onClick = [this] { close(); };

        writeControls();
        setSize (620, 486);
    }

    // --snapshot: the window with this format chosen (0 WAV, 1 MP3, 2 OGG, 3 FLAC) and a few options set
    void setFormatForSnapshot (int format)
    {
        options.format = (exportopts::Format) juce::jlimit (0, 3, format);
        if (format == 3)
        {
            options.tail = exportopts::TailMode::wrap;
            options.loops = 2;
            options.stems = exportopts::Stems::mixerTracks;
            options.stemsIncludeMaster = true;
            options.normalize = true;
        }
        writeControls();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        auto buttons = area.removeFromBottom (32);
        cancelButton.setBounds (buttons.removeFromRight (96));
        buttons.removeFromRight (8);
        exportButton.setBounds (buttons.removeFromRight (128));
        infoLabel.setBounds (buttons.removeFromTop (16));
        warningLabel.setBounds (buttons);
        area.removeFromBottom (8);

        auto mix = area.removeFromBottom (110);
        optionsTitle.setBounds (mix.removeFromTop (26));
        mix.removeFromTop (4);
        auto mixRow = mix.removeFromTop (24);
        normalizeToggle.setBounds (mixRow.removeFromLeft (200));
        masterFxToggle.setBounds (mixRow.removeFromLeft (150));
        trimToggle.setBounds (mixRow);
        mix.removeFromTop (2);
        {
            auto row2 = mix.removeFromTop (24);
            hqToggle.setBounds (row2.removeFromLeft (170));
            polyToggle.setBounds (row2.removeFromLeft (200));
            showToggle.setBounds (row2);
        }
        mix.removeFromTop (2);
        {
            auto row3 = mix.removeFromTop (24);
            warmUpToggle.setBounds (row3.removeFromLeft (200));
            realtimeToggle.setBounds (row3);
        }
        area.removeFromBottom (6);

        auto left = area.removeFromLeft ((area.getWidth() - 24) / 2);
        area.removeFromLeft (24);
        auto right = area;
        const int labelWidth = theme::labelColumnWidth ({ &rangeLabel, &loopsLabel, &tailLabel, &tailLengthLabel, &stemsLabel, &depthLabel,
                                                          &bitrateLabel, &compressionLabel, &rateLabel, &channelsLabel, &ditherLabel },
                                                        92, 150);
        constexpr int rowHeight = 26, gap = 7;

        auto row = [&] (juce::Rectangle<int>& column, juce::Label& label, juce::Component& control) {
            auto r = column.removeFromTop (rowHeight);
            label.setBounds (r.removeFromLeft (labelWidth));
            control.setBounds (r);
            column.removeFromTop (gap);
        };

        whatTitle.setBounds (left.removeFromTop (26));
        left.removeFromTop (8);
        row (left, rangeLabel, rangeBox);
        row (left, loopsLabel, loopsBox);
        row (left, tailLabel, tailBox);
        row (left, tailLengthLabel, tailLengthBox);
        row (left, stemsLabel, stemsBox);
        stemsMaster.setBounds (left.removeFromTop (22).withTrimmedLeft (labelWidth));
        if (! onlyTracks.empty())
        {
            left.removeFromTop (gap);
            row (left, tracksLabel, tracksText);
        }

        formatTitle.setBounds (right.removeFromTop (26));
        right.removeFromTop (8);
        formatButtons.setBounds (right.removeFromTop (28));
        right.removeFromTop (4);
        {
            // the four format ticks keep the room their names need; a caption too long to sit beside them
            // ("Также сохранить", "Auch speichern") gets its own line above them instead of being cut
            const auto ticks = 4 * 54;
            const auto labelNeeds = theme::labelColumnWidth ({ &alsoLabel }, 0, 400);
            const auto ownLine = labelNeeds > (right.getWidth() - ticks) * 11 / 10; // a slight squeeze is fine
            if (ownLine)
                alsoLabel.setBounds (right.removeFromTop (18));
            auto also = right.removeFromTop (22);
            if (! ownLine) // as before round 165o
                alsoLabel.setBounds (also.removeFromLeft (juce::jmax (juce::jmin (70, also.getWidth() - ticks),
                                                                      juce::jmin (theme::labelColumnWidth ({ &alsoLabel }, 70, 130), also.getWidth() - ticks))));
            const auto w = also.getWidth() / 4;
            for (auto& t : alsoToggles)
                t.setBounds (also.removeFromLeft (w));
        }
        right.removeFromTop (gap);
        // the format's own settings: two rows, the others hidden
        using exportopts::Format;
        const auto f = options.format;
        if (f == Format::wav || f == Format::flac)
            row (right, depthLabel, depthBox);
        if (f == Format::mp3)
            row (right, bitrateLabel, mp3Box);
        if (f == Format::ogg)
            row (right, bitrateLabel, oggBox);
        if (f == Format::flac)
            row (right, compressionLabel, flacLevelBox);
        row (right, rateLabel, rateBox);
        row (right, channelsLabel, channelsBox);
        row (right, ditherLabel, ditherBox);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        // a hairline between the two columns
        const auto x = 16 + (getWidth() - 32 - 24) / 2 + 12;
        g.setColour (theme::colour::border);
        g.fillRect (x, 22, 1, getHeight() - 22 - 12 - 32 - 8 - 58 - 6 - 8);
    }

private:
    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState (0);
    }

    static int indexOf (const std::vector<int>& list, int value)
    {
        for (int i = 0; i < (int) list.size(); ++i)
            if (list[(std::size_t) i] == value)
                return i;
        return 0;
    }

    void writeControls()
    {
        using namespace exportopts;
        const auto n = juce::dontSendNotification;
        rangeBox.setSelectedItemIndex ((int) options.range, n);
        loopsBox.setSelectedItemIndex (options.loops - 1, n);
        tailBox.setSelectedItemIndex ((int) options.tail, n);
        const double tails[] = { 3.0, 6.0, 12.0, 30.0 };
        int tailIndex = 0;
        for (int i = 0; i < 4; ++i)
            if (std::abs (tails[i] - options.maxTailSeconds) < std::abs (tails[tailIndex] - options.maxTailSeconds))
                tailIndex = i;
        tailLengthBox.setSelectedItemIndex (tailIndex, n);
        stemsBox.setSelectedItemIndex ((int) options.stems, n);
        stemsMaster.setToggleState (options.stemsIncludeMaster, n);
        formatButtons.setSelected ((int) options.format);
        depthBox.setSelectedItemIndex (options.format == Format::flac ? (options.flacBits == 16 ? 0 : 1) : (options.wavBits == 16 ? 0 : options.wavBits == 24 ? 1 : 2), n);
        mp3Box.setSelectedItemIndex (indexOf (mp3Bitrates(), options.mp3Kbps), n);
        oggBox.setSelectedItemIndex (indexOf (oggBitrates(), options.oggKbps), n);
        flacLevelBox.setSelectedItemIndex (options.flacLevel, n);
        int rate = 0;
        for (int i = 0; i < (int) sampleRates().size(); ++i)
            if (sampleRates()[(std::size_t) i] == options.sampleRate)
                rate = i;
        rateBox.setSelectedItemIndex (rate, n);
        channelsBox.setSelectedItemIndex ((int) options.channels, n);
        ditherBox.setSelectedItemIndex ((int) options.dither, n);
        normalizeToggle.setToggleState (options.normalize, n);
        masterFxToggle.setToggleState (options.masterEffects, n);
        trimToggle.setToggleState (options.trimPdc, n);
        hqToggle.setToggleState (options.hqAllPlugins, n);
        warmUpToggle.setToggleState (options.pluginWarmUp, n);
        showToggle.setToggleState (options.showWhenDone, n);
        polyToggle.setToggleState (options.disableMaxPolyphony, n);
        realtimeToggle.setToggleState (realtime, n);
        for (int i = 0; i < 4; ++i)
            alsoToggles[(std::size_t) i].setToggleState (i == (int) options.format || (options.moreFormats & (1u << i)) != 0, n);
        refresh();
    }

    void readControls()
    {
        using namespace exportopts;
        const auto previousFormat = options.format;
        options.range = (Range) juce::jmax (0, rangeBox.getSelectedItemIndex());
        options.loops = loopsBox.getSelectedItemIndex() + 1;
        options.tail = (TailMode) juce::jmax (0, tailBox.getSelectedItemIndex());
        const double tails[] = { 3.0, 6.0, 12.0, 30.0 };
        options.maxTailSeconds = tails[juce::jlimit (0, 3, tailLengthBox.getSelectedItemIndex())];
        options.stems = (Stems) juce::jmax (0, stemsBox.getSelectedItemIndex());
        options.stemsIncludeMaster = stemsMaster.getToggleState();
        options.format = (Format) formatButtons.getSelected();
        if (options.format == previousFormat)
        {
            const auto depth = depthBox.getSelectedItemIndex();
            if (options.format == Format::flac)
                options.flacBits = depth == 0 ? 16 : 24;
            else
                options.wavBits = depth == 0 ? 16 : depth == 1 ? 24 : 32;
        }
        options.mp3Kbps = mp3Bitrates()[(std::size_t) juce::jlimit (0, (int) mp3Bitrates().size() - 1, mp3Box.getSelectedItemIndex())];
        options.oggKbps = oggBitrates()[(std::size_t) juce::jlimit (0, (int) oggBitrates().size() - 1, oggBox.getSelectedItemIndex())];
        options.flacLevel = juce::jlimit (0, 8, flacLevelBox.getSelectedItemIndex());
        if (! realtime) // (real time shows the device's rate)
            options.sampleRate = sampleRates()[(std::size_t) juce::jlimit (0, (int) sampleRates().size() - 1, rateBox.getSelectedItemIndex())];
        options.channels = (Channels) juce::jmax (0, channelsBox.getSelectedItemIndex());
        options.dither = (DitherMode) juce::jmax (0, ditherBox.getSelectedItemIndex());
        options.normalize = normalizeToggle.getToggleState();
        options.trimPdc = trimToggle.getToggleState();
        options.showWhenDone = showToggle.getToggleState();
        if (! realtime) // (while real time is ticked these show what it uses: the offline choices stay as they were)
        {
            options.masterEffects = masterFxToggle.getToggleState();
            options.hqAllPlugins = hqToggle.getToggleState();
            options.pluginWarmUp = warmUpToggle.getToggleState();
            options.disableMaxPolyphony = polyToggle.getToggleState();
        }
        realtime = realtimeToggle.isVisible() && realtimeToggle.getToggleState();
        options.moreFormats = 0;
        for (int i = 0; i < 4; ++i) // (the previous main format's box was only shown ticked)
            if (i != (int) previousFormat && alsoToggles[(std::size_t) i].getToggleState())
                options.moreFormats |= 1u << i;
        options = clamped (options); // (the main format is not "more")

        if (options.format != previousFormat)
            writeControls(); // the bit depth list changes with the format
        else
            refresh();
    }

    void refresh()
    {
        using namespace exportopts;
        // the depth list: FLAC has no float
        depthBox.setItemEnabled (3, options.format == Format::wav);
        tailLengthBox.setEnabled (options.tail != TailMode::cut);
        stemsMaster.setEnabled (options.stems != Stems::none);
        loopsBox.setEnabled (! realtime);
        // a real-time bounce (round 165c): the device's rate, the song or the selection once, the Master's effects, the
        // playback quality, no stems — the controls it doesn't use are off
        for (auto* c : std::initializer_list<juce::Component*> { &rateBox, &stemsBox, &masterFxToggle, &hqToggle, &polyToggle, &warmUpToggle })
            c->setEnabled (! realtime);
        stemsBox.setEnabled (! realtime && onlyTracks.empty()); // a list of tracks locks the stem choice
        rangeBox.setItemEnabled (2, ! realtime);
        {
            // the boxes show what the export will use: real time = the Master's effects, the playback quality
            const auto used = realtime ? ExportJob::realtimeOptions (options, engine.getTransport().sampleRate.load()) : options;
            masterFxToggle.setToggleState (used.masterEffects, juce::dontSendNotification);
            hqToggle.setToggleState (used.hqAllPlugins, juce::dontSendNotification);
            warmUpToggle.setToggleState (used.pluginWarmUp, juce::dontSendNotification);
            polyToggle.setToggleState (used.disableMaxPolyphony, juce::dontSendNotification);
            for (int i = 0; i < (int) sampleRates().size(); ++i)
                if (sampleRates()[(std::size_t) i] == used.sampleRate)
                    rateBox.setSelectedItemIndex (i, juce::dontSendNotification);
        }
        stemsMaster.setEnabled (! realtime && options.stems != Stems::none);
        const auto ditherable = (options.format == Format::wav && options.wavBits != 32) || options.format == Format::flac;
        ditherBox.setEnabled (ditherable);
        ditherLabel.setEnabled (ditherable);
        const auto f = options.format;
        for (int i = 0; i < 4; ++i)
            alsoToggles[(std::size_t) i].setEnabled (i != (int) f);
        depthLabel.setVisible (f == Format::wav || f == Format::flac);
        depthBox.setVisible (f == Format::wav || f == Format::flac);
        bitrateLabel.setVisible (f == Format::mp3 || f == Format::ogg);
        mp3Box.setVisible (f == Format::mp3);
        oggBox.setVisible (f == Format::ogg);
        compressionLabel.setVisible (f == Format::flac);
        flacLevelBox.setVisible (f == Format::flac);

        const auto& song = engine.getSong();
        const auto shown = realtime ? ExportJob::realtimeOptions (options, engine.getTransport().sampleRate.load()) : options;
        const auto seconds = exportMusicSeconds (song, shown);
        // FL's line: "Length: 144 bars  Total time: 3'57''  Disk space: 38.76 MB" (REA round 134)
        const auto bars = lengthInBars (seconds, song.bpm, song.beatsPerBar);
        auto info = tr ("Length: {0} bars", { juce::String ((int) std::ceil (bars - 0.01)) }) + "  ·  " // whole bars, as FL
                    + tr ("Total time: {0}", { juce::String (formatDuration (seconds)) });
        if (options.tail == TailMode::leave)
            info << " " << tr ("(+ tail up to {0} s)", { juce::String ((int) options.maxTailSeconds) });
        if (options.format == Format::mp3 && mp3SampleRate (options.sampleRate) != options.sampleRate)
            info << "  ·  " << tr ("MP3 at {0} Hz", { juce::String ((int) mp3SampleRate (options.sampleRate)) });
        if (options.stems == Stems::mixerTracks)
            info << "  ·  " << tr ("{0} file(s)", { juce::String ((int) stemTracks (song, onlyTracks).size() + (options.stemsIncludeMaster ? 1 : 0)) });
        else if (options.stems == Stems::channels)
            info << "  ·  " << tr ("{0} file(s)", { juce::String ((int) stemChannels (song, options.range).size() + (options.stemsIncludeMaster ? 1 : 0)) });
        if (options.stems == Stems::none || realtime)
            info << "  ·  " << tr ("Disk space: {0}", { juce::File::descriptionOfSizeInBytes (estimatedBytes (shown, seconds)) });
        if (realtime)
            info << "  ·  " << tr ("{0} Hz (device)", { juce::String ((int) shown.sampleRate) });
        infoLabel.setText (info, juce::dontSendNotification);

        juce::String warning;
        if (! song.mixer.empty() && song.mixer[0].muted)
            warning = tr ("The Master insert is muted: the file will be silent");
        else if (options.range == Range::selection && ! hasSelection (song))
            warning = tr ("There is no Playlist selection: the whole song is rendered");
        else if (realtime && engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
            warning = tr ("No audio device is open: a real-time export needs one");
        else if (realtime)
            warning = tr ("Real time ({0}): leave the transport alone while it records",
                          { juce::String (formatDuration (seconds)) });
        else if (hwinsert::countActive (song) > 0) // round 164d: outboard gear can't run faster than real time
            warning = tr ("Hardware inserts pass their dry signal in an export (tick \"Render through the hardware in real time\" to keep their return)");
        warningLabel.setText (warning, juce::dontSendNotification);
        resized();
        repaint();
    }

    void chooseFile()
    {
        readControls();
        if (persistent)
        {
            auto remembered = options; // (the locked stem choice of a track render is not remembered)
            if (! onlyTracks.empty())
            {
                remembered.stems = rememberedStems;
                remembered.stemsIncludeMaster = rememberedStemsMaster;
            }
            exportSettingsFile().replaceWithText (juce::String (exportopts::toText (remembered)));
        }

        const auto o = options;
        const auto extension = juce::String (exportopts::extension (o.format));
        auto folder = lastFolder.isDirectory() ? lastFolder
                      : suggestedFolder.isDirectory() ? suggestedFolder
                                                      : juce::File::getSpecialLocation (juce::File::userMusicDirectory);

        chooser = std::make_unique<juce::FileChooser> (tr ("Export audio"), folder.getChildFile (suggestedName + extension), "*" + extension);
        auto& engineRef = engine;

        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, o, extension, &engineRef, tracks = onlyTracks, realtime = realtime] (const juce::FileChooser& fc) {
                                  auto file = fc.getResult();

                                  if (file == juce::File())
                                      return;

                                  if (! file.hasFileExtension (extension))
                                      file = file.withFileExtension (extension);

                                  lastFolder = file.getParentDirectory();
                                  engineRef.capturePluginStates(); // the copy carries the current plugin state

                                  if (realtime) // round 165c: through the device, in real time
                                  {
                                      auto job = std::make_unique<ExportJob> (engineRef, o, file);
                                      close(); // deletes this panel: no `this` after this point
                                      auto* thread = new ExportThread (std::move (job), file, {}, tr ("Exporting in real time..."));
                                      thread->reveal = o.showWhenDone;
                                      thread->launchThread();
                                      return;
                                  }
                                  if (o.stems != exportopts::Stems::none)
                                  {
                                      close();
                                      (new StemsThread (engineRef, o, file, tracks))->launchThread();
                                      return;
                                  }

                                  auto job = std::make_unique<ExportJob> (engineRef.getSong(), o, file);
                                  close(); // deletes this panel: no `this` after this point
                                  auto* thread = new ExportThread (std::move (job), file);
                                  thread->reveal = o.showWhenDone;
                                  thread->launchThread();
                              });
    }

    AudioEngine& engine;
    juce::File suggestedFolder;
    juce::String suggestedName;
    bool persistent = true;
    std::vector<int> onlyTracks;
    exportopts::Stems rememberedStems = exportopts::Stems::none;
    bool rememberedStemsMaster = false;
    exportopts::Options options;
    juce::Label tracksLabel, tracksText;

    settingsui::SectionTitle whatTitle, formatTitle, optionsTitle;
    juce::Label rangeLabel, loopsLabel, tailLabel, tailLengthLabel, stemsLabel;
    juce::ComboBox rangeBox, loopsBox, tailBox, tailLengthBox, stemsBox;
    juce::ToggleButton stemsMaster;
    Segmented formatButtons { { "WAV", "MP3", "OGG", "FLAC" } };
    juce::Label depthLabel, bitrateLabel, compressionLabel, rateLabel, channelsLabel, ditherLabel;
    juce::ComboBox depthBox, mp3Box, oggBox, flacLevelBox, rateBox, channelsBox, ditherBox;
    juce::ToggleButton normalizeToggle, masterFxToggle, trimToggle, hqToggle, showToggle, polyToggle, warmUpToggle, realtimeToggle;
    bool realtime = false; // "Render through the hardware in real time" (round 165c; not remembered: it takes the song's length)
    juce::Label alsoLabel;
    std::array<juce::ToggleButton, 4> alsoToggles;
    juce::Label infoLabel, warningLabel;
    juce::TextButton exportButton { tr ("Export...") }, cancelButton { tr ("Cancel") };
    std::unique_ptr<juce::FileChooser> chooser;

    static inline juce::File lastFolder;
};
} // namespace

void bounceInsertToAudio (AudioEngine& engine, int track)
{
    const auto& song = engine.getSong();

    if (track <= 0 || track >= (int) song.mixer.size())
        return;

    engine.capturePluginStates();
    const auto name = juce::String::fromUTF8 (song.mixer[(std::size_t) track].name.c_str());
    const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("LYNX/Renders");
    folder.createDirectory();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (name), ".wav");

    ExportSettings settings;
    settings.format = ExportSettings::Format::wav32Float;
    settings.sampleRate = engine.getTransport().sampleRate.load();
    settings.tailSeconds = 4.0;

    // clip length: the song + the tail, at the ending tempo
    std::int64_t musicTicks = 0;
    makeExportSong (song, true, 0, musicTicks);
    const auto length = musicTicks + (std::int64_t) (settings.tailSeconds * song.tempoAt ((double) musicTicks) * ppq / 60.0);

    auto job = std::make_unique<ExportJob> (makeBounceSong (song, track), settings, file);
    auto* engineRef = &engine;
    (new ExportThread (std::move (job), file, [engineRef, track, file, length] (bool ok) {
        if (! ok)
            return;

        engineRef->editSong ([&] (Song& s) { applyBounce (s, track, file.getFullPathName().toStdString(), length); });
    }, tr ("Rendering insert to audio...")))->launchThread();
}

void consolidatePlaylistTrack (AudioEngine& engine, int track, bool fromSongStart)
{
    engine.capturePluginStates();
    const auto& song = engine.getSong();
    Song render;
    std::int64_t start = 0, musicTicks = 0;
    if (! trackmodes::makeConsolidateSong (song, track, fromSongStart, render, start, musicTicks))
        return;

    const auto name = track < (int) song.playlistTracks.size() && ! song.playlistTracks[(std::size_t) track].name.empty()
                          ? juce::String::fromUTF8 (song.playlistTracks[(std::size_t) track].name.c_str())
                          : "Track " + juce::String (track + 1);
    const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("LYNX/Renders");
    folder.createDirectory();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (name + " (consolidated)"), ".wav");

    ExportSettings settings;
    settings.format = ExportSettings::Format::wav32Float;
    settings.sampleRate = engine.getTransport().sampleRate.load();
    settings.tailSeconds = 2.0;
    const auto length = musicTicks + (std::int64_t) (settings.tailSeconds * song.tempoAt ((double) (start + musicTicks)) * ppq / 60.0);

    auto job = std::make_unique<ExportJob> (render, settings, file);
    auto* engineRef = &engine;
    (new ExportThread (std::move (job), file, [engineRef, track, file, start, length] (bool ok) {
        if (ok)
            engineRef->editSong ([&] (Song& s) { trackmodes::applyConsolidate (s, track, file.getFullPathName().toStdString(), start, length); });
    }, tr ("Consolidating track...")))->launchThread();
}

std::unique_ptr<juce::Component> makeExportPanel (AudioEngine& engine, int format)
{
    auto panel = std::make_unique<ExportPanel> (engine, juce::File(), "LYNX", false);
    panel->setFormatForSnapshot (format);
    return panel;
}

std::unique_ptr<juce::Component> makeExportPanelForTracks (AudioEngine& engine, const std::vector<int>& tracks)
{
    return std::make_unique<ExportPanel> (engine, juce::File(), "LYNX", false, 0, tracks);
}

void showExportDialogForTracks (AudioEngine& engine, const juce::File& suggestedFolder, const juce::String& suggestedName,
                                const std::vector<int>& tracks)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (fitContentToScreen (std::make_unique<ExportPanel> (engine, suggestedFolder, suggestedName.isNotEmpty() ? suggestedName : "LYNX",
                                                                                 true, -1, tracks)).release());
    options.dialogTitle = tr ("Render mixer tracks to audio files");
    options.dialogBackgroundColour = theme::colour::bg2;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    if (auto* dialog = options.launchAsync())
        keepAboveMainWindow (*dialog);
}

void showExportDialog (AudioEngine& engine, const juce::File& suggestedFolder, const juce::String& suggestedName, int format)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (fitContentToScreen (std::make_unique<ExportPanel> (engine, suggestedFolder, suggestedName.isNotEmpty() ? suggestedName : "LYNX", true, format)).release());
    options.dialogTitle = tr ("Export audio");
    options.dialogBackgroundColour = theme::colour::bg2;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    if (auto* dialog = options.launchAsync())
        keepAboveMainWindow (*dialog);
}
} // namespace lynx
