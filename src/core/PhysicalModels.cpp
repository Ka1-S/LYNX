#include "core/PhysicalModels.h"
#include "core/HostSlide.h"
#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace lynx
{
namespace
{
constexpr double pi = 3.141592653589793;

std::uint32_t lcg (std::uint32_t s) noexcept { return s * 0x0BB38435u + 0x3619636Bu; }
double clamp01 (double x) noexcept { return std::clamp (x, 0.0, 1.0); }

// 2-pole high-pass used by both engines (REA_rodada28 §1.4): numerator (1, −2, 1)·b, poles a1/a2
struct HighPass
{
    double b = 0.0, a1 = 0.0, a2 = 0.0, x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    void design (double f, double q, double fs) noexcept
    {
        retune (f, q, fs);
        x1 = x2 = y1 = y2 = 0.0;
    }

    // new coefficients, state kept
    void retune (double f, double q, double fs) noexcept
    {
        const auto w = 2.0 * pi * f / fs, s = std::sin (w), c = std::cos (w);
        b = (1.0 + c) / (s / q + 2.0);
        a1 = -2.0 * q * c / (q + s);
        a2 = (q - s) / (q + s);
    }

    double tick (double x) noexcept
    {
        const auto y = (x - 2.0 * x1 + x2) * b - a2 * y2 - a1 * y1;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// Settings handed from configure() (UI thread) to the audio thread without blocking
template <typename T>
class Mailbox
{
public:
    void write (const T& value) noexcept
    {
        bool expected = false;
        while (! busy.compare_exchange_weak (expected, true))
            expected = false;
        pending = value;
        fresh = true;
        busy.store (false);
    }

    bool read (T& out) noexcept
    {
        bool expected = false;
        if (! busy.compare_exchange_strong (expected, true))
            return false;
        const auto had = fresh;
        if (had)
            out = pending;
        fresh = false;
        busy.store (false);
        return had;
    }

private:
    std::atomic<bool> busy { false };
    T pending {};
    bool fresh = false;
};

// Linear resampler from the engine's internal rate to the host rate (one per instrument, shared by its voices)
template <typename Source>
void renderResampled (Source& source, double internalRate, double hostRate, double& position, float* left, float* right,
                      int n, float gl, float gr, std::array<float, 2>& previous, std::array<float, 2>& current) noexcept
{
    const auto step = internalRate / hostRate;
    for (int i = 0; i < n; ++i)
    {
        while (position >= 1.0)
        {
            previous = current;
            current = source.tick();
            position -= 1.0;
        }
        const auto f = (float) position;
        left[i] += (previous[0] + (current[0] - previous[0]) * f) * gl;
        right[i] += (previous[1] + (current[1] - previous[1]) * f) * gr;
        position += step;
    }
}

// ---------------------------------------------------------------------------
// Sakura (REA_rodada28 §1, REA_rodada29 §1): exciter → 4 string loops (near/far × 2 strings) → combs → stereo mix
// → amp envelope, at 48 kHz, with a per-voice modulation EG, one global LFO and the 8-slot modulation matrix
// re-evaluated per internal chunk (≤ 48 samples). Then, at the host rate: 32 Hz high-pass → resonator bank →
// chorus → reverb → delay → master gain.

using SakuraParams = std::array<float, sakuraParamCount>;

double f32 (double x) noexcept { return (double) (float) x; }
double flushTiny (double x) noexcept { return x > -1e-9 && x < 1e-9 ? 0.0 : x; }

// Modulation envelope (REA_rodada29 §1.1): straight segments, stepped once per internal chunk of n samples (the
// phase is advanced before the value is computed); output clamped at ≥ 0 and stored as float.
struct SakuraEg
{
    double ia = 0.0, id = 0.0, ir = 0.0, sustain = 0.0, start = 0.0, phase = 0.0, out = 0.0, releaseLevel = 0.0;
    int stage = 0;

    void begin (double a, double d, double s) noexcept
    {
        ia = (1.0 / 48000.0) / std::max (a, 0.001);
        id = (1.0 / 48000.0) / std::max (d, 0.001);
        ir = 1.0 / (0.001 * 48000.0);
        sustain = s;
        start = phase = out = releaseLevel = 0.0; // a fresh voice's EG is idle: the attack starts from 0
        stage = 0;
    }

    void release (double r) noexcept
    {
        if (stage == 2)
            return;
        ir = 1.0 / (std::max (r, 0.001) * 48000.0);
        releaseLevel = out;
        stage = 2;
        phase = 0.0;
    }

    double tick (int n) noexcept
    {
        double v;
        if (stage == 0)
        {
            phase += n * ia;
            if (phase >= 1.0)
                v = 1.0, stage = 1, phase = 0.0;
            else
                v = (1.0 - start) * phase + start;
        }
        else if (stage == 1)
        {
            phase += n * id;
            if (phase >= 1.0)
            {
                if (sustain == 0.0)
                    stage = 2; // ends here (the release segment then falls from 0)
                v = sustain;
                phase = 1.0;
            }
            else
                v = (sustain - 1.0) * phase + 1.0;
        }
        else
        {
            phase += n * ir;
            if (phase >= 1.0)
                v = 0.0, phase = 1.0;
            else
                v = (0.0 - releaseLevel) * phase + releaseLevel;
        }
        out = f32 (std::max (v, 0.0));
        return out;
    }
};

struct SakuraVoice
{
    bool active = false;
    int key = 0;
    // per-note
    int lens[4] {}, combLen[2] {};
    std::array<std::vector<float>, 4> loops;
    std::array<std::vector<float>, 2> combs;
    int ptr[4] {}, combPtr[2] {};
    double kdc[4] {}, freq = 0.0, rate = 1.0, velocity = 0.0;
    // matrix sources as of the last chunk: EG, LFO × depth, release gate
    SakuraEg eg;
    double egValue = 0.0, lfoValue = 0.0, gate = 0.0;
    // per-chunk laws
    double step = 0.0, threshold = 1.0, ladderG = 0.0, ladderR = 0.0;
    double coef[4] {}, sharp[4] {}, fbTarget[4] {}, gainTarget[4] {};
    double hcTarget = 0.0, hcRate = 0.0, envTarget = 0.0, envRate = 0.0;
    // state
    std::uint32_t seed = 0;
    double hold = 0.0, s[3] {}, z4 = 0.0, hiCutEnv = 0.0;
    double mean[4] {}, slow[4] {}, z[4] {};
    double fbs[4] {}, gains[4] {};
    double acc = 0.0, prev[2] {}, cur[2] {};
    double env = 0.0, elapsed = 0.0, release = -1.0;
    HighPass lowCut;
    // FL's Final.Pitch beyond the key (issue #14): the channel's PITCH knob + fine / main pitch at note on (cents) and
    // a slide note's glide (round 28 §4: T_note = Final.Pitch/100 − note + 60 shifts the string)
    float noteCents = 0.0f;
    lynx::HostSlide slide;
};

// ---- effects (REA_rodada29 §1.4–1.8): all at the host rate, float arithmetic where the engine rounds ----

// One half of a "tape": samples with their tape positions (stamps). Indices 0…2 are a guard holding the tail of
// the page before it (stamps shifted by −1), so slot i of the page lives at i + guard.
struct TapePage
{
    static constexpr int guard = 3;
    std::vector<float> value;
    std::vector<double> stamp;
    int count = 0, read = 0;

    void allocate (int capacity)
    {
        value.assign ((std::size_t) capacity + 8, 0.0f);
        stamp.assign ((std::size_t) capacity + 8, 0.0);
        count = read = 0;
    }

    void clear() noexcept
    {
        std::fill (value.begin(), value.end(), 0.0f);
        std::fill (stamp.begin(), stamp.end(), 0.0);
        count = read = 0;
    }
};

// Common delay element (§1.4): reads the previous lap at the current tape position (linear interpolation between
// stamps), writes the new sample stamped with the advanced position, swaps the pages at each lap.
struct Tape
{
    TapePage pages[2];
    int writing = 0;
    double position = 0.0;

    void allocate (int length)
    {
        pages[0].allocate (length);
        pages[1].allocate (length);
        writing = 0;
    }

    void clear() noexcept
    {
        pages[0].clear();
        pages[1].clear();
        writing = 0;
    }

    double readAt (double pos) noexcept
    {
        constexpr int g = TapePage::guard;
        auto& pg = pages[1 - writing];
        while (pg.count != 0)
        {
            const auto r = pg.read;
            if (pg.count + 1 <= r)
                break;
            const auto t = pg.stamp[(std::size_t) (r + g)];
            if (pos <= t)
            {
                const auto f5 = f32 (pg.stamp[(std::size_t) (r - 1 + g)]), f3 = f32 (t);
                auto fr = 0.0;
                if (f5 < f3)
                    fr = (pos - f5) / f32 (f3 - f5);
                const auto v0 = (double) pg.value[(std::size_t) (r - 1 + g)];
                return flushTiny (f32 (f32 ((double) pg.value[(std::size_t) (r + g)] - v0) * fr + v0));
            }
            pg.read = r + 1;
        }
        return 0.0;
    }

    // writes w (already float) at stamp newPosition; returns the new tape position
    double write (double w, double newPosition) noexcept
    {
        constexpr int g = TapePage::guard;
        if (newPosition >= 1.0)
        {
            auto& wr = pages[writing];
            auto& rd = pages[1 - writing];
            const auto n = rd.count;
            for (int k = 1; k <= 3; ++k)
            {
                wr.value[(std::size_t) (g - k)] = rd.value[(std::size_t) (n - k + g)];
                wr.stamp[(std::size_t) (g - k)] = rd.stamp[(std::size_t) (n - k + g)] - 1.0;
            }
            wr.value[(std::size_t) (wr.count + g)] = (float) w;
            wr.stamp[(std::size_t) (wr.count + g)] = newPosition;
            wr.read = 0;
            writing = 1 - writing;
            newPosition -= 1.0;
            pages[writing].count = 0;
        }
        auto& wr = pages[writing];
        wr.value[(std::size_t) (wr.count + g)] = (float) w;
        wr.stamp[(std::size_t) (wr.count + g)] = newPosition;
        ++wr.count;
        return newPosition;
    }
};

// Modulated tape delay (CTapeDelay): speed × (1 + depth·t²(3 − 2t)) with a triangle LFO, speed smoothing 0.005 and
// wet smoothing 0.01 per sample, written value = input + feedback × read, output = read × wet.
struct TapeDelay
{
    Tape tape;
    int length = 4;
    double startPosition = 0.0, minSpeed = 0.25, speed = 0.0, wet = 1.0, feedback = 0.0, increment = 0.0, depth = 0.0;
    double speedSmoothed = 0.0, wetSmoothed = 0.0, phase = 0.0;

    void allocate (int n, double pos0)
    {
        length = std::max (4, n);
        startPosition = pos0;
        minSpeed = 1.0 / length;
        tape.allocate (length);
        restart();
    }

    // state of a freshly built line (time 0.5, wet 1, no feedback/modulation)
    void restart() noexcept
    {
        setTime (0.5);
        wet = 1.0;
        feedback = increment = depth = 0.0;
        tape.position = startPosition;
        speedSmoothed = speed;
        wetSmoothed = phase = 0.0;
        tape.clear();
    }

    void setTime (double p) noexcept
    {
        p = std::clamp (p, 0.0, 0.99);
        const auto a = 2.0 / length;
        speed = 1.0 / ((p * (1.0 - a) + a) * length);
    }

    void setRate (double hz, double fs) noexcept { increment = std::max (hz, 0.0) / fs; }
    void setDepth (double d) noexcept { depth = f32 (std::clamp (d, 0.0, 1.0)); }

    double tick (double x) noexcept
    {
        x = flushTiny (x);
        auto ph = phase;
        const auto tri = ph < 0.5 ? ph + ph : 1.0 - (((ph - 0.5) + ph) - 0.5);
        ph += increment;
        auto mult = (3.0 - (tri + tri)) * tri * (depth * tri) + 1.0;
        if (mult <= 1.0)
            mult = 1.0;
        if (ph >= 1.0)
            ph -= (int) ph;
        phase = ph;
        const auto val = tape.readAt (tape.position);
        const auto spd = std::max (std::min (mult * speedSmoothed, 0.2), minSpeed);
        const auto w = f32 (f32 (val * feedback) + x);
        const auto out = f32 (val * wetSmoothed);
        tape.position = tape.write (w, tape.position + spd);
        const auto ws = f32 (f32 (wet - wetSmoothed) * 0.01 + wetSmoothed);
        wetSmoothed = ws > -1e-9 && ws < 1e-9 ? 0.0 : ws;
        speedSmoothed = (speed - speedSmoothed) * 0.005 + speedSmoothed;
        return out;
    }
};

// Resonator unit (CStringDelay): constant-speed tape with a 1-pole low-pass (0.5) in the feedback; output = read
struct StringDelay
{
    Tape tape;
    int length = 4;
    double minSpeed = 0.25, feedback = 0.0, speed = 0.0, z = 0.0;

    void allocate (int n)
    {
        length = std::max (4, n);
        minSpeed = 1.0 / length;
        tape.allocate (length);
        restart();
    }

    void restart() noexcept
    {
        setTime (0.5);
        tape.position = z = 0.0;
        tape.clear();
    }

    void setTime (double p) noexcept
    {
        p = std::clamp (p, 0.0, 0.99);
        const auto a = 2.0 / length;
        speed = std::max (std::min (1.0 / ((p * (1.0 - a) + a) * length), 0.2), minSpeed);
    }

    double tick (double x) noexcept
    {
        x = flushTiny (x);
        const auto val = tape.readAt (tape.position);
        const auto next = tape.position + speed;
        z = f32 (f32 (val - z) * 0.5 + z);
        tape.position = tape.write (f32 (f32 (z * feedback) + x), next);
        return val;
    }
};

// 2-pole high-pass of the effects (float version of HighPass)
struct HighPassF
{
    double b = 0.0, a1 = 0.0, a2 = 0.0, x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    void design (double f, double q, double fs) noexcept
    {
        const auto w = 2.0 * pi * f / fs, s = f32 (std::sin (f32 (w))), c = f32 (std::cos (f32 (w)));
        const auto k = f32 (1.0 / f32 (s + q));
        b = f32 (f32 (c + 1.0) / f32 (f32 (s / q) + 2.0));
        a1 = f32 (f32 (c * k) * q * -2.0);
        a2 = f32 (f32 (q - s) * k);
        x1 = x2 = y1 = y2 = 0.0;
    }

    double tick (double x) noexcept
    {
        x = flushTiny (x);
        const auto y = flushTiny (f32 (f32 (f32 (f32 (f32 (x - f32 (x1 + x1)) + x2) * b) - f32 (y1 * a1)) - f32 (a2 * y2)));
        x2 = x1;
        y2 = y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// Resonator bank (§1.5): 8 resonators on L + R, each two string delays in series (time p and 1.05·p, the second
// fed the negated output of the first); odd/even resonators weighted 0.75/1.25 per side, 32 Hz HP, added to the dry.
struct SakuraResonators
{
    StringDelay a[8], b[8];
    HighPassF hp[2];
    bool on[8] {};
    double time[8] {}, feedback[8] {};

    void allocate (double fs)
    {
        for (int k = 0; k < 8; ++k)
            a[k].allocate ((int) (fs * 0.1)), b[k].allocate ((int) (fs * 0.1));
        hp[0].design (32.0, 1.0, fs);
        hp[1].design (32.0, 1.0, fs);
    }

    void restart (double fs) noexcept
    {
        for (int k = 0; k < 8; ++k)
            a[k].restart(), b[k].restart(), on[k] = false;
        hp[0].design (32.0, 1.0, fs);
        hp[1].design (32.0, 1.0, fs);
    }

    void setParameters (const SakuraParams& p) noexcept
    {
        for (int k = 0; k < 8; ++k)
        {
            const auto enable = p[(std::size_t) (55 + k)] != 0.0f;
            if (enable && ! on[k]) // switching a resonator on clears both of its units
                a[k].restart(), b[k].restart();
            on[k] = enable;
            time[k] = f32 (1.0 - f32 (std::pow ((double) p[(std::size_t) (39 + k)], 0.1)));
            feedback[k] = f32 (std::sqrt (std::max (0.0, (double) p[(std::size_t) (47 + k)])) * 0.9999);
        }
    }

    void process (double* left, double* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const auto x = f32 (left[i] + right[i]);
            double sl = 0.0, sr = 0.0;
            for (int k = 0; k < 8; ++k)
            {
                if (! on[k])
                    continue;
                a[k].setTime (time[k]);
                b[k].setTime (time[k] * 1.05);
                a[k].feedback = b[k].feedback = feedback[k];
                const auto y = b[k].tick (-a[k].tick (x));
                sl = f32 (sl + ((k & 1) != 0 ? 1.25 : 0.75) * y);
                sr = f32 (sr + ((k & 1) != 0 ? 0.75 : 1.25) * y);
            }
            left[i] = f32 (hp[0].tick (sl) + left[i]);
            right[i] = f32 (hp[1].tick (sr) + right[i]);
        }
    }
};

// Chorus (§1.6): three unfed tape delays (20 ms × 1.1 / 0.854 / 1) on L + R; L += b + a, R += b + c.
// Wet polarity: FL stores −Wet and re-applies it at every rebuild, so the sign flips with each later sample-rate
// change; LYNX uses +Wet (state applied after the sample rate is set), as recommended in REA_rodada29 §5.1.
struct SakuraChorus
{
    TapeDelay a, b, c;

    void allocate (double fs)
    {
        const auto d = fs * (double) 0.02f; // a float constant promoted to double
        a.allocate ((int) (d * 1.1), 0.0);
        b.allocate ((int) (d * 0.854), 0.467);
        c.allocate ((int) d, 0.778);
    }

    void restart() noexcept { a.restart(), b.restart(), c.restart(); }

    void setParameters (const SakuraParams& p, double fs) noexcept
    {
        const auto r = (double) p[71] * 5.0 + 0.1;
        a.setRate (r * 0.896, fs);
        b.setRate (r, fs);
        c.setRate (r * 0.689, fs);
        const auto depth = std::pow ((double) p[72], 3.0);
        for (auto* t : { &a, &b, &c })
            t->setDepth (depth), t->wet = (double) p[73];
    }

    void process (double* left, double* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const auto x = f32 (left[i] + right[i]);
            const auto ya = a.tick (x), yc = c.tick (x), yb = b.tick (x);
            left[i] = f32 (f32 (yb + ya) + left[i]);
            right[i] = f32 (f32 (yb + yc) + right[i]);
        }
    }
};

// Delay (§1.7): per channel three tape delays in series-additive form y ← y + D(y); feedback −0.99·Fdbk², wet
// Wet² × 1 / Blur / Blur², optional tempo sync (index 112).
struct SakuraDelay
{
    TapeDelay lines[6];

    void allocate (double fs)
    {
        const auto n = (double) (int) (fs + fs);
        lines[0].allocate ((int) n, 0.0);
        lines[1].allocate ((int) (n * 0.63), 0.27);
        lines[2].allocate ((int) (n * 0.41), 0.4);
        lines[3].allocate ((int) n, 0.3);
        lines[4].allocate ((int) (n * 0.76), 0.62);
        lines[5].allocate ((int) (n * 0.54), 0.87);
    }

    void restart() noexcept
    {
        for (auto& l : lines)
            l.restart();
    }

    void setParameters (const SakuraParams& p, double fs, double bpm) noexcept
    {
        const auto time = (double) p[64], fb = (double) p[65], spd = (double) p[66], dep = (double) p[67];
        const auto blur = (double) p[68], wet = (double) p[69];
        const auto sync = (int) p[112];
        auto t = f32 (f32 (time * time) + 0.01);
        if (sync >= 1 && sync <= 9 && bpm > 0.0)
        {
            static constexpr double ratio[9] { 1.0 / 15, 1.0 / 30, 1.0 / 45, 1.0 / 60, 1.0 / 120, 1.0 / 180, 1.0 / 240, 1.0 / 480,
                                               1.0 / 960 };
            t = f32 (0.5 / (bpm * ratio[sync - 1]));
        }
        const auto r0 = f32 (spd * spd) * 20.0;
        const auto w2 = f32 (wet * wet);
        const double wets[3] { w2, f32 (w2 * blur), f32 (f32 (blur * blur) * w2) };
        for (int k = 0; k < 6; ++k)
        {
            auto& l = lines[k];
            l.setRate ((k & 1) != 0 ? r0 * 1.1 : r0, fs);
            l.setTime (t);
            l.feedback = f32 (f32 (fb * fb) * -0.99);
            l.setDepth (f32 (dep * dep * dep * dep));
            l.wet = wets[k % 3];
        }
    }

    void process (double* left, double* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            auto y = left[i];
            y = f32 (y + lines[0].tick (y));
            y = f32 (y + lines[1].tick (y));
            left[i] = f32 (lines[2].tick (y) + y);
            y = right[i];
            y = f32 (y + lines[3].tick (y));
            y = f32 (y + lines[4].tick (y));
            right[i] = f32 (lines[5].tick (y) + y);
        }
    }
};

// Reverb (§1.8): plate-style tank (4 input all-passes, predelay, two cross-coupled halves with bass shelf, damping,
// decay all-passes and delays, 14 output taps) plus one early-reflection tap per side. Float arithmetic as the
// engine's. Sakura fixes 15 of its 18 parameters; Mix, Decay and Color drive the other three.
class SakuraReverb
{
public:
    void allocate (double sampleRate)
    {
        fs = (float) sampleRate;
        fsi = (int) sampleRate;
        defaults();
        const auto room = (long long) toInt (4, q[4]);
        const auto S = ((room + 15) * fsi) / 115;
        static constexpr int tank[12] { 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };
        for (int j = 0; j < 12; ++j)
        {
            len[j] = std::max (1, (int) ((S * tank[j]) / 20000));
            buf[j].assign ((std::size_t) len[j], 0.0f);
        }
        static constexpr int taps[14] { 266, 2974, 1913, 2496, 1690, 187, 1566, 353, 3627, 1728, 3173, 1811, 335, 621 };
        for (int k = 0; k < 14; ++k)
            tap[k] = (int) ((S * taps[k]) / 20000);
        const auto sc = (float) (((room + 3) * fsi) / 103);
        erLeft = (int) ((float) (1102.0f * 3.4013603e-05f) * sc);
        erRight = (int) ((float) (837.0f * 3.4013603e-05f) * sc);
        const auto m = (float) std::min (erLeft, erRight);
        gainErLeft = (m * 0.5f) / (float) erLeft;
        gainErRight = (m * 0.36f) / (float) erRight;
        int bits = 0;
        for (auto u = (int) (sampleRate * 0.3163265306); u != 0; u >>= 1)
            ++bits;
        erLength = 1 << (bits + 1);
        er.assign ((std::size_t) erLength, 0.0f);
        lengthPre = std::max (1, (fsi * toInt (3, q[3])) / 1000);
        pre.assign ((std::size_t) lengthPre, 1e-30f);
        controls[0] = controls[1] = controls[2] = -1.0f;
        restart();
    }

    void restart() noexcept
    {
        for (int j = 0; j < 12; ++j)
            std::fill (buf[j].begin(), buf[j].end(), 0.0f), idx[j] = 0;
        std::fill (pre.begin(), pre.end(), 1e-30f);
        std::fill (er.begin(), er.end(), 0.0f);
        ipre = ier = 0;
        s280 = s284 = s288 = s2bc = s2c0 = s2c4 = s2c8 = s2ac = s2b0 = 0.0f;
        level = -1000.0f; // the level is set directly on the first sample
    }

    // Mix (75), Decay (76), Color (77); the coefficients are recomputed only when they change
    void setParameters (const SakuraParams& p) noexcept
    {
        if (p[75] == controls[0] && p[76] == controls[1] && p[77] == controls[2])
            return;
        controls[0] = p[75], controls[1] = p[76], controls[2] = p[77];
        defaults();
        auto x = p[75];
        q[12] = x * 0.999f + 0.001f;
        x = p[76];
        q[8] = x * x * x * 0.6f;
        x = p[77];
        q[2] = x * x * x;
        coefficients();
    }

    void process (double* left, double* right, int n) noexcept
    {
        const auto half = 0.5f, tiny = 1e-30f;
        const auto mask = erLength - 1;
        for (int i = 0; i < n; ++i)
        {
            const auto xl = gin * (float) left[i], xr = gin * (float) right[i];
            auto s = ((((xr + xl) * half) + tiny) - s280) * cHc + s280;
            s280 = s;
            const auto y = ((s - s284) * cLc) + s288;
            s288 = s;
            s284 = y;
            if (cLc != 0.0f)
                s = ((s - y) * half) + tiny;
            // early reflections: only the first tap of each side has a non-zero gain
            float erl = 0.0f, err = 0.0f;
            if (ger != 0.0f)
            {
                erl = er[(std::size_t) ((erLeft + ier) & mask)] * gainErLeft;
                err = er[(std::size_t) ((erRight + ier) & mask)] * gainErRight;
            }
            // input diffusion
            const auto v1 = allPass (0, s, c29c, c29c);
            const auto v2 = allPass (1, v1, c2a0, c29c);
            er[(std::size_t) ier] = v1;
            ier = ier > 0 ? (ier - 1) & mask : erLength - 1;
            const auto v3 = allPass (2, v2, c29c, c29c);
            const auto v4 = allPass (3, v3, c2a0, c29c);
            auto pd = pre[(std::size_t) ipre];
            pre[(std::size_t) ipre] = v4;
            ipre = ipre > 0 ? ipre - 1 : lengthPre - 1;
            pd = pd + 1e-20f;
            // tank, first half (fed by the end of the second half)
            auto x = buf[11][(std::size_t) idx[11]] * g;
            auto a = ((x - s2c0) * c2b4) + s2bc;
            s2bc = x;
            s2c0 = a;
            auto yy = (((((a + x) * c2b8) + x) + pd) - s2ac) * c2a8 + s2ac;
            s2ac = yy;
            auto v = allPass (4, yy, c294, c294);
            auto w = delay (5, v) * g;
            v = allPass (6, w, c298, c298);
            buf[7][(std::size_t) idx[7]] = v;
            idx[7] = idx[7] > 0 ? idx[7] - 1 : len[7] - 1;
            // second half
            x = buf[7][(std::size_t) idx[7]] * g;
            a = ((x - s2c8) * c2b4) + s2c4;
            s2c4 = x;
            s2c8 = a;
            yy = (((((a + x) * c2b8) + x) + pd) - s2b0) * c2a8 + s2b0;
            s2b0 = yy;
            v = allPass (8, yy, c294, c294);
            w = delay (9, v) * g;
            v = allPass (10, w, c298, c298);
            buf[11][(std::size_t) idx[11]] = v;
            idx[11] = idx[11] > 0 ? idx[11] - 1 : len[11] - 1;
            // output taps (read after the index updates)
            const auto t40 = read (9, tap[0]), t55 = read (9, tap[1]), t48 = read (10, tap[2]), t47 = read (11, tap[3]);
            const auto t4 = read (5, tap[4]), t5 = read (6, tap[5]), t6 = read (7, tap[6]), t7 = read (5, tap[8]);
            const auto t8 = read (5, tap[7]), t9 = read (6, tap[9]), t10 = read (7, tap[10]), t11 = read (9, tap[11]);
            const auto t12 = read (10, tap[12]), t13 = read (11, tap[13]);
            auto lv = level, target = levelTarget;
            if (! (lv >= -99.0f))
                lv = levelTarget;
            lv = ((target - lv) * 0.001f) + lv;
            level = lv;
            const auto wl = (lv * (((((((t40 - (k43 * t55)) - (k44 * t48)) - (k56 * t47)) + (k42 * t4)) + t5) + (k45 * t6))))
                            + (ger * erl);
            const auto wr = (lv * ((((((((k43 * t7) + t8) - (k44 * t9)) - (k56 * t10)) - (k42 * t11)) + t12) + (k45 * t13))))
                            + (ger * err);
            const auto mid = wr + wl, side = (wl - wr) * width;
            left[i] = (double) (((xl * dry) + side) + mid);
            right[i] = (double) ((xr * dry) + (mid - side));
        }
    }

private:
    // the reverb's parameter floats as Sakura builds it (defaults; High damping off, Diffusion 100 %)
    void defaults() noexcept
    {
        static constexpr std::uint32_t bits[18] { 0x3f5b83f4, 0x3c99e459, 0x3e25ed09, 0, 0x3efd6a05, 0x3f800000,
                                                  0x3e924925, 0x3e76474b, 0x3d901495, 0x3e25ed09, 0x3f5b83f4, 0x3f463b97,
                                                  0x3f128bdc, 0x3f000000, 0, 0, 0, 0 };
        for (int i = 0; i < 18; ++i)
            q[i] = std::bit_cast<float> (bits[i]);
        q[9] = 1.0f;
        q[5] = 1.0f;
    }

    // float → integer "display" value: floor(x·scale + offset)
    static int toInt (int i, float x) noexcept
    {
        static constexpr float conv[18][2] { { 421, -360.5f }, { 2981, 19.5f }, { 216, 5.5f }, { 250, 0.5f }, { 99, 1.5f },
                                             { 100, 0.5f }, { 28, 2.5f }, { 1975, 25.5f }, { 199, 1.5f }, { 216, 5.5f },
                                             { 421, -360.5f }, { 421, -360.5f }, { 421, -360.5f }, { 200, 0.5f }, { 1, 0.5f },
                                             { 1, 0.5f }, { 1, 0.5f }, { 1, 0.5f } };
        return (int) std::floor ((double) (x * conv[i][0] + conv[i][1]));
    }

    static float db (int v) noexcept { return v <= -361 ? 0.0f : (float) std::pow (10.0, (double) ((float) v * 0.005f)); }

    float onePole (int v) const noexcept
    {
        if ((float) v < 221.0f)
        {
            const auto c = (float) std::cos ((double) (((float) v * 628.31854f) / fs));
            const auto b = 2.0f - c;
            return 1.0f - (b - (float) std::sqrt ((double) ((b * b) - 1.0f)));
        }
        return 1.0f;
    }

    void coefficients() noexcept
    {
        int p[18];
        for (int i = 0; i < 18; ++i)
            p[i] = toInt (i, q[i]);
        const auto fpi = (float) pi;
        gin = p[14] != 0 ? 0.0f : db (p[0]);
        if ((float) p[1] > 19.0f)
        {
            const auto xx = std::min (fs * 0.5f, (float) p[1]);
            const auto t = (float) std::tan ((double) ((xx * fpi) / fs));
            cLc = (t - 1.0f) / (t + 1.0f);
        }
        else
            cLc = 0.0f;
        cHc = onePole (p[2]);
        const auto diff = (float) p[5] * 0.01f, inv = 1.0f / fs;
        const auto* L = len;
        if (diff != 0.0f)
        {
            c29c = (float) std::exp ((double) (((float) (L[1] + L[0]) * (-49.34111f / diff)) * inv));
            c2a0 = (float) std::exp ((double) (((-31.398888f / diff) * (float) (L[2] + L[3])) * inv));
            c294 = (float) std::exp ((double) ((((float) (L[4] + L[8]) * -3.4538777f) / fs) * (2.857143f / diff)));
            c298 = (float) std::exp ((double) (((-3.4538777f / fs) * (float) (L[6] + L[10])) * (1.9607843f / diff)));
        }
        else
            c29c = c2a0 = c294 = c298 = 0.0f;
        const auto tsum = ((float) (L[11] + L[9] + L[5] + L[7]) / fs) * -6.9077554f;
        const auto t60 = (float) p[8] * 0.1f;
        g = (float) std::pow ((double) (float) std::exp ((double) (tsum / t60)), 0.25);
        const auto gb = (float) std::pow ((double) (float) std::exp ((double) ((10.0f / ((float) p[6] * t60)) * tsum)), 0.25);
        const auto r = gb / g;
        c2b8 = (r - 1.0f) * 0.5f;
        const auto rr = std::min (r, 1.0f);
        const auto t = (float) std::tan ((double) (((float) p[7] * fpi) / fs));
        c2b4 = (t - rr) / (rr + t);
        c2a8 = onePole (p[9]);
        dry = p[15] != 0 ? 0.0f : db (p[10]);
        ger = p[16] != 0 ? 0.0f : db (p[11]) * 0.5f;
        levelTarget = p[17] != 0 ? 0.0f : db (p[12]) * 0.3f;
        width = (float) p[13] * 0.01f;
        const auto k = 1.0f - g;
        k42 = k * 0.6f + g;
        k43 = k * 0.25f + g;
        k44 = k * 0.7f + g;
        k56 = k * 0.2f + g;
        k45 = k * 0.5f + g;
    }

    float allPass (int j, float x, float cr, float cw) noexcept
    {
        auto& b = buf[j];
        auto i = idx[j];
        const auto v = b[(std::size_t) i] - (cr * x);
        b[(std::size_t) i] = (cw * v) + x;
        idx[j] = i > 0 ? i - 1 : len[j] - 1;
        return v;
    }

    float delay (int j, float x) noexcept
    {
        auto& b = buf[j];
        const auto i = idx[j];
        const auto o = b[(std::size_t) i];
        b[(std::size_t) i] = x;
        idx[j] = i > 0 ? i - 1 : len[j] - 1;
        return o;
    }

    float read (int j, int offset) const noexcept
    {
        auto s = offset + idx[j];
        if (s >= len[j])
            s -= len[j];
        return buf[j][(std::size_t) s];
    }

    float fs = 48000.0f;
    int fsi = 48000;
    float q[18] {}, controls[3] { -1.0f, -1.0f, -1.0f };
    std::vector<float> buf[12], pre, er;
    int len[12] {}, idx[12] {}, tap[14] {}, lengthPre = 1, ipre = 0, erLength = 1, ier = 0, erLeft = 0, erRight = 0;
    float gainErLeft = 0.0f, gainErRight = 0.0f;
    float gin = 1.0f, cLc = 0.0f, cHc = 1.0f, c29c = 0.0f, c2a0 = 0.0f, c294 = 0.0f, c298 = 0.0f, g = 0.0f;
    float c2b8 = 0.0f, c2b4 = 0.0f, c2a8 = 1.0f, dry = 1.0f, ger = 0.0f, levelTarget = 0.0f, width = 0.0f;
    float k42 = 0.0f, k43 = 0.0f, k44 = 0.0f, k56 = 0.0f, k45 = 0.0f;
    float s280 = 0.0f, s284 = 0.0f, s288 = 0.0f, s2bc = 0.0f, s2c0 = 0.0f, s2c4 = 0.0f, s2c8 = 0.0f, s2ac = 0.0f, s2b0 = 0.0f;
    float level = -1000.0f;
};

struct SakuraSettings
{
    SakuraParams params {};
    double bpm = 0.0;
};

class SakuraInstrument final : public Instrument
{
public:
    static constexpr double fs = 48000.0;
    static constexpr int maxVoices = 12;
    static constexpr int maxLength = 9600; // FL allows 48 000 (notes down to 1 Hz); LYNX keeps ≥ 20 Hz × 4 in memory
    static constexpr int internalCapacity = 8192; // 48 kHz samples rendered per pass

    SakuraInstrument()
    {
        for (auto& v : voices)
        {
            for (auto& l : v.loops)
                l.assign (maxLength + 1, 0.0f);
            for (auto& c : v.combs)
                c.assign (maxLength + 1, 0.0f);
        }
        voiceL.assign (internalCapacity, 0.0);
        voiceR.assign (internalCapacity, 0.0);
        computeStatic();
        prepare (48000.0);
    }

    void prepare (double sr) noexcept override
    {
        hostRate = sr;
        // host samples per pass, so that the 48 kHz samples it needs fit in the voice buffers
        maxHostBlock = std::clamp ((int) ((internalCapacity - 2) / (fs / sr)), 1, 4096);
        hostL.assign ((std::size_t) maxHostBlock, 0.0);
        hostR.assign ((std::size_t) maxHostBlock, 0.0);
        outHp[0].design (32.0, 1.0, sr);
        outHp[1].design (32.0, 1.0, sr);
        // the effects are (re)built whenever the sample rate is set; the LFO phase restarts
        resonators.allocate (sr);
        chorus.allocate (sr);
        reverb.allocate (sr);
        delay.allocate (sr);
        fxOn[0] = fxOn[1] = fxOn[2] = fxOn[3] = false;
        lfoPhase = 0.0;
        quietSamples = 1 << 30;
        position = 1.0;
        previous = current = {};
        allNotesOff();
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        SakuraSettings s;
        for (std::size_t i = 0; i < s.params.size() && i < channel.modelParams.size(); ++i)
            s.params[i] = std::isfinite (channel.modelParams[i]) ? channel.modelParams[i] : 0.0f;
        s.bpm = bpm;
        mailbox.write (s);
        pitchCents.store (channel.sampler.pitch * 100.0f);
    }

    void setNextNoteExpression (float, float cents, bool) noexcept override { nextCents = cents; }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        const auto to = 100.0f * (float) (key - 60) + pitchCents.load() + cents;
        for (auto& v : voices)
            if (v.active && v.release < 0.0)
                v.slide.start (to - (100.0f * (float) (v.key - 60) + v.noteCents), samples * (float) (fs / hostRate));
    }

    void noteOn (int key, float velocity) noexcept override
    {
        readSettings(); // the note's lengths and laws come from the latest settings
        SakuraVoice* target = nullptr;
        for (auto& v : voices)
            if (! v.active)
            {
                target = &v;
                break;
            }
        if (target == nullptr)
            target = &voices[(std::size_t) (nextSteal++ % maxVoices)];
        start (*target, key, velocity);
        target->noteCents = pitchCents.load() + nextCents;
        target->slide = {};
        nextCents = 0.0f;
    }

    // release (and matrix source 8 "release gate") from the next chunk on, i.e. the start of the next host block
    void noteOff (int key) noexcept override
    {
        for (auto& v : voices)
            if (v.active && v.key == key && v.release < 0.0)
            {
                v.release = 0.0;
                v.gate = 1.0;
                v.eg.release (egRelease);
            }
    }

    void allNotesOff() noexcept override
    {
        for (auto& v : voices)
            v.active = false;
    }

    // idle once no voice sounds and the effects' tails have been silent for 3 s (delay lines are up to 2 s)
    bool idle() const noexcept override
    {
        return std::none_of (voices.begin(), voices.end(), [] (const SakuraVoice& v) { return v.active; })
               && quietSamples >= (int) (3.0 * hostRate);
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        readSettings();
        applyEffectSettings();
        while (n > 0)
        {
            const auto block = std::min (n, maxHostBlock);
            renderBlock (left, right, block, gl, gr);
            left += block;
            right += block;
            n -= block;
        }
    }

private:
    double p (int i) const noexcept { return i >= 0 && i < sakuraParamCount ? (double) params[(std::size_t) i] : 0.0; }

    void readSettings() noexcept
    {
        SakuraSettings s;
        if (! mailbox.read (s))
            return;
        params = s.params;
        bpm = s.bpm;
        computeStatic();
    }

    static void damping (double a, double offsetSquared, double& nearC, double& farC) noexcept
    {
        const auto base = std::pow (50.0, 2.57 - 0.8995 * std::pow (a, 3.0)) * (2.0 / 44100.0);
        nearC = std::min (1.0, base);
        farC = std::min (1.0, (1.0 - offsetSquared) * base);
    }

    void start (SakuraVoice& v, int key, float velocity) noexcept
    {
        static constexpr double octaves[7] { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
        auto f = 220.0 * std::pow (2.0, (key - 57) / 12.0) * octaves[std::clamp ((int) (p (113) * 6.0), 0, 6)];
        const auto base = std::max (1, (int) (48000.0 / f));
        int l1 = base, l2 = base;
        switch ((int) p (111))
        {
            case 0: l2 = base / 4; l1 = l2 * 4; break;
            case 1: l2 = base / 3; l1 = l2 * 3; break;
            case 2: l2 = base / 2; l1 = l2 * 2; break;
            case 4: l2 = 2 * base; break;
            case 5: l2 = 3 * base; break;
            case 6: l2 = 4 * base; break;
            default: break;
        }
        l1 = std::clamp (l1, 1, maxLength);
        l2 = std::clamp (l2, 1, maxLength);
        v.lens[0] = v.lens[1] = l1;
        v.lens[2] = v.lens[3] = l2;
        v.freq = f;
        v.rate = 48000.0 / l1;
        v.kdc[0] = v.kdc[1] = 1.0 / l1;
        v.kdc[2] = v.kdc[3] = 1.0 / l2;
        const auto ratio = p (19);
        v.combLen[0] = std::clamp ((int) (l1 * ratio), 1, maxLength);
        v.combLen[1] = std::clamp ((int) (l2 * ratio), 1, maxLength);
        for (int j = 0; j < 4; ++j)
        {
            std::fill (v.loops[(std::size_t) j].begin(), v.loops[(std::size_t) j].begin() + v.lens[j], 0.0f);
            v.ptr[j] = 0;
            v.mean[j] = v.slow[j] = v.z[j] = 0.0;
        }
        for (int c = 0; c < 2; ++c)
        {
            std::fill (v.combs[(std::size_t) c].begin(), v.combs[(std::size_t) c].begin() + v.combLen[c], 0.0f);
            v.combPtr[c] = 0;
        }
        v.velocity = std::clamp ((double) velocity, 0.0, 1.0);
        v.eg.begin (egAttack, egDecay, egSustain);
        v.egValue = v.lfoValue = v.gate = 0.0;
        v.seed = 0;
        v.hold = 0.0;
        v.s[0] = v.s[1] = v.s[2] = 0.0;
        v.z4 = v.hiCutEnv = 0.0;
        v.acc = 0.0;
        v.prev[0] = v.prev[1] = v.cur[0] = v.cur[1] = 0.0;
        v.env = v.elapsed = 0.0;
        v.release = -1.0;
        v.lowCut.design (std::pow (2.0, 9.3 * clamp01 (p (24)) + 5.0), loCutQ, fs); // state cleared; retuned per chunk
        v.key = key;
        v.active = true;
    }

    // values that depend only on the parameters
    void computeStatic() noexcept
    {
        auto sign = [this] (int i) { return p (i) != 0.0 ? -1.0 : 1.0; };
        saturation = 100.0 * std::pow (std::pow (p (16), 2.0), 3.0);
        drive = 1000.0 * std::pow (p (33), 4.0);
        noiseA = std::pow (1.0 - p (31), 2.0);
        noiseB = p (31);
        loCutQ = 10.0 * p (25) + 1.0;
        sharp1 = -0.5 * std::pow (p (10), 2.0);
        sharp2 = -0.5 * std::pow (p (15), 2.0);
        combFeedback = std::sqrt (std::max (0.0, p (20))) * sign (21);
        combLevel = p (22) * sign (23);
        tune = std::clamp ((int) (p (1) * 198.0 - 99.0), -99, 99) * 0.01;
        attack = f32 (35.0 * std::pow (p (2), 5.0) + 0.001);
        decay = f32 (50.0 * std::pow (p (3), 5.0) + 0.001);
        sustain = f32 (std::pow (p (4), 2.0));
        releaseTime = f32 (10.0 * std::pow (p (5), 5.0) + 0.1);
        egAttack = f32 (10.0 * std::pow (p (34), 3.0) + 0.001);
        egDecay = f32 (50.0 * std::pow (p (35), 5.0) + 0.001);
        egSustain = f32 (std::pow (p (36), 2.0));
        egRelease = f32 (50.0 * std::pow (p (37), 5.0) + 0.1);
        lfoRate = f32 (10.0 * std::pow (p (38), 3.0) + 0.01);
        // matrix slots (REA_rodada29 §1.3): source, amount² (negated by "invert"), destination
        for (int k = 0; k < 8; ++k)
        {
            slotSource[k] = (int) p (79 + 4 * k);
            slotAmount[k] = std::pow (p (80 + 4 * k), 2.0) * sign (81 + 4 * k);
            slotDestination[k] = (int) p (82 + 4 * k);
        }
    }

    void applyEffectSettings() noexcept
    {
        const bool on[4] { std::any_of (params.begin() + 55, params.begin() + 63, [] (float x) { return x != 0.0f; }),
                           params[70] != 0.0f, params[74] != 0.0f, params[63] != 0.0f };
        // an effect switched on starts from its freshly built state
        if (on[0] && ! fxOn[0])
            resonators.restart (hostRate);
        if (on[1] && ! fxOn[1])
            chorus.restart();
        if (on[2] && ! fxOn[2])
            reverb.restart();
        if (on[3] && ! fxOn[3])
            delay.restart();
        for (int i = 0; i < 4; ++i)
            fxOn[i] = on[i];
        if (on[0])
            resonators.setParameters (params);
        if (on[1])
            chorus.setParameters (params, hostRate);
        if (on[2])
            reverb.setParameters (params);
        if (on[3])
            delay.setParameters (params, hostRate, bpm);
    }

    // one host block: LFO step, the 48 kHz samples it needs (chunks of ≤ 48 per voice), resampling, HP, effects
    void renderBlock (float* left, float* right, int n, float gl, float gr) noexcept
    {
        // global LFO (§1.2): value at the start of the host block, then the phase advances by n host samples
        lfoValue = f32 (std::cos (2.0 * pi * lfoPhase));
        lfoPhase = f32 (lfoPhase + f32 (n * lfoRate) * (1.0 / hostRate));
        if (lfoPhase > 1.0)
            lfoPhase = f32 (lfoPhase - 1.0);

        const auto step = fs / hostRate;
        int needed = 0;
        {
            auto pos = position;
            for (int i = 0; i < n; ++i)
            {
                while (pos >= 1.0)
                    ++needed, pos -= 1.0;
                pos += step;
            }
        }
        std::fill (voiceL.begin(), voiceL.begin() + needed, 0.0);
        std::fill (voiceR.begin(), voiceR.begin() + needed, 0.0);
        bool sounding = false;
        for (auto& v : voices)
            if (v.active)
                renderVoice (v, needed), sounding = true;

        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            while (position >= 1.0)
            {
                previous = current;
                current = { (float) voiceL[(std::size_t) k], (float) voiceR[(std::size_t) k] };
                ++k;
                position -= 1.0;
            }
            const auto f = (float) position;
            hostL[(std::size_t) i] = outHp[0].tick (previous[0] + (current[0] - previous[0]) * f);
            hostR[(std::size_t) i] = outHp[1].tick (previous[1] + (current[1] - previous[1]) * f);
            position += step;
        }

        if (fxOn[0])
            resonators.process (hostL.data(), hostR.data(), n);
        if (fxOn[1])
            chorus.process (hostL.data(), hostR.data(), n);
        if (fxOn[2])
            reverb.process (hostL.data(), hostR.data(), n);
        if (fxOn[3])
            delay.process (hostL.data(), hostR.data(), n);

        // × 1/√2: FL's per-voice levels are Vol·cos/sin((1 + pan)π/4) and LYNX's channel gains √2·cos/sin (§4)
        const auto master = std::pow (10.0, 4.8 * p (0) - 1.8) * 0.70710678;
        double peak = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto l = hostL[(std::size_t) i] * master, r = hostR[(std::size_t) i] * master;
            peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
            left[i] += (float) l * gl;
            right[i] += (float) r * gr;
        }
        quietSamples = sounding || peak > 1e-6 ? 0 : std::min (quietSamples + n, 1 << 30);
    }

    void renderVoice (SakuraVoice& v, int count) noexcept
    {
        for (int done = 0; done < count;)
        {
            const auto n = std::min (48, count - done);
            if (v.slide.moving())
                v.slide.advance ((float) n);
            if (! chunkLaws (v, n))
            {
                v.active = false;
                return;
            }
            for (int i = 0; i < n; ++i)
            {
                const auto [l, r] = tickVoice (v);
                voiceL[(std::size_t) (done + i)] += l;
                voiceR[(std::size_t) (done + i)] += r;
            }
            done += n;
        }
    }

    // laws re-evaluated per internal chunk of n samples (REA_rodada29 §1.3), in the engine's order; false when the
    // release has run out (the voice ends)
    bool chunkLaws (SakuraVoice& v, int n) noexcept
    {
        // matrix sources: 1 EG, 2 LFO × depth, 3 mod wheel, 4 pitch bend, 5 pressure (no MIDI CC routing: 0),
        // 6 velocity, 7 note × 0.0125, 8 release gate
        double src[9] { 0.0, v.egValue, v.lfoValue, 0.0, 0.0, 0.0, v.velocity, v.key * 0.0125, v.gate };
        auto mod = [&] (int d) noexcept
        {
            double total = 0.0;
            bool routed = false;
            for (int k = 0; k < 8; ++k)
            {
                if (slotDestination[k] != d || slotSource[k] == 0)
                    continue;
                routed = true;
                total += slotAmount[k] * (slotSource[k] >= 1 && slotSource[k] <= 8 ? src[slotSource[k]] : 0.0);
            }
            return (d == 0 || d == 1) && ! routed ? 1.0 : total;
        };
        // pitch (destination 3) is evaluated before the EG/LFO update: it uses the previous chunk's values
        const auto m3 = mod (3);
        v.step = v.freq / v.rate * std::pow (2.0, tune / 12.0 + m3 / 6.0);
        if (const auto shift = v.noteCents + v.slide.cents; shift != 0.0f) // FL's Final.Pitch (issue #14)
            v.step *= std::exp2 ((double) shift / 1200.0);
        v.egValue = src[1] = v.eg.tick (n);
        v.lfoValue = src[2] = mod (0) * lfoValue;
        v.threshold = 0.996 * std::pow (1.0 - clamp01 (p (32) + mod (14)), 0.25);
        const auto hcAttack = f32 (std::pow (clamp01 (mod (6) + p (26)), 2.0) * 0.5);
        const auto hcDecay = clamp01 (mod (7) + p (27));
        const auto hcSustain = std::pow (p (28), 2.0);
        v.ladderG = clamp01 (std::pow (mod (5) * 0.5 + p (29), 2.0));
        v.ladderR = std::max (-10.0 * p (30) * (1.0 - v.ladderG), -4.0);
        v.lowCut.retune (std::pow (2.0, 9.3 * clamp01 (mod (4) * 0.5 + p (24)) + 5.0), loCutQ, fs); // keeps its state
        const auto a1 = clamp01 (mod (11) + mod (13) + p (9)), a2 = clamp01 (mod (12) + mod (13) + p (14));
        damping (a1, std::pow (p (8), 2.0), v.coef[0], v.coef[1]);
        damping (a2, std::pow (p (13), 2.0), v.coef[2], v.coef[3]);
        v.sharp[0] = (1.0 - v.coef[0]) * sharp1, v.sharp[1] = (1.0 - v.coef[1]) * sharp1;
        v.sharp[2] = (1.0 - v.coef[2]) * sharp2, v.sharp[3] = (1.0 - v.coef[3]) * sharp2;
        const auto fb1 = std::pow (clamp01 (mod (10) + mod (8) + p (6)), 0.05) * (p (7) != 0.0 ? -1.0 : 1.0);
        const auto fb2 = std::pow (clamp01 (mod (10) + mod (9) + p (11)), 0.05) * (p (12) != 0.0 ? -1.0 : 1.0);
        v.fbTarget[0] = v.fbTarget[1] = fb1;
        v.fbTarget[2] = v.fbTarget[3] = fb2;
        const auto sep = clamp01 (mod (2) + p (18)), sep2 = clamp01 (mod (2) + 1.0 - p (18));
        const auto level = std::pow (10.0, 2.0 * mod (1)) * 0.05;
        const auto g1 = (1.0 - p (17)) * level, g2 = p (17) * level;
        v.gainTarget[0] = (1.0 - sep) * g1, v.gainTarget[1] = sep * g1;
        v.gainTarget[2] = (1.0 - sep2) * g2, v.gainTarget[3] = sep2 * g2;
        if (v.release < 0.0)
        {
            if (hcAttack < v.elapsed)
                v.hcRate = 0.0002 / (3.0 * std::pow (hcDecay, 3.0) + 0.001), v.hcTarget = hcSustain;
            else
                v.hcTarget = 1.0, v.hcRate = hcAttack >= 0.001 ? 0.0002 / hcAttack : 1.0;
            if (attack <= v.elapsed)
                v.envTarget = sustain, v.envRate = 8.82 / (decay * 48000.0);
            else
                v.envTarget = 5.0, v.envRate = 4.658742368e-06 / attack;
        }
        else
        {
            v.hcRate = 0.002;
            v.hcTarget = 0.0;
            if (releaseTime < v.release)
                return false;
            v.envTarget = 0.0;
            v.envRate = 8.82 / (releaseTime * 48000.0);
            v.release += n * 2.0833333333333333e-05;
        }
        for (int j = 0; j < 4; ++j) // the smoothed values restart from their targets at each chunk
            v.fbs[j] = v.fbTarget[j], v.gains[j] = v.gainTarget[j];
        v.elapsed += n * 2.0833333333333333e-05;
        return true;
    }

    std::array<double, 2> tickVoice (SakuraVoice& v) noexcept
    {
        // exciter: sample-and-hold noise / click → Lo-Cut → 4-pole Hi-Cut with envelope, resonance and drive
        const auto x = v.seed * (1.0 / 4294967296.0);
        v.seed = lcg (lcg (lcg (lcg (v.seed))));
        if (x > v.threshold)
            v.hold = x;
        const auto y = v.lowCut.tick (noiseA * v.hold + noiseB);
        v.hiCutEnv += (v.hcTarget - v.hiCutEnv) * v.hcRate;
        const auto c = v.ladderG * v.hiCutEnv;
        v.s[0] += ((v.ladderR * v.z4 + y) - v.s[0]) * c;
        v.s[1] += (v.s[0] - v.s[1]) * c;
        v.s[2] += (v.s[1] - v.s[2]) * c;
        const auto w = v.z4 + (v.s[2] - v.z4) * c;
        v.z4 = w / (w * w * drive + 1.0);
        const auto e = v.z4;

        // the loops run once per crossing of the fractional clock
        v.acc += v.step;
        auto k = (int) v.acc;
        v.acc -= k;
        double d[4];
        for (int j = 0; j < 4; ++j)
            d[j] = e - v.mean[j];
        for (; k > 0; --k)
        {
            v.prev[0] = v.cur[0];
            v.prev[1] = v.cur[1];
            double rr[4];
            for (int j = 0; j < 4; ++j)
                if (v.ptr[j] >= v.lens[j])
                    v.ptr[j] = 0;
            for (int j = 0; j < 4; ++j)
            {
                v.mean[j] += v.kdc[j] * d[j];
                d[j] = e - v.mean[j];
                auto& line = v.loops[(std::size_t) j];
                const auto r = (double) line[(std::size_t) v.ptr[j]];
                rr[j] = r;
                v.slow[j] += ((v.sharp[j] * v.z[j] + r) - v.slow[j]) * v.coef[j];
                const auto vv = v.z[j] + (v.slow[j] - v.z[j]) * v.coef[j];
                v.z[j] = vv / (vv * vv * saturation + 1.0);
                line[(std::size_t) v.ptr[j]] = (float) (v.fbs[j] * v.z[j] + d[j]);
                ++v.ptr[j];
            }
            for (int cIdx = 0; cIdx < 2; ++cIdx)
                if (v.combPtr[cIdx] >= v.combLen[cIdx])
                    v.combPtr[cIdx] = 0;
            auto& ca = v.combs[0];
            auto& cb = v.combs[1];
            const auto oA = combLevel * ca[(std::size_t) v.combPtr[0]] + rr[0];
            const auto oB = combLevel * cb[(std::size_t) v.combPtr[1]] + rr[2];
            v.cur[0] = v.gains[2] * oB + v.gains[0] * oA;
            v.cur[1] = v.gains[3] * oB + v.gains[1] * oA;
            ca[(std::size_t) v.combPtr[0]] = (float) (ca[(std::size_t) v.combPtr[0]] * combFeedback + rr[1]);
            cb[(std::size_t) v.combPtr[1]] = (float) (cb[(std::size_t) v.combPtr[1]] * combFeedback + rr[3]);
            ++v.combPtr[0];
            ++v.combPtr[1];
            for (int j = 0; j < 4; ++j)
            {
                v.fbs[j] += (v.fbTarget[j] - v.fbs[j]) * 0.05;
                v.gains[j] += (v.gainTarget[j] - v.gains[j]) * 0.001;
            }
        }
        const auto outL = ((v.cur[0] - v.prev[0]) * v.acc + v.prev[0]) * v.env;
        const auto outR = ((v.cur[1] - v.prev[1]) * v.acc + v.prev[1]) * v.env;
        v.env = std::min (1.0, (v.envTarget - v.env) * v.envRate + v.env);
        return { outL, outR };
    }

    Mailbox<SakuraSettings> mailbox;
    SakuraParams params {};
    double bpm = 0.0;
    std::array<SakuraVoice, maxVoices> voices {};
    int nextSteal = 0;
    std::atomic<float> pitchCents { 0.0f }; // the channel's PITCH knob (configure)
    float nextCents = 0.0f;                 // the next note's fine pitch + main pitch (setNextNoteExpression)
    double hostRate = 48000.0, position = 1.0;
    int maxHostBlock = 4096, quietSamples = 0;
    std::array<float, 2> previous {}, current {};
    std::vector<double> voiceL, voiceR, hostL, hostR;
    HighPass outHp[2];
    double lfoPhase = 0.0, lfoValue = 1.0;
    SakuraResonators resonators;
    SakuraChorus chorus;
    SakuraReverb reverb;
    SakuraDelay delay;
    bool fxOn[4] {};
    // values that depend only on the parameters
    double saturation = 0.0, drive = 0.0, noiseA = 1.0, noiseB = 0.0, loCutQ = 1.0, sharp1 = 0.0, sharp2 = 0.0;
    double combFeedback = 0.0, combLevel = 0.0, tune = 0.0;
    double attack = 0.001, decay = 0.001, sustain = 1.0, releaseTime = 0.1;
    double egAttack = 0.001, egDecay = 0.001, egSustain = 0.0, egRelease = 0.1, lfoRate = 0.01;
    int slotSource[8] {}, slotDestination[8] {};
    double slotAmount[8] {};
};

// ---------------------------------------------------------------------------
// Drumpad (REA_rodada28 §2): mallet exciter → bank of independent comb loops (length 512 − i, low-pass x²,
// feedback −Decay^0.3, folded-triangle output weights) → self-oscillating filter → LOW high-pass → HIGH shelf →
// MID peak (enveloped) → clip → hold → lo-fi, at 38 kHz. No key tracking (the DSP has none).

using DrumpadParams = std::array<float, drumpadParamCount>;

struct Rbj
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void design (int kind, double f, double gainDb, double bandwidth, double fs) noexcept
    {
        const auto a = std::pow (10.0, gainDb * 0.05);
        const auto w = std::clamp (f * 6.283185 / fs, 0.0, 3.0);
        const auto sn = std::sin (w), cs = std::cos (w);
        const auto sh = std::sinh (0.3465736 * bandwidth * w / sn);
        const auto alpha = sn * sh, beta = std::sqrt (a) * 2.0 * sh * sn;
        double nb0, nb1, nb2, na0, na1, na2;
        if (kind == 1) // peaking
            nb0 = 1 + alpha * a, nb1 = -2 * cs, nb2 = 1 - alpha * a, na0 = 1 + alpha / a, na1 = -2 * cs, na2 = 1 - alpha / a;
        else // high shelf
        {
            nb0 = a * ((a + 1) + (a - 1) * cs + beta);
            nb1 = -2 * a * ((a - 1) + (a + 1) * cs);
            nb2 = a * ((a + 1) + (a - 1) * cs - beta);
            na0 = (a + 1) - (a - 1) * cs + beta;
            na1 = 2 * ((a - 1) - (a + 1) * cs);
            na2 = (a + 1) - (a - 1) * cs - beta;
        }
        b0 = nb0 / na0, b1 = nb1 / na0, b2 = nb2 / na0, a1 = na1 / na0, a2 = na2 / na0;
    }

    double tick (double x) noexcept
    {
        const auto y = x * b0 + x1 * b1 + x2 * b2 - y1 * a1 - y2 * a2;
        x2 = x1, x1 = x, y2 = y1, y1 = y;
        return y;
    }
};

