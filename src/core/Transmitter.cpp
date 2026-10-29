#include "core/Transmitter.h"

#include "core/Denormals.h"
#include "core/Effects.h"
#include "core/IldspMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Transmitter: own implementation of the behaviour described in docs/analise/REA_rodada165g-fltrans165.md.

namespace lynx::transmitter
{
namespace
{
constexpr float tiny = 1.1920929e-07f; // FLT_EPSILON: the engine's floor for divisions and logarithms
constexpr float pi = 3.14159274f;

float expF (float x) { return ildsp::expf_ (x); }
float logF (float x) { return (float) std::log ((double) x); }
float tanF (float x) { return (float) std::tan ((double) x); }

// one-pole glide of a control in topology-preserving form (FL's engine glides its time controls at 10 Hz)
struct Glide
{
    float a = 0, a2 = 0, s = 0;
    void setup (float fs, float hz)
    {
        const float t = tanF (hz * 2.0f * pi / fs);
        a = t / (t + 1.0f);
        a2 = a + a;
    }
    float next (float target)
    {
        const float d = target - s;
        const float out = d * a + s;
        s = d * a2 + s;
        return out;
    }
};

// 2nd-order Butterworth state-variable filter (lowpass / highpass outputs)
struct Svf
{
    float g = 0, k = 1.41421354f, h = 0, s1 = 0, s2 = 0;
    void setup (float fs, float hz)
    {
        g = tanF (hz * pi / fs);
        h = 1.0f / ((g + k) * g + 1.0f);
    }
    void run (float x, float& lp, float& hp)
    {
        hp = (x - (g + k) * s1 - s2) * h;
        const float v1 = g * hp;
        const float bp = v1 + s1;
        s1 = bp + v1;
        const float v2 = g * bp;
        lp = v2 + s2;
        s2 = lp + v2;
    }
};

// fourth-order (two cascaded Butterworth) band split
struct Split
{
    Svf l1, l2, h1, h2;
    void setup (float fs, float hz)
    {
        l1.setup (fs, hz); l2.setup (fs, hz); h1.setup (fs, hz); h2.setup (fs, hz);
    }
    void run (float x, float& lo, float& hi)
    {
        float a, b, c, d;
        l1.run (x, a, b);
        l2.run (a, lo, c);
        h1.run (x, c, b);
        h2.run (b, c, hi);
        (void) d;
    }
};

// ---- the onset detector: an envelope e ∈ [0, 1] of the transients of a mono signal
struct Detector
{
    float fs = 44100, cOne = 0;           // cOne = the time-constant scale (14.444787 / fs)
    Split splits[7];
    float release[8] {};                  // the bands' release coefficients (per mode)
    float stage[8][4] {}, logPrev[8] {};
    float slow = 0, slowA = 0, avg[4] {}, gateTime = 0;
    int sinceTrigger = 0;
    int holdCount[8] {}, holdValue[8] {};
    float env[4] {};
    Glide laGlide, holdGlide, relGlide;
    int modeNow = -1;
    float baseTime = 0.1f, minGap = 0;

