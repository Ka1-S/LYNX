#pragma once

// LYNX's visual plugins (docs/analise/REA_rodada164c.md): Wave Candy (FL's analyser: oscilloscope, spectrum,
// spectrogram, peak / VU meter, vectorscope) and the Visualizer (FL's ZGameEditor Visualizer role: an audio-reactive
// scene of layers, full screen and a video export of the song). Neither changes the sound. The audio thread only
// copies the slot's signal into a lock-free ring (Tap) — no allocation, no lock, a few hundred nanoseconds a block;
// every analysis and all drawing happen in the editor (UI thread) or in the video export, never in the audio callback.
// No JUCE here (tests/Visual164Tests.inc).

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx
{
class Effect;
}

namespace lynx::visualfx
{
// ---------------------------------------------------------------------------------------------------------------
// The tap: the slot's stereo signal for the editor
// ---------------------------------------------------------------------------------------------------------------
constexpr int tapSize = 1 << 15; // ~0.7 s at 48 kHz: enough for a 16384-point spectrum and a 500 ms scope window
constexpr int shownParams = 16;  // the parameters the editor sees as the audio thread used them (automation)

// The video export's recording of what the slot heard while the song rendered: the exporter reserves room for the
// whole song first; process() only writes into what is reserved (never grows a vector) and stops when it is full.
struct Capture
{
    std::vector<float> left, right;
    std::size_t used = 0;
    struct ParamPoint
    {
        std::size_t at = 0; // sample index where these values start
        std::array<float, shownParams> values {};
    };
    std::vector<ParamPoint> params; // one per processed block while playing (reserved by the exporter)
    std::size_t paramsUsed = 0;
    bool overflow = false;

    void reserve (std::size_t frames);
};

class Tap
{
public:
    // audio thread
    void write (const float* left, const float* right, int n) noexcept;
    void publishParams (const EffectParams& params) noexcept;

    // any thread: the newest `count` (≤ tapSize) frames, oldest first (zeros before anything was written); returns how
    // many frames were ever written (the editor compares it with its last read to know what is new)
    std::uint64_t read (float* left, float* right, int count) const noexcept;
    std::uint64_t written() const noexcept { return count.load (std::memory_order_acquire); }
    float param (int i) const noexcept { return i >= 0 && i < shownParams ? params[(std::size_t) i].load (std::memory_order_relaxed) : 0.0f; }