class DrumpadInstrument final : public Instrument
{
public:
    static constexpr double fs = 38000.0;
    static constexpr int rings = 128; // groups of 4 lanes: lane i of group k has length 512 − 4k

    DrumpadInstrument()
    {
        for (int k = 0; k < rings; ++k)
            ring[(std::size_t) k].assign ((std::size_t) (512 - 4 * k), {});
    }

    void prepare (double sr) noexcept override
    {
        hostRate = sr;
        allNotesOff();
    }

    void configure (const ChannelDef& channel, double) noexcept override
    {
        DrumpadParams p {};
        for (std::size_t i = 0; i < p.size() && i < channel.modelParams.size(); ++i)
            p[i] = std::isfinite (channel.modelParams[i]) ? channel.modelParams[i] : 0.0f;
        mailbox.write (p);
        pitchCents.store (channel.sampler.pitch * 100.0f);
    }

    void setNextNoteExpression (float, float cents, bool) noexcept override { nextCents = cents; }

    // a slide note moves Final.Pitch: Drumpad follows it like the channel pitch (round 28 §4, issue #14)
    void slideTo (int key, float cents, float samples) noexcept override
    {
        if (sounding && noteKey >= 0)
            slide.start (100.0f * (float) (key - noteKey) + pitchCents.load() + cents - noteCents, samples);
    }