    void prepare (float rate)
    {
        fs = std::clamp (rate, 1.0f, 192000.0f);
        cOne = 14.444787f / fs;
        // the band edges: 47.6, 113, 269, 640, 1522, 3620, 8611 Hz (split order: 640 first)
        static const float edges[7] = { 640.0f, 113.1f, 47.57f, 269.1f, 3620.4f, 1522.2f, 8611.0f };
        for (int i = 0; i < 7; ++i)
            splits[i].setup (fs, edges[i]);
        slowA = expF (-125.663704f / fs);
        laGlide.setup (fs, 10.0f); holdGlide.setup (fs, 10.0f); relGlide.setup (fs, 10.0f);
        modeNow = -1;
    }
    void setMode (int mode)
    {
        if (mode == modeNow)
            return;
        modeNow = mode;
        baseTime = mode <= 0 ? 0.1f : mode == 1 ? 0.05f : 0.04f;
        minGap = fs * (mode <= 0 ? 0.05f : mode == 1 ? 0.02f : 0.005f) - 1.0f;
        const float t2 = baseTime + baseTime;
        for (int b = 0; b < 8; ++b)
            release[b] = expF (-(cOne / std::max (tiny, t2 - 0.01f * (float) b)));
    }
    // sens 0…1, la / hold / rel in s
    float run (float x, float sensFactor, float la, float hold, float rel)
    {
        // the eight bands
        float bands[8];
        float lo, hi, ll, lh, hl, hh;
        splits[0].run (x, lo, hi);
        splits[1].run (lo, ll, lh);
        splits[2].run (ll, bands[0], bands[1]);
        splits[3].run (lh, bands[2], bands[3]);
        splits[4].run (hi, hl, hh);
        splits[5].run (hl, bands[4], bands[5]);
        splits[6].run (hh, bands[6], bands[7]);
        float flux = 0;
        for (int b = 0; b < 8; ++b)
        {
            const float p = bands[b] * bands[b];
            float r = p / std::max (tiny, stage[b][3]);
            r = std::max (1.0f, logF (std::max (tiny, r)));
            const float attack = expF (-(cOne / std::max (tiny, baseTime / (r * r))));
            float in = p;
            for (int k = 0; k < 4; ++k)
            {
                const float c = in > stage[b][k] ? attack : release[b];
                stage[b][k] = (1.0f - c) * in + c * stage[b][k];
                in = stage[b][k];
            }
            const float l = logF (std::max (tiny, in));
            flux += std::max (0.0f, l - logPrev[b]);
            logPrev[b] = l;
        }
        // the long-term level of the flux (frozen while the input is silent)
        slow = x * x * (1.0f - slowA) + slowA * slow;
        const float live = 1.0f - (slow < 1e-6f ? 1.0f : 0.0f);
        const float t = std::max (tiny, gateTime / std::max (tiny, live));
        gateTime = 3.0f;
        const float a = expF (-(cOne / t));
        float in = flux;
        for (int k = 0; k < 4; ++k)
            in = avg[k] = a * avg[k] + (1.0f - a) * in;
        const int trigger = (sensFactor - 1.0f < (flux - in * 7.0f) * 10.0f && minGap <= (float) sinceTrigger) ? 1 : 0;
        sinceTrigger = trigger ? 0 : sinceTrigger + 1;
        // hold: eight cascaded peak holds of (lookahead + hold) / 8 each
        const float laNow = laGlide.next (la);
        const float holdNow = holdGlide.next (hold);
        const float h = std::nearbyint ((holdNow + laNow) * (fs * 0.125f));
        int v = trigger;
        for (int k = 0; k < 8; ++k)
        {
            if ((float) holdCount[k] < h && v < holdValue[k])
            {
                ++holdCount[k];
                v = holdValue[k];
            }
            else
                holdCount[k] = 0;
            holdValue[k] = v;
        }
        // attack over the lookahead, release over the release time
        const float relNow = std::max (tiny, relGlide.next (rel));
        const float aR = expF (-(cOne / relNow));
        const float aA = expF (-(cOne / std::max (tiny, laNow)));
        float e = (float) v;
        for (int k = 0; k < 4; ++k)
        {
            const float c = e > env[k] ? aA : aR;
            env[k] = (1.0f - c) * e + env[k] * c;
            e = env[k];
        }
        return e;
    }
};

// ---- the lookahead delay of one channel (rounded to whole samples, glided)
struct Lookahead
{
    std::vector<float> ring = std::vector<float> (65536, 0.0f);
    unsigned w = 0;
    Glide glide;
    float fs = 44100;
    void prepare (float rate) { fs = std::clamp (rate, 1.0f, 192000.0f); glide.setup (fs, 10.0f); }
    float run (float x, float seconds)
    {
        ring[w & 0xffff] = x;
        const float d = std::clamp (std::round (glide.next (seconds) * fs), 0.0f, 48000.0f);
        const float y = ring[(w - (unsigned) (int) d) & 0xffff];
        ++w;
        return y;
    }
};

// ---- tilt: one-pole split at 1 kHz, high part × g, low part ÷ g
struct Tilt
{
    float fs = 44100, s[2] {};
    void run (float* l, float* r, int n, float db)
    {
        const float g = ildsp::powf_ (10.0f, db * 0.05f);
        const float fc = std::min (g * 1000.0f, fs * 0.4995f);
        const float t = tanF (fc * (pi / fs));
        const float a = t / (t + 1.0f), a2 = a + a, ig = 1.0f / g;
        float* ch[2] = { l, r };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float x = ch[c][i], v = x - s[c];
                const float lp = v * a + s[c];
                s[c] = v * a2 + s[c];
                ch[c][i] = (x - lp) * g + lp * ig;
            }
    }
};

// ---- Saturation: smoothed tanh gain of the driven signal, warmth = its lowpass, a soft knee above
struct Saturation
{
    float fs = 44100, c = 0, mixG = 0, driveG = 0, warmG = 0, freqG = 0;
    struct Ch { float ap = 0, apD = 0, g1 = 0, g2 = 0; } ch[2];
    void prepare (float rate) { fs = std::clamp (rate, 1.0f, 192000.0f); c = 44.1f / fs; }
    void run (float* l, float* r, int n, float mix, float driveDb, float warmth)
    {
        const float drive = ildsp::powf_ (10.0f, driveDb * 0.05f);
        const float sw = std::sqrt (warmth);
        float* io[2] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            mixG = mixG * (1.0f - c) + c * mix;
            warmG = (1.0f - c) * warmG + sw * c;
            const float rest = 1.0f - warmG;
            freqG = ildsp::powf_ (2.0f, rest * 10.0f) * (20.0f * c) + (1.0f - c) * freqG;
            driveG = (1.0f - c) * driveG + drive * c;
            const float t = tanF (freqG * (pi / fs)), d1 = t + 1.0f;
            const float knee = rest * 0.9f, span = 1.0f - knee;
            for (int k = 0; k < 2; ++k)
            {
                Ch& s = ch[k];
                const float x = io[k][i];
                // the dry path through the same one-pole as an allpass (phase-matched to the wet path)
                const float v = (x - s.ap) * t * (2.0f / d1);
                const float dry = s.ap + s.ap + v - x;
                s.ap += v;
                const float dx = x * driveG;
                const float vd = ((dx - s.apD) * (t + t)) / d1;
                const float apD = s.apD + s.apD + vd - dx;
                s.apD += vd;
                float safe = dx;
                if (safe >= 0.0f) safe = std::max (safe, tiny);
                else safe = std::min (safe, -tiny);
                const float u1 = ((std::tanh (safe) / safe - s.g1) * t) / d1;
                const float lp1 = u1 + s.g1;
                s.g1 += u1 + u1;
                const float u2 = ((lp1 - s.g2) * t) / d1;
                const float gain = u2 + s.g2;
                s.g2 += u2 + u2;
                float y = gain * apD;
                if (knee < std::fabs (y))
                    y = y > knee ? std::tanh ((y - knee) / span) * span + knee : std::tanh ((y + knee) / span) * span - knee;
                io[k][i] = mixG * y + dry * (1.0f - mixG);
            }
        }
    }
};

