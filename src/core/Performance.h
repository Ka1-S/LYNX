#pragma once

// FL's Performance mode (round 165m, docs/analise/REA_rodada165m.md): the clips placed before the Playlist's "Start"
// marker (the performance zone) are launched live — a click on the clip, a controller pad, a note on the performance
// MIDI channel — and loop on their own track by the track's Performance settings (TrackPerformance in Song.h):
// press (Retrigger, Hold & stop, Hold & motion, Latch), motion when the clip ends (Stay, One shot, March & wrap /
// stay / stop, Random, Exclusive random), trigger sync (when a launch starts: off, 1/4 … 4 beats, auto = the clip's
// length; Queue = after the playing clip; Tolerant = a launch a little late still starts on the grid point) and
// position sync (where in the clip it starts: at its start, or at the song position modulo the sync length).
//
// Threads: the UI (or the MIDI callback) posts commands into Live::commands; the audio thread's Player applies them at
// the start of each block, decides which clip plays where on its own steady clock (the transport's ticks, never
// wrapping) and hands the scheduler one "virtual clip" per playing pass. The Player publishes what plays for the
// Playlist (Live::tracks) and, while recording, the passes that started and ended (Live::recorded).

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/LiveEventQueue.h"
#include "core/Scheduler.h"
#include "core/Song.h"

namespace lynx::perf
{
constexpr int maxTracks = 500; // FL's Playlist tracks

// The sync length in ticks (autoLength → the clip's length; off → 0).
std::int64_t syncTicks (TrackPerformance::Sync sync, std::int64_t clipLength) noexcept;

// The performance zone's clips of a song: playlist indices sorted by (track, start). Built off the audio thread.
std::vector<int> zoneClips (const Song& song);

struct Command
{
    enum class Type : std::uint8_t { press, release, stopTrack, stopAll, launchColumn };
    Type type = Type::press;
    int track = 0;
    std::int64_t clipStart = 0; // identifies the clip on its track (stable across edits that don't move it)
    std::int64_t columnStart = 0, columnEnd = 0; // launchColumn: every zone clip starting in [columnStart, columnEnd)
    int column = -1; // press / release from a controller: the track's column-th zone clip (clipStart unused)
};

// FL's "MIDI channel used for playlist live mode" ([DECOMP] 0x1181be1): on that channel a note below 120 launches
// track (note / 12 + trackOffset), column (note % 12 + columnOffset) — one octave per track, twelve clips each;
// 120 / 121 move the track offset down / up (by 1, or 10), 122 / 123 the column offset (by 1, or 12), 124 switches
// the big steps. False when the note is a navigation key (offsets changed, no command).
struct MidiMapping
{
    int trackOffset = 0, columnOffset = 0;
    bool bigSteps = false;
    bool note (int key, bool on, Command& out) noexcept;
};

// A pass the recorder turns into a clip in the arrangement: zone clip (track, clipStart) played from song tick
// `songTick` for `length` ticks, starting `offset` ticks into the clip (its own offset not included).
struct RecordedPass
{
    int track = 0;
    std::int64_t clipStart = 0;
    double songTick = 0.0, length = 0.0, offset = 0.0;
};

struct Live
{
    SpscQueue<Command, 256> commands;     // UI → audio (one producer: the message thread)
    SpscQueue<Command, 256> midiCommands; // MIDI input thread → audio (controller launching)

    // audio → UI, per Playlist track: the clip playing (its start tick, −1 = none), the one waiting to start, and the
    // playing pass's position within the clip (0..1)
    struct TrackView
    {
        std::atomic<std::int64_t> playing { -1 }, pending { -1 };
        std::atomic<float> progress { 0.0f };
    };
    std::array<TrackView, maxTracks> tracks;
    std::atomic<bool> anyPlaying { false };

