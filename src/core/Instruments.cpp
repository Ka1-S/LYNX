#include "core/FruityDance.h"
#include "core/Instruments.h"
#include "core/ControlSurface.h"
#include "core/VoltageController.h"
#include "core/Vfx.h"
#include "core/Scheduler.h"

#include "core/DirectWave.h"
#include "core/FlSynths.h"
#include "core/FlResampler.h"
#include "core/TimeStretch.h"
#include "core/FlVoiceProcessor.h"
#include "core/DrumSynthLive.h"
#include "core/Gms.h"
#include "core/Harmless.h"
#include "core/Morphine.h"
#include "core/SytrusEngine.h"
#include "core/Harmor.h"
#include "core/MiniSynth.h"
#include "core/SimSynth.h"
#include "core/Wasp.h"
#include "core/Plucked.h"
#include "core/SoundFont.h"
#include "core/BeepMap.h"
#include "core/Sawer.h"
#include "core/Drumaxx.h"
#include "core/Granulizer.h"
#include "core/Slayer.h"
#include "core/Ogun.h"
#include "core/TransistorBass.h"
#include "core/Dx10.h"
#include "core/Kepler.h"
#include "core/PoiZone.h"
#include "core/BassDrum.h"
#include "core/FruitKick.h"
#include "core/Toxic.h"
#include "core/ThreeOsc.h"
#include "core/PhysicalModels.h"

#include <algorithm>
#include <utility>
#include <atomic>

#include <cmath>
#include <complex>

namespace lynx
{
namespace
{
// 1 / channelGain at FL's default channel volume (10000 of 12800)
float audioClipUnity() noexcept
{
    static const float unity = 1.0f / channelGain (10000.0f / 12800.0f);
    return unity;
}
} // namespace

namespace
{
constexpr double twoPi = 6.283185307179586;

// Per-sample factor for an exponential decay that drops 60 dB in `seconds`.
float decayCoefficient (double seconds, double sampleRate) noexcept
{
    return (float) std::exp (std::log (0.001) / (seconds * sampleRate));
}

// In-place complex radix-2 FFT (wavetables for the spectral oscillator)
void fftInPlace (std::vector<std::complex<double>>& a, bool inverse)
{
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const auto w = std::polar (1.0, (inverse ? 2.0 : -2.0) * twoPi / 2.0 / (double) len);
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> wn (1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * wn;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wn *= w;
            }
        }
    }
}

double midiToHz (int key) noexcept
{
    return 440.0 * std::pow (2.0, (key - 69) / 12.0);
}

// ---------------------------------------------------------------------------
// Drums: each hit takes a voice; a few voices allow short overlaps.

struct DrumVoice
{
    bool active = false;
    float level = 0.0f;     // amplitude envelope
    float velocity = 0.0f;
    double phase = 0.0;
    double pitchEnv = 0.0;  // kick/snare: pitch envelope
    float filterState = 0.0f;
    float lastNoise = 0.0f;
    float lastHighPass = 0.0f;
    int age = 0;            // samples since the hit
};

class DrumInstrument final : public Instrument
{
public:
    explicit DrumInstrument (InstrumentKind k) : kind (k) {}

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        const double decaySeconds = kind == InstrumentKind::kick    ? 0.45
                                    : kind == InstrumentKind::snare ? 0.22
                                    : kind == InstrumentKind::clap  ? 0.30
                                                                    : 0.06; // hat
        decay = decayCoefficient (decaySeconds, sr);
        pitchDecay = decayCoefficient (kind == InstrumentKind::kick ? 0.08 : 0.03, sr);
        allNotesOff();
    }

    void noteOn (int, float velocity) noexcept override
    {
        // Steals the oldest voice if all are busy.
        DrumVoice* target = &voices[0];

        for (auto& v : voices)
        {
            if (! v.active)
            {
                target = &v;
                break;
            }

            if (v.age > target->age)
                target = &v;
        }

        *target = {};
        target->active = true;
        target->level = 1.0f;
        target->velocity = velocity;
        target->pitchEnv = 1.0;
    }

    void noteOff (int) noexcept override {} // drums play until the end of the decay

    void allNotesOff() noexcept override
    {
        for (auto& v : voices)
            v.active = false;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            for (int i = 0; i < n; ++i)
            {
                const auto s = nextSample (v) * v.velocity;
                left[i] += s * gl;
                right[i] += s * gr;
            }

            if (v.level < 0.0005f)
                v.active = false;
        }
    }

    bool idle() const noexcept override
    {
        return std::none_of (voices.begin(), voices.end(), [] (const auto& v) { return v.active; });
    }

private:
    float nextSample (DrumVoice& v) noexcept
    {
        float out = 0.0f;

        switch (kind)
        {
            case InstrumentKind::kick:
            {
                const auto hz = 45.0 + 110.0 * v.pitchEnv; // sweep 155 → 45 Hz
                v.phase += twoPi * hz / sampleRate;
                out = (float) std::sin (v.phase) * v.level;
                break;
            }

            case InstrumentKind::snare:
            {
                const auto hz = 180.0 + 60.0 * v.pitchEnv;
                v.phase += twoPi * hz / sampleRate;
                const auto tone = (float) std::sin (v.phase) * (float) v.pitchEnv;
                const auto noise = highPass (v, noiseGen.next());
                out = (0.5f * tone + 0.7f * noise) * v.level;
                break;
            }

            case InstrumentKind::hat:
                out = 0.8f * highPass (v, noiseGen.next()) * v.level;
                break;

            case InstrumentKind::clap:
            {
                // Three short snaps followed by a tail: mimics several hands.
                const auto ms = v.age * 1000.0 / sampleRate;
                const auto burst = ms < 30.0 ? (float) std::exp (-std::fmod (ms, 10.0) / 2.0) : 1.0f;
                out = 0.8f * bandPass (v, noiseGen.next()) * v.level * burst;
                break;
            }

            case InstrumentKind::synth:
            case InstrumentKind::sampler:
            case InstrumentKind::audioClip:
            case InstrumentKind::plugin:
            case InstrumentKind::controller:
            case InstrumentKind::midiOut:
            case InstrumentKind::layer:
            case InstrumentKind::dashboard:
            case InstrumentKind::voltage:
            case InstrumentKind::vfx:
            case InstrumentKind::patcher:
            case InstrumentKind::sakura:
            case InstrumentKind::drumpad:
            case InstrumentKind::boobass:
            case InstrumentKind::drumsynth:
            case InstrumentKind::harmless:
            case InstrumentKind::sytrus:
            case InstrumentKind::harmor:
            case InstrumentKind::gms:
            case InstrumentKind::morphine:
            case InstrumentKind::ogun:
            case InstrumentKind::minisynth:
            case InstrumentKind::dx10:
            case InstrumentKind::kepler:
            case InstrumentKind::poizone:
            case InstrumentKind::fruitkick:
            case InstrumentKind::bassdrum:
            case InstrumentKind::toxic:
            case InstrumentKind::threeosc:
            case InstrumentKind::autogun:
            case InstrumentKind::transistorbass:
            case InstrumentKind::simsynth:
            case InstrumentKind::wasp:
            case InstrumentKind::plucked:
            case InstrumentKind::beepmap:
            case InstrumentKind::sawer:
            case InstrumentKind::flkeys:
            case InstrumentKind::granulizer:
            case InstrumentKind::drumaxx:
            case InstrumentKind::slayer:
            case InstrumentKind::fruitydance:
            case InstrumentKind::directwave:
            case InstrumentKind::soundfont:
                break;
        }

        v.level *= decay;
        v.pitchEnv *= pitchDecay;
        ++v.age;
        return out;
    }

    // Simple 1st-order high-pass (difference), good enough for cymbal noise.
    static float highPass (DrumVoice& v, float x) noexcept
    {
        const auto y = 0.85f * (v.filterState + x - v.lastNoise);
        v.lastNoise = x;
        v.filterState = y;
        return y;
    }

    // Cheap band-pass: high-pass followed by a two-point average.
    static float bandPass (DrumVoice& v, float x) noexcept
    {
        const auto hp = highPass (v, x);
        const auto y = 0.5f * (hp + v.lastHighPass);
        v.lastHighPass = hp;
        return y;
    }

    InstrumentKind kind;
    double sampleRate = 44100.0;
    float decay = 0.999f;
    double pitchDecay = 0.999;
    std::array<DrumVoice, 4> voices {};
    Noise noiseGen;
};

