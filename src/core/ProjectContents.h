#pragma once

// What the Browser's "Current project" section lists (FL's Browser > Project, interpreted; REA_rodada115): the
// patterns, the audio files the project uses, the generators (channels), the effects of every insert and the
// automation clips, each with the colour of what it belongs to and a short detail. No JUCE, no file access.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx::projectcontents
{
enum class Kind
{
    pattern,
    sample,
    generator,
    effect,
    automation
};

struct Item
{
    Kind kind = Kind::pattern;
    int index = -1;  // pattern / channel / insert / automation index; samples: the first channel that uses the file
    int slot = -1;   // effects: the slot in the insert
    std::string name;
    std::string detail;   // shown dimmed after the name: "(Kick)", the insert's name, "3 channels"...
    std::string path;     // samples: the audio file
    std::uint32_t colour = 0; // 0xFFRRGGBB of what the item belongs to; 0 = none (the UI picks one)
    int uses = 0;         // samples: channels and effects that play the file; patterns: clips in the Playlist
    bool plugin = false;  // generators / effects: a third-party plugin
};

// Does this instrument play `ChannelDef::samplePath` as audio (and not as the path of an engine file)?
bool playsSampleFile (InstrumentKind kind) noexcept;
// Built-in plugins whose own window takes a dropped sample (Browser or system) and plays it, as FL's do: the Granulizer
// (audit 165: the drop fell through to the Playlist). The Sampler's waveform has its own drop target.
bool pluginWindowTakesSample (InstrumentKind kind) noexcept;

std::vector<Item> patterns (const Song& song);
// Every audio file used by a channel (sample, audio clip, sampler zones) or an effect (source sample), once, in the
// order first used; `colour` and `index` come from the first channel using it.
std::vector<Item> samples (const Song& song);
// Channels: the instrument's name with the insert it plays into as the detail.
std::vector<Item> generators (const Song& song);
// Effect slots of every insert (Master first): the effect's name, the insert's name as the detail.
std::vector<Item> effects (const Song& song);
std::vector<Item> automations (const Song& song);

// The name an effect slot shows (plugin name, the original plugin's for a LYNX substitute, else the built-in name).
std::string effectSlotName (const EffectSlot& slot);
// The insert's name ("Master", or "Insert N" when unnamed).
std::string insertName (const Song& song, int track);
} // namespace lynx::projectcontents