// ---- Delay: feedback delay with a 10 Hz DC blocker and a ~10 kHz lowpass in the loop, glided time
struct Delay
{
    float fs = 44100, hpA = 0, hpB = 0, lpA = 0, lpA2 = 0, glideA = 0, glideB = 0, timeS = 0;
    int maxD = 0;
    unsigned w = 0;
    struct Ch { std::vector<float> ring = std::vector<float> (1u << 19, 0.0f); float prevIn = 0, hp = 0, lp = 0, out = 0; } ch[2];
    void prepare (float rate)
    {
        fs = std::clamp (rate, 1.0f, 192000.0f);
        const float t = tanF (31490.5234f / fs);
        lpA = t / (t + 1.0f); lpA2 = lpA + lpA;
        const float w0 = 62.831852f / fs;
        hpA = 1.0f / (w0 + 1.0f); hpB = 1.0f - w0;
        maxD = 1 - (int) (fs * -2.0f);
        glideA = expF (-4.0f / fs);
        glideB = (1.0f - glideA) * fs * 0.001f;
    }
    void run (float* l, float* r, int n, float mix, float ms, float feedbackPct)
    {
        const float fb = feedbackPct * 0.01f, dry = 1.0f - mix;
        float* io[2] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            timeS = timeS * glideA + ms * glideB;
            const int whole = (int) timeS;
            const int i0 = std::min (std::max (whole, 0), maxD), i1 = std::min (std::max (whole + 1, 0), maxD);
            const float fl = std::floor (timeS), frac = timeS - fl, rest = (1.0f - timeS) + fl;
            for (int k = 0; k < 2; ++k)
            {
                Ch& s = ch[k];
                const float x = io[k][i];
                const float in = fb * s.out + x;
                s.hp = (s.hp * hpB + (in - s.prevIn)) * hpA;
                s.prevIn = in;
                const float v = s.hp - s.lp;
                s.ring[w & 0x7ffff] = v * lpA + s.lp;
                s.lp = v * lpA2 + s.lp;
                s.out = frac * s.ring[(w - (unsigned) i1) & 0x7ffff] + rest * s.ring[(w - (unsigned) i0) & 0x7ffff];
                io[k][i] = mix * s.out + x * dry;
            }
            ++w;
        }
    }
};

