#include "core/VisualFx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#include "core/Effects.h"

namespace lynx::visualfx
{
namespace
{
constexpr double pi = 3.14159265358979323846;

float toDb (double linear) noexcept
{
    return linear > 1.0e-7 ? (float) (20.0 * std::log10 (linear)) : -140.0f;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Tap and effect
// ---------------------------------------------------------------------------------------------------------------
void Capture::reserve (std::size_t frames)
{
    left.assign (frames, 0.0f);
    right.assign (frames, 0.0f);
    used = 0;
    params.assign (frames / 64 + 16, {});
    paramsUsed = 0;
    overflow = false;
}

void Tap::write (const float* left, const float* right, int n) noexcept
{
    const auto at = count.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        const auto k = (std::size_t) ((at + (std::uint64_t) i) & (std::uint64_t) (tapSize - 1));
        l[k].store (left[i], std::memory_order_relaxed);
        r[k].store (right[i], std::memory_order_relaxed);
    }
    count.store (at + (std::uint64_t) n, std::memory_order_release);
}

void Tap::publishParams (const EffectParams& p) noexcept
{
    for (int i = 0; i < shownParams; ++i)
        params[(std::size_t) i].store (p[(std::size_t) i], std::memory_order_relaxed);
}

std::uint64_t Tap::read (float* left, float* right, int n) const noexcept
{
    n = std::clamp (n, 0, tapSize);
    const auto end = count.load (std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
    {
        const auto back = (std::uint64_t) (n - i);
        if (back > end)
        {
            left[i] = right[i] = 0.0f;
            continue;
        }
        const auto k = (std::size_t) ((end - back) & (std::uint64_t) (tapSize - 1));
        left[i] = l[k].load (std::memory_order_relaxed);
        right[i] = r[k].load (std::memory_order_relaxed);
    }
    return end;
}

namespace
{
class VisualEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override { tap.sampleRate.store (sampleRate, std::memory_order_relaxed); }
    void reset() noexcept override {}
    void setPlaying (bool on) noexcept override { playing = on; }
    // the Visualizer's scene lives in the slot's state but changes nothing in the audio: keep running (no rebuild per edit)
    bool acceptState (const std::vector<std::uint8_t>&) noexcept override { return true; }

    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        tap.write (left, right, n);
        tap.publishParams (params);

        // the video export's recording (only while the song plays: the warm-up's silent blocks are not the song)
        auto* c = capture.load (std::memory_order_acquire);
        if (c == nullptr)
            return;
        c->seen += (std::size_t) n;
        if (! playing || c->overflow)
            return;
        if (c->used + (std::size_t) n > c->left.size() || c->paramsUsed >= c->params.size())
        {
            c->overflow = true;
            return;
        }
        auto& point = c->params[c->paramsUsed++];
        point.at = c->used;
        for (int i = 0; i < shownParams; ++i)
            point.values[(std::size_t) i] = params[(std::size_t) i];
        std::memcpy (c->left.data() + c->used, left, sizeof (float) * (std::size_t) n);
        std::memcpy (c->right.data() + c->used, right, sizeof (float) * (std::size_t) n);
        c->used += (std::size_t) n;
    }

    Tap tap;
    std::atomic<Capture*> capture { nullptr };
    bool playing = false;
};
} // namespace

std::unique_ptr<Effect> make()
{
    return std::make_unique<VisualEffect>();
}

Tap* tapOf (Effect* effect) noexcept
{
    auto* v = dynamic_cast<VisualEffect*> (effect);
    return v != nullptr ? &v->tap : nullptr;
}

bool setCapture (Effect* effect, Capture* capture) noexcept
{
    auto* v = dynamic_cast<VisualEffect*> (effect);
    if (v == nullptr)
        return false;
    v->capture.store (capture, std::memory_order_release);
    return true;
}

bool isVisualKind (EffectKind kind) noexcept
{
    return kind == EffectKind::waveCandy || kind == EffectKind::visualizer;
}

// ---------------------------------------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------------------------------------
namespace analysis
{
void fft (std::vector<std::complex<float>>& a) noexcept
{
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const auto angle = -2.0 * pi / (double) len;
        const std::complex<float> step ((float) std::cos (angle), (float) std::sin (angle));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

void spectrumDb (const float* x, int n, std::vector<float>& db, std::vector<std::complex<float>>& work)
{
    work.resize ((std::size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const auto w = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n);
        work[(std::size_t) i] = { (float) (x[i] * w), 0.0f };
    }
    fft (work);
    db.resize ((std::size_t) n / 2 + 1);
    // a full-scale sine: |X| = n/2 · 0.5 (the Hann window's coherent gain)
    const auto scale = 4.0 / (double) n;
    for (int k = 0; k <= n / 2; ++k)
        db[(std::size_t) k] = toDb (std::abs (work[(std::size_t) k]) * scale);
}

void logBands (const std::vector<float>& db, double sampleRate, int fftSize, double loHz, double hiHz, float* out, int bands, double tilt)
{
    if (bands <= 0 || db.size() < 2)
        return;
    const auto binHz = sampleRate / (double) fftSize;
    const auto last = (double) db.size() - 1.0;
    const auto ratio = std::log (hiHz / loHz);
    for (int b = 0; b < bands; ++b)
    {
        const auto f0 = loHz * std::exp (ratio * (double) b / (double) bands);
        const auto f1 = loHz * std::exp (ratio * (double) (b + 1) / (double) bands);
        const auto k0 = std::clamp (f0 / binHz, 0.0, last), k1 = std::clamp (f1 / binHz, 0.0, last);
        float v = -140.0f;
        if (std::floor (k1) - std::ceil (k0) >= 0.0)
            for (auto k = (std::size_t) std::ceil (k0); k <= (std::size_t) std::floor (k1); ++k)
                v = std::max (v, db[k]);
        else
        {
            // narrower than a bin: interpolated at the band's centre
            const auto kc = std::clamp (std::sqrt (f0 * f1) / binHz, 0.0, last);
            const auto i = (std::size_t) kc;
            const auto frac = (float) (kc - (double) i);
            v = i + 1 < db.size() ? db[i] + (db[i + 1] - db[i]) * frac : db[i];
        }
        if (tilt != 0.0)
            v += (float) (tilt * std::log2 (std::sqrt (f0 * f1) / 1000.0));
        out[b] = v;
    }
}

int triggerPoint (const float* x, int n, int span) noexcept
{
    const auto latest = n - span;
    if (latest <= 1)
        return std::max (0, latest);
    // search back at most one span for a rising crossing
    for (int i = latest; i > std::max (1, latest - span); --i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            return i;
    return latest;
}

void Meter::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    // BS.1770 K-weighting (the standard's analog prototypes through the bilinear transform)
    for (int ch = 0; ch < 2; ++ch)
    {
        {
            const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
            const auto k = std::tan (pi * f0 / rate);
            const auto vh = std::pow (10.0, g / 20.0), vb = std::pow (vh, 0.4996667741545416);
            const auto a0 = 1.0 + k / q + k * k;
            auto& s = shelf[(std::size_t) ch];
            s.b0 = (vh + vb * k / q + k * k) / a0;
            s.b1 = 2.0 * (k * k - vh) / a0;
            s.b2 = (vh - vb * k / q + k * k) / a0;
            s.a1 = 2.0 * (k * k - 1.0) / a0;
            s.a2 = (1.0 - k / q + k * k) / a0;
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const auto k = std::tan (pi * f0 / rate);
            const auto a0 = 1.0 + k / q + k * k;
            auto& h = highPass[(std::size_t) ch];
            h.b0 = 1.0;
            h.b1 = -2.0;
            h.b2 = 1.0;
            h.a1 = 2.0 * (k * k - 1.0) / a0;
            h.a2 = (1.0 - k / q + k * k) / a0;
        }
    }
    reset();
}

void Meter::reset()
{
    ms = {};
    ppmState = {};
    vuState = {};
    level = { -144.0f, -144.0f };
    peak = { -144.0f, -144.0f };
    holdLeft = {};
    maximum = lufs = -144.0f;
    for (auto* f : { &shelf, &highPass })
        for (auto& b : *f)
            b.z1 = b.z2 = 0.0;
    kMean = 0.0;
}

void Meter::process (const float* left, const float* right, int n, MeterMode mode, double holdSeconds, double decayDbPerSecond)
{
    if (n <= 0)
        return;
    const auto dt = (double) n / rate;
    const auto c300 = 1.0 - std::exp (-1.0 / (0.3 * rate));
    const auto c5 = 1.0 - std::exp (-1.0 / (0.005 * rate));
    const auto c400 = 1.0 - std::exp (-1.0 / (0.4 * rate));
    std::array<double, 2> blockPeak {};
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* x = ch == 0 ? left : right;
        auto& m = ms[(std::size_t) ch];
        auto& p = ppmState[(std::size_t) ch];
        auto& v = vuState[(std::size_t) ch];
        auto& bp = blockPeak[(std::size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            const double s = x[i], a = std::abs (s);
            bp = std::max (bp, a);
            m += (s * s - m) * c300;
            p = a > p ? p + (a - p) * c5 : p; // PPM: integrates the attack, the fall is the bar's decay
            v += (a - v) * c300;
        }
    }
    // the K-weighted mean square of both channels (summed, BS.1770)
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto y = highPass[(std::size_t) ch].run (shelf[(std::size_t) ch].run ((ch == 0 ? left : right)[i]));
            sum += y * y;
        }
        kMean += (sum - kMean) * c400;
    }
    lufs = kMean > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (kMean)) : -144.0f;

