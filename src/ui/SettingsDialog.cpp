#include "ui/SettingsDialog.h"
#include "ui/ThemeSettings.h"

#include "core/MissingFiles.h"
#include "engine/PluginHost.h"
#include "ui/I18n.h"
#include "ui/MissingFilesDialog.h"
#include "ui/Theme.h"
#include "platform/Paths.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

using namespace settingsui;

// --- Project page ----------------------------------------------------------------------------------
class ProjectPage final : public juce::Component,
                          private juce::ChangeListener
{
public:
    explicit ProjectPage (AudioEngine& e) : engine (e)
    {
        addAndMakeVisible (timeTitle);
        addAndMakeVisible (grooveTitle);
        addAndMakeVisible (gridTitle);

        for (auto* l : { &tempoLabel, &beatsLabel, &swingLabel, &pitchLabel })
            addAndMakeVisible (l);
        styleLabel (tempoLabel, tr ("Tempo"));
        styleLabel (beatsLabel, tr ("Beats per bar"));
        styleLabel (swingLabel, tr ("Swing"));
        styleLabel (pitchLabel, tr ("Master pitch"));

        auto setup = [this] (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix, double def) {
            s.setSliderStyle (juce::Slider::LinearBar);
            s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 90, 26);
            s.setRange (lo, hi, step);
            s.setTextValueSuffix (suffix);
            s.setDoubleClickReturnValue (true, def);
            s.setColour (juce::Slider::trackColourId, theme::colour::bg4); // a quiet bar: the number is what matters here
            s.setColour (juce::Slider::textBoxOutlineColourId, theme::colour::borderStrong);
            addAndMakeVisible (s);
        };
        setup (tempo, 10.0, 999.0, 0.001, " BPM", 130.0);
        setup (beats, 1.0, 32.0, 1.0, {}, 4.0);
        setup (swing, 0.0, 100.0, 1.0, "%", 0.0);
        setup (pitch, -1200.0, 1200.0, 1.0, " " + tr ("cents"), 0.0);
        tempo.setNumDecimalPlacesToDisplay (3);

        tempo.onValueChange = [this] { edit ([v = tempo.getValue()] (Song& s) { s.bpm = v; }); };
        beats.onValueChange = [this] { edit ([v = (int) beats.getValue()] (Song& s) { s.beatsPerBar = v; }); };
        swing.onValueChange = [this] { edit ([v = (float) (swing.getValue() / 100.0)] (Song& s) { s.swing = v; }); };
        pitch.onValueChange = [this] { edit ([v = (float) pitch.getValue()] (Song& s) { s.masterPitch = v; }); };

        for (auto* l : { &ppqText, &stepsText, &signatureText })
        {
            styleLabel (*l, {});
            l->setFont (theme::font (theme::type::small));
            addAndMakeVisible (l);
        }
        ppqText.setText (tr ("Timebase: {0} ticks per quarter note (PPQ). Projects from FL Studio are converted to it.", { juce::String (ppq) }),
                         juce::dontSendNotification);
        stepsText.setText (tr ("Steps per beat: {0} (the Channel Rack's step grid).", { juce::String (stepsPerBeat) }), juce::dontSendNotification);
        signatureText.setText (tr ("Time signature changes along the song: add a time signature marker on the Playlist ruler."),
                               juce::dontSendNotification);

        engine.addChangeListener (this);
        load();
    }

    ~ProjectPage() override { engine.removeChangeListener (this); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 14);
        auto row = [&area] (juce::Label& l, juce::Slider& s) {
            auto r = area.removeFromTop (28);
            l.setBounds (r.removeFromLeft (150));
            s.setBounds (r.removeFromLeft (260));
            area.removeFromTop (8);
        };
        timeTitle.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);
        row (tempoLabel, tempo);
        row (beatsLabel, beats);
        grooveTitle.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);
        row (swingLabel, swing);
        row (pitchLabel, pitch);
        gridTitle.setBounds (area.removeFromTop (28));
        area.removeFromTop (6);
        for (auto* l : { &ppqText, &stepsText, &signatureText })
            l->setBounds (area.removeFromTop (22));
    }