// ---- Diffusion: 16 modulated allpass stages per channel (normalized lattice, coefficient g glided). Each stage's
// delay is its base (a fraction of a second times the Delay control) plus a random offset; a stage takes a new delay
// only at a zero crossing of its output, and crossfades between two taps so the change is click-free.
struct Diffuser
{
    static constexpr int stages = 16;
    // per stage: base delay in seconds (× the Delay control), its jitter scale (seconds per LCG unit)
    static constexpr float baseS[stages] = { 0.270204991f, 0.197686195f, 0.144630313f, 0.105813801f, 0.0774150267f,
        0.0566380434f, 0.0414372794f, 0.030316161f, 0.0221797768f, 0.0162270721f, 0.0118719786f, 0.00868572574f,
        0.0063546123f, 0.00464913389f, 0.00340137887f, 0.00248850207f };
    static constexpr float jitS[stages] = { 1.25824004e-10f, 9.20548082e-11f, 6.73487377e-11f, 4.92733909e-11f,
        3.6049181e-11f, 2.63741442e-11f, 1.92957369e-11f, 1.41170626e-11f, 1.03282634e-11f, 7.55631929e-12f,
        5.52832086e-12f, 4.04460623e-12f, 2.95909694e-12f, 2.16492167e-12f, 1.58389046e-12f, 1.15879908e-12f };
    static constexpr float maxDelayControl = 10.0f; // FL's control range is 0.1…10

    struct Stage
    {
        std::vector<float> ring;
        unsigned mask = 0;
        float d = 0, yPrev = 0, lastNz = 0, held = 0, tapA = 0, tapB = 0;
        int cnt = 0, cntPrev = 0;
    };
    float fs = 44100, c = 0, keep = 1, g = 0, fade = 0, rate = 0;
    int lcg = 0;
    unsigned w = 0;
    float base[stages] {}, jit[stages] {};
    Stage st[2][stages];

    void prepare (float rate_)
    {
        fs = std::clamp (rate_, 1.0f, 192000.0f);
        c = 44.0999985f / fs;
        keep = 1.0f - c;
        for (int k = 0; k < stages; ++k)
        {
            base[k] = fs * baseS[k];
            jit[k] = fs * jitS[k];
            const auto need = (std::size_t) (base[k] * maxDelayControl) + 4802;
            std::size_t size = 1;
            while (size < need) size <<= 1;
            for (auto& ch : st)
            {
                ch[k] = Stage {};
                ch[k].ring.assign (size, 0.0f);
                ch[k].mask = (unsigned) size - 1;
            }
        }
        g = fade = rate = 0;
        lcg = 0;
        w = 0;
    }
    void run (float* l, float* r, int n, float mix, float feedback, float delayControl)
    {
        const float dc = std::max (std::min (delayControl, maxDelayControl), 0.00999999978f);
        float amp = (2.0f - dc) * 0.025f;
        if (amp <= 0.0f) amp = 0.0f;
        amp *= dc;
        float b[stages], j[stages];
        for (int k = 0; k < stages; ++k) { b[k] = dc * base[k]; j[k] = amp * jit[k]; }
        const float target = feedback * c, dry = 1.0f - mix;
        for (int i = 0; i < n; ++i)
        {
            g = g * keep + target;
            const float cs = std::sqrt (1.0f - g * g);
            const float r5 = std::max (std::min (1.0f - g * 5.0f, 1.0f), 0.0f);
            const float fresh = 1.0f / (r5 * 112.0f + 16.0f);
            rate = fade == 0.0f ? fresh : fade == 1.0f ? -fresh : rate;
            fade = std::max (std::min (fade + rate, 1.0f), 0.0f);
            const float a = 1.0f - fade;
            lcg = (int) ((unsigned) lcg * 1103515245u + 12345u);
            const float rnd = (float) lcg;
            float cand[stages];
            for (int k = 0; k < stages; ++k)
                cand[k] = std::max (std::min (rnd * j[k], 4800.0f), -4800.0f) + b[k];
            for (int ch = 0; ch < 2; ++ch)
            {
                float* io = ch == 0 ? l : r;
                const float in = io[i];
                float x = in;
                for (int k = 0; k < stages; ++k)
                {
                    Stage& s = st[ch][k];
                    s.ring[w & s.mask] = x * cs - g * s.d;
                    if (s.cntPrev > s.cnt) s.held = cand[k];   // the stage's counter restarted: take the new delay
                    if (fade == 1.0f) s.tapA = s.held - 1.0f;
                    if (fade == 0.0f) s.tapB = s.held - 1.0f;
                    const int ta = (int) std::min (s.tapA > 0.0f ? s.tapA : 0.0f, 1920000.0f);
                    const int tb = (int) std::min (s.tapB > 0.0f ? s.tapB : 0.0f, 1920000.0f);
                    const float read = a * s.ring[(w - (unsigned) ta) & s.mask] + fade * s.ring[(w - (unsigned) tb) & s.mask];
                    const float y = g * x + cs * s.d;
                    const float nz = s.yPrev != 0.0f ? s.yPrev : s.lastNz;
                    const int next = (s.lastNz * nz >= 0.0f || (float) s.cnt < b[k]) ? s.cnt + 1 : 0;
                    s.lastNz = nz;
                    s.yPrev = y;
                    s.cntPrev = s.cnt;
                    s.cnt = next;
                    s.d = read;
                    x = y;
                }
                io[i] = in * dry + x * mix;
            }
            ++w;
        }
    }
};

