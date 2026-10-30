#pragma once

// FL's Channel Rack looping (FL 20.5+; REA_rodada165f-rack §1): per pattern, the Loop button switches it on and its menu
// picks the type (Loop step channels / Loop all channels / Advanced looping). A looping channel plays its notes up to
// its loop point and then again from the pattern's start, up to the pattern's end — channels of different lengths in
// one pattern (polyrhythms). "Burn to pattern" writes those repeats as real notes and switches the channel's loop off.
// Our own code from FL's observed behaviour ([DECOMP] FLEngine FUN_012cfd20 / FUN_01064140 / FUN_01063c80). No JUCE.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx::channelloops
{
// The channel's stored loop point in the pattern (ChannelLoop::dontLoop when it has none).
int storedSetting (const Pattern& pattern, int channel) noexcept;
bool hasStoredSetting (const Pattern& pattern, int channel) noexcept;
// Stores a loop point (adds or replaces the channel's entry).
void setSetting (Pattern& pattern, int channel, int setting);

// The loop point the channel plays with: none while the pattern's looping is off; otherwise its stored one, or (FL's
// "Loop step channels" / "Loop all channels" without one) the next bar — step channels only for the first type.
int effectiveSetting (const Pattern& pattern, int channel) noexcept;

// The last tick a channel's (not muted) notes cover in the pattern: start + length − 1 of the latest one (−1: none).
std::int64_t lastTick (const Pattern& pattern, int channel) noexcept;

// The loop length in ticks for `setting` (0: none): the next step / beat / bar after the channel's last note, or N steps;
// at most the pattern's length (`patternLength`), and the whole pattern when it would be 0.
std::int64_t loopLength (const Pattern& pattern, int channel, int setting, std::int64_t patternLength, int beatsPerBar) noexcept;
// FL's "Set to next beat" / "Set to next bar": the fixed setting (in steps) of the next beat / bar after the last note.
int nextBoundarySteps (const Pattern& pattern, int channel, int boundary, int beatsPerBar) noexcept; // boundary = nextBeat / nextBar

// The notes the channel plays in one pass of the pattern with loop length `length` (> 0, < patternLength): its own
// notes before the loop point, then repeated every `length` ticks while they start before the pattern's end (the
// repeats end at the pattern's end at the latest).
void expandChannel (Pattern& pattern, int channel, std::int64_t length, std::int64_t patternLength);

// What the engine plays: every looping channel of a pattern with looping on written out as notes (the pattern keeps
// its length). Run on the snapshot the audio thread gets, never on the edited song.
void expandPattern (Pattern& pattern, int beatsPerBar);
void expandSong (Song& song);

// "Burn to pattern": the channel's repeats become real notes and its loop point is "Don't loop". Returns false when the
// channel did not loop (nothing changed).
bool burnChannel (Pattern& pattern, int channel, int beatsPerBar);
// "Burn all looping channels" (selectedOnly empty) / "Burn selected channels": how many channels were burnt.
int burnChannels (Pattern& pattern, const std::vector<int>& channels, int beatsPerBar);
std::vector<int> loopingChannels (const Pattern& pattern, int beatsPerBar);

// The text FL's loop control shows: "..." none, "Step", "Beat", "Bar", or the steps ("12").
std::string settingText (int setting);

// Channel indices of the loop entries after a channel was removed (its entry goes) / the channels were reordered
// (newIndexOf[old] = new index).
void removeChannel (Pattern& pattern, int channel);
void remapChannels (Pattern& pattern, const std::vector<int>& newIndexOf);
} // namespace lynx::channelloops
