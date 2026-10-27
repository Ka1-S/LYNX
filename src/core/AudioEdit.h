#pragma once

// Audio editor core (round 86; the feature set follows FL's Edison, docs/analise/REA_rodada86.md, the code is LYNX's
// own). A Document is audio + markers/regions + a loop; every edit works on a frame range and keeps the markers
// and the loop in place relative to the audio. History keeps whole states (one per edit) for undo/redo and the
// undo history list. Also: a spectrogram, zero-crossing snap, onset detection (auto-slice) and WAV encoding and
// decoding with the markers (cue/labl/ltxt) and the loop (smpl). JUCE-free; tests in tests/AudioEditTests.inc.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lynx::audioedit
{
struct Audio
{
    std::vector<std::vector<float>> channels; // 1 (mono) or 2 (stereo), all the same length
    double sampleRate = 44100.0;

    std::int64_t frames() const noexcept { return channels.empty() ? 0 : (std::int64_t) channels[0].size(); }
    int numChannels() const noexcept { return (int) channels.size(); }
    bool empty() const noexcept { return frames() == 0; }
    double seconds() const noexcept { return sampleRate > 0.0 ? (double) frames() / sampleRate : 0.0; }
};

// A marker (end == start) or a region (end > start), in frames.
struct Marker
{
    std::int64_t start = 0, end = 0;
    std::string name;
    bool isRegion() const noexcept { return end > start; }
};

// --- point envelopes (round 165l: the curves drawn in the tool forms and the amp envelopes over the audio) ---------
// x 0..1 (time or frequency, depending on the tool), y 0..1; `tension` bends the segment that ends at the point
// (-1..1, 0 = straight; positive = slow start). Points are kept sorted by x.
struct EnvPoint
{
    double x = 0.0, y = 0.0;
    float tension = 0.0f;
};

struct Envelope
{
    std::vector<EnvPoint> points;

    bool empty() const noexcept { return points.empty(); }
    // The value at x (the first / last point's value outside them; 0 when there are no points).
    double at (double x) const noexcept;
    void sort();
    // Index of the point inserted at (x, y).
    int insert (double x, double y, float tension = 0.0f);

    static Envelope flat (double y); // two points (0, y) and (1, y)
};

struct Document
{
    Audio audio;
    std::vector<Marker> markers; // sorted by start
    std::int64_t loopStart = -1, loopEnd = -1; // frames [loopStart, loopEnd), -1 = no loop
    std::string title;
    // Edison's amp envelopes drawn over the audio (round 165l; x 0..1 over the whole audio; empty = none), applied
    // by "Mixdown amp envelopes" (core/AudioEditTools.h).
    Envelope volumeEnvelope, panEnvelope, separationEnvelope;

    bool hasLoop() const noexcept { return loopStart >= 0 && loopEnd > loopStart; }
};

// Frames [start, end).
struct Range
{
    std::int64_t start = 0, end = 0;
    std::int64_t length() const noexcept { return end > start ? end - start : 0; }
    bool empty() const noexcept { return end <= start; }
};

// The range clamped to the audio (start <= end).
Range clamp (const Document& doc, Range r) noexcept;
// An empty range means the whole audio (what Edison's tools do without a selection). The level, polarity, fade,
// reverse, clear and time tools below treat an empty range that way; erase/cut/trim do nothing with it.
Range orAll (const Document& doc, Range r) noexcept;

// Silent audio with the document's format.
Audio silence (int channels, double sampleRate, std::int64_t frames);
// `clip` in `channels` channels at `sampleRate` (mono↔stereo, resampling).
Audio conform (const Audio& clip, int channels, double sampleRate);

// --- clipboard edits ------------------------------------------------------------------------------------------
Audio copy (const Audio& audio, Range r);
void erase (Document& doc, Range r);                              // Delete: the rest closes the gap
Audio cut (Document& doc, Range r);                               // copy + erase
void insert (Document& doc, std::int64_t at, const Audio& clip);  // Paste insert (converted to the document's format)
void pasteReplace (Document& doc, Range r, const Audio& clip);    // the range becomes the clip
void pasteMix (Document& doc, std::int64_t at, const Audio& clip); // summed onto the audio (extends it if needed)
void trim (Document& doc, Range r);                               // keeps only the range
void clear (Document& doc, Range r);                              // silence in place (same length)
void insertSilence (Document& doc, std::int64_t at, std::int64_t frames);
void deleteAfterLoop (Document& doc);

// --- level -------------------------------------------------------------------------------------------------------
enum class Curve : std::uint8_t
{
    linear,
    smooth,      // S-curve (half cosine)
    exponential, // slow start (fast end) for fade ins: x^2-like in amplitude, natural for fades in dB
    logarithmic  // fast start
};
// Gain of a fade in at position x (0..1). A fade out uses curveGain (c, 1 - x).
float curveGain (Curve c, float x) noexcept;
void fadeIn (Document& doc, Range r, Curve c = Curve::smooth);
void fadeOut (Document& doc, Range r, Curve c = Curve::smooth);
constexpr double declickSeconds = 0.004; // declick in/out: a smooth fade this long at the range's edge
void declickIn (Document& doc, Range r);
void declickOut (Document& doc, Range r);

float peak (const Audio& audio, Range r) noexcept;
float rms (const Audio& audio, Range r) noexcept;
// Volume (linear) and pan (-1..1: the far side keeps unity, the near side is turned down linearly).
void amp (Document& doc, Range r, float gain, float pan = 0.0f);
// Scales the range so its peak is targetDb (dBFS). Returns the gain applied (1 when silent).
float normalize (Document& doc, Range r, float targetDb = 0.0f);
void reverse (Document& doc, Range r);
void invert (Document& doc, Range r);   // reverse polarity
void removeDc (Document& doc, Range r); // Edison's "Center": subtracts each channel's mean

// --- channels ----------------------------------------------------------------------------------------------------
enum class MonoSource : std::uint8_t
{
    merged,
    left,
    right
};
void toMono (Document& doc, MonoSource source = MonoSource::merged);
void toStereo (Document& doc);
void swapChannels (Document& doc);

// --- time --------------------------------------------------------------------------------------------------------
// Windowed-sinc resampling of one channel to `outFrames` frames (band-limited when shortening).
std::vector<float> resampleChannel (const std::vector<float>& in, std::int64_t outFrames);
// New sample rate, same pitch and duration (the number of frames changes; markers follow).
void resample (Document& doc, double newRate);
// Time stretch of the range without changing pitch (ratio = new length / old length; WSOLA, core/TimeStretch).
void timeStretch (Document& doc, Range r, double ratio);
// Pitch shift of the range (semitones) keeping its length.
void pitchShift (Document& doc, Range r, double semitones);
// Speed change (resample) of the range: pitch and length change together (ratio = new length / old length).
void changeSpeed (Document& doc, Range r, double ratio);

// --- markers, regions, loop ------------------------------------------------------------------------------------
void addMarker (Document& doc, std::int64_t at, std::string name = {});
void addRegion (Document& doc, Range r, std::string name = {});
// Removes the markers at positions within the range and the regions that overlap it (an empty range: those at
// exactly r.start). Returns how many were removed.
int removeMarkers (Document& doc, Range r);
void setLoop (Document& doc, Range r); // an empty range removes the loop
void sortMarkers (Document& doc);

// The zero crossing (sign change of the channels' sum) nearest to `frame` within `radius` frames (or `frame`).
std::int64_t nearestZeroCrossing (const Audio& audio, std::int64_t frame, std::int64_t radius);
// Onsets (attacks) for auto-slicing: sensitivity 0 (dull, only big hits) .. 1 (sharp, many). Frames, sorted.
std::vector<std::int64_t> detectOnsets (const Audio& audio, float sensitivity = 0.5f);

// --- history -----------------------------------------------------------------------------------------------------
// A list of states, the first being the opened audio. Each edit commits the new state with a label; undo/redo
// move through the list. Oldest states are dropped beyond `maxBytes` (at least the current one and one before it
// are kept).
class History
{
public:
    explicit History (std::size_t maxBytes = std::size_t (768) << 20);

    void reset (const Document& doc, std::string label);
    void commit (const Document& doc, std::string label);

    bool canUndo() const noexcept { return position > 0; }
    bool canRedo() const noexcept { return position + 1 < (int) states.size(); }
    const Document& undo();
    const Document& redo();
    const Document& jumpTo (int index);
    const Document& current() const { return *states[(std::size_t) position].doc; }

    int size() const noexcept { return (int) states.size(); }
    int getPosition() const noexcept { return position; }
    const std::string& label (int index) const { return states[(std::size_t) index].label; }
    std::size_t bytes() const noexcept;

private:
    struct State
    {
        std::shared_ptr<const Document> doc;
        std::string label;
    };
    std::vector<State> states;
    int position = -1;
    std::size_t maxBytes;
};

// --- spectrogram ------------------------------------------------------------------------------------------------
struct Spectrogram
{
    int columns = 0, bins = 0; // bins = fftSize / 2
    double sampleRate = 44100.0;
    std::vector<float> db;     // [column * bins + bin], dBFS (floor -120)
    float at (int column, int bin) const noexcept { return db[(std::size_t) column * (std::size_t) bins + (std::size_t) bin]; }
};
// `columns` analysis windows evenly over frames [r.start, r.end) (channels mixed), Hann window of fftSize (power of 2).
Spectrogram spectrogram (const Audio& audio, Range r, int columns, int fftSize = 2048);

// --- WAV ---------------------------------------------------------------------------------------------------------
enum class WavFormat : std::uint8_t
{
    int16,
    int24,
    float32
};
// RIFF/WAVE with the audio, the markers and regions (cue + LIST/adtl labl, ltxt) and the loop (smpl).
std::vector<std::uint8_t> encodeWav (const Document& doc, WavFormat format = WavFormat::int24);
// PCM 8/16/24/32-bit int or 32/64-bit float, with the markers/regions/loop above. False if not a WAV it can read.
bool decodeWav (const std::uint8_t* data, std::size_t size, Document& out);
// Only the markers/regions/loop of a WAV (the audio is read elsewhere). False if none.
bool decodeWavMarkers (const std::uint8_t* data, std::size_t size, Document& out);
} // namespace lynx::audioedit
