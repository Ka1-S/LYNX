#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"
#include "ui/EditorTools.h"
#include "ui/NoteToolDialogs.h"
#include "ui/ValueControl.h"
#include "core/NoteEdit.h"

namespace lynx
{
// Piano Roll for the selected channel in the current pattern.
//  - a tool strip (Pencil, Brush, Eraser, Mute, Knife, Lasso, Zoom, Listen; the wheel over it switches);
//    with the Pencil: left click on empty space creates a note (default length, or the last used one)
//    and drags it, dragging the body moves, either edge resizes (the left edge can be turned off)
//  - the cursor, the hover highlight and the hint bar show what a click would do (core/EditorGestures.h)
//  - every tool: right click deletes (dragging deletes several; press and hold on empty grid = Knife),
//    Ctrl+drag selects (Ctrl+click adds/removes), Alt ignores the grid; Shift+drag on a note drags a copy
//  - while moving: Shift pressed after the click keeps the pitch, Ctrl keeps the time
//  - double click on a note opens the Note inspector (applies to the selection)
//  - channel selector, previous/next channel keys and ghost notes of the other channels (Alt+V);
//    double right-click a ghost note to switch to its channel
//  - the bottom lane edits velocity, pan or fine pitch (combo in its corner): drag draws, right-drag a
//    line, Shift+drag one value, Alt+click resets; Alt+wheel over a note changes it (Ctrl+Alt: fine)
//  - wheel scrolls (smoothly), Shift+wheel horizontal, Ctrl+wheel zooms at the mouse, Ctrl+Alt+wheel
//    zooms vertically, Shift+Alt+wheel nudges the selection; middle-drag pans (on the keyboard: zooms)
//  - keyboard: click previews, Ctrl+click selects the key's notes; ruler: Ctrl+drag selects a time range
//  - keys (via handleKey): Delete, Backspace (global grid on/off), Ctrl+A/C/X/V/B, Ctrl+Q quantise,
//    Shift+Q quantise starts, Ctrl+U chop, Ctrl+G join, Ctrl+L legato, Shift+D shrink to grid,
//    Ctrl+Shift+Left/Right rotate, Ctrl+M mute, Ctrl+P porta, Esc, arrows (Shift = octave/bar)
class PianoRollComponent final : public juce::Component,
                                 public SharesTitleBar,
                                 public ui::HintSource,
                                 private juce::ChangeListener,
                                 private juce::Timer,
                                 private juce::ScrollBar::Listener
{
public:
    explicit PianoRollComponent (AudioEngine& engine);
    ~PianoRollComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void setTitleBarInsets (int left, int right) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void modifierKeysChanged (const juce::ModifierKeys& mods) override;
    ui::Hint hintAt (juce::Point<int> position) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Editing shortcuts; the main window forwards keys when the Piano Roll is visible.
    bool handleKey (const juce::KeyPress& key);
    // The key map's actions of this editor (its tools and single letters: "pianoRoll.tool.knife"...); false if not one.
    bool runKeyAction (const std::string& id);
    // The tool buttons' tooltips show the keys the key map gives them (`keyOf`: action id -> key text, "" = none).
    void refreshToolKeys (const std::function<juce::String (const std::string&)>& keyOf);

    // The key map's pianoRoll.* actions (tool dialogs, zoom presets). Returns false for unknown ids.
    bool runAction (const std::string& id);
    // The shortcut text of a key map action for menus ("\tCtrl+Q"; empty when unbound), set by the main window.
    static inline std::function<juce::String (const std::string& id)> shortcutText;

    // Note tool dialogs on the selection (or all of the channel's notes).
    void openNoteTool (NoteTool tool);

    // Scale snap: new and moved notes (and incoming MIDI) land on the highlighted scale.
    bool isScaleSnapOn() const noexcept { return scaleSnap; }
    void setScaleSnap (bool on);
    bool hasScale() const { return scaleIndex() > 0; } // a scale is chosen in the toolbar

    // Centers the view on the channel's notes and, with `fitPattern`, zooms to show the whole pattern
    // (called on open).
    void scrollToNotes (bool fitPattern = true);

    // FL's parameter menu "Edit events in piano roll" (REA round 146): the bottom lane shows and draws the current
    // pattern's events of `target` (the lane box lists the pattern's event lanes after the note properties).
    void showEventLane (const AutomationTarget& target);
    // The target the bottom lane edits when it shows pattern events.
    std::optional<AutomationTarget> eventLaneTarget() const;

    // For tests/snapshots: selects all of the channel's notes.
    void selectAll();

    // For snapshots: the tool in use and the Note inspector of the selection.
    void setToolForSnapshot (EditTool tool) { tools.setTool (tool); }
    std::unique_ptr<juce::Component> makeNoteInspectorForSnapshot (bool singleNote = false);
    std::vector<int> noteIndicesForSnapshot() const { return selectionOrAll(); }
    std::int64_t gridForSnapshot() const { return gridTicks(); }
    juce::PopupMenu getToolsMenuForSnapshot() { return toolsMenu(); }
    // zooms in (taller rows, wider steps) and hovers the right edge of the first note; returns its hint
    ui::Hint hoverForSnapshot();

    static constexpr int keyboardWidth = 72;
    static constexpr int toolbarHeight = 28;
    static constexpr int overviewHeight = 16; // the overview strip under the toolbar (round 90)
    static constexpr int timelineHeight = 22;
    static constexpr int rulerTop = toolbarHeight + overviewHeight;
    static constexpr int headerHeight = rulerTop + timelineHeight;
    static constexpr int velocityHeight = 72;
    int rowHeight = 14; // vertical zoom (Ctrl+Alt+wheel): 6..32 px per key
    static constexpr int scrollBarSize = 12;

private:
    enum class Drag { none, move, resize, resizeLeft, erase, keyboard, select, velocity, velocityLine, velocitySet, paint, muting, knife,
                      zoomRange, listen, pan, keyZoom, keySelect, rulerSelect, seek, drumPaint, stretch, eventDraw, eventErase };

    // Channel selector: the wheel steps through the channels with notes (Ctrl+wheel: all channels).
    class ChannelBox final : public juce::ComboBox
    {
    public:
        std::function<void (int direction, bool all)> onStep;
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (onStep && wheel.deltaY != 0.0f)
                onStep (wheel.deltaY < 0 ? 1 : -1, e.mods.isCtrlDown());
        }
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;

    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> velocityArea() const;
    double tickToX (double tick) const;
    double xToTick (int x) const;
    int keyToY (int key) const;
    int yToKey (int y) const;
    std::int64_t gridTicks() const;        // the grid step in use (resolving Screen / Nearest note)
    std::int64_t snap (double tick, bool enabled) const;
    std::int64_t viewLengthTicks() const;
    std::int64_t newNoteLength() const;
    int noteAt (int x, int y, bool& onRightEdge) const;
    int noteAt (int x, int y, bool& onRightEdge, bool& onLeftEdge) const;
    int ghostAt (int x, int y) const;      // index of a ghost note (another channel) under the point
    void refreshChannelBox();
    void stepChannel (int direction, bool withNotesOnly);
    void zoomToTicks (double from, double to);
    void zoomIn (int anchorX);
    void zoomOut (int anchorX);
    void listenAt (int x);
    void seekAt (int x, bool snapOn); // the ruler: moves the position (Alt: off the grid)
    void stopListening();
    void cutWithKnife (juce::Point<int> from, juce::Point<int> to, bool snapOn);
    void paintNoteAt (std::int64_t start, int key, bool monophonic);
    void runOperation (const std::function<std::vector<int> (Pattern&, const std::vector<int>&)>& op);
    void showMoreMenu();
    void updateScrollBars();
    void previewOn (int key, float velocity);
    float keyVelocityAt (int x, int key) const; // velocity of a click on the keyboard at x
    void previewOff();
    void visibilityChanged() override;

    bool isSelected (int index) const;
    std::vector<int> channelNotes() const;           // indices of the channel's notes
    std::vector<int> selectionOrAll() const;         // selection, or all of the channel's notes if empty
    void setSelection (std::vector<int> indices);
    void sortNotes();                                // sorts while keeping the selection (no undo step)
    void editNotes (const std::function<void (Pattern&)>& edit, bool undoable = true);
    void applyVelocity (int fromX, int toX, int y);
    float laneValue (const Note& n) const;          // 0..1 of the chosen property
    void setLaneValue (Note& n, float value) const; // inverse
    bool laneIsBipolar() const;
    void toggleMute();
    void togglePorta(); // porta notes glide from the previous note (toggles the channel's porta)

    void deleteSelection();
    void copySelection();
    void paste();
    void duplicateSelection();
    void nudge (std::int64_t ticks, int keys);

    AudioEngine& engine;
    juce::ScrollBar hBar { false }, vBar { true }; // hBar is hidden: the overview strip scrolls sideways
    OverviewBar overview;
    void paintOverview (juce::Graphics& g, juce::Rectangle<float> area, double total);
    std::size_t overviewSignature = 0; // the notes drawn in the overview (repaints it when they change)
    ToolStrip tools { false };
    int titleInsetLeft = 0, titleInsetRight = 0; // the window's title and buttons share the toolbar row
    ChannelBox channelBox;
    SnapBox snapBox { true };         // "Main" (the toolbar snap) or this editor's own grid
    IconButton ghostButton { "ghosts", Icon::ghost };
    IconButton scaleSnapButton { "scaleSnap", Icon::magnet }; // snap to the scale (next to the scale box)
    IconButton moreButton { "more", Icon::more }; // note operations, left-edge resize, MIDI import/export
    IconButton followButton { "follow", Icon::follow }; // Follow (auto scrolling) on/off, EditorGlobals (round 162)
    void quantizeOperation (int kind); // 0 = starts, 1 = starts and lengths (quick quantize), 2 = starts 50% toward the grid
    juce::ComboBox laneBox; // what the bottom lane edits: velocity, pan or fine pitch
    juce::ComboBox chordBox; // chord created on each click (like FL's "stamp")
    juce::TextButton slideButton { "Slide" }; // new notes slide (like FL's slide tool)
    juce::ComboBox scaleBox; // scale highlighted on the keyboard rows
    // note colour groups (FL's 16 note colours; REA round 153): new notes get `noteColour`; the swatch opens a menu
    // that picks it, colours the selection and selects by colour
    class ColourSwatch final : public juce::Button
    {
    public:
        ColourSwatch() : juce::Button ("noteColour") {}
        int colourIndex = 0;
        void paintButton (juce::Graphics& g, bool over, bool down) override;
    };
    ColourSwatch colourButton;
    int noteColour = 0;
    void showColourMenu();
    void colourSelection (int colour);   // one undo step
    void selectByColour (int colour);    // the channel's notes of that colour
    juce::PopupMenu selectMenu();        // FL's Select submenu (invert, odd, random, muted, overlapping, stacked, colour)
    // chord readout (FL 2026's chord detection): the chord of the held MIDI / typing-piano keys, else of the
    // selected notes
    juce::Label chordLabel;
    void updateChordReadout();
    void showKeyMenu (int key); // right-click on the keyboard: key labels of the channel
    std::vector<int> chordIntervals() const;
    bool chordTopDown = false; // the chord stamp puts the clicked key on top (FL 2026's top-down mode)
    bool inScale (int key) const;
    int scaleRoot() const;  // 0..11
    int scaleIndex() const; // index into notetools::scales(), 0 = none
    int snapKey (int key) const; // the key itself, or the nearest in-scale key while scale snap is on
    void publishScale();
    bool scaleSnap = false;
    double panStartTicks = 0.0;
    int panStartY = 0;
    void zoomAroundCentre (double factor);
    void zoomToBars (int bars);
    void zoomToSelection();
    void zoomAll();
    void centreOnPlayhead();
    juce::PopupMenu toolsMenu();
    std::unique_ptr<juce::FileChooser> chooser;
    juce::PopupMenu midiMenu();

    // hover: what a click would hit (drives the cursor, the highlight and the hint)
    struct Hover
    {
        int note = -1, ghost = -1, key = -1;
        gesture::Zone zone = gesture::Zone::outside;
        bool stretch = false; // the selection's stretch handle
        bool operator== (const Hover& o) const
        {
            return note == o.note && ghost == o.ghost && key == o.key && zone == o.zone && stretch == o.stretch;
        }
    };
    Hover hover;
    juce::Point<int> lastMouse { -1, -1 };
    Hover hoverAt (juce::Point<int> p) const;
    gesture::Target targetAt (const Hover& h) const;
    void updateHover (juce::Point<int> p, juce::ModifierKeys mods);
    void updateCursor (juce::ModifierKeys mods);
    void applyLaneValue (int x, float value, int fromX); // sets the lane value of the notes between fromX and x
    // Alt+wheel (FL 2026, round 166e): `note` < 0 = the selection, else that note alone; the note is heard (with the
    // selection: the selected note nearest to `mouse`), and a single note's properties become the new-note template
    void wheelNoteProperty (int note, float deltaY, bool fine, juce::Point<int> mouse);
    noteedit::NoteTemplate noteTemplate; // FL: the properties new notes get from the last clicked note (round 166e)
    juce::uint32 wheelPreviewSerial = 0;
    void postDragHint (const juce::MouseEvent& e); // what the gesture in progress does (hint bar)
    juce::uint32 lastWheelEdit = 0;    // wheel edits within a burst are one undo step
    std::vector<Note> laneOriginals;   // right-drag line / Shift+drag: the pattern's notes when it started
    float laneSetValue = 0.0f;
    // pattern event lanes in the lane box (ids eventLaneIdBase + k; REA round 146)
    static constexpr int eventLaneIdBase = 100;
    std::vector<AutomationTarget> eventTargets;
    void rebuildLaneBox (std::optional<AutomationTarget> shown = std::nullopt, bool force = false);
    void drawEvents (int fromX, int toX, int y, bool erase);
    int keyZoomKey = 60, keyZoomStartRow = 14;

    // smooth wheel scrolling
    gesture::Glide glideX, glideY;
    bool gliding = false;
    void glideBy (double ticks, double pixels);
    void glideFrame();
    juce::VBlankAttachment vblank { this, [this] { glideFrame(); } };

    double pixelsPerTick = 24.0 / ticksPerStep; // 24 px per step
    double scrollTicks = 0.0;
    int scrollY = (127 - 84) * rowHeight;       // starts showing roughly C6..C3

    Drag drag = Drag::none;
    int dragNote = -1;              // grabbed note (reference for snap and preview)
    std::vector<int> dragIndices;   // notes that follow the drag
    std::vector<Note> dragOriginals;
    std::int64_t grabOffset = 0;    // note tick - mouse tick at drag start
    std::int64_t lastLength = ticksPerStep * 2;
    int previewKey = -1;
    int lastScaleDragKeys = 0; // (a snap-to-scale group drag: the mouse's semitones last applied)
    NoteMask litKeys {}; // keys sounding on the channel(s) shown: lit on the keyboard (issue #14, FL)
    NoteMask soundingNow() const;
    bool isLit (int key) const noexcept { return key >= 0 && key < 128 && ((litKeys[(std::size_t) (key >> 6)] >> (key & 63)) & 1u) != 0; }
    double playheadTick = -1.0;

    std::vector<int> selection;     // indices into Pattern::notes, sorted
    std::vector<int> selectionBefore; // selection at the start of a Shift+rectangle
    juce::ModifierKeys downMods;      // modifiers at the click (locks start when Shift/Ctrl are pressed later)
    // the channel box's bold "All" (issue #12): every channel's notes in the pattern are editable at once (new notes
    // still go to the selected channel); off again when another channel is picked
    bool allChannels = false;
    static constexpr int allChannelsId = 100000; // its id in channelBox
    bool edits (const Note& n) const noexcept { return allChannels || n.channel == engine.getSelectedChannel(); }
    void setAllChannels (bool on);
    bool showGhosts = true;
    bool editGhosts = false;          // Ctrl+Alt+V: a press on a ghost note edits it (switching to its channel)
    // the selection's stretch handle (R5): drag it to scale the selected notes in time
    juce::Rectangle<float> stretchHandle() const;
    std::int64_t stretchAnchor = 0, stretchEnd = 0;
    // drum brush: lays one note per grid cell and key
    void drumPaintAt (juce::Point<int> p, bool snapOn);
    std::int64_t lastDrumCell = -1;
    int lastDrumKey = -1;
    bool resizeFromLeft = true;       // the left edge of a note resizes it (Ctrl+Alt+Home turns it off)
    bool startedOnEmpty = false, erasedSomething = false, holdExpired = false;
    juce::uint32 createdAt = 0;       // when the last click created a note (a double click there is not an edit)
    std::int64_t nextPaintTick = 0, paintLength = 0;
    bool muteTarget = false;
    std::vector<int> touchedNotes;    // notes already muted/unmuted during this drag
    juce::Point<int> knifeFrom, knifeTo;
    std::vector<int> listenKeys;
    int listenChannel = -1;
    HeldNotes previewNotes;           // the note a click/drag previews (keyboard, notes); stops on release
    HeldNotes listenNotes;            // the Listen tool's notes under the mouse
    std::vector<std::int64_t> snapCandidates() const;
    juce::Rectangle<int> rubberBand;
    int lastVelocityX = 0;
    int knownPattern = -1, knownChannel = -1;
    std::size_t knownNoteCount = 0;

    static inline std::vector<Note> clipboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
} // namespace lynx
