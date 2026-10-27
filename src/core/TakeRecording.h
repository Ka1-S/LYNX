#pragma once

// Audio recording from hardware inputs, the parts that need no audio device (round 163, docs/analise/REA_rodada163c.md):
//   - where a recorded take lands on the Playlist, to the sample (latency compensation: driver-reported or measured
//     round trip, a manual offset and the plugin delay compensation the Master output had while recording);
//   - the latency "ping": clicks out, found again on an input through a loopback (cable or virtual), the round trip;
//   - the take stream: a lock-free ring the audio thread fills and a writer thread drains, which never loses the
//     timeline (an overflow becomes silence of the same length at the same place, counted);
//   - a WAV file written so a crash loses at most the last second (the header is rewritten every second) and the
//     repair of a WAV left behind by a crash.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Recorder.h"

namespace lynx::takes
{
// --- latency compensation -----------------------------------------------------------------------------------------

// What delays a recorded sample from the moment its sound played. `measured` is a round trip measured with the ping
// (outputs -> inputs, as the audio callback sees it), -1 when there is none for this device setup: then the driver's
// reported input + output latencies stand in. `manual` is the user's fine adjust (may be negative). `pdc` = the
// plugin delay compensation of the Master output (the song is heard that much later than it is scheduled).
struct Latency
{
    int driverInput = 0, driverOutput = 0;
    int measured = -1;
    int manual = 0;
    int pdc = 0;

    int roundTrip() const noexcept { return measured >= 0 ? measured : driverInput + driverOutput; }
    int total() const noexcept { return roundTrip() + manual + pdc; }
};

// Key of a device setup a measurement belongs to ("type|device|input|rate|block"): another driver, device, rate or
// buffer size has another round trip.
std::string setupKey (const std::string& type, const std::string& output, const std::string& input, double sampleRate, int block);

// --- placement ------------------------------------------------------------------------------------------------------

// A recorded take on the song timeline. `firstTick` = transport position of the take's first sample as the audio
// callback saw it (the start of the block it arrived in, fractional ticks), `takeTicks` = the ticks the transport
// moved while it recorded (the sum over blocks, not wrapped by a loop), `ticksPerSample` at that tempo, `latency` =
// Latency::total(). The clip starts on a whole tick; the source offset keeps the rest so the take's samples sit where
// they were played to the sample (offset + offsetFraction are the ticks of audio skipped at the clip start).
struct Placement
{
    std::int64_t start = 0, length = 0, offset = 0;
    float offsetFraction = 0.0f;
    double exactStart = 0.0; // song tick of the take's first sample (may be negative)
    double ticks = 0.0;      // the take's length in ticks (takeTicks)
    double latencyTicks = 0.0; // the compensation in ticks (firstTick = exactStart + latencyTicks)
    bool valid() const noexcept { return length > 0; }
};
Placement place (double firstTick, double takeTicks, double ticksPerSample, int latency);

// The parts of a placed take inside punch ranges (each with its own source offset; the fraction stays).
struct Part
{
    std::int64_t start = 0, length = 0, offset = 0;
    float offsetFraction = 0.0f;
};
std::vector<Part> punch (const Placement& p, const std::vector<PunchRange>& ranges);

// Loop recording: a take recorded while the transport looped over [loopStart, loopEnd) is cut into one part per pass
// (the first pass may start before the loop: pre-roll, which is left out). `wraps` = where the transport jumped back
// to loopStart, in ticks of the take from its first sample (frames * ticksPerSample, as the audio thread saw them;
// empty: computed for an exact loop length). Each part is in song ticks with its source offset. Passes shorter than
// `minimum` ticks (a pass stopped right away) are dropped, except a lone one.
std::vector<Part> loopPasses (const Placement& p, std::int64_t loopStart, std::int64_t loopEnd, std::int64_t minimum = 0,
                              std::vector<double> wraps = {});

// The Playlist track a take recorded through mixer insert `insert` goes to: the first audio track linked to that insert
// (Playlist track mode "audio", REA round 146a) that is free over [start, end); else a new track after the last one
// used. Then linkTakeTrack() links a fresh track to the insert, so the next take of that insert lands there too.
int takeTrack (const Song& song, int insert, std::int64_t start, std::int64_t end);
void linkTakeTrack (Song& song, int track, int insert);

// Puts recorded takes on the Playlist: one audio channel per file (through its insert; 0 = a new free insert) and a
// clip per part. Several parts that cover the same song range (loop passes) are take lanes: the last pass plays on
// the take's track, the earlier ones go muted on the tracks below it, grouped under it (folded), newest first.
struct TakeFile
{
    std::string path, name;
    int insert = 0;
};
void placeTakeClips (Song& song, const std::vector<TakeFile>& files, const std::vector<Part>& parts);

// Comping (round 163): the comp track of a take lane = the group header above a grouped Playlist track (-1: the track
// is not a lane). promoteTake() puts [from, to) of audio clip `clipIndex` (a lane's take) on `compTrack`, replacing
// what plays there in that range (the comp's clips are split at the range's edges); the promoted piece plays, with
// short fades where it was cut out of the take. The range is clamped to the clip. Returns false when nothing changed.
int compTrackOf (const Song& song, int track);
constexpr std::int64_t compFadeTicks = 8; // ~4 ms at 120 bpm: no click where a comp switches takes
bool promoteTake (Song& song, int clipIndex, int compTrack, std::int64_t from, std::int64_t to);

// --- latency ping ---------------------------------------------------------------------------------------------------

// Writes a click every `period` samples on the outputs and finds it again on one input. The delay of each click is
// counted from the output sample it was written at to the input sample its peak came back at, in the callback's
// samples: what a recording has to be moved back by. Real-time safe (no allocation after start()).
class LatencyPing
{
public:
    struct Result
    {
        bool ok = false;
        int samples = 0;       // median round trip
        int spread = 0;        // max - min of the clicks found
        int found = 0, sent = 0;
        float peak = 0.0f;     // level the clicks came back at (0..1)
        float noise = 0.0f;    // input level between clicks
        std::string problem;   // why not ok ("no signal", "unstable", ...)
    };