    std::atomic<double> sampleRate { 48000.0 };

private:
    std::array<std::atomic<float>, tapSize> l {}, r {};
    std::array<std::atomic<float>, shownParams> params {};
    std::atomic<std::uint64_t> count { 0 };
};

// The effect of a Wave Candy or Visualizer slot: passes the audio unchanged and feeds its Tap (and, during a video
// export, the Capture armed on it).
std::unique_ptr<Effect> make();
// The tap of a running Wave Candy / Visualizer effect (nullptr for any other effect).
Tap* tapOf (Effect* effect) noexcept;
// Arms (or with nullptr, disarms) the capture of a running visual effect. The effect records only while the song
// plays (an export's warm-up blocks are left out). Call while the effect is not processing, or accept that the block
// in flight is lost (the exporter arms it before the render starts).
bool setCapture (Effect* effect, Capture* capture) noexcept;

bool isVisualKind (EffectKind kind) noexcept; // waveCandy, visualizer

// ---------------------------------------------------------------------------------------------------------------
// Analysis (used by the editors and by the export; deterministic)
// ---------------------------------------------------------------------------------------------------------------
namespace analysis
{
// In-place radix-2 FFT (size a power of two).
void fft (std::vector<std::complex<float>>& a) noexcept;
// Magnitudes in dB of `n` (power of two) samples with a Hann window, scaled so a full-scale sine reads 0 dB: n/2 + 1
// values into `db`. `work` is scratch (resized as needed — call off the audio thread).
void spectrumDb (const float* x, int n, std::vector<float>& db, std::vector<std::complex<float>>& work);
// `bands` values (dB) at log-spaced frequencies from loHz to hiHz: the loudest bin within each band, interpolated
// between bins where the bands are narrower than a bin; `tilt` adds dB per octave around 1 kHz (pink weighting = 3).
void logBands (const std::vector<float>& db, double sampleRate, int fftSize, double loHz, double hiHz, float* out, int bands,
               double tiltDbPerOctave = 0.0);
// The oscilloscope's start: the newest rising zero crossing that leaves `span` samples after it (else n − span).
int triggerPoint (const float* x, int n, int span) noexcept;

// Level meter ballistics (FL Wave Candy's meter modes): peak (instant), PPM (≈5 ms integration), RMS (300 ms),
// VU (300 ms, the classic needle), LUFS (BS.1770 K-weighted, 400 ms momentary).
enum class MeterMode { peak, ppm, rms, vu, lufs };
class Meter
{
public:
    void prepare (double sampleRate);
    void reset();
    // n new frames; holdSeconds / decayDbPerSecond: the peak marker and the bars' fall
    void process (const float* left, const float* right, int n, MeterMode mode, double holdSeconds, double decayDbPerSecond);
    float levelDb (int ch) const noexcept { return level[(std::size_t) ch]; }   // the bar (with the fall)
    float peakDb (int ch) const noexcept { return peak[(std::size_t) ch]; }     // the held marker
    float maxDb() const noexcept { return maximum; }                           // highest since reset
    float loudness() const noexcept { return lufs; }                           // LUFS mode: the momentary loudness

private:
    double rate = 48000.0;
    std::array<double, 2> ms {}, ppmState {}, vuState {};
    std::array<float, 2> level { -144.0f, -144.0f }, peak { -144.0f, -144.0f };
    std::array<double, 2> holdLeft {};
    float maximum = -144.0f, lufs = -144.0f;
    // K-weighting (two biquads per channel) and the 400 ms mean square
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run (double x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    std::array<Biquad, 2> shelf {}, highPass {};
    double kMean = 0.0;
};
} // namespace analysis

// ---------------------------------------------------------------------------------------------------------------
// Wave Candy (FL's Wave Candy role)
// ---------------------------------------------------------------------------------------------------------------
namespace wavecandy
{
enum Param : int
{
    mode,           // Auto, Oscilloscope, Spectrum, Spectrogram, Peak meter, Vectorscope
    gain,           // display gain ±24 dB
    hold,           // freeze the display
    oscWindow,      // oscilloscope time window 2 … 500 ms
    oscSeparation,  // L / R drawn apart
    oscSync,        // lock the trace to the waveform's period (zero crossing)
    specSpeed,      // spectrogram scroll / spectrum fall speed
    specRange,      // dB range 30 … 120
    specScale,      // frequency scale: 0 linear-ish … 1 log (low-frequency detail)
    specWeighting,  // 0 flat (white) … 1 natural (pink, +3 dB/oct)
    specResolution, // FFT size 1024 … 16384
    meterMode,      // Peak, PPM, RMS, VU, LUFS
    meterHold,      // peak hold 0 … 5 s
    meterDecay,     // bar fall 3 … 60 dB/s
    meterScale,     // meter floor −24 … −96 dB
    vecPersistence, // vectorscope trail
    vecThickness,   // dot size
    colour,         // trace hue
    count
};

enum class Mode { automatic, oscilloscope, spectrum, spectrogram, peakMeter, vectorscope };
constexpr int modeCount = 6;
Mode modeOf (float v) noexcept;
float modeValue (Mode m) noexcept;
const char* modeName (Mode m) noexcept;
// Auto (FL): a tall narrow display shows the meter, a wide short one the oscilloscope, anything else the spectrum
Mode resolve (Mode m, int width, int height) noexcept;

analysis::MeterMode meterModeOf (float v) noexcept;
float meterModeValue (analysis::MeterMode m) noexcept;
const char* meterModeName (analysis::MeterMode m) noexcept;

double gainDb (float v) noexcept;         // −24 … +24
double windowMs (float v) noexcept;       // 2 … 500, log
double rangeDb (float v) noexcept;        // 30 … 120
int fftSize (float v) noexcept;           // 1024, 2048, 4096, 8192, 16384
double holdSeconds (float v) noexcept;    // 0 … 5
double decayDbPerSecond (float v) noexcept; // 3 … 60
double meterFloorDb (float v) noexcept;   // −24 … −96
double specFallDbPerFrame (float v) noexcept;
int spectrogramPixelsPerFrame (float v) noexcept; // 1 … 4

std::string paramText (int param, float v);

// FL's Wave Candy state (the plugin's own data, round 164c): the display mode is the 32-bit value after the display
// title and font (FL: 0 oscilloscope, 1 spectrum, 2 peak meter, 3 vectorscope). Sets slot.params; false = not read.
bool fromFl (const std::vector<std::uint8_t>& state, EffectSlot& slot);
} // namespace wavecandy

// ---------------------------------------------------------------------------------------------------------------
// The Visualizer: a scene of audio-reactive layers
// ---------------------------------------------------------------------------------------------------------------
namespace scene
{
enum class LayerType { background, bars, circle, waveform, particles, rings, image, text, flash };
constexpr int layerTypeCount = 9;
const char* typeId (LayerType t) noexcept;   // saved name
const char* typeName (LayerType t) noexcept; // shown name (English; the UI translates)

enum class Band { level, bass, mid, high, beat };
constexpr int bandCount = 5;
const char* bandName (Band b) noexcept;

struct Layer
{
    LayerType type = LayerType::bars;
    bool enabled = true;
    std::uint32_t colourA = 0xffff3048, colourB = 0xff6a2cff; // ARGB
    float opacity = 1.0f;
    float x = 0.5f, y = 0.5f; // centre / anchor, 0..1 of the frame
    float size = 0.5f;        // 0..1 (each type reads it its way: bar height, radius, font size, image scale…)
    float react = 0.5f;       // how much the band moves it
    Band band = Band::bass;
    float speed = 0.3f;       // motion (rotation, scrolling, particle speed)
    float amount = 0.5f;      // second knob per type (bars: gap, waveform: thickness, particles: density, text: glow…)
    int count = 48;           // bars, particles, rings
    bool mirror = false;      // bars / waveform mirrored around their line
    bool additive = false;    // light blending
    std::string text;         // text layer (several lines with \n)
    std::string path;         // image / background image file