// ---- Bounce: 32 taps per channel like a bouncing ball. Tap t (left: odd 64ths, right: even 64ths up to 1) sits at
// 2 s · (1 − e^(k·t)) / (1 − e^k), k = −10 · Delay Curve, and has the gain (1 − |a|) + |a| · c(t) (c(1 − t) for a > 0),
// c(t) = (e^(−10t) − e^(−10)) / (1 − e^(−10)), a = 0.95 · Amplitude Curve (glided). Taps change through a 50 ms
// crossfade.
struct Bounce
{
    static constexpr int taps = 32;
    float fs = 44100, c = 0, keep = 1, step = 0, mixG = 0, ampG = 0, fade = 0, rate = 0;
    unsigned w = 0, mask = 0;
    float gainNeg[2][taps] {}, gainPos[2][taps] {}, tt[2][taps] {};
    struct Ch { std::vector<float> ring; float tapA[taps] {}, tapB[taps] {}; } ch[2];

    void prepare (float rate_)
    {
        fs = std::clamp (rate_, 1.0f, 192000.0f);
        const float inv = 1.0f / fs;
        c = inv * 44.0999985f;
        keep = 1.0f - c;
        step = inv * 20.0f;
        std::size_t size = 1;
        while (size < (std::size_t) (fs * 2.0f) + 2) size <<= 1;
        mask = (unsigned) size - 1;
        const double e10 = std::exp (-10.0);
        auto curveC = [e10] (double t) { return (float) ((std::exp (-10.0 * t) - e10) / (1.0 - e10)); };
        for (int k = 0; k < 2; ++k)
        {
            ch[k] = Ch {};
            ch[k].ring.assign (size, 0.0f);
            for (int j = 0; j < taps; ++j)
            {
                const int n64 = k == 0 ? 2 * j + 1 : 2 * j + 2;
                tt[k][j] = (float) n64 / 64.0f;
                gainNeg[k][j] = n64 == 64 ? 0.0f : curveC (n64 / 64.0);
                gainPos[k][j] = n64 == 64 ? 1.0f : curveC (1.0 - n64 / 64.0);
            }
        }
        mixG = ampG = fade = rate = 0;
        w = 0;
    }
    void run (float* l, float* r, int n, float mix, float ampCurve, float delayCurve)
    {
        // the tap positions (seconds), from the delay curve
        float k = delayCurve * -10.0f;
        if (k <= 0.0f) { if (k >= -9.99999997e-07f) k = -9.99999997e-07f; }
        else if (k <= 9.99999997e-07f) k = 9.99999997e-07f;
        const float norm = 1.0f / (1.0f - expF (k));
        float pos[2][taps];
        for (int q = 0; q < 2; ++q)
            for (int j = 0; j < taps; ++j)
            {
                if (tt[q][j] == 1.0f) { pos[q][j] = 2.0f; continue; }
                const float v = (1.0f - expF (k * tt[q][j])) * norm;
                pos[q][j] = v + v;
            }
        const float a = ampCurve * 0.949999988f, cm = c * mix;
        float* io[2] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            mixG = keep * mixG + cm;
            ampG = keep * ampG + c * a;
            const float mag = std::fabs (ampG);
            rate = fade == 0.0f ? step : fade == 1.0f ? -step : rate;
            fade = std::max (std::min (fade + rate, 1.0f), 0.0f);
            const float rest = 1.0f - fade;
            for (int q = 0; q < 2; ++q)
            {
                Ch& s = ch[q];
                const float x = io[q][i];
                s.ring[w & mask] = x;
                float sum = 0.0f;
                for (int j = 0; j < taps; ++j)
                {
                    const float d = fs * pos[q][j];
                    if (fade == 1.0f) s.tapA[j] = d;
                    if (fade == 0.0f) s.tapB[j] = d;
                    const int ta = (int) std::min (s.tapA[j] > 0.0f ? s.tapA[j] : 0.0f, 1920000.0f);
                    const int tb = (int) std::min (s.tapB[j] > 0.0f ? s.tapB[j] : 0.0f, 1920000.0f);
                    const float g = ampG <= 0.0f ? mag * gainNeg[q][j] + ampG + 1.0f : mag * gainPos[q][j] + (1.0f - ampG);
                    sum += (fade * s.ring[(w - (unsigned) tb) & mask] + rest * s.ring[(w - (unsigned) ta) & mask]) * g;
                }
                io[q][i] = sum * mixG + (1.0f - mixG) * x;
            }
            ++w;
        }
    }
};