    // `clicks` clicks, one every `period` samples (the first after one period); the click is a short band-limited
    // pulse of `level`.
    void start (int clicks = 8, int period = 12000, float level = 0.5f);
    void cancel() noexcept { running.store (false); }
    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    // audio thread: `in` = the input listened to (may be null), the click is added on `outs` (null entries skipped)
    void process (const float* in, float* const* outs, int numOuts, int n) noexcept;

    // after isRunning() turned false
    Result result() const;

    static constexpr int maxClicks = 32;

private:
    std::atomic<bool> running { false };
    int clicks = 8, period = 12000;
    float level = 0.5f;
    std::int64_t position = 0;
    int sent = 0;
    std::int64_t lastClick = -1; // output sample of the click being listened for
    float best = 0.0f;
    std::int64_t bestAt = -1;
    std::array<int, maxClicks> delays {};
    std::array<float, maxClicks> peaks {};
    int found = 0;
    double noiseSum = 0.0;
    std::int64_t noiseCount = 0;
};

// --- take stream ----------------------------------------------------------------------------------------------------

// One take's samples between the audio thread (push) and the writer thread (pull), planar, 1 or 2 channels.
// When the writer falls behind and the ring is full, the frames that don't fit are counted as a gap at that place:
// pull() hands out that many zeros there, so the file keeps its length and every later sample its time.
class TakeStream
{
public:
    explicit TakeStream (int channels, int capacityFrames = 1 << 20);

    int channels() const noexcept { return numChannels; }

    // audio thread. `data[c]` for c < channels(). Never blocks or allocates.
    void push (const float* const* data, int n) noexcept;
    // audio thread: n frames of silence (an input channel that is not there this block)
    void pushSilence (int n) noexcept;

    // after the audio thread let go of this take (no more push): the frames it lost at the very end are handed out
    // by pull() too
    void finish() noexcept;

