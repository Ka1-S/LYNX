// Sytrus (6-operator FM/RM + 3 filters) approximated by the 3-oscillator synth.
// Format and conversions in docs/analise/REA_rodada11.md §1-3 (clean-room: behavior only).

#include "core/FlSynths.h"
#include "core/Inflate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>

namespace lynx
{
namespace
{
struct Reader
{
    const std::uint8_t* data;
    std::size_t size, pos = 0;
    bool failed = false;

    const std::uint8_t* raw (std::size_t n)
    {
        if (failed || n > size - pos)
        {
            failed = true;
            return nullptr;
        }
        const auto* p = data + pos;
        pos += n;
        return p;
    }

    template <typename T>
    T get()
    {
        T v {};
        if (const auto* p = raw (sizeof (T)))
            std::memcpy (&v, p, sizeof (T));
        return v;
    }

    std::int32_t i32() { return get<std::int32_t>(); }
    std::uint8_t u8() { return get<std::uint8_t>(); }
};

struct Envelope
{
    std::vector<std::pair<double, double>> points; // (Δt, level)
    std::vector<float> tensions;                   // per point (curve of the destination point)
    std::vector<int> curves;                       // curve type per point (0 = single curve)
    int flags = 0;
    bool on = false;
    int sustain = -1;
    int decay = -1; // decay point (Envelope Controller ADSR knobs)
};

struct Articulator
{
    Envelope env, lfo;
    std::array<std::int32_t, 4> knobs { 128, 128, 0, 128 }; // envelope A, D, S, R
    std::int32_t lfoSpeed = 0;
    float envDepth = 1.0f, lfoDepth = 0.0f;
};

struct Unit
{
    std::array<std::int32_t, 10> p {};
    // operators
    double ratio = 1.0, offsetHz = 0.0;
    std::vector<float> harmonics, phases;
    int shape = 0;
    std::array<std::int32_t, 5> form { 0, 0, 0, 0, 128 }; // Shape, Tension, Skew, Sine shaper, Pre-filter
    bool half = false, even = false, absolute = false, gibbs = false;
    // filters
    int filterKind = 3;
    std::vector<Articulator> art;
};

struct State
{
    int version = 0;
    std::array<std::int32_t, 200> main {};
    std::array<std::uint8_t, 162> mute {};
    bool porta = false, altUnisonPitch = false;
    std::array<Unit, 10> units;

