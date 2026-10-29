#pragma once

#include <memory>
#include <set>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/BrowserView.h"
#include "engine/AudioEngine.h"
#include "ui/Icons.h"

namespace lynx::search
{
struct SearchResult;
struct FolderMatches;
}

namespace lynx
{
// File browser (like FL's Browser): drawn by LYNX itself so it can show what the mouse is over and explain it
// in the hint bar.
//  - three tabs, as FL's Browser: STARRED (the files and folders starred from their right-click menu), ALL (the current project, then every sample folder: FL's folders, Documents,
//    Music, Desktop, Downloads and the user's folders, each with its own colour) and PROJECT (only the current
//    project, opened)
//  - "Current project" (REA_rodada115): history, patterns, effects, generators, automation, the project folder
//    and the audio files the project uses, each item in the colour of what it belongs to, with a short detail
//    (the insert, how often it is used, "missing"); click / double-click / drag / right-click act on it
//  - a click on a folder opens / closes it; a click on an audio file previews it (the strip at the bottom
//    shows its waveform and the playback position); double-click: new sampler channel with it
//  - drag a file onto the Channel Rack (new channel, or onto a row: replaces its sample), the Playlist (audio
//    clip where it is dropped) or a mixer insert (effect presets); drag a pattern onto the Playlist (a clip)
//  - double-click a .lynx/.flp/.mid: opens it (onOpenProject)
//  - search box (Ctrl+F): ALL searches the folder of the selection (or every folder) and below
//    (core/SearchQuery syntax) through the search index (core/SearchService: a background worker, round 162);
//    PROJECT filters the project's items; Esc clears and leaves the box, Enter leaves it; a click anywhere else
//    leaves it too, so Space is the transport again (issue #9)
//  - keys while the Browser has focus: Up/Down step through the rows and preview each sample, Shift+Up/Down step
//    and load each file into the selected channel, Enter loads the selected file into the selected channel,
//    Left/Right close / open folders (Left on a file or a closed folder: to its folder)
//  - the Plugin database's plugins can be dragged onto the Channel Rack (generators) and the Mixer (effects)
class BrowserComponent final : public juce::Component,
                               private juce::TextEditor::Listener,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    // The main Browser (a panel) or the separate Sample browser window (round 163): each keeps its own view (which
    // top-level folders it shows, which kinds of files it lists).
    enum class Role
    {
        main,
        samples
    };
    explicit BrowserComponent (AudioEngine& engine, Role role = Role::main);
    ~BrowserComponent() override;
    Role getRole() const noexcept { return role; }

    // What this Browser shows (round 163): the content filter, the hidden top-level entries; saved at once.
    const browserview::View& getView() const noexcept { return view; }
    void setContentFilter (browserview::ContentFilter filter);
    void setRootShown (const juce::String& id, bool shown);
    void showAllRoots();
    // The Browser's options menu (the button beside the search box, the panel menu): filter, folders, search index.
    void addViewItems (juce::PopupMenu& menu);
    void showViewMenu();
    // The Sample browser window opens / closes from here (the main window sets it): View › Sample browser.
    std::function<void()> onToggleSampleBrowser;
    // The Sample browser window's content: a drag container holding a Browser of role `samples`; `wire` connects
    // its callbacks like the main Browser's. The window keeps its bounds and open state in the view file.
    static std::unique_ptr<juce::Component> makeSampleBrowserContent (AudioEngine& engine, const std::function<void (BrowserComponent&)>& wire);
    // Whether the Sample browser was open when LYNX last closed (its view file).
    static bool sampleBrowserWasOpen();
    static juce::Rectangle<int> sampleBrowserBounds(); // its last window bounds (empty: default)

    std::function<void (const juce::File&)> onOpenProject;
    // Enter / Shift+Up/Down: put this file into the selected channel; returns false if it can't take it.
    std::function<bool (const juce::File&)> onLoadIntoChannel;
    // Current project items (set by the main window)
    std::function<void (int)> onOpenPattern;     // double-click a pattern: it opens in the Piano Roll
    std::function<void (int)> onPatternMenu;     // right-click a pattern (already selected): the pattern menu
    std::function<void (int)> onOpenChannel;     // double-click a generator: its settings / plugin window
    std::function<void (int, int)> onOpenEffect; // double-click an effect (insert, slot): its editor
    std::function<void (int)> onShowInsert;      // an effect's / generator's insert in the Mixer
    std::function<void (int)> onOpenAutomation;  // double-click an automation clip: its editor
    // The Plugin database (round 142): double-click a generator = a new channel, an effect = into the selected insert
    std::function<void (ChannelDef)> onAddInstrument;
    std::function<void (EffectSlot)> onAddEffect;
    std::function<void (const juce::File&)> onSendToPlaylist; // a sample's "Send to the Playlist as an audio clip"
    std::function<void()> onPreviewVolumeChanged;             // the preview volume knob moved (the main window saves it)