    const auto fall = (float) (decayDbPerSecond * dt);
    for (int ch = 0; ch < 2; ++ch)
    {
        float value = -144.0f;
        switch (mode)
        {
            case MeterMode::peak: value = toDb (blockPeak[(std::size_t) ch]); break;
            case MeterMode::ppm: value = toDb (ppmState[(std::size_t) ch]); break;
            case MeterMode::rms: value = toDb (std::sqrt (ms[(std::size_t) ch])); break;
            case MeterMode::vu: value = toDb (vuState[(std::size_t) ch] * (pi / 2.0) / std::sqrt (2.0)); break; // a sine reads its RMS
            case MeterMode::lufs: value = lufs; break;
        }
        auto& bar = level[(std::size_t) ch];
        // peak and PPM bars fall at the decay speed; the integrating meters already move slowly
        bar = mode == MeterMode::peak || mode == MeterMode::ppm ? std::max (value, bar - fall) : value;
        if (mode == MeterMode::ppm)
            ppmState[(std::size_t) ch] = std::pow (10.0, bar / 20.0);
        auto& pk = peak[(std::size_t) ch];
        if (bar >= pk)
        {
            pk = bar;
            holdLeft[(std::size_t) ch] = holdSeconds;
        }
        else if ((holdLeft[(std::size_t) ch] -= dt) < 0.0)
            pk = std::max (bar, pk - fall);
        maximum = std::max (maximum, bar);
    }
}
} // namespace analysis

// ---------------------------------------------------------------------------------------------------------------
// Wave Candy
// ---------------------------------------------------------------------------------------------------------------
namespace wavecandy
{
Mode modeOf (float v) noexcept
{
    return (Mode) std::clamp ((int) std::lround (v * (modeCount - 1)), 0, modeCount - 1);
}

float modeValue (Mode m) noexcept
{
    return (float) (int) m / (float) (modeCount - 1);
}

const char* modeName (Mode m) noexcept
{
    switch (m)
    {
        case Mode::automatic: return "Auto";
        case Mode::oscilloscope: return "Oscilloscope";
        case Mode::spectrum: return "Spectrum";
        case Mode::spectrogram: return "Spectrogram";
        case Mode::peakMeter: return "Peak meter";
        case Mode::vectorscope: return "Vectorscope";
    }
    return "Auto";
}

Mode resolve (Mode m, int width, int height) noexcept
{
    if (m != Mode::automatic)
        return m;
    if (height > width * 3 / 2)
        return Mode::peakMeter;
    if (width > height * 3)
        return Mode::oscilloscope;
    return Mode::spectrum;
}

analysis::MeterMode meterModeOf (float v) noexcept
{
    return (analysis::MeterMode) std::clamp ((int) std::lround (v * 4.0f), 0, 4);
}

float meterModeValue (analysis::MeterMode m) noexcept
{
    return (float) (int) m / 4.0f;
}

const char* meterModeName (analysis::MeterMode m) noexcept
{
    switch (m)
    {
        case analysis::MeterMode::peak: return "Peak";
        case analysis::MeterMode::ppm: return "PPM";
        case analysis::MeterMode::rms: return "RMS";
        case analysis::MeterMode::vu: return "VU";
        case analysis::MeterMode::lufs: return "LUFS";
    }
    return "Peak";
}

double gainDb (float v) noexcept { return (v * 2.0 - 1.0) * 24.0; }
double windowMs (float v) noexcept { return 2.0 * std::pow (250.0, (double) v); }
double rangeDb (float v) noexcept { return 30.0 + 90.0 * v; }
int fftSize (float v) noexcept { return 1024 << std::clamp ((int) std::lround (v * 4.0f), 0, 4); }
double holdSeconds (float v) noexcept { return 5.0 * v; }
double decayDbPerSecond (float v) noexcept { return 3.0 * std::pow (20.0, (double) v); }
double meterFloorDb (float v) noexcept { return -24.0 - 72.0 * v; }
double specFallDbPerFrame (float v) noexcept { return 0.5 + 11.5 * v * v; }
int spectrogramPixelsPerFrame (float v) noexcept { return 1 + std::clamp ((int) (v * 4.0f), 0, 3); }

std::string paramText (int param, float v)
{
    char text[64];
    switch (param)
    {
        case mode: return modeName (modeOf (v));
        case gain: std::snprintf (text, sizeof (text), "%+.1f dB", gainDb (v)); return text;
        case hold:
        case oscSync: return v >= 0.5f ? "on" : "off";
        case oscWindow: std::snprintf (text, sizeof (text), "%.0f ms", windowMs (v)); return text;
        case specRange: std::snprintf (text, sizeof (text), "%.0f dB", rangeDb (v)); return text;
        case specResolution: std::snprintf (text, sizeof (text), "%d points", fftSize (v)); return text;
        case meterMode: return meterModeName (meterModeOf (v));
        case meterHold: std::snprintf (text, sizeof (text), "%.1f s", holdSeconds (v)); return text;
        case meterDecay: std::snprintf (text, sizeof (text), "%.0f dB/s", decayDbPerSecond (v)); return text;
        case meterScale: std::snprintf (text, sizeof (text), "%.0f dB", meterFloorDb (v)); return text;
        case colour: std::snprintf (text, sizeof (text), "%.0f°", v * 360.0f); return text;
        default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); return text;
    }
}

bool fromFl (const std::vector<std::uint8_t>& s, EffectSlot& slot)
{
    // [title: length + text] [font: length + text] [font height: negative i32] [4 bytes] [mode: i32 0..3]
    auto printable = [&] (std::size_t at, std::size_t n) {
        if (n == 0 || at + n > s.size())
            return false;
        for (std::size_t i = 0; i < n; ++i)
            if (s[at + i] < 0x20 || s[at + i] > 0x7e)
                return false;
        return true;
    };
    int found = -1;
    for (std::size_t p = 0; p + 2 < s.size(); ++p)
    {
        const std::size_t font = s[p];
        if (font < 3 || font > 40 || ! printable (p + 1, font) || ! std::isupper (s[p + 1]))
            continue;
        const auto at = p + 1 + font;
        if (at + 12 > s.size())
            continue;
        std::int32_t height = 0, mode = 0;
        std::memcpy (&height, s.data() + at, 4);
        std::memcpy (&mode, s.data() + at + 8, 4);
        if (height > -6 || height < -60 || mode < 0 || mode > 3)
            continue;
        found = mode; // (the last match: the display settings follow the controls)
    }
    if (found < 0)
        return false;
    constexpr Mode modes[] = { Mode::oscilloscope, Mode::spectrogram, Mode::peakMeter, Mode::vectorscope }; // FL's "Spectrum" scrolls
    slot.params[(std::size_t) mode] = modeValue (modes[found]);
    return true;
}
} // namespace wavecandy

namespace visualizer
{
std::string paramText (int param, float v)
{
    char text[64];
    switch (param)
    {
        case sensitivity: std::snprintf (text, sizeof (text), "%+.0f dB", (v - 0.5f) * 36.0f); return text;
        case hueShift: std::snprintf (text, sizeof (text), "%.0f°", v * 360.0f); return text;
        case zoom: std::snprintf (text, sizeof (text), "%.2fx", 0.5 + v); return text;
        case speed: std::snprintf (text, sizeof (text), "%.2fx", std::pow (4.0, (double) v * 2.0 - 1.0)); return text;
        default: std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f); return text;
    }
}
} // namespace visualizer