    int cell (int layer, int row, int col) const
    {
        const auto i = (std::size_t) (layer * 81 + row * 9 + col);
        return mute[i] != 0 ? 0 : main[3 + i];
    }
};

bool readEnvelope (Reader& r, Envelope& e)
{
    const auto kind = r.i32(), version = r.i32(), n = r.i32();
    if (r.failed || n < 0 || n > 5000 || version < 0 || version > 3)
        return false;

    e.points.clear();
    e.tensions.clear();
    e.curves.clear();
    for (int k = 0; k < n; ++k)
    {
        double x = 0.0, y = 0.0;
        float tension = 0.0f;
        int curve = 0;
        if (version < 2)
        {
            x = kind == 6 ? r.get<double>() : (double) r.get<float>();
            y = r.get<float>();
            r.raw (version < 1 ? 4 : 8);
        }
        else
        {
            x = r.get<double>();
            y = r.get<double>();
            tension = r.get<float>();
            curve = r.u8();
            r.raw (3);
        }
        e.tensions.push_back (std::isfinite (tension) ? std::clamp (tension, -1.0f, 1.0f) : 0.0f);
        e.curves.push_back (curve);
        if (r.failed)
            return false;
        e.points.emplace_back (std::isfinite (x) ? std::clamp (x, 0.0, 1000.0) : 0.0, std::isfinite (y) ? std::clamp (y, 0.0, 1.0) : 0.0);
    }

    e.flags = r.i32();
    e.on = r.i32() != 0;
    e.decay = r.i32();
    r.i32(); // loop start
    e.sustain = r.i32();
    return ! r.failed;
}

bool readArticulator (Reader& r, Articulator& a, int maps = 6)
{
    r.i32();
    r.i32();
    a.envDepth = r.get<float>();
    a.lfoDepth = r.get<float>();
    if (! readEnvelope (r, a.env))
        return false;
    for (auto& k : a.knobs)
        k = r.i32();

    // the LFO version decides whether there is one extra int: read the version before consuming the envelope
    const auto lfoVersionAt = r.pos + 4;
    if (! readEnvelope (r, a.lfo))
        return false;
    std::int32_t lfoVersion = 0;
    std::memcpy (&lfoVersion, r.data + lfoVersionAt, 4);
    a.lfoSpeed = r.i32();
    r.raw (12);
    if (lfoVersion >= 3)
        r.i32();

    Envelope map;
    for (int k = 0; k < maps; ++k) // maps (Sytrus: keyboard, velocity, X, Y, random, unison)
        if (! readEnvelope (r, map))
            return false;
    return ! r.failed;
}

bool parse (const std::vector<std::uint8_t>& blob, State& s)
{
    if (blob.size() < 8)
        return false;

    std::uint32_t version = 0;
    std::memcpy (&version, blob.data(), 4);
    if (version > 0xffff)
        version >>= 16;
    if (version > 64)
        return false;
    s.version = (int) version;
    const auto v = s.version;

    std::vector<std::uint8_t> body;
    if (v >= 5)
    {
        auto inflated = inflateZlib (blob.data() + 4, blob.size() - 4, false, 4u << 20);
        if (! inflated)
            return false;
        body = std::move (*inflated);
    }
    else
        body.assign (blob.begin() + 4, blob.end());

    Reader r { body.data(), body.size() };
    const auto mainSize = (std::size_t) (962 - (v < 1 ? 9 : 0) - (v < 2 ? 162 : 0));
    const auto* main = r.raw (mainSize);
    if (main == nullptr)
        return false;
    std::memcpy (s.main.data(), main, std::min (mainSize / 4 * 4, s.main.size() * 4));
    if (v >= 2)
        std::memcpy (s.mute.data(), main + 0x320, s.mute.size());

    if (v > 3)
        r.i32();
    if (v > 5)
        r.u8();
    if (v > 6)
        r.u8();
    if (v > 12)
        r.u8();
    if (v > 13)
    {
        const auto k = (std::size_t) (1 + (v > 14 ? 4 : 0) + (v > 17 ? 3 : 0) + (v > 18 ? 1 : 0));
        if (const auto* flags = r.raw (k))
        {
            s.porta = k > 1 && flags[1] != 0;
            s.altUnisonPitch = k > 8 && flags[8] != 0;
        }
    }
    r.raw (3 * 23); // delays
    if (v >= 10)
        r.raw (0x25 + (v > 10 ? 1 : 0)); // reverb
    if (v > 2)
    {
        r.i32();
        r.raw (3 * 4 * (v > 13 ? 4 : 3)); // EQ
    }
    // info text
    if (v < 0x17)
    {
        const auto n = r.i32();
        if (n < 0)
            return false;
        r.raw ((std::size_t) n);
    }
    else
    {
        std::size_t n = r.u8();
        if (n == 255)
        {
            const auto big = r.i32();
            if (big < 0)
                return false;
            n = (std::size_t) big;
        }
        r.raw (2 * n);
    }

    for (int u = 0; u < 10 && ! r.failed; ++u)
    {
        auto& unit = s.units[(std::size_t) u];
        for (auto& p : unit.p)
            p = r.i32();

        if (u < 6)
        {
            const std::size_t blockSize = v < 8 ? 0xe7 : v < 0x11 ? 0x428 : 0x828;
            const auto* op = r.raw (blockSize);
            if (op == nullptr)
                return false;
            std::int32_t ratio = 0, offset = 0;
            std::memcpy (&ratio, op, 4);
            std::memcpy (&offset, op + 4, 4);
            unit.ratio = ratio / 20000.0;
            unit.offsetHz = offset / 100.0;
            const std::size_t harmonicsAt = v < 8 ? 8 : 0xc, count = v < 8 ? 24 : v < 0x11 ? 128 : 256;
            unit.harmonics.resize (count);
            std::memcpy (unit.harmonics.data(), op + harmonicsAt, count * 4);
            if (v >= 0x11)
            {
                std::memcpy (&unit.shape, op + 0x80c, 4);
                std::memcpy (unit.form.data(), op + 0x80c, 5 * 4);
                unit.half = op[0x820] != 0;
                unit.even = op[0x821] != 0;
                unit.absolute = op[0x822] != 0;
                unit.phases.resize (count);
                std::memcpy (unit.phases.data(), op + 0x40c, count * 4);
            }
        }
        else if (u < 9)
        {
            const auto* f = r.raw (v < 9 ? 0x29 : 0x30);
            if (f == nullptr)
                return false;
            if (v >= 9)
                std::memcpy (&unit.filterKind, f + 0x2c, 4);
            Envelope shaper;
            if (! readEnvelope (r, shaper))
                return false;
            if (v >= 12)
                r.u8();
        }

        unit.art.resize (u < 6 ? 6 : u < 9 ? 8 : 2);
        for (auto& a : unit.art)
            if (! readArticulator (r, a))
                return false;
    }

    return ! r.failed;
}

double curve (int v, double full = 256.0)
{
    const auto g = (std::pow (11.0, std::min (std::abs (v) / full, 2.0)) - 1.0) / 10.0;
    return v < 0 ? -g : g;
}

// Operator waveform (REA_rodada23 §2): 16 384-sample table with the base shape B(Shape) on each partial,
// Half/Even, Skew and Sine shaper, peak normalization, Tension, Absolute and Pre-filter; the spectrum (harmonics
// 1…256, sine amplitude and phase in cycles) comes from an FFT. False if the harmonics table is empty.
bool operatorSpectrum (const Unit& op, std::array<float, SynthSettings::spectrumSize>& amplitude, std::array<float, SynthSettings::spectrumSize>& phase)
{
    constexpr int size = 16384;
    constexpr double pi = 3.141592653589793;
    if (op.harmonics.empty())
        return false;
    const auto x = std::clamp (op.form[0], 0, 512) / 128.0;
    double m = 1.0, d = 0.0, t = 0.0, pulse = 0.0;
    if (x <= 1.0)
        m = x, d = 0.5;
    else if (x <= 2.0)
        d = (2.0 - x) / 2.0;
    else if (x <= 3.0)
        t = x - 2.0;
    else
        t = 1.0, pulse = x - 3.0;
    auto base = [&] (double theta) {
        if (pulse > 0.0)
            return theta < (1.0 - 0.75 * pulse) / 2.0 ? 1.0 : -1.0;
        const auto u = std::fmod (theta + d / 2.0, 1.0);
        double w = 0.0;
        if (u < d)
            w = 2.0 * u / d - 1.0;
        else
        {
            w = 1.0 - 2.0 * (u - d) / std::max (1e-12, 1.0 - d);
            if (t > 0.0)
                w = (u < 0.5 ? t : -t) + (1.0 - t) * w;
        }
        return (1.0 - m) * std::sin (2.0 * pi * theta) + m * w;
    };
    const auto skewA = (std::pow (21.0, std::clamp (op.form[2], 0, 128) / 128.0) - 1.0) / 20.0;
    const auto skewK = std::pow (1e9, skewA) - 1.0;
    const auto sine = std::clamp (op.form[3], 0, 128) * 1.5 * pi / 128.0;
    auto evalPartial = [&] (double c) {
        // Skew: bends the local phase φ = 2c around the center 1
        if (skewK > 1e-9)
        {
            const auto phi = 2.0 * c, delta = std::abs (phi - 1.0);
            const auto bent = 1.0 - (std::pow (skewK + 1.0, 1.0 - delta) - 1.0) / skewK;
            c = (phi <= 1.0 ? 1.0 - bent : 1.0 + bent) / 2.0;
        }
        const auto v = base (std::clamp (c, 0.0, 0.999999));
        return sine > 0.0 ? std::sin (v * sine) : v;
    };

    std::vector<double> wave ((std::size_t) size, 0.0);
    for (std::size_t k = 0; k < op.harmonics.size(); ++k)
    {
        const auto a = (double) op.harmonics[k];
        if (! std::isfinite (a) || a == 0.0)
            continue;
        const auto p = k < op.phases.size() && std::isfinite (op.phases[k]) ? (double) op.phases[k] : 0.0;
        const auto harmonic = (double) (k + 1);
        for (int i = 0; i < size; ++i)
        {
            const auto theta = (double) i / size;
            double value = 0.0;
            if (op.half && op.even)
            {
                const auto c = std::fmod (harmonic * theta + p, 1.0);
                value = c <= 0.5 ? evalPartial (c) : 0.0;
            }
            else if (op.half)
                value = evalPartial (std::fmod (harmonic * theta + 2.0 * p, 1.0) / 2.0);
            else if (op.even)
            {
                const auto c = std::fmod (harmonic * theta + p / 2.0, 1.0);
                value = c <= 0.5 ? evalPartial (2.0 * c) : 0.0;
            }
            else
                value = evalPartial (std::fmod (harmonic * theta + p, 1.0));
            wave[(std::size_t) i] += a * value;
        }
    }

    double peak = 0.0;
    for (const auto v : wave)
        peak = std::max (peak, std::abs (v));
    if (peak <= 5.96e-8)
        return false;
    const auto tA = (std::pow (21.0, std::abs (std::clamp (op.form[1], -128, 128)) / 128.0) - 1.0) / 20.0;
    const auto tK = (op.form[1] < 0 ? -1.0 : 1.0) * (std::pow (9.99999984e17, tA) - 1.0);
    for (auto& v : wave)
    {
        v /= peak;
        if (std::abs (tK) > 1e-9)
        {
            const auto u = std::abs (v);
            const auto curved = tK > 0.0 ? 1.0 - (std::pow (tK + 1.0, 1.0 - u) - 1.0) / tK : (std::pow (-tK + 1.0, u) - 1.0) / -tK;
            v = v < 0.0 ? -curved : curved;
        }
        if (op.absolute)
            v = std::abs (v);
    }

    // DFT over harmonics 1…256 (radix-2 FFT of the table)
    std::vector<std::complex<double>> bins (wave.begin(), wave.end());
    for (std::size_t i = 1, j = 0; i < bins.size(); ++i)
    {
        auto bit = bins.size() >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (bins[i], bins[j]);
    }
    for (std::size_t len = 2; len <= bins.size(); len <<= 1)
    {
        const auto w = std::polar (1.0, -2.0 * pi / (double) len);
        for (std::size_t i = 0; i < bins.size(); i += len)
        {
            std::complex<double> wn (1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = bins[i + k], v = bins[i + k + len / 2] * wn;
                bins[i + k] = u + v;
                bins[i + k + len / 2] = u - v;
                wn *= w;
            }
        }
    }
    // Pre-filter: harmonics ≥ M are removed; with the "Gibbs" byte, Lanczos sinc factors
    const auto pre = std::clamp (op.form[4], 0, 128);
    const auto cut = (int) std::lround ((std::pow (10.0, 10.0 * (0.65 + 0.35 * pre / 128.0)) - 1.0) * 1.6384e-6);
    for (int h = 1; h <= SynthSettings::spectrumSize; ++h)
    {
        auto c = bins[(std::size_t) h];
        if (cut <= size / 2 && h >= cut)
            c = 0.0;
        else if (cut <= size / 2 && op.gibbs && cut > 1)
        {
            const auto z = pi * h / (cut - 1);
            c *= std::sin (z) / z;
        }
        // W = Σ A·sin(2π(hθ + φ)): A = 2|X|/N; φ = arg(X)/2π + ¼; LYNX's "spectral" is scaled like the saw (×π/2)
        amplitude[(std::size_t) h - 1] = (float) (2.0 * std::abs (c) / size * pi / 2.0);
        const auto ph = std::arg (c) / (2.0 * pi) + 0.25;
        phase[(std::size_t) h - 1] = (float) (ph - std::floor (ph));
    }
    return true;
}

// operator waveform: from the harmonics table (§1.4) or, if only the fundamental is present, from Shape
Waveform waveOf (const Unit& op)
{
    std::vector<double> a (std::min<std::size_t> (32, op.harmonics.size()));
    double peak = 0.0;
    for (std::size_t k = 0; k < a.size(); ++k)
    {
        const auto h = op.harmonics[k];
        a[k] = std::isfinite (h) ? std::abs ((double) h) : 0.0;
        peak = std::max (peak, a[k]);
    }

    int nonzero = 0;
    for (auto& x : a)
    {
        x /= std::max (peak, 1e-12);
        nonzero += x > 0.02 ? 1 : 0;
    }

    if (peak <= 0.0 || (nonzero == 1 && a[0] > 0.02))
    {
        const auto x = std::clamp (op.shape, 0, 512);
        return x == 0 ? Waveform::sine : x < 192 ? Waveform::triangle : x < 320 ? Waveform::saw : Waveform::square;
    }

    // close to saw (1/k), square (odd 1/k) or triangle (odd 1/k²)?
    auto error = [&] (int kind) {
        double refSum = 0.0, sum = 0.0, e = 0.0;
        std::vector<double> ref (a.size());
        for (std::size_t k = 0; k < a.size(); ++k)
        {
            const auto h = (double) k + 1.0;
            const bool odd = (k % 2) == 0;
            ref[k] = kind == 0 ? 1.0 / h : ! odd ? 0.0 : kind == 1 ? 1.0 / h : 1.0 / (h * h);
            refSum += ref[k];
            sum += a[k];
        }
        for (std::size_t k = 0; k < a.size(); ++k)
            e += std::abs (a[k] / std::max (sum, 1e-9) - ref[k] / refSum);
        return e;
    };
    const double errors[] = { error (0), error (1), error (2) };
    const auto best = (int) (std::min_element (std::begin (errors), std::end (errors)) - std::begin (errors));
    if (errors[best] < 0.35)
        return best == 0 ? Waveform::saw : best == 1 ? Waveform::square : Waveform::triangle;

    double high = 0.0;
    for (std::size_t k = 4; k < a.size(); ++k)
        high += a[k];
    return high > 1.0 ? Waveform::saw : Waveform::triangle;
}

struct Adsr
{
    double a = 0.0, d = 0.0, s = 1.0, r = 0.01, peak = 1.0;
};

// point envelope -> ADSR (§1.6); `knobs` = articulator A/D/S/R, `timeScale` = s per unit
Adsr adsrOf (const Envelope& e, const std::array<std::int32_t, 4>& knobs, double timeScale)
{
    Adsr out;
    if (e.points.empty())
        return out;

    std::vector<double> t (e.points.size());
    double acc = 0.0;
    std::size_t peak = 0;
    for (std::size_t i = 0; i < e.points.size(); ++i)
    {
        acc += e.points[i].first;
        t[i] = acc;
        if (e.points[i].second > e.points[peak].second)
            peak = i;
    }

    const auto last = e.points.size() - 1;
    const auto oneShot = e.sustain < 0 || (std::size_t) e.sustain > last;
    const auto sus = oneShot ? last : (std::size_t) e.sustain;
    const auto peakLevel = std::max (1e-6, e.points[peak].second);

    out.peak = e.points[peak].second;
    out.a = t[peak] - t[0];
    if (oneShot)
    {
        out.d = t[last] - t[peak];
        out.s = e.points[last].second / peakLevel;
        out.r = out.d; // keeps falling after the key is released
    }
    else
    {
        out.d = sus > peak ? t[sus] - t[peak] : 0.0;
        out.s = e.points[sus].second / peakLevel;
        out.r = t[last] - t[sus];
    }

    auto knob = [&] (int i) { return std::clamp (knobs[(std::size_t) i], 0, 1024) / 128.0; };
    out.a *= knob (0) * timeScale;
    out.d *= knob (1) * timeScale;
    out.r *= knob (3) * timeScale;
    out.s = std::clamp (out.s + std::clamp (knobs[2], -128, 128) / 128.0, 0.0, 1.0);
    out.a = std::min (out.a, 20.0);
    out.d = std::min (out.d, 20.0);
    out.r = std::clamp (out.r, 0.003, 20.0);
    return out;
}

// cutoff in Hz from the filter-type curve (§1.7), c = 0..1
double cutoffHz (int kind, double c)
{
    c = std::clamp (c, 0.0, 1.0);
    double hz = 0.0;
    switch (kind)
    {
        case 0: hz = 22050.0 * 0.0396 * (std::pow (26.0, c) - 1.0); break;
        case 1: case 2: hz = std::min (17640.0, 22050.0 * (0.007 + 0.01324 * (std::pow (76.0, c) - 1.0))); break;
        case 3: case 11: hz = 44100.0 * (0.003 + 0.000665 * (std::pow (1501.0, c) - 1.0)); break;
        default: hz = 44100.0 * (0.0001 + 0.0004 * (std::pow (1501.0, c) - 1.0)); break;
    }
    return std::clamp (hz, 20.0, 20000.0);
}

// outside the oscillator range (±47 semitones) shift by octaves, keeping the note
void setPitch (SynthOscillator& osc, double semitones)
{
    if (! std::isfinite (semitones))
        semitones = 0.0;
    while (semitones < -47.0)
        semitones += 12.0;
    while (semitones > 47.0)
        semitones -= 12.0;
    const auto whole = (int) std::lround (semitones);
    osc.octave = std::clamp (whole / 12, -3, 3);
    osc.semitones = std::clamp (whole - osc.octave * 12, -11, 11);
    osc.detune = (float) std::clamp ((semitones - whole) * 100.0, -100.0, 100.0);
}

// operator tuning in semitones relative to the note (ratio + offset; ratio 0 = fixed frequency)
double opSemitones (const Unit& op)
{
    if (op.ratio <= 1e-6)
        return 12.0 * std::log2 (std::max (1.0, std::abs (op.offsetHz)) / 261.63);
    auto semis = 12.0 * std::log2 (op.ratio);
    if (std::abs (op.offsetHz) > 1e-6)
        semis += 12.0 * std::log2 (std::max (0.01, 1.0 + op.offsetHz / (261.63 * op.ratio)));
    return semis;
}
} // namespace

bool synthFromSytrus (const std::vector<std::uint8_t>& state, double bpm, SynthSettings& synth, float& gain)
{
    State s;
    if (! parse (state, s))
        return false;

    const auto beatSeconds = 60.0 / std::max (1.0, bpm);
    auto timeScale = [&] (const Envelope& e) { return (e.flags & 1) != 0 ? beatSeconds : 1.0; };
    auto opVolume = [&] (int i) { return s.units[(std::size_t) i].p[1] / 256.0; };

    // effective output of each filter (direct to OUT or via "to next filter"); a disabled filter passes the sound through
    std::array<double, 3> filterOut {};
    for (int f = 2; f >= 0; --f)
    {
        filterOut[(std::size_t) f] = curve (s.cell (0, 6 + f, 8));
        if (f < 2)
            filterOut[(std::size_t) f] += curve (s.units[(std::size_t) (6 + f)].p[9]) * filterOut[(std::size_t) f + 1];
    }

    // 1. carriers: direct level + level going through the filters
    std::array<double, 6> direct {}, viaFilter {};
    for (int i = 0; i < 6; ++i)
    {
        direct[(std::size_t) i] = curve (s.cell (0, i, 8)) * opVolume (i);
        for (int f = 0; f < 3; ++f)
            viaFilter[(std::size_t) i] += curve (s.cell (0, 6 + f, i)) * filterOut[(std::size_t) f] * opVolume (i);
    }

    std::array<int, 6> order { 0, 1, 2, 3, 4, 5 };
    auto amp = [&] (int i) { return std::abs (direct[(std::size_t) i] + viaFilter[(std::size_t) i]); };
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return amp (a) > amp (b); });

    std::vector<int> carriers;
    for (auto i : order)
        if (amp (i) > 0.01 && carriers.size() < 3)
            carriers.push_back (i);

    SynthSettings out;
    out.osc = { SynthOscillator { Waveform::sine, 0, 0.0f, 1.0f }, SynthOscillator {}, SynthOscillator {} };
    out.attack = 0.0f;
    out.decay = 0.0f;
    out.sustain = 1.0f;
    out.release = 0.1f;
    out.cutoff = 20000.0f;
    out.porta = s.porta;

    auto masterPitch = (double) std::clamp (s.main[2], -48, 48);
    gain = (float) std::clamp (curve (std::clamp (s.main[0], 0, 256), 128.0), 0.0, 4.0);

    if (carriers.empty()) // nothing reaches the output: default patch
    {
        synth = out;
        return true;
    }

    // patches that are too low (ratio 0.25 with Master pitch −24...): raise everything by octaves to fit
    double lowest = 0.0;
    for (auto i : carriers)
        lowest = std::min (lowest, opSemitones (s.units[(std::size_t) i]) + masterPitch);
    while (lowest < -47.0)
    {
        lowest += 12.0;
        masterPitch += 12.0;
    }

    // 2-4. oscillators: tuning, waveform, FM -> brightness, level and pan
    double fmIndex = 0.0; // sum of the strong FMs (for the filter envelope)
    bool usedSpectrum = false;
    int fmModulator = -1;
    for (std::size_t k = 0; k < carriers.size(); ++k)
    {
        const auto i = carriers[k];
        const auto& op = s.units[(std::size_t) i];
        auto& osc = out.osc[k];
        osc.wave = waveOf (op);
        // exact operator shape in the "Harmonics" oscillator (one spectrum per channel: the first carrier that is not
        // a pure sine). REA_rodada23 §2
        if (! usedSpectrum)
        {
            int rich = 0;
            for (const auto h : op.harmonics)
                rich += std::isfinite (h) && std::abs (h) > 1e-4f ? 1 : 0;
            const auto plainSine = rich <= 1 && op.form == std::array<std::int32_t, 5> { 0, 0, 0, 0, 128 } && ! op.half && ! op.even && ! op.absolute;
            if (! plainSine && operatorSpectrum (op, out.spectrum, out.spectrumPhase))
            {
                usedSpectrum = true;
                osc.wave = Waveform::spectral;
            }
        }
        setPitch (osc, opSemitones (op) + masterPitch);
        osc.level = (float) std::min (1.0, amp (i));
        osc.pan = (float) std::clamp (2.0 * s.cell (0, i, 6) / 512.0, -1.0, 1.0);

        double fm = 0.0, strongest = 0.0;
        int modulator = -1;
        for (int j = 0; j < 6; ++j)
        {
            if (j == i)
                continue;
            const auto g = std::abs (curve (s.cell (0, i, j)) * opVolume (j));
            fm += g;
            if (g > strongest)
                strongest = g, modulator = j;
        }

        const auto feedback = std::abs (curve (s.cell (0, i, i)));
        if (feedback > 0.3 && osc.wave == Waveform::sine)
            osc.wave = Waveform::saw;

        if (fm >= 0.15 && modulator >= 0)
        {
            const auto ratio = s.units[(std::size_t) modulator].ratio / std::max (1e-6, op.ratio);
            const auto nearest = std::round (ratio);
            if (std::abs (ratio - nearest) < 0.02 && nearest >= 1.0)
            {
                if (osc.wave == Waveform::sine || osc.wave == Waveform::triangle)
                    osc.wave = ((int) nearest % 2 == 0) ? Waveform::square : Waveform::saw;
            }
            else if (carriers.size() < 3 && out.osc[2].level == 0.0f) // inharmonic: extra sine at the modulator ratio
            {
                out.osc[2] = { Waveform::sine, 0, 0.0f, (float) std::min (1.0, fm) * osc.level * 0.5f };
                setPitch (out.osc[2], opSemitones (s.units[(std::size_t) modulator]) + masterPitch);
            }

            if (k == 0)
            {
                fmIndex = fm;
                fmModulator = modulator;
            }
        }
    }

    // other carriers an integer multiple above the main one (same pan, no offset, no strong FM) join its spectrum:
    // harmonic h of carrier k lands on h·m, scaled by its level relative to the main carrier, summed as phasors.
    // (They then share the main carrier's envelope, as every carrier already did; REA_rodada23 §2 for each shape.)
    if (carriers.size() >= 2 && s.units[(std::size_t) carriers[0]].ratio > 1e-6 && std::abs (s.units[(std::size_t) carriers[0]].offsetHz) < 1e-6)
    {
        auto fmOn = [&] (int i) {
            double fm = 0.0;
            for (int j = 0; j < 6; ++j)
                if (j != i)
                    fm += std::abs (curve (s.cell (0, i, j)) * opVolume (j));
            return fm;
        };
        std::array<std::complex<double>, SynthSettings::spectrumSize> sum {};
        auto add = [&] (const Unit& op, int multiple, double scale) {
            std::array<float, SynthSettings::spectrumSize> a {}, ph {};
            if (! operatorSpectrum (op, a, ph))
                return false;
            for (int h = 1; h * multiple <= SynthSettings::spectrumSize; ++h)
                sum[(std::size_t) (h * multiple - 1)] += std::polar ((double) a[(std::size_t) h - 1] * scale, 2.0 * 3.141592653589793 * ph[(std::size_t) h - 1]);
            return true;
        };
        const auto& mainOp = s.units[(std::size_t) carriers[0]];
        const auto mainLevel = std::max (1e-6, (double) out.osc[0].level);
        std::vector<int> merged;
        if (fmOn (carriers[0]) < 0.15 && add (mainOp, 1, 1.0))
            for (std::size_t k = 1; k < carriers.size(); ++k)
            {
                const auto& op = s.units[(std::size_t) carriers[k]];
                const auto m = op.ratio / mainOp.ratio, multiple = std::round (m);
                if (op.ratio <= 1e-6 || std::abs (op.offsetHz) > 1e-6 || multiple < 1.0 || std::abs (m - multiple) > 1e-3 * multiple
                    || std::abs (out.osc[k].pan - out.osc[0].pan) > 0.02f || fmOn (carriers[k]) >= 0.15)
                    continue;
                if (add (op, (int) multiple, out.osc[k].level / mainLevel))
                    merged.push_back ((int) k);
            }
        if (! merged.empty())
        {
            for (std::size_t h = 0; h < sum.size(); ++h)
            {
                out.spectrum[h] = (float) std::abs (sum[h]);
                out.spectrumPhase[h] = (float) (std::arg (sum[h]) / (2.0 * 3.141592653589793));
                if (out.spectrumPhase[h] < 0.0f)
                    out.spectrumPhase[h] += 1.0f;
            }
            out.osc[0].wave = Waveform::spectral;
            for (auto it = merged.rbegin(); it != merged.rend(); ++it)
            {
                out.osc[(std::size_t) *it] = {};
                carriers.erase (carriers.begin() + *it);
            }
            for (std::size_t k = 0; k < 3; ++k) // keep the remaining carriers in the first slots
                if (out.osc[k].level == 0.0f)
                    for (std::size_t j = k + 1; j < 3; ++j)
                        if (out.osc[j].level > 0.0f)
                        {
                            std::swap (out.osc[k], out.osc[j]);
                            break;
                        }
        }
    }

    // strong RM on the main carrier: ring with the modulator on osc 3, carrier × (1 − |a| + |a|·mod)
    // (REA_rodada18 §2; the sign of a inverts the modulator phase and is not reproduced)
    if (carriers.size() < 3)
    {
        const auto i = carriers[0];
        for (int j = 0; j < 6; ++j)
            if (j != i && std::abs (curve (s.cell (1, i, j))) >= 0.2)
            {
                out.am = true;
                out.ring = true;
                out.osc[2] = { Waveform::sine, 0, 0.0f, (float) std::min (1.0, std::abs (curve (s.cell (1, i, j)))) };
                setPitch (out.osc[2], opSemitones (s.units[(std::size_t) j]) + masterPitch);
                break;
            }
    }

    // 5. amplitude envelope of the main carrier (articulator 1) + global scales
    const auto& main = s.units[(std::size_t) carriers[0]];
    if (const auto& vol = main.art[1]; vol.env.on)
    {
        auto adsr = adsrOf (vol.env, vol.knobs, timeScale (vol.env));
        auto global = [&] (int i) { return std::clamp (s.main[165 + (std::size_t) i], 0, 1024) / 128.0; };
        out.attack = (float) std::min (20.0, adsr.a * global (0));
        out.decay = (float) std::min (20.0, adsr.d * global (1));
        out.sustain = (float) adsr.s;
        out.release = (float) std::clamp (adsr.r * global (3), 0.003, 20.0);
        for (auto& o : out.osc)
            o.level = (float) std::clamp (o.level * adsr.peak, 0.0, 1.0);
    }
    else
    {
        out.attack = 0.002f;
        out.release = 0.01f;
    }

    // 6-7. filter: the first active one that receives some carrier, if most of the sound goes through it
    double total = 0.0, filtered = 0.0;
    for (auto i : carriers)
    {
        total += amp (i);
        filtered += std::abs (viaFilter[(std::size_t) i]);
    }

    bool hasFilter = false;
    for (int f = 0; f < 3 && filtered >= 0.5 * total; ++f)
    {
        const auto& unit = s.units[(std::size_t) (6 + f)];
        const auto mode = std::clamp (unit.p[0], 0, 5);
        bool fed = false;
        for (auto i : carriers)
            fed = fed || s.cell (0, 6 + f, i) != 0;
        if (mode == 0 || ! fed || std::abs (filterOut[(std::size_t) f]) < 1e-4)
            continue;

        const auto kind = std::clamp (unit.filterKind, 0, 12);
        int lynxType = 0;
        switch (kind)
        {
            case 1: case 3: case 11: lynxType = 0; break;
            case 2: case 6: lynxType = 2; break;
            case 4: lynxType = 1; break;
            case 5: lynxType = 3; break;
            case 0:
            {
                const auto low = unit.p[4], band = unit.p[5], high = unit.p[6];
                lynxType = low >= band && low >= high ? 0 : band >= high ? 1 : 2;
                break;
            }
            default: lynxType = -1; break; // all pass, shelf, peak, phaser: no filter
        }
        if (lynxType < 0)
            break;

        const auto c = std::clamp (unit.p[2], 0, 256) / 256.0;
        const auto r = std::clamp (unit.p[3], 0, 256) / 256.0;
        const int stages[] = { 1, 1, 2, 3, 2, 3 };
        const auto stageFactor = std::pow (0.9, stages[mode] - 1);

        double q = 0.0;
        if (kind == 0)
            out.resonance = (float) std::clamp (0.8 * r, 0.0, 0.95);
        else
        {
            q = kind <= 2 ? 1.0 + 0.049 * (std::pow (1001.0, r) - 1.0) : 0.1 + 0.02495 * (std::pow (2001.0, r) - 1.0);
            out.resonance = (float) std::clamp ((q - 0.707) / 8.0, 0.0, 0.95);
        }
        out.filterType = lynxType;

        const auto& cutArt = unit.art[2];
        const auto depth = std::clamp (unit.p[1], 0, 256) / 128.0;
        if (cutArt.env.on && ! cutArt.env.points.empty() && depth > 0.0)
        {
            const auto adsr = adsrOf (cutArt.env, cutArt.knobs, timeScale (cutArt.env));
            // the cutoff articulator is bipolar like the pitch one: level 0.5 = the Cutoff knob
            const auto start = cutArt.env.points.front().second;
            double peak = start;
            for (const auto& p : cutArt.env.points)
                peak = std::max (peak, p.second);
            auto at = [&] (double level) { return cutoffHz (kind, c + depth * (level - 0.5)); };
            const auto susLevel = adsr.s * peak; // (LYNX starts from `base`: an envelope that closes below its start starts from the bottom)
            const auto base = at (std::min (start, susLevel));
            const auto top = at (peak);
            const auto susHz = at (susLevel);
            const auto octaves = std::log2 (top / base);
            out.cutoff = (float) std::clamp (base * stageFactor, 20.0, 20000.0);
            if (octaves > 0.05)
                out.filterEnv = { (float) std::min (10.0, octaves), (float) adsr.a, 0.0f, (float) adsr.d,
                                  (float) std::clamp (std::log2 (susHz / base) / octaves, 0.0, 1.0), (float) adsr.r };
        }
        else
            out.cutoff = (float) std::clamp (cutoffHz (kind, c) * stageFactor, 20.0, 20000.0);

        hasFilter = true;
        break;
    }

    // strong FM on a single carrier: real FM (the modulator on osc 3, index ≈ 4·g cycles, with the
    // modulator's volume envelope making the index fall)
    if (fmIndex >= 0.15 && fmModulator >= 0 && carriers.size() == 1 && ! out.am)
    {
        const auto& modOp = s.units[(std::size_t) fmModulator];
        out.osc[0].wave = waveOf (main);
        out.osc[2] = { waveOf (modOp), 0, 0.0f, 1.0f };
        setPitch (out.osc[2], opSemitones (modOp) + masterPitch);
        out.fm = (float) std::min (8.0, 4.0 * fmIndex);
        if (const auto& modVol = modOp.art[1]; modVol.env.on)
        {
            const auto adsr = adsrOf (modVol.env, modVol.knobs, timeScale (modVol.env));
            if (adsr.s < 0.98)
            {
                out.fmDecay = (float) std::clamp (adsr.d * 2.0, 0.01, 20.0); // (LYNX's decay goes down to −60 dB)
                out.fmSustain = (float) adsr.s;
            }
        }
    }
    // strong FM without a Sytrus filter: the falling index becomes an enveloped low-pass
    else if (fmIndex >= 0.15 && fmModulator >= 0 && (! hasFilter || (out.filterType == 0 && out.filterEnv.amount == 0.0f)))
    {
        const auto octaves = std::min (6.0, 4.0 * fmIndex);
        const auto& modVol = s.units[(std::size_t) fmModulator].art[1];
        if (modVol.env.on)
        {
            const auto adsr = adsrOf (modVol.env, modVol.knobs, timeScale (modVol.env));
            if (! hasFilter)
                out.cutoff = 400.0f;
            out.filterEnv = { (float) octaves, (float) adsr.a, 0.0f, (float) std::max (0.02, adsr.d), (float) adsr.s, (float) adsr.r };
        }
        else if (! hasFilter)
            out.cutoff = (float) std::clamp (400.0 * std::exp2 (octaves), 20.0, 20000.0);
    }

    // 8. pitch envelope of the main carrier (articulator 3): level 0.5 = no deviation
    if (const auto& pitch = main.art[3]; pitch.env.on && pitch.env.points.size() >= 2)
    {
        const auto start = pitch.env.points.front().second;
        const auto semis = (start - 0.5) * 2.0 * std::clamp (main.p[2], 0, 4800) / 100.0;
        if (std::abs (semis) > 0.1)
        {
            double t = 0.0, settle = 0.0;
            for (const auto& p : pitch.env.points)
            {
                t += p.first;
                settle = t;
                if (std::abs (p.second - 0.5) < 0.02)
                    break;
            }
            out.pitchEnv = { (float) std::clamp (semis, -48.0, 48.0), 0.0f, 0.0f, (float) std::min (20.0, settle * timeScale (pitch.env)), 0.0f, 0.05f };
        }
    }

    // 9. enabled LFOs: carrier volume, filter cutoff, pitch, pan
    auto lfoFrom = [&] (const Articulator& a, float amount) {
        ChannelLfo lfo;
        const auto speed = std::clamp (a.lfoSpeed, 512, 65536);
        lfo.period = (float) std::clamp (625.0 / (std::pow (10001.0, speed / 65536.0) - 1.0), 0.01, 600.0);
        lfo.sync = (a.lfo.flags & 1) != 0;
        lfo.global = (a.lfo.flags & 2) != 0;
        lfo.amount = amount;
        return lfo;
    };
    auto depthOf = [] (const Articulator& a) { return std::isfinite (a.lfoDepth) ? std::clamp ((double) std::abs (a.lfoDepth), 0.0, 1.0) : 0.5; };
    if (main.art[1].lfo.on)
        out.lfo[lfoVolume] = lfoFrom (main.art[1], (float) (0.5 * depthOf (main.art[1])));
    if (main.art[3].lfo.on)
        out.lfo[lfoPitch] = lfoFrom (main.art[3], (float) (std::clamp (main.p[2], 0, 4800) / 100.0 * depthOf (main.art[3])));
    if (main.art[0].lfo.on)
        out.lfo[lfoPan] = lfoFrom (main.art[0], (float) depthOf (main.art[0]));
    for (int f = 0; f < 3 && hasFilter; ++f)
        if (const auto& a = s.units[(std::size_t) (6 + f)].art[2]; s.units[(std::size_t) (6 + f)].p[0] != 0 && a.lfo.on)
        {
            out.lfo[lfoCutoff] = lfoFrom (a, (float) (3.0 * depthOf (a)));
            break;
        }

    // 10. unison: only 1 carrier -> three detuned, spread copies
    const auto unison = std::clamp (s.main[173], 1, 9);
    if (unison >= 2 && carriers.size() == 1 && ! out.am && out.fm > 0.0f) // osc 3 is the modulator: only one copy
    {
        const auto pv = std::clamp (s.main[176], 0, 256) / 256.0;
        const auto spread = std::max (5.0, s.altUnisonPitch ? (std::pow (41.0, pv) - 1.0) * 1.875 : (std::pow (11.0, pv) - 1.0) * 5.0);
        const auto pan = (float) (std::clamp (s.main[174], 0, 128) / 128.0);
        out.osc[1] = out.osc[0];
        out.osc[0].level = out.osc[1].level = std::min (1.0f, out.osc[0].level * 0.75f);
        out.osc[1].detune = (float) std::clamp (out.osc[0].detune + spread, -100.0, 100.0);
        out.osc[0].detune = (float) std::clamp (out.osc[0].detune - spread, -100.0, 100.0);
        out.osc[0].pan = std::clamp (out.osc[0].pan - pan, -1.0f, 1.0f);
        out.osc[1].pan = std::clamp (out.osc[1].pan + pan, -1.0f, 1.0f);
    }
    else if (unison >= 2 && carriers.size() == 1 && ! out.am)
    {
        const auto pv = std::clamp (s.main[176], 0, 256) / 256.0;
        const auto spread = std::max (5.0, s.altUnisonPitch ? (std::pow (41.0, pv) - 1.0) * 1.875 : (std::pow (11.0, pv) - 1.0) * 5.0);
        const auto pan = (float) (std::clamp (s.main[174], 0, 128) / 128.0);
        const auto base = out.osc[0];
        const auto level = base.level / 3.0f * 1.5f;
        for (int k = 0; k < 3; ++k)
        {
            out.osc[(std::size_t) k] = base;
            out.osc[(std::size_t) k].level = std::min (1.0f, level);
        }
        out.osc[1].detune = (float) std::clamp (base.detune + spread, -100.0, 100.0);
        out.osc[1].pan = std::clamp (base.pan + pan, -1.0f, 1.0f);
        out.osc[2].detune = (float) std::clamp (base.detune - spread, -100.0, 100.0);
        out.osc[2].pan = std::clamp (base.pan - pan, -1.0f, 1.0f);
    }

    for (auto& o : out.osc)
        if (! std::isfinite (o.level) || ! std::isfinite (o.detune) || ! std::isfinite (o.pan))
            return false;

    synth = out;
    return true;
}
// Harmor (REA_rodada11 §4): 2-part additive; each part becomes a saw oscillator, with filter 1
// (or 2), the volume envelope, the filter envelope and unison. Image, blur, prism,
// harmonizer, phaser and FX are left out.
namespace
{
// value of a shaping curve at x ∈ [0, 1] (REA_rodada23 §1.2): segment up to point j with the shape of point j
double shapingAt (const Envelope& e, double x)
{
    if (e.points.empty())
        return 0.25;
    double total = 0.0;
    for (const auto& pt : e.points)
        total += pt.first;
    if (total <= 0.0)
        return e.points.front().second;
    x *= total;
    double start = 0.0;
    for (std::size_t j = 1; j < e.points.size(); ++j)
    {
        const auto end = start + e.points[j].first;
        if (x <= end || j + 1 == e.points.size())
        {
            const auto t = e.points[j].first > 0.0 ? std::clamp ((x - start) / e.points[j].first, 0.0, 1.0) : 1.0;
            const auto tension = j < e.tensions.size() ? (double) e.tensions[j] : 0.0;
            const auto mode = j < e.curves.size() ? e.curves[j] : 0;
            const auto a = e.points[j - 1].second, b = e.points[j].second;
            const auto n = std::round ((std::pow (1000001.0, 1.0 - 0.6 * std::abs (tension)) - 1.0) * 0.001);
            double f = t;
            switch (mode)
            {
                case 2: f = 0.0; break;
                case 5: f = std::fmod (std::round (t * 2.0 * (n + 1.0) - (tension < 0.0 ? -0.5 : 0.5)), 2.0); break;
                case 6:
                    if (tension < 0.0)
                        f = (std::sin (2.0 * 3.141592653589793 * (t * (n + 0.5) + 0.75)) + 1.0) / 2.0;
                    else if (n > 0.0)
                    {
                        const auto u = t * (n + 0.5);
                        f = std::abs (2.0 * (u - std::floor (u)) - 1.0);
                    }
                    break;
                case 7:
                {
                    const auto u = tension <= 0.0 ? t : 1.0 - t;
                    f = t - (std::pow (u, 6.0) - u) * tension;
                    break;
                }
                case 9: // REA_rodada25 §3.3: t − (t − g)·|T|, g = sin(πt/2) (T > 0) or 1 − cos(πt/2)
                {
                    const auto g = tension > 0.0 ? std::sin (t * 3.141592653589793 / 2.0) : 1.0 - std::cos (t * 3.141592653589793 / 2.0);
                    f = t - (t - g) * std::abs (tension);
                    break;
                }
                default:
                    if (std::abs (tension) > 1e-9)
                    {
                        const auto k = std::pow (1000001.0, std::abs (tension)) - 1.0;
                        f = tension > 0.0 ? 1.0 - (std::pow (k + 1.0, 1.0 - t) - 1.0) / k : (std::pow (k + 1.0, t) - 1.0) / k;
                    }
                    break;
            }
            return a + f * (b - a);
        }
        start = end;
    }
    return e.points.back().second;
}

// Harmor volume curve (REA_rodada18 §1.3): exactly 1 at x = 1; otherwise (3^x − 1)/2
double harmorGain (double x)
{
    return x == 1.0 ? 1.0 : (std::pow (3.0, x) - 1.0) / 2.0;
}
} // namespace

