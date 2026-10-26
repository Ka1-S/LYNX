#pragma once

// Mixer workflow operations (docs/WORKFLOW_FL.md P2: insert multi-selection and reordering, routing and
// sidechain gestures, linking channels, effect slot reordering). Pure functions over the Song model:
// no JUCE, unit-tested in tests/WorkflowP2Tests.inc.

#include <vector>

#include "core/Song.h"

namespace lynx::mixerops
{
// Swaps inserts a and b (both >= 1): everything that refers to them by number follows (channel routing,
// sends, sidechain inputs, automation and MIDI targets, controller sources).
void swapTracks (Song& song, int a, int b);

// Moves the selected inserts left (delta < 0) or right by one place as a block; the Master never moves.
// `selection` is updated. Returns false when nothing moved.
bool moveTracks (Song& song, std::vector<int>& selection, int delta);

// Inserts a blank insert at `at` (1..size; FL's "Insert one"): the inserts from `at` on move one place right,
// everything that refers to them follows, and default names ("Insert n") follow their new number.
// False when the mixer is full or `at` is out of range.
bool insertTrack (Song& song, int at);

// Deletes insert `t` (FL's "Delete"): the following inserts move one place left and a blank insert takes the
// last place, so the count stays. Channels on it go to the Master, sends to it and sidechains from it are
// removed. False for the Master or out of range.
bool deleteTrack (Song& song, int t);

// Routes `channels` to insert `track` (all of them), or, with `startingHere`, to consecutive inserts from
// `track` on (adding inserts as needed, up to maxMixerTracks). Returns the insert of each channel.
std::vector<int> linkChannels (Song& song, const std::vector<int>& channels, int track, bool startingHere);

// Channels whose output goes to `track`.
std::vector<int> channelsOn (const Song& song, int track);

// Makes sure insert `track` exists, adding default inserts ("Insert n") up to it — like FL, a channel can be routed to
// any insert, not only to those the mixer shows yet (round 162, issue #9). Returns `track` clamped to 0..maxMixerTracks-1.
int ensureInsert (Song& song, int track);
// Routes channel `channel` to insert `track` (0 = Master), adding the inserts up to it when needed.
void routeChannel (Song& song, int channel, int track);

// Sends each source insert to `to` (0 = Master). `only`: the source stops sending anywhere else. Sources
// that would make a cycle are skipped. Returns how many were routed.
int routeTo (Song& song, const std::vector<int>& sources, int to, bool only);

// Sidechain link: a level-0 send from `from` to `to` (so `from` is processed first) and every compressor
// or vocoder on `to` without a side input listens to `from`. Returns how many effects now listen.
int sidechainTo (Song& song, int from, int to);

// Moves effect slot `from` to position `to` of the same insert (the others shift); automations follow.
bool moveEffectSlot (Song& song, int track, int from, int to);

// --- FL's mixer track menu (REA round 143) ---------------------------------------------------------------
// "Reset selected track(s) to default": level, pan, stereo separation, phase, swap and EQ back to the defaults,
// mute and solo off (name, colour, effects and routing stay).
void resetToDefault (Song& song, const std::vector<int>& tracks);
// "Reset selected track(s) routing": each insert sends only to the Master again, at full level.
void resetRouting (Song& song, const std::vector<int>& tracks);
// "Unroute tracks routed to the selected track(s)": the sends into them are removed (an insert left with no
// output goes to the Master). Returns how many sends were removed.
int unrouteInto (Song& song, const std::vector<int>& tracks);
// "Select › Tracks this one is routed to" / "Tracks routed to this one" (0 = Master counts as a destination).
std::vector<int> destinationsOf (const Song& song, int track);
std::vector<int> sourcesOf (const Song& song, int track);
// "Insert multiple...": `count` blank inserts at `at`; returns how many were inserted (the mixer may fill up).
int insertTracks (Song& song, int at, int count);

// --- FL's mixer groups and docking (REA round 146a) -----------------------------------------------------
// The order the strips are shown in: the tracks docked left, then the middle ones, then those docked right (each block
// by number).
std::vector<int> displayOrder (const Song& song);
// The group of `track`: the run of tracks around it, in display order and in its dock block, from the nearest track with
// a separator (at or before it) up to the next separator. Inserts only (the Master is never part of a group).
std::vector<int> groupOf (const Song& song, int track);
// "Group › Create group": a separator on the first track of each run of `tracks` and on the track after it, so the
// selection becomes a group; the tracks without a colour take the colour of `current` (FL's FUN_012aa200).
void createGroup (Song& song, const std::vector<int>& tracks, int current);
// "Group › Auto color group": the tracks of `track`'s group take its colour. Returns how many changed.
int autoColorGroup (Song& song, int track);
// "Group › Separator" and "Layout › Dock to" for the selected tracks (inserts; the Master can be docked).
void setSeparator (Song& song, const std::vector<int>& tracks, bool on);
void dockTracks (Song& song, const std::vector<int>& tracks, MixerDock dock);

// All effects of the insert on or off (the "effects" switch).
void setAllEffectsEnabled (Song& song, int track, bool enabled);
bool anyEffectEnabled (const Song& song, int track) noexcept;
} // namespace lynx::mixerops
