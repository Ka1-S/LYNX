#pragma once

#include <atomic>
#include <map>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/KeyMap.h"
#include "ui/UiTrace.h"
#include "core/ToolbarLayout.h"
#include "core/ValueMath.h"
#include "engine/AudioEngine.h"
#include "engine/MidiFileIo.h"
#include "ui/BrowserComponent.h"
#include "ui/Icons.h"
#include "ui/ChannelRackComponent.h"
#include "ui/PianoRollComponent.h"
#include "ui/MixerComponent.h"
#include "ui/PlaylistComponent.h"
#include "ui/HintBar.h"
#include "ui/Splash.h"
#include "ui/Toolbar.h"
#include "ui/ValueControl.h"
#include "ui/Workspace.h"

namespace lynx
{
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            public ui::HintSource,
                            private juce::MenuBarModel,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    // `restoreUserSettings`: load/save the workspace layout, key map and UI options from the user
    // settings (off for --snapshot, which must look the same on every machine).
    explicit MainComponent (AudioEngine& engine, bool restoreUserSettings = true);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override; // right-click on the toolbar: the toolbar menu
    bool keyPressed (const juce::KeyPress& key) override;
    bool keyStateChanged (bool isKeyDown) override;
    ui::Hint hintAt (juce::Point<int> position) override; // the toolbar's background: how to edit it

    // Opens .lynx or .flp (also used by the command line).
    void openFile (const juce::File& file);
    void showSetupWizard(); // first run, or Options > Setup wizard (an overlay over this component)
    void closeSetupWizardForSnapshot() { setupOverlay.reset(); }

    // Asks "save changes?" if there are any; `then` runs unless the user cancels.
    void confirmDiscard (std::function<void()> then);

    // For --snapshot mode: 0 = Channel Rack, 1 = Piano Roll (on the channel with the most notes), 2 = Playlist, 3 = Mixer,
    // each maximized on the workspace; showDesktopForSnapshot shows the default layout (with `extraPanels`, the
    // Piano Roll and Mixer floating too).
    void selectTabForSnapshot (int tab);
    void showDesktopForSnapshot (bool extraPanels);
    void openQuickAddForTest() { showQuickAdd(); } // --windowtest
    void waitForBrowserForSnapshot() { browser.waitForListing (5000); }
    juce::StringArray getMenuNamesForSnapshot() { return getMenuBarNames(); }
    juce::PopupMenu getMenuForSnapshot (int index) { return getMenuForIndex (index, {}); }
    juce::PopupMenu getPanelMenuForSnapshot (workspace::Panel panel)
    {
        juce::PopupMenu menu;
        addPanelMenuItems (panel, menu);
        return menu;
    }
    juce::PopupMenu getRecordMenuForSnapshot()
    {
        juce::PopupMenu menu;
        addRecordOptions (menu);
        return menu;
    }
    void setHintForSnapshot (const ui::Hint& hint, bool live = false)
    {
        hintsFrozen = true; // the timer would replace it with the hint under the (off-screen) mouse
        hintBar.setHint (hint, live);
    }
    juce::Rectangle<int> getToolbarBoundsForSnapshot() const { return { 0, 0, getWidth(), toolbarHeight }; }
    // every visible control with a quick tip (toolbar and panel title bars), in this component's coordinates
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> getQuickTipsForSnapshot();
    // --snapshot: a toolbar layout (icon size, hidden/reordered groups) without saving it
    void setToolbarLayoutForSnapshot (const toolbarlayout::Layout& layout) { setToolbarLayout (layout, false); }
    juce::PopupMenu getToolbarMenuForSnapshot() { return makeToolbarMenu(); }
    void setToolbarEditingForSnapshot (bool editing) { setToolbarEditing (editing); }
    int getToolbarAndTrayHeightForSnapshot() const { return toolbarHeight + (toolbarEditor != nullptr ? toolbar::EditOverlay::trayHeight : 0); }
    juce::Rectangle<int> getToolStripAreaForSnapshot (bool playlistStrip);
    void setMetersForSnapshot (double cpu, std::int64_t memory, int polyphony) { perfMeter.setValues (cpu, memory, polyphony); }
    static std::unique_ptr<juce::Component> makeHelpContent();
    static std::unique_ptr<juce::AlertWindow> makeAboutWindow();
    std::unique_ptr<juce::AlertWindow> makeConfirmDiscardWindow();
    PianoRollComponent& getPianoRollForSnapshot() { return pianoRoll; }
    ChannelRackComponent& getRackForSnapshot() { return rack; }
    void scrollRackForSnapshot (int x) { rackViewport.setViewPosition (x, rackViewport.getViewPositionY()); }
    BrowserComponent& getBrowserForSnapshot() { return browser; }
    MixerComponent& getMixerForSnapshot() { return mixer; }
    PlaylistComponent& getPlaylistForSnapshot() { return playlist; }
    juce::Component& getRackViewportForSnapshot() { return rackViewport; }
    std::unique_ptr<juce::Component> makeQuickAddForSnapshot();
    PatternPicker& getPickerForSnapshot() { return picker; }
    void selectMixerTrackForSnapshot (int track, int slot = -1)
    {
        mixer.selectTrack (track);
        if (slot >= 0)
            mixer.selectSlot (slot);
    }

private:
    using Panel = workspace::Panel;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void showAudioSettings();
    void showImportReport();
    void importFlp (const juce::File& file);
    void flpImported (const juce::File& file); // after the import: title, recent files, missing files
    void openLynxProject (const juce::File& file);
    // The "Missing files" window (ui/MissingFilesDialog.h) for the current project; `afterOpening`: shown by opening a
    // project (only when something is missing; the FL import report follows it), else from the menu (always).
    void showMissingFiles (bool afterOpening);
    void openSettings (int page); // ui/SettingsDialog.h SettingsPage: 0 Project, 1 Info, 2 Files, 3 General, 4 Audio, 5 MIDI
    void showPluginManager();
    void showShortcutManager();
    int scansSeen = 0; // background plugin scans already announced in the hint bar
    int pluginCrashesSeen = 0; // hosted plugins that crashed (engine/PluginHost.h) and were already announced
    void showHelp();