float dbToGain (float db) { return expF (db * 0.115129255f); }
} // namespace

// ---- the front end's mapping (REA §1)
Range controlRange (int k)
{
    if (k < lookahead) return { 0, 10000 };
    if (k == mode) return { 0, 2 };
    if (k == transientEffect || k == sustainEffect) return { 0, 5 };
    return { 0, 1 };
}

int rawFromParam (int k, float v)
{
    const auto r = controlRange (k);
    return r.lo + (int) std::lround (std::clamp (v, 0.0f, 1.0f) * (float) (r.hi - r.lo));
}

float paramFromRaw (int k, int raw)
{
    const auto r = controlRange (k);
    return (float) (std::clamp (raw, r.lo, r.hi) - r.lo) / (float) (r.hi - r.lo);
}

namespace
{
double curve (double x, double lo, double hi, double c) { return x <= 0 ? lo : lo + (hi - lo) * std::exp (std::log (x) / c); }
} // namespace

float engineValue (int k, int raw)
{
    if (k >= lookahead)
        return (float) raw;
    const double x = std::clamp (raw, 0, 10000) * 0.0001;
    switch (k)
    {
        case hold: case release: return (float) curve (x, 1, 250, 1);
        case transientLevel: case sustainLevel: return (float) curve (x, -120, 12, 4);
        case transientTilt: case sustainTilt: return (float) curve (x, -6, 6, 1);
        case tDiffFeedback: case sDiffFeedback: return (float) curve (x, 0, 0.99, 1);
        case tDiffDelay: case sDiffDelay: return (float) std::pow (10.0, x * 2 - 1);
        case tSatDrive: case sSatDrive: return (float) curve (x, 0, 18, 1);
        case tBounceAmpCurve: case tBounceDelayCurve: case sBounceAmpCurve: case sBounceDelayCurve: return (float) curve (x, -1, 1, 1);
        case tDelayTime: case sDelayTime: return (float) curve (x, 0.001, 2, 0.5);
        case tDelaySynced: case sDelaySynced: return (float) (x * 16);
        default: return (float) x;
    }
}

std::array<float, count> defaults()
{
    // FL's Default preset
    static const int raw[count] = { 7475, 1968, 1968, 8743, 8743, 5000, 5000, 10000, 9112, 0, 5000, 5051, 5000, 10000, 3333,
                                    0, 10000, 3342, 0, 10000, 5320, 5275, 10000, 4443, 4642, 10000, 3531, 144, 0, 10000, 3156,
                                    240, 0, 1, 1, 1, 1, 1, 0, 2, 0, 0 };
    std::array<float, count> p {};
    for (int k = 0; k < count; ++k)
        p[(std::size_t) k] = paramFromRaw (k, raw[k]);
    return p;
}

std::string paramText (int k, float v)
{
    const int raw = rawFromParam (k, v);
    const float e = engineValue (k, raw);
    char buf[64];
    static const char* sections[6] = { "Saturation", "Bounce", "Diffusion", "Blur", "Delay", "Off" };
    static const char* modes[3] = { "Smooth", "Balanced", "Dense" };
    switch (k)
    {
        case hold: case release: std::snprintf (buf, sizeof buf, "%.1f ms", e); break;
        case transientLevel: case sustainLevel: case transientTilt: case sustainTilt: case tSatDrive: case sSatDrive:
            std::snprintf (buf, sizeof buf, "%.1f dB", e); break;
        case tDiffDelay: case sDiffDelay: std::snprintf (buf, sizeof buf, "%.2f s", e); break;
        case tDelayTime: case sDelayTime: std::snprintf (buf, sizeof buf, "%.3f s", e); break;
        case tDelaySynced: case sDelaySynced: std::snprintf (buf, sizeof buf, "%.2f steps", e); break;
        case mode: return modes[std::clamp (raw, 0, 2)];
        case transientEffect: case sustainEffect: return sections[std::clamp (raw, 0, 5)];
        case lookahead: case stereoLink: case transientOn: case sustainOn: case tDelaySync: case sDelaySync:
            return raw != 0 ? "On" : "Off";
        default: std::snprintf (buf, sizeof buf, "%d%%", (int) std::lround (e * 100)); break;
    }
    return buf;
}