// Per-voice pan gains (constant power, 1 at center).
inline void voicePan (float pan, float& left, float& right) noexcept
{
    const auto angle = (std::clamp (pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
    left = std::cos (angle) * 1.41421356f;
    right = std::sin (angle) * 1.41421356f;
}

// ---------------------------------------------------------------------------
// Simple polyphonic synth: sawtooth with low-pass and AR envelope.

// ---------------------------------------------------------------------------
// Modulation envelopes (cutoff and pitch), per voice: 0 attack, 1 hold, 2 decay,
// 3 sustain, 4 release. Straight segments (tension 0, the FL default; REA_rodada3 §1.5).

struct ModState
{
    float level = 0.0f;
    float releaseStep = 0.0f; // fixed on note release: from the current level down to 0
    int stage = 0, held = 0;
    // Toxic shape: samples left in the current 128-sample block, elapsed time and the block's target/coefficient
    int blockLeft = 0;
    double elapsed = 0.0;
    float target = 0.0f, coefficient = 0.0f;
};

struct ModCoefficients
{
    bool active = false, toxic = false;
    float amount = 0.0f, attackStep = 1.0f, decayStep = 1.0f, releasePerLevel = 1.0f, sustain = 0.0f;
    float toxicAttack = 0.0f, toxicDecay = 0.0f, toxicRelease = 0.0f; // per-sample coefficients
    int holdSamples = 0;

    static ModCoefficients make (const ModEnvelope& e, double sampleRate) noexcept
    {
        ModCoefficients c;
        c.active = e.amount != 0.0f;
        c.amount = e.amount;
        if (e.shape == 1)
        {
            // REA_rodada33 §2.1–2.2: times in seconds (floor 0.001), sustain level as is
            auto seconds = [] (float t) { return std::max (t, 0.001f); };
            c.toxic = true;
            c.attackStep = seconds (e.attack); // the attack stage lasts while elapsed < A
            c.sustain = e.sustain;
            c.toxicAttack = (float) (8.82 / ((double) seconds (e.attack) * 39.441975 * sampleRate));
            c.toxicDecay = (float) (8.82 / ((double) seconds (e.decay) * sampleRate));
            c.toxicRelease = (float) (8.82 / ((double) seconds (e.release) * sampleRate));
            return c;
        }
        c.attackStep = (float) (1.0 / (std::max (0.0005, (double) e.attack) * sampleRate));
        c.holdSamples = (int) (std::max (0.0f, e.hold) * sampleRate);
        c.sustain = std::clamp (e.sustain, 0.0f, 1.0f);
        c.decayStep = (float) ((1.0 - c.sustain) / (std::max (0.0005, (double) e.decay) * sampleRate)); // from 1 to sustain
        c.releasePerLevel = (float) (1.0 / (std::max (0.002, (double) e.release) * sampleRate));
        return c;
    }

    void advance (ModState& m) const noexcept
    {
        if (toxic)
        {
            // stage chosen at each block start (released → 0, attack → 5 while elapsed < A, else decay → S + 1e-7);
            // one-pole step every sample, clipped at 1
            if (--m.blockLeft < 0)
            {
                m.blockLeft = 127;
                if (m.stage == 4)
                    m.target = 0.0f, m.coefficient = toxicRelease;
                else if ((double) attackStep <= m.elapsed)
                    m.target = sustain + 1.0e-7f, m.coefficient = toxicDecay;
                else
                    m.target = 5.0f, m.coefficient = toxicAttack;
                m.elapsed += 128.0 / 44100.0;
            }
            m.level = std::min (1.0f, (m.target - m.level) * m.coefficient + m.level);
            return;
        }
        switch (m.stage)
        {
            case 0:
                m.level += attackStep;
                if (m.level >= 1.0f) { m.level = 1.0f; m.stage = 1; }
                break;
            case 1:
                if (++m.held >= holdSamples) m.stage = 2;
                break;
            case 2:
                m.level -= decayStep;
                if (m.level <= sustain) { m.level = sustain; m.stage = 3; }
                break;
            case 3: break;
            default: m.level = std::max (0.0f, m.level - m.releaseStep); break;
        }
    }

    void release (ModState& m) const noexcept
    {
        m.stage = 4;
        m.releaseStep = m.level * releasePerLevel;
    }
};

// Channel LFO, advanced on the control tick.
struct LfoState
{
    double phase = 0.0;
    float fade = 0.0f;
    double delayLeft = 0.0; // samples
};

struct LfoCoefficients
{
    bool active = false;
    float amount = 0.0f;
    double increment = 0.0; // cycles per sample
    double delaySamples = 0.0;
    float fadeStep = 1.0f;   // per sample
    int shape = 0;
    bool global = false;
    double cyclesPerTick = 0.0; // global: phase = song position (ticks) × this

    static LfoCoefficients make (const ChannelLfo& l, double sampleRate, double bpm) noexcept
    {
        LfoCoefficients c;
        c.active = l.amount != 0.0f;
        c.amount = l.amount;
        const auto unit = l.sync ? 60.0 / std::max (1.0, bpm) : 1.0; // beats -> seconds
        c.increment = 1.0 / (std::max (0.005, (double) l.period * unit) * sampleRate);
        const auto timeUnit = l.secondsTiming ? 1.0 : unit;
        c.delaySamples = std::max (0.0, (double) l.delay * timeUnit * sampleRate);
        c.fadeStep = l.attack > 0.0f ? (float) (1.0 / ((double) l.attack * timeUnit * sampleRate)) : 1.0f;
        c.shape = std::clamp (l.shape, 0, 2);
        c.global = l.global;
        // period in beats (synced) or in seconds converted via BPM
        const auto periodBeats = l.sync ? std::max (0.005, (double) l.period) : std::max (0.005, (double) l.period) * std::max (1.0, bpm) / 60.0;
        c.cyclesPerTick = 1.0 / (periodBeats * ppq);
        return c;
    }

    // `songTicks`: song position (a global LFO starts at its phase; delay and fade-in stay per note)
    void start (LfoState& s, double songTicks = 0.0) const noexcept
    {
        s = { 0.0, fadeStep >= 1.0f ? 1.0f : 0.0f, delaySamples };
        if (global)
        {
            s.phase = songTicks * cyclesPerTick;
            s.phase -= std::floor (s.phase);
        }
    }

    // advances `n` samples and returns the output (±1, fade-in already applied)
    float tick (LfoState& s, int n) const noexcept
    {
        if (s.delayLeft > 0.0)
        {
            s.delayLeft -= n;
            return 0.0f;
        }

        s.phase += increment * n;
        s.phase -= std::floor (s.phase);
        s.fade = std::min (1.0f, s.fade + fadeStep * (float) n);
        const auto p = s.phase;
        float value = 0.0f;

        switch (shape)
        {
            case 1: value = (float) (p < 0.25 ? 4.0 * p : p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0); break;
            case 2: value = p < 0.5 ? 1.0f : -1.0f; break;
            default: value = (float) std::sin (6.283185307179586 * p); break;
        }

        return value * s.fade;
    }
};

// "TPT" SVF low-pass coefficients (stable at any cutoff), shared by the synth and the sampler.
// The same filter gives all four responses (mode: 0 low-pass, 1 band-pass, 2 high-pass,
// 3 band-stop), like the LP/BP/HP/BS types of FL's channel filter.
struct SvfCoefficients
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 2.0f;
    int mode = 0;
    // 4-pole ladder (Toxic Biohazard, REA_rodada26 §2): f = 2fc/fs, k = 3.6f − 1.6f² − 1, p = (k + 1)/2, feedback
    // res·R(u) with u = (log2 fc − 5)/9; stages s = (x + x_prev)·p − k·s_prev; input saturated by
    // v·(1 − 3a + 4a² − 2a³), a = min(|v|/15, 1). (FL builds its table for 44.1 kHz; LYNX keeps fc in Hz at any rate.)
    bool ladder = false;
    float lp = 0.0f, lk = 0.0f, feedback = 0.0f;

    void setLadder (double cutoffHz, float resonance, double sampleRate, int filterMode) noexcept
    {
        ladder = true;
        mode = std::clamp (filterMode, 0, 2);
        const auto fc = std::clamp (cutoffHz, 32.0, 16384.0 * sampleRate / 44100.0);
        const auto f = 2.0 * fc / sampleRate;
        const auto kk = 3.6 * f - 1.6 * f * f - 1.0, p = (kk + 1.0) / 2.0;
        const auto u = std::clamp ((std::log2 (fc) - 5.0) / 9.0, 0.0, 1.0);
        const auto r = 1.96 * std::pow (1.0 - u, 1.5)
                       + 0.98 * (0.95 * std::exp ((1.0 - p) * 1.386249) + (u > 0.7 ? 8.0 / 9.0 * (u - 0.7) * (u - 0.7) : 0.0));
        lp = (float) p;
        lk = (float) kk;
        feedback = (float) (std::clamp (resonance, 0.0f, 1.0f) * r);
    }

    // state: s1…s4 and the previous input of each stage
    float processLadder (float in, std::array<float, 8>& st) const noexcept
    {
        auto v = in - feedback * st[3];
        const auto a = std::min (std::abs (v) / 15.0f, 1.0f);
        v *= 1.0f - 3.0f * a + 4.0f * a * a - 2.0f * a * a * a;
        auto x = v;
        for (int i = 0; i < 4; ++i)
        {
            const auto s = (x + st[(std::size_t) i + 4]) * lp - lk * st[(std::size_t) i];
            st[(std::size_t) i + 4] = x;
            st[(std::size_t) i] = s;
            x = s;
        }
        switch (mode)
        {
            case 1: return st[2] - st[3];
            case 2: return v - st[3];
            default: return st[3];
        }
    }

    void set (double cutoffHz, float resonance, double sampleRate, int filterMode = 0, bool useLadder = false) noexcept
    {
        if (useLadder)
        {
            setLadder (cutoffHz, resonance, sampleRate, filterMode);
            return;
        }
        ladder = false;
        const auto fc = std::clamp (cutoffHz, 20.0, sampleRate * 0.45);
        const auto g = (float) std::tan (3.14159265358979 * fc / sampleRate);
        k = 2.0f - 1.9f * std::clamp (resonance, 0.0f, 1.0f); // 2 = no resonance
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        mode = std::clamp (filterMode, 0, 3);
    }

    float process (float in, float& low, float& band) const noexcept
    {
        const auto v3 = in - low;
        const auto v1 = a1 * band + a2 * v3;
        const auto v2 = low + a2 * band + a3 * v3;
        band = 2.0f * v1 - band;
        low = 2.0f * v2 - low;

        switch (mode)
        {
            case 1: return v1;
            case 2: return in - k * v1 - v2;
            case 3: return in - k * v1;
            default: return v2;
        }
    }
};

// Kepler's chorus (REA_rodada26 §3): mono input → high-pass (0.5 Hz) → two RBJ low-passes (7 kHz, Q 0.707) →
// one delay line read by two taps (L and R LFO phases) with Catmull-Rom interpolation → 1-pole low-pass 7 kHz →
// equal-power mix (m → 0.5). Delay = base + depth·w(LFO), smoothed by s = 1 − 0.1^(20/fs). Noise/gate not modelled.
class KeplerChorus
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr;
        smooth = 1.0 - std::pow (0.1, 20.0 / sr);
        const auto th = std::tan (3.141592653589793 * 0.5 / sr);
        hpB = 1.0 / (1.0 + th);
        hpA = (1.0 - th) / (1.0 + th);
        const auto tl = std::tan (3.141592653589793 * 7000.0 / sr);
        lpB = tl / (1.0 + tl);
        lpA = (1.0 - tl) / (1.0 + tl);
        const auto w0 = 2.0 * 3.141592653589793 * 7000.0 / sr, alpha = std::sin (w0) / (2.0 * 0.707), a0 = 1.0 + alpha;
        bq = { (1.0 - std::cos (w0)) / 2.0 / a0, (1.0 - std::cos (w0)) / a0, (1.0 - std::cos (w0)) / 2.0 / a0,
               -2.0 * std::cos (w0) / a0, (1.0 - alpha) / a0 };
        reset();
    }

    void reset() noexcept
    {
        line.fill (0.0f);
        write = 0;
        phase = 0.0f;
        delay = { 0.01, 0.01 };
        mix = 0.0;
        outputGain = 0.0;
        hpX = hpY = 0.0;
        biquad = {};
        post = {};
    }

    // in place on a mono-summed signal; left/right receive the stereo result
    void process (float* left, float* right, int n, int mode) noexcept
    {
        const auto rate = mode == 3 ? 8.0 : mode == 2 ? 0.6 : 0.4;
        const auto offset = mode == 3 ? 0.0286111f : 0.5f;
        const auto base = mode == 3 ? 0.00285 : 0.00108, depth = mode == 3 ? 0.00052 : 0.00415;
        const auto increment = (float) (rate / sampleRate);
        for (int i = 0; i < n; ++i)
        {
            const double dry = 0.5 * ((double) left[i] + right[i]);
            // pre-filter (high-pass, then two low-pass biquads) into the delay line
            const auto hp = hpB * (dry - hpX) + hpA * hpY;
            hpX = dry;
            hpY = hp;
            auto x = hp;
            for (auto& st : biquad)
            {
                const auto y = bq[0] * x + st[0];
                st[0] = bq[1] * x - bq[3] * y + st[1];
                st[1] = bq[2] * x - bq[4] * y;
                x = y;
            }
            line[(std::size_t) write] = (float) x;

            mix += (0.5 - mix) * smooth;
            outputGain += (1.0 - outputGain) * smooth; // the unit fades in after it starts (§3.3 step 10)
            const auto wetGain = std::sqrt (mix), dryGain = std::sqrt (1.0 - mix);
            for (int c = 0; c < 2; ++c)
            {
                auto ph = phase + (c == 1 ? offset : 0.0f);
                ph -= std::floor (ph);
                const double w = mode == 3 ? (std::sin (2.0 * 3.141592653589793 * ph) + 1.0) / 2.0
                                 : ph < 0.25f ? 2.0 * ph + 0.5 : ph < 0.75f ? 1.5 - 2.0 * ph : 2.0 * ph - 1.5;
                auto& d = delay[(std::size_t) c];
                d += (base + depth * w - d) * smooth;
                const auto samples = d * sampleRate;
                const auto whole = (int) samples;
                const auto t = samples - whole;
                auto tap = [&] (int back) { return (double) line[(std::size_t) ((write - back) & mask)]; };
                const auto y0 = tap (whole - 1), y1 = tap (whole), y2 = tap (whole + 1), y3 = tap (whole + 2);
                const auto wet = y1 + 0.5 * t * (y2 - y0 + t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0)));
                auto& p = post[(std::size_t) c];
                const auto filtered = lpB * (wet + p[0]) + lpA * p[1];
                p[0] = wet;
                p[1] = filtered;
                (c == 0 ? left : right)[i] = (float) ((filtered * wetGain + dry * dryGain) * outputGain);
            }
            phase += increment;
            if (phase >= 1.0f)
                phase -= 1.0f;
            write = (write + 1) & mask;
        }
    }

private:
    static constexpr int size = 32768, mask = size - 1;
    double sampleRate = 44100.0, smooth = 0.001;
    double hpB = 1.0, hpA = 0.0, lpB = 0.5, lpA = 0.0;
    std::array<double, 5> bq {};
    std::array<std::array<double, 2>, 2> biquad {}, post {};
    double hpX = 0.0, hpY = 0.0, mix = 0.0, outputGain = 0.0;
    std::array<double, 2> delay { 0.01, 0.01 };
    std::array<float, size> line {};
    int write = 0;
    float phase = 0.0f; // float32 LFO phase, as in FL (§3.5)
};

struct SynthVoice
{
    float panL = 1.0f, panR = 1.0f;
    double glideRatio = 1.0;  // multiplies the frequency every sample during a slide
    int glideLeft = 0;
    bool active = false;
    bool releasing = false;
    int key = 0;
    float velocity = 0.0f;
    float level = 0.0f;       // envelope
    int stage = 0;            // 0 attack, 1 decay, 2 sustain, 3 release
    double phaseDelta = 0.0;  // of the fundamental (each oscillator multiplies it)
    std::array<double, 3> phase {};
    float low = 0.0f, band = 0.0f; // filter states
    float lowR = 0.0f, bandR = 0.0f; // right-side filter (only with per-oscillator pan)
    std::array<std::array<float, 8>, 2> ladder {}; // ladder filter states (left, right)

    std::array<ModState, 2> mod {}; // filter, pitch
    std::array<LfoState, lfoTargets> lfo {};
    float lfoGain = 1.0f, lfoGainStep = 0.0f; // volume LFO, ramped between ticks
    float basePan = 0.0f;                     // note pan (the pan LFO adds to it)
    float cents = 0.0f;                       // the note's fine pitch (a VFX may move it while it sounds)
    SvfCoefficients filter; // this voice's (only with a cutoff envelope)
    double pitchMul = 1.0;
    float fmLevel = 1.0f;   // FM index envelope (1 -> fmSustain)
    double sweep = 1.0;     // s of the pitch sweep (1 -> 0)
    float tone[2][2] {};    // states of the two low-passes of the 3-band tone control (per side)
    int controlCountdown = 0;
    int age = 0;
};

// Settings exchange without blocking the audio thread: the UI writes under a short flag and audio
// only copies when it can grab the flag (if it can't, it uses the old settings for this block).
template <typename T>
class SettingsMailbox
{
public:
    void write (const T& value) noexcept
    {
        while (busy.test_and_set (std::memory_order_acquire)) {}
        pending = value;
        dirty = true;
        busy.clear (std::memory_order_release);
    }

    bool read (T& out) noexcept
    {
        if (busy.test_and_set (std::memory_order_acquire))
            return false;

        const auto changed = dirty;

        if (changed)
        {
            out = pending;
            dirty = false;
        }

        busy.clear (std::memory_order_release);
        return changed;
    }

private:
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    T pending {};
    bool dirty = false;
};