    // transport
    void playStop();     // Space: play, or stop and go back to where playback started
    void playPause();    // Ctrl+Space: pause / resume where it is
    void stopAndRewind (bool toStart = false); // toStart: the very start, ignoring "Remember seek time"
    void showStopMenu(); // right-click on Stop: "remember seek time", panic // the Stop button
    double playStartTicks = 0.0;

    // key map and actions
    bool runAction (const std::string& id);
    keys::Context focusContext() const;
    void loadKeyMap();
    keys::KeyMap keyMap;
    bool typingPiano = false; // off by default so single-letter editor tools work (the manual does not state a default)
    void setTypingPiano (bool on);
    juce::String shortcutText (const char* actionId) const; // "\tF5" for menus ("" when unbound)

    // panels
    void togglePanel (Panel panel);
    void fitChannelRack (bool width); // the Channel Rack pop-up sizes itself to its rows (and steps)
    int fittedChannelCount = -1;
    void showPanel (Panel panel);
    void refreshPanelButtons();
    void saveLayoutAs();
    juce::PopupMenu layoutsMenu();

    // patterns
    void selectPattern (int index);
    void nextEmptyPattern();
    void insertPatternHere();
    void splitPatternByChannelNow();
    void clonePatternNow();
    void movePatternNow (int delta);

    // workflow P2 windows and menus
    void showQuickAdd();
    void addPanelMenuItems (Panel panel, juce::PopupMenu& menu); // a window's own items in its title-bar menu
    void showHistory();
    void addRecordOptions (juce::PopupMenu& menu); // count-in, wait, overdub/replace, quantise, song clip
    void addRecordQuantizeItems (juce::PopupMenu& menu); // FL's Quantize items: start, end, duration, step sequencer
    void updateRecordQuantise(); // record quantise follows the global grid
    bool recordQuantiseOn = true;
    void showRenamePattern (int index);

    // user settings (UI options)
    juce::File uiSettingsFile() const;
    void loadUiSettings();
    void saveUiSettings();
    bool persistent = true;
    StartupOptions startup; // splash screen and start-up sound (read by Main.cpp before this window exists)

