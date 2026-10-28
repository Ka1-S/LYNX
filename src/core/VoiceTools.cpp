#include "core/VoiceTools.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/Biquad.h"
#include "core/Effects.h"
#include "core/LrCrossover.h"

// REA_rodada119. Literature behind the two engines:
//  - Linkwitz-Riley crossovers and the all-pass compensation that keeps a band split phase-coherent:
//    S. Linkwitz, "Active Crossover Networks for Noncoincident Drivers", JAES 24(1), 1976; the tree / all-pass
//    arrangement as in U. Zölzer (ed.), "DAFX", 2nd ed., ch. 2 (filters) and ch. 4 (dynamics);
//  - multiband downward expansion as a broadband noise reducer (the classic "noise gate per band" scheme of tape
//    noise reduction units and single-ended hiss reducers), with a soft-knee gain computer and log-domain smoothing:
//    D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor Design — A Tutorial and Analysis",
//    JAES 60(6), 2012 (the same computer, below the threshold);
//  - delay-line pitch shifting with two crossfaded read taps (the "rotating tape head" / Doppler shifter):
//    DAFX ch. 6 (time-segment processing) and J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and
//    Chorus", JAES 45(10), 1997; doubling by short, slightly detuned delays: the same paper (doubler / chorus);
//  - shelving filters: R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients".

namespace lynx::voicetools
{
namespace
{
constexpr double pi = 3.141592653589793;
constexpr double twoPi = 2.0 * pi;
inline double clamp01 (float v) noexcept { return (double) std::clamp (v, 0.0f, 1.0f); }
inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
inline bool on (float v) noexcept { return v >= 0.5f; }
inline double coef (double ms, double sr) noexcept { return ms <= 0.0 ? 0.0 : std::exp (-1000.0 / (ms * sr)); }

std::string fmt (const char* f, double v)
{
    char b[64];
    std::snprintf (b, sizeof b, f, v);
    return b;
}
std::string hzText (double f) { return f >= 1000.0 ? fmt ("%.2f kHz", f / 1000.0) : fmt ("%.0f Hz", f); }
} // namespace

// =====================================================================================================================
// Quell Denoiser
// =====================================================================================================================
namespace quell
{
double outputDb (float v) noexcept { return -12.0 + 24.0 * clamp01 (v); }

const Tuning& defaultTuning() noexcept
{
    // REA_rodada119 §3: fitted to the reference's band gains on three vocal takes (raw and −12 dB) at Amount 23, 40
    // and 70 % plus sine stairs and tone bursts (tmp/w119/py/qfit.py)
    static const Tuning t = []
    {
        Tuning u;
        const double thr0[bands] { -34.00, -52.00, -43.50, -47.50, -51.00, -65.00 };
        const double thr1[bands] { -21.00, -42.50, -39.00, -40.00, -42.00, -48.50 };
        const double r0[bands] { 8.00, 27.00, 29.00, 26.00, 29.00, 13.50 };
        const double r1[bands] { 25.00, 26.00, 27.50, 26.00, 26.00, 17.50 };
        const double c0[bands] { 0.00, 0.00, 0.00, 0.50, 0.50, 2.30 };
        const double c1[bands] { 1.50, -0.75, 0.25, 0.00, 0.50, 3.20 };
        for (int b = 0; b < bands; ++b)
        {
            u.thr0[b] = thr0[b];
            u.thr1[b] = thr1[b];
            u.range0[b] = r0[b];
            u.range1[b] = r1[b];
            u.floorCut0[b] = c0[b];
            u.floorCut1[b] = c1[b];
        }
        u.slope = 1.40;
        u.knee = 19.00;
        u.detAttackMs = 2.00;
        u.detReleaseMs = 5.00;
        u.openMs = 2.00;
        u.closeMs = 45.00;
        return u;
    }();
    return t;
}

double reductionDb (const Tuning& t, int b, double levelDb, double amount) noexcept
{
    b = std::clamp (b, 0, bands - 1);
    amount = std::clamp (amount, 0.0, 1.0);
    const auto thr = t.thr0[b] + (t.thr1[b] - t.thr0[b]) * amount;
    const auto range = t.range0[b] + (t.range1[b] - t.range0[b]) * amount;
    const auto cut = t.floorCut0[b] + (t.floorCut1[b] - t.floorCut0[b]) * amount;
    // soft-knee downward expander: below the knee the reduction grows by `slope` dB per dB
    const auto under = thr - levelDb, half = 0.5 * t.knee;
    double r;
    if (under <= -half)
        r = 0.0;
    else if (under >= half || t.knee <= 0.0)
        r = t.slope * under;
    else
        r = t.slope * (under + half) * (under + half) / (2.0 * t.knee);
    return std::min (range, std::max (0.0, r)) + cut;
}

namespace
{
// ---- the matched engine (round 124) ---------------------------------------------------------------------------------
// [EXEC] black-box measurements of the official NS1 trial (docs/analise/REA_rodada124.md §6), our own code:
// - at amount 0 its output is the input through two all-passes: RBJ 60.07 Hz Q 1.0 and 16 kHz Q 0.90 (0.08° RMS);
// - the bands' gains act on top of that without adding latency: the band below 60 Hz and the band above 16 kHz as
//   gain-reduced parts of the split, the middle as a cascade of peaking cuts (0 dB = exactly transparent);
// - each band: linked power detector, a downward expander (threshold / range move with the amount, soft knee), one
//   loud band also opens its neighbours (spread), fast opening, slower closing; static shelves by amount.
// The parameters were fitted on her takes and pink noise at her four NS1 settings (tmp/vm124/ns/nsmodel.cpp).
// Not reproduced: the reference's slow adaptation to steady noise (seconds), which mostly matters on test noise.
namespace fitted
{
struct Fit
{
    int bands = 7;
    double f0 = 173.0000, step = 1.0, q = 1.2300;
    double t0 = -39.9100, t1 = -30.8000, r0 = 182.1000, r1 = -57.0900; // range clamped to 15 … 60 dB
    double lowCut0 = -1.1780, lowCut1 = 16.0600, highCut0 = 2.3640, highCut1 = 4.0990;
    double slope = 0.4462, knee = 39.1100, spread = 10.2100, detA = 0.6090, detR = 2.9100, open = 3.0050, close = 26.7000;
    double tOff[9] { -0.3251, -0.0701, 0.2107, 0.2187, 0.1810, -0.0680, -0.1600, 0.0210, -0.6890 };
    double lsF = 68.550, lsG0 = -0.0337, lsG1 = 0.0399, hsF = 9000.0, hsG0 = -3.0, hsG1 = -3.0; // the static HF slope, the same for every amount (measured 0.15 … 0.8)
};
const Fit fit;

struct Bq
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] {}, z2[2] {};
    void set (double B0, double B1, double B2, double A0, double A1, double A2) noexcept { b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; }
    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    double run (double x, int c) noexcept
    {
        const auto y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
    void ap (double sr, double fc, double qq) noexcept { const auto w = twoPi * fc / sr, a = std::sin (w) / (2 * qq), c = std::cos (w); set (1 - a, -2 * c, 1 + a, 1 + a, -2 * c, 1 - a); }
    void lp (double sr, double fc, double qq) noexcept { const auto w = twoPi * fc / sr, a = std::sin (w) / (2 * qq), c = std::cos (w); set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a); }
    void bp (double sr, double fc, double qq) noexcept { const auto w = twoPi * fc / sr, a = std::sin (w) / (2 * qq), c = std::cos (w); set (a, 0, -a, 1 + a, -2 * c, 1 - a); }
    void shelf (double sr, double fc, double g, bool high) noexcept
    {
        const auto A = std::pow (10.0, g / 40.0), w = twoPi * fc / sr, cw = std::cos (w), al = std::sin (w) / (2 * 0.7071), sq = 2 * std::sqrt (A) * al;
        if (high)
            set (A * ((A + 1) + (A - 1) * cw + sq), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sq), (A + 1) - (A - 1) * cw + sq, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sq);
        else
            set (A * ((A + 1) - (A - 1) * cw + sq), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sq), (A + 1) + (A - 1) * cw + sq, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sq);
    }
};