bool synthFromHarmor (const std::vector<std::uint8_t>& state, SynthSettings& synth, float& gain)
{
    if (state.size() < 12)
        return false;
    Reader r { state.data(), state.size() };
    auto version = (std::uint32_t) r.i32();
    if (version > 0xffff)
        version >>= 16;
    if (version < 11 || version > 32)
        return false;
    const auto v = (int) version;
    const auto mask = (std::uint32_t) r.i32();
    if ((mask & 1) == 0)
        return false;

    struct Part
    {
        bool present = false;
        std::vector<std::int32_t> p;
        int filterType[2] { 0, 0 };
        int resonanceType[2] { 108, 108 }; // codes 100 Classic … 108 Lowpass, 109 Noise, 110/111 Custom shape 1/2
        int blendMode = 0, partialMode = 1;
        std::vector<Articulator> art;
        std::vector<Envelope> shaping; // 26 curves: 0 Local EQ, 1 phase, 2/3 Timbre 1/2… (REA_rodada23 §1.1)
    };
    Part parts[2];

    for (int k = 0; k < 2; ++k)
    {
        if (((mask >> k) & 1) == 0)
            continue;
        const auto size = (std::size_t) (std::uint32_t) r.i32();
        const auto start = r.pos;
        if (r.failed || size > state.size() - start)
            return false;

        auto& part = parts[k];
        part.p.resize (v >= 16 ? 386 : 385);
        for (auto& x : part.p)
            x = r.i32();
        r.raw (0x30);
        if (const auto* misc = r.raw (v >= 11 ? 0x4b : 0x47))
        {
            std::memcpy (&part.filterType[0], misc + 52, 4);
            std::memcpy (&part.filterType[1], misc + 56, 4);
            std::memcpy (part.resonanceType, misc + 0x3C, sizeof (part.resonanceType)); // codes (REA_rodada25 §1.1)
            std::memcpy (&part.blendMode, misc, 4);
            std::memcpy (&part.partialMode, misc + 0x1C, 4);
        }
        if (r.failed)
            return false;

        if (v >= 16)
        {
            part.art.resize (43);
            for (auto& a : part.art)
                if (! readArticulator (r, a, 9))
                {
                    part.art.clear();
                    break;
                }
            if (! part.art.empty())
            {
                part.shaping.resize (26);
                for (auto& e : part.shaping)
                    if (! readEnvelope (r, e))
                    {
                        part.shaping.clear();
                        break;
                    }
            }
        }
        part.present = true;
        r.pos = start + size;
        r.failed = false;
    }

    std::int32_t g[53] {};
    if ((mask & 4) != 0)
        for (auto& x : g)
            x = r.i32();
    if (r.failed)
        std::fill (std::begin (g), std::end (g), 0);
    const auto abMix = (mask & 4) != 0 && ! r.failed ? std::clamp (g[10], 0, 1000) / 1000.0 : 0.5;
    const auto mainPitch = std::clamp (g[14], -4800, 4800) / 100.0;

    SynthSettings out;
    out.osc = { SynthOscillator { Waveform::saw, 0, 0.0f, 0.0f }, SynthOscillator { Waveform::saw, 0, 0.0f, 0.0f }, SynthOscillator {} };
    double partSemis[2] {};
    for (int k = 0; k < 2; ++k)
    {
        if (! parts[k].present)
            continue;
        const auto& p = parts[k].p;
        const auto ratio = std::clamp (p[40], 1000, 640000) / 10000.0 / std::clamp (p[41], 1, 16);
        partSemis[k] = mainPitch + 12.0 * std::log2 (ratio);
        auto& osc = out.osc[(std::size_t) k];
        osc.detune = 0.0f;
        setPitch (osc, partSemis[k]);
        const auto mix = k == 0 ? 1.0 - abMix : abMix;
        // part volume (REA_rodada18 §1.3): g(x) = (3^x − 1)/2, x = |p11|/1000 (1000 → 1.0; 2000 → 4.0)
        // × p12 "Frequency domain envelope volume" on the same curve (multiplies the volume articulator level)
        osc.level = (float) std::clamp (harmorGain (std::abs (p[11]) / 1000.0) * harmorGain (std::clamp (p[12], 0, 2000) / 1000.0) * mix, 0.0, 4.0); // A&B mix = crossfade (up to +12 dB)
    }

    if (out.osc[0].level + out.osc[1].level < 0.01f)
        out.osc[0].level = 1.0f;

    const auto lead = out.osc[0].level >= out.osc[1].level ? 0 : 1;
    const auto& main = parts[lead];

    // main part timbre (REA_rodada23 §1.7): A(n) = [(1 − m)(4y₁)² + m(4y₂)²]·(4y_L(log2 n/9))²/n and
    // φ(n) = y_φ − 0.5, x = (n − 1)/515; static case (no modulation, no curve filter) and harmonic partials
    if (main.shaping.size() == 26 && main.partialMode >= 1 && main.blendMode >= 0 && main.blendMode <= 4
        && main.p.size() > 43 && main.p[42] == 10000 * std::max (1, main.p[43]))
    {
        const auto m = std::clamp (main.p[4] / 1000.0, 0.0, 1.0);
        for (int n = 1; n <= SynthSettings::spectrumSize; ++n)
        {
            const auto x = (n - 1) / 515.0;
            const auto g1 = std::pow (4.0 * shapingAt (main.shaping[2], x), 2.0);
            auto g2 = std::pow (4.0 * shapingAt (main.shaping[3], x), 2.0);
            switch (main.blendMode)
            {
                case 1: g2 = std::max (g1 - g2, 0.0); break;
                case 2: g2 = g1 * g2; break;
                case 3: g2 = std::max (g1, g2); break;
                case 4: g2 = std::min (g1, g2); break;
                default: break;
            }
            const auto localEq = std::pow (4.0 * shapingAt (main.shaping[0], std::log2 ((double) n) / 9.0), 2.0);
            out.spectrum[(std::size_t) n - 1] = (float) std::clamp (((1.0 - m) * g1 + m * g2) * localEq / n, 0.0, 64.0);
            const auto ph = shapingAt (main.shaping[1], x) - 0.5;
            out.spectrumPhase[(std::size_t) n - 1] = (float) (ph - std::floor (ph));
        }
        out.osc[(std::size_t) lead].wave = Waveform::spectral;
    }

    // volume envelope (articulator 1). When off, FL applies no envelope at all (constant level,
    // REA_rodada18 §1.1); the A 2 ms / R 0.1 s below is just LYNX's declick. When on, the level used is the
    // square of the articulator output: sustain S² (attack/decay keep their times).
    out.attack = 0.002f;
    out.decay = 0.0f;
    out.sustain = 1.0f;
    out.release = 0.1f;
    if (! main.art.empty() && main.art[1].env.on)
    {
        const auto adsr = adsrOf (main.art[1].env, main.art[1].knobs, 1.0);
        out.attack = (float) adsr.a;
        out.decay = (float) adsr.d;
        out.sustain = (float) (adsr.s * adsr.s);
        out.release = (float) adsr.r;
    }

    // filter 1 (or 2 if filter 1 is open). REA_rodada18 §1.2/§1.4: the knob is linear in log2(Hz) between
    // 73.416 Hz and 17 739.7 Hz for a 130.81 Hz note (key 48) and tracks the keyboard with
    // KT = p55/1000 (p67 on filter 2). Types 0…18 = combo without separators; only low-pass/band-pass/high-pass
    // and band-reject have an equivalent.
    const auto& p = main.p;
    int which = p[52] < 8192 ? 0 : p[64] < 8192 ? 1 : -1;
    out.cutoff = 20000.0f;
    if (which >= 0)
    {
        const auto type = main.filterType[which];
        int lynxType = -1;
        switch (type)
        {
            case 0: case 4: case 8: case 9: case 10: case 11: case 12: case 13: lynxType = 0; break;
            case 1: case 5: case 16: lynxType = 1; break;
            case 2: case 6: lynxType = 3; break;
            case 3: case 7: lynxType = 2; break;
            default: break;
        }
        if (lynxType >= 0)
        {
            const auto freq = std::clamp (which == 0 ? p[52] : p[64], 0, 8192);
            const auto lo = std::log2 (73.416), hi = std::log2 (17739.7);
            out.filterType = lynxType;
            out.cutoff = (float) std::clamp (std::exp2 (lo + freq / 8192.0 * (hi - lo)), 20.0, 20000.0);
            out.keyTrack = (float) std::clamp ((which == 0 ? p[55] : p[67]) / 1000.0, -2.0, 2.0);

            // resonance (REA_rodada25): per partial FL multiplies by 2^(r·M(f)·T(pos)), r = p57/1000 (p69), T = the
            // type's table (peak at f_b = fc·2^(p60/1200)), M = the "resonance mask" curve squared over 32.7 Hz…16.7 kHz.
            // LYNX's SVF gets the same peak over the pass-band: Δ = 6.0206·r·M(f_b)·(T_peak − T_below) dB →
            // Q = √(G²(1 + √(1 − 1/G²))/2), G = 10^(Δ/20) (approximation: no offset, width p59 or skirts)
            const auto resAmount = std::clamp (which == 0 ? p[57] : p[69], 0, 1000) / 1000.0;
            const auto code = main.resonanceType[which];
            double peak = 0.0, below = 0.0;
            switch (code)
            {
                case 100: peak = 6.0; below = -1.0; break;  // Classic
                case 101: peak = 6.25; break;               // Cuberdon
                case 102: peak = 5.8; break;                // Pedestal
                case 103: peak = 6.9; break;                // Sedge hat
                case 104: peak = 5.4; break;                // Wide bump
                case 105: peak = 6.2; break;                // Double cone
                case 107: peak = 2.168; break;              // Wormhole
                case 108: peak = 6.0; below = -0.8; break;  // Lowpass
                case 110: case 111:                          // Custom shape 1/2: T = 24·(y − 0.5)
                    if (main.shaping.size() == 26)
                    {
                        const auto& e = main.shaping[(std::size_t) (12 + code - 110)];
                        double top = 0.0;
                        for (const auto& pt : e.points)
                            top = std::max (top, pt.second);
                        peak = 24.0 * (top - 0.5);
                        below = 24.0 * (shapingAt (e, 0.0) - 0.5);
                    }
                    break;
                default: break; // 106 Well (cut only), 109 Noise (not decoded)
            }
            if (resAmount > 0.0 && peak > below)
            {
                const auto bump = out.cutoff * std::exp2 (std::clamp (which == 0 ? p[60] : p[72], -2400, 2400) / 1200.0);
                const auto x = std::clamp ((std::log2 (bump) - std::log2 (32.7032)) / 9.0, 0.0, 1.0);
                const auto m = main.shaping.size() == 26 ? std::pow (shapingAt (main.shaping[(std::size_t) (20 + which)], x), 2.0) : 1.0;
                const auto deltaDb = 6.0206 * resAmount * m * (peak - below);
                if (const auto g = std::pow (10.0, deltaDb / 20.0); g > 1.0)
                {
                    const auto q = std::sqrt (g * g * (1.0 + std::sqrt (1.0 - 1.0 / (g * g))) / 2.0);
                    out.resonance = (float) std::clamp ((2.0 - 1.0 / q) / 1.9, 0.0, 1.0);
                }
            }

            // frequency envelope (articulator 4/5, additive with center 0.5, REA_rodada19 §2.1):
            // shifts by 2·(hi − lo)·amt·(y − 0.5) octaves, amt = p56/1000 (p68 on filter 2)
            const auto amount = 2.0 * (hi - lo) * std::clamp (which == 0 ? p[56] : p[68], -1000, 1000) / 1000.0;
            if (! main.art.empty() && std::abs (amount) > 0.01)
                if (const auto& art = main.art[(std::size_t) (4 + which)]; art.env.on && ! art.env.points.empty())
                {
                    const auto adsr = adsrOf (art.env, art.knobs, 1.0);
                    const auto startLevel = art.env.points.front().second;
                    double peak = startLevel;
                    for (const auto& pt : art.env.points)
                        peak = std::max (peak, pt.second);
                    const auto susLevel = adsr.s * peak;
                    const auto low = std::min (startLevel, susLevel);
                    out.cutoff = (float) std::clamp (out.cutoff * std::exp2 ((low - 0.5) * amount), 20.0, 20000.0);
                    const auto span = (peak - low) * amount;
                    if (std::abs (span) > 0.05)
                        out.filterEnv = { (float) std::clamp (span, -10.0, 10.0), (float) adsr.a, 0.0f, (float) adsr.d,
                                          (float) std::clamp ((susLevel - low) / std::max (1e-6, peak - low), 0.0, 1.0), (float) adsr.r };
                }
        }
    }

    // unison (REA_rodada24 §3): voice k detune = d_k·s cents (s = (41^(p38/256) − 1)·1.875), linear pan
    // (2k/(n − 1) − 1)·pan, per-voice gain 1 − (3^(1 − 1/n) − 1)/2. LYNX has up to 3 oscillators: with n ≤ 3 the
    // voices are exact; with more, the two outer ones and the middle one, with the gain compensating the energy of the missing ones
    if (const auto order = std::clamp (p[34], 1, 9); order >= 2)
    {
        const auto n = order;
        const auto type = std::clamp (p[35], 0, 4);
        std::vector<double> d ((std::size_t) n);
        for (int k = 0; k < n; ++k)
        {
            auto base = std::abs (2.0 * k / (n - 1) - 1.0);
            if (k % 2 == 1)
                base = -base;
            if (n % 2 == 1 && 2 * k > n)
                base = -base;
            d[(std::size_t) k] = base;
        }
        static constexpr double j0[9] { 0.306931, -0.28972, 0.392405, -0.223022, 0.508197, -0.236641, 0.43662, -0.348315, 0.274336 };
        static constexpr double j1[9] { 0.890472, 1.076426, 0.911577, 1.102536, 1.034126, 1.166861, 0.853971, 1.206273, 1.0 };
        if (type == 0 || type == 1)
        {
            double mean = 0.0;
            for (int k = 0; k < n; ++k)
            {
                d[(std::size_t) k] = type == 0 ? d[(std::size_t) k] + j0[k] / (n - 1) : d[(std::size_t) k] * j1[k];
                mean += d[(std::size_t) k] / n;
            }
            for (auto& v : d)
                v -= mean;
        }
        const auto spread = (std::pow (41.0, std::clamp (p[38], 0, 256) / 256.0) - 1.0) * 1.875;
        const auto panRaw = std::clamp (p[37], 0, 256);
        const auto pan = panRaw <= 192 ? panRaw / 192.0 : 1.0 + (panRaw - 192) * 0.046875;
        const auto voiceGain = 1.0 - (std::pow (3.0, 1.0 - 1.0 / n) - 1.0) / 2.0;
        const auto free = out.osc[(std::size_t) (1 - lead)].level < 0.05f ? 3 : 2;
        std::vector<int> picks;
        if (n <= free)
            for (int k = 0; k < n; ++k)
                picks.push_back (k);
        else if (free == 3)
            picks = { 0, n / 2, n - 1 };
        else
            picks = { 0, n - 1 };
        const auto energy = std::sqrt ((double) n / (double) picks.size()); // decorrelated voices
        const auto source = out.osc[(std::size_t) lead];
        const int slots[3] { lead, 2, 1 - lead };
        for (std::size_t i = 0; i < picks.size(); ++i)
        {
            const auto k = picks[i];
            auto voice = source;
            voice.detune = (float) std::clamp (source.detune + d[(std::size_t) k] * spread, -100.0, 100.0);
            voice.pan = (float) std::clamp ((2.0 * k / (n - 1) - 1.0) * pan, -1.0, 1.0);
            voice.level = (float) std::min (4.0, source.level * voiceGain * energy);
            out.osc[(std::size_t) slots[i]] = voice;
        }
    }

    out.porta = g[5] != 0;
    out.mono = g[4] != 0;
    // main volume (global 11) on the same curve as the part volume (REA_rodada18 §1.3)
    gain = (float) std::clamp (((mask & 4) != 0 ? harmorGain (std::clamp (g[11], 0, 2000) / 1000.0) : 1.0), 0.0, 4.0);
    if (gain <= 0.0f)
        gain = 1.0f;
    synth = out;
    return true;
}
// --- Fruity Envelope Controller (REA_rodada15 §10, REA_rodada162e-envelope.md) ---

