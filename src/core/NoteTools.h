#pragma once

// Piano Roll note tools (docs/WORKFLOW_FL.md P2: strum, arpeggiate, flip, randomise, scale levels, limit,
// scale snap) and the per-note "levels" shared by the Piano Roll lane and the step sequencer graph lanes.
// LYNX's own designs of the behaviours the manual describes; pure functions over Pattern, no JUCE.

#include <cstdint>
#include <array>
#include <vector>

#include "core/Song.h"

#ifndef TR_NOOP
 #define TR_NOOP(text) text
#endif

namespace lynx::notetools
{
// --- per-note levels, normalised to 0..1 ------------------------------------------------------
enum class Level
{
    velocity,  // 0..1
    pan,       // -1..1 -> 0..1
    release,   // 0..1
    finePitch, // -120..120 cents -> 0..1
    modX,      // 0..1
    modY       // 0..1
};

constexpr int levelCount = 6;
const char* levelName (Level level); // English, for tr()
float levelValue (const Note& note, Level level) noexcept;
void setLevelValue (Note& note, Level level, float normalised) noexcept;
float levelDefault (Level level) noexcept; // normalised value of a fresh note
bool levelIsBipolar (Level level) noexcept; // drawn from the centre line (pan, fine pitch)

// --- scales -----------------------------------------------------------------------------------
struct ScaleInfo
{
    const char* name; // English, for tr()
    std::vector<int> intervals;
};

const std::vector<ScaleInfo>& scales(); // [0] = no scale
bool inScale (int key, int root, int scale) noexcept;
// The in-scale key nearest to `key` (a tie goes down); `key` itself without a scale. Stays in 0..127.
int snapToScale (int key, int root, int scale) noexcept;
// Moves every note to the nearest in-scale key.
void snapNotesToScale (Pattern& pattern, const std::vector<int>& indices, int root, int scale);

// The Piano Roll's key rows, top to bottom. "Fold to scale" (round 166d, issue #14 — LYNX's own: FL 2026 only
// highlights the scale, its Piano Roll menus have no fold; like Ableton's Scale fold) keeps only the in-scale keys.
struct KeyRows
{
    std::array<int, 128> keyOfRow {}; // the first `count` rows
    std::array<int, 128> rowOfKey {}; // −1 = folded away
    int count = 128;
    bool shown (int key) const noexcept { return key >= 0 && key < 128 && rowOfKey[(std::size_t) key] >= 0; }
    // the key `rows` rows above (+) or below (−) `key` (the nearest shown key for a folded one), clamped to the rows
    int step (int key, int rows) const noexcept;
};
KeyRows keyRows (int root, int scale, bool fold) noexcept;

// --- chords -----------------------------------------------------------------------------------
// Groups the notes into chords: same channel and same start. Each group is sorted by key (low first);
// the groups are in start order.
std::vector<std::vector<int>> chordsOf (const Pattern& pattern, const std::vector<int>& indices);

// --- strum ------------------------------------------------------------------------------------
struct StrumOptions
{
    std::int64_t spread = 40;   // ticks from one chord note to the next
    float curve = 0.0f;         // -1..1: < 0 the strum speeds up, > 0 it slows down
    float velocityStep = 0.0f;  // added to each successive note's velocity (-0.25..0.25)
    bool downward = false;      // high note first
    bool alternate = false;     // every other chord strums the other way
    bool keepEnds = true;       // notes keep their end (they get shorter); otherwise they keep their length
};

void strum (Pattern& pattern, const std::vector<int>& indices, const StrumOptions& options);

// --- arpeggiate (offline) ---------------------------------------------------------------------
struct ArpeggiateOptions
{
    enum Direction { up, down, upDown, downUp };
    std::int64_t step = ticksPerStep; // time between arpeggio notes
    float gate = 0.8f;                // fraction of the step each note sounds (0.05..1)
    int direction = up;
    int octaves = 1;                  // 1..4
};

// Replaces each chord (and, with more than one octave, each single note) by a run of notes over its length.
// Returns the indices of the new notes (not re-sorted).
std::vector<int> arpeggiate (Pattern& pattern, const std::vector<int>& indices, const ArpeggiateOptions& options);

// --- flip -------------------------------------------------------------------------------------
// Horizontal: mirrors the notes in time inside their span. Vertical: mirrors the keys around the middle
// of their range.
void flip (Pattern& pattern, const std::vector<int>& indices, bool horizontal, bool vertical);

// --- randomise --------------------------------------------------------------------------------
struct RandomiseOptions
{
    float velocity = 0.25f;   // maximum change (0..1 of the full range)
    float pan = 0.0f;
    float finePitch = 0.0f;   // fraction of ±120 cents
    float modX = 0.0f, modY = 0.0f;
    std::int64_t timing = 0;  // maximum start shift in ticks (never before 0)
    float length = 0.0f;      // maximum relative length change (0..1)
    bool bipolar = true;      // changes go both ways; otherwise only down (velocity) / only later (timing)
    std::uint32_t seed = 1;
};

void randomise (Pattern& pattern, const std::vector<int>& indices, const RandomiseOptions& options);

// --- scale levels -----------------------------------------------------------------------------
// Each value becomes value * scale + offset (pan and fine pitch around their centre), where `scale` ramps
// from `scaleStart` (first note) to `scaleEnd` (last note) through time: a fade in or out.
void scaleLevels (Pattern& pattern, const std::vector<int>& indices, Level level, float scaleStart, float scaleEnd, float offset);

// --- limit ------------------------------------------------------------------------------------
struct LimitOptions
{
    int lowKey = 36, highKey = 84;   // notes outside move by octaves into the range (or are clamped if it is narrower)
    float minVelocity = 0.0f, maxVelocity = 1.0f;
};

void limit (Pattern& pattern, const std::vector<int>& indices, const LimitOptions& options);

// --- LYNX's own versions of the tools FL ships as piano roll scripts (REA round 143: Tools › Scripts) -----------

// Euclidean rhythm: `pulses` hits spread as evenly as possible over `steps` steps of `stepTicks` (rotated by
// `rotation` steps), repeated for `length` ticks from `start`, on `key` of `channel`. True at a step that hits.
bool euclideanHit (int steps, int pulses, int rotation, int step) noexcept;
struct EuclideanOptions
{
    int steps = 16, pulses = 5, rotation = 0;
    std::int64_t stepTicks = ticksPerStep;
    std::int64_t start = 0, length = 4 * 4 * ticksPerStep;
    int channel = 0, key = 60;
    float velocity = 0.78f;
    float gate = 1.0f;     // note length as a fraction of a step
    float accent = 0.0f;   // extra velocity on the first hit of each cycle
};
// Removes `replace` (note indices) and writes the rhythm; returns the new notes' indices.
std::vector<int> euclidean (Pattern& pattern, const std::vector<int>& replace, const EuclideanOptions& options);

// Humanize: small, musical irregularities. Timing moves each start by up to `timing` ticks (both ways, never
// before 0), with notes of one chord moving together; velocity varies by up to `velocity` (the off-beat notes a
// bit softer when `softerOffbeats`); length by up to `length` (relative).
struct HumanizeOptions
{
    std::int64_t timing = 6;
    float velocity = 0.08f;
    float length = 0.05f;
    bool softerOffbeats = true;
    std::uint32_t seed = 1;
};
void humanize (Pattern& pattern, const std::vector<int>& indices, const HumanizeOptions& options);

// Note repeater: each note becomes repeats every `rate` ticks over its length (gate = repeat length / rate),
// the velocity multiplied by `decay` at each repeat. Returns the indices of all the resulting notes.
struct RepeatOptions
{
    std::int64_t rate = ticksPerStep;
    float gate = 0.5f;
    float decay = 0.9f;
    int pitchStep = 0; // semitones added at each repeat (0 = same key)
};
std::vector<int> repeatNotes (Pattern& pattern, const std::vector<int>& indices, const RepeatOptions& options);

// Select by velocity: the notes of `indices` whose velocity is within [low, high].
std::vector<int> selectByVelocity (const Pattern& pattern, const std::vector<int>& indices, float low, float high);
// The loudest / quietest `fraction` (0..1) of `indices` (at least one note when there are any).
std::vector<int> selectLoudest (const Pattern& pattern, const std::vector<int>& indices, float fraction, bool loudest);

// --- selection helpers (FL's piano roll Select menu, behaviour from its manual; REA round 153) ------------------
// `all` minus `selected` (both index lists; the result is sorted).
std::vector<int> invertSelection (const std::vector<int>& all, const std::vector<int>& selected);
// The notes of `indices` in colour group `colour` (0..15).
std::vector<int> selectByColour (const Pattern& pattern, const std::vector<int>& indices, int colour);
// Every other note in time order (start, then key): the 1st, 3rd, 5th ...
std::vector<int> selectOdd (const Pattern& pattern, const std::vector<int>& indices);
// About `fraction` (0..1) of `indices` chosen at random (each note independently; deterministic for a seed).
std::vector<int> selectRandom (const Pattern& pattern, const std::vector<int>& indices, float fraction, std::uint32_t seed);
std::vector<int> selectMuted (const Pattern& pattern, const std::vector<int>& indices);
// Notes that overlap an earlier note of the same key (every layer except the first of each overlapping run).
std::vector<int> selectOverlapping (const Pattern& pattern, const std::vector<int>& indices);
// Notes that lie completely inside another note of the same key (exact duplicates: all but the first), so deleting
// them leaves what is heard unchanged.
std::vector<int> selectStacked (const Pattern& pattern, const std::vector<int>& indices);
// Note groups (FL's Group Shift+G / Ungroup Alt+G): grouping gives the notes a new group number; ungrouping clears
// it. groupOf: the notes of the same channel in `index`'s group (just `index` when it has none). regroup: notes just
// pasted get new group numbers (each group of the copies its own), so copies never join the originals' groups.
void groupNotes (Pattern& pattern, const std::vector<int>& indices);
void ungroupNotes (Pattern& pattern, const std::vector<int>& indices);
std::vector<int> groupOf (const Pattern& pattern, int index);
void regroup (Pattern& pattern, const std::vector<int>& indices);

// The General MIDI percussion names (keys 35..81; nullptr elsewhere), for the Piano Roll's key labels.
const char* generalMidiDrumName (int key) noexcept;

// Puts the notes into colour group `colour` (0..15).
void setColour (Pattern& pattern, const std::vector<int>& indices, int colour);
} // namespace lynx::notetools