class Engine
{
public:
    static constexpr int maxBands = 7;
    void prepare (double rate) noexcept
    {
        sr = rate;
        ap1.ap (sr, 60.0658, 0.9998);
        ap2.ap (sr, 16000.0, 0.8992);
        lpA.lp (sr, 60.0658, 0.9998);
        lpB = lpA;
        ap2low.ap (sr, 16000.0, 0.8992);
        lp2A.lp (sr, 16000.0, 0.8992);
        lp2B = lp2A;
        for (int b = 0; b < fit.bands; ++b)
            detector[(std::size_t) b].bp (sr, centre (b), fit.q);
        designedAmount = -1.0;
        reset();
    }
    void reset() noexcept
    {
        for (auto* f : { &ap1, &ap2, &lpA, &lpB, &ap2low, &lp2A, &lp2B, &lsh, &hsh })
            f->reset();
        for (auto& f : detector)
            f.reset();
        for (auto& f : peaking)
            f.reset();
        env.fill (0.0);
        gdb.fill (0.0);
    }
    // in place; returns the mean reduction (dB) of the block for the meter
    double process (float* l, float* r, int n, double amt, bool listenOnly, float out) noexcept
    {
        const int nb = fit.bands + 2;
        if (amt != designedAmount)
        {
            designedAmount = amt;
            for (int b = 0; b < nb; ++b)
            {
                threshold[(std::size_t) b] = fit.t0 + (fit.t1 - fit.t0) * amt + fit.tOff[b];
                range[(std::size_t) b] = std::clamp (fit.r0 + (fit.r1 - fit.r0) * amt, 15.0, 60.0);
                staticCut[(std::size_t) b] = b == 0 ? fit.lowCut0 + (fit.lowCut1 - fit.lowCut0) * amt
                                           : (b == nb - 1 ? fit.highCut0 + (fit.highCut1 - fit.highCut0) * amt : 0.0);
            }
            lsh.shelf (sr, fit.lsF, fit.lsG0 + (fit.lsG1 - fit.lsG0) * amt, false);
            hsh.shelf (sr, fit.hsF, fit.hsG0 + (fit.hsG1 - fit.hsG0) * amt, true);
        }
        const auto ea = coef (fit.detA, sr), er = coef (fit.detR, sr), go = coef (fit.open, sr), gc = coef (fit.close, sr);
        double sumRed = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double xs[2] { l[i], r[i] };
            double a[2], low[2], high[2], bandSig[maxBands][2];
            for (int c = 0; c < 2; ++c)
            {
                const auto t = ap1.run (xs[c], c);
                a[c] = ap2.run (t, c);
                low[c] = ap2low.run (lpB.run (lpA.run (xs[c], c), c), c);
                high[c] = a[c] - lp2B.run (lp2A.run (t, c), c);
                for (int b = 0; b < fit.bands; ++b)
                    bandSig[b][c] = detector[(std::size_t) b].run (a[c], c);
            }
            double lev[maxBands + 2];
            const auto pw = [] (double u0, double u1) { return 0.5 * (u0 * u0 + u1 * u1); };
            for (int b = 0; b < nb; ++b)
            {
                const auto p = b == 0 ? pw (low[0], low[1]) : (b == nb - 1 ? pw (high[0], high[1]) : pw (bandSig[b - 1][0], bandSig[b - 1][1]));
                auto& e = env[(std::size_t) b];
                e = p > e ? p + (e - p) * ea : p + (e - p) * er;
                lev[b] = 10.0 * std::log10 (e + 1e-30) + 3.0103;
            }
            for (int b = 0; b < nb; ++b)
            {
                auto level = lev[b];
                if (b >= 1 && b <= fit.bands)
                    for (int j = 1; j <= fit.bands; ++j)
                        if (j != b)
                            level = std::max (level, lev[j] - fit.spread * std::abs (j - b));
                const auto d = threshold[(std::size_t) b] - level;
                double red;
                if (d <= -fit.knee / 2)
                    red = 0.0;
                else if (d >= fit.knee / 2)
                    red = fit.slope * d;
                else
                {
                    const auto u = d + fit.knee / 2;
                    red = fit.slope * u * u / (2 * fit.knee);
                }
                const auto target = -(std::min (range[(std::size_t) b], red) + staticCut[(std::size_t) b]);
                auto& g = gdb[(std::size_t) b];
                g = target > g ? target + (g - target) * go : target + (g - target) * gc;
            }
            for (int b = 0; b < fit.bands; ++b)
            {
                const auto w = twoPi * centre (b) / sr, al = std::sin (w) / (2 * fit.q), cw = std::cos (w), A = std::pow (10.0, gdb[(std::size_t) b + 1] / 40.0);
                peaking[(std::size_t) b].set (1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A);
            }
            const auto gl = std::pow (10.0, gdb[0] / 20.0), gh = std::pow (10.0, gdb[(std::size_t) nb - 1] / 20.0);
            for (int c = 0; c < 2; ++c)
            {
                auto o = a[c] - (1.0 - gl) * low[c] - (1.0 - gh) * high[c];
                for (int b = 0; b < fit.bands; ++b)
                    o = peaking[(std::size_t) b].run (o, c);
                o = hsh.run (lsh.run (o, c), c);
                // Listen: what is taken away (the open chain minus the processed one)
                const auto y = listenOnly ? a[c] - o : o;
                (c == 0 ? l[i] : r[i]) = (float) y * out;
            }
            double red = 0.0;
            for (int b = 0; b < nb; ++b)
                red -= gdb[(std::size_t) b];
            sumRed += red / nb;
        }
        return n > 0 ? sumRed / n : 0.0;
    }