std::optional<State> parse (const std::vector<std::uint8_t>& data)
{
    if (data.size() < 4 + 4 * count)
        return std::nullopt;
    auto rd = [&] (std::size_t i) { std::int32_t v; std::memcpy (&v, data.data() + i, 4); return (int) v; };
    const int version = rd (0);
    if (version < 2 || version > 3)
        return std::nullopt;
    State s;
    for (int k = 0; k < count; ++k)
        s.raw[(std::size_t) k] = std::clamp (rd (4 + 4 * (std::size_t) k), controlRange (k).lo, controlRange (k).hi);
    if (data.size() >= 4 + 4 * count + 12)
    {
        s.transientSend = rd (4 + 4 * count);
        s.sustainSend = rd (8 + 4 * count);
        s.sidechain = rd (12 + 4 * count);
    }
    return s;
}

// ---- the engine
struct Engine::Impl
{
    float fs = 44100, bpm = 120;
    std::array<float, count> v {};   // engine values
    Detector det;
    Lookahead la[2];
    Tilt tilt[2];
    Saturation sat[2];
    Delay delay[2];
    Diffuser diff[2];
    Bounce bounce[2];
    float glideC = 0, gT = 0, gS = 0, onT = 1, onS = 1; // FL starts the level gains at 0 (they glide up)
    std::vector<float> buf[6];

    float delayMs (int part) const
    {
        const int sync = part == 0 ? tDelaySync : sDelaySync;
        if (v[(std::size_t) sync] > 0.5f)
            return 15000.0f / bpm * v[(std::size_t) (part == 0 ? tDelaySynced : sDelaySynced)];
        return v[(std::size_t) (part == 0 ? tDelayTime : sDelayTime)] * 1000.0f;
    }
};

Engine::Engine() : impl (std::make_unique<Impl>())
{
    const auto d = defaults();
    setParams (d.data());
    prepare (44100);
}
Engine::~Engine() = default;

void Engine::prepare (double sampleRate)
{
    auto& m = *impl;
    m = Impl { .fs = (float) sampleRate, .bpm = m.bpm, .v = m.v };
    m.det.prepare (m.fs);
    for (int c = 0; c < 2; ++c)
    {
        m.la[c].prepare (m.fs);
        m.tilt[c].fs = std::clamp (m.fs, 1.0f, 192000.0f);
        m.sat[c].prepare (m.fs);
        m.delay[c].prepare (m.fs);
        m.diff[c].prepare (m.fs);
        m.bounce[c].prepare (m.fs);
    }
    const float w = (float) ((double) (1.0f / m.fs) * 125.66370614359172);
    m.glideC = 1.0f / ((w * 0.5f + 1.0f) * w + 1.0f);
}

void Engine::setTempo (double bpm) { impl->bpm = (float) std::max (10.0, bpm); }

void Engine::setRaw (int k, int raw) { setValue (k, engineValue (k, raw)); }

void Engine::setValue (int k, float value)
{
    if (k >= 0 && k < count)
        impl->v[(std::size_t) k] = value;
}

void Engine::setParams (const float* params)
{
    for (int k = 0; k < count; ++k)
        setRaw (k, rawFromParam (k, params[k]));
}

int Engine::latency() const
{
    return impl->v[lookahead] > 0.5f ? (int) std::lround (impl->fs * 0.002f) : 0;
}