private:
    void edit (std::function<void (Song&)> fn)
    {
        if (loading)
            return;
        engine.editSong (fn);
    }

    void load()
    {
        const juce::ScopedValueSetter<bool> guard (loading, true);
        const auto& s = engine.getSong();
        tempo.setValue (s.bpm, juce::dontSendNotification);
        beats.setValue (s.beatsPerBar, juce::dontSendNotification);
        swing.setValue (s.swing * 100.0, juce::dontSendNotification);
        pitch.setValue (s.masterPitch, juce::dontSendNotification);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (! tempo.isMouseButtonDown() && ! beats.isMouseButtonDown() && ! swing.isMouseButtonDown() && ! pitch.isMouseButtonDown())
            load();
    }

    AudioEngine& engine;
    bool loading = false;
    SectionTitle timeTitle { tr ("Time settings") }, grooveTitle { tr ("Groove and tuning") }, gridTitle { tr ("Timebase") };
    juce::Label tempoLabel, beatsLabel, swingLabel, pitchLabel, ppqText, stepsText, signatureText;
    juce::Slider tempo, beats, swing, pitch;
};

// --- Info page -------------------------------------------------------------------------------------
class InfoPage final : public juce::Component,
                       private juce::ChangeListener
{
public:
    explicit InfoPage (AudioEngine& e) : engine (e)
    {
        addAndMakeVisible (infoTitle);
        for (auto* l : { &titleLabel, &authorLabel, &genreLabel, &urlLabel, &commentsLabel })
            addAndMakeVisible (l);
        styleLabel (titleLabel, tr ("Title"));
        styleLabel (authorLabel, tr ("Author"));
        styleLabel (genreLabel, tr ("Genre"));
        styleLabel (urlLabel, tr ("Web link"));
        styleLabel (commentsLabel, tr ("Comments"));

        for (auto* t : { &title, &author, &genre, &url })
        {
            styleEditor (*t);
            t->onReturnKey = [this] { commit(); };
            t->onFocusLost = [this] { commit(); };
            addAndMakeVisible (t);
        }
        genre.setTextToShowWhenEmpty (tr ("e.g. House, Trap, Lo-fi"), theme::colour::textDisabled);
        url.setTextToShowWhenEmpty ("https://", theme::colour::textDisabled);

        genreMenu.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
        genreMenu.onClick = [this] { showGenres(); };
        addAndMakeVisible (genreMenu);
        openLink.onClick = [this] {
            const juce::URL link (url.getText().trim());
            if (url.getText().trim().isNotEmpty())
                link.launchInDefaultBrowser();
        };
        addAndMakeVisible (openLink);

        styleEditor (comments);
        comments.setMultiLine (true, true);
        comments.setReturnKeyStartsNewLine (true);
        comments.setScrollbarsShown (true);
        comments.onFocusLost = [this] { commit(); };
        addAndMakeVisible (comments);

        showOnOpen.setButtonText (tr ("Show this page when the project is opened"));
        showOnOpen.onClick = [this] { commit(); };
        addAndMakeVisible (showOnOpen);

        engine.addChangeListener (this);
        load();
    }

    ~InfoPage() override
    {
        commit(); // (typed text not yet confirmed)
        engine.removeChangeListener (this);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 14);
        infoTitle.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);
        auto row = [&area] (juce::Label& l) {
            auto r = area.removeFromTop (28);
            l.setBounds (r.removeFromLeft (110));
            area.removeFromTop (8);
            return r;
        };
        title.setBounds (row (titleLabel));
        author.setBounds (row (authorLabel));
        {
            auto r = row (genreLabel);
            genreMenu.setBounds (r.removeFromRight (30));
            r.removeFromRight (4);
            genre.setBounds (r);
        }
        {
            auto r = row (urlLabel);
            openLink.setBounds (r.removeFromRight (70));
            r.removeFromRight (4);
            url.setBounds (r);
        }
        showOnOpen.setBounds (area.removeFromBottom (26));
        area.removeFromBottom (8);
        commentsLabel.setBounds (area.removeFromTop (22));
        comments.setBounds (area);
    }