    // The sample preview volume knob beside the search box (0..125%, engine.setPreviewVolume): follows the engine.
    void refreshPreviewVolume();
    void stopPreview(); // the Stop button beside it
    // F2 on a Plugin database entry: rename it (FL's renamed plugins, issue #11); false when no plugin is selected
    bool renameSelectedPlugin();

    // The search index (round 162): a background pass over every Browser folder (rebuild: from scratch), and its
    // state in words ("12,345 files in 678 folders, checked 14:03").
    void refreshSearchIndex (bool rebuild);
    juce::String searchIndexStatus() const;
    // The search box has the keyboard: it gives it back to the main window (a click elsewhere, Esc, Enter).
    void leaveSearchBox();
    bool isTypingInSearch() const;

    // The project's file (.lynx, or the imported .flp): its folder shows as "Project folder".
    void setProjectFile (const juce::File& file);

    enum Tab
    {
        allTab,
        projectTab,
        starredTab, // the files and folders the user starred (FL's STARRED)
        tabCount
    };
    void setTab (int tab);
    int getTab() const noexcept { return tab; }

    void focusSearch();
    void addUserFolder(); // asks for a folder and adds it to the folder list
    void reloadUserFolders() { rebuildRoots(); } // after the list changed elsewhere (Settings > Files)
    void setSearchForSnapshot (const juce::String& text);
    // For snapshots: selects (and "previews" without sound) the first audio file under the first folder,
    // and puts the mouse over the row after it.
    void selectFirstSampleForSnapshot();
    // --snapshot: the PROJECT tab with the patterns, generators and samples open, a sample selected
    void showProjectForSnapshot();
    void showPluginDatabaseForSnapshot();
    void showPluginDatabase(); // FL's Shift+F8: the All tab with the Plugin database opened and scrolled into view
    // --snapshot (round 164): only `folder`, opened, listed under `filter` (folders with nothing to list left out)
    void showFilterForSnapshot (const juce::File& folder, browserview::ContentFilter filter);
    void resetViewForSnapshot(); // its Generators / Effects FL engines open (round 142)
    // --snapshot: shows this audio file in the preview strip (without playing it)
    void showPreviewForSnapshot (const juce::File& file);
    // Up/Down (load = false) or Shift+Up/Down (load = true); Enter = loadSelected()
    void step (int delta, bool load);
    void loadSelected();
    juce::File getSelectedFile() const;
    // Left / Right: close / open the selected folder (Left on a file goes to its folder)
    void openFolder (bool open);

    // Drags from the Browser (tree or search results): is it one, and which file it carries.
    static bool isBrowserDrag (const juce::DragAndDropTarget::SourceDetails& details);
    static juce::File draggedFile (const juce::DragAndDropTarget::SourceDetails& details);
    // A pattern dragged from "Current project > Patterns": its index, else -1.
    static int draggedPattern (const juce::DragAndDropTarget::SourceDetails& details);
    // A plugin dragged from the Plugin database (round 162): a generator (a new channel) or an effect.
    static bool isGeneratorDrag (const juce::DragAndDropTarget::SourceDetails& details);
    static bool isEffectDrag (const juce::DragAndDropTarget::SourceDetails& details);
    static std::optional<ChannelDef> draggedGenerator (const juce::DragAndDropTarget::SourceDetails& details);
    static std::optional<EffectSlot> draggedEffect (const juce::DragAndDropTarget::SourceDetails& details);
    static bool isAudioFile (const juce::File& file);

    void resized() override;

    // --snapshot: the tree is listed synchronously; kept for the snapshot code
    void waitForListing (int timeoutMs);
    void paint (juce::Graphics& g) override;

    static constexpr const char* dragDescription = "lynx-arquivo";
    static constexpr const char* patternDragDescription = "lynx-pattern";
    static constexpr const char* generatorDragDescription = "lynx-plugin-generator";
    static constexpr const char* effectDragDescription = "lynx-plugin-effect";
    static constexpr int addFolderId = 1000;