    // menu bar
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int, int) override {}
    void rememberRecent (const juce::File& file);
    juce::PopupMenu makePatternMenu();
    // Tools › Last tweaked / Next to last tweaked (REA round 143): the parameter menu of a recently moved control
    juce::PopupMenu makeLastTweakedMenu (int which);
    // File › Import › MIDI file... (REA round 143)
    void importMidiFile();
    void importMidiFile (const juce::File& file, const MidiFileIo::ImportOptions& options);
    static float& lastTweakedClipboard();
    void setTweakedValue (const AutomationTarget& target, float value);
    juce::MenuBarComponent menuBar { this };
    juce::RecentlyOpenedFilesList recent;
    // The main toolbar is two rows high, laid out the way FL Studio arranges its panels (docs/WORKFLOW_FL.md,
    // section 5.1): the menu with the hint bar under it, transport (with the time display and the song
    // position slider), master volume and pitch, recording options, global snap and pattern selector, window
    // and file shortcuts, then CPU / memory / polyphony and the output monitor on the right. Panels drop
    // out by priority when the window is narrow; the hint bar takes the room that is left.
    // The user can hide and reorder the groups, pick the icon size and the UI scale (right-click on the
    // toolbar, or Options > Toolbar; core/ToolbarLayout.h). Row height follows the icon size.
    toolbarlayout::Layout toolbarLayout = toolbarlayout::Layout::makeDefault();
    int rowHeight = toolbarlayout::rowHeight (toolbarlayout::IconSize::medium);
    int toolbarHeight = 2 * rowHeight + 8;
    void setToolbarLayout (const toolbarlayout::Layout& layout, bool save = true);
    juce::PopupMenu makeToolbarMenu();
    void showToolbarMenu();
    void showCustomizeToolbar();
    // The toolbar's edit mode (drag panels to move, add or remove them; toolbar::EditOverlay)
    void setToolbarEditing (bool editing);
    void undoRedo (bool redo); // with "Undone: ..." and the undo level in the hint bar
    std::unique_ptr<toolbar::EditOverlay> toolbarEditor;
    std::unique_ptr<juce::Component> setupOverlay; // the first-start / Options > Setup wizard, over everything
    toolbar::EditOverlay::Bounds panelBounds {}; // each shown toolbar panel (for the edit mode)
    juce::Rectangle<int> fixedToolbarArea;       // the menu and the hint bar
    void newProject (bool askAlways = false); // File › New: the New project window unless turned off
    void startNewProject (Song song, const juce::File& file);
    void saveAsTemplate();
    void chooseFileToOpen();
    void chooseFileToOpenNow();
    void save (bool askForFile);
    void exportAudio (int format = -1); // −1: the last export's format(s); 0 WAV, 1 MP3, 2 OGG, 3 FLAC alone
    void exportMidiFile();
    void switchSmartDisableForAll();
    void refreshToolbar();
    void updateTitle();
    bool shouldReclaimKeyboardFocus(); // the main window is active but nothing in it has the keyboard focus
    void autosave();
    // project safety (REA round 118, FL's backup folder / save new version / project data files interpreted)
    void saveNewVersion();
    void revertToLastBackup();
    void exportPackage();
    void collectSamples();
    void keepOverwrittenCopy (const juce::File& file); // the previous file of a save, kept in the backup folder
    void rotateBackups (const juce::String& stem);
    // unused recordings (REA round 134, FL's "Delete unused recorded files" / "Delete unused files and save project")
    void offerToDeleteUnusedRecordings (bool keepsCurrentState, std::function<void()> then);
    void deleteUnusedRecordingsAndSave();
    void saveToNewProjectFolder();                    // FL's File › "Save to new project folder..."
    juce::File recordingFolderFor (const juce::File& projectFile) const;
    std::vector<std::string> savedSamples;            // samples of the project as it is on disk (when last unmodified)
    bool askUnusedRecordings = true;                  // Settings › General › Backup
    void startSession();                              // crash detection: a marker that a clean exit removes
    void checkPreviousSession (juce::Time sessionStart);
    std::unique_ptr<juce::InterProcessLock> sessionLock;
    juce::File sessionMarker;
    int autosaveTicks = 0;
    std::shared_ptr<std::atomic<bool>> autosaveRunning = std::make_shared<std::atomic<bool>> (false); // the backup file is being written
    int autosaveMinutes = 2, autosaveKeep = 10; // Settings › General › Backup
    bool lastModified = false;
    void openPianoRoll();
    void toggleMode();
    void refreshModeButton();
    void showPatternMenu();

    AudioEngine& engine;

    IconButton newButton { "new", Icon::newFile }, openButton { "open", Icon::open }, saveButton { "save", Icon::save },
        exportButton { "export", Icon::exportAudio }, undoButton { "undo", Icon::undo }, redoButton { "redo", Icon::redo };
    IconButton playButton { "play", Icon::play };
    IconButton stopButton { "stop", Icon::stop };
    IconButton recordButton { "record", Icon::record };
    IconButton recordMenuButton { "recordMenu", Icon::mic };
    IconButton patternModeButton { "patternMode", Icon::pattern }, songModeButton { "songMode", Icon::song };
    IconButton metronomeButton { "metronome", Icon::metronome };
    IconButton typingPianoButton { "typingPiano", Icon::keyboard };
    IconButton countInButton { "countIn", Icon::countIn };
    IconButton waitInputButton { "waitInput", Icon::waitInput };
    void refreshRecordOptionButtons();
    void showRecordMenu();
    // right-click on the metronome: on/off, its volume, count-in (FL keeps the metronome's options there)
    void showMetronomeMenu();
    // the metronome volume knob in a callout under the metronome button
    void showMetronomeVolume();
    // View > Audio editor: the selected channel's sample when it is a Sampler with one, else an empty editor
    void openAudioEditorForSelection();
    void toggleRecording();       // the record button: asks what to record first (unless told not to)
    void armSelectedInsertForAudio(); // "Audio, into the Playlist": arms the selected insert (asks for its input)
    void setControlHints(); // the hint bar text of every toolbar control (markup, see ui/HintBar.h)
    juce::String keyMarkup (const char* actionId) const; // "[Ctrl][S]" for the action's first key ("" when unbound)

    // tempo: a number dragged by its digits (whole BPM or decimals), with tap tempo in its menu
    class TempoDisplay final : public juce::Component,
                               public ui::HintSource
    {
    public:
        explicit TempoDisplay (AudioEngine& e) : engine (e) {}
        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
        ui::Hint hintAt (juce::Point<int> position) override;
        void showMenu();
        void showTap();

    private:
        bool isOnDecimals (int x) const;
        void setBpm (double bpm);
        ui::ValueSpec spec() const;
        AudioEngine& engine;
        double startBpm = 0.0;
        bool dragOnDecimals = false, dragging = false;
        int decimalX = 0; // x of the decimal point, from the last paint
    };
    TempoDisplay tempoDisplay { engine };

    juce::ComboBox patternBox;
    IconButton newPatternButton { "newPattern", Icon::plus };
    IconButton patternMenuButton { "patternMenu", Icon::more };
    juce::Slider swingSlider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox }; // in the Channel Rack's title bar
    // The Channel Rack's title tools (FL keeps them in the rack's title bar, REA round 104): the channel filter
    // group box ("All"), the global swing knob and the graph editor switch.
    struct FilterBox final : public juce::Component
    {
        juce::String text;
        bool hovered = false;
        std::function<void()> onClick;
        void paint (juce::Graphics& g) override;
        void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    };
    // The Channel Rack's pattern selector (round 162, issue #9; FL's pattern selector): "<" previous, the current
    // pattern's name (click: the list of patterns, wheel: step), ">" next.
    struct PatternSelector final : public juce::Component
    {
        juce::String text;
        juce::Colour colour;
        int hoveredPart = -1; // 0 = previous, 1 = name, 2 = next
        std::function<void (int delta)> onStep;
        std::function<void()> onMenu;
        static constexpr int arrowWidth = 16;
        int partAt (int x) const { return x < arrowWidth ? 0 : x >= getWidth() - arrowWidth ? 2 : 1; }
        void paint (juce::Graphics& g) override;
        void mouseMove (const juce::MouseEvent& e) override { setHovered (partAt (e.x)); }
        void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;
        void setHovered (int part) { if (part != hoveredPart) { hoveredPart = part; repaint(); } }
    };
    PatternSelector rackPatternSelector;
    void showRackPatternList();
    void refreshRackPatternSelector();
    struct RackTitleTools final : public juce::Component
    {
        std::function<void()> layout;
        void resized() override { if (layout) layout(); }
    };
    RackTitleTools rackTitleTools;
    FilterBox rackFilterBox;
    IconButton rackGraphButton { "rackGraph", Icon::graph };
    IconButton rackPianoRollButton { "rackPianoRoll", Icon::pianoRoll };
    static constexpr int rackPatternSelectorWidth = 132;
    static constexpr int rackTitleToolsWidth = rackPatternSelectorWidth + 6 + 96 + 4 + 26 + 4 + 24 + 4 + 24;
    // one icon per workspace panel (F5..F9): click toggles, right-click shows it maximized
    std::array<IconButton, workspace::panelCount> panelButtons { IconButton { "playlistPanel", Icon::playlist },
                                                                 IconButton { "rackPanel", Icon::channelRack },
                                                                 IconButton { "pianoRollPanel", Icon::pianoRoll },
                                                                 IconButton { "mixerPanel", Icon::mixer },
                                                                 IconButton { "browserPanel", Icon::browser } };
    IconButton pluginPickerButton { "pluginPicker", Icon::pluginPicker };
    GlobalGridBox gridBox; // global grid of the editors (see ui/EditorTools.h)
    std::vector<int> dividers; // x positions of the toolbar panel separators
    juce::Rectangle<int> snapLabelArea, patternLabelArea, titleArea, transportArea;
    std::unique_ptr<ui::QuickTipWindow> quickTips; // the tool names under the mouse (ui/HintBar.h)
    void setQuickTips();
    void showLanguageChoice (const juce::String& id);

    // the time display with the song position slider under it, master volume and pitch, meters
    toolbar::TimeDisplay timeDisplay { engine };
    toolbar::SongPositionBar songPosition { engine };
    toolbar::MasterKnob masterVolume { engine, toolbar::MasterKnob::Kind::volume };
    toolbar::MasterKnob masterPitch { engine, toolbar::MasterKnob::Kind::pitch };
    toolbar::PerformanceMeter perfMeter;
    uitrace::Monitor uiTraceMonitor; // LYNX_TRACE_UI: message-thread stalls to ui.log (round 151)
    toolbar::MidiActivityLight midiLight;
    juce::StringArray midiLightInputs; // the open MIDI inputs (refreshed once a second)
    int midiInputTicks = 0;
    bool realtimeHintChecked = false; // Linux: the real-time priority hint was considered (round 161)
    toolbar::OutputMonitor output { engine };
    int meterTicks = 0;
    juce::String driverTipType; // the device type the ASIO suggestion was worked out for

    // hint bar: what the control under the mouse does, its live value, and markers for
    // "can be automated" and "has a right-click menu"
    ui::HintBar hintBar;
    bool hintsFrozen = false; // --snapshot: keep the hint that was set
    juce::String idleHint() const;

    ChannelRackComponent rack;
    juce::Viewport rackViewport;
    PianoRollComponent pianoRoll;
    PlaylistComponent playlist;
    PatternPicker picker { engine, playlist };
    PlaylistArea playlistArea { picker, playlist };
    MixerComponent mixer;
    BrowserComponent browser;
    Workspace workspace; // after the panels' contents: it is destroyed first

    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File currentFile;
    juce::File importedFlpFile; // the .flp the current project was imported from (until another project is opened)
    juce::String importedSuffix; // " (imported from FL Studio)" in the title until saved as .lynx
    juce::String projectName; // suggested name for export (also from an imported .flp)
    HeldNotes heldKeys; // notes the typing piano is playing (tag: the key code)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace lynx