private:
    double centre (int b) const noexcept { return fit.f0 * std::pow (2.0, b * fit.step); }
    double sr = 48000.0, designedAmount = -1.0;
    Bq ap1, ap2, lpA, lpB, ap2low, lp2A, lp2B, lsh, hsh;
    std::array<Bq, maxBands> detector, peaking;
    std::array<double, maxBands + 2> env {}, gdb {}, threshold {}, range {}, staticCut {};
};
} // namespace fitted

class Quell final : public Effect
{
public:
    explicit Quell (const Tuning& t) : tuning (t) {}

    void prepare (double sampleRate) noexcept override
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int k = 0; k < bands - 1; ++k)
        {
            splitters[(std::size_t) k].design (sr, split[k], 24);
            for (auto& a : comp[(std::size_t) k])
                a.design (sr, split[k], 24);
        }
        detA = coef (tuning.detAttackMs, sr);
        detR = coef (tuning.detReleaseMs, sr);
        openC = coef (tuning.openMs, sr);
        closeC = coef (tuning.closeMs, sr);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& s : splitters)
            s.reset();
        for (auto& row : comp)
            for (auto& a : row)
                a.reset();
        power.fill (0.0);
        gainDb.fill (0.0);
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        const auto amt = clamp01 (p[amount]);
        const bool listenOnly = on (p[listen]);
        const auto out = (float) dbToGain (outputDb (p[output]));
        if (on (p[matched]))
        {
            if (! matchedOn)
            {
                matchedEngine.prepare (sr);
                matchedOn = true;
            }
            const auto red = matchedEngine.process (l, r, n, amt, listenOnly, out);
            meter[0].store ((float) std::clamp (red / 30.0, 0.0, 1.0), std::memory_order_relaxed);
            return;
        }
        matchedOn = false;
        std::array<double, bands> sumRed {};
        for (int i = 0; i < n; ++i)
        {
            float band[2][bands];
            for (int c = 0; c < 2; ++c)
            {
                float rest = c == 0 ? l[i] : r[i];
                for (int k = 0; k < bands - 1; ++k)
                {
                    float lo, hi;
                    splitters[(std::size_t) k].run (rest, c, lo, hi);
                    // keep the low band in phase with the bands split after it (their splits' all-passes)
                    for (int j = k + 1; j < bands - 1; ++j)
                        lo = comp[(std::size_t) j][(std::size_t) k].allpass (lo, c);
                    band[c][k] = lo;
                    rest = hi;
                }
                band[c][bands - 1] = rest;
            }
            float yl = 0.0f, yr = 0.0f, dl = 0.0f, dr = 0.0f;
            for (int b = 0; b < bands; ++b)
            {
                const auto e = 0.5 * ((double) band[0][b] * band[0][b] + (double) band[1][b] * band[1][b]);
                auto& pw = power[(std::size_t) b];
                pw = e + (pw - e) * (e > pw ? detA : detR);
                const auto levelDb = 10.0 * std::log10 (2.0 * pw + 1e-30); // a sine reads its peak level
                const auto target = -reductionDb (tuning, b, levelDb, amt);
                auto& g = gainDb[(std::size_t) b];
                g = target + (g - target) * (target > g ? openC : closeC);
                sumRed[(std::size_t) b] += -g;
                const auto lin = (float) dbToGain (g);
                yl += lin * band[0][b];
                yr += lin * band[1][b];
                dl += (1.0f - lin) * band[0][b];
                dr += (1.0f - lin) * band[1][b];
            }
            l[i] = out * (listenOnly ? dl : yl);
            r[i] = out * (listenOnly ? dr : yr);
        }
        double mean = 0.0;
        for (int b = 0; b < bands; ++b)
        {
            const auto red = n > 0 ? sumRed[(std::size_t) b] / n : 0.0;
            meter[(std::size_t) b + 1].store ((float) std::clamp (red / 30.0, 0.0, 1.0), std::memory_order_relaxed);
            mean += red / bands;
        }
        meter[0].store ((float) std::clamp (mean / 30.0, 0.0, 1.0), std::memory_order_relaxed);
    }

    float controlOutput (int k) const noexcept override
    {
        return k >= 0 && k <= bands ? meter[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    Tuning tuning;
    double sr = 48000.0, detA = 0.0, detR = 0.0, openC = 0.0, closeC = 0.0;
    std::array<LrSplit, bands - 1> splitters;
    // comp[j][k]: split j's all-pass applied to band k (k < j)
    std::array<std::array<LrSplit, bands - 1>, bands - 1> comp;
    std::array<double, bands> power {}, gainDb {};
    std::array<std::atomic<float>, bands + 1> meter {};
    fitted::Engine matchedEngine;
    bool matchedOn = false;
};
} // namespace

