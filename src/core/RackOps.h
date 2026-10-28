#pragma once

// Pattern and Channel Rack operations used by the workflow keys and menus (docs/WORKFLOW_FL.md,
// P1 items 4 and 5). Pure functions over the Song model: no JUCE, unit-tested in tests/tests.cpp.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/Song.h"

namespace lynx
{
// LYNX's colour palette for patterns, channels and inserts (0xFFRRGGBB). Original values.
const std::array<std::uint32_t, 12>& lynxPalette();

// A palette colour different from `current`, chosen from `seed` (any integer, e.g. a counter or time).
std::uint32_t randomPaletteColour (std::uint32_t current, std::uint32_t seed);

// --- patterns ----------------------------------------------------------------------------------

// True when the pattern has no notes (its name and colour don't count).
bool isPatternEmpty (const Pattern& pattern) noexcept;

// Index of the next/previous pattern (clamped to the list).
int stepPattern (const Song& song, int delta) noexcept;

// The first empty pattern after `from` (wrapping around, never `from` itself); -1 if there is none.
int findNextEmptyPattern (const Song& song, int from) noexcept;

// Selects the next empty pattern, appending a new one ("Pattern N") if every pattern has notes.
// Returns its index.
int gotoNextEmptyPattern (Song& song);

// Inserts an empty pattern before `index` (Playlist clips keep pointing at the same patterns)
// and selects it. Returns the new index.
int insertPattern (Song& song, int index, const std::string& name);

// Creates one pattern per channel that has notes in pattern `index`, named "<pattern> - <channel>"
// and coloured after the channel, inserted right after it. The original pattern is unchanged.
// Returns how many patterns were created.
int splitPatternByChannel (Song& song, int index);

// Default name for a new pattern: "Pattern N" with the lowest N not in use.
std::string newPatternName (const Song& song);

// Copies pattern `index` (notes, length, colour) right after it, named "<name> #2" (or the next free
// number), and selects it. Playlist clips keep pointing at the same patterns. Returns the new index.
int clonePattern (Song& song, int index);

// Swaps pattern `index` with its neighbour above (delta < 0) or below; clips and the current pattern
// follow. Returns false at the ends.
bool movePattern (Song& song, int index, int delta);

// --- step sequencer (docs/WORKFLOW_FL.md P2: fill, Euclidean fill, rotate, graph lanes) --------------

// Steps the pattern shows in the rack (its length in sixteenths, at least 16).
int patternSteps (const Song& song, const Pattern& pattern) noexcept;

// True when the channel's notes in the pattern cannot be shown as step buttons: a note starts off the step
// grid, lasts longer than a step, sits on another key than the step key (60) or slides/glides. The Channel
// Rack then draws a miniature of the notes instead of the steps (as FL does once a channel has piano-roll
// notes) and a click opens the Piano Roll. LYNX's own rule (FL's exact test is not confirmed).
bool showsNotePreview (const Pattern& pattern, int channel) noexcept;
// The Channel Rack's piano-roll view (FL's "step sequencer / piano roll overview" switch): every channel row shows
// its notes as a miniature, step patterns included; otherwise only the rows with piano-roll notes do.
bool showsNotePreview (const Pattern& pattern, int channel, bool pianoRollView) noexcept;

// The key range of the channel's notes in the pattern (lowest, highest); {60, 60} without notes.
std::pair<int, int> channelKeyRange (const Pattern& pattern, int channel) noexcept;

// An Euclidean rhythm: `pulses` onsets spread as evenly as possible over `steps`, starting on the first
// step, then rotated right by `rotation` steps. pulses is clamped to 0..steps.
std::vector<bool> euclideanRhythm (int pulses, int steps, int rotation = 0);

// Turns on every `every`-th step of `channel` (from `offset`) over `steps` steps; existing notes stay.
void fillEverySteps (Pattern& pattern, int channel, int every, int steps, int offset = 0);

// Replaces the channel's step notes (notes starting exactly on a step) inside the first `steps` steps by
// `rhythm`, repeated to fill them. Notes that are off the step grid are left alone.
void setStepRhythm (Pattern& pattern, int channel, const std::vector<bool>& rhythm, int steps);

// FL's ghost steps (issue #11): a step turned on at `step` (0-based, `ticksPerStep` long) after the end of a pattern
// with a set length makes the pattern long enough for it, rounded up to whole bars of `barTicks`. An automatic length
// (0) follows the notes by itself and stays 0.
std::int64_t lengthWithStep (std::int64_t lengthTicks, int step, std::int64_t ticksPerStep, std::int64_t barTicks) noexcept;

// Moves every note of `channel` by `delta` steps, wrapping inside the first `steps` steps.
void rotateSteps (Pattern& pattern, int channel, int delta, int steps);

// True when the pattern has notes the step sequencer can't show as steps (off the step grid, another key
// or a held length): the Playlist then opens it in the Piano Roll.
bool hasPianoRollNotes (const Pattern& pattern) noexcept;

// Indices of the channel's notes that start inside step `step` (the cell the rack draws).
std::vector<int> notesInStep (const Pattern& pattern, int channel, int step);

// --- channel organisation ----------------------------------------------------------------------

enum class ChannelSort { name, colour, insert, type };

// The order (newOrder for permuteChannels) that sorts the channels; stable, so equal keys keep their order.
std::vector<int> channelSortOrder (const Song& song, ChannelSort by);

// Channels no pattern plays and no Playlist clip uses (and that no automation reads as a live source).
std::vector<int> unusedChannels (const Song& song);

// --- channel selection -------------------------------------------------------------------------

// Multi-selection of Channel Rack rows: always sorted, unique and inside [0, count).
struct ChannelSelection
{
    std::vector<int> rows;
    int anchor = 0; // start of Shift+click ranges