private:
    ProjectInfo typed() const
    {
        ProjectInfo i;
        i.title = title.getText().trim().toStdString();
        i.author = author.getText().trim().toStdString();
        i.genre = genre.getText().trim().toStdString();
        i.url = url.getText().trim().toStdString();
        i.comments = comments.getText().trimEnd().replace ("\r\n", "\n").toStdString();
        i.showOnOpen = showOnOpen.getToggleState();
        return i;
    }

    void commit()
    {
        if (loading)
            return;
        const auto info = typed();
        if (info != engine.getSong().info)
            engine.editSong ([info] (Song& s) { s.info = info; });
    }

    void load()
    {
        const juce::ScopedValueSetter<bool> guard (loading, true);
        const auto& i = engine.getSong().info;
        auto set = [] (juce::TextEditor& t, const std::string& value) {
            if (! t.hasKeyboardFocus (true) && t.getText() != u8 (value))
                t.setText (u8 (value), juce::dontSendNotification);
        };
        set (title, i.title);
        set (author, i.author);
        set (genre, i.genre);
        set (url, i.url);
        set (comments, i.comments);
        showOnOpen.setToggleState (i.showOnOpen, juce::dontSendNotification);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (engine.getSong().info != typed())
            load();
    }

    void showGenres()
    {
        // common genre names (the ID3 list's best-known ones and today's styles)
        static const char* const genres[] = { "Ambient", "Bass house", "Blues", "Classical", "Country", "Dance", "Deep house", "Disco",
                                               "Drum & bass", "Dubstep", "EDM", "Electro", "Electronic", "Funk", "Future bass", "Funk carioca",
                                               "Hardstyle", "Hip-hop", "House", "Jazz", "Lo-fi", "Metal", "Phonk", "Pop", "R&B", "Reggae",
                                               "Reggaeton", "Rock", "Soul", "Synthwave", "Techno", "Trance", "Trap" };
        juce::PopupMenu menu;
        for (const auto* g : genres)
            menu.addItem (g, [this, name = juce::String (g)] {
                genre.setText (name, juce::dontSendNotification);
                commit();
            });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&genreMenu));
    }

    AudioEngine& engine;
    bool loading = false;
    SectionTitle infoTitle { tr ("Project information") };
    juce::Label titleLabel, authorLabel, genreLabel, urlLabel, commentsLabel;
    juce::TextEditor title, author, genre, url, comments;
    juce::TextButton genreMenu, openLink { tr ("Open") };
    juce::ToggleButton showOnOpen;
};

// --- Files page ------------------------------------------------------------------------------------
// One editable folder list (Add... / Remove), loaded and saved through the callbacks.
class FolderList final : public juce::Component,
                         private juce::ListBoxModel
{
public:
    FolderList (juce::String heading, juce::String note, std::function<juce::StringArray()> loadFn,
                std::function<void (const juce::StringArray&)> saveFn, bool checkFolders = true)
        : title (std::move (heading)), load (std::move (loadFn)), save (std::move (saveFn)), check (checkFolders)
    {
        addAndMakeVisible (title);
        styleLabel (noteLabel, note);
        noteLabel.setFont (theme::font (theme::type::small));
        addAndMakeVisible (noteLabel);
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        list.setColour (juce::ListBox::outlineColourId, theme::colour::border);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);
        addButton.onClick = [this] { add(); };
        removeButton.onClick = [this] { removeSelected(); };
        addAndMakeVisible (addButton);
        addAndMakeVisible (removeButton);
        folders = load();
        refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        title.setBounds (area.removeFromTop (28));
        area.removeFromTop (4);
        noteLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        auto buttons = area.removeFromRight (100);
        area.removeFromRight (8);
        list.setBounds (area);
        addButton.setBounds (buttons.removeFromTop (26));
        buttons.removeFromTop (6);
        removeButton.setBounds (buttons.removeFromTop (26));
        if (extraButton != nullptr)
        {
            buttons.removeFromTop (6);
            extraButton->setBounds (buttons.removeFromTop (26));
        }
    }

    // A third button under Add / Remove (the plugin folders' "Plugin manager...", as FL's File page has it).
    void addExtraButton (const juce::String& text, std::function<void()> onClick)
    {
        extraButton = std::make_unique<juce::TextButton> (text);
        extraButton->onClick = std::move (onClick);
        addAndMakeVisible (*extraButton);
        resized();
    }