class SynthInstrument final : public Instrument
{
public:
    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        lastPhaseDelta = 0.0;
        chorusUnit.prepare (sr);
        updateCoefficients();
        allNotesOff();
    }

    void configure (const ChannelDef& channel, double songBpm) noexcept override
    {
        bpm.store (std::max (1.0, songBpm)); // first: the synced LFO uses it when reading the settings
        updateFrames (channel.synth);
        mailbox.write (channel.synth);
    }

    ~SynthInstrument() override
    {
        delete incomingFrames.exchange (nullptr);
        delete retiredFrames.exchange (nullptr);
        delete activeFrames;
    }

    void startModulation (SynthVoice& v, float pan) noexcept
    {
        v.basePan = pan;
        v.lfoGain = 1.0f;
        v.lfoGainStep = 0.0f;
        v.sweep = 1.0;
        std::fill (&v.tone[0][0], &v.tone[0][0] + 4, 0.0f);
        v.controlCountdown = 0;

        for (std::size_t i = 0; i < lfoTargets; ++i)
            if (lfos[i].active)
                lfos[i].start (v.lfo[i], songTicks);
    }

    void setSongPosition (double ticks) noexcept override { songTicks = ticks; }

    // portamento: the voice starts at `from` and glides to `to` over the porta time (channel porta,
    // inverted by the note)
    void glide (SynthVoice& v, double from, double to) noexcept
    {
        const auto steps = (int) (settings.portaBeats * 60.0 / bpm.load() * sampleRate);
        const auto on = settings.porta != nextPorta;

        if (! on || settings.portaBeats <= 0.0f || from <= 0.0 || steps < 1)
        {
            v.phaseDelta = to;
            v.glideLeft = 0;
            return;
        }

        v.phaseDelta = from;
        v.glideRatio = std::pow (to / from, 1.0 / steps); // linear in semitones, like FL
        v.glideLeft = steps;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        if (mailbox.read (settings))
            updateCoefficients();

        if (settings.mono)
        {
            // mono channel (polyphony 1 in FL): the sounding voice changes note without resetting phase or filter;
            // the envelopes restart from the current level
            SynthVoice* sounding = nullptr;

            for (auto& v : voices)
                if (v.active && (sounding == nullptr || v.age < sounding->age))
                    sounding = &v;

            if (sounding != nullptr)
            {
                for (auto& v : voices)
                    if (&v != sounding)
                        v.active = false;

                auto& v = *sounding;
                v.key = key;
                v.velocity = velocity;
                v.releasing = false;
                v.stage = 0;
                v.age = 0;
                const auto target = midiToHz (key) / sampleRate * std::pow (2.0, nextCents / 1200.0);
                v.cents = nextCents;
                glide (v, v.phaseDelta, target);
                lastPhaseDelta = target;
                for (auto& m : v.mod)
                    m = { m.level, 0.0f, 0, 0 };
                v.fmLevel = 1.0f;
                voicePan (nextPan, v.panL, v.panR);
                startModulation (v, nextPan);
                nextPan = nextCents = 0.0f;
        nextPorta = false;
                return;
            }
        }

        SynthVoice* target = &voices[0];

        for (auto& v : voices)
        {
            if (! v.active)
            {
                target = &v;
                break;
            }

            if (v.age > target->age)
                target = &v;
        }

        *target = {};
        target->active = true;
        target->key = key;
        target->velocity = velocity;
        const auto delta = midiToHz (key) / sampleRate * std::pow (2.0, nextCents / 1200.0);
        target->cents = nextCents;
        glide (*target, lastPhaseDelta, delta); // with porta, starts from the previous note's pitch
        lastPhaseDelta = delta;
        voicePan (nextPan, target->panL, target->panR);
        startModulation (*target, nextPan);
        nextPan = nextCents = 0.0f;
        nextPorta = false;
    }

    void pitchBend (float semitones) noexcept override
    {
        bendMul = std::isfinite (semitones) ? std::exp2 ((double) std::clamp (semitones, -48.0f, 48.0f) / 12.0) : 1.0;
    }

    void setNextNoteExpression (float pan, float cents, bool porta) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
        nextPorta = porta;
    }

    // a VFX changes a sounding note (round 164b): its fine pitch, pan or volume
    void voiceProperty (int key, int prop, float value) noexcept override
    {
        if (! std::isfinite (value))
            return;
        for (auto& v : voices)
            if (v.active && ! v.releasing && v.key == key)
            {
                if (prop == (int) vfx::Prop::pitch && v.phaseDelta > 0.0)
                {
                    v.phaseDelta *= std::exp2 ((double) (value - v.cents) / 1200.0);
                    v.cents = value;
                }
                else if (prop == (int) vfx::Prop::pan)
                {
                    v.basePan = std::clamp (value, -1.0f, 1.0f);
                    voicePan (v.basePan, v.panL, v.panR);
                }
                else if (prop == (int) vfx::Prop::velocity)
                    v.velocity = std::clamp (value, 0.0f, 1.0f);
            }
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        const auto target = midiToHz (key) / sampleRate * std::pow (2.0, cents / 1200.0);
        const auto steps = std::max (1, (int) samples);

        for (auto& v : voices)
            if (v.active && ! v.releasing && v.phaseDelta > 0.0)
            {
                v.glideRatio = std::pow (target / v.phaseDelta, 1.0 / steps);
                v.glideLeft = steps; // the voice keeps its original key: its "off" is what ends the note
            }
    }

    void noteOff (int key) noexcept override
    {
        for (auto& v : voices)
            if (v.active && ! v.releasing && v.key == key)
            {
                v.releasing = true;
                v.stage = 3;
                for (std::size_t e = 0; e < 2; ++e)
                    modEnv[e].release (v.mod[e]);
            }
    }

    void allNotesOff() noexcept override
    {
        for (auto& v : voices)
            v.active = false;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        if (mailbox.read (settings))
            updateCoefficients();
        // new spectrum frames (built in configure): the old set goes back for the other thread to free
        if (retiredFrames.load() == nullptr)
            if (auto* frames = incomingFrames.exchange (nullptr))
            {
                retiredFrames.store (activeFrames); // a set with count 0 stays active as "static spectrum"
                activeFrames = frames;
                evolving = frames->count > 1;
            }

        // Kepler chorus: voices without the channel's volume/pan → chorus (mono in, stereo out) → volume/pan
        if (settings.chorus > 0)
        {
            const auto anyActive = std::any_of (voices.begin(), voices.end(), [] (const SynthVoice& v) { return v.active; });
            if (anyActive)
                chorusTail = (int) (0.05 * sampleRate); // the delay (≤ 5.2 ms) and filters ring a little after the last voice
            for (int done = 0; done < n; done += scratchSize)
            {
                const auto m = std::min (scratchSize, n - done);
                std::fill (scratchL.begin(), scratchL.begin() + m, 0.0f);
                std::fill (scratchR.begin(), scratchR.begin() + m, 0.0f);
                renderVoices (scratchL.data(), scratchR.data(), m, 1.0f, 1.0f);
                chorusUnit.process (scratchL.data(), scratchR.data(), m, settings.chorus);
                for (int i = 0; i < m; ++i)
                {
                    left[done + i] += scratchL[(std::size_t) i] * gl;
                    right[done + i] += scratchR[(std::size_t) i] * gr;
                }
            }
            if (! anyActive)
                chorusTail = std::max (0, chorusTail - n);
            return;
        }
        renderVoices (left, right, n, gl, gr);
    }

    void renderVoices (float* left, float* right, int n, float gl, float gr) noexcept
    {
        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            for (int i = 0; i < n; ++i)
            {
                if (evolving) // evolving spectrum: the two frames around the note's age
                {
                    const auto t = v.age / sampleRate;
                    const auto x = std::min ((double) (activeFrames->count - 1), (activeFrames->count - 1) * std::sqrt (t / activeFrames->span));
                    const auto k = std::min ((int) x, activeFrames->count - 2);
                    frameA = &activeFrames->tables[(std::size_t) k];
                    frameB = &activeFrames->tables[(std::size_t) k + 1];
                    frameMix = (float) (x - k);
                }
                // ADSR envelope
                switch (v.stage)
                {
                    case 0:
                        v.level += attackStep;
                        if (v.level >= 1.0f) { v.level = 1.0f; v.stage = 1; }
                        break;
                    case 1:
                        v.level = settings.sustain + (v.level - settings.sustain) * decayCoeff;
                        if (v.level - settings.sustain < 0.001f) { v.level = settings.sustain; v.stage = 2; }
                        break;
                    case 2: break;
                    default: v.level *= releaseCoeff; break;
                }

                if (v.glideLeft > 0)
                {
                    v.phaseDelta *= v.glideRatio;
                    --v.glideLeft;
                }

                if (modulated)
                {
                    for (std::size_t e = 0; e < 2; ++e)
                        if (modEnv[e].active)
                            modEnv[e].advance (v.mod[e]);

                    // every 16 samples (the control "tick"): pitch, filter, volume and pan
                    if (--v.controlCountdown <= 0)
                    {
                        v.controlCountdown = 16;
                        auto semitones = modEnv[1].active ? modEnv[1].amount * v.mod[1].level : 0.0f;
                        auto octaves = modEnv[0].active ? modEnv[0].amount * v.mod[0].level : 0.0f;
                        octaves += settings.keyTrack * (float) (v.key - 48) / 12.0f;

                        if (lfos[lfoPitch].active)
                            semitones += lfos[lfoPitch].amount * lfos[lfoPitch].tick (v.lfo[lfoPitch], 16);
                        if (settings.vibratoCents > 0.0f) // 6 Hz global phase, depth × r², r = 3·age/fs ≤ 1
                        {
                            const auto r = std::min (1.0, 3.0 * (v.age + 1) / sampleRate);
                            const auto phase = vibratoPhase + 6.0 * i / sampleRate;
                            semitones += (float) (settings.vibratoCents / 100.0 * r * r * std::sin (2.0 * 3.141592653589793 * phase));
                        }
                        if (lfos[lfoCutoff].active)
                            octaves += lfos[lfoCutoff].amount * lfos[lfoCutoff].tick (v.lfo[lfoCutoff], 16);
                        if (lfos[lfoVolume].active)
                        {
                            const auto target = std::max (0.0f, 1.0f + lfos[lfoVolume].amount * lfos[lfoVolume].tick (v.lfo[lfoVolume], 16));
                            v.lfoGainStep = (target - v.lfoGain) / 16.0f;
                        }
                        if (lfos[lfoPan].active)
                            voicePan (v.basePan + lfos[lfoPan].amount * lfos[lfoPan].tick (v.lfo[lfoPan], 16), v.panL, v.panR);

                        v.pitchMul = semitones != 0.0f ? std::exp2 (semitones / 12.0) : 1.0;
                        if (settings.sweep > 0.0f)
                        {
                            const auto s2 = v.sweep * v.sweep;
                            v.pitchMul *= 1.0 + settings.sweep * s2 * s2;
                            v.sweep *= sweepCoeff;
                        }

                        if (filterModulated)
                            v.filter.set (settings.cutoff * std::exp2 (octaves), settings.resonance, sampleRate, settings.filterType, settings.ladder);
                    }

                    v.lfoGain += v.lfoGainStep;
                }

                float sum = 0.0f, sumR = 0.0f, modulator = 0.0f;
                double fmShift = 0.0;

                // AM/FM: osc 3 is not summed, it modulates (first, so FM shifts the phase of osc 1 + 2)
                if (osc3Modulates)
                {
                    const auto& osc = settings.osc[2];
                    if (osc.level > 0.0f)
                    {
                        const auto delta = std::min (0.49, v.phaseDelta * ratios[2] * v.pitchMul * bendMul);
                        modulator = osc.level * oscillator (osc.wave, v.phase[2], delta);
                        v.phase[2] += delta;
                        if (v.phase[2] >= 1.0)
                            v.phase[2] -= 1.0;
                    }
                    if (settings.fm > 0.0f)
                    {
                        v.fmLevel = settings.fmSustain + (v.fmLevel - settings.fmSustain) * fmDecayCoeff;
                        fmShift = (double) (settings.fm * v.fmLevel * modulator);
                    }
                }

                for (std::size_t o = 0; o < (osc3Modulates ? 2u : 3u); ++o)
                {
                    const auto& osc = settings.osc[o];

                    if (osc.level <= 0.0f)
                        continue;

                    const auto delta = std::min (0.49, v.phaseDelta * ratios[o] * v.pitchMul * bendMul);
                    auto phase = v.phase[o];
                    if (fmShift != 0.0)
                    {
                        phase += fmShift;
                        phase -= std::floor (phase);
                    }
                    const auto value = osc.level * oscillator (osc.wave, phase, delta);
                    sum += value * oscPanL[o];
                    sumR += value * oscPanR[o];
                    v.phase[o] += delta;
                    if (v.phase[o] >= 1.0) // delta < 0.5: at most one wrap (no floor, which is a function call on CPUs without SSE4)
                        v.phase[o] -= 1.0;
                }

                if (settings.am) // (o1·g1 + o2·g2)·(1 − g3·o3), REA_rodada6 §3; ring (Sytrus): 1 − g3 + g3·o3, rodada18 §2
                {
                    const auto factor = settings.ring ? 1.0f - settings.osc[2].level + modulator : 1.0f - modulator;
                    sum *= factor;
                    sumR *= factor;
                }

                // resonant low-pass filter (state variable "TPT": stable at any cutoff)
                const auto& f = filterModulated ? v.filter : sharedFilter;
                auto outL = f.ladder ? f.processLadder (sum, v.ladder[0]) : f.process (sum, v.low, v.band);
                auto outR = ! stereoOscillators ? outL : f.ladder ? f.processLadder (sumR, v.ladder[1]) : f.process (sumR, v.lowR, v.bandR);

                if (useBands)
                {
                    auto bands = [&] (float x, float* st) {
                        st[0] += toneCoeff[0] * (x - st[0]);
                        st[1] += toneCoeff[1] * (x - st[1]);
                        return st[0] * settings.bandGain[0] + (st[1] - st[0]) * settings.bandGain[1] + (x - st[1]) * settings.bandGain[2];
                    };
                    outL = bands (outL, v.tone[0]);
                    outR = stereoOscillators ? bands (outR, v.tone[1]) : outL;
                }

                const auto s = 0.3f * v.level * v.velocity * v.lfoGain;
                auto tl = 1.0f, tr = 1.0f;
                if (settings.stereoTremolo > 0.0f) // rectified 2.5 Hz, L/R a quarter period apart; 0.1 s fade-in per voice
                {
                    const auto theta = 2.0 * 3.141592653589793 * (tremoloPhase + 2.5 * i / sampleRate);
                    const auto d = (double) settings.stereoTremolo;
                    const auto r = std::min (1.0, v.age / std::max (1.0, std::round (0.1 * sampleRate)));
                    tl = (float) (1.0 + r * d * (std::abs (1.25 * std::cos (theta)) - 1.0));
                    tr = (float) (1.0 + r * d * (std::abs (1.25 * std::sin (theta)) - 1.0));
                }
                left[i] += outL * s * gl * v.panL * tl;
                right[i] += outR * s * gr * v.panR * tr;
                ++v.age;
            }

            if (v.stage == 3 && v.level < 0.0005f)
                v.active = false;
        }
        // global free-running phases (Harmless tremolo and vibrato)
        tremoloPhase += 2.5 * n / sampleRate;
        tremoloPhase -= std::floor (tremoloPhase);
        vibratoPhase += 6.0 * n / sampleRate;
        vibratoPhase -= std::floor (vibratoPhase);
    }

    bool idle() const noexcept override
    {
        return chorusTail <= 0 && std::none_of (voices.begin(), voices.end(), [] (const auto& v) { return v.active; });
    }