    bool operator== (const Layer&) const = default;
};

struct Scene
{
    std::string name;
    std::vector<Layer> layers; // drawn in order (first = bottom)
    int width = 1920, height = 1080, fps = 30; // the video export's defaults

    bool operator== (const Scene&) const = default;
};

std::string write (const Scene& s);
std::optional<Scene> parse (const std::string& text);
std::vector<std::uint8_t> toState (const Scene& s);
std::optional<Scene> fromState (const std::vector<std::uint8_t>& state);
Scene defaultScene();
// LYNX's built-in scenes (name, scene)
const std::vector<Scene>& presets();

// FL's ZGameEditor Visualizer state (an INI text, round 164c): its layers' effects mapped to the closest LYNX layers
// (Peak Effects → bars / circle / waveform, Background → background, Image → image, Text → text, Particles → particles…);
// the effects with no counterpart are listed in `unmapped`. nullopt = not a ZGameEditor state.
std::optional<Scene> fromZge (const std::vector<std::uint8_t>& state, std::vector<std::string>* unmapped = nullptr);
} // namespace scene

// The Visualizer's knobs (automatable; they shape every layer)
namespace visualizer
{
enum Param : int { sensitivity, beatThreshold, hueShift, brightness, zoom, speed, count };
std::string paramText (int param, float v);
} // namespace visualizer

// What a frame of the scene reacts to, from the newest audio
struct Frame
{
    double time = 0.0;   // seconds since the start (the song position in an export)
    double dt = 1.0 / 30.0;
    float level = 0.0f, bass = 0.0f, mid = 0.0f, high = 0.0f; // 0..1 (smoothed)
    float beat = 0.0f;   // pulse: 1 on a beat, falls to 0
    bool onset = false;  // a beat starts in this frame
    std::uint32_t beats = 0;
    std::array<float, 64> spectrum {}; // 0..1 log bands 30 Hz … 16 kHz
    std::array<float, 256> waveL {}, waveR {};
    std::array<float, shownParams> params {};

    float bandValue (scene::Band b) const noexcept;
};

class FrameAnalyzer
{
public:
    // `n` newest frames of audio (n a power of two ≥ 1024; the analysis uses the newest 2048), the time and the step
    // since the previous frame; `params` the Visualizer knobs as the audio thread used them
    const Frame& analyze (const float* left, const float* right, int n, double sampleRate, double time, double dt,
                          const std::array<float, shownParams>& params);
    const Frame& last() const noexcept { return frame; }
    void reset();

private:
    Frame frame;
    std::vector<float> mono, db;
    std::vector<std::complex<float>> work;
    float slowBass = 0.0f, bassAverage = 0.0f, waveGain = 1.0f;
    double lastBeat = -10.0;
};

// Particles of a particles layer (deterministic: the same frames give the same picture, live or exported)
struct Particle
{
    float x = 0, y = 0, vx = 0, vy = 0, life = 0, size = 0, hue = 0;
};
class ParticleField
{
public:
    void step (const Frame& f, const scene::Layer& layer, float aspect);
    const std::vector<Particle>& particles() const noexcept { return list; }

private:
    std::vector<Particle> list;
    std::uint32_t seed = 0x1234567u;
    float next();
};
} // namespace lynx::visualfx