private:
    int getNumRows() override { return juce::jmax (1, folders.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (folders.isEmpty())
        {
            g.setFont (theme::font (theme::type::small));
            g.setColour (theme::colour::textDisabled);
            g.drawText (tr ("(none yet)"), 10, 0, width - 20, height, juce::Justification::centredLeft);
            return;
        }
        if (selected)
        {
            g.setColour (theme::colour::accentMuted);
            g.fillRect (0, 0, width, height);
        }
        const auto exists = ! check || juce::File (folders[row]).isDirectory();
        g.setFont (theme::font (theme::type::body));
        g.setColour (exists ? theme::colour::textPrimary : theme::colour::warning);
        g.drawText (folders[row] + (exists ? juce::String() : "  (" + tr ("not found") + ")"), 10, 0, width - 20, height,
                    juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int) override { refresh(); }

    void refresh()
    {
        removeButton.setEnabled (! folders.isEmpty() && list.getSelectedRow() >= 0);
        list.updateContent();
        list.repaint();
    }

    void add()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Add a folder"), juce::File::getSpecialLocation (juce::File::userHomeDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<FolderList> (this)] (const juce::FileChooser& c) {
                                  if (safe == nullptr || ! c.getResult().isDirectory())
                                      return;
                                  safe->folders.addIfNotAlreadyThere (c.getResult().getFullPathName(), true);
                                  safe->save (safe->folders);
                                  safe->refresh();
                              });
    }

    void removeSelected()
    {
        const auto row = list.getSelectedRow();
        if (row < 0 || row >= folders.size())
            return;
        folders.remove (row);
        save (folders);
        list.deselectAllRows();
        refresh();
    }

    SectionTitle title;
    juce::Label noteLabel;
    std::function<juce::StringArray()> load;
    std::function<void (const juce::StringArray&)> save;
    bool check = true;
    juce::StringArray folders;
    juce::ListBox list;
    juce::TextButton addButton { tr ("Add...") }, removeButton { tr ("Remove") };
    std::unique_ptr<juce::TextButton> extraButton;
    std::unique_ptr<juce::FileChooser> chooser;
};

class FilesPage final : public juce::Component
{
public:
    FilesPage (AudioEngine& engine, bool persistent, std::function<void()> browserChanged, std::function<void()> managePlugins = {})
    {
        auto sample = [persistent] (juce::StringArray real, std::initializer_list<const char*> fake) {
            if (persistent)
                return real;
            juce::StringArray list;
            for (const auto* f : fake)
                list.add (f);
            return list;
        };
        auto& host = engine.getPluginHost();

        searchFolders = std::make_unique<FolderList> (
            tr ("Search folders for missing samples"), tr ("The Missing files window looks in these (and their sub-folders) by itself."),
            [persistent, sample] {
                juce::StringArray list;
                if (persistent)
                    for (const auto& f : loadSearchFolders())
                        list.add (u8 (f));
                return sample (list, { "D:\\Library\\Drums", "D:\\Library\\Loops" });
            },
            [persistent] (const juce::StringArray& list) {
                if (! persistent)
                    return;
                std::vector<std::string> folders;
                for (const auto& f : list)
                    folders.push_back (f.toStdString());
                saveSearchFolders (folders);
            }, persistent);

        browserFolders = std::make_unique<FolderList> (
            tr ("Browser folders"), tr ("Shown in the Browser's folder list; also searched for missing samples."),
            [persistent, sample] { return sample (persistent ? browserSearchFolders() : juce::StringArray(), { "C:\\Users\\me\\Music\\Samples" }); },
            [persistent, browserChanged] (const juce::StringArray& list) {
                if (! persistent)
                    return;
                PluginHost::settingsFolder().getChildFile ("browser-folders.txt").replaceWithText (list.joinIntoString ("\n"));
                if (browserChanged)
                    browserChanged();
            }, persistent);

        pluginFolders = std::make_unique<FolderList> (
            tr ("Plugin folders"), tr ("Scanned for {0} plugins (Options > Scan for plugins).", { platform::pluginFormatNames() }),
            [persistent, sample, &host] {
                juce::StringArray list;
                if (persistent)
                {
                    const auto path = host.pluginFolders();
                    for (int i = 0; i < path.getNumPaths(); ++i)
                        list.add (path[i].getFullPathName());
                }
                return sample (list, { "C:\\Program Files\\Common Files\\VST3", "C:\\Program Files\\Common Files\\CLAP" });
            },
            [persistent, &host] (const juce::StringArray& list) {
                if (! persistent)
                    return;
                juce::FileSearchPath path;
                for (const auto& f : list)
                    path.add (juce::File (f));
                host.setPluginFolders (path);
            }, persistent);

        // FL's File page has "Manage plugins" next to the plugin settings (REA round 143b)
        if (managePlugins)
            pluginFolders->addExtraButton (tr ("Plugin manager..."), managePlugins);

        addAndMakeVisible (*searchFolders);
        addAndMakeVisible (*browserFolders);
        addAndMakeVisible (*pluginFolders);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 14);
        const auto h = (area.getHeight() - 24) / 3;
        searchFolders->setBounds (area.removeFromTop (h));
        area.removeFromTop (12);
        browserFolders->setBounds (area.removeFromTop (h));
        area.removeFromTop (12);
        pluginFolders->setBounds (area);
    }