private:
    static constexpr int scratchSize = 1024;
    std::array<float, scratchSize> scratchL {}, scratchR {};
    KeplerChorus chorusUnit;
    int chorusTail = 0;
    double tremoloPhase = 0.0, vibratoPhase = 0.0;

    // sine of a 0..1 phase via a refined parabola (error < 0.1%), much cheaper than std::sin
    static float fastSine (double phase) noexcept
    {
        auto x = (float) (phase < 0.5 ? phase : phase - 1.0) * 2.0f; // -1..1 (half cycle = 1)
        const auto y = 4.0f * x * (1.0f - std::abs (x));            // parabola
        return 0.225f * (y * std::abs (y) - y) + y;                  // refinement
    }

    // aliasing correction at discontinuities (polyBLEP)
    static float polyBlep (double t, double dt) noexcept
    {
        if (t < dt)
        {
            t /= dt;
            return (float) (t + t - t * t - 1.0);
        }

        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return (float) (t * t + t + t + 1.0);
        }

        return 0.0f;
    }

    float oscillator (Waveform w, double phase, double dt) noexcept
    {
        switch (w)
        {
            case Waveform::sine: return fastSine (phase);
            case Waveform::triangle: return (float) (phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase);
            case Waveform::saw: return (float) (2.0 * phase - 1.0) - polyBlep (phase, dt);
            case Waveform::square:
            {
                auto s = phase < 0.5 ? 1.0f : -1.0f;
                s += polyBlep (phase, dt);
                s -= polyBlep (phase < 0.5 ? phase + 0.5 : phase - 0.5, dt);
                return s;
            }
            case Waveform::noise: return noise.next();
            case Waveform::spectral:
            {
                // the table with harmonics up to 1024 >> j stays below Nyquist: j = lowest level that fits
                int level = 0;
                while (level < wavetableLevels - 1 && (double) (1024 >> level) * dt > 0.5)
                    ++level;
                const auto pos = phase * wavetableSize;
                const auto i = std::min ((int) pos, wavetableSize - 1);
                const auto f = (float) (pos - i);
                auto read = [&] (const Tables& tables) {
                    const auto& t = tables[(std::size_t) level];
                    return t[(std::size_t) i] + f * (t[(std::size_t) i + 1] - t[(std::size_t) i]);
                };
                if (evolving)
                {
                    const auto a = read (*frameA);
                    return a + frameMix * (read (*frameB) - a);
                }
                return read (wavetables);
            }
        }

        return 0.0f;
    }

    // band-limited wavetables of the spectrum (IFFT per level; level j holds harmonics ≤ 1024 >> j)
    static constexpr int wavetableSize = 2048, wavetableLevels = 10;
    using Tables = std::array<std::array<float, wavetableSize + 1>, wavetableLevels>;
    Tables wavetables {};

    // evolving spectrum: `count` snapshots at t_f = span·(f/(count − 1))², harmonic h scaled by 10^(−3t/decay_h)
    struct Frames
    {
        int count = 0;
        double span = 1.0;
        std::vector<Tables> tables;
    };
    static constexpr int frameCount = 8;
    std::atomic<Frames*> incomingFrames { nullptr }, retiredFrames { nullptr };
    Frames* activeFrames = nullptr;              // audio thread only
    bool evolving = false;
    const Tables* frameA = nullptr;
    const Tables* frameB = nullptr;
    float frameMix = 0.0f;
    // configure() side: what the last frames were built from
    std::array<float, SynthSettings::spectrumSize> framedSpectrum {}, framedPhase {}, framedDecay {}, framedTarget {};

    static void fillTables (Tables& tables, const std::array<float, SynthSettings::spectrumSize>& amplitude,
                            const std::array<float, SynthSettings::spectrumSize>& phase, std::vector<std::complex<double>>& bins)
    {
        for (int level = 0; level < wavetableLevels; ++level)
        {
            std::fill (bins.begin(), bins.end(), std::complex<double> {});
            const auto top = std::min (SynthSettings::spectrumSize, 1024 >> level);
            for (int h = 1; h <= top; ++h)
            {
                // a·sin(2π(h·t + φ)) = Im: coefficient −i·a/2·e^(i2πφ) at +h and its conjugate at −h
                const auto a = (double) amplitude[(std::size_t) h - 1] * (2.0 / 3.141592653589793); // 1/n saw = LYNX's ±1 saw
                const auto c = std::polar (a / 2.0, twoPi * phase[(std::size_t) h - 1] - twoPi / 4.0);
                bins[(std::size_t) h] = c;
                bins[(std::size_t) (wavetableSize - h)] = std::conj (c);
            }
            fftInPlace (bins, true);
            auto& t = tables[(std::size_t) level];
            for (int i = 0; i < wavetableSize; ++i)
                t[(std::size_t) i] = (float) bins[(std::size_t) i].real();
            t[(std::size_t) wavetableSize] = t[0];
        }
    }

    // called from configure() (off the audio thread): builds the frames when the evolving spectrum changes
    void updateFrames (const SynthSettings& s)
    {
        delete retiredFrames.exchange (nullptr);
        const auto spectral = std::any_of (s.osc.begin(), s.osc.end(), [] (const SynthOscillator& o) { return o.wave == Waveform::spectral; });
        const auto evolving = spectral && std::any_of (s.spectrumDecay.begin(), s.spectrumDecay.end(), [] (float t) { return t > 0.0f; });
        const auto wasEvolving = std::any_of (framedDecay.begin(), framedDecay.end(), [] (float t) { return t > 0.0f; });
        if (! evolving && ! wasEvolving)
            return;
        if (evolving && framedSpectrum == s.spectrum && framedPhase == s.spectrumPhase && framedDecay == s.spectrumDecay
            && framedTarget == s.spectrumTarget)
            return;
        framedTarget = s.spectrumTarget;
        framedSpectrum = s.spectrum;
        framedPhase = s.spectrumPhase;
        framedDecay = evolving ? s.spectrumDecay : std::array<float, SynthSettings::spectrumSize> {};

        auto* frames = new Frames; // count 0 = back to the static spectrum
        if (evolving)
        {
            frames->count = frameCount;
            frames->span = std::min (60.0, (double) *std::max_element (s.spectrumDecay.begin(), s.spectrumDecay.end()));
            frames->tables.resize ((std::size_t) frameCount);
            std::vector<std::complex<double>> bins ((std::size_t) wavetableSize);
            for (int f = 0; f < frameCount; ++f)
            {
                const auto t = frames->span * std::pow ((double) f / (frameCount - 1), 2.0);
                auto amplitude = s.spectrum;
                for (std::size_t h = 0; h < amplitude.size(); ++h)
                    if (s.spectrumDecay[h] > 0.0f)
                        amplitude[h] = s.spectrumTarget[h]
                                       + (amplitude[h] - s.spectrumTarget[h]) * (float) std::pow (10.0, -3.0 * t / s.spectrumDecay[h]);
                fillTables (frames->tables[(std::size_t) f], amplitude, s.spectrumPhase, bins);
            }
        }
        delete incomingFrames.exchange (frames);
    }
    std::array<float, SynthSettings::spectrumSize> builtSpectrum {}, builtPhase {};
    bool wavetablesBuilt = false;
    std::vector<std::complex<double>> fftBins = std::vector<std::complex<double>> ((std::size_t) wavetableSize);

    void buildWavetables()
    {
        if (wavetablesBuilt && builtSpectrum == settings.spectrum && builtPhase == settings.spectrumPhase)
            return;
        builtSpectrum = settings.spectrum;
        builtPhase = settings.spectrumPhase;
        wavetablesBuilt = true;
        fillTables (wavetables, settings.spectrum, settings.spectrumPhase, fftBins); // bins allocated with the instrument
    }


    void updateCoefficients() noexcept
    {
        if (std::any_of (settings.osc.begin(), settings.osc.end(), [] (const SynthOscillator& o) { return o.wave == Waveform::spectral; }))
            buildWavetables(); // only when the spectrum changes
        attackStep = (float) (1.0 / (std::max (0.0005, (double) settings.attack) * sampleRate));
        decayCoeff = decayCoefficient (std::max (0.001, (double) settings.decay), sampleRate);
        releaseCoeff = decayCoefficient (std::max (0.005, (double) settings.release), sampleRate);

        for (std::size_t o = 0; o < 3; ++o)
        {
            const auto& osc = settings.osc[o];
            ratios[o] = std::pow (2.0, osc.octave + (osc.semitones * 100.0 + osc.detune) / 1200.0);
            const auto p = std::clamp (osc.pan, -1.0f, 1.0f);
            oscPanL[o] = p > 0.0f ? 1.0f - p : 1.0f;
            oscPanR[o] = p < 0.0f ? 1.0f + p : 1.0f;
        }

        stereoOscillators = std::any_of (settings.osc.begin(), settings.osc.end(), [] (const SynthOscillator& o) { return o.pan != 0.0f; });
        osc3Modulates = settings.am || settings.fm > 0.0f;
        fmDecayCoeff = settings.fmDecay > 0.0f ? decayCoefficient (std::max (0.001, (double) settings.fmDecay), sampleRate) : 1.0f;

        sharedFilter.set (settings.cutoff, settings.resonance, sampleRate, settings.filterType, settings.ladder);
        modEnv[0] = ModCoefficients::make (settings.filterEnv, sampleRate);
        modEnv[1] = ModCoefficients::make (settings.pitchEnv, sampleRate);
        auto anyLfo = false;

        for (std::size_t i = 0; i < lfoTargets; ++i)
        {
            lfos[i] = LfoCoefficients::make (settings.lfo[i], sampleRate, bpm.load());
            anyLfo = anyLfo || lfos[i].active;
        }

        filterModulated = modEnv[0].active || lfos[lfoCutoff].active || settings.keyTrack != 0.0f;
        modulated = modEnv[0].active || modEnv[1].active || anyLfo || settings.keyTrack != 0.0f || settings.sweep > 0.0f
                    || settings.vibratoCents > 0.0f;
        sweepCoeff = std::exp (-16.0 / (std::max (0.0005, (double) settings.sweepTime) * sampleRate)); // per 16-sample tick
        useBands = settings.bandGain != std::array<float, 3> { 1.0f, 1.0f, 1.0f };
        toneCoeff[0] = (float) std::min (1.0, 0.025 * 44100.0 / sampleRate); // ≈ 178 Hz (REA_rodada18 §11)
        toneCoeff[1] = (float) std::min (1.0, 0.155 * 44100.0 / sampleRate); // ≈ 1.18 kHz
    }

    double sampleRate = 44100.0;
    SettingsMailbox<SynthSettings> mailbox;
    SynthSettings settings;
    float attackStep = 0.01f;
    float decayCoeff = 0.999f;
    float releaseCoeff = 0.999f;
    SvfCoefficients sharedFilter; // without a cutoff envelope, all voices use this one
    std::array<ModCoefficients, 2> modEnv {};
    std::array<LfoCoefficients, lfoTargets> lfos {};
    bool modulated = false, filterModulated = false;
    std::array<double, 3> ratios { 1.0, 1.0, 1.0 };
    std::atomic<double> bpm { 120.0 };
    double lastPhaseDelta = 0.0; // pitch of the last note (where porta starts from)
    double songTicks = 0.0;      // song position in the block (global LFO)
    std::array<float, 3> oscPanL { 1.0f, 1.0f, 1.0f }, oscPanR { 1.0f, 1.0f, 1.0f };
    bool stereoOscillators = false;
    bool osc3Modulates = false; // AM or FM: osc 3 is not summed
    float fmDecayCoeff = 1.0f;
    double sweepCoeff = 1.0;
    bool useBands = false;
    float toneCoeff[2] { 0.025f, 0.155f };
    float nextPan = 0.0f, nextCents = 0.0f;
    double bendMul = 1.0; // the pitch wheel (round 162), audio thread
    bool nextPorta = false;
    Noise noise;
    std::array<SynthVoice, 16> voices {};
};

// ---------------------------------------------------------------------------
// Sampler: plays the whole sample on each note (one-shot, like an FL step),
// transposed by the key (60 = original pitch), with FL's sampler interpolation (core/FlResampler.h: 24-point sinc while
// playing, 32-point when exporting, FL's defaults; REA_rodada69 §1).

struct SamplerVoice
{
    bool active = false;
    double position = 0.0; // read position in region order (reversed voices read the region backwards)
    double rate = 1.0;     // frames per output sample (> 0)
    bool reversed = false;
    int loops = 0;         // times the zone loop has wrapped (FL lets taps before the loop start wrap only then)
    flrs::Kernel kernel;
    bool flEnvelope = false; // FL's volume articulator drives the level (the ADSR stays at 1)
    flvp::Art volumeArt;
    double tickLeft = 0.0;   // samples until the next sequencer tick
    float flLevel = 0.0f, flStep = 0.0f; // the level ramps linearly to each tick's value over the tick (FL)
    float velocity = 0.0f;
    float fade = 1.0f;     // < 1 when the voice was stolen (declick)
    bool stolen = false;
    int age = 0;
    int key = 60;
    float env = 1.0f;      // envelope
    float envStep = 0.0f;  // attack step (> 0) or release step (< 0)
    int stage = 1;         // 0 attack, 1 decay, 2 sustain, 3 release
    double first = 0.0, last = 0.0; // played region: first and last frame (inclusive)
    float clipElapsed = 0.0f, clipRemaining = 0.0f, fadeIn = 0.0f, fadeOut = 0.0f; // clip fades
    float panL = 1.0f, panR = 1.0f;
    double glideRatio = 1.0;
    int glideLeft = 0;
    float lowL = 0.0f, bandL = 0.0f, lowR = 0.0f, bandR = 0.0f; // channel filter
    std::array<ModState, 2> mod {}; // cutoff and pitch envelopes
    std::array<LfoState, lfoTargets> lfo {};
    float lfoGain = 1.0f, lfoGainStep = 0.0f;
    float basePan = 0.0f;
    float cents = 0.0f; // the note's fine pitch (a VFX may move it while it sounds)
    const SampleData* data = nullptr; // this voice's sample (the chosen zone, in multi-sample)
    float zoneGain = 1.0f;
    double loopStart = 0.0, loopEnd = 0.0; // loopEnd > loopStart: the voice jumps back to the loop start
    bool loopWhileHeld = false;
    SvfCoefficients filter;         // this voice's (only with a cutoff envelope)
    double pitchMul = 1.0;
    int controlCountdown = 0;
    // FL's native sampler voice (REA_rodada76): the channel's articulators and filter (flvp), the note's initial
    // volume and pan, the stereo gains ramping between ticks, the fast LP's running coefficient, the song tick index
    bool flVoice = false;
    flvp::Proc flProc;
    float initVol = 1.0f, notePan = 0.0f;
    float gL = 0.0f, gR = 0.0f, sL = 0.0f, sR = 0.0f;
    float fcRun = 0.0f, fcSlope = 0.0f;
    std::int64_t tickIndex = 0;
    double nextGap = 0.0;             // samples from this tick to the next one on the song's grid
    int endFade = -1;                 // ≥ 0: past the sample's end, the last frame fading out (FL's 10 ms end fade)
    int offFade = -1;                 // ≥ 0: a looping voice fading out after its note-off (the same table)
    float holdL = 0.0f, holdR = 0.0f; // the last frame read
};

// FL's note velocity → initial volume (FLEngine FUN_00d29aa0, REA_rodada76 §1): x = byte·0.01; 1 at x = 1, else
// (exp(x·ln 11) − 1)·0.1 — velocity 100 is unity, 128 ≈ 2.05
inline float flInitVolume (float velocity) noexcept
{
    static const float ln11 = (float) std::log (11.0);
    const auto x = (float) std::nearbyint (std::clamp (velocity, 0.0f, 1.0f) * 128.0f) * 0.01f;
    if (x == 1.0f)
        return 1.0f;
    return (float) ((std::exp ((double) (x * ln11)) - 1.0) * (double) 0.1f);
}

class SamplerInstrument final : public Instrument
{
public:
    // clipMode: Playlist audio clip (no transposition, stops on noteOff; it follows Reverse and the stretch)
    SamplerInstrument (std::shared_ptr<const SampleData> s, bool clip) : sample (std::move (s)), clipMode (clip) {}

    // multi-sample: called before the instrument goes to the audio thread
    void setZones (std::vector<LoadedZone> z)
    {
        zones = std::move (z);
        zones.erase (std::remove_if (zones.begin(), zones.end(), [] (const LoadedZone& lz) { return lz.sample == nullptr || lz.sample->frames() < 2; }),
                     zones.end());

        if (sample == nullptr && ! zones.empty())
            sample = zones.front().sample;
    }

