#pragma once

// FL's Layer channel (docs/analise/REA_rodada164a.md §1; FL's manual "Layer channel settings"): a channel that makes no
// sound and plays its children channels with every note it gets. Clean-room: our own code from FL's documented and
// observed behaviour.
//
//  - every child plays each note (Random off, Sequential off); Random: one child at random per note; Sequential:
//    the children in turn (round robin);
//  - Crossfade: the Fade knob moves through the children top to bottom (child 1, 1+2 blend, child 2…);
//  - the Layer's VOL / PAN / PITCH apply to the notes it plays (a child played from its own notes is not affected);
//  - a child plays only the notes inside its key region (ChannelDef::keyLow / keyHigh, any channel); "Split children"
//    gives each child one key from the Layer's root key up, with its root on that key.

#include <cstdint>
#include <vector>

#include "core/Song.h"

namespace lynx::layer
{
// A child a note plays and the note's level factor for it.
struct Pick
{
    int child = 0;
    float gain = 1.0f;
};

// The children (valid channel indices that are not Layers themselves) of channel `layer`.
std::vector<int> validChildren (const Song& song, int layer);

// The level factor of the Layer's VOL knob for its notes: FL multiplies the child voice by the Layer's channel gain
// (the channel volume curve, about −5 dB at the default 78 %; REA_rodada164a §1).
float levelGain (const ChannelDef& layer) noexcept;

// Crossfade: the gain of child `index` of `count` at Fade `fade` (0..1), FL's triangle max (0, 1 − |fade·(count − 1) −
// index|): each child sounds fully at its own position and fades out linearly towards its neighbours.
float crossfadeGain (int index, int count, float fade) noexcept;

// The pitch shift (cents) of the Layer's notes on its children: its PITCH knob minus (root note − C5) × 100.
float pitchCents (const ChannelDef& layer) noexcept;

// Picks the children a note-on of `key` plays (at most `max`, into `out`). `random` is a 32-bit LCG state; `turn` the
// Sequential counter of this Layer (both advanced). Children whose key region excludes `key` are skipped (a Random /
// Sequential pick only chooses among the children that take the key). Returns how many. Audio thread: no allocation.
int pick (const ChannelDef& layer, const std::vector<ChannelDef>& channels, int key, std::uint32_t& random, int& turn, Pick* out, int max) noexcept;

// "Set children": the selected channels (sorted; the Layer itself and other Layers left out) become its children.
void setChildren (Song& song, int layer, std::vector<int> selected);
// "Split children": child k plays only key (root + k) and its root note moves there (a sampler's pitch follows so the
// sample keeps sounding at its own pitch on its key). "Reset children": every child back to the whole keyboard and
// its root on C5.
void splitChildren (Song& song, int layer);
void resetChildren (Song& song, int layer);

// Moves a channel's root note to `key` the way the Root note keyboard does: a sampler's pitch moves with it (within
// ±24 semitones) so the sample keeps its pitch on the root; other instruments just store it.
void setRootNote (ChannelDef& channel, int key);

// The Layers whose children include `channel`.
std::vector<int> parentsOf (const Song& song, int channel);
} // namespace lynx::layer
