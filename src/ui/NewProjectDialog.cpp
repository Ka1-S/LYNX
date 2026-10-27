#include "ui/NewProjectDialog.h"
#include "platform/Paths.h"

#include "core/Preferences.h"
#include "core/Templates.h"
#include "engine/ProjectFile.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
using namespace settingsui;

juce::File newProjectSettingsFile()
{
    return lynx::platform::settingsFile ("new-project.txt");
}

struct Remembered
{
    bool show = true;
    juce::String folder, author;
    bool ownFolder = true;
};

Remembered loadRemembered()
{
    Remembered r;
    const auto kv = prefs::parseKeyValues (newProjectSettingsFile().loadFileAsString().toStdString());
    auto get = [&kv] (const char* key) { const auto it = kv.find (key); return it == kv.end() ? juce::String() : juce::String::fromUTF8 (it->second.c_str()); };
    r.show = get ("show") != "0";
    r.folder = get ("folder");
    r.author = get ("author");
    r.ownFolder = get ("ownFolder") != "0";
    return r;
}

void saveRemembered (const Remembered& r)
{
    juce::String text;
    text << "show=" << (r.show ? 1 : 0) << "\nfolder=" << r.folder << "\nauthor=" << r.author << "\nownFolder=" << (r.ownFolder ? 1 : 0) << "\n";
    newProjectSettingsFile().getParentDirectory().createDirectory();
    newProjectSettingsFile().replaceWithText (text);
}

struct TemplateItem
{
    juce::String name, description;
    int builtIn = -2; // -1 = LYNX's default song, 0.. = makeTemplate(index), -2 = a user file
    juce::File file;
};

std::vector<TemplateItem> templateItems (bool sample)
{
    std::vector<TemplateItem> items;
    items.push_back ({ tr ("Default"), tr ("Drums and a synth on the first inserts, a limiter on Master"), -1, {} });
    const auto names = templateNames();
    const char* const descriptions[] = { TR_NOOP ("Nothing in the patterns"), TR_NOOP ("A four-on-the-floor beat to start from"),
                                         TR_NOOP ("140 BPM, fast hats and an 808 with glide"), TR_NOOP ("124 BPM house groove with bass") };
    for (int i = 0; i < (int) names.size(); ++i)
        items.push_back ({ tr (juce::String::fromUTF8 (names[(std::size_t) i].c_str())), i < 4 ? tr (descriptions[i]) : juce::String(), i, {} });

    if (sample)
    {
        items.push_back ({ "Night Drive kit", tr ("Your template"), -2, juce::File() });
        items.push_back ({ "Vocal session", tr ("Your template"), -2, juce::File() });
        return items;
    }

    for (const auto& f : userTemplatesFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + ProjectFile::extension))
        items.push_back ({ juce::String::fromUTF8 (templateDisplayName (f.getFileName().toStdString()).c_str()), tr ("Your template"), -2, f });

    std::stable_sort (items.begin() + 1 + (long) names.size(), items.end(), [] (const TemplateItem& a, const TemplateItem& b) {
        return a.name.compareNatural (b.name) < 0;
    });
    return items;
}

class TemplateList final : public juce::ListBox,
                           private juce::ListBoxModel
{
public:
    std::vector<TemplateItem> items;
    std::function<void()> onPick;

    TemplateList()
    {
        setModel (this);
        setRowHeight (40);
        setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        setColour (juce::ListBox::outlineColourId, theme::colour::border);
        setOutlineThickness (1);
    }

    int getNumRows() override { return (int) items.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) items.size())
            return;
        const auto& item = items[(std::size_t) row];
        if (selected)
        {
            g.setColour (theme::colour::accentMuted);
            g.fillRect (0, 0, width, height);
            g.setColour (theme::colour::accent);
            g.fillRect (0, 0, 3, height);
        }
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::body, true));
        g.drawText (item.name, 12, 4, width - 20, 18, juce::Justification::centredLeft, true);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::small));
        g.drawText (item.description, 12, 21, width - 20, 15, juce::Justification::centredLeft, true);
        // the line between the built-in templates and the user's
        if (row + 1 < (int) items.size() && item.builtIn != -2 && items[(std::size_t) row + 1].builtIn == -2)
        {
            g.setColour (theme::colour::borderStrong);
            g.fillRect (8, height - 1, width - 16, 1);
        }
    }

    void selectedRowsChanged (int) override
    {
        if (onPick)
            onPick();
    }
};