    // the zone for the key/velocity; several covering the same note take turns like FPC's "cycle" layers (REA_rodada69
    // §4: among the zones covering the note, in order, the one after the last chosen, wrapping); if none covers it, the
    // one with the nearest root key. The velocity index is FL's round(velocity · 128), at most 127.
    const LoadedZone* zoneFor (int key, float velocity) noexcept
    {
        if (zones.empty())
            return nullptr;

        const auto vel = std::min (127, (int) std::nearbyint (std::clamp (velocity, 0.0f, 1.0f) * 128.0f));
        const LoadedZone* nearest = nullptr;
        int matches[16] {};
        int matchCount = 0, lastAt = -1;

        for (int i = 0; i < (int) zones.size(); ++i)
        {
            const auto& z = zones[(std::size_t) i];
            if (key >= z.zone.lowKey && key <= z.zone.highKey && vel >= z.zone.lowVelocity && vel <= z.zone.highVelocity && matchCount < 16)
            {
                if (i == lastZone)
                    lastAt = matchCount;
                matches[matchCount++] = i;
            }

            if (nearest == nullptr || std::abs (z.zone.rootKey - key) < std::abs (nearest->zone.rootKey - key))
                nearest = &z;
        }

        if (matchCount == 0)
            return nearest;

        const auto pick = matchCount == 1 || lastAt + 1 >= matchCount ? 0 : lastAt + 1;
        lastZone = matches[pick];
        return &zones[(std::size_t) lastZone];
    }

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        quality = offlineRendering() ? flrs::renderQuality : flrs::liveQuality;
        fadeStep = (float) (1.0 / (0.003 * sr)); // 3 ms
        updateModulation();
        allNotesOff();
    }

    void configure (const ChannelDef& channel, double songBpm) noexcept override
    {
        const auto& s = channel.sampler;
        pitch.store (std::clamp (s.pitch, -24.0f, 24.0f));
        // the knobs an automation clip drives (issue #12, setParameter) keep the automated value until the song's own
        // value changes (the knob turned by hand)
        auto knob = [this] (int k, float natural) {
            if (! (configuredKnobs[(std::size_t) k] == natural))
            {
                configuredKnobs[(std::size_t) k] = natural;
                automatedKnob[(std::size_t) k].store (false, std::memory_order_relaxed);
            }
            if (! automatedKnob[(std::size_t) k].load (std::memory_order_relaxed))
                storeKnob (k, natural);
        };
        knob (samplerparam::attack, s.attack);
        knob (samplerparam::decay, s.decay);
        knob (samplerparam::sustain, s.sustain);
        knob (samplerparam::release, s.release);
        reverse.store (s.reverse);
        cut.store (s.cut);
        // with "keep pitch" the sample arrives already stretched (RackState): play at normal speed; the keys then
        // transpose it through each voice's pitch shifter (round 162), so its length stays
        stretchBeats.store (s.keepPitch ? 0.0f : std::max (0.0f, s.stretchBeats));
        // keep length (round 163): with "keep pitch" a transposition never changes the length — the keys of a sampler, the
        // pitch knob and the fine tune of a sampler or an audio clip go through each voice's pitch shifter, stretched to
        // the tempo or not
        keyShift.store (s.keepPitch);
        keyShiftMethod.store ((int) s.stretchMethod);
        knob (samplerparam::start, s.regionStart);
        knob (samplerparam::end, s.regionEnd);
        porta.store (s.porta);
        portaBeats.store (std::max (0.0f, s.portaBeats));
        knob (samplerparam::cutoff, s.cutoff);
        knob (samplerparam::resonance, s.resonance);
        filterType.store (std::clamp (s.filterType, 0, 3));
        outGain.store (samplerGain (s.gainDb));
        bpm.store (std::max (1.0, songBpm)); // before the mailbox: the synced LFO reads it when updating
        {
            Modulation m { { s.filterEnv, s.pitchEnv }, s.lfo };
            if (s.flChannel.size() == 89 || s.flChannel.size() == 90)
            {
                // FL's channel articulators and filter in the plugins' 364-byte snapshot layout (REA_rodada76 §4)
                std::array<std::uint8_t, flvp::snapshotSize> snap {};
                std::memcpy (snap.data(), s.flChannel.data(), 85 * 4);
                std::memcpy (snap.data() + 0x15c, &s.flChannel[85], 4);
                std::memcpy (snap.data() + 0x160, &s.flChannel[86], 4);
                std::memcpy (snap.data() + 0x168, &s.flChannel[87], 4);
                const auto ppq = std::clamp ((int) s.flChannel[88], 24, 3840);
                m.flNative = true;
                m.flTriangular = s.flChannel.size() == 90 && s.flChannel[89] == 2;
                m.fl.build (snap.data(), ppq, std::max (1.0, songBpm));
                m.ticksPerSecond = (double) ppq * std::max (1.0, songBpm) / 60.0;
            }
            else if (s.flVolumeEnvelope.size() == 18)
            {
                const auto ppq = std::clamp ((int) s.flVolumeEnvelope[17], 24, 3840);
                m.flVolume = true;
                m.volume.build (s.flVolumeEnvelope.data(), ppq, std::max (1.0, songBpm));
                m.ticksPerSecond = (double) ppq * std::max (1.0, songBpm) / 60.0;
            }
            envelopeMailbox.write (m);
        }
        SliceTable table;
        table.count = (int) std::min<std::size_t> (s.slices.size(), table.starts.size());
        for (int i = 0; i < table.count; ++i)
            table.starts[(std::size_t) i] = std::clamp (s.slices[(std::size_t) i], 0.0f, 1.0f);
        sliceMailbox.write (table);
        bpm.store (std::max (1.0, songBpm));
    }

    // Sample frames read per output sample (before note tuning). With
    // stretch, the whole sample lasts `stretchBeats` at the current tempo (resampling, like FL's
    // "resample" mode: the pitch follows).
    double baseRate (const SampleData& data) const noexcept
    {
        const auto beats = stretchBeats.load();

        if (beats > 0.0f)
            return (double) data.frames() / ((double) beats * 60.0 / bpm.load() * sampleRate);

        return data.sampleRate * (1.0 / sampleRate);
    }

    void noteOn (int key, float velocity) noexcept override { noteOnAt (key, velocity, 0.0); }

    void noteOnAt (int key, float velocity, double startSeconds) noexcept override
    {
        const auto noteGain = std::exchange (nextGain, 1.0f);
        const auto* zone = clipMode ? nullptr : zoneFor (key, velocity);
        const auto* data = zone != nullptr ? zone->sample.get() : sample.get();

        if (envelopeMailbox.read (modulation))
            updateModulation();

        if (data == nullptr || data->frames() < 2)
            return;

        sliceMailbox.read (slices);
        const auto backwards = reverse.load(); // audio clips too (the clip menu's Reverse)
        const auto total = (double) (data->frames() - 1);
        // sample region (the start acts as "start offset"); audio clips use the whole file. `last` is the region's
        // last frame: FL plays until the read position passes it, interpolating with that frame repeated.
        auto first = clipMode ? 0.0 : std::floor (total * std::min (regionStart.load(), regionEnd.load()));
        auto last = clipMode ? total : std::max (first + 1.0, std::floor (total * regionEnd.load()));
        auto sliced = false;

        // sliced sample: each key from C5 up plays one slice, at the original pitch
        if (! clipMode && slices.count > 0)
        {
            const auto index = key - 60;

            if (index < 0 || index >= slices.count)
                return;

            first = std::floor (total * slices.starts[(std::size_t) index]);
            last = index + 1 < slices.count ? std::max (first, std::floor (total * slices.starts[(std::size_t) index + 1]) - 1.0) : total;
            sliced = true;
        }
        const auto base = baseRate (*data);
        auto startFrame = first + std::max (0.0, startSeconds * sampleRate * base);
        // a resampled (pitched) clip reads its file faster or slower: a start inside it moves with that speed (the
        // Playlist shows the file at that speed too, clipops::framesPerTick)
        if (clipMode && ! keyShift.load (std::memory_order_relaxed))
            if (const auto cents = (double) pitch.load() * 100.0 + nextCents; cents != 0.0)
                startFrame = first + std::max (0.0, startSeconds * sampleRate * flrs::speedFor ((float) cents, base));
        if (clipMode) // FL starts an audio clip on a whole source frame (round 110 §1: ceil or round fit, floor is 1 late)
            startFrame = std::round (startFrame);

        if (startFrame >= last + 1.0)
            return;

        if (! clipMode && cut.load())
            for (auto& v : voices)
                if (v.active)
                    v.stolen = true; // "cut itself": the previous note fades out quickly

        SamplerVoice* target = nullptr;

        for (auto& v : voices)
            if (! v.active)
            {
                target = &v;
                break;
            }

        if (target == nullptr)
        {
            // No free voice: steal the oldest.
            target = &voices[0];
            for (auto& v : voices)
                if (v.age > target->age)
                    target = &v;
        }

        // with stretch in FL's Resample mode the note still transposes: pitch and length change together, like tape
        // (FL's manual: "playing a chord plays each note at a different speed"; round 162, issue #9 — LYNX played every
        // key at the sample's own pitch and ignored the pitch knob and the fine tune)
        const auto root = zone != nullptr ? zone->zone.rootKey : 60;
        const auto tune = zone != nullptr ? (double) zone->zone.tune : 0.0;
        // an audio clip has no key: its channel's pitch and fine tune transpose it (round 163 — the Clip properties' and
        // Channel settings' pitch did nothing to clips); Resample changes its speed, "keep pitch" its pitch only
        const auto semitones = clipMode ? (double) pitch.load() + nextCents / 100.0
                                        : (sliced ? 0 : key - root) + tune + (double) pitch.load() + nextCents / 100.0;
        const auto attackSeconds = clipMode ? 0.0f : attack.load();

        *target = {};
        target->active = true;
        target->key = key;
        // FL plays a Playlist audio clip at unity gain at the channel's default volume (78%): the channel volume
        // curve is relative to that point for clips (REA_rodada89 §4.2 [EXEC]: +5.2 dB at every volume and pan)
        target->velocity = clipMode ? velocity * audioClipUnity() : velocity;
        target->data = data;
        target->zoneGain = (zone != nullptr ? zone->zone.gain : 1.0f) * noteGain;

        // zone loop (DirectWave): only when playing forward and with the loop inside the region
        if (zone != nullptr && zone->zone.loopMode != 0 && ! backwards && zone->zone.loopEnd > zone->zone.loopStart + 16
            && (double) zone->zone.loopEnd <= last && (double) zone->zone.loopStart >= first)
        {
            target->loopStart = (double) zone->zone.loopStart;
            target->loopEnd = (double) zone->zone.loopEnd;
            target->loopWhileHeld = zone->zone.loopMode == 2;
        }
        target->position = startFrame;
        target->first = first;
        target->last = last;
        target->clipElapsed = nextClip[0];
        target->clipRemaining = nextClip[1];
        target->fadeIn = nextClip[2];
        target->fadeOut = nextClip[3];
        nextClip = {};
        // FL's read speed: the rate ratio at the original pitch, else exp(cents·ln2/1200)·ratio
        target->rate = flrs::speedFor ((float) (semitones * 100.0), base);
        target->reversed = backwards;
        // the render quality is read at each note (round 160): the export engine prepares its instruments before it
        // switches to offline rendering, so a value taken in prepare() kept the playback quality in most exports
        target->kernel.setQuality (offlineRendering() ? flrs::renderQuality : quality);

        // portamento (channel porta, inverted by the note): starts from the previous note's pitch
        if (! clipMode && (porta.load() != nextPorta) && lastRate != 0.0)
        {
            const auto steps = (int) (portaBeats.load() * 60.0 / bpm.load() * sampleRate);
            const auto from = std::abs (lastRate);

            if (steps >= 1 && from != 0.0)
            {
                target->glideRatio = std::pow (target->rate / from, 1.0 / steps);
                target->glideLeft = steps;
                target->rate = from;
            }
        }

        if (! clipMode)
            lastRate = target->glideLeft > 0 ? target->rate * std::pow (target->glideRatio, target->glideLeft) : target->rate;
        target->env = attackSeconds > 0.0f ? 0.0f : 1.0f;
        target->envStep = attackSeconds > 0.0f ? (float) (1.0 / (attackSeconds * sampleRate)) : 0.0f;
        target->stage = attackSeconds > 0.0f ? 0 : 1;

        // FL's volume articulator: starts at its initial level and is stepped on the voice's first block (FLEngine
        // FUN_00d24ae0 steps a new voice at once, then once per sequencer tick)
        target->flEnvelope = ! clipMode && modulation.flVolume && modulation.ticksPerSecond > 0.0;
        if (target->flEnvelope)
        {
            target->env = 1.0f;
            target->envStep = 0.0f;
            target->stage = 1;
            target->volumeArt.init (modulation.volume);
            target->flLevel = target->volumeArt.val;
            target->flStep = 0.0f;
            target->tickLeft = 0.0;
        }
        // FL's native sampler voice (REA_rodada76): the articulators start, the first render block steps them at once,
        // the following steps fall on the song's tick grid (the tick this note starts on, then round(k · spt))
        target->flVoice = ! clipMode && modulation.flNative && modulation.ticksPerSecond > 0.0;
        if (target->flVoice)
        {
            target->flEnvelope = false;
            target->env = 1.0f;
            target->envStep = 0.0f;
            target->stage = 1;
            target->flProc.init (modulation.fl);
            target->initVol = flInitVolume (velocity);
            target->notePan = nextPan;
            const auto spt = sampleRate / modulation.ticksPerSecond;
            target->tickIndex = (std::int64_t) std::nearbyint (songSample / spt);
            target->tickLeft = 0.0;
            target->nextGap = std::max (1.0, (double) flvp::roundEven ((double) (target->tickIndex + 1) * spt) - songSample);
            // the file's own loop (WAV smpl): FL plays it while the note lasts (REA_rodada76 §8)
            if (zone == nullptr && ! backwards && data->loopStart >= 0 && data->loopEnd > data->loopStart + 1
                && (double) data->loopStart >= first && (double) data->loopEnd <= last + 1.0)
            {
                target->loopStart = (double) data->loopStart;
                target->loopEnd = (double) data->loopEnd;
                target->loopWhileHeld = false;
            }
        }
        voicePan (clipMode ? 0.0f : nextPan, target->panL, target->panR);
        target->basePan = clipMode ? 0.0f : nextPan;
        target->cents = clipMode ? 0.0f : nextCents;

        for (std::size_t i = 0; i < lfoTargets; ++i)
            if (lfos[i].active && ! clipMode)
                lfos[i].start (target->lfo[i], songTicks);
        startKeyShift (*target, semitones, base);
        nextPan = nextCents = 0.0f;
        nextPorta = false;
    }

    // stretch keeping pitch (round 162): each voice's pitch shifter and its source cursor
    static constexpr int keyShiftBlock = 128;
    static constexpr int keyShiftVoices = 8; // shifters per channel (more transposed voices at once play resampled)
    struct KeyShift
    {
        VoicePitchShifter* shifter = nullptr;       // from the pool (prepareKeyShift), while the voice is shifted
        bool on = false;
        bool primed = false;                        // primed in render (a few per block: it costs ~1 ms each)
        SamplerVoice ahead;                         // reads the source primeLength() frames ahead of the voice
        double factor = 1.0, glideRatio = 1.0;      // the note's transposition and its portamento / slide
        int glideLeft = 0;
        std::array<float, keyShiftBlock * 4 + 2> inL {}, inR {}; // the source read `factor` times faster
        std::array<float, keyShiftBlock> outL {}, outR {};
        double owed = 0.0; // input frames owed to the shifter (n / factor per block, the fraction carried)
        int pos = 0, count = 0;
    };
    // Stretch keeping pitch (round 162, issue #9): a transposed note reads the stretched sample at its own speed, so it
    // lasts as long as at the root key, and its pitch shifter transposes it (FL's Stretch modes: "pitch changes keep
    // the original duration", the voices of a chord stay in sync). The root key plays the stretched sample untouched.
    void startKeyShift (SamplerVoice& v, double semitones, double base) noexcept
    {
        const auto index = (std::size_t) (&v - voices.data());
        auto& ks = shifts[index];
        ks.on = false;
        ks.shifter = nullptr;
        if (! keyShift.load (std::memory_order_relaxed) || std::abs (semitones) < 1.0e-6)
        {
            if (keyShift.load (std::memory_order_relaxed))
                lastFactor = 1.0;
            return;
        }
        // a free shifter: none, or its voice ended or plays unshifted
        for (std::size_t k = 0; k < shifterPool.size() && ks.shifter == nullptr; ++k)
        {
            if (shifterPool[k] == nullptr || ! shifterPool[k]->ready() || primeL.size() < (std::size_t) shifterPool[k]->maxPrimeLength())
                continue;
            const auto user = shifterUser[k];
            if (user < 0 || (std::size_t) user == index || ! voices[(std::size_t) user].active || shifts[(std::size_t) user].shifter != shifterPool[k].get())
            {
                shifterUser[k] = (int) index;
                ks.shifter = shifterPool[k].get();
            }
        }
        if (ks.shifter == nullptr)
            return; // every shifter busy: this voice plays resampled

        ks.on = true;
        ks.factor = std::exp2 (semitones / 12.0);
        ks.glideLeft = 0;
        // portamento: the transposition glides from the previous note's (its length stays)
        if (v.glideLeft > 0 && lastFactor > 0.0)
        {
            ks.glideRatio = std::pow (ks.factor / lastFactor, 1.0 / v.glideLeft);
            ks.glideLeft = v.glideLeft;
            ks.factor = lastFactor;
        }
        lastFactor = std::exp2 (semitones / 12.0);
        v.rate = base;
        v.glideLeft = 0;
        lastRate = base;

        ks.ahead = v;
        ks.ahead.pitchMul = 1.0;
        ks.primed = false;
        ks.pos = ks.count = 0;
        ks.owed = 0.0;
    }

    // the transposition the shifter applies now: the note's (gliding), the pitch envelope / LFO and the pitch wheel
    double shiftFactor (const SamplerVoice& v, const KeyShift& ks) const noexcept
    {
        return std::clamp (ks.factor * v.pitchMul * bendMul, VoicePitchShifter::minFactor, VoicePitchShifter::maxFactor);
    }

    // Primes a new shifted voice's shifter with the start of its source (so its output has no latency). A render
    // primes at most two (about 1 ms each); a voice waiting for its turn starts one block later. Exports prime all.
    bool primeKeyShift (const SamplerVoice& v, KeyShift& ks, int& budget) noexcept
    {
        if (ks.primed)
            return true;
        if (budget <= 0)
            return false;
        --budget;
        const auto factor = shiftFactor (v, ks);
        const auto prime = std::min (ks.shifter->primeLength (factor), (int) primeL.size());
        for (int i = 0; i < prime; ++i)
            readAhead (v, ks, factor, primeL[(std::size_t) i], primeR[(std::size_t) i]);
        ks.shifter->start (primeL.data(), primeR.data(), prime);
        ks.primed = true;
        return true;
    }

    // the shifter's source: the stretched sample read at the voice's (untransposed) speed, ahead of the voice
    void readAhead (const SamplerVoice& v, KeyShift& ks, double factor, float& outL, float& outR) noexcept
    {
        auto& a = ks.ahead;
        const auto lastFrame = std::min ((double) (a.data->frames() - 1), a.last);
        const auto looping = a.loopEnd > a.loopStart && (! a.loopWhileHeld || v.stage < 3);
        if (looping && a.position >= a.loopEnd)
        {
            while (a.position >= a.loopEnd)
                a.position = (a.position - a.loopEnd) + a.loopStart;
            ++a.loops;
        }
        if ((double) flrs::wholeFrames (a.position) > lastFrame || a.position < a.first)
            outL = outR = 0.0f;
        else
            readFrame (a, a.data->leftData(), a.data->rightData(), lastFrame, looping, outL, outR, a.rate * factor);
        a.position += a.rate * factor;
    }

    // the next frame of a shifted voice (blocks of keyShiftBlock through its shifter): the transposition, its glide,
    // the pitch envelope / LFO (pitchMul) and the pitch wheel change the shift, never the speed
    void readShifted (const SamplerVoice& v, KeyShift& ks, float& outL, float& outR) noexcept
    {
        if (ks.pos >= ks.count)
        {
            if (ks.glideLeft > 0)
            {
                const auto k = std::min (keyShiftBlock, ks.glideLeft);
                ks.factor *= std::pow (ks.glideRatio, (double) k);
                ks.glideLeft -= k;
            }
            const auto factor = shiftFactor (v, ks);
            ks.owed += keyShiftBlock / factor;
            const auto frames = std::min ((int) ks.inL.size(), (int) ks.owed);
            ks.owed -= frames;
            for (int i = 0; i < frames; ++i)
                readAhead (v, ks, factor, ks.inL[(std::size_t) i], ks.inR[(std::size_t) i]);
            ks.shifter->process (ks.inL.data(), ks.inR.data(), frames, ks.outL.data(), ks.outR.data(), keyShiftBlock);
            ks.pos = 0;
            ks.count = keyShiftBlock;
        }
        outL = ks.outL[(std::size_t) ks.pos];
        outR = ks.outR[(std::size_t) ks.pos];
        ++ks.pos;
    }

    void prepareKeyShift (double sr) override
    {
        if (! keyShift.load())
            return;
        const auto method = (StretchMethod) keyShiftMethod.load();
        for (auto& shifter : shifterPool)
        {
            shifter = std::make_unique<VoicePitchShifter>();
            shifter->allocate (sr, method);
        }
        shifterUser.fill (-1);
        const auto prime = (std::size_t) shifterPool[0]->maxPrimeLength();
        primeL.assign (prime, 0.0f);
        primeR.assign (prime, 0.0f);
    }

    void pitchBend (float semitones) noexcept override
    {
        bendMul = clipMode || ! std::isfinite (semitones) ? 1.0 : std::exp2 ((double) std::clamp (semitones, -48.0f, 48.0f) / 12.0);
    }

    // a VFX changes a sounding note (round 164b): its fine pitch, pan or volume
    void voiceProperty (int key, int prop, float value) noexcept override
    {
        if (! std::isfinite (value))
            return;
        for (auto& v : voices)
            if (v.active && v.stage != 3 && v.key == key)
            {
                if (prop == (int) vfx::Prop::pitch)
                {
                    const auto ratio = std::exp2 ((double) (value - v.cents) / 1200.0);
                    v.rate *= ratio;
                    v.cents = value;
                }
                else if (prop == (int) vfx::Prop::pan)
                {
                    v.basePan = v.notePan = std::clamp (value, -1.0f, 1.0f);
                    voicePan (v.basePan, v.panL, v.panR);
                }
                else if (prop == (int) vfx::Prop::velocity)
                    v.velocity = std::clamp (value, 0.0f, 1.0f);
            }
    }

    void setNextNoteExpression (float pan, float cents, bool porta) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
        nextPorta = porta;
    }

    void setNextClipFades (float elapsed, float remaining, float in, float out) noexcept override
    {
        nextClip = { elapsed, remaining, in, out };
    }

    bool setNextNoteGain (float gain) noexcept override
    {
        nextGain = gain;
        return true;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        if (clipMode)
            return;

        const auto semitones = (key - 60) + (double) pitch.load() + cents / 100.0;
        const auto steps = std::max (1, (int) samples);

        for (auto& v : voices)
            if (v.active && ! v.stolen && v.rate != 0.0 && v.data != nullptr)
            {
                // a shifted voice (stretch keeping pitch) glides its transposition, not its speed (round 162)
                if (auto& ks = shifts[(std::size_t) (&v - voices.data())]; ks.on)
                {
                    ks.glideRatio = std::pow (std::exp2 (semitones / 12.0) / ks.factor, 1.0 / steps);
                    ks.glideLeft = steps;
                    continue;
                }
                // (with stretch in Resample mode the speed is relative to the stretched one)
                const auto target = flrs::speedFor ((float) (semitones * 100.0), baseRate (*v.data));
                v.glideRatio = std::pow (target / v.rate, 1.0 / steps);
                v.glideLeft = steps;
            }
    }

    void noteOff (int key) noexcept override
    {
        if (clipMode)
        {
            // the clip ended: its voice fades out quickly. A clip carries its own key (audioClipKey), so the next clip
            // of the same channel that already started (a crossfade) keeps playing; other callers stop everything.
            if (key < audioClipKeyBase)
                allNotesOff();
            else
                for (auto& v : voices)
                    if (v.active && v.key == key)
                        v.stolen = true;
            return;
        }

        // the cutoff/pitch envelopes always release
        for (auto& v : voices)
            if (v.active && v.key == key && v.stage < 3)
                for (std::size_t e = 0; e < 2; ++e)
                    modEnv[e].release (v.mod[e]);

        // FL's voice: the volume, cutoff, resonance and pitch articulators release (FLEngine FUN_00d241e0); without a
        // volume envelope the note-off changes nothing — the sample plays on and the other envelopes hold (EXEC: FL's
        // renders, REA_rodada76 §6)
        for (auto& v : voices)
            if (v.active && v.key == key && v.flVoice && v.stage < 3)
            {
                if (modulation.fl.recs[1].enabled != 0)
                    for (int k = 1; k < 5; ++k)
                        v.flProc.arts[(std::size_t) k].release (modulation.fl.recs[(std::size_t) k]);
                else if (v.loopEnd > v.loopStart && v.offFade < 0)
                    v.offFade = 0; // a looping sample without volume envelope fades out in 10 ms (EXEC, §8)
                v.stage = 3;
            }

        // FL's volume articulator enters its release stage from the current level
        for (auto& v : voices)
            if (v.active && v.key == key && v.flEnvelope && v.stage < 3)
            {
                v.volumeArt.release (modulation.volume);
                v.stage = 3; // released (stops "while held" loops); the ADSR level stays at 1
            }

        // with release, the released note decays; without release the sample plays in full (one-shot)
        const auto seconds = release.load();

        if (seconds <= 0.0f)
            return;

        // linear release from the current level (like FL)
        for (auto& v : voices)
            if (v.active && v.key == key && v.stage < 3 && ! v.flEnvelope && ! v.flVoice)
            {
                v.stage = 3;
                v.envStep = -(float) (std::max (v.env, 0.001f) / (seconds * sampleRate));
            }
    }

    void allNotesOff() noexcept override
    {
        for (auto& v : voices)
            if (v.active)
                v.stolen = true; // fade out instead of a hard cut
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        const struct Advance
        {
            double& at;
            int n;
            ~Advance() { at += n; }
        } advance { songSample, n };

        if (sample == nullptr && zones.empty())
            return;

        if (envelopeMailbox.read (modulation))
            updateModulation();

        // FL's native sampler voices: the channel's volume and pan act inside each voice (REA_rodada76 §3), so they are
        // recovered from the rack's gains (panGains: gain·√2·cos/sin of (1 + pan)·π/4)
        if (modulation.flNative)
        {
            const auto chanVol = (float) std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
            auto chanPan = chanVol > 0.0f ? (float) (std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0) : 0.0f;
            if (std::fabs (chanPan) < 1.0e-6f)
                chanPan = 0.0f;
            const flvp::X87Extended precision;
            auto primeBudget = offlineRendering() ? keyShiftVoices : 2;
            for (auto& v : voices)
                if (v.active && v.data != nullptr && v.flVoice)
                    if (auto& ks = shifts[(std::size_t) (&v - voices.data())]; ! ks.on || primeKeyShift (v, ks, primeBudget))
                        renderFl (v, left, right, n, chanVol, chanPan);
        }

        // the Gain knob scales the other voices with the channel's gains (FL voices apply it in renderFl)
        {
            const auto g = outGain.load (std::memory_order_relaxed);
            gl *= g;
            gr *= g;
        }

        // channel filter (TPT SVF, same as the synth); off when open, without resonance and without modulation
        const auto fc = cutoff.load(), res = resonance.load();
        const auto type = filterType.load();
        const auto filterModulated = modEnv[0].active || lfos[lfoCutoff].active;
        const auto filtered = fc < 19500.0f || res > 0.0f || type != 0 || filterModulated;
        const auto modulated = modEnv[0].active || modEnv[1].active || anyLfo;

        if (filtered && (fc != filterCutoff || res != filterResonance || type != sharedFilter.mode))
        {
            filterCutoff = fc;
            filterResonance = res;
            sharedFilter.set (fc, res, sampleRate, type);
        }

        const flvp::X87Extended precision; // FL's interpolators run the x87 unit at 64-bit precision

        auto primeBudget = offlineRendering() ? keyShiftVoices : 2;
        for (auto& v : voices)
        {
            if (! v.active || v.data == nullptr || v.flVoice)
                continue;
            if (auto& ks = shifts[(std::size_t) (&v - voices.data())]; ks.on && ! primeKeyShift (v, ks, primeBudget))
                continue; // (its shifter primes in the next block)

            const auto* l = v.data->leftData();
            const auto* r = v.data->rightData();
            const auto lastFrame = std::min ((double) (v.data->frames() - 1), v.last);

            for (int i = 0; i < n; ++i)
            {
                // loop: jump back to its start (the "while held" mode stops looping on release)
                const auto looping = v.loopEnd > v.loopStart && (! v.loopWhileHeld || v.stage < 3);
                if (looping && v.position >= v.loopEnd)
                {
                    while (v.position >= v.loopEnd)
                        v.position = (v.position - v.loopEnd) + v.loopStart;
                    ++v.loops;
                }

                if ((double) flrs::wholeFrames (v.position) > lastFrame || v.position < v.first || (v.stolen && v.fade <= 0.0f) || (v.env <= 0.0f && v.stage >= 1))
                {
                    v.active = false;
                    break;
                }

                if (v.flEnvelope)
                {
                    if (v.tickLeft <= 0.0)
                    {
                        // a sequencer tick: step the articulator; the level ramps to its value over round(tick) samples
                        v.volumeArt.step (modulation.volume);
                        const auto perTick = sampleRate / modulation.ticksPerSecond;
                        v.flStep = (v.volumeArt.val - v.flLevel) / (float) std::max (1.0, std::nearbyint (perTick));
                        v.tickLeft += perTick;
                    }
                    if (v.volumeArt.state == 9 && v.flLevel <= 0.0f && v.flStep <= 0.0f)
                    {
                        v.active = false;
                        break;
                    }
                }

                float sl, sr;
                auto& ks = shifts[(std::size_t) (&v - voices.data())];
                if (ks.on)
                    readShifted (v, ks, sl, sr);
                else
                    readFrame (v, l, r, lastFrame, looping, sl, sr, v.rate * v.pitchMul * bendMul);

                if (modulated)
                {
                    for (std::size_t e = 0; e < 2; ++e)
                        if (modEnv[e].active)
                            modEnv[e].advance (v.mod[e]);

                    if (--v.controlCountdown <= 0) // 16-sample control tick, as in the synth
                    {
                        v.controlCountdown = 16;
                        auto semitones = modEnv[1].active ? modEnv[1].amount * v.mod[1].level : 0.0f;
                        auto octaves = modEnv[0].active ? modEnv[0].amount * v.mod[0].level : 0.0f;

                        if (lfos[lfoPitch].active)
                            semitones += lfos[lfoPitch].amount * lfos[lfoPitch].tick (v.lfo[lfoPitch], 16);
                        if (lfos[lfoCutoff].active)
                            octaves += lfos[lfoCutoff].amount * lfos[lfoCutoff].tick (v.lfo[lfoCutoff], 16);
                        if (lfos[lfoVolume].active)
                        {
                            const auto target = std::max (0.0f, 1.0f + lfos[lfoVolume].amount * lfos[lfoVolume].tick (v.lfo[lfoVolume], 16));
                            v.lfoGainStep = (target - v.lfoGain) / 16.0f;
                        }
                        if (lfos[lfoPan].active)
                            voicePan (v.basePan + lfos[lfoPan].amount * lfos[lfoPan].tick (v.lfo[lfoPan], 16), v.panL, v.panR);

                        v.pitchMul = semitones != 0.0f ? std::exp2 (semitones / 12.0) : 1.0;

                        if (filterModulated)
                            v.filter.set (fc * std::exp2 (octaves), res, sampleRate, type);
                    }

                    v.lfoGain += v.lfoGainStep;
                }

                if (filtered)
                {
                    const auto& f = filterModulated ? v.filter : sharedFilter;
                    sl = f.process (sl, v.lowL, v.bandL);
                    sr = f.process (sr, v.lowR, v.bandR);
                }

                auto gain = v.velocity * v.fade * v.env * v.lfoGain * v.zoneGain;

                if (v.flEnvelope)
                {
                    gain *= std::max (0.0f, v.flLevel);
                    const auto next = v.flLevel + v.flStep;
                    // the ramp stops at the tick's value
                    v.flLevel = (v.flStep > 0.0f && next > v.volumeArt.val) || (v.flStep < 0.0f && next < v.volumeArt.val) ? v.volumeArt.val : next;
                    v.tickLeft -= 1.0;
                }

                if (v.fadeIn > 0.0f || v.fadeOut > 0.0f)
                {
                    if (v.fadeIn > 0.0f && v.clipElapsed < v.fadeIn)
                        gain *= v.clipElapsed / v.fadeIn;
                    if (v.fadeOut > 0.0f && v.clipRemaining < v.fadeOut)
                        gain *= std::max (0.0f, v.clipRemaining) / v.fadeOut;

                    v.clipElapsed += 1.0f;
                    v.clipRemaining -= 1.0f;
                }

                left[i] += sl * gain * gl * v.panL;
                right[i] += sr * gain * gr * v.panR;

                v.position += ks.on ? v.rate : v.rate * v.pitchMul * bendMul;
                ++v.age;

                if (v.glideLeft > 0)
                {
                    v.rate *= v.glideRatio;
                    --v.glideLeft;
                }

                // envelope: 0 attack, 1 decay, 2 sustain, 3 release
                if (v.stage == 0)
                {
                    v.env += v.envStep;

                    if (v.env >= 1.0f)
                    {
                        v.env = 1.0f;
                        v.stage = 1;
                    }
                }
                else if (v.stage == 1)
                {
                    const auto s = sustain.load (std::memory_order_relaxed);
                    const auto d = decay.load (std::memory_order_relaxed);

                    if (s >= 1.0f)
                        v.stage = 2;
                    else if (d <= 0.0f)
                    {
                        v.env = s;
                        v.stage = 2;
                    }
                    else
                    {
                        // the ramp goes from 1 to 0 over `decay` and stops at sustain (like FL)
                        v.env -= (float) (1.0 / (d * sampleRate));

                        if (v.env <= s)
                        {
                            v.env = s;
                            v.stage = 2;
                        }
                    }
                }
                else if (v.stage == 3)
                    v.env += v.envStep;

                if (v.stolen)
                    v.fade -= fadeStep;
            }
        }
    }

    // One sequencer tick of FL's sampler voice (FLEngine FUN_00d24ae0 / FUN_00d24500 / FUN_00d24650, REA_rodada76
    // §3–§4): the articulators step; the target gains are the pan law of (note pan + channel pan + pan articulator,
    // initial volume · channel volume · volume articulator); the filter coefficients follow Final.FCut/FRes plus the
    // cutoff/resonance articulators; gains and the fast LP's coefficient ramp towards their targets over round(spt)
    // samples, at most 0.001·44100/fs per sample.
    void flTick (SamplerVoice& v, float chanVol, float chanPan, int perTick) noexcept
    {
        const auto& S = modulation.fl;
        auto& pr = v.flProc;
        // global LFOs (flag 0x20): one phase for the whole channel, advanced on every song tick — at tick k it is
        // (k + 1)·increment (EXEC: FL's render of a global volume LFO, REA_rodada76 §10)
        if (S.hasGlobal)
            for (auto& r : modulation.fl.recs)
                if ((r.flags & 0x20) != 0)
                    r.globalPhase = (std::uint32_t) ((std::uint64_t) (v.tickIndex + 1) * (std::uint64_t) r.lfoInc);
        pr.control (S);
        const auto finalVol = v.initVol * chanVol, finalPan = v.notePan + chanPan;
        float tL, tR;
        if (modulation.flTriangular)
        {
            // FL's pan-law mode 1 (FLEngine FUN_006a39c0, x87): vol·sin(π/4) on the louder side, (1 ∓ pan)·that on the
            // other (EXEC: FL's render with the project's pan law 2, REA_rodada76 §9)
            const auto pan = std::min (std::max (finalPan + pr.pan, -1.0f), 1.0f);
            const auto vol = finalVol * pr.vol * 0.70710677f;
            using ld = long double;
            if (std::signbit (pan))
                tL = vol, tR = (float) (((ld) 1 + (ld) pan) * (ld) vol);
            else
                tR = vol, tL = (float) (((ld) 1 - (ld) pan) * (ld) vol);
        }
        else
            flvp::panLaw (finalPan + pr.pan, finalVol * pr.vol, tL, tR);
        v.pitchMul = pr.pitch != 0.0f ? std::exp2 ((double) pr.pitch / 1200.0) : 1.0;

        const auto first = pr.first;
        const auto cx = ((float) S.modX * 0.00390625f + 0.0f) + pr.cut;
        const auto cy = ((float) S.modY * 0.00390625f + 0.0f) + pr.res;
        if (pr.kind == 0)
        {
            flvp::coefFast (cx, cy, pr.fc, pr.fq, S.rateRatio);
            if (flvp::bitsOf (pr.fc) != 0x3f800000 || flvp::bitsOf (pr.fq) != 0)
                pr.active = true;
            if (! pr.active || first > 0)
                v.fcRun = pr.fc, v.fcSlope = 0.0f;
            else
                v.fcSlope = flvp::rampStep (pr.fc, v.fcRun, perTick, S.rampLimit);
        }
        else if (pr.kind >= 1 && pr.kind <= 4)
            flvp::coefBiquad (pr.kind, cx, cy, pr.fc, pr.fq, pr.b, S.rateRatio);
        else if (pr.kind == 6)
            flvp::coefSvf (cx, cy, pr.fc, pr.fq, S.rateRatio);

        // the first block: with the volume articulator past its attack the gains start at their (unscaled) target,
        // else they ramp up from 0
        auto prevL = v.gL, prevR = v.gR;
        if (first > 1)
            prevL = tL, prevR = tR;
        pr.first = 0;
        if (pr.active && pr.kind >= 1 && pr.kind <= 4 && pr.stage2 != 0)
        {
            const auto g = (float) (1.0 - (double) pr.fq * (3.2 - (double) pr.fc) * 0.046875);
            tL = tL * g, tR = tR * g;
        }
        v.sL = flvp::rampStep (tL, prevL, perTick, S.rampLimit);
        v.sR = flvp::rampStep (tR, prevR, perTick, S.rampLimit);
        v.gL = prevL, v.gR = prevR;

        // the volume envelope has ended and the level is down: the voice is done
        if (pr.arts[1].state == 9 && tL == 0.0f && tR == 0.0f && std::fabs (prevL) < 1.0e-7f && std::fabs (prevR) < 1.0e-7f)
            v.active = false;
    }

    // FL's sampler voice over n samples: split at the song's ticks, the sample read through FL's interpolators, the
    // channel filter on the stereo block, the gains ramped per sample (the channel's volume and pan are inside)
    void renderFl (SamplerVoice& v, float* left, float* right, int n, float chanVol, float chanPan) noexcept
    {
        const auto spt = sampleRate / modulation.ticksPerSecond;
        const auto perTick = (int) std::max<std::int64_t> (1, flvp::roundEven (spt));
        const auto* l = v.data->leftData();
        const auto* r = v.data->rightData();
        const auto lastFrame = std::min ((double) (v.data->frames() - 1), v.last);
        auto& pr = v.flProc;

        for (int pos = 0; pos < n && v.active;)
        {
            if (v.tickLeft <= 0.0)
            {
                flTick (v, chanVol, chanPan, perTick);
                v.tickLeft += v.nextGap;
                ++v.tickIndex;
                v.nextGap = (double) (flvp::roundEven ((double) (v.tickIndex + 1) * spt) - flvp::roundEven ((double) v.tickIndex * spt));
                if (! v.active)
                    break;
            }
            auto seg = std::min ({ n - pos, (int) std::ceil (v.tickLeft), flBlock });

            // read the segment (loop wrap, region end, declick of stolen voices, clip fades)
            auto ended = false;
            int m = 0;
            for (; m < seg; ++m)
            {
                const auto looping = v.loopEnd > v.loopStart && (! v.loopWhileHeld || v.stage < 3);
                if (looping && v.position >= v.loopEnd)
                {
                    while (v.position >= v.loopEnd)
                        v.position = (v.position - v.loopEnd) + v.loopStart;
                    ++v.loops;
                }
                if (v.stolen && v.fade <= 0.0f)
                {
                    ended = true;
                    break;
                }
                // past the region's end FL holds the last frame and fades it out with the 441-entry end table (EXEC:
                // FL's render, REA_rodada76 §6)
                if (v.endFade < 0 && ((double) flrs::wholeFrames (v.position) > lastFrame || v.position < v.first))
                    v.endFade = 0;
                float sl, sr, endGain = 1.0f;
                if (v.endFade >= 0)
                {
                    const auto& table = flvp::endTable();
                    if (v.endFade >= (int) table.size())
                    {
                        ended = true;
                        break;
                    }
                    endGain = table[(std::size_t) v.endFade++];
                    sl = v.holdL, sr = v.holdR;
                }
                else
                {
                    if (auto& ks = shifts[(std::size_t) (&v - voices.data())]; ks.on)
                        readShifted (v, ks, sl, sr);
                    else
                        readFrame (v, l, r, lastFrame, looping, sl, sr, v.rate * v.pitchMul * bendMul);
                    v.holdL = sl, v.holdR = sr;
                }
                auto gain = v.fade * v.zoneGain * outGain.load (std::memory_order_relaxed);
                if (v.fadeIn > 0.0f || v.fadeOut > 0.0f)
                {
                    if (v.fadeIn > 0.0f && v.clipElapsed < v.fadeIn)
                        gain *= v.clipElapsed / v.fadeIn;
                    if (v.fadeOut > 0.0f && v.clipRemaining < v.fadeOut)
                        gain *= std::max (0.0f, v.clipRemaining) / v.fadeOut;
                    v.clipElapsed += 1.0f;
                    v.clipRemaining -= 1.0f;
                }
                flX[(std::size_t) (2 * m)] = sl;
                flX[(std::size_t) (2 * m + 1)] = sr;
                if (v.offFade >= 0)
                {
                    const auto& table = flvp::endTable();
                    if (v.offFade >= (int) table.size())
                    {
                        ended = true;
                        break;
                    }
                    endGain *= table[(std::size_t) v.offFade++];
                }
                flG[(std::size_t) m] = gain * endGain;
                v.position += shifts[(std::size_t) (&v - voices.data())].on ? v.rate : v.rate * v.pitchMul * bendMul;
                ++v.age;
                if (v.glideLeft > 0)
                {
                    v.rate *= v.glideRatio;
                    --v.glideLeft;
                }
                if (v.stolen)
                    v.fade -= fadeStep;
            }
            seg = m;

            // the channel filter (FLEngine FUN_00d247a0)
            const float* y = flX.data();
            if (pr.active && seg > 0)
            {
                if (pr.kind == 0)
                {
                    flvp::renderFast (pr.fst, pr.fq, flX.data(), flY.data(), seg, v.fcRun, v.fcSlope);
                    v.fcRun = v.fcRun + (float) seg * v.fcSlope;
                    y = flY.data();
                }
                else if (pr.kind >= 1 && pr.kind <= 4)
                {
                    flvp::renderBiquad (pr.st1, pr.b, flX.data(), flY.data(), seg);
                    if (pr.stage2 != 0)
                        flvp::renderBiquad (pr.st2, pr.b, flY.data(), flY.data(), seg);
                    y = flY.data();
                }
                else if (pr.kind == 6)
                {
                    flvp::renderSvf (pr.st1, pr.fc, pr.fq, flX.data(), flY.data(), seg);
                    if (pr.stage2 != 0)
                        flvp::renderSvf (pr.st2, pr.fc, pr.fq + (1.0f - pr.fq) * 0.25f, flY.data(), flY.data(), seg);
                    y = flY.data();
                }
            }

            for (int i = 0; i < seg; ++i)
            {
                const auto g = flG[(std::size_t) i];
                left[pos + i] += y[2 * i] * ((v.gL + (float) i * v.sL) * g);
                right[pos + i] += y[2 * i + 1] * ((v.gR + (float) i * v.sR) * g);
            }
            v.gL = v.gL + v.sL * (float) seg;
            v.gR = v.gR + v.sR * (float) seg;
            v.tickLeft -= seg;
            pos += seg;
            if (ended)
                v.active = false;
        }
    }

    // FL's sampler read (REA_rodada69 §1): the kernel's taps around the read position, frames outside the region
    // repeating its first/last frame, taps past the loop end wrapping to the loop start (and, once the voice has
    // looped, taps before the loop start wrapping to its end), a reversed voice reading the region backwards
    void readFrame (SamplerVoice& v, const float* l, const float* r, double lastFrame, bool looping, float& outL, float& outR, double speed) noexcept
    {
        v.kernel.setSpeed (speed);
        const auto half = v.kernel.halfWidth;
        const auto whole = flrs::wholeFrames (v.position);
        const auto frac = v.position - (double) whole;
        const auto first = (std::int64_t) v.first, last = (std::int64_t) lastFrame;
        const auto lo = whole - half + 1, hi = whole + half;
        const auto loopStart = (std::int64_t) v.loopStart, loopEnd = (std::int64_t) v.loopEnd;

        // at the original rate on a whole frame FL copies the sample instead of interpolating
        if (speed == 1.0 && frac == 0.0)
        {
            const auto q = v.reversed ? first + last - whole : whole;
            outL = l[q];
            outR = r[q];
            return;
        }

        if (! v.reversed && lo >= first && hi <= last && ! (looping && hi >= loopEnd))
        {
            flrs::interpolate (v.kernel, l + lo, r + lo, frac, outL, outR);
            return;
        }

        for (std::int64_t t = 0; t < 2 * half; ++t)
        {
            auto q = lo + t;
            if (looping && loopEnd > loopStart)
            {
                while (q >= loopEnd)
                    q -= loopEnd - loopStart;
                if (v.loops > 0 && q < loopStart)
                    q += loopEnd - loopStart;
            }
            q = std::clamp (q, first, last);
            if (v.reversed)
                q = first + last - q;
            tapL[(std::size_t) t] = l[q];
            tapR[(std::size_t) t] = r[q];
        }
        flrs::interpolate (v.kernel, tapL.data(), tapR.data(), frac, outL, outR);
    }

    bool idle() const noexcept override
    {
        return std::none_of (voices.begin(), voices.end(), [] (const auto& v) { return v.active; });
    }