// ---------------------------------------------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------------------------------------------
namespace scene
{
namespace
{
constexpr const char* header = "LYNX visualizer 1";

std::string escape (const std::string& t)
{
    std::string o;
    for (const auto c : t)
        if (c == '\n')
            o += "\\n";
        else if (c == '\\')
            o += "\\\\";
        else if (c != '\r')
            o += c;
    return o;
}

std::string unescape (const std::string& t)
{
    std::string o;
    for (std::size_t i = 0; i < t.size(); ++i)
        if (t[i] == '\\' && i + 1 < t.size())
        {
            ++i;
            o += t[i] == 'n' ? '\n' : t[i];
        }
        else
            o += t[i];
    return o;
}

const char* bandIds[] = { "level", "bass", "mid", "high", "beat" };
const char* typeIds[] = { "background", "bars", "circle", "waveform", "particles", "rings", "image", "text", "flash",
                          "canvas", "terrain", "object3d", "feedback", "postprocess" };
const char* blendIds[] = { "normal", "add", "screen", "multiply", "overlay", "difference", "lighten", "darken" };

struct EffectDef
{
    const char* id;
    const char* name;
    const char* hint;
};
// (the knobs every shader reads: Size = scale, Amount = strength, Speed = motion, React = how much the band moves it,
// Count = detail, X / Y = centre, the two colours = its palette)
const EffectDef canvasFx[] = {
    { "plasma", "Plasma", "Size: scale, Amount: colour bands, Count: layers of waves" },
    { "tunnel", "Tunnel", "Size: ring spacing, Amount: twist, Speed: flight speed" },
    { "cells", "Cells", "Size: cell size, Amount: border glow, Count: detail" },
    { "kaleido", "Kaleidoscope", "Size: zoom, Amount: folds, Count: iterations" },
    { "clouds", "Clouds", "Size: scale, Amount: density, Speed: drift" },
    { "warp", "Warp stars", "Size: star size, Amount: streaks, Count: density" },
    { "electric", "Electric", "Size: thickness, Amount: jitter, Count: bolts" },
    { "rays", "Sun rays", "Size: sun size, Amount: ray strength, Count: rays" },
};
const EffectDef terrainFx[] = {
    { "mountains", "Spectrum mountains", "The spectrum's history as a wire landscape. Size: height, Amount: line glow, Count: rows" },
    { "hills", "Shaded hills", "A lit landscape shaped by the spectrum. Size: height, Amount: fog, Speed: flight speed" },
    { "road", "Retro road", "A neon grid and a sun. Size: sun size, Amount: grid glow, Speed: flight speed" },
};
const EffectDef objectFx[] = {
    { "blob", "Reactive sphere", "A sphere the bass deforms. Size: radius, Amount: deformation, Speed: spin" },
    { "torus", "Torus", "Size: radius, Amount: thickness, Speed: spin" },
    { "cubes", "Cube grid", "Columns that follow the spectrum. Size: height, Amount: spacing, Count: columns" },
    { "rings", "Ring tunnel", "Rings flying past. Size: radius, Amount: thickness, Speed: flight speed" },
};
const EffectDef feedbackFx[] = {
    { "zoom", "Zoom trails", "Size: zoom per frame, Amount: how long trails last" },
    { "spin", "Spin tunnel", "Size: zoom per frame, Speed: rotation, Amount: trail length" },
    { "kaleido", "Kaleido feedback", "Size: zoom, Amount: trail length, Count: mirror folds" },
    { "warp", "Warp", "Size: warp scale, Amount: trail length, Speed: flow" },
    { "echo", "Echo", "Amount: how long the picture stays" },
    { "drift", "Smoke drift", "Speed: rise, Amount: trail length, Size: spread" },
};
const EffectDef postFx[] = {
    { "bloom", "Bloom", "Glow around bright parts. Amount: strength, Size: radius, Count: threshold" },
    { "blur", "Blur", "Size: radius, React: the band blurs it more" },
    { "rgbshift", "RGB shift", "Amount: colour split, React: the band pushes it" },
    { "pixelate", "Pixelate", "Size: pixel size" },
    { "dots", "Dot matrix", "Size: dot size, Amount: dot sharpness" },
    { "scanlines", "Scan lines", "Size: line spacing, Amount: darkness, Speed: roll" },
    { "vignette", "Vignette", "Size: radius, Amount: darkness" },
    { "color", "Colour correction", "Size: saturation, Amount: contrast, Speed: hue drift, colour A: tint" },
    { "kaleido", "Kaleidoscope", "Count: folds, Speed: rotation" },
    { "mirror", "Mirror", "Count: 1 left-right, 2 top-bottom, 3 both" },
    { "edges", "Edge detect", "Amount: edge strength, colour A: edge colour" },
    { "shake", "Shake", "Amount: how far the beat shakes the picture" },
    { "glitch", "Glitch", "Amount: how often slices jump, React: the band triggers it" },
    { "grain", "Film grain", "Amount: grain strength" },
    { "invert", "Invert on beat", "React: how much the band inverts the colours" },
};

struct EffectList
{
    const EffectDef* list = nullptr;
    int count = 0;
};
EffectList effectsOf (LayerType t) noexcept
{
    switch (t)
    {
        case LayerType::canvas: return { canvasFx, (int) std::size (canvasFx) };
        case LayerType::terrain: return { terrainFx, (int) std::size (terrainFx) };
        case LayerType::object3d: return { objectFx, (int) std::size (objectFx) };
        case LayerType::feedback: return { feedbackFx, (int) std::size (feedbackFx) };
        case LayerType::postprocess: return { postFx, (int) std::size (postFx) };
        default: return {};
    }
}
} // namespace

const char* typeId (LayerType t) noexcept { return typeIds[(int) t]; }

bool isShaderType (LayerType t) noexcept { return (int) t >= (int) LayerType::canvas; }
bool readsBelow (LayerType t) noexcept { return t == LayerType::feedback || t == LayerType::postprocess; }

int effectCount (LayerType t) noexcept { return effectsOf (t).count; }

const char* effectId (LayerType t, int effect) noexcept
{
    const auto e = effectsOf (t);
    return e.count == 0 ? "" : e.list[std::clamp (effect, 0, e.count - 1)].id;
}

const char* effectName (LayerType t, int effect) noexcept
{
    const auto e = effectsOf (t);
    return e.count == 0 ? "" : e.list[std::clamp (effect, 0, e.count - 1)].name;
}

const char* effectHint (LayerType t, int effect) noexcept
{
    const auto e = effectsOf (t);
    return e.count == 0 ? "" : e.list[std::clamp (effect, 0, e.count - 1)].hint;
}

int effectIndex (LayerType t, const std::string& id) noexcept
{
    const auto e = effectsOf (t);
    for (int i = 0; i < e.count; ++i)
        if (id == e.list[i].id)
            return i;
    return -1;
}

const char* blendId (Blend b) noexcept { return blendIds[(int) b]; }

const char* blendName (Blend b) noexcept
{
    switch (b)
    {
        case Blend::normal: return "Normal";
        case Blend::add: return "Add";
        case Blend::screen: return "Screen";
        case Blend::multiply: return "Multiply";
        case Blend::overlay: return "Overlay";
        case Blend::difference: return "Difference";
        case Blend::lighten: return "Lighten";
        case Blend::darken: return "Darken";
    }
    return "Normal";
}

const char* typeName (LayerType t) noexcept
{
    switch (t)
    {
        case LayerType::background: return "Background";
        case LayerType::bars: return "Spectrum bars";
        case LayerType::circle: return "Circle spectrum";
        case LayerType::waveform: return "Waveform";
        case LayerType::particles: return "Particles";
        case LayerType::rings: return "Beat rings";
        case LayerType::image: return "Image";
        case LayerType::text: return "Text";
        case LayerType::flash: return "Beat flash";
        case LayerType::canvas: return "Canvas (GPU)";
        case LayerType::terrain: return "Terrain 3D (GPU)";
        case LayerType::object3d: return "3D object (GPU)";
        case LayerType::feedback: return "Feedback (GPU)";
        case LayerType::postprocess: return "Post-process (GPU)";
    }
    return "Layer";
}

const char* bandName (Band b) noexcept
{
    switch (b)
    {
        case Band::level: return "Level";
        case Band::bass: return "Bass";
        case Band::mid: return "Mids";
        case Band::high: return "Highs";
        case Band::beat: return "Beat";
    }
    return "Level";
}

std::string write (const Scene& s)
{
    std::ostringstream o;
    o << header << "\n";
    o << "name=" << escape (s.name) << "\n";
    o << "size=" << s.width << "x" << s.height << "\n";
    o << "fps=" << s.fps << "\n";
    char hex[16];
    for (const auto& l : s.layers)
    {
        o << "layer=" << typeId (l.type) << "\n";
        o << "enabled=" << (l.enabled ? 1 : 0) << "\n";
        std::snprintf (hex, sizeof (hex), "%08x", l.colourA);
        o << "colourA=" << hex << "\n";
        std::snprintf (hex, sizeof (hex), "%08x", l.colourB);
        o << "colourB=" << hex << "\n";
        o << "opacity=" << l.opacity << "\nx=" << l.x << "\ny=" << l.y << "\nsize=" << l.size << "\nreact=" << l.react << "\n";
        o << "band=" << bandIds[(int) l.band] << "\nspeed=" << l.speed << "\namount=" << l.amount << "\ncount=" << l.count << "\n";
        o << "mirror=" << (l.mirror ? 1 : 0) << "\nadditive=" << (l.additive ? 1 : 0) << "\n";
        if (! l.text.empty())
            o << "text=" << escape (l.text) << "\n";
        if (! l.path.empty())
            o << "path=" << escape (l.path) << "\n";
        if (isShaderType (l.type))
            o << "effect=" << effectId (l.type, l.effect) << "\n";
        if (l.blend != Blend::normal)
            o << "blend=" << blendId (l.blend) << "\n";
    }
    return o.str();
}

std::optional<Scene> parse (const std::string& text)
{
    std::istringstream in (text);
    std::string line;
    if (! std::getline (in, line))
        return std::nullopt;
    if (! line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.rfind ("LYNX visualizer", 0) != 0)
        return std::nullopt;
    Scene s;
    Layer* l = nullptr;
    auto num = [] (const std::string& v, float lo, float hi) {
        char* end = nullptr;
        const auto f = std::strtof (v.c_str(), &end);
        return std::clamp (std::isfinite (f) ? f : 0.0f, lo, hi);
    };
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = line.substr (0, eq), value = line.substr (eq + 1);
        if (key == "layer")
        {
            Layer layer;
            for (int t = 0; t < layerTypeCount; ++t)
                if (value == typeIds[t])
                    layer.type = (LayerType) t;
            s.layers.push_back (layer);
            l = &s.layers.back();
            continue;
        }
        if (l == nullptr)
        {
            if (key == "name")
                s.name = unescape (value);
            else if (key == "size")
            {
                int w = 0, h = 0;
                if (std::sscanf (value.c_str(), "%dx%d", &w, &h) == 2 && w >= 16 && h >= 16 && w <= 7680 && h <= 4320)
                {
                    s.width = w & ~1;
                    s.height = h & ~1;
                }
            }
            else if (key == "fps")
                s.fps = std::clamp (std::atoi (value.c_str()), 1, 120);
            continue;
        }
        if (key == "enabled")
            l->enabled = value != "0";
        else if (key == "colourA")
            l->colourA = (std::uint32_t) std::strtoul (value.c_str(), nullptr, 16);
        else if (key == "colourB")
            l->colourB = (std::uint32_t) std::strtoul (value.c_str(), nullptr, 16);
        else if (key == "opacity")
            l->opacity = num (value, 0, 1);
        else if (key == "x")
            l->x = num (value, -1, 2);
        else if (key == "y")
            l->y = num (value, -1, 2);
        else if (key == "size")
            l->size = num (value, 0, 1);
        else if (key == "react")
            l->react = num (value, 0, 1);
        else if (key == "speed")
            l->speed = num (value, 0, 1);
        else if (key == "amount")
            l->amount = num (value, 0, 1);
        else if (key == "count")
            l->count = std::clamp (std::atoi (value.c_str()), 1, 512);
        else if (key == "mirror")
            l->mirror = value != "0";
        else if (key == "additive")
            l->additive = value != "0";
        else if (key == "band")
        {
            for (int b = 0; b < bandCount; ++b)
                if (value == bandIds[b])
                    l->band = (Band) b;
        }
        else if (key == "text")
            l->text = unescape (value);
        else if (key == "path")
            l->path = unescape (value);
        else if (key == "effect")
            l->effect = std::max (0, effectIndex (l->type, value));
        else if (key == "blend")
        {
            for (int b = 0; b < blendCount; ++b)
                if (value == blendIds[b])
                    l->blend = (Blend) b;
        }
    }
    return s;
}

