#pragma once

// Shared pieces of the Piano Roll and Playlist editors: the edit tools, the global grid (with the
// per-editor "Follow global" choice and the default note length) and the Note inspector.

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/EditorGestures.h"
#include "core/ToolbarLayout.h"
#include "engine/AudioEngine.h"
#include "ui/Icons.h"
#include "ui/ValueControl.h"

namespace lynx
{
// --- shared title bar ---------------------------------------------------------------------
// The Playlist and Piano Roll carry their toolbar in their window's title bar, as FL Studio's editors do
// (docs/WORKFLOW_FL.md, "Layout"): the workspace frame lays the editor over its title row and keeps
// `left` px for its menu button and title and `right` px for its window buttons; the editor lays out
// its toolbar between them.
class SharesTitleBar
{
public:
    virtual ~SharesTitleBar() = default;
    virtual void setTitleBarInsets (int left, int right) = 0;
};

// --- edit tools ------------------------------------------------------------------------
// Rules shared by every tool: right-click deletes, Alt bypasses the grid, Ctrl selects
// (Ctrl+drag draws a selection box). Right-press-and-hold on empty grid gives a temporary Knife.

struct EditToolInfo
{
    EditTool tool;
    Icon icon;
    const char* name;        // English, translated with tr()
    const char* description; // English, translated with tr()
    char key;                // single-key shortcut (only while the typing piano is off)
};

const std::vector<EditToolInfo>& editTools();

// The mouse cursor of each editor gesture (core/EditorGestures.h): the system arrows for moving,
// resizing and selecting, LYNX's own tool icons (outlined, so they read on any background) for
// drawing, painting, erasing, muting, slicing, zooming, listening and slipping. Built once.
juce::MouseCursor editorCursor (gesture::Cursor cursor);

// Hint bar text for what a left press would do (`playlist` words it for clips instead of notes),
// followed by the gestures that apply on top of it. The editors' HintSource uses it.
juce::String actionHint (gesture::Action action, bool playlist);
const EditToolInfo& editToolInfo (EditTool tool);

// A row of radio icon buttons, one per tool. Icon size and button width follow the toolbar icon size
// (Options > Toolbar), shared by every strip; the owner re-lays the strip out after a change.
class ToolStrip final : public juce::Component
{
public:
    explicit ToolStrip (bool includeSlip);
    static inline toolbarlayout::IconSize iconSize = toolbarlayout::IconSize::medium;

    EditTool getTool() const noexcept { return tool; }
    void setTool (EditTool newTool);
    int getIdealWidth (int height) const;

    std::function<void (EditTool)> onChange;

    void resized() override;
    // the wheel over the strip steps through the tools
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    EditTool tool = EditTool::pencil;
    std::vector<std::unique_ptr<IconButton>> buttons;
    std::vector<EditTool> tools;
};

// --- grid --------------------------------------------------------------------------------
namespace grid
{
enum class Kind
{
    none,    // free (whole ticks)
    screen,  // "Line": follows the lines the editor is drawing at its zoom (FL's "Line")
    nearest, // "Events": the nearest note / clip edge (and ghost notes); otherwise the lines
    ticks,   // a fixed fraction of a step or beat
    bar      // whole bars
};

struct Option
{
    Kind kind;
    int ticks; // for Kind::ticks
    const char* name;
};

const std::vector<Option>& options();
constexpr int noneOption = 0;
constexpr int screenOption = 1;

// Editor combo: id 1 = "Follow global", id i + 2 = options()[i]. Global combo: id i + 1.
constexpr int followId = 1;
void fillEditorBox (juce::ComboBox& box);
void fillGlobalBox (juce::ComboBox& box);

// The option an editor combo stands for (resolving "Follow global").
const Option& resolve (const juce::ComboBox& editorBox);

// What an option does, in one sentence (the hint bar shows it for the chosen option).
juce::String describe (const Option& option);
} // namespace grid

// A snap selector that explains its current choice in the hint bar: the component's hint text (ui::setHint
// or the tooltip) followed by what the chosen option does. `editor`: the box has the "Main" (follow the
// toolbar) entry. The wheel steps through the options.
class SnapBox : public juce::ComboBox,
                public ui::HintSource
{
public:
    explicit SnapBox (bool editor);
    ui::Hint hintAt (juce::Point<int>) override;

private:
    bool editor;
};

// State the editors share with the main window's toolbar. Listeners hear about every change.
class EditorGlobals final : public juce::ChangeBroadcaster
{
public:
    static EditorGlobals& get();