private:
    std::shared_ptr<const SampleData> sample;
    bool clipMode = false;
    double sampleRate = 44100.0;
    float fadeStep = 0.01f;
    std::array<SamplerVoice, 16> voices {};
    std::array<KeyShift, 16> shifts {};
    std::array<std::unique_ptr<VoicePitchShifter>, keyShiftVoices> shifterPool;
    std::array<int, keyShiftVoices> shifterUser {}; // the voice using each shifter (-1 = none)
    std::vector<float> primeL, primeR;
    std::atomic<bool> keyShift { false };
    std::atomic<int> keyShiftMethod { 0 };
    double lastFactor = 1.0; // the last shifted note's transposition (where its portamento starts)
    double bendMul = 1.0;    // the pitch wheel (pitchBend), audio thread
    int quality = flrs::liveQuality;        // FL's interpolation index (prepare)
    std::array<float, 1024> tapL {}, tapR {}; // kernel taps gathered near the region edges and loops
    static constexpr int flBlock = 512;       // FL voices: longest segment rendered at once
    std::array<float, 2 * flBlock> flX {}, flY {};
    std::array<float, flBlock> flG {};
    std::atomic<float> pitch { 0.0f }, attack { 0.0f }, decay { 0.0f }, sustain { 1.0f }, release { 0.0f }, stretchBeats { 0.0f };
    std::atomic<double> bpm { 120.0 };
    std::atomic<float> regionStart { 0.0f }, regionEnd { 1.0f };
    std::atomic<float> cutoff { 20000.0f }, resonance { 0.0f };
    // automated knobs (issue #12): which hold an automation's value, and the song values configure last saw (UI thread)
    std::array<std::atomic<bool>, samplerparam::count> automatedKnob {};
    std::array<float, samplerparam::count> configuredKnobs { [] {
        std::array<float, samplerparam::count> a {};
        a.fill (std::numeric_limits<float>::quiet_NaN());
        return a;
    }() };

    void storeKnob (int k, float natural) noexcept
    {
        switch (k)
        {
            case samplerparam::attack: attack.store (std::clamp (natural, 0.0f, 2.0f)); break;
            case samplerparam::decay: decay.store (std::clamp (natural, 0.0f, 10.0f)); break;
            case samplerparam::sustain: sustain.store (std::clamp (natural, 0.0f, 1.0f)); break;
            case samplerparam::release: release.store (std::clamp (natural, 0.0f, 4.0f)); break;
            case samplerparam::cutoff: cutoff.store (std::clamp (natural, 20.0f, 20000.0f)); break;
            case samplerparam::resonance: resonance.store (std::clamp (natural, 0.0f, 1.0f)); break;
            case samplerparam::start: regionStart.store (std::clamp (natural, 0.0f, 1.0f)); break;
            case samplerparam::end: regionEnd.store (std::clamp (natural, 0.0f, 1.0f)); break;
            default: break;
        }
    }

