#include "ui/MissingFilesDialog.h"

#include "engine/PluginHost.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

// "Used by" names come from core/MissingFiles in English ("Audio clip: Hook", "Insert 3: effect", "Master: effect")
juce::String userName (const std::string& user)
{
    auto u = u8 (user);
    juce::String suffix;
    if (u.endsWith (": effect"))
    {
        u = u.dropLastCharacters (8);
        suffix = ": " + tr ("effect");
    }
    if (u.startsWith ("Audio clip: "))
        u = tr ("Audio clip") + ": " + u.substring (12);
    else if (u == "Master")
        u = tr ("Master");
    else if (u.startsWith ("Insert ") && u.substring (7).containsOnly ("0123456789"))
        u = tr ("Insert {0}", { u.substring (7) });
    return u + suffix;
}

juce::File searchFoldersFile() { return PluginHost::settingsFolder().getChildFile ("search-folders.txt"); }

// Recursive search of folders for the missing files' names, off the UI thread.
class SearchThread final : public juce::Thread
{
public:
    SearchThread (std::vector<MissingFile> m, juce::StringArray f, std::function<void (std::vector<std::string>, bool)> done)
        : juce::Thread ("LYNX missing-files search"), missing (std::move (m)), folders (std::move (f)), onDone (std::move (done))
    {
    }

    ~SearchThread() override { stopThread (4000); }

    std::atomic<int> scanned { 0 };

