#pragma once

#include <functional>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"

// LYNX's control contract (docs/WORKFLOW_FL.md, P1 items 7 and 8). Every knob, fader, route box and
// number display behaves the same way:
//   - drag up/down to change, Ctrl+drag for fine steps;
//   - mouse wheel = small step (Ctrl+wheel finer);
//   - Alt+click or middle-click resets to the default (double-click also resets);
//   - right-click opens one menu: Type value..., Reset, then Create automation clip / Link to MIDI
//     controller / Unlink when the control can be automated.
// Custom-painted controls (rack knobs, mixer faders, effect knobs) use ValueSpec + ValueDrag +
// showValueMenu(); plain juce::Sliders get the same contract from installSliderContract().
//
// The hint strip reads `hintFor()`: a HintSource component describes what is under the mouse, other
// components use their hint property or tooltip.
namespace lynx::ui
{
struct ValueSpec
{
    juce::String name;                 // "Kick - Volume"
    double minimum = 0.0, maximum = 1.0, defaultValue = 0.0;
    double interval = 0.0;             // > 0: integer-like controls (route box)
    double pixelsForRange = 150.0;     // drag distance that sweeps the whole range
    // shown value <-> stored value (e.g. 0..1 volume shown as "78%"). Both optional.
    std::function<juce::String (double)> toText;
    std::function<std::optional<double> (const juce::String&)> fromText;
    std::optional<AutomationTarget> target; // automatable: adds the automation / MIDI items

    juce::String text (double value) const;
    std::optional<double> parse (const juce::String& typed) const;
};

// The usual specs.
ValueSpec percentSpec (const juce::String& name, double defaultValue);          // 0..1 shown as 0..100%
ValueSpec panSpec (const juce::String& name);                                   // -1..1 shown as L50 / C / R50
ValueSpec faderSpec (const juce::String& name, double defaultValue);            // 0..1 fader shown in dB

// A utility volume (metronome, Browser preview; core/ValueMath.h utilityGain) as "71% (-6.0 dB)".
juce::String utilityVolumeText (double volume);

// A type-in parser for any display function: finds the value whose shown number is closest to the
// typed one (searches the range in fine steps, so it also works for non-linear curves).
std::function<std::optional<double> (const juce::String&)> inverseOf (std::function<juce::String (double)> toText, double minimum,
                                                                      double maximum);

// One drag gesture on a custom-painted control.
class ValueDrag
{
public:
    void begin (const juce::MouseEvent& e, double value);
    // New value for the current mouse position (Ctrl held = fine; the switch is seamless mid-drag).
    double update (const juce::MouseEvent& e, const ValueSpec& spec);
    bool isActive() const noexcept { return active; }
    void end() noexcept { active = false; }

private:
    double startValue = 0.0;
    float anchorY = 0.0f;
    bool fine = false, active = false;
};

// FL's "Last tweaked" (REA round 143): drags and wheel steps of a control with a target report it here (the main
// window passes it to AudioEngine::noteTweaked). Other controls call reportTweak themselves.
extern std::function<void (const AutomationTarget&)> onTweak;
void reportTweak (const ValueSpec& spec);

// Alt+left-click or middle-click.
bool isResetClick (const juce::MouseEvent& e) noexcept;

// Value after a wheel move over the control (nothing if the wheel did not move vertically).
std::optional<double> wheelValue (double value, const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel, const ValueSpec& spec);

// The right-click menu. `set` receives the new value (typed or reset). `engine` may be null when the
// control cannot be automated.
void showValueMenu (juce::Component& target, AudioEngine* engine, const ValueSpec& spec, double current, std::function<void (double)> set);

// A juce::Slider's parameter menu in FL's order (name: value, Reset, Automation, Remote control, Value), with the
// automation items when `target` is set. A slider that shows it itself calls setOwnMenu, so the app-wide slider
// contract does not open its plain value menu on top of it (two menus at once, round 165z-b).
void showSliderMenu (juce::Slider& slider, AudioEngine* engine, const std::optional<AutomationTarget>& target);
void setOwnMenu (juce::Slider& slider);

// A context menu's title band in the colour of what it is about (FL tints a mixer track's menu with the track's
// colour; LYNX draws a band in its own style): `kind` in small caps ("Mixer track"), then `name`. A transparent
// colour draws a neutral band.
// `onDoubleClick` (optional): a double-click on the title closes the menu and calls it (rename, issue #12).
void addItemHeader (juce::PopupMenu& menu, const juce::String& kind, const juce::String& name, juce::Colour colour,
                    std::function<void()> onDoubleClick = {});
void addValueItems (juce::PopupMenu& menu, juce::Component& target, AudioEngine* engine, const ValueSpec& spec, double current,
                    std::function<void (double)> set);

// "Type value" dialog (asynchronous).
void typeValue (juce::Component& target, const ValueSpec& spec, double current, std::function<void (double)> set);

// --- value read-out --------------------------------------------------------------------------------
// A small floating label with the value, shown next to the control while it is dragged (FL shows the
// value in the hint bar; LYNX also shows it at the control so the eye can stay there).
//   showValueTip: during a drag (stays until hideValueTip, normally on mouse up);
//   flashValueTip: after a wheel step or a reset click (fades out on its own).
// `control` is the control's area in `over`'s coordinates; the tip sits above it.
void showValueTip (juce::Component& over, juce::Rectangle<int> control, const juce::String& text);
void flashValueTip (juce::Component& over, juce::Rectangle<int> control, const juce::String& text);
void hideValueTip();

// --- hints -----------------------------------------------------------------------------------------

struct Hint
{
    juce::String text;         // what the control does
    juce::String value;        // its current value ("" if none)
    bool automatable = false;  // can be automated / linked to MIDI
    bool hasMenu = false;      // right-click opens a menu

    bool isEmpty() const noexcept { return text.isEmpty() && value.isEmpty(); }
};

// Components that paint several controls themselves describe the one at `position`.
class HintSource
{
public:
    virtual ~HintSource() = default;
    virtual Hint hintAt (juce::Point<int> position) = 0;
};

// One line of text fitted the way FL fits its captions (round 165): in `area`, or over up to `room` px of free space
// around it (by the justification) when that is wider; the font shrinks down to `minHeight`, then the text narrows
// down to `minScale` — no "..." while it can still be read.
void drawFittingText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification just, juce::Font font,
                      float minHeight = 7.5f, float room = 0.0f, float minScale = 0.6f);

// A fixed hint for a component (stored in its properties; wins over the tooltip).
void setHint (juce::Component& c, const juce::String& text, bool hasMenu = false, bool automatable = false);

// The hint for the component under the mouse (walks up to the parents).
Hint hintFor (juce::Component* component, juce::Point<int> positionInComponent);

// Hint for a value control described by a spec.
Hint valueHint (const ValueSpec& spec, double value, const juce::String& gesture = {});

// Gives every juce::Slider in the app the contract (installed once, by the main window): right-click
// menu, middle-click reset, Ctrl-drag fine, finer Ctrl+wheel. Sliders with the property
// "lynxOwnContract" set are left alone.
void installSliderContract();
void uninstallSliderContract();
} // namespace lynx::ui