std::vector<std::uint8_t> toState (const Scene& s)
{
    const auto t = write (s);
    return { t.begin(), t.end() };
}

std::optional<Scene> fromState (const std::vector<std::uint8_t>& state)
{
    return parse (std::string (state.begin(), state.end()));
}

namespace
{
// HSV (0..1 each) to ARGB
std::uint32_t hsvArgb (float h, float s, float v, float alpha)
{
    h = h - std::floor (h);
    s = std::clamp (s, 0.0f, 1.0f);
    v = std::clamp (v, 0.0f, 1.0f);
    const auto i = (int) (h * 6.0f) % 6;
    const auto f = h * 6.0f - std::floor (h * 6.0f);
    const auto pp = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    float r = v, g = t, b = pp;
    switch (i)
    {
        case 0: r = v; g = t; b = pp; break;
        case 1: r = q; g = v; b = pp; break;
        case 2: r = pp; g = v; b = t; break;
        case 3: r = pp; g = q; b = v; break;
        case 4: r = t; g = pp; b = v; break;
        default: r = v; g = pp; b = q; break;
    }
    auto byte = [] (float x) { return (std::uint32_t) std::lround (std::clamp (x, 0.0f, 1.0f) * 255.0f); };
    return (byte (alpha) << 24) | (byte (r) << 16) | (byte (g) << 8) | byte (b);
}

Layer make (LayerType type, std::uint32_t a, std::uint32_t b, float x, float y, float size, Band band, float react)
{
    Layer l;
    l.type = type;
    l.colourA = a;
    l.colourB = b;
    l.x = x;
    l.y = y;
    l.size = size;
    l.band = band;
    l.react = react;
    return l;
}
} // namespace

Layer shaderLayer (LayerType type, int effect)
{
    Layer l;
    l.type = type;
    l.effect = std::clamp (effect, 0, std::max (0, effectCount (type) - 1));
    l.band = Band::bass;
    l.react = 0.5f;
    l.size = 0.5f;
    l.amount = 0.5f;
    l.speed = 0.3f;
    l.count = 48;
    const std::string id = effectId (type, l.effect);
    auto colours = [&l] (std::uint32_t a, std::uint32_t b) {
        l.colourA = a;
        l.colourB = b;
    };
    switch (type)
    {
        case LayerType::canvas:
            if (id == "plasma") colours (0xff2a0a6a, 0xffff3070);
            else if (id == "tunnel") { colours (0xff101030, 0xffff3048); l.speed = 0.4f; l.amount = 0.3f; }
            else if (id == "cells") colours (0xff0a1830, 0xff40c0ff);
            else if (id == "kaleido") { colours (0xffff3048, 0xff3060ff); l.amount = 0.4f; }
            else if (id == "clouds") { colours (0xff20307a, 0xffc060ff); l.speed = 0.2f; }
            else if (id == "warp") { colours (0xffffffff, 0xff6aa8ff); l.blend = Blend::add; l.size = 0.3f; l.speed = 0.5f; }
            else if (id == "electric") { colours (0xffffffff, 0xff40a0ff); l.blend = Blend::add; l.size = 0.3f; l.count = 36; l.band = Band::beat; }
            else if (id == "rays") { colours (0xffffb030, 0xffff3048); l.blend = Blend::add; l.size = 0.3f; }
            break;
        case LayerType::terrain:
            if (id == "mountains") { colours (0xffff3048, 0xff6a2cff); l.count = 40; l.amount = 0.4f; l.react = 0.6f; }
            else if (id == "hills") { colours (0xff406080, 0xffd08060); l.size = 0.4f; l.amount = 0.3f; }
            else { colours (0xffff2d95, 0xff6a2cff); l.size = 0.35f; l.amount = 0.4f; l.speed = 0.4f; }
            break;
        case LayerType::object3d:
            if (id == "blob") { colours (0xff3060ff, 0xffff3048); l.size = 0.4f; }
            else if (id == "torus") { colours (0xffff3048, 0xffffb000); l.size = 0.35f; l.amount = 0.3f; }
            else if (id == "cubes") { colours (0xff20c0ff, 0xffff3048); l.amount = 0.3f; l.react = 0.6f; l.speed = 0.2f; }
            else { colours (0xffff3048, 0xff40c0ff); l.size = 0.3f; l.amount = 0.2f; l.band = Band::level; }
            break;
        case LayerType::feedback:
            colours (0xffffffff, 0xffffffff);
            l.size = 0.3f;
            l.amount = 0.6f;
            if (id == "spin") l.speed = 0.6f;
            if (id == "kaleido") l.count = 24;
            break;
        case LayerType::postprocess:
            colours (0xffffffff, 0xffffffff);
            if (id == "bloom") { l.amount = 0.35f; l.count = 128; }
            else if (id == "blur") { l.size = 0.3f; l.react = 0.0f; }
            else if (id == "rgbshift") { l.amount = 0.2f; l.react = 0.6f; l.band = Band::beat; }
            else if (id == "pixelate") l.size = 0.2f;
            else if (id == "dots") { l.size = 0.3f; l.amount = 0.6f; }
            else if (id == "scanlines") { l.size = 0.3f; l.amount = 0.4f; }
            else if (id == "vignette") { l.amount = 0.7f; }
            else if (id == "color") { l.amount = 0.33f; l.speed = 0.0f; }
            else if (id == "mirror") l.count = 1;
            else if (id == "shake" || id == "glitch" || id == "invert") { l.band = Band::beat; l.react = 0.7f; l.amount = id == "glitch" ? 0.3f : 0.5f; }
            else if (id == "grain") l.amount = 0.3f;
            break;
        default: break;
    }
    return l;
}