private:
    std::unique_ptr<FolderList> searchFolders, browserFolders, pluginFolders;
};

class SettingsPanel final : public juce::Component
{
public:
    SettingsPanel (AudioEngine& engine, SettingsPage page, bool persistent, SettingsHooks hooks)
    {
        tabs.setTabBarDepth (34);
        tabs.setOutline (0);
        // FL's order: device pages first, then the app, then the project
        const std::pair<SettingsPage, juce::String> order[] = { { SettingsPage::audio, tr ("Audio") },     { SettingsPage::midi, tr ("MIDI") },
                                                                { SettingsPage::general, tr ("General") }, { SettingsPage::theme, tr ("Theme") },
                                                                { SettingsPage::files, tr ("Files") },
                                                                { SettingsPage::project, tr ("Project") }, { SettingsPage::info, tr ("Info") } };
        for (const auto& [p, name] : order)
        {
            juce::Component* content = nullptr;
            switch (p)
            {
                case SettingsPage::audio: content = makeAudioPage (engine, persistent).release(); break;
                case SettingsPage::midi: content = makeMidiPage (engine, hooks, persistent).release(); break;
                case SettingsPage::general: content = makeGeneralPage (engine, hooks, persistent).release(); break;
                case SettingsPage::theme: content = themeui::makeThemePage().release(); break;
                case SettingsPage::files: content = new FilesPage (engine, persistent, hooks.browserFoldersChanged, hooks.managePlugins); break;
                case SettingsPage::project: content = new ProjectPage (engine); break;
                case SettingsPage::info: content = new InfoPage (engine); break;
            }
            tabs.addTab (name, theme::colour::bg2, content, true);
            if (p == page)
                tabs.setCurrentTabIndex (tabs.getNumTabs() - 1, false);
        }
        addAndMakeVisible (tabs);
        setSize (680, 620);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }
    void resized() override { tabs.setBounds (getLocalBounds()); }

private:
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
};
} // namespace

void showSettings (AudioEngine& engine, SettingsPage page, SettingsHooks hooks)
{
    // one Settings window: opening it again shows the asked page
    openToolWindow (std::make_unique<SettingsPanel> (engine, page, true, std::move (hooks)), tr ("Settings"), "settings");
}

std::unique_ptr<juce::Component> makeSettingsPanel (AudioEngine& engine, SettingsPage page, bool persistent, SettingsHooks hooks)
{
    return std::make_unique<SettingsPanel> (engine, page, persistent, std::move (hooks));
}
} // namespace lynx
