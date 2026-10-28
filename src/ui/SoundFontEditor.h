#pragma once

// The LYNX Soundfont editor (round 158, GitHub issue #6): the font and its preset (bank:patch), FL Soundfont Player-
// style overrides (envelope, pitch LFO, filter, reverb / chorus sends, options) and a preset browser: the loaded font's
// presets with bank:program and category, and a search over every SoundFont in the user's folders (indexed in the
// background, cached between sessions). Click previews a preset, double-click loads it into this channel, dragging a
// preset onto the Channel rack makes a new channel. docs/analise/REA_rodada158.md.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/AudioEngine.h"

namespace lynx
{
std::unique_ptr<juce::Component> makeSoundFontEditor (AudioEngine& engine, int channel);
// The same editor on a LYNX Soundfont node inside a Patcher (round 165c), found through `locate`
std::unique_ptr<juce::Component> makeSoundFontEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate);

// The folders the preset search indexes: FL's SoundFont folders (when FL is installed), Documents, Music, Desktop,
// Downloads, the Browser's own folders and the user's SoundFont folders (Settings: soundfont-folders.txt).
std::vector<std::string> soundFontFolders();
juce::File soundFontFoldersFile();
// Starts (or refreshes) the background index of soundFontFolders() and returns the shared index's preset count.
void ensureSoundFontIndex (bool force = false);

// Plays a short note of any preset (the font is loaded and the note rendered off the message thread)
void previewSoundFontPreset (AudioEngine& engine, const std::string& path, int bank, int program);

// A new LYNX Soundfont channel playing that preset (at the end of the rack, or at `insertAt`); returns its index
int addSoundFontChannel (AudioEngine& engine, const std::string& path, int bank, int program, int insertAt = -1);
// Loads a font / preset into an existing LYNX Soundfont channel (its other settings stay)
void loadSoundFontInto (AudioEngine& engine, int channel, const std::string& path, int bank, int program);

// The lowest bank:patch of a font file (false: not a readable SoundFont)
bool firstSoundFontPreset (const std::string& path, int& bank, int& program);
// A dropped / opened .sf2 / .sf3: into `channel` when it is a LYNX Soundfont, else a new channel; false when unreadable
bool openSoundFontFile (AudioEngine& engine, const juce::File& file, int channel = -1);

// Drag-and-drop of a preset from the editor's lists ("lynx-sf-preset|bank|program|path")
juce::String soundFontDragDescription (const std::string& path, int bank, int program);
bool parseSoundFontDrag (const juce::String& description, std::string& path, int& bank, int& program);
} // namespace lynx