std::unique_ptr<Effect> make (const Tuning& t) { return std::make_unique<Quell> (t); }
} // namespace quell

// =====================================================================================================================
// Gemini Doubler
// =====================================================================================================================
namespace gemini
{
double outputDb (float v) noexcept { return -24.0 + 24.0 * clamp01 (v); }
double gainDb (float v) noexcept { return -48.0 + 48.0 * clamp01 (v); }
double panDegrees (float v) noexcept { return -45.0 + 90.0 * clamp01 (v); }
double delayMs (float v) noexcept { return 100.0 * clamp01 (v); }
double feedback (float v) noexcept { return clamp01 (v); }
double detuneCents (float v) noexcept { return -100.0 + 200.0 * clamp01 (v); }
double depthCents (float v) noexcept { return -200.0 + 400.0 * clamp01 (v); }
double rateValue (float v) noexcept { return 0.1 * std::pow (4000.0, clamp01 (v)); }
double shelfDb (float v) noexcept { return -30.0 + 42.0 * clamp01 (v); }
double freqHz (float v) noexcept { return 16.0 * std::pow (21357.0 / 16.0, clamp01 (v)); }
float freqValue (double hz) noexcept { return (float) std::clamp (std::log (std::max (hz, 1.0) / 16.0) / std::log (21357.0 / 16.0), 0.0, 1.0); }
int sourceOf (float v) noexcept { return std::clamp ((int) std::floor (clamp01 (v) * 3.0), 0, 2); }
double windowMs (float rangeV) noexcept { return on (rangeV) ? 53.333 : 13.333; }

namespace
{
// mono delay line with linear-interpolated reads
struct Line
{
    std::vector<float> buf;
    int pos = 0;
    void allocate (int n) { buf.assign ((std::size_t) std::max (n, 4), 0.0f); pos = 0; }
    void clear() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
    void push (float x) noexcept
    {
        buf[(std::size_t) pos] = x;
        if (++pos == (int) buf.size())
            pos = 0;
    }
    // d samples ago, linearly interpolated (the measured reference's voices)
    float linear (double d) const noexcept
    {
        const auto size = (int) buf.size();
        d = std::clamp (d, 1.0, (double) size - 4.0);
        const auto di = (int) d;
        const auto f = (float) (d - di);
        auto i0 = pos - di;
        if (i0 < 0)
            i0 += size;
        auto i1 = i0 - 1;
        if (i1 < 0)
            i1 += size;
        return buf[(std::size_t) i0] + f * (buf[(std::size_t) i1] - buf[(std::size_t) i0]);
    }
    // d samples ago (d ≥ 1: the sample pushed last is 1 ago)
    float at (double d) const noexcept
    {
        const auto size = (int) buf.size();
        d = std::clamp (d, 1.0, (double) size - 4.0);
        const auto di = (int) d;
        const auto f = (float) (d - di);
        auto i0 = pos - di;
        if (i0 < 0)
            i0 += size;
        // 4-point Hermite between the samples d and d + 1 ago (keeps the top octave of a fractional delay)
        const auto at = [this, size] (int i) { i %= size; return buf[(std::size_t) (i < 0 ? i + size : i)]; };
        const auto xm1 = at (i0 + 1), x0 = at (i0), x1 = at (i0 - 1), x2 = at (i0 - 2);
        const auto c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

class Gemini final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        // longest read: 100 ms of delay + the 53 ms window (+ the octave's doubled sweep) + margin
        const auto n = (int) std::ceil (0.25 * sr) + 8;
        for (auto& v : voice)
            v.line.allocate (n);
        for (auto& d : direct)
            d.allocate ((int) std::ceil (0.03 * sr) + 8);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& v : voice)
        {
            v.line.clear();
            v.offset = 495.0;
            v.second = -1.0;
            v.phase = 0.0;
            v.lfo = 0.0;
            v.last = 0.0f;
            v.fbHp.reset();
        }
        for (auto& d : direct)
            d.clear();
        lowShelf.reset();
        highShelf.reset();
        designed = false;
    }