    // writer thread: up to `maxFrames` frames into out[c] (planar). Returns the frames written (0 = nothing yet).
    int pull (float* const* out, int maxFrames) noexcept;

    std::int64_t pushedFrames() const noexcept { return pushed.load (std::memory_order_acquire); } // incl. gaps
    std::int64_t lostFrames() const noexcept { return lost.load (std::memory_order_acquire); }       // turned to silence
    bool empty() const noexcept;

private:
    int numChannels;
    std::uint64_t capacity, mask;
    std::vector<float> ring; // [channel][capacity]
    std::atomic<std::uint64_t> writeIndex { 0 }, readIndex { 0 };
    // gaps: (ring position where the gap goes, frames); a small SPSC queue
    struct Gap
    {
        std::uint64_t at = 0;
        std::int64_t frames = 0;
    };
    static constexpr std::uint64_t maxGaps = 256;
    std::array<Gap, maxGaps> gaps {};
    std::atomic<std::uint64_t> gapWrite { 0 }, gapRead { 0 };
    std::int64_t pendingGap = 0;          // audio thread: frames lost not yet queued (queue was full)
    std::int64_t zerosLeft = 0;           // writer thread: zeros still to hand out for the current gap
    std::atomic<std::int64_t> finishedPending { 0 }; // lost at the end (finish())
    std::atomic<std::int64_t> pushed { 0 }, lost { 0 };
};

// --- WAV file -------------------------------------------------------------------------------------------------------

enum class SampleFormat : std::uint8_t { pcm24, float32 };

// A WAV writer for takes: the header is valid from the start and rewritten with the current length every
// `syncSeconds` (and the file flushed), so a crash leaves a playable file missing at most that much.
class WavTakeWriter
{
public:
    ~WavTakeWriter();
    bool open (const std::string& path, double sampleRate, int channels, SampleFormat format, double syncSeconds = 1.0);
    bool write (const float* const* data, int n); // planar
    bool close();                                 // final header; the file is complete
    void abandon();                               // tests: closes the file as a crash would leave it (no final header)
    bool isOpen() const noexcept { return file != nullptr; }
    std::int64_t frames() const noexcept { return written; }
    const std::string& error() const noexcept { return problem; }

private:
    bool writeHeader();
    std::FILE* file = nullptr;
    int numChannels = 0;
    SampleFormat format = SampleFormat::pcm24;
    double rate = 48000.0;
    std::int64_t written = 0, sinceSync = 0, syncFrames = 48000;
    std::vector<unsigned char> buffer;
    std::string problem;
};

// A WAV whose header lengths don't match the file (left by a crash while recording): sets them from the file's size.
// Returns true when the file is now a valid WAV (it already was, or was repaired); `frames` = its length.
bool repairWav (const std::string& path, std::int64_t* frames = nullptr, bool* changed = nullptr);

// Reads a WAV written by WavTakeWriter (pcm24 / float32; tests and the recovery's checks). Planar.
bool readWav (const std::string& path, std::vector<std::vector<float>>& channels, double* sampleRate = nullptr);

// --- crash recovery journal -------------------------------------------------------------------------------------------

// What a take in progress needs to be placed again after a crash: written next to the WAV as "<file>.lynxrec" when
// its first sample arrives, removed when the take is finished normally.
struct Journal
{
    std::string wav;
    double firstTick = 0.0, ticksPerSample = 0.0, sampleRate = 0.0;
    int latency = 0;
    int insert = 0;        // mixer insert it played through (0 = a new one)
    int playlistTrack = -1;
    std::string project;   // project file (empty: unsaved)
};
std::string journalText (const Journal& j);
bool parseJournal (const std::string& text, Journal& j);

// File names (round 163): "<Track>_Take<N>" ("Audio" when the insert has no name), with characters a file name can't
// hold replaced. The caller makes the name unique by raising N.
std::string takeName (const std::string& trackName, int take);

// Minutes of recording `bytesFree` holds for `channels` channels in total at `sampleRate` in `format`.
double minutesLeft (std::int64_t bytesFree, double sampleRate, int channels, SampleFormat format) noexcept;
} // namespace lynx::takes