void Engine::process (const float* inL, const float* inR, float* outL, float* outR, int n, bool sidechain,
                      const float* scL, const float* scR)
{
    // FL's front end renders with flush-to-zero / denormals-are-zero; the Diffusion's zero-crossing logic depends on it
    const ScopedFlushDenormals ftz;
    auto& m = *impl;
    for (auto& b : m.buf)
        if ((int) b.size() < n)
            b.resize ((std::size_t) n);
    float *tl = m.buf[0].data(), *tr = m.buf[1].data(), *sl = m.buf[2].data(), *sr = m.buf[3].data();
    const auto& v = m.v;
    const float la = v[lookahead] > 0.5f ? 0.002f : 0.0f;
    m.det.setMode ((int) std::round (v[mode]));
    const float sens = 1.0f - v[sensitivity];
    const float sensFactor = expF (sens * sens * 4.0f);
    const float hold = v[Param::hold] * 0.001f, rel = v[Param::release] * 0.001f;
    for (int i = 0; i < n; ++i)
    {
        const float d = ! sidechain ? (inL[i] + inR[i]) * 0.5f : scL != nullptr && scR != nullptr ? (scL[i] + scR[i]) * 0.5f : 0.0f;
        const float e = m.det.run (d, sensFactor, la, hold, rel);
        const float xl = m.la[0].run (inL[i], la), xr = m.la[1].run (inR[i], la);
        tl[i] = xl * e; tr[i] = xr * e;
        sl[i] = xl * (1.0f - e); sr[i] = xr * (1.0f - e);
    }
    m.tilt[0].run (tl, tr, n, v[transientTilt]);
    m.tilt[1].run (sl, sr, n, v[sustainTilt]);
    for (int part = 0; part < 2; ++part)
    {
        float* l = part == 0 ? tl : sl;
        float* r = part == 0 ? tr : sr;
        const int o = part == 0 ? 0 : 3;   // the sustain's controls follow the transient's by 3 (4 for Bounce / Delay)
        switch ((int) v[(std::size_t) (part == 0 ? transientEffect : sustainEffect)])
        {
            case saturation: m.sat[part].run (l, r, n, v[(std::size_t) (tSatMix + o)], v[(std::size_t) (tSatDrive + o)], v[(std::size_t) (tSatWarmth + o)]); break;
            case delay:
            {
                const int d = part == 0 ? 0 : 4;
                m.delay[part].run (l, r, n, v[(std::size_t) (tDelayMix + d)], m.delayMs (part), v[(std::size_t) (tDelayFeedback + d)] * 100.0f);
                break;
            }
            case diffusion:
                m.diff[part].run (l, r, n, v[(std::size_t) (tDiffMix + o)], v[(std::size_t) (tDiffFeedback + o)], v[(std::size_t) (tDiffDelay + o)]);
                break;
            case bounce:
            {
                const int d = part == 0 ? 0 : 3;
                m.bounce[part].run (l, r, n, v[(std::size_t) (tBounceMix + d)], v[(std::size_t) (tBounceAmpCurve + d)], v[(std::size_t) (tBounceDelayCurve + d)]);
                break;
            }
            default: break; // Blur and Off: the engine passes the part
        }
    }
    const float targetT = dbToGain (v[transientLevel]), targetS = dbToGain (v[sustainLevel]);
    const float wantT = v[transientOn], wantS = v[sustainOn];
    const float c = m.glideC;
    for (int i = 0; i < n; ++i)
    {
        m.gT = (m.gT - targetT) * c + targetT;
        m.onT = (m.onT - wantT) * c + wantT;
        m.onS = (m.onS - wantS) * c + wantS;
        m.gS = (m.gS - targetS) * c + targetS;
        const float l = m.gT * tl[i] * m.onT + m.gS * sl[i] * m.onS;
        const float r = m.gT * tr[i] * m.onT + m.gS * sr[i] * m.onS;
        outL[i] = l;
        outR[i] = r;
    }
}

namespace
{
class TransmitterEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override { rate = sampleRate; engine.prepare (sampleRate); first = true; }
    void reset() noexcept override { engine.prepare (rate); first = true; }
    void process (float* left, float* right, int n, const EffectParams& params, double bpm) noexcept override
    {
        if (bpm > 0 && bpm != lastBpm)
        {
            lastBpm = bpm;
            engine.setTempo (bpm);
        }
        for (int k = 0; k < count; ++k)
            if (first || params[(std::size_t) k] != last[(std::size_t) k])
            {
                last[(std::size_t) k] = params[(std::size_t) k];
                engine.setRaw (k, rawFromParam (k, params[(std::size_t) k]));
            }
        first = false;
        engine.process (left, right, left, right, n, hasSide, sideL, sideR);
        hasSide = false;
        sideL = sideR = nullptr;
    }
    // the slot's sidechain (FL's sidechain input feeds the detection), set before each process() when the slot has one
    void setSidechain (const float* l, const float* r) noexcept override
    {
        hasSide = true;
        sideL = l;
        sideR = r;
    }
    int latencySamples() const noexcept override { return engine.latency(); }

private:
    Engine engine;
    std::array<float, count> last {};
    double rate = 44100, lastBpm = 0;
    bool first = true, hasSide = false;
    const float *sideL = nullptr, *sideR = nullptr;
};
} // namespace

std::unique_ptr<Effect> make() { return std::make_unique<TransmitterEffect>(); }
} // namespace lynx::transmitter
