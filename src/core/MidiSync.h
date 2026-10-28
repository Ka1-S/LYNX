#pragma once

// MIDI synchronisation (REA round 165f, docs/analise/REA_rodada165f-midisync.md): FL Studio's Options › MIDI
// "Send master sync" (MIDI clock + Song Position Pointer, or MIDI Time Code at 24 / 25 / 30 / 29.97 fps, drop or
// non-drop) to an output and "External clock sync" (LYNX follows an incoming MIDI clock: tempo, Start / Continue /
// Stop, Song Position Pointer). Pure logic, no devices: the engine feeds blocks and incoming bytes, tests check it.
// Real-time safe: no allocation, no locks; fixed-capacity outputs.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "core/LiveEventQueue.h"

namespace lynx::midisync
{
// FL's "Synchronization type" list (the order of its combo box, without the separator)
enum class SyncType
{
    midiClock,
    mtc24,
    mtc25,
    mtc30Drop,
    mtc30,
    mtc2997Drop,
    mtc2997
};
constexpr int syncTypeCount = 7;
const char* syncTypeKey (SyncType type) noexcept;     // stable name for the settings file ("clock", "mtc24", …)
SyncType syncTypeFromKey (const std::string& key) noexcept;
const char* syncTypeName (SyncType type) noexcept;    // English UI name, as FL's list

// --- MIDI Time Code ---------------------------------------------------------------------------------------------
struct MtcRate
{
    double fps = 25.0;   // real frames per second (29.97 = 30000 / 1001)
    int nominal = 25;    // frames counted per second in the labels (30 for 29.97)
    bool drop = false;   // drop-frame numbering (labels 0 and 1 skipped each minute except every 10th)
    int code = 1;        // the rate bits in the hours byte: 0 = 24, 1 = 25, 2 = 30 drop, 3 = 30
};
MtcRate mtcRate (SyncType type) noexcept;

struct Timecode
{
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    bool operator== (const Timecode&) const = default;
};
// The label of frame number `frame` (0 = 00:00:00:00), drop-frame numbering when the rate says so.
Timecode frameToTimecode (std::int64_t frame, const MtcRate& rate) noexcept;
// The inverse (for tests and the receiver display).
std::int64_t timecodeToFrame (const Timecode& tc, const MtcRate& rate) noexcept;
// Quarter-frame message piece 0…7 (the data byte after 0xF1) for timecode `tc`.
std::uint8_t quarterFrameData (const Timecode& tc, int piece, int rateCode) noexcept;
// The full-frame SysEx F0 7F 7F 01 01 hh mm ss ff F7 (10 bytes) into `out`.
void fullFrame (const Timecode& tc, int rateCode, std::uint8_t* out) noexcept;

// --- what the generator sends -----------------------------------------------------------------------------------
struct Message
{
    int sample = 0;                 // offset in the block it belongs to
    std::uint8_t size = 0;
    std::array<std::uint8_t, 10> bytes {};
};

template <int Capacity>
struct MessageList
{
    std::array<Message, Capacity> items {};
    int count = 0;
    bool overflow = false;
    void clear() noexcept
    {
        count = 0;
        overflow = false;
    }
    void add (int sample, std::initializer_list<std::uint8_t> b) noexcept
    {
        if (count >= Capacity)
        {
            overflow = true;
            return;
        }
        auto& m = items[(std::size_t) count++];
        m.sample = sample;
        m.size = 0;
        for (auto v : b)
            m.bytes[m.size++] = v;
    }
};
using BlockMessages = MessageList<512>;

// One audio block as the transport plays it.
struct Block
{
    double startTicks = 0.0;      // position at the block's first sample (core ticks, 960 PPQ)
    double ticksPerSample = 0.0;  // the tempo during the block
    double sampleRate = 44100.0;
    int numSamples = 0;
    bool playing = false;
    bool jumped = false;          // the position was moved before this block while playing (also detected: a block
                                  // that doesn't start where the previous one ended)
};

// Song Position Pointer value (MIDI beats = 16th notes) for a tick position; rounded up to the next 16th so the
// first clock after Continue falls exactly on it.
int sppFor (double ticks) noexcept;

// Master sync generator for one output. Clock: 24 pulses per quarter note at the sample where the position crosses
// each pulse, Start at 0 / SPP + Continue elsewhere, Stop when the transport stops, SPP + Continue after a jump.
// MTC: quarter frames (4 per frame, 8 pieces over 2 frames, piece 0 on an even frame) from the song time, a full
// frame on every start and locate.
class Generator
{
public:
    void setType (SyncType t) noexcept;
    SyncType type() const noexcept { return syncType; }
    void reset() noexcept;
    // Appends this block's messages (sample offsets relative to the block). `songSeconds` = the song time of the
    // block's first sample (MTC only; the engine computes it from the position and tempo).
    void process (const Block& block, double songSeconds, BlockMessages& out) noexcept;

private:
    void processClock (const Block& block, BlockMessages& out) noexcept;
    void processMtc (const Block& block, double songSeconds, BlockMessages& out) noexcept;