    void run() override
    {
        std::vector<std::string> hits;
        auto list = [this] (const std::string& folder, const std::function<bool (const std::string&, bool)>& onEntry) {
            for (const auto& entry : juce::RangedDirectoryIterator (juce::File (u8 (folder)), false, "*",
                                                                    juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
            {
                ++scanned;
                if (! onEntry (entry.getFile().getFullPathName().toStdString(), entry.isDirectory()))
                    return;
            }
        };
        for (const auto& folder : folders)
        {
            auto found = searchFolderForMissing (missing, folder.toStdString(), list, 2000000, [this] { return threadShouldExit(); });
            hits.insert (hits.end(), found.begin(), found.end());
            if (threadShouldExit())
                break;
        }
        const auto cancelled = threadShouldExit();
        juce::MessageManager::callAsync ([done = onDone, hits = std::move (hits), cancelled]() mutable { done (std::move (hits), cancelled); });
    }

private:
    std::vector<MissingFile> missing;
    juce::StringArray folders;
    std::function<void (std::vector<std::string>, bool)> onDone;
};

class MissingFilesPanel final : public juce::Component,
                                private juce::ListBoxModel,
                                private juce::Timer
{
public:
    enum class Page { samples, plugins, found };

    MissingFilesPanel (MissingFilesInfo i, bool live) : info (std::move (i)), persistent (live)
    {
        title.setText (tr ("Missing files"), juce::dontSendNotification);
        title.setFont (theme::font (theme::type::display, true));
        addAndMakeVisible (title);
        summary.setFont (theme::font (theme::type::body));
        summary.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        summary.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (summary);

        for (auto* b : { &samplesTab, &pluginsTab, &foundTab })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (81);
            b->setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            b->setColour (juce::TextButton::textColourOnId, theme::colour::textPrimary);
            addAndMakeVisible (b);
        }
        samplesTab.onClick = [this] { showPage (Page::samples); };
        pluginsTab.onClick = [this] { showPage (Page::plugins); };
        foundTab.onClick = [this] { showPage (Page::found); };

        list.setModel (this);
        list.setRowHeight (46);
        list.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        list.setColour (juce::ListBox::outlineColourId, theme::colour::border);
        list.setOutlineThickness (1);
        list.setMultipleSelectionEnabled (true);
        addAndMakeVisible (list);

        searchButton.onClick = [this] { chooseSearchFolder(); };
        locateButton.onClick = [this] { locateSelected(); };
        skipButton.onClick = [this] { toggleSkipSelected(); };
        stopButton.onClick = [this] { stopSearch(); };
        for (auto* b : { &searchButton, &locateButton, &skipButton, &stopButton })
            addAndMakeVisible (b);
        stopButton.setVisible (false);

        status.setFont (theme::font (theme::type::small));
        status.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible (status);

        rememberToggle.setButtonText (tr ("Remember the folders I search"));
        rememberToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (rememberToggle);

        applyButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        applyButton.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        applyButton.onClick = [this] { apply(); };
        closeButton.onClick = [this] { close(); };
        addAndMakeVisible (applyButton);
        addAndMakeVisible (closeButton);

        if (persistent)
            folders = loadSearchFolders();

        showPage (! info.files.empty() ? Page::samples : ! info.plugins.empty() ? Page::plugins : Page::found);
        refresh();
        setSize (760, 560);
    }

    ~MissingFilesPanel() override
    {
        stopTimer();
        search.reset();
    }

    std::function<void (const std::map<std::string, std::string>&)> onApply;
    std::function<void()> onCloseRequest; // the window's; none in snapshots

    void showPageForSnapshot (int p, int row)
    {
        showPage (p == 1 ? Page::plugins : p == 2 ? Page::found : Page::samples);
        if (row >= 0)
            list.selectRow (row);
        refresh();
        if (p == 0)
            status.setText (tr ("{0} still missing, {1} found", { "2", "2" }), juce::dontSendNotification);
    }

    // the remembered search folders and the Browser's folders, searched without asking
    void searchRememberedFolders()
    {
        juce::StringArray all (info.searchFolders);
        for (const auto& f : folders)
            all.addIfNotAlreadyThere (u8 (f));
        all.addArray (browserSearchFolders());
        all.removeDuplicates (true);
        for (int i = all.size(); --i >= 0;)
            if (! juce::File (all[i]).isDirectory())
                all.remove (i);
        if (! all.isEmpty() && stillMissing() > 0)
            startSearch (all, false);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        // header band with the accent mark
        g.setColour (theme::colour::bg3);
        g.fillRect (header);
        g.setColour (theme::colour::accent);
        g.fillRect (header.withWidth (4));
        g.setColour (theme::colour::border);
        g.drawHorizontalLine (header.getBottom(), 0.0f, (float) getWidth());
        g.drawHorizontalLine (footer.getY() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        header = area.removeFromTop (56 + 18 * juce::jmax (1, summaryLines));
        {
            auto h = header.reduced (20, 12);
            h.removeFromLeft (4);
            title.setBounds (h.removeFromTop (28));
            summary.setBounds (h);
        }
        footer = area.removeFromBottom (54);
        {
            auto f = footer.reduced (16, 12);
            closeButton.setBounds (f.removeFromRight (150));
            f.removeFromRight (8);
            applyButton.setBounds (f.removeFromRight (150));
            rememberToggle.setBounds (f.removeFromLeft (280));
        }
        area.reduce (16, 12);
        {
            auto tabs = area.removeFromTop (28);
            for (auto* b : { &samplesTab, &pluginsTab, &foundTab })
            {
                b->setBounds (tabs.removeFromLeft (juce::jmax (120, juce::GlyphArrangement::getStringWidthInt (juce::Font (theme::font (theme::type::body)), b->getButtonText()) + 28)));
                tabs.removeFromLeft (6);
            }
        }
        area.removeFromTop (10);
        auto actions = area.removeFromBottom (30);
        area.removeFromBottom (10);
        list.setBounds (area);
        if (page == Page::samples)
        {
            searchButton.setBounds (actions.removeFromLeft (150));
            actions.removeFromLeft (8);
            locateButton.setBounds (actions.removeFromLeft (110));
            actions.removeFromLeft (8);
            skipButton.setBounds (actions.removeFromLeft (110));
            actions.removeFromLeft (12);
        }
        stopButton.setBounds (actions.removeFromRight (80));
        status.setBounds (actions);
    }

private:
    int stillMissing() const
    {
        return (int) std::count_if (info.files.begin(), info.files.end(), [] (const MissingFile& m) { return m.found.empty() && ! m.skipped; });
    }
    int foundCount() const
    {
        return (int) std::count_if (info.files.begin(), info.files.end(), [] (const MissingFile& m) { return ! m.found.empty() && ! m.skipped; });
    }

    void refresh()
    {
        const auto missing = stillMissing(), found = foundCount();
        juce::StringArray lines;
        if (! info.files.empty())
            lines.add (tr ("{0} sample(s) used by {1} could not be found on this PC.",
                           { juce::String ((int) info.files.size()), info.projectName.isNotEmpty() ? info.projectName : tr ("this project") }));
        if (! info.plugins.empty())
            lines.add (tr ("{0} plugin(s) will not sound as in FL Studio (not installed, not loaded, or without their settings): see what LYNX SHARE did with each.",
                           { juce::String ((int) info.plugins.size()) }));
        if (! info.autoRelocated.empty())
            lines.add (tr ("{0} file(s) were found automatically in another folder.", { juce::String ((int) info.autoRelocated.size()) }));
        if (lines.isEmpty())
            lines.add (tr ("Nothing is missing: every sample and plugin this project uses was found."));
        summaryLines = lines.size();
        summary.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);

        samplesTab.setButtonText (tr ("Samples ({0})", { juce::String ((int) info.files.size()) }));
        pluginsTab.setButtonText (tr ("Plugins ({0})", { juce::String ((int) info.plugins.size()) }));
        foundTab.setButtonText (tr ("Found automatically ({0})", { juce::String ((int) info.autoRelocated.size()) }));

        const auto searching = search != nullptr;
        if (! searching && page == Page::samples)
            status.setText (info.files.empty() ? juce::String()
                            : missing == 0         ? tr ("Every sample has a place. Apply to use them.")
                                                   : tr ("{0} still missing, {1} found", { juce::String (missing), juce::String (found) }),
                            juce::dontSendNotification);
        const auto samplesPage = page == Page::samples;
        for (auto* b : { &searchButton, &locateButton, &skipButton })
            b->setVisible (samplesPage);
        searchButton.setEnabled (! searching && missing > 0);
        locateButton.setEnabled (! searching && list.getNumSelectedRows() > 0);
        skipButton.setEnabled (list.getNumSelectedRows() > 0);
        stopButton.setVisible (searching && samplesPage);
        if (page == Page::plugins)
            status.setText (info.plugins.empty() ? juce::String() : tr ("Install them, scan for plugins (Options menu), then open the project again."), juce::dontSendNotification);
        else if (page == Page::found)
            status.setText (info.autoRelocated.empty() ? juce::String() : tr ("Already in use: nothing to do. Save the project to keep the new places."), juce::dontSendNotification);

        applyButton.setButtonText (found > 0 ? tr ("Apply ({0})", { juce::String (found) }) : tr ("Apply"));
        applyButton.setEnabled (found > 0 && ! searching);
        closeButton.setButtonText (found > 0 ? tr ("Close without changes") : tr ("Close"));
        rememberToggle.setVisible (! info.files.empty());
        list.updateContent();
        list.repaint();
        resized();
    }

    void showPage (Page p)
    {
        page = p;
        (p == Page::samples ? samplesTab : p == Page::plugins ? pluginsTab : foundTab).setToggleState (true, juce::dontSendNotification);
        list.deselectAllRows();
        list.setMultipleSelectionEnabled (p == Page::samples);
        refresh();
    }

    // --- list ---------------------------------------------------------------------------------
    int getNumRows() override
    {
        return page == Page::samples ? (int) info.files.size() : page == Page::plugins ? (int) info.plugins.size() : (int) info.autoRelocated.size();
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (selected)
        {
            g.setColour (theme::colour::accentMuted);
            g.fillRect (0, 0, width, height);
            g.setColour (theme::colour::accent);
            g.fillRect (0, 3, 3, height - 6);
        }
        else if (row % 2 == 1)
        {
            g.setColour (theme::colour::bg1);
            g.fillRect (0, 0, width, height);
        }

        juce::Colour dot;
        juce::String name, right, detail;
        juce::Colour detailColour = theme::colour::textDisabled;

        if (page == Page::samples)
        {
            const auto& m = info.files[(std::size_t) row];
            name = u8 (pathFileName (m.path));
            juce::StringArray users;
            for (const auto& u : m.usedBy)
                users.add (userName (u));
            right = users.joinIntoString (", ");
            if (m.skipped)
            {
                dot = theme::colour::textDisabled;
                detail = tr ("Skipped: stays silent") + "   " + u8 (m.path);
            }
            else if (! m.found.empty())
            {
                dot = theme::colour::ledOn;
                detail = juce::String::fromUTF8 ("\xe2\x86\x92 ") + u8 (m.found);
                detailColour = theme::colour::ledOn.withAlpha (0.85f);
            }
            else
            {
                dot = theme::colour::warning;
                detail = u8 (m.path);
            }
        }
        else if (page == Page::plugins)
        {
            const auto& p = info.plugins[(std::size_t) row];
            dot = theme::colour::warning;
            name = u8 (p.name);
            juce::StringArray users;
            for (const auto& u : p.usedBy)
                users.add (userName (u));
            right = users.joinIntoString (", ");
            detail = tr ("Replaced by: {0}", { p.replacementArg.empty() ? tr (u8 (p.replacement)) : tr (p.replacement.c_str(), { u8 (p.replacementArg) }) });
            if (! p.replacementArg.empty())
                dot = theme::colour::info; // something plays in its place
            detailColour = theme::colour::textSecondary;
        }
        else
        {
            const auto& [from, to] = info.autoRelocated[(std::size_t) row];
            dot = theme::colour::info;
            name = u8 (pathFileName (to));
            detail = u8 (from) + juce::String::fromUTF8 ("  \xe2\x86\x92  ") + u8 (to);
        }

        g.setColour (dot);
        g.fillEllipse (14.0f, 12.0f, 8.0f, 8.0f);

        const auto rightWidth = juce::jmin (width / 3, juce::GlyphArrangement::getStringWidthInt (juce::Font (theme::font (theme::type::small)), right) + 8);
        g.setFont (theme::font (theme::type::body, true));
        g.setColour (theme::colour::textPrimary);
        g.drawText (name, 32, 5, width - 44 - rightWidth, 18, juce::Justification::centredLeft, true);
        g.setFont (theme::font (theme::type::small));
        g.setColour (theme::colour::textSecondary);
        g.drawText (right, width - 12 - rightWidth, 5, rightWidth, 18, juce::Justification::centredRight, true);
        g.setColour (detailColour);
        g.drawText (detail, 32, 24, width - 44, 16, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int) override { refresh(); }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (page == Page::samples && search == nullptr)
        {
            list.selectRow (row);
            locateSelected();
        }
    }

    juce::String getTooltipForRow (int row) override
    {
        if (page == Page::samples && row < (int) info.files.size())
            return u8 (info.files[(std::size_t) row].path);
        return {};
    }

    // --- actions ------------------------------------------------------------------------------
    void chooseSearchFolder()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Search a folder (and its sub-folders) for the missing samples"),
                                                       folders.empty() ? juce::File::getSpecialLocation (juce::File::userHomeDirectory) : juce::File (u8 (folders.front())));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<MissingFilesPanel> (this)] (const juce::FileChooser& c) {
                                  if (safe == nullptr || ! c.getResult().isDirectory())
                                      return;
                                  safe->remember (c.getResult());
                                  safe->startSearch ({ c.getResult().getFullPathName() }, true);
                              });
    }

    void locateSelected()
    {
        const auto row = list.getSelectedRow();
        if (page != Page::samples || row < 0 || row >= (int) info.files.size())
            return;
        const auto wanted = u8 (pathFileName (info.files[(std::size_t) row].path));
        const auto start = folders.empty() ? juce::File::getSpecialLocation (juce::File::userHomeDirectory) : juce::File (u8 (folders.front()));
        chooser = std::make_unique<juce::FileChooser> (tr ("Where is {0}?", { wanted }), start.getChildFile (wanted),
                                                       "*" + juce::File (wanted).getFileExtension());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<MissingFilesPanel> (this), row] (const juce::FileChooser& c) {
                                  if (safe == nullptr || ! c.getResult().existsAsFile())
                                      return;
                                  safe->located (row, c.getResult());
                              });
    }

    // a file picked by hand: used for that row, and its folder is looked at for the others (as FL does)
    void located (int row, const juce::File& file)
    {
        auto& m = info.files[(std::size_t) row];
        m.found = file.getFullPathName().toStdString();
        m.skipped = false;
        std::vector<std::string> siblings;
        for (const auto& entry : juce::RangedDirectoryIterator (file.getParentDirectory(), false, "*", juce::File::findFiles))
            siblings.push_back (entry.getFile().getFullPathName().toStdString());
        const auto more = relocateByName (info.files, siblings);
        remember (file.getParentDirectory());
        refresh();
        if (more > 0)
            status.setText (tr ("{0} more found in the same folder", { juce::String (more) }), juce::dontSendNotification);
    }

    void toggleSkipSelected()
    {
        const auto rows = list.getSelectedRows();
        auto allSkipped = true;
        for (int i = 0; i < rows.size(); ++i)
            allSkipped = allSkipped && info.files[(std::size_t) rows[i]].skipped;
        for (int i = 0; i < rows.size(); ++i)
        {
            auto& m = info.files[(std::size_t) rows[i]];
            m.skipped = ! allSkipped;
            if (m.skipped)
                m.found.clear();
        }
        refresh();
    }

    void remember (const juce::File& folder)
    {
        if (! persistent || ! rememberToggle.getToggleState())
            return;
        rememberFolder (folders, folder.getFullPathName().toStdString());
        saveSearchFolders (folders);
    }

    void startSearch (const juce::StringArray& where, bool userPicked)
    {
        if (search != nullptr)
            return;
        searchedBy = userPicked;
        search = std::make_unique<SearchThread> (info.files, where,
                                                 [safe = juce::Component::SafePointer<MissingFilesPanel> (this)] (std::vector<std::string> hits, bool cancelled) {
                                                     if (safe != nullptr)
                                                         safe->searchFinished (std::move (hits), cancelled);
                                                 });
        searchedFolders = where;
        search->startThread();
        startTimerHz (8);
        refresh();
        timerCallback();
    }

    void stopSearch()
    {
        if (search != nullptr)
            search->signalThreadShouldExit();
    }

    void timerCallback() override
    {
        if (search != nullptr)
            status.setText (tr ("Searching {0}... {1} files looked at",
                                { searchedFolders.size() == 1 ? searchedFolders[0] : tr ("your search folders"), juce::String (search->scanned.load()) }),
                            juce::dontSendNotification);
    }

    void searchFinished (std::vector<std::string> hits, bool cancelled)
    {
        stopTimer();
        search.reset();
        const auto found = relocateByName (info.files, hits);
        refresh();
        status.setText ((cancelled ? tr ("Search stopped.") + " " : juce::String())
                            + tr ("{0} found, {1} still missing", { juce::String (found), juce::String (stillMissing()) }),
                        juce::dontSendNotification);
    }

    void apply()
    {
        const auto map = relocationMap (info.files);
        if (onApply && ! map.empty())
            onApply (map);
        close();
    }

    void close()
    {
        stopSearch();
        if (onCloseRequest)
            onCloseRequest();
    }

    MissingFilesInfo info;
    bool persistent = false, searchedBy = false;
    Page page = Page::samples;
    std::vector<std::string> folders;
    juce::StringArray searchedFolders;
    std::unique_ptr<SearchThread> search;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> header, footer;
    int summaryLines = 1;

    juce::Label title, summary, status;
    juce::TextButton samplesTab, pluginsTab, foundTab;
    juce::ListBox list;
    juce::TextButton searchButton { tr ("Search folder...") }, locateButton { tr ("Locate...") }, skipButton { tr ("Skip") },
        stopButton { tr ("Stop") };
    juce::ToggleButton rememberToggle;
    juce::TextButton applyButton, closeButton;
};