    // one voice (Drumpad retriggers it; the loops keep their contents across notes)
    void noteOn (int key, float velocity) noexcept override
    {
        // the channel's pitch / fine / main pitch shift it (not the key: round 28 §4, ±12 semitones)
        noteKey = key;
        noteCents = pitchCents.load() + nextCents;
        nextCents = 0.0f;
        slide = {};
        mailbox.read (raw);
        computeParams (std::sqrt (std::clamp ((double) velocity, 0.0, 1.0)));
        e = amp;
        if (retrig)
            seed = noiseSeed;
        lowCut.design (lowF, lowQ, fs);
        high.design (2, highF, highDb, std::max (highBw, 1e-4), fs);
        high.x1 = high.x2 = high.y1 = high.y2 = 0.0;
        mid.x1 = mid.x2 = mid.y1 = mid.y2 = 0.0;
        envMid = 1.0;
        midCount = 0;
        midRetrig = midRetrigCount;
        const auto c = std::pow (2.0, (sofF * 0.5 + 0.1) * 9.3 + 5.0) * 0.00015789474;
        sc = (float) std::clamp (c, 0.0, 1.0);
        sk = sc * 333.3333333333333f;
        senv = 1.0f;
        s1 = s2 = s3 = y4 = 0.0f;
        gain = 1.0;
        t = 0.0;
        sounding = true;
        silentFor = 0;
    }

    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override { sounding = false; }
    bool idle() const noexcept override { return ! sounding; }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        if (! sounding)
            return;
        struct Source
        {
            DrumpadInstrument& self;
            std::array<float, 2> tick() noexcept
            {
                const auto x = (float) self.tick();
                return { x, x };
            }
        } source { *this };
        // × 1/√2: FL's per-voice levels are Vol·cos/sin((1 + pan)π/4), LYNX's channel gains √2·cos/sin (REA_rodada28 §4)
        auto rate = fs * pitch;
        if (slide.moving())
            slide.advance ((float) n);
        if (const auto shift = noteCents + slide.cents; shift != 0.0f) // FL's Final.Pitch: 2^(clamp(T, ±12)/12)
            rate *= std::exp2 (std::clamp ((double) shift / 100.0, -12.0, 12.0) / 12.0);
        renderResampled (source, rate, hostRate, position, left, right, n, gl * 0.70710678f, gr * 0.70710678f, previous, current);
        // the voice ends when the hold gain has decayed and the bank is quiet
        if (gain < 1e-5 && std::abs (current[0]) < 1e-6f)
            sounding = false;
    }