    // UI → audio: record the passes (Transport recording with Performance mode on)
    std::atomic<bool> recording { false };
    SpscQueue<RecordedPass, 1024> recorded; // audio → UI
};

// The clip's pass the scheduler plays in this block: a Clip copy placed on the Player's clock.
struct VirtualClip
{
    Clip clip;               // start/length/offset on the Player's clock (offset includes the clip's own offset)
    std::size_t playlistIndex = 0;
};

class Player
{
public:
    static constexpr int maxActive = 64;     // tracks playing at once
    static constexpr int maxVirtual = 4 * maxActive;

    // Audio thread. `clock` = the Player's clock at the block start (ticks, steady), `songTick` = the transport's
    // position at the same sample, `blockTicks` = the block's length in ticks. `zone` = zoneClips (song).
    // Commands are applied, plays advance, and virtualClips() then holds this block's passes.
    void process (const Song& song, const std::vector<int>& zone, Live* live, double clock, double songTick,
                  double blockTicks, bool playing) noexcept;

    // Everything stops (transport stop): plays end at once.
    void stopAll() noexcept;

    int virtualCount() const noexcept { return numVirtual; }
    const VirtualClip& virtualClip (int i) const noexcept { return virtuals[(std::size_t) i]; }

    // the random source for Random / Exclusive random (tests seed it)
    void seed (std::uint32_t s) noexcept { rng = s != 0 ? s : 1u; }

    // test access: what plays on a track now (zone clip start, −1 = none)
    std::int64_t playingOn (int track) const noexcept;

private:
    static constexpr double never = std::numeric_limits<double>::infinity();

    // One pass of a clip: from `from` until `until` on the clock; the clip's position 0 is at `anchor` (+ k·length).
    struct Play
    {
        std::int64_t clipStart = -1;
        double anchor = 0.0, from = 0.0, until = never;
        bool recorded = false;
        double songFrom = 0.0; // recording: the transport position where the current pass began
    };
    struct TrackState
    {
        int track = -1;
        Play current, previous, next; // previous: ended, its last note offs may still fall in this block
        bool held = false;            // hold modes: the trigger is still down
        double motionAt = never;      // when the current play's motion acts (the end of its pass)
    };

    std::array<TrackState, maxActive> active {};
    int numActive = 0;
    std::array<VirtualClip, maxVirtual> virtuals {};
    int numVirtual = 0;
    std::uint32_t rng = 0x9E3779B9u;
    double lastClock = 0.0, lastSong = 0.0;

    TrackState* stateOf (int track, bool create) noexcept;
    void apply (const Song& song, const std::vector<int>& zone, const Command& c, double now, Live* live) noexcept;
    void launch (const Song& song, const std::vector<int>& zone, TrackState& t, std::int64_t clipStart, double now,
                 bool immediate) noexcept;
    void stopTrack (const Song& song, const std::vector<int>& zone, TrackState& t, double now, bool quantized) noexcept;
    void recordPartial (const Song& song, const std::vector<int>& zone, const TrackState& t, const Play& p, double at,
                        Live* live) noexcept;
    void scheduleMotion (const Song& song, const std::vector<int>& zone, TrackState& t) noexcept;
    std::uint32_t nextRandom() noexcept;
};

// Resolves a zone clip (track, start) to its playlist index (−1 = gone).
int findZoneClip (const Song& song, const std::vector<int>& zone, int track, std::int64_t clipStart) noexcept;

// The zone clips of a track, in order (start ticks), into `out` (no allocation past its capacity).
int trackZoneClips (const Song& song, const std::vector<int>& zone, int track, std::int64_t* out, int capacity) noexcept;

// Schedules the Player's passes of this block (after scheduleSong; the block = [clock, clock + numSamples · tps)).
void schedulePerformance (const Song& song, const Player& player, double clock, int numSamples, double tps,
                          EventBuffer& out, const ScheduleIndex* index = nullptr) noexcept;

// Recording: turns a pass into a clip copy for the arrangement (the zone clip moved to the song position).
bool recordedClip (const Song& song, const RecordedPass& pass, Clip& out);
} // namespace lynx::perf