    class FileTree;     // the folder tree / search results (implementation detail)
    class PreviewStrip; // waveform of the selected sample
    class TabBar;       // ALL / PROJECT / STARRED
    class DetailsBox;   // date, size / version and folder of the selection (FL's details box)
    class FoldersPanel; // the "Show folders" window: which folders this Browser shows, the user's own folders (round 164)

    // The "Show folders" window of this Browser (one per Browser), and its content for --snapshot.
    void showFoldersDialog();
    std::unique_ptr<juce::Component> makeFoldersPanelForSnapshot();

    // starred files and folders (one path per line in the settings folder)
    bool isStarred (const juce::File& file) const { return starred.contains (file.getFullPathName()); }
    void setStarred (const juce::File& file, bool on);

private:
    void textEditorTextChanged (juce::TextEditor&) override;
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void runSearch (bool quiet = false); // quiet: the index changed, the results are replaced without "Searching..."
    void searchAnswered (const search::SearchResult& result, const juce::String& query, const juce::Array<juce::File>& scope);
    void indexStatusChanged();
    juce::String statusLine; // under the search box: indexing progress / what the search found
    void showNote (const juce::String& text); // a short note there (files copied in), gone after a few seconds
    juce::Rectangle<int> statusArea;
    void rebuildRoots();
    void rebuildTree();
    static juce::File userFoldersFile();
    static juce::File starredFile();
    static juce::File viewFile (Role role);
    void loadView();
    void saveView() const;
    bool viewLoaded = false, viewFromFile = false;
    bool lists (const juce::File& file) const; // a file listed under the content filter
    // round 164: a folder whose files are all left out by the content filter (from the search index; unknown: false)
    bool filterHides (const juce::File& folder) const;
    std::shared_ptr<const search::FolderMatches> filterMatches() const;
    void applyFilterToTree();
    void removeUserFolder (const juce::File& folder); // takes one of the user's own folders out of the Browsers
    juce::Component::SafePointer<juce::Component> foldersPanel;
    void refreshFoldersPanel();
    struct FolderEntry
    {
        juce::String id, name, path;
        bool shown = true, user = false, empty = false; // empty: nothing in it is listed under the content filter
    };
    std::vector<FolderEntry> folderEntries() const; // the open "Show folders" window's content // drops the folders the index now knows to be empty under the filter
    std::uint64_t prunedGeneration = 0;
    void addHideItem (juce::PopupMenu& menu, const juce::String& id, const juce::String& label);
    juce::String rootId (const juce::File& folder) const; // the root's id, empty if `folder` isn't a root
    juce::StringArray starred;
    void openFile (const juce::File& file); // what a double-click does
    void fileSelected (const juce::File& file, bool preview);
    void showFolderMenu (const juce::File& folder, std::function<void()> expandAll = {});
    void showFileMenu (const juce::File& file);
    void addSystemItems (juce::PopupMenu& menu, const juce::File& file);
    void confirmDeleteFile (const juce::File& file);
    FileTree& activeList() const;
    juce::Array<juce::File> searchRoots() const;
    juce::File previewedFile; // the sample last sent to the preview player
    bool isPreviewPlaying() const;

    std::unique_ptr<FileTree> tree, results;
    std::unique_ptr<juce::Viewport> treeView, resultsView;
    std::unique_ptr<PreviewStrip> strip;
    std::unique_ptr<DetailsBox> details;
    std::unique_ptr<TabBar> tabs;
    struct SearchShared; // the process's search worker (core/SearchService), shared by every Browser
    std::shared_ptr<SearchShared> searcher;
    std::uint64_t searchId = 0;
    bool searchPartial = false;
    juce::TextEditor searchBox;
    class FocusWatcher; // a click outside the search box takes the keyboard back from it
    std::unique_ptr<FocusWatcher> focusWatcher;
    IconButton stopButton { "stop preview", Icon::stop };
    IconButton viewButton { "browser options", Icon::panelMenu };
    void refreshViewButton();
    juce::Slider previewVolume { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    std::unique_ptr<juce::FileChooser> chooser;
    int tab = allTab;
    bool searchPending = false, projectDirty = false;
    juce::File projectFile;
    std::set<juce::String> openPluginFolders; // the Plugin database folders the user opened (round 142)

    AudioEngine& engine;
    Role role = Role::main;
    browserview::View view;
    juce::Array<juce::File> roots; // the folder roots shown
    juce::StringArray rootNames;
    juce::Array<juce::File> allRoots; // every folder root (hidden ones too)
    juce::StringArray allRootNames;
    std::vector<browserview::RootKind> allRootKinds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserComponent)
};
} // namespace lynx
