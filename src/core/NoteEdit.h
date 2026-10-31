#pragma once

// Note editing operations used by the Piano Roll (selection, copy/paste,
// quantize...). They work with indices into Pattern::notes. No JUCE, testable.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx::noteedit
{
// Sorts items with a `start` field (notes, clips) and updates `tracked` (indices) to the new positions.
template <typename T>
void sortByStartTracking (std::vector<T>& items, std::vector<int>& tracked)
{
    std::vector<int> order (items.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = (int) i;

    std::stable_sort (order.begin(), order.end(),
                      [&] (int a, int b) { return items[(std::size_t) a].start < items[(std::size_t) b].start; });

    std::vector<int> newIndex (order.size());
    std::vector<T> sorted;
    sorted.reserve (order.size());

    for (std::size_t j = 0; j < order.size(); ++j)
    {
        newIndex[(std::size_t) order[j]] = (int) j;
        sorted.push_back (items[(std::size_t) order[j]]);
    }

    items = std::move (sorted);

    for (auto& i : tracked)
        if (i >= 0 && i < (int) newIndex.size())
            i = newIndex[(std::size_t) i];
}

// Sorts the notes by start and updates `tracked` (indices) to the new positions.
void sortTracking (Pattern& pattern, std::vector<int>& tracked);

// Removes the notes at the given indices (in any order, duplicates ignored).
void remove (Pattern& pattern, std::vector<int> indices);

// Copies of the notes at the given indices, in start order.
std::vector<Note> copy (const Pattern& pattern, const std::vector<int>& indices);

// Appends `notes` shifted by `offset` ticks; channel >= 0 forces the channel.
// Returns the indices of the new notes (the vector is not re-sorted).
std::vector<int> paste (Pattern& pattern, const std::vector<Note>& notes, std::int64_t offset, int channel = -1);

// Start of the first note and end of the last (0, 0 if empty).
void span (const std::vector<Note>& notes, std::int64_t& start, std::int64_t& end);

// Duplicates the notes right after themselves (like FL's Ctrl+B): the offset is the
// span length rounded up to a multiple of `grid`. Returns the new indices.
std::vector<int> duplicate (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid);

// Rounds the notes' start to the nearest multiple of `grid` (length kept).
void quantize (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid);

// Rounds start and end to the nearest multiple of `grid` (the length stays at least one grid unit).
void quantizeFull (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid);

// Moves the notes by (deltaTicks, deltaKeys), clamping so none leaves [0, ...) and [0, 127].
// Returns the offset actually applied.
void move (Pattern& pattern, const std::vector<int>& indices, std::int64_t& deltaTicks, int& deltaKeys);

// --- quick note operations (each is one undo step in the Piano Roll) -----------------

// Splits note `index` at `tick` (absolute) when the tick is strictly inside it. The original keeps the
// head; the tail is appended. Returns the tail's index, or -1 when nothing was split.
int split (Pattern& pattern, int index, std::int64_t tick);

// Chop to grid: cuts every note at each multiple of `grid` that falls inside it. Returns the indices of
// all the resulting pieces (originals included; the vector is not re-sorted).
std::vector<int> chop (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid);

// Join: merges notes of the same channel and key that touch (end == next start) or overlap into one
// note (the first one's properties are kept). Returns the number of notes removed.
int join (Pattern& pattern, const std::vector<int>& indices);

// Legato: each note (per channel) lasts until the next later start among the given notes; notes that
// start together (a chord) end together. The last ones keep their length.
void legato (Pattern& pattern, const std::vector<int>& indices);

// Shrink to grid: every length becomes one grid unit.
void shrinkToGrid (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid);

// Rotate within the bar: moves the notes `steps` grid units (negative = left), wrapping inside the
// whole bars the notes start in (`bar` = ticks per bar).
void rotate (Pattern& pattern, const std::vector<int>& indices, std::int64_t grid, std::int64_t bar, int steps);

// --- grid --------------------------------------------------------------------------

// The multiple of `step` at or before `tick` (step <= 1: whole ticks).
std::int64_t snapDown (double tick, std::int64_t step);

// The candidate closest to `tick` when it lies within `maxDistance` ticks; otherwise snapDown (tick, fallbackStep).
std::int64_t snapNearest (double tick, const std::vector<std::int64_t>& candidates, double maxDistance, std::int64_t fallbackStep);

// The finest of the usual grid steps (1/4 step, 1/2 step, step, beat, bar, then whole multiples of bars)
// whose width on screen is at least `minPixels`, i.e. the lines the editor draws ("Screen" grid).
std::int64_t screenStep (double pixelsPerTick, std::int64_t bar, double minPixels = 16.0);

// "bar:step:tick" (bar and step from 1, tick from 0; 16 steps per 4/4 bar) <-> ticks.
std::string formatTime (std::int64_t ticks, int beatsPerBar, bool isLength = false); // lengths count bars and steps from 0
bool parseTime (const std::string& text, int beatsPerBar, std::int64_t& ticks, bool isLength = false);

// --- Playlist clips ------------------------------------------------------------------

// Moves the clip's left edge to `newStart` without moving its content (the offset follows). The start
// cannot go before the content's beginning (offset 0) nor past end - minLength.
void trimStart (Clip& clip, std::int64_t newStart, std::int64_t minLength);

// Slip: moves the content under the clip by `delta` ticks (positive = later); the clip stays put.
// The offset never goes below 0.
void slip (Clip& clip, std::int64_t delta);

// Inserts `length` ticks of silence at `at`: clips starting at or after it (and markers) move right.
void insertTime (std::vector<Clip>& clips, std::vector<Marker>& markers, std::int64_t at, std::int64_t length);

// Removes [from, to) from the timeline: what lies inside is deleted, clips crossing an edge are cut,
// and everything after `to` (clips and markers) moves left.
void deleteTime (std::vector<Clip>& clips, std::vector<Marker>& markers, std::int64_t from, std::int64_t to);

// Indices of the clips that overlap [from, to).
std::vector<int> clipsInTime (const std::vector<Clip>& clips, std::int64_t from, std::int64_t to);

// Repeats the time selection [from, to) right after itself: copies of `sources` (or, if empty, of the
// clips that start inside it) shifted by its length. Returns the new indices.
std::vector<int> repeatTime (std::vector<Clip>& clips, std::int64_t from, std::int64_t to, std::vector<int> sources);

// --- ripple editing and time-selection moves (docs/WORKFLOW_FL.md P2) -------------------------

// Ripple delete: removes the clips and closes each gap: on the same track, the clips after a removed
// clip move left by its length.
void rippleDelete (std::vector<Clip>& clips, std::vector<int> indices);

// Ripple resize: clip `index` gets `newLength` (at least 1) and every later clip on its track moves by the
// change, so nothing after it overlaps or opens a gap.
void rippleResize (std::vector<Clip>& clips, int index, std::int64_t newLength);

// Moves (or copies) what lies inside the time selection [from, to) by `delta` ticks: clips crossing its
// edges are cut there first, so only the part inside moves. Returns the indices of the moved clips (or
// of the copies). The start never goes below 0.
std::vector<int> moveTimeRange (std::vector<Clip>& clips, std::int64_t from, std::int64_t to, std::int64_t delta, bool copy);

// --- Playlist tracks and clips (docs/WORKFLOW_FL.md P2: clip and track menus) ------------------

// Cuts clip `index` at every multiple of `grid` (song time) inside it. Returns the indices of all pieces.
std::vector<int> chopClip (std::vector<Clip>& clips, int index, std::int64_t grid);

// Inserts an empty track at `track` (the clips there and below move down); every arrangement follows.
void insertTrack (Song& song, int track);
// Removes track `track` and its clips (the tracks below move up), in every arrangement.
void removeTrack (Song& song, int track);
// Swaps two tracks (their clips and headers), in every arrangement.
void swapTracks (Song& song, int a, int b);
// Solo: `track` plays alone (every other track muted); on the only unmuted track it unmutes all.
void soloTrack (Song& song, int track, int trackCount);

// --- Playlist track groups (FL's "Group with above track", REA round 104) -----------------------------
// A track with `grouped` set belongs to the group of the nearest ungrouped track above it: the header.
bool isGroupHeader (const Song& song, int track);            // the next track is grouped with it
int groupHeaderOf (const Song& song, int track);             // the header of the track's group (itself for a header), -1 if none
std::vector<int> groupMembers (const Song& song, int header); // the grouped tracks under a header
bool isFoldedAway (const Song& song, int track);             // a member of a collapsed group (drawn as a thin row)
void groupWithAbove (Song& song, int track);                 // no-op on track 0
void ungroupTrack (Song& song, int track);                   // a member leaves its group (headers stay)
void setGroupCollapsed (Song& song, int track, bool collapsed); // the group of `track`
void colourGroup (Song& song, int track);                    // "Auto color group": members take the header's colour

// --- Piano roll note template and Alt+wheel (round 166e, FL 2026 measured in docs/analise/REA_rodada166e.md) --------
// FL: clicking a note (or Alt+wheel-editing the nearest one) makes its properties the ones new notes get. Velocity and
// pan were measured; release, fine pitch and mod X / Y travel with them (the length already did: lastLength).
struct NoteTemplate
{
    float velocity = Note {}.velocity, pan = Note {}.pan, release = Note {}.release, finePitch = Note {}.finePitch;
    float modX = Note {}.modX, modY = Note {}.modY;

    void takeFrom (const Note& n) noexcept
    {
        velocity = n.velocity;
        pan = n.pan;
        release = n.release;
        finePitch = n.finePitch;
        modX = n.modX;
        modY = n.modY;
    }
    void applyTo (Note& n) const noexcept
    {
        n.velocity = velocity;
        n.pan = pan;
        n.release = release;
        n.finePitch = finePitch;
        n.modX = modX;
        n.modY = modY;
    }
};

// FL's Alt+wheel with nothing selected: the note of `candidates` nearest to the mouse at (tick, key), measured in
// pixels to the note's rectangle (`pixelsPerTick` across, `rowHeight` per key); −1 when there are none.
inline int nearestNote (const std::vector<Note>& notes, const std::vector<int>& candidates, double tick, int key, double pixelsPerTick,
                        double rowHeight)
{
    int best = -1;
    double bestDistance = 0.0;
    for (auto i : candidates)
    {
        if (i < 0 || i >= (int) notes.size())
            continue;
        const auto& n = notes[(std::size_t) i];
        const auto dx = std::max ({ 0.0, (double) n.start - tick, tick - (double) (n.start + n.length) }) * pixelsPerTick;
        const auto dy = (double) std::abs (n.key - key) * rowHeight;
        const auto d = dx * dx + dy * dy;
        if (best < 0 || d < bestDistance)
        {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}
} // namespace lynx::noteedit