private:
    std::atomic<float> pitchCents { 0.0f }; // the channel's PITCH knob (configure)
    float nextCents = 0.0f, noteCents = 0.0f; // the next / current note's pitch beyond the key (cents)
    int noteKey = -1;
    lynx::HostSlide slide;
    double g (int k, double velocity) const noexcept
    {
        const auto value = (double) raw[(std::size_t) k];
        auto alt = value;
        for (int slot = 0; slot < 4; ++slot) // velocity morph: params 30…33 = target index − 1, 34…37 = value at velocity 1
            if (const auto target = (int) raw[(std::size_t) (30 + slot)]; target != 0 && target + 1 == k)
                alt = raw[(std::size_t) (34 + slot)];
        return (1.0 - velocity) * value + alt * velocity;
    }

    void computeParams (double vel) noexcept
    {
        cut = std::pow (clamp01 (g (7, vel)), 2.0);
        fb = -std::pow (clamp01 (g (6, vel)), 0.3);
        const auto phase = clamp01 (g (9, vel)), tension = clamp01 (g (8, vel)), material = clamp01 (g (10, vel));
        size = std::min (0x1ff, (int) (clamp01 (1.0 - g (11, vel)) * 512.0));
        shape = std::clamp ((int) (g (12, vel) * 128.0), 1, 128);
        amp = std::pow (10.0, (1.0 - clamp01 (g (2, vel))) * -4.0);
        const auto md = 1.0 - clamp01 (g (3, vel));
        malletDecay = (std::pow (md, 4.0) * 500.0 + 1.0) * 1e-4;
        noise = clamp01 (g (4, vel));
        retrig = raw[0] != 0.0f;
        noiseSeed = (std::uint32_t) ((std::uint64_t) ((double) (float) g (5, vel) * 4294967295.0) & 0xffffffffu);
        const auto lf = clamp01 (g (27, vel));
        lofi = lf >= 0.01 ? (int) (std::pow (1.0 - lf, 5.0) * 65536.0 + 4.0) : 0;
        holdTime = std::pow (clamp01 (g (28, vel)), 3.0) * 3.0 + 0.001;
        pitch = std::pow (2.0, 2.0 * clamp01 (g (29, vel)) - 1.0);
        lowOn = g (13, vel) != 0.0;
        lowF = std::pow (2.0, clamp01 (g (13, vel)) * 11.0 + 2.0);
        lowQ = clamp01 (g (14, vel)) * 8.0 + 1.0;
        highF = std::pow (2.0, clamp01 (g (24, vel)) * 4.1 + 8.0);
        highBw = (1.0 - clamp01 (g (26, vel))) * 2.0 + 0.5;
        highDb = (clamp01 (g (25, vel)) - 0.5) * 36.0;
        highOn = std::abs (highDb) > 0.01;
        midF = std::pow (2.0, clamp01 (g (18, vel)) * 9.0 + 5.0);
        midFmod = (clamp01 (g (21, vel)) - 0.5) * 32.0;
        const auto lv = g (19, vel), lm = g (22, vel);
        midLevelOn = lv != 0.5;
        midDb = (clamp01 (lv) - 0.5) * 32.0;
        midLmodOn = lm != 0.5;
        midLmod = (clamp01 (lm) - 0.5) * 48.0;
        midBw = (1.0 - clamp01 (g (20, vel))) * 4.0 + 0.5;
        const auto ma = 1.0 - clamp01 (g (23, vel));
        midAttack = (std::pow (ma, 4.0) * 1000.0 + 1.0) * 1e-5;
        midRetrigCount = std::clamp ((int) (raw[1] * 3.0f), 0, 3);
        sofF = clamp01 (g (15, vel));
        const auto sd = clamp01 (g (16, vel));
        sofDecay = (float) ((std::pow (sd, 4.0) * 0.25 * 250.0 + 1.0) * 1e-5);
        sofOn = g (17, vel) != 0.0;
        sofLevel = (float) (std::pow (clamp01 (g (17, vel)), 8.0) * 0.5);

        // output weights: folded triangle raised to the Material exponent, over the lanes Size…Size + Shape
        const auto m = 2.0 * material - 1.0;
        auto ex = 1.0 + 20.0 * std::pow (m, 4.0);
        if (m < 0.0)
            ex = 1.0 / ex;
        const auto stp = 0.5 * std::pow (tension, 4.0);
        auto ph = phase;
        weights.fill (0.0);
        const auto top = std::min (size + shape, 0x1ff);
        for (int i = 511; i >= 0; --i)
        {
            if (i >= size && i <= top)
            {
                auto x = (ph - 0.5) + ph - 0.5;
                if (x > 0.5)
                    x = 1.0 - x;
                if (x < -0.5)
                    x = -1.0 - x;
                const auto x2 = x + x;
                weights[(std::size_t) i] = x2 < 0.0 ? -std::pow (std::abs (x2), ex) : std::pow (x2, ex);
            }
            ph += stp;
            if (ph > 1.0)
                ph -= 1.0;
        }
        firstRing = std::clamp (size / 4, 0, 127);
        const auto end = size + shape >= 0x200 ? 128 : std::min (128, (size + shape) / 4 + ((size + shape) % 4 != 0 ? 1 : 0));
        ringCount = std::max (0, end - firstRing);
    }

    double tick() noexcept
    {
        e = (1e-7 - e) * malletDecay + e;
        const auto nz = seed * (1.0 / 4294967296.0);
        seed = lcg (seed);
        const auto exc = ((nz - 0.5) * noise + (1.0 - noise)) * e;
        double acc = 0.0;
        for (int k = firstRing; k < firstRing + ringCount; ++k)
        {
            auto& r = ring[(std::size_t) k];
            const auto len = (int) r.size();
            auto& p = ptr[(std::size_t) k];
            auto& lp = lowPass[(std::size_t) k];
            // lanes 1…3 are shorter by 1…3 samples: their last cells mirror the start of the ring
            if (p == len - 1)
                r[(std::size_t) p][1] = r[0][1];
            if (p == len - 2)
                r[(std::size_t) p][2] = r[0][2], r[(std::size_t) p + 1][2] = r[1][2];
            if (p == len - 3)
                r[(std::size_t) p][3] = r[0][3], r[(std::size_t) p + 1][3] = r[1][3], r[(std::size_t) p + 2][3] = r[2][3];
            auto& cell = r[(std::size_t) p];
            for (int j = 0; j < 4; ++j)
            {
                acc += cell[(std::size_t) j] * weights[(std::size_t) (4 * k + j)];
                lp[(std::size_t) j] = (cell[(std::size_t) j] - lp[(std::size_t) j]) * cut + lp[(std::size_t) j];
                cell[(std::size_t) j] = lp[(std::size_t) j] * fb + exc;
            }
            if (++p == len)
            {
                for (int q = 0; q < len - 1; ++q)
                    r[(std::size_t) q][1] = r[(std::size_t) q + 1][1];
                for (int q = 0; q < len - 2; ++q)
                    r[(std::size_t) q][2] = r[(std::size_t) q + 2][2];
                for (int q = 0; q < len - 3; ++q)
                    r[(std::size_t) q][3] = r[(std::size_t) q + 3][3];
                p = 0;
            }
        }
        auto x = acc;
        if (sofOn) // self-oscillating filter, in float32 as in the DLL (its −100000 feedback needs it)
        {
            const auto y4o = y4;
            const auto a = (float) ((float) x + y4o * -100000.0f);
            x = (float) ((float) x + y4o * sofLevel);
            const auto gg = sc / ((sk + 1.0f) - senv * sk);
            const auto tt = a / (std::abs (a) * 0.005f + 1.0f);
            s1 = (tt - s1) * gg + s1;
            s2 = (s1 - s2) * gg + s2;
            s3 = (s2 - s3) * gg + s3;
            senv = senv - senv * sofDecay;
            y4 = (s3 - y4o) * gg + y4o;
        }
        if (lowOn)
            x = lowCut.tick (x);
        if (highOn)
            x = high.tick (x);
        if (midLevelOn || midLmodOn)
        {
            if (midCount == 0)
            {
                const auto db = (1.0 - envMid) * midLmod + midDb;
                const auto fm = midFmod * (1.0 - envMid);
                auto f = midFmod >= 0.0 ? (fm + 1.0) * midF : midF / (1.0 - fm);
                f = std::clamp (std::max (32.0, f), 0.0, 12000.0);
                const auto keep = mid;
                mid.design (1, f, db, std::max (midBw, 1e-4), fs);
                mid.x1 = keep.x1, mid.x2 = keep.x2, mid.y1 = keep.y1, mid.y2 = keep.y2;
            }
            midCount = (midCount + 1) % 6;
            envMid -= envMid * midAttack;
            if (midRetrig != 0 && envMid < 0.01)
                envMid = 1.0, --midRetrig;
            x = mid.tick (x);
        }
        x = std::clamp (x, -1.0, 1.0);
        if (holdTime <= t)
            gain -= gain * 0.001;
        else
            gain = (1.0 - gain) * 0.1 + gain;
        t += 2.6315789e-05;
        x *= gain;
        if (lofi != 0)
            x = (double) (int) (lofi * x) / lofi;
        return x;
    }

    Mailbox<DrumpadParams> mailbox;
    DrumpadParams raw {};
    std::array<std::vector<std::array<double, 4>>, rings> ring;
    std::array<int, rings> ptr {};
    std::array<std::array<double, 4>, rings> lowPass {};
    std::array<double, 512> weights {};
    int firstRing = 0, ringCount = 0;
    double cut = 0, fb = 0, amp = 1e-4, malletDecay = 1e-4, noise = 0, holdTime = 1, pitch = 1;
    int size = 0, shape = 1, lofi = 0, midRetrigCount = 0, midRetrig = 0, midCount = 0;
    bool retrig = false, lowOn = false, highOn = false, midLevelOn = false, midLmodOn = false, sofOn = false, sounding = false;
    std::uint32_t seed = 0, noiseSeed = 0;
    double lowF = 100, lowQ = 1, highF = 1000, highBw = 1, highDb = 0, midF = 1000, midFmod = 0, midDb = 0, midLmod = 0,
           midBw = 1, midAttack = 1e-5;
    double sofF = 0;
    float sofDecay = 0, sofLevel = 0, sc = 0, sk = 0, senv = 1, s1 = 0, s2 = 0, s3 = 0, y4 = 0;
    double e = 0, envMid = 1, gain = 1, t = 0;
    HighPass lowCut;
    Rbj high, mid;
    double hostRate = 48000.0, position = 1.0;
    std::array<float, 2> previous {}, current {};
    int silentFor = 0;
};