const std::vector<Scene>& presets()
{
    static const std::vector<Scene> list = [] {
        std::vector<Scene> out;
        {
            Scene s;
            s.name = "Neon bars";
            s.layers.push_back (make (LayerType::background, 0xff0d0716, 0xff2a0a26, 0.5f, 0.5f, 0.5f, Band::bass, 0.25f));
            auto bars = make (LayerType::bars, 0xffff2d55, 0xff7a3cff, 0.5f, 0.82f, 0.55f, Band::level, 0.8f);
            bars.count = 64;
            bars.mirror = true;
            bars.amount = 0.3f;
            s.layers.push_back (bars);
            auto wave = make (LayerType::waveform, 0xccffffff, 0x00ffffff, 0.5f, 0.4f, 0.22f, Band::level, 0.5f);
            wave.amount = 0.25f;
            wave.additive = true;
            s.layers.push_back (wave);
            auto flash = make (LayerType::flash, 0xffff2d55, 0xffff2d55, 0.5f, 0.5f, 0.5f, Band::beat, 0.35f);
            flash.opacity = 0.35f;
            s.layers.push_back (flash);
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Pulse circle";
            s.layers.push_back (make (LayerType::background, 0xff07070c, 0xff141428, 0.5f, 0.5f, 0.5f, Band::bass, 0.2f));
            auto rings = make (LayerType::rings, 0xffff3048, 0x00ff3048, 0.5f, 0.5f, 0.6f, Band::beat, 0.8f);
            rings.count = 6;
            rings.additive = true;
            s.layers.push_back (rings);
            auto circle = make (LayerType::circle, 0xffff3048, 0xffffb000, 0.5f, 0.5f, 0.32f, Band::level, 0.8f);
            circle.count = 96;
            circle.speed = 0.15f;
            circle.additive = true;
            s.layers.push_back (circle);
            auto parts = make (LayerType::particles, 0xffffd0d8, 0x00ff3048, 0.5f, 0.5f, 0.25f, Band::bass, 0.7f);
            parts.count = 160;
            parts.additive = true;
            s.layers.push_back (parts);
            auto text = make (LayerType::text, 0xffffffff, 0xffff3048, 0.5f, 0.5f, 0.35f, Band::beat, 0.4f);
            text.text = "LYNX SHARE";
            text.amount = 0.6f;
            s.layers.push_back (text);
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Starfield";
            s.layers.push_back (make (LayerType::background, 0xff000000, 0xff05081a, 0.5f, 0.5f, 0.5f, Band::bass, 0.0f));
            auto stars = make (LayerType::particles, 0xffffffff, 0xff6aa8ff, 0.5f, 0.5f, 0.12f, Band::level, 0.9f);
            stars.count = 320;
            stars.speed = 0.6f;
            stars.amount = 0.8f;
            stars.additive = true;
            s.layers.push_back (stars);
            auto wave = make (LayerType::waveform, 0xff6aa8ff, 0xffffffff, 0.5f, 0.5f, 0.3f, Band::level, 0.6f);
            wave.amount = 0.4f;
            wave.mirror = true;
            wave.additive = true;
            s.layers.push_back (wave);
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Title card";
            s.layers.push_back (make (LayerType::background, 0xff1a0810, 0xff05050a, 0.5f, 0.5f, 0.5f, Band::bass, 0.3f));
            auto image = make (LayerType::image, 0xffffffff, 0xffffffff, 0.5f, 0.42f, 0.4f, Band::beat, 0.25f);
            s.layers.push_back (image);
            auto title = make (LayerType::text, 0xffffffff, 0xffff3048, 0.5f, 0.72f, 0.16f, Band::beat, 0.25f);
            title.text = "Artist - Song title";
            title.amount = 0.5f;
            s.layers.push_back (title);
            auto bars = make (LayerType::bars, 0xffffffff, 0xffff3048, 0.5f, 0.97f, 0.18f, Band::level, 0.7f);
            bars.count = 96;
            bars.amount = 0.45f;
            s.layers.push_back (bars);
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Scope";
            s.layers.push_back (make (LayerType::background, 0xff020a06, 0xff06140c, 0.5f, 0.5f, 0.5f, Band::bass, 0.0f));
            auto wave = make (LayerType::waveform, 0xff40ff90, 0xff40ff90, 0.5f, 0.5f, 0.7f, Band::level, 0.0f);
            wave.amount = 0.35f;
            wave.additive = true;
            s.layers.push_back (wave);
            out.push_back (s);
        }
        // GPU scenes (round 165e): shader layers; without OpenGL they are drawn simplified
        auto fx = [] (LayerType t, const char* id) { return shaderLayer (t, std::max (0, effectIndex (t, id))); };
        auto black = [] { return make (LayerType::background, 0xff000000, 0xff000000, 0.5f, 0.5f, 0.5f, Band::bass, 0.0f); };
        {
            Scene s;
            s.name = "Retro road";
            s.layers.push_back (black());
            s.layers.push_back (fx (LayerType::terrain, "road"));
            auto title = make (LayerType::text, 0xffffffff, 0xffff2d95, 0.5f, 0.87f, 0.11f, Band::beat, 0.3f);
            title.text = "LYNX SHARE";
            title.amount = 0.7f;
            s.layers.push_back (title);
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            auto lines = fx (LayerType::postprocess, "scanlines");
            lines.amount = 0.25f;
            s.layers.push_back (lines);
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Spectrum mountains";
            s.layers.push_back (black());
            s.layers.push_back (fx (LayerType::terrain, "mountains"));
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            s.layers.push_back (fx (LayerType::postprocess, "vignette"));
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Reactive sphere";
            s.layers.push_back (black());
            auto clouds = fx (LayerType::canvas, "clouds");
            clouds.colourA = 0xff0a0c30;
            clouds.colourB = 0xff3a1060;
            clouds.opacity = 0.8f;
            s.layers.push_back (clouds);
            auto rings = make (LayerType::rings, 0xffff3048, 0x00ff3048, 0.5f, 0.5f, 0.7f, Band::beat, 0.8f);
            rings.count = 6;
            rings.additive = true;
            s.layers.push_back (rings);
            s.layers.push_back (fx (LayerType::object3d, "blob"));
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            s.layers.push_back (fx (LayerType::postprocess, "vignette"));
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Kaleido feedback";
            s.layers.push_back (black());
            auto bars = make (LayerType::bars, 0xffff2d55, 0xff7a3cff, 0.5f, 0.75f, 0.45f, Band::level, 0.8f);
            bars.count = 48;
            bars.mirror = true;
            s.layers.push_back (bars);
            s.layers.push_back (fx (LayerType::feedback, "kaleido"));
            s.layers.push_back (fx (LayerType::postprocess, "rgbshift"));
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Plasma tunnel";
            s.layers.push_back (black());
            s.layers.push_back (fx (LayerType::canvas, "tunnel"));
            auto circle = make (LayerType::circle, 0xffffffff, 0xffff3048, 0.5f, 0.5f, 0.22f, Band::level, 0.8f);
            circle.count = 120;
            circle.additive = true;
            s.layers.push_back (circle);
            s.layers.push_back (fx (LayerType::feedback, "zoom"));
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Cube city";
            auto sky = fx (LayerType::canvas, "clouds");
            sky.colourA = 0xff050818;
            sky.colourB = 0xff1a2a50;
            s.layers.push_back (black());
            s.layers.push_back (sky);
            s.layers.push_back (fx (LayerType::object3d, "cubes"));
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            s.layers.push_back (fx (LayerType::postprocess, "vignette"));
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Electric storm";
            s.layers.push_back (make (LayerType::background, 0xff02030a, 0xff0a0820, 0.5f, 0.5f, 0.5f, Band::bass, 0.2f));
            s.layers.push_back (fx (LayerType::canvas, "electric"));
            s.layers.push_back (fx (LayerType::feedback, "drift"));
            auto flash = make (LayerType::flash, 0xff80c0ff, 0xff80c0ff, 0.5f, 0.5f, 0.5f, Band::beat, 0.3f);
            flash.opacity = 0.3f;
            s.layers.push_back (flash);
            s.layers.push_back (fx (LayerType::postprocess, "bloom"));
            out.push_back (s);
        }
        {
            Scene s;
            s.name = "Neon CRT";
            s.layers.push_back (make (LayerType::background, 0xff0d0716, 0xff2a0a26, 0.5f, 0.5f, 0.5f, Band::bass, 0.25f));
            auto bars = make (LayerType::bars, 0xffff2d55, 0xff7a3cff, 0.5f, 0.85f, 0.5f, Band::level, 0.8f);
            bars.count = 64;
            s.layers.push_back (bars);
            auto wave = make (LayerType::waveform, 0xccffffff, 0x00ffffff, 0.5f, 0.4f, 0.22f, Band::level, 0.5f);
            wave.amount = 0.25f;
            s.layers.push_back (wave);
            s.layers.push_back (fx (LayerType::postprocess, "rgbshift"));
            s.layers.push_back (fx (LayerType::postprocess, "scanlines"));
            s.layers.push_back (fx (LayerType::postprocess, "vignette"));
            s.layers.push_back (fx (LayerType::postprocess, "grain"));
            out.push_back (s);
        }
        return out;
    }();
    return list;
}