// closes like the panel's buttons and reports when it goes away
class MissingFilesWindow final : public juce::DocumentWindow
{
public:
    MissingFilesWindow (std::unique_ptr<MissingFilesPanel> panel, std::function<void()> closed)
        : juce::DocumentWindow (tr ("Missing files"), theme::colour::bg2, juce::DocumentWindow::closeButton, true), onClosed (std::move (closed))
    {
        panel->onCloseRequest = [this] { closeWindow(); };
        setUsingNativeTitleBar (true);
        setContentOwned (panel.release(), true);
        setResizable (true, false);
        setResizeLimits (620, 420, 1600, 1200);
        centreWithSize (getWidth(), getHeight());
    }

    void closeButtonPressed() override { closeWindow(); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            closeWindow();
            return true;
        }
        return juce::DocumentWindow::keyPressed (key);
    }

    void closeWindow()
    {
        if (! isVisible())
            return;
        setVisible (false);
        if (onClosed)
            onClosed();
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)] { delete safe.getComponent(); });
    }

private:
    std::function<void()> onClosed;
};
} // namespace

std::vector<std::string> loadSearchFolders()
{
    return parseFolderList (searchFoldersFile().loadFileAsString().toStdString());
}

void saveSearchFolders (const std::vector<std::string>& folders)
{
    searchFoldersFile().replaceWithText (u8 (folderListText (folders)));
}