// ---------------------------------------------------------------------------
// BooBass (REA_rodada29 §4): a 6-zone 16-bit multisample compiled into the user's own BooBass_Eng_x64.dll (read at run
// time, never shipped), attack (16 kHz) crossfaded into a single-cycle loop (16 384-point table), dB-linear envelope,
// 3-band EQ twice with sin saturation, slew limiter, 4-section low-pass. Monophonic, velocity has no effect.

struct BooBassZone
{
    std::vector<float> attack; // with 4 guard samples on each side
    std::vector<float> loop;   // 16 384 + 8 (4 guards each side)
    int root = 0;
    double attackLength = 0.0;
};
struct BooBassTables
{
    std::array<BooBassZone, 6> zones;
};

constexpr const char* booBassSha256 = "4e10efcd9f945860911e7cdff9c4065c4f193b58ddffc8304b45add1db51705f";

std::unique_ptr<BooBassTables> loadBooBassTables (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in) // FL-style path on a system with '/' separators (the command-line tools on Linux)
    {
        auto local = path;
        std::replace (local.begin(), local.end(), '\\', '/');
        in.open (local, std::ios::binary);
    }
    if (! in)
        return nullptr;
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (b.size() < 0x1000 || sha256Hex (b.data(), b.size()) != booBassSha256)
        return nullptr; // only the decoded build
    auto rd = [&] (std::size_t off, auto& v) { if (off + sizeof (v) <= b.size()) std::memcpy (&v, b.data() + off, sizeof (v)); };
    std::uint32_t pe = 0;
    rd (0x3c, pe);
    std::uint16_t sections = 0, optSize = 0;
    rd (pe + 6, sections);
    rd (pe + 20, optSize);
    std::uint64_t base = 0;
    rd (pe + 48, base);
    struct Section { std::uint32_t va, vs, ro, rs; };
    std::vector<Section> secs;
    for (std::uint32_t i = 0, o = pe + 24 + optSize; i < sections; ++i, o += 40)
    {
        Section s {};
        rd (o + 8, s.vs);
        rd (o + 12, s.va);
        rd (o + 16, s.rs);
        rd (o + 20, s.ro);
        secs.push_back (s);
    }
    auto offset = [&] (std::uint64_t va) -> std::size_t {
        const auto r = va - base;
        for (const auto& s : secs)
            if (r >= s.va && r < (std::uint64_t) s.va + std::max (s.vs, s.rs))
                return (std::size_t) (s.ro + r - s.va);
        return b.size();
    };
    auto u64 = [&] (std::uint64_t va) { std::uint64_t v = 0; rd (offset (va), v); return v; };
    auto i32 = [&] (std::uint64_t va) { std::int32_t v = 0; rd (offset (va), v); return v; };
    auto f32 = [&] (std::uint64_t va) { float v = 0; rd (offset (va), v); return v; };
    auto samples = [&] (std::uint64_t descriptor) {
        const auto data = u64 (descriptor);
        const auto bytes = (std::size_t) std::max (0, i32 (u64 (descriptor + 8)));
        std::vector<std::int16_t> s (bytes / 2);
        const auto at = offset (data);
        if (at + bytes > b.size() || s.empty())
            return std::vector<std::int16_t> {};
        std::memcpy (s.data(), b.data() + at, s.size() * 2);
        return s;
    };
    auto tables = std::make_unique<BooBassTables>();
    for (int z = 0; z < 6; ++z)
    {
        auto& zone = tables->zones[(std::size_t) z];
        const auto att = samples (base + 0xCB920 + 16 * (std::uint64_t) z), loop = samples (base + 0xCB980 + 16 * (std::uint64_t) z);
        if (att.empty() || loop.empty())
            return nullptr;
        zone.root = i32 (base + 0xCB9E0 + 4 * (std::uint64_t) z);
        const auto attDb = f32 (base + 0xCB9F8 + 4 * (std::uint64_t) z), loopDb = f32 (base + 0xCBA10 + 4 * (std::uint64_t) z);
        const auto ga = (float) std::pow (10.0, -attDb / 20.0);
        zone.attack.assign (att.size() + 8, 0.0f);
        for (std::size_t i = 0; i < att.size(); ++i)
            zone.attack[i + 4] = att[i] * 3.0517578e-05f * ga * 0.5f;
        for (int g = 0; g < 4; ++g)
            zone.attack[(std::size_t) g] = zone.attack[4], zone.attack[att.size() + 4 + (std::size_t) g] = zone.attack[att.size() + 3];
        zone.attackLength = (double) att.size() - 1.0;
        // one loop cycle → 16 384-point periodic Catmull-Rom table, × −0.5·10^(−dB/20)
        const auto n = (int) loop.size();
        std::vector<float> src ((std::size_t) n);
        for (int i = 0; i < n; ++i)
            src[(std::size_t) i] = loop[(std::size_t) i] * 3.0517578e-05f;
        const auto gl = (float) std::pow (10.0, -loopDb / 20.0);
        zone.loop.assign (16384 + 8, 0.0f);
        const auto step = (float) n * 6.1035156e-05f;
        float pos = 0.0f;
        for (int k = 0; k < 16384; ++k)
        {
            const auto i = (int) pos;
            const auto t = pos - (float) i;
            auto at = [&] (int j) { return src[(std::size_t) (((j % n) + n) % n)]; };
            const auto xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
            const auto A = (x2 - xm1) * 0.5f + (x0 - x1) * 1.5f;
            const auto B = ((xm1 - x0 * 2.5f) + x1) + x1 - x2 * 0.5f;
            const auto C = (x1 - xm1) * 0.5f;
            zone.loop[(std::size_t) k + 4] = -((((A * t + B) * t + C) * t + x0) * gl) * 0.5f;
            pos += step;
        }
        for (int g = 0; g < 4; ++g)
            zone.loop[(std::size_t) g] = zone.loop[16384 + (std::size_t) g], zone.loop[16388 + (std::size_t) g] = zone.loop[4 + (std::size_t) g];
    }
    static constexpr int roots[6] { 1100, 1600, 2300, 2800, 3500, 4000 };
    for (int z = 0; z < 6; ++z)
        if (tables->zones[(std::size_t) z].root != roots[z])
            return nullptr;
    return tables;
}

