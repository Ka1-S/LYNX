#pragma once

#include <cstdint>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/MidiImport.h"

namespace lynx
{
class AudioEngine;

// "Pitch..." of a sample channel or an audio clip's channel (round 163): semitones + cents and "Keep length" (the
// stretch engine, else resampling: the speed follows). OK applies it as one undo step (clipops::setChannelPitch).
void showSamplePitchDialog (AudioEngine& engine, int channel);
std::unique_ptr<juce::AlertWindow> makeSamplePitchDialog (float pitch, bool keepLength, bool audioClip);
// FL's "Import MIDI data" window (File › Import › MIDI file..., REA rounds 143 / 146b): the project options, which
// tracks / MIDI channels / controller to import, zero-velocity notes; `pianoRoll` (the piano roll's import into a
// channel) shows "Blend with existing data" instead of the project options. `onAccept` gets the chosen options.
void showMidiImportDialog (const juce::String& fileName, const midiimport::ParsedMidi& midi, const midiimport::Options& current,
                           bool pianoRoll, std::function<void (const midiimport::Options&)> onAccept);
std::unique_ptr<juce::AlertWindow> makeMidiImportDialog();

// "Rename and color" dialog for patterns and channels (docs/WORKFLOW_FL.md, P1 item 4): a name field
// and LYNX's palette (plus "automatic"); F2 inside the dialog picks a random palette color.
// `onDone(name, colour)` runs when the user confirms (colour 0 = automatic / theme default).
void showNameAndColourDialog (const juce::String& title, const juce::String& name, std::uint32_t colour,
                              std::function<void (const juce::String&, std::uint32_t)> onDone);

// The record button's question (FL's "Recording - What would you like to record?", REA_rodada88): audio into
// the Playlist, notes, everything, or audio into the audio editor. `onChoice(choice, dontAskAgain)`: 1 = audio,
// 2 = notes, 3 = everything, 4 = audio editor.
enum RecordChoice
{
    recordChoiceAudio = 1,
    recordChoiceNotes = 2,
    recordChoiceEverything = 3,
    recordChoiceAudioEditor = 4
};
void showRecordChoiceDialog (std::function<void (int, bool)> onChoice);
std::unique_ptr<juce::AlertWindow> makeRecordChoiceDialog();

// The dialog itself, for --snapshot mode.
std::unique_ptr<juce::AlertWindow> makeNameAndColourDialog (const juce::String& title, const juce::String& name, std::uint32_t colour);
} // namespace lynx