Scene defaultScene()
{
    return presets().front();
}

std::optional<Scene> fromZge (const std::vector<std::uint8_t>& state, std::vector<std::string>* unmapped)
{
    const std::string all (state.begin(), state.end());
    const auto start = all.find ("[General]");
    if (start == std::string::npos)
        return std::nullopt;
    // sections → key/value
    std::map<std::string, std::map<std::string, std::string>> ini;
    {
        std::istringstream in (all.substr (start));
        std::string line, section;
        while (std::getline (in, line))
        {
            while (! line.empty() && (line.back() == '\r' || line.back() == '\0'))
                line.pop_back();
            if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            {
                section = line.substr (1, line.size() - 2);
                continue;
            }
            const auto eq = line.find ('=');
            if (eq != std::string::npos && ! section.empty())
                ini[section][line.substr (0, eq)] = line.substr (eq + 1);
        }
    }
    Scene s;
    s.name = "ZGameEditor Visualizer";
    if (auto it = ini.find ("Video export"); it != ini.end())
    {
        const auto w = std::atoi (it->second["VideoW"].c_str()), h = std::atoi (it->second["VideoH"].c_str());
        if (w >= 160 && h >= 120 && w <= 7680 && h <= 4320)
        {
            s.width = w & ~1;
            s.height = h & ~1;
        }
        const auto fps = std::atoi (it->second["VideoRenderFps"].c_str());
        if (fps > 0 && fps <= 120)
            s.fps = fps;
    }
    // the user's text lines ("a","b" or a bare value) and image files
    auto list = [] (const std::string& v) {
        std::vector<std::string> out;
        std::string cur;
        bool quoted = false;
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            const auto c = v[i];
            if (c == '"')
            {
                if (quoted && i + 1 < v.size() && v[i + 1] == '"')
                {
                    cur += '"';
                    ++i;
                }
                else
                    quoted = ! quoted;
            }
            else if (c == ',' && ! quoted)
            {
                out.push_back (cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (! cur.empty() || ! out.empty())
            out.push_back (cur);
        return out;
    };
    const auto& user = ini["UserContent"];
    std::string text;
    if (auto it = user.find ("Text"); it != user.end())
        for (const auto& line : list (it->second))
            if (line != "This is the default text.")
                text += (text.empty() ? "" : "\n") + line;
    std::vector<std::string> images;
    if (auto it = user.find ("Images"); it != user.end())
        for (const auto& path : list (it->second))
            if (path.find (']') == std::string::npos && ! path.empty()) // [presetpath] / [plugpath]: FL's own content
                images.push_back (path);

    // the layers in FL's drawing order
    std::vector<int> order;
    {
        const auto& general = ini["General"];
        const auto count = std::atoi (general.count ("LayerCount") ? general.at ("LayerCount").c_str() : "0");
        if (auto it = general.find ("LayerOrder"); it != general.end())
            for (const auto& v : list (it->second))
                order.push_back (std::atoi (v.c_str()));
        if (order.empty())
            for (int i = 0; i < count; ++i)
                order.push_back (i);
    }
    auto has = [] (const std::string& app, const char* word) { return app.find (word) != std::string::npos; };
    for (const auto index : order)
    {
        auto it = ini.find ("AppSet" + std::to_string (index));
        if (it == ini.end())
            continue;
        const auto app = it->second["App"];
        if (app.empty() || app == "(none)" || it->second["Enabled"] == "0")
            continue;
        // the effect's parameters (0..1)
        std::vector<float> p;
        {
            std::istringstream values (it->second["FParamValues"]);
            std::string v;
            while (std::getline (values, v, ','))
                p.push_back ((float) std::atof (v.c_str()));
        }
        auto param = [&p] (std::size_t i, float fallback) { return i < p.size() && std::isfinite (p[i]) ? p[i] : fallback; };
        // most ZGE effects start with Alpha, Hue, Saturation, Lightness: HSV (hue, saturation, 1 − lightness), alpha 1 − Alpha
        auto zgeColour = [&param] (std::size_t at = 0) {
            const auto alpha = 1.0f - std::clamp (param (at, 0.0f), 0.0f, 1.0f);
            return hsvArgb (param (at + 1, 0.0f), param (at + 2, 0.0f), 1.0f - std::clamp (param (at + 3, 0.0f), 0.0f, 1.0f), alpha);
        };
        auto shader = [] (LayerType t, const char* id) { return shaderLayer (t, std::max (0, effectIndex (t, id))); };
        const auto name = app.substr (app.find_last_of ("\\.") == std::string::npos ? 0 : app.find_last_of ("\\.") + 1);
        auto named = [&name] (std::initializer_list<const char*> words) {
            for (const auto* w : words)
                if (name.find (w) != std::string::npos)
                    return true;
            return false;
        };
        Layer l;
        bool ok = true;
        if (has (app, "SolidColor"))
        {
            const auto c = zgeColour();
            if ((c >> 24) < 8)
                continue; // a transparent colour draws nothing
            l = make (LayerType::background, c | 0xff000000u, c | 0xff000000u, 0.5f, 0.5f, 0.5f, Band::bass, 0.0f);
            l.opacity = (float) (c >> 24) / 255.0f;
        }
        else if (has (app, "FourCornerGradient"))
        {
            // the top-left and bottom-right corners (hue, saturation, value)
            l = make (LayerType::background, hsvArgb (param (2, 0.0f), param (3, 0.0f), param (4, 0.2f), 1.0f),
                      hsvArgb (param (11, 0.0f), param (12, 0.0f), param (13, 0.1f), 1.0f), 0.5f, 0.5f, 0.5f, Band::bass, 0.1f);
            l.opacity = std::clamp (param (1, 1.0f), 0.0f, 1.0f);
        }
        else if (has (app, "ItsFullOfStars"))
            l = shader (LayerType::canvas, "warp");
        else if (has (app, "FogMachine"))
            l = shader (LayerType::canvas, "clouds");
        else if (has (app, "Retro Road") || has (app, "Background\\Grid"))
            l = shader (LayerType::terrain, "road");
        else if (has (app, "Background\\"))
            l = make (LayerType::background, 0xff0d0716, 0xff2a0a26, 0.5f, 0.5f, 0.5f, Band::bass, 0.25f);
        else if (has (app, "Peak Effects\\Polar") || has (app, "FreqRing") || has (app, "HUD Graph Polar") || has (app, "HUD Meter Radial"))
        {
            l = make (LayerType::circle, 0xffff3048, 0xffffb000, 0.5f, 0.5f, 0.3f, Band::level, 0.8f);
            l.count = 96;
            if (has (app, "Peak Effects\\Polar") && p.size() >= 7)
            {
                l.colourA = zgeColour();
                l.size = std::clamp (param (4, 0.3f), 0.05f, 1.0f);
                l.x = param (5, 0.5f);
                l.y = 1.0f - param (6, 0.5f);
            }
        }
        else if (has (app, "Reactive Sphere"))
            l = shader (LayerType::object3d, "blob");
        else if (has (app, "Audio Plane") || has (app, "Peakmap"))
            l = shader (LayerType::terrain, "mountains");
        else if (has (app, "WaveSimple") || has (app, "StereoWaveForm") || has (app, "JoyDividers") || has (app, "Oscilloscope")
                 || has (app, "Youlean Waveform") || has (app, "VectorScope"))
            l = make (LayerType::waveform, 0xffffffff, 0xffffffff, 0.5f, 0.5f, 0.3f, Band::level, 0.5f);
        else if (has (app, "Peak Effects\\") || has (app, "HUD Graph Linear"))
        {
            l = make (LayerType::bars, 0xffff2d55, 0xff7a3cff, 0.5f, 0.85f, 0.5f, Band::level, 0.8f);
            l.count = 64;
            if (has (app, "Peak Effects\\Linear") && p.size() >= 8)
            {
                l.colourA = zgeColour() | 0xff000000u;
                l.opacity = (float) (zgeColour() >> 24) / 255.0f;
                l.size = std::clamp (param (4, 0.5f), 0.05f, 1.0f);
                l.y = 1.0f - param (6, 0.15f);
                l.count = std::clamp ((int) std::lround (8.0f + param (7, 0.25f) * 120.0f), 8, 256);
            }
        }
        else if (has (app, "Particles\\") || has (app, "ReactiveMob") || has (app, "fLuids") || has (app, "ReactiveFlow"))
        {
            l = make (LayerType::particles, 0xffffffff, 0xff6aa8ff, 0.5f, 0.5f, 0.15f, Band::bass, 0.7f);
            l.count = 200;
            l.additive = true;
        }
        else if (has (app, "Text\\") || has (app, "HUD Text"))
        {
            l = make (LayerType::text, 0xffffffff, 0xffff3048, 0.5f, 0.5f, 0.2f, Band::beat, 0.3f);
            l.text = text.empty() ? std::string ("LYNX SHARE") : text;
            if (has (app, "TextTrueType") && p.size() >= 7)
            {
                const auto c = zgeColour();
                l.colourA = c | 0xff000000u;
                l.opacity = std::max (0.05f, (float) (c >> 24) / 255.0f);
                l.x = param (5, 0.5f);
                l.y = 1.0f - param (6, 0.5f);
            }
        }
        else if (has (app, "Image effects\\") || has (app, "HUD Image"))
        {
            l = make (LayerType::image, 0xffffffff, 0xffffffff, 0.5f, 0.5f, 0.6f, Band::beat, 0.2f);
            const auto imageIndex = std::atoi (it->second["ImageIndex"].c_str());
            if (imageIndex >= 0 && imageIndex < (int) images.size())
                l.path = images[(std::size_t) imageIndex];
            else
                ok = false; // one of FL's own pictures: not ours to copy
            if (ok && has (app, "Image effects\\Image") && p.size() >= 7)
            {
                l.size = std::clamp (param (4, 0.6f), 0.05f, 1.0f);
                l.x = param (5, 0.5f);
                l.y = 1.0f - param (6, 0.5f);
                l.opacity = 1.0f - std::clamp (param (0, 0.0f), 0.0f, 1.0f);
            }
        }
        // ---- the GPU layers (round 165e) ----
        else if (has (app, "Feedback\\"))
            l = shader (LayerType::feedback, named ({ "Kaleido" }) ? "kaleido" : named ({ "WormHole" }) ? "spin" : named ({ "WarpBack" }) ? "warp"
                                                : named ({ "BoxedIn" }) ? "echo" : "zoom");
        else if (has (app, "FrameBlur"))
            l = shader (LayerType::feedback, "echo");
        else if (has (app, "Bloom") || has (app, "Blooming") || has (app, "Starburst") || has (app, "Whitehole"))
        {
            l = shader (LayerType::postprocess, "bloom");
            if (has (app, "Youlean Bloom") && p.size() >= 5)
            {
                l.amount = std::clamp (param (0, 0.35f), 0.0f, 1.0f);
                l.count = std::clamp ((int) std::lround ((0.85f - std::clamp (param (1, 0.3f), 0.0f, 0.95f)) / 0.8f * 512.0f), 1, 512);
                l.size = std::clamp (param (2, 0.5f), 0.0f, 1.0f);
                l.opacity = std::clamp (param (4, 1.0f), 0.0f, 1.0f);
            }
            else if (has (app, "Blooming") && p.size() >= 8)
                l.amount = std::clamp (param (7, 0.35f), 0.0f, 1.0f);
        }
        else if (has (app, "AudioShake") || has (app, "Audio Shake") || has (app, "Handheld"))
            l = shader (LayerType::postprocess, "shake");
        else if (has (app, "Youlean Blur") || has (app, "Motion Blur") || has (app, "Postprocess\\Blur") || has (app, "Linear Blur") || has (app, "Smudge")
                 || has (app, "Watercolor") || has (app, "Raindrops"))
        {
            l = shader (LayerType::postprocess, "blur");
            if (p.size() >= 1)
                l.size = std::clamp (param (0, 0.3f), 0.0f, 1.0f);
        }
        else if (has (app, "Color Correction") || has (app, "Color Shaper") || has (app, "Fast HSV") || has (app, "Luminosity") || has (app, "ColorCyclePalette"))
        {
            l = shader (LayerType::postprocess, "color");
            if (has (app, "Youlean Color Correction") && p.size() >= 5)
            {
                l.amount = std::clamp ((param (2, 0.5f) * 2.0f - 0.5f) / 1.5f, 0.0f, 1.0f); // contrast (0.5 = as is)
                l.size = std::clamp (param (4, 0.5f), 0.0f, 1.0f);                          // saturation
            }
            if (has (app, "ColorCyclePalette"))
                l.speed = 0.5f;
        }
        else if (has (app, "Dot Matrix") || has (app, "Point Cloud") || has (app, "Postprocess\\Ascii"))
            l = shader (LayerType::postprocess, "dots");
        else if (has (app, "Dither") || has (app, "Fast Static"))
            l = shader (LayerType::postprocess, "grain");
        else if (has (app, "Inverter"))
            l = shader (LayerType::postprocess, "invert");
        else if (has (app, "Glitch"))
            l = shader (LayerType::postprocess, "glitch");
        else if (has (app, "Edge Detect") || has (app, "Notebook Drawings"))
            l = shader (LayerType::postprocess, "edges");
        else if (has (app, "RGB Shift"))
            l = shader (LayerType::postprocess, "rgbshift");
        else if (has (app, "ScanLines"))
            l = shader (LayerType::postprocess, "scanlines");
        else if (has (app, "Vignette"))
        {
            l = shader (LayerType::postprocess, "vignette");
            if (p.size() >= 5)
            {
                l.amount = 1.0f - std::clamp (param (0, 0.3f), 0.0f, 1.0f);
                l.size = std::clamp (param (4, 0.5f), 0.0f, 1.0f);
            }
        }
        else if (has (app, "Kaleidoscope"))
            l = shader (LayerType::postprocess, "kaleido");
        else if (has (app, "Pixelate"))
            l = shader (LayerType::postprocess, "pixelate");
        else if (has (app, "Terrain\\") || has (app, "Physics\\Heightfield"))
            l = named ({ "CubesAndSpheres" }) ? shader (LayerType::object3d, "cubes")
              : named ({ "Perturbed Grid", "Terrain Lattice", "Heightfield" }) ? shader (LayerType::terrain, "mountains")
                                                                              : shader (LayerType::terrain, "hills");
        else if (has (app, "Object Arrays\\") || has (app, "Physics\\") || has (app, "MatCap"))
            l = named ({ "Rings", "Mobius Weave", "Filaments" }) ? shader (LayerType::object3d, "rings")
              : named ({ "Cube", "Block", "Cubic", "Columns", "Cage", "8x8x8" }) ? shader (LayerType::object3d, "cubes")
                                                                                 : shader (LayerType::object3d, "blob");
        else if (has (app, "Scenes\\"))
            l = named ({ "Mandelbulb", "Polyhedra", "Musicball", "Jewels", "Rhodium", "Mobius", "Gears", "Lantern" }) ? shader (LayerType::object3d, "blob")
              : named ({ "Alps", "Wasteland", "Postcard", "Terrain", "Debris" }) ? shader (LayerType::terrain, "hills")
              : named ({ "Cloud", "Balloon" }) ? shader (LayerType::canvas, "clouds")
              : named ({ "Hex", "Industrial" }) ? shader (LayerType::object3d, "cubes")
                                                : shader (LayerType::canvas, "tunnel");
        else if (has (app, "Clouds\\"))
            l = shader (LayerType::canvas, "clouds");
        else if (has (app, "Tunnel\\"))
            l = named ({ "Torus", "Tube" }) ? shader (LayerType::object3d, "rings") : shader (LayerType::canvas, "tunnel");
        else if (has (app, "Canvas effects\\"))
            l = named ({ "Voronoi", "Cellular", "Hexagonal", "Rounded", "Contours" }) ? shader (LayerType::canvas, "cells")
              : named ({ "Electric", "DarkSpark", "Flaring", "StarTaser" }) ? shader (LayerType::canvas, "electric")
              : named ({ "Rain" }) ? shader (LayerType::canvas, "warp")
              : named ({ "Digital Brain", "Stack Trace", "ShimeringCage", "Lattice", "N-gon", "BitPad", "Blocks" }) ? shader (LayerType::canvas, "kaleido")
                                                                                                                      : shader (LayerType::canvas, "plasma");
        else if (has (app, "Patterns\\"))
            l = named ({ "Truchet", "Weave", "Maze", "Pipe" }) ? shader (LayerType::canvas, "kaleido") : shader (LayerType::canvas, "cells");
        else
            ok = false;
        if (! ok)
        {
            if (unmapped != nullptr)
                unmapped->push_back (app);
            continue;
        }
        s.layers.push_back (l);
    }
    // nothing that draws (only effects without a counterpart): LYNX's default scene, so the slot still shows the music
    const auto draws = std::any_of (s.layers.begin(), s.layers.end(), [] (const Layer& l) {
        return l.type != LayerType::background && ! readsBelow (l.type); // (bloom alone over a colour shows no music)
    });
    if (! draws)
    {
        auto fallback = defaultScene();
        fallback.width = s.width;
        fallback.height = s.height;
        fallback.fps = s.fps;
        fallback.name = s.name;
        // (the project's feedback / post-process layers stay, over LYNX's scene)
        for (const auto& l : s.layers)
            if (readsBelow (l.type))
                fallback.layers.push_back (l);
        return fallback;
    }
    if (s.layers.front().type != LayerType::background)
        s.layers.insert (s.layers.begin(), make (LayerType::background, 0xff000000, 0xff000000, 0.5f, 0.5f, 0.5f, Band::bass, 0.0f));
    return s;
}
} // namespace scene

// ---------------------------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------------------------
float Frame::bandValue (scene::Band b) const noexcept
{
    switch (b)
    {
        case scene::Band::level: return level;
        case scene::Band::bass: return bass;
        case scene::Band::mid: return mid;
        case scene::Band::high: return high;
        case scene::Band::beat: return beat;
    }
    return level;
}

void FrameAnalyzer::reset()
{
    frame = {};
    slowBass = bassAverage = 0.0f;
    waveGain = 1.0f;
    lastBeat = -10.0;
}

const Frame& FrameAnalyzer::analyze (const float* left, const float* right, int n, double sampleRate, double time, double dt,
                                     const std::array<float, shownParams>& params)
{
    constexpr int size = 2048;
    frame.time = time;
    frame.dt = dt = std::clamp (dt, 1.0e-4, 0.5);
    frame.params = params;
    const auto offsetDb = (params[(std::size_t) visualizer::sensitivity] - 0.5f) * 36.0f;
    mono.assign ((std::size_t) size, 0.0f);
    const auto have = std::min (n, size);
    for (int i = 0; i < have; ++i)
        mono[(std::size_t) (size - have + i)] = 0.5f * (left[n - have + i] + right[n - have + i]);

    // spectrum: 64 log bands, pink-weighted so the treble shows, mapped from a 60 dB window to 0..1
    analysis::spectrumDb (mono.data(), size, db, work);
    std::array<float, 64> bands {};
    analysis::logBands (db, sampleRate, size, 30.0, 16000.0, bands.data(), (int) bands.size(), 3.0);
    const auto fall = (float) (dt * 2.2); // the bars' fall: about half a second from full to nothing
    float bass = 0.0f, mid = 0.0f, high = 0.0f;
    int nb = 0, nm = 0, nh = 0;
    for (std::size_t b = 0; b < bands.size(); ++b)
    {
        const auto v = std::clamp ((bands[b] + offsetDb + 62.0f) / 60.0f, 0.0f, 1.0f);
        frame.spectrum[b] = std::max (v, frame.spectrum[b] - fall);
        const auto hz = 30.0 * std::exp (std::log (16000.0 / 30.0) * ((double) b + 0.5) / (double) bands.size());
        if (hz < 160.0)
        {
            bass += v;
            ++nb;
        }
        else if (hz < 2500.0)
        {
            mid += v;
            ++nm;
        }
        else
        {
            high += v;
            ++nh;
        }
    }
    bass /= (float) std::max (1, nb);
    mid /= (float) std::max (1, nm);
    high /= (float) std::max (1, nh);

    double sum = 0.0;
    for (int i = size - 1024; i < size; ++i)
        sum += (double) mono[(std::size_t) i] * mono[(std::size_t) i];
    const auto levelNow = std::clamp ((toDb (std::sqrt (sum / 1024.0)) + offsetDb + 54.0f) / 54.0f, 0.0f, 1.0f);
    auto follow = [&] (float& v, float target) { v = target > v ? v + (target - v) * 0.7f : std::max (target, v - fall); };
    follow (frame.level, levelNow);
    follow (frame.bass, bass);
    follow (frame.mid, mid);
    follow (frame.high, high);

    // beats: the bass jumps above its recent average (the knob sets how far) at most ~5 times a second
    const auto threshold = 1.12f + params[(std::size_t) visualizer::beatThreshold] * 0.9f;
    const auto avgCoeff = (float) (1.0 - std::exp (-dt / 1.2));
    frame.onset = bass > 0.08f && bass > bassAverage * threshold && bass > slowBass + 0.02f && time - lastBeat > 0.19;
    bassAverage += (bass - bassAverage) * avgCoeff;
    slowBass = bass;
    if (frame.onset)
    {
        lastBeat = time;
        frame.beat = 1.0f;
        ++frame.beats;
    }
    else
        frame.beat *= (float) std::exp (-dt / 0.22);

    // the waveform: the newest 1024 samples from a rising zero crossing, 256 points
    {
        constexpr int span = 1024;
        std::vector<float> l ((std::size_t) size), r ((std::size_t) size);
        for (int i = 0; i < have; ++i)
        {
            l[(std::size_t) (size - have + i)] = left[n - have + i];
            r[(std::size_t) (size - have + i)] = right[n - have + i];
        }
        const auto start = analysis::triggerPoint (mono.data(), size, span);
        float peak = 0.0f;
        for (int i = start; i < std::min (size, start + span); ++i)
            peak = std::max ({ peak, std::abs (l[(std::size_t) i]), std::abs (r[(std::size_t) i]) });
        // a slow automatic gain, so a quiet mix still draws a wave (up to +24 dB; a louder peak pulls it down at once)
        const auto wanted = std::clamp (0.7f / std::max (peak, 1.0e-4f), 1.0f, 16.0f);
        waveGain = wanted < waveGain ? wanted : waveGain + (wanted - waveGain) * (float) (1.0 - std::exp (-dt / 1.5));
        for (int p = 0; p < 256; ++p)
        {
            const auto k = (std::size_t) std::min (size - 1, start + p * span / 256);
            frame.waveL[(std::size_t) p] = l[k] * waveGain;
            frame.waveR[(std::size_t) p] = r[k] * waveGain;
        }
    }
    return frame;
}

float ParticleField::next()
{
    seed = seed * 1664525u + 1013904223u;
    return (float) (seed >> 8) / 16777216.0f;
}

void ParticleField::step (const Frame& f, const scene::Layer& layer, float aspect)
{
    const auto dt = (float) f.dt;
    const auto band = f.bandValue (layer.band);
    const auto speedKnob = (float) std::pow (4.0, (double) f.params[(std::size_t) visualizer::speed] * 2.0 - 1.0);
    const auto speed = (0.05f + layer.speed * 0.6f) * (0.35f + band * layer.react * 1.6f) * speedKnob;
    const auto cap = std::clamp (layer.count, 1, 512) * 2;
    for (auto& p : list)
    {
        p.x += p.vx * speed * dt;
        p.y += p.vy * speed * dt * aspect;
        p.life -= dt / (1.2f + layer.speed * 1.8f);
    }
    list.erase (std::remove_if (list.begin(), list.end(),
                                [] (const Particle& p) { return p.life <= 0.0f || p.x < -0.2f || p.x > 1.2f || p.y < -0.2f || p.y > 1.2f; }),
                list.end());
    // a steady stream as loud as the band, a burst on each beat
    auto spawn = (float) layer.count * (0.2f + layer.amount) * dt * (0.15f + band * layer.react * 2.0f);
    if (f.onset)
        spawn += (float) layer.count * 0.25f * (0.3f + layer.react);
    auto whole = (int) spawn;
    if (next() < spawn - (float) whole)
        ++whole;
    for (int i = 0; i < whole && (int) list.size() < cap; ++i)
    {
        Particle p;
        const auto angle = next() * 6.2831853f;
        const auto v = 0.3f + next() * 0.9f;
        p.x = layer.x + (next() - 0.5f) * 0.02f;
        p.y = layer.y + (next() - 0.5f) * 0.02f;
        p.vx = std::cos (angle) * v;
        p.vy = std::sin (angle) * v;
        p.life = 0.6f + next() * 0.4f;
        p.size = 0.3f + next() * 0.7f;
        p.hue = next();
        list.push_back (p);
    }
}
} // namespace lynx::visualfx