class BooBassInstrument final : public Instrument
{
public:
    ~BooBassInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete tables;
    }

    void prepare (double sr) noexcept override
    {
        fs = sr;
        k44 = (float) (44100.0 / sr);
        fs44 = (float) (sr / 44100.0);
        c1 = k44 * 0.025f;
        c2 = k44 * 0.155f;
        inc0 = 267955.84f / (float) sr;
        rate0 = 16000.0f / (float) sr;
        gainCurve[0] = gainCurve[1] = 0.0f;
        for (int i = 2; i < 48; ++i)
            gainCurve[(std::size_t) i] = (float) std::pow (10.0, (i - 48) / 20.0);
        gainCurve[48] = 1.0f;
        gainCurve[49] = 0.0f;
        const auto w = k44 * 0.18f;
        pb = (w * 3.6f - w * 1.6f * w) - 1.0f;
        pa = (pb + 1.0f) * 0.5f;
        active = false;
    }

    void configure (const ChannelDef& channel, double) noexcept override
    {
        delete retired.exchange (nullptr);
        if (channel.samplePath != loadedPath)
        {
            loadedPath = channel.samplePath;
            if (auto t = loadBooBassTables (loadedPath))
                delete incoming.exchange (t.release());
        }
        std::array<float, 3> k { 32767, 32767, 32767 };
        for (std::size_t i = 0; i < 3 && i < channel.modelParams.size(); ++i)
            k[i] = channel.modelParams[i];
        knobs.write (k);
        pitchCents.store (channel.sampler.pitch * 100.0f);
    }

    void setNextNoteExpression (float, float cents, bool) noexcept override { nextCents = cents; }

    // a slide note moves Final.Pitch, which BooBass reads every block (REA_rodada29 §4.2; issue #14)
    void slideTo (int key, float cents, float samples) noexcept override
    {
        if (active && stage < 3)
            slide.start (100.0f * (float) (key - 60) + pitchCents.load() + cents - finalPitch, samples);
    }

    void noteOn (int key, float) noexcept override
    {
        take();
        if (tables == nullptr)
            return;
        // FL's final pitch = 100·(key − 60) cents (round 28 §4); p = trunc(pitch + 2400) cents above 16.35 Hz.
        // Keys below 36 (p < 0) read outside FL's table: LYNX clamps p at 0 (its own choice)
        currentKey = key;
        // + the channel's PITCH knob, fine tune and main pitch: FL's Final.Pitch (issue #14)
        finalPitch = 100.0f * (key - 60) + (pitchCents.load() + nextCents);
        nextCents = 0.0f;
        slide = {};
        const auto p = std::clamp ((int) (finalPitch + 2400.0f), 0, 9600);
        int z = 5;
        while (z >= 0 && tables->zones[(std::size_t) z].root > p)
            --z;
        if (z < 0)
            z = 0;
        else if (z <= 4 && std::abs (tables->zones[(std::size_t) z + 1].root - p) < std::abs (tables->zones[(std::size_t) z].root - p))
            ++z;
        zone = z;
        pitch = p;
        setPitch (p);
        attackPos = 0.0f;
        phase = 0.0f;
        const auto decay = (int) ((float) fs / ((float) std::pow (2.0, p / 1200.0) * 4.088681640625f));
        const auto sustain = (int) ((float) (tables->zones[(std::size_t) z].attack.size() - 8 << 4) * fs44);
        const auto release = (int) ((float) fs / 55.0f);
        counts = { 0, decay, sustain, release, -1 };
        targets = { 1.0f, 0.9375f, 0.0f, 0.0f, 0.0f };
        slopes = { 0.0f, decay ? (0.9375f - 1.0f) / decay : 0.0f, sustain ? (0.0f - 0.9375f) / sustain : 0.0f, 0.0f, 0.0f };
        stage = 0;
        countdown = counts[0];
        slope = slopes[0];
        value = 0.0f;
        gainsNow = gainsTarget;
        active = true;
    }

    void noteOff (int key) noexcept override
    {
        if (active && key == currentKey && stage < 3)
        {
            const auto r = counts[3];
            countdown = r;
            if (r)
                slope = -value / (float) r;
            stage = 3;
        }
    }

    void allNotesOff() noexcept override { active = false; }
    bool idle() const noexcept override { return ! active; }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (! active || tables == nullptr)
            return;
        // FL's per-voice level Vol·cos/sin((1 + pan)π/4) vs LYNX's √2·cos/sin channel gains (REA_rodada28 §4)
        gl *= 0.70710678f;
        gr *= 0.70710678f;
        for (int done = 0; done < n && active; done += 256)
        {
            const auto m = std::min (256, n - done);
            renderBlock (m);
            for (int i = 0; i < m; ++i)
            {
                left[done + i] += block[(std::size_t) i] * gl;
                right[done + i] += block[(std::size_t) i] * gr;
            }
        }
    }

