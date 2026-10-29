#pragma once

// LYNX's editors for a Newtone / Newtime mixer slot (round 142, issue #5): load a sample, analyse it, edit it, hear it.
//  - Newtone (FL's workflow, round 165g): the sample's notes on a pitch grid with a keyboard ruler. Drag a note up or
//    down to change its pitch (snapped to semitones or the chosen scale; Alt = free), Ctrl+drag to move it in time;
//    Shift+click / a drag on empty space selects several (Ctrl+A all); a click on the ruler moves the selected notes
//    to that key; double-click snaps; Cut mode (C) slices a note where clicked, Ctrl+click glues it to the one before.
//    The Center / Variation / Transition knobs act on the selection (or every note); the note panel (FL's Advanced
//    Edit) sets the selected notes' pitch, formant, volume, variation, ramps, vibrato and tremolo. Score: send the
//    notes to a channel's piano roll or save them as MIDI.
//  - Newtime: the sample's waveform with warp markers. Double-click adds a marker, dragging it moves where that point
//    of the sample plays (the sample stretches around it, pitch kept); right-click a marker to remove it.
// The edits are the slot's state (LYNX's own format); "Play with the song" is the plugin's slave mode, Preview plays
// the edited sample alone. Imported FL Newtone states open with their notes; editing one saves it in LYNX's format.

#include "engine/AudioEngine.h"

namespace lynx
{
namespace NewtoneWindows
{
bool handles (EffectKind kind); // newtone, newtime
void open (AudioEngine& engine, int track, int slot);
void closeAll();
} // namespace NewtoneWindows

// The editor alone (snapshots, tests)
std::unique_ptr<juce::Component> makeNewtoneEditor (AudioEngine& engine, int track, int slot);
} // namespace lynx