class NewProjectPanel final : public juce::Component
{
public:
    NewProjectPanel (NewProjectCallback callback, bool persistentState)
        : onCreate (std::move (callback)),
          persistent (persistentState)
    {
        const auto remembered = persistent ? loadRemembered() : Remembered { true, "C:/Users/me/Music/LYNX Projects", "Me", true };

        for (auto* t : { &templateTitle, &nameTitle, &timeTitle, &infoTitle })
            addAndMakeVisible (t);
        templateTitle.setText (tr ("Template"));
        nameTitle.setText (tr ("Name and location"));
        timeTitle.setText (tr ("Time settings"));
        infoTitle.setText (tr ("Information"));

        templates.items = templateItems (! persistent);
        templates.updateContent();
        templates.selectRow (0);
        templates.onPick = [this] { templateChosen(); };
        addAndMakeVisible (templates);
        templatesFolder.setTooltip (tr ("Templates are .lynx projects in this folder (File › Save as template... puts one there)"));
        templatesFolder.onClick = [] {
            userTemplatesFolder().createDirectory();
            userTemplatesFolder().startAsProcess();
        };
        addAndMakeVisible (templatesFolder);

        auto label = [this] (juce::Label& l, const juce::String& text) {
            styleLabel (l, text);
            addAndMakeVisible (l);
        };
        auto editor = [this] (juce::TextEditor& e, const juce::String& placeholder = {}) {
            styleEditor (e);
            e.setTextToShowWhenEmpty (placeholder, theme::colour::textDisabled);
            addAndMakeVisible (e);
        };

        label (nameLabel, tr ("Name"));
        editor (name, tr ("Untitled"));
        name.onTextChange = [this] { updatePath(); };
        label (folderLabel, tr ("Folder"));
        folder = juce::File::isAbsolutePath (remembered.folder) ? juce::File (remembered.folder)
                                                                 : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LYNX Projects");
        if (! persistent)
            folderText = remembered.folder;
        folderValue.setFont (theme::font (theme::type::body));
        folderValue.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        folderValue.setColour (juce::Label::backgroundColourId, theme::colour::bg0);
        folderValue.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (folderValue);
        chooseFolder.onClick = [this] { pickFolder(); };
        addAndMakeVisible (chooseFolder);
        saveNow.setButtonText (tr ("Save it there now"));
        saveNow.setTooltip (tr ("Off: the project opens unsaved and asks where to go when you first save"));
        saveNow.setToggleState (persistent ? remembered.folder.isNotEmpty() : true, juce::dontSendNotification);
        saveNow.onClick = [this] { updatePath(); };
        addAndMakeVisible (saveNow);
        ownFolder.setButtonText (tr ("Create a folder for the project"));
        ownFolder.setToggleState (remembered.ownFolder, juce::dontSendNotification);
        ownFolder.onClick = [this] { updatePath(); };
        addAndMakeVisible (ownFolder);
        pathNote.setFont (theme::font (theme::type::small));
        pathNote.setColour (juce::Label::textColourId, theme::colour::textDisabled);
        pathNote.setMinimumHorizontalScale (0.5f);
        addAndMakeVisible (pathNote);

        label (tempoLabel, tr ("Tempo"));
        tempo.setSliderStyle (juce::Slider::IncDecButtons);
        tempo.setRange (10.0, 522.0, 1.0);
        tempo.setTextValueSuffix (" BPM");
        tempo.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        tempo.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 90, 26);
        addAndMakeVisible (tempo);
        label (meterLabel, tr ("Time signature"));
        for (int i = 1; i <= 16; ++i)
            meter.addItem (juce::String (i) + "/4", i);
        addAndMakeVisible (meter);

        label (titleLabel, tr ("Title"));
        editor (title, tr ("same as the name"));
        label (authorLabel, tr ("Author"));
        editor (author);
        author.setText (remembered.author, false);
        label (genreLabel, tr ("Genre"));
        editor (genre);
        label (urlLabel, tr ("Web link"));
        editor (url, "https://");

