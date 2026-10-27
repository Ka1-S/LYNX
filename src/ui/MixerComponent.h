#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/MeterBallistics.h"
#include "engine/AudioEngine.h"
#include "ui/ValueControl.h"

namespace lynx
{
// The built-in effects offered in the slot menus and the quick-add palette, in menu order.
const std::vector<EffectKind>& addableEffects();
// The FL engines among them need the user's FL installation (core/NativeEngines.h, round 142): whether one can be
// added here, its menu name ("(beta)", "(requires FL Studio)") and a new slot (nullopt = not available here)
bool addableEffectAvailable (EffectKind kind);
juce::String addableEffectName (EffectKind kind);
std::optional<EffectSlot> newAddableEffect (EffectKind kind, double bpm = 120.0);

// FL's mixer track state (Mixer track menu › File, REA round 143), in LYNX's .lynxtrack format: load onto `track`
// (or, with `asNewInsert`, onto a new insert after it, named after the state), save `track`'s state.
bool loadMixerTrackState (AudioEngine& engine, int track, const juce::File& file, bool asNewInsert);
bool saveMixerTrackState (AudioEngine& engine, int track, const juce::File& file);

// External audio input of the inserts (REA_rodada88): FL's "Audio input source" of a mixer track.
namespace mixerinput
{
// Opens Settings > Audio (the main window sets it).
extern std::function<void()> openAudioSettings;
// The insert's input menu: (none), the open device's stereo pairs and single channels, the monitoring mode and
// "Audio settings...". Opens the device input first when it is closed. `chosen` runs after a source is picked.
void showMenu (AudioEngine& engine, int track, juce::Component* target, std::function<void()> chosen = {});
// The arm switch: disarms, or arms; an insert without an input source asks for one first ("Audio input
// needed") and is armed once it has one.
void toggleArm (AudioEngine& engine, int track, juce::Component* target);
// Explains a recording problem to the user, with a way to Settings > Audio when it is about the device.
void explain (const AudioEngine::RecordingNotice& notice);
} // namespace mixerinput

// Hardware output targets (REA round 156; LYNX extra): an insert can also go straight to device outputs, a stereo
// pair or one channel alone (mono: sum / left / right). Optional: by default inserts reach the device via the Master.
namespace mixeroutput
{
// "(none)", "Out 1 - Out 2", "Out 3 (mono)", "... (missing)" — translated. `compact`: the strip badge ("→ 3 m").
juce::String label (const AudioEngine& engine, int track, bool compact = false);
// Whether the track has a target other than its default (inserts: any target; Master: not the first pair).
bool isCustom (const AudioEngine& engine, int track);
// The output target menu of a track.
void showMenu (AudioEngine& engine, int track, juce::Component* target, juce::Rectangle<int> screenArea);
} // namespace mixeroutput

// FL-style mixer: one compact vertical strip per track (Master first).
//  - header: record arm, number, name (once), effect count; drag it sideways to reorder inserts
//  - pan, stereo separation and fader: the shared control contract (ui/ValueControl.h): drag
//    (Ctrl: fine), wheel, Alt/middle-click or double-click resets, right-click: type value / reset /
//    automation; the value shows next to the mouse while dragging
//  - green LED = mute (click), "S" = solo; stereo peak meters with peak hold and a clip latch
//    (click the meter to clear it)
//  - clicking a strip selects it; right-click opens the menu (rename, route the selected
//    inserts to this track); "+" at the end adds an insert
//  - routing lane at the bottom: with an insert selected, every other strip shows a send switch
//    (click toggles the send from the selected insert; right-click: send only / sidechain), a send
//    level knob when the send is on, and wires show where the selected insert goes (red) and what
//    feeds it (blue)
class MixerStrips final : public juce::Component,
                          public ui::HintSource,
                          public juce::DragAndDropTarget,
                          private juce::ChangeListener,
                          private juce::Timer
{
public:
    explicit MixerStrips (AudioEngine& engine);
    ~MixerStrips() override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    ui::Hint hintAt (juce::Point<int> position) override;

    void updateSize();
    void setVisibleHeight (int height);

    int getSelectedTrack() const noexcept { return selected; }
    void selectTrack (int track);
    std::function<void (int)> onSelectionChanged;

    // An effect dragged from the Browser's Plugin database onto a strip (round 162): it goes into that insert.
    std::function<void (int, EffectSlot)> onEffectDropped;
    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDropped (const SourceDetails& details) override;

    // Multi-selection (docs/WORKFLOW_FL.md P2, M1): Ctrl+click on a strip's header toggles it, Shift+click
    // selects a range; selected faders and pans move together. Always contains the selected track.
    const std::vector<int>& getSelection() const noexcept { return selection; }
    void setSelection (std::vector<int> tracks, int primary);
    bool moveSelected (int delta);           // Alt+Left/Right
    void linkSelectedChannels (bool startingHere); // Ctrl+L / Ctrl+Shift+L
    void selectLinkedChannels();             // Alt+L
    // The insert's right-click menu (FL's mixer track menu, REA rounds 115/134/143); public for --snapshot and tests.
    juce::PopupMenu buildMenu (int track);

    static constexpr int stripWidth = 60;
    static constexpr int masterWidth = 76;
    // x of a strip (`track` == the number of tracks: the "+" button), after the docks and separators (round 146a)
    int stripLeft (int track) const noexcept { return stripX (track); }

    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    enum class Part { none, pan, stereo, fader, meter, mute, solo, route, send, header, body, add, arm, input };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    int stripX (int track) const noexcept;
    // FL's docks and separators (REA round 146a): the strips' x in display order (left block, middle, right), the
    // x of each gap between two dock blocks and of each separator line
    void layoutStrips();
    std::vector<int> stripXs;                                    // layout x (before pinning)
    std::vector<std::pair<int, MixerDock>> dockGaps, separatorLines;
    int blockStart[3] {}, blockEnd[3] {};                        // each dock block's layout span (0 width = empty)
    int contentWidth = 0;
    // FL keeps the left / right docks in view while the middle scrolls (round 149): the x shift of a dock block
    // for the viewport's current scroll position (0 for the middle block)
    int pinOffset (MixerDock dock) const noexcept;
    MixerDock dockOf (int track) const noexcept;
    void moved() override { repaint(); }
    static constexpr int dockGap = 14, separatorGap = 8;
    int stripWidthOf (int track) const noexcept { return track == 0 ? masterWidth : stripWidth; }
    int trackAt (int x) const noexcept;
    Part partAt (int track, juce::Point<int> p) const;
    juce::Rectangle<int> faderArea (int track) const;
    juce::Rectangle<int> panArea (int track) const;
    juce::Rectangle<int> muteArea (int track) const;
    juce::Rectangle<int> soloArea (int track) const;
    juce::Rectangle<int> routeArea (int track) const;
    juce::Rectangle<int> armArea (int track) const;
    juce::Rectangle<int> inputArea (int track) const; // the input level bar (inserts with an input source)
    juce::Rectangle<int> stereoArea (int track) const;
    juce::Rectangle<int> meterArea (int track) const;
    juce::Rectangle<int> sendArea (int track) const;   // send level knob (routing lane)
    juce::Rectangle<int> headerArea (int track) const;
    juce::Rectangle<int> faderRail (int track) const;  // the fader without the meters
    int laneTop() const noexcept;
    void paintRouting (juce::Graphics& g);
    void showTip (Part part, int track, bool transient);
    juce::Rectangle<int> tipArea (Part part, int track) const;
    void setHover (Part part, int track);
    Part hoverPart = Part::none;
    int hoverTrack = -1;
    int headerDragTrack = -1;   // dragging a header sideways reorders the inserts
    bool headerDragged = false;
    void showSendMenu (int track);
    void soloExclusive (int track);
    std::vector<int> selection { 0 };
    std::vector<std::pair<int, float>> groupStart; // (track, value) of the selection when a group drag starts
    float groupGrab = 0.0f;
    void showMenu (int track);
    void showTrackEq (int track);
    ui::ValueSpec specFor (Part part, int track) const;
    void setValue (Part part, int track, double value);
    double valueOf (Part part, int track) const;

    AudioEngine& engine;
    int selected = 0;
    int visibleHeight = 400;

    Part dragPart = Part::none;
    int dragTrack = -1;
    float dragStartValue = 0.0f;
    ui::ValueDrag drag;

    std::vector<meters::PeakHold> levels; // per track*2: bar, peak hold and clip latch
    std::vector<float> inputLevels;       // per track: input level (decaying)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrips)
};

// Effects panel for the selected track, in FL's layout (REA round 104): the audio input row, 10 slots, the
// knobs of the chosen slot (LYNX), then the insert's built-in 3-band EQ and its audio output target.
//  - click an empty slot ("Slot N"): choose an effect; click an occupied one: edit its knobs; double click: its window
//  - the arrow on the left of a slot (or right-click): the slot's options (add / replace, move, delete...)
//  - on the right of a slot: the mix knob (drag; double-click resets to 100%) and the LED (on/off, bypass)
//  - EQ: drag a band's handle in the graph (frequency and gain; wheel: width), its fader (gain) or its knobs
//    (frequency, width)
class EffectsPanel final : public juce::Component,
                           public ui::HintSource,
                           public juce::DragAndDropTarget,
                           public juce::FileDragAndDropTarget,
                           private juce::ChangeListener,
                           private juce::Timer
{
public:
    // Effect presets (.fst) dragged from the Browser or Explorer, and effects dragged from the Browser's Plugin
    // database (round 162): dropped on a slot they replace it, anywhere else they go into the first free slot.
    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDropped (const SourceDetails& details) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    bool dropPreset (const juce::File& file, juce::Point<int> position);
    bool dropEffect (EffectSlot fx, juce::Point<int> position);

    explicit EffectsPanel (AudioEngine& engine);
    ~EffectsPanel() override;

    void setTrack (int track);
    int getSelectedSlot() const noexcept { return selectedSlot; }
    void selectSlot (int slot)
    {
        selectedSlot = slot;
        repaint();
    }
    void openSlotEditor (int slot) { openEditor (slot); }

    // Sizes the panel for a view of `viewWidth` x `viewHeight` (round 163): when the slots and the EQ don't both fit,
    // the EQ folds to its title bar (click it to unfold); when even that doesn't fit, the panel is taller than the
    // view and the viewport around it scrolls. Nothing overlaps at any height or UI zoom.
    void fitToView (int viewWidth, int viewHeight, int scrollBarThickness);
    bool isEqFolded() const noexcept { return eqFolded; }
    // the heights the panel needs with the EQ open and folded (the knobs of the selected effect may be cut below)
    int neededHeight (bool folded) const;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    ui::Hint hintAt (juce::Point<int> position) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void showTip (int param, int slot, bool transient);
    // the input row above the slots (inserts only): arm switch, input source box with its level
    juce::Rectangle<int> armBounds() const;
    juce::Rectangle<int> inputBoxBounds() const;
    int hoverInput = 0; // 1 = arm, 2 = input box
    std::array<float, 2> inputLevel {};
    int hoverSlot = -1, hoverParam = -1;  // hover feedback: the slot row, the knob (mixParam = a slot's dry/wet)
    bool hoverLed = false;
    ui::ValueSpec specFor (int param, int slot) const; // param = mixParam: the slot's dry/wet
    double valueOf (int param, int slot) const;
    void setValue (int param, int slot, double value);
    int mixKnobAt (juce::Point<int> p) const; // slot whose dry/wet knob is at p, or -1
    juce::Rectangle<int> slotBounds (int slot) const;
    juce::Rectangle<int> mixBounds (int slot) const; // dry/wet knob on the slot's row
    static constexpr int mixParam = -2;
    // FL's layout (REA round 104)
    juce::Rectangle<int> arrowBounds (int slot) const; // the slot's options arrow
    juce::Rectangle<int> ledBounds (int slot) const;   // the slot's on/off LED
    int bottomTop() const;                             // top of the EQ + output section
    int belowHeight (bool folded) const;               // height of the EQ + output section
    juce::Rectangle<int> eqFoldBounds() const;         // the EQ's title bar (click: fold / unfold)
    void refit();
    bool eqFolded = false;
    int viewWidth = 0, viewHeight = 0, scrollBarThickness = 0;
    static inline int eqFoldChoice = 0;                // 0 automatic, 1 open (the user unfolded it), 2 folded by the user
    juce::Rectangle<int> eqGraphBounds() const;
    juce::Rectangle<int> eqFaderBounds (int band) const;
    juce::Rectangle<int> eqKnobBounds (int band, int what) const; // what: 1 = frequency, 2 = width
    juce::Rectangle<int> outputBounds() const;
    juce::Point<float> eqHandle (int band) const;      // where a band's handle sits in the graph
    int eqControlAt (juce::Point<int> p) const;        // an EQ control under p (eqParamBase/eqPointBase codes), or 0
    void paintEq (juce::Graphics& g);
    void setEqFromGraph (int band, juce::Point<int> p);
    void showOutputMenu();
    juce::String outputName() const;
    static constexpr int eqParamBase = -100; // eqParamBase - band * 3 - what (0 gain, 1 frequency, 2 width), stored 0..1
    static constexpr int eqPointBase = -200; // eqPointBase - band: dragging the band's handle in the graph
    static bool isEqParam (int p) noexcept { return p <= eqParamBase && p > eqParamBase - 9; }
    static bool isEqPoint (int p) noexcept { return p <= eqPointBase && p > eqPointBase - 3; }
    int hoverEq = 0;
    bool hoverOutput = false, hoverArrow = false;
    static constexpr int gateStepParam = -3; // drawing the volume gate steps
    juce::Rectangle<int> knobBounds (int param) const;
    bool gateSelected() const;
    juce::Rectangle<int> gateStepsBounds() const;
    void setGateStep (juce::Point<int> p);
    void showGatePatterns();
    static constexpr int bandLevelParam = -4; // drawing the Fruity Vocoder's band levels
    bool vocoderSelected() const;
    juce::Rectangle<int> bandLevelsBounds() const;
    void setBandLevel (juce::Point<int> p);
    void showBandLevelMenu();
    bool tuneSelected() const;                // LYNX Tune: its 12 note switches are a keyboard strip
    juce::Rectangle<int> tuneKeysBounds() const;
    void toggleTuneNote (juce::Point<int> p);
    int knobAt (juce::Point<int> p) const;
    const std::vector<EffectSlot>* slots() const;
    void showAddMenu (int slot);
    void showSlotMenu (int slot);
    juce::Rectangle<int> pluginButtonBounds() const;
    void openEditor (int slot);
    juce::Rectangle<int> editorButtonBounds() const;
    void openPluginWindow (int slot);
    juce::Rectangle<int> fxSwitchBounds() const; // all effects on/off; right-click opens/closes their windows
    void toggleAllWindows();
    int slotAt (juce::Point<int> p) const;
    int dragSlot = -1, dropSlot = -1; // dragging a slot to reorder it
    // A/B compare: a spare state per (insert, slot), swapped with the live one on request (not saved)
    std::map<std::pair<int, int>, EffectSlot> spare;
    std::unique_ptr<juce::FileChooser> sampleChooser; // Newtone / Newtime source sample

    AudioEngine& engine;
    int track = 0;
    int selectedSlot = -1;
    int dragParam = -1;
    float dragStart = 0.0f;
    ui::ValueDrag drag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectsPanel)
};

class MixerComponent final : public juce::Component
{
public:
    explicit MixerComponent (AudioEngine& engine);
    void resized() override;

    // Selects the first track that has effects (used by --snapshot mode).
    void selectFirstTrackWithEffects();
    void selectTrack (int track); // also scrolls the strip into view
    void selectSlot (int slot) { effects.selectSlot (slot); }
    // Selects the insert and slot and opens the effect's editor (the Browser's "Current project > Effects")
    void openSlotEditor (int track, int slot)
    {
        selectTrack (track);
        effects.selectSlot (slot);
        effects.openSlotEditor (slot);
    }

    // The key map's mixer.* actions.
    bool runAction (const std::string& id);

    // Adds an effect to the first free slot of the selected insert (quick-add palette). False when full.
    bool addEffectToSelected (EffectSlot slot);
    MixerStrips& getStripsForSnapshot() { return strips; }

    static constexpr int effectsWidth = 300;

private:
    AudioEngine& engine;
    MixerStrips strips;
    juce::Viewport viewport;
    juce::Viewport effectsView; // the effects panel scrolls when the mixer is short (round 163)
    EffectsPanel effects;
};
} // namespace lynx