    void process (float* l, float* r, int n, const EffectParams& p, double) noexcept override
    {
        if (voice[0].line.buf.empty())
            return;
        design (p);
        // round 135w: RESET — a flip of the momentary value restarts every LFO at the same phase
        if (const auto r = p[modReset] >= 0.5f; r != lastReset)
        {
            lastReset = r;
            for (auto& v : voice)
                v.lfo = 0.0;
        }
        float peakIn = 0.0f, peakOut = 0.0f; // the editor's level meters (round 129: the reference's output meters)
        for (int i = 0; i < n; ++i)
            peakIn = std::max (peakIn, std::max (std::abs (l[i]), std::abs (r[i])));
        const auto master = (float) dbToGain (outputDb (p[output]));
        const bool dirOn = on (p[directOn]);
        const auto dirGain = dirOn ? (float) dbToGain (gainDb (p[directGain])) : 0.0f;
        const auto rot = -panDegrees (p[directPan]) * pi / 180.0; // −45°: L = (L + R)/√2, R = (R − L)/√2
        const auto rc = (float) std::cos (rot), rs = (float) std::sin (rot);
        const auto window = windowMs (p[range]) * 0.001 * sr;
        // [EXEC] measured: with Align the direct signal comes 6.67 ms late and every pass through a voice takes its
        // delay + 10.3 ms (the repeats too); without it a voice takes its delay alone
        const bool aligned = on (p[align]);
        // [EXEC] round 124: 320 samples at 44.1 and 48 kHz alike (a fixed sample count, not a time)
        const auto alignSamples = aligned ? 320.0 : 0.0;
        const auto voiceExtra = (aligned ? 0.0103 * sr : 0.0) - 0.5 * window;
        const bool lowOnV = on (p[lowOn]), highOnV = on (p[highOn]);

        struct VoiceSetup
        {
            bool active;
            float gl, gr, fb;
            double delay, cents, depth, lfoInc;
            bool octave;
            int source;
        } vs[voices];
        for (int k = 0; k < voices; ++k)
        {
            auto& s = vs[k];
            s.active = on (p[(std::size_t) voiceParam (k, vOn)]);
            const auto g = dbToGain (gainDb (p[(std::size_t) voiceParam (k, vGain)]));
            // equal-power pan over ±45°: −45 = left only, 0 = −3 dB on each side
            const auto a = (panDegrees (p[(std::size_t) voiceParam (k, vPan)]) + 45.0) * pi / 180.0;
            s.gl = (float) (g * std::cos (a));
            s.gr = (float) (g * std::sin (a));
            s.fb = (float) feedback (p[(std::size_t) voiceParam (k, vFeedback)]);
            s.delay = delayMs (p[(std::size_t) voiceParam (k, vDelay)]) * 0.001 * sr;
            s.cents = detuneCents (p[(std::size_t) voiceParam (k, vDetune)]);
            s.depth = depthCents (p[(std::size_t) voiceParam (k, vDepth)]);
            s.lfoInc = 0.5 * rateValue (p[(std::size_t) voiceParam (k, vRate)]) / sr;
            s.octave = on (p[(std::size_t) voiceParam (k, vOctave)]);
            s.source = sourceOf (p[(std::size_t) voiceParam (k, vSource)]);
        }

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            float wetL = 0.0f, wetR = 0.0f;
            for (int k = 0; k < voices; ++k)
            {
                auto& v = voice[k];
                const auto& s = vs[k];
                const auto own = k % 2 == 0 ? inL : inR, other = k % 2 == 0 ? inR : inL; // odd voices: the left side
                const auto src = s.source == 0 ? own : (s.source == 1 ? 0.5f * (own + other) : 0.5f * (own - other));
                // feedback from the voice's last output through the low cut
                v.line.push (src + s.fb * v.fbHp.process (v.last, 0));
                if (! s.active)
                {
                    v.last = 0.0f;
                    continue;
                }
                // triangle LFO on the pitch
                v.lfo += s.lfoInc;
                v.lfo -= std::floor (v.lfo);
                const auto tri = v.lfo < 0.5 ? 4.0 * v.lfo - 1.0 : 3.0 - 4.0 * v.lfo;
                auto ratio = std::pow (2.0, (s.cents + s.depth * tri) / 1200.0);
                if (s.octave)
                    ratio *= 0.5;
                if (! s.octave && aligned)
                {
                    // [EXEC] round 124 (official trial, impulse trains at ±1 … ±100 cents, 44.1 / 48 kHz): ONE read head
                    // at delay + offset, the offset starting at 495 samples (so 0 cents = delay + 495 exactly) and
                    // drifting by (1 − ratio) per sample inside a 496-sample range; at the range's end the head fades
                    // out over 1.3 samples of offset, jumps back by the range and fades in again (pitch down), or a
                    // second head enters at the top while the first finishes at the bottom (pitch up); linear reads
                    const auto drift = 1.0 - ratio;
                    float y = 0.0f;
                    if (drift >= 0.0)
                    {
                        v.offset += drift;
                        if (v.offset >= downMax)
                            v.offset -= downMax - downMin;
                        const auto g = std::min ({ 1.0, (downMax - v.offset) / fade, (v.offset - downMin) / fade });
                        y = (float) std::max (0.0, g) * v.line.linear (1.0 + s.delay + v.offset);
                        v.second = -1.0;
                    }
                    else
                    {
                        v.offset += drift;
                        if (v.second < 0.0 && v.offset <= upMin + upOverlap)
                            v.second = v.offset + (upMax - upMin - upOverlap);
                        if (v.second >= 0.0)
                            v.second += drift;
                        if (v.offset <= upMin)
                        {
                            v.offset = v.second >= 0.0 ? v.second : v.offset + (upMax - upMin);
                            v.second = -1.0;
                        }
                        const auto gIn = std::clamp ((upMax - v.offset) / upFadeIn, 0.0, 1.0);
                        y = (float) gIn * v.line.linear (1.0 + s.delay + v.offset);
                        if (v.second >= 0.0)
                            y += (float) std::clamp ((upMax - v.second) / upFadeIn, 0.0, 1.0) * v.line.linear (1.0 + s.delay + v.second);
                    }
                    v.last = y;
                    wetL += s.gl * y;
                    wetR += s.gr * y;
                    continue;
                }
                // two read taps half a window apart sweep the window at (1 − ratio) samples per sample
                v.phase += (1.0 - ratio) / window;
                v.phase -= std::floor (v.phase);
                float y = 0.0f;
                for (int t = 0; t < 2; ++t)
                {
                    auto ph = v.phase + 0.5 * t;
                    ph -= std::floor (ph);
                    const auto w = (float) std::sin (pi * ph); // power-complementary crossfade of the two grains
                    y += w * v.line.at (1.0 + s.delay + voiceExtra + ph * window);
                }
                v.last = y;
                wetL += s.gl * y;
                wetR += s.gr * y;
            }
            if (lowOnV)
            {
                wetL = lowShelf.process (wetL, 0);
                wetR = lowShelf.process (wetR, 1);
            }
            if (highOnV)
            {
                wetL = highShelf.process (wetL, 0);
                wetR = highShelf.process (wetR, 1);
            }
            // the direct signal: rotated in the stereo field, delayed by half a window when aligned with the voices
            direct[0].push (inL);
            direct[1].push (inR);
            const auto dL = alignSamples > 0.0 ? direct[0].at (1.0 + alignSamples) : inL;
            const auto dR = alignSamples > 0.0 ? direct[1].at (1.0 + alignSamples) : inR;
            const auto oL = dirGain * (rc * dL + rs * dR), oR = dirGain * (rc * dR - rs * dL);
            l[i] = master * (oL + wetL);
            r[i] = master * (oR + wetR);
            peakOut = std::max (peakOut, std::max (std::abs (l[i]), std::abs (r[i])));
        }
        meterIn.store (std::min (peakIn, 1.0f), std::memory_order_relaxed);
        meterOut.store (std::min (peakOut, 1.0f), std::memory_order_relaxed);
    }