        dontShow.setButtonText (tr ("Don't show this window for File › New"));
        dontShow.setTooltip (tr ("File › New then starts the default project at once; this window stays in File › New project..."));
        dontShow.setToggleState (! remembered.show, juce::dontSendNotification);
        addAndMakeVisible (dontShow);

        create.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        create.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        create.onClick = [this] { createProject(); };
        cancel.onClick = [this] { closeToolWindowOf (*this); };
        addAndMakeVisible (create);
        addAndMakeVisible (cancel);

        if (! persistent)
        {
            name.setText ("Night Drive", false);
            genre.setText ("Synthwave", false);
            templates.selectRow (2);
        }
        templateChosen();
        setSize (700, 540);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        auto bottom = area.removeFromBottom (32);
        cancel.setBounds (bottom.removeFromRight (96));
        bottom.removeFromRight (8);
        create.setBounds (bottom.removeFromRight (128));
        dontShow.setBounds (bottom.removeFromLeft (360));
        area.removeFromBottom (10);

        auto left = area.removeFromLeft (240);
        area.removeFromLeft (20);
        templateTitle.setBounds (left.removeFromTop (26));
        left.removeFromTop (8);
        templatesFolder.setBounds (left.removeFromBottom (28));
        left.removeFromBottom (8);
        templates.setBounds (left);

        constexpr int labelWidth = 104, rowHeight = 26, gap = 6;
        auto row = [&] (juce::Label& l, juce::Component& c) {
            auto r = area.removeFromTop (rowHeight);
            l.setBounds (r.removeFromLeft (labelWidth));
            c.setBounds (r);
            area.removeFromTop (gap);
        };

        nameTitle.setBounds (area.removeFromTop (26));
        area.removeFromTop (8);
        row (nameLabel, name);
        {
            auto r = area.removeFromTop (rowHeight);
            folderLabel.setBounds (r.removeFromLeft (labelWidth));
            chooseFolder.setBounds (r.removeFromRight (90));
            r.removeFromRight (6);
            folderValue.setBounds (r);
            area.removeFromTop (gap);
        }
        auto toggles = area.removeFromTop (22).withTrimmedLeft (labelWidth);
        saveNow.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2 - 10));
        ownFolder.setBounds (toggles);
        pathNote.setBounds (area.removeFromTop (18).withTrimmedLeft (labelWidth));
        area.removeFromTop (6);

        timeTitle.setBounds (area.removeFromTop (26));
        area.removeFromTop (8);
        {
            auto r = area.removeFromTop (rowHeight);
            tempoLabel.setBounds (r.removeFromLeft (labelWidth));
            tempo.setBounds (r.removeFromLeft (150));
            area.removeFromTop (gap);
            r = area.removeFromTop (rowHeight);
            meterLabel.setBounds (r.removeFromLeft (labelWidth));
            meter.setBounds (r.removeFromLeft (150));
            area.removeFromTop (gap + 4);
        }

        infoTitle.setBounds (area.removeFromTop (26));
        area.removeFromTop (8);
        row (titleLabel, title);
        row (authorLabel, author);
        row (genreLabel, genre);
        row (urlLabel, url);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

