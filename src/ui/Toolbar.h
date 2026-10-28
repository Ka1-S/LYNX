#pragma once

#include <array>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/ToolbarLayout.h"
#include "engine/AudioEngine.h"
#include "ui/Icons.h"
#include "ui/ValueControl.h"

// Widgets of the main toolbar (docs/WORKFLOW_FL.md section 5.1), each with its own hint: the song position
// slider, the time display, master volume and pitch, the CPU / memory / polyphony meter and the output
// monitor. Laid out by MainComponent::resized().
namespace lynx::toolbar
{
// A thin slider under the time display: where the playhead is in the song (Song mode) or in the
// current pattern (Pattern mode). Click or drag to move the playhead; the wheel moves it by a bar.
class SongPositionBar final : public juce::Component,
                              public ui::HintSource
{
public:
    explicit SongPositionBar (AudioEngine& e);
    void update(); // from the main window's timer
    void paint (juce::Graphics& g) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    ui::Hint hintAt (juce::Point<int> position) override;

private:
    double length() const;
    double position() const;
    void seekTo (int x);
    juce::Rectangle<float> track() const;
    AudioEngine& engine;
    double shownFraction = -1.0;
    bool dragging = false;
};

// The time display: bar : beat : step, minutes : seconds : hundredths or SMPTE time code (a click switches; the
// right-click menu picks the mode and the SMPTE frame rate, as FL's time display menu).
class TimeDisplay final : public juce::Component,
                          public ui::HintSource
{
public:
    explicit TimeDisplay (AudioEngine& e) : engine (e)
    {
        setRepaintsOnMouseActivity (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    void update();
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;
    ui::Hint hintAt (juce::Point<int> position) override;
    bool showsTime() const noexcept { return mode != Mode::bars; }

    enum class Mode
    {
        bars,
        clock,
        smpte
    };

private:
    juce::String text() const;
    void showMenu();
    AudioEngine& engine;
    juce::String shown;
    Mode mode = Mode::bars;
    int smpteRate = 5; // timecode::rates index (30 fps)
};

// Master volume (the Master insert's fader) or master pitch (cents, Song::masterPitch): a small knob with
// its name and value beside it.
class MasterKnob final : public juce::Component,
                         public ui::HintSource
{
public:
    enum class Kind
    {
        volume,
        pitch
    };

    MasterKnob (AudioEngine& e, Kind k);
    void update(); // repaints when the song's value changed
    void paint (juce::Graphics& g) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    ui::Hint hintAt (juce::Point<int> position) override;

private:
    ui::ValueSpec spec() const;
    double value() const;
    void setValue (double v);
    bool available() const;
    AudioEngine& engine;
    Kind kind;
    ui::ValueDrag drag;
    double shown = -1.0;
};

// A small light that blinks while MIDI arrives from the enabled input devices (like FL's MIDI activity light).
// A click reconnects the MIDI inputs (closes and opens them again).
class MidiActivityLight final : public juce::Component,
                                public juce::SettableTooltipClient,
                                public ui::HintSource
{
public:
    // `count` = the engine's running count of received messages; call ~30 times a second.
    void update (std::uint32_t count, const juce::StringArray& activeInputs);
    bool isLit() const noexcept { return litFrames > 0; }
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& e) override;
    ui::Hint hintAt (juce::Point<int> position) override;

    std::function<void()> onReconnect;

private:
    std::uint32_t lastCount = 0;
    bool seenCount = false;
    int litFrames = 0;
    juce::StringArray inputs;
};

// CPU load, memory used by LYNX and the polyphony (notes sounding now), in two rows.
class PerformanceMeter final : public juce::Component,
                               public ui::HintSource
{
public:
    void setValues (double cpu01, std::int64_t memoryBytes, int polyphony);
    // Real-time health (REA round 87): callbacks that missed their deadline (the crackles), the highest load since
    // the last update and the heaviest channels / effects ("name 12%").
    // `deviceUnderruns`: the driver's own count (−1: it does not report one), shown like FL's "N underruns".
    void setRealtime (std::uint64_t overDeadline, std::uint64_t lateCalls, double peakLoad, const juce::StringArray& heaviest,
                      int deviceUnderruns = -1);
    // After dropouts on Windows shared mode: the ASIO driver to suggest (round 160; empty = none)
    void setDriverTip (const juce::String& asioDriver) { driverTip = asioDriver; }
    void paint (juce::Graphics& g) override;
    ui::Hint hintAt (juce::Point<int> position) override;

    static std::int64_t processMemoryBytes(); // working set of this process (0 if unknown)

private:
    std::uint64_t overDeadline = 0, lateCalls = 0, underruns = 0;
    int deviceUnderruns = -1;
    juce::String driverTip;
    double peakLoad = 0.0;
    juce::StringArray heaviest;
    juce::uint32 lastGlitchAt = 0; // ms counter of the last new over-deadline callback (0 = none)
    double cpu = 0.0, cpuSmoothed = 0.0;
    std::int64_t memory = 0;
    int voices = 0, peakVoices = 0;
};

// The output: a display of what is heard and a peak meter (red after clipping, for a while). Like FL's
// output panel, the display has modes (right-click menu, or a click steps through them): the oscilloscope,
// a long scrolling view of the level, and a spectrum (low frequencies on the left).
class OutputMonitor final : public juce::Component,
                            public ui::HintSource
{
public:
    explicit OutputMonitor (AudioEngine& e) : engine (e) {}
    void update();
    void paint (juce::Graphics& g) override;
    ui::Hint hintAt (juce::Point<int> position) override;
    void mouseDown (const juce::MouseEvent& e) override;

    enum Mode
    {
        oscilloscope,
        longView,
        spectrum,
        modeCount_
    };
    static constexpr int modeCount = (int) modeCount_;
    static inline int mode = oscilloscope; // shared by every instance; saved in the UI options
    static juce::String modeName (int m);
    std::function<void()> onModeChanged;   // the main window saves the choice

private:
    void setMode (int newMode);
    AudioEngine& engine;
    std::array<float, 1024> samples {};
    std::array<float, 256> history {};     // long view: one peak per update, oldest first
    std::array<float, 128> bands {};       // spectrum: smoothed level per band, 0..1
    float peak = 0.0f, held = 0.0f;
    int holdFrames = 0, clipFrames = 0;
    int silentFrames = 0; // updates in a row with a silent output: once everything has decayed, no more repaints
};

// "Customize toolbar": the groups in toolbar order, each with a check box (shown / hidden), Move up / Move
// down for the selected group (also Alt+Up / Alt+Down), the icon size, the UI scale and Reset to default.
// Every change applies at once through onChange.
class CustomizePanel final : public juce::Component
{
public:
    explicit CustomizePanel (const toolbarlayout::Layout& layout);
    std::function<void (const toolbarlayout::Layout&)> onChange;
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void select (toolbarlayout::Group g);

private:
    void rebuild();
    void changed();
    void move (int delta);
    juce::Rectangle<int> listArea() const;
    static constexpr int rowHeight = 30;
    toolbarlayout::Layout layout;
    toolbarlayout::Group selected = toolbarlayout::Group::transport;
    juce::OwnedArray<juce::ToggleButton> checks;
    juce::TextButton upButton, downButton, resetButton;
    juce::ComboBox iconSizeBox, scaleBox;
};
// The toolbar's edit mode (FL's toolbar "Edit", interpreted; REA_rodada115): while it is on, the toolbar's panels
// show their outlines and names and do not react to clicks; a tray under the toolbar lists every panel.
//  - drag a panel sideways to move it (an accent line shows where it lands)
//  - drag a panel down into the tray to remove it; drag a panel from the tray onto the toolbar to add it there
//  - double-click a panel in the tray: add / remove it; right-click a panel: remove, move left / right
//  - presets (LYNX's own), "Reset" and "Done" (Esc or Enter) on the tray
class EditOverlay final : public juce::Component,
                          public ui::HintSource
{
public:
    using Bounds = std::array<juce::Rectangle<int>, toolbarlayout::groupCount>;
    struct Host
    {
        std::function<toolbarlayout::Layout()> layout;
        std::function<void (const toolbarlayout::Layout&)> setLayout;
        std::function<Bounds()> panels;                       // each shown panel's area, in this component's coordinates
        std::function<juce::Rectangle<int>()> fixedArea;      // the menu and the hint bar (cannot move)
        std::function<juce::Image (juce::Rectangle<int>)> snapshot; // how a panel looks (for the drag image)
        std::function<void()> done;
    };

    static constexpr int trayHeight = 92;

    EditOverlay (Host host, int toolbarHeight);
    void setToolbarHeight (int height);
    void layoutChanged(); // the layout changed elsewhere: the preset box follows

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    ui::Hint hintAt (juce::Point<int> position) override;

    static Icon groupIcon (toolbarlayout::Group g);

private:
    juce::Rectangle<int> trayArea() const { return { 0, toolbarHeight, getWidth(), trayHeight }; }
    juce::Rectangle<int> chipArea() const;
    juce::Rectangle<int> chipBounds (int index) const; // index in the default order
    int panelAt (juce::Point<int> p) const;            // group or -1
    int chipAt (juce::Point<int> p) const;             // group or -1
    int dropX() const;                                 // where the dragged panel would land (x of the accent line), -1 none
    void apply (toolbarlayout::Layout layout);
    void showPanelMenu (toolbarlayout::Group g, bool inTray);
    void move (toolbarlayout::Group g, int delta);

    Host host;
    int toolbarHeight = 0;
    int hoverPanel = -1, hoverChip = -1;
    int pressedGroup = -1;
    bool pressedInTray = false, dragging = false;
    juce::Point<int> dragPos, grabOffset;
    juce::Image dragImage;
    juce::ComboBox presetBox;
    juce::TextButton resetButton, doneButton;
};
} // namespace lynx::toolbar