    // 2 / 3 = input / output peak (round 129)
    float controlOutput (int k) const noexcept override
    {
        return k == 2 ? meterIn.load (std::memory_order_relaxed) : k == 3 ? meterOut.load (std::memory_order_relaxed) : 0.0f;
    }

private:
    std::atomic<float> meterIn { 0.0f }, meterOut { 0.0f };
    bool lastReset = false;
    void design (const EffectParams& p) noexcept
    {
        float key[5] { p[lowGain], p[lowFreq], p[highGain], p[highFreq], p[feedbackCut] };
        if (designed && std::equal (key, key + 5, lastKey))
            return;
        std::copy (key, key + 5, lastKey);
        designed = true;
        // [EXEC] measured: first-order shelves; a high shelf of gain g at f0 has its corners at f0 and f0 · max (g, 1/g)
        // (-12 dB at 3538 Hz: -2.1 dB at 3.5 kHz, -6.8 at 8 kHz, -11.9 at 16 kHz; +6 dB at 1306 Hz: +2.6 at 1.5 kHz,
        // +4.5 at 3 kHz); the low shelf mirrors it (corners f0 / max (g, 1/g) and f0)
        firstOrderShelf (lowShelf, freqHz (p[lowFreq]), shelfDb (p[lowGain]), false);
        firstOrderShelf (highShelf, freqHz (p[highFreq]), shelfDb (p[highGain]), true);
        // [EXEC] round 124: the feedback low cut is a first-order (bilinear) high-pass — 16 Hz: -3.0 dB / +45° at
        // 16 Hz, first sample 0.99895 of a repeat's impulse
        const auto k = std::tan (pi * std::min (freqHz (p[feedbackCut]), 0.45 * sr) / sr);
        for (auto& v : voice)
        {
            v.fbHp.b0 = 1.0 / (1.0 + k);
            v.fbHp.b1 = -v.fbHp.b0;
            v.fbHp.b2 = 0.0;
            v.fbHp.a1 = (k - 1.0) / (k + 1.0);
            v.fbHp.a2 = 0.0;
        }
    }

