#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/HeldNotes.h"

#include "core/RackOps.h"
#include "engine/AudioEngine.h"
#include "ui/ValueControl.h"

namespace lynx
{
// The FL instrument engines a native project can add (core/NativeEngines.h, round 142, issue #5) as items of `menu`:
// a new one starts from the plugin's Default preset of the user's FL installation; without it the item is disabled
// and says "(requires FL Studio)". `add` receives the new channel (mixerTrack left to it).
void addFlEngineItems (juce::PopupMenu& menu, std::function<void (ChannelDef)> add);
// The engine's menu name ("(beta)", "(requires FL Studio)")
juce::String flEngineName (const natives::InstrumentEngine& e);

// FL-style Channel Rack: one row per channel with mute, pan, volume, mixer insert, name, selector
// and the step grid of the current pattern.
//  - steps: left click turns on (drag paints), right click erases
//  - name (channel button): click opens/closes the channel's editor (plugin window or settings),
//    closing the previously opened one unless Alt is held (or "keep editors open" is on);
//    Ctrl+click auditions; double click opens the Piano Roll; right click opens the channel menu;
//    dragging it up/down reorders the selected channels
//  - selector: click selects one row, Ctrl+click or right click toggles, Shift+click selects a range,
//    double click selects all; left-drag selects a range, right-drag adds the rows it crosses.
//    The selection receives MIDI input and is what the keys act on
//  - knobs, route box: the shared control contract (ui/ValueControl.h), with a value read-out while
//    dragging; double-click on the route box opens the Mixer at that insert
//  - everything under the mouse is highlighted and described in the hint bar
//  - dropping an audio file onto a row turns the channel into a sampler
class ChannelRackComponent final : public juce::Component,
                                   public juce::FileDragAndDropTarget,
                                   public juce::DragAndDropTarget,
                                   public ui::HintSource,
                                   private juce::ChangeListener,
                                   private juce::Timer
{
public:
    explicit ChannelRackComponent (AudioEngine& engine);
    ~ChannelRackComponent() override;
    void visibilityChanged() override;

    std::function<void()> onOpenPianoRoll;
    std::function<void (int)> onOpenMixerInsert; // double-click on a route box: show the Mixer at that insert

    // Settings > "Keep channel editors open": opening an editor never closes the previous one.
    static bool keepEditorsOpen;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    ui::Hint hintAt (juce::Point<int> position) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // files dragged from the LYNX browser
    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDropped (const SourceDetails& details) override;

    // Keyboard actions (the key map's rack.* actions)
    void selectRelative (int delta, bool extend);
    void selectAll();
    bool moveSelected (int delta);
    void cloneSelected();
    void deleteSelected();
    void sendSelectedToInserts();
    void toggleMute (int channel);
    void solo (int channel);
    void setSwingMixOfSelected (int channel); // "Swing mix..." (type a value for the selected channels)

    // Workflow P2: step graph lanes, fills, rotation and organisation
    void toggleGraphLanes();
    // Piano-roll view: every row shows its notes as a miniature (a click opens them in the Piano Roll) instead of
    // the step buttons; FL's "step sequencer / piano roll overview" switch in the rack's title bar.
    void setPianoRollView (bool on);
    bool isPianoRollView() const noexcept { return pianoRollView; }
    bool areGraphLanesShown() const noexcept { return graphLanes; }
    void setGraphLevelForSnapshot (int level) { graphLevel = level; graphLanes = true; updateSize(); repaint(); }
    void rotateSelected (int steps);
    void fillSelected (int every);
    void showEuclideanFill (int channel);
    std::unique_ptr<juce::Component> makeEuclideanPanel (int channel); // the window's content (also for --snapshot)
    void sortChannels (ChannelSort by);
    void selectUnused();

    // Adds a channel at the end (routed to a free insert), selects it and, for plugins, opens its window.
    int addChannel (ChannelDef channel, bool openEditor);

    // Loads `file` into the selected sampler channel (Browser: Enter, Shift+Up/Down). Returns false when
    // the selected channel cannot take a sample.
    bool loadSampleIntoSelected (const juce::File& file);

    // Opens or closes the channel's editor (plugin window or settings window; built-in drums and audio
    // clips have none). `keepOthers`: don't close the previously opened editor.
    void toggleEditor (int channel, bool keepOthers);

    void updateSize();

    // Width that shows the controls and at most `maxSteps` steps (the panel scrolls for longer patterns).
    int widthForSteps (int maxSteps) const { return gridLeft() + juce::jmin (numSteps(), maxSteps) * stepWidth + 8; }
    int naturalWidth() const { return gridLeft() + numSteps() * stepWidth + 8; } // without the ghost steps

    // The menu of instruments and plugins to add as a new channel (also the toolbar's plugin picker).
    void showAddChannelMenu (int insertAt = -1);
    // The same items into `menu` (also the main window's Add menu, REA round 139).
    void addInstrumentItems (juce::PopupMenu& menu, int insertAt);

    // Channel filter groups (FL's "All" dropdown in the rack's title, REA round 104): -1 shows every channel,
    // otherwise only the channels of that group (Song::channelGroups). New channels join the group shown.
    int getFilterGroup() const noexcept { return filterGroup; }
    void setFilterGroup (int group);
    juce::String filterName() const;
    void showFilterMenu (juce::Component* target);
    void groupSelected (); // "Group selected..." (Alt+G): the selected channels go into a new, named group
    void moveSelectedToGroup (int group); // -1 = no group
    bool isShown (int row) const;          // not hidden by the filter
    std::function<void()> onFilterChanged; // the title shows the filter's name and the graph editor's state

    static constexpr int rowHeight = 30;
    static constexpr int muteWidth = 26;
    static constexpr int knobWidth = 28;
    static constexpr int routeWidth = 30;
    static constexpr int nameWidth = 150;
    static constexpr int selectWidth = 12;
    static constexpr int stepWidth = 22;

private:
    enum class Area { none, mute, pan, volume, route, name, select, grid, addRow, laneLevels, lane };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    Area areaAt (int x, int y, int& row) const;
    void moved() override;
    int stickyX = 0; // how far the viewport scrolled the rack sideways (the controls column stays in view)
    int numSteps() const;   // the pattern's steps
    int shownSteps() const; // plus FL's ghost steps: the free width after the pattern's end (issue #11)
    int ghostSteps = 0;
    void parentSizeChanged() override { updateSize(); }
    int nameLeft() const noexcept { return muteWidth + 2 * knobWidth + routeWidth + 2; }
    int selectLeft() const noexcept { return nameLeft() + nameWidth + 4; }
    int gridLeft() const noexcept { return selectLeft() + selectWidth + 8; }
    juce::Rectangle<int> rowBounds (int row) const;
    int rowY (int row) const;       // top of a row (rows below the graph lane shift down)
    int filterGroup = -1;
    static inline std::vector<Note> stepClipboard; // the channel menu's Cut / Copy / Paste (a channel's notes)
    int visibleIndex (int row) const; // the row's place among the shown rows
    int shownCount() const;
    int rowAtY (int y) const;       // row under y (the lane counts as its channel's row)
    int graphRow() const;           // the row the lane belongs to (the primary selection), -1 when hidden
    juce::Rectangle<int> laneBounds() const;
    juce::Rectangle<int> levelButtonBounds (int level) const;
    void paintLane (juce::Graphics& g);
    void paintNotePreview (juce::Graphics& g, int row, const Pattern& pattern, std::uint32_t channelColour);
    void setLaneValue (int step, float value);
    void applyLane (const juce::MouseEvent& e);
    float laneValueAt (int y) const;
    bool graphLanes = false;
    bool pianoRollView = false;
    int graphLevel = 0; // notetools::Level
    int laneDownStep = -1;
    float laneDownValue = 0.0f;
    static constexpr int laneHeight = 72;
    juce::Rectangle<int> stepBounds (int row, int step) const;
    bool cellHasNote (int channel, int step) const;
    void applyPaint (const juce::MouseEvent& e);
    void showChannelMenu (int channel);
    void chooseSample (int channel); // channel < 0 = new channel
    void assignSample (int channel, const juce::File& file);
    void rename (int channel);
    void openPluginWindow (int channel);
    void sliceChannel (int channel);
    bool isEditorOpen (int channel);
    void closeEditor (int channel);

    // selection
    void selectRows (std::vector<int> rows, int primary);
    void clickSelector (int row, const juce::ModifierKeys& mods);
    int selectDownRow = -1;          // a drag over the selector column: where it started
    bool selectDragAdds = false;     // right-drag: add the crossed rows to the selection
    std::vector<int> selectDragBase; // the selection before a right-drag

    // channel button: a click opens the editor on release; dragging reorders the selection
    int nameDownRow = -1;
    bool nameDragged = false, nameDownAlt = false;
    float nameDragY = 0.0f;

    // hover feedback
    void setHover (Area area, int row, int step);
    juce::Rectangle<int> controlBounds (Area area, int row) const; // knob / route box under the mouse (value tip)
    void showTip (Area area, int row, bool transient);
    Area hoverArea = Area::none;
    int hoverRow = -1, hoverStep = -1;

    // the control contract for the row's knobs and route box
    ui::ValueSpec specFor (Area area, int row) const;
    double valueOf (Area area, int row) const;
    void setValue (Area area, int row, double value);

    AudioEngine& engine;
    int playheadStep = -1;

    enum class PaintMode { none, add, remove } paintMode = PaintMode::none;
    int lastPaintedRow = -1, lastPaintedStep = -1;

    Area knobDrag = Area::none;
    int knobRow = -1;
    ui::ValueDrag drag;
    HeldNotes previews;        // Ctrl+click audition and step-click previews: they stop on mouse up
    int openEditorRow = -1;    // the editor opened last by a click on a channel button
    int selectionAnchor = 0;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRackComponent)
};

// The Channel Rack panel's viewport: a sample, SoundFont or generator dropped on the free space under the channels
// becomes a new channel too, as anywhere in FL's Channel Rack (issue #11); the drop goes to the rack.
class ChannelRackViewport final : public juce::Viewport,
                                  public juce::FileDragAndDropTarget,
                                  public juce::DragAndDropTarget
{
public:
    explicit ChannelRackViewport (ChannelRackComponent& r) : rack (r) {}

    bool isInterestedInFileDrag (const juce::StringArray& files) override { return rack.isInterestedInFileDrag (files); }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        const auto p = rack.getLocalPoint (this, juce::Point<int> (x, y));
        rack.filesDropped (files, p.x, p.y);
    }
    bool isInterestedInDragSource (const SourceDetails& details) override { return rack.isInterestedInDragSource (details); }
    void itemDropped (const SourceDetails& details) override
    {
        auto moved = details;
        moved.localPosition = rack.getLocalPoint (this, details.localPosition);
        rack.itemDropped (moved);
    }

private:
    ChannelRackComponent& rack;
};
} // namespace lynx
