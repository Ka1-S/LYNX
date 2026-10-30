#pragma once

// Converts a pattern's notes into events with an exact sample offset within
// an audio block. Runs on the audio thread: no allocation, no locks.

#include <array>
#include <cstdint>
#include <vector>

#include "core/Song.h"

namespace lynx
{
// Audio clip events carry the clip's own "key" (above the MIDI range): the end of a clip stops only the voice that clip
// started. Two clips of the same channel overlapping (FL's crossfaded vocal comps) both play, as in FL (REA_rodada107 §5).
constexpr int audioClipKeyBase = 128;
constexpr int audioClipKey (std::size_t playlistIndex) noexcept { return audioClipKeyBase + (int) (playlistIndex % 1000000u); }

struct NoteEvent
{
    enum class Type : std::uint8_t { on, off, slide, choke, control, voiceProp }; // slide: glides the sounding notes to `key`; choke: cut group
    // voiceProp (round 165c): a VFX's voice property changing while its note sounds (on a port: `port`, `colour`),
    // `key` the note's key, `prop` the vfx::Prop, `velocity` the value
    // control (live MIDI input only, round 162): `key` = CC number 0…127, controlPitchBend or controlPressure;
    // `velocity` = the value, 0..1 (the pitch wheel: -1..1, 0 = centre)
    // controlProgram (round 165c): a program change, `velocity` = program / 127
    static constexpr int controlPitchBend = 128, controlPressure = 129, controlProgram = 130;

    int sampleOffset = 0;   // 0 .. numSamples-1 within the block
    Type type = Type::on;
    int channel = 0;
    int key = 60;           // audio clips: audioClipKey (the clip's playlist index), so a clip's end stops only its voice
    float velocity = 0.0f;
    double startSeconds = 0.0; // audio clips: where the sample starts playing from
    float pan = 0.0f;          // -1..1, from the note
    float cents = 0.0f;        // note's fine pitch
    float slideSamples = 0.0f; // slide: glide duration
    bool porta = false;        // the note inverts the channel's porta
    // audio clips: fades (in samples) and the position within the clip when the note starts
    float clipElapsed = 0.0f, clipRemaining = 0.0f, fadeIn = 0.0f, fadeOut = 0.0f;
    // the note's Mod X / Y (0..1, 0.5 = none) and, on its "off", its release velocity (0..1) — REA round 151
    float modX = 0.5f, modY = 0.5f, release = 0.5f;
    // the note's level factor (a Layer's VOL and crossfade, round 164): the instrument scales the voice when it can,
    // else the velocity is scaled
    float gain = 1.0f;
    std::uint8_t prop = 0;  // voiceProp: the vfx::Prop
    std::int8_t colour = 0; // the note's colour group 0…15 (FL's note colours: VFX Color Mapper, round 164b)
    // A VFX channel (round 164b): −1 = an ordinary note; −2 = one its processor takes (it goes nowhere else); N ≥ 0 =
    // what the processor sent on MIDI port N (only the routes on that port take it)
    std::int16_t port = -1;
};

// Fixed-capacity buffer. Events beyond capacity are dropped
// (and counted), never allocate.
class EventBuffer
{
public:
    static constexpr int capacity = 2048;

    void clear() noexcept { count = 0; dropped = 0; }
    void add (const NoteEvent& e) noexcept;
    void sortByOffset() noexcept;

    int size() const noexcept { return count; }
    int droppedCount() const noexcept { return dropped; }
    const NoteEvent& operator[] (int i) const noexcept { return events[(std::size_t) i]; }
    NoteEvent& at (int i) noexcept { return events[(std::size_t) i]; }
    const NoteEvent* begin() const noexcept { return events.data(); }
    const NoteEvent* end() const noexcept { return events.data() + count; }

private:
    std::array<NoteEvent, capacity> events {};
    int count = 0;
    int dropped = 0;
};

inline double ticksPerSample (double bpm, double sampleRate) noexcept
{
    return bpm * ppq / (60.0 * sampleRate);
}

// Generates the pattern's events (playing in a loop) in the interval [startTick, startTick + numSamples * tps).
// A note starting at t generates "on" at t and "off" at t + length.
void schedulePatternLoop (const Pattern& pattern,
                          std::int64_t patternLength,
                          double startTick,
                          int numSamples,
                          double ticksPerSampleValue,
                          EventBuffer& out,
                          float swing = 0.0f,
                          const std::vector<ChannelDef>* channels = nullptr) noexcept;
// The range the transport loops over: in Song mode the Playlist's loop region, or else the whole song (FL plays the
// song in a loop: at its end the position goes back to the start, issue #4). Pattern mode: none (end <= start); the
// pattern loop is the position modulo the pattern length.
struct PlaybackLoop
{
    double start = 0.0, end = 0.0;
    bool active() const noexcept { return end > start; }
};
// (songStart > 0: Performance mode, the song loops back to its Start marker)
PlaybackLoop playbackLoop (bool songMode, double loopStart, double loopEnd, double songLength, double songStart = 0.0) noexcept;

// The position hosted plugins see (their play head): within the pattern in Pattern mode, within the song in Song
// mode, so plugins synced to the host follow the loop like in FL instead of counting on forever.
double hostPositionTick (const Song& song, double tick, std::int64_t songLength) noexcept; // songLength = song.songLength()

// Index built once per snapshot so scheduleSong doesn't scan every note on every block:
// song length and the longest note of each pattern. Requires notes sorted by start.
struct ScheduleIndex
{
    std::int64_t songLength = 0;
    std::vector<std::int64_t> maxNoteLength; // per pattern
    std::int64_t maxShift = 0;               // the largest channel time shift (FL's Misc › Shift), ticks
};

// When a note plays (ticks within its pattern) and for how long: the swing (with the channel's swing mix), FL's
// Misc › Time › Shift and Gate (REA round 146b). c = the note's channel (nullptr: only the swing).
struct PlayedTiming
{
    std::int64_t start = 0, length = 1;
};
PlayedTiming playedTiming (const Note& note, float swing, const ChannelDef* c) noexcept;

// The largest Misc › Shift of the channels, in ticks.
std::int64_t maxChannelShift (const std::vector<ChannelDef>& channels) noexcept;

ScheduleIndex makeScheduleIndex (const Song& song);

// Generates the Playlist's events (playing the whole song in a loop). Each pattern clip
// plays the pattern's notes that fall in [offset, offset + length); notes are cut at the
// end of the clip. Audio clips trigger the channel's sample at the start and stop at the end.
// `resume` = playback started or jumped to startTick: audio clips that were already
// in progress at that point start in the middle, on the first sample of the block.
void scheduleSong (const Song& song,
                   double startTick,
                   int numSamples,
                   double ticksPerSampleValue,
                   EventBuffer& out,
                   bool resume = false,
                   const ScheduleIndex* index = nullptr,
                   bool wrap = true) noexcept;
// One clip played once where it is (no song loop), its events in the block (not sorted): Performance mode's passes.
void scheduleSingleClip (const Song& song, const Clip& clip, std::size_t clipIndex, double startTick, int numSamples, double tps,
                         EventBuffer& out, const ScheduleIndex* index = nullptr) noexcept;
// (wrap = false: the song plays once and is silent past its end — audio recording without a loop region, round 163)
} // namespace lynx