    // H(s) = k (1 + s/wz) / (1 + s/wp), bilinear with the corners pre-warped
    void firstOrderShelf (Biquad& b, double f0, double db, bool high) const noexcept
    {
        const auto g = dbToGain (db);
        const auto r = std::max (g, 1.0 / g);
        const auto lo = high ? f0 : f0 / r, up = high ? f0 * r : f0;
        // high boost: zero lo, pole up; high cut: pole lo, zero up; low boost: pole lo, zero up, × r; low cut: zero lo, pole up, ÷ r
        double fz, fp, k;
        if (high)
            fz = g >= 1.0 ? lo : up, fp = g >= 1.0 ? up : lo, k = 1.0;
        else
            fz = g >= 1.0 ? up : lo, fp = g >= 1.0 ? lo : up, k = g >= 1.0 ? r : 1.0 / r;
        const auto warp = [this] (double f) { return std::tan (pi * std::min (f, 0.49 * sr) / sr); };
        const auto wz = warp (fz), wp = warp (fp);
        // s → (1 − z⁻¹)/(1 + z⁻¹) with s/w: (1 + s/wz) → ((wz + 1) + (wz − 1) z⁻¹) / (wz (1 + z⁻¹))
        const auto n0 = k * (wz + 1.0) / wz, n1 = k * (wz - 1.0) / wz;
        const auto d0 = (wp + 1.0) / wp, d1 = (wp - 1.0) / wp;
        b.b0 = n0 / d0;
        b.b1 = n1 / d0;
        b.b2 = 0.0;
        b.a1 = d1 / d0;
        b.a2 = 0.0;
    }

    // the measured read-head range (samples of offset over the voice's delay; REA_rodada124 §4)
    static constexpr double downMin = 79.4, downMax = 575.7, fade = 1.3;
    static constexpr double upMin = 62.0, upMax = 558.4, upOverlap = 4.2, upFadeIn = 0.7;

    struct Voice
    {
        Line line;
        double offset = 495.0, second = -1.0;
        double phase = 0.0, lfo = 0.0;
        float last = 0.0f;
        Biquad fbHp;
    };

    double sr = 48000.0;
    std::array<Voice, voices> voice;
    std::array<Line, 2> direct;
    Biquad lowShelf, highShelf;
    bool designed = false;
    float lastKey[5] {};
};
} // namespace
} // namespace gemini

// =====================================================================================================================
// registry
// =====================================================================================================================
const EffectInfo& quellInfo()
{
    static const EffectInfo info { "Quell Denoiser", { { "Amount", 0.3f }, { "Listen", 0.0f }, { "Output", 0.5f }, { "Matched", 0.0f } } };
    return info;
}