    int getGridOption() const noexcept { return gridOption; }
    void setGridOption (int option);
    void toggleGrid(); // Backspace: between the last grid and "No snap"

    // Length of new notes in ticks; 0 = the length of the last note clicked or resized.
    std::int64_t getDefaultNoteLength() const noexcept { return defaultNoteLength; }
    void setDefaultNoteLength (std::int64_t ticks);

    // While the typing piano owns the letter keys, the editors' single-key shortcuts (tools, channel
    // switching) are off. The main window's typing-piano toggle (P1 #14) sets this.
    bool typingPiano = true;

    // "Remember seek time" (the Stop button's right-click menu, as in FL's toolbar): Stop goes back to the
    // position last clicked on the Playlist ruler (`seekMark`, in ticks; -1 = none) instead of the start.
    bool rememberSeek = false;
    std::int64_t seekMark = -1;

    // "Follow" (FL's auto scrolling, round 162 / issue #9): the Playlist (Song mode) and the Piano Roll page along
    // with the playhead while playing; off, they stay where the user scrolled. Saved with the UI options.
    bool isFollowing (bool pianoRoll) const noexcept { return pianoRoll ? followPianoRoll : followPlaylist; }
    void setFollowing (bool pianoRoll, bool on, bool save = true); // save: call onOptionsChanged
    // Called after an option the main window saves changed here (it writes ui-options.txt).
    std::function<void()> onOptionsChanged;

private:
    EditorGlobals() = default;
    int gridOption = grid::screenOption, lastGridOption = grid::screenOption;
    std::int64_t defaultNoteLength = 0;
    bool followPlaylist = true, followPianoRoll = true;
};

// The global grid selector of the main toolbar: left-click picks the grid, right-click sets the
// default note length.
class GlobalGridBox final : public SnapBox,
                            private juce::ChangeListener
{
public:
    GlobalGridBox();
    ~GlobalGridBox() override;

    void mouseDown (const juce::MouseEvent& e) override;
    static void showDefaultLengthMenu (juce::Component* target);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};

// --- overview strip ------------------------------------------------------------------------
// The strip above the Piano Roll's and the Playlist's ruler: the whole content in miniature (drawn by
// `paintContent`) with the visible window outlined. Drag the window to scroll, drag one of its edges to
// zoom, click elsewhere to jump there (and keep dragging), double-click to show everything.
// Units are the editor's (ticks); the editor calls setRange whenever its view changes.
class OverviewBar final : public juce::Component
{
public:
    OverviewBar();

    void setRange (double total, double start, double length);
    double getTotal() const noexcept { return total; }

    // draws the miniature into `area` (the whole strip), `total` units across
    std::function<void (juce::Graphics&, juce::Rectangle<float> area, double total)> paintContent;
    // the user moved (same length) or resized (zoom) the window
    std::function<void (double start, double length)> onChange;
    std::function<void()> onShowAll;
    double minLength = 1.0; // the shortest window the editor can zoom to

    void paint (juce::Graphics& g) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    juce::Rectangle<float> windowArea() const;
    gesture::OverviewGrab grabAt (int x) const;
    void updateCursor (gesture::OverviewGrab g);

    double total = 1.0, start = 0.0, length = 1.0;
    gesture::OverviewGrab grab = gesture::OverviewGrab::none, hoverGrab = gesture::OverviewGrab::none;
    gesture::OverviewWindow grabFrom;
    bool dragging = false;
};

// --- Note inspector ------------------------------------------------------------------------
// Edits velocity, pan, release, fine pitch, mod X/Y, slide and porta of `indices` (notes of the current
// pattern) as one undo step on Apply; Revert puts the controls back. Time and length are offered when a
// single note is inspected. `focus` is the note whose values are shown.
std::unique_ptr<juce::Component> makeNoteInspector (AudioEngine& engine, std::vector<int> indices, int focus);

// Opens the inspector in a call-out pointing at `screenArea`.
void showNoteInspector (AudioEngine& engine, std::vector<int> indices, int focus, juce::Rectangle<int> screenArea);
} // namespace lynx