public:
    // an automation clip on one of the Sampler's knobs (samplerparam, issue #12). Audio thread.
    void setParameter (int index, float value) noexcept override
    {
        if (index < 0 || index >= samplerparam::count)
            return;
        automatedKnob[(std::size_t) index].store (true, std::memory_order_relaxed);
        storeKnob (index, samplerparam::toNatural (index, value));
    }

private:
    std::atomic<bool> porta { false };
    std::atomic<float> outGain { 1.0f }; // SamplerSettings::gainDb as a factor
    std::atomic<float> portaBeats { 0.125f };
    std::vector<LoadedZone> zones; // multi-sample (fixed after creation)
    double songTicks = 0.0;         // song position in the block (global LFO)

public:
    void setSongPosition (double ticks) noexcept override
    {
        songTicks = ticks;
        // the song's sample position (FL's ticks fall at round(k · samples per tick) from the song start)
        const auto tps = (double) ppq * bpm.load() / (60.0 * sampleRate);
        songSample = tps > 0.0 ? std::nearbyint (ticks / tps) : 0.0;
    }

private:
    double songSample = 0.0;        // sample position of the next render call in the song
    int lastZone = -1;              // the zone the last note chose (overlapping layers cycle from it)
    double lastRate = 0.0; // read speed of the last note (where porta starts from)
    std::atomic<int> filterType { 0 };
    float filterCutoff = -1.0f, filterResonance = -1.0f; // values the coefficients were computed with
    SvfCoefficients sharedFilter;
    struct Modulation
    {
        std::array<ModEnvelope, 2> envelopes {};
        std::array<ChannelLfo, lfoTargets> lfo {};
        bool flVolume = false;     // FL's volume articulator replaces the ADSR
        flvp::Rec volume {};       // its settings (FLEngine FUN_009ce9e0 = the plugins' FUN_0071ff00, REA_rodada69 §3)
        double ticksPerSecond = 0; // FL steps it once per sequencer tick
        bool flNative = false;     // FL's whole sampler voice (SamplerSettings::flChannel)
        bool flTriangular = false; // the project's triangular pan law
        flvp::Settings fl {};
    };
    SettingsMailbox<Modulation> envelopeMailbox;
    Modulation modulation;
    std::array<LfoCoefficients, lfoTargets> lfos {};
    bool anyLfo = false;

    void updateModulation() noexcept
    {
        anyLfo = false;
        if (modulation.flNative)
            modulation.fl.setRate (sampleRate); // FLEngine FUN_006a0230: ratio 44100/fs, ramp limit ratio·0.001

        for (std::size_t e = 0; e < 2; ++e)
            modEnv[e] = ModCoefficients::make (modulation.envelopes[e], sampleRate);

        for (std::size_t i = 0; i < lfoTargets; ++i)
        {
            lfos[i] = LfoCoefficients::make (modulation.lfo[i], sampleRate, bpm.load());
            anyLfo = anyLfo || lfos[i].active;
        }
    }
    std::array<ModCoefficients, 2> modEnv {};
    std::array<float, 4> nextClip {}; // fades for the next noteOn (only the audio thread touches it)

    struct SliceTable
    {
        std::array<float, 128> starts {};
        int count = 0;
    };
    SettingsMailbox<SliceTable> sliceMailbox;
    SliceTable slices; // audio thread's copy
    float nextPan = 0.0f, nextCents = 0.0f;
    float nextGain = 1.0f; // the next note's volume factor (setNextNoteGain)
    bool nextPorta = false;
    std::atomic<bool> reverse { false }, cut { false };
};
} // namespace