namespace
{
constexpr std::uint32_t lynxEnvelopeMagic = 0x4345594Cu; // "LYEC"

// an FL point curve (Δx per point) as accumulated points: FL's tension belongs to the segment ending at a point,
// LYNX's to the segment starting at it (as the automation clips' import); a "hold" segment becomes a step
ControllerCurve curveFromFl (const Envelope& e, int* decayOut = nullptr)
{
    ControllerCurve c;
    double t = 0.0;
    if (decayOut != nullptr)
        *decayOut = -1;
    for (std::size_t i = 0; i < e.points.size() && c.count < (int) c.time.size(); ++i)
    {
        const auto [dx, y] = e.points[i];
        t += dx;
        if (i > 0)
        {
            const auto curve = i < e.curves.size() ? e.curves[i] : 0;
            if (curve == 2 && dx > 0.0 && c.count + 1 < (int) c.time.size()) // hold: the previous level, then a step
            {
                c.time[(std::size_t) c.count] = (float) t;
                c.level[(std::size_t) c.count] = c.level[(std::size_t) c.count - 1];
                ++c.count;
            }
            else if (curve <= 1 && i < e.tensions.size())
                c.tension[(std::size_t) c.count - 1] = std::clamp (-e.tensions[i], -1.0f, 1.0f);
        }
        if ((int) i == e.sustain)
            c.sustain = c.count;
        if (decayOut != nullptr && (int) i == e.decay)
            *decayOut = c.count;
        c.time[(std::size_t) c.count] = (float) t;
        c.level[(std::size_t) c.count] = (float) y;
        ++c.count;
    }
    return c;
}

// a map at its mode's neutral (flat 0.5 bipolar, flat 1 unipolar) is left empty
ControllerCurve mapFromFl (const Envelope& e, bool bipolar)
{
    auto c = curveFromFl (e);
    const auto neutral = bipolar ? 0.5f : 1.0f;
    bool flat = true;
    for (int i = 0; i < c.count; ++i)
        flat = flat && std::abs (c.level[(std::size_t) i] - neutral) < 1.0e-6f;
    if (flat)
        c = {};
    return c;
}

bool readCurve (Reader& r, ControllerCurve& c, int maxX)
{
    c = {};
    c.count = std::clamp (r.i32(), 0, (int) c.time.size());
    c.sustain = r.i32();
    for (int i = 0; i < c.count; ++i)
    {
        c.time[(std::size_t) i] = std::clamp (r.get<float>(), 0.0f, (float) maxX);
        c.level[(std::size_t) i] = std::clamp (r.get<float>(), 0.0f, 1.0f);
        c.tension[(std::size_t) i] = std::clamp (r.get<float>(), -1.0f, 1.0f);
        if (i > 0)
            c.time[(std::size_t) i] = std::max (c.time[(std::size_t) i], c.time[(std::size_t) i - 1]);
    }
    if (c.sustain >= c.count)
        c.sustain = -1;
    return ! r.failed;
}

std::optional<EnvelopeControllerData> parseLynxEnvelopeController (const std::vector<std::uint8_t>& state)
{
    Reader r { state.data(), state.size() };
    if ((std::uint32_t) r.i32() != lynxEnvelopeMagic)
        return std::nullopt;
    const auto version = r.i32();
    if (version < 1 || version > 2)
        return std::nullopt;
    EnvelopeControllerData out {};
    if (version >= 2)
    {
        out.modX = std::clamp (r.get<float>(), 0.0f, 1.0f);
        out.modY = std::clamp (r.get<float>(), 0.0f, 1.0f);
        out.continuous = (r.i32() & 1) != 0;
    }
    for (auto& a : out)
    {
        const auto flags = r.i32();
        a.bipolar = (flags & 1) != 0;
        a.envOn = (flags & 2) != 0;
        a.tempo = (flags & 4) != 0;
        a.lfoOn = (flags & 8) != 0;
        a.lfoTempo = (flags & 16) != 0;
        a.lfoGlobal = (flags & 32) != 0;
        a.base = std::clamp (r.get<float>(), 0.0f, 1.0f);
        a.envLevel = std::clamp (r.get<float>(), -1.0f, 1.0f);
        a.sustain = r.i32();
        a.lowKey = r.i32();
        a.highKey = r.i32();
        a.count = std::clamp (r.i32(), 0, (int) a.time.size());
        for (int i = 0; i < a.count; ++i)
        {
            a.time[(std::size_t) i] = std::max (0.0f, r.get<float>());
            a.level[(std::size_t) i] = std::clamp (r.get<float>(), 0.0f, 1.0f);
            a.tension[(std::size_t) i] = std::clamp (r.get<float>(), -1.0f, 1.0f);
            if (i > 0)
                a.time[(std::size_t) i] = std::max (a.time[(std::size_t) i], a.time[(std::size_t) i - 1]);
        }
        if (a.sustain >= a.count)
            a.sustain = -1;
        if (version >= 2)
        {
            for (std::size_t k = 0; k < a.adsr.size(); ++k) // ATT, DEC, REL: 0..256 (128 = as drawn); SUS: -128..128
                a.adsr[k] = k == 2 ? std::clamp (r.i32(), -128, 128) : std::clamp (r.i32(), 0, 256);
            a.decayPoint = r.i32();
            a.lfoLevel = std::clamp (r.get<float>(), -1.0f, 1.0f);
            a.lfoSpeed = std::clamp (r.i32(), 0, 65536);
            a.lfoTension = std::clamp (r.i32(), -128, 128);
            a.lfoSkew = std::clamp (r.i32(), -128, 128);
            a.lfoPulseWidth = std::clamp (r.i32(), -128, 128);
            a.lfoPhase = r.i32();
            if (! readCurve (r, a.lfoAmp, 100000))
                return std::nullopt;
            for (auto& m : a.maps)
                if (! readCurve (r, m, 1))
                    return std::nullopt;
            if (a.decayPoint >= a.count)
                a.decayPoint = -1;
        }
        if (r.failed)
            return std::nullopt;
    }
    return out;
}

void writeCurve (std::vector<std::uint8_t>& out, const ControllerCurve& c)
{
    auto put = [&] (const void* p, std::size_t n) { out.insert (out.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
    const auto count = std::clamp (c.count, 0, (int) c.time.size());
    const std::int32_t header[2] { count, c.sustain };
    put (header, 8);
    for (int i = 0; i < count; ++i)
    {
        const float v[3] { c.time[(std::size_t) i], c.level[(std::size_t) i], c.tension[(std::size_t) i] };
        put (v, 12);
    }
}

// FL's LFO wave shaper (REA_rodada162e-envelope.md, phase shaper of TMLFO): bounds of the falling and rising halves
struct LfoShape
{
    double end = 1.0, mid = 0.5, fall = 0.5; // u > end: 1; u < fall: 1 → −1; u < mid: −1; then −1 → 1 until end
    double t = 0.0;                          // bend (0 = straight triangle)
};

LfoShape lfoShape (const ControllerArticulator& a)
{
    LfoShape s;
    const auto pw = std::clamp (a.lfoPulseWidth, -128, 128) / 128.0;
    const auto skew = std::clamp (a.lfoSkew, -128, 128) / 256.0;
    constexpr double tiny = 1.0 / 4294967296.0;
    s.end = pw > 0.0 ? 1.0 - pw : 1.0;
    s.mid = std::clamp ((1.0 - pw) / 2.0 - skew * (1.0 - std::abs (pw)), tiny, s.end - tiny);
    s.fall = pw < 0.0 ? std::max (tiny, s.mid + pw) : s.mid;
    const auto k = std::abs (std::clamp (a.lfoTension, -128, 128)) / 128.0;
    const auto x = (std::pow (21.0, k) - 1.0) * 0.05;
    s.t = (a.lfoTension < 0 ? -1.0 : 1.0) * (std::pow (1.0e18, x) - 1.0);
    if (a.lfoTension == 0)
        s.t = 0.0;
    return s;
}

// a curve with a sustain point, `sinceOn` after the trigger (`sinceRelease` < 0 = key held)
double sustainedCurveAt (const ControllerCurve& c, double sinceOn, double sinceRelease, float fallback) noexcept
{
    if (c.count == 0)
        return fallback;
    if (c.sustain >= 0)
    {
        const auto sustainTime = (double) c.time[(std::size_t) c.sustain];
        return sinceRelease < 0.0 ? controllerCurveAt (c, std::min (sinceOn, sustainTime), fallback)
                                  : controllerCurveAt (c, sustainTime + sinceRelease, fallback);
    }
    return controllerCurveAt (c, sinceOn, fallback); // no sustain: runs to the end even with the key held
}

ControllerCurve envelopeCurve (const ControllerArticulator& a) noexcept
{
    ControllerCurve c;
    c.count = a.count;
    c.time = a.time;
    c.level = a.level;
    c.tension = a.tension;
    c.sustain = a.sustain;
    return c;
}

bool curveFinished (const ControllerCurve& c, double sinceOn, double sinceRelease) noexcept
{
    if (c.count == 0)
        return true;
    const auto last = (double) c.time[(std::size_t) c.count - 1];
    if (c.sustain >= 0)
        return sinceRelease >= 0.0 && sinceRelease >= last - c.time[(std::size_t) c.sustain];
    return sinceOn >= last;
}
} // namespace

float controllerCurveAt (const ControllerCurve& c, double x, float fallback) noexcept
{
    if (c.count <= 0)
        return fallback;
    if (x <= c.time[0])
        return c.level[0];
    for (int i = 1; i < c.count; ++i)
        if (x < c.time[(std::size_t) i])
        {
            const auto t0 = c.time[(std::size_t) i - 1], t1 = c.time[(std::size_t) i];
            auto f = t1 > t0 ? (x - t0) / (t1 - t0) : 1.0;
            if (const auto k = c.tension[(std::size_t) i - 1]; k != 0.0f) // the automation clips' curve
                f = std::pow (f, std::pow (2.0, k * 3.0));
            return (float) (c.level[(std::size_t) i - 1] + (c.level[(std::size_t) i] - c.level[(std::size_t) i - 1]) * f);
        }
    return c.level[(std::size_t) c.count - 1];
}

std::vector<std::uint8_t> writeEnvelopeController (const EnvelopeControllerData& data)
{
    std::vector<std::uint8_t> out;
    auto put = [&] (const void* p, std::size_t n) { out.insert (out.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
    auto i32 = [&] (std::int32_t v) { put (&v, 4); };
    auto f32 = [&] (float v) { put (&v, 4); };
    i32 ((std::int32_t) lynxEnvelopeMagic);
    i32 (2);
    f32 (data.modX);
    f32 (data.modY);
    i32 (data.continuous ? 1 : 0);
    for (const auto& a : data)
    {
        i32 ((a.bipolar ? 1 : 0) | (a.envOn ? 2 : 0) | (a.tempo ? 4 : 0) | (a.lfoOn ? 8 : 0) | (a.lfoTempo ? 16 : 0) | (a.lfoGlobal ? 32 : 0));
        f32 (a.base);
        f32 (a.envLevel);
        i32 (a.sustain);
        i32 (a.lowKey);
        i32 (a.highKey);
        const auto count = std::clamp (a.count, 0, (int) a.time.size());
        i32 (count);
        for (int i = 0; i < count; ++i)
        {
            f32 (a.time[(std::size_t) i]);
            f32 (a.level[(std::size_t) i]);
            f32 (a.tension[(std::size_t) i]);
        }
        for (const auto k : a.adsr)
            i32 (k);
        i32 (a.decayPoint);
        f32 (a.lfoLevel);
        i32 (a.lfoSpeed);
        i32 (a.lfoTension);
        i32 (a.lfoSkew);
        i32 (a.lfoPulseWidth);
        i32 (a.lfoPhase);
        writeCurve (out, a.lfoAmp);
        for (const auto& m : a.maps)
            writeCurve (out, m);
    }
    return out;
}

EnvelopeControllerData defaultEnvelopeController()
{
    EnvelopeControllerData out {};
    for (auto& a : out)
    {
        a.envOn = true;
        a.base = 0.0f;
        a.envLevel = 1.0f;
        a.count = 4;
        a.time = { 0.0f, 0.02f, 0.3f, 0.8f };
        a.level = { 0.0f, 1.0f, 0.6f, 0.0f };
        a.sustain = 2;
        a.decayPoint = 1;
    }
    return out;
}

std::optional<EnvelopeControllerData> parseEnvelopeController (const std::vector<std::uint8_t>& state)
{
    if (auto mine = parseLynxEnvelopeController (state))
        return mine;
    Reader r { state.data(), state.size() };
    auto version = (std::uint32_t) r.i32();
    if (version > 0xffff)
        version >>= 16;
    if (r.failed || version < 1 || version > 16)
        return std::nullopt;
    const auto articulators = version >= 6 ? 8 : 4;
    const auto maps = version >= 6 ? 5 : 2;
    std::vector<std::int32_t> params ((std::size_t) (articulators * 11 + 2));
    for (auto& p : params)
        p = r.i32(); // (the same values come in the articulators; the last two are Mod X / Mod Y)
    r.i32(); // articulator shown
    r.i32(); // part shown
    const auto globalFlags = r.i32();
    r.i32(); // ModeSelect combo

    EnvelopeControllerData out {};
    out.modX = std::clamp ((float) params[params.size() - 2] / 65536.0f, 0.0f, 1.0f);
    out.modY = std::clamp ((float) params[params.size() - 1] / 65536.0f, 0.0f, 1.0f);
    out.continuous = (globalFlags & 4) != 0;
    for (int k = 0; k < articulators && ! r.failed; ++k)
    {
        auto& a = out[(std::size_t) k];
        const auto flags = version >= 2 ? r.i32() : 0;
        a.bipolar = (flags & 1) != 0;
        r.i32(); // articulator version
        a.base = std::clamp (r.get<float>(), 0.0f, 1.0f);
        a.envLevel = std::clamp (r.get<float>(), -1.0f, 1.0f);
        a.lfoLevel = std::clamp (r.get<float>(), -1.0f, 1.0f);

        Envelope env, lfo;
        if (! readEnvelope (r, env))
            return std::nullopt;
        for (auto& knob : a.adsr)
            knob = std::clamp (r.i32(), 0, 256);
        const auto lfoVersionAt = r.pos + 4;
        if (! readEnvelope (r, lfo))
            return std::nullopt;
        std::int32_t lfoVersion = 0;
        std::memcpy (&lfoVersion, r.data + lfoVersionAt, 4);
        a.lfoSpeed = std::clamp (r.i32(), 0, 65536);
        a.lfoTension = std::clamp (r.i32(), -128, 128);
        a.lfoSkew = std::clamp (r.i32(), -128, 128);
        a.lfoPulseWidth = std::clamp (r.i32(), -128, 128);
        if (lfoVersion >= 3)
            a.lfoPhase = r.i32();
        for (int m = 0; m < maps; ++m)
        {
            Envelope map;
            if (! readEnvelope (r, map))
                return std::nullopt;
            a.maps[(std::size_t) m] = mapFromFl (map, a.bipolar);
        }
        a.lowKey = r.i32();
        a.highKey = r.i32();

        a.envOn = env.on && ! env.points.empty();
        a.tempo = (env.flags & 1) != 0;
        const auto curve = curveFromFl (env, &a.decayPoint);
        a.count = curve.count;
        a.time = curve.time;
        a.level = curve.level;
        a.tension = curve.tension;
        a.sustain = curve.sustain;

        a.lfoOn = lfo.on;
        a.lfoTempo = (lfo.flags & 1) != 0;
        a.lfoGlobal = (lfo.flags & 2) != 0;
        a.lfoAmp = curveFromFl (lfo);
        // a constant full amplitude is FL's default: keep it empty (= 1)
        if (a.lfoAmp.count == 1 && a.lfoAmp.level[0] >= 1.0f)
            a.lfoAmp = {};
    }
    if (r.failed)
        return std::nullopt;
    return out;
}

float controllerValue (const ControllerArticulator& a, double sinceOn, double sinceRelease) noexcept
{
    if (! a.envOn || a.count == 0)
        return a.base;
    const auto env = sustainedCurveAt (envelopeCurve (a), sinceOn, sinceRelease, 0.0f);
    const auto v = a.bipolar ? a.base + (env - 0.5) * a.envLevel : a.base + env * a.envLevel;
    return (float) std::clamp (v, 0.0, 1.0);
}

float controllerVelocityInput (float velocity) noexcept
{
    const auto v = std::clamp ((double) velocity, 0.0, 1.0);
    const auto initVol = (std::pow (11.0, 1.28 * v) - 1.0) / 10.0;
    return (float) (std::log (1.0 + 10.0 * initVol) / std::log (21.551182) * 127.0 / 128.0);
}

double controllerLfoRate (const ControllerArticulator& a) noexcept
{
    return (std::pow (10001.0, std::clamp (a.lfoSpeed, 0, 65536) / 65536.0) - 1.0) / 625.0;
}

float controllerLfoWave (const ControllerArticulator& a, double phase) noexcept
{
    const auto s = lfoShape (a);
    const auto u = phase - std::floor (phase);
    double c = 0.0;
    if (u > s.end)
        return 1.0f;
    if (u < s.fall)
        c = 1.0 - 2.0 * u / s.fall;
    else if (u < s.mid)
        return -1.0f;
    else
        c = 2.0 * (u - s.mid) / std::max (1.0e-12, s.end - s.mid) - 1.0;
    if (s.t == 0.0)
        return (float) c;
    const auto m = std::abs (c);
    const auto t = std::abs (s.t);
    const auto y = s.t > 0.0 ? 1.0 - (std::pow (t + 1.0, 1.0 - m) - 1.0) / t : (std::pow (t + 1.0, m) - 1.0) / t;
    return (float) std::copysign (y, c);
}

double controllerLfoStartPhase (const ControllerArticulator& a) noexcept
{
    const auto s = lfoShape (a);
    return s.mid + (s.end - s.mid) * 0.5 + a.lfoPhase / 4294967296.0;
}

float controllerMapValue (const ControllerArticulator& a, int map, float x) noexcept
{
    if (map < 0 || map >= (int) a.maps.size() || a.maps[(std::size_t) map].count == 0)
        return a.bipolar ? 0.0f : 1.0f;
    const auto y = controllerCurveAt (a.maps[(std::size_t) map], std::clamp ((double) x, 0.0, 1.0), 0.5f);
    return a.bipolar ? y - 0.5f : y;
}

float controllerValue (const ControllerArticulator& a, double sinceOn, double sinceRelease, double lfoPhase,
                       const ControllerNote& note, float globalModX, float globalModY) noexcept
{
    double v = a.base;
    if (a.envOn && a.count > 0)
    {
        const auto env = sustainedCurveAt (envelopeCurve (a), sinceOn, sinceRelease, 0.0f);
        v += (a.bipolar ? env - 0.5 : env) * a.envLevel;
    }
    if (a.lfoOn)
    {
        const auto out = sustainedCurveAt (a.lfoAmp, sinceOn, sinceRelease, 1.0f) * controllerLfoWave (a, lfoPhase);
        v = a.bipolar ? v + out * a.lfoLevel : v * (out * a.lfoLevel * 2.0 + 1.0);
    }
    v = controllerCombine (a, (float) v, controllerMapValue (a, mapKeyboard, note.key / 120.0f));
    v = controllerCombine (a, (float) v, controllerMapValue (a, mapModX, globalModX + note.modX - 0.5f));
    v = controllerCombine (a, (float) v, controllerMapValue (a, mapModY, globalModY + note.modY - 0.5f));
    // the note's velocity and random maps (FL computes them once at the trigger)
    const auto noteFactor = controllerCombine (a, controllerMapValue (a, mapVelocity, controllerVelocityInput (note.velocity)),
                                               controllerMapValue (a, mapRandom, note.random));
    v = controllerCombine (a, (float) v, noteFactor);
    return (float) std::clamp (v, 0.0, 1.0);
}

float controllerRestingValue (const ControllerArticulator& a, float globalModX, float globalModY) noexcept
{
    auto v = controllerCombine (a, a.base, controllerMapValue (a, mapModX, globalModX));
    v = controllerCombine (a, v, controllerMapValue (a, mapModY, globalModY));
    return std::clamp (v, 0.0f, 1.0f);
}

ControllerArticulator controllerWithKnobs (const ControllerArticulator& a) noexcept
{
    auto out = a;
    if (a.adsr == std::array<int, 4> { 128, 128, 0, 128 } || a.count < 2)
        return out;
    const auto decayPoint = std::max (0, a.decayPoint);
    const auto sustain = a.sustain >= 0 ? a.sustain : a.count - 1;
    const auto scale = [&] (int knob) { return std::clamp (knob, 0, 256) / 128.0f; };
    float t = 0.0f;
    for (int i = 1; i < a.count; ++i)
    {
        const auto dt = a.time[(std::size_t) i] - a.time[(std::size_t) i - 1];
        t += dt * (i <= decayPoint ? scale (a.adsr[0]) : i <= sustain ? scale (a.adsr[1]) : scale (a.adsr[3]));
        out.time[(std::size_t) i] = t;
    }
    if (a.adsr[2] != 0 && a.sustain >= 0)
    {
        const auto shift = std::clamp (a.adsr[2], -256, 256) / 128.0f;
        const auto decayStart = a.time[(std::size_t) std::min (decayPoint, sustain)];
        const auto decayLength = a.time[(std::size_t) sustain] - decayStart;
        const auto releaseLength = a.time[(std::size_t) a.count - 1] - a.time[(std::size_t) sustain];
        for (int i = std::min (decayPoint, sustain) + 1; i < a.count; ++i)
        {
            const auto w = i <= sustain ? (decayLength > 0.0f ? (a.time[(std::size_t) i] - decayStart) / decayLength : 1.0f)
                                        : (releaseLength > 0.0f ? (a.time[(std::size_t) a.count - 1] - a.time[(std::size_t) i]) / releaseLength : 0.0f);
            out.level[(std::size_t) i] = std::clamp (a.level[(std::size_t) i] + w * shift, 0.0f, 1.0f);
        }
    }
    return out;
}

bool controllerVoiceFinished (const ControllerArticulator& a, double sinceOn, double sinceRelease) noexcept
{
    if (sinceRelease < 0.0)
        return false;
    const auto envDone = ! a.envOn || curveFinished (envelopeCurve (a), sinceOn, sinceRelease);
    const auto lfoDone = ! a.lfoOn || curveFinished (a.lfoAmp, sinceOn, sinceRelease);
    return envDone && lfoDone;
}
double lovePhilterMapAt (const LovePhilterMap& map, double x) noexcept
{
    if (map.points.empty())
        return 1.0;
    double t = 0.0, prevT = 0.0, prevY = map.points.front().second;
    for (std::size_t i = 0; i < map.points.size(); ++i)
    {
        t += map.points[i].first;
        const auto y = map.points[i].second;
        if (x < t)
            return i == 0 ? y : prevY + (y - prevY) * (x - prevT) / std::max (1e-12, t - prevT);
        prevT = t;
        prevY = y;
    }
    return prevY;
}

std::optional<KeyboardControllerData> parseKeyboardController (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 553)
        return std::nullopt;
    std::uint32_t version = 0;
    std::memcpy (&version, d.data(), 4);
    if (version < 2 || version > 3)
        return std::nullopt; // v0/v1 have a different layout (120 keys, a single time)
    KeyboardControllerData out;
    std::memcpy (out.keys.data(), d.data() + 12, 132 * 4);
    for (auto& v : out.keys)
        v = std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f;
    std::memcpy (&out.noteOffValue, d.data() + 548, 4);
    out.noteOffValue = std::isfinite (out.noteOffValue) ? std::clamp (out.noteOffValue, 0.0f, 1.0f) : 0.0f;
    out.autoRelease = d[552] != 0;
    return out;
}
void writeKeyboardController (const KeyboardControllerData& data, std::vector<std::uint8_t>& d)
{
    if (! parseKeyboardController (d))
    {
        d.assign (554, 0);
        const std::uint32_t version = 3;
        const std::int32_t smoothing = 32; // attack and release (FL's default)
        std::memcpy (d.data(), &version, 4);
        std::memcpy (d.data() + 4, &smoothing, 4);
        std::memcpy (d.data() + 8, &smoothing, 4);
    }
    for (std::size_t k = 0; k < data.keys.size(); ++k)
    {
        const auto v = std::clamp (std::isfinite (data.keys[k]) ? data.keys[k] : 0.0f, 0.0f, 1.0f);
        std::memcpy (d.data() + 12 + k * 4, &v, 4);
    }
    const auto off = std::clamp (std::isfinite (data.noteOffValue) ? data.noteOffValue : 0.0f, 0.0f, 1.0f);
    std::memcpy (d.data() + 548, &off, 4);
    d[552] = data.autoRelease ? 1 : 0;
}

std::optional<LovePhilterState> parseLovePhilter (const std::vector<std::uint8_t>& blob)
{
    if (blob.size() < 8)
        return std::nullopt;
    std::uint32_t version = 0;
    std::memcpy (&version, blob.data(), 4);
    if (version > 0xffff)
        version >>= 16;
    if (version < 1 || version > 12)
        return std::nullopt;
    const auto body = inflateZlib (blob.data() + 4, blob.size() - 4, false, 8u << 20);
    if (! body)
        return std::nullopt;

    LovePhilterState out;
    out.version = (int) version;
    const auto v = out.version;
    Reader r { body->data(), body->size() };
    for (auto& g : out.globals)
        g = r.i32();
    const auto globals2 = (std::size_t) (v >= 11 ? 0x21 : v == 10 ? 0x20 : 0x1F);
    if (const auto* g2 = r.raw (globals2))
    {
        std::memcpy (&out.modX, g2, 4);
        std::memcpy (&out.modY, g2 + 4, 4);
        std::int32_t solo = -1;
        std::memcpy (&solo, g2 + 16, 4);
        out.soloUnit = solo >= 0 && solo < 8 ? solo : -1;
    }
    if (v >= 2)
    {
        r.u8();
        r.i32();
    }
    if (v < 6)
    {
        const auto n = r.i32();
        if (n < 0)
            return std::nullopt;
        r.raw ((std::size_t) n);
    }
    else
    {
        std::size_t n = r.u8();
        if (n == 255)
        {
            const auto big = r.i32();
            if (big < 0)
                return std::nullopt;
            n = (std::size_t) big;
        }
        r.raw (2 * n);
    }

    for (auto& unit : out.units)
    {
        for (auto& p : unit.params)
            p = r.i32();
        unit.mode = r.i32();
        unit.type = r.i32();
        unit.alt = r.u8() != 0;
        unit.enabled = r.u8() != 0;
        unit.waveshaper = r.u8() != 0;
        Envelope e;
        if (! readEnvelope (r, e)) // waveshaper curve
            return std::nullopt;
        r.u8();
        for (auto& a : unit.articulators)
        {
            r.i32(); // version
            a.base = r.get<float>();
            a.envDepth = r.get<float>();
            a.lfoDepth = r.get<float>();
            // only the envelope of the selected pattern (global 5, PatNum 1…10) plays (REA_rodada20 §1.2)
            const auto patNum = std::clamp (out.globals[5], 1, 10) - 1;
            for (int pattern = 0; pattern < 10; ++pattern)
            {
                if (! readEnvelope (r, e))
                    return std::nullopt;
                if (pattern == patNum && e.on)
                {
                    a.pattern.on = true;
                    a.pattern.tempo = (e.flags & 1) != 0;
                    for (std::size_t k = 0; k < e.points.size(); ++k)
                        a.pattern.points.push_back ({ e.points[k].first, e.points[k].second, e.tensions[k], e.curves[k] });
                }
                r.raw (16);
            }
            const auto lfoVersionAt = r.pos + 4;
            if (! readEnvelope (r, e))
                return std::nullopt;
            std::int32_t lfoVersion = 0;
            std::memcpy (&lfoVersion, r.data + lfoVersionAt, 4);
            if (e.on)
            {
                a.lfo.on = true;
                a.lfo.tempo = (e.flags & 1) != 0;
                a.lfo.frozen = (e.flags & 8) != 0;
                a.lfo.amplitude = e.points.empty() ? 0.5f : (float) (e.points.front().second - 0.5);
            }
            const auto knobsAt = r.pos;
            r.raw (lfoVersion >= 3 ? 20 : 16);
            if (! r.failed && e.on)
            {
                std::int32_t k[5] {};
                std::memcpy (k, r.data + knobsAt, lfoVersion >= 3 ? 20 : 16);
                a.lfo.speed = k[0];
                a.lfo.tension = k[1];
                a.lfo.skew = k[2];
                a.lfo.width = k[3];
                a.lfo.phase = (std::uint32_t) k[4];
            }
            for (int m = 0; m < 3; ++m) // X, Y, envelope follower
            {
                if (! readEnvelope (r, e))
                    return std::nullopt;
                if (m == 0)
                    a.mapX.points = e.points;
                else if (m == 1)
                    a.mapY.points = e.points;
            }
        }
        if (r.failed)
            return std::nullopt;
    }
    if (v >= 12)
        r.u8();
    if (r.failed || r.pos != body->size())
        return std::nullopt;
    return out;
}
} // namespace lynx