private:
    void take() noexcept
    {
        std::array<float, 3> k;
        if (knobs.read (k))
            setKnobs (k);
        if (retired.load() == nullptr)
            if (auto* t = incoming.exchange (nullptr))
            {
                retired.store (tables);
                tables = t;
                active = false;
            }
    }

    void setKnobs (const std::array<float, 3>& k) noexcept
    {
        auto x = [&] (int i) { return std::clamp (k[(std::size_t) i], 0.0f, 65535.0f) / 65535.0f; };
        const auto xb = x (0), xm = x (1), xt = x (2);
        auto on = [] (float v) { return v >= 0.1f; };
        gainsTarget[0] = on (xb) ? (float) std::pow (10.0, (xb * 12.0f - 12.0f) / 20.0f) : 0.0f;
        bassSat = on (xb) ? (float) std::pow (10.0, (xb * 24.0f - 24.0f) / 20.0f) : 0.0f;
        gainsTarget[1] = on (xm) ? (float) std::pow (10.0, (xm * 10.0f - 10.0f) / 20.0f) : 0.0f;
        gainsTarget[2] = on (xt) ? (float) std::pow (10.0, (xt * 12.0f - 9.0f) / 20.0f) : 0.0f;
    }

    void setPitch (int p) noexcept
    {
        p = std::min (p, 9600);
        increment = (float) (std::pow (2.0, p / 1200.0) * inc0);
        const auto d = p - tables->zones[(std::size_t) zone].root;
        const auto r = d == 0 ? 1.0f : d > 0 ? (float) std::pow (2.0, d / 1200.0) : (float) (1.0 / std::pow (2.0, -d / 1200.0));
        rate = r * rate0;
    }

    void tickEnvelope() noexcept
    {
        if (countdown == 0)
            while (true)
            {
                if (stage == 2)
                {
                    const auto r = counts[3];
                    targets[3] = 0.0f;
                    slopes[3] = r ? -value / (float) r : 0.0f;
                    stage = 3;
                    countdown = counts[3];
                }
                else
                {
                    value = targets[(std::size_t) stage];
                    ++stage;
                    countdown = counts[(std::size_t) stage];
                }
                slope = slopes[(std::size_t) stage];
                if (countdown != 0)
                    break;
            }
        --countdown;
        value += slope;
    }

    float sample() noexcept
    {
        const auto& zn = tables->zones[(std::size_t) zone];
        auto L = [&] (int i) { return zn.loop[(std::size_t) (i + 4)]; };
        auto A = [&] (int i) { return zn.attack[(std::size_t) (i + 4)]; };
        const auto i0 = (int) phase;
        const auto v0 = L (i0);
        phase += increment;
        auto i1 = (int) phase;
        if (i1 > 0x3fff)
            phase -= 16384.0f, i1 -= 0x4000;
        auto x = v0 + (L (i1) - v0) * (phase - (float) i1);
        float xf = 1.0f;
        if (attackPos < (float) zn.attackLength)
        {
            const auto ia = (int) attackPos;
            const auto fa = attackPos - (float) ia;
            const auto a = (A (ia + 1) - A (ia)) * fa + A (ia);
            attackPos += rate;
            xf = attackPos / (float) zn.attackLength;
            x = a + (x - a) * xf;
        }
        const auto e = std::clamp (value, 0.0f, 1.0f);
        const auto ei = (int) (e * 48.0f);
        x *= (gainCurve[(std::size_t) ei + 1] - gainCurve[(std::size_t) ei]) * (e * 48.0f - (float) ei) + gainCurve[(std::size_t) ei];
        tickEnvelope();
        if (x > -5.9604652e-08f && x < 5.9604652e-08f)
            x = 0.0f;
        const auto gB = gainsNow[0], gM = gainsNow[1], gT = gainsNow[2];
        lp[0] = (x - lp[0]) * c1 + lp[0];
        lp[1] = (x - lp[1]) * c2 + lp[1];
        const auto y = ((lp[1] - lp[0]) * gM + lp[0] * gB) + (x - lp[1]) * gT;
        lp[2] = (y - lp[2]) * c1 + lp[2];
        lp[3] = (y - lp[3]) * c2 + lp[3];
        auto v = lp[2];
        if (v < -1.0f)
            lp[2] = v = -1.0f;
        else if (v > 1.0f)
            lp[2] = v = 1.0f;
        const auto limit = (xf + 0.032812502f) * k44;
        const auto sat = (float) std::sin (v * 1.5707963267948966);
        const auto target = ((((sat - v) * bassSat + v) * gB + (lp[3] - v) * gM) + (y - lp[3]) * gT) * 3.0f - slew;
        slew += std::clamp (target, -limit, limit);
        return slew;
    }

    void renderBlock (int n) noexcept
    {
        if (slide.moving() || slide.cents != 0.0f) // the zone and the decay stay as chosen at note on
        {
            slide.advance ((float) n);
            pitch = std::clamp ((int) (finalPitch + slide.cents + 2400.0f), 0, 9600);
        }
        setPitch (pitch);
        std::array<float, 3> delta {};
        for (std::size_t k = 0; k < 3; ++k)
        {
            auto d = gainsTarget[k] - gainsNow[k];
            if (d > -5.9604652e-08f && d < 5.9604652e-08f)
                d = 0.0f;
            else
            {
                d /= (float) n;
                if (d > -5.9604652e-08f && d < 5.9604652e-08f)
                    d = 0.0f;
                d *= 16.0f;
            }
            delta[k] = d;
        }
        auto count = n;
        for (int i = 0; i < n; ++i)
        {
            auto x = sample();
            if ((count & 15) == 0)
                for (std::size_t k = 0; k < 3; ++k)
                    gainsNow[k] += delta[k];
            --count;
            if (x > -5.9604652e-08f && x < 5.9604652e-08f)
                x = 0.0f;
            for (int s = 0; s < 4; ++s)
            {
                const auto ys = (pa * x + pa * postX[(std::size_t) s]) - pb * postY[(std::size_t) s];
                postX[(std::size_t) s] = x;
                postY[(std::size_t) s] = ys;
                x = ys;
            }
            block[(std::size_t) i] = x;
        }
        if (stage == 4)
            active = false;
    }

    std::atomic<BooBassTables*> incoming { nullptr }, retired { nullptr };
    BooBassTables* tables = nullptr; // audio thread
    std::string loadedPath;          // configure() side
    Mailbox<std::array<float, 3>> knobs;
    double fs = 44100.0;
    float k44 = 1, fs44 = 1, c1 = 0.025f, c2 = 0.155f, inc0 = 6.0f, rate0 = 0.36f, pa = 0, pb = 0;
    std::array<float, 50> gainCurve {};
    std::array<float, 3> gainsTarget {}, gainsNow {};
    float bassSat = 0.0f;
    bool active = false;
    int zone = 0, pitch = 0, currentKey = -1, stage = 0, countdown = 0;
    std::atomic<float> pitchCents { 0.0f }; // the channel's PITCH knob (configure)
    float nextCents = 0.0f, finalPitch = 0.0f;
    lynx::HostSlide slide;
    std::array<int, 5> counts {};
    std::array<float, 5> targets {}, slopes {};
    float slope = 0.0f, value = 0.0f, increment = 0.0f, rate = 0.0f, attackPos = 0.0f, phase = 0.0f, slew = 0.0f;
    std::array<float, 4> lp {}, postX {}, postY {};
    std::array<float, 256> block {};
};
} // namespace

std::unique_ptr<Instrument> makeSakuraInstrument() { return std::make_unique<SakuraInstrument>(); }
std::unique_ptr<Instrument> makeDrumpadInstrument() { return std::make_unique<DrumpadInstrument>(); }
std::unique_ptr<Instrument> makeBooBassInstrument() { return std::make_unique<BooBassInstrument>(); }
bool booBassDllUsable (const std::string& path) { return loadBooBassTables (path) != nullptr; }
} // namespace lynx