    bool contains (int row) const noexcept;
    void selectOnly (int row);
    void toggle (int row);         // Ctrl+click / right-click on the selector
    void extendTo (int row);       // Shift+click: anchor..row
    void selectAll (int count);
    void clamp (int count);        // after channels were removed
};

// --- channel operations ------------------------------------------------------------------------

// Reorders the channels: newOrder[i] = old index of the channel that ends up at i (a permutation).
// Notes, audio clips, automation/MIDI targets and controller sources follow their channel.
void permuteChannels (Song& song, const std::vector<int>& newOrder);

// Moves the selected channels up (delta < 0) or down by one row, as a block that stops at the
// edges. `selection` is updated to the new rows. Returns false when nothing moved.
bool moveChannels (Song& song, std::vector<int>& selection, int delta);

// Copies channel `index` (settings, not notes) to a new channel right below it, named "<name> #2"
// (or the next free number). Returns the new index.
int cloneChannel (Song& song, int index);

// Inserts `channel` at `index` (shifting the others down). Returns the index.
int insertChannel (Song& song, int index, ChannelDef channel);

// Removes several channels (any order). Returns how many were removed.
int removeChannels (Song& song, std::vector<int> channels);

// An insert nobody uses yet: no channel routes to it, it has no effects and no insert sends into it.
bool isInsertFree (const Song& song, int track) noexcept;

// Routes each channel to its own free insert (adding inserts at the end if needed, up to
// maxMixerTracks), named and coloured after the channel. Returns the insert of each channel
// (0 = could not get one).
std::vector<int> sendChannelsToFreeInserts (Song& song, const std::vector<int>& channels);

// --- Channel settings "Misc" tab (REA round 143b; FL's channel window: Levels, Polyphony, Time, Group, Arpeggiator) --
// Which of the per-channel functions a channel's instrument has: Mono (one voice, the new note takes over) and
// Portamento (glide from the previous note) exist for LYNX's synth and sampler; Max polyphony, swing mix, cut groups
// and the arpeggiator exist for every channel.
bool channelHasMono (const ChannelDef& c) noexcept;
bool channelHasPortamento (const ChannelDef& c) noexcept;
bool channelMono (const ChannelDef& c) noexcept;
void setChannelMono (ChannelDef& c, bool on) noexcept;
bool channelPortamento (const ChannelDef& c) noexcept;
float channelPortamentoBeats (const ChannelDef& c) noexcept;
void setChannelPortamento (ChannelDef& c, bool on, float beats) noexcept;
// FL's "Cut self" (the channel's cut group and "cut by" are its own): LYNX gives it the group 1000 + channel.
bool channelCutsItself (const ChannelDef& c) noexcept;
void setChannelCutsItself (ChannelDef& c, int channel, bool on) noexcept;
// The cut-group number as the Misc tab shows it: "--" for none, "self" for the channel's own group, else the number.
std::string cutGroupText (int group, int channel);

// FL's hint bar over a step shows its song position as bar:step:tick ("1:02:00"): bar from 1, step (a 16th) from 1
// with two digits, the tick inside the step (in FL's 96-per-quarter units: 24 per step) with two digits.
std::string stepPositionText (std::int64_t tick, int beatsPerBar);
} // namespace lynx