private:
    void templateChosen()
    {
        const auto row = templates.getSelectedRow();
        if (row < 0 || row >= (int) templates.items.size())
            return;
        const auto song = makeSong (templates.items[(std::size_t) row]);
        tempo.setValue (song.bpm, juce::dontSendNotification);
        meter.setSelectedId (juce::jlimit (1, 16, song.beatsPerBar), juce::dontSendNotification);
        updatePath();
    }

    Song makeSong (const TemplateItem& item) const
    {
        if (item.builtIn >= -1) // LYNX's own templates follow Options › Mixer (round 163); a user's template stays as saved
            return makeNewProject (item.builtIn);
        juce::String error;
        if (auto song = ProjectFile::load (item.file, error))
            return std::move (*song);
        return makeNewProject();
    }

    NewProjectSetup setup() const
    {
        NewProjectSetup s;
        s.name = name.getText().trim().toStdString();
        s.folder = saveNow.getToggleState() ? (folderText.isNotEmpty() ? folderText : folder.getFullPathName()).toStdString() : std::string();
        s.ownFolder = ownFolder.getToggleState();
        s.bpm = tempo.getValue();
        s.beatsPerBar = meter.getSelectedId();
        s.info.title = title.getText().trim().toStdString();
        s.info.author = author.getText().trim().toStdString();
        s.info.genre = genre.getText().trim().toStdString();
        s.info.url = url.getText().trim().toStdString();
        return s;
    }

    void updatePath()
    {
        ownFolder.setEnabled (saveNow.getToggleState());
        folderValue.setText ("  " + (folderText.isNotEmpty() ? folderText : folder.getFullPathName()), juce::dontSendNotification);
        const auto path = newProjectPath (setup(), ProjectFile::extension);
        pathNote.setText (path.empty() ? tr ("Not saved until you choose File › Save") : juce::String::fromUTF8 (path.c_str()).replaceCharacter ('/', juce::File::getSeparatorChar()),
                          juce::dontSendNotification);
        create.setEnabled (true);
    }

    void pickFolder()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Folder for the project"), folder);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
            if (fc.getResult() == juce::File())
                return;
            folder = fc.getResult();
            folderText = {};
            saveNow.setToggleState (true, juce::dontSendNotification);
            updatePath();
        });
    }

    void createProject()
    {
        const auto row = templates.getSelectedRow();
        if (row < 0 || row >= (int) templates.items.size())
            return;
        const auto s = setup();
        auto song = makeSong (templates.items[(std::size_t) row]);
        applySetup (song, s);
        if (s.info.title.empty() && name.getText().trim().isEmpty())
            song.info.title.clear();

        juce::File file;
        if (const auto path = newProjectPath (s, ProjectFile::extension); ! path.empty())
        {
            file = juce::File (juce::String::fromUTF8 (path.c_str()).replaceCharacter ('/', juce::File::getSeparatorChar()));
            if (file.existsAsFile())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("New project"),
                                                        tr ("{0} already exists. Choose another name or folder.", { file.getFullPathName() }));
                return;
            }
        }

        if (persistent)
            saveRemembered ({ ! dontShow.getToggleState(), saveNow.getToggleState() ? folder.getFullPathName() : juce::String(), author.getText().trim(),
                              ownFolder.getToggleState() });

        auto callback = onCreate;
        closeToolWindowOf (*this); // deletes this panel
        if (callback)
            callback (std::move (song), file);
    }

    NewProjectCallback onCreate;
    bool persistent = true;
    juce::File folder;
    juce::String folderText; // snapshot mode: a fixed path to show
    SectionTitle templateTitle, nameTitle, timeTitle, infoTitle;
    TemplateList templates;
    juce::TextButton templatesFolder { tr ("Open the templates folder") };
    juce::Label nameLabel, folderLabel, folderValue, pathNote, tempoLabel, meterLabel, titleLabel, authorLabel, genreLabel, urlLabel;
    juce::TextEditor name, title, author, genre, url;
    juce::TextButton chooseFolder { tr ("Choose...") };
    juce::ToggleButton saveNow, ownFolder, dontShow;
    juce::Slider tempo;
    juce::ComboBox meter;
    juce::TextButton create { tr ("Create") }, cancel { tr ("Cancel") };
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

juce::File userTemplatesFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LYNX/Templates");
}

bool newProjectDialogEnabled()
{
    return loadRemembered().show;
}

bool saveUserTemplate (const Song& song, const juce::String& name, juce::String& error)
{
    const auto legal = juce::String::fromUTF8 (legalProjectName (name.toStdString()).c_str());
    const auto folder = userTemplatesFolder();
    if (! folder.createDirectory())
    {
        error = tr ("could not create {0}", { folder.getFullPathName() });
        return false;
    }
    return ProjectFile::save (song, folder.getChildFile (legal + ProjectFile::extension), error);
}

void showNewProjectDialog (NewProjectCallback onCreate)
{
    openToolWindow (std::make_unique<NewProjectPanel> (std::move (onCreate), true), tr ("New project"), "new-project");
}

std::unique_ptr<juce::Component> makeNewProjectPanel()
{
    return std::make_unique<NewProjectPanel> (NewProjectCallback {}, false);
}
} // namespace lynx