    SyncType syncType = SyncType::midiClock;
    bool wasPlaying = false;
    double nextPulseTicks = 0.0;         // the next clock pulse position (multiple of 40 ticks)
    double expectedTicks = 0.0;          // where the next block should start (else: a jump)
    std::int64_t nextQuarter = 0;        // MTC: the next quarter-frame index (frame * 4 + piece)
    double mtcSeconds = 0.0;             // MTC: the song time at the next block's first sample
};

// --- external clock in ------------------------------------------------------------------------------------------
// Fed by the MIDI thread with each real-time / SPP message and its time stamp (seconds, same clock as the engine's
// Time::getMillisecondCounterHiRes()/1000). Publishes a smoothed tempo and a song position for the audio thread.
class Receiver
{
public:
    enum class Command : std::uint8_t
    {
        none,
        start,
        stop,
        resume,
        locate
    };

    // returns true if the message was a sync message (consumed)
    bool feed (const std::uint8_t* bytes, int size, double seconds) noexcept;
    void reset() noexcept;

    // --- read from any thread (atomics) ---
    double bpm() const noexcept { return tempo.load (std::memory_order_relaxed); }   // 0 = no clock yet
    bool running() const noexcept { return isRunning.load (std::memory_order_relaxed); }
    bool receiving (double now) const noexcept;                                         // a clock in the last 0.5 s
    // The song position (ticks) the master is at at time `seconds`, while running: the last pulse's position plus
    // the time since it at the measured tempo (extrapolated at most 0.1 s, or one pulse).
    double ticksAt (double seconds) const noexcept;
    // Transport commands for the audio thread (start / stop / continue / SPP), oldest first.
    bool popCommand (Command& c, double& ticks) noexcept;
    bool tooFast() const noexcept { return fast.load (std::memory_order_relaxed); }   // over 522 BPM (FL: "Clock is too fast !")

    static constexpr int window = 96;          // pulses in the tempo estimate (four beats)
    static constexpr double hysteresis = 0.1; // BPM
    static constexpr double minBpm = 10.0, maxBpm = 522.0; // FL's tempo range (its receiver clamps there)

private:
    void pushCommand (Command c, double ticks) noexcept;
    void pulse (double seconds) noexcept;

    // MIDI thread only
    std::array<double, window + 1> times {};
    int timeCount = 0, timeHead = 0, changeVotes = 0;
    double shownBpm = 0.0;
    bool armed = false;            // after Start/Continue: the next pulse is the first one played
    double positionTicks = 0.0;    // the master's position (ticks) at the last pulse
    double sppTicks = 0.0;

    std::atomic<double> tempo { 0.0 }, lastPulse { -1.0 }, lastPulseTicks { 0.0 }, pulseSeconds { 0.0 };
    std::atomic<bool> isRunning { false }, fast { false };
    struct Pending
    {
        Command command = Command::none;
        double ticks = 0.0;
    };
    SpscQueue<Pending, 64> commands;
};

// FL's status text: "Receiving clock 120.0 BPM" / "No clock" (the UI translates the parts).
std::string statusText (const Receiver& r, double now);

// Tempo smoothing: a least-squares fit of the last `window` pulse times (robust to per-message jitter), then
// hysteresis: the published tempo moves only when the estimate differs by more than `threshold` BPM, so a steady
// clock with jitter shows a steady tempo. Exposed for tests.
double fitBpm (const double* times, int count) noexcept;
} // namespace lynx::midisync
