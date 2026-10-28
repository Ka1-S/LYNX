#pragma once

// FL's Patcherize (round 165m, [DFM] Channel menu › Patcherize "Convert the channel to Patcher with the current plugin
// loaded", mixer slot menu › Patcherize / Patcherize all): the channel's instrument, or a mixer insert's effect chain,
// moves into a new Patcher that plays the same sound — the generator cabled From LYNX (notes) → it → To LYNX, the
// effects in series From LYNX → 1 → 2 → … → To LYNX. The channel keeps its channel settings (volume, pan, insert,
// arpeggiator, time / echo…) outside; each effect keeps its own on/off and dry/wet inside. Automation and controller
// links of the moved plugin's parameters follow it: they now drive the Patcher's published parameters.

#include <vector>

#include "core/Song.h"

namespace lynx::patcherize
{
// Whether the channel's instrument can go into a Patcher (not a Patcher, Layer, audio clip, controller or MIDI Out…).
bool canPatcherizeChannel (const Song& song, int channel);
// Turns the channel's instrument into a generator Patcher holding it. False (song untouched) when it can't.
bool patcherizeChannel (Song& song, int channel);

// The slots of insert `track` that "Patcherize all" would take (every slot that can move: not a Fruity Send, and at
// most one side-chain source among them), in order.
std::vector<int> slotsForAll (const Song& song, int track);
bool canPatcherizeSlot (const Song& song, int track, int slot);
// Moves slot `slot` (or, with `all`, slotsForAll) into a new Patcher at the first moved slot's place. Returns the new
// slot's index, or −1 (song untouched).
int patcherizeSlots (Song& song, int track, int slot, bool all);
} // namespace lynx::patcherize