juce::StringArray browserSearchFolders()
{
    juce::StringArray list;
    list.addLines (PluginHost::settingsFolder().getChildFile ("browser-folders.txt").loadFileAsString());
    list.trim();
    list.removeEmptyStrings();
    return list;
}

void showMissingFilesDialog (MissingFilesInfo info, std::function<void (const std::map<std::string, std::string>&)> onApply,
                             std::function<void()> onClosed)
{
    auto panel = std::make_unique<MissingFilesPanel> (std::move (info), true);
    panel->onApply = std::move (onApply);
    auto* raw = panel.get();
    auto* window = new MissingFilesWindow (std::move (panel), std::move (onClosed));
    window->setVisible (true);
    keepAboveMainWindow (*window);
    raw->searchRememberedFolders();
}

std::unique_ptr<juce::Component> makeMissingFilesPanel (MissingFilesInfo info, int page, int selectedRow)
{
    auto panel = std::make_unique<MissingFilesPanel> (std::move (info), false);
    panel->showPageForSnapshot (page, selectedRow);
    return panel;
}

MissingFilesInfo missingFilesSnapshotInfo()
{
    MissingFilesInfo info;
    info.projectName = "Night Drive";
    info.files = {
        { "C:\\Users\\Producer\\Samples\\Drums\\Kicks\\Kick Punchy 04.wav", { "Kick" }, "D:\\Library\\Drums\\Kicks\\Kick Punchy 04.wav", false },
        { "C:\\Users\\Producer\\Samples\\Drums\\Snares\\Snare Tight 12.wav", { "Snare", "Snare 2" }, {}, false },
        { "C:\\Users\\Producer\\Samples\\Vocals\\Hook take 3.wav", { "Audio clip: Hook take 3" }, {}, false },
        { "C:\\Users\\Producer\\Samples\\FX\\Riser long.wav", { "Riser" }, {}, true },
        { "E:\\Old drive\\Loops\\Pad texture 90bpm.wav", { "Pad", "Insert 4: Newtone" }, "D:\\Library\\Loops\\Pad texture 90bpm.wav", false },
    };
    info.plugins = {
        { "Serum", { "Lead", "Pluck" }, "Built-in synth (a preset chosen by the channel name)" },
        { "ValhallaRoom", { "Insert 3" }, "Nothing: the slot was left out of the mixer" },
    };
    info.autoRelocated = {
        { "C:\\Users\\Studio PC\\Documents\\Image-Line\\FL Studio\\Audio\\Sliced audio\\Break 2.wav",
          "C:\\Users\\me\\Documents\\Image-Line\\FL Studio\\Audio\\Sliced audio\\Break 2.wav" },
        { "C:\\Projects\\Night Drive\\Samples\\Chord stab.wav", "D:\\Music\\Night Drive\\Samples\\Chord stab.wav" },
    };
    return info;
}
} // namespace lynx