std::unique_ptr<Instrument> makeZonedSampler (std::vector<LoadedZone> zones)
{
    auto sampler = std::make_unique<SamplerInstrument> (nullptr, false);
    sampler->setZones (std::move (zones));
    return sampler;
}

namespace
{
// FL's Envelope Controller (REA_rodada15 §10, REA_rodada162e-envelope.md): no audio; each articulator follows the last note in
// its key range (envelope, LFO and maps restart); with no note the output holds its last value, or with the
// controller's continuous flag follows base and the Mod X / Y maps.
class ControllerInstrument final : public Instrument
{
public:
    void prepare (double sr) noexcept override { sampleRate = sr; }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        Data data {};
        if (auto keyboard = parseKeyboardController (channel.controllerState))
        {
            data.keyboard = true;
            data.keys = *keyboard;
        }
        else if (auto parsed = parseEnvelopeController (channel.controllerState))
        {
            data.articulators = *parsed;
            const auto beat = 60.0 / std::max (1.0, bpm);
            data.beat = beat;
            for (std::size_t k = 0; k < data.articulators.size(); ++k)
            {
                auto& a = data.articulators[k];
                a = controllerWithKnobs (a);
                if (a.tempo)
                {
                    for (int i = 0; i < a.count; ++i)
                        a.time[(std::size_t) i] = (float) (a.time[(std::size_t) i] * beat);
                    a.tempo = false;
                }
                // LFO: cycles per second; its amplitude curve in seconds
                data.lfoRate[k] = controllerLfoRate (a) / (a.lfoTempo ? beat : 1.0);
                if (a.lfoTempo)
                    for (int i = 0; i < a.lfoAmp.count; ++i)
                        a.lfoAmp.time[(std::size_t) i] = (float) (a.lfoAmp.time[(std::size_t) i] * beat);
            }
        }
        mailbox.write (data);
    }

    void setNextNoteModulation (float modX, float modY) noexcept override
    {
        nextModX = modX;
        nextModY = modY;
    }

    // automation / links of the global Mod X / Mod Y (FL's parameters 88 / 89, round 164f): they hold until the knob
    // is moved in the editor
    void setParameter (int index, float value) noexcept override
    {
        if (index == envelopeControllerModXParam)
            autoModX = std::clamp (value, 0.0f, 1.0f);
        else if (index == envelopeControllerModYParam)
            autoModY = std::clamp (value, 0.0f, 1.0f);
    }

    void setSongPosition (double ticks) noexcept override { songTicks = ticks; }

    void noteOn (int key, float velocity) noexcept override
    {
        mailbox.read (data);
        if (data.keyboard) // Keyboard Controller: output 0 is the key's value
        {
            heldKey = key;
            if (key >= 0 && key < (int) data.keys.keys.size())
                values[0] = data.keys.keys[(std::size_t) key];
            return;
        }
        ControllerNote note;
        note.key = (float) key;
        note.velocity = velocity;
        note.modX = nextModX;
        note.modY = nextModY;
        // one random value per note (FL draws it at the trigger)
        randomState = randomState * 1664525u + 1013904223u;
        note.random = (float) ((randomState >> 8) * (1.0 / 16777216.0));
        nextModX = nextModY = 0.5f;
        for (std::size_t k = 0; k < voices.size(); ++k)
        {
            const auto& a = data.articulators[k];
            if (key >= a.lowKey && key <= a.highKey)
            {
                auto& v = voices[k];
                v = { true, key, 0.0, -1.0, note, 0.0 };
                v.lfoPhase = a.lfoGlobal ? globalPhase (k) : controllerLfoStartPhase (a);
                v.used = true;
            }
        }
    }

    void noteOff (int key) noexcept override
    {
        if (data.keyboard)
        {
            if (key == heldKey && data.keys.autoRelease)
                values[0] = data.keys.noteOffValue;
            return;
        }
        for (auto& v : voices)
            if (v.active && v.key == key && v.sinceRelease < 0.0)
                v.sinceRelease = 0.0;
    }

    void allNotesOff() noexcept override
    {
        for (auto& v : voices)
            if (v.active && v.sinceRelease < 0.0)
                v.sinceRelease = 0.0;
    }

    void render (float*, float*, int n, float, float) noexcept override
    {
        const auto changed = mailbox.read (data);
        if (data.keyboard)
            return;
        // the editor moved Mod X / Y: that value wins over the last automation value
        if (data.articulators.modX != seenModX)
        {
            seenModX = data.articulators.modX;
            autoModX = std::numeric_limits<float>::quiet_NaN();
        }
        if (data.articulators.modY != seenModY)
        {
            seenModY = data.articulators.modY;
            autoModY = std::numeric_limits<float>::quiet_NaN();
        }
        const auto modX = std::isnan (autoModX) ? data.articulators.modX : autoModX;
        const auto modY = std::isnan (autoModY) ? data.articulators.modY : autoModY;
        if (changed)
            for (std::size_t k = 0; k < values.size(); ++k)
                if (! voices[k].active && (! voices[k].used || data.articulators.continuous))
                    values[k] = data.articulators.continuous ? controllerRestingValue (data.articulators[k], modX, modY) : data.articulators[k].base;
        const auto seconds = n / sampleRate;
        for (std::size_t k = 0; k < voices.size(); ++k)
        {
            auto& v = voices[k];
            const auto& a = data.articulators[k];
            if (! v.active)
            {
                if (data.articulators.continuous)
                    values[k] = controllerRestingValue (a, modX, modY);
                continue;
            }
            v.sinceOn += seconds;
            if (v.sinceRelease >= 0.0)
                v.sinceRelease += seconds;
            v.lfoPhase = a.lfoGlobal ? globalPhase (k) : v.lfoPhase + data.lfoRate[k] * seconds;
            if (v.lfoPhase > 1.0e6)
                v.lfoPhase -= std::floor (v.lfoPhase);
            values[k] = controllerValue (a, v.sinceOn, v.sinceRelease, v.lfoPhase, v.note, modX, modY);
            if (controllerVoiceFinished (a, v.sinceOn, v.sinceRelease))
                v.active = false; // the output holds its last value
        }
    }

    float controlOutput (int output) const noexcept override
    {
        return output >= 0 && output < (int) values.size() ? values[(std::size_t) output] : 0.0f;
    }

private:
    struct Data
    {
        EnvelopeControllerData articulators {};
        std::array<double, 8> lfoRate {};
        double beat = 0.5;
        bool keyboard = false;
        KeyboardControllerData keys {};
    };
    // a "global" LFO follows the song position instead of restarting on each note
    double globalPhase (std::size_t k) const noexcept
    {
        const auto& a = data.articulators[k];
        const auto seconds = songTicks / ppq * data.beat;
        return controllerLfoStartPhase (a) + data.lfoRate[k] * seconds;
    }

    int heldKey = -1;
    struct Voice
    {
        bool active = false;
        int key = 0;
        double sinceOn = 0.0, sinceRelease = -1.0;
        ControllerNote note {};
        double lfoPhase = 0.0;
        bool used = false; // played once: a finished voice holds its last value
    };

    SettingsMailbox<Data> mailbox;
    Data data {};
    std::array<Voice, 8> voices {};
    std::array<float, 8> values { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    double sampleRate = 44100.0, songTicks = 0.0;
    float nextModX = 0.5f, nextModY = 0.5f;
    float autoModX = std::numeric_limits<float>::quiet_NaN(), autoModY = std::numeric_limits<float>::quiet_NaN();
    float seenModX = -1.0f, seenModY = -1.0f;
    std::uint32_t randomState = 0x2545F491u;
};
} // namespace

namespace
{
std::atomic<bool> offlineRenderingFlag { false };
}

void setOfflineRendering (bool on) noexcept { offlineRenderingFlag.store (on); }
bool offlineRendering() noexcept { return offlineRenderingFlag.load(); }
namespace
{
std::atomic<bool> hqAllFlag { false };
}
void setHqForAllPlugins (bool on) noexcept { hqAllFlag.store (on); }
bool hqForAllPlugins() noexcept { return hqAllFlag.load (std::memory_order_relaxed); }

HostTimeline& hostTimeline() noexcept
{
    static HostTimeline timeline;
    return timeline;
}

namespace
{
// FL's MIDI Out: makes no sound itself (its notes are routed to the plugins on its port by the renderer)
class MidiOutInstrument final : public Instrument
{
public:
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    bool idle() const noexcept override { return true; }
};

// A VFX note processor in the Channel Rack or a Patcher (round 164b): no sound; the renderer / the graph run its
// processor on the notes and send what it plays to the instruments on its outputs.
class VfxInstrument final : public Instrument
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = sampleRate;
        if (processor != nullptr)
            processor->prepare (sampleRate);
    }
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    bool idle() const noexcept override { return true; }
    vfx::Processor* noteProcessor() noexcept override { return processor.get(); }
    // (UI thread: the first call builds the processor before the instrument reaches the audio thread; later ones only
    // hand the new settings over)
    void configure (const ChannelDef& channel, double) noexcept override
    {
        const auto state = vfx::readState (channel.pluginState);
        if (! state)
            return;
        if (processor == nullptr || processor->kind() != state->kind)
        {
            if (processor == nullptr)
            {
                processor = vfx::makeProcessor (state->kind, state->fl);
                processor->prepare (rate);
                processor->setTag (state->tag);
                lastState = state->fl;
            }
            return;
        }
        processor->setTag (state->tag);
        if (state->fl != lastState)
            processor->configure (state->fl);
        lastState = state->fl;
    }
    // a VFX Script's output controllers and controls (round 165d)
    float controlOutput (int output) const noexcept override { return processor != nullptr ? processor->controlOutput (output) : 0.0f; }
    void setParameter (int index, float value) noexcept override
    {
        if (processor != nullptr)
            processor->setParameter (index, value);
    }

private:
    std::unique_ptr<vfx::Processor> processor;
    std::vector<std::uint8_t> lastState;
    double rate = 44100.0;
};
} // namespace

float samplerGain (float db) noexcept
{
    if (! std::isfinite (db) || db == 0.0f)
        return 1.0f;
    return (float) std::pow (10.0, (double) std::clamp (db, -24.0f, 24.0f) / 20.0);
}

std::unique_ptr<Instrument> makeInstrument (InstrumentKind kind, std::shared_ptr<const SampleData> sample)
{
    if (kind == InstrumentKind::controller)
        return std::make_unique<ControllerInstrument>();
    if (kind == InstrumentKind::midiOut || kind == InstrumentKind::layer) // (a Layer's notes go to its children)
        return std::make_unique<MidiOutInstrument>();
    if (kind == InstrumentKind::dashboard) // its controls' values as control outputs (round 164)
        return surface::makeDashboard();
    if (kind == InstrumentKind::voltage) // its notes as control voltages (round 164)
        return voltage::makeInstrument();
    if (kind == InstrumentKind::vfx)
        return std::make_unique<VfxInstrument>();

    if (kind == InstrumentKind::sampler || kind == InstrumentKind::audioClip)
        return std::make_unique<SamplerInstrument> (std::move (sample), kind == InstrumentKind::audioClip);

    if (kind == InstrumentKind::synth)
        return std::make_unique<SynthInstrument>();

    if (kind == InstrumentKind::sakura)
        return makeSakuraInstrument();

    if (kind == InstrumentKind::drumpad)
        return makeDrumpadInstrument();

    if (kind == InstrumentKind::boobass)
        return makeBooBassInstrument();

    if (kind == InstrumentKind::drumsynth)
        return makeDrumSynthInstrument();

    if (kind == InstrumentKind::harmless)
        return makeHarmlessInstrument();
    if (kind == InstrumentKind::sytrus)
        return makeSytrusInstrument();

    if (kind == InstrumentKind::harmor)
        return makeHarmorInstrument();

    if (kind == InstrumentKind::gms)
        return makeGmsInstrument();

    if (kind == InstrumentKind::morphine)
        return makeMorphineInstrument();
    if (kind == InstrumentKind::ogun)
        return makeOgunInstrument();
    if (kind == InstrumentKind::minisynth)
        return makeMiniSynthInstrument();
    if (kind == InstrumentKind::dx10)
        return makeDx10Instrument();
    if (kind == InstrumentKind::kepler)
        return makeKeplerInstrument();
    if (kind == InstrumentKind::poizone)
        return makePoiZoneInstrument();
    if (kind == InstrumentKind::fruitkick)
        return makeFruitKickInstrument();
    if (kind == InstrumentKind::bassdrum)
        return makeBassDrumInstrument();
    if (kind == InstrumentKind::toxic)
        return makeToxicInstrument();
    if (kind == InstrumentKind::threeosc)
        return makeThreeOscInstrument (&offlineRendering);
    if (kind == InstrumentKind::autogun)
        return makeAutogunInstrument();
    if (kind == InstrumentKind::transistorbass)
        return makeTransistorBassInstrument();
    if (kind == InstrumentKind::simsynth)
        return makeSimSynthInstrument();
    if (kind == InstrumentKind::wasp)
        return makeWaspInstrument(); // makes no sound and keeps its state
    if (kind == InstrumentKind::fruitydance)
        return dance::makeInstrument(); // silent: its notes pick the dancer's moves (round 165e)
    if (kind == InstrumentKind::plucked)
        return makePluckedInstrument();
    if (kind == InstrumentKind::beepmap)
        return makeBeepMapInstrument();
    if (kind == InstrumentKind::sawer)
        return makeSawerInstrument();
    if (kind == InstrumentKind::granulizer)
        return makeGranulizerInstrument (std::move (sample));
    if (kind == InstrumentKind::drumaxx)
        return makeDrumaxxInstrument();
    if (kind == InstrumentKind::slayer)
        return makeSlayerInstrument();
    if (kind == InstrumentKind::directwave)
        return dw::makeDirectWaveInstrument ({}); // samples come from RackRenderer
    if (kind == InstrumentKind::soundfont)
        return sf::makeInstrument (nullptr); // the font comes from RackRenderer

    return std::make_unique<DrumInstrument> (kind);
}
} // namespace lynx
