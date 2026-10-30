#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace lynx
{
struct Marker;
struct Song;

// What FL Studio's Playlist marker actions do to playback (REA_rodada165f-markers.md), compiled once per song edit
// into a fixed-size table the audio thread can follow without allocating: the position jumps (or playback pauses)
// when it reaches `at`.
struct MarkerJump
{
    enum class Kind : int
    {
        jump = 1,  // the position goes to `to` (Marker loop, Marker skip)
        pause = 2, // playback stops at `at` (Marker pause); Play continues from there
    };

    std::int64_t at = 0;
    Kind kind = Kind::jump;
    std::int64_t to = 0;
};

struct MarkerPlan
{
    static constexpr int maxJumps = 64;
    std::array<MarkerJump, maxJumps> jumps {}; // sorted by `at`
    int count = 0;
    std::int64_t wrapTo = 0;  // where Song mode goes back to at the song's end (FL's "Loop" marker; else 0)
    std::int64_t start = -1;  // FL's "Start" marker: Play from the song's beginning starts here (< 0 = none)
    std::int64_t songEnd = 0; // the song's end (> 0: overrides Song::songLength for the wrap)

    // the first jump in (from, to]: reached by playing from `from` to `to` (nullptr = none)
    const MarkerJump* firstIn (double from, double to) const noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            const auto at = (double) jumps[(std::size_t) i].at;
            if (at > from && at <= to)
                return &jumps[(std::size_t) i];
            if (at > to)
                break;
        }
        return nullptr;
    }
};

MarkerPlan makeMarkerPlan (const Song& song);

// One step of the audio thread's transport (Song mode, playing): how many of the `n` samples to play before a marker
// jump / pause or the user's quantized jump (`pendingAt`, < 0 = none) falls due, and what happens after them.
// Real-time safe (no allocation).
struct MarkerStep
{
    int samples = 0;                    // play this many samples, then:
    const MarkerJump* jump = nullptr;   // this marker jump / pause is due, or
    bool userJump = false;              // the user's quantized jump is due
};
MarkerStep markerStep (const MarkerPlan& plan, double position, int n, double ticksPerSample, double pendingAt) noexcept;

namespace markers
{
// "Quantize song jumps" (FL's Playlist menu): 0 Off, 1 Beats, 2 Bars, 3 2 bars, 4 4 bars, 5 8 bars. The tick where a
// jump asked at `position` happens (`position` itself when off).
std::int64_t quantizedJumpTick (const Song& song, double position, int quantize);

// Gives `marker` FL's action `action` (Marker::none ... Marker::keyScale): "Time signature" makes it a signature change
// (the song's current one at that point), any other action makes it a plain / action marker again.
void setAction (const Song& song, Marker& marker, int action);
// FL's Playlist menu › Time markers:
// "Add one every" N bars: plain markers on every N-th bar line of the time selection (else the whole song), named by
// their bar number; lines that already have a marker are skipped.
void addEvery (Song& song, int bars);
// "Add jump to next bar": a Marker skip at `tick` and a plain marker one bar later — playback skips that bar
// [SHOT fl_add_jump_next_bar.png]
void addJumpToNextBar (Song& song, std::int64_t tick);
// "Change to marker loop": the time selection becomes a marker loop (a Marker loop at its start, a marker at its end)
// and the selection is cleared
void changeToMarkerLoop (Song& song);

// FL's name of an action ("Marker loop"...; English, for tr()), empty for none / unknown
const char* actionName (int action) noexcept;
// the action's short tag on the ruler (FL draws an icon): "" for plain markers
const char* actionTag (int action) noexcept;
} // namespace markers
} // namespace lynx