const EffectInfo& geminiInfo()
{
    static const EffectInfo info { "Gemini Doubler",
                                   { { "Output", 1.0f }, { "Direct", 1.0f }, { "Direct gain", 1.0f }, { "Direct pan", 0.5f },
                                     // round 165 [EXEC]: the 17.1 trials' fresh instance — Align off, both shelves on (low 0 dB at
                                     // 101 Hz, high +4.1 dB at 1306 Hz); only new instances change (projects keep their values)
                                     { "Align", 0.0f }, { "Low shelf", 1.0f }, { "Low gain", 30.0f / 42.0f },
                                     { "Low freq", gemini::freqValue (101.0) }, { "High shelf", 1.0f }, { "High gain", 34.1f / 42.0f },
                                     { "High freq", gemini::freqValue (1306.0) }, { "Feedback cut", 0.0f }, { "Range", 0.0f },
                                     // voice 1 (left) and voice 2 (right): a classic double, 9.4 / 23.7 ms, ±6 cents, −6 dB
                                     { "V1 on", 1.0f }, { "V1 gain", 0.875f }, { "V1 pan", 0.0f }, { "V1 delay", 0.094f },
                                     { "V1 feedback", 0.0f }, { "V1 detune", 0.53f }, { "V1 depth", 0.5f }, { "V1 rate", 0.2776f },
                                     { "V1 octave", 0.0f }, { "V1 source", 0.1667f },
                                     { "V2 on", 1.0f }, { "V2 gain", 0.875f }, { "V2 pan", 1.0f }, { "V2 delay", 0.237f },
                                     { "V2 feedback", 0.0f }, { "V2 detune", 0.47f }, { "V2 depth", 0.5f }, { "V2 rate", 0.2776f },
                                     { "V2 octave", 0.0f }, { "V2 source", 0.1667f },
                                     // round 135w: voices 3 and 4 (the four-voice doubler's), off; ±45° like its defaults
                                     { "V3 on", 0.0f }, { "V3 gain", 0.875f }, { "V3 pan", 0.0f }, { "V3 delay", 0.14f },
                                     { "V3 feedback", 0.0f }, { "V3 detune", 0.56f }, { "V3 depth", 0.5f }, { "V3 rate", 0.2776f },
                                     { "V3 octave", 0.0f }, { "V3 source", 0.1667f },
                                     { "V4 on", 0.0f }, { "V4 gain", 0.875f }, { "V4 pan", 1.0f }, { "V4 delay", 0.30f },
                                     { "V4 feedback", 0.0f }, { "V4 detune", 0.44f }, { "V4 depth", 0.5f }, { "V4 rate", 0.2776f },
                                     { "V4 octave", 0.0f }, { "V4 source", 0.1667f },
                                     { "Mod reset", 0.0f } } };
    return info;
}

bool isVoiceKind (EffectKind kind) noexcept { return kind == EffectKind::quellDenoiser || kind == EffectKind::geminiDoubler; }

std::unique_ptr<Effect> make (EffectKind kind)
{
    switch (kind)
    {
        case EffectKind::quellDenoiser: return quell::make (quell::defaultTuning());
        case EffectKind::geminiDoubler: return std::make_unique<gemini::Gemini>();
        default: return nullptr;
    }
}

std::string paramText (EffectKind kind, int param, float v)
{
    v = std::clamp (v, 0.0f, 1.0f);
    if (kind == EffectKind::quellDenoiser)
    {
        switch (param)
        {
            case quell::amount: return fmt ("%.1f%%", 100.0 * v);
            case quell::listen: return on (v) ? "removed noise" : "off";
            case quell::output: return fmt ("%+.1f dB", quell::outputDb (v));
            default: return fmt ("%.2f", v);
        }
    }
    if (kind == EffectKind::geminiDoubler)
    {
        using namespace gemini;
        if (param == modReset)
            return "reset";
        if (param >= voice1 && param < modReset)
        {
            switch ((VoiceParam) ((param - voice1) % voiceParams))
            {
                case vOn: case vOctave: return on (v) ? "on" : "off";
                case vGain: return fmt ("%.1f dB", gainDb (v));
                case vPan: return fmt ("%+.1f°", panDegrees (v));
                case vDelay: return fmt ("%.1f ms", delayMs (v));
                case vFeedback: return fmt ("%.0f%%", 100.0 * feedback (v));
                case vDetune: return fmt ("%+.0f c", detuneCents (v));
                case vDepth: return fmt ("%+.0f c", depthCents (v));
                case vRate: return fmt ("%.1f", rateValue (v));
                case vSource:
                {
                    static const char* const names[3] { "own side", "L + R", "L − R" };
                    return names[sourceOf (v)];
                }
                default: break;
            }
            return fmt ("%.2f", v);
        }
        switch (param)
        {
            case output: return fmt ("%.1f dB", outputDb (v));
            case directGain: return fmt ("%.1f dB", gainDb (v));
            case directPan: return fmt ("%+.1f°", panDegrees (v));
            case lowGain: case highGain: return fmt ("%+.1f dB", shelfDb (v));
            case lowFreq: case highFreq: case feedbackCut: return hzText (freqHz (v));
            case range: return on (v) ? "20 Hz" : "80 Hz";
            default: return on (v) ? "on" : "off";
        }
    }
    return fmt ("%.2f", v);
}
} // namespace lynx::voicetools
