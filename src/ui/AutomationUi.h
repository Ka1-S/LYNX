#pragma once

#include "engine/AudioEngine.h"

namespace lynx
{
// Readable name of what an automation controls ("Kick - Volume", "Insert 3 - Pan"...).
juce::String describeAutomationTarget (AudioEngine& engine, const AutomationTarget& target);

// The target's current value as its control shows it ("78%", "-3.2 dB", "L50", a plugin parameter's own text);
// "" when the target is gone (round 162d: the toolbar's last-tweaked read-out).
juce::String describeTargetValue (AudioEngine& engine, const AutomationTarget& target);

// Creates an automation for the target, starting at the current value, plus a clip of it in the Playlist
// (from the start of the song to its end, on a free track), like FL's "Create automation clip".
void createAutomationClip (AudioEngine& engine, const AutomationTarget& target);

// Round 165: shared by the point editor and the Playlist's in-clip editing — a point's value as text (percent, or BPM
// for the tempo), the copied value of FL's "Copy value" (< 0 = none) and FL's "Type in value..." dialog.
juce::String automationPointValueText (const AutomationTarget& target, float value);
float& copiedAutomationValue();
void typeAutomationPointValue (AudioEngine& engine, int automation, int point);

// Opens (or brings to front) the point editor window of an automation.
void openAutomationEditor (AudioEngine& engine, int automation);
void closeAutomationEditors();

// Just the editor (for --snapshot mode).
std::unique_ptr<juce::Component> makeAutomationEditor (AudioEngine& engine, int automation);

// A parameter's menu items, in FL's order (REA round 143 / 146): Edit events in the pattern, Edit events in piano
// roll, Init song with this position, Delete initial value, Create automation clip; remote control: Link to MIDI
// controller, Override global link..., internal controllers, unlink.
// The parameter menu's automation and remote-control items; with `headers`, under FL's section titles "Automation" and
// "Remote control" (REA round 149 [EXEC]: FL's parameter menu).
void addAutomationItem (juce::PopupMenu& menu, AudioEngine& engine, const AutomationTarget& target, bool headers = false);
// Opens the Piano roll with the target's pattern events in its bottom lane (the main window sets it).
extern std::function<void (const AutomationTarget&)> onEditEventsInPianoRoll;

// An output of a built-in controller that knobs can be linked to (Envelope / Keyboard Controller channels, Formula and
// Peak Controller mixer slots), as FL's "Link to controller" lists them.
struct ControllerOutput
{
    int track = -1, slot = 0, output = 0; // a mixer slot's controller effect, or
    int channel = -1;                     // a controller channel
    juce::String label;
};
std::vector<ControllerOutput> controllerOutputs (const Song& song);
// Drives `target` live from that output (replaces the target's previous controller link).
void linkToController (AudioEngine& engine, const AutomationTarget& target, const ControllerOutput& source);
// "Link to internal controller" submenu.
void addControllerLinkItems (juce::PopupMenu& menu, AudioEngine& engine, const AutomationTarget& target);

// "MIDI CC #64 (Damper pedal (sustain))", "MIDI Channel 2 Aftertouch"… for a MIDI index (core/MidiControls.h).
juce::String midiParamLabel (int param);

// Submenu with the MIDI CCs, channel aftertouch and pitch bend a hosted plugin can be automated with (FL's wrapper).
void addMidiParameterItems (juce::PopupMenu& menu, AudioEngine& engine, const AutomationTarget& base);

// Submenu with a plugin's parameters (channel instrument or slot effect).
void addPluginParameterItems (juce::PopupMenu& menu, AudioEngine& engine, juce::AudioPluginInstance& plugin,
                              const AutomationTarget& base);
} // namespace lynx
